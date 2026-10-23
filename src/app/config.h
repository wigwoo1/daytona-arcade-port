// Launcher settings, saved to launcher.ini in the user's data folder (SDL's
// pref path): the ROM set, the GPU backend, fullscreen, audio volume, and
// the control bindings. Plain key=value lines, so it can be edited by hand.
#pragma once

#include "app/controls.h"

#include <string>

namespace app {

struct Resolution { const char *name; int width, height; };
inline constexpr Resolution resolutions[] = {
    {"240p - 320 x 240", 320, 240},
    {"360p - 640 x 360", 640, 360},
    {"Arcade - 496 x 384", 496, 384},
    {"480p - 640 x 480", 640, 480},
    {"720p - 1280 x 720", 1280, 720},
    {"900p - 1600 x 900", 1600, 900},
    {"1080p - 1920 x 1080", 1920, 1080},
    {"1440p - 2560 x 1440", 2560, 1440},
    {"4K - 3840 x 2160", 3840, 2160},
    {"3DS top screen - 400 x 240", 400, 240}
};
inline constexpr int resolution_count = sizeof(resolutions) / sizeof(resolutions[0]);
struct Config {
    std::string rom_path;
    std::string gpu;          // "" (automatic), vulkan, direct3d12, metal
    bool fullscreen = false;
    int resolution = 4; // 720p output, native internal rendering by default
    int fps = 0;        // no software cap; or 60, 120 or 240 target
    bool vsync = true;
    int render_threads = 0; // automatic; 1 for serial reference
    bool require_start = false; // optional coin queue; original arcade behavior by default
    int pending_coins = 0;
    bool widescreen = false;
    bool high_resolution = false;
    bool low_textures = false; // presentation-only half-size mipmaps
    bool interpolate = false;
    bool depth_buffer = false;
    bool gpu_geometry = false;
    int texture_quality = 100;
    int track_extension = 0;
    int model_lod = 0; // Original, lowest car body, highest car body
    bool nearest = false;
    bool show_stats = false;
    float volume = 0.8f;      // 0..1
    bool mute = false;
#ifdef __ANDROID__
    bool touch_overlay = true;
#else
    bool touch_overlay = false;
#endif
    Controls controls;

    Config() {
        controls.set_defaults();
#ifdef __ANDROID__
        fullscreen = true; resolution = 4; fps = 60; render_threads = 4;
        require_start = true; widescreen = true;
#endif
    }
    static std::string data_file(const char *name);
    static std::string path();  // <pref path>/launcher.ini
    void apply_texture_requirements() {
#ifndef __ANDROID__
        if (texture_quality == 300) {
            gpu = "vulkan";
            gpu_geometry = true;
            interpolate = false;
            low_textures = false;
        }
#endif
    }
    void load();
    void save() const;
};

} // namespace app
