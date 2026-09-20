#pragma once

#include <QObject>
#include <QSerialPort>
#include <QString>
#include <QVariantMap>
#include <QTimer>

class DeviceSimulator;

/**
 * All parameters needed to open a port. Value type, serialisable to QVariantMap for
 * QSettings (keys: port, baud, dataBits, parity, stopBits, flow, dtr, rts).
 */
struct SerialSettings
{
    QString portName;
    qint32 baudRate = 115200;
    QSerialPort::DataBits dataBits = QSerialPort::Data8;
    QSerialPort::Parity parity = QSerialPort::NoParity;
    QSerialPort::StopBits stopBits = QSerialPort::OneStop;
    QSerialPort::FlowControl flowControl = QSerialPort::NoFlowControl;
    bool dtr = true;   ///< DTR asserted after open
    bool rts = true;   ///< RTS asserted after open (ignored when hardware flow control is on)

    QVariantMap toMap() const;
    static SerialSettings fromMap(const QVariantMap& map);   ///< missing keys -> defaults

    /// Compact summary for the status bar / tab tooltip, e.g. "115200 8N1" or "9600 7E2 RTS/CTS".
    QString summary() const;

    /// Baud rates offered in the UI (ascending): 300, 1200, 2400, 4800, 9600, 19200, 38400,
    /// 57600, 115200, 230400, 460800, 500000, 921600, 1000000, 1500000, 2000000, 3000000, 4000000.
    static QList<qint32> standardBaudRates();

    /// Parity letter for summary(): N, E, O, S(pace), M(ark).
    static QChar parityLetter(QSerialPort::Parity parity);
    /// "1", "1.5", "2"
    static QString stopBitsText(QSerialPort::StopBits stopBits);
    /// "None", "RTS/CTS", "XON/XOFF"
    static QString flowControlText(QSerialPort::FlowControl flow);

    /// True when portName is one of the built-in simulated devices ("SIM:linux", ...), see
    /// core/DeviceSimulator.h. Public getter added at integration (DESIGN.md 4.6).
    bool isSimulatedPort() const;

    bool operator==(const SerialSettings& other) const;
    bool operator!=(const SerialSettings& other) const { return !(*this == other); }
};

/**
 * Owns one QSerialPort and adds what a terminal session needs on top of it:
 *
 * - Asynchronous, non-blocking I/O on the GUI thread (readyRead -> dataReceived()).
 *   write() never calls waitForBytesWritten().
 * - RX/TX byte counters.
 * - Auto-reconnect: when the device disappears (QSerialPort::ResourceError, typically a
 *   board reboot or USB re-plug) the port is closed, state becomes Reconnecting, and a
 *   timer re-tries open() every reconnectIntervalMs() while the port name is listed by
 *   QSerialPortInfo. close() (user action) always stops reconnecting.
 * - Live parameter changes: setSettings() while open applies baud/data/parity/stop/flow
 *   immediately (QSerialPort supports this) and DTR/RTS via setDtr/setRts. A changed
 *   portName while open is NOT applied until the next open().
 * - Error reporting: every QSerialPort error other than NoError/TimeoutError is forwarded
 *   through errorOccurred(message) with a human-readable message that includes the port
 *   name; PermissionError on open is reported as "port busy or access denied".
 */
class SerialConnection : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Disconnected,
        Connected,
        Reconnecting   ///< device vanished, waiting for it to come back
    };
    Q_ENUM(State)

    explicit SerialConnection(QObject* parent = nullptr);
    ~SerialConnection() override;

    SerialSettings settings() const;
    void setSettings(const SerialSettings& settings);

    State state() const;
    bool isOpen() const;                  ///< state() == Connected
    QString portName() const;             ///< settings().portName
    QString errorString() const;          ///< last error message ("" if none)

    quint64 bytesReceived() const;
    quint64 bytesSent() const;
    void resetCounters();                 ///< emits countersChanged(0, 0)

    bool autoReconnect() const;
    void setAutoReconnect(bool on);       ///< turning off while Reconnecting -> Disconnected
    int reconnectIntervalMs() const;
    void setReconnectIntervalMs(int ms);

    bool dtr() const;
    bool rts() const;

    /// Translatable display text for a state ("Disconnected", "Connected", "Reconnecting...").
    static QString stateText(State state);

public slots:
    /// Open using settings(). Emits stateChanged(Connected) on success; on failure emits
    /// errorOccurred() and stays Disconnected. Returns success. Calling while open returns true.
    bool open();
    /// Close the port and stop any reconnect attempt. Emits stateChanged(Disconnected) if changed.
    void close();
    /// Queue `data` for transmission. Returns the number of bytes accepted (data.size()) or
    /// -1 when not open (an errorOccurred() is NOT emitted for that case - callers check isOpen()).
    /// Emits dataSent(data) and countersChanged() on success.
    qint64 write(const QByteArray& data);
    void setDtr(bool on);                 ///< remembered in settings; applied immediately when open
    void setRts(bool on);
    /// Assert BREAK for `durationMs` (default 250 ms) then release, asynchronously (QTimer).
    void sendBreak(int durationMs = 250);
    /// Discard pending RX/TX buffers (QSerialPort::clear).
    void clearBuffers();

signals:
    void dataReceived(const QByteArray& data);
    void dataSent(const QByteArray& data);
    void stateChanged(SerialConnection::State state);
    void errorOccurred(const QString& message);
    void countersChanged(quint64 rx, quint64 tx);
    void pinsChanged(bool dtr, bool rts);
    /// Emitted once when the device vanished while connected (before reconnect starts).
    void portDisappeared(const QString& portName);
    /// Emitted when a reconnect attempt succeeded (after stateChanged(Connected)).
    void reconnected(const QString& portName);

private slots:
    void onReadyRead();
    void onPortError(QSerialPort::SerialPortError error);
    void tryReconnect();

private:
    void setState(State state);
    bool applyParameters();               ///< baud/data/parity/stop/flow to m_port; logs failures

    // ---- Built-in device simulator ("SIM:" pseudo-ports, DESIGN.md 4.6) -----------------
    bool openSimulator(bool quiet);                ///< open() branch for SerialSettings::isSimulatedPort()
    void closeSimulator();                         ///< detach and dispose of the simulator, if any
    void handleIncoming(const QByteArray& data);   ///< RX bookkeeping shared by QSerialPort and the simulator
    void handleDeviceVanished();                   ///< "device disappeared" path (ResourceError / simulated reboot)

    QSerialPort m_port;
    SerialSettings m_settings;
    State m_state = State::Disconnected;
    QString m_errorString;
    quint64 m_rx = 0;
    quint64 m_tx = 0;
    bool m_autoReconnect = true;
    QTimer m_reconnectTimer;
    QTimer m_breakTimer;
    DeviceSimulator* m_simulator = nullptr;   ///< QObject child while a SIM: port is open, else nullptr
};
