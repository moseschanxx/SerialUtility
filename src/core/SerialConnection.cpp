#include "core/SerialConnection.h"

#include <QSerialPortInfo>
#include <QStringList>
#include <algorithm>

#include "app/Logging.h"
#include "core/DeviceSimulator.h"
#include "core/SerialPortEnumerator.h"

namespace {

constexpr int kMinReconnectMs = 200;
constexpr int kMaxReconnectMs = 60000;
constexpr int kDefaultReconnectMs = 1000;

template <typename Enum>
Enum enumFromVariant(const QVariant& value, Enum fallback, std::initializer_list<Enum> allowed)
{
    if (!value.isValid()) {
        return fallback;
    }
    bool ok = false;
    const int i = value.toInt(&ok);
    if (!ok) {
        return fallback;
    }
    for (Enum e : allowed) {
        if (static_cast<int>(e) == i) {
            return e;
        }
    }
    return fallback;
}

QString portErrorName(QSerialPort::SerialPortError error)
{
    switch (error) {
    case QSerialPort::NoError:
        return QStringLiteral("NoError");
    case QSerialPort::DeviceNotFoundError:
        return QStringLiteral("DeviceNotFoundError");
    case QSerialPort::PermissionError:
        return QStringLiteral("PermissionError");
    case QSerialPort::OpenError:
        return QStringLiteral("OpenError");
    case QSerialPort::WriteError:
        return QStringLiteral("WriteError");
    case QSerialPort::ReadError:
        return QStringLiteral("ReadError");
    case QSerialPort::ResourceError:
        return QStringLiteral("ResourceError");
    case QSerialPort::UnsupportedOperationError:
        return QStringLiteral("UnsupportedOperationError");
    case QSerialPort::UnknownError:
        return QStringLiteral("UnknownError");
    case QSerialPort::TimeoutError:
        return QStringLiteral("TimeoutError");
    case QSerialPort::NotOpenError:
        return QStringLiteral("NotOpenError");
    }
    return QStringLiteral("SerialPortError(%1)").arg(static_cast<int>(error));
}

} // namespace

// ---------------------------------------------------------------------------------------
// SerialSettings
// ---------------------------------------------------------------------------------------

QVariantMap SerialSettings::toMap() const
{
    QVariantMap map;
    map.insert(QStringLiteral("port"), portName);
    map.insert(QStringLiteral("baud"), baudRate);
    map.insert(QStringLiteral("dataBits"), static_cast<int>(dataBits));
    map.insert(QStringLiteral("parity"), static_cast<int>(parity));
    map.insert(QStringLiteral("stopBits"), static_cast<int>(stopBits));
    map.insert(QStringLiteral("flow"), static_cast<int>(flowControl));
    map.insert(QStringLiteral("dtr"), dtr);
    map.insert(QStringLiteral("rts"), rts);
    return map;
}

SerialSettings SerialSettings::fromMap(const QVariantMap& map)
{
    SerialSettings s;
    s.portName = map.value(QStringLiteral("port"), s.portName).toString();
    bool ok = false;
    const qint32 baud = map.value(QStringLiteral("baud")).toInt(&ok);
    if (ok && isValidBaudRate(baud)) {
        s.baudRate = baud;
    }
    s.dataBits = enumFromVariant(map.value(QStringLiteral("dataBits")), s.dataBits,
                                 {QSerialPort::Data5, QSerialPort::Data6, QSerialPort::Data7, QSerialPort::Data8});
    s.parity = enumFromVariant(map.value(QStringLiteral("parity")), s.parity,
                               {QSerialPort::NoParity, QSerialPort::EvenParity, QSerialPort::OddParity,
                                QSerialPort::SpaceParity, QSerialPort::MarkParity});
    s.stopBits = enumFromVariant(map.value(QStringLiteral("stopBits")), s.stopBits,
                                 {QSerialPort::OneStop, QSerialPort::OneAndHalfStop, QSerialPort::TwoStop});
    s.flowControl = enumFromVariant(map.value(QStringLiteral("flow")), s.flowControl,
                                    {QSerialPort::NoFlowControl, QSerialPort::HardwareControl,
                                     QSerialPort::SoftwareControl});
    s.dtr = map.value(QStringLiteral("dtr"), s.dtr).toBool();
    s.rts = map.value(QStringLiteral("rts"), s.rts).toBool();
    return s;
}

QString SerialSettings::summary() const
{
    QString text = QStringLiteral("%1 %2%3%4")
                       .arg(baudRate)
                       .arg(static_cast<int>(dataBits))
                       .arg(parityLetter(parity))
                       .arg(stopBitsText(stopBits));
    if (flowControl != QSerialPort::NoFlowControl) {
        text += QLatin1Char(' ') + flowControlText(flowControl);
    }
    return text;
}

QList<qint32> SerialSettings::standardBaudRates()
{
    return {300,    1200,   2400,   4800,   9600,    19200,   38400,   57600,   115200,
            230400, 460800, 500000, 921600, 1000000, 1500000, 2000000, 3000000, 4000000};
}

QChar SerialSettings::parityLetter(QSerialPort::Parity parity)
{
    switch (parity) {
    case QSerialPort::EvenParity:
        return QLatin1Char('E');
    case QSerialPort::OddParity:
        return QLatin1Char('O');
    case QSerialPort::SpaceParity:
        return QLatin1Char('S');
    case QSerialPort::MarkParity:
        return QLatin1Char('M');
    case QSerialPort::NoParity:
        break;
    }
    return QLatin1Char('N');
}

QString SerialSettings::stopBitsText(QSerialPort::StopBits stopBits)
{
    switch (stopBits) {
    case QSerialPort::OneAndHalfStop:
        return QStringLiteral("1.5");
    case QSerialPort::TwoStop:
        return QStringLiteral("2");
    case QSerialPort::OneStop:
        break;
    }
    return QStringLiteral("1");
}

QString SerialSettings::flowControlText(QSerialPort::FlowControl flow)
{
    switch (flow) {
    case QSerialPort::HardwareControl:
        return QStringLiteral("RTS/CTS");
    case QSerialPort::SoftwareControl:
        return QStringLiteral("XON/XOFF");
    case QSerialPort::NoFlowControl:
        break;
    }
    return QStringLiteral("None");
}

bool SerialSettings::operator==(const SerialSettings& other) const
{
    return portName == other.portName && baudRate == other.baudRate && dataBits == other.dataBits &&
           parity == other.parity && stopBits == other.stopBits && flowControl == other.flowControl &&
           dtr == other.dtr && rts == other.rts;
}

bool SerialSettings::isSimulatedPort() const
{
    return DeviceSimulator::isSimulatedPort(portName);
}

// ---------------------------------------------------------------------------------------
// SerialConnection
// ---------------------------------------------------------------------------------------

SerialConnection::SerialConnection(QObject* parent)
    : QObject(parent)
{
    connect(&m_port, &QSerialPort::readyRead, this, &SerialConnection::onReadyRead);
    connect(&m_port, &QSerialPort::errorOccurred, this, &SerialConnection::onPortError);
    connect(&m_port, &QSerialPort::bytesWritten, this, &SerialConnection::txBytesWritten);

    // Safety net for drivers whose I/O error arrives late or never when the adapter is unplugged:
    // the open port simply drops out of the enumerator's list. Idempotent with onPortError():
    // whichever fires first moves the state to Reconnecting and the other is then ignored. The
    // enumerator only emits once MainWindow has started it, so headless/test code is unaffected.
    connect(&SerialPortEnumerator::instance(), &SerialPortEnumerator::portRemoved, this,
            [this](const QString& removed) {
                if (m_state == State::Connected && !m_simulator && m_port.isOpen() &&
                    removed.compare(m_settings.portName, Qt::CaseInsensitive) == 0) {
                    qCWarning(lcSerial) << m_settings.portName << "removed from the port list while connected";
                    handleDeviceVanished();
                }
            });

    m_reconnectTimer.setInterval(kDefaultReconnectMs);
    m_reconnectTimer.setSingleShot(false);
    connect(&m_reconnectTimer, &QTimer::timeout, this, &SerialConnection::tryReconnect);

    m_breakTimer.setSingleShot(true);
    connect(&m_breakTimer, &QTimer::timeout, this, [this]() {
        if (m_port.isOpen()) {
            if (!m_port.setBreakEnabled(false)) {
                qCWarning(lcSerial) << m_settings.portName << "cannot release BREAK:" << m_port.errorString();
            } else {
                qCDebug(lcSerial) << m_settings.portName << "BREAK released";
            }
        }
    });
}

SerialConnection::~SerialConnection()
{
    m_reconnectTimer.stop();
    m_breakTimer.stop();
    if (m_simulator) {
        disconnect(m_simulator, nullptr, this, nullptr);
        delete m_simulator;   // a QObject child; deleted here so the log line is accurate
        m_simulator = nullptr;
        qCInfo(lcSerial) << "closed" << m_settings.portName << "(simulated device, connection destroyed)";
    }
    if (m_port.isOpen()) {
        m_port.close();
        qCInfo(lcSerial) << "closed" << m_settings.portName << "(connection destroyed)";
    }
}

SerialSettings SerialConnection::settings() const
{
    return m_settings;
}

void SerialConnection::setSettings(const SerialSettings& settings)
{
    const SerialSettings old = m_settings;
    if (m_simulator) {
        // Simulated device: only the pacing (baud rate) and the pin states are meaningful.
        m_settings = settings;
        if (old.baudRate != settings.baudRate) {
            m_simulator->setBaudRate(settings.baudRate);
            qCInfo(lcSerial) << m_settings.portName << "line parameters changed to" << m_settings.summary();
        }
        if (old.dtr != settings.dtr || old.rts != settings.rts) {
            emit pinsChanged(m_settings.dtr, m_settings.rts);
        }
        return;
    }
    if (!m_port.isOpen()) {
        m_settings = settings;   // a closed port accepts anything; validated on the next open()
        return;
    }

    // Live line-parameter changes: apply what differs, one setter at a time, and commit only
    // what the driver accepted. A rejected field is rolled back in QSerialPort too, so its
    // internal cache does not hold a value that the next open() would fail on.
    SerialSettings applied = settings;
    QStringList failures;
    QString reason;
    auto rejected = [this, &failures, &reason](const QString& what, bool restored, const char* field) {
        failures.append(what);
        if (reason.isEmpty()) {
            reason = m_port.errorString();
        }
        if (!restored) {
            qCDebug(lcSerial) << m_settings.portName << "cannot restore previous" << field << ":" << m_port.errorString();
        }
    };
    if (old.baudRate != settings.baudRate && !m_port.setBaudRate(settings.baudRate)) {
        applied.baudRate = old.baudRate;
        rejected(tr("baud rate %1").arg(settings.baudRate), m_port.setBaudRate(old.baudRate), "baud rate");
    }
    if (old.dataBits != settings.dataBits && !m_port.setDataBits(settings.dataBits)) {
        applied.dataBits = old.dataBits;
        rejected(tr("data bits %1").arg(static_cast<int>(settings.dataBits)), m_port.setDataBits(old.dataBits),
                 "data bits");
    }
    if (old.parity != settings.parity && !m_port.setParity(settings.parity)) {
        applied.parity = old.parity;
        rejected(tr("parity %1").arg(SerialSettings::parityLetter(settings.parity)), m_port.setParity(old.parity),
                 "parity");
    }
    if (old.stopBits != settings.stopBits && !m_port.setStopBits(settings.stopBits)) {
        applied.stopBits = old.stopBits;
        rejected(tr("stop bits %1").arg(SerialSettings::stopBitsText(settings.stopBits)),
                 m_port.setStopBits(old.stopBits), "stop bits");
    }
    if (old.flowControl != settings.flowControl && !m_port.setFlowControl(settings.flowControl)) {
        applied.flowControl = old.flowControl;
        rejected(tr("flow control %1").arg(SerialSettings::flowControlText(settings.flowControl)),
                 m_port.setFlowControl(old.flowControl), "flow control");
    }
    m_settings = applied;   // portName, dtr and rts are always taken from `settings`

    if (!failures.isEmpty()) {
        m_errorString =
            tr("Cannot apply %1 to %2: %3").arg(failures.join(QStringLiteral(", ")), m_settings.portName, reason);
        qCWarning(lcSerial) << m_errorString;
        emit errorOccurred(m_errorString);
    }
    if (old.baudRate != applied.baudRate || old.dataBits != applied.dataBits || old.parity != applied.parity ||
        old.stopBits != applied.stopBits || old.flowControl != applied.flowControl) {
        qCInfo(lcSerial) << m_settings.portName << "line parameters changed to" << m_settings.summary();
    }

    bool pins = false;
    if (old.dtr != applied.dtr) {
        pins = true;
        if (!m_port.setDataTerminalReady(applied.dtr)) {
            qCWarning(lcSerial) << m_settings.portName << "cannot set DTR:" << m_port.errorString();
        }
    }
    if (old.rts != applied.rts || (old.flowControl != applied.flowControl)) {
        pins = true;
        if (applied.flowControl != QSerialPort::HardwareControl && !m_port.setRequestToSend(applied.rts)) {
            qCWarning(lcSerial) << m_settings.portName << "cannot set RTS:" << m_port.errorString();
        }
    }
    if (pins) {
        emit pinsChanged(m_settings.dtr, m_settings.rts);
    }
    if (old.portName != applied.portName) {
        qCInfo(lcSerial) << "port name changed to" << applied.portName << "- applied on next open";
    }
}

SerialConnection::State SerialConnection::state() const
{
    return m_state;
}

bool SerialConnection::isOpen() const
{
    return m_state == State::Connected;
}

QString SerialConnection::portName() const
{
    return m_settings.portName;
}

QString SerialConnection::errorString() const
{
    return m_errorString;
}

quint64 SerialConnection::bytesReceived() const
{
    return m_rx;
}

quint64 SerialConnection::bytesSent() const
{
    return m_tx;
}

void SerialConnection::resetCounters()
{
    m_rx = 0;
    m_tx = 0;
    emit countersChanged(0, 0);
}

bool SerialConnection::autoReconnect() const
{
    return m_autoReconnect;
}

void SerialConnection::setAutoReconnect(bool on)
{
    m_autoReconnect = on;
    if (!on && m_state == State::Reconnecting) {
        m_reconnectTimer.stop();
        qCInfo(lcSerial) << "auto-reconnect disabled, giving up on" << m_settings.portName;
        setState(State::Disconnected);
    }
}

int SerialConnection::reconnectIntervalMs() const
{
    return m_reconnectTimer.interval();
}

void SerialConnection::setReconnectIntervalMs(int ms)
{
    const int clamped = qBound(kMinReconnectMs, ms, kMaxReconnectMs);
    if (clamped == m_reconnectTimer.interval()) {
        return;   // QTimer::setInterval() restarts an active timer; a no-op write must not delay a pending reconnect
    }
    m_reconnectTimer.setInterval(clamped);
}

bool SerialConnection::dtr() const
{
    return m_settings.dtr;
}

bool SerialConnection::rts() const
{
    return m_settings.rts;
}

qint64 SerialConnection::pendingTxBytes() const
{
    return (m_simulator || !m_port.isOpen()) ? 0 : m_port.bytesToWrite();
}

QString SerialConnection::stateText(State state)
{
    switch (state) {
    case State::Connected:
        return tr("Connected");
    case State::Reconnecting:
        return tr("Reconnecting...");
    case State::Disconnected:
        break;
    }
    return tr("Disconnected");
}

bool SerialConnection::open()
{
    return openPort(/*quiet=*/false);
}

bool SerialConnection::openPort(bool quiet)
{
    if (m_port.isOpen() || m_simulator) {
        return true;
    }
    const QString name = m_settings.portName;
    const bool wasReconnecting = (m_state == State::Reconnecting);

    auto reportFailure = [this, quiet](const QString& message) {
        m_errorString = message;
        if (quiet) {
            qCDebug(lcSerial) << "reconnect attempt failed:" << message;
        } else {
            qCWarning(lcSerial) << message;
            emit errorOccurred(message);
        }
    };

    if (name.trimmed().isEmpty()) {
        reportFailure(tr("No serial port selected"));
        return false;
    }
    if (m_settings.isSimulatedPort()) {
        if (!openSimulator(quiet)) {
            return false;
        }
    } else {
        m_port.setPortName(name);
        if (!m_port.open(QIODevice::ReadWrite)) {
            QString message;
            switch (m_port.error()) {
            case QSerialPort::PermissionError:
                message = tr("Port %1 is busy or access denied").arg(name);
                break;
            case QSerialPort::DeviceNotFoundError:
                message = tr("Port %1 not found").arg(name);
                break;
            default:
                message = tr("Cannot open %1: %2").arg(name, m_port.errorString());
                break;
            }
            reportFailure(message);
            return false;
        }

        if (!applyParameters()) {
            m_port.close();
            if (quiet) {
                // The port is present and openable but the stored line parameters are rejected:
                // unlike "not back yet" this deserves one visible message. Keep retrying while
                // the port is listed (header contract) so a transient driver failure right after
                // re-enumeration still recovers.
                if (!m_reconnectParamErrorReported) {
                    m_reconnectParamErrorReported = true;
                    // m_errorString is already "Cannot set X on PORT: reason" (applyParameters()).
                    const QString msg = tr("Port %1 is back but %2").arg(name, m_errorString);
                    m_errorString = msg;
                    qCWarning(lcSerial) << msg;
                    emit errorOccurred(msg);
                } else {
                    qCDebug(lcSerial) << "reconnect attempt failed:" << m_errorString;
                }
            } else {
                reportFailure(m_errorString);
            }
            return false;
        }

        if (!m_port.setDataTerminalReady(m_settings.dtr)) {
            qCWarning(lcSerial) << name << "cannot set DTR:" << m_port.errorString();
        }
        if (m_settings.flowControl != QSerialPort::HardwareControl && !m_port.setRequestToSend(m_settings.rts)) {
            qCWarning(lcSerial) << name << "cannot set RTS:" << m_port.errorString();
        }
        if (!m_port.clear(QSerialPort::AllDirections)) {
            qCDebug(lcSerial) << name << "clear() failed:" << m_port.errorString();
        }

        m_errorString.clear();
        m_reconnectParamErrorReported = false;
        m_reconnectTimer.stop();
        qCInfo(lcSerial) << "opened" << name << m_settings.summary() << "DTR" << m_settings.dtr << "RTS"
                         << m_settings.rts;
        setState(State::Connected);
        emit pinsChanged(m_settings.dtr, m_settings.rts);
    }

    if (wasReconnecting) {
        qCInfo(lcSerial) << "reconnected to" << name;
        emit reconnected(name);
    }
    return true;
}

void SerialConnection::close()
{
    const bool wasReconnecting = m_reconnectTimer.isActive() || m_state == State::Reconnecting;
    m_reconnectTimer.stop();
    m_breakTimer.stop();
    m_reconnectParamErrorReported = false;
    if (m_simulator) {
        closeSimulator();
        qCInfo(lcSerial) << "closed" << m_settings.portName << "(simulated device)";
    } else if (m_port.isOpen()) {
        const qint64 dropped = m_port.bytesToWrite();
        if (dropped > 0) {
            qCWarning(lcSerial) << m_settings.portName << "closed with" << dropped << "unsent bytes discarded";
        }
        m_port.close();
        qCInfo(lcSerial) << "closed" << m_settings.portName;
    } else if (wasReconnecting) {
        qCInfo(lcSerial) << "reconnect to" << m_settings.portName << "cancelled";
    }
    setState(State::Disconnected);
}

qint64 SerialConnection::write(const QByteArray& data)
{
    if (!isOpen() || (!m_simulator && !m_port.isOpen())) {
        return -1;
    }
    if (data.isEmpty()) {
        return 0;
    }
    if (m_simulator) {
        m_simulator->receive(data);
        m_tx += static_cast<quint64>(data.size());
        emit dataSent(data);
        emit countersChanged(m_rx, m_tx);
        return data.size();
    }
    const qint64 written = m_port.write(data);
    if (written < 0) {
        m_errorString = tr("Write to %1 failed: %2").arg(m_settings.portName, m_port.errorString());
        qCWarning(lcSerial) << m_errorString;
        emit errorOccurred(m_errorString);
        return -1;
    }
    m_tx += static_cast<quint64>(written);
    emit dataSent(written == data.size() ? data : data.left(written));
    emit countersChanged(m_rx, m_tx);
    return written;
}

void SerialConnection::setDtr(bool on)
{
    m_settings.dtr = on;
    if (m_simulator) {
        qCDebug(lcSerial) << m_settings.portName << "DTR" << on << "(simulated)";
    } else if (m_port.isOpen()) {
        if (!m_port.setDataTerminalReady(on)) {
            m_errorString = tr("Cannot set DTR on %1: %2").arg(m_settings.portName, m_port.errorString());
            qCWarning(lcSerial) << m_errorString;
            emit errorOccurred(m_errorString);
        } else {
            qCDebug(lcSerial) << m_settings.portName << "DTR" << on;
        }
    }
    emit pinsChanged(m_settings.dtr, m_settings.rts);
}

void SerialConnection::setRts(bool on)
{
    m_settings.rts = on;
    if (m_simulator) {
        qCDebug(lcSerial) << m_settings.portName << "RTS" << on << "(simulated)";
    } else if (m_port.isOpen()) {
        if (m_settings.flowControl == QSerialPort::HardwareControl) {
            qCDebug(lcSerial) << m_settings.portName << "RTS is driven by hardware flow control; stored only";
        } else if (!m_port.setRequestToSend(on)) {
            m_errorString = tr("Cannot set RTS on %1: %2").arg(m_settings.portName, m_port.errorString());
            qCWarning(lcSerial) << m_errorString;
            emit errorOccurred(m_errorString);
        } else {
            qCDebug(lcSerial) << m_settings.portName << "RTS" << on;
        }
    }
    emit pinsChanged(m_settings.dtr, m_settings.rts);
}

bool SerialConnection::sendBreak(int durationMs)
{
    if (m_simulator) {
        qCInfo(lcSerial) << m_settings.portName << "BREAK for" << durationMs << "ms (simulated device ignores it)";
        return true;
    }
    if (!m_port.isOpen()) {
        qCDebug(lcSerial) << "sendBreak ignored: not connected";
        return false;
    }
    if (!m_port.setBreakEnabled(true)) {
        m_errorString = tr("Cannot send BREAK on %1: %2").arg(m_settings.portName, m_port.errorString());
        qCWarning(lcSerial) << m_errorString;
        emit errorOccurred(m_errorString);
        return false;
    }
    qCInfo(lcSerial) << m_settings.portName << "BREAK asserted for" << durationMs << "ms";
    m_breakTimer.start(qMax(1, durationMs));
    return true;
}

void SerialConnection::clearBuffers()
{
    if (m_simulator || !m_port.isOpen()) {
        return;
    }
    if (!m_port.clear(QSerialPort::AllDirections)) {
        qCWarning(lcSerial) << m_settings.portName << "clear() failed:" << m_port.errorString();
    }
}

void SerialConnection::onReadyRead()
{
    handleIncoming(m_port.readAll());
}

void SerialConnection::handleIncoming(const QByteArray& data)
{
    if (data.isEmpty()) {
        return;
    }
    m_rx += static_cast<quint64>(data.size());
    emit dataReceived(data);
    emit countersChanged(m_rx, m_tx);
}

void SerialConnection::onPortError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError || error == QSerialPort::TimeoutError) {
        return;
    }
    // Errors raised while not connected (during open(), during a reconnect attempt, from a
    // setter on a closed port) are reported by the code that triggered them.
    if (m_state != State::Connected) {
        qCDebug(lcSerial) << m_settings.portName << "ignored error while" << stateText(m_state) << ":"
                          << portErrorName(error) << m_port.errorString();
        return;
    }

    const QString name = m_settings.portName;
    switch (error) {
    case QSerialPort::ResourceError:
    case QSerialPort::PermissionError:
    case QSerialPort::DeviceNotFoundError: {
        // Device vanished (board reboot, USB re-plug).
        qCWarning(lcSerial) << name << "disappeared:" << portErrorName(error) << m_port.errorString();
        handleDeviceVanished();
        break;
    }
    case QSerialPort::ReadError:
    case QSerialPort::WriteError:
    case QSerialPort::UnknownError:
        // Windows maps ERROR_GEN_FAILURE / ERROR_DEVICE_NOT_CONNECTED / ERROR_NO_SUCH_DEVICE to
        // UnknownError and a failed overlapped read to ReadError, and QSerialPort stops its read
        // loop on any such completion error. If the port is no longer enumerated the handle is
        // dead: take the same path as ResourceError so auto-reconnect can bring the session back.
        // Only reached while Connected (guard above) and Qt emits at most one error before the
        // loop stops, so the fresh QSerialPortInfo scan runs once per outage, not repeatedly.
        if (!isPortListed(/*useEnumeratorSnapshot=*/false)) {
            qCWarning(lcSerial) << name << "disappeared:" << portErrorName(error) << m_port.errorString();
            handleDeviceVanished();
            break;
        }
        m_errorString = tr("Port %1: %2").arg(name, m_port.errorString());
        qCWarning(lcSerial) << m_errorString << "(" << portErrorName(error) << ")";
        emit errorOccurred(m_errorString);
        break;
    default:
        // OpenError / NotOpenError / UnsupportedOperationError are reported by their callers.
        qCDebug(lcSerial) << name << "port error" << portErrorName(error) << m_port.errorString();
        break;
    }
}

void SerialConnection::tryReconnect()
{
    if (m_state != State::Reconnecting) {
        m_reconnectTimer.stop();
        return;
    }
    // Periodic tick: reuse the application-wide enumerator snapshot rather than re-enumerating
    // per tab per tick (on Linux availablePorts() walks sysfs/udev).
    if (!isPortListed(/*useEnumeratorSnapshot=*/true)) {
        return;
    }
    qCDebug(lcSerial) << m_settings.portName << "is back, trying to reopen";
    openPort(/*quiet=*/true);   // emits reconnected() on success
}

bool SerialConnection::isPortListed(bool useEnumeratorSnapshot) const
{
    const QString name = m_settings.portName;
    if (m_settings.isSimulatedPort()) {
        return DeviceSimulator::isPresent(name);
    }
    // Case-insensitive throughout (Windows "com8" vs "COM8").
    const auto sameName = [&name](const QString& other) { return other.compare(name, Qt::CaseInsensitive) == 0; };
    if (useEnumeratorSnapshot) {
        const SerialPortEnumerator& enumerator = SerialPortEnumerator::instance();
        if (enumerator.isRunning()) {
            const QStringList names = enumerator.portNames();
            return std::any_of(names.cbegin(), names.cend(), sameName);
        }
    }
    // Headless / tests, or a one-shot check that must not be a poll interval stale: ask
    // QSerialPortInfo directly.
    const QList<QSerialPortInfo> infos = QSerialPortInfo::availablePorts();
    return std::any_of(infos.cbegin(), infos.cend(),
                       [&sameName](const QSerialPortInfo& info) { return sameName(info.portName()); });
}

void SerialConnection::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    qCDebug(lcSerial) << m_settings.portName << "state ->" << stateText(state);
    emit stateChanged(state);
}

bool SerialConnection::applyParameters()
{
    const QString name = m_settings.portName;
    auto fail = [this, &name](const QString& what) {
        m_errorString = tr("Cannot set %1 on %2: %3").arg(what, name, m_port.errorString());
        qCWarning(lcSerial) << m_errorString;
        return false;
    };
    if (!m_port.setBaudRate(m_settings.baudRate)) {
        return fail(tr("baud rate %1").arg(m_settings.baudRate));
    }
    if (!m_port.setDataBits(m_settings.dataBits)) {
        return fail(tr("data bits %1").arg(static_cast<int>(m_settings.dataBits)));
    }
    if (!m_port.setParity(m_settings.parity)) {
        return fail(tr("parity %1").arg(SerialSettings::parityLetter(m_settings.parity)));
    }
    if (!m_port.setStopBits(m_settings.stopBits)) {
        return fail(tr("stop bits %1").arg(SerialSettings::stopBitsText(m_settings.stopBits)));
    }
    if (!m_port.setFlowControl(m_settings.flowControl)) {
        return fail(tr("flow control %1").arg(SerialSettings::flowControlText(m_settings.flowControl)));
    }
    return true;
}

// ---------------------------------------------------------------------------------------
// Built-in device simulator ("SIM:" pseudo-ports)
// ---------------------------------------------------------------------------------------

bool SerialConnection::openSimulator(bool quiet)
{
    const QString name = m_settings.portName;
    auto fail = [this, quiet](const QString& message) {
        m_errorString = message;
        if (quiet) {
            qCDebug(lcSerial) << "reconnect attempt failed:" << message;
        } else {
            qCWarning(lcSerial) << message;
            emit errorOccurred(message);
        }
        return false;
    };

    const std::optional<DeviceSimulator::Kind> kind = DeviceSimulator::kindFromPortName(name);
    if (!kind) {
        return fail(tr("Port %1 not found").arg(name));
    }
    if (quiet) {
        // Periodic reconnect attempt: the "rebooting" device is still absent.
        if (!DeviceSimulator::isPresent(name)) {
            return fail(tr("Port %1 not found").arg(name));
        }
    } else {
        // An explicit open by the user powers a "powered off" (poweroff / AT+RST) device back on.
        DeviceSimulator::markPresent(name);
    }

    m_simulator = new DeviceSimulator(*kind, m_settings.baudRate, this);
    connect(m_simulator, &DeviceSimulator::dataReady, this, &SerialConnection::handleIncoming);
    connect(m_simulator, &DeviceSimulator::vanished, this, [this](int returnsAfterMs) {
        qCWarning(lcSerial) << m_settings.portName << "simulated device went down"
                            << (returnsAfterMs < 0 ? QStringLiteral("until reopened")
                                                   : QStringLiteral("for %1 ms").arg(returnsAfterMs));
        closeSimulator();
        handleDeviceVanished();
    });

    m_errorString.clear();
    m_reconnectParamErrorReported = false;
    m_reconnectTimer.stop();
    qCInfo(lcSerial) << "opened" << name << m_settings.summary() << "(simulated device)";
    setState(State::Connected);
    emit pinsChanged(m_settings.dtr, m_settings.rts);
    m_simulator->start();
    return true;
}

void SerialConnection::closeSimulator()
{
    if (!m_simulator) {
        return;
    }
    disconnect(m_simulator, nullptr, this, nullptr);
    m_simulator->deleteLater();   // may be called from within one of its own signals
    m_simulator = nullptr;
}

void SerialConnection::handleDeviceVanished()
{
    const QString name = m_settings.portName;
    m_breakTimer.stop();
    if (m_port.isOpen()) {
        const qint64 dropped = m_port.bytesToWrite();
        if (dropped > 0) {
            qCWarning(lcSerial) << name << "closed with" << dropped << "unsent bytes discarded";
        }
        m_port.close();
    }
    // errorOccurred() first (generic, keeps the header contract that every port error is
    // forwarded), then portDisappeared() so its more specific status message wins in the UI.
    m_errorString = tr("Port %1 disconnected").arg(name);
    emit errorOccurred(m_errorString);
    emit portDisappeared(name);
    if (m_autoReconnect) {
        m_reconnectParamErrorReported = false;
        setState(State::Reconnecting);
        m_reconnectTimer.start();
        qCInfo(lcSerial) << "waiting for" << name << "to come back (every" << m_reconnectTimer.interval() << "ms)";
    } else {
        setState(State::Disconnected);
    }
}
