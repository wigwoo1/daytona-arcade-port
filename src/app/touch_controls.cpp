#include "app/touch_controls.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace app {
namespace {
struct Button { float x, y, w, h; int action; const char *label; };
constexpr int stick = -1, toggle = -2, menu = -3;
constexpr Button buttons[] = {
    {0.77f, 0.02f, 0.12f, 0.09f, toggle, "Controls"},
    {0.90f, 0.02f, 0.09f, 0.09f, menu, "Menu"},
    {0.02f, 0.60f, 0.25f, 0.34f, stick, "STEER"},
    {0.78f, 0.68f, 0.20f, 0.27f, Accelerate, "GAS"},
    {0.63f, 0.75f, 0.13f, 0.20f, Brake, "BRAKE"},
    {0.30f, 0.83f, 0.10f, 0.12f, GearDown, "Gear -"},
    {0.42f, 0.83f, 0.10f, 0.12f, GearUp, "Gear +"},
    {0.02f, 0.02f, 0.10f, 0.09f, Coin, "Coin"},
    {0.14f, 0.02f, 0.10f, 0.09f, Start, "Start"},
    {0.31f, 0.02f, 0.08f, 0.09f, View1, "Cam 1"},
    {0.40f, 0.02f, 0.08f, 0.09f, View2, "Cam 2"},
    {0.49f, 0.02f, 0.08f, 0.09f, View3, "Cam 3"},
    {0.58f, 0.02f, 0.08f, 0.09f, View4, "Cam 4"},
};
bool inside(const Button &b, float x, float y) {
    return x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h;
}
}
TouchControls::Command TouchControls::down(int64_t id, float x, float y, bool visible) {
    up(id);
    for (int n = 0; n < int(std::size(buttons)); ++n) {
        const auto &b = buttons[n];
        if ((!visible && b.action >= -1) || !inside(b, x, y)) continue;
        if (b.action == toggle) { clear(); return Toggle; }
        if (b.action == menu) { clear(); return Menu; }
        fingers_.push_back({id, n, x, y});
        break;
    }
    return None;
}
void TouchControls::motion(int64_t id, float x, float y) {
    for (auto &f : fingers_) if (f.id == id) { f.x = x; f.y = y; }
}
void TouchControls::up(int64_t id) {
    std::erase_if(fingers_, [id](const Finger &f) { return f.id == id; });
}
void TouchControls::clear() { fingers_.clear(); }
void TouchControls::apply(Controls &c) const {
    std::fill(std::begin(c.touch), std::end(c.touch), 0.0f);
    for (const auto &f : fingers_) {
        const auto &b = buttons[f.control];
        if (b.action == stick) {
            float value = std::clamp((f.x - (b.x + b.w / 2)) / (b.w / 2), -1.f, 1.f);
            if (std::abs(value) < 0.08f) value = 0;
            c.touch[SteerLeft] = std::max(c.touch[SteerLeft], -value);
            c.touch[SteerRight] = std::max(c.touch[SteerRight], value);
        } else if (inside(b, f.x, f.y)) c.touch[b.action] = 1.f;
    }
}
void TouchControls::draw(float ox, float oy, float width, float height, bool visible) const {
    auto *d = ImGui::GetForegroundDrawList();
    const float font = std::clamp(height * 0.035f, 14.f, 36.f);
    for (const auto &b : buttons) {
        if (!visible && b.action >= -1) continue;
        const ImVec2 a(ox + b.x * width, oy + b.y * height);
        const ImVec2 z(a.x + b.w * width, a.y + b.h * height);
        bool pressed = false;
        for (const auto &f : fingers_) if (&buttons[f.control] == &b && inside(b, f.x, f.y)) pressed = true;
        d->AddRectFilled(a, z, pressed ? IM_COL32(30, 125, 190, 180) : IM_COL32(12, 20, 32, 135), 12.f);
        d->AddRect(a, z, IM_COL32(230, 240, 255, 170), 12.f, 0, 2.f);
        const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(font, 10000.f, 0.f, b.label);
        d->AddText(ImGui::GetFont(), font, ImVec2((a.x + z.x - size.x) / 2, (a.y + z.y - size.y) / 2), IM_COL32_WHITE, b.label);
        if (b.action == stick) {
            float x = (a.x + z.x) / 2;
            for (const auto &f : fingers_) if (&buttons[f.control] == &b) x = ox + std::clamp(f.x, b.x, b.x + b.w) * width;
            d->AddCircleFilled(ImVec2(x, a.y + 0.78f * (z.y - a.y)), height * 0.025f, IM_COL32(160, 210, 255, 200));
        }
    }
}
}
