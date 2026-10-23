// license:BSD-3-Clause
// copyright-holders:R. Belmont, Olivier Galibert, ElSemi, Angelo Salese
//
// The Model 2 board as native runtime (see m2_board.h). Register semantics
// follow MAME's src/mame/sega/model2.cpp at
// dddd73680656e355bb2b5beecab1167c9f07bf81 (BSD-3-Clause; notice above kept):
// irq_request_r/irq_ack_w/irq_enable_w/irq_update, timers_w, videoctl_r/w,
// render_mode_r/w, tgpid_r, the model2o memory map. The I/O board's mailbox
// protocol was read from the game's own traffic (see IoBoard). See
// THIRD_PARTY.md.

#include "runtime/m2_board.h"
#include "runtime/3ds_geo_feedback.h"
#include "runtime/3ds_geo_overlap.h"
#include "runtime/byte_order.h"
#include <chrono>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace rt {

// ---------------------------------------------------------------------------
// I/O board

void IoBoard::write(uint32_t index, uint8_t v) {
    index &= 0x7ff;
    ram_[index] = v;
    if (index != 0x20) return;
    switch (v) {
    case 1: // latch inputs
        ram_[0] = inputs.steer;
        ram_[1] = inputs.accel;
        ram_[2] = inputs.brake;
        for (int i = 3; i < 8; i++) ram_[size_t(i)] = 0xff; // unused ADC channels
        ram_[8] = inputs.in0;
        ram_[9] = inputs.in1;
        ram_[10] = inputs.in2;
        ram_[0x21] = 0x40; // board status the game checks at boot
        ram_[0x20] = 0;
        break;
    case 2: // store settings
        std::memcpy(eeprom.data(), &ram_[0x100], eeprom.size());
        eeprom_dirty = true;
        ram_[0x20] = 0;
        break;
    case 3: // load settings
        std::memcpy(&ram_[0x100], eeprom.data(), eeprom.size());
        ram_[0x20] = 0;
        break;
    default: break;
    }
}

// ---------------------------------------------------------------------------
// Board

M2Board::M2Board(Images images, bool compact_memory, bool deferred_main)
    : img_(std::move(images)), ram_(0x20000), work_(0x100000), cpuctl_(0x1000), backup_(0x4000, 0xff), tile_(0x10000),
#if defined(DAYTONA_N64_LOW_MEMORY)
#if defined(DAYTONA_N64_TEXTURE_10)
      chr_(0x80000), palette_(0x4000), xlat_(0xc000), tex0_(0x33000), tex1_(0x33000), luma_(0x20000), fb_a_(),
#elif defined(DAYTONA_N64_TEXTURE_25)
      chr_(0x80000), palette_(0x4000), xlat_(0xc000), tex0_(0x80000), tex1_(0x80000), luma_(0x20000), fb_a_(),
#else
      chr_(0x80000), palette_(0x4000), xlat_(0xc000), tex0_(0x100000), tex1_(0x100000), luma_(0x20000), fb_a_(),
#endif
#else
      chr_(0x80000), palette_(0x4000), xlat_(0xc000), tex0_(0x200000), tex1_(0x200000), luma_(0x20000), fb_a_(0x80000),
#endif
#if defined(DAYTONA_N64_LOW_MEMORY)
      fb_b_(),
#else
      fb_b_(0x80000),
#endif
      comm_(0x4000), pages_(size_t(1) << (32 - kPageBits - kLeafBits)),
      tgp_(img_.copro_tables, img_.copro_data, compact_memory,
           reinterpret_cast<const uint32_t *>(img_.copro_tables_rom.data), img_.copro_tables_rom.size / 4,
           reinterpret_cast<const uint32_t *>(img_.copro_data_rom.data), img_.copro_data_rom.size / 4,
           img_.copro_tables_rom.read, img_.copro_tables_rom.context, img_.copro_tables_rom.size / 4,
           img_.copro_data_rom.read, img_.copro_data_rom.context, img_.copro_data_rom.size / 4) {
    const auto data = [](const std::vector<uint8_t> &owned, Images::RomView mapped) -> uint8_t * {
        return const_cast<uint8_t *>(mapped.data ? mapped.data : owned.data());
    };
    const auto size = [](const std::vector<uint8_t> &owned, Images::RomView mapped) -> size_t {
        return mapped.available() ? mapped.size : owned.size();
    };
    uint8_t *const program = data(img_.program, img_.program_rom);
    uint8_t *const main_data = data(img_.main_data, img_.main_data_rom);
    const size_t main_data_size = size(img_.main_data, img_.main_data_rom);
    if (compact_memory) {
        // TgpBoard owns independent typed copies for vector-backed images;
        // ROM views may also point into these owned vectors: retain their backing.
        if (!img_.copro_tables_rom.data) std::vector<uint8_t>().swap(img_.copro_tables);
        if (!img_.copro_data_rom.data) std::vector<uint8_t>().swap(img_.copro_data);
    }
    // model2o memory map (MAME model2_base_mem, model2_tgp_mem, model2o_mem)
    if (img_.program_rom.read) map_rom_view(0x00000000, 0x001fffff, img_.program_rom);
    else map(0x00000000, 0x001fffff, Rom, program);
    map(0x00200000, 0x0021ffff, Ram, ram_.data());
    if (img_.program_rom.read) map_rom_view(0x00220000, 0x0023ffff, img_.program_rom, 0x20000);
    else map(0x00220000, 0x0023ffff, Rom, program + 0x20000);
    map(0x00500000, 0x005fffff, Ram, work_.data());
    map(0x00800000, 0x00807fff, Dev, nullptr, 0, false);
    map(0x00880000, 0x00887fff, Dev, nullptr, 0, false);
    map(0x00900000, 0x0091ffff, Dev, nullptr, 0x60000, true); // buffer RAM (TgpBoard)
    map(0x00980000, 0x00980fff, Dev, nullptr, 0, false);
    map(0x00e00000, 0x00e00fff, Ram, cpuctl_.data(), 0, false);
    map(0x00e80000, 0x00e80fff, Dev, nullptr, 0, false);
    map(0x00f00000, 0x00f00fff, Dev, nullptr, 0, false);
    map(0x01000000, 0x0100ffff, Ram, tile_.data(), 0x110000);
    map(0x01040000, 0x01040fff, Dev, nullptr, 0x100000, false);
    map(0x01060000, 0x01060fff, Dev, nullptr, 0x100000, false);
    map(0x01080000, 0x010fffff, Ram, chr_.data(), 0x100000);
    map(0x01800000, 0x01803fff, Ram, palette_.data());
    map(0x01810000, 0x0181bfff, Ram, xlat_.data());
    map(0x0181c000, 0x0181cfff, Dev, nullptr, 0, false);
    map(0x01a00000, 0x01a03fff, Ram, comm_.data(), 0x10000);
    map(0x01a04000, 0x01a04fff, Dev, nullptr, 0x10000, false);
    map(0x01c00000, 0x01c00fff, Dev, nullptr, 0, false);
    map(0x01c80000, 0x01c80fff, Dev, nullptr, 0, false);
    map(0x01d00000, 0x01d03fff, Ram, backup_.data());
    if (deferred_main) {
        if (!compact_memory || main_data_size) throw std::runtime_error("Invalid deferred main ROM");
        map(0x02000000, 0x03ffffff, ZeroRom, nullptr);
        map(0x06000000, 0x06ffffff, ZeroRom, nullptr);
    } else if (compact_memory && main_data_size == 0x1000000) {
        if (img_.main_data_rom.read) map_rom_view(0x02000000, 0x02ffffff, img_.main_data_rom);
        else map(0x02000000, 0x02ffffff, Rom, main_data);
        map(0x03000000, 0x03ffffff, ZeroRom, nullptr);
        map(0x06000000, 0x06ffffff, ZeroRom, nullptr);
    } else if (img_.main_data_rom.read) {
        if (main_data_size >= 0x2000000) {
            map_rom_view(0x02000000, 0x03ffffff, img_.main_data_rom);
        } else {
            map_rom_view(0x02000000, 0x02ffffff, img_.main_data_rom);
            map(0x03000000, 0x03ffffff, ZeroRom, nullptr);
        }
        if (main_data_size >= 0x2000000)
            map_rom_view(0x06000000, 0x06ffffff, img_.main_data_rom, 0x1000000);
        else
            map(0x06000000, 0x06ffffff, ZeroRom, nullptr);
    } else {
        map(0x02000000, 0x03ffffff, Rom, main_data);
        map(0x06000000, 0x06ffffff, Rom, main_data + 0x1000000);
    }
    map(0x10000000, 0x105fffff, Dev, nullptr, 0, false);
#if defined(DAYTONA_N64_LOW_MEMORY)
    // These guest buffers are bus RAM, not the host renderer's output. Keep
    // untouched or uniformly filled pages compact without discarding writes.
    map(0x11600000, 0x116fffff, SparseRam, nullptr);
#else
    map(0x11600000, 0x1167ffff, Ram, fb_a_.data());
    map(0x11680000, 0x116fffff, Ram, fb_b_.data());
#endif
#if defined(DAYTONA_N64_LOW_MEMORY)
#if defined(DAYTONA_N64_TEXTURE_10)
    // Keep page-aligned backing close to 10% of each 2 MiB guest texture sheet.
    // tex_write bounds stores to 0x33000 bytes; the rest reads as zero.
    map(0x12000000, 0x12032fff, Tex, tex0_.data(), 0x200000, true, 0);
    map(0x12033000, 0x121fffff, Tex, nullptr, 0x200000, true, 0);
    map(0x12400000, 0x12432fff, Tex, tex1_.data(), 0x200000, true, 1);
    map(0x12433000, 0x125fffff, Tex, nullptr, 0x200000, true, 1);
#elif defined(DAYTONA_N64_TEXTURE_25)
    // Keep the lower quarter of each 2 MiB guest sheet in 512 KiB. Texture
    // writes still pass through tex_write, which packs the Model 2 lane layout.
    map(0x12000000, 0x1207ffff, Tex, tex0_.data(), 0x200000, true, 0);
    map(0x12080000, 0x121fffff, Tex, nullptr, 0x200000, true, 0);
    map(0x12400000, 0x1247ffff, Tex, tex1_.data(), 0x200000, true, 1);
    map(0x12480000, 0x125fffff, Tex, nullptr, 0x200000, true, 1);
#else
    // The guest-visible range remains 2 MiB, but the write handler stores at
    // most 1 MiB per sheet. Its upper backing half therefore remains zero.
    // Map the written guest half to its backing range and preserve zero reads
    // for the unwritten half while tex_write still handles its bus writes.
    map(0x12000000, 0x120fffff, Tex, tex0_.data(), 0, true, 0);
    map(0x12100000, 0x121fffff, Tex, nullptr, 0, true, 0);
    map(0x12200000, 0x122fffff, Tex, tex0_.data(), 0, true, 0);
    map(0x12300000, 0x123fffff, Tex, nullptr, 0, true, 0);
    map(0x12400000, 0x124fffff, Tex, tex1_.data(), 0, true, 1);
    map(0x12500000, 0x125fffff, Tex, nullptr, 0, true, 1);
    map(0x12600000, 0x126fffff, Tex, tex1_.data(), 0, true, 1);
    map(0x12700000, 0x127fffff, Tex, nullptr, 0, true, 1);
#endif
#else
    map(0x12000000, 0x121fffff, Tex, tex0_.data(), 0x200000);
    map(0x12400000, 0x125fffff, Tex, tex1_.data(), 0x200000);
#endif
    map(0x12800000, 0x1281ffff, Ram, luma_.data());

    if (img_.polygons_rom.read || img_.textures_rom.read) {
        if (!img_.polygons_rom.read || !img_.textures_rom.read || img_.polygons_rom.size % 4 || img_.textures_rom.size % 2)
            throw std::runtime_error("Invalid chunked geometry ROM images");
        geo_ = std::make_unique<Geo>(img_.polygons_rom.read, img_.polygons_rom.context,
                                    img_.polygons_rom.size / 4,
                                    img_.textures_rom.read, img_.textures_rom.context,
                                    img_.textures_rom.size / 2, tgp_.buffer_data());
    } else if (img_.polygons_rom.data || img_.textures_rom.data) {
        if (!img_.polygons_rom.data || !img_.textures_rom.data || img_.polygons_rom.size % 4 || img_.textures_rom.size % 2)
            throw std::runtime_error("Invalid mapped geometry ROM images");
        geo_ = std::make_unique<Geo>(reinterpret_cast<const uint32_t *>(img_.polygons_rom.data),
                                     img_.polygons_rom.size / 4,
                                     reinterpret_cast<const uint16_t *>(img_.textures_rom.data),
                                     img_.textures_rom.size / 2, tgp_.buffer_data());
    } else if (compact_memory) geo_ = std::make_unique<Geo>(img_.polygons, img_.textures, tgp_.buffer_data(), true);
    else geo_ = std::make_unique<Geo>(img_.polygons, img_.textures, tgp_.buffer_data());
    video_ = std::make_unique<Video>(tile_.data(), chr_.data());
}

M2Board::~M2Board() {
    for (auto &leaf : pages_) if (leaf)
        for (auto &p : *leaf) if (p.kind == SparseRam) delete[] p.mapping.base;
}

bool M2Board::sparse_ram_read(const Page &p, uint32_t offset, void *destination, size_t length) {
    constexpr size_t page_size = size_t(1) << kPageBits;
    if (offset > page_size || length > page_size - offset) return false;
    if (p.mapping.base) std::memcpy(destination, p.mapping.base + offset, length);
    else {
        const auto *pattern = reinterpret_cast<const uint8_t *>(&p.rom_offset);
        auto *out = static_cast<uint8_t *>(destination);
        for (size_t i = 0; i < length; ++i) out[i] = pattern[(offset + i) & 3];
    }
    return true;
}

void M2Board::sparse_ram_write(Page &p, uint32_t offset, const void *source, size_t length) {
    constexpr size_t page_size = size_t(1) << kPageBits;
    if (offset > page_size || length > page_size - offset) throw Fatal("sparse RAM write crosses page");
    if (!p.mapping.base) {
        const auto *pattern = reinterpret_cast<const uint8_t *>(&p.rom_offset);
        const auto *bytes = static_cast<const uint8_t *>(source);
        bool same = true;
        for (size_t i = 0; i < length; ++i) if (bytes[i] != pattern[(offset + i) & 3]) { same = false; break; }
        if (same) return;
        auto backing = std::make_unique<uint8_t[]>(page_size);
        for (size_t i = 0; i < page_size; ++i) backing[i] = pattern[i & 3];
        p.mapping.base = backing.release();
    }
    std::memcpy(p.mapping.base + offset, source, length);
    // Arcade RAM-test/clear sweeps finish at the end of each page. Collapse
    // uniform pages then, avoiding a 4 KiB scan for every ordinary bus write.
    if (offset + length == page_size) {
        uint32_t pattern;
        std::memcpy(&pattern, p.mapping.base, sizeof(pattern));
        for (size_t i = 4; i < page_size; i += 4) {
            uint32_t value;
            std::memcpy(&value, p.mapping.base + i, sizeof(value));
            if (value != pattern) return;
        }
        delete[] p.mapping.base;
        p.mapping.base = nullptr;
        p.rom_offset = pattern;
    }
}

bool M2Board::sparse_framebuffer_self_test() {
    Page p;
    p.kind = SparseRam;
    std::array<uint8_t, size_t(1) << kPageBits> expected{}, actual{};
    const auto matches = [&] {
        return sparse_ram_read(p, 0, actual.data(), actual.size()) && actual == expected;
    };
    const auto write = [&](uint32_t offset, const void *value, size_t length) {
        sparse_ram_write(p, offset, value, length);
        std::memcpy(expected.data() + offset, value, length);
    };
    bool passed = false;
    try {
        passed = matches() && !p.mapping.base;
        const uint32_t word = 0x12345678;
        const uint16_t half = 0xabcd;
        const uint8_t byte = 0xe7;
        write(4, &word, sizeof(word));
        write(6, &half, sizeof(half));
        write(5, &byte, sizeof(byte));
        write(4095, &byte, sizeof(byte));
        passed = passed && matches() && p.mapping.base;
        // Repeated whole-page fills must release backing and retain the exact
        // byte pattern on both little-endian hosts and big-endian N64 hardware.
        for (uint32_t pattern : {0x07800f0fu, 0xffffffffu, 0u}) {
            for (uint32_t offset = 0; offset < expected.size(); offset += sizeof(pattern))
                write(offset, &pattern, sizeof(pattern));
            passed = passed && matches() && !p.mapping.base;
        }
        for (uint32_t offset = 0; offset < expected.size(); offset += sizeof(word)) {
            const uint32_t value = offset ^ 0x52414d00u;
            write(offset, &value, sizeof(value));
        }
        passed = passed && matches() && p.mapping.base;
        uint16_t read_half = 0;
        uint32_t read_word = 0;
        passed = passed && sparse_ram_read(p, 4094, &read_half, sizeof(read_half)) &&
                 sparse_ram_read(p, 4092, &read_word, sizeof(read_word));
        passed = passed && std::memcmp(&read_half, expected.data() + 4094, sizeof(read_half)) == 0 &&
                 std::memcmp(&read_word, expected.data() + 4092, sizeof(read_word)) == 0 &&
                 !sparse_ram_read(p, 4095, &read_word, sizeof(read_word));
    } catch (...) { delete[] p.mapping.base; throw; }
    delete[] p.mapping.base;
    return passed;
}

void M2Board::install_main_data(std::vector<uint8_t> bytes) {
    if (!img_.main_data.empty() || bytes.size() != 0x1000000 || cpu_)
        throw std::runtime_error("Invalid deferred main ROM installation");
    img_.main_data = std::move(bytes);
    map(0x02000000, 0x02ffffff, Rom, img_.main_data.data());
}

void M2Board::map(uint32_t start, uint32_t end, Kind k, uint8_t *base, uint32_t mirror, bool burst,
                  uint8_t texture_bank) {
    for (uint32_t m = 0;; m = (m - mirror) & mirror) {
        for (uint64_t a = start; a <= end; a += (1u << kPageBits)) {
            const auto index = uint32_t(a | m) >> kPageBits;
            auto &leaf = pages_[index >> kLeafBits];
            if (!leaf) leaf = std::make_unique<PageLeaf>();
            Page &p = (*leaf)[index & ((1u << kLeafBits) - 1)];
            if (p.kind == SparseRam) delete[] p.mapping.base;
            p.kind = k;
            p.burst = burst;
            p.texture_bank = texture_bank;
            p.mapping.base = base ? base + (a - start) : nullptr;
            p.rom_offset = 0;
            p.chunked_rom = false;
        }
        if (((m - mirror) & mirror) == 0) break;
    }
}

void M2Board::map_rom_view(uint32_t start, uint32_t end, const Images::RomView &view,
                           size_t offset, bool burst) {
    const size_t length = end >= start ? size_t(end - start) + 1 : 0;
    if (!view.read || !length || offset > view.size || length > view.size - offset ||
        offset > UINT32_MAX || length - 1 > size_t(UINT32_MAX) - offset)
        throw std::runtime_error("Invalid chunked ROM mapping");
    for (uint64_t address = start; address <= end; address += (1u << kPageBits)) {
        const auto index = uint32_t(address) >> kPageBits;
        auto &leaf = pages_[index >> kLeafBits];
        if (!leaf) leaf = std::make_unique<PageLeaf>();
        Page &page = (*leaf)[index & ((1u << kLeafBits) - 1)];
        page.kind = Rom;
        page.burst = burst;
        page.texture_bank = 0xff;
        page.mapping.rom_view = &view;
        page.rom_offset = uint32_t(offset + size_t(address - start));
        page.chunked_rom = true;
    }
}

bool M2Board::read_rom(const Page &page, uint32_t page_offset, void *destination, size_t length) const {
    if (page.kind == SparseRam) return sparse_ram_read(page, page_offset, destination, length);
    if (page.chunked_rom && page.mapping.rom_view && page.mapping.rom_view->read)
        return page.mapping.rom_view->read(page.mapping.rom_view->context, page.rom_offset + page_offset, destination, length);
    if (!page.mapping.base || page_offset + length > (size_t(1) << kPageBits)) return false;
    std::memcpy(destination, page.mapping.base + page_offset, length);
    return true;
}

void M2Board::attach(Cpu &cpu, Lockstep &ls) {
    cpu_ = &cpu;
    ls_ = &ls;
}

void M2Board::set_traffic_car_limit(unsigned limit) {
    traffic_car_limit_ = limit == 0 ? 0 : limit <= 5 ? 5 : limit <= 10 ? 10 : limit <= 20 ? 20 : 40;
    if (traffic_car_limit_ < 40 && work_.size() > 0x3200)
        work_[0x3200] = std::min(work_[0x3200], traffic_car_limit_);
}

// --- interrupts (MAME irq_update) --------------------------------------------

void M2Board::irq_update() {
    const int8_t want[4] = {int8_t((intreq_ & 0x001) ? 1 : 0), int8_t((intreq_ & 0x002) ? 1 : 0),
                            int8_t((intreq_ & 0x3fc) ? 1 : 0), int8_t((intreq_ & 0xc00) ? 1 : 0)};
    for (int l = 0; l < 4; l++)
        if (want[l] != lines_[l]) {
            lines_[l] = want[l];
            cpu_->execute_set_input(l, want[l]);
            ls_->poke();
        }
}

#if defined(__3DS__)
void M2Board::finish_geometry() {
    if(!geometry_pending_) return;
    geometry_pending_=false; // A rethrown worker error must not cause a second wait.
    const double elapsed=geometry_executor->finish();
    if(geometry_pending_profile_) geometry_ms_+=elapsed;
    geo_feedback_3ds::mark_parsed();
}
#endif
void M2Board::vblank_start() {
#if defined(__3DS__)
    finish_geometry();
#endif
    const auto profile_begin = frame_profile_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // 60 Hz mode or an even frame: the geometrizer starts a new frame
    if ((videocontrol_ & 1) == 0 || (frame_ & 1) == 0) {
        // N64 presents at native 4:3 and has no desktop widescreen output.
        // Avoid producing a second clipped polygon list and per-frame copies.
#if defined(DAYTONA_N64_LOW_MEMORY)
        geo_->capture_widescreen = false;
#else
        geo_->capture_widescreen = video_->widescreen();
#endif
        geo_->track_reference_models.clear(); geo_->extra_track_objects.clear();
        if (track_extension) {
#if !defined(DAYTONA_N64_LOW_MEMORY)
            // Daytona93's 16x16 scenery pointer grid and visible-cell list,
            // located from native submission traces. Read RAM/ROM only.
            const uint32_t table = read_dword(0x00501420);
            const unsigned count = read_byte(0x005016c0);
            if (table >= 0x02000000 && table <= 0x03fffc00 && count > 0 && count <= 25) {
                std::array<bool, 256> visible{}, candidates{};
                for (unsigned i = 0; i < count; ++i) visible[read_byte(0x005016c1 + i)] = true;
                if (track_extension >= 2) candidates.fill(true);
                const int radius = 1;
                for (unsigned cell = 0; cell < 256; ++cell) if (visible[cell])
                    for (int dy = -radius; dy <= radius; ++dy) for (int dx = -radius; dx <= radius; ++dx) {
                        const int x = int(cell % 16) + dx, y = int(cell / 16) + dy;
                        if (x >= 0 && x < 16 && y >= 0 && y < 16) candidates[y * 16 + x] = true;
                    }
                std::set<std::array<uint32_t, 3>> submitted;
                for (unsigned pass = 0; pass < 2; ++pass)
                for (unsigned cell = 0; cell < 256; ++cell) if (pass == 0 ? visible[cell] : candidates[cell] && !visible[cell]) {
                    const uint32_t descriptor = read_dword(table + cell * 4);
                    if (descriptor < 0x02000000 || descriptor > 0x03fffff0) continue;
                    // Each cell may contain a terminated chain of descriptors,
                    // not just one model. Deduplicate shared world models.
                    for (unsigned entry = 0; entry < 256; ++entry) {
                        const uint32_t address = descriptor + entry * 16;
                        if (address > 0x03fffff0) break;
                        const uint32_t model = read_dword(address);
                        if (!(model & 0x00800000) || model >= 0x00c00000) break;
                        const uint32_t points = read_dword(address + 4), headers = read_dword(address + 8);
                        if (!submitted.insert({model, points, headers}).second) continue;
                        if (pass == 0) geo_->track_reference_models.push_back(model);
                        else geo_->extra_track_objects.push_back({model, 5000, points, headers, {}});
                    }
                }
            }
#endif
        }
#if !defined(DAYTONA_N64_LOW_MEMORY)
        geo_->model_overrides.clear();
        if (model_lod == 1 || model_lod == 2) {
            const unsigned count = read_dword(0x00503200);
            if (count <= 40) for (unsigned i = 0; i < count; ++i) {
                const uint32_t car = read_dword(0x00503208 + i * 8);
                if (car < 0x00500000 || car > 0x0053ff00) continue;
                const uint32_t target = read_dword(car + (model_lod == 1 ? 0x70 : 0x7c));
                if (target < 0x02000000 || target > 0x03fffff0) continue;
                const uint32_t model = read_dword(target), points = read_dword(target + 4), headers = read_dword(target + 8);
                if (model < 0x00800000 || model >= 0x00c00000) continue;
                for (unsigned level = 1; level <= 4; ++level) {
                    const uint32_t source = read_dword(car + 0x6c + level * 4);
                    if (source < 0x02000000 || source > 0x03fffff0) continue;
                    const uint32_t original = read_dword(source);
                    if (original < 0x00800000 || original >= 0x00c00000 || original == model) continue;
                    geo_->model_overrides.push_back({original, read_dword(source + 4), read_dword(source + 8), model, points, headers});
                }
            }
        }
#endif
#if defined(__3DS__)
        // Force actual board-register reads in oracle mode, including previous
        // frame feedback and repeat reads that must use the cached result.
        if (geo_->verify_lazy_native) {
            const uint32_t expected=uint32_t(geo_->polys.size());
            if (read_dword(0x10400000)!=expected) throw GeoFatal("Previous native count register mismatch");
        }
        geo_->lazy_native_allowed = track_extension==0 && model_lod==0;
#endif
        geo_->zclip_w(zclip_);
#if defined(__3DS__)
        const bool verify_lazy=geo_->verify_lazy_native;
        if(geometry_executor && track_extension==0 && model_lod==0) {
            geometry_pending_=geometry_executor->start(*geo_,tgp_.buffer_data(),tgp_.geo_read_start());
            geometry_pending_profile_=frame_profile_;
            if(geometry_pending_) ++geometry_jobs;
        }
        if(!geometry_pending_) {
            geo_overlap_3ds::geometry_begin();
#endif
            geo_->parse(tgp_.geo_read_start());
#if defined(__3DS__)
            geo_overlap_3ds::geometry_end();
            geo_feedback_3ds::mark_parsed();
        }
        if(verify_geometry_worker && geometry_pending_ && frame_%120==30) {
            const uint32_t value=read_dword(0x10400000);
            if(value!=geo_->native_polygon_count()) throw GeoFatal("Geometry worker feedback barrier mismatch");
            ++geometry_forced_checks;
        }
        if (verify_lazy) {
            finish_geometry();
            const uint32_t expected=uint32_t(geo_->polys.size());
            if (read_dword(0x10400000)!=expected) throw GeoFatal("Native count register mismatch");
            const uint64_t computations=geo_->native_count_computations;
            if (read_dword(0x10400000)!=expected || computations!=geo_->native_count_computations)
                throw GeoFatal("Native count cache mismatch");
        }
#endif
#if !defined(DAYTONA_N64_LOW_MEMORY)
#if defined(__3DS__)
        if(!geometry_pending_) {
#endif
        if (!geo_->model_overrides.empty())
            geo_->parse_model_presentation(tgp_.geo_read_start(), track_extension != 0, video_->widescreen());
        else if (track_extension) geo_->extend_track(video_->widescreen());
#if defined(__3DS__)
        }
#endif
#else
        // Custom track extension and selectable LODs are presentation-only
        // experiments. The native game already submits its chosen models.
        geo_->model_overrides.clear();
#endif
        video_->frame_start();
    }
    if (frame_profile_) geometry_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - profile_begin).count();
    if (intena_ & 1) {
        intreq_ |= 1;
        irq_update();
    }
}

void M2Board::vblank_end() {
#if defined(__3DS__)
    finish_geometry();
    geo_overlap_3ds::frame_end();
#endif
    const auto profile_begin = frame_profile_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    VideoMem m;
    m.palram = palette_.data();
    m.colorxlat = xlat_.data();
    m.lumaram = luma_.data();
    m.tex0 = reinterpret_cast<const uint32_t *>(tex0_.data());
    m.tex1 = reinterpret_cast<const uint32_t *>(tex1_.data());
    m.texture_revision = texture_revision_;
    video_->screen_update(geo_->polys, geo_->windows(), m, &geo_->wide_polys,
                         geo_->extended_polys.empty() ? nullptr : &geo_->extended_polys);
    if (frame_profile_) video_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - profile_begin).count();
    ++frame_;
}

bool M2Board::in_idle_loop() const {
    // The game's wait-for-vblank loops (0x12b0: until the frame counter at
    // 0x00500000 changes; 0x12f0: until it reaches 2). MAME takes 99% of
    // vblank interrupts here; the rest land in CPU-bound code such as the
    // boot-time texture upload at 0x1388.
    const uint32_t ip = cpu_->m_IP;
    return (ip >= 0x12b0 && ip <= 0x12bb) || (ip >= 0x12f0 && ip <= 0x12ff);
}

// --- sound UART (i8251, transmit side) ------------------------------------------
// The game sends bytes from its IRQ3 handler whenever TxRDY is set. With no
// clock, a byte moves to the shifter and out at once; the sound runtime
// receives it immediately.

void M2Board::uart_txrdy(bool state) {
    if (state == uart_txrdy_) return;
    uart_txrdy_ = state;
    // MAME sound_ready_w: TxRDY (or RxRDY) sets request bit 10 if enabled
    if (state && (intena_ & (1u << 10))) {
        intreq_ |= 1u << 10;
        irq_update();
    }
}

void M2Board::uart_write_data(uint8_t v) {
    uart_hold_ = v;
    uart_have_hold_ = true;
    uart_txrdy(false);
    if (!uart_shift_busy_) {
        uart_shift_busy_ = true;
        ls_->add_callback(ls_->current_count() + 1, [this] { uart_shift_done(); });
    }
}

void M2Board::uart_shift_done() {
    uart_shift_busy_ = false;
    if (uart_have_hold_) {
        uart_out_.push_back(uart_hold_);
        uart_have_hold_ = false;
    }
    uart_txrdy(true);
}

// --- device registers --------------------------------------------------------

uint32_t M2Board::dev_read(uint32_t addr, uint32_t mask) {
    if (addr >= 0x00900000 && addr <= 0x0097ffff) {
        tgp_.sync(); // the TGP answers through its mailbox here (0x0091fff0-8)
        return tgp_.buffer_r(addr & 0x1ffff);
    }
    if (addr >= 0x00800000 && addr <= 0x00803fff) return tgp_.geo_r((addr - 0x00800000) >> 2);
    if (addr >= 0x00804000 && addr <= 0x00807fff) return 0xffffffffu; // geo_prg_r
    if (addr >= 0x00884000 && addr <= 0x00887fff) return tgp_.fifo_r();
    switch (addr) {
    case 0x00980000: return tgp_.coproctl_r();
    case 0x00980004: {
        const uint32_t v = tgp_.fifo_out_empty() ? 1 : 0;
        if (std::getenv("M2RUN_VERBOSE")) std::fprintf(stderr, "fifo status read -> %u (frame %llu)\n", v, (unsigned long long)frame_);
        return v;
    }
    case 0x0098000c: { // videoctl_r
        const uint32_t framenum = render_mode_ ? uint32_t((frame_ & 1) << 2) : uint32_t((frame_ & 2) << 1);
        return framenum | (videocontrol_ & 3);
    }
    case 0x00e80000: return intreq_;
    case 0x00e80004: return intena_;
    default: break;
    }
    if (addr >= 0x00980030 && addr <= 0x0098003f) { // tgpid_r, byte-wide
        static const uint8_t id[] = {0, 'T', 'A', 'H', 0, 'A', 'K', 'O', 0, 'Z', 'A', 'K', 0, 'M', 'T', 'K'};
        const uint32_t o = addr - 0x00980030;
        return uint32_t(id[o]) | uint32_t(id[o + 1]) << 8 | uint32_t(id[o + 2]) << 16 | uint32_t(id[o + 3]) << 24;
    }
    if (addr >= 0x00f00000 && addr <= 0x00f0000f) return timervals_[(addr >> 2) & 3];
    if ((addr & ~0x10000u) == 0x01a04000) return uint32_t(comm_cn_ | 0xfe) | uint32_t(comm_fg_) << 16; // cn_r, fg_r
    if (addr >= 0x01c00000 && addr <= 0x01c00fff) { // MB8421 through umask 0x00ff00ff
        const uint32_t i = ((addr & 0xfff) >> 2) * 2;
        return uint32_t(io_.read(i)) | uint32_t(io_.read(i + 1)) << 16;
    }
    if (addr >= 0x10000000 && addr <= 0x101fffff) return uint32_t(render_unk_) << 14 | uint32_t(render_mode_) << 2 | uint32_t(render_test_);
    if (addr >= 0x10400000 && addr <= 0x105fffff) { // polygon_count_r
#if defined(__3DS__)
        geo_overlap_3ds::feedback();
        if(geometry_pending_) ++geometry_feedback_waits;
        finish_geometry();
        const uint32_t result=geo_->native_polygon_count();
        geo_feedback_3ds::record(cpu_ ? cpu_->m_IP : 0,result);
#else
        const uint32_t result=uint32_t(geo_->polys.size());
#endif
        return result;
    }
    (void)mask;
    return 0;
}

void M2Board::dev_write(uint32_t addr, uint32_t data, uint32_t mask) {
    if (record_geometry_writes && addr >= 0x00800000 && addr <= 0x0097ffff)
        geometry_writes.push_back({cpu_ ? cpu_->m_IP : 0, addr, data, mask, cpu_ ? cpu_->m_r[2] : 0});
    if (addr >= 0x00900000 && addr <= 0x0097ffff) { tgp_.buffer_w(addr & 0x1ffff, data, mask); return; }
    if (addr == 0x00980000) { tgp_.coproctl_w(data, mask); return; }
    const uint32_t d = data & mask; // handlers without a mask see unwritten lanes as 0
    if (addr >= 0x00884000 && addr <= 0x00887fff) { tgp_.fifo_w(d); return; }
    if (addr >= 0x00880000 && addr <= 0x00883fff) { tgp_.function_port_w((addr - 0x00880000) >> 2, d); return; }
    if (addr >= 0x00804000 && addr <= 0x00807fff) { tgp_.geo_prg_w(d); return; }
    if (addr >= 0x00800000 && addr <= 0x00803fff) { tgp_.geo_w((addr - 0x00800000) >> 2, d); return; }
    switch (addr) {
    case 0x00980008: tgp_.geoctl_w(d); return;
    case 0x0098000c: videocontrol_ = (videocontrol_ & ~mask) | (data & mask); return;
    case 0x00e80000: intreq_ &= data | ~mask; irq_update(); return; // irq_ack_w
    case 0x00e80004: {                                                // irq_enable_w (MAME delays 80 ns)
        intena_ = (intena_ & ~mask) | (data & mask);
        if (uart_txrdy_ && (intena_ & (1u << 10))) intreq_ |= 1u << 10; // irq_mask_delayed_update
        irq_update();
        return;
    }
    case 0x0181c000: zclip_ = d; return;
    default: break;
    }
    if (addr >= 0x00f00000 && addr <= 0x00f0000f) {
        // timers_w: count down at 25 MHz and raise request bit 2+n. Daytona
        // reloads timer 0 each frame and never enables its interrupt, so the
        // value is kept without a clock.
        uint32_t &t = timervals_[(addr >> 2) & 3];
        t = (t & ~mask) | (data & mask);
        return;
    }
    if ((addr & ~0x100000u) == 0x01040000) { if (mask & 0xffff) video_->xhout_w(uint16_t(data)); return; }
    if ((addr & ~0x100000u) == 0x01060000) { if (mask & 0xffff) video_->xvout_w(uint16_t(data)); return; }
    if ((addr & ~0x10000u) == 0x01a04000) {
        if (mask & 0xff) comm_cn_ = uint8_t(data & 1);
        if (mask & 0xff0000) comm_fg_ = uint8_t(data >> 16);
        return;
    }
    if (addr >= 0x01c00000 && addr <= 0x01c00fff) {
        const uint32_t i = ((addr & 0xfff) >> 2) * 2;
        if (mask & 0x000000ff) io_.write(i, uint8_t(data));
        if (mask & 0x00ff0000) io_.write(i + 1, uint8_t(data >> 16));
        return;
    }
    if (addr >= 0x01c80000 && addr <= 0x01c80003) { // i8251, umask16 0x00ff: lane 0 data, lane 2 control
        if (mask & 0x000000ff) uart_write_data(uint8_t(data));
        return; // mode/command bytes: transmitter enabled by the game at boot
    }
    if (addr >= 0x10000000 && addr <= 0x101fffff) { // render_mode_w
        render_test_ = data & 1;
        render_mode_ = (data >> 2) & 1;
        render_unk_ = (data >> 14) & 1;
        return;
    }
}

// Palette and colour translation live in RAM pages; the video output tracks
// what MAME's handlers do on each write.
void M2Board::ram_written(uint32_t addr, uint32_t data, uint32_t mask) {
    (void)data;
    if (addr >= 0x01800000 && addr <= 0x01803fff) {
        for (uint32_t lane = 0; lane < 2; lane++)
            if ((mask >> (16 * lane)) & 0xffff) video_->palette_w(((addr & 0x3fff) >> 1) + lane, palette_.data(), xlat_.data());
    } else if (addr >= 0x01810000 && addr <= 0x0181bfff) {
        for (uint32_t lane = 0; lane < 2; lane++)
            if ((mask >> (16 * lane)) & 0xffff) video_->colorxlat_w(((addr - 0x01810000) >> 1) + lane);
    }
}

void M2Board::tex_write(const Page &p, uint32_t addr, uint32_t lane_data) {
    ++texture_revision_;
#if defined(DAYTONA_N64_LOW_MEMORY)
    if (p.texture_bank > 1) throw Fatal("texture write missing bank mapping");
    uint8_t *const base = p.texture_bank ? tex1_.data() : tex0_.data();
    const uint32_t o = (addr & 0x1fffff) >> 2;
    const uint32_t byte_offset = (o >> 1) * 4 + (o & 1) * 2;
    static_assert((((0x1ffffc >> 2) >> 1) * 4 + ((0x1ffffc >> 2) & 1) * 2 + 1) == 0xfffff);
    if (size_t(byte_offset) + 2 > (p.texture_bank ? tex1_.size() : tex0_.size())) return;
    base[byte_offset] = uint8_t(lane_data);
    base[byte_offset + 1] = uint8_t(lane_data >> 8);
#else
    uint8_t *const base = p.mapping.base - ((addr & 0x1fffff) & ~((1u << kPageBits) - 1));
    const uint32_t o = (addr & 0x1fffff) >> 2;
    uint8_t *const w = base + (o >> 1) * 4 + (o & 1) * 2;
    w[0] = uint8_t(lane_data);
    w[1] = uint8_t(lane_data >> 8);
#endif
}

// --- bus ---------------------------------------------------------------------

uint32_t M2Board::fetch(uint32_t addr) {
    const Page &p = page(addr);
    if (p.kind == ZeroRom) return 0;
    if (p.kind != Rom && p.kind != Ram && p.kind != SparseRam) throw Fatal("instruction fetch from a device");
    uint32_t v;
    if (!read_rom(p, addr & 0xffc, &v, sizeof(v))) throw Fatal("failed to read mapped ROM");
    return little_endian_value(v);
}

uint8_t M2Board::read_byte(uint32_t addr) {
    const Page &p = page(addr);
    const unsigned sh = (addr & 3) * 8;
    switch (p.kind) {
    case Rom: case Ram: case SparseRam: case Tex: {
        uint8_t value = 0;
        if (!read_rom(p, addr & 0xfff, &value, sizeof(value))) return 0;
        return value;
    }
    case Dev: return uint8_t(dev_read(addr & ~3u, 0xffu << sh) >> sh);
    default: return 0;
    }
}

uint16_t M2Board::read_word(uint32_t addr) {
    addr &= ~1u;
    const Page &p = page(addr);
    const unsigned sh = (addr & 2) * 8;
    switch (p.kind) {
    case Rom: case Ram: case SparseRam: case Tex: {
        uint16_t v;
        if (!read_rom(p, addr & 0xfff, &v, sizeof(v))) return 0;
        return little_endian_value(v);
    }
    case Dev: return uint16_t(dev_read(addr & ~3u, 0xffffu << sh) >> sh);
    default: return 0;
    }
}

uint32_t M2Board::read_dword(uint32_t addr) {
    addr &= ~3u;
    const Page &p = page(addr);
    switch (p.kind) {
    case Rom: case Ram: case SparseRam: case Tex: {
        uint32_t v;
        if (!read_rom(p, addr & 0xfff, &v, sizeof(v))) return 0;
        return little_endian_value(v);
    }
    case Dev: return dev_read(addr, 0xffffffffu);
    default: return 0;
    }
}

void M2Board::write_byte(uint32_t addr, uint8_t data) {
    if (addr == 0x00503200 && traffic_car_limit_ < 40)
        data = std::min(data, traffic_car_limit_);
    const Page &p = page(addr);
    const unsigned sh = (addr & 3) * 8;
    switch (p.kind) {
    case Ram: p.mapping.base[addr & 0xfff] = data; track_video_ram_write(addr, 1); ram_written(addr & ~3u, uint32_t(data) << sh, 0xffu << sh); return;
    case SparseRam:
        sparse_ram_write(const_cast<Page &>(p), addr & 0xfff, &data, sizeof(data));
        track_video_ram_write(addr, sizeof(data));
        ram_written(addr & ~3u, uint32_t(data) << sh, (sizeof(data) == 1 ? 0xffu : 0xffffu) << sh);
        return;
    case Tex: tex_write(p, addr, uint32_t(data) << sh); return;
    case Dev: dev_write(addr & ~3u, uint32_t(data) << sh, 0xffu << sh); return;
    default: return;
    }
}

void M2Board::write_word(uint32_t addr, uint16_t data) {
    addr &= ~1u;
    const Page &p = page(addr);
    const uint16_t stored = little_endian_value(data);
    const unsigned sh = (addr & 2) * 8;
    switch (p.kind) {
    case Ram:
        std::memcpy(p.mapping.base + (addr & 0xfff), &stored, 2);
        track_video_ram_write(addr, 2);
        ram_written(addr & ~3u, uint32_t(data) << sh, 0xffffu << sh);
        return;
    case SparseRam:
        sparse_ram_write(const_cast<Page &>(p), addr & 0xfff, &stored, sizeof(stored));
        track_video_ram_write(addr, sizeof(data));
        ram_written(addr & ~3u, uint32_t(data) << sh, (sizeof(data) == 1 ? 0xffu : 0xffffu) << sh);
        return;
    case Tex: tex_write(p, addr, uint32_t(data) << sh); return;
    case Dev: dev_write(addr & ~3u, uint32_t(data) << sh, 0xffffu << sh); return;
    default: return;
    }
}

void M2Board::write_dword(uint32_t addr, uint32_t data) {
    addr &= ~3u;
    const Page &p = page(addr);
    const uint32_t stored = little_endian_value(data);
    switch (p.kind) {
    case Ram: std::memcpy(p.mapping.base + (addr & 0xfff), &stored, 4); track_video_ram_write(addr, 4); ram_written(addr, data, 0xffffffffu); return;
    case SparseRam:
        sparse_ram_write(const_cast<Page &>(p), addr & 0xfff, &stored, sizeof(stored));
        track_video_ram_write(addr, sizeof(data));
        ram_written(addr, data, 0xffffffffu);
        return;
    case Tex: tex_write(p, addr, data); return;
    case Dev: dev_write(addr, data, 0xffffffffu); return;
    default: return;
    }
}

void M2Board::track_video_ram_write(uint32_t addr, uint32_t size) {
    if (!video_write_tracking_) return;
    if (addr >= 0x01000000 && addr < 0x01008000)
        video_->tile_ram_written(addr - 0x01000000, size);
    else if (addr >= 0x01080000 && addr < 0x01100000)
        video_->char_ram_written(addr - 0x01080000, size);
}

} // namespace rt
