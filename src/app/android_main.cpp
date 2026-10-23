// Android entry point: native game runtime, SDL texture presentation and multitouch UI.
#include "app/config.h"
#include "app/coin_gate.h"
#include "app/launcher.h"
#include "app/touch_controls.h"
#include "runtime/game_loop.h"
#include "runtime/rom_import.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlrenderer3.h"
#include "imgui.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
namespace {

constexpr double kArcadeHz = rt::GameLoop::kFrameHz; // 16 MHz / (656 x 424), the board's frame rate

// The sound board's two outputs (the YM3438, and the two MultiPCMs mixed) go
// to their own SDL audio streams at the chips' own rates; SDL resamples and
// mixes them on the device. The game advances on the display's clock and the
// device plays on its own, so a small speed trim on both streams holds the
// queue near kLatency instead of letting it drift into a gap or a backlog.
class Audio {
public:
    static constexpr double kLatency = 0.06; // seconds queued

    bool open(double fm_rate, double pcm_rate) {
        dev_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
        if (!dev_) return false;
        fm_rate_ = int(fm_rate + 0.5);
        const SDL_AudioSpec fm{SDL_AUDIO_F32, 2, fm_rate_}, pcm{SDL_AUDIO_F32, 2, int(pcm_rate + 0.5)};
        fm_ = SDL_CreateAudioStream(&fm, nullptr);
        pcm_ = SDL_CreateAudioStream(&pcm, nullptr);
        if (!fm_ || !pcm_ || !SDL_BindAudioStream(dev_, fm_) || !SDL_BindAudioStream(dev_, pcm_)) return false;
        return true;
    }
    void push(snd::SoundBoard &sb, float gain) {
        if (!fm_) return;
        const std::vector<float> fm = sb.take_fm(), pcm = sb.take_pcm();
        SDL_PutAudioStreamData(fm_, fm.data(), int(fm.size() * sizeof(float)));
        SDL_PutAudioStreamData(pcm_, pcm.data(), int(pcm.size() * sizeof(float)));
        const double queued = double(SDL_GetAudioStreamQueued(fm_)) / (8.0 * fm_rate_);
        if (queued > kLatency * 4) { // a stall (window drag, debugger): drop the backlog
            SDL_ClearAudioStream(fm_);
            SDL_ClearAudioStream(pcm_);
        }
        const double err = std::clamp((queued - kLatency) / kLatency, -1.0, 1.0);
        const float ratio = float(1.0 + 0.005 * err); // at most 0.5%: inaudible
        SDL_SetAudioStreamFrequencyRatio(fm_, ratio);
        SDL_SetAudioStreamFrequencyRatio(pcm_, ratio);
        SDL_SetAudioStreamGain(fm_, gain);
        SDL_SetAudioStreamGain(pcm_, gain);
    }
    void clear() {
        if (fm_) SDL_ClearAudioStream(fm_);
        if (pcm_) SDL_ClearAudioStream(pcm_);
    }
    void close() {
        if (fm_) SDL_DestroyAudioStream(fm_);
        if (pcm_) SDL_DestroyAudioStream(pcm_);
        if (dev_) SDL_CloseAudioDevice(dev_);
        fm_ = pcm_ = nullptr;
        dev_ = 0;
    }

private:
    SDL_AudioDeviceID dev_ = 0;
    SDL_AudioStream *fm_ = nullptr, *pcm_ = nullptr;
    int fm_rate_ = 1;
};

std::string pref_file(const char *name) { return app::Config::data_file(name); }

template <typename C> void load_file(const std::string &path, C &into) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::vector<uint8_t> d{std::istreambuf_iterator<char>(f), {}};
    if (d.size() == into.size()) std::copy(d.begin(), d.end(), into.begin());
}
template <typename C> void save_file(const std::string &path, const C &from) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char *>(from.data()), std::streamsize(from.size()));
}

int fail(const char *what) {
    std::fprintf(stderr, "daytona: %s: %s\n", what, SDL_GetError());
    return 1;
}

} // namespace


struct Lifecycle {
    std::recursive_mutex mutex;
    std::atomic<bool> background{false};
    std::function<void()> checkpoint;
    static bool SDLCALL watch(void *data, SDL_Event *e) {
        auto &self = *static_cast<Lifecycle *>(data);
        if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND || e->type == SDL_EVENT_TERMINATING) {
            self.background = true;
            std::lock_guard lock(self.mutex);
            self.checkpoint();
        } else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) self.background = false;
        return true;
    }
};
int main(int argc, char **argv) {
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
    SDL_SetHint(SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) return fail("SDL_Init");
    app::Config cfg; cfg.load();
    for (int i = 1; i + 1 < argc; ++i) if (!std::strcmp(argv[i], "--rom")) cfg.rom_path = argv[++i];
    Audio audio;
    const bool have_audio = audio.open(snd::SoundBoard::kYmClock / 144.0, snd::SoundBoard::kPcmClock / 224.0);
    SDL_Window *window = SDL_CreateWindow("Daytona Arcade Port", 1280, 720, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) return fail("Create window");
    SDL_Renderer *renderer = SDL_CreateRenderer(window, "opengles2");
    if (!renderer) renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) return fail("Create renderer");
    SDL_SetRenderVSync(renderer, 1);
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    int ww = 1280, wh = 720; SDL_GetWindowSize(window, &ww, &wh);
    const float ui_scale = std::clamp(float(wh) / 480.f, 1.f, 3.f);
    ImGui::GetStyle().ScaleAllSizes(ui_scale);
    ImGui::GetIO().FontGlobalScale = ui_scale;
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    app::Launcher launcher(cfg, window);
    app::TouchControls touch;
    app::CoinGate gate; gate.pending = cfg.pending_coins;
    std::unique_ptr<rt::GameLoop> game;
    Lifecycle life;
    const auto eeprom = pref_file("ioboard_eeprom.bin"), backup = pref_file("backup_ram.bin"), coins = pref_file("pending_coins.txt");
    life.checkpoint = [&] {
        if (game) {
            save_file(eeprom, game->board().io().eeprom);
            save_file(backup, game->board().backup_ram());
        }
        std::ofstream(coins) << gate.pending << '\n';
    };
    SDL_AddEventWatch(Lifecycle::watch, &life);
    auto start_game = [&] {
        std::lock_guard lock(life.mutex);
        life.checkpoint(); gate.reset_delivery(); touch.clear();
        try {
            game = std::make_unique<rt::GameLoop>(rt::import_rom_set(cfg.rom_path));
            game->board().video().enable_presentation(true);
            load_file(eeprom, game->board().io().eeprom); load_file(backup, game->board().backup_ram());
            audio.clear(); launcher.set_error(""); return true;
        } catch (const std::exception &e) { launcher.set_error(e.what()); return false; }
    };
    bool running = true, paused = true;
    SDL_Gamepad *pad = nullptr;
    SDL_Texture *texture = nullptr;
    int tw = 0, th = 0, applied_fps = -1;
    uint64_t last = SDL_GetTicksNS();
    double pending = 0;
    constexpr double frame_ns = 1e9 / kArcadeHz;
    while (running) {
        const uint64_t begin = SDL_GetTicksNS();
        SDL_Rect safe{0, 0, ww, wh};
        SDL_GetWindowSize(window, &ww, &wh);
        if (!SDL_GetWindowSafeArea(window, &safe)) safe = {0, 0, ww, wh};
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_TERMINATING) running = false;
            if (e.type == SDL_EVENT_WILL_ENTER_BACKGROUND || e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                paused = true; touch.clear(); gate.suspend(); audio.clear();
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(e.gdevice.which);
            if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == SDL_GetGamepadID(pad)) {
                SDL_CloseGamepad(pad); pad = nullptr;
            }
            if ((e.type == SDL_EVENT_KEY_DOWN && (e.key.scancode == SDL_SCANCODE_ESCAPE || e.key.scancode == SDL_SCANCODE_AC_BACK)) ||
                (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && pad && e.gbutton.which == SDL_GetGamepadID(pad) && e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK)) {
                paused = true; touch.clear(); gate.suspend();
            }
            if (paused) {
                if (!launcher.handle_event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            } else {
                // Keep ImGui's mouse/key release state current while the overlay
                // is drawn without interactive widgets.
                ImGui_ImplSDL3_ProcessEvent(&e);
                if (e.type != SDL_EVENT_FINGER_DOWN && e.type != SDL_EVENT_FINGER_MOTION &&
                    e.type != SDL_EVENT_FINGER_UP && e.type != SDL_EVENT_FINGER_CANCELED) continue;
                const float x = (e.tfinger.x * ww - safe.x) / std::max(1, safe.w);
                const float y = (e.tfinger.y * wh - safe.y) / std::max(1, safe.h);
                if (e.type == SDL_EVENT_FINGER_DOWN) {
                    auto command = touch.down(e.tfinger.fingerID, x, y, cfg.touch_overlay);
                    if (command == app::TouchControls::Toggle) { cfg.touch_overlay = !cfg.touch_overlay; cfg.save(); }
                    if (command == app::TouchControls::Menu) { paused = true; gate.suspend(); }
                } else if (e.type == SDL_EVENT_FINGER_MOTION) touch.motion(e.tfinger.fingerID, x, y);
                else if (e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED) touch.up(e.tfinger.fingerID);
            }
        }
        if (life.background) { pending = 0; last = SDL_GetTicksNS(); SDL_Delay(20); continue; }
        if (!pad) {
            int count = 0; auto *ids = SDL_GetGamepads(&count);
            if (ids && count) pad = SDL_OpenGamepad(ids[0]); SDL_free(ids);
        }
        if (!cfg.touch_overlay || paused) touch.clear();
        touch.apply(cfg.controls);
        pending = std::min(pending + double(begin - last), frame_ns * 4); last = begin;
        bool new_frame = false;
        if (game && !paused) {
            std::lock_guard lock(life.mutex);
            game->board().video().set_widescreen(cfg.widescreen);
            game->board().video().set_render_threads(unsigned(cfg.render_threads));
            while (pending >= frame_ns) {
                try {
                    game->run_frame(gate.sample(cfg.controls.sample(SDL_GetKeyboardState(nullptr), pad), cfg.require_start));
                    cfg.pending_coins = gate.pending;
                } catch (const std::exception &ex) { launcher.set_error(ex.what()); paused = true; break; }
                pending -= frame_ns; new_frame = true;
            }
            if (game->sound()) {
                if (have_audio) audio.push(*game->sound(), cfg.mute ? 0.f : cfg.volume);
                else { game->sound()->take_fm(); game->sound()->take_pcm(); }
            }
        } else { pending = 0; gate.suspend(); }
        if (applied_fps != cfg.fps) { SDL_SetRenderVSync(renderer, cfg.fps == 0 ? 1 : 0); applied_fps = cfg.fps; }
        if (game && game->frames()) {
            int h = cfg.high_resolution ? app::resolutions[cfg.resolution].height : 384;
            int w = int(std::lround(h * (cfg.widescreen ? 16.0 / 9.0 : 496.0 / 384.0)));
            if (!texture || w != tw || h != th) {
                if (texture) SDL_DestroyTexture(texture);
                texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
                if (!texture) { launcher.set_error(SDL_GetError()); paused = true; }
                tw = w; th = h; new_frame = true;
            }
            game->board().video().set_low_textures(cfg.low_textures);
            if (texture && (new_frame || (cfg.interpolate && !paused))) {
                const auto &pixels = game->board().video().present(w, h, float(pending / frame_ns), cfg.interpolate);
                SDL_UpdateTexture(texture, nullptr, pixels.data(), w * 4);
            }
            if (texture) SDL_SetTextureScaleMode(texture, cfg.nearest ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255); SDL_RenderClear(renderer);
        int outw, outh; SDL_GetRenderOutputSize(renderer, &outw, &outh);
        if (texture) {
            const double aspect = cfg.widescreen ? 16.0 / 9.0 : 496.0 / 384.0;
            const float h = float(std::min(double(outh), outw / aspect)), w = float(h * aspect);
            const SDL_FRect dst{(outw - w) / 2, (outh - h) / 2, w, h}; SDL_RenderTexture(renderer, texture, nullptr, &dst);
        }
        ImGui_ImplSDLRenderer3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
        if (paused) {
            switch (launcher.draw(game != nullptr, pad)) {
            case app::Launcher::StartGame: case app::Launcher::Reset:
                if (start_game()) { paused = false; if (texture) SDL_DestroyTexture(texture); texture = nullptr; }
                break;
            case app::Launcher::Resume: paused = false; touch.clear(); gate.suspend(); break;
            case app::Launcher::Quit: running = false; break;
            default: break;
            }
        } else {
            touch.draw(float(safe.x), float(safe.y), float(safe.w), float(safe.h), cfg.touch_overlay);
            if (gate.pending) {
                char label[96]; std::snprintf(label, sizeof label, "Coins waiting: %d - press Start", gate.pending);
                ImGui::GetForegroundDrawList()->AddText(ImVec2(float(safe.x + 16), float(safe.y + safe.h * 0.14)), IM_COL32_WHITE, label);
            }
        }
        ImGui::Render(); ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer); SDL_RenderPresent(renderer);
        if (cfg.fps) {
            const uint64_t target = 1000000000ull / unsigned(cfg.fps), elapsed = SDL_GetTicksNS() - begin;
            if (elapsed < target) SDL_DelayNS(target - elapsed);
        }
    }
    SDL_RemoveEventWatch(Lifecycle::watch, &life);
    life.checkpoint(); cfg.pending_coins = gate.pending; cfg.save(); audio.close();
    if (pad) SDL_CloseGamepad(pad);
    ImGui_ImplSDLRenderer3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext();
    if (texture) SDL_DestroyTexture(texture); SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
