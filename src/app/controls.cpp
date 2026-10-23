#include "app/controls.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace app {

namespace {
const struct {
    const char *name, *key;
} kActions[kNumActions] = {
    {"Steer left", "steer_left"}, {"Steer right", "steer_right"}, {"Accelerate", "accelerate"}, {"Brake", "brake"},
    {"Gear 1", "gear1"},          {"Gear 2", "gear2"},            {"Gear 3", "gear3"},           {"Gear 4", "gear4"},
    {"Shift up", "gear_up"},      {"Shift down", "gear_down"},    {"View 1 (red)", "view1"},    {"View 2 (blue)", "view2"},
    {"View 3 (yellow)", "view3"}, {"View 4 (green)", "view4"},    {"Coin", "coin"},              {"Start", "start"},
    {"Test", "test"},             {"Service", "service"},
};
} // namespace

const char *action_name(Action a) { return kActions[a].name; }
const char *action_key(Action a) { return kActions[a].key; }

std::string PadInput::describe() const {
    switch (kind) {
    case Button: {
        switch (SDL_GamepadButton(index)) {
        case SDL_GAMEPAD_BUTTON_SOUTH: return "A";
        case SDL_GAMEPAD_BUTTON_EAST: return "B";
        case SDL_GAMEPAD_BUTTON_WEST: return "X";
        case SDL_GAMEPAD_BUTTON_NORTH: return "Y";
        case SDL_GAMEPAD_BUTTON_BACK: return "View / Back";
        case SDL_GAMEPAD_BUTTON_START: return "Menu / Start";
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "LB";
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "RB";
        case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad Up";
        case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad Right";
        case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad Down";
        case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad Left";
        default: break;
        }
        const char *n = SDL_GetGamepadStringForButton(SDL_GamepadButton(index));
        return std::string("Button ") + (n ? n : "?");
    }
    case Axis: {
        if (index == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) return "RT (right trigger)";
        if (index == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) return "LT (left trigger)";
        if (index == SDL_GAMEPAD_AXIS_LEFTX) return dir > 0 ? "Left stick right" : "Left stick left";
        const char *n = SDL_GetGamepadStringForAxis(SDL_GamepadAxis(index));
        const bool trigger = index == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || index == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        return std::string(trigger ? "Trigger " : "Axis ") + (n ? n : "?") + (trigger ? "" : (dir > 0 ? " +" : " -"));
    }
    default: return "-";
    }
}

std::string PadInput::save() const {
    switch (kind) {
    case Button: return std::string("button:") + SDL_GetGamepadStringForButton(SDL_GamepadButton(index));
    case Axis: return std::string("axis:") + SDL_GetGamepadStringForAxis(SDL_GamepadAxis(index)) + (dir > 0 ? ":+" : ":-");
    default: return "none";
    }
}

PadInput PadInput::parse(const std::string &s) {
    PadInput p;
    if (s.compare(0, 7, "button:") == 0) {
        const SDL_GamepadButton b = SDL_GetGamepadButtonFromString(s.substr(7).c_str());
        if (b != SDL_GAMEPAD_BUTTON_INVALID) p.kind = Button, p.index = b;
    } else if (s.compare(0, 5, "axis:") == 0) {
        const auto c = s.find(':', 5);
        const SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(s.substr(5, c - 5).c_str());
        if (a != SDL_GAMEPAD_AXIS_INVALID) p.kind = Axis, p.index = a, p.dir = (c != std::string::npos && s[c + 1] == '-') ? -1 : 1;
    }
    return p;
}

void Controls::set_defaults() {
    auto b = [&](Action a, SDL_Scancode k, PadInput::Kind kind, int index, int dir = 1) {
        bind[a].key = k;
        bind[a].pad.kind = kind;
        bind[a].pad.index = index;
        bind[a].pad.dir = dir;
    };
    b(SteerLeft, SDL_SCANCODE_LEFT, PadInput::Axis, SDL_GAMEPAD_AXIS_LEFTX, -1);
    b(SteerRight, SDL_SCANCODE_RIGHT, PadInput::Axis, SDL_GAMEPAD_AXIS_LEFTX, +1);
    b(Accelerate, SDL_SCANCODE_UP, PadInput::Axis, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
    b(Brake, SDL_SCANCODE_DOWN, PadInput::Axis, SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    b(Gear1, SDL_SCANCODE_1, PadInput::Button, SDL_GAMEPAD_BUTTON_SOUTH);
    b(Gear2, SDL_SCANCODE_2, PadInput::Button, SDL_GAMEPAD_BUTTON_EAST);
    b(Gear3, SDL_SCANCODE_3, PadInput::Button, SDL_GAMEPAD_BUTTON_WEST);
    b(Gear4, SDL_SCANCODE_4, PadInput::Button, SDL_GAMEPAD_BUTTON_NORTH);
    b(GearUp, SDL_SCANCODE_W, PadInput::Button, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    b(GearDown, SDL_SCANCODE_Q, PadInput::Button, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    b(View1, SDL_SCANCODE_A, PadInput::Button, SDL_GAMEPAD_BUTTON_DPAD_UP);
    b(View2, SDL_SCANCODE_S, PadInput::Button, SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    b(View3, SDL_SCANCODE_D, PadInput::Button, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    b(View4, SDL_SCANCODE_F, PadInput::Button, SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    b(Coin, SDL_SCANCODE_5, PadInput::Button, SDL_GAMEPAD_BUTTON_BACK);
    b(Start, SDL_SCANCODE_RETURN, PadInput::Button, SDL_GAMEPAD_BUTTON_START);
    b(Test, SDL_SCANCODE_F2, PadInput::None, 0);
    b(Service, SDL_SCANCODE_F3, PadInput::None, 0);
    std::fill(std::begin(held_), std::end(held_), false);
    std::fill(std::begin(touch), std::end(touch), 0.f);
    gear = 1; steer = accel = brake = 0;
    deadzone = 0.08f;
    steer_invert = false;
}

float Controls::value(Action a, const bool *keys, SDL_Gamepad *pad) const {
    const Binding &b = bind[a];
    float v = (b.key != SDL_SCANCODE_UNKNOWN && keys && keys[b.key]) ? 1.f : 0.f;
    if (pad && b.pad.kind == PadInput::Button && SDL_GetGamepadButton(pad, SDL_GamepadButton(b.pad.index))) v = 1.f;
    if (pad && b.pad.kind == PadInput::Axis) {
        float x = SDL_GetGamepadAxis(pad, SDL_GamepadAxis(b.pad.index)) / 32767.f * float(b.pad.dir);
        x = std::clamp(x, 0.f, 1.f);
        x = x <= deadzone ? 0.f : (x - deadzone) / (1.f - deadzone); // rescale past the dead zone: full travel still reaches 1
        v = std::max(v, x);
    }
    return std::max(v, touch[a]);
}

bool Controls::analog_source(Action a, SDL_Gamepad *pad) const {
    return touch[a] > 0 || (pad && bind[a].pad.kind == PadInput::Axis);
}

rt::Inputs Controls::sample(const bool *keys, SDL_Gamepad *pad) {
    rt::Inputs in;
    // Steering: an analogue stick sets the position directly; keys ramp toward full lock and back.
    const float l = value(SteerLeft, keys, pad), r = value(SteerRight, keys, pad);
    float target = r - l;
    if (steer_invert) target = -target;
    const bool analog = (analog_source(SteerLeft, pad) || analog_source(SteerRight, pad)) &&
                        !(keys && (keys[bind[SteerLeft].key] || keys[bind[SteerRight].key]));
    steer = analog ? target : steer + std::clamp(target - steer, -0.12f, 0.12f);
    accel = value(Accelerate, keys, pad);
    brake = value(Brake, keys, pad);
    // ADC ranges (MAME's daytona ports): steering 0x20-0xe0 centred on 0x80, pedals 0x20 (up) to 0xe0 (floored)
    in.steer = uint8_t(std::lround(0x80 + std::clamp(steer, -1.f, 1.f) * 0x60));
    in.accel = uint8_t(std::lround(0x20 + std::clamp(accel, 0.f, 1.f) * 0xc0));
    in.brake = uint8_t(std::lround(0x20 + std::clamp(brake, 0.f, 1.f) * 0xc0));

    // Gears: direct selection, or sequential shifts on the press
    for (int g = 0; g < 4; g++)
        if (value(Action(Gear1 + g), keys, pad) > 0.5f) gear = g + 1;
    for (Action a : {GearUp, GearDown}) {
        const bool now = value(a, keys, pad) > 0.5f;
        if (now && !held_[a]) gear = std::clamp(gear + (a == GearUp ? 1 : -1), 1, 4);
        held_[a] = now;
    }
    static const uint8_t gearvalue[5] = {0, 2, 1, 6, 5}; // MAME daytona_gearbox_r: neutral, 1-4
    in.in1 = uint8_t((in.in1 & ~0x70) | (gearvalue[gear] << 4));

    auto low = [&](uint8_t &port, uint8_t bit, Action a) { if (value(a, keys, pad) > 0.5f) port &= uint8_t(~bit); };
    low(in.in0, 0x01, Coin);
    low(in.in0, 0x04, Test);
    low(in.in0, 0x08, Service);
    low(in.in0, 0x10, Start);
    low(in.in0, 0x20, View1);
    low(in.in0, 0x40, View2);
    low(in.in0, 0x80, View3);
    low(in.in1, 0x01, View4);
    return in;
}

} // namespace app
