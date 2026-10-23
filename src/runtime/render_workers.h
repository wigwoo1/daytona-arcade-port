#pragma once
#if defined(__3DS__)
extern "C" {
#include <3ds/thread.h>
#include <3ds/synchronization.h>
}
#include <array>
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
namespace rt {
class RenderWorkers {
public:
    explicit RenderWorkers(unsigned count = 1);
    ~RenderWorkers();
    RenderWorkers(const RenderWorkers &) = delete;
    RenderWorkers &operator=(const RenderWorkers &) = delete;
    unsigned size() const { return 1 + worker_count_; }
    void run(size_t count, std::function<void(size_t)> task);
private:
    static void entry(void *self);
    void consume();
    struct WorkerSlot {
        RenderWorkers *pool = nullptr;
        Thread thread = nullptr;
        LightEvent work{};
    };
    std::array<WorkerSlot, 2> workers_{};
    unsigned worker_count_ = 0;
    LightSemaphore done_{};
    LightLock error_lock_;
    std::atomic<bool> stopping_{false};
    std::atomic<size_t> next_{0};
    size_t count_ = 0;
    std::function<void(size_t)> task_;
    std::exception_ptr error_;
    u32 previous_cpu_limit_ = 0;
    bool changed_cpu_limit_ = false;
};
}
#elif defined(DAYTONA_SERIAL_RENDER)
#include <cstddef>
#include <functional>
namespace rt {
class RenderWorkers {
public:
    explicit RenderWorkers(unsigned = 0) {}
    unsigned size() const { return 1; }
    void run(size_t count, std::function<void(size_t)> task) { for (size_t i = 0; i < count; ++i) task(i); }
};
}
#else
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace rt {
// Synchronous, non-reentrant jobs on persistent workers. The calling thread
// participates. Completion is a barrier before any frame state can change.
class RenderWorkers {
public:
    explicit RenderWorkers(unsigned count = 0);
    ~RenderWorkers();
    RenderWorkers(const RenderWorkers &) = delete;
    RenderWorkers &operator=(const RenderWorkers &) = delete;
    unsigned size() const { return unsigned(threads_.size()) + 1; }
    void run(size_t count, std::function<void(size_t)> task);
private:
    void consume();
    void stop();
    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable work_, done_;
    bool stopping_ = false;
    unsigned long long generation_ = 0;
    size_t outstanding_ = 0, count_ = 0;
    std::atomic<size_t> next_{0};
    std::function<void(size_t)> task_;
    std::exception_ptr error_;
};
}

#endif // DAYTONA_SERIAL_RENDER
