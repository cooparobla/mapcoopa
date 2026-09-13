/**
 * @file pass_rivers.h
 * @brief Fourth pass: drops river sources on the high ground and follows the
 *        downhill chain to the sea.
 */

#ifndef COOPA_MAPS_PASSES_PASS_RIVERS_H
#define COOPA_MAPS_PASSES_PASS_RIVERS_H

#include <cstddef>
#include <random>
#include <string>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @class PassRivers
 * @brief Raises `river` volume on the corners and edges a watercourse runs through.
 *
 * Sources are sampled uniformly from the corner set and rejected unless they
 * sit in a band of middling height -- too low and the river has nowhere to
 * run, too high and it starts on a peak the elevation pass made unreachable.
 * From an accepted source the walk follows `downslope` until it reaches the
 * coast, incrementing the volume on each corner and the edge between them.
 */
class PassRivers {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; requires `PassElevation` to have run.
     * @param config Supplies the seed and the number of rivers to attempt.
     * @param logger Receives progress, and a warning if the terrain rejects sources.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: rivers");

        if (graph.corners.empty() || config.river_count <= 0) {
            return;
        }

        std::mt19937 rng(static_cast<std::mt19937::result_type>(config.seed));
        std::uniform_int_distribution<std::size_t> pick(0, graph.corners.size() - 1);

        // The original retried a rejected source by decrementing its loop
        // counter, which spins forever on a map with no land in the accepted
        // elevation band -- an all-ocean seed hangs the generator outright.
        // Bounding the attempts turns that into a logged shortfall.
        const int max_attempts = config.river_count * k_attempts_per_river;
        int placed = 0;
        for (int attempt = 0; attempt < max_attempts && placed < config.river_count; ++attempt) {
            const CornerId source = static_cast<CornerId>(pick(rng));
            const MapCorner& candidate = graph.corners[static_cast<std::size_t>(source)];
            if (candidate.ocean
                || candidate.elevation < k_min_source_elevation
                || candidate.elevation > k_max_source_elevation) {
                continue;
            }
            carve_river_(graph, source);
            ++placed;
        }

        if (placed < config.river_count) {
            logger.warn("map pass: rivers placed " + std::to_string(placed) + " of "
                        + std::to_string(config.river_count)
                        + " requested; too little land in the source elevation band");
        }
    }

private:
    /** @brief Lowest elevation a river may start from. */
    static constexpr double k_min_source_elevation = 0.3;
    /** @brief Highest elevation a river may start from. */
    static constexpr double k_max_source_elevation = 0.9;
    /** @brief Source draws allowed per requested river before the pass gives up. */
    static constexpr int k_attempts_per_river = 16;

    /** @brief Walks one river from `source` down to the coast. */
    void carve_river_(MapGraph& graph, CornerId source) const {
        CornerId current = source;
        // The walk is strictly downhill and terminates at the coast or at a
        // basin, but guard the step count anyway: a downslope chain is only
        // acyclic because the elevations it was built from are, and a NaN
        // height would make that false.
        for (std::size_t step = 0; step < graph.corners.size(); ++step) {
            MapCorner& corner = graph.corners[static_cast<std::size_t>(current)];
            if (corner.coast) {
                break;
            }
            const CornerId next = corner.downslope;
            if (next == k_invalid_id || next == current) {
                break; // Basin: nowhere lower to flow.
            }

            const EdgeId edge_id = find_edge_(graph, current, next);
            if (edge_id == k_invalid_id) {
                break;
            }

            graph.edges[static_cast<std::size_t>(edge_id)].river += 1;
            corner.river += 1;
            graph.corners[static_cast<std::size_t>(next)].river += 1;
            current = next;
        }
    }

    /**
     * @brief Finds the edge joining two adjacent corners.
     *
     * Scans the first corner's own `protrudes` list -- at most a handful of
     * entries. The original scanned the entire edge array on every step of
     * every river, which is the dominant cost of the pass at any real grid size.
     *
     * @return The connecting edge, or `k_invalid_id` if the corners are not adjacent.
     */
    EdgeId find_edge_(const MapGraph& graph, CornerId from, CornerId to) const {
        for (const EdgeId edge_id : graph.corners[static_cast<std::size_t>(from)].protrudes) {
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            if ((edge.v0 == from && edge.v1 == to) || (edge.v0 == to && edge.v1 == from)) {
                return edge_id;
            }
        }
        return k_invalid_id;
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_RIVERS_H
