#pragma once
// Copy one already-built eye's draw commands, changing only its stereo vector.
// The caller emits the new framebuffer/viewport and initial eye vector first.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline bool daytona_stereo_command_pass(uint32_t *destination,
    const uint32_t *source, size_t words, unsigned uniform,
    unsigned config_register, unsigned data_register, unsigned *vectors)
{
    bool configured=false;
    unsigned component=0, count=0;
    for(size_t at=0;at<words;) {
        if(words-at<2) return false;
        const uint32_t header=source[at+1];
        const unsigned parameters=((header>>20)&0x7ffu)+1;
        const size_t packet=(parameters+2u)&~(size_t)1;
        if(parameters>256 || packet>words-at) return false;
        const unsigned base=header&0xffffu;
        for(unsigned p=0;p<parameters;++p) {
            const unsigned reg=base+((header>>31)?p:0);
            const size_t index=at+(p?p+1:0);
            const uint32_t value=source[index];
            if(reg==config_register) {
                if(((header>>16)&15u)!=15 || value!=(0x80000000u|uniform) ||
                    (configured && component!=4)) return false;
                configured=true; component=0;
            } else if(reg>=data_register && reg<data_register+8) {
                // Runtime Citro3D vectors use four float32 words in W,Z,Y,X
                // order. Shader constants/other vectors must be flushed before
                // capture, so any other uniform format is a safe rejection.
                if(reg!=data_register || ((header>>16)&15u)!=15 ||
                    !configured || component>=4 ||
                    (value&0x7f800000u)==0x7f800000u) return false;
                if(component<2 && (value&0x7fffffffu)) return false;
                if(destination && component>=2) {
                    destination[index]=(value&0x7fffffffu)?value^0x80000000u:
                        (component==3?0x80000000u:0u);
                }
                if(++component==4) ++count;
            }
        }
        at+=packet;
    }
    if(configured && component!=4) return false;
    if(vectors) *vectors=count;
    return true;
}

static inline bool daytona_copy_stereo_commands(uint32_t *destination,
    size_t capacity, const uint32_t *source, size_t words, unsigned uniform,
    unsigned config_register, unsigned data_register, unsigned *vectors)
{
    if(vectors) *vectors=0;
    if(uniform>=96 || words>capacity || words>SIZE_MAX/sizeof(uint32_t)) return false;
    if(!words) return true;
    if(!source || !destination) return false;
    const uintptr_t s=(uintptr_t)source,d=(uintptr_t)destination;
    const size_t bytes=words*sizeof(uint32_t);
    if((d>=s?d-s:s-d)<bytes) return false;
    unsigned count=0;
    // Validate the complete list before writing: malformed lists cannot append
    // partial commands or overwrite the destination guard region.
    if(!daytona_stereo_command_pass(NULL,source,words,uniform,config_register,data_register,&count)) return false;
    memcpy(destination,source,bytes);
    if(!daytona_stereo_command_pass(destination,source,words,uniform,config_register,data_register,NULL)) return false;
    if(vectors) *vectors=count;
    return true;
}