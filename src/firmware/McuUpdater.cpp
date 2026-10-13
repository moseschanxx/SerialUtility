#include "firmware/McuUpdater.h"

#include "app/Logging.h"

#include <QAbstractSocket>
#include <QIODevice>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>
#include <QtEndian>
#include <utility>

// The CC1310 updater (see McuUpdater.h), a port of CC1310-Firmware/tools/fw_update.py class Updater
// plus the INFO / PROGRESS / COMPLETE lines of run_on() and report_restart().
//
// The Python code is a chain of blocking calls (write, then read until a deadline). Here every one of
// those waits is a "step" of one state machine: a step writes its request, arms the single QTimer
// with the step's deadline and returns to the event loop; readyRead() feeds the bytes into one
// receive buffer, which is consumed according to the current step (frames up to the next 0x00 in the
// bootloader steps, lines up to the next '\n' in the CLI steps), and the QTimer firing means the
// deadline passed. The step table:
//
//   HelloProbe       HELLO x probeRetries                      -> INFO, or the application probe
//   AppClear         "\r\n", 300 ms (flushes HELLO bytes the application's line editor holds)
//   AppVersion       "version", cliTimeoutMs
//   AppHelp          "help", cliTimeoutMs (only when "version" gave no role=)
//   Power            "power", cliTimeoutMs (only through the SoC)
//   BootloaderCmd    "bootloader", bootloaderCommandTimeoutMs
//   Settle           resetSettleMs, then the input is discarded
//   HelloReboot      HELLO x rebootRetries                     -> INFO
//   Begin            BEGIN -> READY          Transfer  DATA -> ACK/NACK        End  END -> COMPLETE
//   SetRoleRequest   SET_ROLE -> COMPLETE
//   Banner           lines until "cli=ready" or bannerTimeoutMs
//
// A CLI step always listens for its whole window, like Python's _iter_lines(): the window's lines
// are evaluated when it ends. Only the banner wait ends early.
//
// A frame that is not the awaited answer (another sequence, an unexpected type) is ignored and the
// step keeps waiting for its deadline; Python's request() instead counts it as a spent attempt and
// sends the request again at once. The header's contract says "ignored"; the two differ only while
// stray frames are on the line (here they cost nothing, there they use up retries).
//
// Every signal is emitted through a helper that reports whether the operation is still the same one
// afterwards, because a slot may cancel() or start a new operation from inside a signal.

namespace {

constexpr int kProbeClearMs = 300;           // fw_update.py: self._command("", 0.3)
constexpr int kMaxPendingBytes = 70000;      // a frame never gets this long without a zero: noise

QByteArray le32(quint32 value)
{
    QByteArray bytes(4, '\0');
    qToLittleEndian<quint32>(value, bytes.data());
    return bytes;
}

quint32 readLe32(const QByteArray& bytes)
{
    return qFromLittleEndian<quint32>(bytes.constData());   // callers check the size first
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

QStringList splitWords(const QString& line)
{
    return line.simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

void mergeFields(McuUpdater::CliFields& into, const McuUpdater::CliFields& from)
{
    for (auto it = from.constBegin(); it != from.constEnd(); ++it)
        into.insert(it.key(), it.value());
}

const char* operationName(McuUpdater::Operation operation)
{
    switch (operation) {
    case McuUpdater::Operation::Info:    return "info";
    case McuUpdater::Operation::Flash:   return "flash";
    case McuUpdater::Operation::SetRole: return "set-role";
    case McuUpdater::Operation::None:    break;
    }
    return "none";
}

} // namespace

struct McuUpdater::Private
{
    enum class Step {
        Idle, HelloProbe, AppClear, AppVersion, AppHelp, Power, BootloaderCmd, Settle, HelloReboot,
        Begin, Transfer, End, SetRoleRequest, Banner, Done
    };
    enum class Mode { Discard, Frames, Text };

    explicit Private(McuUpdater* owner) : q(owner)
    {
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, owner, [this] { onTimeout(); });
    }

    McuUpdater* q;

    // ---- configuration --------------------------------------------------------------------
    QPointer<QIODevice> device;
    QList<QMetaObject::Connection> deviceConnections;
    bool throughSoc = false;
    bool autoBootloader = true;
    CcPackage package;
    int frameTimeoutMs = 500;
    int frameRetries = 5;
    int probeRetries = 2;
    int rebootRetries = 10;
    int chunkSize = CcProtocol::kDataChunkSize;
    int cliTimeoutMs = 1000;
    int bootloaderCommandTimeoutMs = 2000;
    int resetSettleMs = 500;
    int bannerTimeoutMs = 8000;

    // ---- the running operation ------------------------------------------------------------
    bool running = false;
    Operation operation = Operation::None;     // the running one, or the last one that ran
    Phase phase = Phase::Idle;
    Step step = Step::Idle;
    quint32 role = 0;
    quint16 sequence = 0;
    quint64 generation = 0;                    // bumped per operation: stale callbacks see a mismatch
    QTimer timer;                              // the deadline of the current step
    QByteArray rx;                             // received, not yet consumed
    int invalidFrames = 0;

    // a framed request in flight (Updater.request())
    quint8 requestType = 0;
    QByteArray wire;
    QList<quint8> expected;
    int attempts = 0;
    int maxAttempts = 0;

    // a CLI window in flight, and what the steps collected
    QStringList lines;
    CliFields appFields;
    CliFields bannerFields;
    qint64 offset = 0;                         // next image byte the device expects

    // ---- plumbing -------------------------------------------------------------------------
    static Phase phaseOf(Step s)
    {
        switch (s) {
        case Step::Idle:           return Phase::Idle;
        case Step::HelloProbe:     return Phase::ProbeBootloader;
        case Step::AppClear:
        case Step::AppVersion:
        case Step::AppHelp:        return Phase::ProbeApplication;
        case Step::Power:          return Phase::ReadPower;
        case Step::BootloaderCmd:  return Phase::EnterBootloader;
        case Step::Settle:         return Phase::SettleAfterReset;
        case Step::HelloReboot:    return Phase::RebootProbe;
        case Step::Begin:          return Phase::Begin;
        case Step::Transfer:       return Phase::Transfer;
        case Step::End:            return Phase::End;
        case Step::SetRoleRequest: return Phase::SetRole;
        case Step::Banner:         return Phase::Banner;
        case Step::Done:           return Phase::Done;
        }
        return Phase::Idle;
    }

    static Mode modeOf(Step s)
    {
        switch (s) {
        case Step::HelloProbe:
        case Step::HelloReboot:
        case Step::Begin:
        case Step::Transfer:
        case Step::End:
        case Step::SetRoleRequest: return Mode::Frames;
        case Step::AppClear:
        case Step::AppVersion:
        case Step::AppHelp:
        case Step::Power:
        case Step::BootloaderCmd:
        case Step::Banner:         return Mode::Text;
        case Step::Idle:
        case Step::Settle:
        case Step::Done:           break;
        }
        return Mode::Discard;
    }

    bool alive(quint64 gen) const { return running && gen == generation; }

    /// Enter a step; false when a slot ended the operation from inside phaseChanged().
    bool setStep(Step s)
    {
        step = s;
        const Phase next = phaseOf(s);
        if (next == phase)
            return true;
        phase = next;
        const quint64 gen = generation;
        emit q->phaseChanged(next);
        return alive(gen);
    }

    /// Emit logLine(); false when the operation was cancelled by a slot meanwhile.
    bool log(const QString& line)
    {
        const quint64 gen = generation;
        emit q->logLine(line);
        return alive(gen);
    }

    void finish(bool ok, const QString& message)
    {
        if (!running)
            return;
        running = false;
        timer.stop();
        rx.clear();
        lines.clear();
        step = Step::Done;
        qCInfo(lcFirmware) << "MCU updater" << operationName(operation) << (ok ? "finished:" : "failed:") << message;
        if (phase != Phase::Done) {
            phase = Phase::Done;
            emit q->phaseChanged(Phase::Done);
        }
        emit q->finished(ok, message);
    }

    bool writeBytes(const QByteArray& bytes)
    {
        QIODevice* dev = device;
        if (!dev || !dev->isOpen() || !dev->isWritable() || dev->write(bytes) != bytes.size()) {
            qCWarning(lcFirmware) << "write to the MCU link failed:" << (dev ? dev->errorString() : QString());
            finish(false, McuUpdater::tr("connection closed"));
            return false;
        }
        if (auto* socket = qobject_cast<QAbstractSocket*>(dev))
            socket->flush();
        return true;
    }

    // ---- starting an operation ------------------------------------------------------------
    bool startOperation(Operation op, quint32 roleValue)
    {
        if (running) {
            qCWarning(lcFirmware) << "MCU updater: an operation is already running";
            return false;
        }
        if (!device || !device->isOpen()) {
            qCWarning(lcFirmware) << "MCU updater: no open device";
            return false;
        }
        if (op == Operation::Flash && !package.isValid()) {
            qCWarning(lcFirmware) << "MCU updater: no valid package set";
            return false;
        }
        operation = op;
        role = roleValue;
        running = true;
        ++generation;
        rx.clear();
        lines.clear();
        appFields.clear();
        bannerFields.clear();
        offset = 0;
        attempts = 0;
        invalidFrames = 0;
        device->readAll();                     // bytes left over from an earlier exchange
        qCInfo(lcFirmware) << "MCU updater starts" << operationName(op) << "role" << roleValue
                           << "throughSoc" << throughSoc << "autoBootloader" << autoBootloader;
        const quint64 gen = generation;
        if (!setStep(Step::HelloProbe))
            return true;
        // The first write happens from the event loop so that a failing device ends the operation
        // after start() returned, never from inside it.
        QMetaObject::invokeMethod(q, [this, gen] { if (alive(gen)) begin(); }, Qt::QueuedConnection);
        return true;
    }

    void begin()
    {
        if (operation == Operation::Flash && !log(package.summary()))
            return;
        sendHello(probeRetries);
    }

    // ---- framed requests (Updater.request()) -----------------------------------------------
    void sendHello(int retries)
    {
        startRequest(CcProtocol::Hello, QByteArray(), {CcProtocol::Info}, retries);
    }

    void startRequest(quint8 type, const QByteArray& payload, const QList<quint8>& accepted, int retries)
    {
        sequence = static_cast<quint16>(sequence + 1);          // 16-bit wrap
        requestType = type;
        wire = CcProtocol::encodeFrame(type, sequence, payload);
        expected = accepted;
        maxAttempts = qMax(1, retries);
        attempts = 0;
        sendAttempt();
    }

    void sendAttempt()
    {
        ++attempts;
        if (!writeBytes(wire))
            return;
        timer.start(frameTimeoutMs);
    }

    void requestTimedOut()
    {
        rx.clear();                            // half a frame that never ended is lost, as in _read_frame()
        if (attempts < maxAttempts) {
            qCDebug(lcFirmware) << "no answer to" << CcProtocol::messageTypeName(requestType)
                                << "sequence" << sequence << "attempt" << attempts << "of" << maxAttempts;
            sendAttempt();
            return;
        }
        requestFailed();
    }

    void requestFailed()
    {
        if (step == Step::HelloProbe) {
            // Updater.connect(): the bootloader is silent; `info` and the auto-bootloader path go on
            // with the application CLI.
            if (operation != Operation::Info && !autoBootloader) {
                finish(false, McuUpdater::tr("no bootloader response; put the device in UART update mode "
                                             "(CLI command 'bootloader') or drop --no-auto-bootloader"));
                return;
            }
            startApplicationProbe();
            return;
        }
        finish(false, McuUpdater::tr("no acceptable response after %1 retries").arg(maxAttempts));
    }

    // ---- input ----------------------------------------------------------------------------
    void onReadyRead()
    {
        if (!device)
            return;
        const QByteArray data = device->readAll();
        if (data.isEmpty() || !running)        // nobody is waiting for it
            return;
        rx.append(data);
        pump();
    }

    void onDeviceClosed()
    {
        if (!running)
            return;
        if (device && device->isOpen() && device->isReadable()) {
            const QByteArray rest = device->readAll();
            if (!rest.isEmpty()) {
                rx.append(rest);
                pump();
            }
        }
        if (running)
            finish(false, McuUpdater::tr("connection closed"));
    }

    /// Consume `rx` according to the current step. A step change inside the loop (the COMPLETE frame
    /// followed by the banner text in the same read) simply changes how the rest is consumed.
    void pump()
    {
        const quint64 gen = generation;
        while (alive(gen) && !rx.isEmpty()) {
            switch (modeOf(step)) {
            case Mode::Discard:
                rx.clear();
                return;
            case Mode::Frames: {
                const int zero = rx.indexOf('\0');
                if (zero < 0) {
                    if (rx.size() > kMaxPendingBytes) {
                        rx.clear();
                        ++invalidFrames;
                    }
                    return;
                }
                const QByteArray chunk = rx.left(zero + 1);
                rx.remove(0, zero + 1);
                if (chunk.size() == 1)         // an idle zero
                    break;
                CcProtocol::Frame frame;
                QString reason;
                if (!CcProtocol::decodeFrame(chunk, &frame, &reason)) {
                    ++invalidFrames;           // line noise before a reset: dropped, reading goes on
                    qCDebug(lcFirmware) << "dropped a bad frame:" << reason;
                    break;
                }
                onFrame(frame);
                break;
            }
            case Mode::Text: {
                const int newline = rx.indexOf('\n');
                if (newline < 0)
                    return;
                const QString line = decodeAscii(rx.left(newline)).trimmed();
                rx.remove(0, newline + 1);
                if (!line.isEmpty())
                    onLine(line);
                break;
            }
            }
        }
    }

    void onLine(const QString& line)
    {
        if (step == Step::Banner)
            bannerLine(line);
        else
            lines.append(line);
    }

    /// The text that stopped short of a newline when a window closed is its last line.
    void flushPartial()
    {
        if (rx.isEmpty())
            return;
        const QString text = decodeAscii(rx).trimmed();
        rx.clear();
        if (!text.isEmpty())
            onLine(text);
    }

    // ---- frame answers --------------------------------------------------------------------
    void onFrame(const CcProtocol::Frame& frame)
    {
        if (frame.sequence != sequence || !expected.contains(frame.type)) {
            qCDebug(lcFirmware) << "ignoring" << CcProtocol::messageTypeName(frame.type)
                                << "sequence" << frame.sequence << "(waiting for" << sequence << ")";
            return;
        }
        timer.stop();
        switch (step) {
        case Step::HelloProbe:     onInfo(frame.payload); break;
        case Step::HelloReboot:    onInfo(frame.payload); break;
        case Step::Begin:          onReady(frame.payload); break;
        case Step::Transfer:       onDataAnswer(frame); break;
        case Step::End:
        case Step::SetRoleRequest: onComplete(); break;
        default:                   break;
        }
    }

    void onInfo(const QByteArray& payload)
    {
        CcProtocol::DeviceInfo info;
        if (!CcProtocol::parseInfo(payload, &info)) {
            finish(false, McuUpdater::tr("invalid INFO response"));
            return;
        }
        const quint64 gen = generation;
        emit q->deviceInfo(info);
        if (!alive(gen))
            return;
        if (operation == Operation::Info) {
            const QString text = info.toString();
            if (log(text))
                finish(true, text);
            return;
        }
        connected(info);
    }

    /// The bootloader answered (directly, or after the reboot): the operation proper starts.
    void connected(const CcProtocol::DeviceInfo& info)
    {
        qCInfo(lcFirmware) << "bootloader answered:" << info.toString();
        if (operation == Operation::Flash) {
            if (info.target != package.target) {
                finish(false, McuUpdater::tr("package target does not match device"));
                return;
            }
            if (package.size > info.maxSize) {
                finish(false, McuUpdater::tr("package exceeds device App slot"));
                return;
            }
            if (!setStep(Step::Begin))
                return;
            startRequest(CcProtocol::Begin, package.header + le32(role), {CcProtocol::Ready}, frameRetries);
        } else {
            if (!setStep(Step::SetRoleRequest))
                return;
            startRequest(CcProtocol::SetRole, le32(role), {CcProtocol::Complete}, frameRetries);
        }
    }

    void onReady(const QByteArray& payload)
    {
        if (payload.size() != 4) {
            finish(false, McuUpdater::tr("invalid READY response"));
            return;
        }
        if (readLe32(payload) != 0) {
            finish(false, McuUpdater::tr("bootloader requested unsupported resume offset"));
            return;
        }
        offset = 0;
        if (setStep(Step::Transfer))
            sendData();
    }

    void sendData()
    {
        const QByteArray block = package.image.mid(static_cast<qsizetype>(offset), chunkSize);
        startRequest(CcProtocol::Data, le32(static_cast<quint32>(offset)) + block,
                     {CcProtocol::Ack, CcProtocol::Nack}, frameRetries);
    }

    void onDataAnswer(const CcProtocol::Frame& frame)
    {
        if (frame.payload.size() != 4) {
            finish(false, McuUpdater::tr("invalid ACK/NACK response"));
            return;
        }
        const quint32 next = readLe32(frame.payload);
        if (frame.type == CcProtocol::Nack && next > offset) {
            finish(false, McuUpdater::tr("invalid NACK offset"));
            return;
        }
        offset = next;                         // ACK: the next offset; NACK: resume from there
        const qint64 total = package.image.size();
        const quint64 gen = generation;
        emit q->progress(qMin<qint64>(offset, total), total);
        if (!alive(gen))
            return;
        if (offset < total) {
            sendData();
            return;
        }
        if (setStep(Step::End))
            startRequest(CcProtocol::End, QByteArray(), {CcProtocol::Complete}, frameRetries);
    }

    void onComplete()
    {
        if (!log(QStringLiteral("COMPLETE")))
            return;
        startBanner();
    }

    // ---- restart banner (wait_for_application) ---------------------------------------------
    void startBanner()
    {
        bannerFields.clear();
        lines.clear();
        if (!setStep(Step::Banner))
            return;
        timer.start(bannerTimeoutMs);          // the leftover bytes of this read are consumed by pump()
    }

    void bannerLine(const QString& line)
    {
        if (line.contains(QLatin1String("version=")))
            mergeFields(bannerFields, McuUpdater::parseCliFields(line));
        else if (line.contains(QLatin1String("cli=ready")) && !bannerFields.isEmpty())
            finishBanner();
    }

    void finishBanner()
    {
        timer.stop();
        const quint64 gen = generation;
        if (!bannerFields.isEmpty()) {
            const CliFields fields = bannerFields;
            emit q->applicationRestarted(fields);
            if (!alive(gen))
                return;
            if (!log(QStringLiteral("INFO application restarted: version=%1 role=%2")
                         .arg(fields.value(QStringLiteral("version"), QStringLiteral("unknown")),
                              fields.value(QStringLiteral("role"), QStringLiteral("unknown")))))
                return;
        } else {
            if (!log(QStringLiteral("INFO application banner not seen within %1 s; check the device manually")
                         .arg(QString::number(bannerTimeoutMs / 1000.0, 'f', 0))))
                return;
            emit q->bannerTimeout();
            if (!alive(gen))
                return;
        }
        finish(true, QStringLiteral("COMPLETE"));
    }

    // ---- the application CLI --------------------------------------------------------------
    /// Send `command` + CRLF and listen for `ms` (Updater._command()).
    void startWindow(const QByteArray& command, int ms)
    {
        lines.clear();
        if (!writeBytes(command + "\r\n"))
            return;
        timer.start(ms);
    }

    void startApplicationProbe()
    {
        appFields.clear();
        if (!setStep(Step::AppClear))
            return;
        rx.clear();
        // An empty line first: it terminates whatever partial line the CLI holds (the HELLO bytes).
        startWindow(QByteArray(), kProbeClearMs);
    }

    void applicationMissing()
    {
        if (operation == Operation::Info)
            finish(false, McuUpdater::tr("no acceptable response after %1 retries").arg(maxAttempts));
        else
            finish(false, McuUpdater::tr("neither the bootloader nor the application CLI answered on this port"));
    }

    void applicationFound()
    {
        const quint64 gen = generation;
        const CliFields fields = appFields;
        emit q->applicationInfo(fields);
        if (!alive(gen))
            return;
        if (operation == Operation::Info) {
            const QString text = McuUpdater::formatApplication(fields)
                + QStringLiteral(" state=running (bootloader inactive; 'flash' and 'set-role' reboot into it automatically)");
            if (log(text))
                finish(true, text);
            return;
        }
        if (throughSoc) {
            if (setStep(Step::Power))
                startWindow("power", cliTimeoutMs);
            return;
        }
        enterBootloader();
    }

    void enterBootloader()
    {
        if (!log(McuUpdater::formatApplication(appFields) + QStringLiteral("; rebooting it into the UART updater")))
            return;
        if (setStep(Step::BootloaderCmd))
            startWindow("bootloader", bootloaderCommandTimeoutMs);
    }

    // ---- deadlines ------------------------------------------------------------------------
    void onTimeout()
    {
        if (!running)
            return;
        switch (step) {
        case Step::HelloProbe:
        case Step::HelloReboot:
        case Step::Begin:
        case Step::Transfer:
        case Step::End:
        case Step::SetRoleRequest:
            requestTimedOut();
            break;
        case Step::AppClear:
            flushPartial();
            if (running && setStep(Step::AppVersion))
                startWindow("version", cliTimeoutMs);
            break;
        case Step::AppVersion:
            flushPartial();
            if (running)
                versionWindowEnded();
            break;
        case Step::AppHelp:
            flushPartial();
            if (running)
                helpWindowEnded();
            break;
        case Step::Power:
            flushPartial();
            if (running)
                powerWindowEnded();
            break;
        case Step::BootloaderCmd:
            flushPartial();
            if (running)
                bootloaderWindowEnded();
            break;
        case Step::Settle:
            if (device)
                device->readAll();             // ser.reset_input_buffer()
            rx.clear();
            if (setStep(Step::HelloReboot))
                sendHello(rebootRetries);
            break;
        case Step::Banner:
            flushPartial();                    // the banner may be the last, unterminated line
            if (running)
                finishBanner();
            break;
        case Step::Idle:
        case Step::Done:
            break;
        }
    }

    void versionWindowEnded()
    {
        for (const QString& line : std::as_const(lines)) {
            if (line.startsWith(QLatin1String("OK version=")))
                mergeFields(appFields, McuUpdater::parseCliFields(line));
        }
        if (!appFields.contains(QStringLiteral("version"))) {
            applicationMissing();
            return;
        }
        if (appFields.contains(QStringLiteral("role"))) {
            applicationFound();
            return;
        }
        if (setStep(Step::AppHelp))
            startWindow("help", cliTimeoutMs);
    }

    void helpWindowEnded()
    {
        // The role is the one whose command the application lists: "OK tx ..." / "OK rx ..." rows.
        QStringList commands;
        for (const QString& line : std::as_const(lines)) {
            if (!line.startsWith(QLatin1String("OK ")))
                continue;
            const QStringList words = splitWords(line);
            if (words.size() > 1)
                commands.append(words.at(1));
        }
        const QString roleText = commands.contains(QLatin1String("tx")) ? QStringLiteral("tx")
            : commands.contains(QLatin1String("rx")) ? QStringLiteral("rx") : QStringLiteral("unknown");
        appFields.insert(QStringLiteral("role"), roleText);
        applicationFound();
    }

    void powerWindowEnded()
    {
        const CliFields fields = McuUpdater::powerFields(lines);
        if (!fields.isEmpty()) {
            const quint64 gen = generation;
            emit q->powerInfo(fields);
            if (!alive(gen))
                return;
            const QString unknown = QStringLiteral("?");
            if (!log(QStringLiteral("INFO power soc=%1 supply=%2 usb=%3")
                         .arg(fields.value(QStringLiteral("soc"), unknown),
                              fields.value(QStringLiteral("supply"), unknown),
                              fields.value(QStringLiteral("usb"), unknown))))
                return;
        }
        enterBootloader();
    }

    void bootloaderWindowEnded()
    {
        bool accepted = false;
        for (const QString& line : std::as_const(lines))
            accepted = accepted || line.startsWith(QLatin1String("OK rebooting_to_bootloader"));
        if (!accepted) {
            finish(false, McuUpdater::tr("application did not enter the bootloader: %1")
                              .arg(lines.isEmpty() ? McuUpdater::tr("no reply") : lines.join(QStringLiteral("; "))));
            return;
        }
        // The device resets now: let it settle, then drop whatever the reset printed.
        if (setStep(Step::Settle)) {
            rx.clear();
            timer.start(resetSettleMs);
        }
    }
};

McuUpdater::McuUpdater(QObject* parent) : QObject(parent), d(new Private(this)) {}

McuUpdater::~McuUpdater() = default;

void McuUpdater::setDevice(QIODevice* device)
{
    if (d->running) {
        qCWarning(lcFirmware) << "MCU updater: the device is replaced only while idle";
        return;
    }
    for (const QMetaObject::Connection& connection : std::as_const(d->deviceConnections))
        QObject::disconnect(connection);
    d->deviceConnections.clear();
    d->device = device;
    if (!device)
        return;
    d->deviceConnections << connect(device, &QIODevice::readyRead, this, [this] { d->onReadyRead(); })
                         << connect(device, &QIODevice::readChannelFinished, this, [this] { d->onDeviceClosed(); })
                         << connect(device, &QIODevice::aboutToClose, this, [this] { d->onDeviceClosed(); })
                         << connect(device, &QObject::destroyed, this, [this] { d->onDeviceClosed(); });
}

QIODevice* McuUpdater::device() const { return d->device; }
void McuUpdater::setThroughSoc(bool on) { d->throughSoc = on; }
bool McuUpdater::throughSoc() const { return d->throughSoc; }
void McuUpdater::setAutoBootloader(bool on) { d->autoBootloader = on; }
bool McuUpdater::autoBootloader() const { return d->autoBootloader; }
void McuUpdater::setPackage(const CcPackage& package) { d->package = package; }
CcPackage McuUpdater::package() const { return d->package; }

bool McuUpdater::isRunning() const { return d->running; }
McuUpdater::Operation McuUpdater::operation() const { return d->operation; }
McuUpdater::Phase McuUpdater::phase() const { return d->phase; }
quint16 McuUpdater::sequence() const { return d->sequence; }

void McuUpdater::setFrameTimeoutMs(int ms) { d->frameTimeoutMs = qMax(0, ms); }
int McuUpdater::frameTimeoutMs() const { return d->frameTimeoutMs; }
void McuUpdater::setFrameRetries(int count) { d->frameRetries = qMax(1, count); }
int McuUpdater::frameRetries() const { return d->frameRetries; }
void McuUpdater::setProbeRetries(int count) { d->probeRetries = qMax(1, count); }
int McuUpdater::probeRetries() const { return d->probeRetries; }
void McuUpdater::setRebootRetries(int count) { d->rebootRetries = qMax(1, count); }
int McuUpdater::rebootRetries() const { return d->rebootRetries; }
void McuUpdater::setChunkSize(int bytes) { d->chunkSize = qBound(1, bytes, 0xFFFF - 4); }
int McuUpdater::chunkSize() const { return d->chunkSize; }
void McuUpdater::setCliTimeoutMs(int ms) { d->cliTimeoutMs = qMax(0, ms); }
int McuUpdater::cliTimeoutMs() const { return d->cliTimeoutMs; }
void McuUpdater::setBootloaderCommandTimeoutMs(int ms) { d->bootloaderCommandTimeoutMs = qMax(0, ms); }
int McuUpdater::bootloaderCommandTimeoutMs() const { return d->bootloaderCommandTimeoutMs; }
void McuUpdater::setResetSettleMs(int ms) { d->resetSettleMs = qMax(0, ms); }
int McuUpdater::resetSettleMs() const { return d->resetSettleMs; }
void McuUpdater::setBannerTimeoutMs(int ms) { d->bannerTimeoutMs = qMax(0, ms); }
int McuUpdater::bannerTimeoutMs() const { return d->bannerTimeoutMs; }

McuUpdater::CliFields McuUpdater::powerFields(const QStringList& lines)
{
    for (const QString& line : lines) {
        if (line.startsWith(QLatin1String("OK power ")))
            return parseCliFields(line);
    }
    return {};
}

McuUpdater::CliFields McuUpdater::parseCliFields(const QString& line)
{
    CliFields fields;
    for (const QString& token : splitWords(line)) {
        const int equals = token.indexOf(QLatin1Char('='));
        if (equals >= 0)
            fields.insert(token.left(equals), token.mid(equals + 1));
    }
    return fields;
}

QString McuUpdater::formatApplication(const CliFields& fields)
{
    return QStringLiteral("INFO application version=%1 role=%2")
        .arg(fields.value(QStringLiteral("version"), QStringLiteral("unknown")),
             fields.value(QStringLiteral("role"), QStringLiteral("unknown")));
}

bool McuUpdater::info() { return d->startOperation(Operation::Info, 0); }
bool McuUpdater::flash(quint32 role) { return d->startOperation(Operation::Flash, role); }
bool McuUpdater::setRole(quint32 role) { return d->startOperation(Operation::SetRole, role); }

void McuUpdater::cancel()
{
    if (d->running)
        d->finish(false, tr("cancelled"));
}
