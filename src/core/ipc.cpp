// SPDX-License-Identifier: GPL-3.0-or-later
//
// Port of riley dock/src/ipc.rs.
#include <riley-dock/ipc.h>

#include "unix_io.h"

#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace riley::dock {

namespace {

// strerror_r exists in an XSI form (int) and a GNU form (char *); either gives the text Rust's
// std::sys::os::error_string reads.
[[maybe_unused]] const char *strerrorText(int result, const char *buffer)
{
    return result == 0 ? buffer : "Unknown error";
}
[[maybe_unused]] const char *strerrorText(const char *result, const char *)
{
    return result;
}

} // namespace

// Rust std::sys::decode_error_kind for the kinds this library names, and Rust's Display of an OS error:
// "<strerror> (os error <n>)".
IoError IoError::fromErrno(int errnum, const QString &context)
{
    IoErrorKind kind = IoErrorKind::Other;
    switch (errnum) {
    case ENOENT: kind = IoErrorKind::NotFound; break;
    case EACCES:
    case EPERM: kind = IoErrorKind::PermissionDenied; break;
    case EADDRINUSE: kind = IoErrorKind::AddrInUse; break;
    case EEXIST: kind = IoErrorKind::AlreadyExists; break;
    case ETIMEDOUT: kind = IoErrorKind::TimedOut; break;
    case EAGAIN: kind = IoErrorKind::WouldBlock; break;
    case EPIPE: kind = IoErrorKind::BrokenPipe; break;
    case ECONNREFUSED: kind = IoErrorKind::ConnectionRefused; break;
    default: break;
    }
    char buffer[256] = {};
    const char *text = strerrorText(strerror_r(errnum, buffer, sizeof buffer), buffer);
    QString message = u"%1 (os error %2)"_s.arg(QString::fromLocal8Bit(text)).arg(errnum);
    if (!context.isEmpty())
        message = context + u": "_s + message;
    return IoError{kind, message};
}

namespace ipc {

UnixStream::UnixStream(UnixStream &&other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}

UnixStream &UnixStream::operator=(UnixStream &&other) noexcept
{
    if (this != &other) {
        if (m_fd >= 0)
            ::close(m_fd);
        m_fd = std::exchange(other.m_fd, -1);
    }
    return *this;
}

UnixStream::~UnixStream()
{
    if (m_fd >= 0)
        ::close(m_fd);
}

// Rust Write::write_all on a UnixStream: short writes and EINTR are retried; a write of 0 bytes is WriteZero
// ("failed to write whole buffer", Other here). Rust's runtime ignores SIGPIPE, so writing to a closed peer is an
// EPIPE error; MSG_NOSIGNAL gives the same result without a process-wide signal setting.
IoResult<> UnixStream::writeAll(QByteArrayView bytes)
{
    const char *data = bytes.data();
    qsizetype left = bytes.size();
    while (left > 0) {
        const ssize_t written = ::send(m_fd, data, std::size_t(left), MSG_NOSIGNAL);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            return IoError::fromErrno(errno);
        }
        if (written == 0)
            return unix_io::other(u"failed to write whole buffer"_s);
        data += written;
        left -= qsizetype(written);
    }
    return std::monostate{};
}

namespace {

// Rust PathBuf::push on Unix: an absolute component replaces the path; otherwise a separator is added unless the
// path is empty or already ends with one.
QString pushPath(const QString &base, const QString &component)
{
    if (component.startsWith(u'/'))
        return component;
    if (!base.isEmpty() && !base.endsWith(u'/'))
        return base + u'/' + component;
    return base + component;
}

} // namespace

// riley dock/src/ipc.rs:15
IoResult<QString> instancePath(const QString &name)
{
    // riley dock/src/ipc.rs:16 std::env::var_os: any value, set is enough.
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime == nullptr)
        return unix_io::other(u"XDG_RUNTIME_DIR is not set"_s);
    // riley dock/src/ipc.rs:18 std::env::var: unset or not UTF-8 are both errors.
    const char *signatureBytes = std::getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (signatureBytes == nullptr || !unix_io::validUtf8(QByteArrayView(signatureBytes)))
        return unix_io::other(u"HYPRLAND_INSTANCE_SIGNATURE is not set"_s);
    const QString signature = QString::fromUtf8(signatureBytes);
    if (signature.contains(u'/') || signature == u".."_s)
        return unix_io::other(u"invalid compositor instance"_s);
    // riley dock/src/ipc.rs:23
    return pushPath(pushPath(pushPath(QFile::decodeName(runtime), u"hypr"_s), signature), name);
}

// riley dock/src/ipc.rs:26
IoResult<QByteArray> request(const QString &command)
{
    const auto path = instancePath(u".socket.sock"_s);
    if (!path.ok())
        return path.error();
    auto connected = unix_io::connectUnix(path.value());
    if (!connected.ok())
        return connected.error();
    UnixStream stream = std::move(connected).value();
    if (const auto set = unix_io::setTimeout(stream.fd(), SO_RCVTIMEO, std::chrono::seconds(2)); !set.ok())
        return set.error();
    if (const auto set = unix_io::setTimeout(stream.fd(), SO_SNDTIMEO, std::chrono::seconds(2)); !set.ok())
        return set.error();
    if (const auto written = stream.writeAll(command.toUtf8()); !written.ok())
        return written.error();
    // riley dock/src/ipc.rs:32 take(8 * 1024 * 1024).read_to_string
    return unix_io::readToString(stream.fd(), 8 * 1024 * 1024);
}

// riley dock/src/ipc.rs:36
IoResult<QList<rules::Monitor>> monitors()
{
    const auto reply = request(u"j/monitors"_s);
    if (!reply.ok())
        return reply.error();
    auto parsed = rules::monitorsFromJson(reply.value());
    // serde_json's error text has no counterpart; the kind (Other) is Rust's io::Error::other.
    if (!parsed)
        return unix_io::other(u"j/monitors: invalid JSON"_s);
    return std::move(*parsed);
}

namespace {

// serde_json::from_str::<serde_json::Value>: any one JSON value. QJsonDocument only takes an array or an object at
// the top, so the text is parsed as the single element of an array.
std::optional<QJsonValue> jsonValue(const QByteArray &json)
{
    QJsonParseError error {};
    const QJsonDocument document = QJsonDocument::fromJson("[" + json + "]", &error);
    if (error.error != QJsonParseError::NoError || !document.isArray() || document.array().size() != 1)
        return std::nullopt;
    return document.array().at(0);
}

} // namespace

// riley dock/src/ipc.rs:40
IoResult<Snapshot> snapshot()
{
    const auto clientsReply = request(u"j/clients"_s);
    if (!clientsReply.ok())
        return clientsReply.error();
    auto clients = rules::clientsFromJson(clientsReply.value());
    if (!clients)
        return unix_io::other(u"j/clients: invalid JSON"_s);
    const auto activeReply = request(u"j/activewindow"_s);
    if (!activeReply.ok())
        return activeReply.error();
    const auto active = jsonValue(activeReply.value());
    if (!active)
        return unix_io::other(u"j/activewindow: invalid JSON"_s);
    // riley dock/src/ipc.rs:46 active["address"].as_str().unwrap_or_default()
    QString address;
    if (active->isObject()) {
        const QJsonValue member = active->toObject().value(u"address"_s);
        if (member.isString())
            address = member.toString();
    }
    return Snapshot{std::move(*clients), address};
}

namespace {

// riley dock/src/ipc.rs:50 refresh_event
bool refreshEvent(const QString &line)
{
    const qsizetype split = line.indexOf(u">>"_s);
    if (split < 0)
        return false;
    const QStringView name = QStringView(line).left(split);
    static const char16_t *const names[] = {u"openwindow", u"closewindow", u"activewindowv2", u"movewindowv2",
                                            u"workspace", u"workspacev2", u"focusedmon", u"activespecial",
                                            u"activespecialv2", u"windowtitle", u"windowtitlev2"};
    for (const char16_t *candidate : names) {
        if (name == QStringView(candidate))
            return true;
    }
    return false;
}

// u8::is_ascii_hexdigit
bool asciiHexDigit(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'0' && u <= u'9') || (u >= u'a' && u <= u'f') || (u >= u'A' && u <= u'F');
}

bool allAsciiHex(QStringView text)
{
    for (const QChar c : text) {
        if (!asciiHexDigit(c))
            return false;
    }
    return true;
}

} // namespace

// riley dock/src/ipc.rs:66
bool Pending::absorb(const QString &line)
{
    static const QString prefix = u"urgent>>"_s;
    if (line.startsWith(prefix)) {
        const QStringView address = QStringView(line).mid(prefix.size());
        const bool valid = !address.isEmpty() && allAsciiHex(address);
        urgent |= valid;
        return valid;
    }
    const bool refreshed = refreshEvent(line);
    refresh |= refreshed;
    return refreshed;
}

// riley dock/src/ipc.rs:78
qsizetype Pending::revealRequests() const
{
    return urgent ? 1 : 0;
}

namespace detail {

// Rust mpsc::sync_channel::<Update>(4) (riley dock/src/ipc.rs:88): send blocks while 4 updates wait and fails once
// the receiver is gone; dropping the receiver discards what waits.
struct UpdateChannel {
    static constexpr std::size_t capacity = 4;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Update> items;
    bool receiverAlive = true;

    bool send(Update value)
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return !receiverAlive || items.size() < capacity; });
        if (!receiverAlive)
            return false;
        items.push_back(std::move(value));
        changed.notify_all();
        return true;
    }
};

// Rust mpsc::sync_channel::<()>(1) (riley dock/src/ipc.rs:104): the reader's wake-ups for the worker.
struct WakeChannel {
    std::mutex mutex;
    std::condition_variable changed;
    bool full = false;
    bool senderAlive = true;
    bool receiverAlive = true;

    enum class TrySend { Sent, Full, Disconnected };
    TrySend trySend()
    {
        std::lock_guard lock(mutex);
        if (!receiverAlive)
            return TrySend::Disconnected;
        if (full)
            return TrySend::Full;
        full = true;
        changed.notify_all();
        return TrySend::Sent;
    }
    bool tryRecv()
    {
        std::lock_guard lock(mutex);
        return std::exchange(full, false);
    }
    // Rust Receiver::recv: a waiting wake-up, else blocks; an error once the sender is gone and nothing waits.
    bool recv()
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return full || !senderAlive; });
        return std::exchange(full, false);
    }
    void dropSender()
    {
        std::lock_guard lock(mutex);
        senderAlive = false;
        changed.notify_all();
    }
    void dropReceiver()
    {
        std::lock_guard lock(mutex);
        receiverAlive = false;
        full = false;
        changed.notify_all();
    }
};

// riley dock/src/ipc.rs:103 Arc<Mutex<Pending>>.
struct SharedPending {
    std::mutex mutex;
    Pending pending;
};

} // namespace detail

// The receiving end. Its destruction is the Rust Receiver's drop: the worker's next send fails.
struct Updates::Queue {
    explicit Queue(std::shared_ptr<detail::UpdateChannel> channel) : channel(std::move(channel)) {}
    Queue(const Queue &) = delete;
    Queue &operator=(const Queue &) = delete;
    ~Queue()
    {
        std::deque<Update> dropped;
        {
            std::lock_guard lock(channel->mutex);
            channel->receiverAlive = false;
            dropped.swap(channel->items);
            channel->changed.notify_all();
        }
    }
    std::shared_ptr<detail::UpdateChannel> channel;
};

std::optional<Update> Updates::tryReceive()
{
    detail::UpdateChannel &channel = *m_queue->channel;
    std::lock_guard lock(channel.mutex);
    if (channel.items.empty())
        return std::nullopt;
    Update value = std::move(channel.items.front());
    channel.items.pop_front();
    channel.changed.notify_all();
    return value;
}

namespace {

// riley dock/src/ipc.rs:106 the reader thread: BufReader::new(stream).lines(); a line that cannot be read or is not
// UTF-8 ends it, like the end of the stream.
void readEvents(UnixStream stream, std::shared_ptr<detail::SharedPending> shared,
                std::shared_ptr<detail::WakeChannel> events)
{
    QByteArray buffered;
    bool ended = false;
    char chunk[8192];
    for (;;) {
        // BufRead::read_line: up to and including '\n', or the rest at end of file.
        qsizetype newline = buffered.indexOf('\n');
        while (newline < 0 && !ended) {
            const ssize_t got = ::read(stream.fd(), chunk, sizeof chunk);
            if (got < 0) {
                if (errno == EINTR)
                    continue;
                events->dropSender();
                return; // Err(line): break
            }
            if (got == 0) {
                ended = true;
                break;
            }
            const qsizetype searchFrom = buffered.size();
            buffered.append(chunk, qsizetype(got));
            newline = buffered.indexOf('\n', searchFrom);
        }
        QByteArray raw;
        if (newline >= 0) {
            raw = buffered.left(newline + 1);
            buffered.remove(0, newline + 1);
        } else {
            if (buffered.isEmpty())
                break; // end of the stream
            raw = std::exchange(buffered, QByteArray());
        }
        if (!unix_io::validUtf8(raw))
            break; // Err(line): break
        // Lines::next strips "\n", then a "\r" before it.
        if (raw.endsWith('\n')) {
            raw.chop(1);
            if (raw.endsWith('\r'))
                raw.chop(1);
        }
        const QString line = QString::fromUtf8(raw);
        bool wanted = false;
        {
            std::lock_guard lock(shared->mutex);
            wanted = shared->pending.absorb(line);
        }
        if (wanted && events->trySend() == detail::WakeChannel::TrySend::Disconnected)
            break;
    }
    events->dropSender();
}

// The JoinHandle of the reader: joined on the normal path; dropped (detached) when the worker returns early.
struct ReaderHandle {
    std::thread thread;
    ~ReaderHandle()
    {
        if (thread.joinable())
            thread.detach();
    }
};

// `pending` of riley dock/src/ipc.rs:104: dropped when the worker leaves this connection.
struct WakeReceiver {
    std::shared_ptr<detail::WakeChannel> channel;
    ~WakeReceiver() { channel->dropReceiver(); }
};

// riley dock/src/ipc.rs:91 the worker thread.
void updatesWorker(std::shared_ptr<detail::UpdateChannel> send, UnixStream notify)
{
    using namespace std::chrono_literals;
    using Clock = std::chrono::steady_clock;
    // riley dock/src/ipc.rs:92
    auto deliver = [&](Update value) { return send->send(std::move(value)) && notify.writeAll(".").ok(); };
    for (;;) {
        // riley dock/src/ipc.rs:94
        auto path = instancePath(u".socket2.sock"_s);
        IoResult<UnixStream> connection = path.ok() ? unix_io::connectUnix(path.value()) : IoResult<UnixStream>(path.error());
        if (!connection.ok()) {
            if (!deliver(update::Refresh{std::nullopt, u"event socket: "_s + connection.error().message}))
                return;
            std::this_thread::sleep_for(2s);
            continue;
        }
        // riley dock/src/ipc.rs:103
        auto queued = std::make_shared<detail::SharedPending>();
        queued->pending = Pending{true, false};
        auto wake = std::make_shared<detail::WakeChannel>();
        ReaderHandle reader;
        reader.thread = std::thread(readEvents, std::move(connection).value(), queued, wake);
        WakeReceiver pending{wake};
        // riley dock/src/ipc.rs:115
        auto last = Clock::now() - 50ms;
        for (;;) {
            // riley dock/src/ipc.rs:117 std::mem::take
            Pending batch;
            {
                std::lock_guard lock(queued->mutex);
                batch = std::exchange(queued->pending, Pending{});
            }
            if (batch.revealRequests() > 0 && !deliver(update::Attention{}))
                return;
            if (batch.refresh) {
                // riley dock/src/ipc.rs:120 Duration::from_millis(50).checked_sub(last.elapsed())
                const auto elapsed = Clock::now() - last;
                if (elapsed <= 50ms)
                    std::this_thread::sleep_for(50ms - elapsed);
                // riley dock/src/ipc.rs:124 records arriving during the wait join this refresh; their urgency stays
                // queued.
                {
                    std::lock_guard lock(queued->mutex);
                    queued->pending.refresh = false;
                }
                while (wake->tryRecv()) {
                }
                auto result = snapshot();
                update::Refresh refreshed;
                if (result.ok())
                    refreshed.snapshot = std::move(result).value();
                else
                    refreshed.error = u"refresh: "_s + result.error().message;
                if (!deliver(std::move(refreshed)))
                    return;
                last = Clock::now();
            }
            // riley dock/src/ipc.rs:130
            bool more = false;
            {
                std::lock_guard lock(queued->mutex);
                more = queued->pending.refresh || queued->pending.urgent;
            }
            if (!more && !wake->recv())
                break;
        }
        // riley dock/src/ipc.rs:133
        reader.thread.join();
        if (!deliver(update::Refresh{std::nullopt, u"event socket disconnected; reconnecting"_s}))
            return;
        std::this_thread::sleep_for(1s);
    }
}

} // namespace

// riley dock/src/ipc.rs:87
IoResult<std::pair<Updates, UnixStream>> updates()
{
    auto channel = std::make_shared<detail::UpdateChannel>();
    auto pair = unix_io::socketPair();
    if (!pair.ok())
        return pair.error();
    UnixStream notify = std::move(pair.value().first);
    UnixStream signal = std::move(pair.value().second);
    if (const auto set = unix_io::setNonblocking(signal.fd()); !set.ok())
        return set.error();
    Updates receiver(std::make_shared<Updates::Queue>(channel));
    std::thread(updatesWorker, std::move(channel), std::move(notify)).detach();
    return std::pair<Updates, UnixStream>(std::move(receiver), std::move(signal));
}

// riley dock/src/ipc.rs:145
IoResult<QStringList> commands(const QString &address, const WindowAction &action)
{
    // riley dock/src/ipc.rs:146 address.strip_prefix("0x").unwrap_or("")
    const QStringView hex = address.startsWith(u"0x"_s) ? QStringView(address).mid(2) : QStringView();
    if (hex.isEmpty() || !allAsciiHex(hex))
        return unix_io::other(u"invalid window address"_s);
    const QString focus = u"dispatch hl.dsp.focus({ window = \"address:%1\" })"_s.arg(address);
    struct Visitor {
        const QString &address;
        const QString &focus;
        IoResult<QStringList> operator()(const window_action::Focus &) const { return QStringList{focus}; }
        IoResult<QStringList> operator()(const window_action::Close &) const
        {
            return QStringList{u"dispatch hl.dsp.window.close({ window = \"address:%1\" })"_s.arg(address)};
        }
        IoResult<QStringList> operator()(const window_action::Float &) const
        {
            return QStringList{
                u"dispatch hl.dsp.window.float({ action = \"toggle\", window = \"address:%1\" })"_s.arg(address)};
        }
        IoResult<QStringList> operator()(const window_action::Fullscreen &) const
        {
            return QStringList{focus, u"dispatch hl.dsp.window.fullscreen({ mode = \"fullscreen\" })"_s};
        }
        IoResult<QStringList> operator()(const window_action::Move &move) const
        {
            // riley dock/src/ipc.rs:156 Move(n @ 1..=9)
            if (move.workspace >= 1 && move.workspace <= 9) {
                return QStringList{
                    u"dispatch hl.dsp.window.move({ workspace = \"%1\", follow = false, window = \"address:%2\" })"_s
                        .arg(QString::number(move.workspace), address)};
            }
            return unix_io::other(u"workspace must be 1–9"_s);
        }
    };
    return std::visit(Visitor{address, focus}, action);
}

} // namespace ipc

} // namespace riley::dock
