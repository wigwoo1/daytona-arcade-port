// Lockstep interrupt replay (design doc, M1 plan; UART option (a)): applies
// MAME's interrupt events at the same completed-instruction counts, and
// checks every interrupt the core takes against MAME's log. Shared by the
// interpreter harness and recompiled code, so both use one definition.
#pragma once

#include "runtime/cpu.h"
#include "runtime/m2_replay_bus.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rt {

class Lockstep {
public:
    // Loads MAME's IRQ log (M2TRACE_IRQLOG) and hooks the core's take callback.
    Lockstep(Cpu &core, const std::string &irq_log_path);
    // Free run (the game on its own, no MAME): no log; the board schedules
    // its events with add_callback, and interrupts the board raises are taken
    // at the next instruction boundary (poke()).
    explicit Lockstep(Cpu &core);

    uint64_t count = 0;            // completed instructions so far
    uint64_t end_count = UINT64_MAX; // MAME's run ends here
    uint64_t next_count = UINT64_MAX; // count of the next event to apply
    uint32_t fast_count = 0; // completed instructions since begin_fast_frame
    uint64_t current_count() const { return fast_active_ ? fast_base_count_ + fast_count : count; }
    void begin_fast_frame() {
        fast_base_count_ = count;
        fast_count = 0;
        fast_active_ = true;
        refresh_fast_boundary();
    }
    void end_fast_frame() {
        if (!fast_active_) return;
        count = fast_base_count_ + fast_count;
        fast_count = 0;
        fast_active_ = false;
    }
    bool fast_boundary() {
        if (!fast_active_) return boundary();
        if (fast_count < fast_boundary_count_) return false;
        count = fast_base_count_ + fast_count;
        const bool done = apply();
        fast_base_count_ = count;
        fast_count = 0;
        refresh_fast_boundary();
        return done;
    }
    // Call before each instruction. Returns true when the run is over or an
    // interrupt was taken (IP changed); the caller re-dispatches on m_IP.
    bool boundary() {
        if (count < boundary_count_) return false;
        return apply();
    }
    bool finished() const { return current_count() >= end_count; }
    void set_end_count(uint64_t at) {
        end_count = at;
        boundary_count_ = std::min(next_count, end_count);
        refresh_fast_boundary();
    }
    // Run fn when `at` instructions have completed, before any interrupt
    // event at the same count (MAME's vblank handler parses the display list
    // before it raises the vblank line). Lockstep: call before the run
    // starts. Free run: callbacks may add further callbacks.
    void add_callback(uint64_t at, std::function<void()> fn);
    // Free run: an interrupt line changed; take it at the next boundary.
    void poke() {
        next_count = std::min(next_count, current_count() + 1);
        boundary_count_ = std::min(next_count, end_count);
        refresh_fast_boundary();
        poked_ = true;
    }
    int interrupts() const { return taken_; }
    size_t callback_slots() const { return calls_.size(); }

private:
    struct Event {
        enum Kind { Call, Line, Imm, Pend, End } kind;
        uint64_t count;
        int a = 0, b = 0;
        uint32_t ip = 0;
        size_t fn = 0; // Call: index into calls_
    };
    std::vector<std::function<void()>> calls_;
    std::vector<size_t> free_calls_;
    bool apply();
    void refresh_next();
    void refresh_fast_boundary() {
        if (!fast_active_) return;
        const uint64_t target = boundary_count_ > fast_base_count_ ? boundary_count_ - fast_base_count_ : 0;
        fast_boundary_count_ = uint32_t(std::min<uint64_t>(target, UINT32_MAX));
    }
    static void on_take(void *ctx, int vector, uint32_t ip, bool pending);

    Cpu &core_;
    std::vector<Event> log_;
    size_t next_ = 0;
    bool free_run_ = false, poked_ = false;
    int taken_ = 0;
    uint64_t boundary_count_ = UINT64_MAX;
    uint64_t fast_base_count_ = 0;
    uint32_t fast_boundary_count_ = UINT32_MAX;
    bool fast_active_ = false;
};

} // namespace rt
