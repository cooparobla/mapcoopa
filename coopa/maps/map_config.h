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
    Rectangle, /**< @brief Axis-aligned, `width_m` by `height_m`. */
    Circle,    /**< @brief `diameter_m` across. */
    Triangle   /**< @brief Equilateral, `edge_length_m` a side, turned by `rotation`. */
};

/** @brief Number of distinct `MapShape` values. */
inline constexpr std::size_t k_map_shape_count = 3;

/**
 * @brief Maps a landmass shape to its serialisation name.
 * @param shape The shape to name.
 * @return A `snake_case` identifier, e.g. `"circle"`.
 */
inline std::string_view map_shape_name(MapShape shape) {
    switch (shape) {
        case MapShape::Rectangle: return "rectangle";
        case MapShape::Circle:    return "circle";
        case MapShape::Triangle:  return "triangle";
    }
    return "rectangle";
}

/**
 * @brief Resolves a serialisation name back to a shape.
 * @param name A name previously produced by `map_shape_name()`; `"rect"` is accepted too.
 * @return The matching shape, or `MapShape::Rectangle` if the name is unknown.
 */
inline MapShape map_shape_from_name(std::string_view name) {
    if (name == "circle")   return MapShape::Circle;
    if (name == "triangle") return MapShape::Triangle;
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
     */
    double river_depth_m = 1.0;
    /** @brief Additional depth per unit of river volume, in metres. */
    double river_depth_per_volume_m = 0.35;
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
 * @brief How far inside the landmass shape a point lies, in grid units.
 *
 * The one predicate the shape of the world comes from. `border_check_()` flags
 * any corner whose inset falls below `border_length`; the water pass forces those
 * cells to sea, and the elevation pass measures height as distance from them -- so
 * changing this function changes the coastline, the mountains, the regions and
 * the roads, all without any of them knowing shapes exist.
 *
 * A zero dimension spans the canvas, which is what makes the default rectangle
 * reproduce the pre-shape map exactly.
 *
 * @param config Supplies the shape, the grid size and the world scale.
 * @param x Horizontal grid position.
 * @param y Vertical grid position.
 * @return Positive inside the shape, negative outside, zero on its boundary.
 */
inline double shape_inset(const MapConfig& config, double x, double y) {
    const double grid = static_cast<double>(config.grid_size);
    const double centre = grid * 0.5;
    const double dx = x - centre;
    const double dy = y - centre;

    // A dimension of 0 means "as big as the canvas allows".
    const auto extent = [&config, grid](double meters) {
        return meters > 0.0 ? meters / config.meters_per_grid_unit : grid;
    };

    switch (config.shape.shape) {
        case MapShape::Circle: {
            const double radius = extent(config.shape.diameter_m) * 0.5;
            return radius - std::sqrt(dx * dx + dy * dy);
        }
        case MapShape::Triangle: {
            // Rotate into the triangle's own frame, then measure. Rotating the
            // query rather than the triangle keeps the shape description to one
            // number.
            const double c = std::cos(-config.shape.rotation);
            const double sn = std::sin(-config.shape.rotation);
            const double lx = dx * c - dy * sn;
            const double ly = dx * sn + dy * c;

            // Signed distance to an equilateral triangle centred on its
            // centroid, with `half_side` as the base half-width and k = sqrt(3).
            const double k = 1.7320508075688772;
            const double half_side = extent(config.shape.edge_length_m) * 0.5;
            if (half_side <= 0.0) {
                return -1.0;
            }
            // The formula is written for maths axes, where y climbs; image rows
            // descend. Flipping here is what puts the apex at the top of the
            // picture rather than the bottom.
            double px = std::abs(lx) - half_side;
            double py = -ly + half_side / k;
            if (px + k * py > 0.0) {
                const double folded_x = (px - k * py) * 0.5;
                const double folded_y = (-k * px - py) * 0.5;
                px = folded_x;
                py = folded_y;
            }
            px -= std::clamp(px, -2.0 * half_side, 0.0);
            // Distance, signed by which side of the folded edge the point landed
            // on. Positive inside, to match the other two shapes.
            return std::sqrt(px * px + py * py) * (py > 0.0 ? 1.0 : -1.0);
        }
        case MapShape::Rectangle:
            break;
    }

    const double half_width = extent(config.shape.width_m) * 0.5;
    const double half_height = extent(config.shape.height_m) * 0.5;
    return std::min(half_width - std::abs(dx), half_height - std::abs(dy));
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
