// The game running on the native board, one frame at a time: the frame
// pacing m2run and the windowed game share. No clock: vblank starts when the
// game waits in its wait-for-vblank loop (or, for a CPU-bound frame, after
// one frame's worth of i960 work), and the frame is done when the vblank
// handler has returned. The caller shows the frame and calls again; with a
// window that is the display's vsync, so the frame rate is the only limit.
#pragma once

#include "runtime/gen_support.h"
#include "runtime/lockstep.h"
#include "runtime/m2_board.h"
#include "runtime/sound_board.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rt {

class GameLoop {
public:
    struct FrameTiming { double machine_ms = 0, sound_ms = 0, tgp_ms = 0, geometry_ms = 0, video_ms = 0; uint64_t tgp_runs = 0; };
    // images_dir: the importer's output (build/rom_cache/daytona93).
    explicit GameLoop(const std::string &images_dir, bool compact_memory = false, bool deferred_main = false);
    // Images already loaded (rt::import_rom_set: straight from the ROM zip).
    explicit GameLoop(M2Board::Images images, bool compact_memory = false,
                      std::function<std::vector<uint8_t>()> main_loader = {});

    // Run until the next screen is composed. `inputs` are latched by the I/O
    // board at the start of this frame's vblank.
    void run_frame(const Inputs &inputs, FrameTiming *timing = nullptr, bool defer_sound = false);
    void advance_sound_frame(FrameTiming *timing = nullptr);

    const std::vector<uint32_t> &screen() const { return board_->video().screen(); } // desktop is 496x384; N64 size is selected at build time
    static constexpr int kWidth = Video::W, kHeight = Video::H;
    M2Board &board() { return *board_; }
    snd::SoundBoard *sound() { return sound_.get(); } // null without the sound ROMs
    static constexpr double kFrameHz = 16000000.0 / (656.0 * 424.0); // the board's video timing
    uint64_t frames() const { return frames_; }
    uint64_t instructions() const { return ls_->current_count(); }
    int interrupts() const { return ls_->interrupts(); }
    size_t callback_slots() const { return ls_->callback_slots(); }

private:
    void probe();
    std::unique_ptr<M2Board> board_;
    std::unique_ptr<Cpu> cpu_;
    std::unique_ptr<Lockstep> ls_;
    std::unique_ptr<gen::Env> env_;
    std::unique_ptr<snd::SoundBoard> sound_;
    Inputs inputs_;
    bool in_vblank_ = false, frame_done_ = false;
    uint64_t frame_start_ = 0, vblank_start_ = 0, frames_ = 0;
};

} // namespace rt
