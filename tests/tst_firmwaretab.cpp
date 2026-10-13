// STUB (v0.5 scaffolding): the ui work package writes the real suite (FirmwareTab offscreen with
// FakeToolRunner, FakeMcuServer and TestSshServer; MainWindow with the Firmware tab current).
#include <QtTest>

#include "ui/FirmwareTab.h"
#include "ui/MainWindow.h"
#include "support/FakeMcu.h"
#include "support/FakeToolRunner.h"
#include "support/TestSshServer.h"

class tst_firmwaretab : public QObject
{
    Q_OBJECT
private slots:
    void placeholder() { QVERIFY(true); }
};

QTEST_MAIN(tst_firmwaretab)
#include "tst_firmwaretab.moc"
