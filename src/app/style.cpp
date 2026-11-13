// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/style.h"

#include "app/xmlfile.h"

namespace riley::dock::app {

namespace {

enum class Kind { Color, Size, FontSize, Family };

struct Key {
    const char *name;
    Kind kind;
    const char *defaultValue; // colours and families as text, sizes as decimal text
};

// docs/files.md section 3.2 "Elements", by element name; the defaults equal style.example.xml and riley
// dock/style.example.css.
constexpr Key KEYS[] = {
    {"bar-background", Kind::Color, "#1a1b26b8"},
    {"bar-foreground", Kind::Color, "#c0caf5"},
    {"bar-border", Kind::Color, "#7aa2f759"},
    {"bar-border-width", Kind::Size, "1"},
    {"bar-radius", Kind::Size, "18"},
    {"accent", Kind::Color, "#7aa2f7"},
    {"shelf", Kind::Color, "#7aa2f72e"},
    {"active-line", Kind::Color, "#7aa2f7"},
    {"active-line-width", Kind::Size, "3"},
    {"name-background", Kind::Color, "#1a1b26f0"},
    {"name-foreground", Kind::Color, "#c0caf5"},
    {"name-border", Kind::Color, "#7aa2f74d"},
    {"name-border-width", Kind::Size, "1"},
    {"name-radius", Kind::Size, "8"},
    {"name-padding-x", Kind::Size, "11"},
    {"name-padding-y", Kind::Size, "4"},
    {"name-font-family", Kind::Family, "Inter"},
    {"name-font-size", Kind::FontSize, "12"},
    {"menu-padding", Kind::Size, "6"},
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

const QString STYLE_SCHEMA = QStringLiteral(":/riley-dock/schemas/style.xsd");

// The text of an element of a file that validated against schemas/style.xsd, so it is well formed and in range.
QVariant valueOf(const Key &key, const QString &text)
{
    const QString value = collapseWhitespace(text);
    switch (key.kind) {
    case Kind::Color:
        if (const auto color = DockStyle::parseColor(value))
            return QVariant::fromValue(*color);
        return defaultOf(key);
    case Kind::Size:
    case Kind::FontSize: {
        bool ok = false;
        const int number = (value.startsWith(u'+') ? value.mid(1) : value).toInt(&ok, 10);
        return ok ? QVariant(number) : defaultOf(key);
    }
    case Kind::Family: return value;
    }
    return defaultOf(key);
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
    const XmlRead read = readValidated(path, STYLE_SCHEMA);
    if (read.status == XmlRead::Status::Error) {
        // Keep the style in use: the defaults at start, else the last file that was read without an error.
        reportFileError("style", path, read.error);
        return;
    }
    clearFileError(path);
    reset();
    if (read.status == XmlRead::Status::Ok) {
        for (const XmlElement &element : read.root.children) {
            for (const Key &key : KEYS) {
                if (element.name == QLatin1StringView(key.name))
                    m_values.insert(element.name, valueOf(key, element.text));
            }
        }
    }
    Q_EMIT changed();
}

} // namespace riley::dock::app
