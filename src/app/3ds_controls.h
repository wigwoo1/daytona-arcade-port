#pragma once
#include "runtime/m2_board.h"
#include <algorithm>
#include <cstdlib>
namespace app {
// libctru HID bit assignments; checked against SDK constants by the front end.
struct ThreeDsButtons {
    static constexpr uint32_t A=1, B=2, Select=4, Start=8,
        Right=16, Left=32, Up=64, Down=128, R=256, L=512, X=1024, Y=2048;
};
class ThreeDsControls {
public:
    int gear = 1;
    rt::Inputs sample(uint32_t held, uint32_t pressed, int circle_x) {
        rt::Inputs in;
        const int steering = std::abs(circle_x) < 15 ? 0 : circle_x;
        in.steer = uint8_t(std::clamp(128 + steering * 96 / 156, 32, 224));
        if (held & ThreeDsButtons::A) in.accel = 224;
        if (held & ThreeDsButtons::B) in.brake = 224;
        if (pressed & ThreeDsButtons::L) gear = std::max(1, gear-1);
        if (pressed & ThreeDsButtons::R) gear = std::min(4, gear+1);
        constexpr unsigned gears[] = {0,2,1,6,5};
        in.in1 = (in.in1 & ~0x70) | (gears[gear] << 4);
        if (held & ThreeDsButtons::Select) in.in0 &= ~1u;
        if (held & ThreeDsButtons::Start) in.in0 &= ~0x10u;
        if (held & ThreeDsButtons::Up) in.in0 &= ~0x20u;
        if (held & ThreeDsButtons::Right) in.in0 &= ~0x40u;
        if (held & ThreeDsButtons::Down) in.in0 &= ~0x80u;
        if (held & ThreeDsButtons::Left) in.in1 &= ~1u;
        return in;
    }
};
}
