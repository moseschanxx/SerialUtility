#pragma once

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QList>
#include <QTimer>
#include <QElapsedTimer>
#include <QHash>
#include <QMap>
#include <optional>
#include <functional>

#include "core/SerialPortEnumerator.h"

/**
 * Built-in simulated serial devices ("SIM:" pseudo-ports).
 *
 * Purpose: let the whole application be exercised - and demonstrated - without hardware:
 * the terminal emulation, line editing, quick commands, logging, hex view, encodings and
 * the auto-reconnect path. They appear in the port list (SerialPortEnumerator appends
 * DeviceSimulator::entries() when AppSettings::showSimulatedPorts() is on) and are opened
 * through the normal SerialConnection API: SerialConnection::open() creates a
 * DeviceSimulator instead of a QSerialPort when SerialSettings::isSimulatedPort() is true.
 *
 * Kinds
 *  - Loopback ("SIM:loopback"): every byte received from the host is echoed back
 *    unchanged, after the pacing delay. No banner.
 *  - Linux ("SIM:linux"): a Rockchip RV1106-style board. On start() it prints a realistic
 *    coloured boot log (U-Boot handoff line, ~40 kernel lines with "[    1.234567]"
 *    timestamps, a few [ OK ] / [FAILED]-style init lines in green/red, one line of
 *    Chinese UTF-8 text), then "rv1106 login: ". Any user name is accepted; "Password: "
 *    is not echoed; then a busybox-like shell with prompt "[root@rv1106:~]# " and
 *    canonical line editing performed BY THE DEVICE (like a real tty): printable bytes are
 *    echoed, 0x7F/0x08 erase with "\b \b", Ctrl+U clears the line, Ctrl+C prints "^C" and
 *    a fresh prompt, Ctrl+L clears the screen (ESC[H ESC[2J) and reprints the prompt,
 *    Ctrl+D at an empty line logs out, Tab is ignored, "\r" executes the line (a following
 *    "\n" is swallowed), arrow keys are ignored. Commands (with realistic output):
 *      help, uname -a, cat /proc/cpuinfo, cat /proc/meminfo, cat /proc/version, free,
 *      ls [-l] [path] (a small fake filesystem: /, /dev, /etc, /root, /oem, /userdata),
 *      pwd, cd <dir>, df -h, ifconfig, ip addr, dmesg (coloured, ~60 lines),
 *      date, uptime, echo <args>, clear, top -n 1 (uses ESC[H ESC[2J, ESC[<r>;<c>H,
 *      bold header, ~12 process rows), ps, color (16-colour, 256-colour and truecolor
 *      test pattern), progress (a "\r"-driven progress bar 0-100% over ~3 s with a
 *      spinner), chinese (a few lines of Chinese text mixed with ASCII), wide (box drawing
 *      + CJK alignment test), stty cols N rows M (acknowledged silently; "stty size"
 *      prints them), env, export, cat <file> for the fake files, sleep N, reboot
 *      (prints "The system is going down for reboot NOW!", then emits vanished(3000):
 *      the device is gone for 3 s and comes back booting), poweroff (vanished(-1): never
 *      returns until the user reconnects manually), exit / logout (back to the login
 *      prompt). Unknown -> "-sh: <cmd>: not found". Output lines end with "\r\n".
 *  - UBoot ("SIM:uboot"): prints the U-Boot banner and "Hit any key to stop autoboot:  3"
 *    counting down once per second by overwriting the digit with "\b" (like real U-Boot).
 *    Any byte during the countdown stops it and shows the "=> " prompt (same line editing
 *    as Linux). Commands: help, version, printenv, setenv <k> <v>, saveenv, bdinfo,
 *    mmc info, mmc list, md <addr>, reset (banner again -> countdown), boot / run bootcmd
 *    (prints "## Booting kernel ..." then switches to Linux behaviour: boot log + login).
 *    If not interrupted, the countdown reaches 0 and boots automatically.
 *  - Mcu ("SIM:mcu"): "BuildAI MCU shell v1.0 (STM32F4 @168MHz)" banner, prompt "> ",
 *    echo of typed characters, lines end with "\r\n". Commands: help, version, reset
 *    (banner again), led on|off|toggle, adc (8 channel readings), temp, uptime,
 *    telemetry on|off (every 2 s prints "[  12.345] temp=36.5C vbat=3.98V rssi=-67dBm"),
 *    echo <text>, AT -> "OK", AT+GMR -> version line + "OK", ATE0/ATE1 (echo off/on),
 *    AT+RST -> "OK" then vanished(1500), any other AT... -> "ERROR". Also accepts "\n"
 *    as a line terminator (many MCU tools send LF).
 *
 * Pacing: output is emitted in chunks through dataReady() at roughly baudRate/10 bytes per
 * second (10 bits per byte), with a chunk every ~20 ms, so a 1500000-baud simulation
 * streams the boot log visibly faster than a 115200 one and the widget's coalesced repaint
 * path is exercised. Host->device bytes are processed immediately (receive()).
 *
 * Presence: after vanished(ms) the pseudo-port is reported absent by isPresent() for that
 * many milliseconds (a static per-port "down until" table), so SerialConnection's reconnect
 * timer sees the port disappear and reappear exactly like an unplugged/rebooted board.
 * ms < 0 means absent until markPresent(portName) is called (SerialConnection calls it
 * when the user explicitly opens the port again).
 *
 * Thread affinity: GUI thread; pure Qt Core; unit-tested in tests/tst_devicesimulator.cpp.
 */
class DeviceSimulator : public QObject
{
    Q_OBJECT
public:
    enum class Kind { Loopback, Linux, UBoot, Mcu };
    Q_ENUM(Kind)

    static QString portPrefix();                              ///< "SIM:"
    static bool isSimulatedPort(const QString& portName);     ///< starts with "SIM:" (case-insensitive)
    static std::optional<Kind> kindFromPortName(const QString& portName);
    static QString portName(Kind kind);                       ///< "SIM:loopback" | "SIM:linux" | "SIM:uboot" | "SIM:mcu"
    static QString description(Kind kind);                    ///< translated, e.g. "Simulated Rockchip Linux console"
    static QList<SerialPortEntry> entries();                  ///< all kinds, manufacturer "BuildAI Simulator", in the order above
    static bool isPresent(const QString& portName);           ///< false while "rebooting" (see vanished())
    static void markPresent(const QString& portName);
    static void markAbsent(const QString& portName, int forMs); ///< forMs < 0 = until markPresent()

    explicit DeviceSimulator(Kind kind, qint32 baudRate = 115200, QObject* parent = nullptr);
    ~DeviceSimulator() override;

    Kind kind() const;
    qint32 baudRate() const;
    void setBaudRate(qint32 baud);                            ///< live change of pacing
    bool isStarted() const;

    /// Power on: begins the banner / boot output (asynchronously, via the pacing timer).
    void start();
    /// Bytes written by the host (terminal keystrokes, quick commands, file sends).
    void receive(const QByteArray& hostToDevice);
    /// Immediately flush all pending output (tests).
    void flushOutput();
    /// Text of everything the device would still send (tests).
    QByteArray pendingOutput() const;

signals:
    /// Bytes from the device to the host (already paced).
    void dataReady(const QByteArray& deviceToHost);
    /// The device "rebooted"/"powered off": the connection must drop. The port is absent
    /// for `returnsAfterMs` ms (< 0: until reopened by the user).
    void vanished(int returnsAfterMs);

private slots:
    void onPaceTimer();
    void onTelemetryTimer();
    void onCountdownTimer();
    void onDelayedAction();

private:
    // Implementation is free to add private members / helpers; the public API above is fixed.
    enum class Stage { Off, Booting, Countdown, Login, Password, Shell, Sleeping, Down };

    /// One queued piece of device output; `delayMs` is a pause taken before it is sent
    /// (boot logs and progress bars pace themselves independently of the baud rate).
    struct Segment
    {
        QByteArray bytes;
        int delayMs = 0;
    };

    void emitText(const QString& text);                       ///< enqueue UTF-8 text (no newline conversion)
    void emitLine(const QString& line);                       ///< text + "\r\n"
    void emitLines(const QStringList& lines, int delayEveryNLines, int delayMs);
    void emitRaw(const QByteArray& bytes, int delayBeforeMs = 0);
    void pause(int ms);                                       ///< pause marker before the next output
    void prompt();
    void handleByte(char byte);                               ///< canonical line editing
    void handleLine(const QString& line);
    void handleShellCommand(const QString& line);
    void handleUBootCommand(const QString& line);
    void handleMcuCommand(const QString& line);
    void bootLinux();
    void bootFromUBoot();
    void showLogin();
    void showUBootBanner();
    void showMcuBanner();
    void showTop();
    void showColorTest();
    void beginCountdown();
    void reboot(int downMs);
    void startProgress();
    void progressStep();
    void eraseLastChar();
    void clearLine();
    void executeLine();
    bool isLinuxShell() const;                                ///< Linux kind, or UBoot kind after booting
    bool runShellTextCommand(const QString& cmd, const QStringList& args, QStringList& out);
    void resetEnvironment();
    void runDelayed(int ms, std::function<void()> action);
    void cancelDelayed();
    int bytesPerTick() const;
    double temperature() const;
    QString resolvePath(const QString& path) const;
    QString expandVariables(const QString& text) const;

    Kind m_kind;
    qint32 m_baud;
    Stage m_stage = Stage::Off;
    QList<Segment> m_segments;                                ///< pending device -> host output
    QTimer m_paceTimer;
    QTimer m_telemetryTimer;
    QTimer m_countdownTimer;
    QTimer m_delayTimer;
    QElapsedTimer m_uptime;
    QString m_lineBuffer;
    QByteArray m_utf8Pending;                                 ///< partial multi-byte character typed by the host
    QString m_user;
    QString m_cwd = QStringLiteral("/root");
    QMap<QString, QString> m_env;                             ///< Linux shell environment (sorted output)
    QMap<QString, QString> m_ubootEnv;                        ///< U-Boot environment (printenv is sorted)
    int m_countdown = 3;
    int m_cols = 80;
    int m_rows = 24;
    int m_escState = 0;                                       ///< ESC sequence swallowing state
    int m_progressStep = 0;
    bool m_swallowLf = false;                                 ///< "\n" right after "\r" is ignored
    bool m_echo = true;
    bool m_ledOn = false;
    bool m_telemetry = false;
    bool m_started = false;
    bool m_uBootMode = false;   ///< UBoot kind currently at the "=>" prompt (vs booted into Linux)
    std::function<void()> m_delayedAction;
};
