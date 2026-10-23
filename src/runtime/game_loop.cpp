#include "runtime/game_loop.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>

namespace rt {

namespace {
std::vector<uint8_t> load(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path + " (run the setup / importer first)");
    return {std::istreambuf_iterator<char>(f), {}};
}

constexpr uint64_t kProbe = 1024;       // instructions between wait-loop checks
constexpr uint64_t kFrameCap = 110000;  // a CPU-bound frame: 25 MHz / 57.52 Hz of i960 work, as MAME measures
constexpr uint64_t kVblankCap = 40000;  // a vblank handler that never returns to the wait loop
constexpr uint64_t kMinFrame = kProbe * 2;
} // namespace

GameLoop::GameLoop(const std::string &dir, bool compact_memory, bool deferred_main) : GameLoop([&] {
    M2Board::Images img;
    img.program = load(dir + "/program.bin");
    if (!deferred_main) img.main_data = load(dir + "/main_data.bin");
    img.copro_tables = load(dir + "/copro_tables.bin");
    img.copro_data = load(dir + "/copro_data.bin");
    img.polygons = load(dir + "/polygons.bin");
    img.textures = load(dir + "/textures.bin");
    img.sound_program = load(dir + "/sound_program.bin");
    img.pcm1 = load(dir + "/pcm1.bin");
    img.pcm2 = load(dir + "/pcm2.bin");
    return img;
}(), compact_memory, deferred_main ? std::function<std::vector<uint8_t>()>([dir] {
    auto bytes = load(dir + "/main_data.bin");
    if (bytes.size() != 0x2000000 || !std::all_of(bytes.begin() + 0x1000000, bytes.end(), [](uint8_t b) { return b == 0; }))
        throw std::runtime_error("Invalid deferred main ROM");
    bytes.resize(0x1000000); bytes.shrink_to_fit();
    return bytes;
}) : std::function<std::vector<uint8_t>()>{}) {}

GameLoop::GameLoop(M2Board::Images img, bool compact_memory,
                   std::function<std::vector<uint8_t>()> main_loader) {
    // The upper main-data half is padding in this verified ROM set. Retain
    // its zero reads through sparse board pages instead of sixteen MiB of RAM.
    if (compact_memory && img.main_data.size() == 0x2000000 &&
        std::all_of(img.main_data.begin() + 0x1000000, img.main_data.end(), [](uint8_t b) { return b == 0; })) {
        img.main_data.resize(0x1000000);
        img.main_data.shrink_to_fit();
    }
    if (!img.sound_program.empty() || img.sound_program_rom.available()) {
        const auto data = [](const std::vector<uint8_t> &owned, M2Board::Images::RomView mapped) {
            return mapped.data ? mapped.data : owned.data();
        };
        const auto size = [](const std::vector<uint8_t> &owned, M2Board::Images::RomView mapped) {
            return mapped.available() ? mapped.size : owned.size();
        };
        const uint8_t *const program = data(img.sound_program, img.sound_program_rom);
        const uint8_t *const pcm1 = data(img.pcm1, img.pcm1_rom);
        const uint8_t *const pcm2 = data(img.pcm2, img.pcm2_rom);
        if (img.sound_program_rom.read || img.pcm1_rom.read || img.pcm2_rom.read) {
            if (!img.sound_program_rom.read || !img.pcm1_rom.read || !img.pcm2_rom.read)
                throw Fatal("incomplete chunked sound ROM views");
            sound_ = std::make_unique<snd::SoundBoard>(img.sound_program_rom.read, img.sound_program_rom.context,
                                                       size(img.sound_program, img.sound_program_rom),
                                                       img.pcm1_rom.read, img.pcm1_rom.context,
                                                       size(img.pcm1, img.pcm1_rom),
                                                       img.pcm2_rom.read, img.pcm2_rom.context,
                                                       size(img.pcm2, img.pcm2_rom));
        } else if (img.sound_program_rom.data || img.pcm1_rom.data || img.pcm2_rom.data)
            sound_ = std::make_unique<snd::SoundBoard>(program, size(img.sound_program, img.sound_program_rom),
                                                       pcm1, size(img.pcm1, img.pcm1_rom),
                                                       pcm2, size(img.pcm2, img.pcm2_rom));
        else sound_ = std::make_unique<snd::SoundBoard>(img.sound_program, img.pcm1, img.pcm2);
    }
    if (compact_memory && sound_) {
        std::vector<uint8_t>().swap(img.sound_program);
        std::vector<uint8_t>().swap(img.pcm1);
        std::vector<uint8_t>().swap(img.pcm2);
    }
    board_ = std::make_unique<M2Board>(std::move(img), compact_memory, bool(main_loader));
#if defined(N64) && defined(DAYTONA_N64_LOW_MEMORY)
    board_->video().set_native_output(true);
    // The N64's software rasterizer is CPU-bound. Use the coarsest source
    // mip and one texel fetch per pixel while keeping full ROM textures intact.
    board_->video().set_texture_quality(10);
    board_->video().set_nearest_textures(true);
#endif
    if (main_loader) board_->install_main_data(main_loader());
    cpu_ = std::make_unique<Cpu>(board_.get());
    ls_ = std::make_unique<Lockstep>(*cpu_);
    board_->attach(*cpu_, *ls_);
    cpu_->reset();
    env_ = std::make_unique<gen::Env>(gen::Env{*cpu_, *ls_});
    ls_->add_callback(kProbe, [this] { probe(); });
}

void GameLoop::probe() {
    const uint64_t now = ls_->current_count();
    const bool idle = board_->in_idle_loop();
    if (in_vblank_) {
        if ((idle && now - vblank_start_ >= kProbe * 2) || now - vblank_start_ >= kVblankCap) {
            board_->vblank_end();
            in_vblank_ = false;
            frame_start_ = now;
            ++frames_;
            frame_done_ = true;
            ls_->set_end_count(now); // gen::run returns to the caller
        }
    } else {
        const uint64_t since = now - frame_start_;
        if ((idle && since >= kMinFrame) || since >= kFrameCap) {
            board_->io().inputs = inputs_;
            board_->vblank_start();
            in_vblank_ = true;
            vblank_start_ = now;
        }
    }
    ls_->add_callback(now + kProbe, [this] { probe(); });
}

void GameLoop::run_frame(const Inputs &inputs, FrameTiming *timing, bool defer_sound) {
    using Clock = std::chrono::steady_clock;
    const auto start = timing ? Clock::now() : Clock::time_point{};
    board_->tgp().begin_profile(timing != nullptr);
    board_->begin_frame_profile(timing != nullptr);
    inputs_ = inputs;
    frame_done_ = false;
    ls_->set_end_count(UINT64_MAX);
    ls_->begin_fast_frame();
    while (!frame_done_) {
        if (!gen::has_code(cpu_->m_IP)) {
            char b[128];
            std::snprintf(b, sizeof b, "no recompiled code at %08x: add it to the seeds", unsigned(cpu_->m_IP));
            throw Fatal(b);
        }
        gen::run(*env_);
    }
    ls_->end_fast_frame();
    const auto machine_end = timing ? Clock::now() : Clock::time_point{};
    // The sound board runs alongside: this frame's command bytes go down the
    // serial line, and it advances one frame of board time.
    if (sound_ && !defer_sound) advance_sound_frame(timing);
    if (timing) {
        timing->machine_ms = std::chrono::duration<double, std::milli>(machine_end-start).count();
        if (!defer_sound) timing->sound_ms = std::chrono::duration<double, std::milli>(Clock::now()-machine_end).count();
        timing->tgp_ms = board_->tgp().profile_ms();
        timing->geometry_ms = board_->geometry_profile_ms();
        timing->video_ms = board_->video_profile_ms();
        timing->tgp_runs = board_->tgp().profile_runs();
    }
}

void GameLoop::advance_sound_frame(FrameTiming *timing) {
    if (!sound_) return;
    using Clock = std::chrono::steady_clock;
    const auto start = timing ? Clock::now() : Clock::time_point{};
    const std::vector<uint8_t> bytes = board_->take_sound_bytes();
    sound_->send(bytes.data(), bytes.size());
    sound_->advance(1.0 / kFrameHz);
    if (timing) timing->sound_ms = std::chrono::duration<double, std::milli>(Clock::now()-start).count();
}

} // namespace rt
