// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/style.h"

#include "app/log.h"

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>

#include <cmath>
#include <limits>

namespace riley::dock::app {

namespace {

enum class Kind { Color, Size, FontSize, Family };

struct Key {
    const char *name;
    Kind kind;
    const char *defaultValue; // colours and families as text, sizes as decimal text
};

// docs/files.md section 3.2 "Keys"; the defaults equal style.example.json and riley dock/style.example.css.
constexpr Key KEYS[] = {
    {"bar_background", Kind::Color, "#1a1b26b8"},
    {"bar_foreground", Kind::Color, "#c0caf5"},
    {"bar_border", Kind::Color, "#7aa2f759"},
    {"bar_border_width", Kind::Size, "1"},
    {"bar_radius", Kind::Size, "18"},
    {"accent", Kind::Color, "#7aa2f7"},
    {"shelf", Kind::Color, "#7aa2f72e"},
    {"active_line", Kind::Color, "#7aa2f7"},
    {"active_line_width", Kind::Size, "3"},
    {"name_background", Kind::Color, "#1a1b26f0"},
    {"name_foreground", Kind::Color, "#c0caf5"},
    {"name_border", Kind::Color, "#7aa2f74d"},
    {"name_border_width", Kind::Size, "1"},
    {"name_radius", Kind::Size, "8"},
    {"name_padding_x", Kind::Size, "11"},
    {"name_padding_y", Kind::Size, "4"},
    {"name_font_family", Kind::Family, "Inter"},
    {"name_font_size", Kind::FontSize, "12"},
    {"menu_padding", Kind::Size, "6"},
};

QVariant defaultOf(const Key &key)
{
    switch (key.kind) {
    case Kind::Color: return QVariant::fromValue(*DockStyle::parseColor(QString::fromLatin1(key.defaultValue)));
    case Kind::Size:
    case Kind::FontSize: return QString::fromLatin1(key.defaultValue).toInt();
    case Kind::Family: return QString::fromLatin1(key.defaultValue);
    }
    return {};
}

std::optional<QVariant> valueOf(const Key &key, const QJsonValue &value)
{
    switch (key.kind) {
    case Kind::Color:
        if (!value.isString())
            return std::nullopt;
        if (const auto color = DockStyle::parseColor(value.toString()))
            return QVariant::fromValue(*color);
        return std::nullopt;
    case Kind::Size:
    case Kind::FontSize: {
        if (!value.isDouble())
            return std::nullopt;
        const double number = value.toDouble();
        const double least = key.kind == Kind::FontSize ? 1 : 0;
        if (!std::isfinite(number) || number != std::floor(number) || number < least
            || number > std::numeric_limits<int>::max())
            return std::nullopt;
        return static_cast<int>(number);
    }
    case Kind::Family:
        if (!value.isString() || value.toString().isEmpty())
            return std::nullopt;
        return value.toString();
    }
    return std::nullopt;
}

int hexDigit(QChar c)
{
    const char16_t u = c.unicode();
    if (u >= '0' && u <= '9')
        return u - '0';
    if (u >= 'a' && u <= 'f')
        return u - 'a' + 10;
    if (u >= 'A' && u <= 'F')
        return u - 'A' + 10;
    return -1;
}

} // namespace

DockStyle::DockStyle(QObject *parent) : QObject(parent)
{
    reset();
}

std::optional<QColor> DockStyle::parseColor(const QString &text)
{
    if ((text.size() != 7 && text.size() != 9) || text.front() != u'#')
        return std::nullopt;
    int bytes[4] = {0, 0, 0, 255};
    for (qsizetype i = 1, n = 0; i < text.size(); i += 2, ++n) {
        const int high = hexDigit(text[i]), low = hexDigit(text[i + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        bytes[n] = high * 16 + low;
    }
    return QColor(bytes[0], bytes[1], bytes[2], bytes[3]);
}

void DockStyle::reset()
{
    m_values.clear();
    for (const Key &key : KEYS)
        m_values.insert(QString::fromLatin1(key.name), defaultOf(key));
}

void DockStyle::load(const QString &path)
{
    reset();
    QFile file(path);
    if (!QFileInfo::exists(path)) {
        Q_EMIT changed(); // missing file: every default, no message
        return;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        log(QStringLiteral("style %1: %2; using the default style").arg(path, file.errorString()));
        Q_EMIT changed();
        return;
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) {
        log(QStringLiteral("style %1: %2; using the default style")
                .arg(path, error.error != QJsonParseError::NoError ? error.errorString()
                                                                    : QStringLiteral("not a JSON object")));
        Q_EMIT changed();
        return;
    }
    const QJsonObject object = document.object();
    for (const Key &key : KEYS) {
        const QString name = QString::fromLatin1(key.name);
        if (!object.contains(name))
            continue;
        if (const auto value = valueOf(key, object.value(name)))
            m_values.insert(name, *value);
        else
            log(QStringLiteral("style %1: bad value for %2; using its default").arg(path, name));
    }
    Q_EMIT changed();
}

} // namespace riley::dock::app
