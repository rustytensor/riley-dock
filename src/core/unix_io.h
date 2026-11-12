// SPDX-License-Identifier: GPL-3.0-or-later
//
// Private to the core (not installed, not part of the API): the Rust std behaviours that riley dock/src/ipc.rs and
// riley dock/src/control.rs rely on through std::os::unix::net::UnixStream, std::fs and std::io::Read, written once
// for ipc.cpp and control.cpp.
#pragma once

#include <riley-dock/ipc.h>

#include <QtCore/QByteArray>
#include <QtCore/QByteArrayView>
#include <QtCore/QFile>
#include <QtCore/QString>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <chrono>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

namespace riley::dock::unix_io {

// Rust io::Error::other(message).
inline IoError other(const QString &message)
{
    return IoError{IoErrorKind::Other, message};
}

// Rust core::str::from_utf8: strict UTF-8 (no overlong forms, no surrogates, nothing above U+10FFFF).
inline bool validUtf8(QByteArrayView bytes)
{
    const qsizetype n = bytes.size();
    qsizetype i = 0;
    while (i < n) {
        const uchar c = uchar(bytes[i]);
        if (c < 0x80) {
            ++i;
            continue;
        }
        int length = 0;
        uchar low = 0x80, high = 0xBF; // allowed range of the second byte
        if (c >= 0xC2 && c <= 0xDF) {
            length = 2;
        } else if (c == 0xE0) {
            length = 3;
            low = 0xA0;
        } else if ((c >= 0xE1 && c <= 0xEC) || c == 0xEE || c == 0xEF) {
            length = 3;
        } else if (c == 0xED) {
            length = 3;
            high = 0x9F;
        } else if (c == 0xF0) {
            length = 4;
            low = 0x90;
        } else if (c >= 0xF1 && c <= 0xF3) {
            length = 4;
        } else if (c == 0xF4) {
            length = 4;
            high = 0x8F;
        } else {
            return false;
        }
        if (i + length > n)
            return false;
        const uchar second = uchar(bytes[i + 1]);
        if (second < low || second > high)
            return false;
        for (int k = 2; k < length; ++k) {
            const uchar next = uchar(bytes[i + k]);
            if (next < 0x80 || next > 0xBF)
                return false;
        }
        i += length;
    }
    return true;
}

// Rust's path -> CString conversion for std::fs calls: an interior NUL is refused (Rust: InvalidInput, "file name
// contained an unexpected NUL byte"; this library has no InvalidInput kind, so Other).
inline IoResult<QByteArray> encodePath(const QString &path)
{
    QByteArray bytes = QFile::encodeName(path);
    if (bytes.contains('\0'))
        return other(QStringLiteral("file name contained an unexpected NUL byte"));
    return bytes;
}

// Rust std::fs::symlink_metadata (follow = false, lstat) and std::fs::metadata (follow = true, stat).
inline IoResult<struct stat> statPath(const QString &path, bool follow)
{
    const auto name = encodePath(path);
    if (!name.ok())
        return name.error();
    struct stat st {};
    const int result = follow ? ::stat(name.value().constData(), &st) : ::lstat(name.value().constData(), &st);
    if (result != 0)
        return IoError::fromErrno(errno);
    return st;
}

// Rust std::fs::remove_file: unlink(2).
inline IoResult<> removeFile(const QString &path)
{
    const auto name = encodePath(path);
    if (!name.ok())
        return name.error();
    if (::unlink(name.value().constData()) != 0)
        return IoError::fromErrno(errno);
    return std::monostate{};
}

// Rust std::os::unix::net sockaddr_un(path).
inline IoResult<std::pair<sockaddr_un, socklen_t>> socketAddress(const QString &path)
{
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const QByteArray bytes = QFile::encodeName(path);
    if (bytes.contains('\0'))
        return other(QStringLiteral("paths must not contain interior null bytes"));
    if (std::size_t(bytes.size()) >= sizeof(address.sun_path))
        return other(QStringLiteral("path must be shorter than SUN_LEN"));
    std::memcpy(address.sun_path, bytes.constData(), std::size_t(bytes.size()));
    socklen_t length = socklen_t(offsetof(sockaddr_un, sun_path) + std::size_t(bytes.size()));
    if (!bytes.isEmpty() && bytes[0] != '\0')
        ++length;
    return std::pair{address, length};
}

// Rust UnixStream::connect(path).
inline IoResult<ipc::UnixStream> connectUnix(const QString &path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return IoError::fromErrno(errno);
    ipc::UnixStream stream(fd);
    const auto address = socketAddress(path);
    if (!address.ok())
        return address.error();
    if (::connect(fd, reinterpret_cast<const sockaddr *>(&address.value().first), address.value().second) != 0)
        return IoError::fromErrno(errno);
    return stream;
}

// Rust UnixStream::pair(): a connected socket pair.
inline IoResult<std::pair<ipc::UnixStream, ipc::UnixStream>> socketPair()
{
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) != 0)
        return IoError::fromErrno(errno);
    return std::pair{ipc::UnixStream(fds[0]), ipc::UnixStream(fds[1])};
}

// Rust UnixStream::set_nonblocking(true): ioctl FIONBIO.
inline IoResult<> setNonblocking(int fd)
{
    int on = 1;
    if (::ioctl(fd, FIONBIO, &on) != 0)
        return IoError::fromErrno(errno);
    return std::monostate{};
}

// Rust UnixStream::set_read_timeout / set_write_timeout(Some(duration)): a zero duration is refused (Rust:
// InvalidInput, "cannot set a 0 duration timeout"); a duration under one microsecond becomes one microsecond.
// `option` is SO_RCVTIMEO or SO_SNDTIMEO. A negative duration, which Rust's Duration cannot hold, is refused like
// zero.
inline IoResult<> setTimeout(int fd, int option, std::chrono::milliseconds timeout)
{
    if (timeout.count() <= 0)
        return other(QStringLiteral("cannot set a 0 duration timeout"));
    timeval value {};
    value.tv_sec = time_t(timeout.count() / 1000);
    value.tv_usec = suseconds_t((timeout.count() % 1000) * 1000);
    if (value.tv_sec == 0 && value.tv_usec == 0)
        value.tv_usec = 1;
    if (::setsockopt(fd, SOL_SOCKET, option, &value, sizeof value) != 0)
        return IoError::fromErrno(errno);
    return std::monostate{};
}

// Rust (&mut stream).take(limit).read_to_end(&mut buffer): appends until end of file or until `limit` bytes were
// read; EINTR is retried; any other read error stops and is returned, the bytes read so far kept in `buffer`.
inline IoResult<> readToEnd(int fd, qsizetype limit, QByteArray &buffer)
{
    qsizetype remaining = limit;
    char chunk[4096];
    while (remaining > 0) {
        const std::size_t want = std::size_t(std::min<qsizetype>(remaining, qsizetype(sizeof chunk)));
        const ssize_t got = ::read(fd, chunk, want);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            return IoError::fromErrno(errno);
        }
        if (got == 0)
            break;
        buffer.append(chunk, qsizetype(got));
        remaining -= qsizetype(got);
    }
    return std::monostate{};
}

// Rust stream.take(limit).read_to_string(&mut reply) into an empty String (std::io::append_to_string): the bytes
// must be UTF-8; a read error is returned as it is, otherwise invalid UTF-8 is InvalidData ("stream did not contain
// valid UTF-8").
inline IoResult<QByteArray> readToString(int fd, qsizetype limit)
{
    QByteArray buffer;
    const IoResult<> read = readToEnd(fd, limit, buffer);
    if (!read.ok())
        return read.error();
    if (!validUtf8(buffer))
        return IoError{IoErrorKind::InvalidData, QStringLiteral("stream did not contain valid UTF-8")};
    return buffer;
}

} // namespace riley::dock::unix_io
