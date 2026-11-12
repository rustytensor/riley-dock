// SPDX-License-Identifier: GPL-3.0-or-later
//
// libriley-dock: autohide. Mirrors riley dock/src/autohide.rs.
//
// Auto-hide: the lip geometry, slide motion and the dwell/hide timeline. Pure: times are seconds on a
// caller-supplied clock, distances are surface-local logical pixels, and positive offsets move the painted dock
// down inside its fixed surface.
#pragma once

#include <riley-dock/motion.h>
#include <riley-dock/rules.h>

#include <QtCore/QDebug>
#include <QtCore/QList>
#include <QtCore/QString>

#include <optional>
#include <utility>
#include <variant>

namespace riley::dock::autohide {

// riley dock/src/autohide.rs:9 LIP. Visible height of the hidden dock: the bar's rounded top and the running-app
// dots. The one definition of the hidden lip height; everything else derives from it.
inline constexpr double LIP = 6.0;
// riley dock/src/autohide.rs:11 TRIGGER. Bottom screen-edge strip that also reveals; it lies inside the lip.
inline constexpr double TRIGGER = 2.0;
// riley dock/src/autohide.rs:12 DOT_RADIUS.
inline constexpr double DOT_RADIUS = 1.0;
// riley dock/src/autohide.rs:13 DWELL_SECONDS.
inline constexpr double DWELL_SECONDS = 0.120;
// riley dock/src/autohide.rs:14 HIDE_DELAY_SECONDS.
inline constexpr double HIDE_DELAY_SECONDS = 0.700;
// riley dock/src/autohide.rs:15 HIDE_SECONDS.
inline constexpr double HIDE_SECONDS = 0.200;
// riley dock/src/autohide.rs:17 REVEAL_PEAK_SECONDS. Same first peak and settling deadline as the icon spring, with
// 8% overshoot of the slide travel.
inline constexpr double REVEAL_PEAK_SECONDS = 0.140;
// riley dock/src/autohide.rs:18 REVEAL_SECONDS.
inline constexpr double REVEAL_SECONDS = 0.350;

// riley dock/src/autohide.rs:20 reveal_damping(). ln(12.5) / REVEAL_PEAK_SECONDS.
double revealDamping();

// riley dock/src/autohide.rs:23 enabled(config). `AUTOHIDE=1|0`: the last line whose trimmed key is "AUTOHIDE"
// gives the trimmed value; true only when it is exactly "1". Missing or invalid is off.
bool enabled(const QString &config);

// riley dock/src/autohide.rs:34 dock_layout(size, count, apps, reflections) -> (LayoutConfig, i32). The dock's
// layout for an icon size and slot count, and its fixed surface height (bar plus the 8 px gap):
//   rest = max(rules::drawnSize(size, apps), 1) * 0.8; barHeight = rest + 23;
//   baseHeight = int(ceil(rest*2.2 + 36));
//   width = ceil(count*rest*1.8 + max(count-1, 0)*8 + 20)  (fixed transparent surface; only the bar changes width);
//   config = motion::reflectionLayout({width, rest, gap 8, sidePadding 10, baseHeight - barHeight, barHeight,
//            baseHeight - 17}, reflections);
//   height = baseHeight + int(motion::reflectionPadding(rest, reflections)) + 8.
std::pair<motion::LayoutConfig, int> dockLayout(quint32 size, qsizetype count, qsizetype apps, bool reflections);

// riley dock/src/autohide.rs:48 hidden_offset(surface_height, bar_y). surfaceHeight - LIP - barY: how far the
// painted dock slides so that exactly the lip stays on screen.
double hiddenOffset(double surfaceHeight, double barY);

// riley dock/src/autohide.rs:53 exclusive_zone(autohide, bar_height). autohide ? int(ceil(LIP)) :
// int(ceil(barHeight)) + 8. The lip is reserved in every pose when auto-hide is on.
int exclusiveZone(bool autohide, double barHeight);

// riley dock/src/autohide.rs:58 lip_region(bar, surface_height). Rect{bar.x, surfaceHeight - LIP, bar.width,
// LIP}.outward(): the activation band under the rest bar's horizontal span, at the bottom of the surface.
motion::InputRect lipRegion(motion::Rect bar, double surfaceHeight);

// riley dock/src/autohide.rs:62 trigger_strip(bar, surface_height). Rect{bar.x, surfaceHeight - TRIGGER, bar.width,
// TRIGGER}.outward().
motion::InputRect triggerStrip(motion::Rect bar, double surfaceHeight);

// riley dock/src/autohide.rs:68 running(items). Per item: id != "launcher" && it has windows. Minimized windows
// count, several windows still give one; pinned-only items and the app grid give none.
QList<bool> running(const QList<rules::DockItem> &items);

// riley dock/src/autohide.rs:73 lip_dots(rest, running, surface_height). For each rest icon r paired with
// running[i] (stopping at the shorter list) that is true: (r.x + r.width/2, surfaceHeight - LIP/2).
QList<motion::Point> lipDots(const motion::Layout &rest, const QList<bool> &running, double surfaceHeight);

// riley dock/src/autohide.rs:78 clip(rect, width, height). The part of rect inside [0, width) x [0, height), or
// nullopt when empty.
std::optional<motion::InputRect> clip(motion::InputRect rect, int width, int height);

// riley dock/src/autohide.rs:84 translate(layout, offset). Bar and every icon with y += offset.
motion::Layout translate(const motion::Layout &layout, double offset);

// riley dock/src/autohide.rs:90 hide_sample(from, distance, seconds) -> (value, velocity). Quadratic ease-in from
// `from` to the hidden distance over HIDE_SECONDS: t = clamp(seconds, 0, HIDE_SECONDS)/HIDE_SECONDS,
// span = distance - from; value = from + span*t*t; velocity = seconds < HIDE_SECONDS ? 2*span*t/HIDE_SECONDS : 0.
std::pair<double, double> hideSample(double from, double distance, double seconds);

// riley dock/src/autohide.rs:97 reveal_raw(from, velocity, seconds) -> (value, velocity). Unsnapped damped spring
// towards offset 0. t = max(seconds, 0), a = revealDamping(), w = pi/REVEAL_PEAK_SECONDS, d = (velocity + a*from)/w,
// decay = exp(-a*t), offset = from*cos(w*t) + d*sin(w*t);
// value = decay*offset, velocity = decay*((d*w*cos - from*w*sin) - a*offset).
std::pair<double, double> revealRaw(double from, double velocity, double seconds);

// riley dock/src/autohide.rs:109 enum Zone { Outside, Band, Dock }.
enum class Zone { Outside, Band, Dock };

// riley dock/src/autohide.rs:112 enum Pose { Hidden, Revealing { start, from, velocity }, Shown,
// Hiding { start, from } }. Compared exactly (Rust derive(PartialEq)).
namespace pose {
struct Hidden { bool operator==(const Hidden &) const = default; };
struct Revealing {
    double start = 0;
    double from = 0;
    double velocity = 0;
    bool operator==(const Revealing &) const = default;
};
struct Shown { bool operator==(const Shown &) const = default; };
struct Hiding {
    double start = 0;
    double from = 0;
    bool operator==(const Hiding &) const = default;
};
} // namespace pose
using Pose = std::variant<pose::Hidden, pose::Revealing, pose::Shown, pose::Hiding>;

// riley dock/src/autohide.rs:122 struct Controller. One dock's auto-hide state. Waiting is done by the caller's
// one-shot timer at nextDeadline(), never by frame polling; frames are needed only while animating().
class Controller {
public:
    // riley dock/src/autohide.rs:136 Controller::new(enabled, distance). Startup with auto-hide on is Hidden,
    // otherwise Shown; zone Outside, not blocked, no timers, band not held, no hide after reveal.
    Controller(bool enabled, double distance);

    // riley dock/src/autohide.rs:123 pub distance: the hidden offset (hiddenOffset).
    double distance = 0;

    bool enabled() const;       // riley dock/src/autohide.rs:141
    Pose pose() const;          // riley dock/src/autohide.rs:142
    bool settledHidden() const; // riley dock/src/autohide.rs:143 pose == Hidden
    bool settledShown() const;  // riley dock/src/autohide.rs:144 pose == Shown
    bool animating() const;     // riley dock/src/autohide.rs:145 pose is Revealing or Hiding

    // riley dock/src/autohide.rs:146 next_deadline(). The earlier of the dwell and hide timers, nullopt when
    // neither is set.
    std::optional<double> nextDeadline() const;

    // riley dock/src/autohide.rs:151 band_input(). The activation band takes input while Hidden or Hiding, and
    // while the pointer stays in it after a reveal (band held).
    bool bandInput() const;

    // riley dock/src/autohide.rs:169 offset(now) = motion(now).value.
    double offset(double now) const;

    // riley dock/src/autohide.rs:172 advance(now). Apply every transition due by `now`, in time order. Loop: the
    // candidates are the pose end (Revealing: start + REVEAL_SECONDS; Hiding: start + HIDE_SECONDS), the dwell
    // timer and the hide timer; take the smallest that is <= now (none: return). If it is the pose end: Revealing
    // -> Shown, and if hide-after-reveal was set (it is cleared) armHide(at); Hiding -> Hidden, band not held.
    // Else if it is the dwell: clear it; if Hidden and zone != Outside, reveal(at). Else (the hide timer): clear
    // it; if enabled, zone Outside, not blocked and pose Shown or Revealing: pose = Hiding{at, offset(at)}, band
    // not held. Ties are taken in that order (pose end, dwell, hide).
    void advance(double now);

    // riley dock/src/autohide.rs:218 pointer(zone, now). advance(now); remember the previous zone and set the new
    // one; if disabled stop there. Hidden: zone Outside clears the dwell; else entering from Outside starts
    // dwell = now + DWELL_SECONDS (motion inside the band never restarts it). Hiding: zone != Outside ->
    // reveal(now). Revealing or Shown: zone != Band releases the held band; zone Outside -> armHide(now), else the
    // hide timer is cleared.
    void pointer(Zone zone, double now);

    // riley dock/src/autohide.rs:237 set_blockers(menu, label, now). An open menu or hover label blocks hiding; a
    // fresh delay starts only when all are gone. advance(now); blocked = menu || label; unchanged -> return.
    // Becoming blocked clears the hide timer and a Hiding pose reveals(now); becoming unblocked in Shown or
    // Revealing armHide(now).
    void setBlockers(bool menu, bool label, double now);

    // riley dock/src/autohide.rs:251 show(now). Keyboard, control socket or attention: reveal at once, without
    // dwell. advance(now); disabled -> return. Hidden or Hiding: reveal(now) and set hide-after-reveal. Revealing:
    // nothing (idempotent). Shown: if a hide timer is set, clear it and armHide(now) (a fresh delay).
    void show(double now);

    // riley dock/src/autohide.rs:262 set_enabled(enabled, now). advance(now); unchanged -> return. Enabling keeps
    // the pose and, in Shown or Revealing, armHide(now). Disabling: Hidden or Hiding reveal(now); then dwell and
    // hide timers cleared, band not held, hide-after-reveal cleared.
    void setEnabled(bool enabled, double now);

    // riley dock/src/autohide.rs:279 input_region(now, shown, lip, width, height). Surface input for this pose.
    // `shown` is the untranslated shown region; everything is clipped to width x height. Unless Hidden: every shown
    // rect moved down by int(round(offset(now))) (round half away from zero), clipped, empty ones dropped. Then,
    // if bandInput(), the clipped lip.
    QList<motion::InputRect> inputRegion(double now, const QList<motion::InputRect> &shown, motion::InputRect lip,
                                         int width, int height) const;

private:
    // riley dock/src/autohide.rs:155 motion(now) -> (offset, velocity). Hidden (distance, 0); Shown (0, 0);
    // Hiding hideSample(from, distance, now - start); Revealing: now - start >= REVEAL_SECONDS -> (0, 0), else
    // revealRaw(from, velocity, now - start), clamped to (distance, 0) when its value >= distance (an interrupted
    // hide still moving down must never carry the lip off screen).
    std::pair<double, double> motionAt(double now) const;

    // riley dock/src/autohide.rs:204 arm_hide(at). If enabled, zone Outside, not blocked and no hide timer:
    // hide timer = at + HIDE_DELAY_SECONDS.
    void armHide(double at);

    // riley dock/src/autohide.rs:210 reveal(at). (from, velocity) = motionAt(at); pose = Revealing{at, from,
    // velocity}; band held = (zone == Band); dwell and hide timers cleared.
    void reveal(double at);

    bool m_enabled = false;             // enabled
    Pose m_pose;                        // pose
    Zone m_zone = Zone::Outside;        // zone
    bool m_blocked = false;             // blocked
    std::optional<double> m_dwell;      // dwell
    std::optional<double> m_hideAt;     // hide_at
    bool m_bandHeld = false;            // band_held
    bool m_hideAfterReveal = false;     // hide_after_reveal
};

namespace pose {
// Rust #[derive(Debug)] on Pose (found by argument-dependent lookup through the variant's alternatives).
QDebug operator<<(QDebug debug, const Pose &pose);
} // namespace pose

} // namespace riley::dock::autohide
