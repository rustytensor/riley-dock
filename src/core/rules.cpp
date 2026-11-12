// SPDX-License-Identifier: GPL-3.0-or-later
//
// Port of riley dock/src/rules.rs.
#include <riley-dock/rules.h>

#include <QtCore/QDebugStateSaver>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QJsonValue>
#include <QtCore/QMetaType>
#include <QtCore/QVariant>

#include <algorithm>
#include <cstdint>
#include <limits>

using namespace Qt::StringLiterals;

namespace riley::dock::rules {

namespace {

// Rust char::is_whitespace (Unicode White_Space); every member is in the BMP.
bool isRustWhitespace(char16_t c)
{
    return (c >= 0x09 && c <= 0x0D) || c == 0x20 || c == 0x85 || c == 0xA0 || c == 0x1680
        || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 || c == 0x202F || c == 0x205F
        || c == 0x3000;
}

// Rust str::trim.
QStringView rustTrim(QStringView s)
{
    qsizetype begin = 0;
    qsizetype end = s.size();
    while (begin < end && isRustWhitespace(s[begin].unicode()))
        ++begin;
    while (end > begin && isRustWhitespace(s[end - 1].unicode()))
        --end;
    return s.mid(begin, end - begin);
}

// Rust str::lines: split at '\n'; a line that ended with "\r\n" loses the '\r'; a last line without '\n' is kept
// as is (also a trailing '\r'); a final '\n' adds no empty line.
QList<QStringView> rustLines(QStringView s)
{
    QList<QStringView> lines;
    qsizetype start = 0;
    while (start < s.size()) {
        const qsizetype nl = s.indexOf(u'\n', start);
        if (nl < 0) {
            lines.append(s.mid(start));
            break;
        }
        QStringView line = s.mid(start, nl - start);
        if (line.endsWith(u'\r'))
            line.chop(1);
        lines.append(line);
        start = nl + 1;
    }
    return lines;
}

// Rust str::split_once('='): the key before the first '=', the rest after it.
bool splitOnceEquals(QStringView line, QStringView *key, QStringView *value)
{
    const qsizetype eq = line.indexOf(u'=');
    if (eq < 0)
        return false;
    *key = line.left(eq);
    *value = line.mid(eq + 1);
    return true;
}

// Rust Ord for str: byte order of UTF-8, which is code point order. UTF-16 unit order differs only where a
// surrogate meets a unit >= U+E000, so those are remapped before comparing (code point order fix-up).
int compareCodePoints(QStringView a, QStringView b)
{
    const qsizetype n = std::min(a.size(), b.size());
    for (qsizetype i = 0; i < n; ++i) {
        char16_t x = a[i].unicode();
        char16_t y = b[i].unicode();
        if (x == y)
            continue;
        auto fix = [](char16_t u) -> char32_t {
            if (u >= 0xE000)
                return char32_t(u) - 0x800;
            if (u >= 0xD800)
                return char32_t(u) + 0x2000;
            return u;
        };
        return fix(x) < fix(y) ? -1 : 1;
    }
    if (a.size() == b.size())
        return 0;
    return a.size() < b.size() ? -1 : 1;
}

// riley dock/src/rules.rs:59
bool minimized(const Client &c)
{
    return c.workspace.name == u"special:minimized";
}

// serde_json never produces a String holding a lone surrogate ("\ud800" without its pair is an error).
bool hasLoneSurrogate(const QString &s)
{
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s[i];
        if (c.isHighSurrogate()) {
            if (i + 1 < s.size() && s[i + 1].isLowSurrogate()) {
                ++i;
                continue;
            }
            return true;
        }
        if (c.isLowSurrogate())
            return true;
    }
    return false;
}

std::optional<QString> jsonString(const QJsonValue &value)
{
    if (!value.isString())
        return std::nullopt;
    QString s = value.toString();
    if (hasLoneSurrogate(s))
        return std::nullopt;
    return s;
}

// serde's i64/i32 visitors accept JSON integers only (no fraction or exponent) in range of the target type.
std::optional<qint64> jsonInteger(const QJsonValue &value, qint64 min, qint64 max)
{
    if (!value.isDouble())
        return std::nullopt;
    const QVariant variant = value.toVariant();
    if (variant.typeId() != QMetaType::LongLong)
        return std::nullopt;
    const qint64 n = variant.toLongLong();
    if (n < min || n > max)
        return std::nullopt;
    return n;
}

// A #[derive(Deserialize)] struct reads from a JSON object (by field name, unknown keys ignored, every field
// required) or from a JSON array holding exactly its fields in declaration order.
std::optional<QList<QJsonValue>> structFields(const QJsonValue &value, std::initializer_list<QStringView> names)
{
    QList<QJsonValue> fields;
    if (value.isObject()) {
        const QJsonObject object = value.toObject();
        for (QStringView name : names) {
            const auto it = object.constFind(name);
            if (it == object.constEnd())
                return std::nullopt; // serde: missing field
            fields.append(it.value());
        }
        return fields;
    }
    if (value.isArray()) {
        const QJsonArray array = value.toArray();
        if (array.size() != qsizetype(names.size()))
            return std::nullopt; // serde: invalid length / trailing elements
        for (const QJsonValue &item : array)
            fields.append(item);
        return fields;
    }
    return std::nullopt;
}

std::optional<Workspace> workspaceFromJson(const QJsonValue &value)
{
    const auto fields = structFields(value, {u"id", u"name"});
    if (!fields)
        return std::nullopt;
    const auto id = jsonInteger((*fields)[0], std::numeric_limits<qint64>::min(), std::numeric_limits<qint64>::max());
    const auto name = jsonString((*fields)[1]);
    if (!id || !name)
        return std::nullopt;
    return Workspace{*id, *name};
}

std::optional<Client> clientFromJson(const QJsonValue &value)
{
    const auto fields = structFields(value, {u"address", u"class", u"title", u"workspace"});
    if (!fields)
        return std::nullopt;
    const auto address = jsonString((*fields)[0]);
    const auto className = jsonString((*fields)[1]);
    const auto title = jsonString((*fields)[2]);
    const auto workspace = workspaceFromJson((*fields)[3]);
    if (!address || !className || !title || !workspace)
        return std::nullopt;
    return Client{*address, *className, *title, *workspace};
}

std::optional<Monitor> monitorFromJson(const QJsonValue &value)
{
    const auto fields = structFields(value, {u"name", u"description", u"x", u"y"});
    if (!fields)
        return std::nullopt;
    const auto name = jsonString((*fields)[0]);
    const auto description = jsonString((*fields)[1]);
    const auto x = jsonInteger((*fields)[2], std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    const auto y = jsonInteger((*fields)[3], std::numeric_limits<int>::min(), std::numeric_limits<int>::max());
    if (!name || !description || !x || !y)
        return std::nullopt;
    return Monitor{*name, *description, int(*x), int(*y)};
}

// Two things serde_json rejects that QJsonDocument accepts: a raw control character (U+0000..U+001F) inside any
// string, and a number with a fraction or exponent where an integer is wanted (Qt stores 1.0 or 1e2 as the integer
// it equals). The second is kept visible by replacing every such number with "{}", which no integer, string or
// struct field accepts and an ignored key still ignores. Call only on text QJsonDocument parsed without error.
std::optional<QByteArray> serdeView(const QByteArray &json)
{
    QByteArray out;
    out.reserve(json.size());
    const qsizetype n = json.size();
    bool inString = false;
    for (qsizetype i = 0; i < n; ++i) {
        const char c = json[i];
        if (inString) {
            if (static_cast<unsigned char>(c) < 0x20)
                return std::nullopt; // serde: control character while parsing a string
            out.append(c);
            if (c == '\\' && i + 1 < n)
                out.append(json[++i]);
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
            out.append(c);
            continue;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            qsizetype end = i;
            bool integer = true;
            while (end < n) {
                const char d = json[end];
                if (d == '.' || d == 'e' || d == 'E')
                    integer = false;
                else if (!(d == '-' || d == '+' || (d >= '0' && d <= '9')))
                    break;
                ++end;
            }
            out.append(integer ? json.mid(i, end - i) : QByteArray("{}"));
            i = end - 1;
            continue;
        }
        out.append(c);
    }
    return out;
}

template<typename T, typename F>
std::optional<QList<T>> listFromJson(const QByteArray &json, F element)
{
    QJsonParseError error;
    if (QJsonDocument::fromJson(json, &error); error.error != QJsonParseError::NoError)
        return std::nullopt;
    const std::optional<QByteArray> view = serdeView(json);
    if (!view)
        return std::nullopt;
    const QJsonDocument document = QJsonDocument::fromJson(*view, &error);
    if (error.error != QJsonParseError::NoError || !document.isArray())
        return std::nullopt;
    QList<T> result;
    for (const QJsonValue &value : document.array()) {
        std::optional<T> item = element(value);
        if (!item)
            return std::nullopt;
        result.append(std::move(*item));
    }
    return result;
}

// Rust u32::from_str: optional '+', then one or more ASCII digits, no overflow.
std::optional<quint32> parseU32(QStringView s)
{
    if (s.startsWith(u'+'))
        s = s.mid(1);
    if (s.isEmpty())
        return std::nullopt;
    quint64 n = 0;
    for (QChar c : s) {
        const char16_t u = c.unicode();
        if (u < u'0' || u > u'9')
            return std::nullopt;
        n = n * 10 + (u - u'0');
        if (n > std::numeric_limits<quint32>::max())
            return std::nullopt;
    }
    return quint32(n);
}

} // namespace

std::optional<QList<Client>> clientsFromJson(const QByteArray &json)
{
    return listFromJson<Client>(json, clientFromJson);
}

std::optional<QList<Monitor>> monitorsFromJson(const QByteArray &json)
{
    return listFromJson<Monitor>(json, monitorFromJson);
}

// riley dock/src/rules.rs:31
QList<DockItem> items(const QStringList &pins, const QList<Client> &clients)
{
    QList<DockItem> result{DockItem{u"launcher"_s, {}}};
    for (const QString &id : pins) {
        const bool present = std::any_of(result.cbegin(), result.cend(),
                                         [&](const DockItem &item) { return item.id == id; });
        if (!present)
            result.append(DockItem{id, {}});
    }
    QList<const Client *> sorted;
    for (const Client &c : clients) {
        if (!c.className.isEmpty())
            sorted.append(&c);
    }
    // riley dock/src/rules.rs:39 sort_by is a stable sort.
    std::stable_sort(sorted.begin(), sorted.end(), [](const Client *a, const Client *b) {
        if (a->workspace.id != b->workspace.id)
            return a->workspace.id < b->workspace.id;
        return compareCodePoints(a->className, b->className) < 0;
    });
    for (const Client *client : sorted) {
        auto it = std::find_if(result.begin() + 1, result.end(),
                               [&](const DockItem &item) { return item.id == client->className; });
        if (it != result.end())
            it->windows.append(*client);
        else
            result.append(DockItem{client->className, {*client}});
    }
    return result;
}

// riley dock/src/rules.rs:63
Action leftClick(const QString &id, const QList<Client> &windows, const QString &active)
{
    if (windows.isEmpty())
        return action::Launch{id};
    if (windows.size() == 1) {
        const Client &c = windows.front();
        if (minimized(c))
            return action::Restore{c.address};
        if (c.address == active)
            return action::Minimize{c.address};
        return action::Focus{c.address};
    }
    return action::Menu{windows};
}

// riley dock/src/rules.rs:73
Action menuPick(const Client &client)
{
    if (minimized(client))
        return action::Restore{client.address};
    return action::Focus{client.address};
}

// riley dock/src/rules.rs:77
QString windowLine(const Client &client)
{
    // Count Unicode scalar values (Rust chars), not UTF-16 units.
    const QString &t = client.title;
    qsizetype count = 0;
    qsizetype cut47 = t.size(); // UTF-16 index after the 47th character
    for (qsizetype i = 0; i < t.size(); ++i) {
        if (t[i].isHighSurrogate() && i + 1 < t.size() && t[i + 1].isLowSurrogate())
            ++i;
        ++count;
        if (count == 47)
            cut47 = i + 1;
    }
    const QString title = count > 48 ? t.left(cut47) + QChar(0x2026) : t;
    const QString workspace = minimized(client) ? u"minimized"_s : client.workspace.name;
    return title + u"  · " + workspace;
}

// riley dock/src/rules.rs:87
QString hoverText(const QString &name, const QList<Client> &windows)
{
    QStringList parts{name};
    for (const Client &c : windows)
        parts.append(windowLine(c));
    return parts.join(u'\n');
}

// riley dock/src/rules.rs:94
Indicator indicator(const QList<Client> &windows)
{
    if (windows.isEmpty())
        return Indicator::None;
    if (std::all_of(windows.cbegin(), windows.cend(), minimized))
        return Indicator::Hollow;
    if (windows.size() == 1)
        return Indicator::One;
    return Indicator::Two;
}

// riley dock/src/rules.rs:101
quint32 iconSize(const QString &text)
{
    for (QStringView line : rustLines(text)) {
        if (!line.startsWith(u"ICON_SIZE="))
            continue;
        const auto size = parseU32(rustTrim(line.mid(10)));
        if (size && *size > 0)
            return *size;
    }
    return 44;
}

// riley dock/src/rules.rs:107
quint32 drawnSize(quint32 size, qsizetype count)
{
    const quint64 drawn = quint64(size) * 5 / 4;
    const quint64 apps = count > 0 ? quint64(count) : 0;
    const quint64 divisor = 6 + (apps > 6 ? apps - 6 : 0) / 3;
    return quint32(std::min<quint64>(drawn * 6 / divisor, std::numeric_limits<quint32>::max()));
}

// riley dock/src/rules.rs:114
int iconLookupSize(quint32 drawn)
{
    const quint64 scaled = quint64(drawn) * 8;
    const quint64 ceiled = scaled / 5 + (scaled % 5 != 0 ? 1 : 0); // u64::div_ceil
    return int(std::min<quint64>(ceiled, quint64(std::numeric_limits<int>::max())));
}

// riley dock/src/rules.rs:118
QStringList screenFilters(const QString &config)
{
    QStringView value;
    for (QStringView line : rustLines(config)) {
        QStringView key, contents;
        if (splitOnceEquals(line, &key, &contents) && rustTrim(key) == u"SCREENS")
            value = contents;
    }
    QStringList filters;
    for (QStringView item : value.split(u',')) {
        const QStringView trimmed = rustTrim(item);
        if (trimmed.isEmpty())
            continue;
        const QString owned = trimmed.toString();
        if (!filters.contains(owned))
            filters.append(owned);
    }
    return filters;
}

// riley dock/src/rules.rs:132
bool reflectionsEnabled(const QString &config, bool allowed)
{
    bool enabled = true;
    for (QStringView line : rustLines(config)) {
        QStringView key, value;
        if (splitOnceEquals(line, &key, &value) && rustTrim(key) == u"REFLECTIONS")
            enabled = rustTrim(value) != u"0";
    }
    return allowed && enabled;
}

// riley dock/src/rules.rs:149
std::optional<ScreenSelection> startupSelection(std::optional<ScreenSelection> current,
                                                std::optional<ScreenSelection> &pending, bool configured,
                                                bool expired)
{
    if (current) {
        if (current->names.isEmpty())
            pending.reset();
        else if (!current->fallback || !configured)
            return current;
        else
            pending = std::move(current);
    }
    if (expired) {
        std::optional<ScreenSelection> taken = std::move(pending);
        pending.reset();
        return taken;
    }
    return std::nullopt;
}

// riley dock/src/rules.rs:159
ScreenSelection selectMonitors(const QList<Monitor> &monitors, const std::optional<QString> &output,
                               const QStringList &filters)
{
    if (output) {
        // Explicit test outputs must never silently put a dock on another screen.
        QStringList names;
        for (const Monitor &m : monitors) {
            if (m.name == *output)
                names.append(m.name);
        }
        return ScreenSelection{names, false};
    }
    QStringList names;
    for (const Monitor &m : monitors) {
        const bool match = std::any_of(filters.cbegin(), filters.cend(),
                                       [&](const QString &part) { return m.description.contains(part); });
        if (match)
            names.append(m.name);
    }
    if (!names.isEmpty())
        return ScreenSelection{names, false};
    // Iterator::min_by keeps the first of equal elements.
    const Monitor *best = nullptr;
    for (const Monitor &m : monitors) {
        if (!best) {
            best = &m;
            continue;
        }
        int order = m.y < best->y ? -1 : (m.y > best->y ? 1 : 0);
        if (order == 0)
            order = m.x < best->x ? -1 : (m.x > best->x ? 1 : 0);
        if (order == 0)
            order = compareCodePoints(m.name, best->name);
        if (order < 0)
            best = &m;
    }
    if (best)
        names.append(best->name);
    return ScreenSelection{names, !names.isEmpty()};
}

QDebug operator<<(QDebug debug, const Workspace &workspace)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "Workspace { id: " << workspace.id << ", name: " << workspace.name << " }";
    return debug;
}

QDebug operator<<(QDebug debug, const Client &client)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "Client { address: " << client.address << ", class: " << client.className
                    << ", title: " << client.title << ", workspace: " << client.workspace << " }";
    return debug;
}

QDebug operator<<(QDebug debug, const Monitor &monitor)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "Monitor { name: " << monitor.name << ", description: " << monitor.description
                    << ", x: " << monitor.x << ", y: " << monitor.y << " }";
    return debug;
}

QDebug operator<<(QDebug debug, const DockItem &item)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "DockItem { id: " << item.id << ", windows: " << item.windows << " }";
    return debug;
}

QDebug operator<<(QDebug debug, const ScreenSelection &selection)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "ScreenSelection { names: " << selection.names << ", fallback: " << selection.fallback
                    << " }";
    return debug;
}

namespace action {

QDebug operator<<(QDebug debug, const Action &action)
{
    QDebugStateSaver saver(debug);
    debug.nospace();
    std::visit(
        [&](const auto &a) {
            using T = std::decay_t<decltype(a)>;
            if constexpr (std::is_same_v<T, Launch>)
                debug << "Launch(" << a.id << ")";
            else if constexpr (std::is_same_v<T, Focus>)
                debug << "Focus(" << a.address << ")";
            else if constexpr (std::is_same_v<T, Minimize>)
                debug << "Minimize(" << a.address << ")";
            else if constexpr (std::is_same_v<T, Restore>)
                debug << "Restore(" << a.address << ")";
            else
                debug << "Menu(" << a.windows << ")";
        },
        action);
    return debug;
}

} // namespace action

} // namespace riley::dock::rules
