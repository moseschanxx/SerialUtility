#pragma once

#include <QDialog>

#include "core/FileSender.h"

QT_BEGIN_NAMESPACE
namespace Ui { class SendFileDialog; }
QT_END_NAMESPACE

/**
 * Send a file over the session (Session > Send File..., Ctrl+Shift+O, or drag & drop).
 *
 * Controls (SendFileDialog.ui): filePathEdit + browseButton, modeTextRadio / modeBinaryRadio,
 * lineDelaySpin (ms), lineEndingCombo, stripEndingsCheck, skipEmptyCheck, chunkSizeSpin,
 * chunkDelaySpin, progressBar, statusLabel, startButton, pauseButton, cancelButton, closeButton.
 * Text-mode controls are enabled only in text mode and vice versa.
 *
 * The dialog owns a FileSender; its chunkReady() is re-emitted as sendChunk() which
 * SessionWidget connects to sendBytes(). While sending, the Start button becomes disabled,
 * Pause toggles Pause/Resume, Cancel aborts. Progress shows "12.3 KB / 45.6 KB (27%) -
 * line 120/430". On finish the status label shows the FileSender message. The last used
 * options are remembered in QSettings (group "sendFile"). Modeless (setModal(false)) so the
 * user can watch the terminal while it sends.
 *
 * Backpressure: SessionWidget feeds SerialConnection::pendingTxBytes() into updatePendingTx()
 * after every chunk and on every txBytesWritten(). When more than two chunks (binary: chunkSize,
 * text: 512 bytes) are still queued in QSerialPort the sender is held ("Waiting for the port to
 * drain...") and released once the queue is down to one chunk; the hold never flips the
 * Pause/Resume button and a user Pause always wins over it. After the last chunk the progress
 * bar stays at 99% ("... still leaving the port") until updatePendingTx(0) arrives. Simulated
 * devices always report 0 pending bytes, so they are unaffected.
 *
 * Retranslates itself on QEvent::LanguageChange (the dialog outlives language switches).
 */
class SendFileDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SendFileDialog(QWidget* parent = nullptr);
    ~SendFileDialog() override;

    void setFilePath(const QString& path);
    QString filePath() const;
    FileSender::Options options() const;
    bool isSending() const;

    /// Enable/disable Start according to the session's connection state.
    void setConnected(bool connected);

public slots:
    /// Bytes still queued in the port's write buffer (SerialConnection::pendingTxBytes()); see
    /// the class comment for the hold/release thresholds. Public slot added at integration.
    void updatePendingTx(qint64 pendingBytes);

signals:
    void sendChunk(const QByteArray& data);
    void sendingStarted();
    void sendingFinished(bool completed, const QString& message);

protected:
    void changeEvent(QEvent* event) override;   ///< LanguageChange -> retranslate()

private slots:
    void onBrowse();
    void onStart();
    void onPauseResume();
    void onCancel();
    void onModeChanged();
    void onProgress(qint64 sentBytes, qint64 totalBytes, int sentLines, int totalLines);
    void onFinished(bool completed, const QString& message);

private:
    void retranslate();
    void loadOptions();
    void saveOptions() const;
    void updateControls();

    Ui::SendFileDialog* ui;
    FileSender* m_sender;
    bool m_connected = false;
    bool m_drainHold = false;        ///< paused by updatePendingTx() until the port drains
    bool m_userPaused = false;       ///< paused by the user (or by a disconnect); wins over the drain hold
    bool m_awaitingDrain = false;    ///< completed, but bytes were still queued: 99% until updatePendingTx(0)
    qint64 m_lastPending = 0;        ///< last value passed to updatePendingTx()
    QString m_finishedMessage;       ///< FileSender's completion message, shown once the port drained
};
