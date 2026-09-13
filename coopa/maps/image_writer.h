/**
 * @file image_writer.h
 * @brief Encodes an `Image` to a PNG file on disk.
 *
 * This is the one header in `coopa::maps` that pulls in an implementation
 * rather than just declarations, because stb_image_write is a single-header
 * library with no separate compilation unit.
 *
 * `STB_IMAGE_WRITE_STATIC` gives every translation unit that includes this
 * header its own internal-linkage copy of the encoder. The alternatives were
 * both worse: defining `STB_IMAGE_WRITE_IMPLEMENTATION` with external linkage
 * in a header -- what the original map generator did -- is a one-definition-rule
 * violation the moment a second translation unit includes it, and moving the
 * implementation into a `.cpp` would make libcoopa no longer header-only. The
 * cost is a duplicated encoder in each translation unit that writes a PNG,
 * which in practice is one.
 */

#ifndef COOPA_MAPS_IMAGE_WRITER_H
#define COOPA_MAPS_IMAGE_WRITER_H

#include <string>

#include <coopa/maps/image.h>

#ifndef STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_STATIC
#endif
#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#endif
#include <stb/stb_image_write.h>

namespace coopa {
namespace maps {

/**
 * @brief Writes an image to a PNG file.
 * @param filepath Destination path, including the `.png` extension.
 * @param image The image to encode; must have a non-empty pixel buffer.
 * @return True on success, false if the image is empty or the file cannot be written.
 */
inline bool write_png(const std::string& filepath, const Image& image) {
    if (image.width <= 0 || image.height <= 0 || image.pixels.empty()) {
        return false;
    }
    return stbi_write_png(filepath.c_str(), image.width, image.height, image.channels,
                          image.pixels.data(), image.width * image.channels) != 0;
}

} // namespace maps
} // namespace coopa

#endif // COOPA_MAPS_IMAGE_WRITER_H
