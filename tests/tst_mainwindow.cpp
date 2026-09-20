// PLACEHOLDER GUI test - replaced by the gui-tests workflow. Runs offscreen (QT_QPA_PLATFORM=offscreen).
#include <QtTest>
#include <QApplication>
class Tst_mainwindow : public QObject
{
    Q_OBJECT
private slots:
    void placeholder() { QVERIFY(qApp != nullptr); }
};
QTEST_MAIN(Tst_mainwindow)
#include "tst_mainwindow.moc"
