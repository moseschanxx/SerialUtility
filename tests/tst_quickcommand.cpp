#include <QtTest>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QSignalSpy>

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

private:
    static QuickCommand sample();
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
    // Linux 11 + U-Boot 7 + MCU 5 + Control 6 (docs/DESIGN.md 4.3)
    QCOMPARE(defs.size(), 29);

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

    // Group order as in the design document.
    QuickCommandStore store;
    QCOMPARE(store.groups(), QStringList({QStringLiteral("Linux"), QStringLiteral("U-Boot"), QStringLiteral("MCU"),
                                          QStringLiteral("Control")}));
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

    // Non-object entries inside "commands" are skipped, the rest is loaded.
    const QString mixed = dir.filePath(QStringLiteral("mixed.json"));
    {
        QFile f(mixed);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{ \"version\": 1, \"commands\": [ 42, { \"name\": \"ok\", \"command\": \"ls\" }, \"str\" ] }");
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
    QCOMPARE(doc.object().value(QLatin1String("version")).toInt(), 1);
    QVERIFY(doc.object().value(QLatin1String("commands")).isArray());
    QCOMPARE(doc.object().value(QLatin1String("commands")).toArray().size(), 1);
    QCOMPARE(doc.object().value(QLatin1String("commands")).toArray().first().toObject(), sample().toJson());
}

QTEST_GUILESS_MAIN(Tst_quickcommand)
#include "tst_quickcommand.moc"
