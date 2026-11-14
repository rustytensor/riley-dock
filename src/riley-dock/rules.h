// SPDX-License-Identifier: GPL-3.0-or-later
//
// libriley-dock: rules. Mirrors riley dock/src/rules.rs.
//
// Hyprland records (clients, workspaces, monitors), dock item ordering, click actions, indicators, hover text,
// icon sizes and the SCREENS / REFLECTIONS settings. Pure functions, no I/O.
//
// Config text conventions shared by every parser in this library (they mirror Rust's str methods):
//   - lines: split at '\n'; a trailing '\r' is removed from each line; a final '\n' does not add an empty line
//     (Rust str::lines).
//   - key/value: a line is split at its FIRST '=' (Rust split_once('=')); a line without '=' is ignored.
//   - trim: leading and trailing Unicode White_Space removed (Rust str::trim).
//   - when a key appears more than once, the LAST occurrence wins unless a function says otherwise.
#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QDebug>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <optional>
#include <variant>

namespace riley::dock::rules {

// The id a special workspace gets when hyprctl leaves it out (Hyprland 0.57 does, for every special workspace):
// -99, the first special id older Hyprland versions reported, so such windows still sort before workspace 1.
inline constexpr qint64 SPECIAL_WORKSPACE_ID = -99;

// riley dock/src/rules.rs:4 struct Workspace { id: i64, name: String }. A workspace object without "id" is accepted
// only when its "type" is "special"; it then gets SPECIAL_WORKSPACE_ID.
struct Workspace {
    qint64 id = 0;
    QString name;
    bool operator==(const Workspace &) const = default;
};

// riley dock/src/rules.rs:10 struct Client { address, class, title, workspace }. One window as hyprctl `j/clients`
// reports it. The Rust field `class` (JSON key "class") is `className` here because `class` is a C++ keyword.
struct Client {
    QString address;
    QString className;
    QString title;
    Workspace workspace;
    bool operator==(const Client &) const = default;
};

// riley dock/src/rules.rs:18 struct Monitor { name, description, x: i32, y: i32 }. One screen as hyprctl
// `j/monitors` reports it.
struct Monitor {
    QString name;
    QString description;
    int x = 0;
    int y = 0;
    bool operator==(const Monitor &) const = default;
};

// riley dock/src/rules.rs:26 struct DockItem { id, windows }. One slot of the dock: the launcher control
// ("launcher"), a pin, or a running app class with its windows.
struct DockItem {
    QString id;
    QList<Client> windows;
    bool operator==(const DockItem &) const = default;
};

// JSON decoding, mirroring serde_json::from_str::<Vec<Client>> / ::<Vec<Monitor>> with the #[derive(Deserialize)]
// of riley dock/src/rules.rs:3, :9 and :17. The text must be a JSON array of objects; every field above must be
// present with the right JSON type (strings for strings, integers in range for id/x/y, an object for "workspace");
// unknown keys are ignored. Any violation makes the whole parse fail (std::nullopt), as serde does. Array order is
// preserved.
std::optional<QList<Client>> clientsFromJson(const QByteArray &json);   // serde Vec<Client>
std::optional<QList<Monitor>> monitorsFromJson(const QByteArray &json); // serde Vec<Monitor>

// riley dock/src/rules.rs:31 items(pins, clients). Result order:
//   1. the launcher item {id "launcher", no windows}, always first;
//   2. each pin in pin order, skipped when an item with that id already exists (repeats, or a pin "launcher");
//   3. clients with a non-empty class, STABLY sorted by (workspace.id ascending, then class ascending by code
//      point); each client joins the first item after the launcher whose id equals its class, else it starts a
//      new item {id = class, windows = [client]} appended at the end.
// Clients with an empty class (closing windows) are dropped.
QList<DockItem> items(const QStringList &pins, const QList<Client> &clients);

// riley dock/src/rules.rs:51 enum Action { Launch(String), Focus(String), Minimize(String), Restore(String),
// Menu(Vec<Client>) }.
namespace action {
struct Launch { QString id; bool operator==(const Launch &) const = default; };
struct Focus { QString address; bool operator==(const Focus &) const = default; };
struct Minimize { QString address; bool operator==(const Minimize &) const = default; };
struct Restore { QString address; bool operator==(const Restore &) const = default; };
struct Menu { QList<Client> windows; bool operator==(const Menu &) const = default; };
} // namespace action
using Action = std::variant<action::Launch, action::Focus, action::Minimize, action::Restore, action::Menu>;

// riley dock/src/rules.rs:59 (private fn minimized): a window is minimized when its workspace name is exactly
// "special:minimized".

// riley dock/src/rules.rs:63 left_click(id, windows, active).
//   no windows                      -> Launch(id)
//   one window, minimized           -> Restore(address)   (checked before focus: even if it is `active`)
//   one window, address == active   -> Minimize(address)
//   one window, otherwise           -> Focus(address)
//   two or more windows             -> Menu(windows, unchanged order)
Action leftClick(const QString &id, const QList<Client> &windows, const QString &active);

// riley dock/src/rules.rs:73 menu_pick(c). Minimized -> Restore(address), otherwise Focus(address). Never Minimize.
Action menuPick(const Client &client);

// riley dock/src/rules.rs:77 window_line(c). "<title>  · <workspace>" (two spaces, U+00B7 MIDDLE DOT, one space).
// The workspace part is "minimized" for a minimized window, else workspace.name. A title longer than 48 Unicode
// scalar values (code points, NOT UTF-16 units) becomes its first 47 code points plus U+2026 "…".
QString windowLine(const Client &client);

// riley dock/src/rules.rs:87 hover_text(name, windows). `name`, then one windowLine per window, joined by '\n'.
// No windows: just name.
QString hoverText(const QString &name, const QList<Client> &windows);

// riley dock/src/rules.rs:92 enum Indicator { None, One, Two, Hollow }.
enum class Indicator { None, One, Two, Hollow };

// riley dock/src/rules.rs:94 indicator(windows). Empty -> None; ALL windows minimized -> Hollow; exactly one ->
// One; else Two.
Indicator indicator(const QList<Client> &windows);

// riley dock/src/rules.rs:101 icon_size(text). Over the lines that START WITH "ICON_SIZE=" exactly (the line is
// not trimmed), the first whose trimmed value parses as an unsigned 32-bit decimal (Rust u32::from_str: optional
// leading '+', ASCII digits only, no overflow) and is > 0 wins (FIRST occurrence, unlike the other keys).
// Otherwise 44.
quint32 iconSize(const QString &text);

// riley dock/src/rules.rs:107 drawn_size(size, count). count = app slots only, not the launcher; count >= 0.
//   drawn = size*5/4 (integer, 64-bit); divisor = 6 + max(count-6, 0)/3 (integer);
//   result = min(drawn*6/divisor, UINT32_MAX). Integer truncation at every step.
quint32 drawnSize(quint32 size, qsizetype count);

// riley dock/src/rules.rs:114 icon_lookup_size(drawn). ceil(drawn*8/5) in 64-bit integers, capped at INT_MAX: the
// source icon size that covers the full 1.6x swell.
int iconLookupSize(quint32 drawn);

// riley dock/src/rules.rs:118 screen_filters(config). Value of the last line whose trimmed key is "SCREENS" (value
// NOT trimmed as a whole), split at ',', each item trimmed, empty items dropped, repeats dropped keeping the first
// position.
QStringList screenFilters(const QString &config);

// riley dock/src/rules.rs:132 reflections_enabled(config, allowed). Starts true; every line whose trimmed key is
// "REFLECTIONS" sets it to (trimmed value != "0"), so the last occurrence wins and an invalid value means on.
// Result: allowed && that.
bool reflectionsEnabled(const QString &config, bool allowed);

// riley dock/src/rules.rs:143 struct ScreenSelection { names, fallback }.
struct ScreenSelection {
    QStringList names;
    bool fallback = false;
    bool operator==(const ScreenSelection &) const = default;
};

// riley dock/src/rules.rs:149 startup_selection(current, &mut pending, configured, expired).
//   if current is set:
//     names empty                          -> pending = nullopt
//     !current.fallback || !configured     -> return current (pending untouched)
//     otherwise (a fallback while waiting) -> pending = current
//   then: expired ? take pending (return it, pending becomes nullopt) : nullopt.
std::optional<ScreenSelection> startupSelection(std::optional<ScreenSelection> current,
                                                std::optional<ScreenSelection> &pending, bool configured,
                                                bool expired);

// riley dock/src/rules.rs:159 select_monitors(monitors, output, filters).
//   output set   -> {names of monitors whose name == output, in input order; fallback false} (may be empty).
//   otherwise    -> names of monitors whose description contains ANY filter (substring, case-sensitive), input
//                   order; if non-empty -> fallback false.
//   no match     -> the single monitor minimal by (y, then x, then name by code point; the first of equal ones,
//                   Rust Iterator::min_by), fallback true; no monitors at all -> {[], fallback false}.
ScreenSelection selectMonitors(const QList<Monitor> &monitors, const std::optional<QString> &output,
                               const QStringList &filters);

// Rust #[derive(Debug)]: diagnostic text for test failure messages. Any readable form.
QDebug operator<<(QDebug debug, const Workspace &workspace);
QDebug operator<<(QDebug debug, const Client &client);
QDebug operator<<(QDebug debug, const Monitor &monitor);
QDebug operator<<(QDebug debug, const DockItem &item);
QDebug operator<<(QDebug debug, const ScreenSelection &selection);

namespace action {
// Rust #[derive(Debug)] on Action (found by argument-dependent lookup through the variant's alternatives).
QDebug operator<<(QDebug debug, const Action &action);
} // namespace action

} // namespace riley::dock::rules
