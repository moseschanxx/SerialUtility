#include <QtTest>
#include <QHash>
#include <QSet>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTextDocument>   // Qt::mightBeRichText

#include "core/QuickCommand.h"
#include "core/HexUtils.h"

// LineEnding::toString(Mode) is found by ADL from QTest::toString and returns a QString, so
// enum values are compared through their integer value.
#define QCOMPARE_MODE(actual, expected) QCOMPARE(static_cast<int>(actual), static_cast<int>(expected))

class Tst_quickcommand : public QObject
{
    Q_OBJECT
private slots:
    // QuickCommand
    void payloadText();
    void payloadLineEndings();
    void payloadEscapes();
    void payloadHex();
    void payloadErrors();
    void jsonRoundTrip();
    void jsonDefaults();
    void jsonUnknownLineEnding();
    void equality();

    // defaults()
    void defaultsMatchDesign();
    void defaultsAllParse();
    void defaultsStartWithNodeGroup();
    void nodeDefaultsOrderAndPayloads();
    void nodeDefaultsTooltips();

    // QuickCommandStore
    void storeStartsWithDefaults();
    void storeGroups();
    void storeGroupsWhitespaceIsGeneral();
    void storeSetCommandsEmitsChanged();
    void storeLoadMissingFile();
    void storeSaveAndLoad();
    void storeLoadMalformed();
    void storeLoadWrongShape();
    void storeImportExport();
    void storeImportErrors();
    void storeExportError();
    void storeResetToDefaults();
    void storeFileFormat();

    // File version 2: the Node group is added once to a version-1 file
    void storeFileVersionIsTwo();
    void storeMigratesVersion1();
    void storeMigratesFileWithoutVersionKey();
    void storeNoDoubleMigration();
    void storeVersion1WithNodeCommandIsNotTouched();
    void storeVersion2IsLoadedAsIs();
    void storeMigratedFlagEdgeCases();
    void storeImportDoesNotMigrate();

private:
    static QuickCommand sample();
    /// Write `json` to `dir`/`name`; returns the path, or an empty string when the file cannot be written.
    static QString writeJson(const QTemporaryDir& dir, const QString& name, const QByteArray& json);
};

QuickCommand Tst_quickcommand::sample()
{
    QuickCommand qc;
    qc.name = QStringLiteral("Reboot board");
    qc.command = QStringLiteral("reboot -f");
    qc.group = QStringLiteral("Linux");
    qc.lineEnding = LineEnding::Mode::LF;
    qc.hex = false;
    qc.escapes = true;
    qc.shortcut = QStringLiteral("Ctrl+1");
    qc.tooltip = QString::fromUtf8("Force reboot 重启");
    return qc;
}

QString Tst_quickcommand::writeJson(const QTemporaryDir& dir, const QString& name, const QByteArray& json)
{
    const QString path = dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(json) != json.size()) {
        return QString();
    }
    return path;
}

// ---------------------------------------------------------------------------------------

void Tst_quickcommand::payloadText()
{
    QuickCommand qc;
    qc.command = QStringLiteral("uname -a");
    QString error = QStringLiteral("stale");
    QCOMPARE(qc.payload(&error), QByteArray("uname -a\r"));
    QVERIFY(error.isEmpty());

    qc.command = QString::fromUtf8("echo 中文");
    qc.lineEnding = LineEnding::Mode::None;
    QCOMPARE(qc.payload(), QString::fromUtf8("echo 中文").toUtf8());

    // Without escapes enabled a backslash is literal.
    qc.command = QStringLiteral("a\\nb");
    QCOMPARE(qc.payload(), QByteArray("a\\nb"));
}

void Tst_quickcommand::payloadLineEndings()
{
    QuickCommand qc;
    qc.command = QStringLiteral("AT");
    qc.lineEnding = LineEnding::Mode::CRLF;
    QCOMPARE(qc.payload(), QByteArray("AT\r\n"));
    qc.lineEnding = LineEnding::Mode::LF;
    QCOMPARE(qc.payload(), QByteArray("AT\n"));
    qc.lineEnding = LineEnding::Mode::CR;
    QCOMPARE(qc.payload(), QByteArray("AT\r"));
    qc.lineEnding = LineEnding::Mode::None;
    QCOMPARE(qc.payload(), QByteArray("AT"));
    // An empty command still sends the line ending (a bare Enter).
    qc.command.clear();
    qc.lineEnding = LineEnding::Mode::CR;
    QCOMPARE(qc.payload(), QByteArray("\r"));
}

void Tst_quickcommand::payloadEscapes()
{
    QuickCommand qc;
    qc.escapes = true;
    qc.command = QStringLiteral("\\x03\\e[A\\ttab");
    qc.lineEnding = LineEnding::Mode::None;
    QCOMPARE(qc.payload(), QByteArray::fromHex("031b5b41") + QByteArray("\ttab"));
    qc.lineEnding = LineEnding::Mode::CRLF;
    QCOMPARE(qc.payload(), QByteArray::fromHex("031b5b41") + QByteArray("\ttab\r\n"));
}

void Tst_quickcommand::payloadHex()
{
    QuickCommand qc;
    qc.hex = true;
    qc.command = QStringLiteral("03");
    qc.lineEnding = LineEnding::Mode::CRLF;   // ignored for hex
    QCOMPARE(qc.payload(), QByteArray(1, '\x03'));

    qc.command = QStringLiteral("0xAA, 0x55 ff");
    qc.escapes = true;   // ignored for hex
    QCOMPARE(qc.payload(), QByteArray::fromHex("aa55ff"));

    qc.command.clear();
    QString error;
    QVERIFY(qc.payload(&error).isEmpty());
    QVERIFY(error.isEmpty());
}

void Tst_quickcommand::payloadErrors()
{
    QuickCommand qc;
    qc.hex = true;
    qc.command = QStringLiteral("abc");
    QString error;
    QVERIFY(qc.payload(&error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(qc.payload(nullptr).isEmpty());

    QuickCommand esc;
    esc.escapes = true;
    esc.command = QStringLiteral("bad\\q");
    error.clear();
    QVERIFY(esc.payload(&error).isEmpty());
    QVERIFY(error.contains(QStringLiteral("\\q")));

    // Successful call clears a stale error.
    esc.command = QStringLiteral("ok");
    QCOMPARE(esc.payload(&error), QByteArray("ok\r"));
    QVERIFY(error.isEmpty());
}

void Tst_quickcommand::jsonRoundTrip()
{
    const QuickCommand original = sample();
    const QJsonObject json = original.toJson();
    QCOMPARE(json.value(QLatin1String("name")).toString(), original.name);
    QCOMPARE(json.value(QLatin1String("command")).toString(), original.command);
    QCOMPARE(json.value(QLatin1String("group")).toString(), original.group);
    QCOMPARE(json.value(QLatin1String("lineEnding")).toString(), QStringLiteral("lf"));
    QCOMPARE(json.value(QLatin1String("hex")).toBool(), false);
    QCOMPARE(json.value(QLatin1String("escapes")).toBool(), true);
    QCOMPARE(json.value(QLatin1String("shortcut")).toString(), original.shortcut);
    QCOMPARE(json.value(QLatin1String("tooltip")).toString(), original.tooltip);

    const QuickCommand restored = QuickCommand::fromJson(json);
    QVERIFY(restored == original);
    QVERIFY(!(restored != original));

    // Through a real document (text) as well.
    const QByteArray text = QJsonDocument(json).toJson();
    const QuickCommand again = QuickCommand::fromJson(QJsonDocument::fromJson(text).object());
    QCOMPARE(again, original);

    QuickCommand hexCmd;
    hexCmd.name = QStringLiteral("Ctrl+C");
    hexCmd.command = QStringLiteral("03");
    hexCmd.hex = true;
    hexCmd.lineEnding = LineEnding::Mode::None;
    QCOMPARE(QuickCommand::fromJson(hexCmd.toJson()), hexCmd);
}

void Tst_quickcommand::jsonDefaults()
{
    const QuickCommand qc = QuickCommand::fromJson(QJsonObject());
    QVERIFY(qc.name.isEmpty());
    QVERIFY(qc.command.isEmpty());
    QVERIFY(qc.group.isEmpty());
    QCOMPARE_MODE(qc.lineEnding, LineEnding::Mode::CR);
    QCOMPARE(qc.hex, false);
    QCOMPARE(qc.escapes, false);
    QVERIFY(qc.shortcut.isEmpty());
    QVERIFY(qc.tooltip.isEmpty());
    QCOMPARE(qc, QuickCommand());

    // Partial object: only the given fields change.
    QJsonObject partial;
    partial.insert(QLatin1String("name"), QStringLiteral("n"));
    partial.insert(QLatin1String("hex"), true);
    const QuickCommand p = QuickCommand::fromJson(partial);
    QCOMPARE(p.name, QStringLiteral("n"));
    QVERIFY(p.hex);
    QCOMPARE_MODE(p.lineEnding, LineEnding::Mode::CR);
}

void Tst_quickcommand::jsonUnknownLineEnding()
{
    QJsonObject obj;
    obj.insert(QLatin1String("lineEnding"), QStringLiteral("weird"));
    QCOMPARE_MODE(QuickCommand::fromJson(obj).lineEnding, LineEnding::Mode::CR);
    obj.insert(QLatin1String("lineEnding"), 3);   // wrong type
    QCOMPARE_MODE(QuickCommand::fromJson(obj).lineEnding, LineEnding::Mode::CR);
    obj.insert(QLatin1String("lineEnding"), QStringLiteral("none"));
    QCOMPARE_MODE(QuickCommand::fromJson(obj).lineEnding, LineEnding::Mode::None);
}

void Tst_quickcommand::equality()
{
    QuickCommand a = sample();
    QuickCommand b = sample();
    QVERIFY(a == b);
    b.tooltip += QLatin1Char('!');
    QVERIFY(a != b);
    b = sample();
    b.hex = true;
    QVERIFY(a != b);
    b = sample();
    b.lineEnding = LineEnding::Mode::CRLF;
    QVERIFY(a != b);
}

// ---------------------------------------------------------------------------------------

void Tst_quickcommand::defaultsMatchDesign()
{
    const QList<QuickCommand> defs = QuickCommandStore::defaults();
    // Node 15 (v0.5) + Linux 11 + U-Boot 7 + MCU 5 + Control 6 (docs/DESIGN.md 4.3)
    QCOMPARE(defs.size(), 44);

    auto find = [&defs](const QString& group, const QString& name) -> QuickCommand {
        for (const QuickCommand& qc : defs) {
            if (qc.group == group && qc.name == name) {
                return qc;
            }
        }
        return QuickCommand();
    };
    auto countGroup = [&defs](const QString& group) {
        int n = 0;
        for (const QuickCommand& qc : defs) {
            if (qc.group == group) {
                ++n;
            }
        }
        return n;
    };

    QCOMPARE(countGroup(QStringLiteral("Node")), 15);
    QCOMPARE(countGroup(QStringLiteral("Linux")), 11);
    QCOMPARE(countGroup(QStringLiteral("U-Boot")), 7);
    QCOMPARE(countGroup(QStringLiteral("MCU")), 5);
    QCOMPARE(countGroup(QStringLiteral("Control")), 6);

    QCOMPARE(find(QStringLiteral("Linux"), QStringLiteral("cpuinfo")).command, QStringLiteral("cat /proc/cpuinfo"));
    QCOMPARE(find(QStringLiteral("Linux"), QStringLiteral("dmesg tail")).command, QStringLiteral("dmesg | tail -n 50"));
    QCOMPARE(find(QStringLiteral("Linux"), QStringLiteral("top")).command, QStringLiteral("top -n 1"));
    QCOMPARE(find(QStringLiteral("Linux"), QStringLiteral("ls /dev")).command,
             QStringLiteral("ls -l /dev/tty* /dev/video* 2>/dev/null"));
    QCOMPARE(find(QStringLiteral("Linux"), QStringLiteral("reboot")).payload(), QByteArray("reboot\r"));

    QCOMPARE(find(QStringLiteral("U-Boot"), QStringLiteral("mmc info")).payload(), QByteArray("mmc info\r"));
    QCOMPARE_MODE(find(QStringLiteral("U-Boot"), QStringLiteral("reset")).lineEnding, LineEnding::Mode::CR);

    const QuickCommand at = find(QStringLiteral("MCU"), QStringLiteral("AT"));
    QCOMPARE_MODE(at.lineEnding, LineEnding::Mode::CRLF);
    QCOMPARE(at.payload(), QByteArray("AT\r\n"));
    QCOMPARE(find(QStringLiteral("MCU"), QStringLiteral("AT+GMR")).payload(), QByteArray("AT+GMR\r\n"));
    QCOMPARE(find(QStringLiteral("MCU"), QStringLiteral("help")).payload(), QByteArray("help\r"));

    const QuickCommand ctrlC = find(QStringLiteral("Control"), QStringLiteral("Ctrl+C"));
    QVERIFY(ctrlC.hex);
    QCOMPARE(ctrlC.payload(), QByteArray(1, '\x03'));
    QCOMPARE(find(QStringLiteral("Control"), QStringLiteral("Ctrl+D")).payload(), QByteArray(1, '\x04'));
    QCOMPARE(find(QStringLiteral("Control"), QStringLiteral("Ctrl+Z")).payload(), QByteArray(1, '\x1a'));
    QCOMPARE(find(QStringLiteral("Control"), QStringLiteral("ESC")).payload(), QByteArray(1, '\x1b'));
    QCOMPARE(find(QStringLiteral("Control"), QStringLiteral("Enter")).payload(), QByteArray(1, '\r'));
    QCOMPARE(find(QStringLiteral("Control"), QStringLiteral("Ctrl+L")).payload(), QByteArray(1, '\x0c'));

    // The Node commands the design document names.
    QCOMPARE(find(QStringLiteral("Node"), QStringLiteral("reboot loader")).payload(), QByteArray("reboot loader\r"));
    QCOMPARE(find(QStringLiteral("Node"), QStringLiteral("rfmcu console")).payload(), QByteArray("rfmcu console\r"));
    QCOMPARE(find(QStringLiteral("Node"), QStringLiteral("~.")).payload(), QByteArray("~.\r"));

    // Group order as in the design document: Node first (v0.5).
    QuickCommandStore store;
    QCOMPARE(store.groups(), QStringList({QStringLiteral("Node"), QStringLiteral("Linux"), QStringLiteral("U-Boot"),
                                          QStringLiteral("MCU"), QStringLiteral("Control")}));
}

void Tst_quickcommand::defaultsAllParse()
{
    const QList<QuickCommand> defs = QuickCommandStore::defaults();
    for (const QuickCommand& qc : defs) {
        QString error;
        const QByteArray bytes = qc.payload(&error);
        QVERIFY2(error.isEmpty(), qPrintable(qc.name + QStringLiteral(": ") + error));
        QVERIFY2(!bytes.isEmpty(), qPrintable(qc.name));
        QVERIFY2(!qc.name.isEmpty(), "every default has a name");
        QVERIFY2(!qc.group.isEmpty(), qPrintable(qc.name));
        // Every default survives a JSON round trip unchanged.
        QCOMPARE(QuickCommand::fromJson(qc.toJson()), qc);
    }
}

void Tst_quickcommand::defaultsStartWithNodeGroup()
{
    const QList<QuickCommand> defs = QuickCommandStore::defaults();
    const QList<QuickCommand> node = QuickCommandStore::nodeDefaults();
    QCOMPARE(QuickCommandStore::nodeGroupName(), QStringLiteral("Node"));
    QCOMPARE(node.size(), 15);
    QVERIFY(defs.size() > node.size());

    // The very first default is "reboot loader" and the whole Node group comes before every other group.
    QCOMPARE(defs.first().name, QStringLiteral("reboot loader"));
    QCOMPARE(defs.first().group, QuickCommandStore::nodeGroupName());
    for (int i = 0; i < node.size(); ++i) {
        QCOMPARE(defs.at(i), node.at(i));
    }
    // ... and after it the groups of 0.4 follow unchanged, in their old order.
    QCOMPARE(defs.at(node.size()).group, QStringLiteral("Linux"));
    QCOMPARE(defs.at(node.size()).name, QStringLiteral("uname -a"));
    QCOMPARE(defs.last().name, QStringLiteral("Ctrl+L"));
    QCOMPARE(defs.last().group, QStringLiteral("Control"));
}

void Tst_quickcommand::nodeDefaultsOrderAndPayloads()
{
    const QStringList expected = {
        QStringLiteral("reboot loader"), QStringLiteral("rfmcu console"), QStringLiteral("help"),
        QStringLiteral("version"),       QStringLiteral("sys"),           QStringLiteral("stack"),
        QStringLiteral("uart"),          QStringLiteral("power"),         QStringLiteral("bat"),
        QStringLiteral("rx status"),     QStringLiteral("rx dump on"),    QStringLiteral("rx dump off"),
        QStringLiteral("tx status"),     QStringLiteral("bootloader"),    QStringLiteral("~."),
    };
    const QList<QuickCommand> node = QuickCommandStore::nodeDefaults();
    QCOMPARE(node.size(), expected.size());
    for (int i = 0; i < node.size(); ++i) {
        const QuickCommand& qc = node.at(i);
        QCOMPARE(qc.name, expected.at(i));
        QCOMPARE(qc.command, expected.at(i));   // button label and text are the same everywhere in the group
        QCOMPARE(qc.group, QStringLiteral("Node"));
        QCOMPARE_MODE(qc.lineEnding, LineEnding::Mode::CR);
        QVERIFY2(!qc.hex && !qc.escapes, qPrintable(qc.name));
        QVERIFY2(qc.shortcut.isEmpty(), qPrintable(qc.name));
        QString error;
        QCOMPARE(qc.payload(&error), expected.at(i).toUtf8() + QByteArray("\r"));
        QVERIFY(error.isEmpty());
    }
    // Each button is one command; no duplicates in the group.
    QCOMPARE(QSet<QString>(expected.begin(), expected.end()).size(), expected.size());
}

void Tst_quickcommand::nodeDefaultsTooltips()
{
    const QList<QuickCommand> node = QuickCommandStore::nodeDefaults();
    QHash<QString, QString> tip;
    for (const QuickCommand& qc : node) {
        QVERIFY2(!qc.tooltip.isEmpty(), qPrintable(qc.name));
        QVERIFY2(!qc.tooltip.contains(QLatin1Char('\n')), qPrintable(qc.name));   // one line each
        // The tooltips contain "<rate>" / "<mV>": none of them may be taken for HTML by the tooltip.
        QVERIFY2(!Qt::mightBeRichText(qc.tooltip), qPrintable(qc.name));
        tip.insert(qc.name, qc.tooltip);
    }
    // The SoC-side and the leave-the-console buttons, word for word.
    QCOMPARE(tip.value(QStringLiteral("reboot loader")),
             QStringLiteral("SoC console: reboot into the USB download (Loader) mode for flashing"));
    QCOMPARE(tip.value(QStringLiteral("rfmcu console")),
             QStringLiteral("SoC console: open the CC1310 CLI through the SoC; ~. leaves it"));
    QCOMPARE(tip.value(QStringLiteral("bootloader")),
             QStringLiteral("CC1310: writes an update request and reboots into the UART updater"));
    QCOMPARE(tip.value(QStringLiteral("~.")), QStringLiteral("leave rfmcu console"));
    // The CC1310 commands that take arguments carry their usage line (CC1310 CLI reference, Part B).
    QVERIFY(tip.value(QStringLiteral("power")).contains(QStringLiteral("power | power off [delay_ms] | power soc on|off")));
    QVERIFY(tip.value(QStringLiteral("bat")).contains(QStringLiteral("bat | bat sim <mV>|off | bat charge on|off")));
    QVERIFY(tip.value(QStringLiteral("rx status")).contains(QStringLiteral("rx status | rx dump on|off")));
    QVERIFY(tip.value(QStringLiteral("uart")).contains(QStringLiteral("uart | uart baud <rate>")));
    QVERIFY(tip.value(QStringLiteral("sys")).contains(QStringLiteral("sys | sys wdt_test")));
}

// ---------------------------------------------------------------------------------------

void Tst_quickcommand::storeStartsWithDefaults()
{
    QuickCommandStore store;
    QCOMPARE(store.commands(), QuickCommandStore::defaults());
}

void Tst_quickcommand::storeGroups()
{
    QuickCommandStore store;
    QuickCommand a;
    a.name = QStringLiteral("a");
    a.group = QStringLiteral("Zeta");
    QuickCommand b;
    b.name = QStringLiteral("b");   // empty group -> "General"
    QuickCommand c;
    c.name = QStringLiteral("c");
    c.group = QStringLiteral("Alpha");
    QuickCommand d;
    d.name = QStringLiteral("d");
    d.group = QStringLiteral("Zeta");
    store.setCommands({a, b, c, d});
    QCOMPARE(store.groups(), QStringList({QStringLiteral("Zeta"), QStringLiteral("General"), QStringLiteral("Alpha")}));
    store.setCommands({});
    QVERIFY(store.groups().isEmpty());
}

void Tst_quickcommand::storeGroupsWhitespaceIsGeneral()
{
    QCOMPARE(QuickCommandStore::generalGroupKey(), QStringLiteral("General"));

    QuickCommandStore store;
    QuickCommand a;
    a.name = QStringLiteral("a");
    a.group = QStringLiteral("  	 "); // whitespace-only -> the general group
    QuickCommand b;
    b.name = QStringLiteral("b");
    b.group = QStringLiteral("Alpha");
    QuickCommand c;
    c.name = QStringLiteral("c"); // empty -> the general group as well
    store.setCommands({a, b, c});
    QCOMPARE(store.groups(), QStringList({QStringLiteral("General"), QStringLiteral("Alpha")}));
    QCOMPARE(QuickCommandStore::effectiveGroup(a), QuickCommandStore::generalGroupKey());
    QCOMPARE(QuickCommandStore::effectiveGroup(b), QStringLiteral("Alpha"));
    QCOMPARE(QuickCommandStore::effectiveGroup(c), QuickCommandStore::generalGroupKey());
    // Without a translator the display name is the key itself; other groups pass through.
    QCOMPARE(QuickCommandStore::groupDisplayName(QuickCommandStore::generalGroupKey()), QStringLiteral("General"));
    QCOMPARE(QuickCommandStore::groupDisplayName(QStringLiteral("Alpha")), QStringLiteral("Alpha"));
}

void Tst_quickcommand::storeSetCommandsEmitsChanged()
{
    QuickCommandStore store;
    QSignalSpy spy(&store, &QuickCommandStore::changed);
    store.setCommands(QuickCommandStore::defaults());   // identical -> no signal
    QCOMPARE(spy.count(), 0);
    QList<QuickCommand> list = store.commands();
    list.removeLast();
    store.setCommands(list);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(store.commands(), list);
    store.setCommands(list);
    QCOMPARE(spy.count(), 1);
}

void Tst_quickcommand::storeLoadMissingFile()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QuickCommandStore store;
    store.setCommands({});
    QSignalSpy spy(&store, &QuickCommandStore::changed);
    QVERIFY(store.load(dir.filePath(QStringLiteral("missing.json"))));
    QCOMPARE(store.commands(), QuickCommandStore::defaults());
    QCOMPARE(spy.count(), 1);
    QVERIFY(!QFile::exists(dir.filePath(QStringLiteral("missing.json"))));
}

void Tst_quickcommand::storeSaveAndLoad()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("sub/dir/quick_commands.json"));

    QuickCommandStore store;
    QList<QuickCommand> list = {sample()};
    QuickCommand hexCmd;
    hexCmd.name = QStringLiteral("Break");
    hexCmd.command = QStringLiteral("00 00");
    hexCmd.hex = true;
    hexCmd.group = QStringLiteral("Control");
    hexCmd.lineEnding = LineEnding::Mode::None;
    list.append(hexCmd);
    store.setCommands(list);
    QVERIFY(store.save(path));   // creates sub/dir
    QVERIFY(QFile::exists(path));

    QuickCommandStore other;
    QSignalSpy spy(&other, &QuickCommandStore::changed);
    QVERIFY(other.load(path));
    QCOMPARE(other.commands(), list);
    QCOMPARE(spy.count(), 1);

    // Saving and reloading the defaults is lossless too.
    store.resetToDefaults();
    QVERIFY(store.save(path));
    QVERIFY(other.load(path));
    QCOMPARE(other.commands(), QuickCommandStore::defaults());

    // An empty list round-trips as an empty list (not the defaults).
    store.setCommands({});
    QVERIFY(store.save(path));
    QVERIFY(other.load(path));
    QVERIFY(other.commands().isEmpty());
}

void Tst_quickcommand::storeLoadMalformed()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("bad.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("{ \"version\": 1, \"commands\": [ { \"name\": ");
    file.close();

    QuickCommandStore store;
    const QList<QuickCommand> before = {sample()};
    store.setCommands(before);
    QSignalSpy spy(&store, &QuickCommandStore::changed);
    QVERIFY(!store.load(path));
    QCOMPARE(store.commands(), before);
    QCOMPARE(spy.count(), 0);
}

void Tst_quickcommand::storeLoadWrongShape()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QuickCommandStore store;
    const QList<QuickCommand> before = {sample()};
    store.setCommands(before);

    // Valid JSON but not our document shape.
    const QString arrayPath = dir.filePath(QStringLiteral("array.json"));
    {
        QFile f(arrayPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("[1, 2, 3]");
    }
    QVERIFY(!store.load(arrayPath));
    QCOMPARE(store.commands(), before);

    const QString noCommands = dir.filePath(QStringLiteral("nocommands.json"));
    {
        QFile f(noCommands);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{ \"version\": 1 }");
    }
    QVERIFY(!store.load(noCommands));
    QCOMPARE(store.commands(), before);

    // Non-object entries inside "commands" are skipped, the rest is loaded. (Version 2: a version-1
    // file would get the Node group in front, see storeMigratesVersion1.)
    const QString mixed = dir.filePath(QStringLiteral("mixed.json"));
    {
        QFile f(mixed);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{ \"version\": 2, \"commands\": [ 42, { \"name\": \"ok\", \"command\": \"ls\" }, \"str\" ] }");
    }
    QVERIFY(store.load(mixed));
    QCOMPARE(store.commands().size(), 1);
    QCOMPARE(store.commands().first().name, QStringLiteral("ok"));
    QCOMPARE(store.commands().first().payload(), QByteArray("ls\r"));
}

void Tst_quickcommand::storeImportExport()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("export/shared.json"));

    QuickCommandStore source;
    QList<QuickCommand> list = QuickCommandStore::defaults();
    list.prepend(sample());
    source.setCommands(list);
    QString error = QStringLiteral("stale");
    QVERIFY(source.exportToFile(path, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(QFile::exists(path));

    QuickCommandStore target;
    QSignalSpy spy(&target, &QuickCommandStore::changed);
    error = QStringLiteral("stale");
    QVERIFY(target.importFromFile(path, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(target.commands(), list);
    QCOMPARE(spy.count(), 1);

    // Export and save produce the same document, so load() reads an export as well.
    QuickCommandStore viaLoad;
    QVERIFY(viaLoad.load(path));
    QCOMPARE(viaLoad.commands(), list);
}

void Tst_quickcommand::storeImportErrors()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QuickCommandStore store;
    const QList<QuickCommand> before = store.commands();

    QString error;
    QVERIFY(!store.importFromFile(dir.filePath(QStringLiteral("nope.json")), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.commands(), before);

    const QString bad = dir.filePath(QStringLiteral("bad.json"));
    {
        QFile f(bad);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not json at all");
    }
    error.clear();
    QVERIFY(!store.importFromFile(bad, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(store.commands(), before);
    // Null error pointer accepted.
    QVERIFY(!store.importFromFile(bad, nullptr));
}

void Tst_quickcommand::storeExportError()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A regular file where a directory would be needed makes the export fail.
    const QString blocker = dir.filePath(QStringLiteral("blocker"));
    {
        QFile f(blocker);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
    }
    QuickCommandStore store;
    QString error;
    QVERIFY(!store.exportToFile(blocker + QStringLiteral("/inner/out.json"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!store.save(blocker + QStringLiteral("/inner/out.json")));
}

void Tst_quickcommand::storeResetToDefaults()
{
    QuickCommandStore store;
    store.setCommands({sample()});
    QSignalSpy spy(&store, &QuickCommandStore::changed);
    store.resetToDefaults();
    QCOMPARE(store.commands(), QuickCommandStore::defaults());
    QCOMPARE(spy.count(), 1);
    store.resetToDefaults();
    QCOMPARE(spy.count(), 1);
}

void Tst_quickcommand::storeFileFormat()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("format.json"));
    QuickCommandStore store;
    store.setCommands({sample()});
    QVERIFY(store.save(path));

    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QVERIFY(doc.isObject());
    QCOMPARE(doc.object().value(QLatin1String("version")).toInt(), 2);   // v0.5: the Node group exists
    QVERIFY(doc.object().value(QLatin1String("commands")).isArray());
    QCOMPARE(doc.object().value(QLatin1String("commands")).toArray().size(), 1);
    QCOMPARE(doc.object().value(QLatin1String("commands")).toArray().first().toObject(), sample().toJson());
}

// ---------------------------------------------------------------------------------------
// File version 2 (v0.5): the Node group is added once to a version-1 file

void Tst_quickcommand::storeFileVersionIsTwo()
{
    QCOMPARE(QuickCommandStore::fileVersion(), 2);

    // Every file written by the store, whatever it holds, is version 2: save() and the export alike.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QuickCommandStore store;
    for (const QList<QuickCommand>& list : {QuickCommandStore::defaults(), QList<QuickCommand>{}}) {
        store.setCommands(list);
        const QString saved = dir.filePath(QStringLiteral("saved.json"));
        const QString exported = dir.filePath(QStringLiteral("exported.json"));
        QVERIFY(store.save(saved));
        QVERIFY(store.exportToFile(exported));
        for (const QString& path : {saved, exported}) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value(QLatin1String("version")).toInt(), 2);
        }
    }
}

void Tst_quickcommand::storeMigratesVersion1()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A file as 0.4 wrote it: version 1, the user's own groups, no Node group, one command of the general group.
    const QString path = writeJson(dir, QStringLiteral("quick_commands.json"),
                                   "{\n"
                                   "    \"version\": 1,\n"
                                   "    \"commands\": [\n"
                                   "        { \"name\": \"uname -a\", \"command\": \"uname -a\", \"group\": \"Linux\",\n"
                                   "          \"lineEnding\": \"cr\", \"hex\": false, \"escapes\": false,\n"
                                   "          \"shortcut\": \"Ctrl+1\", \"tooltip\": \"kernel\" },\n"
                                   "        { \"name\": \"mine\", \"command\": \"echo hi\", \"group\": \"\",\n"
                                   "          \"lineEnding\": \"lf\", \"hex\": false, \"escapes\": false,\n"
                                   "          \"shortcut\": \"\", \"tooltip\": \"\" }\n"
                                   "    ]\n"
                                   "}\n");
    QVERIFY(!path.isEmpty());
    const QByteArray before = [&path] {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }();

    QuickCommandStore store;
    QSignalSpy spy(&store, &QuickCommandStore::changed);
    bool migrated = false;
    QVERIFY(store.load(path, &migrated));
    QVERIFY(migrated);
    QCOMPARE(spy.count(), 1);   // one change for the caller, not one for the load and one for the migration

    QList<QuickCommand> expected = QuickCommandStore::nodeDefaults();
    QuickCommand uname;
    uname.name = QStringLiteral("uname -a");
    uname.command = QStringLiteral("uname -a");
    uname.group = QStringLiteral("Linux");
    uname.shortcut = QStringLiteral("Ctrl+1");
    uname.tooltip = QStringLiteral("kernel");
    QuickCommand mine;
    mine.name = QStringLiteral("mine");
    mine.command = QStringLiteral("echo hi");
    mine.lineEnding = LineEnding::Mode::LF;
    expected.append(uname);
    expected.append(mine);
    QCOMPARE(store.commands(), expected);   // Node in front, the user's commands untouched and in order
    QCOMPARE(store.commands().size(), 17);
    QCOMPARE(store.commands().first().name, QStringLiteral("reboot loader"));
    QCOMPARE(store.groups(), QStringList({QStringLiteral("Node"), QStringLiteral("Linux"), QStringLiteral("General")}));

    // load() itself never rewrites the file; saving is the caller's decision.
    QFile f(path);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), before);
}

void Tst_quickcommand::storeMigratesFileWithoutVersionKey()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // No "version" at all counts as version 1.
    const QString path = writeJson(dir, QStringLiteral("old.json"),
                                   "{ \"commands\": [ { \"name\": \"ok\", \"command\": \"ls\", \"group\": \"Linux\" } ] }");
    QVERIFY(!path.isEmpty());
    QuickCommandStore store;
    bool migrated = false;
    QVERIFY(store.load(path, &migrated));
    QVERIFY(migrated);
    QCOMPARE(store.commands().size(), 16);
    QCOMPARE(store.commands().first().name, QStringLiteral("reboot loader"));
    QCOMPARE(store.commands().last().name, QStringLiteral("ok"));

    // A version that is not a number counts as 1 as well.
    const QString odd = writeJson(dir, QStringLiteral("odd.json"),
                                  "{ \"version\": \"two\", \"commands\": [ { \"name\": \"ok\", \"command\": \"ls\" } ] }");
    QVERIFY(!odd.isEmpty());
    migrated = false;
    QVERIFY(store.load(odd, &migrated));
    QVERIFY(migrated);
    QCOMPARE(store.commands().size(), 16);
}

void Tst_quickcommand::storeNoDoubleMigration()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = writeJson(dir, QStringLiteral("quick_commands.json"),
                                   "{ \"version\": 1, \"commands\": [ { \"name\": \"ps\", \"command\": \"ps\", \"group\": \"Linux\" } ] }");
    QVERIFY(!path.isEmpty());

    QuickCommandStore store;
    bool migrated = false;
    QVERIFY(store.load(path, &migrated));
    QVERIFY(migrated);
    const QList<QuickCommand> afterFirstLoad = store.commands();
    QCOMPARE(afterFirstLoad.size(), 16);
    QVERIFY(store.save(path));   // what MainWindow does after a migration: the file becomes version 2

    // Every later start reads version 2: loaded as is, nothing prepended again.
    for (int round = 0; round < 3; ++round) {
        QuickCommandStore next;
        migrated = true;   // load() must reset it
        QVERIFY(next.load(path, &migrated));
        QVERIFY2(!migrated, "the second load must not migrate again");
        QCOMPARE(next.commands(), afterFirstLoad);
        QCOMPARE(next.commands().size(), 16);
        QVERIFY(next.save(path));
    }

    // A user who removes the whole Node group keeps it removed: the file is version 2 now.
    QList<QuickCommand> withoutNode;
    for (const QuickCommand& qc : afterFirstLoad) {
        if (QuickCommandStore::effectiveGroup(qc) != QuickCommandStore::nodeGroupName()) {
            withoutNode.append(qc);
        }
    }
    QCOMPARE(withoutNode.size(), 1);
    store.setCommands(withoutNode);
    QVERIFY(store.save(path));
    QuickCommandStore reloaded;
    migrated = true;
    QVERIFY(reloaded.load(path, &migrated));
    QVERIFY(!migrated);
    QCOMPARE(reloaded.commands(), withoutNode);
}

void Tst_quickcommand::storeVersion1WithNodeCommandIsNotTouched()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A hand-made version-1 file that already has a command in the Node group: nothing is added.
    const QString path = writeJson(dir, QStringLiteral("handmade.json"),
                                   "{ \"version\": 1, \"commands\": [\n"
                                   "  { \"name\": \"ps\", \"command\": \"ps\", \"group\": \"Linux\" },\n"
                                   "  { \"name\": \"my node\", \"command\": \"rfmcu cmd version\", \"group\": \"Node\" }\n"
                                   "] }");
    QVERIFY(!path.isEmpty());
    QuickCommandStore store;
    QSignalSpy spy(&store, &QuickCommandStore::changed);
    bool migrated = true;
    QVERIFY(store.load(path, &migrated));
    QVERIFY(!migrated);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(store.commands().size(), 2);
    QCOMPARE(store.commands().at(0).name, QStringLiteral("ps"));
    QCOMPARE(store.commands().at(1).name, QStringLiteral("my node"));
    QCOMPARE(store.groups(), QStringList({QStringLiteral("Linux"), QStringLiteral("Node")}));
}

void Tst_quickcommand::storeVersion2IsLoadedAsIs()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // Version 2 without a Node group (the user deleted it): as is.
    const QString v2 = writeJson(dir, QStringLiteral("v2.json"),
                                 "{ \"version\": 2, \"commands\": [ { \"name\": \"ps\", \"command\": \"ps\", \"group\": \"Linux\" } ] }");
    QVERIFY(!v2.isEmpty());
    QuickCommandStore store;
    bool migrated = true;
    QVERIFY(store.load(v2, &migrated));
    QVERIFY(!migrated);
    QCOMPARE(store.commands().size(), 1);
    QCOMPARE(store.commands().first().name, QStringLiteral("ps"));

    // So is a file of a later release (version 3) and an empty version-2 list.
    const QString v3 = writeJson(dir, QStringLiteral("v3.json"),
                                 "{ \"version\": 3, \"commands\": [ { \"name\": \"ps\", \"command\": \"ps\" } ] }");
    QVERIFY(!v3.isEmpty());
    QVERIFY(store.load(v3, &migrated));
    QVERIFY(!migrated);
    QCOMPARE(store.commands().size(), 1);

    const QString empty = writeJson(dir, QStringLiteral("empty.json"), "{ \"version\": 2, \"commands\": [] }");
    QVERIFY(!empty.isEmpty());
    QVERIFY(store.load(empty, &migrated));
    QVERIFY(!migrated);
    QVERIFY(store.commands().isEmpty());
}

void Tst_quickcommand::storeMigratedFlagEdgeCases()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QuickCommandStore store;

    // A null pointer is accepted for a version-1 file (the default argument of load()).
    const QString v1 = writeJson(dir, QStringLiteral("v1.json"),
                                 "{ \"version\": 1, \"commands\": [ { \"name\": \"ps\", \"command\": \"ps\" } ] }");
    QVERIFY(!v1.isEmpty());
    QVERIFY(store.load(v1));
    QCOMPARE(store.commands().size(), 16);
    QVERIFY(store.load(v1, nullptr));
    QCOMPARE(store.commands().size(), 16);   // a reload starts from the file again, not from the loaded list

    // A missing file gives the defaults and is not a migration; a malformed one fails and is not either.
    bool migrated = true;
    QVERIFY(store.load(dir.filePath(QStringLiteral("missing.json")), &migrated));
    QVERIFY(!migrated);
    QCOMPARE(store.commands(), QuickCommandStore::defaults());

    const QString bad = writeJson(dir, QStringLiteral("bad.json"), "{ \"version\": 1, \"commands\": [ { ");
    QVERIFY(!bad.isEmpty());
    migrated = true;
    const QList<QuickCommand> before = store.commands();
    QVERIFY(!store.load(bad, &migrated));
    QVERIFY(!migrated);
    QCOMPARE(store.commands(), before);

    // An empty version-1 list is a version-1 file without Node commands: it gets the Node group.
    const QString empty = writeJson(dir, QStringLiteral("empty.json"), "{ \"version\": 1, \"commands\": [] }");
    QVERIFY(!empty.isEmpty());
    migrated = false;
    QVERIFY(store.load(empty, &migrated));
    QVERIFY(migrated);
    QCOMPARE(store.commands(), QuickCommandStore::nodeDefaults());
}

void Tst_quickcommand::storeImportDoesNotMigrate()
{
    // importFromFile() takes exactly what the file holds (a set shared between machines); only load(),
    // the startup path, adds the Node group to an old file.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString v1 = writeJson(dir, QStringLiteral("shared_v1.json"),
                                 "{ \"version\": 1, \"commands\": [ { \"name\": \"ps\", \"command\": \"ps\", \"group\": \"Linux\" } ] }");
    QVERIFY(!v1.isEmpty());
    QuickCommandStore store;
    QString error;
    QVERIFY(store.importFromFile(v1, &error));
    QVERIFY(error.isEmpty());
    QCOMPARE(store.commands().size(), 1);
    QCOMPARE(store.commands().first().name, QStringLiteral("ps"));
}

QTEST_GUILESS_MAIN(Tst_quickcommand)
#include "tst_quickcommand.moc"
