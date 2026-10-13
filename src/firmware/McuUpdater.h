#pragma once

#include <QObject>
#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <memory>

#include "firmware/CcPackage.h"
#include "firmware/CcProtocol.h"

class QIODevice;

/**
 * The CC1310 updater state machine of CC1310-Firmware/tools/fw_update.py (class Updater, plus
 * run_on()'s messages), asynchronous over a QIODevice - the QTcpSocket McuLink opens to the
 * board's `rfmcu bridge`, or a serial port / test device. Never blocks: every wait is a QTimer
 * deadline; bytes arrive through QIODevice::readyRead().
 *
 * Operations (one at a time; start() returns false while busy or without a device):
 *  - Info:    HELLO (probeRetries) -> deviceInfo(); when the bootloader does not answer, the
 *             application CLI is probed and applicationInfo() reports it with the note
 *             "state=running (bootloader inactive; 'flash' and 'set-role' reboot into it
 *             automatically)". The device is left as it is.
 *  - Flash:   connect sequence (below), target / size checks ("package target does not match
 *             device", "package exceeds device App slot"), BEGIN(package.header + role u32) ->
 *             READY (4 bytes, offset must be 0: "bootloader requested unsupported resume offset"),
 *             DATA(offset u32 + up to 128 bytes) -> ACK / NACK (4 bytes = the offset the device
 *             expects next; a NACK with an offset beyond what was sent is "invalid NACK offset";
 *             the loop resumes from the reported offset, so a NACK re-sends from there),
 *             progress(offset, imageSize) per answer, END -> COMPLETE, logLine("COMPLETE"), then
 *             the restart banner (below). finished(true, "COMPLETE").
 *  - SetRole: connect sequence, SET_ROLE(role u32) -> COMPLETE, banner, finished(true, ...).
 *
 * Connect sequence (Updater.connect()): HELLO up to probeRetries (2) -> INFO ends it. Otherwise
 * the application CLI is probed (probe_application(): an empty line, 300 ms, then "version",
 * cliTimeoutMs; a line "OK version=0.8.1 role=rx" gives the fields; without "role=" the "help"
 * reply decides: "tx" listed -> tx, "rx" -> rx, else "unknown"). No answer at all ->
 * finished(false, "neither the bootloader nor the application CLI answered on this port").
 * With autoBootloader() off -> finished(false, "no bootloader response; put the device in UART
 * update mode (CLI command 'bootloader') or drop --no-auto-bootloader"). Otherwise, when
 * throughSoc() is set, "power" is sent first and its "OK power ..." reply logged as
 * "INFO power soc=<soc> supply=<supply> usb=<usb>" (powerInfo()); then
 * logLine("INFO application version=<v> role=<r>; rebooting it into the UART updater"),
 * "bootloader" with bootloaderCommandTimeoutMs for "OK rebooting_to_bootloader" (else
 * finished(false, "application did not enter the bootloader: <replies joined by '; '>" or
 * "... : no reply")), resetSettleMs, the input discarded, HELLO up to rebootRetries (10).
 *
 * Framed requests (Updater.request()): the sequence number is incremented (16-bit wrap) per
 * request; the frame is sent and an answer with the same sequence and an expected type is awaited
 * for frameTimeoutMs; other frames are ignored; on timeout the same frame is sent again, up to
 * `retries` times in total, then finished(false, "no acceptable response after <n> retries").
 * An INFO payload that is neither 20 nor 24 bytes is "invalid INFO response"; READY / ACK / NACK
 * payloads that are not 4 bytes are "invalid READY response" / "invalid ACK/NACK response".
 *
 * Restart banner (wait_for_application(), bannerTimeoutMs): text lines are read; every line with
 * "version=" contributes its key=value fields; a line with "cli=ready" after that ends the wait.
 * applicationRestarted(fields) + logLine("INFO application restarted: version=<v> role=<r>") when
 * seen, else logLine("INFO application banner not seen within 8 s; check the device manually")
 * and bannerTimeout(). The operation's finished() comes after the banner wait either way.
 *
 * Timeouts and counts default to the Python values and are settable for the tests:
 * frameTimeoutMs 500, frameRetries 5, probeRetries 2, rebootRetries 10, chunkSize 128,
 * cliTimeoutMs 1000, bootloaderCommandTimeoutMs 2000, resetSettleMs 500, bannerTimeoutMs 8000.
 *
 * cancel() stops the current operation at once (finished(false, "cancelled")); a device that was
 * already put into the updater stays there (Part J of the CC1310 runbook: the next flash starts
 * again from byte 0). The device closing (readChannelFinished / aboutToClose) while an operation
 * runs ends it with finished(false, "connection closed").
 *
 * logLine() carries the user-visible lines in fw_update.py's wording ("INFO package ...",
 * "INFO power ...", "INFO application ...", "COMPLETE", the banner lines); progress() replaces
 * the "\rPROGRESS a/b (p%)" counter. Secret-free, safe to show in the tab's log pane.
 */
class McuUpdater : public QObject
{
    Q_OBJECT
public:
    enum class Operation { None, Info, Flash, SetRole };
    Q_ENUM(Operation)

    /// Where the state machine is, for the UI's status line and the tests.
    enum class Phase {
        Idle, ProbeBootloader, ProbeApplication, ReadPower, EnterBootloader, SettleAfterReset,
        RebootProbe, Begin, Transfer, End, SetRole, Banner, Done
    };
    Q_ENUM(Phase)

    /// The application CLI's fields ("version", "role", ... from "OK version=0.8.1 role=rx").
    using CliFields = QMap<QString, QString>;

    explicit McuUpdater(QObject* parent = nullptr);
    ~McuUpdater() override;

    void setDevice(QIODevice* device);          ///< not owned; must be open; replaced only while idle
    QIODevice* device() const;
    void setThroughSoc(bool on);                ///< log the CLI "power" reply before "bootloader"
    bool throughSoc() const;
    void setAutoBootloader(bool on);            ///< default true (false = fw_update.py --no-auto-bootloader)
    bool autoBootloader() const;
    void setPackage(const CcPackage& package);  ///< what flash() sends; logLine(package.summary()) at start
    CcPackage package() const;

    bool isRunning() const;
    Operation operation() const;
    Phase phase() const;
    quint16 sequence() const;                   ///< the last sequence number used

    // ---- Timing (defaults = fw_update.py) ------------------------------------------------
    void setFrameTimeoutMs(int ms);             int frameTimeoutMs() const;
    void setFrameRetries(int count);            int frameRetries() const;
    void setProbeRetries(int count);            int probeRetries() const;
    void setRebootRetries(int count);           int rebootRetries() const;
    void setChunkSize(int bytes);               int chunkSize() const;
    void setCliTimeoutMs(int ms);               int cliTimeoutMs() const;
    void setBootloaderCommandTimeoutMs(int ms); int bootloaderCommandTimeoutMs() const;
    void setResetSettleMs(int ms);              int resetSettleMs() const;
    void setBannerTimeoutMs(int ms);            int bannerTimeoutMs() const;

    /// "OK power state=on supply=usb ..." -> its key=value fields; empty when no such line.
    static CliFields powerFields(const QStringList& lines);
    /// "OK version=0.2.1 role=tx" -> {version: 0.2.1, role: tx} (every token with '=').
    static CliFields parseCliFields(const QString& line);
    /// fw_update.py format_application(): "INFO application version=<v> role=<r>".
    static QString formatApplication(const CliFields& fields);

public slots:
    bool info();                                  ///< Operation::Info
    bool flash(quint32 role);                     ///< Operation::Flash with the set package; role 1 rx / 2 tx
    bool setRole(quint32 role);                   ///< Operation::SetRole
    void cancel();

signals:
    void phaseChanged(McuUpdater::Phase phase);
    void logLine(const QString& line);
    void deviceInfo(const CcProtocol::DeviceInfo& info);        ///< every INFO answer that was accepted
    void applicationInfo(const McuUpdater::CliFields& fields);  ///< the CLI probe's version/role
    void powerInfo(const McuUpdater::CliFields& fields);        ///< the "OK power ..." fields
    void progress(qint64 done, qint64 total);                   ///< bytes the device acknowledged
    void applicationRestarted(const McuUpdater::CliFields& fields);
    void bannerTimeout();
    void finished(bool ok, const QString& message);

private:
    struct Private;
    std::unique_ptr<Private> d;
};

Q_DECLARE_METATYPE(McuUpdater::CliFields)
