#include "app/config.h"

#include <algorithm>

#include <SDL3/SDL.h>

#include <cstdlib>
#include <fstream>
#include <filesystem>

namespace app {

std::string Config::data_file(const char *name) {
#ifdef __ANDROID__
    const std::filesystem::path base(SDL_GetAndroidInternalStoragePath());
    std::filesystem::create_directories(base / "settings");
    return (base / "settings" / name).string();
#else
    const std::filesystem::path base(SDL_GetBasePath());
    if (std::filesystem::exists(base / "portable.txt")) {
        std::error_code ec;
        std::filesystem::create_directories(base / "settings", ec);
        if (!ec) return (base / "settings" / name).string();
    }
    char *pref = SDL_GetPrefPath("daytona-recomp", "daytona93");
    std::string p = pref ? std::string(pref) + name : std::string(name);
    SDL_free(pref);
    return p;
#endif
}
std::string Config::path() { return data_file("launcher.ini"); }

void Config::load() {
    std::ifstream f(path());
    std::string line;
    while (std::getline(f, line)) {
        const auto eq = line.find('=');
        if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "rom") rom_path = v;
        else if (k == "gpu") gpu = v;
        else if (k == "fullscreen") fullscreen = v == "1";
        else if (k == "resolution") resolution = std::clamp(std::atoi(v.c_str()), 0, resolution_count - 1);
        else if (k == "fps") { int n = std::atoi(v.c_str()); fps = n == 60 || n == 120 || n == 240 ? n : 0; }
        else if (k == "render_threads") render_threads = std::clamp(std::atoi(v.c_str()), 0, 32);
        else if (k == "vsync") vsync = v == "1";
        else if (k == "require_start") require_start = v == "1";
        else if (k == "pending_coins") pending_coins = std::clamp(std::atoi(v.c_str()), 0, 999);
        else if (k == "touch_overlay") touch_overlay = v == "1";
        else if (k == "widescreen") widescreen = v == "1";
        else if (k == "low_textures") low_textures = v == "1";
        else if (k == "high_resolution") high_resolution = v == "1";
        else if (k == "interpolate") interpolate = v == "1";
        else if (k == "depth_buffer") depth_buffer = v == "1";
        else if (k == "gpu_geometry") gpu_geometry = v == "1";
        else if (k == "texture_quality") {
            int percent = std::atoi(v.c_str());
            texture_quality = percent == 300 || percent == 50 || percent == 25 || percent == 10 ? percent : 100;
        }
        else if (k == "model_lod") model_lod = std::clamp(std::atoi(v.c_str()), 0, 2);
        else if (k == "track_extension") track_extension = std::clamp(std::atoi(v.c_str()), 0, 2);
        else if (k == "nearest") nearest = v == "1";
        else if (k == "show_stats") show_stats = v == "1";
        else if (k == "volume") volume = std::clamp(std::strtof(v.c_str(), nullptr), 0.0f, 1.0f);
        else if (k == "mute") mute = v == "1";
        else if (k == "deadzone") controls.deadzone = std::strtof(v.c_str(), nullptr);
        else if (k == "steer_invert") controls.steer_invert = v == "1";
        else
            for (int a = 0; a < kNumActions; a++) {
                const std::string base = action_key(Action(a));
                if (k == base + ".key") controls.bind[a].key = v.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(v.c_str());
                else if (k == base + ".pad") controls.bind[a].pad = PadInput::parse(v);
            }
    }
    apply_texture_requirements();
#ifdef __ANDROID__
    std::ifstream pending(data_file("pending_coins.txt"));
    int saved = 0; if (pending >> saved) pending_coins = std::clamp(saved, 0, 999);
#endif
}

void Config::save() const {
    std::ofstream f(path());
    f << "# Daytona USA launcher settings\n";
    f << "rom=" << rom_path << "\n";
    f << "gpu=" << gpu << "\n";
    f << "fullscreen=" << (fullscreen ? 1 : 0) << "\n";
    f << "resolution=" << resolution << "\n";
    f << "fps=" << fps << "\n";
    f << "render_threads=" << render_threads << "\n";
    f << "vsync=" << vsync << "\n";
    f << "require_start=" << require_start << "\n";
    f << "pending_coins=" << pending_coins << "\n";
    f << "touch_overlay=" << touch_overlay << "\n";
    f << "widescreen=" << widescreen << "\n";
    f << "high_resolution=" << high_resolution << "\n";
    f << "low_textures=" << low_textures << "\n";
    f << "interpolate=" << interpolate << "\n";
    f << "depth_buffer=" << depth_buffer << "\n";
    f << "gpu_geometry=" << gpu_geometry << "\n";
    f << "texture_quality=" << texture_quality << "\n";
    f << "model_lod=" << model_lod << "\n";
    f << "track_extension=" << track_extension << "\n";
    f << "nearest=" << nearest << "\n";
    f << "show_stats=" << show_stats << "\n";
    f << "volume=" << volume << "\n";
    f << "mute=" << (mute ? 1 : 0) << "\n";
    f << "deadzone=" << controls.deadzone << "\n";
    f << "steer_invert=" << (controls.steer_invert ? 1 : 0) << "\n";
    for (int a = 0; a < kNumActions; a++) {
        const Binding &b = controls.bind[a];
        f << action_key(Action(a)) << ".key=" << (b.key == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(b.key)) << "\n";
        f << action_key(Action(a)) << ".pad=" << b.pad.save() << "\n";
    }
}

} // namespace app
