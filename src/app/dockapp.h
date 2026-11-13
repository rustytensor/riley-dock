// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-dock-qt: the state every dock shares and everything that is not one window. Mirrors riley dock/src/ui.rs
// struct State (:43-64), start (:70-251), place (:253-280), refresh (:287-360), show_docks and reply (:553-594), the
// menus (:751-851), perform and dispatch (:862-901).
#pragma once

#include "app/style.h"

#include <riley-dock/control.h>
#include <riley-dock/grid.h>
#include <riley-dock/ipc.h>
#include <riley-dock/rules.h>

#include <QtCore/QList>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <memory>
#include <optional>
#include <vector>

class QQmlEngine;
class QSocketNotifier;

namespace riley::dock::app {

class DockWindow;
class FileWatch;
class DesktopEntriesWatch;

// riley dock/src/ui.rs:15 type Signature = Vec<(String, rules::Indicator, bool)>.
struct SignatureEntry {
    QString id;
    rules::Indicator indicator = rules::Indicator::None;
    bool active = false;
    bool operator==(const SignatureEntry &) const = default;
};

// riley dock/src/view.rs:7-14 struct Icon, with the icon resolved for QML instead of a GTK paintable.
struct Icon {
    QString iconSource;              // paintable: "" for the launcher
    bool symbolic = false;
    QString name;                    // the hover name's first line (ui.rs:735-736)
    std::optional<grid::Look> grid;  // set only for the launcher
    rules::Indicator indicator = rules::Indicator::None;
    bool active = false;
    bool shadow = false;
    double bounce = 0;
};

// riley dock/src/ui.rs:43-64 struct State (the docks are DockApp::docks()).
struct State {
    ipc::Snapshot snapshot;
    std::optional<ipc::Snapshot> pending;
    qsizetype menuCount = 0;
    QList<rules::DockItem> items;
    QList<SignatureEntry> signature;
    QStringList pinned;
    QString pinsPath;
    QString configPath;
    QString stylePath;
    quint32 size = 44;
    bool swell = true;
    bool reflections = true;
    bool reflectionsAllowed = true;
    grid::Position launcher = grid::Position::Left;
    bool refreshIcons = true;
    bool autohide = false;
    double epoch = 0;
    std::vector<ipc::UnixStream> acks;
    bool ackWatch = false;
};

class DockApp : public QObject {
    Q_OBJECT
public:
    DockApp(bool swell, bool reflectionsAllowed, std::optional<QString> controlPath, QObject *parent = nullptr);
    ~DockApp() override;

    // riley dock/src/ui.rs:70 start. False when there is no dock to run (the caller exits).
    bool start(const QStringList &outputs);

    State &state() { return m_state; }
    const QList<DockWindow *> &docks() const { return m_docks; }
    DockStyle *style() { return &m_style; }
    QQmlEngine *engine() { return m_engine; }

    // riley dock/src/ui.rs:67 State::clock: seconds since the dock started, the auto-hide clock.
    double clock() const;

    void refresh();                                         // ui.rs:287
    void showDocks(std::optional<ipc::UnixStream> ack);     // ui.rs:553
    // One tick of the acknowledgement watch (ui.rs:572-584), run on the first visible dock's frames. True while
    // the watch needs more frames.
    bool ackTick();
    void openWindowMenu(int dock, int item, const QList<rules::Client> &windows);  // ui.rs:792
    void openContextMenu(int dock, int item, const rules::DockItem &dockItem);    // ui.rs:808
    void menuClosed();                                                            // ui.rs:769-788
    void perform(const rules::Action &action, int dock);                          // ui.rs:862
    static void dispatch(const QString &address, const ipc::WindowAction &action);  // ui.rs:887
    static void launchLauncher();                                                 // ui.rs:853
    void togglePin(const QString &id, bool pinned);                               // ui.rs:836-848

    // Quit cleanup (ui.rs:244-250): drop the control endpoint so its socket is unlinked, close the name windows.
    void shutdown();

private:
    void place();          // ui.rs:253
    void applyUpdates();   // ui.rs:154-183
    void takeRequests();   // ui.rs:189-202
    void reply(const QString &text);  // ui.rs:587
    void openMenu(int dock, int item);

    State m_state;
    DockStyle m_style;
    QQmlEngine *m_engine = nullptr;
    QList<DockWindow *> m_docks;
    std::optional<QString> m_controlPath;
    std::optional<ipc::Updates> m_updates;
    ipc::UnixStream m_updateSignal;
    QSocketNotifier *m_updateNotifier = nullptr;
    QString m_lastError;
    std::optional<control::Requests> m_requests;
    ipc::UnixStream m_requestSignal;
    std::optional<control::Endpoint> m_endpoint;
    QSocketNotifier *m_requestNotifier = nullptr;
    QList<FileWatch *> m_watches;
    DesktopEntriesWatch *m_desktopWatch = nullptr;
    double m_ackStarted = 0;
    int m_ackSettled = 0;
    DockWindow *m_ackDock = nullptr;
};

} // namespace riley::dock::app
