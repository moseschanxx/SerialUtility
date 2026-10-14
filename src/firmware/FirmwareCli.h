#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <memory>
#include <optional>

class FirmwareJob;
class QIODevice;
class SshProfileStore;
class ToolRunner;

/**
 * The logic of the `su-fw` console tool (v0.6.0; docs/firmware-update/CLI_PLAN.md sections 3 and
 * 4): the Firmware tab's operations for scripts and AI agents, without a window. UI-free (su_core);
 * src/cli/main.cpp is only the process around it (QCoreApplication, stdout / stderr / stdin, the
 * console's echo, Ctrl+C), so the tests drive the very same code in-process with FakeToolRunner,
 * TestSshServer and FakeMcuServer.
 *
 * run(arguments, out, err): `arguments` is the command line after the program name
 * ("flash-soc --all --json"). One QCommandLineParser per command parses it; the command
 * configures a fresh FirmwareJob (the chain the tab drives: same steps, tools, rules and
 * messages), runs one operation on the event loop until operationFinished() and returns the exit
 * code. Commands: status, partitions, pull, flash-soc, mcu-info, mcu-update, mcu-set-role,
 * pull-flash; helpText() documents every option for an agent to read (`su-fw --help`,
 * `su-fw <command> --help`). The settings are the app's: the SSH profiles and SecretStore, the
 * Preferences (tool paths, SSH defaults, Loader / ADB waits, cache folder, git describe, extra
 * files) and the flash layouts remembered per board (a layout read from a board is stored there
 * too, as the tab does); nothing else is written back.
 *
 * Output (all on `out`; only usage errors go to `err`): every log line of the job, the tab's
 * wording, as "HH:MM:SS text"; upgrade_tool's raw lines except its "(nn%)" progress, which comes
 * as the job's progress() every 10 %; the command's own lines (the board, the partition table,
 * the image summary); last a result line "su-fw: exit <n> (<meaning>): <message>". --json prints
 * one JSON object per line instead: {"time": ISO 8601 with offset, "step": the job's step or the
 * command, "event": "log" | "progress" | "step" | "done", "text", "percent" (progress with a
 * percentage), "ok" (step, done), "exit" (done)}; the "done" object is the last line and carries
 * the command's result fields ("board", "mcu", "layout", "partitions", "image", "boardInfo").
 * --quiet prints only the result line; --log FILE appends every line to FILE as well (also with
 * --quiet).
 *
 * Exit codes (ExitCode): 0 done; 1 the operation failed (the last log line says why); 2 usage;
 * 3 no board, or the board in a mode the command cannot use; 4 SSH: unreachable, host key
 * refused, authentication failed; 5 --timeout or interrupt() (Ctrl+C).
 *
 * Decisions: an unknown host key is accepted (and remembered in known_hosts) only with
 * --accept-host-key, a changed one never without a terminal to ask; a password / passphrase
 * prompt is answered once from --password-env VAR or --password-stdin, else SshConnection's saved
 * SecretStore secret is all there is. On an interactive terminal without --yes su-fw asks instead
 * (questions on `err`, the password read without echo). Secrets are never on the command line and
 * never echoed: not the password, not the name of the variable holding it, not a stray argument.
 * Download mode: a board on ADB is rebooted into Loader mode by the flasher itself (`adb reboot
 * loader`); without a board the MASKROM instructions are logged and the flasher waits (the job's
 * headless default).
 *
 * interrupt() (Ctrl+C) and --timeout cancel the running operation through FirmwareJob::cancel();
 * the job refuses that while upgrade_tool writes the flash (logged), and su-fw then cancels at
 * the next point where it can (cancellableChanged(true)) - the write itself is never cut. A wait
 * outside an operation (the status query, the partition plan) ends at once. Both exit with 5.
 */
class FirmwareCli : public QObject
{
    Q_OBJECT
public:
    enum ExitCode : int {
        ExitOk = 0,
        ExitFailed = 1,      ///< the operation failed; the last log line says why
        ExitUsage = 2,       ///< unknown command / option, a missing or invalid value, an unusable file
        ExitNoBoard = 3,     ///< no board, or the board in a mode the command cannot use
        ExitSsh = 4,         ///< the build machine: unreachable, host key refused, authentication failed
        ExitCancelled = 5    ///< --timeout passed or interrupt() (Ctrl+C)
    };

    /// Reads one line the user typed (stdin): `secret` = without echo on a terminal. std::nullopt =
    /// end of input. Questions are written to `err` before it is called.
    using LineReader = std::function<std::optional<QString>(bool secret)>;

    /// `runner` null = real processes (the job's own ProcessToolRunner); the tests pass a fake.
    explicit FirmwareCli(ToolRunner* runner = nullptr, QObject* parent = nullptr);
    ~FirmwareCli() override;

    /// Runs one command line (without the program name) to its end on the event loop; returns the
    /// exit code. Needs a QCoreApplication. Not re-entrant (a nested call returns ExitUsage).
    int run(const QStringList& arguments, QIODevice* out, QIODevice* err);
    bool isRunning() const;

    // ---- What main.cpp (and the tests) hand in ----------------------------------------------------
    /// stdin: --password-stdin reads its first line; `interactive` (a terminal) allows questions
    /// unless --yes. Default: no input, not interactive.
    void setInput(LineReader reader, bool interactive);
    /// Not owned. Default: a store of the job's own, loaded from SshProfileStore::defaultFilePath()
    /// at every run (and never saved: su-fw does not change the app's profiles).
    void setProfileStore(SshProfileStore* store);
    /// Fixed tool paths instead of --tools / ToolLocator (the tests' "upgrade_tool" / "adb").
    void setToolPaths(const QString& upgradeTool, const QString& adb);
    /// Called with the configured job right before its operation starts (the tests shorten polls
    /// and waits here).
    void setJobSetup(std::function<void(FirmwareJob& job)> setup);

    // ---- The command line ---------------------------------------------------------------------------
    static QStringList commands();                            ///< "status", "partitions", ... "pull-flash"
    /// Every option `command` accepts, without the dashes (the common ones included).
    static QStringList commandOptions(const QString& command);
    /// `su-fw --help` (empty `command`) or `su-fw <command> --help`.
    static QString helpText(const QString& command = QString());
    static QString versionText();                             ///< "su-fw 0.6.0 (BuildAI Serial Utility, <git hash>)"

public slots:
    /// Ctrl+C / SIGINT: cancels the running operation (see the class comment); a no-op while idle.
    void interrupt();

private:
    struct Private;
    std::unique_ptr<Private> d;
};
