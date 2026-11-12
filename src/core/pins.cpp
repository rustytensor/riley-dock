// SPDX-License-Identifier: GPL-3.0-or-later
//
// Port of riley dock/src/pins.rs.
#include <riley-dock/pins.h>

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QList>
#include <QtCore/QStringDecoder>

#include <atomic>
#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace riley::dock::pins {

namespace {

// Rust io::Error Display for an OS error: "<strerror> (os error <n>)".
QString osError(int code)
{
    return QString::fromLocal8Bit(std::strerror(code)) + u" (os error "_s + QString::number(code) + u')';
}

void setError(QString *error, const QString &message)
{
    if (error)
        *error = message;
}

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
// as is; a final '\n' adds no empty line.
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

// Strict UTF-8 as Rust's str::from_utf8 accepts it: no overlong forms, no surrogates, nothing above U+10FFFF, no
// truncated sequence.
bool isValidUtf8(const QByteArray &bytes)
{
    const auto *s = reinterpret_cast<const unsigned char *>(bytes.constData());
    const qsizetype n = bytes.size();
    qsizetype i = 0;
    while (i < n) {
        const unsigned char c = s[i];
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

// A Unix path split as Rust's Path::components() does: an optional root, a leading "." kept only at the start of
// a relative path, empty and inner "." components dropped.
struct Components {
    bool root = false;
    QStringList parts; // Normal, ".." and a leading "."
};

Components components(const QString &path)
{
    Components result;
    result.root = path.startsWith(u'/');
    const QStringList pieces = path.split(u'/');
    for (qsizetype i = 0; i < pieces.size(); ++i) {
        const QString &piece = pieces[i];
        if (piece.isEmpty())
            continue;
        if (piece == u"."_s && !(i == 0 && !result.root))
            continue;
        result.parts.append(piece);
    }
    return result;
}

QString joinComponents(bool root, const QStringList &parts)
{
    return (root ? u"/"_s : QString()) + parts.join(u'/');
}

// Rust Path::parent(): None for "" and for a root alone; otherwise the path without its last component.
std::optional<QString> rustParent(const QString &path)
{
    const Components c = components(path);
    if (c.parts.isEmpty())
        return std::nullopt;
    return joinComponents(c.root, c.parts.mid(0, c.parts.size() - 1));
}

// Rust Path::join for a relative file name.
QString rustJoin(const QString &dir, const QString &name)
{
    if (dir.isEmpty() || dir.endsWith(u'/'))
        return dir + name;
    return dir + u'/' + name;
}

bool isDir(const QByteArray &path)
{
    struct stat st;
    return ::stat(path.constData(), &st) == 0 && S_ISDIR(st.st_mode);
}

// Rust std::fs::create_dir_all (DirBuilder, mode 0o777 before the umask).
int createDirAll(const QString &path)
{
    if (components(path).parts.isEmpty() && !components(path).root)
        return 0; // Path::new("") is Ok
    const QByteArray native = QFile::encodeName(path);
    if (::mkdir(native.constData(), 0777) == 0)
        return 0;
    int e = errno;
    if (e != ENOENT) {
        if (isDir(native))
            return 0;
        return e;
    }
    const std::optional<QString> parent = rustParent(path);
    if (!parent)
        return -1; // "failed to create whole tree"
    if (const int pe = createDirAll(*parent); pe != 0)
        return pe;
    if (::mkdir(native.constData(), 0777) == 0)
        return 0;
    e = errno;
    if (isDir(native))
        return 0;
    return e;
}

// Rust Write::write_all on a File: retries EINTR, a zero-length write is an error.
int writeAll(int fd, const QByteArray &bytes)
{
    const char *p = bytes.constData();
    qsizetype left = bytes.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, size_t(left));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return errno;
        }
        if (n == 0)
            return -2; // "failed to write whole buffer"
        p += n;
        left -= n;
    }
    return 0;
}

QString errorText(int code)
{
    if (code == -1)
        return u"failed to create whole tree"_s;
    if (code == -2)
        return u"failed to write whole buffer"_s;
    return osError(code);
}

} // namespace

// riley dock/src/pins.rs:6
std::optional<QStringList> load(const QString &path, QString *error)
{
    // fs::read_to_string
    const QByteArray native = QFile::encodeName(path);
    int fd;
    do
        fd = ::open(native.constData(), O_RDONLY | O_CLOEXEC);
    while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        const int e = errno;
        if (e == ENOENT)
            return QStringList(); // riley dock/src/pins.rs:9 NotFound
        setError(error, osError(e));
        return std::nullopt;
    }
    QByteArray bytes;
    char buffer[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            const int e = errno;
            ::close(fd);
            setError(error, osError(e));
            return std::nullopt;
        }
        if (n == 0)
            break;
        bytes.append(buffer, n);
    }
    ::close(fd);
    if (!isValidUtf8(bytes)) {
        setError(error, u"stream did not contain valid UTF-8"_s);
        return std::nullopt;
    }
    // Rust keeps a leading byte order mark as U+FEFF; so does this decoder.
    QStringDecoder decoder(QStringConverter::Utf8,
                           QStringConverter::Flag::ConvertInitialBom | QStringConverter::Flag::Stateless);
    const QString text = decoder.decode(bytes);
    QStringList result;
    for (QStringView line : rustLines(text)) {
        const QStringView trimmed = rustTrim(line);
        if (!trimmed.isEmpty())
            result.append(trimmed.toString());
    }
    return result;
}

// riley dock/src/pins.rs:14
bool save(const QString &path, const QStringList &pins, QString *error)
{
    static std::atomic<quint64> next{0};
    // riley dock/src/pins.rs:16 parent().filter(non-empty).unwrap_or(".")
    QString parent = rustParent(path).value_or(QString());
    if (parent.isEmpty())
        parent = u"."_s;
    if (const int e = createDirAll(parent); e != 0) {
        setError(error, errorText(e));
        return false;
    }
    // A unique sibling keeps concurrent saves separate and rename on one filesystem.
    QString temporary;
    int fd = -1;
    for (;;) {
        // The counter moves on only when the name is taken (EEXIST); EINTR retries the same name (Rust cvt_r).
        temporary = rustJoin(parent, u".dock-pins-"_s + QString::number(::getpid()) + u'-'
                                         + QString::number(next.fetch_add(1, std::memory_order_relaxed))
                                         + u".tmp"_s);
        do
            fd = ::open(QFile::encodeName(temporary).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
        while (fd < 0 && errno == EINTR);
        if (fd >= 0)
            break;
        const int e = errno;
        if (e == EEXIST)
            continue;
        setError(error, osError(e));
        return false;
    }
    const QByteArray nativeTemporary = QFile::encodeName(temporary);
    int failure = 0;
    for (const QString &pin : pins) {
        failure = writeAll(fd, pin.toUtf8() + '\n');
        if (failure != 0)
            break;
    }
    if (failure == 0) {
        while (::fsync(fd) != 0) {
            if (errno != EINTR) {
                failure = errno;
                break;
            }
        }
    }
    if (failure == 0 && ::rename(nativeTemporary.constData(), QFile::encodeName(path).constData()) != 0)
        failure = errno;
    if (failure != 0)
        ::unlink(nativeTemporary.constData());
    ::close(fd);
    if (failure != 0) {
        setError(error, errorText(failure));
        return false;
    }
    return true;
}

// riley dock/src/pins.rs:36
void pin(QStringList &pins, const QString &id)
{
    if (!pins.contains(id))
        pins.append(id);
}

// riley dock/src/pins.rs:40
void unpin(QStringList &pins, const QString &id)
{
    pins.removeAll(id);
}

} // namespace riley::dock::pins
