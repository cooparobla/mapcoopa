/**
 * @file map_viewer.cpp
 * @brief A uicoopa GUI front-end for the map generator: edit, refresh, look, export.
 *
 * The CLI sibling (examples/map_generator.cpp) generates once from ~40 flags and exits, so
 * judging a setting means re-running it and opening a PNG somewhere else. This viewer keeps
 * a map on screen and regenerates it on demand: a File menu for importing and exporting
 * .yaml and .png, a collapsible left panel of the settings that most change what a map
 * looks like, and a layer selector over the render.
 *
 * Build:   cbuild                (the mapcoopa_viewer target; -DMAPCOOPA_WITH_VIEWER=OFF skips it)
 * Run:     ./build/mapcoopa_viewer
 *
 * Honours the same scripted-capture env vars every uicoopa demo does, via
 * ContextConfig::from_env(): MAX_FRAMES=<n> exits after n frames, ONESHOT=1 renders a
 * single frame, SCREENSHOT_NAME=<name> names the PNG written to output/ on exit. Plus
 * SEED=<n> to start on a chosen seed, and THEME=<name> to pick a uicoopa theme.
 *
 * ## Two resolutions, on purpose
 *
 * `MapConfig::image_size` defaults to 3000, and a 3000x3000 composite is seconds of
 * software rasterising and 36 MB of texture -- fine for an export, useless for a view that
 * has to feel interactive. So the on-screen render uses its own smaller config derived from
 * the live one (see make_preview_config()), and every export keeps using the real
 * image_size. The preview resolution is itself a control in the panel.
 *
 * ## Nothing blocks the frame loop
 *
 * Generation and preview rendering both run on a shared coopa::job::JobEngine and are polled
 * through MapTask, so the UI keeps drawing (and the progress bar keeps moving) while they
 * run. MapTask's destructor cancels and waits, which is what makes retiring a stale task as
 * simple as resetting the optional that holds it.
 */

// The one raw Vulkan include here, for save_screenshot()'s framebuffer create/destroy pair
// -- gfxcoopa has no Framebuffer wrapper. Same single escape every uicoopa demo makes, and
// for the same reason; see that function's doc.
#include <volk/volk.h>

#include <root_directory.h>

#include <gfxcoopa/app/context.h>
#include <gfxcoopa/memory/image.h>
#include <gfxcoopa/memory/buffer.h>
#include <gfxcoopa/command/command_buffer.h>
#include <gfxcoopa/util/image_readback.h>
#include <gfxcoopa/engine/data/texture.h>

#include <uicoopa/layout/rect_transform.h>
#include <uicoopa/layout/canvas.h>
#include <uicoopa/layout/canvas_scaler.h>
#include <uicoopa/render/draw_list.h>
#include <uicoopa/render/ui_pass.h>
#include <uicoopa/render/icon_library.h>
#include <uicoopa/widgets/image.h>
#include <uicoopa/widgets/text.h>
#include <uicoopa/text/font.h>
#include <uicoopa/text/font_defaults.h>
#include <uicoopa/input/ui_input.h>
#include <uicoopa/input/event_system.h>
#include <uicoopa/groups/content_size_fitter.h>
#include <uicoopa/layout/layout_element.h>
#include <uicoopa/builder/ui_builder.h>
#include <uicoopa/widgets/tooltip.h>
#include <uicoopa/ui_yaml.h>

#include <coopa/asset/asset_manager.h>
#include <coopa/job/engine.h>
#include <coopa/debug/logger.h>
#include <coopa/scene/scene.h>
#include <coopa/scene/scene_object.h>

#include <coopa/maps/map_config.h>
#include <coopa/maps/map_data.h>
#include <coopa/maps/map_generator.h>
#include <coopa/maps/map_renderer.h>
#include <coopa/maps/map_task.h>
#include <coopa/maps/map_yaml.h>

// The viewer's PNG seam. map_export.h and image_writer.h are deliberately NOT included
// here: they compile stb_image_write's body, which gfxcoopa also carries, and the two
// cannot coexist in one TU. See map_viewer_export.h.
#include "map_viewer_export.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

using namespace coopa::ui;
using coopa::scene::Scene;
using coopa::scene::SceneObject;

namespace maps = coopa::maps;

namespace {

/** @brief Where the viewer's asset roots live: uicoopa's, not mapcoopa's. */
constexpr const char* k_ui_assets = UICOOPA_ASSET_DIR;

// ---------------------------------------------------------------------------
// Map <-> GPU
// ---------------------------------------------------------------------------

/**
 * @brief Widens a mapcoopa image to tightly packed RGBA8.
 *
 * MapLayers::render() returns 3-channel RGB for the surface layers and 4-channel RGBA for
 * the overlay ones (Roads/Structures/Landmarks -- see map_layer_has_alpha()), while
 * Texture::upload() only takes tightly packed RGBA8. A 3-channel source becomes opaque.
 *
 * @param img The rendered layer.
 * @return width * height * 4 bytes, ready to upload.
 */
std::vector<std::uint8_t> to_rgba8(const maps::Image& img) {
    const std::size_t px =
        static_cast<std::size_t>(img.width) * static_cast<std::size_t>(img.height);
    std::vector<std::uint8_t> out(px * 4u, 255u);
    if (img.channels == 4) {
        std::copy(img.pixels.begin(), img.pixels.end(), out.begin());
        return out;
    }
    const std::size_t src_stride = static_cast<std::size_t>(img.channels);
    for (std::size_t i = 0; i < px; ++i) {
        const std::size_t s = i * src_stride;
        const std::size_t d = i * 4u;
        if (s + 2 >= img.pixels.size()) break;
        out[d + 0] = img.pixels[s + 0];
        out[d + 1] = img.pixels[s + 1];
        out[d + 2] = img.pixels[s + 2];
    }
    return out;
}

/** @brief The nine MapLayer values, in enum order, for the layer dropdown. */
const std::vector<maps::MapLayer>& layer_order() {
    static const std::vector<maps::MapLayer> k_layers = {
        maps::MapLayer::Composite,  maps::MapLayer::Biomes,     maps::MapLayer::Elevation,
        maps::MapLayer::Water,      maps::MapLayer::Regions,    maps::MapLayer::Roads,
        maps::MapLayer::Structures, maps::MapLayer::Landmarks,  maps::MapLayer::Caves,
    };
    return k_layers;
}

/** @brief Display names for the layer dropdown, taken from map_layer_name(). */
std::vector<std::string> layer_names() {
    std::vector<std::string> names;
    names.reserve(layer_order().size());
    for (maps::MapLayer l : layer_order()) names.emplace_back(maps::map_layer_name(l));
    return names;
}

/**
 * @brief Builds the item list for an enum dropdown from its own `*_name()` function.
 * @tparam Fn Callable taking the enum and returning a std::string_view name.
 * @param count How many values the enum has (its k_*_count constant).
 * @param name_of The library's name function for that enum.
 * @return One string per value, in enum order.
 */
template <typename E, typename Fn>
std::vector<std::string> enum_items(std::size_t count, Fn name_of) {
    std::vector<std::string> items;
    items.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        items.emplace_back(name_of(static_cast<E>(i)));
    }
    return items;
}

}  // namespace

namespace {

/**
 * @struct ViewerState
 * @brief Everything the viewer owns between frames: the map, the GPU copy, and the work in flight.
 */
struct ViewerState {
    /** @brief Live; panel controls write straight into it. */
    maps::MapConfig    config;
    /** @brief Moved out of the generator when it finishes. */
    maps::MapGraph     graph;
    maps::BiomePalette palette;
    maps::MapLayer     layer = maps::MapLayer::Composite;

    /**
     * @brief The frequency `config` was loaded with, before grid-size scaling.
     *
     * apply_derived() scales noise_island.frequency by 40/grid_size the way the CLI does.
     * Scaling the already-scaled value on the next Refresh would compound it, so the
     * pristine value is kept here and the scaling always applied to this.
     */
    double base_island_frequency = 0.0;

    /** @brief On-screen resolution; NOT config.image_size. */
    int  preview_size = 1024;
    bool has_map = false;

    /** @brief Outlives generate_async(); owns the graph until it lands. */
    std::unique_ptr<maps::MapGenerator> generator;
    std::optional<maps::MapTask> gen_task;
    std::optional<maps::MapTask> render_task;
    std::optional<maps::MapTask> export_task;
    /** @brief Written by the render job, read on the UI thread. */
    std::shared_ptr<maps::Image> pending_preview;

    /** @brief Held for MapExporter::export_layers_async(), which keeps its arguments BY REFERENCE.
     */
    std::string export_prefix;

    /**
     * @brief The map's GPU copy. Replaced wholesale on every refresh -- Texture is
     *        upload-once and move-only, so there is no in-place update in either repo.
     *        See upload_preview() for the retirement dance that replacing one requires.
     */
    std::unique_ptr<coopa::gfx::engine::data::Texture> map_texture;

    Sprite map_sprite;

    /**
     * @brief The visible window onto the map, in image space: normalized, Y-DOWN.
     *
     * Y-down because that is the source image's convention (row 0 is the top). The uv
     * actually handed to the sprite is derived from this and flipped on write -- see
     * apply_view_uv() for why the two cannot simply be the same rect.
     *
     * `zoom` is how many times over the map fills the pane: 1 is fit-to-pane, 4 shows a
     * quarter of it per axis.
     */
    float     zoom = 1.0f;
    glm::vec2 view_center{0.5f, 0.5f};

    /** @brief Resolution multiplier the live texture was rendered at. */
    float rendered_scale = 1.0f;
    /** @brief Seconds since zoom last changed; debounces the sharpen re-render. */
    float zoom_settle = 0.0f;
    /** @brief Middle-drag in progress, latched from inside the map rect. */
    bool  panning = false;

    Image* map_widget = nullptr;
    SceneObject* map_view = nullptr;
    Text* status_line = nullptr;
    ProgressBar* progress = nullptr;
    SceneObject* progress_node = nullptr;
    Button* refresh_button = nullptr;
};

/**
 * @brief Applies every value the CLI derives after config load, so both agree on a seed.
 *
 * examples/map_generator.cpp derives the per-pass noise seeds from the master seed, scales
 * the island frequency by the grid size, and re-derives image_size -- all after the config
 * file is read and the flags are applied. A viewer that skipped any of it would quietly
 * produce a different map from the CLI for the same seed, which is the one thing a tool
 * like this must not do.
 *
 * @param state The viewer state whose config is being finalised.
 */
void apply_derived(ViewerState& state) {
    maps::MapConfig& c = state.config;
    c.noise_island.seed      = c.seed;
    c.noise_temperature.seed = c.seed + 1;
    c.noise_blend.seed       = c.seed + 2;
    c.noise_cave.seed        = c.seed + 3;

    // k_reference_grid_size in the CLI: features keep their real-world scale as the cell
    // count changes. Always from the pristine value -- see base_island_frequency.
    const double grid = c.grid_size > 0 ? static_cast<double>(c.grid_size) : 40.0;
    c.noise_island.frequency = state.base_island_frequency * (40.0 / grid);

    c.image_size = maps::derive_image_size(c);
}

/**
 * @brief Re-derives meters_per_pixel from a wanted export resolution, then image_size.
 *
 * map_config.h documents the invariant `image_size * meters_per_pixel == grid_size *
 * meters_per_grid_unit`. image_size is therefore never assignable on its own: writing it
 * behind the scale's back leaves the two disagreeing about how big the world is. This is
 * the CLI's --image-size branch, in one place.
 *
 * @param c      The config to adjust.
 * @param pixels The wanted width/height in pixels; values below 1 are ignored.
 */
void set_image_size(maps::MapConfig& c, int pixels) {
    if (pixels < 1) return;
    const double world_meters = static_cast<double>(c.grid_size) * c.meters_per_grid_unit;
    c.meters_per_pixel = world_meters / static_cast<double>(pixels);
    c.image_size = maps::derive_image_size(c);
}

/**
 * @brief A copy of `config` scaled down to the on-screen preview resolution.
 * @param state Supplies the live config and the wanted preview size.
 * @return A config identical but for its pixel scale. See this file's doc for why.
 */
/**
 * @brief The resolution multiplier the preview should be rendered at for `zoom`.
 *
 * Quantised to 1/2/4 rather than tracking zoom continuously, so drifting the wheel does not
 * queue a full re-render on every notch -- each step is a whole new rasterisation of the map.
 *
 * @param zoom The current zoom factor.
 * @return 1, 2 or 4.
 */
float render_scale_for(float zoom) {
    if (zoom >= 3.5f) return 4.0f;
    if (zoom >= 1.75f) return 2.0f;
    return 1.0f;
}

/**
 * @brief A copy of `config` scaled to the on-screen preview resolution.
 *
 * `scale` sharpens a zoomed view. mapcoopa cannot render a sub-region -- RenderSlice carries
 * a row band only, which is a clip rather than a translation, and the world-to-pixel mapping
 * is a pure scale anchored at the origin -- so the only lever available is to render the
 * WHOLE map bigger and keep showing a window onto it. At 4x that is sixteen times the pixels
 * for the same visible area, which is why the result is capped rather than following zoom.
 *
 * @param state Supplies the live config and the wanted preview size.
 * @param scale Resolution multiplier; see render_scale_for().
 * @return A config identical but for its pixel scale.
 */
maps::MapConfig make_preview_config(const ViewerState& state, float scale = 1.0f) {
    maps::MapConfig preview = state.config;
    constexpr int k_max_preview_px = 4096;
    const int px = std::min(k_max_preview_px,
                            static_cast<int>(static_cast<float>(state.preview_size) * scale));
    set_image_size(preview, px);
    return preview;
}

/**
 * @brief Kicks a preview render of the current layer onto the job engine.
 *
 * MapTask's engine constructor is public precisely so a host can wrap its own map work in
 * the same handle the library's own async calls return -- so the frame loop polls this
 * exactly like it polls generation.
 *
 * @param state The viewer state; its graph must already be populated.
 * @param jobs  The engine to run on.
 */
void start_preview_render(ViewerState& state, coopa::job::JobEngine& jobs, float scale = 1.0f) {
    if (!state.has_map) return;
    state.render_task.reset();  // Cancels and waits for any render still in flight.
    state.rendered_scale = scale;

    state.pending_preview = std::make_shared<maps::Image>();
    auto task_state = std::make_shared<maps::MapTaskState>();
    task_state->total.store(1);

    const maps::MapConfig preview_config = make_preview_config(state, scale);
    const maps::MapLayer layer = state.layer;
    auto out = state.pending_preview;
    const maps::MapGraph* graph = &state.graph;
    const maps::BiomePalette* palette = &state.palette;

    coopa::job::JobHandle handle = jobs.create_handle();
    jobs.submit([out, task_state, preview_config, layer, graph, palette]() {
            *out = maps::MapLayers::render(layer, *graph, preview_config, *palette);
            task_state->step();
            task_state->finish();
        }, maps::k_map_job_type, handle);

    state.render_task.emplace(&jobs, handle, task_state);
}

/**
 * @brief Starts an asynchronous generation from the live config.
 * @param state  The viewer state; its generator and gen_task are replaced.
 * @param jobs   The engine to run on.
 * @param logger Progress sink the generator reports through.
 */
void start_generation(ViewerState& state, coopa::job::JobEngine& jobs,
                      coopa::debug::Logger& logger) {
    apply_derived(state);
    state.gen_task.reset();
    state.render_task.reset();
    // Also the export: MapExporter::export_layers_async() holds state.graph BY REFERENCE
    // for its task's lifetime, and this function is about to move-assign that graph out
    // from under it. Each reset() cancels and waits.
    state.export_task.reset();

    state.generator = std::make_unique<maps::MapGenerator>(state.config, logger);
    state.generator->set_job_engine(&jobs);
    state.gen_task = state.generator->generate_async();

    if (state.refresh_button) state.refresh_button->interactable = false;
    if (state.progress_node) state.progress_node->set_active(true);
    if (state.progress) state.progress->set_value(0.0f, false);
    if (state.status_line) state.status_line->text = "Generating...";
}

/**
 * @brief Clamps the view window into the image and writes it to the sprite's uv.
 *
 * Two things make this less trivial than it looks.
 *
 * The uv uicoopa wants is V-FLIPPED, and the flip is expressed as an *inverted* Rect --
 * `min.y > max.y`. That is a valid thing to hand DrawList::add_quad(), which copies uv
 * corners verbatim, but it is not a valid `Rect` for any of rect.h's helpers (its size().y
 * is negative). So the view is kept as an honest Y-down window here and only flipped at the
 * moment it is written.
 *
 * And the window must stay inside [0,1]: the sampler is ClampToEdge, so a uv that runs off
 * the texture smears the map's edge pixels outward into a streak rather than showing
 * nothing. Clamping the centre keeps the window flush with the edge instead.
 *
 * @param state The viewer state; its view_center is clamped in place.
 */
void apply_view_uv(ViewerState& state) {
    const float span = 1.0f / std::max(1.0f, state.zoom);
    const float half = span * 0.5f;
    // At zoom 1 the window is the whole image and the clamp collapses to 0.5 exactly.
    state.view_center = glm::clamp(state.view_center, glm::vec2(half), glm::vec2(1.0f - half));

    const glm::vec2 lo = state.view_center - half;
    const glm::vec2 hi = state.view_center + half;
    state.map_sprite.uv = Rect{ glm::vec2(lo.x, 1.0f - lo.y), glm::vec2(hi.x, 1.0f - hi.y) };
}

/**
 * @brief Uploads a rendered layer and points the map widget's sprite at it.
 *
 * Retiring the outgoing texture is the fiddly part, and getting it wrong is what made
 * layer switching appear broken: UiPass caches a descriptor set per texture keyed by the
 * raw VkImageView handle, and a driver hands a freed handle straight back to the next
 * texture created. Destroy the old one without telling the pass and the replacement
 * compares equal to it, skips registration, and gets drawn through the dead texture's
 * descriptor -- so the map keeps showing the previous layer. Hence unregister-then-destroy,
 * behind a device wait so no in-flight frame still references it.
 *
 * wait_idle() rather than frame-fence bookkeeping because a swap only happens on a human
 * click or a finished render, and the whole view is about to change anyway -- the stall is
 * invisible and the alternative is materially more code.
 *
 * @param state   The viewer state; takes ownership of the new texture.
 * @param ctx     Supplies the device, allocator and command pool to upload through.
 * @param ui_pass The pass whose caches must forget the outgoing texture.
 * @param img     The rendered layer; a zero-sized image is ignored.
 */
void upload_preview(ViewerState& state, coopa::gfx::app::Context& ctx, UiPass& ui_pass,
                    const maps::Image& img) {
    if (img.width <= 0 || img.height <= 0) return;
    const std::vector<std::uint8_t> rgba = to_rgba8(img);

    coopa::gfx::SamplerDesc sampler;
    // ClampToEdge so the map's own edge pixels never wrap around; linear because the
    // preview is scaled to whatever the pane happens to be, not shown texel-for-texel.
    sampler.address = coopa::gfx::AddressMode::ClampToEdge;

    if (state.map_texture) {
        ctx.wait_idle();
        ui_pass.unregister_texture(state.map_texture->view_typed());
        state.map_texture.reset();
    }

    state.map_texture = std::make_unique<coopa::gfx::engine::data::Texture>(
        coopa::gfx::engine::data::Texture::upload(
            ctx.device(), ctx.allocator(), ctx.command_pool(), rgba.data(),
            static_cast<std::uint32_t>(img.width), static_cast<std::uint32_t>(img.height),
            // RGBA8_Unorm, matching what texture_factory.h uses for every other UI texture --
            // a map that colour-shifted against the exported PNG would be a bug, not a look.
            coopa::gfx::Format::RGBA8_Unorm, sampler));

    state.map_sprite.texture = state.map_texture.get();
    // Re-derived rather than reset to the full-texture rect: this runs on every layer switch
    // and every Refresh, so writing a constant uv here would silently throw away the user's
    // zoom each time.
    apply_view_uv(state);
    if (state.map_widget) state.map_widget->sprite = &state.map_sprite;
}

/**
 * @struct SettingHelp
 * @brief One row's hover text, keyed by the label the row was built with.
 */
struct SettingHelp {
    const char* label; /**< @brief Matches the first argument of the add_*_row() call. */
    const char* text;  /**< @brief What the tooltip says. */
};

/**
 * @brief Attaches every entry of `help` to the matching row inside `panel`.
 *
 * Keyed by label rather than wrapping each add_*_row() call, for two reasons. The row calls
 * stay exactly as wide as they were -- wrapping them pushed most past the 100-column limit
 * the rest of this repo keeps to. And all the help text ends up in one block per section,
 * which is the form that can actually be read against map_config.h to check it is still
 * accurate.
 *
 * The lookup is the row node detail::begin_row_() builds, `"<label>_Row"`. A label that
 * matches nothing is a silent no-op in a release build, so it complains instead -- a typo
 * here would otherwise just quietly drop one tooltip.
 *
 * @param panel The builder the rows were added to.
 * @param help The rows to annotate.
 */
template <std::size_t N>
void apply_help(const UIBuilder& panel, const SettingHelp (&help)[N]) {
    for (const SettingHelp& entry : help) {
        coopa::scene::SceneObject* row =
            panel.node() ? panel.node()->find_descendant(std::string(entry.label) + "_Row")
                         : nullptr;
        if (!row) {
            std::cerr << "[map_viewer] no settings row labelled '" << entry.label
                      << "' to attach help to.\n";
            continue;
        }
        set_tooltip(row, entry.text);
    }
}

/** @brief Radians per degree, for the shape rotation control. */
inline constexpr double k_deg_to_rad = 3.14159265358979323846 / 180.0;

/**
 * @brief Converts radians to degrees in `[0, 360)`.
 *
 * Wrapped rather than clamped because a rotation is periodic -- showing 2578 degrees as 360
 * would be wrong twice over, and clamping would write that wrong value back the first time the
 * slider was touched.
 *
 * @param radians The stored angle.
 * @return The equivalent angle in degrees, in `[0, 360)`.
 */
inline double wrap_degrees(double radians) {
    double deg = std::fmod(radians / k_deg_to_rad, 360.0);
    return deg < 0.0 ? deg + 360.0 : deg;
}

/**
 * @struct ModeRow
 * @brief One row's visibility, as a bitmask over the values of the enum that gates it.
 */
struct ModeRow {
    const char* label; /**< @brief Matches the first argument of the add_*_row() call. */
    unsigned    modes; /**< @brief Bit i set means "shown when the dropdown is on value i". */
};

/**
 * @brief Shows exactly those rows whose mask includes `mode`, and hides the rest.
 *
 * Several settings only mean anything under one value of the dropdown above them -- the blur
 * knobs do nothing unless the surface is Blended, and a rectangle has no use for a landmass
 * count. Leaving them on screen invites you to turn a knob that cannot change the map.
 *
 * Keyed by label for the same reasons apply_help() is: the add_*_row() calls stay untouched,
 * and each section's rule reads as one table next to its help text.
 *
 * The caller must re-run CollapsibleHandle::fit() when this returns true. Hiding a row takes it
 * out of detail::fit_content_height()'s measure, but the section's own height is a number
 * written once by fit() -- without a re-fit the section keeps its old height and leaves a gap
 * where the hidden rows were.
 *
 * @param panel The builder the rows were added to.
 * @param rows The visibility table.
 * @param mode The dropdown's current index.
 * @return Whether any row's visibility actually changed, so a caller can skip a needless re-fit.
 */
template <std::size_t N>
bool apply_mode_rows(const UIBuilder& panel, const ModeRow (&rows)[N], int mode) {
    bool changed = false;
    for (const ModeRow& entry : rows) {
        coopa::scene::SceneObject* row =
            panel.node() ? panel.node()->find_descendant(std::string(entry.label) + "_Row")
                         : nullptr;
        if (!row) {
            std::cerr << "[map_viewer] no settings row labelled '" << entry.label
                      << "' to show or hide.\n";
            continue;
        }
        const bool want = mode >= 0 && (entry.modes & (1u << static_cast<unsigned>(mode))) != 0u;
        if (row->active() == want) continue;
        row->set_active(want);
        changed = true;
    }
    return changed;
}

/** @brief A one-line summary of the current map, for the status readout. */
std::string describe(const ViewerState& state) {
    if (!state.has_map) return "No map yet -- press Refresh.";
    return std::to_string(state.config.grid_size) + "x" + std::to_string(state.config.grid_size) +
           " cells  |  " + std::to_string(state.graph.towns.size()) + " towns  |  " +
           std::to_string(state.graph.rivers.size()) + " rivers  |  " +
           std::to_string(state.graph.caves.size()) + " caves  |  seed " +
           std::to_string(state.config.seed) + "  |  export " +
           std::to_string(state.config.image_size) + "px";
}

}  // namespace

namespace {

/**
 * @brief Re-renders draw_list into a throwaway offscreen image and writes it to a PNG.
 *
 * Copied from uicoopa's demos so this viewer honours SCREENSHOT_NAME the same way they do:
 * a scripted `MAX_FRAMES=n SCREENSHOT_NAME=x ./build/mapcoopa_viewer` produces a checkable
 * image with nobody at the window.
 *
 * Reuses ui_pass's existing pipeline by giving the capture image the swapchain's format and
 * wrapping it in a fresh framebuffer against that same render pass. The create/destroy pair
 * is the one place this file touches raw Vulkan -- gfxcoopa has no Framebuffer wrapper, and
 * this needs one bound to an existing render pass rather than a gfxcoopa-owned target.
 *
 * Call only when no render pass is open and the device is idle; frame-slot 0's geometry
 * buffers are reused here.
 *
 * @param ctx          The live context, for its device/allocator/render pass.
 * @param ui_pass      The pass whose pipeline and buffers are reused.
 * @param draw_list    The geometry to re-emit.
 * @param width        Capture width in pixels.
 * @param height       Capture height in pixels.
 * @param scale_factor The canvas scale the draw list was built at.
 * @param out_path     Where to write the PNG.
 */
void save_screenshot(coopa::gfx::app::Context& ctx, UiPass& ui_pass, const DrawList& draw_list,
                     uint32_t width, uint32_t height, float scale_factor, const std::string& out_path) {
    using namespace coopa::gfx;

    if (width == 0 || height == 0) {
        std::cerr << "[map_viewer] save_screenshot: zero-sized framebuffer, skipping.\n";
        return;
    }

    ui_pass.register_textures(draw_list);
    ui_pass.begin_frame(/*frame_index=*/0, draw_list.vertices().size(),
                        draw_list.indices().size());

    memory::Image capture_image(ctx.device(), ctx.allocator(), width, height, ctx.color_format(),
                                ImageUsage::ColorAttachment | ImageUsage::TransferSrc);

    VkImageView attachment = capture_image.view(); // gfx-allow-vulkan
    VkFramebufferCreateInfo fb_info{}; // gfx-allow-vulkan
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO; // gfx-allow-vulkan
    fb_info.renderPass = ctx.render_pass().handle();
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &attachment;
    fb_info.width = width;
    fb_info.height = height;
    fb_info.layers = 1;

    VkFramebuffer framebuffer = VK_NULL_HANDLE; // gfx-allow-vulkan
    if (vkCreateFramebuffer(ctx.device().handle(), &fb_info, nullptr, &framebuffer) != VK_SUCCESS) { // gfx-allow-vulkan
        std::cerr << "[map_viewer] save_screenshot: vkCreateFramebuffer failed.\n"; // gfx-allow-vulkan
        return;
    }

    ctx.command_pool().submit_once([&](command::CommandBuffer& cmd) {
        cmd.begin_render_pass(ctx.render_pass().handle(), framebuffer, { width, height },
                              VkClearColorValue{{ 0.05f, 0.05f, 0.07f, 1.0f }}); // gfx-allow-vulkan
        ui_pass.draw(cmd, /*frame_index=*/0, width, height, scale_factor, draw_list);
        cmd.end_render_pass();
        capture_image.mark_transitioned(TextureUsage::Present);
    });

    vkDestroyFramebuffer(ctx.device().handle(), framebuffer, nullptr); // gfx-allow-vulkan

    util::ImageData data = util::read_image(ctx.device(), ctx.allocator(), ctx.command_pool(), capture_image);
    for (size_t i = 3; i < data.pixels.size(); i += 4) data.pixels[i] = 255;  // force opaque -- see test_window.cpp's doc.

    util::save_image_png(data, out_path);
    std::cout << "[map_viewer] Saved screenshot to " << out_path << "\n";
}

}  // namespace

int main() {
    std::cout << "==========================================================\n";
    std::cout << "  mapcoopa viewer\n";
    std::cout << "  Edit the settings on the left, press Refresh, pick a\n";
    std::cout << "  layer. File > Import/Export for .yaml and .png.\n";
    std::cout << "  Press ESC to quit.\n";
    std::cout << "==========================================================\n\n";

    coopa::gfx::app::ContextConfig config = coopa::gfx::app::ContextConfig::from_env(
        coopa::gfx::app::ContextConfig{
            .title = "mapcoopa viewer", .width = 1600, .height = 900, .resizable = true,
        });
#ifdef NDEBUG
    config.validation = false;
#endif
    coopa::gfx::app::Context ctx(config);

    const std::string shader_dir = std::string(k_ui_assets) + "/shaders";
    UiPass ui_pass(ctx.device(), ctx.allocator(), ctx.command_pool(), ctx.render_pass(),
                   shader_dir + "/ui.vert.spv", shader_dir + "/ui_quad.frag.spv",
                   shader_dir + "/ui_text.frag.spv");

    // One engine for everything: the asset decoder, map generation, preview rendering and
    // PNG export all queue onto it rather than each spinning up its own pool.
    coopa::job::JobEngine jobs;
    coopa::asset::AssetManager assets(&jobs);
    assets.add_search_root(k_ui_assets);
    IconLibrary::instance().configure(assets, ctx.device(), ctx.allocator(), ctx.command_pool());
    IconLibrary::instance().add_sheet(assets, "icons/icons.yaml");
    IconLibrary::instance().add_sheet(assets, "icons/cursors.yaml");

    Font ui_font(ctx.device(), ctx.allocator(), ctx.command_pool(),
                 std::string(k_ui_assets) + "/fonts/DejaVuSans.ttf");
    FontDefaults::font = &ui_font;
    FontDefaults::note_text_size = [](Font* f, std::uint32_t sz) {
        UIResourceCache::instance().note_text_atlas_use(f, sz);
    };
    UIResourceCache::instance().configure(ctx.device(), ctx.allocator(), ctx.command_pool());
    FontDefaults::resolve_font = [](const std::string& path) {
        return UIResourceCache::instance().font_for_path(path);
    };

    ThemeLibrary::instance().set_search_dir(std::string(k_ui_assets) + "/themes");
    const char* theme_env = std::getenv("THEME");
    const UITheme& theme = ThemeLibrary::instance().load(theme_env && theme_env[0] ? theme_env : "dark");
    ThemeLibrary::instance().set_active(theme);

    coopa::debug::Logger logger("mapview");

    // --- Start from exactly the config the CLI starts from. ---
    //
    // examples/map_generator.cpp layers its settings MapConfig defaults -> tool defaults ->
    // assets/config.yaml, and only then applies flags. The viewer has no flags, but it must
    // begin at the same place or the two tools would disagree about what "seed 251" looks
    // like -- which is the one thing a front-end for the same generator must not do. These
    // five are that file's tool defaults verbatim; see its comment for why no image_size.
    ViewerState state;
    state.config.grid_size = 80;
    state.config.towns.town_count = 28;
    state.config.river_count = 55;
    state.config.landmarks.max_natural = 70;
    state.config.landmarks.max_abandoned = 45;

    const std::string config_path = std::string(ROOT_DIR) + "/assets/config.yaml";
    const maps::ConfigLoadResult loaded = maps::load_config(config_path, state.config);
    if (loaded.status == maps::ConfigLoad::Malformed) {
        std::cerr << "[map_viewer] " << loaded.message << "\n";
        return 1;
    }
    if (loaded.status == maps::ConfigLoad::NotFound) {
        std::cout << "[map_viewer] " << loaded.message << " -- using built-in defaults.\n";
    }

    // Captured after the config file has had its say, so the grid-size scaling in
    // apply_derived() always starts from the authored value. See base_island_frequency.
    state.base_island_frequency = state.config.noise_island.frequency;

    // The CLI reseeds from std::random_device when no --seed was given; SEED= is the
    // viewer's equivalent, and config.yaml's own seed wins when neither is set.
    if (const char* seed_env = std::getenv("SEED")) {
        state.config.seed = std::atoi(seed_env);
    } else if (!loaded.has_seed) {
        std::random_device rd;
        state.config.seed = static_cast<int>(rd() & 0x7fffffffu);
    }
    apply_derived(state);

    // ======================================================================
    // UI
    // ======================================================================
    Scene scene("map_viewer");
    auto canvas_obj = std::make_unique<SceneObject>("Canvas");
    auto* canvas = canvas_obj->add_component<CanvasComponent>();
    canvas->scaler.mode = ScaleMode::ScaleWithScreenSize;
    canvas->scaler.reference_resolution = {1600.0f, 900.0f};
    canvas->scaler.match_width_or_height = 0.5f;

    UIBuilder root(canvas_obj.get(), &theme);
    root.panel("Background")->get_component<Image>()->color = theme.panel.background;

    SectionSet rows = root.split_rows({ Section{"MenuStrip", 0.0f, theme.menu.bar_height,
                                      SectionFlow::None},
                                        Section{"Content", 1.0f, 0.0f, SectionFlow::None} }, 0.0f);
    SectionSet cols = rows["Content"].split_columns({ Section{"Sidebar", 0.0f, 340.0f,
                           SectionFlow::None},
                                                      Section{"MapView", 1.0f, 0.0f,
                                                              SectionFlow::None} }, 0.0f);
    UIBuilder side_col = cols["Sidebar"];
    UIBuilder map_col = cols["MapView"];

    // --- The map view: a square, centred Image plus a layer picker and a status line. ---
    state.map_view = map_col.node();
    {
        Image* img = map_col.add_image(nullptr, {512.0f, 512.0f}, glm::vec4(1.0f),
                                       ImageType::Simple, "MapImage");
        auto* img_rt = img->owner->get_component<RectTransform>();
        img_rt->anchor_preset(AnchorPreset::MiddleCenter);
        img_rt->hittable = false;   // Nothing to click on the map itself; let the menu scrim win.
        state.map_widget = img;

        Text* status = map_col.add_status_line("No map yet -- press Refresh.", TextRole::Secondary,
                                               "StatusLine");
        auto* st_rt = status->owner->get_component<RectTransform>();
        st_rt->anchor_preset(AnchorPreset::StretchBottom);
        st_rt->set_size_delta({0.0f, 22.0f});
        st_rt->set_anchored_position({0.0f, 10.0f});
        st_rt->hittable = false;
        state.status_line = status;
    }

    // --- The sidebar: one horizontally-collapsing panel wrapping the settings list. ---
    detail::CollapsibleOptions side_opts;
    side_opts.axis = CollapseAxis::Horizontal;
    side_opts.expanded_extent = 340.0f;
    side_opts.collapsed_extent = theme.collapsible.rail_width;
    // The split Section is the node the parent sizes, not the panel inside it.
    side_opts.size_node = side_col.node();
    side_opts.start_expanded = true;
    CollapsibleHandle sidebar = side_col.collapsible("Settings", "Map Settings", side_opts);

    UIBuilder list = sidebar.body();
    list.with_padding(10.0f, 10.0f, 8.0f, 8.0f);
    // The body must absorb the sidebar's leftover height: the collapsible root's layout
    // group has child_force_expand_height = false, so without this it sits at its own
    // preferred height and the scroll view below never gets a viewport to scroll in.
    list.node()->add_component<LayoutElement>()->flexible_size = {-1.0f, 1.0f};

    // Only the settings sections scroll. The separator, progress bar and Refresh button are
    // built into `list` AFTER this, so they stay pinned at the bottom of the sidebar and
    // remain reachable however many sections are expanded.
    //
    // with_header = false: the CollapsiblePanel above already provides the "Map Settings"
    // title strip, and scroll_view's own bar would stack a second one under it.
    //
    // The width MUST be the real pixel width -- make_scroll_view bakes size.x into the
    // Content width and the scrollbar's offsets, so the frame cannot stretch. 340px section
    // minus the 10+10 padding just applied = 320.
    UIBuilder settings = list.scroll_view("SidebarScroll", "", {320.0f, 600.0f},
                                          AnchorPreset::TopLeft, {0.0f, 0.0f},
                                          /*with_header=*/false);
    // scroll_view() returns the Content node; the frame is its grandparent.
    SceneObject* scroll_frame = settings.node()->parent()->parent();
    scroll_frame->add_component<LayoutElement>()->flexible_size = {-1.0f, 1.0f};

    // ContentSizeFitter rather than a one-shot fit_content_height(): ScrollRect derives its
    // range from Content's size_delta, and folding a section open or shut rewrites that
    // section's LayoutElement::preferred_size.y. A snapshot taken at build time would leave
    // the range permanently stale -- scrolling past the end, or not at all. The fitter
    // re-measures every frame off the sibling VerticalLayoutGroup, and Content's parent
    // (Viewport) carries no layout group, so nothing overwrites what it writes.
    // Vertical only: horizontal would clobber the Content width computed above.
    auto* fitter = settings.node()->add_component<ContentSizeFitter>();
    fitter->vertical_fit = FitMode::PreferredSize;

    // Every control writes into state.config immediately but regenerates nothing; only
    // Refresh does. That keeps a ~1s generate off every slider drag.
    maps::MapConfig& cfg = state.config;

    CollapsibleHandle world = settings.collapsible("SecWorld", "World");
    {
        UIBuilder b = world.body();
        b.add_spinbox_row("Seed", 0.0, 2147483647.0, static_cast<double>(cfg.seed),
                          1.0,
                          [&cfg](double v) { cfg.seed = static_cast<int>(v); });
        b.add_slider_row("Grid size", 20.0f, 160.0f, static_cast<float>(cfg.grid_size),
                         1.0f,
                         [&cfg](float v) { cfg.grid_size = static_cast<int>(v); });
        b.add_spinbox_row("Export px", 512.0, 8192.0,
                          static_cast<double>(cfg.image_size), 256.0,
                          [&cfg](double v) { set_image_size(cfg,
                           static_cast<int>(v)); });
        b.add_dropdown_row("Preview px", {"512", "1024", "1536", "2048"}, 1,
                           [&state](int i, const std::string& s) {
                               (void)i;
                               state.preview_size = std::stoi(s);
                           });
        // Which dimension each shape actually reads, from ShapeField's constructor and
        // build_landmasses_(). Rotation is the one that does not match its own doc comment:
        // that says "Triangle only", but blob_inset_() subtracts it from the polar angle, so it
        // turns Continent and Archipelago outlines too -- only Rectangle and Circle ignore it.
        // continent_count is Archipelago-only because build_landmasses_() forces it to 1 for a
        // single Continent, and size_variance only does anything once there is more than one.
        constexpr unsigned k_rect = 1u << static_cast<unsigned>(maps::MapShape::Rectangle);
        constexpr unsigned k_circ = 1u << static_cast<unsigned>(maps::MapShape::Circle);
        constexpr unsigned k_tri  = 1u << static_cast<unsigned>(maps::MapShape::Triangle);
        constexpr unsigned k_cont = 1u << static_cast<unsigned>(maps::MapShape::Continent);
        constexpr unsigned k_arch = 1u << static_cast<unsigned>(maps::MapShape::Archipelago);
        static const ModeRow k_shape_rows[] = {
            {"Width (m)",     k_rect},
            {"Height (m)",    k_rect},
            {"Diameter (m)",  k_circ},
            {"Edge (m)",      k_tri},
            {"Rotation",      k_tri | k_cont | k_arch},
            {"Size (m)",      k_cont | k_arch},
            {"Landmasses",    k_arch},
            {"Irregularity",  k_cont | k_arch},
            {"Size variance", k_arch},
            {"Coast detail",  k_cont | k_arch},
        };

        b.add_dropdown_row("Shape",
                           enum_items<maps::MapShape>(maps::k_map_shape_count,
                                                      maps::map_shape_name),
                           static_cast<int>(cfg.shape.shape),
                           [&cfg, b, world](int i, const std::string&) {
                               cfg.shape.shape = static_cast<maps::MapShape>(i);
                               if (apply_mode_rows(b, k_shape_rows, i)) world.fit();
                           });

        // Each row below is shown only for the shapes that read it -- see k_shape_rows. A zero in
        // any of them means "fill the canvas", which is what ShapeConfig defaults them all to.
        b.add_spinbox_row("Width (m)", 0.0, 100000.0, cfg.shape.width_m, 100.0,
                          [&cfg](double v) { cfg.shape.width_m = v; });
        b.add_spinbox_row("Height (m)", 0.0, 100000.0, cfg.shape.height_m, 100.0,
                          [&cfg](double v) { cfg.shape.height_m = v; });
        b.add_spinbox_row("Diameter (m)", 0.0, 100000.0, cfg.shape.diameter_m, 100.0,
                          [&cfg](double v) { cfg.shape.diameter_m = v; });
        b.add_spinbox_row("Edge (m)", 0.0, 100000.0, cfg.shape.edge_length_m, 100.0,
                          [&cfg](double v) { cfg.shape.edge_length_m = v; });
        b.add_spinbox_row("Size (m)", 0.0, 100000.0, cfg.shape.continent_size_m, 100.0,
                          [&cfg](double v) { cfg.shape.continent_size_m = v; });
        // Degrees here, radians in the config -- the same conversion the CLI's --shape-rot does.
        // Wrapped into [0, 360) on the way in: rotation is periodic, and assets/config.yaml
        // ships `rotation: 45.0` under a comment calling it radians, which is ~2578 degrees and
        // would otherwise pin this slider at its maximum.
        b.add_slider_row("Rotation", 0.0f, 360.0f,
                         static_cast<float>(wrap_degrees(cfg.shape.rotation)), 1.0f,
                         [&cfg](float v) { cfg.shape.rotation = v * k_deg_to_rad; });
        b.add_spinbox_row("Landmasses", 1.0, 24.0,
                          static_cast<double>(cfg.shape.continent_count), 1.0,
                          [&cfg](double v) { cfg.shape.continent_count = static_cast<int>(v); });
        b.add_slider_row("Irregularity", 0.0f, 0.6f,
                         static_cast<float>(cfg.shape.irregularity), 0.0f,
                         [&cfg](float v) { cfg.shape.irregularity = v; });
        b.add_slider_row("Size variance", 0.0f, 0.9f,
                         static_cast<float>(cfg.shape.size_variance), 0.0f,
                         [&cfg](float v) { cfg.shape.size_variance = v; });
        b.add_slider_row("Coast detail", 0.0f, 0.5f,
                         static_cast<float>(cfg.shape.coast_detail), 0.0f,
                         [&cfg](float v) { cfg.shape.coast_detail = v; });
        static const SettingHelp k_help[] = {
            {"Seed",
             "Master seed; every pass derives its generator from this. The same seed "
             "reproduces a map exactly."},
            {"Grid size",
             "Cells per axis; the point set is (grid_size + 1)^2 plus a boundary ring. Raising "
             "it adds detail and generation time."},
            {"Export px",
             "Side length in pixels of the square PNG renders. Affects exports only, not what "
             "is on screen."},
            {"Preview px",
             "Resolution the on-screen preview is rendered at. Independent of the export size; "
             "zooming in raises it automatically."},
            {"Shape",
             "Which outline to use: a rectangle or circle filling the canvas, a triangle, a "
             "single continent, or an archipelago. The rows below follow this choice."},
            {"Landmasses",
             "How many landmasses Archipelago attempts, at least 1. Ignored by the other "
             "shapes."},
            {"Irregularity",
             "How far the outline of a landmass wanders from a circle, 0 to 0.6. Clamped, so a "
             "landmass can never pinch itself in two; zero gives a plain disc."},
            {"Width (m)",
             "Rectangle width in metres. Zero spans the canvas, which is what every shape "
             "dimension defaults to."},
            {"Height (m)",
             "Rectangle height in metres; zero spans the canvas."},
            {"Diameter (m)",
             "Circle diameter in metres; zero spans the canvas."},
            {"Edge (m)",
             "Equilateral triangle side in metres; zero spans the canvas."},
            {"Rotation",
             "Rotation about the centre, in degrees. Turns the triangle, and re-phases the "
             "coastal wobble of a continent or archipelago; a rectangle and a circle ignore it."},
            {"Size (m)",
             "Mean diameter of one landmass in metres; zero sizes it to the canvas. Its own "
             "dimension rather than the circle's, because this is the average of several "
             "landmasses that then vary about it."},
            {"Size variance",
             "Spread of landmass sizes about the mean, 0 to 0.9, so an archipelago comes out as "
             "a couple of large landmasses and a scatter of smaller ones rather than clones."},
            {"Coast detail",
             "Amplitude of the coastal warp, as a fraction of the mean radius. It is what "
             "produces peninsulas, inlets and the occasional offshore island; without it every "
             "ray from the centre crosses the coast exactly once."},
        };
        apply_help(b, k_help);
        apply_mode_rows(b, k_shape_rows, static_cast<int>(cfg.shape.shape));
        world.fit();
    }

    CollapsibleHandle terrain = settings.collapsible("SecTerrain", "Terrain");
    {
        UIBuilder b = terrain.body();
        b.add_slider_row("Sea level", 0.0f, 1.0f, static_cast<float>(cfg.sea_level),
                         0.0f,
                         [&cfg](float v) { cfg.sea_level = v; });
        b.add_slider_row("Relief", 0.0f, 1.5f, static_cast<float>(cfg.terrain_relief),
                         0.0f,
                         [&cfg](float v) { cfg.terrain_relief = v; });
        b.add_slider_row("Roughness", 0.0f, 1.0f,
                         static_cast<float>(cfg.terrain_roughness), 0.0f,
                         [&cfg](float v) { cfg.terrain_roughness = v; });
        b.add_spinbox_row("Height (m)", 50.0, 4000.0, cfg.elevation_range_m, 50.0,
                          [&cfg](double v) { cfg.elevation_range_m = v; });
        b.add_spinbox_row("Cell (m)", 5.0, 500.0, cfg.meters_per_grid_unit, 5.0,
                          [&cfg](double v) { cfg.meters_per_grid_unit = v; });
        b.add_slider_row("Smoothing", 0.0f, 20.0f,
                         static_cast<float>(cfg.elevation_smoothing_iterations), 1.0f,
                         [&cfg](float v) {
                             cfg.elevation_smoothing_iterations = static_cast<int>(v);
                         });
        // Only Blended reads either knob: elevation_blend is read by blend_radius_() alone, and
        // smooth_elevation_raster_() returns immediately under the other two surfaces. Neither
        // Interpolated nor Flat has a setting of its own, so they simply show nothing extra.
        static const ModeRow k_surface_rows[] = {
            {"Blend radius",    1u << static_cast<unsigned>(maps::ElevationSurface::Blended)},
            {"Blend variation", 1u << static_cast<unsigned>(maps::ElevationSurface::Blended)},
        };

        b.add_dropdown_row("Surface",
                           enum_items<maps::ElevationSurface>(maps::k_elevation_surface_count,
                                                             maps::elevation_surface_name),
                           static_cast<int>(cfg.elevation_surface),
                           [&cfg, b, terrain](int i, const std::string&) {
                               cfg.elevation_surface = static_cast<maps::ElevationSurface>(i);
                               if (apply_mode_rows(b, k_surface_rows, i)) terrain.fit();
                           });
        // Shown only under Blended -- see k_surface_rows above.
        b.add_slider_row("Blend radius", 0.0f, 1.0f, static_cast<float>(cfg.elevation_blend),
                         0.0f,
                         [&cfg](float v) { cfg.elevation_blend = v; });
        b.add_slider_row("Blend variation", 0.0f, 1.0f,
                         static_cast<float>(cfg.elevation_blend_variation), 0.0f,
                         [&cfg](float v) { cfg.elevation_blend_variation = v; });
        static const SettingHelp k_help[] = {
            {"Sea level",
             "The waterline, in the normalised [0, 1] height field. Land spans [sea_level, 1], "
             "so everything below it carries real bathymetry."},
            {"Relief",
             "How much fractal relief reshapes the coast-distance height field, 0 to 1."},
            {"Roughness",
             "How strongly the detail field displaces the sampled surface, 0 to 1. It only "
             "textures the drawn ground under the interpolated surface, but still shapes river "
             "channels, water surfaces and cave floors under every one."},
            {"Height (m)",
             "Metres of height the normalised [0, 1] elevation field spans."},
            {"Cell (m)",
             "Side length in metres of one grid unit. With grid size, this sets how big the "
             "world is."},
            {"Smoothing",
             "Laplacian relaxation passes applied to the height field. More passes give "
             "smoother, rounder terrain."},
            {"Surface",
             "How the height field is drawn between the cells it is stored at: interpolated, "
             "flat per cell, or blended. Only blended has settings of its own."},
            {"Blend radius",
             "Radius of the box blur blended runs over the finished raster, in cells -- 0.5 is "
             "half a cell at any render size. At 0 it is flat byte for byte; one cell is the "
             "ceiling, past which the blur erases landforms along with the facets."},
            {"Blend variation",
             "How much the blend radius varies from cell to cell, 0 to 1. Every cell gets its "
             "own radius, so one keeps hard edges while its neighbour is fully smoothed. It "
             "scales the blend radius, so a small blend leaves little to vary."},
        };
        apply_help(b, k_help);
        apply_mode_rows(b, k_surface_rows, static_cast<int>(cfg.elevation_surface));
        terrain.fit();
    }

    CollapsibleHandle water = settings.collapsible("SecWater", "Water", false);
    {
        UIBuilder b = water.body();
        b.add_slider_row("Rivers", 0.0f, 200.0f, static_cast<float>(cfg.river_count),
                         1.0f,
                         [&cfg](float v) { cfg.river_count = static_cast<int>(v); });
        b.add_spinbox_row("Min length", 1.0, 40.0,
                          static_cast<double>(cfg.river_min_length), 1.0,
                          [&cfg](double v) { cfg.river_min_length = static_cast<int>(v); });
        b.add_spinbox_row("Width (m)", 1.0, 60.0, cfg.river_width_base_m, 1.0,
                          [&cfg](double v) { cfg.river_width_base_m = v; });
        b.add_spinbox_row("Incision (m)", 0.0, 400.0, cfg.river_incision_m, 5.0,
                          [&cfg](double v) { cfg.river_incision_m = v; });
        b.add_slider_row("Lake thresh", 0.0f, 1.0f,
                         static_cast<float>(cfg.threshold_water), 0.0f,
                         [&cfg](float v) { cfg.threshold_water = v; });
        static const SettingHelp k_help[] = {
            {"Rivers",
             "Number of river sources attempted. Fewer appear if the terrain offers nowhere "
             "for them to run."},
            {"Min length",
             "Corners a watercourse must run through before it counts as a river."},
            {"Width (m)",
             "Width of a volume-zero stream; larger rivers widen from here as they gather "
             "volume."},
            {"Incision (m)",
             "How deep a river cuts the valley it runs in, in metres."},
            {"Lake thresh",
             "Noise value above which a corner is water. Higher values leave less inland "
             "water."},
        };
        apply_help(b, k_help);
        water.fit();
    }

    CollapsibleHandle features = settings.collapsible("SecFeatures", "Features", false);
    {
        UIBuilder b = features.body();
        b.add_slider_row("Towns", 0.0f, 120.0f,
                         static_cast<float>(cfg.towns.town_count), 1.0f,
                         [&cfg](float v) { cfg.towns.town_count = static_cast<int>(v); });
        b.add_spinbox_row("Buildings", 0.0, 400.0,
                          static_cast<double>(cfg.towns.buildings_per_town), 10.0,
                          [&cfg](double v) { cfg.towns.buildings_per_town = static_cast<int>(v); });
        b.add_spinbox_row("Road hubs", 0.0, 200.0,
                          static_cast<double>(cfg.roads.hub_count), 1.0,
                          [&cfg](double v) { cfg.roads.hub_count = static_cast<int>(v); });
        b.add_spinbox_row("Countries", 0.0, 40.0,
                          static_cast<double>(cfg.regions.country_count), 1.0,
                          [&cfg](double v) { cfg.regions.country_count = static_cast<int>(v); });
        b.add_spinbox_row("Regions each", 1.0, 20.0,
                          static_cast<double>(cfg.regions.regions_per_country), 1.0,
                          [&cfg](double v) {
                              cfg.regions.regions_per_country = static_cast<int>(v);
                          });
        b.add_spinbox_row("Landmarks", 0.0, 300.0,
                          static_cast<double>(cfg.landmarks.max_natural), 5.0,
                          [&cfg](double v) { cfg.landmarks.max_natural = static_cast<int>(v); });
        b.add_spinbox_row("Caves", 0.0, 200.0,
                          static_cast<double>(cfg.caves.cave_count), 1.0,
                          [&cfg](double v) { cfg.caves.cave_count = static_cast<int>(v); });
        b.add_spinbox_row("Cave depth", 20.0, 1200.0, cfg.caves.max_depth_m, 20.0,
                          [&cfg](double v) { cfg.caves.max_depth_m = v; });
        b.add_separator();
        b.add_toggle_row("Rivers",    cfg.enable_rivers,
                         [&cfg](bool v) { cfg.enable_rivers = v; });
        b.add_toggle_row("Roads",     cfg.enable_roads,
                         [&cfg](bool v) { cfg.enable_roads = v; });
        b.add_toggle_row("Regions",   cfg.enable_regions,
                         [&cfg](bool v) { cfg.enable_regions = v; });
        b.add_toggle_row("Towns",     cfg.enable_towns,
                         [&cfg](bool v) { cfg.enable_towns = v; });
        b.add_toggle_row("Landmarks", cfg.enable_landmarks,
                         [&cfg](bool v) { cfg.enable_landmarks = v; });
        b.add_toggle_row("Caves",     cfg.enable_caves,
                         [&cfg](bool v) { cfg.enable_caves = v; });
        static const SettingHelp k_help[] = {
            {"Towns",
             "Upper bound on settlements placed; fewer appear if the map lacks room."},
            {"Buildings",
             "Buildings attempted in a capital across all its cells; lesser tiers get a "
             "fraction."},
            {"Road hubs",
             "Anchors the road network is routed between; clamped to the land available."},
            {"Countries",
             "Nations to place, terrain permitting."},
            {"Regions each",
             "Provinces carved out of each country."},
            {"Landmarks",
             "Cap on terrain features recorded -- peaks, waterfalls, canyons and the like."},
            {"Caves",
             "Cave systems to open; clamped to the qualifying slopes the map actually has."},
            {"Cave depth",
             "Hard cap on how far below its own mouth a system may reach, in metres."},
        };
        apply_help(b, k_help);
        features.fit();
    }

    CollapsibleHandle render = settings.collapsible("SecRender", "Render", false);
    {
        UIBuilder b = render.body();
        b.add_dropdown_row("Shading",
                           enum_items<maps::CompositeShading>(maps::k_composite_shading_count,
                                                             maps::composite_shading_name),
                           static_cast<int>(cfg.composite_shading),
                           [&cfg](int i, const std::string&) {
                               cfg.composite_shading = static_cast<maps::CompositeShading>(i);
                           });
        b.add_toggle_row("Show regions", cfg.show_regions,
                         [&cfg](bool v) { cfg.show_regions = v; });
        b.add_slider_row("Region tint", 0.0f, 1.0f, cfg.region_tint, 0.0f,
                         [&cfg](float v) { cfg.region_tint = v; });
        b.add_slider_row("PNG level", 1.0f, 9.0f,
                         static_cast<float>(cfg.png_compression_level), 1.0f,
                         [&cfg](float v) { cfg.png_compression_level = static_cast<int>(v); });
        static const SettingHelp k_help[] = {
            {"Shading",
             "How the composite layer lights the biome colours: by elevation, or with a "
             "hillshade."},
            {"Show regions",
             "Tint cells by the region that claims them, so borders are visible."},
            {"Region tint",
             "How strongly the region tint is mixed over the biome colour, in [0, 1]."},
            {"PNG level",
             "Deflate effort used when encoding a PNG; higher is smaller and slower."},
        };
        apply_help(b, k_help);
        render.fit();
    }

    // Pinned below the scroll view, not inside it -- see the scroll_view() call above.
    list.add_separator();
    state.progress = list.add_progress_bar("GenProgress", 0.0f, 1.0f, 0.0f,
                                           ProgressBarRole::Neutral);
    state.progress_node = state.progress->owner;
    state.refresh_button = list.add_button("Refresh", ButtonRole::Primary, [&]() {
        start_generation(state, jobs, logger);
    });
    // No sidebar.fit() here: CollapsibleHandle::fit() returns immediately on the Horizontal
    // axis, whose extents are authored rather than measured. The per-section fit() calls
    // above are the ones that matter -- they feed the ContentSizeFitter.

    // --- Layer picker, floating over the top-left of the map view. ---
    ComboBox* layer_select = nullptr;
    {
        UIBuilder picker = map_col.horizontal_layout("LayerPicker", 6.0f);
        auto* prt = picker.node()->get_component<RectTransform>();
        prt->anchor_preset(AnchorPreset::TopLeft);
        prt->set_size_delta({260.0f, theme.metrics.row_height});
        prt->set_anchored_position({12.0f, -12.0f});
        // add_label() anchors StretchAll and reports no preferred width, so inside a
        // HorizontalLayoutGroup it collapses and the dropdown lands on top of it. Give it
        // an explicit width, the way the labelled *_row() helpers do for their own columns.
        Text* layer_label = picker.add_label("Layer", 0.0f, theme.text.secondary, "LayerLabel");
        layer_label->owner->add_component<LayoutElement>()->preferred_size =
            {46.0f, theme.metrics.row_height};
        // Switching layers re-renders from the existing graph -- no regeneration.
        layer_select = picker.add_dropdown("LayerSelect", layer_names(), 0,
                            [&state, &jobs](int i, const std::string&) {
                                const auto& order = layer_order();
                                if (i < 0 || i >= static_cast<int>(order.size())) return;
                                state.layer = order[static_cast<std::size_t>(i)];
                                start_preview_render(state, jobs, render_scale_for(state.zoom));
                            });
    }

    // --- One file dialog, retargeted per menu action. ---
    //
    // Cheaper than five dialogs, and the reason FileDialogHandle has retarget()/
    // disconnect_all() at all: each entry below sets the mode, the extension filter and the
    // callback just before opening, so only one browser subtree ever exists.
    FileDialogHandle picker_dlg = root.file_dialog("FilePicker", "Open", FileDialogMode::Open,
                                                   {".yaml"});

    auto set_status = [&state](const std::string& msg) {
        if (state.status_line) state.status_line->text = msg;
    };

    // Resolved once: the shared dialog's header text, so each action can relabel it.
    Text* picker_title = nullptr;
    if (SceneObject* header = picker_dlg.dialog().header().node()) {
        if (SceneObject* title_node = header->find_descendant("Title")) {
            picker_title = title_node->get_component<Text>();
        }
    }

    /**
     * Retargets the one shared dialog and opens it: new title, mode, filter and callback.
     * disconnect_all() first, or every previous action's callback would still be connected
     * and they would all fire on the next confirm.
     */
    auto open_picker = [&](const std::string& title, FileDialogMode mode,
                           std::vector<std::string> exts, const std::string& suggested,
                           std::function<void(const std::string&)> on_pick) {
        picker_dlg.disconnect_all();
        picker_dlg.retarget(mode, std::move(exts));
        if (picker_title) picker_title->text = title;
        picker_dlg.on_confirm(std::move(on_pick));
        picker_dlg.open("", suggested);
    };

    MenuBarHandle menu_bar = rows["MenuStrip"].add_menu_bar("Toolbar", theme.menu.bar_height,
                                  260.0f);
    MenuHandle file_menu = menu_bar.add_menu("File");

    file_menu.add_item("Import map (.yaml)...", [&]() {
        open_picker("Import map", FileDialogMode::Open, {".yaml", ".yml"}, "",
                    [&](const std::string& path) {
                        if (!maps::load_map(path, state.graph, state.config)) {
                            set_status("Failed to load map: " + path);
                            return;
                        }
                        state.base_island_frequency = state.config.noise_island.frequency;
                        state.has_map = true;
                        start_preview_render(state, jobs, render_scale_for(state.zoom));
                        set_status("Loaded " + path);
                    });
    });

    file_menu.add_item("Import config (.yaml)...", [&]() {
        open_picker("Import config", FileDialogMode::Open, {".yaml", ".yml"}, "",
                    [&](const std::string& path) {
                        // load_config never prints -- it reports, and the caller decides how
                        // loud to be about it. Surface its own message rather than inventing one.
                        const maps::ConfigLoadResult r = maps::load_config(path, state.config);
                        if (r.status != maps::ConfigLoad::Ok) {
                            set_status("Config: " + r.message);
                            return;
                        }
                        state.base_island_frequency = state.config.noise_island.frequency;
                        apply_derived(state);
                        set_status("Loaded config " + path + " -- press Refresh to apply.");
                    });
    });

    file_menu.add_separator();

    file_menu.add_item("Export map (.yaml)...", [&]() {
        if (!state.has_map) { set_status("Nothing to export yet -- press Refresh."); return; }
        open_picker("Export map", FileDialogMode::Save, {".yaml"}, "world.yaml",
                    [&](const std::string& path) {
                        try {
                            maps::save_map(state.graph, state.config, path);   // throws
                            set_status("Wrote " + path);
                        } catch (const std::exception& e) {
                            set_status(std::string("Export failed: ") + e.what());
                        }
                    });
    });

    file_menu.add_separator();

    file_menu.add_item("Export current layer (.png)...", [&]() {
        if (!state.has_map) { set_status("Nothing to export yet -- press Refresh."); return; }
        open_picker("Export layer", FileDialogMode::Save, {".png"},
                    std::string(maps::map_layer_name(state.layer)) + ".png",
                    [&](const std::string& path) {
                        // At the full configured image_size, not the preview size -- and on
                        // the job engine, because this is the multi-second path.
                        viewer::set_png_level(state.config.png_compression_level);
                        state.export_task.reset();
                        auto task_state = std::make_shared<maps::MapTaskState>();
                        task_state->total.store(1);
                        const maps::MapConfig export_config = state.config;
                        const maps::MapLayer layer = state.layer;
                        const maps::MapGraph* graph = &state.graph;
                        const maps::BiomePalette* palette = &state.palette;
                        coopa::job::JobHandle h = jobs.create_handle();
                        jobs.submit([=]() {
                                viewer::write_layer_png(layer, *graph, export_config, *palette,
                                                        path);
                                task_state->step();
                                task_state->finish();
                            }, maps::k_map_job_type, h);
                        state.export_task.emplace(&jobs, h, task_state);
                        set_status("Writing " + path + "...");
                    });
    });

    file_menu.add_item("Export all layers (.png)...", [&]() {
        if (!state.has_map) { set_status("Nothing to export yet -- press Refresh."); return; }
        open_picker("Export all layers", FileDialogMode::Save, {}, "world",
                    [&](const std::string& path) {
                        viewer::set_png_level(state.config.png_compression_level);
                        // export_layers_async() holds graph/config/prefix/palette BY REFERENCE
                        // for the task's lifetime, so the prefix must outlive this lambda --
                        // hence ViewerState::export_prefix rather than a local.
                        state.export_prefix = path;
                        state.export_task.reset();
                        state.export_task = viewer::export_all_layers(
                            jobs, state.graph, state.config, state.export_prefix,
                            state.palette, &logger);
                        set_status("Writing " + path + "_*.png ...");
                    });
    });

    root.enable_cursor(ctx.input());
    // One bubble for the whole canvas; the per-row text is attached by with_tooltip() above.
    // Text is transcribed from the @brief docs on the matching MapConfig field -- see
    // coopa/maps/map_config.h, which stays the source of truth if any of it changes.
    root.enable_tooltips();
    coopa::ui::register_ui_animated_properties();

    SceneObject* canvas_raw = scene.add_root_object(std::move(canvas_obj));

    // The single point where every Dialog, CollapsiblePanel and Menu applies its initial
    // visibility -- see each component's start() for why they are built (and left) active
    // until this call reaches them.
    scene.start();
    canvas->set_default_texture(ui_pass.white_view());
    UIResourceCache::instance().mark_text_atlases(ui_pass);

    // Scripted-capture hooks, in the spirit of the per-demo env vars every uicoopa demo
    // carries (OPEN_DIALOG, OPEN_COMBO, ...): they drive a state a screenshot needs without
    // a pointer anywhere near the window. Applied after scene.start(), so they override the
    // initial visibility each component just settled.
    if (std::getenv("EXPAND_ALL")) {
        for (CollapsibleHandle h : {world, terrain, water, features, render}) h.expand();
    }
    // SHAPE=/SURFACE= drive the two gated dropdowns through their own ComboBox, so the panel
    // ends up in exactly the state a click would leave it in -- rows hidden, section re-fitted.
    auto select_by_name = [&](const char* env, const char* combo, std::size_t count,
                              auto name_of) {
        const char* want = std::getenv(env);
        if (!want) return;
        SceneObject* obj = canvas_raw->find_descendant(combo);
        ComboBox* box = obj ? obj->get_component<ComboBox>() : nullptr;
        if (!box) return;
        for (std::size_t i = 0; i < count; ++i) {
            if (name_of(i) == std::string(want)) {
                box->set_current_index(static_cast<int>(i), true);
                return;
            }
        }
        std::cerr << "[map_viewer] " << env << "=" << want << " matched nothing.\n";
    };
    select_by_name("SHAPE", "Shape", maps::k_map_shape_count, [](std::size_t i) {
        return std::string(maps::map_shape_name(static_cast<maps::MapShape>(i)));
    });
    select_by_name("SURFACE", "Surface", maps::k_elevation_surface_count, [](std::size_t i) {
        return std::string(maps::elevation_surface_name(static_cast<maps::ElevationSurface>(i)));
    });

    if (const char* zoom_env = std::getenv("ZOOM")) {
        state.zoom = std::max(1.0f, std::strtof(zoom_env, nullptr));
        apply_view_uv(state);
    }
    // Parks the pointer over a named settings row, so a scripted capture can show the
    // tooltip that row carries without a hand on the mouse.
    const char* hover_row = std::getenv("HOVER_ROW");
    if (std::getenv("OPEN_MENU")) file_menu.open();
    if (std::getenv("OPEN_PICKER")) open_picker("Import map", FileDialogMode::Open,
                                                {".yaml", ".yml"}, "", [](const std::string&) {});
    if (std::getenv("COLLAPSE")) sidebar.collapse();
    if (const char* layer_env = std::getenv("LAYER")) {
        const auto& order = layer_order();
        for (std::size_t i = 0; i < order.size(); ++i) {
            if (maps::map_layer_name(order[i]) != std::string(layer_env)) continue;
            // Through the ComboBox rather than writing state.layer directly, so the picker's
            // own label agrees with what is on screen. notify = true runs the same callback
            // a click would, which is also what kicks the render.
            if (layer_select) layer_select->set_current_index(static_cast<int>(i), true);
            state.layer = order[i];
            break;
        }
    }

    // First map, so the window opens on something rather than an empty pane.
    start_generation(state, jobs, logger);

    while (!ctx.should_close()) {
        ctx.poll();
        if (ctx.input().key_down(coopa::input::Key::Escape)) ctx.window().set_should_close(true);

        const float dt = ctx.delta_time();
        auto [sw, sh] = ctx.window().framebuffer_size();
        if (sw == 0 || sh == 0) continue;  // minimized

        canvas->set_viewport(sw, sh);
        if (hover_row) {
            // Injected in canvas space, the same way a world canvas feeds a ray hit in.
            SceneObject* row = canvas_raw->find_descendant(std::string(hover_row) + "_Row");
            canvas->set_world_input(ctx.input(),
                                    row ? row->get_component<RectTransform>()->rect().center()
                                        : glm::vec2(-1.0e6f));
        } else {
            canvas->set_input(ctx.input());
        }
        assets.update(dt);

        // --- Poll the work in flight. ---
        //
        // Every branch here resets the optional holding a finished task; MapTask's
        // destructor cancels and waits, so a stale task is never left running behind one
        // that replaced it.
        if (state.gen_task && state.gen_task->done()) {
            // Move the graph out rather than copying it -- it is the biggest object here by
            // a wide margin, and the generator has no further use for it.
            state.graph = std::move(state.generator->graph());
            state.has_map = true;
            state.gen_task.reset();
            if (state.refresh_button) state.refresh_button->interactable = true;
            if (state.status_line) state.status_line->text = describe(state);
            start_preview_render(state, jobs, render_scale_for(state.zoom));
        } else if (state.gen_task && state.progress) {
            state.progress->set_value(state.gen_task->progress(), false);
        }

        if (state.render_task && state.render_task->done()) {
            // done() is an OR of "the work said it finished" and "the job handle drained",
            // and the second can go true for work that never ran at all -- an exhausted
            // handle pool hands back an invalid handle that reports complete immediately.
            // Requiring real pixels turns that from a silently-dropped render into a
            // message, rather than a map that just never updates.
            const bool got_pixels = state.pending_preview && state.pending_preview->width > 0;
            if (got_pixels) {
                upload_preview(state, ctx, ui_pass, *state.pending_preview);
            } else if (state.status_line) {
                state.status_line->text = "Preview render was dropped.";
            }
            state.pending_preview.reset();
            state.render_task.reset();
            if (state.progress_node) state.progress_node->set_active(false);
            if (got_pixels && state.status_line) state.status_line->text = describe(state);
        }

        if (state.export_task && state.export_task->done()) {
            state.export_task.reset();
            if (state.status_line) state.status_line->text = "Export finished.";
        } else if (state.export_task && state.progress) {
            if (state.progress_node) state.progress_node->set_active(true);
            state.progress->set_value(state.export_task->progress(), false);
        }

        // --- Keep the map square and letterboxed inside whatever the pane is. ---
        if (state.map_widget && state.map_view) {
            if (auto* view_rt = state.map_view->get_component<RectTransform>()) {
                const glm::vec2 view = view_rt->rect().size();
                const float side = std::max(32.0f, std::min(view.x - 24.0f, view.y - 56.0f));
                state.map_widget->owner->get_component<RectTransform>()
                    ->set_size_delta({side, side});
            }
        }

        // --- Zoom and pan ---
        //
        // Polled rather than handled through EventSystem, and it has to be: EventSystem
        // dispatches press/drag for the LEFT button only (kPrimaryButton), so a middle-drag
        // never reaches a handler at all, and the map Image is built hittable = false so it
        // cannot receive on_scroll either. Gating on the cursor being inside the map rect is
        // what keeps this from fighting the sidebar's ScrollRect -- that only ever sees
        // events dispatched to its own hit chain, so the two cannot both claim a wheel tick.
        if (state.map_widget && state.has_map) {
            auto* map_rt = state.map_widget->owner->get_component<RectTransform>();
            const Rect map_rect = map_rt->rect();
            const glm::vec2 cursor = canvas->input().position();
            const bool over_map = contains(map_rect, cursor);
            const float old_zoom = state.zoom;

            // Where the cursor sits in the current view window, in image space (Y-down).
            const glm::vec2 rect_size = glm::max(map_rect.size(), glm::vec2(1.0f));
            glm::vec2 t = (cursor - map_rect.min) / rect_size;
            t.y = 1.0f - t.y;  // canvas is +Y up, image space is +Y down
            const float span_before = 1.0f / std::max(1.0f, state.zoom);
            const glm::vec2 anchor = state.view_center - span_before * 0.5f + t * span_before;

            if (over_map && canvas->input().scroll_delta().y != 0.0f) {
                // Exponential so each notch is the same proportional step in and out.
                const float step = std::exp(canvas->input().scroll_delta().y * 0.2f);
                state.zoom = glm::clamp(state.zoom * step, 1.0f, 16.0f);
                // Re-centre so the image point that was under the cursor stays under it --
                // without this the view drifts away from whatever you were aiming at.
                const float span_after = 1.0f / state.zoom;
                state.view_center = anchor - (t - 0.5f) * span_after;
                apply_view_uv(state);
            }

            const int k_middle = static_cast<int>(coopa::input::MouseButton::Middle);
            if (over_map && canvas->input().is_button_pressed(k_middle)) state.panning = true;
            if (!canvas->input().is_button_down(k_middle)) state.panning = false;
            if (state.panning) {
                // Drag the map with the pointer: content follows the cursor, so the view
                // window moves the opposite way. Y negated for the same +Y-up/+Y-down flip.
                const glm::vec2 d = canvas->input().delta() / rect_size * span_before;
                state.view_center += glm::vec2(-d.x, d.y);
                apply_view_uv(state);
            }

            const bool ctrl = ctx.input().key_down(coopa::input::Key::LeftControl) ||
                              ctx.input().key_down(coopa::input::Key::RightControl);
            if (ctrl && (ctx.input().key_pressed(coopa::input::Key::Num0) ||
                         ctx.input().key_pressed(coopa::input::Key::Kp0))) {
                state.zoom = 1.0f;
                state.view_center = glm::vec2(0.5f);
                apply_view_uv(state);
            }

            // Sharpen once the zoom has settled. The magnified texture stays up meanwhile, so
            // zooming itself never waits on a render.
            state.zoom_settle = (state.zoom != old_zoom) ? 0.0f : state.zoom_settle + dt;
            const float wanted = render_scale_for(state.zoom);
            if (state.zoom_settle > 0.25f && wanted != state.rendered_scale &&
                !state.render_task && !state.gen_task) {
                start_preview_render(state, jobs, wanted);
            }
        }

        scene.update(dt);
        scene.late_update(dt);

        UIResourceCache::instance().mark_text_atlases(ui_pass);
        ui_pass.register_textures(canvas->draw_list());
        ui_pass.begin_frame(ctx.current_frame(), canvas->draw_list().vertices().size(),
                            canvas->draw_list().indices().size());

        coopa::gfx::app::FrameCallbacks cb;
        cb.record = [&](coopa::gfx::command::CommandBuffer& cmd) {
            ui_pass.draw(cmd, ctx.current_frame(), sw, sh, canvas->scale_factor(),
                         canvas->draw_list());
        };
        cb.clear = coopa::gfx::ClearColor{ 0.05f, 0.05f, 0.07f, 1.0f };
        ctx.frame(cb);

        if (config.headless_oneshot) break;
        if (ctx.max_frames() > 0 && ctx.frame_index() >= ctx.max_frames()) break;
    }

    ctx.wait_idle();

    auto [final_w, final_h] = ctx.window().framebuffer_size();
    const char* out_env = std::getenv("SCREENSHOT_NAME");
    if (out_env && out_env[0] != '\0') {
        const std::string screenshot_path = std::string(ROOT_DIR) + "/output/" + out_env + ".png";
        save_screenshot(ctx, ui_pass, canvas->draw_list(), final_w, final_h,
                        canvas->scale_factor(), screenshot_path);
    }

    std::cout << "[map_viewer] Exiting cleanly.\n";

    // Retire every task BEFORE the graph/config they reference goes out of scope --
    // export_layers_async() in particular holds its arguments by reference for the task's
    // whole lifetime. Each destructor cancels and waits.
    state.export_task.reset();
    state.render_task.reset();
    state.gen_task.reset();
    jobs.shutdown();

    // GPU-resident statics, torn down while the device and allocator are still alive.
    // ThemeLibrary first: a loaded theme holds non-owning Font* into UIResourceCache.
    // Before the texture dies, so ui_pass never outlives a cache entry describing freed
    // memory -- the same contract upload_preview() follows on every swap.
    if (state.map_texture) {
        ui_pass.unregister_texture(state.map_texture->view_typed());
        state.map_texture.reset();
    }
    IconLibrary::instance().clear();
    ThemeLibrary::instance().clear();
    UIResourceCache::instance().clear();
    FontDefaults::font = nullptr;
    FontDefaults::note_text_size = nullptr;
    FontDefaults::resolve_font = nullptr;

    ctx.input().set_cursor_mode(coopa::input::CursorMode::Normal);
    return 0;
}
