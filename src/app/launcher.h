// The launcher: a Dear ImGui interface in the game's window. Game tab: the
// ROM set (browse, per-file verification), graphics API, fullscreen.
// Controls tab: every arcade control's key and gamepad binding (press to
// bind), live steering and pedal meters, dead zone. Settings are saved as
// they change. Video resolution and upscaling are to come.
#pragma once

#include "app/config.h"
#include "runtime/rom_import.h"

#include <SDL3/SDL.h>

#include <mutex>
#include <string>
#include <vector>

namespace app {

class Launcher {
public:
    Launcher(Config &cfg, SDL_Window *window);

    enum Result { Stay, StartGame, Resume, Reset, Quit };
    // Returns true if the event was consumed (binding capture).
    bool handle_event(const SDL_Event &e);
    // Draw the interface (between ImGui::NewFrame and ImGui::Render).
    Result draw(bool game_running, SDL_Gamepad *pad);

    bool rom_ok() const { return rom_ok_; }
    void set_error(const std::string &e) { error_ = e; }

private:
    void check_rom();
    void browse();
    static void SDLCALL dialog_done(void *self, const char *const *files, int filter);

    Config &cfg_;
    SDL_Window *window_;
    std::vector<rt::RomCheck> checks_;
    bool rom_ok_ = false;
    std::string rom_message_, error_;
    char path_buf_[1024] = {};

    std::mutex dialog_mutex_;
    std::string dialog_result_;
    bool dialog_pending_ = false;

    int capture_action_ = -1; // binding being captured
    bool capture_pad_ = false;
};

} // namespace app
