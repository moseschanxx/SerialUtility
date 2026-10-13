#include "firmware/ImagePuller.h"

#include "app/Logging.h"
#include "ssh/SshConnection.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QPointer>
#include <QSaveFile>
#include <QTimeZone>
#include <QTimer>

#include <utility>

namespace {

constexpr int kGitDescribeTimeoutMs = 15000;   ///< `git describe` on a big checkout; never worth more

// ---- Remote paths ------------------------------------------------------------------------
// The SshConnection calls take absolute paths or paths relative to the remote home (SFTP and an
// exec channel both start there), and neither SFTP nor a single-quoted shell word expands "~".
// So "~/rk/x" becomes "rk/x" and "~" becomes ".", before anything is sent.

QString normalizeRemotePath(const QString& path)
{
    QString text = path.trimmed();
    while (text.size() > 1 && text.endsWith(QLatin1Char('/'))) {
        text.chop(1);
    }
    if (text == QLatin1String("~")) {
        return QStringLiteral(".");
    }
    if (text.startsWith(QLatin1String("~/"))) {
        text.remove(0, 2);
        while (text.startsWith(QLatin1Char('/'))) {
            text.remove(0, 1);
        }
        return text.isEmpty() ? QStringLiteral(".") : text;
    }
    return text;
}

QString joinRemotePath(const QString& folder, const QString& name)
{
    if (folder == QLatin1String(".") || folder.isEmpty()) {
        return name;
    }
    return folder.endsWith(QLatin1Char('/')) ? folder + name : folder + QLatin1Char('/') + name;
}

/// The parent of a normalized remote path ("a/b" -> "a", "a" -> ".", "/a" -> "/").
QString parentOf(const QString& path)
{
    if (path == QLatin1String("/") || path == QLatin1String(".")) {
        return path;
    }
    const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash < 0) {
        return QStringLiteral(".");
    }
    return slash == 0 ? QStringLiteral("/") : path.left(slash);
}

/// `'text'` with every embedded quote written as '\'' (the only way text reaches a command line).
QString shellQuote(const QString& text)
{
    QString quoted = text;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

/// What an extra file name may look like: one plain file name, nothing that could leave the
/// cache folder (separators, "..", a Windows drive or stream colon) or hit the image files.
bool isPlainFileName(const QString& name)
{
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        return false;
    }
    for (const QChar ch : name) {
        if (ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char(':') || ch.unicode() < 0x20) {
            return false;
        }
    }
    return !name.startsWith(QLatin1String(ImagePuller::kImageName), Qt::CaseInsensitive)
        && !name.startsWith(QLatin1String(ImagePuller::kPreviousImageName), Qt::CaseInsensitive);
}

// ---- Texts -------------------------------------------------------------------------------

QString sizeText(qint64 bytes)
{
    // Decimal units like the build machine's `ls -h` and the message of the old puller
    // (11354755 -> "11.4 MB"), independent of the user's locale.
    return QLocale::c().formattedDataSize(bytes, 1, QLocale::DataSizeSIFormat);
}

QString timeText(const QDateTime& when)
{
    return when.isValid() ? when.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")) : QStringLiteral("?");
}

/// The skip rule compares mtimes with second precision (SFTP and `stat %Y` carry no more).
bool sameSecond(const QDateTime& a, const QDateTime& b)
{
    return a.isValid() && b.isValid() && a.toSecsSinceEpoch() == b.toSecsSinceEpoch();
}

QString lastLine(const QByteArray& output)
{
    const QStringList lines = QString::fromUtf8(output).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return lines.isEmpty() ? QString() : lines.last().trimmed();
}

// ---- Files -------------------------------------------------------------------------------

/// True when `path` starts with the RKFW magic of a Rockchip update package. Local on purpose:
/// the integrator switches the call in completeImage() to RockchipTool::isRkfwImage().
bool hasRkfwMagic(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) && file.read(4) == QByteArrayLiteral("RKFW");
}

bool removeIfExists(const QString& path)
{
    return !QFileInfo::exists(path) || QFile::remove(path);
}

bool renameFile(const QString& from, const QString& to, QString* error)
{
    QFile file(from);
    if (file.rename(to)) {
        return true;
    }
    if (error) {
        *error = ImagePuller::tr("Cannot rename %1 to %2: %3")
                     .arg(QDir::toNativeSeparators(from), QDir::toNativeSeparators(to), file.errorString());
    }
    return false;
}

/// Puts `source` at `target` (which may exist) so that a failure leaves the old `target` in place:
/// the old file is moved aside first and moved back when the rename of `source` fails.
bool replaceFile(const QString& source, const QString& target, QString* error)
{
    if (!QFileInfo::exists(target)) {
        return renameFile(source, target, error);
    }
    const QString aside = target + QStringLiteral(".replaced");
    if (!removeIfExists(aside)) {
        if (error) {
            *error = ImagePuller::tr("Cannot remove %1").arg(QDir::toNativeSeparators(aside));
        }
        return false;
    }
    if (!renameFile(target, aside, error)) {
        return false;
    }
    if (!renameFile(source, target, error)) {
        QString ignored;
        renameFile(aside, target, &ignored);
        return false;
    }
    QFile::remove(aside);
    return true;
}

/// Exchanges two files (either may be missing: the other one then just moves). Everything is
/// put back when a step fails, so the pair never ends up with one file lost.
bool swapFiles(const QString& a, const QString& b, QString* error)
{
    const bool haveA = QFileInfo::exists(a);
    const bool haveB = QFileInfo::exists(b);
    if (!haveA && !haveB) {
        return true;
    }
    if (!haveA) {
        return renameFile(b, a, error);
    }
    if (!haveB) {
        return renameFile(a, b, error);
    }
    const QString aside = a + QStringLiteral(".swap");
    if (!removeIfExists(aside)) {
        if (error) {
            *error = ImagePuller::tr("Cannot remove %1").arg(QDir::toNativeSeparators(aside));
        }
        return false;
    }
    QString ignored;
    if (!renameFile(a, aside, error)) {
        return false;
    }
    if (!renameFile(b, a, error)) {
        renameFile(aside, a, &ignored);
        return false;
    }
    if (!renameFile(aside, b, error)) {
        renameFile(a, b, &ignored);
        renameFile(aside, a, &ignored);
        return false;
    }
    return true;
}

QString noteFileOf(const QString& image)
{
    return image + QLatin1String(ImagePuller::kNoteSuffix);
}

/// One-level history: `newImage` becomes `<cache>/update.img`. Unless `keepPrevious`, the current
/// image first becomes `update.prev.img` (the older previous one and its note are dropped) and its
/// note follows it. A failing step leaves the cache as it was.
bool putIntoPlace(const QString& cacheDir, const QString& newImage, bool keepPrevious, QString* error)
{
    const QString image = ImagePuller::imagePath(cacheDir);
    if (keepPrevious || !QFileInfo::exists(image)) {
        return replaceFile(newImage, image, error);
    }
    const QString previous = ImagePuller::previousImagePath(cacheDir);
    if (!removeIfExists(previous) || !removeIfExists(noteFileOf(previous))) {
        if (error) {
            *error = ImagePuller::tr("Cannot remove the old %1").arg(QDir::toNativeSeparators(previous));
        }
        return false;
    }
    if (!renameFile(image, previous, error)) {
        return false;
    }
    if (QFileInfo::exists(noteFileOf(image))) {
        QString ignored;
        if (!renameFile(noteFileOf(image), noteFileOf(previous), &ignored)) {
            QFile::remove(noteFileOf(image));   // a note that stayed behind would describe the new image
        }
    }
    if (!renameFile(newImage, image, error)) {
        QString ignored;
        renameFile(previous, image, &ignored);
        renameFile(noteFileOf(previous), noteFileOf(image), &ignored);
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------------------
// Private: one pull
// ---------------------------------------------------------------------------------------

struct ImagePuller::Private
{
    /// What the run is waiting for; the answer of anything else is ignored.
    enum class Step { Idle, Stat, Git, Download, ExtraStat, ExtraDownload };

    explicit Private(ImagePuller* owner)
        : q(owner)
    {
    }

    ImagePuller* q;
    QPointer<SshConnection> connection;
    QString remoteFolder;
    QString cacheDir;
    QString sdkDir;
    bool gitDescribe = true;
    bool force = false;
    QStringList extra;

    // ---- the run in progress ----
    bool running = false;
    quint64 runToken = 0;                 ///< lets a deferred failure tell its own run from a later one
    Step step = Step::Idle;
    bool cancelRequested = false;
    int pendingId = 0;                    ///< the statRemote() / runCommand() id being waited for
    QList<QMetaObject::Connection> links;
    QString localImage;                   ///< localImagePath()
    QString runCache;                     ///< the cache folder of this run
    QString runTarget;                    ///< "user@host:port" for notes and messages
    QString runFolder;                    ///< normalized remote folder (relative to the home, or absolute)
    QString runImage;                     ///< runFolder/update.img
    QString runNew;                       ///< <cache>/update.img.new
    SshConnection::RemoteFileInfo remote;
    QString git;
    QStringList extraQueue;
    QString currentExtra;
    int extrasPulled = 0;

    void log(const QString& line)
    {
        qCInfo(lcFirmware).noquote() << "image pull:" << line;
        emit q->logLine(line);
    }

    void begin();
    void failLater(const QString& message);
    void finish(bool ok, const QString& message);
    void abandon();
    void connectConnection(SshConnection* c);

    void onStat(int id, const SshConnection::RemoteFileInfo& info);
    void onCommand(int id, int exitStatus, const QByteArray& output);
    void onTransferFinished(bool ok, const QString& message);
    void onStateChanged(Transport::State state);

    void runGitDescribe();
    void remoteFactsComplete();
    void startImageDownload();
    void completeImage();
    void startNextExtra();
    void startExtraDownload();
    bool sdkPath(QString* sdk) const;
};

void ImagePuller::Private::begin()
{
    running = true;
    ++runToken;
    cancelRequested = false;
    step = Step::Idle;
    pendingId = 0;
    localImage.clear();
    runNew.clear();                       // finish() must not touch a previous run's files
    remote = SshConnection::RemoteFileInfo();
    git.clear();
    extraQueue.clear();
    currentExtra.clear();
    extrasPulled = 0;

    SshConnection* c = connection.data();
    if (!c || c->state() != Transport::State::Connected) {
        failLater(ImagePuller::tr("Not connected"));
        return;
    }
    if (remoteFolder.trimmed().isEmpty()) {
        failLater(ImagePuller::tr("No remote folder given"));
        return;
    }
    if (cacheDir.trimmed().isEmpty()) {
        failLater(ImagePuller::tr("No cache folder given"));
        return;
    }
    runCache = QDir::cleanPath(cacheDir);
    if (!QDir().mkpath(runCache)) {
        failLater(ImagePuller::tr("Cannot create the cache folder %1").arg(QDir::toNativeSeparators(runCache)));
        return;
    }
    runTarget = c->profile().displayTarget();
    runFolder = normalizeRemotePath(remoteFolder);
    runImage = joinRemotePath(runFolder, QLatin1String(kImageName));
    runNew = ImagePuller::imagePath(runCache) + QStringLiteral(".new");
    // Leftovers of a run that was cut short (the transfer itself writes "<file>.part").
    QFile::remove(runNew);
    QFile::remove(runNew + QStringLiteral(".part"));

    connectConnection(c);
    log(ImagePuller::tr("Looking for %1 in %2 on %3").arg(QLatin1String(kImageName), remoteFolder, runTarget));
    step = Step::Stat;
    pendingId = c->statRemote(runImage);
    if (pendingId == 0) {
        step = Step::Idle;
        failLater(ImagePuller::tr("Not connected"));
    }
}

void ImagePuller::Private::connectConnection(SshConnection* c)
{
    links.append(QObject::connect(c, &SshConnection::remoteStatReceived, q,
                                  [this](int id, const QString&, const SshConnection::RemoteFileInfo& info) { onStat(id, info); }));
    links.append(QObject::connect(c, &SshConnection::commandFinished, q,
                                  [this](int id, int exitStatus, const QByteArray& output, const QString&) {
                                      onCommand(id, exitStatus, output);
                                  }));
    links.append(QObject::connect(c, &SshConnection::transferProgress, q, [this](qint64 done, qint64 total) {
        if (running && (step == Step::Download || step == Step::ExtraDownload)) {
            emit q->progress(done, total);
        }
    }));
    links.append(QObject::connect(c, &SshConnection::transferFinished, q,
                                  [this](bool ok, const QString& message) { onTransferFinished(ok, message); }));
    links.append(QObject::connect(c, &Transport::stateChanged, q,
                                  [this](Transport::State state) { onStateChanged(state); }));
    links.append(QObject::connect(c, &QObject::destroyed, q, [this] {
        finish(false, ImagePuller::tr("The connection was closed"));
    }));
}

void ImagePuller::Private::failLater(const QString& message)
{
    // start() has to return before the owner hears of a failure.
    const quint64 token = runToken;
    QTimer::singleShot(0, q, [this, token, message] {
        if (running && runToken == token) {
            finish(false, message);
        }
    });
}

void ImagePuller::Private::finish(bool ok, const QString& message)
{
    if (!running) {
        return;
    }
    // Everything is settled before the signal: a receiver may start the next pull from it.
    const bool userCancelled = cancelRequested;
    running = false;
    step = Step::Idle;
    pendingId = 0;
    for (const QMetaObject::Connection& link : std::as_const(links)) {
        QObject::disconnect(link);
    }
    links.clear();
    if (ok || userCancelled) {   // a cancel is what the user asked for, not a problem
        qCInfo(lcFirmware).noquote() << "image pull finished:" << message;
    } else {
        qCWarning(lcFirmware).noquote() << "image pull failed:" << message;
    }
    if (!ok && !runNew.isEmpty()) {
        // A download that did not end in a validated image leaves nothing behind.
        QFile::remove(runNew);
        QFile::remove(runNew + QStringLiteral(".part"));
    }
    emit q->finished(ok, message);
}

void ImagePuller::Private::abandon()
{
    // The puller is being destroyed in the middle of a pull: no signal, no more listening.
    for (const QMetaObject::Connection& link : std::as_const(links)) {
        QObject::disconnect(link);
    }
    links.clear();
    if (connection && (step == Step::Download || step == Step::ExtraDownload)) {
        connection->cancelTransfer();   // the connection removes the ".part" it writes
    }
    if (!runNew.isEmpty()) {
        QFile::remove(runNew);
    }
    running = false;
    step = Step::Idle;
}

// ---- 1. + 2.: what the build machine has ---------------------------------------------------

void ImagePuller::Private::onStat(int id, const SshConnection::RemoteFileInfo& info)
{
    if (!running || id != pendingId) {
        return;
    }
    pendingId = 0;
    if (step == Step::Stat) {
        if (!info.error.isEmpty()) {
            finish(false, ImagePuller::tr("Cannot read %1 in %2 on %3: %4")
                              .arg(QLatin1String(kImageName), remoteFolder, runTarget, info.error));
            return;
        }
        if (!info.exists) {
            finish(false, ImagePuller::tr("no %1 in %2 on %3").arg(QLatin1String(kImageName), remoteFolder, runTarget));
            return;
        }
        if (info.isDirectory) {
            finish(false, ImagePuller::tr("%1 in %2 on %3 is a folder, not a file")
                              .arg(QLatin1String(kImageName), remoteFolder, runTarget));
            return;
        }
        remote = info;
        log(ImagePuller::tr("%1 on %2: %3, %4")
                .arg(QLatin1String(kImageName), runTarget, remote.size >= 0 ? sizeText(remote.size) : QStringLiteral("?"),
                     timeText(remote.modified)));
        if (gitDescribe) {
            runGitDescribe();
        } else {
            remoteFactsComplete();
        }
    } else if (step == Step::ExtraStat) {
        if (!info.error.isEmpty() || !info.exists || info.isDirectory) {
            log(ImagePuller::tr("Skipping %1: %2")
                    .arg(currentExtra, !info.error.isEmpty() ? info.error
                                                             : (info.exists ? ImagePuller::tr("it is a folder")
                                                                            : ImagePuller::tr("not found on the build machine"))));
            startNextExtra();
            return;
        }
        startExtraDownload();
    }
}

bool ImagePuller::Private::sdkPath(QString* sdk) const
{
    // The folder's grandparent ("<sdk>/output/image" -> "<sdk>") unless the owner named it.
    *sdk = sdkDir.trimmed().isEmpty() ? parentOf(parentOf(runFolder)) : normalizeRemotePath(sdkDir);
    return !sdk->isEmpty();
}

void ImagePuller::Private::runGitDescribe()
{
    QString sdk;
    SshConnection* c = connection.data();
    if (!sdkPath(&sdk) || !c) {
        remoteFactsComplete();
        return;
    }
    step = Step::Git;
    pendingId = c->runCommand(QStringLiteral("git -C ") + shellQuote(sdk) + QStringLiteral(" describe --tags --dirty 2>/dev/null"),
                              kGitDescribeTimeoutMs);
    if (pendingId == 0) {   // cannot happen while Connected; the version is a nicety, go on without it
        remoteFactsComplete();
    }
}

void ImagePuller::Private::onCommand(int id, int exitStatus, const QByteArray& output)
{
    if (!running || step != Step::Git || id != pendingId) {
        return;
    }
    pendingId = 0;
    if (exitStatus == 0) {
        git = lastLine(output);
        log(git.isEmpty() ? ImagePuller::tr("git describe printed nothing") : ImagePuller::tr("git describe: %1").arg(git));
    } else {
        log(ImagePuller::tr("No git version (git describe ended with status %1)").arg(exitStatus));
    }
    remoteFactsComplete();
}

// ---- 3. skip rule ---------------------------------------------------------------------------

void ImagePuller::Private::remoteFactsComplete()
{
    const quint64 token = runToken;
    emit q->remoteInfo(QLatin1String(kImageName), remote.size, remote.modified, git);
    if (!running || runToken != token) {
        return;   // a slot of remoteInfo() cancelled (or cancelled and started again): not this run's transfer
    }

    const CacheEntry cached = ImagePuller::readNote(ImagePuller::imagePath(runCache));
    const bool sameBuild = cached.isValid() && remote.size >= 0 && cached.size == remote.size
        && sameSecond(cached.remoteModified, remote.modified);
    if (sameBuild && !force) {
        const QString message = ImagePuller::tr("%1 unchanged (%2, %3)")
                                    .arg(QLatin1String(kImageName), sizeText(remote.size), timeText(remote.modified));
        localImage = ImagePuller::imagePath(runCache);
        emit q->skipped(message);
        finish(true, message);
        return;
    }
    startImageDownload();
}

// ---- 4. download ----------------------------------------------------------------------------

void ImagePuller::Private::startImageDownload()
{
    SshConnection* c = connection.data();
    SshConnection::TransferRequest request;
    request.direction = SshConnection::TransferDirection::Download;
    request.localPath = runNew;
    request.remotePath = runImage;
    request.overwrite = true;
    request.preservePermissions = false;
    step = Step::Download;
    log(ImagePuller::tr("Downloading %1 (%2)").arg(QLatin1String(kImageName), remote.size >= 0 ? sizeText(remote.size) : QStringLiteral("?")));
    if (!c || !c->startTransfer(request)) {
        finish(false, ImagePuller::tr("Cannot start the download: the connection is busy or closed"));
    }
}

void ImagePuller::Private::onTransferFinished(bool ok, const QString& message)
{
    if (!running) {
        return;
    }
    if (step == Step::Download) {
        if (cancelRequested) {
            finish(false, ImagePuller::tr("cancelled"));
        } else if (!ok) {
            finish(false, message);
        } else {
            completeImage();
        }
    } else if (step == Step::ExtraDownload) {
        if (cancelRequested) {
            finish(false, ImagePuller::tr("cancelled"));
            return;
        }
        if (ok) {
            ++extrasPulled;
            log(ImagePuller::tr("Pulled %1").arg(currentExtra));
        } else {
            log(ImagePuller::tr("Skipping %1: %2").arg(currentExtra, message));
        }
        startNextExtra();
    }
}

// ---- 5. + 6.: validation, rotation, note ---------------------------------------------------

void ImagePuller::Private::completeImage()
{
    const QFileInfo downloaded(runNew);
    if (!downloaded.isFile()) {
        finish(false, ImagePuller::tr("The download finished but %1 is missing").arg(QDir::toNativeSeparators(runNew)));
        return;
    }
    if (!hasRkfwMagic(runNew)) {
        finish(false, ImagePuller::tr("downloaded file is not a Rockchip RKFW image"));
        return;
    }
    if (remote.size >= 0 && downloaded.size() != remote.size) {
        finish(false, ImagePuller::tr("Downloaded %1 bytes but %2 is %3 bytes on %4 (it changed during the download; pull again)")
                          .arg(downloaded.size())
                          .arg(QLatin1String(kImageName))
                          .arg(remote.size)
                          .arg(runTarget));
        return;
    }

    // Pulling the same build again (forceDownload) replaces the image in place; rotating would
    // push the real previous image out of the history for a copy of the current one.
    const CacheEntry cached = ImagePuller::readNote(ImagePuller::imagePath(runCache));
    const bool sameBuild = cached.isValid() && cached.size == remote.size && sameSecond(cached.remoteModified, remote.modified);
    QString error;
    if (!putIntoPlace(runCache, runNew, sameBuild, &error)) {
        finish(false, error);
        return;
    }

    CacheEntry entry;
    entry.path = ImagePuller::imagePath(runCache);
    entry.size = downloaded.size();
    entry.remoteModified = remote.modified;
    entry.pulledAt = QDateTime::currentDateTimeUtc();
    entry.target = runTarget;
    entry.remoteFolder = remoteFolder;
    entry.gitDescribe = git;
    if (!ImagePuller::writeNote(entry, &error)) {
        // The image is in place; only the skip rule loses its note.
        log(ImagePuller::tr("Cannot write the note of %1: %2").arg(QLatin1String(kImageName), error));
    }
    localImage = entry.path;

    // 7. extra files, plain names only
    extraQueue.clear();
    for (const QString& name : std::as_const(extra)) {
        const QString trimmed = name.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        if (!isPlainFileName(trimmed)) {
            log(ImagePuller::tr("Skipping %1: not a plain file name").arg(trimmed));
        } else if (!extraQueue.contains(trimmed)) {
            extraQueue.append(trimmed);
        }
    }
    startNextExtra();
}

// ---- 7. extra files -------------------------------------------------------------------------

void ImagePuller::Private::startNextExtra()
{
    SshConnection* c = connection.data();
    if (extraQueue.isEmpty() || !c) {
        const qint64 size = QFileInfo(localImage).size();
        finish(true, ImagePuller::tr("pulled %1 (%2) from %3:%4")
                         .arg(QLatin1String(kImageName), sizeText(size), runTarget, remoteFolder));
        return;
    }
    currentExtra = extraQueue.takeFirst();
    step = Step::ExtraStat;
    pendingId = c->statRemote(joinRemotePath(runFolder, currentExtra));
    if (pendingId == 0) {
        finish(false, ImagePuller::tr("Not connected"));
    }
}

void ImagePuller::Private::startExtraDownload()
{
    SshConnection* c = connection.data();
    SshConnection::TransferRequest request;
    request.direction = SshConnection::TransferDirection::Download;
    request.localPath = QDir(runCache).filePath(currentExtra);
    request.remotePath = joinRemotePath(runFolder, currentExtra);
    request.overwrite = true;
    request.preservePermissions = false;
    step = Step::ExtraDownload;
    if (!c || !c->startTransfer(request)) {
        log(ImagePuller::tr("Skipping %1: cannot start the download").arg(currentExtra));
        startNextExtra();
    }
}

// ---- The connection going away ------------------------------------------------------------

void ImagePuller::Private::onStateChanged(Transport::State state)
{
    if (!running || state == Transport::State::Connected) {
        return;
    }
    const QString reason = connection ? connection->errorString() : QString();
    finish(false, reason.isEmpty() ? ImagePuller::tr("The connection was lost") : reason);
}

// ---------------------------------------------------------------------------------------
// CacheEntry
// ---------------------------------------------------------------------------------------

QString ImagePuller::CacheEntry::summary() const
{
    if (!isValid()) {
        return QString();
    }
    QString text = QStringLiteral("%1 %2").arg(QFileInfo(path).fileName(), sizeText(size));
    if (remoteModified.isValid()) {
        text += QCoreApplication::translate("ImagePuller", ", built %1").arg(timeText(remoteModified));
    }
    if (!gitDescribe.isEmpty()) {
        text += QStringLiteral(" (%1)").arg(gitDescribe);
    }
    if (pulledAt.isValid()) {
        text += QCoreApplication::translate("ImagePuller", ", pulled %1").arg(timeText(pulledAt));
    }
    if (!target.isEmpty()) {
        text += QCoreApplication::translate("ImagePuller", " from %1").arg(target);
    }
    return text;
}

// ---------------------------------------------------------------------------------------
// ImagePuller
// ---------------------------------------------------------------------------------------

ImagePuller::ImagePuller(QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this))
{
}

ImagePuller::~ImagePuller()
{
    if (d->running) {
        d->abandon();
    }
}

void ImagePuller::setConnection(SshConnection* connection)
{
    if (d->running) {
        qCWarning(lcFirmware) << "ImagePuller::setConnection ignored while a pull is running";
        return;
    }
    d->connection = connection;
}

SshConnection* ImagePuller::connection() const
{
    return d->connection.data();
}

void ImagePuller::setRemoteFolder(const QString& folder)
{
    d->remoteFolder = folder;
}

QString ImagePuller::remoteFolder() const
{
    return d->remoteFolder;
}

void ImagePuller::setCacheDirectory(const QString& dir)
{
    d->cacheDir = dir;
}

QString ImagePuller::cacheDirectory() const
{
    return d->cacheDir;
}

void ImagePuller::setSdkDirectory(const QString& dir)
{
    d->sdkDir = dir;
}

QString ImagePuller::sdkDirectory() const
{
    return d->sdkDir;
}

void ImagePuller::setGitDescribe(bool on)
{
    d->gitDescribe = on;
}

bool ImagePuller::gitDescribe() const
{
    return d->gitDescribe;
}

void ImagePuller::setForceDownload(bool on)
{
    d->force = on;
}

bool ImagePuller::forceDownload() const
{
    return d->force;
}

void ImagePuller::setExtraFiles(const QStringList& names)
{
    d->extra = names;
}

QStringList ImagePuller::extraFiles() const
{
    return d->extra;
}

bool ImagePuller::isRunning() const
{
    return d->running;
}

QString ImagePuller::localImagePath() const
{
    return d->localImage;
}

QString ImagePuller::imagePath(const QString& cacheDir)
{
    return QDir(cacheDir).filePath(QLatin1String(kImageName));
}

QString ImagePuller::previousImagePath(const QString& cacheDir)
{
    return QDir(cacheDir).filePath(QLatin1String(kPreviousImageName));
}

// ---------------------------------------------------------------------------------------
// Cache notes and history
// ---------------------------------------------------------------------------------------

ImagePuller::CacheEntry ImagePuller::cachedImage(const QString& cacheDir)
{
    return readNote(imagePath(cacheDir));
}

ImagePuller::CacheEntry ImagePuller::previousImage(const QString& cacheDir)
{
    return readNote(previousImagePath(cacheDir));
}

ImagePuller::CacheEntry ImagePuller::readNote(const QString& imagePath)
{
    CacheEntry entry;
    const QFileInfo info(imagePath);
    if (!info.isFile()) {
        return entry;
    }
    entry.path = imagePath;
    entry.size = info.size();

    QFile file(noteFileOf(imagePath));
    if (!file.open(QIODevice::ReadOnly)) {
        return entry;   // an image without a note: the file facts only
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return entry;
    }
    const QJsonObject object = document.object();
    if (object.value(QStringLiteral("size")).toInteger(-1) != entry.size) {
        return entry;   // the image was replaced behind our back: the note describes another file
    }
    entry.remoteModified = QDateTime::fromString(object.value(QStringLiteral("remoteModified")).toString(), Qt::ISODate);
    entry.pulledAt = QDateTime::fromString(object.value(QStringLiteral("pulledAt")).toString(), Qt::ISODate);
    entry.target = object.value(QStringLiteral("target")).toString();
    entry.remoteFolder = object.value(QStringLiteral("remoteFolder")).toString();
    entry.gitDescribe = object.value(QStringLiteral("gitDescribe")).toString();
    return entry;
}

bool ImagePuller::writeNote(const CacheEntry& entry, QString* error)
{
    if (entry.path.isEmpty()) {
        if (error) {
            *error = tr("No image path");
        }
        return false;
    }
    QJsonObject object;
    object.insert(QStringLiteral("format"), 1);
    object.insert(QStringLiteral("target"), entry.target);
    object.insert(QStringLiteral("remoteFolder"), entry.remoteFolder);
    object.insert(QStringLiteral("size"), entry.size);
    object.insert(QStringLiteral("remoteModified"),
                  entry.remoteModified.isValid() ? entry.remoteModified.toUTC().toString(Qt::ISODate) : QString());
    object.insert(QStringLiteral("pulledAt"), entry.pulledAt.isValid() ? entry.pulledAt.toUTC().toString(Qt::ISODate) : QString());
    object.insert(QStringLiteral("gitDescribe"), entry.gitDescribe);

    QSaveFile file(noteFileOf(entry.path));
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

bool ImagePuller::rollBack(const QString& cacheDir, QString* error)
{
    const QString image = imagePath(cacheDir);
    const QString previous = previousImagePath(cacheDir);
    if (!QFileInfo(previous).isFile()) {
        if (error) {
            *error = tr("There is no previous image to go back to");
        }
        return false;
    }
    if (!swapFiles(image, previous, error)) {
        return false;
    }
    QString noteError;
    if (!swapFiles(noteFileOf(image), noteFileOf(previous), &noteError)) {
        // The images are swapped; a stale note is ignored by readNote() (its size no longer matches
        // unless both builds have the same size, which the next pull's mtime check still catches).
        qCWarning(lcFirmware) << "image notes were not swapped:" << noteError;
    }
    qCInfo(lcFirmware) << "rolled back to the previous image in" << cacheDir;
    return true;
}

// ---------------------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------------------

bool ImagePuller::start()
{
    if (d->running) {
        qCWarning(lcFirmware) << "ImagePuller::start: a pull is already running";
        return false;
    }
    d->begin();
    return true;
}

void ImagePuller::cancel()
{
    if (!d->running || d->cancelRequested) {
        return;
    }
    d->cancelRequested = true;
    if (d->step == Private::Step::Download || d->step == Private::Step::ExtraDownload) {
        // The transfer ends with transferFinished(false, ...); finishing then (not now) lets the
        // connection close and remove its ".part" first.
        if (d->connection) {
            d->connection->cancelTransfer();
        }
        return;
    }
    // A stat or a command cannot be recalled; its answer is ignored once pendingId is cleared.
    d->finish(false, tr("cancelled"));
}
