#include "app/launcher.h"

#include "imgui.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace app {

Launcher::Launcher(Config &cfg, SDL_Window *window) : cfg_(cfg), window_(window) {
    std::snprintf(path_buf_, sizeof path_buf_, "%s", cfg_.rom_path.c_str());
    check_rom();
}

void Launcher::check_rom() {
    checks_.clear();
    rom_ok_ = false;
    if (cfg_.rom_path.empty()) {
        rom_message_ = "Choose your daytona93 ROM set (.zip or .7z).";
        return;
    }
    // Keep the path absolute, so the saved setting works from any directory.
    std::error_code ec;
    const auto abs = std::filesystem::absolute(cfg_.rom_path, ec);
    if (!ec && abs.string() != cfg_.rom_path && std::filesystem::exists(abs, ec)) {
        cfg_.rom_path = abs.lexically_normal().string();
        std::snprintf(path_buf_, sizeof path_buf_, "%s", cfg_.rom_path.c_str());
        cfg_.save();
    }
    try {
        checks_ = rt::check_rom_set(cfg_.rom_path);
        int good = 0;
        for (const auto &c : checks_) good += c.ok;
        rom_ok_ = good == int(checks_.size());
        rom_message_ = rom_ok_ ? "All " + std::to_string(good) + " files verified."
                               : std::to_string(int(checks_.size()) - good) + " of " + std::to_string(checks_.size()) +
                                     " files missing or wrong: this is not the daytona93 set.";
    } catch (const std::exception &e) {
        rom_message_ = e.what();
    }
}

void SDLCALL Launcher::dialog_done(void *self, const char *const *files, int) {
    auto *l = static_cast<Launcher *>(self);
    std::lock_guard<std::mutex> g(l->dialog_mutex_);
    l->dialog_pending_ = false;
    if (files && files[0]) l->dialog_result_ = files[0];
    else if (!files) l->error_ = std::string("File dialog unavailable (") + SDL_GetError() + "); type the path instead.";
}

void Launcher::browse() {
    static const SDL_DialogFileFilter filters[] = {{"ROM set (zip, 7z)", "zip;7z"}, {"All files", "*"}};
    dialog_pending_ = true;
    SDL_ShowOpenFileDialog(dialog_done, this, window_, filters, 2, cfg_.rom_path.empty() ? nullptr : cfg_.rom_path.c_str(), false);
}

bool Launcher::handle_event(const SDL_Event &e) {
    if (capture_action_ < 0) return false;
    Binding &b = cfg_.controls.bind[capture_action_];
    if (e.type == SDL_EVENT_KEY_DOWN) {
        if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
            capture_action_ = -1; // cancel
        } else if (!capture_pad_) {
            b.key = (e.key.scancode == SDL_SCANCODE_BACKSPACE || e.key.scancode == SDL_SCANCODE_DELETE) ? SDL_SCANCODE_UNKNOWN
                                                                                                         : e.key.scancode;
            capture_action_ = -1;
            cfg_.save();
        }
        return true;
    }
    if (capture_pad_ && e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        b.pad.kind = PadInput::Button;
        b.pad.index = e.gbutton.button;
        capture_action_ = -1;
        cfg_.save();
        return true;
    }
    if (capture_pad_ && e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION && std::abs(int(e.gaxis.value)) > 20000) {
        b.pad.kind = PadInput::Axis;
        b.pad.index = e.gaxis.axis;
        b.pad.dir = e.gaxis.value > 0 ? 1 : -1;
        capture_action_ = -1;
        cfg_.save();
        return true;
    }
    return e.type == SDL_EVENT_KEY_UP || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
}

Launcher::Result Launcher::draw(bool game_running, SDL_Gamepad *pad) {
    {
        std::lock_guard<std::mutex> g(dialog_mutex_);
        if (!dialog_result_.empty()) {
            cfg_.rom_path = dialog_result_;
            dialog_result_.clear();
            std::snprintf(path_buf_, sizeof path_buf_, "%s", cfg_.rom_path.c_str());
            cfg_.save();
            check_rom();
        }
    }

    Result result = Stay;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowBgAlpha(game_running ? 0.85f : 1.0f);
    ImGui::Begin("Daytona USA", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextUnformatted("DAYTONA USA");
#ifndef __ANDROID__
    ImGui::SameLine();
    ImGui::TextDisabled("  static recompilation, native runtime");
#endif
    ImGui::Separator();

#ifdef __ANDROID__
    ImGui::BeginDisabled(!rom_ok_);
    if (ImGui::Button(game_running ? "Resume game" : "Launch game", ImVec2(240, 64))) result = game_running ? Resume : StartGame;
    ImGui::EndDisabled();
    ImGui::Separator();
#endif
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Game")) {
            ImGui::Spacing();
#ifdef __ANDROID__
            ImGui::TextUnformatted("Game data");
#else
            ImGui::TextUnformatted("ROM set");
#endif
#ifndef __ANDROID__
            ImGui::SetNextItemWidth(-200);
            if (ImGui::InputText("##rom", path_buf_, sizeof path_buf_, ImGuiInputTextFlags_EnterReturnsTrue)) {
                cfg_.rom_path = path_buf_;
                cfg_.save();
                check_rom();
            }
            ImGui::SameLine();
            if (ImGui::Button("Browse...", ImVec2(90, 0)) && !dialog_pending_) browse();
            ImGui::SameLine();
            if (ImGui::Button("Check", ImVec2(90, 0))) {
                cfg_.rom_path = path_buf_;
                cfg_.save();
                check_rom();
            }
#endif
            ImGui::TextColored(rom_ok_ ? ImVec4(0.4f, 0.9f, 0.4f, 1) : ImVec4(1, 0.6f, 0.3f, 1), "%s", rom_message_.c_str());
            if (!checks_.empty() && ImGui::TreeNode("Files")) {
                if (ImGui::BeginTable("files", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    for (const auto &c : checks_) {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(c.file.c_str());
                        ImGui::TableNextColumn();
                        if (c.ok) ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1), "ok");
                        else ImGui::TextColored(ImVec4(1, 0.5f, 0.3f, 1), "%s", c.problem.c_str());
                    }
                    ImGui::EndTable();
                }
                ImGui::TreePop();
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted("Display");
#ifndef __ANDROID__
            static const char *apis[] = {"Automatic", "Vulkan", "Direct3D 12", "Metal"};
            static const char *api_ids[] = {"", "vulkan", "direct3d12", "metal"};
            int api = 0;
            for (int i = 0; i < 4; i++)
                if (cfg_.gpu == api_ids[i]) api = i;
            ImGui::SetNextItemWidth(200);
            ImGui::BeginDisabled(cfg_.texture_quality==300);
            if (ImGui::Combo("Graphics API (applies on restart)", &api, apis, 4)) {
                cfg_.gpu = api_ids[api];
                cfg_.save();
            }
            ImGui::EndDisabled();
            if (ImGui::Checkbox("Fullscreen", &cfg_.fullscreen)) {
                SDL_SetWindowFullscreen(window_, cfg_.fullscreen);
                cfg_.save();
            }
#endif
            ImGui::SetNextItemWidth(260 * ImGui::GetIO().FontGlobalScale);
            if (ImGui::BeginCombo("Output resolution", resolutions[cfg_.resolution].name)) {
                for (int i = 0; i < resolution_count; ++i)
                    if (ImGui::Selectable(resolutions[i].name, cfg_.resolution == i)) {
                        cfg_.resolution = i; cfg_.save();
                    }
                ImGui::EndCombo();
            }
            static const char *rates[] = {"Uncapped", "60 FPS", "120 FPS", "240 FPS"};
            static const int values[] = {0, 60, 120, 240};
            int rate = cfg_.fps == 240 ? 3 : cfg_.fps == 120 ? 2 : cfg_.fps == 60 ? 1 : 0;
            ImGui::SetNextItemWidth(260 * ImGui::GetIO().FontGlobalScale);
            if (ImGui::Combo("Frame-rate target", &rate, rates, 4)) { cfg_.fps = values[rate]; cfg_.save(); }
            if (ImGui::Checkbox("VSync (prevent screen tearing)", &cfg_.vsync)) cfg_.save();
            static const char *workers[] = {"Automatic", "1 (single thread)", "2", "4", "8", "16"};
            static const int worker_values[] = {0, 1, 2, 4, 8, 16};
            int worker = 0;
            for (int i = 0; i < 6; ++i) if (cfg_.render_threads == worker_values[i]) worker = i;
            ImGui::SetNextItemWidth(260 * ImGui::GetIO().FontGlobalScale);
            if (ImGui::Combo("Rendering workers", &worker, workers, 6)) { cfg_.render_threads = worker_values[worker]; cfg_.save(); }
            if (ImGui::Checkbox("Widescreen 16:9 (experimental)", &cfg_.widescreen)) cfg_.save();
            if (ImGui::Checkbox("Render 3D at output resolution", &cfg_.high_resolution)) cfg_.save();
#ifdef __ANDROID__
            if (ImGui::Checkbox("Half-resolution textures", &cfg_.low_textures)) cfg_.save();
#else
            static const char *texture_labels[] = {"100% - Original", "50%", "25%", "10%", "300% - Repaint preview"};
            static const int texture_values[] = {100,50,25,10,300};
            int texture_choice = 0;
            const int texture_percent = cfg_.low_textures && cfg_.texture_quality == 100 ? 50 : cfg_.texture_quality;
            for (int i=0;i<5;i++) if (texture_values[i]==texture_percent) texture_choice=i;
            ImGui::SetNextItemWidth(260 * ImGui::GetIO().FontGlobalScale);
            if (ImGui::Combo("Texture resolution", &texture_choice, texture_labels, 5)) {
                cfg_.texture_quality=texture_values[texture_choice]; cfg_.low_textures=cfg_.texture_quality==50;
                cfg_.apply_texture_requirements(); cfg_.save();
            }
            if(cfg_.texture_quality==300)ImGui::TextWrapped("300% textures use Vulkan GPU geometry with motion interpolation off. Restart after changing Graphics API. Original logos, text and HUD are preserved.");
#endif
#ifndef __ANDROID__
            ImGui::BeginDisabled(cfg_.texture_quality==300);
#endif
            if (ImGui::Checkbox("Experimental 3D motion interpolation", &cfg_.interpolate)) cfg_.save();
#ifndef __ANDROID__
            ImGui::EndDisabled();
#endif
            if (ImGui::Checkbox("Depth buffer (reduce polygon overlap)", &cfg_.depth_buffer)) cfg_.save();
#ifndef __ANDROID__
            ImGui::BeginDisabled(cfg_.texture_quality==300);
            if (ImGui::Checkbox("GPU geometry (Vulkan)", &cfg_.gpu_geometry)) cfg_.save();
            ImGui::EndDisabled();
            ImGui::TextDisabled("Motion interpolation uses the CPU renderer.");
            static const char *lods[] = {"Original", "Always low LOD", "Always High LOD"};
            ImGui::SetNextItemWidth(260 * ImGui::GetIO().FontGlobalScale);
            if (ImGui::Combo("Traffic car LOD", &cfg_.model_lod, lods, 3)) cfg_.save();
            ImGui::TextWrapped("Controls traffic car body polygon detail. Your car, wheels and scenery retain their original detail. High LOD costs more rendering time.");
            static const char *distances[] = {"Original", "Extended", "Farthest"};
            ImGui::SetNextItemWidth(260 * ImGui::GetIO().FontGlobalScale);
            if (ImGui::Combo("Track draw distance (experimental)", &cfg_.track_extension, distances, 3)) cfg_.save();
            if (cfg_.track_extension == 2) ImGui::TextWrapped("Farthest submits the whole track grid. Occlusion still applies; this costs more rendering time.");
            else if (cfg_.track_extension) ImGui::TextWrapped("Adds neighbouring track scenery. Longer distance costs more rendering time.");
#endif
            if (ImGui::Checkbox("Sharp pixel scaling", &cfg_.nearest)) cfg_.save();
#ifdef __ANDROID__
            if (ImGui::Checkbox("Show touch controls", &cfg_.touch_overlay)) cfg_.save();
            ImGui::TextWrapped("Use the Controls button during play to hide or show touch controls. Menu pauses the game.");
#endif
            if (ImGui::Checkbox("Require Start after coins", &cfg_.require_start)) cfg_.save();
#ifndef __ANDROID__
            if (ImGui::Checkbox("Show actual frame rate in title", &cfg_.show_stats)) cfg_.save();
#endif
            ImGui::TextWrapped("Original game speed is preserved. Widescreen expands the 3D view and keeps the HUD centered. High-resolution 3D uses more CPU. Frame-rate targets depend on hardware; use a matching high-refresh display.");
            ImGui::TextWrapped("Widescreen can reveal scenery popping at the edges; original 2D backgrounds fill the wider view.");
            ImGui::TextWrapped("Interpolation smooths matching 3D surfaces only. HUD, scene cuts and changed geometry keep their arcade cadence; some judder or artifacts may remain.");

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::TextUnformatted("Audio");
            ImGui::SetNextItemWidth(200);
            int vol = int(cfg_.volume * 100.0f + 0.5f);
            if (ImGui::SliderInt("Volume", &vol, 0, 100, "%d%%")) {
                cfg_.volume = float(vol) / 100.0f;
                cfg_.save();
            }
            ImGui::SameLine();
            if (ImGui::Checkbox("Mute", &cfg_.mute)) cfg_.save();

            ImGui::Spacing();
            ImGui::Separator();
            if (!error_.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.4f, 1));
                ImGui::TextWrapped("%s", error_.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::BeginDisabled(!rom_ok_);
            if (game_running) {
                if (ImGui::Button("Resume", ImVec2(140, 40))) result = Resume;
                ImGui::SameLine();
                if (ImGui::Button("Reset", ImVec2(140, 40))) result = Reset;
            } else if (ImGui::Button("Start", ImVec2(140, 40))) {
                result = StartGame;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Quit", ImVec2(140, 40))) result = Quit;
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Controls")) {
            Controls &c = cfg_.controls;
            ImGui::Spacing();
            ImGui::Text("Gamepad: %s", pad ? SDL_GetGamepadName(pad) : "none connected");
            ImGui::TextWrapped("Xbox defaults: left stick steer, RT accelerate, LT brake; LB/RB shift; A/B/X/Y gears 1/2/3/4.");
            ImGui::TextWrapped("D-pad Up/Right/Down/Left: cameras 1/2/3/4. View/Back: insert coin. Menu/Start: start. Click right stick: pause/settings.");
            // live meters
            const bool *keys = SDL_GetKeyboardState(nullptr);
            const float steer = c.value(SteerRight, keys, pad) - c.value(SteerLeft, keys, pad);
            char label[32];
            std::snprintf(label, sizeof label, "%+.2f", c.steer_invert ? -steer : steer);
            ImGui::ProgressBar(0.5f + 0.5f * (c.steer_invert ? -steer : steer), ImVec2(200, 0), label);
            ImGui::SameLine();
            ImGui::TextUnformatted("Steering");
            const float acc = c.value(Accelerate, keys, pad), brk = c.value(Brake, keys, pad);
            ImGui::ProgressBar(acc, ImVec2(200, 0));
            ImGui::SameLine();
            ImGui::TextUnformatted("Accelerator");
            ImGui::ProgressBar(brk, ImVec2(200, 0));
            ImGui::SameLine();
            ImGui::TextUnformatted("Brake");
            ImGui::SetNextItemWidth(200);
            if (ImGui::SliderFloat("Dead zone", &c.deadzone, 0.0f, 0.4f, "%.2f")) cfg_.save();
            ImGui::SameLine();
            if (ImGui::Checkbox("Invert steering", &c.steer_invert)) cfg_.save();
            ImGui::TextDisabled("Triggers and sticks are analogue: bind the accelerator and brake to triggers for full travel.");

            if (ImGui::BeginTable("binds", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
                ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthFixed, 160);
                ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthFixed, 180);
                ImGui::TableSetupColumn("Gamepad");
                ImGui::TableHeadersRow();
                for (int a = 0; a < kNumActions; a++) {
                    ImGui::PushID(a);
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(action_name(Action(a)));
                    ImGui::TableNextColumn();
                    const bool cap_key = capture_action_ == a && !capture_pad_;
                    const char *kn = c.bind[a].key == SDL_SCANCODE_UNKNOWN ? "-" : SDL_GetScancodeName(c.bind[a].key);
                    if (ImGui::Button(cap_key ? "Press a key..." : kn, ImVec2(170, 0))) capture_action_ = a, capture_pad_ = false;
                    ImGui::TableNextColumn();
                    const bool cap_pad = capture_action_ == a && capture_pad_;
                    const std::string pn = cap_pad ? "Press a button or move an axis..." : c.bind[a].pad.describe();
                    if (ImGui::Button(pn.c_str(), ImVec2(260, 0))) capture_action_ = a, capture_pad_ = true;
                    ImGui::SameLine();
                    if (ImGui::SmallButton("clear")) {
                        c.bind[a].pad = PadInput{};
                        cfg_.save();
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Esc cancels a binding; Backspace clears a key.");
            if (ImGui::Button("Reset to defaults")) {
                c.set_defaults();
                cfg_.save();
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
    return result;
}

} // namespace app
