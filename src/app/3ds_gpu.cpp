#include <3ds.h>
#include <citro3d.h>
extern "C" void Daytona_DrawArraysPacked(GPU_Primitive_t primitive, int first, int size);
extern "C" void Daytona_DrawTrianglesPacked(int count, const void *indices);
extern "C" void Daytona_SetFastUniformChecks(bool enabled);
extern "C" void Daytona_SetPackedTextures(bool enabled, bool verify);
extern "C" void Daytona_ClearAuxTextures(void);
extern "C" void Daytona_FrameEndFlushed(void);
extern "C" void Daytona_FlushGpuContext(void);
extern "C" bool Daytona_ReplayStereoCommands(const u32*,u32,unsigned);
#undef BIT
#include "app/3ds_gpu.h"
#include "app/3ds_gpu_shader.h"
#include "app/3ds_stereo.h"
#include "app/3ds_opaque_combiner.h"
#include "app/3ds_texture_pool_index.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <iterator>
#include <vector>
#include <stdexcept>
#include <tuple>

namespace app {
namespace {
// Preserve batch boundaries while packed colors save 96 KiB of linear memory.
constexpr unsigned VertexCapacity = 8192;
constexpr size_t CacheBudget = 4u * 1024u * 1024u;
constexpr u32 TransferFlags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
    GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
struct Vertex { float x, y, z, w, u, v; u8 r, g, b, a; };
static_assert(sizeof(Vertex)==28);
struct TickScope {
    u64 *sum, start;
    TickScope(bool enabled,u64 &destination):sum(enabled?&destination:nullptr),start(enabled?svcGetSystemTick():0){}
    ~TickScope(){if(sum)*sum+=svcGetSystemTick()-start;}
};
unsigned morton(unsigned x, unsigned y, unsigned width) {
    const unsigned lo = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) |
        ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
    return (((y >> 3) * (width >> 3) + (x >> 3)) << 6) + lo;
}
u16 le16(const uint8_t *p, unsigned i) { return u16(p[i*2] | (p[i*2+1] << 8)); }
unsigned gamma(unsigned value) { return value > 64 ? (value - 64) * 255 / 191 : 0; }
u32 color(const rt::VideoMem &m, unsigned base, unsigned luma, unsigned alpha = 255) {
    const unsigned r = gamma(le16(m.colorxlat, (((base >> 0) & 31) << 8) + luma) & 255);
    const unsigned g = gamma(le16(m.colorxlat, 0x2000 + (((base >> 5) & 31) << 8) + luma) & 255);
    const unsigned b = gamma(le16(m.colorxlat, 0x4000 + (((base >> 10) & 31) << 8) + luma) & 255);
    return (r << 24) | (g << 16) | (b << 8) | alpha;
}
unsigned texel(const uint32_t *sheet, unsigned bx, unsigned by, unsigned x, unsigned y) {
    unsigned xx = bx + x, yy = by + y;
    if (xx >= 1024) { xx -= 1024; yy ^= 1024; }
    const unsigned offset = ((yy / 2) * 512) + (xx / 2);
    u32 t = sheet[(offset >> 1) & 0x7ffff];
    if (offset & 1) t >>= 16;
    if (!(y & 1)) t >>= 8;
    if (!(x & 1)) t >>= 4;
    return t & 15;
}
void init_texture(C3D_Tex &t, unsigned w, unsigned h) {
    if (!C3D_TexInit(&t, u16(w), u16(h), GPU_RGBA8))
        throw std::runtime_error("3DS GPU texture memory is full");
    C3D_TexSetFilter(&t, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&t, GPU_REPEAT, GPU_REPEAT);
}
}
struct ThreeDsGpu::Impl {
    struct CachedTexture {
        uint64_t header;
        unsigned level;
        bool opaque=true;
        std::array<u32, 16> palette;
        C3D_Tex tex{};
        uint64_t used;
        u32 lookup_hash = 0;
        CachedTexture *next_hash = nullptr;
        uint64_t allocation_serial = 0;
    };
    C3D_RenderTarget *target[2]{}, *geometry_target[2]{};
    C3D_Tex geometry_texture[2]{};
    float scale=1;
    shaderProgram_s program{};
    DVLB_s *binary = nullptr;
    Vertex *vertices = nullptr;
    static constexpr unsigned IndexCapacity=3*VertexCapacity;
    u16 *group_indices=nullptr;
    unsigned index_cursor=0, grouped_polygons=0, grouped_calls=0, opaque_group_polygons=0, opaque_selector_mask=0, opaque_mixed_groups=0;
    bool adjacent_groups=false, three_textures=false, solid_color_groups=false, opaque_color_groups=false;
    static constexpr unsigned SolidPaletteSide=64;
    C3D_Tex solid_palette{};
    unsigned solid_palette_cursor=0;
    C3D_Tex layers[2]{}, tile_maps[4]{};
    struct TileStorage {
        std::array<uint64_t,16384> identities;
        std::array<uint32_t,16384> palettes{};
        std::array<uint32_t,4096> colors{};
        std::array<uint32_t,256> palette_versions{};
        TileStorage(){identities.fill(UINT64_MAX);}
    };
    std::unique_ptr<TileStorage> tiles;
    u8 *tile_staging=nullptr;
    static constexpr unsigned TileStagingBytes=512u*1024u;
    bool tile_mode=false, raw_projection=false;
    bool stereo_chunks=false, stereo_recording=false, shader_stereo=false, packed_draw=false;
    bool hud_stereo=false, replay_stereo=false, packed_elements=false;
    uint64_t replayed_chunks=0, replayed_words=0;
    unsigned packed_element_draws=0;
    int stereo_uniform=-1;
    float pair_eye=0;
    struct StereoDraw {
        C3D_Tex *texture=nullptr;
        unsigned first[2]{}, count=0;
        bool screen_space=false, opaque=false;
        std::array<u32,4> scissor{};
    };
    static constexpr unsigned StereoDrawCapacity=128;
    std::array<StereoDraw,StereoDrawCapacity> stereo_draws{};
    unsigned stereo_batch_limit=64;
    unsigned stereo_count=0;
    float raw_scale_x=1, raw_scale_y=1, raw_offset_x=0, raw_eye=0, raw_focus=1;
    int raw_crtc_x=0, raw_crtc_y=0;
    rt::Raster projection{400, 240};
    std::list<CachedTexture> cache;
    using CacheIterator=std::list<CachedTexture>::iterator;
    struct EvictionEntry { CacheIterator entry; uint64_t used; };
    std::vector<EvictionEntry> eviction_heap;
    uint64_t allocation_serial=0;
    bool indexed_eviction=false,verify_eviction=false,eviction_heap_valid=false;
    bool canonical_texture_key=false, quarter_textures=false;
    unsigned eviction_checks=0,eviction_refreshes=0,eviction_builds=0;
    static bool later_eviction(const EvictionEntry &a,const EvictionEntry &b) {
        return a.used!=b.used ? a.used>b.used :
            a.entry->allocation_serial>b.entry->allocation_serial;
    }
    void invalidate_eviction_heap() { eviction_heap.clear();eviction_heap_valid=false; }
    CacheIterator scan_oldest() {
        auto oldest=cache.end();
        for(auto i=cache.begin();i!=cache.end();++i)
            if(i->used!=texture_epoch && (oldest==cache.end() || i->used<oldest->used)) oldest=i;
        if(timing_active) eviction_probes+=cache.size();
        return oldest;
    }
    CacheIterator indexed_oldest() {
        if(!eviction_heap_valid) {
            eviction_heap.clear();
            for(auto i=cache.begin();i!=cache.end();++i) eviction_heap.push_back({i,i->used});
            std::make_heap(eviction_heap.begin(),eviction_heap.end(),later_eviction);
            eviction_heap_valid=true;
            ++eviction_builds;
        }
        while(!eviction_heap.empty()) {
            const auto first=eviction_heap.front();
            // Usage epochs only increase. A stale key is a lower bound: refresh
            // roots until the minimum is current, preserving the old scan's ties.
            if(first.used!=first.entry->used) {
                std::pop_heap(eviction_heap.begin(),eviction_heap.end(),later_eviction);
                eviction_heap.back().used=first.entry->used;
                std::push_heap(eviction_heap.begin(),eviction_heap.end(),later_eviction);
                ++eviction_refreshes;
                continue;
            }
            if(first.used==texture_epoch) return cache.end(); // All entries pinned.
            std::pop_heap(eviction_heap.begin(),eviction_heap.end(),later_eviction);
            eviction_heap.pop_back(); // Remove iterator before its texture is erased.
            return first.entry;
        }
        return cache.end();
    }
    std::array<CachedTexture*,1024> cache_buckets{};
    struct FastMaterial {
        uint64_t header=0;
        CachedTexture *texture=nullptr;
        uint8_t luma=0;
    };
    std::array<FastMaterial,256> fast_materials{};
    unsigned material_lookups=0, material_hits=0;
    int combiner_mode=-1;
    C3D_Tex *bound_texture=nullptr;
    std::array<C3D_Tex*,2> bound_aux{};
    unsigned state_calls=0, combiner_updates=0, texture_binds=0;
    bool scissor_valid=false;
    std::array<u32,5> last_scissor{};
    unsigned scissor_calls=0, scissor_updates=0;
    void invalidate_draw_state() {
        combiner_mode=-1;bound_texture=nullptr;scissor_valid=false;
        if(three_textures) {Daytona_ClearAuxTextures();bound_aux.fill(nullptr);}
    }
    void clear_fast_materials() {
        for(auto &entry:fast_materials) entry.texture=nullptr;
    }
    size_t cache_bytes = 0;
    u8 *texture_pool = nullptr;
    static constexpr size_t PoolUnit = 128;
    std::array<u8, CacheBudget / PoolUnit> pool_used{};
    std::unique_ptr<ThreeDsTexturePoolIndex> pool_index;
    bool verify_pool_index=false;
    uint64_t frame = 0, revision = UINT64_MAX, texture_epoch = 0;
    unsigned cursor = 0;
    int output_width = 400, layer_width = 400, layer_height = 240;
    bool half = true;
    bool initialized = false, program_initialized = false;
    const rt::VideoMem *memory = nullptr;
    Stats counters;
    bool trace_coverage = false, detailed_profile = false, timing_active = false, benchmark_mode = false;
    u64 material_ticks = 0, decode_ticks = 0, batch_ticks = 0, draw_ticks = 0;
    u64 pool_ticks=0, eviction_ticks=0, pool_probes=0, eviction_probes=0;
    unsigned pool_queries=0, pool_failures=0, evictions=0;
    unsigned coverage_frame = 210;
    FILE *coverage = nullptr;
    Impl() {
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-raw-projection.txt","r")){raw_projection=true;std::fclose(flag);}
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-stereo-chunks.txt","r")){stereo_chunks=raw_projection;std::fclose(flag);}
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-shader-stereo.txt","r")){shader_stereo=stereo_chunks;std::fclose(flag);}
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-stereo-batch128.txt","r")){if(stereo_chunks)stereo_batch_limit=128;std::fclose(flag);}
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-packed-draw.txt","r")){packed_draw=true;std::fclose(flag);}
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-packed-elements.txt","r")){packed_elements=true;std::fclose(flag);}
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-hud-stereo.txt","r")){hud_stereo=shader_stereo;std::fclose(flag);}
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-replay-stereo-commands.txt","r")){replay_stereo=shader_stereo;std::fclose(flag);}
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-adjacent-groups.txt","r")) {
            adjacent_groups=shader_stereo;std::fclose(flag);
        }
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-three-texture-groups.txt","r")) {
            int mode=1;std::fscanf(flag,"%d",&mode);std::fclose(flag);
            three_textures=shader_stereo;
            solid_color_groups=three_textures && mode==2;
            opaque_color_groups=three_textures && mode==3;
        }
        bool packed_textures=false, verify_textures=false;
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-packed-textures.txt","r")){packed_textures=true;std::fclose(flag);}
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/verify-packed-textures.txt","r")){verify_textures=true;std::fclose(flag);}
        Daytona_SetPackedTextures(packed_textures,verify_textures);
        Daytona_SetFastUniformChecks(false);
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-fast-uniforms.txt","r")){Daytona_SetFastUniformChecks(true);std::fclose(flag);}
        projection.set_raw_polygon_input(raw_projection);
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-borrow-polygons.txt","r")){projection.set_borrow_raw_polygons(true);std::fclose(flag);}
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-tile-layers.txt","r")){tile_mode=true;std::fclose(flag);}
        if (FILE *flag = std::fopen("sdmc:/3ds/daytona/trace-texture-coverage.txt", "r")) {
            trace_coverage = true; std::fclose(flag);
        }
        if (FILE *flag = std::fopen("sdmc:/3ds/daytona/trace-texture-frame.txt", "r")) {
            unsigned requested=0;
            if(std::fscanf(flag,"%u",&requested)==1 && requested>=1 && requested<=100000)
                coverage_frame=requested; // Presented GPU frame, not simulation frame.
            std::fclose(flag);
        }
        if (FILE *flag = std::fopen("sdmc:/3ds/daytona/detailed-gpu-profile.txt", "r")) {
            detailed_profile = true; std::fclose(flag);
        }
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/benchmark.txt","r")){benchmark_mode=true;detailed_profile=false;trace_coverage=false;std::fclose(flag);}
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-indexed-pool.txt","r")) {
            pool_index=std::make_unique<ThreeDsTexturePoolIndex>();std::fclose(flag);
        }
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/verify-indexed-pool.txt","r")) {
            verify_pool_index=pool_index && !benchmark_mode;std::fclose(flag);
        }
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-quarter-textures.txt","r")) {
            quarter_textures=true;std::fclose(flag);
        }
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-canonical-texture-key.txt","r")) {
            canonical_texture_key=true;std::fclose(flag);
        }
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/gpu-indexed-eviction.txt","r")) {
            indexed_eviction=true;std::fclose(flag);
        }
        if(FILE *flag=std::fopen("sdmc:/3ds/daytona/verify-indexed-eviction.txt","r")) {
            verify_eviction=indexed_eviction && !benchmark_mode;std::fclose(flag);
        }
        try {
            if (!C3D_Init(512u * 1024u)) throw std::runtime_error("Cannot initialize 3DS GPU");
            initialized = true;
            binary = DVLB_ParseFile(reinterpret_cast<u32*>(daytona_gpu_shader), sizeof(daytona_gpu_shader));
            if (!binary) throw std::runtime_error("Cannot load 3DS GPU shader");
            shaderProgramInit(&program); program_initialized = true;
            if (R_FAILED(shaderProgramSetVsh(&program, &binary->DVLE[0])))
                throw std::runtime_error("Cannot bind 3DS GPU shader");
            stereo_uniform=shaderInstanceGetUniformLocation(program.vertexShader,"stereo_shift");
            if(stereo_uniform<0) throw std::runtime_error("Missing stereo shader uniform");
            for (int i=0; i<2; ++i) {
                target[i] = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, -1);
                if (!target[i]) throw std::runtime_error("Cannot allocate 3DS screen target");
                C3D_RenderTargetSetOutput(target[i], GFX_TOP, i ? GFX_RIGHT : GFX_LEFT, TransferFlags);
                if (!C3D_TexInitVRAM(&geometry_texture[i],256,512,GPU_RGBA8))
                    throw std::runtime_error("Cannot allocate GPU geometry texture");
                C3D_TexSetFilter(&geometry_texture[i],GPU_NEAREST,GPU_NEAREST);
                C3D_TexSetWrap(&geometry_texture[i],GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
                geometry_target[i]=C3D_RenderTargetCreateFromTex(&geometry_texture[i],GPU_TEXFACE_2D,0,-1);
                if(!geometry_target[i]) throw std::runtime_error("Cannot allocate GPU geometry target");
                if(!tile_mode) {
                    init_texture(layers[i], 512, 256);
                    C3D_TexSetWrap(&layers[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
                    C3D_TexSetFilter(&layers[i], GPU_NEAREST, GPU_NEAREST);
                }
            }
            vertices = static_cast<Vertex*>(linearAlloc(VertexCapacity * sizeof(Vertex)));
            if (!vertices) throw std::runtime_error("Cannot allocate 3DS GPU vertices");
            if(adjacent_groups || three_textures) {
                group_indices=static_cast<u16*>(linearAlloc(IndexCapacity*sizeof(u16)));
                if(!group_indices) throw std::runtime_error("Cannot allocate GPU grouped indices");
            }
            if(solid_color_groups) {
                init_texture(solid_palette,SolidPaletteSide,SolidPaletteSide);
                C3D_TexSetFilter(&solid_palette,GPU_NEAREST,GPU_NEAREST);
                C3D_TexSetWrap(&solid_palette,GPU_CLAMP_TO_EDGE,GPU_CLAMP_TO_EDGE);
                std::fill_n(static_cast<u32*>(solid_palette.data),SolidPaletteSide*SolidPaletteSide,0xffffffffu);
                C3D_TexFlush(&solid_palette);
            }
            texture_pool = static_cast<u8*>(linearAlloc(CacheBudget));
            if (!texture_pool) throw std::runtime_error("Cannot allocate bounded GPU texture pool");
            if(tile_mode) {
                tiles=std::make_unique<TileStorage>();
                tile_staging=static_cast<u8*>(linearAlloc(TileStagingBytes));
                if(!tile_staging) throw std::runtime_error("Cannot allocate GPU tile staging");
                for(auto &tex:tile_maps) {
                    if(!C3D_TexInitVRAM(&tex,512,512,GPU_RGBA8)) throw std::runtime_error("Cannot allocate GPU tilemap VRAM");
                    C3D_TexSetFilter(&tex,GPU_NEAREST,GPU_NEAREST);
                    C3D_TexSetWrap(&tex,GPU_REPEAT,GPU_REPEAT);
                }
            }
            if(FILE *f=std::fopen("sdmc:/3ds/daytona/gpu-memory.log","w")) {
                std::fprintf(f,"texture_pool=%lu linear_free=%lu vram_free=%lu tile_mode=%d indexed=%d verify_indexed=%d indexed_eviction=%d verify_eviction=%d canonical_key=%d quarter_textures=%d\n",
                    (unsigned long)CacheBudget,(unsigned long)linearSpaceFree(),
                    (unsigned long)vramSpaceFree(),tile_mode,bool(pool_index),verify_pool_index,indexed_eviction,verify_eviction,canonical_texture_key,quarter_textures);
                std::fprintf(f,"stereo_batch_limit=%u split_command_flush=1 three_texture_groups=%d solid_color_groups=%d solid_palette_bytes=%u\n",stereo_batch_limit,three_textures,solid_color_groups,solid_color_groups?SolidPaletteSide*SolidPaletteSide*4:0u);
                std::fprintf(f,"stereo_command_replay=%d\n",replay_stereo);
                std::fprintf(f,"packed_elements=%d\n",packed_elements);
                std::fclose(f);
            }
            projection.release_pixel_storage();
            projection.set_depth_buffer(true); // Provide inverse depth for every material.
            projection.set_polygon_sink([this](int r, const rt::GeoPoly &p, const int *clip, const rt::Raster::Extra &m) {
                polygon(r, p, clip, m);
            });
        } catch (...) { release(); throw; }
    }
    ~Impl() { release(); }
    void release() {
        if (coverage) { std::fclose(coverage); coverage = nullptr; }
        if (initialized) { C3D_FrameEnd(0); C3D_FrameBegin(0); C3D_FrameEnd(0); }
        // Cached texture descriptors borrow ranges of this single allocation.
        if (texture_pool) { linearFree(texture_pool); texture_pool = nullptr; }
        clear_fast_materials();invalidate_draw_state();invalidate_eviction_heap();cache.clear(); cache_buckets.fill(nullptr);
        if (vertices) { linearFree(vertices); vertices = nullptr; }
        if(group_indices){linearFree(group_indices);group_indices=nullptr;}
        if(solid_palette.data) C3D_TexDelete(&solid_palette);
        if(tile_staging){linearFree(tile_staging);tile_staging=nullptr;}
        for(auto &tex:tile_maps) if(tex.data) C3D_TexDelete(&tex);
        tiles.reset();
        for (int i=0;i<2;++i) {
            if (geometry_target[i]) { C3D_RenderTargetDelete(geometry_target[i]);geometry_target[i]=nullptr; }
            if (geometry_texture[i].data) C3D_TexDelete(&geometry_texture[i]);
            if (layers[i].data) C3D_TexDelete(&layers[i]);
            if (target[i]) { C3D_RenderTargetDelete(target[i]); target[i]=nullptr; }
        }
        if (program_initialized) { shaderProgramFree(&program); program_initialized=false; }
        if (binary) { DVLB_Free(binary); binary=nullptr; }
        if (initialized) { C3D_Fini(); initialized=false; }
    }
    void erase(std::list<CachedTexture>::iterator i,bool heap_entry_removed=false) {
        // Capture and texture revision resets erase arbitrary entries. Discard
        // all borrowed iterators before those paths can free their targets.
        if(!heap_entry_removed) invalidate_eviction_heap();
        CachedTexture **link=&cache_buckets[i->lookup_hash & (cache_buckets.size()-1)];
        while(*link && *link!=&*i) link=&(*link)->next_hash;
        if(*link) *link=i->next_hash;
        const size_t begin = (static_cast<u8*>(i->tex.data) - texture_pool) / PoolUnit;
        const size_t count = i->tex.size / PoolUnit;
        std::fill_n(pool_used.begin() + begin, count, u8(0));
        if(pool_index) pool_index->set_used(begin,count,false);
        cache_bytes -= i->tex.size; cache.erase(i);
    }
    size_t free_pool_block(size_t bytes) {
        TickScope timer(timing_active,pool_ticks);
        if(timing_active) ++pool_queries;
        const size_t count = bytes / PoolUnit;
        if(pool_index) {
            const size_t indexed=pool_index->find(count);
            if(verify_pool_index && indexed!=scan_pool_block(count))
                throw std::runtime_error("Indexed texture allocation differs from original first fit");
            if(timing_active && indexed==pool_used.size()) ++pool_failures;
            return indexed;
        }
        const size_t result=scan_pool_block(count);
        if(timing_active && result==pool_used.size()) ++pool_failures;
        return result;
    }
    size_t scan_pool_block(size_t count) {
        // Power-of-two dimensions give aligned buddy-sized ranges. All ranges
        // are multiples of the PICA texture alignment and cannot overlap.
        for (size_t base = 0; base + count <= pool_used.size(); base += count) {
            bool free = true;
            size_t inspected=0;
            for (; inspected < count; ++inspected)
                if (pool_used[base + inspected]) { free = false; break; }
            if(timing_active) pool_probes += inspected + (free ? 0 : 1);
            if (free) return base;
        }
        return pool_used.size();
    }
    void submit_batch() {
        TickScope timer(timing_active,batch_ticks);
        flush_stereo_draws();
        GSPGPU_FlushDataCache(vertices,cursor*sizeof(Vertex));
        if(index_cursor) GSPGPU_FlushDataCache(group_indices,index_cursor*sizeof(u16));
        if(solid_palette_cursor) C3D_TexFlush(&solid_palette);
        Daytona_FrameEndFlushed();
        counters.command_usage=std::max(counters.command_usage,C3D_GetCmdBufUsage());
        C3D_FrameBegin(0);
        counters.gpu_ms+=C3D_GetDrawingTime();
        // FrameBegin(0) waits for the completed queue before this memory is reused.
        cursor=0;index_cursor=0;solid_palette_cursor=0;
        ++counters.batches;
        clear_fast_materials();invalidate_draw_state(); // No fast pointer survives removal of batch pins.
        // Retain recency across completed batches; only the current epoch is pinned.
        ++texture_epoch;
    }
    void ensure_command_room(unsigned vertex_count=6) {
        if(vertex_count>VertexCapacity) throw std::runtime_error("3DS primitive exceeds GPU vertex capacity");
        u32 words=0, used=0;
        GPUCMD_GetBuffer(nullptr,&words,&used);
        // Reserve 16 KiB for pending state, this primitive and frame closure.
        // Citro3D splits reduce the remaining size when switching targets.
        const unsigned reserve=4096+(stereo_recording?4096+(stereo_count+1)*512:0);
        if(used>words || words-used<reserve || vertex_count>VertexCapacity-cursor) submit_batch();
    }
    C3D_Tex *texture(int renderer, const rt::Raster::Extra &m, const rt::GeoPoly &poly, bool *opaque=nullptr) {
        TickScope timer(timing_active,material_ticks);
        // Video memory and texture quality are stable throughout this render.
        // Header includes palette/luma bases, dimensions, wrapping and renderer.
        u32 fast_hash=u32(m.texture_key)^u32(m.texture_key>>32)^(u32(m.luma)*0x9e3779b9u);
        fast_hash^=fast_hash>>16;fast_hash*=0x7feb352du;fast_hash^=fast_hash>>15;
        auto &fast=fast_materials[fast_hash&(fast_materials.size()-1)];
        ++material_lookups;
        if(!coverage && fast.texture && fast.header==m.texture_key && fast.luma==m.luma) {
            ++material_hits;fast.texture->used=texture_epoch;
            if(opaque) *opaque=fast.texture->opaque;
            return &fast.texture->tex;
        }
        auto remember=[&](CachedTexture &texture) {
            if(opaque) *opaque=texture.opaque;
            if(!coverage) {fast.header=m.texture_key;fast.luma=m.luma;fast.texture=&texture;}
            return &texture.tex;
        };
        unsigned level = half ? (quarter_textures ? 2 : 1) : 0;
        while ((m.texwidth >> level) > 512 || (m.texheight >> level) > 512) ++level;
        const unsigned w = std::max<uint32_t>(8u, m.texwidth >> level), h = std::max<uint32_t>(8u, m.texheight >> level);
        std::array<u32, 16> palette;
        const unsigned base = le16(memory->palram, m.colorbase + 0x1000) & 0x7fff;
        for (unsigned i=0;i<16;++i) {
            const unsigned l = std::min(63u, unsigned(memory->lumaram[(m.lumabase+i*8)*4]) * m.luma / 256);
            palette[i] = color(*memory, base, l, renderer == 3 && i == 15 ? 0 : 255);
            // Bits discarded by RGBA5551 cannot distinguish stored textures.
            if(half) palette[i] &= 0xf8f8f8ffu;
        }
        auto trace = [&](bool hit) {
            if (!coverage) return;
            float u0=INFINITY, u1=-INFINITY, v0=INFINITY, v1=-INFINITY;
            for (unsigned i=0;i<poly.num_vertices;++i) {
                const auto &v=poly.v[i];
                const float u=(raw_projection?v.p[1]*.125f:v.p[1]/v.p[0])/m.texwidth*w;
                const float t=(raw_projection?v.p[2]*.125f:v.p[2]/v.p[0])/m.texheight*h;
                u0=std::min(u0,u);u1=std::max(u1,u);
                v0=std::min(v0,t);v1=std::max(v1,t);
            }
            uint64_t hash=14695981039346656037ull;
            for(u32 c:palette){hash^=c;hash*=1099511628211ull;}
            std::fprintf(coverage,"%llx,%llx,%u,%u,%u,%.5f,%.5f,%.5f,%.5f,%d\n",
                (unsigned long long)m.texture_key,(unsigned long long)hash,level,w,h,u0,u1,v0,v1,hit);
        };
        // Cached pixels depend on dimensions, source sheet/origin and the
        // resolved palette. Only mirror bits affect this descriptor's wrap.
        // Palette/luma addresses and unused arcade LOD bits do not distinguish
        // GPU images once the complete resolved palette is compared below.
        const uint64_t image_header = canonical_texture_key ?
            (m.texture_key & (uint64_t(0x033f) | (uint64_t(0x17ff) << 32))) :
            m.texture_key;
        const GPU_TEXCOLOR format = half ? GPU_RGBA5551 : GPU_RGBA8;
        u32 lookup_hash=2166136261u;
        auto mix=[&](u32 value){lookup_hash=(lookup_hash^value)*16777619u;};
        mix(u32(image_header));mix(u32(image_header>>32));mix(level);mix(u32(format));
        for(u32 c:palette) mix(c);
        // Mix high color bits into bucket bits after compact-palette masking.
        lookup_hash ^= lookup_hash >> 16;
        lookup_hash *= 0x7feb352du;
        lookup_hash ^= lookup_hash >> 15;
        const size_t bucket=lookup_hash & (cache_buckets.size()-1);
        for (auto *t=cache_buckets[bucket];t;t=t->next_hash)
            if (t->lookup_hash==lookup_hash && t->header==image_header && t->level==level && t->tex.fmt==format && t->palette==palette) {
                trace(true);t->used=texture_epoch;return remember(*t);
            }
        trace(false);
        const size_t bytes = size_t(w)*h*(half ? 2 : 4);
        size_t block;
        while ((block = free_pool_block(bytes)) == pool_used.size()) {
            auto oldest = cache.end();
            {
                TickScope timer(timing_active,eviction_ticks);
                oldest=indexed_eviction?indexed_oldest():scan_oldest();
                if(verify_eviction) {
                    ++eviction_checks;
                    if(oldest!=scan_oldest())
                        throw std::runtime_error("Indexed eviction differs from original oldest texture");
                }
            }
            if(oldest==cache.end()) {
                // No texture can be evicted until all its GPU readers finish.
                // Outputs remain detached during these batches, so partial
                // stereo frames are never presented to the user.
                submit_batch();

                continue;
            }
            if(timing_active) ++evictions;
            erase(oldest,indexed_eviction);
        }
        cache.push_back({image_header, level, renderer!=3, palette, {}, texture_epoch});
        auto &t = cache.back();
        t.allocation_serial=++allocation_serial;
        if(indexed_eviction && eviction_heap_valid) {
            eviction_heap.push_back({std::prev(cache.end()),t.used});
            std::push_heap(eviction_heap.begin(),eviction_heap.end(),later_eviction);
        }
        t.lookup_hash=lookup_hash;t.next_hash=cache_buckets[bucket];cache_buckets[bucket]=&t;
        // Same public descriptor fields as C3D_TexInitWithParams, but storage
        // belongs to our bounded pool; never C3D_TexDelete a borrowed range.
        t.tex.data = texture_pool + block * PoolUnit;
        t.tex.width = u16(w); t.tex.height = u16(h);
        t.tex.fmt = format; t.tex.size = bytes;
        t.tex.param = GPU_TEXTURE_MODE(GPU_TEX_2D);
        std::fill_n(pool_used.begin() + block, bytes / PoolUnit, u8(1));
        if(pool_index) pool_index->set_used(block,bytes/PoolUnit,true);
        C3D_TexSetFilter(&t.tex, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&t.tex, m.texmirrorx ? GPU_MIRRORED_REPEAT : GPU_REPEAT,
                                m.texmirrory ? GPU_MIRRORED_REPEAT : GPU_REPEAT);
        // Low-texture mode stores RGBA5551; alpha stays exact (0 or 255).
        const unsigned bx = ((m.texx-2048u) >> level) & 2047u;
        const unsigned by = ((m.texy-1024u) >> level) & 1023u;
        const auto *sheet = m.texsheet[level & 1];
        const u64 decode_begin=timing_active?svcGetSystemTick():0;
        auto decode = [&](auto *out, const auto &colors) {
            if ((bx | by) & 1u) {
                // Retain the scalar path for an unaligned source origin.
                for (unsigned y=0; y<h; ++y) for (unsigned x=0; x<w; ++x)
                    out[morton(x,y,w)] = colors[texel(sheet,bx,by,x,y)];
            } else {
                // One packed 16-bit source word contains a 2x2 block. Its four
                // output pixels are consecutive in PICA Morton order. Decode the
                // address once rather than once per pixel, preserving exact RGBA.
                for (unsigned y=0; y<h; y+=2) for (unsigned x=0; x<w; x+=2) {
                    unsigned xx=bx+x, yy=by+y;
                    if (xx>=1024) { xx-=1024; yy^=1024; }
                    const unsigned offset=(yy/2)*512+xx/2;
                    u32 packed=sheet[(offset>>1)&0x7ffff];
                    if(offset&1) packed>>=16;
                    auto *block=out+morton(x,y,w);
                    block[0]=colors[(packed>>12)&15];
                    block[1]=colors[(packed>>8)&15];
                    block[2]=colors[(packed>>4)&15];
                    block[3]=colors[packed&15];
                }
            }
        };
        if (half) {
            std::array<u16,16> compact;
            for (unsigned i=0;i<16;++i) {
                const u32 c=palette[i];
                compact[i]=u16(((c>>16)&0xf800u)|((c>>13)&0x07c0u)|((c>>10)&0x003eu)|((c&255u)?1u:0u));
            }
            decode(static_cast<u16*>(t.tex.data),compact);
        } else decode(static_cast<u32*>(t.tex.data),palette);
        if(opaque_color_groups && renderer==3) {
            // Determine opacity from the actual sampled texture, not just the
            // arcade renderer's ability to use a transparent palette index.
            // Cache it once; all fast material hits retain this descriptor.
            t.opaque=half ? three_ds_texture_is_opaque(static_cast<const u16*>(t.tex.data),w*h,1u) :
                three_ds_texture_is_opaque(static_cast<const u32*>(t.tex.data),w*h,255u);
        }
        C3D_TexFlush(&t.tex);
        if(timing_active)decode_ticks+=svcGetSystemTick()-decode_begin;
        cache_bytes+=bytes; ++counters.uploads;
        return remember(t);
    }
    void set_scissor(GPU_SCISSORMODE mode,u32 left,u32 top,u32 right,u32 bottom) {
        ++scissor_calls;
        const std::array<u32,5> requested={u32(mode),left,top,right,bottom};
        if(!scissor_valid || requested!=last_scissor) {
            C3D_SetScissor(mode,left,top,right,bottom);
            last_scissor=requested;scissor_valid=true;++scissor_updates;
        }
    }
    void state(C3D_Tex *tex) {
        ++state_calls;
        const int mode=tex?1:0;
        if(combiner_mode!=mode) {
            if(three_textures) {
                C3D_TexEnvInit(C3D_GetTexEnv(1));C3D_TexEnvInit(C3D_GetTexEnv(2));
                if(opaque_color_groups) C3D_TexEnvInit(C3D_GetTexEnv(3));
                Daytona_ClearAuxTextures();bound_aux.fill(nullptr);
            }
            auto *env=C3D_GetTexEnv(0); C3D_TexEnvInit(env);
            if(tex) {
                C3D_TexEnvSrc(env,C3D_Both,GPU_TEXTURE0,GPU_PRIMARY_COLOR);
                C3D_TexEnvFunc(env,C3D_Both,GPU_MODULATE);
            } else {
                C3D_TexEnvSrc(env,C3D_Both,GPU_PRIMARY_COLOR);
                C3D_TexEnvFunc(env,C3D_Both,GPU_REPLACE);
            }
            combiner_mode=mode;++combiner_updates;
        }
        // Batch pins protect material descriptors; invalidate before pins are reset.
        if(tex && bound_texture!=tex) {
            C3D_TexBind(0,tex);bound_texture=tex;++texture_binds;
        }
    }
    void multi_state(const std::array<C3D_Tex*,3> &textures,bool opaque=false) {
        ++state_calls;
        if(opaque) {
            if(combiner_mode!=3) {
                C3D_TexEnv *stages[]={C3D_GetTexEnv(0),C3D_GetTexEnv(1),C3D_GetTexEnv(2),C3D_GetTexEnv(3)};
                three_ds_configure_opaque_combiner(stages);
                combiner_mode=3;++combiner_updates;
            }
        } else if(combiner_mode!=2) {
            if(opaque_color_groups) C3D_TexEnvInit(C3D_GetTexEnv(3));
            auto *first=C3D_GetTexEnv(0);C3D_TexEnvInit(first);
            C3D_TexEnvSrc(first,C3D_Both,GPU_TEXTURE0,GPU_TEXTURE1,GPU_PRIMARY_COLOR);
            C3D_TexEnvOpRgb(first,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_R);
            C3D_TexEnvOpAlpha(first,GPU_TEVOP_A_SRC_ALPHA,GPU_TEVOP_A_SRC_ALPHA,GPU_TEVOP_A_SRC_R);
            C3D_TexEnvFunc(first,C3D_Both,GPU_INTERPOLATE);
            auto *second=C3D_GetTexEnv(1);C3D_TexEnvInit(second);
            C3D_TexEnvSrc(second,C3D_Both,GPU_PREVIOUS,GPU_TEXTURE2,GPU_PRIMARY_COLOR);
            C3D_TexEnvOpRgb(second,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_G);
            C3D_TexEnvOpAlpha(second,GPU_TEVOP_A_SRC_ALPHA,GPU_TEVOP_A_SRC_ALPHA,GPU_TEVOP_A_SRC_G);
            C3D_TexEnvFunc(second,C3D_Both,GPU_INTERPOLATE);
            auto *last=C3D_GetTexEnv(2);C3D_TexEnvInit(last);
            C3D_TexEnvSrc(last,C3D_Alpha,GPU_PREVIOUS,GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(last,C3D_Alpha,GPU_MODULATE);
            combiner_mode=2;++combiner_updates;
        }
        if(bound_texture!=textures[0]) {
            C3D_TexBind(0,textures[0]);bound_texture=textures[0];++texture_binds;
        }
        for(unsigned i=0;i<2;++i) if(bound_aux[i]!=textures[i+1]) {
            C3D_TexBind(i+1,textures[i+1]);bound_aux[i]=textures[i+1];++texture_binds;
        }
    }
    void append(float x,float y,float q,float u,float v,u32 rgba) {
        if(cursor>=VertexCapacity) throw std::runtime_error("3DS scene exceeds GPU vertex capacity");
        // Reference raster input is screen position and (1/z,u/z,v/z).
        // Reconstruct homogeneous W to let PICA interpolate UVs perspectively.
        const float w=1.f/q;
        const float padding=(400-output_width)*.5f;
        vertices[cursor++]={ (1.f-y/120.f)*w, (1.f-(x+padding)/200.f)*w,
            -.5f*w,w,u,v, u8(rgba>>24),u8(rgba>>16),u8(rgba>>8),u8(rgba) };
        ++counters.vertices;
    }
    // Commands keep cached textures pinned until submit_batch completes both eyes.
    void flush_stereo_draws() {
        if(!stereo_count) return;
        TickScope timer(timing_active,draw_ticks);
        // Preserve every polygon and triangle in native order. Textured runs
        // can use three material slots; optional opaque solids share a palette.
        // Translucent solids keep their original color/blending path.
        std::array<unsigned,StereoDrawCapacity> run_end{},run_first{},run_count{};
        std::array<bool,StereoDrawCapacity> run_opaque{};
        std::array<std::array<C3D_Tex*,3>,StereoDrawCapacity> run_textures{};
        for(unsigned i=0;i<stereo_count;) {
            unsigned end=i+1;
            const auto &first=stereo_draws[i];
            if(opaque_color_groups && first.opaque) {
                auto &textures=run_textures[i];
                unsigned texture_count=0;
                end=i;
                while(end<stereo_count) {
                    const auto &next=stereo_draws[end];
                    if(!next.opaque || first.screen_space!=next.screen_space || first.scissor!=next.scissor) break;
                    if(next.texture) {
                        unsigned slot=0;while(slot<texture_count && textures[slot]!=next.texture) ++slot;
                        if(slot==texture_count) {
                            if(texture_count==3) break;
                            textures[texture_count++]=next.texture;
                        }
                    }
                    ++end;
                }
                // Alpha selects three opaque textures or the original vertex
                // RGB. All choices produce alpha 255. Transparent textures and
                // checker blending remain on the original material path.
                if(texture_count) {
                    bool solid_seen=false;
                    for(unsigned j=i;j<end;++j) {
                        const auto &draw=stereo_draws[j];
                        unsigned slot=3;
                        if(draw.texture) {
                            slot=0;while(slot<texture_count && textures[slot]!=draw.texture) ++slot;
                            if(slot==texture_count) throw std::runtime_error("GPU opaque material slot missing");
                        }
                        opaque_selector_mask|=1u<<slot;solid_seen|=slot==3;
                        for(unsigned k=0;k<draw.count;++k)
                            vertices[draw.first[0]+k].a=three_ds_opaque_slot_alpha(slot);
                    }
                    for(unsigned slot=texture_count;slot<3;++slot) textures[slot]=textures[0];
                    run_opaque[i]=true;
                    if(solid_seen) ++opaque_mixed_groups;
                }
                // Solid-only runs retain primary RGBA and need no texture.
                opaque_group_polygons+=end-i;
            } else if(three_textures && (first.texture || (solid_color_groups && vertices[first.first[0]].a==255))) {
                auto &textures=run_textures[i];
                unsigned texture_count=0;
                const unsigned texture_limit=solid_color_groups?2:3;
                end=i;
                while(end<stereo_count) {
                    const auto &next=stereo_draws[end];
                    if((!next.texture && (!solid_color_groups || vertices[next.first[0]].a!=255)) || first.screen_space!=next.screen_space || first.scissor!=next.scissor) break;
                    if(next.texture) {
                        unsigned slot=0;while(slot<texture_count && textures[slot]!=next.texture) ++slot;
                        if(slot==texture_count) {
                            if(texture_count==texture_limit) break;
                            textures[texture_count++]=next.texture;
                        }
                    }
                    ++end;
                }
                for(unsigned j=i;j<end;++j) {
                    const auto &draw=stereo_draws[j];
                    unsigned slot=2;
                    float solid_u=0,solid_v=0;
                    if(draw.texture) {
                        slot=0;while(slot<texture_count && textures[slot]!=draw.texture) ++slot;
                        if(slot==texture_count) throw std::runtime_error("GPU material slot missing");
                    } else {
                        // Each GPU batch owns its palette cells until FrameBegin
                        // has joined the submitted queue. Both eyes share them.
                        if(solid_palette_cursor>=SolidPaletteSide*SolidPaletteSide)
                            throw std::runtime_error("GPU solid palette capacity exceeded");
                        const unsigned index=solid_palette_cursor++;
                        const auto &source=vertices[draw.first[0]];
                        static_cast<u32*>(solid_palette.data)[morton(index%SolidPaletteSide,index/SolidPaletteSide,SolidPaletteSide)]=
                            (u32(source.r)<<24)|(u32(source.g)<<16)|(u32(source.b)<<8)|255u;
                        solid_u=(float(index%SolidPaletteSide)+.5f)/SolidPaletteSide;
                        solid_v=(float(index/SolidPaletteSide)+.5f)/SolidPaletteSide;
                    }
                    for(unsigned k=0;k<draw.count;++k) {
                        auto &v=vertices[draw.first[0]+k];
                        if(!draw.texture) {v.u=solid_u;v.v=solid_v;}
                        v.r=slot==0?255:0;v.g=slot==2?0:255;
                        // A retains transparent/checker blending.
                    }
                }
                C3D_Tex *fallback=texture_count?textures[0]:&solid_palette;
                for(unsigned slot=texture_count;slot<texture_limit;++slot) textures[slot]=fallback;
                if(solid_color_groups) textures[2]=&solid_palette;
            } else if(adjacent_groups) while(end<stereo_count) {
                const auto &next=stereo_draws[end];
                if(first.texture!=next.texture || first.screen_space!=next.screen_space || first.scissor!=next.scissor) break;
                ++end;
            }
            run_end[i]=end;
            if(end>i+1) {
                run_first[i]=index_cursor;
                for(unsigned j=i;j<end;++j) {
                    const auto &draw=stereo_draws[j];
                    const unsigned needed=3*(draw.count-2);
                    if(needed>IndexCapacity-index_cursor) throw std::runtime_error("GPU grouped index capacity exceeded");
                    for(unsigned k=1;k+1<draw.count;++k) {
                        group_indices[index_cursor++]=u16(draw.first[0]);
                        group_indices[index_cursor++]=u16(draw.first[0]+k);
                        group_indices[index_cursor++]=u16(draw.first[0]+k+1);
                    }
                }
                run_count[i]=index_cursor-run_first[i];
                grouped_polygons+=end-i; ++grouped_calls;
            }
            i=end;
        }
        u32 *record_buffer=nullptr, record_first=0, record_words=0;
        for(unsigned eye=0;eye<2;++eye) {
            C3D_FrameDrawOn(geometry_target[eye]);
            C3D_SetViewport(0,0,unsigned(240*scale),unsigned(400*scale));
            if(replay_stereo && eye==1) {
                const float offset=(raw_scale_x/200.f)*pair_eye;
                C3D_FVUnifSet(GPU_VERTEX_SHADER,stereo_uniform,-offset/raw_focus,offset,0,0);
                // Keep the final material state mirrored in Citro3D. Only the
                // new framebuffer, viewport and initial eye vector go before
                // the copied list; none can redirect the recorded draws.
                Daytona_FlushGpuContext();
                if(!Daytona_ReplayStereoCommands(record_buffer+record_first,record_words,unsigned(stereo_uniform)))
                    throw std::runtime_error("3DS stereo command replay rejected unsafe command list");
                ++replayed_chunks;replayed_words+=record_words;
                // SetViewport resets Citro3D's scissor shadow. The replay ends
                // at the left list's final scissor, so restore that shadow too.
                C3D_SetScissor(GPU_SCISSORMODE(last_scissor[0]),last_scissor[1],last_scissor[2],last_scissor[3],last_scissor[4]);
                continue;
            }
            invalidate_draw_state();
            if(shader_stereo) {
                const float offset=(raw_scale_x/200.f)*(eye?pair_eye:-pair_eye);
                C3D_FVUnifSet(GPU_VERTEX_SHADER,stereo_uniform,-offset/raw_focus,offset,0,0);
            }
            if(replay_stereo) {
                // Consume all pending target/program/initial uniform state so
                // it cannot be copied into the second eye's draw list.
                Daytona_FlushGpuContext();
                GPUCMD_GetBuffer(&record_buffer,nullptr,&record_first);
            }
            bool screen_space=false;
            for(unsigned i=0;i<stereo_count;i=run_end[i]) {
                const auto &draw=stereo_draws[i];
                if(hud_stereo && screen_space!=draw.screen_space) {
                    screen_space=draw.screen_space;
                    const float offset=screen_space?0.f:(raw_scale_x/200.f)*(eye?pair_eye:-pair_eye);
                    C3D_FVUnifSet(GPU_VERTEX_SHADER,stereo_uniform,-offset/raw_focus,offset,0,0);
                }
                if(run_textures[i][0]) multi_state(run_textures[i],run_opaque[i]); else state(draw.texture);
                set_scissor(GPU_SCISSOR_NORMAL,draw.scissor[0],draw.scissor[1],draw.scissor[2],draw.scissor[3]);
                if(run_count[i]) {
                    if(packed_elements) {
                        Daytona_DrawTrianglesPacked(run_count[i],group_indices+run_first[i]);
                        ++packed_element_draws;
                    } else C3D_DrawElements(GPU_TRIANGLES,run_count[i],C3D_UNSIGNED_SHORT,group_indices+run_first[i]);
                }
                else if(packed_draw) Daytona_DrawArraysPacked(GPU_TRIANGLE_FAN,draw.first[eye],draw.count);
                else C3D_DrawArrays(GPU_TRIANGLE_FAN,draw.first[eye],draw.count);
            }
            if(replay_stereo) {
                u32 *end_buffer=nullptr,end_words=0;
                GPUCMD_GetBuffer(&end_buffer,nullptr,&end_words);
                if(end_buffer!=record_buffer || end_words<record_first)
                    throw std::runtime_error("3DS stereo command buffer changed during capture");
                record_words=end_words-record_first;
            }
        }
        stereo_count=0;
        if(shader_stereo) C3D_FVUnifSet(GPU_VERTEX_SHADER,stereo_uniform,0,0,0,0);
    }
    void stereo_polygon(int renderer,const rt::GeoPoly &p,const int *clip,const rt::Raster::Extra &m) {
        if(stereo_count==stereo_batch_limit) flush_stereo_draws();
        ensure_command_room((shader_stereo?1:2)*p.num_vertices);
        bool sampled_opaque=renderer!=3;
        C3D_Tex *tex=renderer&2 ? texture(renderer,m,p,&sampled_opaque) : nullptr;
        u32 rgba=0xffffffff;
        if(!tex) rgba=color(*memory,le16(memory->palram,m.colorbase+0x1000),m.luma>>2);
        if(m.checker) rgba=(rgba&0xffffff00u)|128u;
        // Texture allocation may submit a batch, so acquire the record afterward.
        auto &draw=stereo_draws[stereo_count++];
        draw.texture=tex;draw.count=p.num_vertices;
        draw.screen_space=hud_stereo && three_ds_screen_polygon(p);
        draw.opaque=three_ds_opaque_material(renderer,m.checker,sampled_opaque);
        const unsigned pad=(400-output_width)/2;
        draw.scissor={unsigned((240-(clip[3]+1))*scale),unsigned((400-(clip[1]+1+pad))*scale),unsigned((240-clip[2])*scale),unsigned((400-(clip[0]+pad))*scale)};
        const float ax=raw_scale_x/200.f, ay=raw_scale_y/120.f;
        const float cx=1.f-((raw_crtc_x+p.center[0])*raw_scale_x+raw_offset_x+pad)/200.f;
        const float cy=1.f-((384-p.center[1]+raw_crtc_y)*raw_scale_y)/120.f;
        const float us=tex?.125f/m.texwidth:0, vs=tex?.125f/m.texheight:0;
        for(unsigned eye=0;eye<(shader_stereo?1u:2u);++eye) {
            const float stereo=shader_stereo?0.f:ax*(eye?pair_eye:-pair_eye), stereo_z=stereo/raw_focus;
            draw.first[eye]=cursor;
            for(unsigned i=0;i<p.num_vertices;++i) {
                const auto &v=p.v[i];const float z=v.p[0];
                vertices[cursor++]={cy*z+ay*v.y,(cx-stereo_z)*z-ax*v.x+stereo,
                    -.5f*z,z,v.p[1]*us,v.p[2]*vs,
                    u8(rgba>>24),u8(rgba>>16),u8(rgba>>8),u8(rgba)};
                ++counters.vertices;
            }
        }
        if(shader_stereo) draw.first[1]=draw.first[0];
        counters.polygons+=2;
    }
    void polygon(int renderer, const rt::GeoPoly &p, const int *clip, const rt::Raster::Extra &m) {
        if(p.num_vertices<3) return;
        for(unsigned i=0;i<p.num_vertices;++i)
            if(!(p.v[i].p[0]>0) || !std::isfinite(p.v[i].p[0]) || !std::isfinite(p.v[i].x) || !std::isfinite(p.v[i].y)) return;
        if(stereo_recording){stereo_polygon(renderer,p,clip,m);return;}
        ensure_command_room(raw_projection?p.num_vertices:3*(p.num_vertices-2));
        bool sampled_opaque=renderer!=3;
        C3D_Tex *tex=renderer&2 ? texture(renderer,m,p,&sampled_opaque) : nullptr;
        u32 rgba=0xffffffff;
        if(!tex) rgba=color(*memory,le16(memory->palram,m.colorbase+0x1000),m.luma>>2);
        if(m.checker) rgba=(rgba&0xffffff00u)|128u;
        state(tex);
        const unsigned pad=(400-output_width)/2;
        // The target is rotated: physical X is logical (240-Y), physical Y is (400-X).
        set_scissor(GPU_SCISSOR_NORMAL,unsigned((240-(clip[3]+1))*scale),unsigned((400-(clip[1]+1+pad))*scale),unsigned((240-clip[2])*scale),unsigned((400-(clip[0]+pad))*scale));
        const unsigned first=cursor;
        const float ax=raw_scale_x/200.f, ay=raw_scale_y/120.f;
        const float cx=1.f-((raw_crtc_x+p.center[0])*raw_scale_x+raw_offset_x+pad)/200.f;
        const float cy=1.f-((384-p.center[1]+raw_crtc_y)*raw_scale_y)/120.f;
        const float stereo=ax*raw_eye, stereo_z=stereo/raw_focus;
        const float us=tex?.125f/m.texwidth:0, vs=tex?.125f/m.texheight:0;
        auto emit=[&](const rt::GeoVertex &v) {
            if(raw_projection) {
                // PICA performs perspective division; keep view-space depth as W.
                const float z=v.p[0];
                vertices[cursor++]={cy*z+ay*v.y,(cx-stereo_z)*z-ax*v.x+stereo,
                    -.5f*z,z,v.p[1]*us,v.p[2]*vs,
                    u8(rgba>>24),u8(rgba>>16),u8(rgba>>8),u8(rgba)};
                ++counters.vertices;
            } else {
                const float inv=v.p[0];
                append(v.x,v.y,inv, tex ? v.p[1]/inv/m.texwidth : 0, tex ? v.p[2]/inv/m.texheight : 0, rgba);
            }
        };
        // The native fan uses the same (0,i,i+1) triangles without repeating vertices.
        if(raw_projection) {
            for(unsigned i=0;i<p.num_vertices;++i) emit(p.v[i]);
        } else {
            for(unsigned i=1;i+1<p.num_vertices;++i){emit(p.v[0]);emit(p.v[i]);emit(p.v[i+1]);}
        }
        C3D_DrawArrays(raw_projection?GPU_TRIANGLE_FAN:GPU_TRIANGLES,first,cursor-first);
        ++counters.polygons;
    }
    void update_tiles(const rt::Video &video) {
        if(!video.tile_span_capture()) throw std::runtime_error("GPU tiles require layer commands");
        const u32 *pens=video.tile_colors();
        for(unsigned p=0;p<256;++p) {
            if(std::memcmp(tiles->colors.data()+p*16,pens+p*16,64)) {
                std::memcpy(tiles->colors.data()+p*16,pens+p*16,64);
                if(++tiles->palette_versions[p]==0) {tiles->identities.fill(UINT64_MAX);++tiles->palette_versions[p];}
            }
        }
        unsigned used=0;
        for(bool front:{false,true}) for(const auto &s:video.tile_spans(front)) used|=1u<<s.layer;
        unsigned staging=0, transfers=0;
        for(unsigned layer=0;layer<4;++layer) {
            if(!(used&(1u<<layer))) continue;
            const auto &pixels=video.tile_pixels(layer);
            auto dirty=[&](unsigned t) {
                const unsigned index=layer*4096+t;
                const uint64_t identity=video.tile_identity(index);
                const unsigned pal=unsigned(identity>>39)&255;
                return tiles->identities[index]!=identity || tiles->palettes[index]!=tiles->palette_versions[pal];
            };
            for(unsigned t=0;t<4096;) {
                if(!dirty(t)){++t;continue;}
                if(staging==TileStagingBytes || transfers==16){submit_batch();staging=0;transfers=0;}
                const unsigned first=t, begin=staging;
                do {
                    u32 *out=reinterpret_cast<u32*>(tile_staging+staging);
                    const unsigned tx=(t&63)*8,ty=(t>>6)*8,index=layer*4096+t;
                    for(unsigned y=0;y<8;++y) for(unsigned x=0;x<8;++x) {
                        const u16 pixel=pixels[(ty+y)*512+tx+x];
                        const unsigned alpha=(pixel&0x2000)?((pixel&0x1000)?255:192):((pixel&0x1000)?128:64);
                        out[morton(x,y,8)]=(pens[pixel&0xfff]<<8)|alpha;
                    }
                    const uint64_t identity=video.tile_identity(index);
                    tiles->identities[index]=identity;
                    tiles->palettes[index]=tiles->palette_versions[unsigned(identity>>39)&255];
                    staging+=256;++t;++counters.tile_uploads;
                } while(t<4096 && staging<TileStagingBytes && dirty(t));
                GSPGPU_FlushDataCache(tile_staging+begin,staging-begin);
                C3D_SyncTextureCopy(reinterpret_cast<u32*>(tile_staging+begin),0,
                    reinterpret_cast<u32*>(static_cast<u8*>(tile_maps[layer].data)+first*256),0,staging-begin,8);
                ++transfers;
            }
        }
        // Retain staging until every queued copy finishes before geometry uses it.
        if(transfers) submit_batch();
    }
    void tile_layer(const rt::Video &video,bool front) {
        if(layer_width!=output_width || layer_height!=240) throw std::runtime_error("GPU tile output dimensions unsupported");
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ZERO,GPU_ONE,GPU_ZERO);
        if(!front) {
            ensure_command_room();state(nullptr);C3D_AlphaTest(false,GPU_ALWAYS,0);
            set_scissor(GPU_SCISSOR_DISABLE,0,0,240,400);
            const unsigned first=cursor;const u32 rgba=(video.tile_colors()[0]<<8)|255;
            append(0,0,1,0,0,rgba);append(output_width,0,1,0,0,rgba);append(output_width,240,1,0,0,rgba);
            append(0,0,1,0,0,rgba);append(output_width,240,1,0,0,rgba);append(0,240,1,0,0,rgba);
            C3D_DrawArrays(GPU_TRIANGLES,first,6);
        }
        const float source_width=front && video.widescreen()?layer_width*.75f:float(layer_width);
        const float offset=front && video.widescreen()?layer_width*.125f:0.f;
        for(const auto &s:video.tile_spans(front)) {
            ensure_command_room();state(&tile_maps[s.layer]);
            set_scissor(GPU_SCISSOR_DISABLE,0,0,240,400);
            switch(s.rule) {
                case 0:C3D_AlphaTest(false,GPU_ALWAYS,0);break;
                case 1:C3D_AlphaTest(true,GPU_EQUAL,128);break;
                case 2:C3D_AlphaTest(true,GPU_EQUAL,255);break;
                case 3:C3D_AlphaTest(true,GPU_LEQUAL,128);break;
                case 4:C3D_AlphaTest(true,GPU_GEQUAL,192);break;
                default:throw std::runtime_error("Unknown GPU tile selection rule");
            }
            const unsigned first=cursor;
            auto emit=[&](float x,float y) {
                const float u=((x-.5f-offset)*496.f/source_width+s.sx+1.f/4096.f)/512.f;
                const float v=1.f-((y-.5f)*384.f/layer_height+s.sy+1.f/4096.f)/512.f;
                append(x,y,1,u,v,0xffffffff);
            };
            emit(s.x0,s.y0);emit(s.x1,s.y0);emit(s.x1,s.y1);
            emit(s.x0,s.y0);emit(s.x1,s.y1);emit(s.x0,s.y1);
            C3D_DrawArrays(GPU_TRIANGLES,first,6);++counters.tile_commands;
        }
        C3D_AlphaTest(true,GPU_GREATER,0);
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
    }
    void upload_layer(C3D_Tex &tex,const std::vector<u32> &pixels,bool front) {
        if(pixels.size()!=size_t(layer_width)*layer_height)
            throw std::runtime_error("GPU HUD layer size does not match sampled output");
        auto *out=static_cast<u32*>(tex.data);
        for(unsigned y=0;y<unsigned(layer_height);++y) for(unsigned x=0;x<unsigned(layer_width);++x){
            const u32 p=pixels[size_t(y)*layer_width+x];
            out[morton(x,y,512)]=(p<<8)|((front && !p)?0u:255u);
        }
        C3D_TexFlush(&tex);
    }
    void layer(C3D_Tex &tex) {
        ensure_command_room();
        state(&tex); set_scissor(GPU_SCISSOR_DISABLE,0,0,240,400);
        const unsigned first=cursor;
        const float u=float(layer_width)/512.f,v=float(layer_height)/256.f;
        append(0,0,1,0,1,0xffffffff);append(output_width,0,1,u,1,0xffffffff);append(output_width,240,1,u,1-v,0xffffffff);
        append(0,0,1,0,1,0xffffffff);append(output_width,240,1,u,1-v,0xffffffff);append(0,240,1,0,1-v,0xffffffff);
        C3D_DrawArrays(GPU_TRIANGLES,first,6);
    }
    void geometry_layer(unsigned e) {
        ensure_command_room();
        state(&geometry_texture[e]);set_scissor(GPU_SCISSOR_DISABLE,0,0,240,400);
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
        const unsigned first=cursor;
        const float pad=(400-output_width)*.5f;
        auto emit=[&](float x,float y){append(x-pad,y,1,(240-y)*scale/256,(400-x)*scale/512,0xffffffff);};
        emit(0,0);emit(400,0);emit(400,240);emit(0,0);emit(400,240);emit(0,240);
        C3D_DrawArrays(GPU_TRIANGLES,first,6);
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
    }
    void render(const rt::Video &video,int width,float eye,float focus,bool half_textures,float render_scale) {
        if(!video.sampled_layer_output_active()) throw std::runtime_error("GPU renderer requires sampled HUD layers");
        // FrameBegin waits for the previous GPU frame before any buffer is reused.
        timing_active=detailed_profile && (frame+1)%60==30;
        const bool phase_timing=benchmark_mode || timing_active;
        material_ticks=decode_ticks=batch_ticks=draw_ticks=0;
        packed_element_draws=0;
        pool_ticks=eviction_ticks=pool_probes=eviction_probes=0;
        pool_queries=pool_failures=evictions=0;
        eviction_checks=eviction_refreshes=eviction_builds=0;
        u64 geometry_ticks=0,composite_ticks=0;
        const u64 started=svcGetSystemTick();
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        const u64 after_wait=phase_timing?svcGetSystemTick():0;
        const float previous_gpu_chunk_ms=timing_active?C3D_GetDrawingTime():0;
        ++frame; ++texture_epoch; cursor=0; index_cursor=0; solid_palette_cursor=0;grouped_polygons=grouped_calls=opaque_group_polygons=opaque_selector_mask=opaque_mixed_groups=0; counters={}; output_width=width; half=half_textures;scale=render_scale;
        clear_fast_materials();invalidate_draw_state();material_lookups=material_hits=0;state_calls=combiner_updates=texture_binds=0;scissor_calls=scissor_updates=0;
        if(trace_coverage && frame==coverage_frame) {
            coverage=std::fopen("sdmc:/3ds/daytona/texture-coverage.csv","w");
            if(coverage) std::fprintf(coverage,"header,palette,level,width,height,min_u,max_u,min_v,max_v,hit\n");
        }
        for(int i=0;i<2;++i)C3D_RenderTargetDetachOutput(target[i]);
        const auto scene=video.presentation_scene(); memory=&scene.memory;
        raw_crtc_x=scene.crtc_x;raw_crtc_y=scene.crtc_y;
        raw_scale_x=float(width)/(video.widescreen()?496.f*4.f/3.f:496.f);
        raw_scale_y=240.f/384.f;raw_offset_x=video.widescreen()?float(width)/8.f:0.f;
        raw_focus=focus;
        const auto dimensions=video.sampled_layer_dimensions();
        layer_width=dimensions.first;layer_height=dimensions.second;
        if(revision!=memory->texture_revision){counters.revision_resets=1;while(!cache.empty())erase(cache.begin());revision=memory->texture_revision;}
        const u64 upload_begin=phase_timing?svcGetSystemTick():0;
        if(tile_mode) update_tiles(video); else {upload_layer(layers[0],scene.back,false);upload_layer(layers[1],scene.front,true);}
        const u64 upload_end=phase_timing?svcGetSystemTick():0;
        C3D_BindProgram(&program);
        C3D_FVUnifSet(GPU_VERTEX_SHADER,stereo_uniform,0,0,0,0);
        auto *attrs=C3D_GetAttrInfo();AttrInfo_Init(attrs);
        AttrInfo_AddLoader(attrs,0,GPU_FLOAT,4);AttrInfo_AddLoader(attrs,1,GPU_FLOAT,2);AttrInfo_AddLoader(attrs,2,GPU_UNSIGNED_BYTE,4);
        auto *buffers=C3D_GetBufInfo();BufInfo_Init(buffers);BufInfo_Add(buffers,vertices,sizeof(Vertex),3,0x210);
        C3D_CullFace(GPU_CULL_NONE); C3D_DepthTest(false,GPU_ALWAYS,GPU_WRITE_COLOR);
        C3D_AlphaTest(true,GPU_GREATER,0);
        C3D_AlphaBlend(GPU_BLEND_ADD,GPU_BLEND_ADD,GPU_SRC_ALPHA,GPU_ONE_MINUS_SRC_ALPHA,GPU_ONE,GPU_ONE_MINUS_SRC_ALPHA);
        if(stereo_chunks) {
            for(unsigned e=0;e<2;++e)
                C3D_RenderTargetClear(geometry_target[e],C3D_CLEAR_COLOR,0x00000000,0);
            pair_eye=eye;stereo_count=0;stereo_recording=true;
            projection.set_target(width,240,video.widescreen());projection.set_stereo(0,focus);
            const u64 geometry_begin=phase_timing?svcGetSystemTick():0;
            projection.render(scene.polys,scene.windows,scene.memory,scene.crtc_x,scene.crtc_y,scene.render_x,scene.render_y,video.widescreen()?-83:0,video.widescreen()?rt::Video::W+82:rt::Video::W-1,0,rt::Video::H-1);
            flush_stereo_draws();stereo_recording=false;
            const u64 composite_begin=phase_timing?svcGetSystemTick():0;
            if(phase_timing)geometry_ticks+=composite_begin-geometry_begin;
            for(unsigned e=0;e<2;++e) {
                C3D_RenderTargetClear(target[e],C3D_CLEAR_COLOR,0x000000ff,0);
                C3D_FrameDrawOn(target[e]);scissor_valid=false;
                if(tile_mode){tile_layer(video,false);geometry_layer(e);tile_layer(video,true);} else {layer(layers[0]);geometry_layer(e);layer(layers[1]);}
            }
            if(phase_timing)composite_ticks+=svcGetSystemTick()-composite_begin;
        } else {
        for(int e=0;e<2;++e){
            C3D_RenderTargetClear(geometry_target[e],C3D_CLEAR_COLOR,0x00000000,0);
            C3D_FrameDrawOn(geometry_target[e]);scissor_valid=false;
            C3D_SetViewport(0,0,unsigned(240*scale),unsigned(400*scale));scissor_valid=false;
            projection.set_target(width,240,video.widescreen());projection.set_stereo(e?eye:-eye,focus);
            raw_eye=e?eye:-eye;
            const u64 geometry_begin=phase_timing?svcGetSystemTick():0;
            projection.render(scene.polys,scene.windows,scene.memory,scene.crtc_x,scene.crtc_y,scene.render_x,scene.render_y,video.widescreen()?-83:0,video.widescreen()?rt::Video::W+82:rt::Video::W-1,0,rt::Video::H-1);
            const u64 composite_begin=phase_timing?svcGetSystemTick():0;
            if(phase_timing)geometry_ticks+=composite_begin-geometry_begin;
            C3D_RenderTargetClear(target[e],C3D_CLEAR_COLOR,0x000000ff,0);
            C3D_FrameDrawOn(target[e]);scissor_valid=false;
            if(tile_mode){tile_layer(video,false);geometry_layer(e);tile_layer(video,true);} else {layer(layers[0]);geometry_layer(e);layer(layers[1]);}
            if(phase_timing)composite_ticks+=svcGetSystemTick()-composite_begin;
        }
        }
        GSPGPU_FlushDataCache(vertices,cursor*sizeof(Vertex));
        if(index_cursor) GSPGPU_FlushDataCache(group_indices,index_cursor*sizeof(u16));
        if(solid_palette_cursor) C3D_TexFlush(&solid_palette);
        for(int i=0;i<2;++i)C3D_RenderTargetSetOutput(target[i],GFX_TOP,i?GFX_RIGHT:GFX_LEFT,TransferFlags);
        Daytona_FrameEndFlushed();
        if(coverage){std::fclose(coverage);coverage=nullptr;}
        counters.textures=cache.size();counters.texture_bytes=cache_bytes;
        counters.cpu_ms=float(svcGetSystemTick()-started)*1000.f/SYSCLOCK_ARM11;
        if(phase_timing) {
            const float ms=1000.f/SYSCLOCK_ARM11;
            counters.wait_ms=(after_wait-started)*ms;
            counters.upload_ms=(upload_end-upload_begin)*ms;
            counters.geometry_ms=geometry_ticks*ms;
            counters.composite_ms=composite_ticks*ms;
        }
        counters.command_usage=std::max(counters.command_usage,C3D_GetCmdBufUsage());
        if(packed_elements && !benchmark_mode && frame%60==30) {
            if(FILE *f=std::fopen("sdmc:/3ds/daytona/gpu-packed-elements.log","a")) {
                std::fprintf(f,"frame=%llu draws=%u\n",(unsigned long long)frame,packed_element_draws);
                std::fclose(f);
            }
        }
        if(replay_stereo && !benchmark_mode && frame%60==30) {
            if(FILE *f=std::fopen("sdmc:/3ds/daytona/gpu-replay.log","a")) {
                std::fprintf(f,"frame=%llu chunks=%llu words=%llu\n",(unsigned long long)frame,
                    (unsigned long long)replayed_chunks,(unsigned long long)replayed_words);
                std::fclose(f);
            }
        }
        if(timing_active) {
            const double ms=1000.0/SYSCLOCK_ARM11;
            if(FILE *pool=std::fopen("sdmc:/3ds/daytona/gpu-pool-profile.log","a")) {
                std::fprintf(pool,"frame=%llu pool_ms=%.6f eviction_ms=%.6f queries=%u failures=%u evictions=%u pool_probes=%llu eviction_probes=%llu cache_entries=%lu cache_bytes=%lu linear_free=%lu vram_free=%lu heap_checks=%u heap_refreshes=%u heap_builds=%u heap_capacity=%lu\n",
                    (unsigned long long)frame,pool_ticks*ms,eviction_ticks*ms,pool_queries,pool_failures,evictions,
                    (unsigned long long)pool_probes,(unsigned long long)eviction_probes,
                    (unsigned long)cache.size(),(unsigned long)cache_bytes,
                                        (unsigned long)linearSpaceFree(),(unsigned long)vramSpaceFree(),
                    eviction_checks,eviction_refreshes,eviction_builds,(unsigned long)eviction_heap.capacity());
                std::fclose(pool);
            }
            if(FILE *groups=std::fopen("sdmc:/3ds/daytona/gpu-group-profile.log","a")) {
                std::fprintf(groups,"frame=%llu enabled=%d polygons=%u calls=%u saved_draws=%u index_bytes=%u three_textures=%d opaque_groups=%d opaque_polygons=%u selectors=%u mixed_groups=%u\n",
                    (unsigned long long)frame,adjacent_groups || three_textures,grouped_polygons,grouped_calls,
                    2*(grouped_polygons-grouped_calls),index_cursor*unsigned(sizeof(u16)),three_textures,opaque_color_groups,opaque_group_polygons,opaque_selector_mask,opaque_mixed_groups);
                std::fclose(groups);
            }
            FILE *f=std::fopen("sdmc:/3ds/daytona/gpu-detail.log","a");
            if(f){
                std::fprintf(f,"frame=%llu total_ms=%.3f wait_ms=%.3f setup_ms=%.3f upload_ms=%.3f geometry_ms=%.3f material_ms=%.3f decode_ms=%.3f batch_ms=%.3f draw_ms=%.3f composite_ms=%.3f previous_gpu_chunk_ms=%.3f material_lookups=%u material_hits=%u state_calls=%u combiner_updates=%u texture_binds=%u scissor_calls=%u scissor_updates=%u\n",
                    (unsigned long long)frame,counters.cpu_ms,(after_wait-started)*ms,(upload_begin-after_wait)*ms,
                    (upload_end-upload_begin)*ms,geometry_ticks*ms,material_ticks*ms,decode_ticks*ms,batch_ticks*ms,draw_ticks*ms,composite_ticks*ms,previous_gpu_chunk_ms,material_lookups,material_hits,state_calls,combiner_updates,texture_binds,scissor_calls,scissor_updates);
                std::fclose(f);
            }
        }
    }
    void capture(const char *left,const char *right) {
        // Wait for the GPU transfer; capture the actual completed GPU framebuffer.
        C3D_FrameBegin(0);C3D_FrameEnd(0);
        // No GPU readers remain. Reclaim sampled material storage for readback;
        // the next render rebuilds the cache from unchanged ROM data.
        clear_fast_materials();invalidate_draw_state();
        while (!cache.empty()) erase(cache.begin());
        for(unsigned e=0;e<2;++e) {
            u8 *fb=texture_pool; // Completed cache was cleared above; reuse its first range.
            if(!fb)throw std::runtime_error("Cannot allocate GPU capture buffer");
            C3D_SyncDisplayTransfer(static_cast<u32*>(target[e]->frameBuf.colorBuf),GX_BUFFER_DIM(240,400),
                reinterpret_cast<u32*>(fb),GX_BUFFER_DIM(240,400),TransferFlags);
            GSPGPU_InvalidateDataCache(fb,400*240*4);
            FILE *f=std::fopen(e?right:left,"wb");if(!f){continue;}
            std::fprintf(f,"P6\n400 240\n255\n");
            for(unsigned y=0;y<240;++y)for(unsigned x=0;x<400;++x){const size_t i=(x*240+239-y)*4;const u8 rgb[]={fb[i+3],fb[i+2],fb[i+1]};std::fwrite(rgb,1,3,f);}
            std::fclose(f);
        }
    }
};
ThreeDsGpu::ThreeDsGpu():impl_(std::make_unique<Impl>()){}
ThreeDsGpu::~ThreeDsGpu()=default;
void ThreeDsGpu::render(const rt::Video &v,int w,float e,float c,bool h,float s){impl_->render(v,w,e,c,h,s);}
void ThreeDsGpu::set_texture_quality(unsigned quality){impl_->quarter_textures=quality==2;}
const ThreeDsGpu::Stats &ThreeDsGpu::stats()const{return impl_->counters;}
void ThreeDsGpu::capture(const char *l,const char *r){impl_->capture(l,r);}
}
