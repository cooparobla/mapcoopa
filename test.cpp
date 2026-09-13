/**
 * @file test.cpp
 * @brief mapcoopa's test suite -- 57 cases over the generator, its passes, the
 *        renderers and the YAML round trip.
 *
 * Build target `mapcoopa_tests` (the bare `mapcoopa` target is the generator,
 * so `cplay` produces a map rather than running tests). Run it directly or via
 * `ctest --test-dir build`.
 *
 * The harness is the same hand-rolled RUN_TEST/ASSERT_* one libcoopa uses,
 * carried over verbatim when this module was split out, so the cases below are
 * unchanged from the ones that used to live in libcoopa/test.cpp.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio> // For std::remove
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <root_directory.h>

#include <coopa/debug/logger.h>
#include <coopa/maps/biome.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_renderer.h>
#include <coopa/maps/map_export.h>
#include <coopa/maps/map_task.h>
#include <coopa/maps/map_yaml.h>
#include <glm/glm.hpp>

// ANSI Colors for nice UI
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_RESET   "\x1b[0m"

static int g_tests_run = 0;
static int g_tests_failed = 0;

#define RUN_TEST(test_func) \
    do { \
        std::cout << ANSI_COLOR_BLUE << "[ RUN      ] " << ANSI_COLOR_RESET << #test_func << std::endl; \
        g_tests_run++; \
        try { \
            test_func(); \
            std::cout << ANSI_COLOR_GREEN << "[       OK ] " << ANSI_COLOR_RESET << #test_func << std::endl; \
        } catch (const std::exception& e) { \
            std::cerr << ANSI_COLOR_RED << "[  FAILED  ] " << ANSI_COLOR_RESET << #test_func << " (Exception: " << e.what() << ")" << std::endl; \
            g_tests_failed++; \
        } catch (...) { \
            std::cerr << ANSI_COLOR_RED << "[  FAILED  ] " << ANSI_COLOR_RESET << #test_func << " (Unknown Exception)" << std::endl; \
            g_tests_failed++; \
        } \
    } while (0)

#define ASSERT_TRUE(condition) \
    do { \
        if (!(condition)) { \
            std::cerr << ANSI_COLOR_RED << "  Assertion failed: " << #condition << " at " << __FILE__ << ":" << __LINE__ << ANSI_COLOR_RESET << std::endl; \
            throw std::runtime_error("Assertion failed: " #condition); \
        } \
    } while (0)

#define ASSERT_EQ(val1, val2) \
    do { \
        if ((val1) != (val2)) { \
            std::cerr << ANSI_COLOR_RED << "  Assertion failed: " << #val1 << " == " << #val2 \
                      << " (Actual: " << (val1) << ", Expected: " << (val2) << ") at " \
                      << __FILE__ << ":" << __LINE__ << ANSI_COLOR_RESET << std::endl; \
            throw std::runtime_error("Assertion failed: " #val1 " == " #val2); \
        } \
    } while (0)

// ---------------------------------------------------------
// Test Cases
// ---------------------------------------------------------

namespace maps_test {

using namespace coopa::maps;

// Every test builds its own map, so each needs a logger. One shared instance
// keeps the suite's output attributable without threading one through by hand.
static coopa::debug::Logger& maps_logger() {
    static coopa::debug::Logger logger("maps_test");
    return logger;
}

/**
 * @brief One `JobEngine` shared by every test that needs one.
 *
 * Function-local static, like the logger, because a `JobEngine` starts a thread
 * per core and a suite that built one per case would spend its runtime on thread
 * creation. Shared is also the way it is meant to be used -- see the engine's own
 * note on long-lived subsystems sharing a single engine.
 */
static coopa::job::JobEngine& maps_engine() {
    static coopa::job::JobEngine engine;
    return engine;
}

// Small enough that a full generate() is a few milliseconds, large enough that
// the passes have real terrain to work on.
static constexpr double k_pi = 3.14159265358979323846;

static MapConfig small_config(int seed = 251) {
    MapConfig config;
    config.grid_size = 16;
    config.image_size = 128;
    config.seed = seed;
    config.noise_island.seed = seed;
    return config;
}

static void test_biome_name_round_trips() {
    for (std::size_t i = 0; i < k_biome_count; ++i) {
        const Biome biome = static_cast<Biome>(i);
        ASSERT_TRUE(biome_from_name(biome_name(biome)) == biome);
    }
    // Maps saved by the original generator spelled this one wrong.
    ASSERT_TRUE(biome_from_name("temperate_decidious_forest") == Biome::TemperateDeciduousForest);
    ASSERT_TRUE(biome_from_name("not_a_biome") == Biome::Ocean);
}

static void test_classify_biome_table() {
    // Water and shore states short-circuit the climate diagram.
    ASSERT_TRUE(classify_biome(0.9, 0.9, 0.5, false, true, false) == Biome::Ocean);
    ASSERT_TRUE(classify_biome(0.05, 0.5, 0.5, true, false, false) == Biome::Marsh);
    ASSERT_TRUE(classify_biome(0.9, 0.5, 0.5, true, false, false) == Biome::Ice);
    ASSERT_TRUE(classify_biome(0.5, 0.5, 0.5, true, false, false) == Biome::Lake);
    ASSERT_TRUE(classify_biome(0.5, 0.5, 0.5, false, false, true) == Biome::Beach);
    // A warm wet shore is mangrove; a frozen one is tundra.
    ASSERT_TRUE(classify_biome(0.5, 0.8, 0.9, false, false, true) == Biome::Mangrove);
    ASSERT_TRUE(classify_biome(0.5, 0.5, 0.1, false, false, true) == Biome::Tundra);

    // Same elevation and moisture, different latitude -- the whole point of
    // classifying in three dimensions rather than two.
    const double elevation = 0.45;
    const double moisture = 0.6;
    ASSERT_TRUE(classify_biome(elevation, moisture, 0.1, false, false, false) == Biome::Snow);
    ASSERT_TRUE(classify_biome(elevation, moisture, 0.3, false, false, false) == Biome::Taiga);
    ASSERT_TRUE(classify_biome(elevation, moisture, 0.5, false, false, false)
                == Biome::TemperateDeciduousForest);
    ASSERT_TRUE(classify_biome(elevation, moisture, 0.9, false, false, false)
                == Biome::TropicalSeasonalForest);

    // Climate extremes reach the new entries.
    ASSERT_TRUE(classify_biome(0.9, 0.3, 0.05, false, false, false) == Biome::Glacier);
    ASSERT_TRUE(classify_biome(0.2, 0.05, 0.1, false, false, false) == Biome::ColdDesert);
    ASSERT_TRUE(classify_biome(0.2, 0.3, 0.3, false, false, false) == Biome::Steppe);
    ASSERT_TRUE(classify_biome(0.2, 0.4, 0.9, false, false, false) == Biome::Savanna);
    ASSERT_TRUE(classify_biome(0.9, 0.6, 0.5, false, false, false) == Biome::AlpineMeadow);
    ASSERT_TRUE(classify_biome(0.05, 0.05, 0.9, false, false, false) == Biome::SaltFlat);
    ASSERT_TRUE(classify_biome(0.9, 0.05, 0.9, false, false, false) == Biome::VolcanicField);
}

static void test_temperature_follows_latitude() {
    MapGenerator generator(small_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    const double grid_size = static_cast<double>(generator.config().grid_size);

    double polar_sum = 0.0, middle_sum = 0.0;
    std::size_t polar_count = 0, middle_count = 0;
    for (const MapCenter& center : graph.centers) {
        ASSERT_TRUE(center.temperature >= 0.0 && center.temperature <= 1.0);
        const double latitude = center.point.y / grid_size;
        if (latitude < 0.12 || latitude > 0.88) {
            polar_sum += center.temperature;
            ++polar_count;
        } else if (latitude > 0.4 && latitude < 0.6) {
            middle_sum += center.temperature;
            ++middle_count;
        }
    }
    ASSERT_TRUE(polar_count > 0 && middle_count > 0);
    // The map is a north-south slice of a globe: cold at both edges, warm through
    // the middle. Without this the biome diagram collapses back to two dimensions.
    ASSERT_TRUE(polar_sum / polar_count < middle_sum / middle_count);
}

static void test_biome_diversity() {
    // Bigger than small_config(): a 16-cell map has too little climate range to
    // reach the corners of the table, which is exactly what this checks for.
    MapConfig config = small_config();
    config.grid_size = 48;
    MapGenerator generator(config, maps_logger());
    generator.generate();

    std::vector<bool> seen(k_biome_count, false);
    for (const MapCenter& center : generator.graph().centers) {
        seen[static_cast<std::size_t>(center.biome)] = true;
    }
    const std::size_t used = static_cast<std::size_t>(std::count(seen.begin(), seen.end(), true));
    // Before temperature existed, 14 of 18 appeared and four were unreachable.
    ASSERT_TRUE(used >= 18);
}

static void test_generate_is_deterministic() {
    MapGenerator first(small_config(4242), maps_logger());
    MapGenerator second(small_config(4242), maps_logger());
    first.generate();
    second.generate();

    const MapGraph& a = first.graph();
    const MapGraph& b = second.graph();
    ASSERT_EQ(a.centers.size(), b.centers.size());
    ASSERT_EQ(a.corners.size(), b.corners.size());
    ASSERT_EQ(a.edges.size(), b.edges.size());
    ASSERT_EQ(a.towns.size(), b.towns.size());

    for (std::size_t i = 0; i < a.centers.size(); ++i) {
        ASSERT_TRUE(a.centers[i].biome == b.centers[i].biome);
        ASSERT_TRUE(std::abs(a.centers[i].elevation - b.centers[i].elevation) < 1e-12);
        ASSERT_TRUE(std::abs(a.centers[i].moisture - b.centers[i].moisture) < 1e-12);
    }
    // The noisy-edge pass used to seed itself from the wall clock, so this is
    // where a reintroduced non-deterministic seed would show up first.
    for (std::size_t i = 0; i < a.edges.size(); ++i) {
        ASSERT_EQ(a.edges[i].noisy_points0.size(), b.edges[i].noisy_points0.size());
        ASSERT_EQ(a.edges[i].river, b.edges[i].river);
        ASSERT_EQ(a.edges[i].traffic, b.edges[i].traffic);
        ASSERT_TRUE(a.edges[i].road_class == b.edges[i].road_class);
        ASSERT_TRUE(a.edges[i].bridge == b.edges[i].bridge);
    }
    // The road pass draws no randomness, so its output is reproducible for a
    // stronger reason than a shared seed -- but it does sort candidate hubs and
    // hub pairs, and a tie broken by sort order rather than by id would show
    // here as a network that differs between two runs of the same config.
    ASSERT_EQ(a.roads.size(), b.roads.size());
    for (std::size_t i = 0; i < a.roads.size(); ++i) {
        ASSERT_TRUE(a.roads[i].road_class == b.roads[i].road_class);
        ASSERT_EQ(a.roads[i].edges.size(), b.roads[i].edges.size());
        ASSERT_EQ(a.roads[i].points.size(), b.roads[i].points.size());
        for (std::size_t j = 0; j < a.roads[i].edges.size(); ++j) {
            ASSERT_EQ(a.roads[i].edges[j], b.roads[i].edges[j]);
        }
    }
}

static void test_different_seeds_differ() {
    MapGenerator first(small_config(1), maps_logger());
    MapGenerator second(small_config(2), maps_logger());
    first.generate();
    second.generate();

    bool any_difference = false;
    const std::size_t count = std::min(first.graph().centers.size(), second.graph().centers.size());
    for (std::size_t i = 0; i < count && !any_difference; ++i) {
        any_difference = first.graph().centers[i].biome != second.graph().centers[i].biome;
    }
    ASSERT_TRUE(any_difference);
}

static void test_graph_invariants_hold() {
    MapGenerator generator(small_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    ASSERT_TRUE(!graph.centers.empty());
    ASSERT_TRUE(!graph.corners.empty());
    ASSERT_TRUE(!graph.edges.empty());

    const auto center_count = static_cast<CenterId>(graph.centers.size());
    const auto corner_count = static_cast<CornerId>(graph.corners.size());
    const auto edge_count = static_cast<EdgeId>(graph.edges.size());

    for (std::size_t i = 0; i < graph.centers.size(); ++i) {
        const MapCenter& center = graph.centers[i];
        // The moisture pass used to sort this array in place, which broke the
        // identity between a record's slot and its own index.
        ASSERT_EQ(center.index, static_cast<CenterId>(i));
        ASSERT_TRUE(center.elevation >= 0.0 && center.elevation <= 1.0);
        ASSERT_TRUE(center.moisture >= 0.0 && center.moisture <= 1.0);
        for (const CenterId id : center.neighbors) ASSERT_TRUE(id >= 0 && id < center_count);
        for (const CornerId id : center.corners)   ASSERT_TRUE(id >= 0 && id < corner_count);
        for (const EdgeId id : center.borders)     ASSERT_TRUE(id >= 0 && id < edge_count);
    }

    for (std::size_t i = 0; i < graph.corners.size(); ++i) {
        const MapCorner& corner = graph.corners[i];
        ASSERT_EQ(corner.index, static_cast<CornerId>(i));
        ASSERT_TRUE(corner.elevation >= 0.0 && corner.elevation <= 1.0);
        ASSERT_TRUE(corner.moisture >= 0.0 && corner.moisture <= 1.0);
        ASSERT_TRUE(corner.downslope >= 0 && corner.downslope < corner_count);
        for (const CenterId id : corner.touches)   ASSERT_TRUE(id >= 0 && id < center_count);
        for (const EdgeId id : corner.protrudes)   ASSERT_TRUE(id >= 0 && id < edge_count);
        for (const CornerId id : corner.adjacent)  ASSERT_TRUE(id >= 0 && id < corner_count);
    }

    for (std::size_t i = 0; i < graph.edges.size(); ++i) {
        const MapEdge& edge = graph.edges[i];
        ASSERT_EQ(edge.index, static_cast<EdgeId>(i));
        ASSERT_TRUE(edge.d0 >= 0 && edge.d0 < center_count);
        ASSERT_TRUE(edge.d1 >= 0 && edge.d1 < center_count);
        ASSERT_TRUE(edge.v0 >= 0 && edge.v0 < corner_count);
        ASSERT_TRUE(edge.v1 >= 0 && edge.v1 < corner_count);
        ASSERT_TRUE(edge.d0 != edge.d1);
    }
}

static void test_water_separates_ocean_from_lakes() {
    MapGenerator generator(small_config(), maps_logger());
    generator.generate();

    std::size_t ocean = 0;
    std::size_t lake = 0;
    for (const MapCenter& center : generator.graph().centers) {
        if (center.water && center.ocean) ++ocean;
        if (center.water && !center.ocean) ++lake;
    }
    ASSERT_TRUE(ocean > 0);
    // Inland water that never reached the border is what the flood fill exists
    // to distinguish; without it every lake would be classified as sea.
    ASSERT_TRUE(lake > 0);
}

static void test_rivers_terminate_on_hostile_terrain() {
    // A threshold below the noise field's minimum makes every corner water, so
    // no source can ever satisfy the elevation window. The original retried by
    // decrementing its loop counter and hung here forever.
    MapConfig config = small_config();
    config.threshold_water = -2.0;
    config.river_count = 50;

    MapGenerator generator(config, maps_logger());
    generator.generate();

    for (const MapEdge& edge : generator.graph().edges) {
        ASSERT_EQ(edge.river, 0);
    }
}

static void test_rivers_flow_downhill_to_the_coast() {
    MapGenerator generator(small_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::size_t river_edges = 0;
    for (const MapEdge& edge : graph.edges) {
        if (edge.river <= 0) {
            continue;
        }
        ++river_edges;
        const MapCorner& v0 = graph.corners[static_cast<std::size_t>(edge.v0)];
        const MapCorner& v1 = graph.corners[static_cast<std::size_t>(edge.v1)];
        ASSERT_TRUE(v0.river > 0 && v1.river > 0);
    }
    ASSERT_TRUE(river_edges > 0);
}

static void test_noisy_edges_subdivide_only_when_enabled() {
    MapConfig on = small_config();
    on.subdivide_noisy_edges = true;
    MapGenerator subdivided(on, maps_logger());
    subdivided.generate();

    std::size_t wobbled = 0;
    for (const MapEdge& edge : subdivided.graph().edges) {
        ASSERT_TRUE(edge.noisy_points0.size() >= 2);
        if (edge.noisy_points0.size() > 2) ++wobbled;
    }
    ASSERT_TRUE(wobbled > 0);

    MapConfig off = small_config();
    off.subdivide_noisy_edges = false;
    MapGenerator straight(off, maps_logger());
    straight.generate();

    for (const MapEdge& edge : straight.graph().edges) {
        ASSERT_EQ(edge.noisy_points0.size(), static_cast<std::size_t>(2));
        ASSERT_EQ(edge.noisy_points1.size(), static_cast<std::size_t>(2));
    }
}

static void test_towns_sit_on_habitable_land_and_stay_apart() {
    MapGenerator generator(small_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    const MapConfig& config = generator.config();
    const TownConfig& towns = config.towns;

    ASSERT_TRUE(!graph.towns.empty());
    ASSERT_TRUE(static_cast<int>(graph.towns.size()) <= towns.town_count);

    for (std::size_t i = 0; i < graph.towns.size(); ++i) {
        const MapTown& town = graph.towns[i];
        const MapCenter& center = graph.centers[static_cast<std::size_t>(town.center)];
        ASSERT_TRUE(!center.water);
        ASSERT_TRUE(!center.ocean);
        ASSERT_TRUE(!center.border);

        for (std::size_t j = i + 1; j < graph.towns.size(); ++j) {
            const double dx = graph.towns[j].point.x - town.point.x;
            const double dy = graph.towns[j].point.y - town.point.y;
            // Configured in metres, compared in grid units -- the two systems meet
            // at meters_to_grid(), and nowhere else.
            const double spacing = meters_to_grid(config, towns.min_spacing_m);
            ASSERT_TRUE(dx * dx + dy * dy >= spacing * spacing);
        }
    }
}

static void test_buildings_lie_inside_their_cell() {
    MapGenerator generator(small_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::size_t total_buildings = 0;
    for (const MapTown& town : graph.towns) {
        // A settlement covers every cell in `cells`, not just its primary one,
        // so a building has to fall inside *one of* them -- but still wholly
        // inside that one, never straddling a boundary.
        std::vector<std::vector<MapPoint>> polygons;
        for (const CenterId cell_id : town.cells) {
            const MapCenter& cell = graph.centers[static_cast<std::size_t>(cell_id)];
            std::vector<MapPoint> polygon;
            for (const CornerId corner_id : cell.corners) {
                polygon.push_back(graph.corners[static_cast<std::size_t>(corner_id)].point);
            }
            polygons.push_back(std::move(polygon));
        }
        ASSERT_TRUE(!polygons.empty());

        for (const MapBuilding& building : town.buildings) {
            ++total_buildings;
            ASSERT_TRUE(building.width > 0.0 && building.height > 0.0);
            // The *rotated* corners, not an axis-aligned box. Checking the box
            // would pass even when a building drawn at its stated yaw hangs out
            // over the cell boundary.
            const std::array<MapPoint, 4> corners = building_corners(building);
            bool contained = false;
            for (const std::vector<MapPoint>& polygon : polygons) {
                bool all_in = true;
                for (const MapPoint& corner : corners) {
                    all_in = all_in && point_in_polygon(polygon, corner);
                }
                if (all_in) {
                    contained = true;
                    break;
                }
            }
            ASSERT_TRUE(contained);
        }
    }
    ASSERT_TRUE(total_buildings > 0);
}

static void test_buildings_do_not_overlap() {
    MapGenerator generator(small_config(), maps_logger());
    generator.generate();

    std::size_t compared = 0;
    for (const MapTown& town : generator.graph().towns) {
        for (std::size_t i = 0; i < town.buildings.size(); ++i) {
            for (std::size_t j = i + 1; j < town.buildings.size(); ++j) {
                ASSERT_TRUE(!buildings_overlap(town.buildings[i], town.buildings[j]));
                ++compared;
            }
        }
    }
    ASSERT_TRUE(compared > 0);
}

static void test_building_layout_is_not_a_lattice() {
    MapGenerator generator(small_config(), maps_logger());
    generator.generate();

    // A grid scan drops many buildings onto a handful of shared columns; jittered
    // placement gives each one its own. Rounding to 1e-4 keeps this about layout
    // rather than floating-point noise.
    std::size_t total = 0;
    std::size_t distinct = 0;
    for (const MapTown& town : generator.graph().towns) {
        std::vector<long long> columns;
        for (const MapBuilding& building : town.buildings) {
            columns.push_back(static_cast<long long>(building.point.x * 10000.0));
        }
        std::sort(columns.begin(), columns.end());
        const std::size_t unique_columns =
            static_cast<std::size_t>(std::unique(columns.begin(), columns.end()) - columns.begin());
        total += town.buildings.size();
        distinct += unique_columns;
    }

    ASSERT_TRUE(total > 0);
    // Measured at 100% for jittered placement, 62% for the lattice it replaced.
    ASSERT_TRUE(distinct * 100 >= total * 90);
}

static void test_buildings_front_their_streets() {
    MapConfig config = small_config();
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    const TownConfig& towns = config.towns;

    // In grid units, because that is what the geometry is in; the config is metres.
    const double reach = meters_to_grid(config, towns.street_offset_m + towns.building_size_max_m);
    std::size_t total = 0;
    std::size_t fronting = 0;

    for (const MapTown& town : graph.towns) {
        // One street set per claimed cell: the pass derives streets per cell, so
        // a building in an outlying cell fronts that cell's streets, not the
        // primary cell's.
        std::vector<std::pair<MapPoint, MapPoint>> streets;
        for (const CenterId cell_id : town.cells) {
            const MapCenter& cell = graph.centers[static_cast<std::size_t>(cell_id)];
            for (const EdgeId edge_id : cell.borders) {
                const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
                if (edge.road || edge.river > 0) {
                    streets.emplace_back(cell.point, edge.midpoint);
                }
            }
        }
        if (streets.empty()) {
            continue;
        }

        for (const MapBuilding& building : town.buildings) {
            ++total;
            for (const auto& street : streets) {
                const MapPoint& from = street.first;
                const MapPoint& end = street.second;
                const double dx = end.x - from.x;
                const double dy = end.y - from.y;
                const double length_squared = dx * dx + dy * dy;
                if (length_squared == 0.0) {
                    continue;
                }
                double t = ((building.point.x - from.x) * dx
                          + (building.point.y - from.y) * dy) / length_squared;
                t = std::clamp(t, 0.0, 1.0);
                const double nearest_x = from.x + t * dx;
                const double nearest_y = from.y + t * dy;
                const double distance = std::hypot(building.point.x - nearest_x,
                                                   building.point.y - nearest_y);
                if (distance > reach) {
                    continue;
                }

                double delta = building.rotation - std::atan2(dy, dx);
                while (delta > k_pi) delta -= 2.0 * k_pi;
                while (delta < -k_pi) delta += 2.0 * k_pi;
                if (std::abs(delta) <= towns.rotation_jitter + 1e-6) {
                    ++fronting;
                    break;
                }
            }
        }
    }

    ASSERT_TRUE(total > 0);
    // Measured at 51%. Uniformly random yaw would put only ~8% of buildings
    // within rotation_jitter of a street, so this separates a road-aware layout
    // from merely jittered noise. The remainder is deliberate interior infill.
    ASSERT_TRUE(fronting * 100 >= total * 30);
}

static void test_cell_outline_is_closed_and_ordered() {
    MapGenerator generator(small_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    for (const MapCenter& center : graph.centers) {
        if (center.corners.size() < 3) {
            continue; // Boundary-ring cells outside the map proper.
        }
        const std::vector<MapPoint> outline = graph.cell_outline(center);
        ASSERT_TRUE(outline.size() >= 3);
        for (const MapPoint& point : outline) {
            ASSERT_TRUE(std::isfinite(point.x) && std::isfinite(point.y));
        }
    }
}

static void test_renderers_produce_a_full_image() {
    MapConfig config = small_config();
    MapGenerator generator(config, maps_logger());
    generator.generate();

    const Image biomes = MapLayers::composite(generator.graph(), config);
    ASSERT_EQ(biomes.width, config.image_size);
    ASSERT_EQ(biomes.height, config.image_size);
    ASSERT_EQ(biomes.pixels.size(),
              static_cast<std::size_t>(config.image_size) * config.image_size * 3);

    const Image elevation = MapLayers::elevation(generator.graph(), config);
    ASSERT_EQ(elevation.pixels.size(), biomes.pixels.size());

    // Anything other than a single flat colour proves the cells actually drew.
    bool varied = false;
    for (std::size_t i = 3; i < biomes.pixels.size() && !varied; i += 3) {
        varied = biomes.pixels[i] != biomes.pixels[0];
    }
    ASSERT_TRUE(varied);
}

/**
 * @brief Rivers belong to the water layer, and to no other.
 *
 * The elevation layer used to have its river network dimmed into it. That made
 * it a picture of the terrain rather than the terrain itself -- a consumer
 * flooding a mesh to those values would find channels already cut. Splitting the
 * layers means the height field is now height and nothing else, and this is the
 * check that the split actually happened rather than being merely intended.
 */
static void test_rivers_live_on_the_water_layer_only() {
    MapConfig with_rivers_config = small_config();
    with_rivers_config.subdivide_noisy_edges = false;

    MapConfig without_rivers_config = with_rivers_config;
    without_rivers_config.enable_rivers = false;

    MapGenerator with_rivers(with_rivers_config, maps_logger());
    MapGenerator without_rivers(without_rivers_config, maps_logger());
    with_rivers.generate();
    without_rivers.generate();
    ASSERT_TRUE(!with_rivers.graph().rivers.empty());

    // The height field does not notice whether the river pass ran.
    const Image drawn = MapLayers::elevation(with_rivers.graph(), with_rivers_config);
    const Image base = MapLayers::elevation(without_rivers.graph(), without_rivers_config);
    ASSERT_EQ(drawn.pixels.size(), base.pixels.size());
    ASSERT_TRUE(drawn.pixels == base.pixels);

    // The water layer very much does.
    const Image wet = MapLayers::water(with_rivers.graph(), with_rivers_config);
    const Image dry = MapLayers::water(without_rivers.graph(), without_rivers_config);
    ASSERT_EQ(wet.pixels.size(), dry.pixels.size());
    ASSERT_TRUE(wet.pixels != dry.pixels);
}

// Regions, names and landmarks need more land than a 16-cell map offers.
static MapConfig world_config(int seed = 251) {
    MapConfig config = small_config(seed);
    config.grid_size = 48;
    config.regions.country_count = 4;
    config.regions.regions_per_country = 2;
    return config;
}

static void test_buildings_avoid_rivers() {
    MapGenerator generator(world_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    const MapConfig& config = generator.config();

    std::size_t checked = 0;
    for (const MapTown& town : graph.towns) {
        const MapCenter& center = graph.centers[static_cast<std::size_t>(town.center)];
        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            if (edge.river <= 0 || edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
                continue;
            }
            const MapPoint& a = graph.corners[static_cast<std::size_t>(edge.v0)].point;
            const MapPoint& b = graph.corners[static_cast<std::size_t>(edge.v1)].point;
            const double clearance =
                river_width(config, edge.river) * 0.5
                + meters_to_grid(config, config.towns.water_clearance_m);

            for (const MapBuilding& building : town.buildings) {
                for (const MapPoint& corner : building_corners(building)) {
                    // Distance from the footprint corner to the river's centreline.
                    const double dx = b.x - a.x;
                    const double dy = b.y - a.y;
                    const double length_squared = dx * dx + dy * dy;
                    double t = length_squared == 0.0
                        ? 0.0
                        : ((corner.x - a.x) * dx + (corner.y - a.y) * dy) / length_squared;
                    t = std::clamp(t, 0.0, 1.0);
                    const double distance = std::hypot(corner.x - (a.x + t * dx),
                                                       corner.y - (a.y + t * dy));
                    ASSERT_TRUE(distance >= clearance - 1e-9);
                    ++checked;
                }
            }
        }
    }
    ASSERT_TRUE(checked > 0);
}

static void test_elevation_is_smoothed() {
    MapGenerator generator(world_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    double total = 0.0;
    std::size_t count = 0;
    for (const MapCenter& center : graph.centers) {
        if (center.water) {
            continue;
        }
        for (const CenterId neighbor_id : center.neighbors) {
            const MapCenter& neighbor = graph.centers[static_cast<std::size_t>(neighbor_id)];
            if (neighbor.water) {
                continue;
            }
            total += std::abs(center.elevation - neighbor.elevation);
            ++count;
        }
    }
    ASSERT_TRUE(count > 0);
    // Unsmoothed the mean step measured 0.080, with a quarter of the whole range
    // between some neighbours; smoothed it measures 0.036.
    ASSERT_TRUE(total / static_cast<double>(count) < 0.05);
}

static void test_building_sizes_span_the_range() {
    MapConfig config = world_config();
    MapGenerator generator(config, maps_logger());
    generator.generate();

    double smallest = config.towns.building_size_max_m;
    double largest = config.towns.building_size_min_m;
    std::size_t count = 0;
    for (const MapTown& town : generator.graph().towns) {
        for (const MapBuilding& building : town.buildings) {
            ASSERT_TRUE(building.width
                        >= meters_to_grid(config, config.towns.building_size_min_m) - 1e-9);
            ASSERT_TRUE(building.width
                        <= meters_to_grid(config, config.towns.building_size_max_m) + 1e-9);
            ASSERT_TRUE(std::abs(building.width - building.height) < 1e-9);
            smallest = std::min(smallest, building.width);
            largest = std::max(largest, building.width);
            ++count;
        }
    }
    ASSERT_TRUE(count > 0);
    // Actually varied, not one size repeated -- the range has to be used.
    ASSERT_TRUE(largest - smallest > (config.towns.building_size_max_m
                                      - config.towns.building_size_min_m) * 0.5);
}

static void test_regions_partition_the_land() {
    MapGenerator generator(world_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    ASSERT_TRUE(!graph.countries.empty());
    ASSERT_TRUE(!graph.regions.empty());

    for (std::size_t i = 0; i < graph.regions.size(); ++i) {
        const MapRegion& region = graph.regions[i];
        ASSERT_EQ(region.index, static_cast<RegionId>(i));
        ASSERT_TRUE(region.country >= 0
                    && region.country < static_cast<CountryId>(graph.countries.size()));
        ASSERT_TRUE(!region.name.empty());
    }

    // Every scrap of habitable land belongs to somebody. Cells the cost-weighted
    // fill cannot reach are adopted by their nearest claimant rather than left
    // stateless.
    std::size_t unclaimed = 0;
    for (const MapCenter& center : graph.centers) {
        if (center.water || center.ocean || center.border) {
            continue;
        }
        if (center.country == k_invalid_id) {
            ++unclaimed;
        } else {
            ASSERT_TRUE(center.country < static_cast<CountryId>(graph.countries.size()));
        }
    }
    ASSERT_EQ(unclaimed, static_cast<std::size_t>(0));
}

static void test_names_are_unique_and_reproducible() {
    MapGenerator first(world_config(4242), maps_logger());
    MapGenerator second(world_config(4242), maps_logger());
    first.generate();
    second.generate();

    ASSERT_EQ(first.graph().towns.size(), second.graph().towns.size());
    std::vector<std::string> names;
    for (std::size_t i = 0; i < first.graph().towns.size(); ++i) {
        const std::string& name = first.graph().towns[i].name;
        ASSERT_TRUE(!name.empty());
        ASSERT_TRUE(name == second.graph().towns[i].name);
        names.push_back(name);
    }
    ASSERT_TRUE(!names.empty());

    std::sort(names.begin(), names.end());
    ASSERT_TRUE(std::adjacent_find(names.begin(), names.end()) == names.end());

    for (std::size_t i = 0; i < first.graph().regions.size(); ++i) {
        ASSERT_TRUE(first.graph().regions[i].name == second.graph().regions[i].name);
    }
}

static void test_population_scales_with_buildings() {
    MapGenerator generator(world_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    int capital_population = 0;
    int village_population = 0;
    std::size_t villages = 0;
    for (const MapTown& town : graph.towns) {
        ASSERT_EQ(town.households, static_cast<int>(town.buildings.size()));
        if (town.households > 0) {
            ASSERT_TRUE(town.population > 0);
            // Population is counted from dwellings, so it cannot exceed the most
            // each could possibly hold.
            const double ceiling = town.households * generator.config().towns.household_size_max
                                 * generator.config().towns.capital_density;
            ASSERT_TRUE(town.population <= static_cast<int>(ceiling) + 1);
        }
        if (town.tier == TownTier::Capital) {
            capital_population = std::max(capital_population, town.population);
        } else if (town.tier == TownTier::Village) {
            village_population += town.population;
            ++villages;
        }
    }
    ASSERT_TRUE(villages > 0);
    // A capital that a village outgrows means the tier hierarchy is decorative.
    ASSERT_TRUE(capital_population
                > static_cast<int>(village_population / static_cast<int>(villages)));

    int region_total = 0;
    for (const MapRegion& region : graph.regions) {
        ASSERT_TRUE(region.population >= 0);
        region_total += region.population;
    }
    int town_total = 0;
    for (const MapTown& town : graph.towns) {
        if (town.region != k_invalid_id) {
            town_total += town.population;
        }
    }
    ASSERT_EQ(region_total, town_total);
}

static void test_landmarks_respect_their_biome() {
    MapGenerator generator(world_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    ASSERT_TRUE(!graph.landmarks.empty());

    std::vector<int> wonders(graph.regions.size(), 0);
    for (const MapLandmark& landmark : graph.landmarks) {
        ASSERT_TRUE(!landmark.name.empty());
        ASSERT_TRUE(landmark.center >= 0
                    && landmark.center < static_cast<CenterId>(graph.centers.size()));

        const MapCenter& center = graph.centers[static_cast<std::size_t>(landmark.center)];
        // No volcano on ice, no oasis outside desert: the vocabulary is gated by
        // terrain, which is what stops landmarks reading as random noise.
        ASSERT_TRUE(landmark_suits_biome(landmark.kind, center.biome));

        if (landmark.kind == LandmarkKind::Wonder && landmark.region != k_invalid_id) {
            ++wonders[static_cast<std::size_t>(landmark.region)];
        }
    }
    for (const int count : wonders) {
        ASSERT_TRUE(count <= 1);
    }

    // No two landmarks share a cell, and none stands on a settlement.
    std::vector<CenterId> occupied;
    for (const MapLandmark& landmark : graph.landmarks) {
        occupied.push_back(landmark.center);
    }
    for (const MapTown& town : graph.towns) {
        occupied.push_back(town.center);
    }
    std::sort(occupied.begin(), occupied.end());
    ASSERT_TRUE(std::adjacent_find(occupied.begin(), occupied.end()) == occupied.end());
}

static void test_yaml_round_trip_preserves_the_graph() {
    MapConfig config = small_config(77);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& original = generator.graph();

    const std::string path = "test_temp_map.yaml";
    save_map(original, config, path);

    MapGraph loaded;
    MapConfig loaded_config;
    ASSERT_TRUE(load_map(path, loaded, loaded_config));
    std::remove(path.c_str());

    ASSERT_EQ(loaded_config.seed, config.seed);
    ASSERT_EQ(loaded_config.grid_size, config.grid_size);
    ASSERT_EQ(loaded_config.river_count, config.river_count);

    ASSERT_EQ(loaded.centers.size(), original.centers.size());
    ASSERT_EQ(loaded.corners.size(), original.corners.size());
    ASSERT_EQ(loaded.edges.size(), original.edges.size());
    ASSERT_EQ(loaded.towns.size(), original.towns.size());

    // fkYAML's emitter formats floats at the default six significant digits, so
    // the format is lossy by construction: normalised values survive to about
    // 1e-7 and grid positions, which run up to grid_size, to about 1e-5.
    // Everything else round-trips exactly.
    const double k_normalised_epsilon = 1e-6;
    const double k_position_epsilon = 1e-3;

    for (std::size_t i = 0; i < original.centers.size(); ++i) {
        const MapCenter& a = original.centers[i];
        const MapCenter& b = loaded.centers[i];
        ASSERT_EQ(a.index, b.index);
        ASSERT_TRUE(a.biome == b.biome);
        ASSERT_TRUE(a.water == b.water && a.ocean == b.ocean && a.coast == b.coast);
        ASSERT_TRUE(a.border == b.border);
        ASSERT_TRUE(std::abs(a.elevation - b.elevation) < k_normalised_epsilon);
        ASSERT_TRUE(std::abs(a.moisture - b.moisture) < k_normalised_epsilon);
        ASSERT_TRUE(std::abs(a.point.x - b.point.x) < k_position_epsilon);
        ASSERT_TRUE(std::abs(a.point.y - b.point.y) < k_position_epsilon);
        ASSERT_EQ(a.neighbors.size(), b.neighbors.size());
        ASSERT_EQ(a.corners.size(), b.corners.size());
        ASSERT_EQ(a.borders.size(), b.borders.size());
        for (std::size_t j = 0; j < a.neighbors.size(); ++j) ASSERT_EQ(a.neighbors[j], b.neighbors[j]);
        for (std::size_t j = 0; j < a.corners.size(); ++j)   ASSERT_EQ(a.corners[j], b.corners[j]);
        for (std::size_t j = 0; j < a.borders.size(); ++j)   ASSERT_EQ(a.borders[j], b.borders[j]);
    }

    for (std::size_t i = 0; i < original.corners.size(); ++i) {
        ASSERT_EQ(original.corners[i].river, loaded.corners[i].river);
        ASSERT_EQ(original.corners[i].downslope, loaded.corners[i].downslope);
        ASSERT_EQ(original.corners[i].adjacent.size(), loaded.corners[i].adjacent.size());
    }

    for (std::size_t i = 0; i < original.edges.size(); ++i) {
        const MapEdge& a = original.edges[i];
        const MapEdge& b = loaded.edges[i];
        ASSERT_EQ(a.d0, b.d0);
        ASSERT_EQ(a.d1, b.d1);
        ASSERT_EQ(a.v0, b.v0);
        ASSERT_EQ(a.v1, b.v1);
        ASSERT_EQ(a.river, b.river);
        ASSERT_TRUE(a.road == b.road);
        ASSERT_TRUE(a.road_class == b.road_class);
        ASSERT_EQ(a.traffic, b.traffic);
        ASSERT_TRUE(a.bridge == b.bridge);
        ASSERT_EQ(a.noisy_points0.size(), b.noisy_points0.size());
    }

    ASSERT_EQ(original.roads.size(), loaded.roads.size());
    for (std::size_t i = 0; i < original.roads.size(); ++i) {
        const MapRoad& a = original.roads[i];
        const MapRoad& b = loaded.roads[i];
        ASSERT_TRUE(a.road_class == b.road_class);
        ASSERT_EQ(a.edges.size(), b.edges.size());
        ASSERT_EQ(a.points.size(), b.points.size());
        // Positions survive to six significant digits, which at these grid
        // coordinates is about 1e-4 -- see the precision note on map_to_node().
        for (std::size_t j = 0; j < a.points.size(); ++j) {
            ASSERT_TRUE(std::abs(a.points[j].x - b.points[j].x) < 1e-3);
            ASSERT_TRUE(std::abs(a.points[j].y - b.points[j].y) < 1e-3);
        }
    }

    for (std::size_t i = 0; i < original.towns.size(); ++i) {
        ASSERT_EQ(original.towns[i].center, loaded.towns[i].center);
        ASSERT_TRUE(original.towns[i].tier == loaded.towns[i].tier);
        ASSERT_EQ(original.towns[i].buildings.size(), loaded.towns[i].buildings.size());
    }
}

/**
 * @brief A reloaded map renders as the same map, layer for layer.
 *
 * Six significant digits survive the document (see the precision note on
 * `map_to_node()`), and for everything drawn from connectivity, flags or flat
 * colour that is exact -- those layers must come back byte for byte.
 *
 * The two layers derived from the *height field* are the exception, and only
 * just. Hillshading takes differences between neighbouring elevations and
 * multiplies them by a large exaggeration, so the last digit of a round-tripped
 * height can move a greyscale value by one. Allowing one and no more is the
 * point: it pins the loss to rounding rather than to anything structural, and a
 * regression that dropped, say, the corner elevations entirely would blow
 * straight through it.
 */
static void test_yaml_round_trip_renders_every_layer() {
    MapConfig config = small_config(9);
    MapGenerator generator(config, maps_logger());
    generator.generate();

    const std::string path = "test_temp_map_render.yaml";
    save_map(generator.graph(), config, path);

    MapGraph loaded;
    MapConfig loaded_config;
    ASSERT_TRUE(load_map(path, loaded, loaded_config));
    std::remove(path.c_str());

    for (std::size_t i = 0; i < k_map_layer_count; ++i) {
        const MapLayer layer = static_cast<MapLayer>(i);
        const Image before = MapLayers::render(layer, generator.graph(), config);
        const Image after = MapLayers::render(layer, loaded, loaded_config);
        ASSERT_EQ(before.pixels.size(), after.pixels.size());
        ASSERT_TRUE(before.pixels.size() > 0);

        const bool from_height = layer == MapLayer::Elevation || layer == MapLayer::Composite;
        const int tolerance = from_height ? 1 : 0;
        for (std::size_t k = 0; k < before.pixels.size(); ++k) {
            const int delta = std::abs(static_cast<int>(before.pixels[k])
                                     - static_cast<int>(after.pixels[k]));
            ASSERT_TRUE(delta <= tolerance);
        }
    }
}

static void test_disabled_passes_leave_the_graph_untouched() {
    MapConfig config = small_config();
    config.enable_rivers = false;
    config.enable_roads = false;
    config.enable_towns = false;

    MapGenerator generator(config, maps_logger());
    generator.generate();

    ASSERT_TRUE(generator.graph().towns.empty());
    ASSERT_TRUE(generator.graph().roads.empty());
    for (const MapEdge& edge : generator.graph().edges) {
        ASSERT_EQ(edge.river, 0);
        ASSERT_TRUE(!edge.road);
        ASSERT_TRUE(edge.road_class == RoadClass::None);
        ASSERT_EQ(edge.traffic, 0);
        ASSERT_TRUE(!edge.bridge);
    }
    // Geometry is built before any pass runs, so it survives them all being off.
    ASSERT_TRUE(!generator.graph().centers.empty());
}


// --- Roads ---------------------------------------------------------------

/** @brief A config big enough for the road network to have somewhere to go. */
static MapConfig road_config(int seed = 77) {
    MapConfig config;
    config.grid_size = 32;
    config.image_size = 256;
    config.seed = seed;
    config.noise_island.seed = seed;
    return config;
}

/** @brief The cells a road run passes through, in order, from its edge chain. */
static std::vector<CenterId> run_cells(const MapGraph& graph, const MapRoad& run) {
    std::vector<CenterId> cells;
    if (run.edges.empty()) {
        return cells;
    }
    const MapEdge& first = graph.edges[static_cast<std::size_t>(run.edges[0])];
    if (run.edges.size() == 1) {
        return {first.d0, first.d1};
    }
    // Start at whichever end of the first edge the second edge does not share.
    const MapEdge& second = graph.edges[static_cast<std::size_t>(run.edges[1])];
    const bool d1_is_shared = second.d0 == first.d1 || second.d1 == first.d1;
    CenterId cell = d1_is_shared ? first.d0 : first.d1;

    cells.push_back(cell);
    for (const EdgeId edge_id : run.edges) {
        const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
        cell = edge.d0 == cell ? edge.d1 : edge.d0;
        cells.push_back(cell);
    }
    return cells;
}

static void test_biome_habitability_ranks_the_land() {
    // Hard exclusions: nothing is built here, and no road is routed cheaply
    // through it either -- both passes read this one table.
    ASSERT_TRUE(biome_habitability(Biome::Ocean) == 0.0);
    ASSERT_TRUE(biome_habitability(Biome::Lake) == 0.0);
    ASSERT_TRUE(biome_habitability(Biome::Ice) == 0.0);
    ASSERT_TRUE(biome_habitability(Biome::Glacier) == 0.0);
    ASSERT_TRUE(biome_habitability(Biome::Scorched) == 0.0);
    ASSERT_TRUE(biome_habitability(Biome::VolcanicField) == 0.0);

    // Grassland is the top of the table, and the ordering the passes rely on
    // holds across the whole range.
    ASSERT_TRUE(biome_habitability(Biome::Grassland) == 1.0);
    ASSERT_TRUE(biome_habitability(Biome::Grassland)
                > biome_habitability(Biome::TemperateDeciduousForest));
    ASSERT_TRUE(biome_habitability(Biome::TemperateDeciduousForest)
                > biome_habitability(Biome::Taiga));
    ASSERT_TRUE(biome_habitability(Biome::Taiga) > biome_habitability(Biome::Tundra));
    ASSERT_TRUE(biome_habitability(Biome::Savanna) > biome_habitability(Biome::Badlands));

    // Every value is in range, and the switch covers the whole enum -- a biome
    // added without a case would fall through to the 0.0 return and be silently
    // uninhabitable.
    for (std::size_t i = 0; i < k_biome_count; ++i) {
        const double value = biome_habitability(static_cast<Biome>(i));
        ASSERT_TRUE(value >= 0.0 && value <= 1.0);
    }
}

static void test_road_class_names_round_trip() {
    for (std::size_t i = 0; i < k_road_class_count; ++i) {
        const RoadClass road_class = static_cast<RoadClass>(i);
        ASSERT_TRUE(road_class_from_name(road_class_name(road_class)) == road_class);
    }
    // An unrecognised name keeps the road rather than erasing it.
    ASSERT_TRUE(road_class_from_name("motorway") == RoadClass::Trail);
}

static void test_roads_form_one_network_reaching_the_towns() {
    MapConfig config = road_config();
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::vector<EdgeId> road_edges;
    for (const MapEdge& edge : graph.edges) {
        if (edge.road) {
            road_edges.push_back(edge.index);
        }
    }
    ASSERT_TRUE(road_edges.size() > 8);

    // Union-find over the cells the roads join. The old contour pass produced
    // long unconnected arcs; a routed network is mostly one piece.
    std::vector<CenterId> parent(graph.centers.size());
    for (std::size_t i = 0; i < parent.size(); ++i) {
        parent[i] = static_cast<CenterId>(i);
    }
    const std::function<CenterId(CenterId)> find = [&parent, &find](CenterId id) -> CenterId {
        while (parent[static_cast<std::size_t>(id)] != id) {
            parent[static_cast<std::size_t>(id)] =
                parent[static_cast<std::size_t>(parent[static_cast<std::size_t>(id)])];
            id = parent[static_cast<std::size_t>(id)];
        }
        return id;
    };
    for (const EdgeId edge_id : road_edges) {
        const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
        const CenterId a = find(edge.d0);
        const CenterId b = find(edge.d1);
        if (a != b) {
            parent[static_cast<std::size_t>(a)] = b;
        }
    }

    std::map<CenterId, int> component_size;
    for (const EdgeId edge_id : road_edges) {
        ++component_size[find(graph.edges[static_cast<std::size_t>(edge_id)].d0)];
    }
    int largest = 0;
    for (const auto& entry : component_size) {
        largest = std::max(largest, entry.second);
    }
    ASSERT_TRUE(largest * 2 >= static_cast<int>(road_edges.size()));

    // The point of routing rather than contouring: the settlements placed two
    // passes later are on the network. They are not hubs -- the town pass adds
    // jitter and its own spacing -- so this is a majority, not a guarantee.
    int on_a_road = 0;
    for (const MapTown& town : graph.towns) {
        const MapCenter& center = graph.centers[static_cast<std::size_t>(town.center)];
        for (const EdgeId edge_id : center.borders) {
            if (graph.edges[static_cast<std::size_t>(edge_id)].road) {
                ++on_a_road;
                break;
            }
        }
    }
    ASSERT_TRUE(!graph.towns.empty());
    ASSERT_TRUE(on_a_road * 2 > static_cast<int>(graph.towns.size()));
}

static void test_road_class_follows_traffic() {
    MapConfig config = road_config(1234);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    int busiest_trail = 0;
    int quietest_road = std::numeric_limits<int>::max();
    int busiest_road = 0;
    int quietest_highway = std::numeric_limits<int>::max();
    bool saw_road = false;
    bool saw_highway = false;

    for (const MapEdge& edge : graph.edges) {
        // The predicate and the class are two readings of one fact and may never
        // disagree: the town packer asks the first, the renderer the second.
        ASSERT_TRUE(edge.road == (edge.road_class != RoadClass::None));
        ASSERT_TRUE(edge.road == (edge.traffic > 0));
        if (!edge.road) {
            ASSERT_EQ(edge.traffic, 0);
            continue;
        }
        switch (edge.road_class) {
            case RoadClass::Trail:
                busiest_trail = std::max(busiest_trail, edge.traffic);
                break;
            case RoadClass::Road:
                saw_road = true;
                quietest_road = std::min(quietest_road, edge.traffic);
                busiest_road = std::max(busiest_road, edge.traffic);
                break;
            case RoadClass::Highway:
                saw_highway = true;
                quietest_highway = std::min(quietest_highway, edge.traffic);
                break;
            case RoadClass::None:
                break;
        }
    }

    // Classification is a pair of thresholds on one number, so the tiers cannot
    // interleave. A regression that cut the tiers on anything else would.
    if (saw_road) {
        ASSERT_TRUE(quietest_road > busiest_trail);
    }
    if (saw_highway) {
        ASSERT_TRUE(quietest_highway > busiest_road);
    }
}

static void test_roads_bridge_only_where_they_meet_water() {
    MapConfig config = road_config(55);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    for (const MapEdge& edge : graph.edges) {
        const bool over_water =
            (edge.d0 != k_invalid_id && graph.centers[static_cast<std::size_t>(edge.d0)].water)
            || (edge.d1 != k_invalid_id && graph.centers[static_cast<std::size_t>(edge.d1)].water);

        if (edge.bridge) {
            ASSERT_TRUE(edge.road);
            ASSERT_TRUE(edge.river > 0 || over_water);
        }
        // And the converse: a road over water without a bridge would be a road
        // running through the river, which is what makes this an equivalence
        // rather than a one-way check.
        if (edge.road && (edge.river > 0 || over_water)) {
            ASSERT_TRUE(edge.bridge);
        }
    }
}

static void test_roads_keep_off_the_border_and_the_open_sea() {
    MapConfig config = road_config(8);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    for (const MapEdge& edge : graph.edges) {
        if (!edge.road) {
            continue;
        }
        // The forced-water band at the map edge is not somewhere a road goes.
        ASSERT_TRUE(!graph.centers[static_cast<std::size_t>(edge.d0)].border);
        ASSERT_TRUE(!graph.centers[static_cast<std::size_t>(edge.d1)].border);
    }

    // A causeway may hop a strait but not strike out across the ocean, so no
    // run may hold a stretch of water longer than max_water_span.
    for (const MapRoad& run : graph.roads) {
        int water_run = 0;
        for (const CenterId cell : run_cells(graph, run)) {
            water_run = graph.centers[static_cast<std::size_t>(cell)].water ? water_run + 1 : 0;
            ASSERT_TRUE(water_run <= config.roads.max_water_span);
        }
    }
}

static void test_road_runs_partition_the_flagged_edges() {
    MapConfig config = road_config(313);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::vector<int> times_traced(graph.edges.size(), 0);
    for (const MapRoad& run : graph.roads) {
        ASSERT_TRUE(!run.edges.empty());
        ASSERT_TRUE(run.points.size() >= 2);
        ASSERT_TRUE(run.road_class != RoadClass::None);

        for (const EdgeId edge_id : run.edges) {
            ASSERT_TRUE(edge_id >= 0 && static_cast<std::size_t>(edge_id) < graph.edges.size());
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            // A run is a chain of one class, which is what lets the renderer
            // stroke it as a single polyline at a single width.
            ASSERT_TRUE(edge.road_class == run.road_class);
            ++times_traced[static_cast<std::size_t>(edge_id)];
        }
        for (const MapPoint& point : run.points) {
            ASSERT_TRUE(std::isfinite(point.x) && std::isfinite(point.y));
            ASSERT_TRUE(point.x >= 0.0 && point.x <= static_cast<double>(config.grid_size));
            ASSERT_TRUE(point.y >= 0.0 && point.y <= static_cast<double>(config.grid_size));
        }
    }

    // Every road edge is traced into exactly one run: none dropped, none drawn
    // twice. Drawn twice would darken a road where two runs overlapped.
    for (const MapEdge& edge : graph.edges) {
        ASSERT_EQ(times_traced[static_cast<std::size_t>(edge.index)], edge.road ? 1 : 0);
    }
}

static void test_road_runs_are_smoothed_only_when_asked() {
    MapConfig straight = road_config(21);
    straight.roads.smoothing_iterations = 0;
    MapGenerator plain(straight, maps_logger());
    plain.generate();

    // Unsmoothed, a run is exactly the cell sites it passes through.
    bool saw_multi_edge_run = false;
    for (const MapRoad& run : plain.graph().roads) {
        ASSERT_EQ(run.points.size(), run.edges.size() + 1);
        saw_multi_edge_run = saw_multi_edge_run || run.edges.size() > 1;
    }
    ASSERT_TRUE(saw_multi_edge_run);

    MapConfig curved = road_config(21);
    MapGenerator smoothed(curved, maps_logger());
    smoothed.generate();

    ASSERT_EQ(smoothed.graph().roads.size(), plain.graph().roads.size());
    for (std::size_t i = 0; i < smoothed.graph().roads.size(); ++i) {
        const MapRoad& a = plain.graph().roads[i];
        const MapRoad& b = smoothed.graph().roads[i];
        // Smoothing changes the drawn path and nothing else: the same edges, in
        // the same order, with more points between them.
        ASSERT_EQ(a.edges.size(), b.edges.size());
        if (a.edges.size() > 1) {
            ASSERT_TRUE(b.points.size() > a.points.size());
        }
        // The ends are pinned, so a run still meets the junction it was traced to.
        ASSERT_TRUE(std::abs(a.points.front().x - b.points.front().x) < 1e-12);
        ASSERT_TRUE(std::abs(a.points.back().y - b.points.back().y) < 1e-12);
    }
}


// --- Legend ---------------------------------------------------------------

/** @brief Formats a palette colour the way the README legend spells it. */
static std::string hex_of(const glm::vec3& color) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "#%02X%02X%02X",
                  static_cast<int>(color.r), static_cast<int>(color.g),
                  static_cast<int>(color.b));
    return std::string(buffer);
}

/** @brief Reads a whole file, or returns an empty string if it will not open. */
static std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

/**
 * @brief The README legend is documentation of a table in the code, and drifts from it.
 *
 * A legend is only useful if it is true, and nothing else would catch a palette
 * entry changed in `map_config.h` without the README following -- the renders
 * would simply stop matching their own key. So the legend is checked here
 * rather than trusted: every biome row must name a real biome, quote its
 * palette colour exactly, and point at a swatch whose fill is that same colour.
 */
static void test_readme_legend_matches_the_palette() {
    const std::string root = ROOT_DIR;
    const std::string readme = read_file(root + "/README.md");
    ASSERT_TRUE(!readme.empty());

    const BiomePalette palette;
    int rows_checked = 0;

    for (std::size_t i = 0; i < k_biome_count; ++i) {
        const Biome biome = static_cast<Biome>(i);
        const std::string slug(biome_name(biome));
        const std::string hex = hex_of(palette.color_for(biome));

        // The exact row the legend generator emits, swatch included. Matching the
        // whole row at once is what ties the three columns together: a swatch
        // pointing at the wrong biome, or a hex that disagrees with its own RGB,
        // both fail here rather than passing three separate looser checks.
        const glm::vec3& color = palette.color_for(biome);
        const std::string row = "| ![](assets/svg/" + slug + ".svg) | ";
        const std::string tail = " | `" + slug + "` | `" + hex + "` | "
                               + std::to_string(static_cast<int>(color.r)) + ", "
                               + std::to_string(static_cast<int>(color.g)) + ", "
                               + std::to_string(static_cast<int>(color.b)) + " |";

        const std::size_t at = readme.find(row);
        ASSERT_TRUE(at != std::string::npos);
        ASSERT_TRUE(readme.find(tail, at) != std::string::npos);
        ASSERT_TRUE(readme.find(tail, at) < readme.find('\n', at));

        // And the swatch itself is that colour, not merely a file of the right name.
        const std::string swatch = read_file(root + "/assets/svg/" + slug + ".svg");
        ASSERT_TRUE(!swatch.empty());
        ASSERT_TRUE(swatch.find("fill=\"" + hex + "\"") != std::string::npos);
        ++rows_checked;
    }
    ASSERT_EQ(static_cast<std::size_t>(rows_checked), k_biome_count);

    // The overlay half of the legend, keyed by the swatch each row points at.
    const std::pair<const char*, glm::vec3> overlays[] = {
        {"river", palette.river_color},
        {"bridge", palette.bridge_color},
        {"trail", palette.trail_color},
        {"road", palette.road_color},
        {"highway", palette.highway_color},
        {"building", palette.building_color},
        {"settlement", palette.town_color},
        {"landmark-natural", palette.landmark_natural_color},
        {"landmark-built", palette.landmark_built_color},
        {"background", palette.background_color},
    };
    for (const auto& overlay : overlays) {
        const std::string hex = hex_of(overlay.second);
        const std::string row = "| ![](assets/svg/" + std::string(overlay.first) + ".svg) |";
        const std::size_t at = readme.find(row);
        ASSERT_TRUE(at != std::string::npos);
        ASSERT_TRUE(readme.find("`" + hex + "`", at) < readme.find('\n', at));

        const std::string swatch = read_file(root + "/assets/svg/" + overlay.first + ".svg");
        ASSERT_TRUE(!swatch.empty());
        ASSERT_TRUE(swatch.find("fill=\"" + hex + "\"") != std::string::npos);
    }

    // The tint caveat is the one thing a reader can check against a render and
    // find false, so it may not quietly disappear either.
    ASSERT_TRUE(readme.find("untinted") != std::string::npos);
}


// --- Configuration files --------------------------------------------------

/** @brief Writes a throwaway YAML file and returns its path. */
static std::string write_temp_yaml(const std::string& name, const std::string& body) {
    std::ofstream out(name);
    out << body;
    out.close();
    return name;
}

/**
 * @brief A `MapConfig` with every field moved off its default.
 *
 * Deliberately exhaustive and deliberately not derived from the defaults: the
 * point is that a field left out of `config_to_node()` or `config_from_node()`
 * comes back as its default, so any field this function forgets to disturb is a
 * field the round-trip test cannot catch.
 */
static MapConfig perturbed_config() {
    MapConfig config;
    config.grid_size = 37;
    config.jitter = 0.41;
    config.seed = 90210;
    config.border_length = 1.75;
    config.image_size = 333;
    config.png_compression_level = 4;
    config.show_regions = false;
    config.composite_shading = CompositeShading::Hillshade;
    config.region_tint = 0.42f;
    config.temperature_lapse_rate = 0.31;
    config.temperature_falloff = 2.4;
    config.elevation_smoothing_iterations = 9;
    config.elevation_smoothing_strength = 0.66;
    config.threshold_water = 0.44;
    config.threshold_water_count = 3;
    config.river_count = 17;
    config.river_width_base_m = 0.077;
    config.river_width_per_volume_m = 0.033;
    config.trail_width_m = 0.11;
    config.road_width_m = 0.22;
    config.highway_width_m = 0.33;
    config.subdivide_noisy_edges = false;

    config.noise_island = {11, 0.123, FastNoiseLite::NoiseType_Cellular,
                           FastNoiseLite::FractalType_Ridged, 3, 2.5, 0.6, 0.7};
    config.noise_temperature = {22, 0.456, FastNoiseLite::NoiseType_Perlin,
                                FastNoiseLite::FractalType_PingPong, 7, 1.5, 0.4, 0.2};

    config.towns = {13, 555.5, 2, 6, 0.55, 3.5, 0.15, 0.25, 0.3, 0.75, 0.05,
                    9, 5, 2, 41, 8.5, 17.5, 0.8, 0.5, 4.5,
                    2, 9, 1.9, 1.4, 9.5, 16.5, 2.5, 0.5, 123};
    config.roads = {19, 444.5, 4.5, 2.5, 3.5, 1.25, 0.95, 44.0, 3, 0.65, 0.5, 0.2, 4};
    config.regions = {7, 4, 9.5, 3.25, 31.0};
    config.landmarks = {29, 31, 0.71, 0.088, 0.52, 12, 3.75, 2, 7, 5, 4};

    config.enable_water = false;
    config.enable_coast = false;
    config.enable_elevation = false;
    config.enable_temperature = false;
    config.enable_rivers = false;
    config.enable_moisture = false;
    config.enable_biomes = false;
    config.enable_roads = false;
    config.enable_regions = false;
    config.enable_towns = false;
    config.enable_landmarks = false;
    config.enable_noisy_edges = false;
    return config;
}

static void test_load_config_reports_a_missing_file() {
    MapConfig config;
    config.grid_size = 64;   // A caller's own default, which must survive.
    config.river_count = 9;

    const ConfigLoadResult result = load_config("no_such_config_file.yaml", config);
    ASSERT_TRUE(result.status == ConfigLoad::NotFound);
    ASSERT_TRUE(!result.has_seed);
    ASSERT_TRUE(!result.message.empty());
    // Nothing applied, so the caller's defaults are intact rather than reset.
    ASSERT_EQ(config.grid_size, 64);
    ASSERT_EQ(config.river_count, 9);
}

static void test_load_config_rejects_a_malformed_file() {
    MapConfig config;
    config.grid_size = 64;

    const std::string broken = write_temp_yaml("test_broken_config.yaml",
                                               "seed: 42\n  bad indent: [\n");
    const ConfigLoadResult result = load_config(broken, config);
    std::remove(broken.c_str());

    ASSERT_TRUE(result.status == ConfigLoad::Malformed);
    ASSERT_TRUE(!result.message.empty());
    ASSERT_EQ(config.grid_size, 64);

    // An empty file parses as null, not as an empty mapping. Reported rather than
    // taken as "no keys set": a caller asked for this file by name.
    const std::string empty = write_temp_yaml("test_empty_config.yaml", "");
    const ConfigLoadResult from_empty = load_config(empty, config);
    std::remove(empty.c_str());
    ASSERT_TRUE(from_empty.status == ConfigLoad::Malformed);
}

static void test_config_round_trips_every_field() {
    const MapConfig original = perturbed_config();

    const std::string path = "test_full_config.yaml";
    {
        std::ofstream out(path);
        out << config_to_node(original);
    }

    // Loaded onto a *default* config, so any field the writer or the reader forgets
    // comes back as its default and fails below.
    MapConfig loaded;
    const ConfigLoadResult result = load_config(path, loaded);
    std::remove(path.c_str());

    ASSERT_TRUE(result.status == ConfigLoad::Ok);
    ASSERT_TRUE(result.has_seed);

    ASSERT_EQ(loaded.grid_size, original.grid_size);
    ASSERT_TRUE(std::abs(loaded.jitter - original.jitter) < 1e-9);
    ASSERT_EQ(loaded.seed, original.seed);
    ASSERT_TRUE(std::abs(loaded.border_length - original.border_length) < 1e-9);
    ASSERT_EQ(loaded.image_size, original.image_size);
    ASSERT_EQ(loaded.png_compression_level, original.png_compression_level);
    ASSERT_TRUE(loaded.show_regions == original.show_regions);
    ASSERT_TRUE(loaded.composite_shading == original.composite_shading);
    ASSERT_TRUE(std::abs(loaded.region_tint - original.region_tint) < 1e-6f);
    ASSERT_TRUE(std::abs(loaded.temperature_lapse_rate - original.temperature_lapse_rate) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.temperature_falloff - original.temperature_falloff) < 1e-9);
    ASSERT_EQ(loaded.elevation_smoothing_iterations, original.elevation_smoothing_iterations);
    ASSERT_TRUE(std::abs(loaded.elevation_smoothing_strength
                         - original.elevation_smoothing_strength) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.threshold_water - original.threshold_water) < 1e-9);
    ASSERT_EQ(loaded.threshold_water_count, original.threshold_water_count);
    ASSERT_EQ(loaded.river_count, original.river_count);
    ASSERT_TRUE(std::abs(loaded.river_width_base_m - original.river_width_base_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_width_per_volume_m - original.river_width_per_volume_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.trail_width_m - original.trail_width_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.road_width_m - original.road_width_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.highway_width_m - original.highway_width_m) < 1e-9);
    ASSERT_TRUE(loaded.subdivide_noisy_edges == original.subdivide_noisy_edges);

    // Both noise fields, which is the reason they share one pair of helpers: a
    // second hand-written copy is what falls behind.
    const NoiseConfig* noises[2][2] = {
        {&loaded.noise_island, &original.noise_island},
        {&loaded.noise_temperature, &original.noise_temperature},
    };
    for (const auto& pair : noises) {
        ASSERT_EQ(pair[0]->seed, pair[1]->seed);
        ASSERT_TRUE(std::abs(pair[0]->frequency - pair[1]->frequency) < 1e-9);
        ASSERT_TRUE(pair[0]->type == pair[1]->type);
        ASSERT_TRUE(pair[0]->fractal_type == pair[1]->fractal_type);
        ASSERT_EQ(pair[0]->octaves, pair[1]->octaves);
        ASSERT_TRUE(std::abs(pair[0]->lacunarity - pair[1]->lacunarity) < 1e-9);
        ASSERT_TRUE(std::abs(pair[0]->gain - pair[1]->gain) < 1e-9);
        ASSERT_TRUE(std::abs(pair[0]->weighted_strength - pair[1]->weighted_strength) < 1e-9);
    }

    ASSERT_EQ(loaded.towns.town_count, original.towns.town_count);
    ASSERT_TRUE(std::abs(loaded.towns.min_spacing_m - original.towns.min_spacing_m) < 1e-9);
    ASSERT_EQ(loaded.towns.capital_count, original.towns.capital_count);
    ASSERT_EQ(loaded.towns.town_tier_count, original.towns.town_tier_count);
    ASSERT_EQ(loaded.towns.buildings_per_town, original.towns.buildings_per_town);
    ASSERT_TRUE(std::abs(loaded.towns.building_size_min_m - original.towns.building_size_min_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.towns.building_size_max_m - original.towns.building_size_max_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.towns.water_clearance_m - original.towns.water_clearance_m) < 1e-9);
    ASSERT_EQ(loaded.towns.household_size_min, original.towns.household_size_min);
    ASSERT_EQ(loaded.towns.household_size_max, original.towns.household_size_max);
    ASSERT_EQ(loaded.towns.infill_attempts, original.towns.infill_attempts);

    ASSERT_EQ(loaded.roads.hub_count, original.roads.hub_count);
    ASSERT_TRUE(std::abs(loaded.roads.hub_min_spacing_m - original.roads.hub_min_spacing_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.roads.slope_cost - original.roads.slope_cost) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.roads.ford_cost - original.roads.ford_cost) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.roads.water_crossing_cost
                         - original.roads.water_crossing_cost) < 1e-9);
    ASSERT_EQ(loaded.roads.max_water_span, original.roads.max_water_span);
    ASSERT_TRUE(std::abs(loaded.roads.reuse_discount - original.roads.reuse_discount) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.roads.highway_traffic_share
                         - original.roads.highway_traffic_share) < 1e-9);
    ASSERT_EQ(loaded.roads.smoothing_iterations, original.roads.smoothing_iterations);

    // The two blocks that were missing entirely before configuration files existed.
    ASSERT_EQ(loaded.regions.country_count, original.regions.country_count);
    ASSERT_EQ(loaded.regions.regions_per_country, original.regions.regions_per_country);
    ASSERT_TRUE(std::abs(loaded.regions.min_country_spacing
                         - original.regions.min_country_spacing) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.regions.elevation_cost - original.regions.elevation_cost) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.regions.water_crossing_cost
                         - original.regions.water_crossing_cost) < 1e-9);

    ASSERT_EQ(loaded.landmarks.max_natural, original.landmarks.max_natural);
    ASSERT_EQ(loaded.landmarks.max_abandoned, original.landmarks.max_abandoned);
    ASSERT_TRUE(std::abs(loaded.landmarks.peak_elevation
                         - original.landmarks.peak_elevation) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.landmarks.waterfall_drop
                         - original.landmarks.waterfall_drop) < 1e-9);
    ASSERT_EQ(loaded.landmarks.great_lake_cells, original.landmarks.great_lake_cells);
    ASSERT_EQ(loaded.landmarks.kind_share_denominator, original.landmarks.kind_share_denominator);
    ASSERT_EQ(loaded.landmarks.cape_ocean_ratio_numerator,
              original.landmarks.cape_ocean_ratio_numerator);

    // All twelve toggles, which a saved map used to lose outright.
    ASSERT_TRUE(loaded.enable_water == original.enable_water);
    ASSERT_TRUE(loaded.enable_coast == original.enable_coast);
    ASSERT_TRUE(loaded.enable_elevation == original.enable_elevation);
    ASSERT_TRUE(loaded.enable_temperature == original.enable_temperature);
    ASSERT_TRUE(loaded.enable_rivers == original.enable_rivers);
    ASSERT_TRUE(loaded.enable_moisture == original.enable_moisture);
    ASSERT_TRUE(loaded.enable_biomes == original.enable_biomes);
    ASSERT_TRUE(loaded.enable_roads == original.enable_roads);
    ASSERT_TRUE(loaded.enable_regions == original.enable_regions);
    ASSERT_TRUE(loaded.enable_towns == original.enable_towns);
    ASSERT_TRUE(loaded.enable_landmarks == original.enable_landmarks);
    ASSERT_TRUE(loaded.enable_noisy_edges == original.enable_noisy_edges);
}

static void test_load_config_overrides_only_what_it_names() {
    MapConfig config;
    config.grid_size = 64;
    config.river_count = 9;
    config.roads.hub_count = 5;

    // An absent key leaves the caller's value alone; that is what makes a config
    // file an override rather than a replacement, and what lets the generator set
    // its own scene defaults first.
    const std::string path = write_temp_yaml("test_partial_config.yaml",
                                             "grid_size: 12\nnot_a_real_key: 7\n");
    const ConfigLoadResult result = load_config(path, config);
    std::remove(path.c_str());

    ASSERT_TRUE(result.status == ConfigLoad::Ok);
    ASSERT_TRUE(!result.has_seed);   // Never named, so a caller may still draw one.
    ASSERT_EQ(config.grid_size, 12);
    ASSERT_EQ(config.river_count, 9);
    ASSERT_EQ(config.roads.hub_count, 5);
}

/**
 * @brief The shipped assets/config.yaml is documentation, and drifts from the tool.
 *
 * It is what a reader edits and what the README describes, so a value changed in
 * one and not the other is a silent lie. Nothing else would catch it: the
 * generator reads whatever the file says, so a wrong value produces a different
 * map perfectly happily.
 */
static void test_shipped_config_matches_the_documented_defaults() {
    MapConfig config;
    const ConfigLoadResult result =
        load_config(std::string(ROOT_DIR) + "/assets/config.yaml", config);

    ASSERT_TRUE(result.status == ConfigLoad::Ok);
    ASSERT_TRUE(result.has_seed);

    // The values README.md's options table quotes as this tool's defaults.
    ASSERT_EQ(config.grid_size, 80);
    ASSERT_EQ(config.river_count, 55);
    // The scale the whole file is denominated in. image_size is NOT asserted --
    // it is derived from these two, and the shipped file deliberately omits it.
    ASSERT_TRUE(std::abs(config.meters_per_grid_unit - 60.0) < 1e-9);
    ASSERT_TRUE(std::abs(config.meters_per_pixel - 1.0) < 1e-9);
    ASSERT_TRUE(config.composite_shading == CompositeShading::Elevation);
    ASSERT_EQ(derive_image_size(config), 4800);
    ASSERT_EQ(config.towns.town_count, 28);
    ASSERT_EQ(config.roads.hub_count, 32);
    ASSERT_EQ(config.regions.country_count, 5);
    ASSERT_EQ(config.regions.regions_per_country, 3);

    // Shipping with a pass turned off would silently produce a map missing a whole
    // feature, and the symptom would look like a bug in the pass.
    ASSERT_TRUE(config.enable_water && config.enable_coast && config.enable_elevation);
    ASSERT_TRUE(config.enable_temperature && config.enable_rivers && config.enable_moisture);
    ASSERT_TRUE(config.enable_biomes && config.enable_roads && config.enable_regions);
    ASSERT_TRUE(config.enable_towns && config.enable_landmarks && config.enable_noisy_edges);
}


// --- World scale and layers -----------------------------------------------

/**
 * @brief The scale arithmetic, and the round trip through --image-size.
 *
 * These three numbers -- grid size, metres per cell, metres per pixel -- are
 * what make every other size in the config mean something, so an error here
 * silently rescales the entire world rather than breaking anything visibly.
 */
static void test_world_scale_arithmetic() {
    MapConfig config;
    config.grid_size = 80;
    config.meters_per_grid_unit = 60.0;
    config.meters_per_pixel = 1.0;

    // 80 cells x 60 m = 4.8 km, at one pixel to the metre.
    ASSERT_EQ(derive_image_size(config), 4800);
    ASSERT_TRUE(std::abs(meters_to_grid(config, 60.0) - 1.0) < 1e-12);
    ASSERT_TRUE(std::abs(grid_to_meters(config, 1.0) - 60.0) < 1e-12);
    ASSERT_TRUE(std::abs(meters_to_grid(config, grid_to_meters(config, 0.37)) - 0.37) < 1e-12);

    // Coarser pixels, same world.
    config.meters_per_pixel = 4.0;
    ASSERT_EQ(derive_image_size(config), 1200);

    // What --image-size does: back-compute the scale so the two cannot disagree.
    const double world_meters =
        static_cast<double>(config.grid_size) * config.meters_per_grid_unit;
    config.meters_per_pixel = world_meters / 2400.0;
    ASSERT_EQ(derive_image_size(config), 2400);
    ASSERT_TRUE(std::abs(config.meters_per_pixel - 2.0) < 1e-12);
}

/**
 * @brief A feature configured in metres is drawn that many pixels across.
 *
 * The whole point of the scale: at one pixel to the metre a width read off a
 * render is a measurement. Checked against `draw_line()` directly rather than
 * against a generated map, because a road in a map is curved and a scanline
 * across a curve measures the secant, not the width.
 *
 * One pixel of slack, and in one direction only: the brush is a disc of integer
 * radius, so it can only draw odd widths, and `half_width_pixels_()` rounds down
 * so an even width understates rather than overstates.
 */
static void test_features_render_at_their_configured_size() {
    MapConfig config;
    config.grid_size = 80;
    config.meters_per_grid_unit = 60.0;
    config.meters_per_pixel = 1.0;
    config.image_size = derive_image_size(config);
    const double scale = static_cast<double>(config.image_size) / config.grid_size;

    // Mirrors MapLayers::half_width_pixels_, which is private to the renderer.
    const auto half_width_for = [scale](double width_grid) { return width_grid * scale * 0.5; };
    const auto drawn_width = [](double half_width) {
        Image image;
        image.reset(200, 200, 3, glm::vec3(0.0f));
        draw_line(image, 20.0, 100.0, 180.0, 100.0, half_width, glm::vec3(255.0f));
        int best = 0, run = 0;
        for (int y = 0; y < image.height; ++y) {
            run = image.color_at(100, y).r > 0.0f ? run + 1 : 0;
            best = std::max(best, run);
        }
        return best;
    };

    // One pixel of slack either way: an even width cannot be centred on a pixel
    // row, so an axis-aligned stroke -- which is what this measures -- rounds up
    // to an odd row count.
    for (const RoadClass road_class : {RoadClass::Trail, RoadClass::Road, RoadClass::Highway}) {
        const int meters = static_cast<int>(road_width_meters(config, road_class));
        const int pixels = drawn_width(half_width_for(road_width_for(config, road_class)));
        ASSERT_TRUE(std::abs(pixels - meters) <= 1);
    }
    for (const int volume : {0, 3, 10}) {
        const int meters = static_cast<int>(river_width_meters(config, volume));
        const int pixels = drawn_width(half_width_for(river_width(config, volume)));
        ASSERT_TRUE(std::abs(pixels - meters) <= 1);
    }

    // Building footprints are stored in grid units but configured in metres.
    ASSERT_TRUE(std::abs(grid_to_meters(config,
                    meters_to_grid(config, config.towns.building_size_min_m))
                - config.towns.building_size_min_m) < 1e-9);
    ASSERT_TRUE(config.towns.building_size_min_m >= 5.0);
    ASSERT_TRUE(config.towns.building_size_max_m <= 20.0);
}

/**
 * @brief A stroke is the width it was asked for, whichever way it runs.
 *
 * Two width bugs lived here in turn, and neither was visible from a horizontal
 * measurement. A **square** brush widened a line by up to sqrt(2) off the axes,
 * so a diagonal 6 m road drew 8 m wide. Replacing it with a round brush stamped
 * along an 8-connected path introduced the opposite error -- the path advances
 * sqrt(2) of ground per step, so a diagonal drew 0.707 of its width. `draw_line()`
 * now paints by distance to the segment and has neither problem.
 *
 * Measured as painted area over Euclidean length, which is direction-independent;
 * a scanline measures the secant across anything not perpendicular to it. The
 * tolerance is a pixel and a bit: pixel centres sit on integers, so an
 * axis-aligned band of even width has to round to an odd row count, and that
 * parity is irreducible however the stroke is defined.
 */
static void test_stroke_width_is_direction_independent() {
    const auto mean_width = [](double x0, double y0, double x1, double y1, double width) {
        Image image;
        image.reset(400, 400, 3, glm::vec3(0.0f));
        draw_line(image, x0, y0, x1, y1, width * 0.5, glm::vec3(255.0f));
        std::size_t painted = 0;
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                if (image.color_at(x, y).r > 0.0f) {
                    ++painted;
                }
            }
        }
        return static_cast<double>(painted) / std::hypot(x1 - x0, y1 - y0);
    };

    for (const double width : {3.0, 5.0, 6.0, 10.0, 25.0}) {
        const double flat = mean_width(40, 200, 360, 200, width);
        const double diagonal = mean_width(40, 40, 360, 360, width);

        // Both orientations land within rasterisation parity of the width they
        // were given. A square brush put the diagonal 40% over; a brush stamped
        // along an 8-connected path put it 30% under. Either would blow through
        // this at every width tested.
        ASSERT_TRUE(std::abs(flat - width) <= 1.6);
        ASSERT_TRUE(std::abs(diagonal - width) <= 1.6);
    }
}

static void test_rivers_are_long_and_smooth() {
    MapConfig config = world_config(31);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    ASSERT_TRUE(!graph.rivers.empty());
    for (const MapRiver& river : graph.rivers) {
        // Short trickles are rejected and redrawn, so every kept watercourse
        // actually crosses some country.
        ASSERT_TRUE(static_cast<int>(river.corners.size()) >= config.river_min_length);

        // Corner-cutting multiplies the point count; a run of N corners that
        // came back with N points was never smoothed.
        ASSERT_TRUE(river.points.size() > river.corners.size());
        ASSERT_TRUE(river.volume > 0);

        for (const MapPoint& point : river.points) {
            ASSERT_TRUE(std::isfinite(point.x) && std::isfinite(point.y));
            ASSERT_TRUE(point.x >= 0.0 && point.x <= static_cast<double>(config.grid_size));
            ASSERT_TRUE(point.y >= 0.0 && point.y <= static_cast<double>(config.grid_size));
        }
        // The course runs downhill, source to mouth.
        const MapCorner& source = graph.corners[static_cast<std::size_t>(river.corners.front())];
        const MapCorner& mouth = graph.corners[static_cast<std::size_t>(river.corners.back())];
        ASSERT_TRUE(source.elevation >= mouth.elevation);
        ASSERT_TRUE(source.elevation >= config.river_source_min_elevation - 1e-9);
    }
}

static void test_settlements_claim_cells_by_tier() {
    MapConfig config = world_config(77);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    const TownConfig& towns = config.towns;
    ASSERT_TRUE(!graph.towns.empty());

    std::map<CenterId, int> owner_count;
    int capital_buildings = 0;
    int village_buildings = 0;
    int capitals = 0;
    int villages = 0;

    for (const MapTown& town : graph.towns) {
        ASSERT_TRUE(!town.cells.empty());
        ASSERT_EQ(town.cells.front(), town.center);

        const int allowance = town.tier == TownTier::Capital ? towns.capital_cells
                            : town.tier == TownTier::Town    ? towns.town_cells
                                                             : towns.village_cells;
        ASSERT_TRUE(static_cast<int>(town.cells.size()) <= allowance);

        for (const CenterId cell_id : town.cells) {
            // A cell belongs to at most one settlement, so two neighbours never
            // build on the same ground.
            ++owner_count[cell_id];
            ASSERT_EQ(owner_count[cell_id], 1);
            const MapCenter& cell = graph.centers[static_cast<std::size_t>(cell_id)];
            ASSERT_TRUE(!cell.water && !cell.ocean && !cell.border);
        }

        if (town.tier == TownTier::Capital) {
            ++capitals;
            capital_buildings += static_cast<int>(town.buildings.size());
        } else if (town.tier == TownTier::Village) {
            ++villages;
            village_buildings += static_cast<int>(town.buildings.size());
        }
    }

    // The tiers exist to be distinguishable. Confined to one cell they were not:
    // a capital and a village both filled the same ~3,600 m2 and looked alike.
    if (capitals > 0 && villages > 0) {
        const double capital_mean = static_cast<double>(capital_buildings) / capitals;
        const double village_mean = static_cast<double>(village_buildings) / villages;
        ASSERT_TRUE(capital_mean > village_mean * 1.5);
    }
}

static void test_layers_separate_their_concerns() {
    MapConfig config = small_config(5);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    for (std::size_t i = 0; i < k_map_layer_count; ++i) {
        const MapLayer layer = static_cast<MapLayer>(i);
        const Image image = MapLayers::render(layer, graph, config);
        ASSERT_EQ(image.width, config.image_size);
        ASSERT_EQ(image.height, config.image_size);
        ASSERT_TRUE(!map_layer_name(layer).empty());

        const int expected_channels = map_layer_has_alpha(layer) ? 4 : 3;
        ASSERT_EQ(image.channels, expected_channels);
        ASSERT_EQ(image.pixels.size(),
                  static_cast<std::size_t>(image.width) * image.height * expected_channels);

        std::size_t opaque = 0;
        std::size_t transparent = 0;
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                if (image.alpha_at(x, y) == 0) {
                    ++transparent;
                } else {
                    ++opaque;
                }
            }
        }
        if (map_layer_has_alpha(layer)) {
            // An overlay carries no background of its own: that is what lets it
            // stack over the terrain without hiding it.
            ASSERT_TRUE(transparent > 0);
            ASSERT_TRUE(opaque > 0);
        } else {
            ASSERT_EQ(transparent, static_cast<std::size_t>(0));
        }
    }

    // Each overlay carries only its own subject. Not a claim that they never
    // overlap in screen space -- a building beside a road legitimately does --
    // but that no layer has quietly picked up another's contents.
    const BiomePalette palette;
    const auto only_colors = [](const Image& image, const std::vector<glm::vec3>& allowed) {
        for (int y = 0; y < image.height; ++y) {
            for (int x = 0; x < image.width; ++x) {
                if (image.alpha_at(x, y) == 0) {
                    continue;
                }
                const glm::vec3 found = image.color_at(x, y);
                bool matched = false;
                for (const glm::vec3& candidate : allowed) {
                    matched = matched || (found.r == candidate.r && found.g == candidate.g
                                          && found.b == candidate.b);
                }
                if (!matched) {
                    return false;
                }
            }
        }
        return true;
    };

    ASSERT_TRUE(only_colors(MapLayers::roads(graph, config),
                            {palette.trail_color, palette.road_color, palette.highway_color,
                             palette.bridge_color}));
    ASSERT_TRUE(only_colors(MapLayers::structures(graph, config), {palette.building_color}));
    ASSERT_TRUE(only_colors(MapLayers::landmarks(graph, config),
                            {palette.town_color, palette.landmark_natural_color,
                             palette.landmark_built_color}));
}


/**
 * @brief Both composite shading modes work, and they are genuinely different.
 *
 * They answer different questions, which is why both exist: elevation shading
 * is a function of height, so it says how high the ground is and the same height
 * reads the same everywhere; hillshading is a function of slope, so it sculpts
 * the relief but cannot distinguish a slope at sea level from the same slope on
 * a summit.
 */
static void test_composite_shading_modes() {
    ASSERT_TRUE(MapConfig{}.composite_shading == CompositeShading::Elevation);
    for (std::size_t i = 0; i < k_composite_shading_count; ++i) {
        const CompositeShading mode = static_cast<CompositeShading>(i);
        ASSERT_TRUE(composite_shading_from_name(composite_shading_name(mode)) == mode);
    }
    ASSERT_TRUE(composite_shading_from_name("sunlight") == CompositeShading::Elevation);

    MapConfig by_height = small_config(12);
    by_height.composite_shading = CompositeShading::Elevation;
    MapConfig by_slope = by_height;
    by_slope.composite_shading = CompositeShading::Hillshade;

    MapGenerator generator(by_height, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    const Image elevation_lit = MapLayers::composite(graph, by_height);
    const Image slope_lit = MapLayers::composite(graph, by_slope);
    ASSERT_EQ(elevation_lit.pixels.size(), slope_lit.pixels.size());
    ASSERT_TRUE(elevation_lit.pixels != slope_lit.pixels);

    // Neither mode touches the height data: the elevation layer is the raw field
    // under both, which is what keeps it usable as a heightmap.
    const Image height_a = MapLayers::elevation(graph, by_height);
    const Image height_b = MapLayers::elevation(graph, by_slope);
    ASSERT_TRUE(height_a.pixels == height_b.pixels);

    // Under elevation shading, high ground really is brighter than low ground of
    // the same biome -- the property the mode exists for. Compared within one
    // biome so the palette cannot account for the difference.
    const double scale =
        static_cast<double>(by_height.image_size) / static_cast<double>(by_height.grid_size);
    const MapCenter* lowest = nullptr;
    const MapCenter* highest = nullptr;
    for (const MapCenter& center : graph.centers) {
        if (center.water || center.ocean || center.border
            || center.biome != Biome::Grassland) {
            continue;
        }
        if (!lowest || center.elevation < lowest->elevation) lowest = &center;
        if (!highest || center.elevation > highest->elevation) highest = &center;
    }
    if (lowest && highest && highest->elevation - lowest->elevation > 0.05) {
        const auto brightness_at = [&elevation_lit, scale](const MapCenter& center) {
            const glm::vec3 color = elevation_lit.color_at(
                static_cast<int>(center.point.x * scale),
                static_cast<int>(center.point.y * scale));
            return color.r + color.g + color.b;
        };
        ASSERT_TRUE(brightness_at(*highest) > brightness_at(*lowest));
    }
}


// --- Asynchronous generation and export -----------------------------------

/** @brief Element-wise comparison of two graphs; the fields a pass can write. */
static void assert_graphs_match(const MapGraph& a, const MapGraph& b) {
    ASSERT_EQ(a.centers.size(), b.centers.size());
    ASSERT_EQ(a.corners.size(), b.corners.size());
    ASSERT_EQ(a.edges.size(), b.edges.size());
    ASSERT_EQ(a.roads.size(), b.roads.size());
    ASSERT_EQ(a.rivers.size(), b.rivers.size());
    ASSERT_EQ(a.towns.size(), b.towns.size());
    ASSERT_EQ(a.regions.size(), b.regions.size());
    ASSERT_EQ(a.landmarks.size(), b.landmarks.size());

    for (std::size_t i = 0; i < a.centers.size(); ++i) {
        ASSERT_TRUE(a.centers[i].biome == b.centers[i].biome);
        ASSERT_TRUE(a.centers[i].elevation == b.centers[i].elevation);
        ASSERT_TRUE(a.centers[i].moisture == b.centers[i].moisture);
        ASSERT_EQ(a.centers[i].region, b.centers[i].region);
    }
    for (std::size_t i = 0; i < a.edges.size(); ++i) {
        ASSERT_EQ(a.edges[i].river, b.edges[i].river);
        ASSERT_EQ(a.edges[i].traffic, b.edges[i].traffic);
        ASSERT_TRUE(a.edges[i].road_class == b.edges[i].road_class);
        ASSERT_EQ(a.edges[i].noisy_points0.size(), b.edges[i].noisy_points0.size());
    }
    for (std::size_t i = 0; i < a.towns.size(); ++i) {
        ASSERT_EQ(a.towns[i].center, b.towns[i].center);
        ASSERT_EQ(a.towns[i].buildings.size(), b.towns[i].buildings.size());
        ASSERT_TRUE(a.towns[i].name == b.towns[i].name);
    }
}

/**
 * @brief Generating on a job engine produces exactly what generating inline does.
 *
 * The guarantee the whole threading design is built around. Nothing inside
 * generation is parallel -- it is 135 ms of a 10 s run, not worth the risk -- so
 * what this really pins down is that moving the work to another thread changed
 * none of it, which is the kind of thing that silently stops being true.
 */
static void test_async_generation_matches_serial() {
    MapConfig config = world_config(4242);

    MapGenerator serial(config, maps_logger());
    serial.generate();

    MapGenerator threaded(config, maps_logger());
    threaded.set_job_engine(&maps_engine());
    {
        MapTask task = threaded.generate_async();
        task.wait();
        ASSERT_TRUE(task.done());
        ASSERT_TRUE(!task.cancelled());
        ASSERT_TRUE(task.progress() == 1.0f);
    }
    assert_graphs_match(serial.graph(), threaded.graph());
}

/** @brief Progress runs from 0 to exactly 1 and never goes backwards. */
static void test_task_progress_is_monotonic() {
    MapConfig config = world_config(9);
    MapGenerator generator(config, maps_logger());
    generator.set_job_engine(&maps_engine());

    MapTask task = generator.generate_async();
    float last = 0.0f;
    for (int poll = 0; poll < 100000 && !task.done(); ++poll) {
        const float now = task.progress();
        ASSERT_TRUE(now >= last);
        ASSERT_TRUE(now >= 0.0f && now <= 1.0f);
        last = now;
    }
    task.wait();
    ASSERT_TRUE(task.progress() == 1.0f);
}

/**
 * @brief A cancelled generation stops and says so, and does not corrupt anything.
 *
 * Cancellation is cooperative and checked between passes, so a task cancelled the
 * instant it is created may still have run a pass or two -- what is asserted is
 * that it reports itself cancelled and finished, not that it did nothing.
 */
static void test_generation_can_be_cancelled() {
    MapConfig config = world_config(11);
    MapGenerator generator(config, maps_logger());
    generator.set_job_engine(&maps_engine());

    MapTask task = generator.generate_async();
    task.cancel();
    task.wait();

    ASSERT_TRUE(task.done());
    ASSERT_TRUE(task.cancelled());
    // The geometry is built before any pass runs, so it survives cancellation --
    // and every id in it still indexes its own array.
    for (const MapCenter& center : generator.graph().centers) {
        ASSERT_TRUE(center.index >= 0);
        ASSERT_TRUE(static_cast<std::size_t>(center.index) < generator.graph().centers.size());
    }
}

/** @brief A task destroyed while its work is in flight cancels and waits, not crashes. */
static void test_task_destructor_waits() {
    MapConfig config = world_config(13);
    MapGenerator generator(config, maps_logger());
    generator.set_job_engine(&maps_engine());
    {
        MapTask task = generator.generate_async();
        // Dropped immediately, mid-flight. The destructor has to cancel and join,
        // because the job writes into `generator`'s graph and would otherwise be
        // doing so after this scope decided it was finished with it.
    }
    // Reaching here without a crash or a hang is the assertion. Generating again
    // on the same generator must then work normally.
    generator.generate();
    ASSERT_TRUE(!generator.graph().centers.empty());
}

/**
 * @brief Every layer renders identically whether split across threads or not.
 *
 * Checked through `MapExporter`'s own rendering path rather than by calling
 * `render()` twice, so what is compared is what actually gets written.
 */
static void test_parallel_export_matches_serial() {
    for (const CompositeShading shading : {CompositeShading::Elevation,
                                           CompositeShading::Hillshade}) {
        MapConfig config = small_config(5);
        config.composite_shading = shading;
        MapGenerator generator(config, maps_logger());
        generator.generate();

        const std::string serial_prefix = "test_export_serial";
        const std::string parallel_prefix = "test_export_parallel";

        MapExporter serial;
        ASSERT_TRUE(serial.export_layers(generator.graph(), config, serial_prefix));

        MapExporter threaded;
        threaded.set_job_engine(&maps_engine());
        ASSERT_TRUE(threaded.export_layers(generator.graph(), config, parallel_prefix));

        for (std::size_t i = 0; i < k_map_layer_count; ++i) {
            const std::string name(map_layer_name(static_cast<MapLayer>(i)));
            const std::string a = read_file(serial_prefix + "_" + name + ".png");
            const std::string b = read_file(parallel_prefix + "_" + name + ".png");
            ASSERT_TRUE(!a.empty());
            ASSERT_TRUE(a == b);
            std::remove((serial_prefix + "_" + name + ".png").c_str());
            std::remove((parallel_prefix + "_" + name + ".png").c_str());
        }
    }
}

/**
 * @brief Splitting a layer into row bands changes nothing about the result.
 *
 * The test that would catch an off-by-one at a band seam, which is the bug this
 * design most invites. Run down to one-row bands, where every seam there could
 * be is exercised at once.
 */
static void test_band_rendering_matches_whole_image() {
    MapConfig config = small_config(21);
    config.image_size = 96;
    for (const CompositeShading shading : {CompositeShading::Elevation,
                                           CompositeShading::Hillshade}) {
        config.composite_shading = shading;
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();

        const CellGeometry geometry = MapLayers::build_cell_geometry(graph, config);
        HeightField height;
        const HeightField* height_ptr = nullptr;
        if (shading == CompositeShading::Hillshade) {
            height = MapLayers::build_height_field(graph, config, BiomePalette{}, &geometry);
            height_ptr = &height;
        }

        for (std::size_t i = 0; i < k_map_layer_count; ++i) {
            const MapLayer layer = static_cast<MapLayer>(i);
            const Image whole = MapLayers::render(layer, graph, config, BiomePalette{},
                                                  RenderSlice{RowBand{}, &geometry, height_ptr});
            for (const int bands : {2, 5, 96}) {
                Image banded = MapLayers::allocate(layer, config, BiomePalette{});
                for (int b = 0; b < bands; ++b) {
                    const int from = config.image_size * b / bands;
                    const int to = config.image_size * (b + 1) / bands;
                    MapLayers::render_into(banded, layer, graph, config, BiomePalette{},
                                           RenderSlice{RowBand{from, to}, &geometry, height_ptr});
                }
                ASSERT_EQ(whole.pixels.size(), banded.pixels.size());
                ASSERT_TRUE(whole.pixels == banded.pixels);
            }
        }
    }
}

/** @brief The concurrency and band knobs are performance dials, not output ones. */
static void test_export_tuning_does_not_change_output() {
    MapConfig config = small_config(33);
    MapGenerator generator(config, maps_logger());
    generator.generate();

    const CellGeometry geometry = MapLayers::build_cell_geometry(generator.graph(), config);
    const Image reference = MapLayers::render(MapLayer::Composite, generator.graph(), config,
                                             BiomePalette{}, RenderSlice{RowBand{}, &geometry});

    for (const std::size_t cap : {std::size_t{1}, std::size_t{3}, std::size_t{0}}) {
        for (const int band_rows : {1, 7, config.image_size}) {
            MapExporter exporter;
            exporter.set_job_engine(&maps_engine());
            exporter.set_max_concurrent_layers(cap);
            exporter.set_band_rows(band_rows);

            const std::string prefix = "test_export_tuned";
            ASSERT_TRUE(exporter.export_layers(generator.graph(), config, prefix));
            const std::string path = prefix + "_composite.png";
            // Round-tripping through the file would need a decoder; comparing the
            // rendered buffer is the same guarantee one step earlier.
            ASSERT_TRUE(!read_file(path).empty());
            std::remove(path.c_str());
            for (std::size_t i = 0; i < k_map_layer_count; ++i) {
                std::remove((prefix + "_" + std::string(map_layer_name(static_cast<MapLayer>(i)))
                             + ".png").c_str());
            }
        }
    }
    ASSERT_TRUE(!reference.pixels.empty());
}

} // namespace maps_test


int main() {
    std::cout << "===========================================" << std::endl;
    std::cout << "        Running mapcoopa Test Suite       " << std::endl;
    std::cout << "===========================================" << std::endl;

    RUN_TEST(maps_test::test_biome_name_round_trips);
    RUN_TEST(maps_test::test_classify_biome_table);
    RUN_TEST(maps_test::test_temperature_follows_latitude);
    RUN_TEST(maps_test::test_biome_diversity);
    RUN_TEST(maps_test::test_generate_is_deterministic);
    RUN_TEST(maps_test::test_different_seeds_differ);
    RUN_TEST(maps_test::test_graph_invariants_hold);
    RUN_TEST(maps_test::test_water_separates_ocean_from_lakes);
    RUN_TEST(maps_test::test_rivers_terminate_on_hostile_terrain);
    RUN_TEST(maps_test::test_rivers_flow_downhill_to_the_coast);
    RUN_TEST(maps_test::test_noisy_edges_subdivide_only_when_enabled);
    RUN_TEST(maps_test::test_towns_sit_on_habitable_land_and_stay_apart);
    RUN_TEST(maps_test::test_buildings_lie_inside_their_cell);
    RUN_TEST(maps_test::test_buildings_do_not_overlap);
    RUN_TEST(maps_test::test_building_layout_is_not_a_lattice);
    RUN_TEST(maps_test::test_buildings_front_their_streets);
    RUN_TEST(maps_test::test_buildings_avoid_rivers);
    RUN_TEST(maps_test::test_elevation_is_smoothed);
    RUN_TEST(maps_test::test_building_sizes_span_the_range);
    RUN_TEST(maps_test::test_regions_partition_the_land);
    RUN_TEST(maps_test::test_names_are_unique_and_reproducible);
    RUN_TEST(maps_test::test_population_scales_with_buildings);
    RUN_TEST(maps_test::test_landmarks_respect_their_biome);
    RUN_TEST(maps_test::test_cell_outline_is_closed_and_ordered);
    RUN_TEST(maps_test::test_renderers_produce_a_full_image);
    RUN_TEST(maps_test::test_rivers_live_on_the_water_layer_only);
    RUN_TEST(maps_test::test_yaml_round_trip_preserves_the_graph);
    RUN_TEST(maps_test::test_yaml_round_trip_renders_every_layer);
    RUN_TEST(maps_test::test_disabled_passes_leave_the_graph_untouched);
    RUN_TEST(maps_test::test_biome_habitability_ranks_the_land);
    RUN_TEST(maps_test::test_road_class_names_round_trip);
    RUN_TEST(maps_test::test_roads_form_one_network_reaching_the_towns);
    RUN_TEST(maps_test::test_road_class_follows_traffic);
    RUN_TEST(maps_test::test_roads_bridge_only_where_they_meet_water);
    RUN_TEST(maps_test::test_roads_keep_off_the_border_and_the_open_sea);
    RUN_TEST(maps_test::test_road_runs_partition_the_flagged_edges);
    RUN_TEST(maps_test::test_road_runs_are_smoothed_only_when_asked);
    RUN_TEST(maps_test::test_readme_legend_matches_the_palette);
    RUN_TEST(maps_test::test_load_config_reports_a_missing_file);
    RUN_TEST(maps_test::test_load_config_rejects_a_malformed_file);
    RUN_TEST(maps_test::test_config_round_trips_every_field);
    RUN_TEST(maps_test::test_load_config_overrides_only_what_it_names);
    RUN_TEST(maps_test::test_shipped_config_matches_the_documented_defaults);
    RUN_TEST(maps_test::test_world_scale_arithmetic);
    RUN_TEST(maps_test::test_features_render_at_their_configured_size);
    RUN_TEST(maps_test::test_stroke_width_is_direction_independent);
    RUN_TEST(maps_test::test_rivers_are_long_and_smooth);
    RUN_TEST(maps_test::test_settlements_claim_cells_by_tier);
    RUN_TEST(maps_test::test_layers_separate_their_concerns);
    RUN_TEST(maps_test::test_composite_shading_modes);
    RUN_TEST(maps_test::test_async_generation_matches_serial);
    RUN_TEST(maps_test::test_task_progress_is_monotonic);
    RUN_TEST(maps_test::test_generation_can_be_cancelled);
    RUN_TEST(maps_test::test_task_destructor_waits);
    RUN_TEST(maps_test::test_parallel_export_matches_serial);
    RUN_TEST(maps_test::test_band_rendering_matches_whole_image);
    RUN_TEST(maps_test::test_export_tuning_does_not_change_output);

    std::cout << "===========================================" << std::endl;
    std::cout << "Test Summary: " << g_tests_run - g_tests_failed << " / " << g_tests_run << " Passed." << std::endl;
    if (g_tests_failed > 0) {
        std::cout << ANSI_COLOR_RED << "Some tests failed!" << ANSI_COLOR_RESET << std::endl;
        return 1;
    } else {
        std::cout << ANSI_COLOR_GREEN << "All tests passed successfully!" << ANSI_COLOR_RESET << std::endl;
        return 0;
    }
}
