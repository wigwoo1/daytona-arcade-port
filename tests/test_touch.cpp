#include "app/touch_controls.h"
#include <cstdio>
#include <cmath>
int main() {
    app::Controls c; c.set_defaults(); app::TouchControls touch; int errors=0;
    auto check=[&](bool ok,const char *n){if(!ok){++errors;std::printf("FAIL: %s\n",n);}};
    touch.down(1,0.26f,0.75f,true); touch.down(2,0.88f,0.80f,true); touch.apply(c);
    auto in=c.sample(nullptr,nullptr);
    check(in.steer>0xd0 && in.accel==0xe0,"simultaneous steering and accelerator");
    touch.down(3,0.69f,0.85f,true);touch.apply(c);check(c.sample(nullptr,nullptr).brake==0xe0,"third finger brake");
    touch.up(2);touch.apply(c);in=c.sample(nullptr,nullptr);check(in.accel==0x20 && in.brake==0xe0,"independent finger release");
    touch.motion(1,0.025f,0.75f);touch.apply(c);check(c.sample(nullptr,nullptr).steer<0x30,"steering drag crosses center");
    touch.clear();touch.down(10,0.07f,0.06f,true);touch.apply(c);check(!(c.sample(nullptr,nullptr).in0&1),"touch coin");
    touch.clear();touch.down(11,0.18f,0.06f,true);touch.apply(c);check(!(c.sample(nullptr,nullptr).in0&0x10),"touch Start");
    for(int i=0;i<4;++i){touch.clear();touch.down(12,0.35f+i*0.09f,0.06f,true);touch.apply(c);in=c.sample(nullptr,nullptr);check(i==3?!(in.in1&1):!(in.in0&(0x20<<i)),"all four touch cameras");}
    touch.clear();c.set_defaults();touch.down(13,0.47f,0.89f,true);touch.apply(c);c.sample(nullptr,nullptr);c.sample(nullptr,nullptr);check(c.gear==2,"held touch Gear+ shifts once");
    check(touch.down(14,0.83f,0.06f,true)==app::TouchControls::Toggle,"overlay toggle always reachable");touch.apply(c);
    for(float v:c.touch)check(v==0,"hide clears held controls");
    touch.down(15,0.88f,0.80f,false);touch.apply(c);check(c.touch[app::Accelerate]==0,"hidden overlay cannot activate pedal");
    check(touch.down(16,0.83f,0.06f,false)==app::TouchControls::Toggle,"hidden overlay can be restored");
    check(touch.down(17,0.94f,0.06f,false)==app::TouchControls::Menu,"menu reachable while hidden");
    touch.clear();touch.down(18,0.88f,0.80f,true);touch.motion(18,0.5f,0.5f);touch.apply(c);check(c.touch[app::Accelerate]==0,"sliding off pedal releases it");
    bool keys[SDL_SCANCODE_COUNT]={};keys[SDL_SCANCODE_UP]=true;
    check(c.sample(keys,nullptr).accel==0xe0,"physical input remains usable with overlay hidden");
    std::printf("Touch tests: %d failures\n",errors);return errors?1:0;
}
