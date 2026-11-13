// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-dock-qt: watch one file the way the Rust dock's GIO file monitors do (riley dock/src/ui.rs:207-229,
// gio::FileMonitorFlags::WATCH_MOVES): writes, a replace by rename, removal and creation all count, also when the
// folder does not exist yet.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QFileSystemWatcher>
#include <QtCore/QObject>
#include <QtCore/QString>

namespace riley::dock::app {

class FileWatch : public QObject {
    Q_OBJECT
public:
    explicit FileWatch(QString path, QObject *parent = nullptr);

Q_SIGNALS:
    // The file's identity, size or times changed, or it appeared or disappeared.
    void changed();

private:
    void rearm();
    void check();
    QByteArray signature() const;

    QString m_path;
    QFileSystemWatcher m_watcher;
    QByteArray m_signature;
};

// The folders whose desktop entries the core reads (riley-dock/desktop.h: $XDG_DATA_HOME/applications, then each
// $XDG_DATA_DIRS entry + "/applications"); watched as gio::AppInfoMonitor does (riley dock/src/ui.rs:230-238).
class DesktopEntriesWatch : public QObject {
    Q_OBJECT
public:
    explicit DesktopEntriesWatch(QObject *parent = nullptr);

Q_SIGNALS:
    void changed();

private:
    void rearm();
    QFileSystemWatcher m_watcher;
};

} // namespace riley::dock::app
