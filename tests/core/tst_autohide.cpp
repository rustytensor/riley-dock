// SPDX-License-Identifier: GPL-3.0-or-later
//
// Literal port of riley dock/tests/autohide.rs. Every expected value was worked out by hand before the Rust
// implementation; they are the Rust test's, unchanged. The Rust control-socket directory is a private
// QTemporaryDir here, never the real dock socket.
#include <riley-dock/autohide.h>
#include <riley-dock/control.h>
#include <riley-dock/grid.h>
#include <riley-dock/ipc.h>
#include <riley-dock/motion.h>
#include <riley-dock/rules.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <optional>
#include <utility>

namespace autohide = riley::dock::autohide;
namespace control = riley::dock::control;
namespace grid = riley::dock::grid;
namespace ipc = riley::dock::ipc;
namespace motion = riley::dock::motion;
namespace pose = riley::dock::autohide::pose;
namespace rules = riley::dock::rules;
using autohide::Controller;
using autohide::Pose;
using autohide::Zone;
using grid::Position;
using motion::InputRect;
using motion::LayoutConfig;
using motion::Point;
using motion::Rect;
using rules::Client;
using rules::Workspace;
using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

namespace {

// riley dock/tests/autohide.rs:5 TOLERANCE and :6 near().
constexpr double TOLERANCE = 1e-6;

#define NEAR(actual, expected)                                                                                   \
    do {                                                                                                         \
        const double near_a = (actual);                                                                          \
        const double near_e = (expected);                                                                        \
        QVERIFY2(std::abs(near_a - near_e) < TOLERANCE,                                                          \
                 qPrintable(QStringLiteral("%1 != %2").arg(near_a, 0, 'g', 17).arg(near_e, 0, 'g', 17)));       \
    } while (false)

// Rust assert_eq! on f64: exact equality.
#define EXACT(actual, expected, message)                                                                         \
    do {                                                                                                         \
        const double exact_a = (actual);                                                                         \
        const double exact_e = (expected);                                                                       \
        QVERIFY2(exact_a == exact_e, qPrintable(QStringLiteral("%1 != %2 %3")                                    \
                                                    .arg(exact_a, 0, 'g', 17)                                    \
                                                    .arg(exact_e, 0, 'g', 17)                                    \
                                                    .arg(QStringLiteral(message))));                             \
    } while (false)

// riley dock/tests/autohide.rs:9 fixture(). The 200 px fixture: R40, gap8, padding10, accepted 45% reflection
// band: bar (22,64,156,74), surface 146.
LayoutConfig fixture()
{
    return motion::reflectionLayout(LayoutConfig{.surfaceWidth = 200.0, .restWidth = 40.0, .gap = 8.0,
                                                 .sidePadding = 10.0, .barY = 64.0, .barHeight = 56.0,
                                                 .restBottom = 104.0},
                                    true);
}
// riley dock/tests/autohide.rs:13 SURFACE.
constexpr double SURFACE = 146.0;

// riley dock/tests/autohide.rs:15 client().
Client client(const QString &address, const QString &className, const QString &workspace)
{
    return Client{address, className, QString(), Workspace{1, workspace}};
}

// riley dock/tests/autohide.rs:21 apps(). A runs one window, B is minimized, C has two windows: three running
// apps, LAUNCHER=off.
QList<Client> apps()
{
    return {client(u"0xa"_s, u"A"_s, u"1"_s), client(u"0xb"_s, u"B"_s, u"special:minimized"_s),
            client(u"0xc1"_s, u"C"_s, u"1"_s), client(u"0xc2"_s, u"C"_s, u"1"_s)};
}

QStringList ids(const QList<rules::DockItem> &items)
{
    QStringList result;
    for (const rules::DockItem &item : items)
        result.append(item.id);
    return result;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

} // namespace

class tst_autohide : public QObject
{
    Q_OBJECT

private slots:
    // Source: riley dock/tests/autohide.rs:26 a_hidden_lip_is_exactly_six_pixels
    void a_hidden_lip_is_exactly_six_pixels()
    {
        const LayoutConfig config = fixture();
        // Exact hidden height first: the planted fault (lip height 0) must fail here, before any dock starts.
        const double offset = autohide::hiddenOffset(SURFACE, config.barY);
        EXACT(SURFACE - (config.barY + offset), 6.0, "hidden height must be exactly 6 px"); // rs:30
        EXACT(offset, 76.0, "");                                                            // rs:31
        EXACT(config.barY + config.barHeight + 8.0, SURFACE, "");                           // rs:32
        // Screen bottom S: surface top S-146 in both poses; visible bar top S-82 shown, S-6 hidden.
        EXACT(SURFACE - config.barY, 82.0, "");             // rs:34
        EXACT(SURFACE - (config.barY + offset), 6.0, "");   // rs:35
    }

    // Source: riley dock/tests/autohide.rs:39 geometry_dots_and_zones
    void geometry_dots_and_zones()
    {
        const QList<rules::DockItem> items = grid::placeItems(rules::items({}, apps()), Position::Off);
        QCOMPARE(ids(items), (QStringList{u"A"_s, u"B"_s, u"C"_s})); // rs:41
        const motion::Layout rest = grid::layout(fixture(), {1.0, 1.0, 1.0}, std::nullopt);
        QCOMPARE(rest.bar, (Rect{22.0, 64.0, 156.0, 74.0})); // rs:43
        QCOMPARE(autohide::lipDots(rest, autohide::running(items), SURFACE),
                 (QList<Point>{Point{52.0, 143.0}, Point{100.0, 143.0}, Point{148.0, 143.0}})); // rs:44
        EXACT(autohide::DOT_RADIUS, 1.0, ""); // rs:45
        // Grid and pinned-but-not-running D give no dot: G,D,A,B,C on a 296 px surface, bar x22, centres 52..244.
        const QList<rules::DockItem> mixed = grid::placeItems(rules::items({u"D"_s}, apps()), Position::Left);
        QCOMPARE(ids(mixed), (QStringList{u"launcher"_s, u"D"_s, u"A"_s, u"B"_s, u"C"_s})); // rs:48
        LayoutConfig wideConfig = fixture();
        wideConfig.surfaceWidth = 296.0;
        const motion::Layout wide = grid::layout(wideConfig, {1.0, 1.0, 1.0, 1.0, 1.0}, 0);
        QCOMPARE(autohide::lipDots(wide, autohide::running(mixed), SURFACE),
                 (QList<Point>{Point{148.0, 143.0}, Point{196.0, 143.0}, Point{244.0, 143.0}})); // rs:50
        // Exclusive zone: 6 with auto-hide in every pose, 82 (bar74 + gap8) without.
        QCOMPARE(autohide::exclusiveZone(true, 74.0), 6);   // rs:52
        QCOMPARE(autohide::exclusiveZone(false, 74.0), 82); // rs:53
        // Reflections off: bar56, surface128, hidden offset58; reservations 64 / 6.
        LayoutConfig plain = fixture();
        plain.barHeight = 56.0;
        EXACT(autohide::hiddenOffset(128.0, plain.barY), 58.0, ""); // rs:56
        QCOMPARE(autohide::exclusiveZone(false, plain.barHeight), 64); // rs:57
        QCOMPARE(autohide::exclusiveZone(true, plain.barHeight), 6);   // rs:58
        // Unshrunk ICON_SIZE=44: hidden offset 89, reservation 95 / 6; reflections off 69, 75 / 6.
        struct Case {
            bool reflections;
            double offset;
            int normal;
        };
        for (const Case &c : {Case{true, 89.0, 95}, Case{false, 69.0, 75}}) {
            const auto [config, height] = autohide::dockLayout(44, 3, 3, c.reflections);
            EXACT(autohide::hiddenOffset(double(height), config.barY), c.offset, ""); // rs:62
            QCOMPARE(autohide::exclusiveZone(false, config.barHeight), c.normal);    // rs:63
            QCOMPARE(autohide::exclusiveZone(true, config.barHeight), 6);            // rs:64
        }
    }

    // Source: riley dock/tests/autohide.rs:69 input_regions
    void input_regions()
    {
        const motion::Layout rest = grid::layout(fixture(), {1.0, 1.0, 1.0}, std::nullopt);
        const InputRect lip = autohide::lipRegion(rest.bar, SURFACE);
        QCOMPARE(lip, (InputRect{22, 140, 156, 6})); // rs:72
        const InputRect strip = autohide::triggerStrip(rest.bar, SURFACE);
        QCOMPARE(strip, (InputRect{22, 144, 156, 2})); // rs:74
        QVERIFY2(strip.x >= lip.x && strip.y >= lip.y && strip.x + strip.width <= lip.x + lip.width
                     && strip.y + strip.height <= lip.y + lip.height,
                 "trigger strip must lie inside the lip"); // rs:75
        const QList<InputRect> shown = rest.inputRegion();
        QCOMPARE(shown, (QList<InputRect>{InputRect{22, 64, 156, 74}})); // rs:78
        const QList<InputRect> swollen = grid::layout(fixture(), {1.0, 1.6, 1.0}, std::nullopt).inputRegion();
        QCOMPARE(swollen, (QList<InputRect>{InputRect{10, 64, 180, 74}, InputRect{68, 28, 64, 36}})); // rs:80
        // The fully translated bar clips to the lip.
        QCOMPARE(autohide::clip(InputRect{22, 140, 156, 74}, 200, 146), std::optional<InputRect>(lip)); // rs:82

        Controller dock(true, 76.0);
        QCOMPARE(dock.inputRegion(0.0, shown, lip, 200, 146), (QList<InputRect>{lip})); // rs:85
        // R2 B: revealed from the band, the band stays while the pointer stays in it, then goes.
        dock.pointer(Zone::Band, 0.0);
        dock.advance(0.5);
        QVERIFY(dock.settledShown()); // rs:89
        QCOMPARE(dock.inputRegion(0.5, shown, lip, 200, 146), (QList<InputRect>{shown[0], lip})); // rs:90
        dock.pointer(Zone::Dock, 0.6);
        QCOMPARE(dock.inputRegion(0.6, shown, lip, 200, 146), shown); // rs:92
    }

    // Source: riley dock/tests/autohide.rs:96 motion_oracle
    void motion_oracle()
    {
        for (const auto &[t, expected] : {Point{0.0, 0.0}, Point{0.1, 19.0}, Point{0.2, 76.0}})
            NEAR(autohide::hideSample(0.0, 76.0, t).first, expected); // rs:97
        // R1 A: a = ln(12.5)/0.14, w = pi/0.14, from 76 with velocity 0.
        NEAR(autohide::revealDamping(), std::log(12.5) / 0.14); // rs:99
        for (const auto &[t, expected] :
             {Point{0.0, 76.0}, Point{0.1, 0.063494}, Point{0.14, -6.08}, Point{0.35, 0.110605}})
            NEAR(autohide::revealRaw(76.0, 0.0, t).first, expected); // rs:101
        Controller dock(true, 76.0);
        dock.show(0.0);
        NEAR(dock.offset(0.14), -6.08); // rs:105
        dock.advance(0.35);
        QVERIFY2(dock.offset(0.35) == 0.0 && !dock.animating(), "350 ms: 0, inactive"); // rs:107
        // Hiding interrupted at 199 ms: no jump (76*(0.199/0.2)^2 = 75.2419), and the clamp keeps the lip.
        Controller hiding(false, 76.0);
        hiding.setEnabled(true, 0.0);
        hiding.advance(0.7);
        NEAR(hiding.offset(0.899), 75.2419); // rs:112
        hiding.pointer(Zone::Dock, 0.899);
        NEAR(hiding.offset(0.899), 75.2419); // rs:114
        double highest = std::numeric_limits<double>::lowest(); // Rust f64::MIN
        for (int ms = 0; ms < 350; ++ms)
            highest = std::max(highest, hiding.offset(0.899 + double(ms) / 1000.0));
        EXACT(highest, 76.0, "unclamped this reversal would reach 78.75"); // rs:116
    }

    // Source: riley dock/tests/autohide.rs:120 timer_timeline
    void timer_timeline()
    {
        // 1. Enter the lip at 0, leave at 119: no reveal at 120.
        Controller dock(true, 76.0);
        dock.pointer(Zone::Band, 0.0);
        QCOMPARE(dock.nextDeadline(), std::optional<double>(0.12)); // rs:124
        dock.pointer(Zone::Band, 0.05); // Motion inside the band never restarts the dwell.
        QCOMPARE(dock.nextDeadline(), std::optional<double>(0.12)); // rs:126
        dock.pointer(Zone::Outside, 0.119);
        QCOMPARE(dock.nextDeadline(), std::optional<double>()); // rs:128
        dock.advance(0.12);
        QCOMPARE(dock.pose(), Pose(pose::Hidden{})); // rs:130
        // 2. Enter at 200 and stay: hidden through 319, reveal at 320, settled at 670; no frames during the dwell.
        dock.pointer(Zone::Band, 0.2);
        QCOMPARE(dock.nextDeadline(), std::optional<double>(0.32)); // rs:133
        dock.advance(0.319);
        QVERIFY(dock.settledHidden() && !dock.animating()); // rs:135
        dock.advance(0.32);
        QVERIFY(dock.animating()); // rs:137
        NEAR(dock.offset(0.32), 76.0); // rs:138
        dock.advance(0.669);
        QVERIFY(dock.animating()); // rs:140
        dock.advance(0.67);
        QVERIFY(dock.settledShown() && !dock.nextDeadline().has_value()); // rs:142
        // 3. Leave everything at 1000: shown through 1699, hide at 1700, 19 at 1800, hidden 76 at 1900, then idle.
        dock.pointer(Zone::Outside, 1.0);
        QCOMPARE(dock.nextDeadline(), std::optional<double>(1.7)); // rs:145
        dock.advance(1.699);
        QVERIFY(dock.settledShown()); // rs:147
        dock.advance(1.7);
        QVERIFY(dock.animating()); // rs:149
        NEAR(dock.offset(1.8), 19.0); // rs:150
        dock.advance(1.9);
        QVERIFY2(dock.offset(1.9) == 76.0 && dock.settledHidden() && !dock.animating()
                     && !dock.nextDeadline().has_value(),
                 "(offset, settled_hidden, animating, next_deadline) != (76.0, true, false, None)"); // rs:152
        // 4. Shown: leave at 0, menu opens 600, closes 900 with the label open, label closes 1000: hide at 1700.
        Controller shown(false, 76.0);
        shown.pointer(Zone::Dock, -1.0);
        shown.setEnabled(true, -1.0);
        shown.pointer(Zone::Outside, 0.0);
        QCOMPARE(shown.nextDeadline(), std::optional<double>(0.7)); // rs:158
        shown.setBlockers(true, false, 0.6);
        QCOMPARE(shown.nextDeadline(), std::optional<double>()); // rs:160
        shown.setBlockers(false, true, 0.9);
        QCOMPARE(shown.nextDeadline(), std::optional<double>()); // rs:162
        shown.setBlockers(false, false, 1.0);
        QCOMPARE(shown.nextDeadline(), std::optional<double>(1.7)); // rs:164
        shown.advance(1.699);
        QVERIFY(shown.settledShown()); // rs:166
        shown.advance(1.7);
        QVERIFY(shown.animating()); // rs:168
    }

    // Source: riley dock/tests/autohide.rs:172 show_enable_and_config
    void show_enable_and_config()
    {
        // Keyboard/attention: no dwell; pointer outside, so the hide delay starts when the reveal completes.
        Controller dock(true, 76.0);
        dock.show(2.0);
        dock.show(2.1); // A repeated request does not restart the running reveal.
        QCOMPARE(dock.pose(), Pose(pose::Revealing{2.0, 76.0, 0.0})); // rs:177
        dock.advance(2.35);
        QCOMPARE(dock.nextDeadline(), std::optional<double>(2.35 + 0.7)); // rs:179
        // Disabling clears timers and reveals; enabling keeps the pose and uses the normal leave delay.
        dock.setEnabled(false, 2.5);
        QCOMPARE(dock.nextDeadline(), std::optional<double>()); // rs:182
        QVERIFY(dock.settledShown()); // rs:183
        dock.setEnabled(true, 3.0);
        QVERIFY(dock.settledShown()); // rs:185
        QCOMPARE(dock.nextDeadline(), std::optional<double>(3.7)); // rs:186
        Controller hidden(true, 76.0);
        hidden.setEnabled(false, 0.0);
        QVERIFY(hidden.animating() && !hidden.nextDeadline().has_value()); // rs:189

        QVERIFY(!autohide::enabled(QString()));                                     // rs:191
        QVERIFY(!autohide::enabled(u"AUTOHIDE=yes"_s));                             // rs:192
        QVERIFY(autohide::enabled(u" AUTOHIDE = 1 "_s));                            // rs:193
        QVERIFY(!autohide::enabled(u"AUTOHIDE=1\nAUTOHIDE=0"_s));                   // rs:194
        QVERIFY(autohide::enabled(u"ICON_SIZE=44\nAUTOHIDE=0\nAUTOHIDE=1\n"_s));    // rs:195
    }

    // Source: riley dock/tests/autohide.rs:199 urgency_records
    void urgency_records()
    {
        // Local records only; nothing is broadcast into the session.
        ipc::Pending batch;
        batch.absorb(u"windowtitlev2>>55aa,New title"_s);
        batch.absorb(u"urgent>>55aa"_s);
        QVERIFY(batch.refresh); // rs:204
        QCOMPARE(batch.revealRequests(), qsizetype(1)); // rs:205
        ipc::Pending title;
        title.absorb(u"windowtitlev2>>55aa,New title"_s);
        QVERIFY(title.refresh && title.revealRequests() == 0); // rs:208
    }

    // Source: riley dock/tests/autohide.rs:212 control_socket_ownership
    void control_socket_ownership()
    {
        QTemporaryDir base; // replaces the Rust test's fixed base directory (rs:213)
        QVERIFY(base.isValid());
        const QString dir = base.filePath(u"control-%1"_s.arg(QCoreApplication::applicationPid()));
        QVERIFY(QDir().mkpath(dir)); // rs:215 unwrap
        QVERIFY(QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                               | QFileDevice::ExeOwner)); // rs:216 unwrap (mode 0700)
        // A regular file at the path is not ours to unlink.
        const QString file = dir + u"/plain"_s;
        QVERIFY(writeFile(file, "keep")); // rs:219 unwrap
        QVERIFY(!control::Server::bind(file).ok()); // rs:220
        QCOMPARE(readFile(file), QByteArray("keep")); // rs:221
        // A live endpoint is never replaced; a missing server is an error, never a new dock.
        const QString path = dir + u"/dock.sock"_s;
        auto bound = control::Server::bind(path);
        QVERIFY(bound.ok()); // rs:224 unwrap
        auto served = std::move(bound).value().serve();
        QVERIFY(served.ok()); // rs:224 unwrap
        control::Requests requests = std::move(served.value().requests);
        ipc::UnixStream signal = std::move(served.value().signal);
        std::optional<control::Endpoint> endpoint(std::move(served.value().endpoint));
        Q_UNUSED(signal);
        const auto again = control::Server::bind(path);
        QVERIFY(!again.ok() && again.error().kind == riley::dock::IoErrorKind::AddrInUse); // rs:225
        QVERIFY(QFileInfo::exists(path)); // rs:226
        auto client = std::async(std::launch::async, [path] { return control::requestShow(path, 2s); });
        std::optional<ipc::UnixStream> stream = requests.receive(2s);
        QVERIFY(stream.has_value()); // rs:228 unwrap
        QVERIFY(stream->writeAll("shown\n").ok()); // rs:229 unwrap
        stream.reset(); // rs:230 drop(stream)
        const auto reply = client.get();
        QVERIFY(reply.ok()); // rs:231 unwrap
        QCOMPARE(reply.value(), u"shown"_s); // rs:231
        endpoint.reset(); // rs:232 drop(endpoint)
        QVERIFY(!QFileInfo::exists(path)); // rs:233
        QVERIFY(!control::requestShow(path, 1s).ok()); // rs:234
        QVERIFY(QDir(dir).removeRecursively()); // rs:235 unwrap
    }
};

QTEST_GUILESS_MAIN(tst_autohide)
#include "tst_autohide.moc"
