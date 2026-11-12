// SPDX-License-Identifier: GPL-3.0-or-later
//
// Port of riley dock/src/control.rs.
#include <riley-dock/control.h>

#include "unix_io.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace riley::dock::control {

namespace {

// riley dock/src/control.rs:16
uid_t uid()
{
    return ::getuid();
}

// riley dock/src/control.rs:18 lock_path: the path with ".lock" appended.
QString lockPath(const QString &path)
{
    return path + u".lock"_s;
}

// Rust Components::trim_right as Path::parent and Components::as_path apply it: drops trailing separators and "."
// components after the first `bodyStart` characters.
qsizetype trimRight(const QString &path, qsizetype length, qsizetype bodyStart)
{
    while (length > bodyStart) {
        const qsizetype slash = path.lastIndexOf(u'/', length - 1);
        const qsizetype start = slash >= bodyStart ? slash + 1 : bodyStart;
        const QStringView component = QStringView(path).mid(start, length - start);
        if (!component.isEmpty() && component != u".")
            break;
        length = slash >= bodyStart ? slash : bodyStart;
    }
    return length;
}

// riley dock/src/control.rs:25 path.parent().filter(|p| !p.as_os_str().is_empty()).unwrap_or(Path::new(".")),
// with Rust's Path::parent on Unix: the path without its last normal (or "..") component, trailing separators and
// "." components trimmed; no parent (the root, an empty path, a lone ".") or an empty parent is ".".
QString parentOf(const QString &path)
{
    const bool root = path.startsWith(u'/');
    const bool currentDir = !root && (path == u"."_s || path.startsWith(u"./"_s));
    const qsizetype bodyStart = (root || currentDir) ? 1 : 0;
    qsizetype length = trimRight(path, path.size(), bodyStart);
    if (length <= bodyStart)
        return u"."_s;
    const qsizetype slash = path.lastIndexOf(u'/', length - 1);
    length = slash >= bodyStart ? slash + 1 : bodyStart;
    length = trimRight(path, length, bodyStart);
    const QString parent = path.left(length);
    return parent.isEmpty() ? u"."_s : parent;
}

// riley dock/src/control.rs:24 check_parent
IoResult<> checkParent(const QString &path)
{
    const QString parent = parentOf(path);
    const auto meta = unix_io::statPath(parent, true);
    if (!meta.ok())
        return meta.error();
    if (meta.value().st_uid != uid() || (meta.value().st_mode & 0022) != 0)
        return unix_io::other(u"%1 is not a private directory of this user"_s.arg(parent));
    return std::monostate{};
}

// riley dock/src/control.rs:109 peer_uid: SO_PEERCRED, nullopt when the query fails.
std::optional<uid_t> peerUid(int fd)
{
    ucred cred {};
    cred.uid = uid_t(-1);
    socklen_t size = sizeof cred;
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &size) != 0)
        return std::nullopt;
    return cred.uid;
}

void closeFd(int &fd)
{
    if (fd >= 0)
        ::close(std::exchange(fd, -1));
}

} // namespace

// riley dock/src/control.rs:14
IoResult<QString> defaultPath()
{
    return ipc::instancePath(u"riley-dock.sock"_s);
}

// riley dock/src/control.rs:34
bool parseRequest(QByteArrayView bytes)
{
    return bytes == QByteArrayView("show") || bytes == QByteArrayView("show\n");
}

Endpoint::Endpoint(Endpoint &&other) noexcept
    : m_path(std::exchange(other.m_path, QString())),
      m_device(std::exchange(other.m_device, 0)),
      m_inode(std::exchange(other.m_inode, 0))
{
}

Endpoint &Endpoint::operator=(Endpoint &&other) noexcept
{
    if (this != &other) {
        Endpoint dropped(std::move(*this));
        m_path = std::exchange(other.m_path, QString());
        m_device = std::exchange(other.m_device, 0);
        m_inode = std::exchange(other.m_inode, 0);
    }
    return *this;
}

// riley dock/src/control.rs:54
Endpoint::~Endpoint()
{
    if (m_path.isEmpty())
        return; // moved from: Rust never drops a moved value
    const auto meta = unix_io::statPath(m_path, false);
    if (meta.ok() && quint64(meta.value().st_dev) == m_device && quint64(meta.value().st_ino) == m_inode)
        (void)unix_io::removeFile(m_path);
}

namespace detail {

// Rust mpsc::channel::<UnixStream>() (riley dock/src/control.rs:87): unbounded; send fails once the receiver is
// gone, whose drop discards what waits; receiving fails once the sender is gone and nothing waits.
struct RequestChannel {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<ipc::UnixStream> items;
    bool receiverAlive = true;
    bool senderAlive = true;

    bool send(ipc::UnixStream stream)
    {
        std::lock_guard lock(mutex);
        if (!receiverAlive)
            return false; // the stream is dropped with the error, as in Rust
        items.push_back(std::move(stream));
        changed.notify_all();
        return true;
    }
    void dropSender()
    {
        std::lock_guard lock(mutex);
        senderAlive = false;
        changed.notify_all();
    }
};

} // namespace detail

// The receiving end. Its destruction is the Rust Receiver's drop: the worker's next send fails.
struct Requests::Queue {
    explicit Queue(std::shared_ptr<detail::RequestChannel> channel) : channel(std::move(channel)) {}
    Queue(const Queue &) = delete;
    Queue &operator=(const Queue &) = delete;
    ~Queue()
    {
        std::deque<ipc::UnixStream> dropped;
        {
            std::lock_guard lock(channel->mutex);
            channel->receiverAlive = false;
            dropped.swap(channel->items);
            channel->changed.notify_all();
        }
    }
    std::shared_ptr<detail::RequestChannel> channel;
};

std::optional<ipc::UnixStream> Requests::receive(std::chrono::milliseconds timeout)
{
    detail::RequestChannel &channel = *m_queue->channel;
    std::unique_lock lock(channel.mutex);
    channel.changed.wait_for(lock, timeout, [&] { return !channel.items.empty() || !channel.senderAlive; });
    if (channel.items.empty())
        return std::nullopt;
    ipc::UnixStream stream = std::move(channel.items.front());
    channel.items.pop_front();
    return stream;
}

std::optional<ipc::UnixStream> Requests::tryReceive()
{
    detail::RequestChannel &channel = *m_queue->channel;
    std::lock_guard lock(channel.mutex);
    if (channel.items.empty())
        return std::nullopt;
    ipc::UnixStream stream = std::move(channel.items.front());
    channel.items.pop_front();
    return stream;
}

Server::Server(int listener, Endpoint endpoint, int lock)
    : m_listener(listener), m_endpoint(std::move(endpoint)), m_lock(lock)
{
}

Server::Server(Server &&other) noexcept
    : m_listener(std::exchange(other.m_listener, -1)),
      m_endpoint(std::move(other.m_endpoint)),
      m_lock(std::exchange(other.m_lock, -1))
{
}

Server &Server::operator=(Server &&other) noexcept
{
    if (this != &other) {
        Server dropped(std::move(*this));
        m_listener = std::exchange(other.m_listener, -1);
        m_endpoint = std::move(other.m_endpoint);
        m_lock = std::exchange(other.m_lock, -1);
    }
    return *this;
}

// Rust drops the fields in declaration order: listener, endpoint, lock.
Server::~Server()
{
    closeFd(m_listener);
    {
        Endpoint dropped(std::move(m_endpoint));
    }
    closeFd(m_lock);
}

// riley dock/src/control.rs:64
IoResult<Server> Server::bind(const QString &path)
{
    if (const auto checked = checkParent(path); !checked.ok())
        return checked.error();
    // riley dock/src/control.rs:66 OpenOptions read, write, create, mode 0600, O_NOFOLLOW | O_CLOEXEC.
    const auto lockName = unix_io::encodePath(lockPath(path));
    if (!lockName.ok())
        return lockName.error();
    int lock = -1;
    do {
        lock = ::open(lockName.value().constData(), O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    } while (lock < 0 && errno == EINTR);
    if (lock < 0)
        return IoError::fromErrno(errno);
    // riley dock/src/control.rs:68
    struct stat lockMeta {};
    if (::fstat(lock, &lockMeta) != 0) {
        const int error = errno;
        closeFd(lock);
        return IoError::fromErrno(error);
    }
    if (lockMeta.st_uid != uid()) {
        closeFd(lock);
        return unix_io::other(u"control lock belongs to another user"_s);
    }
    // riley dock/src/control.rs:69
    if (::flock(lock, LOCK_EX | LOCK_NB) != 0) {
        closeFd(lock);
        return IoError{IoErrorKind::AddrInUse, u"another dock owns this control socket"_s};
    }
    // riley dock/src/control.rs:72
    const auto existing = unix_io::statPath(path, false);
    if (existing.ok()) {
        if (S_ISSOCK(existing.value().st_mode) && existing.value().st_uid == uid()) {
            if (const auto removed = unix_io::removeFile(path); !removed.ok()) {
                closeFd(lock);
                return removed.error();
            }
        } else {
            closeFd(lock);
            return IoError{IoErrorKind::AlreadyExists, u"%1 exists and is not this user's socket; left alone"_s.arg(path)};
        }
    } else if (existing.error().kind != IoErrorKind::NotFound) {
        closeFd(lock);
        return existing.error();
    }
    // riley dock/src/control.rs:79 UnixListener::bind: socket, bind, listen with Rust std's Linux backlog (-1, the
    // system maximum).
    const auto address = unix_io::socketAddress(path);
    if (!address.ok()) {
        closeFd(lock);
        return address.error();
    }
    int listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        const int error = errno;
        closeFd(lock);
        return IoError::fromErrno(error);
    }
    if (::bind(listener, reinterpret_cast<const sockaddr *>(&address.value().first), address.value().second) != 0
        || ::listen(listener, -1) != 0) {
        const int error = errno;
        closeFd(listener);
        closeFd(lock);
        return IoError::fromErrno(error);
    }
    // riley dock/src/control.rs:80 set_permissions 0600 (chmod).
    const QByteArray name = QFile::encodeName(path);
    int changed = -1;
    do {
        changed = ::chmod(name.constData(), 0600);
    } while (changed != 0 && errno == EINTR);
    if (changed != 0) {
        const int error = errno;
        closeFd(listener);
        closeFd(lock);
        return IoError::fromErrno(error);
    }
    // riley dock/src/control.rs:81
    const auto meta = unix_io::statPath(path, false);
    if (!meta.ok()) {
        closeFd(listener);
        closeFd(lock);
        return meta.error();
    }
    return Server(listener, Endpoint(path, quint64(meta.value().st_dev), quint64(meta.value().st_ino)), lock);
}

namespace {

// riley dock/src/control.rs:91 the worker thread. It owns the listener and the lifetime lock.
void serveRequests(int listener, int lock, std::shared_ptr<detail::RequestChannel> send, ipc::UnixStream notify)
{
    using namespace std::chrono_literals;
    // riley dock/src/control.rs:93 the lifetime lock is held for as long as this endpoint can accept.
    struct Owned {
        int listener;
        int lock;
        detail::RequestChannel &send;
        ~Owned()
        {
            send.dropSender();
            closeFd(lock);
            closeFd(listener);
        }
    } owned{listener, lock, *send};
    // riley dock/src/control.rs:94 listener.incoming(): accept4 with SOCK_CLOEXEC, EINTR retried; errors skipped.
    for (;;) {
        const int fd = ::accept4(owned.listener, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        ipc::UnixStream stream(fd);
        // riley dock/src/control.rs:96
        if (peerUid(fd) != std::optional<uid_t>(uid()))
            continue;
        // riley dock/src/control.rs:97
        (void)unix_io::setTimeout(fd, SO_RCVTIMEO, 500ms);
        (void)unix_io::setTimeout(fd, SO_SNDTIMEO, 500ms);
        // riley dock/src/control.rs:100
        QByteArray buffer;
        (void)unix_io::readToEnd(fd, MAX_REQUEST + 1, buffer);
        // riley dock/src/control.rs:101
        if (!parseRequest(buffer)) {
            (void)stream.writeAll("error\n");
            continue;
        }
        // riley dock/src/control.rs:102
        if (!send->send(std::move(stream)) || !notify.writeAll(".").ok())
            break;
    }
}

} // namespace

// riley dock/src/control.rs:86
IoResult<Serving> Server::serve() &&
{
    // `self` by value: on an early error it is dropped here, as in Rust.
    Server self(std::move(*this));
    auto channel = std::make_shared<detail::RequestChannel>();
    auto pair = unix_io::socketPair();
    if (!pair.ok())
        return pair.error();
    ipc::UnixStream notify = std::move(pair.value().first);
    ipc::UnixStream signal = std::move(pair.value().second);
    if (const auto set = unix_io::setNonblocking(signal.fd()); !set.ok())
        return set.error();
    const int listener = std::exchange(self.m_listener, -1);
    const int lock = std::exchange(self.m_lock, -1);
    Endpoint endpoint(std::move(self.m_endpoint));
    Requests requests(std::make_shared<Requests::Queue>(channel));
    std::thread(serveRequests, listener, lock, std::move(channel), std::move(notify)).detach();
    return Serving{std::move(requests), std::move(signal), std::move(endpoint)};
}

// riley dock/src/control.rs:118
IoResult<QString> requestShow(const QString &path, std::chrono::milliseconds timeout)
{
    const auto meta = unix_io::statPath(path, false);
    if (!meta.ok())
        return IoError{meta.error().kind, u"no running dock at %1: %2"_s.arg(path, meta.error().message)};
    if (!S_ISSOCK(meta.value().st_mode) || meta.value().st_uid != uid())
        return unix_io::other(u"%1 is not this user's dock socket"_s.arg(path));
    auto connected = unix_io::connectUnix(path);
    if (!connected.ok())
        return connected.error();
    ipc::UnixStream stream = std::move(connected).value();
    if (const auto set = unix_io::setTimeout(stream.fd(), SO_RCVTIMEO, timeout); !set.ok())
        return set.error();
    if (const auto set = unix_io::setTimeout(stream.fd(), SO_SNDTIMEO, timeout); !set.ok())
        return set.error();
    if (const auto written = stream.writeAll("show\n"); !written.ok())
        return written.error();
    if (::shutdown(stream.fd(), SHUT_WR) != 0)
        return IoError::fromErrno(errno);
    const auto reply = unix_io::readToString(stream.fd(), 64);
    if (!reply.ok())
        return reply.error();
    // riley dock/src/control.rs:131 str::trim: Unicode White_Space at both ends, the set QString::trimmed removes.
    return QString::fromUtf8(reply.value()).trimmed();
}

} // namespace riley::dock::control
