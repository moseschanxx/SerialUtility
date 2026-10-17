#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <optional>

/**
 * One board family the tool knows (v0.8.0; docs/firmware-update/RV1106G2_FACTS.md section 4,
 * docs/DESIGN.md 4.14): the facts that tell a Node (RV1106BG2) from a Stacked core (RV1106G2) on
 * the PC side, and the rules a flash has to follow on each. A value type filled from one static
 * table (BoardProfiles::all()); a JSON file is NOT a 0.8.0 feature. Every field is a fact of
 * RV1106G2_FACTS.md (the row is given), nothing is measured by the tool:
 *
 *  | field                   | Node (RV1106BG2)                      | Stacked (RV1106G2)                        | row |
 *  |-------------------------|---------------------------------------|-------------------------------------------|-----|
 *  | id / name / soc         | "node" / "Node (RV1106BG2)" / RV1106BG2 | "stacked" / "Stacked (RV1106G2)" / RV1106G2 | 1 |
 *  | chipTags (RKFW / SFI)   | {"110E"}                              | {"1106"}                                  | 20  |
 *  | loaderPids (U-Boot rockusb) | {0x110e}                          | {0x110c}                                  | 32  |
 *  | adbProducts (gadget)    | {"rv1106bg2-gc2083"}                  | {"rv1106g-stacked-gc2083"}                | 29  |
 *  | mtdParts (expected)     | sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),3M(boot),5M(rootfs),3M(userdata) | sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),12M(boot),3M(userdata) | 9 |
 *  | dailyPartitions         | {"rootfs"}                            | {"boot"} (the erofs rootfs lives inside)  | 11  |
 *  | maskromEntry            | Button                                | Unknown (not documented anywhere)         | 42  |
 *  | cc1310ResetCutsSocPower | false                                 | true (DIO8 = SOC_PWR_EN)                  | 44  |
 *  | loaderWaitSeconds       | 30 (the Preferences default)          | 45, a recommendation only (D7)            | 40  |
 *
 * The loader partitions (idblock, uboot, env, meta) are the same on both boards and stay
 * FleetFlasher::bootloaderPartitions(): one definition, not a profile field.
 */
struct BoardProfile
{
    /// How a board gets into MASKROM (BootROM download) mode: Button = a documented button route
    /// (the Node's "hold MASKROM and replug"); Unknown = no documented entry (the stacked board, row
    /// 42): a bad idblock / uboot / env / meta write could not be recovered, so those writes are
    /// refused on such a profile unless forced (ProfileRules::loaderPartitionRefusal()).
    enum class MaskromEntry { Button, Unknown };

    QString id;                     ///< "node" / "stacked": the --profile value and the tab's combo key
    QString name;                   ///< "Node (RV1106BG2)" / "Stacked (RV1106G2)": every message names it so
    QString soc;                    ///< "RV1106BG2" / "RV1106G2"
    QStringList chipTags;           ///< the RKFW chip tags accepted on this board, upper case ("110E" / "1106")
    QList<quint16> loaderPids;      ///< the USB PID of the board's U-Boot rockusb (Loader mode); the BootROM's PID is not known (row 33)
    QStringList adbProducts;        ///< the ADB gadget's USB product string(s), as /usr/bin/usb_adb.sh writes them
    QString mtdParts;               ///< the expected flash layout (normalized: no "mtdparts=", with "sfc_nor:")
    QStringList dailyPartitions;    ///< what a daily firmware update writes: the tab's default checks and the --help examples
    MaskromEntry maskromEntry = MaskromEntry::Unknown;
    bool cc1310ResetCutsSocPower = false;   ///< a CC1310 reset drops SOC_PWR_EN: mcu-update / mcu-set-role need --confirm-soc-power
    /// The Loader wait recommended for this board (Preferences > Firmware; the Node's is the 30 s
    /// default). Informational: a profile NEVER changes the wait a flow uses (the configured one),
    /// it adds its hint to the timeout message (loaderWaitHint()) - so the fast tests with a 1 s
    /// wait on a known profile stay fast, and the user's Preferences stay the user's.
    int loaderWaitSeconds = 30;

    bool isValid() const { return !id.isEmpty(); }
    bool acceptsChipTag(const QString& tag) const;    ///< case-insensitive, trimmed; false for an empty tag
    bool hasLoaderPid(quint16 pid) const;
    bool hasAdbProduct(const QString& product) const;  ///< exact, trimmed
    QString maskromEntryText() const;                  ///< "button" / "unknown" (the inventory, the JSON)
    /// The instruction for a board that is nowhere (FirmwareJob::maskromInstructions()): the Node's
    /// "No board found: hold the MASKROM button while plugging in the USB-C cable (or while
    /// power-cycling the board); waiting up to %1 s"; the stacked board's "No board found: the
    /// Stacked (RV1106G2) board has no documented MASKROM entry: put it into Loader mode from a
    /// running system (adb reboot loader, or \"reboot loader\" on its console); waiting up to %1 s".
    QString maskromInstructions(int waitSeconds) const;
    /// "" for the Node; for the stacked board the sentence appended to "board did not enter
    /// MASKROM/Loader within %1 s": "; on the stacked board the CC1310 may have cut the SoC's power:
    /// power-cycle it, then retry" (D7).
    QString loaderWaitHint() const;
    bool operator==(const BoardProfile& other) const;
};

/**
 * What the tool saw, handed to BoardProfiles::detectBoard() / detectImage(): every field "" / 0
 * when not seen. The detection takes the first evidence in this order and names it, so the log
 * says why a profile was chosen.
 */
struct ProfileEvidence
{
    QString overrideId;     ///< --profile ID / the tab's Board profile box; "" = auto-detect
    QString chipTag;        ///< the image's RKFW chip tag (RockchipTool::readRkfwHeader(), or SFI's "Chip Tag:")
    /// The PID `upgrade_tool LD` printed for the board in LOADER mode only (RockusbDevice::pid with
    /// Mode::Loader: U-Boot rockusb's compile-time constant, row 32); 0 = none. A MASKROM device's
    /// PID is the BootROM's, which is not known on either board (row 33): the callers hand 0 in for
    /// a Maskrom status, never its PID (the test fixture's kLdMaskrom carries Pid=0x110c and must not
    /// read as a Stacked board - a BootROM PID equal to the other board's Loader PID would otherwise
    /// block the only recovery route, UF, of a bricked board).
    quint16 loaderPid = 0;
    /// The ADB gadget's USB product string ("rv1106bg2-gc2083" / "rv1106g-stacked-gc2083", row 29):
    /// from the USB enumeration where the OS reports it (UsbDeviceInfo::description on Linux), or
    /// read from the running board itself - `adb -s <serial> shell cat
    /// /sys/kernel/config/usb_gadget/rockchip/strings/0x409/product`, the configfs path usb_adb.sh
    /// writes - by FirmwareJob / SocFlasher step 2 when the board's /proc/cmdline layout matches no
    /// profile (the one evidence an ADB board on Windows can give besides its layout); "" = none.
    QString adbProduct;
    QString layout;         ///< a flash layout (mtdparts, with or without the prefix): the board's /proc/cmdline, the one read in Loader mode, the remembered one - or the image's env.img for detectImage()
};

/**
 * A profile with the evidence it rests on. known() is false when nothing matched; then every
 * rule that needs the profile is inert and the log says "profile: unknown (...)" - a layout-less
 * board or an image without a chip tag is never guessed.
 */
struct ProfileDetection
{
    enum class Source { None, Option, ChipTag, LoaderPid, AdbProduct, BoardLayout, ImageLayout };

    BoardProfile profile;           ///< isValid() false when unknown
    Source source = Source::None;
    QString evidence;               ///< the id, the tag ("1106"), the PID ("0x110c"), the product string, or the layout's partition names ("6 partitions: env, idblock, uboot, meta, boot, userdata")

    bool known() const { return profile.isValid(); }
    /// "set by --profile stacked (or the Board profile box)" / "from the image's chip tag 1106" /
    /// "from the board's Loader PID 0x110c" / "from the ADB gadget's product string
    /// rv1106g-stacked-gc2083" / "from the board's flash layout (6 partitions: env, idblock, uboot,
    /// meta, boot, userdata)" / "from the image's env.img layout (7 partitions: ...)"; "" when None.
    QString sourceText() const;
    /// The log line: "profile: Stacked (RV1106G2) - from the image's chip tag 1106"; unknown:
    /// "profile: unknown (no --profile, no Loader PID, product string or flash layout matched a
    /// profile)".
    QString text() const;
    bool operator==(const ProfileDetection& other) const;
};

namespace BoardProfiles {

constexpr const char* kNodeId = "node";
constexpr const char* kStackedId = "stacked";

const QList<BoardProfile>& all();                     ///< the static table: Node first, then Stacked
QStringList ids();                                    ///< {"node", "stacked"}: the --profile values, the tab's combo
std::optional<BoardProfile> byId(const QString& id);  ///< case-insensitive, trimmed
std::optional<BoardProfile> byChipTag(const QString& tag);
std::optional<BoardProfile> byLoaderPid(quint16 pid);
std::optional<BoardProfile> byAdbProduct(const QString& product);
/// The profile whose expected layout has the same partition NAMES in the same order as `mtdparts`
/// (sizes and offsets are not compared: a build may resize a partition, the family stays).
/// std::nullopt when the layout does not parse or matches no profile.
std::optional<BoardProfile> byLayout(const QString& mtdparts);
/// "6 partitions: env, idblock, uboot, meta, boot, userdata" for the evidence texts ("" when the
/// layout does not parse).
QString layoutSummary(const QString& mtdparts);

/// The board's profile: overrideId (an unknown id is NOT an error here - the CLI refuses it as a
/// usage error, the tab cannot produce one) > loaderPid > adbProduct > layout (BoardLayout). The
/// override wins over the product string and the layout; against a known Loader PID that names the
/// OTHER profile it does not win - the callers refuse the flow with ProfileRules::overrideRefusal()
/// before they use this result (a PID is a constant of the board's U-Boot and cannot be wrong; an
/// override is the operator's claim "when the tool cannot tell").
ProfileDetection detectBoard(const ProfileEvidence& evidence);
/// The image's profile: chipTag > layout (the env.img's, ImageLayout). The override never applies
/// to an image: it says which board is attached, the image says what it is for.
ProfileDetection detectImage(const ProfileEvidence& evidence);

} // namespace BoardProfiles

/**
 * The four refusals of v0.8.0 and the fleet's mixed-bench stop, as pure functions with one wording
 * each, so SocFlasher (the single-board flow), FirmwareJob (the fleet's inputs, the MCU
 * operations) and FleetFlasher (the profile gate) refuse with the same words. Every function
 * returns the refusal text ("" = allowed); where an override exists, `forced` turns the refusal
 * into a warning handed back in *note (logged by the caller). Texts are
 * QCoreApplication::translate("ProfileRules", ...).
 */
namespace ProfileRules {

/// Rule 1, the image against the board (replaces nothing: new). Inert unless BOTH are known.
/// Refusal: "the image is for another board: its chip tag 110E is the Node (RV1106BG2)'s, the
/// board is Stacked (RV1106G2) (from the board's Loader PID 0x110c); nothing was written" (an image
/// known by its env.img: "its env.img layout (7 partitions: ...) is the Node (RV1106BG2)'s"). No
/// override: a wrong image is never written.
QString imageRefusal(const ProfileDetection& board, const ProfileDetection& image);

/// Rule 2, the image's env.img layout against the board's (replaces 0.5.1's "the image's wins").
/// Inert unless both parse; equal layouts (RockchipTool::parseMtdParts() lists equal) pass.
/// Refusal: "the image's flash layout differs from the board's (image: <x>; board: <y>): refused,
/// the image's layout would be written over a board laid out differently; --force-layout (the tab:
/// confirm the question) writes it anyway; nothing was written". forced -> "" and *note = "warning:
/// the image's flash layout differs from the board's; the image's is used (--force-layout). image:
/// <x>; board: <y>".
QString layoutRefusal(const QString& imageLayout, const QString& boardLayout, bool forced, QString* note = nullptr);

/// Rule 3, loader partitions on a board without a documented MASKROM entry. Inert unless the
/// profile is known and its maskromEntry is Unknown; `names` are the rows about to be written, of
/// which the ones in FleetFlasher::bootloaderPartitions() count (none -> ""). Refusal: "idblock,
/// uboot: loader partition(s) on a Stacked (RV1106G2) board, whose MASKROM entry is not
/// documented: a bad write could not be recovered; refused until the MASKROM route is known -
/// --force-loader-partitions (the tab: confirm the question) writes them anyway; nothing was
/// written". forced -> "" and *note = "warning: writing loader partition(s) idblock, uboot on a
/// Stacked (RV1106G2) board without a documented MASKROM entry (--force-loader-partitions)".
QString loaderPartitionRefusal(const BoardProfile& board, const QStringList& names, bool forced, QString* note = nullptr);

/// Rule 4, the CC1310 that holds the SoC's power. Inert unless the profile is known and
/// cc1310ResetCutsSocPower - and an ADB board's profile is known from its layout OR, when the
/// layout tells nothing (a /proc/cmdline without mtdparts, open question 6), from the gadget's
/// product string FirmwareJob reads from the board (ProfileEvidence::adbProduct), so a G2 is never
/// "unknown" merely because Windows reports the driver's name. Refusal: "the CC1310 of a Stacked (RV1106G2) board holds the SoC's
/// power (SOC_PWR_EN): rebooting it into its bootloader can switch the SoC off and cut the update;
/// refused until confirmed - --confirm-soc-power (the tab: confirm the question) once the carrier's
/// CC1310 bootloader is known to hold SOC_PWR_EN (the 14 Oct 2026 build or later)". confirmed ->
/// "" and *note = "warning: the CC1310 of a Stacked (RV1106G2) board holds the SoC's power;
/// proceeding as confirmed (--confirm-soc-power)".
QString socPowerRefusal(const BoardProfile& board, bool confirmed, QString* note = nullptr);

/// The fleet's profile gate (FleetFlasher step 2 b from the layout, step 2 d from the mapped
/// Loader PID): inert unless both are known and differ. "mixed bench: board 62ecfc6ddeb3f579 is
/// Stacked (RV1106G2) (from the board's flash layout (...))" / "(from the board's Loader PID 0x110c)"
/// "but the plan is for Node (RV1106BG2) (from the image's chip tag 110E); one profile per run: name
/// boards of one profile with --serials" - the reason of a mapping stop.
QString mixedBenchStop(const QString& serial, const ProfileDetection& board, const ProfileDetection& plan);

/// The override against a known Loader PID (v0.8.0; SocFlasher step 1 / step 4, FleetFlasher at
/// Mapped). Inert unless overrideId names a profile, loaderPid is a known Loader PID
/// (byLoaderPid()) and the two profiles differ. Refusal (no override of the override): "the
/// --profile stacked override contradicts the board's Loader PID 0x110e (the Node (RV1106BG2)'s);
/// drop --profile" (the tab's box: the same text; the user clears the box). The PID is a constant
/// of the board's U-Boot; an override that contradicts it is the operator's mistake, and applying
/// it would let rule 1 pass a foreign image and rule 2's question write the other board's layout.
QString overrideRefusal(const QString& overrideId, quint16 loaderPid);

} // namespace ProfileRules
