#pragma once

#include <QDialog>
#include <QFont>
#include <QKeySequence>
#include <QList>
#include <QString>

QT_BEGIN_NAMESPACE
namespace Ui { class PreferencesDialog; }
QT_END_NAMESPACE

class QAbstractButton;

/// One configurable MainWindow action, as listed by MainWindow::shortcutEntries() for the
/// Keyboard page: the QAction's objectName (the AppSettings::shortcut() key), a title such as
/// "File > New Session" (menu and action text without '&' and a trailing "...") and the .ui
/// default sequence (empty = no default shortcut). The same struct describes the fixed
/// shortcuts of PreferencesDialog::setFixedShortcuts() (MainWindow::fixedShortcutEntries()):
/// there defaultSequence is the sequence that is always active, title what a conflict message
/// names ("Window > Next Tab", "File menu") and objectName the action that owns it (its own
/// row may repeat it) or empty for a menu mnemonic.
struct ShortcutEntry
{
    QString objectName;
    QString title;
    QKeySequence defaultSequence;
};

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
 *              cursorBlinkCheck, bellCheck, implicitCrCheck, pauseWhileSelectingCheck
 *              (AppSettings::pauseWhileSelecting(); the View menu action mirrors the same setting),
 *              rightClickPastesCheck (AppSettings::rightClickPastes(); mirrored by View > Right
 *              Click Pastes the same way)
 *  Input:      enterSendsCombo (LineEnding::allModes()), backspaceDeleteCheck,
 *              localEchoCheck, encodingCombo (AnsiParser::availableEncodings(), plus the stored
 *              encoding when it is not in that list)
 *  Connection: defaultBaudCombo (editable; item 0 is "Auto" with item data 0, like the
 *              ConnectionBar, followed by SerialSettings::standardBaudRates(); the validator is
 *              a QIntValidator over SerialSettings::kMinBaudRate..kMaxBaudRate that also
 *              accepts the word "Auto" in any case. "Auto" stores SerialSettings::autoBaud in
 *              defaultSerialSettings() - baudRate keeps the stored starting rate - so a new
 *              session's bar opens on its Auto item; an out-of-range or unparsable entry is
 *              ignored on OK/Apply and the previous value re-selected), defaultDataBitsCombo, defaultParityCombo,
 *              defaultStopBitsCombo, defaultFlowCombo, dtrCheck, rtsCheck,
 *              autoReconnectCheck, reconnectIntervalSpin (200..60000 ms), showSimulatedPortsCheck,
 *              and the automatic baud-rate detection (AppSettings::autoBaud*):
 *              autoBaudCandidatesEdit (comma-separated candidate list; only digits, commas and
 *              spaces can be typed, the tooltip lists the default; an empty or unparsable list
 *              stores the default list, values outside the valid baud range are dropped),
 *              autoBaudSampleSpin (300..10000, suffix " ms"), autoBaudWatchdogCheck
 *              ("Re-detect when the output turns into garbage")
 *  SSH:        sshKnownHostsEdit + sshKnownHostsBrowseButton (AppSettings::sshKnownHostsFile(); empty
 *              = ~/.ssh/known_hosts, shown as the placeholder), sshIdentityEdit +
 *              sshIdentityBrowseButton (sshDefaultIdentityFile()), sshTerminalTypeEdit
 *              (sshDefaultTerminalType(); empty falls back to xterm-256color), sshKeepAliveSpin
 *              (sshDefaultKeepAliveSeconds(), 0..600 s, 0 shown as "Off"), sshSecretsNoteLabel
 *              (read-only: SecretStore::storageDescription())
 *  Keyboard:   shortcutTable (QTableWidget, columns Action | Shortcut | Default, one row per
 *              ShortcutEntry given to setShortcutEntries(), which MainWindow calls before exec();
 *              the Shortcut column shows the sequence being edited, native text, empty = none),
 *              shortcutEdit (QKeySequenceEdit for the selected row, single key stroke; every
 *              recorded chord is taken over at once - an emptied editor is not: Backspace /
 *              Delete, and the editor's own clearing of a selected text before it records a
 *              key, leave the row as it is (the Clear button removes a shortcut) - provided
 *              the chord can be an application
 *              shortcut: it needs Ctrl, Alt or Meta, or an F-key (F1..F35, Shift allowed).
 *              A bare letter, digit, Enter, Space, Backspace, Tab, Esc, arrow or navigation
 *              key, with or without Shift, is text or a control key of the connected terminal
 *              and is refused: the row keeps its value, the editor snaps back to it and
 *              shortcutConflictLabel says why until the next edit or row change),
 *              shortcutClearButton ("Clear": the selected row
 *              gets no shortcut), shortcutRestoreButton ("Restore Default": the selected row
 *              back to its default); the page-level Restore Defaults resets every row.
 *              Conflicts: two rows with the same non-empty sequence are painted red and
 *              shortcutConflictLabel names them; so is a row whose sequence is one of the
 *              fixed shortcuts given to setFixedShortcuts() - the alternates Ctrl+PgDown /
 *              Ctrl+PgUp of Next / Previous Tab (allowed on their own rows) and the menu bar's
 *              Alt+<letter> mnemonics - which MainWindow::fixedShortcutEntries() lists; OK /
 *              Apply are disabled while a conflict exists
 *              and re-enabled once it is resolved. shortcutNoteLabel explains the rule: while a
 *              session is connected, every key that is not an application shortcut is sent to
 *              the device, so bare Ctrl+letter combinations reach the shell unless assigned here.
 *              OK / Apply write, for every row whose value differs from AppSettings::shortcut()
 *              (the stored value or the default), setShortcut(objectName, value) - an emptied
 *              row is stored as an empty sequence, i.e. "no shortcut" - or clearShortcut() when
 *              the value equals the default again; MainWindow re-applies on AppSettings::changed.
 *  Logging:    logDirEdit + logDirBrowseButton, autoLogCheck,
 *              logFormatCombo (raw/text/hex via SessionLogger::formatToString), logIncludeTxCheck
 *  General:    confirmCloseCheck, restoreSessionsCheck
 *  buttonBox with Ok | Cancel | Apply | RestoreDefaults
 * Page order (tab indices): Terminal, Input, Connection, SSH, Keyboard, Logging, General.
 */
class PreferencesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit PreferencesDialog(QWidget* parent = nullptr);
    ~PreferencesDialog() override;

    /// The rows of the Keyboard page (MainWindow::shortcutEntries()); the current value of each
    /// row is read from AppSettings::shortcut(objectName, defaultSequence). Replaces any previous
    /// list and discards unsaved edits of the page.
    void setShortcutEntries(const QList<ShortcutEntry>& entries);
    /// Shortcuts that are always active and cannot be edited (MainWindow::fixedShortcutEntries():
    /// the fixed tab alternates and the menu mnemonics; see ShortcutEntry): a row set to one of
    /// them is a conflict, unless the entry's objectName is that row's action. Replaces any
    /// previous list; the conflicts are re-evaluated at once.
    void setFixedShortcuts(const QList<ShortcutEntry>& fixed);

signals:
    /// Emitted after settings were written (OK or Apply). MainWindow re-applies to sessions.
    void applied();

private slots:
    void loadFromSettings();
    void saveToSettings();
    void onFontButton();
    void onBrowseLogDir();
    void onBrowseKnownHosts();
    void onBrowseIdentityFile();
    void onRestoreDefaults();
    void onButtonClicked(QAbstractButton* button);
    void onShortcutRowChanged();                        ///< table selection -> shortcutEdit
    void onShortcutEdited(const QKeySequence& sequence); ///< shortcutEdit -> the selected row
    void onShortcutClear();                             ///< "Clear"
    void onShortcutRestore();                           ///< "Restore Default" (one row)

private:
    void setupPages();
    void updateFontPreview();
    // ---- Keyboard page ------------------------------------------------------------------
    void loadShortcutRows();                            ///< m_shortcutEntries -> table + m_shortcutValues
    void setShortcutValue(int row, const QKeySequence& value);   ///< value, cell text, conflicts
    void updateShortcutConflicts();                     ///< row colours, the label, OK / Apply
    void restoreShortcutDefaults();                     ///< every row back to its default
    void saveShortcuts();                               ///< the OK / Apply part for the page
    int currentShortcutRow() const;                     ///< selected row, -1 without one
    // ---- Connection page: auto baud ---------------------------------------------------------
    QList<qint32> autoBaudCandidatesFromEdit() const;   ///< parsed, invalid values dropped
    void setAutoBaudCandidatesText(const QList<qint32>& candidates);

    Ui::PreferencesDialog* ui;
    QFont m_font;
    QList<ShortcutEntry> m_shortcutEntries;
    QList<QKeySequence> m_shortcutValues;   ///< one per entry: the value being edited
    QList<ShortcutEntry> m_fixedShortcuts;  ///< setFixedShortcuts(): always active, checked for conflicts
    QString m_shortcutNotice;               ///< why the last edit was refused; shown while no conflict is
    bool m_shortcutConflict = false;
};
