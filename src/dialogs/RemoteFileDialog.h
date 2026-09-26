#pragma once

#include <QDialog>

#include "ssh/SshConnection.h"

QT_BEGIN_NAMESPACE
namespace Ui { class RemoteFileDialog; }
QT_END_NAMESPACE

/**
 * Upload / download a file over the session's SFTP subsystem (Session > Upload File to
 * Remote..., Session > Download File from Remote..., or a file dropped on an SSH terminal).
 *
 * Layout in RemoteFileDialog.ui: radioUpload / radioDownload, editLocalPath + buttonBrowseLocal,
 * editRemotePath + buttonRemoteHome ("~" fills the remote home resolved via
 * SshConnection::requestRemoteHome()), checkOverwrite, checkPreservePermissions, progressBar,
 * labelStatus, buttonStart, buttonCancel, buttonClose.
 *
 * Behaviour: upload defaults the remote path to "<remote home>/<local file name>" once the
 * home is known; download defaults the local path to <Downloads>/<remote file name>.
 * Start -> SshConnection::startTransfer(); progress "12.3 MB / 45.6 MB (27%)" at ~10 Hz;
 * Cancel -> cancelTransfer(); the dialog is modeless and disables Start while the connection
 * is not Connected or a transfer is active. Last used paths are remembered in QSettings
 * (group "remoteFile": lastLocalPath, lastRemotePath).
 *
 * Details of this implementation: a derived destination path follows later changes of the
 * source (browse another file and the remote name follows) until the user edits it; a
 * destination the user typed is never replaced. Switching the direction swaps the roles, so
 * the previous source becomes a derived destination: its directory is kept and the file name
 * follows the new source. The paths restored from QSettings count as derived for the same
 * reason (a file dropped in a later session lands next to the previous one, never on top of
 * it). A remote path ending in "/" is treated as a directory and completed with the file name.
 * The paths are saved when a transfer starts and when the dialog is destroyed. The Start
 * button reads "Start upload" / "Start download" (Alt+S; the direction radios own Alt+U and
 * Alt+D). Being modeless, the dialog retranslates itself on QEvent::LanguageChange (captions,
 * tooltips and the idle status line).
 */
class RemoteFileDialog : public QDialog
{
    Q_OBJECT
public:
    explicit RemoteFileDialog(SshConnection* connection, QWidget* parent = nullptr);
    ~RemoteFileDialog() override;

    void setDirection(SshConnection::TransferDirection direction);
    void setLocalPath(const QString& path);
    void setRemotePath(const QString& path);
    SshConnection::TransferRequest request() const;

signals:
    void transferFinished(bool ok, const QString& message);

protected:
    void changeEvent(QEvent* event) override;

private slots:
    void onBrowseLocal();
    void onRemoteHome();
    void onStart();
    void onCancel();
    void onDirectionChanged();
    void onProgress(qint64 done, qint64 total);
    void onFinished(bool ok, const QString& message);
    void onConnectionState(Transport::State state);
    void onTransferStarted(const SshConnection::TransferRequest& request);
    void onRemoteHomeReceived(const QString& path);

private:
    void updateControls();
    void loadPaths();
    void savePaths() const;
    void updateDirectionUi();             ///< captions / tooltips for the current direction
    void applyDefaults();                 ///< derive the destination path from the source (class comment)
    void fillRemoteHome(const QString& home);   ///< "~": remote = <home>/<file name>
    void setIdleStatus();
    bool isUpload() const;
    QString localPath() const;            ///< forward slashes, trimmed
    QString remotePath() const;           ///< trimmed
    void setLocalText(const QString& path, bool derived);
    void setRemoteText(const QString& path, bool derived);

    Ui::RemoteFileDialog* ui;
    SshConnection* m_connection;
    QString m_remoteHome;
    bool m_localDerived = false;          ///< the local path was derived, not typed: may be replaced
    bool m_remoteDerived = false;
    bool m_homeWanted = false;            ///< "~" was pressed before the remote home was known
};
