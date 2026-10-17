// UI-free test suite for the board profiles of v0.8.0 (src/firmware/BoardProfile.h): the static
// table of the Node (RV1106BG2) and the Stacked core (RV1106G2), the lookups, the detection order
// with its evidence, and the texts of ProfileRules - pure functions, no tool, no board, no event
// loop. Every expected value is a fact of docs/firmware-update/RV1106G2_FACTS.md (the rows are in
// BoardProfile.h) or a text of the header, verbatim.
#include <QtTest>

#include "firmware/BoardProfile.h"
#include "firmware/FleetFlasher.h"
#include "support/FirmwareFixtures.h"

using namespace FirmwareFixtures;

namespace {

BoardProfile node()
{
    return *BoardProfiles::byId(QStringLiteral("node"));
}

BoardProfile stacked()
{
    return *BoardProfiles::byId(QStringLiteral("stacked"));
}

ProfileEvidence evidence(const QString& overrideId, quint16 pid, const QString& product, const QString& layout,
                         const QString& chipTag = QString())
{
    ProfileEvidence e;
    e.overrideId = overrideId;
    e.chipTag = chipTag;
    e.loaderPid = pid;
    e.adbProduct = product;
    e.layout = layout;
    return e;
}

const QString kNodeSummary = QStringLiteral("7 partitions: env, idblock, uboot, meta, boot, rootfs, userdata");
const QString kStackedSummary = QStringLiteral("6 partitions: env, idblock, uboot, meta, boot, userdata");
const QString kUnknownText =
    QStringLiteral("profile: unknown (no --profile, no Loader PID, product string or flash layout matched a profile)");

} // namespace

class Tst_boardprofile : public QObject
{
    Q_OBJECT

private slots:
    void tableFacts();
    void byIdCaseInsensitive();
    void byChipTag();
    void byLoaderPid();
    void byAdbProduct();
    void byLayoutMatchesNames();
    void layoutSummary();
    void detectBoardOrder();
    void detectBoardNothing();
    void detectImage();
    void detectionTexts();
    void imageRefusal();
    void layoutRefusal();
    void loaderPartitionRefusal();
    void socPowerRefusal();
    void mixedBenchStop();
    void maskromInstructionsAndWaitHint();
    void recordOperatorsAndValidity();
    void overrideRefusal();
};

void Tst_boardprofile::tableFacts()
{
    const QList<BoardProfile>& all = BoardProfiles::all();
    QCOMPARE(all.size(), qsizetype(2));
    QCOMPARE(BoardProfiles::ids(), (QStringList{QStringLiteral("node"), QStringLiteral("stacked")}));
    QCOMPARE(QString::fromLatin1(BoardProfiles::kNodeId), QStringLiteral("node"));
    QCOMPARE(QString::fromLatin1(BoardProfiles::kStackedId), QStringLiteral("stacked"));

    const BoardProfile& n = all.at(0);   // Node first (rows 1, 9, 11, 20, 29, 32, 40, 42, 44)
    QCOMPARE(n.id, QStringLiteral("node"));
    QCOMPARE(n.name, QStringLiteral("Node (RV1106BG2)"));
    QCOMPARE(n.soc, QStringLiteral("RV1106BG2"));
    QCOMPARE(n.chipTags, QStringList{QStringLiteral("110E")});
    QCOMPARE(n.loaderPids, QList<quint16>{0x110e});
    QCOMPARE(n.adbProducts, QStringList{QStringLiteral("rv1106bg2-gc2083")});
    QCOMPARE(n.mtdParts, kNodeMtdParts);   // the normalized form of the board's own /proc/cmdline
    QCOMPARE(n.dailyPartitions, QStringList{QStringLiteral("rootfs")});
    QCOMPARE(n.maskromEntry, BoardProfile::MaskromEntry::Button);
    QCOMPARE(n.cc1310ResetCutsSocPower, false);
    QCOMPARE(n.loaderWaitSeconds, 30);

    const BoardProfile& s = all.at(1);
    QCOMPARE(s.id, QStringLiteral("stacked"));
    QCOMPARE(s.name, QStringLiteral("Stacked (RV1106G2)"));
    QCOMPARE(s.soc, QStringLiteral("RV1106G2"));
    QCOMPARE(s.chipTags, QStringList{QStringLiteral("1106")});
    QCOMPARE(s.loaderPids, QList<quint16>{0x110c});
    QCOMPARE(s.adbProducts, QStringList{QStringLiteral("rv1106g-stacked-gc2083")});
    QCOMPARE(s.mtdParts, kStackedMtdParts);
    QCOMPARE(s.mtdParts, QStringLiteral("sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),12M(boot),3M(userdata)"));
    QCOMPARE(s.dailyPartitions, QStringList{QStringLiteral("boot")});
    QCOMPARE(s.maskromEntry, BoardProfile::MaskromEntry::Unknown);
    QCOMPARE(s.cc1310ResetCutsSocPower, true);
    QCOMPARE(s.loaderWaitSeconds, 45);

    // The layouts are what the parser reads (row 10: boot 0xF0000 / 12 MiB on the stacked board).
    const QList<MtdPartition> parts = RockchipTool::parseMtdParts(s.mtdParts);
    QCOMPARE(parts.size(), qsizetype(6));
    QCOMPARE(parts.at(4).name, QStringLiteral("boot"));
    QCOMPARE(parts.at(4).offset, quint64(0xF0000));
    QCOMPARE(parts.at(4).size, quint64(0xC00000));
    QCOMPARE(parts.at(5).offset, quint64(0xCF0000));
    QCOMPARE(RockchipTool::parseMtdParts(n.mtdParts).size(), qsizetype(7));
}

void Tst_boardprofile::byIdCaseInsensitive()
{
    QCOMPARE(BoardProfiles::byId(QStringLiteral("node"))->id, QStringLiteral("node"));
    QCOMPARE(BoardProfiles::byId(QStringLiteral("Node "))->id, QStringLiteral("node"));
    QCOMPARE(BoardProfiles::byId(QStringLiteral("STACKED"))->id, QStringLiteral("stacked"));
    QVERIFY(!BoardProfiles::byId(QStringLiteral("x")));
    QVERIFY(!BoardProfiles::byId(QString()));
}

void Tst_boardprofile::byChipTag()
{
    QCOMPARE(BoardProfiles::byChipTag(QStringLiteral("110E"))->id, QStringLiteral("node"));
    QCOMPARE(BoardProfiles::byChipTag(QStringLiteral("110e"))->id, QStringLiteral("node"));
    QCOMPARE(BoardProfiles::byChipTag(QStringLiteral(" 1106 "))->id, QStringLiteral("stacked"));
    QVERIFY(!BoardProfiles::byChipTag(QString()));
    QVERIFY(!BoardProfiles::byChipTag(QStringLiteral("3399")));
    QVERIFY(!node().acceptsChipTag(QString()));
    QVERIFY(stacked().acceptsChipTag(QStringLiteral("1106")));
}

void Tst_boardprofile::byLoaderPid()
{
    QCOMPARE(BoardProfiles::byLoaderPid(0x110e)->id, QStringLiteral("node"));
    QCOMPARE(BoardProfiles::byLoaderPid(0x110c)->id, QStringLiteral("stacked"));
    QVERIFY(!BoardProfiles::byLoaderPid(0x0006));   // the ADB gadget
    QVERIFY(!BoardProfiles::byLoaderPid(0));
    QVERIFY(!node().hasLoaderPid(0));
}

void Tst_boardprofile::byAdbProduct()
{
    QCOMPARE(BoardProfiles::byAdbProduct(QStringLiteral("rv1106bg2-gc2083"))->id, QStringLiteral("node"));
    QCOMPARE(BoardProfiles::byAdbProduct(QStringLiteral("rv1106g-stacked-gc2083"))->id, QStringLiteral("stacked"));
    QCOMPARE(BoardProfiles::byAdbProduct(QStringLiteral("  rv1106g-stacked-gc2083\n"))->id, QStringLiteral("stacked"));
    QVERIFY(!BoardProfiles::byAdbProduct(QString()));
    QVERIFY(!BoardProfiles::byAdbProduct(QStringLiteral("rv1106")));
    QVERIFY(!BoardProfiles::byAdbProduct(QStringLiteral("Android ADB Interface")));   // what Windows reports
}

void Tst_boardprofile::byLayoutMatchesNames()
{
    QCOMPARE(BoardProfiles::byLayout(kNodeMtdParts)->id, QStringLiteral("node"));
    QCOMPARE(BoardProfiles::byLayout(kStackedMtdParts)->id, QStringLiteral("stacked"));
    // A build may resize a partition: the family is told by the names.
    QCOMPARE(BoardProfiles::byLayout(QString(kStackedMtdParts).replace(QStringLiteral("12M(boot)"), QStringLiteral("11M(boot)")))->id,
             QStringLiteral("stacked"));
    QCOMPARE(BoardProfiles::byLayout(QStringLiteral("mtdparts=") + kNodeMtdParts)->id, QStringLiteral("node"));
    QVERIFY(!BoardProfiles::byLayout(
        QStringLiteral("sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),3M(rootfs),5M(boot),3M(userdata)")));   // reordered
    QVERIFY(!BoardProfiles::byLayout(kStackedMtdParts + QStringLiteral(",64K(spare)")));   // an extra partition
    QVERIFY(!BoardProfiles::byLayout(QStringLiteral("garbage")));
    QVERIFY(!BoardProfiles::byLayout(QString()));
}

void Tst_boardprofile::layoutSummary()
{
    QCOMPARE(BoardProfiles::layoutSummary(kNodeMtdParts), kNodeSummary);
    QCOMPARE(BoardProfiles::layoutSummary(kStackedMtdParts), kStackedSummary);
    QCOMPARE(BoardProfiles::layoutSummary(QStringLiteral("garbage")), QString());
    QCOMPARE(BoardProfiles::layoutSummary(QString()), QString());
}

void Tst_boardprofile::detectBoardOrder()
{
    using Source = ProfileDetection::Source;
    const QString nodeProduct = QStringLiteral("rv1106bg2-gc2083");
    // Everything at once: the override wins over a stacked PID, a Node product and a stacked layout.
    ProfileDetection d = BoardProfiles::detectBoard(evidence(QStringLiteral("node"), 0x110c, nodeProduct, kStackedMtdParts));
    QCOMPARE(d.profile.id, QStringLiteral("node"));
    QCOMPARE(d.source, Source::Option);
    QCOMPARE(d.evidence, QStringLiteral("node"));
    // Without the override: the PID wins over the product string and the layout.
    d = BoardProfiles::detectBoard(evidence(QString(), 0x110c, nodeProduct, kNodeMtdParts));
    QCOMPARE(d.profile.id, QStringLiteral("stacked"));
    QCOMPARE(d.source, Source::LoaderPid);
    QCOMPARE(d.evidence, QStringLiteral("0x110c"));
    // Without a PID: the product string wins over the layout (and is read trimmed).
    d = BoardProfiles::detectBoard(evidence(QString(), 0, QStringLiteral(" rv1106bg2-gc2083\n"), kStackedMtdParts));
    QCOMPARE(d.profile.id, QStringLiteral("node"));
    QCOMPARE(d.source, Source::AdbProduct);
    QCOMPARE(d.evidence, nodeProduct);
    // The layout last.
    d = BoardProfiles::detectBoard(evidence(QString(), 0, QString(), kStackedMtdParts));
    QCOMPARE(d.profile.id, QStringLiteral("stacked"));
    QCOMPARE(d.source, Source::BoardLayout);
    QCOMPARE(d.evidence, kStackedSummary);
    // An unknown override id (the CLI refuses it; here it is no error) falls through to the PID;
    // a PID of no profile, a product of none, to the layout.
    d = BoardProfiles::detectBoard(evidence(QStringLiteral("nope"), 0x110e, QString(), kStackedMtdParts));
    QCOMPARE(d.profile.id, QStringLiteral("node"));
    QCOMPARE(d.source, Source::LoaderPid);
    d = BoardProfiles::detectBoard(evidence(QString(), 0x1234, QStringLiteral("rv1106"), kNodeMtdParts));
    QCOMPARE(d.profile.id, QStringLiteral("node"));
    QCOMPARE(d.source, Source::BoardLayout);
    // The chip tag is the image's: never board evidence.
    d = BoardProfiles::detectBoard(evidence(QString(), 0, QString(), QString(), QStringLiteral("1106")));
    QVERIFY(!d.known());
}

void Tst_boardprofile::detectBoardNothing()
{
    const ProfileDetection d = BoardProfiles::detectBoard(ProfileEvidence());
    QVERIFY(!d.known());
    QVERIFY(!d.profile.isValid());
    QCOMPARE(d.source, ProfileDetection::Source::None);
    QVERIFY(d.evidence.isEmpty());
    QCOMPARE(d.text(), kUnknownText);
    QCOMPARE(d.sourceText(), QString());
}

void Tst_boardprofile::detectImage()
{
    using Source = ProfileDetection::Source;
    // The chip tag wins over the env.img layout.
    ProfileDetection d = BoardProfiles::detectImage(evidence(QString(), 0, QString(), kNodeMtdParts, QStringLiteral("1106")));
    QCOMPARE(d.profile.id, QStringLiteral("stacked"));
    QCOMPARE(d.source, Source::ChipTag);
    QCOMPARE(d.evidence, QStringLiteral("1106"));
    // The layout alone (an RKFW whose code names no profile).
    d = BoardProfiles::detectImage(evidence(QString(), 0, QString(), kNodeMtdParts, QStringLiteral("0x00000000")));
    QCOMPARE(d.profile.id, QStringLiteral("node"));
    QCOMPARE(d.source, Source::ImageLayout);
    QCOMPARE(d.evidence, kNodeSummary);
    // The override says which board is attached, never what the image is for; nor do PID / product.
    d = BoardProfiles::detectImage(evidence(QStringLiteral("stacked"), 0x110c, QStringLiteral("rv1106g-stacked-gc2083"), QString(),
                                            QStringLiteral("110e")));
    QCOMPARE(d.profile.id, QStringLiteral("node"));
    QCOMPARE(d.source, Source::ChipTag);
    QCOMPARE(d.evidence, QStringLiteral("110E"));
    d = BoardProfiles::detectImage(evidence(QStringLiteral("stacked"), 0x110c, QStringLiteral("rv1106g-stacked-gc2083"), QString()));
    QVERIFY(!d.known());
    QVERIFY(!BoardProfiles::detectImage(ProfileEvidence()).known());
}

void Tst_boardprofile::detectionTexts()
{
    using Source = ProfileDetection::Source;
    const struct
    {
        Source source;
        const char* evidence;
        const char* sourceText;
    } cases[] = {
        {Source::Option, "stacked", "set by --profile stacked (or the Board profile box)"},
        {Source::ChipTag, "1106", "from the image's chip tag 1106"},
        {Source::LoaderPid, "0x110c", "from the board's Loader PID 0x110c"},
        {Source::AdbProduct, "rv1106g-stacked-gc2083", "from the ADB gadget's product string rv1106g-stacked-gc2083"},
        {Source::BoardLayout, "6 partitions: env, idblock, uboot, meta, boot, userdata",
         "from the board's flash layout (6 partitions: env, idblock, uboot, meta, boot, userdata)"},
        {Source::ImageLayout, "6 partitions: env, idblock, uboot, meta, boot, userdata",
         "from the image's env.img layout (6 partitions: env, idblock, uboot, meta, boot, userdata)"},
    };
    for (const auto& c : cases) {
        ProfileDetection d;
        d.profile = stacked();
        d.source = c.source;
        d.evidence = QLatin1String(c.evidence);
        QCOMPARE(d.sourceText(), QLatin1String(c.sourceText));
        QCOMPARE(d.text(), QStringLiteral("profile: Stacked (RV1106G2) - ") + QLatin1String(c.sourceText));
    }
    ProfileDetection none;
    QCOMPARE(none.sourceText(), QString());
    QCOMPARE(none.text(), kUnknownText);
    // The two lines of the CLI contract.
    QCOMPARE(BoardProfiles::detectBoard(evidence(QString(), 0, QString(), kNodeMtdParts)).text(),
             QStringLiteral("profile: Node (RV1106BG2) - from the board's flash layout (7 partitions: env, idblock, uboot, meta, boot, "
                            "rootfs, userdata)"));
    QCOMPARE(BoardProfiles::detectImage(evidence(QString(), 0, QString(), QString(), QStringLiteral("1106"))).text(),
             QStringLiteral("profile: Stacked (RV1106G2) - from the image's chip tag 1106"));
}

void Tst_boardprofile::imageRefusal()
{
    const ProfileDetection nodeBoard = BoardProfiles::detectBoard(evidence(QString(), 0x110e, QString(), QString()));
    const ProfileDetection stackedBoard = BoardProfiles::detectBoard(evidence(QString(), 0x110c, QString(), QString()));
    const ProfileDetection nodeImage = BoardProfiles::detectImage(evidence(QString(), 0, QString(), QString(), QStringLiteral("110E")));
    const ProfileDetection stackedImage = BoardProfiles::detectImage(evidence(QString(), 0, QString(), QString(), QStringLiteral("1106")));
    const ProfileDetection unknown;
    QCOMPARE(ProfileRules::imageRefusal(unknown, stackedImage), QString());
    QCOMPARE(ProfileRules::imageRefusal(nodeBoard, unknown), QString());
    QCOMPARE(ProfileRules::imageRefusal(nodeBoard, nodeImage), QString());
    QCOMPARE(ProfileRules::imageRefusal(stackedBoard, stackedImage), QString());
    QCOMPARE(ProfileRules::imageRefusal(nodeBoard, stackedImage),
             QStringLiteral("the image is for another board: its chip tag 1106 is the Stacked (RV1106G2)'s, the board is Node "
                            "(RV1106BG2) (from the board's Loader PID 0x110e); nothing was written"));
    QCOMPARE(ProfileRules::imageRefusal(stackedBoard, nodeImage),
             QStringLiteral("the image is for another board: its chip tag 110E is the Node (RV1106BG2)'s, the board is Stacked "
                            "(RV1106G2) (from the board's Loader PID 0x110c); nothing was written"));
    // An image known by its env.img layout.
    const ProfileDetection layoutImage = BoardProfiles::detectImage(evidence(QString(), 0, QString(), kNodeMtdParts));
    QCOMPARE(ProfileRules::imageRefusal(stackedBoard, layoutImage),
             QStringLiteral("the image is for another board: its env.img layout (7 partitions: env, idblock, uboot, meta, boot, rootfs, "
                            "userdata) is the Node (RV1106BG2)'s, the board is Stacked (RV1106G2) (from the board's Loader PID "
                            "0x110c); nothing was written"));
    // The override as the board.
    const ProfileDetection byOption = BoardProfiles::detectBoard(evidence(QStringLiteral("stacked"), 0, QString(), QString()));
    QVERIFY(ProfileRules::imageRefusal(byOption, nodeImage).contains(QStringLiteral("(set by --profile stacked (or the Board profile box))")));
}

void Tst_boardprofile::layoutRefusal()
{
    const QString older = QStringLiteral("sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),4M(boot),5M(rootfs),2M(userdata)");
    QString note = QStringLiteral("stale");
    QCOMPARE(ProfileRules::layoutRefusal(kNodeMtdParts, kNodeMtdParts, false, &note), QString());
    QVERIFY(note.isEmpty());
    QCOMPARE(ProfileRules::layoutRefusal(kNodeMtdParts, QStringLiteral("mtdparts=") + kNodeMtdParts, false), QString());
    QCOMPARE(ProfileRules::layoutRefusal(kNodeMtdParts, QString(kNodeMtdParts).replace(QStringLiteral("64K(env)"), QStringLiteral("64K@0(env)")),
                                         false),
             QString());
    QCOMPARE(ProfileRules::layoutRefusal(QString(), older, false), QString());
    QCOMPARE(ProfileRules::layoutRefusal(kNodeMtdParts, QString(), false), QString());
    QCOMPARE(ProfileRules::layoutRefusal(QStringLiteral("garbage"), older, false), QString());
    const QString text = ProfileRules::layoutRefusal(kNodeMtdParts, QStringLiteral("mtdparts=") + older, false, &note);
    QCOMPARE(text, QStringLiteral("the image's flash layout differs from the board's (image: %1; board: %2): refused, the image's layout "
                                  "would be written over a board laid out differently; --force-layout (the tab: confirm the question) "
                                  "writes it anyway; nothing was written")
                       .arg(kNodeMtdParts, older));
    QVERIFY(note.isEmpty());
    QCOMPARE(ProfileRules::layoutRefusal(kNodeMtdParts, older, true, &note), QString());
    QCOMPARE(note, QStringLiteral("warning: the image's flash layout differs from the board's; the image's is used (--force-layout). "
                                  "image: %1; board: %2")
                       .arg(kNodeMtdParts, older));
    QCOMPARE(ProfileRules::layoutRefusal(kNodeMtdParts, older, true), QString());   // no note wanted
}

void Tst_boardprofile::loaderPartitionRefusal()
{
    QString note = QStringLiteral("stale");
    QCOMPARE(ProfileRules::loaderPartitionRefusal(node(), {QStringLiteral("idblock")}, false, &note), QString());   // a button route
    QVERIFY(note.isEmpty());
    QCOMPARE(ProfileRules::loaderPartitionRefusal(stacked(), {QStringLiteral("boot")}, false), QString());
    const QStringList rows{QStringLiteral("idblock"), QStringLiteral("boot"), QStringLiteral("uboot")};
    QCOMPARE(ProfileRules::loaderPartitionRefusal(stacked(), rows, false, &note),
             QStringLiteral("idblock, uboot: loader partition(s) on a Stacked (RV1106G2) board, whose MASKROM entry is not documented: "
                            "a bad write could not be recovered; refused until the MASKROM route is known - "
                            "--force-loader-partitions (the tab: confirm the question) writes them anyway; nothing was written"));
    QVERIFY(note.isEmpty());
    QCOMPARE(ProfileRules::loaderPartitionRefusal(stacked(), rows, true, &note), QString());
    QCOMPARE(note, QStringLiteral("warning: writing loader partition(s) idblock, uboot on a Stacked (RV1106G2) board without a "
                                  "documented MASKROM entry (--force-loader-partitions)"));
    QCOMPARE(ProfileRules::loaderPartitionRefusal(BoardProfile(), rows, false), QString());   // unknown: inert
    // Every row of the stacked image, in row order: the four loader partitions named.
    QVERIFY(ProfileRules::loaderPartitionRefusal(stacked(), stackedNames(), false)
                .startsWith(QStringLiteral("env, idblock, uboot, meta: loader partition(s) on a Stacked (RV1106G2) board")));
    QCOMPARE(FleetFlasher::bootloaderPartitions(),
             (QStringList{QStringLiteral("idblock"), QStringLiteral("uboot"), QStringLiteral("env"), QStringLiteral("meta")}));
}

void Tst_boardprofile::socPowerRefusal()
{
    QString note = QStringLiteral("stale");
    QCOMPARE(ProfileRules::socPowerRefusal(node(), false, &note), QString());
    QVERIFY(note.isEmpty());
    QCOMPARE(ProfileRules::socPowerRefusal(stacked(), false, &note),
             QStringLiteral("the CC1310 of a Stacked (RV1106G2) board holds the SoC's power (SOC_PWR_EN): rebooting it into its "
                            "bootloader can switch the SoC off and cut the update; refused until confirmed - --confirm-soc-power (the "
                            "tab: confirm the question) once the carrier's CC1310 bootloader is known to hold SOC_PWR_EN (the 14 Oct "
                            "2026 build or later)"));
    QCOMPARE(ProfileRules::socPowerRefusal(stacked(), true, &note), QString());
    QCOMPARE(note, QStringLiteral("warning: the CC1310 of a Stacked (RV1106G2) board holds the SoC's power; proceeding as confirmed "
                                  "(--confirm-soc-power)"));
    QCOMPARE(ProfileRules::socPowerRefusal(BoardProfile(), false, &note), QString());
    QVERIFY(note.isEmpty());
}

void Tst_boardprofile::mixedBenchStop()
{
    const ProfileDetection stackedBoard = BoardProfiles::detectBoard(evidence(QString(), 0, QString(), kStackedMtdParts));
    const ProfileDetection nodeBoard = BoardProfiles::detectBoard(evidence(QString(), 0, QString(), kNodeMtdParts));
    const ProfileDetection nodePlan = BoardProfiles::detectImage(evidence(QString(), 0, QString(), QString(), QStringLiteral("110E")));
    QCOMPARE(ProfileRules::mixedBenchStop(QStringLiteral("fe89cc526a354343"), stackedBoard, nodePlan),
             QStringLiteral("mixed bench: board fe89cc526a354343 is Stacked (RV1106G2) (from the board's flash layout (6 partitions: "
                            "env, idblock, uboot, meta, boot, userdata)) but the plan is for Node (RV1106BG2) (from the image's chip "
                            "tag 110E); one profile per run: name boards of one profile with --serials"));
    const ProfileDetection fromPid = BoardProfiles::detectBoard(evidence(QString(), 0x110c, QString(), QString()));
    QVERIFY(ProfileRules::mixedBenchStop(QStringLiteral("X"), fromPid, nodePlan)
                .contains(QStringLiteral("is Stacked (RV1106G2) (from the board's Loader PID 0x110c) but the plan")));
    QCOMPARE(ProfileRules::mixedBenchStop(QStringLiteral("X"), nodeBoard, nodePlan), QString());
    QCOMPARE(ProfileRules::mixedBenchStop(QStringLiteral("X"), ProfileDetection(), nodePlan), QString());
    QCOMPARE(ProfileRules::mixedBenchStop(QStringLiteral("X"), stackedBoard, ProfileDetection()), QString());
}

void Tst_boardprofile::maskromInstructionsAndWaitHint()
{
    QCOMPARE(node().maskromInstructions(30),
             QStringLiteral("No board found: hold the MASKROM button while plugging in the USB-C cable (or while power-cycling the "
                            "board); waiting up to 30 s"));
    QCOMPARE(stacked().maskromInstructions(45),
             QStringLiteral("No board found: the Stacked (RV1106G2) board has no documented MASKROM entry: put it into Loader mode "
                            "from a running system (adb reboot loader, or \"reboot loader\" on its console); waiting up to 45 s"));
    QCOMPARE(node().loaderWaitHint(), QString());
    QCOMPARE(stacked().loaderWaitHint(),
             QStringLiteral("; on the stacked board the CC1310 may have cut the SoC's power: power-cycle it, then retry"));
    QCOMPARE(node().loaderWaitSeconds, 30);
    QCOMPARE(stacked().loaderWaitSeconds, 45);
}

void Tst_boardprofile::recordOperatorsAndValidity()
{
    QVERIFY(!BoardProfile().isValid());
    QVERIFY(node().isValid() && stacked().isValid());
    QVERIFY(node() == node());
    QVERIFY(!(node() == stacked()));
    BoardProfile changed = stacked();
    changed.loaderWaitSeconds = 46;
    QVERIFY(!(changed == stacked()));
    changed = stacked();
    changed.dailyPartitions << QStringLiteral("userdata");
    QVERIFY(!(changed == stacked()));
    QCOMPARE(node().maskromEntryText(), QStringLiteral("button"));
    QCOMPARE(stacked().maskromEntryText(), QStringLiteral("unknown"));
    QCOMPARE(BoardProfile().maskromEntryText(), QStringLiteral("unknown"));   // the default is Unknown

    ProfileDetection a;
    QVERIFY(!a.known());
    QVERIFY(a == ProfileDetection());
    ProfileDetection b = BoardProfiles::detectBoard(evidence(QString(), 0x110c, QString(), QString()));
    QVERIFY(b.known());
    QVERIFY(!(a == b));
    ProfileDetection c = b;
    QVERIFY(c == b);
    c.evidence = QStringLiteral("0x110d");
    QVERIFY(!(c == b));
    c = b;
    c.source = ProfileDetection::Source::Option;
    QVERIFY(!(c == b));
}

void Tst_boardprofile::overrideRefusal()
{
    QCOMPARE(ProfileRules::overrideRefusal(QStringLiteral("stacked"), 0x110e),
             QStringLiteral("the --profile stacked override contradicts the board's Loader PID 0x110e (the Node (RV1106BG2)'s); drop "
                            "--profile"));
    QCOMPARE(ProfileRules::overrideRefusal(QStringLiteral("node"), 0x110c),
             QStringLiteral("the --profile node override contradicts the board's Loader PID 0x110c (the Stacked (RV1106G2)'s); drop "
                            "--profile"));
    QCOMPARE(ProfileRules::overrideRefusal(QStringLiteral("node"), 0x110e), QString());
    QCOMPARE(ProfileRules::overrideRefusal(QStringLiteral("stacked"), 0x110c), QString());
    QCOMPARE(ProfileRules::overrideRefusal(QString(), 0x110e), QString());
    QCOMPARE(ProfileRules::overrideRefusal(QStringLiteral("stacked"), 0), QString());
    QCOMPARE(ProfileRules::overrideRefusal(QStringLiteral("node"), 0x0006), QString());
    QCOMPARE(ProfileRules::overrideRefusal(QStringLiteral("stacked"), 0x1234), QString());
    QCOMPARE(ProfileRules::overrideRefusal(QStringLiteral("nope"), 0x110e), QString());
    QCOMPARE(ProfileRules::overrideRefusal(QStringLiteral(" STACKED "), 0x110e).left(36), QStringLiteral("the --profile stacked override contr"));
}

QTEST_GUILESS_MAIN(Tst_boardprofile)
#include "tst_boardprofile.moc"
