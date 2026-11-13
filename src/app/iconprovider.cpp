// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/iconprovider.h"

#include "app/log.h"

#include <QtCore/QDir>
#include <QtCore/QSettings>
#include <QtCore/QUrl>
#include <QtDBus/QDBusConnection>
#include <QtDBus/QDBusMessage>
#include <QtDBus/QDBusVariant>
#include <QtGui/QIcon>
#include <QtGui/QImageReader>

namespace riley::dock::app {

namespace {

constexpr auto BASE64 = QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals;

// AppInfo::icon is the GIcon string (riley-dock/desktop.h): a path, a name, or the rare tokenized themed form
// ". GThemedIcon <name> <variant>", whose first name is used.
QString themeName(const QString &spec)
{
    if (!spec.startsWith(QStringLiteral(". GThemedIcon ")))
        return spec;
    const QStringList parts = spec.split(u' ', Qt::SkipEmptyParts);
    return parts.size() >= 3 ? QUrl::fromPercentEncoding(parts[2].toUtf8()) : spec;
}

QImage fromFile(const QString &path, const QSize &requested)
{
    QImageReader reader(path);
    if (requested.isValid() && !requested.isEmpty())
        reader.setScaledSize(requested);
    return reader.read();
}

// org.freedesktop.portal.Settings: ReadOne (version 2) answers v(value), the older Read answers v(v(value)).
QString portalIconTheme()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected())
        return {};
    for (const char *method : {"ReadOne", "Read"}) {
        QDBusMessage call = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.Settings"), QString::fromLatin1(method));
        call << QStringLiteral("org.gnome.desktop.interface") << QStringLiteral("icon-theme");
        const QDBusMessage reply = bus.call(call, QDBus::Block, 1000);
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
            continue;
        QVariant value = reply.arguments().constFirst();
        while (value.canConvert<QDBusVariant>() && value.userType() == qMetaTypeId<QDBusVariant>())
            value = value.value<QDBusVariant>().variant();
        const QString name = value.toString().trimmed();
        if (!name.isEmpty())
            return name;
    }
    return {};
}

} // namespace

QString iconUrl(const QString &spec)
{
    return QStringLiteral("image://%1/%2").arg(ICON_PROVIDER, QString::fromLatin1(spec.toUtf8().toBase64(BASE64)));
}

void chooseIconTheme()
{
    QString name = portalIconTheme();
    if (!name.isEmpty()) {
        QIcon::setThemeName(name);
        return;
    }
    name = QStringLiteral("Adwaita");
    const QString config = homePath("XDG_CONFIG_HOME", QStringLiteral(".config"));
    for (const char *folder : {"gtk-4.0", "gtk-3.0"}) {
        const QString path = QDir(config).filePath(QString::fromLatin1(folder) + QStringLiteral("/settings.ini"));
        if (!QFileInfo::exists(path))
            continue;
        const QString value = QSettings(path, QSettings::IniFormat).value(QStringLiteral("Settings/gtk-icon-theme-name"))
                                  .toString().trimmed();
        if (!value.isEmpty()) {
            name = value;
            break;
        }
    }
    QIcon::setThemeName(name);
}

DockIconProvider::DockIconProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage DockIconProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const QString spec = QString::fromUtf8(QByteArray::fromBase64(id.toLatin1(), BASE64));
    const QSize wanted = requestedSize.isValid() && !requestedSize.isEmpty() ? requestedSize : QSize(64, 64);
    QImage image;
    if (spec.startsWith(u'/')) {
        image = fromFile(spec, requestedSize);
    } else if (spec.startsWith(QStringLiteral("file://"))) {
        image = fromFile(QUrl(spec).toLocalFile(), requestedSize);
    } else {
        const QIcon icon = QIcon::fromTheme(themeName(spec));
        if (!icon.isNull())
            image = icon.pixmap(wanted).toImage();
    }
    if (image.isNull()) {
        const QIcon missing = QIcon::fromTheme(QStringLiteral("image-missing"));
        if (!missing.isNull())
            image = missing.pixmap(wanted).toImage();
    }
    if (image.isNull()) {
        image = QImage(wanted, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
    }
    if (size)
        *size = image.size();
    return image;
}

} // namespace riley::dock::app
