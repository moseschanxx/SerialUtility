// STUB (v0.5 scaffolding): replaced by the protocol work package.
#include "firmware/McuUpdater.h"

#include <QIODevice>

struct McuUpdater::Private
{
    QIODevice* device = nullptr;
    bool throughSoc = false;
    bool autoBootloader = true;
    CcPackage package;
    int frameTimeoutMs = 500, frameRetries = 5, probeRetries = 2, rebootRetries = 10, chunkSize = 128;
    int cliTimeoutMs = 1000, bootloaderCommandTimeoutMs = 2000, resetSettleMs = 500, bannerTimeoutMs = 8000;
};

McuUpdater::McuUpdater(QObject* parent) : QObject(parent), d(new Private) {}
McuUpdater::~McuUpdater() = default;
void McuUpdater::setDevice(QIODevice* device) { d->device = device; }
QIODevice* McuUpdater::device() const { return d->device; }
void McuUpdater::setThroughSoc(bool on) { d->throughSoc = on; }
bool McuUpdater::throughSoc() const { return d->throughSoc; }
void McuUpdater::setAutoBootloader(bool on) { d->autoBootloader = on; }
bool McuUpdater::autoBootloader() const { return d->autoBootloader; }
void McuUpdater::setPackage(const CcPackage& package) { d->package = package; }
CcPackage McuUpdater::package() const { return d->package; }
bool McuUpdater::isRunning() const { return false; }
McuUpdater::Operation McuUpdater::operation() const { return Operation::None; }
McuUpdater::Phase McuUpdater::phase() const { return Phase::Idle; }
quint16 McuUpdater::sequence() const { return 0; }
void McuUpdater::setFrameTimeoutMs(int ms) { d->frameTimeoutMs = ms; }
int McuUpdater::frameTimeoutMs() const { return d->frameTimeoutMs; }
void McuUpdater::setFrameRetries(int n) { d->frameRetries = n; }
int McuUpdater::frameRetries() const { return d->frameRetries; }
void McuUpdater::setProbeRetries(int n) { d->probeRetries = n; }
int McuUpdater::probeRetries() const { return d->probeRetries; }
void McuUpdater::setRebootRetries(int n) { d->rebootRetries = n; }
int McuUpdater::rebootRetries() const { return d->rebootRetries; }
void McuUpdater::setChunkSize(int n) { d->chunkSize = n; }
int McuUpdater::chunkSize() const { return d->chunkSize; }
void McuUpdater::setCliTimeoutMs(int ms) { d->cliTimeoutMs = ms; }
int McuUpdater::cliTimeoutMs() const { return d->cliTimeoutMs; }
void McuUpdater::setBootloaderCommandTimeoutMs(int ms) { d->bootloaderCommandTimeoutMs = ms; }
int McuUpdater::bootloaderCommandTimeoutMs() const { return d->bootloaderCommandTimeoutMs; }
void McuUpdater::setResetSettleMs(int ms) { d->resetSettleMs = ms; }
int McuUpdater::resetSettleMs() const { return d->resetSettleMs; }
void McuUpdater::setBannerTimeoutMs(int ms) { d->bannerTimeoutMs = ms; }
int McuUpdater::bannerTimeoutMs() const { return d->bannerTimeoutMs; }
McuUpdater::CliFields McuUpdater::powerFields(const QStringList&) { return {}; }
McuUpdater::CliFields McuUpdater::parseCliFields(const QString&) { return {}; }
QString McuUpdater::formatApplication(const CliFields&) { return QString(); }
bool McuUpdater::info() { return false; }
bool McuUpdater::flash(quint32) { return false; }
bool McuUpdater::setRole(quint32) { return false; }
void McuUpdater::cancel() {}
