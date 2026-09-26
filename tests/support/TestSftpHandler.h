#pragma once

#include <QByteArray>
#include <QFileInfoList>
#include <QString>
#include <memory>
#include <unordered_map>

class QFile;

/**
 * Minimal SFTP version 3 server used by TestSshServer for the "sftp" subsystem.
 *
 * libssh ships a default SFTP server (sftp_channel_default_subsystem_request /
 * sftp_channel_default_data_callback) but it is compiled out on Windows (sftpserver.c is
 * "#ifndef _WIN32"), and its packet decoder is not exported, so the test server carries this
 * small implementation on every platform instead. It is a pure protocol handler: feed() takes
 * the raw bytes the client wrote to the channel and appends the complete replies to `out`; it
 * never touches libssh, so it can be unit-tested with hand-made packets.
 *
 * Files live on the real file system. Relative paths, "." and "~" resolve against the root
 * directory given at construction; absolute paths are used as given (on Windows a POSIX-style
 * "/name" counts as relative and lands under the root as well).
 *
 * Requests: INIT (answers VERSION 3, no extensions), OPEN, CLOSE, READ, WRITE, LSTAT, FSTAT,
 * STAT, SETSTAT, FSETSTAT (permissions on POSIX, size = truncate; ignored otherwise), OPENDIR,
 * READDIR (".", ".." included, batches of 100), REMOVE, MKDIR, RMDIR, REALPATH (canonical
 * path when it exists, cleaned absolute path otherwise), RENAME (fails when the target
 * exists, as SFTP v3 demands). READLINK, SYMLINK and EXTENDED answer SSH_FX_OP_UNSUPPORTED.
 * Handles are 4-byte big-endian counters; at most 256 are open at once.
 */
class TestSftpHandler
{
public:
    explicit TestSftpHandler(const QString& rootDir);
    ~TestSftpHandler();

    TestSftpHandler(const TestSftpHandler&) = delete;
    TestSftpHandler& operator=(const TestSftpHandler&) = delete;

    /// Consume channel bytes; complete requests are answered into `out` (appended).
    void feed(const QByteArray& data, QByteArray* out);

    QString rootDir() const { return m_root; }
    int openHandleCount() const { return static_cast<int>(m_handles.size()); }
    int handledRequests() const { return m_handledRequests; }
    bool initialised() const { return m_initialised; }

    /// Resolve a client path as described in the class comment.
    QString resolvePath(const QString& path) const;

private:
    struct Handle
    {
        QString path;
        std::unique_ptr<QFile> file;
        bool append = false;
        bool isDir = false;
        QFileInfoList entries;
        int nextEntry = 0;
    };

    void handlePacket(quint8 type, const QByteArray& payload, QByteArray* out);
    void handleOpen(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleClose(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleRead(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleWrite(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleStat(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleFstat(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleSetstat(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleFsetstat(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleOpendir(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleReaddir(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleRemove(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleMkdir(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleRmdir(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleRealpath(quint32 id, const QByteArray& payload, QByteArray* out);
    void handleRename(quint32 id, const QByteArray& payload, QByteArray* out);

    Handle* handleFor(const QByteArray& handle);
    quint32 allocateHandle(std::unique_ptr<Handle> handle);

    QString m_root;
    QByteArray m_input;
    std::unordered_map<quint32, std::unique_ptr<Handle>> m_handles;
    quint32 m_nextHandle = 1;
    int m_handledRequests = 0;
    bool m_initialised = false;
};
