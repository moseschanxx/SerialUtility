// STUB (v0.5 scaffolding): replaced by the protocol work package. Every function compiles and
// returns a default so the tree links; nothing here is correct yet.
#include "firmware/CcProtocol.h"

namespace CcProtocol {

QString messageTypeName(quint8 type) { return QStringLiteral("type %1").arg(type); }
quint16 crc16Ccitt(const QByteArray&) { return 0; }
QByteArray cobsEncode(const QByteArray&) { return QByteArray(); }
bool cobsDecode(const QByteArray&, QByteArray*) { return false; }
bool Frame::operator==(const Frame& o) const { return type == o.type && sequence == o.sequence && payload == o.payload; }
QByteArray encodeFrame(quint8, quint16, const QByteArray&) { return QByteArray(); }
bool decodeFrame(const QByteArray&, Frame*, QString*) { return false; }
QList<Frame> FrameSplitter::feed(const QByteArray&) { return {}; }
void FrameSplitter::clear() { m_buffer.clear(); }
QString DeviceInfo::toString() const { return QString(); }
bool DeviceInfo::operator==(const DeviceInfo& o) const
{
    return target == o.target && state == o.state && maxSize == o.maxSize && imageSize == o.imageSize
        && version == o.version && role == o.role;
}
bool parseInfo(const QByteArray&, DeviceInfo*) { return false; }
QString formatVersion(quint32) { return QString(); }
QString bootStateName(quint32) { return QStringLiteral("unknown"); }
QString roleName(quint32) { return QStringLiteral("unknown"); }
std::optional<quint32> parseRole(const QString&) { return std::nullopt; }
QString targetName(quint32) { return QStringLiteral("unknown"); }
QString formatEnum(quint32 value, const QString& name) { return QStringLiteral("%1(%2)").arg(name).arg(value); }
QString formatTarget(quint32 target) { return QStringLiteral("unknown(0x%1)").arg(target, 8, 16, QLatin1Char('0')).toUpper(); }

} // namespace CcProtocol
