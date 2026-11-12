// SPDX-License-Identifier: GPL-3.0-or-later
//
// libriley-dock: control. Mirrors riley dock/src/control.rs.
//
// Same-user control socket. It accepts one bounded request, `show`, and never starts a dock.
#pragma once

#include <riley-dock/ipc.h>

#include <QtCore/QByteArrayView>
#include <QtCore/QLatin1StringView>
#include <QtCore/QString>

#include <chrono>
#include <memory>
#include <optional>

namespace riley::dock::control {

// riley dock/src/control.rs:11 MAX_REQUEST, riley dock/src/control.rs:12 SHOWN.
inline constexpr qsizetype MAX_REQUEST = 16;
inline constexpr QLatin1StringView SHOWN{"shown"};

// riley dock/src/control.rs:14 default_path(). ipc::instancePath("riley-dock.sock").
IoResult<QString> defaultPath();

// riley dock/src/control.rs:34 parse_request(bytes). Exactly "show" or "show\n".
bool parseRequest(QByteArrayView bytes);

// riley dock/src/control.rs:45 struct Endpoint { path, identity }. Unlinks only the endpoint this server created,
// never a newer or foreign one. Move-only.
class Endpoint {
public:
    Endpoint() = default;
    Endpoint(QString path, quint64 device, quint64 inode)
        : m_path(std::move(path)), m_device(device), m_inode(inode) {}
    Endpoint(Endpoint &&other) noexcept;            // leaves `other` empty (its destructor unlinks nothing)
    Endpoint &operator=(Endpoint &&other) noexcept;
    Endpoint(const Endpoint &) = delete;
    Endpoint &operator=(const Endpoint &) = delete;

    // riley dock/src/control.rs:54 Drop: lstat(path); if its (st_dev, st_ino) equals the identity recorded at
    // bind, unlink(path); otherwise leave it.
    ~Endpoint();

    // riley dock/src/control.rs:51 Endpoint::path().
    const QString &path() const { return m_path; }

private:
    QString m_path;
    quint64 m_device = 0; // identity.0, st_dev
    quint64 m_inode = 0;  // identity.1, st_ino
};

// Rust Receiver<UnixStream> (riley dock/src/control.rs:87 mpsc::channel()): accepted, validated `show` requests.
// Each stream is the requesting client, waiting for the reply ("shown\n" once the dock has shown); dropping the
// stream ends the reply.
class Requests {
public:
    struct Queue; // shared with the worker thread; defined with the implementation
    explicit Requests(std::shared_ptr<Queue> queue) : m_queue(std::move(queue)) {}
    // Rust Receiver::recv_timeout: the next accepted request, nullopt after `timeout` or when the worker ended.
    std::optional<ipc::UnixStream> receive(std::chrono::milliseconds timeout);
    // Rust Receiver::try_recv: the next accepted request, nullopt when none is waiting.
    std::optional<ipc::UnixStream> tryReceive();

private:
    std::shared_ptr<Queue> m_queue;
};

// riley dock/src/control.rs:86 the tuple serve() returns: (Receiver<UnixStream>, UnixStream, Endpoint).
struct Serving {
    Requests requests;      // accepted requests
    ipc::UnixStream signal; // nonblocking read end of a socket pair: one byte "." per accepted request
    Endpoint endpoint;      // unlinks the socket when dropped
};

// riley dock/src/control.rs:38 struct Server { listener, endpoint, lock }. Move-only.
class Server {
public:
    // riley dock/src/control.rs:64 Server::bind(path). Hold the lifetime lock, replace only a stale socket of this
    // user, and bind with mode 0600:
    //   1. (riley dock/src/control.rs:24 check_parent) the parent directory ("." when none) must be owned by this
    //      uid with no group/other write bit (mode & 022 == 0), else error Other;
    //   2. open "<path>.lock" read-write, create, mode 0600, O_NOFOLLOW|O_CLOEXEC; owned by another uid -> Other;
    //   3. flock(LOCK_EX|LOCK_NB) fails -> error AddrInUse ("another dock owns this control socket");
    //   4. lstat(path): a socket owned by this uid is unlinked; anything else existing -> error AlreadyExists
    //      ("<path> exists and is not this user's socket; left alone"), the file untouched; ENOENT is fine; other
    //      errors are returned;
    //   5. bind a listening Unix socket at path, chmod 0600, record (st_dev, st_ino) of the new socket.
    static IoResult<Server> bind(const QString &path);

    // riley dock/src/control.rs:86 Server::serve(self). A worker thread owns the listener and the lock (held for
    // as long as the endpoint can accept) and, for each connection: skips it unless the peer uid (SO_PEERCRED,
    // riley dock/src/control.rs:109 peer_uid) is this uid; sets 500 ms read/write timeouts; reads to EOF at most
    // MAX_REQUEST + 1 bytes; if !parseRequest writes "error\n" and drops it; else hands the stream to Requests and
    // writes "." to the signal pair. It stops when handing over or signalling fails.
    IoResult<Serving> serve() &&;

    Server(Server &&other) noexcept;
    Server &operator=(Server &&other) noexcept;
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    ~Server(); // closes listener and lock when not served; the endpoint unlinks per Endpoint's rule

private:
    Server(int listener, Endpoint endpoint, int lock);
    int m_listener = -1; // listener: UnixListener
    Endpoint m_endpoint; // endpoint
    int m_lock = -1;     // lock: File, flock'ed
};

// riley dock/src/control.rs:118 request_show(path, timeout). The `--show` client: ask the running dock and wait for
// its completed shown frame. lstat(path) fails -> that error's kind, message "no running dock at <path>: <error>";
// not a socket or not this uid's -> Other ("<path> is not this user's dock socket"); connect; read/write timeouts
// `timeout`; write "show\n"; shutdown(SHUT_WR); read to EOF at most 64 bytes (UTF-8); return it trimmed.
IoResult<QString> requestShow(const QString &path, std::chrono::milliseconds timeout);

} // namespace riley::dock::control
