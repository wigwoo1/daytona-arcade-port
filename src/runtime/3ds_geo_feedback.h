#pragma once
#if defined(__3DS__)
#include <cstdint>
namespace rt::geo_feedback_3ds {
struct Counts {
    uint64_t frames=0, read_frames=0, parsed_frames=0, before=0, after=0;
    uint32_t first_pc=0, last_pc=0, minimum=UINT32_MAX, maximum=0;
};
inline Counts counts;
inline bool enabled=false, parsed=false, read_this_frame=false;
inline void begin_frame(){if(enabled){parsed=false;read_this_frame=false;}}
inline void mark_parsed(){if(enabled){if(!parsed)++counts.parsed_frames;parsed=true;}}
inline void record(uint32_t pc,uint32_t value){
    if(!enabled)return;
    if(!read_this_frame){++counts.read_frames;read_this_frame=true;}
    if(counts.before+counts.after==0)counts.first_pc=pc;
    counts.last_pc=pc;
    if(value<counts.minimum)counts.minimum=value;
    if(value>counts.maximum)counts.maximum=value;
    if(parsed)++counts.after;else ++counts.before;
}
}
#endif