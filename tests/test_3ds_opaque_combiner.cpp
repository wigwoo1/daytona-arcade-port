// Evaluate the production configuration's real SDK descriptor words with an
// independent fixed-point TEV interpreter, then compare original material RGB.
#include <cstdint>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define BIT(n) (1u<<(n))
#include <3ds/gpu/gpu.h>
#include <c3d/texenv.h>
#include "app/3ds_opaque_combiner.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
using Color=std::array<int,4>;
static int failures=0,cases=0;
static void check(bool ok,const char *why) {
    if(!ok){if(failures<8)std::printf("FAIL %s\n",why);++failures;}
}
static Color source(unsigned src,const Color &primary,const std::array<Color,3> &tex,
                    const Color &previous,u32 constant) {
    switch(src) {
    case GPU_PRIMARY_COLOR:return primary;
    case GPU_TEXTURE0:return tex[0];
    case GPU_TEXTURE1:return tex[1];
    case GPU_TEXTURE2:return tex[2];
    case GPU_PREVIOUS:return previous;
    case GPU_CONSTANT:return {int(constant&255),int((constant>>8)&255),int((constant>>16)&255),int(constant>>24)};
    default:check(false,"unsupported source");return {};
    }
}
static int combine(unsigned func,int a,int b,int c) {
    switch(func) {
    case GPU_REPLACE:return a;
    case GPU_SUBTRACT:return a-b;
    case GPU_INTERPOLATE:return (a*c+b*(255-c))/255;
    default:check(false,"unsupported function");return 0;
    }
}
static Color evaluate(const C3D_TexEnv (&env)[4],const Color &primary,const std::array<Color,3> &tex) {
    Color previous{};
    for(const auto &s:env) {
        Color next{};
        for(unsigned ch=0;ch<4;++ch) {
            const unsigned sources=ch==3?s.srcAlpha:s.srcRgb;
            const unsigned operands=ch==3?s.opAlpha:s.opRgb;
            int args[3];
            for(unsigned k=0;k<3;++k) {
                const auto in=source((sources>>(4*k))&15,primary,tex,previous,s.color);
                const unsigned op=(operands>>(4*k))&15;
                if(ch==3) {check(op==GPU_TEVOP_A_SRC_ALPHA,"alpha operand");args[k]=in[3];}
                else if(op==GPU_TEVOP_RGB_SRC_COLOR)args[k]=in[ch];
                else if(op==GPU_TEVOP_RGB_SRC_ALPHA)args[k]=in[3];
                else {check(false,"RGB operand");args[k]=0;}
            }
            const unsigned scale=ch==3?s.scaleAlpha:s.scaleRgb;
            next[ch]=std::clamp(combine(ch==3?s.funcAlpha:s.funcRgb,args[0],args[1],args[2])*(1<<scale),0,255);
        }
        previous=next;
    }
    return previous;
}
int main() {
    C3D_TexEnv env[4];C3D_TexEnv *stages[]={env,env+1,env+2,env+3};
    app::three_ds_configure_opaque_combiner(stages);
    u32 seed=0x86421;
    auto random_byte=[&](){seed=seed*1664525u+1013904223u;return int(seed>>24);};
    for(unsigned slot=0;slot<4;++slot) {
        for(unsigned i=0;i<16384;++i) {
            Color primary{random_byte(),random_byte(),random_byte(),app::three_ds_opaque_slot_alpha(slot)};
            std::array<Color,3> tex;
            for(auto &t:tex)t={random_byte(),random_byte(),random_byte(),255};
            Color expected=slot<3?tex[slot]:primary;expected[3]=255;
            for(int drift=-2;drift<=2;++drift) { auto rounded=primary;rounded[3]=std::clamp(primary[3]+drift,0,255);check(evaluate(env,rounded,tex)==expected,"selected original RGB with selector rounding");++cases; }
        }
    }
    // Exercise every byte value in each selected channel, including extremes.
    for(unsigned slot=0;slot<4;++slot)for(unsigned channel=0;channel<3;++channel)for(int value=0;value<256;++value) {
        Color primary{0,255,127,app::three_ds_opaque_slot_alpha(slot)};
        std::array<Color,3> tex={Color{255,0,127,255},Color{127,255,0,255},Color{0,127,255,255}};
        if(slot<3)tex[slot][channel]=value;else primary[channel]=value;
        Color expected=slot<3?tex[slot]:primary;expected[3]=255;
        check(evaluate(env,primary,tex)==expected,"exhaustive selected channel");++cases;
    }
    check(app::three_ds_opaque_material(0,false),"solid eligible");
    check(app::three_ds_opaque_material(2,false),"opaque texture eligible");
    check(!app::three_ds_opaque_material(1,false),"disabled renderer rejected");
    check(!app::three_ds_opaque_material(3,false),"transparent texture rejected");
    for(int renderer=0;renderer<4;++renderer)check(!app::three_ds_opaque_material(renderer,true),"checker rejected");
    check(app::three_ds_opaque_material(3,false,true),"fully opaque sampled transparent-capable texture eligible");
    check(!app::three_ds_opaque_material(3,true,true),"sample opacity does not bypass checker blending");
    for(unsigned value=0;value<65536;++value) {
        u16 pixels[]={u16(0xffff),u16(value),u16(0xffff)};
        check(app::three_ds_texture_is_opaque(pixels,3,1u)==bool(value&1u),"every 5551 value opacity matches stored alpha");
    }
    for(unsigned alpha=0;alpha<256;++alpha)for(unsigned location=0;location<3;++location) {
        u32 pixels[]={0xffffffffu,0x00ffffffu,0x13579bffu};
        pixels[location]=(pixels[location]&0xffffff00u)|alpha;
        check(app::three_ds_texture_is_opaque(pixels,3,255u)==(alpha==255),"RGBA8 partial or absent alpha cannot enter opaque path");
    }
    std::printf("opaque TEV: %d colour cases plus 66304 opacity cases, failures=%d\n",cases,failures);
    return failures?1:0;
}