#include "ui/ConnectionBar.h"

#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QIntValidator>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStyle>
#include <QToolButton>

#include "app/Logging.h"

namespace {

constexpr int kPortComboMinWidth = 220;
constexpr int kDotSizePx = 10;
constexpr qint32 kFallbackBaud = 115200;

/// Item data roles used by the port combo (Qt::UserRole holds the port name).
constexpr int kPlaceholderRole = Qt::UserRole + 1;

const QColor kDotConnected(0x2E, 0xCC, 0x71);    // green
const QColor kDotDisconnected(0xE7, 0x4C, 0x3C); // red
const QColor kDotReconnecting(0xF1, 0xC4, 0x0F); // amber
const QColor kDotUnavailable(0x95, 0xA5, 0xA6);  // grey

/// A small filled circle used as the Connect button icon.
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

/// Combo text for a port that is selected but no longer enumerated.
QString unavailableText(const QString& portName)
{
    return QStringLiteral("%1 (%2)").arg(portName, ConnectionBar::tr("unavailable"));
}

/// Select the item whose Qt::UserRole data equals `value`; returns false when absent.
bool selectByData(QComboBox* combo, const QVariant& value)
{
    const int index = combo->findData(value);
    if (index < 0) {
        return false;
    }
    combo->setCurrentIndex(index);
    return true;
}

} // namespace

ConnectionBar::ConnectionBar(QWidget* parent)
    : QWidget(parent)
{
    setupUi();
    retranslate();
    setPorts(SerialPortEnumerator::instance().ports());
    setConnectionState(SerialConnection::State::Disconnected);
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

void ConnectionBar::setupUi()
{
    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(4, 2, 4, 2);
    layout->setSpacing(4);

    // Port
    m_portLabel = new QLabel(this);
    m_portCombo = new QComboBox(this);
    m_portCombo->setObjectName(QStringLiteral("portCombo"));
    m_portCombo->setMinimumWidth(kPortComboMinWidth);
    m_portCombo->setSizeAdjustPolicy(QComboBox::AdjustToContentsOnFirstShow);
    m_portCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_portCombo->setMaximumWidth(420);
    m_portLabel->setBuddy(m_portCombo);

    m_refreshButton = new QToolButton(this);
    m_refreshButton->setObjectName(QStringLiteral("refreshButton"));
    m_refreshButton->setAutoRaise(true);
    m_refreshButton->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
    m_refreshButton->setFocusPolicy(Qt::NoFocus);

    // Baud
    m_baudLabel = new QLabel(this);
    m_baudCombo = new QComboBox(this);
    m_baudCombo->setObjectName(QStringLiteral("baudCombo"));
    m_baudCombo->setEditable(true);
    m_baudCombo->setInsertPolicy(QComboBox::NoInsert);
    m_baudCombo->setValidator(
        new QIntValidator(SerialSettings::kMinBaudRate, SerialSettings::kMaxBaudRate, m_baudCombo));
    m_baudCombo->setMinimumContentsLength(8);
    m_baudCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    for (const qint32 baud : SerialSettings::standardBaudRates()) {
        m_baudCombo->addItem(QString::number(baud), baud);
    }
    m_baudLabel->setBuddy(m_baudCombo);

    // Data bits
    m_dataBitsCombo = new QComboBox(this);
    m_dataBitsCombo->setObjectName(QStringLiteral("dataBitsCombo"));
    m_dataBitsCombo->addItem(QStringLiteral("5"), static_cast<int>(QSerialPort::Data5));
    m_dataBitsCombo->addItem(QStringLiteral("6"), static_cast<int>(QSerialPort::Data6));
    m_dataBitsCombo->addItem(QStringLiteral("7"), static_cast<int>(QSerialPort::Data7));
    m_dataBitsCombo->addItem(QStringLiteral("8"), static_cast<int>(QSerialPort::Data8));
    m_dataBitsCombo->setCurrentIndex(3);

    // Parity
    m_parityCombo = new QComboBox(this);
    m_parityCombo->setObjectName(QStringLiteral("parityCombo"));
    m_parityCombo->addItem(QStringLiteral("N"), static_cast<int>(QSerialPort::NoParity));
    m_parityCombo->addItem(QStringLiteral("E"), static_cast<int>(QSerialPort::EvenParity));
    m_parityCombo->addItem(QStringLiteral("O"), static_cast<int>(QSerialPort::OddParity));
    m_parityCombo->addItem(QStringLiteral("S"), static_cast<int>(QSerialPort::SpaceParity));
    m_parityCombo->addItem(QStringLiteral("M"), static_cast<int>(QSerialPort::MarkParity));
    m_parityCombo->setCurrentIndex(0);

    // Stop bits
    m_stopBitsCombo = new QComboBox(this);
    m_stopBitsCombo->setObjectName(QStringLiteral("stopBitsCombo"));
    m_stopBitsCombo->addItem(QStringLiteral("1"), static_cast<int>(QSerialPort::OneStop));
    m_stopBitsCombo->addItem(QStringLiteral("1.5"), static_cast<int>(QSerialPort::OneAndHalfStop));
    m_stopBitsCombo->addItem(QStringLiteral("2"), static_cast<int>(QSerialPort::TwoStop));
    m_stopBitsCombo->setCurrentIndex(0);

    // Flow control
    m_flowLabel = new QLabel(this);
    m_flowCombo = new QComboBox(this);
    m_flowCombo->setObjectName(QStringLiteral("flowCombo"));
    m_flowCombo->addItem(QString(), static_cast<int>(QSerialPort::NoFlowControl)); // text set in retranslate()
    m_flowCombo->addItem(QStringLiteral("RTS/CTS"), static_cast<int>(QSerialPort::HardwareControl));
    m_flowCombo->addItem(QStringLiteral("XON/XOFF"), static_cast<int>(QSerialPort::SoftwareControl));
    m_flowCombo->setCurrentIndex(0);
    m_flowLabel->setBuddy(m_flowCombo);

    for (QComboBox* combo : {m_dataBitsCombo, m_parityCombo, m_stopBitsCombo, m_flowCombo}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    }

    // Modem lines / break
    m_dtrButton = new QToolButton(this);
    m_dtrButton->setObjectName(QStringLiteral("dtrButton"));
    m_dtrButton->setText(QStringLiteral("DTR"));
    m_dtrButton->setCheckable(true);
    m_dtrButton->setChecked(true);
    m_dtrButton->setFocusPolicy(Qt::NoFocus);

    m_rtsButton = new QToolButton(this);
    m_rtsButton->setObjectName(QStringLiteral("rtsButton"));
    m_rtsButton->setText(QStringLiteral("RTS"));
    m_rtsButton->setCheckable(true);
    m_rtsButton->setChecked(true);
    m_rtsButton->setFocusPolicy(Qt::NoFocus);

    m_breakButton = new QToolButton(this);
    m_breakButton->setObjectName(QStringLiteral("breakButton"));
    m_breakButton->setFocusPolicy(Qt::NoFocus);

    // Connect
    m_connectButton = new QPushButton(this);
    m_connectButton->setObjectName(QStringLiteral("connectButton"));
    m_connectButton->setAutoDefault(false);
    m_connectButton->setDefault(false);
    m_connectButton->setFocusPolicy(Qt::NoFocus);
    m_connectButton->setMinimumWidth(130);

    layout->addWidget(m_portLabel);
    layout->addWidget(m_portCombo, 1);
    layout->addWidget(m_refreshButton);
    layout->addSpacing(6);
    layout->addWidget(m_baudLabel);
    layout->addWidget(m_baudCombo);
    layout->addWidget(m_dataBitsCombo);
    layout->addWidget(m_parityCombo);
    layout->addWidget(m_stopBitsCombo);
    layout->addSpacing(6);
    layout->addWidget(m_flowLabel);
    layout->addWidget(m_flowCombo);
    layout->addSpacing(6);
    layout->addWidget(m_dtrButton);
    layout->addWidget(m_rtsButton);
    layout->addWidget(m_breakButton);
    layout->addStretch(1);
    layout->addWidget(m_connectButton);

    // ---- Signals --------------------------------------------------------------------
    connect(m_portCombo, &QComboBox::currentIndexChanged, this, [this](int) { emitSettingsChanged(); });
    connect(m_refreshButton, &QToolButton::clicked, this, &ConnectionBar::refreshRequested);

    connect(m_baudCombo, &QComboBox::currentIndexChanged, this, [this](int) { emitSettingsChanged(); });
    if (QLineEdit* edit = m_baudCombo->lineEdit()) {
        connect(edit, &QLineEdit::editingFinished, this, [this]() { emitSettingsChanged(); });
    }
    for (QComboBox* combo : {m_dataBitsCombo, m_parityCombo, m_stopBitsCombo, m_flowCombo}) {
        connect(combo, &QComboBox::currentIndexChanged, this, [this](int) { emitSettingsChanged(); });
    }
    // The parity combo's own tooltip follows the selected item so the letter is explained.
    connect(m_parityCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
        m_parityCombo->setToolTip(m_parityCombo->itemData(index, Qt::ToolTipRole).toString());
    });

    connect(m_dtrButton, &QToolButton::toggled, this, [this](bool on) {
        if (!m_updating) {
            emit dtrToggled(on);
            emitSettingsChanged();
        }
    });
    connect(m_rtsButton, &QToolButton::toggled, this, [this](bool on) {
        if (!m_updating) {
            emit rtsToggled(on);
            emitSettingsChanged();
        }
    });
    connect(m_breakButton, &QToolButton::clicked, this, &ConnectionBar::sendBreakRequested);

    connect(m_connectButton, &QPushButton::clicked, this, [this]() {
        if (m_state == SerialConnection::State::Disconnected) {
            emit connectRequested();
        } else {
            emit disconnectRequested();
        }
    });
}

void ConnectionBar::retranslate()
{
    m_portLabel->setText(tr("Port:"));
    m_baudLabel->setText(tr("Baud:"));
    m_flowLabel->setText(tr("Flow:"));

    m_portCombo->setPlaceholderText(tr("Select a port"));
    m_portCombo->setToolTip(tr("Serial port"));
    m_refreshButton->setToolTip(tr("Refresh the port list"));
    m_baudCombo->setToolTip(tr("Baud rate (type a custom value and press Enter)"));
    m_dataBitsCombo->setToolTip(tr("Data bits"));
    m_stopBitsCombo->setToolTip(tr("Stop bits"));
    m_flowCombo->setToolTip(tr("Flow control"));

    m_parityCombo->setItemData(0, tr("Parity: None"), Qt::ToolTipRole);
    m_parityCombo->setItemData(1, tr("Parity: Even"), Qt::ToolTipRole);
    m_parityCombo->setItemData(2, tr("Parity: Odd"), Qt::ToolTipRole);
    m_parityCombo->setItemData(3, tr("Parity: Space"), Qt::ToolTipRole);
    m_parityCombo->setItemData(4, tr("Parity: Mark"), Qt::ToolTipRole);
    m_parityCombo->setToolTip(m_parityCombo->itemData(m_parityCombo->currentIndex(), Qt::ToolTipRole).toString());

    m_flowCombo->setItemText(0, tr("None"));

    m_dtrButton->setToolTip(tr("Assert DTR (Data Terminal Ready)"));
    m_rtsButton->setToolTip(tr("Assert RTS (Request To Send)"));
    m_breakButton->setText(tr("Break"));
    m_breakButton->setToolTip(tr("Send BREAK (250 ms)"));

    // Refresh any "(unavailable)" placeholder text and the connect button caption.
    for (int i = 0; i < m_portCombo->count(); ++i) {
        if (m_portCombo->itemData(i, kPlaceholderRole).toBool()) {
            m_portCombo->setItemText(i, unavailableText(m_portCombo->itemData(i).toString()));
        }
    }
    setConnectionState(m_state);
}

void ConnectionBar::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

// ---------------------------------------------------------------------------
// Settings <-> controls
// ---------------------------------------------------------------------------

SerialSettings ConnectionBar::settings() const
{
    SerialSettings s;
    s.portName = selectedPortName();

    bool ok = false;
    const qint32 baud = m_baudCombo->currentText().trimmed().toInt(&ok);
    s.baudRate = (ok && SerialSettings::isValidBaudRate(baud)) ? baud : kFallbackBaud;

    s.dataBits = static_cast<QSerialPort::DataBits>(m_dataBitsCombo->currentData().toInt());
    s.parity = static_cast<QSerialPort::Parity>(m_parityCombo->currentData().toInt());
    s.stopBits = static_cast<QSerialPort::StopBits>(m_stopBitsCombo->currentData().toInt());
    s.flowControl = static_cast<QSerialPort::FlowControl>(m_flowCombo->currentData().toInt());
    s.dtr = m_dtrButton->isChecked();
    s.rts = m_rtsButton->isChecked();
    return s;
}

void ConnectionBar::setSettings(const SerialSettings& settings)
{
    const bool wasUpdating = m_updating;
    m_updating = true;

    selectPort(settings.portName);

    const int baudIndex = m_baudCombo->findData(settings.baudRate);
    if (baudIndex >= 0) {
        m_baudCombo->setCurrentIndex(baudIndex);
    } else {
        m_baudCombo->setCurrentIndex(-1);
        m_baudCombo->setEditText(QString::number(settings.baudRate));
    }

    selectByData(m_dataBitsCombo, static_cast<int>(settings.dataBits));
    selectByData(m_parityCombo, static_cast<int>(settings.parity));
    selectByData(m_stopBitsCombo, static_cast<int>(settings.stopBits));
    selectByData(m_flowCombo, static_cast<int>(settings.flowControl));
    m_dtrButton->setChecked(settings.dtr);
    m_rtsButton->setChecked(settings.rts);

    m_updating = wasUpdating;
    m_hasEmitted = false;   // programmatic change: report the next user change even if it repeats the last emitted value
}

QString ConnectionBar::selectedPortName() const
{
    return m_portCombo->currentData().toString();
}

void ConnectionBar::selectPort(const QString& portName)
{
    const bool wasUpdating = m_updating;
    m_updating = true;

    // Drop stale placeholders (a previously selected port that vanished) unless it is the one wanted.
    for (int i = m_portCombo->count() - 1; i >= 0; --i) {
        if (m_portCombo->itemData(i, kPlaceholderRole).toBool() && m_portCombo->itemData(i).toString() != portName) {
            m_portCombo->removeItem(i);
        }
    }

    if (portName.isEmpty()) {
        m_portCombo->setCurrentIndex(-1);
    } else if (!selectByData(m_portCombo, portName)) {
        m_portCombo->addItem(unavailableText(portName), portName);
        const int index = m_portCombo->count() - 1;
        m_portCombo->setItemData(index, true, kPlaceholderRole);
        m_portCombo->setItemData(index, tr("%1 is not currently available").arg(portName), Qt::ToolTipRole);
        // Not selectable from the popup, but it can be the current item programmatically.
        if (auto* model = qobject_cast<QStandardItemModel*>(m_portCombo->model())) {
            if (QStandardItem* item = model->item(index)) {
                item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
            }
        }
        m_portCombo->setCurrentIndex(index);
    }

    m_updating = wasUpdating;
    m_hasEmitted = false;   // programmatic change: report the next user change even if it repeats the last emitted value
    setConnectionState(m_state); // the connect button's dot depends on whether a port is selected
}

void ConnectionBar::setPorts(const QList<SerialPortEntry>& ports)
{
    const QString selected = selectedPortName();

    const bool wasUpdating = m_updating;
    m_updating = true;
    {
        const QSignalBlocker blocker(m_portCombo);
        m_portCombo->clear();
        for (const SerialPortEntry& entry : ports) {
            m_portCombo->addItem(entry.displayText(), entry.portName);
            const int index = m_portCombo->count() - 1;
            QString tip = entry.toolTip();
            const QString hint = entry.kindHint();
            if (!hint.isEmpty()) {
                tip = tip.isEmpty() ? hint : tip + QLatin1Char('\n') + hint;
            }
            m_portCombo->setItemData(index, tip, Qt::ToolTipRole);
        }
        // Restore the selection (adds a disabled placeholder when the port vanished).
        m_portCombo->setCurrentIndex(-1);
        if (!selected.isEmpty()) {
            selectPort(selected);
        }
    }
    m_updating = wasUpdating;
    m_hasEmitted = false;   // programmatic change: report the next user change even if it repeats the last emitted value
    setConnectionState(m_state);

    qCDebug(lcUi) << "port list updated:" << ports.size() << "port(s), selected" << selected;
}

void ConnectionBar::setConnectionState(SerialConnection::State state)
{
    m_state = state;
    const qreal dpr = devicePixelRatioF();

    switch (state) {
    case SerialConnection::State::Connected:
        m_connectButton->setText(tr("Disconnect"));
        m_connectButton->setIcon(dotIcon(kDotDisconnected, dpr));
        m_connectButton->setToolTip(tr("Close the port"));
        break;
    case SerialConnection::State::Reconnecting:
        m_connectButton->setText(tr("Reconnecting..."));
        m_connectButton->setIcon(dotIcon(kDotReconnecting, dpr));
        m_connectButton->setToolTip(tr("Waiting for the port to come back (click to cancel)"));
        break;
    case SerialConnection::State::Disconnected:
        m_connectButton->setText(tr("Connect"));
        m_connectButton->setIcon(dotIcon(selectedPortName().isEmpty() ? kDotUnavailable : kDotConnected, dpr));
        m_connectButton->setToolTip(tr("Open the selected port"));
        break;
    }

    const bool idle = (state == SerialConnection::State::Disconnected);
    m_portCombo->setEnabled(idle);
    m_refreshButton->setEnabled(idle);
    m_breakButton->setEnabled(state == SerialConnection::State::Connected);
}

void ConnectionBar::setPinStates(bool dtr, bool rts)
{
    const bool wasUpdating = m_updating;
    m_updating = true;
    m_dtrButton->setChecked(dtr);
    m_rtsButton->setChecked(rts);
    m_updating = wasUpdating;
    m_hasEmitted = false;   // programmatic change: report the next user change even if it repeats the last emitted value
}

void ConnectionBar::setFocusToPort()
{
    m_portCombo->setFocus(Qt::OtherFocusReason);
}

void ConnectionBar::emitSettingsChanged()
{
    if (m_updating) {
        return;
    }
    // The connect button's dot reflects whether a port is selected.
    if (m_state == SerialConnection::State::Disconnected) {
        m_connectButton->setIcon(
            dotIcon(selectedPortName().isEmpty() ? kDotUnavailable : kDotConnected, devicePixelRatioF()));
    }
    // The editable baud combo reports both currentIndexChanged and (on focus loss, including
    // the focus loss of a closing window) editingFinished: emit only when something differs
    // from what listeners already know.
    const SerialSettings current = settings();
    if (m_hasEmitted && current == m_lastEmitted) {
        return;
    }
    m_lastEmitted = current;
    m_hasEmitted = true;
    emit settingsChanged(current);
}
