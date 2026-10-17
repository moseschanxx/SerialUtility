#include "support/FirmwareFixtures.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QPair>
#include <QPointer>

#include <functional>
#include <utility>

#include "firmware/ToolRunner.h"
#include "support/FakeToolRunner.h"
#include "support/FakeUsbBus.h"

namespace FirmwareFixtures {

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

QByteArray rkfwBytes(int size, const QString& chipTag)
{
    // The header of the real Node image of 2026-10-13 (RockchipTool.h, RkfwHeader), with the
    // chip code of `chipTag` and simple offsets.
    QByteArray header(0x66, '\0');
    header.replace(0, 4, "RKFW");
    header[4] = '\x66';                                 // header length 0x66 LE; version 0.0.00 = zeros
    header[0x0e] = static_cast<char>(2026 & 0xFF);       // year LE
    header[0x0f] = static_cast<char>(2026 >> 8);
    header[0x10] = 10;
    header[0x11] = 13;
    header[0x12] = 21;
    header[0x13] = 45;
    header[0x14] = 50;
    const QByteArray tag = chipTag.toLatin1().leftJustified(4, '\0', true);
    for (int k = 0; k < 4; ++k) {
        header[0x15 + k] = tag.at(3 - k);                // "110E" is stored as 45 30 31 31
    }
    header[0x19] = '\x66';                              // loader offset 0x66, loader size 0
    header[0x21] = '\x66';                              // image offset 0x66, image size 0
    QByteArray bytes = header;
    if (size > bytes.size()) {
        bytes.append(QByteArray(size - bytes.size(), '\x5A'));
    }
    return bytes;
}

const QByteArray kAdbBoard = "List of devices attached\r\n0225ce3c8d8d6e44\tdevice\r\n\r\n";
const QByteArray kLdLoader = "rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\n"
                             "List of rockusb connected(1)\r\n"
                             "DevNo=1\tVid=0x2207,Pid=0x110e,LocationID=104\tMode=Loader\tSerialNo=\r\n";
const QByteArray kLdMaskrom = "rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\n"
                              "List of rockusb connected(1)\r\n"
                              "DevNo=1\tVid=0x2207,Pid=0x110c,LocationID=104\tMode=Maskrom\tSerialNo=\r\n";
const QByteArray kProcVersion = "Linux version 5.10.209+ (builder@rk) #1 Tue Oct 13 21:40:00 CST 2026\n"
                                " 10:42:01 up 0 min,  load average: 0.10, 0.03, 0.01\n";
const QByteArray kLdLoaderStacked = "rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\n"
                                    "List of rockusb connected(1)\r\n"
                                    "DevNo=1\tVid=0x2207,Pid=0x110c,LocationID=104\tMode=Loader\tSerialNo=\r\n";
const QByteArray kLdEmpty = "rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\n"
                            "List of rockusb connected(0)\r\n";
const QByteArray kLdTwoLoaders = "rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\n"
                                 "List of rockusb connected(2)\r\n"
                                 "DevNo=1\tVid=0x2207,Pid=0x110e,LocationID=104\tMode=Loader\tSerialNo=\r\n"
                                 "DevNo=2\tVid=0x2207,Pid=0x110c,LocationID=112\tMode=Loader\tSerialNo=\r\n";
const QByteArray kProductStacked = "rv1106g-stacked-gc2083\n";

const QRegularExpression& productPattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("^adb (-s \\S+ )?shell \"?cat /sys/kernel/config/usb_gadget/rockchip/strings/0x409/product\"?$"));
    return pattern;
}

const QString kNodeMtdParts =
    QStringLiteral("sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),3M(boot),5M(rootfs),3M(userdata)");
const QByteArray kNodeCmdline = "console=ttyFIQ0 root=/dev/mtdblock5 rootfstype=squashfs rootwait "
                                "mtdparts=sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),3M(boot),5M(rootfs),"
                                "3M(userdata) rk_dma_heap_cma=72M\n";
const char* const kNodeNames[kNodePartitionCount] = {"env", "idblock", "uboot", "meta", "boot", "rootfs", "userdata"};
const qint64 kNodeImageBytes[kNodePartitionCount] = {65536, 174080, 196608, 393216, 3043840, 3830272, 3145728};
const int kNodeSectors[kNodePartitionCount] = {0, 128, 640, 1152, 1920, 8064, 18304};
const int kNodeSizeSectors[kNodePartitionCount] = {128, 512, 512, 768, 6144, 10240, 6144};

QStringList nodeNames()
{
    QStringList names;
    for (const char* name : kNodeNames) {
        names << QLatin1String(name);
    }
    return names;
}

QByteArray nodeEnvImage(const QString& layout)
{
    QByteArray env = QByteArray::fromHex("6a6798f8") + "mtdparts=" + layout.toLatin1() + '\0'
                     + "sys_bootargs= rootfstype=erofs mtdparts=" + layout.toLatin1() + " rk_dma_heap_cma=72M" + '\0'
                     + "sd_parts=mmcblk0:16K@512(env),512K@32K(idblock),4M(uboot),32M(boot),2G(rootfs),-(userdata)" + '\0' + '\0';
    env.append(QByteArray(kNodeImageBytes[0] - env.size(), '\0'));
    return env;
}

const QString kStackedMtdParts =
    QStringLiteral("sfc_nor:64K(env),256K@64K(idblock),256K(uboot),384K(meta),12M(boot),3M(userdata)");
const QByteArray kStackedCmdline = "console=ttyFIQ0 root=/dev/rd0 rootfstype=erofs mtdparts=sfc_nor:64K(env),256K@64K(idblock),"
                                   "256K(uboot),384K(meta),12M(boot),3M(userdata) rk_dma_heap_cma=48M\n";
const char* const kStackedNames[kStackedPartitionCount] = {"env", "idblock", "uboot", "meta", "boot", "userdata"};
const qint64 kStackedImageBytes[kStackedPartitionCount] = {65536, 174080, 196608, 393216, 2097152, 3145728};
const int kStackedSectors[kStackedPartitionCount] = {0, 128, 640, 1152, 1920, 26496};
const int kStackedSizeSectors[kStackedPartitionCount] = {128, 512, 512, 768, 24576, 6144};

QStringList stackedNames()
{
    QStringList names;
    for (const char* name : kStackedNames) {
        names << QLatin1String(name);
    }
    return names;
}

QByteArray stackedEnvImage(const QString& layout)
{
    QByteArray env = QByteArray::fromHex("5b1e0c77") + "mtdparts=" + layout.toLatin1() + '\0'
                     + "sys_bootargs= mtdparts=" + layout.toLatin1() + " rk_dma_heap_cma=48M" + '\0'
                     + "sd_parts=mmcblk0:16K@512(env),512K@32K(idblock),4M(uboot)" + '\0' + '\0';
    env.append(QByteArray(kStackedImageBytes[0] - env.size(), '\0'));
    return env;
}

void stackedExtractor(FakeToolInvocation* i)
{
    const QStringList args = i->arguments();
    const QString dir = args.size() >= 3 ? QDir::fromNativeSeparators(args.at(2)) : QString();
    const bool big = args.size() >= 2 && QFileInfo(args.at(1)).fileName().contains(QLatin1String("big"));
    const bool withEnv = args.size() >= 2 && QDir::fromNativeSeparators(args.at(1)).contains(QLatin1String("withenv"));
    QByteArray list;
    bool ok = !dir.isEmpty();
    for (int k = 0; k < kStackedPartitionCount && ok; ++k) {
        const qint64 size = (big && k == 4) ? 12 * 1024 * 1024 + 1 : kStackedImageBytes[k];
        const QByteArray bytes = (withEnv && k == 0) ? stackedEnvImage(kStackedMtdParts) : QByteArray(size, 's');
        ok = writeFile(dir + QLatin1Char('/') + QLatin1String(kStackedNames[k]) + QStringLiteral(".img"), bytes);
        list += QByteArray(kStackedNames[k]) + '\t' + kStackedNames[k] + ".img\n";
    }
    list += "package-file\tpackage-file\nbootloader\tdownload.bin\n";
    ok = ok && writeFile(dir + QStringLiteral("/package-file"), list) && writeFile(dir + QStringLiteral("/download.bin"), "LDR ");
    if (!ok) {
        i->emitOutput("Extract failed\r\n");
        i->finish(1);
        return;
    }
    i->emitOutput("start to extract boot.img...(100%)\r\nExtract ok.\r\n");
    i->finish(0);
}

QString expectedStackedWl(int k, const QString& extractDir)
{
    return ToolRunner::formatCommandLine(
        QStringLiteral("upgrade_tool"),
        {QStringLiteral("WL"), QString::number(kStackedSectors[k]), QString::number(kStackedSizeSectors[k]),
         QDir::toNativeSeparators(extractDir + QLatin1Char('/') + QLatin1String(kStackedNames[k]) + QStringLiteral(".img"))});
}

void nodeExtractor(FakeToolInvocation* i)
{
    const QStringList args = i->arguments();
    const QString dir = args.size() >= 3 ? QDir::fromNativeSeparators(args.at(2)) : QString();
    const bool big = args.size() >= 2 && QFileInfo(args.at(1)).fileName().contains(QLatin1String("big"));
    const bool withEnv = args.size() >= 2 && QDir::fromNativeSeparators(args.at(1)).contains(QLatin1String("/withenv"));
    QByteArray list;
    bool ok = !dir.isEmpty();
    for (int k = 0; k < kNodePartitionCount && ok; ++k) {
        const qint64 size = (big && k == 4) ? 3 * 1024 * 1024 + 1 : kNodeImageBytes[k];
        const QByteArray bytes = (withEnv && k == 0) ? nodeEnvImage(kNodeMtdParts) : QByteArray(size, 'n');
        ok = writeFile(dir + QLatin1Char('/') + QLatin1String(kNodeNames[k]) + QStringLiteral(".img"), bytes);
        list += QByteArray(kNodeNames[k]) + '\t' + kNodeNames[k] + ".img\n";
    }
    list += "package-file\tpackage-file\nbootloader\tdownload.bin\n";
    ok = ok && writeFile(dir + QStringLiteral("/package-file"), list) && writeFile(dir + QStringLiteral("/download.bin"), "LDR ");
    if (!ok) {
        i->emitOutput("Extract failed\r\n");
        i->finish(1);
        return;
    }
    i->emitOutput("start to extract boot.img...(100%)\r\nExtract ok.\r\n");
    i->finish(0);
}

void scriptSocFlash(FakeToolRunner& runner, const QByteArray& ld)
{
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool LD$")), ld);
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool EXF .*stacked")), &stackedExtractor);   // first: the path decides
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool EXF ")), &nodeExtractor);
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool WL ")), "Write LBA from file (50%)\rWrite LBA from file (100%)\r\n");
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool RD$")), "Reset Device OK.\r\n");
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool UF ")), "Download Image... (100%)\r\nUpgrade firmware ok.\r\n");
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool SFI ")), "Type:Update Firmware\r\nChip Tag:110E\r\n");
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? get-state$")), "device\n");
    runner.addScript(QRegularExpression(QStringLiteral("cat /proc/version")), kProcVersion);
    runner.addScript(QRegularExpression(QStringLiteral("cat /proc/cmdline")), kNodeCmdline);
}

const QByteArray kPlNode = "rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\n"
                           "List of partitions(7)\r\n"
                           "No  Offset(LBA)  Size(LBA)  Name\r\n"
                           "01  0x00000000  0x00000080  env\r\n"
                           "02  0x00000080  0x00000200  idblock\r\n"
                           "03  0x00000280  0x00000200  uboot\r\n"
                           "04  0x00000480  0x00000300  meta\r\n"
                           "05  0x00000780  0x00001800  boot\r\n"
                           "06  0x00001F80  0x00002800  rootfs\r\n"
                           "07  0x00004780  0x00001800  userdata\r\n";
const QByteArray kPlStacked = "rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\n"
                              "List of partitions(6)\r\n"
                              "No  Offset(LBA)  Size(LBA)  Name\r\n"
                              "01  0x00000000  0x00000080  env\r\n"
                              "02  0x00000080  0x00000200  idblock\r\n"
                              "03  0x00000280  0x00000200  uboot\r\n"
                              "04  0x00000480  0x00000300  meta\r\n"
                              "05  0x00000780  0x00006000  boot\r\n"
                              "06  0x00006780  0x00001800  userdata\r\n";
const QByteArray kPlDecimal = "rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\n"
                              "List of partitions(7)\r\n"
                              "Name  Offset(LBA)  Size(LBA)\r\n"
                              "env  0  128\r\n"
                              "idblock  128  512\r\n"
                              "uboot  640  512\r\n"
                              "meta  1152  768\r\n"
                              "boot  1920  6144\r\n"
                              "rootfs  8064  10240\r\n"
                              "userdata  18304  6144\r\n";
const QByteArray kPlGarbage = "rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\n"
                              "List of partitions(0)\r\n";
const QByteArray kPlHeaderOnly = "rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\n"
                                 "List of partitions(7)\r\n"
                                 "No  Offset(LBA)  Size(LBA)  Name\r\n";

const QRegularExpression& loaderEnvReadPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^upgrade_tool -s \\d+ RL 0 128 "));
    return pattern;
}

const QRegularExpression& loaderPartitionListPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^upgrade_tool -s \\d+ PL$"));
    return pattern;
}

void scriptLoaderLayoutRead(FakeToolRunner& runner, const QByteArray& envImage, const QByteArray& plOutput, int plExitCode)
{
    runner.addHandler(loaderEnvReadPattern(), [envImage](FakeToolInvocation* i) {
        // The file is the last argument (native separators, as RockchipTool passes it).
        const QString file = i->arguments().isEmpty() ? QString() : QDir::fromNativeSeparators(i->arguments().last());
        if (envImage.isEmpty() || file.isEmpty() || !writeFile(file, envImage)) {
            i->emitOutput("Read LBA failed!\r\n");
            i->finish(1);
            return;
        }
        i->emitOutput("Read LBA from file (100%)\r\n");
        i->finish(0);
    });
    runner.addScript(loaderPartitionListPattern(), plOutput, plExitCode);
}

// ---- (v0.8.0, verify-rl) FakeFlash ------------------------------------------------------------------

struct FakeFlash::State
{
    QMap<quint64, QString> written;                 ///< sector -> the file its last WL wrote (as the command named it)
    QMap<quint64, QPair<qint64, quint8>> corrupt;   ///< sector -> (index, value): the next read only
    QMap<quint64, qint64> shortReads;               ///< sector -> bytes: the next read only
    bool failing = false;
    QByteArray failOutput;
    int failExit = 0;
    bool gone = false;
    bool hold = false;
    QList<std::function<void()>> held;
    QStringList reads;
    QStringList writes;
    int unaddressed = 0;
    QMap<quint64, int> writesBeforeRead;
};

namespace {

/// "Read LBA from file (10%)" ... "(90%)" and then the file and "(100%)", exit 0.
void finishRead(FakeToolInvocation* i, const QString& target, const QByteArray& bytes)
{
    if (!writeFile(target, bytes)) {
        i->emitOutput("Read LBA failed!\r\n");
        i->finish(1);
        return;
    }
    i->emitOutput("Read LBA from file (100%)\r\n");
    i->finish(0);
}

} // namespace

FakeFlash::FakeFlash() : state(std::make_shared<State>()) {}

void FakeFlash::script(FakeToolRunner& runner)
{
    const std::shared_ptr<State> s = state;
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool (-s \\S+ )?WL ")), [s](FakeToolInvocation* i) {
        const QStringList args = i->arguments();
        const qsizetype at = args.indexOf(QStringLiteral("WL"));
        s->writes << i->commandLine();
        s->written.insert(args.value(at + 1).toULongLong(), args.value(at + 3));
        i->emitOutput("Write LBA from file (50%)\rWrite LBA from file (100%)\r\n");
        i->finish(0);
    });
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool -s \\S+ RL ")), [s](FakeToolInvocation* i) {
        const QStringList args = i->arguments();   // -s ID RL start count file
        const quint64 sector = args.value(3).toULongLong();
        const qint64 count = args.value(4).toLongLong();
        const QString target = QDir::fromNativeSeparators(args.value(5));
        s->reads << i->commandLine();
        s->writesBeforeRead.insert(sector, static_cast<int>(s->writes.size()));
        if (s->gone) {
            i->emitOutput("rockusb driver = 5.12,please upgrade it to enjoy fast download\r\n\r\nNo found specific rockusb device!\r\n");
            i->finish(255);
            return;
        }
        if (s->failing) {
            if (!s->failOutput.isEmpty()) {
                i->emitOutput(s->failOutput);
            }
            i->finish(s->failExit);
            return;
        }
        QByteArray bytes;
        QFile file(QDir::fromNativeSeparators(s->written.value(sector)));
        if (s->written.contains(sector) && file.open(QIODevice::ReadOnly)) {
            bytes = file.read(count * 512);
        }
        if (bytes.size() < count * 512) {
            bytes.append(QByteArray(count * 512 - bytes.size(), '\xff'));
        }
        if (s->corrupt.contains(sector)) {
            const QPair<qint64, quint8> flip = s->corrupt.take(sector);
            if (flip.first >= 0 && flip.first < bytes.size()) {
                bytes[flip.first] = static_cast<char>(flip.second);
            }
        }
        if (s->shortReads.contains(sector)) {
            bytes = bytes.left(s->shortReads.take(sector));
        }
        if (s->hold) {
            i->emitOutput("Read LBA from file (10%)\r");
            i->hang();
            const QPointer<FakeToolInvocation> guard(i);
            s->held << [guard, target, bytes] {
                if (guard) {
                    finishRead(guard, target, bytes);
                }
            };
            return;
        }
        for (int percent = 10; percent < 100; percent += 10) {
            i->emitOutput("Read LBA from file (" + QByteArray::number(percent) + "%)\r");
        }
        finishRead(i, target, bytes);
    });
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool RL ")), [s](FakeToolInvocation* i) {
        ++s->unaddressed;
        i->emitOutput("fake: unaddressed read refused\r\n");
        i->finish(99);
    });
}

void FakeFlash::corruptByte(quint64 sector, qint64 index, quint8 value) { state->corrupt.insert(sector, {index, value}); }
void FakeFlash::shortRead(quint64 sector, qint64 bytes) { state->shortReads.insert(sector, bytes); }

void FakeFlash::failReads(const QByteArray& output, int exitCode)
{
    state->failing = true;
    state->failOutput = output;
    state->failExit = exitCode;
}

void FakeFlash::noSuchDevice() { state->gone = true; }
void FakeFlash::hangReads() { state->hold = true; }

void FakeFlash::releaseReads()
{
    state->hold = false;
    const QList<std::function<void()>> held = std::exchange(state->held, {});
    for (const auto& finish : held) {
        finish();
    }
}

QStringList FakeFlash::readCommands() const { return state->reads; }
QStringList FakeFlash::writeCommands() const { return state->writes; }
int FakeFlash::unaddressedReads() const { return state->unaddressed; }
int FakeFlash::writesBefore(quint64 sector) const { return state->writesBeforeRead.value(sector, -1); }

void scriptSocFlashWithReadBack(FakeToolRunner& runner, const QByteArray& ld, FakeFlash& flash)
{
    flash.script(runner);           // first: its WL handler takes every WL, its RL handler every RL
    scriptSocFlash(runner, ld);
}

QStringList wlCommands(const FakeToolRunner& runner)
{
    return runner.invocations().filter(QRegularExpression(QStringLiteral("^upgrade_tool WL ")));
}

QString expectedWl(int k, const QString& extractDir)
{
    return ToolRunner::formatCommandLine(
        QStringLiteral("upgrade_tool"),
        {QStringLiteral("WL"), QString::number(kNodeSectors[k]), QString::number(kNodeSizeSectors[k]),
         QDir::toNativeSeparators(extractDir + QLatin1Char('/') + QLatin1String(kNodeNames[k]) + QStringLiteral(".img"))});
}

QString extractDirOf(const QString& image)
{
    const QFileInfo info(image);
    return info.absolutePath() + QLatin1Char('/') + info.completeBaseName() + QStringLiteral(".extracted");
}

const QRegularExpression& exfPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^upgrade_tool EXF "));
    return pattern;
}

void scriptMcuBridge(FakeToolRunner& runner, quint16 bridgePort)
{
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? devices$")), kAdbBoard);
    runner.addScript(QRegularExpression(QStringLiteral("command -v rfmcu")), "/usr/bin/rfmcu\n");
    runner.addScript(QRegularExpression(QStringLiteral("killall rfmcu")), QByteArray());
    runner.addScript(QRegularExpression(QStringLiteral("forward tcp:0 tcp:7000$")), QByteArray::number(bridgePort) + "\n");
    runner.addHandler(QRegularExpression(QStringLiteral("shell rfmcu bridge 7000$")), [](FakeToolInvocation* i) {
        i->emitOutput("rfmcu: bridging /dev/ttyS1 (115200 baud) to 127.0.0.1:7000\n");
        i->hang();
    });
    runner.addScript(QRegularExpression(QStringLiteral("forward --remove tcp:\\d+$")), QByteArray());
}

const FleetBench kFleetBench[7] = {
    {"A", "0225ce3c8d8d6e44", "6&38635FDB&0&3"}, {"B", "62ecfc6ddeb3f579", "6&38635FDB&0&2"},
    {"C", "fe89cc526a354343", "6&38635FDB&0&4"}, {"D", "13fb949c8cac107f", "6&38635FDB&0&1"},
    {"E", "64fad5e41d3273d5", "6&3A944CE5&0&2"}, {"F", "c4130950110960e0", "6&3A944CE5&0&1"},
    {"G", "5a33c81a6e996266", "6&3A944CE5&0&4"},
};

void scriptFleet(FakeUsbBus& bus, FakeToolRunner& runner)
{
    for (const FleetBench& board : kFleetBench) {
        bus.addBoard(QLatin1String(board.serial), QLatin1String(board.port));
    }
    runner.addHandler(exfPattern(), &nodeExtractor);
    bus.script(runner);
}

bool BuildMachine::start(SshProfileStore* store, const QString& workDir)
{
    if (!root.isValid()) {
        return false;
    }
    imageFolder = root.path() + QStringLiteral("/image");
    if (!writeFile(imageFolder + QStringLiteral("/update.img"), image)) {
        return false;
    }
    if (!QDir().mkpath(workDir)) {
        return false;
    }
    TestSshServer::Options options;
    options.rootDir = root.path();
    if (passwordAuth) {
        options.allowPassword = true;
        options.allowPublicKey = false;
        options.password = password;
    } else {
        keyFile = workDir + QStringLiteral("/client_ed25519");
        QFile::remove(keyFile);
        QString publicKey;
        if (!TestSshServer::generateClientKeyPair(keyFile, &publicKey)) {
            return false;
        }
        options.authorizedPublicKey = publicKey;
        options.allowPassword = false;
    }
    server = std::make_unique<TestSshServer>(options);
    QString error;
    if (!server->start(&error)) {
        qWarning() << "TestSshServer:" << error;
        return false;
    }
    knownHostsFile = workDir + QStringLiteral("/known_hosts");
    const QByteArray knownHosts = trustServer ? (server->knownHostsLine() + QLatin1Char('\n')).toUtf8() : QByteArray();
    if (!writeFile(knownHostsFile, knownHosts)) {
        return false;
    }
    SshProfile p;
    p.name = QStringLiteral("Build machine");
    p.host = QStringLiteral("127.0.0.1");
    p.port = server->port();
    p.user = options.user;
    p.auth = passwordAuth ? SshProfile::Auth::Password : SshProfile::Auth::PublicKey;
    p.identityFile = keyFile;
    p.knownHostsFile = knownHostsFile;
    p.connectTimeoutSeconds = 5;
    if (!store) {
        profile = p;
        return true;
    }
    profile = store->upsert(p);
    return !profile.id.isEmpty();
}

QString BuildMachine::target() const
{
    return QStringLiteral("%1@127.0.0.1:%2").arg(profile.user).arg(server ? server->port() : 0);
}

} // namespace FirmwareFixtures
