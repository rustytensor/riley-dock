// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/dockwindow.h"

#include "app/log.h"

#include <riley-dock/desktop.h>
#include <riley-dock/rules.h>

#include <LayerShellQt/Window>

#include <QtGui/QMouseEvent>
#include <QtGui/QScreen>
#include <QtQuick/QQuickItem>

#include <algorithm>
#include <cmath>

namespace riley::dock::app {

namespace {

// GTK button numbers (riley dock/src/ui.rs:647, :676-686): 1 primary, 2 middle, 3 secondary, 0 anything else.
int gtkButton(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton: return 1;
    case Qt::MiddleButton: return 2;
    case Qt::RightButton: return 3;
    default: return 0;
    }
}

QRectF toQt(const motion::Rect &r)
{
    return QRectF(r.x, r.y, r.width, r.height);
}

} // namespace

DockWindow::DockWindow(DockApp &app, int index, QString output, QScreen *screen, bool autohide)
    : QQuickView(app.engine(), nullptr), m_app(app), m_index(index), m_output(std::move(output)),
      m_autohide(autohide, 0.0)
{
    const double now = monotonicSeconds();
    m_lastTick = now;
    m_gridEpoch = now;
    m_model = new DockModel(app.style(), m_output, this);

    // riley dock/src/ui.rs:94-102: a 1x1 start size, layer TOP, bottom anchor only (centred), bottom margin 0 (the
    // canvas includes the 8 px gap, with no input in that tail), namespace "riley-dock", no keyboard.
    setColor(Qt::transparent);
    setResizeMode(QQuickView::SizeRootObjectToView);
    setScreen(screen);
    resize(1, 1);
    m_layer = LayerShellQt::Window::get(this);
    m_layer->setScope(QStringLiteral("riley-dock"));
    m_layer->setLayer(LayerShellQt::Window::LayerTop);
    m_layer->setScreen(screen);
    m_layer->setAnchors(LayerShellQt::Window::AnchorBottom);
    m_layer->setMargins(QMargins());
    m_layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
    // The exclusive zone is set by frame() alone, from the core, before the first show.

    // riley dock/src/ui.rs:105-120: the hover name, its own surface on the OVERLAY layer, anchored bottom-left,
    // ignoring other surfaces' zones, with an empty input region.
    m_nameView = new QQuickView(app.engine(), nullptr);
    m_nameView->setColor(Qt::transparent);
    m_nameView->setFlag(Qt::WindowTransparentForInput, true);
    m_nameView->setResizeMode(QQuickView::SizeRootObjectToView);
    m_nameView->setScreen(screen);
    m_nameView->resize(1, 1);
    m_nameLayer = LayerShellQt::Window::get(m_nameView);
    m_nameLayer->setScope(QStringLiteral("riley-dock-name"));
    m_nameLayer->setLayer(LayerShellQt::Window::LayerOverlay);
    m_nameLayer->setScreen(screen);
    m_nameLayer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorBottom)
                            | LayerShellQt::Window::AnchorLeft);
    m_nameLayer->setExclusiveZone(-1); // the name surface, not the dock: never reserves space
    m_nameLayer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);

    DockMenu *menu = m_model->menu();
    menu->onClosed = [this] { m_app.menuClosed(); };
    menu->onPick = [this](int i) {
        if (i < m_menuWindows.size())
            m_app.perform(rules::menuPick(m_menuWindows[i]), m_index);
    };
    menu->onWindowAction = [this](int window, int action) {
        if (window >= m_menuWindows.size())
            return;
        // riley dock/src/ui.rs:816-818: Close, Float/tile, Full screen, then Move to workspace 1..9.
        ipc::WindowAction chosen = ipc::window_action::Close{};
        if (action == 1)
            chosen = ipc::window_action::Float{};
        else if (action == 2)
            chosen = ipc::window_action::Fullscreen{};
        else if (action >= 3)
            chosen = ipc::window_action::Move{quint8(action - 2)};
        DockApp::dispatch(m_menuWindows[window].address, chosen);
    };
    menu->onTogglePin = [this] { m_app.togglePin(m_model->menu()->itemId(), m_model->menu()->pinned()); };

    const QVariantMap properties{{QStringLiteral("dock"), QVariant::fromValue(m_model)}};
    setInitialProperties(properties);
    loadFromModule("RileyDock", "Dock");
    m_nameView->setInitialProperties(properties);
    m_nameView->loadFromModule("RileyDock", "HoverName");

    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, &DockWindow::onTimer);
    connect(this, &QQuickWindow::afterAnimating, this, &DockWindow::onAfterAnimating);
}

DockWindow::~DockWindow()
{
    delete m_nameView;
}

bool DockWindow::loaded() const
{
    return status() == QQuickView::Ready && rootObject() && m_nameView->status() == QQuickView::Ready
           && m_nameView->rootObject();
}

// --- frames ------------------------------------------------------------------------------------------------------

void DockWindow::beginLeave()
{
    QList<motion::Spring> springs;
    for (qsizetype i = 0; i < m_scales.size() && i < m_velocities.size(); ++i)
        springs.append(motion::Spring{m_scales[i], m_velocities[i], 1.0});
    m_leaving = std::pair{monotonicSeconds(), springs};
}

void DockWindow::rebuild(const QList<Icon> &icons, const QStringList &oldIds, const QStringList &ids,
                         bool launcherReset)
{
    QHash<QString, std::pair<double, double>> old;
    for (qsizetype i = 0; i < oldIds.size() && i < m_scales.size() && i < m_velocities.size(); ++i)
        old.insert(oldIds[i], {m_scales[i], m_velocities[i]});
    m_scales.clear();
    m_velocities.clear();
    for (const QString &id : ids) {
        const auto found = old.constFind(id);
        m_scales.append(found == old.constEnd() ? 1.0 : found->first);
        m_velocities.append(found == old.constEnd() ? 0.0 : found->second);
    }
    m_icons = icons;
    if (launcherReset) {
        m_gridAnimation = grid::Animation{};
        m_gridEpoch = monotonicSeconds();
        if (m_hovered == QStringLiteral("launcher"))
            m_hovered.reset();
    }
    if (m_hovered && !ids.contains(*m_hovered))
        m_hovered.reset();
    if (!m_pointer)
        beginLeave();
}

bool DockWindow::frame()
{
    // GTK synthesizes a motion event at the unchanged pointer position after every relayout, and the Rust view
    // relayouts on every scene update (riley dock/src/view.rs:234, queue_allocate), so on the next frame the Rust
    // dock runs pointer() again against the rectangles it last drew (dock/src/ui.rs:607-609, :705-725). Without it
    // an icon stays hovered (name label, shadow) after the swell has moved it out from under a still pointer.
    if (m_pointer) {
        const QPointF p = *m_pointer;
        pointer(p.x(), p.y());
    }
    const double now = monotonicSeconds();
    State &s = m_app.state();
    const bool reflections = s.reflections;
    const auto [config, height] = autohide::dockLayout(s.size, s.items.size(), grid::appCount(s.items), reflections);
    QStringList ids;
    for (const rules::DockItem &item : s.items)
        ids.append(item.id);
    const qsizetype found = ids.indexOf(QStringLiteral("launcher"));
    const std::optional<qsizetype> gridIndex = found >= 0 ? std::optional<qsizetype>(found) : std::nullopt;
    const bool swell = s.swell;
    const bool hide = s.autohide;
    const double clock = m_app.clock();
    const QList<bool> running = autohide::running(s.items);
    // Kept in step with the items by DockApp::refresh; guarded so a mismatch can never index out of range.
    m_icons.resize(ids.size());
    m_scales.resize(ids.size(), 1.0);
    m_velocities.resize(ids.size(), 0.0);

    const double dt = std::min(now - m_lastTick, 0.1);
    m_lastTick = now;
    m_autohide.distance = autohide::hiddenOffset(double(height), config.barY);
    m_autohide.advance(clock);
    const bool hidden = m_autohide.settledHidden();
    if (hidden) {
        // Nothing but the lip is visible: no invisible swell, grid or launch animation.
        m_pointer.reset();
        m_hovered.reset();
        m_leaving.reset();
        m_clicks.clear();
        m_launches.clear();
        std::fill(m_scales.begin(), m_scales.end(), 1.0);
        std::fill(m_velocities.begin(), m_velocities.end(), 0.0);
        m_gridAnimation = grid::Animation{};
    }
    bool active = false;
    const QList<double> ones(ids.size(), 1.0);
    if (swell) {
        if (m_pointer) {
            m_leaving.reset();
            const motion::Layout rest = grid::layout(config, ones, gridIndex);
            const QList<double> targets = grid::pointerScales(rest, m_pointer->x(), config.restWidth, gridIndex);
            for (qsizetype i = 0; i < targets.size(); ++i) {
                const double target = targets[i];
                if (gridIndex == i) {
                    m_scales[i] = 1.0;
                    m_velocities[i] = 0.0;
                    continue;
                }
                const double next = target + (m_scales[i] - target) * std::exp(-dt / 0.045);
                if (std::abs(next - target) < 0.0001) {
                    m_scales[i] = target;
                    m_velocities[i] = 0.0;
                } else {
                    m_velocities[i] = (target - next) / 0.045;
                    m_scales[i] = next;
                    active = true;
                }
            }
        } else if (m_leaving) {
            const auto &[start, springs] = *m_leaving;
            for (qsizetype i = 0; i < springs.size() && i < m_scales.size(); ++i) {
                if (gridIndex == i) {
                    m_scales[i] = 1.0;
                    m_velocities[i] = 0.0;
                    continue;
                }
                const motion::Sample sample = springs[i].sample(now - start);
                m_scales[i] = sample.value;
                m_velocities[i] = sample.velocity;
                active |= sample.active;
            }
            if (!active)
                m_leaving.reset();
        }
    } else {
        std::fill(m_scales.begin(), m_scales.end(), 1.0);
        std::fill(m_velocities.begin(), m_velocities.end(), 0.0);
        m_leaving.reset();
    }
    QList<double> scales = m_scales;
    for (qsizetype i = 0; i < ids.size(); ++i) {
        m_icons[i].bounce = 0.0;
        if (gridIndex == i)
            continue;
        if (const auto click = m_clicks.constFind(ids[i]); click != m_clicks.constEnd()) {
            const motion::Sample sample = motion::clickSquash(now - *click);
            scales[i] *= sample.value;
            active |= sample.active;
        }
        if (const auto launch = m_launches.constFind(ids[i]); launch != m_launches.constEnd()) {
            const double t = now - *launch;
            if (t < 2.0) {
                const double phase = std::fmod(t, 0.7) / 0.42;
                m_icons[i].bounce = phase < 1.0 ? 12.0 * std::sin(M_PI * phase) : 0.0;
                active = true;
            }
        }
    }
    m_clicks.removeIf([now](const auto &entry) { return !(now - entry.value() < 0.44); });
    m_launches.removeIf([now](const auto &entry) { return !(now - entry.value() < 2.0); });
    motion::Layout layout = grid::layout(config, scales, gridIndex);
    if (!swell && m_hovered) {
        const qsizetype i = ids.indexOf(*m_hovered);
        if (i >= 0 && gridIndex != i)
            layout.icons[i].y -= 6.0;
    }
    const motion::Layout rest = grid::layout(config, ones, gridIndex);
    m_restBar = rest.bar;
    m_surfaceHeight = double(height);
    QList<motion::InputRect> shownInput = layout.inputRegion();
    // Launch bounce also draws above the normal icon rectangle.
    for (qsizetype i = 0; i < m_icons.size() && i < layout.icons.size(); ++i) {
        if (m_icons[i].bounce > 0.0) {
            motion::Rect r = layout.icons[i];
            r.y -= m_icons[i].bounce;
            shownInput.append(r.outward());
        }
    }
    const int width = static_cast<int>(config.surfaceWidth);
    const motion::InputRect lip = autohide::lipRegion(rest.bar, double(height));
    const QList<motion::InputRect> input = m_autohide.inputRegion(clock, shownInput, lip, width, height);
    // Drawing, hit tests and names all use the translated geometry while the dock moves.
    const double offset = m_autohide.offset(clock);
    layout = autohide::translate(layout, offset);
    active |= m_autohide.animating();
    bool gridHovered = false;
    if (gridIndex && m_pointer) {
        const motion::Rect &r = layout.icons[*gridIndex];
        const double x = m_pointer->x(), y = m_pointer->y();
        gridHovered = x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
    }
    if (gridHovered && m_hovered != QStringLiteral("launcher"))
        m_hovered = QStringLiteral("launcher");
    else if (!gridHovered && m_hovered == QStringLiteral("launcher"))
        m_hovered.reset();
    const double time = now - m_gridEpoch;
    m_gridAnimation.setHover(gridHovered, time);
    const grid::Sample control = m_gridAnimation.sample(time);
    active |= control.active;
    for (qsizetype i = 0; i < m_icons.size(); ++i) {
        m_icons[i].grid = gridIndex == i ? std::optional<grid::Look>(control.look) : std::nullopt;
        m_icons[i].shadow = swell && gridIndex != i && m_hovered == ids[i];
    }
    m_rectangles = layout.icons;
    std::optional<motion::Rect> shelf;
    if (reflections)
        shelf = motion::shelf(layout, config.restBottom + offset, config.sidePadding);
    // Set only when the required value changes; the slide never touches the reservation. The ONE place the dock's
    // exclusive zone is set: autohide::exclusiveZone gives LIP (6 px) with auto-hide on, else the bar plus 8 px.
    const int zone = autohide::exclusiveZone(hide, config.barHeight);
    if (m_zone != zone) {
        m_layer->setExclusiveZone(zone);
        m_zone = zone;
    }
    QList<motion::Point> lipDots;
    if (hidden)
        lipDots = autohide::lipDots(rest, running, double(height));
    const int lookup = rules::iconLookupSize(std::max(rules::drawnSize(s.size, grid::appCount(s.items)), quint32(1)));
    update(Scene{width, height, layout, shelf, input, hidden, lipDots, m_icons,
                 config.restBottom + offset + motion::reflectionPadding(config.restWidth, reflections) + 6.0, offset},
           config, lookup);
    updateName();
    const bool menu = s.menuCount > 0;
    m_autohide.setBlockers(menu, nameVisible(), clock);
    active |= m_autohide.animating();
    schedule();
    if (control.launch)
        DockApp::launchLauncher();
    return active;
}

void DockWindow::update(const Scene &scene, const motion::LayoutConfig &config, int lookupSize)
{
    if (QQuickView::width() != scene.width || QQuickView::height() != scene.height)
        resize(scene.width, scene.height);
    QRegion region;
    for (const motion::InputRect &r : scene.input)
        region += QRect(r.x, r.y, r.width, r.height);
    if (region != m_mask || m_maskEmpty != region.isEmpty()) {
        m_mask = region;
        m_maskEmpty = region.isEmpty();
        applyInputRegion();
    }

    DockModel::Scene out;
    out.surfaceWidth = scene.width;
    out.surfaceHeight = scene.height;
    out.bar = toQt(scene.layout.bar);
    out.hidden = scene.hidden;
    out.offset = scene.offset;
    for (const motion::Point &dot : scene.lipDots)
        out.lipDots.append(QPointF(dot.first, dot.second));
    out.shelfVisible = scene.shelf.has_value() && !scene.hidden;
    if (scene.shelf)
        out.shelf = toQt(*scene.shelf);
    out.indicatorY = scene.indicatorY;
    out.reflections = scene.shelf.has_value();
    out.swell = m_app.state().swell;
    out.restWidth = config.restWidth;
    out.iconLookupSize = lookupSize;

    QList<ItemData> rows;
    const State &s = m_app.state();
    for (qsizetype i = 0; i < scene.icons.size() && i < scene.layout.icons.size(); ++i) {
        const Icon &icon = scene.icons[i];
        const motion::Rect &rect = scene.layout.icons[i];
        ItemData row;
        row.itemId = i < s.items.size() ? s.items[i].id : QString();
        row.name = icon.name;
        row.iconSource = icon.iconSource;
        row.symbolic = icon.symbolic;
        row.launcher = icon.grid.has_value();
        row.running = i < s.items.size() && !s.items[i].windows.isEmpty();
        row.focused = icon.active;
        row.minimized = icon.indicator == rules::Indicator::Hollow;
        row.indicator = int(icon.indicator);
        // riley dock/src/view.rs:144-146
        switch (icon.indicator) {
        case rules::Indicator::None: break;
        case rules::Indicator::Two: row.dots = {-3.0, 3.0}; break;
        default: row.dots = {0.0}; break;
        }
        row.rect = toQt(rect);
        motion::Rect drawn = rect;
        drawn.y -= icon.bounce;
        row.drawRect = toQt(drawn);
        row.bounce = icon.bounce;
        row.scale = config.restWidth > 0 ? rect.width / config.restWidth : 1.0;
        row.hovered = m_hovered && *m_hovered == row.itemId;
        row.shadow = icon.shadow;
        // riley dock/src/view.rs:101-105: no reflection for the launcher (its mirrored dots read as running-app
        // indicators), none while hidden.
        if (scene.shelf && !scene.hidden && !row.launcher) {
            const motion::Reflection reflected = motion::reflection(drawn, scene.shelf->y);
            row.reflected = true;
            row.reflectionImage = toQt(reflected.image);
            row.reflectionClip = toQt(reflected.visible);
            row.reflectionFlipY = reflected.flipY;
            for (const motion::Point &stop : reflected.stops)
                row.reflectionStops.append(QPointF(stop.first, stop.second));
        }
        if (icon.grid) {
            // riley dock/src/view.rs:161-171: the dots in the icon's own coordinates.
            const auto [dots, radius] = grid::dots(motion::Rect{0, 0, rect.width, rect.height}, *icon.grid);
            for (const motion::Point &dot : dots)
                row.gridDots.append(QPointF(dot.first, dot.second));
            row.gridDotRadius = radius;
            row.gridAccent = icon.grid->accent;
            row.gridSpacing = icon.grid->spacing;
        }
        rows.append(row);
    }
    // riley dock/src/view.rs:68-77: the line under the focused app, a 20 x 3 px box centred under its icon. GTK
    // draws that box inside its CSS margin, `.dock-item { margin: 0 1px }` (riley dock/style.example.css:10), so
    // the line itself is 18 px wide, 1 px in from each side; the Rust dock's DP-2 capture measures 22.5 device
    // pixels at scale 1.25, which is 18.0 px.
    for (qsizetype i = 0; i < scene.icons.size() && i < scene.layout.icons.size(); ++i) {
        if (!scene.icons[i].active)
            continue;
        const motion::Rect &rect = scene.layout.icons[i];
        out.activeLineVisible = !scene.hidden;
        out.activeLine = QRectF(rect.x + rect.width / 2.0 - 10.0 + 1.0, scene.indicatorY + 4.0, 18.0, 3.0);
        break;
    }
    m_model->setScene(out, rows);
}

void DockWindow::applyInputRegion()
{
    // riley dock/src/view.rs:226-232: the surface's input region is exactly the core's rectangles. An empty
    // QRegion means "the whole window" to QWindow::setMask, so an empty region is made transparent for input.
    if (m_maskEmpty) {
        setFlag(Qt::WindowTransparentForInput, true);
        return;
    }
    if (flags().testFlag(Qt::WindowTransparentForInput))
        setFlag(Qt::WindowTransparentForInput, false);
    setMask(m_mask);
}

void DockWindow::wake()
{
    if (m_ticking)
        return;
    m_ticking = true;
    m_lastTick = monotonicSeconds() - 0.016;
    QQuickView::update();
}

void DockWindow::onAfterAnimating()
{
    // One tick of the frame clock (gtk add_tick_callback): the dock's motion, then the acknowledgement watch.
    bool more = false;
    if (m_ticking) {
        if (frame())
            more = true;
        else
            m_ticking = false;
    }
    if (m_ackWatch) {
        m_ackWatch = m_app.ackTick();
        more |= m_ackWatch;
    }
    if (more)
        QMetaObject::invokeMethod(this, [this] { QQuickView::update(); }, Qt::QueuedConnection);
}

void DockWindow::startAckWatch()
{
    m_ackWatch = true;
    QQuickView::update();
}

void DockWindow::schedule()
{
    const double now = m_app.clock();
    const std::optional<double> deadline = m_autohide.nextDeadline();
    if (m_timerAt == deadline)
        return;
    m_timer.stop();
    m_timerAt.reset();
    if (!deadline)
        return;
    const double micros = std::ceil(std::max(*deadline - now, 0.0) * 1e6);
    m_timerAt = deadline;
    m_timer.start(std::chrono::milliseconds(qint64(std::ceil(micros / 1000.0))));
}

void DockWindow::onTimer()
{
    if (!m_timerAt)
        return;
    const double at = *m_timerAt;
    m_timerAt.reset(); // this timer is finished
    m_autohide.advance(std::max(m_app.clock(), at));
    const bool moving = m_autohide.animating();
    schedule();
    if (moving)
        wake();
}

// --- input -------------------------------------------------------------------------------------------------------

autohide::Zone DockWindow::zoneAt(double x, double y) const
{
    // The activation band counts only while it takes input, and only under the rest bar's span.
    const motion::Rect &bar = m_restBar;
    const bool band = m_autohide.bandInput() && y >= m_surfaceHeight - autohide::LIP && x >= bar.x
                      && x < bar.x + bar.width;
    return band ? autohide::Zone::Band : autohide::Zone::Dock;
}

std::optional<qsizetype> DockWindow::hit(double x, double y) const
{
    for (qsizetype i = 0; i < m_rectangles.size(); ++i) {
        const motion::Rect &r = m_rectangles[i];
        const double top = r.y - (i < m_icons.size() ? m_icons[i].bounce : 0.0);
        if (x >= r.x && x < r.x + r.width && y >= top && y < top + r.height)
            return i;
    }
    return std::nullopt;
}

void DockWindow::pointer(double x, double y)
{
    const State &s = m_app.state();
    const double now = m_app.clock();
    const autohide::Zone zone = zoneAt(x, y);
    // A held activation band feeds no position into icon or grid hover.
    std::optional<QString> hovered;
    if (zone != autohide::Zone::Band) {
        if (const auto i = hit(x, y); i && *i < s.items.size())
            hovered = s.items[*i].id;
    }
    const bool wasHidden = m_autohide.settledHidden();
    m_autohide.pointer(zone, now);
    const bool quiet = wasHidden && m_autohide.settledHidden();
    if (zone == autohide::Zone::Band) {
        if (m_pointer)
            beginLeave();
        m_pointer.reset();
    } else {
        m_pointer = QPointF(x, y);
    }
    m_gridAnimation.setHover(hovered == QStringLiteral("launcher"), monotonicSeconds() - m_gridEpoch);
    m_hovered = hovered;
    schedule();
    // No frame work during the dwell itself: the hidden dock looks the same until its timer fires.
    if (!quiet)
        wake();
}

void DockWindow::leave()
{
    const double now = m_app.clock();
    const bool menu = m_app.state().menuCount > 0;
    m_pointer.reset();
    m_hovered.reset();
    m_gridAnimation.setHover(false, monotonicSeconds() - m_gridEpoch);
    hideName();
    beginLeave();
    m_autohide.setBlockers(menu, false, now);
    m_autohide.pointer(autohide::Zone::Outside, now);
    const bool quiet = m_autohide.settledHidden();
    schedule();
    if (!quiet)
        wake();
}

void DockWindow::pressed(int button, double x, double y)
{
    const State &s = m_app.state();
    // The activation band only reveals: it never launches, focuses or minimizes.
    if (zoneAt(x, y) == autohide::Zone::Band || m_autohide.settledHidden())
        return;
    const auto i = hit(x, y);
    if (!i || *i >= s.items.size())
        return;
    const QString id = s.items[*i].id;
    if (id == QStringLiteral("launcher")) {
        if (button == 1 || button == 2)
            m_gridAnimation.press(monotonicSeconds() - m_gridEpoch);
    } else {
        m_clicks.insert(id, monotonicSeconds());
    }
    wake();
}

void DockWindow::released(int button, double x, double y)
{
    const State &s = m_app.state();
    const bool band = zoneAt(x, y) == autohide::Zone::Band || m_autohide.settledHidden();
    std::optional<qsizetype> hitIndex = band ? std::nullopt : hit(x, y);
    if (hitIndex && *hitIndex >= s.items.size())
        hitIndex.reset();
    if (m_gridAnimation.isPressed()) {
        const bool activate = hitIndex && s.items[*hitIndex].id == QStringLiteral("launcher")
                              && (button == 1 || button == 2);
        m_gridAnimation.release(monotonicSeconds() - m_gridEpoch, activate);
        wake();
        return;
    }
    if (!hitIndex)
        return;
    const int i = int(*hitIndex);
    const rules::DockItem item = s.items[i];
    const QString active = s.snapshot.active;
    const bool launcher = item.id == QStringLiteral("launcher");
    if ((button == 1 || button == 2) && launcher)
        return;
    if (button == 1) {
        const rules::Action action = rules::leftClick(item.id, item.windows, active);
        if (std::holds_alternative<rules::action::Menu>(action))
            m_app.openWindowMenu(m_index, i, item.windows);
        else
            m_app.perform(action, m_index);
    } else if (button == 2) {
        m_app.perform(rules::action::Launch{item.id}, m_index);
    } else if (button == 3 && !launcher) {
        m_app.openContextMenu(m_index, i, item);
    }
}

void DockWindow::cancelGrid()
{
    if (!m_gridAnimation.isPressed())
        return;
    m_gridAnimation.release(monotonicSeconds() - m_gridEpoch, false);
    wake();
}

bool DockWindow::event(QEvent *event)
{
    switch (event->type()) {
    case QEvent::Enter: {
        const QPointF p = static_cast<QEnterEvent *>(event)->position();
        pointer(p.x(), p.y());
        break;
    }
    case QEvent::MouseMove: {
        const QPointF p = static_cast<QMouseEvent *>(event)->position();
        pointer(p.x(), p.y());
        break;
    }
    case QEvent::Leave:
        leave();
        break;
    case QEvent::MouseButtonPress: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        pressed(gtkButton(mouse->button()), mouse->position().x(), mouse->position().y());
        break;
    }
    case QEvent::MouseButtonRelease: {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        released(gtkButton(mouse->button()), mouse->position().x(), mouse->position().y());
        break;
    }
    case QEvent::UngrabMouse: // the press was taken away (GtkGestureClick::cancel, ui.rs:690)
        cancelGrid();
        break;
    default:
        break;
    }
    return QQuickView::event(event);
}

void DockWindow::exposeEvent(QExposeEvent *event)
{
    QQuickView::exposeEvent(event);
    if (!isExposed())
        return;
    if (!m_exposedOnce) {
        // The input region of a surface made before this window was shown: apply it to the new surface.
        m_exposedOnce = true;
        if (!m_maskEmpty) {
            setMask(QRegion());
            setMask(m_mask);
        }
    }
    // riley dock/src/ui.rs:693 connect_map -> wake.
    if (m_ticking)
        QQuickView::update();
    else
        wake();
}

// --- hover name --------------------------------------------------------------------------------------------------

bool DockWindow::nameVisible() const
{
    return m_nameView->isVisible();
}

void DockWindow::hideName()
{
    if (m_nameView->isVisible())
        m_nameView->setVisible(false);
    m_model->setHover(m_model->hoverText(), false);
}

void DockWindow::updateName()
{
    const State &s = m_app.state();
    if (s.menuCount > 0)
        return hideName();
    qsizetype i = -1;
    for (qsizetype n = 0; n < s.items.size(); ++n) {
        if (m_hovered && s.items[n].id == *m_hovered) {
            i = n;
            break;
        }
    }
    if (i < 0)
        return hideName();
    if (i >= m_rectangles.size())
        return;
    const motion::Rect rect = m_rectangles[i];
    const rules::DockItem &item = s.items[i];
    const bool launcher = item.id == QStringLiteral("launcher");
    QString name = QStringLiteral("Applications");
    if (!launcher) {
        const auto info = desktop::appInfo(item.id);
        name = info ? info->name : item.id;
    }
    m_model->setHover(rules::hoverText(name, item.windows), m_model->hoverVisible());
    QScreen *monitor = screen();
    QQuickItem *label = m_nameView->rootObject();
    if (!monitor || !label)
        return;
    const int natural = int(std::ceil(label->implicitWidth()));
    const int naturalHeight = int(std::ceil(label->implicitHeight()));
    const int monitorWidth = monitor->geometry().width();
    const int x = (monitorWidth - QQuickView::width()) / 2 + static_cast<int>(rect.x + rect.width / 2.0) - natural / 2;
    const int left = std::min(std::max(x, 0), std::max(monitorWidth - natural, 0));
    // The lower grid must not put its label over neighbouring application icons.
    double top = rect.y;
    if (launcher) {
        for (const motion::Rect &r : m_rectangles)
            top = std::min(top, r.y);
    }
    const int bottom = QQuickView::height() - static_cast<int>(std::floor(top)) + 6;
    const QSize size(std::max(natural, 1), std::max(naturalHeight, 1));
    if (m_nameView->size() != size)
        m_nameView->resize(size);
    const QMargins margins(left, 0, 0, bottom);
    if (margins != m_nameMargins) {
        m_nameMargins = margins;
        m_nameLayer->setMargins(margins);
    }
    if (!m_nameView->isVisible())
        m_nameView->setVisible(true);
    m_model->setHover(m_model->hoverText(), true);
}

// --- screens -----------------------------------------------------------------------------------------------------

void DockWindow::placeOn(QScreen *target)
{
    if (!target) {
        if (isVisible()) {
            log(QStringLiteral("screen %1 gone, dock hidden").arg(m_output));
            setVisible(false);
            m_nameView->setVisible(false);
        }
        return;
    }
    if (isVisible() && screen() == target)
        return;
    log(QStringLiteral("screen %1 back, dock shown").arg(m_output));
    setVisible(false);
    m_nameView->setVisible(false);
    setScreen(target);
    m_layer->setScreen(target);
    m_nameView->setScreen(target);
    m_nameLayer->setScreen(target);
    setVisible(true);
}

} // namespace riley::dock::app
