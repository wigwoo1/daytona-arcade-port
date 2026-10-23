#pragma once
#include "runtime/video.h"
#include <memory>

namespace app {
// Native PICA200 polygon rendering; the shared runtime remains authoritative
// for game state, projection, painter order, palettes and System 24 HUD layers.
class ThreeDsGpu {
public:
    struct Stats {
        unsigned polygons = 0, vertices = 0, textures = 0, uploads = 0, batches = 0;
        unsigned tile_uploads = 0, tile_commands = 0;
        size_t texture_bytes = 0;
        unsigned revision_resets = 0;
        float cpu_ms = 0, gpu_ms = 0, command_usage = 0;
        float wait_ms = 0, upload_ms = 0, geometry_ms = 0, composite_ms = 0;
    };
    ThreeDsGpu();
    ~ThreeDsGpu();
    ThreeDsGpu(const ThreeDsGpu &) = delete;
    ThreeDsGpu &operator=(const ThreeDsGpu &) = delete;
    void render(const rt::Video &video, int width, float eye, float convergence, bool half_textures, float scale);
    void set_texture_quality(unsigned quality);
    const Stats &stats() const;
    void capture(const char *left_path, const char *right_path);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
