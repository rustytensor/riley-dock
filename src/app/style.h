// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-dock-qt: the dock style, read from riley-dock/style.xml (docs/files.md section 3.2). It replaces the GTK CSS
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
    QML_UNCREATABLE("The dock's style comes from style.xml")

    // One property per element of docs/files.md section 3.2; every one changes together with `changed`.
    Q_PROPERTY(QColor barBackground READ barBackground NOTIFY changed)     // bar-background
    Q_PROPERTY(QColor barForeground READ barForeground NOTIFY changed)     // bar-foreground
    Q_PROPERTY(QColor barBorder READ barBorder NOTIFY changed)             // bar-border
    Q_PROPERTY(int barBorderWidth READ barBorderWidth NOTIFY changed)      // bar-border-width
    Q_PROPERTY(int barRadius READ barRadius NOTIFY changed)                // bar-radius
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)                   // accent
    Q_PROPERTY(QColor unpinnedDot READ unpinnedDot NOTIFY changed)         // unpinned-dot
    Q_PROPERTY(QColor shelf READ shelf NOTIFY changed)                     // shelf
    Q_PROPERTY(QColor activeLine READ activeLine NOTIFY changed)           // active-line
    Q_PROPERTY(int activeLineWidth READ activeLineWidth NOTIFY changed)    // active-line-width
    Q_PROPERTY(QColor nameBackground READ nameBackground NOTIFY changed)   // name-background
    Q_PROPERTY(QColor nameForeground READ nameForeground NOTIFY changed)   // name-foreground
    Q_PROPERTY(QColor nameBorder READ nameBorder NOTIFY changed)           // name-border
    Q_PROPERTY(int nameBorderWidth READ nameBorderWidth NOTIFY changed)    // name-border-width
    Q_PROPERTY(int nameRadius READ nameRadius NOTIFY changed)              // name-radius
    Q_PROPERTY(int namePaddingX READ namePaddingX NOTIFY changed)          // name-padding-x
    Q_PROPERTY(int namePaddingY READ namePaddingY NOTIFY changed)          // name-padding-y
    Q_PROPERTY(QString nameFontFamily READ nameFontFamily NOTIFY changed)  // name-font-family
    Q_PROPERTY(int nameFontSize READ nameFontSize NOTIFY changed)          // name-font-size (pixels)
    Q_PROPERTY(int menuPadding READ menuPadding NOTIFY changed)            // menu-padding

public:
    explicit DockStyle(QObject *parent = nullptr);

    // Read the whole file again (docs/files.md section 3.2 "Reading it"): a missing file gives every default; a file
    // that validates gives its values, and every element it leaves out its default; a file that cannot be read, parsed
    // or validated writes the one error line and keeps the style in use (the defaults at start). Emits `changed` once
    // when the style was set again, never after an error.
    void load(const QString &path);

    QColor barBackground() const { return color("bar-background"); }
    QColor barForeground() const { return color("bar-foreground"); }
    QColor barBorder() const { return color("bar-border"); }
    int barBorderWidth() const { return size("bar-border-width"); }
    int barRadius() const { return size("bar-radius"); }
    QColor accent() const { return color("accent"); }
    // The running dot of an open app that is not pinned. Without the element: bar-foreground's colour, which is
    // never the accent in Riley's themes and follows the theme.
    QColor unpinnedDot() const
    {
        return m_values.contains(QStringLiteral("unpinned-dot")) ? color("unpinned-dot") : barForeground();
    }
    QColor shelf() const { return color("shelf"); }
    QColor activeLine() const { return color("active-line"); }
    int activeLineWidth() const { return size("active-line-width"); }
    QColor nameBackground() const { return color("name-background"); }
    QColor nameForeground() const { return color("name-foreground"); }
    QColor nameBorder() const { return color("name-border"); }
    int nameBorderWidth() const { return size("name-border-width"); }
    int nameRadius() const { return size("name-radius"); }
    int namePaddingX() const { return size("name-padding-x"); }
    int namePaddingY() const { return size("name-padding-y"); }
    QString nameFontFamily() const { return m_values.value(QStringLiteral("name-font-family")).toString(); }
    int nameFontSize() const { return size("name-font-size"); }
    int menuPadding() const { return size("menu-padding"); }

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
