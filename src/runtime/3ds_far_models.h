#pragma once
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

namespace rt::far_models_3ds {
using Point=std::array<float,3>;
struct Model {
    Point minimum{},maximum{};
    std::array<Point,4> tail{},sort_vertices{};
    uint32_t texture_words=0,header_words=0,polygons=0,distance_culled=0;
    uint32_t sort_mode=0,sort_count=0,end_attribute=0,end_index=9;
    bool valid=false;
};
// ROM geometry is immutable. RAM and callback-backed geometry never enter this cache.
struct Entry {
    const uint32_t *rom=nullptr;
    uint32_t mask=0,address=0,count=0;
    Model model;
};
inline std::array<Entry,512> cache;
inline void clear_cache() { for(auto &e:cache) e.rom=nullptr; }
inline Model decode(const uint32_t *rom,uint32_t mask,uint32_t address,uint32_t count) {
    Model m;
    m.minimum.fill(std::numeric_limits<float>::infinity());
    m.maximum.fill(-std::numeric_limits<float>::infinity());
    bool finite=true;
    auto word=[&](){return rom[(address++)&mask];};
    auto point=[&](){
        Point p;
        for(unsigned axis=0;axis<3;++axis) {
            p[axis]=std::bit_cast<float>(word());
            finite=finite && std::isfinite(p[axis]);
            if(p[axis]<m.minimum[axis])m.minimum[axis]=p[axis];
            if(p[axis]>m.maximum[axis])m.maximum[axis]=p[axis];
        }
        return p;
    };
    Point previous0=point(),previous1=point(),third{},fourth{};
    // Bound malformed or unexpectedly huge models; the original parser remains the fallback.
    for(uint32_t i=0;i<count && i<65536;++i) {
        const uint32_t attr=word(); address+=3; // The normal is read even for a terminator.
        if(!(attr&3)) {m.end_attribute=attr&0x3ffff;m.end_index=0;break;}
        third=point();
        const bool quad=attr&1;
        if(quad)fourth=point();
        else {address+=3;fourth=third;} // Native triangle processing also copies P0(n) into P1(n).
        const unsigned vertices=quad?4:3;
        ++m.polygons;m.texture_words+=vertices*2;
        int32_t header_delta=(attr>>12)&31;
        if(header_delta&16)header_delta|=-16;
        m.header_words+=uint32_t(header_delta*4);
        if((attr&(1u<<17)) && ((attr>>8)&3))++m.distance_culled;
        if(const unsigned sort=(attr>>10)&3) {
            m.sort_mode=sort;m.sort_count=vertices;
            m.sort_vertices={previous1,previous0,third,fourth};
        }
        switch((attr>>8)&3) {
        case 0:case 2:previous0=third;previous1=fourth;break;
        case 1:previous1=third;break;
        case 3:previous0=fourth;break;
        }
        if(i==65535 && i+1<count)return Model{};
    }
    m.tail={previous0,previous1,third,fourth};
    m.valid=finite && m.polygons>0;
    return m;
}
inline const Model &lookup(const uint32_t *rom,uint32_t mask,uint32_t address,uint32_t count) {
    const uint32_t hash=(address*0x9e3779b9u)^(count*0x85ebca6bu);
    auto &entry=cache[(hash^(hash>>16))&(cache.size()-1)];
    if(entry.rom!=rom || entry.mask!=mask || entry.address!=address || entry.count!=count) {
        entry.rom=rom;entry.mask=mask;entry.address=address;entry.count=count;
        entry.model=decode(rom,mask,address,count);
    }
    return entry.model;
}
inline bool entirely_beyond(const Model &model,const float *matrix,float distance) {
    if(!model.valid || !(distance>0) || !std::isfinite(distance))return false;
    float low[3],high[3];
    for(unsigned axis=0;axis<3;++axis) {
        const float coefficient=matrix[axis*3+2];
        if(!std::isfinite(coefficient))return false;
        low[axis]=coefficient<0?model.maximum[axis]:model.minimum[axis];
        high[axis]=coefficient<0?model.minimum[axis]:model.maximum[axis];
    }
    // Each operation is monotone in its inputs. Use the parser's operation order
    // and its exact 24-bit quantization, with no approximate bounding epsilon.
    const float minimum=(low[0]*matrix[2])+(low[1]*matrix[5])+(low[2]*matrix[8])+matrix[11];
    const float maximum=(high[0]*matrix[2])+(high[1]*matrix[5])+(high[2]*matrix[8])+matrix[11];
    if(!std::isfinite(minimum) || !std::isfinite(maximum))return false;
    const float quantized=std::bit_cast<float>(std::bit_cast<uint32_t>(minimum)&0xffffff00u);
    return quantized>distance;
}
}