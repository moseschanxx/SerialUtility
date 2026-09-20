#pragma once

#include <QObject>
#include <QList>
#include <QString>
#include <QTimer>

/**
 * A snapshot of one serial port as reported by QSerialPortInfo.
 */
struct SerialPortEntry
{
    QString portName;         ///< "COM8" / "ttyUSB0"
    QString description;      ///< "USB-Enhanced-SERIAL CH343"
    QString manufacturer;     ///< "wch.cn"
    QString serialNumber;
    QString systemLocation;   ///< "\\\\.\\COM8" / "/dev/ttyUSB0"
    quint16 vendorId = 0;
    quint16 productId = 0;
    bool hasVidPid = false;

    /// Combo-box text: "COM8 - USB-Enhanced-SERIAL CH343" (description omitted when empty).
    QString displayText() const;
    /// Multi-line tooltip with manufacturer, VID:PID (hex), serial number and location.
    QString toolTip() const;
    /// Heuristic hint for the UI: "Rockchip / USB-UART bridge", "J-Link", "Bluetooth", ...
    /// Empty when unknown. Based on VID/PID and description keywords (CH34x 1A86, CP210x
    /// 10C4, FTDI 0403, Prolific 067B, SEGGER 1366, STMicro 0483, "Bluetooth").
    QString kindHint() const;

    bool operator==(const SerialPortEntry& other) const;
    bool operator!=(const SerialPortEntry& other) const { return !(*this == other); }
};

/**
 * Application-wide poller of the serial port list.
 *
 * - Singleton: SerialPortEnumerator::instance(). start() is called once from MainWindow.
 * - Polls QSerialPortInfo::availablePorts() every pollInterval() ms (default 1000) on the
 *   GUI thread (enumeration is cheap on Windows and Linux) and emits portsChanged() only
 *   when the sorted list differs from the previous snapshot, plus portAdded()/portRemoved()
 *   per difference.
 * - Ports are sorted naturally: COM3 < COM8 < COM13 (numeric-aware), ttyS* before ttyUSB*.
 */
class SerialPortEnumerator : public QObject
{
    Q_OBJECT
public:
    static SerialPortEnumerator& instance();

    QList<SerialPortEntry> ports() const;
    QStringList portNames() const;
    bool contains(const QString& portName) const;

    int pollInterval() const;
    void setPollInterval(int ms);
    bool isRunning() const;
    void start();
    void stop();

    /// Natural sort comparator exposed for tests / other lists.
    static bool naturalLess(const QString& a, const QString& b);

public slots:
    /// Immediate rescan; emits the signals if anything changed.
    void refresh();

signals:
    void portsChanged(const QList<SerialPortEntry>& ports);
    void portAdded(const SerialPortEntry& port);
    void portRemoved(const QString& portName);

private:
    explicit SerialPortEnumerator(QObject* parent = nullptr);
    Q_DISABLE_COPY_MOVE(SerialPortEnumerator)

    QList<SerialPortEntry> m_ports;
    QTimer m_timer;
};
