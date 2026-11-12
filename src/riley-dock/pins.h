// SPDX-License-Identifier: GPL-3.0-or-later
//
// libriley-dock: pins. Mirrors riley dock/src/pins.rs.
//
// The pins file: one app id per line, in dock order. Written by the dock and by Riley Settings > Dock.
#pragma once

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

namespace riley::dock::pins {

// riley dock/src/pins.rs:6 load(path) -> io::Result<Vec<String>>.
// Reads the file as UTF-8 (invalid UTF-8 is an error, as fs::read_to_string); every line (Rust str::lines) is
// trimmed and empty lines are dropped; order is kept. A missing file (ENOENT, including a missing parent
// directory) is NOT an error: it returns an empty list. Any other failure returns std::nullopt and, when `error`
// is given, a message in it.
std::optional<QStringList> load(const QString &path, QString *error = nullptr);

// riley dock/src/pins.rs:14 save(path, pins) -> io::Result<()>.
// Atomic replace: creates the parent directories (parent "." when the path has none), writes each pin followed by
// '\n' to a NEW unique sibling file (".dock-pins-<pid>-<counter>.tmp", one process-wide counter, opened with
// O_CREAT|O_EXCL, retrying the next counter on EEXIST), fsyncs it, then rename(2)s it over `path`. A reader that
// opened the old file keeps reading the old inode. On any failure the temporary is removed, false is returned and
// `error` (when given) gets a message.
bool save(const QString &path, const QStringList &pins, QString *error = nullptr);

// riley dock/src/pins.rs:36 pin(&mut pins, id). Appends id unless already present (exact match).
void pin(QStringList &pins, const QString &id);

// riley dock/src/pins.rs:40 unpin(&mut pins, id). Removes every entry equal to id; the rest keep their order.
void unpin(QStringList &pins, const QString &id);

} // namespace riley::dock::pins
