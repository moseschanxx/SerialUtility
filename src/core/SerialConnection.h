#pragma once

#include <QObject>
#include <QSerialPort>
#include <QString>
#include <QVariantMap>
#include <QTimer>

#include "core/Transport.h"

class DeviceSimulator;

/**
 * All parameters needed to open a port. Value type, serialisable to QVariantMap for
 * QSettings (keys: port, baud, dataBits, parity, stopBits, flow, dtr, rts, autoBaud).
 *
 * autoBaud (v0.4) means "the baud rate is detected automatically" (the "Auto" item of the
 * ConnectionBar's baud combo): baudRate is then the effective rate - the last one detected, or
 * the one to open the port with before the detection runs - and every consumer of baudRate
 * (SerialConnection, DeviceSimulator, the log header) keeps using it as a plain rate. The flag
 * only changes summary() ("Auto (115200) 8N1") and what SessionWidget does after open().
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
    bool autoBaud = false;   ///< "Auto": baudRate is the effective (detected / starting) rate, see above

    QVariantMap toMap() const;
    static SerialSettings fromMap(const QVariantMap& map);   ///< missing keys -> defaults

    /// Compact summary for the status bar / tab tooltip, e.g. "115200 8N1" or "9600 7E2 RTS/CTS";
    /// "Auto (115200) 8N1" while autoBaud is set (the number is the effective rate in baudRate).
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
 * The serial-port Transport: owns one QSerialPort (or a DeviceSimulator for the "SIM:" pseudo-
 * ports) and adds what a terminal session needs on top of it. State, error string, RX/TX
 * counters, auto-reconnect flag/interval and the generic signals (dataReceived, dataSent,
 * stateChanged, errorOccurred, countersChanged, txBytesWritten, connectionLost,
 * connectionRestored) come from Transport; this class adds the serial-specific API.
 *
 * - kind() is Kind::Serial, displayName() the port name ("COM8", "SIM:linux"), summary() the
 *   line parameters ("115200 8N1"), settingsMap() {"kind":"serial"} merged with
 *   SerialSettings::toMap(). A serial port connects synchronously: the state is never Connecting.
 * - Asynchronous, non-blocking I/O on the GUI thread (readyRead -> dataReceived()).
 *   write() never calls waitForBytesWritten().
 * - Auto-reconnect: when the device disappears (QSerialPort::ResourceError / PermissionError /
 *   DeviceNotFoundError while connected, a Read/Write/UnknownError while the port is no longer
 *   listed by QSerialPortInfo - Windows drivers report USB removal as ERROR_GEN_FAILURE, which
 *   Qt maps to UnknownError and after which QSerialPort stops reading - or
 *   SerialPortEnumerator::portRemoved for the open port) the port is closed, state becomes
 *   Reconnecting, and a timer re-tries open() every reconnectIntervalMs() while the port name
 *   is listed by SerialPortEnumerator's snapshot (or by QSerialPortInfo directly when the
 *   enumerator is not running). close() (user action) always stops reconnecting; so does
 *   setAutoReconnect(false) (state -> Disconnected). When the port is back but the stored line
 *   parameters are rejected by the driver, that is reported once through errorOccurred() and the
 *   timer keeps polling. setReconnectIntervalMs() clamps to 200..60000; a no-op change never
 *   restarts a pending reconnect.
 * - Drop / recovery reporting: portDisappeared(port) is followed by the generic
 *   connectionLost(port); reconnected(port) (after stateChanged(Connected)) by connectionRestored(port).
 * - Live parameter changes: setSettings() while open applies baud/data/parity/stop/flow
 *   immediately (QSerialPort supports this) and DTR/RTS via setDtr/setRts. A changed
 *   portName while open is NOT applied until the next open(). A parameter the driver rejects
 *   is reported through errorOccurred() and NOT stored: settings() keeps the last accepted
 *   value for that field; callers re-read settings() after a rejection. A live change is
 *   never mistaken for a lost device: the QSerialPort errors the setters raise while they run
 *   are reported by setSettings() itself and ignored by the error handler, so a baud-rate
 *   change (BaudRateDetector switches rates every few hundred ms while it searches) never
 *   emits portDisappeared() or starts a reconnect. The autoBaud flag is stored with the
 *   other fields and shows in summary(); it changes nothing else here.
 * - Error reporting: every QSerialPort error other than NoError/TimeoutError is forwarded
 *   through errorOccurred(message) with a human-readable message that includes the port
 *   name; PermissionError on open is reported as "port busy or access denied".
 * - notifyTerminalSize() is ignored (a UART has no notion of a window size).
 */
class SerialConnection : public Transport
{
    Q_OBJECT
public:
    /// Kept so every existing SerialConnection::State::Connected spelling compiles; the enum
    /// (and its Q_ENUM registration) lives in Transport.
    using State = Transport::State;

    explicit SerialConnection(QObject* parent = nullptr);
    ~SerialConnection() override;

    SerialSettings settings() const;
    void setSettings(const SerialSettings& settings);

    QString portName() const;             ///< settings().portName

    Kind kind() const override;           ///< Kind::Serial
    QString displayName() const override; ///< portName()
    QString summary() const override;     ///< settings().summary()
    QVariantMap settingsMap() const override;   ///< {"kind":"serial"} + SerialSettings::toMap()

    /// Turning it off stops the reconnect timer (and, while Reconnecting, -> Disconnected).
    void setAutoReconnect(bool on) override;
    /// Clamped 200..60000; a no-op change never restarts a pending reconnect.
    void setReconnectIntervalMs(int ms) override;

    bool dtr() const;
    bool rts() const;

    /// Bytes accepted by write() that QSerialPort has not yet handed to the driver
    /// (QSerialPort::bytesToWrite()); 0 for a simulated device or while not open. Together
    /// with txBytesWritten() this lets a paced sender (SendFileDialog) apply backpressure
    /// instead of growing the unbounded write buffer.
    qint64 pendingTxBytes() const override;

public slots:
    /// Open using settings(). Emits stateChanged(Connected) on success (plus reconnected() /
    /// connectionRestored() if the state was Reconnecting); on failure emits errorOccurred() and
    /// the state is unchanged (Disconnected, or Reconnecting with the retry timer still polling).
    /// Returns success. Calling while open returns true. An explicit open() while Reconnecting
    /// is a forced, non-quiet attempt.
    bool open() override;
    /// Close the port and stop any reconnect attempt. Emits stateChanged(Disconnected) if changed.
    void close() override;
    /// Queue `data` for transmission. Returns the number of bytes accepted (data.size()) or
    /// -1 when not open (an errorOccurred() is NOT emitted for that case - callers check isOpen()).
    /// Emits dataSent(data) and countersChanged() on success.
    qint64 write(const QByteArray& data) override;
    void setDtr(bool on);                 ///< remembered in settings; applied immediately when open
    void setRts(bool on);
    /// Assert BREAK for `durationMs` (default 250 ms) then release, asynchronously (QTimer).
    /// Returns true when BREAK was asserted (or the session is a simulated device, which
    /// ignores BREAK); returns false when not open (no errorOccurred() emitted, callers check
    /// isOpen()) or when the driver rejects BREAK (errorOccurred() emitted with
    /// "Cannot send BREAK on <port>: <reason>").
    bool sendBreak(int durationMs = 250);
    /// Discard the pending RX and/or TX buffers (QSerialPort::clear(directions); a no-op for a
    /// simulated device or a closed port). Input only is what BaudRateDetector uses between two
    /// candidate rates: the TX side must be left alone there, because clearing it drops every
    /// byte the session queued (a SendFileDialog transfer would silently lose chunks) and, on
    /// Windows, aborts an overlapped write in flight, whose ERROR_OPERATION_ABORTED completion
    /// Qt reports later as a ResourceError - the "device vanished" signature. An input purge
    /// can only abort a pending overlapped read, which Qt's Windows backend returns at once
    /// (ReadIntervalTimeout = MAXDWORD), so that race is practically closed; should it happen,
    /// the ResourceError takes the usual vanished-device path and the port is reopened, which
    /// is also the only way to revive QSerialPort's read loop after such an abort.
    void clearBuffers(QSerialPort::Directions directions = QSerialPort::AllDirections);

signals:
    void pinsChanged(bool dtr, bool rts);
    /// Emitted once when the device vanished while connected (after errorOccurred(), before
    /// reconnect starts); connectionLost(portName) follows immediately.
    void portDisappeared(const QString& portName);
    /// Emitted when the port was reopened after portDisappeared(), whether by the reconnect
    /// timer or by an explicit open() (after stateChanged(Connected)); connectionRestored(portName)
    /// follows immediately.
    void reconnected(const QString& portName);

private slots:
    void onReadyRead();
    void onPortError(QSerialPort::SerialPortError error);
    void tryReconnect();

private:
    void changeState(State state);        ///< Transport::setState() plus a debug log line
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
    bool m_reconnectParamErrorReported = false;   ///< "port is back but parameters rejected" reported once per outage
    bool m_applyingParameters = false;   ///< inside setSettings()' live setters: their errors are theirs to report
    QTimer m_reconnectTimer;
    QTimer m_breakTimer;
    DeviceSimulator* m_simulator = nullptr;   ///< QObject child while a SIM: port is open, else nullptr
};
