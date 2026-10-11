/**
 * @file water_levels.h
 * @brief Gives every body of water one flat surface, at the height it would really stand.
 *
 * Shared by the two passes that finish moving the ground: `PassElevation` sets the levels once
 * the height field exists, and `PassValleys` settles them again after carving, because a valley
 * cut through a lake's shore lowers the very rim the lake was levelled against.
 */

#ifndef COOPA_MAPS_PASSES_WATER_LEVELS_H
#define COOPA_MAPS_PASSES_WATER_LEVELS_H

#include <algorithm>
#include <cstddef>
#include <limits>
#include <queue>
#include <vector>

#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @brief How far a lake stands under the lowest point of its shore, in metres.
 *
 * Exactly at the rim, the lowest shore cell's site would sit *on* the water and the triangles
 * running away from it, down the outflow side, would read as wet ground outside the lake. A
 * metre under it keeps the whole shoreline strictly above the surface.
 */
inline constexpr double k_lake_spill_margin_m = 1.0;

/**
 * @brief Gives every body of water one flat surface height, and every lake a bed under it.
 *
 * `elevation` is the height of the *ground*, which under water is the bed. `water_level` is the
 * height of the *surface*, and a surface is flat -- which a bed is not. Stating them separately
 * is what lets a consumer flood a terrain mesh, and what lets the water layer draw a sheet
 * instead of a mottled field.
 *
 * - **The sea** takes `MapConfig::sea_level` everywhere, which after the elevation pass's split
 *   rank remap is exactly the height its bed rises to meet.
 * - **A lake** takes its **spill level**: the lowest cell on its shore, less
 *   `k_lake_spill_margin_m`. That is the height water poured into the basin would stop at, and
 *   the only one that keeps every shore above the water. (It used to be the highest bed in the
 *   body, which put a lake on a slope up to a hundred metres over the low side of its shore --
 *   water standing in the air.) Never below the sea it would otherwise drain into.
 * - **A lake's bed** is lowered to at least `MapConfig::lake_depth_m` under that surface. Lake
 *   cells are ranked with the land, so a cell the water pass flooded can carry a "bed" well up a
 *   hillside; without this the ground would rise through its own lake.
 *
 * Only cell heights move. Corners keep the heights the drainage was read from, and the surface
 * a consumer samples (`MapGraph::elevation_at()`) interpolates cells, so the basin shows there.
 *
 * @param graph The graph to annotate; requires cell elevations to be final.
 * @param config Supplies the waterline and the lake depth.
 */
inline void assign_water_levels(MapGraph& graph, const MapConfig& config) {
    for (MapCenter& center : graph.centers) {
        // Below the waterline the ground is sea bed whatever the water pass decided, so the
        // sea's surface is the waterline there too.
        center.water_level = (center.ocean || center.elevation < config.sea_level) ? config.sea_level : 0.0;
    }

    const double margin = meters_to_height(config, k_lake_spill_margin_m);
    const double depth = meters_to_height(config, std::max(0.0, config.lake_depth_m));
    std::vector<bool> visited(graph.centers.size(), false);

    for (MapCenter& seed : graph.centers) {
        const std::size_t seed_index = static_cast<std::size_t>(seed.index);
        if (!seed.water || seed.ocean || visited[seed_index]) {
            continue;
        }

        std::vector<CenterId> body;
        std::queue<CenterId> pending;
        pending.push(seed.index);
        visited[seed_index] = true;

        double rim = std::numeric_limits<double>::max();
        while (!pending.empty()) {
            const CenterId current = pending.front();
            pending.pop();
            body.push_back(current);

            for (const CenterId neighbor_id : graph.centers[static_cast<std::size_t>(current)].neighbors) {
                const std::size_t index = static_cast<std::size_t>(neighbor_id);
                const MapCenter& neighbor = graph.centers[index];
                if (!neighbor.water) {
                    rim = std::min(rim, neighbor.elevation);
                    continue;
                }
                if (visited[index] || neighbor.ocean) {
                    continue;
                }
                visited[index] = true;
                pending.push(neighbor_id);
            }
        }

        // A lake with no dry shore at all (a flooded pocket ringed by other water) has nothing
        // to spill over; its highest bed is the only height that keeps it out of the air.
        if (rim == std::numeric_limits<double>::max()) {
            rim = 0.0;
            for (const CenterId cell_id : body) {
                rim = std::max(rim, graph.centers[static_cast<std::size_t>(cell_id)].elevation + margin);
            }
        }
        const double surface = std::max(config.sea_level, rim - margin);
        for (const CenterId cell_id : body) {
            MapCenter& cell = graph.centers[static_cast<std::size_t>(cell_id)];
            cell.water_level = surface;
            cell.elevation = std::min(cell.elevation, surface - depth);
        }
    }
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_WATER_LEVELS_H
