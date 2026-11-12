// SPDX-License-Identifier: GPL-3.0-or-later
//
// Literal port of riley dock/tests/motion.rs. Expected values are the Rust test's, unchanged.
#include <riley-dock/motion.h>

#include <QtTest/QTest>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

using namespace riley::dock::motion;

namespace {

// riley dock/tests/motion.rs:6 near(): |actual - expected| < 1e-9.
constexpr double TOLERANCE = 1e-9;

#define NEAR(actual, expected)                                                                                   \
    do {                                                                                                         \
        const double near_a = (actual);                                                                          \
        const double near_e = (expected);                                                                        \
        QVERIFY2(std::abs(near_a - near_e) < TOLERANCE,                                                          \
                 qPrintable(QStringLiteral("%1 != %2").arg(near_a, 0, 'g', 17).arg(near_e, 0, 'g', 17)));       \
    } while (false)

// riley dock/tests/motion.rs:10 fixture().
LayoutConfig fixture()
{
    return LayoutConfig{.surfaceWidth = 200.0, .restWidth = 40.0, .gap = 8.0, .sidePadding = 10.0, .barY = 64.0,
                        .barHeight = 56.0, .restBottom = 104.0};
}

} // namespace

class tst_motion : public QObject
{
    Q_OBJECT

private slots:
    // Source: riley dock/tests/motion.rs:18 cosine_at_hand_worked_distances
    void cosine_at_hand_worked_distances()
    {
        // cos(2pi/5)=(sqrt(5)-1)/4; cos(4pi/5)=-(sqrt(5)+1)/4.
        // Substitute in 1+0.3*(1+cos); beyond 2.5 widths the scale is exactly 1.
        const std::pair<double, double> cases[] = {
            {0.0, 1.6}, {1.0, 1.3927050983124842}, {2.0, 1.0572949016875157}, {2.5, 1.0}, {3.0, 1.0},
        };
        for (const auto &[distance, expected] : cases) {
            NEAR(swellScale(distance), expected);  // rs:25
            NEAR(swellScale(-distance), expected); // rs:26
        }
        // Pointer at 60, fixed centres 20/60/100, width 40: distances 1/0/1.
        const QList<double> scales = pointerScales({20.0, 60.0, 100.0}, 60.0, 40.0);
        const double expectedScales[] = {1.3927050983124842, 1.6, 1.3927050983124842};
        // Rust zips the two sequences: it compares as many as the shorter one holds.
        for (qsizetype i = 0; i < std::min<qsizetype>(scales.size(), 3); ++i)
            NEAR(scales[i], expectedScales[i]); // rs:31
        // No pointer gives three rest scales, regardless of centre positions.
        QCOMPARE(pointerScales({20.0, 60.0, 100.0}, std::nullopt, 40.0), (QList<double>{1.0, 1.0, 1.0})); // rs:34
    }

    // Source: riley dock/tests/motion.rs:38 packed_icons_and_exact_click_through_region
    void packed_icons_and_exact_click_through_region()
    {
        const Layout layout = fixture().layout({1.0, 1.6, 1.0});
        // 40+64+40+2*8=160 content; +20 padding=180, centred at x=10.
        QCOMPARE(layout.bar, (Rect{10.0, 64.0, 180.0, 56.0})); // rs:41
        // Centre rise=20*0.6=12; bottom=104-12=92; top=92-64=28.
        QCOMPARE(layout.icons, (QList<Rect>{
                                   Rect{20.0, 64.0, 40.0, 40.0},
                                   Rect{68.0, 28.0, 64.0, 64.0},
                                   Rect{140.0, 64.0, 40.0, 40.0},
                               })); // rs:43
        // Only y=28..64 of the centre icon protrudes: height 36.
        const QList<InputRect> region = layout.inputRegion();
        QCOMPARE(region, (QList<InputRect>{
                             InputRect{10, 64, 180, 56},
                             InputRect{68, 28, 64, 36},
                         })); // rs:50
        const auto contains = [&region](int x, int y) {
            return std::any_of(region.cbegin(), region.cend(), [x, y](const InputRect &r) {
                return x >= r.x && x < r.x + r.width && y >= r.y && y < r.y + r.height;
            });
        };
        QVERIFY(contains(70, 30));   // rs:57
        QVERIFY(contains(15, 70));   // rs:58
        QVERIFY(!contains(15, 30));  // rs:59
        QVERIFY(!contains(150, 30)); // rs:60
        // Rest width=120+16+20=156; (200-156)/2=22. No icon protrudes.
        QCOMPARE(fixture().layout({1.0, 1.0, 1.0}).inputRegion(), (QList<InputRect>{
                                                                     InputRect{22, 64, 156, 56},
                                                                 })); // rs:62
        // Legacy rise of 4 leaves widths unchanged; middle top becomes 60.
        const Layout plain = fixture().noSwell(3, 1, 4.0);
        QCOMPARE(plain.icons[1], (Rect{80.0, 60.0, 40.0, 40.0})); // rs:67
        QVERIFY(plain.bar.height == 56.0); // rs:68 Reservation stays the bar height.
        // Bounds 1.25..3.75 and 2.5..5.25 round outward to 1..4 and 2..6.
        QCOMPARE((Rect{1.25, 2.5, 2.5, 2.75}.outward()), (InputRect{1, 2, 3, 4})); // rs:70
    }

    // Source: riley dock/tests/motion.rs:75 leave_spring_and_idle_deadline
    void leave_spring_and_idle_deadline()
    {
        const Spring spring{1.6, 0.0, 1.0};
        // a=ln(7.5)/.14, w=pi/.14. At .1 the phase is 5pi/7.
        // At .14: sin=0, cos=-1, decay=1/7.5 => 1-.6/7.5=.92.
        // At .35: phase=5pi/2, cos=0, sin=1 => residual .0024980571343722.
        const std::pair<double, double> cases[] = {
            {0.0, 1.6}, {0.100, 0.9826359544035586}, {0.140, 0.92}, {0.350, 1.0024980571343722},
        };
        for (const auto &[seconds, expected] : cases)
            NEAR(spring.raw(seconds).value, expected); // rs:84
        NEAR(spring.raw(0.0).velocity, 0.0); // rs:86
        QVERIFY(spring.sample(0.140).active); // rs:87
        // The animation, unlike raw maths, snaps at the agreed 350 ms deadline.
        QCOMPARE(spring.sample(0.350), (Sample{1.0, 0.0, false})); // rs:89
        QCOMPARE(spring.sample(1.0), (Sample{1.0, 0.0, false}));   // rs:90
        // New initial value/velocity are preserved: no reset to 1.6 on retarget.
        const Sample retargeted = Spring{1.2, -0.4, 1.4}.sample(0.0);
        NEAR(retargeted.value, 1.2);     // rs:93
        NEAR(retargeted.velocity, -0.4); // rs:94
    }

    // Source: riley dock/tests/motion.rs:98 click_multiplier_and_return
    void click_multiplier_and_return()
    {
        // Half cosine: cos(0)=1, cos(pi/2)=0, cos(pi)=-1 => 1, .95, .9.
        NEAR(clickSquash(0.0).value, 1.0);    // rs:100
        NEAR(clickSquash(0.045).value, 0.95); // rs:101
        NEAR(clickSquash(0.090).value, 0.9);  // rs:102
        NEAR(clickSquash(0.0).velocity, 0.0);   // rs:103
        NEAR(clickSquash(0.090).velocity, 0.0); // rs:104
        // Swell 1.6 times .9 is 1.44, not absolute .9.
        NEAR(1.6 * clickSquash(0.090).value, 1.44); // rs:106
        // 140 ms into return: initial offset -.1 changes to +.1/7.5.
        NEAR(clickSquash(0.230).value, 1.0133333333333334); // rs:108
        // 90 ms squash + 350 ms return = 440 ms; no idle tick survives.
        QCOMPARE(clickSquash(0.440), (Sample{1.0, 0.0, false})); // rs:110
    }
};

QTEST_GUILESS_MAIN(tst_motion)
#include "tst_motion.moc"
