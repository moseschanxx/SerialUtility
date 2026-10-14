#include "firmware/FirmwareCli.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDateTime>
#include <QDeadlineTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QPair>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTime>
#include <QTimer>

#include <utility>

#include "Version.h"
#include "app/AppSettings.h"
#include "firmware/CcPackage.h"
#include "firmware/FirmwareJob.h"
#include "firmware/ImagePuller.h"
#include "firmware/RockchipTool.h"
#include "firmware/SocFlasher.h"
#include "firmware/ToolRunner.h"
#include "ssh/SshConnection.h"
#include "ssh/SshProfile.h"

namespace {

// ---- Commands --------------------------------------------------------------------------------
constexpr QLatin1StringView kStatus{"status"};
constexpr QLatin1StringView kPartitions{"partitions"};
constexpr QLatin1StringView kPull{"pull"};
constexpr QLatin1StringView kFlashSoc{"flash-soc"};
constexpr QLatin1StringView kMcuInfo{"mcu-info"};
constexpr QLatin1StringView kMcuUpdate{"mcu-update"};
constexpr QLatin1StringView kMcuSetRole{"mcu-set-role"};
constexpr QLatin1StringView kPullFlash{"pull-flash"};

// ---- Waits outside an operation (an operation itself waits for operationFinished()) -----------
constexpr int kPollMs = 20;
constexpr int kStatusWaitMs = 60000;       ///< `upgrade_tool LD` and `adb devices` time out after 15 s each
constexpr int kLayoutReadWaitMs = 20000;   ///< `adb shell cat /proc/cmdline` (15 s adb timeout)
constexpr int kPlanWaitMs = 300000;        ///< `upgrade_tool EXF` of an image takes seconds; a slow disk more
constexpr int kMaxTimeoutSeconds = 7 * 24 * 3600;   ///< --timeout in ms must fit QTimer's int

/// The value an option takes, for the parser and the help ("" = a switch).
QString optionValueName(const QString& option)
{
    static const QList<QPair<QLatin1String, QLatin1String>> values = {
        {QLatin1String("tools"), QLatin1String("DIR")},           {QLatin1String("log"), QLatin1String("FILE")},
        {QLatin1String("timeout"), QLatin1String("SECONDS")},     {QLatin1String("serial"), QLatin1String("SERIAL")},
        {QLatin1String("image"), QLatin1String("FILE")},          {QLatin1String("cache"), QLatin1String("DIR")},
        {QLatin1String("layout"), QLatin1String("MTDPARTS")},     {QLatin1String("target"), QLatin1String("TARGET")},
        {QLatin1String("remote-folder"), QLatin1String("FOLDER")}, {QLatin1String("identity"), QLatin1String("FILE")},
        {QLatin1String("password-env"), QLatin1String("VAR")},    {QLatin1String("partitions"), QLatin1String("NAMES")},
        {QLatin1String("package"), QLatin1String("FILE")},        {QLatin1String("role"), QLatin1String("ROLE")},
    };
    for (const auto& entry : values) {
        if (option == entry.first) {
            return entry.second;
        }
    }
    return QString();
}

const QStringList& commonOptionNames()
{
    static const QStringList names = {QStringLiteral("tools"), QStringLiteral("log"), QStringLiteral("json"),
                                      QStringLiteral("quiet"), QStringLiteral("timeout"), QStringLiteral("yes")};
    return names;
}

QStringList pullOptionNames()
{
    return {QStringLiteral("target"), QStringLiteral("remote-folder"), QStringLiteral("cache"), QStringLiteral("identity"),
            QStringLiteral("password-env"), QStringLiteral("password-stdin"), QStringLiteral("accept-host-key")};
}

QStringList specificOptionNames(const QString& command)
{
    if (command == kStatus) {
        return {QStringLiteral("serial"), QStringLiteral("mcu")};
    }
    if (command == kPartitions) {
        return {QStringLiteral("image"), QStringLiteral("cache"), QStringLiteral("layout"), QStringLiteral("serial")};
    }
    if (command == kPull) {
        return pullOptionNames();
    }
    if (command == kFlashSoc) {
        return {QStringLiteral("image"), QStringLiteral("cache"), QStringLiteral("all"), QStringLiteral("partitions"),
                QStringLiteral("serial"), QStringLiteral("layout"), QStringLiteral("no-wait")};
    }
    if (command == kMcuInfo) {
        return {QStringLiteral("serial")};
    }
    if (command == kMcuUpdate) {
        return {QStringLiteral("package"), QStringLiteral("role"), QStringLiteral("serial")};
    }
    if (command == kMcuSetRole) {
        return {QStringLiteral("role"), QStringLiteral("serial")};
    }
    if (command == kPullFlash) {
        return pullOptionNames()
               + QStringList{QStringLiteral("package"), QStringLiteral("role"), QStringLiteral("serial"),
                             QStringLiteral("layout"), QStringLiteral("all"), QStringLiteral("partitions")};
    }
    return {};
}

// ---- Help (English only: read by people in a shell and by agents) ------------------------------

const char* const kCommonOptionsHelp = R"HELP(  --tools DIR          Use upgrade_tool and adb from DIR. Default: the paths set in the app's
                       Preferences > Firmware, else tools/ next to su-fw (the bundled copies),
                       else - adb only - the first adb on PATH.
  --log FILE           Append every output line to FILE as well (also with --quiet).
  --json               One JSON object per line instead of text (see Output in su-fw --help).
  --quiet              Print only the result line.
  --timeout SECONDS    Give up after SECONDS: the operation is cancelled at the next point where
                       that is safe and su-fw exits with 5. Not a hard limit: a flash write in
                       progress is always waited for, and an upgrade_tool that stays silent is
                       never killed (the log says so every 5 min; end it by hand). A harness that
                       kills the su-fw process tree on a timeout of its own still cuts the write:
                       give it more than SECONDS plus the longest write (about two minutes for a
                       full image). Default 0: no limit.
  --yes                Never ask anything: an unknown host key or a password prompt is refused
                       (exit 4). This is also what happens when stdin is not a terminal.
  -h, --help           This help.
)HELP";

const char* const kOutputHelp = R"HELP(Output:
  Text (stdout): one line per event, "HH:MM:SS text" - the Firmware tab's log lines (every
  command line run, the tools' answers, the steps), progress every 10 %, the command's own lines
  - and last the result line "su-fw: exit <code> (<meaning>): <message>".
  --json: one object per line, e.g. {"time":"2026-10-13T18:42:01.123+08:00","step":"flash",
  "event":"log","text":"..."}. "event" is "log" (a log line), "progress" (with "percent" when
  there is one), "step" (a step ended; "ok") or "done": the last line, with "ok", "exit", "text"
  and the command's results ("board", "layout", "mcu", "partitions", "image", "boardInfo").
  A usage error (exit 2) goes to stderr; found before anything ran, it leaves stdout empty.
)HELP";

const char* const kExitCodesHelp = R"HELP(Exit codes:
  0  done
  1  the operation failed; the last log lines say why (also when another firmware operation
     runs on this PC: "another firmware operation is running (<program>, pid <n>)")
  2  usage: an unknown command or option, a missing or invalid value, a file that cannot be used
  3  no board, or the board in a mode the command cannot use (e.g. mcu-info in Loader mode)
  4  SSH: the build machine could not be reached, its host key was refused, or the
     authentication failed
  5  --timeout passed, or Ctrl+C (also SIGTERM, SIGHUP, Ctrl+Break). It ends the waiting phases,
     never a flash write in progress: su-fw then stops at the next safe point - when that is the
     wait for ADB the result says "interrupted after the image was written; the ADB wait was
     cancelled". The tools run without su-fw's console (Windows) or in a process group of
     their own (Linux): no keystroke, closed window or signal meant for su-fw reaches them.
)HELP";

const char* const kGlobalHelp = R"HELP(su-fw - flash the BuildAI Node board (RV1106 SoC + CC1310 RF MCU) from a command shell

Usage:
  su-fw <command> [options]
  su-fw <command> --help       every option of one command, its exit codes and examples
  su-fw --help | --version

su-fw runs the operations of the Serial Utility's Firmware tab without a window - the same
steps, the same tools (upgrade_tool, adb, the CC1310 updater) and the same messages - for people
in a shell and for scripts and AI agents. The log goes to stdout, the result is the exit code.
It uses the app's settings: the SSH profiles and remembered passwords, Preferences > Firmware
(tool paths, cache folder, waits) and the flash layouts remembered per board. One firmware
operation runs at a time on a PC: while the app's Firmware tab or another su-fw pulls, flashes or
talks to the CC1310, su-fw refuses (exit 1) and status pauses, and the other way round.

Commands:
  status        The board the PC sees (ADB / Loader / Maskrom / no board); --mcu adds the
                CC1310's firmware version and role.
  partitions    The partitions of an update.img in the flash layout - name, file, offset,
                partition size, image size, fits - to choose before flashing.
  pull          Download update.img from the build machine over SSH (skipped when unchanged;
                the previous image is kept).
  flash-soc     Flash the SoC: in Loader mode the chosen partitions (--all or --partitions), in
                MASKROM mode the whole image. A running board is put into Loader mode over ADB.
  mcu-info      Read the running CC1310's firmware version and role through the board.
  mcu-update    Update the CC1310 with a .pkg package (--role rx|tx).
  mcu-set-role  Set the CC1310's role (rx or tx) without updating it.
  pull-flash    Pull, flash every partition, wait for the board, then update the CC1310 when a
                package is given and its version differs from the running one.

Options of every command:
)HELP";

const char* const kGlobalHelpTail = R"HELP(
Secrets are never taken from the command line: use --password-env VAR, --password-stdin,
--identity FILE, or the password a profile saved in the app ("Remember").

Examples:
  su-fw status --mcu
  su-fw partitions --image update.img
  su-fw pull-flash --target builder@10.0.10.71 --password-env SU_PASSWORD --package firmware-0.8.1.pkg --json
)HELP";

const char* const kStatusHelp = R"HELP(su-fw status - the board the PC sees

Usage: su-fw status [--serial SERIAL] [--mcu] [options]

Asks upgrade_tool (Loader / Maskrom mode) and adb (a running board), like the Firmware tab's
status line: "ADB device <serial>", "Loader mode (...)", "Maskrom mode (...)", "no board", or the
hint to install the Rockchip USB drivers. For a running board the flash layout is shown too: it
is read from the board once and remembered, as the tab does.

Options:
  --serial SERIAL      The ADB board to use when several are attached (adb devices lists them).
  --mcu                Also read the CC1310's firmware version and role (the board must be
                       running: ADB).
)HELP";

const char* const kStatusTail = R"HELP(
Exit codes:
  0  a board was found (and with --mcu the CC1310 answered)
  1  --mcu: the CC1310 did not answer, or reading it failed
  2  usage
  3  no board (or a board without a driver), or --mcu with a board that is not running on ADB
  5  --timeout or Ctrl+C

Examples:
  su-fw status
  su-fw status --mcu --json
)HELP";

const char* const kPartitionsHelp = R"HELP(su-fw partitions - what flash-soc would write

Usage: su-fw partitions [--image FILE] [--layout MTDPARTS | --serial SERIAL] [options]

Unpacks the image (upgrade_tool EXF, into <image folder>/<image name>.extracted) and lists one
row per partition, as the Firmware tab's table: name, image file, flash offset, partition size,
image size, whether it fits - and why a row cannot be written. The layout is the image's own when
its env.img carries one; else the board's: --layout, the layout remembered for --serial, else the
one read last from a board (su-fw status reads it from a running board). The board is not touched.

Options:
  --image FILE         The update.img to list. Default: the cached image of the last pull.
  --cache DIR          The cache folder that holds that image. Default: the app's
                       (Preferences > Firmware).
  --layout MTDPARTS    The board's flash layout, e.g. "sfc_nor:64K(env),256K@64K(idblock),...".
  --serial SERIAL      Use the layout remembered for the board with this ADB serial.
)HELP";

const char* const kPartitionsTail = R"HELP(
Exit codes:
  0  listed (a row that cannot be written says why)
  1  the image could not be unpacked (upgrade_tool missing or failing)
  2  usage: no image, a file that is not a Rockchip RKFW image, a layout that does not parse
  5  --timeout or Ctrl+C

Examples:
  su-fw partitions --image update.img
  su-fw partitions --image update.img --layout "sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),3M(boot),5M(rootfs),3M(userdata)" --json
)HELP";

const char* const kPullOptionsHelp = R"HELP(  --target TARGET      The build machine: the name of one of the app's SSH profiles, or
                       user@host[:port]. Default: the Firmware tab's last build machine.
  --remote-folder FOLDER
                       The folder on the build machine that holds update.img. Default: the
                       folder used last for this target, else ~/rk/rockchip-platforms/output/image.
  --cache DIR          The local cache folder (update.img, update.prev.img). Default: the app's
                       (Preferences > Firmware).
  --identity FILE      The private key for a user@host target (a profile keeps its own key).
  --password-env VAR   Answer the password (or key passphrase) prompt with the value of the
                       environment variable VAR.
  --password-stdin     Answer it with the first line read from stdin.
  --accept-host-key    Trust a host key that is not known yet and remember it in known_hosts
                       (like ssh -o StrictHostKeyChecking=accept-new). A changed key is always
                       refused (remove the old line from known_hosts if the machine was rebuilt).
)HELP";

const char* const kPullHelp = R"HELP(su-fw pull - download update.img from the build machine

Usage: su-fw pull [--target TARGET] [--remote-folder FOLDER] [--cache DIR] [--identity FILE]
                  [--password-env VAR | --password-stdin] [--accept-host-key] [options]

Connects over SSH (the host key is checked against known_hosts, as the app does), shows the
remote update.img (size, build time, git describe of the SDK), skips the download when the cached
copy has the same size and time, else downloads it to a .part file, checks the RKFW magic and
keeps the previous image as update.prev.img. Prints the image summary.
Without a password option the password the app remembered for the profile / target is used; a
prompt that nobody can answer ends the pull with exit code 4.

Options:
)HELP";

const char* const kPullTail = R"HELP(
Exit codes:
  0  pulled, or unchanged
  1  the pull failed after connecting: no update.img in the folder, not an RKFW image, the
     transfer failed, the cache cannot be written
  2  usage: no or an unknown target, a missing key file, an empty password variable
  4  SSH: the build machine could not be reached, its host key was refused, or the
     authentication failed
  5  --timeout or Ctrl+C

Examples:
  su-fw pull --target "Build machine"
  su-fw pull --target builder@10.0.10.71 --password-env SU_PASSWORD --accept-host-key --json
)HELP";

const char* const kFlashSocHelp = R"HELP(su-fw flash-soc - flash the RV1106 SoC

Usage: su-fw flash-soc [--image FILE] (--all | --partitions NAMES) [--serial SERIAL]
                       [--layout MTDPARTS] [--no-wait] [options]

Finds the board: already in Loader / Maskrom mode, or running on ADB (its /proc/version and
flash layout are read, then "adb reboot loader" - no question asked); with no board at all it
logs how to enter MASKROM mode and waits (30 s, Preferences > Firmware). Then it writes:
  Loader mode:  the chosen partitions, one "upgrade_tool WL <offset> <size> <file>" each (every
                image is checked against its partition first), then RD restarts the board.
  MASKROM mode: the whole image with "upgrade_tool UF" (a partition choice does not apply; the
                log says so).
Last it waits for the board to come back on ADB and prints its kernel version and uptime.
Ctrl+C and --timeout cancel the waiting, never the write.

Options:
  --image FILE         The Rockchip update.img. Default: the cached image of the last pull.
  --cache DIR          The cache folder that holds that image. Default: the app's.
  --all                Every partition of the image. Refused before anything is written when an
                       image does not fit its partition.
  --partitions NAMES   Exactly these partitions, comma separated, e.g. boot,rootfs (su-fw
                       partitions lists them). An unknown name or a partition that cannot be
                       written is refused before anything is written.
  --serial SERIAL      The ADB board to use when several are attached.
  --layout MTDPARTS    The board's flash layout: only needed for a board already in Loader mode
                       whose layout is not remembered, flashed with an image without one of its
                       own (the image's layout always wins).
  --no-wait            Do not wait for the board to come back on ADB after the flash.
)HELP";

const char* const kFlashSocTail = R"HELP(
Exit codes:
  0  flashed (also when the board did not come back on ADB in time: the log warns)
  1  the flash failed: upgrade_tool's error (its last lines are in the log), an image that does
     not fit, the layout unknown in Loader mode
  2  usage: neither or both of --all / --partitions, an unknown partition, no image
  3  no board entered Loader / Maskrom mode in time
  5  --timeout or Ctrl+C while waiting

Examples:
  su-fw flash-soc --image update.img --all
  su-fw flash-soc --partitions boot,rootfs --json
)HELP";

const char* const kMcuInfoHelp = R"HELP(su-fw mcu-info - the CC1310's firmware version and role

Usage: su-fw mcu-info [--serial SERIAL] [options]

Through the running board: a stale rfmcu is stopped first, then "adb shell rfmcu cmd version"
(and "help" when the firmware does not report its role). Needs the board running on ADB with
rfmcu (rockchip-platforms v1.2.0 or later).

Options:
  --serial SERIAL      The ADB board to use when several are attached.
)HELP";

const char* const kMcuInfoTail = R"HELP(
Exit codes:
  0  read: "MCU application version=<version> role=<rx|tx|unknown>"
  1  the CC1310 did not answer (it may sit in its UART updater), its port is busy, rfmcu failed
  2  usage
  3  no board running on ADB (none, or one in Loader / Maskrom mode)
  5  --timeout or Ctrl+C

Examples:
  su-fw mcu-info
  su-fw mcu-info --serial 0225ce3c8d8d6e44 --json
)HELP";

const char* const kMcuUpdateHelp = R"HELP(su-fw mcu-update - update the CC1310's firmware

Usage: su-fw mcu-update --package FILE --role rx|tx [--serial SERIAL] [options]

Checks the package (header, CRCs), opens the link through the board (adb forward to "rfmcu
bridge"), reboots the CC1310 into its UART updater, sends the package (progress per acknowledged
block), waits for the restart banner and takes the link down again.

Options:
  --package FILE       The CC1310 package (firmware-x.y.z.pkg).
  --role rx|tx         The role the CC1310 runs with afterwards. Required: the role is never
                       guessed (su-fw mcu-info shows the running one).
  --serial SERIAL      The ADB board to use when several are attached.
)HELP";

const char* const kMcuUpdateTail = R"HELP(
Exit codes:
  0  updated
  1  the update failed (the log has the updater's reason)
  2  usage: no or an invalid package, no or an invalid role
  3  no board running on ADB
  5  --timeout or Ctrl+C

Examples:
  su-fw mcu-update --package firmware-0.8.1.pkg --role rx
  su-fw mcu-update --package firmware-0.8.1.pkg --role tx --serial 0225ce3c8d8d6e44 --json
)HELP";

const char* const kMcuSetRoleHelp = R"HELP(su-fw mcu-set-role - set the CC1310's role

Usage: su-fw mcu-set-role --role rx|tx [--serial SERIAL] [options]

Like mcu-update, but sends only the new role (SET_ROLE); the firmware stays as it is.

Options:
  --role rx|tx         The new role. Required.
  --serial SERIAL      The ADB board to use when several are attached.
)HELP";

const char* const kMcuSetRoleTail = R"HELP(
Exit codes:
  0  the role is set
  1  setting it failed (the log has the updater's reason)
  2  usage: no or an invalid role
  3  no board running on ADB
  5  --timeout or Ctrl+C

Examples:
  su-fw mcu-set-role --role tx
  su-fw mcu-set-role --role rx --json
)HELP";

const char* const kPullFlashHelp = R"HELP(su-fw pull-flash - pull, flash and update in one go (the tab's Pull & Flash)

Usage: su-fw pull-flash [--target TARGET] [pull options] [--package FILE] [--role rx|tx]
                        [--serial SERIAL] [--layout MTDPARTS] [--all] [options]

pull (skipped when unchanged) -> flash-soc with every partition -> wait for the board on ADB ->
read the CC1310's version -> mcu-update when --package is given and its version differs from the
running one (or the CC1310 does not answer: it may sit in its updater). Stops at the first
failure and says where: "stopped at <step>: <reason>".

Options:
)HELP";

const char* const kPullFlashOptionsHelp = R"HELP(  --package FILE       The CC1310 package; without one the CC1310 is left alone.
  --role rx|tx         Only used for a CC1310 that cannot report its role (no answer: it sits in
                       its updater). A running CC1310 keeps the role it reports (mcu-set-role
                       changes it).
  --serial SERIAL      The ADB board to use when several are attached.
  --layout MTDPARTS    As for flash-soc.
  --all                Every partition: the default, and the only choice here.
  --partitions NAMES   Not available: pull-flash writes every partition, like the tab's Pull &
                       Flash. To write some: su-fw pull, then su-fw flash-soc --partitions NAMES.
)HELP";

const char* const kPullFlashTail = R"HELP(
Exit codes:
  0  done (the MCU step may say "update skipped"; without --package: "no MCU package chosen")
  1  a step failed: the result says "stopped at <step>: <reason>"
  2  usage
  3  no board entered Loader / Maskrom mode, or no board on ADB for the MCU step
  4  SSH: the pull could not connect
  5  --timeout or Ctrl+C

Examples:
  su-fw pull-flash --target "Build machine"
  su-fw pull-flash --target builder@10.0.10.71 --password-env SU_PASSWORD --package firmware-0.8.1.pkg --json
)HELP";

// ---- Output helpers ------------------------------------------------------------------------------

QString nativePath(const QString& path)
{
    return QDir::toNativeSeparators(path);
}

QString clockText()
{
    return QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
}

QString isoNow()
{
    const QDateTime now = QDateTime::currentDateTime();
    return now.toOffsetFromUtc(now.offsetFromUtc()).toString(Qt::ISODateWithMs);
}

/// One JSON value in its compact form (QJsonDocument only takes objects and arrays).
QByteArray jsonText(const QJsonValue& value)
{
    const QByteArray array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return array.mid(1, array.size() - 2);
}

using JsonFields = QList<QPair<QString, QJsonValue>>;

/// The fields in the order given ("time", "step", "event" first reads better than QJsonObject's
/// alphabetical order); then `extra` (alphabetical).
QString jsonLine(const JsonFields& fields, const QJsonObject& extra = QJsonObject())
{
    QByteArray line("{");
    bool first = true;
    const auto add = [&line, &first](const QString& key, const QJsonValue& value) {
        if (!first) {
            line += ',';
        }
        first = false;
        line += jsonText(QJsonValue(key)) + ':' + jsonText(value);
    };
    for (const auto& field : fields) {
        add(field.first, field.second);
    }
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) {
        add(it.key(), it.value());
    }
    line += '}';
    return QString::fromUtf8(line);
}

QString modeKey(SocFlasher::DeviceStatus::Mode mode)
{
    switch (mode) {
    case SocFlasher::DeviceStatus::Mode::Adb: return QStringLiteral("adb");
    case SocFlasher::DeviceStatus::Mode::Loader: return QStringLiteral("loader");
    case SocFlasher::DeviceStatus::Mode::Maskrom: return QStringLiteral("maskrom");
    case SocFlasher::DeviceStatus::Mode::NoDriver: return QStringLiteral("no-driver");
    case SocFlasher::DeviceStatus::Mode::None: break;
    }
    return QStringLiteral("none");
}

QJsonObject boardJson(const SocFlasher::DeviceStatus& status)
{
    return QJsonObject{{QStringLiteral("mode"), modeKey(status.mode)},
                       {QStringLiteral("serial"), status.serial},
                       {QStringLiteral("detail"), status.detail},
                       {QStringLiteral("hint"), status.hint},
                       {QStringLiteral("text"), status.text()}};
}

QJsonObject imageJson(const ImagePuller::CacheEntry& entry)
{
    return QJsonObject{{QStringLiteral("path"), nativePath(entry.path)},
                       {QStringLiteral("size"), double(entry.size)},
                       {QStringLiteral("built"), entry.remoteModified.isValid() ? entry.remoteModified.toString(Qt::ISODate) : QString()},
                       {QStringLiteral("pulledAt"), entry.pulledAt.isValid() ? entry.pulledAt.toString(Qt::ISODate) : QString()},
                       {QStringLiteral("target"), entry.target},
                       {QStringLiteral("remoteFolder"), entry.remoteFolder},
                       {QStringLiteral("gitDescribe"), entry.gitDescribe},
                       {QStringLiteral("summary"), entry.summary()}};
}

void writeTo(QIODevice* device, const QByteArray& bytes)
{
    if (!device) {
        return;
    }
    device->write(bytes);
    if (auto* file = qobject_cast<QFileDevice*>(device)) {
        file->flush();   // stdout may be a pipe an agent reads line by line
    }
}

/// adb's and AdbTool's ways of saying that the board is not there (or not usable) for an MCU step.
bool noAdbBoard(const QString& reason)
{
    return reason.contains(QLatin1String("no ADB board attached")) || reason.contains(QLatin1String("no devices/emulators found"))
           || reason.contains(QLatin1String("is not attached; attached:")) || reason.contains(QLatin1String("not ready (unauthorized/offline"))
           || reason.contains(QLatin1String("several ADB boards attached")) || reason.contains(QLatin1String("device offline"))
           || reason.contains(QLatin1String("device unauthorized"));
}

} // namespace

// =======================================================================================
// Private
// =======================================================================================

struct FirmwareCli::Private
{
    Private(FirmwareCli* owner, ToolRunner* toolRunner) : q(owner), runner(toolRunner) {}

    /// What the command line asked for (validated before the job exists).
    struct Inputs
    {
        QString serial;
        bool mcu = false;
        QString image;              ///< --image (checked); "" = the cache's update.img
        QString cache;              ///< --cache or the app's cache folder
        QString layout;             ///< --layout (parses) or the one remembered for --serial
        QString layoutSource;       ///< how the board layout was chosen, for the partition lines
        QString target;
        bool targetIsProfile = false;
        QString remoteFolder;
        QString identity;
        bool acceptHostKey = false;
        bool all = false;
        QStringList partitionNames;
        bool noWait = false;
        QString package;            ///< checked; "" = none
        quint32 role = 0;
        QString toolsDir;           ///< --tools; "" = the app's paths / the bundled tools
    };

    FirmwareCli* q;
    ToolRunner* runner;

    // ---- What main.cpp / the tests hand in -----------------------------------------------------
    LineReader lineReader;
    bool inputInteractive = false;
    QPointer<SshProfileStore> givenStore;
    bool toolsFixed = false;
    QString fixedUpgradeTool;
    QString fixedAdb;
    std::function<void(FirmwareJob&)> jobSetup;

    // ---- One run ------------------------------------------------------------------------------------
    bool running = false;
    QIODevice* outDevice = nullptr;
    QIODevice* errDevice = nullptr;
    std::unique_ptr<QFile> logFile;
    bool jsonOutput = false;
    bool quietOutput = false;
    bool noQuestions = true;
    QString commandName;
    Inputs opts;
    std::unique_ptr<SshProfileStore> ownStore;
    SshProfileStore* profileStore = nullptr;
    std::unique_ptr<FirmwareJob> job;
    QTimer timeoutTimer;
    int timeoutSeconds = 0;
    bool interrupted = false;
    bool timedOut = false;
    bool cancelPending = false;      ///< a cancel the job refused (a write): sent again when it can be
    bool asking = false;             ///< a question waits for stdin inside one of the job's callbacks

    QString startedOp;
    bool opFinished = false;
    bool opOk = false;
    QString opMessage;
    QString failedStep;              ///< the first step that failed, and why
    QString failedMessage;
    bool sshAttempted = false;
    bool sshConnected = false;
    bool layoutReadDone = false;

    QString progressKey;
    int progressBucket = -1;

    std::optional<QString> secretValue;
    QString secretSource;            ///< "--password-env" / "--password-stdin" (never the variable's name)
    bool secretUsed = false;

    QJsonObject resultFields;
    QString resultMessage;

    // ---- Run framing ---------------------------------------------------------------------------------
    void beginRun(QIODevice* out, QIODevice* err);
    void endRun();
    int execute(const QStringList& arguments);
    int usageError(const QString& message);
    QString parseCommon(const QCommandLineParser& parser);
    QString parseInputs(const QCommandLineParser& parser);
    QString parseTarget(const QCommandLineParser& parser);
    QString parseSecret(const QCommandLineParser& parser);
    QString openLog(const QCommandLineParser& parser);
    bool aborted() const { return interrupted || timedOut; }
    bool imageWrittenThenCancelled() const;
    void stopRun(const QString& why, bool* flag);
    void requestCancel();

    // ---- Output -------------------------------------------------------------------------------------------
    void writeLine(const QString& line, bool force = false);
    void writeErr(const QString& text);
    QString currentStep() const;
    void log(const QString& text);
    void onToolOutput(const QString& line);
    void onProgress(const QString& stage, int percent);
    void onStepFinished(const QString& step, bool ok, const QString& message);
    void finish(int code, const QString& message);

    // ---- The job ---------------------------------------------------------------------------------------
    void createJob();
    void configureTools();
    void configurePull();
    void applyLayout();
    void selectImage();
    bool waitUntil(const std::function<bool()>& done, int timeoutMs, bool abortable);
    void runOperation(const QString& op, const std::function<void()>& start);
    int operationExitCode() const;
    QString operationResultMessage() const;
    bool queryBoard();
    int requireRunningBoard();
    bool waitForPlan();
    void logPartitionTable();
    QJsonObject mcuJson() const;

    // ---- Commands --------------------------------------------------------------------------------------
    int runCommand();
    int cmdStatus();
    int cmdPartitions();
    int cmdPull();
    int cmdFlashSoc();
    int cmdMcu();
    int cmdPullFlash();

    // ---- Questions -------------------------------------------------------------------------------------
    bool canAsk() const { return !noQuestions && lineReader; }
    FirmwareJob::HostKeyDecision decideHostKey(const SshConnection::HostKeyInfo& info);
    std::optional<FirmwareJob::AuthAnswer> answerPrompt(const SshConnection::AuthPrompt& prompt);
};

// =======================================================================================
// Run framing
// =======================================================================================

void FirmwareCli::Private::beginRun(QIODevice* out, QIODevice* err)
{
    running = true;
    outDevice = out;
    errDevice = err;
    logFile.reset();
    jsonOutput = false;
    quietOutput = false;
    noQuestions = true;
    commandName.clear();
    opts = Inputs();
    ownStore.reset();
    profileStore = nullptr;
    job.reset();
    timeoutSeconds = 0;
    interrupted = false;
    timedOut = false;
    cancelPending = false;
    asking = false;
    startedOp.clear();
    opFinished = false;
    opOk = false;
    opMessage.clear();
    failedStep.clear();
    failedMessage.clear();
    sshAttempted = false;
    sshConnected = false;
    layoutReadDone = false;
    progressKey.clear();
    progressBucket = -1;
    secretValue.reset();
    secretSource.clear();
    secretUsed = false;
    resultFields = QJsonObject();
    resultMessage.clear();
}

void FirmwareCli::Private::endRun()
{
    timeoutTimer.stop();
    job.reset();   // its destructor takes the MCU link down and closes the build-machine connection
    if (logFile) {
        logFile->close();
        logFile.reset();
    }
    ownStore.reset();
    profileStore = nullptr;
    secretValue.reset();
    outDevice = nullptr;
    errDevice = nullptr;
    running = false;
}

int FirmwareCli::Private::usageError(const QString& message)
{
    QString text = QStringLiteral("su-fw: ") + message + QLatin1Char('\n');
    text += commandName.isEmpty() ? QStringLiteral("Run 'su-fw --help' for the commands and options.\n")
                                  : QStringLiteral("Run 'su-fw %1 --help' for its options.\n").arg(commandName);
    writeErr(text);
    resultMessage = message;   // the result line, when stdout has already begun
    return ExitUsage;
}

int FirmwareCli::Private::execute(const QStringList& arguments)
{
    if (arguments.isEmpty()) {
        return usageError(QStringLiteral("a command is required"));
    }
    const QString first = arguments.first().trimmed();
    if (first == QLatin1String("-h") || first == QLatin1String("--help") || first == QLatin1String("-?")
        || first == QLatin1String("help")) {
        if (first == QLatin1String("help") && arguments.size() > 1) {
            if (!FirmwareCli::commands().contains(arguments.at(1))) {
                return usageError(QStringLiteral("unknown command \"%1\"").arg(arguments.at(1)));
            }
            writeTo(outDevice, FirmwareCli::helpText(arguments.at(1)).toUtf8());
            return ExitOk;
        }
        writeTo(outDevice, FirmwareCli::helpText().toUtf8());
        return ExitOk;
    }
    if (first == QLatin1String("--version") || first == QLatin1String("-v")) {
        writeTo(outDevice, (FirmwareCli::versionText() + QLatin1Char('\n')).toUtf8());
        return ExitOk;
    }
    if (!FirmwareCli::commands().contains(first)) {
        return usageError(first.startsWith(QLatin1Char('-')) ? QStringLiteral("the command comes first: su-fw <command> [options]")
                                                             : QStringLiteral("unknown command \"%1\"").arg(first));
    }
    commandName = first;

    QCommandLineParser parser;
    // "-json" = "--json"; there are no one-letter options to combine except -h.
    parser.setSingleDashWordOptionMode(QCommandLineParser::ParseAsLongOptions);
    const QCommandLineOption helpOption({QStringLiteral("h"), QStringLiteral("help")}, QString());
    parser.addOption(helpOption);
    for (const QString& name : FirmwareCli::commandOptions(commandName)) {
        const QString value = optionValueName(name);
        parser.addOption(value.isEmpty() ? QCommandLineOption(name, QString()) : QCommandLineOption(name, QString(), value));
    }
    if (!parser.parse(QStringList{QStringLiteral("su-fw")} + arguments.mid(1))) {
        return usageError(parser.errorText());
    }
    if (parser.isSet(helpOption)) {
        writeTo(outDevice, FirmwareCli::helpText(commandName).toUtf8());
        return ExitOk;
    }
    if (!parser.positionalArguments().isEmpty()) {
        // Never echoed: a stray word may be a password typed in the wrong place.
        return usageError(QStringLiteral("%1 takes options only, and a value goes right after its option "
                                         "(an unexpected argument is not shown here)")
                              .arg(commandName));
    }
    QString problem = parseCommon(parser);
    if (problem.isEmpty()) {
        problem = parseInputs(parser);
    }
    if (problem.isEmpty()) {
        problem = openLog(parser);   // only a command line that is valid as a whole creates the file
    }
    if (aborted()) {
        // Ctrl+C while the command line was read (the password from stdin): nothing has run, and
        // nothing starts. Before a usage error too: the read may have failed because of it.
        log(QStringLiteral("Interrupted (Ctrl+C)"));
        finish(ExitCancelled, QStringLiteral("interrupted before anything ran"));
        return ExitCancelled;
    }
    if (!problem.isEmpty()) {
        return usageError(problem);
    }

    createJob();
    configureTools();
    if (jobSetup) {
        jobSetup(*job);
    }
    if (timeoutSeconds > 0) {
        timeoutTimer.start(timeoutSeconds * 1000);
    }
    // A usage error found by the command (a partition name the image does not have) went to
    // stderr already; stdout has begun, so it gets its result line too.
    const int code = runCommand();
    finish(code, resultMessage);
    return code;
}

QString FirmwareCli::Private::parseCommon(const QCommandLineParser& parser)
{
    jsonOutput = parser.isSet(QStringLiteral("json"));
    quietOutput = parser.isSet(QStringLiteral("quiet"));
    noQuestions = parser.isSet(QStringLiteral("yes")) || !inputInteractive || !lineReader;
    if (parser.isSet(QStringLiteral("timeout"))) {
        bool ok = false;
        const int seconds = parser.value(QStringLiteral("timeout")).trimmed().toInt(&ok);
        if (!ok || seconds < 0 || seconds > kMaxTimeoutSeconds) {
            return QStringLiteral("--timeout needs a whole number of seconds up to %1 (0 = no limit)").arg(kMaxTimeoutSeconds);
        }
        timeoutSeconds = seconds;
    }
    if (parser.isSet(QStringLiteral("tools"))) {
        const QString dir = QDir::fromNativeSeparators(parser.value(QStringLiteral("tools")).trimmed());
        if (dir.isEmpty() || !QFileInfo(dir).isDir()) {
            return QStringLiteral("--tools: no such folder %1").arg(nativePath(dir));
        }
        opts.toolsDir = QFileInfo(dir).absoluteFilePath();
    }
    return QString();
}

QString FirmwareCli::Private::openLog(const QCommandLineParser& parser)
{
    if (!parser.isSet(QStringLiteral("log"))) {
        return QString();
    }
    const QString path = parser.value(QStringLiteral("log")).trimmed();
    auto file = std::make_unique<QFile>(path);
    if (path.isEmpty() || !file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        return QStringLiteral("--log: cannot write %1: %2").arg(nativePath(path), file->errorString());
    }
    logFile = std::move(file);
    return QString();
}

QString FirmwareCli::Private::parseInputs(const QCommandLineParser& parser)
{
    const AppSettings& settings = AppSettings::instance();
    // Only the command's own options may be asked for (QCommandLineParser warns about the others).
    const QStringList known = FirmwareCli::commandOptions(commandName);
    const auto isSet = [&parser, &known](const char* name) {
        return known.contains(QLatin1String(name)) && parser.isSet(QLatin1String(name));
    };
    const auto value = [&parser, &isSet](const char* name) {
        return isSet(name) ? parser.value(QLatin1String(name)).trimmed() : QString();
    };
    const bool pulling = commandName == kPull || commandName == kPullFlash;

    opts.serial = value("serial");
    opts.mcu = isSet("mcu");
    opts.noWait = isSet("no-wait");
    opts.cache = isSet("cache") ? QDir::fromNativeSeparators(value("cache")) : settings.firmwareCacheDirectory();
    if (isSet("cache") && opts.cache.isEmpty()) {
        return QStringLiteral("--cache needs a folder");
    }

    // ---- The image (partitions, flash-soc) ----
    if (commandName == kPartitions || commandName == kFlashSoc) {
        if (isSet("image")) {
            const QString path = QDir::fromNativeSeparators(value("image"));
            QString reason;
            if (path.isEmpty() || !QFileInfo(path).isFile()) {
                reason = QStringLiteral("file not found");
            } else if (!RockchipTool::isRkfwImage(path, &reason) && reason.isEmpty()) {
                reason = QStringLiteral("not a Rockchip RKFW image");
            }
            if (!reason.isEmpty()) {
                return QStringLiteral("Image %1 refused: %2").arg(nativePath(path), reason);   // the job's wording
            }
            opts.image = QFileInfo(path).absoluteFilePath();
        } else if (!QFileInfo(ImagePuller::imagePath(opts.cache)).isFile()) {
            return QStringLiteral("no image: pass --image FILE, or pull one first (there is no update.img in %1)")
                .arg(nativePath(opts.cache));
        }
    }

    // ---- The board's layout ----
    if (isSet("layout")) {
        opts.layout = value("layout");
        if (RockchipTool::parseMtdParts(opts.layout).isEmpty()) {
            return QStringLiteral("--layout: \"%1\" is not a flash layout such as "
                                  "sfc_nor:64K(env),256K@64K(idblock),256K(uboot),...")
                .arg(opts.layout);
        }
        opts.layoutSource = QStringLiteral("--layout");
    } else if (!opts.serial.isEmpty()) {
        opts.layout = settings.firmwareMtdParts(opts.serial);
        if (!opts.layout.isEmpty()) {
            opts.layoutSource = QStringLiteral("remembered for board %1").arg(opts.serial);
        }
    }

    // ---- The partition choice ----
    if (commandName == kFlashSoc || commandName == kPullFlash) {
        opts.all = isSet("all");
        const bool some = isSet("partitions");
        if (commandName == kPullFlash && some) {
            return QStringLiteral("pull-flash writes every partition, like the tab's Pull & Flash; to write some, run "
                                  "su-fw pull, then su-fw flash-soc --partitions NAMES");
        }
        if (commandName == kFlashSoc) {
            if (opts.all == some) {
                return opts.all ? QStringLiteral("choose either --all or --partitions NAMES, not both")
                                : QStringLiteral("choose what to write: --all, or --partitions NAMES (su-fw partitions lists them)");
            }
            for (const QString& word : parser.values(QStringLiteral("partitions"))) {
                for (const QString& name : word.split(QLatin1Char(','))) {
                    const QString trimmed = name.trimmed();
                    if (!trimmed.isEmpty() && !opts.partitionNames.contains(trimmed)) {
                        opts.partitionNames << trimmed;
                    }
                }
            }
            if (some && opts.partitionNames.isEmpty()) {
                return QStringLiteral("--partitions needs names, comma separated (e.g. boot,rootfs)");
            }
        }
    }

    // ---- The MCU ----
    if (isSet("package")) {
        const QString path = QDir::fromNativeSeparators(value("package"));
        CcPackage loaded;
        QString reason;
        if (!CcPackage::load(path, &loaded, &reason)) {
            return QStringLiteral("Package %1 refused: %2")
                .arg(QFileInfo(path).fileName(), reason.isEmpty() ? QStringLiteral("unreadable file") : reason);
        }
        opts.package = QFileInfo(path).absoluteFilePath();
    } else if (commandName == kMcuUpdate) {
        return QStringLiteral("--package FILE is required");
    }
    if (isSet("role")) {
        opts.role = FirmwareJob::roleFromText(value("role"));
        if (opts.role == 0) {
            return QStringLiteral("--role must be rx or tx");
        }
    } else if (commandName == kMcuUpdate || commandName == kMcuSetRole) {
        return QStringLiteral("--role rx|tx is required: the MCU role is never guessed (su-fw mcu-info shows the running one)");
    }

    // ---- The build machine ----
    if (pulling) {
        QString problem = parseTarget(parser);
        if (problem.isEmpty()) {
            problem = parseSecret(parser);
        }
        return problem;
    }
    return QString();
}

QString FirmwareCli::Private::parseTarget(const QCommandLineParser& parser)
{
    if (givenStore) {
        profileStore = givenStore;
    } else {
        ownStore = std::make_unique<SshProfileStore>();
        ownStore->load();   // a missing file is an empty list; a broken one is logged by the store
        profileStore = ownStore.get();
    }
    const AppSettings& settings = AppSettings::instance();
    opts.target = parser.isSet(QStringLiteral("target")) ? parser.value(QStringLiteral("target")).trimmed()
                                                         : settings.firmwareSourceTarget().trimmed();
    if (opts.target.isEmpty()) {
        return QStringLiteral("--target is required: the build machine, a profile name or user@host[:port]");
    }
    // The forms FirmwareJob understands: a restore key, a profile name, user@host[:port].
    QString key = opts.target;
    const QLatin1String profilePrefix("ssh:profile:");
    const QLatin1String targetPrefix("ssh:target:");
    if (opts.target.startsWith(profilePrefix)) {
        if (!profileStore->profile(opts.target.mid(profilePrefix.size()))) {
            return QStringLiteral("--target: the app has no SSH profile with the id %1").arg(opts.target.mid(profilePrefix.size()));
        }
        opts.targetIsProfile = true;
    } else {
        const QString plain = opts.target.startsWith(targetPrefix) ? opts.target.mid(targetPrefix.size()) : opts.target;
        if (const std::optional<SshProfile> named = profileStore->profileByName(plain)) {
            opts.targetIsProfile = true;
            key = QString(profilePrefix) + named->id;
        } else {
            SshProfile parsed;
            if (!SshProfile::parseTarget(plain, parsed)) {
                return QStringLiteral("--target: \"%1\" is neither one of the app's SSH profiles nor user@host[:port]").arg(plain);
            }
            key = plain;
        }
    }
    opts.remoteFolder = parser.isSet(QStringLiteral("remote-folder")) ? parser.value(QStringLiteral("remote-folder")).trimmed()
                                                                      : settings.firmwareRemoteFolder(key);
    if (opts.remoteFolder.isEmpty()) {
        return QStringLiteral("--remote-folder needs a folder");
    }
    if (parser.isSet(QStringLiteral("identity"))) {
        opts.identity = QDir::fromNativeSeparators(parser.value(QStringLiteral("identity")).trimmed());
        if (opts.identity.isEmpty() || !QFileInfo(opts.identity).isFile()) {
            return QStringLiteral("--identity: no such key file %1").arg(nativePath(opts.identity));
        }
    }
    opts.acceptHostKey = parser.isSet(QStringLiteral("accept-host-key"));
    return QString();
}

QString FirmwareCli::Private::parseSecret(const QCommandLineParser& parser)
{
    const bool fromEnv = parser.isSet(QStringLiteral("password-env"));
    const bool fromStdin = parser.isSet(QStringLiteral("password-stdin"));
    if (fromEnv && fromStdin) {
        return QStringLiteral("use either --password-env or --password-stdin");
    }
    if (fromEnv) {
        // The variable's name is not repeated in any message: it might be the password itself,
        // typed in the wrong place.
        const QString name = parser.value(QStringLiteral("password-env")).trimmed();
        const QString secret = name.isEmpty() ? QString() : qEnvironmentVariable(name.toLocal8Bit().constData());
        if (!name.isEmpty()) {
            // Out of the environment at once: every tool su-fw starts inherits it, and the adb
            // server the first adb call starts outlives su-fw.
            qunsetenv(name.toLocal8Bit().constData());
        }
        if (secret.isEmpty()) {
            return QStringLiteral("--password-env: the environment variable it names is not set or empty");
        }
        secretValue = secret;
        secretSource = QStringLiteral("--password-env");
    } else if (fromStdin) {
        if (lineReader && inputInteractive) {
            writeErr(QStringLiteral("Password (--password-stdin): "));   // someone at a terminal, not a pipe
        }
        const std::optional<QString> line = lineReader ? lineReader(/*secret=*/true) : std::nullopt;
        if (!line || line->isEmpty()) {
            return QStringLiteral("--password-stdin: no password could be read from stdin (one line is expected)");
        }
        secretValue = *line;
        secretSource = QStringLiteral("--password-stdin");
    }
    return QString();
}

void FirmwareCli::Private::stopRun(const QString& why, bool* flag)
{
    const bool again = *flag;
    *flag = true;
    log(why);
    if (!job || !job->isBusy()) {
        return;   // a wait outside an operation notices aborted() at its next poll
    }
    if (again && cancelPending) {
        log(QStringLiteral("Still waiting for upgrade_tool to finish writing the flash: su-fw stops right after it, "
                           "the write itself is never interrupted"));
        return;
    }
    requestCancel();
}

void FirmwareCli::Private::requestCancel()
{
    if (job->isCancellable()) {
        job->cancel();
        return;
    }
    // Refused while upgrade_tool writes the flash (the job logs it); the next phase that can be
    // cancelled (the ADB wait, the next step) gets it.
    job->cancel();
    if (job->isBusy()) {
        cancelPending = true;
        log(QStringLiteral("su-fw stops at the next point where that is safe"));
    }
}

// =======================================================================================
// Output
// =======================================================================================

void FirmwareCli::Private::writeLine(const QString& line, bool force)
{
    const QByteArray bytes = line.toUtf8() + '\n';
    if (!quietOutput || force) {
        writeTo(outDevice, bytes);
    }
    if (logFile) {
        logFile->write(bytes);
        logFile->flush();
    }
}

void FirmwareCli::Private::writeErr(const QString& text)
{
    writeTo(errDevice, text.toUtf8());
}

QString FirmwareCli::Private::currentStep() const
{
    if (job) {
        if (!job->currentStep().isEmpty()) {
            return job->currentStep();
        }
        if (!job->currentOperation().isEmpty()) {
            return job->currentOperation();
        }
        if (!startedOp.isEmpty() && !opFinished) {
            return startedOp;   // the operation's result line comes after the job cleared its name
        }
    }
    return commandName;
}

void FirmwareCli::Private::log(const QString& text)
{
    // The tab's splitting: one line per line, the blank lines of multi-line tool output dropped.
    QString clean = text;
    clean.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    clean.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    const QStringList lines = clean.split(QLatin1Char('\n'));
    const QString step = currentStep();
    for (const QString& part : lines) {
        const QString trimmed = part.trimmed();
        if (trimmed.isEmpty() && lines.size() > 1) {
            continue;
        }
        if (jsonOutput) {
            writeLine(jsonLine({{QStringLiteral("time"), isoNow()},
                                {QStringLiteral("step"), step},
                                {QStringLiteral("event"), QStringLiteral("log")},
                                {QStringLiteral("text"), trimmed}}));
        } else {
            writeLine(clockText() + QLatin1Char(' ') + trimmed);
        }
    }
}

void FirmwareCli::Private::onToolOutput(const QString& line)
{
    // upgrade_tool repeats "Download Image... (42%)" hundreds of times; the job's progress()
    // carries the same as a percentage. Everything else (errors, stage lines) is a log line.
    static const QRegularExpression progressLine(QStringLiteral(R"(\(\d{1,3}%\)\s*$)"));
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty() || progressLine.match(trimmed).hasMatch()) {
        return;
    }
    log(line);
}

void FirmwareCli::Private::onProgress(const QString& stage, int percent)
{
    if (percent < 0) {
        return;   // a phase without a percentage: the log lines narrate it
    }
    // Text every 10 %, JSON every 1 %; a new stage ("Pulling update.img", "Flashing SoC",
    // "Updating MCU": the text before the first colon) starts over.
    const QString key = stage.section(QLatin1Char(':'), 0, 0);
    const int bucket = jsonOutput ? percent : percent / 10;
    if (key == progressKey && bucket == progressBucket) {
        return;
    }
    progressKey = key;
    progressBucket = bucket;
    if (jsonOutput) {
        writeLine(jsonLine({{QStringLiteral("time"), isoNow()},
                            {QStringLiteral("step"), currentStep()},
                            {QStringLiteral("event"), QStringLiteral("progress")},
                            {QStringLiteral("text"), stage},
                            {QStringLiteral("percent"), percent}}));
    } else {
        writeLine(clockText() + QLatin1Char(' ') + stage);
    }
}

void FirmwareCli::Private::onStepFinished(const QString& step, bool ok, const QString& message)
{
    if (!ok && failedStep.isEmpty()) {
        failedStep = step;
        failedMessage = message;
    }
    if (jsonOutput) {
        writeLine(jsonLine({{QStringLiteral("time"), isoNow()},
                            {QStringLiteral("step"), step},
                            {QStringLiteral("event"), QStringLiteral("step")},
                            {QStringLiteral("text"), message},
                            {QStringLiteral("ok"), ok}}));
    }
}

void FirmwareCli::Private::finish(int code, const QString& message)
{
    QString meaning;
    switch (code) {
    case ExitOk: meaning = QStringLiteral("done"); break;
    case ExitFailed: meaning = QStringLiteral("failed"); break;
    case ExitUsage: meaning = QStringLiteral("usage"); break;
    case ExitNoBoard: meaning = QStringLiteral("no board"); break;
    case ExitSsh: meaning = QStringLiteral("SSH"); break;
    default: meaning = timedOut ? QStringLiteral("timeout") : QStringLiteral("cancelled"); break;
    }
    if (jsonOutput) {
        writeLine(jsonLine({{QStringLiteral("time"), isoNow()},
                            {QStringLiteral("step"), commandName},
                            {QStringLiteral("event"), QStringLiteral("done")},
                            {QStringLiteral("text"), message},
                            {QStringLiteral("ok"), code == ExitOk},
                            {QStringLiteral("exit"), code}},
                           resultFields),
                  /*force=*/true);
    } else {
        writeLine(QStringLiteral("%1 su-fw: exit %2 (%3): %4").arg(clockText()).arg(code).arg(meaning, message), /*force=*/true);
    }
}

// =======================================================================================
// The job
// =======================================================================================

void FirmwareCli::Private::createJob()
{
    job = std::make_unique<FirmwareJob>(runner);
    FirmwareJob* j = job.get();
    QObject::connect(j, &FirmwareJob::logLine, q, [this](const QString& line) { log(line); });
    QObject::connect(j, &FirmwareJob::toolOutput, q, [this](const QString& line) { onToolOutput(line); });
    QObject::connect(j, &FirmwareJob::progress, q, [this](const QString& stage, int percent) { onProgress(stage, percent); });
    QObject::connect(j, &FirmwareJob::stepFinished, q,
                     [this](const QString& step, bool ok, const QString& message) { onStepFinished(step, ok, message); });
    QObject::connect(j, &FirmwareJob::operationFinished, q, [this](const QString&, bool ok, const QString& message) {
        opFinished = true;
        opOk = ok;
        opMessage = message;
    });
    QObject::connect(j, &FirmwareJob::cancellableChanged, q, [this](bool cancellable) {
        if (!cancellable || !cancelPending) {
            return;
        }
        // From the event loop, never inside this emission: it comes from the flasher entering its
        // next phase (the ADB wait), which goes on with that phase once the signal returns - a
        // cancel in here would end the operation in the middle of it and leave the flasher to
        // start its first poll for an operation that has ended.
        QMetaObject::invokeMethod(
            q,
            [this]() {
                if (cancelPending && job && job->isBusy() && job->isCancellable()) {
                    cancelPending = false;
                    job->cancel();
                }
            },
            Qt::QueuedConnection);
    });
    QObject::connect(j->sshConnection(), &Transport::stateChanged, q, [this](Transport::State state) {
        if (state == Transport::State::Connecting) {
            sshAttempted = true;
        } else if (state == Transport::State::Connected) {
            sshConnected = true;
        }
    });
    // The flash layout of a board, remembered per serial in the app's settings exactly as the tab
    // does, so a later flash of the same board in Loader mode knows it.
    j->setLayoutProvider([](const QString& serial) { return AppSettings::instance().firmwareMtdParts(serial); });
    QObject::connect(j, &FirmwareJob::mtdPartsRead, q, [this](const QString& serial, const QString& mtdparts) {
        AppSettings& settings = AppSettings::instance();
        if (!serial.isEmpty() && settings.firmwareMtdParts(serial) != mtdparts) {
            settings.setFirmwareMtdParts(serial, mtdparts);
        }
        if (settings.firmwareMtdParts(QString()) != mtdparts) {
            settings.setFirmwareMtdParts(QString(), mtdparts);
        }
    });
    // The end of the status's own layout read, whatever it found (queryBoard() waits for it).
    QObject::connect(j, &FirmwareJob::layoutReadFinished, q, [this](const QString&, bool) { layoutReadDone = true; });

    const AppSettings& settings = AppSettings::instance();
    j->setLoaderWaitSeconds(settings.firmwareLoaderWaitSeconds());
    j->setAdbReturnSeconds(settings.firmwareAdbReturnSeconds());
    j->setSerial(opts.serial);
}

void FirmwareCli::Private::configureTools()
{
    QString upgradeTool;
    QString adb;
    QString upgradeToolText;
    QString adbText;
    if (toolsFixed) {
        upgradeTool = fixedUpgradeTool;
        adb = fixedAdb;
        upgradeToolText = upgradeTool.isEmpty() ? QStringLiteral("not found") : upgradeTool;
        adbText = adb.isEmpty() ? QStringLiteral("not found") : adb;
    } else if (!opts.toolsDir.isEmpty()) {
        const QFileInfo upgradeFile(QDir(opts.toolsDir).filePath(ToolLocator::fileName(ToolLocator::Tool::UpgradeTool)));
        const QFileInfo adbFile(QDir(opts.toolsDir).filePath(ToolLocator::fileName(ToolLocator::Tool::Adb)));
        upgradeTool = upgradeFile.isFile() ? upgradeFile.absoluteFilePath() : QString();
        adb = adbFile.isFile() ? adbFile.absoluteFilePath() : QDir::fromNativeSeparators(QStandardPaths::findExecutable(QStringLiteral("adb")));
        upgradeToolText = upgradeTool.isEmpty() ? QStringLiteral("not found in %1").arg(nativePath(opts.toolsDir))
                                                : QStringLiteral("--tools ") + nativePath(upgradeTool);
        adbText = adbFile.isFile() ? QStringLiteral("--tools ") + nativePath(adb)
                  : adb.isEmpty()  ? QStringLiteral("not found in %1 or on PATH").arg(nativePath(opts.toolsDir))
                                   : QStringLiteral("PATH ") + nativePath(adb);
    } else {
        const AppSettings& settings = AppSettings::instance();
        upgradeTool = ToolLocator::locate(ToolLocator::Tool::UpgradeTool, settings.firmwareUpgradeToolPath());
        adb = ToolLocator::locate(ToolLocator::Tool::Adb, settings.firmwareAdbPath());
        upgradeToolText = ToolLocator::describe(ToolLocator::Tool::UpgradeTool, settings.firmwareUpgradeToolPath());
        adbText = ToolLocator::describe(ToolLocator::Tool::Adb, settings.firmwareAdbPath());
    }
    log(QStringLiteral("su-fw %1: %2").arg(QStringLiteral(APP_VERSION), commandName));
    if (commandName == kPull) {
        // A pull needs no tool; without upgrade_tool the job does not unpack the pulled image for
        // a partition table nobody shows (the tab does), and su-fw does not end with it running.
        return;
    }
    job->setToolPaths(upgradeTool, adb);
    log(QStringLiteral("upgrade_tool: %1").arg(upgradeToolText));   // the tab's lines
    log(QStringLiteral("adb: %1").arg(adbText));
}

void FirmwareCli::Private::configurePull()
{
    const AppSettings& settings = AppSettings::instance();
    job->setProfileStore(profileStore);
    job->setSourceTarget(opts.target);
    job->setRemoteFolder(opts.remoteFolder);
    job->setCacheDirectory(opts.cache);
    job->setGitDescribe(settings.firmwareGitDescribe());
    job->setExtraFiles(settings.firmwareExtraFiles());
    FirmwareJob::SshDefaults defaults;
    defaults.knownHostsFile = settings.sshKnownHostsFile();
    defaults.identityFile = opts.identity.isEmpty() ? settings.sshDefaultIdentityFile() : opts.identity;
    defaults.terminalType = settings.sshDefaultTerminalType();
    defaults.keepAliveSeconds = settings.sshDefaultKeepAliveSeconds();
    job->setSshDefaults(defaults);
    if (!opts.identity.isEmpty() && opts.targetIsProfile) {
        log(QStringLiteral("--identity is not used: the profile %1 brings its own key settings").arg(opts.target));
    }
    job->setHostKeyHandler([this](const SshConnection::HostKeyInfo& info) { return decideHostKey(info); });
    // Without a secret and without a terminal to ask, the job's own default: the prompt is
    // cancelled and the pull fails with "authentication failed: nobody answered "<prompt>"".
    if (secretValue || canAsk()) {
        job->setAuthPromptHandler([this](const SshConnection::AuthPrompt& prompt) { return answerPrompt(prompt); });
    }
}

void FirmwareCli::Private::applyLayout()
{
    if (opts.layout.isEmpty()) {
        return;   // the job asks the app's memory (setLayoutProvider)
    }
    // The job's own memory comes before the app's: "" is the layout for a board met in Loader mode
    // (no serial), the serial's for the board when it shows up on ADB.
    job->setMtdParts(opts.layout);
    if (!opts.serial.isEmpty()) {
        job->setMtdParts(opts.layout, opts.serial);
    }
}

void FirmwareCli::Private::selectImage()
{
    if (!opts.image.isEmpty()) {
        job->setImagePath(opts.image);   // checked in parseInputs(); logs "Using the local image ..."
        return;
    }
    job->setCacheDirectory(opts.cache);
    log(QStringLiteral("Image: %1 (the cache's)").arg(nativePath(job->imagePath())));
}

bool FirmwareCli::Private::waitUntil(const std::function<bool()>& done, int timeoutMs, bool abortable)
{
    if (done()) {
        return true;
    }
    if (abortable && aborted()) {
        return false;
    }
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(kPollMs);
    const QDeadlineTimer deadline = timeoutMs < 0 ? QDeadlineTimer(QDeadlineTimer::Forever) : QDeadlineTimer(timeoutMs);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (done() || (abortable && aborted()) || deadline.hasExpired()) {
            loop.quit();
        }
    });
    poll.start();
    loop.exec();
    return done();
}

void FirmwareCli::Private::runOperation(const QString& op, const std::function<void()>& start)
{
    startedOp = op;
    opFinished = false;
    opOk = false;
    opMessage.clear();
    failedStep.clear();
    failedMessage.clear();
    if (aborted()) {
        opFinished = true;   // Ctrl+C / the timeout came before the operation could start
        opMessage = QStringLiteral("not started");
        return;
    }
    start();   // input errors end it right inside the slot
    // Not abortable: Ctrl+C and the timeout cancel the job, which then ends the operation.
    waitUntil([this]() { return opFinished; }, -1, /*abortable=*/false);
}

int FirmwareCli::Private::operationExitCode() const
{
    if (opOk) {
        return ExitOk;
    }
    if (aborted()) {
        return ExitCancelled;
    }
    const QString step = failedStep.isEmpty() ? startedOp : failedStep;
    const QString reason = failedStep.isEmpty() ? opMessage : failedMessage;
    if (step == FirmwareJob::kOpPull && sshAttempted && !sshConnected) {
        return ExitSsh;   // never got through: unreachable, host key, authentication
    }
    if (step == FirmwareJob::kOpFlash && reason.startsWith(QLatin1String("board did not enter MASKROM/Loader"))) {
        return ExitNoBoard;
    }
    if ((step == FirmwareJob::kOpMcu || step == FirmwareJob::kOpSetRole || step == FirmwareJob::kOpInfo) && noAdbBoard(reason)) {
        return ExitNoBoard;
    }
    return ExitFailed;
}

bool FirmwareCli::Private::imageWrittenThenCancelled() const
{
    // A flash step can only be cancelled while it waits: before the write (no method chosen yet)
    // or for ADB after it (the write is done).
    return failedStep == FirmwareJob::kOpFlash && job && job->socFlasher()->phase() == SocFlasher::Phase::Cancelled
           && job->socFlasher()->flashMethod() != SocFlasher::FlashMethod::None;
}

QString FirmwareCli::Private::operationResultMessage() const
{
    if (opOk || !aborted()) {
        return opMessage;
    }
    if (imageWrittenThenCancelled()) {
        // Not "cancelled" alone: an agent would flash again, or report a failed update.
        const QString why = timedOut ? QStringLiteral("timeout (%1 s)").arg(timeoutSeconds) : QStringLiteral("interrupted");
        return QStringLiteral("%1 after the image was written; the ADB wait was cancelled").arg(why);
    }
    const QString why = timedOut ? QStringLiteral("timeout after %1 s").arg(timeoutSeconds) : QStringLiteral("interrupted");
    return QStringLiteral("%1: %2").arg(why, opMessage);
}

bool FirmwareCli::Private::queryBoard()
{
    bool reported = false;
    bool readingLayout = false;
    QString serialSeen;
    const QMetaObject::Connection connection =
        QObject::connect(job.get(), &FirmwareJob::statusChanged, q, [&](const SocFlasher::DeviceStatus& status) {
            if (reported) {
                return;
            }
            reported = true;
            if (status.mode == SocFlasher::DeviceStatus::Mode::Adb && !status.serial.isEmpty()) {
                serialSeen = status.serial;
                // The job reads /proc/cmdline right after this signal when nobody knows the layout yet.
                readingLayout = job->mtdParts(status.serial).isEmpty() && !job->adbPath().isEmpty();
            }
        });
    layoutReadDone = false;
    job->refreshStatus();
    if (!job->statusPausedReason().isEmpty()) {
        // The tab or another su-fw runs an operation on this PC: no tool may talk to the board now
        // (the job logged it).
        QObject::disconnect(connection);
        resultMessage = job->statusPausedReason();
        return false;
    }
    const bool answered = waitUntil([&reported]() { return reported; }, kStatusWaitMs, /*abortable=*/true);
    QObject::disconnect(connection);
    if (!answered) {
        if (!aborted()) {
            resultMessage = QStringLiteral("no answer from upgrade_tool / adb within %1 s").arg(kStatusWaitMs / 1000);
            log(resultMessage);
        }
        return false;
    }
    const SocFlasher::DeviceStatus status = job->lastStatus();
    resultFields.insert(QStringLiteral("board"), boardJson(status));
    log(QStringLiteral("Board: %1").arg(status.text()));
    if (!serialSeen.isEmpty()) {
        if (readingLayout) {
            // Its lines say what came of it; waiting keeps the next adb command from overlapping.
            waitUntil([this]() { return layoutReadDone; }, kLayoutReadWaitMs, /*abortable=*/true);
        } else {
            log(QStringLiteral("Flash layout of board %1 (remembered): %2").arg(serialSeen, job->mtdParts(serialSeen)));
        }
        const QString layout = job->mtdParts(serialSeen);
        if (!layout.isEmpty()) {
            resultFields.insert(QStringLiteral("layout"), layout);
        }
    }
    return !aborted();
}

int FirmwareCli::Private::requireRunningBoard()
{
    if (!queryBoard()) {
        return aborted() ? ExitCancelled : ExitFailed;
    }
    const SocFlasher::DeviceStatus status = job->lastStatus();
    if (status.mode == SocFlasher::DeviceStatus::Mode::Adb) {
        return ExitOk;
    }
    resultMessage = QStringLiteral("%1 needs the board running (ADB); the PC sees: %2").arg(commandName, status.text());
    log(resultMessage);
    return ExitNoBoard;
}

bool FirmwareCli::Private::waitForPlan()
{
    if (job->planState() != FirmwareJob::PlanState::Planning) {
        job->planPartitions();
    }
    return waitUntil([this]() { return job->planState() != FirmwareJob::PlanState::Planning; }, kPlanWaitMs, /*abortable=*/true);
}

void FirmwareCli::Private::logPartitionTable()
{
    const QList<PartitionImage> rows = job->partitions();
    const bool known = job->layoutKnown();
    QString source;
    if (job->usingImageLayout()) {
        source = QStringLiteral("from the image's env.img");
    } else if (!opts.layoutSource.isEmpty()) {
        source = opts.layoutSource;
    } else if (known) {
        source = QStringLiteral("remembered from the board read last");
    }
    if (known) {
        log(QStringLiteral("Flash layout (%1): %2").arg(source, job->flashLayout()));
    } else {
        log(QStringLiteral("Flash layout: unknown - the image carries none and no board layout is remembered; "
                           "pass --layout, or run su-fw status once with the board running"));
    }

    QJsonArray array;
    QList<QStringList> table;
    table.append(QStringList{QStringLiteral("name"), QStringLiteral("file"), QStringLiteral("offset"), QStringLiteral("partition"),
                             QStringLiteral("image"), QStringLiteral("fits")});
    QStringList notes{QString()};
    int writable = 0;
    for (const PartitionImage& row : rows) {
        const QString problem = FirmwareJob::partitionProblem(row, known);
        if (problem.isEmpty()) {
            ++writable;
        }
        table.append(QStringList{row.name, row.fileSize >= 0 ? QFileInfo(row.file).fileName() : QStringLiteral("-"),
                      row.inLayout ? QStringLiteral("0x%1").arg(row.offset, 0, 16) : QStringLiteral("-"),
                      !row.inLayout ? QStringLiteral("-") : row.size == 0 ? QStringLiteral("to the end") : QString::number(row.size),
                      row.fileSize >= 0 ? QString::number(row.fileSize) : QStringLiteral("missing"),
                      row.inLayout && row.fileSize >= 0 ? (row.fits ? QStringLiteral("yes") : QStringLiteral("no")) : QStringLiteral("-")});
        notes << problem;
        array.append(QJsonObject{{QStringLiteral("name"), row.name},
                                 {QStringLiteral("file"), nativePath(row.file)},
                                 {QStringLiteral("offset"), double(row.offset)},
                                 {QStringLiteral("size"), double(row.size)},
                                 {QStringLiteral("imageSize"), double(row.fileSize)},
                                 {QStringLiteral("inLayout"), row.inLayout},
                                 {QStringLiteral("fits"), row.fits},
                                 {QStringLiteral("writable"), problem.isEmpty()},
                                 {QStringLiteral("problem"), problem}});
    }
    log(QStringLiteral("Partitions of %1: %2, %3 can be written")
            .arg(QFileInfo(job->plannedImage()).fileName())
            .arg(rows.size())
            .arg(writable));
    QList<qsizetype> widths(table.first().size(), 0);
    for (const QStringList& cells : std::as_const(table)) {
        for (qsizetype c = 0; c < cells.size(); ++c) {
            widths[c] = qMax(widths[c], cells.at(c).size());
        }
    }
    for (qsizetype r = 0; r < table.size(); ++r) {
        QString line;
        for (qsizetype c = 0; c < table.at(r).size(); ++c) {
            line += table.at(r).at(c).leftJustified(widths.at(c) + 2);
        }
        if (!notes.at(r).isEmpty()) {
            line += QStringLiteral("- ") + notes.at(r);
        }
        log(line);
    }
    resultFields.insert(QStringLiteral("image"), nativePath(job->plannedImage()));
    resultFields.insert(QStringLiteral("layout"), job->flashLayout());
    resultFields.insert(QStringLiteral("layoutSource"), job->usingImageLayout() ? QStringLiteral("image")
                                                        : known                ? QStringLiteral("board")
                                                                               : QString());
    resultFields.insert(QStringLiteral("partitions"), array);
    resultMessage = QStringLiteral("%1 partitions, %2 can be written").arg(rows.size()).arg(writable);
}

QJsonObject FirmwareCli::Private::mcuJson() const
{
    return QJsonObject{{QStringLiteral("version"), job->runningMcuVersion()}, {QStringLiteral("role"), job->runningMcuRole()}};
}

// =======================================================================================
// Commands
// =======================================================================================

int FirmwareCli::Private::runCommand()
{
    if (commandName == kStatus) {
        return cmdStatus();
    }
    if (commandName == kPartitions) {
        return cmdPartitions();
    }
    if (commandName == kPull) {
        return cmdPull();
    }
    if (commandName == kFlashSoc) {
        return cmdFlashSoc();
    }
    if (commandName == kPullFlash) {
        return cmdPullFlash();
    }
    return cmdMcu();
}

int FirmwareCli::Private::cmdStatus()
{
    if (!queryBoard()) {
        if (aborted()) {
            resultMessage = timedOut ? QStringLiteral("timeout after %1 s").arg(timeoutSeconds) : QStringLiteral("interrupted");
            return ExitCancelled;
        }
        return ExitFailed;
    }
    const SocFlasher::DeviceStatus status = job->lastStatus();
    resultMessage = status.text();
    if (status.mode == SocFlasher::DeviceStatus::Mode::None || status.mode == SocFlasher::DeviceStatus::Mode::NoDriver) {
        return ExitNoBoard;
    }
    if (!opts.mcu) {
        return ExitOk;
    }
    if (status.mode != SocFlasher::DeviceStatus::Mode::Adb) {
        resultMessage = QStringLiteral("%1; the MCU is read through the running board (ADB)").arg(status.text());
        log(QStringLiteral("MCU: not read: the board is not running (ADB)"));
        return ExitNoBoard;
    }
    runOperation(QString(FirmwareJob::kOpInfo), [this]() { job->readMcuInfo(); });
    const int code = operationExitCode();
    if (opOk) {
        resultFields.insert(QStringLiteral("mcu"), mcuJson());
        resultMessage = QStringLiteral("%1; MCU version=%2 role=%3").arg(status.text(), job->runningMcuVersion(), job->runningMcuRole());
    } else {
        resultMessage = QStringLiteral("%1; MCU: %2").arg(status.text(), operationResultMessage());
    }
    return code;
}

int FirmwareCli::Private::cmdPartitions()
{
    applyLayout();
    selectImage();
    if (!waitForPlan()) {
        resultMessage = aborted() ? (timedOut ? QStringLiteral("timeout after %1 s").arg(timeoutSeconds) : QStringLiteral("interrupted"))
                                  : QStringLiteral("the image was not unpacked within %1 s").arg(kPlanWaitMs / 1000);
        return aborted() ? ExitCancelled : ExitFailed;
    }
    if (job->planState() != FirmwareJob::PlanState::Ready) {
        resultMessage = QStringLiteral("cannot list the partitions of %1: %2")
                            .arg(nativePath(job->imagePath()), job->planMessage().isEmpty() ? QStringLiteral("no plan") : job->planMessage());
        return ExitFailed;
    }
    logPartitionTable();
    return ExitOk;
}

int FirmwareCli::Private::cmdPull()
{
    configurePull();
    runOperation(QString(FirmwareJob::kOpPull), [this]() { job->pull(); });
    const int code = operationExitCode();
    resultMessage = operationResultMessage();
    if (opOk) {
        const ImagePuller::CacheEntry entry = job->cacheEntry();
        log(QStringLiteral("Image: %1").arg(entry.summary()));
        log(QStringLiteral("Image file: %1").arg(nativePath(entry.path)));
        resultFields.insert(QStringLiteral("image"), imageJson(entry));
    } else if (code == ExitSsh && opMessage.contains(QLatin1String("nobody answered"))) {
        log(QStringLiteral("The build machine asks for a password: pass --password-env VAR or --password-stdin, "
                           "or let the app remember it once (Remember)"));
    }
    return code;
}

int FirmwareCli::Private::cmdFlashSoc()
{
    applyLayout();
    selectImage();
    if (opts.noWait) {
        job->socFlasher()->setWaitForAdb(false);
    }
    // The partition list the choice is checked against (the flash itself reuses the unpacked files).
    if (!waitForPlan() && aborted()) {
        resultMessage = timedOut ? QStringLiteral("timeout after %1 s").arg(timeoutSeconds) : QStringLiteral("interrupted");
        return ExitCancelled;
    }
    const bool ready = job->planState() == FirmwareJob::PlanState::Ready;
    const bool known = job->layoutKnown();
    if (opts.all) {
        job->selectAllPartitions();
        // "Every partition" means every one: a row that does not fit would be left out by the
        // selection, so refuse before the board is touched (Pull & Flash's flasher does the same).
        QStringList problems;
        for (const PartitionImage& row : job->partitions()) {
            const QString problem = FirmwareJob::partitionProblem(row, known);
            if (ready && known && row.inLayout && !problem.isEmpty()) {
                problems << problem;
            }
        }
        if (!problems.isEmpty()) {
            for (const QString& problem : std::as_const(problems)) {
                log(QStringLiteral("Partition refused: %1").arg(problem));
            }
            resultMessage = QStringLiteral("--all: not every partition can be written (%1); nothing was written")
                                .arg(problems.join(QStringLiteral("; ")));
            return ExitFailed;
        }
    } else {
        if (!ready) {
            resultMessage = QStringLiteral("--partitions needs the image's partition list, which could not be made: %1")
                                .arg(job->planMessage().isEmpty() ? QStringLiteral("no plan") : job->planMessage());
            return ExitFailed;
        }
        QStringList available;
        for (const PartitionImage& row : job->partitions()) {
            available << row.name;
        }
        for (const QString& name : std::as_const(opts.partitionNames)) {
            const qsizetype index = available.indexOf(name);
            if (index < 0) {
                return usageError(QStringLiteral("--partitions: %1 has no partition \"%2\"; it has %3")
                                      .arg(QFileInfo(job->plannedImage()).fileName(), name, available.join(QStringLiteral(", "))));
            }
            const QString problem = FirmwareJob::partitionProblem(job->partitions().at(index), known);
            if (!problem.isEmpty()) {
                return usageError(QStringLiteral("--partitions: %1 cannot be written: %2").arg(name, problem));
            }
        }
        job->setSelectedPartitions(opts.partitionNames);
    }
    resultFields.insert(QStringLiteral("image"), nativePath(job->imagePath()));
    resultFields.insert(QStringLiteral("partitions"), QJsonArray::fromStringList(job->selectedPartitions()));
    runOperation(QString(FirmwareJob::kOpFlash), [this]() { job->flashSoc(); });
    const int code = operationExitCode();
    resultMessage = operationResultMessage();
    const SocFlasher::FlashMethod method = job->socFlasher()->flashMethod();
    resultFields.insert(QStringLiteral("method"), method == SocFlasher::FlashMethod::PartitionWrites   ? QStringLiteral("partition-writes")
                                                  : method == SocFlasher::FlashMethod::UpgradeFirmware ? QStringLiteral("upgrade-firmware")
                                                                                                       : QString());
    if (!job->boardInfo().isEmpty()) {
        resultFields.insert(QStringLiteral("boardInfo"), job->boardInfo());
    }
    return code;
}

int FirmwareCli::Private::cmdMcu()
{
    if (!opts.package.isEmpty()) {
        job->setPackagePath(opts.package);   // checked in parseInputs(); logs the package summary
    }
    if (opts.role != 0) {
        job->setRole(opts.role);
    }
    const int board = requireRunningBoard();
    if (board != ExitOk) {
        if (board == ExitCancelled) {
            resultMessage = timedOut ? QStringLiteral("timeout after %1 s").arg(timeoutSeconds) : QStringLiteral("interrupted");
        }
        return board;
    }
    if (commandName == kMcuInfo) {
        runOperation(QString(FirmwareJob::kOpInfo), [this]() { job->readMcuInfo(); });
    } else if (commandName == kMcuUpdate) {
        runOperation(QString(FirmwareJob::kOpMcu), [this]() { job->updateMcu(); });
    } else {
        runOperation(QString(FirmwareJob::kOpSetRole), [this]() { job->setMcuRole(); });
    }
    const int code = operationExitCode();
    resultMessage = operationResultMessage();
    resultFields.insert(QStringLiteral("mcu"), mcuJson());
    return code;
}

int FirmwareCli::Private::cmdPullFlash()
{
    configurePull();
    applyLayout();
    if (!opts.package.isEmpty()) {
        job->setPackagePath(opts.package);
    }
    if (opts.role != 0) {
        job->setRole(opts.role);
        // The chain pre-selects what a running MCU reports; --role only covers an MCU that cannot say.
        const quint32 wanted = opts.role;
        QObject::connect(job.get(), &FirmwareJob::roleChanged, q, [this, wanted](quint32 reported) {
            if (reported != 0 && reported != wanted) {
                log(QStringLiteral("The MCU reports role %1: --role %2 is not used (su-fw mcu-set-role changes the role)")
                        .arg(FirmwareJob::roleText(reported), FirmwareJob::roleText(wanted)));
            }
        });
    }
    runOperation(QString(FirmwareJob::kOpPullFlash), [this]() { job->pullAndFlash(); });
    const int code = operationExitCode();
    resultMessage = operationResultMessage();
    if (failedStep == FirmwareJob::kOpMcu && failedMessage.contains(QLatin1String("no role is confirmed"))) {
        // The job's wording is the tab's ("choose rx or tx, then press Update MCU"); here it is an option.
        log(QStringLiteral("The MCU cannot report its role (it may sit in its UART updater): pass --role rx|tx, the role it "
                           "should run with"));
    }
    const ImagePuller::CacheEntry entry = job->cacheEntry();
    if (entry.isValid()) {
        resultFields.insert(QStringLiteral("image"), imageJson(entry));
    }
    if (!job->boardInfo().isEmpty()) {
        resultFields.insert(QStringLiteral("boardInfo"), job->boardInfo());
    }
    if (!job->runningMcuVersion().isEmpty()) {
        resultFields.insert(QStringLiteral("mcu"), mcuJson());
    }
    return code;
}

// =======================================================================================
// Questions
// =======================================================================================

FirmwareJob::HostKeyDecision FirmwareCli::Private::decideHostKey(const SshConnection::HostKeyInfo& info)
{
    const QString who = QStringLiteral("%1:%2").arg(info.host).arg(info.port);
    const QString key = QStringLiteral("%1 %2").arg(info.keyType, info.fingerprintSha256);
    const QString knownHosts = nativePath(info.knownHostsFile.isEmpty() ? SshConnection::defaultKnownHostsFile() : info.knownHostsFile);
    const bool changed = info.status == SshConnection::HostKeyStatus::Changed;
    log(changed ? QStringLiteral("The host key of %1 has CHANGED (now %2): someone may be intercepting the connection, "
                                 "or the build machine was reinstalled")
                      .arg(who, key)
                : QStringLiteral("The host key of %1 is not known yet (%2)").arg(who, key));
    if (canAsk()) {
        writeErr(changed ? QStringLiteral("Replace the stored host key of %1 and connect? [y] yes, once / [a] yes, and remember it / [N] no: ")
                               .arg(who)
                         : QStringLiteral("Trust this host key? [y] yes, once / [a] yes, and remember it in %1 / [N] no: ").arg(knownHosts));
        asking = true;
        const std::optional<QString> typed = lineReader(/*secret=*/false);
        asking = false;
        const QString answer = typed ? typed->trimmed().toLower() : QString();
        if (answer == QLatin1String("a") || answer == QLatin1String("always")) {
            log(QStringLiteral("Host key accepted and remembered in %1").arg(knownHosts));
            return {true, true};
        }
        if (answer == QLatin1String("y") || answer == QLatin1String("yes")) {
            log(QStringLiteral("Host key accepted for this connection"));
            return {true, false};
        }
        log(QStringLiteral("Host key refused"));
        return {false, false};
    }
    if (!changed && opts.acceptHostKey) {
        log(QStringLiteral("Host key accepted (--accept-host-key) and remembered in %1").arg(knownHosts));
        return {true, true};
    }
    log(changed ? QStringLiteral("Refused: a changed host key is never accepted without asking; if the build machine was "
                                 "reinstalled, remove its old line from %1 (or connect once from the app)")
                      .arg(knownHosts)
                : QStringLiteral("Refused: pass --accept-host-key to trust it (it is then remembered in %1), or connect once "
                                 "from the app")
                      .arg(knownHosts));
    return {false, false};
}

std::optional<FirmwareJob::AuthAnswer> FirmwareCli::Private::answerPrompt(const SshConnection::AuthPrompt& prompt)
{
    const QString question = prompt.prompt.trimmed().isEmpty() ? prompt.title.trimmed() : prompt.prompt.trimmed();
    if (secretValue) {
        if (!secretUsed) {
            secretUsed = true;
            log(QStringLiteral("Answering \"%1\" with the secret from %2").arg(question, secretSource));
            return FirmwareJob::AuthAnswer{*secretValue, false};
        }
        log(QStringLiteral("\"%1\" was asked again: the secret from %2 was not accepted").arg(question, secretSource));
        return std::nullopt;
    }
    if (!canAsk()) {
        return std::nullopt;
    }
    writeErr(question + QLatin1Char(' '));
    asking = true;
    const std::optional<QString> typed = lineReader(/*secret=*/!prompt.echo);
    asking = false;
    if (!typed) {
        return std::nullopt;
    }
    return FirmwareJob::AuthAnswer{*typed, false};   // su-fw never stores a secret
}

// =======================================================================================
// FirmwareCli
// =======================================================================================

FirmwareCli::FirmwareCli(ToolRunner* runner, QObject* parent)
    : QObject(parent)
    , d(std::make_unique<Private>(this, runner))
{
    d->timeoutTimer.setSingleShot(true);
    connect(&d->timeoutTimer, &QTimer::timeout, this, [this]() {
        d->stopRun(QStringLiteral("Timeout: %1 s passed (--timeout); cancelling").arg(d->timeoutSeconds), &d->timedOut);
    });
}

FirmwareCli::~FirmwareCli() = default;

int FirmwareCli::run(const QStringList& arguments, QIODevice* out, QIODevice* err)
{
    if (d->running) {
        writeTo(err, QByteArrayLiteral("su-fw: already running\n"));
        return ExitUsage;
    }
    d->beginRun(out, err);
    const int code = d->execute(arguments);
    d->endRun();
    return code;
}

bool FirmwareCli::isRunning() const
{
    return d->running;
}

void FirmwareCli::setInput(LineReader reader, bool interactive)
{
    d->lineReader = std::move(reader);
    d->inputInteractive = interactive;
}

void FirmwareCli::setProfileStore(SshProfileStore* store)
{
    d->givenStore = store;
}

void FirmwareCli::setToolPaths(const QString& upgradeTool, const QString& adb)
{
    d->toolsFixed = true;
    d->fixedUpgradeTool = upgradeTool;
    d->fixedAdb = adb;
}

void FirmwareCli::setJobSetup(std::function<void(FirmwareJob& job)> setup)
{
    d->jobSetup = std::move(setup);
}

void FirmwareCli::interrupt()
{
    if (!d->running) {
        return;
    }
    if (!d->job) {
        // The command line is still being read (the password from stdin): remembered, and
        // execute() starts nothing.
        d->interrupted = true;
        return;
    }
    if (d->asking) {
        // A question inside one of the job's callbacks: the cancel goes after it, from the event loop.
        QMetaObject::invokeMethod(this, &FirmwareCli::interrupt, Qt::QueuedConnection);
        return;
    }
    d->stopRun(QStringLiteral("Interrupted (Ctrl+C)"), &d->interrupted);
}

QStringList FirmwareCli::commands()
{
    return {QString(kStatus), QString(kPartitions), QString(kPull), QString(kFlashSoc),
            QString(kMcuInfo), QString(kMcuUpdate), QString(kMcuSetRole), QString(kPullFlash)};
}

QStringList FirmwareCli::commandOptions(const QString& command)
{
    if (!commands().contains(command)) {
        return {};
    }
    return specificOptionNames(command) + commonOptionNames();
}

QString FirmwareCli::helpText(const QString& command)
{
    const QString common = QString::fromLatin1(kCommonOptionsHelp);
    if (command.isEmpty()) {
        return QString::fromLatin1(kGlobalHelp) + common + QLatin1Char('\n') + QString::fromLatin1(kOutputHelp) + QLatin1Char('\n')
               + QString::fromLatin1(kExitCodesHelp) + QString::fromLatin1(kGlobalHelpTail);
    }
    const auto page = [&common](const char* head, const char* tail) {
        return QString::fromLatin1(head) + common + QString::fromLatin1(tail);
    };
    if (command == kStatus) {
        return page(kStatusHelp, kStatusTail);
    }
    if (command == kPartitions) {
        return page(kPartitionsHelp, kPartitionsTail);
    }
    if (command == kPull) {
        return QString::fromLatin1(kPullHelp) + QString::fromLatin1(kPullOptionsHelp) + common + QString::fromLatin1(kPullTail);
    }
    if (command == kFlashSoc) {
        return page(kFlashSocHelp, kFlashSocTail);
    }
    if (command == kMcuInfo) {
        return page(kMcuInfoHelp, kMcuInfoTail);
    }
    if (command == kMcuUpdate) {
        return page(kMcuUpdateHelp, kMcuUpdateTail);
    }
    if (command == kMcuSetRole) {
        return page(kMcuSetRoleHelp, kMcuSetRoleTail);
    }
    if (command == kPullFlash) {
        return QString::fromLatin1(kPullFlashHelp) + QString::fromLatin1(kPullOptionsHelp) + QString::fromLatin1(kPullFlashOptionsHelp)
               + common + QString::fromLatin1(kPullFlashTail);
    }
    return QString();
}

QString FirmwareCli::versionText()
{
    return QStringLiteral("su-fw %1 (BuildAI Serial Utility, %2)").arg(QStringLiteral(APP_VERSION), QStringLiteral(APP_GIT_HASH));
}
