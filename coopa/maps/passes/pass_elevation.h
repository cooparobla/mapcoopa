/**
 * @file pass_elevation.h
 * @brief Third pass: raises terrain by distance from the coast, reshapes the
 *        height distribution, and computes the downhill flow direction.
 */

#ifndef COOPA_MAPS_PASSES_PASS_ELEVATION_H
#define COOPA_MAPS_PASSES_PASS_ELEVATION_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <queue>
#include <vector>

#include <coopa/debug/logger.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @class PassElevation
 * @brief Assigns `elevation` to every corner and cell, and `downslope` to every corner.
 *
 * Height is graph distance from the map border rather than a second noise
 * field, which is what makes coastlines land at sea level and mountains form
 * in the interior of a landmass rather than at random. Crossing between two
 * land corners costs a full unit; every other step costs 0.01, so water is
 * effectively free to traverse and an inland sea does not push the mountains
 * around it any higher.
 */
class PassElevation {
public:
    /**
     * @brief Runs the pass.
     * @param graph The graph to annotate; requires `PassCoast` to have run.
     * @param config Supplies the smoothing iterations and strength.
     * @param logger Receives a one-line progress message.
     */
    void execute(MapGraph& graph, const MapConfig& config, coopa::debug::Logger& logger) const {
        logger.info("map pass: elevation");

        assign_corner_elevations_(graph);
        redistribute_elevations_(graph);
        // Before downslopes, not after: the flow direction has to be derived from
        // the field rivers will actually run on, or they carve uphill.
        smooth_elevations_(graph, config);
        assign_downslopes_(graph);
        assign_center_elevations_(graph);
        // Cells carry the mean of their corners, and a cell with few corners can
        // still sit well clear of its neighbours after the corner field is
        // smooth. Relaxing the cell field too is what the renderer and the biome
        // classifier actually read.
        smooth_center_elevations_(graph, config);
    }

private:
    /** @brief Cost of a step between two corners, before the land surcharge. */
    static constexpr double k_step_cost = 0.01;
    /** @brief Extra cost charged when both ends of a step are dry land. */
    static constexpr double k_land_step_cost = 1.0;
    /**
     * @brief Shapes the height histogram.
     *
     * Values above 1 place more of the map at low elevation, so a continent
     * reads as broad plains with a few peaks rather than a uniform ramp.
     */
    static constexpr double k_scale_factor = 1.1;

    /** @brief Breadth-first relaxation of corner heights outward from the border. */
    void assign_corner_elevations_(MapGraph& graph) const {
        std::queue<CornerId> pending;
        for (MapCorner& corner : graph.corners) {
            if (corner.border) {
                corner.elevation = 0.0;
                pending.push(corner.index);
            } else {
                corner.elevation = std::numeric_limits<double>::infinity();
            }
        }

        while (!pending.empty()) {
            const CornerId current_id = pending.front();
            pending.pop();

            const double current_elevation = graph.corners[static_cast<std::size_t>(current_id)].elevation;
            const bool current_is_water = graph.corners[static_cast<std::size_t>(current_id)].water;

            for (const CornerId neighbor_id : graph.corners[static_cast<std::size_t>(current_id)].adjacent) {
                MapCorner& neighbor = graph.corners[static_cast<std::size_t>(neighbor_id)];
                double new_elevation = k_step_cost + current_elevation;
                if (!current_is_water && !neighbor.water) {
                    new_elevation += k_land_step_cost;
                }
                if (new_elevation < neighbor.elevation) {
                    neighbor.elevation = new_elevation;
                    pending.push(neighbor.index);
                }
            }
        }
    }

    /**
     * @brief Remaps heights onto `sqrt(k) - sqrt(k * (1 - y))` by rank.
     *
     * Ranks are computed over an index array rather than by sorting the corner
     * storage: `MapGraph` guarantees `corners[i].index == i`, and every
     * adjacency list in the graph is expressed in those indices, so reordering
     * the array itself would silently corrupt the whole map.
     */
    void redistribute_elevations_(MapGraph& graph) const {
        const std::size_t count = graph.corners.size();
        if (count < 2) {
            return;
        }

        std::vector<CornerId> order(count);
        std::iota(order.begin(), order.end(), static_cast<CornerId>(0));
        std::sort(order.begin(), order.end(), [&graph](CornerId a, CornerId b) {
            return graph.corners[static_cast<std::size_t>(a)].elevation
                 < graph.corners[static_cast<std::size_t>(b)].elevation;
        });

        for (std::size_t rank = 0; rank < count; ++rank) {
            const double y = static_cast<double>(rank) / static_cast<double>(count - 1);
            double x = std::sqrt(k_scale_factor) - std::sqrt(k_scale_factor * (1.0 - y));
            if (x > 1.0) x = 1.0;
            graph.corners[static_cast<std::size_t>(order[rank])].elevation = x;
        }
    }

    /**
     * @brief Relaxes the height field toward the mean of each corner's neighbours.
     *
     * Border corners are pinned at zero so the coastline does not creep inland
     * as the interior settles. Reads from a snapshot each pass, so a corner is
     * not influenced by neighbours already moved this iteration -- doing it in
     * place would bias the result in traversal order.
     */
    void smooth_elevations_(MapGraph& graph, const MapConfig& config) const {
        if (config.elevation_smoothing_iterations <= 0 || graph.corners.empty()) {
            return;
        }
        const double strength = std::clamp(config.elevation_smoothing_strength, 0.0, 1.0);

        std::vector<double> previous(graph.corners.size());
        for (int iteration = 0; iteration < config.elevation_smoothing_iterations; ++iteration) {
            for (std::size_t i = 0; i < graph.corners.size(); ++i) {
                previous[i] = graph.corners[i].elevation;
            }
            for (MapCorner& corner : graph.corners) {
                if (corner.border || corner.adjacent.empty()) {
                    continue;
                }
                double sum = 0.0;
                for (const CornerId neighbor_id : corner.adjacent) {
                    sum += previous[static_cast<std::size_t>(neighbor_id)];
                }
                const double mean = sum / static_cast<double>(corner.adjacent.size());
                corner.elevation = previous[static_cast<std::size_t>(corner.index)]
                                 + strength * (mean - previous[static_cast<std::size_t>(corner.index)]);
            }
        }
    }

    /** @brief Points each corner at its lowest neighbour, or at itself in a basin. */
    void assign_downslopes_(MapGraph& graph) const {
        for (MapCorner& corner : graph.corners) {
            CornerId lowest = corner.index;
            double lowest_elevation = corner.elevation;
            for (const CornerId neighbor_id : corner.adjacent) {
                const MapCorner& neighbor = graph.corners[static_cast<std::size_t>(neighbor_id)];
                if (neighbor.elevation <= lowest_elevation) {
                    lowest = neighbor.index;
                    lowest_elevation = neighbor.elevation;
                }
            }
            corner.downslope = lowest;
        }
    }

    /**
     * @brief Relaxes the per-cell height field toward each cell's neighbours.
     *
     * Water cells are left alone so the coastline keeps its step -- smoothing
     * across the shore would drag the sea up onto the land and blur the very
     * boundary the elevation pass built everything else from.
     */
    void smooth_center_elevations_(MapGraph& graph, const MapConfig& config) const {
        if (config.elevation_smoothing_iterations <= 0 || graph.centers.empty()) {
            return;
        }
        const double strength = std::clamp(config.elevation_smoothing_strength, 0.0, 1.0);

        std::vector<double> previous(graph.centers.size());
        for (int iteration = 0; iteration < config.elevation_smoothing_iterations; ++iteration) {
            for (std::size_t i = 0; i < graph.centers.size(); ++i) {
                previous[i] = graph.centers[i].elevation;
            }
            for (MapCenter& center : graph.centers) {
                if (center.water || center.border || center.neighbors.empty()) {
                    continue;
                }
                double sum = 0.0;
                std::size_t counted = 0;
                for (const CenterId neighbor_id : center.neighbors) {
                    if (graph.centers[static_cast<std::size_t>(neighbor_id)].water) {
                        continue;
                    }
                    sum += previous[static_cast<std::size_t>(neighbor_id)];
                    ++counted;
                }
                if (counted == 0) {
                    continue;
                }
                const double mean = sum / static_cast<double>(counted);
                const double own = previous[static_cast<std::size_t>(center.index)];
                center.elevation = own + strength * (mean - own);
            }
        }
    }

    /** @brief Averages corner heights down onto their cells. */
    void assign_center_elevations_(MapGraph& graph) const {
        for (MapCenter& center : graph.centers) {
            if (center.corners.empty()) {
                center.elevation = 0.0;
                continue;
            }
            double sum = 0.0;
            for (const CornerId corner_id : center.corners) {
                sum += graph.corners[static_cast<std::size_t>(corner_id)].elevation;
            }
            center.elevation = sum / static_cast<double>(center.corners.size());
        }
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_PASSES_PASS_ELEVATION_H
