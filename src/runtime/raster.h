// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels, Ville Linde, Aaron Giles
//
// CPU reference rasterizer for the Model 2 3D layer: projects, orders and
// draws a frame's display list (Geo::polys) into a 512x512 RGB32 layer.
// Transplanted from MAME at dddd73680656e355bb2b5beecab1167c9f07bf81
// (BSD-3-Clause; notices above kept as the licence requires):
// src/mame/sega/model2_v.cpp (model2_3d_project, render_polygons,
// model2_renderer::model2_3d_render), src/mame/sega/model2rd.ipp (scanline
// shaders, bilinear texel fetch) and the parts of src/devices/video/poly.h
// they use (render_triangle, render_polygon, round_coordinate), drawn
// immediately in order instead of through MAME's work queue. This is the
// ground truth for the GPU renderer, checked against MAME frame by frame.
// See THIRD_PARTY.md.
#pragma once

#include "runtime/geo.h"

#include <cstdint>
#include <array>
#include <vector>
#include <functional>

namespace rt {
class RenderWorkers;

// The video memories the rasterizer reads, as the i960 sees them.
struct VideoMem {
    const uint8_t *palram = nullptr;    // 0x01800000, 0x4000 bytes (u16 entries)
    const uint8_t *colorxlat = nullptr; // 0x01810000, 0xc000 bytes (u16 entries)
    const uint8_t *lumaram = nullptr;   // 0x12800000, 0x20000 bytes (byte lane 0 of each dword)
    const uint32_t *tex0 = nullptr;     // texture RAM 0, packed as MAME stores it (0x80000 dwords)
    const uint32_t *tex1 = nullptr;     // texture RAM 1
    uint64_t texture_revision = UINT64_MAX; // presentation cache invalidation, not board-visible state
};

class Raster {
public:
    Raster();
    Raster(int max_width, int max_height);
    void set_target(int width, int height, bool widescreen = false);
    // Reclaimed by GPU hosts; the next software render restores storage.
    void release_pixel_storage();
    void set_stereo(float eye_offset, float convergence);
    void set_low_textures(bool enabled) { set_texture_quality(enabled ? 50 : 100); }
    void set_texture_quality(int percent) {
        texture_quality_ = percent == 50 || percent == 25 || percent == 10 ? percent : 100;
        low_textures_ = texture_quality_ != 100;
    }
    int texture_quality() const { return texture_quality_; }
    void set_depth_buffer(bool enabled) { depth_buffer_ = enabled; }
    void set_nearest_textures(bool enabled) { nearest_textures_ = enabled; }
    bool nearest_textures() const { return nearest_textures_; }
    int stride() const { return stride_; }
    int height() const { return height_; }

    // MAME render_polygons: clear, then draw windows from the last down to
    // 0, each in z-bucket order (low to high z; newest first within a
    // bucket). crtc_x/crtc_y: MAME's m_crtc_xoffset/m_crtc_yoffset (the
    // projection); render_x/render_y: the renderer's offsets (the viewport;
    // equal to the CRTC's once the game has set them). clip: the visible
    // area, inclusive.
    void render(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem, int crtc_x, int crtc_y,
                int clip_minx, int clip_maxx, int clip_miny, int clip_maxy) {
        render(polys, windows, mem, crtc_x, crtc_y, crtc_x, crtc_y, clip_minx, clip_maxx, clip_miny, clip_maxy);
    }
    void render(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem, int crtc_x, int crtc_y, int render_x,
                int render_y, int clip_minx, int clip_maxx, int clip_miny, int clip_maxy, RenderWorkers *workers = nullptr);
    void render_stereo_pair(Raster &right, const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem,
                            int crtc_x, int crtc_y, int render_x, int render_y,
                            int clip_minx, int clip_maxx, int clip_miny, int clip_maxy,
                            RenderWorkers *workers, std::array<double, 2> *eye_task_ms = nullptr);

    const uint32_t *pixels() const { return dest_.data(); } // 512x512, 0x00RRGGBB
    uint64_t hash(int minx, int maxx, int miny, int maxy) const; // as the MAME log computes it

    struct Extra {
        uint8_t checker = 0;
        uint32_t lumabase = 0, colorbase = 0;
        const uint32_t *texsheet[2] = {nullptr, nullptr};
        uint32_t texwidth = 0, texheight = 0, texx = 0, texy = 0;
        uint8_t texwrapx = 0, texwrapy = 0, texmirrorx = 0, texmirrory = 0, utex = 0, utexminlod = 0;
        uint32_t utexx = 0, utexy = 0;
        int32_t texlod = 0;
        uint8_t luma = 0, window = 0;
        uint64_t texture_key = 0;
    };
    // Presentation only: emit arcade scan coverage and interpolation to an
    // accelerator instead of shading pixels on the CPU. No game RAM changes.
    using SpanSink = std::function<void(int, int, int, int, const float *, const float *, const Extra &)>;
    void set_span_sink(SpanSink sink) { span_sink_ = std::move(sink); }
    // Polygon accelerator hook: receives the reference projection, viewport,
    // material and perspective attributes, before any CPU scan conversion.
    using PolygonSink = std::function<void(int, const GeoPoly &, const int *, const Extra &)>;
    void set_polygon_sink(PolygonSink sink) { polygon_sink_ = std::move(sink); }
    // Optional GPU input: retain view-space XYZ and original UVs (in eighth texels).
    // Viewport clipping, material setup and painter order remain unchanged.
    void set_raw_polygon_input(bool enabled) { raw_polygon_input_ = enabled; }
    void set_borrow_raw_polygons(bool enabled) { borrow_raw_polygons_ = enabled; }

private:
    void init_gamma();
    SpanSink span_sink_;
    PolygonSink polygon_sink_;
    bool raw_polygon_input_ = false;
    bool borrow_raw_polygons_ = false;
    float eye_offset_ = 0, convergence_ = 1;
    bool low_textures_ = false;
    int texture_quality_ = 100;
    bool nearest_textures_ = false;
    bool depth_buffer_ = false;
    std::vector<float> inverse_depth_;
    std::vector<uint8_t> depth_window_;
    bool fragment_visible(size_t index, float inverse_depth, uint8_t window) const;
    void record_depth(size_t index, float inverse_depth, uint8_t window);
#if defined(DAYTONA_N64_LOW_MEMORY)
    int stride_ = 0, height_ = 0;
#else
    int stride_ = 512, height_ = 512;
#endif
    float scale_x_ = 1, scale_y_ = 1, offset_x_ = 0;
    std::vector<uint32_t> dest_;
    uint8_t gamma_[256];
    const VideoMem *mem_ = nullptr;

    void project(GeoPoly &poly, int crtc_x, int crtc_y) const;
    void render_one(const GeoPoly &source, int crtc_x, int crtc_y, int render_x, int render_y, int clip_minx, int clip_maxx,
                    int clip_miny, int clip_maxy, int pixel_miny = 0, int pixel_maxy = 2147483647, bool projected = false);
    template <bool Translucent> void draw_scanline_solid(int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o);
    template <bool Translucent> void draw_scanline_tex(int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o);
    void scanline(int renderer, int32_t y, int32_t x0, int32_t x1, const float *start, const float *dpdx, const Extra &o);
    void render_triangle(const int *clip, int renderer, const Extra &o, const GeoVertex &v1, const GeoVertex &v2, const GeoVertex &v3);
    template <int NumVerts> void render_polygon(const int *clip, int renderer, const Extra &o, const GeoVertex *v);
    template <bool Translucent> uint32_t fetch_bilinear_texel(const Extra &o, int32_t miplevel, int32_t u, int32_t v) const;
};

} // namespace rt
