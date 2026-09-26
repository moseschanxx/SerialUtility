#pragma once

#include <QObject>
#include <QString>
#include <QByteArray>

/**
 * Abstract byte-stream transport behind a session tab. SerialConnection (serial ports and the
 * SIM: pseudo-devices) and SshConnection (SSH shell channels) implement it; SessionWidget,
 * SessionLogger, HexDumpView and the terminal only depend on this interface.
 *
 * Contract
 *  - GUI-thread API. Implementations that use worker threads marshal everything to the GUI
 *    thread before emitting signals or changing state.
 *  - open() is asynchronous-capable: it may return true while the state is still
 *    Connecting; the outcome is reported through stateChanged()/errorOccurred(). Serial
 *    ports connect synchronously; SSH goes Connecting -> Connected or Disconnected.
 *  - write() never blocks; it returns the number of bytes accepted (data.size()) or -1
 *    when the transport is not connected. dataSent() is emitted for accepted bytes.
 *  - Every received byte goes through dataReceived(); counters are kept by the base class
 *    (countReceived()/countSent()) and published via countersChanged().
 *  - connectionLost()/connectionRestored() describe an unexpected drop and a successful
 *    automatic reconnect (serial: device vanished/re-plugged; SSH: socket dropped/re-opened).
 *  - notifyTerminalSize() is called by the session whenever the terminal grid changes; SSH
 *    forwards it as a window-change request, serial ignores it (a UART has no notion of size;
 *    "stty" is sent explicitly by the user).
 */
class Transport : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Disconnected,
        Connecting,     ///< open() in progress (SSH: TCP + key exchange + auth)
        Connected,
        Reconnecting    ///< link dropped, automatic re-open in progress
    };
    Q_ENUM(State)

    enum class Kind { Serial, Ssh };
    Q_ENUM(Kind)

    explicit Transport(QObject* parent = nullptr);
    ~Transport() override;

    virtual Kind kind() const = 0;
    State state() const;
    bool isOpen() const;                       ///< state() == Connected
    QString errorString() const;               ///< last error message ("" if none)

    /// Short identity for tab titles and the window title: "COM8", "SIM:linux", "root@10.0.0.24".
    virtual QString displayName() const = 0;
    /// Parameter summary for the status bar: "115200 8N1", "ssh-ed25519 · publickey".
    virtual QString summary() const = 0;
    /// Kind-specific settings map (persisted with the session list; see SessionWidget).
    virtual QVariantMap settingsMap() const = 0;

    quint64 bytesReceived() const;
    quint64 bytesSent() const;
    void resetCounters();                      ///< emits countersChanged(0, 0)
    /// Bytes accepted by write() that have not left the process yet (0 when unknown).
    virtual qint64 pendingTxBytes() const;

    bool autoReconnect() const;
    virtual void setAutoReconnect(bool on);    ///< turning it off while Reconnecting -> Disconnected
    int reconnectIntervalMs() const;
    virtual void setReconnectIntervalMs(int ms);

    /// Translatable display text for a state ("Disconnected", "Connecting...", "Connected", "Reconnecting...").
    static QString stateText(State state);
    static QString kindText(Kind kind);        ///< "Serial" / "SSH"

public slots:
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual qint64 write(const QByteArray& data) = 0;
    /// Terminal grid changed (cols x rows). Default: no-op.
    virtual void notifyTerminalSize(int cols, int rows);

signals:
    void dataReceived(const QByteArray& data);
    void dataSent(const QByteArray& data);
    void stateChanged(Transport::State state);
    void errorOccurred(const QString& message);
    void countersChanged(quint64 rx, quint64 tx);
    /// Bytes handed to the OS/socket since the previous emission (serial: QSerialPort::bytesWritten).
    void txBytesWritten(qint64 bytes);
    /// The link dropped unexpectedly (before any automatic reconnect starts).
    void connectionLost(const QString& name);
    /// An automatic reconnect succeeded (after stateChanged(Connected)).
    void connectionRestored(const QString& name);

protected:
    void setState(State state);                ///< emits stateChanged() on change
    void setErrorString(const QString& message);
    void countReceived(qint64 bytes);          ///< emits countersChanged()
    void countSent(qint64 bytes);              ///< emits countersChanged()

    bool m_autoReconnect = true;
    int m_reconnectIntervalMs = 1000;

private:
    State m_state = State::Disconnected;
    QString m_errorString;
    quint64 m_rx = 0;
    quint64 m_tx = 0;
};
