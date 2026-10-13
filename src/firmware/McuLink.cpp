#include "firmware/McuLink.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

#include <utility>

#include "app/Logging.h"

namespace {

constexpr int kBridgeEndGraceMs = 3000;   ///< close(): the bridge gets this long to end before the cleanup goes on
constexpr int kConnectTimeoutMs = 5000;   ///< the local forward accepts at once; this only catches a dead adb
constexpr int kMaxBridgeLines = 20;       ///< the bridge's own lines kept for the error messages

const QString kProbeCommand = QStringLiteral("command -v rfmcu || which rfmcu || true");
/// An rfmcu left by an earlier session (a killed task, an open console) holds the UART's flock.
const QString kStopRfmcuCommand = QStringLiteral("killall rfmcu 2>/dev/null; true");

} // namespace

/**
 * The state machine. One `generation` number is bumped by every open() and every close; the
 * callbacks of an adb command or of the bridge capture the generation they were started in and
 * do nothing when it has moved on, so a cancelled step can never advance a newer one.
 *
 * Failure and loss both go through beginClose(): the link is torn down in the order of adb_link.py
 * (socket, bridge, `killall rfmcu`, `forward --remove`), the reason signal (failed / linkLost) is
 * emitted at once and closed() follows from the event loop when the cleanup is done, so a slot of
 * the reason signal may call open() again (it is queued behind the cleanup).
 */
struct McuLink::Private
{
    enum class State { Closed, Selecting, CheckingRfmcu, StoppingStale, Forwarding, StartingBridge, Connecting, Open, Closing };
    enum class Reason { None, Failed, Lost };

    Private(McuLink* owner, ToolRunner* toolRunner) : q(owner), runner(toolRunner), adb(new AdbTool(toolRunner, owner))
    {
        bridgeTimer = new QTimer(owner);
        bridgeTimer->setSingleShot(true);
        QObject::connect(bridgeTimer, &QTimer::timeout, owner, [this] { onBridgeStartTimeout(); });
        connectTimer = new QTimer(owner);
        connectTimer->setSingleShot(true);
        QObject::connect(connectTimer, &QTimer::timeout, owner, [this] { onConnectTimeout(); });
        graceTimer = new QTimer(owner);
        graceTimer->setSingleShot(true);
        QObject::connect(graceTimer, &QTimer::timeout, owner, [this] { continueClose(); });
    }

    McuLink* q;
    QPointer<ToolRunner> runner;
    AdbTool* adb;
    QTimer* bridgeTimer = nullptr;
    QTimer* connectTimer = nullptr;
    QTimer* graceTimer = nullptr;

    State state = State::Closed;
    quint64 generation = 0;
    QString wantedSerial;
    QString selectedSerial;
    int bridgeStartTimeoutMs = 5000;
    quint16 localPort = 0;
    bool touchedBoard = false;       ///< the stale-rfmcu kill was sent: cleanup must repeat it
    bool openAfterClose = false;
    bool cleanupStarted = false;

    QPointer<ToolInvocation> step;
    QPointer<ToolInvocation> bridge;
    QPointer<ToolInvocation> closingBridge;
    QByteArray bridgeBuffer;
    QStringList bridgeLines;
    QPointer<QTcpSocket> socket;

    // ---- entry points --------------------------------------------------------------------

    bool open()
    {
        if (state == State::Closing) {
            if (openAfterClose) {
                return false;   // already queued
            }
            openAfterClose = true;   // the cleanup of the last link is still running
            return true;
        }
        if (state != State::Closed) {
            return false;
        }
        beginOpen();
        return true;
    }

    void close()
    {
        openAfterClose = false;   // an explicit close also withdraws a queued open()
        beginClose(Reason::None, QString());
    }

    bool isOpening() const
    {
        switch (state) {
        case State::Selecting:
        case State::CheckingRfmcu:
        case State::StoppingStale:
        case State::Forwarding:
        case State::StartingBridge:
        case State::Connecting:
            return true;
        case State::Closing:
            return openAfterClose;
        case State::Closed:
        case State::Open:
            break;
        }
        return false;
    }

    // ---- opening -------------------------------------------------------------------------

    template <typename Handler>
    void watch(ToolInvocation* invocation, Handler onFinished)
    {
        const quint64 gen = generation;
        step = invocation;
        QObject::connect(invocation, &ToolInvocation::finished, q,
                         [this, gen, callback = std::move(onFinished)](const ToolResult& result) {
                             if (gen != generation || result.cancelled) {
                                 return;   // close() moved on
                             }
                             callback(result);
                         });
    }

    /// Why an adb command did not do its job, empty when it did (adb_link.AdbLink._adb()).
    /// `checkExit` is Python's check=True: a non-zero exit is an error, otherwise only a command
    /// that could not run or timed out is.
    QString adbProblem(const QString& what, const ToolResult& result, bool checkExit) const
    {
        if (!result.started) {
            return QCoreApplication::translate("McuLink", "cannot run %1: %2").arg(adb->adbPath(), result.error);
        }
        if (result.timedOut) {
            return QCoreApplication::translate("McuLink", "adb %1 timed out after %2 s")
                .arg(what)
                .arg(adb->timeoutMs() / 1000);
        }
        if (checkExit && result.exitCode != 0) {
            const QString said = result.outputText().trimmed();
            return QCoreApplication::translate("McuLink", "adb %1 failed (exit %2): %3")
                .arg(what)
                .arg(result.exitCode)
                .arg(said.isEmpty() ? QCoreApplication::translate("McuLink", "no output") : said);
        }
        return QString();
    }

    void beginOpen()
    {
        ++generation;
        selectedSerial.clear();
        localPort = 0;
        touchedBoard = false;
        cleanupStarted = false;
        bridgeLines.clear();
        bridgeBuffer.clear();
        state = State::Selecting;
        if (!runner) {
            failOpen(QCoreApplication::translate("McuLink", "cannot run adb: the tool runner is gone"));
            return;
        }
        // `adb devices` lists every board whatever -s says; select() in adb_link.py clears it too.
        adb->setSerial(QString());
        watch(adb->devices(), [this](const ToolResult& result) { onDevices(result); });
    }

    void onDevices(const ToolResult& result)
    {
        const QString problem = adbProblem(QStringLiteral("devices"), result, true);
        if (!problem.isEmpty()) {
            failOpen(problem);
            return;
        }
        QString error;
        const QString picked = AdbTool::selectDevice(AdbTool::parseDevices(result.outputText()), wantedSerial, &error);
        if (picked.isEmpty()) {
            failOpen(error);
            return;
        }
        const quint64 gen = generation;
        selectedSerial = picked;
        adb->setSerial(picked);
        emit q->serialSelected(picked);
        if (gen != generation) {
            return;   // a slot closed the link
        }
        state = State::CheckingRfmcu;
        watch(adb->shell(kProbeCommand), [this](const ToolResult& r) { onProbe(r); });
    }

    void onProbe(const ToolResult& result)
    {
        const QString problem = adbProblem(QStringLiteral("shell ") + kProbeCommand, result, false);
        if (!problem.isEmpty()) {
            failOpen(problem);
            return;
        }
        if (!result.outputText().contains(QLatin1String("rfmcu"))) {
            failOpen(QCoreApplication::translate("McuLink", "board %1 has no rfmcu: %2")
                         .arg(selectedSerial, AdbTool::socFirmwareHint()));
            return;
        }
        state = State::StoppingStale;
        touchedBoard = true;
        watch(adb->shell(kStopRfmcuCommand), [this](const ToolResult& r) { onStaleStopped(r); });
    }

    void onStaleStopped(const ToolResult& result)
    {
        // Its exit status is ignored (nothing to kill is the usual case), a dead adb is not.
        const QString problem = adbProblem(QStringLiteral("shell ") + kStopRfmcuCommand, result, false);
        if (!problem.isEmpty()) {
            failOpen(problem);
            return;
        }
        state = State::Forwarding;
        watch(adb->forwardAnyPort(kRemotePort), [this](const ToolResult& r) { onForwarded(r); });
    }

    void onForwarded(const ToolResult& result)
    {
        const QString problem =
            adbProblem(QStringLiteral("forward tcp:0 tcp:%1").arg(kRemotePort), result, true);
        if (!problem.isEmpty()) {
            failOpen(problem);
            return;
        }
        const quint16 port = AdbTool::parseForwardedPort(result.outputText());
        if (port == 0) {
            failOpen(QCoreApplication::translate("McuLink", "adb forward did not report a local port (got '%1'); update adb")
                         .arg(result.outputText().trimmed()));
            return;
        }
        localPort = port;
        startBridge();
    }

    void startBridge()
    {
        state = State::StartingBridge;
        const quint64 gen = generation;
        ToolInvocation* invocation = adb->startBridge(kRemotePort);
        invocation->setAutoDelete(false);   // its output is watched; deleted in onBridgeFinished()
        bridge = invocation;
        QObject::connect(invocation, &ToolInvocation::outputReceived, q, [this, gen](const QByteArray& chunk) {
            if (gen == generation) {
                onBridgeOutput(chunk);
            }
        });
        QObject::connect(invocation, &ToolInvocation::finished, q, [this, gen, invocation](const ToolResult& result) {
            onBridgeFinished(gen, invocation, result);
        });
        bridgeTimer->start(bridgeStartTimeoutMs);
    }

    void onBridgeOutput(const QByteArray& chunk)
    {
        bridgeBuffer.append(chunk);
        qsizetype start = 0;
        for (qsizetype i = 0; i < bridgeBuffer.size(); ++i) {
            const char c = bridgeBuffer.at(i);
            if (c == '\n' || c == '\r') {
                takeBridgeLine(bridgeBuffer.mid(start, i - start));
                start = i + 1;
            }
        }
        bridgeBuffer.remove(0, start);
    }

    void takeBridgeLine(const QByteArray& raw)
    {
        const QString line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty()) {
            return;
        }
        qCDebug(lcFirmware).noquote() << "rfmcu bridge:" << line;
        bridgeLines << line;
        while (bridgeLines.size() > kMaxBridgeLines) {
            bridgeLines.removeFirst();
        }
        if (state == State::StartingBridge && line.contains(QLatin1String("bridging"))) {
            bridgeTimer->stop();
            startConnecting();
        }
    }

    QString bridgeSaid() const
    {
        return bridgeLines.isEmpty() ? QCoreApplication::translate("McuLink", "no output")
                                     : bridgeLines.join(QStringLiteral("; "));
    }

    void onBridgeStartTimeout()
    {
        if (state == State::StartingBridge) {
            failOpen(QCoreApplication::translate("McuLink", "rfmcu bridge did not start on board %1: %2")
                         .arg(selectedSerial, bridgeSaid()));
        }
    }

    void onBridgeFinished(quint64 gen, ToolInvocation* invocation, const ToolResult& result)
    {
        invocation->deleteLater();   // autoDelete was switched off for the output watch
        if (gen != generation) {
            if (state == State::Closing && invocation == closingBridge.data()) {
                continueClose();   // the bridge we waited for is gone
            }
            return;
        }
        if (!bridgeBuffer.isEmpty()) {
            takeBridgeLine(bridgeBuffer);   // a last line without its newline
            bridgeBuffer.clear();
        }
        bridgeTimer->stop();
        switch (state) {
        case State::StartingBridge:
        case State::Connecting:
            failOpen(QCoreApplication::translate("McuLink", "rfmcu bridge did not start on board %1: %2")
                         .arg(selectedSerial, bridgeSaid()));
            break;
        case State::Open:
            lose(QCoreApplication::translate("McuLink", "the rfmcu bridge on board %1 ended (exit %2): %3")
                     .arg(selectedSerial)
                     .arg(result.exitCode)
                     .arg(bridgeSaid()));
            break;
        default:
            break;
        }
    }

    void startConnecting()
    {
        state = State::Connecting;
        const quint64 gen = generation;
        auto* tcp = new QTcpSocket(q);   // owned by the link; deleted after closed()
        socket = tcp;
        QObject::connect(tcp, &QAbstractSocket::connected, q, [this, gen] {
            if (gen == generation && state == State::Connecting) {
                onConnected();
            }
        });
        QObject::connect(tcp, &QAbstractSocket::errorOccurred, q, [this, gen, tcp] {
            if (gen != generation) {
                return;
            }
            if (state == State::Connecting) {
                failOpen(QCoreApplication::translate("McuLink", "cannot connect to the rfmcu bridge on 127.0.0.1:%1: %2")
                             .arg(localPort)
                             .arg(tcp->errorString()));
            }
        });
        QObject::connect(tcp, &QAbstractSocket::disconnected, q, [this, gen] {
            if (gen == generation && state == State::Open) {
                lose(QCoreApplication::translate("McuLink", "the connection to the rfmcu bridge on board %1 was closed")
                         .arg(selectedSerial));
            }
        });
        connectTimer->start(kConnectTimeoutMs);
        tcp->connectToHost(QHostAddress(QHostAddress::LocalHost), localPort);
    }

    void onConnectTimeout()
    {
        if (state == State::Connecting) {
            failOpen(QCoreApplication::translate("McuLink", "cannot connect to the rfmcu bridge on 127.0.0.1:%1: %2")
                         .arg(localPort)
                         .arg(QCoreApplication::translate("McuLink", "timed out")));
        }
    }

    void onConnected()
    {
        connectTimer->stop();
        if (socket) {
            socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);   // rfmcu sets TCP_NODELAY too
        }
        state = State::Open;
        const quint64 gen = generation;
        const QString board = selectedSerial;
        const quint16 port = localPort;
        emit q->opened(board, port);
        if (gen != generation) {
            return;
        }
        emit q->logLine(QStringLiteral("INFO adb board=%1 bridge=socket://127.0.0.1:%2 (rfmcu bridge, /dev/ttyS1)")
                            .arg(board)
                            .arg(port));
    }

    // ---- failing, losing, closing --------------------------------------------------------

    void failOpen(const QString& message)
    {
        qCWarning(lcFirmware).noquote() << "mcu link: open failed:" << message;
        beginClose(Reason::Failed, message);
    }

    void lose(const QString& message)
    {
        qCWarning(lcFirmware).noquote() << "mcu link: link lost:" << message;
        beginClose(Reason::Lost, message);
    }

    void beginClose(Reason reason, const QString& message)
    {
        if (state == State::Closed || state == State::Closing) {
            return;
        }
        state = State::Closing;
        ++generation;   // every callback of the open sequence is stale from here on
        cleanupStarted = false;
        bridgeTimer->stop();
        connectTimer->stop();
        if (step && !step->isFinished()) {
            step->cancel();
        }
        step.clear();
        if (socket) {
            // The object stays until closed() (a McuUpdater may still hold the pointer); the
            // connection goes now, and the signals of this socket are stale (generation).
            socket->abort();
        }
        closingBridge = bridge;
        bridge.clear();
        if (closingBridge && !closingBridge->isFinished()) {
            closingBridge->cancel();
            graceTimer->start(kBridgeEndGraceMs);   // terminate, kill: do not wait for ever
        } else {
            closingBridge.clear();
            QTimer::singleShot(0, q, [this, gen = generation] {
                if (gen == generation && state == State::Closing) {
                    continueClose();
                }
            });
        }

        // The reason last: a slot may call open() (queued), close() (no-op) or delete the link.
        switch (reason) {
        case Reason::Failed: emit q->failed(message); break;
        case Reason::Lost: emit q->linkLost(message); break;
        case Reason::None: break;
        }
    }

    /// The bridge is gone (or given up on): stop the board's rfmcu and drop the forward, errors ignored.
    void continueClose()
    {
        if (state != State::Closing || cleanupStarted) {
            return;
        }
        cleanupStarted = true;
        graceTimer->stop();
        closingBridge.clear();
        const quint64 gen = generation;
        if (!runner || selectedSerial.isEmpty() || (!touchedBoard && localPort == 0)) {
            finishClose();
            return;
        }
        adb->setSerial(selectedSerial);
        if (touchedBoard) {
            // adbd does not reliably end the board process when the host side goes away.
            ToolInvocation* stop = adb->shell(kStopRfmcuCommand);
            QObject::connect(stop, &ToolInvocation::finished, q, [this, gen] {
                if (gen == generation && state == State::Closing) {
                    removeForward();
                }
            });
        } else {
            removeForward();
        }
    }

    void removeForward()
    {
        if (localPort == 0 || !runner) {
            finishClose();
            return;
        }
        const quint64 gen = generation;
        ToolInvocation* remove = adb->removeForward(localPort);
        QObject::connect(remove, &ToolInvocation::finished, q, [this, gen] {
            if (gen == generation && state == State::Closing) {
                finishClose();
            }
        });
    }

    void finishClose()
    {
        state = State::Closed;
        localPort = 0;
        touchedBoard = false;
        if (socket) {
            socket->deleteLater();
            socket.clear();
        }
        adb->setSerial(wantedSerial);
        emit q->closed();
        if (openAfterClose && state == State::Closed) {
            openAfterClose = false;
            beginOpen();
        }
    }

    /// The destructor cannot wait for the asynchronous cleanup: end the bridge and fire the two
    /// cleanup commands detached (ToolRunner::Options::detached). They must outlive the runner
    /// too: the link is destroyed while its owner goes away (the Firmware tab at quit), the
    /// runner right after it, and a runner's process invocations are killed with it - a killed
    /// `killall rfmcu` leaves the board's bridge holding the UART lock and the forward in place.
    void abandon()
    {
        ++generation;
        bridgeTimer->stop();
        connectTimer->stop();
        graceTimer->stop();
        if (step && !step->isFinished()) {
            step->cancel();
        }
        // The bridge (and one that close() was still waiting for) was kept alive for its output; with
        // no link left to delete it, it must delete itself once it ends - or now, when it already has.
        for (const QPointer<ToolInvocation>& pending : {bridge, closingBridge}) {
            if (!pending) {
                continue;
            }
            if (pending->isFinished()) {
                pending->deleteLater();
            } else {
                pending->setAutoDelete(true);
                pending->cancel();
            }
        }
        if (socket) {
            socket->abort();
        }
        if (runner && !selectedSerial.isEmpty() && state != State::Closed) {
            // The same command lines as AdbTool::shell() / removeForward(), with the serial.
            ToolRunner::Options detached;
            detached.detached = true;
            const QStringList board{QStringLiteral("-s"), selectedSerial};
            if (touchedBoard) {
                runner->run(adb->adbPath(), board + QStringList{QStringLiteral("shell"), kStopRfmcuCommand}, detached);
            }
            if (localPort != 0) {
                runner->run(adb->adbPath(),
                            board + QStringList{QStringLiteral("forward"), QStringLiteral("--remove"),
                                                QStringLiteral("tcp:%1").arg(localPort)},
                            detached);
            }
        }
        state = State::Closed;
    }
};

// ---------------------------------------------------------------------------------------------
// McuLink
// ---------------------------------------------------------------------------------------------

McuLink::McuLink(ToolRunner* runner, QObject* parent) : QObject(parent), d(new Private(this, runner)) {}

McuLink::~McuLink()
{
    if (d->state != Private::State::Closed) {
        d->abandon();
    }
}

AdbTool* McuLink::adb() const { return d->adb; }

void McuLink::setSerial(const QString& serial)
{
    d->wantedSerial = serial;
    if (d->state == Private::State::Closed) {
        d->adb->setSerial(serial);   // so adb() and cliCommand() agree with it
    }
}

QString McuLink::serial() const
{
    return d->selectedSerial.isEmpty() ? d->wantedSerial : d->selectedSerial;
}

void McuLink::setBridgeStartTimeoutMs(int ms) { d->bridgeStartTimeoutMs = ms; }
int McuLink::bridgeStartTimeoutMs() const { return d->bridgeStartTimeoutMs; }
bool McuLink::isOpen() const { return d->state == Private::State::Open; }
bool McuLink::isOpening() const { return d->isOpening(); }
quint16 McuLink::localPort() const { return d->localPort; }
QTcpSocket* McuLink::socket() const { return d->state == Private::State::Open ? d->socket.data() : nullptr; }

ToolInvocation* McuLink::cliCommand(const QString& text)
{
    d->adb->setSerial(serial());
    return d->adb->shell(QStringLiteral("rfmcu cmd ") + text);
}

bool McuLink::open() { return d->open(); }
void McuLink::close() { d->close(); }
