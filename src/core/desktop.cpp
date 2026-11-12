// SPDX-License-Identifier: GPL-3.0-or-later
//
// Port of riley dock/src/desktop.rs. The Rust code asks GIO (gio::DesktopAppInfo::new, gio::AppInfo::all) for
// desktop entries; this file reads the entries itself and follows the GIO and GLib rules those calls rely on:
// the XDG data directories, desktop-file ids, the key file syntax, the validity checks of a desktop entry
// (Type, TryExec, Exec), localised keys, the Hidden key and how an entry's icon is turned into a string.
#include <riley-dock/desktop.h>

#include <QtCore/QByteArray>
#include <QtCore/QDebugStateSaver>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QString>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace riley::dock::desktop {

namespace {

// ---------------------------------------------------------------------------------------------------------------
// Small byte helpers (GLib works on UTF-8 C strings).

bool asciiSpace(char c) // g_ascii_isspace
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

bool asciiControl(char c) // g_ascii_iscntrl
{
    const auto u = static_cast<unsigned char>(c);
    return u < 0x20 || u == 0x7F;
}

// g_utf8_validate: no overlong forms, no surrogates, nothing above U+10FFFF, no truncated sequence, no NUL.
bool validUtf8(QByteArrayView bytes)
{
    const auto *s = reinterpret_cast<const unsigned char *>(bytes.data());
    const qsizetype n = bytes.size();
    qsizetype i = 0;
    while (i < n) {
        const unsigned char c = s[i];
        if (c == 0)
            return false;
        if (c < 0x80) {
            ++i;
            continue;
        }
        int extra;
        unsigned char lo = 0x80, hi = 0xBF;
        if (c >= 0xC2 && c <= 0xDF) {
            extra = 1;
        } else if (c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            if (c == 0xE0)
                lo = 0xA0;
            else if (c == 0xED)
                hi = 0x9F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            if (c == 0xF0)
                lo = 0x90;
            else if (c == 0xF4)
                hi = 0x8F;
        } else {
            return false;
        }
        if (i + extra >= n)
            return false;
        if (s[i + 1] < lo || s[i + 1] > hi)
            return false;
        for (int k = 2; k <= extra; ++k) {
            if (s[i + k] < 0x80 || s[i + k] > 0xBF)
                return false;
        }
        i += extra + 1;
    }
    return true;
}

// g_build_filename for two elements: one '/' between them, empty elements ignored.
QByteArray buildFilename(const QByteArray &a, const QByteArray &b)
{
    if (a.isEmpty())
        return b;
    if (b.isEmpty())
        return a;
    QByteArray head = a;
    while (head.size() > 1 && head.endsWith('/'))
        head.chop(1);
    qsizetype skip = 0;
    while (skip < b.size() && b[skip] == '/')
        ++skip;
    const QByteArray tail = b.mid(skip);
    if (head == "/")
        return head + tail;
    return head + '/' + tail;
}

QByteArray environment(const char *name)
{
    const char *value = std::getenv(name);
    return value ? QByteArray(value) : QByteArray();
}

bool isDirectory(const QByteArray &path) // g_file_test (G_FILE_TEST_IS_DIR), follows symbolic links
{
    struct stat st;
    return ::stat(path.constData(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isExecutable(const QByteArray &path) // g_file_test (G_FILE_TEST_IS_EXECUTABLE)
{
    if (::access(path.constData(), X_OK) != 0)
        return false;
    if (::getuid() != 0)
        return true;
    // For root, access() may succeed without any execute bit; GLib then checks the mode bits.
    struct stat st;
    return ::stat(path.constData(), &st) == 0 && (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
}

// ---------------------------------------------------------------------------------------------------------------
// GLib g_find_program_for_path (program, NULL, working_dir), used by GIO for TryExec and Exec.

bool findProgram(const QByteArray &program, const std::optional<QByteArray> &workingDir)
{
    const bool absolute = program.startsWith('/');
    if (absolute || program.contains('/')) {
        QByteArray candidate = program;
        if (!absolute && workingDir)
            candidate = buildFilename(*workingDir, program);
        if (isExecutable(candidate) && !isDirectory(candidate))
            return true;
        if (absolute)
            return false;
        // A relative path with a directory part is then also looked for along PATH, as GLib does.
    }
    const char *pathEnv = std::getenv("PATH");
    const QByteArray path = pathEnv ? QByteArray(pathEnv) : QByteArray("/bin:/usr/bin:.");
    qsizetype start = 0;
    for (;;) {
        qsizetype end = path.indexOf(':', start);
        if (end < 0)
            end = path.size();
        const QByteArray dir = path.mid(start, end - start);
        // An empty element (adjacent colons, or one at either end) means the current directory.
        QByteArray candidate = dir.isEmpty() ? program : dir + '/' + program;
        if (workingDir && !candidate.startsWith('/'))
            candidate = buildFilename(*workingDir, candidate);
        if (isExecutable(candidate) && !isDirectory(candidate))
            return true;
        if (end >= path.size())
            break;
        start = end + 1;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// GLib g_shell_parse_argv: the Exec value must tokenize and unquote without error; its first word is the program.

std::optional<QList<QByteArray>> tokenizeCommandLine(const QByteArray &commandLine)
{
    QList<QByteArray> tokens;
    std::optional<QByteArray> current;
    char currentQuote = '\0';
    bool quoted = false;
    const qsizetype n = commandLine.size();
    auto delimit = [&] {
        if (current) {
            tokens.append(*current);
            current.reset();
        }
    };
    auto append = [&](char c) {
        if (!current)
            current = QByteArray();
        current->append(c);
    };
    for (qsizetype i = 0; i < n; ++i) {
        const char c = commandLine[i];
        if (currentQuote == '\\') {
            // Backslash-newline becomes nothing; otherwise both are kept for the unquote step.
            if (c != '\n') {
                append('\\');
                append(c);
            }
            currentQuote = '\0';
        } else if (currentQuote == '#') {
            // Discard up to and including the next newline.
            while (i < n && commandLine[i] != '\n')
                ++i;
            currentQuote = '\0';
            if (i >= n)
                break;
        } else if (currentQuote) {
            if (c == currentQuote && !(currentQuote == '"' && quoted))
                currentQuote = '\0';
            append(c);
        } else {
            switch (c) {
            case '\n':
                delimit();
                break;
            case ' ':
            case '\t':
                if (current && !current->isEmpty())
                    delimit();
                break;
            case '\'':
            case '"':
                append(c);
                currentQuote = c;
                break;
            case '\\':
                currentQuote = c;
                break;
            case '#':
                if (i == 0) {
                    currentQuote = c;
                    break;
                }
                switch (commandLine[i - 1]) {
                case ' ':
                case '\n':
                case '\0':
                    currentQuote = c;
                    break;
                default:
                    append(c);
                    break;
                }
                break;
            default:
                append(c);
                break;
            }
        }
        // Consecutive backslashes counted mod 2 detect an escaped double quote.
        const char last = i < n ? commandLine[i] : '\0';
        if (last != '\\')
            quoted = false;
        else
            quoted = !quoted;
    }
    delimit();
    if (currentQuote)
        return std::nullopt; // text ended after a backslash, or before a matching quote
    if (tokens.isEmpty())
        return std::nullopt; // empty, or only whitespace
    return tokens;
}

std::optional<QByteArray> shellUnquote(const QByteArray &quotedString)
{
    QByteArray result;
    const qsizetype n = quotedString.size();
    qsizetype i = 0;
    while (i < n) {
        while (i < n && quotedString[i] != '"' && quotedString[i] != '\'') {
            if (quotedString[i] == '\\') {
                ++i;
                if (i < n) {
                    if (quotedString[i] != '\n')
                        result.append(quotedString[i]);
                    ++i;
                }
            } else {
                result.append(quotedString[i]);
                ++i;
            }
        }
        if (i >= n)
            break;
        const char quote = quotedString[i];
        ++i;
        bool closed = false;
        if (quote == '"') {
            while (i < n) {
                const char c = quotedString[i];
                if (c == '"') {
                    ++i;
                    closed = true;
                    break;
                }
                if (c == '\\') {
                    ++i;
                    const char next = i < n ? quotedString[i] : '\0';
                    switch (next) {
                    case '"':
                    case '\\':
                    case '`':
                    case '$':
                    case '\n':
                        result.append(next);
                        ++i;
                        break;
                    default:
                        result.append('\\');
                        break;
                    }
                } else {
                    result.append(c);
                    ++i;
                }
            }
        } else {
            while (i < n) {
                const char c = quotedString[i];
                ++i;
                if (c == '\'') {
                    closed = true;
                    break;
                }
                result.append(c);
            }
        }
        if (!closed)
            return std::nullopt; // unmatched quotation mark
    }
    return result;
}

std::optional<QByteArray> shellArgv0(const QByteArray &commandLine)
{
    const auto tokens = tokenizeCommandLine(commandLine);
    if (!tokens)
        return std::nullopt;
    std::optional<QByteArray> first;
    for (const QByteArray &token : *tokens) {
        const auto word = shellUnquote(token);
        if (!word)
            return std::nullopt;
        if (!first)
            first = *word;
    }
    return first;
}

// ---------------------------------------------------------------------------------------------------------------
// GLib GKeyFile, as g_key_file_load_from_file (G_KEY_FILE_NONE) reads a desktop file.

struct KeyFile {
    QByteArray startGroup;
    bool hasStartGroup = false;
    QHash<QByteArray, QHash<QByteArray, QByteArray>> groups; // raw values; a repeated key keeps the last one

    // g_key_file_get_string: the raw value must be valid UTF-8 and unescape cleanly.
    std::optional<QByteArray> string(const QByteArray &group, const QByteArray &key) const;
    // g_key_file_get_locale_string with the process language list.
    std::optional<QByteArray> localeString(const QByteArray &group, const QByteArray &key,
                                           const QList<QByteArray> &languages) const;
    // g_key_file_get_boolean; an absent or invalid value reads as false.
    bool boolean(const QByteArray &group, const QByteArray &key) const;

private:
    const QByteArray *raw(const QByteArray &group, const QByteArray &key) const
    {
        const auto g = groups.constFind(group);
        if (g == groups.constEnd())
            return nullptr;
        const auto v = g->constFind(key);
        return v == g->constEnd() ? nullptr : &*v;
    }
};

std::optional<QByteArray> KeyFile::string(const QByteArray &group, const QByteArray &key) const
{
    const QByteArray *value = raw(group, key);
    if (!value || !validUtf8(*value))
        return std::nullopt;
    QByteArray result;
    for (qsizetype i = 0; i < value->size(); ++i) {
        const char c = (*value)[i];
        if (c != '\\') {
            result.append(c);
            continue;
        }
        ++i;
        if (i >= value->size())
            return std::nullopt; // escape character at end of line
        switch ((*value)[i]) {
        case 's': result.append(' '); break;
        case 'n': result.append('\n'); break;
        case 't': result.append('\t'); break;
        case 'r': result.append('\r'); break;
        case '\\': result.append('\\'); break;
        default: return std::nullopt; // invalid escape sequence
        }
    }
    return result;
}

std::optional<QByteArray> KeyFile::localeString(const QByteArray &group, const QByteArray &key,
                                                const QList<QByteArray> &languages) const
{
    for (const QByteArray &language : languages) {
        // GLib never uses a "Key[C]" translation; the language list ends at its first "C".
        if (language == "C")
            break;
        if (auto value = string(group, key + '[' + language + ']'))
            return value;
    }
    return string(group, key);
}

bool KeyFile::boolean(const QByteArray &group, const QByteArray &key) const
{
    const QByteArray *value = raw(group, key);
    if (!value)
        return false;
    qsizetype length = 0; // up to the last non-whitespace byte
    for (qsizetype i = 0; i < value->size(); ++i) {
        if (!asciiSpace((*value)[i]))
            length = i + 1;
    }
    const QByteArray word = value->left(length);
    return word == "true" || word == "1";
}

// g_key_file_is_key_name
bool isKeyName(const QByteArray &name)
{
    const qsizetype n = name.size();
    qsizetype q = 0;
    while (q < n && name[q] != '=' && name[q] != '[' && name[q] != ']')
        ++q;
    if (q == 0)
        return false; // no empty keys
    if (name[0] == ' ' || name[q - 1] == ' ')
        return false;
    if (q < n && name[q] == '[') {
        ++q;
        while (q < n) {
            const char c = name[q];
            if (c == '-' || c == '_' || c == '.' || c == '@') {
                ++q;
                continue;
            }
            const auto u = static_cast<unsigned char>(c);
            if (u < 0x80) {
                if (!std::isalnum(u))
                    break;
                ++q;
                continue;
            }
            // A non-ASCII character: it must be valid UTF-8 and alphanumeric (g_unichar_isalnum: L* or Nd).
            const int extra = u >= 0xF0 ? 3 : u >= 0xE0 ? 2 : u >= 0xC0 ? 1 : -1;
            if (extra < 0 || q + extra >= n || !validUtf8(QByteArrayView(name).sliced(q, extra + 1)))
                break;
            const QString ch = QString::fromUtf8(QByteArrayView(name).sliced(q, extra + 1));
            const char32_t ucs = ch.size() == 2 ? QChar::surrogateToUcs4(ch[0], ch[1]) : char32_t(ch[0].unicode());
            const QChar::Category category = QChar::category(ucs);
            const bool alnum = category == QChar::Letter_Uppercase || category == QChar::Letter_Lowercase
                || category == QChar::Letter_Titlecase || category == QChar::Letter_Modifier
                || category == QChar::Letter_Other || category == QChar::Number_DecimalDigit;
            if (!alnum)
                break;
            q += extra + 1;
        }
        if (q >= n || name[q] != ']')
            return false;
        ++q;
    }
    return q == n;
}

// g_key_file_is_group_name
bool isGroupName(const QByteArray &name)
{
    if (name.isEmpty())
        return false;
    for (char c : name) {
        if (c == '[' || c == ']' || asciiControl(c))
            return false;
    }
    return true;
}

std::optional<KeyFile> parseKeyFile(const QByteArray &data)
{
    KeyFile file;
    std::optional<QByteArray> currentGroup; // none before the first group line
    auto parseLine = [&](QByteArray line) -> bool {
        qsizetype start = 0;
        while (start < line.size() && asciiSpace(line[start]))
            ++start;
        line = line.mid(start);
        if (line.isEmpty() || line[0] == '#')
            return true; // comment or blank line
        if (line[0] == '[') {
            const qsizetype close = line.indexOf(']', 1);
            if (close >= 0) {
                qsizetype after = close + 1;
                while (after < line.size() && (line[after] == ' ' || line[after] == '\t'))
                    ++after;
                if (after == line.size()) {
                    const QByteArray name = line.mid(1, line.lastIndexOf(']') - 1);
                    if (!isGroupName(name))
                        return false;
                    if (!file.hasStartGroup) {
                        file.startGroup = name;
                        file.hasStartGroup = true;
                    }
                    file.groups[name]; // a repeated group continues the earlier one
                    currentGroup = name;
                    return true;
                }
            }
        }
        const qsizetype eq = line.indexOf('=');
        if (eq <= 0)
            return false; // not a key-value pair, group or comment
        if (!currentGroup)
            return false; // key file does not start with a group
        qsizetype keyEnd = eq;
        while (keyEnd > 0 && asciiSpace(line[keyEnd - 1]))
            --keyEnd;
        const QByteArray key = line.left(keyEnd);
        qsizetype valueStart = eq + 1;
        while (valueStart < line.size() && asciiSpace(line[valueStart]))
            ++valueStart;
        const QByteArray value = line.mid(valueStart);
        if (!isKeyName(key))
            return false;
        if (*currentGroup == file.startGroup && key == "Encoding" && value.compare("UTF-8", Qt::CaseInsensitive) != 0)
            return false; // unsupported encoding
        file.groups[*currentGroup].insert(key, value);
        return true;
    };
    qsizetype i = 0;
    const qsizetype n = data.size();
    while (i < n) {
        const qsizetype nl = data.indexOf('\n', i);
        if (nl < 0) {
            // The last line without a newline keeps a trailing '\r'.
            if (!parseLine(data.mid(i)))
                return std::nullopt;
            break;
        }
        QByteArray line = data.mid(i, nl - i);
        if (line.endsWith('\r'))
            line.chop(1);
        if (!parseLine(line))
            return std::nullopt;
        i = nl + 1;
    }
    return file;
}

// ---------------------------------------------------------------------------------------------------------------
// GLib g_get_language_names (): the LC_MESSAGES language list with every locale variant, then "C".

const QHash<QByteArray, QByteArray> &localeAliases()
{
    static QHash<QByteArray, QByteArray> table;
    static std::once_flag once;
    std::call_once(once, [] {
        // GLib read_aliases: first column (ended by tab, space or ':'), second column; the first entry wins.
        QFile file(u"/usr/share/locale/locale.alias"_s);
        if (!file.open(QIODevice::ReadOnly))
            return;
        const QList<QByteArray> lines = file.readAll().split('\n');
        for (QByteArray line : lines) {
            line = line.trimmed();
            if (line.isEmpty() || line[0] == '#')
                continue;
            qsizetype p = 0;
            while (p < line.size() && line[p] != '\t' && line[p] != ' ' && line[p] != ':')
                ++p;
            if (p >= line.size())
                continue; // one column only
            const QByteArray first = line.left(p);
            qsizetype q = p + 1;
            while (q < line.size() && (line[q] == '\t' || line[q] == ' '))
                ++q;
            if (q >= line.size())
                continue;
            qsizetype e = q;
            while (e < line.size() && line[e] != '\t' && line[e] != ' ')
                ++e;
            if (!table.contains(first))
                table.insert(first, line.mid(q, e - q));
        }
    });
    return table;
}

QByteArray unaliasLanguage(QByteArray language)
{
    const auto &aliases = localeAliases();
    for (int level = 0; level <= 30; ++level) {
        const auto it = aliases.constFind(language);
        if (it == aliases.constEnd() || *it == language)
            break;
        language = *it;
    }
    return language;
}

void appendLocaleVariants(QList<QByteArray> &list, const QByteArray &locale)
{
    enum { Codeset = 1 << 0, Territory = 1 << 1, Modifier = 1 << 2 };
    const qsizetype underscore = locale.indexOf('_');
    const qsizetype dot = locale.indexOf('.', underscore >= 0 ? underscore : 0);
    const qsizetype at = locale.indexOf('@', dot >= 0 ? dot : (underscore >= 0 ? underscore : 0));
    unsigned mask = 0;
    QByteArray modifier, codeset, territory;
    qsizetype atPos = locale.size();
    if (at >= 0) {
        mask |= Modifier;
        modifier = locale.mid(at);
        atPos = at;
    }
    qsizetype dotPos = atPos;
    if (dot >= 0) {
        mask |= Codeset;
        codeset = locale.mid(dot, atPos - dot);
        dotPos = dot;
    }
    qsizetype underscorePos = dotPos;
    if (underscore >= 0) {
        mask |= Territory;
        territory = locale.mid(underscore, dotPos - underscore);
        underscorePos = underscore;
    }
    const QByteArray language = locale.left(underscorePos);
    for (unsigned j = 0; j <= mask; ++j) {
        const unsigned v = mask - j;
        if ((v & ~mask) == 0)
            list.append(language + ((v & Territory) ? territory : QByteArray())
                        + ((v & Codeset) ? codeset : QByteArray()) + ((v & Modifier) ? modifier : QByteArray()));
    }
}

QList<QByteArray> languageNames()
{
    QByteArray value;
    for (const char *name : {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"}) {
        value = environment(name);
        if (!value.isEmpty())
            break;
    }
    if (value.isEmpty())
        value = "C";
    QList<QByteArray> list;
    for (const QByteArray &piece : value.split(':'))
        appendLocaleVariants(list, unaliasLanguage(piece));
    list.append("C");
    return list;
}

// ---------------------------------------------------------------------------------------------------------------
// GIO GDesktopAppInfo

// What g_desktop_app_info_load_from_keyfile keeps of a usable entry.
struct Entry {
    std::optional<QByteArray> name;
    std::optional<QByteArray> icon;
    QByteArray exec;
    std::optional<QByteArray> startupWmClass;
    bool hidden = false;
};

// g_desktop_app_info_new_from_filename: NULL unless the key file loads, starts with [Desktop Entry], has
// Type=Application, and TryExec (if not empty) and the program of Exec (if not empty) are found.
std::optional<Entry> loadEntry(const QByteArray &filename, const QList<QByteArray> &languages)
{
    QFile file(QFile::decodeName(filename));
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const auto keyFile = parseKeyFile(file.readAll());
    if (!keyFile || !keyFile->hasStartGroup || keyFile->startGroup != "Desktop Entry")
        return std::nullopt;
    const QByteArray group = "Desktop Entry";
    if (keyFile->string(group, "Type") != QByteArray("Application"))
        return std::nullopt;
    const std::optional<QByteArray> workingDir = keyFile->string(group, "Path");
    const std::optional<QByteArray> tryExec = keyFile->string(group, "TryExec");
    if (tryExec && !tryExec->isEmpty() && !findProgram(*tryExec, workingDir))
        return std::nullopt;
    const std::optional<QByteArray> exec = keyFile->string(group, "Exec");
    if (exec && !exec->isEmpty()) {
        const auto program = shellArgv0(*exec);
        if (!program || !findProgram(*program, workingDir))
            return std::nullopt;
    }
    Entry entry;
    entry.name = keyFile->localeString(group, "Name", languages);
    entry.hidden = keyFile->boolean(group, "Hidden");
    entry.icon = keyFile->localeString(group, "Icon", languages);
    entry.exec = exec.value_or(QByteArray());
    entry.startupWmClass = keyFile->string(group, "StartupWMClass");
    return entry;
}

// GLib g_canonicalize_filename for an absolute path: "." and ".." resolved by name, repeated '/' collapsed, no
// trailing '/'; exactly two leading slashes are kept (POSIX), more become one.
QByteArray canonicalizeAbsolute(const QByteArray &path)
{
    qsizetype slashes = 0;
    while (slashes < path.size() && path[slashes] == '/')
        ++slashes;
    const QByteArray root = slashes == 2 ? QByteArray("//") : QByteArray("/");
    QList<QByteArray> parts;
    for (const QByteArray &part : path.mid(slashes).split('/')) {
        if (part.isEmpty() || part == ".")
            continue;
        if (part == "..") {
            if (!parts.isEmpty())
                parts.removeLast();
            continue;
        }
        parts.append(part);
    }
    QByteArray result = root;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        if (i > 0)
            result += '/';
        result += parts[i];
    }
    return result;
}

// g_string_append_uri_escaped (s, token, G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, TRUE)
QByteArray uriEscaped(const QByteArray &token)
{
    static const char hex[] = "0123456789ABCDEF";
    QByteArray result;
    for (qsizetype i = 0; i < token.size(); ++i) {
        const auto u = static_cast<unsigned char>(token[i]);
        if (u >= 0x80) {
            result.append(char(u)); // valid UTF-8 is kept as is (key file strings are valid UTF-8)
            continue;
        }
        if (std::isalnum(u) || std::strchr("-._~!$&'()*+,;=:@/", u)) {
            result.append(char(u));
            continue;
        }
        result.append('%');
        result.append(hex[u >> 4]);
        result.append(hex[u & 15]);
    }
    return result;
}

// AppInfo::icon().to_string() for the Icon value GIO loaded: an absolute path is a file icon (its canonical path);
// any other name loses a ".png", ".xpm" or ".svg" suffix and is a themed icon, which prints as its name unless the
// name starts with '.', when GIO prints the tokenized form with the name and its symbolic variant.
QString iconString(const QByteArray &icon)
{
    if (icon.startsWith('/'))
        return QString::fromUtf8(canonicalizeAbsolute(icon));
    QByteArray name = icon;
    const qsizetype dot = name.lastIndexOf('.');
    if (dot >= 0) {
        const QByteArray suffix = name.mid(dot);
        if (suffix == ".png" || suffix == ".xpm" || suffix == ".svg")
            name.truncate(dot);
    }
    if (!name.startsWith('.'))
        return QString::fromUtf8(name);
    const QByteArray variant = name.endsWith("-symbolic") ? name.chopped(9) : name + "-symbolic";
    return QString::fromUtf8(". GThemedIcon " + uriEscaped(name) + ' ' + uriEscaped(variant));
}

AppInfo makeInfo(const QByteArray &desktopId, const QByteArray &filename, const Entry &entry)
{
    AppInfo info;
    info.id = QString::fromUtf8(desktopId);
    // g_desktop_app_info_get_name falls back to the GLib message "Unnamed" (untranslated here).
    info.name = entry.name ? QString::fromUtf8(*entry.name) : u"Unnamed"_s;
    info.icon = entry.icon ? iconString(*entry.icon) : QString();
    info.filename = QFile::decodeName(filename);
    info.exec = QString::fromUtf8(entry.exec);
    info.startupWmClass = entry.startupWmClass ? QString::fromUtf8(*entry.startupWmClass) : QString();
    return info;
}

// One applications directory and its desktop-file ids (GIO desktop_file_dir, unindexed).
struct DesktopDir {
    QByteArray path;
    QHash<QByteArray, QByteArray> appNames; // desktop-file id -> file name
};

// GIO get_apps_from_dir: every "*.desktop" name is an id (prefix + name); other directories are entered with the
// prefix "<name>-". Directory entries are read in readdir order; a later id replaces an earlier equal one.
void scanApplications(QHash<QByteArray, QByteArray> &apps, const QByteArray &dirname, const QByteArray &prefix)
{
    DIR *dir = ::opendir(dirname.constData());
    if (!dir)
        return;
    while (const dirent *ent = ::readdir(dir)) {
        const QByteArray basename(ent->d_name);
        if (basename == "." || basename == "..")
            continue;
        const QByteArray filename = buildFilename(dirname, basename);
        if (basename.endsWith(".desktop"))
            apps.insert(prefix + basename, filename);
        else if (isDirectory(filename))
            scanApplications(apps, filename, prefix + basename + '-');
    }
    ::closedir(dir);
}

QByteArray homeDir() // g_get_home_dir
{
    QByteArray home = environment("HOME");
    if (!home.isEmpty())
        return home;
    if (const passwd *pw = ::getpwuid(::getuid()); pw && pw->pw_dir)
        return QByteArray(pw->pw_dir);
    return "/";
}

// GIO desktop_file_dirs: $XDG_DATA_HOME/applications (default ~/.local/share/applications), then each
// $XDG_DATA_DIRS element + "/applications" (default /usr/local/share/:/usr/share/), empty elements included.
QList<DesktopDir> desktopDirs()
{
    QList<QByteArray> dataDirs;
    QByteArray user = environment("XDG_DATA_HOME");
    if (user.isEmpty())
        user = buildFilename(buildFilename(homeDir(), ".local"), "share");
    dataDirs.append(user);
    QByteArray system = environment("XDG_DATA_DIRS");
    if (system.isEmpty())
        system = "/usr/local/share/:/usr/share/";
    dataDirs.append(system.split(':'));
    QList<DesktopDir> dirs;
    for (const QByteArray &dataDir : dataDirs) {
        DesktopDir dir;
        dir.path = buildFilename(dataDir, "applications");
        scanApplications(dir.appNames, dir.path, QByteArray());
        dirs.append(std::move(dir));
    }
    return dirs;
}

// riley dock/src/desktop.rs:6 struct Cache. The directory scan and language list stand for GIO's own caches and
// are dropped with the rest by invalidate().
struct Cache {
    bool ready = false;
    QList<DesktopDir> dirs;
    QList<QByteArray> languages;
    QHash<QString, std::optional<AppInfo>> ids;
    std::optional<QHash<QString, AppInfo>> classes;

    void setup()
    {
        if (ready)
            return;
        dirs = desktopDirs();
        languages = languageNames();
        ready = true;
    }

    // gio::DesktopAppInfo::new (g_desktop_app_info_new): the first directory whose file for the id loads; a
    // Hidden entry is then not returned.
    std::optional<AppInfo> byDesktopId(const QByteArray &desktopId)
    {
        setup();
        for (const DesktopDir &dir : std::as_const(dirs)) {
            const auto it = dir.appNames.constFind(desktopId);
            if (it == dir.appNames.constEnd())
                continue;
            const auto entry = loadEntry(*it, languages);
            if (!entry)
                continue;
            if (entry->hidden)
                return std::nullopt;
            return makeInfo(desktopId, *it, *entry);
        }
        return std::nullopt;
    }

    // gio::AppInfo::all (g_app_info_get_all): for each id the highest directory that has it decides; an entry that
    // does not load or is Hidden is left out (and hides the same id in lower directories). GIO returns them in
    // hash table order; this port uses directory order, then id order.
    QList<std::pair<AppInfo, std::optional<QByteArray>>> all()
    {
        setup();
        QList<std::pair<AppInfo, std::optional<QByteArray>>> result; // each with its StartupWMClass
        QHash<QByteArray, bool> seen;
        for (const DesktopDir &dir : std::as_const(dirs)) {
            QList<QByteArray> ids = dir.appNames.keys();
            std::sort(ids.begin(), ids.end());
            for (const QByteArray &id : std::as_const(ids)) {
                if (seen.contains(id))
                    continue;
                seen.insert(id, true);
                const QByteArray &filename = dir.appNames.value(id);
                const auto entry = loadEntry(filename, languages);
                if (entry && !entry->hidden)
                    result.append({makeInfo(id, filename, *entry), entry->startupWmClass});
            }
        }
        return result;
    }
};

// riley dock/src/desktop.rs:11 thread_local! CACHE
thread_local Cache cache;

} // namespace

// riley dock/src/desktop.rs:14
void invalidate()
{
    cache = Cache();
}

// riley dock/src/desktop.rs:18
std::optional<AppInfo> appInfo(const QString &id)
{
    if (const auto it = cache.ids.constFind(id); it != cache.ids.constEnd())
        return *it;
    std::optional<AppInfo> found;
    for (const QString &candidate : {id, id.toLower()}) {
        if (auto info = cache.byDesktopId((candidate + u".desktop"_s).toUtf8())) {
            found = std::move(info);
            break;
        }
    }
    if (!found) {
        if (!cache.classes) {
            QHash<QString, AppInfo> classes;
            for (const auto &[info, startupWmClass] : cache.all()) {
                // riley dock/src/desktop.rs:34 only entries with a StartupWMClass; the first one per class wins.
                if (!startupWmClass)
                    continue;
                const QString key = QString::fromUtf8(*startupWmClass).toLower();
                if (!classes.contains(key))
                    classes.insert(key, info);
            }
            cache.classes = std::move(classes);
        }
        if (const auto it = cache.classes->constFind(id.toLower()); it != cache.classes->constEnd())
            found = *it;
    }
    cache.ids.insert(id, found);
    return found;
}

QDebug operator<<(QDebug debug, const AppInfo &info)
{
    QDebugStateSaver saver(debug);
    debug.nospace() << "AppInfo { id: " << info.id << ", name: " << info.name << ", icon: " << info.icon
                    << ", filename: " << info.filename << ", exec: " << info.exec
                    << ", startupWmClass: " << info.startupWmClass << " }";
    return debug;
}

} // namespace riley::dock::desktop
