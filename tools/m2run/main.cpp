// m2run: the recompiled game running on its own on the native board: no
// trace, no MAME, no emulated CPU and no instruction clock. Headless for now:
// frames go to raw dumps (scripts/rgb2png.py converts them).
//
//   m2run IMAGES_DIR FRAMES [--inputs scripts/inputs/X.txt] [--dump DIR --every N] [--wav FILE]
//
// --wav writes the sound board's output (YM3438 + both MultiPCMs, mixed at
// 48 kHz, 16-bit stereo).
//
// Frame pacing is the game's own: vblank starts when the game has finished
// its frame and waits in its idle loop (or after a cap, for frames that never
// idle), and ends when the vblank handler has returned. A windowed build
// waits for the display's vsync at that point; the frame rate is the only
// limit.

#include "runtime/game_loop.h"
#include "app/coin_gate.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

// scripts/inputs format: "frames N", "<from>-<to> name=value" or "<at> name=value".
struct Script {
    struct Line {
        uint64_t from, to;
        std::string name;
        unsigned value;
    };
    std::vector<Line> lines;
    void load(const std::string &path) {
        std::ifstream f(path);
        if (!f) throw std::runtime_error("cannot open " + path);
        std::string s;
        while (std::getline(f, s)) {
            if (s.empty() || s[0] == '#' || s.compare(0, 6, "frames") == 0) continue;
            std::istringstream in(s);
            std::string range, kv;
            in >> range >> kv;
            const auto dash = range.find('-'), eq = kv.find('=');
            if (eq == std::string::npos) continue;
            Line l;
            l.from = std::stoull(range.substr(0, dash));
            l.to = dash == std::string::npos ? l.from : std::stoull(range.substr(dash + 1));
            l.name = kv.substr(0, eq);
            l.value = unsigned(std::stoul(kv.substr(eq + 1), nullptr, 0));
            lines.push_back(l);
        }
    }
    rt::Inputs at(uint64_t frame) const {
        rt::Inputs in;
        static const struct {
            const char *name;
            int port; // 0: IN0, 1: IN1
            uint8_t bit;
        } buttons[] = {{"coin", 0, 0x01}, {"test", 0, 0x04}, {"service", 0, 0x08}, {"start", 0, 0x10}, {"vr1", 0, 0x20},
                       {"vr2", 0, 0x40},  {"vr3", 0, 0x80},  {"vr4", 1, 0x01}};
        static const uint8_t gearvalue[5] = {0, 2, 1, 6, 5}; // MAME daytona_gearbox_r
        for (const Line &l : lines) {
            if (frame < l.from || frame > l.to) continue;
            if (l.name == "steer") in.steer = uint8_t(l.value);
            else if (l.name == "accel") in.accel = uint8_t(l.value);
            else if (l.name == "brake") in.brake = uint8_t(l.value);
            else if (l.name.compare(0, 4, "gear") == 0 && l.value) {
                const int g = l.name[4] - '0';
                if (g >= 0 && g < 5) in.in1 = uint8_t((in.in1 & ~0x70) | (gearvalue[g] << 4));
            } else
                for (const auto &b : buttons)
                    if (l.name == b.name && l.value) (b.port ? in.in1 : in.in0) &= uint8_t(~b.bit); // active low
        }
        return in;
    }
};

// Linear resampling of interleaved stereo to `rate`, added into out.
void mix_into(std::vector<float> &out, const std::vector<float> &in, double in_rate, double rate) {
    const size_t frames_in = in.size() / 2;
    if (frames_in < 2) return;
    const size_t frames_out = size_t(double(frames_in - 1) * rate / in_rate);
    if (out.size() < frames_out * 2) out.resize(frames_out * 2, 0.0f);
    for (size_t i = 0; i < frames_out; ++i) {
        const double pos = double(i) * in_rate / rate;
        const size_t k = size_t(pos);
        const float f = float(pos - double(k));
        for (int ch = 0; ch < 2; ++ch) out[i * 2 + ch] += in[k * 2 + ch] * (1 - f) + in[(k + 1) * 2 + ch] * f;
    }
}

void write_wav(const std::string &path, const std::vector<float> &mix, uint32_t rate) {
    std::ofstream f(path, std::ios::binary);
    auto u32 = [&](uint32_t v) { f.put(char(v)); f.put(char(v >> 8)); f.put(char(v >> 16)); f.put(char(v >> 24)); };
    auto u16 = [&](uint16_t v) { f.put(char(v)); f.put(char(v >> 8)); };
    const uint32_t bytes = uint32_t(mix.size() * 2);
    f.write("RIFF", 4); u32(36 + bytes); f.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    f.write("data", 4); u32(bytes);
    for (float v : mix) u16(uint16_t(int16_t(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f))));
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: m2run IMAGES_DIR FRAMES [--inputs FILE] [--dump DIR --every N] [--wav FILE] [--polygon-log FILE] [--traffic-car-limit 10|20|40]\n");
        return 2;
    }
    const std::string dir = argv[1];
    const uint64_t frames = std::strtoull(argv[2], nullptr, 10);
    std::string dump_dir, inputs_path, wav_path, nv_dir, hash_path, timing_path, polygon_path;
    uint64_t every = 0;
    bool manual_start = false, widescreen = false, compact_memory = false, tile_cache = false, deferred_main = false;
    bool video_write_tracking = false;
    bool low_textures = false, nearest_textures = false;
    int render_height = 0, bench_present = 0, render_threads = 0;
    float render_scale = 1;
    std::string render_dump;
    std::string geometry_path;
    bool depth_buffer = false;
    bool presentation_only = false;
    unsigned track_extension = 0, model_lod = 0;
    unsigned traffic_car_limit = 0;
    std::string geometry_write_path;
    std::string lod_model_path;
    for (int i = 3; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--geometry-log")) geometry_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--depth-buffer")) depth_buffer = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--presentation-only")) presentation_only = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--model-lod")) model_lod = unsigned(std::clamp(std::atoi(argv[i + 1]), 0, 2));
        else if (!std::strcmp(argv[i], "--traffic-car-limit")) traffic_car_limit = unsigned(std::max(0, std::atoi(argv[i + 1])));
        else if (!std::strcmp(argv[i], "--track-extension")) track_extension = unsigned(std::clamp(std::atoi(argv[i + 1]), 0, 2));
        else if (!std::strcmp(argv[i], "--geometry-write-log")) geometry_write_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--lod-model-log")) lod_model_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--hash-log")) hash_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--timing-log")) timing_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--polygon-log")) polygon_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--low-textures")) low_textures = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--nearest-textures")) nearest_textures = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--tile-cache")) tile_cache = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--video-write-tracking")) video_write_tracking = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--compact-memory")) compact_memory = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--deferred-main")) deferred_main = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--widescreen")) widescreen = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--manual-start")) manual_start = std::atoi(argv[i + 1]) != 0;
        else if (!std::strcmp(argv[i], "--nv-dir")) nv_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--inputs")) inputs_path = argv[i + 1];
        else if (!std::strcmp(argv[i], "--dump")) dump_dir = argv[i + 1];
        else if (!std::strcmp(argv[i], "--every")) every = std::strtoull(argv[i + 1], nullptr, 10);
        else if (!std::strcmp(argv[i], "--render-threads")) render_threads = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--bench-present")) bench_present = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--render-height")) render_height = std::atoi(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--render-scale")) render_scale = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--render-dump")) render_dump = argv[i + 1];
        else if (!std::strcmp(argv[i], "--wav")) wav_path = argv[i + 1];
    }

    try {
        if (traffic_car_limit && traffic_car_limit != 10 && traffic_car_limit != 20 && traffic_car_limit != 40)
            throw std::invalid_argument("--traffic-car-limit must be 10, 20, or 40");
        rt::GameLoop game(dir, compact_memory, deferred_main);
        if (traffic_car_limit) game.board().set_traffic_car_limit(traffic_car_limit);
        if (!nv_dir.empty()) {
            auto read_nv = [&](const char *name, auto &target) {
                std::ifstream file(nv_dir + "/" + name, std::ios::binary);
                if (!file) throw std::runtime_error(std::string("cannot read ") + name);
                const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(file), {}};
                if (bytes.size() != target.size()) throw std::runtime_error(std::string("wrong size: ") + name);
                std::copy(bytes.begin(), bytes.end(), target.begin());
            };
            read_nv("ioboard_eeprom.bin", game.board().io().eeprom);
            read_nv("backup_ram.bin", game.board().backup_ram());
        }
        game.board().video().set_render_threads(unsigned(std::clamp(render_threads, 0, 32)));
        game.board().video().enable_presentation(render_height > 0, presentation_only && render_height > 0);
        game.board().video().set_widescreen(widescreen);
        game.board().video().set_tile_cache(tile_cache);
        game.board().set_video_write_tracking(video_write_tracking);
        game.board().video().set_low_textures(low_textures);
        game.board().video().set_nearest_textures(nearest_textures);
        game.board().video().set_render_scale(render_scale);
        game.board().video().set_depth_buffer(depth_buffer);
        game.board().track_extension = track_extension;
        game.board().model_lod = model_lod;
        uint64_t matched_frames = 0, distinct_frames = 0;
        app::CoinGate coin_gate;
        Script script;
        if (!inputs_path.empty()) script.load(inputs_path);
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<float> fm, pcm;
        std::ofstream hashes;
        std::ofstream timings;
        std::ofstream polygon_counts;
        std::ofstream geometry;
        std::ofstream geometry_writes;
        std::ofstream lod_models;
        if (!lod_model_path.empty()) {
            lod_models.open(lod_model_path);
            if (!lod_models) throw std::runtime_error("cannot write model detail diagnostic");
            lod_models << "frame,car,selected_level,level,descriptor,model,texture_points,texture_headers\n";
        }
        if (!geometry_write_path.empty()) {
            geometry_writes.open(geometry_write_path);
            if (!geometry_writes) throw std::runtime_error("cannot write geometry bus log");
            geometry_writes << "frame,instruction_pointer,address,data,mask,return_pointer\n";
        }
        if (!geometry_path.empty()) {
            geometry.open(geometry_path);
            if (!geometry) throw std::runtime_error("cannot write geometry submission log");
            game.board().geometry().record_objects = true;
            geometry << "frame,address,count,texture_points,texture_headers";
            for (unsigned i = 0; i < 12; ++i) geometry << ",matrix" << i;
            geometry << '\n';
        }
        if (!timing_path.empty()) {
            timings.open(timing_path);
            if (!timings) throw std::runtime_error("cannot write host timing log");
        }
        if (!polygon_path.empty()) {
            polygon_counts.open(polygon_path);
            if (!polygon_counts) throw std::runtime_error("cannot write polygon count profile");
            polygon_counts << "frame,polygons,culled_by_distance\n";
        }
        if (!hash_path.empty()) {
            hashes.open(hash_path);
            if (!hashes) throw std::runtime_error("cannot write replay hash log");
        }
        for (uint64_t f = 0; f < frames; f++) {
            game.board().record_geometry_writes = geometry_writes.is_open() && game.frames() == 2999;
            game.board().geometry_writes.clear();
            rt::GameLoop::FrameTiming timing;
            game.run_frame(coin_gate.sample(script.at(game.board().frame()), manual_start), timings.is_open() ? &timing : nullptr);
            if (polygon_counts.is_open())
                polygon_counts << game.frames() << ',' << game.board().geometry().polys.size() << ','
                              << game.board().geometry().render_distance_culled() << '\n';
            if (polygon_counts.is_open()) polygon_counts.flush();
            if (lod_models.is_open() && game.frames() % (every ? every : 60) == 0) {
                auto &board = game.board();
                const unsigned count = board.read_byte(0x00503200);
                if (count <= 40) for (unsigned i = 0; i < count; ++i) {
                    const uint32_t car = board.read_dword(0x00503208 + i * 8);
                    if (car < 0x00500000 || car > 0x0053ff00) continue;
                    const unsigned selected = board.read_byte(car + 0xc4) & 7;
                    for (unsigned level = 1; level <= 4; ++level) {
                        const uint32_t descriptor = board.read_dword(car + 0x6c + level * 4);
                        if (descriptor < 0x02000000 || descriptor > 0x03fffff0) continue;
                        lod_models << game.frames() << ',' << car << ',' << selected << ',' << level << ','
                                   << descriptor << ',' << board.read_dword(descriptor) << ','
                                   << board.read_dword(descriptor + 4) << ',' << board.read_dword(descriptor + 8) << '\n';
                    }
                }
            }
            for (const auto &write : game.board().geometry_writes)
                geometry_writes << game.frames() << ',' << write.instruction_pointer << ',' << write.address
                    << ',' << write.data << ',' << write.mask << ',' << write.return_pointer << '\n';
            if (geometry.is_open() && game.frames() % (every ? every : 60) == 0) {
                for (const auto &object : game.board().geometry().object_submissions) {
                    geometry << game.frames() << ',' << object.address << ',' << object.count
                        << ',' << object.texture_points << ',' << object.texture_headers;
                    for (float value : object.matrix) geometry << ',' << value;
                    geometry << '\n';
                }
            }
            if (timings.is_open()) {
                timings << "frame=" << game.frames() << " machine_ms=" << timing.machine_ms
                    << " sound_ms=" << timing.sound_ms << " tgp_ms=" << timing.tgp_ms
                    << " tgp_runs=" << timing.tgp_runs << '\n';
            }
            if (hashes.is_open()) {
                char line[256];
                std::snprintf(line, sizeof line, "frame=%llu hash=%016llx instructions=%llu tgp=%llu sound=%llu\n",
                    (unsigned long long)game.frames(), (unsigned long long)game.board().video().screen_hash(),
                    (unsigned long long)game.instructions(), (unsigned long long)game.board().tgp().tgp_instructions(),
                    game.sound() ? (unsigned long long)game.sound()->instructions() : 0);
                hashes << line;
            }
            if (render_height > 0 && f % 30 == 0) {
                auto &video = game.board().video();
                const auto original_hash = video.screen_hash();
                const auto a = video.present(496, 384, 0.0f, true);
                const auto b = video.present(496, 384, 0.5f, true);
                if (video.interpolated_polygons()) ++matched_frames;
                if (a != b) ++distinct_frames;
                if (video.screen_hash() != original_hash) throw std::runtime_error("presentation modified native frame");
            }
            if (!wav_path.empty() && game.sound()) {
                const auto a = game.sound()->take_fm(), b = game.sound()->take_pcm();
                fm.insert(fm.end(), a.begin(), a.end());
                pcm.insert(pcm.end(), b.begin(), b.end());
            } else if (game.sound()) {
                game.sound()->take_fm(); game.sound()->take_pcm();
            }
            if (!dump_dir.empty() && every && game.board().frame() % every == 0) {
                char path[512];
                std::snprintf(path, sizeof path, "%s/run_%05" PRIu64 ".rgb", dump_dir.c_str(), game.board().frame());
                if (FILE *d = std::fopen(path, "wb")) {
                    std::fwrite(game.screen().data(), 4, game.screen().size(), d);
                    std::fclose(d);
                }
            }
        }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("memory map storage: %zu bytes (excluding allocator overhead)\n", game.board().memory_map_bytes());
        std::printf("m2run: %" PRIu64 " frames, %" PRIu64 " i960 instructions (all native), %" PRIu64
                    " TGP instructions, %d interrupts, %" PRIu64 " bytes to the sound board; %.2f s (%.0f frames/s)\n",
                    game.frames(), game.instructions(), game.board().tgp().tgp_instructions(), game.interrupts(),
                    game.board().sound_bytes_total(), s, double(game.frames()) / s);
        std::printf("  last screen hash %016" PRIx64 "\n", game.board().video().screen_hash());
        if (const snd::SoundBoard *sb = game.sound())
            std::printf("  sound board: %" PRIu64 " 68000 instructions (all native), %zu command bytes received\n", sb->instructions(),
                        sb->bytes_received());
        if (!wav_path.empty() && game.sound()) {
            std::vector<float> mix;
            mix_into(mix, fm, game.sound()->fm_rate(), 48000);
            mix_into(mix, pcm, game.sound()->pcm_rate(), 48000);
            write_wav(wav_path, mix, 48000);
            std::printf("  wrote %s (%.1f s)\n", wav_path.c_str(), double(mix.size() / 2) / 48000.0);
        }
        if (render_height > 0) {
            if (track_extension) std::printf("  track extension: %u neighbours, %zu extra models, %zu native polygons, %zu presentation polygons\n", track_extension,
                game.board().geometry().extra_track_objects.size(), game.board().geometry().polys.size(), game.board().geometry().extended_polys.size());
            const int width = int(std::lround(double(render_height) * (widescreen ? 16.0 / 9.0 : 496.0 / 384.0)));
            const auto render_start = std::chrono::steady_clock::now();
            const auto &pixels = game.board().video().present(width, render_height, 1.0f, false);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - render_start).count();
            std::printf("  presentation: %dx%d, %.2f ms; %llu sampled frames matched, %llu distinct intermediate frames\n", width, render_height, ms,
                (unsigned long long)matched_frames, (unsigned long long)distinct_frames);
            if (bench_present > 0) {
                std::vector<double> times;
                for (int n = 0; n < bench_present; ++n) {
                    game.board().video().invalidate_presentation();
                    const auto begin = std::chrono::steady_clock::now();
                    game.board().video().present(width, render_height, 1.0f, false);
                    times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count());
                }
                std::printf("  renderer workers: %u\n", game.board().video().render_threads());
                std::sort(times.begin(), times.end());
                std::printf("  render benchmark: %d samples, median %.3f ms, min %.3f ms, max %.3f ms\n", bench_present,
                    times[times.size()/2], times.front(), times.back());
            }
            if (!render_dump.empty()) {
                std::ofstream image(render_dump, std::ios::binary);
                image << "P6\n" << width << " " << render_height << "\n255\n";
                for (const auto px : pixels) { image.put(char(px >> 16)); image.put(char(px >> 8)); image.put(char(px)); }
            }
        }
        return 0;
    } catch (const std::exception &e) {
        std::printf("m2run: stopped: %s\n", e.what());
        return 1;
    }
}
