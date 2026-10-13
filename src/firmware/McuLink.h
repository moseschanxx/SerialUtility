#pragma once

#include <QObject>
#include <QString>
#include <memory>

#include "firmware/AdbTool.h"

class QTcpSocket;

/**
 * The road from this PC to the CC1310 through the Node board's SoC, as adb_link.py's AdbLink:
 *
 *   McuUpdater -- QTcpSocket 127.0.0.1:<local> -- adb forward -- USB-C -- board: rfmcu bridge -- /dev/ttyS1 -- CC1310
 *
 * open() runs, each step asynchronous through AdbTool, stopping with failed(message) on the
 * first error (messages as adb_link.py):
 *  1. `adb devices` -> AdbTool::selectDevice() (the only ready board, or serial()); the serial is
 *     reported in serialSelected() and used for every later command;
 *  2. `adb shell "command -v rfmcu || which rfmcu || true"`: no "rfmcu" in the output ->
 *     "board <serial> has no rfmcu: <socFirmwareHint>";
 *  3. `adb shell "killall rfmcu 2>/dev/null; true"` (an rfmcu left by an earlier session holds
 *     the UART's flock; its exit status is ignored);
 *  4. `adb forward tcp:0 tcp:7000` -> local port, or "adb forward did not report a local port
 *     (got '...'); update adb";
 *  5. `adb shell rfmcu bridge 7000` as a long-running process; its output is watched for a line
 *     containing "bridging" within bridgeStartTimeoutMs (5 s); the process ending first, or the
 *     timeout, is "rfmcu bridge did not start on board <serial>: <its lines joined by '; '>" (or
 *     "no output") and close() is run;
 *  6. a QTcpSocket connects to 127.0.0.1:<local port>; connected -> opened(serial, localPort),
 *     logLine("INFO adb board=<serial> bridge=socket://127.0.0.1:<port> (rfmcu bridge, /dev/ttyS1)").
 *
 * close() (idempotent; also run by the destructor): the socket is closed, the bridge process
 * cancelled (terminate, kill after 3 s), then on a board that was selected `killall rfmcu` again
 * (adbd does not reliably end the board process) and `adb forward --remove tcp:<local>`; adb
 * errors during close are ignored (cable pulled). closed() when done. The bridge process ending
 * on its own while open (cable pulled, rfmcu killed by someone) -> linkLost(message) and the
 * state is closed the same way.
 *
 * The socket is owned by the link and valid between opened() and closed(); hand it to
 * McuUpdater::setDevice(). cliCommand() is `adb shell rfmcu cmd <text>` for the tab's "read the
 * MCU version / role" without opening the bridge (the exit status 0 = OK, 1 = ERR, 2 = port
 * busy, 3 = no reply, as rfmcu documents; output lines start with "OK" / "ERR").
 */
class McuLink : public QObject
{
    Q_OBJECT
public:
    static constexpr quint16 kRemotePort = 7000;

    explicit McuLink(ToolRunner* runner, QObject* parent = nullptr);
    ~McuLink() override;

    AdbTool* adb() const;                       ///< path and serial are set here
    void setSerial(const QString& serial);      ///< empty = the only attached board
    QString serial() const;                     ///< the selected one once open() passed step 1
    void setBridgeStartTimeoutMs(int ms);       ///< default 5000
    int bridgeStartTimeoutMs() const;

    bool isOpen() const;                        ///< opened() emitted and not yet closed
    bool isOpening() const;
    quint16 localPort() const;                  ///< 0 while not forwarded
    QTcpSocket* socket() const;                 ///< null while not open

    ToolInvocation* cliCommand(const QString& text);   ///< adb shell rfmcu cmd <text> (serial() or the only board)

public slots:
    bool open();                                ///< false while opening or open
    void close();

signals:
    void serialSelected(const QString& serial);
    void opened(const QString& serial, quint16 localPort);
    void failed(const QString& message);        ///< open() did not get through; the link is closed again
    void linkLost(const QString& message);      ///< the bridge ended while open; the link is closed
    void closed();
    void logLine(const QString& line);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
