#include "ui/QuickCommandBar.h"

#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QMouseEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QToolButton>
#include <QWheelEvent>

#include <functional>
#include <utility>

#include "app/Logging.h"
#include "core/HexUtils.h"
#include "core/LineEnding.h"

namespace {

const char kGroupSettingsKey[] = "ui/quickCommandGroup";
const char kGeneralGroup[] = "General";
const char kMiddleFilterName[] = "quickCommandMiddleClickFilter";
const char kBaseToolTipProperty[] = "baseToolTip";

/**
 * Event filter installed on every command button: a middle-button release over the
 * button is reported through the callback (QAbstractButton itself only reacts to the
 * left button, so the event would otherwise be ignored).
 */
class MiddleClickFilter : public QObject
{
public:
    explicit MiddleClickFilter(std::function<void()> onMiddleClick, QObject* parent = nullptr)
        : QObject(parent)
        , m_onMiddleClick(std::move(onMiddleClick))
    {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto* mouseEvent = static_cast<QMouseEvent*>(event);
            if (mouseEvent->button() == Qt::MiddleButton) {
                auto* widget = qobject_cast<QWidget*>(watched);
                if (widget && widget->isEnabled() && widget->rect().contains(mouseEvent->position().toPoint())) {
                    if (m_onMiddleClick) {
                        m_onMiddleClick();
                    }
                    return true;
                }
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    std::function<void()> m_onMiddleClick;
};

/**
 * Event filter on the scroll area's viewport: a vertical mouse wheel scrolls the button
 * row horizontally, since there is no vertical scroll bar to receive it.
 */
class WheelToHorizontalFilter : public QObject
{
public:
    explicit WheelToHorizontalFilter(QScrollBar* bar, QObject* parent = nullptr)
        : QObject(parent)
        , m_bar(bar)
    {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::Wheel && m_bar) {
            auto* wheel = static_cast<QWheelEvent*>(event);
            int delta = wheel->angleDelta().x();
            if (delta == 0) {
                delta = wheel->angleDelta().y();
            }
            if (delta != 0 && m_bar->maximum() > 0) {
                // One wheel notch (120 units) moves three single steps, like QAbstractSlider.
                const int steps = (delta / 120) * 3 * m_bar->singleStep();
                m_bar->setValue(m_bar->value() - (steps != 0 ? steps : (delta > 0 ? 1 : -1) * m_bar->singleStep()));
                return true;
            }
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QScrollBar* m_bar;
};

/// Effective group of a command ("General" when the field is empty), matching QuickCommandStore::groups().
QString effectiveGroup(const QuickCommand& command)
{
    return command.group.trimmed().isEmpty() ? QString::fromLatin1(kGeneralGroup) : command.group;
}

/// Technical part of a button tooltip: the bytes that will be sent plus the line ending / HEX marker.
QString commandToolTip(const QuickCommand& command)
{
    QString tip;
    QByteArray bytes;
    QString error;
    if (command.hex) {
        if (HexUtils::parseHexString(command.command, bytes, &error)) {
            tip = HexUtils::toHexString(bytes) + QStringLiteral(" + HEX");
        } else {
            tip = command.command + QStringLiteral(" + HEX (") + error + QLatin1Char(')');
        }
    } else {
        bytes = command.escapes ? HexUtils::unescape(command.command, &error) : command.command.toUtf8();
        tip = HexUtils::escapeForDisplay(bytes) + QStringLiteral(" + ") + LineEnding::displayName(command.lineEnding);
        if (!error.isEmpty()) {
            tip += QStringLiteral(" (") + error + QLatin1Char(')');
        }
    }
    if (!command.tooltip.trimmed().isEmpty()) {
        tip += QLatin1Char('\n') + command.tooltip;
    }
    return tip;
}

/// Keep the scroll area exactly one button row high (plus the horizontal scroll bar when shown).
void fitScrollAreaHeight(QScrollArea* area, QWidget* host, int fallbackHeight)
{
    int height = host->sizeHint().height();
    if (height <= 0) {
        height = fallbackHeight;
    }
    if (area->horizontalScrollBar()->maximum() > 0) {
        height += area->horizontalScrollBar()->sizeHint().height();
    }
    height += area->frameWidth() * 2;
    area->setFixedHeight(height);
}

} // namespace

QuickCommandBar::QuickCommandBar(QuickCommandStore* store, QWidget* parent)
    : QWidget(parent)
    , m_store(store)
{
    setupUi();
    retranslate();

    if (m_store) {
        connect(m_store, &QuickCommandStore::changed, this, &QuickCommandBar::rebuild);
    }
    rebuild();
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void QuickCommandBar::setupUi()
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    m_groupCombo = new QComboBox(this);
    m_groupCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_groupCombo->setFocusPolicy(Qt::NoFocus);
    m_groupCombo->addItem(QString(), QString()); // "All" (text set in retranslate())

    m_buttonHost = new QWidget;
    m_buttonLayout = new QHBoxLayout(m_buttonHost);
    m_buttonLayout->setContentsMargins(0, 0, 0, 0);
    m_buttonLayout->setSpacing(2);
    m_buttonLayout->addStretch(1);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_scrollArea->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_scrollArea->setFocusPolicy(Qt::NoFocus);
    m_scrollArea->setWidget(m_buttonHost);
    m_scrollArea->viewport()->installEventFilter(
        new WheelToHorizontalFilter(m_scrollArea->horizontalScrollBar(), m_scrollArea));

    m_editButton = new QToolButton(this);
    m_editButton->setAutoRaise(true);
    m_editButton->setText(QStringLiteral("\u2699")); // gear
    m_editButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_editButton->setFocusPolicy(Qt::NoFocus);

    layout->addWidget(m_groupCombo);
    layout->addWidget(m_scrollArea, 1);
    layout->addWidget(m_editButton);

    // One shared filter for the middle-click -> edit gesture on the command buttons.
    auto* middleFilter = new MiddleClickFilter([this]() { emit editRequested(); }, this);
    middleFilter->setObjectName(QString::fromLatin1(kMiddleFilterName));

    // The scroll area never grows vertically; re-fit when the scroll bar appears/disappears.
    connect(m_scrollArea->horizontalScrollBar(), &QScrollBar::rangeChanged, this,
            [this](int, int) { fitScrollAreaHeight(m_scrollArea, m_buttonHost, m_editButton->sizeHint().height()); });
    fitScrollAreaHeight(m_scrollArea, m_buttonHost, m_editButton->sizeHint().height());

    connect(m_groupCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) {
            return;
        }
        QSettings settings;
        settings.setValue(QString::fromLatin1(kGroupSettingsKey), m_groupCombo->itemData(index).toString());
        rebuild();
    });
    connect(m_editButton, &QToolButton::clicked, this, &QuickCommandBar::editRequested);
}

void QuickCommandBar::retranslate()
{
    m_groupCombo->setItemText(0, tr("All"));
    m_groupCombo->setToolTip(tr("Quick command group"));
    m_editButton->setToolTip(tr("Edit quick commands..."));
    for (QToolButton* button : std::as_const(m_buttons)) {
        const QString base = button->property(kBaseToolTipProperty).toString();
        button->setToolTip(base + QLatin1Char('\n') + tr("Click: send, middle-click or Alt+click: edit"));
    }
}

void QuickCommandBar::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

QuickCommandStore* QuickCommandBar::store() const
{
    return m_store;
}

QString QuickCommandBar::currentGroup() const
{
    const int index = m_groupCombo->currentIndex();
    if (index <= 0) {
        return QString();
    }
    return m_groupCombo->itemData(index).toString();
}

void QuickCommandBar::setCurrentGroup(const QString& group)
{
    int index = group.isEmpty() ? 0 : m_groupCombo->findData(group);
    if (index < 0) {
        index = 0;
    }
    if (index == m_groupCombo->currentIndex()) {
        return;
    }
    m_groupCombo->setCurrentIndex(index); // persists the choice and rebuilds
}

void QuickCommandBar::setEnabledForConnection(bool connected)
{
    m_connected = connected;
    for (QToolButton* button : std::as_const(m_buttons)) {
        button->setEnabled(connected);
    }
}

// ---------------------------------------------------------------------------
// Rebuilding
// ---------------------------------------------------------------------------

void QuickCommandBar::rebuild()
{
    // ---- Group combo: "All" + the store's groups; restore the remembered choice ---------
    QString wanted;
    {
        QSettings settings;
        wanted = settings.value(QString::fromLatin1(kGroupSettingsKey)).toString();
    }

    const QStringList groups = m_store ? m_store->groups() : QStringList();
    {
        const QSignalBlocker blocker(m_groupCombo);
        while (m_groupCombo->count() > 1) {
            m_groupCombo->removeItem(m_groupCombo->count() - 1);
        }
        for (const QString& group : groups) {
            m_groupCombo->addItem(group, group);
        }
        int index = wanted.isEmpty() ? 0 : m_groupCombo->findData(wanted);
        if (index < 0) {
            index = 0; // remembered group no longer exists -> All (the setting is kept for when it returns)
        }
        m_groupCombo->setCurrentIndex(index);
    }
    const QString group = currentGroup();

    // ---- Buttons --------------------------------------------------------------------
    for (QToolButton* button : std::as_const(m_buttons)) {
        m_buttonLayout->removeWidget(button);
        button->hide();
        button->deleteLater();
    }
    m_buttons.clear();

    QObject* middleFilter = findChild<QObject*>(QString::fromLatin1(kMiddleFilterName), Qt::FindDirectChildrenOnly);

    const QList<QuickCommand> commands = m_store ? m_store->commands() : QList<QuickCommand>();
    int inserted = 0;
    for (const QuickCommand& command : commands) {
        if (!group.isEmpty() && effectiveGroup(command) != group) {
            continue;
        }

        auto* button = new QToolButton(m_buttonHost);
        button->setToolButtonStyle(Qt::ToolButtonTextOnly);
        button->setAutoRaise(true);
        button->setFocusPolicy(Qt::NoFocus);
        button->setText(command.name.trimmed().isEmpty() ? command.command : command.name);

        QString baseTip = commandToolTip(command);
        if (!command.shortcut.trimmed().isEmpty()) {
            const QKeySequence sequence(command.shortcut, QKeySequence::PortableText);
            if (!sequence.isEmpty()) {
                button->setShortcut(sequence);
                baseTip += QLatin1Char('\n') + tr("Shortcut: %1").arg(sequence.toString(QKeySequence::NativeText));
            }
        }
        button->setProperty(kBaseToolTipProperty, baseTip);
        button->setToolTip(baseTip + QLatin1Char('\n') + tr("Click: send, middle-click or Alt+click: edit"));
        button->setEnabled(m_connected);
        if (middleFilter) {
            button->installEventFilter(middleFilter);
        }

        connect(button, &QToolButton::clicked, this, [this, command]() {
            if (QApplication::keyboardModifiers().testFlag(Qt::AltModifier)) {
                emit editRequested();
            } else {
                emit commandTriggered(command);
            }
        });

        m_buttonLayout->insertWidget(inserted, button);
        ++inserted;
        m_buttons.append(button);
    }

    m_buttonHost->adjustSize();
    fitScrollAreaHeight(m_scrollArea, m_buttonHost, m_editButton->sizeHint().height());

    qCDebug(lcUi) << "quick command bar rebuilt:" << inserted << "button(s), group"
                  << (group.isEmpty() ? QStringLiteral("All") : group);
}
