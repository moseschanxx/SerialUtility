// Throughput and responsiveness suite for the RX pipeline (docs/DESIGN.md 4.7 "High-rate
// output"). Runs offscreen (QT_QPA_PLATFORM=offscreen) against the real widgets and feeds a
// coloured Rockchip-style boot log (about 120 bytes and six SGR sequences per line) the way
// SerialConnection / LogReplayer deliver it: 4 KB chunks, one per event-loop iteration.
//
// Every case prints the measured rate with qInfo() and asserts only a generous floor so a slow
// CI runner does not fail; the numbers to compare against are in DESIGN.md 4.7. Stages:
//   (a) AnsiParser + TerminalScreen alone (no widget, no event loop)
//   (b) TerminalWidget::feedData with the widget shown (coalesced repaints)
//   (c) the SessionWidget RX path: terminal + hex view + (inactive) logger, hex view hidden (the
//       normal case during a boot log: the hex view queues, DESIGN.md 4.7) / shown (renders per chunk)
//   (d) LogReplayer at unlimited speed through SessionWidget::replayLogFile(), with a 50 ms
//       QTimer in the same event loop measuring how often the UI gets a turn
#include <QtTest>

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextDocument>
#include <QTimer>

#include <functional>
#include <memory>

#include "app/AppSettings.h"
#include "core/CommandHistory.h"
#include "core/LogReplayer.h"
#include "core/QuickCommand.h"
#include "core/SessionLogger.h"
#include "terminal/AnsiParser.h"
#include "terminal/TerminalScreen.h"
#include "terminal/TerminalWidget.h"
#include "ui/HexDumpView.h"
#include "ui/SessionWidget.h"

namespace {

constexpr qint64 kChunkBytes = 4096;             ///< LogReplayer's unlimited-speed chunk / a typical readyRead
constexpr qint64 kMiB = 1024 * 1024;
constexpr qint64 kDefaultPayloadBytes = 3 * kMiB; ///< about 25 000 log lines
constexpr int kUiTimerMs = 50;

// Floors (see the file comment): far below what a development machine reaches, high enough to
// catch a regression that reintroduces per-chunk work proportional to the document size.
constexpr double kParserFloorMBps = 1.0;
constexpr double kWidgetFloorMBps = 0.3;
constexpr double kSessionFloorMBps = 0.3;
constexpr double kHexShownFloorMBps = 0.05;
constexpr double kReplayFloorMBps = 0.3;
/// The 50 ms UI timer must get at least this many turns during the 3 MB replay - or one per
/// 100 ms when the replay is over in less than 500 ms (a fast machine cannot fire it five times).
constexpr int kMinUiTicksDuringReplay = 5;
/// Longest tolerated gap between two turns of that timer (the UI is "frozen" for that long).
constexpr qint64 kMaxUiStallMs = 500;

/// The offscreen platform's FreeType font database only scans <Qt>/lib/fonts (absent in
/// current Qt builds); without fonts every glyph is a QFontEngineBox rectangle and painting
/// costs nothing like the real thing. Point it at the system fonts (see tst_terminalwidget).
const bool s_fontDirConfigured = [] {
    if (!qEnvironmentVariableIsSet("QT_QPA_FONTDIR")) {
        const QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::FontsLocation);
        for (const QString& dir : dirs) {
            if (QDir(dir).exists()) {
                qputenv("QT_QPA_FONTDIR", QDir::toNativeSeparators(dir).toLocal8Bit());
                break;
            }
        }
    }
    return true;
}();

/// SU_PERF_BYTES overrides the payload size (bytes) for ad-hoc measurements.
qint64 payloadBytes()
{
    bool ok = false;
    const qint64 env = qEnvironmentVariableIntValue("SU_PERF_BYTES", &ok);
    return (ok && env > 0) ? env : kDefaultPayloadBytes;
}

/// A coloured boot log of at least `minBytes`: the line format of the 1.5 Mbaud Rockchip logs
/// this tool is built for (timestamp, bold green driver name, two more coloured fields, hex).
QByteArray bootLog(qint64 minBytes)
{
    QByteArray out;
    out.reserve(static_cast<qsizetype>(minBytes + 256));
    const QByteArray esc = QByteArrayLiteral("\x1b");
    for (int i = 0; out.size() < minBytes; ++i) {
        const QByteArray stamp = QByteArray::number(i * 0.001234, 'f', 6).rightJustified(12, ' ');
        out += '[';
        out += stamp;
        out += "] ";
        out += esc + "[1;32mrockchip-driver" + esc + "[0m subsystem " + QByteArray::number(i) + ": ";
        out += esc + "[33mstatus" + esc + "[0m ok, ";
        out += esc + "[36mlatency" + esc + "[0m=" + QByteArray::number(i % 97) + " us, ";
        out += "buffer 0x" + QByteArray::number(qint64(i) * 4096 & 0xffffffffLL, 16).rightJustified(8, '0');
        out += "\r\n";
    }
    return out;
}

int lineCount(const QByteArray& data)
{
    return static_cast<int>(data.count('\n'));
}

double mbPerSecond(qint64 bytes, qint64 ms)
{
    return ms > 0 ? (static_cast<double>(bytes) / static_cast<double>(kMiB)) / (static_cast<double>(ms) / 1000.0) : 0.0;
}

struct RunResult
{
    qint64 elapsedMs = 0;
    int uiTicks = 0;        ///< 50 ms timer fired this many times while the data was being fed
    qint64 maxUiGapMs = 0;  ///< longest interval between two turns of that timer (includes its 50 ms period)

    double mbps(qint64 bytes) const { return mbPerSecond(bytes, elapsedMs); }
    double ticksPerSecond() const { return elapsedMs > 0 ? uiTicks * 1000.0 / static_cast<double>(elapsedMs) : 0.0; }
    /// Turns the 50 ms timer must have had for the run to count as responsive (see kMinUiTicksDuringReplay).
    int requiredUiTicks() const { return static_cast<int>(qMin<qint64>(kMinUiTicksDuringReplay, elapsedMs / 100)); }
};

/// A kUiTimerMs QTimer standing in for the rest of the UI: counts its turns while data is fed and
/// remembers the longest wait between two of them (how long the UI was blocked at worst).
class UiProbe
{
public:
    explicit UiProbe(RunResult* result)
        : m_result(result)
    {
        m_timer.setInterval(kUiTimerMs);
        m_timer.setTimerType(Qt::PreciseTimer);
        QObject::connect(&m_timer, &QTimer::timeout, [this]() {
            ++m_result->uiTicks;
            m_result->maxUiGapMs = qMax(m_result->maxUiGapMs, m_sinceTick.restart());
        });
    }
    void start()
    {
        m_sinceTick.start();
        m_timer.start();
    }
    void stop()
    {
        m_timer.stop();
        m_result->maxUiGapMs = qMax(m_result->maxUiGapMs, m_sinceTick.elapsed());
    }

private:
    RunResult* m_result;
    QTimer m_timer;
    QElapsedTimer m_sinceTick;
};

/// Feeds `data` to `sink` in kChunkBytes chunks from a zero-interval timer, one chunk per
/// event-loop iteration (roughly what QSerialPort::readyRead delivers; LogReplayer at unlimited
/// speed feeds several per iteration), while a UiProbe counts how often the loop gets back to
/// it. Returns once every chunk is delivered and the pending repaint has run.
RunResult feedThroughEventLoop(const QByteArray& data, const std::function<void(const QByteArray&)>& sink)
{
    RunResult result;
    QEventLoop loop;
    QTimer feeder;
    feeder.setInterval(0);
    UiProbe ui(&result);

    qint64 pos = 0;
    QElapsedTimer timer;
    QObject::connect(&feeder, &QTimer::timeout, [&]() {
        if (pos >= data.size()) {
            feeder.stop();
            result.elapsedMs = timer.elapsed();
            loop.quit();
            return;
        }
        const qint64 count = qMin<qint64>(kChunkBytes, data.size() - pos);
        sink(data.mid(pos, count));
        pos += count;
    });
    timer.start();
    ui.start();
    feeder.start();
    loop.exec();
    ui.stop();
    QTest::qWait(40);   // let the coalesced repaint run
    return result;
}

} // namespace

class Tst_terminalperf : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void parserAndScreen();
    void terminalWidgetShown();
    void sessionWidgetHexHidden();
    void sessionWidgetHexShown();
    void replayUnlimitedKeepsUiResponsive();

private:
    std::unique_ptr<SessionWidget> newSession();
    void reportRun(const char* label, const QByteArray& data, const RunResult& r);

    QTemporaryDir m_tempDir;
    QuickCommandStore* m_store = nullptr;
    CommandHistory m_history;
    QByteArray m_log;
};

// ---------------------------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------------------------

void Tst_terminalperf::initTestCase()
{
    QVERIFY(s_fontDirConfigured);
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-perf"));
    QVERIFY(m_tempDir.isValid());
    // Keep AppSettings out of the registry and inside the temporary directory.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_tempDir.filePath(QStringLiteral("settings")));
    QSettings settings;
    settings.clear();
    QVERIFY(settings.fileName().contains(QStringLiteral("SerialUtilityTest-perf")));

    AppSettings& app = AppSettings::instance();
    app.setLogDirectory(m_tempDir.filePath(QStringLiteral("logs")));
    app.setAutoLog(false);
    app.setShowSimulatedPorts(true);
    app.setCursorBlink(false);
    app.setBellEnabled(false);

    m_store = new QuickCommandStore(this);
    QVERIFY(m_store->load(m_tempDir.filePath(QStringLiteral("quick_commands.json"))));

    m_log = bootLog(payloadBytes());
    qInfo("payload: %lld bytes, %d lines, %d-byte chunks", static_cast<long long>(m_log.size()), lineCount(m_log),
          static_cast<int>(kChunkBytes));
}

void Tst_terminalperf::init()
{
    AppSettings::instance().setAutoLog(false);
}

std::unique_ptr<SessionWidget> Tst_terminalperf::newSession()
{
    auto session = std::make_unique<SessionWidget>(m_store, &m_history);
    session->resize(1000, 700);
    session->show();
    if (!QTest::qWaitForWindowExposed(session.get())) {
        return nullptr;
    }
    session->activateWindow();
    QTest::qWait(40);   // initial paint / scrollbar setup out of the way
    return session;
}

void Tst_terminalperf::reportRun(const char* label, const QByteArray& data, const RunResult& r)
{
    const int lines = lineCount(data);
    const double seconds = static_cast<double>(r.elapsedMs) / 1000.0;
    qInfo("%s: %.2f MB (%d lines) in %lld ms = %.2f MB/s (%.0f lines/s); 50 ms UI timer fired %d times (%.1f/s), "
          "longest UI stall %lld ms",
          label, static_cast<double>(data.size()) / static_cast<double>(kMiB), lines,
          static_cast<long long>(r.elapsedMs), r.mbps(data.size()), seconds > 0 ? lines / seconds : 0.0, r.uiTicks,
          r.ticksPerSecond(), static_cast<long long>(r.maxUiGapMs));
}

// ---------------------------------------------------------------------------------------------
// (a) parser + screen model
// ---------------------------------------------------------------------------------------------

void Tst_terminalperf::parserAndScreen()
{
    TerminalScreen screen(40, 120, 10000);
    AnsiParser parser(&screen);
    QElapsedTimer timer;
    timer.start();
    for (qint64 pos = 0; pos < m_log.size(); pos += kChunkBytes) {
        parser.feed(m_log.mid(pos, kChunkBytes));
    }
    RunResult r;
    r.elapsedMs = timer.elapsed();
    reportRun("(a) AnsiParser+TerminalScreen", m_log, r);
    QVERIFY(screen.scrollbackSize() == 10000);
    QVERIFY2(r.mbps(m_log.size()) >= kParserFloorMBps, qPrintable(QStringLiteral("%1 MB/s").arg(r.mbps(m_log.size()))));
}

// ---------------------------------------------------------------------------------------------
// (b) TerminalWidget shown
// ---------------------------------------------------------------------------------------------

void Tst_terminalperf::terminalWidgetShown()
{
    TerminalWidget term;
    term.setBellEnabled(false);
    term.setCursorBlink(false);
    term.resize(1000, 700);
    term.show();
    QVERIFY(QTest::qWaitForWindowExposed(&term));
    QTest::qWait(40);

    const RunResult r = feedThroughEventLoop(m_log, [&term](const QByteArray& chunk) { term.feedData(chunk); });
    reportRun("(b) TerminalWidget::feedData shown", m_log, r);
    QVERIFY(term.isAtBottom());
    QCOMPARE(term.verticalScrollBar()->value(), term.verticalScrollBar()->maximum());
    QCOMPARE(term.verticalScrollBar()->maximum(), term.screen()->scrollbackSize());
    QVERIFY(!term.screen()->isDirty());   // the last coalesced repaint consumed the dirty state
    QVERIFY2(r.mbps(m_log.size()) >= kWidgetFloorMBps, qPrintable(QStringLiteral("%1 MB/s").arg(r.mbps(m_log.size()))));
    QVERIFY2(r.maxUiGapMs <= kMaxUiStallMs, qPrintable(QStringLiteral("UI stalled for %1 ms").arg(r.maxUiGapMs)));
}

// ---------------------------------------------------------------------------------------------
// (c) SessionWidget RX path (terminal + hex view + logger), hex view hidden / shown
// ---------------------------------------------------------------------------------------------

void Tst_terminalperf::sessionWidgetHexHidden()
{
    auto session = newSession();
    QVERIFY(session);
    QCOMPARE(session->viewMode(), SessionWidget::ViewMode::Terminal);
    QVERIFY(!session->isLogging());
    TerminalWidget* terminal = session->terminal();
    HexDumpView* hex = session->hexView();
    SessionLogger* logger = session->logger();

    // The three RX consumers, wired exactly like SerialConnection::dataReceived / LogReplayer::chunkReady.
    const RunResult r = feedThroughEventLoop(m_log, [terminal, hex, logger](const QByteArray& chunk) {
        terminal->feedData(chunk);
        hex->appendReceived(chunk);
        logger->logReceived(chunk);
    });
    reportRun("(c1) SessionWidget path, hex view hidden", m_log, r);
    QVERIFY(terminal->isAtBottom());
    QCOMPARE(terminal->screen()->scrollbackSize(), 10000);
    QVERIFY2(r.mbps(m_log.size()) >= kSessionFloorMBps,
             qPrintable(QStringLiteral("%1 MB/s").arg(r.mbps(m_log.size()))));
    QVERIFY2(r.maxUiGapMs <= kMaxUiStallMs, qPrintable(QStringLiteral("UI stalled for %1 ms").arg(r.maxUiGapMs)));

    // Hidden, the hex view only queued the chunks (its document was never touched); showing it
    // renders the bounded tail of the traffic before the first paint.
    QCOMPARE(hex->document()->blockCount(), 1);
    session->setViewMode(SessionWidget::ViewMode::HexDump);
    QTRY_VERIFY(hex->isVisible());
    QVERIFY(hex->toPlainText().contains(QStringLiteral("RX 4096 bytes")));
    QVERIFY(hex->document()->blockCount() <= hex->maxLines() + 2);
    QVERIFY(hex->document()->blockCount() >= hex->maxLines() - 2);
}

void Tst_terminalperf::sessionWidgetHexShown()
{
    auto session = newSession();
    QVERIFY(session);
    session->setViewMode(SessionWidget::ViewMode::HexDump);
    HexDumpView* hex = session->hexView();
    QTRY_VERIFY(hex->isVisible());
    TerminalWidget* terminal = session->terminal();
    SessionLogger* logger = session->logger();

    // The hex view renders a QTextDocument per chunk; keep this case short.
    const QByteArray data = m_log.left(qMin<qsizetype>(m_log.size(), kMiB));
    const RunResult r = feedThroughEventLoop(data, [terminal, hex, logger](const QByteArray& chunk) {
        terminal->feedData(chunk);
        hex->appendReceived(chunk);
        logger->logReceived(chunk);
    });
    reportRun("(c2) SessionWidget path, hex view shown", data, r);
    QVERIFY(hex->toPlainText().contains(QStringLiteral("RX 4096 bytes")));
    QVERIFY(hex->document()->blockCount() <= hex->maxLines() + 2);
    QVERIFY2(r.mbps(data.size()) >= kHexShownFloorMBps, qPrintable(QStringLiteral("%1 MB/s").arg(r.mbps(data.size()))));
}

// ---------------------------------------------------------------------------------------------
// (d) LogReplayer unlimited speed through SessionWidget::replayLogFile
// ---------------------------------------------------------------------------------------------

void Tst_terminalperf::replayUnlimitedKeepsUiResponsive()
{
    const QString path = m_tempDir.filePath(QStringLiteral("boot.log"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(m_log), qint64(m_log.size()));
    }
    auto session = newSession();
    QVERIFY(session);
    TerminalWidget* terminal = session->terminal();

    QEventLoop loop;
    QElapsedTimer timer;
    RunResult r;
    connect(session.get(), &SessionWidget::replayStateChanged, this, [&](bool active) {
        if (!active) {
            r.elapsedMs = timer.elapsed();
            loop.quit();
        }
    });
    UiProbe ui(&r);
    QTimer guard;
    guard.setSingleShot(true);
    connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);

    timer.start();
    ui.start();
    guard.start(120000);
    session->replayLogFile(path, 0);
    QVERIFY(session->isReplaying());
    loop.exec();
    ui.stop();
    QVERIFY2(!session->isReplaying(), "replay did not finish within the guard time");
    QTest::qWait(40);

    reportRun("(d) LogReplayer unlimited -> SessionWidget", m_log, r);
    QVERIFY(terminal->isAtBottom());
    const TerminalScreen* screen = terminal->screen();
    QCOMPARE(screen->scrollbackSize(), 10000);
    // The last log line and the "replay finished" system line made it to the screen.
    QString tail;
    for (int i = 0; i < screen->rows(); ++i) {
        tail += screen->lineText(screen->scrollbackSize() + i);
        tail += QLatin1Char('\n');
    }
    QVERIFY2(tail.contains(QStringLiteral("subsystem %1:").arg(lineCount(m_log) - 1)), qPrintable(tail.right(400)));
    QVERIFY(tail.contains(QStringLiteral("replay of boot.log finished")));
    QVERIFY2(r.mbps(m_log.size()) >= kReplayFloorMBps, qPrintable(QStringLiteral("%1 MB/s").arg(r.mbps(m_log.size()))));
    QVERIFY2(r.uiTicks >= r.requiredUiTicks(),
             qPrintable(QStringLiteral("50 ms timer fired only %1 times in %2 ms").arg(r.uiTicks).arg(r.elapsedMs)));
    QVERIFY2(r.maxUiGapMs <= kMaxUiStallMs, qPrintable(QStringLiteral("UI stalled for %1 ms").arg(r.maxUiGapMs)));
}

QTEST_MAIN(Tst_terminalperf)
#include "tst_terminalperf.moc"
