#include "core/DeviceSimulator.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStringList>
#include <QtMath>
#include <algorithm>
#include <cmath>

#include "app/Logging.h"
#include "terminal/CharWidth.h"

namespace {

constexpr int kPaceIntervalMs = 20;
constexpr int kTicksPerSecond = 1000 / kPaceIntervalMs;
constexpr int kBitsPerByte = 10;               ///< start + 8 data + stop
constexpr int kRebootDownMs = 3000;
constexpr int kMcuResetDownMs = 1500;
constexpr int kVanishDelayMs = 400;            ///< lets the "going down" line reach the host first
constexpr int kTelemetryIntervalMs = 2000;
constexpr int kCountdownStart = 3;
constexpr int kProgressSteps = 100;
constexpr int kProgressStepMs = 30;
constexpr int kMaxSleepSeconds = 30;
constexpr qint32 kDefaultBaud = 115200;

const QLatin1String kPrefix("SIM:");
const QLatin1String kManufacturer("BuildAI Simulator");
const QLatin1String kHostName("rv1106");

// ---- presence table ---------------------------------------------------------------------

QHash<QString, qint64>& downUntilTable()
{
    static QHash<QString, qint64> table;
    return table;
}

QString presenceKey(const QString& portName)
{
    return portName.trimmed().toLower();
}

// ---- static device texts ----------------------------------------------------------------

const char* const kUBootBanner[] = {
    "",
    "U-Boot 2017.09-g9c9ea3b6-dirty (Sep 17 2026 - 10:21:43 +0800)",
    "",
    "Model: Rockchip RV1106 EVB",
    "PreSerial: 2, raw, 0xff4c0000",
    "DRAM:  64 MiB",
    "Sysmem: init",
    "Relocation Offset: 02b2c000",
    "Relocation fdt: 02f7b6b8 - 02f7f890",
    "CLK: (sync kernel. arm: enter 1200000 KHz, init 1200000 KHz, kernel 0N/A)",
    "  apll 1200000 KHz",
    "  dpll 900000 KHz",
    "  gpll 1188000 KHz",
    "  cpll 1000000 KHz",
    "  aclk_bus 300000 KHz",
    "  hclk_bus 200000 KHz",
    "  pclk_bus 100000 KHz",
    "Net:   No ethernet found.",
    "MMC:   dwmmc@ffaa0000: 0",
    "Loading Environment from MMC... OK",
    "Bootdev(atags): mmc 0",
    "MMC0: HS200, 200Mhz",
    "PartType: EFI",
    "boot mode: None",
    "",
};

const char* const kKernelBoot[] = {
    "[    0.000000] Booting Linux on physical CPU 0x0",
    "[    0.000000] Linux version 5.10.160 (buildai@ci) (arm-rockchip830-linux-uclibcgnueabihf-gcc (crosstool-NG "
    "1.24.0) 8.3.0, GNU ld (crosstool-NG 1.24.0) 2.32) #1 PREEMPT Wed Sep 17 10:21:43 CST 2026",
    "[    0.000000] CPU: ARMv7 Processor [410fc075] revision 5 (ARMv7), cr=10c5387d",
    "[    0.000000] CPU: div instructions available: patching division code",
    "[    0.000000] CPU: PIPT / VIPT nonaliasing data cache, VIPT aliasing instruction cache",
    "[    0.000000] OF: fdt: Machine model: Luckfox Pico Max",
    "[    0.000000] Memory policy: Data cache writeback",
    "[    0.000000] Zone ranges:",
    "[    0.000000]   Normal   [mem 0x0000000000000000-0x0000000003ffffff]",
    "[    0.000000] Kernel command line: earlycon=uart8250,mmio32,0xff4c0000 console=ttyFIQ0 root=/dev/mmcblk0p5 "
    "rootfstype=squashfs rootwait snd_aloop.index=7",
    "[    0.000000] Dentry cache hash table entries: 8192 (order: 3, 32768 bytes, linear)",
    "[    0.000000] Memory: 52376K/65536K available (4096K kernel code, 337K rwdata, 1160K rodata, 1024K init, "
    "210K bss, 13160K reserved, 0K cma-reserved)",
    "[    0.000000] rcu: Preemptible hierarchical RCU implementation.",
    "[    0.000000] NR_IRQS: 16, nr_irqs: 16, preallocated irqs: 16",
    "[    0.000000] arch_timer: cp15 timer(s) running at 24.00MHz (phys).",
    "[    0.000010] sched_clock: 56 bits at 24MHz, resolution 41ns, wraps every 4398046511097ns",
    "[    0.000340] Console: colour dummy device 80x30",
    "[    0.000381] Calibrating delay loop (skipped), value calculated using timer frequency.. 48.00 BogoMIPS "
    "(lpj=240000)",
    "[    0.001123] CPU: Testing write buffer coherency: ok",
    "[    0.002145] Setting up static identity map for 0x100000 - 0x100060",
    "[    0.003876] devtmpfs: initialized",
    "[    0.011245] clocksource: jiffies: mask: 0xffffffff max_cycles: 0xffffffff, max_idle_ns: 19112604462750000 ns",
    "[    0.012011] pinctrl core: initialized pinctrl subsystem",
    "[    0.014532] NET: Registered protocol family 16",
    "[    0.021873] rockchip-gpio ff380000.gpio: probed /pinctrl/gpio@ff380000",
    "[    0.034091] iommu: Default domain type: Translated",
    "[    0.041256] SCSI subsystem initialized",
    "[    0.045680] usbcore: registered new interface driver usbfs",
    "[    0.052731] clocksource: Switched to clocksource arch_sys_counter",
    "[    0.081324] NET: Registered protocol family 2",
    "[    0.092155] Initialise system trusted keyrings",
    "[    0.101334] workingset: timestamp_bits=30 max_order=14 bucket_order=0",
    "[    0.132458] rockchip-pinctrl pinctrl: probed pinctrl",
    "[    0.144923] Serial: 8250/16550 driver, 5 ports, IRQ sharing disabled",
    "[    0.151006] fiq_debugger fiq_debugger.0: IRQ fiq not found",
    "[    0.151702] Registered fiq debugger ttyFIQ0 (with debug console)",
    "[    0.152418] ff4c0000.serial: ttyS2 at MMIO 0xff4c0000 (irq = 24, base_baud = 1500000) is a 16550A",
    "[    0.221356] rknpu fdab0000.npu: RKNPU: rknpu iommu is enabled, using iommu mode",
    "[    0.290337] mmc0: new high speed SDHC card at address aaaa",
    "[    0.294812] mmcblk0: mmc0:aaaa SD8GB 7.42 GiB",
    "[    0.312455]  mmcblk0: p1 p2 p3 p4 p5 p6 p7",
    "[    0.354190] usbcore: registered new interface driver usbhid",
    "[    0.362811] rockchip-thermal ff3e0000.tsadc: tsadc is 40 C",
    "[    0.389904] VFS: Mounted root (squashfs filesystem) readonly on device 179:5.",
    "[    0.402118] devtmpfs: mounted",
    "[    0.415702] Freeing unused kernel memory: 1024K",
    "[    0.416233] Run /sbin/init as init process",
    "[    1.012455] rockchip-vop ff4a0000.vop: rockchip vop probed successfully",
    "[    1.190338] EXT4-fs (mmcblk0p7): mounted filesystem with ordered data mode. Opts: (null)",
};

const char* const kDmesgExtra[] = {
    "[    1.204551] rk_gmac-dwmac ffa80000.ethernet: IRQ eth_wake_irq not found",
    "[    1.205109] rk_gmac-dwmac ffa80000.ethernet: PTP uses main clock",
    "[    1.216430] rk_gmac-dwmac ffa80000.ethernet: clock input or output? (output).",
    "[    1.243212] rockchip-usb2phy ff3e0000.usb2-phy: Looking up phy-supply from device tree",
    "[    1.302019] \x1b[33mmmc1: Failed to initialize a non-removable card\x1b[0m",
    "[    1.331877] rkcif_mipi_lvds: rkcif_plat_probe: probe success",
    "[    1.402330] rkisp rkisp-vir0: Entity type for entity rkisp-isp-subdev was not initialized!",
    "[    1.431200] rkisp rkisp-vir0: rkisp driver version: v02.02.01",
    "[    1.502115] \x1b[31mrkisp rkisp-vir0: failed to find sensor, retry later\x1b[0m",
    "[    1.612834] input: rk805 pwrkey as /devices/platform/ff3c0000.i2c/i2c-0/0-0018/rk805-pwrkey/input/input0",
    "[    1.700415] usb 1-1: new high-speed USB device number 2 using dwc2",
    "[    1.921209] usb 1-1: New USB device found, idVendor=1a86, idProduct=55d3, bcdDevice= 4.43",
    "[    1.921661] usb 1-1: New USB device strings: Mfr=1, Product=2, SerialNumber=3",
    "[    2.014522] rknpu fdab0000.npu: \x1b[32mRKNPU: enable rknpu power, hw version 0x0000010b\x1b[0m",
    "[    2.145908] random: crng init done",
    "[    3.000217] \x1b[1;31mrkisp rkisp-vir0: no sensor found, using test pattern\x1b[0m",
};

const char* const kInitLines[] = {
    "Starting syslogd: OK",
    "Starting klogd: OK",
    "Running sysctl: OK",
    "Saving 2048 bits of non-creditable seed for next boot",
    "Starting network: [  \x1b[32mOK\x1b[0m  ]",
    "Starting rkipc: [  \x1b[32mOK\x1b[0m  ]",
    "Starting sshd: [\x1b[31mFAILED\x1b[0m] (no host keys, run ssh-keygen -A)",
    "Starting dropbear sshd: [  \x1b[32mOK\x1b[0m  ]",
    "Starting BuildAI agent: [  \x1b[32mOK\x1b[0m  ]",
    // UTF-8 source, compiled with /utf-8 (MSVC) or the GCC/Clang default execution charset.
    "本机信息：瑞芯微 RV1106 开发板，内核 5.10.160，欢迎使用 BuildAI Linux",
};

const char* const kCpuInfo[] = {
    "processor\t: 0",
    "model name\t: ARMv7 Processor rev 5 (v7l)",
    "BogoMIPS\t: 48.00",
    "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt vfpd32 lpae ",
    "CPU implementer\t: 0x41",
    "CPU architecture: 7",
    "CPU variant\t: 0x0",
    "CPU part\t: 0xc07",
    "CPU revision\t: 5",
    "",
    "Hardware\t: Rockchip (Device Tree)",
    "Revision\t: 0000",
    "Serial\t\t: 1a2b3c4d5e6f7081",
};

const char* const kMemInfo[] = {
    "MemTotal:          62896 kB",
    "MemFree:           21660 kB",
    "MemAvailable:      19280 kB",
    "Buffers:            2048 kB",
    "Cached:            12288 kB",
    "SwapCached:            0 kB",
    "Active:            18904 kB",
    "Inactive:           9012 kB",
    "Active(anon):      14556 kB",
    "Inactive(anon):      120 kB",
    "Active(file):       4348 kB",
    "Inactive(file):     8892 kB",
    "Unevictable:           0 kB",
    "Mlocked:               0 kB",
    "SwapTotal:             0 kB",
    "SwapFree:              0 kB",
    "Dirty:                 0 kB",
    "Writeback:             0 kB",
    "AnonPages:         14580 kB",
    "Mapped:             6120 kB",
    "Shmem:               172 kB",
    "KReclaimable:       2912 kB",
    "Slab:               6540 kB",
    "SReclaimable:       2912 kB",
    "SUnreclaim:         3628 kB",
    "KernelStack:         520 kB",
    "PageTables:          392 kB",
    "CommitLimit:       31448 kB",
    "Committed_AS:      29744 kB",
    "VmallocTotal:     950272 kB",
    "VmallocUsed:        3212 kB",
    "CmaTotal:              0 kB",
    "CmaFree:               0 kB",
};

const char* const kPsLines[] = {
    "  PID USER       VSZ STAT COMMAND",
    "    1 root      1968 S    init",
    "    2 root         0 SW   [kthreadd]",
    "    3 root         0 IW   [rcu_gp]",
    "    8 root         0 IW<  [mm_percpu_wq]",
    "    9 root         0 SW   [ksoftirqd/0]",
    "   10 root         0 IW   [rcu_preempt]",
    "   11 root         0 SW   [migration/0]",
    "   45 root         0 SW<  [kworker/0:1H]",
    "   62 root         0 SW   [mmcqd/0]",
    "   98 root         0 SW<  [rkisp-vir0]",
    "  140 root      1892 S    /sbin/syslogd -n",
    "  144 root      1892 S    /sbin/klogd -n",
    "  180 root      2156 S    /usr/sbin/dropbear -R -B",
    "  201 root     18432 S    /oem/usr/bin/rkipc -a /oem/usr/share/iqfiles",
    "  214 root      2244 S    /usr/bin/buildai-agent --uart /dev/ttyS2",
    "  230 root      1972 S    -sh",
    "  241 root      1968 R    ps",
};

const char* const kTopRows[] = {
    "  201     1 root     S    18432  29%   3% /oem/usr/bin/rkipc -a /oem/usr/share/iqfiles",
    "  214     1 root     S     2244   4%   1% /usr/bin/buildai-agent --uart /dev/ttyS2",
    "  242   230 root     R     1972   3%   1% top -n 1",
    "  180     1 root     S     2156   3%   0% /usr/sbin/dropbear -R -B",
    "  230     1 root     S     1972   3%   0% -sh",
    "    1     0 root     S     1968   3%   0% init",
    "  140     1 root     S     1892   3%   0% /sbin/syslogd -n",
    "  144     1 root     S     1892   3%   0% /sbin/klogd -n",
    "    9     2 root     SW       0   0%   0% [ksoftirqd/0]",
    "   10     2 root     IW       0   0%   0% [rcu_preempt]",
    "   62     2 root     SW       0   0%   0% [mmcqd/0]",
    "   98     2 root     SW<      0   0%   0% [rkisp-vir0]",
};

const char* const kIfconfig[] = {
    "eth0      Link encap:Ethernet  HWaddr 3A:1F:5C:2B:9E:41  ",
    "          inet addr:192.168.1.120  Bcast:192.168.1.255  Mask:255.255.255.0",
    "          UP BROADCAST RUNNING MULTICAST  MTU:1500  Metric:1",
    "          RX packets:1284 errors:0 dropped:0 overruns:0 frame:0",
    "          TX packets:392 errors:0 dropped:0 overruns:0 carrier:0",
    "          collisions:0 txqueuelen:1000 ",
    "          RX bytes:187654 (183.2 KiB)  TX bytes:41208 (40.2 KiB)",
    "          Interrupt:27 ",
    "",
    "lo        Link encap:Local Loopback  ",
    "          inet addr:127.0.0.1  Mask:255.0.0.0",
    "          UP LOOPBACK RUNNING  MTU:65536  Metric:1",
    "          RX packets:16 errors:0 dropped:0 overruns:0 frame:0",
    "          TX packets:16 errors:0 dropped:0 overruns:0 carrier:0",
    "          collisions:0 txqueuelen:1000 ",
    "          RX bytes:1152 (1.1 KiB)  TX bytes:1152 (1.1 KiB)",
    "",
};

const char* const kIpAddr[] = {
    "1: lo: <LOOPBACK,UP,LOWER_UP> mtu 65536 qdisc noqueue qlen 1000",
    "    link/loopback 00:00:00:00:00:00 brd 00:00:00:00:00:00",
    "    inet 127.0.0.1/8 scope host lo",
    "       valid_lft forever preferred_lft forever",
    "2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc mq qlen 1000",
    "    link/ether 3a:1f:5c:2b:9e:41 brd ff:ff:ff:ff:ff:ff",
    "    inet 192.168.1.120/24 brd 192.168.1.255 scope global eth0",
    "       valid_lft forever preferred_lft forever",
};

const char* const kDfLines[] = {
    "Filesystem                Size      Used Available Use% Mounted on",
    "/dev/root                32.0M     28.4M      3.6M  89% /",
    "devtmpfs                 30.6M         0     30.6M   0% /dev",
    "tmpfs                    30.7M         0     30.7M   0% /dev/shm",
    "tmpfs                    30.7M     32.0K     30.7M   0% /tmp",
    "tmpfs                    30.7M    108.0K     30.6M   0% /run",
    "/dev/mmcblk0p6          128.0M     52.1M     75.9M  41% /oem",
    "/dev/mmcblk0p7            7.1G    128.0K      7.1G   0% /userdata",
};

const char* const kBdInfo[] = {
    "arch_number = 0x00000000",
    "boot_params = 0x00000100",
    "DRAM bank   = 0x00000000",
    "-> start    = 0x00000000",
    "-> size     = 0x04000000",
    "baudrate    = 115200 bps",
    "TLB addr    = 0x03ff0000",
    "relocaddr   = 0x03b2c000",
    "reloc off   = 0x02b2c000",
    "irq_sp      = 0x02f7b6a0",
    "sp start    = 0x02f7b690",
    "Early malloc usage: 2c8 / 4000",
    "fdt_blob    = 0x02f7b6b8",
};

const char* const kMmcInfo[] = {
    "Device: dwmmc@ffaa0000",
    "Manufacturer ID: 6f",
    "OEM: 0",
    "Name: SD8GB ",
    "Bus Speed: 50000000",
    "Mode: SD High Speed (50MHz)",
    "Rd Block Len: 512",
    "SD version 3.0",
    "High Capacity: Yes",
    "Capacity: 7.4 GiB",
    "Bus Width: 4-bit",
    "Erase Group Size: 512 Bytes",
};

const char* const kUBootBootLines[] = {
    "switch to partitions #0, OK",
    "mmc0 is current device",
    "Scanning mmc 0:5...",
    "## Booting kernel from Legacy Image at 02008000 ...",
    "   Image Name:   Linux-5.10.160",
    "   Image Type:   ARM Linux Kernel Image (uncompressed)",
    "   Data Size:    4205016 Bytes = 4 MiB",
    "   Load Address: 02008000",
    "   Entry Point:  02008000",
    "   Verifying Checksum ... OK",
    "## Flattened Device Tree blob at 08300000",
    "   Booting using the fdt blob at 0x8300000",
    "   Loading Kernel Image",
    "   Using Device Tree in place at 08300000, end 0830b3e3",
    "Adding bank: 0x00000000 - 0x04000000 (size: 0x04000000)",
    "Total: 812.257 ms",
    "",
    "Starting kernel ...",
    "",
};

const char* const kUBootHelp[] = {
    "bdinfo  - print Board Info structure",
    "boot    - boot default, i.e., run 'bootcmd'",
    "help    - print command description/usage",
    "md      - memory display",
    "mmc     - MMC sub system",
    "printenv- print environment variables",
    "reset   - Perform RESET of the CPU",
    "run     - run commands in an environment variable",
    "saveenv - save environment variables to persistent storage",
    "setenv  - set environment variables",
    "version - print monitor, compiler and linker version",
};

const char* const kMcuHelp[] = {
    "Commands:",
    "  help                 this text",
    "  version              firmware version",
    "  reset                restart the MCU",
    "  led on|off|toggle    drive the status LED",
    "  adc                  read the 8 ADC channels",
    "  temp                 die temperature",
    "  uptime               seconds since reset",
    "  telemetry on|off     periodic telemetry line every 2 s",
    "  echo <text>          echo back",
    "  AT, AT+GMR, ATE0/1, AT+RST",
};

const QLatin1String kMcuVersion("BuildAI MCU shell v1.0 (STM32F4 @168MHz)");
const QLatin1String kUBootVersion("U-Boot 2017.09-g9c9ea3b6-dirty (Sep 17 2026 - 10:21:43 +0800)");

/// Directory listing of the fake root file system ("" = empty directory).
const struct
{
    const char* path;
    const char* entries;
} kDirectories[] = {
    {"/", "bin data dev etc lib linuxrc media mnt oem opt proc root run sbin sys tmp userdata usr var"},
    {"/bin", "ash busybox cat chmod cp date dd df dmesg echo grep kill ln ls mkdir mount mv ps rm sh sleep umount "
             "uname"},
    {"/data", ""},
    {"/dev", "console cpu_dma_latency fd full kmsg log mem mmcblk0 mmcblk0p1 mmcblk0p2 mmcblk0p3 mmcblk0p4 mmcblk0p5 "
             "mmcblk0p6 mmcblk0p7 null ptmx pts random rkipc rtc0 shm stderr stdin stdout tty ttyFIQ0 ttyS0 ttyS1 "
             "ttyS2 urandom video0 video1 video11 watchdog zero"},
    {"/etc", "fstab group hostname hosts init.d inittab passwd profile resolv.conf shadow ssl"},
    {"/etc/init.d", "S01syslogd S02klogd S02sysctl S40network S50dropbear S60rkipc S99buildai"},
    {"/lib", "ld-uClibc.so.1 libc.so.1 libm.so.1 libpthread.so.1 modules"},
    {"/media", ""},
    {"/mnt", ""},
    {"/oem", "usr"},
    {"/oem/usr", "bin lib share"},
    {"/oem/usr/bin", "rkipc rkipc_client rkmedia_test"},
    {"/oem/usr/share", "iqfiles rkipc.ini"},
    {"/opt", ""},
    {"/proc", "cmdline cpuinfo meminfo mounts uptime version"},
    {"/root", "app.log"},
    {"/run", ""},
    {"/sbin", "getty halt ifconfig init insmod klogd modprobe poweroff reboot route syslogd"},
    {"/sys", "block bus class dev devices firmware fs kernel module power"},
    {"/tmp", ""},
    {"/userdata", "logs media.cfg recording"},
    {"/userdata/logs", "agent.log rkipc.log"},
    {"/userdata/recording", ""},
    {"/usr", "bin lib sbin share"},
    {"/usr/bin", "buildai-agent dropbearkey free killall top uptime"},
    {"/usr/sbin", "dropbear"},
    {"/var", "lock log run tmp"},
};

const struct
{
    const char* path;
    const char* content;
} kFiles[] = {
    {"/etc/hostname", "rv1106\n"},
    {"/etc/hosts", "127.0.0.1\tlocalhost\n127.0.1.1\trv1106\n"},
    {"/etc/passwd", "root:x:0:0:root:/root:/bin/sh\ndaemon:x:1:1:daemon:/usr/sbin:/bin/false\n"
                    "nobody:x:65534:65534:nobody:/nonexistent:/bin/false\n"},
    {"/etc/group", "root:x:0:\ndaemon:x:1:\nnogroup:x:65534:\n"},
    {"/etc/fstab", "# <file system>\t<mount pt>\t<type>\t<options>\t<dump>\t<pass>\n"
                   "/dev/root\t/\t\text2\trw,noauto\t0\t1\n"
                   "proc\t\t/proc\t\tproc\tdefaults\t0\t0\n"
                   "devpts\t\t/dev/pts\tdevpts\tdefaults,gid=5,mode=620,ptmxmode=0666\t0\t0\n"
                   "tmpfs\t\t/dev/shm\ttmpfs\tmode=0777\t0\t0\n"
                   "tmpfs\t\t/tmp\t\ttmpfs\tmode=1777\t0\t0\n"
                   "/dev/mmcblk0p6\t/oem\t\text4\tdefaults\t0\t2\n"
                   "/dev/mmcblk0p7\t/userdata\text4\tdefaults\t0\t2\n"},
    {"/etc/profile", "export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/oem/usr/bin\nexport PS1='[\\u@\\h:\\w]\\$ '\n"
                     "umask 022\n"},
    {"/etc/inittab", "::sysinit:/etc/init.d/rcS\nttyFIQ0::respawn:/sbin/getty -L ttyFIQ0 1500000 vt100\n"
                     "::ctrlaltdel:/sbin/reboot\n::shutdown:/etc/init.d/rcK\n"},
    {"/etc/resolv.conf", "nameserver 192.168.1.1\nnameserver 8.8.8.8\n"},
    {"/proc/cmdline", "earlycon=uart8250,mmio32,0xff4c0000 console=ttyFIQ0 root=/dev/mmcblk0p5 rootfstype=squashfs "
                      "rootwait snd_aloop.index=7\n"},
    {"/proc/version", "Linux version 5.10.160 (buildai@ci) (arm-rockchip830-linux-uclibcgnueabihf-gcc (crosstool-NG "
                      "1.24.0) 8.3.0, GNU ld (crosstool-NG 1.24.0) 2.32) #1 PREEMPT Wed Sep 17 10:21:43 CST 2026\n"},
    {"/proc/mounts", "/dev/root / squashfs ro,relatime 0 0\nproc /proc proc rw,relatime 0 0\n"
                     "devtmpfs /dev devtmpfs rw,relatime,size=31324k,nr_inodes=7831,mode=755 0 0\n"
                     "/dev/mmcblk0p6 /oem ext4 rw,relatime 0 0\n/dev/mmcblk0p7 /userdata ext4 rw,relatime 0 0\n"},
    {"/root/app.log", "2026-09-17 10:30:01 agent started (uart /dev/ttyS2, 1500000 8N1)\n"
                      "2026-09-17 10:30:02 camera pipeline ready 1920x1080@30\n"
                      "2026-09-17 10:31:15 inference: 42 objects, 18.3 ms\n"},
    {"/userdata/media.cfg", "[video0]\nresolution=1920x1080\nfps=30\ncodec=h265\n[audio]\nrate=16000\n"},
    {"/oem/usr/share/rkipc.ini", "[video.0]\nwidth = 1920\nheight = 1080\nmax_rate = 30\n[isp]\niq_dir = "
                                 "/oem/usr/share/iqfiles\n"},
};

QStringList linesOf(const char* const* begin, std::size_t count)
{
    QStringList list;
    list.reserve(static_cast<qsizetype>(count));
    for (std::size_t i = 0; i < count; ++i) {
        list.append(QString::fromUtf8(begin[i]));
    }
    return list;
}

#define SU_LINES(array) linesOf(array, sizeof(array) / sizeof(array[0]))

QString normalizePath(const QString& path)
{
    QStringList parts;
    const QStringList raw = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString& part : raw) {
        if (part == QLatin1String(".")) {
            continue;
        }
        if (part == QLatin1String("..")) {
            if (!parts.isEmpty()) {
                parts.removeLast();
            }
            continue;
        }
        parts.append(part);
    }
    return QLatin1Char('/') + parts.join(QLatin1Char('/'));
}

std::optional<QStringList> directoryEntries(const QString& path)
{
    for (const auto& dir : kDirectories) {
        if (path == QLatin1String(dir.path)) {
            return QString::fromLatin1(dir.entries).split(QLatin1Char(' '), Qt::SkipEmptyParts);
        }
    }
    return std::nullopt;
}

std::optional<QString> fileContent(const QString& path)
{
    for (const auto& file : kFiles) {
        if (path == QLatin1String(file.path)) {
            return QString::fromUtf8(file.content);
        }
    }
    return std::nullopt;
}

bool isDirectory(const QString& path)
{
    return directoryEntries(path).has_value();
}

bool isFile(const QString& path)
{
    return fileContent(path).has_value() || (path.startsWith(QLatin1String("/dev/")) && !isDirectory(path)) ||
           path.startsWith(QLatin1String("/bin/")) || path.startsWith(QLatin1String("/sbin/")) ||
           path.startsWith(QLatin1String("/usr/")) || path.startsWith(QLatin1String("/oem/usr/bin/"));
}

/// Deterministic pseudo size for `ls -l`.
int fakeSize(const QString& path)
{
    return 128 + static_cast<int>(qHash(path) % 40000u);
}

QString longListingLine(const QString& dirPath, const QString& name)
{
    const QString full = dirPath == QLatin1String("/") ? QLatin1Char('/') + name : dirPath + QLatin1Char('/') + name;
    QString perms;
    QString size;
    if (isDirectory(full)) {
        perms = QStringLiteral("drwxr-xr-x");
        size = QStringLiteral("1024");
    } else if (full.startsWith(QLatin1String("/dev/"))) {
        perms = name.startsWith(QLatin1String("mmcblk")) ? QStringLiteral("brw-rw----") : QStringLiteral("crw-rw----");
        size = QStringLiteral("%1, %2").arg(name.startsWith(QLatin1String("tty")) ? 4 : 179).arg(qHash(name) % 64u);
    } else if (full.startsWith(QLatin1String("/bin/")) || full.startsWith(QLatin1String("/sbin/")) ||
               full.contains(QLatin1String("/bin/"))) {
        perms = QStringLiteral("-rwxr-xr-x");
        size = QString::number(fakeSize(full) * 16);
    } else {
        perms = QStringLiteral("-rw-r--r--");
        size = QString::number(fakeSize(full));
    }
    return QStringLiteral("%1    1 root     root     %2 Sep 17 10:21 %3").arg(perms, size.rightJustified(9), name);
}

QString formatUptime(qint64 ms)
{
    const qint64 secs = ms / 1000;
    const qint64 mins = secs / 60;
    const qint64 hours = mins / 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2").arg(hours).arg(mins % 60, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1 min").arg(mins);
}

QString hexWord(quint32 value)
{
    return QStringLiteral("%1").arg(value, 8, 16, QLatin1Char('0'));
}

} // namespace

// =========================================================================================
// Static helpers
// =========================================================================================

QString DeviceSimulator::portPrefix()
{
    return kPrefix;
}

bool DeviceSimulator::isSimulatedPort(const QString& portName)
{
    return portName.trimmed().startsWith(kPrefix, Qt::CaseInsensitive);
}

std::optional<DeviceSimulator::Kind> DeviceSimulator::kindFromPortName(const QString& portName)
{
    if (!isSimulatedPort(portName)) {
        return std::nullopt;
    }
    const QString suffix = portName.trimmed().mid(kPrefix.size()).trimmed().toLower();
    if (suffix == QLatin1String("loopback")) {
        return Kind::Loopback;
    }
    if (suffix == QLatin1String("linux")) {
        return Kind::Linux;
    }
    if (suffix == QLatin1String("uboot")) {
        return Kind::UBoot;
    }
    if (suffix == QLatin1String("mcu")) {
        return Kind::Mcu;
    }
    return std::nullopt;
}

QString DeviceSimulator::portName(Kind kind)
{
    switch (kind) {
    case Kind::Loopback:
        return QStringLiteral("SIM:loopback");
    case Kind::Linux:
        return QStringLiteral("SIM:linux");
    case Kind::UBoot:
        return QStringLiteral("SIM:uboot");
    case Kind::Mcu:
        break;
    }
    return QStringLiteral("SIM:mcu");
}

QString DeviceSimulator::description(Kind kind)
{
    switch (kind) {
    case Kind::Loopback:
        return QCoreApplication::translate("DeviceSimulator", "Simulated loopback (echoes every byte)");
    case Kind::Linux:
        return QCoreApplication::translate("DeviceSimulator", "Simulated Rockchip Linux console");
    case Kind::UBoot:
        return QCoreApplication::translate("DeviceSimulator", "Simulated U-Boot prompt (boots into Linux)");
    case Kind::Mcu:
        break;
    }
    return QCoreApplication::translate("DeviceSimulator", "Simulated MCU firmware shell");
}

QList<SerialPortEntry> DeviceSimulator::entries()
{
    QList<SerialPortEntry> list;
    for (Kind kind : {Kind::Loopback, Kind::Linux, Kind::UBoot, Kind::Mcu}) {
        SerialPortEntry entry;
        entry.portName = portName(kind);
        entry.description = description(kind);
        entry.manufacturer = kManufacturer;
        entry.systemLocation = QStringLiteral("sim://") + entry.portName.mid(kPrefix.size()).toLower();
        list.append(entry);
    }
    return list;
}

bool DeviceSimulator::isPresent(const QString& portName)
{
    if (!kindFromPortName(portName)) {
        return false;
    }
    QHash<QString, qint64>& table = downUntilTable();
    const auto it = table.constFind(presenceKey(portName));
    if (it == table.constEnd()) {
        return true;
    }
    if (it.value() < 0 || QDateTime::currentMSecsSinceEpoch() < it.value()) {
        return false;
    }
    table.remove(presenceKey(portName));
    return true;
}

void DeviceSimulator::markPresent(const QString& portName)
{
    downUntilTable().remove(presenceKey(portName));
}

void DeviceSimulator::markAbsent(const QString& portName, int forMs)
{
    const qint64 until = forMs < 0 ? -1 : QDateTime::currentMSecsSinceEpoch() + forMs;
    downUntilTable().insert(presenceKey(portName), until);
}

// =========================================================================================
// Construction / basic state
// =========================================================================================

DeviceSimulator::DeviceSimulator(Kind kind, qint32 baudRate, QObject* parent)
    : QObject(parent)
    , m_kind(kind)
    , m_baud(baudRate > 0 ? baudRate : kDefaultBaud)
{
    m_paceTimer.setSingleShot(true);
    m_paceTimer.setTimerType(Qt::PreciseTimer);
    connect(&m_paceTimer, &QTimer::timeout, this, &DeviceSimulator::onPaceTimer);

    m_telemetryTimer.setInterval(kTelemetryIntervalMs);
    connect(&m_telemetryTimer, &QTimer::timeout, this, &DeviceSimulator::onTelemetryTimer);

    m_countdownTimer.setInterval(1000);
    connect(&m_countdownTimer, &QTimer::timeout, this, &DeviceSimulator::onCountdownTimer);

    m_delayTimer.setSingleShot(true);
    connect(&m_delayTimer, &QTimer::timeout, this, &DeviceSimulator::onDelayedAction);

    resetEnvironment();
}

DeviceSimulator::~DeviceSimulator() = default;

DeviceSimulator::Kind DeviceSimulator::kind() const
{
    return m_kind;
}

qint32 DeviceSimulator::baudRate() const
{
    return m_baud;
}

void DeviceSimulator::setBaudRate(qint32 baud)
{
    if (baud > 0) {
        m_baud = baud;
    }
}

bool DeviceSimulator::isStarted() const
{
    return m_started;
}

void DeviceSimulator::start()
{
    if (m_started) {
        return;
    }
    m_started = true;
    m_uptime.start();
    m_lineBuffer.clear();
    m_utf8Pending.clear();
    qCInfo(lcSerial) << "simulator" << portName(m_kind) << "powered on at" << m_baud << "baud";

    switch (m_kind) {
    case Kind::Loopback:
        m_stage = Stage::Shell;
        break;
    case Kind::Linux:
        bootLinux();
        break;
    case Kind::UBoot:
        m_uBootMode = true;
        showUBootBanner();
        beginCountdown();
        break;
    case Kind::Mcu:
        showMcuBanner();
        m_stage = Stage::Shell;
        prompt();
        break;
    }
}

void DeviceSimulator::receive(const QByteArray& hostToDevice)
{
    if (!m_started || m_stage == Stage::Off || m_stage == Stage::Down) {
        return;
    }
    if (m_kind == Kind::Loopback) {
        emitRaw(hostToDevice);
        return;
    }
    for (const char c : hostToDevice) {
        handleByte(c);
    }
}

void DeviceSimulator::flushOutput()
{
    QByteArray all;
    for (const Segment& segment : m_segments) {
        all += segment.bytes;
    }
    m_segments.clear();
    m_paceTimer.stop();
    if (!all.isEmpty()) {
        emit dataReady(all);
    }
}

QByteArray DeviceSimulator::pendingOutput() const
{
    QByteArray all;
    for (const Segment& segment : m_segments) {
        all += segment.bytes;
    }
    return all;
}

// =========================================================================================
// Output queue and pacing
// =========================================================================================

int DeviceSimulator::bytesPerTick() const
{
    return qMax(1, m_baud / kBitsPerByte / kTicksPerSecond);
}

void DeviceSimulator::emitRaw(const QByteArray& bytes, int delayBeforeMs)
{
    if (bytes.isEmpty() && delayBeforeMs <= 0) {
        return;
    }
    Segment segment;
    segment.bytes = bytes;
    segment.delayMs = qMax(0, delayBeforeMs);
    m_segments.append(segment);
    if (!m_paceTimer.isActive()) {
        m_paceTimer.start(kPaceIntervalMs);
    }
}

void DeviceSimulator::emitText(const QString& text)
{
    emitRaw(text.toUtf8());
}

void DeviceSimulator::emitLine(const QString& line)
{
    emitRaw(line.toUtf8() + QByteArrayLiteral("\r\n"));
}

void DeviceSimulator::emitLines(const QStringList& lines, int delayEveryNLines, int delayMs)
{
    int counter = 0;
    for (const QString& line : lines) {
        const bool pauseHere = delayEveryNLines > 0 && counter > 0 && (counter % delayEveryNLines) == 0;
        emitRaw(line.toUtf8() + QByteArrayLiteral("\r\n"), pauseHere ? delayMs : 0);
        ++counter;
    }
}

void DeviceSimulator::pause(int ms)
{
    emitRaw(QByteArray(), ms);
}

void DeviceSimulator::onPaceTimer()
{
    int budget = bytesPerTick();
    QByteArray out;
    while (budget > 0 && !m_segments.isEmpty()) {
        Segment& front = m_segments.first();
        if (front.delayMs > 0) {
            // A pause marker: deliver what we have, then wait before continuing.
            const int wait = front.delayMs;
            front.delayMs = 0;
            if (!out.isEmpty()) {
                emit dataReady(out);
            }
            m_paceTimer.start(wait);
            return;
        }
        const int take = static_cast<int>(qMin<qsizetype>(budget, front.bytes.size()));
        out += front.bytes.left(take);
        front.bytes.remove(0, take);
        budget -= take;
        if (front.bytes.isEmpty()) {
            m_segments.removeFirst();
        }
    }
    if (!out.isEmpty()) {
        emit dataReady(out);
    }
    if (!m_segments.isEmpty()) {
        m_paceTimer.start(kPaceIntervalMs);
    }
}

void DeviceSimulator::runDelayed(int ms, std::function<void()> action)
{
    m_delayedAction = std::move(action);
    m_delayTimer.start(qMax(0, ms));
}

void DeviceSimulator::cancelDelayed()
{
    m_delayTimer.stop();
    m_delayedAction = nullptr;
}

void DeviceSimulator::onDelayedAction()
{
    std::function<void()> action = std::move(m_delayedAction);
    m_delayedAction = nullptr;
    if (action) {
        action();
    }
}

// =========================================================================================
// Input: canonical line editing performed by the device
// =========================================================================================

void DeviceSimulator::handleByte(char c)
{
    const auto byte = static_cast<unsigned char>(c);

    if (m_stage == Stage::Countdown) {
        // Any key stops autoboot; the key itself is discarded (like real U-Boot).
        m_countdownTimer.stop();
        emitText(QStringLiteral("\r\n"));
        m_stage = Stage::Shell;
        m_uBootMode = true;
        prompt();
        return;
    }
    if (m_stage == Stage::Sleeping) {
        if (byte == 0x03) {
            cancelDelayed();
            emitText(QStringLiteral("^C\r\n"));
            m_stage = Stage::Shell;
            prompt();
        }
        return;
    }
    if (m_stage == Stage::Booting) {
        return;
    }

    // Escape sequences (arrow keys, function keys) are consumed and ignored.
    if (m_escState != 0) {
        if (m_escState == 1) {
            if (byte == '[') {
                m_escState = 2;
            } else if (byte == 'O') {
                m_escState = 3;
            } else {
                m_escState = 0;   // ESC <key>: Alt-key, ignored together with the key
            }
            return;
        }
        if (m_escState == 2) {
            if (byte >= 0x40 && byte <= 0x7E) {
                m_escState = 0;
            }
            return;
        }
        m_escState = 0;   // SS3: one final byte
        return;
    }
    if (byte == 0x1B) {
        m_escState = 1;
        return;
    }

    if (m_swallowLf) {
        m_swallowLf = false;
        if (byte == '\n') {
            return;
        }
    }

    switch (byte) {
    case '\r':
        m_swallowLf = true;
        executeLine();
        return;
    case '\n':
        executeLine();
        return;
    case 0x7F:
    case 0x08:
        eraseLastChar();
        return;
    case 0x15:   // Ctrl+U
        clearLine();
        return;
    case 0x03:   // Ctrl+C
        if (m_stage == Stage::Shell && m_kind != Kind::Mcu) {
            m_lineBuffer.clear();
            m_utf8Pending.clear();
            emitText(QStringLiteral("^C\r\n"));
            prompt();
        }
        return;
    case 0x0C:   // Ctrl+L
        if (m_stage == Stage::Shell && isLinuxShell()) {
            emitText(QStringLiteral("\x1b[H\x1b[2J"));
            prompt();
            emitText(m_lineBuffer);
        }
        return;
    case 0x04:   // Ctrl+D
        if (m_stage == Stage::Shell && isLinuxShell() && m_lineBuffer.isEmpty()) {
            emitText(QStringLiteral("\r\n"));
            showLogin();
        }
        return;
    case '\t':
        return;
    default:
        break;
    }

    if (byte < 0x20) {
        return;   // other control characters are ignored
    }

    // Printable ASCII or a UTF-8 byte: collect complete code points.
    m_utf8Pending.append(c);
    const auto lead = static_cast<unsigned char>(m_utf8Pending.at(0));
    int needed = 1;
    if (lead >= 0xF0) {
        needed = 4;
    } else if (lead >= 0xE0) {
        needed = 3;
    } else if (lead >= 0xC0) {
        needed = 2;
    } else if (lead >= 0x80) {
        m_utf8Pending.clear();   // stray continuation byte
        return;
    }
    if (m_utf8Pending.size() < needed) {
        return;
    }
    const QString text = QString::fromUtf8(m_utf8Pending);
    const QByteArray raw = m_utf8Pending;
    m_utf8Pending.clear();
    if (text.contains(QChar(0xFFFD))) {
        return;   // invalid sequence: dropped
    }
    m_lineBuffer += text;
    if (m_echo && m_stage != Stage::Password) {
        emitRaw(raw);
    }
}

void DeviceSimulator::eraseLastChar()
{
    m_utf8Pending.clear();
    if (m_lineBuffer.isEmpty()) {
        return;
    }
    int width = 1;
    if (m_lineBuffer.size() >= 2 && m_lineBuffer.at(m_lineBuffer.size() - 1).isLowSurrogate() &&
        m_lineBuffer.at(m_lineBuffer.size() - 2).isHighSurrogate()) {
        const char32_t cp = QChar::surrogateToUcs4(m_lineBuffer.at(m_lineBuffer.size() - 2),
                                                   m_lineBuffer.at(m_lineBuffer.size() - 1));
        width = qMax(1, Terminal::charWidth(cp));
        m_lineBuffer.chop(2);
    } else {
        width = qMax(1, Terminal::charWidth(m_lineBuffer.back().unicode()));
        m_lineBuffer.chop(1);
    }
    if (m_echo && m_stage != Stage::Password) {
        emitText(QString(width, QLatin1Char('\b')) + QString(width, QLatin1Char(' ')) +
                 QString(width, QLatin1Char('\b')));
    }
}

void DeviceSimulator::clearLine()
{
    m_utf8Pending.clear();
    while (!m_lineBuffer.isEmpty()) {
        eraseLastChar();
    }
}

void DeviceSimulator::executeLine()
{
    const QString line = m_lineBuffer;
    m_lineBuffer.clear();
    m_utf8Pending.clear();
    if (m_echo || m_stage == Stage::Password) {
        emitText(QStringLiteral("\r\n"));
    }
    handleLine(line);
}

bool DeviceSimulator::isLinuxShell() const
{
    return (m_kind == Kind::Linux) || (m_kind == Kind::UBoot && !m_uBootMode);
}

void DeviceSimulator::prompt()
{
    switch (m_kind) {
    case Kind::Loopback:
        return;
    case Kind::Mcu:
        emitText(QStringLiteral("> "));
        return;
    case Kind::UBoot:
        if (m_uBootMode) {
            emitText(QStringLiteral("=> "));
            return;
        }
        break;
    case Kind::Linux:
        break;
    }
    const QString cwd = m_cwd == QLatin1String("/root") ? QStringLiteral("~") : m_cwd;
    emitText(QStringLiteral("[root@%1:%2]# ").arg(kHostName, cwd));
}

void DeviceSimulator::handleLine(const QString& line)
{
    switch (m_stage) {
    case Stage::Login: {
        const QString user = line.trimmed();
        if (user.isEmpty()) {
            emitText(QStringLiteral("%1 login: ").arg(kHostName));
            return;
        }
        m_user = user;
        m_stage = Stage::Password;
        emitText(QStringLiteral("Password: "));
        return;
    }
    case Stage::Password:
        m_stage = Stage::Shell;
        m_cwd = QStringLiteral("/root");
        emitLine(QStringLiteral("Welcome to BuildAI Linux (%1), kernel 5.10.160").arg(kHostName));
        emitLine(QStringLiteral("Last login: %1 on ttyFIQ0")
                     .arg(QLocale::c().toString(QDateTime::currentDateTime(),
                                                QStringLiteral("ddd MMM d HH:mm:ss yyyy"))));
        prompt();
        return;
    case Stage::Shell:
        break;
    case Stage::Off:
    case Stage::Booting:
    case Stage::Countdown:
    case Stage::Sleeping:
    case Stage::Down:
        return;
    }

    const QString command = line.trimmed();
    if (m_kind == Kind::Mcu) {
        handleMcuCommand(command);
    } else if (m_kind == Kind::UBoot && m_uBootMode) {
        handleUBootCommand(command);
    } else {
        handleShellCommand(command);
    }
}

// =========================================================================================
// Linux shell
// =========================================================================================

void DeviceSimulator::resetEnvironment()
{
    m_env.clear();
    m_env.insert(QStringLiteral("HOME"), QStringLiteral("/root"));
    m_env.insert(QStringLiteral("HOSTNAME"), kHostName);
    m_env.insert(QStringLiteral("PATH"), QStringLiteral("/bin:/sbin:/usr/bin:/usr/sbin:/oem/usr/bin"));
    m_env.insert(QStringLiteral("PS1"), QStringLiteral("[\\u@\\h:\\w]\\$ "));
    m_env.insert(QStringLiteral("PWD"), QStringLiteral("/root"));
    m_env.insert(QStringLiteral("SHELL"), QStringLiteral("/bin/sh"));
    m_env.insert(QStringLiteral("TERM"), QStringLiteral("vt100"));
    m_env.insert(QStringLiteral("USER"), QStringLiteral("root"));

    m_ubootEnv.clear();
    m_ubootEnv.insert(QStringLiteral("arch"), QStringLiteral("arm"));
    m_ubootEnv.insert(QStringLiteral("baudrate"), QStringLiteral("115200"));
    m_ubootEnv.insert(QStringLiteral("board"), QStringLiteral("evb_rv1106"));
    m_ubootEnv.insert(QStringLiteral("board_name"), QStringLiteral("evb_rv1106"));
    m_ubootEnv.insert(QStringLiteral("boot_targets"), QStringLiteral("mmc1 mmc0 mtd2 mtd1 mtd0 usb0 pxe dhcp"));
    m_ubootEnv.insert(QStringLiteral("bootargs"),
                      QStringLiteral("earlycon=uart8250,mmio32,0xff4c0000 console=ttyFIQ0 root=/dev/mmcblk0p5 "
                                     "rootfstype=squashfs rootwait snd_aloop.index=7"));
    m_ubootEnv.insert(QStringLiteral("bootcmd"), QStringLiteral("boot_fit"));
    m_ubootEnv.insert(QStringLiteral("bootdelay"), QStringLiteral("3"));
    m_ubootEnv.insert(QStringLiteral("cpu"), QStringLiteral("armv7"));
    m_ubootEnv.insert(QStringLiteral("ethaddr"), QStringLiteral("3a:1f:5c:2b:9e:41"));
    m_ubootEnv.insert(QStringLiteral("fdt_addr_r"), QStringLiteral("0x08300000"));
    m_ubootEnv.insert(QStringLiteral("kernel_addr_r"), QStringLiteral("0x02008000"));
    m_ubootEnv.insert(QStringLiteral("ramdisk_addr_r"), QStringLiteral("0x0a200000"));
    m_ubootEnv.insert(QStringLiteral("serial#"), QStringLiteral("1a2b3c4d5e6f7081"));
    m_ubootEnv.insert(QStringLiteral("soc"), QStringLiteral("rv1106"));
    m_ubootEnv.insert(QStringLiteral("stderr"), QStringLiteral("serial"));
    m_ubootEnv.insert(QStringLiteral("stdin"), QStringLiteral("serial"));
    m_ubootEnv.insert(QStringLiteral("stdout"), QStringLiteral("serial"));
    m_ubootEnv.insert(QStringLiteral("vendor"), QStringLiteral("rockchip"));
}

QString DeviceSimulator::resolvePath(const QString& path) const
{
    if (path.isEmpty()) {
        return m_cwd;
    }
    QString p = path;
    if (p == QLatin1String("~") || p.startsWith(QLatin1String("~/"))) {
        p = QStringLiteral("/root") + p.mid(1);
    }
    if (!p.startsWith(QLatin1Char('/'))) {
        p = m_cwd + QLatin1Char('/') + p;
    }
    return normalizePath(p);
}

QString DeviceSimulator::expandVariables(const QString& text) const
{
    static const QRegularExpression re(QStringLiteral("\\$(\\{([A-Za-z_][A-Za-z0-9_]*)\\}|([A-Za-z_][A-Za-z0-9_]*))"));
    QString out;
    qsizetype last = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(last, m.capturedStart() - last);
        const QString name = m.captured(2).isEmpty() ? m.captured(3) : m.captured(2);
        out += m_env.value(name);
        last = m.capturedEnd();
    }
    out += text.mid(last);
    return out;
}

void DeviceSimulator::bootLinux()
{
    m_uBootMode = false;
    m_stage = Stage::Booting;
    m_telemetry = false;
    m_telemetryTimer.stop();
    m_countdownTimer.stop();
    m_cwd = QStringLiteral("/root");
    m_user.clear();

    QStringList kernel = SU_LINES(kKernelBoot);
    // A realistic boot takes a few seconds regardless of the baud rate: pause every few lines.
    emitLines(kernel, 6, 90);
    pause(250);
    emitLines(SU_LINES(kInitLines), 2, 120);
    pause(200);
    emitLine(QString());
    showLogin();
}

void DeviceSimulator::showLogin()
{
    m_stage = Stage::Login;
    m_lineBuffer.clear();
    m_utf8Pending.clear();
    m_user.clear();
    emitLine(QStringLiteral("BuildAI Linux 5.10.160 %1 ttyFIQ0").arg(kHostName));
    emitLine(QString());
    emitText(QStringLiteral("%1 login: ").arg(kHostName));
}

void DeviceSimulator::reboot(int downMs)
{
    m_stage = Stage::Down;
    m_telemetry = false;
    m_telemetryTimer.stop();
    m_countdownTimer.stop();
    cancelDelayed();
    const QString name = portName(m_kind);
    qCInfo(lcSerial) << "simulator" << name << "going down for" << downMs << "ms";
    runDelayed(kVanishDelayMs, [this, downMs, name]() {
        markAbsent(name, downMs);
        emit vanished(downMs);
    });
}

void DeviceSimulator::startProgress()
{
    m_stage = Stage::Sleeping;
    m_progressStep = 0;
    progressStep();
}

void DeviceSimulator::progressStep()
{
    if (m_stage != Stage::Sleeping) {
        return;
    }
    static const char spinner[] = {'|', '/', '-', '\\'};
    const int pct = qMin(m_progressStep, kProgressSteps);
    const int filled = pct * 40 / kProgressSteps;
    const QString bar = QStringLiteral("\rDownloading firmware  [%1%2] %3% %4")
                            .arg(QString(filled, QLatin1Char('#')), QString(40 - filled, QLatin1Char('.')),
                                 QString::number(pct).rightJustified(3),
                                 QChar::fromLatin1(spinner[m_progressStep % 4]));
    emitText(bar);
    if (m_progressStep >= kProgressSteps) {
        emitText(QStringLiteral("\r\n"));
        emitLine(QStringLiteral("Download complete: 4205016 bytes, sha256 OK."));
        m_stage = Stage::Shell;
        prompt();
        return;
    }
    ++m_progressStep;
    runDelayed(kProgressStepMs, [this]() { progressStep(); });
}

bool DeviceSimulator::runShellTextCommand(const QString& cmd, const QStringList& args, QStringList& out)
{
    if (cmd == QLatin1String("uname")) {
        if (args.contains(QLatin1String("-a"))) {
            out << QStringLiteral("Linux %1 5.10.160 #1 PREEMPT Wed Sep 17 10:21:43 CST 2026 armv7l GNU/Linux")
                       .arg(kHostName);
        } else if (args.contains(QLatin1String("-r"))) {
            out << QStringLiteral("5.10.160");
        } else if (args.contains(QLatin1String("-m"))) {
            out << QStringLiteral("armv7l");
        } else if (args.contains(QLatin1String("-n"))) {
            out << kHostName;
        } else {
            out << QStringLiteral("Linux");
        }
        return true;
    }
    if (cmd == QLatin1String("cat")) {
        if (args.isEmpty()) {
            out << QStringLiteral("Usage: cat [FILE]...");
            return true;
        }
        for (const QString& arg : args) {
            if (arg.startsWith(QLatin1Char('-'))) {
                continue;
            }
            const QString path = resolvePath(arg);
            if (path == QLatin1String("/proc/uptime")) {
                const double secs = static_cast<double>(m_uptime.elapsed()) / 1000.0;
                out << QStringLiteral("%1 %2").arg(secs, 0, 'f', 2).arg(secs * 0.94, 0, 'f', 2);
                continue;
            }
            if (path == QLatin1String("/proc/cpuinfo")) {
                out << SU_LINES(kCpuInfo);
                continue;
            }
            if (path == QLatin1String("/proc/meminfo")) {
                out << SU_LINES(kMemInfo);
                continue;
            }
            if (const auto content = fileContent(path)) {
                QString text = *content;
                if (text.endsWith(QLatin1Char('\n'))) {
                    text.chop(1);
                }
                out << text.split(QLatin1Char('\n'));
                continue;
            }
            if (isDirectory(path)) {
                out << QStringLiteral("cat: read error: Is a directory");
                continue;
            }
            out << QStringLiteral("cat: can't open '%1': No such file or directory").arg(arg);
        }
        return true;
    }
    if (cmd == QLatin1String("ls")) {
        bool longFormat = false;
        QStringList targets;
        for (const QString& arg : args) {
            if (arg.startsWith(QLatin1Char('-'))) {
                longFormat = longFormat || arg.contains(QLatin1Char('l'));
                continue;
            }
            targets << arg;
        }
        if (targets.isEmpty()) {
            targets << QString();
        }
        const bool multiple = targets.size() > 1;
        bool first = true;
        for (const QString& target : targets) {
            const QString path = resolvePath(target);
            // Glob support for patterns such as /dev/tty* (the shipped quick command uses it).
            if (target.contains(QLatin1Char('*')) || target.contains(QLatin1Char('?'))) {
                const qsizetype slash = path.lastIndexOf(QLatin1Char('/'));
                const QString dir = slash <= 0 ? QStringLiteral("/") : path.left(slash);
                const QString pattern = path.mid(slash + 1);
                const QRegularExpression re(QRegularExpression::wildcardToRegularExpression(pattern));
                const auto entries = directoryEntries(dir);
                bool any = false;
                if (entries) {
                    for (const QString& name : *entries) {
                        if (re.match(name).hasMatch()) {
                            any = true;
                            const QString full = dir == QLatin1String("/") ? QLatin1Char('/') + name
                                                                          : dir + QLatin1Char('/') + name;
                            out << (longFormat ? longListingLine(dir, name).replace(name, full) : full);
                        }
                    }
                }
                if (!any) {
                    out << QStringLiteral("ls: %1: No such file or directory").arg(target);
                }
                continue;
            }
            const auto entries = directoryEntries(path);
            if (!entries) {
                if (isFile(path)) {
                    out << (longFormat ? longListingLine(path.left(path.lastIndexOf(QLatin1Char('/'))),
                                                         path.mid(path.lastIndexOf(QLatin1Char('/')) + 1))
                                       : target);
                } else {
                    out << QStringLiteral("ls: %1: No such file or directory").arg(target);
                }
                continue;
            }
            if (multiple) {
                if (!first) {
                    out << QString();
                }
                out << QStringLiteral("%1:").arg(target);
            }
            first = false;
            if (longFormat) {
                out << QStringLiteral("total %1").arg(entries->size() * 4);
                for (const QString& name : *entries) {
                    out << longListingLine(path, name);
                }
            } else {
                // busybox ls without a tty pipes one per line; on a tty it uses columns.
                QString row;
                for (const QString& name : *entries) {
                    if (row.size() + name.size() + 2 > qMax(20, m_cols)) {
                        out << row.trimmed();
                        row.clear();
                    }
                    row += name.leftJustified(14);
                }
                if (!row.trimmed().isEmpty()) {
                    out << row.trimmed();
                }
            }
        }
        return true;
    }
    if (cmd == QLatin1String("pwd")) {
        out << m_cwd;
        return true;
    }
    if (cmd == QLatin1String("df")) {
        out << SU_LINES(kDfLines);
        return true;
    }
    if (cmd == QLatin1String("free")) {
        out << QStringLiteral("              total        used        free      shared  buff/cache   available");
        out << QStringLiteral("Mem:          62896       41236       21660         172       14336       19280");
        out << QStringLiteral("Swap:             0           0           0");
        return true;
    }
    if (cmd == QLatin1String("ifconfig")) {
        out << SU_LINES(kIfconfig);
        return true;
    }
    if (cmd == QLatin1String("ip")) {
        out << SU_LINES(kIpAddr);
        return true;
    }
    if (cmd == QLatin1String("dmesg")) {
        out << SU_LINES(kKernelBoot) << SU_LINES(kDmesgExtra);
        return true;
    }
    if (cmd == QLatin1String("date")) {
        const QDateTime now = QDateTime::currentDateTime();
        out << QLocale::c().toString(now, QStringLiteral("ddd MMM d HH:mm:ss")) + QStringLiteral(" UTC ") +
                   QString::number(now.date().year());
        return true;
    }
    if (cmd == QLatin1String("uptime")) {
        out << QStringLiteral(" %1 up %2,  load average: 0.12, 0.08, 0.02")
                   .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                        formatUptime(m_uptime.elapsed()));
        return true;
    }
    if (cmd == QLatin1String("echo")) {
        QStringList words;
        for (const QString& arg : args) {
            if (arg == QLatin1String("-n") || arg == QLatin1String("-e")) {
                continue;
            }
            words << expandVariables(arg);
        }
        out << words.join(QLatin1Char(' '));
        return true;
    }
    if (cmd == QLatin1String("ps")) {
        out << SU_LINES(kPsLines);
        return true;
    }
    if (cmd == QLatin1String("env") || (cmd == QLatin1String("export") && args.isEmpty())) {
        for (auto it = m_env.cbegin(); it != m_env.cend(); ++it) {
            out << (cmd == QLatin1String("env") ? QStringLiteral("%1=%2").arg(it.key(), it.value())
                                                : QStringLiteral("export %1='%2'").arg(it.key(), it.value()));
        }
        return true;
    }
    if (cmd == QLatin1String("whoami")) {
        out << (m_user.isEmpty() ? QStringLiteral("root") : m_user);
        return true;
    }
    if (cmd == QLatin1String("hostname")) {
        out << kHostName;
        return true;
    }
    if (cmd == QLatin1String("id")) {
        out << QStringLiteral("uid=0(root) gid=0(root) groups=0(root)");
        return true;
    }
    if (cmd == QLatin1String("chinese")) {
        // Source file is UTF-8 and compiled with /utf-8 (MSVC) or the GCC/Clang default.
        out << QString::fromUtf8("欢迎使用 BuildAI 串口工具")
            << QString::fromUtf8("系统启动完成，登录成功。")
            << QString::fromUtf8("当前设备：Luckfox Pico RV1106（Rockchip）")
            << QString::fromUtf8("温度：40°C，内存：62896 kB，存储：7.4 GiB")
            << QString::fromUtf8("中英文混排测试 mixed CJK / ASCII line 1234567890");
        return true;
    }
    if (cmd == QLatin1String("wide")) {
        // Box drawing + CJK alignment test (each CJK character occupies two cells).
        out << QString::fromUtf8("┌──────────┬──────────┐")
            << QString::fromUtf8("│ 中文测试 │ ASCII    │")
            << QString::fromUtf8("├──────────┼──────────┤")
            << QString::fromUtf8("│ 你好世界 │ hello    │")
            << QString::fromUtf8("│ 串口工具 │ serial   │")
            << QString::fromUtf8("└──────────┴──────────┘")
            << QString::fromUtf8("Wide: 漢字 かな カナ 한글 ｆｕｌｌｗｉｄｔｈ | narrow: abc")
            << QStringLiteral("0123456789012345678901234567890123456789");
        return true;
    }
    if (cmd == QLatin1String("help")) {
        out << QStringLiteral("Built-in commands of the simulated shell:")
            << QStringLiteral("  cat cd chinese clear color date df dmesg echo env export free help hostname id")
            << QStringLiteral("  ifconfig ip ls poweroff progress ps pwd reboot sleep stty top uname uptime whoami")
            << QStringLiteral("  wide exit")
            << QStringLiteral("Pipes: cmd | tail -n N, cmd | head -n N, cmd | grep TEXT")
            << QStringLiteral("Keys: Backspace/DEL erase, Ctrl+U clear line, Ctrl+C interrupt, Ctrl+L clear screen,")
            << QStringLiteral("      Ctrl+D at an empty line logs out.");
        return true;
    }
    return false;
}

void DeviceSimulator::handleShellCommand(const QString& line)
{
    if (line.isEmpty()) {
        prompt();
        return;
    }

    // Very small pipeline support: "cmd args | tail -n 50", "| head -n N", "| grep text".
    QString main = line;
    QString filter;
    const qsizetype pipe = line.indexOf(QLatin1Char('|'));
    if (pipe >= 0) {
        main = line.left(pipe).trimmed();
        filter = line.mid(pipe + 1).trimmed();
    }

    QStringList args = main.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    // Drop shell redirections the quick commands use (2>/dev/null, >/dev/null).
    args.erase(std::remove_if(args.begin(), args.end(),
                              [](const QString& a) {
                                  return a.contains(QLatin1Char('>')) && a.contains(QLatin1String("/dev/null"));
                              }),
               args.end());
    if (args.isEmpty()) {
        prompt();
        return;
    }
    const QString cmd = args.takeFirst();

    // ---- commands with side effects / special output -------------------------------------
    if (cmd == QLatin1String("cd")) {
        const QString target = args.isEmpty() ? QStringLiteral("/root") : resolvePath(args.first());
        if (isDirectory(target)) {
            m_cwd = target;
            m_env.insert(QStringLiteral("PWD"), m_cwd);
        } else if (isFile(target)) {
            emitLine(QStringLiteral("-sh: cd: can't cd to %1: Not a directory").arg(args.first()));
        } else {
            emitLine(QStringLiteral("-sh: cd: can't cd to %1: No such file or directory").arg(args.first()));
        }
        prompt();
        return;
    }
    if (cmd == QLatin1String("clear")) {
        emitText(QStringLiteral("\x1b[H\x1b[2J"));
        prompt();
        return;
    }
    if (cmd == QLatin1String("export") && !args.isEmpty()) {   // bare "export" lists, see below
        for (const QString& assignment : args) {
            const qsizetype eq = assignment.indexOf(QLatin1Char('='));
            if (eq > 0) {
                QString value = assignment.mid(eq + 1);
                if (value.size() >= 2 && ((value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\''))) ||
                                          (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))))) {
                    value = value.mid(1, value.size() - 2);
                }
                m_env.insert(assignment.left(eq), expandVariables(value));
            }
        }
        prompt();
        return;
    }
    if (cmd == QLatin1String("stty")) {
        if (args.size() == 1 && args.first() == QLatin1String("size")) {
            emitLine(QStringLiteral("%1 %2").arg(m_rows).arg(m_cols));
        } else {
            for (qsizetype i = 0; i + 1 < args.size(); ++i) {
                bool ok = false;
                const int value = args.at(i + 1).toInt(&ok);
                if (!ok || value <= 0) {
                    continue;
                }
                if (args.at(i) == QLatin1String("cols") || args.at(i) == QLatin1String("columns")) {
                    m_cols = value;
                } else if (args.at(i) == QLatin1String("rows")) {
                    m_rows = value;
                }
            }
        }
        prompt();
        return;
    }
    if (cmd == QLatin1String("top")) {
        showTop();
        prompt();
        return;
    }
    if (cmd == QLatin1String("color") || cmd == QLatin1String("colour")) {
        showColorTest();
        prompt();
        return;
    }
    if (cmd == QLatin1String("progress")) {
        startProgress();
        return;
    }
    if (cmd == QLatin1String("sleep")) {
        bool ok = false;
        int seconds = args.isEmpty() ? 1 : args.first().toInt(&ok);
        if (!args.isEmpty() && !ok) {
            emitLine(QStringLiteral("sleep: invalid number '%1'").arg(args.first()));
            prompt();
            return;
        }
        seconds = qBound(0, seconds, kMaxSleepSeconds);
        m_stage = Stage::Sleeping;
        runDelayed(seconds * 1000, [this]() {
            if (m_stage == Stage::Sleeping) {
                m_stage = Stage::Shell;
                prompt();
            }
        });
        return;
    }
    if (cmd == QLatin1String("reboot")) {
        emitLine(QString());
        emitLine(QStringLiteral("The system is going down for reboot NOW!"));
        emitLine(QStringLiteral("Stopping BuildAI agent: OK"));
        emitLine(QStringLiteral("Stopping rkipc: OK"));
        emitLine(QStringLiteral("Stopping network: OK"));
        emitLine(QStringLiteral("Unmounting file systems: OK"));
        emitLine(QStringLiteral("[  %1] reboot: Restarting system")
                     .arg(static_cast<double>(m_uptime.elapsed()) / 1000.0 + 12.0, 9, 'f', 6));
        reboot(kRebootDownMs);
        return;
    }
    if (cmd == QLatin1String("poweroff") || cmd == QLatin1String("halt")) {
        emitLine(QString());
        emitLine(QStringLiteral("The system is going down for system halt NOW!"));
        emitLine(QStringLiteral("Unmounting file systems: OK"));
        emitLine(QStringLiteral("[  %1] reboot: Power down")
                     .arg(static_cast<double>(m_uptime.elapsed()) / 1000.0 + 12.0, 9, 'f', 6));
        reboot(-1);
        return;
    }
    if (cmd == QLatin1String("exit") || cmd == QLatin1String("logout")) {
        emitLine(QStringLiteral("logout"));
        showLogin();
        return;
    }
    if (cmd == QLatin1String("true") || cmd == QLatin1String("false") || cmd == QLatin1String(":")) {
        prompt();
        return;
    }

    // ---- plain text commands (support the tiny pipeline) --------------------------------
    QStringList out;
    if (!runShellTextCommand(cmd, args, out)) {
        emitLine(QStringLiteral("-sh: %1: not found").arg(cmd));
        prompt();
        return;
    }
    if (!filter.isEmpty()) {
        const QStringList f = filter.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        const QString tool = f.isEmpty() ? QString() : f.first();
        int n = 10;
        const qsizetype nIndex = f.indexOf(QStringLiteral("-n"));
        if (nIndex >= 0 && nIndex + 1 < f.size()) {
            n = qMax(0, f.at(nIndex + 1).toInt());
        } else if (f.size() >= 2 && f.at(1).startsWith(QLatin1Char('-')) && f.at(1).size() > 1) {
            n = qMax(0, f.at(1).mid(1).toInt());
        }
        if (tool == QLatin1String("tail")) {
            if (out.size() > n) {
                out = out.mid(out.size() - n);
            }
        } else if (tool == QLatin1String("head")) {
            out = out.mid(0, n);
        } else if (tool == QLatin1String("grep") && f.size() >= 2) {
            const QString needle = f.last();
            QStringList kept;
            for (const QString& l : out) {
                if (l.contains(needle, f.contains(QLatin1String("-i")) ? Qt::CaseInsensitive : Qt::CaseSensitive)) {
                    kept << l;
                }
            }
            out = kept;
        } else if (tool == QLatin1String("wc")) {
            out = QStringList{QString::number(out.size())};
        }
    }
    emitLines(out, 0, 0);
    prompt();
}

void DeviceSimulator::showTop()
{
    const double secs = static_cast<double>(m_uptime.elapsed()) / 1000.0;
    emitText(QStringLiteral("\x1b[H\x1b[2J"));
    emitText(QStringLiteral("\x1b[1;1H"));
    emitLine(QStringLiteral("\x1b[1mMem: 41236K used, 21660K free, 172K shrd, 2048K buff, 12288K cached\x1b[0m"));
    emitLine(QStringLiteral("CPU:   3% usr   2% sys   0% nic  94% idle   0% io   0% irq   0% sirq"));
    emitLine(QStringLiteral("Load average: 0.12 0.08 0.02 1/62 242   uptime %1 s").arg(secs, 0, 'f', 1));
    emitText(QStringLiteral("\x1b[4;1H"));
    emitLine(QStringLiteral("\x1b[7m  PID  PPID USER     STAT   VSZ %VSZ %CPU COMMAND")
             + QString(34, QLatin1Char(' ')) + QStringLiteral("\x1b[0m"));
    emitLines(SU_LINES(kTopRows), 0, 0);
}

void DeviceSimulator::showColorTest()
{
    QString line;
    line = QStringLiteral("Standard:  ");
    for (int i = 0; i < 8; ++i) {
        line += QStringLiteral("\x1b[%1m %2 ").arg(40 + i).arg(i, 2);
    }
    line += QStringLiteral("\x1b[0m");
    emitLine(line);
    line = QStringLiteral("Bright:    ");
    for (int i = 0; i < 8; ++i) {
        line += QStringLiteral("\x1b[%1m %2 ").arg(100 + i).arg(8 + i, 2);
    }
    line += QStringLiteral("\x1b[0m");
    emitLine(line);
    line = QStringLiteral("Foreground: ");
    static const char* const names[] = {"black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
    for (int i = 0; i < 8; ++i) {
        line += QStringLiteral("\x1b[%1m%2\x1b[0m ").arg(30 + i).arg(QLatin1String(names[i]));
    }
    emitLine(line);
    line = QStringLiteral("Bold bright: ");
    for (int i = 0; i < 8; ++i) {
        line += QStringLiteral("\x1b[1;%1m%2\x1b[0m ").arg(30 + i).arg(QLatin1String(names[i]));
    }
    emitLine(line);
    emitLine(QStringLiteral("Attributes: \x1b[1mbold\x1b[0m \x1b[2mdim\x1b[0m \x1b[3mitalic\x1b[0m "
                            "\x1b[4munderline\x1b[0m \x1b[7minverse\x1b[0m \x1b[9mstrike\x1b[0m "
                            "\x1b[1;4;31mcombined\x1b[0m"));
    emitLine(QStringLiteral("256-colour cube:"));
    for (int row = 0; row < 6; ++row) {
        line.clear();
        for (int col = 0; col < 36; ++col) {
            line += QStringLiteral("\x1b[48;5;%1m  ").arg(16 + row * 36 + col);
        }
        line += QStringLiteral("\x1b[0m");
        emitLine(line);
    }
    line = QStringLiteral("Greys: ");
    for (int i = 232; i < 256; ++i) {
        line += QStringLiteral("\x1b[48;5;%1m  ").arg(i);
    }
    line += QStringLiteral("\x1b[0m");
    emitLine(line);
    line = QStringLiteral("Truecolor: ");
    for (int i = 0; i < 64; ++i) {
        const int r = i * 4;
        const int g = 255 - i * 4;
        const int b = (i * 8) % 256;
        line += QStringLiteral("\x1b[48;2;%1;%2;%3m ").arg(r).arg(g).arg(b);
    }
    line += QStringLiteral("\x1b[0m");
    emitLine(line);
    emitLine(QStringLiteral("Colon form: \x1b[38:2::255:128:0mtruecolor fg\x1b[0m \x1b[38:5:82m256 fg\x1b[0m"));
}

// =========================================================================================
// U-Boot
// =========================================================================================

void DeviceSimulator::showUBootBanner()
{
    emitLines(SU_LINES(kUBootBanner), 5, 60);
}

void DeviceSimulator::beginCountdown()
{
    m_stage = Stage::Countdown;
    m_countdown = kCountdownStart;
    emitText(QStringLiteral("Hit any key to stop autoboot:  %1").arg(m_countdown));
    m_countdownTimer.start();
}

void DeviceSimulator::onCountdownTimer()
{
    if (m_stage != Stage::Countdown) {
        m_countdownTimer.stop();
        return;
    }
    --m_countdown;
    if (m_countdown > 0) {
        emitText(QStringLiteral("\b%1").arg(m_countdown));
        return;
    }
    m_countdownTimer.stop();
    emitText(QStringLiteral("\b0\r\n"));
    bootFromUBoot();
}

void DeviceSimulator::bootFromUBoot()
{
    emitLines(SU_LINES(kUBootBootLines), 4, 80);
    pause(400);
    bootLinux();
}

void DeviceSimulator::handleUBootCommand(const QString& line)
{
    if (line.isEmpty()) {
        prompt();
        return;
    }
    const QStringList args = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const QString cmd = args.first();

    if (cmd == QLatin1String("help") || cmd == QLatin1String("?")) {
        emitLines(SU_LINES(kUBootHelp), 0, 0);
    } else if (cmd == QLatin1String("version")) {
        emitLine(kUBootVersion);
        emitLine(QStringLiteral("arm-linux-gnueabihf-gcc (Linaro GCC 6.3-2017.05) 6.3.1 20170404"));
        emitLine(QStringLiteral("GNU ld (Linaro_Binutils-2017.05) 2.27.0.20161019"));
    } else if (cmd == QLatin1String("printenv") || cmd == QLatin1String("print") || cmd == QLatin1String("env")) {
        if (args.size() > 1 && cmd != QLatin1String("env")) {
            for (qsizetype i = 1; i < args.size(); ++i) {
                const auto it = m_ubootEnv.constFind(args.at(i));
                if (it == m_ubootEnv.constEnd()) {
                    emitLine(QStringLiteral("## Error: \"%1\" not defined").arg(args.at(i)));
                } else {
                    emitLine(QStringLiteral("%1=%2").arg(it.key(), it.value()));
                }
            }
        } else {
            int size = 0;
            for (auto it = m_ubootEnv.cbegin(); it != m_ubootEnv.cend(); ++it) {
                emitLine(QStringLiteral("%1=%2").arg(it.key(), it.value()));
                size += static_cast<int>(it.key().size() + it.value().size() + 2);
            }
            emitLine(QString());
            emitLine(QStringLiteral("Environment size: %1/32764 bytes").arg(size));
        }
    } else if (cmd == QLatin1String("setenv")) {
        if (args.size() < 2) {
            emitLine(QStringLiteral("setenv - set environment variables"));
            emitLine(QString());
            emitLine(QStringLiteral("Usage:"));
            emitLine(QStringLiteral("setenv [-f] name value ..."));
        } else if (args.size() == 2) {
            m_ubootEnv.remove(args.at(1));
        } else {
            m_ubootEnv.insert(args.at(1), args.mid(2).join(QLatin1Char(' ')));
        }
    } else if (cmd == QLatin1String("saveenv")) {
        emitLine(QStringLiteral("Saving Environment to MMC... Writing to MMC(0)... OK"));
    } else if (cmd == QLatin1String("bdinfo")) {
        emitLines(SU_LINES(kBdInfo), 0, 0);
    } else if (cmd == QLatin1String("mmc")) {
        const QString sub = args.size() > 1 ? args.at(1) : QString();
        if (sub == QLatin1String("info")) {
            emitLines(SU_LINES(kMmcInfo), 0, 0);
        } else if (sub == QLatin1String("list")) {
            emitLine(QStringLiteral("dwmmc@ffaa0000: 0 (SD)"));
        } else if (sub == QLatin1String("dev")) {
            emitLine(QStringLiteral("switch to partitions #0, OK"));
            emitLine(QStringLiteral("mmc0 is current device"));
        } else if (sub == QLatin1String("part")) {
            emitLine(QString());
            emitLine(QStringLiteral("Partition Map for MMC device 0  --   Partition Type: EFI"));
            emitLine(QString());
            emitLine(QStringLiteral("Part\tStart LBA\tEnd LBA\t\tName"));
            emitLine(QStringLiteral("  1\t0x00000040\t0x000003ff\t\"env\""));
            emitLine(QStringLiteral("  2\t0x00000400\t0x000023ff\t\"idblock\""));
            emitLine(QStringLiteral("  3\t0x00002400\t0x000043ff\t\"uboot\""));
            emitLine(QStringLiteral("  4\t0x00004400\t0x000143ff\t\"boot\""));
            emitLine(QStringLiteral("  5\t0x00014400\t0x000643ff\t\"rootfs\""));
            emitLine(QStringLiteral("  6\t0x00064400\t0x000a43ff\t\"oem\""));
            emitLine(QStringLiteral("  7\t0x000a4400\t0x00ee7fde\t\"userdata\""));
        } else {
            emitLine(QStringLiteral("mmc - MMC sub system"));
            emitLine(QString());
            emitLine(QStringLiteral("Usage:"));
            emitLine(QStringLiteral("mmc info - display info of the current MMC device"));
            emitLine(QStringLiteral("mmc list - lists available devices"));
            emitLine(QStringLiteral("mmc part - lists available partition on current mmc device"));
            emitLine(QStringLiteral("mmc dev [dev] [part] - show or set current mmc device [partition]"));
        }
    } else if (cmd == QLatin1String("md") || cmd == QLatin1String("md.l")) {
        bool ok = false;
        quint32 addr = args.size() > 1 ? args.at(1).toUInt(&ok, 16) : 0;
        if (!ok) {
            emitLine(QStringLiteral("md - memory display"));
            emitLine(QString());
            emitLine(QStringLiteral("Usage:"));
            emitLine(QStringLiteral("md [.b, .w, .l] address [# of objects]"));
        } else {
            addr &= ~3u;
            for (int row = 0; row < 4; ++row) {
                QString words;
                QString ascii;
                for (int col = 0; col < 4; ++col) {
                    const quint32 a = addr + static_cast<quint32>(row * 16 + col * 4);
                    const quint32 value = (a * 2654435761u) ^ 0xE1A00000u;
                    words += hexWord(value) + QLatin1Char(' ');
                    for (int b = 0; b < 4; ++b) {
                        const int ch = static_cast<int>((value >> (b * 8)) & 0xFF);
                        const bool printable = ch >= 0x20 && ch < 0x7F;
                        ascii += printable ? QChar::fromLatin1(static_cast<char>(ch)) : QLatin1Char('.');
                    }
                }
                const QString rowAddress = hexWord(addr + static_cast<quint32>(row * 16));
                emitLine(QStringLiteral("%1: %2   %3").arg(rowAddress, words.trimmed(), ascii));
            }
        }
    } else if (cmd == QLatin1String("reset")) {
        emitLine(QStringLiteral("resetting ..."));
        pause(300);
        m_lineBuffer.clear();
        showUBootBanner();
        beginCountdown();
        return;
    } else if (cmd == QLatin1String("boot") || cmd == QLatin1String("bootm") || cmd == QLatin1String("bootd") ||
               (cmd == QLatin1String("run") && args.size() > 1 && args.at(1) == QLatin1String("bootcmd"))) {
        bootFromUBoot();
        return;
    } else if (cmd == QLatin1String("run")) {
        if (args.size() < 2) {
            emitLine(QStringLiteral("run - run commands in an environment variable"));
        } else if (!m_ubootEnv.contains(args.at(1))) {
            emitLine(QStringLiteral("## Error: \"%1\" not defined").arg(args.at(1)));
        }
    } else if (cmd == QLatin1String("echo")) {
        emitLine(args.mid(1).join(QLatin1Char(' ')));
    } else {
        emitLine(QStringLiteral("Unknown command '%1' - try 'help'").arg(cmd));
    }
    prompt();
}

// =========================================================================================
// MCU shell
// =========================================================================================

void DeviceSimulator::showMcuBanner()
{
    emitLine(QString());
    emitLine(kMcuVersion);
    emitLine(QStringLiteral("Build Sep 17 2026 10:21:43, HAL 1.8.1, FreeRTOS 10.5"));
    emitLine(QStringLiteral("Type 'help' for a list of commands."));
    m_echo = true;
    m_ledOn = false;
    m_telemetry = false;
    m_telemetryTimer.stop();
}

void DeviceSimulator::handleMcuCommand(const QString& line)
{
    if (line.isEmpty()) {
        prompt();
        return;
    }
    const QStringList args = line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    const QString cmd = args.first();
    const QString upper = line.toUpper();

    // ---- AT command set -------------------------------------------------------------------
    if (upper.startsWith(QLatin1String("AT"))) {
        if (upper == QLatin1String("AT")) {
            emitLine(QStringLiteral("OK"));
        } else if (upper == QLatin1String("AT+GMR")) {
            emitLine(kMcuVersion);
            emitLine(QStringLiteral("SDK version: v1.0.0-buildai"));
            emitLine(QStringLiteral("compile time: Sep 17 2026 10:21:43"));
            emitLine(QStringLiteral("OK"));
        } else if (upper == QLatin1String("ATE0")) {
            m_echo = false;
            emitLine(QStringLiteral("OK"));
        } else if (upper == QLatin1String("ATE1")) {
            m_echo = true;
            emitLine(QStringLiteral("OK"));
        } else if (upper == QLatin1String("AT+RST")) {
            emitLine(QStringLiteral("OK"));
            reboot(kMcuResetDownMs);
            return;
        } else {
            emitLine(QStringLiteral("ERROR"));
        }
        prompt();
        return;
    }

    // ---- plain shell commands ---------------------------------------------------------------
    if (cmd == QLatin1String("help") || cmd == QLatin1String("?")) {
        emitLines(SU_LINES(kMcuHelp), 0, 0);
    } else if (cmd == QLatin1String("version")) {
        emitLine(kMcuVersion);
    } else if (cmd == QLatin1String("reset")) {
        emitLine(QStringLiteral("Resetting..."));
        pause(200);
        m_uptime.restart();
        showMcuBanner();
    } else if (cmd == QLatin1String("led")) {
        const QString what = args.size() > 1 ? args.at(1).toLower() : QString();
        if (what == QLatin1String("on")) {
            m_ledOn = true;
        } else if (what == QLatin1String("off")) {
            m_ledOn = false;
        } else if (what == QLatin1String("toggle")) {
            m_ledOn = !m_ledOn;
        } else {
            emitLine(QStringLiteral("usage: led on|off|toggle"));
            prompt();
            return;
        }
        emitLine(QStringLiteral("LED is %1").arg(m_ledOn ? QStringLiteral("ON") : QStringLiteral("OFF")));
    } else if (cmd == QLatin1String("adc")) {
        const double t = static_cast<double>(m_uptime.elapsed()) / 1000.0;
        for (int ch = 0; ch < 8; ++ch) {
            const int raw = qBound(0, 2048 + static_cast<int>(1500.0 * std::sin(t * 0.7 + ch * 0.8)) +
                                          static_cast<int>(QRandomGenerator::global()->bounded(-8, 9)), 4095);
            emitLine(QStringLiteral("ADC%1: %2 (%3 V)").arg(ch).arg(raw, 4).arg(raw * 3.3 / 4095.0, 0, 'f', 3));
        }
    } else if (cmd == QLatin1String("temp")) {
        emitLine(QStringLiteral("Temperature: %1 C").arg(temperature(), 0, 'f', 1));
    } else if (cmd == QLatin1String("uptime")) {
        emitLine(QStringLiteral("Uptime: %1 s").arg(static_cast<double>(m_uptime.elapsed()) / 1000.0, 0, 'f', 3));
    } else if (cmd == QLatin1String("telemetry")) {
        const QString what = args.size() > 1 ? args.at(1).toLower() : QString();
        if (what == QLatin1String("on")) {
            m_telemetry = true;
            m_telemetryTimer.start();
            emitLine(QStringLiteral("Telemetry enabled (every 2 s)"));
        } else if (what == QLatin1String("off")) {
            m_telemetry = false;
            m_telemetryTimer.stop();
            emitLine(QStringLiteral("Telemetry disabled"));
        } else {
            emitLine(QStringLiteral("Telemetry is %1").arg(m_telemetry ? QStringLiteral("on") : QStringLiteral("off")));
        }
    } else if (cmd == QLatin1String("echo")) {
        emitLine(args.mid(1).join(QLatin1Char(' ')));
    } else {
        emitLine(QStringLiteral("Unknown command: %1 (try 'help')").arg(cmd));
    }
    prompt();
}

double DeviceSimulator::temperature() const
{
    const double t = static_cast<double>(m_uptime.elapsed()) / 1000.0;
    return 36.2 + 0.8 * std::sin(t / 7.0) + (m_ledOn ? 0.4 : 0.0);
}

void DeviceSimulator::onTelemetryTimer()
{
    if (!m_telemetry || m_stage != Stage::Shell) {
        return;
    }
    const double t = static_cast<double>(m_uptime.elapsed()) / 1000.0;
    const double vbat = 3.98 - t * 0.0002;
    const int rssi = -67 + QRandomGenerator::global()->bounded(-3, 4);
    emitLine(QStringLiteral("[%1] temp=%2C vbat=%3V rssi=%4dBm")
                 .arg(t, 9, 'f', 3)
                 .arg(temperature(), 0, 'f', 1)
                 .arg(vbat, 0, 'f', 2)
                 .arg(rssi));
}
