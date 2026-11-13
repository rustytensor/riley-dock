// SPDX-License-Identifier: GPL-3.0-or-later
//
// Literal port of riley dock/tests/launcher.rs. Expected values are the Rust test's, unchanged.
#include <riley-dock/grid.h>
#include <riley-dock/motion.h>
#include <riley-dock/rules.h>

#include <QtTest/QTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace grid = riley::dock::grid;
namespace motion = riley::dock::motion;
namespace rules = riley::dock::rules;
using grid::Animation;
using grid::Look;
using grid::Position;
using motion::InputRect;
using motion::LayoutConfig;
using motion::Point;
using motion::Rect;

namespace {

// riley dock/tests/launcher.rs:3 near(): |actual - expected| < 1e-9.
constexpr double TOLERANCE = 1e-9;

#define NEAR(actual, expected)                                                                                   \
    do {                                                                                                         \
        const double near_a = (actual);                                                                          \
        const double near_e = (expected);                                                                        \
        QVERIFY2(std::abs(near_a - near_e) < TOLERANCE,                                                          \
                 qPrintable(QStringLiteral("%1 != %2").arg(near_a, 0, 'g', 17).arg(near_e, 0, 'g', 17)));       \
    } while (false)

// riley dock/tests/launcher.rs:4 fixture().
LayoutConfig fixture()
{
    return motion::reflectionLayout(LayoutConfig{.surfaceWidth = 200.0, .restWidth = 40.0, .gap = 8.0,
                                                 .sidePadding = 10.0, .barY = 64.0, .barHeight = 56.0,
                                                 .restBottom = 104.0},
                                    true);
}

QStringList ids(const QList<rules::DockItem> &items)
{
    QStringList result;
    for (const rules::DockItem &item : items)
        result.append(item.id);
    return result;
}

std::optional<qsizetype> launcherIndex(const QList<rules::DockItem> &items)
{
    for (qsizetype i = 0; i < items.size(); ++i)
        if (items[i].id == QLatin1StringView("launcher"))
            return i;
    return std::nullopt;
}

using Dots = std::array<Point, 9>;

} // namespace

class tst_launcher : public QObject
{
    Q_OBJECT

private slots:
    // Source: riley dock/tests/launcher.rs:10 grid_positions_and_fixed_frame
    void grid_positions_and_fixed_frame()
    {
        const QStringList pins = {QStringLiteral("A"), QStringLiteral("B")};
        struct Case {
            Position position;
            QStringList order;
            std::optional<double> x;
        };
        const Case cases[] = {
            {Position::Left, {"launcher", "A", "B"}, 32.0},
            {Position::Right, {"A", "B", "launcher"}, 128.0},
            {Position::Center, {"A", "launcher", "B"}, 80.0},
            {Position::Off, {"A", "B"}, std::nullopt},
        };
        for (const Case &c : cases) {
            const QList<rules::DockItem> items = grid::placeItems(rules::items(pins, {}), c.position);
            QCOMPARE(ids(items), c.order); // rs:19
            QCOMPARE(grid::appCount(items), qsizetype(2)); // rs:20 Hiding a control must not change app crowding.
            const std::optional<qsizetype> index = launcherIndex(items);
            const motion::Layout layout = grid::layout(fixture(), QList<double>(items.size(), 1.0), index);
            if (c.x) {
                // Bar74 centred at y101 => fixed grid y81; three slots give bar156 at x22.
                QVERIFY(index.has_value()); // rs:25 unwrap
                QCOMPARE(layout.icons[*index], (Rect{*c.x, 81.0, 40.0, 40.0})); // rs:25
                QCOMPARE(layout.bar, (Rect{22.0, 64.0, 156.0, 74.0}));          // rs:26
            } else {
                QVERIFY(!index.has_value()); // rs:28
                // Two apps: 80+8+20=108, centred at x46; no grid slot/input remains.
                QCOMPARE(layout.inputRegion(), (QList<InputRect>{InputRect{46, 64, 108, 74}})); // rs:30
            }
        }
        const motion::Layout hover = grid::layout(fixture(), {1.6, 1.0, 1.0}, 0);
        QVERIFY2(hover.icons[0].width == 40.0, "grid must not inherit the swell"); // rs:34
        QCOMPARE(hover.icons[0], (Rect{32.0, 81.0, 40.0, 40.0})); // rs:35
        // Neighbour grows40->64: centred bar widens24, so grid x shifts left12. y/w/h stay fixed.
        const motion::Layout neighbour = grid::layout(fixture(), {1.0, 1.6, 1.0}, 0);
        QCOMPARE(neighbour.icons[0], (Rect{20.0, 81.0, 40.0, 40.0})); // rs:38
        const motion::Reflection reflected = grid::reflection(hover.icons[0]);
        QCOMPARE(reflected.visible, (Rect{32.0, 121.0, 40.0, 18.0})); // rs:40
        QCOMPARE(reflected.stops, (std::array<Point, 2>{Point{0.0, 0.55}, Point{1.0, 0.0}})); // rs:41
        LayoutConfig plain = fixture();
        plain.barHeight = 56.0;
        NEAR(grid::layout(plain, {1.6, 1.0, 1.0}, 0).icons[0].y, 72.0); // rs:43
        const QList<rules::DockItem> odd = grid::placeItems(
            rules::items({QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")}, {}), Position::Center);
        QCOMPARE(ids(odd), (QStringList{"A", "B", "launcher", "C"})); // rs:45
    }

    // Source: riley dock/tests/launcher.rs:49 grid_dots_and_animation
    void grid_dots_and_animation()
    {
        const Rect rect{32.0, 81.0, 40.0, 40.0};
        const auto [dots, radius] = grid::dots(rect, Look{});
        // Centre52,101; rest step10, radius2.5. Nine literal points, no scaled button image.
        QCOMPARE(dots, (Dots{Point{42.0, 91.0}, Point{52.0, 91.0}, Point{62.0, 91.0}, Point{42.0, 101.0},
                             Point{52.0, 101.0}, Point{62.0, 101.0}, Point{42.0, 111.0}, Point{52.0, 111.0},
                             Point{62.0, 111.0}})); // rs:53
        NEAR(radius, 2.5); // rs:55
        const auto [hover, r] = grid::dots(rect, Look{1.2, 1.0});
        QCOMPARE(hover, (Dots{Point{40.0, 89.0}, Point{52.0, 89.0}, Point{64.0, 89.0}, Point{40.0, 101.0},
                              Point{52.0, 101.0}, Point{64.0, 101.0}, Point{40.0, 113.0}, Point{52.0, 113.0},
                              Point{64.0, 113.0}})); // rs:57
        NEAR(r, 2.5); // rs:59
        const auto pressed = grid::dots(rect, Look{0.6, 1.0}).first;
        QCOMPARE(pressed, (Dots{Point{46.0, 95.0}, Point{52.0, 95.0}, Point{58.0, 95.0}, Point{46.0, 101.0},
                                Point{52.0, 101.0}, Point{58.0, 101.0}, Point{46.0, 107.0}, Point{52.0, 107.0},
                                Point{58.0, 107.0}})); // rs:61
        Animation animation;
        QVERIFY(!animation.sample(0.0).active); // rs:64
        animation.setHover(true, 0.0);
        const grid::Sample halfway = animation.sample(0.075);
        // Cubic ease-out at half duration: 1-(1/2)^3=.875 => step11.75.
        NEAR(halfway.look.spacing * 10.0, 11.75); // rs:68
        NEAR(halfway.look.accent, 0.875);         // rs:69
        QVERIFY(!animation.sample(0.15).active);  // rs:70
        animation.press(0.15);
        NEAR(animation.sample(0.15).look.spacing * 10.0, 6.0); // rs:72
        animation.release(0.15, true);
        // Existing spring at140ms: target12 + (12-6)/7.5 =12.8.
        NEAR(animation.sample(0.29).look.spacing * 10.0, 12.8); // rs:75
        const grid::Sample done = animation.sample(0.5);
        NEAR(done.look.spacing * 10.0, 12.0);  // rs:77
        QVERIFY(!done.active && done.launch);  // rs:78
        QVERIFY(!animation.sample(0.6).launch); // rs:79 One launch decision per accepted click.
        animation.setHover(false, 0.6);
        NEAR(animation.sample(0.675).look.spacing * 10.0, 10.25); // rs:81
        QVERIFY(!animation.sample(0.75).active); // rs:82
    }

    // Source: riley dock/tests/launcher.rs:86 grid_preferences
    void grid_preferences()
    {
        QCOMPARE(grid::position(QString()), Position::Left);                                       // rs:87
        QCOMPARE(grid::position(QStringLiteral("LAUNCHER=unknown")), Position::Left);              // rs:88
        QCOMPARE(grid::position(QStringLiteral("LAUNCHER=left\n LAUNCHER = center \n")), Position::Center); // rs:89
        QCOMPARE(grid::position(QStringLiteral("LAUNCHER=right")), Position::Right);               // rs:90
        QCOMPARE(grid::position(QStringLiteral("LAUNCHER=off")), Position::Off);                   // rs:91
    }

    // Not a Rust port: the pointer on the grid button swells no icon. Expected values worked out by hand.
    // Layout: surface 400, rest 40, gap 8, side padding 10, five slots: bar width 5*40 + 4*8 + 20 = 252,
    // bar.x = (400 - 252)/2 = 74, slot x = 84, 132, 180, 228, 276, centres 104, 152, 200, 248, 296.
    // Swell: 1 + 0.3*(1 + cos(pi*d/2.5)) for d = |pointer - centre|/40 < 2.5, else 1:
    //   d 0 -> 1.6, 0.6 -> 1.5186905882264234, 1.2 -> 1.3188371558587941, 1.8 -> 1.1087728030753932,
    //   2.4 -> 1.0023655896056567, 3.0 and 3.6 -> 1.0.
    void grid_button_stops_the_swell()
    {
        const LayoutConfig config{.surfaceWidth = 400.0, .restWidth = 40.0, .gap = 8.0, .sidePadding = 10.0,
                                  .barY = 64.0, .barHeight = 56.0, .restBottom = 104.0};
        QList<rules::DockItem> apps;
        for (const char *id : {"A", "B", "C", "D"})
            apps.append(rules::DockItem{QString::fromLatin1(id), {}});
        const QList<double> ones(5, 1.0);
        const QList<double> still{1.0, 1.0, 1.0, 1.0, 1.0};
        const auto check = [](const QList<double> &actual, const QList<double> &expected) {
            QCOMPARE(actual.size(), expected.size());
            for (qsizetype i = 0; i < expected.size(); ++i)
                NEAR(actual[i], expected[i]);
        };

        // LAUNCHER=center: 4 apps put the grid at ceil(4/2) = 2, rest rect x 180..220.
        const auto centre = launcherIndex(grid::placeItems(apps, Position::Center));
        QCOMPARE(centre, std::optional<qsizetype>(2));
        const motion::Layout centreRest = grid::layout(config, ones, centre);
        NEAR(centreRest.icons[2].x, 180.0);
        for (const double x : {200.0, 180.0, 220.0}) // centre, left edge, right edge
            check(grid::pointerScales(centreRest, x, 40.0, centre), still);
        // In the gap right of the grid (x 224): the usual swell, grid slot 1.0.
        check(grid::pointerScales(centreRest, 224.0, 40.0, centre),
              {1.0, 1.1087728030753932, 1.0, 1.5186905882264234, 1.1087728030753932});
        // On the next icon's centre (x 248).
        check(grid::pointerScales(centreRest, 248.0, 40.0, centre),
              {1.0, 1.0023655896056567, 1.0, 1.6, 1.3188371558587941});

        // LAUNCHER=left: the grid at the start, index 0, rest rect x 84..124.
        const auto start = launcherIndex(grid::placeItems(apps, Position::Left));
        QCOMPARE(start, std::optional<qsizetype>(0));
        const motion::Layout startRest = grid::layout(config, ones, start);
        NEAR(startRest.icons[0].x, 84.0);
        for (const double x : {104.0, 84.0, 124.0}) // centre, left edge, right edge
            check(grid::pointerScales(startRest, x, 40.0, start), still);
        // On the next icon's centre (x 152).
        check(grid::pointerScales(startRest, 152.0, 40.0, start),
              {1.0, 1.6, 1.3188371558587941, 1.0023655896056567, 1.0});
    }
};

QTEST_GUILESS_MAIN(tst_launcher)
#include "tst_launcher.moc"
