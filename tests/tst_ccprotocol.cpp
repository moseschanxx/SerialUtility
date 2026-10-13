// STUB (v0.5 scaffolding): the protocol work package writes the real suite (known-answer vectors
// from tests/data/firmware/vectors.json, CcPackage parse / build, FrameSplitter).
#include <QtTest>

#include "firmware/CcProtocol.h"
#include "firmware/CcPackage.h"

class tst_ccprotocol : public QObject
{
    Q_OBJECT
private slots:
    void placeholder() { QVERIFY(true); }
};

QTEST_GUILESS_MAIN(tst_ccprotocol)
#include "tst_ccprotocol.moc"
