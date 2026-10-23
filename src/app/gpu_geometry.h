#pragma once
#include "runtime/video.h"
#include <SDL3/SDL.h>
#include <array>
#include <span>
#include <unordered_map>

namespace app {
// The CPU emits the reference polygon coverage; SDL compute shades the
// geometry, resolves arcade priority/depth, and composes native tile layers.
class GpuGeometry {
public:
    explicit GpuGeometry(SDL_GPUDevice *device, std::span<const uint8_t> private_shader = {});
    ~GpuGeometry();
    bool available() const { return pipeline_ != nullptr; }
    size_t replacement_count() const { return replacement_candidates_.size(); }
    size_t replacement_spans() const { return replacement_spans_; }
    bool render(SDL_GPUCommandBuffer *cmd, SDL_GPUTexture *target, const rt::Video &video,
                int width, int height, bool depth, int texture_quality, bool diagnose_repaint = false,
                bool experiment_repaint_detail = false);
private:
    SDL_GPUDevice *device_;
    SDL_GPUComputePipeline *pipeline_ = nullptr;
    SDL_GPUBuffer *buffer_ = nullptr;
    SDL_GPUBuffer *replacement_buffer_ = nullptr;
    SDL_GPUTransferBuffer *replacement_upload_ = nullptr;
    std::vector<uint32_t> replacement_pixels_;
    struct Replacement { uint32_t id; uint64_t source_hash; };
    std::unordered_multimap<uint64_t,Replacement> replacement_candidates_;
    std::unordered_map<uint64_t,uint32_t> replacements_;
    uint64_t matched_texture_revision_ = UINT64_MAX;
    const uint32_t *matched_texture_base_ = nullptr;
    uint32_t match_replacement(uint64_t key, const rt::VideoMem &memory);
    bool replacement_uploaded_ = false;
    bool replacement_pack_checked_ = false;
    size_t replacement_spans_ = 0;
    void load_replacements(bool include_pack);
    SDL_GPUTransferBuffer *upload_ = nullptr;
    uint32_t capacity_ = 0;
    rt::Raster coverage_;
    std::vector<std::vector<std::array<uint32_t,24>>> rows_;
    std::vector<uint32_t> packet_;
};
}
