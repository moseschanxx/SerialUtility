#include "dialogs/AuthPromptDialog.h"
#include "ui_AuthPromptDialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QToolButton>

#include "app/Logging.h"
#include "ssh/SecretStore.h"

namespace {

constexpr int kMaxAttempts = 3;

QString defaultTitle(const SshConnection::AuthPrompt& prompt)
{
    switch (prompt.kind) {
    case SshConnection::PromptKind::Password:
        return AuthPromptDialog::tr("Password");
    case SshConnection::PromptKind::Passphrase:
        return AuthPromptDialog::tr("Key Passphrase");
    case SshConnection::PromptKind::KeyboardInteractive:
        return AuthPromptDialog::tr("Authentication");
    }
    return AuthPromptDialog::tr("Authentication");
}

QString defaultPrompt(const SshConnection::AuthPrompt& prompt)
{
    switch (prompt.kind) {
    case SshConnection::PromptKind::Password:
        return AuthPromptDialog::tr("Password:");
    case SshConnection::PromptKind::Passphrase:
        return prompt.keyFile.isEmpty()
            ? AuthPromptDialog::tr("Passphrase for the private key:")
            : AuthPromptDialog::tr("Passphrase for key '%1':").arg(QDir::toNativeSeparators(prompt.keyFile));
    case SshConnection::PromptKind::KeyboardInteractive:
        return AuthPromptDialog::tr("Response:");
    }
    return AuthPromptDialog::tr("Response:");
}

/// "user@host" (or whichever part is known) - the headline of the dialog.
QString targetText(const SshConnection::AuthPrompt& prompt)
{
    if (prompt.user.isEmpty()) {
        return prompt.host;
    }
    return prompt.host.isEmpty() ? prompt.user : QStringLiteral("%1@%2").arg(prompt.user, prompt.host);
}

/// The caption of the Remember checkbox: what the answer is saved for (class comment).
QString rememberLabel(const SshConnection::AuthPrompt& prompt)
{
    const QString rememberTarget = prompt.rememberTarget.trimmed();
    const QString target = rememberTarget.isEmpty() ? targetText(prompt) : rememberTarget;
    switch (prompt.kind) {
    case SshConnection::PromptKind::Password:
        return target.isEmpty() ? AuthPromptDialog::tr("Remember password")
                                : AuthPromptDialog::tr("Remember password for %1").arg(target);
    case SshConnection::PromptKind::Passphrase: {
        const QString file = rememberTarget.isEmpty() ? prompt.keyFile : rememberTarget;
        const QString name = QFileInfo(file).fileName();
        return name.isEmpty() ? AuthPromptDialog::tr("Remember passphrase")
                              : AuthPromptDialog::tr("Remember passphrase for %1").arg(name);
    }
    case SshConnection::PromptKind::KeyboardInteractive:
        return target.isEmpty() ? AuthPromptDialog::tr("Remember answer")
                                : AuthPromptDialog::tr("Remember answer for %1").arg(target);
    }
    return AuthPromptDialog::tr("Remember password");
}

/// Smaller, dimmed text for secondary information (where the secret would be stored).
void makeSubtle(QLabel* label)
{
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * 0.9);
    label->setFont(font);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::Disabled, QPalette::WindowText));
    label->setPalette(palette);
}

} // namespace

AuthPromptDialog::AuthPromptDialog(const SshConnection::AuthPrompt& prompt, QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::AuthPromptDialog)
{
    ui->setupUi(this);
    setModal(true);
    setWindowTitle(prompt.title.trimmed().isEmpty() ? defaultTitle(prompt) : prompt.title.trimmed());

    // user@host
    const QString target = targetText(prompt);
    ui->labelTarget->setText(target);
    ui->labelTarget->setVisible(!target.isEmpty());

    ui->labelInstruction->setText(prompt.instruction.trimmed());
    ui->labelInstruction->setVisible(!prompt.instruction.trimmed().isEmpty());

    if (prompt.attempt > 1) {
        ui->labelAttempt->setText(
            tr("Authentication failed, try again (attempt %1 of %2)").arg(prompt.attempt).arg(kMaxAttempts));
        QPalette palette = ui->labelAttempt->palette();
        palette.setColor(QPalette::WindowText, QColor(0xC0, 0x1C, 0x28));
        ui->labelAttempt->setPalette(palette);
        ui->labelAttempt->setVisible(true);
    } else {
        ui->labelAttempt->clear();
        ui->labelAttempt->setVisible(false);
    }

    ui->labelPrompt->setText(prompt.prompt.trimmed().isEmpty() ? defaultPrompt(prompt) : prompt.prompt.trimmed());

    // The response is masked unless the server asked for echo; "Show" reveals it.
    ui->editResponse->setEchoMode(prompt.echo ? QLineEdit::Normal : QLineEdit::Password);
    ui->buttonShow->setChecked(false);
    ui->buttonShow->setVisible(!prompt.echo);
    ui->buttonShow->setFocusPolicy(Qt::NoFocus);
    connect(ui->buttonShow, &QToolButton::toggled, this, [this](bool show) {
        ui->editResponse->setEchoMode(show ? QLineEdit::Normal : QLineEdit::Password);
        ui->buttonShow->setText(show ? tr("Hide") : tr("Show"));
    });

    // Remember (SecretStore under the profile id, the ad-hoc target or the key file - the
    // connection decides; the caption names what the answer is saved for). Never pre-checked.
    const QString note = SecretStore::storageDescription();
    ui->checkRemember->setChecked(false);
    ui->checkRemember->setText(rememberLabel(prompt));
    ui->checkRemember->setVisible(prompt.canRemember);
    ui->checkRemember->setToolTip(note);
    ui->labelNote->setText(note);
    ui->labelNote->setVisible(prompt.canRemember && !note.isEmpty());
    makeSubtle(ui->labelNote);

    QPushButton* ok = ui->buttonBox->button(QDialogButtonBox::Ok);
    ok->setDefault(true);   // Enter in the response field accepts
    connect(ui->buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);

    ui->editResponse->setFocus(Qt::OtherFocusReason);
    adjustSize();
    qCDebug(lcSsh) << "auth prompt shown:" << static_cast<int>(prompt.kind) << target << "attempt" << prompt.attempt
                   << "remember:" << prompt.canRemember << prompt.rememberTarget;
}

AuthPromptDialog::~AuthPromptDialog()
{
    delete ui;
}

QString AuthPromptDialog::response() const
{
    return ui->editResponse->text();
}

bool AuthPromptDialog::remember() const
{
    return ui->checkRemember->isChecked();
}
