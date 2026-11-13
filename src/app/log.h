// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-dock-qt: the dock's log and the home-relative paths. Mirrors riley dock/src/main.rs:10-22.
#pragma once

#include <QtCore/QString>

namespace riley::dock::app {

// riley dock/src/main.rs:10 home_path(variable, fallback): $variable when set, else $HOME/<fallback>.
QString homePath(const char *variable, const QString &fallback);

// riley dock/src/main.rs:16 log(message): one line appended to $XDG_STATE_HOME/riley/dock.log (fallback
// $HOME/.local/state). Failures are ignored. Safe to call from any thread.
void log(const QString &message);

// Seconds on a monotonic clock (Rust Instant). Only differences are meaningful.
double monotonicSeconds();

// std::fs::read_to_string(path).unwrap_or_default(): the file as UTF-8, or "" when it is missing, unreadable or
// not valid UTF-8.
QString readTextOrEmpty(const QString &path);

} // namespace riley::dock::app
