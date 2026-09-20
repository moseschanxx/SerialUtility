#pragma once

#include <QWidget>

#include "core/LineEnding.h"

class QLineEdit;
class QComboBox;
class QCheckBox;
class QPushButton;
class CommandHistory;

/**
 * Line-mode input strip under the terminal (for users who prefer composing a whole
 * command, and for hex / escaped payloads):
 *
 *  [> command text .............................] [CR v] [x] HEX [x] \ Esc  [Send]
 *
 * - Enter or Send -> emits sendRequested(payload, displayText) where payload is:
 *     HEX on   : HexUtils::parseHexString(text) (no line ending appended)
 *     Esc on   : HexUtils::unescape(text) + LineEnding::bytes(lineEnding())
 *     otherwise: text.toUtf8() (encoding is applied by SessionWidget if not UTF-8)
 *                + LineEnding::bytes(lineEnding())
 *   Invalid hex / escape -> the field turns red, a tooltip shows the error, nothing is sent.
 *   The text is added to the CommandHistory (when set) and the field is cleared.
 * - Up/Down arrows navigate the history (CommandHistory::previous/next); Esc clears.
 * - Ctrl+L clears the field; the placeholder text explains the mode.
 * - setEnabledForConnection(false) disables Send (typing stays possible).
 * - A QCompleter over the history entries is optional.
 */
class CommandInput : public QWidget
{
    Q_OBJECT
public:
    explicit CommandInput(QWidget* parent = nullptr);

    void setHistory(CommandHistory* history);   ///< not owned; may be nullptr
    CommandHistory* history() const;

    LineEnding::Mode lineEnding() const;
    void setLineEnding(LineEnding::Mode mode);
    bool hexMode() const;
    void setHexMode(bool on);
    bool escapeMode() const;
    void setEscapeMode(bool on);
    QString text() const;
    void setText(const QString& text);

    void setEnabledForConnection(bool connected);
    void focusInput();

public slots:
    void send();

signals:
    void sendRequested(const QByteArray& payload, const QString& displayText);
    void lineEndingChanged(LineEnding::Mode mode);

protected:
    void changeEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;   ///< Up/Down on the line edit

private:
    void setupUi();
    void retranslate();
    void showError(const QString& message);
    void clearError();

    QLineEdit* m_edit = nullptr;
    QComboBox* m_lineEndingCombo = nullptr;
    QCheckBox* m_hexCheck = nullptr;
    QCheckBox* m_escapeCheck = nullptr;
    QPushButton* m_sendButton = nullptr;
    CommandHistory* m_history = nullptr;
    QString m_draft;   ///< text typed before navigating into the history
};
