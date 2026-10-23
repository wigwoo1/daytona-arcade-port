#pragma once
// Include after Citro3D. Four opaque materials share one ordered draw, using
// primary alpha as a selector while primary RGB retains each solid's colour.
namespace app {
template<class Pixel>
inline bool three_ds_texture_is_opaque(const Pixel *pixels,unsigned count,unsigned alpha_mask) {
    for(unsigned i=0;i<count;++i) if((unsigned(pixels[i])&alpha_mask)!=alpha_mask) return false;
    return true;
}
inline bool three_ds_opaque_material(int renderer,bool checker,bool sampled_opaque=false) {
    return !checker && (renderer==0 || renderer==2 || (renderer==3 && sampled_opaque));
}
inline unsigned char three_ds_opaque_slot_alpha(unsigned slot) {
    return static_cast<unsigned char>(slot*85u);
}
inline void three_ds_configure_opaque_combiner(C3D_TexEnv *const stages[4]) {
    for(unsigned i=0;i<3;++i) {
        auto *env=stages[i];C3D_TexEnvInit(env);
        if(i==0) {
            C3D_TexEnvSrc(env,C3D_RGB,GPU_TEXTURE0);
        } else {
            C3D_TexEnvSrc(env,C3D_RGB,GPU_PREVIOUS,i==1?GPU_TEXTURE1:GPU_TEXTURE2,GPU_PREVIOUS);
            C3D_TexEnvOpRgb(env,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_ALPHA);
            C3D_TexEnvFunc(env,C3D_RGB,GPU_INTERPOLATE);
        }
        // Exact masks at input alpha 0/85/170/255: 4*(threshold-alpha),
        // clamped to [0,255], with ten codes of rounding margin on each side.
        C3D_TexEnvSrc(env,C3D_Alpha,GPU_CONSTANT,GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(env,C3D_Alpha,GPU_SUBTRACT);
        C3D_TexEnvScale(env,C3D_Alpha,GPU_TEVSCALE_4);
        C3D_TexEnvColor(env,0x00ffffffu|((74u+i*85u)<<24));
    }
    auto *last=stages[3];C3D_TexEnvInit(last);
    C3D_TexEnvSrc(last,C3D_RGB,GPU_PREVIOUS,GPU_PRIMARY_COLOR,GPU_PREVIOUS);
    C3D_TexEnvOpRgb(last,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_COLOR,GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(last,C3D_RGB,GPU_INTERPOLATE);
    C3D_TexEnvSrc(last,C3D_Alpha,GPU_CONSTANT);
    // Only fully opaque materials may use this final alpha.
    C3D_TexEnvColor(last,0xffffffffu);
}
}