#pragma once
#if defined(__3DS__)
#include <cstdint>
extern "C" unsigned long long svcGetSystemTick(void);
namespace rt::geo_overlap_3ds {
inline bool active=false;
inline uint64_t geometry_start=0, geometry_ticks=0, tail_start=0, tail_ticks=0, first_feedback_ticks=0;
inline unsigned feedback_reads=0;
inline void begin(bool enabled) {
    active=enabled;
    geometry_start=geometry_ticks=tail_start=tail_ticks=first_feedback_ticks=0;
    feedback_reads=0;
}
inline void geometry_begin() { if(active) geometry_start=svcGetSystemTick(); }
inline void geometry_end() {
    if(active) { tail_start=svcGetSystemTick();geometry_ticks=tail_start-geometry_start; }
}
inline void feedback() {
    if(active && tail_start && !feedback_reads++) first_feedback_ticks=svcGetSystemTick()-tail_start;
}
inline void frame_end() { if(active && tail_start) tail_ticks=svcGetSystemTick()-tail_start; }
}
#endif