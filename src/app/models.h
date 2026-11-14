// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-dock-qt: what QML sees. One DockModel per dock window, holding the scene of the last frame (riley
// dock/src/view.rs:17-29 struct Scene) with every rectangle, scale, colour and gradient stop already computed by the
// core. QML draws; it never derives a position, size or timing of its own. docs/qml-model.md describes each property.
#pragma once

#include "app/style.h"

#include <QtCore/QAbstractListModel>
#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QPointF>
#include <QtCore/QRect>
#include <QtCore/QRectF>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariantList>
#include <QtQml/qqmlregistration.h>

#include <functional>

namespace riley::dock::app {

// One row of DockItems: one dock slot (riley dock/src/view.rs:7-14 struct Icon, plus its rectangle in the frame).
struct ItemData {
    QString itemId;          // "launcher", a pin id or a window class
    QString name;            // "Applications" for the launcher, else the desktop entry's Name, else itemId
    QString iconSource;      // "image://dockicon/..." ("" for the launcher)
    bool symbolic = false;   // the icon name ends in "-symbolic": paint it in style.barForeground
    bool launcher = false;   // the app-grid button (Launcher.qml), never an app icon
    bool running = false;    // has windows (minimized ones count)
    bool pinned = false;     // its id is in the pins file (the launcher: false); picks the running dots' colour
    bool focused = false;    // one of its windows is the active window
    bool minimized = false;  // every window is minimized (indicator Hollow)
    int indicator = 0;       // rules::Indicator: 0 None, 1 One, 2 Two, 3 Hollow
    QVariantList dots;       // x offsets of the running dots from the icon centre: [], [0] or [-3, 3]
    QRectF rect;             // icon rectangle after swell, squash and the auto-hide slide (no bounce)
    QRectF drawRect;         // where the icon is painted: rect moved up by bounce
    double bounce = 0;       // launch bounce in pixels, upwards
    double scale = 1;        // swell times click squash (rect.width / restWidth for app icons)
    bool hovered = false;    // under the pointer (the hover name belongs to it)
    bool shadow = false;     // paint the hover shadow (DockModel.shadow*)
    bool reflected = false;  // paint a reflection (reflections on, not hidden, not the launcher)
    QRectF reflectionImage;  // motion::Reflection.image: where the mirrored icon is painted
    QRectF reflectionClip;   // motion::Reflection.visible: the clip, and the gradient's extent (top to bottom)
    double reflectionFlipY = 0;   // motion::Reflection.flip_y: paint at (image.x, flipY) scaled (1, -1), view.rs:118-121
    QVariantList reflectionStops; // [QPointF(position 0..1, alpha), ...]: the alpha mask from clip top to bottom
    QVariantList gridDots;   // launcher only: the nine dot centres in icon-local coordinates (grid::dots)
    double gridDotRadius = 0;
    double gridAccent = 0;   // launcher only: 0 = style.barForeground, 1 = style.accent, mixed per channel
    double gridSpacing = 1;  // launcher only: grid::Look.spacing (already applied to gridDots)
    bool operator==(const ItemData &) const = default;
};

class DockItems : public QAbstractListModel {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the dock")
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        ItemIdRole = Qt::UserRole + 1, NameRole, IconSourceRole, SymbolicRole, LauncherRole, RunningRole,
        FocusedRole, MinimizedRole, IndicatorRole, DotsRole, RectRole, DrawRectRole, BounceRole, ScaleRole,
        HoveredRole, ShadowRole, ReflectedRole, ReflectionImageRole, ReflectionClipRole, ReflectionFlipYRole,
        ReflectionStopsRole, GridDotsRole, GridDotRadiusRole, GridAccentRole, GridSpacingRole,
        PinnedRole,
    };
    Q_ENUM(Role)

    using QAbstractListModel::QAbstractListModel;
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return int(m_rows.size()); }

    // A new list: reset when the ids changed, else dataChanged for the rows that differ.
    void set(const QList<ItemData> &rows);

Q_SIGNALS:
    void countChanged();

private:
    QList<ItemData> m_rows;
};

// The right-click menu (riley dock/src/ui.rs:808-851 context_menu) and the left-click window list for an app with
// several windows (ui.rs:792-806 window_menu), at most one open per dock. QML shows it while `open`; every way of
// closing it ends in exactly one call of pickWindow, windowAction, togglePin or dismiss.
class DockMenu : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the dock")
    Q_PROPERTY(bool open READ open NOTIFY changed)
    Q_PROPERTY(QString kind READ kind NOTIFY changed)         // "windows" (Minimized.qml) or "context" (Menu.qml)
    Q_PROPERTY(QRect anchor READ anchor NOTIFY changed)       // the icon's rectangle, outward, surface coordinates
    Q_PROPERTY(QString itemId READ itemId NOTIFY changed)
    Q_PROPERTY(QVariantList windows READ windows NOTIFY changed) // [{line, address, minimized}]
    Q_PROPERTY(bool pinned READ pinned NOTIFY changed)        // context: the item is pinned now
    Q_PROPERTY(QString pinLabel READ pinLabel NOTIFY changed) // context: "Unpin" or "Pin"
    Q_PROPERTY(QStringList actions READ actions CONSTANT)     // context: the per-window actions, in order

public:
    using QObject::QObject;

    bool open() const { return m_open; }
    QString kind() const { return m_kind; }
    QRect anchor() const { return m_anchor; }
    QString itemId() const { return m_itemId; }
    QVariantList windows() const { return m_windows; }
    bool pinned() const { return m_pinned; }
    QString pinLabel() const { return m_pinned ? QStringLiteral("Unpin") : QStringLiteral("Pin"); }
    QStringList actions() const;

    // window list: pick window `index` (rules::menuPick: restore a minimized window, else focus it).
    Q_INVOKABLE void pickWindow(int index);
    // context menu: run action `action` (index into `actions`) on window `window`.
    Q_INVOKABLE void windowAction(int window, int action);
    // context menu: Pin or Unpin the item.
    Q_INVOKABLE void togglePin();
    // closed without a choice (click outside, Escape, the popup dismissed). Does nothing when not open.
    Q_INVOKABLE void dismiss();

    // C++ side.
    void show(const QString &kind, const QRect &anchor, const QString &itemId, const QVariantList &windows,
              bool pinned);
    void hide();
    std::function<void(int)> onPick;
    std::function<void(int, int)> onWindowAction;
    std::function<void()> onTogglePin;
    std::function<void()> onClosed; // called once whenever an open menu closes, before any action runs

Q_SIGNALS:
    void changed();

private:
    bool m_open = false;
    QString m_kind;
    QRect m_anchor;
    QString m_itemId;
    QVariantList m_windows;
    bool m_pinned = false;
};

class DockModel : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Owned by the dock")

    // The scene of the last frame (riley dock/src/view.rs:17-29). All change together with sceneChanged.
    Q_PROPERTY(int surfaceWidth READ surfaceWidth NOTIFY sceneChanged)
    Q_PROPERTY(int surfaceHeight READ surfaceHeight NOTIFY sceneChanged)
    Q_PROPERTY(QRectF bar READ bar NOTIFY sceneChanged)
    Q_PROPERTY(bool hidden READ hidden NOTIFY sceneChanged)
    Q_PROPERTY(double offset READ offset NOTIFY sceneChanged)
    Q_PROPERTY(QVariantList lipDots READ lipDots NOTIFY sceneChanged)
    Q_PROPERTY(bool shelfVisible READ shelfVisible NOTIFY sceneChanged)
    Q_PROPERTY(QRectF shelf READ shelf NOTIFY sceneChanged)
    Q_PROPERTY(double indicatorY READ indicatorY NOTIFY sceneChanged)
    Q_PROPERTY(bool activeLineVisible READ activeLineVisible NOTIFY sceneChanged)
    Q_PROPERTY(QRectF activeLine READ activeLine NOTIFY sceneChanged)
    Q_PROPERTY(bool reflections READ reflections NOTIFY sceneChanged)
    Q_PROPERTY(bool swell READ swell NOTIFY sceneChanged)
    Q_PROPERTY(double restWidth READ restWidth NOTIFY sceneChanged)
    Q_PROPERTY(int iconLookupSize READ iconLookupSize NOTIFY sceneChanged)

    // The hover name (riley dock/src/ui.rs:727-749), shown in its own window by HoverName.qml.
    Q_PROPERTY(QString hoverText READ hoverText NOTIFY hoverChanged)
    Q_PROPERTY(bool hoverVisible READ hoverVisible NOTIFY hoverChanged)

    // Fixed drawing constants of the Rust view (riley dock/src/view.rs), so QML has no numbers of its own.
    Q_PROPERTY(double dotRadius READ dotRadius CONSTANT)             // view.rs:149 running dot radius 2
    Q_PROPERTY(double hollowLineWidth READ hollowLineWidth CONSTANT) // view.rs:150 hollow dot stroke 1.2
    Q_PROPERTY(double lipDotRadius READ lipDotRadius CONSTANT)       // view.rs:96 autohide::DOT_RADIUS
    Q_PROPERTY(QColor shadowColor READ shadowColor CONSTANT)         // view.rs:131-132 rgba(0, 0, 0, 0.35)
    Q_PROPERTY(double shadowOffsetX READ shadowOffsetX CONSTANT)     // 0
    Q_PROPERTY(double shadowOffsetY READ shadowOffsetY CONSTANT)     // 6
    Q_PROPERTY(double shadowRadius READ shadowRadius CONSTANT)       // 10 (GSK blur radius)

    Q_PROPERTY(DockItems *items READ items CONSTANT)
    Q_PROPERTY(DockMenu *menu READ menu CONSTANT)
    Q_PROPERTY(DockStyle *style READ style CONSTANT)
    Q_PROPERTY(QString output READ output CONSTANT)

public:
    DockModel(DockStyle *style, QString output, QObject *parent = nullptr);

    struct Scene {
        int surfaceWidth = 1;
        int surfaceHeight = 1;
        QRectF bar;
        bool hidden = false;
        double offset = 0;
        QVariantList lipDots;
        bool shelfVisible = false;
        QRectF shelf;
        double indicatorY = 0;
        bool activeLineVisible = false;
        QRectF activeLine;
        bool reflections = false;
        bool swell = true;
        double restWidth = 0;
        int iconLookupSize = 0;
        bool operator==(const Scene &) const = default;
    };
    void setScene(const Scene &scene, const QList<ItemData> &items);
    void setHover(const QString &text, bool visible);

    int surfaceWidth() const { return m_scene.surfaceWidth; }
    int surfaceHeight() const { return m_scene.surfaceHeight; }
    QRectF bar() const { return m_scene.bar; }
    bool hidden() const { return m_scene.hidden; }
    double offset() const { return m_scene.offset; }
    QVariantList lipDots() const { return m_scene.lipDots; }
    bool shelfVisible() const { return m_scene.shelfVisible; }
    QRectF shelf() const { return m_scene.shelf; }
    double indicatorY() const { return m_scene.indicatorY; }
    bool activeLineVisible() const { return m_scene.activeLineVisible; }
    QRectF activeLine() const { return m_scene.activeLine; }
    bool reflections() const { return m_scene.reflections; }
    bool swell() const { return m_scene.swell; }
    double restWidth() const { return m_scene.restWidth; }
    int iconLookupSize() const { return m_scene.iconLookupSize; }
    QString hoverText() const { return m_hoverText; }
    bool hoverVisible() const { return m_hoverVisible; }
    double dotRadius() const { return 2.0; }
    double hollowLineWidth() const { return 1.2; }
    double lipDotRadius() const;
    QColor shadowColor() const { return QColor::fromRgbF(0, 0, 0, 0.35f); }
    double shadowOffsetX() const { return 0; }
    double shadowOffsetY() const { return 6; }
    double shadowRadius() const { return 10; }
    DockItems *items() { return &m_items; }
    DockMenu *menu() { return &m_menu; }
    DockStyle *style() { return m_style; }
    QString output() const { return m_output; }

Q_SIGNALS:
    void sceneChanged();
    void hoverChanged();

private:
    DockStyle *m_style;
    QString m_output;
    Scene m_scene;
    DockItems m_items;
    DockMenu m_menu;
    QString m_hoverText;
    bool m_hoverVisible = false;
};

} // namespace riley::dock::app
