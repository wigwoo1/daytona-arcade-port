#pragma once
#include <cmath>
#include <stdexcept>

namespace app {
// Select displayed frames without changing the number or timing of simulation steps.
class PresentationCadence {
public:
    explicit PresentationCadence(double simulation_hz) : hz_(simulation_hz) {
        if (!std::isfinite(hz_) || hz_ <= 0) throw std::invalid_argument("invalid simulation rate");
    }
    void set_limit(unsigned fps) {
        if (fps != 0 && fps != 30 && fps != 40) throw std::invalid_argument("invalid display limit");
        if (fps != limit_) { limit_ = fps; phase_ = hz_ - fps; }
    }
    bool step() {
        if (!limit_ || limit_ >= hz_) return true;
        phase_ += limit_;
        if (phase_ < hz_) return false;
        phase_ -= hz_;
        return true;
    }
private:
    double hz_, phase_ = 0;
    unsigned limit_ = 0;
};
}
