// SPDX-License-Identifier: GPL-3.0-or-later
//
// libriley-dock: desktop. Mirrors riley dock/src/desktop.rs.
//
// Matches a Hyprland window class (or a pin id) to an installed desktop entry. The Rust version used GIO
// (gio::DesktopAppInfo); this one reads the desktop files itself and must give the same answers as GIO for the
// cases that riley dock/tests/parity.rs:190 desktop_matching_isolated pins down.
//
// Where entries are looked for (XDG Base Directory spec, as GIO does): $XDG_DATA_HOME/applications (default
// $HOME/.local/share/applications), then each $XDG_DATA_DIRS entry + "/applications" (default
// /usr/local/share:/usr/share). The environment is read when the cache is (re)built, never earlier. A desktop-file
// id is the path below "applications/" with '/' replaced by '-'; the first directory that has an id wins.
//
// An entry is usable (GIO: g_desktop_app_info_new returns non-NULL) only if its [Desktop Entry] group has
// Type=Application, Hidden is not true, TryExec (if present) is found, and the program of Exec (first word,
// looked up in $PATH unless it is a path) exists and is executable. NoDisplay entries are still usable.
// Name/Icon are the unlocalised keys under LC_ALL=C; with another locale the best localised key (Name[xx]) as GIO
// picks it.
#pragma once

#include <QtCore/QDebug>
#include <QtCore/QString>

#include <optional>

namespace riley::dock::desktop {

// What the Rust code read from gio::DesktopAppInfo (riley dock/src/desktop.rs:18 returns one).
struct AppInfo {
    QString id;             // DesktopAppInfo::id(), e.g. "firefox.desktop" (always set for a found entry)
    QString name;           // AppInfo::name(), the Name key
    QString icon;           // AppInfo::icon().to_string(), the Icon key as written ("" when absent)
    QString filename;       // DesktopAppInfo::filename(), absolute path of the .desktop file
    QString exec;           // Exec key as written
    QString startupWmClass; // StartupWMClass key ("" when absent)
    bool operator==(const AppInfo &) const = default;
};

// riley dock/src/desktop.rs:18 app_info(id) -> Option<DesktopAppInfo>.
//   1. a cached answer for exactly `id` (found or not found) is returned as is;
//   2. try the desktop-file id "<id>.desktop", then "<lowercase id>.desktop";
//   3. otherwise look in a class map built once per cache: every usable entry with a StartupWMClass, keyed by the
//      lowercased StartupWMClass, the FIRST entry in GIO's AppInfo::all() order kept for each key; look up
//      lowercase(id);
//   4. remember the answer for `id` (also when not found) and return it.
// The cache (riley dock/src/desktop.rs:6 struct Cache, :11 thread_local!) is per thread and is only cleared by
// invalidate(): a desktop file changed on disk keeps its cached answer until then.
std::optional<AppInfo> appInfo(const QString &id);

// riley dock/src/desktop.rs:14 invalidate(). Forget every cached answer and the class map (called when installed
// entries change).
void invalidate();

// Diagnostic text for test failure messages.
QDebug operator<<(QDebug debug, const AppInfo &info);

} // namespace riley::dock::desktop
