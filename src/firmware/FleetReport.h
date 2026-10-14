#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include "firmware/FleetFlasher.h"

/**
 * The record of one fleet flash (v0.7.0): the selected boards as FleetFlasher left them, the
 * run's timing, whether it was a dry run, the reason it stopped and the download-mode devices it
 * could not name - and from them the text the user reads (summary(), tableLines()), the fields of
 * su-fw's JSON `done` object (json()) and the exit code (exitCode()). A value class without
 * QObject: FleetFlasher::report() fills it, FirmwareJob::fleetReport() hands it on, FirmwareCli
 * prints it, and the tests check the table, the JSON and the exit mapping without a run.
 *
 * summary() (also FleetFlasher::summary(), finished()'s message and the operation's result):
 *  - a real run: "7 of 7 boards flashed in 48.3 s (mapping 9.8 s, writing 31.2 s)"; with
 *    problems the counts that are not zero follow: "5 of 7 boards flashed in 61.0 s (mapping 9.8 s,
 *    writing 31.2 s); 1 not back on ADB; 1 failed" / "...; 2 skipped"; and "; j Loader device(s)
 *    left unattributed" when unattributed() is not empty (the same words as the stop form; the
 *    boards' results and allOk() are not changed by it);
 *  - a stop before any write: "mapping stopped: <reason>; nothing was written" (plus "; 2 boards
 *    reset back to ADB" when mapped boards were reset, "; 1 Loader device left unattributed" when
 *    one was); a refusal: the reason itself ("a rockusb device is already in Loader mode ...",
 *    "no selected board is on ADB (attached: none)", "port mapping not available on this platform");
 *  - a dry run: "dry run: 6 of 7 boards ready; 1 skipped" ("dry run: 7 of 7 boards ready").
 *  "flashed" means Done: written, reset and back on ADB (or --no-wait). su-fw prefixes an aborted
 *  run itself ("interrupted: ...", "timeout after 300 s: ...").
 *
 * tableLines(): the header "board  serial  port  loader  result  mapping  writing  seconds  detail"
 * and one row per selected board in selection order, column widths from the content, two blanks
 * between columns (as su-fw's partition table): tag; serial; the learned port ("-" when never
 * mapped); the LocationID learned in the run ("-" when never mapped; kept for done and not-back
 * boards: FleetBoard::locationId is never cleared); resultText(); mappingSeconds, writingSeconds and seconds with one
 * decimal ("-" when 0); detail = a done board's "5.10.209+, up 0 min" (+ ", rfmcu: pid=.. uart=owned
 * mcu=alive" when read) + " (moved)" when it moved, else the board's detail text. A dry run's table
 * instead: "board  serial  adb  expected port  result  detail" (adb = the state or "-", the
 * remembered port or "unknown", ready / skipped). An empty board list gives the header only.
 *
 * json(): the `done` object's fields, every one always present: "fleet" true, "dryRun", "image"
 * (native path), "partitions" (names), "maxParallel", "seconds", "mappingSeconds", "writingSeconds",
 * "stop" (the reason or ""), "unattributed" ([{"loader","port","mode","hint"}]), "summary" ({"selected",
 * "done","notBack","failed","skipped"}; a dry run: {"selected","ready","skipped"}) and "boards"
 * (boardJson() each, selection order). boardJson(): "tag", "serial", "adb", "expectedPort", "port",
 * "loader", "stage", "result" (resultText()), "ok", "seconds", "mappingSeconds", "writingSeconds",
 * "detail", "written" (names), "retry" (the addressed command lines of FleetBoard::retry, [] for
 * every board not left in Loader mode), "adbBack", "leftInLoader", "kernel", "uptime", "boardInfo",
 * "rfmcu".
 *
 * exitCode() (the values of FirmwareCli::ExitCode), decided from the boards and the stop text alone
 * so the CLI needs no detection of its own: 5 when aborted (Ctrl+C / --timeout), whatever the
 * boards came to; 3 when noneOnAdb(); 0 when allOk(); else 1 (a board NotBack / Failed / Skipped,
 * a stop, a refusal). An EMPTY board list is never 0: it is 3 when noneOnAdb() (--all-boards found
 * nothing: stop "no selected board is on ADB (attached: none)") and 1 otherwise - a run that never
 * selected (the lock, the platform, the layout, the plan, an adb that could not run) leaves the
 * flasher's report empty with stop "" or its refusal text, and exits 1.
 */
class FleetReport
{
public:
    struct Totals
    {
        int selected = 0;
        int ready = 0;      ///< dry run only
        int done = 0;
        int notBack = 0;
        int failed = 0;
        int skipped = 0;
        bool operator==(const Totals& other) const;
    };

    void setBoards(const QList<FleetBoard>& boards);
    void setDryRun(bool on);
    void setImage(const QString& path);
    void setPartitions(const QStringList& names);
    void setMaxParallel(int count);
    void setSeconds(double total, double mapping, double writing);
    void setStop(const QString& reason);                                      ///< "" = the run was not stopped / refused
    void setResetCount(int boards);                                           ///< mapped boards reset back to ADB by a stop or a cancel
    void setUnattributed(const QList<FleetFlasher::UnattributedDevice>& devices);   ///< a dry run: foreignDevices(); a real run: unattributedDevices() (a stop's, the reconciliation's and the end-of-run snapshot's)
    void setAborted(bool aborted);                                            ///< Ctrl+C / --timeout ended the run

    QList<FleetBoard> boards() const;
    bool dryRun() const;
    QString image() const;
    QStringList partitions() const;
    int maxParallel() const;
    double seconds() const;
    double mappingSeconds() const;
    double writingSeconds() const;
    QString stop() const;
    int resetCount() const;
    QList<FleetFlasher::UnattributedDevice> unattributed() const;
    bool aborted() const;

    Totals totals() const;
    bool allOk() const;                 ///< at least one board, every one Done (Ready in a dry run) and no stop; FALSE for an empty board list
    bool noneOnAdb() const;             ///< the exit-3 condition: stop() starts with "no selected board is on ADB" (the selection skipped every board, or --all-boards found nothing)
    QString summary() const;
    QStringList tableLines() const;
    QJsonObject json() const;
    static QJsonObject boardJson(const FleetBoard& board);
    int exitCode() const;
    /// Rows (the first one the header) to lines: every column as wide as its widest cell, two
    /// blanks between columns, no trailing blanks.
    static QStringList alignColumns(const QList<QStringList>& rows);

private:
    QList<FleetBoard> m_boards;
    bool m_dryRun = false;
    QString m_image;
    QStringList m_partitions;
    int m_maxParallel = 0;
    double m_seconds = 0.0;
    double m_mappingSeconds = 0.0;
    double m_writingSeconds = 0.0;
    QString m_stop;
    int m_resetCount = 0;
    QList<FleetFlasher::UnattributedDevice> m_unattributed;
    bool m_aborted = false;
};
