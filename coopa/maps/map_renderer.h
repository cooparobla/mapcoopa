/**
 * @file map_renderer.h
 * @brief Software renderers that turn a `MapGraph` into the map's image layers.
 */

#ifndef COOPA_MAPS_MAP_RENDERER_H
#define COOPA_MAPS_MAP_RENDERER_H

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string_view>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/maps/image.h>
#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @enum MapLayer
 * @brief One rendered view of a map.
 *
 * The generator writes one PNG per value. Splitting them apart is what lets a
 * consumer take the height field without the roads drawn over it, or the road
 * network without the terrain under it -- which a single composited image can
 * never give back.
 */
enum class MapLayer {
    Elevation,  /**< @brief Terrain height, greyscale. Land and lake beds; water is its own layer. */
    Water,      /**< @brief Water-surface height: sea, lakes and river channels. */
    Biomes,     /**< @brief Flat terrain colour, no overlays. */
    Roads,      /**< @brief The road network, transparent elsewhere. */
    Structures, /**< @brief Building footprints, transparent elsewhere. */
    Landmarks,  /**< @brief Settlement and landmark markers, transparent elsewhere. */
    Composite   /**< @brief Every layer above, hillshaded and blended. */
};

/** @brief Number of distinct `MapLayer` values. */
inline constexpr std::size_t k_map_layer_count = 7;

/**
 * @brief Maps a layer to the suffix its file takes, after the output prefix.
 * @param layer The layer to name.
 * @return A `snake_case` identifier, e.g. `"elevation"`.
 */
inline std::string_view map_layer_name(MapLayer layer) {
    switch (layer) {
        case MapLayer::Elevation:  return "elevation";
        case MapLayer::Water:      return "water";
        case MapLayer::Biomes:     return "biomes";
        case MapLayer::Roads:      return "roads";
        case MapLayer::Structures: return "structures";
        case MapLayer::Landmarks:  return "landmarks";
        case MapLayer::Composite:  return "composite";
    }
    return "composite";
}

/**
 * @brief True when a layer is an overlay, drawn on transparency rather than on ground.
 * @param layer The layer to classify.
 * @return True if it should be rendered RGBA.
 */
inline bool map_layer_has_alpha(MapLayer layer) {
    return layer == MapLayer::Roads || layer == MapLayer::Structures
        || layer == MapLayer::Landmarks;
}

/**
 * @struct CellGeometry
 * @brief Every cell's outline in pixel space, and the rows it spans.
 *
 * Built once and reused, for two reasons. Each of the seven layers otherwise
 * rebuilds the same 6,889 outlines from scratch -- `MapGraph::cell_outline()`
 * walks a cell's edges and concatenates their subdivided paths, which is not
 * free and does not depend on which layer is being drawn.
 *
 * And it is what lets a render be split across threads by row band without the
 * split costing more than it saves: a band needs to know which cells reach into
 * its rows, and `min_row`/`max_row` answer that from the *actual* outline rather
 * than from an estimate. The subdivided boundary bulges outside the straight
 * corner polygon -- toward the neighbouring cell's site -- so a range derived
 * from the corners alone would be wrong at exactly the seams that matter.
 */
struct CellGeometry {
    /** @brief One outline per cell, in pixel coordinates, indexed by `CenterId`. */
    std::vector<std::vector<MapPoint>> outlines;
    /** @brief First row each cell's outline touches. */
    std::vector<int> min_row;
    /** @brief Last row each cell's outline touches. */
    std::vector<int> max_row;

    /** @brief Whether geometry has been built for a given cell count. */
    bool covers(std::size_t cell_count) const { return outlines.size() == cell_count; }
};

/**
 * @struct HeightField
 * @brief A single-channel copy of the height raster, for shading only.
 *
 * One byte per pixel rather than three: the shading reads height alone, and
 * at 23 megapixels the two channels it would otherwise carry are 46 MB of
 * nothing.
 */
struct HeightField {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> values;

    /** @brief Reads a sample, clamping at the edges rather than falling off them. */
    double at(int x, int y) const {
        const int cx = std::clamp(x, 0, width - 1);
        const int cy = std::clamp(y, 0, height - 1);
        return static_cast<double>(
                   values[static_cast<std::size_t>(cy) * static_cast<std::size_t>(width)
                          + static_cast<std::size_t>(cx)])
             / 255.0;
    }
};

/**
 * @struct RenderSlice
 * @brief What part of a layer to draw, and the geometry to draw it from.
 *
 * Defaulted everywhere: a caller that wants a whole layer and does not care
 * about sharing geometry never mentions it.
 */
struct RenderSlice {
    /** @brief Rows this render may touch. */
    RowBand band;
    /** @brief Shared cell outlines, or null to build them locally. */
    const CellGeometry* geometry = nullptr;
    /**
     * @brief Shared height field for hillshading, or null to build one locally.
     *
     * Must be supplied when a hillshaded composite is drawn in bands, and the
     * reason is a factor of eighty. The field is smoothed and its gradient is
     * sampled across a whole cell, so both read well outside whatever band is
     * being drawn -- a band cannot build a correct one from its own rows. Left to
     * build its own, every band rebuilds the *entire* raster and re-blurs it,
     * which made a threaded hillshade render slower than a serial one.
     */
    const HeightField* height = nullptr;
};

/**
 * @class MapLayers
 * @brief Renders a `MapGraph` to any of its image layers.
 *
 * Every layer is drawn at the same scale, so they register pixel for pixel and
 * stack without resampling. At the default `meters_per_pixel` of 1 that scale is
 * one pixel to the metre: a 6 m road is 6 pixels wide because it is 6 metres
 * wide, and a pixel count read off a render is a measurement rather than a
 * rendering artefact.
 *
 * One layer is built at a time. At a 4.8 km world and 1 m/px a layer is 23
 * megapixels, which is ~92 MB held as RGBA; the generator renders, writes and
 * drops each in turn rather than holding all seven at once.
 */
class MapLayers {
public:
    /**
     * @brief Builds the shared cell geometry for a map at a given render scale.
     *
     * Serial by design -- it is the prologue a parallel render needs before it can
     * safely divide the image up, and it allocates, so it is not worth threading.
     *
     * @param graph The map to outline.
     * @param config Supplies the image and grid size the outlines are scaled to.
     * @return Outlines and row ranges for every cell.
     */
    static CellGeometry build_cell_geometry(const MapGraph& graph, const MapConfig& config) {
        const double scale = pixels_per_grid_unit_(config);
        CellGeometry geometry;
        geometry.outlines.resize(graph.centers.size());
        geometry.min_row.assign(graph.centers.size(), 0);
        geometry.max_row.assign(graph.centers.size(), -1);

        for (const MapCenter& center : graph.centers) {
            const std::size_t index = static_cast<std::size_t>(center.index);
            std::vector<MapPoint>& outline = geometry.outlines[index];
            outline = graph.cell_outline(center);
            int min_row = std::numeric_limits<int>::max();
            int max_row = std::numeric_limits<int>::min();
            for (MapPoint& point : outline) {
                point.x *= scale;
                point.y *= scale;
                min_row = std::min(min_row, static_cast<int>(std::floor(point.y)));
                max_row = std::max(max_row, static_cast<int>(std::ceil(point.y)));
            }
            if (outline.empty()) {
                min_row = 0;
                max_row = -1;
            }
            geometry.min_row[index] = min_row;
            geometry.max_row[index] = max_row;
        }
        return geometry;
    }

    /**
     * @brief Builds the smoothed height field a hillshaded composite needs.
     *
     * Only `CompositeShading::Hillshade` uses this; under the default elevation
     * shading the composite samples `MapGraph::elevation_at()` per pixel and
     * needs nothing precomputed. Build it once per image and hand it to every
     * band through `RenderSlice::height`.
     *
     * @param graph The map to measure.
     * @param config Supplies the render size and scale.
     * @param palette Passed through to the elevation render it is derived from.
     * @return The smoothed field, ready to share.
     */
    static HeightField build_height_field(const MapGraph& graph, const MapConfig& config,
                                          const BiomePalette& palette = BiomePalette{},
                                          const CellGeometry* geometry = nullptr) {
        const double scale = pixels_per_grid_unit_(config);
        const Image raster = elevation(graph, config, palette, RenderSlice{RowBand{}, geometry});
        return smoothed_height_(raster,
                                std::max(1, static_cast<int>(scale * k_shade_blur_cells)));
    }

    /**
     * @brief Allocates and clears the buffer a layer draws into, drawing nothing.
     *
     * Separate from drawing so a banded render can clear once and then have every
     * band draw into the same buffer. A band that cleared its own rows would be
     * fine; a band that cleared the whole image would erase its neighbours, and
     * the two are one easy mistake apart.
     *
     * @param layer Which layer the buffer is for; decides channels and clear colour.
     * @param config Supplies the image size.
     * @param palette Supplies the background colour for the layers that have one.
     * @return A cleared buffer, RGBA for an overlay layer and RGB otherwise.
     */
    static Image allocate(MapLayer layer, const MapConfig& config,
                          const BiomePalette& palette = BiomePalette{}) {
        Image image;
        if (map_layer_has_alpha(layer)) {
            image.reset(config.image_size, config.image_size, 4, glm::vec4(0.0f));
            return image;
        }
        const bool on_background = layer == MapLayer::Biomes || layer == MapLayer::Composite;
        image.reset(config.image_size, config.image_size, 3,
                    on_background ? palette.background_color : glm::vec3(0.0f));
        return image;
    }

    /**
     * @brief Draws a slice of a layer into a buffer `allocate()` already prepared.
     *
     * The entry point a threaded render uses: several calls with disjoint bands,
     * into one buffer, add up to exactly what one whole-image call would have
     * produced.
     *
     * @param image The target, already sized and cleared by `allocate()`.
     * @param layer Which layer to draw.
     * @param graph The map to draw.
     * @param config Supplies the render size, scale and feature widths.
     * @param palette Colours for each biome and overlay.
     * @param slice The rows to draw, and the shared cell geometry to draw from.
     */
    static void render_into(Image& image, MapLayer layer, const MapGraph& graph,
                            const MapConfig& config,
                            const BiomePalette& palette = BiomePalette{},
                            const RenderSlice& slice = RenderSlice{}) {
        switch (layer) {
            case MapLayer::Elevation:  draw_elevation_(image, graph, config, palette, slice); return;
            case MapLayer::Water:      draw_water_(image, graph, config, palette, slice); return;
            case MapLayer::Biomes:     draw_biomes_(image, graph, config, palette, slice); return;
            case MapLayer::Roads:      draw_roads_layer_(image, graph, config, palette, slice); return;
            case MapLayer::Structures: draw_structures_layer_(image, graph, config, palette, slice); return;
            case MapLayer::Landmarks:  draw_landmarks_layer_(image, graph, config, palette, slice); return;
            case MapLayer::Composite:  break;
        }
        draw_composite_(image, graph, config, palette, slice);
    }

    /**
     * @brief Renders one layer.
     * @param layer Which view to draw.
     * @param graph The map to draw.
     * @param config Supplies the image size, world scale and feature widths.
     * @param palette Colours for each biome and overlay.
     * @return The rendered image: RGBA for an overlay layer, RGB otherwise.
     */
    static Image render(MapLayer layer, const MapGraph& graph, const MapConfig& config,
                        const BiomePalette& palette = BiomePalette{},
                        const RenderSlice& slice = RenderSlice{}) {
        switch (layer) {
            case MapLayer::Elevation:  return elevation(graph, config, palette, slice);
            case MapLayer::Water:      return water(graph, config, palette, slice);
            case MapLayer::Biomes:     return biomes(graph, config, palette, slice);
            case MapLayer::Roads:      return roads(graph, config, palette, slice);
            case MapLayer::Structures: return structures(graph, config, palette, slice);
            case MapLayer::Landmarks:  return landmarks(graph, config, palette, slice);
            case MapLayer::Composite:  break;
        }
        return composite(graph, config, palette, slice);
    }

    /**
     * @brief Terrain height as greyscale, black at sea level and white at the summit.
     *
     * Shaded per pixel from the cell's corner heights rather than filled flat: a
     * flat fill draws the Voronoi tessellation, not the terrain. Rivers are *not*
     * dimmed into it any more -- water has its own layer, and a height field with
     * channels cut into it has stopped being a height field.
     */
    static Image elevation(const MapGraph& graph, const MapConfig& config,
                           const BiomePalette& palette = BiomePalette{},
                           const RenderSlice& slice = RenderSlice{}) {
        Image image = allocate(MapLayer::Elevation, config, palette);
        draw_elevation_(image, graph, config, palette, slice);
        return image;
    }

    /**
     * @brief Height of the water surface: sea, lakes and river channels.
     *
     * Dry land is black, meaning *no water* rather than water at zero. The sea
     * sits near 0 and a lake at the height of the basin holding it, so a consumer
     * can flood a terrain mesh to these values directly. River channels are
     * stroked along their smoothed centrelines at the height of the ground they
     * run over, widening with volume.
     */
    static Image water(const MapGraph& graph, const MapConfig& config,
                       const BiomePalette& palette = BiomePalette{},
                       const RenderSlice& slice = RenderSlice{}) {
        Image image = allocate(MapLayer::Water, config, palette);
        draw_water_(image, graph, config, palette, slice);
        return image;
    }

    /** @brief Flat biome colour per cell, tinted by region when `show_regions` is set. */
    static Image biomes(const MapGraph& graph, const MapConfig& config,
                        const BiomePalette& palette = BiomePalette{},
                        const RenderSlice& slice = RenderSlice{}) {
        Image image = allocate(MapLayer::Biomes, config, palette);
        draw_biomes_(image, graph, config, palette, slice);
        return image;
    }

    /**
     * @brief The road network on transparency, stroked at its real width.
     *
     * Trails, then roads, then highways, so a highway is never interrupted by the
     * trail joining it. No casing: a 6 m road over shaded ground reads as a road
     * without an outline, and the outline was what made it read as drawn-on ink.
     */
    static Image roads(const MapGraph& graph, const MapConfig& config,
                       const BiomePalette& palette = BiomePalette{},
                       const RenderSlice& slice = RenderSlice{}) {
        Image image = allocate(MapLayer::Roads, config, palette);
        draw_roads_layer_(image, graph, config, palette, slice);
        return image;
    }

    /** @brief Building footprints on transparency, as the rotated quads they are. */
    static Image structures(const MapGraph& graph, const MapConfig& config,
                            const BiomePalette& palette = BiomePalette{},
                            const RenderSlice& slice = RenderSlice{}) {
        Image image = allocate(MapLayer::Structures, config, palette);
        draw_structures_layer_(image, graph, config, palette, slice);
        return image;
    }

    /** @brief Settlement and landmark markers on transparency. */
    static Image landmarks(const MapGraph& graph, const MapConfig& config,
                           const BiomePalette& palette = BiomePalette{},
                           const RenderSlice& slice = RenderSlice{}) {
        Image image = allocate(MapLayer::Landmarks, config, palette);
        draw_landmarks_layer_(image, graph, config, palette, slice);
        return image;
    }

    /**
     * @brief Everything at once: lit terrain, then water, roads, buildings, markers.
     *
     * The light comes from the elevation field the map already carries rather
     * than from a second noise source, so it can never disagree with the
     * heightmap layer. Flat biome polygons read as a diagram; lit ones read as
     * ground.
     *
     * `MapConfig::composite_shading` chooses between brightness following
     * *height* -- the default, so high ground is pale -- and brightness
     * following *slope*, which sculpts the relief but is blind to altitude.
     */
    static Image composite(const MapGraph& graph, const MapConfig& config,
                           const BiomePalette& palette = BiomePalette{},
                           const RenderSlice& slice = RenderSlice{}) {
        Image image = allocate(MapLayer::Composite, config, palette);
        draw_composite_(image, graph, config, palette, slice);
        return image;
    }

private:
    /** @brief Draws the elevation layer into an already-allocated buffer. */
    static void draw_elevation_(Image& image, const MapGraph& graph, const MapConfig& config,
                         const BiomePalette& palette, const RenderSlice& slice) {
        (void)palette;
        const double scale = pixels_per_grid_unit_(config);
        const double inverse_scale = 1.0 / scale;
        Outlines outlines(graph, config, slice);
        for (const MapCenter& center : graph.centers) {
            const std::vector<MapPoint>& outline = outlines.of(center, slice);
            if (outline.empty()) {
                continue;
            }
            fill_polygon_shaded(image, outline,
                [&graph, &center, inverse_scale](double px, double py) {
                    const double height = graph.elevation_at(center, px * inverse_scale,
                                                             py * inverse_scale);
                    const float grey = static_cast<float>(std::clamp(height, 0.0, 1.0) * 255.0);
                    return glm::vec3(grey, grey, grey);
                }, slice.band);
        }
    }

    /** @brief Draws the water layer into an already-allocated buffer. */
    static void draw_water_(Image& image, const MapGraph& graph, const MapConfig& config,
                         const BiomePalette& palette, const RenderSlice& slice) {
        (void)palette;
        Outlines outlines(graph, config, slice);
        for (const MapCenter& center : graph.centers) {
            if (!center.water) {
                continue;
            }
            const std::vector<MapPoint>& outline = outlines.of(center, slice);
            if (outline.empty()) {
                continue;
            }
            fill_polygon(image, outline, height_grey_(center.elevation), slice.band);
        }

        for (const MapRiver& river : graph.rivers) {
            stroke_river_(image, graph, river, config,
                [](const MapCorner& corner) { return height_grey_(corner.elevation); },
                slice.band);
        }
    }

    /** @brief Draws the biomes layer into an already-allocated buffer. */
    static void draw_biomes_(Image& image, const MapGraph& graph, const MapConfig& config,
                             const BiomePalette& palette, const RenderSlice& slice) {
        Outlines outlines(graph, config, slice);
        for (const MapCenter& center : graph.centers) {
            const std::vector<MapPoint>& outline = outlines.of(center, slice);
            if (outline.empty()) {
                continue;
            }
            fill_polygon(image, outline, biome_color_(graph, center, config, palette),
                         slice.band);
        }
    }

    /** @brief Draws the roads layer into an already-allocated buffer. */
    static void draw_roads_layer_(Image& image, const MapGraph& graph, const MapConfig& config,
                                  const BiomePalette& palette, const RenderSlice& slice) {
        draw_roads_(image, graph, config, palette, slice.band);
    }

    /** @brief Draws the structures layer into an already-allocated buffer. */
    static void draw_structures_layer_(Image& image, const MapGraph& graph, const MapConfig& config,
                                       const BiomePalette& palette, const RenderSlice& slice) {
        draw_structures_(image, graph, config, palette, slice.band);
    }

    /** @brief Draws the landmarks layer into an already-allocated buffer. */
    static void draw_landmarks_layer_(Image& image, const MapGraph& graph, const MapConfig& config,
                                      const BiomePalette& palette, const RenderSlice& slice) {
        draw_markers_(image, graph, config, palette, slice.band);
    }

    /** @brief Draws the composite layer into an already-allocated buffer. */
    static void draw_composite_(Image& image, const MapGraph& graph, const MapConfig& config,
                         const BiomePalette& palette, const RenderSlice& slice) {
        (void)palette;
        const double scale = pixels_per_grid_unit_(config);
        const double inverse_scale = 1.0 / scale;
        Outlines outlines(graph, config, slice);

        // Only the slope mode needs the rasterised height field, and it is the
        // expensive half of this function -- a second full-resolution pass plus a
        // blur. It is always built from the *whole* image, never from the band:
        // the blur and the gradient both read outside the band, so a band-local
        // field would make one band's output depend on which others had run.
        //
        // Which is exactly why a caller drawing in bands must pass one in. Built
        // here per band instead, the whole raster is rebuilt once per band.
        HeightField local;
        const HeightField* height = slice.height;
        int baseline = 1;
        if (config.composite_shading == CompositeShading::Hillshade) {
            if (height == nullptr) {
                local = build_height_field(graph, config, palette, slice.geometry);
                height = &local;
            }
            baseline = std::max(1, static_cast<int>(scale * k_shade_baseline_cells));
        }

        for (const MapCenter& center : graph.centers) {
            const std::vector<MapPoint>& outline = outlines.of(center, slice);
            if (outline.empty()) {
                continue;
            }
            const glm::vec3 base = biome_color_(graph, center, config, palette);
            // Water is filled flat under either mode: a lake surface is level,
            // and shading it from the basin beneath would ripple still water.
            if (center.water) {
                fill_polygon(image, outline, base, slice.band);
                continue;
            }
            if (config.composite_shading == CompositeShading::Hillshade) {
                fill_polygon_shaded(image, outline,
                    [height, base, baseline](double px, double py) {
                        return base * hillshade_(*height, static_cast<int>(px),
                                                 static_cast<int>(py), baseline);
                    }, slice.band);
            } else {
                fill_polygon_shaded(image, outline,
                    [&graph, &center, base, inverse_scale](double px, double py) {
                        return base * elevation_shade_(graph, center, px * inverse_scale,
                                                       py * inverse_scale);
                    }, slice.band);
            }
        }

        for (const MapRiver& river : graph.rivers) {
            stroke_river_(image, graph, river, config,
                          [&palette](const MapCorner&) { return palette.river_color; },
                          slice.band);
        }
        draw_roads_(image, graph, config, palette, slice.band);
        draw_structures_(image, graph, config, palette, slice.band);
        draw_markers_(image, graph, config, palette, slice.band);
    }

    /** @brief Light direction for the hillshade: from the north-west, 45 degrees up. */
    static constexpr double k_light_x = -0.5773502692;
    static constexpr double k_light_y = -0.5773502692;
    static constexpr double k_light_z = 0.5773502692;
    /**
     * @brief How much the height field is exaggerated before its normal is taken.
     *
     * Elevation is normalised to `[0, 1]` across a 4.8 km world, so the true
     * gradients are numerically tiny -- a whole grid cell of the steepest ground
     * climbs a few thousandths -- and an unexaggerated normal points almost
     * straight up everywhere, giving a uniform grey wash. This is a drawing
     * choice, and deliberately does not feed back into the height data.
     */
    static constexpr double k_relief_exaggeration = 600.0;
    /**
     * @brief Gradient baseline for the hillshade, as a fraction of a cell.
     *
     * Wide enough to step over the per-cell interpolation and read the shape of
     * the land; narrow enough that a ridge still casts an edge.
     */
    static constexpr double k_shade_baseline_cells = 0.5;
    /** @brief Box-blur radius applied to the height field before shading, in cells. */
    static constexpr double k_shade_blur_cells = 0.45;
    /**
     * @brief Multiplier applied to a biome colour at sea level, and at the summit.
     *
     * A wide range on purpose. Elevation is rank-remapped by the elevation pass
     * so the land spans most of `[0, 1]`, and a timid range would leave a
     * continent looking uniformly flat -- the one thing this mode exists to
     * avoid. Values above 1 clamp per channel on the way to the buffer, so a
     * biome already near white simply saturates rather than wrapping.
     */
    static constexpr double k_elevation_shade_min = 0.68;
    static constexpr double k_elevation_shade_max = 1.24;
    /** @brief Darkest the hillshade may drive a biome colour. */
    static constexpr float k_shade_min = 0.58f;
    /** @brief Brightest the hillshade may drive a biome colour. */
    static constexpr float k_shade_max = 1.14f;
    /** @brief Contrast applied to the Lambertian term before clamping. */
    static constexpr float k_shade_gain = 1.28f;
    /** @brief How far a bridge parapet reaches either side of the road, in road widths. */
    static constexpr double k_bridge_span = 0.9;
    /** @brief Marker half-width in metres for a capital, a town and a village. */
    static constexpr double k_capital_marker_m = 12.0;
    static constexpr double k_town_marker_m = 8.0;
    static constexpr double k_village_marker_m = 5.0;
    /** @brief Marker half-width in metres for a wonder, and for every other landmark. */
    static constexpr double k_wonder_marker_m = 10.0;
    static constexpr double k_landmark_marker_m = 6.0;

    /** @brief Pixels per grid unit; equals `meters_per_grid_unit / meters_per_pixel`. */
    static double pixels_per_grid_unit_(const MapConfig& config) {
        return static_cast<double>(config.image_size) / static_cast<double>(config.grid_size);
    }

    /**
     * @brief Converts a width in grid units to a stroke half-width in pixels.
     *
     * Kept fractional. `draw_line()` paints by distance to the segment, so it can
     * honour a half-width of 3.0 exactly and draw a 6 m road 6 pixels across --
     * where a pixel-centred brush could only manage the nearest odd number.
     */
    static double half_width_pixels_(double width_grid, double scale) {
        return std::max(0.0, width_grid * scale * 0.5);
    }

    /** @brief A normalised height as a greyscale colour. */
    static glm::vec3 height_grey_(double height) {
        const float grey = static_cast<float>(std::clamp(height, 0.0, 1.0) * 255.0);
        return glm::vec3(grey, grey, grey);
    }

    /** @brief Converts a length in metres to whole pixels, at least 1. */
    static int meters_to_pixels_(double meters, const MapConfig& config) {
        const double per_pixel = config.meters_per_pixel > 0.0 ? config.meters_per_pixel : 1.0;
        return std::max(1, static_cast<int>(meters / per_pixel));
    }

    /**
     * @class Outlines
     * @brief Hands out a cell's pixel-space outline, shared if the caller has one.
     *
     * Two jobs in one place. It hides whether the geometry was precomputed -- a
     * threaded render shares one `CellGeometry` across every band and every layer,
     * a lone call builds outlines as it goes -- and it performs the band check,
     * returning an empty outline for a cell that does not reach into the rows
     * being drawn.
     *
     * The band check needs the *real* outline extent, which is why it lives here
     * rather than being estimated from the corners: a subdivided boundary bulges
     * outside the straight corner polygon, and a cell wrongly skipped leaves a
     * hairline of background along a band seam.
     */
    class Outlines {
    public:
        Outlines(const MapGraph& graph, const MapConfig& config, const RenderSlice& slice)
            : scale_(pixels_per_grid_unit_(config)),
              shared_(slice.geometry != nullptr && slice.geometry->covers(graph.centers.size())
                          ? slice.geometry
                          : nullptr),
              graph_(graph) {}

        /**
         * @brief The cell's outline, or an empty one if it misses the band.
         * @param center The cell to outline.
         * @param slice The band being drawn.
         * @return A reference valid until the next call for a different cell.
         */
        const std::vector<MapPoint>& of(const MapCenter& center, const RenderSlice& slice) {
            const std::size_t index = static_cast<std::size_t>(center.index);
            if (shared_ != nullptr) {
                if (!slice.band.intersects(shared_->min_row[index], shared_->max_row[index])) {
                    return empty_;
                }
                return shared_->outlines[index];
            }

            scratch_ = graph_.cell_outline(center);
            int min_row = std::numeric_limits<int>::max();
            int max_row = std::numeric_limits<int>::min();
            for (MapPoint& point : scratch_) {
                point.x *= scale_;
                point.y *= scale_;
                min_row = std::min(min_row, static_cast<int>(std::floor(point.y)));
                max_row = std::max(max_row, static_cast<int>(std::ceil(point.y)));
            }
            if (scratch_.empty() || !slice.band.intersects(min_row, max_row)) {
                return empty_;
            }
            return scratch_;
        }

    private:
        double scale_ = 1.0;
        const CellGeometry* shared_ = nullptr;
        const MapGraph& graph_;
        std::vector<MapPoint> scratch_;
        std::vector<MapPoint> empty_;
    };

    /** @brief A cell's biome colour, tinted by its region when the config asks for it. */
    static glm::vec3 biome_color_(const MapGraph& graph, const MapCenter& center,
                                  const MapConfig& config, const BiomePalette& palette) {
        glm::vec3 color = palette.color_for(center.biome);
        if (config.show_regions && center.region != k_invalid_id
            && static_cast<std::size_t>(center.region) < graph.regions.size()) {
            // Tint rather than replace: a political map that hides the terrain
            // under flat colour stops being a map of the world.
            const glm::vec3& tint = graph.regions[static_cast<std::size_t>(center.region)].color;
            color = color * (1.0f - config.region_tint) + tint * config.region_tint;
        }
        return color;
    }

    /**
     * @brief Brightness factor from height alone: the higher the ground, the paler it is.
     *
     * Read straight from `MapGraph::elevation_at()`, which is interpolated from
     * the cell's own corners and agrees with its neighbour along a shared edge --
     * so the result is continuous across the map and needs none of the machinery
     * the slope mode does. The cusps that ruin a *gradient* taken from this
     * function are harmless to its *value*, which is all this reads.
     *
     * Being a function of elevation and nothing else, the same height comes out
     * the same brightness wherever it appears, which is what makes the shading
     * legible rather than merely decorative.
     *
     * @param graph The map being shaded.
     * @param center The cell the pixel falls in.
     * @param x Horizontal grid position.
     * @param y Vertical grid position.
     * @return A multiplier for the surface colour, in
     *         `[k_elevation_shade_min, k_elevation_shade_max]`.
     */
    static float elevation_shade_(const MapGraph& graph, const MapCenter& center, double x,
                                  double y) {
        const double height = std::clamp(graph.elevation_at(center, x, y), 0.0, 1.0);
        return static_cast<float>(k_elevation_shade_min
                                  + (k_elevation_shade_max - k_elevation_shade_min) * height);
    }

    /**
     * @brief Extracts the height raster and box-blurs it, for shading only.
     *
     * The blur is what removes the cell faceting. Elevation inside a cell is
     * interpolated from that cell's own corners, so the surface is continuous
     * across a shared edge but its *slope* is not -- and a hillshade is a
     * function of slope, so every cell boundary shows up as a crease. Widening
     * the gradient stencil alone only softens it; smoothing the field first
     * removes it, and the terrain-scale relief survives because the blur is a
     * fraction of a cell while the landforms are many cells across.
     *
     * Separable, two passes with a running sum, so cost is linear in pixels
     * rather than in the square of the radius. The blurred field is never
     * written out -- the elevation layer on disk is the unsmoothed original.
     *
     * @param source The rendered elevation layer.
     * @param radius Box radius in pixels.
     * @return The smoothed single-channel field.
     */
    static HeightField smoothed_height_(const Image& source, int radius) {
        HeightField field;
        field.width = source.width;
        field.height = source.height;
        const std::size_t count =
            static_cast<std::size_t>(source.width) * static_cast<std::size_t>(source.height);
        field.values.resize(count);

        std::vector<unsigned char> scratch(count);
        for (int y = 0; y < source.height; ++y) {
            for (int x = 0; x < source.width; ++x) {
                scratch[static_cast<std::size_t>(y) * static_cast<std::size_t>(source.width)
                        + static_cast<std::size_t>(x)] =
                    static_cast<unsigned char>(source.color_at(x, y).r);
            }
        }

        const auto blur_axis = [radius](const std::vector<unsigned char>& in,
                                        std::vector<unsigned char>& out, int width, int height,
                                        bool horizontal) {
            const int along = horizontal ? width : height;
            const int across = horizontal ? height : width;
            for (int a = 0; a < across; ++a) {
                const auto read = [&in, width, a, horizontal](int b) {
                    const std::size_t index =
                        horizontal ? static_cast<std::size_t>(a) * static_cast<std::size_t>(width)
                                         + static_cast<std::size_t>(b)
                                   : static_cast<std::size_t>(b) * static_cast<std::size_t>(width)
                                         + static_cast<std::size_t>(a);
                    return static_cast<int>(in[index]);
                };
                int total = 0;
                for (int b = -radius; b <= radius; ++b) {
                    total += read(std::clamp(b, 0, along - 1));
                }
                const int window = 2 * radius + 1;
                for (int b = 0; b < along; ++b) {
                    const std::size_t index =
                        horizontal ? static_cast<std::size_t>(a) * static_cast<std::size_t>(width)
                                         + static_cast<std::size_t>(b)
                                   : static_cast<std::size_t>(b) * static_cast<std::size_t>(width)
                                         + static_cast<std::size_t>(a);
                    out[index] = static_cast<unsigned char>(total / window);
                    total -= read(std::clamp(b - radius, 0, along - 1));
                    total += read(std::clamp(b + radius + 1, 0, along - 1));
                }
            }
        };

        blur_axis(scratch, field.values, source.width, source.height, true);
        blur_axis(field.values, scratch, source.width, source.height, false);
        field.values.swap(scratch);
        return field;
    }

    /**
     * @brief Lambertian shading factor from the slope of the rasterised height field.
     *
     * The gradient is taken over a baseline of most of a cell, not over one
     * pixel, and over a field already smoothed by `smoothed_height_()`. A
     * one-pixel gradient on the raw raster picks up the inverse-distance
     * weighting inside each cell -- which is cusped at the corners -- and lights
     * every cell as its own little dome. Between them the blur and the wide
     * stencil step over that structure entirely and leave the slope of the
     * landscape, which is the thing worth drawing.
     *
     * Edges clamp rather than read off the raster, so the map border does not
     * light up as a cliff down to zero.
     *
     * @param height The smoothed height field.
     * @param x Column in pixels.
     * @param y Row in pixels.
     * @param baseline Half the gradient stencil, in pixels.
     * @return A multiplier for the surface colour, in `[k_shade_min, k_shade_max]`.
     */
    static float hillshade_(const HeightField& height, int x, int y, int baseline) {
        const double left = height.at(x - baseline, y);
        const double right = height.at(x + baseline, y);
        const double up = height.at(x, y - baseline);
        const double down = height.at(x, y + baseline);

        const double span = 2.0 * static_cast<double>(baseline);
        const double dx = (right - left) / span * k_relief_exaggeration;
        const double dy = (down - up) / span * k_relief_exaggeration;
        // Normal of the exaggerated height field is (-dx, -dy, 1); dotting it
        // with the light and dividing by its length normalises in one step.
        const double length = std::sqrt(dx * dx + dy * dy + 1.0);
        const double lambert = (-dx * k_light_x - dy * k_light_y + k_light_z) / length;

        return std::clamp(static_cast<float>(lambert) * k_shade_gain, k_shade_min, k_shade_max);
    }

    /**
     * @brief Strokes a river along its smoothed centreline.
     *
     * Width comes from the volume carried at each corner, so a channel widens
     * toward its mouth as tributaries join it. The shader is handed the nearest
     * corner so a caller can colour by the height of the ground there.
     *
     * @tparam Shader Callable of `(const MapCorner&) -> Color`.
     */
    template <typename Shader>
    static void stroke_river_(Image& image, const MapGraph& graph, const MapRiver& river,
                              const MapConfig& config, Shader&& shader,
                              const RowBand& band = RowBand{}) {
        if (river.points.size() < 2 || river.corners.empty()) {
            return;
        }
        const double scale = pixels_per_grid_unit_(config);
        const std::size_t segments = river.points.size() - 1;
        const std::size_t last_corner = river.corners.size() - 1;

        for (std::size_t i = 0; i < segments; ++i) {
            // Map a smoothed point back onto the corner chain it came from:
            // corner-cutting multiplies the point count, so the relationship is
            // proportional rather than one-to-one.
            const std::size_t corner_index =
                std::min(last_corner, river.corners.size() * i / segments);
            const MapCorner& corner =
                graph.corners[static_cast<std::size_t>(river.corners[corner_index])];

            draw_line(image, river.points[i].x * scale, river.points[i].y * scale,
                      river.points[i + 1].x * scale, river.points[i + 1].y * scale,
                      half_width_pixels_(river_width(config, corner.river), scale),
                      shader(corner), band);
        }
    }

    /** @brief Strokes every road run, least travelled first, then the bridges over them. */
    static void draw_roads_(Image& image, const MapGraph& graph, const MapConfig& config,
                            const BiomePalette& palette, const RowBand& band = RowBand{}) {
        const double scale = pixels_per_grid_unit_(config);
        static constexpr std::array<RoadClass, 3> k_order = {RoadClass::Trail, RoadClass::Road,
                                                             RoadClass::Highway};
        for (const RoadClass road_class : k_order) {
            const double half_width = half_width_pixels_(road_width_for(config, road_class), scale);
            const glm::vec3& color = palette.color_for(road_class);

            if (graph.roads.empty()) {
                // A map loaded from a document written before roads were traced
                // into runs still has its per-edge flags. Stroke those straight
                // rather than drawing nothing.
                for (const MapEdge& edge : graph.edges) {
                    if (edge.road_class != road_class || edge.d0 == k_invalid_id
                        || edge.d1 == k_invalid_id) {
                        continue;
                    }
                    draw_segment_(image, graph.centers[static_cast<std::size_t>(edge.d0)].point,
                                  graph.centers[static_cast<std::size_t>(edge.d1)].point, scale,
                                  half_width, color, band);
                }
                continue;
            }
            for (const MapRoad& run : graph.roads) {
                if (run.road_class != road_class) {
                    continue;
                }
                for (std::size_t i = 0; i + 1 < run.points.size(); ++i) {
                    draw_segment_(image, run.points[i], run.points[i + 1], scale, half_width,
                                  color, band);
                }
            }
        }

        for (const MapEdge& edge : graph.edges) {
            if (edge.bridge) {
                draw_bridge_(image, graph, edge, config, scale, palette, band);
            }
        }
    }

    /** @brief Fills every building footprint as the rotated quad it is. */
    static void draw_structures_(Image& image, const MapGraph& graph, const MapConfig& config,
                                 const BiomePalette& palette,
                                 const RowBand& band = RowBand{}) {
        const double scale = pixels_per_grid_unit_(config);
        std::vector<MapPoint> footprint(4);
        for (const MapTown& town : graph.towns) {
            // The rotated quad, not an axis-aligned blob: the yaw is part of what
            // the generator guarantees, and a square marker would hide whether
            // buildings actually line up along their street.
            for (const MapBuilding& building : town.buildings) {
                const std::array<MapPoint, 4> corners = building_corners(building);
                for (std::size_t i = 0; i < 4; ++i) {
                    footprint[i] = {corners[i].x * scale, corners[i].y * scale};
                }
                fill_polygon(image, footprint, palette.building_color, band);
            }
        }
    }

    /** @brief Draws a marker per settlement and per landmark. */
    static void draw_markers_(Image& image, const MapGraph& graph, const MapConfig& config,
                              const BiomePalette& palette, const RowBand& band = RowBand{}) {
        const double scale = pixels_per_grid_unit_(config);
        for (const MapTown& town : graph.towns) {
            draw_marker_(image, town.point, scale, marker_radius_(town.tier, config),
                         palette.town_color, band);
        }
        for (const MapLandmark& landmark : graph.landmarks) {
            draw_landmark_(image, landmark, scale, config, palette, band);
        }
    }

    /** @brief Strokes one grid-space segment, converting to pixels on the way. */
    template <typename Color>
    static void draw_segment_(Image& image, const MapPoint& from, const MapPoint& to, double scale,
                              double half_width, const Color& color,
                              const RowBand& band = RowBand{}) {
        draw_line(image, from.x * scale, from.y * scale, to.x * scale, to.y * scale, half_width,
                  color, band);
    }

    /**
     * @brief Marker half-width in pixels for each settlement size class.
     *
     * Sized in metres, so a marker means the same thing at any resolution: a
     * capital reads about 25 m across, which is a settlement-sized dot on a 1 m
     * map. A fixed pixel radius would instead shrink relative to the world every
     * time the render got finer.
     */
    static int marker_radius_(TownTier tier, const MapConfig& config) {
        switch (tier) {
            case TownTier::Capital: return meters_to_pixels_(k_capital_marker_m, config);
            case TownTier::Town:    return meters_to_pixels_(k_town_marker_m, config);
            case TownTier::Village: break;
        }
        return meters_to_pixels_(k_village_marker_m, config);
    }

    /**
     * @brief Draws a landmark, shaped so its kind is readable at a glance.
     *
     * Natural features get a diamond, works a square, a region's wonder a larger
     * diamond -- enough to tell them apart on a preview without a legend.
     */
    static void draw_landmark_(Image& image, const MapLandmark& landmark, double scale,
                               const MapConfig& config, const BiomePalette& palette,
                               const RowBand& band = RowBand{}) {
        const int cx = static_cast<int>(landmark.point.x * scale);
        const int cy = static_cast<int>(landmark.point.y * scale);
        const bool natural = landmark.kind != LandmarkKind::Ruins
                          && landmark.kind != LandmarkKind::StandingStones
                          && landmark.kind != LandmarkKind::Monolith
                          && landmark.kind != LandmarkKind::Wreck
                          && landmark.kind != LandmarkKind::Tower
                          && landmark.kind != LandmarkKind::Shrine;
        const int radius = meters_to_pixels_(
            landmark.kind == LandmarkKind::Wonder ? k_wonder_marker_m : k_landmark_marker_m,
            config);
        const glm::vec3& color =
            natural ? palette.landmark_natural_color : palette.landmark_built_color;

        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                // Diamond for natural, square for built.
                if (natural && std::abs(x) + std::abs(y) > radius) {
                    continue;
                }
                if (cy + y < band.begin || cy + y >= band.end) {
                    continue;
                }
                image.set_pixel(cx + x, cy + y, color);
            }
        }
    }

    /** @brief Draws a filled square centred on a grid-space point. */
    static void draw_marker_(Image& image, const MapPoint& point, double scale, int radius,
                             const glm::vec3& color, const RowBand& band = RowBand{}) {
        const int cx = static_cast<int>(point.x * scale);
        const int cy = static_cast<int>(point.y * scale);
        for (int y = -radius; y <= radius; ++y) {
            if (cy + y < band.begin || cy + y >= band.end) {
                continue;
            }
            for (int x = -radius; x <= radius; ++x) {
                image.set_pixel(cx + x, cy + y, color);
            }
        }
    }

    /**
     * @brief Draws the parapet of a bridge or causeway, across the road.
     *
     * Placed at the midpoint of the Delaunay edge, which is exactly where the
     * road crosses the water: the dual Voronoi edge the river runs along lies on
     * the perpendicular bisector of `d0`-`d1`, so it meets the road there and
     * nowhere else. The tick runs along that bisector -- square across the
     * road -- so a crossing reads as a crossing at a glance, the same way
     * `draw_landmark_()` shapes a marker to its kind.
     */
    static void draw_bridge_(Image& image, const MapGraph& graph, const MapEdge& edge,
                             const MapConfig& config, double scale, const BiomePalette& palette,
                             const RowBand& band = RowBand{}) {
        if (edge.d0 == k_invalid_id || edge.d1 == k_invalid_id) {
            return;
        }
        const MapPoint& d0 = graph.centers[static_cast<std::size_t>(edge.d0)].point;
        const MapPoint& d1 = graph.centers[static_cast<std::size_t>(edge.d1)].point;

        const double dx = d1.x - d0.x;
        const double dy = d1.y - d0.y;
        const double length = std::hypot(dx, dy);
        if (length == 0.0) {
            return;
        }
        const double width = road_width_for(config, edge.road_class);
        const double reach = width * k_bridge_span;
        const double offset_x = -dy / length * reach;
        const double offset_y = dx / length * reach;
        const MapPoint crossing{(d0.x + d1.x) * 0.5, (d0.y + d1.y) * 0.5};

        draw_segment_(image, {crossing.x + offset_x, crossing.y + offset_y},
                      {crossing.x - offset_x, crossing.y - offset_y}, scale,
                      half_width_pixels_(width * 0.5, scale), palette.bridge_color, band);
    }
};

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_MAP_RENDERER_H
