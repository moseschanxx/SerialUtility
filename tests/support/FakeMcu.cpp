// STUB (v0.5 scaffolding): replaced by the protocol work package.
#include "support/FakeMcu.h"

#include <QTcpSocket>

struct FakeMcu::Private
{
    QString soc = QStringLiteral("na");
    QString supply = QStringLiteral("usb");
    quint32 role = 1;
    bool inUpdater = false;
    int dropFrames = 0;
    qint64 nackAt = -1;
    bool noBanner = false, refuseBootloader = false, silent = false;
    QByteArray out, image;
    QStringList commands;
};

FakeMcu::FakeMcu(QObject* parent) : QObject(parent), d(new Private) {}
FakeMcu::~FakeMcu() = default;
void FakeMcu::setSoc(const QString& soc) { d->soc = soc; }
void FakeMcu::setSupply(const QString& supply) { d->supply = supply; }
void FakeMcu::setRole(quint32 role) { d->role = role; }
quint32 FakeMcu::role() const { return d->role; }
void FakeMcu::setInUpdater(bool on) { d->inUpdater = on; }
bool FakeMcu::inUpdater() const { return d->inUpdater; }
void FakeMcu::setDropFrames(int count) { d->dropFrames = count; }
void FakeMcu::setNackAt(qint64 offset) { d->nackAt = offset; }
void FakeMcu::setNoBanner(bool on) { d->noBanner = on; }
void FakeMcu::setRefuseBootloader(bool on) { d->refuseBootloader = on; }
void FakeMcu::setSilent(bool on) { d->silent = on; }
void FakeMcu::feed(const QByteArray&) {}
QByteArray FakeMcu::take(int size) { QByteArray r = d->out.left(size); d->out.remove(0, r.size()); return r; }
QByteArray FakeMcu::out() const { return d->out; }
bool FakeMcu::hasOutput() const { return !d->out.isEmpty(); }
QStringList FakeMcu::commands() const { return d->commands; }
QByteArray FakeMcu::image() const { return d->image; }
int FakeMcu::framesReceived() const { return 0; }
int FakeMcu::dataFramesReceived() const { return 0; }

struct FakeMcuServer::Private
{
    FakeMcu* mcu = nullptr;
    QTcpServer server;
    int connections = 0;
};
FakeMcuServer::FakeMcuServer(FakeMcu* mcu, QObject* parent) : QObject(parent), d(new Private) { d->mcu = mcu; }
FakeMcuServer::~FakeMcuServer() = default;
bool FakeMcuServer::listen() { return d->server.listen(QHostAddress::LocalHost, 0); }
quint16 FakeMcuServer::port() const { return d->server.serverPort(); }
FakeMcu* FakeMcuServer::mcu() const { return d->mcu; }
int FakeMcuServer::connectionCount() const { return d->connections; }
bool FakeMcuServer::hasClient() const { return false; }
void FakeMcuServer::dropClient() {}
