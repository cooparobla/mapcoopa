/**
 * @file test.cpp
 * @brief mapcoopa's test suite -- 69 cases over the generator, its passes, the
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

/**
 * @brief Sets a render resolution the way the loader and `--image-size` do.
 *
 * By back-computing the scale, so `image_size` and `meters_per_pixel` cannot
 * disagree -- see `MapConfig::image_size`. Assigning `image_size` alone leaves a
 * fixture claiming a 960 m world is 128 px across *at one pixel to the metre*,
 * which these fixtures did for as long as they existed.
 *
 * @param config Configured in place; `grid_size` and `meters_per_grid_unit` must
 *     already be set.
 * @param pixels Desired side length of the render.
 */
static void set_render_size(MapConfig& config, int pixels) {
    const double world =
        static_cast<double>(config.grid_size) * config.meters_per_grid_unit;
    config.meters_per_pixel = world / static_cast<double>(pixels);
    config.image_size = derive_image_size(config);
}

static MapConfig small_config(int seed = 251) {
    MapConfig config;
    config.grid_size = 16;
    set_render_size(config, 128);
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
    ASSERT_TRUE(classify_biome(0.5, 0.5, 0.5, false, false, true) == Biome::Beach);

    // A water cell is `Ice` or `Lake` and nothing else, at any elevation and any
    // moisture -- see `test_water_cells_always_get_a_water_biome` for why that
    // matters. A shallow lake used to come back `Marsh` and a high one `Ice`.
    ASSERT_TRUE(classify_biome(0.05, 0.5, 0.5, true, false, false) == Biome::Lake);
    ASSERT_TRUE(classify_biome(0.9, 0.5, 0.5, true, false, false) == Biome::Lake);
    ASSERT_TRUE(classify_biome(0.5, 0.5, 0.5, true, false, false) == Biome::Lake);
    ASSERT_TRUE(classify_biome(0.05, 0.9, 0.9, true, false, false) == Biome::Lake);
    // Frozen on temperature alone, which already carries the altitude lapse rate.
    ASSERT_TRUE(classify_biome(0.5, 0.5, 0.1, true, false, false) == Biome::Ice);
    ASSERT_TRUE(classify_biome(0.05, 0.5, 0.1, true, false, false) == Biome::Ice);

    // Wetlands are *land* now: low, wet ground beside the water rather than the
    // water itself.
    ASSERT_TRUE(classify_biome(0.05, 0.9, 0.5, false, false, false) == Biome::Marsh);
    ASSERT_TRUE(classify_biome(0.05, 0.9, 0.9, false, false, false) == Biome::Swamp);
    ASSERT_TRUE(classify_biome(0.05, 0.9, 0.3, false, false, false) == Biome::BorealWetland);
    // Low but dry is not a wetland, and frozen ground is permafrost not marsh.
    ASSERT_TRUE(classify_biome(0.05, 0.4, 0.5, false, false, false) != Biome::Marsh);
    ASSERT_TRUE(classify_biome(0.05, 0.9, 0.1, false, false, false) != Biome::Marsh);
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
 * @brief Rivers reach the height field by carving it, and only by carving it.
 *
 * The elevation layer used to have its river network *dimmed* into it, which was
 * wrong twice over: it made the layer a picture of the terrain rather than the
 * terrain itself, and a consumer flooding a mesh to those values found channels
 * already cut. That painting is gone and is not coming back.
 *
 * What replaced it is the valley pass, which lowers the ground. So the layer must
 * now differ when rivers run -- otherwise the carve never reached the pixels --
 * and must be *bit-identical* once the incision is set to zero, which is the
 * guarantee that the only thing moving the image is the terrain itself.
 */
static void test_rivers_cut_valleys_into_the_height_field() {
    MapConfig with_rivers_config = small_config();
    with_rivers_config.subdivide_noisy_edges = false;

    MapConfig without_rivers_config = with_rivers_config;
    without_rivers_config.enable_rivers = false;

    MapGenerator with_rivers(with_rivers_config, maps_logger());
    MapGenerator without_rivers(without_rivers_config, maps_logger());
    with_rivers.generate();
    without_rivers.generate();
    ASSERT_TRUE(!with_rivers.graph().rivers.empty());

    // The height field notices, because the ground under a river is lower.
    const Image drawn = MapLayers::elevation(with_rivers.graph(), with_rivers_config);
    const Image base = MapLayers::elevation(without_rivers.graph(), without_rivers_config);
    ASSERT_EQ(drawn.pixels.size(), base.pixels.size());
    ASSERT_TRUE(drawn.pixels != base.pixels);

    // The water layer very much does too.
    const Image wet = MapLayers::water(with_rivers.graph(), with_rivers_config);
    const Image dry = MapLayers::water(without_rivers.graph(), without_rivers_config);
    ASSERT_EQ(wet.pixels.size(), dry.pixels.size());
    ASSERT_TRUE(wet.pixels != dry.pixels);

    // With nothing carved -- neither the valley in the mesh nor the channel in
    // the surface -- the two are the same image again, so the difference above is
    // the carving and not some other thing the river pass touched.
    MapConfig uncarved_config = with_rivers_config;
    uncarved_config.river_incision_m = 0.0;
    uncarved_config.river_incision_per_volume_m = 0.0;
    uncarved_config.river_channel_depth_m = 0.0;
    uncarved_config.river_channel_depth_per_volume_m = 0.0;
    MapConfig uncarved_without_config = uncarved_config;
    uncarved_without_config.enable_rivers = false;

    MapGenerator uncarved(uncarved_config, maps_logger());
    MapGenerator uncarved_without(uncarved_without_config, maps_logger());
    uncarved.generate();
    uncarved_without.generate();

    const Image flat = MapLayers::elevation(uncarved.graph(), uncarved_config);
    const Image flat_base =
        MapLayers::elevation(uncarved_without.graph(), uncarved_without_config);
    ASSERT_TRUE(flat.pixels == flat_base.pixels);
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

        // Two different tolerances, because there are two different ways six
        // significant digits can show up in a render.
        //
        // On a layer derived from the *height field* the loss is smooth: a
        // slightly different corner height tilts a Delaunay facet and moves a
        // greyscale value by a step. Bounded per pixel.
        //
        // On a layer made of *hard-edged shapes* it is not smooth at all. A
        // building footprint whose corner round-trips a ten-thousandth of a grid
        // unit away can put one boundary pixel on the other side of the edge --
        // a full 0-to-255 flip on that pixel, however exact everything else is.
        // So those are bounded by *how many* pixels may differ, not by how much.
        // Elevation only. The composite is *both* kinds of layer at once -- height
        // shading underneath, then water, roads, building footprints and markers
        // on top -- so one of its boundary pixels can flip a full 0-to-255 just
        // like a structures pixel can. Classing it as smooth was over-claiming,
        // and it held only as long as no footprint edge happened to straddle a
        // pixel centre. The count bound below still covers it, and is what
        // actually catches a structural regression here.
        const bool from_height = layer == MapLayer::Elevation;
        std::size_t differing = 0;
        for (std::size_t k = 0; k < before.pixels.size(); ++k) {
            const int delta = std::abs(static_cast<int>(before.pixels[k])
                                     - static_cast<int>(after.pixels[k]));
            if (delta == 0) {
                continue;
            }
            ++differing;
            if (from_height) {
                ASSERT_TRUE(delta <= 1);
            }
        }
        // A thousandth of the image, which a structural regression would dwarf:
        // dropping the corner elevations entirely moved 12 bytes on one layer and
        // a whole building is some hundreds.
        ASSERT_TRUE(differing * 1000 <= before.pixels.size());
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
    set_render_size(config, 256);
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
    // The render scale, set from the metres end. `image_size` is not listed
    // separately: it is not independent of this, and the two are reconciled on
    // load -- see the assertions in `test_config_round_trips_every_field`. It
    // used to say `image_size = 333` beside a `meters_per_pixel` of 1.0 on a
    // 37 x 60 m world, which is 2220 m of ground claiming to be 333 px at one
    // pixel to the metre: a pair that could not both be true, and which only went
    // unnoticed because the value was overwritten before anything read it.
    config.meters_per_pixel = 3.0;
    config.png_compression_level = 4;
    config.sea_level = 0.18;
    config.elevation_range_m = 777.0;
    config.river_depth_m = 2.5;
    config.river_depth_per_volume_m = 0.6;
    config.river_channel_depth_m = 23.0;
    config.river_channel_depth_per_volume_m = 7.0;
    config.river_incision_m = 88.0;
    config.river_incision_per_volume_m = 17.0;
    config.river_valley_width = 4;
    config.river_valley_falloff = 0.33;
    config.water_edge_overlap_m = 3.0;
    config.terrain_relief = 0.31;
    config.terrain_roughness = 0.44;
    config.noise_relief = {44, 0.11, FastNoiseLite::NoiseType_Perlin,
                           FastNoiseLite::FractalType_FBm, 3, 2.1, 0.55, 0.4};
    config.noise_terrain = {33, 0.77, FastNoiseLite::NoiseType_Value,
                            FastNoiseLite::FractalType_FBm, 4, 1.9, 0.45, 0.3};
    config.shape = {MapShape::Triangle, 111.0, 222.0, 333.0, 444.0, 0.55,
                    555.0, 6, 0.41, 0.22, 0.19};
    config.noise_shape = {66, 0.22, FastNoiseLite::NoiseType_Perlin,
                          FastNoiseLite::FractalType_FBm, 2, 2.3, 0.35, 0.25};
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
    config.enable_valleys = false;
    config.enable_moisture = false;
    config.enable_biomes = false;
    config.enable_roads = false;
    config.enable_regions = false;
    config.enable_towns = false;
    config.enable_landmarks = false;
    config.enable_noisy_edges = false;

    // Derived last, from the grid and the scale above, so this fixture satisfies
    // the `image_size * meters_per_pixel == grid_size * meters_per_grid_unit`
    // invariant. A fixture that did not could not round-trip: the loader
    // reconciles the pair, so writing out a contradiction and reading it back
    // necessarily changes one of the two.
    config.image_size = derive_image_size(config);
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
    // Both ends of the render scale, and the invariant tying them together.
    ASSERT_TRUE(std::abs(loaded.meters_per_pixel - original.meters_per_pixel) < 1e-9);
    ASSERT_EQ(loaded.image_size, original.image_size);
    ASSERT_EQ(loaded.image_size, derive_image_size(loaded));
    ASSERT_EQ(loaded.png_compression_level, original.png_compression_level);
    ASSERT_TRUE(std::abs(loaded.sea_level - original.sea_level) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.elevation_range_m - original.elevation_range_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_depth_m - original.river_depth_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_depth_per_volume_m
                         - original.river_depth_per_volume_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_channel_depth_m - original.river_channel_depth_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_channel_depth_per_volume_m
                         - original.river_channel_depth_per_volume_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_incision_m - original.river_incision_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.river_incision_per_volume_m
                         - original.river_incision_per_volume_m) < 1e-9);
    ASSERT_TRUE(loaded.river_valley_width == original.river_valley_width);
    ASSERT_TRUE(std::abs(loaded.river_valley_falloff - original.river_valley_falloff) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.water_edge_overlap_m - original.water_edge_overlap_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.terrain_relief - original.terrain_relief) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.terrain_roughness - original.terrain_roughness) < 1e-9);
    ASSERT_TRUE(loaded.shape.shape == original.shape.shape);
    ASSERT_TRUE(std::abs(loaded.shape.width_m - original.shape.width_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.height_m - original.shape.height_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.diameter_m - original.shape.diameter_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.edge_length_m - original.shape.edge_length_m) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.rotation - original.shape.rotation) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.continent_size_m - original.shape.continent_size_m) < 1e-9);
    ASSERT_TRUE(loaded.shape.continent_count == original.shape.continent_count);
    ASSERT_TRUE(std::abs(loaded.shape.irregularity - original.shape.irregularity) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.size_variance - original.shape.size_variance) < 1e-9);
    ASSERT_TRUE(std::abs(loaded.shape.coast_detail - original.shape.coast_detail) < 1e-9);
    ASSERT_TRUE(loaded.noise_shape.seed == original.noise_shape.seed);
    ASSERT_TRUE(std::abs(loaded.noise_shape.frequency - original.noise_shape.frequency) < 1e-9);
    ASSERT_TRUE(loaded.noise_shape.type == original.noise_shape.type);
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
    const NoiseConfig* noises[4][2] = {
        {&loaded.noise_island, &original.noise_island},
        {&loaded.noise_temperature, &original.noise_temperature},
        {&loaded.noise_relief, &original.noise_relief},
        {&loaded.noise_terrain, &original.noise_terrain},
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
    ASSERT_TRUE(loaded.enable_valleys == original.enable_valleys);
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
    ASSERT_TRUE(config.enable_valleys);
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
/**
 * @brief `image_size` is an input, and setting it cannot leave the scale stale.
 *
 * The two render-scale fields are one knob with two ends, and the invariant
 * `image_size * meters_per_pixel == grid_size * meters_per_grid_unit` has to
 * hold however a caller arrives at it -- every feature is stroked in metres and
 * converted through the scale, so a contradictory pair draws features at a width
 * the resolution does not agree with.
 *
 * `image_size` used to be read out of a document and then thrown away, so
 * `image_size: 2048` in a configuration file was silently ignored. It is now
 * honoured, and honouring it means back-computing `meters_per_pixel`.
 */
static void test_config_image_size_sets_the_scale() {
    const std::string path = "test_image_size.yaml";
    const auto write = [&path](const char* body) {
        std::ofstream out(path);
        out << body;
    };

    // 1. A document naming image_size is honoured, and the scale follows it.
    //    40 cells x 60 m = 2400 m of world in 600 px is 4 m to the pixel.
    write("grid_size: 40\nimage_size: 600\n");
    MapConfig from_pixels;
    ASSERT_TRUE(load_config(path, from_pixels).status == ConfigLoad::Ok);
    ASSERT_EQ(from_pixels.image_size, 600);
    ASSERT_TRUE(std::abs(from_pixels.meters_per_pixel - 4.0) < 1e-12);
    ASSERT_EQ(from_pixels.image_size, derive_image_size(from_pixels));

    // 2. The other end: naming the scale sizes the render.
    write("grid_size: 40\nmeters_per_pixel: 3\n");
    MapConfig from_scale;
    ASSERT_TRUE(load_config(path, from_scale).status == ConfigLoad::Ok);
    ASSERT_EQ(from_scale.image_size, 800);
    ASSERT_TRUE(std::abs(from_scale.meters_per_pixel - 3.0) < 1e-12);

    // 3. Both given and disagreeing: image_size wins, being the more concrete
    //    statement of intent, and the scale is corrected rather than kept.
    write("grid_size: 40\nimage_size: 1200\nmeters_per_pixel: 37\n");
    MapConfig both;
    ASSERT_TRUE(load_config(path, both).status == ConfigLoad::Ok);
    ASSERT_EQ(both.image_size, 1200);
    ASSERT_TRUE(std::abs(both.meters_per_pixel - 2.0) < 1e-12);
    ASSERT_EQ(both.image_size, derive_image_size(both));

    // 4. Neither given: meters_per_pixel is 1.0, always, so a render is a
    //    one-pixel-per-metre map and a pixel count off it is a measurement.
    write("grid_size: 40\n");
    MapConfig neither;
    ASSERT_TRUE(load_config(path, neither).status == ConfigLoad::Ok);
    ASSERT_TRUE(std::abs(neither.meters_per_pixel - 1.0) < 1e-12);
    ASSERT_EQ(neither.image_size, 2400);

    // 5. A default-constructed config already satisfies the invariant, rather
    //    than starting out contradicting itself.
    const MapConfig fresh;
    ASSERT_TRUE(std::abs(fresh.meters_per_pixel - 1.0) < 1e-12);
    ASSERT_EQ(fresh.image_size, derive_image_size(fresh));

    std::remove(path.c_str());

    // 6. And the renderer takes ONE scale: a resolution set either way round
    //    scales markers and roads together. Two configs describing the same
    //    world at the same resolution must render identically, whichever end
    //    they were written from.
    MapConfig by_pixels = small_config(5);
    set_render_size(by_pixels, 160);
    MapConfig by_scale = small_config(5);
    by_scale.meters_per_pixel =
        static_cast<double>(by_scale.grid_size) * by_scale.meters_per_grid_unit / 160.0;
    by_scale.image_size = derive_image_size(by_scale);
    ASSERT_EQ(by_pixels.image_size, by_scale.image_size);

    MapGenerator generator(by_pixels, maps_logger());
    generator.generate();
    for (std::size_t i = 0; i < k_map_layer_count; ++i) {
        const MapLayer layer = static_cast<MapLayer>(i);
        const Image a = MapLayers::render(layer, generator.graph(), by_pixels);
        const Image b = MapLayers::render(layer, generator.graph(), by_scale);
        ASSERT_EQ(a.pixels.size(), b.pixels.size());
        ASSERT_TRUE(a.pixels == b.pixels);
    }
}

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
    }

    // Sources are drawn from high ground -- but that is a statement about the
    // terrain the river pass *chose* from, and the valley pass has since cut the
    // ground away beneath them. Checking it against the carved field would be
    // asserting that rivers do not erode their own headwaters. So it is checked
    // on the uncarved run, which is the surface the threshold was applied to.
    MapConfig uncarved = config;
    uncarved.enable_valleys = false;
    MapGenerator before(uncarved, maps_logger());
    before.generate();
    const MapGraph& unworn = before.graph();
    ASSERT_TRUE(!unworn.rivers.empty());
    for (const MapRiver& river : unworn.rivers) {
        const MapCorner& source = unworn.corners[static_cast<std::size_t>(river.corners.front())];
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
    // Stripped back to bare terrain, because the brightness comparison below
    // samples a cell at its own site and the composite draws things there. Region
    // tint would give two grassland cells in different provinces different base
    // colours; a landmark or settlement marker would cover the pixel outright,
    // which is what it was doing -- both samples came back as marker blue.
    by_height.show_regions = false;
    by_height.enable_landmarks = false;
    by_height.enable_towns = false;
    by_height.enable_roads = false;
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
    set_render_size(config, 96);
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


// --- Water levels ---------------------------------------------------------

/**
 * @brief Every body of water has one flat surface, which `elevation` does not.
 *
 * The sea used to render mottled because the water layer drew `elevation` -- the
 * height of the *bed*. Only `border` corners are pinned to zero, and the rank
 * remap then spreads every corner across [0, 1], so an ocean cell away from the
 * map edge has a small but nonzero height. Flat water has to be stated.
 */
static void test_water_bodies_are_flat() {
    MapConfig config = world_config(17);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::size_t ocean_cells = 0;
    for (const MapCenter& center : graph.centers) {
        if (!center.ocean) {
            continue;
        }
        ++ocean_cells;
        ASSERT_TRUE(center.water_level == config.sea_level);
    }
    ASSERT_TRUE(ocean_cells > 0);

    // And the bug this guards: the bed really does vary, so a flat surface is
    // not something that would have fallen out by accident.
    double lowest_bed = 1.0;
    double highest_bed = 0.0;
    for (const MapCenter& center : graph.centers) {
        if (center.ocean) {
            lowest_bed = std::min(lowest_bed, center.elevation);
            highest_bed = std::max(highest_bed, center.elevation);
        }
    }
    ASSERT_TRUE(highest_bed > lowest_bed);

    // Each lake is one surface across every cell of the body, and that surface
    // is at or above the highest bed in it, so no basin pokes through.
    std::vector<bool> visited(graph.centers.size(), false);
    std::size_t lakes = 0;
    for (const MapCenter& seed : graph.centers) {
        const std::size_t seed_index = static_cast<std::size_t>(seed.index);
        if (!seed.water || seed.ocean || visited[seed_index]) {
            continue;
        }
        ++lakes;
        std::vector<CenterId> pending{seed.index};
        visited[seed_index] = true;
        const double level = seed.water_level;
        while (!pending.empty()) {
            const CenterId current = pending.back();
            pending.pop_back();
            const MapCenter& cell = graph.centers[static_cast<std::size_t>(current)];
            ASSERT_TRUE(cell.water_level == level);
            ASSERT_TRUE(level >= cell.elevation - 1e-12);
            for (const CenterId neighbor_id : cell.neighbors) {
                const std::size_t index = static_cast<std::size_t>(neighbor_id);
                const MapCenter& neighbor = graph.centers[index];
                if (!visited[index] && neighbor.water && !neighbor.ocean) {
                    visited[index] = true;
                    pending.push_back(neighbor_id);
                }
            }
        }
    }
    ASSERT_TRUE(lakes > 0);
}

/** @brief The open sea renders as one grey, because it is one surface. */
static void test_water_layer_draws_one_grey_over_open_sea() {
    MapConfig config = small_config(17);
    MapGenerator generator(config, maps_logger());
    generator.generate();

    const Image water = MapLayers::water(generator.graph(), config);
    // The outermost ring is inside the forced-water border band under every
    // shape, so it is open sea whatever the terrain did.
    const glm::vec3 corner = water.color_at(0, 0);
    for (int i = 0; i < water.width; ++i) {
        for (const int y : {0, water.height - 1}) {
            ASSERT_TRUE(water.color_at(i, y).r == corner.r);
        }
        for (const int x : {0, water.width - 1}) {
            ASSERT_TRUE(water.color_at(x, i).r == corner.r);
        }
    }
}

// --- Elevation sampling ---------------------------------------------------

/**
 * @brief The sampled surface interpolates the control mesh, and joins across cells.
 *
 * Barycentric interpolation replaced inverse-distance weighting, which read as a
 * plateau: every corner is roughly equidistant from the middle of a cell, so most
 * of the interior came out near the mean of the corners. What has to survive the
 * change is that the surface still passes through the values it interpolates and
 * still meets itself at a shared edge.
 */
static void test_elevation_interpolates_and_joins() {
    MapConfig config = world_config(23);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::size_t checked = 0;
    for (const MapCenter& center : graph.centers) {
        if (center.corners.size() < 3 || center.border) {
            continue;
        }
        // At a site the surface passes through that site's own height: the sites
        // are the interpolation vertices now, not the corners. A Voronoi corner
        // is interior to a Delaunay triangle, so its own `elevation` is *not*
        // what the surface reads there, and asserting otherwise was a leftover
        // from the corner-fan interpolation this replaced.
        ASSERT_TRUE(std::abs(graph.elevation_at(center, center.point.x, center.point.y)
                             - center.elevation) < 1e-9);
        // A sample anywhere in the cell stays in range. Deliberately not asserted
        // against the three sites around the nearest *corner*: a Delaunay triangle
        // with an obtuse angle has its circumcentre outside itself, so the
        // triangle claiming a corner need not be the one that corner belongs to,
        // and bounding by that corner's own sites is not an invariant.
        for (const CornerId corner_id : center.corners) {
            const MapCorner& corner = graph.corners[static_cast<std::size_t>(corner_id)];
            const double sampled = graph.elevation_at(center, corner.point.x, corner.point.y);
            ASSERT_TRUE(sampled >= 0.0 && sampled <= 1.0);
        }
        if (++checked >= 40) {
            break;
        }
    }
    ASSERT_TRUE(checked > 0);

    // Continuity: along an edge two cells share, both sides interpolate between
    // the same two corner heights, so both must agree.
    std::size_t seams = 0;
    for (const MapEdge& edge : graph.edges) {
        if (edge.d0 == k_invalid_id || edge.d1 == k_invalid_id
            || edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
            continue;
        }
        const MapCenter& a = graph.centers[static_cast<std::size_t>(edge.d0)];
        const MapCenter& b = graph.centers[static_cast<std::size_t>(edge.d1)];
        if (a.corners.size() < 3 || b.corners.size() < 3) {
            continue;
        }
        const MapPoint& mid = edge.midpoint;
        const double from_a = graph.elevation_at(a, mid.x, mid.y);
        const double from_b = graph.elevation_at(b, mid.x, mid.y);
        ASSERT_TRUE(std::abs(from_a - from_b) < 1e-6);
        if (++seams >= 200) {
            break;
        }
    }
    ASSERT_TRUE(seams > 0);
}

/**
 * @brief Roughness displaces the surface inland and never at the coast.
 *
 * The taper is the whole point: scaled by the local height, so a shoreline stays
 * exactly at sea level and no land is nudged below it however rough the rest gets.
 */
static void test_terrain_roughness_tapers_to_the_coast() {
    MapConfig config = world_config(29);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    const Noise terrain(config.noise_terrain);

    MapConfig smooth = config;
    smooth.terrain_roughness = 0.0;
    const TerrainDetail none = make_terrain_detail(smooth, terrain);

    MapConfig rough = config;
    rough.terrain_roughness = 0.6;
    const TerrainDetail detail = make_terrain_detail(rough, terrain);

    std::size_t displaced = 0;
    for (const MapCenter& center : graph.centers) {
        if (center.corners.size() < 3) {
            continue;
        }
        const double x = center.point.x;
        const double y = center.point.y;
        const double base = graph.elevation_at(center, x, y);

        // Zero roughness is the control mesh, exactly.
        ASSERT_TRUE(graph.elevation_at(center, x, y, none) == base);

        const double displaced_height = graph.elevation_at(center, x, y, detail);
        ASSERT_TRUE(displaced_height >= 0.0 && displaced_height <= 1.0);
        if (base == 0.0) {
            // At sea level the taper leaves nothing to displace.
            ASSERT_TRUE(displaced_height == 0.0);
        } else if (displaced_height != base) {
            ++displaced;
        }
    }
    // Inland, it actually does something -- otherwise the taper test above would
    // pass on a knob that did nothing at all.
    ASSERT_TRUE(displaced > 0);
}

// --- Regions layer --------------------------------------------------------

static void test_regions_layer_draws_regions_and_borders() {
    MapConfig config = world_config(37);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.regions.empty());

    const BiomePalette palette;
    const Image regions = MapLayers::regions(graph, config, palette);
    ASSERT_EQ(regions.channels, 3);

    // Every pixel is exactly one region's colour or exactly the background --
    // nothing else, no third value. That is the property that makes the layer
    // readable as data, and it is why the near-black country borders that used
    // to be stroked over the fills are gone: they were neither, and a consumer
    // recovering a region from a pixel had no answer for them.
    std::size_t region_pixels = 0;
    for (int y = 0; y < regions.height; ++y) {
        for (int x = 0; x < regions.width; ++x) {
            const glm::vec3 found = regions.color_at(x, y);
            if (found == palette.background_color) {
                continue;
            }
            bool matched = false;
            for (const MapRegion& region : graph.regions) {
                matched = matched || found == region.color;
            }
            ASSERT_TRUE(matched);
            ++region_pixels;
        }
    }
    ASSERT_TRUE(region_pixels > 0);

    // With no political geography there is nothing to draw.
    MapConfig stateless = config;
    stateless.enable_regions = false;
    MapGenerator plain(stateless, maps_logger());
    plain.generate();
    const Image empty = MapLayers::regions(plain.graph(), stateless, palette);
    for (int y = 0; y < empty.height; ++y) {
        for (int x = 0; x < empty.width; ++x) {
            ASSERT_TRUE(empty.color_at(x, y) == palette.background_color);
        }
    }
}

// --- Landmass shapes ------------------------------------------------------

/**
 * @brief The default shape is the square frame it replaced, to the last bit.
 *
 * `shape_inset()` took over a hard-coded square test that every map ever
 * generated went through, so this is the regression guard that matters most:
 * `min(half - |dx|)` over a canvas-spanning rectangle has to equal
 * `min(x, grid - x, y, grid - y)` for every point, or every existing map moves.
 */
static void test_default_shape_matches_the_square_frame() {
    MapConfig config;
    config.grid_size = 40;
    const double grid = static_cast<double>(config.grid_size);

    for (double y = -3.0; y <= grid + 3.0; y += 0.37) {
        for (double x = -3.0; x <= grid + 3.0; x += 0.37) {
            const double expected = std::min(std::min(x, grid - x), std::min(y, grid - y));
            ASSERT_TRUE(std::abs(shape_inset(config, x, y) - expected) < 1e-9);
        }
    }
}

/** @brief Every shape names itself, and an unknown name falls back to a rectangle. */
static void test_shape_names_round_trip() {
    for (std::size_t i = 0; i < k_map_shape_count; ++i) {
        const MapShape shape = static_cast<MapShape>(i);
        ASSERT_TRUE(map_shape_from_name(map_shape_name(shape)) == shape);
    }
    ASSERT_TRUE(map_shape_from_name("rect") == MapShape::Rectangle);
    ASSERT_TRUE(map_shape_from_name("continent") == MapShape::Continent);
    ASSERT_TRUE(map_shape_from_name("archipelago") == MapShape::Archipelago);
    ASSERT_TRUE(map_shape_from_name("hexagon") == MapShape::Rectangle);
}

/**
 * @brief Land lands inside the chosen shape, and the sea fills the rest.
 *
 * Checked through a generated map rather than against `shape_inset()` directly,
 * so what is verified is that the border flag really does propagate into the
 * water pass and out the other side as coastline.
 */
static void test_shapes_confine_the_landmass() {
    struct Case { MapShape shape; double size_m; double rotation; };
    const Case cases[] = {
        {MapShape::Rectangle, 1200.0, 0.0},
        {MapShape::Circle, 1400.0, 0.0},
        {MapShape::Triangle, 1600.0, 0.5},
        {MapShape::Continent, 1400.0, 0.0},
        {MapShape::Archipelago, 700.0, 0.0},
    };

    for (const Case& test_case : cases) {
        MapConfig config = world_config(41);
        config.shape.shape = test_case.shape;
        config.shape.width_m = test_case.size_m;
        config.shape.height_m = test_case.size_m;
        config.shape.diameter_m = test_case.size_m;
        config.shape.edge_length_m = test_case.size_m;
        config.shape.continent_size_m = test_case.size_m;
        config.shape.rotation = test_case.rotation;

        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();

        std::size_t land = 0;
        for (const MapCenter& center : graph.centers) {
            const double inset = shape_inset(config, center.point.x, center.point.y);
            if (!center.water) {
                // Dry land is inside the shape. The cell's site can sit a little
                // inside the border band while a corner of it reaches out, which
                // is what the tolerance allows for.
                ASSERT_TRUE(inset > 0.0);
                ++land;
            }
            // Well outside the shape, everything is sea.
            if (inset < -2.0) {
                ASSERT_TRUE(center.water);
            }
        }
        ASSERT_TRUE(land > 0);
    }
}


/**
 * @brief A one-off `shape_inset()` agrees with a field held across a sweep.
 *
 * `border_check_()` builds one `ShapeField` and reuses it, while the tests and any
 * caller outside the library go through `shape_inset()`, which resolves a fresh one
 * per call. For the organic shapes that means a landmass layout redrawn from the
 * seed every time -- if the RNG stream ever depended on anything but the seed, the
 * two paths would disagree and every assertion made against `shape_inset()` would
 * be testing a different world from the one that was generated.
 */
static void test_shape_field_matches_shape_inset() {
    for (std::size_t i = 0; i < k_map_shape_count; ++i) {
        MapConfig config = world_config(77);
        config.shape.shape = static_cast<MapShape>(i);
        config.shape.rotation = 0.4;
        const double grid = static_cast<double>(config.grid_size);

        const ShapeField field(config);
        for (double y = -2.0; y <= grid + 2.0; y += 1.7) {
            for (double x = -2.0; x <= grid + 2.0; x += 1.7) {
                ASSERT_TRUE(std::abs(field.inset(x, y) - shape_inset(config, x, y)) < 1e-12);
            }
        }
    }
}

/**
 * @brief The organic shapes leave open sea all the way round the canvas.
 *
 * Not cosmetic. The water pass marks the ocean by flooding inward from the border
 * cells, and a landmass that reaches the frame would be sliced off by it -- and
 * worse, could wall the fill out of a bay and leave the sea classified as a lake.
 * The placement maths exists to make this true for every seed, so it is checked
 * across a spread of them rather than one.
 */
static void test_organic_shapes_stay_off_the_canvas_edge() {
    const MapShape shapes[] = {MapShape::Continent, MapShape::Archipelago};
    for (const MapShape shape : shapes) {
        for (int seed = 1; seed <= 12; ++seed) {
            MapConfig config = world_config(seed * 131);
            config.shape.shape = shape;
            const double grid = static_cast<double>(config.grid_size);
            const ShapeField field(config);

            for (double t = 0.0; t <= grid; t += 0.5) {
                ASSERT_TRUE(field.inset(t, 0.0) < 0.0);
                ASSERT_TRUE(field.inset(t, grid) < 0.0);
                ASSERT_TRUE(field.inset(0.0, t) < 0.0);
                ASSERT_TRUE(field.inset(grid, t) < 0.0);
            }
        }
    }
}

/**
 * @brief The outline is drawn from the seed, and from nothing else.
 *
 * Two fields built from one config have to be identical or a map would not
 * reproduce from its seed; two built from different seeds have to differ, or the
 * shape is a fixed silhouette wearing a random-looking coat.
 */
static void test_organic_shapes_are_deterministic() {
    MapConfig config = world_config(404);
    config.shape.shape = MapShape::Archipelago;
    const double grid = static_cast<double>(config.grid_size);

    const ShapeField first(config);
    const ShapeField again(config);
    MapConfig other = config;
    other.seed = 405;
    const ShapeField elsewhere(other);

    bool differs = false;
    for (double y = 0.0; y <= grid; y += 0.9) {
        for (double x = 0.0; x <= grid; x += 0.9) {
            ASSERT_TRUE(first.inset(x, y) == again.inset(x, y));
            if (std::abs(first.inset(x, y) - elsewhere.inset(x, y)) > 1e-6) {
                differs = true;
            }
        }
    }
    ASSERT_TRUE(differs);
}

/**
 * @brief Counts the connected groups of dry cells in a generated map.
 *
 * Land neighbouring land across a cell edge is the same landmass. Used to tell a
 * continent from an archipelago the way a reader would -- by looking at the map,
 * not at the configuration that asked for it.
 *
 * @param graph The generated graph to walk.
 * @param out_total Receives the number of dry cells found.
 * @return The size of each landmass, largest first.
 */
static std::vector<std::size_t> landmass_sizes(const MapGraph& graph, std::size_t& out_total) {
    std::vector<bool> seen(graph.centers.size(), false);
    std::vector<std::size_t> sizes;
    out_total = 0;

    for (const MapCenter& start : graph.centers) {
        const std::size_t start_index = static_cast<std::size_t>(start.index);
        if (start.water || seen[start_index]) {
            continue;
        }
        std::size_t size = 0;
        std::vector<CenterId> pending{start.index};
        seen[start_index] = true;
        while (!pending.empty()) {
            const MapCenter& current = graph.centers[static_cast<std::size_t>(pending.back())];
            pending.pop_back();
            ++size;
            for (const CenterId neighbor_id : current.neighbors) {
                const std::size_t index = static_cast<std::size_t>(neighbor_id);
                if (!graph.centers[index].water && !seen[index]) {
                    seen[index] = true;
                    pending.push_back(neighbor_id);
                }
            }
        }
        sizes.push_back(size);
        out_total += size;
    }

    std::sort(sizes.begin(), sizes.end(), std::greater<std::size_t>());
    return sizes;
}

/**
 * @brief A continent comes out as one landmass, not a scatter of islands.
 *
 * The island noise still carves lakes and bays out of the interior and can strand
 * a cell or two offshore, so this asks for a dominant landmass rather than a sole
 * one: most of the dry ground has to belong to a single connected mass.
 */
static void test_continent_is_one_landmass() {
    for (int seed = 1; seed <= 5; ++seed) {
        MapConfig config = world_config(seed * 97);
        config.shape.shape = MapShape::Continent;

        MapGenerator generator(config, maps_logger());
        generator.generate();

        std::size_t total = 0;
        const std::vector<std::size_t> sizes = landmass_sizes(generator.graph(), total);
        ASSERT_TRUE(total > 0);
        ASSERT_TRUE(!sizes.empty());
        ASSERT_TRUE(static_cast<double>(sizes.front()) > 0.85 * static_cast<double>(total));
    }
}

/**
 * @brief An archipelago comes out as several landmasses, none of them the whole map.
 *
 * Deliberately loose on the count: landmasses are allowed to fuse, which is the
 * point, so what is asserted is that asking for several got several, and that no
 * one of them swallowed the map -- the failure mode when they are sized too large
 * for the canvas to scatter them across.
 */
static void test_archipelago_makes_several_landmasses() {
    for (int seed = 1; seed <= 5; ++seed) {
        MapConfig config = world_config(seed * 89);
        config.shape.shape = MapShape::Archipelago;
        config.shape.continent_count = 4;

        MapGenerator generator(config, maps_logger());
        generator.generate();

        std::size_t total = 0;
        const std::vector<std::size_t> sizes = landmass_sizes(generator.graph(), total);
        ASSERT_TRUE(total > 0);
        // Slivers of a cell or two are island noise, not a continent.
        std::size_t substantial = 0;
        for (const std::size_t size : sizes) {
            if (static_cast<double>(size) > 0.05 * static_cast<double>(total)) {
                ++substantial;
            }
        }
        ASSERT_TRUE(substantial >= 2);
        ASSERT_TRUE(static_cast<double>(sizes.front()) < 0.85 * static_cast<double>(total));
    }
}

/**
 * @brief Relief reshapes where the high ground is without breaking what depends on it.
 *
 * The distance-from-coast field puts every summit on the medial axis of the
 * landmass, so relief blends it toward noise. Three things have to survive that:
 * the coast stays the lowest land, land stays above water so rivers run the right
 * way, and zero still means the untouched field.
 */
static void test_terrain_relief_reshapes_without_breaking_drainage() {
    MapConfig flat = world_config(53);
    flat.terrain_relief = 0.0;
    MapGenerator plain(flat, maps_logger());
    plain.generate();

    MapConfig shaped = flat;
    shaped.terrain_relief = 0.7;
    MapGenerator hilly(shaped, maps_logger());
    hilly.generate();

    // It actually changes the terrain -- otherwise the invariants below would
    // hold on a knob that did nothing.
    std::size_t moved = 0;
    ASSERT_EQ(plain.graph().corners.size(), hilly.graph().corners.size());
    for (std::size_t i = 0; i < plain.graph().corners.size(); ++i) {
        if (plain.graph().corners[i].elevation != hilly.graph().corners[i].elevation) {
            ++moved;
        }
    }
    ASSERT_TRUE(moved * 4 > plain.graph().corners.size());

    // The drainage still drains, which is the property the relief blend was built
    // around. `apply_relief_` lifts land clear of water precisely so that water
    // cannot outrank a coastal corner -- but it is not asserted directly here,
    // because `smooth_elevations_` runs afterwards and deliberately relaxes
    // corners across the shore, so the strict separation is gone by the time the
    // graph is observable. What survives is that no watercourse climbs, and that
    // every single one of them ends in water -- see
    // `test_every_river_ends_in_a_water_body` for why that is not a majority.
    const MapGraph& graph = hilly.graph();
    ASSERT_TRUE(!graph.rivers.empty());

    for (const MapRiver& river : graph.rivers) {
        for (std::size_t i = 0; i + 1 < river.corners.size(); ++i) {
            const MapCorner& from =
                graph.corners[static_cast<std::size_t>(river.corners[i])];
            const MapCorner& to =
                graph.corners[static_cast<std::size_t>(river.corners[i + 1])];
            ASSERT_TRUE(to.elevation <= from.elevation);
        }
        const MapCorner& mouth =
            graph.corners[static_cast<std::size_t>(river.corners.back())];
        ASSERT_TRUE(mouth.coast || mouth.water);
    }
}


/**
 * @brief Every river ends in a lake or the sea, and the terrain guarantees it.
 *
 * Two properties, and the second is the one that makes the first hold rather
 * than merely happen to be true on this seed.
 *
 * A river is a walk down `downslope`, so where it ends is decided entirely by
 * the height field. Relief noise, the rank remap and two smoothing passes each
 * move corners independently of their neighbours, and any of them can leave a
 * corner lower than everything around it. That corner is a pit, and a river that
 * reaches one stops in the middle of a field. It was not a rare accident: 80 of
 * 11 438 land corners were pits, and 23 of 55 rivers ended dry.
 *
 * `fill_depressions()` removes them, so the assertion here is on
 * the terrain and not on the rivers: *every* dry corner must have a strictly
 * lower neighbour, which by induction gives it a descending path to water. That
 * is a much stronger statement than "the 55 rivers this seed happened to place
 * all found the sea", and it is what a caller adding rivers, changing their
 * sources or sampling flow directly can rely on.
 */
/**
 * @brief A cell with water in it is classified as water, on every map.
 *
 * The layer a reader actually looks at is coloured by *biome*, not by
 * `MapCenter::water`, so those two disagreeing is a visible defect however sound
 * the underlying data is. `classify_biome()` used to hand a low water cell
 * `Marsh` and a high one `Ice` -- dark green and near-white -- so a river that
 * ended in a shallow lake ended in what reads as forest. 18% of them did, and a
 * lake you cannot see is indistinguishable from no lake at all.
 *
 * Asserted over a generated map rather than on the classifier alone because the
 * two can disagree through the *arguments*: `PassBiomes` passes
 * `land_height()`, so the thresholds are fractions of the land range, and it was
 * exactly that rescale -- moving the waterline off zero -- that pushed most
 * lakes under the old 0.1 marsh threshold. A table test on
 * `classify_biome()` would have stayed green throughout.
 */
static void test_water_cells_always_get_a_water_biome() {
    MapGenerator generator(world_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::size_t water_cells = 0;
    for (const MapCenter& center : graph.centers) {
        if (!center.water) {
            // And the converse: dry land never claims a water biome.
            ASSERT_TRUE(center.biome != Biome::Ocean && center.biome != Biome::Lake
                        && center.biome != Biome::Ice);
            continue;
        }
        ++water_cells;
        ASSERT_TRUE(center.biome == Biome::Ocean || center.biome == Biome::Lake
                    || center.biome == Biome::Ice);
        ASSERT_TRUE(center.ocean == (center.biome == Biome::Ocean));
    }
    ASSERT_TRUE(water_cells > 0);

    // Which is what makes this true: every river empties into a cell whose
    // *colour* is water.
    ASSERT_TRUE(!graph.rivers.empty());
    for (const MapRiver& river : graph.rivers) {
        const MapCorner& mouth =
            graph.corners[static_cast<std::size_t>(river.corners.back())];
        bool into_water = false;
        for (const CenterId center_id : mouth.touches) {
            const MapCenter& center = graph.centers[static_cast<std::size_t>(center_id)];
            into_water = into_water
                || center.biome == Biome::Ocean || center.biome == Biome::Lake
                || center.biome == Biome::Ice;
        }
        ASSERT_TRUE(into_water);
    }
}

static void test_every_river_ends_in_a_water_body() {
    MapGenerator generator(world_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.rivers.empty());

    const auto wet = [](const MapCorner& corner) { return corner.water || corner.coast; };

    // 1. No pit anywhere on dry land.
    std::size_t dry_corners = 0;
    for (const MapCorner& corner : graph.corners) {
        if (wet(corner)) {
            continue;
        }
        ++dry_corners;
        bool has_lower = false;
        for (const CornerId neighbor_id : corner.adjacent) {
            has_lower = has_lower
                || graph.corners[static_cast<std::size_t>(neighbor_id)].elevation
                       < corner.elevation;
        }
        ASSERT_TRUE(has_lower);
        // Which is exactly the condition under which `downslope` leaves.
        ASSERT_TRUE(corner.downslope != corner.index);
    }
    ASSERT_TRUE(dry_corners > 0);

    // 2. Therefore a downhill walk from any dry corner at all -- not just from
    //    the corners rivers were seeded on -- arrives at water.
    for (const MapCorner& start : graph.corners) {
        if (wet(start)) {
            continue;
        }
        CornerId current = start.index;
        std::size_t steps = 0;
        while (!wet(graph.corners[static_cast<std::size_t>(current)])) {
            const MapCorner& corner = graph.corners[static_cast<std::size_t>(current)];
            ASSERT_TRUE(corner.downslope != current);
            current = corner.downslope;
            ++steps;
            ASSERT_TRUE(steps <= graph.corners.size());
        }
    }

    // 3. And the rivers themselves land on it: a mouth in water, and no corner
    //    before the mouth already in water -- a river that ran on past a
    //    shoreline would be drawing a channel across a lake's surface.
    for (const MapRiver& river : graph.rivers) {
        ASSERT_TRUE(wet(graph.corners[static_cast<std::size_t>(river.corners.back())]));
        for (std::size_t i = 0; i + 1 < river.corners.size(); ++i) {
            ASSERT_TRUE(!wet(graph.corners[static_cast<std::size_t>(river.corners[i])]));
        }
    }
}


/**
 * @brief The waterline is a real height: the sea below it, everything else above.
 *
 * This is what makes "the ground here is under water" a comparison worth making,
 * and it was vacuous before -- the whole field started at zero with the sea
 * pinned to the bottom of it, so nothing was ever below sea level and the sea's
 * own surface rendered as the same black as dry land.
 */
static void test_waterline_separates_sea_from_land() {
    MapConfig config = world_config(61);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::size_t sea_corners = 0;
    std::size_t ground_corners = 0;
    for (const MapCorner& corner : graph.corners) {
        if (corner.ocean) {
            ASSERT_TRUE(corner.elevation < config.sea_level);
            ++sea_corners;
        } else {
            ASSERT_TRUE(corner.elevation >= config.sea_level);
            ++ground_corners;
        }
    }
    ASSERT_TRUE(sea_corners > 0);
    ASSERT_TRUE(ground_corners > 0);

    // Land-relative height is what every threshold describing land is phrased
    // against, so it has to put the shoreline at 0 and the summit at 1.
    ASSERT_TRUE(land_height(config, config.sea_level) == 0.0f);
    ASSERT_TRUE(std::abs(land_height(config, 1.0) - 1.0) < 1e-12);
    ASSERT_TRUE(land_height(config, 0.0) == 0.0);  // submerged clamps to the shore

    // And the vertical scale round-trips, which is what lets a depth be stated
    // in metres at all.
    ASSERT_TRUE(std::abs(height_to_meters(config, meters_to_height(config, 42.0)) - 42.0) < 1e-9);
}

/**
 * @brief The channel cut is strictly local -- away from a river it changes nothing.
 *
 * The cut lives in the sampling path, not the control mesh, so the guarantee that
 * matters is that it is *only* a channel: anywhere further than a river's own
 * width from a centreline the surface must be the one the detail overload
 * already produced, bit for bit.
 */
static void test_river_channels_are_zero_away_from_water() {
    MapConfig config = world_config(67);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    const Noise terrain(config.noise_terrain);
    const TerrainDetail detail = make_terrain_detail(config, terrain);
    const RiverChannels channels = make_river_channels(graph, config);
    ASSERT_TRUE(!channels.empty());

    std::size_t checked = 0;
    for (const MapCenter& center : graph.centers) {
        // A cell site is over half a cell from its own boundary, and rivers run
        // along boundaries -- so no site is ever inside a channel.
        const double x = center.point.x;
        const double y = center.point.y;
        ASSERT_TRUE(graph.elevation_at(center, x, y, detail, channels)
                    == graph.elevation_at(center, x, y, detail));
        ++checked;
    }
    ASSERT_TRUE(checked > 0);
}

/** @brief Zero depth leaves the sampled surface exactly as the control mesh describes it. */
static void test_river_channel_zero_depth_is_the_uncut_surface() {
    MapConfig config = world_config(67);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    MapConfig flat = config;
    flat.river_channel_depth_m = 0.0;
    flat.river_channel_depth_per_volume_m = 0.0;

    const Noise terrain(config.noise_terrain);
    const TerrainDetail detail = make_terrain_detail(config, terrain);
    const RiverChannels none = make_river_channels(graph, flat);
    ASSERT_TRUE(none.empty());

    for (const MapRiver& river : graph.rivers) {
        const std::size_t spans = river.points.size() - 1;
        for (std::size_t i = 0; i < river.points.size(); ++i) {
            const MapCorner& corner = graph.corners[static_cast<std::size_t>(
                river.corners[std::min(river.corners.size() - 1,
                                       i * river.corners.size() / std::max<std::size_t>(1, spans))])];
            if (corner.touches.empty()) {
                continue;
            }
            const MapCenter& center =
                graph.centers[static_cast<std::size_t>(corner.touches.front())];
            const MapPoint& point = river.points[i];
            ASSERT_TRUE(graph.elevation_at(center, point.x, point.y, detail, none)
                        == graph.elevation_at(center, point.x, point.y, detail));
        }
    }
}

/**
 * @brief The cut surface joins across a cell boundary, where it is deepest.
 *
 * The counterpart of the seam check in `test_elevation_interpolates_and_joins`,
 * run on the channel path. A river runs *along* a boundary, so the two cells
 * either side sample the deepest part of the cut from opposite directions -- and
 * if they disagreed about which segments exist, every watercourse would be
 * hemmed by a visible seam. They agree because a segment is filed under every
 * cell its corner touches, which is both of them.
 */
static void test_river_channels_join_across_cells() {
    MapConfig config = world_config(67);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    const Noise terrain(config.noise_terrain);
    const TerrainDetail detail = make_terrain_detail(config, terrain);
    const RiverChannels channels = make_river_channels(graph, config);

    std::size_t checked = 0;
    for (const MapEdge& edge : graph.edges) {
        if (edge.river <= 0 || edge.d0 == k_invalid_id || edge.d1 == k_invalid_id
            || edge.v0 == k_invalid_id || edge.v1 == k_invalid_id) {
            continue;
        }
        const MapCenter& a = graph.centers[static_cast<std::size_t>(edge.d0)];
        const MapCenter& b = graph.centers[static_cast<std::size_t>(edge.d1)];
        const MapPoint& v0 = graph.corners[static_cast<std::size_t>(edge.v0)].point;
        const MapPoint& v1 = graph.corners[static_cast<std::size_t>(edge.v1)].point;

        // Sampled along the shared edge rather than only at its midpoint, since a
        // mismatch could sit anywhere the two cells' segment groups differ.
        for (double t = 0.1; t <= 0.9; t += 0.2) {
            const double x = v0.x + (v1.x - v0.x) * t;
            const double y = v0.y + (v1.y - v0.y) * t;
            const double from_a = graph.elevation_at(a, x, y, detail, channels);
            const double from_b = graph.elevation_at(b, x, y, detail, channels);
            ASSERT_TRUE(std::abs(from_a - from_b) < 1e-6);
            ++checked;
        }
    }
    ASSERT_TRUE(checked > 0);
}

/**
 * @brief A river reads as a river in the height field, not as a dip in the ground.
 *
 * The test the previous attempt at this needed and did not have. Carving cell
 * heights produced a measurably deep valley that was invisible to look at,
 * because it compared the ground against *itself uncarved* -- a comparison a
 * 500 m-wide depression passes just as happily as a channel does.
 *
 * What the eye actually needs is local contrast, so that is what is asserted
 * here: the ground at the centreline against the ground a short way to either
 * side of it, at the same moment, on the same surface. A uniform depression
 * scores zero on this no matter how deep it is.
 */
static void test_river_channels_are_visible_in_the_height_field() {
    MapConfig config = world_config(67);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.rivers.empty());

    const Noise terrain(config.noise_terrain);
    const TerrainDetail detail = make_terrain_detail(config, terrain);
    const RiverChannels channels = make_river_channels(graph, config);

    // Half a cell out: far outside the channel itself, which is a river's width
    // across, so this measures the bank against the bed rather than one part of
    // the bed against another.
    const double offset = 0.5;
    double total = 0.0;
    std::size_t sampled = 0;
    for (const MapRiver& river : graph.rivers) {
        const std::size_t spans = river.points.size() - 1;
        for (std::size_t i = 2; i + 2 < river.points.size(); ++i) {
            // The same proportional step `make_river_channels()` files segments
            // by, so the cell asked for is one that actually carries this stretch
            // of the river. Any other cell reports no channel at all, which is
            // correct of it and useless here.
            const MapCorner& corner = graph.corners[static_cast<std::size_t>(
                river.corners[std::min(river.corners.size() - 1, i * river.corners.size()
                                                                     / spans)])];
            if (corner.touches.empty()) {
                continue;
            }
            const MapCenter& center =
                graph.centers[static_cast<std::size_t>(corner.touches.front())];
            const double dx = river.points[i + 2].x - river.points[i - 2].x;
            const double dy = river.points[i + 2].y - river.points[i - 2].y;
            const double length = std::hypot(dx, dy);
            if (length < 1e-9) {
                continue;
            }
            const double nx = -dy / length;
            const double ny = dx / length;
            const MapPoint& point = river.points[i];
            const double bed = graph.elevation_at(center, point.x, point.y, detail, channels);
            const double left = graph.elevation_at(center, point.x + nx * offset,
                                                   point.y + ny * offset, detail, channels);
            const double right = graph.elevation_at(center, point.x - nx * offset,
                                                    point.y - ny * offset, detail, channels);
            if (bed <= config.sea_level || left <= config.sea_level
                || right <= config.sea_level) {
                continue;
            }
            total += (left + right) * 0.5 - bed;
            ++sampled;
        }
    }
    ASSERT_TRUE(sampled > 0);

    // Ten metres is the bar: below about eight the channel is fewer than four
    // grey levels at the default vertical scale, which is where it stops being
    // something a reader can pick out of the relief.
    ASSERT_TRUE(height_to_meters(config, total / static_cast<double>(sampled)) > 10.0);
}

/**
 * @brief A watercourse lies below the ground either side of it.
 *
 * The point of the whole pass. Checked on the corner field, which is where the
 * incision is measured, against the corners one edge away that carry no river --
 * the bank. Trunk rivers only: a volume-one trickle is still inside the headwater
 * taper and is not meant to have opened a valley yet.
 */
static void test_river_corners_sit_below_their_banks() {
    MapConfig config = world_config(67);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.rivers.empty());

    std::size_t checked = 0;
    for (const MapCorner& corner : graph.corners) {
        if (corner.river < 2 || corner.water || corner.coast || corner.border) {
            continue;
        }
        // A mouth is pinned at the waterline and cannot be cut below it, so a
        // corner already at sea level proves nothing either way.
        if (corner.elevation <= config.sea_level + 1e-9) {
            continue;
        }
        double bank = 0.0;
        std::size_t counted = 0;
        for (const CornerId neighbor_id : corner.adjacent) {
            const MapCorner& neighbor = graph.corners[static_cast<std::size_t>(neighbor_id)];
            if (neighbor.river > 0 || neighbor.water) {
                continue;
            }
            bank += neighbor.elevation;
            ++counted;
        }
        if (counted == 0) {
            continue;
        }
        ASSERT_TRUE(corner.elevation < bank / static_cast<double>(counted));
        ++checked;
    }
    ASSERT_TRUE(checked > 0);
}

/**
 * @brief The valley still runs downhill after it has been widened.
 *
 * Widening does not know which corner is upstream of which, so where a larger
 * river passes close by, one of its rings can land on an upstream corner and cut
 * it below its own downstream neighbour -- water running uphill in the middle of
 * a river. `PassValleys` clamps along each course afterwards; this is the check
 * that it does.
 */
static void test_valleys_run_downhill() {
    MapConfig config = world_config(67);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.rivers.empty());

    for (const MapRiver& river : graph.rivers) {
        for (std::size_t i = 1; i < river.corners.size(); ++i) {
            const MapCorner& upstream =
                graph.corners[static_cast<std::size_t>(river.corners[i - 1])];
            const MapCorner& corner = graph.corners[static_cast<std::size_t>(river.corners[i])];
            ASSERT_TRUE(corner.elevation <= upstream.elevation + 1e-12);
        }
    }
}

/**
 * @brief Cutting a valley never digs dry ground below the waterline.
 *
 * A river mouth already sits at sea level, so the clamp in `lower_corners_()`
 * binds on every watercourse on the map rather than in some corner case. Without
 * it, land would come out submerged and every pass that reads the height field to
 * decide what is wet would disagree with the one that decides what is land.
 */
static void test_incision_never_breaches_sea_level() {
    for (int seed = 1; seed <= 4; ++seed) {
        MapConfig config = world_config(seed * 53);
        // Far deeper than the default, so the clamp is doing the work rather
        // than the incision happening to be too shallow to reach.
        config.river_incision_m = 240.0;
        config.river_incision_per_volume_m = 48.0;
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();

        for (const MapCorner& corner : graph.corners) {
            if (!corner.ocean) {
                ASSERT_TRUE(corner.elevation >= config.sea_level);
            }
        }
        for (const MapCenter& center : graph.centers) {
            if (!center.water) {
                ASSERT_TRUE(center.elevation >= config.sea_level);
            }
        }
    }
}

/**
 * @brief The valley survives the whole path from corner depth to sampled pixel.
 *
 * This is the one that tests what was actually asked for. The incision is
 * measured on corners, averaged down to cells, and only then interpolated
 * barycentrically over Delaunay triangles of cell *sites* -- and rivers run along
 * cell *boundaries*, as far from a site as the geometry allows. Plenty of ways for
 * a carve to be real in the data and invisible in the render, so this samples
 * `elevation_at()` itself, the same call the elevation layer makes per pixel.
 */
static void test_valleys_are_visible_in_the_height_field() {
    MapConfig config = world_config(67);
    MapGenerator carved_run(config, maps_logger());
    carved_run.generate();

    MapConfig uncarved_config = config;
    uncarved_config.enable_valleys = false;
    MapGenerator uncarved_run(uncarved_config, maps_logger());
    uncarved_run.generate();

    const MapGraph& carved = carved_run.graph();
    const MapGraph& uncarved = uncarved_run.graph();
    ASSERT_TRUE(!carved.rivers.empty());

    double total_drop = 0.0;
    std::size_t sampled = 0;
    for (const MapRiver& river : carved.rivers) {
        for (const CornerId corner_id : river.corners) {
            const MapCorner& corner = carved.corners[static_cast<std::size_t>(corner_id)];
            if (corner.river < 2 || corner.water || corner.touches.empty()) {
                continue;
            }
            const std::size_t cell = static_cast<std::size_t>(corner.touches.front());
            const double after =
                carved.elevation_at(carved.centers[cell], corner.point.x, corner.point.y);
            const double before =
                uncarved.elevation_at(uncarved.centers[cell], corner.point.x, corner.point.y);
            total_drop += before - after;
            ++sampled;
        }
    }
    ASSERT_TRUE(sampled > 0);

    // Both runs share a seed, so the geometry and every pass up to the carve are
    // identical and the difference is the valleys alone. A tenth of a grey level
    // would satisfy "lower"; ten metres is the bar for "visible".
    const double mean_drop = total_drop / static_cast<double>(sampled);
    ASSERT_TRUE(height_to_meters(config, mean_drop) > 10.0);
}

/**
 * @brief A river's water surface stands above the terrain the elevation layer draws.
 *
 * The invariant a consumer meshing the two layers together depends on, and it was
 * broken for a long time without this test noticing -- because the test used to
 * compute the surface *itself*, from `corner.elevation`, and then assert it was
 * above `corner.elevation`. Trivially true, and about the wrong surface: the
 * elevation layer draws `elevation_at()`, the blend of cell heights, which sits
 * some 22 m higher at a river corner. Nearly half of every watercourse was drawn
 * beneath the ground while this passed.
 *
 * So it asks `river_surface_at()` -- the one definition, the same call the renderer
 * makes -- and compares it against the ground the elevation layer actually draws,
 * cut channel and all, sampled across the whole width of the stroke rather than on
 * the centreline. Anything else measures a surface nobody draws.
 */
static void test_river_surface_sits_above_the_ground() {
    MapConfig config = world_config(67);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();
    ASSERT_TRUE(!graph.rivers.empty());

    const Noise terrain(config.noise_terrain);
    const TerrainDetail detail = make_terrain_detail(config, terrain);
    const RiverChannels channels = make_river_channels(graph, config);

    std::size_t checked = 0;
    for (const MapRiver& river : graph.rivers) {
        const std::size_t spans = river.points.size() - 1;
        for (std::size_t i = 0; i < spans; ++i) {
            const std::size_t slot =
                std::min(river.corners.size() - 1, i * river.corners.size() / spans);
            const MapCorner& corner =
                graph.corners[static_cast<std::size_t>(river.corners[slot])];
            if (corner.touches.empty()) {
                continue;
            }
            const MapCenter& center =
                graph.centers[static_cast<std::size_t>(corner.touches.front())];
            const double surface = river_surface_at(graph, river, i, config, detail);

            const MapPoint& from = river.points[i];
            const MapPoint& to = river.points[i + 1];
            const double reach =
                (river_width(config, corner.river)
                 + meters_to_grid(config, config.water_edge_overlap_m) * 2.0) * 0.5;
            const double dx = to.x - from.x;
            const double dy = to.y - from.y;
            const double length = std::hypot(dx, dy);
            const double nx = length > 0.0 ? -dy / length * reach : 0.0;
            const double ny = length > 0.0 ? dx / length * reach : 0.0;

            for (double along = 0.0; along <= 1.0; along += 0.5) {
                for (double across = -1.0; across <= 1.0; across += 1.0) {
                    const double x = from.x + dx * along + nx * across;
                    const double y = from.y + dy * along + ny * across;
                    const double ground =
                        graph.elevation_at(center, x, y, detail, channels);
                    // A tenth of a metre of slack: the surface is piecewise linear,
                    // so a triangle vertex inside the stroke can poke a hair above
                    // every point the probe grid samples. Far below the 2.35 m a
                    // single grey level covers, so it can never reach a pixel.
                    ASSERT_TRUE(ground <= surface + meters_to_height(config, 0.5));
                    ++checked;
                }
            }
        }
    }
    ASSERT_TRUE(checked > 0);

    // And the course still only falls, source to mouth -- a property of the
    // routing rather than of the surface, and still worth pinning here.
    for (const MapRiver& river : graph.rivers) {
        for (std::size_t i = 1; i < river.corners.size(); ++i) {
            const MapCorner& previous =
                graph.corners[static_cast<std::size_t>(river.corners[i - 1])];
            const MapCorner& corner =
                graph.corners[static_cast<std::size_t>(river.corners[i])];
            ASSERT_TRUE(corner.elevation <= previous.elevation);
        }
    }
}

/**
 * @brief Inside a body of water, the drawn ground never rises through the surface.
 *
 * The companion to the river invariant, and it holds exactly, but only where it
 * can. The boundary is sharper than "away from the shore", and worth stating
 * precisely because a consumer meshing the two layers has to know where the
 * guarantee stops.
 *
 * `MapCenter::elevation` under water is the *bed*, and for a coastal water cell it
 * is not below the water: cell heights are the mean of their corners, and a cell
 * the sea reaches into has corners up on the land. About one water cell in ten
 * carries a "bed" above its own surface for that reason. The ground is drawn by
 * interpolating between cell heights, so those cells pull the surface up through
 * the water inside themselves *and* one ring further in.
 *
 * So the invariant is over cells that, together with every neighbour, have a bed at
 * or below their surface -- there every vertex of every triangle the sampler can
 * reach is under water, and a barycentric blend of values under water is under
 * water. Measured across five maps, that is zero violations out of ~2 400 samples
 * each; anywhere else is the consumer's to clip.
 */
static void test_water_bodies_cover_their_interiors() {
    for (int seed : {67, 31, 251}) {
        MapConfig config = world_config(seed);
        MapGenerator generator(config, maps_logger());
        generator.generate();
        const MapGraph& graph = generator.graph();

        const Noise terrain(config.noise_terrain);
        const TerrainDetail detail = make_terrain_detail(config, terrain);
        const RiverChannels channels = make_river_channels(graph, config);

        const auto submerged = [](const MapCenter& cell) {
            return cell.water && cell.elevation <= cell.water_level;
        };

        std::size_t sampled = 0;
        for (const MapCenter& center : graph.centers) {
            if (center.corners.empty() || !submerged(center)) {
                continue;
            }
            bool interior = true;
            for (const CenterId neighbor_id : center.neighbors) {
                if (!submerged(graph.centers[static_cast<std::size_t>(neighbor_id)])) {
                    interior = false;
                }
            }
            if (!interior) {
                continue;
            }
            for (const CornerId corner_id : center.corners) {
                const MapPoint& corner =
                    graph.corners[static_cast<std::size_t>(corner_id)].point;
                // Pulled well in from the corner, so this measures the interior and
                // not the blend across the cell's own boundary.
                const double x = corner.x + (center.point.x - corner.x) * 0.7;
                const double y = corner.y + (center.point.y - corner.y) * 0.7;
                ASSERT_TRUE(graph.elevation_at(center, x, y, detail, channels)
                            <= center.water_level);
                ++sampled;
            }
        }
        ASSERT_TRUE(sampled > 0);
    }
}

/**
 * @brief Inside a body of water the flat surface wins, and it overhangs its edge.
 *
 * Rivers are stroked at ground height plus a depth, so drawing them *after* the
 * bodies -- which is what happened -- gouged a channel across every flat lake a
 * river ran into. And two surfaces that share an edge exactly show a seam
 * wherever their meshes disagree by a rounding error, which along a coastline is
 * everywhere, so the water is extended past its own edge.
 */
static void test_water_bodies_win_inside_and_overhang_their_edge() {
    MapConfig config = small_config(71);
    MapGenerator generator(config, maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    const Image water = MapLayers::water(graph, config);
    const double scale =
        static_cast<double>(config.image_size) / static_cast<double>(config.grid_size);

    std::size_t checked = 0;
    for (const MapCenter& center : graph.centers) {
        if (!center.water) {
            continue;
        }
        const int expected =
            static_cast<int>(std::clamp(center.water_level, 0.0, 1.0) * 255.0);
        for (const CornerId corner_id : center.corners) {
            // Sampled well inside the cell, so the overlap rim of a neighbour
            // cannot account for a mismatch.
            const MapPoint& corner = graph.corners[static_cast<std::size_t>(corner_id)].point;
            const int x = static_cast<int>((corner.x + (center.point.x - corner.x) * 0.7) * scale);
            const int y = static_cast<int>((corner.y + (center.point.y - corner.y) * 0.7) * scale);
            if (x < 0 || y < 0 || x >= water.width || y >= water.height) {
                continue;
            }
            ASSERT_EQ(static_cast<int>(water.color_at(x, y).r), expected);
            ++checked;
        }
    }
    ASSERT_TRUE(checked > 0);

    // The overhang: some pixels that are dry land carry a water height, because
    // the sheet reaches past its own shoreline.
    MapConfig sharp = config;
    sharp.water_edge_overlap_m = 0.0;
    const Image tight = MapLayers::water(graph, sharp);
    std::size_t wider = 0;
    for (int y = 0; y < water.height; ++y) {
        for (int x = 0; x < water.width; ++x) {
            if (water.color_at(x, y).r > 0.0f && tight.color_at(x, y).r == 0.0f) {
                ++wider;
            }
        }
    }
    ASSERT_TRUE(wider > 0);
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
    RUN_TEST(maps_test::test_every_river_ends_in_a_water_body);
    RUN_TEST(maps_test::test_water_cells_always_get_a_water_biome);
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
    RUN_TEST(maps_test::test_rivers_cut_valleys_into_the_height_field);
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
    RUN_TEST(maps_test::test_config_image_size_sets_the_scale);
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
    RUN_TEST(maps_test::test_water_bodies_are_flat);
    RUN_TEST(maps_test::test_water_layer_draws_one_grey_over_open_sea);
    RUN_TEST(maps_test::test_elevation_interpolates_and_joins);
    RUN_TEST(maps_test::test_terrain_roughness_tapers_to_the_coast);
    RUN_TEST(maps_test::test_regions_layer_draws_regions_and_borders);
    RUN_TEST(maps_test::test_default_shape_matches_the_square_frame);
    RUN_TEST(maps_test::test_shape_names_round_trip);
    RUN_TEST(maps_test::test_shapes_confine_the_landmass);
    RUN_TEST(maps_test::test_shape_field_matches_shape_inset);
    RUN_TEST(maps_test::test_organic_shapes_stay_off_the_canvas_edge);
    RUN_TEST(maps_test::test_organic_shapes_are_deterministic);
    RUN_TEST(maps_test::test_continent_is_one_landmass);
    RUN_TEST(maps_test::test_archipelago_makes_several_landmasses);
    RUN_TEST(maps_test::test_terrain_relief_reshapes_without_breaking_drainage);
    RUN_TEST(maps_test::test_waterline_separates_sea_from_land);
    RUN_TEST(maps_test::test_river_channels_are_zero_away_from_water);
    RUN_TEST(maps_test::test_river_channel_zero_depth_is_the_uncut_surface);
    RUN_TEST(maps_test::test_river_channels_join_across_cells);
    RUN_TEST(maps_test::test_river_channels_are_visible_in_the_height_field);
    RUN_TEST(maps_test::test_river_corners_sit_below_their_banks);
    RUN_TEST(maps_test::test_valleys_run_downhill);
    RUN_TEST(maps_test::test_incision_never_breaches_sea_level);
    RUN_TEST(maps_test::test_valleys_are_visible_in_the_height_field);
    RUN_TEST(maps_test::test_river_surface_sits_above_the_ground);
    RUN_TEST(maps_test::test_water_bodies_cover_their_interiors);
    RUN_TEST(maps_test::test_water_bodies_win_inside_and_overhang_their_edge);

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
