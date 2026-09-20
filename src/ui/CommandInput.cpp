#include "ui/CommandInput.h"

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolTip>

#include "app/Logging.h"
#include "core/CommandHistory.h"
#include "core/HexUtils.h"

namespace {

/// Style applied to the line edit while it holds an invalid payload.
const char kErrorStyle[] = "QLineEdit { border: 1px solid #E74C3C; border-radius: 2px; }";

} // namespace

CommandInput::CommandInput(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
    retranslate();
    setEnabledForConnection(false);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void CommandInput::setupUi()
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    auto* prompt = new QLabel(QStringLiteral(">"), this);
    prompt->setObjectName(QStringLiteral("commandPrompt"));

    m_edit = new QLineEdit(this);
    m_edit->setObjectName(QStringLiteral("commandEdit"));
    m_edit->setClearButtonEnabled(true);
    m_edit->installEventFilter(this);

    m_lineEndingCombo = new QComboBox(this);
    m_lineEndingCombo->setObjectName(QStringLiteral("lineEndingCombo"));
    for (const LineEnding::Mode mode : LineEnding::allModes()) {
        m_lineEndingCombo->addItem(LineEnding::displayName(mode), static_cast<int>(mode));
    }
    m_lineEndingCombo->setCurrentIndex(m_lineEndingCombo->findData(static_cast<int>(LineEnding::Mode::CR)));
    m_lineEndingCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_lineEndingCombo->setFocusPolicy(Qt::NoFocus);

    m_hexCheck = new QCheckBox(this);
    m_hexCheck->setObjectName(QStringLiteral("hexCheck"));
    m_hexCheck->setFocusPolicy(Qt::NoFocus);
    m_escapeCheck = new QCheckBox(this);
    m_escapeCheck->setObjectName(QStringLiteral("escapeCheck"));
    m_escapeCheck->setFocusPolicy(Qt::NoFocus);

    m_sendButton = new QPushButton(this);
    m_sendButton->setObjectName(QStringLiteral("sendButton"));
    m_sendButton->setAutoDefault(false);
    m_sendButton->setDefault(false);
    m_sendButton->setFocusPolicy(Qt::NoFocus);

    layout->addWidget(prompt);
    layout->addWidget(m_edit, 1);
    layout->addWidget(m_lineEndingCombo);
    layout->addWidget(m_hexCheck);
    layout->addWidget(m_escapeCheck);
    layout->addWidget(m_sendButton);

    // ---- Signals --------------------------------------------------------------------
    connect(m_edit, &QLineEdit::returnPressed, this, &CommandInput::send);
    connect(m_sendButton, &QPushButton::clicked, this, &CommandInput::send);
    connect(m_edit, &QLineEdit::textChanged, this, [this](const QString&) {
        if (!m_edit->styleSheet().isEmpty()) {
            clearError();
        }
    });
    connect(m_lineEndingCombo, &QComboBox::currentIndexChanged, this,
            [this](int) { emit lineEndingChanged(lineEnding()); });
    connect(m_hexCheck, &QCheckBox::toggled, this, [this](bool on) {
        // Hex payloads carry no line ending and no escapes.
        m_lineEndingCombo->setEnabled(!on);
        m_escapeCheck->setEnabled(!on);
        clearError();
        retranslate();
    });
    connect(m_escapeCheck, &QCheckBox::toggled, this, [this](bool) {
        clearError(); // a stale "invalid escape" mark no longer applies once the mode flips
        retranslate();
    });
}

void CommandInput::retranslate()
{
    // Hex first: it takes precedence in send() even when the (disabled) escape box is still checked.
    if (hexMode()) {
        m_edit->setPlaceholderText(tr("Hex bytes, e.g. AA 55 0D"));
    } else if (escapeMode()) {
        m_edit->setPlaceholderText(
            tr("Command with C escapes, e.g. \\x1b[A or AT\\tOK (\\\\ for a literal backslash)"));
    } else {
        m_edit->setPlaceholderText(tr("Type a command and press Enter (Up/Down for history)"));
    }
    m_edit->setToolTip(tr("Up/Down: history, Esc or Ctrl+L: clear"));
    m_lineEndingCombo->setToolTip(tr("Line ending appended to the command"));
    m_hexCheck->setText(tr("HEX"));
    m_hexCheck->setToolTip(tr("Interpret the text as hex bytes (e.g. \"AA 55 0D\"); no line ending is added"));
    m_escapeCheck->setText(tr("\\ Esc"));
    m_escapeCheck->setToolTip(tr("Interpret C-style escapes: \\n \\r \\t \\e \\xHH \\uHHHH"));
    m_sendButton->setText(tr("Send"));
    m_sendButton->setToolTip(tr("Send the command (Enter)"));
}

void CommandInput::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

// ---------------------------------------------------------------------------
// Properties
// ---------------------------------------------------------------------------

void CommandInput::setHistory(CommandHistory* history)
{
    m_history = history;
    m_draft.clear();
}

CommandHistory* CommandInput::history() const
{
    return m_history;
}

LineEnding::Mode CommandInput::lineEnding() const
{
    const int index = m_lineEndingCombo->currentIndex();
    if (index < 0) {
        return LineEnding::Mode::CR;
    }
    return static_cast<LineEnding::Mode>(m_lineEndingCombo->itemData(index).toInt());
}

void CommandInput::setLineEnding(LineEnding::Mode mode)
{
    const int index = m_lineEndingCombo->findData(static_cast<int>(mode));
    if (index >= 0) {
        m_lineEndingCombo->setCurrentIndex(index); // emits lineEndingChanged() when it differs
    }
}

bool CommandInput::hexMode() const
{
    return m_hexCheck->isChecked();
}

void CommandInput::setHexMode(bool on)
{
    m_hexCheck->setChecked(on);
}

bool CommandInput::escapeMode() const
{
    return m_escapeCheck->isChecked();
}

void CommandInput::setEscapeMode(bool on)
{
    m_escapeCheck->setChecked(on);
}

QString CommandInput::text() const
{
    return m_edit->text();
}

void CommandInput::setText(const QString& text)
{
    m_edit->setText(text);
}

void CommandInput::setEnabledForConnection(bool connected)
{
    m_sendButton->setEnabled(connected);
    if (connected && !m_edit->styleSheet().isEmpty()) {
        clearError();
    }
}

void CommandInput::focusInput()
{
    m_edit->setFocus(Qt::OtherFocusReason);
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

void CommandInput::send()
{
    const QString text = m_edit->text();

    if (!m_sendButton->isEnabled()) {
        showError(tr("Not connected"));
        return;
    }

    QByteArray payload;
    QString error;

    if (hexMode()) {
        if (!HexUtils::parseHexString(text, payload, &error)) {
            showError(error.isEmpty() ? tr("Invalid hex string") : error);
            return;
        }
        if (payload.isEmpty()) {
            return; // nothing to send
        }
    } else if (escapeMode()) {
        payload = HexUtils::unescape(text, &error);
        if (!error.isEmpty()) {
            showError(error);
            return;
        }
        payload += LineEnding::bytes(lineEnding());
    } else {
        payload = text.toUtf8() + LineEnding::bytes(lineEnding());
    }

    if (m_history) {
        m_history->add(text); // ignores empty text, resets navigation
    }
    m_draft.clear();
    clearError();
    m_edit->clear();

    qCDebug(lcUi) << "command input send" << payload.size() << "byte(s)";
    emit sendRequested(payload, text);
}

// ---------------------------------------------------------------------------
// Keyboard handling (history navigation, clearing)
// ---------------------------------------------------------------------------

bool CommandInput::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_edit && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        const Qt::KeyboardModifiers mods = keyEvent->modifiers() & ~Qt::KeypadModifier;

        switch (keyEvent->key()) {
        case Qt::Key_Up:
            if (mods == Qt::NoModifier && m_history && m_history->size() > 0) {
                if (!m_history->isNavigating()) {
                    m_draft = m_edit->text();
                }
                m_edit->setText(m_history->previous());
                m_edit->end(false);
                return true;
            }
            break;

        case Qt::Key_Down:
            if (mods == Qt::NoModifier && m_history && m_history->isNavigating()) {
                const QString entry = m_history->next();
                m_edit->setText(m_history->isNavigating() ? entry : m_draft);
                m_edit->end(false);
                if (!m_history->isNavigating()) {
                    m_draft.clear();
                }
                return true;
            }
            break;

        case Qt::Key_Escape:
            if (mods == Qt::NoModifier) {
                m_edit->clear();
                m_draft.clear();
                clearError();
                if (m_history) {
                    m_history->resetNavigation();
                }
                return true;
            }
            break;

        case Qt::Key_L:
            if (mods == Qt::ControlModifier) {
                m_edit->clear();
                m_draft.clear();
                clearError();
                if (m_history) {
                    m_history->resetNavigation();
                }
                return true;
            }
            break;

        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

// ---------------------------------------------------------------------------
// Validation feedback
// ---------------------------------------------------------------------------

void CommandInput::showError(const QString& message)
{
    m_edit->setStyleSheet(QString::fromLatin1(kErrorStyle));
    m_edit->setToolTip(message);
    const QPoint below = m_edit->mapToGlobal(QPoint(0, m_edit->height()));
    QToolTip::showText(below, message, m_edit);
    qCDebug(lcUi) << "command input error:" << message;
}

void CommandInput::clearError()
{
    if (m_edit->styleSheet().isEmpty()) {
        return;
    }
    m_edit->setStyleSheet(QString());
    m_edit->setToolTip(tr("Up/Down: history, Esc or Ctrl+L: clear"));
    QToolTip::hideText();
}
