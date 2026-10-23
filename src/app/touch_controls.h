#pragma once
#include "app/controls.h"
#include <cstdint>
#include <vector>

namespace app {
// Coordinates are normalized to the usable landscape area, independent of DPI.
class TouchControls {
public:
    enum Command { None, Toggle, Menu };
    Command down(int64_t finger, float x, float y, bool visible);
    void motion(int64_t finger, float x, float y);
    void up(int64_t finger);
    void clear();
    void apply(Controls &controls) const;
    void draw(float x, float y, float width, float height, bool visible) const;
private:
    struct Finger { int64_t id; int control; float x, y; };
    std::vector<Finger> fingers_;
};
}
