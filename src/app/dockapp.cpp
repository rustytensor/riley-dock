// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/dockapp.h"

#include "app/dockwindow.h"
#include "app/filewatch.h"
#include "app/iconprovider.h"
#include "app/launch.h"
#include "app/log.h"

#include <riley-dock/autohide.h>
#include <riley-dock/desktop.h>
#include <riley-dock/pins.h>

#include <QtCore/QDir>
#include <QtCore/QHash>
#include <QtCore/QSocketNotifier>
#include <QtCore/QTimer>
#include <QtGui/QGuiApplication>
#include <QtGui/QIcon>
#include <QtGui/QScreen>
#include <QtQml/QQmlEngine>

#include <cerrno>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

namespace riley::dock::app {

namespace {

QScreen *findScreen(const QString &output)
{
    for (QScreen *screen : QGuiApplication::screens()) {
        if (screen->name() == output)
            return screen;
    }
    return nullptr;
}

// Drain a nonblocking signal socket (riley dock/src/ui.rs:156-164). False when it reached end of file or failed:
// the watch stops (glib::ControlFlow::Break).
bool drain(int fd)
{
    char bytes[64];
    for (;;) {
        const ssize_t n = ::read(fd, bytes, sizeof bytes);
        if (n > 0)
            continue;
        if (n == 0)
            return false;
        if (errno == EINTR)
            continue;
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
}

bool minimizedWindow(const rules::Client &client)
{
    return client.workspace.name == QStringLiteral("special:minimized");
}

} // namespace

DockApp::DockApp(bool swell, bool reflectionsAllowed, std::optional<QString> controlPath, QObject *parent)
    : QObject(parent), m_controlPath(std::move(controlPath))
{
    m_state.swell = swell;
    m_state.reflectionsAllowed = reflectionsAllowed;
    m_state.epoch = monotonicSeconds();
}

DockApp::~DockApp()
{
    qDeleteAll(m_docks); // before the engine, a child of this object
    m_docks.clear();
}

double DockApp::clock() const
{
    return monotonicSeconds() - m_state.epoch;
}

bool DockApp::start(const QStringList &outputs)
{
    State &s = m_state;
    const QDir config(homePath("XDG_CONFIG_HOME", QStringLiteral(".config")));
    s.pinsPath = QDir(homePath("XDG_CACHE_HOME", QStringLiteral(".cache"))).filePath(QStringLiteral("nwg-dock-pinned"));
    s.configPath = config.filePath(QStringLiteral("nwg-dock-hyprland/riley.conf"));
    s.stylePath = config.filePath(QStringLiteral("riley-dock/style.json"));
    const QString preferences = readTextOrEmpty(s.configPath);
    s.size = rules::iconSize(preferences);
    s.reflections = rules::reflectionsEnabled(preferences, s.reflectionsAllowed);
    s.launcher = grid::position(preferences);
    const bool hide = autohide::enabled(preferences);
    s.autohide = hide;
    m_style.load(s.stylePath);
    chooseIconTheme();

    m_engine = new QQmlEngine(this);
    m_engine->addImageProvider(ICON_PROVIDER, new DockIconProvider);
    for (const QString &output : outputs) {
        QScreen *screen = findScreen(output);
        if (!screen) {
            log(QStringLiteral("Qt cannot find screen %1").arg(output));
            continue;
        }
        auto *dock = new DockWindow(*this, int(m_docks.size()), output, screen, hide);
        if (!dock->loaded()) {
            log(QStringLiteral("the dock's QML did not load"));
            delete dock;
            return false;
        }
        m_docks.append(dock);
    }
    if (m_docks.isEmpty()) {
        log(QStringLiteral("no matching Qt screens, no dock"));
        return false;
    }
    s.pinned = pins::load(s.pinsPath).value_or(QStringList());
    refresh();
    for (DockWindow *dock : m_docks)
        dock->setVisible(true);

    // A sleeping screen is unplugged: hide its dock while it is gone and put it back when it returns. A returning
    // screen can get its name a moment later (riley dock/src/ui.rs:137-147).
    const auto screensChanged = [this] {
        for (const int delay : {500, 2500})
            QTimer::singleShot(delay, this, &DockApp::place);
    };
    connect(qGuiApp, &QGuiApplication::screenAdded, this, screensChanged);
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, screensChanged);

    auto updates = ipc::updates();
    if (!updates.ok()) {
        log(QStringLiteral("event notification: %1").arg(updates.error().message));
        return false;
    }
    m_updates.emplace(std::move(updates.value().first));
    m_updateSignal = std::move(updates.value().second);
    m_updateNotifier = new QSocketNotifier(m_updateSignal.fd(), QSocketNotifier::Read, this);
    connect(m_updateNotifier, &QSocketNotifier::activated, this, &DockApp::applyUpdates);

    if (m_controlPath) {
        auto bound = control::Server::bind(*m_controlPath);
        std::optional<IoError> failure;
        if (bound.ok()) {
            auto serving = std::move(bound.value()).serve();
            if (serving.ok()) {
                m_requests.emplace(std::move(serving.value().requests));
                m_requestSignal = std::move(serving.value().signal);
                m_endpoint.emplace(std::move(serving.value().endpoint));
                m_requestNotifier = new QSocketNotifier(m_requestSignal.fd(), QSocketNotifier::Read, this);
                connect(m_requestNotifier, &QSocketNotifier::activated, this, &DockApp::takeRequests);
            } else {
                failure = serving.error();
            }
        } else {
            failure = bound.error();
        }
        if (failure)
            log(QStringLiteral("control socket %1: %2").arg(*m_controlPath, failure->message));
    }

    // riley dock/src/ui.rs:207-229: the style redraws; the pins and the config refresh the icons.
    auto *style = new FileWatch(s.stylePath, this);
    connect(style, &FileWatch::changed, this, [this] { m_style.load(m_state.stylePath); });
    m_watches.append(style);
    for (const QString &path : {s.pinsPath, s.configPath}) {
        auto *watch = new FileWatch(path, this);
        connect(watch, &FileWatch::changed, this, [this] {
            m_state.refreshIcons = true;
            refresh();
        });
        m_watches.append(watch);
    }
    // riley dock/src/ui.rs:230-238: installed applications changed.
    m_desktopWatch = new DesktopEntriesWatch(this);
    connect(m_desktopWatch, &DesktopEntriesWatch::changed, this, [this] {
        desktop::invalidate();
        m_state.refreshIcons = true;
        refresh();
    });
    return true;
}

void DockApp::shutdown()
{
    m_endpoint.reset(); // unlinks the control socket this dock bound (riley dock/src/ui.rs:248)
    for (DockWindow *dock : m_docks)
        dock->hideName();
}

void DockApp::place()
{
    for (DockWindow *dock : m_docks)
        dock->placeOn(findScreen(dock->output()));
}

void DockApp::applyUpdates()
{
    if (!drain(m_updateSignal.fd())) {
        m_updateNotifier->setEnabled(false);
        return;
    }
    std::optional<ipc::Snapshot> latest;
    bool attention = false;
    while (auto update = m_updates->tryReceive()) {
        if (std::holds_alternative<ipc::update::Attention>(*update)) {
            attention = true;
            continue;
        }
        auto &refreshed = std::get<ipc::update::Refresh>(*update);
        if (refreshed.snapshot) {
            latest = std::move(refreshed.snapshot);
            m_lastError.clear();
        } else if (refreshed.error != m_lastError) {
            log(refreshed.error);
            m_lastError = refreshed.error;
        }
    }
    // Attention reveals available docks; it never focuses or moves a window.
    if (attention)
        showDocks(std::nullopt);
    if (latest) {
        if (m_state.menuCount > 0) {
            m_state.pending = std::move(latest);
        } else {
            m_state.snapshot = std::move(*latest);
            refresh();
        }
    }
}

void DockApp::takeRequests()
{
    if (!drain(m_requestSignal.fd())) {
        m_requestNotifier->setEnabled(false);
        return;
    }
    while (auto stream = m_requests->tryReceive())
        showDocks(std::move(stream));
}

void DockApp::refresh()
{
    State &s = m_state;
    if (s.menuCount > 0)
        return;
    QStringList pinned = s.pinned;
    quint32 size = s.size;
    bool reflections = s.reflections;
    grid::Position launcher = s.launcher;
    bool hide = s.autohide;
    if (s.refreshIcons) {
        const QString preferences = readTextOrEmpty(s.configPath);
        pinned = pins::load(s.pinsPath).value_or(s.pinned);
        size = rules::iconSize(preferences);
        reflections = rules::reflectionsEnabled(preferences, s.reflectionsAllowed);
        launcher = grid::position(preferences);
        hide = autohide::enabled(preferences);
    }
    // The Settings switch is applied live, without a restart that would unreserve and re-reserve.
    if (hide != s.autohide) {
        s.autohide = hide;
        const double now = clock();
        for (DockWindow *dock : m_docks)
            dock->autohide().setEnabled(hide, now);
    }
    QList<rules::DockItem> items = grid::placeItems(rules::items(pinned, s.snapshot.clients), launcher);
    QList<SignatureEntry> signature;
    for (const rules::DockItem &item : items) {
        bool active = false;
        for (const rules::Client &client : item.windows)
            active |= client.address == s.snapshot.active;
        signature.append({item.id, rules::indicator(item.windows), active});
    }
    const bool rebuild = signature != s.signature || size != s.size || reflections != s.reflections
                         || launcher != s.launcher || s.refreshIcons;
    const grid::Position oldLauncher = s.launcher;
    QStringList oldIds;
    for (const rules::DockItem &item : s.items)
        oldIds.append(item.id);
    s.items = std::move(items);
    s.signature = signature;
    s.pinned = pinned;
    s.size = size;
    s.reflections = reflections;
    s.launcher = launcher;
    if (rebuild) {
        s.refreshIcons = false;
        QList<Icon> icons;
        QStringList ids;
        for (qsizetype i = 0; i < s.items.size(); ++i) {
            const rules::DockItem &item = s.items[i];
            ids.append(item.id);
            Icon icon;
            if (item.id == QStringLiteral("launcher")) {
                icon.name = QStringLiteral("Applications");
                icon.grid = grid::Look{};
            } else {
                // riley dock/src/ui.rs:322-329: the entry's icon, else a theme icon named like the item, else
                // "image-missing".
                const auto info = desktop::appInfo(item.id);
                QString spec;
                if (info && !info->icon.isEmpty())
                    spec = info->icon;
                else
                    spec = QIcon::hasThemeIcon(item.id) ? item.id : QStringLiteral("image-missing");
                icon.iconSource = iconUrl(spec);
                icon.symbolic = spec.endsWith(QStringLiteral("-symbolic"));
                icon.name = info ? info->name : item.id;
            }
            icon.indicator = signature[i].indicator;
            icon.active = signature[i].active;
            icons.append(icon);
        }
        for (DockWindow *dock : m_docks)
            dock->rebuild(icons, oldIds, ids, launcher != oldLauncher || launcher == grid::Position::Off);
    }
    // Title/workspace text is live, but unchanged icons never get rebuilt.
    for (DockWindow *dock : m_docks)
        dock->updateName();
    if (rebuild) {
        for (DockWindow *dock : m_docks) {
            dock->frame();
            dock->wake();
        }
    }
}

void DockApp::showDocks(std::optional<ipc::UnixStream> ack)
{
    const double now = clock();
    QList<DockWindow *> visible;
    for (DockWindow *dock : m_docks) {
        if (dock->isVisible())
            visible.append(dock);
    }
    for (DockWindow *dock : visible)
        dock->autohide().show(now);
    if (ack)
        m_state.acks.push_back(std::move(*ack));
    for (DockWindow *dock : visible) {
        dock->schedule();
        dock->wake();
    }
    if (visible.isEmpty())
        return reply(QStringLiteral("none"));
    if (m_state.acks.empty() || m_state.ackWatch)
        return;
    m_state.ackWatch = true;
    // Acknowledge after a frame has been painted with every available dock settled shown.
    m_ackDock = visible.front();
    m_ackStarted = monotonicSeconds();
    m_ackSettled = 0;
    m_ackDock->startAckWatch();
}

bool DockApp::ackTick()
{
    bool all = true;
    for (DockWindow *dock : m_docks) {
        if (dock->isVisible())
            all &= dock->autohide().settledShown();
    }
    m_ackSettled = all ? m_ackSettled + 1 : 0;
    const bool timeout = monotonicSeconds() - m_ackStarted > 3.0;
    if (m_ackSettled < 2 && !timeout)
        return true;
    m_state.ackWatch = false;
    reply(timeout ? QStringLiteral("timeout") : QString(control::SHOWN));
    return false;
}

void DockApp::reply(const QString &text)
{
    std::vector<ipc::UnixStream> acks = std::move(m_state.acks);
    m_state.acks.clear();
    const QByteArray line = text.toUtf8() + '\n';
    for (ipc::UnixStream &stream : acks) {
        const int flags = ::fcntl(stream.fd(), F_GETFL);
        if (flags >= 0)
            ::fcntl(stream.fd(), F_SETFL, flags | O_NONBLOCK);
        (void)stream.writeAll(line);
    }
}

void DockApp::openMenu(int index, int item)
{
    State &s = m_state;
    DockWindow *dock = m_docks[index];
    DockMenu *menu = dock->model()->menu();
    if (menu->open())
        menu->dismiss(); // one menu per dock: the earlier one closes first
    s.menuCount += 1;
    const double now = clock();
    for (DockWindow *each : m_docks)
        each->autohide().setBlockers(true, false, now);
    dock->hideName();
    Q_UNUSED(item);
}

void DockApp::openWindowMenu(int index, int item, const QList<rules::Client> &windows)
{
    openMenu(index, item);
    DockWindow *dock = m_docks[index];
    QVariantList rows;
    for (const rules::Client &window : windows)
        rows.append(QVariantMap{{QStringLiteral("line"), rules::windowLine(window)},
                                {QStringLiteral("address"), window.address},
                                {QStringLiteral("minimized"), minimizedWindow(window)}});
    dock->m_menuWindows = windows;
    const motion::InputRect r = dock->rectangles().value(item).outward();
    dock->model()->menu()->show(QStringLiteral("windows"), QRect(r.x, r.y, r.width, r.height),
                                m_state.items.value(item).id, rows, false);
}

void DockApp::openContextMenu(int index, int item, const rules::DockItem &dockItem)
{
    openMenu(index, item);
    DockWindow *dock = m_docks[index];
    QVariantList rows;
    for (const rules::Client &window : dockItem.windows)
        rows.append(QVariantMap{{QStringLiteral("line"), rules::windowLine(window)},
                                {QStringLiteral("address"), window.address},
                                {QStringLiteral("minimized"), minimizedWindow(window)}});
    dock->m_menuWindows = dockItem.windows;
    const motion::InputRect r = dock->rectangles().value(item).outward();
    dock->model()->menu()->show(QStringLiteral("context"), QRect(r.x, r.y, r.width, r.height), dockItem.id, rows,
                                m_state.pinned.contains(dockItem.id));
}

void DockApp::menuClosed()
{
    State &s = m_state;
    s.menuCount = std::max<qsizetype>(s.menuCount - 1, 0);
    if (s.menuCount == 0 && s.pending) {
        s.snapshot = std::move(*s.pending);
        s.pending.reset();
    }
    const double now = clock();
    const bool menu = s.menuCount > 0;
    for (DockWindow *dock : m_docks)
        dock->autohide().setBlockers(menu, dock->nameVisible(), now);
    for (DockWindow *dock : m_docks)
        dock->schedule();
    refresh();
}

void DockApp::togglePin(const QString &id, bool pinned)
{
    State &s = m_state;
    QStringList next = pins::load(s.pinsPath).value_or(s.pinned);
    if (pinned)
        pins::unpin(next, id);
    else
        pins::pin(next, id);
    QString error;
    if (!pins::save(s.pinsPath, next, &error)) {
        log(QStringLiteral("save pins: %1").arg(error));
        return;
    }
    s.refreshIcons = true;
    refresh();
}

void DockApp::launchLauncher()
{
    runLogged(QStringLiteral("fuzzel"), {}, QStringLiteral("launcher"), false);
}

void DockApp::perform(const rules::Action &action, int index)
{
    if (const auto *launch = std::get_if<rules::action::Launch>(&action)) {
        const auto info = desktop::appInfo(launch->id);
        if (!info) {
            log(QStringLiteral("cannot launch app: no desktop entry"));
            return;
        }
        QString error;
        if (!launchApp(*info, &error)) {
            log(QStringLiteral("launch failed: %1").arg(error));
            return;
        }
        m_docks[index]->m_launches.insert(launch->id, monotonicSeconds());
        m_docks[index]->wake();
    } else if (const auto *focus = std::get_if<rules::action::Focus>(&action)) {
        dispatch(focus->address, ipc::window_action::Focus{});
    } else if (const auto *minimize = std::get_if<rules::action::Minimize>(&action)) {
        runLogged(QStringLiteral("hypr-minimize"), {QStringLiteral("--minimize"), minimize->address});
    } else if (const auto *restore = std::get_if<rules::action::Restore>(&action)) {
        runLogged(QStringLiteral("hypr-minimize"), {QStringLiteral("--restore"), restore->address});
    }
}

void DockApp::dispatch(const QString &address, const ipc::WindowAction &action)
{
    auto commands = ipc::commands(address, action);
    if (!commands.ok()) {
        log(commands.error().message);
        return;
    }
    std::thread([commands = std::move(commands).value()] {
        for (const QString &command : commands) {
            const auto reply = ipc::request(command);
            if (!reply.ok()) {
                log(QStringLiteral("window action: %1").arg(reply.error().message));
                break;
            }
            if (reply.value().trimmed() != "ok") {
                log(QStringLiteral("compositor rejected window action"));
                break;
            }
        }
    }).detach();
}

} // namespace riley::dock::app
