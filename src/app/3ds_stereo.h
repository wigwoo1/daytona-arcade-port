#pragma once
#include "runtime/geo.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace app {
// Daytona draws some HUD marks as untextured, constant-depth polygons at
// approximately 0.00009. Keep this narrow class at screen depth; ordinary
// world geometry and the much deeper car-selection floor remain untouched.
inline bool three_ds_screen_polygon(const rt::GeoPoly &poly) {
    if (poly.num_vertices < 3 || poly.texheader[0] || poly.texheader[1] || poly.texheader[2]) return false;
    const float depth = poly.v[0].p[0];
    if (!std::isfinite(depth) || depth <= 0 || depth > 0.001f) return false;
    for (unsigned i=1; i<poly.num_vertices; ++i) {
        const float z=poly.v[i].p[0];
        if (!std::isfinite(z) || z <= 0 || std::abs(z-depth) > 0.0000001f) return false;
    }
    return true;
}
inline float three_ds_world_eye_offset(const std::vector<rt::GeoPoly> &polys,
                                       float strength, float focus, float max_disparity) {
    if (strength <= 0 || max_disparity <= 0 || !std::isfinite(focus) || focus <= 0) return 0;
    float nearest=std::numeric_limits<float>::infinity(), farthest=0;
    for (const auto &poly:polys) {
        if (three_ds_screen_polygon(poly)) continue;
        for (unsigned i=0;i<poly.num_vertices;++i) {
            const float z=poly.v[i].p[0];
            if (std::isfinite(z) && z > 0) {nearest=std::min(nearest,z);farthest=std::max(farthest,z);}
        }
    }
    if (!farthest) return 0;
    // Inverse depth is monotonic: the maximum deviation occurs at an endpoint.
    const double inverse_focus=1.0/focus;
    const double deviation=std::max(std::abs(1.0/nearest-inverse_focus),std::abs(1.0/farthest-inverse_focus));
    if (!deviation) return 0;
    return float(std::clamp(double(strength),0.0,1.0)*std::min(double(focus)*4,double(max_disparity)/deviation));
}
}