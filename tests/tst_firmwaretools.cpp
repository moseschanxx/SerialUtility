// STUB (v0.5 scaffolding): the tools work package writes the real suite (ToolRunner / FakeToolRunner,
// ToolLocator, RockchipTool parsers incl. ProgressParser, AdbTool parsers, UsbProbe::driverHint,
// McuLink and SocFlasher state machines with FakeToolRunner).
#include <QtTest>

#include "firmware/AdbTool.h"
#include "firmware/McuLink.h"
#include "firmware/RockchipTool.h"
#include "firmware/SocFlasher.h"
#include "firmware/ToolRunner.h"
#include "firmware/UsbProbe.h"
#include "support/FakeToolRunner.h"

class tst_firmwaretools : public QObject
{
    Q_OBJECT
private slots:
    void placeholder() { QVERIFY(true); }
};

QTEST_GUILESS_MAIN(tst_firmwaretools)
#include "tst_firmwaretools.moc"
