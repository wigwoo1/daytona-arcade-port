#include "runtime/geo.h"
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <iostream>
#include <memory>
#if defined(__3DS__)
#include "runtime/3ds_geo_options.h"
#include <cstring>
#if defined(DAYTONA_HOST_GEO_TEST)
// Geometry tests keep profiling disabled; no console clock is needed here.
extern "C" unsigned long long svcGetSystemTick(void) { return 0; }
#endif
struct ReentrantRom {
    const void *data=nullptr;
    size_t bytes=0;
    rt::Geo *nested=nullptr;
    bool fired=false;
};
static bool read_reentrant_rom(void *context,size_t offset,void *out,size_t length) {
    auto &rom=*static_cast<ReentrantRom*>(context);
    if(offset>rom.bytes || length>rom.bytes-offset) return false;
    if(rom.nested && !rom.fired) {
        rom.fired=true;
        rom.nested->parse(0);
    }
    std::memcpy(out,static_cast<const unsigned char*>(rom.data)+offset,length);
    return true;
}
#endif

static uint32_t bits(float value) { return std::bit_cast<uint32_t>(value); }
static void require(bool condition, const char *message) {
    if (!condition) { std::cerr << message << '\n'; std::exit(1); }
}
static bool same(const rt::GeoPoly &a, const rt::GeoPoly &b) {
    if (a.z != b.z || a.luma != b.luma || a.texlod != b.texlod || a.window != b.window ||
        a.reverse != b.reverse || a.num_vertices != b.num_vertices ||
        !std::equal(std::begin(a.texheader), std::end(a.texheader), b.texheader) ||
        !std::equal(std::begin(a.viewport), std::end(a.viewport), b.viewport) ||
        !std::equal(std::begin(a.center), std::end(a.center), b.center)) return false;
    for (unsigned i = 0; i < a.num_vertices; ++i) {
        if (bits(a.v[i].x) != bits(b.v[i].x) || bits(a.v[i].y) != bits(b.v[i].y)) return false;
        for (int p = 0; p < 3; ++p) if (bits(a.v[i].p[p]) != bits(b.v[i].p[p])) return false;
    }
    return true;
}
int main() {
    std::vector<uint32_t> model(4096), commands(0x8000);
    std::vector<uint16_t> textures(4096);
#if defined(__3DS__)
    // Distinct UV/header words make stale scratch values visible to comparison.
    for(unsigned i=0;i<textures.size();++i) textures[i]=uint16_t((i*137u)^0x5a39u);
#endif
    size_t pos = 0;
    auto point = [&](float x, float y, float z) { model[pos++] = bits(x); model[pos++] = bits(y); model[pos++] = bits(z); };
    point(-.5f, -.5f, 1.f); point(.5f, -.5f, 1.f);
    for (unsigned link = 0; link < 24; ++link) {
        const bool quad = link % 3 != 0;
        model[pos++] = (1u << 17) | ((link % 3 + 1) << 8) | (1u << 10) | (1u << 12) | (quad ? 1u : 2u);
        point(0, 0, -1);
        const float z = link % 5 == 0 ? .4f : 1.f + float(link) / 16;
        point(-.5f + float(link % 4) / 8, .5f, z);
        point(.5f, .5f, z);
    }
    model[pos++] = 0; point(0, 0, 0);

    for (unsigned mode = 0; mode < 4; ++mode) for (bool wide : {false, true})
        for (float distance : {0.f, .75f, 1.5f, 3.f}) {
            size_t cursor = 0;
            auto word = [&](uint32_t value) { commands[cursor++] = value; };
            auto op = [&](unsigned code) { word(code << 23); };
            op(0x0b);
            for (float value : {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f}) word(bits(value));
            op(9); word(bits(100.f)); word(bits(100.f));
            op(3); word(0); word((496u << 16) | 384u);
            for (int i = 0; i < 4; ++i) word((248u << 16) | 192u);
            op(0x0a); word(bits(0)); word(bits(0)); word(bits(-1));
            op(6); word(0); word(1); word(64u | (128u << 8) | (32u << 16)); word(bits(1));
            op(7); word(mode);
            op(1); word(0); word(1024); word(0x800000); word(25);
            op(0x0f);
            auto reference = std::make_unique<rt::Geo>(model.data(), model.size(), textures.data(), textures.size(), commands.data());
            auto batched = std::make_unique<rt::Geo>(model.data(), model.size(), textures.data(), textures.size(), commands.data());
            reference->record_pushes = true;
            reference->capture_widescreen = batched->capture_widescreen = wide;
            reference->zclip_w(255); batched->zclip_w(255);
            reference->set_render_distance(distance); batched->set_render_distance(distance);
            for (int pass = 0; pass < 3; ++pass) {
                reference->record_unclipped = batched->record_unclipped = pass == 1;
#if defined(__3DS__)
                reference->fast_clip_accept=batched->fast_clip_accept=pass==2;
                reference->verify_fast_clip=batched->verify_fast_clip=pass==2;
                rt::geo_options_3ds::direct_texture_reads=pass!=0;
                rt::geo_options_3ds::reuse_polygon_scratch=false;
                rt::geo_options_3ds::verify_polygon_scratch=false;
#endif
                reference->parse(0);
#if defined(__3DS__)
                rt::geo_options_3ds::reuse_polygon_scratch=true;
                rt::geo_options_3ds::verify_polygon_scratch=true;
#endif
                batched->parse(0);
#if defined(__3DS__)
                require(reference->native_polygon_count()==batched->native_polygon_count(),"scratch native count differs");
#endif
                require(!reference->pushed.empty(), "reference did not execute original word submissions");
                if (distance == 0) require(!reference->polys.empty(), "fixture failed to emit visible geometry");
                require(reference->polys.size() == batched->polys.size(), "batched polygon count differs");
                require(std::equal(reference->polys.begin(), reference->polys.end(), batched->polys.begin(), same), "batched polygon data differs");
                require(reference->wide_polys.size() == batched->wide_polys.size(), "widescreen polygon count differs");
                require(std::equal(reference->wide_polys.begin(), reference->wide_polys.end(), batched->wide_polys.begin(), same), "widescreen polygon data differs");
                require(reference->unclipped_polys.size() == batched->unclipped_polys.size(), "unclipped polygon count differs");
                require(std::equal(reference->unclipped_polys.begin(), reference->unclipped_polys.end(), batched->unclipped_polys.begin(), same), "unclipped polygon data differs");
                require(bits(reference->render_reference_depth()) == bits(batched->render_reference_depth()), "reference depth differs");
                require(reference->render_distance_culled() == batched->render_distance_culled(), "distance culling differs");
            }
#if defined(__3DS__)
            if(mode==1 && wide && distance==0.f) {
                auto nested=std::make_unique<rt::Geo>(model.data(),model.size(),textures.data(),textures.size(),commands.data());
                nested->capture_widescreen=true;nested->zclip_w(255);nested->fast_clip_accept=true;
                ReentrantRom model_rom{model.data(),model.size()*sizeof(uint32_t)};
                ReentrantRom texture_rom{textures.data(),textures.size()*sizeof(uint16_t),nested.get()};
                auto outer=std::make_unique<rt::Geo>(read_reentrant_rom,&model_rom,model.size(),read_reentrant_rom,&texture_rom,textures.size(),commands.data());
                outer->capture_widescreen=true;outer->zclip_w(255);outer->fast_clip_accept=true;
                outer->parse(0);
                require(texture_rom.fired,"nested geometry callback was not exercised");
                require(rt::geo_options_3ds::scratch_nested_fallbacks>0,"nested scratch fallback was not exercised");
                for(auto *actual:{outer.get(),nested.get()}) {
                    require(reference->polys.size()==actual->polys.size(),"nested native count differs");
                    require(std::equal(reference->polys.begin(),reference->polys.end(),actual->polys.begin(),same),"nested native geometry differs");
                    require(reference->wide_polys.size()==actual->wide_polys.size(),"nested wide count differs");
                    require(std::equal(reference->wide_polys.begin(),reference->wide_polys.end(),actual->wide_polys.begin(),same),"nested wide geometry differs");
                }
            }
#endif
        }
#if defined(__3DS__)
    require(rt::geo_options_3ds::scratch_poisoned_polygons>0,"scratch poisoning was not exercised");
    require(rt::geo_options_3ds::scratch_nested_fallbacks>0,"scratch nested fallback was not exercised");
    std::cout<<"Poisoned scratch polygons: "<<rt::geo_options_3ds::scratch_poisoned_polygons
             <<", nested fallbacks: "<<rt::geo_options_3ds::scratch_nested_fallbacks<<'\n';
#endif
    std::cout << "Geometry batching matches original vertex submissions\n";
}
