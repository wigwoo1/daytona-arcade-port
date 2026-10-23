#include "app/controls.h"
#include "app/coin_gate.h"
#include <cstdio>

int main() {
    if (!SDL_Init(SDL_INIT_GAMEPAD)) { std::printf("SDL init: %s\n", SDL_GetError()); return 1; }
    SDL_VirtualJoystickDesc d{}; SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD; d.name = "Xbox mapping test";
    d.naxes = SDL_GAMEPAD_AXIS_COUNT; d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    const auto id = SDL_AttachVirtualJoystick(&d);
    auto *pad = SDL_OpenGamepad(id);
    if (!pad) { std::printf("virtual controller: %s\n", SDL_GetError()); SDL_Quit(); return 1; }
    auto *joy = SDL_GetGamepadJoystick(pad);
    int failures = 0;
    auto check = [&](bool ok, const char *name) { if (!ok) { ++failures; std::printf("FAIL: %s\n", name); } };
    auto button = [&](int b, bool down) { SDL_SetJoystickVirtualButton(joy, b, down); SDL_UpdateJoysticks(); };
    auto axis = [&](int a, int value) { SDL_SetJoystickVirtualAxis(joy, a, Sint16(value)); SDL_UpdateJoysticks(); };
    axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768); axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
    app::Controls c; c.set_defaults();
    auto sample = [&] { return c.sample(nullptr, pad); };
    check(sample().steer == 0x80 && sample().accel == 0x20 && sample().brake == 0x20, "neutral steering and pedals");
    axis(SDL_GAMEPAD_AXIS_LEFTX, 32767); check(sample().steer == 0xe0, "full right steering");
    axis(SDL_GAMEPAD_AXIS_LEFTX, -32768); check(sample().steer == 0x20, "full left steering");
    axis(SDL_GAMEPAD_AXIS_LEFTX, 0);
    axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767); check(sample().accel == 0xe0, "RT full accelerator");
    axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767); check(sample().brake == 0xe0, "LT full brake");
    axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 0); const auto half = sample().accel;
    check(half > 0x20 && half < 0xe0, "analogue partial pedal");
    for (int b : {SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_DPAD_UP,
                  SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT}) {
        button(b, true); const auto in = sample();
        const bool active = b == SDL_GAMEPAD_BUTTON_BACK ? !(in.in0 & 1) : b == SDL_GAMEPAD_BUTTON_START ? !(in.in0 & 0x10) :
            b == SDL_GAMEPAD_BUTTON_DPAD_UP ? !(in.in0 & 0x20) : b == SDL_GAMEPAD_BUTTON_DPAD_RIGHT ? !(in.in0 & 0x40) :
            b == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? !(in.in0 & 0x80) : !(in.in1 & 1);
        check(active, "coin, start or D-pad camera reaches arcade input");
        button(b, false);
        const auto released = sample(); check((released.in0 & 0xf1) == 0xf1 && (released.in1 & 1), "arcade button releases");
    }
    const int gears[] = {SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH};
    for (int i = 0; i < 4; ++i) { button(gears[i], true); sample(); check(c.gear == i + 1, "face button direct gear"); button(gears[i], false); }
    c.set_defaults(); button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, true); sample(); sample();
    check(c.gear == 2, "held RB shifts once"); button(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, false); sample();
    button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true); sample(); check(c.gear == 1, "LB downshift");
    app::CoinGate gate;
    rt::Inputs idle, coin, start;
    coin.in0 &= uint8_t(~1); start.in0 &= uint8_t(~0x10);
    gate.sample(idle, true);
    for (int n = 0; n < 3; ++n) {
        for (int f = 0; f < 30; ++f) check(gate.sample(coin, true).in0 & 1, "held coin stays queued");
        gate.sample(idle, true);
    }
    check(gate.pending == 3, "three presses count three coins");
    for (int f = 0; f < 500; ++f) check(gate.sample(idle, true).in0 & 1, "attract receives no coins before Start");
    int edges = 0; bool previous = false;
    for (int f = 0; f < 100; ++f) {
        bool active = !(gate.sample(start, true).in0 & 1);
        if (active && !previous) ++edges;
        previous = active;
    }
    check(edges == 3 && gate.pending == 0, "Start sends exactly three separated coin pulses");
    gate.sample(coin, true); // Start released, another coin inserted
    check(gate.pending == 1, "new coin after authorization waits again");
    gate.suspend();
    for (int f = 0; f < 40; ++f) check(gate.sample(start, true).in0 & 1, "launcher-held Start cannot spend coins");
    gate.sample(idle, true);
    check(!(gate.sample(start, true).in0 & 1), "fresh Start after resume spends coin");
    app::CoinGate restored; restored.pending = 2;
    restored.sample(idle, true);
    check(restored.sample(idle, true).in0 & 1, "restored coins wait for Start");
    app::CoinGate original;
    check(original.sample(coin, false).in0 == coin.in0, "disabled policy preserves arcade input");
    SDL_CloseGamepad(pad); SDL_DetachVirtualJoystick(id); SDL_Quit();
    std::printf("Xbox controller tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
