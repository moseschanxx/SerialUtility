// STUB (v0.5 scaffolding): the protocol work package writes the real suite (McuUpdater against
// FakeMcu / FakeMcuServer: full update, already in the updater, NACK/resume, lost frames, banner,
// set role, power logged before bootloader, cancel, connection closed).
#include <QtTest>

#include "firmware/McuUpdater.h"
#include "support/FakeMcu.h"

class tst_mcuupdater : public QObject
{
    Q_OBJECT
private slots:
    void placeholder() { QVERIFY(true); }
};

QTEST_GUILESS_MAIN(tst_mcuupdater)
#include "tst_mcuupdater.moc"
