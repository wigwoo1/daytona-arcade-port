#include "runtime/video.h"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <atomic>
#include <stdexcept>
#include <vector>

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char *name) { if (!ok) { std::printf("FAIL: %s\n", name); ++failures; } };
    std::vector<uint8_t> tile(0x10000), chars(0x80000), pal(0x4000), color(0xc000, 0xff), luma(0x20000);
    std::vector<uint32_t> tex(0x80000);
    for (int i = 0; i < 4; ++i) tile[(0x5004 + i) * 2 + 1] = 0x80; // no tile layers
    rt::VideoMem mem{pal.data(), color.data(), luma.data(), tex.data(), tex.data()};
    rt::GeoPoly p;
    p.num_vertices = 4; p.viewport[0] = 0; p.viewport[1] = 0; p.viewport[2] = 495; p.viewport[3] = 384;
    p.v[0] = {100, 284, {1, 0, 0}}; p.v[1] = {200, 284, {1, 8, 0}};
    p.v[2] = {200, 184, {1, 8, 8}}; p.v[3] = {100, 184, {1, 0, 8}};
    rt::Raster r;
    r.render({p}, 0, mem, 0, 0, 0, 495, 0, 383);
    check(r.pixels()[150 * 512 + 150] == 0xffffffffu, "native polygon inside");
    check(r.pixels()[50 * 512 + 50] == 0, "native polygon outside");
    {
        auto depth_colors = color;
        for (unsigned channel : {0u, 0x4000u, 0x8000u})
            for (unsigned light = 0; light < 64; ++light)
                depth_colors[channel + light * 2] = uint8_t(light * 4);
        auto depth_mem = mem; depth_mem.colorxlat = depth_colors.data();
        auto near_face = p; near_face.luma = 252; near_face.z = 100;
        auto far_face = p; far_face.z = 0;
        for (auto &vertex : far_face.v) { vertex.x *= 2; vertex.y *= 2; vertex.p[0] = 2; }
        rt::Raster depth_raster;
        depth_raster.render({far_face, near_face}, 1, depth_mem, 0, 0, 0, 495, 0, 383);
        const auto ordered_pixel = depth_raster.pixels()[150 * 512 + 150];
        depth_raster.set_depth_buffer(true);
        depth_raster.render({far_face, near_face}, 1, depth_mem, 0, 0, 0, 495, 0, 383);
        check(depth_raster.pixels()[150 * 512 + 150] != ordered_pixel, "depth selects nearer face despite native bucket ordering");
        const auto near_pixel = depth_raster.pixels()[150 * 512 + 150];
        far_face.window = 1;
        depth_raster.render({far_face, near_face}, 1, depth_mem, 0, 0, 0, 495, 0, 383);
        check(depth_raster.pixels()[150 * 512 + 150] == ordered_pixel, "depth preserves viewport priority over physical distance");
        far_face.window = 0;
        rt::RenderWorkers depth_workers(4);
        depth_raster.render({far_face, near_face}, 1, depth_mem, 0, 0, 0, 0, 0, 495, 0, 383, &depth_workers);
        check(depth_raster.pixels()[150 * 512 + 150] == near_pixel, "parallel depth agrees with serial near surface");
        depth_raster.set_depth_buffer(false);
        depth_raster.render({far_face, near_face}, 1, depth_mem, 0, 0, 0, 495, 0, 383);
        check(depth_raster.pixels()[150 * 512 + 150] == ordered_pixel, "depth off restores original ordering");
    }
    for (const int height : {240, 360, 384, 480, 720, 900, 1080, 1440, 2160}) {
        const int width = (height * 496 + 192) / 384;
        r.set_target(width, height);
        r.render({p}, 0, mem, 0, 0, 0, 495, 0, 383);
        check(r.pixels()[(150 * height / 384) * width + 150 * width / 496] == 0xffffffffu, "scaled polygon inside");
        check(r.pixels()[(50 * height / 384) * width + 50 * width / 496] == 0, "scaled polygon outside");
    }
    // Compare exact pixels across worker counts, non-integral scaling and
    // band boundaries, with overlapping solid/textured/translucent polygons.
    std::vector<rt::GeoPoly> scene;
    for (int n = 0; n < 12; ++n) {
        auto poly = p;
        for (auto &vertex : poly.v) { vertex.x += float(n * 9 - 45); vertex.y += float(n * 17 - 90); }
        poly.z = uint16_t(n % 3); poly.window = uint8_t(n % 2);
        poly.texheader[0] = uint16_t((n % 4) << 13);
        if (n % 3 == 0) poly.texheader[0] |= 0x8000;
        scene.push_back(poly);
    }
    std::fill(tex.begin(), tex.end(), 0x1234abcdu);
    for (unsigned count : {1u, 2u, 4u, 8u, 16u}) {
        rt::RenderWorkers workers(count);
        for (int height : {1, 239, 384, 721, 1080}) {
            const int width = std::max(1, (height * 496 + 192) / 384);
            rt::Raster serial, parallel;
            serial.set_target(width, height); parallel.set_target(width, height);
            for (float eye : {-2.f, 0.f, 2.f}) {
            serial.set_stereo(eye, 4); parallel.set_stereo(eye, 4);
            serial.render(scene, 1, mem, 0, 0, 0, 0, 0, 495, 0, 383);
            parallel.render(scene, 1, mem, 0, 0, 0, 0, 0, 495, 0, 383, &workers);
            check(std::equal(serial.pixels(), serial.pixels() + size_t(width) * height, parallel.pixels()),
                  "parallel rendering matches serial pixels exactly");
            }
        }
        {
            const int width = 100, height = 60;
            rt::Raster expected_left, expected_right, paired_left, paired_right;
            expected_left.set_target(width, height); expected_right.set_target(width, height);
            paired_left.set_target(width, height); paired_right.set_target(width, height);
            expected_left.set_stereo(-2.f, 4.f); expected_right.set_stereo(2.f, 4.f);
            paired_left.set_stereo(-2.f, 4.f); paired_right.set_stereo(2.f, 4.f);
            expected_left.render(scene, 1, mem, 0, 0, 0, 0, 0, 495, 0, 383);
            expected_right.render(scene, 1, mem, 0, 0, 0, 0, 0, 495, 0, 383);
            paired_left.render_stereo_pair(paired_right, scene, 1, mem, 0, 0, 0, 0,
                                           0, 495, 0, 383, &workers);
            check(std::equal(expected_left.pixels(), expected_left.pixels() + size_t(width) * height, paired_left.pixels()),
                  "band-parallel left stereo image matches serial reference exactly");
            check(std::equal(expected_right.pixels(), expected_right.pixels() + size_t(width) * height, paired_right.pixels()),
                  "band-parallel right stereo image matches serial reference exactly");
        }
        std::atomic<int> jobs{0};
        for (int batch = 0; batch < 100; ++batch) workers.run(37, [&](size_t) { ++jobs; });
        check(jobs == 3700, "worker pool does every job exactly once across batches");
        bool caught = false;
        try { workers.run(20, [](size_t i) { if (i == 3) throw std::runtime_error("test"); }); }
        catch (const std::runtime_error &) { caught = true; }
        check(caught, "worker exception reaches caller");
        workers.run(1, [&](size_t) { ++jobs; });
        check(jobs == 3701, "workers reusable after exception");
    }
    bool threw = false;
    try { r.set_target(0, 240); } catch (...) { threw = true; }
    check(threw, "reject invalid size");
    rt::Video v(tile.data(), chars.data()); v.enable_presentation(true);
    // Establish the same offsets used by the simple raster fixture.
    v.xhout_w(uint16_t(-84)); v.xvout_w(uint16_t(-130));
    v.screen_update({p}, 0, mem);
    const auto reference = v.screen();
    {
        auto extra = p; for (auto &vertex : extra.v) vertex.x += 100;
        std::vector<rt::GeoPoly> extended{p, extra};
        v.frame_start(); v.screen_update({p}, 0, mem, nullptr, &extended);
        check(v.screen() == reference, "extended scenery does not change native framebuffer");
        const auto extended_image = v.present(496, 384, 1, false);
        check(extended_image[150 * 496 + 250] == 0xffffffffu, "extended scenery appears in presentation");
        v.frame_start(); v.screen_update({p}, 0, mem);
        check(v.present(496, 384, 1, false) == reference, "extension off restores original frame");
    }
    const auto high = v.present(496, 384, 1, false);
    const auto generation = v.presentation_generation();
    v.present(496, 384, 0.2f, false);
    check(v.presentation_generation() == generation, "identical frame reuses cached pixels");
    check(reference == high, "enhanced 1x agrees with native composition");
    auto q = p; for (auto &vertex : q.v) vertex.x += 20;
    v.frame_start(); v.screen_update({q}, 0, mem);
    const auto before = v.screen_hash();
    const auto a = v.present(496, 384, 0, true);
    check(v.interpolated_polygons() == 1, "matching surface interpolated");
    const auto b = v.present(496, 384, 0.5f, true);
    const auto c = v.present(496, 384, 1, true);
    const auto moving_generation = v.presentation_generation();
    v.present(496, 384, 0.25f, true);
    check(v.presentation_generation() > moving_generation, "changed interpolation phase redraws");
    v.set_render_threads(1); const auto single = v.present(721, 558, 0.5f, true);
    v.set_render_threads(8); const auto multi = v.present(721, 558, 0.5f, true);
    check(single == multi, "parallel composition and interpolation match serial output");
    check(a != b && b != c, "intermediate frame is distinct from endpoints");
    check(v.screen_hash() == before, "presentation never changes native output");
    q.texheader[1] = 1; v.frame_start(); v.screen_update({q}, 0, mem);
    v.present(496, 384, 0.5f, true);
    check(v.interpolated_polygons() == 0, "changed surface is not interpolated");
    v.frame_start(); v.screen_update({q, q}, 0, mem); v.present(496, 384, 0.5f, true);
    check(v.interpolated_polygons() == 0, "topology change is not interpolated");
    v.frame_start(); v.screen_update({p, p}, 0, mem);
    auto moved_face = p;
    for (auto &vertex : moved_face.v) vertex.x += 8;
    auto clipped_face = moved_face; clipped_face.v[0].p[1] += 1;
    v.frame_start(); v.screen_update({moved_face, clipped_face}, 0, mem);
    const auto seam_safe = v.present(1920, 1080, .5f, true);
    check(v.interpolated_polygons() == 0, "unmatched neighbour holds all road faces");
    check(seam_safe == v.present(1920, 1080, 1, false), "mixed topology holds intact native frame at 1080p");
    v.frame_start(); v.screen_update({}, 0, mem);
    const auto blank = v.present(496, 384, 0.5f, true);
    check(blank == v.screen(), "blank geometry does not retain previous polygons");
    // Wider clipping exposes a polygon outside the original view without
    // changing the native framebuffer or stretching the central polygon.
    auto side = p;
    side.viewport[0] = -83; side.viewport[2] = 578;
    for (auto &vertex : side.v) vertex.x -= 170; // x=-70..30
    auto center = p; center.viewport[0] = -83; center.viewport[2] = 578;
    std::vector<rt::GeoPoly> wide_scene{center, side};
    v.set_widescreen(true); v.frame_start(); v.screen_update({p}, 0, mem, &wide_scene);
    const auto native_wide_hash = v.screen_hash();
    const auto wide_image = v.present(992, 576, 1, false);
    check(wide_image[225 * 992 + 49] == 0xffffffffu, "widescreen exposes side geometry");
    check(wide_image[225 * 992 + 349] == 0xffffffffu, "widescreen central geometry keeps scale");
    check(wide_image[225 * 992 + 450] != 0xffffffffu, "central geometry is not stretched");
    check(v.screen_hash() == native_wide_hash, "widescreen leaves native pixels unchanged");
    v.set_render_threads(1); const auto wide_single = v.present(1280, 720, 1, false);
    v.set_render_threads(8); const auto wide_multi = v.present(1280, 720, 1, false);
    check(wide_single == wide_multi, "widescreen workers are pixel identical");
    for (int height : {240, 360, 384, 480, 720, 900, 1080, 1440, 2160}) {
        int width = (height * 16 + 4) / 9;
        check(v.present(width, height, 1, false).size() == size_t(width) * height, "widescreen resolution range");
    }
    v.set_widescreen(false); v.frame_start(); v.screen_update({p}, 0, mem);
    check(v.present(496, 384, 1, false) == reference, "disabling widescreen restores original composition");
    // A quality change must invalidate cached presentation without modifying
    // the native framebuffer, and switching it off must recover exact pixels.
    const auto full_quality = v.present(400, 240, 1, false);
    const auto full_generation = v.presentation_generation();
    const auto native_quality_hash = v.screen_hash();
    v.set_low_textures(true); v.present(400, 240, 1, false);
    check(v.presentation_generation() > full_generation, "texture quality invalidates presentation");
    check(v.screen_hash() == native_quality_hash, "texture quality leaves native framebuffer intact");
    v.set_low_textures(false);
    check(v.present(400, 240, 1, false) == full_quality, "full texture quality restored exactly");
    const auto zero_eye = v.present(400, 240, 1, false, 0, 2);
    check(zero_eye == full_quality, "zero eye matches mono exactly");
    const auto stereo_native_hash = v.screen_hash();
    const auto left_eye = v.present(400, 240, 1, false, -12, 2);
    const auto right_eye = v.present(400, 240, 1, false, 12, 2);
    check(left_eye != right_eye, "stereo eyes have different geometry");
    check(v.screen_hash() == stereo_native_hash, "stereo preserves native framebuffer");
    check(v.present(400, 240, 1, false) == full_quality, "stereo off restores mono exactly");
    bool bad_stereo = false;
    try { v.present(400, 240, 1, false, 3, 0); }
    catch (const std::invalid_argument &) { bad_stereo = true; }
    check(bad_stereo, "invalid stereo convergence rejected");
    r.set_target(496, 384);
    auto depth_poly = p;
    for (auto &vertex : depth_poly.v) { vertex.x *= 2; vertex.y *= 2; vertex.p[0] = 2; }
    r.set_stereo(-12, 2); r.render({depth_poly}, 0, mem, 0, 0, 0, 495, 0, 383);
    check(r.pixels()[150 * 496 + 100] == 0xffffffffu && r.pixels()[150 * 496 + 99] == 0,
          "convergence plane stays at the same screen position");
    r.render({p}, 0, mem, 0, 0, 0, 495, 0, 383);
    check(r.pixels()[150 * 496 + 100] == 0 && r.pixels()[150 * 496 + 106] == 0xffffffffu,
          "near geometry shifts right in the left camera image");
    for (auto &vertex : depth_poly.v) { vertex.x *= 2; vertex.y *= 2; vertex.p[0] = 4; }
    r.render({depth_poly}, 0, mem, 0, 0, 0, 495, 0, 383);
    check(r.pixels()[150 * 496 + 96] == 0 && r.pixels()[150 * 496 + 97] == 0xffffffffu,
          "far geometry shifts left in the left camera image");
    r.set_stereo(12, 2); r.render({p}, 0, mem, 0, 0, 0, 495, 0, 383);
    check(r.pixels()[150 * 496 + 93] == 0 && r.pixels()[150 * 496 + 94] == 0xffffffffu,
          "near geometry shifts left in the right camera image");
    r.render({depth_poly}, 0, mem, 0, 0, 0, 495, 0, 383);
    check(r.pixels()[150 * 496 + 102] == 0 && r.pixels()[150 * 496 + 103] == 0xffffffffu,
          "far geometry shifts right in the right camera image");
    v.enable_presentation(true, true);
    v.frame_start(); v.screen_update({p}, 0, mem);
    check(v.present(400, 240, 1, false) == full_quality,
          "presentation-only draw matches normal mono output");
    check(v.stereo_reference_depth() > 0, "stereo depth available from scene");
    v.frame_start(); v.screen_update({}, 0, mem);
    check(v.stereo_reference_depth() == 0, "blank scene has no stereo depth");

    // The handheld's sampled tile-layer compositor must reproduce the
    // existing full-resolution pipeline at the exact 310x240 and 400x240
    // source positions, including scroll/window modes, HUD mapping and stereo.
    {
        std::vector<uint8_t> sample_tile(0x10000), sample_chars(0x80000);
        std::vector<uint8_t> sample_pal(0x4000), sample_color(0xc000), sample_luma(0x20000);
        std::vector<uint32_t> sample_tex(0x80000);
        auto put_word = [&](uint32_t index, uint16_t value) {
            sample_tile[index * 2] = uint8_t(value);
            sample_tile[index * 2 + 1] = uint8_t(value >> 8);
        };
        for (uint32_t i = 0; i < sample_chars.size(); ++i)
            sample_chars[i] = uint8_t((i * 31u + (i >> 5) * 17u) ^ (i >> 9));
        for (uint32_t layer = 0; layer < 4; ++layer) {
            for (uint32_t t = 0; t < 0x1000; ++t) {
                const uint16_t value = uint16_t(((t * 73u + layer * 211u) & 0x3fffu) |
                    (((t * 19u + layer * 47u) & 0xffu) << 7) | ((t + layer) & 1u ? 0x8000u : 0));
                put_word(layer * 0x1000u + t, value);
            }
            put_word(0x5000u + layer, uint16_t(11u + layer * 27u));
            put_word(0x5004u + layer, uint16_t(23u + layer * 31u));
        }
        for (uint32_t i = 0; i < 0x1000; ++i) {
            const uint16_t value = uint16_t((i * 29u) & 0x7fffu);
            sample_pal[i * 2] = uint8_t(value); sample_pal[i * 2 + 1] = uint8_t(value >> 8);
        }
        for (uint32_t channel = 0; channel < 3; ++channel)
            for (uint32_t component = 0; component < 32; ++component) {
                const uint32_t table_index = channel * 0x2000u + component * 0x100u;
                const uint16_t value = uint16_t((component * (7u + channel * 4u) + 29u + channel * 53u) & 0xffu);
                sample_color[table_index * 2] = uint8_t(value);
                sample_color[table_index * 2 + 1] = uint8_t(value >> 8);
            }
        rt::VideoMem sample_mem{sample_pal.data(), sample_color.data(), sample_luma.data(), sample_tex.data(), sample_tex.data()};
        for (const bool wide : {false, true}) {
            const int output_width = wide ? 400 : 310;
            rt::Video full_layers(sample_tile.data(), sample_chars.data());
            rt::Video sampled_layers(sample_tile.data(), sample_chars.data());
            full_layers.enable_presentation(true, false);
            sampled_layers.enable_presentation(true, true);
            full_layers.set_render_threads(2); sampled_layers.set_render_threads(2);
            sampled_layers.set_sampled_layer_output(true, output_width, 240);
            full_layers.set_widescreen(wide); sampled_layers.set_widescreen(wide);
            for (unsigned pen = 0; pen < 0x1000; ++pen) {
                full_layers.palette_w(pen, sample_pal.data(), sample_color.data());
                sampled_layers.palette_w(pen, sample_pal.data(), sample_color.data());
            }
            for (int mode = 0; mode < 4; ++mode) {
                for (uint32_t layer = 0; layer < 4; ++layer) {
                    const uint32_t hscroll = 0x5000u + layer;
                    const uint32_t vscroll = 0x5004u + layer;
                    if (mode == 0) {
                        put_word(hscroll, uint16_t(17u + layer * 23u));
                        put_word(vscroll, uint16_t(29u + layer * 13u));
                    } else if (mode == 1 || mode == 3) {
                        put_word(hscroll, 0x8000u);
                        if (mode == 1) put_word(vscroll, uint16_t(15u + layer * 9u));
                        const uint32_t table = 0x4000u + 0x200u * layer;
                        for (uint32_t y = 0; y < 0x200; ++y)
                            put_word(table + y, uint16_t((y * 11u + layer * 37u) & 0x1ffu));
                    } else {
                        put_word(hscroll, uint16_t(5u + layer * 7u));
                        put_word(vscroll, uint16_t((layer < 2 ? 0x2000u : 0x6000u) | (13u + layer * 3u)));
                    }
                }
                full_layers.frame_start(); sampled_layers.frame_start();
                full_layers.screen_update({p}, 0, sample_mem);
                sampled_layers.screen_update({p}, 0, sample_mem);
                for (const float eye : {-3.f, 3.f}) {
                    const auto expected = full_layers.present(output_width, 240, 1, false, eye, 2);
                    const auto actual = sampled_layers.present(output_width, 240, 1, false, eye, 2);
                    check(actual == expected, "sampled tile layers match full-resolution composition pixel-for-pixel");
                }
            }
            sampled_layers.set_parallel_stereo_eyes(true);
            const auto expected_left = full_layers.present(output_width, 240, 1, false, -3.f, 2);
            const auto expected_right = full_layers.present(output_width, 240, 1, false, 3.f, 2);
            const auto [paired_left, paired_right] = sampled_layers.present_stereo(output_width, 240, 1, false, 3.f, 2);
            check(paired_left == expected_left && paired_right == expected_right,
                  "parallel stereo eyes match sampled full-resolution composition pixel-for-pixel");
            full_layers.set_parallel_stereo_eyes(true);
            const auto [full_pair_left, full_pair_right] = full_layers.present_stereo(output_width, 240, 1, false, 3.f, 2);
            check(full_pair_left == expected_left && full_pair_right == expected_right,
                  "parallel stereo eyes match unsampled composition pixel-for-pixel");
        }
        for (const bool wide : {false, true}) {
            const int output_width = wide ? 400 : 310;
            for (const float scale : {1.f, .75f, .5f, .25f}) {
                const int layer_width = std::max(1, int(output_width * scale));
                const int layer_height = std::max(1, int(240 * scale));
                rt::Video scaled_layers(sample_tile.data(), sample_chars.data());
                scaled_layers.enable_presentation(true, true);
                scaled_layers.set_sampled_layer_output(true, layer_width, layer_height);
                scaled_layers.set_widescreen(wide);
                scaled_layers.set_render_scale(scale);
                for (unsigned pen = 0; pen < 0x1000; ++pen)
                    scaled_layers.palette_w(pen, sample_pal.data(), sample_color.data());
                scaled_layers.frame_start();
                scaled_layers.screen_update({}, 0, sample_mem);
                const auto scene = scaled_layers.presentation_scene();
                const auto &actual = scaled_layers.present(output_width, 240, 1, false, 0, 2);
                for (int y = 0; y < 240; ++y) for (int x = 0; x < output_width; ++x) {
                    const size_t src = size_t(y * layer_height / 240) * layer_width +
                                       size_t(x * layer_width / output_width);
                    const uint32_t expected = scene.front[src] ? scene.front[src] : scene.back[src];
                    check(actual[size_t(y) * output_width + x] == expected,
                          "scaled sampled layers nearest-upscale to the 3DS output size");
                }
            }
        }
    }
    v.frame_start(); v.screen_update({p}, 0, mem);
    for (float depth : {0.001f, 0.1f, 0.5f, 2.f, 100.f, 100000.f}) {
        auto near = p; for (auto &vertex : near.v) vertex.p[0] = depth;
        v.frame_start(); v.screen_update({p, near}, 0, mem);
        for (float strength : {0.f, 0.25f, 0.5f, 1.f, 2.f}) {
            const auto eye = v.stereo_eye_offset(strength, 2);
            check(std::isfinite(eye) && eye >= 0, "stereo slider returns finite eye separation");
            check(std::abs(eye * (1.0 / depth - 0.5)) <= 4.00001,
                  "stereo limits near and far geometry disparity");
            if (!strength) check(eye == 0, "slider off gives exact mono");
        }
    }
    v.frame_start(); v.screen_update({}, 0, mem);
    check(v.stereo_eye_offset(1, 2) == 0, "blank scene uses mono");
    // Cache must detect writes through the bus's byte arrays, including chars
    // shared by many tiles and tiles changing code, color or priority.
    rt::Video uncached(tile.data(), chars.data()), cached(tile.data(), chars.data()), tracked(tile.data(), chars.data());
    cached.set_tile_cache(true);
    tracked.set_tile_cache(true);
    tracked.set_ram_write_tracking(true);
    for (int step = 0; step < 12; ++step) {
        if (step == 1) { std::fill(tile.begin(), tile.end(), 0); tracked.tile_ram_written(0, 0x8000); }
        if (step == 2) { std::fill(chars.begin(), chars.end(), 0x12); tracked.char_ram_written(0, 0x80000); }
        if (step == 3) { chars[1] = 0x34; tracked.char_ram_written(1, 1); }
        if (step == 4) { tile[0] = 1; tracked.tile_ram_written(0, 1); }
        if (step == 5) { tile[1] = 0x80; tracked.tile_ram_written(1, 1); }
        if (step == 6) { pal[2] = 1; color[0x280] = 200; }
        if (step == 7) { cached.set_tile_cache(false); tracked.set_tile_cache(false); }
        if (step == 8) { cached.set_tile_cache(true); tracked.set_tile_cache(true); }
        for (unsigned pen = 0; pen < 8192; ++pen) {
            uncached.palette_w(pen, pal.data(), color.data());
            cached.palette_w(pen, pal.data(), color.data());
            tracked.palette_w(pen, pal.data(), color.data());
        }
        uncached.frame_start(); cached.frame_start(); tracked.frame_start();
        uncached.screen_update({}, 0, mem); cached.screen_update({}, 0, mem); tracked.screen_update({}, 0, mem);
        check(uncached.screen() == cached.screen(), "tile cache tracks RAM and palette mutations");
        check(uncached.screen() == tracked.screen(), "write-tracked tile cache matches full rebuilds");
    }
    // Distinct native mip levels prove quality selection changes real texels.
    std::vector<uint32_t> mip0(0x80000, 0), mip1(0x80000, 0x11111111);
    for (int y=0; y<64; ++y) for (int x=0; x<32; ++x) mip1[y*256+x]=0x22222222;
    std::vector<uint8_t> mip_pal(0x4000), mip_color(0xc000), mip_luma(0x20000);
    mip_luma[32]=255;
    for (unsigned base : {0u, 0x4000u, 0x8000u}) mip_color[base+126]=255;
    rt::VideoMem mip_mem{mip_pal.data(),mip_color.data(),mip_luma.data(),mip0.data(),mip1.data()};
    auto textured=p; textured.texheader[0]=0x4000; textured.luma=255; textured.texlod=256;
    rt::Raster quality; quality.render({textured},0,mip_mem,0,0,0,495,0,383);
    const auto full_pixel=quality.pixels()[150*512+150];
    quality.set_low_textures(true); quality.render({textured},0,mip_mem,0,0,0,495,0,383);
    check(full_pixel==0xff000000u && quality.pixels()[150*512+150]==0xffffffffu,
          "half texture mode selects the next native mip's texels");
    textured.texheader[0]|=0x1000;
    quality.render({textured},0,mip_mem,0,0,0,495,0,383);
    check(quality.pixels()[150*512+150]==0xffffffffu,
          "half texture mode omits full-detail microtexture blend");
    quality.set_nearest_textures(true);
    quality.render({textured},0,mip_mem,0,0,0,495,0,383);
    check(quality.pixels()[150*512+150]==0xffffffffu,
          "nearest sampling retains the selected native mip palette");
    auto transparent = textured;
    transparent.texheader[0] = 0x6000;
    std::fill(mip1.begin(), mip1.end(), 0xffffffffu);
    quality.render({transparent},0,mip_mem,0,0,0,495,0,383);
    check(quality.pixels()[150*512+150]==0,
          "nearest translucent sampling leaves transparent texels uncovered");
    std::fill(mip1.begin(), mip1.end(), 0x22222222u);
    quality.render({transparent},0,mip_mem,0,0,0,495,0,383);
    check(quality.pixels()[150*512+150]!=0,
          "nearest translucent sampling retains opaque texel coverage");
    quality.set_nearest_textures(false);
    quality.set_low_textures(false); quality.render({textured},0,mip_mem,0,0,0,495,0,383);
    check(quality.pixels()[150*512+150]==full_pixel,"full texture mode restored");
    mip_luma[64]=255;
    quality.set_texture_quality(25); quality.render({textured},0,mip_mem,0,0,0,495,0,383);
    check(quality.pixels()[150*512+150]==full_pixel,"quarter textures sample the second smaller native mip");
    quality.set_texture_quality(10); quality.render({textured},0,mip_mem,0,0,0,495,0,383);
    check(quality.pixels()[150*512+150]==0xffffffffu,"ten percent textures sample and rescale the third smaller native mip");
    quality.set_texture_quality(100); quality.render({textured},0,mip_mem,0,0,0,495,0,383);
    check(quality.pixels()[150*512+150]==full_pixel,"texture quality returns to original texels");
    v.enable_presentation(true); v.frame_start(); v.screen_update({p},0,mem);
    const auto full_resolution=v.present(400,240,1,false);
    const auto scale_native=v.screen_hash();
    for(float scale : {.25f,.5f,.75f,1.f}) {
        v.set_render_scale(scale); const auto reduced=v.present(400,240,1,false);
        check(reduced.size()==400*240,"render scale retains screen output resolution");
        check(v.screen_hash()==scale_native,"render scale preserves native framebuffer");
    }
    check(v.present(400,240,1,false)==full_resolution,"full render resolution restored exactly");
    v.frame_start(); v.screen_update({},0,mem);const auto hud_full=v.present(400,240,1,false);
    v.set_render_scale(.5f);check(v.present(400,240,1,false)==hud_full,"render scale leaves tilemap text unchanged");
    bool bad_scale=false;try{v.set_render_scale(0);}catch(const std::invalid_argument&){bad_scale=true;}
    check(bad_scale,"invalid render scale rejected");
    v.set_render_scale(1); v.frame_start(); v.screen_update({p},0,mem);
    const auto unrestricted = v.present(400,240,1,false);
    const auto distance_native = v.screen_hash();
    v.set_draw_distance(.01f);
    check(v.present(400,240,1,false) != unrestricted,"far geometry removed by draw distance");
    check(v.screen_hash() == distance_native,"draw distance preserves native framebuffer");
    v.set_draw_distance(0);
    check(v.present(400,240,1,false) == unrestricted,"full draw distance restored exactly");
    bool bad_distance=false; try{v.set_draw_distance(-1);}catch(const std::invalid_argument&){bad_distance=true;}
    check(bad_distance,"negative draw distance rejected");
    {
        // Batch refresh must match the existing per-entry conversion across
        // palette writes and translation-table changes, including ignored high bytes.
        std::vector<uint8_t> tiles(0x10000), glyphs(0x80000), palette(0x4000), xlat(0xc000);
        for (size_t i = 0; i < glyphs.size(); ++i) glyphs[i] = uint8_t((i & 15) * 17);
        for (unsigned layer = 0; layer < 4; ++layer)
            for (unsigned cell = 0; cell < 4096; ++cell) {
                const unsigned value = (cell & 255) << 7;
                const size_t off = (layer * 4096 + cell) * 2;
                tiles[off] = uint8_t(value); tiles[off + 1] = uint8_t(value >> 8);
            }
        rt::Video batch(tiles.data(), glyphs.data()), reference(tiles.data(), glyphs.data());
        rt::VideoMem colors{palette.data(), xlat.data(), luma.data(), tex.data(), tex.data()};
        batch.colorxlat_w(0x40);
        std::vector<uint32_t> previous_palette_image;
        for (unsigned phase = 0; phase < 3; ++phase) {
            for (size_t i = 0; i < xlat.size(); ++i) xlat[i] = uint8_t(i * 37 + (i >> 8) * 53 + phase * 83);
            for (unsigned i = 0; i < 8192; ++i) {
                const unsigned value = i * 173 + phase * 391;
                palette[i * 2] = uint8_t(value); palette[i * 2 + 1] = uint8_t(value >> 8);
                reference.palette_w(i, palette.data(), xlat.data());
                if (i >= 4096) batch.palette_w(i, palette.data(), xlat.data());
            }
            batch.screen_update({}, 0, colors); reference.screen_update({}, 0, colors);
            check(batch.screen() == reference.screen(), "batch palette refresh matches per-entry native conversion");
            auto distinct = reference.screen();
            std::sort(distinct.begin(), distinct.end());
            check(std::unique(distinct.begin(), distinct.end()) - distinct.begin() >= 64,
                "palette comparison exercises diverse visible colors");
            check(phase == 0 || previous_palette_image != reference.screen(),
                "palette comparison observes translation and RAM changes");
            previous_palette_image = reference.screen();
        }
    }
    {
        // Parallel scanline bands must preserve ordered System 24 layers,
        // h/v scroll wrapping, per-line scroll and special window modes.
        std::vector<uint8_t> tiles(0x10000), glyphs(0x80000), palette(0x4000), xlat(0xc000), luma_ram(0x20000);
        std::vector<uint32_t> texture_ram(0x80000);
        auto put_word = [&](uint32_t address, uint16_t value) {
            tiles[address * 2] = uint8_t(value);
            tiles[address * 2 + 1] = uint8_t(value >> 8);
        };
        for (size_t i = 0; i < glyphs.size(); ++i) glyphs[i] = uint8_t((i * 37 + i / 7) & 0xff);
        for (size_t i = 0; i < xlat.size(); ++i) xlat[i] = uint8_t((i * 19 + i / 256 * 53) & 0xff);
        for (size_t i = 0; i < palette.size(); i += 2) {
            palette[i] = uint8_t(i * 13);
            palette[i + 1] = uint8_t(i >> 3);
        }
        for (unsigned layer = 0; layer < 4; ++layer)
            for (unsigned cell = 0; cell < 4096; ++cell)
                put_word(layer * 4096 + cell, uint16_t(((cell + layer * 29) & 0xff) << 7));
        rt::Video serial(tiles.data(), glyphs.data()), parallel(tiles.data(), glyphs.data());
        serial.set_render_threads(1);
        parallel.set_render_threads(2);
        rt::VideoMem band_mem{palette.data(), xlat.data(), luma_ram.data(), texture_ram.data(), texture_ram.data()};
        for (unsigned pen = 0; pen < 8192; ++pen) {
            serial.palette_w(pen, palette.data(), xlat.data());
            parallel.palette_w(pen, palette.data(), xlat.data());
        }
        std::vector<uint64_t> hashes;
        for (unsigned mode = 0; mode < 6; ++mode) {
            std::fill(tiles.begin() + 0x4000 * 2, tiles.end(), 0);
            for (unsigned y = 0; y < 512; ++y)
                for (unsigned group = 0; group < 4; ++group)
                    put_word(0x4000 + group * 0x200 + y, uint16_t((y * 13 + group * 47) & 0x1ff));
            for (unsigned y = 0; y < 0x800; ++y) {
                put_word(0x6000 + y, uint16_t((y * 0x123 + mode * 0x421) & 0xffff));
                put_word(0x6800 + y, uint16_t((y * 0x321 + mode * 0x249) & 0xffff));
            }
            for (unsigned group = 0; group < 4; ++group) {
                const uint16_t hscroll = (mode == 1 || mode == 3) ? 0x8000 : uint16_t(480 + mode * 17);
                const uint16_t vscroll = uint16_t((mode == 5 ? 511 : mode * 29) & 0x1ff);
                put_word(0x5000 + group, hscroll);
                put_word(0x5004 + group, vscroll);
            }
            const uint16_t special = mode == 2 ? 0x2000 : mode == 3 ? 0x4000 : mode == 4 ? 0x6000 : 0;
            put_word(0x5004, uint16_t(special | (mode == 5 ? 511 : mode * 29)));
            put_word(0x5006, uint16_t(special | (mode == 5 ? 511 : mode * 29)));
            serial.frame_start(); parallel.frame_start();
            serial.screen_update({}, 0, band_mem); parallel.screen_update({}, 0, band_mem);
            check(serial.screen() == parallel.screen(), "parallel System 24 scanline bands match serial pixels");
            hashes.push_back(serial.screen_hash());
        }
        check(std::adjacent_find(hashes.begin(), hashes.end(), std::not_equal_to<uint64_t>()) != hashes.end(),
              "scanline fixtures exercise distinct scroll and window modes");
    }
    std::printf("presentation tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
