// The Model 2 board as the recompiled game runs on it outside lockstep: the
// whole i960 memory map with every device answered natively (no trace, no
// MAME, no emulated CPUs). Frame pacing is the only clock: the game's native
// code runs until it waits for vblank (its idle loop), then the board starts
// vblank: the geometrizer parses the display list, the vblank interrupt is
// raised, and at vblank end the screen is composed.
//
// Devices, each native: RAM/ROM and texture RAM (as the replay bus), the TGP
// and geometry ports (TgpBoard, recompiled TGP), the geometrizer (Geo), the
// screen (Video: tilemaps, palette, 3D layer), the interrupt controller and
// timers, video/render mode registers, the I/O board (IoBoard: its dual-port
// RAM mailbox protocol, inputs, settings EEPROM), the sound UART (bytes to
// the sound runtime), the comm board's shared RAM and backup RAM.
#pragma once

#include "runtime/cpu.h"
#include "runtime/geo.h"
#include "runtime/lockstep.h"
#include "runtime/m2_tgp_board.h"
#include "runtime/video.h"

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rt {

struct Inputs {
    uint8_t steer = 0x80, accel = 0x20, brake = 0x20; // ADC values, 0x20-0xe0
    uint8_t in0 = 0xff, in1 = 0x8f, in2 = 0xff;       // switches, active low (MAME IN0/IN1/IN2 layout)
};

// The I/O board's side of the MB8421 dual-port RAM (2 KB): the game writes a
// command to byte 0x20 and the board answers. 1: latch the inputs into bytes
// 0-10. 3: copy the 128-byte settings EEPROM to bytes 0x100-0x17f. 2: store
// bytes 0x100-0x17f to the EEPROM. The byte returns to 0 when done.
class IoBoard {
public:
    uint8_t read(uint32_t index) const { return ram_[index & 0x7ff]; }
    void write(uint32_t index, uint8_t v);
    Inputs inputs;
    std::array<uint8_t, 128> eeprom;
    bool eeprom_dirty = false;
    IoBoard() { ram_.fill(0); eeprom.fill(0xff); }

private:
    std::array<uint8_t, 0x800> ram_;
};

#if defined(__3DS__)
struct GeometryExecutor {
    virtual ~GeometryExecutor() = default;
    virtual bool start(Geo &geo,const uint32_t *commands,uint32_t read_start)=0;
    virtual double finish()=0; // Joins and returns worker execution milliseconds.
};
#endif
class M2Board : public Bus {
public:
    // Optional sampled presentation phases; no clocks on unprofiled frames.
    void begin_frame_profile(bool enabled) { frame_profile_ = enabled; geometry_ms_ = video_ms_ = 0; }
    double geometry_profile_ms() const { return geometry_ms_; }
    double video_profile_ms() const { return video_ms_; }
    struct Images {
        std::vector<uint8_t> program, main_data, copro_tables, copro_data, polygons, textures;
        std::vector<uint8_t> sound_program, pcm1, pcm2; // sound board (68000 program, MultiPCM samples)
        // Optional read-only images backed either by stable memory or a bounded
        // random-access reader. The caller keeps the view context alive.
        struct RomView {
            using Read = bool (*)(void *context, size_t offset, void *destination, size_t length);
            const uint8_t *data = nullptr;
            size_t size = 0;
            Read read = nullptr;
            void *context = nullptr;
            bool available() const { return data || read; }
        };
        RomView program_rom, main_data_rom, copro_tables_rom, copro_data_rom, polygons_rom, textures_rom;
        RomView sound_program_rom, pcm1_rom, pcm2_rom;
    };
    explicit M2Board(Images images, bool compact_memory = false, bool deferred_main = false);
    ~M2Board() override;
    static bool sparse_framebuffer_self_test();
    // Install verified compact main ROM before attaching/running the CPU.
    void install_main_data(std::vector<uint8_t> bytes);

    // The CPU and scheduler (free-run Lockstep) the board raises interrupts on.
    void attach(Cpu &cpu, Lockstep &ls);

    // Bus
    uint32_t fetch(uint32_t addr) override;
    uint8_t read_byte(uint32_t addr) override;
    uint16_t read_word(uint32_t addr) override;
    uint32_t read_dword(uint32_t addr) override;
    void write_byte(uint32_t addr, uint8_t data) override;
    void write_word(uint32_t addr, uint16_t data) override;
    void write_dword(uint32_t addr, uint32_t data) override;
    uint16_t flags(uint32_t addr) override { return page(addr).burst ? Cpu::BURST : 0; }

    // Frame control, called by the runner.
    void vblank_start();       // MAME screen_vblank: geometrizer, vblank IRQ
    void vblank_end();         // screen_update
    bool in_idle_loop() const; // the game is waiting for vblank
    uint64_t frame() const { return frame_; }
    size_t memory_map_bytes() const {
        size_t bytes = pages_.size() * sizeof(pages_[0]);
        for (const auto &leaf : pages_) if (leaf) bytes += sizeof(PageLeaf);
        return bytes;
    }

    Video &video() { return *video_; }
    size_t texture_memory_bytes() const { return tex0_.size() + tex1_.size(); }
    void set_video_write_tracking(bool enabled) {
        video_write_tracking_ = enabled;
        video_->set_ram_write_tracking(enabled);
    }
#if defined(__3DS__)
    GeometryExecutor *geometry_executor=nullptr; // Owned by the app; detached before board destruction.
    bool verify_geometry_worker=false;
    uint64_t geometry_jobs=0, geometry_feedback_waits=0, geometry_forced_checks=0;
    void finish_geometry();
#endif
    Geo &geometry() {
#if defined(__3DS__)
        finish_geometry();
#endif
        return *geo_;
    } // diagnostics, not game memory
    void set_traffic_car_limit(unsigned limit);
    unsigned model_lod = 0; // presentation-only car body detail
    unsigned track_extension = 0; // opt-in Daytona93 presentation experiment
    struct GeometryWrite { uint32_t instruction_pointer, address, data, mask, return_pointer; };
    bool record_geometry_writes = false;
    std::vector<GeometryWrite> geometry_writes;
    IoBoard &io() { return io_; }
    TgpBoard &tgp() { return tgp_; }
    // Bytes sent to the sound board since the last take.
    std::vector<uint8_t> take_sound_bytes() { sound_total_ += uart_out_.size(); return std::exchange(uart_out_, {}); }
    uint64_t sound_bytes_total() const { return sound_total_ + uart_out_.size(); }
    std::vector<uint8_t> &backup_ram() { return backup_; }

private:
#if defined(__3DS__)
    bool geometry_pending_=false,geometry_pending_profile_=false;
#endif
    bool frame_profile_ = false;
    double geometry_ms_ = 0, video_ms_ = 0;
    enum Kind : uint8_t { Unmapped, Rom, Ram, Tex, Dev, ZeroRom, SparseRam };
    struct Page {
        // ROM-view and direct backing pointers are mutually exclusive. Keep
        // them in one word: the page table has thousands of entries and is
        // permanently resident on the memory-constrained N64 target.
        union Mapping {
            uint8_t *base;
            const Images::RomView *rom_view;
            constexpr Mapping() : base(nullptr) {}
        } mapping;
        uint32_t rom_offset = 0;
        Kind kind = Unmapped;
        bool burst = false;
        uint8_t texture_bank = 0xff;
        bool chunked_rom = false;
    };
    static_assert(sizeof(Page) <= 16, "page record must stay compact for the N64 bus map");
    static constexpr unsigned kPageBits = 12;
    // A 1 MiB leaf keeps sparsely mapped 4 MiB regions from reserving pages
    // for large gaps while retaining a two-level lookup on every bus access.
    static constexpr unsigned kLeafBits = 8;
    using PageLeaf = std::array<Page, size_t(1) << kLeafBits>;
    const Page &page(uint32_t addr) const {
        const auto index = addr >> kPageBits;
        const auto &leaf = pages_[index >> kLeafBits];
        static const Page unmapped{};
        return leaf ? (*leaf)[index & ((1u << kLeafBits) - 1)] : unmapped;
    }
    void map(uint32_t start, uint32_t end, Kind k, uint8_t *base, uint32_t mirror = 0, bool burst = true,
             uint8_t texture_bank = 0xff);
    void map_rom_view(uint32_t start, uint32_t end, const Images::RomView &view, size_t offset = 0,
                      bool burst = true);
    bool read_rom(const Page &page, uint32_t page_offset, void *destination, size_t length) const;

    static bool sparse_ram_read(const Page &page, uint32_t offset, void *destination, size_t length);
    static void sparse_ram_write(Page &page, uint32_t offset, const void *source, size_t length);

    // Device dword access: data in its byte lanes, mask = lanes accessed.
    uint32_t dev_read(uint32_t addr, uint32_t mask);
    void dev_write(uint32_t addr, uint32_t data, uint32_t mask);
    void ram_written(uint32_t addr, uint32_t data, uint32_t mask); // video registers kept in RAM pages
    void track_video_ram_write(uint32_t addr, uint32_t size);
    void tex_write(const Page &p, uint32_t addr, uint32_t lane_data);
    uint64_t texture_revision_ = 0;

    void irq_update();
    void uart_write_data(uint8_t v);
    void uart_txrdy(bool state);

    Images img_;
    std::vector<uint8_t> ram_, work_, cpuctl_, backup_, tile_, chr_, palette_, xlat_, tex0_, tex1_, luma_, fb_a_, fb_b_,
        comm_;
    std::vector<std::unique_ptr<PageLeaf>> pages_;

    TgpBoard tgp_;
    std::unique_ptr<Geo> geo_;
    std::unique_ptr<Video> video_;
    bool video_write_tracking_ = false;
#if defined(DAYTONA_N64_LOW_MEMORY)
    uint8_t traffic_car_limit_ = 20;
#else
    uint8_t traffic_car_limit_ = 40;
#endif
    IoBoard io_;
    Cpu *cpu_ = nullptr;
    Lockstep *ls_ = nullptr;

    uint32_t intreq_ = 0, intena_ = 0;
    int8_t lines_[4] = {-1, -1, -1, -1};
    uint32_t timervals_[4] = {0, 0, 0, 0};
    uint32_t videocontrol_ = 0, zclip_ = 0;
    bool render_mode_ = false, render_test_ = false, render_unk_ = false;
    uint8_t comm_cn_ = 0, comm_fg_ = 0;
    uint64_t frame_ = 0;

    // i8251 UART to the sound board (transmit side)
    bool uart_txrdy_ = true, uart_shift_busy_ = false, uart_have_hold_ = false;
    uint8_t uart_hold_ = 0;
    std::vector<uint8_t> uart_out_;
    uint64_t sound_total_ = 0;
    void uart_shift_done();
};

} // namespace rt
