/**
 * @file map_renderer.h
 * @brief Software renderers that turn a `MapGraph` into a biome map or a heightmap.
 */

#ifndef COOPA_MAPS_MAP_RENDERER_H
#define COOPA_MAPS_MAP_RENDERER_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/maps/image.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @class BiomeRenderer
 * @brief Draws the map as coloured terrain, with rivers, roads and settlements.
 *
 * Cells are filled first, then rivers over them, then the road network, then
 * towns, so each layer covers the one below. Rivers follow the Voronoi edge
 * between two corners, roads follow the Delaunay edge between two cell sites --
 * the same `MapEdge` read two different ways, which is also why a bridge needs
 * no geometry of its own.
 *
 * Roads are stroked from `MapGraph::roads`, each run twice: a dark casing, then
 * the class colour one pixel narrower on top. The casing is what separates a
 * road from the ground it crosses -- without it a trail over dark forest
 * vanishes and a highway over pale desert is a smudge -- and the warm earth
 * colours are what stop a road reading as an administrative border, which is
 * what the old flat-black uniform stroke did.
 */
class BiomeRenderer {
public:
    /**
     * @brief Renders the map.
     * @param graph The map to draw.
     * @param config Supplies the image size, grid size and stroke widths.
     * @param palette Colours for each biome and overlay.
     * @return The rendered image, `config.image_size` square, RGB.
     */
    static Image render(const MapGraph& graph, const MapConfig& config,
                        const BiomePalette& palette = BiomePalette{}) {
        Image image;
        image.reset(config.image_size, config.image_size, 3, palette.background_color);

        const double scale = static_cast<double>(config.image_size)
                           / static_cast<double>(config.grid_size);

        std::vector<MapPoint> outline;
        for (const MapCenter& center : graph.centers) {
            outline = graph.cell_outline(center);
            for (MapPoint& point : outline) {
                point.x *= scale;
                point.y *= scale;
            }

            glm::vec3 color = palette.color_for(center.biome);
            if (config.show_regions && center.region != k_invalid_id
                && static_cast<std::size_t>(center.region) < graph.regions.size()) {
                // Tint rather than replace: a political map that hides the terrain
                // under flat colour stops being a map of the world.
                const glm::vec3& tint =
                    graph.regions[static_cast<std::size_t>(center.region)].color;
                color = color * (1.0f - config.region_tint) + tint * config.region_tint;
            }
            fill_polygon(image, outline, color);
        }

        for (const MapEdge& edge : graph.edges) {
            if (edge.river <= 0 || edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
                continue;
            }
            // A river that has already reached the sea is not drawn across it.
            if (graph.corners[static_cast<std::size_t>(edge.v1)].ocean) {
                continue;
            }
            const MapPoint& v0 = graph.corners[static_cast<std::size_t>(edge.v0)].point;
            const MapPoint& v1 = graph.corners[static_cast<std::size_t>(edge.v1)].point;
            draw_line(image,
                      static_cast<int>(v0.x * scale), static_cast<int>(v0.y * scale),
                      static_cast<int>(v1.x * scale), static_cast<int>(v1.y * scale),
                      half_width_pixels_(river_width(config, edge.river), scale),
                      palette.river_color);
        }

        // Every casing first, then every fill, rather than casing-and-fill per
        // run: drawn run by run, the dark casing of whichever road happens to
        // come later cuts a notch straight across the one already there, and
        // every junction in the network ends up nicked. Within each phase the
        // order is least to most travelled, so a highway is never interrupted
        // by the trail that joins it.
        static constexpr std::array<RoadClass, 3> k_road_order = {
            RoadClass::Trail, RoadClass::Road, RoadClass::Highway};

        for (int phase = 0; phase < 2; ++phase) {
            const bool casing = phase == 0;
            for (const RoadClass road_class : k_road_order) {
                const int half_width =
                    half_width_pixels_(road_width_for(config, road_class), scale)
                    + (casing ? 1 : 0);
                const glm::vec3& color =
                    casing ? palette.road_casing_color : palette.color_for(road_class);

                if (graph.roads.empty()) {
                    // A map loaded from a document written before roads were
                    // traced into runs still has its per-edge flags. Stroke
                    // those straight rather than drawing nothing.
                    for (const MapEdge& edge : graph.edges) {
                        if (edge.road_class != road_class || edge.d0 == k_invalid_id
                            || edge.d1 == k_invalid_id) {
                            continue;
                        }
                        draw_segment_(image, graph.centers[static_cast<std::size_t>(edge.d0)].point,
                                      graph.centers[static_cast<std::size_t>(edge.d1)].point,
                                      scale, half_width, color);
                    }
                    continue;
                }

                for (const MapRoad& run : graph.roads) {
                    if (run.road_class != road_class) {
                        continue;
                    }
                    for (std::size_t i = 0; i + 1 < run.points.size(); ++i) {
                        draw_segment_(image, run.points[i], run.points[i + 1], scale, half_width,
                                      color);
                    }
                }
            }
        }

        for (const MapEdge& edge : graph.edges) {
            if (edge.bridge) {
                draw_bridge_(image, graph, edge, config, scale, palette);
            }
        }

        for (const MapTown& town : graph.towns) {
            // Drawn as the rotated quad, not an axis-aligned blob: the yaw is
            // part of what the generator guarantees, and a square marker would
            // hide whether buildings actually line up along their street.
            std::vector<MapPoint> footprint(4);
            for (const MapBuilding& building : town.buildings) {
                const std::array<MapPoint, 4> corners = building_corners(building);
                for (std::size_t i = 0; i < 4; ++i) {
                    footprint[i] = {corners[i].x * scale, corners[i].y * scale};
                }
                fill_polygon(image, footprint, palette.building_color);
            }
            draw_marker_(image, town.point, scale, marker_radius_(town.tier), palette.town_color);
        }

        for (const MapLandmark& landmark : graph.landmarks) {
            draw_landmark_(image, landmark, scale, palette);
        }

        return image;
    }

private:
    /**
     * @brief Converts a physical width in grid units to a brush half-width in pixels.
     * @param width_grid The feature's width in grid units.
     * @param scale Pixels per grid unit.
     * @return At least 1, so a thin feature never vanishes at low resolution.
     */
    static int half_width_pixels_(double width_grid, double scale) {
        return std::max(1, static_cast<int>(width_grid * scale * 0.5));
    }

    /** @brief Strokes one grid-space segment, converting to pixels on the way. */
    static void draw_segment_(Image& image, const MapPoint& from, const MapPoint& to, double scale,
                              int half_width, const glm::vec3& color) {
        draw_line(image,
                  static_cast<int>(from.x * scale), static_cast<int>(from.y * scale),
                  static_cast<int>(to.x * scale), static_cast<int>(to.y * scale),
                  half_width, color);
    }

    /**
     * @brief Draws the parapet of a bridge or causeway, across the road.
     *
     * Placed at the midpoint of the Delaunay edge, which is exactly where the
     * road crosses the water: the dual Voronoi edge the river runs along lies on
     * the perpendicular bisector of `d0`-`d1`, so it meets the road there and
     * nowhere else. The tick runs along that bisector -- square across the
     * road -- so a crossing reads as a crossing at a glance, the same way
     * `draw_landmark_()` shapes a marker to its kind.
     */
    static void draw_bridge_(Image& image, const MapGraph& graph, const MapEdge& edge,
                             const MapConfig& config, double scale, const BiomePalette& palette) {
        if (edge.d0 == k_invalid_id || edge.d1 == k_invalid_id) {
            return;
        }
        const MapPoint& d0 = graph.centers[static_cast<std::size_t>(edge.d0)].point;
        const MapPoint& d1 = graph.centers[static_cast<std::size_t>(edge.d1)].point;

        const double dx = d1.x - d0.x;
        const double dy = d1.y - d0.y;
        const double length = std::hypot(dx, dy);
        if (length == 0.0) {
            return;
        }
        const double reach = road_width_for(config, edge.road_class) * k_bridge_span;
        const double offset_x = -dy / length * reach;
        const double offset_y = dx / length * reach;
        const MapPoint crossing{(d0.x + d1.x) * 0.5, (d0.y + d1.y) * 0.5};

        draw_segment_(image, {crossing.x + offset_x, crossing.y + offset_y},
                      {crossing.x - offset_x, crossing.y - offset_y}, scale,
                      half_width_pixels_(road_width_for(config, edge.road_class) * 0.5, scale),
                      palette.road_casing_color);
    }

    /** @brief How far a bridge parapet reaches either side of the road, in road widths. */
    static constexpr double k_bridge_span = 1.1;

    /** @brief Marker half-width in pixels for each settlement size class. */
    static int marker_radius_(TownTier tier) {
        switch (tier) {
            case TownTier::Capital: return 6;
            case TownTier::Town:    return 4;
            case TownTier::Village: return 2;
        }
        return 2;
    }

    /**
     * @brief Draws a landmark, shaped so its kind is readable at a glance.
     *
     * Natural features get a diamond, works a square, a region's wonder a larger
     * diamond -- enough to tell them apart on a preview without a legend.
     */
    static void draw_landmark_(Image& image, const MapLandmark& landmark, double scale,
                               const BiomePalette& palette) {
        const int cx = static_cast<int>(landmark.point.x * scale);
        const int cy = static_cast<int>(landmark.point.y * scale);
        const bool natural = landmark.kind != LandmarkKind::Ruins
                          && landmark.kind != LandmarkKind::StandingStones
                          && landmark.kind != LandmarkKind::Monolith
                          && landmark.kind != LandmarkKind::Wreck
                          && landmark.kind != LandmarkKind::Tower
                          && landmark.kind != LandmarkKind::Shrine;
        const int radius = landmark.kind == LandmarkKind::Wonder ? 5 : 3;
        const glm::vec3& color =
            natural ? palette.landmark_natural_color : palette.landmark_built_color;

        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                // Diamond for natural, square for built.
                if (natural && std::abs(x) + std::abs(y) > radius) {
                    continue;
                }
                image.set_pixel(cx + x, cy + y, color);
            }
        }
    }

    /** @brief Draws a filled square centred on a grid-space point. */
    static void draw_marker_(Image& image, const MapPoint& point, double scale, int radius,
                             const glm::vec3& color) {
        const int cx = static_cast<int>(point.x * scale);
        const int cy = static_cast<int>(point.y * scale);
        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                image.set_pixel(cx + x, cy + y, color);
            }
        }
    }
};

/**
 * @class ElevationRenderer
 * @brief Draws the map as a greyscale heightmap.
 *
 * Flat-shaded per cell rather than interpolated per pixel: a consumer wanting
 * a smooth heightfield should sample `MapGraph::elevation_at()` at whatever
 * resolution it needs, not re-derive it from a lossy 8-bit image.
 */
class ElevationRenderer {
public:
    /**
     * @brief Renders the heightmap.
     * @param graph The map to draw.
     * @param config Supplies the image size, grid size and river widths.
     * @param palette Supplies the background colour only.
     * @return The rendered image, `config.image_size` square, RGB greyscale.
     */
    static Image render(const MapGraph& graph, const MapConfig& config,
                        const BiomePalette& palette = BiomePalette{}) {
        Image image;
        image.reset(config.image_size, config.image_size, 3, palette.background_color);

        const double scale = static_cast<double>(config.image_size)
                           / static_cast<double>(config.grid_size);

        std::vector<MapPoint> outline;
        for (const MapCenter& center : graph.centers) {
            outline = graph.cell_outline(center);
            for (MapPoint& point : outline) {
                point.x *= scale;
                point.y *= scale;
            }

            // Shaded per pixel from the cell's corner heights rather than filled
            // flat: a flat fill draws the Voronoi tessellation, not the terrain.
            const double inverse_scale = 1.0 / scale;
            fill_polygon_shaded(image, outline,
                [&graph, &center, inverse_scale](double px, double py) {
                    const double height = graph.elevation_at(center, px * inverse_scale,
                                                             py * inverse_scale);
                    const float grey = static_cast<float>(std::clamp(height, 0.0, 1.0) * 255.0);
                    return glm::vec3(grey, grey, grey);
                });
        }

        // The whole river network is stencilled first and dimmed in one pass, so
        // a pixel drops by k_river_darken whether one stroke crosses it or six
        // meet there. Dimming per stroke instead makes confluences read as
        // black pits and every river several times deeper than intended.
        CoverageMask rivers;
        rivers.reset(image.width, image.height);
        for (const MapEdge& edge : graph.edges) {
            if (edge.river <= 0 || edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
                continue;
            }
            const MapPoint& v0 = graph.corners[static_cast<std::size_t>(edge.v0)].point;
            const MapPoint& v1 = graph.corners[static_cast<std::size_t>(edge.v1)].point;
            mark_line(rivers,
                      static_cast<int>(v0.x * scale), static_cast<int>(v0.y * scale),
                      static_cast<int>(v1.x * scale), static_cast<int>(v1.y * scale),
                      std::max(1, static_cast<int>(river_width(config, edge.river) * scale * 0.5)));
        }
        darken_masked(image, rivers, k_river_darken);

        return image;
    }

    /** @brief How much a river dims the terrain beneath it, per channel. */
    static constexpr int k_river_darken = 10;

};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_RENDERER_H
