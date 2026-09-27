#pragma once

#include <QDialog>

#include "ssh/SshConnection.h"

QT_BEGIN_NAMESPACE
namespace Ui { class AuthPromptDialog; }
QT_END_NAMESPACE

/**
 * Password / key passphrase / keyboard-interactive prompt (SshConnection::authPromptRequired).
 * Shows "user@host", the instruction (if any) and the prompt; the line edit is masked unless
 * prompt.echo; a "Show" toggle reveals it.
 *
 * The Remember checkbox is shown whenever prompt.canRemember (since v0.4 every password and
 * passphrase prompt, for a stored profile and for an ad-hoc target alike), unchecked by default,
 * and its caption says what the answer is saved for:
 *  - Password:              "Remember password for <rememberTarget>"   ("root@host", or
 *                           "root@host:2222" when the port is not 22: SshProfile::displayTarget())
 *  - Passphrase:            "Remember passphrase for <key file name>"  (the file name of
 *                           prompt.rememberTarget, or of prompt.keyFile when the target is empty)
 *  - Keyboard-interactive:  "Remember answer for <rememberTarget>"
 * An empty rememberTarget falls back to the "user@host" shown at the top (a worker that does not
 * fill the field yet), and to the plain "Remember password" / "Remember passphrase" / "Remember
 * answer" when even that is empty. SecretStore::storageDescription() is the checkbox tooltip and
 * the dim note under it. Attempt > 1 shows "Authentication failed, try again (attempt n of 3)"
 * in red.
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
