#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace app {

// Copy a 400x240 RGB image to the 3DS top-screen 24-bit framebuffer. The
// framebuffer is column-major and vertically flipped; small tiles keep nearby
// source pixels in cache while retaining the console's native byte layout.
inline void write_rgb24_top_frame(const std::vector<uint32_t> &pixels, uint8_t *framebuffer) {
    constexpr int screen_width = 400;
    constexpr int screen_height = 240;
    constexpr int tile_width = 8;
    constexpr int tile_height = 16;
    const int width = int(pixels.size() / screen_height);
    const int border = (screen_width - width) / 2;
    for (int tile_x = 0; tile_x < screen_width; tile_x += tile_width)
        for (int tile_y = 0; tile_y < screen_height; tile_y += tile_height)
            for (int x = tile_x; x < tile_x + tile_width; ++x)
                for (int y = tile_y; y < tile_y + tile_height; ++y) {
                    const uint32_t color = x < border || x >= border + width
                        ? 0u : pixels[size_t(y) * width + size_t(x - border)];
                    const size_t offset = (size_t(x) * screen_height + size_t(screen_height - 1 - y)) * 3;
                    framebuffer[offset] = uint8_t(color);
                    framebuffer[offset + 1] = uint8_t(color >> 8);
                    framebuffer[offset + 2] = uint8_t(color >> 16);
                }
}

// RGBA8 framebuffers store bytes in R,G,B,A order. Runtime pixels are held as
// 0xAARRGGBB, so swap the red and blue byte positions before the aligned word
// store. This is the New 3DS default because the 32-bit copy is substantially
// faster than writing 24-bit pixels one channel at a time.
inline void write_rgba8_top_frame(const std::vector<uint32_t> &pixels, uint8_t *framebuffer) {
    constexpr int screen_width = 400;
    constexpr int screen_height = 240;
    const int width = int(pixels.size() / screen_height);
    const int border = (screen_width - width) / 2;
    auto *dst = reinterpret_cast<uint32_t *>(framebuffer);
    for (int x = 0; x < screen_width; ++x) {
        const bool inside = x >= border && x < border + width;
        const size_t source_x = inside ? size_t(x - border) : 0;
        size_t offset = size_t(x) * screen_height + screen_height - 1;
        for (int y = 0; y < screen_height; ++y, --offset) {
            const uint32_t color = inside ? pixels[size_t(y) * width + source_x] : 0xff000000u;
            dst[offset] = (color & 0xff00ff00u) |
                          ((color & 0x000000ffu) << 16) |
                          ((color & 0x00ff0000u) >> 16);
        }
    }
}

} // namespace app
