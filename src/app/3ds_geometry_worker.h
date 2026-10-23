#pragma once
#include "runtime/m2_board.h"
#include <3ds.h>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <memory>

namespace app {
class ThreeDsGeometryWorker final : public rt::GeometryExecutor {
    rt::M2Board &board_;
    Thread thread_=nullptr;
    LightEvent work_{},done_{};
    std::atomic<bool> stopping_{false};
    bool pending_=false;
    std::unique_ptr<std::array<uint32_t,0x8000>> commands_;
    rt::Geo *geo_=nullptr;
    uint32_t read_start_=0;
    u64 elapsed_=0;
    std::exception_ptr error_;
    static void entry(void *context) {
        auto &self=*static_cast<ThreeDsGeometryWorker*>(context);
        for(;;) {
            LightEvent_Wait(&self.work_);
            if(self.stopping_.load(std::memory_order_acquire)) return;
            const u64 start=svcGetSystemTick();
            try { self.geo_->parse_snapshot(self.read_start_,self.commands_->data()); }
            catch(...) { self.error_=std::current_exception(); }
            self.elapsed_=svcGetSystemTick()-start;
            LightEvent_Signal(&self.done_);
        }
    }
public:
    u64 jobs=0,copy_ticks=0,wait_ticks=0;
    explicit ThreeDsGeometryWorker(rt::M2Board &board):board_(board) {
        commands_=std::make_unique<std::array<uint32_t,0x8000>>();
        LightEvent_Init(&work_,RESET_ONESHOT);LightEvent_Init(&done_,RESET_ONESHOT);
        s32 priority=0x30;svcGetThreadPriority(&priority,CUR_THREAD_HANDLE);
        thread_=threadCreate(entry,this,64*1024,priority,2,false);
        if(thread_) board_.geometry_executor=this;
        else commands_.reset();
    }
    ~ThreeDsGeometryWorker() override {
        try { board_.finish_geometry(); } catch(...) { /* Completion has already been joined. */ }
        board_.geometry_executor=nullptr;
        if(thread_) {
            stopping_.store(true,std::memory_order_release);
            LightEvent_Signal(&work_);threadJoin(thread_,U64_MAX);threadFree(thread_);
        }
    }
    bool available() const { return thread_!=nullptr; }
    bool start(rt::Geo &geo,const uint32_t *commands,uint32_t read_start) override {
        if(!thread_) return false;
        if(pending_) throw rt::GeoFatal("Geometry worker dispatched before previous completion");
        const u64 start=svcGetSystemTick();
        std::memcpy(commands_->data(),commands,commands_->size()*sizeof(uint32_t));
        copy_ticks+=svcGetSystemTick()-start;
        geo_=&geo;read_start_=read_start;error_=nullptr;elapsed_=0;pending_=true;++jobs;
        LightEvent_Signal(&work_);
        return true;
    }
    double finish() override {
        if(!pending_) return 0;
        const u64 start=svcGetSystemTick();
        LightEvent_Wait(&done_);wait_ticks+=svcGetSystemTick()-start;
        pending_=false;geo_=nullptr;
        if(error_) std::rethrow_exception(error_);
        return double(elapsed_)*1000.0/SYSCLOCK_ARM11;
    }
};
}