// GUI test suite for the SSH strip and dialogs: SshConnectionBar (stored profiles + recent
// targets, free-text targets, connect button states), HostKeyDialog (every HostKeyStatus
// variant), AuthPromptDialog, SshProfilesDialog (working copy, validation, port forwards,
// secrets, import / export) and RemoteFileDialog (defaults per direction, request contents,
// QSettings persistence). Runs offscreen (QT_QPA_PLATFORM=offscreen); QSettings and every
// file live in a temporary directory or the QStandardPaths test-mode data directory, so
// nothing touches the user's real configuration, known_hosts file or keys. SshConnection is
// never opened: the dialogs only see its signals, which the tests emit themselves.
#include <QtTest>

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMap>
#include <QMetaObject>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>

#include <memory>

#include "app/AppSettings.h"
#include "core/Transport.h"
#include "dialogs/AuthPromptDialog.h"
#include "dialogs/HostKeyDialog.h"
#include "dialogs/RemoteFileDialog.h"
#include "dialogs/SshProfilesDialog.h"
#include "ssh/SecretStore.h"
#include "ssh/SshConnection.h"
#include "ssh/SshProfile.h"
#include "ui/SshConnectionBar.h"

namespace {

using State = Transport::State;
using HostKeyStatus = SshConnection::HostKeyStatus;
using PromptKind = SshConnection::PromptKind;
using Direction = SshConnection::TransferDirection;

const QString kLuckfoxId = QStringLiteral("id-luckfox");
const QString kKaliId = QStringLiteral("id-kali");
const QString kKaliItemText = QStringLiteral("Kali box - moses@10.0.0.24:2200");
const QString kLuckfoxItemText = QStringLiteral("Luckfox Pico - root@192.168.100.2");
const QString kRecentPi = QStringLiteral("pi@raspberrypi:2222");
const QString kRecentRoot = QStringLiteral("root@10.0.0.24");

template <typename T>
T* child(const QObject* parent, const char* objectName)
{
    return parent->findChild<T*>(QLatin1String(objectName));
}

/// Show a top-level widget offscreen and wait until it is exposed.
bool expose(QWidget* widget)
{
    widget->show();
    return QTest::qWaitForWindowExposed(widget);
}

/// QComboBox::insertSeparator() marks the item with this accessible description.
bool isSeparator(const QComboBox* combo, int index)
{
    return combo->itemData(index, Qt::AccessibleDescriptionRole).toString() == QLatin1String("separator");
}

QStringList itemTexts(const QComboBox* combo)
{
    QStringList texts;
    for (int i = 0; i < combo->count(); ++i) {
        texts.append(isSeparator(combo, i) ? QStringLiteral("<sep>") : combo->itemText(i));
    }
    return texts;
}

bool isReddish(const QColor& color)
{
    return color.red() > 150 && color.red() > color.green() + 60 && color.red() > color.blue() + 60;
}

bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    return file.write(bytes) == bytes.size();
}

/// Whole file, with the handle closed again on return (a QSaveFile cannot replace an open file on Windows).
QByteArray readFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

SshProfile luckfoxProfile()
{
    SshProfile p;
    p.id = kLuckfoxId;
    p.name = QStringLiteral("Luckfox Pico");
    p.host = QStringLiteral("192.168.100.2");
    p.user = QStringLiteral("root");
    p.description = QStringLiteral("RV1106 over the direct cable");
    return p;
}

SshProfile kaliProfile()
{
    SshProfile p;
    p.id = kKaliId;
    p.name = QStringLiteral("Kali box");
    p.host = QStringLiteral("10.0.0.24");
    p.user = QStringLiteral("moses");
    p.port = 2200;
    p.auth = SshProfile::Auth::Password;
    return p;
}

SshConnection::HostKeyInfo hostKeyInfo(HostKeyStatus status, const QString& knownHostsFile)
{
    SshConnection::HostKeyInfo info;
    info.host = QStringLiteral("192.168.100.2");
    info.port = 22;
    info.keyType = QStringLiteral("ssh-ed25519");
    info.fingerprintSha256 = QStringLiteral("SHA256:5m3GxE0yKQz0v0J1yD9a8QmYw3iVfXZxQ5bE0kqkq0M");
    info.fingerprintMd5 = QStringLiteral("MD5:9f:86:d0:81:88:4c:7d:65:9a:2f:ea:a0:c5:5a:d0:15");
    info.publicKeyLine = QStringLiteral("ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGtest");
    info.status = status;
    info.knownHostsFile = knownHostsFile;
    return info;
}

SshConnection::AuthPrompt authPrompt(PromptKind kind = PromptKind::Password)
{
    SshConnection::AuthPrompt p;
    p.kind = kind;
    p.prompt = QStringLiteral("Password:");
    p.user = QStringLiteral("root");
    p.host = QStringLiteral("192.168.100.2");
    p.canRemember = true;
    return p;
}

} // namespace

class Tst_sshdialogs : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // ---- SshConnectionBar -----------------------------------------------------------
    void barListsProfilesAndRecentTargets();
    void barSelectProfile();
    void barAdHocTarget();
    void barInvalidTextIsRed();
    void barConnectButtonPerState();
    void barEnterConnects();
    void barProfileChangedEmissions();
    void barStoreChangedKeepsSelection();
    void barRemovedProfileFallsBackToTarget();
    void barSummaryLabel();
    void barGearEmitsEdit();
    void barRetranslate();

    // ---- HostKeyDialog ---------------------------------------------------------------
    void hostKeyUnknownVariants_data();
    void hostKeyUnknownVariants();
    void hostKeyRememberAndOnce();
    void hostKeyChangedVariant();
    void hostKeyCancelRejects();
    void hostKeyFingerprintFields();

    // ---- AuthPromptDialog ------------------------------------------------------------
    void authMaskedAndShowToggle();
    void authEchoPrompt();
    void authRememberVisibility();
    void authRememberLabels_data();
    void authRememberLabels();
    void authAttemptLabel();
    void authEnterAccepts();
    void authEscapeRejects();

    // ---- SshProfilesDialog -----------------------------------------------------------
    void profilesLoadsStore();
    void profilesNewDuplicateDelete();
    void profilesEditingUpdatesWorkingCopyAndList();
    void profilesValidationEmptyHost();
    void profilesValidationPublicKeyFile();
    void profilesForwardsRoundTrip();
    void profilesApplyWritesStoreAndSaves();
    void profilesConnectEmitsEditedProfile();
    void profilesCancelDiscards();
    void profilesImportExportRoundTrip();
    void profilesPasswordSaveAndRemove();

    // ---- RemoteFileDialog ------------------------------------------------------------
    void remoteDirectionDefaults();
    void remoteRequestContents();
    void remoteStartDisabledWhileDisconnected();
    void remoteHomeButton();
    void remotePathsPersist();
    void remoteRestoredPathsAreDerived();
    void remoteRetranslate();
    void remoteMethodTexts();
    void remoteRequestNormalisation();
    void remoteAcceleratorsUnique();
    void remoteHomeUnresolved();
    void remoteErrorLineSurvivesDisconnect();

private:
    std::unique_ptr<SshProfileStore> newStore();
    QString tempPath(const QString& name) const;

    QTemporaryDir m_tempDir;
};

// =======================================================================================
// Fixture
// =======================================================================================

void Tst_sshdialogs::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication::setOrganizationName(QStringLiteral("BuildAI-Test"));
    QCoreApplication::setApplicationName(QStringLiteral("SerialUtilityTest-sshdialogs"));
    QVERIFY(m_tempDir.isValid());

    // QSettings (AppSettings, SecretStore, the RemoteFileDialog paths) stays out of the
    // registry and inside the temporary directory.
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_tempDir.filePath(QStringLiteral("settings")));
    QSettings settings;
    settings.clear();
    QVERIFY(settings.fileName().contains(QStringLiteral("BuildAI-Test")));
    QVERIFY(settings.fileName().contains(QStringLiteral("SerialUtilityTest-sshdialogs")));

    // The profile store's default file lives in the test-mode data directory.
    QVERIFY(SshProfileStore::defaultFilePath().contains(QStringLiteral("SerialUtilityTest-sshdialogs")));
    QFile::remove(SshProfileStore::defaultFilePath());
}

void Tst_sshdialogs::init()
{
    QFile::remove(SshProfileStore::defaultFilePath());
    QSettings().remove(QStringLiteral("remoteFile"));
}

std::unique_ptr<SshProfileStore> Tst_sshdialogs::newStore()
{
    auto store = std::make_unique<SshProfileStore>();
    // A missing file loads an empty list; nothing is ever written unless a test saves.
    if (!store->load(tempPath(QStringLiteral("does-not-exist.json")))) {
        return nullptr;
    }
    store->setProfiles({luckfoxProfile(), kaliProfile()});
    store->addRecentTarget(kRecentRoot);
    store->addRecentTarget(kRecentPi);   // most recent first
    return store;
}

QString Tst_sshdialogs::tempPath(const QString& name) const
{
    return m_tempDir.filePath(name);
}

// =======================================================================================
// SshConnectionBar
// =======================================================================================

void Tst_sshdialogs::barListsProfilesAndRecentTargets()
{
    auto store = newStore();
    QVERIFY(store);
    SshConnectionBar bar;
    bar.setStore(store.get());
    QCOMPARE(bar.store(), store.get());
    QVERIFY(expose(&bar));

    auto* combo = child<QComboBox>(&bar, "targetCombo");
    QVERIFY(combo);
    QVERIFY(combo->isEditable());
    QVERIFY(combo->completer());
    QCOMPARE(itemTexts(combo),
             QStringList({kKaliItemText, kLuckfoxItemText, QStringLiteral("<sep>"), kRecentPi, kRecentRoot}));
    QCOMPARE(combo->itemData(0).toString(), kKaliId);
    QCOMPARE(combo->itemData(1).toString(), kLuckfoxId);
    QVERIFY(isSeparator(combo, 2));
    QVERIFY(combo->itemData(3).toString().isEmpty());
    QVERIFY(combo->itemData(4).toString().isEmpty());

    // Nothing selected yet: no target, grey dot, nothing red.
    QCOMPARE(combo->currentIndex(), -1);
    QVERIFY(bar.targetText().isEmpty());
    QVERIFY(!bar.hasValidTarget());
    QVERIFY(!bar.currentProfile().isValid());
    QVERIFY(!isReddish(combo->lineEdit()->palette().color(QPalette::Text)));

    // Without a store the combo is empty but free text still works.
    SshConnectionBar bare;
    QVERIFY(bare.store() == nullptr);
    QCOMPARE(child<QComboBox>(&bare, "targetCombo")->count(), 0);
    bare.setTarget(QStringLiteral("root@host"));
    QVERIFY(bare.hasValidTarget());
}

void Tst_sshdialogs::barSelectProfile()
{
    auto store = newStore();
    QVERIFY(store);
    SshConnectionBar bar;
    bar.setStore(store.get());
    QVERIFY(expose(&bar));
    QSignalSpy changed(&bar, &SshConnectionBar::profileChanged);

    bar.selectProfile(kLuckfoxId);
    QVERIFY(bar.hasValidTarget());
    QCOMPARE(bar.targetText(), kLuckfoxItemText);
    SshProfile current = bar.currentProfile();
    QCOMPARE(current.id, kLuckfoxId);
    QCOMPARE(current.host, QStringLiteral("192.168.100.2"));
    QCOMPARE(current.user, QStringLiteral("root"));
    QCOMPARE(current.description, QStringLiteral("RV1106 over the direct cable"));

    bar.selectProfile(kKaliId);
    current = bar.currentProfile();
    QCOMPARE(current.id, kKaliId);
    QCOMPARE(current.port, quint16(2200));
    QCOMPARE(current.auth, SshProfile::Auth::Password);

    // Unknown ids (and the empty id) are ignored.
    bar.selectProfile(QStringLiteral("no-such-profile"));
    QCOMPARE(bar.currentProfile().id, kKaliId);
    bar.selectProfile(QString());
    QCOMPARE(bar.currentProfile().id, kKaliId);

    // Programmatic selection is silent (SessionWidget reads currentProfile() itself).
    QCOMPARE(changed.count(), 0);
}

void Tst_sshdialogs::barAdHocTarget()
{
    auto store = newStore();
    QVERIFY(store);
    SshConnectionBar bar;
    bar.setStore(store.get());
    QVERIFY(expose(&bar));
    auto* combo = child<QComboBox>(&bar, "targetCombo");

    bar.setTarget(QStringLiteral("  root@192.168.1.5:2222 "));
    QCOMPARE(bar.targetText(), QStringLiteral("root@192.168.1.5:2222"));
    QVERIFY(bar.hasValidTarget());
    SshProfile adHoc = bar.currentProfile();
    QVERIFY(adHoc.id.isEmpty());
    QCOMPARE(adHoc.host, QStringLiteral("192.168.1.5"));
    QCOMPARE(adHoc.port, quint16(2222));
    QCOMPARE(adHoc.user, QStringLiteral("root"));
    QCOMPARE(adHoc.auth, SshProfile::Auth::Auto);
    QVERIFY(!isReddish(combo->lineEdit()->palette().color(QPalette::Text)));

    // ssh:// URLs and bracketed IPv6 literals parse too.
    bar.setTarget(QStringLiteral("ssh://moses@gateway:2200"));
    adHoc = bar.currentProfile();
    QCOMPARE(adHoc.host, QStringLiteral("gateway"));
    QCOMPARE(adHoc.port, quint16(2200));
    QCOMPARE(adHoc.user, QStringLiteral("moses"));
    bar.setTarget(QStringLiteral("[fe80::1]:22"));
    QVERIFY(bar.hasValidTarget());
    QCOMPARE(bar.currentProfile().host, QStringLiteral("fe80::1"));

    // A typed recent target selects its item, still an ad-hoc profile (no id).
    bar.setTarget(kRecentPi);
    QCOMPARE(combo->currentIndex(), 3);
    adHoc = bar.currentProfile();
    QVERIFY(adHoc.id.isEmpty());
    QCOMPARE(adHoc.host, QStringLiteral("raspberrypi"));
    QCOMPARE(adHoc.port, quint16(2222));

    // Editing the text of a selected stored profile makes it free text again.
    bar.selectProfile(kLuckfoxId);
    QCOMPARE(bar.currentProfile().id, kLuckfoxId);
    QTest::keyClicks(combo->lineEdit(), QStringLiteral("x"));
    QVERIFY(bar.currentProfile().id.isEmpty());
    QVERIFY(!bar.hasValidTarget());   // "Luckfox Pico - root@192.168.100.2x" is not a target
}

void Tst_sshdialogs::barInvalidTextIsRed()
{
    auto store = newStore();
    QVERIFY(store);
    SshConnectionBar bar;
    bar.setStore(store.get());
    QVERIFY(expose(&bar));
    auto* combo = child<QComboBox>(&bar, "targetCombo");
    QLineEdit* edit = combo->lineEdit();
    QVERIFY(edit);
    const QColor normal = edit->palette().color(QPalette::Text);

    QTest::keyClicks(edit, QStringLiteral("not a target!!"));
    QVERIFY(!bar.hasValidTarget());
    QVERIFY(isReddish(edit->palette().color(QPalette::Text)));
    QCOMPARE(combo->toolTip(), QStringLiteral("Enter user@host[:port]"));

    edit->clear();
    QVERIFY(!bar.hasValidTarget());   // empty is not valid either...
    QCOMPARE(edit->palette().color(QPalette::Text), normal);   // ...but nothing to complain about
    QVERIFY(combo->toolTip() != QStringLiteral("Enter user@host[:port]"));

    QTest::keyClicks(edit, QStringLiteral("root@1.2.3.4"));
    QVERIFY(bar.hasValidTarget());
    QCOMPARE(edit->palette().color(QPalette::Text), normal);
}

void Tst_sshdialogs::barConnectButtonPerState()
{
    auto store = newStore();
    QVERIFY(store);
    SshConnectionBar bar;
    bar.setStore(store.get());
    QVERIFY(expose(&bar));
    auto* button = child<QPushButton>(&bar, "connectButton");
    auto* combo = child<QComboBox>(&bar, "targetCombo");
    auto* gear = child<QToolButton>(&bar, "profilesButton");
    QVERIFY(button && combo && gear);
    QSignalSpy connectSpy(&bar, &SshConnectionBar::connectRequested);
    QSignalSpy disconnectSpy(&bar, &SshConnectionBar::disconnectRequested);

    bar.setTarget(QStringLiteral("root@1.2.3.4"));
    QCOMPARE(button->text(), QStringLiteral("Connect"));
    QVERIFY(combo->isEnabled());
    QVERIFY(gear->isEnabled());
    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(connectSpy.count(), 1);
    QCOMPARE(disconnectSpy.count(), 0);

    bar.setConnectionState(State::Connecting);
    QCOMPARE(button->text(), QStringLiteral("Connecting..."));
    QVERIFY(!combo->isEnabled());
    QVERIFY(!gear->isEnabled());
    QTest::mouseClick(button, Qt::LeftButton);   // cancel
    QCOMPARE(connectSpy.count(), 1);
    QCOMPARE(disconnectSpy.count(), 1);

    bar.setConnectionState(State::Connected);
    QCOMPARE(button->text(), QStringLiteral("Disconnect"));
    QVERIFY(!combo->isEnabled());
    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(disconnectSpy.count(), 2);

    bar.setConnectionState(State::Reconnecting);
    QCOMPARE(button->text(), QStringLiteral("Reconnecting..."));
    QVERIFY(!combo->isEnabled());
    QTest::mouseClick(button, Qt::LeftButton);   // cancel
    QCOMPARE(disconnectSpy.count(), 3);

    bar.setConnectionState(State::Disconnected);
    QCOMPARE(button->text(), QStringLiteral("Connect"));
    QVERIFY(combo->isEnabled());
    QVERIFY(gear->isEnabled());
    QCOMPARE(connectSpy.count(), 1);
}

void Tst_sshdialogs::barEnterConnects()
{
    auto store = newStore();
    QVERIFY(store);
    SshConnectionBar bar;
    bar.setStore(store.get());
    QVERIFY(expose(&bar));
    QLineEdit* edit = child<QComboBox>(&bar, "targetCombo")->lineEdit();
    QSignalSpy connectSpy(&bar, &SshConnectionBar::connectRequested);

    QTest::keyClicks(edit, QStringLiteral("root@1.2.3.4"));
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(connectSpy.count(), 1);

    // Invalid text: Enter does nothing.
    edit->clear();
    QTest::keyClicks(edit, QStringLiteral("garbage target"));
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(connectSpy.count(), 1);

    // Not while a session is up.
    edit->clear();
    QTest::keyClicks(edit, QStringLiteral("root@1.2.3.4"));
    bar.setConnectionState(State::Connected);
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(connectSpy.count(), 1);
}

void Tst_sshdialogs::barProfileChangedEmissions()
{
    auto store = newStore();
    QVERIFY(store);
    SshConnectionBar bar;
    bar.setStore(store.get());
    QVERIFY(expose(&bar));
    auto* combo = child<QComboBox>(&bar, "targetCombo");
    QLineEdit* edit = combo->lineEdit();
    QList<SshProfile> emitted;
    connect(&bar, &SshConnectionBar::profileChanged, this, [&emitted](const SshProfile& p) { emitted.append(p); });

    bar.selectProfile(kLuckfoxId);   // programmatic: silent
    QCOMPARE(emitted.size(), 0);

    combo->setCurrentIndex(0);       // like picking from the popup
    QCOMPARE(emitted.size(), 1);
    QCOMPARE(emitted.last().id, kKaliId);

    combo->setCurrentIndex(3);       // a recent target: ad-hoc profile
    QCOMPARE(emitted.size(), 2);
    QVERIFY(emitted.last().id.isEmpty());
    QCOMPARE(emitted.last().host, QStringLiteral("raspberrypi"));
    QCOMPARE(emitted.last().port, quint16(2222));

    // Typed text + Enter: reported once, even though Enter also matches the recent item
    // (currentIndexChanged) and the line edit reports editingFinished.
    edit->clear();
    QTest::keyClicks(edit, kRecentRoot);
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(emitted.size(), 3);
    QCOMPARE(emitted.last().host, QStringLiteral("10.0.0.24"));
    QCOMPARE(emitted.last().port, quint16(22));
    QTest::keyClick(edit, Qt::Key_Return);   // same target again: nothing new
    QCOMPARE(emitted.size(), 3);

    // Invalid text is never reported; a fresh valid one is.
    edit->clear();
    QTest::keyClicks(edit, QStringLiteral("nope nope"));
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(emitted.size(), 3);
    edit->clear();
    QTest::keyClicks(edit, QStringLiteral("moses@10.0.0.24:2200"));
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(emitted.size(), 4);
    QCOMPARE(emitted.last().port, quint16(2200));
}

void Tst_sshdialogs::barStoreChangedKeepsSelection()
{
    auto store = newStore();
    QVERIFY(store);
    SshConnectionBar bar;
    bar.setStore(store.get());
    QVERIFY(expose(&bar));
    auto* combo = child<QComboBox>(&bar, "targetCombo");
    QSignalSpy changed(&bar, &SshConnectionBar::profileChanged);

    // A stored profile stays selected when the list grows in front of it.
    bar.selectProfile(kLuckfoxId);
    SshProfile extra;
    extra.id = QStringLiteral("id-aardvark");
    extra.name = QStringLiteral("Aardvark");
    extra.host = QStringLiteral("a.example");
    store->upsert(extra);
    QCOMPARE(combo->count(), 6);
    QCOMPARE(combo->itemText(0), QStringLiteral("Aardvark - a.example"));
    QCOMPARE(bar.currentProfile().id, kLuckfoxId);
    QCOMPARE(bar.targetText(), kLuckfoxItemText);
    QCOMPARE(combo->currentIndex(), 2);

    // A renamed profile shows its new text.
    extra.name = QStringLiteral("Zebra");
    store->upsert(extra);
    QCOMPARE(combo->itemText(2), QStringLiteral("Zebra - a.example"));
    QCOMPARE(bar.currentProfile().id, kLuckfoxId);

    // Free text survives a rebuild.
    bar.setTarget(QStringLiteral("root@1.2.3.4"));
    store->addRecentTarget(QStringLiteral("x@y"));
    QCOMPARE(bar.targetText(), QStringLiteral("root@1.2.3.4"));
    QVERIFY(bar.hasValidTarget());
    QVERIFY(bar.currentProfile().id.isEmpty());

    // A selected recent target is re-selected.
    bar.setTarget(kRecentRoot);
    QVERIFY(combo->currentIndex() >= 0);
    store->addRecentTarget(QStringLiteral("new@host"));
    QCOMPARE(bar.targetText(), kRecentRoot);
    QCOMPARE(combo->itemText(combo->currentIndex()), kRecentRoot);

    QCOMPARE(changed.count(), 0);   // rebuilds are silent
}

void Tst_sshdialogs::barRemovedProfileFallsBackToTarget()
{
    auto store = newStore();
    QVERIFY(store);
    SshConnectionBar bar;
    bar.setStore(store.get());
    QVERIFY(expose(&bar));

    bar.selectProfile(kKaliId);
    QVERIFY(store->remove(kKaliId));
    QCOMPARE(bar.targetText(), QStringLiteral("moses@10.0.0.24:2200"));
    QVERIFY(bar.hasValidTarget());
    const SshProfile current = bar.currentProfile();
    QVERIFY(current.id.isEmpty());
    QCOMPARE(current.host, QStringLiteral("10.0.0.24"));
    QCOMPARE(current.port, quint16(2200));
    QCOMPARE(current.user, QStringLiteral("moses"));

    // Dropping the store empties the list, the text stays.
    bar.setStore(nullptr);
    QCOMPARE(child<QComboBox>(&bar, "targetCombo")->count(), 0);
    QCOMPARE(bar.targetText(), QStringLiteral("moses@10.0.0.24:2200"));
}

void Tst_sshdialogs::barSummaryLabel()
{
    auto store = newStore();
    QVERIFY(store);
    SshConnectionBar bar;
    bar.setStore(store.get());
    QVERIFY(expose(&bar));
    auto* info = child<QLabel>(&bar, "infoLabel");
    QVERIFY(info);
    QVERIFY(!info->isVisible());

    bar.setSummary(QStringLiteral("ssh-ed25519 \u00b7 publickey"));
    QVERIFY(!info->isVisible());   // only while a session is up
    bar.setConnectionState(State::Connected);
    QVERIFY(info->isVisible());
    QCOMPARE(info->text(), QStringLiteral("ssh-ed25519 \u00b7 publickey"));
    bar.setConnectionState(State::Reconnecting);
    QVERIFY(info->isVisible());
    bar.setConnectionState(State::Disconnected);
    QVERIFY(!info->isVisible());

    bar.setConnectionState(State::Connected);
    bar.setSummary(QString());
    QVERIFY(!info->isVisible());
}

void Tst_sshdialogs::barGearEmitsEdit()
{
    SshConnectionBar bar;
    QVERIFY(expose(&bar));
    QSignalSpy spy(&bar, &SshConnectionBar::profilesEditRequested);
    QTest::mouseClick(child<QToolButton>(&bar, "profilesButton"), Qt::LeftButton);
    QCOMPARE(spy.count(), 1);

    // setFocusToTarget(): the combo is the focus widget (its line edit's focus proxy in Qt 6)
    // and the current text is selected for overtyping.
    auto* combo = child<QComboBox>(&bar, "targetCombo");
    bar.setTarget(QStringLiteral("root@1.2.3.4"));
    bar.setFocusToTarget();
    QCOMPARE(bar.focusWidget(), combo);
    QCOMPARE(combo->lineEdit()->selectedText(), QStringLiteral("root@1.2.3.4"));
}

void Tst_sshdialogs::barRetranslate()
{
    auto store = newStore();
    QVERIFY(store);
    SshConnectionBar bar;
    bar.setStore(store.get());
    QVERIFY(expose(&bar));
    bar.selectProfile(kLuckfoxId);
    bar.setConnectionState(State::Connected);

    QEvent event(QEvent::LanguageChange);
    QApplication::sendEvent(&bar, &event);
    QCOMPARE(child<QPushButton>(&bar, "connectButton")->text(), QStringLiteral("Disconnect"));
    QCOMPARE(bar.currentProfile().id, kLuckfoxId);
    QCOMPARE(bar.targetText(), kLuckfoxItemText);
    QVERIFY(!child<QComboBox>(&bar, "targetCombo")->isEnabled());   // state survives the retranslate
}

// =======================================================================================
// HostKeyDialog
// =======================================================================================

void Tst_sshdialogs::hostKeyUnknownVariants_data()
{
    QTest::addColumn<int>("status");
    QTest::addColumn<QString>("message");
    QTest::newRow("unknown") << static_cast<int>(HostKeyStatus::Unknown) << QString();
    QTest::newRow("key type changed") << static_cast<int>(HostKeyStatus::KeyTypeChanged) << QString();
    QTest::newRow("error") << static_cast<int>(HostKeyStatus::Error)
                           << QStringLiteral("known_hosts could not be read: permission denied");
}

void Tst_sshdialogs::hostKeyUnknownVariants()
{
    QFETCH(int, status);
    QFETCH(QString, message);
    SshConnection::HostKeyInfo info =
        hostKeyInfo(static_cast<HostKeyStatus>(status), tempPath(QStringLiteral("known_hosts")));
    info.message = message;

    HostKeyDialog dialog(info);
    QVERIFY(expose(&dialog));
    auto* headline = child<QLabel>(&dialog, "labelHeadline");
    auto* remember = child<QPushButton>(&dialog, "buttonRemember");
    auto* once = child<QPushButton>(&dialog, "buttonOnce");
    auto* cancel = child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Cancel);
    auto* understand = child<QCheckBox>(&dialog, "checkUnderstand");
    auto* messageLabel = child<QLabel>(&dialog, "labelMessage");
    auto* knownHosts = child<QLabel>(&dialog, "labelKnownHosts");
    QVERIFY(headline && remember && once && cancel && understand && messageLabel && knownHosts);

    QCOMPARE(headline->text(), QStringLiteral("The authenticity of host '192.168.100.2:22' can't be established."));
    QCOMPARE(child<QLabel>(&dialog, "labelHost")->text(), QStringLiteral("192.168.100.2:22"));
    QCOMPARE(child<QLabel>(&dialog, "labelKeyType")->text(), QStringLiteral("ssh-ed25519"));
    QCOMPARE(remember->text(), QStringLiteral("Connect and remember"));
    QVERIFY(remember->isEnabled());
    QVERIFY(remember->isDefault());
    QCOMPARE(once->text(), QStringLiteral("Connect once"));
    QVERIFY(once->isEnabled());
    QVERIFY(!once->isDefault());
    QVERIFY(!cancel->isDefault());
    QVERIFY(!understand->isVisible());
    QCOMPARE(messageLabel->isVisible(), !message.isEmpty());
    QCOMPARE(messageLabel->text(), message);
    QVERIFY(knownHosts->text().contains(QStringLiteral("known_hosts")));
    QVERIFY(knownHosts->text().contains(QDir::toNativeSeparators(tempPath(QStringLiteral("known_hosts")))));
    QVERIFY(!dialog.remember());
}

void Tst_sshdialogs::hostKeyRememberAndOnce()
{
    {
        HostKeyDialog dialog(hostKeyInfo(HostKeyStatus::Unknown, tempPath(QStringLiteral("known_hosts"))));
        QVERIFY(expose(&dialog));
        QTest::mouseClick(child<QPushButton>(&dialog, "buttonRemember"), Qt::LeftButton);
        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
        QVERIFY(!dialog.isVisible());
        QVERIFY(dialog.remember());
    }
    {
        HostKeyDialog dialog(hostKeyInfo(HostKeyStatus::Unknown, tempPath(QStringLiteral("known_hosts"))));
        QVERIFY(expose(&dialog));
        QTest::mouseClick(child<QPushButton>(&dialog, "buttonOnce"), Qt::LeftButton);
        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
        QVERIFY(!dialog.remember());
    }
    {
        // Enter picks the default button ("Connect and remember") like the OpenSSH prompt's yes.
        HostKeyDialog dialog(hostKeyInfo(HostKeyStatus::Unknown, tempPath(QStringLiteral("known_hosts"))));
        QVERIFY(expose(&dialog));
        QTest::keyClick(&dialog, Qt::Key_Return);
        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
        QVERIFY(dialog.remember());
    }
}

void Tst_sshdialogs::hostKeyChangedVariant()
{
    HostKeyDialog dialog(hostKeyInfo(HostKeyStatus::Changed, tempPath(QStringLiteral("known_hosts"))));
    QVERIFY(expose(&dialog));
    auto* headline = child<QLabel>(&dialog, "labelHeadline");
    auto* remember = child<QPushButton>(&dialog, "buttonRemember");
    auto* once = child<QPushButton>(&dialog, "buttonOnce");
    auto* cancel = child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Cancel);
    auto* understand = child<QCheckBox>(&dialog, "checkUnderstand");
    QVERIFY(headline && remember && once && cancel && understand);

    QCOMPARE(headline->text(), QStringLiteral("WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!"));
    QVERIFY(isReddish(headline->palette().color(QPalette::WindowText)));
    QVERIFY(child<QLabel>(&dialog, "labelExplanation")->text().contains(QStringLiteral("man-in-the-middle")));
    QCOMPARE(remember->text(), QStringLiteral("Replace key and connect"));
    QVERIFY(!remember->isEnabled());
    QVERIFY(once->isEnabled());
    QVERIFY(cancel->isDefault());
    QVERIFY(!remember->isDefault());
    QVERIFY(understand->isVisible());
    QVERIFY(!understand->isChecked());

    // Enter = Cancel here.
    QTest::keyClick(&dialog, Qt::Key_Return);
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Rejected));
    QVERIFY(!dialog.remember());

    // The checkbox unlocks the replace button.
    QVERIFY(expose(&dialog));
    understand->click();
    QVERIFY(remember->isEnabled());
    understand->click();
    QVERIFY(!remember->isEnabled());
    understand->click();
    QTest::mouseClick(remember, Qt::LeftButton);
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
    QVERIFY(dialog.remember());

    // "Connect once" needs no checkbox.
    HostKeyDialog onceDialog(hostKeyInfo(HostKeyStatus::Changed, tempPath(QStringLiteral("known_hosts"))));
    QVERIFY(expose(&onceDialog));
    QTest::mouseClick(child<QPushButton>(&onceDialog, "buttonOnce"), Qt::LeftButton);
    QCOMPARE(onceDialog.result(), static_cast<int>(QDialog::Accepted));
    QVERIFY(!onceDialog.remember());
}

void Tst_sshdialogs::hostKeyCancelRejects()
{
    {
        HostKeyDialog dialog(hostKeyInfo(HostKeyStatus::Unknown, tempPath(QStringLiteral("known_hosts"))));
        QVERIFY(expose(&dialog));
        auto* cancel = child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Cancel);
        QTest::mouseClick(cancel, Qt::LeftButton);
        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Rejected));
        QVERIFY(!dialog.isVisible());
        QVERIFY(!dialog.remember());
    }
    {
        HostKeyDialog dialog(hostKeyInfo(HostKeyStatus::Changed, tempPath(QStringLiteral("known_hosts"))));
        QVERIFY(expose(&dialog));
        QTest::keyClick(&dialog, Qt::Key_Escape);
        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Rejected));
    }
    {
        // Modal use: closed from a timer while exec() runs.
        HostKeyDialog dialog(hostKeyInfo(HostKeyStatus::Unknown, tempPath(QStringLiteral("known_hosts"))));
        QTimer::singleShot(0, &dialog, [&dialog]() {
            QTest::mouseClick(child<QPushButton>(&dialog, "buttonRemember"), Qt::LeftButton);
        });
        QCOMPARE(dialog.exec(), static_cast<int>(QDialog::Accepted));
        QVERIFY(dialog.remember());
    }
}

void Tst_sshdialogs::hostKeyFingerprintFields()
{
    SshConnection::HostKeyInfo info = hostKeyInfo(HostKeyStatus::Unknown, QString());   // no file -> the default path
    HostKeyDialog dialog(info);
    QVERIFY(expose(&dialog));
    auto* sha = child<QLineEdit>(&dialog, "editFingerprintSha256");
    auto* md5 = child<QLineEdit>(&dialog, "editFingerprintMd5");
    QVERIFY(sha && md5);
    QCOMPARE(sha->text(), info.fingerprintSha256);
    QCOMPARE(md5->text(), info.fingerprintMd5);
    QVERIFY(sha->isReadOnly());
    QVERIFY(md5->isReadOnly());
    QVERIFY(md5->isVisible());
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    QCOMPARE(sha->font().family(), mono.family());
    QCOMPARE(md5->font().family(), mono.family());
    QVERIFY(child<QLabel>(&dialog, "labelKnownHosts")->text().contains(QStringLiteral("known_hosts")));

    // Selectable: select all and read it back.
    sha->selectAll();
    QCOMPARE(sha->selectedText(), info.fingerprintSha256);

    // No MD5 fingerprint: the row is hidden.
    info.fingerprintMd5.clear();
    HostKeyDialog noMd5(info);
    QVERIFY(expose(&noMd5));
    QVERIFY(!child<QLineEdit>(&noMd5, "editFingerprintMd5")->isVisible());
    QVERIFY(child<QLineEdit>(&noMd5, "editFingerprintSha256")->isVisible());
}

// =======================================================================================
// AuthPromptDialog
// =======================================================================================

void Tst_sshdialogs::authMaskedAndShowToggle()
{
    AuthPromptDialog dialog(authPrompt());
    QVERIFY(expose(&dialog));
    auto* edit = child<QLineEdit>(&dialog, "editResponse");
    auto* show = child<QToolButton>(&dialog, "buttonShow");
    QVERIFY(edit && show);
    QCOMPARE(dialog.windowTitle(), QStringLiteral("Password"));
    QCOMPARE(child<QLabel>(&dialog, "labelTarget")->text(), QStringLiteral("root@192.168.100.2"));
    QCOMPARE(child<QLabel>(&dialog, "labelPrompt")->text(), QStringLiteral("Password:"));
    QVERIFY(!child<QLabel>(&dialog, "labelInstruction")->isVisible());
    QCOMPARE(dialog.focusWidget(), edit);

    QCOMPARE(edit->echoMode(), QLineEdit::Password);
    QVERIFY(show->isVisible());
    QTest::mouseClick(show, Qt::LeftButton);
    QCOMPARE(edit->echoMode(), QLineEdit::Normal);
    QCOMPARE(show->text(), QStringLiteral("Hide"));
    QTest::mouseClick(show, Qt::LeftButton);
    QCOMPARE(edit->echoMode(), QLineEdit::Password);
    QCOMPARE(show->text(), QStringLiteral("Show"));

    QTest::keyClicks(edit, QStringLiteral("hunter2"));
    QCOMPARE(dialog.response(), QStringLiteral("hunter2"));
    QVERIFY(!dialog.remember());
}

void Tst_sshdialogs::authEchoPrompt()
{
    SshConnection::AuthPrompt prompt = authPrompt(PromptKind::KeyboardInteractive);
    prompt.title = QStringLiteral("Two-factor login");
    prompt.instruction = QStringLiteral("Enter the code from your token.");
    prompt.prompt = QStringLiteral("Token: ");
    prompt.echo = true;
    AuthPromptDialog dialog(prompt);
    QVERIFY(expose(&dialog));
    QCOMPARE(dialog.windowTitle(), QStringLiteral("Two-factor login"));
    QCOMPARE(child<QLineEdit>(&dialog, "editResponse")->echoMode(), QLineEdit::Normal);
    QVERIFY(!child<QToolButton>(&dialog, "buttonShow")->isVisible());
    auto* instruction = child<QLabel>(&dialog, "labelInstruction");
    QVERIFY(instruction->isVisible());
    QCOMPARE(instruction->text(), QStringLiteral("Enter the code from your token."));
    QCOMPARE(child<QLabel>(&dialog, "labelPrompt")->text(), QStringLiteral("Token:"));

    // A passphrase prompt names the key file when no prompt text is given.
    SshConnection::AuthPrompt passphrase = authPrompt(PromptKind::Passphrase);
    passphrase.prompt.clear();
    passphrase.keyFile = QStringLiteral("/home/moses/.ssh/id_ed25519");
    AuthPromptDialog keyDialog(passphrase);
    QCOMPARE(keyDialog.windowTitle(), QStringLiteral("Key Passphrase"));
    QVERIFY(child<QLabel>(&keyDialog, "labelPrompt")->text().contains(QStringLiteral("id_ed25519")));
}

void Tst_sshdialogs::authRememberVisibility()
{
    {
        SshConnection::AuthPrompt prompt = authPrompt();
        prompt.canRemember = false;   // the connection has nowhere to store the answer
        AuthPromptDialog dialog(prompt);
        QVERIFY(expose(&dialog));
        QVERIFY(!child<QCheckBox>(&dialog, "checkRemember")->isVisible());
        QVERIFY(!child<QLabel>(&dialog, "labelNote")->isVisible());
        QVERIFY(!dialog.remember());
    }
    {
        AuthPromptDialog dialog(authPrompt());   // canRemember, no rememberTarget: the headline target
        QVERIFY(expose(&dialog));
        auto* remember = child<QCheckBox>(&dialog, "checkRemember");
        auto* note = child<QLabel>(&dialog, "labelNote");
        QVERIFY(remember->isVisible());
        QCOMPARE(remember->text(), QStringLiteral("Remember password for root@192.168.100.2"));
        const QString description = SecretStore::storageDescription();
        QCOMPARE(remember->toolTip(), description);
        QCOMPARE(note->isVisible(), !description.isEmpty());
        QCOMPARE(note->text(), description);
        QVERIFY(!dialog.remember());
        remember->click();
        QVERIFY(dialog.remember());
    }
}

void Tst_sshdialogs::authRememberLabels_data()
{
    QTest::addColumn<int>("kind");
    QTest::addColumn<QString>("rememberTarget");
    QTest::addColumn<QString>("keyFile");
    QTest::addColumn<QString>("expected");

    // v0.4: every password / passphrase prompt can be remembered; the caption names the target
    // ("user@host:port" for an ad-hoc connection) or the key file the answer is stored for.
    QTest::newRow("password, ad-hoc target") << static_cast<int>(PromptKind::Password)
                                             << QStringLiteral("root@192.168.100.2:22") << QString()
                                             << QStringLiteral("Remember password for root@192.168.100.2:22");
    QTest::newRow("password, custom port") << static_cast<int>(PromptKind::Password)
                                           << QStringLiteral("moses@10.0.0.24:2200") << QString()
                                           << QStringLiteral("Remember password for moses@10.0.0.24:2200");
    QTest::newRow("password, no target given") << static_cast<int>(PromptKind::Password) << QString() << QString()
                                               << QStringLiteral("Remember password for root@192.168.100.2");
    QTest::newRow("passphrase, key file target") << static_cast<int>(PromptKind::Passphrase)
                                                 << QStringLiteral("/home/moses/.ssh/id_ed25519")
                                                 << QStringLiteral("/home/moses/.ssh/id_ed25519")
                                                 << QStringLiteral("Remember passphrase for id_ed25519");
    QTest::newRow("passphrase, from keyFile") << static_cast<int>(PromptKind::Passphrase) << QString()
                                              << QStringLiteral("C:/Users/moses/.ssh/board_rsa")
                                              << QStringLiteral("Remember passphrase for board_rsa");
    QTest::newRow("keyboard-interactive") << static_cast<int>(PromptKind::KeyboardInteractive)
                                          << QStringLiteral("pi@10.0.0.5:2222") << QString()
                                          << QStringLiteral("Remember answer for pi@10.0.0.5:2222");
}

void Tst_sshdialogs::authRememberLabels()
{
    QFETCH(int, kind);
    QFETCH(QString, rememberTarget);
    QFETCH(QString, keyFile);
    QFETCH(QString, expected);

    SshConnection::AuthPrompt prompt = authPrompt(static_cast<PromptKind>(kind));
    prompt.canRemember = true;
    prompt.rememberTarget = rememberTarget;
    prompt.keyFile = keyFile;
    AuthPromptDialog dialog(prompt);
    QVERIFY(expose(&dialog));
    auto* remember = child<QCheckBox>(&dialog, "checkRemember");
    auto* note = child<QLabel>(&dialog, "labelNote");
    QVERIFY(remember && note);
    QVERIFY(remember->isVisible());
    QCOMPARE(remember->text(), expected);
    QVERIFY(!remember->isChecked());   // never pre-checked
    QVERIFY(!dialog.remember());
    const QString description = SecretStore::storageDescription();
    QCOMPARE(remember->toolTip(), description);
    QCOMPARE(note->isVisible(), !description.isEmpty());
    QCOMPARE(note->text(), description);
    remember->click();
    QVERIFY(dialog.remember());

    // A retry keeps the caption and the unchecked default.
    prompt.attempt = 2;
    AuthPromptDialog retry(prompt);
    QVERIFY(expose(&retry));
    QCOMPARE(child<QCheckBox>(&retry, "checkRemember")->text(), expected);
    QVERIFY(child<QCheckBox>(&retry, "checkRemember")->isVisible());
    QVERIFY(!retry.remember());
}

void Tst_sshdialogs::authAttemptLabel()
{
    {
        AuthPromptDialog dialog(authPrompt());
        QVERIFY(expose(&dialog));
        QVERIFY(!child<QLabel>(&dialog, "labelAttempt")->isVisible());
    }
    {
        SshConnection::AuthPrompt prompt = authPrompt();
        prompt.attempt = 2;
        AuthPromptDialog dialog(prompt);
        QVERIFY(expose(&dialog));
        auto* attempt = child<QLabel>(&dialog, "labelAttempt");
        QVERIFY(attempt->isVisible());
        QCOMPARE(attempt->text(), QStringLiteral("Authentication failed, try again (attempt 2 of 3)"));
        QVERIFY(isReddish(attempt->palette().color(QPalette::WindowText)));
    }
}

void Tst_sshdialogs::authEnterAccepts()
{
    AuthPromptDialog dialog(authPrompt());
    QWidget* focused = nullptr;
    QTimer::singleShot(0, &dialog, [&dialog, &focused]() {
        focused = dialog.focusWidget();
        auto* edit = child<QLineEdit>(&dialog, "editResponse");
        QTest::keyClicks(edit, QStringLiteral("s3cret"));
        QTest::keyClick(edit, Qt::Key_Return);
    });
    QCOMPARE(dialog.exec(), static_cast<int>(QDialog::Accepted));
    QCOMPARE(dialog.response(), QStringLiteral("s3cret"));
    QCOMPARE(focused, child<QLineEdit>(&dialog, "editResponse"));

    // The OK button works too.
    AuthPromptDialog okDialog(authPrompt());
    QVERIFY(expose(&okDialog));
    QTest::keyClicks(child<QLineEdit>(&okDialog, "editResponse"), QStringLiteral("pw"));
    child<QDialogButtonBox>(&okDialog, "buttonBox")->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(okDialog.result(), static_cast<int>(QDialog::Accepted));
    QCOMPARE(okDialog.response(), QStringLiteral("pw"));
}

void Tst_sshdialogs::authEscapeRejects()
{
    AuthPromptDialog dialog(authPrompt());
    QVERIFY(expose(&dialog));
    QTest::keyClicks(child<QLineEdit>(&dialog, "editResponse"), QStringLiteral("typed"));
    QTest::keyClick(&dialog, Qt::Key_Escape);
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Rejected));
    QVERIFY(!dialog.isVisible());

    AuthPromptDialog modal(authPrompt());
    QTimer::singleShot(0, &modal, [&modal]() {
        child<QDialogButtonBox>(&modal, "buttonBox")->button(QDialogButtonBox::Cancel)->click();
    });
    QCOMPARE(modal.exec(), static_cast<int>(QDialog::Rejected));
}

// =======================================================================================
// SshProfilesDialog
// =======================================================================================

void Tst_sshdialogs::profilesLoadsStore()
{
    auto store = newStore();
    QVERIFY(store);
    SshProfilesDialog dialog(store.get());
    QVERIFY(expose(&dialog));
    auto* list = child<QListWidget>(&dialog, "listProfiles");
    QVERIFY(list);
    QCOMPARE(list->count(), 2);
    QCOMPARE(list->item(0)->text(), QStringLiteral("Kali box"));
    QCOMPARE(list->item(1)->text(), QStringLiteral("Luckfox Pico"));
    QCOMPARE(dialog.selectedProfileId(), kKaliId);   // the first one is selected

    QCOMPARE(child<QLineEdit>(&dialog, "editName")->text(), QStringLiteral("Kali box"));
    QCOMPARE(child<QLineEdit>(&dialog, "editHost")->text(), QStringLiteral("10.0.0.24"));
    QCOMPARE(child<QSpinBox>(&dialog, "spinPort")->value(), 2200);
    QCOMPARE(child<QLineEdit>(&dialog, "editUser")->text(), QStringLiteral("moses"));
    auto* auth = child<QComboBox>(&dialog, "comboAuth");
    QCOMPARE(auth->count(), 5);
    QCOMPARE(auth->currentData().toInt(), static_cast<int>(SshProfile::Auth::Password));
    QCOMPARE(child<QSpinBox>(&dialog, "spinKeepAlive")->value(), 30);
    QCOMPARE(child<QSpinBox>(&dialog, "spinTimeout")->value(), 15);
    QCOMPARE(child<QLineEdit>(&dialog, "editTerminalType")->text(), QStringLiteral("xterm-256color"));
    QVERIFY(child<QLineEdit>(&dialog, "editPassword")->isEnabled() == false);   // "Save password" is off
    QCOMPARE(child<QTableWidget>(&dialog, "tableForwards")->rowCount(), 0);
    QVERIFY(!child<QLabel>(&dialog, "labelValidation")->isVisible());
    QVERIFY(child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Ok)->isEnabled());
    QVERIFY(child<QPushButton>(&dialog, "buttonConnect")->isEnabled());
    dialog.resize(800, 600);   // usable at 800x600: the layout must not force a bigger window
    QVERIFY2(dialog.width() <= 800 && dialog.height() <= 600,
             qPrintable(QStringLiteral("%1x%2").arg(dialog.width()).arg(dialog.height())));

    dialog.selectProfile(kLuckfoxId);
    QCOMPARE(dialog.selectedProfileId(), kLuckfoxId);
    QCOMPARE(list->currentRow(), 1);
    QCOMPARE(child<QLineEdit>(&dialog, "editHost")->text(), QStringLiteral("192.168.100.2"));
    QCOMPARE(child<QSpinBox>(&dialog, "spinPort")->value(), 22);
    QCOMPARE(child<QPlainTextEdit>(&dialog, "editDescription")->toPlainText(),
             QStringLiteral("RV1106 over the direct cable"));
    QCOMPARE(auth->currentData().toInt(), static_cast<int>(SshProfile::Auth::Auto));

    // An empty store: the form is disabled, OK still works (nothing to validate).
    SshProfileStore empty;
    QVERIFY(empty.load(tempPath(QStringLiteral("missing.json"))));
    SshProfilesDialog emptyDialog(&empty);
    QVERIFY(expose(&emptyDialog));
    QVERIFY(emptyDialog.selectedProfileId().isEmpty());
    QVERIFY(!child<QTabWidget>(&emptyDialog, "tabs")->isEnabled());
    QVERIFY(!child<QPushButton>(&emptyDialog, "buttonConnect")->isEnabled());
    QVERIFY(!child<QPushButton>(&emptyDialog, "buttonDelete")->isEnabled());
    QVERIFY(child<QDialogButtonBox>(&emptyDialog, "buttonBox")->button(QDialogButtonBox::Ok)->isEnabled());
}

void Tst_sshdialogs::profilesNewDuplicateDelete()
{
    auto store = newStore();
    QVERIFY(store);
    SshProfilesDialog dialog(store.get());
    QVERIFY(expose(&dialog));
    auto* list = child<QListWidget>(&dialog, "listProfiles");
    auto* name = child<QLineEdit>(&dialog, "editName");
    auto* host = child<QLineEdit>(&dialog, "editHost");
    auto* ok = child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Ok);

    // New: selected, named, invalid until a host is typed.
    child<QPushButton>(&dialog, "buttonNew")->click();
    QCOMPARE(list->count(), 3);
    const QString newId = dialog.selectedProfileId();
    QVERIFY(!newId.isEmpty());
    QVERIFY(newId != kKaliId && newId != kLuckfoxId);
    QCOMPARE(name->text(), QStringLiteral("New profile"));
    QCOMPARE(list->currentItem()->text(), QStringLiteral("New profile"));
    QVERIFY(host->text().isEmpty());
    QVERIFY(!ok->isEnabled());
    QTest::keyClicks(host, QStringLiteral("newhost"));
    QVERIFY(ok->isEnabled());

    // A second New gets a distinct name.
    child<QPushButton>(&dialog, "buttonNew")->click();
    QCOMPARE(list->count(), 4);
    QCOMPARE(name->text(), QStringLiteral("New profile 2"));
    child<QPushButton>(&dialog, "buttonDelete")->click();
    QCOMPARE(list->count(), 3);

    // Duplicate copies the fields under a new id.
    dialog.selectProfile(newId);
    child<QPushButton>(&dialog, "buttonDuplicate")->click();
    QCOMPARE(list->count(), 4);
    const QString dupId = dialog.selectedProfileId();
    QVERIFY(dupId != newId);
    QCOMPARE(name->text(), QStringLiteral("New profile (copy)"));
    QCOMPARE(host->text(), QStringLiteral("newhost"));

    // Delete selects a neighbour; the store is untouched until Apply/OK.
    child<QPushButton>(&dialog, "buttonDelete")->click();
    QCOMPARE(list->count(), 3);
    QVERIFY(dialog.selectedProfileId() != dupId);
    QVERIFY(!dialog.selectedProfileId().isEmpty());
    QCOMPARE(store->profiles().size(), 2);

    // Delete everything: the form goes blank and disabled.
    child<QPushButton>(&dialog, "buttonDelete")->click();
    child<QPushButton>(&dialog, "buttonDelete")->click();
    child<QPushButton>(&dialog, "buttonDelete")->click();
    QCOMPARE(list->count(), 0);
    QVERIFY(dialog.selectedProfileId().isEmpty());
    QVERIFY(!child<QTabWidget>(&dialog, "tabs")->isEnabled());
    QVERIFY(ok->isEnabled());
    ok->click();
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
    QVERIFY(store->profiles().isEmpty());
}

void Tst_sshdialogs::profilesEditingUpdatesWorkingCopyAndList()
{
    auto store = newStore();
    QVERIFY(store);
    SshProfilesDialog dialog(store.get());
    QVERIFY(expose(&dialog));
    auto* list = child<QListWidget>(&dialog, "listProfiles");
    auto* name = child<QLineEdit>(&dialog, "editName");
    auto* host = child<QLineEdit>(&dialog, "editHost");
    auto* user = child<QLineEdit>(&dialog, "editUser");
    dialog.selectProfile(kLuckfoxId);

    name->clear();
    QTest::keyClicks(name, QStringLiteral("Pico Ultra"));
    QCOMPARE(list->currentItem()->text(), QStringLiteral("Pico Ultra"));   // label follows while typing
    QCOMPARE(store->profile(kLuckfoxId)->name, QStringLiteral("Luckfox Pico"));   // store untouched

    host->clear();
    QTest::keyClicks(host, QStringLiteral("192.168.100.9"));
    user->clear();
    QTest::keyClicks(user, QStringLiteral("pico"));
    child<QSpinBox>(&dialog, "spinPort")->setValue(2222);
    child<QCheckBox>(&dialog, "checkCompression")->click();
    child<QPlainTextEdit>(&dialog, "editDescription")->setPlainText(QStringLiteral("edited"));

    // Switching profiles and back keeps the edits (they live in the working copy).
    dialog.selectProfile(kKaliId);
    QCOMPARE(host->text(), QStringLiteral("10.0.0.24"));
    dialog.selectProfile(kLuckfoxId);
    QCOMPARE(name->text(), QStringLiteral("Pico Ultra"));
    QCOMPARE(host->text(), QStringLiteral("192.168.100.9"));
    QCOMPARE(user->text(), QStringLiteral("pico"));
    QCOMPARE(child<QSpinBox>(&dialog, "spinPort")->value(), 2222);
    QVERIFY(child<QCheckBox>(&dialog, "checkCompression")->isChecked());
    // Re-sorted by the new name: "Kali box" < "Pico Ultra".
    QCOMPARE(list->item(0)->text(), QStringLiteral("Kali box"));
    QCOMPARE(list->item(1)->text(), QStringLiteral("Pico Ultra"));

    // Rename it ahead of "Kali box": the list re-sorts while typing, the selection follows.
    name->clear();
    QTest::keyClicks(name, QStringLiteral("Aardvark"));
    QCOMPARE(list->item(0)->text(), QStringLiteral("Aardvark"));
    QCOMPARE(list->currentRow(), 0);
    QCOMPARE(dialog.selectedProfileId(), kLuckfoxId);
    QCOMPARE(name->text(), QStringLiteral("Aardvark"));   // the form is not reloaded under the typist

    child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Apply)->click();
    const std::optional<SshProfile> stored = store->profile(kLuckfoxId);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->name, QStringLiteral("Aardvark"));
    QCOMPARE(stored->host, QStringLiteral("192.168.100.9"));
    QCOMPARE(stored->user, QStringLiteral("pico"));
    QCOMPARE(stored->port, quint16(2222));
    QVERIFY(stored->compression);
    QCOMPARE(stored->description, QStringLiteral("edited"));
    QVERIFY(dialog.isVisible());   // Apply keeps the dialog open
}

void Tst_sshdialogs::profilesValidationEmptyHost()
{
    auto store = newStore();
    QVERIFY(store);
    SshProfilesDialog dialog(store.get());
    QVERIFY(expose(&dialog));
    auto* host = child<QLineEdit>(&dialog, "editHost");
    auto* buttons = child<QDialogButtonBox>(&dialog, "buttonBox");
    auto* connectButton = child<QPushButton>(&dialog, "buttonConnect");
    auto* warning = child<QLabel>(&dialog, "labelValidation");
    const QColor normalBase = QLineEdit().palette().color(QPalette::Base);

    host->clear();
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QVERIFY(!buttons->button(QDialogButtonBox::Apply)->isEnabled());
    QVERIFY(!connectButton->isEnabled());
    QVERIFY(warning->isVisible());
    QVERIFY(warning->text().contains(QStringLiteral("Host")));
    QVERIFY(host->palette().color(QPalette::Base) != normalBase);

    // The other profile's problems block OK too, and are named.
    dialog.selectProfile(kLuckfoxId);
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QVERIFY(warning->text().contains(QStringLiteral("Kali box")));
    dialog.selectProfile(kKaliId);

    QTest::keyClicks(host, QStringLiteral("10.0.0.1"));
    QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());
    QVERIFY(connectButton->isEnabled());
    QVERIFY(!warning->isVisible());
    QCOMPARE(host->palette().color(QPalette::Base), normalBase);

    // A proxy jump must parse.
    auto* jump = child<QLineEdit>(&dialog, "editProxyJump");
    QTest::keyClicks(jump, QStringLiteral("not a host!"));
    QVERIFY(!buttons->button(QDialogButtonBox::Ok)->isEnabled());
    jump->clear();
    QTest::keyClicks(jump, QStringLiteral("moses@gateway:2222"));
    QVERIFY(buttons->button(QDialogButtonBox::Ok)->isEnabled());

    // accept() refuses while invalid.
    host->clear();
    dialog.accept();
    QVERIFY(dialog.isVisible());
}

void Tst_sshdialogs::profilesValidationPublicKeyFile()
{
    auto store = newStore();
    QVERIFY(store);
    SshProfilesDialog dialog(store.get());
    QVERIFY(expose(&dialog));
    dialog.selectProfile(kLuckfoxId);
    auto* auth = child<QComboBox>(&dialog, "comboAuth");
    auto* identity = child<QLineEdit>(&dialog, "editIdentityFile");
    auto* ok = child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Ok);
    auto* warning = child<QLabel>(&dialog, "labelValidation");
    auto* savePassword = child<QCheckBox>(&dialog, "checkSavePassword");

    auth->setCurrentIndex(auth->findData(static_cast<int>(SshProfile::Auth::PublicKey)));
    QVERIFY(!ok->isEnabled());
    QVERIFY(warning->text().contains(QStringLiteral("identity"), Qt::CaseInsensitive));
    QVERIFY(savePassword->text().contains(QStringLiteral("passphrase")));

    identity->setText(QDir::toNativeSeparators(tempPath(QStringLiteral("missing_key"))));
    QVERIFY(!ok->isEnabled());
    QVERIFY(warning->text().contains(QStringLiteral("not found")));

    const QString keyPath = tempPath(QStringLiteral("id_test"));
    QVERIFY(writeFile(keyPath, "-----BEGIN OPENSSH PRIVATE KEY-----\nnot really\n-----END OPENSSH PRIVATE KEY-----\n"));
    identity->setText(QDir::toNativeSeparators(keyPath));
    QVERIFY(ok->isEnabled());
    QVERIFY(!warning->isVisible());

    // With Auto the file is optional, but a wrong path is still an error.
    auth->setCurrentIndex(auth->findData(static_cast<int>(SshProfile::Auth::Auto)));
    QVERIFY(ok->isEnabled());
    identity->setText(QDir::toNativeSeparators(tempPath(QStringLiteral("missing_key"))));
    QVERIFY(!ok->isEnabled());
    identity->clear();
    QVERIFY(ok->isEnabled());

    // Agent: no key file, no password.
    auth->setCurrentIndex(auth->findData(static_cast<int>(SshProfile::Auth::Agent)));
    QVERIFY(!identity->isEnabled());
    QVERIFY(!savePassword->isEnabled());
    QVERIFY(ok->isEnabled());

    ok->click();
    QCOMPARE(store->profile(kLuckfoxId)->auth, SshProfile::Auth::Agent);
}

void Tst_sshdialogs::profilesForwardsRoundTrip()
{
    auto store = newStore();
    QVERIFY(store);
    SshProfilesDialog dialog(store.get());
    QVERIFY(expose(&dialog));
    dialog.selectProfile(kLuckfoxId);
    auto* table = child<QTableWidget>(&dialog, "tableForwards");
    auto* add = child<QPushButton>(&dialog, "buttonAddForward");
    auto* remove = child<QPushButton>(&dialog, "buttonRemoveForward");
    auto* apply = child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Apply);
    QVERIFY(table && add && remove && apply);
    QCOMPARE(table->columnCount(), 4);
    QCOMPARE(table->rowCount(), 0);
    QVERIFY(!remove->isEnabled());

    add->click();
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 0)->data(Qt::EditRole).toInt(), 8080);
    QCOMPARE(table->item(0, 1)->text(), QStringLiteral("127.0.0.1"));
    QCOMPARE(table->item(0, 2)->data(Qt::EditRole).toInt(), 80);
    QCOMPARE(table->item(0, 3)->text(), QStringLiteral("127.0.0.1"));
    QVERIFY(remove->isEnabled());

    // Edit the cells (what the delegates' editors write) and add a second row.
    table->item(0, 0)->setData(Qt::EditRole, 9090);
    table->item(0, 1)->setText(QStringLiteral("10.0.0.5"));
    table->item(0, 2)->setData(Qt::EditRole, 443);
    table->item(0, 3)->setText(QStringLiteral("0.0.0.0"));
    add->click();
    QCOMPARE(table->rowCount(), 2);

    apply->click();
    std::optional<SshProfile> stored = store->profile(kLuckfoxId);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->localForwards.size(), 2);
    QCOMPARE(stored->localForwards.at(0).localPort, quint16(9090));
    QCOMPARE(stored->localForwards.at(0).remoteHost, QStringLiteral("10.0.0.5"));
    QCOMPARE(stored->localForwards.at(0).remotePort, quint16(443));
    QCOMPARE(stored->localForwards.at(0).bindAddress, QStringLiteral("0.0.0.0"));
    QCOMPARE(stored->localForwards.at(0).displayText(), QStringLiteral("0.0.0.0:9090 -> 10.0.0.5:443"));
    QCOMPARE(stored->localForwards.at(1).displayText(), QStringLiteral("8080 -> 127.0.0.1:80"));

    // Reload shows both rows; remove the first; Apply persists one.
    dialog.selectProfile(kKaliId);
    dialog.selectProfile(kLuckfoxId);
    QCOMPARE(table->rowCount(), 2);
    table->setCurrentCell(0, 0);
    remove->click();
    QCOMPARE(table->rowCount(), 1);
    apply->click();
    stored = store->profile(kLuckfoxId);
    QCOMPARE(stored->localForwards.size(), 1);
    QCOMPARE(stored->localForwards.at(0).localPort, quint16(8080));
}

void Tst_sshdialogs::profilesApplyWritesStoreAndSaves()
{
    auto store = newStore();
    QVERIFY(store);
    const QString savedPath = SshProfileStore::defaultFilePath();
    QFile::remove(savedPath);
    QVERIFY(!QFileInfo::exists(savedPath));
    QSignalSpy changed(store.get(), &SshProfileStore::changed);

    SshProfilesDialog dialog(store.get());
    QVERIFY(expose(&dialog));
    auto* host = child<QLineEdit>(&dialog, "editHost");
    host->clear();
    QTest::keyClicks(host, QStringLiteral("10.0.0.42"));
    QCOMPARE(changed.count(), 0);

    child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Apply)->click();
    QVERIFY(changed.count() >= 1);
    QCOMPARE(store->profile(kKaliId)->host, QStringLiteral("10.0.0.42"));
    QVERIFY(dialog.isVisible());
    QVERIFY2(QFileInfo::exists(savedPath), qPrintable(savedPath));

    SshProfileStore reloaded;
    QVERIFY(reloaded.load(savedPath));
    QCOMPARE(reloaded.profiles().size(), 2);
    QCOMPARE(reloaded.profile(kKaliId)->host, QStringLiteral("10.0.0.42"));
    QCOMPARE(reloaded.recentTargets(), QStringList({kRecentPi, kRecentRoot}));

    // Nothing secret in the file (the handle is closed again before the next save replaces it).
    const QByteArray json = readFile(savedPath);
    QVERIFY(!json.isEmpty());
    QVERIFY(!json.contains("password\":"));   // only the passwordSaved flag exists
    QVERIFY(json.contains("\"passwordSaved\": false"));

    // OK applies, saves and closes.
    host->clear();
    QTest::keyClicks(host, QStringLiteral("10.0.0.43"));
    child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
    QVERIFY(!dialog.isVisible());
    QCOMPARE(store->profile(kKaliId)->host, QStringLiteral("10.0.0.43"));
    SshProfileStore reloadedAgain;
    QVERIFY(reloadedAgain.load(savedPath));
    QCOMPARE(reloadedAgain.profile(kKaliId)->host, QStringLiteral("10.0.0.43"));
}

void Tst_sshdialogs::profilesConnectEmitsEditedProfile()
{
    auto store = newStore();
    QVERIFY(store);
    SshProfilesDialog dialog(store.get());
    QVERIFY(expose(&dialog));
    dialog.selectProfile(kLuckfoxId);
    QList<SshProfile> requested;
    connect(&dialog, &SshProfilesDialog::connectRequested, this,
            [&requested](const SshProfile& p) { requested.append(p); });

    auto* host = child<QLineEdit>(&dialog, "editHost");
    host->clear();
    QTest::keyClicks(host, QStringLiteral("10.0.0.99"));
    QTest::mouseClick(child<QPushButton>(&dialog, "buttonConnect"), Qt::LeftButton);

    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.first().id, kLuckfoxId);
    QCOMPARE(requested.first().host, QStringLiteral("10.0.0.99"));
    QCOMPARE(requested.first().name, QStringLiteral("Luckfox Pico"));
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
    QVERIFY(!dialog.isVisible());
    QCOMPARE(store->profile(kLuckfoxId)->host, QStringLiteral("10.0.0.99"));   // saved first

    // Connect is refused while the form is invalid.
    SshProfilesDialog second(store.get());
    QVERIFY(expose(&second));
    QList<SshProfile> none;
    connect(&second, &SshProfilesDialog::connectRequested, this, [&none](const SshProfile& p) { none.append(p); });
    child<QLineEdit>(&second, "editHost")->clear();
    QVERIFY(!child<QPushButton>(&second, "buttonConnect")->isEnabled());
    QTest::mouseClick(child<QPushButton>(&second, "buttonConnect"), Qt::LeftButton);
    QVERIFY(none.isEmpty());
    QVERIFY(second.isVisible());
}

void Tst_sshdialogs::profilesCancelDiscards()
{
    auto store = newStore();
    QVERIFY(store);
    const QList<SshProfile> original = store->profiles();
    QSignalSpy changed(store.get(), &SshProfileStore::changed);
    const QString savedPath = SshProfileStore::defaultFilePath();

    SshProfilesDialog dialog(store.get());
    QVERIFY(expose(&dialog));
    auto* host = child<QLineEdit>(&dialog, "editHost");
    host->clear();
    QTest::keyClicks(host, QStringLiteral("changed.example"));
    child<QPushButton>(&dialog, "buttonNew")->click();
    QTest::keyClicks(child<QLineEdit>(&dialog, "editHost"), QStringLiteral("another"));
    dialog.selectProfile(kLuckfoxId);
    child<QPushButton>(&dialog, "buttonDelete")->click();
    QCOMPARE(child<QListWidget>(&dialog, "listProfiles")->count(), 2);

    child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Cancel)->click();
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Rejected));
    QVERIFY(!dialog.isVisible());
    QCOMPARE(changed.count(), 0);
    QVERIFY(store->profiles() == original);
    QVERIFY(!QFileInfo::exists(savedPath));

    // Escape cancels as well.
    SshProfilesDialog second(store.get());
    QVERIFY(expose(&second));
    child<QLineEdit>(&second, "editHost")->clear();
    QTest::keyClick(&second, Qt::Key_Escape);
    QCOMPARE(second.result(), static_cast<int>(QDialog::Rejected));
    QVERIFY(store->profiles() == original);
}

void Tst_sshdialogs::profilesImportExportRoundTrip()
{
    auto store = newStore();
    QVERIFY(store);
    const QString exportPath = tempPath(QStringLiteral("export/profiles.json"));
    QVERIFY(QDir().mkpath(QFileInfo(exportPath).absolutePath()));

    SshProfilesDialog dialog(store.get());
    QVERIFY(expose(&dialog));
    // Unsaved edits are part of the export (it writes the working copy).
    dialog.selectProfile(kLuckfoxId);
    auto* user = child<QLineEdit>(&dialog, "editUser");
    user->clear();
    QTest::keyClicks(user, QStringLiteral("exported"));
    bool ok = false;
    QVERIFY(QMetaObject::invokeMethod(&dialog, "exportFile", Q_RETURN_ARG(bool, ok), Q_ARG(QString, exportPath)));
    QVERIFY(ok);
    QVERIFY(QFileInfo::exists(exportPath));

    QFile file(exportPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    QVERIFY(doc.isObject());
    QCOMPARE(doc.object().value(QStringLiteral("profiles")).toArray().size(), 2);
    QVERIFY(store->profiles() == QList<SshProfile>({kaliProfile(), luckfoxProfile()}));   // store untouched

    // Import into an empty store through a second dialog and accept.
    SshProfileStore target;
    QVERIFY(target.load(tempPath(QStringLiteral("target-missing.json"))));
    SshProfilesDialog importer(&target);
    QVERIFY(expose(&importer));
    QVERIFY(QMetaObject::invokeMethod(&importer, "importFile", Q_RETURN_ARG(bool, ok), Q_ARG(QString, exportPath)));
    QVERIFY(ok);
    auto* list = child<QListWidget>(&importer, "listProfiles");
    QCOMPARE(list->count(), 2);
    QCOMPARE(list->item(0)->text(), QStringLiteral("Kali box"));
    QCOMPARE(list->item(1)->text(), QStringLiteral("Luckfox Pico"));
    child<QDialogButtonBox>(&importer, "buttonBox")->button(QDialogButtonBox::Ok)->click();
    QCOMPARE(importer.result(), static_cast<int>(QDialog::Accepted));

    QCOMPARE(target.profiles().size(), 2);
    const std::optional<SshProfile> luckfox = target.profile(kLuckfoxId);
    QVERIFY(luckfox.has_value());
    QCOMPARE(luckfox->user, QStringLiteral("exported"));
    QCOMPARE(luckfox->host, QStringLiteral("192.168.100.2"));
    QCOMPARE(luckfox->description, QStringLiteral("RV1106 over the direct cable"));
    QCOMPARE(target.profile(kKaliId)->port, quint16(2200));
    QCOMPARE(target.profile(kKaliId)->auth, SshProfile::Auth::Password);

    // Importing again merges by id (no duplicates).
    SshProfilesDialog again(&target);
    QVERIFY(expose(&again));
    QVERIFY(QMetaObject::invokeMethod(&again, "importFile", Q_RETURN_ARG(bool, ok), Q_ARG(QString, exportPath)));
    QVERIFY(ok);
    QCOMPARE(child<QListWidget>(&again, "listProfiles")->count(), 2);
    again.reject();
}

void Tst_sshdialogs::profilesPasswordSaveAndRemove()
{
    auto store = newStore();
    QVERIFY(store);
    const QString key = QStringLiteral("ssh/%1/password").arg(kKaliId);
    SecretStore::remove(key);

    SshProfilesDialog dialog(store.get());
    QVERIFY(expose(&dialog));
    dialog.selectProfile(kKaliId);   // Auth::Password
    auto* save = child<QCheckBox>(&dialog, "checkSavePassword");
    auto* password = child<QLineEdit>(&dialog, "editPassword");
    auto* apply = child<QDialogButtonBox>(&dialog, "buttonBox")->button(QDialogButtonBox::Apply);
    QVERIFY(save && password && apply);
    QCOMPARE(password->echoMode(), QLineEdit::Password);
    QVERIFY(!save->isChecked());
    QVERIFY(!password->isEnabled());
    QCOMPARE(child<QLabel>(&dialog, "labelSecretNote")->isVisible(), !SecretStore::storageDescription().isEmpty());

    save->click();
    QVERIFY(password->isEnabled());
    QTest::keyClicks(password, QStringLiteral("hunter2"));

    // The typed password survives a switch to another profile.
    dialog.selectProfile(kLuckfoxId);
    QVERIFY(!save->isChecked());
    QVERIFY(password->text().isEmpty());
    dialog.selectProfile(kKaliId);
    QVERIFY(save->isChecked());
    QCOMPARE(password->text(), QStringLiteral("hunter2"));

    apply->click();
    QVERIFY(store->profile(kKaliId)->passwordSaved);
    QVERIFY(password->text().isEmpty());
    QCOMPARE(password->placeholderText(), QStringLiteral("(saved)"));
    QVERIFY(save->isChecked());
    if (const std::optional<QString> stored = SecretStore::load(key)) {
        QCOMPARE(*stored, QStringLiteral("hunter2"));
    } else {
        qWarning() << "SecretStore did not return the stored password (stub or unavailable backend)";
    }

    // The profile file never carries the password.
    const QByteArray json = readFile(SshProfileStore::defaultFilePath());
    QVERIFY(!json.isEmpty());
    QVERIFY(!json.contains("hunter2"));
    QVERIFY(json.contains("\"passwordSaved\": true"));

    // Unchecking removes the secret on Apply.
    save->click();
    QVERIFY(!password->isEnabled());
    apply->click();
    QVERIFY(!store->profile(kKaliId)->passwordSaved);
    QVERIFY(!SecretStore::contains(key));
    QVERIFY(password->placeholderText() != QStringLiteral("(saved)"));
    QVERIFY(readFile(SshProfileStore::defaultFilePath()).contains("\"passwordSaved\": false"));
}

// =======================================================================================
// RemoteFileDialog
// =======================================================================================

void Tst_sshdialogs::remoteDirectionDefaults()
{
    SshConnection connection;   // never opened
    RemoteFileDialog dialog(&connection);
    QVERIFY(expose(&dialog));
    auto* upload = child<QRadioButton>(&dialog, "radioUpload");
    auto* download = child<QRadioButton>(&dialog, "radioDownload");
    auto* local = child<QLineEdit>(&dialog, "editLocalPath");
    auto* remote = child<QLineEdit>(&dialog, "editRemotePath");
    auto* start = child<QPushButton>(&dialog, "buttonStart");
    QVERIFY(upload && download && local && remote && start);
    QVERIFY(upload->isChecked());
    QCOMPARE(dialog.request().direction, Direction::Upload);
    QVERIFY(local->text().isEmpty());
    QVERIFY(remote->text().isEmpty());
    QCOMPARE(start->text(), QStringLiteral("&Start upload"));   // Alt+U belongs to the radio button

    // Download: the local path defaults to <Downloads>/<remote file name>.
    dialog.setDirection(Direction::Download);
    QVERIFY(download->isChecked());
    QCOMPARE(dialog.request().direction, Direction::Download);
    QCOMPARE(start->text(), QStringLiteral("&Start download"));
    dialog.setRemotePath(QStringLiteral("/root/app.bin"));
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation).isEmpty()
        ? QDir::homePath()
        : QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QCOMPARE(QDir::fromNativeSeparators(local->text()), QDir(downloads).filePath(QStringLiteral("app.bin")));
    // ...and follows the remote name until the user types a local path.
    dialog.setRemotePath(QStringLiteral("/root/other.bin"));
    QCOMPARE(QDir::fromNativeSeparators(local->text()), QDir(downloads).filePath(QStringLiteral("other.bin")));

    // Back to Upload: the remote path was typed as the download *source*, so as the upload
    // destination it counts as derived - its directory is kept, the name follows the local file
    // (uploading fw.bin over /root/other.bin would be a surprise).
    dialog.setDirection(Direction::Upload);
    QCOMPARE(remote->text(), QStringLiteral("/root/other.bin"));
    const QString firmware = tempPath(QStringLiteral("fw.bin"));
    QVERIFY(writeFile(firmware, "fw"));
    dialog.setLocalPath(firmware);
    QCOMPARE(remote->text(), QStringLiteral("/root/fw.bin"));

    // With an empty remote path the upload destination is derived once the home is known.
    dialog.setRemotePath(QString());
    dialog.setLocalPath(firmware);
    QVERIFY(remote->text().isEmpty());   // home unknown, nothing to derive from
    emit connection.remoteHomeReceived(QStringLiteral("/home/test"));
    QCOMPARE(remote->text(), QStringLiteral("/home/test/fw.bin"));
    const QString other = tempPath(QStringLiteral("other.img"));
    QVERIFY(writeFile(other, "img"));
    dialog.setLocalPath(other);
    QCOMPARE(remote->text(), QStringLiteral("/home/test/other.img"));   // derived: follows the file

    // A remote directory (trailing slash) is completed with the file name.
    dialog.setRemotePath(QStringLiteral("/oem/"));
    QCOMPARE(remote->text(), QStringLiteral("/oem/other.img"));
    dialog.setLocalPath(firmware);
    QCOMPARE(remote->text(), QStringLiteral("/oem/fw.bin"));

    // Typed by the user: never replaced while it is the destination.
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("/tmp/mine.bin"));
    dialog.setLocalPath(other);
    QCOMPARE(remote->text(), QStringLiteral("/tmp/mine.bin"));
    // Switching to Download makes it the source (kept as typed) and the local file - chosen as
    // the upload source - a derived destination next to itself: never written over.
    dialog.setDirection(Direction::Download);
    QCOMPARE(remote->text(), QStringLiteral("/tmp/mine.bin"));
    QCOMPARE(QDir::fromNativeSeparators(local->text()), tempPath(QStringLiteral("mine.bin")));
}

void Tst_sshdialogs::remoteRestoredPathsAreDerived()
{
    // The paths remembered from the previous session were not typed into this dialog: a file
    // dropped on the terminal (SessionWidget::uploadFile -> setLocalPath) must land next to the
    // previous upload, not be written over it under the old name.
    {
        QSettings settings;
        settings.beginGroup(QStringLiteral("remoteFile"));
        settings.setValue(QStringLiteral("lastLocalPath"), tempPath(QStringLiteral("fw_v1.bin")));
        settings.setValue(QStringLiteral("lastRemotePath"), QStringLiteral("/oem/fw_v1.bin"));
    }
    SshConnection connection;   // never opened
    RemoteFileDialog dialog(&connection);
    QVERIFY(expose(&dialog));
    auto* local = child<QLineEdit>(&dialog, "editLocalPath");
    auto* remote = child<QLineEdit>(&dialog, "editRemotePath");
    QVERIFY(local && remote);
    QCOMPARE(remote->text(), QStringLiteral("/oem/fw_v1.bin"));   // restored as they were
    QCOMPARE(QDir::fromNativeSeparators(local->text()), tempPath(QStringLiteral("fw_v1.bin")));

    const QString dropped = tempPath(QStringLiteral("fw_v2.bin"));
    QVERIFY(writeFile(dropped, "v2"));
    dialog.setDirection(Direction::Upload);
    dialog.setLocalPath(dropped);
    QCOMPARE(remote->text(), QStringLiteral("/oem/fw_v2.bin"));   // same directory, its own name
    QCOMPARE(dialog.request().remotePath, QStringLiteral("/oem/fw_v2.bin"));

    // A download proposed after a restore behaves the same for the local side.
    dialog.setDirection(Direction::Download);
    dialog.setRemotePath(QStringLiteral("/oem/config.json"));
    QCOMPARE(QDir::fromNativeSeparators(local->text()), tempPath(QStringLiteral("config.json")));

    // Once the user types a destination it is theirs again.
    QTest::keyClicks(local, QStringLiteral("x"));
    dialog.setRemotePath(QStringLiteral("/oem/other.json"));
    QVERIFY(local->text().endsWith(QLatin1Char('x')));
}

void Tst_sshdialogs::remoteRequestContents()
{
    SshConnection connection;
    RemoteFileDialog dialog(&connection);
    QVERIFY(expose(&dialog));
    auto* overwrite = child<QCheckBox>(&dialog, "checkOverwrite");
    auto* preserve = child<QCheckBox>(&dialog, "checkPreservePermissions");
    QVERIFY(overwrite->isChecked());
    QVERIFY(preserve->isChecked());

    dialog.setLocalPath(QStringLiteral("C:/tmp/a b/fw.bin"));
    dialog.setRemotePath(QStringLiteral("  /oem/fw.bin  "));
    SshConnection::TransferRequest req = dialog.request();
    QCOMPARE(req.direction, Direction::Upload);
    QCOMPARE(req.localPath, QStringLiteral("C:/tmp/a b/fw.bin"));   // forward slashes whatever is shown
    QCOMPARE(req.remotePath, QStringLiteral("/oem/fw.bin"));
    QVERIFY(req.overwrite);
    QVERIFY(req.preservePermissions);
    QCOMPARE(child<QLineEdit>(&dialog, "editLocalPath")->text(),
             QDir::toNativeSeparators(QStringLiteral("C:/tmp/a b/fw.bin")));

    overwrite->click();
    preserve->click();
    dialog.setDirection(Direction::Download);
    req = dialog.request();
    QCOMPARE(req.direction, Direction::Download);
    QVERIFY(!req.overwrite);
    QVERIFY(!req.preservePermissions);
    QCOMPARE(req.remotePath, QStringLiteral("/oem/fw.bin"));
}

void Tst_sshdialogs::remoteStartDisabledWhileDisconnected()
{
    SshConnection connection;
    RemoteFileDialog dialog(&connection);
    QVERIFY(expose(&dialog));
    auto* start = child<QPushButton>(&dialog, "buttonStart");
    auto* cancel = child<QPushButton>(&dialog, "buttonCancel");
    auto* status = child<QLabel>(&dialog, "labelStatus");
    QVERIFY(start && cancel && status);
    QVERIFY(!dialog.isModal());

    QVERIFY(!start->isEnabled());
    QCOMPARE(status->text(), QStringLiteral("Not connected."));
    dialog.setLocalPath(tempPath(QStringLiteral("fw.bin")));
    dialog.setRemotePath(QStringLiteral("/oem/fw.bin"));
    QCOMPARE(connection.state(), State::Disconnected);
    QVERIFY(!start->isEnabled());
    QVERIFY(!cancel->isEnabled());
    QCOMPARE(start->toolTip(), QStringLiteral("Connect the session first."));

    // Nothing happens on a click either (the button is disabled).
    QTest::mouseClick(start, Qt::LeftButton);
    QCOMPARE(status->text(), QStringLiteral("Not connected."));

    // A finished report from the connection is re-emitted by the dialog.
    QSignalSpy finished(&dialog, &RemoteFileDialog::transferFinished);
    emit connection.transferFinished(false, QStringLiteral("Connection closed"));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(status->text(), QStringLiteral("Connection closed"));
    emit connection.transferProgress(12345678, 45678901);
    QVERIFY(status->text().contains(QStringLiteral("(27%)")));
    QCOMPARE(child<QProgressBar>(&dialog, "progressBar")->value(), 27);

    child<QPushButton>(&dialog, "buttonClose")->click();
    QVERIFY(!dialog.isVisible());
}

void Tst_sshdialogs::remoteHomeButton()
{
    SshConnection connection;
    RemoteFileDialog dialog(&connection);
    QVERIFY(expose(&dialog));
    auto* home = child<QToolButton>(&dialog, "buttonRemoteHome");
    auto* remote = child<QLineEdit>(&dialog, "editRemotePath");
    QVERIFY(home && remote);
    const QString firmware = tempPath(QStringLiteral("fw.bin"));
    QVERIFY(writeFile(firmware, "fw"));
    dialog.setLocalPath(firmware);

    // Home unknown and not connected: nothing to fill in yet.
    QTest::mouseClick(home, Qt::LeftButton);
    QVERIFY(remote->text().isEmpty());

    // Once known, "~" completes <home>/<local file name> for an upload...
    emit connection.remoteHomeReceived(QStringLiteral("/root"));
    QCOMPARE(remote->text(), QStringLiteral("/root/fw.bin"));   // derived on arrival
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("/somewhere/else.bin"));
    QTest::mouseClick(home, Qt::LeftButton);
    QCOMPARE(remote->text(), QStringLiteral("/root/fw.bin"));

    // ...and <home>/<remote file name> for a download.
    dialog.setDirection(Direction::Download);
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("/oem/app.bin"));
    QTest::mouseClick(home, Qt::LeftButton);
    QCOMPARE(remote->text(), QStringLiteral("/root/app.bin"));
    remote->clear();
    QTest::mouseClick(home, Qt::LeftButton);
    QCOMPARE(remote->text(), QStringLiteral("/root/"));
}

void Tst_sshdialogs::remotePathsPersist()
{
    SshConnection connection;
    const QString localFile = tempPath(QStringLiteral("persist.bin"));
    {
        RemoteFileDialog dialog(&connection);
        QVERIFY(expose(&dialog));
        dialog.setLocalPath(localFile);
        dialog.setRemotePath(QStringLiteral("/oem/persist.bin"));
    }
    QSettings settings;
    QCOMPARE(settings.value(QStringLiteral("remoteFile/lastLocalPath")).toString(), localFile);
    QCOMPARE(settings.value(QStringLiteral("remoteFile/lastRemotePath")).toString(),
             QStringLiteral("/oem/persist.bin"));

    RemoteFileDialog dialog(&connection);
    QVERIFY(expose(&dialog));
    QCOMPARE(child<QLineEdit>(&dialog, "editLocalPath")->text(), QDir::toNativeSeparators(localFile));
    QCOMPARE(child<QLineEdit>(&dialog, "editRemotePath")->text(), QStringLiteral("/oem/persist.bin"));
    QCOMPARE(dialog.request().localPath, localFile);
}

void Tst_sshdialogs::remoteRetranslate()
{
    // The dialog is modeless: a language change must rebuild the direction-dependent captions
    // and the idle status line (retranslateUi() alone would reset them to the .ui defaults).
    SshConnection connection;   // never opened: Disconnected
    RemoteFileDialog dialog(&connection);
    QVERIFY(expose(&dialog));
    dialog.setDirection(Direction::Download);
    auto* start = child<QPushButton>(&dialog, "buttonStart");
    auto* status = child<QLabel>(&dialog, "labelStatus");
    auto* labelLocal = child<QLabel>(&dialog, "labelLocal");
    QVERIFY(start && status && labelLocal);
    QCOMPARE(start->text(), QStringLiteral("&Start download"));
    QCOMPARE(status->text(), QStringLiteral("Not connected."));

    QEvent event(QEvent::LanguageChange);
    QApplication::sendEvent(&dialog, &event);
    QCOMPARE(dialog.windowTitle(), QStringLiteral("Remote File Transfer"));
    QCOMPARE(start->text(), QStringLiteral("&Start download"));
    QCOMPARE(labelLocal->text(), QStringLiteral("Save &to:"));   // Alt+S stays with Start, Alt+A with WeChat
    QCOMPARE(status->text(), QStringLiteral("Not connected."));
    QVERIFY(!start->isEnabled());
    QCOMPARE(dialog.request().direction, Direction::Download);
}

void Tst_sshdialogs::remoteMethodTexts()
{
    // The status line names the method the connection chose - transferStatus().method, which
    // the worker publishes before transferStarted(). Without a worker the test supplies the
    // status through the dialog's slot after the signal, then drives progress and the end.
    SshConnection connection;   // never opened
    RemoteFileDialog dialog(&connection);
    QVERIFY(expose(&dialog));
    auto* status = child<QLabel>(&dialog, "labelStatus");
    auto* start = child<QPushButton>(&dialog, "buttonStart");
    auto* cancel = child<QPushButton>(&dialog, "buttonCancel");
    auto* progress = child<QProgressBar>(&dialog, "progressBar");
    QVERIFY(status && start && cancel && progress);
    QVERIFY(!isReddish(status->palette().color(QPalette::WindowText)));

    SshConnection::TransferRequest up;
    up.direction = Direction::Upload;
    up.localPath = QStringLiteral("C:/tmp/fw v2.bin");
    up.remotePath = QStringLiteral("/oem/fw v2.bin");
    emit connection.transferStarted(up);
    QCOMPARE(status->text(), QStringLiteral("Uploading fw v2.bin..."));   // no method published yet

    SshConnection::TransferStatus sftp;
    sftp.active = true;
    sftp.direction = Direction::Upload;
    sftp.localPath = up.localPath;
    sftp.remotePath = up.remotePath;
    sftp.total = 3000000;
    sftp.method = QStringLiteral("sftp");
    QVERIFY(QMetaObject::invokeMethod(&dialog, "applyTransferStatus", Q_ARG(SshConnection::TransferStatus, sftp)));
    QCOMPARE(status->text(), QStringLiteral("Uploading fw v2.bin via SFTP..."));
    emit connection.transferProgress(1500000, 3000000);
    QVERIFY2(status->text().startsWith(QStringLiteral("Uploading fw v2.bin via SFTP: ")), qPrintable(status->text()));
    QVERIFY2(status->text().endsWith(QStringLiteral("(50%)")), qPrintable(status->text()));
    QCOMPARE(progress->value(), 50);
    QVERIFY(!isReddish(status->palette().color(QPalette::WindowText)));

    // Finished: the connection's own message, verbatim and not red.
    QSignalSpy finished(&dialog, &RemoteFileDialog::transferFinished);
    const QString done = QStringLiteral("Uploaded fw v2.bin to /oem/fw v2.bin (3.0 MB, SFTP)");
    emit connection.transferFinished(true, done);
    QCOMPARE(status->text(), done);
    QCOMPARE(progress->value(), 100);
    QVERIFY(!isReddish(status->palette().color(QPalette::WindowText)));
    QCOMPARE(finished.count(), 1);

    // The shell fallback on a download with an unknown size.
    SshConnection::TransferRequest down;
    down.direction = Direction::Download;
    down.localPath = QStringLiteral("C:/tmp/app.bin");
    down.remotePath = QStringLiteral("/oem/app.bin");
    emit connection.transferStarted(down);
    QCOMPARE(status->text(), QStringLiteral("Downloading app.bin..."));
    SshConnection::TransferStatus shell;
    shell.active = true;
    shell.direction = Direction::Download;
    shell.localPath = down.localPath;
    shell.remotePath = down.remotePath;
    shell.total = -1;
    shell.method = QStringLiteral("shell");
    QVERIFY(QMetaObject::invokeMethod(&dialog, "applyTransferStatus", Q_ARG(SshConnection::TransferStatus, shell)));
    QCOMPARE(status->text(), QStringLiteral("Downloading app.bin via shell (cat)..."));
    emit connection.transferProgress(4096, -1);
    QVERIFY2(status->text().startsWith(QStringLiteral("Downloading app.bin via shell (cat): ")),
             qPrintable(status->text()));
    QCOMPARE(progress->maximum(), 0);   // busy indicator

    // Neither method works: the message in red, Cancel off, the progress bar back to determinate.
    const QString failure =
        QStringLiteral("Cannot start SFTP on root@192.168.100.2:22 (subsystem request failed) and the shell fallback failed: cat: not found");
    emit connection.transferFinished(false, failure);
    QCOMPARE(status->text(), failure);
    QVERIFY(isReddish(status->palette().color(QPalette::WindowText)));
    QVERIFY(!cancel->isEnabled());
    QCOMPARE(progress->maximum(), 100);
    QCOMPARE(finished.count(), 2);
    QVERIFY(!finished.at(1).at(0).toBool());
    // Start follows the connection state (never opened here: off, with the reason).
    QVERIFY(!start->isEnabled());
    QCOMPARE(start->toolTip(), QStringLiteral("Connect the session first."));

    // The next idle line is in the normal colour again (a direction change rewrites it).
    dialog.setDirection(Direction::Download);
    QCOMPARE(status->text(), QStringLiteral("Not connected."));
    QVERIFY(!isReddish(status->palette().color(QPalette::WindowText)));
}

void Tst_sshdialogs::remoteRequestNormalisation()
{
    SshConnection connection;   // never opened: the home is unknown at first
    RemoteFileDialog dialog(&connection);
    QVERIFY(expose(&dialog));
    auto* remote = child<QLineEdit>(&dialog, "editRemotePath");
    QVERIFY(remote);
    const QString firmware = tempPath(QStringLiteral("norm fw.bin"));
    QVERIFY(writeFile(firmware, "fw"));
    dialog.setLocalPath(firmware);
    QVERIFY(remote->text().isEmpty());   // home unknown

    // Backslashes are a Windows habit: converted, never sent to the host.
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("\\oem\\fw.bin"));
    QCOMPARE(dialog.request().remotePath, QStringLiteral("/oem/fw.bin"));

    // A directory typed and Start pressed from the field (Alt+S, no editingFinished): the
    // request carries the file name although the field still shows the directory.
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("/tmp/"));
    QCOMPARE(remote->text(), QStringLiteral("/tmp/"));
    QCOMPARE(dialog.request().remotePath, QStringLiteral("/tmp/norm fw.bin"));

    // "~" with the home unknown: relative to the login directory (SFTP and the shell fallback
    // both resolve it there).
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("~"));
    QCOMPARE(dialog.request().remotePath, QStringLiteral("norm fw.bin"));
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("~/"));
    QCOMPARE(dialog.request().remotePath, QStringLiteral("norm fw.bin"));
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("~/boards/fw.bin"));
    QCOMPARE(dialog.request().remotePath, QStringLiteral("boards/fw.bin"));
    // setRemotePath("~") keeps the tilde in the field until the home is known...
    dialog.setRemotePath(QStringLiteral("~"));
    QCOMPARE(remote->text(), QStringLiteral("~/norm fw.bin"));
    QCOMPARE(dialog.request().remotePath, QStringLiteral("norm fw.bin"));

    // ...and spells it out once it is (a trailing slash on the home is tolerated).
    emit connection.remoteHomeReceived(QStringLiteral("/home/test/"));
    QCOMPARE(remote->text(), QStringLiteral("/home/test/norm fw.bin"));
    QCOMPARE(dialog.request().remotePath, QStringLiteral("/home/test/norm fw.bin"));
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("~/boards/fw.bin"));
    QCOMPARE(dialog.request().remotePath, QStringLiteral("/home/test/boards/fw.bin"));
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("~"));
    QCOMPARE(dialog.request().remotePath, QStringLiteral("/home/test/norm fw.bin"));
    dialog.setRemotePath(QStringLiteral("~/sub/"));
    QCOMPARE(remote->text(), QStringLiteral("/home/test/sub/norm fw.bin"));

    // A download resolves "~" the same way and derives the local name from it.
    dialog.setDirection(Direction::Download);
    dialog.setRemotePath(QStringLiteral("~/app.bin"));
    QCOMPARE(dialog.request().remotePath, QStringLiteral("/home/test/app.bin"));
    QCOMPARE(QFileInfo(dialog.request().localPath).fileName(), QStringLiteral("app.bin"));
    // A bare "~" is no file to download: the request stays empty, Start off.
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("~"));
    QCOMPARE(dialog.request().remotePath, QStringLiteral("/home/test/"));
    QVERIFY(!child<QPushButton>(&dialog, "buttonStart")->isEnabled());
}

void Tst_sshdialogs::remoteAcceleratorsUnique()
{
    // Every Alt+letter of the dialog is unique in both directions: Qt gives an ambiguous
    // shortcut to the widgets in turn and a push button only takes the focus on it, so Alt+S
    // did not start a download while "&Save as:" shared the letter (and Cl&ose shared Alt+O
    // with &Overwrite).
    SshConnection connection;
    RemoteFileDialog dialog(&connection);
    QVERIFY(expose(&dialog));
    for (Direction direction : {Direction::Upload, Direction::Download}) {
        dialog.setDirection(direction);
        QMap<QChar, QStringList> owners;
        const QList<QWidget*> widgets = dialog.findChildren<QWidget*>();
        for (QWidget* widget : widgets) {
            QString text;
            if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
                text = button->text();
            } else if (auto* label = qobject_cast<QLabel*>(widget)) {
                text = label->buddy() ? label->text() : QString();
            }
            const qsizetype amp = text.indexOf(QLatin1Char('&'));
            if (amp >= 0 && amp + 1 < text.size() && text.at(amp + 1) != QLatin1Char('&')) {
                owners[text.at(amp + 1).toLower()].append(text);
            }
        }
        QVERIFY(owners.contains(QLatin1Char('s')));   // Start
        QVERIFY(owners.contains(QLatin1Char('r')));   // the remote field
        for (auto it = owners.cbegin(); it != owners.cend(); ++it) {
            QVERIFY2(it.value().size() == 1,
                     qPrintable(QStringLiteral("Alt+%1 shared by: %2").arg(it.key()).arg(it.value().join(QStringLiteral(", ")))));
        }
    }
}

void Tst_sshdialogs::remoteHomeUnresolved()
{
    // A server without SFTP (dropbear on a buildroot board) cannot resolve the home: an upload
    // with no destination still proposes the plain file name - the login directory - so Start
    // is not stuck disabled with nothing to say.
    SshConnection connection;
    RemoteFileDialog dialog(&connection);
    QVERIFY(expose(&dialog));
    auto* remote = child<QLineEdit>(&dialog, "editRemotePath");
    auto* home = child<QToolButton>(&dialog, "buttonRemoteHome");
    auto* status = child<QLabel>(&dialog, "labelStatus");
    QVERIFY(remote && home && status);
    const QString firmware = tempPath(QStringLiteral("fw.bin"));
    QVERIFY(writeFile(firmware, "fw"));
    dialog.setLocalPath(firmware);
    QVERIFY(remote->text().isEmpty());

    emit connection.remoteHomeReceived(QString());   // the worker's answer when SFTP is unavailable
    QCOMPARE(remote->text(), QStringLiteral("fw.bin"));
    QCOMPARE(dialog.request().remotePath, QStringLiteral("fw.bin"));
    // Derived: a later file follows.
    const QString other = tempPath(QStringLiteral("other.img"));
    QVERIFY(writeFile(other, "img"));
    dialog.setLocalPath(other);
    QCOMPARE(remote->text(), QStringLiteral("other.img"));
    // A typed destination is left alone by the failure.
    remote->clear();
    QTest::keyClicks(remote, QStringLiteral("/oem/x.bin"));
    emit connection.remoteHomeReceived(QString());
    QCOMPARE(remote->text(), QStringLiteral("/oem/x.bin"));
    QVERIFY(!isReddish(status->palette().color(QPalette::WindowText)));

    // The "~" button explains itself while disconnected.
    QTest::mouseClick(home, Qt::LeftButton);
    QCOMPARE(status->text(), QStringLiteral("The remote home directory is known once the session is connected."));
    QCOMPARE(remote->text(), QStringLiteral("/oem/x.bin"));
}

void Tst_sshdialogs::remoteErrorLineSurvivesDisconnect()
{
    // Header: a failure line - "Transfer aborted: the connection was closed", which the
    // connection reports right before its state changes on close() or a dropped link - is kept
    // while the link is down instead of being replaced by "Not connected."; a direction change,
    // the next transfer or Connected write over it, and a success line is not kept.
    SshConnection connection;   // never opened
    RemoteFileDialog dialog(&connection);
    QVERIFY(expose(&dialog));
    auto* status = child<QLabel>(&dialog, "labelStatus");
    QVERIFY(status);
    QSignalSpy finished(&dialog, &RemoteFileDialog::transferFinished);
    QCOMPARE(status->text(), QStringLiteral("Not connected."));

    const QString aborted = QStringLiteral("Transfer aborted: the connection was closed");
    emit connection.transferFinished(false, aborted);
    QCOMPARE(status->text(), aborted);
    QVERIFY(isReddish(status->palette().color(QPalette::WindowText)));
    emit connection.stateChanged(Transport::State::Disconnected);
    QCOMPARE(status->text(), aborted);   // kept: the reason stays next to the disabled Start
    QVERIFY(isReddish(status->palette().color(QPalette::WindowText)));
    emit connection.stateChanged(Transport::State::Reconnecting);
    QCOMPARE(status->text(), aborted);
    QCOMPARE(finished.count(), 1);
    QVERIFY(!child<QPushButton>(&dialog, "buttonStart")->isEnabled());

    // A direction change writes the idle line again, in the normal colour.
    dialog.setDirection(SshConnection::TransferDirection::Download);
    QCOMPARE(status->text(), QStringLiteral("Not connected."));
    QVERIFY(!isReddish(status->palette().color(QPalette::WindowText)));

    // A success line is replaced by the idle line on a state change.
    const QString done = QStringLiteral("Uploaded fw.bin to /oem/fw.bin (1.0 MB, SFTP)");
    emit connection.transferFinished(true, done);
    QCOMPARE(status->text(), done);
    emit connection.stateChanged(Transport::State::Disconnected);
    QCOMPARE(status->text(), QStringLiteral("Not connected."));
    QCOMPARE(finished.count(), 2);

    // Connected replaces an error line as well (the idle text follows the connection's state,
    // which this never-opened connection still reports as Disconnected).
    emit connection.transferFinished(false, aborted);
    QVERIFY(isReddish(status->palette().color(QPalette::WindowText)));
    emit connection.stateChanged(Transport::State::Connected);
    QCOMPARE(status->text(), QStringLiteral("Not connected."));
    QVERIFY(!isReddish(status->palette().color(QPalette::WindowText)));
}

QTEST_MAIN(Tst_sshdialogs)
#include "tst_sshdialogs.moc"
