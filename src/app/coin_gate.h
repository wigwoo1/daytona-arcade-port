// Optional host input policy. The original game and its credit accounting stay intact.
#pragma once
#include "runtime/m2_board.h"
#include <algorithm>
namespace app {
class CoinGate {
public:
    int pending = 0; // coins not yet delivered to the game; persisted by the app
    void suspend() { armed_ = false; }
    void reset_delivery() { remaining_ = phase_ = 0; suspend(); }
    rt::Inputs sample(rt::Inputs in, bool enabled) {
        if (!enabled && !pending && !remaining_ && !phase_) return in;
        const bool coin = !(in.in0 & 1), start = !(in.in0 & 0x10);
        in.in0 |= 1;
        if (!armed_) {
            in.in0 |= 0x10;
            if (!coin && !start) armed_ = true;
            coin_ = coin; start_ = start;
        } else {
            if (coin && !coin_) pending = std::min(pending + 1, 999);
            if ((start && !start_) || !enabled) remaining_ = pending;
            coin_ = coin; start_ = start;
        }
        // Ten native frames on, ten off: distinct coin-switch pulses even
        // when the host button is held. Coins added later need another Start.
        if (phase_ == 0 && remaining_ > 0) {
            --pending; --remaining_; phase_ = 20;
        }
        if (phase_ > 10) in.in0 &= uint8_t(~1);
        if (phase_) --phase_;
        return in;
    }
private:
    bool armed_ = false, coin_ = false, start_ = false;
    int remaining_ = 0, phase_ = 0;
};
} // namespace app
