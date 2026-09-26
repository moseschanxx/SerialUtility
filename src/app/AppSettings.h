#pragma once

#include <QObject>
#include <QFont>
#include <QString>
#include <QStringList>
#include <QByteArray>

#include "core/LineEnding.h"
#include "core/SerialConnection.h"

/**
 * Typed façade over QSettings for every user preference of the application.
 *
 * - Process-wide singleton (AppSettings::instance()); QSettings is configured in main()
 *   via QCoreApplication::setOrganizationName(APP_ORGANIZATION) / setApplicationName(APP_NAME),
 *   so this class simply uses the default QSettings() constructor.
 * - Every setter writes through immediately and emits changed(). Widgets that mirror a
 *   preference (TerminalWidget font, SessionWidget line endings, ...) listen to changed()
 *   or are re-applied explicitly by SessionWidget::applyPreferences().
 * - Getters return the documented default when the key is absent.
 *
 * Key names (for reference / migration):  general/language, terminal/font, terminal/theme,
 * terminal/scrollback, terminal/cursorBlink, terminal/bell, terminal/implicitCr,
 * terminal/pauseWhileSelecting, terminal/rightClickPastes,
 * input/enterSends, input/backspaceSendsDelete, input/localEcho, input/encoding,
 * connection/default (QVariantMap of SerialSettings), connection/autoReconnect,
 * connection/reconnectIntervalMs, connection/lastPort, connection/showSimulatedPorts,
 * logging/directory, logging/autoLog, logging/format, logging/includeTx, session/confirmClose,
 * session/restoreLastPorts, window/geometry, window/state.
 */
class AppSettings : public QObject
{
    Q_OBJECT
public:
    static AppSettings& instance();

    // ---- General --------------------------------------------------------------------
    QString language() const;                 ///< "en_US" | "zh_CN"; default: derived from QLocale::system()
    void setLanguage(const QString& code);

    // ---- Terminal appearance --------------------------------------------------------
    QFont terminalFont() const;               ///< default: defaultTerminalFont()
    void setTerminalFont(const QFont& font);
    QString themeName() const;                ///< TerminalTheme name; default "dark"
    void setThemeName(const QString& name);
    int scrollbackLines() const;              ///< default 10000, clamp 100..1000000
    void setScrollbackLines(int lines);
    bool cursorBlink() const;                 ///< default true
    void setCursorBlink(bool on);
    bool bellEnabled() const;                 ///< audible/visual bell on BEL; default true
    void setBellEnabled(bool on);
    bool implicitCr() const;                  ///< treat LF as CR+LF (bare-\n MCU output); default true
    void setImplicitCr(bool on);
    /// Freeze the terminal display while text is selected (cmd.exe mark mode): incoming bytes
    /// queue up, Enter copies the selection and resumes, Esc cancels. Mirrored by the View menu
    /// action and the Preferences checkbox (TerminalWidget::setPauseWhileSelecting). Default true.
    bool pauseWhileSelecting() const;
    void setPauseWhileSelecting(bool on);
    /// cmd.exe QuickEdit-style right click in the terminal: a plain right click pastes the
    /// clipboard, or copies the selection when text is selected; the context menu moves to
    /// Shift+right click (and the Menu key). Mirrored by the View menu action and the Preferences
    /// checkbox (TerminalWidget::setRightClickPastes). Default true.
    bool rightClickPastes() const;
    void setRightClickPastes(bool on);

    // ---- Input behaviour ------------------------------------------------------------
    LineEnding::Mode enterSends() const;      ///< bytes sent for Enter in the terminal; default CR
    void setEnterSends(LineEnding::Mode mode);
    bool backspaceSendsDelete() const;        ///< true -> 0x7F, false -> 0x08; default true
    void setBackspaceSendsDelete(bool on);
    bool localEcho() const;                   ///< echo typed bytes locally; default false
    void setLocalEcho(bool on);
    QString encoding() const;                 ///< QStringConverter codec name; default "UTF-8"
    void setEncoding(const QString& name);

    // ---- Connection -----------------------------------------------------------------
    SerialSettings defaultSerialSettings() const;   ///< default: 115200 8N1, no flow, DTR/RTS on
    void setDefaultSerialSettings(const SerialSettings& settings);
    bool autoReconnect() const;               ///< default true
    void setAutoReconnect(bool on);
    int reconnectIntervalMs() const;          ///< default 1000, clamp 200..60000
    void setReconnectIntervalMs(int ms);
    QString lastPortName() const;             ///< port pre-selected in a new session
    void setLastPortName(const QString& name);
    /// List the built-in simulated devices ("SIM:loopback", "SIM:linux", "SIM:uboot", "SIM:mcu";
    /// see core/DeviceSimulator.h) in the port list. Default true; off for production use.
    bool showSimulatedPorts() const;
    void setShowSimulatedPorts(bool on);

    // ---- Logging --------------------------------------------------------------------
    QString logDirectory() const;             ///< default: defaultLogDirectory()
    void setLogDirectory(const QString& dir);
    bool autoLog() const;                     ///< start a log automatically on connect; default false
    void setAutoLog(bool on);
    QString logFormat() const;                ///< "raw" | "text" | "hex"; default "text"
    void setLogFormat(const QString& format);
    bool logIncludeTx() const;                ///< default true
    void setLogIncludeTx(bool on);

    // ---- Session / window -----------------------------------------------------------
    bool confirmCloseWhenConnected() const;   ///< default true
    void setConfirmCloseWhenConnected(bool on);
    bool restoreLastPorts() const;            ///< reopen previous session tabs on start; default true
    void setRestoreLastPorts(bool on);
    QStringList lastOpenPorts() const;
    void setLastOpenPorts(const QStringList& ports);
    QByteArray mainWindowGeometry() const;
    void setMainWindowGeometry(const QByteArray& geometry);
    QByteArray mainWindowState() const;
    void setMainWindowState(const QByteArray& state);

    /// Location of per-user data files (quick_commands.json, history.txt):
    /// QStandardPaths::AppConfigLocation, created on demand.
    static QString dataDirectory();

    /// Font used when terminal/font is absent: "Consolas" 10pt on Windows, "Monospace" 10pt
    /// elsewhere (style hint Monospace, fixed pitch).
    static QFont defaultTerminalFont();
    /// Directory used when logging/directory is absent: <Documents>/BuildAI/SerialLogs, or
    /// <home>/BuildAI/SerialLogs when QStandardPaths reports no Documents location. Forward slashes.
    static QString defaultLogDirectory();

    void sync();

signals:
    /// Emitted after any setter. `key` is the settings key that changed.
    void changed(const QString& key);

private:
    explicit AppSettings(QObject* parent = nullptr);
    Q_DISABLE_COPY_MOVE(AppSettings)
};
