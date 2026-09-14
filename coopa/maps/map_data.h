/**
 * @file map_data.h
 * @brief The dual Delaunay/Voronoi graph a generated map is made of: cells,
 *        corners, edges, settlements, and the container that owns them.
 */

#ifndef COOPA_MAPS_MAP_DATA_H
#define COOPA_MAPS_MAP_DATA_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/maps/biome.h>
#include <coopa/maps/landmark.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/noise.h>

namespace coopa {
namespace maps {

/** @brief Index of a `MapCenter` within `MapGraph::centers`. */
using CenterId = std::int32_t;
/** @brief Index of a `MapCorner` within `MapGraph::corners`. */
using CornerId = std::int32_t;
/** @brief Index of a `MapEdge` within `MapGraph::edges`. */
using EdgeId = std::int32_t;
/** @brief Index of a `MapRegion` within `MapGraph::regions`. */
using RegionId = std::int32_t;
/** @brief Index of a `MapCountry` within `MapGraph::countries`. */
using CountryId = std::int32_t;

/** @brief The value an unset `CenterId`, `CornerId` or `EdgeId` holds. */
inline constexpr std::int32_t k_invalid_id = -1;

/**
 * @struct MapPoint
 * @brief A position in grid space, where the map spans `[0, grid_size]` on both axes.
 */
struct MapPoint {
    double x = 0.0; /**< @brief Horizontal position in grid units. */
    double y = 0.0; /**< @brief Vertical position in grid units. */

    /**
     * @brief Euclidean distance to another point.
     * @param other The point to measure to.
     * @return The distance in grid units.
     */
    double distance_to(const MapPoint& other) const {
        return std::sqrt((x - other.x) * (x - other.x) + (y - other.y) * (y - other.y));
    }
};

/**
 * @brief Sorts points counter-clockwise about their centroid, in place.
 *
 * Recovers a winding order from an unordered vertex set. Correct only for a
 * convex polygon -- a subdivided cell boundary is not one, which is why
 * `MapGraph::cell_outline()` builds its result in order and falls back to this
 * only when a cell's edges do not form a closed ring.
 *
 * @param points The points to order.
 */
inline void sort_points_radially(std::vector<MapPoint>& points) {
    if (points.size() < 3) {
        return;
    }
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (const MapPoint& point : points) {
        sum_x += point.x;
        sum_y += point.y;
    }
    const double count = static_cast<double>(points.size());
    const MapPoint centroid{sum_x / count, sum_y / count};

    std::sort(points.begin(), points.end(), [centroid](const MapPoint& a, const MapPoint& b) {
        return std::atan2(a.y - centroid.y, a.x - centroid.x)
             < std::atan2(b.y - centroid.y, b.x - centroid.x);
    });
}

/**
 * @brief Ray-casting point-in-polygon test; an odd crossing count means inside.
 *
 * Works for any simple polygon, convex or not, which matters because a cell
 * outline is convex only before its edges are subdivided.
 *
 * @param polygon The polygon's vertices, in winding order.
 * @param point The point to test.
 * @return True if the point lies inside the polygon.
 */
inline bool point_in_polygon(const std::vector<MapPoint>& polygon, const MapPoint& point) {
    bool inside = false;
    const std::size_t count = polygon.size();
    if (count < 3) {
        return false;
    }
    for (std::size_t i = 0, j = count - 1; i < count; j = i++) {
        const MapPoint& a = polygon[i];
        const MapPoint& b = polygon[j];
        if ((a.y > point.y) != (b.y > point.y)) {
            const double denominator = b.y - a.y;
            if (denominator == 0.0) {
                continue;
            }
            const double x_at_y = (b.x - a.x) * (point.y - a.y) / denominator + a.x;
            if (point.x < x_at_y) {
                inside = !inside;
            }
        }
    }
    return inside;
}

/**
 * @brief Chaikin corner-cutting, with the two endpoints pinned.
 *
 * Each interior segment is replaced by its quarter and three-quarter points,
 * which rounds every corner without the curve drifting off the points it was
 * derived from. The ends are kept exactly where they are so a smoothed path
 * still meets the junction, the settlement or the coastline it was traced to --
 * a road that stops a quarter of a cell short of its own junction is worse than
 * one drawn straight.
 *
 * Shared by the road and river passes. Both trace a chain of graph positions and
 * both need the joints rounded off without the course being straightened: the
 * meander of a river and the switchback of a mountain road are the *data*, and
 * corner-cutting preserves them precisely because it never moves a point far.
 *
 * @param points The path to smooth, in place.
 * @param iterations Passes to apply; zero or fewer leaves the path alone.
 */
inline void chaikin_smooth(std::vector<MapPoint>& points, int iterations) {
    for (int pass = 0; pass < iterations && points.size() > 2; ++pass) {
        std::vector<MapPoint> cut;
        cut.reserve(points.size() * 2);
        cut.push_back(points.front());
        for (std::size_t i = 0; i + 1 < points.size(); ++i) {
            const MapPoint& a = points[i];
            const MapPoint& b = points[i + 1];
            cut.push_back({a.x * 0.75 + b.x * 0.25, a.y * 0.75 + b.y * 0.25});
            cut.push_back({a.x * 0.25 + b.x * 0.75, a.y * 0.25 + b.y * 0.75});
        }
        cut.push_back(points.back());
        points = std::move(cut);
    }
}

/**
 * @struct MapCenter
 * @brief One Voronoi cell -- a polygon of terrain, and a vertex of the Delaunay triangulation.
 *
 * This is the unit gameplay cares about: it carries the biome, the elevation
 * and the moisture. Its three adjacency lists hold indices into `MapGraph`
 * rather than pointers; the original stored `shared_ptr`s in both directions,
 * so `center -> corners -> touches -> center` formed a reference cycle and the
 * entire graph leaked on every generation.
 */
struct MapCenter {
    CenterId index = k_invalid_id; /**< @brief This cell's own index; equals its slot in `MapGraph::centers`. */
    MapPoint point;                /**< @brief The generating site, at the polygon's approximate centre. */

    bool water = false;  /**< @brief Lake or ocean. */
    bool ocean = false;  /**< @brief Water connected to the map border; false for an inland lake. */
    bool coast = false;  /**< @brief Land bordering at least one ocean cell. */
    bool border = false; /**< @brief Lies in the forced-water band at the edge of the map. */

    Biome biome = Biome::Ocean; /**< @brief Terrain classification, assigned by the biome pass. */
    double elevation = 0.0;     /**< @brief Mean of the cell's corner elevations, in `[0, 1]`. */
    /**
     * @brief Height of the water surface over this cell; meaningless on dry land.
     *
     * Flat per *body* of water, which `elevation` is not. The sea sits at
     * `MapConfig::sea_level` everywhere, and every cell of one lake shares that
     * lake's single surface -- so a consumer can flood a terrain mesh to this
     * number directly, and the water layer can draw a sheet rather than a
     * mottled field.
     *
     * `elevation` remains the height of the *ground* underneath, which for a
     * water cell is its bed.
     */
    double water_level = 0.0;
    double moisture = 0.0;      /**< @brief Mean of the cell's corner moistures, in `[0, 1]`. */
    /** @brief Mean of the cell's corner temperatures, in `[0, 1]`; 0 polar, 1 equatorial. */
    double temperature = 0.0;

    /** @brief The region this cell belongs to, or `k_invalid_id` for water and unclaimed land. */
    RegionId region = k_invalid_id;
    /** @brief The country this cell belongs to, or `k_invalid_id`. */
    CountryId country = k_invalid_id;

    std::vector<CenterId> neighbors; /**< @brief Cells sharing an edge with this one. */
    std::vector<EdgeId> borders;     /**< @brief Edges bounding this cell. */
    std::vector<CornerId> corners;   /**< @brief Polygon vertices, sorted counter-clockwise. */
};

/**
 * @struct MapCorner
 * @brief One Voronoi vertex -- the circumcentre of a Delaunay triangle.
 *
 * Elevation, moisture and rivers are all computed here first and averaged down
 * to cells afterwards, because water flows between corners, not between cells.
 */
struct MapCorner {
    CornerId index = k_invalid_id; /**< @brief This corner's own index; equals its slot in `MapGraph::corners`. */
    MapPoint point;                /**< @brief The circumcentre position. */

    bool ocean = false;  /**< @brief Every cell touching this corner is ocean. */
    bool water = false;  /**< @brief Lake or ocean. */
    bool coast = false;  /**< @brief Touches both land and ocean cells. */
    bool border = false; /**< @brief Lies at or beyond the map edge. */

    double elevation = 0.0;    /**< @brief Height in `[0, 1]`, zero at the coast. */
    double moisture = 0.0;     /**< @brief Wetness in `[0, 1]`. */
    double temperature = 0.0;  /**< @brief Warmth in `[0, 1]`; 0 polar, 1 equatorial. */
    int river = 0;             /**< @brief Volume of river water passing through, or 0 for none. */

    std::vector<CenterId> touches;  /**< @brief Cells this corner is a vertex of. */
    std::vector<EdgeId> protrudes;  /**< @brief Edges meeting at this corner. */
    std::vector<CornerId> adjacent; /**< @brief Corners one edge away. */

    /** @brief The adjacent corner water flows to; self when this is a local minimum. */
    CornerId downslope = k_invalid_id;
};

/**
 * @struct MapEdge
 * @brief A Delaunay edge and its dual Voronoi edge, stored as one object.
 *
 * `d0`/`d1` are the two cells the edge separates; `v0`/`v1` are the two corners
 * it runs between. Rivers follow the Voronoi edge, roads follow the Delaunay
 * edge, which is why both live here.
 */
struct MapEdge {
    EdgeId index = k_invalid_id; /**< @brief This edge's own index; equals its slot in `MapGraph::edges`. */

    CenterId d0 = k_invalid_id; /**< @brief Cell on one side. */
    CenterId d1 = k_invalid_id; /**< @brief Cell on the other side. */
    CornerId v0 = k_invalid_id; /**< @brief Corner at one end. */
    CornerId v1 = k_invalid_id; /**< @brief Corner at the other end. */

    MapPoint midpoint; /**< @brief Halfway between `v0` and `v1`. */
    int river = 0;     /**< @brief Volume of water flowing along this edge, or 0. */
    bool noisy = false;/**< @brief The noisy-edge pass has already processed this edge. */
    /**
     * @brief A road runs along this edge.
     *
     * Kept as a plain predicate beside `road_class` because most consumers only
     * ask whether one is there -- the town packer turns a road-flagged border
     * into a street without caring how busy it is. Always equal to
     * `road_class != RoadClass::None`; the road pass is what keeps them agreeing.
     */
    bool road = false;
    /** @brief Traffic tier of the road here; `None` when no road runs along this edge. */
    RoadClass road_class = RoadClass::None;
    /**
     * @brief Routes the network pass sent along this edge; what `road_class` is derived from.
     *
     * Kept rather than discarded after classification so a consumer can re-cut
     * the tiers at its own thresholds, and so a test can check that a highway
     * really did earn the title.
     */
    int traffic = 0;
    /**
     * @brief The road here spans water -- a river, a lake neck or a strait.
     *
     * Exactly representable because a road follows the Delaunay edge `d0`-`d1`
     * and a river the dual Voronoi edge `v0`-`v1`, and those are this same
     * object: the two cross each other by construction, so a road on a
     * river-carrying edge crosses that river and nothing else needs deciding.
     */
    bool bridge = false;

    /** @brief The wobbled path from `v0` to `midpoint`; exactly two points when not subdivided. */
    std::vector<MapPoint> noisy_points0;
    /** @brief The wobbled path from `v1` to `midpoint`; exactly two points when not subdivided. */
    std::vector<MapPoint> noisy_points1;
};

/**
 * @struct MapRoad
 * @brief One continuous run of road, as a smoothed centreline in grid units.
 *
 * The per-edge flags say *where* roads are; this says what one *looks like*.
 * A run is a maximal chain of same-class road edges between two junctions, so a
 * consumer can follow a highway from end to end -- to drive a caravan along it,
 * or to stroke it as a single polyline -- without rediscovering the chain from
 * the edge flags every time.
 *
 * `points` is denser than `edges` is long: the chain of cell sites is corner-cut
 * before it is stored, because a route drawn straight between sites is visibly
 * faceted at every cell and no road is.
 */
struct MapRoad {
    RoadClass road_class = RoadClass::Trail; /**< @brief The class every edge in this run shares. */
    std::vector<MapPoint> points; /**< @brief The smoothed centreline, in grid units, in order. */
    std::vector<EdgeId> edges;    /**< @brief The edges this run covers, in order along it. */
};

/**
 * @struct MapRiver
 * @brief One watercourse, as a smoothed centreline in grid units.
 *
 * The counterpart of `MapRoad`: the per-corner and per-edge `river` volumes say
 * *where* the water is, this says what the channel looks like. Drawn straight
 * between Voronoi corners a river is visibly angular at every corner, which is
 * the one shape moving water never has.
 *
 * Ordered source to mouth, so `volume` -- which is the volume where it ends --
 * is also the largest the channel ever gets.
 */
struct MapRiver {
    std::vector<MapPoint> points;  /**< @brief The smoothed centreline, in grid units, source first. */
    std::vector<CornerId> corners; /**< @brief The corners it runs through, in order. */
    int volume = 0;                /**< @brief Volume at the mouth; the channel widens toward it. */
};

/**
 * @struct MapBuilding
 * @brief One axis-aligned building footprint packed inside a settlement's cell.
 */
struct MapBuilding {
    MapPoint point;        /**< @brief Footprint centre, in grid units. */
    double width = 0.0;    /**< @brief Footprint width, in grid units. */
    double height = 0.0;   /**< @brief Footprint height, in grid units. */
    double rotation = 0.0; /**< @brief Yaw in radians, for a consumer that renders oriented meshes. */
};

/**
 * @struct MapTown
 * @brief A settlement occupying one cell, with the buildings packed into it.
 */
struct MapTown {
    /**
     * @brief The cell the settlement grew from; the first entry of `cells`.
     *
     * Still the settlement's identity -- its position, its region and its
     * spacing against other settlements are all measured from here.
     */
    CenterId center = k_invalid_id;
    /**
     * @brief Every cell this settlement covers, the primary one first.
     *
     * A settlement larger than a hamlet does not fit in one Voronoi cell, so a
     * capital claims its neighbours and builds across all of them. Disjoint
     * between settlements: a cell belongs to at most one.
     */
    std::vector<CenterId> cells;
    MapPoint point;                  /**< @brief Settlement centre, in grid units. */
    TownTier tier = TownTier::Village; /**< @brief Size class, by site quality rank. */
    double score = 0.0;              /**< @brief Habitability score the site was chosen on. */
    std::string name;                /**< @brief Generated in the dialect of its region. */
    RegionId region = k_invalid_id;  /**< @brief The region this settlement belongs to. */

    /** @brief Number of occupied dwellings; equals `buildings.size()`. */
    int households = 0;
    /**
     * @brief Souls living here.
     *
     * Derived from the building count rather than invented: each dwelling holds
     * a drawn number of occupants, scaled by tier density and by how well the
     * land feeds them. A settlement's population and its footprint therefore
     * cannot disagree.
     */
    int population = 0;
    /** @brief Relative wealth in `[0, 1]`, from site quality and access to trade. */
    double prosperity = 0.0;

    std::vector<MapBuilding> buildings; /**< @brief Footprints that fit inside the cell polygon. */
};

/**
 * @brief The four corners of a building's footprint, with its yaw applied.
 *
 * Returned counter-clockwise from the footprint's local bottom-left. This is
 * the authoritative shape of a building: `MapBuilding::point` is only its
 * centre, and the generator guarantees containment and non-overlap against
 * *these* corners, not against an axis-aligned box.
 *
 * @param building The building to expand.
 * @return Its four corners in world grid units.
 */
inline std::array<MapPoint, 4> building_corners(const MapBuilding& building) {
    const double half_w = building.width * 0.5;
    const double half_h = building.height * 0.5;
    const double c = std::cos(building.rotation);
    const double s = std::sin(building.rotation);

    const double local[4][2] = {{-half_w, -half_h}, {half_w, -half_h},
                                {half_w, half_h},   {-half_w, half_h}};
    std::array<MapPoint, 4> corners{};
    for (std::size_t i = 0; i < 4; ++i) {
        corners[i] = {building.point.x + local[i][0] * c - local[i][1] * s,
                      building.point.y + local[i][0] * s + local[i][1] * c};
    }
    return corners;
}

/**
 * @brief Separating-axis test for two oriented footprints.
 *
 * Two convex shapes miss each other exactly when some axis separates their
 * projections, and for rectangles only the four edge normals can be that axis.
 * A bounding-circle test would be cheaper but rejects far more than it needs
 * to, which would stop buildings from lining a street closely enough to read
 * as one.
 *
 * @param a First footprint.
 * @param b Second footprint.
 * @return True if the two footprints intersect.
 */
inline bool buildings_overlap(const MapBuilding& a, const MapBuilding& b) {
    const std::array<MapPoint, 4> box_a = building_corners(a);
    const std::array<MapPoint, 4> box_b = building_corners(b);

    for (int shape = 0; shape < 2; ++shape) {
        const std::array<MapPoint, 4>& source = shape == 0 ? box_a : box_b;
        for (std::size_t i = 0; i < 2; ++i) {
            // Edge normal; only two per rectangle are distinct.
            const double axis_x = -(source[i + 1].y - source[i].y);
            const double axis_y = source[i + 1].x - source[i].x;
            const double length = std::hypot(axis_x, axis_y);
            if (length == 0.0) {
                continue;
            }
            const double nx = axis_x / length;
            const double ny = axis_y / length;

            double min_a = 0.0, max_a = 0.0, min_b = 0.0, max_b = 0.0;
            for (std::size_t k = 0; k < 4; ++k) {
                const double pa = box_a[k].x * nx + box_a[k].y * ny;
                const double pb = box_b[k].x * nx + box_b[k].y * ny;
                if (k == 0) {
                    min_a = max_a = pa;
                    min_b = max_b = pb;
                } else {
                    min_a = std::min(min_a, pa); max_a = std::max(max_a, pa);
                    min_b = std::min(min_b, pb); max_b = std::max(max_b, pb);
                }
            }
            if (max_a <= min_b || max_b <= min_a) {
                return false; // This axis separates them.
            }
        }
    }
    return true;
}

/**
 * @struct MapLandmark
 * @brief A notable place worth putting on a map and worth travelling to.
 */
struct MapLandmark {
    CenterId center = k_invalid_id;  /**< @brief The cell it stands on. */
    MapPoint point;                  /**< @brief Its position, in grid units. */
    LandmarkKind kind = LandmarkKind::Ruins; /**< @brief What sort of place it is. */
    std::string name;                /**< @brief Named in the dialect of its region. */
    RegionId region = k_invalid_id;  /**< @brief The region it lies in, or invalid. */
};

/**
 * @brief The language seed for a country, from the map seed and its id.
 *
 * Shared so the region pass and the town pass derive the same language without
 * passing one between them.
 *
 * @param map_seed `MapConfig::seed`.
 * @param country The country's id.
 * @return A stable seed for `language_for()`.
 */
inline std::uint32_t country_language_seed(int map_seed, CountryId country) {
    return static_cast<std::uint32_t>(map_seed) * 2654435761u
         + static_cast<std::uint32_t>(country + 1) * 40503u;
}

/**
 * @brief The dialect seed for a region, from the map seed and its id.
 * @param map_seed `MapConfig::seed`.
 * @param region The region's id.
 * @return A stable seed for `dialect_for()`.
 */
inline std::uint32_t region_dialect_seed(int map_seed, RegionId region) {
    return static_cast<std::uint32_t>(map_seed) * 2246822519u
         + static_cast<std::uint32_t>(region + 1) * 3266489917u;
}

/**
 * @struct MapRegion
 * @brief A province: a contiguous block of cells inside one country.
 *
 * Regions are the unit a settlement belongs to and takes its name from. Each
 * carries a dialect of its country's language, so its towns sound related to
 * their neighbours without being identical.
 */
struct MapRegion {
    RegionId index = k_invalid_id;   /**< @brief This region's own index in `MapGraph::regions`. */
    CountryId country = k_invalid_id;/**< @brief The country this region belongs to. */
    std::string name;                /**< @brief Generated in the region's own dialect. */

    CenterId seed = k_invalid_id;    /**< @brief The cell the region grew outward from. */
    CenterId capital = k_invalid_id; /**< @brief Its foremost settlement's cell, or invalid. */
    std::vector<CenterId> cells;     /**< @brief Every cell claimed by this region. */

    Biome dominant_biome = Biome::Grassland; /**< @brief The biome covering the most of it. */
    int population = 0;              /**< @brief Sum of its settlements' populations. */
    double area = 0.0;               /**< @brief Total cell area, in square grid units. */
    glm::vec3 color = glm::vec3(255.0f); /**< @brief Tint used when regions are drawn. */
};

/**
 * @struct MapCountry
 * @brief A nation: one or more regions under a single language.
 */
struct MapCountry {
    CountryId index = k_invalid_id;  /**< @brief This country's own index in `MapGraph::countries`. */
    std::string name;                /**< @brief Generated in the country's language. */

    std::vector<RegionId> regions;   /**< @brief The regions it is divided into. */
    CenterId capital = k_invalid_id; /**< @brief Its largest settlement's cell, or invalid. */
    int population = 0;              /**< @brief Sum of its regions' populations. */
    double area = 0.0;               /**< @brief Total claimed area, in square grid units. */
    glm::vec3 color = glm::vec3(255.0f); /**< @brief Tint used when countries are drawn. */
};

/**
 * @struct TerrainDetail
 * @brief A fractal field sampled on top of the control mesh, and how hard to apply it.
 *
 * Built from a `MapConfig` by `make_terrain_detail()` and handed to
 * `MapGraph::elevation_at()`. Holds a borrowed pointer rather than a `Noise` by
 * value so one sampler serves a whole render: `Noise` wraps a FastNoiseLite,
 * which is not free to construct.
 */
struct TerrainDetail {
    const Noise* noise = nullptr; /**< @brief The detail field; null disables displacement. */
    double roughness = 0.0;       /**< @brief Displacement amplitude; 0 disables it. */
};

/**
 * @brief Builds the detail sampler `MapGraph::elevation_at()` displaces with.
 *
 * The `Noise` must outlive every sample taken through the returned struct, which
 * only borrows it -- build both once per render, not per pixel.
 *
 * @param config Supplies the roughness amplitude.
 * @param noise A sampler built from `MapConfig::noise_terrain`.
 * @return The detail field, or a disabled one when roughness is zero.
 */
inline TerrainDetail make_terrain_detail(const MapConfig& config, const Noise& noise) {
    TerrainDetail detail;
    if (config.terrain_roughness > 0.0) {
        detail.noise = &noise;
        detail.roughness = config.terrain_roughness;
    }
    return detail;
}

/**
 * @class MapGraph
 * @brief Owns every cell, corner, edge, road run and settlement of one generated map.
 *
 * The three arrays are index-addressed and self-consistent: `centers[i].index
 * == i` holds for all three after generation, and every id stored in an
 * adjacency list is a valid slot in the corresponding array. Passes that
 * reorder anything must sort an index array instead of the storage itself.
 */
class MapGraph {
public:
    std::vector<MapCenter> centers; /**< @brief Voronoi cells, one per generating site. */
    std::vector<MapCorner> corners; /**< @brief Voronoi vertices, one per Delaunay triangle. */
    std::vector<MapEdge> edges;     /**< @brief Shared Delaunay/Voronoi edges. */
    std::vector<MapRoad> roads;     /**< @brief Road runs traced by the road pass. */
    std::vector<MapRiver> rivers;   /**< @brief Watercourses traced by the river pass. */
    std::vector<MapTown> towns;     /**< @brief Settlements placed by the town pass. */
    std::vector<MapRegion> regions; /**< @brief Provinces carved by the region pass. */
    std::vector<MapCountry> countries; /**< @brief Nations carved by the region pass. */
    std::vector<MapLandmark> landmarks; /**< @brief Notable places found by the landmark pass. */

    /** @brief Drops every array, returning the graph to its freshly constructed state. */
    void clear() {
        centers.clear();
        corners.clear();
        edges.clear();
        roads.clear();
        rivers.clear();
        towns.clear();
        regions.clear();
        countries.clear();
        landmarks.clear();
    }

    /**
     * @brief Interpolates the ground height at a point.
     *
     * Barycentric over the **Delaunay triangle** containing the point, blending
     * the three cell-site heights at its vertices. That is the natural
     * piecewise-linear surface through samples taken at the sites, and it is what
     * a terrain mesh built from this data would be.
     *
     * `center` is a hint, not a constraint: the triangles searched are the ones
     * incident to that cell, found through `MapCorner::touches` -- each corner of
     * a cell is the circumcentre of a Delaunay triangle that cell is a vertex of.
     * The nearest site to any point is always a vertex of the Delaunay triangle
     * containing it, so for a point in the cell the answer is in that set.
     *
     * ### Two interpolations this replaced, and why both were wrong
     *
     * **Inverse-distance weighting over the cell's corners** read as a plateau.
     * Every corner is roughly equidistant from the middle of a cell, so the
     * interior came out near the mean of the corners and only approached a
     * corner's own value in the last few pixels before it.
     *
     * **Barycentric over the cell's own corner fan** -- site to two consecutive
     * corners -- was worse. It put a crease at each of the six-odd internal fan
     * edges *and* a tent pole at every site, whose height is the mean of its
     * corners, so the surface came out visibly crumpled. And the fan covers only
     * the *straight* corner polygon while the renderer draws the *subdivided*
     * outline, which bulges outside it: 1.78% of drawn pixels missed every fan
     * triangle and fell through to the inverse-distance formula, speckling every
     * cell boundary with slivers of a different surface.
     *
     * The Delaunay triangulation has neither problem. It tiles the hull, so there
     * is no outside to fall through, and it creases once per edge rather than six
     * times per cell.
     *
     * @param center The cell to search from; the point need not be strictly inside it.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @return The interpolated height, or 0 if the cell has no corners.
     */
    double elevation_at(const MapCenter& center, double x, double y) const {
        if (center.corners.empty()) {
            return 0.0;
        }
        const MapPoint query{x, y};

        double height = 0.0;
        if (triangle_elevation_(center, query, height)) {
            return height;
        }
        // The cell's own triangles cover its Voronoi region, but the renderer
        // draws the *subdivided* outline, which bulges past it into a
        // neighbour's -- about 2% of drawn pixels. Those belong to a
        // neighbour's triangles, so look there before giving up.
        for (const CenterId neighbor_id : center.neighbors) {
            if (triangle_elevation_(centers[static_cast<std::size_t>(neighbor_id)], query,
                                    height)) {
                return height;
            }
        }
        return inverse_distance_elevation_(center, query);
    }

    /**
     * @brief Interpolates the ground height, with fractal detail on top.
     *
     * The control mesh gives the landform; the detail field gives it texture.
     * Keeping the two apart is what stops the noise reaching the data everything
     * else is derived from -- biomes, rivers and roads all classify on the cell
     * and corner heights, which stay exactly as the passes left them, while
     * anyone sampling the *surface* sees the displaced version.
     *
     * The displacement is scaled by the base height, so it fades to nothing at
     * the coast: a shoreline stays exactly at sea level and no land is nudged
     * below it.
     *
     * @param center The cell to sample within.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @param detail The detail field and how strongly to apply it.
     * @return The displaced height, clamped to `[0, 1]`.
     */
    double elevation_at(const MapCenter& center, double x, double y,
                        const TerrainDetail& detail) const {
        const double base = elevation_at(center, x, y);
        if (detail.noise == nullptr || detail.roughness <= 0.0) {
            return base;
        }
        // Taper by the base height itself. Anything that does not vanish at zero
        // would either lift the shoreline off sea level or push it under.
        const double amplitude = detail.roughness * base;
        const double displaced = base + amplitude * static_cast<double>(detail.noise->sample(x, y));
        return std::clamp(displaced, 0.0, 1.0);
    }

    /**
     * @brief Interpolates over whichever of a cell's Delaunay triangles claims a point.
     *
     * A cell's corners are exactly the circumcentres of the Delaunay triangles it
     * is a vertex of, so `MapCorner::touches` enumerates that cell's whole star.
     *
     * @param center The cell whose triangles to try.
     * @param query The point to sample.
     * @param out_height Receives the interpolated height when one claims it.
     * @return True if a triangle contained the point.
     */
    bool triangle_elevation_(const MapCenter& center, const MapPoint& query,
                             double& out_height) const {
        for (const CornerId corner_id : center.corners) {
            const MapCorner& corner = corners[static_cast<std::size_t>(corner_id)];
            if (corner.touches.size() != 3) {
                continue; // Not a complete triangle; only possible at the hull.
            }
            const MapCenter& a = centers[static_cast<std::size_t>(corner.touches[0])];
            const MapCenter& b = centers[static_cast<std::size_t>(corner.touches[1])];
            const MapCenter& c = centers[static_cast<std::size_t>(corner.touches[2])];

            double wa = 0.0;
            double wb = 0.0;
            double wc = 0.0;
            if (barycentric_(a.point, b.point, c.point, query, wa, wb, wc)) {
                out_height = wa * a.elevation + wb * b.elevation + wc * c.elevation;
                return true;
            }
        }
        return false;
    }

    /**
     * @brief Barycentric coordinates of a point in a triangle, if it falls inside.
     *
     * A small negative tolerance on each weight, so a sample sitting exactly on a
     * shared edge is claimed by a triangle rather than falling through every one
     * of them to the fallback.
     *
     * @param a First vertex.
     * @param b Second vertex.
     * @param c Third vertex.
     * @param point The point to locate.
     * @param out_a Weight of `a`.
     * @param out_b Weight of `b`.
     * @param out_c Weight of `c`.
     * @return True if the point lies in the triangle.
     */
    static bool barycentric_(const MapPoint& a, const MapPoint& b, const MapPoint& c,
                             const MapPoint& point, double& out_a, double& out_b,
                             double& out_c) {
        const double v0x = b.x - a.x, v0y = b.y - a.y;
        const double v1x = c.x - a.x, v1y = c.y - a.y;
        const double denominator = v0x * v1y - v1x * v0y;
        if (denominator == 0.0) {
            return false; // Degenerate sliver; let the next triangle try.
        }
        const double px = point.x - a.x, py = point.y - a.y;
        out_b = (px * v1y - v1x * py) / denominator;
        out_c = (v0x * py - px * v0y) / denominator;
        out_a = 1.0 - out_b - out_c;

        constexpr double tolerance = -1e-9;
        return out_a >= tolerance && out_b >= tolerance && out_c >= tolerance;
    }

    /**
     * @brief The inverse-distance fallback, for a point no incident triangle claims.
     *
     * Only reachable at the convex hull, where a corner may not have three
     * touching cells and the triangulation runs out. Inside the map it should
     * never be hit -- `test_elevation_interpolation_has_no_gaps` is what holds
     * that to account.
     */
    double inverse_distance_elevation_(const MapCenter& center, const MapPoint& query) const {
        double total_weight = 0.0;
        double weighted_elevation = 0.0;
        for (const CornerId corner_id : center.corners) {
            const MapCorner& corner = corners[static_cast<std::size_t>(corner_id)];
            const double distance = query.distance_to(corner.point);
            if (distance == 0.0) {
                return corner.elevation;
            }
            const double weight = 1.0 / distance;
            weighted_elevation += weight * corner.elevation;
            total_weight += weight;
        }
        if (total_weight == 0.0) {
            return 0.0;
        }
        return weighted_elevation / total_weight;
    }

    /**
     * @brief Interpolates elevation along an edge from its two endpoint heights.
     * @param edge The edge to sample along.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @return The interpolated height, or 0 if either endpoint is unset.
     */
    double elevation_at(const MapEdge& edge, double x, double y) const {
        if (edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
            return 0.0;
        }
        const MapCorner& c0 = corners[static_cast<std::size_t>(edge.v0)];
        const MapCorner& c1 = corners[static_cast<std::size_t>(edge.v1)];

        const MapPoint query{x, y};
        const double distance_to_v0 = query.distance_to(c0.point);
        const double distance_to_v1 = query.distance_to(c1.point);
        const double total_distance = distance_to_v0 + distance_to_v1;

        if (total_distance == 0.0) {
            return c0.elevation;
        }
        return (distance_to_v1 / total_distance) * c0.elevation
             + (distance_to_v0 / total_distance) * c1.elevation;
    }

    /**
     * @brief Collects a cell's polygon outline in grid space, in winding order.
     *
     * Walks the cell's corners -- which the generator has already sorted
     * counter-clockwise -- and, for each consecutive pair, appends the whole
     * path of the edge joining them. Each edge stores its path as two halves
     * meeting at the midpoint, so one half is reversed to run the right way
     * round.
     *
     * Building the outline in order matters once edges are subdivided: a
     * wobbled path is not in convex position, so re-deriving the winding by
     * sorting the points about their centroid puts near-collinear points in the
     * wrong order and the fill tears. Cells whose edges do not form a closed
     * ring -- only possible at the outer hull -- fall back to the unordered
     * point set, which the caller sorts.
     *
     * @param center The cell to outline.
     * @return The polygon's vertices, in grid units and winding order.
     */
    std::vector<MapPoint> cell_outline(const MapCenter& center) const {
        std::vector<MapPoint> outline;
        const std::size_t corner_count = center.corners.size();

        if (corner_count >= 3) {
            bool complete = true;
            for (std::size_t i = 0; i < corner_count && complete; ++i) {
                const CornerId from = center.corners[i];
                const CornerId to = center.corners[(i + 1) % corner_count];

                const EdgeId edge_id = edge_between(center, from, to);
                if (edge_id == k_invalid_id) {
                    complete = false;
                    break;
                }

                const MapEdge& edge = edges[static_cast<std::size_t>(edge_id)];
                const bool forward = (edge.v0 == from);
                const std::vector<MapPoint>& first = forward ? edge.noisy_points0 : edge.noisy_points1;
                const std::vector<MapPoint>& second = forward ? edge.noisy_points1 : edge.noisy_points0;

                outline.insert(outline.end(), first.begin(), first.end());
                outline.insert(outline.end(), second.rbegin(), second.rend());
            }
            if (complete) {
                return outline;
            }
            outline.clear();
        }

        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = edges[static_cast<std::size_t>(edge_id)];
            outline.insert(outline.end(), edge.noisy_points0.begin(), edge.noisy_points0.end());
            outline.insert(outline.end(), edge.noisy_points1.begin(), edge.noisy_points1.end());
        }
        sort_points_radially(outline);
        return outline;
    }

    /**
     * @brief Polygon area of a cell, by the shoelace formula.
     *
     * Computed from `MapCenter::corners`, which the generator has already sorted
     * counter-clockwise, so the straight-edged cell is measured -- not the
     * wobbled one. That is the area a pass actually wants: subdivision moves a
     * boundary in and out about the same midpoint and leaves the enclosed area
     * essentially unchanged, and the straight polygon exists before the
     * noisy-edge pass has run.
     *
     * @param center The cell to measure.
     * @return Its area in square grid units; 0 for a degenerate cell.
     */
    double cell_area(const MapCenter& center) const {
        if (center.corners.size() < 3) {
            return 0.0;
        }
        double twice_area = 0.0;
        const std::size_t count = center.corners.size();
        for (std::size_t i = 0, j = count - 1; i < count; j = i++) {
            const MapPoint& a = corners[static_cast<std::size_t>(center.corners[i])].point;
            const MapPoint& b = corners[static_cast<std::size_t>(center.corners[j])].point;
            twice_area += (b.x + a.x) * (b.y - a.y);
        }
        return std::abs(twice_area) * 0.5;
    }

    /**
     * @brief Finds the edge of a cell joining two of its corners.
     *
     * Scans only the cell's own bounding edges, of which there are as many as
     * it has corners.
     *
     * @param center The cell whose boundary to search.
     * @param a One corner.
     * @param b The other corner.
     * @return The joining edge, or `k_invalid_id` if the corners are not adjacent.
     */
    EdgeId edge_between(const MapCenter& center, CornerId a, CornerId b) const {
        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = edges[static_cast<std::size_t>(edge_id)];
            if ((edge.v0 == a && edge.v1 == b) || (edge.v0 == b && edge.v1 == a)) {
                return edge_id;
            }
        }
        return k_invalid_id;
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_DATA_H
