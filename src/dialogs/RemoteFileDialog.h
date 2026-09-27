#pragma once

#include <QDialog>
#include <QPalette>

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
 * Alt+D, the fields Alt+L / Alt+T ("Save to") and Alt+R, Overwrite Alt+O, Preserve Alt+P, Cancel
 * Alt+C, Close Alt+E - no letter is shared, so every shortcut works from any field; Alt+A is
 * avoided because it is WeChat's global screenshot hotkey). Being modeless,
 * the dialog retranslates itself on QEvent::LanguageChange (captions, tooltips and the idle
 * status line).
 *
 * Method and errors (v0.4): once the connection reports transferStarted() the status line names
 * the file and the transfer method from SshConnection::transferStatus().method - "Uploading
 * fw.bin via SFTP..." / "Uploading fw.bin via shell (cat)..." - and every progress line keeps
 * that prefix ("Uploading fw.bin via SFTP: 1.2 MB / 3.0 MB (40%)"); a method the connection has
 * not published yet is read again on the next progress signal. transferFinished(ok, message)
 * shows the connection's message as it is ("Uploaded x to y (1.2 MB, SFTP)"); a failure - a
 * missing remote file, a cancelled transfer, or a server on which neither SFTP nor the shell
 * fallback works - is shown in red, and Start and the inputs are enabled again at once so the
 * path can be corrected and retried without reopening the dialog. A red line outlives a
 * state change to Disconnected / Reconnecting (a transfer aborted by a dropped link or by
 * Session > Disconnect keeps its reason next to the disabled Start instead of a bare "Not
 * connected."); Connected, a direction change or the next transfer replace it.
 *
 * Remote path rules (request()): backslashes become slashes; a leading "~" / "~/" is replaced by
 * the remote home when it is known and simply dropped otherwise (a relative path resolves
 * against the login directory on SFTP and in the shell fallback alike); for an upload a path
 * ending in "/" - and a bare "~" - gets the local file name appended even when the field was
 * never left (Alt+S straight from the field), and Start completes it in the field too. Start
 * refuses a remote path typed as a Windows path ("C:\...", drive letter and backslash: the two
 * fields swapped) with a readable status line. When the remote home cannot be resolved (a server
 * without SFTP, such as dropbear) an upload with an empty remote path proposes the bare file
 * name so Start is usable, and "~" says so in the status line. The home remembered by the
 * dialog is dropped on Disconnected (the next connect may reach another host) and re-read from
 * the connection on Connected; the dialog asks the connection for it at most once per connect
 * (SessionWidget asks at connect time anyway), except for the "~" button, which always asks.
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
    /// Renders the status line of a running transfer from `status` - the connection's
    /// transferStatus() (onTransferStarted), or one supplied through QMetaObject::invokeMethod
    /// by a test that has no worker to publish a method: "Uploading fw.bin via SFTP...".
    void applyTransferStatus(const SshConnection::TransferStatus& status);

private:
    void updateControls();
    void loadPaths();
    void savePaths() const;
    void updateDirectionUi();             ///< captions / tooltips for the current direction
    void applyDefaults();                 ///< derive the destination path from the source (class comment)
    void fillRemoteHome(const QString& home);   ///< "~": remote = <home>/<file name>
    void setIdleStatus();
    void setStatus(const QString& text, bool error = false);   ///< the status line, red for an error
    QString runningText(const QString& progress = QString()) const;   ///< "Uploading x via SFTP..." / ": 1 MB / 2 MB (50%)"
    bool isUpload() const;
    QString localPath() const;            ///< forward slashes, trimmed
    QString remoteText() const;           ///< the field as typed: trimmed, backslashes -> slashes ("~/x" kept)
    QString remotePath() const;           ///< remoteText() with "~" expanded and a directory completed (class comment)
    QString knownHome() const;            ///< the remote home known to the dialog or the connection, else empty
    void setLocalText(const QString& path, bool derived);
    void setRemoteText(const QString& path, bool derived);

    Ui::RemoteFileDialog* ui;
    SshConnection* m_connection;
    QString m_remoteHome;
    QPalette m_statusPalette;             ///< labelStatus as designed; the error colour is applied on top
    bool m_statusIsError = false;         ///< the status line is a red failure: kept across Disconnected
    QString m_method;                     ///< "sftp" / "shell" of the running transfer, empty while unknown
    QString m_activeName;                 ///< file name of the running transfer (empty when idle)
    bool m_activeUpload = true;
    bool m_localDerived = false;          ///< the local path was derived, not typed: may be replaced
    bool m_remoteDerived = false;
    bool m_homeWanted = false;            ///< "~" was pressed before the remote home was known
    bool m_homeAsked = false;             ///< requestRemoteHome() was sent for this connect (applyDefaults)
};
