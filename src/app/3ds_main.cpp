#include "runtime/3ds_geo_feedback.h"
#include "runtime/3ds_geo_options.h"
#include "runtime/3ds_geo_profile.h"
#include "runtime/3ds_geo_overlap.h"
// New Nintendo 3DS front end; shared runtime and rasterizer.
#include <3ds.h>
#undef BIT
#include "runtime/game_loop.h"
#include "runtime/zip.h"
#include "app/coin_gate.h"
#include "app/3ds_controls.h"
#include "app/3ds_geometry_worker.h"
#include "app/3ds_stereo.h"
#include "app/3ds_framebuffer.h"
#if defined(DAYTONA_3DS_PICA200)
#include "app/3ds_gpu.h"
#endif
#include "app/presentation_cadence.h"
#include "daytona_assets.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <malloc.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <utility>
#include <vector>

// Record allocation failure without allocating/logging in the exhausted heap.
// The return address can be resolved against this exact ELF after the test.
namespace {
size_t failed_new_bytes = 0;
void *failed_new_caller = nullptr;
struct mallinfo failed_new_heap{};
}
void *operator new(std::size_t size) {
    if (void *p = std::malloc(size ? size : 1)) return p;
    failed_new_bytes = size;
    failed_new_caller = __builtin_return_address(0);
    failed_new_heap = mallinfo();
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
// Do not consume the entire process resource-limit remainder: a real console
// can reject that allocation even when the emulator accepts it. The compact
// runtime's measured replay peak fits in this bounded application heap.
extern "C" {
#if defined(DAYTONA_3DS_PICA200)
u32 __ctru_heap_size = 82u * 1024u * 1024u;
u32 __ctru_linear_heap_size = 8u * 1024u * 1024u;
#else
u32 __ctru_heap_size = 88u * 1024u * 1024u;
u32 __ctru_linear_heap_size = 2u * 1024u * 1024u;
#endif
}
namespace {
constexpr const char *Root = "sdmc:/3ds/daytona/";
std::unique_ptr<rt::GameLoop> game;
bool native_gpu = false;
app::CoinGate gate;
bool paused = false, low_textures = true, wide = true, diagnostic_mode = false;
unsigned texture_quality = 1; // 0=100%, 1=50%, 2=25% (GPU only).
const char *texture_quality_label() { return texture_quality==2 ? "25%" : texture_quality==1 ? "50%" : "100%"; }
int fast_render = 0;
unsigned distance_level = 2, stereo_depth = 1;
unsigned resolution_level = 2;
unsigned settings_row = 0;
unsigned traffic_car_limit = 20;
unsigned render_workers = 2;
uint64_t capture_frame = 0;
unsigned status_log_stride = 0;
uint64_t status_log_frame = 0;
bool benchmark_mode = false, benchmark_race = false;
unsigned benchmark_warmup = 150, benchmark_frames = 600;
u64 benchmark_start = 0, benchmark_last = 0;
uint64_t benchmark_start_frame = 0, benchmark_start_displayed = 0;
std::vector<u64> benchmark_frame_ticks;
struct BenchmarkGpuSample {
    float wait_ms=0, upload_ms=0, geometry_ms=0, composite_ms=0;
    unsigned uploads=0, polygons=0, vertices=0, batches=0, resets=0, textures=0;
    size_t texture_bytes=0;
};
struct BenchmarkPhaseBin {
    unsigned frames=0;
    u64 total=0, game=0, gpu=0, audio_wait=0, audio_push=0;
    u64 game_max=0, gpu_max=0;
    double gpu_wait=0, gpu_upload=0, gpu_geometry=0, gpu_composite=0;
    double machine_ms=0,tgp_ms=0,geometry_ms=0,video_ms=0;
    uint64_t uploads=0, polygons=0, vertices=0, batches=0, resets=0, textures=0, texture_bytes=0;
    uint64_t tgp_runs=0;unsigned cpu_samples=0;
};
std::array<BenchmarkPhaseBin,18> benchmark_phase_bins{};
float render_scale() {
    const float scales[] = {1.f, .75f, .5f, .25f, .125f};
    return diagnostic_mode ? (fast_render == 2 ? .25f : fast_render == 1 ? .5f : 1.f) : scales[resolution_level];
}
std::pair<int, int> sampled_layer_resolution() {
    const float scale = native_gpu ? 1.f : render_scale();
    return {std::max(1, int((wide ? 400 : 310) * scale)),
            std::max(1, int(240 * scale))};
}
void configure_sampled_layers(rt::Video &video) {
    const auto [width, height] = sampled_layer_resolution();
    video.set_sampled_layer_output(!diagnostic_mode, width, height);
}
app::ThreeDsControls controller;
unsigned display_limit = 0;
float simulation_speed = 0;
uint64_t displayed_total = 0;
static_assert(KEY_A == app::ThreeDsButtons::A && KEY_B == app::ThreeDsButtons::B &&
    KEY_SELECT == app::ThreeDsButtons::Select && KEY_START == app::ThreeDsButtons::Start &&
    KEY_DUP == app::ThreeDsButtons::Up && KEY_DRIGHT == app::ThreeDsButtons::Right &&
    KEY_DDOWN == app::ThreeDsButtons::Down && KEY_DLEFT == app::ThreeDsButtons::Left &&
    KEY_L == app::ThreeDsButtons::L && KEY_R == app::ThreeDsButtons::R);

std::string path(const char *name) { return std::string(Root) + name; }

bool benchmark_step(u64 game_ticks,u64 gpu_ticks,u64 audio_wait_ticks,u64 audio_push_ticks,const BenchmarkGpuSample &gpu_sample,const rt::GameLoop::FrameTiming &cpu_sample,bool cpu_sampled) {
    const u64 now=svcGetSystemTick();
    if(!benchmark_start) {
        if(game->frames()<benchmark_warmup) return false;
        benchmark_start=benchmark_last=now;
        benchmark_start_frame=game->frames();
        benchmark_start_displayed=displayed_total;
        return false;
    }
    auto &bin=benchmark_phase_bins[benchmark_frame_ticks.size()/100];
    ++bin.frames;bin.total+=now-benchmark_last;bin.game+=game_ticks;bin.gpu+=gpu_ticks;
    bin.audio_wait+=audio_wait_ticks;bin.audio_push+=audio_push_ticks;
    bin.game_max=std::max(bin.game_max,game_ticks);bin.gpu_max=std::max(bin.gpu_max,gpu_ticks);
    bin.gpu_wait+=gpu_sample.wait_ms;bin.gpu_upload+=gpu_sample.upload_ms;
    bin.gpu_geometry+=gpu_sample.geometry_ms;bin.gpu_composite+=gpu_sample.composite_ms;
    if(cpu_sampled){++bin.cpu_samples;bin.machine_ms+=cpu_sample.machine_ms;bin.tgp_ms+=cpu_sample.tgp_ms;bin.geometry_ms+=cpu_sample.geometry_ms;bin.video_ms+=cpu_sample.video_ms;bin.tgp_runs+=cpu_sample.tgp_runs;}
    bin.uploads+=gpu_sample.uploads;bin.polygons+=gpu_sample.polygons;
    bin.vertices+=gpu_sample.vertices;bin.batches+=gpu_sample.batches;
    bin.resets+=gpu_sample.resets;bin.textures+=gpu_sample.textures;bin.texture_bytes+=gpu_sample.texture_bytes;
    benchmark_frame_ticks.push_back(now-benchmark_last);
    benchmark_last=now;
    if(benchmark_frame_ticks.size()<benchmark_frames) return false;
    // Stop the timed interval before sorting or performing any file I/O.
    const double seconds=double(now-benchmark_start)/SYSCLOCK_ARM11;
    const auto simulated=game->frames()-benchmark_start_frame;
    const auto displayed=displayed_total-benchmark_start_displayed;
    std::sort(benchmark_frame_ticks.begin(),benchmark_frame_ticks.end());
    const auto n=benchmark_frame_ticks.size();
    const double ms=1000.0/SYSCLOCK_ARM11;
    const auto slow=std::count_if(benchmark_frame_ticks.begin(),benchmark_frame_ticks.end(),
        [](u64 ticks){return double(ticks)*30.0>SYSCLOCK_ARM11;});
    FILE *f=std::fopen(path("benchmark-result.txt").c_str(),"w");
    if(!f) throw std::runtime_error("Cannot write benchmark result");
    const int wrote=std::fprintf(f,
        "complete=1 start_frame=%llu end_frame=%llu simulated=%llu displayed=%llu elapsed_seconds=%.6f simulation_fps=%.3f display_fps=%.3f game_speed_percent=%.3f frame_p50_ms=%.3f frame_p95_ms=%.3f frame_max_ms=%.3f frames_over_33ms=%llu scale=%.3f wide=%d slider=%.3f low_textures=%d distance=%u traffic=%u display_limit=%u gpu=%d race=%d texture_percent=%u\n",
        (unsigned long long)benchmark_start_frame,(unsigned long long)game->frames(),
        (unsigned long long)simulated,(unsigned long long)displayed,seconds,
        simulated/seconds,displayed/seconds,100.0*simulated/seconds/rt::GameLoop::kFrameHz,
        benchmark_frame_ticks[(n-1)/2]*ms,benchmark_frame_ticks[(95*n+99)/100-1]*ms,
        benchmark_frame_ticks.back()*ms,(unsigned long long)slow,render_scale(),wide,
        osGet3DSliderState(),low_textures,distance_level,traffic_car_limit,display_limit,native_gpu,benchmark_race,100u>>texture_quality);
    bool phases_written=true;
    for(size_t i=0;i<benchmark_phase_bins.size();++i) {
        const auto &bin=benchmark_phase_bins[i];
        if(!bin.frames) break;
        const double average_ms=ms/bin.frames;
        if(std::fprintf(f,"phase_bin=%lu start_frame=%llu frames=%u total_mean_ms=%.3f game_mean_ms=%.3f gpu_submit_mean_ms=%.3f audio_wait_mean_ms=%.3f audio_push_mean_ms=%.3f game_max_ms=%.3f gpu_submit_max_ms=%.3f gpu_wait_mean_ms=%.3f gpu_upload_mean_ms=%.3f gpu_geometry_mean_ms=%.3f gpu_composite_mean_ms=%.3f cpu_samples=%u machine_sample_ms=%.3f tgp_sample_ms=%.3f geo_sample_ms=%.3f video_sample_ms=%.3f tgp_runs_sample=%.1f uploads_mean=%.3f polygons_mean=%.3f vertices_mean=%.3f batches_mean=%.3f cache_resets=%llu textures_mean=%.3f texture_bytes_mean=%.0f\n",
            (unsigned long)i,(unsigned long long)(benchmark_start_frame+1+i*100),bin.frames,
            bin.total*average_ms,bin.game*average_ms,bin.gpu*average_ms,
            bin.audio_wait*average_ms,bin.audio_push*average_ms,bin.game_max*ms,bin.gpu_max*ms,
            bin.gpu_wait/bin.frames,bin.gpu_upload/bin.frames,bin.gpu_geometry/bin.frames,bin.gpu_composite/bin.frames,bin.cpu_samples,bin.cpu_samples?bin.machine_ms/bin.cpu_samples:0,bin.cpu_samples?bin.tgp_ms/bin.cpu_samples:0,bin.cpu_samples?bin.geometry_ms/bin.cpu_samples:0,bin.cpu_samples?bin.video_ms/bin.cpu_samples:0,bin.cpu_samples?double(bin.tgp_runs)/bin.cpu_samples:0,
            double(bin.uploads)/bin.frames,double(bin.polygons)/bin.frames,double(bin.vertices)/bin.frames,double(bin.batches)/bin.frames,
            (unsigned long long)bin.resets,double(bin.textures)/bin.frames,double(bin.texture_bytes)/bin.frames)<0) phases_written=false;
    }
    // Sample after the timed interval, while the game's texture cache is alive.
    // Cache byte totals alone do not include its ordinary-heap bookkeeping.
    const auto end_heap=mallinfo();
    const bool heap_written=std::fprintf(f,"heap_used_bytes=%lu heap_free_bytes=%lu linear_free_bytes=%lu\n",
        (unsigned long)end_heap.uordblks,(unsigned long)end_heap.fordblks,
        (unsigned long)linearSpaceFree())>=0;
    const int closed=std::fclose(f);
    if(wrote<0 || !phases_written || !heap_written || closed!=0) throw std::runtime_error("Benchmark result write failed");
    return true;
}

bool exists(const char *name) { FILE *f = std::fopen(path(name).c_str(), "rb"); if (!f) return false; std::fclose(f); return true; }
template<class T> void read_save(const char *name, T &data) {
    FILE *f = std::fopen(path(name).c_str(), "rb"); if (!f) return;
    std::fseek(f, 0, SEEK_END); const long length = std::ftell(f); std::rewind(f);
    if (length == long(data.size())) std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
}
template<class T> bool write_save(const char *name, const T &data) {
    const auto final = path(name), temp = final + ".tmp";
    FILE *f = std::fopen(temp.c_str(), "wb"); if (!f) return false;
    const bool wrote = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    const bool closed = std::fclose(f) == 0;
    return wrote && closed && std::rename(temp.c_str(), final.c_str()) == 0;
}
void checkpoint() {
    if (!game || diagnostic_mode) return;
    write_save("settings/ioboard_eeprom.bin", game->board().io().eeprom);
    write_save("settings/backup_ram.bin", game->board().backup_ram());
    const std::array<unsigned char, 4> settings{1, (unsigned char)low_textures, (unsigned char)wide, (unsigned char)fast_render};
    write_save("settings/display.bin", settings);
    const std::array<unsigned char,2> textures{1,static_cast<unsigned char>(texture_quality)};
    write_save("settings/textures.bin", textures);
    const std::array<unsigned char, 1> cadence{(unsigned char)display_limit};
    write_save("settings/presentation.bin", cadence);
    const std::array<unsigned char, 4> quality{2, (unsigned char)distance_level, (unsigned char)stereo_depth, (unsigned char)resolution_level};
    write_save("settings/quality.bin", quality);
    const std::array<unsigned char, 1> traffic{(unsigned char)traffic_car_limit};
    write_save("settings/traffic.bin", traffic);
    FILE *f = std::fopen(path("settings/pending.txt").c_str(), "w");
    if (f) { std::fprintf(f, "%d\n", gate.pending); std::fclose(f); }
}
void lifecycle(APT_HookType event, void*) {
    if (event == APTHOOK_ONSUSPEND || event == APTHOOK_ONSLEEP || event == APTHOOK_ONEXIT) {
        checkpoint(); paused = true; gate.suspend();
    }
}
std::vector<uint8_t> load_main_data();
rt::M2Board::Images load_images() {
    rt::M2Board::Images images;
    // Reserve the largest final ROM bank first, before temporary loaders can
    // divide free memory into blocks too small for this 16 MiB allocation.
    images.main_data = load_main_data();
    for (const auto &asset : daytona_assets) {
        if (asset.member == &rt::M2Board::Images::main_data) continue;
        std::printf("Loading %s\n", asset.name);
        FILE *progress = std::fopen(path("startup.log").c_str(), "a");
        if (progress) { std::fprintf(progress, "loading %s\n", asset.name); std::fclose(progress); }
        FILE *f = std::fopen(path((std::string("data/") + asset.name).c_str()).c_str(), "rb");
        if (!f) throw std::runtime_error(std::string("Missing data: ") + asset.name);
        std::fseek(f, 0, SEEK_END); const long length = std::ftell(f); std::rewind(f);
        if (length != long(asset.size)) { std::fclose(f); throw std::runtime_error(std::string("Wrong file size: ") + asset.name); }
        auto &bytes = images.*(asset.member); bytes.resize(asset.size);
        const bool read = std::fread(bytes.data(), 1, bytes.size(), f) == bytes.size(); std::fclose(f);
        if (!read || rt::crc32(bytes.data(), bytes.size()) != asset.crc) throw std::runtime_error(std::string("Damaged data: ") + asset.name);
        if (!aptMainLoop()) throw std::runtime_error("Loading cancelled");
    }
    // M2Board retains these owned vectors. Its existing ROM-view interface
    // can read their verified, aligned storage without a temporary 16 MiB
    // typed copy of each bank during Geo construction.
    if ((reinterpret_cast<uintptr_t>(images.polygons.data()) & 3u) ||
        (reinterpret_cast<uintptr_t>(images.textures.data()) & 1u))
        throw std::runtime_error("Unaligned geometry ROM storage");
    if (images.copro_data.size() == 0x800000 &&
        std::all_of(images.copro_data.begin()+0x400000,images.copro_data.end(),[](uint8_t b){return b==0;})) {
        images.copro_data.resize(0x400000);images.copro_data.shrink_to_fit();
    }
    images.copro_tables_rom = {images.copro_tables.data(), images.copro_tables.size()};
    images.copro_data_rom = {images.copro_data.data(), images.copro_data.size()};
    images.polygons_rom = {images.polygons.data(), images.polygons.size()};
    images.textures_rom = {images.textures.data(), images.textures.size()};
    FILE *ready=std::fopen(path("startup.log").c_str(),"a");
    if(ready){std::fprintf(ready,"images verified heap_used=%lu\n",(unsigned long)mallinfo().uordblks);std::fclose(ready);}
    return images;
}
std::vector<uint8_t> load_main_data() {
    for (const auto &asset : daytona_assets) {
        if (asset.member != &rt::M2Board::Images::main_data) continue;
        FILE *raw = std::fopen(path((std::string("data/") + asset.name).c_str()).c_str(), "rb");
        if (!raw) throw std::runtime_error("Missing main ROM");
        std::unique_ptr<FILE, decltype(&std::fclose)> file(raw, &std::fclose);
        std::fseek(raw, 0, SEEK_END); const long length = std::ftell(raw); std::rewind(raw);
        if ((asset.size != 0x1000000 && asset.size != 0x2000000) || length != long(asset.size))
            throw std::runtime_error("Wrong main ROM size");
        std::vector<uint8_t> bytes(0x1000000);
        if (std::fread(bytes.data(), 1, bytes.size(), raw) != bytes.size()) throw std::runtime_error("Unreadable main ROM");
        uint32_t crc = rt::crc32(bytes.data(), bytes.size());
        std::array<uint8_t, 4096> padding{};
        for (size_t offset = bytes.size(); offset < asset.size; offset += padding.size()) {
            if (std::fread(padding.data(), 1, padding.size(), raw) != padding.size() ||
                !std::all_of(padding.begin(), padding.end(), [](uint8_t value) { return value == 0; }))
                throw std::runtime_error("Damaged main ROM padding");
            crc = rt::crc32(padding.data(), padding.size(), crc);
            if (!aptMainLoop()) throw std::runtime_error("Loading cancelled");
        }
        if (crc != asset.crc) throw std::runtime_error("Damaged main ROM");
        return bytes;
    }
    throw std::runtime_error("Missing main ROM manifest");
}
void write_frame(const std::vector<uint32_t> &pixels, gfx3dSide_t eye) {
    auto *fb = gfxGetFramebuffer(GFX_TOP, eye, nullptr, nullptr);
#if defined(DAYTONA_3DS_RGBA_FRAMEBUFFER)
    app::write_rgba8_top_frame(pixels, fb);
#else
    app::write_rgb24_top_frame(pixels, fb);
#endif
}
class Audio {
    static constexpr size_t Capacity = 1024, Slots = 8;
    std::array<ndspWaveBuf, Slots> buffers_{};
    double carry_ = 0;
    bool initialized_ = false;
public:
    bool enabled = false;
    uint64_t queued = 0, completed = 0;
    float peak = 0;
    Audio() {
        if (R_FAILED(ndspInit())) return;
        initialized_ = enabled = true;
        ndspSetOutputMode(NDSP_OUTPUT_STEREO); ndspChnReset(0);
        ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE); ndspChnSetRate(0, 44100);
        ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
        for (auto &b : buffers_) {
            b.data_pcm16 = static_cast<s16*>(linearAlloc(Capacity * 2 * sizeof(s16)));
            if (!b.data_pcm16) { enabled = false; break; }
        }
    }
    ~Audio() {
        // Stop the DSP before releasing memory it may still read.
        if (initialized_) ndspExit();
        for (auto &b : buffers_) if (b.data_pcm16) linearFree(b.data_pcm16);
    }
    void clear() { if (enabled) ndspChnWaveBufClear(0); }
    void push(snd::SoundBoard &sound) {
        auto fm = sound.take_fm(), pcm = sound.take_pcm();
        if (!enabled) return;
        ndspWaveBuf *buf = nullptr;
        for (auto &b : buffers_) if (b.status == NDSP_WBUF_FREE || b.status == NDSP_WBUF_DONE) {
            if (b.status == NDSP_WBUF_DONE) ++completed;
            buf = &b; break;
        }
        if (!buf) return;
        carry_ += 44100.0 / rt::GameLoop::kFrameHz;
        const size_t frames = size_t(carry_); carry_ -= frames;
        if (!frames || frames > Capacity) return;
        auto sample = [](const std::vector<float> &src, size_t j, size_t total, size_t channel) {
            const size_t count = src.size() / 2; if (!count) return 0.0f;
            const double pos = double(j) * count / total;
            const size_t a = std::min(size_t(pos), count - 1), b = std::min(a + 1, count - 1);
            return std::lerp(src[a * 2 + channel], src[b * 2 + channel], float(pos - a));
        };
        for (size_t i = 0; i < frames; ++i) for (size_t ch = 0; ch < 2; ++ch) {
            const float v = .8f * (sample(fm, i, frames, ch) + sample(pcm, i, frames, ch));
            if (diagnostic_mode) peak = std::max(peak, std::abs(v));
            buf->data_pcm16[i * 2 + ch] = s16(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767));
        }
        buf->nsamples = frames; buf->looping = false;
        DSP_FlushDataCache(buf->data_pcm16, frames * 2 * sizeof(s16)); ndspChnWaveBufAdd(0, buf);
        ++queued;
    }
};
// Run sound-board emulation on New 3DS processor 1 while the main thread
// prepares the next image and processor 2 helps rasterize it. Diagnostics keep
// the serial path so worker-parity tests can claim both available workers.
class SoundWorker {
    Thread thread_ = nullptr;
    LightEvent work_{}, done_{};
    bool stopping_ = false, active_ = false, changed_cpu_limit_ = false;
    u32 previous_cpu_limit_ = 0;
    snd::SoundBoard *sound_ = nullptr;
    std::vector<uint8_t> bytes_;
    rt::GameLoop::FrameTiming *timing_ = nullptr;

    static void entry(void *self) {
        auto &worker = *static_cast<SoundWorker *>(self);
        for (;;) {
            LightEvent_Wait(&worker.work_);
            if (worker.stopping_) return;
            const u64 start = svcGetSystemTick();
            worker.sound_->send(worker.bytes_.data(), worker.bytes_.size());
            worker.sound_->advance(1.0 / rt::GameLoop::kFrameHz);
            if (worker.timing_)
                worker.timing_->sound_ms = double(svcGetSystemTick() - start) * 1000.0 / SYSCLOCK_ARM11;
            worker.sound_ = nullptr;
            worker.timing_ = nullptr;
            worker.bytes_.clear();
            LightEvent_Signal(&worker.done_);
        }
    }

    void restore_cpu_limit() {
        if (changed_cpu_limit_) {
            APT_SetAppCpuTimeLimit(previous_cpu_limit_);
            changed_cpu_limit_ = false;
        }
    }

public:
    explicit SoundWorker(bool enabled) {
        if (!enabled) return;
        LightEvent_Init(&work_, RESET_ONESHOT);
        LightEvent_Init(&done_, RESET_ONESHOT);
        s32 priority = 0x30;
        svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
        if (R_FAILED(APT_GetAppCpuTimeLimit(&previous_cpu_limit_)) ||
            R_FAILED(APT_SetAppCpuTimeLimit(80))) return;
        changed_cpu_limit_ = true;
        thread_ = threadCreate(entry, this, 64 * 1024, priority, 1, false);
        if (!thread_) restore_cpu_limit();
    }

    ~SoundWorker() {
        wait();
        if (thread_) {
            stopping_ = true;
            LightEvent_Signal(&work_);
            threadJoin(thread_, U64_MAX);
            threadFree(thread_);
        }
        restore_cpu_limit();
    }

    void start(rt::GameLoop &loop, rt::GameLoop::FrameTiming *timing) {
        wait();
        if (!loop.sound()) return;
        if (!thread_) {
            loop.advance_sound_frame(timing);
            return;
        }
        bytes_ = loop.board().take_sound_bytes();
        sound_ = loop.sound();
        timing_ = timing;
        active_ = true;
        LightEvent_Signal(&work_);
    }

    void wait() {
        if (active_) {
            LightEvent_Wait(&done_);
            active_ = false;
        }
    }
};
rt::Inputs controls(u32 held, u32 down) {
    circlePosition circle{}; hidCircleRead(&circle);
    return controller.sample(held, down, int(circle.dx));
}
void status(float fps, bool audio) {
    const auto heap = mallinfo();
    const uint64_t status_frame=game?game->frames():0;
    const bool write_status=!benchmark_mode && (!status_log_stride || !status_frame || status_frame-status_log_frame>=status_log_stride);
    FILE *stats = write_status ? std::fopen(path("performance.txt").c_str(), "w") : nullptr;
    if (stats) {
        std::fprintf(stats, "frames=%llu fps=%.2f instructions=%llu heap_used=%lu heap_free=%lu callback_slots=%lu app_region=%lu displayed=%llu display_limit=%u simulation_speed=%.2f map_bytes=%lu\n",
            game ? (unsigned long long)game->frames() : 0, fps,
            game ? (unsigned long long)game->instructions() : 0,
            (unsigned long)heap.uordblks, (unsigned long)heap.fordblks,
            game ? (unsigned long)game->callback_slots() : 0,
            (unsigned long)osGetMemRegionSize(MEMREGION_APPLICATION),
            (unsigned long long)displayed_total, display_limit, simulation_speed,
            game ? (unsigned long)game->board().memory_map_bytes() : 0);
        std::fclose(stats);
        status_log_frame=status_frame;
    }
    consoleClear();
    std::printf("Daytona Arcade Port\nNew Nintendo 3DS\n\n");
    const char *distances[] = {"Full", "Long", "Medium", "Short", "Near", "Extreme"};
    if (paused) {
        const char *percentages[] = {"100%", "75%", "50%", "25%", "12.5%"};
        std::printf("PAUSED / SETTINGS\nUp/Down: select\nLeft/Right: change\nA or X: Resume\n\n");
        std::printf("%c Draw distance: %s\n", settings_row == 0 ? '>' : ' ', distances[distance_level]);
        std::printf("%c Resolution: %s\n", settings_row == 1 ? '>' : ' ', percentages[resolution_level]);
        std::printf("%c Textures: %s\n", settings_row == 2 ? '>' : ' ', texture_quality_label());
        std::printf("%c Aspect: %s\n", settings_row == 3 ? '>' : ' ', wide ? "Widescreen" : "Native");
        std::printf("%c 3D depth: %s\n", settings_row == 4 ? '>' : ' ', stereo_depth == 0 ? "Low" : stereo_depth == 1 ? "Medium" : "High");
        if (display_limit) std::printf("%c Display limit: %u FPS\n", settings_row == 5 ? '>' : ' ', display_limit);
        else std::printf("%c Display limit: native\n", settings_row == 5 ? '>' : ' ');
        std::printf("%c Traffic cars: %u\n", settings_row == 6 ? '>' : ' ', traffic_car_limit);
        return;
    }
    std::printf("Draw distance: %s\n3D depth: %s\n", distances[distance_level], stereo_depth == 0 ? "Low" : stereo_depth == 1 ? "Medium" : "High");
    const char *resolutions[] = {"100%", "75%", "50%", "25%", "12.5%"};
    std::printf("Resolution: %s\n", resolutions[resolution_level]);
    if (display_limit) std::printf("Display limit: %u FPS\n", display_limit);
    else std::printf("Display limit: native\n");
    std::printf("Game speed: %.0f%%\n", simulation_speed);
    std::printf("Circle Pad: steer\nA: gas   B: brake\nL/R: gears 1-4\nD-pad: camera 1-4\nSELECT: insert coin\nSTART: start / confirm\nX: pause / settings\nSELECT + START: exit\n\n");
        std::printf("Queued coins: %d   Gear: %d\nTraffic cars: %u\nTextures: %s\n3D: use the 3D slider\n%.1f FPS\nAudio: %s\n", gate.pending, controller.gear, traffic_car_limit, texture_quality_label(), fps, audio ? "on" : "unavailable");
}
void diagnostic_stage(const char *stage) {
    if (game && game->frames() < 100) {
        FILE *f = std::fopen(path("frame-stages.log").c_str(), "a");
        if (f) { std::fprintf(f, "%llu %s\n", (unsigned long long)game->frames(), stage); std::fclose(f); }
    }
}
void diagnostic_dump(const char *name, const std::vector<uint32_t> &pixels) {
    FILE *f = std::fopen(path(name).c_str(), "wb"); if (!f) return;
    std::fprintf(f, "P6\n%u 240\n255\n", unsigned(pixels.size() / 240));
    for (auto px : pixels) { std::fputc(px >> 16, f); std::fputc(px >> 8, f); std::fputc(px, f); }
    std::fclose(f);
}
}
int main() {
#if defined(DAYTONA_3DS_RGBA_FRAMEBUFFER)
    gfxInit(GSP_RGBA8_OES, GSP_BGR8_OES, false);
#else
    gfxInitDefault();
#endif
    gfxSet3D(true); consoleInit(GFX_BOTTOM, nullptr);
    bool new_model = false; APT_CheckNew3DS(&new_model);
    if (!new_model) {
        std::printf("This build requires a New 3DS.\nPress START to exit.\n");
        while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
        gfxExit(); return 1;
    }
    osSetSpeedupEnable(true);
    mkdir(path("settings").c_str(), 0777);
    aptHookCookie hook{}; aptHook(&hook, lifecycle, nullptr);
    try {
        std::printf("Daytona Arcade Port\nChecking game data...\n");
        const bool diagnostic = exists("diagnostic.txt");
        benchmark_mode = exists("benchmark.txt");
        if (benchmark_mode) {
            if (diagnostic) throw std::runtime_error("Benchmark cannot use diagnostic mode");
            FILE *f=std::fopen(path("benchmark.txt").c_str(),"r");
            unsigned warmup=150, frames=600;
            if(f){std::fscanf(f,"%u %u",&warmup,&frames);std::fclose(f);}
            benchmark_warmup=std::clamp(warmup,1u,6000u);
            benchmark_frames=std::clamp(frames,1u,1800u);
            benchmark_frame_ticks.reserve(benchmark_frames);
        }
        const bool stereo_depth_profile = !benchmark_mode && exists("stereo-depth-profile.txt");
        const bool geo_overlap_profile=!benchmark_mode && exists("geo-overlap-profile.txt");
        rt::geo_profile_3ds::dense=!benchmark_mode && exists("geo-dense-profile.txt");
        const bool geo_stage_profile = !benchmark_mode && (exists("geo-stage-profile.txt") || rt::geo_profile_3ds::dense);
        rt::geo_feedback_3ds::enabled=!benchmark_mode && exists("geo-feedback-profile.txt");
        rt::geo_options_3ds::skip_culled_lighting=exists("skip-culled-lighting.txt");
        rt::geo_options_3ds::direct_texture_reads=exists("direct-texture-reads.txt");
        rt::geo_options_3ds::verify_texture_reads=rt::geo_options_3ds::direct_texture_reads && !benchmark_mode && exists("verify-texture-reads.txt");
        rt::geo_options_3ds::reuse_polygon_scratch=exists("reuse-polygon-scratch.txt");
        rt::geo_options_3ds::verify_polygon_scratch=rt::geo_options_3ds::reuse_polygon_scratch && !benchmark_mode && exists("verify-polygon-scratch.txt");
        rt::geo_options_3ds::far_model_distance=exists("far-model-distance.txt");
        rt::geo_options_3ds::verify_far_model_distance=rt::geo_options_3ds::far_model_distance && !benchmark_mode && exists("verify-far-model-distance.txt");
        const bool profile_only = diagnostic && exists("diagnostic-profile.txt");
        const bool diagnostic_distance = diagnostic && exists("diagnostic-distance.txt");
        const bool diagnostic_no_dump = diagnostic && exists("diagnostic-no-dump.txt");
        const bool worker_check = diagnostic && exists("diagnostic-worker-check.txt");
        const bool late_check = diagnostic && exists("diagnostic-late-check.txt");
        diagnostic_mode = diagnostic;
#if defined(DAYTONA_3DS_PICA200)
        native_gpu = !diagnostic && !exists("software-renderer.txt") && !worker_check && !late_check;
#endif
        const bool hud_stereo = native_gpu && exists("gpu-hud-stereo.txt") && exists("gpu-raw-projection.txt") &&
            exists("gpu-stereo-chunks.txt") && exists("gpu-shader-stereo.txt");
        FILE *capture = std::fopen(path("capture-frame.txt").c_str(), "r");
        if (capture) { unsigned long long frame = 0; if (std::fscanf(capture, "%llu", &frame) == 1) capture_frame = frame; std::fclose(capture); }
        if (benchmark_mode) capture_frame=0;
        int validation_coins = 0;
        int validation_steering = 0;
        int validation_steering_start = 1500;
        int validation_steering_frames = 40;
        int validation_accelerator_start = 1760;
        int validation_accelerator_end = 2030;
        int validation_course_start = 0;
        int validation_steering2 = 0;
        int validation_steering_start2 = 2100;
        int validation_steering_frames2 = 40;
        int validation_accelerator_start2 = 0;
        int validation_accelerator_end2 = 6000;
        int validation_course_start2 = 0;
        if (!diagnostic) {
            FILE *f = std::fopen(path("validation-coins.txt").c_str(), "r");
            if (f) { std::fscanf(f, "%d", &validation_coins); std::fclose(f); }
            if (validation_coins < 1 || validation_coins > 3) validation_coins = 0;
            f = std::fopen(path("validation-steering.txt").c_str(), "r");
            if (f) {
                std::fscanf(f, "%d %d %d %d %d %d %d %d %d %d %d %d",
                    &validation_steering, &validation_steering_start, &validation_steering_frames,
                    &validation_accelerator_start, &validation_accelerator_end, &validation_course_start,
                    &validation_steering2, &validation_steering_start2, &validation_steering_frames2,
                    &validation_accelerator_start2, &validation_accelerator_end2, &validation_course_start2);
                std::fclose(f);
            }
            validation_steering = std::clamp(validation_steering, -96, 96);
            validation_steering_start = std::clamp(validation_steering_start, 1350, 2500);
            validation_steering_frames = std::clamp(validation_steering_frames, 1, 300);
            validation_accelerator_start = std::clamp(validation_accelerator_start, 1350, 2600);
            validation_accelerator_end = std::clamp(validation_accelerator_end, validation_accelerator_start, 2700);
            validation_course_start = validation_course_start ? std::clamp(validation_course_start, 1350, 2600) : 0;
            validation_steering2 = std::clamp(validation_steering2, -96, 96);
            validation_steering_start2 = std::clamp(validation_steering_start2, 1350, 2600);
            validation_steering_frames2 = std::clamp(validation_steering_frames2, 1, 300);
            validation_accelerator_start2 = std::clamp(validation_accelerator_start2, 0, 5999);
            validation_accelerator_end2 = std::clamp(validation_accelerator_end2, validation_accelerator_start2, 6000);
            validation_course_start2 = validation_course_start2 ? std::clamp(validation_course_start2, 1350, 2600) : 0;
        }
        benchmark_race = benchmark_mode && exists("benchmark-race.txt");
        if (benchmark_mode && validation_coins && !benchmark_race)
            throw std::runtime_error("Race benchmark requires explicit benchmark-race.txt");
        if (benchmark_race && (!validation_coins || benchmark_warmup < 3000))
            throw std::runtime_error("Race benchmark requires scripted inputs and warmup of at least 3000 frames");
        if (validation_coins) {
            std::array<unsigned char, 4> first{1, 0, 1, 0}, second{1, 1, 0, 1}, saved{};
            if (!write_save("settings/validation-save.bin", first) ||
                !write_save("settings/validation-save.bin", second))
                throw std::runtime_error("Save replacement failed");
            read_save("settings/validation-save.bin", saved);
            if (saved != second) throw std::runtime_error("Save round trip failed");
            FILE *f = std::fopen(path("validation.log").c_str(), "a");
            if (f) { std::fprintf(f, "save_replace_round_trip=1 coins=%d\n", validation_coins); std::fclose(f); }
        }
        if (diagnostic) fast_render = exists("diagnostic-fast.txt");
        if (diagnostic && exists("diagnostic-quarter.txt")) fast_render = 2;
        if (diagnostic_distance && exists("diagnostic-distance-level.txt")) {
            FILE *f = std::fopen(path("diagnostic-distance-level.txt").c_str(), "r");
            unsigned level = distance_level;
            if (f) { if (std::fscanf(f, "%u", &level) == 1 && level <= 5) distance_level = level; std::fclose(f); }
        }
        if (diagnostic && exists("diagnostic-display-40.txt")) display_limit = 40;
        if (diagnostic && exists("diagnostic-cars0.txt")) traffic_car_limit = 0;
        if (diagnostic && exists("diagnostic-cars5.txt")) traffic_car_limit = 5;
        if (diagnostic && exists("diagnostic-cars10.txt")) traffic_car_limit = 10;
        if (diagnostic && exists("diagnostic-cars40.txt")) traffic_car_limit = 40;
        game = std::make_unique<rt::GameLoop>(load_images(), true);
        game->board().geometry().lazy_native_count = native_gpu && exists("lazy-native-count.txt");
        game->board().geometry().verify_lazy_native = !benchmark_mode &&
            game->board().geometry().lazy_native_count && exists("verify-lazy-native.txt");
        if (game->board().geometry().verify_lazy_native) rt::geo_feedback_3ds::enabled=true;
        game->board().geometry().gpu_presentation_clip = native_gpu &&
            exists("gpu-raw-projection.txt") && exists("gpu-stereo-chunks.txt") &&
            exists("gpu-shader-stereo.txt") && exists("gpu-presentation-clip.txt");
        if (FILE *flag = std::fopen("sdmc:/3ds/daytona/clip-fast-accept.txt", "r")) {
            game->board().geometry().fast_clip_accept = true;
            std::fclose(flag);
        }
        if (FILE *flag = std::fopen("sdmc:/3ds/daytona/verify-fast-clip.txt", "r")) {
            game->board().geometry().verify_fast_clip = true;
            std::fclose(flag);
        }
        FILE *machine_ready=std::fopen(path("startup.log").c_str(),"a");
        if(machine_ready){std::fprintf(machine_ready,"machine ready heap_used=%lu\n",(unsigned long)mallinfo().uordblks);std::fclose(machine_ready);}
        if (!diagnostic) {
            read_save("settings/ioboard_eeprom.bin", game->board().io().eeprom);
            read_save("settings/backup_ram.bin", game->board().backup_ram());
            std::array<unsigned char, 4> settings{1, 1, 1, 0}; read_save("settings/display.bin", settings);
            if (settings[0] == 1) { low_textures = settings[1] != 0; wide = settings[2] != 0; fast_render = settings[3] <= 2 ? settings[3] : 0; }
            std::array<unsigned char, 1> cadence{0}; read_save("settings/presentation.bin", cadence);
            if (cadence[0] == 30 || cadence[0] == 40) display_limit = cadence[0];
            std::array<unsigned char, 4> quality{2, 2, 1, static_cast<unsigned char>(native_gpu ? 0 : 2)}; read_save("settings/quality.bin", quality);
            if (quality[0] == 2) { distance_level = std::min(unsigned(quality[1]), 5u); stereo_depth = std::min(unsigned(quality[2]), 2u); resolution_level = std::min(unsigned(quality[3]), 4u); }
            std::array<unsigned char, 1> traffic{20}; read_save("settings/traffic.bin", traffic);
            traffic_car_limit = traffic[0] >= 40 ? 40 : traffic[0] >= 20 ? 20 : traffic[0] >= 10 ? 10 : 5;
            std::array<unsigned char, 1> workers{2}; read_save("settings/workers.bin", workers);
            render_workers = workers[0] == 1 ? 1 : 2;
            FILE *f = std::fopen(path("settings/pending.txt").c_str(), "r");
            if (f) { int pending = 0; if (std::fscanf(f, "%d", &pending) == 1) gate.pending = std::clamp(pending, 0, 999); std::fclose(f); }
        }
        texture_quality=low_textures?1u:0u;
        if(native_gpu && low_textures && exists("gpu-quarter-textures.txt")) texture_quality=2;
        if(!diagnostic) {
            std::array<unsigned char,2> textures{0,0};
            read_save("settings/textures.bin",textures);
            if(textures[0]==1 && textures[1]<=2) texture_quality=textures[1];
        }
        if(!native_gpu && texture_quality==2) texture_quality=1;
        low_textures=texture_quality!=0;
        game->board().set_traffic_car_limit(traffic_car_limit);
        auto &video = game->board().video();
#if defined(DAYTONA_3DS_PICA200)
        std::unique_ptr<app::ThreeDsGpu> gpu;
        if (native_gpu) {
            video.release_software_targets();
            gpu=std::make_unique<app::ThreeDsGpu>();
        }
#endif
        video.enable_presentation(true, !diagnostic || profile_only);
        // Leave processor 1 to the sound worker and share stereo scanline
        // bands between the game core and processor 2.
        video.set_render_threads(native_gpu ? 1 : render_workers);
        video.set_parallel_stereo_eyes(true);
        video.set_tile_cache(true);
        game->board().set_video_write_tracking(true);
        video.set_widescreen(wide);
        configure_sampled_layers(video);
        const bool gpu_tile_layers=native_gpu && exists("gpu-tile-layers.txt");
        const bool validate_tile_layers=native_gpu && exists("validate-tile-spans.txt");
        video.set_tile_span_capture(gpu_tile_layers || validate_tile_layers);
        video.set_tile_span_output(gpu_tile_layers && !validate_tile_layers);
        video.set_low_textures(low_textures);
        video.set_render_scale(render_scale());
        video.set_nearest_textures(diagnostic ? fast_render != 0 : resolution_level != 0);
        FILE *progress = std::fopen(path("startup.log").c_str(), "a");
        if (progress) { std::fprintf(progress, "runtime ready texture_percent=%u\n",100u>>texture_quality); std::fclose(progress); }
        Audio audio;
        SoundWorker sound_worker(!diagnostic);
        std::unique_ptr<app::ThreeDsGeometryWorker> geometry_worker;
        if(native_gpu && !diagnostic && exists("geometry-worker.txt"))
            geometry_worker=std::make_unique<app::ThreeDsGeometryWorker>(game->board());
        game->board().verify_geometry_worker=!benchmark_mode && exists("verify-geometry-worker.txt");
        if(FILE *f=std::fopen(path("geometry-worker.log").c_str(),"w")) {
            std::fprintf(f,"available=%d snapshot_bytes=%u\n",geometry_worker && geometry_worker->available(),geometry_worker && geometry_worker->available()?131072u:0u);
            std::fclose(f);
        }
        progress = std::fopen(path("startup.log").c_str(), "a");
        if (progress) { std::fprintf(progress, "audio ready: %d\n", audio.enabled); std::fclose(progress); }
        if(FILE *stride=std::fopen(path("status-log-stride.txt").c_str(),"r")) {
            unsigned value=0;if(std::fscanf(stride,"%u",&value)==1) status_log_stride=std::min(value,600u);
            std::fclose(stride);
        }
        float convergence = 0, fps = 0;
        u64 last_status = svcGetSystemTick(), deadline = last_status;
        uint64_t count = 0;
        uint64_t last_simulated = game->frames();
        app::PresentationCadence cadence(rt::GameLoop::kFrameHz);
        cadence.set_limit(display_limit);
        const bool texture_menu_check = !diagnostic && !benchmark_mode &&
            !validation_coins && exists("validate-texture-menu.txt");
        unsigned texture_menu_step=0;
        u32 texture_menu_held=0;
        u32 validation_held = 0;
        status(0, audio.enabled);
        while (aptMainLoop()) {
            hidScanInput(); u32 held = hidKeysHeld(), down = hidKeysDown();
            if (validation_coins) {
                held = 0;
                const auto frame = game->frames();
                for (int coin = 0; coin < validation_coins; ++coin)
                    if (frame >= 1200 + 40 * coin && frame < 1210 + 40 * coin) held |= KEY_SELECT;
                // Release the accelerator during the coin/start transition.
                // Course Select requires a fresh pedal press after it appears;
                // holding A from before that screen never generated the edge.
                if (frame >= unsigned(validation_accelerator_start) && frame < unsigned(validation_accelerator_end)) held |= KEY_A;
                if (validation_accelerator_start2 && frame >= unsigned(validation_accelerator_start2) &&
                    frame < unsigned(validation_accelerator_end2)) held |= KEY_A;
                if ((frame >= 1400 && frame < 1410) || (frame >= 1600 && frame < 1610)) held |= KEY_START;
                if (validation_course_start && frame >= unsigned(validation_course_start) &&
                    frame < unsigned(validation_course_start + 10)) held |= KEY_START;
                if (validation_course_start2 && frame >= unsigned(validation_course_start2) &&
                    frame < unsigned(validation_course_start2 + 10)) held |= KEY_START;
                down = held & ~validation_held; validation_held = held;
            }
            // Isolated test fixture drives the normal menu input path. Its
            // counter advances while paused, unlike the simulation frame.
            if(texture_menu_check && game->frames()>=150 && texture_menu_step<18) {
                ++texture_menu_step;
                if(texture_menu_step==1 && (!native_gpu || texture_quality!=1 || paused || settings_row!=0))
                    throw std::runtime_error("Texture menu test requires fresh GPU half-texture settings");
                unsigned expected=99;
                switch(texture_menu_step) {
                case 8: case 12: case 16: case 18: expected=2;break;
                case 10: expected=0;break;
                case 14: expected=1;break;
                }
                if(expected!=99) {
                    std::array<unsigned char,2> saved{255,255};
                    read_save("settings/textures.bin",saved);
                    if(texture_quality!=expected || low_textures!=(expected!=0) ||
                        settings_row!=2 || paused!=(texture_menu_step!=18) ||
                        saved[0]!=1 || saved[1]!=expected)
                        throw std::runtime_error("Texture menu navigation or saved value mismatch");
                    FILE *check=std::fopen(path("texture-menu-check.log").c_str(),"a");
                    if(!check) throw std::runtime_error("Cannot write texture menu check");
                    std::fprintf(check,"step=%u texture_percent=%u saved=1 paused=%d complete=%d\n",
                        texture_menu_step,100u>>expected,paused,texture_menu_step==18);
                    std::fclose(check);
                }
                held=0;
                switch(texture_menu_step) {
                case 1: held=KEY_X;break;
                case 3: case 5: held=KEY_DDOWN;break;
                case 7: case 9: case 15: held=KEY_DRIGHT;break;
                case 11: case 13: held=KEY_DLEFT;break;
                case 17: held=KEY_A;break;
                }
                down=held & ~texture_menu_held;
                texture_menu_held=held;
            }
            if ((held & (KEY_SELECT | KEY_START)) == (KEY_SELECT | KEY_START)) break;
            if (down & KEY_X) { paused = !paused; gate.suspend(); audio.clear(); status(fps, audio.enabled); }
            if (paused) {
                if (down & KEY_DUP) settings_row = (settings_row + 6) % 7;
                if (down & KEY_DDOWN) settings_row = (settings_row + 1) % 7;
                const int direction = (down & KEY_DRIGHT) ? 1 : (down & KEY_DLEFT) ? -1 : 0;
                if (direction) {
                    const auto cycle = [direction](unsigned value, unsigned count) { return unsigned((int(value) + direction + int(count)) % int(count)); };
                    switch (settings_row) {
                    case 0: distance_level = cycle(distance_level, 6); break;
                    case 1: resolution_level = cycle(resolution_level, 5); fast_render = resolution_level ? 1 : 0; video.set_render_scale(render_scale()); video.set_nearest_textures(resolution_level != 0); configure_sampled_layers(video); break;
                    case 2: texture_quality = cycle(texture_quality, native_gpu ? 3u : 2u); low_textures = texture_quality != 0; video.set_low_textures(low_textures); break;
                    case 3: wide = !wide; video.set_widescreen(wide); configure_sampled_layers(video); break;
                    case 4: stereo_depth = cycle(stereo_depth, 3); break;
                    case 5: { const unsigned limits[] = {0, 40, 30}; const unsigned index = display_limit == 40 ? 1 : display_limit == 30 ? 2 : 0; display_limit = limits[cycle(index, 3)]; cadence.set_limit(display_limit); break; }
                    case 6: {
                        const unsigned limits[] = {5, 10, 20, 40};
                        const unsigned index = traffic_car_limit >= 40 ? 3 : traffic_car_limit >= 20 ? 2 : traffic_car_limit >= 10 ? 1 : 0;
                        traffic_car_limit = limits[cycle(index, 4)];
                        game->board().set_traffic_car_limit(traffic_car_limit);
                        break;
                    }
                    }
                    checkpoint();
                }
                if (direction || (down & (KEY_DUP | KEY_DDOWN))) status(fps, audio.enabled);
                if (down & KEY_A) { paused = false; gate.suspend(); status(fps, audio.enabled); }
                deadline = svcGetSystemTick(); gspWaitForVBlank(); continue;
            }
            auto input = controls(held, down);
            if (validation_coins && validation_steering && game->frames() >= unsigned(validation_steering_start) &&
                game->frames() < unsigned(validation_steering_start + validation_steering_frames))
                input.steer = uint8_t(std::clamp(128 + validation_steering, 32, 224));
            else if (validation_coins && validation_steering2 && game->frames() >= unsigned(validation_steering_start2) &&
                game->frames() < unsigned(validation_steering_start2 + validation_steering_frames2))
                input.steer = uint8_t(std::clamp(128 + validation_steering2, 32, 224));
            if (validation_coins && (game->frames() == 1600 || game->frames() == 1680 ||
                game->frames() == 1700 || game->frames() == 1740 || game->frames() == 1800 ||
                game->frames() == 2000 || game->frames() == 2050 || game->frames() == 2100 ||
                game->frames() == 2140 || game->frames() == 2200 || game->frames() == 2250)) {
                FILE *f = std::fopen(path("validation.log").c_str(), "a");
                if (f) {
                    std::fprintf(f, "input frame=%llu steer=%u accel=%u brake=%u in0=%02x in1=%02x\n",
                        (unsigned long long)game->frames(), input.steer, input.accel, input.brake, input.in0, input.in1);
                    std::fclose(f);
                }
            }
            if (diagnostic) {
                input = rt::Inputs{};
                const auto frame = game->frames();
                if ((frame >= 1200 && frame < 1210) || (frame >= 1240 && frame < 1250) || (frame >= 1280 && frame < 1290)) input.in0 &= ~1u;
                if ((frame >= 1400 && frame < 1410) || (frame >= 1600 && frame < 1610) || (frame >= 2000 && frame < 2010) || (frame >= 2400 && frame < 2410)) input.in0 &= ~0x10u;
                if ((frame >= 1800 && frame < 1810) || (frame >= 2200 && frame < 2210)) input.in0 &= ~0x20u;
                if (frame >= 1400) input.accel = 224;
            }
            if (diagnostic) diagnostic_stage("before game");
            const bool native_checkpoint = late_check && (game->frames() + 1) % 60 == 0;
            // Render the preceding frame too, covering the board's 30Hz
            // mode where an odd frame reuses the preceding raster output.
            const auto native_phase = (game->frames() + 1) % 60;
            const bool native_pass = late_check && (native_phase == 0 || native_phase == 59);
            if (late_check) video.enable_presentation(true, !native_pass);
            // Sample normal play too: emulator timing cannot identify the
            // physical console's bottleneck. Only one frame in sixty is timed.
            const bool timing_sample = !benchmark_mode && (diagnostic || (game->frames() + 1) % 60 == 30);
            const bool game_profile_sample=timing_sample || (benchmark_mode && (game->frames()+1)%60==30);
            // Convergence is queried only once every 60 frames after it has
            // been initialized. Skip vertex collection and median selection
            // on intervening frames.
            const bool reference_needed = !convergence || (game->frames() + 1) % 60 == 0 ||
                (down & (KEY_DUP | KEY_DRIGHT | KEY_DDOWN | KEY_DLEFT));
            game->board().geometry().set_capture_reference_depth(reference_needed);
            const u64 game_begin = (timing_sample || benchmark_mode) ? svcGetSystemTick() : 0;
            rt::GameLoop::FrameTiming machine_timing;
            rt::geo_feedback_3ds::begin_frame();
            rt::geo_overlap_3ds::begin(geo_overlap_profile && game_profile_sample);
            rt::geo_profile_3ds::begin(geo_stage_profile && (game->frames()+1)%60==30);
            game->run_frame(diagnostic ? input : gate.sample(input, true), game_profile_sample ? &machine_timing : nullptr, true);
            const u64 game_end = (timing_sample || benchmark_mode) ? svcGetSystemTick() : 0;
            if(geometry_worker && !benchmark_mode && game->frames()%60==30) {
                if(FILE *f=std::fopen(path("geometry-worker.log").c_str(),"a")) {
                    const auto &b=game->board();
                    const double ms=1000.0/SYSCLOCK_ARM11;
                    std::fprintf(f,"frame=%llu jobs=%llu feedback_waits=%llu forced_checks=%llu copy_ms=%.6f wait_ms=%.6f\n",
                        (unsigned long long)game->frames(),(unsigned long long)b.geometry_jobs,
                        (unsigned long long)b.geometry_feedback_waits,(unsigned long long)b.geometry_forced_checks,
                        geometry_worker->copy_ticks*ms,geometry_worker->wait_ticks*ms);
                    std::fclose(f);
                }
            }
            if(rt::geo_overlap_3ds::active) {
                using namespace rt::geo_overlap_3ds;
                if(FILE *f=std::fopen(path("geo-overlap-profile.log").c_str(),"a")) {
                    const double ms=1000.0/SYSCLOCK_ARM11;
                    std::fprintf(f,"frame=%llu parsed=%d geometry_ms=%.6f tail_ms=%.6f feedback_reads=%u first_feedback_ms=%.6f\n",
                        (unsigned long long)game->frames(),tail_start!=0,geometry_ticks*ms,tail_ticks*ms,feedback_reads,first_feedback_ticks*ms);
                    std::fclose(f);
                }
            }
            if(rt::geo_options_3ds::verify_polygon_scratch && game->frames()%60==30) {
                if(FILE *f=std::fopen(path("geo-scratch-checks.log").c_str(),"a")) {
                    std::fprintf(f,"frame=%llu poisoned_polygons=%llu nested_fallbacks=%llu\n",
                        (unsigned long long)game->frames(),rt::geo_options_3ds::scratch_poisoned_polygons,
                        rt::geo_options_3ds::scratch_nested_fallbacks);
                    std::fclose(f);
                }
            }
            if(rt::geo_options_3ds::far_model_distance && !benchmark_mode && game->frames()%60==30) {
                if(FILE *f=std::fopen(path("far-model-distance.log").c_str(),"a")) {
                    std::fprintf(f,"frame=%llu objects=%llu polygons=%llu verified=%llu\n",
                        (unsigned long long)game->frames(),rt::geo_options_3ds::far_models,
                        rt::geo_options_3ds::far_polygons,rt::geo_options_3ds::far_verified);
                    std::fclose(f);
                }
            }
            if(rt::geo_options_3ds::verify_texture_reads && game->frames()%60==30) {
                if(FILE *f=std::fopen(path("texture-read-checks.log").c_str(),"a")) {
                    std::fprintf(f,"frame=%llu checked_words=%llu\n",(unsigned long long)game->frames(),rt::geo_options_3ds::texture_read_checks);
                    std::fclose(f);
                }
            }
            if (!benchmark_mode && game->board().geometry().gpu_presentation_clip && game->frames()%60==30) {
                const auto &g=game->board().geometry();
                if(FILE *f=std::fopen(path("gpu-presentation-clip.log").c_str(),"a")) {
                    std::fprintf(f,"frame=%llu native=%lu wide=%lu gpu_candidates=%u fallback=%u heap_used=%lu heap_free=%lu\n",
                        (unsigned long long)game->frames(),(unsigned long)g.polys.size(),
                        (unsigned long)g.wide_polys.size(),unsigned(g.gpu_clip_submitted),
                        unsigned(g.gpu_clip_fallback),(unsigned long)mallinfo().uordblks,
                        (unsigned long)mallinfo().fordblks);
                    std::fclose(f);
                }
            }
            if (!benchmark_mode && game->board().geometry().lazy_native_count && game->frames()%60==30) {
                const auto &g=game->board().geometry();
                if(FILE *f=std::fopen(path("lazy-native-profile.log").c_str(),"a")) {
                    std::fprintf(f,"frame=%llu active=%d oracle=%d native_list=%lu wide=%lu candidates=%lu bytes=%lu computations=%llu heap_free=%lu\n",
                        (unsigned long long)game->frames(),g.lazy_native_active(),g.verify_lazy_native,
                        (unsigned long)g.polys.size(),(unsigned long)g.wide_polys.size(),
                        (unsigned long)g.native_candidate_count(),(unsigned long)g.native_candidate_bytes(),
                        (unsigned long long)g.native_count_computations,(unsigned long)mallinfo().fordblks);
                    std::fclose(f);
                }
            }
            if(rt::geo_feedback_3ds::enabled) {
                auto &g=rt::geo_feedback_3ds::counts;
                if(++g.frames==60) {
                    if(FILE *f=std::fopen(path("geo-feedback-profile.log").c_str(),"a")) {
                        std::fprintf(f,"end_frame=%llu frames=%llu parsed_frames=%llu read_frames=%llu reads_before_parse=%llu reads_after_parse=%llu first_pc=%08x last_pc=%08x minimum=%u maximum=%u\n",
                            (unsigned long long)game->frames(),(unsigned long long)g.frames,
                            (unsigned long long)g.parsed_frames,(unsigned long long)g.read_frames,
                            (unsigned long long)g.before,(unsigned long long)g.after,
                            unsigned(g.first_pc),unsigned(g.last_pc),unsigned(g.minimum),unsigned(g.maximum));
                        std::fclose(f);
                    }
                    g={};
                }
            }
            if(rt::geo_profile_3ds::active) {
                const auto &g=rt::geo_profile_3ds::stats;
                const double ms=1000.0/SYSCLOCK_ARM11;
                FILE *f=std::fopen(path("geo-stage-profile.log").c_str(),"a");
                if(f){
                    std::fprintf(f,"frame=%llu parse_ms=%.6f polygons=%llu sampled_polygons=%llu sampled_polygon_ms=%.6f native_clip_samples=%llu native_clip_ms=%.6f wide_clip_samples=%llu wide_clip_ms=%.6f",
                        (unsigned long long)game->frames(),g.parse_ticks*ms,(unsigned long long)g.polygons,
                        (unsigned long long)g.polygon_samples,g.polygon_ticks*ms,
                        (unsigned long long)g.clip_samples[0],g.clip_ticks[0]*ms,
                        (unsigned long long)g.clip_samples[1],g.clip_ticks[1]*ms);
                    for(unsigned mode=0;mode<4;++mode) std::fprintf(f," mode%u_objects=%llu mode%u_ms=%.6f",mode,(unsigned long long)g.object_count[mode],mode,g.object_ticks[mode]*ms);
                    if(rt::geo_profile_3ds::dense) std::fprintf(f," dense=1 timer_pairs=%llu timer_min_ms=%.6f timer_max_ms=%.6f timer_mean_ms=%.6f",
                        (unsigned long long)g.timer_pairs,g.timer_min*ms,g.timer_max*ms,g.timer_pairs?g.timer_total*ms/g.timer_pairs:0.0);
                    std::fprintf(f,"\n");std::fclose(f);
                }
                rt::geo_profile_3ds::active=false;
            }
            u64 benchmark_gpu_ticks=0;
            BenchmarkGpuSample benchmark_gpu_sample{};
            if (diagnostic) diagnostic_stage("after game");
            sound_worker.start(*game, game_profile_sample ? &machine_timing : nullptr);
            u64 audio_wait_ticks = 0, audio_push_ticks = 0;
            auto finish_audio = [&] {
                const u64 wait_begin = (timing_sample || benchmark_mode) ? svcGetSystemTick() : 0;
                sound_worker.wait();
                const u64 push_begin = (timing_sample || benchmark_mode) ? svcGetSystemTick() : 0;
                if (game->sound()) audio.push(*game->sound());
                if (timing_sample || benchmark_mode) {
                    audio_wait_ticks = push_begin - wait_begin;
                    audio_push_ticks = svcGetSystemTick() - push_begin;
                }
                if (!diagnostic && timing_sample && game->frames() >= 150) {
                // Record simulation even if the display limiter skips this
                // frame; otherwise fixed-phase sampling can miss all samples.
                    FILE *f = std::fopen(path("profile.log").c_str(), "a");
                    if (f) {
                        const double milliseconds = 1000.0 / SYSCLOCK_ARM11;
                        const auto &video_stages = video.stage_times();
                        std::fprintf(f, "machine_sample frame=%llu game_ms=%.3f audio_ms=%.3f machine_ms=%.3f sound_ms=%.3f tgp_ms=%.3f tgp_runs=%llu geometry_ms=%.3f geo_early_culled=%llu video_ms=%.3f video_prepare_ms=%.3f video_rear_ms=%.3f video_poly_ms=%.3f video_front_ms=%.3f scale=%.2f distance=%u low_textures=%d wide=%d slider=%.3f display_limit=%u sampled_layers=%d\n",
                            (unsigned long long)game->frames(), (game_end-game_begin)*milliseconds,
                            machine_timing.sound_ms, machine_timing.machine_ms, machine_timing.sound_ms,
                            machine_timing.tgp_ms, (unsigned long long)machine_timing.tgp_runs, machine_timing.geometry_ms,
                            (unsigned long long)game->board().geometry().render_distance_culled(), machine_timing.video_ms,
                            video_stages.prepare_ms, video_stages.rear_layers_ms, video_stages.polygon_stage_ms, video_stages.front_layers_ms,
                            render_scale(), distance_level, low_textures, wide, osGet3DSliderState(), display_limit,
                            video.sampled_layer_output_active());
                        std::fclose(f);
                    }
                }
            };
            // The isolated input fixture advances boot without drawing; its
            // screenshots use the unchanged normal presentation path.
            if (validation_coins && game->frames() < 1350) { finish_audio(); continue; }
            if (late_check && game->frames() < 7200) {
                finish_audio();
                if (diagnostic) diagnostic_stage("after audio");
                if (game->frames() % 300 == 0) status(0, audio.enabled);
                if (native_checkpoint) {
                    FILE *f = std::fopen(path("diagnostic.log").c_str(), "a");
                    if (f) {
                        std::fprintf(f, "frame=%llu hash=%016llx instructions=%llu\n",
                            (unsigned long long)game->frames(), (unsigned long long)video.screen_hash(),
                            (unsigned long long)game->instructions());
                        std::fclose(f);
                    }
                }
                continue;
            }
            const u64 presentation_setup_begin = timing_sample ? svcGetSystemTick() : 0;
            if (cadence.step()) {
            if (!convergence || game->frames() % 60 == 0 ||
                (down & (KEY_DUP | KEY_DRIGHT | KEY_DDOWN | KEY_DLEFT))) {
                const float reference = game->board().geometry().render_reference_depth();
                if (reference > 0) convergence = reference;
            }
            const float focus = convergence > 0 ? convergence : 1;
            const float slider = diagnostic ? 1.f : std::clamp(osGet3DSliderState(), 0.f, 1.f);
            const float draw_distance = (diagnostic && !diagnostic_distance) || distance_level == 0 ? 0 : focus * (distance_level == 1 ? 4.f : distance_level == 2 ? 2.f : distance_level == 3 ? 1.f : distance_level == 4 ? 0.5f : 0.25f);
            video.set_draw_distance(draw_distance);
            game->board().geometry().set_render_distance(draw_distance);
            const float disparity = stereo_depth == 0 ? 4.f : stereo_depth == 1 ? 8.f : 12.f;
            const float eye = hud_stereo ? app::three_ds_world_eye_offset(video.presentation_scene().polys, slider, focus, disparity) :
                video.stereo_eye_offset(slider, focus, disparity);
            if (stereo_depth_profile && game->frames() % 60 == 30) {
                const auto scene = video.presentation_scene();
                std::vector<float> depths;
                const rt::GeoPoly *nearest = nullptr;
                float minimum = INFINITY;
                unsigned invalid = 0;
                for (const auto &poly : scene.polys) for (unsigned i = 0; i < poly.num_vertices; ++i) {
                    const float z = poly.v[i].p[0];
                    if (std::isfinite(z) && z > 0) {
                        depths.push_back(z);
                        if (z < minimum) { minimum = z; nearest = &poly; }
                    } else ++invalid;
                }
                std::sort(depths.begin(), depths.end());
                if (FILE *log = std::fopen(path("stereo-depth.log").c_str(), "a")) {
                    auto percentile = [&](unsigned percent) {
                        return depths.empty() ? 0.f : depths[(depths.size()-1)*percent/100];
                    };
                    std::fprintf(log, "frame=%llu focus=%.9g eye=%.9g slider=%.3f polygons=%u valid=%u invalid=%u min=%.9g p1=%.9g p5=%.9g p50=%.9g p95=%.9g max=%.9g\n",
                        (unsigned long long)game->frames(), focus, eye, slider, unsigned(scene.polys.size()),
                        unsigned(depths.size()), invalid, percentile(0), percentile(1), percentile(5), percentile(50), percentile(95), percentile(100));
                    if (nearest) {
                        std::fprintf(log, "nearest vertices=%u window=%u center=%d,%d viewport=%d,%d,%d,%d header=%04x,%04x,%04x,%04x\n",
                            unsigned(nearest->num_vertices), unsigned(nearest->window), nearest->center[0], nearest->center[1],
                            nearest->viewport[0], nearest->viewport[1], nearest->viewport[2], nearest->viewport[3],
                            nearest->texheader[0], nearest->texheader[1], nearest->texheader[2], nearest->texheader[3]);
                        for (unsigned i=0; i<nearest->num_vertices; ++i) {
                            const auto &v=nearest->v[i];
                            std::fprintf(log, "vertex=%u x=%.9g y=%.9g z=%.9g projected_x=%.9g projected_y=%.9g\n",
                                i,v.x,v.y,v.p[0],double(v.x)/v.p[0],double(v.y)/v.p[0]);
                        }
                    }
                    std::fclose(log);
                }
            }
            if (worker_check && (game->frames() == 240 || game->frames() == 1680 ||
                                 game->frames() == 3060 || game->frames() == 6000)) {
                video.set_render_threads(1);
                const auto serial = video.present(400, 240, 1, false, -eye, focus);
                video.set_render_threads(3);
                const auto &parallel = video.present(400, 240, 1, false, -eye, focus);
                const bool equal = serial == parallel;
                FILE *log = std::fopen(path("worker-check.log").c_str(), "a");
                if (log) {
                    std::fprintf(log, "frame=%llu workers=%u identical=%d\n",
                        (unsigned long long)game->frames(), video.render_threads(), equal);
                    std::fclose(log);
                }
                if (!equal) throw std::runtime_error("Parallel renderer pixel mismatch");
            }
            const u64 presentation_setup_ticks = timing_sample ? svcGetSystemTick() - presentation_setup_begin : 0;
            const int output_width = wide ? 400 : 310;
            video.set_presentation_profile(timing_sample);
            u64 left_present_ticks=0, left_copy_ticks=0, right_copy_ticks=0, render_end=0;
            rt::Video::PresentationTimes left_present{},right_present{};
#if defined(DAYTONA_3DS_PICA200)
            if(gpu) {
                if(validate_tile_layers && timing_sample) {
                    if(!video.validate_tile_spans()) throw std::runtime_error("GPU tile command comparison failed");
                    FILE *check=std::fopen(path("tile-spans.log").c_str(),"a");
                    if(check){std::fprintf(check,"frame=%llu identical=1 back=%lu front=%lu pixels=%lu\n",
                        (unsigned long long)game->frames(),(unsigned long)video.tile_spans(false).size(),
                        (unsigned long)video.tile_spans(true).size(),(unsigned long)(2*video.sampled_layer_dimensions().first*video.sampled_layer_dimensions().second));std::fclose(check);}
                }
                const u64 benchmark_gpu_begin=benchmark_mode?svcGetSystemTick():0;
                gpu->set_texture_quality(texture_quality);
                gpu->render(video,output_width,eye,focus,low_textures,render_scale());
                if(benchmark_mode) {
                    benchmark_gpu_ticks=svcGetSystemTick()-benchmark_gpu_begin;
                    const auto &g=gpu->stats();
                    benchmark_gpu_sample={g.wait_ms,g.upload_ms,g.geometry_ms,g.composite_ms,g.uploads,g.polygons,g.vertices,g.batches,g.revision_resets,g.textures,g.texture_bytes};
                }
                if(capture_frame && game->frames()==capture_frame)
                    gpu->capture(path("capture-left.ppm").c_str(),path("capture-right.ppm").c_str());
                if(timing_sample) {
                    const auto &g=gpu->stats();
                    FILE *f=std::fopen(path("gpu-profile.log").c_str(),"a");
                    if(f){std::fprintf(f,"frame=%llu gpu_backend=pica200 polygons=%u vertices=%u textures=%u uploads=%u batches=%u texture_bytes=%lu cpu_ms=%.3f gpu_ms=%.3f command_usage=%.3f scale=%.3f eye=%.6g tile_uploads=%u tile_commands=%u tile_output=%u\n",
                        (unsigned long long)game->frames(),g.polygons,g.vertices,g.textures,g.uploads,g.batches,
                        (unsigned long)g.texture_bytes,g.cpu_ms,g.gpu_ms,g.command_usage,render_scale(),eye,g.tile_uploads,g.tile_commands,unsigned(video.tile_span_output()));std::fclose(f);}
                    left_present.polygons=g.polygons/2;right_present.polygons=g.polygons/2;
                }
                finish_audio();render_end=timing_sample?svcGetSystemTick():0;
            } else
#endif
            {
            const u64 left_present_begin = timing_sample ? svcGetSystemTick() : 0;
            const auto stereo_frames = video.present_stereo(output_width, 240, 1, false, eye, focus);
            const auto &left = stereo_frames.first;
            const auto &right = stereo_frames.second;
            left_present_ticks = timing_sample ? svcGetSystemTick() - left_present_begin : 0;
            left_present = video.presentation_times();
            right_present = video.right_eye_presentation_times();
            if (diagnostic) diagnostic_stage("left rendered");
            const u64 left_copy_begin = timing_sample ? svcGetSystemTick() : 0;
            write_frame(left, GFX_LEFT);
            if (capture_frame && game->frames() == capture_frame) diagnostic_dump("capture-left.ppm", left);
            left_copy_ticks = timing_sample ? svcGetSystemTick() - left_copy_begin : 0;
            if (validation_coins && (game->frames() == 1380 || game->frames() == 1500)) {
                diagnostic_dump(game->frames() == 1380 ? "validation-before-start.ppm" : "validation-after-start.ppm", left);
                FILE *f = std::fopen(path("validation.log").c_str(), "a");
                if (f) {
                    std::fprintf(f, "frame=%llu queued_coins=%d instructions=%llu\n",
                        (unsigned long long)game->frames(), gate.pending, (unsigned long long)game->instructions());
                    std::fclose(f);
                }
            }
            if (diagnostic && !diagnostic_no_dump && (game->frames() == 1 || game->frames() % 60 == 0)) diagnostic_dump("diagnostic-left.ppm", left);
            video.set_presentation_profile(false);
            if (diagnostic) diagnostic_stage("right rendered");
            const u64 right_copy_begin = timing_sample ? svcGetSystemTick() : 0;
            write_frame(right, GFX_RIGHT);
            if (capture_frame && game->frames() == capture_frame) diagnostic_dump("capture-right.ppm", right);
            right_copy_ticks = timing_sample ? svcGetSystemTick() - right_copy_begin : 0;
            finish_audio();
            if (diagnostic) diagnostic_stage("after audio");
            render_end = timing_sample ? svcGetSystemTick() : 0;
            if (diagnostic && !diagnostic_no_dump && (game->frames() == 1 || game->frames() % 60 == 0)) {
                diagnostic_dump("diagnostic-right.ppm", right);
                FILE *log = std::fopen(path("diagnostic.log").c_str(), "a");
                if (log) {
                    std::fprintf(log, "frame=%llu %s=%016llx instructions=%llu audio=%d convergence=%.6g\n",
                        (unsigned long long)game->frames(), profile_only && !native_checkpoint ? "unused_native_hash" : "hash",
                        (unsigned long long)video.screen_hash(), (unsigned long long)game->instructions(), audio.enabled, convergence);
                    std::fclose(log);
                }
            }
            }
            if(!native_gpu) {
                gspWaitForVBlank();gfxFlushBuffers();gfxSwapBuffers();
            } else {
                // Citro3D owns the top-screen swap. The console uses only bottom.
                GSPGPU_FlushDataCache(gfxGetFramebuffer(GFX_BOTTOM,GFX_LEFT,nullptr,nullptr),320*240*3);
                gfxScreenSwapBuffers(GFX_BOTTOM,false);
            }
            ++displayed_total;
            if (diagnostic) diagnostic_stage("buffers swapped");
            const u64 now = svcGetSystemTick(); ++count;
            // Sample between image dumps and after verbose boot diagnostics.
            if (timing_sample && game->frames() >= 150 && game->frames() % 60 == 30) {
                FILE *f = std::fopen(path("profile.log").c_str(), "a");
                if (f) {
                    const double milliseconds = 1000.0 / SYSCLOCK_ARM11;
                    std::fprintf(f, "audio_output frame=%llu wait_ms=%.3f push_ms=%.3f\n",
                        (unsigned long long)game->frames(), audio_wait_ticks * milliseconds, audio_push_ticks * milliseconds);
                    std::fprintf(f, "frame=%llu game_ms=%.3f audio_ms=%.3f eyes_ms=%.3f display_ms=%.3f fast=%d machine_ms=%.3f sound_ms=%.3f audio_queued=%llu audio_completed=%llu audio_peak=%.6g workers=%u tgp_ms=%.3f tgp_runs=%llu setup_ms=%.3f left_present_cpu_ms=%.3f left_total_ms=%.3f left_prepare_ms=%.3f left_raster_ms=%.3f left_compose_ms=%.3f left_polygons=%llu left_cache=%d left_framebuffer_ms=%.3f right_present_cpu_ms=%.3f right_total_ms=%.3f right_prepare_ms=%.3f right_raster_ms=%.3f right_compose_ms=%.3f right_polygons=%llu right_cache=%d right_framebuffer_ms=%.3f\n",
                        (unsigned long long)game->frames(), (game_end-game_begin)*milliseconds,
                        machine_timing.sound_ms, (render_end-game_end)*milliseconds,
                        (now-render_end)*milliseconds, fast_render, machine_timing.machine_ms, machine_timing.sound_ms,
                        (unsigned long long)audio.queued, (unsigned long long)audio.completed, audio.peak, video.render_threads(),
                        machine_timing.tgp_ms, (unsigned long long)machine_timing.tgp_runs, presentation_setup_ticks*milliseconds,
                        left_present_ticks*milliseconds,
                        left_present.total_ms, left_present.prepare_ms, left_present.raster_ms, left_present.compose_ms,
                        (unsigned long long)left_present.polygons, left_present.cache_hit, (left_copy_ticks)*milliseconds,
                        0.0,
                        right_present.total_ms, right_present.prepare_ms, right_present.raster_ms, right_present.compose_ms,
                        (unsigned long long)right_present.polygons, right_present.cache_hit, (right_copy_ticks)*milliseconds);
                    std::fclose(f);
                }
            }
            }
            else {
                finish_audio();
                if (diagnostic) diagnostic_stage("after audio");
            }
            const u64 now = svcGetSystemTick();
            if (now - last_status >= SYSCLOCK_ARM11) {
                fps = float(double(count) * SYSCLOCK_ARM11 / (now - last_status));
                simulation_speed = float(100.0 * double(game->frames() - last_simulated) * SYSCLOCK_ARM11 /
                    (double(now - last_status) * rt::GameLoop::kFrameHz));
                last_simulated = game->frames();
                count = 0; last_status = now; status(fps, audio.enabled);
                if (diagnostic) diagnostic_stage("status complete");
            }
            deadline += u64(SYSCLOCK_ARM11 / rt::GameLoop::kFrameHz);
            if (diagnostic && game->frames() < 100) {
                FILE *f = std::fopen(path("timing.log").c_str(), "a");
                if (f) { std::fprintf(f, "%llu now=%llu deadline=%llu delta=%lld\n", (unsigned long long)game->frames(), (unsigned long long)now, (unsigned long long)deadline, (long long)(deadline-now)); std::fclose(f); }
            }
            if (deadline > now) svcSleepThread(s64((deadline - now) * 1000000000ull / SYSCLOCK_ARM11));
            else if (now - deadline > SYSCLOCK_ARM11 / 10) deadline = now;
            if (benchmark_mode && benchmark_step(game_end-game_begin,benchmark_gpu_ticks,audio_wait_ticks,audio_push_ticks,benchmark_gpu_sample,machine_timing,game_profile_sample)) break;
            if (diagnostic) diagnostic_stage("frame complete");
            if (validation_coins && game->frames() >= 6000) break;
        }
        if (!diagnostic) checkpoint();
        if (validation_coins) {
            FILE *f = std::fopen(path("validation.log").c_str(), "a");
            if (f) { std::fprintf(f, "completed frames=%llu queued_coins=%d\n", (unsigned long long)game->frames(), gate.pending); std::fclose(f); }
        }
        geometry_worker.reset();
        game.reset();
    } catch (const std::exception &e) {
        FILE *failure = std::fopen(path("startup.log").c_str(), "a");
        if (failure) { std::fprintf(failure, "error: %s\n", e.what()); if (failed_new_bytes) std::fprintf(failure, "allocation_failure bytes=%lu caller=%p used=%lu free=%lu top_free=%lu frame=%llu\n", (unsigned long)failed_new_bytes, failed_new_caller, (unsigned long)failed_new_heap.uordblks, (unsigned long)failed_new_heap.fordblks, (unsigned long)failed_new_heap.keepcost, (unsigned long long)(game ? game->frames() : 0)); std::fclose(failure); }
        consoleClear(); std::printf("Unable to start Daytona\n\n%s\n\nPress START to exit.\n", e.what());
        while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & KEY_START) break; gspWaitForVBlank(); }
        game.reset();
    }
    aptUnhook(&hook); gfxExit(); return 0;
}
