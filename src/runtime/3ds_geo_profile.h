#pragma once
#if defined(__3DS__)
#include <cstdint>
extern "C" unsigned long long svcGetSystemTick(void);
namespace rt::geo_profile_3ds {
struct Stats {
    uint64_t parse_ticks=0, object_ticks[4]{}, object_count[4]{};
    uint64_t polygons=0, polygon_samples=0, polygon_ticks=0;
    uint64_t clip_ticks[2]{}, clip_samples[2]{};
    uint64_t timer_min=UINT64_MAX,timer_max=0,timer_total=0,timer_pairs=0;
};
inline Stats stats;
inline bool active=false, dense=false;
inline void begin(bool enabled) {
    active=enabled;
    if(!enabled) return;
    stats={};
    if(dense) for(unsigned i=0;i<128;++i) {
        const uint64_t start=svcGetSystemTick(),elapsed=svcGetSystemTick()-start;
        if(elapsed<stats.timer_min) stats.timer_min=elapsed;
        if(elapsed>stats.timer_max) stats.timer_max=elapsed;
        stats.timer_total+=elapsed; ++stats.timer_pairs;
    }
}
struct Scope {
    uint64_t *destination, start;
    Scope(bool enabled,uint64_t &sum):destination(enabled?&sum:nullptr),start(enabled?svcGetSystemTick():0){}
    ~Scope(){if(destination)*destination+=svcGetSystemTick()-start;}
};
}
#endif