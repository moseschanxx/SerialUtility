#pragma once

#include <QDialog>

#include "core/FileSender.h"

QT_BEGIN_NAMESPACE
namespace Ui { class SendFileDialog; }
QT_END_NAMESPACE

/**
 * Send a file over the session (Session > Send File..., Ctrl+O, or drag & drop).
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
 * options are remembered in QSettings ("sendFile/*"). Modeless (setModal(false)) so the
 * user can watch the terminal while it sends.
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

signals:
    void sendChunk(const QByteArray& data);
    void sendingStarted();
    void sendingFinished(bool completed, const QString& message);

private slots:
    void onBrowse();
    void onStart();
    void onPauseResume();
    void onCancel();
    void onModeChanged();
    void onProgress(qint64 sentBytes, qint64 totalBytes, int sentLines, int totalLines);
    void onFinished(bool completed, const QString& message);

private:
    void loadOptions();
    void saveOptions() const;
    void updateControls();

    Ui::SendFileDialog* ui;
    FileSender* m_sender;
    bool m_connected = false;
};
