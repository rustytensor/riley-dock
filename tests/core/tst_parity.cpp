// SPDX-License-Identifier: GPL-3.0-or-later
//
// Literal port of riley dock/tests/parity.rs. Expected values are the Rust test's, unchanged. The Rust scratch
// directory (riley dock/tests/parity.rs:10 struct Scratch) is a private QTemporaryDir here.
#include <riley-dock/desktop.h>
#include <riley-dock/pins.h>
#include <riley-dock/rules.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QTemporaryDir>
#include <QtTest/QTest>

#include <optional>
#include <utility>

namespace desktop = riley::dock::desktop;
namespace pins = riley::dock::pins;
namespace rules = riley::dock::rules;
namespace action = riley::dock::rules::action;
using rules::Action;
using rules::Client;
using rules::Indicator;
using rules::Monitor;
using namespace Qt::StringLiterals;

namespace {

// riley dock/tests/parity.rs:24 windows().
std::optional<QList<Client>> windows()
{
    return rules::clientsFromJson(R"json([
        {"address":"0xa1","class":"firefox","title":"Inbox","workspace":{"id":1,"name":"1"}},
        {"address":"0xa2","class":"firefox","title":"Docs","workspace":{"id":4,"name":"4"}},
        {"address":"0xa3","class":"firefox","title":"Hidden","workspace":{"id":-99,"name":"special:minimized"}}
    ])json");
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

// Prefixes every line so that a child's own report lines never start a line of this test's log.
QByteArray quoted(const QByteArray &text)
{
    QByteArray result;
    for (const QByteArray &line : text.split('\n'))
        result += "    | " + line + '\n';
    return result;
}

} // namespace

class tst_parity : public QObject
{
    Q_OBJECT

private slots:
    // Source: riley dock/tests/parity.rs:33 pins_replace_file_and_preserve_order
    void pins_replace_file_and_preserve_order()
    {
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid()); // rs:16 create_dir_all unwrap
        const QString path = scratch.filePath(u"cache/pinned"_s);
        const auto missing = pins::load(path);
        QVERIFY(missing.has_value());         // rs:36 unwrap
        QCOMPARE(*missing, QStringList());    // rs:36
        QVERIFY(QDir().mkpath(QFileInfo(path).path())); // rs:37 unwrap
        const QByteArray oldBytes = " firefox \n\norg.gnome.Ptyxis\n";
        QVERIFY(writeFile(path, oldBytes)); // rs:39 unwrap
        QFile oldFile(path);
        QVERIFY(oldFile.open(QIODevice::ReadOnly | QIODevice::Unbuffered)); // rs:40 unwrap
        auto loaded = pins::load(path);
        QVERIFY(loaded.has_value()); // rs:41 unwrap
        QStringList list = *loaded;
        // Whitespace and empty lines do not become app ids; line order survives.
        QCOMPARE(list, (QStringList{u"firefox"_s, u"org.gnome.Ptyxis"_s})); // rs:43
        pins::pin(list, u"google-chrome"_s);
        pins::pin(list, u"google-chrome"_s);
        pins::unpin(list, u"firefox"_s);
        QVERIFY(pins::save(path, list)); // rs:47 unwrap
        // Removing Firefox leaves Terminal then Chrome; each has one newline.
        QCOMPARE(readFile(path), QByteArray("org.gnome.Ptyxis\ngoogle-chrome\n")); // rs:49
        const QByteArray retained = oldFile.readAll(); // rs:51
        // A pre-existing descriptor must still reference the old inode after rename.
        QCOMPARE(retained, oldBytes); // rs:53
    }

    // Source: riley dock/tests/parity.rs:57 pinned_then_workspace_and_class_order
    void pinned_then_workspace_and_class_order()
    {
        const auto clients = rules::clientsFromJson(R"json([
            {"address":"0xc1","class":"zeta","title":"Z","workspace":{"id":4,"name":"4"}},
            {"address":"0xf1","class":"firefox","title":"Inbox","workspace":{"id":1,"name":"1"}},
            {"address":"0xb1","class":"beta","title":"B","workspace":{"id":2,"name":"2"}},
            {"address":"0xa1","class":"alpha","title":"A","workspace":{"id":2,"name":"2"}},
            {"address":"0xf2","class":"firefox","title":"Docs","workspace":{"id":4,"name":"4"}},
            {"address":"0xp1","class":"org.gnome.Ptyxis","title":"Shell","workspace":{"id":3,"name":"3"}},
            {"address":"0x00","class":"","title":"Closing","workspace":{"id":1,"name":"1"}},
            {"address":"0xm1","class":"minimized-app","title":"Hidden","workspace":{"id":-99,"name":"special:minimized"}}
        ])json");
        QVERIFY(clients.has_value()); // rs:67 unwrap
        const QStringList pinList = {u"org.gnome.Ptyxis"_s, u"firefox"_s, u"pinned-only"_s};
        const QList<rules::DockItem> items = rules::items(pinList, *clients);
        // Pins lead, -99 precedes workspace 2, alpha precedes beta; closing ghost disappears.
        QStringList ids;
        QList<QStringList> addresses;
        for (const rules::DockItem &item : items) {
            ids.append(item.id);
            QStringList windowAddresses;
            for (const Client &c : item.windows)
                windowAddresses.append(c.address);
            addresses.append(windowAddresses);
        }
        QCOMPARE(ids, (QStringList{u"launcher"_s, u"org.gnome.Ptyxis"_s, u"firefox"_s, u"pinned-only"_s,
                                   u"minimized-app"_s, u"alpha"_s, u"beta"_s, u"zeta"_s})); // rs:71
        QCOMPARE(addresses, (QList<QStringList>{{}, {u"0xp1"_s}, {u"0xf1"_s, u"0xf2"_s}, {}, {u"0xm1"_s},
                                                {u"0xa1"_s}, {u"0xb1"_s}, {u"0xc1"_s}})); // rs:73
    }

    // Source: riley dock/tests/parity.rs:78 clicks_restore_before_considering_focus
    void clicks_restore_before_considering_focus()
    {
        const auto parsed = windows();
        QVERIFY(parsed.has_value()); // rs:29 unwrap
        const QList<Client> &w = *parsed;
        // The active visible singleton minimizes; a hidden one restores even if last active.
        QCOMPARE(rules::leftClick(u"firefox"_s, {}, u"0xa1"_s), Action(action::Launch{u"firefox"_s}));          // rs:81
        QCOMPARE(rules::leftClick(u"firefox"_s, w.mid(0, 1), u"0xa1"_s), Action(action::Minimize{u"0xa1"_s})); // rs:82
        QCOMPARE(rules::leftClick(u"firefox"_s, w.mid(1, 1), u"0xa1"_s), Action(action::Focus{u"0xa2"_s}));    // rs:83
        QCOMPARE(rules::leftClick(u"firefox"_s, w.mid(2, 1), u"0xa1"_s), Action(action::Restore{u"0xa3"_s}));  // rs:84
        QCOMPARE(rules::leftClick(u"firefox"_s, w.mid(2, 1), u"0xa3"_s), Action(action::Restore{u"0xa3"_s}));  // rs:85
        QCOMPARE(rules::leftClick(u"firefox"_s, w.mid(0, 2), u"0xa1"_s), Action(action::Menu{w.mid(0, 2)}));   // rs:86
        // Menu choices always bring a window forward, never minimize it.
        QCOMPARE(rules::menuPick(w[0]), Action(action::Focus{u"0xa1"_s}));   // rs:88
        QCOMPARE(rules::menuPick(w[2]), Action(action::Restore{u"0xa3"_s})); // rs:89
    }

    // Source: riley dock/tests/parity.rs:93 indicators_and_hover_lines
    void indicators_and_hover_lines()
    {
        const auto parsed = windows();
        QVERIFY(parsed.has_value()); // rs:29 unwrap
        const QList<Client> &w = *parsed;
        // Hollow means every window is minimized, not merely one of them.
        QCOMPARE(rules::indicator({}), Indicator::None);                      // rs:96
        QCOMPARE(rules::indicator(w.mid(0, 1)), Indicator::One);              // rs:97
        QCOMPARE(rules::indicator(w.mid(0, 2)), Indicator::Two);              // rs:98
        QCOMPARE(rules::indicator(w.mid(2)), Indicator::Hollow);              // rs:99
        QCOMPARE(rules::indicator({w[2], w[2]}), Indicator::Hollow);          // rs:100
        QCOMPARE(rules::indicator({w[0], w[2]}), Indicator::Two);             // rs:101
        QCOMPARE(rules::hoverText(u"Firefox"_s, w), u"Firefox\nInbox  · 1\nDocs  · 4\nHidden  · minimized"_s); // rs:102
        QCOMPARE(rules::hoverText(u"Firefox"_s, {}), u"Firefox"_s); // rs:103
        QCOMPARE(rules::windowLine(w[1]), u"Docs  · 4"_s);          // rs:104
        Client longTitle = w[2];
        // 48 Unicode scalar characters fit; 49 become 47 plus one ellipsis, before the suffix.
        longTitle.title = u"é"_s.repeated(48);
        QCOMPARE(rules::windowLine(longTitle), u"é"_s.repeated(48) + u"  · minimized"_s); // rs:108
        longTitle.title.append(u"é"_s);
        QCOMPARE(rules::windowLine(longTitle), u"é"_s.repeated(47) + u"…  · minimized"_s); // rs:110
    }

    // Source: riley dock/tests/parity.rs:114 icon_configuration_and_integer_shrink
    void icon_configuration_and_integer_shrink()
    {
        QCOMPARE(rules::iconSize(QString()), quint32(44));                        // rs:115
        QCOMPARE(rules::iconSize(u"ICON_SIZE=nope\n"_s), quint32(44));            // rs:116
        QCOMPARE(rules::iconSize(u"# comment\nICON_SIZE=48\n"_s), quint32(48));   // rs:117
        // 44*5/4=55. App counts 7 and 8 still have floor((n-6)/3)=0.
        QCOMPARE(rules::drawnSize(44, 6), quint32(55)); // rs:119
        QCOMPARE(rules::drawnSize(44, 7), quint32(55)); // rs:120
        QCOMPARE(rules::drawnSize(44, 8), quint32(55)); // rs:121
        // 330/7 and 330/8 truncate to 47 and 41, not a smooth reduction.
        QCOMPARE(rules::drawnSize(44, 9), quint32(47));  // rs:123
        QCOMPARE(rules::drawnSize(44, 12), quint32(41)); // rs:124
    }

    // Source: riley dock/tests/parity.rs:128 icon_lookup_covers_swell
    void icon_lookup_covers_swell()
    {
        // Six apps do not shrink. 44*5/4=55, then ceil(55*8/5)=88.
        quint32 drawn = rules::drawnSize(44, 6);
        QCOMPARE(drawn, quint32(55));                // rs:131
        QCOMPARE(rules::iconLookupSize(drawn), 88);  // rs:132
        // 48*5/4=60, then ceil(60*8/5)=96. These are source lookup sizes.
        drawn = rules::drawnSize(48, 6);
        QCOMPARE(drawn, quint32(60));                // rs:135
        QCOMPARE(rules::iconLookupSize(drawn), 96);  // rs:136
    }

    // Source: riley dock/tests/parity.rs:140 screens_config_and_fallback
    void screens_config_and_fallback()
    {
        const auto parsedMonitors = rules::monitorsFromJson(R"json([
            {"name":"DP-2","description":"GWD ARZOPA 0x37330206","x":1440,"y":1847},
            {"name":"HDMI-A-1","description":"Dell Inc. DELL U2515H YKFWP4CC09VL","x":0,"y":0},
            {"name":"HDMI-A-2","description":"GWD ARZOPA 0000001504150","x":4495,"y":1847},
            {"name":"DP-1","description":"Samsung Electric Company S34C65xU HCNYB00237","x":1440,"y":407},
            {"name":"DP-3","description":"Samsung Electric Company S34C65xU HCNYB00019","x":4880,"y":407}
        ])json");
        QVERIFY(parsedMonitors.has_value()); // rs:147 unwrap
        const QList<Monitor> &monitors = *parsedMonitors;
        // Captured layout: the two Samsungs match the selector; the Dell alone has y=0.
        const QStringList filters = rules::screenFilters(u"SCREENS=S34C65xU\n"_s);
        QCOMPARE(rules::selectMonitors(monitors, std::nullopt, filters),
                 (rules::ScreenSelection{{u"DP-1"_s, u"DP-3"_s}, false})); // rs:150
        QVERIFY2(rules::selectMonitors(monitors, std::nullopt, {}) == (rules::ScreenSelection{{u"HDMI-A-1"_s}, true}),
                 "unset falls back to top-left"); // rs:152
        QCOMPARE(rules::selectMonitors(monitors, std::nullopt, rules::screenFilters(u"SCREENS=, ,"_s)),
                 (rules::ScreenSelection{{u"HDMI-A-1"_s}, true})); // rs:154
        const auto vm = rules::monitorsFromJson(R"json([
            {"name":"Virtual-1","description":"Virtual display","x":0,"y":0}
        ])json");
        QVERIFY(vm.has_value()); // rs:158 unwrap
        // A copied machine-specific setting must not leave the VM without a dock.
        QVERIFY2(rules::selectMonitors(*vm, std::nullopt, filters) == (rules::ScreenSelection{{u"Virtual-1"_s}, true}),
                 "no-match VM fallback"); // rs:160
        QCOMPARE(rules::selectMonitors(monitors, u"DP-2"_s, filters),
                 (rules::ScreenSelection{{u"DP-2"_s}, false})); // rs:162
        QCOMPARE(rules::selectMonitors(monitors, u"unknown"_s, filters),
                 (rules::ScreenSelection{{}, false})); // rs:164
        const QList<Monitor> moved = {Monitor{u"DP-9"_s, monitors[3].description, 0, 0}};
        QCOMPARE(rules::selectMonitors(moved, std::nullopt, filters).names, QStringList{u"DP-9"_s}); // rs:167
        const auto ties = rules::monitorsFromJson(R"json([
            {"name":"Z","description":"Z","x":-1920,"y":-1080},
            {"name":"B","description":"B","x":0,"y":-1080},
            {"name":"A","description":"A","x":-1920,"y":-1080}
        ])json");
        QVERIFY(ties.has_value()); // rs:172 unwrap
        // All y tie; -1920 wins x, then A wins the name tie with Z.
        QCOMPARE(rules::selectMonitors(*ties, std::nullopt, {}).names, QStringList{u"A"_s}); // rs:174
        QCOMPARE(rules::selectMonitors({}, std::nullopt, filters), (rules::ScreenSelection{{}, false})); // rs:175
    }

    // Source: riley dock/tests/parity.rs:180 screens_parsing
    void screens_parsing()
    {
        // Whitespace is decoration; empty and repeated items are not extra selectors.
        QCOMPARE(rules::screenFilters(u" ICON_SIZE=48\n SCREENS = S34C65xU, , ARZOPA , S34C65xU \n"_s),
                 (QStringList{u"S34C65xU"_s, u"ARZOPA"_s})); // rs:182
        QVERIFY(rules::screenFilters(u"ICON_SIZE=48\n# SCREENS=ignored\n"_s).isEmpty()); // rs:184
        QVERIFY(rules::screenFilters(u"SCREENS= , , \n"_s).isEmpty());                   // rs:185
        QCOMPARE(rules::screenFilters(u"SCREENS=S34C65xU\nICON_SIZE=48\nSCREENS=Virtual\n"_s),
                 QStringList{u"Virtual"_s}); // rs:186
    }

    // Source: riley dock/tests/parity.rs:190 desktop_matching_isolated
    // The Rust test re-runs its own binary with private XDG directories and checks real GIO there. This port does
    // the same with its own Qt Test binary; the C++ core matches desktop files itself, without GIO.
    void desktop_matching_isolated()
    {
        if (qEnvironmentVariableIsSet("RILEY_DESKTOP_FIXTURE")) {
            // Real lookups read only these child-process XDG directories, before any cache exists.
            const auto firefox = desktop::appInfo(u"firefox"_s);
            QVERIFY(firefox.has_value());                     // rs:193 unwrap
            QCOMPARE(firefox->id, u"firefox.desktop"_s);      // rs:194
            QCOMPARE(firefox->name, u"Firefox"_s);            // rs:195
            QCOMPARE(firefox->icon, u"firefox"_s);            // rs:196
            const auto upper = desktop::appInfo(u"Firefox"_s);
            QVERIFY(upper.has_value());                       // rs:197 unwrap
            QCOMPARE(upper->id, firefox->id);                 // rs:197
            // No google-chrome.desktop exists: this result requires StartupWMClass matching.
            const auto chrome = desktop::appInfo(u"google-chrome"_s);
            QVERIFY(chrome.has_value());                                   // rs:199 unwrap
            QCOMPARE(chrome->id, u"google-chrome-stable.desktop"_s);       // rs:200
            QCOMPARE(chrome->name, u"Google Chrome"_s);                    // rs:201
            const auto shouting = desktop::appInfo(u"GOOGLE-CHROME"_s);
            QVERIFY(shouting.has_value());                    // rs:202 unwrap
            QCOMPARE(shouting->id, chrome->id);               // rs:202
            const auto terminal = desktop::appInfo(u"org.gnome.Ptyxis"_s);
            QVERIFY(terminal.has_value());                    // rs:203 unwrap
            QCOMPARE(terminal->name, u"Terminal"_s);          // rs:204
            QCOMPARE(terminal->icon, u"org.gnome.Ptyxis"_s);  // rs:205
            QVERIFY(!desktop::appInfo(u"no-such-fixture-app"_s).has_value()); // rs:206
            // A changed desktop file stays cached until the application monitor invalidates it.
            QVERIFY(writeFile(firefox->filename, "[Desktop Entry]\nType=Application\nName=Firefox Updated\n"
                                                 "Icon=firefox\nExec=firefox %u\n")); // rs:208 unwrap
            const auto cached = desktop::appInfo(u"firefox"_s);
            QVERIFY(cached.has_value());                      // rs:209 unwrap
            QCOMPARE(cached->name, u"Firefox"_s);             // rs:209
            desktop::invalidate();
            const auto updated = desktop::appInfo(u"firefox"_s);
            QVERIFY(updated.has_value());                     // rs:211 unwrap
            QCOMPARE(updated->name, u"Firefox Updated"_s);    // rs:211
            return;
        }
        QTemporaryDir scratch;
        QVERIFY(scratch.isValid()); // rs:16 create_dir_all unwrap
        const QString data = scratch.filePath(u"data"_s);
        const QString apps = data + u"/applications"_s;
        const QString bin = scratch.filePath(u"bin"_s);
        QVERIFY(QDir().mkpath(apps)); // rs:218 unwrap
        QVERIFY(QDir().mkpath(bin));  // rs:219 unwrap
        // Executable stubs make desktop validity independent of installed applications. Never launched.
        for (const QString &exe : {u"firefox"_s, u"google-chrome-stable"_s, u"ptyxis"_s})
            QVERIFY(QFile::link(u"/bin/true"_s, bin + u'/' + exe)); // rs:222 unwrap
        const std::pair<QString, QByteArray> entries[] = {
            {u"firefox"_s, "Name=Firefox\nIcon=firefox\nExec=firefox %u\n"},
            {u"google-chrome-stable"_s, "Name=Google Chrome\nIcon=google-chrome\nExec=google-chrome-stable %U\n"
                                        "StartupWMClass=google-chrome\n"},
            {u"org.gnome.Ptyxis"_s, "Name=Terminal\nIcon=org.gnome.Ptyxis\nExec=ptyxis\n"},
        };
        for (const auto &[id, body] : entries)
            QVERIFY(writeFile(apps + u'/' + id + u".desktop"_s, "[Desktop Entry]\nType=Application\n" + body)); // rs:229 unwrap
        QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
        environment.insert(u"RILEY_DESKTOP_FIXTURE"_s, u"1"_s);
        environment.insert(u"HOME"_s, scratch.path());
        environment.insert(u"XDG_DATA_HOME"_s, data);
        environment.insert(u"XDG_DATA_DIRS"_s, scratch.filePath(u"empty-data"_s));
        environment.insert(u"XDG_CONFIG_HOME"_s, scratch.filePath(u"config"_s));
        environment.insert(u"XDG_CONFIG_DIRS"_s, scratch.filePath(u"empty-config"_s));
        environment.insert(u"XDG_CACHE_HOME"_s, scratch.filePath(u"cache"_s));
        environment.insert(u"LC_ALL"_s, u"C"_s);
        environment.remove(u"LANGUAGE"_s);
        environment.insert(u"PATH"_s, bin);
        QProcess child;
        child.setProcessEnvironment(environment);
        child.setProcessChannelMode(QProcess::MergedChannels);
        child.start(QCoreApplication::applicationFilePath(), {u"desktop_matching_isolated"_s});
        QVERIFY(child.waitForStarted()); // rs:243 status().unwrap()
        const bool finished = child.waitForFinished(60000);
        const QByteArray report = "isolated desktop matching failed\n" + quoted(child.readAll());
        QVERIFY2(finished && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
                 report.constData()); // rs:244
    }
};

QTEST_GUILESS_MAIN(tst_parity)
#include "tst_parity.moc"
