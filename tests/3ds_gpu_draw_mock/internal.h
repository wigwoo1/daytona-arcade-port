#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1u<<(n))
// Use the real SDK enums, registers and command macros in both implementations.
#include <3ds/gpu/gpu.h>
typedef struct { struct { u32 base_paddr; } bufInfo; u32 flags; } C3D_Context;
enum { C3DiF_DrawUsed=BIT(1), USERBREAK_PANIC=1 };
C3D_Context *C3Di_GetContext(void);
void C3Di_UpdateContext(void);
u32 osConvertVirtToPhys(const void *pointer);
void svcBreak(int reason);
