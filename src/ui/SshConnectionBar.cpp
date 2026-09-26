#include "ui/SshConnectionBar.h"

#include <QComboBox>
#include <QCompleter>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>

#include "app/Logging.h"

namespace {

constexpr int kTargetComboMinWidth = 260;
constexpr int kTargetComboMaxWidth = 560;
constexpr int kConnectButtonMinWidth = 130;
constexpr int kDotSizePx = 10;

const QColor kDotConnected(0x2E, 0xCC, 0x71);    // green
const QColor kDotDisconnected(0xE7, 0x4C, 0x3C); // red
const QColor kDotBusy(0xF1, 0xC4, 0x0F);         // amber
const QColor kDotUnavailable(0x95, 0xA5, 0xA6);  // grey
const QColor kInvalidText(0xE7, 0x4C, 0x3C);     // red text for an unparsable target

/// Item data roles of the target combo (Qt::UserRole holds the profile id).
constexpr int kTargetRole = Qt::UserRole + 1;    ///< displayTarget() of a stored profile item

/// A small filled circle used as the Connect button icon (same look as ConnectionBar).
QIcon dotIcon(const QColor& color, qreal devicePixelRatio)
{
    const int px = qMax(1, qRound(kDotSizePx * devicePixelRatio));
    QPixmap pixmap(px, px);
    pixmap.setDevicePixelRatio(devicePixelRatio);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(QRectF(0.5, 0.5, kDotSizePx - 1.0, kDotSizePx - 1.0));
    painter.end();
    return QIcon(pixmap);
}

/// Combo text of a stored profile: "name - user@host:port" (just the target when unnamed).
QString profileItemText(const SshProfile& profile)
{
    const QString name = profile.name.trimmed();
    const QString target = profile.displayTarget();
    if (name.isEmpty() || name == target) {
        return target;
    }
    return QStringLiteral("%1 - %2").arg(name, target);
}

/// QComboBox::insertSeparator() marks the item with this accessible description.
bool isSeparator(const QComboBox* combo, int index)
{
    return combo->itemData(index, Qt::AccessibleDescriptionRole).toString() == QLatin1String("separator");
}

/// The recent-target item (no profile id, not a separator) whose text equals `text`, or -1.
int findRecentItem(const QComboBox* combo, const QString& text)
{
    for (int i = 0; i < combo->count(); ++i) {
        if (!isSeparator(combo, i) && combo->itemData(i).toString().isEmpty() && combo->itemText(i) == text) {
            return i;
        }
    }
    return -1;
}

} // namespace

SshConnectionBar::SshConnectionBar(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
    retranslate();
    rebuild();
    setConnectionState(Transport::State::Disconnected);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void SshConnectionBar::setupUi()
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    // Target
    m_targetLabel = new QLabel(this);
    m_targetCombo = new QComboBox(this);
    m_targetCombo->setObjectName(QStringLiteral("targetCombo"));
    m_targetCombo->setEditable(true);
    m_targetCombo->setInsertPolicy(QComboBox::NoInsert);   // recent targets are managed by the store
    m_targetCombo->setMinimumWidth(kTargetComboMinWidth);
    m_targetCombo->setMaximumWidth(kTargetComboMaxWidth);
    m_targetCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    m_targetCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_targetLabel->setBuddy(m_targetCombo);

    // Substring completion over the profiles and recent targets ("pico" finds "Luckfox Pico - ...").
    auto* completer = new QCompleter(m_targetCombo->model(), m_targetCombo);
    completer->setCaseSensitivity(Qt::CaseInsensitive);
    completer->setFilterMode(Qt::MatchContains);
    completer->setCompletionMode(QCompleter::PopupCompletion);
    m_targetCombo->setCompleter(completer);
    m_targetCombo->installEventFilter(this);   // Return/Enter = connect, see eventFilter()

    m_profilesButton = new QToolButton(this);
    m_profilesButton->setObjectName(QStringLiteral("profilesButton"));
    m_profilesButton->setAutoRaise(true);
    m_profilesButton->setText(QStringLiteral("⚙"));   // gear
    m_profilesButton->setFocusPolicy(Qt::NoFocus);

    // Host key type / auth method once connected
    m_infoLabel = new QLabel(this);
    m_infoLabel->setObjectName(QStringLiteral("infoLabel"));
    m_infoLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_infoLabel->hide();

    // Connect
    m_connectButton = new QPushButton(this);
    m_connectButton->setObjectName(QStringLiteral("connectButton"));
    m_connectButton->setAutoDefault(false);
    m_connectButton->setDefault(false);
    m_connectButton->setFocusPolicy(Qt::NoFocus);
    m_connectButton->setMinimumWidth(kConnectButtonMinWidth);

    layout->addWidget(m_targetLabel);
    layout->addWidget(m_targetCombo, 1);
    layout->addWidget(m_profilesButton);
    layout->addSpacing(6);
    layout->addWidget(m_infoLabel);
    layout->addStretch(1);
    layout->addWidget(m_connectButton);

    // ---- Signals --------------------------------------------------------------------
    connect(m_targetCombo, &QComboBox::currentIndexChanged, this, [this](int) {
        if (m_updating) {
            return;
        }
        updateTargetValidity();
        emitProfileChangedIfValid();
    });
    connect(m_targetCombo, &QComboBox::editTextChanged, this, [this](const QString&) {
        if (!m_updating) {
            updateTargetValidity();
        }
    });
    if (QLineEdit* edit = m_targetCombo->lineEdit()) {
        connect(edit, &QLineEdit::editingFinished, this, [this]() {
            if (!m_updating) {
                emitProfileChangedIfValid();
            }
        });
        // Enter in the target field = Connect (the combo's own returnPressed handling is a no-op
        // with NoInsert; a typed recent target is matched to its item by editingFinished).
        connect(edit, &QLineEdit::returnPressed, this, &SshConnectionBar::handleReturnKey);
    }
    connect(m_profilesButton, &QToolButton::clicked, this, &SshConnectionBar::profilesEditRequested);
    connect(m_connectButton, &QPushButton::clicked, this, [this]() {
        if (m_state == Transport::State::Disconnected) {
            emit connectRequested();
        } else {
            emit disconnectRequested();   // Connecting / Reconnecting: cancel; Connected: close
        }
    });
}

void SshConnectionBar::retranslate()
{
    m_targetLabel->setText(tr("Target:"));
    if (QLineEdit* edit = m_targetCombo->lineEdit()) {
        edit->setPlaceholderText(tr("user@host[:port]"));
    }
    m_profilesButton->setToolTip(tr("Manage SSH profiles..."));
    m_infoLabel->setToolTip(tr("Host key type and authentication method"));
    updateTargetValidity();      // combo tooltip (normal or "Enter user@host[:port]")
    setConnectionState(m_state); // connect button caption and tooltip
}

void SshConnectionBar::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

bool SshConnectionBar::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == m_targetCombo && event->type() == QEvent::KeyPress) {
        const auto* key = static_cast<QKeyEvent*>(event);
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            // Focus can sit on the combo itself (a fresh tab focused through setFocusToTarget())
            // rather than on its line edit: then the key would never reach returnPressed(). When
            // the line edit did see it first, handleReturnKey() ignores this second delivery.
            handleReturnKey();
            return true;   // never travels on to a default button of the window
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SshConnectionBar::handleReturnKey()
{
    if (m_updating) {
        return;
    }
    if (m_returnGuard.isValid() && m_returnGuard.elapsed() < 150) {
        return;   // the same key press, delivered a second time (line edit, then the combo)
    }
    m_returnGuard.start();
    emitProfileChangedIfValid();
    if (m_state == Transport::State::Disconnected && hasValidTarget()) {
        emit connectRequested();
    }
}

// ---------------------------------------------------------------------------
// Store / items
// ---------------------------------------------------------------------------

void SshConnectionBar::setStore(SshProfileStore* store)
{
    if (m_store == store) {
        return;
    }
    if (m_storeConnection) {
        disconnect(m_storeConnection);
    }
    m_store = store;
    if (m_store) {
        m_storeConnection = connect(m_store, &SshProfileStore::changed, this, &SshConnectionBar::rebuild);
        connect(m_store, &QObject::destroyed, this, [this, store]() {
            if (m_store == store) {
                m_store = nullptr;
                rebuild();
            }
        });
    }
    rebuild();
}

SshProfileStore* SshConnectionBar::store() const
{
    return m_store;
}

void SshConnectionBar::rebuild()
{
    // What is shown now, so it can be restored: a stored profile by id, otherwise the text.
    const QString beforeId = selectedStoredId();
    QString beforeText = m_targetCombo->currentText().trimmed();
    QString beforeTarget;   // the stored profile's user@host:port (the store may have dropped it already)
    if (!beforeId.isEmpty()) {
        beforeTarget = m_targetCombo->itemData(m_targetCombo->currentIndex(), kTargetRole).toString();
    }

    const bool wasUpdating = m_updating;
    m_updating = true;
    {
        const QSignalBlocker blocker(m_targetCombo);
        m_targetCombo->clear();
        QList<SshProfile> profiles;
        QStringList recent;
        if (m_store) {
            profiles = m_store->profiles();
            recent = m_store->recentTargets();
        }
        for (const SshProfile& profile : profiles) {
            m_targetCombo->addItem(profileItemText(profile), profile.id);
            QString tip = SshProfile::authText(profile.auth);
            if (!profile.identityFile.isEmpty()) {
                tip += QStringLiteral(" (%1)").arg(profile.identityFile);
            }
            if (!profile.description.trimmed().isEmpty()) {
                tip = profile.description.trimmed() + QLatin1Char('\n') + tip;
            }
            m_targetCombo->setItemData(m_targetCombo->count() - 1, tip, Qt::ToolTipRole);
            m_targetCombo->setItemData(m_targetCombo->count() - 1, profile.displayTarget(), kTargetRole);
        }
        if (!profiles.isEmpty() && !recent.isEmpty()) {
            m_targetCombo->insertSeparator(m_targetCombo->count());
        }
        for (const QString& target : recent) {
            m_targetCombo->addItem(target);
        }

        m_targetCombo->setCurrentIndex(-1);
        bool restored = false;
        if (!beforeId.isEmpty()) {
            const int index = m_targetCombo->findData(beforeId);
            if (index >= 0) {
                m_targetCombo->setCurrentIndex(index);
                restored = true;
            } else if (!beforeTarget.isEmpty()) {
                // The selected profile was deleted from the store: keep its target as free text.
                beforeText = beforeTarget;
            }
        }
        if (!restored && !beforeText.isEmpty()) {
            const int index = findRecentItem(m_targetCombo, beforeText);
            if (index >= 0) {
                m_targetCombo->setCurrentIndex(index);
            } else {
                m_targetCombo->setEditText(beforeText);
            }
        }
    }
    m_updating = wasUpdating;
    m_hasEmitted = false;   // programmatic change: report the next user change even if it repeats the last one
    updateTargetValidity();

    qCDebug(lcSsh) << "SSH target list rebuilt:" << m_targetCombo->count() << "item(s), selected" << targetText();
}

QString SshConnectionBar::selectedStoredId() const
{
    const int index = m_targetCombo->currentIndex();
    if (index < 0 || isSeparator(m_targetCombo, index)) {
        return QString();
    }
    const QString id = m_targetCombo->itemData(index).toString();
    if (id.isEmpty()) {
        return QString();
    }
    // The line edit may have been edited after the item was picked: then the text rules.
    if (m_targetCombo->itemText(index) != m_targetCombo->currentText().trimmed()) {
        return QString();
    }
    return id;
}

// ---------------------------------------------------------------------------
// Target
// ---------------------------------------------------------------------------

SshProfile SshConnectionBar::currentProfile() const
{
    const QString id = selectedStoredId();
    if (!id.isEmpty() && m_store) {
        if (const std::optional<SshProfile> stored = m_store->profile(id)) {
            return *stored;
        }
    }
    SshProfile adHoc;   // no id, Auth::Auto
    if (!SshProfile::parseTarget(targetText(), adHoc)) {
        return SshProfile();
    }
    return adHoc;
}

bool SshConnectionBar::hasValidTarget() const
{
    const QString id = selectedStoredId();
    if (!id.isEmpty()) {
        return m_store && m_store->profile(id).has_value();
    }
    SshProfile probe;
    return SshProfile::parseTarget(targetText(), probe);
}

void SshConnectionBar::selectProfile(const QString& id)
{
    if (id.isEmpty()) {
        return;
    }
    const int index = m_targetCombo->findData(id);
    if (index < 0) {
        qCDebug(lcSsh) << "SSH profile not in the target list:" << id;
        return;
    }
    const bool wasUpdating = m_updating;
    m_updating = true;
    m_targetCombo->setCurrentIndex(index);
    m_updating = wasUpdating;
    m_hasEmitted = false;
    updateTargetValidity();
}

void SshConnectionBar::setTarget(const QString& text)
{
    const QString trimmed = text.trimmed();
    const bool wasUpdating = m_updating;
    m_updating = true;
    m_targetCombo->setCurrentIndex(-1);
    const int index = findRecentItem(m_targetCombo, trimmed);
    if (index >= 0) {
        m_targetCombo->setCurrentIndex(index);
    } else {
        m_targetCombo->setEditText(trimmed);
    }
    m_updating = wasUpdating;
    m_hasEmitted = false;
    updateTargetValidity();
}

QString SshConnectionBar::targetText() const
{
    return m_targetCombo->currentText().trimmed();
}

void SshConnectionBar::setFocusToTarget()
{
    m_targetCombo->setFocus(Qt::OtherFocusReason);
    if (QLineEdit* edit = m_targetCombo->lineEdit()) {
        edit->selectAll();
    }
}

void SshConnectionBar::updateTargetValidity()
{
    const bool valid = hasValidTarget();
    const bool empty = targetText().isEmpty();
    QLineEdit* edit = m_targetCombo->lineEdit();
    if (!valid && !empty) {
        if (edit) {
            QPalette palette = edit->palette();
            palette.setColor(QPalette::Text, kInvalidText);
            edit->setPalette(palette);
        }
        m_targetCombo->setToolTip(tr("Enter user@host[:port]"));
    } else {
        if (edit) {
            edit->setPalette(QPalette());   // back to the inherited palette
        }
        m_targetCombo->setToolTip(tr("Saved profile or user@host[:port] (also ssh://user@host:port, [v6]:port)"));
    }
    if (edit) {
        edit->setToolTip(m_targetCombo->toolTip());
    }
    if (m_state == Transport::State::Disconnected) {
        m_connectButton->setIcon(dotIcon(valid ? kDotConnected : kDotUnavailable, devicePixelRatioF()));
    }
}

void SshConnectionBar::emitProfileChangedIfValid()
{
    if (m_updating || !hasValidTarget()) {
        return;
    }
    const SshProfile profile = currentProfile();
    const QString key = profile.id.isEmpty() ? QStringLiteral("target:") + profile.displayTarget()
                                             : QStringLiteral("id:") + profile.id;
    // An editable combo reports both currentIndexChanged and editingFinished (Enter, focus loss):
    // report only what listeners do not already know.
    if (m_hasEmitted && key == m_lastEmittedKey) {
        return;
    }
    m_lastEmittedKey = key;
    m_hasEmitted = true;
    emit profileChanged(profile);
}

// ---------------------------------------------------------------------------
// Connection state
// ---------------------------------------------------------------------------

void SshConnectionBar::setConnectionState(Transport::State state)
{
    m_state = state;
    const qreal dpr = devicePixelRatioF();

    switch (state) {
    case Transport::State::Connected:
        m_connectButton->setText(tr("Disconnect"));
        m_connectButton->setIcon(dotIcon(kDotDisconnected, dpr));
        m_connectButton->setToolTip(tr("Close the SSH session"));
        break;
    case Transport::State::Connecting:
        m_connectButton->setText(tr("Connecting..."));
        m_connectButton->setIcon(dotIcon(kDotBusy, dpr));
        m_connectButton->setToolTip(tr("Connecting (click to cancel)"));
        break;
    case Transport::State::Reconnecting:
        m_connectButton->setText(tr("Reconnecting..."));
        m_connectButton->setIcon(dotIcon(kDotBusy, dpr));
        m_connectButton->setToolTip(tr("Waiting for the link to come back (click to cancel)"));
        break;
    case Transport::State::Disconnected:
        m_connectButton->setText(tr("Connect"));
        m_connectButton->setIcon(dotIcon(hasValidTarget() ? kDotConnected : kDotUnavailable, dpr));
        m_connectButton->setToolTip(tr("Open an SSH session to the selected target"));
        break;
    }

    const bool idle = (state == Transport::State::Disconnected);
    m_targetCombo->setEnabled(idle);
    m_profilesButton->setEnabled(idle);
    m_infoLabel->setVisible(!idle && !m_summary.isEmpty());
}

void SshConnectionBar::setSummary(const QString& text)
{
    m_summary = text.trimmed();
    m_infoLabel->setText(m_summary);
    m_infoLabel->setVisible(m_state != Transport::State::Disconnected && !m_summary.isEmpty());
}
