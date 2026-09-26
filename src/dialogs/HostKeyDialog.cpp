#include "dialogs/HostKeyDialog.h"
#include "ui_HostKeyDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFontDatabase>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

#include "app/Logging.h"

namespace {

/// "host:port", with IPv6 literals in brackets like known_hosts / OpenSSH print them.
QString hostText(const SshConnection::HostKeyInfo& info)
{
    const QString host = info.host.contains(QLatin1Char(':')) ? QStringLiteral("[%1]").arg(info.host) : info.host;
    return QStringLiteral("%1:%2").arg(host).arg(info.port);
}

/// Smaller, dimmed text for secondary information (the known_hosts path).
void makeSubtle(QLabel* label)
{
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * 0.9);
    label->setFont(font);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::Disabled, QPalette::WindowText));
    label->setPalette(palette);
}

void makeRed(QLabel* label)
{
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, QColor(0xC0, 0x1C, 0x28));
    label->setPalette(palette);
}

} // namespace

HostKeyDialog::HostKeyDialog(const SshConnection::HostKeyInfo& info, QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::HostKeyDialog)
    , m_info(info)
{
    ui->setupUi(this);
    setModal(true);

    // Fingerprints: monospace, read-only, selectable (the user compares them with the device).
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    for (QLineEdit* edit : {ui->editFingerprintSha256, ui->editFingerprintMd5}) {
        edit->setFont(mono);
        edit->setReadOnly(true);
    }
    ui->editFingerprintSha256->setText(m_info.fingerprintSha256);
    ui->editFingerprintSha256->setCursorPosition(0);
    ui->editFingerprintMd5->setText(m_info.fingerprintMd5);
    ui->editFingerprintMd5->setCursorPosition(0);
    const bool hasMd5 = !m_info.fingerprintMd5.isEmpty();
    ui->labelMd5Caption->setVisible(hasMd5);
    ui->editFingerprintMd5->setVisible(hasMd5);

    const QString host = hostText(m_info);
    const QString keyType = m_info.keyType.isEmpty() ? tr("unknown") : m_info.keyType;
    ui->labelHost->setText(host);
    ui->labelKeyType->setText(keyType);
    ui->labelMessage->setText(m_info.message);
    ui->labelMessage->setVisible(!m_info.message.isEmpty());

    const QString knownHosts =
        m_info.knownHostsFile.isEmpty() ? SshConnection::defaultKnownHostsFile() : m_info.knownHostsFile;
    ui->labelKnownHosts->setText(tr("known_hosts file: %1").arg(QDir::toNativeSeparators(knownHosts)));
    makeSubtle(ui->labelKnownHosts);

    // Buttons: [Connect and remember | Replace key and connect] [Connect once] [Cancel]
    QPushButton* cancel = ui->buttonBox->button(QDialogButtonBox::Cancel);
    QPushButton* remember = ui->buttonBox->addButton(QString(), QDialogButtonBox::AcceptRole);
    remember->setObjectName(QStringLiteral("buttonRemember"));
    QPushButton* once = ui->buttonBox->addButton(tr("Connect once"), QDialogButtonBox::AcceptRole);
    once->setObjectName(QStringLiteral("buttonOnce"));
    once->setToolTip(tr("Connect without changing the known_hosts file"));
    for (QPushButton* button : {cancel, remember, once}) {
        button->setAutoDefault(false);
        button->setDefault(false);
    }

    const bool changed = (m_info.status == SshConnection::HostKeyStatus::Changed);
    if (changed) {
        setWindowTitle(tr("Host Key Changed"));
        ui->labelHeadline->setText(tr("WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!"));
        makeRed(ui->labelHeadline);
        ui->labelExplanation->setText(
            tr("The %1 key presented by %2 differs from the one stored for this host. Someone could be "
               "eavesdropping on you right now (man-in-the-middle attack) - or the host key simply changed, "
               "for example because the board was re-flashed or reinstalled, which is common with development "
               "boards.\n\nOnly replace the stored key if you are sure the change is expected.")
                .arg(keyType, host));
        ui->checkUnderstand->setVisible(true);
        ui->checkUnderstand->setChecked(false);
        remember->setText(tr("Replace key and connect"));
        remember->setToolTip(tr("Replace the stored key for this host in known_hosts and connect"));
        remember->setEnabled(false);
        connect(ui->checkUnderstand, &QCheckBox::toggled, remember, &QPushButton::setEnabled);
        cancel->setDefault(true);
    } else {
        setWindowTitle(tr("Host Key Verification"));
        ui->labelHeadline->setText(tr("The authenticity of host '%1' can't be established.").arg(host));
        QString explanation;
        switch (m_info.status) {
        case SshConnection::HostKeyStatus::KeyTypeChanged:
            explanation = tr("known_hosts holds a key of another type for this host; the server now also offers "
                             "a %1 key. This is normal after an SSH server upgrade, but compare the fingerprint "
                             "with the one shown on the device before you continue.")
                              .arg(keyType);
            break;
        case SshConnection::HostKeyStatus::Error:
            explanation = tr("The known_hosts file could not be read, so the key could not be checked. Compare "
                             "the fingerprint with the one shown on the device before you continue.");
            break;
        case SshConnection::HostKeyStatus::Known:
        case SshConnection::HostKeyStatus::Unknown:
        case SshConnection::HostKeyStatus::Changed:
            explanation = tr("This host is not in your known_hosts file yet. Compare the fingerprint with the "
                             "one shown on the device (ssh-keygen -lf /etc/ssh/ssh_host_%1_key.pub) before you "
                             "continue.")
                              .arg(keyType.startsWith(QLatin1String("ssh-")) ? keyType.mid(4)
                                                                             : QStringLiteral("ed25519"));
            break;
        }
        ui->labelExplanation->setText(explanation);
        ui->checkUnderstand->setVisible(false);
        remember->setText(tr("Connect and remember"));
        remember->setToolTip(tr("Add the key to known_hosts and connect"));
        remember->setDefault(true);
    }

    connect(remember, &QPushButton::clicked, this, [this]() {
        m_remember = true;
        qCInfo(lcSsh) << "host key accepted and remembered for" << hostText(m_info) << m_info.keyType;
        accept();
    });
    connect(once, &QPushButton::clicked, this, [this]() {
        m_remember = false;
        qCInfo(lcSsh) << "host key accepted once for" << hostText(m_info) << m_info.keyType;
        accept();
    });
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    adjustSize();
}

HostKeyDialog::~HostKeyDialog()
{
    delete ui;
}

bool HostKeyDialog::remember() const
{
    return m_remember;
}
