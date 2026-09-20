#pragma once

#include <QDialog>
#include <QFont>

QT_BEGIN_NAMESPACE
namespace Ui { class PreferencesDialog; }
QT_END_NAMESPACE

class QAbstractButton;

/**
 * Preferences (Edit > Preferences..., Ctrl+,). A QTabWidget with pages; every control
 * maps to one AppSettings property. OK / Apply write to AppSettings and emit applied();
 * Cancel discards. "Restore Defaults" resets the current page's controls (not the settings
 * until applied).
 *
 * Pages / controls (objectNames in PreferencesDialog.ui):
 *  Terminal:   fontButton (opens QFontDialog, monospace-only filter) + fontPreviewLabel,
 *              themeCombo (TerminalTheme::names() with displayName; a stored name not in the
 *              list is appended so it round-trips), scrollbackSpin (100..1000000),
 *              cursorBlinkCheck, bellCheck, implicitCrCheck
 *  Input:      enterSendsCombo (LineEnding::allModes()), backspaceDeleteCheck,
 *              localEchoCheck, encodingCombo (AnsiParser::availableEncodings(), plus the stored
 *              encoding when it is not in that list)
 *  Connection: defaultBaudCombo (editable, QIntValidator SerialSettings::kMinBaudRate..kMaxBaudRate;
 *              an out-of-range or unparsable entry is ignored on OK/Apply and the previous
 *              value re-selected), defaultDataBitsCombo, defaultParityCombo,
 *              defaultStopBitsCombo, defaultFlowCombo, dtrCheck, rtsCheck,
 *              autoReconnectCheck, reconnectIntervalSpin (200..60000 ms)
 *  Logging:    logDirEdit + logDirBrowseButton, autoLogCheck,
 *              logFormatCombo (raw/text/hex via SessionLogger::formatToString), logIncludeTxCheck
 *  General:    confirmCloseCheck, restoreSessionsCheck
 *  buttonBox with Ok | Cancel | Apply | RestoreDefaults
 */
class PreferencesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget* parent = nullptr);
    ~PreferencesDialog() override;

signals:
    /// Emitted after settings were written (OK or Apply). MainWindow re-applies to sessions.
    void applied();

private slots:
    void loadFromSettings();
    void saveToSettings();
    void onFontButton();
    void onBrowseLogDir();
    void onRestoreDefaults();
    void onButtonClicked(QAbstractButton* button);

private:
    void setupPages();
    void updateFontPreview();

    Ui::PreferencesDialog* ui;
    QFont m_font;
};
