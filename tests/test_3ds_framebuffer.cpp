#include "app/3ds_framebuffer.h"

#include <cstdint>
#include <cstdio>
#include <vector>

int main() {
    constexpr int screen_width = 400, screen_height = 240;
    for (const int width : {310, 400}) {
        std::vector<uint32_t> pixels(size_t(width) * screen_height);
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = uint32_t(i * 2654435761u) ^ 0xff7142c3u;
        std::vector<uint8_t> expected(size_t(screen_width) * screen_height * 3);
        std::vector<uint8_t> actual(expected.size());
        const int border = (screen_width - width) / 2;
        for (int x = 0; x < screen_width; ++x)
            for (int y = 0; y < screen_height; ++y) {
                const uint32_t color = x < border || x >= border + width
                    ? 0u : pixels[size_t(y) * width + size_t(x - border)];
                const size_t offset = (size_t(x) * screen_height + size_t(screen_height - 1 - y)) * 3;
                expected[offset] = uint8_t(color);
                expected[offset + 1] = uint8_t(color >> 8);
                expected[offset + 2] = uint8_t(color >> 16);
            }
        app::write_rgb24_top_frame(pixels, actual.data());
        if (actual != expected) {
            std::fprintf(stderr, "framebuffer copy mismatch at width %d\n", width);
            return 1;
        }
    }
    for (const int width : {310, 400}) {
        std::vector<uint32_t> pixels(size_t(width) * screen_height);
        for (size_t i = 0; i < pixels.size(); ++i)
            pixels[i] = uint32_t(i * 2654435761u) ^ 0xff7142c3u;
        std::vector<uint8_t> actual(size_t(screen_width) * screen_height * 4);
        const int border = (screen_width - width) / 2;
        app::write_rgba8_top_frame(pixels, actual.data());
        for (int x = 0; x < screen_width; ++x)
            for (int y = 0; y < screen_height; ++y) {
                const uint32_t color = x < border || x >= border + width
                    ? 0xff000000u : pixels[size_t(y) * width + size_t(x - border)];
                const size_t offset = (size_t(x) * screen_height + size_t(screen_height - 1 - y)) * 4;
                if (actual[offset] != uint8_t(color >> 16) ||
                    actual[offset + 1] != uint8_t(color >> 8) ||
                    actual[offset + 2] != uint8_t(color) ||
                    actual[offset + 3] != uint8_t(color >> 24)) {
                    std::fprintf(stderr, "RGBA framebuffer copy mismatch at width %d x %d y %d\n", width, x, y);
                    return 1;
                }
            }
    }
    std::puts("3DS framebuffer copy tests: 0 failures");
    return 0;
}
