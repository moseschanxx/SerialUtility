#pragma once

#include <QByteArray>
#include <QString>

/**
 * A CC1310 firmware package (.pkg) as CC1310-Firmware/tools/fw_package.py builds and verifies it.
 *
 * Layout: a 32-byte little-endian header <magic u32, format u16, header size u16, target u32,
 * address u32, size u32, version u32, image crc32 u32, header crc32 u32> followed by the image
 * (`size` bytes, padded to a multiple of 4 with 0xFF by the builder). magic = 0x4B505746 ("FWPK"
 * in memory order), format = 1, header size = 32, address = 0x8000, 0 < size <= 0x17000, version
 * packed 0x00MMmmpp (the most significant byte reserved). header crc32 = CRC32 (zlib / IEEE
 * 802.3, reflected, init and final xor 0xFFFFFFFF) of the first 28 bytes; image crc32 = CRC32 of
 * the image.
 *
 * parse() applies the Python checks in the same order with the same reasons: "package is shorter
 * than its header", "unsupported package header" (magic / format / header size), "header CRC32
 * mismatch", "invalid application address or size", "version must reserve its most-significant
 * byte", "package length does not match header image size", "image CRC32 mismatch".
 * The first 32 bytes (header()) are what BEGIN carries to the bootloader.
 */
struct CcPackage
{
    static constexpr quint32 kMagic = 0x4B505746;
    static constexpr quint16 kFormatVersion = 1;
    static constexpr quint16 kHeaderSize = 32;
    static constexpr quint32 kAppAddress = 0x8000;
    static constexpr quint32 kMaxImageSize = 0x17000;

    quint32 target = 0;
    quint32 address = 0;
    quint32 size = 0;          ///< image bytes
    quint32 version = 0;       ///< packed 0x00MMmmpp
    quint32 imageCrc = 0;
    quint32 headerCrc = 0;
    QByteArray header;         ///< the 32 header bytes as read
    QByteArray image;          ///< `size` bytes
    QString filePath;          ///< set by load()

    QString versionText() const;   ///< CcProtocol::formatVersion(version)
    QString targetText() const;    ///< CcProtocol::formatTarget(target)
    bool isValid() const { return !header.isEmpty(); }
    /// fw_update.py: "INFO package target=0x4343314D version=0.8.1 code=0x00000801 size=78040"
    QString summary() const;

    /// Parse the bytes of a whole .pkg file; false with *error (reasons above).
    static bool parse(const QByteArray& file, CcPackage* package, QString* error = nullptr);
    /// QFile read + parse(); sets filePath. Also fails on an unreadable file.
    static bool load(const QString& path, CcPackage* package, QString* error = nullptr);
    /// fw_package.build(): pads the image to 4 bytes with 0xFF; fails on an empty / oversized
    /// image ("image size is outside the App slot") or a version with its top byte set.
    static QByteArray build(const QByteArray& image, quint32 target, quint32 version, QString* error = nullptr);

    /// CRC32 as zlib.crc32() computes it (table-driven; no zlib dependency).
    static quint32 crc32(const QByteArray& data, quint32 seed = 0);
};
