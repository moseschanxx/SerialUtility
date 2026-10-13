#include "firmware/CcProtocol.h"

#include <QStringList>
#include <QtEndian>
#include <array>

// The CC1310 UART bootloader protocol (see CcProtocol.h). Every function is a straight port of
// CC1310-Firmware/tools/fw_protocol.py and the format helpers of fw_update.py / fw_package.py; the
// Python tools stay the reference, tests/data/firmware/vectors.json holds what they produced.

namespace CcProtocol {

namespace {

constexpr int kMinRawFrame = kHeaderSize + kCrcSize;   // header + empty payload + crc

/// A frame that grows past this many bytes without a terminating zero is line noise, not a frame
/// (the largest legal one is a 65535 byte payload plus header, crc and COBS overhead).
constexpr int kMaxPendingBytes = 70000;

/// CRC16-CCITT lookup table, built at compile time (polynomial 0x1021, no reflection).
constexpr std::array<quint16, 256> makeCrc16Table()
{
    std::array<quint16, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        quint16 crc = static_cast<quint16>(i << 8);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc & 0x8000) ? static_cast<quint16>((crc << 1) ^ 0x1021) : static_cast<quint16>(crc << 1);
        table[i] = crc;
    }
    return table;
}
constexpr std::array<quint16, 256> kCrc16Table = makeCrc16Table();

} // namespace

QString messageTypeName(quint8 type)
{
    switch (type) {
    case Hello:    return QStringLiteral("HELLO");
    case Info:     return QStringLiteral("INFO");
    case Begin:    return QStringLiteral("BEGIN");
    case Ready:    return QStringLiteral("READY");
    case Data:     return QStringLiteral("DATA");
    case Ack:      return QStringLiteral("ACK");
    case Nack:     return QStringLiteral("NACK");
    case End:      return QStringLiteral("END");
    case Complete: return QStringLiteral("COMPLETE");
    case Error:    return QStringLiteral("ERROR");
    case SetRole:  return QStringLiteral("SET_ROLE");
    default:       return QStringLiteral("type %1").arg(static_cast<uint>(type));
    }
}

quint16 crc16Ccitt(const QByteArray& data)
{
    quint16 crc = 0xFFFF;
    for (char c : data)
        crc = static_cast<quint16>((crc << 8) ^ kCrc16Table[((crc >> 8) ^ static_cast<quint8>(c)) & 0xFF]);
    return crc;
}

QByteArray cobsEncode(const QByteArray& data)
{
    QByteArray out;
    out.reserve(data.size() + data.size() / 254 + 2);
    out.append('\0');                    // placeholder of the first code byte
    int codeIndex = 0;
    unsigned code = 1;
    for (char c : data) {
        if (c == 0) {
            out[codeIndex] = static_cast<char>(code);
            codeIndex = out.size();
            out.append('\0');
            code = 1;
        } else {
            out.append(c);
            ++code;
            if (code == 0xFF) {          // a full block: no implicit zero follows it
                out[codeIndex] = static_cast<char>(code);
                codeIndex = out.size();
                out.append('\0');
                code = 1;
            }
        }
    }
    out[codeIndex] = static_cast<char>(code);
    return out;
}

bool cobsDecode(const QByteArray& encoded, QByteArray* out)
{
    QByteArray result;
    result.reserve(encoded.size());
    const int size = encoded.size();
    int index = 0;
    while (index < size) {
        const int code = static_cast<quint8>(encoded.at(index));
        // A code byte describes itself plus code - 1 following data bytes; a truncated block is an
        // error, not a shorter slice.
        if (code == 0 || index + code > size)
            return false;
        ++index;
        result.append(encoded.constData() + index, code - 1);
        index += code - 1;
        if (code != 0xFF && index < size)
            result.append('\0');
    }
    if (out)
        *out = result;
    return true;
}

bool Frame::operator==(const Frame& other) const
{
    return type == other.type && sequence == other.sequence && payload == other.payload;
}

QByteArray encodeFrame(quint8 type, quint16 sequence, const QByteArray& payload)
{
    QByteArray raw(kHeaderSize, '\0');
    raw[0] = static_cast<char>(kProtocolVersion);
    raw[1] = static_cast<char>(type);
    qToLittleEndian<quint16>(sequence, raw.data() + 2);
    qToLittleEndian<quint16>(static_cast<quint16>(payload.size()), raw.data() + 4);
    raw.append(payload);
    const quint16 crc = crc16Ccitt(raw);
    raw.resize(raw.size() + kCrcSize);
    qToLittleEndian<quint16>(crc, raw.data() + raw.size() - kCrcSize);
    QByteArray wire = cobsEncode(raw);
    wire.append('\0');
    return wire;
}

bool decodeFrame(const QByteArray& wire, Frame* frame, QString* error)
{
    auto fail = [error](const char* reason) {
        if (error)
            *error = QString::fromLatin1(reason);
        return false;
    };

    int end = wire.size();               // bytes.rstrip(b"\0")
    while (end > 0 && wire.at(end - 1) == '\0')
        --end;

    QByteArray raw;
    if (!cobsDecode(wire.left(end), &raw))
        return fail("invalid COBS frame");
    if (raw.size() < kMinRawFrame)
        return fail("short frame");

    const uchar* bytes = reinterpret_cast<const uchar*>(raw.constData());
    const quint8 version = bytes[0];
    const quint8 type = bytes[1];
    const quint16 sequence = qFromLittleEndian<quint16>(bytes + 2);
    const quint16 length = qFromLittleEndian<quint16>(bytes + 4);
    if (version != kProtocolVersion || raw.size() != kHeaderSize + length + kCrcSize)
        return fail("invalid frame header");

    const int crcOffset = raw.size() - kCrcSize;
    if (crc16Ccitt(raw.left(crcOffset)) != qFromLittleEndian<quint16>(bytes + crcOffset))
        return fail("frame CRC16 mismatch");

    if (frame) {
        frame->type = type;
        frame->sequence = sequence;
        frame->payload = raw.mid(kHeaderSize, length);
    }
    return true;
}

QList<Frame> FrameSplitter::feed(const QByteArray& bytes)
{
    QList<Frame> frames;
    m_buffer.append(bytes);
    int start = 0;
    for (;;) {
        const int zero = m_buffer.indexOf('\0', start);
        if (zero < 0)
            break;
        const QByteArray chunk = m_buffer.mid(start, zero - start + 1);
        start = zero + 1;
        if (chunk.size() == 1)           // back-to-back zeros: an idle delimiter, not a bad frame
            continue;
        Frame frame;
        if (decodeFrame(chunk, &frame))
            frames.append(frame);
        else
            ++m_invalid;
    }
    m_buffer.remove(0, start);
    if (m_buffer.size() > kMaxPendingBytes) {   // no zero in sight: noise, drop it
        m_buffer.clear();
        ++m_invalid;
    }
    return frames;
}

void FrameSplitter::clear()
{
    m_buffer.clear();
}

QString formatVersion(quint32 packed)
{
    // fw_package.format_version() raises for a reserved top byte; a display helper must not, and
    // the INFO line prints the raw word next to it anyway.
    if (packed > 0x00FFFFFFu)
        return QStringLiteral("unknown");
    return QStringLiteral("%1.%2.%3").arg((packed >> 16) & 0xFF).arg((packed >> 8) & 0xFF).arg(packed & 0xFF);
}

QString bootStateName(quint32 state)
{
    switch (state) {
    case 1:  return QStringLiteral("valid_application");
    case 2:  return QStringLiteral("update_requested");
    case 3:  return QStringLiteral("update_in_progress");
    default: return QStringLiteral("unknown");
    }
}

QString roleName(quint32 role)
{
    switch (role) {
    case RoleUnset: return QStringLiteral("unset");
    case RoleRx:    return QStringLiteral("rx");
    case RoleTx:    return QStringLiteral("tx");
    default:        return QStringLiteral("unknown");
    }
}

std::optional<quint32> parseRole(const QString& text)
{
    const QString lower = text.toLower();
    if (lower == QLatin1String("rx"))
        return static_cast<quint32>(RoleRx);
    if (lower == QLatin1String("tx"))
        return static_cast<quint32>(RoleTx);
    return std::nullopt;
}

QString targetName(quint32 target)
{
    return target == 0x4343314Du ? QStringLiteral("CC1M") : QStringLiteral("unknown");
}

QString formatEnum(quint32 value, const QString& name)
{
    return QStringLiteral("%1(%2)").arg(name).arg(value);
}

QString formatTarget(quint32 target)
{
    const QString hex = QString::number(target, 16).rightJustified(8, QLatin1Char('0')).toUpper();
    return QStringLiteral("%1(0x%2)").arg(targetName(target), hex);
}

bool DeviceInfo::operator==(const DeviceInfo& other) const
{
    return target == other.target && state == other.state && maxSize == other.maxSize
        && imageSize == other.imageSize && version == other.version && role == other.role;
}

QString DeviceInfo::toString() const
{
    const QString versionHex = QString::number(version, 16).rightJustified(8, QLatin1Char('0')).toUpper();
    QStringList fields;
    fields << QStringLiteral("target=%1").arg(formatTarget(target))
           << QStringLiteral("state=%1").arg(formatEnum(state, bootStateName(state)))
           << QStringLiteral("max_size=%1").arg(maxSize)
           << QStringLiteral("image_size=%1").arg(imageSize)
           << QStringLiteral("version=%1(0x%2)").arg(formatVersion(version), versionHex);
    if (role)
        fields << QStringLiteral("role=%1").arg(formatEnum(*role, roleName(*role)));
    return QStringLiteral("INFO ") + fields.join(QLatin1Char(' '));
}

bool parseInfo(const QByteArray& payload, DeviceInfo* info)
{
    if (payload.size() != 20 && payload.size() != 24)
        return false;
    if (info) {
        const uchar* bytes = reinterpret_cast<const uchar*>(payload.constData());
        DeviceInfo parsed;
        parsed.target = qFromLittleEndian<quint32>(bytes);
        parsed.state = qFromLittleEndian<quint32>(bytes + 4);
        parsed.maxSize = qFromLittleEndian<quint32>(bytes + 8);
        parsed.imageSize = qFromLittleEndian<quint32>(bytes + 12);
        parsed.version = qFromLittleEndian<quint32>(bytes + 16);
        if (payload.size() == 24)
            parsed.role = qFromLittleEndian<quint32>(bytes + 20);
        *info = parsed;
    }
    return true;
}

} // namespace CcProtocol
