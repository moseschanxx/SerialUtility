#pragma once

#include <QWidget>
#include <QList>

#include "core/QuickCommand.h"

class QComboBox;
class QToolButton;
class QScrollArea;
class QHBoxLayout;

/**
 * A row of macro buttons built from a QuickCommandStore:
 *
 *  [Group: Linux v] [uname -a] [cpuinfo] [meminfo] [df -h] [ifconfig] [dmesg] ...   [⚙]
 *
 * - The group combo filters buttons ("All" + QuickCommandStore::groups()); the last chosen
 *   group is remembered in QSettings ("ui/quickCommandGroup").
 * - Buttons show QuickCommand::name, tooltip = command (escaped for display) + line ending.
 *   Click -> commandTriggered(command). Middle-click or Alt+click -> editRequested() as well.
 * - Buttons with a `shortcut` get that QKeySequence (application-wide via the button's
 *   shortcut; conflicts are simply ignored by Qt).
 * - Buttons live in a horizontally scrollable QScrollArea (no vertical growth) so many
 *   commands remain usable in a narrow window.
 * - The gear button -> editRequested() (MainWindow opens QuickCommandsDialog).
 * - rebuild() runs on store->changed().
 * - setEnabledForConnection(false) disables the command buttons (not the group combo / gear).
 */
class QuickCommandBar : public QWidget
{
    Q_OBJECT
public:
    explicit QuickCommandBar(QuickCommandStore* store, QWidget* parent = nullptr);

    QuickCommandStore* store() const;
    QString currentGroup() const;          ///< "" = All
    void setCurrentGroup(const QString& group);
    void setEnabledForConnection(bool connected);

public slots:
    void rebuild();

signals:
    void commandTriggered(const QuickCommand& command);
    void editRequested();

protected:
    void changeEvent(QEvent* event) override;

private:
    void setupUi();
    void retranslate();

    QuickCommandStore* m_store;
    QComboBox* m_groupCombo = nullptr;
    QScrollArea* m_scrollArea = nullptr;
    QWidget* m_buttonHost = nullptr;
    QHBoxLayout* m_buttonLayout = nullptr;
    QToolButton* m_editButton = nullptr;
    QList<QToolButton*> m_buttons;
    bool m_connected = false;
};
