#include "app/3ds_gpu_command_replay.h"
#include <stdio.h>
#include <stdlib.h>
#define CFG 0x2c0u
#define DATA 0x2c1u
#define U 5u
static uint32_t src[4096], expected[4096], saved[4096], dst[4098];
static size_t used;
static unsigned failures,cases;
static void check(bool ok,const char *message){if(!ok){++failures;printf("FAIL %s\n",message);}}
static void packet(unsigned reg,const uint32_t *v,unsigned n){
    src[used]=expected[used]=v[0];
    src[used+1]=expected[used+1]=(reg|0x000f0000u|((n-1)<<20));
    for(unsigned i=1;i<n;++i)src[used+i+1]=expected[used+i+1]=v[i];
    used+=n+1;
    if(!(n&1))src[used]=expected[used]=0xdecafbad,++used;
}
static uint32_t bits(float value){uint32_t b;memcpy(&b,&value,4);return b;}
static void vector(float y,float x,bool split){
    const uint32_t config=0x80000000u|U;packet(CFG,&config,1);
    const uint32_t v[4]={0,0,bits(y),bits(x)};
    const float ry=y==0?0.f:-y,rx=x==0?-0.f:-x;
    const uint32_t right[4]={0,0,bits(ry),bits(rx)};
    if(split){for(unsigned c=0;c<4;++c){const size_t at=used;packet(DATA,v+c,1);expected[at]=right[c];}}
    else {const size_t at=used;packet(DATA,v,4);expected[at+3]=right[2];expected[at+4]=right[3];}
}
static void valid(unsigned vectors){
    memcpy(saved,src,used*4);for(size_t i=0;i<4098;++i)dst[i]=0xfeedbabe;
    unsigned count=999;
    check(daytona_copy_stereo_commands(dst+1,4096,src,used,U,CFG,DATA,&count),"accept supported command stream");
    check(count==vectors,"count complete stereo vectors");
    check(!memcmp(dst+1,expected,used*4),"preserve every register/draw word except reference right-eye vector");
    check(!memcmp(src,saved,used*4),"recorded left-eye source is immutable");
    check(dst[0]==0xfeedbabe&&dst[used+1]==0xfeedbabe,"output guards survive");++cases;
}
static void invalid(size_t n,const char *message){
    for(size_t i=0;i<4098;++i)dst[i]=0xfeedbabe;
    unsigned count=999;
    check(!daytona_copy_stereo_commands(dst+1,4096,src,n,U,CFG,DATA,&count),message);
    check(count==0,"rejected list reports no vectors");
    for(size_t i=0;i<4098;++i)if(dst[i]!=0xfeedbabe){check(false,"rejected list writes no partial commands");break;}
    ++cases;
}
int main(void){
    uint32_t seed=1234567;
    for(unsigned test=0;test<768;++test){
        used=0;unsigned vectors=0;
        // Independent expected stream uses float negation, including the
        // right-eye zero signs, rather than the production bit operations.
        for(unsigned run=0;run<1+test%5;++run){
            seed=seed*1664525u+1013904223u;
            float y=-(float)(seed&65535u)/4096.f;
            float x=(float)((seed>>16)&65535u)/32768.f;
            if((test+run)%3==0)y=0,x=-0.f; // HUD: no separation.
            if((test+run)%7==0)y=-0.f,x=0; // Slider zero in world geometry.
            if((test+run)%11==0)y=-1e-30f,x=1e-30f;
            vector(y,x,(test+run)&1);++vectors;
            uint32_t draw[256];unsigned n=1+(seed%256);
            for(unsigned j=0;j<n;++j)draw[j]=seed+j;
            packet(0x100,draw,n); // Arbitrary full-size nonuniform packets.
        }
        valid(vectors);
    }
    used=0;{uint32_t v=1;packet(0x100,&v,1);}valid(0);
    used=0;vector(-2.f,3.f,false);const size_t n=used;
    invalid(1,"reject missing command header");invalid(n-1,"reject missing aligned packet padding");
    uint32_t old=src[0];src[0]=0x80000000u|(U+1);invalid(n,"reject another uniform");src[0]=old;
    src[0]=U;invalid(n,"reject float24 shader constants in captured list");src[0]=old;
    old=src[5];src[5]=0x7f800000;invalid(n,"reject nonfinite separation");src[5]=old;
    old=src[2];src[2]=bits(1.f);invalid(n,"reject unexpected stereo Z/W components");src[2]=old;
    old=src[3];src[3]&=~0x00010000u;invalid(n,"reject partial uniform byte masks");src[3]=old;
    old=src[3];src[3]=DATA|0x000f0000u|(256u<<20);invalid(n,"reject unsupported oversized packet");src[3]=old;
    check(!daytona_copy_stereo_commands(dst,n-1,src,n,U,CFG,DATA,NULL),"reject insufficient destination room");
    check(!daytona_copy_stereo_commands(src,n,src,n,U,CFG,DATA,NULL),"reject source/destination alias");
    check(!daytona_copy_stereo_commands(src+1,n,src,n,U,CFG,DATA,NULL),"reject partially overlapping ranges");
    check(!daytona_copy_stereo_commands(NULL,n,src,n,U,CFG,DATA,NULL),"reject missing destination");
    check(!daytona_copy_stereo_commands(dst,n,NULL,n,U,CFG,DATA,NULL),"reject missing source");
    check(!daytona_copy_stereo_commands(dst,n,src,n,96,CFG,DATA,NULL),"reject out-of-range uniform");
    used=0;{uint32_t v[4]={0,0,bits(-1.f),bits(2.f)};packet(DATA,v,4);}invalid(used,"reject data without explicit register configuration");
    used=0;{uint32_t c=0x80000000u|U;packet(CFG,&c,1);}invalid(used,"reject unfinished vector");
    printf("Stereo command replay: %u streams, %u failures\n",cases,failures);
    return failures?1:0;
}