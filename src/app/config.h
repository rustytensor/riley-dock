// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-dock-qt: the dock's config, from riley-dock/config.xml or, as a fallback, the old nwg-dock-hyprland/riley.conf
// (docs/files.md section 1). Both files give the same settings; the core's functions read the old file's text, and
// the XML file is mapped onto the same values.
#pragma once

#include <riley-dock/grid.h>

#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>

namespace riley::dock::app {

// The settings riley.conf holds (docs/files.md section 1.2). `reflections` is the file's value alone; the command
// line's --no-reflections is applied where it is used (riley dock/src/rules.rs:139).
struct DockConfig {
    quint32 iconSize = 44;                             // rules::iconSize
    QStringList screens;                               // rules::screenFilters
    bool reflections = true;                           // rules::reflectionsEnabled(text, true)
    grid::Position launcher = grid::Position::Left;    // grid::position
    bool autohide = false;                             // autohide::enabled
    bool operator==(const DockConfig &) const = default;
};

struct ConfigPaths {
    QString xml;   // $XDG_CONFIG_HOME/riley-dock/config.xml
    QString conf;  // $XDG_CONFIG_HOME/nwg-dock-hyprland/riley.conf
};

ConfigPaths configPaths();

// riley.conf's text read by the core's rules, exactly as before config.xml existed.
DockConfig fromRileyConf(const QString &text);

enum class ConfigSource { Xml, RileyConf };

struct LoadedConfig {
    DockConfig config;
    ConfigSource source = ConfigSource::RileyConf;
};

// docs/files.md section 1.1, "Which file the dock reads": config.xml when it is present and valid; when it is
// missing, riley.conf (itself missing: every default). When config.xml is present but fails to parse or validate,
// the one error line is written and `current` (the settings in use) is kept; at start, with no `current`, riley.conf
// is read instead.
LoadedConfig loadConfig(const ConfigPaths &paths, const std::optional<LoadedConfig> &current);

} // namespace riley::dock::app
