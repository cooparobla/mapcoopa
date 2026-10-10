/**
 * @file water_test.cpp
 * @brief Seas and lakes: ocean/lake separation, the waterline, flat water surfaces, and the
 *        water layer drawing those surfaces over (and slightly past) their beds.
 *
 * Rivers are rivers_test / river_surface_test; the shape of the landmass the sea surrounds is
 * shapes_test.
 */

#include <coopa/testing/test.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include <coopa/maps/map_renderer.h>

#include "support/map_fixtures.h"

using namespace mapcoopa_test;

COOPA_TEST_SUITE("water");

COOPA_TEST(water_separates_ocean_from_lakes) {
    std::size_t ocean = 0;
    std::size_t lake = 0;
    for (const MapCenter& center : shared_small_world().graph().centers) {
        if (center.water && center.ocean) ++ocean;
        if (center.water && !center.ocean) ++lake;
    }
    ASSERT_TRUE(ocean > 0);
    // Inland water that never reached the border is what the flood fill exists to
    // distinguish; without it every lake would be classified as sea.
    ASSERT_TRUE(lake > 0);
}

/**
 * @brief The waterline is a real height: the sea below it, everything else above.
 *
 * This is what makes "the ground here is under water" a comparison worth making. With the whole
 * field starting at zero and the sea pinned to the bottom of it, nothing would ever be below sea
 * level and the sea's own surface would render as the same black as dry land.
 */
COOPA_TEST(waterline_separates_sea_from_land) {
    MapConfig config = world_config(61);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::size_t sea_corners = 0;
    std::size_t ground_corners = 0;
    for (const MapCorner& corner : graph.corners) {
        if (corner.ocean) {
            ASSERT_TRUE(corner.elevation < config.sea_level);
            ++sea_corners;
        } else {
            ASSERT_TRUE(corner.elevation >= config.sea_level);
            ++ground_corners;
        }
    }
    ASSERT_TRUE(sea_corners > 0);
    ASSERT_TRUE(ground_corners > 0);

    // Land-relative height is what every threshold describing land is phrased against, so it
    // has to put the shoreline at 0 and the summit at 1.
    ASSERT_TRUE(land_height(config, config.sea_level) == 0.0f);
    ASSERT_TRUE(std::abs(land_height(config, 1.0) - 1.0) < 1e-12);
    ASSERT_TRUE(land_height(config, 0.0) == 0.0);  // submerged clamps to the shore

    // And the vertical scale round-trips, which is what lets a depth be stated in metres at all.
    ASSERT_TRUE(std::abs(height_to_meters(config, meters_to_height(config, 42.0)) - 42.0) < 1e-9);
}

/**
 * @brief Every body of water has one flat surface, which `elevation` does not.
 *
 * Drawing the water layer from `elevation` -- the height of the *bed* -- renders the sea
 * mottled. Only `border` corners are pinned to zero, and the rank remap then spreads every
 * corner across [0, 1], so an ocean cell away from the map edge has a small but nonzero height.
 * Flat water has to be stated.
 */
COOPA_TEST(water_bodies_are_flat) {
    MapConfig config = world_config(17);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::size_t ocean_cells = 0;
    for (const MapCenter& center : graph.centers) {
        if (!center.ocean) {
            continue;
        }
        ++ocean_cells;
        ASSERT_TRUE(center.water_level == config.sea_level);
    }
    ASSERT_TRUE(ocean_cells > 0);

    // And the bug this guards: the bed really does vary, so a flat surface is not something
    // that would have fallen out by accident.
    double lowest_bed = 1.0;
    double highest_bed = 0.0;
    for (const MapCenter& center : graph.centers) {
        if (center.ocean) {
            lowest_bed = std::min(lowest_bed, center.elevation);
            highest_bed = std::max(highest_bed, center.elevation);
        }
    }
    ASSERT_TRUE(highest_bed > lowest_bed);

    // Each lake is one surface across every cell of the body, and that surface is at or above
    // the highest bed in it, so no basin pokes through.
    std::vector<bool> visited(graph.centers.size(), false);
    std::size_t lakes = 0;
    for (const MapCenter& seed : graph.centers) {
        const std::size_t seed_index = static_cast<std::size_t>(seed.index);
        if (!seed.water || seed.ocean || visited[seed_index]) {
            continue;
        }
        ++lakes;
        std::vector<CenterId> pending{seed.index};
        visited[seed_index] = true;
        const double level = seed.water_level;
        while (!pending.empty()) {
            const CenterId current = pending.back();
            pending.pop_back();
            const MapCenter& cell = graph.centers[static_cast<std::size_t>(current)];
            ASSERT_TRUE(cell.water_level == level);
            ASSERT_TRUE(level >= cell.elevation - 1e-12);
            for (const CenterId neighbor_id : cell.neighbors) {
                const std::size_t index = static_cast<std::size_t>(neighbor_id);
                const MapCenter& neighbor = graph.centers[index];
                if (!visited[index] && neighbor.water && !neighbor.ocean) {
                    visited[index] = true;
                    pending.push_back(neighbor_id);
                }
            }
        }
    }
    ASSERT_TRUE(lakes > 0);
}

/**
 * @brief Inside a body of water, the drawn ground never rises through the surface.
 *
 * The companion to the river invariant, and it holds exactly, but only where it can. The
 * boundary is sharper than "away from the shore", and worth stating precisely because a
 * consumer meshing the two layers has to know where the guarantee stops.
 *
 * `MapCenter::elevation` under water is the *bed*, and for a coastal water cell it is not below
 * the water: cell heights are the mean of their corners, and a cell the sea reaches into has
 * corners up on the land. About one water cell in ten carries a "bed" above its own surface for
 * that reason. The ground is drawn by interpolating between cell heights, so those cells pull
 * the surface up through the water inside themselves *and* one ring further in.
 *
 * So the invariant is over cells that, together with every neighbour, have a bed at or below
 * their surface -- there every vertex of every triangle the sampler can reach is under water,
 * and a barycentric blend of values under water is under water. Measured across five maps, that
 * is zero violations out of ~2 400 samples each; anywhere else is the consumer's to clip.
 */
COOPA_TEST(water_bodies_cover_their_interiors) {
    for (int seed : {67, 31, 251}) {
        MapConfig config = world_config(seed);
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();

        const Noise terrain(config.noise_terrain);
        const TerrainDetail detail = make_terrain_detail(config, terrain);
        const RiverChannels channels = make_river_channels(graph, config);

        const auto submerged = [](const MapCenter& cell) {
            return cell.water && cell.elevation <= cell.water_level;
        };

        std::size_t sampled = 0;
        for (const MapCenter& center : graph.centers) {
            if (center.corners.empty() || !submerged(center)) {
                continue;
            }
            bool interior = true;
            for (const CenterId neighbor_id : center.neighbors) {
                if (!submerged(graph.centers[static_cast<std::size_t>(neighbor_id)])) {
                    interior = false;
                }
            }
            if (!interior) {
                continue;
            }
            for (const CornerId corner_id : center.corners) {
                const MapPoint& corner = graph.corners[static_cast<std::size_t>(corner_id)].point;
                // Pulled well in from the corner, so this measures the interior and not the
                // blend across the cell's own boundary.
                const double x = corner.x + (center.point.x - corner.x) * 0.7;
                const double y = corner.y + (center.point.y - corner.y) * 0.7;
                ASSERT_TRUE(graph.elevation_at(center, x, y, detail, channels)
                            <= center.water_level);
                ++sampled;
            }
        }
        ASSERT_TRUE(sampled > 0);
    }
}

/**
 * @brief Inside a body of water the flat surface wins, and it overhangs its edge.
 *
 * Rivers are stroked at ground height plus a depth, so drawing them *after* the bodies -- which
 * is what happened -- gouged a channel across every flat lake a river ran into. And two surfaces
 * that share an edge exactly show a seam wherever their meshes disagree by a rounding error,
 * which along a coastline is everywhere, so the water is extended past its own edge.
 */
COOPA_TEST(water_bodies_win_inside_and_overhang_their_edge) {
    MapConfig config = small_config(71);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    const Image water = MapLayers::water(graph, config);
    const double scale =
        static_cast<double>(config.image_size) / static_cast<double>(config.grid_size);

    std::size_t checked = 0;
    for (const MapCenter& center : graph.centers) {
        if (!center.water) {
            continue;
        }
        const int expected = static_cast<int>(std::clamp(center.water_level, 0.0, 1.0) * 255.0);
        for (const CornerId corner_id : center.corners) {
            // Sampled well inside the cell, so the overlap rim of a neighbour cannot account for
            // a mismatch.
            const MapPoint& corner = graph.corners[static_cast<std::size_t>(corner_id)].point;
            const int x = static_cast<int>((corner.x + (center.point.x - corner.x) * 0.7) * scale);
            const int y = static_cast<int>((corner.y + (center.point.y - corner.y) * 0.7) * scale);
            if (x < 0 || y < 0 || x >= water.width || y >= water.height) {
                continue;
            }
            ASSERT_EQ(static_cast<int>(water.color_at(x, y).r), expected);
            ++checked;
        }
    }
    ASSERT_TRUE(checked > 0);

    // The overhang: some pixels that are dry land carry a water height, because the sheet
    // reaches past its own shoreline.
    MapConfig sharp = config;
    sharp.water_edge_overlap_m = 0.0;
    const Image tight = MapLayers::water(graph, sharp);
    std::size_t wider = 0;
    for (int y = 0; y < water.height; ++y) {
        for (int x = 0; x < water.width; ++x) {
            if (water.color_at(x, y).r > 0.0f && tight.color_at(x, y).r == 0.0f) {
                ++wider;
            }
        }
    }
    ASSERT_TRUE(wider > 0);
}
