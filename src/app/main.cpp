// SPDX-License-Identifier: GPL-3.0-or-later
//
// riley-dock-qt: the command line, the --show client and the startup screen choice. Mirrors riley dock/src/main.rs.
#include "app/config.h"
#include "app/dockapp.h"
#include "app/log.h"

#include <riley-dock/control.h>
#include <riley-dock/ipc.h>
#include <riley-dock/rules.h>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QSocketNotifier>
#include <QtGui/QGuiApplication>
#include <QtQuick/QQuickWindow>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <optional>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

using namespace riley::dock;

namespace {

int signalPipe[2] = {-1, -1};

void onSignal(int)
{
    const char byte = '.';
    [[maybe_unused]] const ssize_t written = ::write(signalPipe[0], &byte, 1);
}

void printErr(const QString &text)
{
    std::fprintf(stderr, "%s\n", text.toLocal8Bit().constData());
}

// Qt's own messages always go to stderr, with the level spelled out the way GLib does for the GTK dock
// ("WARNING", "CRITICAL"), so a check of the dock's stderr sees them. Without this, Qt may send them to the
// journal when stderr is not a terminal.
void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    const char *level = "DEBUG";
    switch (type) {
    case QtDebugMsg: level = "DEBUG"; break;
    case QtInfoMsg: level = "INFO"; break;
    case QtWarningMsg: level = "WARNING"; break;
    case QtCriticalMsg: level = "CRITICAL"; break;
    case QtFatalMsg: level = "CRITICAL"; break;
    }
    const char *category = context.category ? context.category : "default";
    std::fprintf(stderr, "(riley-dock-qt:%d): %s-%s **: %s\n", int(::getpid()), category, level,
                 message.toLocal8Bit().constData());
    std::fflush(stderr);
}

} // namespace

int main(int argc, char **argv)
{
    const QString program = QFileInfo(QString::fromLocal8Bit(argv[0])).fileName();
    std::optional<QString> output;
    bool swell = true;
    bool reflectionsAllowed = true;
    bool show = false;
    std::optional<QString> controlSocket;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg == QStringLiteral("--output")) {
            if (i + 1 >= argc) {
                printErr(QStringLiteral("--output needs a screen name"));
                return 2;
            }
            output = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == QStringLiteral("--no-swell")) {
            swell = false;
        } else if (arg == QStringLiteral("--no-reflections")) {
            reflectionsAllowed = false;
        } else if (arg == QStringLiteral("--show")) {
            show = true;
        } else if (arg == QStringLiteral("--control-socket")) {
            if (i + 1 >= argc) {
                printErr(QStringLiteral("--control-socket needs a path"));
                return 2;
            }
            controlSocket = QString::fromLocal8Bit(argv[++i]);
        } else if (arg == QStringLiteral("--help") || arg == QStringLiteral("-h")) {
            std::printf("%s [--output NAME] [--no-swell] [--no-reflections] [--control-socket PATH]\n"
                        "%s --show [--control-socket PATH]   reveal the running dock; never starts one\n",
                        program.toLocal8Bit().constData(), program.toLocal8Bit().constData());
            return 0;
        } else {
            printErr(QStringLiteral("unknown argument: %1").arg(arg));
            return 2;
        }
    }

    std::optional<QString> controlPath = controlSocket;
    if (!controlPath) {
        auto path = control::defaultPath();
        if (path.ok()) {
            controlPath = path.value();
        } else if (show) {
            printErr(QStringLiteral("%1 --show: %2").arg(program, path.error().message));
            return 1;
        } else {
            app::log(QStringLiteral("no control socket: %1").arg(path.error().message));
        }
    }
    if (show) {
        const auto reply = control::requestShow(*controlPath, std::chrono::seconds(5));
        if (!reply.ok()) {
            printErr(QStringLiteral("%1 --show: %2").arg(program, reply.error().message));
            return 1;
        }
        if (reply.value() == control::SHOWN)
            return 0;
        printErr(QStringLiteral("%1 --show: dock replied \"%2\"").arg(program, reply.value()));
        return 1;
    }

    // riley dock/src/main.rs:65-90: which screens get a dock. SCREENS is read once, at start, from config.xml or
    // riley.conf (docs/files.md section 1.1).
    const QStringList filters = app::loadConfig(app::configPaths(), std::nullopt).config.screens;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::optional<rules::ScreenSelection> pending;
    QStringList selected;
    for (;;) {
        std::optional<rules::ScreenSelection> current;
        std::optional<IoError> error;
        auto monitors = ipc::monitors();
        if (monitors.ok())
            current = rules::selectMonitors(monitors.value(), output, filters);
        else
            error = monitors.error();
        // One deadline decision per response: expiry must not discard a pending fallback.
        const auto now = std::chrono::steady_clock::now();
        const bool expired = now >= deadline;
        if (const auto chosen = rules::startupSelection(current, pending, !filters.isEmpty(), expired)) {
            if (chosen->fallback) {
                const QString reason = filters.isEmpty() ? QStringLiteral("SCREENS unset or empty")
                                                         : QStringLiteral("no SCREENS match");
                app::log(QStringLiteral("%1; falling back to %2").arg(reason, chosen->names.value(0)));
            }
            selected = chosen->names;
            break;
        }
        if (expired) {
            if (error)
                app::log(QStringLiteral("cannot list screens: %1").arg(error->message));
            else
                app::log(output ? QStringLiteral("requested screen not found, no dock")
                                : QStringLiteral("no screens found, no dock"));
            return 0;
        }
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
            std::chrono::milliseconds(250), deadline - now));
    }

    // Qt gets no command-line arguments of its own (riley dock/src/main.rs:94 runs GTK with none).
    int qtArgc = 1;
    char *qtArgv[] = {argv[0], nullptr};
    qInstallMessageHandler(messageHandler);
    // No desktop file name: the dock has no .desktop file, and with a name set Qt registers it with the host portal,
    // which refuses an unknown id with a warning. The dock's surfaces are layer surfaces and popups, which carry no
    // app id, so nothing else depends on it (the GTK dock's "org.riley.Dock" was only its GApplication id).
    QQuickWindow::setDefaultAlphaBuffer(true);
    QGuiApplication application(qtArgc, qtArgv);
    QGuiApplication::setQuitOnLastWindowClosed(false); // a dock whose screen sleeps closes; the dock stays
    const QString platform = QGuiApplication::platformName();
    if (!platform.startsWith(QStringLiteral("wayland")) && platform != QStringLiteral("offscreen")) {
        // riley dock/src/ui.rs:74. "offscreen" is let through for headless checks; nothing is shown there.
        app::log(QStringLiteral("Wayland layer shell is unavailable"));
        return 0;
    }

    // Quit through the event loop on SIGTERM/SIGINT so shutdown unlinks the control endpoint (ui.rs:239-250).
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, signalPipe) == 0) {
        struct sigaction action {};
        action.sa_handler = onSignal;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_RESTART;
        sigaction(SIGTERM, &action, nullptr);
        sigaction(SIGINT, &action, nullptr);
    }
    QSocketNotifier quitNotifier(signalPipe[1], QSocketNotifier::Read);
    QObject::connect(&quitNotifier, &QSocketNotifier::activated, &application, [] { QGuiApplication::quit(); });

    app::DockApp dock(swell, reflectionsAllowed, controlPath);
    QObject::connect(&application, &QGuiApplication::aboutToQuit, &dock, [&dock] { dock.shutdown(); });
    if (!dock.start(selected)) {
        dock.shutdown();
        return 0;
    }
    return application.exec();
}
