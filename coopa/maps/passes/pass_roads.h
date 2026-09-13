/**
 * @file pass_roads.h
 * @brief Seventh pass: lays roads along the elevation contours of a landmass.
 */

#ifndef COOPA_MAPS_PASSES_PASS_ROADS_H
#define COOPA_MAPS_PASSES_PASS_ROADS_H

#include <array>
#include <cstddef>
#include <queue>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @class PassRoads
 * @brief Assigns `MapEdge::road`.
 *
 * Every cell is given a contour level -- how many elevation bands inland from
 * the sea it sits -- by flooding outward from the coast. A road then runs
 * along any edge whose two corners fall in different bands, which traces the
 * boundary between bands. The result reads as switchbacks climbing a slope,
 * because that is geometrically what a contour line is.
 */
class PassRoads {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; requires `PassElevation` to have run.
     * @param config Unused; present so every pass shares one signature.
     * @param logger Receives a one-line progress message.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        (void)config;
        logger.info("map pass: roads");

        const std::array<double, k_contour_levels> thresholds = {0.0, 0.05, 0.54, 0.74};
        std::vector<int> center_contour(graph.centers.size(), k_unreached_contour);
        std::vector<int> corner_contour(graph.corners.size(), k_unreached_contour);

        std::queue<CenterId> pending;
        for (const MapCenter& center : graph.centers) {
            if (center.coast || center.ocean) {
                center_contour[static_cast<std::size_t>(center.index)] = 1;
                pending.push(center.index);
            }
        }

        while (!pending.empty()) {
            const CenterId current_id = pending.front();
            pending.pop();
            const int current_level = center_contour[static_cast<std::size_t>(current_id)];

            for (const CenterId neighbor_id : graph.centers[static_cast<std::size_t>(current_id)].neighbors) {
                const MapCenter& neighbor = graph.centers[static_cast<std::size_t>(neighbor_id)];

                int new_level = current_level;
                // Bounds first: the original indexed the threshold table before
                // checking the level was in range.
                while (new_level < static_cast<int>(k_contour_levels) - 1
                       && neighbor.elevation > thresholds[static_cast<std::size_t>(new_level)]) {
                    if (neighbor.water) {
                        break; // Water does not climb.
                    }
                    ++new_level;
                }

                if (new_level < center_contour[static_cast<std::size_t>(neighbor_id)]) {
                    center_contour[static_cast<std::size_t>(neighbor_id)] = new_level;
                    pending.push(neighbor_id);
                }
            }
        }

        for (const MapCenter& center : graph.centers) {
            const int level = center_contour[static_cast<std::size_t>(center.index)];
            for (const CornerId corner_id : center.corners) {
                if (corner_contour[static_cast<std::size_t>(corner_id)] > level) {
                    corner_contour[static_cast<std::size_t>(corner_id)] = level;
                }
            }
        }

        for (MapEdge& edge : graph.edges) {
            if (edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
                continue;
            }
            edge.road = corner_contour[static_cast<std::size_t>(edge.v0)]
                     != corner_contour[static_cast<std::size_t>(edge.v1)];
        }
    }

private:
    /** @brief Number of elevation bands a road network is cut into. */
    static constexpr std::size_t k_contour_levels = 4;
    /** @brief Sentinel level for a cell the flood fill has not reached. */
    static constexpr int k_unreached_contour = 999;
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_ROADS_H
