#include "runtime/video.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>

namespace rt {
namespace {
bool compatible(const GeoPoly &a, const GeoPoly &b) {
    if (a.num_vertices != b.num_vertices || a.window != b.window || a.reverse != b.reverse ||
        !std::equal(std::begin(a.texheader), std::end(a.texheader), b.texheader) ||
        !std::equal(std::begin(a.viewport), std::end(a.viewport), b.viewport) ||
        !std::equal(std::begin(a.center), std::end(a.center), b.center)) return false;
    for (int i = 0; i < a.num_vertices; ++i) {
        const auto &u = a.v[i]; const auto &v = b.v[i];
        // Changed UVs identify clipped edges or a different surface. Abrupt
        // projected movement identifies cuts; don't smear across either.
        if (!(u.p[0] > 0 && v.p[0] > 0) ||
            std::abs(u.p[1] - v.p[1]) > 0.01f || std::abs(u.p[2] - v.p[2]) > 0.01f ||
            !std::isfinite(u.x / u.p[0]) || !std::isfinite(v.x / v.p[0]) ||
            !std::isfinite(u.y / u.p[0]) || !std::isfinite(v.y / v.p[0]) ||
            std::abs(u.x / u.p[0] - v.x / v.p[0]) > 32 ||
            std::abs(u.y / u.p[0] - v.y / v.p[0]) > 32 ||
            v.p[0] / u.p[0] < 0.75f || v.p[0] / u.p[0] > 1.3334f) return false;
    }
    return true;
}
}
void Video::set_render_scale(float scale) {
    if (!std::isfinite(scale) || scale <= 0 || scale > 1)
        throw std::invalid_argument("render scale outside 0..1");
    if (render_scale_ != scale) { render_scale_ = scale; invalidate_presentation(); }
}
void Video::set_draw_distance(float depth) {
    if (!std::isfinite(depth) || depth < 0) throw std::invalid_argument("invalid draw distance");
    if (draw_distance_ != depth) { draw_distance_ = depth; invalidate_presentation(); }
}
float Video::stereo_eye_offset(float strength, float convergence, float max_disparity) const {
    if (!std::isfinite(strength) || !std::isfinite(convergence) || convergence <= 0 ||
        !std::isfinite(max_disparity) || max_disparity < 0)
        throw std::invalid_argument("invalid stereo depth parameters");
    // At zero slider strength the clamped result is always zero; avoid walking
    // every polygon vertex just to multiply the computed limit by zero.
    if (strength <= 0 || max_disparity == 0) return 0;
    const double focus_inverse = 1.0 / convergence;
    double max_inverse = 0;
    for (const auto &poly : current_polys_)
        for (int i = 0; i < poly.num_vertices; ++i) {
            const float depth = poly.v[i].p[0];
            if (std::isfinite(depth) && depth > 0)
                max_inverse = std::max(max_inverse, std::abs(1.0 / depth - focus_inverse));
        }
    if (max_inverse == 0) return 0;
    return float(std::clamp(double(strength), 0.0, 1.0) *
                 std::min(double(convergence) * 4, double(max_disparity) / max_inverse));
}

float Video::stereo_reference_depth() const {
    std::vector<float> depths;
    for (const auto &poly : current_polys_)
        for (int i = 0; i < poly.num_vertices; ++i)
            if (std::isfinite(poly.v[i].p[0]) && poly.v[i].p[0] > 0) depths.push_back(poly.v[i].p[0]);
    if (depths.empty()) return 0;
    auto mid = depths.begin() + depths.size() / 2;
    std::nth_element(depths.begin(), mid, depths.end());
    return *mid;
}
const std::vector<uint32_t> &Video::present(int width, int height, float alpha, bool interpolate, float eye_offset, float convergence) {
    using Clock = std::chrono::steady_clock;
    const bool profile = profile_presentation_;
    const auto profile_start = profile ? Clock::now() : Clock::time_point{};
    if (width <= 0 || height <= 0 || width > 3840 || height > 2160)
        throw std::invalid_argument("presentation target outside 1..3840 x 1..2160");
        presentation_raster_.set_stereo(eye_offset, convergence);
    if (!render_workers_) render_workers_ = std::make_unique<RenderWorkers>(render_thread_limit_);
    if (interpolate && matches_revision_ != presentation_revision_) {
        interpolation_matches_.clear();
        if (previous_polys_.size() == current_polys_.size()) {
            for (size_t n = 0; n < current_polys_.size(); ++n) {
                const auto &a = previous_polys_[n]; const auto &b = current_polys_[n];
                // The list has no persistent surface identities. Moving only
                // some faces opens seams against their unmatched neighbours.
                // Hold the whole native frame across clipping/topology changes.
                if (!compatible(a, b)) { interpolation_matches_.clear(); break; }
                bool moved = false;
                for (int i = 0; i < a.num_vertices; ++i)
                    moved |= a.v[i].x != b.v[i].x || a.v[i].y != b.v[i].y || a.v[i].p[0] != b.v[i].p[0];
                if (moved) interpolation_matches_.push_back(n);
            }
        }
        matches_revision_ = presentation_revision_;
    }
    const bool moving = interpolate && !interpolation_matches_.empty();
    interpolated_polygons_ = moving ? interpolation_matches_.size() : 0;
    alpha = moving ? std::clamp(alpha, 0.0f, 1.0f) : 1.0f;
    if (cached_revision_ == presentation_revision_ && cached_width_ == width &&
        cached_height_ == height && cached_alpha_ == alpha &&
        cached_eye_offset_ == eye_offset && cached_convergence_ == convergence) {
        if (profile) {
            const auto end = Clock::now();
            presentation_times_ = {};
            presentation_times_.prepare_ms = std::chrono::duration<double, std::milli>(end - profile_start).count();
            presentation_times_.total_ms = presentation_times_.prepare_ms;
            presentation_times_.polygons = current_polys_.size();
            presentation_times_.cache_hit = true;
        }
        return presentation_;
    }
    const int render_width = std::max(1, int(width * render_scale_));
    const int render_height = std::max(1, int(height * render_scale_));
    presentation_raster_.set_target(render_width, render_height, widescreen_);
    presentation_.resize(size_t(width) * height);
    const bool sampled_layers = sampled_layer_output_ && presentation_only_ &&
        back_layer_.size() == size_t(sampled_layer_width_) * sampled_layer_height_ &&
        front_layer_.size() == size_t(sampled_layer_width_) * sampled_layer_height_;
    if (cached_width_ != width || cached_widescreen_ != widescreen_) {
        source_x_.resize(width); hud_source_x_.resize(width);
        for (int x = 0; x < width; ++x) {
            hud_source_x_[x] = widescreen_ ? int(std::floor((double(x) - double(width) / 8.0) * W / (double(width) * 0.75))) : x * W / width;
        }
    }
    source_x_.resize(width);
    for (int x = 0; x < width; ++x)
        source_x_[x] = sampled_layers ? x * sampled_layer_width_ / width : x * W / width;
    if (cached_width_ != width || geometry_source_width_ != render_width) {
        geometry_source_x_.resize(width);
        for (int x = 0; x < width; ++x) geometry_source_x_[x] = x * render_width / width;
        geometry_source_width_ = render_width;
    }
    const auto prepare_end = profile ? Clock::now() : Clock::time_point{};
    Clock::time_point raster_begin{}, raster_end{};
    if (back_layer_.empty()) {
        std::fill(presentation_.begin(), presentation_.end(), 0xff000000u);
    } else {
        const std::vector<GeoPoly> *polys = &current_polys_;
        if (moving) {
            blended_polys_ = current_polys_;
            for (size_t n : interpolation_matches_) {
                const auto &a = previous_polys_[n]; const auto &b = current_polys_[n];
                auto &out = blended_polys_[n];
                for (int i = 0; i < out.num_vertices; ++i) {
                    out.v[i].x = std::lerp(a.v[i].x, b.v[i].x, alpha);
                    out.v[i].y = std::lerp(a.v[i].y, b.v[i].y, alpha);
                    for (int j = 0; j < 3; ++j) out.v[i].p[j] = std::lerp(a.v[i].p[j], b.v[i].p[j], alpha);
                }
            }
            polys = &blended_polys_;
        }
        if (draw_distance_ > 0) {
            distance_polys_.clear();
            for (const auto &poly : *polys) {
                bool visible = false;
                for (int i = 0; i < poly.num_vertices; ++i)
                    visible |= !std::isfinite(poly.v[i].p[0]) || poly.v[i].p[0] <= draw_distance_;
                if (visible) distance_polys_.push_back(poly);
            }
            polys = &distance_polys_;
        }
        if (profile) raster_begin = Clock::now();
        presentation_raster_.render(*polys, presentation_windows_, presentation_mem_,
            presentation_crtc_x_, presentation_crtc_y_, presentation_render_x_, presentation_render_y_,
            widescreen_ ? -83 : 0, widescreen_ ? W + 82 : W - 1, 0, H - 1, render_workers_.get());
        if (profile) raster_end = Clock::now();
        const auto *three_d = presentation_raster_.pixels();
        const int bands = std::min(height, int(render_workers_->size() * 4));
        render_workers_->run(size_t(bands), [&](size_t band) {
            const int y0 = int(band) * height / bands;
            const int y1 = (int(band) + 1) * height / bands;
            if (sampled_layers) {
                for (int y = y0; y < y1; ++y) {
                    const size_t sampled_row = size_t(y) * sampled_layer_height_ / height * sampled_layer_width_;
                    const size_t row = size_t(y) * width;
                    const size_t geometry_row = size_t(y * render_height / height) * render_width;
                    for (int x = 0; x < width; ++x) {
                        const size_t sampled_src = sampled_row + source_x_[x];
                        const uint32_t front = front_layer_[sampled_src];
                        const uint32_t geometry = three_d[geometry_row + geometry_source_x_[x]];
                        presentation_[row + x] = front ? front : geometry ? geometry : back_layer_[sampled_src];
                    }
                }
            } else {
                for (int y = y0; y < y1; ++y) {
                    const size_t sy = size_t(y * H / height) * W;
                    const size_t row = size_t(y) * width;
                    const size_t geometry_row = size_t(y * render_height / height) * render_width;
                    for (int x = 0; x < width; ++x) {
                        const int hud_x = hud_source_x_[x];
                        const uint32_t front = hud_x >= 0 && hud_x < W ? front_layer_[sy + hud_x] : 0;
                        const uint32_t geometry = three_d[geometry_row + geometry_source_x_[x]];
                        presentation_[row + x] = front ? front : geometry ? geometry : back_layer_[sy + source_x_[x]];
                    }
                }
            }
        });
    }
    cached_revision_ = presentation_revision_; cached_width_ = width; cached_height_ = height; cached_alpha_ = alpha;
    cached_widescreen_ = widescreen_; cached_eye_offset_ = eye_offset; cached_convergence_ = convergence;
    ++presentation_generation_;
    if (profile) {
        const auto end = Clock::now();
        presentation_times_.prepare_ms = std::chrono::duration<double, std::milli>(prepare_end - profile_start).count();
        if (raster_begin != Clock::time_point{})
            presentation_times_.raster_ms = std::chrono::duration<double, std::milli>(raster_end - raster_begin).count();
        const auto compose_begin = raster_end != Clock::time_point{} ? raster_end : prepare_end;
        presentation_times_.compose_ms = std::chrono::duration<double, std::milli>(end - compose_begin).count();
        presentation_times_.total_ms = std::chrono::duration<double, std::milli>(end - profile_start).count();
        presentation_times_.polygons = draw_distance_ > 0 ? distance_polys_.size() : current_polys_.size();
        presentation_times_.cache_hit = false;
    }
    return presentation_;
}

std::pair<const std::vector<uint32_t> &, const std::vector<uint32_t> &> Video::present_stereo(
    int width, int height, float alpha, bool interpolate, float eye_offset, float convergence) {
    auto sequential = [&]() -> std::pair<const std::vector<uint32_t> &, const std::vector<uint32_t> &> {
        const auto &left = present(width, height, alpha, interpolate, -eye_offset, convergence);
        right_eye_presentation_ = left;
        present(width, height, alpha, interpolate, eye_offset, convergence);
        return {right_eye_presentation_, presentation_};
    };
    if (!parallel_stereo_eyes_ || !render_workers_ || render_workers_->size() < 2)
        return sequential();
    if (width <= 0 || height <= 0 || width > 3840 || height > 2160)
        throw std::invalid_argument("presentation target outside 1..3840 x 1..2160");
    if (interpolate) {
        // Keep the normal interpolation and cache policy for this uncommon mode.
        return sequential();
    }
    const auto pair_start = std::chrono::steady_clock::now();
    if (back_layer_.empty()) {
        presentation_.resize(size_t(width) * height);
        right_eye_presentation_.resize(size_t(width) * height);
        std::fill(presentation_.begin(), presentation_.end(), 0xff000000u);
        std::fill(right_eye_presentation_.begin(), right_eye_presentation_.end(), 0xff000000u);
        return {presentation_, right_eye_presentation_};
    }
    if (!render_workers_) render_workers_ = std::make_unique<RenderWorkers>(render_thread_limit_);
    const int render_width = std::max(1, int(width * render_scale_));
    const int render_height = std::max(1, int(height * render_scale_));
    presentation_raster_.set_target(render_width, render_height, widescreen_);
    right_eye_raster_.set_target(render_width, render_height, widescreen_);
    presentation_raster_.set_stereo(-eye_offset, convergence);
    right_eye_raster_.set_stereo(eye_offset, convergence);
    right_eye_raster_.set_texture_quality(texture_quality_);
    right_eye_raster_.set_nearest_textures(nearest_textures_);
    right_eye_raster_.set_depth_buffer(depth_buffer_);
    presentation_.resize(size_t(width) * height);
    right_eye_presentation_.resize(size_t(width) * height);
    const bool sampled_layers = sampled_layer_output_ && presentation_only_ &&
        back_layer_.size() == size_t(sampled_layer_width_) * sampled_layer_height_ &&
        front_layer_.size() == size_t(sampled_layer_width_) * sampled_layer_height_;

    // The mapping is identical on every output row. Compute it once rather
    // than performing ARM software integer division for each stereo pixel.
    std::vector<size_t> layer_x(width), geometry_x(width);
    std::vector<int> hud_x(width);
    for (int x = 0; x < width; ++x) {
        geometry_x[x] = size_t(x * render_width / width);
        if (sampled_layers) {
            layer_x[x] = size_t(x) * sampled_layer_width_ / width;
        } else {
            hud_x[x] = widescreen_ ? int(std::floor((double(x) - double(width) / 8.0) * W / (double(width) * 0.75))) : x * W / width;
            layer_x[x] = widescreen_ ? size_t(std::clamp(int((double(x) - double(width) / 8.0) * W / (double(width) * 0.75)), 0, W - 1)) : size_t(x * W / width);
        }
    }

    const std::vector<GeoPoly> *polys = &current_polys_;
    if (draw_distance_ > 0) {
        distance_polys_.clear();
        for (const auto &poly : *polys) {
            bool visible = false;
            for (int i = 0; i < poly.num_vertices; ++i)
                visible |= !std::isfinite(poly.v[i].p[0]) || poly.v[i].p[0] <= draw_distance_;
            if (visible) distance_polys_.push_back(poly);
        }
        polys = &distance_polys_;
    }
    std::array<double, 2> eye_raster_ms{};
    presentation_raster_.render_stereo_pair(right_eye_raster_, *polys, presentation_windows_, presentation_mem_,
        presentation_crtc_x_, presentation_crtc_y_, presentation_render_x_, presentation_render_y_,
        widescreen_ ? -83 : 0, widescreen_ ? W + 82 : W - 1, 0, H - 1,
        render_workers_.get(), profile_presentation_ ? &eye_raster_ms : nullptr);
    const auto compose_start = std::chrono::steady_clock::now();

    const auto *left_pixels = presentation_raster_.pixels();
    const auto *right_pixels = right_eye_raster_.pixels();
    const int bands = std::min(height, int(render_workers_->size() * 4));
    render_workers_->run(size_t(bands), [&](size_t band) {
        const int y0 = int(band) * height / bands;
        const int y1 = (int(band) + 1) * height / bands;
        for (int y = y0; y < y1; ++y) {
            const size_t row = size_t(y) * width;
            const size_t geometry_row = size_t(y * render_height / height) * render_width;
            if (sampled_layers) {
                const size_t layer_row = size_t(y) * sampled_layer_height_ / height * sampled_layer_width_;
                for (int x = 0; x < width; ++x) {
                    const size_t layer = layer_row + layer_x[x];
                    const size_t geometry = geometry_row + geometry_x[x];
                    const uint32_t front = front_layer_[layer];
                    const uint32_t left = left_pixels[geometry], right = right_pixels[geometry];
                    const uint32_t back = back_layer_[layer];
                    presentation_[row + x] = front ? front : left ? left : back;
                    right_eye_presentation_[row + x] = front ? front : right ? right : back;
                }
            } else {
                const size_t sy = size_t(y * H / height) * W;
                for (int x = 0; x < width; ++x) {
                    const int hud_source = hud_x[x];
                    const uint32_t front = hud_source >= 0 && hud_source < W ? front_layer_[sy + hud_source] : 0;
                    const size_t geometry = geometry_row + geometry_x[x];
                    const uint32_t left = left_pixels[geometry], right = right_pixels[geometry];
                    const uint32_t back = back_layer_[sy + layer_x[x]];
                    presentation_[row + x] = front ? front : left ? left : back;
                    right_eye_presentation_[row + x] = front ? front : right ? right : back;
                }
            }
        }
    });
    cached_revision_ = UINT64_MAX;
    if (profile_presentation_) {
        presentation_times_ = {};
        right_eye_presentation_times_ = {};
        presentation_times_.polygons = polys->size();
        right_eye_presentation_times_.polygons = polys->size();
        presentation_times_.raster_ms = eye_raster_ms[0];
        right_eye_presentation_times_.raster_ms = eye_raster_ms[1];
        presentation_times_.compose_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compose_start).count();
        presentation_times_.total_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - pair_start).count();
    }
    return {presentation_, right_eye_presentation_};
}
} // namespace rt
