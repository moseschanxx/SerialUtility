#include "dialogs/QuickCommandsDialog.h"
#include "ui_QuickCommandsDialog.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFont>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeySequence>
#include <QMessageBox>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <utility>

#include "app/AppSettings.h"
#include "app/Logging.h"
#include "core/LineEnding.h"

// ---------------------------------------------------------------------------------------
// LineEndingDelegate - QComboBox editor for the Line Ending column
// ---------------------------------------------------------------------------------------
namespace {

class LineEndingDelegate : public QStyledItemDelegate
{
public:
    explicit LineEndingDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent)
    {
    }

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        Q_UNUSED(option)
        Q_UNUSED(index)
        auto* combo = new QComboBox(parent);
        const QList<LineEnding::Mode> modes = LineEnding::allModes();
        for (LineEnding::Mode mode : modes) {
            combo->addItem(LineEnding::displayName(mode), static_cast<int>(mode));
        }
        combo->setFrame(false);
        return combo;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override
    {
        auto* combo = qobject_cast<QComboBox*>(editor);
        if (combo == nullptr) {
            QStyledItemDelegate::setEditorData(editor, index);
            return;
        }
        const int mode = index.data(Qt::EditRole).toInt();
        const int item = combo->findData(mode);
        combo->setCurrentIndex(item >= 0 ? item : 0);
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override
    {
        auto* combo = qobject_cast<QComboBox*>(editor);
        if (combo == nullptr) {
            QStyledItemDelegate::setModelData(editor, model, index);
            return;
        }
        model->setData(index, combo->currentData(), Qt::EditRole);
    }

    void updateEditorGeometry(QWidget* editor, const QStyleOptionViewItem& option,
                              const QModelIndex& index) const override
    {
        Q_UNUSED(index)
        editor->setGeometry(option.rect);
    }
};

/// Parse a quick-command JSON file ({ "version": 1, "commands": [ ... ] }) into `out`.
bool readQuickCommandFile(const QString& path, QList<QuickCommand>& out, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error != nullptr) {
            *error = parseError.errorString();
        }
        return false;
    }

    QJsonArray array;
    if (doc.isObject()) {
        array = doc.object().value(QStringLiteral("commands")).toArray();
    } else if (doc.isArray()) {
        array = doc.array();
    }
    if (array.isEmpty()) {
        if (error != nullptr) {
            *error = QuickCommandsDialog::tr("The file contains no quick commands.");
        }
        return false;
    }

    out.clear();
    for (const QJsonValue& value : std::as_const(array)) {
        if (value.isObject()) {
            out.append(QuickCommand::fromJson(value.toObject()));
        }
    }
    return !out.isEmpty();
}

} // namespace

// ---------------------------------------------------------------------------------------
// QuickCommandModel
// ---------------------------------------------------------------------------------------
QuickCommandModel::QuickCommandModel(QObject* parent)
    : QAbstractTableModel(parent)
{
}

QList<QuickCommand> QuickCommandModel::commands() const
{
    return m_commands;
}

void QuickCommandModel::setCommands(const QList<QuickCommand>& commands)
{
    beginResetModel();
    m_commands = commands;
    endResetModel();
}

int QuickCommandModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_commands.size());
}

int QuickCommandModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : ColumnCount;
}

QVariant QuickCommandModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_commands.size()) {
        return {};
    }
    const QuickCommand& cmd = m_commands.at(index.row());

    switch (index.column()) {
    case Name:
        if (role == Qt::DisplayRole || role == Qt::EditRole) {
            return cmd.name;
        }
        if (role == Qt::ToolTipRole && !cmd.tooltip.isEmpty()) {
            return cmd.tooltip;
        }
        break;
    case Command:
        if (role == Qt::DisplayRole || role == Qt::EditRole) {
            return cmd.command;
        }
        if (role == Qt::FontRole) {
            QFont font;
            font.setStyleHint(QFont::Monospace);
            font.setFamily(QStringLiteral("Consolas"));
            font.setFixedPitch(true);
            return font;
        }
        if (role == Qt::ToolTipRole) {
            QString error;
            const QByteArray payload = cmd.payload(&error);
            if (!error.isEmpty()) {
                return tr("Error: %1").arg(error);
            }
            return tr("%n byte(s) will be sent", nullptr, static_cast<int>(payload.size()));
        }
        break;
    case Group:
        if (role == Qt::DisplayRole) {
            return cmd.group.isEmpty() ? tr("General") : cmd.group;
        }
        if (role == Qt::EditRole) {
            return cmd.group;
        }
        break;
    case LineEndingCol:
        if (role == Qt::DisplayRole) {
            return cmd.hex ? QStringLiteral("-") : LineEnding::displayName(cmd.lineEnding);
        }
        if (role == Qt::EditRole) {
            return static_cast<int>(cmd.lineEnding);
        }
        if (role == Qt::ToolTipRole && cmd.hex) {
            return tr("No line ending is appended to HEX commands.");
        }
        break;
    case Hex:
        if (role == Qt::CheckStateRole) {
            return cmd.hex ? Qt::Checked : Qt::Unchecked;
        }
        if (role == Qt::ToolTipRole) {
            return tr("Interpret the command as hex bytes, e.g. \"03\" or \"0x1B 0x5B\".");
        }
        if (role == Qt::TextAlignmentRole) {
            return static_cast<int>(Qt::AlignCenter);
        }
        break;
    case Escapes:
        if (role == Qt::CheckStateRole) {
            return cmd.escapes ? Qt::Checked : Qt::Unchecked;
        }
        if (role == Qt::ToolTipRole) {
            return tr("Interpret C-style escapes (\\n, \\r, \\t, \\xHH) in the command.");
        }
        if (role == Qt::TextAlignmentRole) {
            return static_cast<int>(Qt::AlignCenter);
        }
        break;
    case Shortcut:
        if (role == Qt::DisplayRole) {
            return cmd.shortcut.isEmpty() ? QString()
                                          : QKeySequence::fromString(cmd.shortcut, QKeySequence::PortableText)
                                                .toString(QKeySequence::NativeText);
        }
        if (role == Qt::EditRole) {
            return cmd.shortcut;
        }
        if (role == Qt::ToolTipRole) {
            return tr("Optional keyboard shortcut, e.g. Ctrl+1 or Alt+Shift+R.");
        }
        break;
    case Tooltip:
        if (role == Qt::DisplayRole || role == Qt::EditRole) {
            return cmd.tooltip;
        }
        break;
    default:
        break;
    }
    return {};
}

bool QuickCommandModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_commands.size()) {
        return false;
    }
    QuickCommand& cmd = m_commands[index.row()];
    bool changed = false;

    switch (index.column()) {
    case Name:
        if (role == Qt::EditRole) {
            const QString text = value.toString().trimmed();
            changed = (cmd.name != text);
            cmd.name = text;
        }
        break;
    case Command:
        if (role == Qt::EditRole) {
            const QString text = value.toString();
            changed = (cmd.command != text);
            cmd.command = text;
        }
        break;
    case Group:
        if (role == Qt::EditRole) {
            const QString text = value.toString().trimmed();
            changed = (cmd.group != text);
            cmd.group = text;
        }
        break;
    case LineEndingCol:
        if (role == Qt::EditRole) {
            LineEnding::Mode mode = cmd.lineEnding;
            bool ok = false;
            const int asInt = value.toInt(&ok);
            if (ok) {
                mode = static_cast<LineEnding::Mode>(asInt);
            } else {
                mode = LineEnding::fromString(value.toString());
            }
            changed = (cmd.lineEnding != mode);
            cmd.lineEnding = mode;
        }
        break;
    case Hex:
        if (role == Qt::CheckStateRole || role == Qt::EditRole) {
            const bool on = (role == Qt::CheckStateRole) ? (value.toInt() == Qt::Checked) : value.toBool();
            changed = (cmd.hex != on);
            cmd.hex = on;
            if (changed) {
                // The HEX flag changes how Command and Line Ending are displayed.
                emit dataChanged(index.siblingAtColumn(Command), index.siblingAtColumn(LineEndingCol));
            }
        }
        break;
    case Escapes:
        if (role == Qt::CheckStateRole || role == Qt::EditRole) {
            const bool on = (role == Qt::CheckStateRole) ? (value.toInt() == Qt::Checked) : value.toBool();
            changed = (cmd.escapes != on);
            cmd.escapes = on;
        }
        break;
    case Shortcut:
        if (role == Qt::EditRole) {
            const QString text = value.toString().trimmed();
            const QString portable =
                text.isEmpty() ? QString() : QKeySequence::fromString(text).toString(QKeySequence::PortableText);
            changed = (cmd.shortcut != portable);
            cmd.shortcut = portable;
        }
        break;
    case Tooltip:
        if (role == Qt::EditRole) {
            const QString text = value.toString();
            changed = (cmd.tooltip != text);
            cmd.tooltip = text;
        }
        break;
    default:
        return false;
    }

    if (changed) {
        emit dataChanged(index, index);
    }
    return true;
}

QVariant QuickCommandModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Vertical) {
        return (role == Qt::DisplayRole) ? QVariant(section + 1) : QVariant();
    }
    if (role != Qt::DisplayRole) {
        return {};
    }
    switch (section) {
    case Name:
        return tr("Name");
    case Command:
        return tr("Command");
    case Group:
        return tr("Group");
    case LineEndingCol:
        return tr("Line Ending");
    case Hex:
        return tr("HEX");
    case Escapes:
        return tr("Escapes");
    case Shortcut:
        return tr("Shortcut");
    case Tooltip:
        return tr("Tooltip");
    default:
        return {};
    }
}

Qt::ItemFlags QuickCommandModel::flags(const QModelIndex& index) const
{
    if (!index.isValid()) {
        return Qt::NoItemFlags;
    }
    Qt::ItemFlags result = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    if (index.column() == Hex || index.column() == Escapes) {
        result |= Qt::ItemIsUserCheckable;
    } else {
        result |= Qt::ItemIsEditable;
    }
    return result;
}

void QuickCommandModel::insertCommand(int row, const QuickCommand& command)
{
    const int count = static_cast<int>(m_commands.size());
    const int at = qBound(0, row, count);
    beginInsertRows(QModelIndex(), at, at);
    m_commands.insert(at, command);
    endInsertRows();
}

void QuickCommandModel::removeCommand(int row)
{
    if (row < 0 || row >= m_commands.size()) {
        return;
    }
    beginRemoveRows(QModelIndex(), row, row);
    m_commands.removeAt(row);
    endRemoveRows();
}

bool QuickCommandModel::moveCommand(int from, int to)
{
    const int count = static_cast<int>(m_commands.size());
    if (from < 0 || from >= count || to < 0 || to >= count || from == to) {
        return false;
    }
    // beginMoveRows() wants the destination expressed as "insert before this row" in
    // pre-move numbering: moving down means one past the target row.
    const int destination = (to > from) ? to + 1 : to;
    if (!beginMoveRows(QModelIndex(), from, from, QModelIndex(), destination)) {
        return false;
    }
    m_commands.move(from, to);
    endMoveRows();
    return true;
}

// ---------------------------------------------------------------------------------------
// QuickCommandsDialog
// ---------------------------------------------------------------------------------------
QuickCommandsDialog::QuickCommandsDialog(QuickCommandStore* store, QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::QuickCommandsDialog)
    , m_store(store)
    , m_model(new QuickCommandModel(this))
{
    ui->setupUi(this);
    setWindowTitle(tr("Quick Commands"));

    if (m_store != nullptr) {
        m_model->setCommands(m_store->commands());
    } else {
        qCWarning(lcUi) << "QuickCommandsDialog created without a store; changes cannot be saved";
    }

    ui->tableView->setModel(m_model);
    ui->tableView->setItemDelegateForColumn(QuickCommandModel::LineEndingCol, new LineEndingDelegate(this));
    ui->tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableView->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->tableView->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed
                                   | QAbstractItemView::AnyKeyPressed);
    ui->tableView->verticalHeader()->setVisible(false);
    ui->tableView->verticalHeader()->setDefaultSectionSize(ui->tableView->fontMetrics().height() + 8);

    QHeaderView* header = ui->tableView->horizontalHeader();
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(QuickCommandModel::Command, QHeaderView::Stretch);
    header->setSectionResizeMode(QuickCommandModel::Hex, QHeaderView::ResizeToContents);
    header->setSectionResizeMode(QuickCommandModel::Escapes, QHeaderView::ResizeToContents);
    header->setStretchLastSection(false);
    ui->tableView->setColumnWidth(QuickCommandModel::Name, 130);
    ui->tableView->setColumnWidth(QuickCommandModel::Group, 90);
    ui->tableView->setColumnWidth(QuickCommandModel::LineEndingCol, 100);
    ui->tableView->setColumnWidth(QuickCommandModel::Shortcut, 90);
    ui->tableView->setColumnWidth(QuickCommandModel::Tooltip, 160);

    connect(ui->addButton, &QPushButton::clicked, this, &QuickCommandsDialog::onAdd);
    connect(ui->removeButton, &QPushButton::clicked, this, &QuickCommandsDialog::onRemove);
    connect(ui->moveUpButton, &QPushButton::clicked, this, &QuickCommandsDialog::onMoveUp);
    connect(ui->moveDownButton, &QPushButton::clicked, this, &QuickCommandsDialog::onMoveDown);
    connect(ui->importButton, &QPushButton::clicked, this, &QuickCommandsDialog::onImport);
    connect(ui->exportButton, &QPushButton::clicked, this, &QuickCommandsDialog::onExport);
    connect(ui->restoreDefaultsButton, &QPushButton::clicked, this, &QuickCommandsDialog::onRestoreDefaults);

    connect(ui->tableView->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex&, const QModelIndex&) { updateButtons(); });
    connect(m_model, &QAbstractItemModel::rowsInserted, this, &QuickCommandsDialog::updateButtons);
    connect(m_model, &QAbstractItemModel::rowsRemoved, this, &QuickCommandsDialog::updateButtons);
    connect(m_model, &QAbstractItemModel::rowsMoved, this, &QuickCommandsDialog::updateButtons);
    connect(m_model, &QAbstractItemModel::modelReset, this, &QuickCommandsDialog::updateButtons);

    if (m_model->rowCount() > 0) {
        setCurrentRow(0);
    }
    updateButtons();
}

QuickCommandsDialog::~QuickCommandsDialog()
{
    delete ui;
}

void QuickCommandsDialog::setCurrentRow(int row)
{
    if (row < 0 || row >= m_model->rowCount()) {
        return;
    }
    const QModelIndex index = m_model->index(row, QuickCommandModel::Name);
    ui->tableView->setCurrentIndex(index);
    ui->tableView->selectRow(row);
    ui->tableView->scrollTo(index);
    updateButtons();
}

void QuickCommandsDialog::accept()
{
    // Commit an editor that may still be open on the current cell (focus change closes it).
    if (QWidget* focused = QApplication::focusWidget(); focused != nullptr && focused != ui->tableView
                                                          && ui->tableView->isAncestorOf(focused)) {
        ui->tableView->setFocus(Qt::OtherFocusReason);
    }

    if (m_store != nullptr) {
        m_store->setCommands(m_model->commands());
        if (!m_store->save()) {
            qCWarning(lcUi) << "Failed to save quick commands to" << QuickCommandStore::defaultFilePath();
            QMessageBox::warning(this, tr("Quick Commands"),
                                 tr("The quick commands could not be saved to\n%1\n\nThe changes are applied for "
                                    "this session only.")
                                     .arg(QDir::toNativeSeparators(QuickCommandStore::defaultFilePath())));
        } else {
            qCInfo(lcUi) << "Saved" << m_model->rowCount() << "quick commands";
        }
    }
    QDialog::accept();
}

void QuickCommandsDialog::onAdd()
{
    const QModelIndex current = ui->tableView->currentIndex();
    const int row = current.isValid() ? current.row() + 1 : m_model->rowCount();

    QuickCommand cmd;
    cmd.name = tr("New command");
    if (current.isValid()) {
        cmd.group = m_model->commands().at(current.row()).group;
    }
    m_model->insertCommand(row, cmd);
    setCurrentRow(row);
    ui->tableView->edit(m_model->index(row, QuickCommandModel::Name));
}

void QuickCommandsDialog::onRemove()
{
    const QModelIndex current = ui->tableView->currentIndex();
    if (!current.isValid()) {
        return;
    }
    const int row = current.row();
    m_model->removeCommand(row);
    const int count = m_model->rowCount();
    if (count > 0) {
        setCurrentRow(qMin(row, count - 1));
    }
    updateButtons();
}

void QuickCommandsDialog::onMoveUp()
{
    const QModelIndex current = ui->tableView->currentIndex();
    if (!current.isValid() || current.row() == 0) {
        return;
    }
    const int row = current.row();
    if (m_model->moveCommand(row, row - 1)) {
        setCurrentRow(row - 1);
    }
}

void QuickCommandsDialog::onMoveDown()
{
    const QModelIndex current = ui->tableView->currentIndex();
    if (!current.isValid() || current.row() >= m_model->rowCount() - 1) {
        return;
    }
    const int row = current.row();
    if (m_model->moveCommand(row, row + 1)) {
        setCurrentRow(row + 1);
    }
}

void QuickCommandsDialog::onImport()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Import Quick Commands"), AppSettings::dataDirectory(),
                                                      tr("Quick command files (*.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }

    QList<QuickCommand> imported;
    QString error;
    if (!readQuickCommandFile(path, imported, &error)) {
        qCWarning(lcUi) << "Quick command import failed:" << path << error;
        QMessageBox::warning(this, tr("Import Quick Commands"),
                             tr("Could not import quick commands from\n%1\n\n%2").arg(QDir::toNativeSeparators(path), error));
        return;
    }

    // Append, skipping exact duplicates of commands already in the table.
    const QList<QuickCommand> existing = m_model->commands();
    int added = 0;
    int firstNewRow = -1;
    for (const QuickCommand& cmd : std::as_const(imported)) {
        if (existing.contains(cmd)) {
            continue;
        }
        const int row = m_model->rowCount();
        m_model->insertCommand(row, cmd);
        if (firstNewRow < 0) {
            firstNewRow = row;
        }
        ++added;
    }
    if (firstNewRow >= 0) {
        setCurrentRow(firstNewRow);
    }
    qCInfo(lcUi) << "Imported" << added << "quick commands from" << path;
    QMessageBox::information(this, tr("Import Quick Commands"),
                             tr("%n command(s) imported.", nullptr, added)
                                 + (imported.size() > added
                                        ? QLatin1Char(' ')
                                              + tr("%n duplicate(s) skipped.", nullptr,
                                                   static_cast<int>(imported.size()) - added)
                                        : QString()));
}

void QuickCommandsDialog::onExport()
{
    const QString suggested = QDir(AppSettings::dataDirectory()).filePath(QStringLiteral("quick_commands.json"));
    QString path = QFileDialog::getSaveFileName(this, tr("Export Quick Commands"), suggested,
                                                tr("Quick command files (*.json);;All files (*)"));
    if (path.isEmpty()) {
        return;
    }
    if (!path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) && !path.contains(QLatin1Char('.'))) {
        path += QStringLiteral(".json");
    }

    QuickCommandStore temp;
    temp.setCommands(m_model->commands());
    QString error;
    if (!temp.exportToFile(path, &error)) {
        qCWarning(lcUi) << "Quick command export failed:" << path << error;
        QMessageBox::warning(this, tr("Export Quick Commands"),
                             tr("Could not export quick commands to\n%1\n\n%2").arg(QDir::toNativeSeparators(path), error));
        return;
    }
    qCInfo(lcUi) << "Exported" << m_model->rowCount() << "quick commands to" << path;
}

void QuickCommandsDialog::onRestoreDefaults()
{
    const auto answer = QMessageBox::question(
        this, tr("Restore Default Quick Commands"),
        tr("Replace the current list with the built-in default quick commands?\n\nNothing is saved until you press OK."),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes) {
        return;
    }
    m_model->setCommands(QuickCommandStore::defaults());
    if (m_model->rowCount() > 0) {
        setCurrentRow(0);
    }
    updateButtons();
}

void QuickCommandsDialog::updateButtons()
{
    const QModelIndex current = ui->tableView->currentIndex();
    const int count = m_model->rowCount();
    const bool hasCurrent = current.isValid() && current.row() >= 0 && current.row() < count;

    ui->removeButton->setEnabled(hasCurrent);
    ui->moveUpButton->setEnabled(hasCurrent && current.row() > 0);
    ui->moveDownButton->setEnabled(hasCurrent && current.row() < count - 1);
    ui->exportButton->setEnabled(count > 0);
}
