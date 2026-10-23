#include "app/3ds_controls.h"
#include "app/coin_gate.h"
#include "app/presentation_cadence.h"
#include "runtime/game_loop.h"
#include <cstdio>
int main() {
    int failures=0; auto check=[&](bool ok,const char* what){if(!ok){std::printf("FAIL: %s\n",what);++failures;}};
    for (unsigned limit : {0u, 30u, 40u}) {
        app::PresentationCadence cadence(rt::GameLoop::kFrameHz);
        cadence.set_limit(limit);
        unsigned displayed = 0, gap = 0;
        for (unsigned simulated = 0; simulated < 60000; ++simulated) {
            if (cadence.step()) { ++displayed; gap = 0; }
            else ++gap;
            check(gap <= 1, "display cadence never omits two consecutive simulation frames");
        }
        const double expected = limit ? 60000.0 * limit / rt::GameLoop::kFrameHz : 60000.0;
        check(std::abs(displayed - expected) <= 1, "30/40 FPS cadence preserves all simulation steps");
        cadence.set_limit(0);
        check(cadence.step(), "native presentation resumes immediately");
    }
    bool bad_limit = false;
    try { app::PresentationCadence cadence(rt::GameLoop::kFrameHz); cadence.set_limit(15); }
    catch (const std::invalid_argument&) { bad_limit = true; }
    check(bad_limit, "unsupported display limits rejected");
    app::ThreeDsControls pad;
    check(pad.sample(0,0,0).steer==128,"neutral steering");
    check(pad.sample(0,0,-156).steer==32 && pad.sample(0,0,156).steer==224,"Circle Pad extremes");
    check(pad.sample(0,0,14).steer==128 && pad.sample(0,0,-14).steer==128,"Circle Pad dead zone");
    auto in=pad.sample(3,0,0); check(in.accel==224 && in.brake==224,"independent gas and brake");
    check(!(pad.sample(4,0,0).in0&1),"Select inserts coin");
    check(!(pad.sample(8,0,0).in0&0x10),"Start confirms");
    check((pad.sample(64,0,0).in0&0xe0)==0xc0,"Up camera 1");
    check((pad.sample(16,0,0).in0&0xe0)==0xa0,"Right camera 2");
    check((pad.sample(128,0,0).in0&0xe0)==0x60,"Down camera 3");
    check(!(pad.sample(32,0,0).in1&1),"Left camera 4");
    for(int i=0;i<10;++i)pad.sample(256,256,0);
    check(pad.gear==4 && ((pad.sample(0,0,0).in1>>4)&7)==5,"top gear clamp and encoding");
    for(int i=0;i<10;++i)pad.sample(512,512,0);
    check(pad.gear==1 && ((pad.sample(0,0,0).in1>>4)&7)==2,"first gear clamp and encoding");
    pad.sample(256,0,0); check(pad.gear==1,"held shoulder does not repeat shifts");
    app::CoinGate gate; gate.sample(pad.sample(0,0,0),true);
    for(int coin=0;coin<3;++coin){for(int j=0;j<20;++j){in=gate.sample(pad.sample(4,0,0),true);check(in.in0&1,"queued coins stay in attract");}gate.sample(pad.sample(0,0,0),true);}
    check(gate.pending==3,"three distinct presses queued");
    in=gate.sample(pad.sample(8,8,0),true);check(!(in.in0&1) && gate.pending==2,"fresh Start delivers queued coins");
    int pulses=1;bool last=true;
    for(int i=0;i<100;++i){in=gate.sample(pad.sample(0,0,0),true);bool coin=!(in.in0&1);if(coin&&!last)++pulses;last=coin;}
    check(pulses==3 && gate.pending==0,"three separate coin pulses delivered");
    std::printf("3DS control tests: %d failures\n",failures);return failures?1:0;
}
