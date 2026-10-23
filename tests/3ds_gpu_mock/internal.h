#pragma once
#include <stddef.h>
#include <stdint.h>
typedef uint32_t u32;
typedef struct { u32* cmdBuf; size_t cmdBufSize; } C3D_Context;
#define USERBREAK_PANIC 1
#define GX_CMDLIST_FLUSH 2
#define R_FAILED(result) ((result) < 0)
C3D_Context* C3Di_GetContext(void);
void GPUCMD_GetBuffer(u32** address, u32* size, u32* offset);
int GSPGPU_FlushDataCache(const void* data, size_t size);
void C3D_FrameEnd(unsigned char flags);
void svcBreak(int reason);