#pragma once

#include <QWidget>
#include <QList>

#include "core/SerialConnection.h"
#include "core/SerialPortEnumerator.h"

class QComboBox;
class QToolButton;
class QPushButton;
class QLabel;

/**
 * The strip above the terminal with the port and line parameters:
 *
 *  [Port: COM8 - CH343 v][⟳] [Baud: 115200 v] [8 v][N v][1 v][Flow: None v] [DTR][RTS] [Break]   [● Connect]
 *
 * - Port combo lists SerialPortEntry::displayText() with toolTip(); item data = portName.
 *   setPorts() keeps the current selection if the port still exists; if the selected port
 *   disappeared it stays as a greyed placeholder item so the user sees what was selected.
 *   Refresh button -> refreshRequested().
 * - Baud combo is editable with SerialSettings::standardBaudRates(); its validator is a
 *   QIntValidator (SerialSettings::kMinBaudRate..kMaxBaudRate, i.e. 50..10000000) that also
 *   accepts the word "Auto" (typed in any case, or its translation). The first item is "Auto"
 *   (item data 0): while it is selected settings() returns autoBaud = true and baudRate = the
 *   effective rate - the last rate detected by SessionWidget's BaudRateDetector, initially the
 *   first AppSettings::autoBaudCandidates() entry (115200) - which setEffectiveBaudRate()
 *   updates (the item's and the combo's tooltip read "Auto (1500000)"; the selection and the
 *   edit text stay "Auto", nothing is emitted). setSettings() with autoBaud selects "Auto" and
 *   remembers settings.baudRate as the effective rate; a fixed rate selects the matching item
 *   (or lives in the edit text) as before.
 * - Data bits 5/6/7/8, parity N/E/O/S/M (tooltips spell them out), stop bits 1/1.5/2,
 *   flow None / RTS-CTS / XON-XOFF.
 * - DTR / RTS are checkable tool buttons (state mirrored from the connection via setPinStates).
 * - Connect button: green dot + "Connect" when disconnected, red dot + "Disconnect" when
 *   connected, amber dot + "Reconnecting..." while reconnecting (still clickable = cancel);
 *   Transport::State::Connecting (never produced by a serial port) is drawn the same way in amber.
 * - While connected the port combo and refresh are disabled; parameter combos remain
 *   enabled and changes emit settingsChanged() so SerialConnection can apply them live.
 * - Compact: uses a QHBoxLayout with small margins; labels hidden when the bar is narrow
 *   is NOT required (fixed labels are fine).
 */
class ConnectionBar : public QWidget
{
    Q_OBJECT
public:
    explicit ConnectionBar(QWidget* parent = nullptr);

    SerialSettings settings() const;               ///< current combo values (+ selected port); see "Auto" above
    void setSettings(const SerialSettings& settings);   ///< no signals emitted
    /// The rate the "Auto" item stands for (detected by the session): shown in the tooltips and
    /// returned as settings().baudRate while "Auto" is selected. Never changes the selection or
    /// emits anything; ignored unless SerialSettings::isValidBaudRate(baud).
    void setEffectiveBaudRate(qint32 baud);
    qint32 effectiveBaudRate() const;
    QString selectedPortName() const;
    void selectPort(const QString& portName);      ///< adds a placeholder if not listed
    void setPorts(const QList<SerialPortEntry>& ports);
    void setConnectionState(SerialConnection::State state);
    void setPinStates(bool dtr, bool rts);
    void setFocusToPort();

signals:
    void connectRequested();
    void disconnectRequested();
    void refreshRequested();
    void settingsChanged(const SerialSettings& settings);   ///< any parameter or port changed by the user
    void dtrToggled(bool on);
    void rtsToggled(bool on);
    void sendBreakRequested();

protected:
    void changeEvent(QEvent* event) override;   ///< LanguageChange -> retranslate()

private:
    void setupUi();
    void retranslate();
    void emitSettingsChanged();
    bool isAutoText(const QString& text) const;   ///< "Auto" (any case) or its translation
    void updateBaudToolTips();                     ///< "Auto (<effective>)" on the item and, while selected, the combo

    QComboBox* m_portCombo = nullptr;
    QToolButton* m_refreshButton = nullptr;
    QComboBox* m_baudCombo = nullptr;
    QComboBox* m_dataBitsCombo = nullptr;
    QComboBox* m_parityCombo = nullptr;
    QComboBox* m_stopBitsCombo = nullptr;
    QComboBox* m_flowCombo = nullptr;
    QToolButton* m_dtrButton = nullptr;
    QToolButton* m_rtsButton = nullptr;
    QToolButton* m_breakButton = nullptr;
    QPushButton* m_connectButton = nullptr;
    QLabel* m_portLabel = nullptr;
    QLabel* m_baudLabel = nullptr;
    QLabel* m_flowLabel = nullptr;
    SerialConnection::State m_state = SerialConnection::State::Disconnected;
    qint32 m_effectiveBaud = 115200;   ///< the rate behind "Auto"; see setEffectiveBaudRate()
    bool m_updating = false;   ///< suppress settingsChanged() during programmatic updates
    SerialSettings m_lastEmitted;  ///< value of the last settingsChanged(); duplicates (e.g. editingFinished after
    bool m_hasEmitted = false;     ///< currentIndexChanged) are not re-emitted; reset by programmatic updates
};
