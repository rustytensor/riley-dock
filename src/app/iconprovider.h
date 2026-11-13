// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-dock-qt: application icons for QML, as "image://dockicon/<id>". The id is iconId(spec): the icon spec
// (an absolute file path or an icon theme name, riley-dock/desktop.h AppInfo::icon, or the pin/class id) in
// base64url, so any name survives the URL. Looked up as GTK did (riley dock/src/ui.rs:318-330): the theme icon at
// the requested size, "image-missing" when there is none.
#pragma once

#include <QtCore/QString>
#include <QtQuick/QQuickImageProvider>

namespace riley::dock::app {

inline constexpr QLatin1StringView ICON_PROVIDER{"dockicon"};

// The URL QML loads for an icon spec: "image://dockicon/<base64url(spec)>".
QString iconUrl(const QString &spec);

// Use the icon theme the GTK dock used, found the way GTK 4 finds it on Wayland: the desktop's
// org.gnome.desktop.interface icon-theme from the settings portal (GTK reads it there and it outranks settings.ini),
// then the gtk-icon-theme-name of $XDG_CONFIG_HOME/gtk-4.0/settings.ini, then gtk-3.0, else GTK's default "Adwaita".
// Qt's own platform theme (qt6ct, for example) is not consulted: the GTK dock never saw it.
void chooseIconTheme();

class DockIconProvider : public QQuickImageProvider {
public:
    DockIconProvider();
    // Called on the GUI thread (QML Image with asynchronous: false, the default): QIcon is not thread-safe.
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

} // namespace riley::dock::app
