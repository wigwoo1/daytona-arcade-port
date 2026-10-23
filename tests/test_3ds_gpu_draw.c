// Compare the production helper against the unmodified local SDK source
// compiled as reference-drawElements.c by the Y: experiment runner.
#include "internal.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
void C3D_DrawElements(GPU_Primitive_t,int,int,const void*);
void Daytona_DrawTrianglesPacked(int,const void*);
u32 *gpuCmdBuf, gpuCmdBufSize, gpuCmdBufOffset;
static u32 buffer[256], expected[256], physical, prefix;
static C3D_Context context;
static int failures, panics, updates, raw_calls, cases;
static u16 indices[8];
static void check(int ok,const char *message) {
    if(!ok){printf("FAIL case %d: %s\n",cases,message);++failures;}
}
C3D_Context *C3Di_GetContext(void){return &context;}
u32 osConvertVirtToPhys(const void *pointer) {
    check(pointer==indices,"index address passed unchanged to physical conversion");
    return physical;
}
void svcBreak(int reason){check(reason==USERBREAK_PANIC,"safe panic reason");++panics;}
void GPUCMD_Add(u32 header,const u32 *params,u32 count) {
    check(count==1 && gpuCmdBuf && gpuCmdBufOffset+2<=gpuCmdBufSize,"reference command fits");
    if(count!=1 || !gpuCmdBuf || gpuCmdBufOffset+2>gpuCmdBufSize)return;
    gpuCmdBuf[gpuCmdBufOffset++]=*params;
    gpuCmdBuf[gpuCmdBufOffset++]=header;
}
void GPUCMD_AddRawCommands(const u32 *words,u32 count) {
    ++raw_calls;
    check(gpuCmdBuf && gpuCmdBufOffset<=gpuCmdBufSize && count<=gpuCmdBufSize-gpuCmdBufOffset,"packed append fits");
    if(!gpuCmdBuf || gpuCmdBufOffset>gpuCmdBufSize || count>gpuCmdBufSize-gpuCmdBufOffset)return;
    memcpy(gpuCmdBuf+gpuCmdBufOffset,words,count*sizeof(u32));
    gpuCmdBufOffset+=count;
}
void C3Di_UpdateContext(void) {
    ++updates;
    for(u32 i=0;i<prefix;i+=2)GPUCMD_AddWrite(GPUREG_0000,0xcafe0000u+i);
}
static void reset(u32 pa,u32 base,u32 start,u32 state_words,u32 capacity) {
    ++cases;physical=pa;context.bufInfo.base_paddr=base;context.flags=0x40000000;
    prefix=state_words;panics=updates=raw_calls=0;
    for(u32 i=0;i<256;++i)buffer[i]=0xabcd0000u+i;
    gpuCmdBuf=buffer;gpuCmdBufSize=capacity;gpuCmdBufOffset=start;
}
static void compare(int count,u32 pa,u32 base,u32 start,u32 state_words) {
    reset(pa,base,start,state_words,256);
    C3D_DrawElements(GPU_TRIANGLES,count,1,indices); // SDK C3D_UNSIGNED_SHORT=1.
    memcpy(expected,buffer,sizeof(buffer));
    const u32 offset=gpuCmdBufOffset,flags=context.flags;
    const int update_count=updates;
    check(!panics && !raw_calls,"SDK reference completed");
    reset(pa,base,start,state_words,256);
    Daytona_DrawTrianglesPacked(count,indices);
    check(!panics,"valid packed draw does not panic");
    check(!memcmp(expected,buffer,sizeof(buffer)),"every command word and untouched guard matches actual SDK");
    check(offset==gpuCmdBufOffset && flags==context.flags,"command offset and all context flags match");
    check(updates==update_count,"state updates occur at the same point");
    check(raw_calls==(pa>=base?1:0),"draw packet appended once or rejected before state");
}
int main(void) {
    const int counts[]={0,3,6,24,384,24576,INT_MAX,-1};
    const u32 addresses[]={0,0x18000000,0x1c000000,0x20000000,0x80000000,0xfffffffe};
    const u32 bases[]={0,0x18000000,0x1c000000,0x80000000,0xffffffff};
    for(unsigned c=0;c<sizeof(counts)/sizeof(*counts);++c)
        for(unsigned a=0;a<sizeof(addresses)/sizeof(*addresses);++a)
            for(unsigned b=0;b<sizeof(bases)/sizeof(*bases);++b)
                compare(counts[c],addresses[a],bases[b],(a+b)%9*2,(c%3)*2);
    // Exact capacity after a context-state prefix, then one word too little.
    reset(0x18000010,0x18000000,8,2,40);Daytona_DrawTrianglesPacked(6,indices);
    check(!panics && raw_calls==1 && gpuCmdBufOffset==40,"exact capacity succeeds after context update");
    reset(0x18000010,0x18000000,8,2,39);Daytona_DrawTrianglesPacked(6,indices);
    check(panics==1 && !raw_calls && gpuCmdBufOffset==10,"one word short rejects the draw after state update");
    check(context.flags==0x40000000,"failed draw never marked used");
    check(buffer[10]==0xabcd000au && buffer[39]==0xabcd0027u,"capacity failure leaves draw area and guard untouched");
    reset(0x18000010,0x18000000,100,0,99);Daytona_DrawTrianglesPacked(6,indices);
    check(panics==1 && !raw_calls && gpuCmdBufOffset==100,"offset beyond buffer rejects without unsigned wrap");
    reset(0x18000010,0x18000000,0,0,256);gpuCmdBuf=NULL;Daytona_DrawTrianglesPacked(6,indices);
    check(panics==1 && !raw_calls,"null command buffer rejected");
    printf("Packed indexed draw: %d cases, %d failures\n",cases,failures);
    return failures?1:0;
}
