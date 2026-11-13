// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-dock-qt: the dock style, read from riley-dock/style.json (docs/files.md section 3.2). It replaces the GTK CSS
// the Rust dock reads (riley dock/src/ui.rs:83-86, :207-217, :282-285). Colours, fonts and a few sizes only: the
// style never changes the surface size, the input region or the reserved space.
#pragma once

#include <QtCore/QHash>
#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtGui/QColor>
#include <QtQml/qqmlregistration.h>

namespace riley::dock::app {

class DockStyle : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("The dock's style comes from style.json")

    // One property per key of docs/files.md section 3.2; every one changes together with `changed`.
    Q_PROPERTY(QColor barBackground READ barBackground NOTIFY changed)     // bar_background
    Q_PROPERTY(QColor barForeground READ barForeground NOTIFY changed)     // bar_foreground
    Q_PROPERTY(QColor barBorder READ barBorder NOTIFY changed)             // bar_border
    Q_PROPERTY(int barBorderWidth READ barBorderWidth NOTIFY changed)      // bar_border_width
    Q_PROPERTY(int barRadius READ barRadius NOTIFY changed)                // bar_radius
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)                   // accent
    Q_PROPERTY(QColor shelf READ shelf NOTIFY changed)                     // shelf
    Q_PROPERTY(QColor activeLine READ activeLine NOTIFY changed)           // active_line
    Q_PROPERTY(int activeLineWidth READ activeLineWidth NOTIFY changed)    // active_line_width
    Q_PROPERTY(QColor nameBackground READ nameBackground NOTIFY changed)   // name_background
    Q_PROPERTY(QColor nameForeground READ nameForeground NOTIFY changed)   // name_foreground
    Q_PROPERTY(QColor nameBorder READ nameBorder NOTIFY changed)           // name_border
    Q_PROPERTY(int nameBorderWidth READ nameBorderWidth NOTIFY changed)    // name_border_width
    Q_PROPERTY(int nameRadius READ nameRadius NOTIFY changed)              // name_radius
    Q_PROPERTY(int namePaddingX READ namePaddingX NOTIFY changed)          // name_padding_x
    Q_PROPERTY(int namePaddingY READ namePaddingY NOTIFY changed)          // name_padding_y
    Q_PROPERTY(QString nameFontFamily READ nameFontFamily NOTIFY changed)  // name_font_family
    Q_PROPERTY(int nameFontSize READ nameFontSize NOTIFY changed)          // name_font_size (pixels)
    Q_PROPERTY(int menuPadding READ menuPadding NOTIFY changed)            // menu_padding

public:
    explicit DockStyle(QObject *parent = nullptr);

    // Read the whole file again and apply docs/files.md section 3.2 "Missing file, missing key, bad value": a missing
    // file gives every default silently; an unreadable file, invalid JSON or a non-object gives every default and one
    // log line; a missing key gives its default; a bad value gives that key's default and one log line naming it.
    // Emits `changed` once.
    void load(const QString &path);

    QColor barBackground() const { return color("bar_background"); }
    QColor barForeground() const { return color("bar_foreground"); }
    QColor barBorder() const { return color("bar_border"); }
    int barBorderWidth() const { return size("bar_border_width"); }
    int barRadius() const { return size("bar_radius"); }
    QColor accent() const { return color("accent"); }
    QColor shelf() const { return color("shelf"); }
    QColor activeLine() const { return color("active_line"); }
    int activeLineWidth() const { return size("active_line_width"); }
    QColor nameBackground() const { return color("name_background"); }
    QColor nameForeground() const { return color("name_foreground"); }
    QColor nameBorder() const { return color("name_border"); }
    int nameBorderWidth() const { return size("name_border_width"); }
    int nameRadius() const { return size("name_radius"); }
    int namePaddingX() const { return size("name_padding_x"); }
    int namePaddingY() const { return size("name_padding_y"); }
    QString nameFontFamily() const { return m_values.value(QStringLiteral("name_font_family")).toString(); }
    int nameFontSize() const { return size("name_font_size"); }
    int menuPadding() const { return size("menu_padding"); }

    // "#rrggbb" or "#rrggbbaa" (alpha last, as CSS), hexadecimal digits in either case; nullopt otherwise.
    static std::optional<QColor> parseColor(const QString &text);

Q_SIGNALS:
    void changed();

private:
    QColor color(const char *key) const { return m_values.value(QLatin1StringView(key)).value<QColor>(); }
    int size(const char *key) const { return m_values.value(QLatin1StringView(key)).toInt(); }
    void reset();

    QHash<QString, QVariant> m_values;
};

} // namespace riley::dock::app
