#include "firmware/FleetReport.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QRegularExpression>

#include <cmath>

namespace {

/// The prefix of a stop of the mapping (FleetFlasher::stopReason()): "mapping stopped: <reason>".
/// Any other non-empty stop is a refusal, whose summary is the reason itself.
const QLatin1String kMappingStopped("mapping stopped: ");
/// The exit-3 condition (FirmwareCli::ExitNoBoard): the selection found no board on ADB.
const QLatin1String kNoneOnAdb("no selected board is on ADB");

// FirmwareCli::ExitCode, as plain numbers: su_core's value class does not include the CLI.
constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitNoBoard = 3;
constexpr int kExitCancelled = 5;

QString oneDecimal(double seconds)
{
    return QString::number(seconds, 'f', 1);
}

/// A table cell of seconds: one decimal, "-" for a stage the board never reached.
QString secondsCell(double seconds)
{
    return seconds > 0.0 ? oneDecimal(seconds) : QStringLiteral("-");
}

/// Seconds for the JSON: one decimal, as the lines print them.
double jsonSeconds(double seconds)
{
    return std::round(seconds * 10.0) / 10.0;
}

QString orDash(const QString& text)
{
    return text.isEmpty() ? QStringLiteral("-") : text;
}

/// "Linux version 5.10.209+ (...)" -> "5.10.209+": the kernel the flashed board runs, from its
/// board info; the one read before the reboot when the board info is empty.
QString kernelAfterFlash(const FleetBoard& board)
{
    static const QRegularExpression pattern(QStringLiteral("Linux version (\\S+)"));
    const QRegularExpressionMatch match = pattern.match(board.boardInfo);
    return match.hasMatch() ? match.captured(1) : board.kernel;
}

bool moved(const FleetBoard& board)
{
    return !board.expectedPort.isEmpty() && !board.port.isEmpty() && board.expectedPort != board.port;
}

/// The detail column of the real run's table.
QString detailCell(const FleetBoard& board)
{
    if (board.result != FleetBoard::Result::Done) {
        return board.detail;
    }
    QString text;
    if (!board.boardInfo.isEmpty()) {
        text = kernelAfterFlash(board);
        if (!board.uptime.isEmpty()) {
            text += QStringLiteral(", ") + board.uptime;
        }
        if (!board.rfmcu.isEmpty()) {
            text += QStringLiteral(", rfmcu: ") + board.rfmcu.simplified();
        }
    }
    if (moved(board)) {
        text += text.isEmpty() ? QStringLiteral("(moved)") : QStringLiteral(" (moved)");
    }
    if (!board.detail.isEmpty()) {
        text += text.isEmpty() ? board.detail : QStringLiteral("; ") + board.detail;
    }
    return text;
}

QString unattributedSuffix(qsizetype count)
{
    if (count <= 0) {
        return QString();
    }
    return count == 1 ? QCoreApplication::translate("FleetReport", "; 1 Loader device left unattributed")
                      : QCoreApplication::translate("FleetReport", "; %1 Loader devices left unattributed").arg(count);
}

} // namespace

// ---------------------------------------------------------------------------------------------
// Totals
// ---------------------------------------------------------------------------------------------

bool FleetReport::Totals::operator==(const Totals& o) const
{
    return selected == o.selected && ready == o.ready && done == o.done && notBack == o.notBack && failed == o.failed
           && skipped == o.skipped;
}

// ---------------------------------------------------------------------------------------------
// Inputs and getters
// ---------------------------------------------------------------------------------------------

void FleetReport::setBoards(const QList<FleetBoard>& boards) { m_boards = boards; }
void FleetReport::setDryRun(bool on) { m_dryRun = on; }
void FleetReport::setImage(const QString& path) { m_image = path; }
void FleetReport::setPartitions(const QStringList& names) { m_partitions = names; }
void FleetReport::setMaxParallel(int count) { m_maxParallel = count; }

void FleetReport::setSeconds(double total, double mapping, double writing)
{
    m_seconds = total;
    m_mappingSeconds = mapping;
    m_writingSeconds = writing;
}

void FleetReport::setStop(const QString& reason) { m_stop = reason; }
void FleetReport::setResetCount(int boards) { m_resetCount = boards; }
void FleetReport::setUnattributed(const QList<FleetFlasher::UnattributedDevice>& devices) { m_unattributed = devices; }
void FleetReport::setAborted(bool aborted) { m_aborted = aborted; }

QList<FleetBoard> FleetReport::boards() const { return m_boards; }
bool FleetReport::dryRun() const { return m_dryRun; }
QString FleetReport::image() const { return m_image; }
QStringList FleetReport::partitions() const { return m_partitions; }
int FleetReport::maxParallel() const { return m_maxParallel; }
double FleetReport::seconds() const { return m_seconds; }
double FleetReport::mappingSeconds() const { return m_mappingSeconds; }
double FleetReport::writingSeconds() const { return m_writingSeconds; }
QString FleetReport::stop() const { return m_stop; }
int FleetReport::resetCount() const { return m_resetCount; }
QList<FleetFlasher::UnattributedDevice> FleetReport::unattributed() const { return m_unattributed; }
bool FleetReport::aborted() const { return m_aborted; }

// ---------------------------------------------------------------------------------------------
// What the user reads
// ---------------------------------------------------------------------------------------------

FleetReport::Totals FleetReport::totals() const
{
    Totals totals;
    totals.selected = static_cast<int>(m_boards.size());
    for (const FleetBoard& board : m_boards) {
        switch (board.result) {
        case FleetBoard::Result::Ready: ++totals.ready; break;
        case FleetBoard::Result::Done: ++totals.done; break;
        case FleetBoard::Result::NotBack: ++totals.notBack; break;
        case FleetBoard::Result::Failed: ++totals.failed; break;
        case FleetBoard::Result::Skipped:
        case FleetBoard::Result::Pending:   // a board the run never decided: never written
            ++totals.skipped;
            break;
        }
    }
    return totals;
}

bool FleetReport::allOk() const
{
    if (m_boards.isEmpty() || !m_stop.isEmpty()) {
        return false;   // never vacuously true: a run that selected nobody did not flash anything
    }
    const FleetBoard::Result wanted = m_dryRun ? FleetBoard::Result::Ready : FleetBoard::Result::Done;
    for (const FleetBoard& board : m_boards) {
        if (board.result != wanted) {
            return false;
        }
    }
    return true;
}

bool FleetReport::noneOnAdb() const
{
    return m_stop.startsWith(kNoneOnAdb);
}

QString FleetReport::summary() const
{
    if (!m_stop.isEmpty()) {
        if (!m_stop.startsWith(kMappingStopped)) {
            return m_stop;   // a refusal: the reason itself
        }
        QString text = m_stop + QCoreApplication::translate("FleetReport", "; nothing was written");
        if (m_resetCount > 0) {
            text += m_resetCount == 1 ? QCoreApplication::translate("FleetReport", "; 1 board reset back to ADB")
                                      : QCoreApplication::translate("FleetReport", "; %1 boards reset back to ADB").arg(m_resetCount);
        }
        return text + unattributedSuffix(m_unattributed.size());
    }
    const Totals counts = totals();
    if (m_dryRun) {
        QString text = QCoreApplication::translate("FleetReport", "dry run: %1 of %2 boards ready").arg(counts.ready).arg(counts.selected);
        const int notReady = counts.selected - counts.ready;
        if (notReady > 0) {
            text += QCoreApplication::translate("FleetReport", "; %1 skipped").arg(notReady);
        }
        return text;
    }
    QString text = QCoreApplication::translate("FleetReport", "%1 of %2 boards flashed in %3 s (mapping %4 s, writing %5 s)")
                       .arg(counts.done)
                       .arg(counts.selected)
                       .arg(oneDecimal(m_seconds), oneDecimal(m_mappingSeconds), oneDecimal(m_writingSeconds));
    if (counts.notBack > 0) {
        text += QCoreApplication::translate("FleetReport", "; %1 not back on ADB").arg(counts.notBack);
    }
    if (counts.failed > 0) {
        text += QCoreApplication::translate("FleetReport", "; %1 failed").arg(counts.failed);
    }
    if (counts.skipped > 0) {
        text += QCoreApplication::translate("FleetReport", "; %1 skipped").arg(counts.skipped);
    }
    return text + unattributedSuffix(m_unattributed.size());
}

QStringList FleetReport::tableLines() const
{
    QList<QStringList> rows;
    if (m_dryRun) {
        rows.append({QStringLiteral("board"), QStringLiteral("serial"), QStringLiteral("adb"), QStringLiteral("expected port"),
                     QStringLiteral("result"), QStringLiteral("detail")});
        for (const FleetBoard& board : m_boards) {
            rows.append({board.tag, board.serial, orDash(board.adbState),
                         board.expectedPort.isEmpty() ? QStringLiteral("unknown") : board.expectedPort, board.resultText(),
                         board.detail});
        }
        return alignColumns(rows);
    }
    rows.append({QStringLiteral("board"), QStringLiteral("serial"), QStringLiteral("port"), QStringLiteral("loader"),
                 QStringLiteral("result"), QStringLiteral("mapping"), QStringLiteral("writing"), QStringLiteral("seconds"),
                 QStringLiteral("detail")});
    for (const FleetBoard& board : m_boards) {
        rows.append({board.tag, board.serial, orDash(board.port), orDash(board.locationId), board.resultText(),
                     secondsCell(board.mappingSeconds), secondsCell(board.writingSeconds), secondsCell(board.seconds),
                     detailCell(board)});
    }
    return alignColumns(rows);
}

QJsonObject FleetReport::boardJson(const FleetBoard& board)
{
    return QJsonObject{{QStringLiteral("tag"), board.tag},
                       {QStringLiteral("serial"), board.serial},
                       {QStringLiteral("adb"), board.adbState},
                       {QStringLiteral("expectedPort"), board.expectedPort},
                       {QStringLiteral("port"), board.port},
                       {QStringLiteral("loader"), board.locationId},
                       {QStringLiteral("stage"), board.stageText()},
                       {QStringLiteral("result"), board.resultText()},
                       {QStringLiteral("ok"), board.ok()},
                       {QStringLiteral("seconds"), jsonSeconds(board.seconds)},
                       {QStringLiteral("mappingSeconds"), jsonSeconds(board.mappingSeconds)},
                       {QStringLiteral("writingSeconds"), jsonSeconds(board.writingSeconds)},
                       {QStringLiteral("detail"), board.detail},
                       {QStringLiteral("written"), QJsonArray::fromStringList(board.written)},
                       {QStringLiteral("retry"), QJsonArray::fromStringList(board.retry)},
                       {QStringLiteral("adbBack"), board.adbBack},
                       {QStringLiteral("leftInLoader"), board.leftInLoader},
                       {QStringLiteral("kernel"), board.kernel},
                       {QStringLiteral("profile"), board.profile},   // (v0.8.0) "" = unknown
                       {QStringLiteral("uptime"), board.uptime},
                       {QStringLiteral("boardInfo"), board.boardInfo},
                       {QStringLiteral("rfmcu"), board.rfmcu}};
}

QJsonObject FleetReport::json() const
{
    QJsonArray unattributed;
    for (const FleetFlasher::UnattributedDevice& device : m_unattributed) {
        unattributed.append(QJsonObject{{QStringLiteral("loader"), device.locationId},
                                        {QStringLiteral("port"), device.port},
                                        {QStringLiteral("mode"), device.mode},
                                        {QStringLiteral("hint"), device.hint()}});
    }
    const Totals counts = totals();
    const QJsonObject summaryObject =
        m_dryRun ? QJsonObject{{QStringLiteral("selected"), counts.selected},
                               {QStringLiteral("ready"), counts.ready},
                               {QStringLiteral("skipped"), counts.selected - counts.ready}}
                 : QJsonObject{{QStringLiteral("selected"), counts.selected},
                               {QStringLiteral("done"), counts.done},
                               {QStringLiteral("notBack"), counts.notBack},
                               {QStringLiteral("failed"), counts.failed},
                               {QStringLiteral("skipped"), counts.skipped}};
    QJsonArray boardArray;
    for (const FleetBoard& board : m_boards) {
        boardArray.append(boardJson(board));
    }
    return QJsonObject{{QStringLiteral("fleet"), true},
                       {QStringLiteral("dryRun"), m_dryRun},
                       {QStringLiteral("image"), QDir::toNativeSeparators(m_image)},
                       {QStringLiteral("partitions"), QJsonArray::fromStringList(m_partitions)},
                       {QStringLiteral("maxParallel"), m_maxParallel},
                       {QStringLiteral("seconds"), jsonSeconds(m_seconds)},
                       {QStringLiteral("mappingSeconds"), jsonSeconds(m_mappingSeconds)},
                       {QStringLiteral("writingSeconds"), jsonSeconds(m_writingSeconds)},
                       {QStringLiteral("stop"), m_stop},
                       {QStringLiteral("unattributed"), unattributed},
                       {QStringLiteral("summary"), summaryObject},
                       {QStringLiteral("boards"), boardArray}};
}

int FleetReport::exitCode() const
{
    if (m_aborted) {
        return kExitCancelled;
    }
    if (noneOnAdb()) {
        return kExitNoBoard;
    }
    return allOk() ? kExitOk : kExitFailed;
}

QStringList FleetReport::alignColumns(const QList<QStringList>& rows)
{
    QList<qsizetype> widths;
    for (const QStringList& cells : rows) {
        for (qsizetype c = 0; c < cells.size(); ++c) {
            if (c >= widths.size()) {
                widths.append(0);
            }
            widths[c] = qMax(widths.at(c), cells.at(c).size());
        }
    }
    QStringList lines;
    for (const QStringList& cells : rows) {
        QString line;
        for (qsizetype c = 0; c < cells.size(); ++c) {
            line += c + 1 < cells.size() ? cells.at(c).leftJustified(widths.at(c) + 2) : cells.at(c);
        }
        while (line.endsWith(QLatin1Char(' '))) {
            line.chop(1);
        }
        lines << line;
    }
    return lines;
}
