#pragma once

#include <QDialog>

#include "ssh/SshConnection.h"

QT_BEGIN_NAMESPACE
namespace Ui { class HostKeyDialog; }
QT_END_NAMESPACE

/**
 * Host key verification (SshConnection::hostKeyVerificationRequired).
 *
 * Unknown / KeyTypeChanged / Error:
 *   "The authenticity of host 'host:port' can't be established." + key type + SHA256 and MD5
 *   fingerprints in a monospace, selectable field; buttons "Connect and remember" (default),
 *   "Connect once", "Cancel".
 * Changed:
 *   red "WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!" banner, the explanation that this
 *   may be a man-in-the-middle attack or a reinstalled board (common with dev boards), a
 *   checkbox "I understand the risk, replace the stored key" that enables
 *   "Replace key and connect"; plus "Connect once" and "Cancel" (default).
 * result(): QDialog::Accepted -> accepted(); remember() tells whether known_hosts is updated.
 * Layout in HostKeyDialog.ui: labelHeadline, labelHost, labelKeyType, editFingerprintSha256,
 * editFingerprintMd5, labelMessage, checkUnderstand, buttonBox with custom buttons.
 */
class HostKeyDialog : public QDialog
{
    Q_OBJECT
public:
    explicit HostKeyDialog(const SshConnection::HostKeyInfo& info, QWidget* parent = nullptr);
    ~HostKeyDialog() override;

    bool remember() const;            ///< valid after exec()/accept()

private:
    Ui::HostKeyDialog* ui;
    SshConnection::HostKeyInfo m_info;
    bool m_remember = false;
};
