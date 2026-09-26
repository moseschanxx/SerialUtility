#include "core/Transport.h"

#include <QCoreApplication>
#include <algorithm>

Transport::Transport(QObject* parent)
    : QObject(parent)
{
}

Transport::~Transport() = default;

Transport::State Transport::state() const
{
    return m_state;
}

bool Transport::isOpen() const
{
    return m_state == State::Connected;
}

QString Transport::errorString() const
{
    return m_errorString;
}

quint64 Transport::bytesReceived() const
{
    return m_rx;
}

quint64 Transport::bytesSent() const
{
    return m_tx;
}

void Transport::resetCounters()
{
    m_rx = 0;
    m_tx = 0;
    emit countersChanged(0, 0);
}

qint64 Transport::pendingTxBytes() const
{
    return 0;
}

bool Transport::autoReconnect() const
{
    return m_autoReconnect;
}

void Transport::setAutoReconnect(bool on)
{
    m_autoReconnect = on;
    if (!on && m_state == State::Reconnecting) {
        setState(State::Disconnected);
    }
}

int Transport::reconnectIntervalMs() const
{
    return m_reconnectIntervalMs;
}

void Transport::setReconnectIntervalMs(int ms)
{
    m_reconnectIntervalMs = std::clamp(ms, 200, 60000);
}

QString Transport::stateText(State state)
{
    // The state strings keep the "SerialConnection" translation context: until v0.3.0 they were
    // SerialConnection::stateText()'s own tr() strings and translations/*.ts carry their
    // translations under that context, so a Chinese serial tab keeps showing the translated
    // state after the move into the Transport base class.
    switch (state) {
    case State::Disconnected:
        return QCoreApplication::translate("SerialConnection", "Disconnected");
    case State::Connecting:
        return QCoreApplication::translate("SerialConnection", "Connecting...");
    case State::Connected:
        return QCoreApplication::translate("SerialConnection", "Connected");
    case State::Reconnecting:
        return QCoreApplication::translate("SerialConnection", "Reconnecting...");
    }
    return QString();
}

QString Transport::kindText(Kind kind)
{
    switch (kind) {
    case Kind::Serial:
        return QCoreApplication::translate("Transport", "Serial");
    case Kind::Ssh:
        return QCoreApplication::translate("Transport", "SSH");
    }
    return QString();
}

void Transport::notifyTerminalSize(int /*cols*/, int /*rows*/)
{
}

void Transport::setState(State state)
{
    if (m_state == state) {
        return;
    }
    m_state = state;
    emit stateChanged(state);
}

void Transport::setErrorString(const QString& message)
{
    m_errorString = message;
}

void Transport::countReceived(qint64 bytes)
{
    if (bytes > 0) {
        m_rx += static_cast<quint64>(bytes);
        emit countersChanged(m_rx, m_tx);
    }
}

void Transport::countSent(qint64 bytes)
{
    if (bytes > 0) {
        m_tx += static_cast<quint64>(bytes);
        emit countersChanged(m_rx, m_tx);
    }
}
