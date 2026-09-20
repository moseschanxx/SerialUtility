#pragma once

#include <QDialog>
#include <QAbstractTableModel>
#include <QKeySequence>
#include <QList>

#include "core/QuickCommand.h"

QT_BEGIN_NAMESPACE
namespace Ui { class QuickCommandsDialog; }
QT_END_NAMESPACE

class QuickCommandStore;

/**
 * Editable table model over a QList<QuickCommand>. Columns: Name, Command, Group,
 * Line Ending (combo delegate text: LineEnding::displayName), HEX (checkbox), Escapes
 * (checkbox), Shortcut, Tooltip. Supports insert/remove/move rows.
 *
 * The Shortcut column accepts any single-stroke QKeySequence, but flags (tooltip, warning
 * colour and icon) sequences the focused, connected TerminalWidget consumes before the
 * application shortcut map runs; see isTerminalSafeShortcut().
 */
class QuickCommandModel : public QAbstractTableModel
{
    Q_OBJECT
public:
    enum Column { Name = 0, Command, Group, LineEndingCol, Hex, Escapes, Shortcut, Tooltip, ColumnCount };

    explicit QuickCommandModel(QObject* parent = nullptr);

    /**
     * True when `sequence` is a single key stroke that reaches the application shortcut map
     * even while a connected TerminalWidget has keyboard focus. Mirrors the terminal's
     * ShortcutOverride rules without depending on it: Meta+<anything> passes through;
     * F1-F12 and the editing/navigation keys (Tab, Escape, Return, arrows, Home/End, Insert,
     * Delete, Page Up/Down, Backspace) are always claimed; Alt+<key> becomes ESC + key;
     * plain and Shift+<key> are typed text; Ctrl+<letter> (without Shift), Ctrl+0 and the
     * Ctrl+<punctuation> control bytes / zoom keys are consumed; Ctrl+T, Ctrl+W, Ctrl+Comma
     * and Ctrl(+Shift)+Tab belong to MainWindow. Everything else - notably Ctrl+<digit 1-9>,
     * optionally with Shift - is safe.
     */
    static bool isTerminalSafeShortcut(const QKeySequence& sequence);

    QList<QuickCommand> commands() const;
    void setCommands(const QList<QuickCommand>& commands);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex& index, const QVariant& value, int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex& index) const override;

    void insertCommand(int row, const QuickCommand& command);
    void removeCommand(int row);
    bool moveCommand(int from, int to);   ///< to = destination row before move

private:
    QList<QuickCommand> m_commands;
};

/**
 * Quick command editor (Edit > Quick Commands...). Works on a copy of the store's list;
 * OK writes back via QuickCommandStore::setCommands() + save().
 * Buttons: Add, Remove, Move Up, Move Down, Import..., Export..., Restore Defaults,
 * buttonBox (Ok | Cancel). Table: tableView with the model above; the Line Ending column
 * uses a QComboBox delegate; HEX / Escapes are checkboxes.
 * Optionally preselect a row with setCurrentRow().
 */
class QuickCommandsDialog : public QDialog
{
    Q_OBJECT
public:
    explicit QuickCommandsDialog(QuickCommandStore* store, QWidget* parent = nullptr);
    ~QuickCommandsDialog() override;

    void setCurrentRow(int row);

public slots:
    void accept() override;

private slots:
    void onAdd();
    void onRemove();
    void onMoveUp();
    void onMoveDown();
    void onImport();
    void onExport();
    void onRestoreDefaults();
    void updateButtons();

private:
    Ui::QuickCommandsDialog* ui;
    QuickCommandStore* m_store;
    QuickCommandModel* m_model;
};
