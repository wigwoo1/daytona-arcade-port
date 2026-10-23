#include "runtime/render_workers.h"
#if defined(__3DS__)
extern "C" {
#include <3ds/services/apt.h>
}
namespace rt {
RenderWorkers::RenderWorkers(unsigned count) {
    LightSemaphore_Init(&done_, 0, 2);
    LightLock_Init(&error_lock_);
    if (count <= 1) return;
    s32 priority = 0x30;
    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);

    workers_[0].pool = this;
    LightEvent_Init(&workers_[0].work, RESET_ONESHOT);
    workers_[0].thread = threadCreate(entry, &workers_[0], 64 * 1024, priority, 2, false);
    if (workers_[0].thread) ++worker_count_;

    // Processor 3 is reserved and normal applications cannot create threads
    // there. Processor 1 can host one app worker after requesting a CPU share.
    // Keep it optional so the core-0 + core-2 path remains the safe fallback.
    auto create_core1_worker = [&](WorkerSlot &slot) {
        if (!changed_cpu_limit_) {
            // libctru's sound worker already raises the app share to 80% to
            // enable the system core. Preserve that full allowance here; a
            // 30% cap starves the main game thread during raster work.
            if (R_FAILED(APT_GetAppCpuTimeLimit(&previous_cpu_limit_)) ||
                R_FAILED(APT_SetAppCpuTimeLimit(80))) return false;
            changed_cpu_limit_ = true;
        }
        slot.pool = this;
        LightEvent_Init(&slot.work, RESET_ONESHOT);
        slot.thread = threadCreate(entry, &slot, 64 * 1024, priority, 1, false);
        if (!slot.thread) {
            APT_SetAppCpuTimeLimit(previous_cpu_limit_);
            changed_cpu_limit_ = false;
            return false;
        }
        ++worker_count_;
        return true;
    };

    const bool core2_available = workers_[0].thread != nullptr;
    if (!core2_available) {
        create_core1_worker(workers_[0]);
    }
    if (count > 2 && core2_available) {
        create_core1_worker(workers_[1]);
    }
}
RenderWorkers::~RenderWorkers() {
    if (worker_count_) {
        stopping_.store(true, std::memory_order_release);
        for (unsigned i = 0; i < workers_.size(); ++i)
            if (workers_[i].thread) LightEvent_Signal(&workers_[i].work);
        for (auto &worker : workers_) {
            if (!worker.thread) continue;
            threadJoin(worker.thread, U64_MAX);
            threadFree(worker.thread);
        }
    }
    if (changed_cpu_limit_) APT_SetAppCpuTimeLimit(previous_cpu_limit_);
}
void RenderWorkers::entry(void *self) {
    auto &slot = *static_cast<WorkerSlot*>(self);
    auto &pool = *slot.pool;
    for (;;) {
        LightEvent_Wait(&slot.work);
        if (pool.stopping_.load(std::memory_order_acquire)) return;
        pool.consume();
        LightSemaphore_Release(&pool.done_, 1);
    }
}
void RenderWorkers::consume() {
    for (;;) {
        const size_t index = next_.fetch_add(1, std::memory_order_relaxed);
        if (index >= count_) return;
        try { task_(index); }
        catch (...) {
            LightLock_Lock(&error_lock_);
            if (!error_) error_ = std::current_exception();
            LightLock_Unlock(&error_lock_);
        }
    }
}
void RenderWorkers::run(size_t count, std::function<void(size_t)> task) {
    if (!count) return;
    if (!worker_count_) { for (size_t i = 0; i < count; ++i) task(i); return; }
    task_ = std::move(task); count_ = count; error_ = nullptr;
    next_.store(0, std::memory_order_relaxed);
    for (auto &worker : workers_)
        if (worker.thread) LightEvent_Signal(&worker.work);
    consume();
    LightSemaphore_Acquire(&done_, worker_count_);
    task_ = {};
    if (error_) std::rethrow_exception(error_);
}
}
#elif !defined(DAYTONA_SERIAL_RENDER)
#include <algorithm>

namespace rt {
RenderWorkers::RenderWorkers(unsigned count) {
    if (!count) count = std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
    count = std::clamp(count, 1u, 32u);
    try {
        for (unsigned i = 1; i < count; ++i) threads_.emplace_back([this] {
            unsigned long long seen = 0;
            for (;;) {
                std::unique_lock lock(mutex_);
                work_.wait(lock, [&] { return stopping_ || generation_ != seen; });
                if (stopping_) return;
                seen = generation_;
                lock.unlock();
                consume();
                lock.lock();
                if (--outstanding_ == 0) done_.notify_one();
            }
        });
    } catch (...) { stop(); throw; }
}
void RenderWorkers::stop() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    work_.notify_all();
    for (auto &thread : threads_) if (thread.joinable()) thread.join();
}
RenderWorkers::~RenderWorkers() { stop(); }
void RenderWorkers::consume() {
    for (;;) {
        const size_t index = next_.fetch_add(1, std::memory_order_relaxed);
        if (index >= count_) return;
        try { task_(index); }
        catch (...) { std::lock_guard lock(mutex_); if (!error_) error_ = std::current_exception(); }
    }
}
void RenderWorkers::run(size_t count, std::function<void(size_t)> task) {
    if (!count) return;
    if (threads_.empty()) { for (size_t i = 0; i < count; ++i) task(i); return; }
    {
        std::lock_guard lock(mutex_);
        task_ = std::move(task); count_ = count; next_.store(0, std::memory_order_relaxed);
        error_ = nullptr; outstanding_ = threads_.size(); ++generation_;
    }
    work_.notify_all();
    consume();
    std::unique_lock lock(mutex_);
    done_.wait(lock, [&] { return outstanding_ == 0; });
    task_ = {};
    if (error_) std::rethrow_exception(error_);
}
}

#endif // DAYTONA_SERIAL_RENDER
