// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/config.h"

#include "app/log.h"
#include "app/xmlfile.h"

#include <riley-dock/autohide.h>
#include <riley-dock/rules.h>

#include <QtCore/QDir>

namespace riley::dock::app {

namespace {

const QString CONFIG_SCHEMA = QStringLiteral(":/riley-dock/schemas/config.xsd");

bool xmlBoolean(const QString &text)
{
    const QString value = collapseWhitespace(text);
    return value == u"true" || value == u"1";
}

// The element texts of a file that validated against schemas/config.xsd, so every value is in range.
DockConfig fromXml(const XmlElement &root)
{
    DockConfig config;
    for (const XmlElement &element : root.children) {
        if (element.name == u"icon-size") {
            QString digits = collapseWhitespace(element.text);
            if (digits.startsWith(u'+'))
                digits.remove(0, 1);
            bool ok = false;
            const qulonglong size = digits.toULongLong(&ok, 10);
            if (ok && size > 0 && size <= 0xffffffffull)
                config.iconSize = quint32(size);
        } else if (element.name == u"screens") {
            // Each <screen> is one part of the old comma-separated list (it can hold no comma or line break), so the
            // core's own SCREENS rule trims the parts, drops empty ones and repeats (riley dock/src/rules.rs:118-130).
            QStringList parts;
            for (const XmlElement &screen : element.children)
                parts.append(screen.text);
            config.screens = rules::screenFilters(QStringLiteral("SCREENS=") + parts.join(u','));
        } else if (element.name == u"reflections") {
            config.reflections = xmlBoolean(element.text);
        } else if (element.name == u"launcher") {
            config.launcher = grid::position(QStringLiteral("LAUNCHER=") + collapseWhitespace(element.text));
        } else if (element.name == u"autohide") {
            config.autohide = xmlBoolean(element.text);
        }
    }
    return config;
}

} // namespace

ConfigPaths configPaths()
{
    const QDir config(homePath("XDG_CONFIG_HOME", QStringLiteral(".config")));
    return {config.filePath(QStringLiteral("riley-dock/config.xml")),
            config.filePath(QStringLiteral("nwg-dock-hyprland/riley.conf"))};
}

DockConfig fromRileyConf(const QString &text)
{
    DockConfig config;
    config.iconSize = rules::iconSize(text);
    config.screens = rules::screenFilters(text);
    config.reflections = rules::reflectionsEnabled(text, true);
    config.launcher = grid::position(text);
    config.autohide = autohide::enabled(text);
    return config;
}

LoadedConfig loadConfig(const ConfigPaths &paths, const std::optional<LoadedConfig> &current)
{
    const XmlRead read = readValidated(paths.xml, CONFIG_SCHEMA);
    switch (read.status) {
    case XmlRead::Status::Ok:
        clearFileError(paths.xml);
        return {fromXml(read.root), ConfigSource::Xml};
    case XmlRead::Status::Error:
        reportFileError("config", paths.xml, read.error);
        if (current)
            return *current;
        break;
    case XmlRead::Status::Missing:
        clearFileError(paths.xml);
        break;
    }
    return {fromRileyConf(readTextOrEmpty(paths.conf)), ConfigSource::RileyConf};
}

} // namespace riley::dock::app
