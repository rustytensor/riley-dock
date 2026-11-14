// SPDX-License-Identifier: GPL-3.0-or-later
//
// libriley-dock: grid. Mirrors riley dock/src/grid.rs.
//
// The launcher ("app grid") button is a fixed-size control, not an application icon: it never swells, sits
// vertically centred in the bar, and has its own nine-dot look and press animation.
#pragma once

#include <riley-dock/motion.h>
#include <riley-dock/rules.h>

#include <QtCore/QDebug>
#include <QtCore/QList>
#include <QtCore/QString>

#include <array>
#include <optional>
#include <utility>

namespace riley::dock::grid {

// riley dock/src/grid.rs:5 enum Position { Left, Right, Center, Off }.
enum class Position { Left, Right, Center, Off };

// riley dock/src/grid.rs:7 position(config). Value of the last line whose trimmed key is "LAUNCHER", trimmed:
// "right" -> Right, "center" -> Center, "off" -> Off, anything else or missing -> Left.
Position position(const QString &config);

// riley dock/src/grid.rs:18 place_items(items, position). Removes the first item with id "launcher" (if any);
// unless Off, inserts it back (or a new {id "launcher", no windows}) at: Left 0, Right n, Center ceil(n/2), with
// n = remaining count. Off leaves no launcher item.
QList<rules::DockItem> placeItems(QList<rules::DockItem> items, Position position);

// riley dock/src/grid.rs:28 app_count(items). Items whose id is not "launcher".
qsizetype appCount(const QList<rules::DockItem> &items);

// riley dock/src/grid.rs:32 layout(config, scales, grid). The grid slot (if grid is set and in range) gets scale
// 1.0 before packing (config.layout), and its icon rect's y becomes bar.y + (bar.height - rect.height)/2
// (vertically centred).
motion::Layout layout(const motion::LayoutConfig &config, const QList<double> &scales,
                      std::optional<qsizetype> grid);

// Swell targets with the grid button in the row. rest is the unswelled layout (grid::layout with all scales 1.0);
// the scales are motion::pointerScales of its icon centres, with the grid slot always 1.0. While the pointer's x
// lies on the grid button's rest rectangle (both edges included), no icon swells: every scale is 1.0.
QList<double> pointerScales(const motion::Layout &rest, std::optional<double> pointerX, double restWidth,
                            std::optional<qsizetype> grid);

// riley dock/src/grid.rs:42 reflection(rect). motion::reflection(rect, rect.y + rect.height): mirrored about its
// own bottom.
motion::Reflection reflection(motion::Rect rect);

// riley dock/src/grid.rs:47 struct Look { spacing, accent }; Default (riley dock/src/grid.rs:48) is {1.0, 0.0},
// the member initialisers here.
struct Look {
    double spacing = 1.0;
    double accent = 0.0;
    bool operator==(const Look &) const = default;
};

// riley dock/src/grid.rs:52 dots(rect, look) -> ([(f64, f64); 9], f64). size = min(width, height),
// step = size/4*spacing; dot i (row-major, i = 0..8): (x + width/2 + (i%3)*step - step,
// y + height/2 + (i/3)*step - step); radius = size/16.
std::pair<std::array<motion::Point, 9>, double> dots(motion::Rect rect, Look look);

// The hover look (Marc, 2026-11-13: "when the mouse cursor hovers over it, make the dots slightly spread apart
// without hitting the other icons - just make it look good and smooth"). The Rust dock spread to 1.2 in 0.15 s with a
// cubic ease-out, which starts at three times the mean speed and reads as a jump. Now 1.4 in 0.18 s with a cubic
// ease-in-out. The outermost dot edge lies size * (spacing/4 + 1/16) from the centre, so the dots stay inside the
// button's own square while spacing < 1.75: at 1.4, 0.4125 * size (44 px button: 18.15 px of 22).
inline constexpr double HOVER_SPACING = 1.4;
inline constexpr double HOVER_SECONDS = 0.18;
// The largest spacing whose dots stay inside the button: 4 * (1/2 - 1/16).
inline constexpr double SLOT_SPACING = 1.75;

// riley dock/src/grid.rs:60 grid::Sample { look, active, launch }.
struct Sample {
    Look look;
    bool active = false;
    bool launch = false;
};

// riley dock/src/grid.rs:63 struct Animation (#[derive(Default)]: all fields below at their initialisers). Times
// are seconds on the caller's clock.
class Animation {
public:
    // riley dock/src/grid.rs:96 set_hover(hovered, now). No-op if unchanged. Else evaluate(now); from = look;
    // began = now; set hovered; a running spring (start, s) is sampled at now - start and replaced by
    // (now, Spring{value, velocity, target().spacing}) with the NEW target.
    void setHover(bool hovered, double now);

    // riley dock/src/grid.rs:108 press(now). evaluate(now); pressed = true; spring and launch deadline cleared.
    void press(double now);

    // riley dock/src/grid.rs:115 is_pressed().
    bool isPressed() const;

    // riley dock/src/grid.rs:117 release(now, activate). pressed = false; spring = (now, Spring{0.6, 0,
    // target().spacing}); launchAt = activate ? now + motion::SETTLE_SECONDS : nullopt.
    void release(double now, bool activate);

    // riley dock/src/grid.rs:123 sample(now). (look, active) = evaluate(now); launch = launchAt set &&
    // now >= launchAt (then cleared: one launch per accepted click); returns {look, active || launchAt still set,
    // launch}.
    Sample sample(double now);

private:
    // riley dock/src/grid.rs:74 target(). hovered ? Look{HOVER_SPACING, 1.0} : Look{}.
    Look target() const;

    // riley dock/src/grid.rs:78 evaluate(now). t = clamp((now - began)/HOVER_SECONDS, 0, 1), q = 4t^3 for t < 1/2,
    // else 1 - (2 - 2t)^3/2 (cubic ease-in-out); look = from + (target - from)*q per field; active = t < 1 && from != target. Then pressed ->
    // spacing = 0.6; else a spring (start, s) gives spacing = s.sample(now - start).value, active |= sample.active,
    // and the spring is dropped once its sample is inactive. Stores look; returns (look, active).
    std::pair<Look, bool> evaluate(double now);

    bool m_hovered = false;                                    // hovered
    bool m_pressed = false;                                    // pressed
    Look m_from;                                               // from
    Look m_look;                                               // look
    double m_began = 0;                                        // began
    std::optional<std::pair<double, motion::Spring>> m_spring; // spring: (start time, spring)
    std::optional<double> m_launchAt;                          // launch_at
};

// Rust #[derive(Debug)]: diagnostic text for test failure messages.
QDebug operator<<(QDebug debug, const Look &look);

} // namespace riley::dock::grid
