/**
 * @file water_levels.h
 * @brief Gives every body of water one flat surface, at the height it would really stand, and a
 *        basin under it.
 *
 * Shared by the two passes that finish moving the ground: `PassElevation` sets the levels once
 * the height field exists, and `PassValleys` settles them again after carving, because a valley
 * cut through a lake's shore lowers the very rim the lake was levelled against.
 */

#ifndef COOPA_MAPS_PASSES_WATER_LEVELS_H
#define COOPA_MAPS_PASSES_WATER_LEVELS_H

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>
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
 * @brief How far each cell of one body of water lies from its shoreline, in grid units.
 *
 * A Dijkstra relaxation over site-to-site distances *inside* the body. Real distance rather than
 * a count of hops, so an elongated lake gets an elongated trough instead of a staircase of
 * terraces, and so the answer does not change meaning when `grid_size` changes the spacing of
 * the cells.
 *
 * Seeded from the **shoreline**, not from the shore cells: a cell touching dry land starts at
 * half the distance to that land's site, which is where the boundary between the two runs. Seeded
 * at the sites instead, every cell of a small lake -- where every cell touches land -- would read
 * zero and the body would come out flat however wide it was.
 *
 * A body entirely ringed by other water has no shoreline to seed from. Every cell then reads
 * zero, which is the only honest answer when there is no shore to slope away from.
 *
 * @param graph The graph; reads `centers`.
 * @param body The cells of one body of water, as a flood fill collected them.
 * @return One distance per entry of `body`, in the same order.
 */
inline std::vector<double> shoreline_distances(const MapGraph& graph, const std::vector<CenterId>& body) {
    const double infinity = std::numeric_limits<double>::max();
    std::vector<double> distance(body.size(), infinity);
    // The body is a handful of cells out of thousands, so it is indexed rather than walked: a
    // per-cell array over the whole graph would be allocated once per lake.
    std::unordered_map<CenterId, std::size_t> slot;
    slot.reserve(body.size() * 2);
    for (std::size_t i = 0; i < body.size(); ++i) {
        slot.emplace(body[i], i);
    }

    // Smallest distance first, so a cell is settled the first time it is popped.
    using Entry = std::pair<double, std::size_t>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pending;
    for (std::size_t i = 0; i < body.size(); ++i) {
        const MapCenter& cell = graph.centers[static_cast<std::size_t>(body[i])];
        double shore = infinity;
        for (const CenterId neighbor_id : cell.neighbors) {
            const MapCenter& neighbor = graph.centers[static_cast<std::size_t>(neighbor_id)];
            if (!neighbor.water) {
                shore = std::min(shore, 0.5 * cell.point.distance_to(neighbor.point));
            }
        }
        if (shore < infinity) {
            distance[i] = shore;
            pending.emplace(shore, i);
        }
    }

    while (!pending.empty()) {
        const Entry entry = pending.top();
        pending.pop();
        if (entry.first > distance[entry.second]) {
            continue;   // already settled by a shorter path
        }
        const MapCenter& cell = graph.centers[static_cast<std::size_t>(body[entry.second])];
        for (const CenterId neighbor_id : cell.neighbors) {
            const auto found = slot.find(neighbor_id);
            if (found == slot.end()) {
                continue;   // outside this body
            }
            const MapCenter& neighbor = graph.centers[static_cast<std::size_t>(neighbor_id)];
            const double reached = entry.first + cell.point.distance_to(neighbor.point);
            if (reached < distance[found->second]) {
                distance[found->second] = reached;
                pending.emplace(reached, found->second);
            }
        }
    }

    for (double& d : distance) {
        if (d == infinity) {
            d = 0.0;   // no dry shore anywhere in the body
        }
    }
    return distance;
}

/**
 * @brief Gives every body of water one flat surface height, and every lake a basin under it.
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
 * - **A lake's bed** becomes a basin: at least `MapConfig::lake_depth_m` under that surface
 *   anywhere, and deeper by `MapConfig::lake_bed_grade` for every metre in from the shoreline,
 *   to a floor of `MapConfig::lake_depth_max_m`. Lake cells are ranked with the land, so
 *   a cell the water pass flooded can carry a "bed" well up a hillside; without the lowering the
 *   ground would rise through its own lake, and with one flat depth for the whole body a
 *   kilometre-wide lake is a pan of exactly zero bed spread.
 *
 * Only cell heights move. Corners keep the heights the drainage was read from, and the surface
 * a consumer samples (`MapGraph::elevation_at()`) interpolates cells, so the basin shows there.
 * The finer relief over that basin is `NaturalTerrainConfig::seabed`, applied at sample time.
 *
 * @param graph The graph to annotate; requires cell elevations to be final.
 * @param config Supplies the waterline, the lake depths and the bed grade.
 */
inline void assign_water_levels(MapGraph& graph, const MapConfig& config) {
    for (MapCenter& center : graph.centers) {
        // Below the waterline the ground is sea bed whatever the water pass decided, so the
        // sea's surface is the waterline there too.
        center.water_level = (center.ocean || center.elevation < config.sea_level) ? config.sea_level : 0.0;
    }

    const double margin = meters_to_height(config, k_lake_spill_margin_m);
    const double least_depth = meters_to_height(config, std::max(0.0, config.lake_depth_m));
    const double max_depth = std::max(least_depth, meters_to_height(config, std::max(0.0, config.lake_depth_max_m)));
    // The grade is rise over run in real metres both ways; folding the horizontal scale in here
    // leaves the walk below free to work in the graph's own grid units.
    const double grade = meters_to_height(
        config, std::max(0.0, config.lake_bed_grade) * config.meters_per_grid_unit);
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
        const std::vector<double> from_shore = shoreline_distances(graph, body);
        for (std::size_t i = 0; i < body.size(); ++i) {
            MapCenter& cell = graph.centers[static_cast<std::size_t>(body[i])];
            const double depth = std::min(max_depth, least_depth + grade * from_shore[i]);
            cell.water_level = surface;
            // Never under the floor of the height field: a basin deeper than its surface stands
            // high would otherwise be clamped flat, which is what the grade is here to avoid.
            cell.elevation = std::max(0.0, std::min(cell.elevation, surface - depth));
        }
    }
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_WATER_LEVELS_H
