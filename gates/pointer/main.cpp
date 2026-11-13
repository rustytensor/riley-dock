// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-dock-pointer: the capture gate's pointer helper (gates/qt_capture.py).
//
// It moves the session's pointer with the wlr virtual pointer protocol, so the dock under test gets real motion
// events, and shows a small red ring at the scripted position in its own overlay layer, so a person at the desk can
// tell the test's pointer from their own. It sends pointer MOTION only (motion_absolute and frame): there is no code
// path for a button, axis or key event. It always puts the pointer back where it was when it started: on "quit", at
// the end of stdin, and on SIGTERM, SIGINT or SIGHUP.

#include <QBackingStore>
#include <QExposeEvent>
#include <QGuiApplication>
#include <QPainter>
#include <QPen>
#include <QResizeEvent>
#include <QScreen>
#include <QSocketNotifier>
#include <QSurfaceFormat>
#include <QTimer>
#include <QWindow>

#include <LayerShellQt/Window>

#include <wayland-client.h>

#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <functional>
#include <optional>
#include <string>
#include <unistd.h>

namespace {

constexpr const char *kProgram = "riley-dock-pointer";
constexpr const char *kNamespace = "riley-dock-test-marker";
constexpr double kRingWidth = 24.0;        // outer diameter of the ring, logical pixels
constexpr double kRingStroke = 3.0;        // its stroke, logical pixels
constexpr double kUnitsPerPixel = 8.0;     // motion_absolute units per logical pixel: halves and quarters are exact
constexpr int kReadyTimeoutMs = 5000;      // the ring's layer must be mapped within this time
constexpr std::size_t kMaxLine = 4096;

const char kUsage[] = R"(Usage: riley-dock-pointer --restore X Y [--output NAME] [--layout X Y WIDTH HEIGHT]

The capture gate's pointer helper (gates/qt_capture.py). It moves the pointer of the Wayland session with the wlr
virtual pointer protocol (zwlr_virtual_pointer_v1: absolute motion and frame only, never a button, axis or key)
and draws a small red ring at the scripted position in its own overlay layer (namespace riley-dock-test-marker) on
one output, so a person can tell the test's pointer from their own. The ring's layer takes no input, no keyboard
and no space.

Options:
  --restore X Y      where the pointer is now (from hyprctl cursorpos). The pointer is moved back there on quit,
                     at the end of stdin and on SIGTERM, SIGINT or SIGHUP. Required.
  --output NAME      the output that shows the ring (default DP-2)
  --layout X Y W H   the bounding box of all outputs in logical pixels (from hyprctl monitors -j). Absolute motion
                     is relative to this box. Default: the union of the screens Qt reports.
  --help             this text

Commands on stdin, one per line:
  move X Y           move the pointer to X,Y (global layout coordinates, logical pixels) and the ring with it
  hide               hide the ring (for a screenshot)
  show               show the ring again
  quit               move the pointer back to the --restore position and exit

Answers on stdout, one line each: "ready" once the ring's layer is mapped and the virtual pointer exists; then
"ok" for each command, written after the compositor has processed it (for hide: has the frame without the ring),
or "error <reason>" for a command that was not carried out.

Exit status: 0 after quit or the end of stdin, 128+N after signal N, 1 when it cannot start or loses the Wayland
connection, 2 for bad arguments. Without a Wayland display it refuses to start.
)";

struct Options {
    double restoreX = 0;
    double restoreY = 0;
    bool haveRestore = false;
    QString output = QStringLiteral("DP-2");
    std::optional<QRectF> layout;
};

bool parseNumber(const char *text, double &out)
{
    char *end = nullptr;
    errno = 0;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || errno != 0 || !std::isfinite(value))
        return false;
    out = value;
    return true;
}

void complain(const std::string &text)
{
    std::fprintf(stderr, "%s: %s\n", kProgram, text.c_str());
}

void answer(const std::string &text)
{
    std::fputs(text.c_str(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

uint32_t nowMs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint32_t(uint64_t(ts.tv_sec) * 1000u + uint64_t(ts.tv_nsec) / 1000000u);
}

int signalPipe[2] = {-1, -1};

void onSignal(int signo)
{
    const unsigned char byte = static_cast<unsigned char>(signo);
    const ssize_t written = write(signalPipe[1], &byte, 1);
    (void)written;
}

// --- The virtual pointer, on a private event queue of Qt's own Wayland connection ---------------------------------
// Sharing Qt's connection keeps the ring's commits and the pointer's requests in one ordered stream, so one sync
// answers for both. The private queue keeps this code's events out of Qt's dispatching.

class VirtualPointer
{
public:
    explicit VirtualPointer(QRectF layout) : m_layout(layout) {}
    VirtualPointer(const VirtualPointer &) = delete;
    VirtualPointer &operator=(const VirtualPointer &) = delete;

    ~VirtualPointer()
    {
        if (m_pointer)
            zwlr_virtual_pointer_v1_destroy(m_pointer);
        if (m_manager)
            zwlr_virtual_pointer_manager_v1_destroy(m_manager);
        if (m_registry)
            wl_registry_destroy(m_registry);
        if (m_wrapper)
            wl_proxy_wrapper_destroy(m_wrapper);
        if (m_display)
            wl_display_flush(m_display);
        if (m_queue)
            wl_event_queue_destroy(m_queue);
    }

    bool connect(wl_display *display, wl_seat *seat, std::string &error)
    {
        m_display = display;
        m_queue = wl_display_create_queue(display);
        m_wrapper = static_cast<wl_display *>(wl_proxy_create_wrapper(display));
        if (!m_queue || !m_wrapper) {
            error = "cannot make a private Wayland event queue";
            return false;
        }
        wl_proxy_set_queue(reinterpret_cast<wl_proxy *>(m_wrapper), m_queue);
        m_registry = wl_display_get_registry(m_wrapper);
        static const wl_registry_listener listener = {&VirtualPointer::global, &VirtualPointer::globalRemove};
        wl_registry_add_listener(m_registry, &listener, this);
        if (wl_display_roundtrip_queue(m_display, m_queue) < 0) {
            error = "lost the Wayland connection while listing its globals";
            return false;
        }
        if (!m_manager) {
            error = "the compositor offers no zwlr_virtual_pointer_manager_v1";
            return false;
        }
        m_pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(m_manager, seat);
        if (!m_pointer || !sync()) {
            error = "cannot make a virtual pointer";
            return false;
        }
        return true;
    }

    bool contains(double x, double y) const
    {
        return x >= m_layout.left() && x <= m_layout.right() && y >= m_layout.top() && y <= m_layout.bottom();
    }

    // Absolute motion to x,y (global layout coordinates, logical pixels), then a frame, then a sync, so the
    // compositor has applied the move when this returns true. Motion only: this class sends nothing else.
    bool moveTo(double x, double y)
    {
        const auto extent = [](double size) {
            return uint32_t(std::clamp<long long>(std::llround(size * kUnitsPerPixel), 1, 0xffffffffLL));
        };
        const uint32_t xExtent = extent(m_layout.width());
        const uint32_t yExtent = extent(m_layout.height());
        const auto units = [](double offset, uint32_t limit) {
            return uint32_t(std::clamp<long long>(std::llround(offset * kUnitsPerPixel), 0, limit));
        };
        zwlr_virtual_pointer_v1_motion_absolute(m_pointer, nowMs(), units(x - m_layout.x(), xExtent),
                                                units(y - m_layout.y(), yExtent), xExtent, yExtent);
        zwlr_virtual_pointer_v1_frame(m_pointer);
        return sync();
    }

    // A wl_display.sync on this connection: true once the compositor has processed every earlier request on it,
    // the ring's surface commits included.
    bool sync()
    {
        bool done = false;
        wl_callback *callback = wl_display_sync(m_wrapper);
        static const wl_callback_listener listener = {&VirtualPointer::syncDone};
        wl_callback_add_listener(callback, &listener, &done);
        while (!done) {
            if (wl_display_dispatch_queue(m_display, m_queue) < 0)
                return false;
        }
        return true;
    }

private:
    static void global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
    {
        auto *self = static_cast<VirtualPointer *>(data);
        if (!self->m_manager && std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0
            && version >= 1) {
            self->m_manager = static_cast<zwlr_virtual_pointer_manager_v1 *>(
                wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1));
        }
    }
    static void globalRemove(void *, wl_registry *, uint32_t) {}
    static void syncDone(void *data, wl_callback *callback, uint32_t)
    {
        *static_cast<bool *>(data) = true;
        wl_callback_destroy(callback);
    }

    QRectF m_layout;
    wl_display *m_display = nullptr;
    wl_event_queue *m_queue = nullptr;
    wl_display *m_wrapper = nullptr;
    wl_registry *m_registry = nullptr;
    zwlr_virtual_pointer_manager_v1 *m_manager = nullptr;
    zwlr_virtual_pointer_v1 *m_pointer = nullptr;
};

// --- The red ring: a full-output, transparent overlay layer that takes no input -----------------------------------

class Marker : public QWindow
{
public:
    explicit Marker(QScreen *screen) : m_store(this), m_origin(screen->geometry().topLeft())
    {
        // Qt::WindowTransparentForInput gives the surface an empty input region: it never takes hover or clicks.
        setFlags(Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus);
        QSurfaceFormat surface = format();
        surface.setAlphaBufferSize(8);
        setFormat(surface);
        setScreen(screen);
        auto *layer = LayerShellQt::Window::get(this);
        layer->setScope(QString::fromLatin1(kNamespace));
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setScreen(screen);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop)
                          | LayerShellQt::Window::AnchorBottom | LayerShellQt::Window::AnchorLeft
                          | LayerShellQt::Window::AnchorRight);
        // -1: reserves no space and is not moved by other layers' reserved space, so it covers the whole output,
        // the dock's area included.
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        layer->setActivateOnShow(false);
        layer->setCloseOnDismissed(false);
        resize(screen->geometry().size());
    }

    std::function<void()> onFirstFrame;

    void setRing(QPointF global)
    {
        m_ring = global;
        render();
    }

    void setRingVisible(bool visible)
    {
        m_visible = visible;
        render();
    }

protected:
    void exposeEvent(QExposeEvent *) override
    {
        if (!isExposed())
            return;
        render();
        if (!m_framed) {
            m_framed = true;
            if (onFirstFrame)
                onFirstFrame();
        }
    }

    void resizeEvent(QResizeEvent *) override
    {
        if (isExposed())
            render();
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::UpdateRequest) {
            render();
            return true;
        }
        return QWindow::event(event);
    }

private:
    // Paints the whole surface and commits it at once, so the caller's next sync covers this frame.
    void render()
    {
        if (!isExposed())
            return;
        const QRect rect(QPoint(0, 0), size());
        if (m_store.size() != size())
            m_store.resize(size());
        m_store.beginPaint(rect);
        {
            QPainter painter(m_store.paintDevice());
            painter.setCompositionMode(QPainter::CompositionMode_Source);
            painter.fillRect(rect, Qt::transparent);
            if (m_visible && m_ring) {
                painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
                painter.setRenderHint(QPainter::Antialiasing);
                QPen pen(QColor(255, 0, 0));
                pen.setWidthF(kRingStroke);
                painter.setPen(pen);
                painter.setBrush(Qt::NoBrush);
                const double radius = (kRingWidth - kRingStroke) / 2.0;   // the stroke's centre line
                painter.drawEllipse(*m_ring - QPointF(m_origin), radius, radius);
            }
        }
        m_store.endPaint();
        m_store.flush(rect);
    }

    QBackingStore m_store;
    QPoint m_origin;
    std::optional<QPointF> m_ring;
    bool m_visible = true;
    bool m_framed = false;
};

// --- Commands -------------------------------------------------------------------------------------------------

class Session
{
public:
    Session(const Options &options, VirtualPointer &pointer, Marker &marker)
        : m_options(options), m_pointer(pointer), m_marker(marker)
    {
    }

    bool finished() const { return m_finished; }

    // Puts the pointer back (only if this helper moved it) and ends the event loop with `code`.
    void finish(int code, const char *reply = nullptr)
    {
        if (m_finished)
            return;
        m_finished = true;
        if (m_moved && !m_pointer.moveTo(m_options.restoreX, m_options.restoreY)) {
            complain("lost the Wayland connection; the pointer could not be put back");
            if (reply)
                answer("error lost the Wayland connection");
            QCoreApplication::exit(1);
            return;
        }
        if (reply)
            answer(reply);
        QCoreApplication::exit(code);
    }

    void feed(const char *data, std::size_t size)
    {
        m_buffer.append(data, size);
        std::size_t end;
        while (!m_finished && (end = m_buffer.find('\n')) != std::string::npos) {
            const std::string line = m_buffer.substr(0, end);
            m_buffer.erase(0, end + 1);
            run(line);
        }
        if (m_buffer.size() > kMaxLine) {
            m_buffer.clear();
            answer("error line too long");
        }
    }

private:
    void run(const std::string &raw)
    {
        const QStringList words = QString::fromUtf8(raw.data(), qsizetype(raw.size()))
                                      .split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const QString command = words.value(0).trimmed();
        if (command == QLatin1String("move")) {
            double x = 0, y = 0;
            if (words.size() != 3 || !parseNumber(words[1].toUtf8().constData(), x)
                || !parseNumber(words[2].toUtf8().constData(), y)) {
                answer("error move needs two numbers: move X Y");
                return;
            }
            if (!m_pointer.contains(x, y)) {
                answer("error the position is outside the layout");
                return;
            }
            m_marker.setRing(QPointF(x, y));
            m_moved = true;
            if (!m_pointer.moveTo(x, y))
                return lost();
            answer("ok");
        } else if ((command == QLatin1String("hide") || command == QLatin1String("show")) && words.size() == 1) {
            m_marker.setRingVisible(command == QLatin1String("show"));
            if (!m_pointer.sync())
                return lost();
            answer("ok");
        } else if (command == QLatin1String("quit") && words.size() == 1) {
            finish(0, "ok");
        } else {
            answer("error unknown command (move X Y, hide, show, quit)");
        }
    }

    void lost()
    {
        complain("lost the Wayland connection");
        answer("error lost the Wayland connection");
        m_finished = true;
        QCoreApplication::exit(1);
    }

    const Options &m_options;
    VirtualPointer &m_pointer;
    Marker &m_marker;
    std::string m_buffer;
    bool m_moved = false;
    bool m_finished = false;
};

int parseArguments(int argc, char **argv, Options &options)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto numbers = [&](int count, double *out) {
            if (i + count >= argc)
                return false;
            for (int k = 0; k < count; ++k) {
                if (!parseNumber(argv[i + 1 + k], out[k]))
                    return false;
            }
            i += count;
            return true;
        };
        if (arg == "--help" || arg == "-h") {
            std::fputs(kUsage, stdout);
            return 0;
        } else if (arg == "--restore") {
            double xy[2];
            if (!numbers(2, xy)) {
                complain("--restore needs two numbers: X Y");
                return 2;
            }
            options.restoreX = xy[0];
            options.restoreY = xy[1];
            options.haveRestore = true;
        } else if (arg == "--layout") {
            double box[4];
            if (!numbers(4, box) || box[2] <= 0 || box[3] <= 0) {
                complain("--layout needs four numbers, X Y WIDTH HEIGHT, with a size above 0");
                return 2;
            }
            options.layout = QRectF(box[0], box[1], box[2], box[3]);
        } else if (arg == "--output") {
            if (i + 1 >= argc || !*argv[i + 1]) {
                complain("--output needs a name");
                return 2;
            }
            options.output = QString::fromLocal8Bit(argv[++i]);
        } else {
            complain("unknown argument '" + arg + "' (see --help)");
            return 2;
        }
    }
    if (!options.haveRestore) {
        complain("--restore X Y is required (see --help)");
        return 2;
    }
    return -1;
}

} // namespace

int main(int argc, char **argv)
{
    Options options;
    if (const int status = parseArguments(argc, argv, options); status >= 0)
        return status;

    // Refuse to start without a Wayland display, before Qt picks another platform or aborts.
    unsetenv("WAYLAND_SOCKET");
    const char *waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (!waylandDisplay || !*waylandDisplay) {
        complain("no Wayland display: WAYLAND_DISPLAY is not set; refusing to start");
        return 1;
    }
    if (wl_display *probe = wl_display_connect(nullptr)) {
        wl_display_disconnect(probe);
    } else {
        complain(std::string("cannot connect to the Wayland display '") + waylandDisplay + "'; refusing to start");
        return 1;
    }
    setenv("QT_QPA_PLATFORM", "wayland", 1);

    if (pipe2(signalPipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        complain(std::string("pipe: ") + std::strerror(errno));
        return 1;
    }
    struct sigaction action{};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    for (const int signo : {SIGTERM, SIGINT, SIGHUP})
        sigaction(signo, &action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);   // a closed stdout must not kill the helper before it puts the pointer back

    QGuiApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    auto *wayland = app.nativeInterface<QNativeInterface::QWaylandApplication>();
    if (QGuiApplication::platformName() != QLatin1String("wayland") || !wayland || !wayland->display()) {
        complain("Qt is not on Wayland; refusing to start");
        return 1;
    }

    QScreen *screen = nullptr;
    QRectF united;
    for (QScreen *candidate : QGuiApplication::screens()) {
        united = united.united(QRectF(candidate->geometry()));
        if (candidate->name() == options.output)
            screen = candidate;
    }
    if (!screen) {
        complain("no output named '" + options.output.toStdString() + "'");
        return 1;
    }
    VirtualPointer pointer(options.layout.value_or(united));
    if (!pointer.contains(options.restoreX, options.restoreY)) {
        complain("the --restore position is outside the layout");
        return 2;
    }
    std::string error;
    if (!pointer.connect(wayland->display(), wayland->seat(), error)) {
        complain(error);
        return 1;
    }

    Marker marker(screen);
    Session session(options, pointer, marker);

    QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
    input.setEnabled(false);
    QObject::connect(&input, &QSocketNotifier::activated, [&] {
        char buffer[4096];
        const ssize_t got = read(STDIN_FILENO, buffer, sizeof buffer);
        if (got < 0 && (errno == EINTR || errno == EAGAIN))
            return;
        if (got <= 0) {   // end of stdin, or it cannot be read: put the pointer back and leave
            input.setEnabled(false);
            session.finish(0);
            return;
        }
        session.feed(buffer, std::size_t(got));
        if (session.finished())
            input.setEnabled(false);
    });

    QSocketNotifier signalNotifier(signalPipe[0], QSocketNotifier::Read);
    QObject::connect(&signalNotifier, &QSocketNotifier::activated, [&] {
        unsigned char signo = 0;
        if (read(signalPipe[0], &signo, 1) == 1) {
            input.setEnabled(false);
            session.finish(128 + signo);
        }
    });

    bool ready = false;
    marker.onFirstFrame = [&] {
        if (!pointer.sync()) {
            complain("lost the Wayland connection");
            QCoreApplication::exit(1);
            return;
        }
        ready = true;
        answer("ready");
        input.setEnabled(true);
    };
    QTimer::singleShot(kReadyTimeoutMs, &app, [&] {
        if (!ready) {
            complain("the ring's layer on " + options.output.toStdString() + " was not shown within 5 s");
            session.finish(1);
        }
    });

    marker.show();
    return app.exec();
}
