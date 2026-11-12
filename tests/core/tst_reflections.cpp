// SPDX-License-Identifier: GPL-3.0-or-later
//
// Literal port of riley dock/tests/reflections.rs. Expected values are the Rust test's, unchanged.
//
// The approved design (riley dock/tests/reflections.rs:1-8), calculated before the code:
// alpha .55 -> 0, midpoint .275; visible height = .45*h, E = ceil(.45*R).
// R40/B104: rest band (80,104,40,18), midpoint113, end122.
// At1.6: h64,r12; band (68,116,64,28.8), midpoint130.4, end144.8.
// Flip origins remain144/180. Fixture E18: bar74, bottom138, surface146,
// reservation82. (90,140) is reflection-only and must pass input through.
// R44: E20, bar87, reservation95, surface161. Off remains bar67/reservation75.
#include <riley-dock/motion.h>
#include <riley-dock/rules.h>

#include <QtTest/QTest>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

using namespace riley::dock::motion;
namespace rules = riley::dock::rules;

namespace {

// riley dock/tests/reflections.rs:11 near(): |actual - expected| < 1e-9.
constexpr double TOLERANCE = 1e-9;

#define NEAR(actual, expected)                                                                                   \
    do {                                                                                                         \
        const double near_a = (actual);                                                                          \
        const double near_e = (expected);                                                                        \
        QVERIFY2(std::abs(near_a - near_e) < TOLERANCE,                                                          \
                 qPrintable(QStringLiteral("%1 != %2").arg(near_a, 0, 'g', 17).arg(near_e, 0, 'g', 17)));       \
    } while (false)

// riley dock/tests/reflections.rs:15 fixture().
LayoutConfig fixture()
{
    return LayoutConfig{.surfaceWidth = 200.0, .restWidth = 40.0, .gap = 8.0, .sidePadding = 10.0, .barY = 64.0,
                        .barHeight = 56.0, .restBottom = 104.0};
}

const std::array<Point, 2> STOPS = {Point{0.0, 0.55}, Point{1.0, 0.0}};

} // namespace

class tst_reflections : public QObject
{
    Q_OBJECT

private slots:
    // Source: riley dock/tests/reflections.rs:21 reflection_maths_rectangles_and_gradients
    void reflection_maths_rectangles_and_gradients()
    {
        const LayoutConfig config = reflectionLayout(fixture(), true);
        const Layout rest = config.layout({1.0, 1.0, 1.0});
        const Layout grown = config.layout({1.0, 1.6, 1.0});
        const Reflection a = reflection(rest.icons[1], config.restBottom);
        const Reflection b = reflection(grown.icons[1], config.restBottom);
        // Mirror about B=104. Rest bottom=104; swollen bottom=92, hence rise=12.
        QCOMPARE(a.image, (Rect{80.0, 104.0, 40.0, 40.0}));   // rs:28
        QCOMPARE(a.visible, (Rect{80.0, 104.0, 40.0, 18.0})); // rs:29
        QVERIFY2(b.image == (Rect{68.0, 116.0, 64.0, 64.0}), "reflection follows rise downward"); // rs:30
        QCOMPARE(b.visible, (Rect{68.0, 116.0, 64.0, 28.8})); // rs:31
        // y' = flip_y - y: the original bottom reaches the reflected top.
        NEAR(a.flipY, 144.0);        // rs:33
        NEAR(b.flipY, 180.0);        // rs:34
        NEAR(a.flipY - 40.0, 104.0); // rs:35
        NEAR(b.flipY - 64.0, 116.0); // rs:36
        for (const Reflection &r : {a, b})
            QCOMPARE(r.stops, STOPS); // rs:37
        // Half of .55 is .275. Half-band positions: 104+9=113, 116+14.4=130.4.
        NEAR(a.visible.y + a.visible.height / 2.0, 113.0);        // rs:39
        NEAR(b.visible.y + b.visible.height / 2.0, 130.4);        // rs:40
        NEAR((a.stops[0].second + a.stops[1].second) / 2.0, 0.275); // rs:41
        NEAR((b.stops[0].second + b.stops[1].second) / 2.0, 0.275); // rs:42
        NEAR(a.visible.y + a.visible.height, 122.0); // rs:43
        NEAR(b.visible.y + b.visible.height, 144.8); // rs:44
    }

    // Source: riley dock/tests/reflections.rs:48 reflection_maths_lane_and_input
    void reflection_maths_lane_and_input()
    {
        const LayoutConfig config = reflectionLayout(fixture(), true);
        // ceil(40*.45)=18 extra pixels. Bar56+18=74; +gap8 reserves82.
        NEAR(config.barHeight, 74.0);                           // rs:51
        NEAR(std::ceil(config.barHeight) + 8.0, 82.0);          // rs:52
        NEAR(config.barY + config.barHeight + 8.0, 146.0);      // rs:53
        const Layout rest = config.layout({1.0, 1.0, 1.0});
        const Layout grown = config.layout({1.0, 1.6, 1.0});
        QCOMPARE(shelf(rest, 104.0, 10.0), (Rect{32.0, 104.0, 136.0, 1.0}));  // rs:56
        QCOMPARE(shelf(grown, 104.0, 10.0), (Rect{20.0, 104.0, 160.0, 1.0})); // rs:57
        QCOMPARE(rest.inputRegion(), (QList<InputRect>{InputRect{22, 64, 156, 74}})); // rs:58
        const QList<InputRect> region = grown.inputRegion();
        QCOMPARE(region, (QList<InputRect>{InputRect{10, 64, 180, 74}, InputRect{68, 28, 64, 36}})); // rs:60
        // (90,140) lies inside the reflection but below the bar's bottom138.
        QVERIFY(!std::any_of(region.cbegin(), region.cend(), [](const InputRect &r) {
            return 90 >= r.x && 90 < r.x + r.width && 140 >= r.y && 140 < r.y + r.height;
        })); // rs:63
        LayoutConfig unshrunk = fixture();
        unshrunk.restWidth = 44.0;
        unshrunk.barHeight = 67.0;
        unshrunk.barY = 66.0;
        unshrunk.restBottom = 116.0;
        // Default unshrunk icons: ceil(44*.45)=20, bar67+20=87; reservation95.
        const LayoutConfig enabled = reflectionLayout(unshrunk, true);
        NEAR(enabled.barHeight, 87.0);                          // rs:68
        NEAR(std::ceil(enabled.barHeight) + 8.0, 95.0);         // rs:69
        NEAR(enabled.barY + enabled.barHeight + 8.0, 161.0);    // rs:70
        const LayoutConfig disabled = reflectionLayout(unshrunk, false);
        NEAR(disabled.barHeight, 67.0);                         // rs:72
        NEAR(std::ceil(disabled.barHeight) + 8.0, 75.0);        // rs:73
    }

    // Source: riley dock/tests/reflections.rs:77 reflection_preferences
    void reflection_preferences()
    {
        QVERIFY(rules::reflectionsEnabled(QString(), true));                 // rs:78
        QVERIFY(!rules::reflectionsEnabled(QStringLiteral("REFLECTIONS=0\n"), true));  // rs:79
        QVERIFY(!rules::reflectionsEnabled(QStringLiteral("REFLECTIONS=1\n"), false)); // rs:80
        // Invalid values use the documented default; last occurrence wins.
        QVERIFY(rules::reflectionsEnabled(QStringLiteral("REFLECTIONS=0\n REFLECTIONS = invalid\n"), true)); // rs:82
    }

    // Source: riley dock/tests/reflections.rs:86 startup_keeps_pending_fallback_at_expiry
    void startup_keeps_pending_fallback_at_expiry()
    {
        const rules::ScreenSelection fallback{{QStringLiteral("Virtual-1")}, true};
        std::optional<rules::ScreenSelection> pending;
        // Configured target absent before the deadline: wait with Virtual-1 retained.
        QVERIFY(!rules::startupSelection(fallback, pending, true, false).has_value()); // rs:90
        // The deadline expires even if the final IPC read failed: Virtual-1 still wins over no dock.
        QCOMPARE(rules::startupSelection(std::nullopt, pending, true, true),
                 std::optional<rules::ScreenSelection>(fallback)); // rs:92
        QVERIFY(!pending.has_value()); // rs:93
        // With no requested descriptions, default selection is immediate, not delayed ten seconds.
        const auto immediate = rules::startupSelection(fallback, pending, false, false);
        QVERIFY(immediate.has_value());                                // rs:95 unwrap
        QCOMPARE(immediate->names, QStringList{QStringLiteral("Virtual-1")}); // rs:95
        const rules::ScreenSelection matched{{QStringLiteral("DP-1"), QStringLiteral("DP-3")}, false};
        QCOMPARE(rules::startupSelection(matched, pending, true, false),
                 std::optional<rules::ScreenSelection>(matched)); // rs:97
    }
};

QTEST_GUILESS_MAIN(tst_reflections)
#include "tst_reflections.moc"
