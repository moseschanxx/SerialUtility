#pragma once

#include <QDialog>
#include <QHash>
#include <QList>
#include <QStringList>

#include "ssh/SshProfile.h"

QT_BEGIN_NAMESPACE
namespace Ui { class SshProfilesDialog; }
QT_END_NAMESPACE

class SshProfileStore;
class QAbstractButton;
class QPushButton;

/**
 * SSH profile manager (Edit > SSH Profiles..., or the gear in the SSH connection bar).
 * Works on a copy of the store; OK/Apply write back via SshProfileStore::setProfiles() + save().
 *
 * Layout in SshProfilesDialog.ui: listProfiles (left) with buttons buttonNew, buttonDuplicate,
 * buttonDelete, buttonImport, buttonExport; the form on the right: editName, editHost,
 * spinPort (1..65535), editUser, comboAuth (SshProfile::authText), editIdentityFile +
 * buttonBrowseKey, checkSavePassword + editPassword (+ labelSecretNote from
 * SecretStore::storageDescription()), editRemoteCommand, editStartupCommand,
 * editTerminalType, spinKeepAlive (0..600 s), spinTimeout (1..120 s), checkCompression,
 * editKnownHosts + buttonBrowseKnownHosts, tableForwards (local port, remote host, remote
 * port, bind) with buttonAddForward/buttonRemoveForward, editDescription, and a buttonBox
 * with Ok | Cancel | Apply plus a custom "Connect" button (connectRequested(profile), then
 * accept()). Field changes update the selected profile immediately (in the working copy);
 * the list shows displayName() and re-sorts on rename. Password field: enabled only with
 * checkSavePassword; on Apply/OK the password is written to SecretStore (or removed when
 * unchecked). Validation: host required, port range, identity file must exist when Auth is
 * PublicKey; invalid fields are highlighted and OK/Apply/Connect stay disabled.
 *
 * Details of this implementation: the form is split over three tabs (Connection, Session,
 * Port forwarding) so it fits 800x600; editProxyJump is on the Session tab; the validation
 * summary is labelValidation above the buttons and covers every profile of the working copy.
 * Secrets go to SecretStore under "ssh/<id>/password", or "ssh/<id>/passphrase" when the
 * profile authenticates with a public key; the typed text is kept per profile until Apply/OK
 * and never enters the profile JSON. passwordSaved becomes true only once SecretStore::store()
 * succeeded; a failed store is reported in a message box and the typed secret stays pending
 * for the next Apply. Export writes the working copy (including unsaved edits).
 */
class SshProfilesDialog : public QDialog
{
    Q_OBJECT
public:
    explicit SshProfilesDialog(SshProfileStore* store, QWidget* parent = nullptr);
    ~SshProfilesDialog() override;

    void selectProfile(const QString& id);
    QString selectedProfileId() const;

public slots:
    void accept() override;

signals:
    void connectRequested(const SshProfile& profile);

private slots:
    void onNew();
    void onDuplicate();
    void onDelete();
    void onImport();
    void onExport();
    void onBrowseKey();
    void onBrowseKnownHosts();
    void onAddForward();
    void onRemoveForward();
    void onSelectionChanged();
    void onFieldChanged();
    void onButtonClicked(QAbstractButton* button);
    /// File-level import / export behind onImport() / onExport() (a QMessageBox reports errors).
    bool importFile(const QString& path);
    bool exportFile(const QString& path);

private:
    struct PendingSecret
    {
        bool save = false;   ///< checkSavePassword state
        QString text;        ///< typed password / passphrase (empty = keep what is stored)
    };

    void loadForm(const SshProfile& profile);
    SshProfile formToProfile() const;
    bool validate(QStringList* problems = nullptr) const;
    void applyToStore();
    /// Sort the working copy, rebuild the list and select `selectId` (the first profile when
    /// unknown); reloadForm = false keeps the form as it is (re-sort while typing a name).
    void rebuildList(const QString& selectId, bool reloadForm = true);
    void updateFieldStates();             ///< enabled states that depend on the auth method / checkbox
    void updateValidation();              ///< labelValidation, field highlights, button enabled states
    void onConnect();
    SshProfile* workingProfile(const QString& id);
    const SshProfile* workingProfile(const QString& id) const;
    QString uniqueName(const QString& base) const;

    Ui::SshProfilesDialog* ui;
    SshProfileStore* m_store;
    QList<SshProfile> m_working;
    QString m_currentId;
    bool m_loading = false;
    QPushButton* m_connectButton = nullptr;
    QHash<QString, PendingSecret> m_pendingSecrets;   ///< per profile id, applied by applyToStore()
    QStringList m_deletedIds;                         ///< their secrets are removed by applyToStore()
};
