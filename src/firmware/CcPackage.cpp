#include "firmware/CcPackage.h"

#include "app/Logging.h"
#include "firmware/CcProtocol.h"

#include <QFile>
#include <QtEndian>
#include <array>

// The CC1310 package format (see CcPackage.h), ported from CC1310-Firmware/tools/fw_package.py.
// parse() keeps the Python order of checks so the first reason a damaged file reports is the same
// one the Python tool prints.

namespace {

constexpr int kHeaderBytes = CcPackage::kHeaderSize;   // 32
constexpr int kHeaderCrcBytes = kHeaderBytes - 4;      // the header CRC32 covers all but itself
constexpr quint32 kReservedVersionMask = 0xFF000000u;  // the most significant version byte

/// CRC32 (zlib / IEEE 802.3) lookup table, reflected polynomial 0xEDB88320, built at compile time.
constexpr std::array<quint32, 256> makeCrc32Table()
{
    std::array<quint32, 256> table{};
    for (quint32 i = 0; i < 256; ++i) {
        quint32 crc = i;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        table[i] = crc;
    }
    return table;
}
constexpr std::array<quint32, 256> kCrc32Table = makeCrc32Table();

bool setError(QString* error, const QString& text)
{
    if (error)
        *error = text;
    return false;
}

} // namespace

QString CcPackage::versionText() const { return CcProtocol::formatVersion(version); }
QString CcPackage::targetText() const { return CcProtocol::formatTarget(target); }

QString CcPackage::summary() const
{
    const auto hex8 = [](quint32 value) { return QString::number(value, 16).rightJustified(8, QLatin1Char('0')).toUpper(); };
    return QStringLiteral("INFO package target=0x%1 version=%2 code=0x%3 size=%4")
        .arg(hex8(target), versionText(), hex8(version)).arg(size);
}

quint32 CcPackage::crc32(const QByteArray& data, quint32 seed)
{
    quint32 crc = ~seed;
    for (char c : data)
        crc = kCrc32Table[(crc ^ static_cast<quint8>(c)) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool CcPackage::parse(const QByteArray& file, CcPackage* package, QString* error)
{
    if (file.size() < kHeaderBytes)
        return setError(error, QStringLiteral("package is shorter than its header"));

    const uchar* bytes = reinterpret_cast<const uchar*>(file.constData());
    const quint32 magic = qFromLittleEndian<quint32>(bytes);
    const quint16 format = qFromLittleEndian<quint16>(bytes + 4);
    const quint16 headerSize = qFromLittleEndian<quint16>(bytes + 6);
    CcPackage parsed;
    parsed.target = qFromLittleEndian<quint32>(bytes + 8);
    parsed.address = qFromLittleEndian<quint32>(bytes + 12);
    parsed.size = qFromLittleEndian<quint32>(bytes + 16);
    parsed.version = qFromLittleEndian<quint32>(bytes + 20);
    parsed.imageCrc = qFromLittleEndian<quint32>(bytes + 24);
    parsed.headerCrc = qFromLittleEndian<quint32>(bytes + 28);

    if (magic != kMagic || format != kFormatVersion || headerSize != kHeaderSize)
        return setError(error, QStringLiteral("unsupported package header"));
    if (crc32(file.left(kHeaderCrcBytes)) != parsed.headerCrc)
        return setError(error, QStringLiteral("header CRC32 mismatch"));
    if (parsed.address != kAppAddress || parsed.size > kMaxImageSize || parsed.size == 0)
        return setError(error, QStringLiteral("invalid application address or size"));
    if (parsed.version & kReservedVersionMask)
        return setError(error, QStringLiteral("version must reserve its most-significant byte"));

    parsed.header = file.left(kHeaderBytes);
    parsed.image = file.mid(kHeaderBytes);
    if (static_cast<quint32>(parsed.image.size()) != parsed.size)
        return setError(error, QStringLiteral("package length does not match header image size"));
    if (crc32(parsed.image) != parsed.imageCrc)
        return setError(error, QStringLiteral("image CRC32 mismatch"));

    if (package)
        *package = parsed;
    return true;
}

bool CcPackage::load(const QString& path, CcPackage* package, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return setError(error, QStringLiteral("cannot read %1: %2").arg(path, file.errorString()));
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError)
        return setError(error, QStringLiteral("cannot read %1: %2").arg(path, file.errorString()));

    CcPackage parsed;
    if (!parse(bytes, &parsed, error))
        return false;
    parsed.filePath = path;
    if (package)
        *package = parsed;
    qCInfo(lcFirmware) << "package loaded" << path << parsed.size << "bytes, version" << parsed.versionText();
    return true;
}

QByteArray CcPackage::build(const QByteArray& image, quint32 target, quint32 version, QString* error)
{
    if (version & kReservedVersionMask) {
        setError(error, QStringLiteral("version must reserve its most-significant byte"));
        return QByteArray();
    }
    // The CC1310 flash accepts word-sized writes: pad with erased-flash bytes, which the image CRC
    // and the package length include (fw_package.build()).
    QByteArray padded = image;
    padded.append(QByteArray((4 - padded.size() % 4) % 4, '\xFF'));
    if (padded.isEmpty() || static_cast<quint32>(padded.size()) > kMaxImageSize) {
        setError(error, QStringLiteral("image size is outside the App slot"));
        return QByteArray();
    }

    QByteArray header(kHeaderBytes, '\0');
    uchar* bytes = reinterpret_cast<uchar*>(header.data());
    qToLittleEndian<quint32>(kMagic, bytes);
    qToLittleEndian<quint16>(kFormatVersion, bytes + 4);
    qToLittleEndian<quint16>(kHeaderSize, bytes + 6);
    qToLittleEndian<quint32>(target, bytes + 8);
    qToLittleEndian<quint32>(kAppAddress, bytes + 12);
    qToLittleEndian<quint32>(static_cast<quint32>(padded.size()), bytes + 16);
    qToLittleEndian<quint32>(version, bytes + 20);
    qToLittleEndian<quint32>(crc32(padded), bytes + 24);
    qToLittleEndian<quint32>(crc32(header.left(kHeaderCrcBytes)), bytes + 28);
    return header + padded;
}
