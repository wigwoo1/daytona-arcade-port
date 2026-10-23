// Run the production submission helper against separate CPU/GPU cache images.
// FrameEnd's FLUSH bit updates only its final segment, as libctru gx.c does.
#include "internal.h"
#include <stdio.h>
#include <string.h>

void Daytona_FrameEndFlushed(void);
static u32 cpu[128], gpu[128];
static C3D_Context context = { cpu, 128 };
static size_t tail, used, prefix_bytes;
static int calls, panic_count, fail_flush, failures;
static unsigned char end_flags;
static void check(int ok, const char* message) {
    if (!ok) { printf("FAIL: %s\n", message); ++failures; }
}
C3D_Context* C3Di_GetContext(void) { return &context; }
void GPUCMD_GetBuffer(u32** address, u32* size, u32* offset) {
    if (address) *address = (u32*)((uintptr_t)cpu + tail * sizeof(u32));
    if (size) *size = tail <= 128 ? (u32)(128-tail) : 0;
    if (offset) *offset = (u32)used;
}
int GSPGPU_FlushDataCache(const void* data, size_t bytes) {
    check(calls == 0, "completed command prefix is flushed before submission");
    check(data == cpu && bytes <= sizeof(cpu), "flush stays inside command allocation");
    if (fail_flush) return -1;
    prefix_bytes = bytes;
    memcpy(gpu, data, bytes);
    return 0;
}
void C3D_FrameEnd(unsigned char flags) {
    ++calls; end_flags = flags;
    // Citro3D appends closing commands after the application's prefix flush.
    if (used) {
        cpu[tail+used] = 0x12345678;
        cpu[tail+used+1] = 0x87654321;
        if (flags & GX_CMDLIST_FLUSH)
            memcpy(gpu+tail, cpu+tail, (used+2)*sizeof(u32));
    }
}
void svcBreak(int reason) { check(reason == USERBREAK_PANIC, "invalid buffers panic safely"); ++panic_count; }
static void reset(size_t split, size_t active) {
    tail=split; used=active; prefix_bytes=0;
    calls=panic_count=fail_flush=0; end_flags=0;
    for (size_t i=0;i<128;++i) { cpu[i]=0xabc00000u+(u32)i; gpu[i]=0; }
}
static void valid_case(size_t split, size_t active) {
    reset(split,active);
    Daytona_FrameEndFlushed();
    check(!panic_count && calls == 1, "valid frame submits once");
    check(end_flags == GX_CMDLIST_FLUSH, "final segment flush includes SDK closing commands");
    check(prefix_bytes == split*sizeof(u32), "every earlier split is flushed");
    const size_t words = split + (active ? active+2 : 0);
    check(!memcmp(cpu,gpu,words*sizeof(u32)), "GPU sees current commands in every segment");
}
int main(void) {
    valid_case(0,12);     // One unsplit command list.
    valid_case(16,12);    // A target change before the final list.
    valid_case(80,20);    // Multiple earlier lists in one command allocation.
    valid_case(80,0);     // Final list empty: previous lists must still be flushed.
    valid_case(0,0);      // No commands.
    reset(129,0); Daytona_FrameEndFlushed();
    check(panic_count == 1 && calls == 0, "reject command pointer beyond allocation");
    reset(100,40); Daytona_FrameEndFlushed();
    check(panic_count == 1 && calls == 0, "reject active commands beyond allocation");
    reset(16,12); fail_flush=1; Daytona_FrameEndFlushed();
    check(panic_count == 1 && calls == 0, "failed cache flush never submits stale commands");
    printf("3DS split command flush tests: %d failures\n", failures);
    return failures ? 1 : 0;
}