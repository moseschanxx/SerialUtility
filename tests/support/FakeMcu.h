#pragma once

#include <QObject>
#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTcpServer>
#include <memory>

#include "firmware/CcProtocol.h"

/**
 * The CC1310 as the host sees it, ported byte for byte from CC1310-Firmware
 * tools/tests/test_adb_link.py (class FakeMcu): first the application CLI, then - after the
 * "bootloader" command - the UART updater.
 *
 * Application mode: feed() splits lines at "\n" (a "\r" before it is stripped); an empty line or
 * one containing a NUL byte is ignored (that is how HELLO bytes sent to the application are
 * swallowed); every command is recorded in commands(). "version" answers
 * "OK version=0.8.1 role=<rx|tx>\r\n" (role 1 rx / 2 tx); "power" answers "OK power state=on
 * supply=<supply> bat=ok usb=<1 when supply is usb, else 0> key=0 latch=1 soc=<soc>
 * charge=on\r\n"; "bootloader" answers "OK rebooting_to_bootloader\r\n" and switches to updater
 * mode; "help" answers the real firmware's rows: "OK commands:", "OK help [command]",
 * "OK version show firmware version", "OK power ...", "OK bootloader ..." and, last, the role's
 * command row "OK rx rx status | rx dump on|off" (rx) or "OK tx tx sync_frame ... | tx status" (tx),
 * anything else "ERR CMD unknown_command; use help\r\n".
 *
 * Updater mode: feed() splits frames at 0x00 and decodes each with CcProtocol::decodeFrame
 * (undecodable ones are ignored): HELLO -> INFO(24 bytes: target 0x4343314D, state 2, max size
 * 0x17000, image size = bytes received so far, version 0x000800, role); BEGIN -> the image is
 * cleared, the role taken from payload[32..36], READY(0); DATA(offset + bytes) -> appended when
 * offset == bytes received so far, ACK(bytes received so far) - a DATA with a stale offset is thus
 * answered with the real offset like a NACK would be; END -> COMPLETE, mode back to application,
 * then the banner "CC1310 boot version=0.8.1 role=<role>\r\nOK cli=ready\r\n"; SET_ROLE -> the
 * role is taken, COMPLETE, mode application, the banner.
 *
 * Fault injection for the McuUpdater tests (all off by default):
 *  - setDropFrames(n): the first n DATA frames get no answer at all (a lost frame; the updater
 *    must retry the same offset);
 *  - setNackAt(offset): the first DATA at `offset` is answered with NACK(offset - chunk) after
 *    the image is truncated to that point (resume from an earlier offset);
 *  - setNoBanner(true): nothing is printed after COMPLETE;
 *  - setRefuseBootloader(true): "bootloader" answers "ERR PWR busy\r\n" and stays in the app;
 *  - setSilent(true): nothing answers at all (neither CLI nor updater).
 * take(size) returns up to `size` bytes of pending output (what the host reads); out() the whole
 * pending buffer.
 */
class FakeMcu : public QObject
{
    Q_OBJECT
public:
    static constexpr quint32 kTarget = 0x4343314D;
    static constexpr quint32 kVersion = 0x000800;

    explicit FakeMcu(QObject* parent = nullptr);
    ~FakeMcu() override;

    void setSoc(const QString& soc);            ///< "na" (default), "on", "off"
    void setSupply(const QString& supply);      ///< "usb" (default), "bat", "unknown"
    void setRole(quint32 role);                 ///< 1 rx (default), 2 tx
    quint32 role() const;
    void setInUpdater(bool on);                 ///< start in updater mode
    bool inUpdater() const;

    void setDropFrames(int count);
    void setNackAt(qint64 offset);
    void setNoBanner(bool on);
    void setRefuseBootloader(bool on);
    void setSilent(bool on);

    void feed(const QByteArray& data);          ///< bytes from the host
    QByteArray take(int size);                  ///< up to `size` pending output bytes
    QByteArray out() const;
    bool hasOutput() const;

    QStringList commands() const;               ///< CLI commands received, in order
    QByteArray image() const;                   ///< bytes received through DATA
    int framesReceived() const;
    int dataFramesReceived() const;

signals:
    void outputAvailable();                     ///< something was appended to the output

private:
    struct Private;
    std::unique_ptr<Private> d;
};

/**
 * A TCP server standing in for `rfmcu bridge` behind an adb forward (test_adb_link.py
 * BridgeServer): one client at a time, bytes from the client go to FakeMcu::feed(), the MCU's
 * output goes back to the client as soon as it appears. Listens on 127.0.0.1, an ephemeral port
 * (port()). Tests connect a QTcpSocket to it and hand the socket to McuUpdater, or have
 * FakeToolRunner's "adb forward" handler print port().
 */
class FakeMcuServer : public QObject
{
    Q_OBJECT
public:
    explicit FakeMcuServer(FakeMcu* mcu, QObject* parent = nullptr);
    ~FakeMcuServer() override;

    bool listen();
    quint16 port() const;
    FakeMcu* mcu() const;
    int connectionCount() const;
    bool hasClient() const;
    void dropClient();                          ///< close the client socket (a bridge that died)

signals:
    void clientConnected();
    void clientDisconnected();

private:
    struct Private;
    std::unique_ptr<Private> d;
};
