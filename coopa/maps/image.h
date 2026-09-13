/**
 * @file image.h
 * @brief A plain 8-bit interleaved pixel buffer, and the rasterisation
 *        primitives the map renderers draw into it with.
 */

#ifndef COOPA_MAPS_IMAGE_H
#define COOPA_MAPS_IMAGE_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include <glm/glm.hpp>

#include <coopa/maps/map_data.h>

namespace coopa {
namespace maps {

/**
 * @struct Image
 * @brief An 8-bit-per-channel image with interleaved components, origin top-left.
 *
 * Deliberately not a texture: libcoopa carries no graphics dependency, so this
 * is a buffer a consumer can upload, encode or discard as it likes.
 */
struct Image {
    int width = 0;    /**< @brief Width in pixels. */
    int height = 0;   /**< @brief Height in pixels. */
    int channels = 3; /**< @brief Components per pixel; 3 for RGB. */
    std::vector<unsigned char> pixels; /**< @brief `width * height * channels` bytes. */

    /**
     * @brief Allocates the buffer and clears it to a colour.
     * @param w Width in pixels.
     * @param h Height in pixels.
     * @param c Components per pixel.
     * @param color Clear colour, in 0-255 component range.
     */
    void reset(int w, int h, int c, const glm::vec3& color) {
        width = w;
        height = h;
        channels = c;
        pixels.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h)
                          * static_cast<std::size_t>(c), 0);
        fill(color);
    }

    /**
     * @brief Overwrites every pixel with one colour.
     * @param color The colour to fill with, in 0-255 component range.
     */
    void fill(const glm::vec3& color) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                set_pixel(x, y, color);
            }
        }
    }

    /**
     * @brief Writes one pixel, ignoring coordinates outside the image.
     * @param x Column.
     * @param y Row.
     * @param color The colour to write, in 0-255 component range; clamped.
     */
    void set_pixel(int x, int y, const glm::vec3& color) {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return;
        }
        const std::size_t index =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
             + static_cast<std::size_t>(x)) * static_cast<std::size_t>(channels);
        pixels[index] = to_byte(color.r);
        if (channels > 1) pixels[index + 1] = to_byte(color.g);
        if (channels > 2) pixels[index + 2] = to_byte(color.b);
    }

    /**
     * @brief Darkens one pixel by a fixed amount per channel.
     *
     * Dimming rather than overwriting is what lets the elevation render show a
     * river without hiding the height beneath it. Because subtraction compounds,
     * call this through `darken_masked()` rather than per stroke -- see the note
     * there.
     *
     * @param x Column.
     * @param y Row.
     * @param amount Value subtracted from every channel, clamped at zero.
     */
    void darken_pixel(int x, int y, int amount) {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return;
        }
        const std::size_t index =
            (static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
             + static_cast<std::size_t>(x)) * static_cast<std::size_t>(channels);
        for (int c = 0; c < channels; ++c) {
            const int value = static_cast<int>(pixels[index + static_cast<std::size_t>(c)]) - amount;
            pixels[index + static_cast<std::size_t>(c)] =
                static_cast<unsigned char>(std::max(0, value));
        }
    }

    /**
     * @brief Converts a 0-255 float component to a byte.
     *
     * Clamps rather than casting straight through: a palette entry outside the
     * range would otherwise wrap and produce a pixel of an unrelated colour.
     *
     * @param value The component value.
     * @return The clamped byte.
     */
    static unsigned char to_byte(float value) {
        return static_cast<unsigned char>(std::clamp(value, 0.0f, 255.0f));
    }
};

/**
 * @brief Fills a triangle using half-space edge functions.
 *
 * Accepts either winding by testing the sign of the triangle's own area, and
 * includes pixels exactly on an edge so adjacent triangles of a fan leave no
 * seam between them.
 *
 * @param image The target buffer.
 * @param a First vertex, in pixel coordinates.
 * @param b Second vertex.
 * @param c Third vertex.
 * @param color Fill colour, in 0-255 component range.
 */
inline void fill_triangle(Image& image, const MapPoint& a, const MapPoint& b, const MapPoint& c,
                          const glm::vec3& color) {
    const auto edge_function = [](const MapPoint& p, const MapPoint& q, const MapPoint& r) {
        return (r.x - p.x) * (q.y - p.y) - (r.y - p.y) * (q.x - p.x);
    };

    const int min_x = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int max_x = std::min(image.width - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int min_y = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int max_y = std::min(image.height - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));

    const double area = edge_function(a, b, c);
    const bool positive_area = area > 0.0;

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const MapPoint point{static_cast<double>(x), static_cast<double>(y)};
            const double w0 = edge_function(b, c, point);
            const double w1 = edge_function(c, a, point);
            const double w2 = edge_function(a, b, point);

            const bool on_edge = w0 == 0.0 || w1 == 0.0 || w2 == 0.0;
            const bool inside = positive_area ? (w0 > 0.0 && w1 > 0.0 && w2 > 0.0)
                                              : (w0 < 0.0 && w1 < 0.0 && w2 < 0.0);
            if (on_edge || inside) {
                image.set_pixel(x, y, color);
            }
        }
    }
}

/**
 * @brief Fills a convex polygon by fanning triangles out from its centroid.
 * @param image The target buffer.
 * @param vertices The polygon outline, already ordered, in pixel coordinates.
 * @param color Fill colour, in 0-255 component range.
 */
inline void fill_polygon(Image& image, const std::vector<MapPoint>& vertices,
                         const glm::vec3& color) {
    if (vertices.size() < 3) {
        return;
    }
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (const MapPoint& vertex : vertices) {
        sum_x += vertex.x;
        sum_y += vertex.y;
    }
    const double count = static_cast<double>(vertices.size());
    const MapPoint centroid{sum_x / count, sum_y / count};

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        fill_triangle(image, centroid, vertices[i], vertices[(i + 1) % vertices.size()], color);
    }
}

/**
 * @brief Fills a triangle, choosing each pixel's colour from a shader.
 * @tparam Shader Callable of `(double x, double y) -> glm::vec3`, in pixel coordinates.
 * @param image The target buffer.
 * @param a First vertex, in pixel coordinates.
 * @param b Second vertex.
 * @param c Third vertex.
 * @param shader Supplies the colour for each covered pixel.
 */
template <typename Shader>
inline void fill_triangle_shaded(Image& image, const MapPoint& a, const MapPoint& b,
                                 const MapPoint& c, Shader&& shader) {
    const auto edge_function = [](const MapPoint& p, const MapPoint& q, const MapPoint& r) {
        return (r.x - p.x) * (q.y - p.y) - (r.y - p.y) * (q.x - p.x);
    };

    const int min_x = std::max(0, static_cast<int>(std::floor(std::min({a.x, b.x, c.x}))));
    const int max_x = std::min(image.width - 1, static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}))));
    const int min_y = std::max(0, static_cast<int>(std::floor(std::min({a.y, b.y, c.y}))));
    const int max_y = std::min(image.height - 1, static_cast<int>(std::ceil(std::max({a.y, b.y, c.y}))));

    const double area = edge_function(a, b, c);
    const bool positive_area = area > 0.0;

    for (int y = min_y; y <= max_y; ++y) {
        for (int x = min_x; x <= max_x; ++x) {
            const MapPoint point{static_cast<double>(x), static_cast<double>(y)};
            const double w0 = edge_function(b, c, point);
            const double w1 = edge_function(c, a, point);
            const double w2 = edge_function(a, b, point);

            const bool on_edge = w0 == 0.0 || w1 == 0.0 || w2 == 0.0;
            const bool inside = positive_area ? (w0 > 0.0 && w1 > 0.0 && w2 > 0.0)
                                              : (w0 < 0.0 && w1 < 0.0 && w2 < 0.0);
            if (on_edge || inside) {
                image.set_pixel(x, y, shader(point.x, point.y));
            }
        }
    }
}

/**
 * @brief Fills a convex polygon with a per-pixel shaded colour.
 *
 * The gradient counterpart to `fill_polygon()`. A heightmap flat-shaded per cell
 * shows the Voronoi tessellation rather than the terrain; sampling a height
 * function per pixel is what makes a continuous surface out of the same data.
 *
 * @tparam Shader Callable of `(double x, double y) -> glm::vec3`, in pixel coordinates.
 * @param image The target buffer.
 * @param vertices The polygon outline, already ordered, in pixel coordinates.
 * @param shader Supplies the colour for each covered pixel.
 */
template <typename Shader>
inline void fill_polygon_shaded(Image& image, const std::vector<MapPoint>& vertices,
                                Shader&& shader) {
    if (vertices.size() < 3) {
        return;
    }
    double sum_x = 0.0;
    double sum_y = 0.0;
    for (const MapPoint& vertex : vertices) {
        sum_x += vertex.x;
        sum_y += vertex.y;
    }
    const double count = static_cast<double>(vertices.size());
    const MapPoint centroid{sum_x / count, sum_y / count};

    for (std::size_t i = 0; i < vertices.size(); ++i) {
        fill_triangle_shaded(image, centroid, vertices[i], vertices[(i + 1) % vertices.size()],
                             shader);
    }
}

/**
 * @brief Draws a Bresenham line with a square brush.
 * @param image The target buffer.
 * @param x0 Start column.
 * @param y0 Start row.
 * @param x1 End column.
 * @param y1 End row.
 * @param half_width Brush radius in pixels; 0 draws a single-pixel line.
 * @param color Stroke colour, in 0-255 component range.
 */
inline void draw_line(Image& image, int x0, int y0, int x1, int y1, int half_width,
                      const glm::vec3& color) {
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    while (true) {
        for (int j = -half_width; j <= half_width; ++j) {
            for (int i = -half_width; i <= half_width; ++i) {
                image.set_pixel(x0 + i, y0 + j, color);
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int error2 = 2 * error;
        if (error2 >= dy) { error += dy; x0 += sx; }
        if (error2 <= dx) { error += dx; y0 += sy; }
    }
}

/**
 * @struct CoverageMask
 * @brief A one-bit-per-pixel stencil marking where an overlay layer applies.
 *
 * Exists so an overlay whose per-pixel effect is *not* idempotent can still be
 * drawn from overlapping strokes. See `darken_masked()`.
 */
struct CoverageMask {
    int width = 0;  /**< @brief Width in pixels. */
    int height = 0; /**< @brief Height in pixels. */
    std::vector<bool> covered; /**< @brief `width * height` bits, row-major. */

    /**
     * @brief Sizes the mask and clears every bit.
     * @param w Width in pixels.
     * @param h Height in pixels.
     */
    void reset(int w, int h) {
        width = w;
        height = h;
        covered.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), false);
    }

    /**
     * @brief Sets one bit, ignoring coordinates outside the mask.
     * @param x Column.
     * @param y Row.
     */
    void mark(int x, int y) {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return;
        }
        covered[static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                + static_cast<std::size_t>(x)] = true;
    }

    /**
     * @brief Reads one bit.
     * @param x Column.
     * @param y Row.
     * @return True if the pixel is covered; false for it, or for any coordinate outside the mask.
     */
    bool at(int x, int y) const {
        if (x < 0 || y < 0 || x >= width || y >= height) {
            return false;
        }
        return covered[static_cast<std::size_t>(y) * static_cast<std::size_t>(width)
                       + static_cast<std::size_t>(x)];
    }
};

/**
 * @brief Marks a Bresenham line with a square brush into a coverage mask.
 *
 * The same walk and brush as `draw_line()`, but it sets bits instead of writing
 * pixels, so calling it repeatedly over the same ground costs nothing.
 *
 * @param mask The mask to mark into.
 * @param x0 Start column.
 * @param y0 Start row.
 * @param x1 End column.
 * @param y1 End row.
 * @param half_width Brush radius in pixels; 0 marks a single-pixel line.
 */
inline void mark_line(CoverageMask& mask, int x0, int y0, int x1, int y1, int half_width) {
    int dx = std::abs(x1 - x0);
    int dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    while (true) {
        for (int j = -half_width; j <= half_width; ++j) {
            for (int i = -half_width; i <= half_width; ++i) {
                mask.mark(x0 + i, y0 + j);
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int error2 = 2 * error;
        if (error2 >= dy) { error += dy; x0 += sx; }
        if (error2 <= dx) { error += dx; y0 += sy; }
    }
}

/**
 * @brief Darkens every marked pixel by `amount`, exactly once.
 *
 * Darkening is subtraction, which does not survive being applied twice. Drawing
 * strokes straight into the image compounds wherever they overlap -- and they
 * always do: a square brush covers each pixel on three consecutive steps of its
 * own Bresenham walk, before any two strokes meet at a shared endpoint. Marking
 * the whole layer into a stencil first and applying it in one pass makes the
 * result depend on *which* pixels a layer covers rather than on how many times
 * it happened to cover them.
 *
 * @param image The target buffer.
 * @param mask Which pixels to darken; coordinates outside the image are ignored.
 * @param amount Value subtracted from every channel, clamped at zero.
 */
inline void darken_masked(Image& image, const CoverageMask& mask, int amount) {
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            if (mask.at(x, y)) {
                image.darken_pixel(x, y, amount);
            }
        }
    }
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_IMAGE_H
