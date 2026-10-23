// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese, Matthew Daniels
//
// Model 2 geometrizer and the rasterizer's front end (culling, clipping,
// z-sort keys): the display list, as native C++. The original Model 2's
// geometrizer runs code from ROM inside its DSP that nobody has dumped, so
// this is MAME's high-level implementation (src/mame/sega/model2_v.cpp at
// dddd73680656e355bb2b5beecab1167c9f07bf81, BSD-3-Clause; notice above kept
// as the licence requires), transplanted: geo_parse and its commands,
// model2_3d_push, model2_3d_process_polygon. Memory is read through
// bounds-checked cursors, so a display list that runs off its buffer is a
// hard error instead of MAME's undefined behaviour. See THIRD_PARTY.md.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace rt {

struct GeoFatal : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct GeoVertex {
    float x = 0, y = 0;
    float p[3] = {0, 0, 0}; // pz, pu, pv
};

// One polygon kept for rendering (after culling and clipping), as MAME's
// model2_3d_process_polygon adds it to its list.
struct GeoPoly {
    uint16_t z = 0;
    uint16_t texheader[4] = {0, 0, 0, 0};
    uint8_t luma = 0;
    int32_t texlod = 0;
    int16_t viewport[4] = {0, 0, 0, 0};
    int16_t center[2] = {0, 0};
    uint8_t window = 0;
    uint32_t reverse = 0;
    uint8_t num_vertices = 0;
    GeoVertex v[8];
};

// Cursors standing in for MAME's raw u32/u16 pointers. Reads run on from a
// masked start, so a start near the end runs off it (time attack does, in
// polygon RAM and in texture memory). MAME reads past its array there; we
// wrap, as the hardware's address counter would. Sizes are powers of 2.
using GeoRomRead = bool (*)(void *context, size_t byte_offset, void *destination, size_t length);

struct GeoPtr {
    uint32_t *base = nullptr;
    uint32_t size = 0, i = 0;
    GeoRomRead read = nullptr;
    void *context = nullptr;
    bool null() const { return base == nullptr && read == nullptr; }
    uint32_t at() const {
        if (!size) throw GeoFatal("geometrizer read from missing memory");
        const uint32_t index = i & (size - 1);
        if (base) return base[index];
        if (!read) throw GeoFatal("geometrizer read from missing memory");
        uint32_t value = 0;
        if (!read(context, size_t(index) * sizeof(value), &value, sizeof(value)))
            throw GeoFatal("geometrizer failed to read cartridge ROM");
        return value;
    }
    uint32_t operator*() const { return at(); }
    void store(uint32_t value) const {
        if (!base) throw GeoFatal("geometrizer write to read-only ROM");
        base[i & (size - 1)] = value;
    }
    GeoPtr operator++(int) { GeoPtr t = *this; ++i; return t; }
    GeoPtr &operator+=(uint32_t n) { i += n; return *this; }
};
struct GeoPtr16 {
    const uint16_t *base = nullptr;
    uint32_t size = 0, i = 0;
    GeoRomRead read = nullptr;
    void *context = nullptr;
    uint16_t operator*() const {
        if (!size) throw GeoFatal("rasterizer read from missing texture memory");
        const uint32_t index = i & (size - 1);
        if (base) return base[index];
        if (!read) throw GeoFatal("rasterizer read from missing texture memory");
        uint16_t value = 0;
        if (!read(context, size_t(index) * sizeof(value), &value, sizeof(value)))
            throw GeoFatal("rasterizer failed to read cartridge texture ROM");
        return value;
    }
    GeoPtr16 operator++(int) { GeoPtr16 t = *this; ++i; return t; }
};

class Geo {
public:
    // polygons: the model ROM (0x1000000 bytes); textures: the texture ROM
    // (0x1000000 bytes); buffer: buffer RAM (0x8000 dwords), which the
    // display list is read from.
    Geo(const std::vector<uint8_t> &polygons, const std::vector<uint8_t> &textures, uint32_t *buffer);

    // Compact hosts can release the byte images after making typed ROM storage.
    Geo(std::vector<uint8_t> &polygons, std::vector<uint8_t> &textures, uint32_t *buffer, bool consume);
    // Read-only ROM banks can be mapped directly from cartridge storage on
    // constrained systems. The caller retains ownership for the lifetime of
    // Geo; words must already use the host's native byte order.
    Geo(const uint32_t *polygons, size_t polygon_words, const uint16_t *textures, size_t texture_words,
        uint32_t *buffer);
    Geo(GeoRomRead polygon_read, void *polygon_context, size_t polygon_words,
        GeoRomRead texture_read, void *texture_context, size_t texture_words, uint32_t *buffer);

    // MAME screen_vblank -> geo_parse: walk this frame's display list from
    // read_start (the geometrizer's read-start register).
    void parse(uint32_t read_start);
#if defined(__3DS__)
    void parse_snapshot(uint32_t read_start, uint32_t *commands);
#endif
    void set_render_distance(float depth);
    void set_capture_reference_depth(bool enabled) { capture_reference_depth_ = enabled; }
    uint64_t render_distance_culled() const { return render_distance_culled_; }
    float render_reference_depth() const { return render_reference_depth_; }

    // This frame's output.
    std::vector<GeoPoly> polys;       // kept polygons, in the order added
#if defined(__3DS__)
    // Stage-one experiment: only the presentation pass uses hardware clipping.
    // Native polygons/count/depth and all command side effects stay unchanged.
    bool lazy_native_count = false, verify_lazy_native = false;
    bool lazy_native_allowed = false; // Board disables this for presentation replays.
    uint32_t native_polygon_count();
    bool lazy_native_active() const { return lazy_native_active_; }
    uint64_t native_count_computations = 0;
    size_t native_candidate_count() const { return native_candidates_.size(); }
    size_t native_candidate_bytes() const {
        return native_candidates_.capacity()*sizeof(NativeCandidate) +
            native_planes_.capacity()*sizeof(std::array<plane,4>);
    }
    bool gpu_presentation_clip = false;
    uint32_t gpu_clip_submitted = 0, gpu_clip_fallback = 0;
    bool fast_clip_accept = false; // Experimental; retains native feedback.
    bool verify_fast_clip = false; // Oracle comparison; disable for benchmarks.
#endif
    bool record_unclipped = false; // opt-in analysis only
    bool record_vertical_clip = false; // analysis: retain vertical projection validity
    std::vector<GeoPoly> unclipped_polys; // culled, before viewport clipping; never rendered
    std::vector<uint32_t> pushed;     // every word handed to the rasterizer
    bool record_pushes = false;
    struct ObjectSubmission {
        uint32_t address, count, texture_points, texture_headers;
        float matrix[12];
    };
    bool record_objects = false; // opt-in analysis; never changes game-visible output
    std::vector<ObjectSubmission> object_submissions;
    std::vector<uint32_t> track_reference_models;
    std::vector<ObjectSubmission> extra_track_objects;
    std::vector<GeoPoly> extended_polys;
    void extend_track(bool widescreen);
    struct ModelOverride { uint32_t model, points, headers, replacement, replacement_points, replacement_headers; };
    std::vector<ModelOverride> model_overrides;
    void parse_model_presentation(uint32_t read_start, bool track, bool widescreen);
    uint32_t overridden_objects = 0;
    bool capture_widescreen = false;
    std::vector<GeoPoly> wide_polys; // presentation only; never exposed to the game

    struct plane {
        GeoVertex normal;
        float distance = 0;
    };
    struct texture_parameter {
        float diffuse = 0, ambient = 0;
        uint32_t specular_control = 0;
        float specular_scale = 0;
    };
    struct raster_state {
        const uint16_t *texture_rom = nullptr;
        GeoRomRead texture_read = nullptr;
        void *texture_context = nullptr;
        uint32_t texture_rom_mask = 0;
        int16_t viewport[4] = {0, 0, 0, 0};
        int16_t center[4][2] = {{0, 0}, {0, 0}, {0, 0}, {0, 0}};
        uint16_t center_sel = 0;
        uint32_t reverse = 0;
        int32_t z_adjust = 0;
        float polygon_z = 0;
        uint8_t master_z_clip = 0;
        uint32_t cur_command = 0;
        uint32_t command_buffer[32] = {};
        uint32_t command_index = 0;
        uint32_t poly_list_index = 0;
        uint16_t min_z = 0, max_z = 0;
        uint16_t texture_ram[0x10000] = {};
        uint8_t log_ram[0x8000] = {};
        uint8_t cur_window = 0;
        plane clip_plane[4][4];
#if defined(__3DS__)
        // Presentation-only values; copied with raster snapshots, never board RAM.
        struct WideClipCache {
            float distance = 0, normal_x = 0, normal_z = 0;
            bool valid = false;
        } wide_clip_cache[4][2];
#endif
    };
    struct geo_state {
        raster_state *raster = nullptr;
        uint32_t mode = 0;
        const uint32_t *polygon_rom = nullptr;
        GeoRomRead polygon_read = nullptr;
        void *polygon_context = nullptr;
        uint32_t polygon_rom_mask = 0;
        float matrix[12] = {};
        GeoVertex focus, light;
        float lod = 0;
        float coef_table[32] = {};
        texture_parameter texture_parameters[32];
        uint32_t polygon_ram0[0x8000] = {};
        uint32_t polygon_ram1[0x8000] = {};
    };

    int windows() const { return raster_.cur_window; } // after parse: the last window used
    void zclip_w(uint32_t data) { raster_.master_z_clip = uint8_t(data); } // model2_3d_zclip_w

private:
    bool applying_model_overrides_ = false;
#if defined(__3DS__)
    struct NativeCandidate {
        GeoVertex vertices[4];
        uint32_t planes = 0;
        uint16_t z = 0;
        uint8_t count = 0;
    };
    std::vector<NativeCandidate> native_candidates_;
    std::vector<std::array<plane,4>> native_planes_;
    bool lazy_native_active_ = false, native_count_valid_ = false;
    uint32_t native_count_ = 0;
    uint16_t native_min_z_ = 0xffff, native_max_z_ = 0;
    void retain_native_candidate(const GeoVertex *vertices, unsigned count, uint16_t z, const plane *planes);
#endif
#if defined(DAYTONA_N64_LOW_MEMORY)
    // The native N64 path is 4:3 with no stereo convergence controls.
    bool capture_reference_depth_ = false;
#else
    bool capture_reference_depth_ = true;
#endif
    float render_distance_ = 0;
    uint64_t render_distance_culled_ = 0;
    float render_reference_depth_ = 0;
    std::vector<float> render_reference_depths_;
    std::vector<uint32_t> polygon_rom_;
    std::vector<uint16_t> texture_rom_;
    const uint32_t *polygon_rom_data_ = nullptr;
    const uint16_t *texture_rom_data_ = nullptr;
    GeoRomRead polygon_rom_read_ = nullptr, texture_rom_read_ = nullptr;
    void *polygon_rom_context_ = nullptr, *texture_rom_context_ = nullptr;
    size_t polygon_rom_words_ = 0, texture_rom_words_ = 0;
    uint32_t *buffer_;
    void bind_roms();
    raster_state raster_;
    geo_state geo_;
    std::unique_ptr<geo_state> track_geo_;
    std::unique_ptr<raster_state> track_raster_;

    GeoPtr buf(uint32_t i) { return GeoPtr{buffer_, 0x8000, i}; }
    uint16_t float_to_zval(float floatval, int32_t z_adjust);
    bool check_culling(raster_state *raster, uint32_t attr, float min_z, float max_z);
    template <unsigned NumVerts> void model2_3d_process_polygon(raster_state *raster, uint32_t attr);
    void model2_3d_push(raster_state *raster, uint32_t input);
    void model2_3d_push_point(raster_state *raster, const GeoVertex &point);
      void model2_3d_push_lit_point(raster_state *raster, uint32_t luma, uint32_t distance,
                                   const GeoVertex &point);
      bool polygon_beyond_render_distance(const raster_state *raster, const GeoVertex &third,
                                          const GeoVertex *fourth) const;
    void render_frame_start();
    void geo_parse_np_ns(geo_state *geo, GeoPtr input, uint32_t count);
    void geo_parse_np_s(geo_state *geo, GeoPtr input, uint32_t count);
    void geo_parse_nn_ns(geo_state *geo, GeoPtr input, uint32_t count);
    void geo_parse_nn_s(geo_state *geo, GeoPtr input, uint32_t count);
    GeoPtr geo_nop(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_object_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_direct_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_window_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_texture_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_polygon_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_texture_parameters(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_mode(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_zsort_mode(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_focal_distance(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_light_source(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_matrix_write(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_translate_write(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_data_mem_push(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_test(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_end(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_dummy(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_log_data(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_lod(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_code_upload(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_code_jump(geo_state *geo, uint32_t opcode, GeoPtr input);
    GeoPtr geo_process_command(geo_state *geo, uint32_t opcode, GeoPtr input, bool *end_code);
};

} // namespace rt
