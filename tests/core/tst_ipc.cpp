// SPDX-License-Identifier: GPL-3.0-or-later
//
// Literal port of the inline test module of riley dock/src/ipc.rs (riley dock/src/ipc.rs:161 mod tests). Expected
// values are the Rust test's, unchanged.
#include <riley-dock/ipc.h>

#include <QtTest/QTest>

namespace ipc = riley::dock::ipc;
namespace window_action = riley::dock::ipc::window_action;

class tst_ipc : public QObject
{
    Q_OBJECT

private slots:
    // Source: riley dock/src/ipc.rs:165 lua_actions
    void lua_actions()
    {
        // The 0.56.2 brief specifies these literal commands; fullscreen acts on focus.
        const auto focus = ipc::commands(QStringLiteral("0xa1"), window_action::Focus{});
        QVERIFY(focus.ok()); // rs:167 unwrap
        QCOMPARE(focus.value(), (QStringList{QStringLiteral("dispatch hl.dsp.focus({ window = \"address:0xa1\" })")})); // rs:167
        const auto close = ipc::commands(QStringLiteral("0xa1"), window_action::Close{});
        QVERIFY(close.ok()); // rs:168 unwrap
        QCOMPARE(close.value(), (QStringList{QStringLiteral("dispatch hl.dsp.window.close({ window = \"address:0xa1\" })")})); // rs:168
        const auto floating = ipc::commands(QStringLiteral("0xa1"), window_action::Float{});
        QVERIFY(floating.ok()); // rs:169 unwrap
        QCOMPARE(floating.value(), (QStringList{QStringLiteral("dispatch hl.dsp.window.float({ action = \"toggle\", window = \"address:0xa1\" })")})); // rs:169
        const auto fullscreen = ipc::commands(QStringLiteral("0xa1"), window_action::Fullscreen{});
        QVERIFY(fullscreen.ok()); // rs:170 unwrap
        QCOMPARE(fullscreen.value(), (QStringList{QStringLiteral("dispatch hl.dsp.focus({ window = \"address:0xa1\" })"),
                                                  QStringLiteral("dispatch hl.dsp.window.fullscreen({ mode = \"fullscreen\" })")})); // rs:170
        const auto move = ipc::commands(QStringLiteral("0xa1"), window_action::Move{3});
        QVERIFY(move.ok()); // rs:171 unwrap
        QCOMPARE(move.value(), (QStringList{QStringLiteral("dispatch hl.dsp.window.move({ workspace = \"3\", follow = false, window = \"address:0xa1\" })")})); // rs:171
        QVERIFY(!ipc::commands(QStringLiteral("0xa1\" })"), window_action::Close{}).ok()); // rs:172
    }
};

QTEST_GUILESS_MAIN(tst_ipc)
#include "tst_ipc.moc"
