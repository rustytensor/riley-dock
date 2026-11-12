// SPDX-License-Identifier: GPL-3.0-or-later
//
// libriley-dock: motion. Mirrors riley dock/src/motion.rs.
//
// Pure dock geometry and motion. Distances are surface-local logical pixels; times are seconds. All doubles are
// IEEE binary64 and the formulas below must be evaluated in the order written (the tests compare many results
// exactly, as the Rust tests do with assert_eq!).
#pragma once

#include <QtCore/QDebug>
#include <QtCore/QList>

#include <array>
#include <optional>
#include <utility>

namespace riley::dock::motion {

// riley dock/src/motion.rs:5 SETTLE_SECONDS, riley dock/src/motion.rs:6 SQUASH_SECONDS.
inline constexpr double SETTLE_SECONDS = 0.350;
inline constexpr double SQUASH_SECONDS = 0.090;

// Rust (f64, f64): a point or a (position, alpha) gradient stop. Compared exactly.
using Point = std::pair<double, double>;

// riley dock/src/motion.rs:9 swell_scale(distance). d = |distance| in rest icon widths; d >= 2.5 -> 1.0, else
// 1 + 0.3*(1 + cos(pi*d/2.5)).
double swellScale(double distance);

// riley dock/src/motion.rs:19 pointer_scales(rest_centres, pointer_x, rest_width). For each rest centre x: no
// pointer -> 1.0, else swellScale((pointer - x) / restWidth). Precondition (Rust assert!): restWidth finite and > 0.
QList<double> pointerScales(const QList<double> &restCentres, std::optional<double> pointerX, double restWidth);

// riley dock/src/motion.rs:35 struct InputRect { x, y, width, height: i32 }. A surface input-region rectangle.
struct InputRect {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    bool operator==(const InputRect &) const = default;
};

// riley dock/src/motion.rs:27 struct Rect { x, y, width, height: f64 }. Compared exactly.
struct Rect {
    double x = 0;
    double y = 0;
    double width = 0;
    double height = 0;
    bool operator==(const Rect &) const = default;

    // riley dock/src/motion.rs:44 Rect::outward. left = floor(x), top = floor(y), width = ceil(x+width) - left,
    // height = ceil(y+height) - top: fractional edge pixels stay clickable.
    InputRect outward() const;
};

// riley dock/src/motion.rs:68 struct Layout { bar, icons }.
struct Layout {
    Rect bar;
    QList<Rect> icons;
    bool operator==(const Layout &) const = default;

    // riley dock/src/motion.rs:109 Layout::input_region. First bar.outward(); then for each icon r in order: if r
    // does not overlap the bar (left >= cutRight || top >= cutBottom, with left = max(r.x, bar.x),
    // top = max(r.y, bar.y), cutRight = min(r.x+r.width, bar.x+bar.width),
    // cutBottom = min(r.y+r.height, bar.y+bar.height)) push r.outward(); otherwise push, in this order and only when
    // width > 0 and height > 0, the outward() of the parts outside the bar: above {r.x, r.y, r.width, top-r.y},
    // below {r.x, cutBottom, r.width, bottom-cutBottom}, left {r.x, top, left-r.x, cutBottom-top},
    // right {cutRight, top, right-cutRight, cutBottom-top}. Shadows never take input.
    QList<InputRect> inputRegion() const;
};

// riley dock/src/motion.rs:57 struct LayoutConfig.
struct LayoutConfig {
    double surfaceWidth = 0; // surface_width
    double restWidth = 0;    // rest_width: icon size at scale 1
    double gap = 0;          // gap between icons
    double sidePadding = 0;  // side_padding inside the bar
    double barY = 0;         // bar_y: top of the painted bar
    double barHeight = 0;    // bar_height
    double restBottom = 0;   // rest_bottom: icon bottom at scale 1 (the reflection baseline)

    // riley dock/src/motion.rs:75 LayoutConfig::layout(scales) = pack(scales, no hover) (riley
    // dock/src/motion.rs:84). Scales positive and finite.
    //   content = sum(scales)*restWidth + max(n-1, 0)*gap; width = content + 2*sidePadding;
    //   bar = {(surfaceWidth - width)/2, barY, width, barHeight};
    //   x starts at bar.x + sidePadding; icon i: size = restWidth*scale, rise = 0.5*restWidth*(scale-1),
    //   rect = {x, restBottom - rise - size, size, size}; then x += size + gap.
    Layout layout(const QList<double> &scales) const;

    // riley dock/src/motion.rs:80 LayoutConfig::no_swell(count, hovered, hover_rise). pack(count ones, hover) where
    // the hovered icon's rise is hoverRise pixels instead of the scale rise (0 at scale 1). count >= 0.
    Layout noSwell(qsizetype count, std::optional<qsizetype> hovered, double hoverRise) const;
};

// riley dock/src/motion.rs:139 struct Reflection { image, visible, flip_y, stops }.
struct Reflection {
    Rect image;
    Rect visible;
    double flipY = 0;
    std::array<Point, 2> stops{};
    bool operator==(const Reflection &) const = default;
};

// riley dock/src/motion.rs:146 reflection(icon, baseline). rise = baseline - (icon.y + icon.height);
// top = baseline + rise; image = {icon.x, top, icon.width, icon.height}; visible = image with
// height = icon.height*0.45; flipY = top + icon.height; stops = {(0.0, 0.55), (1.0, 0.0)}.
Reflection reflection(Rect icon, double baseline);

// riley dock/src/motion.rs:154 reflection_padding(rest_width, enabled). enabled ? ceil(restWidth*0.45) : 0.
double reflectionPadding(double restWidth, bool enabled);

// riley dock/src/motion.rs:158 reflection_layout(config, enabled). config with
// barHeight += reflectionPadding(restWidth, enabled).
LayoutConfig reflectionLayout(LayoutConfig config, bool enabled);

// riley dock/src/motion.rs:163 shelf(layout, baseline, side_padding).
// {bar.x + sidePadding, baseline, max(bar.width - 2*sidePadding, 0), 1}.
Rect shelf(const Layout &layout, double baseline, double sidePadding);

// riley dock/src/motion.rs:169 struct Sample { value, velocity, active }.
struct Sample {
    double value = 0;
    double velocity = 0; // units per second, for continuous retargeting
    bool active = false; // a frame callback is needed only while this is true
    bool operator==(const Sample &) const = default;
};

// riley dock/src/motion.rs:179 struct Spring { initial, velocity, target }. Spring::new(initial, velocity, target)
// (riley dock/src/motion.rs:186) is aggregate initialisation here. Retarget by sampling the old spring and passing
// its value and velocity to a new one.
struct Spring {
    double initial = 0;
    double velocity = 0;
    double target = 0;

    // riley dock/src/motion.rs:191 Spring::raw. Unsnapped analytic response, t = max(seconds, 0):
    //   a = ln(7.5)/0.14, w = pi/0.14, c = initial - target, d = (velocity + a*c)/w,
    //   decay = exp(-a*t), offset = c*cos(w*t) + d*sin(w*t);
    //   value = target + decay*offset, velocity = decay*((d*w*cos - c*w*sin) - a*offset),
    //   active = c != 0 || velocity != 0.
    Sample raw(double seconds) const;

    // riley dock/src/motion.rs:207 Spring::sample. seconds >= SETTLE_SECONDS -> {target, 0, false}, else raw(seconds).
    Sample sample(double seconds) const;
};

// riley dock/src/motion.rs:217 click_squash(seconds). Multiplies the current swell. t = max(seconds, 0):
//   t <= SQUASH_SECONDS: phase = pi*t/SQUASH_SECONDS, value = 1 - 0.1*(1 - cos(phase))/2,
//                        velocity = -0.05*pi/SQUASH_SECONDS*sin(phase), active = true;
//   otherwise Spring{0.9, 0, 1}.sample(t - SQUASH_SECONDS).
Sample clickSquash(double seconds);

// Rust #[derive(Debug)]: diagnostic text for test failure messages. Any readable form.
QDebug operator<<(QDebug debug, const InputRect &rect);
QDebug operator<<(QDebug debug, const Rect &rect);
QDebug operator<<(QDebug debug, const Layout &layout);
QDebug operator<<(QDebug debug, const Reflection &reflection);
QDebug operator<<(QDebug debug, const Sample &sample);

} // namespace riley::dock::motion
