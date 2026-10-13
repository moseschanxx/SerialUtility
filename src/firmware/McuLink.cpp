// STUB (v0.5 scaffolding): replaced by the tools work package.
#include "firmware/McuLink.h"

#include <QTcpSocket>

struct McuLink::Private
{
    AdbTool* adb = nullptr;
    QString serial;
    int bridgeStartTimeoutMs = 5000;
};

McuLink::McuLink(ToolRunner* runner, QObject* parent) : QObject(parent), d(new Private)
{
    d->adb = new AdbTool(runner, this);
}
McuLink::~McuLink() = default;
AdbTool* McuLink::adb() const { return d->adb; }
void McuLink::setSerial(const QString& serial) { d->serial = serial; d->adb->setSerial(serial); }
QString McuLink::serial() const { return d->serial; }
void McuLink::setBridgeStartTimeoutMs(int ms) { d->bridgeStartTimeoutMs = ms; }
int McuLink::bridgeStartTimeoutMs() const { return d->bridgeStartTimeoutMs; }
bool McuLink::isOpen() const { return false; }
bool McuLink::isOpening() const { return false; }
quint16 McuLink::localPort() const { return 0; }
QTcpSocket* McuLink::socket() const { return nullptr; }
ToolInvocation* McuLink::cliCommand(const QString& text) { return d->adb->shell(QStringLiteral("rfmcu cmd ") + text); }
bool McuLink::open() { return false; }
void McuLink::close() {}
