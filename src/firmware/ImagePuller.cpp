// STUB (v0.5 scaffolding): replaced by the ssh-pull work package.
#include "firmware/ImagePuller.h"

struct ImagePuller::Private
{
    SshConnection* connection = nullptr;
    QString remoteFolder, cacheDir, sdkDir;
    bool gitDescribe = true;
    bool force = false;
    QStringList extra;
};

QString ImagePuller::CacheEntry::summary() const { return QString(); }
ImagePuller::ImagePuller(QObject* parent) : QObject(parent), d(new Private) {}
ImagePuller::~ImagePuller() = default;
void ImagePuller::setConnection(SshConnection* c) { d->connection = c; }
SshConnection* ImagePuller::connection() const { return d->connection; }
void ImagePuller::setRemoteFolder(const QString& f) { d->remoteFolder = f; }
QString ImagePuller::remoteFolder() const { return d->remoteFolder; }
void ImagePuller::setCacheDirectory(const QString& dir) { d->cacheDir = dir; }
QString ImagePuller::cacheDirectory() const { return d->cacheDir; }
void ImagePuller::setSdkDirectory(const QString& dir) { d->sdkDir = dir; }
QString ImagePuller::sdkDirectory() const { return d->sdkDir; }
void ImagePuller::setGitDescribe(bool on) { d->gitDescribe = on; }
bool ImagePuller::gitDescribe() const { return d->gitDescribe; }
void ImagePuller::setForceDownload(bool on) { d->force = on; }
bool ImagePuller::forceDownload() const { return d->force; }
void ImagePuller::setExtraFiles(const QStringList& names) { d->extra = names; }
QStringList ImagePuller::extraFiles() const { return d->extra; }
bool ImagePuller::isRunning() const { return false; }
QString ImagePuller::localImagePath() const { return QString(); }
QString ImagePuller::imagePath(const QString& cacheDir) { return cacheDir + QLatin1Char('/') + QLatin1String(kImageName); }
QString ImagePuller::previousImagePath(const QString& cacheDir) { return cacheDir + QLatin1Char('/') + QLatin1String(kPreviousImageName); }
ImagePuller::CacheEntry ImagePuller::cachedImage(const QString&) { return {}; }
ImagePuller::CacheEntry ImagePuller::previousImage(const QString&) { return {}; }
bool ImagePuller::rollBack(const QString&, QString* error)
{
    if (error) *error = QStringLiteral("not implemented");
    return false;
}
bool ImagePuller::writeNote(const CacheEntry&, QString* error)
{
    if (error) *error = QStringLiteral("not implemented");
    return false;
}
ImagePuller::CacheEntry ImagePuller::readNote(const QString&) { return {}; }
bool ImagePuller::start() { return false; }
void ImagePuller::cancel() {}
