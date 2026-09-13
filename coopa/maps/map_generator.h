/**
 * @file map_generator.h
 * @brief Builds the Voronoi graph from a jittered point grid and drives the
 *        generation passes over it.
 */

#ifndef COOPA_MAPS_MAP_GENERATOR_H
#define COOPA_MAPS_MAP_GENERATOR_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <delaunator/delaunator.hpp>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/passes/pass_biomes.h>
#include <coopa/maps/passes/pass_coast.h>
#include <coopa/maps/passes/pass_elevation.h>
#include <coopa/maps/passes/pass_landmarks.h>
#include <coopa/maps/passes/pass_moisture.h>
#include <coopa/maps/passes/pass_noisy_edges.h>
#include <coopa/maps/passes/pass_rivers.h>
#include <coopa/maps/passes/pass_temperature.h>
#include <coopa/maps/passes/pass_regions.h>
#include <coopa/maps/passes/pass_roads.h>
#include <coopa/maps/passes/pass_towns.h>
#include <coopa/maps/passes/pass_water.h>

namespace coopa {
namespace maps {

/**
 * @class MapGenerator
 * @brief Turns a `MapConfig` into a finished `MapGraph`.
 *
 * The geometry comes first and never changes afterwards: points are laid on a
 * jittered lattice, triangulated, and the Voronoi dual is read off the
 * triangulation's half-edges. Every pass then annotates that fixed graph in
 * turn, each depending on the one before it.
 *
 * The graph is exposed through `graph()` rather than kept private, because the
 * map data -- not the render of it -- is what a game engine consumes.
 */
class MapGenerator {
public:
    /**
     * @brief Constructs a generator.
     * @param config The map description; copied, so the caller may discard it.
     * @param logger Receives one line per stage.
     */
    MapGenerator(const MapConfig& config, coopa::debug::Logger& logger)
        : config_(config), logger_(logger) {}

    /**
     * @brief Generates the map, discarding any previous result.
     * @throws std::runtime_error If the point set is degenerate and cannot be triangulated.
     */
    void generate() {
        logger_.info("map generation: start (seed " + std::to_string(config_.seed)
                     + ", grid " + std::to_string(config_.grid_size) + ")");

        graph_.clear();
        generate_points_();
        triangulate_points_();
        sort_cell_corners_();
        border_check_();
        execute_passes_();

        logger_.info("map generation: done (" + std::to_string(graph_.centers.size())
                     + " cells, " + std::to_string(graph_.corners.size())
                     + " corners, " + std::to_string(graph_.edges.size()) + " edges)");
    }

    /** @brief The generated map. Empty until `generate()` has been called. */
    const MapGraph& graph() const { return graph_; }

    /** @brief Mutable access to the generated map, for consumers that annotate it further. */
    MapGraph& graph() { return graph_; }

    /** @brief The configuration this generator was built with. */
    const MapConfig& config() const { return config_; }

private:
    MapConfig config_;                /**< @brief The map description being generated. */
    coopa::debug::Logger& logger_;    /**< @brief Progress sink. */
    MapGraph graph_;                  /**< @brief The graph under construction. */
    std::vector<MapPoint> points_;    /**< @brief Generating sites, one per cell. */

    /** @brief How far outside the grid the boundary ring of points is placed. */
    static constexpr double k_boundary_margin = 1.0;

    /**
     * @brief Lays out the generating sites.
     *
     * A jittered lattice rather than Poisson-disc sampling or Lloyd relaxation:
     * it is one pass, it is trivially reproducible from the seed, and at these
     * densities the resulting cells are close enough to uniform that the
     * difference does not show.
     *
     * A ring of points outside the grid is added so the cells along the map's
     * edge are bounded polygons rather than the unbounded Voronoi regions the
     * hull points would otherwise produce.
     */
    void generate_points_() {
        std::mt19937 rng(static_cast<std::mt19937::result_type>(config_.seed));
        std::uniform_real_distribution<double> jitter(-1.0, 1.0);

        points_.clear();
        const int grid_size = config_.grid_size;
        points_.reserve(static_cast<std::size_t>((grid_size + 1) * (grid_size + 1))
                        + static_cast<std::size_t>(4 * (grid_size + 1) + 4));

        for (int x = 0; x <= grid_size; ++x) {
            for (int y = 0; y <= grid_size; ++y) {
                points_.push_back({x + config_.jitter * (jitter(rng) - 0.5),
                                   y + config_.jitter * (jitter(rng) - 0.5)});
            }
        }

        const double margin = k_boundary_margin;
        const double far_edge = grid_size + margin;
        for (int x = 0; x <= grid_size; ++x) {
            points_.push_back({x - margin, -margin});
            points_.push_back({x + margin, far_edge});
        }
        for (int y = 0; y <= grid_size; ++y) {
            points_.push_back({-margin, y - margin});
            points_.push_back({far_edge, y + margin});
        }
        points_.push_back({-margin, -margin});
        points_.push_back({-margin, far_edge});
        points_.push_back({far_edge, -margin});
        points_.push_back({far_edge, far_edge});
    }

    /** @brief Triangulates the point set and reads the Voronoi dual off it. */
    void triangulate_points_() {
        std::vector<double> coords;
        coords.reserve(points_.size() * 2);
        for (const MapPoint& point : points_) {
            coords.push_back(point.x);
            coords.push_back(point.y);
        }

        const delaunator::Delaunator delaunay(coords);
        collect_delaunay_data_(delaunay);
    }

    /**
     * @brief Builds cells, corners and edges from the triangulation.
     *
     * One cell per input point, one corner per triangle circumcentre, and one
     * edge per pair of adjacent triangles. Each interior edge is visited from
     * both of its half-edges; only the lower index creates the edge, so the
     * arrays hold one record per geometric edge rather than the two the
     * original produced.
     */
    void collect_delaunay_data_(const delaunator::Delaunator& delaunay) {
        graph_.centers.resize(points_.size());
        for (std::size_t i = 0; i < points_.size(); ++i) {
            graph_.centers[i].index = static_cast<CenterId>(i);
            graph_.centers[i].point = points_[i];
        }

        const std::size_t triangle_count = delaunay.triangles.size() / 3;
        graph_.corners.resize(triangle_count);
        for (std::size_t t = 0; t < triangle_count; ++t) {
            const std::size_t i0 = delaunay.triangles[t * 3];
            const std::size_t i1 = delaunay.triangles[t * 3 + 1];
            const std::size_t i2 = delaunay.triangles[t * 3 + 2];

            MapCorner& corner = graph_.corners[t];
            corner.index = static_cast<CornerId>(t);
            corner.point = circumcenter_(points_[i0], points_[i1], points_[i2]);

            const CornerId corner_id = corner.index;
            for (const std::size_t center_index : {i0, i1, i2}) {
                graph_.centers[center_index].corners.push_back(corner_id);
                corner.touches.push_back(static_cast<CenterId>(center_index));
            }
        }

        for (std::size_t e = 0; e < delaunay.halfedges.size(); ++e) {
            const std::size_t paired = delaunay.halfedges[e];
            if (paired == delaunator::INVALID_INDEX || paired < e) {
                continue; // Hull edge, or the far side of an edge already built.
            }

            const CenterId d0 = static_cast<CenterId>(delaunay.triangles[e]);
            const CenterId d1 = static_cast<CenterId>(delaunay.triangles[paired]);
            const CornerId v0 = static_cast<CornerId>(e / 3);
            const CornerId v1 = static_cast<CornerId>(paired / 3);

            MapEdge edge;
            edge.index = static_cast<EdgeId>(graph_.edges.size());
            edge.d0 = d0;
            edge.d1 = d1;
            edge.v0 = v0;
            edge.v1 = v1;
            edge.midpoint = {(graph_.corners[static_cast<std::size_t>(v0)].point.x
                              + graph_.corners[static_cast<std::size_t>(v1)].point.x) * 0.5,
                             (graph_.corners[static_cast<std::size_t>(v0)].point.y
                              + graph_.corners[static_cast<std::size_t>(v1)].point.y) * 0.5};

            const EdgeId edge_id = edge.index;
            graph_.edges.push_back(edge);

            graph_.centers[static_cast<std::size_t>(d0)].borders.push_back(edge_id);
            graph_.centers[static_cast<std::size_t>(d1)].borders.push_back(edge_id);
            graph_.centers[static_cast<std::size_t>(d0)].neighbors.push_back(d1);
            graph_.centers[static_cast<std::size_t>(d1)].neighbors.push_back(d0);

            graph_.corners[static_cast<std::size_t>(v0)].protrudes.push_back(edge_id);
            graph_.corners[static_cast<std::size_t>(v1)].protrudes.push_back(edge_id);
            graph_.corners[static_cast<std::size_t>(v0)].adjacent.push_back(v1);
            graph_.corners[static_cast<std::size_t>(v1)].adjacent.push_back(v0);
        }
    }

    /**
     * @brief Sorts each cell's corners counter-clockwise about its centroid.
     *
     * Corners arrive in triangulation order, which is arbitrary; fan-filling or
     * containment-testing an arbitrarily ordered polygon produces nonsense. The
     * original had this call commented out and re-sorted inside each renderer
     * instead, which left `MapCenter::corners` unusable for anything else.
     */
    void sort_cell_corners_() {
        for (MapCenter& center : graph_.centers) {
            if (center.corners.size() < 3) {
                continue;
            }
            const MapPoint centroid = centroid_of_(center.corners);
            std::sort(center.corners.begin(), center.corners.end(),
                      [this, centroid](CornerId a, CornerId b) {
                          const MapPoint& pa = graph_.corners[static_cast<std::size_t>(a)].point;
                          const MapPoint& pb = graph_.corners[static_cast<std::size_t>(b)].point;
                          return std::atan2(pa.y - centroid.y, pa.x - centroid.x)
                               < std::atan2(pb.y - centroid.y, pb.x - centroid.x);
                      });
        }
    }

    /** @brief Mean position of a set of corners. */
    MapPoint centroid_of_(const std::vector<CornerId>& corner_ids) const {
        double sum_x = 0.0;
        double sum_y = 0.0;
        for (const CornerId corner_id : corner_ids) {
            sum_x += graph_.corners[static_cast<std::size_t>(corner_id)].point.x;
            sum_y += graph_.corners[static_cast<std::size_t>(corner_id)].point.y;
        }
        const double count = static_cast<double>(corner_ids.size());
        return {sum_x / count, sum_y / count};
    }

    /**
     * @brief Flags the band of cells and corners at the edge of the map.
     *
     * Bordering cells are forced to water by the water pass, which is what
     * guarantees a map is an island rather than a landmass sliced off by the
     * frame. Unlike the original this marks every offending corner of a cell,
     * not just the first one found.
     */
    void border_check_() {
        const double border = config_.border_length;
        const double grid_size = static_cast<double>(config_.grid_size);

        for (MapCenter& center : graph_.centers) {
            for (const CornerId corner_id : center.corners) {
                MapCorner& corner = graph_.corners[static_cast<std::size_t>(corner_id)];
                const double x = corner.point.x;
                const double y = corner.point.y;

                const bool outside = x < -border || x > grid_size + border
                                  || y < -border || y > grid_size + border;
                const double distance_to_frame =
                    std::min(std::min(x, grid_size - x), std::min(y, grid_size - y));

                if (outside || distance_to_frame <= border) {
                    corner.border = true;
                    center.border = true;
                }
            }
        }
    }

    /** @brief Runs every enabled pass, in dependency order. */
    void execute_passes_() {
        if (config_.enable_water)        PassWater{}.execute(graph_, config_, logger_);
        if (config_.enable_coast)        PassCoast{}.execute(graph_, config_, logger_);
        if (config_.enable_elevation)    PassElevation{}.execute(graph_, config_, logger_);
        if (config_.enable_temperature)  PassTemperature{}.execute(graph_, config_, logger_);
        if (config_.enable_rivers)       PassRivers{}.execute(graph_, config_, logger_);
        if (config_.enable_moisture)     PassMoisture{}.execute(graph_, config_, logger_);
        if (config_.enable_biomes)       PassBiomes{}.execute(graph_, config_, logger_);
        if (config_.enable_roads)        PassRoads{}.execute(graph_, config_, logger_);
        if (config_.enable_regions)      PassRegions{}.execute(graph_, config_, logger_);
        if (config_.enable_towns)        PassTowns{}.execute(graph_, config_, logger_);
        if (config_.enable_landmarks)    PassLandmarks{}.execute(graph_, config_, logger_);
        if (config_.enable_noisy_edges)  PassNoisyEdges{}.execute(graph_, config_, logger_);
    }

    /**
     * @brief Circumcentre of a triangle, which is the Voronoi vertex of its three cells.
     *
     * Falls back to the centroid when the three points are collinear. The
     * original divided by the zero determinant unguarded, seeding the graph
     * with NaN corners that then propagated through every pass.
     */
    static MapPoint circumcenter_(const MapPoint& a, const MapPoint& b, const MapPoint& c) {
        const double d = 2.0 * (a.x * (b.y - c.y) + b.x * (c.y - a.y) + c.x * (a.y - b.y));
        if (d == 0.0 || !std::isfinite(d)) {
            return {(a.x + b.x + c.x) / 3.0, (a.y + b.y + c.y) / 3.0};
        }

        const double a2 = a.x * a.x + a.y * a.y;
        const double b2 = b.x * b.x + b.y * b.y;
        const double c2 = c.x * c.x + c.y * c.y;
        return {(a2 * (b.y - c.y) + b2 * (c.y - a.y) + c2 * (a.y - b.y)) / d,
                (a2 * (c.x - b.x) + b2 * (a.x - c.x) + c2 * (b.x - a.x)) / d};
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_GENERATOR_H
