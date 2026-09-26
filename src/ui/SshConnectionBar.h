#pragma once

#include <QWidget>

#include "core/Transport.h"
#include "ssh/SshProfile.h"

class QComboBox;
class QToolButton;
class QPushButton;
class QLabel;
class SshProfileStore;

/**
 * The strip above the terminal of an SSH session:
 *
 *  [Target: Luckfox Pico (root@192.168.100.2) v][⚙] [🔒 ssh-ed25519 · publickey]      [● Connect]
 *
 * - The editable target combo lists the store's profiles ("name - user@host:port", item data =
 *   profile id) followed by a separator and the recent ad-hoc targets. Free text is parsed
 *   with SshProfile::parseTarget() into an ad-hoc profile (no id, Auth::Auto) when Connect is
 *   pressed or Enter is typed; invalid text turns the field red with a tooltip.
 * - currentProfile() returns the selected stored profile (a copy) or the ad-hoc one.
 * - The gear button -> profilesEditRequested() (MainWindow opens SshProfilesDialog).
 * - Connect button: green dot + "Connect" when disconnected, amber "Connecting..." /
 *   "Reconnecting..." (clickable = cancel), red "Disconnect" when connected. While not
 *   Disconnected the combo and gear are disabled.
 * - The info label shows Transport::summary() when connected (host key type · auth method).
 * - setStore(): rebuilds on store->changed(), keeping the current selection when possible.
 * - profileChanged() is emitted for user changes only (popup selection, completer, Enter or
 *   focus loss with a valid target) and never twice in a row for the same target; programmatic
 *   selectProfile() / setTarget() / rebuilds are silent, like ConnectionBar::setSettings().
 */
class SshConnectionBar : public QWidget
{
    Q_OBJECT
public:
    explicit SshConnectionBar(QWidget* parent = nullptr);

    void setStore(SshProfileStore* store);      ///< not owned
    SshProfileStore* store() const;

    SshProfile currentProfile() const;
    bool hasValidTarget() const;
    void selectProfile(const QString& id);      ///< no-op when unknown
    void setTarget(const QString& text);        ///< ad-hoc "user@host:port"
    QString targetText() const;
    void setConnectionState(Transport::State state);
    void setSummary(const QString& text);
    void setFocusToTarget();

signals:
    void connectRequested();
    void disconnectRequested();
    void profilesEditRequested();
    void profileChanged(const SshProfile& profile);   ///< selection or text changed to a valid target

protected:
    void changeEvent(QEvent* event) override;

private:
    void setupUi();
    void retranslate();
    void rebuild();
    void updateTargetValidity();          ///< red text / tooltip / connect dot for the current text
    void emitProfileChangedIfValid();     ///< user change: emit profileChanged() unless it repeats the last one
    QString selectedStoredId() const;     ///< id of the stored profile item whose text is currently shown, else ""

    SshProfileStore* m_store = nullptr;
    QComboBox* m_targetCombo = nullptr;
    QToolButton* m_profilesButton = nullptr;
    QLabel* m_targetLabel = nullptr;
    QLabel* m_infoLabel = nullptr;
    QPushButton* m_connectButton = nullptr;
    Transport::State m_state = Transport::State::Disconnected;
    bool m_updating = false;
    QString m_summary;                    ///< last setSummary() text (shown while not Disconnected)
    QString m_lastEmittedKey;             ///< "id:<id>" or "target:<user@host:port>" of the last profileChanged()
    bool m_hasEmitted = false;            ///< reset by programmatic updates so the next user change is reported
    QMetaObject::Connection m_storeConnection;
};
