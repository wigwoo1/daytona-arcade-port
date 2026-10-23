// daytona: the recompiled game with its launcher, in one window. SDL3 for the
// window, input and GPU (SDL_GPU: Vulkan or Direct3D 12 on Windows, Vulkan
// on Linux, Metal on macOS); Dear ImGui for the launcher. The game runs on
// the native board runtime (rt::GameLoop), loaded straight from the user's
// ROM zip (rt::import_rom_set); each composed frame is uploaded to a GPU
// texture and scaled onto the swapchain, and the sound board's output is
// played through SDL audio.
//
//   daytona [--rom FILE.zip] [--autostart] [--gpu vulkan|direct3d12|metal]
//           [--fullscreen] [--frames N]
//
// In the game: Esc opens the launcher (resume, reset, controls), F11
// toggles fullscreen. Controls are set in the launcher and saved.

#include "app/config.h"
#include "app/coin_gate.h"
#include "app/launcher.h"
#include "app/gpu_geometry.h"
#include "runtime/game_loop.h"
#include "runtime/rom_import.h"

#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "imgui.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <filesystem>
#include <cmath>
#include <memory>
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

int main(int argc, char **argv) {
    app::Config cfg;
    cfg.load();
    // Coins belong to this play session; never restore them on a new launch.
    cfg.pending_coins = 0;
    uint64_t max_frames = 0;
    bool benchmark = false, hidden = false;
    std::string screenshot_path;
    if (cfg.rom_path.empty() || !std::filesystem::exists(cfg.rom_path)) {
        const auto bundled = std::filesystem::path(SDL_GetBasePath()) / "roms" / "daytona93.zip";
        if (std::filesystem::exists(bundled)) cfg.rom_path = bundled.string();
    }
    bool autostart = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--rom") && i + 1 < argc) cfg.rom_path = argv[++i];
        else if (!std::strcmp(argv[i], "--gpu") && i + 1 < argc) cfg.gpu = argv[++i];
        else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) max_frames = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--resolution") && i + 1 < argc) {
            const int h = std::atoi(argv[++i]);
            for (int n = 0; n < app::resolution_count; ++n) if (app::resolutions[n].height == h) { cfg.resolution = n; break; }
        }
        else if (!std::strcmp(argv[i], "--fps") && i + 1 < argc) {
            const int fps = std::atoi(argv[++i]);
            cfg.fps = fps == 60 || fps == 120 || fps == 240 ? fps : 0;
        }
        else if (!std::strcmp(argv[i], "--render-threads") && i + 1 < argc) cfg.render_threads = std::clamp(std::atoi(argv[++i]), 0, 32);
        else if (!std::strcmp(argv[i], "--widescreen")) cfg.widescreen = true;
        else if (!std::strcmp(argv[i], "--high-resolution")) cfg.high_resolution = true;
        else if (!std::strcmp(argv[i], "--3ds-resolution")) cfg.resolution = app::resolution_count - 1;
        else if (!std::strcmp(argv[i], "--low-textures")) cfg.low_textures = true;
        else if (!std::strcmp(argv[i], "--interpolate")) cfg.interpolate = true;
        else if (!std::strcmp(argv[i], "--model-lod") && i + 1 < argc) cfg.model_lod = std::clamp(std::atoi(argv[++i]), 0, 2);
        else if (!std::strcmp(argv[i], "--gpu-geometry")) cfg.gpu_geometry = true;
        else if (!std::strcmp(argv[i], "--cpu-geometry")) cfg.gpu_geometry = false;
        else if (!std::strcmp(argv[i], "--benchmark")) benchmark = true;
        else if (!std::strcmp(argv[i], "--hidden")) hidden = true;
        else if (!std::strcmp(argv[i], "--screenshot") && i + 1 < argc) screenshot_path = argv[++i];
        else if (!std::strcmp(argv[i], "--fullscreen")) cfg.fullscreen = true;
        else if (!std::strcmp(argv[i], "--autostart")) autostart = true;
    }

    if (!cfg.gpu.empty()) SDL_SetHint(SDL_HINT_GPU_DRIVER, cfg.gpu.c_str());
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) return fail("SDL_Init");
    Audio audio;
    bool have_audio = false;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO))
        have_audio = audio.open(snd::SoundBoard::kYmClock / 144.0, snd::SoundBoard::kPcmClock / 224.0);
    if (have_audio) std::printf("daytona: audio driver %s\n", SDL_GetCurrentAudioDriver());
    else std::fprintf(stderr, "daytona: no audio output (%s); the game runs silent\n", SDL_GetError());

    constexpr int W = rt::GameLoop::kWidth, H = rt::GameLoop::kHeight;
    SDL_Window *window = SDL_CreateWindow("Daytona USA", std::max(992, app::resolutions[cfg.resolution].width), std::max(768, app::resolutions[cfg.resolution].height),
                                          SDL_WINDOW_RESIZABLE | (cfg.fullscreen ? SDL_WINDOW_FULLSCREEN : 0) | (hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!window) return fail("SDL_CreateWindow");
    constexpr SDL_GPUShaderFormat formats = SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_MSL;
    SDL_GPUDevice *dev = SDL_CreateGPUDevice(formats, false, nullptr);
    if (!dev && !cfg.gpu.empty()) {
        // The chosen API is not available here (Vulkan on a Mac without MoltenVK): use the automatic choice.
        std::fprintf(stderr, "daytona: %s; falling back to automatic\n", SDL_GetError());
        SDL_ResetHint(SDL_HINT_GPU_DRIVER);
        cfg.gpu.clear();
        dev = SDL_CreateGPUDevice(formats, false, nullptr);
    }
    if (!dev) return fail("SDL_CreateGPUDevice");
    if (!SDL_ClaimWindowForGPUDevice(dev, window)) return fail("SDL_ClaimWindowForGPUDevice");
    std::printf("daytona: GPU driver %s\n", SDL_GetGPUDeviceDriver(dev));

    // Dear ImGui on SDL3 + SDL_GPU (its shaders ship precompiled for every backend)
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(SDL_GetWindowDisplayScale(window));
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo ii;
    ii.Device = dev;
    ii.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(dev, window);
    ImGui_ImplSDLGPU3_Init(&ii);

    auto geometry = std::make_unique<app::GpuGeometry>(dev);
    std::printf("daytona: GPU geometry %s\n", geometry->available() ? "available" : "unavailable; CPU fallback");
    SDL_GPUTextureCreateInfo ti{};
    ti.type = SDL_GPU_TEXTURETYPE_2D;
    ti.format = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; // the screen's 0xAARRGGBB words, little-endian
    ti.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    if (geometry->available()) {
        ti.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        ti.usage |= SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE;
    }
    ti.width = W;
    ti.height = H;
    ti.layer_count_or_depth = 1;
    ti.num_levels = 1;
    SDL_GPUTexture *screen = SDL_CreateGPUTexture(dev, &ti);
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size = W * H * 4;
    SDL_GPUTransferBuffer *upload = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!screen || !upload) return fail("SDL_CreateGPUTexture");

    int texture_w = W, texture_h = H;
    uint64_t uploaded_generation = UINT64_MAX;
    app::Launcher launcher(cfg, window);
    std::unique_ptr<rt::GameLoop> game;
    const std::string eeprom_path = pref_file("ioboard_eeprom.bin"), backup_path = pref_file("backup_ram.bin");
    app::CoinGate coin_gate;
    coin_gate.pending = cfg.pending_coins;
    auto save_nv = [&] {
        if (!game) return;
        save_file(eeprom_path, game->board().io().eeprom);
        save_file(backup_path, game->board().backup_ram());
    };
    auto start_game = [&] {
        save_nv();
        coin_gate.reset_delivery();
        coin_gate.pending = cfg.pending_coins = 0;
        game.reset();
        try {
            game = std::make_unique<rt::GameLoop>(rt::import_rom_set(cfg.rom_path));
            game->board().video().enable_presentation(true);
            game->board().video().set_tile_cache(true);
            game->board().video().set_widescreen(cfg.widescreen);
            game->board().video().set_depth_buffer(cfg.depth_buffer);
            game->board().track_extension = unsigned(cfg.track_extension);
            game->board().model_lod = unsigned(cfg.model_lod);
            uploaded_generation = UINT64_MAX;
            audio.clear();
            load_file(eeprom_path, game->board().io().eeprom);
            load_file(backup_path, game->board().backup_ram());
            launcher.set_error("");
            return true;
        } catch (const std::exception &e) {
            launcher.set_error(e.what());
            return false;
        }
    };

    bool in_launcher = true, running = true, have_frame = false, new_frame = false;
    if (autostart && launcher.rom_ok() && start_game()) in_launcher = false;
    SDL_Gamepad *pad = nullptr;
    uint64_t last = SDL_GetTicksNS();
    double pending = 0;
    const double frame_ns = 1e9 / kArcadeHz;

    int applied_fps = -1, applied_width = -1, applied_height = -1;
    int applied_vsync = -1;
    uint64_t presentations = 0, stat_time = SDL_GetTicksNS(), stat_frames = 0, stat_game_frames = 0;
    uint64_t next_present = SDL_GetTicksNS();
    double measured_fps = 0;
    const uint64_t started = SDL_GetTicksNS();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) running = false;
            else if (e.type == SDL_EVENT_GAMEPAD_ADDED && !pad) pad = SDL_OpenGamepad(e.gdevice.which);
            else if (e.type == SDL_EVENT_GAMEPAD_REMOVED && pad && e.gdevice.which == SDL_GetGamepadID(pad)) {
                SDL_CloseGamepad(pad);
                pad = nullptr;
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11) {
                cfg.fullscreen = !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN);
                SDL_SetWindowFullscreen(window, cfg.fullscreen);
                cfg.save();
            }
            if (!in_launcher && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN && pad &&
                e.gbutton.which == SDL_GetGamepadID(pad) && e.gbutton.button == SDL_GAMEPAD_BUTTON_RIGHT_STICK)
                in_launcher = true;
            if (in_launcher) {
                if (!launcher.handle_event(e)) ImGui_ImplSDL3_ProcessEvent(&e);
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_ESCAPE) {
                in_launcher = true; // pause and show the launcher
            }
        }

        if (!pad) {
            int count = 0;
            SDL_JoystickID *ids = SDL_GetGamepads(&count);
            if (ids && count) pad = SDL_OpenGamepad(ids[0]);
            SDL_free(ids);
        }
        const auto &resolution = app::resolutions[cfg.resolution];
        const int window_w = in_launcher ? std::max(992, resolution.width) : resolution.width;
        const int window_h = in_launcher ? std::max(768, resolution.height) : resolution.height;
        if (window_w != applied_width || window_h != applied_height) {
            SDL_SetWindowSize(window, window_w, window_h);
            applied_width = window_w; applied_height = window_h;
        }
        const bool sync_display = cfg.vsync;
        if (applied_fps != cfg.fps || applied_vsync != int(sync_display)) {
            SDL_GPUPresentMode mode = sync_display ? SDL_GPU_PRESENTMODE_VSYNC : SDL_GPU_PRESENTMODE_IMMEDIATE;
            if (!SDL_WindowSupportsGPUPresentMode(dev, window, mode)) mode = SDL_GPU_PRESENTMODE_VSYNC;
            if (!SDL_SetGPUSwapchainParameters(dev, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
                return fail("present mode");
            applied_fps = cfg.fps;
            applied_vsync = int(sync_display);
            next_present = SDL_GetTicksNS();
        }

        // Game: arcade speed (57.52 frames/s), presented at the display's rate.
        const uint64_t now = SDL_GetTicksNS();
        pending = std::min(pending + double(now - last), frame_ns * 4);
        last = now;
        if (benchmark && game && !in_launcher) pending = frame_ns;
        if (game && !in_launcher) {
            const bool enhanced_render = cfg.high_resolution || cfg.interpolate || cfg.widescreen || cfg.low_textures || cfg.texture_quality != 100 || cfg.depth_buffer || cfg.track_extension || cfg.model_lod || (cfg.gpu_geometry && geometry->available());
            game->board().video().enable_presentation(true, enhanced_render);
            game->board().track_extension = unsigned(cfg.track_extension);
            game->board().model_lod = unsigned(cfg.model_lod);
            game->board().video().set_widescreen(cfg.widescreen);
            game->board().video().set_depth_buffer(cfg.depth_buffer);
            while (pending >= frame_ns) {
                try {
                    game->run_frame(coin_gate.sample(cfg.controls.sample(SDL_GetKeyboardState(nullptr), pad), cfg.require_start));
                    cfg.pending_coins = coin_gate.pending;
                } catch (const std::exception &error) {
                    launcher.set_error(error.what()); in_launcher = true; pending = 0; break;
                }
                pending -= frame_ns;
                new_frame = have_frame = true;
                if (max_frames && game->frames() >= max_frames) { running = false; break; }
            }
            if (game->sound()) {
                if (have_audio) audio.push(*game->sound(), cfg.mute ? 0.0f : cfg.volume);
                else game->sound()->take_fm(), game->sound()->take_pcm(); // nowhere to play it
            }
        } else {
            coin_gate.suspend();
            pending = 0;
        }

        const bool enhanced = cfg.high_resolution || cfg.interpolate || cfg.widescreen || cfg.low_textures || cfg.texture_quality != 100 || cfg.depth_buffer || cfg.track_extension || cfg.model_lod || (cfg.gpu_geometry && geometry->available());
        int rw = cfg.widescreen ? 683 : W, rh = H;
        if (cfg.high_resolution) {
            rh = resolution.height;
            rw = int(std::lround(double(rh) * (cfg.widescreen ? 16.0 / 9.0 : double(W) / H)));
        }
        if (rw != texture_w || rh != texture_h) {
            SDL_WaitForGPUIdle(dev);
            SDL_ReleaseGPUTexture(dev, screen); SDL_ReleaseGPUTransferBuffer(dev, upload);
            ti.width = rw; ti.height = rh; tbi.size = rw * rh * 4;
            screen = SDL_CreateGPUTexture(dev, &ti); upload = SDL_CreateGPUTransferBuffer(dev, &tbi);
            if (!screen || !upload) return fail("resize render target");
            texture_w = rw; texture_h = rh; new_frame = have_frame;
            uploaded_generation = UINT64_MAX;
        }
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(dev);
        if (!cmd) return fail("SDL_AcquireGPUCommandBuffer");
        if (have_frame && (new_frame || (cfg.interpolate && !in_launcher))) {
            game->board().video().set_render_threads(unsigned(cfg.render_threads));
            const int texture_quality=cfg.low_textures && cfg.texture_quality==100 ? 50 : cfg.texture_quality;
            game->board().video().set_texture_quality(texture_quality);
            const bool gpu_drawn = cfg.gpu_geometry && !cfg.interpolate && geometry->render(cmd, screen,
                game->board().video(), rw, rh, cfg.depth_buffer, texture_quality);
            if (!gpu_drawn) {
            const auto &pixels = enhanced ? game->board().video().present(rw, rh, float(pending / frame_ns), cfg.interpolate) : game->screen();
            if (!enhanced || uploaded_generation != game->board().video().presentation_generation()) {
            void *p = SDL_MapGPUTransferBuffer(dev, upload, true);
            if (!p) return fail("map frame upload");
            std::memcpy(p, pixels.data(), size_t(rw) * rh * 4);
            if (geometry->available()) {
                auto *rgba = static_cast<uint32_t *>(p);
                for (size_t i=0; i<size_t(rw)*rh; ++i) {
                    const uint32_t c=rgba[i]; rgba[i]=(c&0xff00ff00)|((c&255)<<16)|((c>>16)&255);
                }
            }
            if (!running && !screenshot_path.empty()) {
                SDL_Surface *surface = SDL_CreateSurfaceFrom(rw, rh, SDL_PIXELFORMAT_ARGB8888, const_cast<uint32_t *>(pixels.data()), rw * 4);
                if (surface) { SDL_SaveBMP(surface, screenshot_path.c_str()); SDL_DestroySurface(surface); }
            }
            SDL_UnmapGPUTransferBuffer(dev, upload);
            SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTextureTransferInfo src{};
            src.transfer_buffer = upload;
            SDL_GPUTextureRegion dst{};
            dst.texture = screen;
            dst.w = rw;
            dst.h = rh;
            dst.d = 1;
            SDL_UploadToGPUTexture(copy, &src, &dst, true);
            SDL_EndGPUCopyPass(copy);
            uploaded_generation = enhanced ? game->board().video().presentation_generation() : UINT64_MAX;
            }
            } else {
                uploaded_generation = UINT64_MAX;
                if (!running && !screenshot_path.empty()) {
                    SDL_GPUTransferBufferCreateInfo download_info{};
                    download_info.usage=SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; download_info.size=rw*rh*4;
                    auto *download=SDL_CreateGPUTransferBuffer(dev,&download_info);
                    if (!download) return fail("GPU screenshot buffer");
                    auto *copy=SDL_BeginGPUCopyPass(cmd);
                    SDL_GPUTextureRegion source{}; source.texture=screen; source.w=rw; source.h=rh; source.d=1;
                    SDL_GPUTextureTransferInfo dest{}; dest.transfer_buffer=download;
                    SDL_DownloadFromGPUTexture(copy,&source,&dest); SDL_EndGPUCopyPass(copy);
                    auto *fence=SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
                    if (!fence || !SDL_WaitForGPUFences(dev,true,&fence,1)) return fail("GPU screenshot wait");
                    SDL_ReleaseGPUFence(dev,fence);
                    void *pixels=SDL_MapGPUTransferBuffer(dev,download,false);
                    if (!pixels) return fail("GPU screenshot map");
                    auto *surface=SDL_CreateSurfaceFrom(rw,rh,SDL_PIXELFORMAT_RGBA32,pixels,rw*4);
                    if (surface) { SDL_SaveBMP(surface,screenshot_path.c_str()); SDL_DestroySurface(surface); }
                    SDL_UnmapGPUTransferBuffer(dev,download); SDL_ReleaseGPUTransferBuffer(dev,download);
                    cmd=SDL_AcquireGPUCommandBuffer(dev);
                    if (!cmd) return fail("command after GPU screenshot");
                }
            }
            new_frame = false;
        }

        ImDrawData *draw = nullptr;
        if (in_launcher) {
            ImGui_ImplSDLGPU3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            switch (launcher.draw(game != nullptr, pad)) {
            case app::Launcher::StartGame:
            case app::Launcher::Reset:
                if (start_game()) in_launcher = false, have_frame = false;
                break;
            case app::Launcher::Resume: in_launcher = false; break;
            case app::Launcher::Quit: running = false; break;
            default: break;
            }
            ImGui::Render();
            draw = ImGui::GetDrawData();
            ImGui_ImplSDLGPU3_PrepareDrawData(draw, cmd);
        }

        if (!draw && !in_launcher && coin_gate.pending > 0) {
            ImGui_ImplSDLGPU3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(8, 8));
            ImGui::SetNextWindowBgAlpha(0.85f);
            ImGui::Begin("Coins waiting", nullptr, ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav);
            ImGui::Text("Coins waiting: %d", coin_gate.pending);
            ImGui::TextUnformatted("Press Start / Enter to use");
            ImGui::End();
            ImGui::Render();
            draw = ImGui::GetDrawData();
            ImGui_ImplSDLGPU3_PrepareDrawData(draw, cmd);
        }

        SDL_GPUTexture *swap = nullptr;
        Uint32 sw = 0, sh = 0;
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window, &swap, &sw, &sh)) return fail("swapchain");
        if (swap) {
            if (have_frame) { // the game's screen, 4:3 letterboxed
                const double aspect = cfg.widescreen ? 16.0 / 9.0 : double(W) / H;
                const double draw_height = std::min(double(sh), double(sw) / aspect);
                const Uint32 dw = Uint32(std::lround(draw_height * aspect)), dh = Uint32(std::lround(draw_height));
                SDL_GPUBlitInfo blit{};
                blit.source.texture = screen;
                blit.source.w = rw;
                blit.source.h = rh;
                blit.destination.texture = swap;
                blit.destination.x = (sw - dw) / 2;
                blit.destination.y = (sh - dh) / 2;
                blit.destination.w = dw;
                blit.destination.h = dh;
                blit.load_op = SDL_GPU_LOADOP_CLEAR;
                blit.clear_color = SDL_FColor{0, 0, 0, 1};
                blit.filter = cfg.nearest ? SDL_GPU_FILTER_NEAREST : SDL_GPU_FILTER_LINEAR;
                SDL_BlitGPUTexture(cmd, &blit);
            }
            if (draw || !have_frame) { // the launcher on top (or a clear screen)
                SDL_GPUColorTargetInfo ct{};
                ct.texture = swap;
                ct.load_op = have_frame ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
                ct.store_op = SDL_GPU_STOREOP_STORE;
                ct.clear_color = SDL_FColor{0.05f, 0.05f, 0.08f, 1};
                SDL_GPURenderPass *pass = SDL_BeginGPURenderPass(cmd, &ct, 1, nullptr);
                if (draw) ImGui_ImplSDLGPU3_RenderDrawData(draw, cmd, pass);
                SDL_EndGPURenderPass(pass);
            }
        }
        if (!SDL_SubmitGPUCommandBuffer(cmd)) return fail("submit frame");
        if (swap) ++presentations;
        const uint64_t end = SDL_GetTicksNS();
        if (end - stat_time >= 1000000000ULL) {
            measured_fps = double(presentations - stat_frames) * 1e9 / double(end - stat_time);
            const uint64_t game_frames = game ? game->frames() : 0;
            if (cfg.show_stats) {
                char title[200];
                std::snprintf(title, sizeof title, "Daytona USA | %.1f FPS | %.1f game FPS | %dx%d 3D | %s", measured_fps,
                    double(game_frames >= stat_game_frames ? game_frames - stat_game_frames : game_frames) * 1e9 / double(end - stat_time), rw, rh,
                    cfg.gpu_geometry && geometry->available() && !cfg.interpolate ? "GPU" : "CPU");
                SDL_SetWindowTitle(window, title);
            } else SDL_SetWindowTitle(window, "Daytona USA");
            stat_time = end; stat_frames = presentations; stat_game_frames = game_frames;
        }
        // Fixed-deadline pacing avoids adding render time to each period.
        // A missed deadline drops presentation work, never changes game speed.
        if (cfg.fps && !benchmark) {
            const uint64_t period = 1000000000ULL / uint64_t(cfg.fps);
            next_present += period;
            if (next_present > end) SDL_DelayPrecise(next_present - end);
            else if (end - next_present > period) next_present = end;
        } else if (!swap) SDL_Delay(10);
    }

    std::printf("daytona: %llu presentations in %.3f seconds; last measured %.1f FPS\n", (unsigned long long)presentations,
                double(SDL_GetTicksNS() - started) / 1e9, measured_fps);
    save_nv();
    cfg.pending_coins = 0;
    cfg.save();
    if (game) std::printf("daytona: %llu frames\n", (unsigned long long)game->frames());
    audio.close();
    SDL_WaitForGPUIdle(dev);
    geometry.reset();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseGPUTransferBuffer(dev, upload);
    SDL_ReleaseGPUTexture(dev, screen);
    SDL_ReleaseWindowFromGPUDevice(dev, window);
    SDL_DestroyGPUDevice(dev);
    SDL_DestroyWindow(window);
    if (pad) SDL_CloseGamepad(pad);
    SDL_Quit();
    return 0;
}
