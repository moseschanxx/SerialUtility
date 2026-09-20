#include "ui/SystemLogViewer.h"

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QMutexLocker>
#include <QScrollBar>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>

#include <utility>

#include "app/AppSettings.h"

// Register LogLevel with the meta-type system before any queued invocation uses it.
[[maybe_unused]] static const int s_logLevelMetaTypeId = qRegisterMetaType<LogLevel>("LogLevel");

SystemLogViewer* SystemLogViewer::s_instance = nullptr;
QMutex SystemLogViewer::s_instanceMutex;

namespace {

/// The Qt message handler that was active before installMessageHandler(); still called so
/// output reaches the debugger / stderr.
QtMessageHandler s_previousHandler = nullptr;
bool s_handlerInstalled = false;

/// Messages emitted while no instance is registered (e.g. during MainWindow's constructor),
/// guarded by SystemLogViewer::s_instanceMutex; delivered by flushPendingLocked().
struct Pending
{
    QDateTime timestamp;
    LogLevel level = LogLevel::Info;
    QString category;
    QString message;
};
QList<Pending> s_pending;
constexpr int kMaxPending = 512;

const char* const kTimestampFormat = "yyyy-MM-dd HH:mm:ss.zzz";

LogLevel levelForMsgType(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return LogLevel::Debug;
    case QtInfoMsg:
        return LogLevel::Info;
    case QtWarningMsg:
        return LogLevel::Warning;
    case QtCriticalMsg:
    case QtFatalMsg:
        return LogLevel::Error;
    }
    return LogLevel::Info;
}

bool passesFilter(LogLevel level, LogLevel minLevel)
{
    // LogLevel::Log is the "always visible" user level.
    return level == LogLevel::Log || static_cast<int>(level) >= static_cast<int>(minLevel);
}

} // namespace

// ---------------------------------------------------------------------------------------
// Construction / singleton
// ---------------------------------------------------------------------------------------

SystemLogViewer::SystemLogViewer(QWidget* parent)
    : QWidget(parent)
{
    setupUi();

    QMutexLocker locker(&s_instanceMutex);
    if (!s_instance) {
        s_instance = this;
        flushPendingLocked(this);
    }
}

SystemLogViewer::~SystemLogViewer()
{
    QMutexLocker locker(&s_instanceMutex);
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

SystemLogViewer* SystemLogViewer::instance()
{
    QMutexLocker locker(&s_instanceMutex);
    return s_instance;
}

void SystemLogViewer::setInstance(SystemLogViewer* viewer)
{
    QMutexLocker locker(&s_instanceMutex);
    s_instance = viewer;
    if (viewer) {
        flushPendingLocked(viewer);
    }
}

void SystemLogViewer::flushPendingLocked(SystemLogViewer* target)
{
    const QList<Pending> pending = std::exchange(s_pending, {});
    for (const Pending& e : pending) {
        QMetaObject::invokeMethod(
            target, [target, e]() { target->appendEntry(e.timestamp, e.level, e.category, e.message); },
            Qt::QueuedConnection);
    }
}

void SystemLogViewer::installMessageHandler()
{
    if (s_handlerInstalled) {
        return;
    }
    s_handlerInstalled = true;

    // A capture-less lambda converts to QtMessageHandler; being defined inside a member
    // function it may access the private statics (s_instance / s_instanceMutex).
    s_previousHandler = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& context,
                                                  const QString& message) {
        const LogLevel level = levelForMsgType(type);
        QString category;
        if (context.category && qstrcmp(context.category, "default") != 0) {
            category = QString::fromLatin1(context.category);
        }

        {
            // Hold the lock while posting so the instance cannot be destroyed in between; the
            // queued event is discarded by ~QObject if the viewer dies before it is delivered.
            QMutexLocker locker(&s_instanceMutex);
            if (s_instance) {
                SystemLogViewer* target = s_instance;
                const QString text = message;
                QMetaObject::invokeMethod(
                    target, [target, level, category, text]() { target->appendCategorised(level, category, text); },
                    Qt::QueuedConnection);
            } else {
                // No viewer yet (start-up) or detached: keep a bounded backlog for the next one.
                s_pending.append({QDateTime::currentDateTime(), level, category, message});
                if (s_pending.size() > kMaxPending) {
                    s_pending.removeFirst();
                }
            }
        }

        if (s_previousHandler) {
            s_previousHandler(type, context, message);
        }
    });
}

// ---------------------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------------------

void SystemLogViewer::setupUi()
{
    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(5, 5, 5, 5);
    mainLayout->setSpacing(5);

    m_textEdit = new QTextEdit(this);
    m_textEdit->setReadOnly(true);
    m_textEdit->setLineWrapMode(QTextEdit::WidgetWidth);
    m_textEdit->setUndoRedoEnabled(false);
    m_textEdit->setAcceptRichText(false);

    QFont font(QStringLiteral("Consolas"), 9);
    font.setStyleHint(QFont::Monospace);
    if (!QFontInfo(font).fixedPitch()) {
        font = QFont(QStringLiteral("Courier New"), 9);
        font.setStyleHint(QFont::Monospace);
    }
    m_textEdit->setFont(font);
    m_textEdit->setStyleSheet(QStringLiteral("QTextEdit {"
                                             "   background-color: #1e1e1e;"
                                             "   color: #d4d4d4;"
                                             "   border: 1px solid #3c3c3c;"
                                             "}"));
    mainLayout->addWidget(m_textEdit, 1);

    auto* buttonLayout = new QHBoxLayout();
    buttonLayout->setSpacing(6);

    m_filterLabel = new QLabel(this);
    buttonLayout->addWidget(m_filterLabel);

    m_levelFilter = new QComboBox(this);
    m_levelFilter->addItem(QString(), static_cast<int>(LogLevel::Debug));
    m_levelFilter->addItem(QString(), static_cast<int>(LogLevel::Info));
    m_levelFilter->addItem(QString(), static_cast<int>(LogLevel::Warning));
    m_levelFilter->addItem(QString(), static_cast<int>(LogLevel::Error));
    m_levelFilter->setCurrentIndex(0);
    m_levelFilter->setMinimumWidth(110);
    connect(m_levelFilter, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0) {
            return;
        }
        const auto level = static_cast<LogLevel>(m_levelFilter->itemData(index).toInt());
        if (level == m_minLevel) {
            return;
        }
        m_minLevel = level;
        rebuildView();
    });
    buttonLayout->addWidget(m_levelFilter);
    buttonLayout->addStretch();

    m_copyButton = new QPushButton(this);
    m_copyButton->setMinimumWidth(90);
    connect(m_copyButton, &QPushButton::clicked, this, &SystemLogViewer::copyAll);
    buttonLayout->addWidget(m_copyButton);

    m_saveButton = new QPushButton(this);
    m_saveButton->setMinimumWidth(90);
    connect(m_saveButton, &QPushButton::clicked, this, &SystemLogViewer::saveToFile);
    buttonLayout->addWidget(m_saveButton);

    m_clearButton = new QPushButton(this);
    m_clearButton->setMinimumWidth(90);
    connect(m_clearButton, &QPushButton::clicked, this, &SystemLogViewer::clearLogs);
    buttonLayout->addWidget(m_clearButton);

    mainLayout->addLayout(buttonLayout);
    setLayout(mainLayout);

    retranslate();
}

void SystemLogViewer::retranslate()
{
    if (m_filterLabel) {
        m_filterLabel->setText(tr("Level:"));
    }
    if (m_levelFilter && m_levelFilter->count() == 4) {
        m_levelFilter->setItemText(0, tr("All"));
        m_levelFilter->setItemText(1, tr("Info+"));
        m_levelFilter->setItemText(2, tr("Warning+"));
        m_levelFilter->setItemText(3, tr("Error"));
        m_levelFilter->setToolTip(tr("Hide messages below this level"));
    }
    if (m_copyButton) {
        m_copyButton->setText(tr("Copy All"));
        m_copyButton->setToolTip(tr("Copy the visible log to the clipboard"));
    }
    if (m_saveButton) {
        m_saveButton->setText(tr("Save..."));
        m_saveButton->setToolTip(tr("Save the visible log as a text file"));
    }
    if (m_clearButton) {
        m_clearButton->setText(tr("Clear"));
        m_clearButton->setToolTip(tr("Discard all messages"));
    }
}

void SystemLogViewer::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        retranslate();
    }
    QWidget::changeEvent(event);
}

// ---------------------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------------------

int SystemLogViewer::maxLines() const
{
    return m_maxLines;
}

void SystemLogViewer::setMaxLines(int lines)
{
    QMutexLocker locker(&m_mutex);
    m_maxLines = qMax(1, lines);
    while (m_entries.size() > m_maxLines) {
        m_entries.removeFirst();
    }
    trimDocument();
}

void SystemLogViewer::trimDocument()
{
    if (!m_textEdit) {
        return;
    }
    QTextDocument* doc = m_textEdit->document();
    const int excess = doc->blockCount() - m_maxLines;
    if (excess <= 0) {
        return;
    }
    QTextCursor cursor(doc);
    cursor.movePosition(QTextCursor::Start);
    cursor.movePosition(QTextCursor::NextBlock, QTextCursor::KeepAnchor, excess);
    cursor.removeSelectedText();
}

// ---------------------------------------------------------------------------------------
// Appending
// ---------------------------------------------------------------------------------------

void SystemLogViewer::appendLog(LogLevel level, const QString& message)
{
    appendCategorised(level, QString(), message);
}

void SystemLogViewer::appendCategorised(LogLevel level, const QString& category, const QString& message)
{
    appendEntry(QDateTime::currentDateTime(), level, category, message);
}

void SystemLogViewer::appendEntry(const QDateTime& timestamp, LogLevel level, const QString& category,
                                  const QString& message)
{
    QMutexLocker locker(&m_mutex);
    if (!m_textEdit) {
        return;
    }

    Entry entry;
    entry.timestamp = timestamp;
    entry.level = level;
    entry.category = category;
    entry.message = message;

    m_entries.append(entry);
    while (m_entries.size() > m_maxLines) {
        m_entries.removeFirst();
    }

    if (!passesFilter(level, m_minLevel)) {
        return;
    }

    QScrollBar* scrollBar = m_textEdit->verticalScrollBar();
    const bool autoScroll = scrollBar->value() >= scrollBar->maximum();

    m_textEdit->append(formatEntry(entry));
    trimDocument();

    if (autoScroll) {
        scrollBar->setValue(scrollBar->maximum());
    }
}

void SystemLogViewer::rebuildView()
{
    QMutexLocker locker(&m_mutex);
    if (!m_textEdit) {
        return;
    }
    m_textEdit->clear();
    for (const Entry& entry : std::as_const(m_entries)) {
        if (passesFilter(entry.level, m_minLevel)) {
            m_textEdit->append(formatEntry(entry));
        }
    }
    QScrollBar* scrollBar = m_textEdit->verticalScrollBar();
    scrollBar->setValue(scrollBar->maximum());
}

void SystemLogViewer::clearLogs()
{
    QMutexLocker locker(&m_mutex);
    m_entries.clear();
    if (m_textEdit) {
        m_textEdit->clear();
    }
}

void SystemLogViewer::copyAll()
{
    if (!m_textEdit) {
        return;
    }
    QApplication::clipboard()->setText(m_textEdit->toPlainText());
}

void SystemLogViewer::saveToFile()
{
    if (!m_textEdit) {
        return;
    }
    const QString dir = AppSettings::instance().logDirectory();
    QDir().mkpath(dir);
    const QString suggested = QDir(dir).filePath(QStringLiteral("system_log_%1.txt").arg(
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"))));

    const QString path = QFileDialog::getSaveFileName(this, tr("Save System Log"), suggested,
                                                      tr("Text files (*.txt);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QMessageBox::warning(this, tr("Save System Log"),
                             tr("Cannot write %1:\n%2").arg(QDir::toNativeSeparators(path), file.errorString()));
        return;
    }
    QString text = m_textEdit->toPlainText();
    if (!text.endsWith(QLatin1Char('\n'))) {
        text.append(QLatin1Char('\n'));
    }
    file.write(text.toUtf8());
    file.close();
}

// ---------------------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------------------

QString SystemLogViewer::formatEntry(const Entry& entry) const
{
    return QStringLiteral("<span style='color: #808080;'>%1</span> %2")
        .arg(entry.timestamp.toString(QLatin1String(kTimestampFormat)),
             formatMessage(entry.level, entry.category, entry.message));
}

QString SystemLogViewer::formatMessage(LogLevel level, const QString& category, const QString& message) const
{
    QString body = message.toHtmlEscaped();
    body.replace(QStringLiteral("\n"), QStringLiteral("<br/>"));
    body.replace(QStringLiteral("  "), QStringLiteral("&nbsp; "));

    QString categoryHtml;
    if (!category.isEmpty()) {
        categoryHtml = QStringLiteral("<span style='color: #9cdcfe;'>[%1]</span> ").arg(category.toHtmlEscaped());
    }

    const QString messageColor = (level == LogLevel::Error) ? QStringLiteral("#f48771") : QStringLiteral("#d4d4d4");

    return QStringLiteral("<span style='color: %1; font-weight: bold;'>[%2]</span> "
                          "%3"
                          "<span style='color: %4;'>%5</span>")
        .arg(levelColor(level), levelText(level), categoryHtml, messageColor, body);
}

QString SystemLogViewer::levelColor(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:
        return QStringLiteral("#569cd6");   // light blue
    case LogLevel::Info:
        return QStringLiteral("#4ec9b0");   // cyan
    case LogLevel::Warning:
        return QStringLiteral("#dcdcaa");   // yellow
    case LogLevel::Error:
        return QStringLiteral("#f48771");   // red
    case LogLevel::Log:
        return QStringLiteral("#21f732");   // green
    }
    return QStringLiteral("#d4d4d4");
}

QString SystemLogViewer::levelText(LogLevel level)
{
    switch (level) {
    case LogLevel::Debug:
        return QStringLiteral("DEBUG");
    case LogLevel::Info:
        return QStringLiteral("INFO ");
    case LogLevel::Warning:
        return QStringLiteral("WARN ");
    case LogLevel::Error:
        return QStringLiteral("ERROR");
    case LogLevel::Log:
        return QStringLiteral("LOG  ");
    }
    return QStringLiteral("LOG  ");
}
