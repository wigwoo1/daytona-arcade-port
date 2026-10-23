#include "runtime/lockstep.h"
#include <cstdio>
#include <functional>
#include <memory>
#include <vector>

int main() {
    rt::Cpu core(nullptr);
    rt::Lockstep schedule(core);
    int failures = 0, iterations = 0, future = 0;
    auto check = [&](bool ok, const char *message) {
        if (!ok) { std::printf("FAIL: %s\n", message); ++failures; }
    };
    schedule.add_callback(400000, [&] { ++future; });
    std::function<void()> recurring;
    recurring = [&] {
        if (++iterations < 300000) schedule.add_callback(schedule.count + 1, recurring);
    };
    schedule.add_callback(1, recurring);
    for (schedule.count = 1; schedule.count <= 300000; ++schedule.count) schedule.boundary();
    check(iterations == 300000, "all self-rescheduled callbacks execute");
    check(future == 0, "future callback is not overwritten by slot reuse");
    check(schedule.callback_slots() <= 4, "callback storage stays bounded during a long run");
    auto payload = std::make_shared<int>(77);
    std::weak_ptr<int> weak = payload;
    schedule.add_callback(300001, [payload] {});
    payload.reset();
    check(!weak.expired(), "queued callback retains its payload");
    schedule.count = 300001; schedule.boundary();
    check(weak.expired(), "completed callback releases its payload");
    std::vector<int> order;
    schedule.add_callback(300002, [&] { order.push_back(1); });
    schedule.add_callback(300002, [&] {
        order.push_back(2);
        schedule.add_callback(300002, [&] { order.push_back(3); });
    });
    schedule.count = 300002; schedule.boundary();
    check(order == std::vector<int>({2, 3, 1}), "same-count insertion and reentrant ordering stay unchanged");
    schedule.count = 400000; schedule.boundary();
    check(future == 1, "long-lived callback executes exactly once");

    rt::Cpu boundary_core(nullptr);
    rt::Lockstep boundary_schedule(boundary_core);
    int due = 0;
    boundary_schedule.add_callback(11, [&] { ++due; });
    boundary_schedule.set_end_count(12);
    boundary_schedule.count = 10;
    check(!boundary_schedule.boundary(), "combined boundary waits before the next event");
    boundary_schedule.count = 11;
    boundary_schedule.boundary();
    check(due == 1, "event fires before the run-end boundary");
    boundary_schedule.count = 12;
    check(boundary_schedule.boundary(), "combined boundary stops exactly at end_count");

    rt::Cpu exact_core(nullptr);
    rt::Lockstep exact(exact_core);
    uint64_t first_at = UINT64_MAX, future_at = UINT64_MAX, nested_at = UINT64_MAX;
    exact.add_callback(5, [&] { future_at = exact.count; });
    exact.add_callback(2, [&] {
        first_at = exact.count;
        exact.add_callback(exact.count + 3, [&] { nested_at = exact.count; });
    });
    while (exact.count < 8) {
        if (!exact.boundary()) ++exact.count;
    }
    check(first_at == 2, "callback countdown preserves its exact instruction boundary");
    check(future_at == 5, "farther scheduled callbacks retain exact timing");
    check(nested_at == 5, "callback scheduled inside a boundary retains exact timing");

    rt::Cpu poke_core(nullptr);
    rt::Lockstep poke_schedule(poke_core);
    poke_schedule.poke(); // before the first instruction: take at count 1
    check(!poke_schedule.boundary(), "pre-run poke does not fire before instruction zero");
    ++poke_schedule.count;
    poke_schedule.boundary();
    check(poke_schedule.next_count == UINT64_MAX, "pre-run poke is applied at count one");

    rt::Cpu fast_core(nullptr);
    rt::Lockstep fast(fast_core);
    uint64_t fast_at = UINT64_MAX;
    fast.add_callback(5, [&] { fast_at = fast.count; });
    fast.begin_fast_frame();
    for (int i = 0; i < 8; ++i) {
        if (!fast.fast_boundary()) ++fast.fast_count;
    }
    fast.end_fast_frame();
    check(fast_at == 5, "fast counter applies callbacks at the same completed-instruction count");
    check(fast.count == 8 && fast.fast_count == 0, "fast counter synchronizes its total at frame end");

    rt::Cpu fast_dynamic_core(nullptr);
    rt::Lockstep fast_dynamic(fast_dynamic_core);
    uint64_t dynamic_at = UINT64_MAX, nested_fast_at = UINT64_MAX;
    fast_dynamic.begin_fast_frame();
    fast_dynamic.add_callback(2, [&] {
        dynamic_at = fast_dynamic.current_count();
        fast_dynamic.add_callback(fast_dynamic.current_count() + 3, [&] { nested_fast_at = fast_dynamic.current_count(); });
    });
    while (fast_dynamic.current_count() < 8) {
        if (!fast_dynamic.fast_boundary()) ++fast_dynamic.fast_count;
    }
    fast_dynamic.end_fast_frame();
    check(dynamic_at == 2, "fast callback observes the synchronized absolute count");
    check(nested_fast_at == 5, "fast callback can schedule another callback at an exact count");

    rt::Cpu fast_poke_core(nullptr);
    rt::Lockstep fast_poke(fast_poke_core);
    fast_poke.begin_fast_frame();
    fast_poke.poke();
    check(!fast_poke.fast_boundary(), "fast pre-run poke does not fire before the next instruction");
    ++fast_poke.fast_count;
    check(!fast_poke.fast_boundary(), "free-run poke changes state without an interrupt IP change");
    check(fast_poke.current_count() == 1, "fast poke is serviced at the same next-instruction boundary");
    fast_poke.end_fast_frame();

    rt::Cpu fast_end_core(nullptr);
    rt::Lockstep fast_end(fast_end_core);
    fast_end.set_end_count(3);
    fast_end.begin_fast_frame();
    for (int i = 0; i < 3; ++i) {
        if (!fast_end.fast_boundary()) ++fast_end.fast_count;
    }
    check(fast_end.fast_boundary(), "fast counter stops exactly at the frame end count");
    fast_end.end_fast_frame();
    check(fast_end.count == 3, "fast counter preserves the exact stop count");

    rt::Cpu in_instruction_core(nullptr);
    rt::Lockstep in_instruction(in_instruction_core);
    check(!in_instruction.boundary(), "first instruction starts before the IRQ poke");
    in_instruction.poke(); // while instruction zero is active: take at count 1
    ++in_instruction.count;
    in_instruction.boundary();
    check(in_instruction.next_count == UINT64_MAX, "in-instruction poke is applied at the next boundary");
    return failures ? 1 : 0;
}
