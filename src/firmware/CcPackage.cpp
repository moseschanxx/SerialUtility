// STUB (v0.5 scaffolding): replaced by the protocol work package.
#include "firmware/CcPackage.h"
#include "firmware/CcProtocol.h"

QString CcPackage::versionText() const { return CcProtocol::formatVersion(version); }
QString CcPackage::targetText() const { return CcProtocol::formatTarget(target); }
QString CcPackage::summary() const { return QString(); }
bool CcPackage::parse(const QByteArray&, CcPackage*, QString* error)
{
    if (error) *error = QStringLiteral("not implemented");
    return false;
}
bool CcPackage::load(const QString&, CcPackage*, QString* error)
{
    if (error) *error = QStringLiteral("not implemented");
    return false;
}
QByteArray CcPackage::build(const QByteArray&, quint32, quint32, QString* error)
{
    if (error) *error = QStringLiteral("not implemented");
    return QByteArray();
}
quint32 CcPackage::crc32(const QByteArray&, quint32 seed) { return seed; }
