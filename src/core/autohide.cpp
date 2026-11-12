// SPDX-License-Identifier: GPL-3.0-or-later
//
// libriley-dock: autohide. A port of riley dock/src/autohide.rs; each function cites the Rust item it translates.
#include <riley-dock/autohide.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace riley::dock::autohide {

namespace {

// Rust `f64 as i32`: truncation towards zero, saturating at the i32 range, NaN to 0.
int asI32(double value)
{
    if (std::isnan(value))
        return 0;
    if (value <= double(std::numeric_limits<int>::min()))
        return std::numeric_limits<int>::min();
    if (value >= double(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return static_cast<int>(value);
}

} // namespace

// riley dock/src/autohide.rs:20
double revealDamping()
{
    return std::log(12.5) / REVEAL_PEAK_SECONDS;
}

// riley dock/src/autohide.rs:23
bool enabled(const QString &config)
{
    std::optional<QString> value;
    for (const QString &line : config.split(u'\n')) {
        const qsizetype eq = line.indexOf(u'=');
        if (eq < 0)
            continue;
        if (line.left(eq).trimmed() == u"AUTOHIDE")
            value = line.mid(eq + 1).trimmed();
    }
    return value.has_value() && *value == u"1";
}

// riley dock/src/autohide.rs:34
std::pair<motion::LayoutConfig, int> dockLayout(quint32 size, qsizetype count, qsizetype apps, bool reflections)
{
    const double rest = double(std::max<quint32>(rules::drawnSize(size, apps), 1)) * 0.8;
    const double barHeight = rest + 23.0;
    const int baseHeight = asI32(std::ceil(rest * 2.2 + 36.0));
    // Keep a fixed transparent surface; only the painted bar changes width (riley dock/src/autohide.rs:38).
    const qsizetype gaps = count > 0 ? count - 1 : 0;
    const double width = std::ceil(double(count) * rest * 1.8 + double(gaps) * 8.0 + 20.0);
    motion::LayoutConfig base;
    base.surfaceWidth = width;
    base.restWidth = rest;
    base.gap = 8.0;
    base.sidePadding = 10.0;
    base.barY = double(baseHeight) - barHeight;
    base.barHeight = barHeight;
    base.restBottom = double(baseHeight) - 17.0;
    const motion::LayoutConfig config = motion::reflectionLayout(base, reflections);
    return {config, baseHeight + asI32(motion::reflectionPadding(rest, reflections)) + 8};
}

// riley dock/src/autohide.rs:48
double hiddenOffset(double surfaceHeight, double barY)
{
    return surfaceHeight - LIP - barY;
}

// riley dock/src/autohide.rs:53
int exclusiveZone(bool autohide, double barHeight)
{
    if (autohide)
        return asI32(std::ceil(LIP));
    return asI32(std::ceil(barHeight)) + 8;
}

// riley dock/src/autohide.rs:58
motion::InputRect lipRegion(motion::Rect bar, double surfaceHeight)
{
    return motion::Rect{bar.x, surfaceHeight - LIP, bar.width, LIP}.outward();
}

// riley dock/src/autohide.rs:62
motion::InputRect triggerStrip(motion::Rect bar, double surfaceHeight)
{
    return motion::Rect{bar.x, surfaceHeight - TRIGGER, bar.width, TRIGGER}.outward();
}

// riley dock/src/autohide.rs:68
QList<bool> running(const QList<rules::DockItem> &items)
{
    QList<bool> result;
    result.reserve(items.size());
    for (const rules::DockItem &item : items)
        result.append(item.id != u"launcher" && !item.windows.isEmpty());
    return result;
}

// riley dock/src/autohide.rs:73
QList<motion::Point> lipDots(const motion::Layout &rest, const QList<bool> &running, double surfaceHeight)
{
    QList<motion::Point> dots;
    const qsizetype n = std::min(rest.icons.size(), running.size());
    for (qsizetype i = 0; i < n; ++i) {
        if (!running[i])
            continue;
        const motion::Rect &r = rest.icons[i];
        dots.append({r.x + r.width / 2.0, surfaceHeight - LIP / 2.0});
    }
    return dots;
}

// riley dock/src/autohide.rs:78
std::optional<motion::InputRect> clip(motion::InputRect rect, int width, int height)
{
    const int left = std::max(rect.x, 0);
    const int top = std::max(rect.y, 0);
    const int right = std::min(rect.x + rect.width, width);
    const int bottom = std::min(rect.y + rect.height, height);
    if (right > left && bottom > top)
        return motion::InputRect{left, top, right - left, bottom - top};
    return std::nullopt;
}

// riley dock/src/autohide.rs:84
motion::Layout translate(const motion::Layout &layout, double offset)
{
    const auto shift = [offset](const motion::Rect &r) {
        motion::Rect moved = r;
        moved.y = r.y + offset;
        return moved;
    };
    motion::Layout result;
    result.bar = shift(layout.bar);
    result.icons.reserve(layout.icons.size());
    for (const motion::Rect &r : layout.icons)
        result.icons.append(shift(r));
    return result;
}

// riley dock/src/autohide.rs:90
std::pair<double, double> hideSample(double from, double distance, double seconds)
{
    const double t = std::clamp(seconds, 0.0, HIDE_SECONDS) / HIDE_SECONDS;
    const double span = distance - from;
    return {from + span * t * t, seconds < HIDE_SECONDS ? 2.0 * span * t / HIDE_SECONDS : 0.0};
}

// riley dock/src/autohide.rs:97
std::pair<double, double> revealRaw(double from, double velocity, double seconds)
{
    const double t = std::fmax(seconds, 0.0); // Rust f64::max: a NaN argument yields the other one
    const double a = revealDamping();
    const double w = std::numbers::pi / REVEAL_PEAK_SECONDS;
    const double d = (velocity + a * from) / w;
    const double sin = std::sin(w * t);
    const double cos = std::cos(w * t);
    const double decay = std::exp(-a * t);
    const double offset = from * cos + d * sin;
    return {decay * offset, decay * ((d * w * cos - from * w * sin) - a * offset)};
}

// riley dock/src/autohide.rs:136
Controller::Controller(bool enabled, double distance)
    : distance(distance)
    , m_enabled(enabled)
    , m_pose(enabled ? Pose{pose::Hidden{}} : Pose{pose::Shown{}})
{
}

// riley dock/src/autohide.rs:141
bool Controller::enabled() const
{
    return m_enabled;
}

// riley dock/src/autohide.rs:142
Pose Controller::pose() const
{
    return m_pose;
}

// riley dock/src/autohide.rs:143
bool Controller::settledHidden() const
{
    return std::holds_alternative<pose::Hidden>(m_pose);
}

// riley dock/src/autohide.rs:144
bool Controller::settledShown() const
{
    return std::holds_alternative<pose::Shown>(m_pose);
}

// riley dock/src/autohide.rs:145
bool Controller::animating() const
{
    return std::holds_alternative<pose::Revealing>(m_pose) || std::holds_alternative<pose::Hiding>(m_pose);
}

// riley dock/src/autohide.rs:146
std::optional<double> Controller::nextDeadline() const
{
    if (m_dwell && m_hideAt)
        return std::fmin(*m_dwell, *m_hideAt);
    return m_dwell ? m_dwell : m_hideAt;
}

// riley dock/src/autohide.rs:151
bool Controller::bandInput() const
{
    return std::holds_alternative<pose::Hidden>(m_pose) || std::holds_alternative<pose::Hiding>(m_pose)
        || m_bandHeld;
}

// riley dock/src/autohide.rs:155
std::pair<double, double> Controller::motionAt(double now) const
{
    if (std::holds_alternative<pose::Hidden>(m_pose))
        return {distance, 0.0};
    if (std::holds_alternative<pose::Shown>(m_pose))
        return {0.0, 0.0};
    if (const auto *hiding = std::get_if<pose::Hiding>(&m_pose))
        return hideSample(hiding->from, distance, now - hiding->start);
    const auto &revealing = std::get<pose::Revealing>(m_pose);
    if (now - revealing.start >= REVEAL_SECONDS)
        return {0.0, 0.0};
    const auto [value, speed] = revealRaw(revealing.from, revealing.velocity, now - revealing.start);
    // An interrupted hide still moving down must never carry the lip off screen (riley dock/src/autohide.rs:163).
    if (value >= distance)
        return {distance, 0.0};
    return {value, speed};
}

// riley dock/src/autohide.rs:169
double Controller::offset(double now) const
{
    return motionAt(now).first;
}

// riley dock/src/autohide.rs:172
void Controller::advance(double now)
{
    for (;;) {
        std::optional<double> end;
        if (const auto *revealing = std::get_if<pose::Revealing>(&m_pose))
            end = revealing->start + REVEAL_SECONDS;
        else if (const auto *hiding = std::get_if<pose::Hiding>(&m_pose))
            end = hiding->start + HIDE_SECONDS;

        std::optional<double> next;
        for (const std::optional<double> &candidate : {end, m_dwell, m_hideAt}) {
            if (!candidate || !(*candidate <= now))
                continue;
            next = next ? std::fmin(*next, *candidate) : *candidate;
        }
        if (!next)
            return;
        const double at = *next;

        if (end == at) {
            if (std::holds_alternative<pose::Revealing>(m_pose)) {
                m_pose = pose::Shown{};
                const bool hideAfter = std::exchange(m_hideAfterReveal, false);
                if (hideAfter)
                    armHide(at);
            } else {
                m_pose = pose::Hidden{};
                m_bandHeld = false;
            }
        } else if (m_dwell == at) {
            m_dwell.reset();
            if (std::holds_alternative<pose::Hidden>(m_pose) && m_zone != Zone::Outside)
                reveal(at);
        } else {
            m_hideAt.reset();
            if (m_enabled && m_zone == Zone::Outside && !m_blocked
                && (std::holds_alternative<pose::Shown>(m_pose) || std::holds_alternative<pose::Revealing>(m_pose))) {
                m_pose = pose::Hiding{at, offset(at)};
                m_bandHeld = false;
            }
        }
    }
}

// riley dock/src/autohide.rs:204
void Controller::armHide(double at)
{
    if (m_enabled && m_zone == Zone::Outside && !m_blocked && !m_hideAt)
        m_hideAt = at + HIDE_DELAY_SECONDS;
}

// riley dock/src/autohide.rs:210
void Controller::reveal(double at)
{
    const auto [from, velocity] = motionAt(at);
    m_pose = pose::Revealing{at, from, velocity};
    m_bandHeld = m_zone == Zone::Band;
    m_dwell.reset();
    m_hideAt.reset();
}

// riley dock/src/autohide.rs:218
void Controller::pointer(Zone zone, double now)
{
    advance(now);
    const Zone previous = std::exchange(m_zone, zone);
    if (!m_enabled)
        return;
    if (std::holds_alternative<pose::Hidden>(m_pose)) {
        // Continuous presence: motion inside the band never restarts the dwell (riley dock/src/autohide.rs:223).
        if (zone == Zone::Outside)
            m_dwell.reset();
        else if (previous == Zone::Outside)
            m_dwell = now + DWELL_SECONDS;
    } else if (std::holds_alternative<pose::Hiding>(m_pose)) {
        if (zone != Zone::Outside)
            reveal(now);
    } else {
        if (zone != Zone::Band)
            m_bandHeld = false;
        if (zone == Zone::Outside)
            armHide(now);
        else
            m_hideAt.reset();
    }
}

// riley dock/src/autohide.rs:237
void Controller::setBlockers(bool menu, bool label, double now)
{
    advance(now);
    const bool blocked = menu || label;
    if (blocked == m_blocked)
        return;
    m_blocked = blocked;
    if (blocked) {
        m_hideAt.reset();
        if (std::holds_alternative<pose::Hiding>(m_pose))
            reveal(now);
    } else if (std::holds_alternative<pose::Shown>(m_pose) || std::holds_alternative<pose::Revealing>(m_pose)) {
        armHide(now);
    }
}

// riley dock/src/autohide.rs:251
void Controller::show(double now)
{
    advance(now);
    if (!m_enabled)
        return;
    if (std::holds_alternative<pose::Hidden>(m_pose) || std::holds_alternative<pose::Hiding>(m_pose)) {
        reveal(now);
        m_hideAfterReveal = true;
    } else if (std::holds_alternative<pose::Shown>(m_pose)) {
        if (m_hideAt) {
            m_hideAt.reset();
            armHide(now);
        }
    }
    // Revealing: nothing (riley dock/src/autohide.rs:256).
}

// riley dock/src/autohide.rs:262
void Controller::setEnabled(bool enabled, double now)
{
    advance(now);
    if (enabled == m_enabled)
        return;
    m_enabled = enabled;
    if (enabled) {
        if (std::holds_alternative<pose::Shown>(m_pose) || std::holds_alternative<pose::Revealing>(m_pose))
            armHide(now);
    } else {
        const bool hidden = std::holds_alternative<pose::Hidden>(m_pose) || std::holds_alternative<pose::Hiding>(m_pose);
        if (hidden)
            reveal(now);
        m_dwell.reset();
        m_hideAt.reset();
        m_bandHeld = false;
        m_hideAfterReveal = false;
    }
}

// riley dock/src/autohide.rs:279
QList<motion::InputRect> Controller::inputRegion(double now, const QList<motion::InputRect> &shown,
                                                 motion::InputRect lip, int width, int height) const
{
    QList<motion::InputRect> region;
    if (!std::holds_alternative<pose::Hidden>(m_pose)) {
        const int offset = asI32(std::round(this->offset(now)));
        for (const motion::InputRect &r : shown) {
            motion::InputRect moved = r;
            moved.y = r.y + offset;
            if (const auto clipped = clip(moved, width, height))
                region.append(*clipped);
        }
    }
    if (bandInput()) {
        if (const auto clipped = clip(lip, width, height))
            region.append(*clipped);
    }
    return region;
}

namespace pose {

// riley dock/src/autohide.rs:111 #[derive(Debug)] on Pose.
QDebug operator<<(QDebug debug, const Pose &value)
{
    const QDebugStateSaver saver(debug);
    debug.nospace() << qSetRealNumberPrecision(17);
    if (std::holds_alternative<Hidden>(value)) {
        debug << "Hidden";
    } else if (std::holds_alternative<Shown>(value)) {
        debug << "Shown";
    } else if (const auto *revealing = std::get_if<Revealing>(&value)) {
        debug << "Revealing { start: " << revealing->start << ", from: " << revealing->from
              << ", velocity: " << revealing->velocity << " }";
    } else {
        const auto &hiding = std::get<Hiding>(value);
        debug << "Hiding { start: " << hiding.start << ", from: " << hiding.from << " }";
    }
    return debug;
}

} // namespace pose

} // namespace riley::dock::autohide
