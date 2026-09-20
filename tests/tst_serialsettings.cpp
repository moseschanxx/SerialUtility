#include <QtTest>
#include <QSignalSpy>
#include <algorithm>

#include "core/SerialConnection.h"
#include "core/SerialPortEnumerator.h"

class Tst_serialsettings : public QObject
{
    Q_OBJECT
private slots:
    // SerialSettings
    void defaults();
    void toMapKeys();
    void mapRoundTrip();
    void fromMapMissingKeys();
    void fromMapInvalidValues();
    void summaryText();
    void standardBaudRates();
    void helperTexts();
    void equality();

    // SerialPortEntry
    void entryDisplayText();
    void entryToolTip();
    void entryKindHint();
    void entryEquality();

    // SerialPortEnumerator
    void naturalLessBasics();
    void naturalLessOrdering();
    void naturalLessSortsList();
    void enumeratorSingleton();

    // SerialConnection without hardware
    void connectionInitialState();
    void connectionOpenWithoutPort();
    void connectionOpenMissingPort();
    void connectionWriteWhenClosed();
    void connectionCounters();
    void connectionReconnectSettings();
    void connectionPins();
    void connectionStateText();

private:
    static SerialSettings custom();
};

SerialSettings Tst_serialsettings::custom()
{
    SerialSettings s;
    s.portName = QStringLiteral("COM8");
    s.baudRate = 1500000;
    s.dataBits = QSerialPort::Data7;
    s.parity = QSerialPort::EvenParity;
    s.stopBits = QSerialPort::TwoStop;
    s.flowControl = QSerialPort::HardwareControl;
    s.dtr = false;
    s.rts = false;
    return s;
}

// ---------------------------------------------------------------------------------------

void Tst_serialsettings::defaults()
{
    const SerialSettings s;
    QVERIFY(s.portName.isEmpty());
    QCOMPARE(s.baudRate, 115200);
    QCOMPARE(s.dataBits, QSerialPort::Data8);
    QCOMPARE(s.parity, QSerialPort::NoParity);
    QCOMPARE(s.stopBits, QSerialPort::OneStop);
    QCOMPARE(s.flowControl, QSerialPort::NoFlowControl);
    QVERIFY(s.dtr);
    QVERIFY(s.rts);
    QCOMPARE(s.summary(), QStringLiteral("115200 8N1"));
}

void Tst_serialsettings::toMapKeys()
{
    const QVariantMap map = custom().toMap();
    const QStringList expectedKeys = {QStringLiteral("baud"),     QStringLiteral("dataBits"), QStringLiteral("dtr"),
                                      QStringLiteral("flow"),     QStringLiteral("parity"),   QStringLiteral("port"),
                                      QStringLiteral("rts"),      QStringLiteral("stopBits")};
    QCOMPARE(map.keys(), expectedKeys);   // QVariantMap keys are sorted
    QCOMPARE(map.value(QStringLiteral("port")).toString(), QStringLiteral("COM8"));
    QCOMPARE(map.value(QStringLiteral("baud")).toInt(), 1500000);
    QCOMPARE(map.value(QStringLiteral("dataBits")).toInt(), 7);
    QCOMPARE(map.value(QStringLiteral("parity")).toInt(), static_cast<int>(QSerialPort::EvenParity));
    QCOMPARE(map.value(QStringLiteral("stopBits")).toInt(), static_cast<int>(QSerialPort::TwoStop));
    QCOMPARE(map.value(QStringLiteral("flow")).toInt(), static_cast<int>(QSerialPort::HardwareControl));
    QCOMPARE(map.value(QStringLiteral("dtr")).toBool(), false);
    QCOMPARE(map.value(QStringLiteral("rts")).toBool(), false);
}

void Tst_serialsettings::mapRoundTrip()
{
    const SerialSettings a = custom();
    QCOMPARE(SerialSettings::fromMap(a.toMap()), a);
    const SerialSettings d;
    QCOMPARE(SerialSettings::fromMap(d.toMap()), d);

    SerialSettings s;
    s.portName = QStringLiteral("/dev/ttyUSB0");
    s.baudRate = 9600;
    s.stopBits = QSerialPort::OneAndHalfStop;
    s.parity = QSerialPort::MarkParity;
    s.flowControl = QSerialPort::SoftwareControl;
    QCOMPARE(SerialSettings::fromMap(s.toMap()), s);

    // Values stored as strings (as an INI backend would hand them back) are accepted.
    QVariantMap stringy;
    stringy.insert(QStringLiteral("baud"), QStringLiteral("921600"));
    stringy.insert(QStringLiteral("dataBits"), QStringLiteral("7"));
    stringy.insert(QStringLiteral("dtr"), QStringLiteral("false"));
    const SerialSettings fromStrings = SerialSettings::fromMap(stringy);
    QCOMPARE(fromStrings.baudRate, 921600);
    QCOMPARE(fromStrings.dataBits, QSerialPort::Data7);
    QCOMPARE(fromStrings.dtr, false);
}

void Tst_serialsettings::fromMapMissingKeys()
{
    QCOMPARE(SerialSettings::fromMap(QVariantMap()), SerialSettings());

    QVariantMap partial;
    partial.insert(QStringLiteral("port"), QStringLiteral("COM3"));
    partial.insert(QStringLiteral("baud"), 9600);
    const SerialSettings s = SerialSettings::fromMap(partial);
    QCOMPARE(s.portName, QStringLiteral("COM3"));
    QCOMPARE(s.baudRate, 9600);
    QCOMPARE(s.dataBits, QSerialPort::Data8);
    QCOMPARE(s.parity, QSerialPort::NoParity);
    QCOMPARE(s.stopBits, QSerialPort::OneStop);
    QCOMPARE(s.flowControl, QSerialPort::NoFlowControl);
    QVERIFY(s.dtr);
    QVERIFY(s.rts);
}

void Tst_serialsettings::fromMapInvalidValues()
{
    QVariantMap bad;
    bad.insert(QStringLiteral("baud"), -5);
    bad.insert(QStringLiteral("dataBits"), 42);
    bad.insert(QStringLiteral("parity"), QStringLiteral("even"));
    bad.insert(QStringLiteral("stopBits"), 99);
    bad.insert(QStringLiteral("flow"), 7);
    const SerialSettings s = SerialSettings::fromMap(bad);
    QCOMPARE(s.baudRate, 115200);
    QCOMPARE(s.dataBits, QSerialPort::Data8);
    QCOMPARE(s.parity, QSerialPort::NoParity);
    QCOMPARE(s.stopBits, QSerialPort::OneStop);
    QCOMPARE(s.flowControl, QSerialPort::NoFlowControl);

    QVariantMap zeroBaud;
    zeroBaud.insert(QStringLiteral("baud"), 0);
    QCOMPARE(SerialSettings::fromMap(zeroBaud).baudRate, 115200);
}

void Tst_serialsettings::summaryText()
{
    SerialSettings s;
    QCOMPARE(s.summary(), QStringLiteral("115200 8N1"));
    s.baudRate = 1500000;
    QCOMPARE(s.summary(), QStringLiteral("1500000 8N1"));

    s.baudRate = 9600;
    s.dataBits = QSerialPort::Data7;
    s.parity = QSerialPort::EvenParity;
    s.stopBits = QSerialPort::TwoStop;
    s.flowControl = QSerialPort::HardwareControl;
    QCOMPARE(s.summary(), QStringLiteral("9600 7E2 RTS/CTS"));

    s.flowControl = QSerialPort::SoftwareControl;
    s.parity = QSerialPort::OddParity;
    s.stopBits = QSerialPort::OneAndHalfStop;
    s.dataBits = QSerialPort::Data5;
    QCOMPARE(s.summary(), QStringLiteral("9600 5O1.5 XON/XOFF"));

    s.flowControl = QSerialPort::NoFlowControl;
    s.parity = QSerialPort::SpaceParity;
    s.stopBits = QSerialPort::OneStop;
    s.dataBits = QSerialPort::Data8;
    QCOMPARE(s.summary(), QStringLiteral("9600 8S1"));
    s.parity = QSerialPort::MarkParity;
    QCOMPARE(s.summary(), QStringLiteral("9600 8M1"));
    // The port name and DTR/RTS are not part of the summary.
    s.portName = QStringLiteral("COM9");
    s.dtr = false;
    QCOMPARE(s.summary(), QStringLiteral("9600 8M1"));
}

void Tst_serialsettings::standardBaudRates()
{
    const QList<qint32> rates = SerialSettings::standardBaudRates();
    QCOMPARE(rates.size(), 18);
    QCOMPARE(rates.first(), 300);
    QCOMPARE(rates.last(), 4000000);
    QVERIFY(std::is_sorted(rates.cbegin(), rates.cend()));
    QVERIFY(std::adjacent_find(rates.cbegin(), rates.cend()) == rates.cend());   // no duplicates
    for (qint32 must : {9600, 115200, 921600, 1500000}) {
        QVERIFY2(rates.contains(must), qPrintable(QString::number(must)));
    }
    QCOMPARE(rates, QList<qint32>({300, 1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 500000,
                                   921600, 1000000, 1500000, 2000000, 3000000, 4000000}));
}

void Tst_serialsettings::helperTexts()
{
    QCOMPARE(SerialSettings::parityLetter(QSerialPort::NoParity), QLatin1Char('N'));
    QCOMPARE(SerialSettings::parityLetter(QSerialPort::EvenParity), QLatin1Char('E'));
    QCOMPARE(SerialSettings::parityLetter(QSerialPort::OddParity), QLatin1Char('O'));
    QCOMPARE(SerialSettings::parityLetter(QSerialPort::SpaceParity), QLatin1Char('S'));
    QCOMPARE(SerialSettings::parityLetter(QSerialPort::MarkParity), QLatin1Char('M'));

    QCOMPARE(SerialSettings::stopBitsText(QSerialPort::OneStop), QStringLiteral("1"));
    QCOMPARE(SerialSettings::stopBitsText(QSerialPort::OneAndHalfStop), QStringLiteral("1.5"));
    QCOMPARE(SerialSettings::stopBitsText(QSerialPort::TwoStop), QStringLiteral("2"));

    QCOMPARE(SerialSettings::flowControlText(QSerialPort::NoFlowControl), QStringLiteral("None"));
    QCOMPARE(SerialSettings::flowControlText(QSerialPort::HardwareControl), QStringLiteral("RTS/CTS"));
    QCOMPARE(SerialSettings::flowControlText(QSerialPort::SoftwareControl), QStringLiteral("XON/XOFF"));
}

void Tst_serialsettings::equality()
{
    SerialSettings a = custom();
    SerialSettings b = custom();
    QVERIFY(a == b);
    QVERIFY(!(a != b));
    b.rts = true;
    QVERIFY(a != b);
    b = custom();
    b.portName = QStringLiteral("COM9");
    QVERIFY(a != b);
    b = custom();
    b.baudRate = 115200;
    QVERIFY(a != b);
}

// ---------------------------------------------------------------------------------------

void Tst_serialsettings::entryDisplayText()
{
    SerialPortEntry e;
    e.portName = QStringLiteral("COM8");
    QCOMPARE(e.displayText(), QStringLiteral("COM8"));
    e.description = QStringLiteral("USB-Enhanced-SERIAL CH343");
    QCOMPARE(e.displayText(), QStringLiteral("COM8 - USB-Enhanced-SERIAL CH343"));
    e.description = QStringLiteral("   ");
    QCOMPARE(e.displayText(), QStringLiteral("COM8"));
}

void Tst_serialsettings::entryToolTip()
{
    SerialPortEntry e;
    e.portName = QStringLiteral("COM8");
    e.description = QStringLiteral("USB-Enhanced-SERIAL CH343");
    e.manufacturer = QStringLiteral("wch.cn");
    e.serialNumber = QStringLiteral("5A6B7C");
    e.systemLocation = QStringLiteral("\\\\.\\COM8");
    e.vendorId = 0x1A86;
    e.productId = 0x55D3;
    e.hasVidPid = true;
    const QString tip = e.toolTip();
    QVERIFY(tip.contains(QStringLiteral("COM8")));
    QVERIFY(tip.contains(QStringLiteral("USB-Enhanced-SERIAL CH343")));
    QVERIFY(tip.contains(QStringLiteral("wch.cn")));
    QVERIFY(tip.contains(QStringLiteral("1A86:55D3")));
    QVERIFY(tip.contains(QStringLiteral("5A6B7C")));
    QVERIFY(tip.contains(QStringLiteral("\\\\.\\COM8")));
    QVERIFY(tip.contains(QLatin1Char('\n')));   // multi-line
    QVERIFY(tip.contains(QStringLiteral("CH34x")));

    // Without VID/PID the hex pair is omitted; leading zeros are kept otherwise.
    SerialPortEntry bare;
    bare.portName = QStringLiteral("ttyS0");
    const QString bareTip = bare.toolTip();
    QVERIFY(bareTip.startsWith(QStringLiteral("ttyS0")));
    QVERIFY(!bareTip.contains(QLatin1Char(':')));

    SerialPortEntry ftdi;
    ftdi.portName = QStringLiteral("COM3");
    ftdi.vendorId = 0x0403;
    ftdi.productId = 0x6001;
    ftdi.hasVidPid = true;
    QVERIFY(ftdi.toolTip().contains(QStringLiteral("0403:6001")));
}

void Tst_serialsettings::entryKindHint()
{
    auto withVid = [](quint16 vid) {
        SerialPortEntry e;
        e.portName = QStringLiteral("COMx");
        e.vendorId = vid;
        e.productId = 1;
        e.hasVidPid = true;
        return e;
    };
    QVERIFY(withVid(0x1A86).kindHint().contains(QStringLiteral("CH34x")));
    QVERIFY(withVid(0x10C4).kindHint().contains(QStringLiteral("CP210x")));
    QVERIFY(withVid(0x0403).kindHint().contains(QStringLiteral("FTDI")));
    QVERIFY(withVid(0x067B).kindHint().contains(QStringLiteral("Prolific")));
    QVERIFY(withVid(0x1366).kindHint().contains(QStringLiteral("J-Link")));
    QVERIFY(withVid(0x0483).kindHint().contains(QStringLiteral("STMicro")));
    QVERIFY(withVid(0x2207).kindHint().contains(QStringLiteral("Rockchip")));
    QVERIFY(withVid(0x303A).kindHint().contains(QStringLiteral("Espressif")));
    QVERIFY(withVid(0x2E8A).kindHint().contains(QStringLiteral("RP2040")));
    QVERIFY(withVid(0x1234).kindHint().isEmpty());

    // The VID is only trusted when hasVidPid is set.
    SerialPortEntry noVid = withVid(0x1A86);
    noVid.hasVidPid = false;
    QVERIFY(noVid.kindHint().isEmpty());

    SerialPortEntry bt;
    bt.portName = QStringLiteral("COM5");
    bt.description = QStringLiteral("Standard Serial over Bluetooth link");
    QVERIFY(bt.kindHint().contains(QStringLiteral("Bluetooth")));
    bt.description = QStringLiteral("standard serial over BLUETOOTH link");
    QVERIFY(bt.kindHint().contains(QStringLiteral("Bluetooth")));

    // A known VID wins over the description.
    SerialPortEntry both = withVid(0x1366);
    both.description = QStringLiteral("Bluetooth-ish");
    QVERIFY(both.kindHint().contains(QStringLiteral("J-Link")));

    SerialPortEntry unknown;
    unknown.portName = QStringLiteral("COM1");
    unknown.description = QStringLiteral("Communications Port");
    QVERIFY(unknown.kindHint().isEmpty());
}

void Tst_serialsettings::entryEquality()
{
    SerialPortEntry a;
    a.portName = QStringLiteral("COM8");
    a.description = QStringLiteral("d");
    a.vendorId = 1;
    a.productId = 2;
    a.hasVidPid = true;
    SerialPortEntry b = a;
    QVERIFY(a == b);
    QVERIFY(!(a != b));
    b.productId = 3;
    QVERIFY(a != b);
    b = a;
    b.hasVidPid = false;
    QVERIFY(a != b);
    b = a;
    b.serialNumber = QStringLiteral("x");
    QVERIFY(a != b);
}

// ---------------------------------------------------------------------------------------

void Tst_serialsettings::naturalLessBasics()
{
    using E = SerialPortEnumerator;
    QVERIFY(E::naturalLess(QStringLiteral("COM3"), QStringLiteral("COM13")));
    QVERIFY(!E::naturalLess(QStringLiteral("COM13"), QStringLiteral("COM3")));
    QVERIFY(E::naturalLess(QStringLiteral("COM8"), QStringLiteral("COM10")));
    QVERIFY(E::naturalLess(QStringLiteral("COM9"), QStringLiteral("COM10")));
    QVERIFY(E::naturalLess(QStringLiteral("ttyS0"), QStringLiteral("ttyUSB0")));
    QVERIFY(!E::naturalLess(QStringLiteral("ttyUSB0"), QStringLiteral("ttyS0")));
    QVERIFY(E::naturalLess(QStringLiteral("ttyUSB1"), QStringLiteral("ttyUSB2")));
    QVERIFY(E::naturalLess(QStringLiteral("ttyUSB2"), QStringLiteral("ttyUSB10")));
    QVERIFY(E::naturalLess(QStringLiteral("ttyACM0"), QStringLiteral("ttyS0")));

    // Irreflexive and antisymmetric.
    QVERIFY(!E::naturalLess(QStringLiteral("COM3"), QStringLiteral("COM3")));
    QVERIFY(!E::naturalLess(QString(), QString()));
    // Prefix sorts first; empty string first of all.
    QVERIFY(E::naturalLess(QStringLiteral("COM1"), QStringLiteral("COM1A")));
    QVERIFY(E::naturalLess(QString(), QStringLiteral("COM1")));
    // Leading zeros compare numerically equal, then the longer spelling after.
    QVERIFY(E::naturalLess(QStringLiteral("COM7"), QStringLiteral("COM007")) !=
            E::naturalLess(QStringLiteral("COM007"), QStringLiteral("COM7")));
    QVERIFY(E::naturalLess(QStringLiteral("COM007"), QStringLiteral("COM8")));
    // Case-insensitive on the alphabetic part.
    QVERIFY(E::naturalLess(QStringLiteral("com3"), QStringLiteral("COM13")));
    QVERIFY(E::naturalLess(QStringLiteral("COM3"), QStringLiteral("com13")));
    // Big numbers (no int overflow on long digit runs).
    QVERIFY(E::naturalLess(QStringLiteral("p99999999999999999998"), QStringLiteral("p99999999999999999999")));
    QVERIFY(E::naturalLess(QStringLiteral("p5"), QStringLiteral("p99999999999999999999")));
}

void Tst_serialsettings::naturalLessOrdering()
{
    // Strict weak ordering: a<b and b<c imply a<c for a representative set.
    const QStringList chain = {QStringLiteral("COM1"),    QStringLiteral("COM2"),     QStringLiteral("COM10"),
                               QStringLiteral("COM11"),   QStringLiteral("COM100"),   QStringLiteral("ttyACM0"),
                               QStringLiteral("ttyS0"),   QStringLiteral("ttyS1"),    QStringLiteral("ttyS10"),
                               QStringLiteral("ttyUSB0"), QStringLiteral("ttyUSB1")};
    for (int i = 0; i < chain.size(); ++i) {
        for (int j = 0; j < chain.size(); ++j) {
            const bool lessIJ = SerialPortEnumerator::naturalLess(chain.at(i), chain.at(j));
            QCOMPARE(lessIJ, i < j);
        }
    }
}

void Tst_serialsettings::naturalLessSortsList()
{
    QStringList names = {QStringLiteral("COM13"), QStringLiteral("COM3"),   QStringLiteral("COM8"),
                         QStringLiteral("COM1"),  QStringLiteral("ttyUSB0"), QStringLiteral("ttyS0"),
                         QStringLiteral("COM10"), QStringLiteral("ttyS1")};
    std::sort(names.begin(), names.end(), SerialPortEnumerator::naturalLess);
    QCOMPARE(names, QStringList({QStringLiteral("COM1"), QStringLiteral("COM3"), QStringLiteral("COM8"),
                                 QStringLiteral("COM10"), QStringLiteral("COM13"), QStringLiteral("ttyS0"),
                                 QStringLiteral("ttyS1"), QStringLiteral("ttyUSB0")}));
}

void Tst_serialsettings::enumeratorSingleton()
{
    SerialPortEnumerator& e = SerialPortEnumerator::instance();
    QCOMPARE(&e, &SerialPortEnumerator::instance());
    QCOMPARE(e.pollInterval(), 1000);
    QVERIFY(!e.isRunning());
    e.setPollInterval(2500);
    QCOMPARE(e.pollInterval(), 2500);
    e.setPollInterval(1000);

    // refresh() without hardware assumptions: ports() and portNames() agree and are sorted.
    e.refresh();
    const QList<SerialPortEntry> ports = e.ports();
    const QStringList names = e.portNames();
    QCOMPARE(names.size(), ports.size());
    for (int i = 0; i < ports.size(); ++i) {
        QCOMPARE(names.at(i), ports.at(i).portName);
        QVERIFY(e.contains(names.at(i)));
        if (i > 0) {
            QVERIFY(!SerialPortEnumerator::naturalLess(names.at(i), names.at(i - 1)));
        }
    }
    QVERIFY(!e.contains(QStringLiteral("SU_NO_SUCH_PORT_0")));

    // A second refresh with an unchanged list is silent.
    QSignalSpy changedSpy(&e, &SerialPortEnumerator::portsChanged);
    e.refresh();
    QCOMPARE(changedSpy.count(), 0);

    e.start();
    QVERIFY(e.isRunning());
    e.start();   // idempotent
    QVERIFY(e.isRunning());
    e.stop();
    QVERIFY(!e.isRunning());
}

// ---------------------------------------------------------------------------------------

void Tst_serialsettings::connectionInitialState()
{
    SerialConnection c;
    QCOMPARE(c.state(), SerialConnection::State::Disconnected);
    QVERIFY(!c.isOpen());
    QVERIFY(c.portName().isEmpty());
    QVERIFY(c.errorString().isEmpty());
    QCOMPARE(c.bytesReceived(), quint64(0));
    QCOMPARE(c.bytesSent(), quint64(0));
    QVERIFY(c.autoReconnect());
    QCOMPARE(c.reconnectIntervalMs(), 1000);
    QVERIFY(c.dtr());
    QVERIFY(c.rts());
    QCOMPARE(c.settings(), SerialSettings());

    const SerialSettings s = custom();
    c.setSettings(s);
    QCOMPARE(c.settings(), s);
    QCOMPARE(c.portName(), QStringLiteral("COM8"));
    QVERIFY(!c.dtr());
    QVERIFY(!c.rts());
    // Closing an already closed connection is harmless and silent.
    QSignalSpy stateSpy(&c, &SerialConnection::stateChanged);
    c.close();
    QCOMPARE(stateSpy.count(), 0);
    QCOMPARE(c.state(), SerialConnection::State::Disconnected);
}

void Tst_serialsettings::connectionOpenWithoutPort()
{
    SerialConnection c;
    QSignalSpy errorSpy(&c, &SerialConnection::errorOccurred);
    QSignalSpy stateSpy(&c, &SerialConnection::stateChanged);
    QVERIFY(!c.open());
    QCOMPARE(c.state(), SerialConnection::State::Disconnected);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(stateSpy.count(), 0);
    QVERIFY(!c.errorString().isEmpty());
    QCOMPARE(errorSpy.first().first().toString(), c.errorString());
}

void Tst_serialsettings::connectionOpenMissingPort()
{
    SerialConnection c;
    SerialSettings s;
    s.portName = QStringLiteral("SU_NO_SUCH_PORT_42");
    c.setSettings(s);
    QSignalSpy errorSpy(&c, &SerialConnection::errorOccurred);
    QSignalSpy stateSpy(&c, &SerialConnection::stateChanged);
    QVERIFY(!c.open());
    QVERIFY(!c.isOpen());
    QCOMPARE(c.state(), SerialConnection::State::Disconnected);
    QCOMPARE(errorSpy.count(), 1);
    QCOMPARE(stateSpy.count(), 0);
    const QString message = errorSpy.first().first().toString();
    QVERIFY2(message.contains(QStringLiteral("SU_NO_SUCH_PORT_42")), qPrintable(message));
    QCOMPARE(c.errorString(), message);
    // The failure does not start a reconnect loop: the state stays Disconnected.
    QTest::qWait(50);
    QCOMPARE(c.state(), SerialConnection::State::Disconnected);
}

void Tst_serialsettings::connectionWriteWhenClosed()
{
    SerialConnection c;
    QSignalSpy errorSpy(&c, &SerialConnection::errorOccurred);
    QSignalSpy sentSpy(&c, &SerialConnection::dataSent);
    QCOMPARE(c.write(QByteArray("hello")), qint64(-1));
    QCOMPARE(errorSpy.count(), 0);   // callers check isOpen(); no error is emitted
    QCOMPARE(sentSpy.count(), 0);
    QCOMPARE(c.bytesSent(), quint64(0));
    // Pin/break/clear helpers are no-ops on a closed port.
    QSignalSpy pinSpy(&c, &SerialConnection::pinsChanged);
    c.sendBreak(10);
    c.clearBuffers();
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(pinSpy.count(), 0);
}

void Tst_serialsettings::connectionCounters()
{
    SerialConnection c;
    QSignalSpy counterSpy(&c, &SerialConnection::countersChanged);
    c.resetCounters();
    QCOMPARE(counterSpy.count(), 1);
    QCOMPARE(counterSpy.first().at(0).toULongLong(), 0ULL);
    QCOMPARE(counterSpy.first().at(1).toULongLong(), 0ULL);
    QCOMPARE(c.bytesReceived(), quint64(0));
    QCOMPARE(c.bytesSent(), quint64(0));
}

void Tst_serialsettings::connectionReconnectSettings()
{
    SerialConnection c;
    c.setReconnectIntervalMs(50);
    QCOMPARE(c.reconnectIntervalMs(), 200);   // clamped to the minimum
    c.setReconnectIntervalMs(100000);
    QCOMPARE(c.reconnectIntervalMs(), 60000);
    c.setReconnectIntervalMs(2500);
    QCOMPARE(c.reconnectIntervalMs(), 2500);

    QSignalSpy stateSpy(&c, &SerialConnection::stateChanged);
    c.setAutoReconnect(false);
    QVERIFY(!c.autoReconnect());
    QCOMPARE(stateSpy.count(), 0);   // not reconnecting -> nothing to give up
    c.setAutoReconnect(true);
    QVERIFY(c.autoReconnect());
}

void Tst_serialsettings::connectionPins()
{
    SerialConnection c;
    QSignalSpy pinSpy(&c, &SerialConnection::pinsChanged);
    QSignalSpy errorSpy(&c, &SerialConnection::errorOccurred);
    c.setDtr(false);
    QCOMPARE(pinSpy.count(), 1);
    QCOMPARE(pinSpy.last().at(0).toBool(), false);
    QCOMPARE(pinSpy.last().at(1).toBool(), true);
    QVERIFY(!c.dtr());
    QVERIFY(!c.settings().dtr);
    c.setRts(false);
    QCOMPARE(pinSpy.count(), 2);
    QCOMPARE(pinSpy.last().at(0).toBool(), false);
    QCOMPARE(pinSpy.last().at(1).toBool(), false);
    QVERIFY(!c.rts());
    QVERIFY(!c.settings().rts);
    c.setDtr(true);
    QVERIFY(c.dtr());
    QCOMPARE(errorSpy.count(), 0);   // remembered only while closed, no error
}

void Tst_serialsettings::connectionStateText()
{
    QCOMPARE(SerialConnection::stateText(SerialConnection::State::Disconnected), QStringLiteral("Disconnected"));
    QCOMPARE(SerialConnection::stateText(SerialConnection::State::Connected), QStringLiteral("Connected"));
    QCOMPARE(SerialConnection::stateText(SerialConnection::State::Reconnecting), QStringLiteral("Reconnecting..."));
}

QTEST_GUILESS_MAIN(Tst_serialsettings)
#include "tst_serialsettings.moc"
