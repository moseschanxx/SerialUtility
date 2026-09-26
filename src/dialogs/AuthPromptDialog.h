#pragma once

#include <QDialog>

#include "ssh/SshConnection.h"

QT_BEGIN_NAMESPACE
namespace Ui { class AuthPromptDialog; }
QT_END_NAMESPACE

/**
 * Password / key passphrase / keyboard-interactive prompt (SshConnection::authPromptRequired).
 * Shows "user@host", the instruction (if any) and the prompt; the line edit is masked unless
 * prompt.echo; a "Show" toggle reveals it; "Remember in this profile" (visible when
 * prompt.canRemember) with SecretStore::storageDescription() as its tooltip / note.
 * Attempt > 1 shows "Authentication failed, try again (attempt n of 3)" in red.
 * Layout in AuthPromptDialog.ui: labelTarget, labelInstruction, labelPrompt, editResponse,
 * buttonShow, checkRemember, labelNote, labelAttempt, buttonBox (Ok | Cancel).
 */
class AuthPromptDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AuthPromptDialog(const SshConnection::AuthPrompt& prompt, QWidget* parent = nullptr);
    ~AuthPromptDialog() override;

    QString response() const;
    bool remember() const;

private:
    Ui::AuthPromptDialog* ui;
};
