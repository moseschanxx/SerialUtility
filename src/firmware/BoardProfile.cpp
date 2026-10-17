#include "firmware/BoardProfile.h"

#include <QCoreApplication>

#include "firmware/FleetFlasher.h"
#include "firmware/RockchipTool.h"

namespace {

/// The PID as the evidence and the messages write it: "0x110c".
QString pidText(quint16 pid)
{
    return QStringLiteral("0x%1").arg(pid, 4, 16, QLatin1Char('0'));
}

/// The partition names of a layout in order; empty when it does not parse.
QStringList layoutNames(const QString& mtdparts)
{
    QStringList names;
    for (const MtdPartition& partition : RockchipTool::parseMtdParts(mtdparts)) {
        names << partition.name;
    }
    return names;
}

/// A layout as the rule texts quote it: without "mtdparts=" and the surrounding blanks.
QString quotedLayout(const QString& mtdparts)
{
    QString text = mtdparts.trimmed();
    if (text.startsWith(QLatin1String("mtdparts="))) {
        text = text.mid(9).trimmed();
    }
    return text;
}

/// Every field of both profiles is a fact of docs/firmware-update/RV1106G2_FACTS.md (the rows are
/// given in BoardProfile.h); the names are product names and stay untranslated, like the ids.
QList<BoardProfile> buildTable()
{
    BoardProfile node;
    node.id = QLatin1String(BoardProfiles::kNodeId);
    node.name = QStringLiteral("Node (RV1106BG2)");
    node.soc = QStringLiteral("RV1106BG2");
    node.chipTags = {QStringLiteral("110E")};
    node.loaderPids = {0x110e};
    node.adbProducts = {QStringLiteral("rv1106bg2-gc2083")};
    node.mtdParts = QStringLiteral("sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),3M(boot),5M(rootfs),3M(userdata)");
    node.dailyPartitions = {QStringLiteral("rootfs")};
    node.maskromEntry = BoardProfile::MaskromEntry::Button;
    node.cc1310ResetCutsSocPower = false;
    node.loaderWaitSeconds = 30;

    BoardProfile stacked;
    stacked.id = QLatin1String(BoardProfiles::kStackedId);
    stacked.name = QStringLiteral("Stacked (RV1106G2)");
    stacked.soc = QStringLiteral("RV1106G2");
    stacked.chipTags = {QStringLiteral("1106")};
    stacked.loaderPids = {0x110c};
    stacked.adbProducts = {QStringLiteral("rv1106g-stacked-gc2083")};
    stacked.mtdParts = QStringLiteral("sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),12M(boot),3M(userdata)");
    stacked.dailyPartitions = {QStringLiteral("boot")};   // the erofs rootfs lives inside boot (row 11)
    stacked.maskromEntry = BoardProfile::MaskromEntry::Unknown;
    stacked.cc1310ResetCutsSocPower = true;
    stacked.loaderWaitSeconds = 45;

    return {node, stacked};
}

} // namespace

// ---------------------------------------------------------------------------------------------
// BoardProfile
// ---------------------------------------------------------------------------------------------

bool BoardProfile::acceptsChipTag(const QString& tag) const
{
    const QString wanted = tag.trimmed();
    if (wanted.isEmpty()) {
        return false;
    }
    for (const QString& accepted : chipTags) {
        if (accepted.compare(wanted, Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool BoardProfile::hasLoaderPid(quint16 pid) const
{
    return pid != 0 && loaderPids.contains(pid);
}

bool BoardProfile::hasAdbProduct(const QString& product) const
{
    const QString wanted = product.trimmed();
    return !wanted.isEmpty() && adbProducts.contains(wanted);
}

QString BoardProfile::maskromEntryText() const
{
    return maskromEntry == MaskromEntry::Button ? QStringLiteral("button") : QStringLiteral("unknown");
}

QString BoardProfile::maskromInstructions(int waitSeconds) const
{
    if (maskromEntry == MaskromEntry::Button) {
        return QCoreApplication::translate("BoardProfile",
                                           "No board found: hold the MASKROM button while plugging in the USB-C cable "
                                           "(or while power-cycling the board); waiting up to %1 s")
            .arg(waitSeconds);
    }
    return QCoreApplication::translate("BoardProfile",
                                       "No board found: the %1 board has no documented MASKROM entry: put it into Loader "
                                       "mode from a running system (adb reboot loader, or \"reboot loader\" on its console); "
                                       "waiting up to %2 s")
        .arg(name)
        .arg(waitSeconds);
}

QString BoardProfile::loaderWaitHint() const
{
    if (!cc1310ResetCutsSocPower) {
        return QString();
    }
    return QCoreApplication::translate("BoardProfile",
                                       "; on the stacked board the CC1310 may have cut the SoC's power: power-cycle it, then retry");
}

bool BoardProfile::operator==(const BoardProfile& other) const
{
    return id == other.id && name == other.name && soc == other.soc && chipTags == other.chipTags && loaderPids == other.loaderPids
           && adbProducts == other.adbProducts && mtdParts == other.mtdParts && dailyPartitions == other.dailyPartitions
           && maskromEntry == other.maskromEntry && cc1310ResetCutsSocPower == other.cc1310ResetCutsSocPower
           && loaderWaitSeconds == other.loaderWaitSeconds;
}

// ---------------------------------------------------------------------------------------------
// ProfileDetection
// ---------------------------------------------------------------------------------------------

QString ProfileDetection::sourceText() const
{
    switch (source) {
    case Source::None:
        break;
    case Source::Option:
        return QCoreApplication::translate("BoardProfile", "set by --profile %1 (or the Board profile box)").arg(evidence);
    case Source::ChipTag:
        return QCoreApplication::translate("BoardProfile", "from the image's chip tag %1").arg(evidence);
    case Source::LoaderPid:
        return QCoreApplication::translate("BoardProfile", "from the board's Loader PID %1").arg(evidence);
    case Source::AdbProduct:
        return QCoreApplication::translate("BoardProfile", "from the ADB gadget's product string %1").arg(evidence);
    case Source::BoardLayout:
        return QCoreApplication::translate("BoardProfile", "from the board's flash layout (%1)").arg(evidence);
    case Source::ImageLayout:
        return QCoreApplication::translate("BoardProfile", "from the image's env.img layout (%1)").arg(evidence);
    }
    return QString();
}

QString ProfileDetection::text() const
{
    if (!known()) {
        return QCoreApplication::translate("BoardProfile",
                                           "profile: unknown (no --profile, no Loader PID, product string or flash layout matched "
                                           "a profile)");
    }
    return QCoreApplication::translate("BoardProfile", "profile: %1 - %2").arg(profile.name, sourceText());
}

bool ProfileDetection::operator==(const ProfileDetection& other) const
{
    return profile == other.profile && source == other.source && evidence == other.evidence;
}

// ---------------------------------------------------------------------------------------------
// BoardProfiles
// ---------------------------------------------------------------------------------------------

namespace BoardProfiles {

const QList<BoardProfile>& all()
{
    static const QList<BoardProfile> table = buildTable();
    return table;
}

QStringList ids()
{
    QStringList list;
    for (const BoardProfile& profile : all()) {
        list << profile.id;
    }
    return list;
}

std::optional<BoardProfile> byId(const QString& id)
{
    const QString wanted = id.trimmed();
    if (wanted.isEmpty()) {
        return std::nullopt;
    }
    for (const BoardProfile& profile : all()) {
        if (profile.id.compare(wanted, Qt::CaseInsensitive) == 0) {
            return profile;
        }
    }
    return std::nullopt;
}

std::optional<BoardProfile> byChipTag(const QString& tag)
{
    for (const BoardProfile& profile : all()) {
        if (profile.acceptsChipTag(tag)) {
            return profile;
        }
    }
    return std::nullopt;
}

std::optional<BoardProfile> byLoaderPid(quint16 pid)
{
    for (const BoardProfile& profile : all()) {
        if (profile.hasLoaderPid(pid)) {
            return profile;
        }
    }
    return std::nullopt;
}

std::optional<BoardProfile> byAdbProduct(const QString& product)
{
    for (const BoardProfile& profile : all()) {
        if (profile.hasAdbProduct(product)) {
            return profile;
        }
    }
    return std::nullopt;
}

std::optional<BoardProfile> byLayout(const QString& mtdparts)
{
    // The family is told by the names in their order; sizes and offsets may change with a build.
    const QStringList names = layoutNames(mtdparts);
    if (names.isEmpty()) {
        return std::nullopt;
    }
    for (const BoardProfile& profile : all()) {
        if (layoutNames(profile.mtdParts) == names) {
            return profile;
        }
    }
    return std::nullopt;
}

QString layoutSummary(const QString& mtdparts)
{
    const QStringList names = layoutNames(mtdparts);
    if (names.isEmpty()) {
        return QString();
    }
    return QStringLiteral("%1 partitions: %2").arg(names.size()).arg(names.join(QStringLiteral(", ")));
}

ProfileDetection detectBoard(const ProfileEvidence& evidence)
{
    ProfileDetection detection;
    if (const std::optional<BoardProfile> chosen = byId(evidence.overrideId)) {
        detection.profile = *chosen;
        detection.source = ProfileDetection::Source::Option;
        detection.evidence = chosen->id;
        return detection;
    }
    if (const std::optional<BoardProfile> fromPid = byLoaderPid(evidence.loaderPid)) {
        detection.profile = *fromPid;
        detection.source = ProfileDetection::Source::LoaderPid;
        detection.evidence = pidText(evidence.loaderPid);
        return detection;
    }
    if (const std::optional<BoardProfile> fromProduct = byAdbProduct(evidence.adbProduct)) {
        detection.profile = *fromProduct;
        detection.source = ProfileDetection::Source::AdbProduct;
        detection.evidence = evidence.adbProduct.trimmed();
        return detection;
    }
    if (const std::optional<BoardProfile> fromLayout = byLayout(evidence.layout)) {
        detection.profile = *fromLayout;
        detection.source = ProfileDetection::Source::BoardLayout;
        detection.evidence = layoutSummary(evidence.layout);
        return detection;
    }
    return detection;
}

ProfileDetection detectImage(const ProfileEvidence& evidence)
{
    // The override says which board is attached; the image says what it is for: never mixed.
    ProfileDetection detection;
    if (const std::optional<BoardProfile> fromTag = byChipTag(evidence.chipTag)) {
        detection.profile = *fromTag;
        detection.source = ProfileDetection::Source::ChipTag;
        detection.evidence = evidence.chipTag.trimmed().toUpper();
        return detection;
    }
    if (const std::optional<BoardProfile> fromLayout = byLayout(evidence.layout)) {
        detection.profile = *fromLayout;
        detection.source = ProfileDetection::Source::ImageLayout;
        detection.evidence = layoutSummary(evidence.layout);
        return detection;
    }
    return detection;
}

} // namespace BoardProfiles

// ---------------------------------------------------------------------------------------------
// ProfileRules
// ---------------------------------------------------------------------------------------------

namespace ProfileRules {

QString imageRefusal(const ProfileDetection& board, const ProfileDetection& image)
{
    if (!board.known() || !image.known() || board.profile.id == image.profile.id) {
        return QString();
    }
    QString what;
    switch (image.source) {
    case ProfileDetection::Source::ChipTag:
        what = QCoreApplication::translate("ProfileRules", "its chip tag %1").arg(image.evidence);
        break;
    case ProfileDetection::Source::ImageLayout:
        what = QCoreApplication::translate("ProfileRules", "its env.img layout (%1)").arg(image.evidence);
        break;
    default:
        what = QCoreApplication::translate("ProfileRules", "it");
        break;
    }
    return QCoreApplication::translate("ProfileRules",
                                       "the image is for another board: %1 is the %2's, the board is %3 (%4); nothing was written")
        .arg(what, image.profile.name, board.profile.name, board.sourceText());
}

QString layoutRefusal(const QString& imageLayout, const QString& boardLayout, bool forced, QString* note)
{
    if (note) {
        note->clear();
    }
    const QList<MtdPartition> image = RockchipTool::parseMtdParts(imageLayout);
    const QList<MtdPartition> board = RockchipTool::parseMtdParts(boardLayout);
    if (image.isEmpty() || board.isEmpty() || image == board) {
        return QString();
    }
    const QString x = quotedLayout(imageLayout);
    const QString y = quotedLayout(boardLayout);
    if (forced) {
        if (note) {
            *note = QCoreApplication::translate("ProfileRules",
                                                "warning: the image's flash layout differs from the board's; the image's is used "
                                                "(--force-layout). image: %1; board: %2")
                        .arg(x, y);
        }
        return QString();
    }
    return QCoreApplication::translate("ProfileRules",
                                       "the image's flash layout differs from the board's (image: %1; board: %2): refused, the "
                                       "image's layout would be written over a board laid out differently; --force-layout (the "
                                       "tab: confirm the question) writes it anyway; nothing was written")
        .arg(x, y);
}

QString loaderPartitionRefusal(const BoardProfile& board, const QStringList& names, bool forced, QString* note)
{
    if (note) {
        note->clear();
    }
    if (!board.isValid() || board.maskromEntry != BoardProfile::MaskromEntry::Unknown) {
        return QString();
    }
    const QStringList loader = FleetFlasher::bootloaderPartitions();
    QStringList hit;
    for (const QString& name : names) {
        if (loader.contains(name) && !hit.contains(name)) {
            hit << name;   // row order
        }
    }
    if (hit.isEmpty()) {
        return QString();
    }
    const QString list = hit.join(QStringLiteral(", "));
    if (forced) {
        if (note) {
            *note = QCoreApplication::translate("ProfileRules",
                                                "warning: writing loader partition(s) %1 on a %2 board without a documented "
                                                "MASKROM entry (--force-loader-partitions)")
                        .arg(list, board.name);
        }
        return QString();
    }
    return QCoreApplication::translate("ProfileRules",
                                       "%1: loader partition(s) on a %2 board, whose MASKROM entry is not documented: a bad "
                                       "write could not be recovered; refused until the MASKROM route is known - "
                                       "--force-loader-partitions (the tab: confirm the question) writes them anyway; nothing was "
                                       "written")
        .arg(list, board.name);
}

QString socPowerRefusal(const BoardProfile& board, bool confirmed, QString* note)
{
    if (note) {
        note->clear();
    }
    if (!board.isValid() || !board.cc1310ResetCutsSocPower) {
        return QString();
    }
    if (confirmed) {
        if (note) {
            *note = QCoreApplication::translate("ProfileRules",
                                                "warning: the CC1310 of a %1 board holds the SoC's power; proceeding as confirmed "
                                                "(--confirm-soc-power)")
                        .arg(board.name);
        }
        return QString();
    }
    return QCoreApplication::translate("ProfileRules",
                                       "the CC1310 of a %1 board holds the SoC's power (SOC_PWR_EN): rebooting it into its "
                                       "bootloader can switch the SoC off and cut the update; refused until confirmed - "
                                       "--confirm-soc-power (the tab: confirm the question) once the carrier's CC1310 bootloader "
                                       "is known to hold SOC_PWR_EN (the 14 Oct 2026 build or later)")
        .arg(board.name);
}

QString mixedBenchStop(const QString& serial, const ProfileDetection& board, const ProfileDetection& plan)
{
    if (!board.known() || !plan.known() || board.profile.id == plan.profile.id) {
        return QString();
    }
    return QCoreApplication::translate("ProfileRules",
                                       "mixed bench: board %1 is %2 (%3) but the plan is for %4 (%5); one profile per run: name "
                                       "boards of one profile with --serials")
        .arg(serial, board.profile.name, board.sourceText(), plan.profile.name, plan.sourceText());
}

QString overrideRefusal(const QString& overrideId, quint16 loaderPid)
{
    const std::optional<BoardProfile> claimed = BoardProfiles::byId(overrideId);
    const std::optional<BoardProfile> actual = BoardProfiles::byLoaderPid(loaderPid);
    if (!claimed || !actual || claimed->id == actual->id) {
        return QString();
    }
    return QCoreApplication::translate("ProfileRules",
                                       "the --profile %1 override contradicts the board's Loader PID %2 (the %3's); drop --profile")
        .arg(claimed->id, pidText(loaderPid), actual->name);
}

} // namespace ProfileRules
