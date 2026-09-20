#pragma once

#include <QWidget>
#include <QTextEdit>
#include <QPushButton>
#include <QComboBox>
#include <QMutex>
#include <QMetaType>
#include <QDateTime>
#include <QList>

class QLabel;

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
    Log = 4
};
Q_DECLARE_METATYPE(LogLevel)

/**
 * Console-like viewer for the application's own diagnostics, hosted in a QDockWidget by
 * MainWindow ("View > System Log"). Same look as the Vispek reference tool: dark
 * background, monospace, colour-coded level tags, timestamps.
 *
 * - installMessageHandler() installs a qInstallMessageHandler hook that forwards every Qt
 *   message (with its category) to the current instance via a queued invocation (safe
 *   from any thread) and then calls the previous handler so messages still reach the
 *   debugger / stderr. QtDebugMsg -> Debug, QtInfoMsg -> Info, QtWarningMsg -> Warning,
 *   QtCriticalMsg/QtFatalMsg -> Error.
 * - A level filter combo (All / Info+ / Warning+ / Error) hides lower levels.
 * - Bounded to maxLines() (default 2000) entries.
 * - Buttons: Clear, Copy All, Save... (plain text).
 */
class SystemLogViewer : public QWidget
{
    Q_OBJECT
public:
    explicit SystemLogViewer(QWidget* parent = nullptr);
    ~SystemLogViewer() override;

    static SystemLogViewer* instance();
    static void setInstance(SystemLogViewer* viewer);
    /// Route Qt messages to the instance (call once from main() after creating the window).
    static void installMessageHandler();

    int maxLines() const;
    void setMaxLines(int lines);

public slots:
    void appendLog(LogLevel level, const QString& message);
    void appendCategorised(LogLevel level, const QString& category, const QString& message);
    void clearLogs();
    void copyAll();
    void saveToFile();

private:
    void setupUi();
    QString formatMessage(LogLevel level, const QString& category, const QString& message) const;
    static QString levelColor(LogLevel level);
    static QString levelText(LogLevel level);
    void retranslate();
    void changeEvent(QEvent* event) override;

    QTextEdit* m_textEdit = nullptr;
    QComboBox* m_levelFilter = nullptr;
    QPushButton* m_clearButton = nullptr;
    QPushButton* m_copyButton = nullptr;
    QPushButton* m_saveButton = nullptr;
    int m_maxLines = 2000;
    LogLevel m_minLevel = LogLevel::Debug;
    QMutex m_mutex;

    // ---- Implementation state (private; added by the ui-main package) ------------------
    struct Entry
    {
        QDateTime timestamp;
        LogLevel level = LogLevel::Info;
        QString category;
        QString message;
    };
    QString formatEntry(const Entry& entry) const;   ///< formatMessage() with the entry's own timestamp
    void rebuildView();                              ///< re-render m_entries through the level filter
    void trimDocument();                             ///< drop leading blocks beyond m_maxLines
    QList<Entry> m_entries;                          ///< retained so the filter can be changed after the fact
    QLabel* m_filterLabel = nullptr;

    static SystemLogViewer* s_instance;
    static QMutex s_instanceMutex;
};
