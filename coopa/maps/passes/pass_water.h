/**
 * @file pass_water.h
 * @brief First pass: samples the island noise field to decide land from water,
 *        then separates the connected ocean from inland lakes.
 */

#ifndef COOPA_MAPS_PASSES_PASS_WATER_H
#define COOPA_MAPS_PASSES_PASS_WATER_H

#include <cstddef>
#include <queue>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/noise.h>

namespace coopa {
namespace maps {

/**
 * @class PassWater
 * @brief Assigns `water` and `ocean` to every corner and cell.
 *
 * Three stages. A corner is water where the island noise field exceeds
 * `MapConfig::threshold_water`, or where it sits in the forced border band. A
 * cell is water once more than `threshold_water_count` of its corners are. A
 * flood fill outward from the border's water cells then marks `ocean`, which
 * is what distinguishes the sea from a lake -- both are `water`, only the sea
 * reaches the edge of the map.
 */
class PassWater {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; corners and cells must already be linked.
     * @param config Supplies the noise field and both water thresholds.
     * @param logger Receives a one-line progress message.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: water");

        // One sampler for the whole pass: building a FastNoiseLite per corner
        // would dominate generation time.
        const Noise noise(config.noise_island);

        for (MapCorner& corner : graph.corners) {
            const float value = noise.sample(corner.point.x, corner.point.y);
            corner.water = (value > config.threshold_water) || corner.border;
        }

        for (MapCenter& center : graph.centers) {
            if (center.border) {
                center.water = true;
                continue;
            }
            int water_count = 0;
            for (const CornerId corner_id : center.corners) {
                const MapCorner& corner = graph.corners[static_cast<std::size_t>(corner_id)];
                if (corner.water || corner.border) {
                    ++water_count;
                }
            }
            if (water_count > config.threshold_water_count) {
                center.water = true;
                center.ocean = false; // A lake until the flood fill proves otherwise.
            }
        }

        std::queue<CenterId> pending;
        for (MapCenter& center : graph.centers) {
            if (center.border && center.water) {
                center.ocean = true;
                pending.push(center.index);
            }
        }

        while (!pending.empty()) {
            const CenterId current = pending.front();
            pending.pop();
            for (const CenterId neighbor_id : graph.centers[static_cast<std::size_t>(current)].neighbors) {
                MapCenter& neighbor = graph.centers[static_cast<std::size_t>(neighbor_id)];
                if (neighbor.water && !neighbor.ocean) {
                    neighbor.ocean = true;
                    pending.push(neighbor.index);
                }
            }
        }
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_WATER_H
