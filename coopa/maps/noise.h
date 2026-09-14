/**
 * @file noise.h
 * @brief A configured fractal noise sampler, wrapping FastNoiseLite.
 */

#ifndef COOPA_MAPS_NOISE_H
#define COOPA_MAPS_NOISE_H

#include <noise/FastNoiseLite.h>

#include <coopa/maps/map_config.h>

namespace coopa {
namespace maps {

/**
 * @class Noise
 * @brief Samples a fractal noise field configured once at construction.
 *
 * Holding the `FastNoiseLite` instance as a member is the whole point of this
 * class. The original wrapper rebuilt the generator and reapplied all eight
 * settings on *every* sample, and the water pass constructed a fresh wrapper
 * for every corner -- tens of thousands of full generator setups to produce
 * one island mask.
 */
class Noise {
public:
    /**
     * @brief Builds a sampler from a noise configuration.
     * @param config The field parameters to apply.
     */
    explicit Noise(const NoiseConfig& config) { configure_noise(generator_, config); }

    /**
     * @brief Samples the field.
     * @param x Horizontal position in grid units.
     * @param y Vertical position in grid units.
     * @return The noise value, nominally in `[-1, 1]`.
     */
    float sample(double x, double y) const {
        return generator_.GetNoise(static_cast<float>(x), static_cast<float>(y));
    }

private:
    FastNoiseLite generator_; /**< @brief The configured field; never reconfigured after construction. */
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_NOISE_H
