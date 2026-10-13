#include "support/FakeMcu.h"

#include <QAbstractSocket>
#include <QPointer>
#include <QTcpSocket>
#include <QtEndian>

// FakeMcu: the CC1310 as the host sees it, ported from CC1310-Firmware tools/tests/test_adb_link.py
// (class FakeMcu) with the fault injection of the McuUpdater tests added (see FakeMcu.h).
// FakeMcuServer: the same file's BridgeServer, a TCP server standing in for `rfmcu bridge`.

namespace {

QByteArray le32(quint32 value)
{
    QByteArray bytes(4, '\0');
    qToLittleEndian<quint32>(value, bytes.data());
    return bytes;
}

/// int.from_bytes(bytes, "little") for up to 4 bytes (a short slice reads as zero-padded).
quint32 readLe32(const QByteArray& bytes, int offset = 0)
{
    QByteArray word = bytes.mid(offset, 4);
    word.append(QByteArray(4 - word.size(), '\0'));
    return qFromLittleEndian<quint32>(word.constData());
}

/// bytes.decode("ascii", "replace"): anything above 0x7F becomes U+FFFD.
QString decodeAscii(const QByteArray& bytes)
{
    QString text;
    text.reserve(bytes.size());
    for (char c : bytes) {
        const uchar byte = static_cast<uchar>(c);
        text.append(byte < 0x80 ? QChar(static_cast<ushort>(byte)) : QChar(0xFFFD));
    }
    return text;
}

} // namespace

struct FakeMcu::Private
{
    QString soc = QStringLiteral("na");
    QString supply = QStringLiteral("usb");
    quint32 role = 1;
    bool inUpdater = false;
    int dropFrames = 0;
    qint64 nackAt = -1;
    bool noBanner = false;
    bool refuseBootloader = false;
    bool silent = false;
    QByteArray received;      ///< bytes from the host not yet split into lines / frames
    QByteArray out;           ///< pending output
    QByteArray image;
    QStringList commands;
    int frames = 0;
    int dataFrames = 0;

    /// Python's FakeMcu prints "rx" for role 1 and "tx" for everything else.
    QByteArray roleText() const { return role == 1 ? "rx" : "tx"; }

    void say(const QByteArray& text)
    {
        if (!silent)
            out.append(text);
    }

    void reply(quint8 type, quint16 sequence, const QByteArray& payload = QByteArray())
    {
        if (!silent)
            out.append(CcProtocol::encodeFrame(type, sequence, payload));
    }

    void banner()
    {
        if (!noBanner)
            say("CC1310 boot version=0.8.1 role=" + roleText() + "\r\nOK cli=ready\r\n");
    }

    void handleLine(const QString& text)
    {
        // An empty line, or one carrying a NUL (the HELLO bytes the application cannot parse), is
        // swallowed without a trace.
        if (text.isEmpty() || text.contains(QChar(0)))
            return;
        commands.append(text);
        if (silent)
            return;
        if (text == QLatin1String("version")) {
            say("OK version=0.8.1 role=" + roleText() + "\r\n");
        } else if (text == QLatin1String("power")) {
            say("OK power state=on supply=" + supply.toLatin1() + " bat=ok usb="
                + (supply == QLatin1String("usb") ? "1" : "0") + " key=0 latch=1 soc=" + soc.toLatin1()
                + " charge=on\r\n");
        } else if (text == QLatin1String("bootloader")) {
            if (refuseBootloader) {
                say("ERR PWR busy\r\n");
            } else {
                say("OK rebooting_to_bootloader\r\n");
                inUpdater = true;
            }
        } else if (text == QLatin1String("help")) {
            // One "OK <name> <usage>" row per command, as the real firmware prints it (the role's
            // command word, rx or tx, is how a host learns the role when "version" carries none).
            say("OK commands:\r\n"
                "OK help [command]\r\n"
                "OK version show firmware version\r\n"
                "OK power power | power off [delay_ms] | power soc on|off\r\n"
                "OK bootloader reboot into UART firmware updater\r\n");
            say(role == 2 ? QByteArray("OK tx tx sync_frame <exposure_hex> | tx sync_time <utc_hex> | tx status\r\n")
                          : QByteArray("OK rx rx status | rx dump on|off\r\n"));
        } else {
            say("ERR CMD unknown_command; use help\r\n");
        }
    }

    void handleFrame(const CcProtocol::Frame& frame)
    {
        ++frames;
        if (frame.type == CcProtocol::Data)
            ++dataFrames;
        if (silent)
            return;
        const QByteArray& payload = frame.payload;
        switch (frame.type) {
        case CcProtocol::Hello: {
            const QByteArray info = le32(FakeMcu::kTarget) + le32(2) + le32(0x17000)
                + le32(static_cast<quint32>(image.size())) + le32(FakeMcu::kVersion) + le32(role);
            reply(CcProtocol::Info, frame.sequence, info);
            break;
        }
        case CcProtocol::Begin:
            image.clear();
            role = readLe32(payload, 32);
            reply(CcProtocol::Ready, frame.sequence, le32(0));
            break;
        case CcProtocol::Data:
            handleData(frame);
            break;
        case CcProtocol::End:
            reply(CcProtocol::Complete, frame.sequence);
            inUpdater = false;
            banner();
            break;
        case CcProtocol::SetRole:
            role = readLe32(payload);
            reply(CcProtocol::Complete, frame.sequence);
            inUpdater = false;
            banner();
            break;
        default:
            break;
        }
    }

    void handleData(const CcProtocol::Frame& frame)
    {
        const quint32 offset = readLe32(frame.payload);
        const QByteArray data = frame.payload.mid(4);
        if (dropFrames > 0) {                       // lost on the line: no effect, no answer
            --dropFrames;
            return;
        }
        if (nackAt >= 0 && static_cast<qint64>(offset) == nackAt) {
            nackAt = -1;                            // only the first DATA at that offset
            const qint64 resume = qMax<qint64>(0, static_cast<qint64>(offset) - data.size());
            image.truncate(static_cast<qsizetype>(resume));
            reply(CcProtocol::Nack, frame.sequence, le32(static_cast<quint32>(resume)));
            return;
        }
        // A stale offset is answered with the real one, which makes the host resume from there.
        if (static_cast<qint64>(offset) == image.size())
            image.append(data);
        reply(CcProtocol::Ack, frame.sequence, le32(static_cast<quint32>(image.size())));
    }
};

FakeMcu::FakeMcu(QObject* parent) : QObject(parent), d(new Private) {}
FakeMcu::~FakeMcu() = default;

void FakeMcu::setSoc(const QString& soc) { d->soc = soc; }
void FakeMcu::setSupply(const QString& supply) { d->supply = supply; }
void FakeMcu::setRole(quint32 role) { d->role = role; }
quint32 FakeMcu::role() const { return d->role; }
void FakeMcu::setInUpdater(bool on) { d->inUpdater = on; }
bool FakeMcu::inUpdater() const { return d->inUpdater; }
void FakeMcu::setDropFrames(int count) { d->dropFrames = count; }
void FakeMcu::setNackAt(qint64 offset) { d->nackAt = offset; }
void FakeMcu::setNoBanner(bool on) { d->noBanner = on; }
void FakeMcu::setRefuseBootloader(bool on) { d->refuseBootloader = on; }
void FakeMcu::setSilent(bool on) { d->silent = on; }

void FakeMcu::feed(const QByteArray& data)
{
    d->received.append(data);
    const qsizetype before = d->out.size();
    // The mode is looked at again for every unit, so bytes that follow "bootloader" or END in the
    // same write are read in the new mode.
    for (;;) {
        if (d->inUpdater) {
            const int zero = d->received.indexOf('\0');
            if (zero < 0)
                break;
            const QByteArray chunk = d->received.left(zero + 1);
            d->received.remove(0, zero + 1);
            CcProtocol::Frame frame;
            if (CcProtocol::decodeFrame(chunk, &frame))      // anything that does not decode is ignored
                d->handleFrame(frame);
        } else {
            const int newline = d->received.indexOf('\n');
            if (newline < 0)
                break;
            const QByteArray line = d->received.left(newline);
            d->received.remove(0, newline + 1);
            d->handleLine(decodeAscii(line).trimmed());
        }
    }
    if (d->out.size() != before)
        emit outputAvailable();
}

QByteArray FakeMcu::take(int size)
{
    const QByteArray bytes = d->out.left(size);
    d->out.remove(0, bytes.size());
    return bytes;
}

QByteArray FakeMcu::out() const { return d->out; }
bool FakeMcu::hasOutput() const { return !d->out.isEmpty(); }
QStringList FakeMcu::commands() const { return d->commands; }
QByteArray FakeMcu::image() const { return d->image; }
int FakeMcu::framesReceived() const { return d->frames; }
int FakeMcu::dataFramesReceived() const { return d->dataFrames; }

// ---- FakeMcuServer -----------------------------------------------------------------------

struct FakeMcuServer::Private
{
    explicit Private(FakeMcuServer* owner, FakeMcu* fake) : q(owner), mcu(fake) {}

    FakeMcuServer* q;
    FakeMcu* mcu;
    QTcpServer server;
    QPointer<QTcpSocket> client;
    int connections = 0;
    bool closing = false;      ///< the destructor is running: the sockets' last signals must not touch `server`

    void accept()
    {
        while (server.hasPendingConnections()) {
            QTcpSocket* socket = server.nextPendingConnection();
            if (client) {                           // one client at a time (the bridge's flock)
                socket->deleteLater();
                continue;
            }
            client = socket;
            ++connections;
            socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
            QObject::connect(socket, &QTcpSocket::readyRead, q, [this, socket] {
                mcu->feed(socket->readAll());
                flush();
            });
            QObject::connect(socket, &QTcpSocket::disconnected, q, [this, socket] {
                if (closing)
                    return;
                socket->deleteLater();
                if (client == socket)
                    client = nullptr;
                server.resumeAccepting();
                emit q->clientDisconnected();
            });
            server.pauseAccepting();                // later connections wait in the backlog
            emit q->clientConnected();
            flush();                                // output that was pending before the client came
        }
    }

    void flush()
    {
        while (client && client->state() == QAbstractSocket::ConnectedState && mcu->hasOutput())
            client->write(mcu->take(4096));
    }
};

FakeMcuServer::FakeMcuServer(FakeMcu* mcu, QObject* parent) : QObject(parent), d(new Private(this, mcu))
{
    connect(&d->server, &QTcpServer::newConnection, this, [this] { d->accept(); });
    connect(mcu, &FakeMcu::outputAvailable, this, [this] { d->flush(); });
}

FakeMcuServer::~FakeMcuServer()
{
    d->closing = true;
}

bool FakeMcuServer::listen() { return d->server.listen(QHostAddress::LocalHost, 0); }
quint16 FakeMcuServer::port() const { return d->server.serverPort(); }
FakeMcu* FakeMcuServer::mcu() const { return d->mcu; }
int FakeMcuServer::connectionCount() const { return d->connections; }
bool FakeMcuServer::hasClient() const { return d->client && d->client->state() == QAbstractSocket::ConnectedState; }

void FakeMcuServer::dropClient()
{
    if (d->client)
        d->client->disconnectFromHost();
}
