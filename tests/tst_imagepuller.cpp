// STUB (v0.5 scaffolding): the ssh-pull work package writes the real suite (SshConnection
// runCommand / statRemote against TestSshServer with and without SFTP; ImagePuller: stat, skip,
// .part, RKFW magic, one-level history, roll back, git describe, extra files, cancel).
#include <QtTest>

#include "firmware/ImagePuller.h"
#include "ssh/SshConnection.h"
#include "support/TestSshServer.h"

class tst_imagepuller : public QObject
{
    Q_OBJECT
private slots:
    void placeholder() { QVERIFY(true); }
};

QTEST_GUILESS_MAIN(tst_imagepuller)
#include "tst_imagepuller.moc"
