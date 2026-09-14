/**
 * @file map_config.h
 * @brief Every knob that shapes a generated map: grid density, the island
 *        noise field, per-pass parameters, town placement, and the render palette.
 */

#ifndef COOPA_MAPS_MAP_CONFIG_H
#define COOPA_MAPS_MAP_CONFIG_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <random>
#include <vector>

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
 * @brief Applies a noise configuration to a FastNoiseLite generator.
 *
 * The eight setters live here rather than in `Noise`'s constructor because two
 * different objects need them -- the `Noise` wrapper and `ShapeField`, which
 * warps the landmass outline. Duplicating the block is how a field ends up
 * configured in one place and silently ignored in the other.
 *
 * @param generator The generator to configure.
 * @param config The field parameters to apply.
 */
inline void configure_noise(FastNoiseLite& generator, const NoiseConfig& config) {
    generator.SetSeed(config.seed);
    generator.SetFrequency(static_cast<float>(config.frequency));
    generator.SetNoiseType(config.type);
    generator.SetFractalType(config.fractal_type);
    generator.SetFractalOctaves(config.octaves);
    generator.SetFractalLacunarity(static_cast<float>(config.lacunarity));
    generator.SetFractalGain(static_cast<float>(config.gain));
    generator.SetFractalWeightedStrength(static_cast<float>(config.weighted_strength));
}

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
 * @enum CompositeShading
 * @brief How the composite layer lights the biome colours.
 *
 * Both are *drawing* choices applied to the biome fill; neither touches the
 * height data, and the elevation layer on disk is unaffected either way.
 */
enum class CompositeShading {
    /**
     * @brief Brightness follows height: the higher the ground, the paler it is.
     *
     * A function of elevation alone, so the same height reads the same
     * everywhere on the map and a colour can be compared against the legend
     * plus a known offset. It shows *where the high ground is*.
     */
    Elevation,
    /**
     * @brief Brightness follows slope, lit from the north-west.
     *
     * A function of the height *gradient*, so it shows the shape of the
     * land -- ridgelines, valley walls, which way a face turns -- but says
     * nothing about altitude: a slope at sea level and the same slope on a
     * summit are drawn identically.
     */
    Hillshade
};

/** @brief Number of distinct `CompositeShading` values. */
inline constexpr std::size_t k_composite_shading_count = 2;

/**
 * @brief Maps a composite shading mode to its serialisation name.
 * @param shading The mode to name.
 * @return A `snake_case` identifier, e.g. `"hillshade"`.
 */
inline std::string_view composite_shading_name(CompositeShading shading) {
    switch (shading) {
        case CompositeShading::Elevation: return "elevation";
        case CompositeShading::Hillshade: return "hillshade";
    }
    return "elevation";
}

/**
 * @brief Resolves a serialisation name back to a shading mode.
 * @param name A name previously produced by `composite_shading_name()`.
 * @return The matching mode, or `CompositeShading::Elevation` if the name is unknown.
 */
inline CompositeShading composite_shading_from_name(std::string_view name) {
    if (name == "hillshade") return CompositeShading::Hillshade;
    return CompositeShading::Elevation;
}

/**
 * @enum MapShape
 * @brief The outline the landmass is confined to; everything outside it is sea.
 */
enum class MapShape {
    Rectangle,   /**< @brief Axis-aligned, `width_m` by `height_m`. */
    Circle,      /**< @brief `diameter_m` across. */
    Triangle,    /**< @brief Equilateral, `edge_length_m` a side, turned by `rotation`. */
    Continent,   /**< @brief One irregular landmass, `diameter_m` across on average. */
    Archipelago  /**< @brief `continent_count` irregular landmasses of varying size. */
};

/** @brief Number of distinct `MapShape` values. */
inline constexpr std::size_t k_map_shape_count = 5;

/**
 * @brief Maps a landmass shape to its serialisation name.
 * @param shape The shape to name.
 * @return A `snake_case` identifier, e.g. `"circle"`.
 */
inline std::string_view map_shape_name(MapShape shape) {
    switch (shape) {
        case MapShape::Rectangle:   return "rectangle";
        case MapShape::Circle:      return "circle";
        case MapShape::Triangle:    return "triangle";
        case MapShape::Continent:   return "continent";
        case MapShape::Archipelago: return "archipelago";
    }
    return "rectangle";
}

/**
 * @brief Resolves a serialisation name back to a shape.
 * @param name A name previously produced by `map_shape_name()`; `"rect"` is accepted too.
 * @return The matching shape, or `MapShape::Rectangle` if the name is unknown.
 */
inline MapShape map_shape_from_name(std::string_view name) {
    if (name == "circle")      return MapShape::Circle;
    if (name == "triangle")    return MapShape::Triangle;
    if (name == "continent")   return MapShape::Continent;
    if (name == "archipelago") return MapShape::Archipelago;
    return MapShape::Rectangle;
}

/**
 * @struct ShapeConfig
 * @brief The outline land is allowed to occupy, centred on the canvas.
 *
 * The canvas itself stays `grid_size` square whatever this says -- the shape is
 * inscribed in it, and the margin left over becomes open sea. So a smaller shape
 * is a smaller world on the same size of map, not a smaller image.
 *
 * **Every dimension defaults to zero, meaning "fill the canvas".** That makes the
 * default a rectangle the size of the whole grid, which is exactly what the
 * generator did before shapes existed, down to the byte.
 */
struct ShapeConfig {
    /** @brief Which outline to use. */
    MapShape shape = MapShape::Rectangle;
    /** @brief Rectangle width in metres; 0 spans the canvas. */
    double width_m = 0.0;
    /** @brief Rectangle height in metres; 0 spans the canvas. */
    double height_m = 0.0;
    /** @brief Circle diameter in metres; 0 spans the canvas. */
    double diameter_m = 0.0;
    /** @brief Equilateral triangle side in metres; 0 spans the canvas. */
    double edge_length_m = 0.0;
    /** @brief Rotation in radians, applied about the centre. Triangle only. */
    double rotation = 0.0;
    /**
     * @brief Mean diameter of one landmass in metres; 0 sizes it to the canvas.
     *
     * Its own dimension rather than a reuse of `diameter_m`, because the two are
     * not the same measurement: a circle's diameter is the whole world, while
     * this is the *average* of several landmasses that then vary about it. Wiring
     * them together would make an archipelago inherit whatever the circle happened
     * to be set to and fuse into one lumpy continent.
     *
     * `Continent` and `Archipelago` only. 0 means "sized to the canvas", which for
     * one continent is as wide as will fit and for an archipelago is landmasses
     * covering `k_archipelago_fill` of it between them.
     */
    double continent_size_m = 0.0;
    /**
     * @brief How many landmasses `Archipelago` attempts, at least 1.
     *
     * An upper bound, not a promise. Two landmasses placed close enough fuse
     * into one larger continent, which is what stops an archipelago reading as
     * a row of evenly spaced blobs.
     */
    int continent_count = 4;
    /**
     * @brief How far the outline of a landmass wanders from a circle, 0 to 0.6.
     *
     * Clamped to that range, so the radius never falls below 40% of the mean and
     * a landmass can never pinch itself in two. Zero gives a plain disc.
     * `Continent` and `Archipelago` only.
     */
    double irregularity = 0.35;
    /**
     * @brief Spread of landmass sizes about the mean in `Archipelago`, 0 to 0.9.
     *
     * At the default a continent is between 65% and 135% of `diameter_m`, so an
     * archipelago comes out as a couple of large landmasses and a scatter of
     * smaller ones rather than clones.
     */
    double size_variance = 0.35;
    /**
     * @brief Amplitude of the coastal warp, as a fraction of the mean radius.
     *
     * The radial outline alone is star-convex -- every ray from the centre
     * crosses the coast exactly once, so there are no fjords and no headland
     * that folds back on itself. Displacing the whole boundary by a noise field
     * breaks that, and is what produces peninsulas, inlets and the occasional
     * island lying off the coast. `Continent` and `Archipelago` only.
     */
    double coast_detail = 0.12;
};

/**
 * @struct TownConfig
 * @brief Controls settlement placement and the building footprints packed into each one.
 */
struct TownConfig {
    /** @brief Upper bound on settlements placed; fewer appear if the map lacks room. */
    int town_count = 12;
    /** @brief Minimum distance in grid units between two settlements. */
    double min_spacing_m = 360.0;
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
    /**
     * @brief Cells a capital may claim, itself included.
     *
     * A settlement is not one Voronoi cell. At 60 m to the grid unit a cell is
     * about 3,600 m^2, which holds ten buildings of 10 m at a believable
     * density -- so a capital confined to its own cell is a hamlet, and the
     * three tiers become indistinguishable. Claiming neighbours is what lets a
     * capital be a town-sized thing while a village stays a village.
     */
    int capital_cells = 7;
    /** @brief Cells a town may claim, itself included. */
    int town_cells = 3;
    /** @brief Cells a village may claim; one, which is what makes it a village. */
    int village_cells = 1;
    /** @brief Buildings attempted in a capital across all its cells; lesser tiers get a fraction. */
    int buildings_per_town = 70;
    /** @brief Smallest side length of a square building footprint, in grid units. */
    double building_size_min_m = 7.0;
    /** @brief Largest side length of a square building footprint, in grid units. */
    double building_size_max_m = 14.0;
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
    double water_clearance_m = 3.0;
    /** @brief Fewest occupants in a household. */
    int household_size_min = 3;
    /** @brief Most occupants in a household. */
    int household_size_max = 7;
    /** @brief Population density multiplier applied to a capital. */
    double capital_density = 1.6;
    /** @brief Population density multiplier applied to a town. */
    double town_density = 1.2;
    /** @brief Perpendicular distance from a street's centreline to a plot centre. */
    double street_offset_m = 8.0;
    /** @brief Spacing along a street between successive plots. */
    double street_spacing_m = 14.0;
    /** @brief Random offset applied to a street-front plot, on both axes. */
    double position_jitter_m = 1.5;
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
     * Matches `TownConfig::min_spacing_m` on purpose. Hubs are scored the way
     * settlement sites are, so spacing them the same way is what puts a road
     * through most of the towns the next pass but one goes on to place.
     */
    double hub_min_spacing_m = 360.0;
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

    // --- World scale ---

    /**
     * @brief Side length in metres of one grid unit.
     *
     * The number that makes every other size in this struct mean something. A
     * cell is roughly one grid unit across, so this is also about how big a cell
     * is on the ground: at 60 m a cell holds a cluster of ten or so buildings,
     * which is what a village is, and `grid_size = 80` gives a 4.8 km world.
     *
     * Before this existed, grid units were abstract and every physical size was
     * tuned by eye against a fixed render resolution. Roads came out 12 m wide
     * and nothing in the generator could have said so.
     */
    double meters_per_grid_unit = 60.0;

    /**
     * @brief Metres of height the normalised `[0, 1]` elevation field spans.
     *
     * The vertical scale, and the world had none: `meters_per_grid_unit` fixed
     * how far a grid unit reaches *across* the ground while height stayed a bare
     * fraction, so "a river one metre deep" had nowhere to land. With this, the
     * sea floor is at 0 m, the waterline at `sea_level * elevation_range_m`, and
     * the highest peak at the full range.
     *
     * 600 m over a 4.8 km world puts the steepest ground at roughly a 1-in-8
     * grade, which is hilly without being alpine.
     */
    double elevation_range_m = 600.0;

    // --- Rendering ---

    /**
     * @brief Ground covered by one rendered pixel, in metres.
     *
     * **Always 1.0 by default**, and deliberately so: a PNG is then a literal
     * one-pixel-per-metre map, and a pixel count read off a render *is* a
     * measurement -- a 6 m road is 6 px wide because it is 6 m wide. Every
     * physical size in this struct is denominated in metres on that
     * understanding, so 1.0 is the setting under which a render can be measured
     * rather than merely looked at.
     *
     * Raising it renders the same world smaller and faster. Setting
     * `image_size` instead back-computes this, which is the same knob from the
     * other end -- see `image_size` for which wins.
     */
    double meters_per_pixel = 1.0;

    /**
     * @brief How the composite layer lights the biome colours.
     *
     * Elevation by default. Hillshading draws a more sculptural picture, but it
     * is a function of slope rather than height, so it answers "which way does
     * this face turn" and not "how high is this" -- and on a map whose point is
     * the terrain underneath it, the second question is usually the one being
     * asked.
     */
    CompositeShading composite_shading = CompositeShading::Elevation;

    /**
     * @brief Deflate effort used when encoding a PNG; higher is smaller and slower.
     *
     * stb's own default is 8. Encoding is the dominant cost of an export -- more
     * than half the runtime of a default map -- so this is worth reaching for
     * when a run is a preview rather than an artefact. It changes file size only;
     * the pixels are identical at every level.
     */
    int png_compression_level = 8;

    /**
     * @brief Side length in pixels of the square PNG renders.
     *
     * This and `meters_per_pixel` are the two ends of one knob, and they must
     * never disagree: every feature is stroked in *metres* and converted through
     * the scale, so a stale pairing would draw a "10 m highway" at whatever width
     * the wrong scale implied. The invariant is
     * `image_size * meters_per_pixel == grid_size * meters_per_grid_unit`.
     *
     * Either end may be the one you set. Naming a resolution is the more concrete
     * statement of intent, so `image_size` wins where both are given and
     * `meters_per_pixel` is back-computed from it -- `apply_config_node()` does
     * that on load and `--image-size` does it on the command line. Say nothing
     * and `derive_image_size()` fills this in from the scale.
     *
     * The default is `derive_image_size()` of the other defaults rather than a
     * round number, so a default-constructed config already satisfies the
     * invariant instead of starting out contradicting itself.
     */
    int image_size = 3000;
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
     * @brief The large-scale field that decides where the high ground goes.
     *
     * Low frequency, because this shapes massifs rather than texture: a feature
     * spans many cells. Compare `noise_terrain`, which is deliberately finer
     * than a single cell.
     */
    NoiseConfig noise_relief{7717, 0.055};
    /**
     * @brief The field that warps the coastline of the organic landmass shapes.
     *
     * Only read by `MapShape::Continent` and `MapShape::Archipelago`. Low
     * frequency on purpose: this bends the *outline* of a continent, so a
     * feature is a bay or a headland spanning many cells. The island field
     * carves the detail inside whatever silhouette this leaves.
     */
    NoiseConfig noise_shape{5233, 0.03};
    /**
     * @brief How much fractal relief reshapes the coast-distance height field, 0 to 1.
     *
     * Height is breadth-first *distance from the coast*, which is what keeps
     * coastlines at sea level and puts mountains inland -- but taken alone it
     * makes the high ground the literal medial axis of the landmass, so peaks
     * trace thin winding ridges equidistant from the bays either side. Real
     * terrain does not do that.
     *
     * This multiplies the distance field by a noise factor before the heights are
     * rank-remapped, which reorders them: some of the skeleton drops into
     * valleys, some of the flanks rise, and what comes out reads as massifs and
     * basins. It is applied to *land* corners only, so the ordering of water
     * against land is untouched and rivers still run downhill to a coast that is
     * still the lowest ground there is.
     *
     * Defaults to 0.65, where the skeleton is gone but the coast-distance trend
     * still reads -- land still broadly rises inland. Zero restores the pure
     * distance field, which is what Amit Patel's original generator produced and
     * what this looked like before.
     */
    double terrain_relief = 0.65;
    /**
     * @brief The detail field displacing the sampled ground surface.
     *
     * A higher frequency than the island or temperature fields on purpose: this
     * is texture within a cell, not a landform. Cells are 60 m across at the
     * default scale, so the detail has to be finer than that to show at all.
     */
    NoiseConfig noise_terrain{4919, 0.35};
    /**
     * @brief How strongly the detail field displaces the sampled surface, 0 to 1.
     *
     * Zero -- the default -- leaves the surface exactly as the passes computed
     * it. Raising it adds jaggedness *without* touching the cell and corner
     * heights, so biomes, rivers and roads are classified on the same smooth
     * control field either way and only the sampled surface roughens. The
     * displacement is scaled by the local height, so coastlines stay at sea level
     * however high this goes.
     *
     * Pair it with `elevation_smoothing_iterations`: that controls how smooth the
     * underlying landform is, this controls how rough the skin over it is.
     */
    double terrain_roughness = 0.0;
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
    /**
     * @brief The waterline, in the normalised `[0, 1]` height field.
     *
     * A real height, not a convention. The sea bed occupies everything below it
     * and land everything above, so "the ground here is under water" is an
     * honest comparison rather than a vacuous one -- which it was when the whole
     * field started at zero and the sea was pinned to the bottom of it.
     *
     * Two consequences worth knowing. The elevation layer now carries genuine
     * bathymetry, dark where the sea is deep. And the sea's surface is a visible
     * mid-grey on the same scale as everything else, rather than the black that
     * made it indistinguishable from dry land.
     *
     * Land therefore spans `[sea_level, 1]`, so anything comparing against a
     * *land* height must go through `land_height()` first.
     */
    double sea_level = 0.25;
    /** @brief Noise value above which a corner is water. */
    double threshold_water = 0.3;
    /** @brief A cell becomes water once more than this many of its corners are. */
    int threshold_water_count = 2;
    /** @brief Number of river sources attempted. */
    int river_count = 25;
    /**
     * @brief Corners a watercourse must run through before it counts as a river.
     *
     * Most land is close to a coast, so a source drawn uniformly usually sits a
     * couple of cells from the sea and produces a trickle. Rejecting the short
     * ones and drawing again is what leaves rivers that actually cross the map.
     */
    int river_min_length = 6;
    /**
     * @brief Lowest elevation a river may start from.
     *
     * The other half of why rivers used to be short: at the old 0.3 a source
     * could appear on the coastal plain it was meant to run down to.
     */
    double river_source_min_elevation = 0.45;
    /** @brief Highest elevation a river may start from. */
    double river_source_max_elevation = 0.9;
    /** @brief Chaikin corner-cutting passes applied to each traced watercourse. */
    int river_smoothing_iterations = 2;

    /**
     * @brief Width of a volume-zero stream, in grid units.
     *
     * Widths are physical, not pixel counts. The original expressed them in
     * pixels, which made a river's width depend on the render resolution -- the
     * same map at 2048 had rivers half as wide in world terms as at 1024, and
     * the town packer had no resolution-independent number to keep buildings
     * out of the channel.
     */
    double river_width_base_m = 5.0;
    /** @brief Additional width per unit of river volume, in metres. */
    double river_width_per_volume_m = 2.0;
    /**
     * @brief How far a river's surface sits above the ground it runs over, in metres.
     *
     * A river is not a line painted on the terrain, it is water standing in a
     * channel -- so its surface has to be *above* the ground, or a mesh built
     * from the two fights with itself along every watercourse. Deepens with
     * volume: a stream is ankle-deep and a trunk river is not.
     *
     * Above the ground **as the elevation layer draws it** -- `river_surface_at()`
     * measures from `MapGraph::elevation_at()`, not from `MapCorner::elevation`.
     * The two are different surfaces, by some 22 m at a river corner, and while
     * this was measured from the corner heights the sheet was drawn below the
     * terrain along nearly half of every watercourse.
     *
     * Complementary to `river_incision_m` and `river_channel_depth_m`, not a
     * duplicate of either: incision cuts the valley, the channel depth cuts the
     * bed within it, and this floats the sheet of water above that bed. A metre
     * of freeboard, twenty of channel and sixty of valley are all true of the
     * same river.
     */
    double river_depth_m = 1.0;
    /** @brief Additional depth per unit of river volume, in metres. */
    double river_depth_per_volume_m = 0.35;
    /**
     * @brief How deep the channel is cut into the sampled ground, in metres.
     *
     * The third and last of the river depths, and the only one that puts a
     * watercourse in the *picture* of the terrain. Worth stating what each does,
     * because three depths on one river invites the wrong guess:
     *
     * - `river_incision_m` carves the **valley** into the control mesh -- corner
     *   and cell heights. A landform, hundreds of metres across.
     * - This cuts the **channel** into the surface sampled *between* those
     *   heights. A few metres across, and the only one fine enough to read as a
     *   river rather than as a dip in the ground.
     * - `river_depth_m` floats the **water sheet** above the control mesh, which
     *   is what stops a mesh built from ground and water fighting itself.
     *
     * It has to live at sample time rather than in the control mesh because the
     * mesh cannot hold it: `elevation_at()` interpolates between cell sites some
     * 60 m apart, and a river is 5 to 20 m wide. Carved into cell heights the
     * best achievable was a 500 m depression with no edge -- measurably deep,
     * invisible to look at. Same reason `terrain_roughness` lives here.
     *
     * Zero leaves the sampled surface exactly as the control mesh describes it.
     */
    double river_channel_depth_m = 18.0;
    /**
     * @brief Additional channel depth per unit of river volume, in metres.
     *
     * The channel widens with volume through `river_width()` whether this is set
     * or not; this is what also makes it deepen, so a trunk river reads as a
     * trench and a headwater stream as a scratch.
     */
    double river_channel_depth_per_volume_m = 4.0;
    /**
     * @brief How deep a river cuts the valley it runs in, in metres.
     *
     * Rivers erode. Without this the elevation field has no idea a river pass
     * ever ran -- elevation is computed before rivers are routed, so the height
     * field came out of the generator with a drainage network drawn on a surface
     * that has nowhere for the water to go, and the elevation layer showed no
     * trace of the rivers the water layer is full of.
     *
     * This is a *valley*, not a channel, and the difference is forced by the
     * geometry. The rendered surface interpolates cell-site heights over Delaunay
     * triangles, while rivers run along Voronoi edges -- cell *boundaries*. So the
     * narrowest thing the surface can express is about a cell across, 60 m at the
     * default scale, against a river 5 to 20 m wide. Which is the right answer
     * anyway: a river sits in a valley far wider than itself.
     *
     * Zero restores the uncarved height field exactly, which is what the
     * generator produced before valleys existed.
     */
    double river_incision_m = 60.0;
    /**
     * @brief Additional valley depth per unit of river volume, in metres.
     *
     * A trunk river has had far longer to cut than the stream feeding it, so the
     * valley deepens downstream on its own rather than needing to be authored.
     */
    double river_incision_per_volume_m = 12.0;
    /**
     * @brief How many corners out from the watercourse the valley opens.
     *
     * Rings of `MapCorner::adjacent`, so 1 is a trench with the river at the
     * bottom of it and nothing either side, and 2 gives it banks. Raising it
     * widens and gentles the valley rather than deepening it.
     */
    int river_valley_width = 2;
    /**
     * @brief Depth multiplier per ring outward from the watercourse, 0 to 1.
     *
     * What turns a step into a slope. At the default each ring is carved half as
     * deeply as the one inside it, so the valley wall grades away instead of
     * dropping vertically -- which matters for more than looks, since a cliff at
     * every watercourse would push the mean elevation step between neighbouring
     * cells past what `elevation_smoothing_iterations` is there to hold down.
     */
    double river_valley_falloff = 0.5;
    /**
     * @brief How far every water surface is extended past its own edge, in metres.
     *
     * So the water clips *into* the terrain rather than meeting it exactly. Two
     * surfaces that share an edge exactly will show a seam wherever the meshes
     * disagree by a rounding error, and along a coastline they always do.
     */
    double water_edge_overlap_m = 1.0;
    /**
     * @brief Carriageway width of a `RoadClass::Trail`, in metres.
     *
     * A cart track. These three are real widths and not render tuning: at the
     * default `meters_per_pixel` they come out 3, 6 and 10 pixels, because that
     * is what 3, 6 and 10 metres of ground are. They were previously expressed
     * in grid units and chosen so the three would land on different *pixel*
     * widths, which at 60 m to the grid unit meant a 12 m road and an 18 m
     * highway -- motorway proportions on a medieval map.
     */
    double trail_width_m = 3.0;
    /** @brief Carriageway width of a `RoadClass::Road`, in metres; two carts abreast. */
    double road_width_m = 6.0;
    /** @brief Carriageway width of a `RoadClass::Highway`, in metres. */
    double highway_width_m = 10.0;
    /** @brief The outline land is confined to. */
    ShapeConfig shape;
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
    bool enable_valleys = true;     /**< @brief Run the valley pass, which cuts rivers into the terrain. */
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

/** @brief How many angular harmonics wander the outline of one landmass. */
inline constexpr std::size_t k_shape_harmonics = 4;

/**
 * @brief The angular frequencies summed into a landmass outline.
 *
 * Whole numbers, so the outline closes on itself with no seam at `theta = pi`,
 * and pairwise coprime, so the sum has no shorter period than a full turn --
 * a set like {2, 4, 6} would come out twofold symmetric and read as a manufactured
 * shape rather than a coastline.
 */
inline constexpr std::array<double, k_shape_harmonics> k_shape_modes{2.0, 3.0, 5.0, 7.0};

/** @brief Amplitude multiplier between successive outline harmonics. */
inline constexpr double k_shape_harmonic_gain = 0.55;

/** @brief Seed offset for landmass placement; distinct from every pass's own. */
inline constexpr unsigned int k_shape_seed_offset = 6291469u;

/**
 * @brief How much of the canvas an `Archipelago` fills when `diameter_m` is 0.
 *
 * The fraction of the largest disc that fits the canvas which the landmasses,
 * counting the full reach of their outlines, cover between them. Well under 1,
 * and that is the point: the slack is the room they have to scatter into. Size
 * them to fill the canvas and every centre gets pinned near the middle, and the
 * archipelago fuses into one continent with a lumpy edge.
 */
inline constexpr double k_archipelago_fill = 0.55;

/** @brief Candidate positions considered per landmass when placing an archipelago. */
inline constexpr int k_shape_placement_darts = 32;

/**
 * @brief How far out of the middle candidate landmass positions are pushed, 0 to 1.
 *
 * A landmass is large next to the disc its centre is allowed to fall in, so a
 * centre sampled near the middle of that disc overlaps everything else there is
 * -- area-uniform sampling puts a landmass in the one place it cannot help but
 * fuse. Holding the candidates out to an annulus instead keeps roughly one more
 * landmass distinct at the same size and the same coverage, and it is what puts
 * open sea *between* the landmasses rather than only around the outside of them.
 */
inline constexpr double k_shape_placement_bias = 0.55;

/**
 * @brief Grid size the coastal warp frequency is quoted against.
 *
 * `noise_shape.frequency` is in grid units, so holding it fixed while raising
 * `grid_size` would not resolve the coast more finely -- it would crumble a
 * continent into a hundred little wobbles. Scaling by this keeps the silhouette
 * of a map the same shape at every resolution, which is what `--grid-size` is
 * supposed to control. The island field is scaled the same way, for the same
 * reason.
 */
inline constexpr double k_shape_reference_grid = 40.0;

/**
 * @struct ShapeBlob
 * @brief One landmass: where it sits, how big it is, and how its outline wanders.
 */
struct ShapeBlob {
    /** @brief Centre, in grid units. */
    double centre_x = 0.0;
    /** @brief Centre, in grid units. */
    double centre_y = 0.0;
    /** @brief Mean radius in grid units; the outline varies about it. */
    double radius = 0.0;
    /** @brief Phase of each outline harmonic, in radians. */
    std::array<double, k_shape_harmonics> phase{};
};

/**
 * @class ShapeField
 * @brief The outline the landmass is confined to, resolved once and sampled cheaply.
 *
 * The shape of the world comes from one predicate: `inset()` returns how far
 * inside the outline a point lies. `border_check_()` flags any corner whose inset
 * falls below `border_length`; the water pass forces those cells to sea, and the
 * elevation pass measures height as distance from them -- so this class decides
 * the coastline, the mountains, the regions and the roads, all without any of
 * them knowing shapes exist.
 *
 * It is an object rather than a free function because the organic shapes carry
 * state: a landmass layout drawn from the seed, and a noise generator that warps
 * the coast. Rebuilding those per sample is the exact mistake `Noise` was
 * introduced to undo -- `inset()` is called once per cell *and* once per corner,
 * so it runs on the order of a hundred thousand times per map. Build one field
 * and reuse it; `shape_inset()` is the convenience path for one-off queries.
 *
 * `Rectangle`, `Circle` and `Triangle` are pure geometry and touch neither the
 * layout nor the generator, so constructing a field for them costs nothing.
 *
 * ### Sizing
 *
 * Every dimension treats 0 as "as big as the canvas allows", which is what makes
 * the default rectangle span the grid. `continent_size_m` is the *mean* diameter
 * of a single landmass, so 0 has to mean something per-shape there: a `Continent`
 * grows until its widest lobe just fits the canvas, and an `Archipelago` sizes its
 * landmasses so they cover `k_archipelago_fill` of it between them.
 */
class ShapeField {
public:
    /**
     * @brief Resolves the outline described by a configuration.
     * @param config Supplies the shape, its dimensions, the grid size, the world
     *        scale, the master seed and the coastal warp field.
     */
    explicit ShapeField(const MapConfig& config)
        : shape_(config.shape.shape),
          grid_(static_cast<double>(config.grid_size)),
          centre_(static_cast<double>(config.grid_size) * 0.5),
          rotation_(config.shape.rotation) {
        // A dimension of 0 means "as big as the canvas allows".
        const auto extent = [&config, this](double meters) {
            return meters > 0.0 ? meters / config.meters_per_grid_unit : grid_;
        };

        switch (shape_) {
            case MapShape::Circle:
                radius_ = extent(config.shape.diameter_m) * 0.5;
                return;
            case MapShape::Triangle:
                half_side_ = extent(config.shape.edge_length_m) * 0.5;
                return;
            case MapShape::Continent:
            case MapShape::Archipelago:
                build_landmasses_(config);
                return;
            case MapShape::Rectangle:
                break;
        }
        half_width_ = extent(config.shape.width_m) * 0.5;
        half_height_ = extent(config.shape.height_m) * 0.5;
    }

    /**
     * @brief How far inside the outline a point lies, in grid units.
     *
     * Positive inside, negative outside, zero on the boundary. The three
     * geometric shapes return an exact signed distance; the organic ones return a
     * signed pseudo-distance, whose sign is exact and whose magnitude near the
     * boundary is unit-scaled -- which is all `border_length` compares against.
     *
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @return Positive inside the shape, negative outside, zero on its boundary.
     */
    double inset(double x, double y) const {
        const double dx = x - centre_;
        const double dy = y - centre_;

        switch (shape_) {
            case MapShape::Circle:
                return radius_ - std::sqrt(dx * dx + dy * dy);
            case MapShape::Triangle: {
                // Rotate into the triangle's own frame, then measure. Rotating the
                // query rather than the triangle keeps the shape description to one
                // number.
                const double c = std::cos(-rotation_);
                const double sn = std::sin(-rotation_);
                const double lx = dx * c - dy * sn;
                const double ly = dx * sn + dy * c;

                // Signed distance to an equilateral triangle centred on its
                // centroid, with `half_side_` as the base half-width and k = sqrt(3).
                const double k = 1.7320508075688772;
                if (half_side_ <= 0.0) {
                    return -1.0;
                }
                // The formula is written for maths axes, where y climbs; image rows
                // descend. Flipping here is what puts the apex at the top of the
                // picture rather than the bottom.
                double px = std::abs(lx) - half_side_;
                double py = -ly + half_side_ / k;
                if (px + k * py > 0.0) {
                    const double folded_x = (px - k * py) * 0.5;
                    const double folded_y = (-k * px - py) * 0.5;
                    px = folded_x;
                    py = folded_y;
                }
                px -= std::clamp(px, -2.0 * half_side_, 0.0);
                // Distance, signed by which side of the folded edge the point landed
                // on. Positive inside, to match the other two shapes.
                return std::sqrt(px * px + py * py) * (py > 0.0 ? 1.0 : -1.0);
            }
            case MapShape::Continent:
            case MapShape::Archipelago: {
                // Union of the landmasses: a point is inside the shape if it is
                // inside any one of them, so two that overlap fuse into a single
                // larger continent instead of drawing a coast through each other.
                double best = blob_inset_(blobs_.front(), x, y);
                for (std::size_t i = 1; i < blobs_.size(); ++i) {
                    best = std::max(best, blob_inset_(blobs_[i], x, y));
                }
                // Warping after the union rather than per landmass keeps the coastal
                // texture continuous across a fused pair, and is what lets a coast
                // fold back on itself -- the radial outline alone cannot, so without
                // this there are no fjords and no offshore islands.
                return best + warp_amplitude_ *
                                  static_cast<double>(warp_.GetNoise(static_cast<float>(x),
                                                                    static_cast<float>(y)));
            }
            case MapShape::Rectangle:
                break;
        }

        return std::min(half_width_ - std::abs(dx), half_height_ - std::abs(dy));
    }

    /**
     * @brief The landmasses the organic shapes resolved to.
     * @return One entry per landmass; empty for the geometric shapes.
     */
    const std::vector<ShapeBlob>& landmasses() const { return blobs_; }

private:
    /**
     * @brief Draws the landmass layout from the master seed.
     *
     * Sizes come first, because a landmass has to know how far it reaches before
     * anywhere is a legal place to put it: the centre is confined to a disc small
     * enough that the widest lobe plus the coastal warp still clears the canvas
     * edge by `margin_`. Without that the ocean flood fill can find a landmass
     * touching the border and mistake the sea for a lake.
     *
     * @param config Supplies the seed, the world scale and the shape parameters.
     */
    void build_landmasses_(const MapConfig& config) {
        irregularity_ = std::clamp(config.shape.irregularity, 0.0, 0.6);
        const double detail = std::max(config.shape.coast_detail, 0.0);
        const double variance = std::clamp(config.shape.size_variance, 0.0, 0.9);
        const int count = shape_ == MapShape::Archipelago
                              ? std::max(1, config.shape.continent_count)
                              : 1;
        margin_ = std::max(1.0, grid_ * 0.03);

        // How much wider than its mean radius a landmass can get: the tallest
        // outline lobe, plus the coastal warp on top of it.
        const double reach = 1.0 + irregularity_ + detail;
        const double fitting_radius = std::max(0.0, (grid_ * 0.5 - margin_) / reach);

        double base_radius =
            config.shape.continent_size_m > 0.0
                ? config.shape.continent_size_m / config.meters_per_grid_unit * 0.5
                : 0.0;
        if (base_radius <= 0.0) {
            // Pack `count` landmasses into the canvas disc at `k_archipelago_fill`
            // coverage: N * (R * reach)^2 = fill * (grid/2 - margin)^2. A lone
            // continent skips the packing and simply grows until it fits.
            base_radius = shape_ == MapShape::Archipelago
                              ? fitting_radius * std::sqrt(k_archipelago_fill /
                                                           static_cast<double>(count))
                              : fitting_radius;
        }
        base_radius = std::min(base_radius, fitting_radius);
        warp_amplitude_ = detail * base_radius;

        configure_noise(warp_, config.noise_shape);
        warp_.SetFrequency(static_cast<float>(config.noise_shape.frequency *
                                              (k_shape_reference_grid / grid_)));

        std::mt19937 rng(static_cast<std::mt19937::result_type>(config.seed) + k_shape_seed_offset);
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::uniform_real_distribution<double> turn(0.0, 2.0 * 3.14159265358979323846);

        blobs_.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            ShapeBlob blob;
            blob.radius =
                count > 1 ? base_radius * (1.0 + variance * (unit(rng) * 2.0 - 1.0)) : base_radius;
            blob.radius = std::min(blob.radius, fitting_radius);
            for (std::size_t k = 0; k < k_shape_harmonics; ++k) {
                blob.phase[k] = turn(rng);
            }

            const double limit =
                std::max(0.0, grid_ * 0.5 - margin_ - blob.radius * (1.0 + irregularity_) -
                                  warp_amplitude_);
            place_(blob, limit, rng, unit, turn);
            blobs_.push_back(blob);
        }
    }

    /**
     * @brief Picks a centre for one landmass by Mitchell's best-candidate sampling.
     *
     * Throws `k_shape_placement_darts` positions into the legal disc and keeps the
     * one furthest from every landmass already placed. Rejection sampling against a
     * minimum separation would spread them more evenly but can run out of room and
     * fail to place the last landmass at all; this always places one, and still
     * lets two land close enough to fuse -- which is what stops an archipelago
     * reading as a row of evenly spaced blobs.
     *
     * @param blob The landmass to position; its radius must already be set.
     * @param limit Radius of the disc the centre may fall in, in grid units.
     * @param rng The layout random stream.
     * @param unit A `[0, 1)` distribution over `rng`.
     * @param turn A `[0, 2*pi)` distribution over `rng`.
     */
    void place_(ShapeBlob& blob, double limit, std::mt19937& rng,
                std::uniform_real_distribution<double>& unit,
                std::uniform_real_distribution<double>& turn) const {
        if (limit <= 0.0 || blobs_.empty()) {
            // Nowhere to go, or nothing to stay away from: sample once so the
            // stream advances identically either way.
            const double angle = turn(rng);
            const double distance = sample_distance_(limit, unit(rng));
            blob.centre_x = centre_ + std::cos(angle) * distance;
            blob.centre_y = centre_ + std::sin(angle) * distance;
            return;
        }

        double best_x = centre_;
        double best_y = centre_;
        double best_clearance = -1.0;
        for (int dart = 0; dart < k_shape_placement_darts; ++dart) {
            const double angle = turn(rng);
            const double distance = sample_distance_(limit, unit(rng));
            const double x = centre_ + std::cos(angle) * distance;
            const double y = centre_ + std::sin(angle) * distance;

            double clearance = std::numeric_limits<double>::max();
            for (const ShapeBlob& placed : blobs_) {
                const double dx = x - placed.centre_x;
                const double dy = y - placed.centre_y;
                // Measured between the rims, not the centres, so a large landmass
                // pushes its neighbours further away than a small one does.
                clearance = std::min(clearance,
                                     std::sqrt(dx * dx + dy * dy) - placed.radius - blob.radius);
            }
            if (clearance > best_clearance) {
                best_clearance = clearance;
                best_x = x;
                best_y = y;
            }
        }
        blob.centre_x = best_x;
        blob.centre_y = best_y;
    }

    /**
     * @brief Turns a uniform sample into a distance from the canvas centre.
     *
     * The square root is what makes the draw area-uniform over a disc rather than
     * bunched at the middle, since a ring has more area the further out it sits.
     * `k_shape_placement_bias` then lifts the floor off the centre entirely.
     *
     * @param limit Radius of the disc the centre may fall in, in grid units.
     * @param sample A uniform draw in `[0, 1)`.
     * @return A distance in `[bias * limit, limit]`.
     */
    static double sample_distance_(double limit, double sample) {
        return limit * (k_shape_placement_bias +
                        (1.0 - k_shape_placement_bias) * std::sqrt(sample));
    }

    /**
     * @brief Inset relative to a single landmass, before the coastal warp.
     * @param blob The landmass to measure against.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @return Positive inside that landmass, negative outside.
     */
    double blob_inset_(const ShapeBlob& blob, double x, double y) const {
        const double dx = x - blob.centre_x;
        const double dy = y - blob.centre_y;
        const double distance = std::sqrt(dx * dx + dy * dy);
        const double theta = std::atan2(dy, dx) - rotation_;

        // A fractal sum in the angle: each harmonic turns faster and counts for
        // less, which is the one-dimensional equivalent of the fbm that gives the
        // terrain its shape. Normalised by the total weight so `irregularity_`
        // means the same thing however many harmonics there are.
        double wobble = 0.0;
        double weight = 0.0;
        double gain = 1.0;
        for (std::size_t k = 0; k < k_shape_harmonics; ++k) {
            wobble += gain * std::sin(k_shape_modes[k] * theta + blob.phase[k]);
            weight += gain;
            gain *= k_shape_harmonic_gain;
        }
        return blob.radius * (1.0 + irregularity_ * (wobble / weight)) - distance;
    }

    MapShape shape_;              /**< @brief Which outline this field describes. */
    double grid_;                 /**< @brief Canvas size in grid units. */
    double centre_;               /**< @brief Canvas centre in grid units, both axes. */
    double rotation_;             /**< @brief Rotation about the centre, in radians. */
    double radius_ = 0.0;         /**< @brief Circle radius, in grid units. */
    double half_side_ = 0.0;      /**< @brief Triangle base half-width, in grid units. */
    double half_width_ = 0.0;     /**< @brief Rectangle half-width, in grid units. */
    double half_height_ = 0.0;    /**< @brief Rectangle half-height, in grid units. */
    double irregularity_ = 0.0;   /**< @brief Clamped outline wander, 0 to 0.6. */
    double margin_ = 0.0;         /**< @brief Open sea kept clear round the canvas edge. */
    double warp_amplitude_ = 0.0; /**< @brief Coastal warp amplitude, in grid units. */
    std::vector<ShapeBlob> blobs_; /**< @brief The landmasses; empty unless organic. */
    FastNoiseLite warp_;           /**< @brief The coastal warp field; unused unless organic. */
};

/**
 * @brief How far inside the landmass shape a point lies, in grid units.
 *
 * A one-off query against a freshly resolved `ShapeField`. Convenient, but it
 * rebuilds the field every call -- anything sampling the shape more than a
 * handful of times should hold a `ShapeField` and call `ShapeField::inset()`,
 * which is what `border_check_()` does.
 *
 * @param config Supplies the shape, the grid size and the world scale.
 * @param x Horizontal grid position.
 * @param y Vertical grid position.
 * @return Positive inside the shape, negative outside, zero on its boundary.
 */
inline double shape_inset(const MapConfig& config, double x, double y) {
    return ShapeField(config).inset(x, y);
}

/**
 * @brief Converts a height in metres into the normalised elevation field.
 *
 * The vertical counterpart of `meters_to_grid()`. Heights are stored as a
 * fraction of `elevation_range_m`, so anything physical -- how deep a river is,
 * how far water overhangs its bank -- crosses here.
 *
 * @param config Supplies the vertical scale.
 * @param meters The height in metres.
 * @return The same height as a fraction of the elevation range.
 */
inline double meters_to_height(const MapConfig& config, double meters) {
    return config.elevation_range_m > 0.0 ? meters / config.elevation_range_m : 0.0;
}

/**
 * @brief Converts a normalised height into metres.
 * @param config Supplies the vertical scale.
 * @param height A height from the `[0, 1]` field.
 * @return The same height in metres above the sea floor.
 */
inline double height_to_meters(const MapConfig& config, double height) {
    return height * config.elevation_range_m;
}

/**
 * @brief Re-expresses a height as a fraction of the land above the waterline.
 *
 * Land occupies `[sea_level, 1]` of the height field, so a threshold that
 * describes *land* -- where a peak begins, where a town is too high to reach,
 * which row of the Whittaker diagram a cell falls in -- cannot be compared
 * against a raw elevation without first taking the sea out of the range. Every
 * such threshold is phrased against this, which is what kept them meaning the
 * same thing when the waterline moved off zero.
 *
 * @param config Supplies the waterline.
 * @param elevation A raw height from the `[0, 1]` field.
 * @return 0 at the shoreline, 1 at the highest ground; 0 for anything submerged.
 */
inline double land_height(const MapConfig& config, double elevation) {
    const double span = 1.0 - config.sea_level;
    if (span <= 0.0) {
        return 0.0;
    }
    return std::clamp((elevation - config.sea_level) / span, 0.0, 1.0);
}

/**
 * @brief Converts a length in metres to grid units.
 *
 * The bridge between the two systems the generator speaks. Sizes are configured
 * and reasoned about in metres, because that is what they physically are;
 * geometry is computed in grid units, because that is the space the Voronoi
 * graph lives in. Everything crosses here.
 *
 * @param config Supplies `meters_per_grid_unit`.
 * @param meters The length in metres.
 * @return The same length in grid units.
 */
inline double meters_to_grid(const MapConfig& config, double meters) {
    return meters / config.meters_per_grid_unit;
}

/**
 * @brief Converts a length in grid units to metres.
 * @param config Supplies `meters_per_grid_unit`.
 * @param grid The length in grid units.
 * @return The same length in metres.
 */
inline double grid_to_meters(const MapConfig& config, double grid) {
    return grid * config.meters_per_grid_unit;
}

/**
 * @brief The render resolution the world extent and `meters_per_pixel` imply.
 *
 * `grid_size * meters_per_grid_unit` metres of world, divided by the ground each
 * pixel covers. A free function rather than a member so the arithmetic has
 * exactly one home, and so `image_size` cannot drift from the scale that
 * produced it.
 *
 * @param config Supplies the grid size and both scale factors.
 * @return The side length in pixels, at least 1.
 */
inline int derive_image_size(const MapConfig& config) {
    const double meters = static_cast<double>(config.grid_size) * config.meters_per_grid_unit;
    const double pixels = meters / (config.meters_per_pixel > 0.0 ? config.meters_per_pixel : 1.0);
    return std::max(1, static_cast<int>(pixels + 0.5));
}

/**
 * @brief The width of a river carrying a given volume, in metres.
 * @param config Supplies the width parameters.
 * @param volume The edge's river volume.
 * @return The river's width in metres.
 */
inline double river_width_meters(const MapConfig& config, int volume) {
    return config.river_width_base_m
         + config.river_width_per_volume_m * static_cast<double>(volume);
}

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
    return meters_to_grid(config, river_width_meters(config, volume));
}

/**
 * @brief The width of a road of a given class, in metres.
 *
 * The counterpart of `river_width_meters()`, and the one definition of how wide
 * a road is: the renderer scales it to pixels, and a consumer laying geometry
 * along a road reads the same number.
 *
 * @param config Supplies the per-class widths.
 * @param road_class The class of road.
 * @return The road's width in metres; 0 for `RoadClass::None`.
 */
inline double road_width_meters(const MapConfig& config, RoadClass road_class) {
    switch (road_class) {
        case RoadClass::Trail:   return config.trail_width_m;
        case RoadClass::Road:    return config.road_width_m;
        case RoadClass::Highway: return config.highway_width_m;
        case RoadClass::None:    break;
    }
    return 0.0;
}

/**
 * @brief The width of a road of a given class, in grid units.
 *
 * The grid-space counterpart, so a caller working in graph coordinates never has
 * to remember which system a width was configured in.
 *
 * @param config Supplies the per-class widths and the world scale.
 * @param road_class The class of road.
 * @return The road's width in grid units; 0 for `RoadClass::None`.
 */
inline double road_width_for(const MapConfig& config, RoadClass road_class) {
    return meters_to_grid(config, road_width_meters(config, road_class));
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
        glm::vec3(146, 206, 231),  // Ice
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
     * @brief Colour of a bridge or causeway parapet, drawn square across the road.
     *
     * Stone, not the near-black this used to share with a road casing. The
     * casing is gone: it was a dark outline under every stroke, added so a road
     * would not vanish against dark forest, and at 6 m wide over a hillshaded
     * composite a road no longer needs one. What it did instead was make every
     * road read as drawn-on ink rather than as ground.
     */
    glm::vec3 bridge_color = glm::vec3(112, 108, 102);
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
        return bridge_color;
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
