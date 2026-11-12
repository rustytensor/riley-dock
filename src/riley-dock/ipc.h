// SPDX-License-Identifier: GPL-3.0-or-later
//
// libriley-dock: ipc. Mirrors riley dock/src/ipc.rs.
//
// The Hyprland IPC client: the instance socket paths, hyprctl-style JSON requests, the event-socket records
// (refresh and urgency), and the window commands the dock sends. Also holds the small I/O result types the ipc
// and control modules share (the Rust code used std::io::Result and std::os::unix::net::UnixStream).
#pragma once

#include <riley-dock/rules.h>

#include <QtCore/QByteArray>
#include <QtCore/QByteArrayView>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QStringList>

#include <chrono>
#include <memory>
#include <optional>
#include <utility>
#include <variant>

namespace riley::dock {

// Rust std::io::ErrorKind, the kinds this library reports. Errors made with io::Error::other(..) are Other.
enum class IoErrorKind { Other, NotFound, PermissionDenied, AddrInUse, AlreadyExists, TimedOut, WouldBlock,
                         InvalidData, BrokenPipe, ConnectionRefused };

// Rust std::io::Error: a kind and a readable message.
struct IoError {
    IoErrorKind kind = IoErrorKind::Other;
    QString message;

    // The kind Rust's std gives for errno `errnum` (ENOENT NotFound, EACCES/EPERM PermissionDenied, EADDRINUSE
    // AddrInUse, EEXIST AlreadyExists, ETIMEDOUT TimedOut, EAGAIN WouldBlock, EPIPE BrokenPipe, ECONNREFUSED
    // ConnectionRefused, anything else Other), with strerror text after `context`.
    static IoError fromErrno(int errnum, const QString &context = {});
};

// Rust std::io::Result<T>: a value or an IoError. IoResult<> is io::Result<()>.
template <typename T = std::monostate>
class [[nodiscard]] IoResult {
public:
    IoResult(T value) : m_value(std::move(value)) {}
    IoResult(IoError error) : m_error(std::move(error)) {}
    bool ok() const { return m_value.has_value(); }
    T &value() & { return *m_value; }
    const T &value() const & { return *m_value; }
    T &&value() && { return std::move(*m_value); }
    const IoError &error() const { return m_error; }

private:
    std::optional<T> m_value;
    IoError m_error;
};

} // namespace riley::dock

namespace riley::dock::ipc {

// Rust std::os::unix::net::UnixStream as the dock uses it: an owned, connected Unix stream socket. Move-only; the
// destructor closes it (Rust drop).
class UnixStream {
public:
    UnixStream() = default;
    explicit UnixStream(int fd) : m_fd(fd) {}
    UnixStream(UnixStream &&other) noexcept;
    UnixStream &operator=(UnixStream &&other) noexcept;
    UnixStream(const UnixStream &) = delete;
    UnixStream &operator=(const UnixStream &) = delete;
    ~UnixStream();

    int fd() const { return m_fd; }
    bool isOpen() const { return m_fd >= 0; }

    // Rust Write::write_all: writes every byte (retrying short writes and EINTR) or fails.
    IoResult<> writeAll(QByteArrayView bytes);

private:
    int m_fd = -1;
};

// riley dock/src/ipc.rs:10 struct Snapshot { clients, active }.
struct Snapshot {
    QList<rules::Client> clients;
    QString active; // address of the active window, "" when none
};

// riley dock/src/ipc.rs:15 instance_path(name). $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/<name>.
// Errors (kind Other): XDG_RUNTIME_DIR unset ("XDG_RUNTIME_DIR is not set"), HYPRLAND_INSTANCE_SIGNATURE unset or
// not UTF-8 ("HYPRLAND_INSTANCE_SIGNATURE is not set"), a signature containing '/' or equal to ".." ("invalid
// compositor instance").
IoResult<QString> instancePath(const QString &name);

// riley dock/src/ipc.rs:26 request(command). Connects to instancePath(".socket.sock"), 2 s read and write
// timeouts, writes `command` (e.g. "j/monitors"), reads to EOF at most 8 MiB, which must be UTF-8.
IoResult<QByteArray> request(const QString &command);

// riley dock/src/ipc.rs:36 monitors(). rules::monitorsFromJson(request("j/monitors")); a parse failure is an
// error of kind Other.
IoResult<QList<rules::Monitor>> monitors();

// riley dock/src/ipc.rs:40 snapshot(). clients = rules::clientsFromJson(request("j/clients")); then
// request("j/activewindow") must be JSON; active = its string "address" member, "" when missing or not a string.
IoResult<Snapshot> snapshot();

// riley dock/src/ipc.rs:50 (private fn refresh_event): an event record "<name>>><data>" asks for a refresh when
// <name> (the text before the first ">>") is one of openwindow, closewindow, activewindowv2, movewindowv2,
// workspace, workspacev2, focusedmon, activespecial, activespecialv2, windowtitle, windowtitlev2.

// riley dock/src/ipc.rs:59 struct Pending { refresh, urgent } (Default: both false). Event records waiting for the
// worker. Urgency is kept beside a queued refresh, never folded into it.
struct Pending {
    bool refresh = false;
    bool urgent = false;
    bool operator==(const Pending &) const = default;

    // riley dock/src/ipc.rs:66 Pending::absorb(line). "urgent>>ADDRESS": valid when ADDRESS is non-empty and all
    // ASCII hex digits; urgent |= valid; returns valid (a refresh is never considered for it). Any other line:
    // refresh |= refresh_event(line); returns that.
    bool absorb(const QString &line);

    // riley dock/src/ipc.rs:78 Pending::reveal_requests(). One reveal request per drained batch: urgent ? 1 : 0.
    qsizetype revealRequests() const;
};

// riley dock/src/ipc.rs:81 enum Update { Snapshot(Result<Snapshot, String>), Attention }.
namespace update {
struct Refresh {                      // Rust Update::Snapshot(result)
    std::optional<Snapshot> snapshot; // Ok(snapshot)
    QString error;                    // Err(message) when snapshot is nullopt
};
struct Attention {}; // Rust Update::Attention
} // namespace update
using Update = std::variant<update::Refresh, update::Attention>;

// Rust Receiver<Update> (riley dock/src/ipc.rs:88 mpsc::sync_channel(4)): what the worker thread delivered.
class Updates {
public:
    struct Queue; // shared with the worker thread; defined with the implementation
    explicit Updates(std::shared_ptr<Queue> queue) : m_queue(std::move(queue)) {}
    // Rust Receiver::try_recv: the next delivered update, nullopt when none is waiting.
    std::optional<Update> tryReceive();

private:
    std::shared_ptr<Queue> m_queue;
};

// riley dock/src/ipc.rs:87 updates() -> (Receiver<Update>, UnixStream). Starts the worker thread and returns its
// receiver plus the nonblocking read end of a socket pair that gets one byte "." per delivered update (for a
// QSocketNotifier). The worker: connect instancePath(".socket2.sock") (on failure deliver
// Refresh{error "event socket: <error>"}, wait 2 s, retry); queued = Pending{refresh true}; a reader thread
// absorbs each line and wakes the worker when absorb returns true; per batch (Pending taken and reset): urgent ->
// deliver Attention; refresh -> wait until 50 ms after the last refresh, clear queued.refresh, drain wake-ups,
// deliver Refresh{snapshot() or error "refresh: <error>"}; repeat while more is queued, else wait for a wake-up;
// when the reader ends deliver Refresh{error "event socket disconnected; reconnecting"}, wait 1 s, reconnect.
// The worker stops when delivering fails (receiver gone).
IoResult<std::pair<Updates, UnixStream>> updates();

// riley dock/src/ipc.rs:142 enum WindowAction { Focus, Close, Float, Fullscreen, Move(u8) }.
namespace window_action {
struct Focus { bool operator==(const Focus &) const = default; };
struct Close { bool operator==(const Close &) const = default; };
struct Float { bool operator==(const Float &) const = default; };
struct Fullscreen { bool operator==(const Fullscreen &) const = default; };
struct Move { quint8 workspace = 0; bool operator==(const Move &) const = default; };
} // namespace window_action
using WindowAction = std::variant<window_action::Focus, window_action::Close, window_action::Float,
                                  window_action::Fullscreen, window_action::Move>;

// riley dock/src/ipc.rs:145 commands(address, action). Addresses are compositor identifiers, never executable Lua
// supplied by a title: `address` must be "0x" followed by one or more ASCII hex digits, else error (Other,
// "invalid window address"). With A = address, the literal commands are:
//   Focus      -> ["dispatch hl.dsp.focus({ window = \"address:A\" })"]
//   Close      -> ["dispatch hl.dsp.window.close({ window = \"address:A\" })"]
//   Float      -> ["dispatch hl.dsp.window.float({ action = \"toggle\", window = \"address:A\" })"]
//   Fullscreen -> [the Focus command, "dispatch hl.dsp.window.fullscreen({ mode = \"fullscreen\" })"]
//   Move(n), n in 1..9 -> ["dispatch hl.dsp.window.move({ workspace = \"n\", follow = false, window = \"address:A\" })"]
//   Move(other) -> error (Other, "workspace must be 1–9").
IoResult<QStringList> commands(const QString &address, const WindowAction &action);

} // namespace riley::dock::ipc
