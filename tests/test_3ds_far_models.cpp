#include "runtime/geo.h"
#include "runtime/3ds_geo_options.h"
#include "runtime/3ds_far_models.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
extern "C" unsigned long long svcGetSystemTick(void){return 0;}
static uint32_t bits(float x){return std::bit_cast<uint32_t>(x);}
static void require(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
static bool same(const rt::GeoPoly &a,const rt::GeoPoly &b){
    return a.z==b.z && a.luma==b.luma && a.texlod==b.texlod && a.window==b.window &&
        a.reverse==b.reverse && a.num_vertices==b.num_vertices &&
        !std::memcmp(a.texheader,b.texheader,sizeof a.texheader) &&
        !std::memcmp(a.viewport,b.viewport,sizeof a.viewport) && !std::memcmp(a.center,b.center,sizeof a.center) &&
        !std::memcmp(a.v,b.v,a.num_vertices*sizeof a.v[0]);
}
static void compare(const rt::Geo &a,const rt::Geo &b){
    auto lists=[](const auto &x,const auto &y){return x.size()==y.size() && std::equal(x.begin(),x.end(),y.begin(),same);};
    require(lists(a.polys,b.polys),"native output differs");
    require(lists(a.wide_polys,b.wide_polys),"wide output differs");
    require(lists(a.unclipped_polys,b.unclipped_polys),"unclipped output differs");
    require(a.render_distance_culled()==b.render_distance_culled(),"distance counter differs");
}
int main(){try{
    std::array<uint32_t,4096> model{};
    std::array<uint16_t,8192> textures{};
    std::array<uint32_t,0x8000> commands{};
    uint32_t random=0x521ca972;
    auto next=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    auto scalar=[&](){return (int(next()%2049)-1024)/1024.f;};
    unsigned cases=0;
    for(unsigned seed=0;seed<96;++seed){
        rt::far_models_3ds::clear_cache();
        size_t at=0;
        auto raw_point=[&](bool padding=false){for(unsigned v=0;v<3;++v)model[at++]=padding?0x7fc12345u:bits(scalar());};
        raw_point();raw_point();
        const unsigned polygons=1+next()%32;
        for(unsigned i=0;i<polygons;++i){
            const bool quad=next()&1;
            model[at++]=(next()&0xfffffffc)|(quad?1:2)|((next()%4)<<8); // All sort/header/link/double-side cases.
            model[at++]=bits(0);model[at++]=bits(0);model[at++]=bits(-1);
            raw_point();raw_point(!quad);
        }
        model[at++]=0x1400;raw_point(true); // Nonzero bits in a terminating attribute are retained.
        const unsigned count=seed%3==0?0:seed%3==1?polygons+1:1+next()%polygons;
        for(unsigned mode=0;mode<4;++mode)for(bool wide:{false,true}){
            size_t cursor=0;
            auto word=[&](uint32_t v){commands[cursor++]=v;};
            auto op=[&](unsigned v){word(v<<23);};
            op(11);
            for(unsigned i=0;i<9;++i)word(bits((i%4==0?1.f:0.f)+scalar()*.2f));
            word(bits(0));word(bits(0));word(bits(0));
            op(9);word(bits(100));word(bits(100));
            op(3);word(0);word((496u<<16)|384u);
            for(unsigned i=0;i<4;++i)word((248u<<16)|192u);
            op(10);word(bits(0));word(bits(0));word(bits(-1));
            op(6);word(0);word(32);
            for(unsigned i=0;i<32;++i){word(64|(128<<8)|(32<<16));word(bits(1));}
            op(7);word(mode);
            for(unsigned object=0;object<3;++object){
                op(12);word(bits(.1f));word(bits(-.1f));word(bits(object==1?3.f:100.f));
                op(1);word(object*200);word(2048+object*32);word(0x800000);word(count);
            }
            op(15);
            auto reference=std::make_unique<rt::Geo>(model.data(),model.size(),textures.data(),textures.size(),commands.data());
            auto candidate=std::make_unique<rt::Geo>(model.data(),model.size(),textures.data(),textures.size(),commands.data());
            reference->capture_widescreen=candidate->capture_widescreen=wide;
            reference->zclip_w(255);candidate->zclip_w(255); reference->set_capture_reference_depth(false);candidate->set_capture_reference_depth(false);
            const float distance=seed%7==0?0:seed%7==1?101.f:10.f;
            reference->set_render_distance(distance);candidate->set_render_distance(distance);
            reference->record_unclipped=candidate->record_unclipped=seed%11==0;
            rt::geo_options_3ds::far_model_distance=false;reference->parse(0);
            rt::geo_options_3ds::far_model_distance=true;
            rt::geo_options_3ds::verify_far_model_distance=true;candidate->parse(0);compare(*reference,*candidate);
            rt::geo_options_3ds::verify_far_model_distance=false;candidate->parse(0);compare(*reference,*candidate);
            ++cases;
        }
    }
    require(rt::geo_options_3ds::far_verified>100,"no significant original-parser oracle coverage");
    // Bounds are conservative with negative coefficients, cancellation and float quantization.
    rt::far_models_3ds::Model box;box.valid=true;box.minimum={-3,-2,-1};box.maximum={2,4,3};
    for(unsigned i=0;i<10000;++i){
        float matrix[12]{};matrix[2]=scalar()*32;matrix[5]=scalar()*32;matrix[8]=scalar()*32;matrix[11]=scalar()*256;
        const float threshold=std::fabs(scalar()*20);
        if(rt::far_models_3ds::entirely_beyond(box,matrix,threshold))for(unsigned j=0;j<8;++j){
            const float x=j&1?box.maximum[0]:box.minimum[0],y=j&2?box.maximum[1]:box.minimum[1],z=j&4?box.maximum[2]:box.minimum[2];
            const float depth=x*matrix[2]+y*matrix[5]+z*matrix[8]+matrix[11];
            require(std::bit_cast<float>(bits(depth)&0xffffff00u)>threshold,"unsafe depth bound");
        }
    }
    std::printf("Far model cases=%u objects=%llu polygons=%llu original_state_checks=%llu\n",cases,
        rt::geo_options_3ds::far_models,rt::geo_options_3ds::far_polygons,rt::geo_options_3ds::far_verified);
    std::puts("Far-model distance regression passed.");return 0;
}catch(const std::exception &e){std::fprintf(stderr,"FAILED: %s\n",e.what());return 1;}}