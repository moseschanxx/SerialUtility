#include "support/FirmwareFixtures.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>

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

QByteArray rkfwBytes(int size)
{
    QByteArray bytes("RKFW");
    bytes.append(QByteArray(size - 4, '\x5A'));
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
    runner.addHandler(QRegularExpression(QStringLiteral("^upgrade_tool EXF ")), &nodeExtractor);
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool WL ")), "Write LBA from file (50%)\rWrite LBA from file (100%)\r\n");
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool RD$")), "Reset Device OK.\r\n");
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool UF ")), "Download Image... (100%)\r\nUpgrade firmware ok.\r\n");
    runner.addScript(QRegularExpression(QStringLiteral("^upgrade_tool SFI ")), "Type:Update Firmware\r\nChip Tag:110E\r\n");
    runner.addScript(QRegularExpression(QStringLiteral("^adb( -s \\S+)? get-state$")), "device\n");
    runner.addScript(QRegularExpression(QStringLiteral("cat /proc/version")), kProcVersion);
    runner.addScript(QRegularExpression(QStringLiteral("cat /proc/cmdline")), kNodeCmdline);
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
