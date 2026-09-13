/**
 * @file map_generator.cpp
 * @brief Standalone tool that generates a random map and writes it out as two
 *        PNG renders and one YAML document.
 *
 * Build target `coopa_mapgen`. With no arguments it draws a seed from the
 * system entropy source and prints it, so every run produces a different map
 * that can still be reproduced afterwards with `--seed=`.
 *
 * @code
 * ./build/coopa_mapgen                          # random map, written to ./map_out.*
 * ./build/coopa_mapgen --seed=251 --out=/tmp/m  # reproducible, written to /tmp/m.*
 * @endcode
 */

#include <cstdlib>
#include <iostream>
#include <random>
#include <string>
#include <string_view>

#include <coopa/debug/logger.h>
#include <coopa/maps/image_writer.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_renderer.h>
#include <coopa/maps/map_yaml.h>

namespace {

/** @brief Grid size the library's default noise frequency is tuned for. */
constexpr double k_reference_grid_size = 40.0;

/** @brief Prints the accepted flags and their defaults. */
void print_usage() {
    std::cout
        << "usage: coopa_mapgen [options]\n"
        << "\n"
        << "  --seed=N         master seed; random each run when omitted\n"
        << "  --grid-size=N    cells per axis (default 80)\n"
        << "  --image-size=N   render size in pixels, square (default 2048)\n"
        << "  --rivers=N       river sources to attempt (default 55)\n"
        << "  --towns=N        settlements to place (default 28)\n"
        << "  --countries=N    nations to carve out (default 5)\n"
        << "  --regions=N      provinces per nation (default 3)\n"
        << "  --no-regions     skip political geography entirely\n"
        << "  --no-landmarks   skip notable places\n"
        << "  --no-subdivide   draw straight cell boundaries instead of wobbled ones\n"
        << "  --out=PATH       output prefix (default \"map_out\")\n"
        << "  --help           show this message\n"
        << "\n"
        << "writes PATH_biomes.png, PATH_elevation.png and PATH.yaml\n";
}

/**
 * @brief Matches `--name=value` and extracts the value.
 * @param argument The raw command-line argument.
 * @param name The flag name, without the leading dashes.
 * @param out_value Receives the value when the argument matches.
 * @return True if the argument is this flag.
 */
bool match_option(std::string_view argument, std::string_view name, std::string_view& out_value) {
    const std::string prefix = "--" + std::string(name) + "=";
    if (argument.rfind(prefix, 0) != 0) {
        return false;
    }
    out_value = argument.substr(prefix.size());
    return true;
}

} // namespace

/**
 * @brief Parses the command line, generates a map, and writes the three output files.
 * @param argc Argument count.
 * @param argv Argument values.
 * @return 0 on success, 1 on a bad argument or a failed write.
 */
int main(int argc, char** argv) {
    coopa::maps::MapConfig config;
    // Large enough that climate bands, several nations and a spread of landmarks
    // all have room to appear; the YAML lands around 15 MB.
    config.grid_size = 80;
    config.image_size = 2048;
    // Scaled to the scene rather than left at the library defaults, which are
    // sized for the much smaller map a consumer gets from a bare MapConfig.
    config.towns.town_count = 28;
    config.river_count = 55;
    config.landmarks.max_natural = 70;
    config.landmarks.max_abandoned = 45;

    std::string out_prefix = "map_out";
    bool seed_given = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        std::string_view value;

        if (argument == "--help" || argument == "-h") {
            print_usage();
            return 0;
        } else if (argument == "--no-subdivide") {
            config.subdivide_noisy_edges = false;
        } else if (argument == "--no-regions") {
            config.enable_regions = false;
            config.show_regions = false;
        } else if (argument == "--no-landmarks") {
            config.enable_landmarks = false;
        } else if (match_option(argument, "seed", value)) {
            config.seed = std::atoi(std::string(value).c_str());
            seed_given = true;
        } else if (match_option(argument, "grid-size", value)) {
            config.grid_size = std::atoi(std::string(value).c_str());
        } else if (match_option(argument, "image-size", value)) {
            config.image_size = std::atoi(std::string(value).c_str());
        } else if (match_option(argument, "rivers", value)) {
            config.river_count = std::atoi(std::string(value).c_str());
        } else if (match_option(argument, "towns", value)) {
            config.towns.town_count = std::atoi(std::string(value).c_str());
        } else if (match_option(argument, "countries", value)) {
            config.regions.country_count = std::atoi(std::string(value).c_str());
        } else if (match_option(argument, "regions", value)) {
            config.regions.regions_per_country = std::atoi(std::string(value).c_str());
        } else if (match_option(argument, "out", value)) {
            out_prefix = std::string(value);
        } else {
            std::cerr << "coopa_mapgen: unrecognised argument '" << argument << "'\n\n";
            print_usage();
            return 1;
        }
    }

    if (config.grid_size < 2 || config.image_size < 16) {
        std::cerr << "coopa_mapgen: --grid-size must be at least 2 and --image-size at least 16\n";
        return 1;
    }

    if (!seed_given) {
        std::random_device entropy;
        config.seed = static_cast<int>(entropy() & 0x7fffffffu);
    }
    // The island field is seeded from the master seed too, so one --seed value
    // reproduces the whole map rather than just the pass ordering.
    config.noise_island.seed = config.seed;
    config.noise_temperature.seed = config.seed + 1;

    // Scale the island field with the grid so --grid-size controls detail, not
    // the size of the world. The frequency is in grid units, so holding it fixed
    // while enlarging the map shrinks every landmass instead of resolving it more
    // finely -- a big map comes out as an archipelago of the same small islands.
    config.noise_island.frequency *= k_reference_grid_size / static_cast<double>(config.grid_size);

    coopa::debug::Logger logger("mapgen");
    coopa::maps::MapGenerator generator(config, logger);
    generator.generate();

    const coopa::maps::BiomePalette palette;
    const std::string biomes_path = out_prefix + "_biomes.png";
    const std::string elevation_path = out_prefix + "_elevation.png";
    const std::string yaml_path = out_prefix + ".yaml";

    if (!coopa::maps::write_png(biomes_path,
                                coopa::maps::BiomeRenderer::render(generator.graph(), config, palette))) {
        std::cerr << "coopa_mapgen: failed to write " << biomes_path << "\n";
        return 1;
    }
    if (!coopa::maps::write_png(elevation_path,
                                coopa::maps::ElevationRenderer::render(generator.graph(), config, palette))) {
        std::cerr << "coopa_mapgen: failed to write " << elevation_path << "\n";
        return 1;
    }

    try {
        coopa::maps::save_map(generator.graph(), config, yaml_path);
    } catch (const std::exception& e) {
        std::cerr << "coopa_mapgen: failed to write " << yaml_path << ": " << e.what() << "\n";
        return 1;
    }

    int population = 0;
    for (const coopa::maps::MapTown& town : generator.graph().towns) {
        population += town.population;
    }

    std::cout << "\nseed:       " << config.seed << "  (rerun with --seed=" << config.seed << ")\n"
              << "cells:      " << generator.graph().centers.size() << "\n"
              << "countries:  " << generator.graph().countries.size()
              << "  regions: " << generator.graph().regions.size() << "\n"
              << "towns:      " << generator.graph().towns.size()
              << "  population: " << population << "\n"
              << "landmarks:  " << generator.graph().landmarks.size() << "\n"
              << "written:   " << biomes_path << "\n"
              << "           " << elevation_path << "\n"
              << "           " << yaml_path << std::endl;
    return 0;
}
