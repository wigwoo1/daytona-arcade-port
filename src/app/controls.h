// Control bindings: each arcade control is bound to a key and a gamepad input
// (a button, or one half of an axis). Axis bindings are analogue: triggers
// give the accelerator and brake their full travel, a stick gives steering.
#pragma once

#include "runtime/m2_board.h"

#include <SDL3/SDL.h>

#include <string>

namespace app {

enum Action {
    SteerLeft, SteerRight, Accelerate, Brake,
    Gear1, Gear2, Gear3, Gear4, GearUp, GearDown,
    View1, View2, View3, View4,
    Coin, Start, Test, Service,
    kNumActions
};
const char *action_name(Action a);   // for the UI
const char *action_key(Action a);    // for the config file

struct PadInput {
    enum Kind { None, Button, Axis } kind = None;
    int index = 0; // SDL_GamepadButton or SDL_GamepadAxis
    int dir = 1;   // axis: +1 the positive half, -1 the negative half
    std::string describe() const;
    std::string save() const;
    static PadInput parse(const std::string &s);
};

struct Binding {
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    PadInput pad;
};

struct Controls {
    Binding bind[kNumActions];
    float touch[kNumActions] = {}; // independent multitouch values merged with physical controls
    float deadzone = 0.08f;       // stick and trigger dead zone (fraction of travel)
    bool steer_invert = false;

    void set_defaults();
    // Current value of an action, 0..1 (keys and buttons are 0 or 1).
    float value(Action a, const bool *keys, SDL_Gamepad *pad) const;
    bool analog_source(Action a, SDL_Gamepad *pad) const; // an axis binding is in use

    // Build this frame's I/O board inputs. Keyboard steering ramps; analogue
    // steering and pedals are direct.
    rt::Inputs sample(const bool *keys, SDL_Gamepad *pad);

    // Live values for the UI
    float steer = 0, accel = 0, brake = 0;
    int gear = 1;
private:
    bool held_[kNumActions] = {};
};

} // namespace app
