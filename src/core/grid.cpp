// SPDX-License-Identifier: GPL-3.0-or-later
//
// libriley-dock: grid. Port of riley dock/src/grid.rs.
#include <riley-dock/grid.h>

#include <QtCore/QDebugStateSaver>
#include <QtCore/QStringView>

#include <algorithm>
#include <cmath>

namespace riley::dock::grid {

namespace {

// Unicode White_Space, the set Rust str::trim removes. Every member is in the Basic Multilingual Plane, so one
// UTF-16 unit is enough.
bool isWhiteSpace(char16_t c)
{
    return (c >= 0x0009 && c <= 0x000D) || c == 0x0020 || c == 0x0085 || c == 0x00A0 || c == 0x1680
           || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F
           || c == 0x3000;
}

// Rust str::trim.
QStringView trim(QStringView text)
{
    qsizetype begin = 0;
    qsizetype end = text.size();
    while (begin < end && isWhiteSpace(text[begin].unicode()))
        ++begin;
    while (end > begin && isWhiteSpace(text[end - 1].unicode()))
        --end;
    return text.sliced(begin, end - begin);
}

// Rust str::lines: split at '\n'; a line that ended with '\n' also loses one trailing '\r'; a final '\n' does
// not start an empty line.
QList<QStringView> lines(QStringView text)
{
    QList<QStringView> result;
    qsizetype start = 0;
    while (start < text.size()) {
        const qsizetype newline = text.indexOf(u'\n', start);
        if (newline < 0) {
            result.append(text.sliced(start));
            break;
        }
        QStringView line = text.sliced(start, newline - start);
        if (line.endsWith(u'\r'))
            line.chop(1);
        result.append(line);
        start = newline + 1;
    }
    return result;
}

} // namespace

// riley dock/src/grid.rs:7
Position position(const QString &config)
{
    QStringView value;
    for (const QStringView line : lines(config)) {
        // riley dock/src/grid.rs:10 split_once('=')
        const qsizetype equals = line.indexOf(u'=');
        if (equals < 0)
            continue;
        if (trim(line.first(equals)) == u"LAUNCHER")
            value = trim(line.sliced(equals + 1));
    }
    // riley dock/src/grid.rs:14
    if (value == u"right")
        return Position::Right;
    if (value == u"center")
        return Position::Center;
    if (value == u"off")
        return Position::Off;
    return Position::Left;
}

// riley dock/src/grid.rs:18
QList<rules::DockItem> placeItems(QList<rules::DockItem> items, Position position)
{
    std::optional<rules::DockItem> control;
    for (qsizetype i = 0; i < items.size(); ++i) {
        if (items[i].id == u"launcher") {
            control = items.takeAt(i);
            break;
        }
    }
    if (position != Position::Off) {
        // riley dock/src/grid.rs:21
        qsizetype index = 0;
        switch (position) {
        case Position::Left:
            index = 0;
            break;
        case Position::Right:
            index = items.size();
            break;
        case Position::Center:
            index = (items.size() + 1) / 2; // usize::div_ceil(2)
            break;
        case Position::Off:
            Q_UNREACHABLE();
        }
        items.insert(index, control ? std::move(*control) : rules::DockItem{QStringLiteral("launcher"), {}});
    }
    return items;
}

// riley dock/src/grid.rs:28
qsizetype appCount(const QList<rules::DockItem> &items)
{
    return std::count_if(items.cbegin(), items.cend(),
                         [](const rules::DockItem &item) { return item.id != u"launcher"; });
}

// riley dock/src/grid.rs:32
motion::Layout layout(const motion::LayoutConfig &config, const QList<double> &scales,
                      std::optional<qsizetype> grid)
{
    QList<double> owned = scales;
    if (grid && *grid >= 0 && *grid < owned.size())
        owned[*grid] = 1.0;
    motion::Layout result = config.layout(owned);
    if (grid && *grid >= 0 && *grid < result.icons.size()) {
        motion::Rect &rect = result.icons[*grid];
        rect.y = result.bar.y + (result.bar.height - rect.height) / 2.0;
    }
    return result;
}

// riley dock/src/grid.rs:42
motion::Reflection reflection(motion::Rect rect)
{
    return motion::reflection(rect, rect.y + rect.height);
}

// riley dock/src/grid.rs:52
std::pair<std::array<motion::Point, 9>, double> dots(motion::Rect rect, Look look)
{
    const double size = std::fmin(rect.width, rect.height); // f64::min: a NaN operand yields the other
    const double step = size / 4.0 * look.spacing;
    std::array<motion::Point, 9> points{};
    for (std::size_t i = 0; i < points.size(); ++i)
        points[i] = {rect.x + rect.width / 2.0 + static_cast<double>(i % 3) * step - step,
                     rect.y + rect.height / 2.0 + static_cast<double>(i / 3) * step - step};
    return {points, size / 16.0};
}

// riley dock/src/grid.rs:74
Look Animation::target() const
{
    return m_hovered ? Look{1.2, 1.0} : Look{};
}

// riley dock/src/grid.rs:78
std::pair<Look, bool> Animation::evaluate(double now)
{
    const Look target = this->target();
    const double t = std::clamp((now - m_began) / 0.15, 0.0, 1.0);
    const double u = 1.0 - t;
    const double q = 1.0 - u * u * u; // f64::powi(3): (u*u)*u
    Look look{m_from.spacing + (target.spacing - m_from.spacing) * q,
              m_from.accent + (target.accent - m_from.accent) * q};
    bool active = t < 1.0 && m_from != target;
    if (m_pressed) {
        look.spacing = 0.6;
    } else if (m_spring) {
        const auto [start, spring] = *m_spring;
        const motion::Sample sample = spring.sample(now - start);
        look.spacing = sample.value;
        active = active || sample.active;
        if (!sample.active)
            m_spring.reset();
    }
    m_look = look;
    return {look, active};
}

// riley dock/src/grid.rs:96
void Animation::setHover(bool hovered, double now)
{
    if (m_hovered == hovered)
        return;
    evaluate(now);
    m_from = m_look;
    m_began = now;
    m_hovered = hovered;
    if (m_spring) {
        const auto [start, spring] = *m_spring;
        const motion::Sample sample = spring.sample(now - start);
        m_spring = std::pair{now, motion::Spring{sample.value, sample.velocity, target().spacing}};
    }
}

// riley dock/src/grid.rs:108
void Animation::press(double now)
{
    evaluate(now);
    m_pressed = true;
    m_spring.reset();
    m_launchAt.reset();
}

// riley dock/src/grid.rs:115
bool Animation::isPressed() const
{
    return m_pressed;
}

// riley dock/src/grid.rs:117
void Animation::release(double now, bool activate)
{
    m_pressed = false;
    m_spring = std::pair{now, motion::Spring{0.6, 0.0, target().spacing}};
    m_launchAt = activate ? std::optional<double>(now + motion::SETTLE_SECONDS) : std::nullopt;
}

// riley dock/src/grid.rs:123
Sample Animation::sample(double now)
{
    const auto [look, active] = evaluate(now);
    const bool launch = m_launchAt && now >= *m_launchAt;
    if (launch)
        m_launchAt.reset();
    return Sample{look, active || m_launchAt.has_value(), launch};
}

QDebug operator<<(QDebug debug, const Look &look)
{
    const QDebugStateSaver saver(debug);
    debug.nospace() << qSetRealNumberPrecision(17) << "Look { spacing: " << look.spacing << ", accent: "
                    << look.accent << " }";
    return debug;
}

} // namespace riley::dock::grid
