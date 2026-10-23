#pragma once
#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace app {
// Preserve the original aligned first-fit placement. Each node stores the
// largest free block order below it (0=full), allowing a bounded tree search.
class ThreeDsTexturePoolIndex {
public:
    static constexpr size_t Blocks=32768; // 4 MiB / 128-byte GPU alignment.
    ThreeDsTexturePoolIndex() {
        for(size_t first=1, order=16;first<2*Blocks;first*=2,--order)
            std::fill(nodes_.begin()+first,nodes_.begin()+2*first,uint8_t(order));
    }
    size_t find(size_t blocks) const {
        const unsigned order=checked_order(blocks);
        const unsigned required=order+1;
        if(nodes_[1]<required) return Blocks;
        size_t node=1;
        const size_t first=Blocks>>order;
        while(node<first) {
            node*=2;
            if(nodes_[node]<required) ++node;
        }
        return (node-first)<<order;
    }
    void set_used(size_t base,size_t blocks,bool used) {
        const unsigned order=checked_order(blocks);
        if(base>=Blocks || base%blocks || blocks>Blocks-base)
            throw std::runtime_error("Invalid texture pool range");
        size_t node=(Blocks+base)>>order;
        unsigned full_order=order+1;
        if(used && nodes_[node]!=full_order)
            throw std::runtime_error("Texture pool allocation overlaps");
        nodes_[node]=used?0:uint8_t(full_order);
        while(node>1) {
            node/=2;
            const auto left=nodes_[node*2],right=nodes_[node*2+1];
            nodes_[node]=(left==full_order && right==full_order)?
                uint8_t(full_order+1):std::max(left,right);
            ++full_order;
        }
    }
private:
    static unsigned checked_order(size_t blocks) {
        if(!blocks || blocks>Blocks || (blocks&(blocks-1)))
            throw std::runtime_error("Invalid texture pool block size");
        return std::countr_zero(blocks);
    }
    std::array<uint8_t,2*Blocks> nodes_{};
};
}