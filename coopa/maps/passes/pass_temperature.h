/**
 * @file pass_temperature.h
 * @brief Assigns a climate to every corner and cell, from latitude, altitude
 *        and a noise field.
 */

#ifndef COOPA_MAPS_PASSES_PASS_TEMPERATURE_H
#define COOPA_MAPS_PASSES_PASS_TEMPERATURE_H

#include <algorithm>
#include <cmath>
#include <cstddef>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/noise.h>

namespace coopa {
namespace maps {

/**
 * @class PassTemperature
 * @brief Assigns `temperature` to every corner and cell.
 *
 * Without this the map has no climate: biomes were classified on elevation and
 * moisture alone, so a desert was as likely at the pole as at the equator and
 * whole swathes of the biome table were unreachable. Latitude runs along the y
 * axis, so a generated world reads as a north-south slice of a globe -- cold at
 * both edges, warm through the middle.
 *
 * Three terms combine: a latitude band, a lapse rate that cools high ground,
 * and a noise field that keeps isotherms from running as straight lines.
 */
class PassTemperature {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; requires `PassElevation` to have run.
     * @param config Supplies the lapse rate and the temperature noise field.
     * @param logger Receives a one-line progress message.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: temperature");

        const Noise noise(config.noise_temperature);
        const double grid_size = static_cast<double>(config.grid_size);
        if (grid_size <= 0.0) {
            return;
        }

        for (MapCorner& corner : graph.corners) {
            corner.temperature = sample_(noise, config, grid_size, corner.point, corner.elevation);
        }

        for (MapCenter& center : graph.centers) {
            if (center.corners.empty()) {
                center.temperature =
                    sample_(noise, config, grid_size, center.point, center.elevation);
                continue;
            }
            double sum = 0.0;
            for (const CornerId corner_id : center.corners) {
                sum += graph.corners[static_cast<std::size_t>(corner_id)].temperature;
            }
            center.temperature = sum / static_cast<double>(center.corners.size());
        }
    }

private:
    /** @brief Strength of the noise term, as a fraction of the full range. */
    static constexpr double k_variation_scale = 0.12;

    /**
     * @brief Temperature at one point.
     *
     * @param noise The configured variation field.
     * @param config Supplies the lapse rate.
     * @param grid_size Map extent, for normalising latitude.
     * @param point Position in grid units.
     * @param elevation Height in `[0, 1]`.
     * @return Warmth in `[0, 1]`; 0 polar, 1 equatorial.
     */
    static double sample_(const Noise& noise, const MapConfig& config, double grid_size,
                          const MapPoint& point, double elevation) {
        // 1 at the equator, falling to 0 at either pole.
        const double latitude = std::clamp(point.y / grid_size, 0.0, 1.0);
        const double band =
            1.0 - std::pow(std::abs(2.0 * latitude - 1.0), config.temperature_falloff);
        const double lapse = elevation * config.temperature_lapse_rate;
        const double variation = noise.sample(point.x, point.y) * k_variation_scale;
        return std::clamp(band - lapse + variation, 0.0, 1.0);
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_TEMPERATURE_H
