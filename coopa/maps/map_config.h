/**
 * @file map_config.h
 * @brief Every knob that shapes a generated map: grid density, the island
 *        noise field, per-pass parameters, town placement, and the render palette.
 */

#ifndef COOPA_MAPS_MAP_CONFIG_H
#define COOPA_MAPS_MAP_CONFIG_H

#include <array>
#include <cstddef>

#include <glm/glm.hpp>
#include <noise/FastNoiseLite.h>

#include <coopa/maps/biome.h>

namespace coopa {
namespace maps {

/**
 * @struct NoiseConfig
 * @brief Parameters for the fractal noise field that decides land from water.
 *
 * The fields map one-to-one onto FastNoiseLite's setters. They are typed
 * against FastNoiseLite's own enums rather than the raw `int`s the original
 * configuration used -- those were `static_cast` at the point of use, so
 * `type = 0` silently meant OpenSimplex2 and nothing in the file said so.
 */
struct NoiseConfig {
    /** @brief Seed for the noise field; independent of `MapConfig::seed`. */
    int seed = 251;
    /** @brief Base frequency -- larger values give smaller, more numerous islands. */
    double frequency = 0.08;
    /** @brief Which noise algorithm generates the field. */
    FastNoiseLite::NoiseType type = FastNoiseLite::NoiseType_OpenSimplex2;
    /** @brief How octaves are combined. */
    FastNoiseLite::FractalType fractal_type = FastNoiseLite::FractalType_FBm;
    /** @brief Number of octaves summed into the field. */
    int octaves = 5;
    /** @brief Frequency multiplier between successive octaves. */
    double lacunarity = 2.0;
    /** @brief Amplitude multiplier between successive octaves. */
    double gain = 0.5;
    /** @brief How strongly an octave's amplitude is weighted by the previous one. */
    double weighted_strength = 0.45;
};

/**
 * @enum TownTier
 * @brief The size class awarded to a settlement, best site first.
 */
enum class TownTier {
    Capital,
    Town,
    Village
};

/** @brief Maps a town tier to its serialisation name. */
inline std::string_view town_tier_name(TownTier tier) {
    switch (tier) {
        case TownTier::Capital: return "capital";
        case TownTier::Town:    return "town";
        case TownTier::Village: return "village";
    }
    return "village";
}

/** @brief Resolves a serialisation name back to a town tier, defaulting to `Village`. */
inline TownTier town_tier_from_name(std::string_view name) {
    if (name == "capital") return TownTier::Capital;
    if (name == "town")    return TownTier::Town;
    return TownTier::Village;
}

/**
 * @enum RoadClass
 * @brief How much traffic a road carries, and therefore what kind of road it is.
 *
 * Assigned from the number of routes the network pass pushed along an edge, not
 * chosen: a highway is a highway because everything goes that way. Ordered
 * least to most travelled so a comparison against a tier means what it reads
 * like, and `None` is first so a default-constructed edge carries no road.
 */
enum class RoadClass {
    None,
    Trail,
    Road,
    Highway
};

/** @brief Number of distinct `RoadClass` values, `None` included. */
inline constexpr std::size_t k_road_class_count = 4;

/**
 * @brief Maps a road class to its serialisation name.
 *
 * These `snake_case` names are the stable on-disk identity of a class, exactly
 * as `biome_name()` is for a biome.
 *
 * @param road_class The class to name.
 * @return A `snake_case` identifier, e.g. `"highway"`.
 */
inline std::string_view road_class_name(RoadClass road_class) {
    switch (road_class) {
        case RoadClass::None:    return "none";
        case RoadClass::Trail:   return "trail";
        case RoadClass::Road:    return "road";
        case RoadClass::Highway: return "highway";
    }
    return "none";
}

/**
 * @brief Resolves a serialisation name back to a road class.
 *
 * Unknown names become `Trail` rather than `None`: the name was written because
 * a road was there, and downgrading it to the least travelled class loses less
 * than erasing it.
 *
 * @param name A name previously produced by `road_class_name()`.
 * @return The matching class, or `RoadClass::Trail` if the name is unknown.
 */
inline RoadClass road_class_from_name(std::string_view name) {
    if (name == "none")    return RoadClass::None;
    if (name == "road")    return RoadClass::Road;
    if (name == "highway") return RoadClass::Highway;
    return RoadClass::Trail;
}

/**
 * @struct TownConfig
 * @brief Controls settlement placement and the building footprints packed into each one.
 */
struct TownConfig {
    /** @brief Upper bound on settlements placed; fewer appear if the map lacks room. */
    int town_count = 12;
    /** @brief Minimum distance in grid units between two settlements. */
    double min_spacing = 4.0;
    /** @brief Rank cutoff: the first `capital_count` sites become capitals. */
    int capital_count = 1;
    /** @brief Rank cutoff: the sites after the capitals, up to this many, become towns. */
    int town_tier_count = 4;
    /** @brief Elevation above which a site is penalised for being hard to reach. */
    double elevation_penalty_start = 0.6;
    /** @brief Score subtracted per unit of elevation above `elevation_penalty_start`. */
    double elevation_penalty_scale = 2.0;
    /** @brief Score added when a site borders the ocean. */
    double coast_bonus = 0.35;
    /** @brief Score added when a site borders a river. */
    double river_bonus = 0.45;
    /** @brief Score added when a site borders a road. */
    double road_bonus = 0.2;
    /**
     * @brief Score added for a roomy cell, scaled by its area against the mean.
     *
     * Without it the highest-scoring site can be a cramped sliver, and the
     * capital ends up with fewer buildings than the towns beneath it -- a
     * settlement cannot grow past what its cell will hold.
     */
    double area_bonus = 0.5;
    /** @brief Magnitude of the seeded random jitter applied to each site score. */
    double score_jitter = 0.15;
    /** @brief Buildings attempted in a capital; lesser tiers get a fraction of this. */
    int buildings_per_town = 30;
    /** @brief Smallest side length of a square building footprint, in grid units. */
    double building_size_min = 0.09;
    /** @brief Largest side length of a square building footprint, in grid units. */
    double building_size_max = 0.16;
    /** @brief Fraction of `buildings_per_town` a town-tier settlement receives. */
    double town_building_scale = 0.7;
    /** @brief Fraction of `buildings_per_town` a village receives. */
    double village_building_scale = 0.4;
    /**
     * @brief Clear ground kept between a building and any water, in grid units.
     *
     * Added to a river's half-width, and applied on its own along a boundary
     * shared with a lake or the sea. Without it buildings stand in the channel:
     * rivers are used as streets, so the packer marches plots straight at them.
     */
    double water_clearance = 0.04;
    /** @brief Fewest occupants in a household. */
    int household_size_min = 3;
    /** @brief Most occupants in a household. */
    int household_size_max = 7;
    /** @brief Population density multiplier applied to a capital. */
    double capital_density = 1.6;
    /** @brief Population density multiplier applied to a town. */
    double town_density = 1.2;
    /** @brief Perpendicular distance from a street's centreline to a plot centre. */
    double street_offset = 0.10;
    /** @brief Spacing along a street between successive plots. */
    double street_spacing = 0.16;
    /** @brief Random offset applied to a street-front plot, on both axes. */
    double position_jitter = 0.02;
    /** @brief Radians of yaw wobble about a street's bearing. */
    double rotation_jitter = 0.25;
    /** @brief Rejection draws before interior infill gives up on a cramped cell. */
    int infill_attempts = 200;
};

/**
 * @struct RegionConfig
 * @brief Controls how the land is divided into countries and regions.
 */
struct RegionConfig {
    /** @brief Nations to place, terrain permitting. */
    int country_count = 5;
    /** @brief Provinces carved out of each country. */
    int regions_per_country = 3;
    /** @brief Minimum distance in grid units between two country seeds. */
    double min_country_spacing = 8.0;
    /**
     * @brief How strongly a height difference resists expansion.
     *
     * The reason borders land on ridges rather than cutting across them: a
     * claim spreads cheaply along a valley and dearly over a mountain.
     */
    double elevation_cost = 6.0;
    /**
     * @brief Cost of claiming across water.
     *
     * High but finite. Infinite would make every island its own nation; this
     * lets a country hold both banks of a strait while still stopping at an ocean.
     */
    double water_crossing_cost = 25.0;
};

/**
 * @struct RoadConfig
 * @brief Controls where roads run, how busy they get, and how they are drawn.
 *
 * Every cost here is a multiplier on the distance actually travelled, never a
 * flat number of hops, so the network does not change character when
 * `grid_size` does -- the same reasoning that made river widths physical
 * rather than pixel counts.
 */
struct RoadConfig {
    /** @brief Anchors the network is routed between; clamped to the land available. */
    int hub_count = 32;
    /**
     * @brief Minimum distance in grid units between two hubs.
     *
     * Matches `TownConfig::min_spacing` on purpose. Hubs are scored the way
     * settlement sites are, so spacing them the same way is what puts a road
     * through most of the towns the next pass but one goes on to place.
     */
    double hub_min_spacing = 4.0;
    /**
     * @brief Cost per unit of elevation climbed between two cells.
     *
     * The reason mountain roads switchback. Climbing straight up is dear and
     * traversing a slope is not, so a route crosses the contour at a shallow
     * angle and doubles back -- which is what the old contour-flooding pass
     * drew directly, now arrived at for a reason rather than by construction.
     */
    double slope_cost = 9.0;
    /** @brief Cost per unit of absolute height; pushes a crossing onto the saddle. */
    double elevation_cost = 1.5;
    /** @brief Scales `1 - biome_habitability()` into a penalty for rough ground. */
    double rough_ground_cost = 2.0;
    /**
     * @brief Cost of crossing a volume-zero stream; a ford, not yet a bridge.
     *
     * Deliberately small. A headwater stream is a wet crossing and little more,
     * and pricing it like a bridge sends every route detouring to a river's
     * source -- which reads as a network that is frightened of water rather
     * than one that has learned where to cross it.
     */
    double ford_cost = 0.6;
    /** @brief Added to `ford_cost` per unit of river volume, so wide water is dear to span. */
    double bridge_cost_per_volume = 0.35;
    /**
     * @brief Cost of stepping into a lake or ocean cell.
     *
     * High but finite, and bounded in extent by `max_water_span`. Infinite
     * would strand every island with its own closed network; this lets a
     * causeway hop a narrow strait while leaving the open sea uncrossable --
     * the same bargain `RegionConfig::water_crossing_cost` already strikes for
     * a country holding both banks.
     */
    double water_crossing_cost = 30.0;
    /** @brief Consecutive water cells a crossing may span; a longer run is impassable. */
    int max_water_span = 2;
    /**
     * @brief Cost multiplier on an edge some earlier route already runs along.
     *
     * The whole reason the network has a shape rather than being a fan of
     * independent optimal paths. Routes are laid one at a time and each sees the
     * ground already built on as cheap, so a later route bends to join an
     * existing road instead of cutting its own parallel line a cell away --
     * which is how a trunk road forms, and why traffic concentrates enough for
     * a hierarchy to be worth reading off.
     */
    double reuse_discount = 0.40;
    /**
     * @brief Share of all routes that must pass along an edge for it to be a highway.
     *
     * A share and not a count, because a count cannot mean anything fixed: with
     * `n` hubs every pair is routed, so a dead-end spur carries exactly `n - 1`
     * routes and a trunk carries a large fraction of all `n(n-1)/2` of them.
     * Thresholds in absolute traffic would reclassify the entire map the moment
     * `hub_count` moved -- at 24 hubs a flat cutoff of 12 made highways of
     * 308 edges out of 322.
     */
    double highway_traffic_share = 0.30;
    /** @brief Share of all routes that makes an edge a road; below it, a trail. */
    double road_traffic_share = 0.10;
    /** @brief Chaikin corner-cutting passes applied to each road run. */
    int smoothing_iterations = 2;
};

/**
 * @struct LandmarkConfig
 * @brief Controls which notable places are found and how many.
 */
struct LandmarkConfig {
    /** @brief Cap on terrain features recorded. */
    int max_natural = 40;
    /** @brief Cap on ruins and abandoned works recorded. */
    int max_abandoned = 25;
    /** @brief Elevation at or above which a local summit counts as a peak. */
    double peak_elevation = 0.62;
    /** @brief Height difference across a river edge that makes it a waterfall. */
    double waterfall_drop = 0.055;
    /** @brief Elevation above which arid ground can be cut into a gorge. */
    double canyon_elevation = 0.45;
    /** @brief Connected fresh-water cells needed before a lake is "great". */
    std::size_t great_lake_cells = 8;
    /** @brief Distance a ruin must keep from any living settlement, in grid units. */
    double town_clearance = 2.5;
    /** @brief Numerator of the share of the natural budget any one kind may take. */
    int kind_share_numerator = 1;
    /** @brief Denominator of that share; 1/5 by default. */
    int kind_share_denominator = 5;
    /** @brief Numerator of the ocean-neighbour fraction that makes a cell a cape. */
    int cape_ocean_ratio_numerator = 3;
    /** @brief Denominator of the ocean-neighbour fraction that makes a cell a cape. */
    int cape_ocean_ratio_denominator = 2;
};

/**
 * @struct MapConfig
 * @brief The complete description of a map to generate.
 *
 * A `MapConfig` plus its `seed` fully determines the output: every pass draws
 * from a generator seeded from this struct, so two runs with equal configs
 * produce byte-identical maps. That was not true of the original, whose
 * noisy-edge pass seeded itself from the wall clock.
 */
struct MapConfig {
    // --- Sampling ---

    /** @brief Cells per axis; the point set is `(grid_size + 1)^2` plus a boundary ring. */
    int grid_size = 50;
    /** @brief How far a grid point may wander from its lattice position, in grid units. */
    double jitter = 0.5;
    /** @brief Master seed; every pass derives its generator from this. */
    int seed = 251;
    /** @brief Width of the band around the map edge whose cells are forced to border. */
    double border_length = 1.0;

    // --- Rendering ---

    /** @brief Side length in pixels of the square PNG renders. */
    int image_size = 1024;
    /** @brief Tint cells by the region that claims them, so borders are visible. */
    bool show_regions = true;
    /** @brief How strongly the region tint is mixed over the biome colour, in `[0, 1]`. */
    float region_tint = 0.13f;

    // --- Pass parameters ---

    /** @brief The island noise field sampled by the water pass. */
    NoiseConfig noise_island;
    /** @brief The variation field that keeps isotherms from running straight. */
    NoiseConfig noise_temperature{1733, 0.035};
    /**
     * @brief How much a full unit of elevation cools the air.
     *
     * At the default a mountain top is roughly half a climate band colder than
     * the lowland on the same latitude, which is what puts snow on equatorial
     * peaks.
     */
    double temperature_lapse_rate = 0.40;
    /**
     * @brief Shapes the latitude curve between pole and equator.
     *
     * A straight ramp (1.0) makes most of the map cold once the lapse rate is
     * subtracted -- the mean latitude term is only 0.5 before altitude takes its
     * cut. Values above 1 hold the temperate band wide across the middle and
     * push the drop out toward narrow polar caps, which is how a real world is
     * distributed.
     */
    double temperature_falloff = 1.7;
    /**
     * @brief Laplacian relaxation passes applied to the height field.
     *
     * Distance-from-coast elevation is terraced: a quarter of the full height
     * range could fall between two adjacent cells, which reads as facets rather
     * than terrain. Each pass pulls a corner toward the mean of its neighbours.
     */
    int elevation_smoothing_iterations = 6;
    /** @brief How far toward the neighbour mean each smoothing pass moves a corner. */
    double elevation_smoothing_strength = 0.5;
    /** @brief Noise value above which a corner is water. */
    double threshold_water = 0.3;
    /** @brief A cell becomes water once more than this many of its corners are. */
    int threshold_water_count = 2;
    /** @brief Number of river sources attempted. */
    int river_count = 25;

    /**
     * @brief Width of a volume-zero stream, in grid units.
     *
     * Widths are physical, not pixel counts. The original expressed them in
     * pixels, which made a river's width depend on the render resolution -- the
     * same map at 2048 had rivers half as wide in world terms as at 1024, and
     * the town packer had no resolution-independent number to keep buildings
     * out of the channel.
     */
    double river_width_base = 0.05;
    /** @brief Additional width per unit of river volume, in grid units. */
    double river_width_per_volume = 0.02;
    /**
     * @brief Width of a drawn `RoadClass::Trail`, in grid units.
     *
     * The three road widths are spaced so that they still land on *different*
     * pixel widths after `BiomeRenderer` halves them and truncates to whole
     * pixels. Spacing them evenly in grid units but finely -- the 0.035 / 0.06 /
     * 0.10 that looks reasonable written down -- collapses all three to a
     * one-pixel brush at every render size this generator ships with, and the
     * hierarchy the road pass worked out is invisible.
     */
    double trail_width = 0.10;
    /**
     * @brief Width of a drawn `RoadClass::Road`, in grid units.
     *
     * Wider than the single `road_width` that preceded it, because it is now
     * the middle of three tiers rather than the only one, and because a road
     * drawn a single pixel wide at any render size is a hairline, not a road.
     */
    double road_width = 0.20;
    /** @brief Width of a drawn `RoadClass::Highway`, in grid units. */
    double highway_width = 0.30;
    /** @brief Settlement placement parameters. */
    TownConfig towns;
    /** @brief Road network parameters. */
    RoadConfig roads;
    /** @brief Political geography parameters. */
    RegionConfig regions;
    /** @brief Notable-place parameters. */
    LandmarkConfig landmarks;

    // --- Pass toggles ---

    bool enable_water = true;       /**< @brief Run the water pass. */
    bool enable_coast = true;       /**< @brief Run the coast pass. */
    bool enable_elevation = true;   /**< @brief Run the elevation pass. */
    bool enable_temperature = true; /**< @brief Run the temperature pass. */
    bool enable_rivers = true;      /**< @brief Run the river pass. */
    bool enable_moisture = true;    /**< @brief Run the moisture pass. */
    bool enable_biomes = true;      /**< @brief Run the biome pass. */
    bool enable_roads = true;       /**< @brief Run the road pass. */
    bool enable_regions = true;     /**< @brief Run the region pass. */
    bool enable_towns = true;       /**< @brief Run the town pass. */
    bool enable_landmarks = true;   /**< @brief Run the landmark pass. */
    bool enable_noisy_edges = true; /**< @brief Run the noisy-edge pass. */

    /**
     * @brief Subdivide noisy edges rather than emitting a straight two-point line.
     *
     * Off is the cheap path and reproduces the original generator's output,
     * which shipped with subdivision commented out and therefore drew every
     * cell boundary straight. On gives the ragged, natural coastlines the
     * algorithm was written for, at the cost of a few extra points per edge.
     */
    bool subdivide_noisy_edges = true;
};

/**
 * @brief The physical width of a river carrying a given volume, in grid units.
 *
 * The one definition of how wide a river is. The renderer scales it to pixels;
 * the town packer uses it directly to keep buildings on the bank.
 *
 * @param config Supplies the width parameters.
 * @param volume The edge's river volume.
 * @return The river's width in grid units.
 */
inline double river_width(const MapConfig& config, int volume) {
    return config.river_width_base + config.river_width_per_volume * static_cast<double>(volume);
}

/**
 * @brief The physical width of a road of a given class, in grid units.
 *
 * The counterpart of `river_width()`, and the one definition of how wide a road
 * is: the renderer scales it to pixels, and a consumer laying geometry along a
 * road reads the same number.
 *
 * @param config Supplies the per-class widths.
 * @param road_class The class of road.
 * @return The road's width in grid units; 0 for `RoadClass::None`.
 */
inline double road_width_for(const MapConfig& config, RoadClass road_class) {
    switch (road_class) {
        case RoadClass::Trail:   return config.trail_width;
        case RoadClass::Road:    return config.road_width;
        case RoadClass::Highway: return config.highway_width;
        case RoadClass::None:    break;
    }
    return 0.0;
}

/**
 * @struct BiomePalette
 * @brief Render colours, in 0-255 component range, indexed by `Biome`.
 *
 * A flat array rather than one named field per biome: the renderer needs a
 * colour per cell per frame, and indexing an array by the enum replaces the
 * twenty-branch string comparison chain the original did at that point.
 */
struct BiomePalette {
    /** @brief One colour per `Biome`, indexed by `static_cast<std::size_t>(biome)`. */
    std::array<glm::vec3, k_biome_count> biome_colors = {
        glm::vec3(94, 182, 223),   // Ocean
        glm::vec3(94, 182, 223),   // Lake
        glm::vec3(33, 94, 33),     // Marsh
        glm::vec3(210, 255, 252),  // Ice
        glm::vec3(245, 222, 179),  // Beach
        glm::vec3(255, 250, 250),  // Snow
        glm::vec3(169, 169, 169),  // Tundra
        glm::vec3(201, 180, 155),  // Bare
        glm::vec3(153, 130, 109),  // Scorched
        glm::vec3(51, 102, 0),     // Taiga
        glm::vec3(128, 128, 0),    // Shrubland
        glm::vec3(238, 214, 175),  // TemperateDesert
        glm::vec3(85, 107, 47),    // TemperateRainForest
        glm::vec3(34, 139, 34),    // TemperateDeciduousForest
        glm::vec3(124, 252, 0),    // Grassland
        glm::vec3(0, 100, 0),      // TropicalRainForest
        glm::vec3(107, 142, 35),   // TropicalSeasonalForest
        glm::vec3(250, 250, 210),  // SubtropicalDesert
        glm::vec3(142, 186, 124),  // AlpineMeadow
        glm::vec3(222, 241, 247),  // Glacier
        glm::vec3(186, 184, 160),  // ColdDesert
        glm::vec3(178, 182, 108),  // Steppe
        glm::vec3(196, 190, 90),   // Savanna
        glm::vec3(150, 160, 92),   // Chaparral
        glm::vec3(126, 116, 96),   // Moorland
        glm::vec3(72, 110, 96),    // BorealWetland
        glm::vec3(58, 92, 62),     // Swamp
        glm::vec3(46, 120, 90),    // Mangrove
        glm::vec3(96, 150, 118),   // CloudForest
        glm::vec3(178, 128, 88),   // Badlands
        glm::vec3(238, 238, 230),  // SaltFlat
        glm::vec3(232, 206, 148),  // Dunes
        glm::vec3(92, 74, 70)      // VolcanicField
    };

    /** @brief Colour of river strokes. */
    glm::vec3 river_color = glm::vec3(94, 182, 223);
    /**
     * @brief Colour of a `RoadClass::Road` stroke.
     *
     * Warm earth rather than the flat black this used to be. A pure black line
     * of even width across a coloured map reads as an administrative border,
     * not as a road -- which is exactly how the old contour roads read.
     */
    glm::vec3 road_color = glm::vec3(122, 101, 82);
    /** @brief Colour of a `RoadClass::Trail` stroke. */
    glm::vec3 trail_color = glm::vec3(136, 122, 100);
    /** @brief Colour of a `RoadClass::Highway` stroke. */
    glm::vec3 highway_color = glm::vec3(146, 108, 62);
    /**
     * @brief Colour of the outline drawn under every road stroke.
     *
     * A road is stroked twice, this colour one pixel wider underneath. The
     * casing is what separates a road from the terrain it crosses: without it a
     * trail over dark forest is invisible and a highway over pale desert is a
     * smudge.
     */
    glm::vec3 road_casing_color = glm::vec3(43, 33, 24);
    /** @brief Colour of the settlement marker drawn at a town's centre. */
    glm::vec3 town_color = glm::vec3(120, 40, 40);
    /** @brief Colour of a natural landmark marker. */
    glm::vec3 landmark_natural_color = glm::vec3(40, 60, 130);
    /** @brief Colour of a built landmark marker. */
    glm::vec3 landmark_built_color = glm::vec3(90, 60, 130);
    /** @brief Colour of an individual packed building footprint. */
    glm::vec3 building_color = glm::vec3(70, 50, 40);
    /** @brief Colour the canvas is cleared to before any cell is filled. */
    glm::vec3 background_color = glm::vec3(255, 255, 255);

    /**
     * @brief Looks up the stroke colour for a road class.
     * @param road_class The class to colour; `None` returns the casing colour.
     * @return The palette entry, in 0-255 component range.
     */
    const glm::vec3& color_for(RoadClass road_class) const {
        switch (road_class) {
            case RoadClass::Trail:   return trail_color;
            case RoadClass::Road:    return road_color;
            case RoadClass::Highway: return highway_color;
            case RoadClass::None:    break;
        }
        return road_casing_color;
    }

    /**
     * @brief Looks up the colour for a biome.
     * @param biome The biome to colour.
     * @return The palette entry, in 0-255 component range.
     */
    const glm::vec3& color_for(Biome biome) const {
        return biome_colors[static_cast<std::size_t>(biome)];
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_CONFIG_H
