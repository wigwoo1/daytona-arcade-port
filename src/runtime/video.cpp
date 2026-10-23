// license:BSD-3-Clause
// copyright-holders:Olivier Galibert, R. Belmont, ElSemi, Angelo Salese
//
// Model 2 screen output, transplanted from MAME (see video.h): segaic24's
// tile_info, draw_rect (rgb32) and draw_common; model2_state::palette_w,
// colorxlat_w, horizontal/vertical_sync_w, render_polygons' frame logic and
// screen_update. MAME's tilemap engine is replaced by build_layer (the same
// pixmap and flags MAME's tilemap caches hold) and tilemap_draw (the same
// pixel rule as tilemap_t::draw with one scroll value).

#include "runtime/video.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <stdexcept>
#if defined(N64) && defined(DAYTONA_N64_LOW_MEMORY)
#include <debug.h>
#endif

namespace rt {

namespace {
constexpr uint8_t PIXEL_LAYER0 = 0x10;  // TILEMAP_PIXEL_LAYER0
constexpr uint8_t CATEGORY_MASK = 0x0f; // TILEMAP_PIXEL_CATEGORY_MASK
constexpr int DRAW_OPAQUE = 0x80;       // TILEMAP_DRAW_OPAQUE
// Cached pens use their otherwise-unused high bits for the tilemap metadata.
// Palette indices are at most 4095, so bits 12 and 13 safely hold opacity and
// category while eliminating the separate one-byte-per-pixel flags planes.
constexpr uint16_t CACHED_OPAQUE = 0x1000;
constexpr uint16_t CACHED_CATEGORY = 0x2000;
inline uint16_t cached_pen(uint16_t pixel) { return pixel & 0x0fff; }
inline bool cached_draw_rect_pixel(uint16_t pixel, uint16_t tpri) {
    const uint16_t expected = uint16_t(CACHED_OPAQUE | ((tpri & CATEGORY_MASK) ? CACHED_CATEGORY : 0));
    return (pixel & (CACHED_OPAQUE | CACHED_CATEGORY)) == expected;
}
inline bool cached_tilemap_pixel(uint16_t pixel, uint8_t cat, bool opaque) {
    const uint8_t pixel_category = (pixel & CACHED_CATEGORY) ? 1 : 0;
    return pixel_category == cat && (opaque || (pixel & CACHED_OPAQUE));
}
inline uint16_t le16(const uint8_t *b, uint32_t i) { return uint16_t(b[i * 2] | b[i * 2 + 1] << 8); }
inline uint32_t rgb(uint32_t r, uint32_t g, uint32_t b) { return 0xff000000u | (r << 16) | (g << 8) | b; }
#if defined(N64) && defined(DAYTONA_N64_LOW_MEMORY)
#include "n64_tile_sample.inc"
#define DAYTONA_TILE_PIXEL(layer, index) n64_sample_tile(tile_ram_, char_ram_, (layer), (index))
#else
#define DAYTONA_TILE_PIXEL(layer, index) pixmap_[(layer)][(index)]
#endif
inline size_t tile_pixel_index(int x, int y) {
#if defined(DAYTONA_N64_LOW_MEMORY)
    return size_t((y & 511) >> 1) * 256 + size_t((x & 511) >> 1);
#else
    return size_t(y & 511) * 512 + size_t(x & 511);
#endif
}
} // namespace

Video::Video(const uint8_t *tile_ram, const uint8_t *char_ram)
    : tile_ram_(tile_ram), char_ram_(char_ram), screen_(size_t(
#if defined(N64) && defined(DAYTONA_N64_LOW_MEMORY)
          N64_W) * N64_H)
#else
          W) * H)
#endif
#if !defined(DAYTONA_N64_LOW_MEMORY)
      , sys24_(size_t(W) * (H + 4))
#endif
{
    for (auto &p : pens_) p = rgb(0, 0, 0); // palette_device starts black
    for (int i = 0; i < 256; i++) gamma_[i] = uint8_t(std::max((double(i) - 64.0) * 255.0 / 191.0, 0.0));
#if defined(N64) && defined(DAYTONA_N64_LOW_MEMORY)
    if (!n64_tile_sampling_self_test()) throw std::runtime_error("N64 tile sampling self-test failed");
    debugf("DAYTONA_N64_TEST tile_sampling=passed comparisons=262144\n");
#else
    for (int l = 0; l < 4; l++)
#if defined(DAYTONA_N64_LOW_MEMORY)
        pixmap_[l].assign(256 * 256, 0);
#else
        pixmap_[l].assign(512 * 512, 0);
#endif
#endif
#if defined(DAYTONA_N64_LOW_MEMORY)
    raster_.set_target(N64_W, N64_H, false);
#endif
#if defined(N64) && defined(DAYTONA_N64_LOW_MEMORY)
    native_output_ = true;
    set_sampled_layer_output(true, N64_W, N64_H);
#endif
}

void Video::set_native_output(bool enabled) {
#if defined(DAYTONA_N64_LOW_MEMORY)
    if (native_output_ == enabled) return;
    native_output_ = enabled;
    if (enabled) {
        set_sampled_layer_output(true, N64_W, N64_H);
        std::vector<uint32_t>(size_t(N64_W) * N64_H, pens_[0]).swap(screen_);
    } else {
        set_sampled_layer_output(false);
        screen_.assign(size_t(W) * H, pens_[0]);
    }
#else
    (void)enabled;
#endif
}

void Video::palette_w(uint32_t offset, const uint8_t *palram, const uint8_t *colorxlat) {
    const uint16_t palcolor = le16(palram, offset);
    const uint8_t r = uint8_t(le16(colorxlat, (0x0080 >> 1) + (((palcolor >> 0) & 0x1f) << 8)));
    const uint8_t g = uint8_t(le16(colorxlat, (0x4080 >> 1) + (((palcolor >> 5) & 0x1f) << 8)));
    const uint8_t b = uint8_t(le16(colorxlat, (0x8080 >> 1) + (((palcolor >> 10) & 0x1f) << 8)));
    pens_[offset & 0x1fff] = rgb(gamma_[r], gamma_[g], gamma_[b]);
}

void Video::set_tile_cache(bool enabled) {
    if (enabled && !tile_cache_) {
        tile_cache_ = std::make_unique<TileCache>();
        if (ram_write_tracking_) { tile_dirty_.fill(0xff); char_dirty_.fill(0xff); }
    }
    if (!enabled) tile_cache_.reset();
}
void Video::set_ram_write_tracking(bool enabled) {
    ram_write_tracking_ = enabled;
    if (enabled) { tile_dirty_.fill(0xff); char_dirty_.fill(0xff); }
}
void Video::set_tile_span_output(bool enabled) {
    if(tile_span_output_==enabled) return;
    if(enabled && (!sampled_layer_output_active() || native_output_))
        throw std::invalid_argument("GPU tile output requires sampled presentation-only layers");
    tile_span_output_=enabled;
    if(enabled) {
        tile_span_capture_=true;
        std::vector<uint32_t>().swap(screen_);
        std::vector<uint32_t>().swap(sys24_);
        std::vector<uint32_t>().swap(back_layer_);
        std::vector<uint32_t>().swap(front_layer_);
    } else {
        screen_.assign(size_t(W)*H,pens_[0]);
#if !defined(DAYTONA_N64_LOW_MEMORY)
        sys24_.assign(size_t(W)*(H+4),0u);
#endif
    }
    invalidate_presentation();
}
void Video::set_sampled_layer_output(bool enabled, int width, int height) {
    if(tile_span_output_ && !enabled) set_tile_span_output(false);
    if (enabled && (width <= 0 || height <= 0 || width > 3840 || height > 2160))
        throw std::invalid_argument("sampled layer target outside 1..3840 x 1..2160");
    if (sampled_layer_output_ == enabled && (!enabled ||
        (sampled_layer_width_ == width && sampled_layer_height_ == height))) return;
    sampled_layer_output_ = enabled;
    if (enabled) {
        sampled_layer_width_ = width;
        sampled_layer_height_ = height;
        rebuild_sampled_layer_maps();
    } else {
        sampled_layer_x_.clear(); sampled_layer_hud_x_.clear(); sampled_layer_y_.clear();
    }
    back_layer_.clear(); front_layer_.clear();
    invalidate_presentation();
}
void Video::rebuild_sampled_layer_maps() {
    if (!sampled_layer_output_) return;
    sampled_layer_x_.resize(sampled_layer_width_);
    sampled_layer_hud_x_.resize(sampled_layer_width_);
    sampled_layer_y_.resize(sampled_layer_height_);
    for (int x = 0; x < sampled_layer_width_; ++x) {
        sampled_layer_x_[x] = x * W / sampled_layer_width_;
        sampled_layer_hud_x_[x] = widescreen_
            ? int(std::floor((double(x) - double(sampled_layer_width_) / 8.0) * W /
                             (double(sampled_layer_width_) * 0.75)))
            : sampled_layer_x_[x];
    }
    for (int y = 0; y < sampled_layer_height_; ++y)
        sampled_layer_y_[y] = y * H / sampled_layer_height_;
}
void Video::tile_ram_written(uint32_t offset, uint32_t size) {
    if (!ram_write_tracking_ || !size || offset >= 0x8000) return;
    const uint32_t first = offset >> 1;
    const uint32_t last = std::min<uint32_t>(0x3fffu, uint32_t((offset + size - 1) >> 1));
    for (uint32_t i = first; i <= last; ++i) tile_dirty_[i >> 3] |= uint8_t(1u << (i & 7));
}
void Video::char_ram_written(uint32_t offset, uint32_t size) {
    if (!ram_write_tracking_ || !size || offset >= 0x80000) return;
    const uint32_t first = offset >> 5;
    const uint32_t last = std::min<uint32_t>(0x3fffu, uint32_t((offset + size - 1) >> 5));
    for (uint32_t i = first; i <= last; ++i) char_dirty_[i >> 3] |= uint8_t(1u << (i & 7));
}
bool Video::sync_tile_cache() {
    if (!tile_cache_) return true;
    auto &cache = *tile_cache_;
    ++cache.generation;
    if (!cache.generation) { cache.initialized = false; ++cache.generation; }
    // Static character RAM is common during gameplay. A single contiguous
    // comparison avoids thousands of individual glyph comparisons in that case.
    if (cache.initialized &&
        std::memcmp(cache.chars.data(), char_ram_, cache.chars.size()) == 0) return false;
    for (size_t code = 0; code < 0x4000; ++code) {
        auto *old = cache.chars.data() + code * 32;
        const auto *current = char_ram_ + code * 32;
        if (!cache.initialized || std::memcmp(old, current, 32)) {
            std::memcpy(old, current, 32);
            cache.char_generation[code] = cache.generation;
        }
    }
    return true;
}
bool Video::sync_dirty_characters() {
    if (!tile_cache_) return true;
    auto &cache = *tile_cache_;
    ++cache.generation;
    if (!cache.generation) { cache.initialized = false; ++cache.generation; }
    bool changed = false;
    for (size_t byte = 0; byte < char_dirty_.size(); ++byte) {
        uint8_t bits = char_dirty_[byte];
        while (bits) {
            unsigned bit = 0;
            while (!(bits & (1u << bit))) ++bit;
            const size_t code = byte * 8 + bit;
            auto *old = cache.chars.data() + code * 32;
            const auto *current = char_ram_ + code * 32;
            if (!cache.initialized || std::memcmp(old, current, 32)) {
                std::memcpy(old, current, 32);
                cache.char_generation[code] = cache.generation;
                changed = true;
            }
            bits &= uint8_t(bits - 1);
        }
    }
    char_dirty_.fill(0);
    return changed;
}

// segaic24 tile_info + MAME tilemap pixmap: 64x64 tiles (TILEMAP_SCAN_ROWS)
// of 8x8, 4bpp chars (char_layout, bit order swapped within 16-bit words),
// pen = color * 16 + pixel, pen 0 transparent, category = tile bit 15.
void Video::build_tile(int layer, uint32_t t) {
    const uint32_t base = uint32_t(layer) * 0x1000; // tile_info_0s/0w/1s/1w
    const uint16_t val = tile(t | base);
    const uint32_t code = val & 0x3fff;
    if (tile_cache_) {
        auto &cache = *tile_cache_;
        const size_t index = base | t;
        const auto generation = cache.char_generation[code];
        if (cache.initialized && cache.tile_values[index] == val &&
            cache.tile_generation[index] == generation) return;
        cache.tile_values[index] = val;
        cache.tile_generation[index] = generation;
    }
    const uint32_t color = (val >> 7) & 0xff;
    const uint8_t category = (val & 0x8000) ? 1 : 0;
    const uint32_t tx = (t & 63) *
#if defined(DAYTONA_N64_LOW_MEMORY)
        4, ty = (t >> 6) * 4;
#else
        8, ty = (t >> 6) * 8;
#endif
    uint16_t *pm = pixmap_[layer].data();
#if defined(DAYTONA_N64_LOW_MEMORY)
    for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++) {
            const uint32_t source_x = x * 2, source_y = y * 2;
            const uint32_t b = code * 32 + source_y * 4 + (source_x >> 1);
#else
    for (uint32_t y = 0; y < 8; y++)
        for (uint32_t x = 0; x < 8; x++) {
            const uint32_t b = code * 32 + y * 4 + (x >> 1);
#endif
            const uint8_t byte = char_ram_[b ^ 1];
            const uint8_t pix =
#if defined(DAYTONA_N64_LOW_MEMORY)
                (source_x & 1) ? (byte & 0x0f) : (byte >> 4);
#else
                (x & 1) ? (byte & 0x0f) : (byte >> 4);
#endif
            const size_t i =
#if defined(DAYTONA_N64_LOW_MEMORY)
                size_t(ty + y) * 256 + (tx + x);
#else
                size_t(ty + y) * 512 + (tx + x);
#endif
            pm[i] = uint16_t(color * 16 + pix) | (pix ? CACHED_OPAQUE : 0) |
                    (category ? CACHED_CATEGORY : 0);
        }
}
void Video::build_layer(int layer) {
    for (uint32_t t = 0; t < 64 * 64; t++) build_tile(layer, t);
}

// segaic24 draw_rect, rgb32 version (model 1/2): copy a rectangle of the
// layer's pixmap to the bitmap through the 8-pixel window mask.
static inline void copy_rect_span(const uint16_t *src, uint32_t *dst,
                                  const uint32_t *pens, uint16_t tpri, int count, bool opaque) {
    if (opaque) {
        for (; count >= 8; count -= 8, src += 8, dst += 8) {
            dst[0] = pens[cached_pen(src[0])]; dst[1] = pens[cached_pen(src[1])];
            dst[2] = pens[cached_pen(src[2])]; dst[3] = pens[cached_pen(src[3])];
            dst[4] = pens[cached_pen(src[4])]; dst[5] = pens[cached_pen(src[5])];
            dst[6] = pens[cached_pen(src[6])]; dst[7] = pens[cached_pen(src[7])];
        }
        for (; count; --count) *dst++ = pens[cached_pen(*src++)];
    } else {
        for (; count >= 8; count -= 8, src += 8, dst += 8) {
            if (cached_draw_rect_pixel(src[0], tpri)) dst[0] = pens[cached_pen(src[0])];
            if (cached_draw_rect_pixel(src[1], tpri)) dst[1] = pens[cached_pen(src[1])];
            if (cached_draw_rect_pixel(src[2], tpri)) dst[2] = pens[cached_pen(src[2])];
            if (cached_draw_rect_pixel(src[3], tpri)) dst[3] = pens[cached_pen(src[3])];
            if (cached_draw_rect_pixel(src[4], tpri)) dst[4] = pens[cached_pen(src[4])];
            if (cached_draw_rect_pixel(src[5], tpri)) dst[5] = pens[cached_pen(src[5])];
            if (cached_draw_rect_pixel(src[6], tpri)) dst[6] = pens[cached_pen(src[6])];
            if (cached_draw_rect_pixel(src[7], tpri)) dst[7] = pens[cached_pen(src[7])];
        }
        for (; count; --count, ++src, ++dst)
            if (cached_draw_rect_pixel(*src, tpri)) *dst = pens[cached_pen(*src)];
    }
}

void Video::emit_tile_span(int layer,int rule,int x0,int y0,int x1,int y1,int sx,int sy) {
    if(x0>=x1 || y0>=y1) return;
    auto &spans=tile_spans_[sampled_layer_front_?1:0];
    if(!spans.empty()) {
        auto &last=spans.back();
        if(last.layer==layer && last.rule==rule && last.sx==sx && last.sy==sy) {
            if(last.x0==x0 && last.x1==x1 && last.y1==y0) { last.y1=int16_t(y1); return; }
            if(last.y0==y0 && last.y1==y1 && last.x1==x0) { last.x1=int16_t(x1); return; }
        }
    }
    if(spans.size()>=16384) throw std::runtime_error("GPU tile command capacity exceeded");
    spans.push_back({int16_t(x0),int16_t(y0),int16_t(x1),int16_t(y1),int16_t(sx),int16_t(sy),uint8_t(layer),uint8_t(rule)});
}

bool Video::validate_tile_spans() const {
    if(!tile_span_capture_ || tile_span_output_ || !sampled_layer_output_active() || native_output_) return false;
    const size_t count=size_t(sampled_layer_width_)*sampled_layer_height_;
    std::vector<uint32_t> replay(count);
    for(unsigned front=0;front<2;++front) {
        std::fill(replay.begin(),replay.end(),front?0u:pens_[0]);
        const auto &xm=front?sampled_layer_hud_x_:sampled_layer_x_;
        for(const auto &s:tile_spans_[front]) {
            if(s.x0<0 || s.y0<0 || s.x1>sampled_layer_width_ || s.y1>sampled_layer_height_ || s.layer>=4 || s.rule>4) return false;
            for(int y=s.y0;y<s.y1;++y) for(int x=s.x0;x<s.x1;++x) {
                const uint16_t p=DAYTONA_TILE_PIXEL(s.layer, tile_pixel_index(xm[x]+s.sx,sampled_layer_y_[y]+s.sy));
                const unsigned category=(p>>13)&1;
                const bool paint=s.rule==0 || (s.rule<=2 ? ((p&0x1000) && category==unsigned(s.rule-1)) : category==unsigned(s.rule-3));
                if(paint) replay[size_t(y)*sampled_layer_width_+x]=pens_[p&0xfff];
            }
        }
        if(replay!=(front?front_layer_:back_layer_)) return false;
    }
    return true;
}
void Video::draw_rect(std::vector<uint32_t> &dm, const uint16_t *mask, uint16_t tpri, int flags, int win, int L, int sx,
                      int sy, int xx1, int yy1, int xx2, int yy2, int clip_min_y, int clip_max_y) {
    const int original_y1 = yy1;
    const int clipped_y1 = std::max(yy1, clip_min_y);
    const int clipped_y2 = std::min(yy2, clip_max_y);
    if (clipped_y1 >= clipped_y2) return;
    if (sampled_layer_output_active()) {
        const bool opaque = (flags & DRAW_OPAQUE) != 0;
        tpri |= PIXEL_LAYER0;
        const auto &x_map = sampled_layer_front_ ? sampled_layer_hud_x_ : sampled_layer_x_;
        const int ox_begin = int(std::lower_bound(x_map.begin(), x_map.end(), xx1) - x_map.begin());
        const int ox_end = int(std::lower_bound(x_map.begin(), x_map.end(), xx2) - x_map.begin());
        const int oy_begin = int(std::lower_bound(sampled_layer_y_.begin(), sampled_layer_y_.end(), clipped_y1) - sampled_layer_y_.begin());
        const int oy_end = int(std::lower_bound(sampled_layer_y_.begin(), sampled_layer_y_.end(), clipped_y2) - sampled_layer_y_.begin());
        if(tile_span_capture_) {
            for(int oy=oy_begin;oy<oy_end;++oy) {
                const int y=sampled_layer_y_[oy];
                const uint16_t *row=mask+y*4;
                int run=-1;
                for(int ox=ox_begin;ox<ox_end;) {
                    const int x=x_map[ox];
                    uint16_t bits=row[x>>7]; if(win) bits=uint16_t(~bits);
                    const bool visible=!(bits&(0x8000u>>((x&127)>>3)));
                    // Uniform mask words cover 128 native pixels; no need to
                    // search the sampled map once for every eight-pixel bit.
                    const int boundary=(bits==0 || bits==0xffff)?((x&~127)+128):((x&~7)+8);
                    const int next=int(std::lower_bound(x_map.begin()+ox+1,x_map.begin()+ox_end,boundary)-x_map.begin());
                    if(visible && run<0) run=ox;
                    if(!visible && run>=0) {
                        emit_tile_span(L,opaque?0:1+(tpri&1),run,oy,ox,oy+1,sx-xx1,sy-original_y1); run=-1;
                    }
                    ox=next;
                }
                if(run>=0) emit_tile_span(L,opaque?0:1+(tpri&1),run,oy,ox_end,oy+1,sx-xx1,sy-original_y1);
            }
        }
        if(tile_span_output_) return;
        for (int oy = oy_begin; oy < oy_end; ++oy) {
            const int native_y = sampled_layer_y_[oy];
            const uint16_t *window_row = mask + native_y * 4;
            const int source_y = sy + native_y - original_y1;
            uint32_t *dst = &dm[size_t(oy) * sampled_layer_width_];
            for (int ox = ox_begin; ox < ox_end; ++ox) {
                const int native_x = x_map[ox];
                uint16_t window = window_row[native_x >> 7];
                if (win) window = uint16_t(~window);
                if (window & (0x8000u >> ((native_x & 127) >> 3))) continue;
                const size_t source = tile_pixel_index(sx + native_x - xx1, source_y);
                const uint16_t pixel = DAYTONA_TILE_PIXEL(L, source);
                if (opaque || cached_draw_rect_pixel(pixel, tpri))
                    dst[ox] = pens_[cached_pen(pixel)];
            }
        }
        return;
    }
#if defined(DAYTONA_N64_LOW_MEMORY)
    const bool opaque = (flags & DRAW_OPAQUE) != 0;
    tpri |= PIXEL_LAYER0;
    for (int y = clipped_y1; y < clipped_y2; ++y) {
        const int source_y = sy + y - original_y1;
        const uint16_t *mask_row = mask + size_t(y) * 4;
        uint32_t *dest = dm.data() + size_t(y) * W;
        for (int x = xx1; x < xx2; ++x) {
            uint16_t window = mask_row[x >> 7];
            if (win) window = uint16_t(~window);
            if (window & (0x8000u >> ((x & 127) >> 3))) continue;
            const int source_x = sx + x - xx1;
            const size_t source = tile_pixel_index(source_x, source_y);
            const uint16_t pixel = DAYTONA_TILE_PIXEL(L, source);
            if (opaque || cached_draw_rect_pixel(pixel, tpri)) dest[x] = pens_[cached_pen(pixel)];
        }
    }
    return;
#else
    const size_t source_offset = size_t(sy + clipped_y1 - original_y1) * 512 + size_t(sx);
    const uint16_t *source = &pixmap_[L][source_offset];
    uint32_t *dest = &dm[size_t(clipped_y1) * W + size_t(xx1)];
    const bool opaque = (flags & DRAW_OPAQUE) != 0;
    tpri |= PIXEL_LAYER0;
    mask += clipped_y1 * 4;
    yy2 = clipped_y2 - clipped_y1;
    yy1 = clipped_y1;
    while (xx1 >= 128) {
        xx1 -= 128;
        xx2 -= 128;
        mask++;
    }
    for (int y = 0; y < yy2; y++) {
        const uint16_t *src = source;
        uint32_t *dst = dest;
        const uint16_t *mask1 = mask;
        int llx = xx2;
        int cur_x = xx1;
        while (llx > 0) {
            uint16_t m = *mask1++;
            if (win) m = uint16_t(~m);
            if (!cur_x && llx >= 128) {
                if (!m) {
                    copy_rect_span(src, dst, pens_, tpri, 128, opaque);
                    src += 128; dst += 128;
                } else if (m == 0xffff) {
                    src += 128; dst += 128;
                } else {
                    for (int x = 0; x < 128; x += 8) {
                        if (!(m & 0x8000)) copy_rect_span(src, dst, pens_, tpri, 8, opaque);
                        src += 8; dst += 8;
                        m = uint16_t(m << 1);
                    }
                }
            } else {
                const int llx1 = llx >= 128 ? 128 : llx;
                if (!m) {
                    const int count = llx1 - cur_x;
                    copy_rect_span(src, dst, pens_, tpri, count, opaque);
                    src += count; dst += count;
                } else if (m == 0xffff) {
                    src += 128 - cur_x; dst += 128 - cur_x;
                } else {
                    for (int x = cur_x; x < llx1;) {
                        const int count = std::min(llx1 - x, 8 - (x & 7));
                        if (!(m & (0x8000 >> (x >> 3))))
                            copy_rect_span(src, dst, pens_, tpri, count, opaque);
                        src += count; dst += count;
                        x += count;
                    }
                }
            }
            llx -= 128;
            cur_x = 0;
        }
        source += 512; dest += W; mask += 4;
    }
#endif
}

// tilemap_t::draw with one scroll value: dest (x, y) takes pixmap
// ((x + sx) & 511, (y + sy) & 511) where (flags & mask) == value; mask is the
// category, plus layer 0 (opacity) unless drawing opaque.
void Video::tilemap_draw(std::vector<uint32_t> &dm, int L, int sx, int sy, int minx, int maxx, int miny, int maxy, int flags,
                         int clip_min_y, int clip_max_y) {
    const uint8_t cat = uint8_t(flags & CATEGORY_MASK);
    const bool opaque = (flags & DRAW_OPAQUE) != 0;
    const int x0 = std::max(minx, 0), x1 = std::min(maxx, W - 1);
    const int row_pixels = x1 - x0 + 1;
    if (row_pixels <= 0) return;

    if (sampled_layer_output_active()) {
        const auto &x_map = sampled_layer_front_ ? sampled_layer_hud_x_ : sampled_layer_x_;
        const int ox_begin = int(std::lower_bound(x_map.begin(), x_map.end(), x0) - x_map.begin());
        const int ox_end = int(std::lower_bound(x_map.begin(), x_map.end(), x1 + 1) - x_map.begin());
        const int y_begin = std::max(miny, clip_min_y);
        const int y_end = std::min(maxy + 1, clip_max_y);
        const int oy_begin = int(std::lower_bound(sampled_layer_y_.begin(), sampled_layer_y_.end(), y_begin) - sampled_layer_y_.begin());
        const int oy_end = int(std::lower_bound(sampled_layer_y_.begin(), sampled_layer_y_.end(), y_end) - sampled_layer_y_.begin());
        if(tile_span_capture_) emit_tile_span(L,(opaque?3:1)+cat,ox_begin,oy_begin,ox_end,oy_end,sx,sy);
        if(tile_span_output_) return;
        for (int oy = oy_begin; oy < oy_end; ++oy) {
            const int y = sampled_layer_y_[oy];
            uint32_t *dst = &dm[size_t(oy) * sampled_layer_width_];
            for (int ox = ox_begin; ox < ox_end; ++ox) {
                const int x = x_map[ox];
                const size_t source = tile_pixel_index(x + sx, y + sy);
                const uint16_t pixel = DAYTONA_TILE_PIXEL(L, source);
                if (cached_tilemap_pixel(pixel, cat, opaque)) dst[ox] = pens_[cached_pen(pixel)];
            }
        }
        return;
    }

    // Scroll is constant across this tilemap_draw call. Resolve Y once per row
    // and split the source row at its 512-pixel wrap boundary so the inner loop
    // can walk contiguous flags, pens, and destination spans.
    for (int y = std::max({miny, clip_min_y, 0}); y <= std::min({maxy, clip_max_y - 1, H - 1}); y++) {
        int source_x = (x0 + sx) & 511;
#if defined(DAYTONA_N64_LOW_MEMORY)
        uint32_t *dst = dm.data() + size_t(y) * W + size_t(x0);
        for (int x = 0; x < row_pixels; ++x) {
            const size_t source = tile_pixel_index(source_x + x, y + sy);
            const uint16_t pixel = DAYTONA_TILE_PIXEL(L, source);
            if (cached_tilemap_pixel(pixel, cat, opaque)) dst[x] = pens_[cached_pen(pixel)];
        }
#else
        const size_t source_row = size_t((y + sy) & 511) * 512;
        int remaining = row_pixels;
        size_t destination = size_t(y) * W + size_t(x0);
        while (remaining > 0) {
            const int count = std::min(remaining, 512 - source_x);
            size_t source = source_row + size_t(source_x);
            for (int x = 0; x < count; ++x, ++source, ++destination) {
                const uint16_t pixel = DAYTONA_TILE_PIXEL(L, source);
                if (cached_tilemap_pixel(pixel, cat, opaque)) dm[destination] = pens_[cached_pen(pixel)];
            }
            remaining -= count;
            source_x = 0;
        }
#endif
    }
}

// segaic24 draw_common for the rgb32 bitmap, cliprect = the whole screen.
void Video::draw(std::vector<uint32_t> &bitmap, int layer, int flags, int min_y, int max_y) {
    min_y = std::clamp(min_y, 0, H);
    max_y = std::clamp(max_y, min_y, H);
    if (min_y == max_y) return;
    uint16_t hscr = tile(0x5000 + uint32_t(layer >> 1));
    uint16_t vscr = tile(0x5004 + uint32_t(layer >> 1));
    const uint16_t ctrl = tile(0x5004 + uint32_t((layer >> 1) & 2));
    uint16_t mask[0x800];
    for (uint32_t i = 0; i < 0x800; i++) mask[i] = tile((layer & 4 ? 0x6800 : 0x6000) + i);
    const uint16_t tpri = uint16_t(layer & 1);
    layer >>= 1;
    const int fl = tpri | flags;

    if (vscr & 0x8000) return; // layer disable

    if (ctrl & 0x6000) { // special window/scroll modes
        if (layer & 1) return;
        const int sy = vscr & 0x1ff;
        if (hscr & 0x8000) {
            const uint32_t hscrtb = 0x4000 + 0x200 * uint32_t(layer);
            switch ((ctrl & 0x6000) >> 13) {
            case 1: {
                const uint16_t v = uint16_t((-vscr) & 0x1ff);
                if (!((-vscr) & 0x200)) layer ^= 1;
                for (int y = min_y; y < max_y; y++) {
                    const int l1 = y >= v ? layer ^ 1 : layer;
                    const uint16_t h = tile(hscrtb + uint32_t(y)) & 0x1ff;
                    tilemap_draw(bitmap, l1, -h, sy, 0, W - 1, y, y, fl, min_y, max_y);
                }
                break;
            }
            case 2:
            case 3:
                for (int y = min_y; y < max_y; y++) {
                    hscr = tile(hscrtb + uint32_t(y));
                    const int h = hscr & 0x1ff;
                    int l1 = layer;
                    if (!(hscr & 0x200)) l1 ^= 1;
                    tilemap_draw(bitmap, l1, -h, sy, 0, std::min(W - 1, h - 1), y, y, fl, min_y, max_y);
                    tilemap_draw(bitmap, l1 ^ 1, -h, sy, std::max(0, h), W - 1, y, y, fl, min_y, max_y);
                }
                break;
            }
        } else {
            const int sx = -(hscr & 0x1ff);
            switch ((ctrl & 0x6000) >> 13) {
            case 1: {
                const int v = (-vscr) & 0x1ff;
                if (!((-vscr) & 0x200)) layer ^= 1;
                tilemap_draw(bitmap, layer, sx, sy, 0, W - 1, 0, std::min(H - 1, v - 1), fl, min_y, max_y);
                tilemap_draw(bitmap, layer ^ 1, sx, sy, 0, W - 1, std::max(0, v), H - 1, fl, min_y, max_y);
                break;
            }
            case 2:
            case 3: {
                const int h = hscr & 0x1ff;
                if (!(hscr & 0x200)) layer ^= 1;
                tilemap_draw(bitmap, layer, sx, sy, 0, std::min(W - 1, h - 1), 0, H - 1, fl, min_y, max_y);
                tilemap_draw(bitmap, layer ^ 1, sx, sy, std::max(0, h), W - 1, 0, H - 1, fl, min_y, max_y);
                break;
            }
            }
        }
        return;
    }

    const int win = layer & 1;
    if (hscr & 0x8000) {
        const uint32_t hscrtb = 0x4000 + 0x200 * uint32_t(layer);
        vscr &= 0x1ff;
        vscr = uint16_t((vscr + min_y) & 0x1ff);
        for (int y = min_y; y < std::min(max_y, 384); y++) {
            hscr = uint16_t((-tile(hscrtb + uint32_t(y))) & 0x1ff);
            if (hscr + 496 <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, y, 496, y + 1, min_y, max_y);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, y, 512 - hscr, y + 1, min_y, max_y);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, y, 496, y + 1, min_y, max_y);
            }
            vscr = (vscr + 1) & 0x1ff;
        }
    } else {
        hscr = uint16_t((-hscr) & 0x1ff);
        vscr = uint16_t((+vscr) & 0x1ff);
        if (hscr + 496 <= 512) {
            if (vscr + 384 <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 496, 384, min_y, max_y);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 496, 512 - vscr, min_y, max_y);
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, 0, 0, 512 - vscr, 496, 384, min_y, max_y);
            }
        } else {
            if (vscr + 384 <= 512) {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 512 - hscr, 384, min_y, max_y);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, 0, 496, 384, min_y, max_y);
            } else {
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, vscr, 0, 0, 512 - hscr, 512 - vscr, min_y, max_y);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, vscr, 512 - hscr, 0, 496, 512 - vscr, min_y, max_y);
                draw_rect(bitmap, mask, tpri, flags, win, layer, hscr, 0, 0, 512 - vscr, 512 - hscr, 384, min_y, max_y);
                draw_rect(bitmap, mask, tpri, flags, win, layer, 0, 0, 512 - hscr, 512 - vscr, 496, 384, min_y, max_y);
            }
        }
    }
}

void Video::screen_update(const std::vector<GeoPoly> &polys, int windows, const VideoMem &mem, const std::vector<GeoPoly> *wide_polys, const std::vector<GeoPoly> *extended_polys) {
    using Clock = std::chrono::steady_clock;
    const auto profile_start = Clock::now();
    const bool sampled_layers = sampled_layer_output_active();
    // if the scroll colour table was written, refresh the palette (MAME never clears the flag)
    if (palette_dirty_) {
        // The same 32 component conversions are shared by all 4096 pens.
        // Rebuild on every sticky-dirty refresh, retaining native write semantics.
        std::array<uint32_t, 32> red, green, blue;
        for (uint32_t i = 0; i < 32; ++i) {
            red[i] = uint32_t(gamma_[uint8_t(le16(mem.colorxlat, 0x0040 + (i << 8)))]) << 16;
            green[i] = uint32_t(gamma_[uint8_t(le16(mem.colorxlat, 0x2040 + (i << 8)))]) << 8;
            blue[i] = gamma_[uint8_t(le16(mem.colorxlat, 0x4040 + (i << 8)))];
        }
        for (uint32_t i = 0; i < 0x1000; ++i) {
            const uint16_t value = le16(mem.palram, i);
            pens_[i] = 0xff000000u | red[value & 31] | green[(value >> 5) & 31] | blue[(value >> 10) & 31];
        }
    }

#if !defined(N64) || !defined(DAYTONA_N64_LOW_MEMORY)
    if (tile_cache_ && ram_write_tracking_) {
        auto &cache = *tile_cache_;
        const bool chars_changed = sync_dirty_characters();
        if (!cache.initialized || chars_changed) {
            for (int l = 0; l < 4; l++) build_layer(l);
        } else {
            for (size_t byte = 0; byte < tile_dirty_.size(); ++byte) {
                uint8_t bits = tile_dirty_[byte];
                while (bits) {
                    unsigned bit = 0;
                    while (!(bits & (1u << bit))) ++bit;
                    const uint32_t index = uint32_t(byte * 8 + bit);
                    build_tile(int(index >> 12), index & 0xfff);
                    bits &= uint8_t(bits - 1);
                }
            }
        }
        tile_dirty_.fill(0);
        cache.initialized = true;
    } else {
        if (tile_cache_) sync_tile_cache();
        for (int l = 0; l < 4; l++) build_layer(l);
        if (tile_cache_) tile_cache_->initialized = true;
    }
#endif
    if (!sampled_layers) {
        std::fill(screen_.begin(), screen_.end(), pens_[0]);
#if !defined(DAYTONA_N64_LOW_MEMORY)
        std::fill(sys24_.begin(), sys24_.end(), 0u);
#endif
    } else {
        std::fill(screen_.begin(), screen_.end(), pens_[0]);
    }
    const size_t bands = !tile_span_capture_ && render_workers_ ? render_workers_->size() : 1;
    auto draw_layers = [&](bool front, std::vector<uint32_t> &target) {
        sampled_layer_front_ = front;
        if(tile_span_capture_) tile_spans_[front?1:0].clear();
        auto draw_band = [&](size_t band) {
            const int min_y = int(band * H / bands);
            const int max_y = int((band + 1) * H / bands);
            if (front) {
                for (int layer = 3; layer >= 0; --layer) draw(target, (layer << 1) | 1, 0, min_y, max_y);
            } else {
                for (int layer = 3; layer >= 2; --layer) draw(target, layer << 1, DRAW_OPAQUE, min_y, max_y);
                for (int layer = 1; layer >= 0; --layer) draw(target, layer << 1, 0, min_y, max_y);
            }
        };
        if (bands > 1 && render_workers_) render_workers_->run(bands, draw_band);
        else draw_band(0);
    };
    const auto prepare_done = Clock::now();
    if (sampled_layers) {
        if (native_output_) draw_layers(false, screen_);
        else {
            if(!tile_span_output_) back_layer_.assign(size_t(sampled_layer_width_) * sampled_layer_height_, pens_[0]);
            draw_layers(false, back_layer_);
        }
    } else {
#if defined(DAYTONA_N64_LOW_MEMORY)
        draw_layers(false, screen_);
#else
        draw_layers(false, sys24_);
#endif
    }
    #if !defined(DAYTONA_N64_LOW_MEMORY)
    auto copy_trans = [&](const uint32_t *src, size_t stride) {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                if (const uint32_t p = src[size_t(y) * stride + size_t(x)]) screen_[size_t(y) * W + size_t(x)] = p;
    };
    #endif
    auto copy_raster = [&]() {
#if defined(DAYTONA_N64_LOW_MEMORY)
        const uint32_t *src = raster_.pixels();
        if (native_output_) {
            if (raster_.stride() == N64_W && raster_.height() == N64_H) {
                const size_t count = std::min(screen_.size(), size_t(raster_.stride()) * raster_.height());
                for (size_t i = 0; i < count; ++i) if (src[i]) screen_[i] = src[i];
            } else {
                for (int y = 0; y < N64_H; ++y) {
                    const size_t source_row = size_t(y) * raster_.height() / N64_H * raster_.stride();
                    for (int x = 0; x < N64_W; ++x)
                        if (const uint32_t pixel = src[source_row + size_t(x) * raster_.stride() / N64_W])
                            screen_[size_t(y) * N64_W + x] = pixel;
                }
            }
        } else {
            const int source_width = raster_.stride(), source_height = raster_.height();
            for (int y = 0; y < H; ++y) {
                const size_t source_row = size_t(y) * source_height / H * source_width;
                for (int x = 0; x < W; ++x)
                    if (const uint32_t p = src[source_row + size_t(x) * source_width / W])
                        screen_[size_t(y) * W + size_t(x)] = p;
            }
        }
#else
        copy_trans(raster_.pixels(), 512);
#endif
    };
    if (!sampled_layers) {
#if !defined(DAYTONA_N64_LOW_MEMORY)
        copy_trans(sys24_.data(), W);
#endif
        if (capture_presentation_) back_layer_ = screen_;
    }
    const auto rear_done = Clock::now();

    // render_polygons
    rendered_now_ = false;
    if (capture_presentation_) {
        if (render_done_) previous_polys_ = current_polys_;
        else {
            previous_polys_ = current_polys_;
            current_polys_ = extended_polys ? *extended_polys : widescreen_ && wide_polys ? *wide_polys : polys;
        }
    }
    if (presentation_only_) {
        rendered_now_ = !render_done_ && !polys.empty();
        render_done_ = true;
    } else if (render_done_) {
        copy_raster();
    } else if (!polys.empty()) {
        raster_.render(polys, windows, mem, crtc_x_, crtc_y_, render_x_, render_y_, 0, W - 1, 0, H - 1);
        copy_raster();
        render_done_ = true;
        rendered_now_ = true;
    }
    const auto polygon_done = Clock::now();

    if (sampled_layers) {
        if (native_output_) draw_layers(true, screen_);
        else {
            if(!tile_span_output_) front_layer_.assign(size_t(sampled_layer_width_) * sampled_layer_height_, 0u);
            draw_layers(true, front_layer_);
        }
    } else {
#if defined(DAYTONA_N64_LOW_MEMORY)
        draw_layers(true, screen_);
#else
        std::fill(sys24_.begin(), sys24_.end(), 0u);
        draw_layers(true, sys24_);
        copy_trans(sys24_.data(), W);
#endif
    }
    if (capture_presentation_) {
        ++presentation_revision_;
#if !defined(DAYTONA_N64_LOW_MEMORY)
        if (!sampled_layers) front_layer_.assign(sys24_.begin(), sys24_.begin() + W * H);
#endif
        presentation_mem_ = mem;
        presentation_windows_ = windows;
        presentation_crtc_x_ = crtc_x_; presentation_crtc_y_ = crtc_y_;
        presentation_render_x_ = render_x_; presentation_render_y_ = render_y_;
    }
    const auto front_done = Clock::now();
    const auto milliseconds = [](Clock::duration elapsed) {
        return std::chrono::duration<double, std::milli>(elapsed).count();
    };
    stage_times_.prepare_ms = milliseconds(prepare_done - profile_start);
    stage_times_.rear_layers_ms = milliseconds(rear_done - prepare_done);
    stage_times_.polygon_stage_ms = milliseconds(polygon_done - rear_done);
    stage_times_.front_layers_ms = milliseconds(front_done - polygon_done);
}

uint64_t Video::screen_hash() const {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (uint32_t px : screen_)
        for (int b = 0; b < 4; b++) {
            h ^= (px >> (8 * b)) & 0xff;
            h *= 0x100000001b3ULL;
        }
    return h;
}

} // namespace rt
