// SPDX-License-Identifier: GPL-3.0-or-later
//
// libriley-dock: motion. Port of riley dock/src/motion.rs: the swell cosine, icon packing, click-through input
// regions, reflections, the leave spring and the click squash. Every formula keeps the Rust operation order.
#include <riley-dock/motion.h>

#include <QtCore/QByteArray>
#include <QtCore/QLocale>

#include <cmath>
#include <limits>
#include <numbers>

namespace riley::dock::motion {

namespace {

constexpr double PI = std::numbers::pi; // riley dock/src/motion.rs:3 std::f64::consts::PI

// Rust `f64 as i32`: truncates toward zero, saturates at the i32 bounds, NaN becomes 0.
int asI32(double value)
{
    if (std::isnan(value))
        return 0;
    if (value <= static_cast<double>(std::numeric_limits<int>::min()))
        return std::numeric_limits<int>::min();
    if (value >= static_cast<double>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return static_cast<int>(value);
}

// riley dock/src/motion.rs:84 LayoutConfig::pack(scales, hover).
Layout pack(const LayoutConfig &config, const QList<double> &scales, std::optional<std::pair<qsizetype, double>> hover)
{
    // scales.iter().sum::<f64>(): a left-to-right sum.
    double sum = 0.0;
    for (const double scale : scales)
        sum += scale;
    // scales.len().saturating_sub(1) as f64
    const qsizetype gaps = scales.size() > 0 ? scales.size() - 1 : 0;
    const double contentWidth = sum * config.restWidth + static_cast<double>(gaps) * config.gap;
    const double width = contentWidth + 2.0 * config.sidePadding;
    Layout result;
    result.bar = Rect{(config.surfaceWidth - width) / 2.0, config.barY, width, config.barHeight};
    double x = result.bar.x + config.sidePadding;
    result.icons.reserve(scales.size());
    for (qsizetype i = 0; i < scales.size(); ++i) {
        const double scale = scales[i];
        const double size = config.restWidth * scale;
        const double rise = (hover && hover->first == i) ? hover->second : 0.5 * config.restWidth * (scale - 1.0);
        result.icons.append(Rect{x, config.restBottom - rise - size, size, size});
        x += size + config.gap;
    }
    return result;
}

// Shortest round-trip text of a double, unquoted, for the debug output below.
struct Num {
    QByteArray text;
};
Num num(double value)
{
    return Num{QByteArray::number(value, 'g', QLocale::FloatingPointShortest)};
}
QDebug operator<<(QDebug debug, const Num &n)
{
    return debug << n.text.constData();
}

} // namespace

// riley dock/src/motion.rs:9
double swellScale(double distance)
{
    const double d = std::abs(distance);
    if (d >= 2.5)
        return 1.0;
    return 1.0 + 0.3 * (1.0 + std::cos(PI * d / 2.5));
}

// riley dock/src/motion.rs:19
QList<double> pointerScales(const QList<double> &restCentres, std::optional<double> pointerX, double restWidth)
{
    // riley dock/src/motion.rs:20 assert! (always checked, as in Rust release builds).
    if (!(std::isfinite(restWidth) && restWidth > 0.0))
        qFatal("assertion failed: rest_width.is_finite() && rest_width > 0.0");
    QList<double> scales;
    scales.reserve(restCentres.size());
    for (const double x : restCentres)
        scales.append(pointerX ? swellScale((*pointerX - x) / restWidth) : 1.0);
    return scales;
}

// riley dock/src/motion.rs:44
InputRect Rect::outward() const
{
    const int left = asI32(std::floor(x));
    const int top = asI32(std::floor(y));
    return InputRect{left, top, asI32(std::ceil(x + width)) - left, asI32(std::ceil(y + height)) - top};
}

// riley dock/src/motion.rs:75
Layout LayoutConfig::layout(const QList<double> &scales) const
{
    return pack(*this, scales, std::nullopt);
}

// riley dock/src/motion.rs:80
Layout LayoutConfig::noSwell(qsizetype count, std::optional<qsizetype> hovered, double hoverRise) const
{
    std::optional<std::pair<qsizetype, double>> hover;
    if (hovered)
        hover = std::pair<qsizetype, double>{*hovered, hoverRise};
    return pack(*this, QList<double>(count, 1.0), hover);
}

// riley dock/src/motion.rs:109
QList<InputRect> Layout::inputRegion() const
{
    QList<InputRect> region{bar.outward()};
    const Rect b = bar;
    for (const Rect &r : icons) {
        const double right = r.x + r.width;
        const double bottom = r.y + r.height;
        const double left = std::fmax(r.x, b.x); // f64::max / f64::min are fmax / fmin
        const double top = std::fmax(r.y, b.y);
        const double cutRight = std::fmin(right, b.x + b.width);
        const double cutBottom = std::fmin(bottom, b.y + b.height);
        if (left >= cutRight || top >= cutBottom) {
            region.append(r.outward());
            continue;
        }
        const Rect parts[] = {
            Rect{r.x, r.y, r.width, top - r.y},
            Rect{r.x, cutBottom, r.width, bottom - cutBottom},
            Rect{r.x, top, left - r.x, cutBottom - top},
            Rect{cutRight, top, right - cutRight, cutBottom - top},
        };
        for (const Rect &part : parts) {
            if (part.width > 0.0 && part.height > 0.0)
                region.append(part.outward());
        }
    }
    return region;
}

// riley dock/src/motion.rs:146
Reflection reflection(Rect icon, double baseline)
{
    const double rise = baseline - (icon.y + icon.height);
    const double top = baseline + rise;
    const Rect image{icon.x, top, icon.width, icon.height};
    Rect visible = image;
    visible.height = icon.height * 0.45;
    return Reflection{image, visible, top + icon.height, {Point{0.0, 0.55}, Point{1.0, 0.0}}};
}

// riley dock/src/motion.rs:154
double reflectionPadding(double restWidth, bool enabled)
{
    return enabled ? std::ceil(restWidth * 0.45) : 0.0;
}

// riley dock/src/motion.rs:158
LayoutConfig reflectionLayout(LayoutConfig config, bool enabled)
{
    config.barHeight += reflectionPadding(config.restWidth, enabled);
    return config;
}

// riley dock/src/motion.rs:163
Rect shelf(const Layout &layout, double baseline, double sidePadding)
{
    // f64::max: NaN on one side yields the other, as std::fmax does.
    return Rect{layout.bar.x + sidePadding, baseline, std::fmax(layout.bar.width - 2.0 * sidePadding, 0.0), 1.0};
}

// riley dock/src/motion.rs:191
Sample Spring::raw(double seconds) const
{
    const double t = std::fmax(seconds, 0.0);
    const double a = std::log(7.5) / 0.14;
    const double w = PI / 0.14;
    const double c = initial - target;
    const double d = (velocity + a * c) / w;
    const double sin = std::sin(w * t);
    const double cos = std::cos(w * t);
    const double decay = std::exp(-a * t);
    const double offset = c * cos + d * sin;
    return Sample{target + decay * offset, decay * ((d * w * cos - c * w * sin) - a * offset),
                  c != 0.0 || velocity != 0.0};
}

// riley dock/src/motion.rs:207
Sample Spring::sample(double seconds) const
{
    if (seconds >= SETTLE_SECONDS)
        return Sample{target, 0.0, false};
    return raw(seconds);
}

// riley dock/src/motion.rs:217
Sample clickSquash(double seconds)
{
    const double t = std::fmax(seconds, 0.0);
    if (t <= SQUASH_SECONDS) {
        const double phase = PI * t / SQUASH_SECONDS;
        return Sample{1.0 - 0.1 * (1.0 - std::cos(phase)) / 2.0, -0.05 * PI / SQUASH_SECONDS * std::sin(phase), true};
    }
    return Spring{0.9, 0.0, 1.0}.sample(t - SQUASH_SECONDS);
}

QDebug operator<<(QDebug debug, const InputRect &rect)
{
    const QDebugStateSaver saver(debug);
    debug.nospace() << "InputRect { x: " << rect.x << ", y: " << rect.y << ", width: " << rect.width
                    << ", height: " << rect.height << " }";
    return debug;
}

QDebug operator<<(QDebug debug, const Rect &rect)
{
    const QDebugStateSaver saver(debug);
    debug.nospace() << "Rect { x: " << num(rect.x) << ", y: " << num(rect.y)
                    << ", width: " << num(rect.width)
                    << ", height: " << num(rect.height) << " }";
    return debug;
}

QDebug operator<<(QDebug debug, const Layout &layout)
{
    const QDebugStateSaver saver(debug);
    debug.nospace() << "Layout { bar: " << layout.bar << ", icons: [";
    for (qsizetype i = 0; i < layout.icons.size(); ++i)
        debug << (i ? ", " : "") << layout.icons[i];
    debug << "] }";
    return debug;
}

QDebug operator<<(QDebug debug, const Reflection &reflection)
{
    const QDebugStateSaver saver(debug);
    debug.nospace() << "Reflection { image: " << reflection.image << ", visible: " << reflection.visible
                    << ", flip_y: " << num(reflection.flipY) << ", stops: [";
    for (std::size_t i = 0; i < reflection.stops.size(); ++i)
        debug << (i ? ", " : "") << "(" << num(reflection.stops[i].first) << ", "
              << num(reflection.stops[i].second) << ")";
    debug << "] }";
    return debug;
}

QDebug operator<<(QDebug debug, const Sample &sample)
{
    const QDebugStateSaver saver(debug);
    debug.nospace() << "Sample { value: " << num(sample.value)
                    << ", velocity: " << num(sample.velocity)
                    << ", active: " << sample.active << " }";
    return debug;
}

} // namespace riley::dock::motion
