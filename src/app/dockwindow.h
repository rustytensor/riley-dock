// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-dock-qt: one dock on one screen. A Qt Quick window on a wlr-layer-shell surface (layer top, bottom anchor,
// namespace "riley-dock", no keyboard), plus its hover-name surface. Mirrors riley dock/src/ui.rs struct Dock
// (:17-41) and the per-dock functions frame (:384-516), schedule (:519-543), wake (:367-382), zone_at, hit,
// connect_input, pointer and update_name (:545-749), and DockView::update (riley dock/src/view.rs:223-236).
#pragma once

#include "app/dockapp.h"
#include "app/models.h"

#include <riley-dock/autohide.h>
#include <riley-dock/grid.h>
#include <riley-dock/motion.h>

#include <QtCore/QHash>
#include <QtCore/QPointF>
#include <QtCore/QTimer>
#include <QtGui/QRegion>
#include <QtQuick/QQuickView>

#include <optional>
#include <utility>

namespace LayerShellQt {
class Window;
}

namespace riley::dock::app {

class DockWindow : public QQuickView {
    Q_OBJECT
public:
    DockWindow(DockApp &app, int index, QString output, QScreen *screen, bool autohide);
    ~DockWindow() override;

    bool loaded() const;
    const QString &output() const { return m_output; }
    DockModel *model() { return m_model; }

    // riley dock/src/ui.rs:384 frame: one frame of motion; true while more frames are needed.
    bool frame();
    void wake();       // ui.rs:367: frames until frame() returns false
    void schedule();   // ui.rs:519: the one-shot timer for the next dwell or hide deadline
    void updateName(); // ui.rs:727
    void hideName();
    bool nameVisible() const;
    void beginLeave(); // ui.rs:362
    // Move this dock to `screen` (ui.rs:269-276) or hide it while its screen is gone (ui.rs:264-268).
    void placeOn(QScreen *screen);
    void startAckWatch();

    autohide::Controller &autohide() { return m_autohide; }
    const QList<motion::Rect> &rectangles() const { return m_rectangles; }

    // Called by DockApp::refresh on a rebuild (ui.rs:335-348).
    void rebuild(const QList<Icon> &icons, const QStringList &oldIds, const QStringList &ids, bool launcherReset);

protected:
    bool event(QEvent *event) override;
    void exposeEvent(QExposeEvent *event) override;

private:
    struct Scene { // riley dock/src/view.rs:17-29
        int width = 1;
        int height = 1;
        motion::Layout layout;
        std::optional<motion::Rect> shelf;
        QList<motion::InputRect> input;
        bool hidden = false;
        QList<motion::Point> lipDots;
        QList<Icon> icons;
        double indicatorY = 0;
        double offset = 0; // the auto-hide slide this frame was laid out with
    };
    void update(const Scene &scene, const motion::LayoutConfig &config, int lookupSize); // view.rs:223
    void applyInputRegion();
    void onAfterAnimating();
    void onTimer();
    autohide::Zone zoneAt(double x, double y) const;               // ui.rs:546
    std::optional<qsizetype> hit(double x, double y) const;        // ui.rs:596
    void pointer(double x, double y);                              // ui.rs:705
    void leave();                                                  // ui.rs:615-634
    void pressed(int button, double x, double y);                  // ui.rs:639-656
    void released(int button, double x, double y);                 // ui.rs:658-688
    void cancelGrid();                                             // ui.rs:696

    DockApp &m_app;
    int m_index;
    QString m_output;
    LayerShellQt::Window *m_layer = nullptr;
    DockModel *m_model = nullptr;
    QQuickView *m_nameView = nullptr;
    LayerShellQt::Window *m_nameLayer = nullptr;
    QMargins m_nameMargins;

    QList<Icon> m_icons;
    std::optional<QString> m_hovered;
    std::optional<QPointF> m_pointer;
    QList<double> m_scales;
    QList<double> m_velocities;
    std::optional<std::pair<double, QList<motion::Spring>>> m_leaving;
    QHash<QString, double> m_clicks;
    QHash<QString, double> m_launches;
    QList<motion::Rect> m_rectangles;
    bool m_ticking = false;
    double m_lastTick = 0;
    grid::Animation m_gridAnimation;
    double m_gridEpoch = 0;
    autohide::Controller m_autohide;
    QTimer m_timer;
    std::optional<double> m_timerAt;
    std::optional<int> m_zone;
    motion::Rect m_restBar;
    double m_surfaceHeight = 0;

    QRegion m_mask;
    bool m_maskEmpty = false;
    bool m_exposedOnce = false;
    bool m_ackWatch = false;
    QList<rules::Client> m_menuWindows; // the windows of the open menu, in its row order

    friend class DockApp;
};

} // namespace riley::dock::app
