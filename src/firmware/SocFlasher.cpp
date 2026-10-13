// STUB (v0.5 scaffolding): replaced by the tools work package.
#include "firmware/SocFlasher.h"

struct SocFlasher::Private
{
    RockchipTool* upgradeTool = nullptr;
    AdbTool* adb = nullptr;
    QString serial;
    QString imagePath;
    int waitTimeoutSeconds = 30;
    int adbReturnTimeoutSeconds = 60;
    int pollIntervalMs = 1000;
    bool waitForAdb = true;
    DeviceStatus status;
};

QString SocFlasher::DeviceStatus::text() const { return QString(); }
bool SocFlasher::DeviceStatus::operator==(const DeviceStatus& o) const
{
    return mode == o.mode && serial == o.serial && detail == o.detail && hint == o.hint;
}

SocFlasher::SocFlasher(ToolRunner* runner, QObject* parent) : QObject(parent), d(new Private)
{
    d->upgradeTool = new RockchipTool(runner, this);
    d->adb = new AdbTool(runner, this);
}
SocFlasher::~SocFlasher() = default;
RockchipTool* SocFlasher::upgradeTool() const { return d->upgradeTool; }
AdbTool* SocFlasher::adb() const { return d->adb; }
void SocFlasher::setSerial(const QString& serial) { d->serial = serial; d->adb->setSerial(serial); }
QString SocFlasher::serial() const { return d->serial; }
void SocFlasher::setImagePath(const QString& path) { d->imagePath = path; }
QString SocFlasher::imagePath() const { return d->imagePath; }
void SocFlasher::setWaitTimeoutSeconds(int s) { d->waitTimeoutSeconds = s; }
int SocFlasher::waitTimeoutSeconds() const { return d->waitTimeoutSeconds; }
void SocFlasher::setAdbReturnTimeoutSeconds(int s) { d->adbReturnTimeoutSeconds = s; }
int SocFlasher::adbReturnTimeoutSeconds() const { return d->adbReturnTimeoutSeconds; }
void SocFlasher::setPollIntervalMs(int ms) { d->pollIntervalMs = ms; }
int SocFlasher::pollIntervalMs() const { return d->pollIntervalMs; }
void SocFlasher::setWaitForAdb(bool on) { d->waitForAdb = on; }
bool SocFlasher::waitForAdb() const { return d->waitForAdb; }
bool SocFlasher::isRunning() const { return false; }
SocFlasher::Phase SocFlasher::phase() const { return Phase::Idle; }
SocFlasher::DeviceStatus SocFlasher::lastStatus() const { return d->status; }
void SocFlasher::queryStatus() {}
bool SocFlasher::start() { return false; }
void SocFlasher::cancel() {}
