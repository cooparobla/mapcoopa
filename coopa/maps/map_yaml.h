/**
 * @file map_yaml.h
 * @brief Serialises a generated map to YAML and reads it back.
 */

#ifndef COOPA_MAPS_MAP_YAML_H
#define COOPA_MAPS_MAP_YAML_H

#include <cstddef>
#include <exception>
#include <string>
#include <vector>

#include <fkYAML/node.hpp>
#include <glm/glm.hpp>

#include <coopa/collections/yaml_map.h>
#include <coopa/maps/biome.h>
#include <coopa/maps/landmark.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @brief Version stamped into every saved map.
 *
 * Written as `version:` at the document root so a loader can recognise a
 * schema it predates instead of silently misreading it.
 */
inline constexpr int k_map_yaml_version = 1;

/**
 * @brief Pixels per grid unit assumed when converting legacy pixel-valued widths.
 *
 * The old defaults rendered a 40-unit grid at 1024 pixels.
 */
inline constexpr double k_legacy_pixels_per_grid_unit = 1024.0 / 40.0;

namespace detail {

/** @brief Builds a YAML sequence node from a list of integer ids. */
template <typename IdType>
inline fkyaml::node id_sequence(const std::vector<IdType>& ids) {
    std::vector<fkyaml::node> nodes;
    nodes.reserve(ids.size());
    for (const IdType id : ids) {
        nodes.push_back(static_cast<int>(id));
    }
    return fkyaml::node::sequence(std::move(nodes));
}

/** @brief Reads a YAML sequence of integers back into a list of ids. */
template <typename IdType>
inline std::vector<IdType> read_id_sequence(const fkyaml::node& parent, const char* key) {
    std::vector<IdType> ids;
    if (!parent.contains(key)) {
        return ids;
    }
    const fkyaml::node& sequence = parent.at(key);
    if (!sequence.is_sequence()) {
        return ids;
    }
    ids.reserve(sequence.size());
    for (const fkyaml::node& item : sequence) {
        ids.push_back(static_cast<IdType>(item.get_value<int>()));
    }
    return ids;
}

/** @brief Builds a YAML sequence of `[x, y]` pairs from a polyline. */
inline fkyaml::node point_sequence(const std::vector<MapPoint>& points) {
    std::vector<fkyaml::node> nodes;
    nodes.reserve(points.size());
    for (const MapPoint& point : points) {
        nodes.push_back(fkyaml::node::sequence({fkyaml::node(point.x), fkyaml::node(point.y)}));
    }
    return fkyaml::node::sequence(std::move(nodes));
}

/** @brief Reads a YAML sequence of `[x, y]` pairs back into a polyline. */
inline std::vector<MapPoint> read_point_sequence(const fkyaml::node& parent, const char* key) {
    std::vector<MapPoint> points;
    if (!parent.contains(key)) {
        return points;
    }
    const fkyaml::node& sequence = parent.at(key);
    if (!sequence.is_sequence()) {
        return points;
    }
    points.reserve(sequence.size());
    for (const fkyaml::node& pair : sequence) {
        if (pair.is_sequence() && pair.size() >= 2) {
            points.push_back({pair[0].get_value<double>(), pair[1].get_value<double>()});
        }
    }
    return points;
}

/**
 * @brief Reads a key if present and convertible, otherwise returns the fallback.
 *
 * Tolerates a null node, which is what an empty string round-trips to: fkYAML
 * emits `key:` with nothing after it and parses that back as null, not as "".
 * Without this guard one unnamed place would abort a whole map load.
 */
template <typename T>
inline T read_or(const fkyaml::node& node, const char* key, T fallback) {
    if (!node.is_mapping() || !node.contains(key)) {
        return fallback;
    }
    const fkyaml::node& value = node.at(key);
    if (value.is_null()) {
        return fallback;
    }
    try {
        return value.get_value<T>();
    } catch (const std::exception&) {
        return fallback;
    }
}

} // namespace detail

/**
 * @brief Serialises a `MapConfig` to a YAML mapping node.
 * @param config The configuration to encode.
 * @return A mapping node holding every generation parameter.
 */
inline fkyaml::node config_to_node(const MapConfig& config) {
    fkyaml::node noise = fkyaml::node::mapping();
    noise["seed"] = config.noise_island.seed;
    noise["frequency"] = config.noise_island.frequency;
    noise["type"] = static_cast<int>(config.noise_island.type);
    noise["fractal_type"] = static_cast<int>(config.noise_island.fractal_type);
    noise["octaves"] = config.noise_island.octaves;
    noise["lacunarity"] = config.noise_island.lacunarity;
    noise["gain"] = config.noise_island.gain;
    noise["weighted_strength"] = config.noise_island.weighted_strength;

    fkyaml::node towns = fkyaml::node::mapping();
    towns["town_count"] = config.towns.town_count;
    towns["min_spacing"] = config.towns.min_spacing;
    towns["capital_count"] = config.towns.capital_count;
    towns["town_tier_count"] = config.towns.town_tier_count;
    towns["buildings_per_town"] = config.towns.buildings_per_town;
    towns["building_size_min"] = config.towns.building_size_min;
    towns["building_size_max"] = config.towns.building_size_max;
    towns["town_building_scale"] = config.towns.town_building_scale;
    towns["village_building_scale"] = config.towns.village_building_scale;
    towns["water_clearance"] = config.towns.water_clearance;
    towns["household_size_min"] = config.towns.household_size_min;
    towns["household_size_max"] = config.towns.household_size_max;
    towns["capital_density"] = config.towns.capital_density;
    towns["town_density"] = config.towns.town_density;
    towns["street_offset"] = config.towns.street_offset;
    towns["street_spacing"] = config.towns.street_spacing;
    towns["position_jitter"] = config.towns.position_jitter;
    towns["rotation_jitter"] = config.towns.rotation_jitter;
    towns["infill_attempts"] = config.towns.infill_attempts;

    fkyaml::node roads = fkyaml::node::mapping();
    roads["hub_count"] = config.roads.hub_count;
    roads["hub_min_spacing"] = config.roads.hub_min_spacing;
    roads["slope_cost"] = config.roads.slope_cost;
    roads["elevation_cost"] = config.roads.elevation_cost;
    roads["rough_ground_cost"] = config.roads.rough_ground_cost;
    roads["ford_cost"] = config.roads.ford_cost;
    roads["bridge_cost_per_volume"] = config.roads.bridge_cost_per_volume;
    roads["water_crossing_cost"] = config.roads.water_crossing_cost;
    roads["max_water_span"] = config.roads.max_water_span;
    roads["reuse_discount"] = config.roads.reuse_discount;
    roads["highway_traffic_share"] = config.roads.highway_traffic_share;
    roads["road_traffic_share"] = config.roads.road_traffic_share;
    roads["smoothing_iterations"] = config.roads.smoothing_iterations;

    fkyaml::node node = fkyaml::node::mapping();
    node["seed"] = config.seed;
    node["grid_size"] = config.grid_size;
    node["jitter"] = config.jitter;
    node["image_size"] = config.image_size;
    node["border_length"] = config.border_length;
    node["threshold_water"] = config.threshold_water;
    node["threshold_water_count"] = config.threshold_water_count;
    node["river_count"] = config.river_count;
    node["river_width_base"] = config.river_width_base;
    node["river_width_per_volume"] = config.river_width_per_volume;
    node["trail_width"] = config.trail_width;
    node["road_width"] = config.road_width;
    node["highway_width"] = config.highway_width;
    node["temperature_lapse_rate"] = config.temperature_lapse_rate;
    node["temperature_falloff"] = config.temperature_falloff;
    node["elevation_smoothing_iterations"] = config.elevation_smoothing_iterations;
    node["elevation_smoothing_strength"] = config.elevation_smoothing_strength;
    node["subdivide_noisy_edges"] = config.subdivide_noisy_edges;
    node["noise_island"] = std::move(noise);
    node["towns"] = std::move(towns);
    node["roads"] = std::move(roads);
    return node;
}

/**
 * @brief Reads a `MapConfig` back from a YAML mapping node.
 *
 * Every key is optional and falls back to the in-struct default, so a map
 * saved before a parameter existed still loads.
 *
 * @param node The mapping node to decode.
 * @return The decoded configuration.
 */
inline MapConfig config_from_node(const fkyaml::node& node) {
    MapConfig config;
    if (!node.is_mapping()) {
        return config;
    }

    config.seed = detail::read_or(node, "seed", config.seed);
    config.grid_size = detail::read_or(node, "grid_size", config.grid_size);
    config.jitter = detail::read_or(node, "jitter", config.jitter);
    config.image_size = detail::read_or(node, "image_size", config.image_size);
    config.border_length = detail::read_or(node, "border_length", config.border_length);
    config.threshold_water = detail::read_or(node, "threshold_water", config.threshold_water);
    config.threshold_water_count = detail::read_or(node, "threshold_water_count", config.threshold_water_count);
    config.river_count = detail::read_or(node, "river_count", config.river_count);
    config.river_width_base = detail::read_or(node, "river_width_base", config.river_width_base);
    config.river_width_per_volume =
        detail::read_or(node, "river_width_per_volume", config.river_width_per_volume);
    config.trail_width = detail::read_or(node, "trail_width", config.trail_width);
    config.road_width = detail::read_or(node, "road_width", config.road_width);
    config.highway_width = detail::read_or(node, "highway_width", config.highway_width);
    config.temperature_lapse_rate =
        detail::read_or(node, "temperature_lapse_rate", config.temperature_lapse_rate);
    config.temperature_falloff =
        detail::read_or(node, "temperature_falloff", config.temperature_falloff);
    config.elevation_smoothing_iterations =
        detail::read_or(node, "elevation_smoothing_iterations", config.elevation_smoothing_iterations);
    config.elevation_smoothing_strength =
        detail::read_or(node, "elevation_smoothing_strength", config.elevation_smoothing_strength);

    // Legacy: widths used to be pixel counts at a nominal 1024 render of a
    // 40-unit grid. Convert rather than ignore, so a map saved before widths
    // became physical still draws its rivers and roads at the intended size.
    if (!node.contains("river_width_base") && node.contains("river_factor")) {
        const double pixels = static_cast<double>(detail::read_or(node, "river_factor", 1));
        config.river_width_base = (pixels * 2.0) / k_legacy_pixels_per_grid_unit;
        config.river_width_per_volume = 2.0 / k_legacy_pixels_per_grid_unit;
    }
    if (!node.contains("road_width") && node.contains("road_size")) {
        const double pixels = static_cast<double>(detail::read_or(node, "road_size", 1));
        config.road_width = (pixels * 2.0) / k_legacy_pixels_per_grid_unit;
    }
    config.subdivide_noisy_edges = detail::read_or(node, "subdivide_noisy_edges", config.subdivide_noisy_edges);

    if (node.contains("noise_island")) {
        const fkyaml::node& noise = node.at("noise_island");
        config.noise_island.seed = detail::read_or(noise, "seed", config.noise_island.seed);
        config.noise_island.frequency = detail::read_or(noise, "frequency", config.noise_island.frequency);
        config.noise_island.type = static_cast<FastNoiseLite::NoiseType>(
            detail::read_or(noise, "type", static_cast<int>(config.noise_island.type)));
        config.noise_island.fractal_type = static_cast<FastNoiseLite::FractalType>(
            detail::read_or(noise, "fractal_type", static_cast<int>(config.noise_island.fractal_type)));
        config.noise_island.octaves = detail::read_or(noise, "octaves", config.noise_island.octaves);
        config.noise_island.lacunarity = detail::read_or(noise, "lacunarity", config.noise_island.lacunarity);
        config.noise_island.gain = detail::read_or(noise, "gain", config.noise_island.gain);
        config.noise_island.weighted_strength =
            detail::read_or(noise, "weighted_strength", config.noise_island.weighted_strength);
    }

    if (node.contains("roads")) {
        const fkyaml::node& roads = node.at("roads");
        RoadConfig& target = config.roads;
        target.hub_count = detail::read_or(roads, "hub_count", target.hub_count);
        target.hub_min_spacing = detail::read_or(roads, "hub_min_spacing", target.hub_min_spacing);
        target.slope_cost = detail::read_or(roads, "slope_cost", target.slope_cost);
        target.elevation_cost = detail::read_or(roads, "elevation_cost", target.elevation_cost);
        target.rough_ground_cost =
            detail::read_or(roads, "rough_ground_cost", target.rough_ground_cost);
        target.ford_cost = detail::read_or(roads, "ford_cost", target.ford_cost);
        target.bridge_cost_per_volume =
            detail::read_or(roads, "bridge_cost_per_volume", target.bridge_cost_per_volume);
        target.water_crossing_cost =
            detail::read_or(roads, "water_crossing_cost", target.water_crossing_cost);
        target.max_water_span = detail::read_or(roads, "max_water_span", target.max_water_span);
        target.reuse_discount = detail::read_or(roads, "reuse_discount", target.reuse_discount);
        target.highway_traffic_share =
            detail::read_or(roads, "highway_traffic_share", target.highway_traffic_share);
        target.road_traffic_share =
            detail::read_or(roads, "road_traffic_share", target.road_traffic_share);
        target.smoothing_iterations =
            detail::read_or(roads, "smoothing_iterations", target.smoothing_iterations);
    }

    if (node.contains("towns")) {
        const fkyaml::node& towns = node.at("towns");
        config.towns.town_count = detail::read_or(towns, "town_count", config.towns.town_count);
        config.towns.min_spacing = detail::read_or(towns, "min_spacing", config.towns.min_spacing);
        config.towns.capital_count = detail::read_or(towns, "capital_count", config.towns.capital_count);
        config.towns.town_tier_count = detail::read_or(towns, "town_tier_count", config.towns.town_tier_count);
        config.towns.buildings_per_town = detail::read_or(towns, "buildings_per_town", config.towns.buildings_per_town);
        config.towns.building_size_min =
            detail::read_or(towns, "building_size_min", config.towns.building_size_min);
        config.towns.building_size_max =
            detail::read_or(towns, "building_size_max", config.towns.building_size_max);
        // Legacy: one fixed size became a range. Collapse the range onto it so an
        // older map's buildings keep exactly the footprint they were saved with.
        if (!towns.contains("building_size_min") && towns.contains("building_size")) {
            const double fixed = detail::read_or(towns, "building_size", config.towns.building_size_min);
            config.towns.building_size_min = fixed;
            config.towns.building_size_max = fixed;
        }
        config.towns.town_building_scale =
            detail::read_or(towns, "town_building_scale", config.towns.town_building_scale);
        config.towns.village_building_scale =
            detail::read_or(towns, "village_building_scale", config.towns.village_building_scale);
        config.towns.water_clearance =
            detail::read_or(towns, "water_clearance", config.towns.water_clearance);
        config.towns.household_size_min =
            detail::read_or(towns, "household_size_min", config.towns.household_size_min);
        config.towns.household_size_max =
            detail::read_or(towns, "household_size_max", config.towns.household_size_max);
        config.towns.capital_density =
            detail::read_or(towns, "capital_density", config.towns.capital_density);
        config.towns.town_density =
            detail::read_or(towns, "town_density", config.towns.town_density);
        config.towns.street_offset = detail::read_or(towns, "street_offset", config.towns.street_offset);
        config.towns.street_spacing = detail::read_or(towns, "street_spacing", config.towns.street_spacing);
        config.towns.position_jitter = detail::read_or(towns, "position_jitter", config.towns.position_jitter);
        config.towns.rotation_jitter = detail::read_or(towns, "rotation_jitter", config.towns.rotation_jitter);
        config.towns.infill_attempts = detail::read_or(towns, "infill_attempts", config.towns.infill_attempts);
    }

    return config;
}

/**
 * @brief Serialises a map and the configuration that produced it to a YAML document.
 *
 * The whole graph goes in -- cells, corners, edges, road runs and settlements, with every
 * adjacency list -- so the document is a save of generator state rather than a
 * derived export, and `map_from_node()` reconstructs it exactly.
 *
 * Keys are abbreviated (`i`, `x`, `d0`) on the bulk arrays. At a grid size of
 * 50 that is roughly 2,600 cells, 10,000 corners and 15,000 edges, and spelling
 * the keys out in full roughly doubles the file for no gain in legibility once
 * a document is that size.
 *
 * Floating-point fields are lossy: fkYAML's emitter formats them through a
 * default-precision `std::ostringstream`, so they survive to six significant
 * digits and no further. Positions therefore round-trip to about 1e-5 grid
 * units and normalised values to about 1e-7, which is far below the resolution
 * anything downstream distinguishes -- a reloaded map renders pixel-identical.
 * Connectivity, biomes, river volumes and flags are integral or boolean and
 * round-trip exactly. Anything needing bit-exact heights should re-run the
 * generator from the saved seed rather than read them back from here.
 *
 * @param graph The map to encode.
 * @param config The configuration it was generated from.
 * @return The document root node.
 */
inline fkyaml::node map_to_node(const MapGraph& graph, const MapConfig& config) {
    std::vector<fkyaml::node> centers;
    centers.reserve(graph.centers.size());
    for (const MapCenter& center : graph.centers) {
        fkyaml::node node = fkyaml::node::mapping();
        node["i"] = static_cast<int>(center.index);
        node["x"] = center.point.x;
        node["y"] = center.point.y;
        node["elevation"] = center.elevation;
        node["moisture"] = center.moisture;
        node["temperature"] = center.temperature;
        node["biome"] = std::string(biome_name(center.biome));
        node["region"] = static_cast<int>(center.region);
        node["country"] = static_cast<int>(center.country);
        node["water"] = center.water;
        node["ocean"] = center.ocean;
        node["coast"] = center.coast;
        node["border"] = center.border;
        node["neighbors"] = detail::id_sequence(center.neighbors);
        node["corners"] = detail::id_sequence(center.corners);
        node["borders"] = detail::id_sequence(center.borders);
        centers.push_back(std::move(node));
    }

    std::vector<fkyaml::node> corners;
    corners.reserve(graph.corners.size());
    for (const MapCorner& corner : graph.corners) {
        fkyaml::node node = fkyaml::node::mapping();
        node["i"] = static_cast<int>(corner.index);
        node["x"] = corner.point.x;
        node["y"] = corner.point.y;
        node["elevation"] = corner.elevation;
        node["moisture"] = corner.moisture;
        node["temperature"] = corner.temperature;
        node["river"] = corner.river;
        node["downslope"] = static_cast<int>(corner.downslope);
        node["water"] = corner.water;
        node["ocean"] = corner.ocean;
        node["coast"] = corner.coast;
        node["border"] = corner.border;
        node["touches"] = detail::id_sequence(corner.touches);
        node["protrudes"] = detail::id_sequence(corner.protrudes);
        node["adjacent"] = detail::id_sequence(corner.adjacent);
        corners.push_back(std::move(node));
    }

    std::vector<fkyaml::node> edges;
    edges.reserve(graph.edges.size());
    for (const MapEdge& edge : graph.edges) {
        fkyaml::node node = fkyaml::node::mapping();
        node["i"] = static_cast<int>(edge.index);
        node["d0"] = static_cast<int>(edge.d0);
        node["d1"] = static_cast<int>(edge.d1);
        node["v0"] = static_cast<int>(edge.v0);
        node["v1"] = static_cast<int>(edge.v1);
        node["mx"] = edge.midpoint.x;
        node["my"] = edge.midpoint.y;
        node["river"] = edge.river;
        node["road"] = edge.road;
        // Written only when there is something to say, as with noisy0/noisy1
        // below: most edges carry no road, and three keys apiece across every
        // edge of an 80-cell grid is megabytes spent saying "none".
        if (edge.road_class != RoadClass::None) {
            node["road_class"] = std::string(road_class_name(edge.road_class));
            node["traffic"] = edge.traffic;
        }
        if (edge.bridge) {
            node["bridge"] = true;
        }
        // Only written when subdivision actually produced a path worth keeping:
        // an unsubdivided edge's two points are recoverable from v0, v1 and the
        // midpoint, and writing them would inflate the document for nothing.
        if (edge.noisy_points0.size() > 2 || edge.noisy_points1.size() > 2) {
            node["noisy0"] = detail::point_sequence(edge.noisy_points0);
            node["noisy1"] = detail::point_sequence(edge.noisy_points1);
        }
        edges.push_back(std::move(node));
    }

    std::vector<fkyaml::node> roads;
    roads.reserve(graph.roads.size());
    for (const MapRoad& road : graph.roads) {
        fkyaml::node node = fkyaml::node::mapping();
        node["class"] = std::string(road_class_name(road.road_class));
        node["edges"] = detail::id_sequence(road.edges);
        node["points"] = detail::point_sequence(road.points);
        roads.push_back(std::move(node));
    }

    std::vector<fkyaml::node> towns;
    towns.reserve(graph.towns.size());
    for (const MapTown& town : graph.towns) {
        std::vector<fkyaml::node> buildings;
        buildings.reserve(town.buildings.size());
        for (const MapBuilding& building : town.buildings) {
            fkyaml::node node = fkyaml::node::mapping();
            node["x"] = building.point.x;
            node["y"] = building.point.y;
            node["w"] = building.width;
            node["h"] = building.height;
            node["r"] = building.rotation;
            buildings.push_back(std::move(node));
        }

        fkyaml::node node = fkyaml::node::mapping();
        node["center"] = static_cast<int>(town.center);
        node["x"] = town.point.x;
        node["y"] = town.point.y;
        node["tier"] = std::string(town_tier_name(town.tier));
        node["score"] = town.score;
        if (!town.name.empty()) {
            node["name"] = town.name;
        }
        node["region"] = static_cast<int>(town.region);
        node["households"] = town.households;
        node["population"] = town.population;
        node["prosperity"] = town.prosperity;
        node["buildings"] = fkyaml::node::sequence(std::move(buildings));
        towns.push_back(std::move(node));
    }

    std::vector<fkyaml::node> regions;
    regions.reserve(graph.regions.size());
    for (const MapRegion& region : graph.regions) {
        fkyaml::node node = fkyaml::node::mapping();
        node["i"] = static_cast<int>(region.index);
        node["country"] = static_cast<int>(region.country);
        node["name"] = region.name;
        node["seed"] = static_cast<int>(region.seed);
        node["capital"] = static_cast<int>(region.capital);
        node["biome"] = std::string(biome_name(region.dominant_biome));
        node["population"] = region.population;
        node["area"] = region.area;
        // The renderer tints cells by this, so it is map data, not a derived
        // convenience -- a reloaded map must draw identically.
        node["r"] = region.color.r;
        node["g"] = region.color.g;
        node["b"] = region.color.b;
        node["cells"] = detail::id_sequence(region.cells);
        regions.push_back(std::move(node));
    }

    std::vector<fkyaml::node> countries;
    countries.reserve(graph.countries.size());
    for (const MapCountry& country : graph.countries) {
        fkyaml::node node = fkyaml::node::mapping();
        node["i"] = static_cast<int>(country.index);
        node["name"] = country.name;
        node["capital"] = static_cast<int>(country.capital);
        node["population"] = country.population;
        node["area"] = country.area;
        node["r"] = country.color.r;
        node["g"] = country.color.g;
        node["b"] = country.color.b;
        node["regions"] = detail::id_sequence(country.regions);
        countries.push_back(std::move(node));
    }

    std::vector<fkyaml::node> landmarks;
    landmarks.reserve(graph.landmarks.size());
    for (const MapLandmark& landmark : graph.landmarks) {
        fkyaml::node node = fkyaml::node::mapping();
        node["center"] = static_cast<int>(landmark.center);
        node["x"] = landmark.point.x;
        node["y"] = landmark.point.y;
        node["kind"] = std::string(landmark_kind_name(landmark.kind));
        node["name"] = landmark.name;
        node["region"] = static_cast<int>(landmark.region);
        landmarks.push_back(std::move(node));
    }

    fkyaml::node root = fkyaml::node::mapping();
    root["version"] = k_map_yaml_version;
    root["config"] = config_to_node(config);
    root["centers"] = fkyaml::node::sequence(std::move(centers));
    root["corners"] = fkyaml::node::sequence(std::move(corners));
    root["edges"] = fkyaml::node::sequence(std::move(edges));
    root["roads"] = fkyaml::node::sequence(std::move(roads));
    root["towns"] = fkyaml::node::sequence(std::move(towns));
    root["regions"] = fkyaml::node::sequence(std::move(regions));
    root["countries"] = fkyaml::node::sequence(std::move(countries));
    root["landmarks"] = fkyaml::node::sequence(std::move(landmarks));
    return root;
}

/**
 * @brief Reconstructs a map and its configuration from a YAML document.
 *
 * Edges whose noisy paths were omitted have them rebuilt from the endpoints
 * and midpoint, so a round-tripped graph renders identically to the original
 * whether or not subdivision was enabled when it was saved.
 *
 * @param root The document root produced by `map_to_node()`.
 * @param out_graph Receives the decoded map.
 * @param out_config Receives the decoded configuration.
 * @return True if the document held a recognised map, false otherwise.
 */
inline bool map_from_node(const fkyaml::node& root, MapGraph& out_graph, MapConfig& out_config) {
    if (!root.is_mapping() || !root.contains("centers")) {
        return false;
    }

    out_graph.clear();
    out_config = root.contains("config") ? config_from_node(root.at("config")) : MapConfig{};

    if (root.at("centers").is_sequence()) {
        for (const fkyaml::node& node : root.at("centers")) {
            MapCenter center;
            center.index = static_cast<CenterId>(detail::read_or(node, "i", 0));
            center.point = {detail::read_or(node, "x", 0.0), detail::read_or(node, "y", 0.0)};
            center.elevation = detail::read_or(node, "elevation", 0.0);
            center.moisture = detail::read_or(node, "moisture", 0.0);
            center.temperature = detail::read_or(node, "temperature", 0.0);
            center.biome = biome_from_name(detail::read_or(node, "biome", std::string("ocean")));
            center.region = static_cast<RegionId>(
                detail::read_or(node, "region", static_cast<int>(k_invalid_id)));
            center.country = static_cast<CountryId>(
                detail::read_or(node, "country", static_cast<int>(k_invalid_id)));
            center.water = detail::read_or(node, "water", false);
            center.ocean = detail::read_or(node, "ocean", false);
            center.coast = detail::read_or(node, "coast", false);
            center.border = detail::read_or(node, "border", false);
            center.neighbors = detail::read_id_sequence<CenterId>(node, "neighbors");
            center.corners = detail::read_id_sequence<CornerId>(node, "corners");
            center.borders = detail::read_id_sequence<EdgeId>(node, "borders");
            out_graph.centers.push_back(std::move(center));
        }
    }

    if (root.contains("corners") && root.at("corners").is_sequence()) {
        for (const fkyaml::node& node : root.at("corners")) {
            MapCorner corner;
            corner.index = static_cast<CornerId>(detail::read_or(node, "i", 0));
            corner.point = {detail::read_or(node, "x", 0.0), detail::read_or(node, "y", 0.0)};
            corner.elevation = detail::read_or(node, "elevation", 0.0);
            corner.moisture = detail::read_or(node, "moisture", 0.0);
            corner.temperature = detail::read_or(node, "temperature", 0.0);
            corner.river = detail::read_or(node, "river", 0);
            corner.downslope = static_cast<CornerId>(
                detail::read_or(node, "downslope", static_cast<int>(k_invalid_id)));
            corner.water = detail::read_or(node, "water", false);
            corner.ocean = detail::read_or(node, "ocean", false);
            corner.coast = detail::read_or(node, "coast", false);
            corner.border = detail::read_or(node, "border", false);
            corner.touches = detail::read_id_sequence<CenterId>(node, "touches");
            corner.protrudes = detail::read_id_sequence<EdgeId>(node, "protrudes");
            corner.adjacent = detail::read_id_sequence<CornerId>(node, "adjacent");
            out_graph.corners.push_back(std::move(corner));
        }
    }

    if (root.contains("edges") && root.at("edges").is_sequence()) {
        for (const fkyaml::node& node : root.at("edges")) {
            MapEdge edge;
            edge.index = static_cast<EdgeId>(detail::read_or(node, "i", 0));
            edge.d0 = static_cast<CenterId>(detail::read_or(node, "d0", static_cast<int>(k_invalid_id)));
            edge.d1 = static_cast<CenterId>(detail::read_or(node, "d1", static_cast<int>(k_invalid_id)));
            edge.v0 = static_cast<CornerId>(detail::read_or(node, "v0", static_cast<int>(k_invalid_id)));
            edge.v1 = static_cast<CornerId>(detail::read_or(node, "v1", static_cast<int>(k_invalid_id)));
            edge.midpoint = {detail::read_or(node, "mx", 0.0), detail::read_or(node, "my", 0.0)};
            edge.river = detail::read_or(node, "river", 0);
            edge.road = detail::read_or(node, "road", false);
            // A document written before roads had classes says only `road: true`.
            // Read that as a plain road rather than discarding it: the flag was
            // written because a road was there. Same courtesy the legacy
            // `road_size` conversion above extends to widths.
            edge.road_class = edge.road
                ? road_class_from_name(detail::read_or(node, "road_class", std::string("road")))
                : RoadClass::None;
            edge.traffic = detail::read_or(node, "traffic", 0);
            edge.bridge = detail::read_or(node, "bridge", false);
            edge.noisy_points0 = detail::read_point_sequence(node, "noisy0");
            edge.noisy_points1 = detail::read_point_sequence(node, "noisy1");
            edge.noisy = !edge.noisy_points0.empty();
            out_graph.edges.push_back(std::move(edge));
        }
    }

    if (root.contains("roads") && root.at("roads").is_sequence()) {
        for (const fkyaml::node& node : root.at("roads")) {
            MapRoad road;
            road.road_class =
                road_class_from_name(detail::read_or(node, "class", std::string("trail")));
            road.edges = detail::read_id_sequence<EdgeId>(node, "edges");
            road.points = detail::read_point_sequence(node, "points");
            out_graph.roads.push_back(std::move(road));
        }
    }

    if (root.contains("towns") && root.at("towns").is_sequence()) {
        for (const fkyaml::node& node : root.at("towns")) {
            MapTown town;
            town.center = static_cast<CenterId>(
                detail::read_or(node, "center", static_cast<int>(k_invalid_id)));
            town.point = {detail::read_or(node, "x", 0.0), detail::read_or(node, "y", 0.0)};
            town.tier = town_tier_from_name(detail::read_or(node, "tier", std::string("village")));
            town.score = detail::read_or(node, "score", 0.0);
            town.name = detail::read_or(node, "name", std::string());
            town.region = static_cast<RegionId>(
                detail::read_or(node, "region", static_cast<int>(k_invalid_id)));
            town.households = detail::read_or(node, "households", 0);
            town.population = detail::read_or(node, "population", 0);
            town.prosperity = detail::read_or(node, "prosperity", 0.0);

            if (node.contains("buildings") && node.at("buildings").is_sequence()) {
                for (const fkyaml::node& building_node : node.at("buildings")) {
                    MapBuilding building;
                    building.point = {detail::read_or(building_node, "x", 0.0),
                                      detail::read_or(building_node, "y", 0.0)};
                    building.width = detail::read_or(building_node, "w", 0.0);
                    building.height = detail::read_or(building_node, "h", 0.0);
                    building.rotation = detail::read_or(building_node, "r", 0.0);
                    town.buildings.push_back(building);
                }
            }
            out_graph.towns.push_back(std::move(town));
        }
    }

    if (root.contains("regions") && root.at("regions").is_sequence()) {
        for (const fkyaml::node& node : root.at("regions")) {
            MapRegion region;
            region.index = static_cast<RegionId>(detail::read_or(node, "i", 0));
            region.country = static_cast<CountryId>(
                detail::read_or(node, "country", static_cast<int>(k_invalid_id)));
            region.name = detail::read_or(node, "name", std::string());
            region.seed = static_cast<CenterId>(
                detail::read_or(node, "seed", static_cast<int>(k_invalid_id)));
            region.capital = static_cast<CenterId>(
                detail::read_or(node, "capital", static_cast<int>(k_invalid_id)));
            region.dominant_biome =
                biome_from_name(detail::read_or(node, "biome", std::string("grassland")));
            region.population = detail::read_or(node, "population", 0);
            region.area = detail::read_or(node, "area", 0.0);
            region.color = glm::vec3(detail::read_or(node, "r", 255.0f),
                                     detail::read_or(node, "g", 255.0f),
                                     detail::read_or(node, "b", 255.0f));
            region.cells = detail::read_id_sequence<CenterId>(node, "cells");
            out_graph.regions.push_back(std::move(region));
        }
    }

    if (root.contains("countries") && root.at("countries").is_sequence()) {
        for (const fkyaml::node& node : root.at("countries")) {
            MapCountry country;
            country.index = static_cast<CountryId>(detail::read_or(node, "i", 0));
            country.name = detail::read_or(node, "name", std::string());
            country.capital = static_cast<CenterId>(
                detail::read_or(node, "capital", static_cast<int>(k_invalid_id)));
            country.population = detail::read_or(node, "population", 0);
            country.area = detail::read_or(node, "area", 0.0);
            country.color = glm::vec3(detail::read_or(node, "r", 255.0f),
                                      detail::read_or(node, "g", 255.0f),
                                      detail::read_or(node, "b", 255.0f));
            country.regions = detail::read_id_sequence<RegionId>(node, "regions");
            out_graph.countries.push_back(std::move(country));
        }
    }

    if (root.contains("landmarks") && root.at("landmarks").is_sequence()) {
        for (const fkyaml::node& node : root.at("landmarks")) {
            MapLandmark landmark;
            landmark.center = static_cast<CenterId>(
                detail::read_or(node, "center", static_cast<int>(k_invalid_id)));
            landmark.point = {detail::read_or(node, "x", 0.0), detail::read_or(node, "y", 0.0)};
            landmark.kind =
                landmark_kind_from_name(detail::read_or(node, "kind", std::string("ruins")));
            landmark.name = detail::read_or(node, "name", std::string());
            landmark.region = static_cast<RegionId>(
                detail::read_or(node, "region", static_cast<int>(k_invalid_id)));
            out_graph.landmarks.push_back(std::move(landmark));
        }
    }

    // Rebuild the straight-edge paths the writer omitted.
    for (MapEdge& edge : out_graph.edges) {
        if (!edge.noisy_points0.empty()
            || edge.v0 == k_invalid_id || edge.v1 == k_invalid_id
            || static_cast<std::size_t>(edge.v0) >= out_graph.corners.size()
            || static_cast<std::size_t>(edge.v1) >= out_graph.corners.size()) {
            continue;
        }
        edge.noisy_points0 = {out_graph.corners[static_cast<std::size_t>(edge.v0)].point, edge.midpoint};
        edge.noisy_points1 = {out_graph.corners[static_cast<std::size_t>(edge.v1)].point, edge.midpoint};
        edge.noisy = true;
    }

    return true;
}

/**
 * @brief Writes a map to a YAML file.
 * @param graph The map to save.
 * @param config The configuration it was generated from.
 * @param filepath Destination path, including the `.yaml` extension.
 * @throws std::runtime_error If the file cannot be opened for writing.
 */
inline void save_map(const MapGraph& graph, const MapConfig& config, const std::string& filepath) {
    coopa::collections::YAMLMap document(map_to_node(graph, config));
    document.save(filepath);
}

/**
 * @brief Reads a map back from a YAML file.
 * @param filepath Path to a file previously written by `save_map()`.
 * @param out_graph Receives the decoded map.
 * @param out_config Receives the decoded configuration.
 * @return True on success, false if the file is missing or holds no map.
 */
inline bool load_map(const std::string& filepath, MapGraph& out_graph, MapConfig& out_config) {
    const coopa::collections::YAMLMap document = coopa::collections::YAMLMap::load(filepath);
    return map_from_node(document.get_raw_node(), out_graph, out_config);
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_YAML_H
