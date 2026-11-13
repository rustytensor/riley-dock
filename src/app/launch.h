// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-dock-qt: starting programs. The Rust dock launched desktop entries with GIO (riley dock/src/ui.rs:862-870,
// gio::AppInfo::launch with no files) and ran helper programs on threads (ui.rs:853-860, :872-882).
#pragma once

#include <riley-dock/desktop.h>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

namespace riley::dock::app {

// The argument vector GIO builds for an entry launched with no files: the Exec value with its field codes expanded
// (%f %F %u %U and the deprecated %d %D %n %N %v %m dropped, %i -> "--icon <Icon>" when there is an icon,
// %c -> Name, %k -> the desktop file, %% -> %), each expansion shell-quoted, then split as g_shell_parse_argv does.
// nullopt when the Exec value is empty or does not parse.
std::optional<QStringList> launchArguments(const desktop::AppInfo &info);

// Start the entry detached (gio::AppInfo::launch). False with `error` set when it cannot be started.
bool launchApp(const desktop::AppInfo &info, QString *error);

// Run `program args` beside the dock (reaped when it ends) and log "<what>: <error>" when it cannot be started and,
// with `logExit`, "<what> failed" when it exits unsuccessfully. `what` defaults to the program name.
void runLogged(const QString &program, const QStringList &args, const QString &what = {}, bool logExit = true);

} // namespace riley::dock::app
