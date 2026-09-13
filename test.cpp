/**
 * @file test.cpp
 * @brief mapcoopa's test suite -- 39 cases over the generator, its passes, the
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
#include <vector>

#include <root_directory.h>

#include <coopa/debug/logger.h>
#include <coopa/maps/biome.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_renderer.h>
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
    const TownConfig& towns = generator.config().towns;

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
            ASSERT_TRUE(dx * dx + dy * dy >= towns.min_spacing * towns.min_spacing);
        }
    }
}

static void test_buildings_lie_inside_their_cell() {
    MapGenerator generator(small_config(), maps_logger());
    generator.generate();
    const MapGraph& graph = generator.graph();

    std::size_t total_buildings = 0;
    for (const MapTown& town : graph.towns) {
        const MapCenter& center = graph.centers[static_cast<std::size_t>(town.center)];
        std::vector<MapPoint> polygon;
        for (const CornerId corner_id : center.corners) {
            polygon.push_back(graph.corners[static_cast<std::size_t>(corner_id)].point);
        }

        for (const MapBuilding& building : town.buildings) {
            ++total_buildings;
            ASSERT_TRUE(building.width > 0.0 && building.height > 0.0);
            // The *rotated* corners, not an axis-aligned box. Checking the box
            // would pass even when a building drawn at its stated yaw hangs out
            // over the cell boundary.
            for (const MapPoint& corner : building_corners(building)) {
                ASSERT_TRUE(point_in_polygon(polygon, corner));
            }
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

    const double reach = towns.street_offset + towns.building_size_max;
    std::size_t total = 0;
    std::size_t fronting = 0;

    for (const MapTown& town : graph.towns) {
        const MapCenter& center = graph.centers[static_cast<std::size_t>(town.center)];

        // The streets the pass would have derived: the approach from the cell's
        // site out to each road or river edge it borders.
        std::vector<MapPoint> street_ends;
        for (const EdgeId edge_id : center.borders) {
            const MapEdge& edge = graph.edges[static_cast<std::size_t>(edge_id)];
            if (edge.road || edge.river > 0) {
                street_ends.push_back(edge.midpoint);
            }
        }
        if (street_ends.empty()) {
            continue;
        }

        for (const MapBuilding& building : town.buildings) {
            ++total;
            for (const MapPoint& end : street_ends) {
                const double dx = end.x - center.point.x;
                const double dy = end.y - center.point.y;
                const double length_squared = dx * dx + dy * dy;
                if (length_squared == 0.0) {
                    continue;
                }
                double t = ((building.point.x - center.point.x) * dx
                          + (building.point.y - center.point.y) * dy) / length_squared;
                t = std::clamp(t, 0.0, 1.0);
                const double nearest_x = center.point.x + t * dx;
                const double nearest_y = center.point.y + t * dy;
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

    const Image biomes = BiomeRenderer::render(generator.graph(), config);
    ASSERT_EQ(biomes.width, config.image_size);
    ASSERT_EQ(biomes.height, config.image_size);
    ASSERT_EQ(biomes.pixels.size(),
              static_cast<std::size_t>(config.image_size) * config.image_size * 3);

    const Image elevation = ElevationRenderer::render(generator.graph(), config);
    ASSERT_EQ(elevation.pixels.size(), biomes.pixels.size());

    // Anything other than a single flat colour proves the cells actually drew.
    bool varied = false;
    for (std::size_t i = 3; i < biomes.pixels.size() && !varied; i += 3) {
        varied = biomes.pixels[i] != biomes.pixels[0];
    }
    ASSERT_TRUE(varied);
}

static void test_coverage_mask_darkens_each_pixel_once() {
    // Two strokes that overlap along a run and then cross. Under per-stroke
    // darkening the shared pixels drop twice over, plus three times again from
    // the square brush covering each pixel on consecutive Bresenham steps.
    const int size = 64;
    const int amount = 10;
    const unsigned char base = 200;

    Image image;
    image.reset(size, size, 3, glm::vec3(base, base, base));

    CoverageMask mask;
    mask.reset(size, size);
    mark_line(mask, 8, 32, 56, 32, 1);   // horizontal
    mark_line(mask, 32, 8, 32, 56, 1);   // vertical, crosses the first
    mark_line(mask, 8, 32, 56, 32, 1);   // the horizontal again, start to finish
    darken_masked(image, mask, amount);

    std::size_t darkened = 0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * size + x) * 3;
            const int value = image.pixels[index];
            const bool covered = mask.at(x, y);
            ASSERT_EQ(value, covered ? base - amount : static_cast<int>(base));
            if (covered) {
                ++darkened;
            }
        }
    }
    ASSERT_TRUE(darkened > 0);
    // The crossing pixel is the one a per-stroke implementation gets most wrong.
    ASSERT_TRUE(mask.at(32, 32));
    ASSERT_EQ(static_cast<int>(image.pixels[(32 * static_cast<std::size_t>(size) + 32) * 3]),
              base - amount);
}

static void test_elevation_rivers_darken_exactly_once() {
    // Straight cell boundaries in both renders: the elevation pass runs before
    // the river pass, so turning rivers off cannot change any cell's height, and
    // with subdivision off the outlines are purely geometric and so cannot
    // change either. The two images therefore differ only by the river layer.
    MapConfig with_rivers_config = small_config();
    with_rivers_config.subdivide_noisy_edges = false;

    MapConfig without_rivers_config = with_rivers_config;
    without_rivers_config.enable_rivers = false;

    MapGenerator with_rivers(with_rivers_config, maps_logger());
    MapGenerator without_rivers(without_rivers_config, maps_logger());
    with_rivers.generate();
    without_rivers.generate();

    const Image drawn = ElevationRenderer::render(with_rivers.graph(), with_rivers_config);
    const Image base = ElevationRenderer::render(without_rivers.graph(), without_rivers_config);
    ASSERT_EQ(drawn.pixels.size(), base.pixels.size());

    std::size_t differing = 0;
    for (std::size_t i = 0; i < base.pixels.size(); ++i) {
        const int base_value = base.pixels[i];
        const int drawn_value = drawn.pixels[i];
        if (drawn_value == base_value) {
            continue;
        }
        ++differing;
        // Exactly one dip, never a multiple of it. The std::max arm covers
        // terrain already darker than the dip, which clamps at zero.
        ASSERT_EQ(drawn_value, std::max(0, base_value - ElevationRenderer::k_river_darken));
    }
    // Guards against the assertion above passing because nothing was drawn.
    ASSERT_TRUE(differing > 0);
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
                river_width(config, edge.river) * 0.5 + config.towns.water_clearance;

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

    double smallest = config.towns.building_size_max;
    double largest = config.towns.building_size_min;
    std::size_t count = 0;
    for (const MapTown& town : generator.graph().towns) {
        for (const MapBuilding& building : town.buildings) {
            ASSERT_TRUE(building.width >= config.towns.building_size_min - 1e-9);
            ASSERT_TRUE(building.width <= config.towns.building_size_max + 1e-9);
            ASSERT_TRUE(std::abs(building.width - building.height) < 1e-9);
            smallest = std::min(smallest, building.width);
            largest = std::max(largest, building.width);
            ++count;
        }
    }
    ASSERT_TRUE(count > 0);
    // Actually varied, not one size repeated -- the range has to be used.
    ASSERT_TRUE(largest - smallest > (config.towns.building_size_max
                                      - config.towns.building_size_min) * 0.5);
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

static void test_yaml_round_trip_renders_identically() {
    MapConfig config = small_config(9);
    MapGenerator generator(config, maps_logger());
    generator.generate();

    const std::string path = "test_temp_map_render.yaml";
    save_map(generator.graph(), config, path);

    MapGraph loaded;
    MapConfig loaded_config;
    ASSERT_TRUE(load_map(path, loaded, loaded_config));
    std::remove(path.c_str());

    // The serialiser drops an edge's noisy path when subdivision left it at two
    // points; this is the check that reconstructing it puts the same pixels down.
    const Image before = BiomeRenderer::render(generator.graph(), config);
    const Image after = BiomeRenderer::render(loaded, loaded_config);
    ASSERT_EQ(before.pixels.size(), after.pixels.size());
    ASSERT_TRUE(before.pixels == after.pixels);
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
        const std::string row = "| ![](docs/legend/" + slug + ".svg) | ";
        const std::string tail = " | `" + slug + "` | `" + hex + "` | "
                               + std::to_string(static_cast<int>(color.r)) + ", "
                               + std::to_string(static_cast<int>(color.g)) + ", "
                               + std::to_string(static_cast<int>(color.b)) + " |";

        const std::size_t at = readme.find(row);
        ASSERT_TRUE(at != std::string::npos);
        ASSERT_TRUE(readme.find(tail, at) != std::string::npos);
        ASSERT_TRUE(readme.find(tail, at) < readme.find('\n', at));

        // And the swatch itself is that colour, not merely a file of the right name.
        const std::string swatch = read_file(root + "/docs/legend/" + slug + ".svg");
        ASSERT_TRUE(!swatch.empty());
        ASSERT_TRUE(swatch.find("fill=\"" + hex + "\"") != std::string::npos);
        ++rows_checked;
    }
    ASSERT_EQ(static_cast<std::size_t>(rows_checked), k_biome_count);

    // The overlay half of the legend, keyed by the swatch each row points at.
    const std::pair<const char*, glm::vec3> overlays[] = {
        {"river", palette.river_color},
        {"road-casing", palette.road_casing_color},
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
        const std::string row = "| ![](docs/legend/" + std::string(overlay.first) + ".svg) |";
        const std::size_t at = readme.find(row);
        ASSERT_TRUE(at != std::string::npos);
        ASSERT_TRUE(readme.find("`" + hex + "`", at) < readme.find('\n', at));

        const std::string swatch = read_file(root + "/docs/legend/" + overlay.first + ".svg");
        ASSERT_TRUE(!swatch.empty());
        ASSERT_TRUE(swatch.find("fill=\"" + hex + "\"") != std::string::npos);
    }

    // The tint caveat is the one thing a reader can check against a render and
    // find false, so it may not quietly disappear either.
    ASSERT_TRUE(readme.find("untinted") != std::string::npos);
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
    RUN_TEST(maps_test::test_coverage_mask_darkens_each_pixel_once);
    RUN_TEST(maps_test::test_elevation_rivers_darken_exactly_once);
    RUN_TEST(maps_test::test_yaml_round_trip_preserves_the_graph);
    RUN_TEST(maps_test::test_yaml_round_trip_renders_identically);
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
