// license:BSD-3-Clause
// copyright-holders:Olivier Galibert, R. Belmont, ElSemi, Angelo Salese
//
// Model 2 screen output: the Sega System 24 tilemap chip (segaic24: four
// 64x64-tile layers, per-line scroll, 8-pixel window masks), the palette the
// tilemaps use, the CRTC offsets, and the composition of the 2D layers with
// the 3D layer (Raster), as MAME's model2_state::screen_update does.
// Transplanted from MAME at dddd73680656e355bb2b5beecab1167c9f07bf81
// (src/mame/sega/segaic24.cpp, model2_v.cpp, model2.cpp; BSD-3-Clause,
// notices above kept as the licence requires). MAME's generic tilemap engine
// is replaced by a direct renderer with the same pixel rules. See
// THIRD_PARTY.md.
#pragma once

#include "runtime/raster.h"
#include "runtime/render_workers.h"
#include <memory>
#include <array>
#include <utility>

#include <cstdint>
#include <vector>

namespace rt {

class Video {
public:
    struct StageTimes {
        double prepare_ms = 0;
        double rear_layers_ms = 0;
        double polygon_stage_ms = 0;
        double front_layers_ms = 0;
    };
    struct PresentationTimes {
        double prepare_ms = 0;
        double raster_ms = 0;
        double compose_ms = 0;
        double total_ms = 0;
        size_t polygons = 0;
        bool cache_hit = false;
    };
    // tile_ram: 0x01000000 (0x10000 bytes, u16 entries); char_ram: 0x01080000
    // (0x80000 bytes, u16 entries); both as the i960 wrote them.
    Video(const uint8_t *tile_ram, const uint8_t *char_ram);

    // Register writes (MAME handlers), fed by the bus as they happen.
    // palette_w after the bus has stored the write (palram holds the new value).
    void palette_w(uint32_t offset, const uint8_t *palram, const uint8_t *colorxlat);
    void colorxlat_w(uint32_t offset) { if ((offset & 0xff) == 0x80 / 2) palette_dirty_ = true; }
    void xhout_w(uint16_t data) { crtc_x_ = 84 + int16_t(data); render_x_ = crtc_x_; }
    void xvout_w(uint16_t data) { crtc_y_ = 130 + int16_t(data); render_y_ = crtc_y_; }

    // The geometrizer started a new frame (MAME render_frame_start).
    void frame_start() { render_done_ = false; }
    // MAME screen_update at the end of vblank: 2D back layers, the 3D layer
    // (drawn from `polys` once per geometrizer frame, then reused), 2D front
    // layers. Desktop output: 496x384; N64 output: sampled 320x240.
    void screen_update(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem, const std::vector<GeoPoly> *wide_polys = nullptr, const std::vector<GeoPoly> *extended_polys = nullptr);
    const std::vector<uint32_t> &screen() const { return screen_; }
    uint64_t screen_hash() const;
    void enable_presentation(bool enabled, bool presentation_only = false) { capture_presentation_ = enabled; presentation_only_ = enabled && presentation_only; }
    const std::vector<uint32_t> &present(int width, int height, float alpha, bool interpolate, float eye_offset = 0, float convergence = 1);
    std::pair<const std::vector<uint32_t> &, const std::vector<uint32_t> &> present_stereo(
        int width, int height, float alpha, bool interpolate, float eye_offset, float convergence);
    // External polygon presentation does not need these CPU render targets.
    // Raster restores them lazily if software presentation is requested later.
    void release_software_targets() {
        raster_.release_pixel_storage();
        presentation_raster_.release_pixel_storage();
        right_eye_raster_.release_pixel_storage();
        invalidate_presentation();
    }
    void set_presentation_profile(bool enabled) { profile_presentation_ = enabled; }
    const PresentationTimes &presentation_times() const { return presentation_times_; }
    const PresentationTimes &right_eye_presentation_times() const { return right_eye_presentation_times_; }
    void set_widescreen(bool enabled) {
        if (widescreen_ != enabled) {
            widescreen_ = enabled; previous_polys_.clear(); current_polys_.clear();
            rebuild_sampled_layer_maps();
            ++presentation_revision_; invalidate_presentation();
        }
    }
    // Render the 2D tile layers only at the exact source pixels consumed by
    // the target output. Intended for the presentation-only handheld path.
    void set_sampled_layer_output(bool enabled, int width = 400, int height = 240);
    // Switch the N64 runtime to a composed, native 320x240 framebuffer.
    void set_native_output(bool enabled);
    std::pair<int, int> sampled_layer_dimensions() const { return {sampled_layer_width_, sampled_layer_height_}; }
    bool sampled_layer_output_active() const {
#if defined(DAYTONA_N64_LOW_MEMORY)
        return native_output_ || (sampled_layer_output_ && capture_presentation_ && presentation_only_);
#else
        return sampled_layer_output_ && capture_presentation_ && presentation_only_;
#endif
    }
    void set_low_textures(bool enabled) {
        set_texture_quality(enabled ? 50 : 100);
    }
    void set_texture_quality(int percent) {
        if (texture_quality_ != percent) {
            texture_quality_ = percent; low_textures_ = percent != 100;
            raster_.set_texture_quality(percent);
            presentation_raster_.set_texture_quality(percent);
            right_eye_raster_.set_texture_quality(percent);
            invalidate_presentation();
        }
    }
    // Ordered layer rectangles for an external GPU compositor. Coordinates are
    // sampled output pixels; source offsets apply to the existing integer maps.
    struct TileSpan {
        int16_t x0,y0,x1,y1,sx,sy;
        uint8_t layer, rule; // 0:any; 1/2:opaque cat0/1; 3/4:any cat0/1.
    };
    void set_tile_span_capture(bool enabled) { tile_span_capture_=enabled || tile_span_output_; }
    void set_tile_span_output(bool enabled);
    bool tile_span_output() const { return tile_span_output_; }
    bool tile_span_capture() const { return tile_span_capture_; }
    const std::vector<TileSpan> &tile_spans(bool front) const { return tile_spans_[front?1:0]; }
    bool validate_tile_spans() const;
    const std::vector<uint16_t> &tile_pixels(unsigned layer) const { return pixmap_[layer]; }
    const uint32_t *tile_colors() const { return pens_; }
    uint64_t tile_identity(unsigned index) const {
        return tile_cache_ ? (uint64_t(tile_cache_->tile_values[index])<<32)|tile_cache_->tile_generation[index] : 0;
    }
    void set_tile_cache(bool enabled);
    void set_ram_write_tracking(bool enabled);
    void tile_ram_written(uint32_t offset, uint32_t size);
    void char_ram_written(uint32_t offset, uint32_t size);
    void set_nearest_textures(bool enabled) {
        nearest_textures_ = enabled;
        raster_.set_nearest_textures(enabled);
        presentation_raster_.set_nearest_textures(enabled);
        right_eye_raster_.set_nearest_textures(enabled);
        invalidate_presentation();
    }
    void set_render_scale(float scale);
    void set_draw_distance(float depth);
    void set_depth_buffer(bool enabled) {
        if (depth_buffer_ != enabled) { depth_buffer_ = enabled; presentation_raster_.set_depth_buffer(enabled); invalidate_presentation(); }
    }
    float stereo_reference_depth() const;
    float stereo_eye_offset(float strength, float convergence, float max_disparity = 4) const;
    bool widescreen() const { return widescreen_; }
    struct PresentationScene {
        const std::vector<GeoPoly> &polys;
        const std::vector<uint32_t> &back, &front;
        const VideoMem &memory;
        int windows, crtc_x, crtc_y, render_x, render_y;
    };
    PresentationScene presentation_scene() const {
        return {current_polys_, back_layer_, front_layer_, presentation_mem_, presentation_windows_,
                presentation_crtc_x_, presentation_crtc_y_, presentation_render_x_, presentation_render_y_};
    }
    void set_render_threads(unsigned count) {
        if (count != render_thread_limit_) {
            render_thread_limit_ = count;
            render_workers_.reset();
            render_workers_ = std::make_unique<RenderWorkers>(count);
            invalidate_presentation();
        }
    }
    void set_parallel_stereo_eyes(bool enabled) {
        if (parallel_stereo_eyes_ != enabled) { parallel_stereo_eyes_ = enabled; invalidate_presentation(); }
    }
    unsigned render_threads() const { return render_workers_ ? render_workers_->size() : 1; }
    uint64_t presentation_generation() const { return presentation_generation_; }
    void invalidate_presentation() { cached_revision_ = UINT64_MAX; }
    size_t interpolated_polygons() const { return interpolated_polygons_; }
    const Raster &raster() const { return raster_; }
    bool rendered_now() const { return rendered_now_; } // the last update drew the 3D layer afresh
    uint64_t raster_hash() const { return raster_.hash(0, 495, 0, 383); }
    const StageTimes &stage_times() const { return stage_times_; }

    static constexpr int W = 496, H = 384;
#if defined(DAYTONA_N64_HALF_RESOLUTION)
    static constexpr int N64_W = 160, N64_H = 120;
#else
    static constexpr int N64_W = 320, N64_H = 240;
#endif

private:
    void emit_tile_span(int layer,int rule,int x0,int y0,int x1,int y1,int sx,int sy);
    bool tile_span_capture_=false, tile_span_output_=false;
    std::vector<TileSpan> tile_spans_[2];
    struct TileCache {
        std::array<uint8_t, 0x80000> chars{};
        std::array<uint32_t, 0x4000> char_generation{};
        std::array<uint16_t, 0x4000> tile_values{};
        std::array<uint32_t, 0x4000> tile_generation{};
        uint32_t generation = 0;
        bool initialized = false;
    };
    std::unique_ptr<TileCache> tile_cache_;
    std::array<uint8_t, 0x800> tile_dirty_{};
    std::array<uint8_t, 0x800> char_dirty_{};
    bool ram_write_tracking_ = false;
    bool sync_tile_cache();
    bool sync_dirty_characters();
    void build_tile(int layer, uint32_t tile_index);
    void rebuild_sampled_layer_maps();
    uint16_t tile(uint32_t i) const { return uint16_t(tile_ram_[i * 2] | tile_ram_[i * 2 + 1] << 8); }
    void build_layer(int layer); // packed pen/category/opacity cache for one tilemap
    void draw(std::vector<uint32_t> &bitmap, int layer, int flags, int min_y, int max_y);
    void draw_rect(std::vector<uint32_t> &dm, const uint16_t *mask, uint16_t tpri, int flags, int win, int L, int sx,
                   int sy, int xx1, int yy1, int xx2, int yy2, int clip_min_y, int clip_max_y);
    void tilemap_draw(std::vector<uint32_t> &dm, int L, int sx, int sy, int minx, int maxx, int miny, int maxy, int flags,
                      int clip_min_y, int clip_max_y);

    const uint8_t *tile_ram_, *char_ram_;
    uint32_t pens_[8192];
    bool palette_dirty_ = false;
    int crtc_x_ = 0, crtc_y_ = 0, render_x_ = 90, render_y_ = -8;
    uint8_t gamma_[256];
    std::vector<uint16_t> pixmap_[4];
    std::vector<uint32_t> screen_, sys24_;
    Raster raster_, presentation_raster_;
    // 3DS presentation tops out at 400x240. The N64 does not use stereo, so
    // leave this scratch raster unallocated until a stereo presentation asks
    // for it; desktop/3DS builds retain their eagerly sized buffer.
#if defined(DAYTONA_N64_LOW_MEMORY)
    Raster right_eye_raster_;
#else
    Raster right_eye_raster_{400, 240};
#endif
    bool parallel_stereo_eyes_ = false;
    std::vector<uint32_t> right_eye_presentation_;
    bool capture_presentation_ = false, widescreen_ = false, low_textures_ = false, presentation_only_ = false;
    bool depth_buffer_ = false;
    bool nearest_textures_ = false;
    int texture_quality_ = 100;
    std::vector<GeoPoly> previous_polys_, current_polys_, blended_polys_;
    std::vector<uint32_t> back_layer_, front_layer_, presentation_;
    VideoMem presentation_mem_{};
    int presentation_windows_ = 0;
    int presentation_crtc_x_ = 0, presentation_crtc_y_ = 0;
    int presentation_render_x_ = 0, presentation_render_y_ = 0;
    size_t interpolated_polygons_ = 0;
    uint64_t presentation_revision_ = 0, cached_revision_ = UINT64_MAX, matches_revision_ = UINT64_MAX;
    uint64_t presentation_generation_ = 0;
    int cached_width_ = 0, cached_height_ = 0;
    int geometry_source_width_ = 0;
    float render_scale_ = 1;
    float cached_alpha_ = -1, cached_eye_offset_ = 0, cached_convergence_ = 1;
    std::vector<size_t> interpolation_matches_;
    std::vector<int> source_x_, hud_source_x_, geometry_source_x_;
    bool sampled_layer_output_ = false;
    bool native_output_ = false;
    bool sampled_layer_front_ = false;
    int sampled_layer_width_ = 0, sampled_layer_height_ = 0;
    std::vector<int> sampled_layer_x_, sampled_layer_hud_x_, sampled_layer_y_;
    bool cached_widescreen_ = false;
    float draw_distance_ = 0;
    std::vector<GeoPoly> distance_polys_;
    unsigned render_thread_limit_ = 0;
    std::unique_ptr<RenderWorkers> render_workers_;
    bool rendered_now_ = false, render_done_ = false;
    StageTimes stage_times_{};
    bool profile_presentation_ = false;
    PresentationTimes presentation_times_{};
    PresentationTimes right_eye_presentation_times_{};
};

} // namespace rt
