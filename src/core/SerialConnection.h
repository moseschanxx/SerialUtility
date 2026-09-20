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

    /// Inclusive range accepted by every baud editor (ConnectionBar, Preferences) and by fromMap().
    static constexpr qint32 kMinBaudRate = 50;
    static constexpr qint32 kMaxBaudRate = 10000000;
    /// True when baud lies in [kMinBaudRate, kMaxBaudRate].
    static constexpr bool isValidBaudRate(qint32 baud) { return baud >= kMinBaudRate && baud <= kMaxBaudRate; }

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
 * - Auto-reconnect: when the device disappears (QSerialPort::ResourceError / PermissionError /
 *   DeviceNotFoundError while connected, a Read/Write/UnknownError while the port is no longer
 *   listed by QSerialPortInfo - Windows drivers report USB removal as ERROR_GEN_FAILURE, which
 *   Qt maps to UnknownError and after which QSerialPort stops reading - or
 *   SerialPortEnumerator::portRemoved for the open port) the port is closed, state becomes
 *   Reconnecting, and a timer re-tries open() every reconnectIntervalMs() while the port name
 *   is listed by SerialPortEnumerator's snapshot (or by QSerialPortInfo directly when the
 *   enumerator is not running). close() (user action) always stops reconnecting. When the
 *   port is back but the stored line parameters are rejected by the driver, that is reported
 *   once through errorOccurred() and the timer keeps polling.
 * - Live parameter changes: setSettings() while open applies baud/data/parity/stop/flow
 *   immediately (QSerialPort supports this) and DTR/RTS via setDtr/setRts. A changed
 *   portName while open is NOT applied until the next open(). A parameter the driver rejects
 *   is reported through errorOccurred() and NOT stored: settings() keeps the last accepted
 *   value for that field; callers re-read settings() after a rejection.
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
    void setReconnectIntervalMs(int ms);   ///< clamped 200..60000; a no-op change never restarts a pending reconnect

    bool dtr() const;
    bool rts() const;

    /// Bytes accepted by write() that QSerialPort has not yet handed to the driver
    /// (QSerialPort::bytesToWrite()); 0 for a simulated device or while not open. Together
    /// with txBytesWritten() this lets a paced sender (SendFileDialog) apply backpressure
    /// instead of growing the unbounded write buffer. Public getter/signal added at integration.
    qint64 pendingTxBytes() const;

    /// Translatable display text for a state ("Disconnected", "Connected", "Reconnecting...").
    static QString stateText(State state);

public slots:
    /// Open using settings(). Emits stateChanged(Connected) on success (plus reconnected() if the
    /// state was Reconnecting); on failure emits errorOccurred() and the state is unchanged
    /// (Disconnected, or Reconnecting with the retry timer still polling). Returns success.
    /// Calling while open returns true. An explicit open() while Reconnecting is a forced,
    /// non-quiet attempt.
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
    /// Returns true when BREAK was asserted (or the session is a simulated device, which
    /// ignores BREAK); returns false when not open (no errorOccurred() emitted, callers check
    /// isOpen()) or when the driver rejects BREAK (errorOccurred() emitted with
    /// "Cannot send BREAK on <port>: <reason>").
    bool sendBreak(int durationMs = 250);
    /// Discard pending RX/TX buffers (QSerialPort::clear).
    void clearBuffers();

signals:
    void dataReceived(const QByteArray& data);
    void dataSent(const QByteArray& data);
    void stateChanged(SerialConnection::State state);
    void errorOccurred(const QString& message);
    void countersChanged(quint64 rx, quint64 tx);
    void pinsChanged(bool dtr, bool rts);
    /// QSerialPort::bytesWritten forwarded: `bytes` left the write buffer for the driver. Never
    /// emitted for a simulated device (its write() is synchronous, pendingTxBytes() stays 0).
    void txBytesWritten(qint64 bytes);
    /// Emitted once when the device vanished while connected (after errorOccurred(), before
    /// reconnect starts).
    void portDisappeared(const QString& portName);
    /// Emitted when the port was reopened after portDisappeared(), whether by the reconnect
    /// timer or by an explicit open() (after stateChanged(Connected)).
    void reconnected(const QString& portName);

private slots:
    void onReadyRead();
    void onPortError(QSerialPort::SerialPortError error);
    void tryReconnect();

private:
    void setState(State state);
    bool applyParameters();               ///< baud/data/parity/stop/flow to m_port; logs failures
    /// Shared body of open()/tryReconnect(); quiet = periodic reconnect attempt: failures are
    /// logged at debug level only (no errorOccurred(), except once for rejected line parameters)
    /// and a simulated device is not re-powered.
    bool openPort(bool quiet);
    /// settings().portName currently enumerated: DeviceSimulator::isPresent() for SIM: ports,
    /// otherwise SerialPortEnumerator's snapshot when `useEnumeratorSnapshot` and the enumerator
    /// is running (up to one poll interval stale, but free), else QSerialPortInfo directly.
    bool isPortListed(bool useEnumeratorSnapshot) const;

    // ---- Built-in device simulator ("SIM:" pseudo-ports, DESIGN.md 4.6) -----------------
    bool openSimulator(bool quiet);                ///< openPort() branch for SerialSettings::isSimulatedPort()
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
    bool m_reconnectParamErrorReported = false;   ///< "port is back but parameters rejected" reported once per outage
    QTimer m_reconnectTimer;
    QTimer m_breakTimer;
    DeviceSimulator* m_simulator = nullptr;   ///< QObject child while a SIM: port is open, else nullptr
};
