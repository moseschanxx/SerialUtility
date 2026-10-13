#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <optional>

/**
 * The CC1310 UART bootloader protocol, exactly as CC1310-Firmware/tools/fw_protocol.py defines it
 * (the Python tool is the golden reference; tests/data/firmware/vectors.json holds vectors it
 * generated).
 *
 * Wire format of one frame: COBS(header + payload + crc) followed by one 0x00 terminator.
 *   header  = version u8 (kProtocolVersion = 2), type u8 (MessageType), sequence u16 LE,
 *             length u16 LE (payload size)                                    -> kHeaderSize = 6
 *   crc     = CRC16-CCITT (init 0xFFFF, polynomial 0x1021, no reflection, no final xor) over
 *             header + payload, u16 LE
 * decodeFrame() rejects: invalid COBS, a frame shorter than header + crc ("short frame"), a
 * version other than 2 or a length that does not match ("invalid frame header"), a wrong CRC
 * ("frame CRC16 mismatch") - the same reasons and words as the Python decoder.
 *
 * COBS (Consistent Overhead Byte Stuffing) as in fw_protocol.py: cobsEncode() never emits the
 * terminating zero; cobsDecode() takes the bytes without it and refuses a zero code byte or a
 * block that runs past the end ("invalid COBS frame"). A full 254-byte block (code 0xFF) adds no
 * implicit zero. Encoding an empty array gives "\x01".
 *
 * Messages (fw_update.py): HELLO -> INFO (20 or 24 bytes: target u32, boot state u32, max image
 * size u32, image size u32, version u32 packed 0x00MMmmpp, optional role u32), BEGIN (the
 * package's 32-byte header + role u32) -> READY (resume offset u32, always 0), DATA (offset u32 +
 * up to kDataChunkSize bytes) -> ACK / NACK (the offset the device expects next, u32), END ->
 * COMPLETE, SET_ROLE (role u32) -> COMPLETE. Every payload integer is little-endian.
 */
namespace CcProtocol {

constexpr quint8 kProtocolVersion = 2;
constexpr int kHeaderSize = 6;
constexpr int kCrcSize = 2;
constexpr int kDataChunkSize = 128;   ///< fw_update.py CHUNK_SIZE

enum MessageType : quint8 {
    Hello = 1, Info = 2, Begin = 3, Ready = 4, Data = 5, Ack = 6, Nack = 7, End = 8,
    Complete = 9, Error = 10, SetRole = 11
};
QString messageTypeName(quint8 type);   ///< "HELLO", "INFO", ... ; "type <n>" for an unknown one

/// CRC16-CCITT (0xFFFF init, 0x1021): crc16Ccitt("123456789") == 0x29B1.
quint16 crc16Ccitt(const QByteArray& data);

QByteArray cobsEncode(const QByteArray& data);                 ///< without the terminating zero
bool cobsDecode(const QByteArray& encoded, QByteArray* out);   ///< `encoded` without the terminating zero

struct Frame
{
    quint8 type = 0;
    quint16 sequence = 0;
    QByteArray payload;
    bool operator==(const Frame& other) const;
    bool operator!=(const Frame& other) const { return !(*this == other); }
};

/// The complete wire bytes of a frame, terminating zero included (fw_protocol.encode()).
QByteArray encodeFrame(quint8 type, quint16 sequence, const QByteArray& payload = QByteArray());
/// Decode one frame; `wire` may carry its terminating zero(s) (they are stripped like
/// bytes.rstrip(b"\0")). False with *error on any of the rejections listed above.
bool decodeFrame(const QByteArray& wire, Frame* frame, QString* error = nullptr);

/**
 * Splits a byte stream into frames at every 0x00 (what Updater._read_frame() does one byte at a
 * time): feed() returns the frames completed by these bytes, in order; a chunk between two zeros
 * that does not decode is dropped (counted in invalidFrames()) and reading continues with the
 * next one, so garbage on the line before a reset never blocks the stream. Bytes after the last
 * zero stay buffered until the next feed(); clear() drops them (used after a reset settle).
 */
class FrameSplitter
{
public:
    QList<Frame> feed(const QByteArray& bytes);
    void clear();
    int invalidFrames() const { return m_invalid; }
    QByteArray pending() const { return m_buffer; }

private:
    QByteArray m_buffer;
    int m_invalid = 0;
};

/// The INFO payload (fw_update.py Updater.info()).
struct DeviceInfo
{
    quint32 target = 0;
    quint32 state = 0;       ///< 1 valid_application, 2 update_requested, 3 update_in_progress
    quint32 maxSize = 0;
    quint32 imageSize = 0;
    quint32 version = 0;     ///< packed 0x00MMmmpp
    std::optional<quint32> role;   ///< present when the payload is 24 bytes: 0 unset, 1 rx, 2 tx

    /// fw_update.py format_info(): "INFO target=CC1M(0x4343314D) state=valid_application(1)
    /// max_size=94208 image_size=78040 version=0.8.1(0x00000801) role=rx(1)".
    QString toString() const;
    bool operator==(const DeviceInfo& other) const;
};
/// False when the payload is neither 20 nor 24 bytes ("invalid INFO response").
bool parseInfo(const QByteArray& payload, DeviceInfo* info);

enum Role : quint32 { RoleUnset = 0, RoleRx = 1, RoleTx = 2 };

QString formatVersion(quint32 packed);      ///< "0.8.1" for 0x00000801 (fw_package.format_version)
QString bootStateName(quint32 state);       ///< "valid_application" / "update_requested" / "update_in_progress" / "unknown"
QString roleName(quint32 role);             ///< "unset" / "rx" / "tx" / "unknown"
std::optional<quint32> parseRole(const QString& text);   ///< "rx" -> 1, "tx" -> 2 (case-insensitive), else nullopt
QString targetName(quint32 target);         ///< "CC1M" for 0x4343314D, else "unknown"
/// "name(value)" / "name(0x%08X)" helpers used by the messages above.
QString formatEnum(quint32 value, const QString& name);
QString formatTarget(quint32 target);

} // namespace CcProtocol
