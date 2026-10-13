#pragma once

#include <QObject>
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <memory>

class SshConnection;

/**
 * Pulls the Rockchip `update.img` (and optional extra files) from the Linux build machine into a
 * local cache over an SshConnection that the owner already connected (the Firmware tab reuses
 * the SSH stack: profiles, known_hosts, remembered passwords, SFTP with the shell fallback).
 * Covers what the old Python puller (scp_image_puller.py) was used for: one folder, a few files,
 * skip when unchanged, progress, cancel.
 *
 * start() needs a Connected connection, a remote folder and a cache folder; it runs:
 *  1. SshConnection::statRemote(remoteFolder + "/update.img") -> remoteInfo(name, size, mtime).
 *     A missing file -> finished(false, "no update.img in <folder> on <target>").
 *  2. When gitDescribe() is on: SshConnection::runCommand("git -C '<sdk>' describe --tags
 *     --dirty 2>/dev/null") with sdkDirectory() (default: the remote folder's grandparent, so
 *     "<sdk>/output/image" -> "<sdk>"); its trimmed stdout is reported in remoteInfo() /
 *     gitDescribe() and stored in the cache note; a failure leaves it empty (never an error).
 *  3. Skip rule: the cache note (`<cache>/update.img.json`: remote target, folder, size, mtime,
 *     pulled-at, git describe) matches size and mtime and `<cache>/update.img` exists with that
 *     size -> skipped("update.img unchanged (<size>, <mtime>)") and finished(true, ...) without a
 *     transfer. forceDownload() overrides.
 *  4. Download through SshConnection::startTransfer() to `<cache>/update.img.new` (the transfer
 *     itself writes "<path>.part" and renames); progress(done, total) forwarded.
 *  5. Validation: RockchipTool::isRkfwImage() on the new file; a wrong magic removes it and ends
 *     with finished(false, "downloaded file is not a Rockchip RKFW image").
 *  6. Rotation (one-level history): an existing `<cache>/update.img` becomes
 *     `<cache>/update.prev.img` (replacing an older one, its note `update.prev.img.json` likewise),
 *     then `update.img.new` -> `update.img`, the note written; finished(true, "pulled update.img
 *     (<size>) from <target>:<folder>"), localImagePath() set.
 *  7. Extra files (extraFiles(), e.g. "*.txt" patterns are NOT supported - plain names such as
 *     "sd_update.txt"): each is downloaded next to the image (no history, overwrite); a missing
 *     extra is logged and skipped, never an error.
 * cancel() cancels the running transfer (SshConnection::cancelTransfer()) and removes the `.new`
 * file; finished(false, "cancelled"). The connection dropping -> finished(false, its message).
 * Only one pull at a time; start() returns false while one runs.
 *
 * rollBack(cacheDir) (static, no connection): swaps update.img <-> update.prev.img and their
 * notes; false when there is no previous image. cachedImage(cacheDir) / previousImage(cacheDir)
 * read the notes for the tab's image lines (CacheEntry::isValid() when the file exists).
 */
class ImagePuller : public QObject
{
    Q_OBJECT
public:
    struct CacheEntry
    {
        QString path;             ///< the image file
        qint64 size = -1;
        QDateTime remoteModified; ///< the remote mtime at pull time
        QDateTime pulledAt;
        QString target;           ///< "user@host:port"
        QString remoteFolder;
        QString gitDescribe;
        bool isValid() const { return !path.isEmpty() && size >= 0; }
        QString summary() const;  ///< "update.img 11.4 MB, built 2026-10-13 21:45 (v1.3.0-4-ge0e0d41), pulled 2026-10-13 10:40 from moses@10.0.10.71"
    };

    static constexpr const char* kImageName = "update.img";
    static constexpr const char* kPreviousImageName = "update.prev.img";
    static constexpr const char* kNoteSuffix = ".json";

    explicit ImagePuller(QObject* parent = nullptr);
    ~ImagePuller() override;

    void setConnection(SshConnection* connection);   ///< not owned; Connected before start()
    SshConnection* connection() const;
    void setRemoteFolder(const QString& folder);     ///< "~/rk/rockchip-platforms/output/image" ("~" is the remote home)
    QString remoteFolder() const;
    void setCacheDirectory(const QString& dir);      ///< created on demand
    QString cacheDirectory() const;
    void setSdkDirectory(const QString& dir);        ///< empty = derived from the remote folder
    QString sdkDirectory() const;
    void setGitDescribe(bool on);                    ///< default true
    bool gitDescribe() const;
    void setForceDownload(bool on);                  ///< default false: the skip rule applies
    bool forceDownload() const;
    void setExtraFiles(const QStringList& names);    ///< plain file names in the remote folder
    QStringList extraFiles() const;

    bool isRunning() const;
    QString localImagePath() const;                  ///< "<cache>/update.img" once a pull succeeded or was skipped

    static QString imagePath(const QString& cacheDir);
    static QString previousImagePath(const QString& cacheDir);
    static CacheEntry cachedImage(const QString& cacheDir);
    static CacheEntry previousImage(const QString& cacheDir);
    static bool rollBack(const QString& cacheDir, QString* error = nullptr);
    static bool writeNote(const CacheEntry& entry, QString* error = nullptr);   ///< entry.path + ".json"
    static CacheEntry readNote(const QString& imagePath);

public slots:
    bool start();
    void cancel();

signals:
    void remoteInfo(const QString& name, qint64 size, const QDateTime& modified, const QString& gitDescribe);
    void progress(qint64 done, qint64 total);
    void skipped(const QString& reason);
    void logLine(const QString& line);
    void finished(bool ok, const QString& message);

private:
    struct Private;
    std::unique_ptr<Private> d;
};
