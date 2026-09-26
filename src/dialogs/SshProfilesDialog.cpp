#include "dialogs/SshProfilesDialog.h"
#include "ui_SshProfilesDialog.h"

#include <QAbstractButton>
#include <QAbstractItemModel>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QUuid>

#include <algorithm>
#include <utility>

#include "app/Logging.h"
#include "ssh/SecretStore.h"
#include "ssh/SshConnection.h"

namespace {

enum ForwardColumn { ColLocalPort = 0, ColRemoteHost, ColRemotePort, ColBindAddress, ColCount };

constexpr int kIdRole = Qt::UserRole;
const QColor kInvalidBase(0xFF, 0xE3, 0xE3);

/// Spin box editor (1..65535) for the port columns of the forwards table.
class PortDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        auto* spin = new QSpinBox(parent);
        spin->setRange(1, 65535);
        spin->setFrame(false);
        return spin;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override
    {
        if (auto* spin = qobject_cast<QSpinBox*>(editor)) {
            spin->setValue(index.data(Qt::EditRole).toInt());
        }
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override
    {
        if (auto* spin = qobject_cast<QSpinBox*>(editor)) {
            spin->interpretText();
            model->setData(index, spin->value(), Qt::EditRole);
        }
    }
};

/// Line edit without whitespace for the host columns; an emptied cell keeps its old value.
class HostDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget* createEditor(QWidget* parent, const QStyleOptionViewItem&, const QModelIndex&) const override
    {
        auto* edit = new QLineEdit(parent);
        edit->setFrame(false);
        edit->setValidator(new QRegularExpressionValidator(QRegularExpression(QStringLiteral("\\S+")), edit));
        return edit;
    }

    void setEditorData(QWidget* editor, const QModelIndex& index) const override
    {
        if (auto* edit = qobject_cast<QLineEdit*>(editor)) {
            edit->setText(index.data(Qt::EditRole).toString());
        }
    }

    void setModelData(QWidget* editor, QAbstractItemModel* model, const QModelIndex& index) const override
    {
        auto* edit = qobject_cast<QLineEdit*>(editor);
        if (!edit) {
            return;
        }
        const QString text = edit->text().trimmed();
        if (!text.isEmpty()) {
            model->setData(index, text, Qt::EditRole);
        }
    }
};

QString newProfileId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

/// Same order as SshProfileStore: display name, case-insensitive, then id.
bool lessByName(const SshProfile& a, const SshProfile& b)
{
    const int cmp = QString::compare(a.displayName(), b.displayName(), Qt::CaseInsensitive);
    if (cmp != 0) {
        return cmp < 0;
    }
    return a.id < b.id;
}

/// SecretStore key of a profile's secret: the password, or the key passphrase for public key auth.
QString secretKey(const SshProfile& profile)
{
    return QStringLiteral("ssh/%1/%2")
        .arg(profile.id, profile.auth == SshProfile::Auth::PublicKey ? QStringLiteral("passphrase")
                                                                     : QStringLiteral("password"));
}

QStringList allSecretKeys(const QString& id)
{
    return {QStringLiteral("ssh/%1/password").arg(id), QStringLiteral("ssh/%1/passphrase").arg(id)};
}

bool identityFileMissing(const SshProfile& profile)
{
    const QString file = profile.identityFile.trimmed();
    return !file.isEmpty() && !QFileInfo(file).isFile();
}

bool proxyJumpInvalid(const SshProfile& profile)
{
    if (profile.proxyJump.trimmed().isEmpty()) {
        return false;
    }
    SshProfile probe;
    return !SshProfile::parseTarget(profile.proxyJump, probe);
}

/// Why a profile cannot be used (empty when it can).
QStringList problemsOf(const SshProfile& profile)
{
    QStringList problems;
    if (profile.host.trimmed().isEmpty()) {
        problems << SshProfilesDialog::tr("Host is required.");
    }
    if (profile.port == 0) {
        problems << SshProfilesDialog::tr("Port must be between 1 and 65535.");
    }
    if (profile.auth == SshProfile::Auth::PublicKey && profile.identityFile.trimmed().isEmpty()) {
        problems << SshProfilesDialog::tr("An identity file is required for public key authentication.");
    } else if (identityFileMissing(profile)) {
        problems << SshProfilesDialog::tr("Identity file not found: %1")
                        .arg(QDir::toNativeSeparators(profile.identityFile.trimmed()));
    }
    if (proxyJumpInvalid(profile)) {
        problems << SshProfilesDialog::tr("Proxy jump must be [user@]host[:port].");
    }
    for (qsizetype i = 0; i < profile.localForwards.size(); ++i) {
        const SshLocalForward& fwd = profile.localForwards.at(i);
        if (fwd.localPort == 0 || fwd.remotePort == 0 || fwd.remoteHost.trimmed().isEmpty()) {
            problems << SshProfilesDialog::tr("Port forward %1 is incomplete.").arg(i + 1);
        }
    }
    return problems;
}

/// Light red background for a field that blocks OK/Apply.
void markInvalid(QWidget* widget, bool invalid)
{
    if (invalid) {
        QPalette palette = widget->palette();
        palette.setColor(QPalette::Base, kInvalidBase);
        palette.setColor(QPalette::Text, Qt::black);
        widget->setPalette(palette);
    } else {
        widget->setPalette(QPalette());   // back to the inherited palette
    }
}

/// Smaller, dimmed text for secondary information (where secrets are stored).
void makeSubtle(QLabel* label)
{
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * 0.9);
    label->setFont(font);
    QPalette palette = label->palette();
    palette.setColor(QPalette::WindowText, palette.color(QPalette::Disabled, QPalette::WindowText));
    label->setPalette(palette);
}

QTableWidgetItem* portItem(quint16 port)
{
    auto* item = new QTableWidgetItem;
    item->setData(Qt::EditRole, static_cast<int>(port));
    return item;
}

QTableWidgetItem* textItem(const QString& text)
{
    return new QTableWidgetItem(text);
}

QString documentsDirectory()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    return dir.isEmpty() ? QDir::homePath() : dir;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

SshProfilesDialog::SshProfilesDialog(SshProfileStore* store, QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::SshProfilesDialog)
    , m_store(store)
{
    ui->setupUi(this);
    setWindowTitle(tr("SSH Profiles"));

    for (const SshProfile::Auth auth :
         {SshProfile::Auth::Auto, SshProfile::Auth::PublicKey, SshProfile::Auth::Password,
          SshProfile::Auth::KeyboardInteractive, SshProfile::Auth::Agent}) {
        ui->comboAuth->addItem(SshProfile::authText(auth), static_cast<int>(auth));
    }
    ui->spinPort->setRange(1, 65535);
    ui->spinKeepAlive->setRange(0, 600);
    ui->spinKeepAlive->setSpecialValueText(tr("Off"));
    ui->spinTimeout->setRange(1, 120);
    ui->editTerminalType->setPlaceholderText(QStringLiteral("xterm-256color"));
    ui->editKnownHosts->setPlaceholderText(
        tr("Default: %1").arg(QDir::toNativeSeparators(SshConnection::defaultKnownHostsFile())));
    const QString localUser = SshConnection::localUserName();
    if (!localUser.isEmpty()) {
        ui->editUser->setPlaceholderText(tr("Local user name (%1)").arg(localUser));
    }

    const QString note = SecretStore::storageDescription();
    ui->labelSecretNote->setText(note);
    ui->labelSecretNote->setVisible(!note.isEmpty());
    ui->checkSavePassword->setToolTip(note);
    makeSubtle(ui->labelSecretNote);

    QPalette warning = ui->labelValidation->palette();
    warning.setColor(QPalette::WindowText, QColor(0xC0, 0x1C, 0x28));
    ui->labelValidation->setPalette(warning);
    ui->labelValidation->hide();

    // Port forwards
    ui->tableForwards->setColumnCount(ColCount);
    ui->tableForwards->setHorizontalHeaderLabels(
        {tr("Local port"), tr("Remote host"), tr("Remote port"), tr("Bind address")});
    ui->tableForwards->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->tableForwards->setItemDelegateForColumn(ColLocalPort, new PortDelegate(this));
    ui->tableForwards->setItemDelegateForColumn(ColRemotePort, new PortDelegate(this));
    ui->tableForwards->setItemDelegateForColumn(ColRemoteHost, new HostDelegate(this));
    ui->tableForwards->setItemDelegateForColumn(ColBindAddress, new HostDelegate(this));

    // Buttons
    m_connectButton = ui->buttonBox->addButton(tr("Connect"), QDialogButtonBox::ActionRole);
    m_connectButton->setObjectName(QStringLiteral("buttonConnect"));
    m_connectButton->setToolTip(tr("Save the profiles and open a session to the selected one"));
    m_connectButton->setAutoDefault(false);
    connect(ui->buttonBox, &QDialogButtonBox::clicked, this, &SshProfilesDialog::onButtonClicked);

    connect(ui->buttonNew, &QPushButton::clicked, this, &SshProfilesDialog::onNew);
    connect(ui->buttonDuplicate, &QPushButton::clicked, this, &SshProfilesDialog::onDuplicate);
    connect(ui->buttonDelete, &QPushButton::clicked, this, &SshProfilesDialog::onDelete);
    connect(ui->buttonImport, &QPushButton::clicked, this, &SshProfilesDialog::onImport);
    connect(ui->buttonExport, &QPushButton::clicked, this, &SshProfilesDialog::onExport);
    connect(ui->buttonBrowseKey, &QPushButton::clicked, this, &SshProfilesDialog::onBrowseKey);
    connect(ui->buttonBrowseKnownHosts, &QPushButton::clicked, this, &SshProfilesDialog::onBrowseKnownHosts);
    connect(ui->buttonAddForward, &QPushButton::clicked, this, &SshProfilesDialog::onAddForward);
    connect(ui->buttonRemoveForward, &QPushButton::clicked, this, &SshProfilesDialog::onRemoveForward);
    connect(ui->listProfiles, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem*, QListWidgetItem*) { onSelectionChanged(); });

    // Every field writes through to the working copy.
    for (QLineEdit* edit : {ui->editName, ui->editHost, ui->editUser, ui->editIdentityFile, ui->editPassword,
                            ui->editRemoteCommand, ui->editStartupCommand, ui->editTerminalType, ui->editProxyJump,
                            ui->editKnownHosts}) {
        connect(edit, &QLineEdit::textChanged, this, [this](const QString&) { onFieldChanged(); });
    }
    for (QSpinBox* spin : {ui->spinPort, ui->spinKeepAlive, ui->spinTimeout}) {
        connect(spin, &QSpinBox::valueChanged, this, [this](int) { onFieldChanged(); });
    }
    connect(ui->comboAuth, &QComboBox::currentIndexChanged, this, [this](int) { onFieldChanged(); });
    connect(ui->checkSavePassword, &QCheckBox::toggled, this, [this](bool) { onFieldChanged(); });
    connect(ui->checkCompression, &QCheckBox::toggled, this, [this](bool) { onFieldChanged(); });
    connect(ui->editDescription, &QPlainTextEdit::textChanged, this, [this]() { onFieldChanged(); });
    connect(ui->tableForwards, &QTableWidget::cellChanged, this, [this](int, int) { onFieldChanged(); });

    m_working = m_store ? m_store->profiles() : QList<SshProfile>();
    rebuildList(m_working.isEmpty() ? QString() : m_working.first().id);
}

SshProfilesDialog::~SshProfilesDialog()
{
    delete ui;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void SshProfilesDialog::selectProfile(const QString& id)
{
    for (int row = 0; row < ui->listProfiles->count(); ++row) {
        if (ui->listProfiles->item(row)->data(kIdRole).toString() == id) {
            ui->listProfiles->setCurrentRow(row);
            return;
        }
    }
}

QString SshProfilesDialog::selectedProfileId() const
{
    return m_currentId;
}

void SshProfilesDialog::accept()
{
    if (!validate()) {
        return;
    }
    applyToStore();
    QDialog::accept();
}

// ---------------------------------------------------------------------------
// List
// ---------------------------------------------------------------------------

SshProfile* SshProfilesDialog::workingProfile(const QString& id)
{
    if (id.isEmpty()) {
        return nullptr;
    }
    for (SshProfile& profile : m_working) {
        if (profile.id == id) {
            return &profile;
        }
    }
    return nullptr;
}

const SshProfile* SshProfilesDialog::workingProfile(const QString& id) const
{
    if (id.isEmpty()) {
        return nullptr;
    }
    for (const SshProfile& profile : m_working) {
        if (profile.id == id) {
            return &profile;
        }
    }
    return nullptr;
}

QString SshProfilesDialog::uniqueName(const QString& base) const
{
    const auto taken = [this](const QString& name) {
        return std::any_of(m_working.cbegin(), m_working.cend(), [&name](const SshProfile& p) {
            return QString::compare(p.displayName(), name, Qt::CaseInsensitive) == 0;
        });
    };
    if (!taken(base)) {
        return base;
    }
    for (int n = 2; n < 1000; ++n) {
        const QString candidate = QStringLiteral("%1 %2").arg(base).arg(n);
        if (!taken(candidate)) {
            return candidate;
        }
    }
    return base;
}

void SshProfilesDialog::rebuildList(const QString& selectId, bool reloadForm)
{
    const bool wasLoading = m_loading;
    m_loading = true;
    std::sort(m_working.begin(), m_working.end(), lessByName);
    ui->listProfiles->clear();
    int selectRow = -1;
    for (const SshProfile& profile : m_working) {
        auto* item = new QListWidgetItem(profile.displayName(), ui->listProfiles);
        item->setData(kIdRole, profile.id);
        item->setToolTip(profile.displayTarget());
        if (profile.id == selectId) {
            selectRow = ui->listProfiles->count() - 1;
        }
    }
    if (selectRow < 0 && ui->listProfiles->count() > 0) {
        selectRow = 0;
    }
    ui->listProfiles->setCurrentRow(selectRow);
    m_loading = wasLoading;
    if (reloadForm) {
        onSelectionChanged();
    }
}

void SshProfilesDialog::onSelectionChanged()
{
    if (m_loading) {
        return;
    }
    const QListWidgetItem* item = ui->listProfiles->currentItem();
    m_currentId = item ? item->data(kIdRole).toString() : QString();
    const SshProfile* profile = workingProfile(m_currentId);
    if (!profile) {
        m_currentId.clear();
        loadForm(SshProfile());
        ui->tabs->setEnabled(false);
    } else {
        ui->tabs->setEnabled(true);
        loadForm(*profile);
    }
    updateValidation();
}

// ---------------------------------------------------------------------------
// Form <-> working copy
// ---------------------------------------------------------------------------

void SshProfilesDialog::loadForm(const SshProfile& profile)
{
    const bool wasLoading = m_loading;
    m_loading = true;

    ui->editName->setText(profile.name);
    ui->editHost->setText(profile.host);
    ui->spinPort->setValue(profile.port == 0 ? 22 : profile.port);
    ui->editUser->setText(profile.user);
    const int authIndex = ui->comboAuth->findData(static_cast<int>(profile.auth));
    ui->comboAuth->setCurrentIndex(authIndex >= 0 ? authIndex : 0);
    ui->editIdentityFile->setText(QDir::toNativeSeparators(profile.identityFile));

    const bool hasPending = m_pendingSecrets.contains(profile.id);
    const PendingSecret pending = m_pendingSecrets.value(profile.id);
    ui->checkSavePassword->setChecked(hasPending ? pending.save : profile.passwordSaved);
    ui->editPassword->setText(pending.text);
    ui->editPassword->setPlaceholderText(profile.passwordSaved ? tr("(saved)")
                                                               : tr("Stored on Apply, never in the profile file"));

    ui->editRemoteCommand->setText(profile.remoteCommand);
    ui->editStartupCommand->setText(profile.startupCommand);
    ui->editTerminalType->setText(profile.terminalType);
    ui->spinKeepAlive->setValue(profile.keepAliveSeconds);
    ui->spinTimeout->setValue(profile.connectTimeoutSeconds);
    ui->editProxyJump->setText(profile.proxyJump);
    ui->checkCompression->setChecked(profile.compression);
    ui->editKnownHosts->setText(QDir::toNativeSeparators(profile.knownHostsFile));
    ui->editDescription->setPlainText(profile.description);

    ui->tableForwards->setRowCount(0);
    for (const SshLocalForward& fwd : profile.localForwards) {
        const int row = ui->tableForwards->rowCount();
        ui->tableForwards->insertRow(row);
        ui->tableForwards->setItem(row, ColLocalPort, portItem(fwd.localPort));
        ui->tableForwards->setItem(row, ColRemoteHost, textItem(fwd.remoteHost));
        ui->tableForwards->setItem(row, ColRemotePort, portItem(fwd.remotePort));
        ui->tableForwards->setItem(row, ColBindAddress, textItem(fwd.bindAddress));
    }

    m_loading = wasLoading;
    updateFieldStates();
}

SshProfile SshProfilesDialog::formToProfile() const
{
    SshProfile profile;
    if (const SshProfile* current = workingProfile(m_currentId)) {
        profile = *current;   // id, passwordSaved and lastUsed are not edited here
    }
    profile.name = ui->editName->text().trimmed();
    profile.host = ui->editHost->text().trimmed();
    profile.port = static_cast<quint16>(ui->spinPort->value());
    profile.user = ui->editUser->text().trimmed();
    profile.auth = static_cast<SshProfile::Auth>(ui->comboAuth->currentData().toInt());
    profile.identityFile = QDir::fromNativeSeparators(ui->editIdentityFile->text().trimmed());
    profile.remoteCommand = ui->editRemoteCommand->text().trimmed();
    profile.startupCommand = ui->editStartupCommand->text().trimmed();
    profile.terminalType = ui->editTerminalType->text().trimmed();
    if (profile.terminalType.isEmpty()) {
        profile.terminalType = QStringLiteral("xterm-256color");
    }
    profile.keepAliveSeconds = ui->spinKeepAlive->value();
    profile.connectTimeoutSeconds = ui->spinTimeout->value();
    profile.proxyJump = ui->editProxyJump->text().trimmed();
    profile.compression = ui->checkCompression->isChecked();
    profile.knownHostsFile = QDir::fromNativeSeparators(ui->editKnownHosts->text().trimmed());
    profile.description = ui->editDescription->toPlainText().trimmed();

    profile.localForwards.clear();
    const QTableWidget* table = ui->tableForwards;
    for (int row = 0; row < table->rowCount(); ++row) {
        const auto value = [table, row](int column) {
            const QTableWidgetItem* item = table->item(row, column);
            return item ? item->data(Qt::EditRole) : QVariant();
        };
        SshLocalForward fwd;
        const int local = value(ColLocalPort).toInt();
        const int remote = value(ColRemotePort).toInt();
        fwd.localPort = (local > 0 && local <= 65535) ? static_cast<quint16>(local) : 0;
        fwd.remotePort = (remote > 0 && remote <= 65535) ? static_cast<quint16>(remote) : 0;
        fwd.remoteHost = value(ColRemoteHost).toString().trimmed();
        fwd.bindAddress = value(ColBindAddress).toString().trimmed();
        if (fwd.bindAddress.isEmpty()) {
            fwd.bindAddress = QStringLiteral("127.0.0.1");
        }
        profile.localForwards.append(fwd);
    }
    return profile;
}

void SshProfilesDialog::onFieldChanged()
{
    if (m_loading) {
        return;
    }
    SshProfile* profile = workingProfile(m_currentId);
    if (!profile) {
        return;
    }
    *profile = formToProfile();
    const QString label = profile->displayName();
    const QString target = profile->displayTarget();
    profile = nullptr;   // a re-sort below may move the element

    PendingSecret pending;
    pending.save = ui->checkSavePassword->isChecked();
    pending.text = ui->editPassword->text();
    m_pendingSecrets.insert(m_currentId, pending);

    if (QListWidgetItem* item = ui->listProfiles->currentItem()) {
        if (item->text() != label) {
            rebuildList(m_currentId, false);   // renamed: the list re-sorts, the form stays as typed
        } else {
            item->setToolTip(target);
        }
    }
    updateFieldStates();
    updateValidation();
}

void SshProfilesDialog::updateFieldStates()
{
    const auto auth = static_cast<SshProfile::Auth>(ui->comboAuth->currentData().toInt());
    const bool keyAuth = (auth == SshProfile::Auth::PublicKey || auth == SshProfile::Auth::Auto);
    ui->editIdentityFile->setEnabled(keyAuth);
    ui->buttonBrowseKey->setEnabled(keyAuth);

    const bool secretAllowed = (auth != SshProfile::Auth::Agent);
    ui->checkSavePassword->setEnabled(secretAllowed);
    ui->checkSavePassword->setText(auth == SshProfile::Auth::PublicKey ? tr("&Save key passphrase:")
                                                                       : tr("&Save password:"));
    ui->editPassword->setEnabled(secretAllowed && ui->checkSavePassword->isChecked());
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

bool SshProfilesDialog::validate(QStringList* problems) const
{
    QStringList all;
    if (!m_currentId.isEmpty()) {
        all << problemsOf(formToProfile());
    }
    for (const SshProfile& other : m_working) {
        if (other.id == m_currentId) {
            continue;
        }
        const QStringList otherProblems = problemsOf(other);
        for (const QString& problem : otherProblems) {
            all << tr("%1: %2").arg(other.displayName(), problem);
        }
    }
    if (problems) {
        *problems = all;
    }
    return all.isEmpty();
}

void SshProfilesDialog::updateValidation()
{
    QStringList problems;
    const bool ok = validate(&problems);
    ui->labelValidation->setText(problems.join(QLatin1Char('\n')));
    ui->labelValidation->setVisible(!ok);

    if (!m_currentId.isEmpty()) {
        const SshProfile profile = formToProfile();
        markInvalid(ui->editHost, profile.host.isEmpty());
        markInvalid(ui->editIdentityFile,
                    (profile.auth == SshProfile::Auth::PublicKey && profile.identityFile.isEmpty())
                        || identityFileMissing(profile));
        markInvalid(ui->editProxyJump, proxyJumpInvalid(profile));
    } else {
        markInvalid(ui->editHost, false);
        markInvalid(ui->editIdentityFile, false);
        markInvalid(ui->editProxyJump, false);
    }

    const bool haveCurrent = !m_currentId.isEmpty();
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(ok);
    ui->buttonBox->button(QDialogButtonBox::Apply)->setEnabled(ok);
    m_connectButton->setEnabled(ok && haveCurrent);
    ui->buttonDuplicate->setEnabled(haveCurrent);
    ui->buttonDelete->setEnabled(haveCurrent);
    ui->buttonExport->setEnabled(!m_working.isEmpty());
    ui->buttonAddForward->setEnabled(haveCurrent);
    ui->buttonRemoveForward->setEnabled(haveCurrent && ui->tableForwards->rowCount() > 0);
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------

void SshProfilesDialog::applyToStore()
{
    if (!m_store) {
        return;
    }
    // Secrets: written / removed here, never part of the profile JSON. passwordSaved is only
    // set once SecretStore really holds the value; a failed store keeps the typed text pending
    // (so a later Apply retries) and is reported below.
    QStringList notStored;
    QHash<QString, PendingSecret> stillPending;
    for (SshProfile& profile : m_working) {
        const auto it = m_pendingSecrets.constFind(profile.id);
        if (it == m_pendingSecrets.constEnd()) {
            continue;
        }
        if (it->save) {
            if (!it->text.isEmpty()) {
                if (SecretStore::store(secretKey(profile), it->text)) {
                    profile.passwordSaved = true;
                } else {
                    qCWarning(lcSsh) << "could not store the secret of profile" << profile.displayName();
                    notStored.append(profile.displayName());
                    stillPending.insert(profile.id, *it);
                }
            }
            // Checked without typing anything: whatever is stored stays.
        } else {
            for (const QString& key : allSecretKeys(profile.id)) {
                SecretStore::remove(key);
            }
            profile.passwordSaved = false;
        }
    }
    for (const QString& id : std::as_const(m_deletedIds)) {
        for (const QString& key : allSecretKeys(id)) {
            SecretStore::remove(key);
        }
    }
    m_deletedIds.clear();
    m_pendingSecrets = stillPending;
    if (!notStored.isEmpty()) {
        QMessageBox::warning(this, tr("SSH Profiles"),
                             tr("The password could not be saved for: %1\n\n%2\nYou will be asked for it when connecting.")
                                 .arg(notStored.join(QLatin1String(", ")), SecretStore::storageDescription()));
    }

    m_store->setProfiles(m_working);
    if (!m_store->save()) {
        qCWarning(lcSsh) << "SSH profiles could not be saved to" << SshProfileStore::defaultFilePath();
        QMessageBox::warning(this, tr("SSH Profiles"),
                             tr("The profiles could not be saved to %1. The changes apply to this run only.")
                                 .arg(QDir::toNativeSeparators(SshProfileStore::defaultFilePath())));
    } else {
        qCInfo(lcSsh) << "SSH profiles saved:" << m_working.size() << "profile(s)";
    }
    m_working = m_store->profiles();
    rebuildList(m_currentId);
}

void SshProfilesDialog::onConnect()
{
    if (m_currentId.isEmpty() || !validate()) {
        return;
    }
    applyToStore();
    const SshProfile* profile = workingProfile(m_currentId);
    if (!profile) {
        return;
    }
    const SshProfile copy = *profile;
    qCInfo(lcSsh) << "connect requested from the profile manager:" << copy.displayName();
    emit connectRequested(copy);
    QDialog::accept();
}

void SshProfilesDialog::onButtonClicked(QAbstractButton* button)
{
    switch (ui->buttonBox->buttonRole(button)) {
    case QDialogButtonBox::AcceptRole:
        accept();
        break;
    case QDialogButtonBox::RejectRole:
        reject();
        break;
    case QDialogButtonBox::ApplyRole:
        if (validate()) {
            applyToStore();
        }
        break;
    case QDialogButtonBox::ActionRole:
        if (button == m_connectButton) {
            onConnect();
        }
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// List buttons
// ---------------------------------------------------------------------------

void SshProfilesDialog::onNew()
{
    SshProfile profile;
    profile.id = newProfileId();
    profile.name = uniqueName(tr("New profile"));
    m_working.append(profile);
    rebuildList(profile.id);
    ui->tabs->setCurrentWidget(ui->tabConnection);
    ui->editHost->setFocus(Qt::OtherFocusReason);
}

void SshProfilesDialog::onDuplicate()
{
    const SshProfile* current = workingProfile(m_currentId);
    if (!current) {
        return;
    }
    SshProfile copy = *current;
    copy.id = newProfileId();
    copy.name = uniqueName(tr("%1 (copy)").arg(current->displayName()));
    copy.passwordSaved = false;   // the secret is not duplicated
    copy.lastUsed = QDateTime();
    m_working.append(copy);
    rebuildList(copy.id);
}

void SshProfilesDialog::onDelete()
{
    if (m_currentId.isEmpty()) {
        return;
    }
    const QString id = m_currentId;
    const int row = ui->listProfiles->currentRow();
    m_working.erase(std::remove_if(m_working.begin(), m_working.end(),
                                   [&id](const SshProfile& p) { return p.id == id; }),
                    m_working.end());
    m_deletedIds.append(id);
    m_pendingSecrets.remove(id);

    QString next;
    if (!m_working.isEmpty()) {
        const int nextRow = qBound(0, row, static_cast<int>(m_working.size()) - 1);
        next = m_working.at(nextRow).id;   // m_working is in list order
    }
    rebuildList(next);
}

void SshProfilesDialog::onImport()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Import SSH Profiles"), documentsDirectory(),
                                                      tr("JSON files (*.json);;All files (*)"));
    if (!path.isEmpty()) {
        importFile(path);
    }
}

void SshProfilesDialog::onExport()
{
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Export SSH Profiles"), QDir(documentsDirectory()).filePath(QStringLiteral("ssh_profiles.json")),
        tr("JSON files (*.json);;All files (*)"));
    if (!path.isEmpty()) {
        exportFile(path);
    }
}

bool SshProfilesDialog::importFile(const QString& path)
{
    SshProfileStore imported;   // scratch store: parses and validates the file
    QString error;
    if (!imported.importFromFile(path, &error)) {
        qCWarning(lcSsh) << "SSH profile import failed:" << path << error;
        QMessageBox::warning(this, tr("Import SSH Profiles"), error);
        return false;
    }
    QString firstId;
    const QList<SshProfile> profiles = imported.profiles();
    for (const SshProfile& profile : profiles) {
        if (firstId.isEmpty()) {
            firstId = profile.id;
        }
        if (SshProfile* existing = workingProfile(profile.id)) {
            SshProfile merged = profile;
            merged.passwordSaved = existing->passwordSaved;   // the local secret survives a re-import
            *existing = merged;
        } else {
            m_working.append(profile);
        }
    }
    rebuildList(firstId);
    qCInfo(lcSsh) << "imported" << profiles.size() << "SSH profile(s) from" << path;
    return true;
}

bool SshProfilesDialog::exportFile(const QString& path)
{
    SshProfileStore scratch;
    scratch.setProfiles(m_working);
    QString error;
    if (!scratch.exportToFile(path, &error)) {
        qCWarning(lcSsh) << "SSH profile export failed:" << path << error;
        QMessageBox::warning(this, tr("Export SSH Profiles"), error);
        return false;
    }
    qCInfo(lcSsh) << "exported" << m_working.size() << "SSH profile(s) to" << path;
    return true;
}

// ---------------------------------------------------------------------------
// Form buttons
// ---------------------------------------------------------------------------

void SshProfilesDialog::onBrowseKey()
{
    QString start = QDir::homePath() + QStringLiteral("/.ssh");
    const QFileInfo current(QDir::fromNativeSeparators(ui->editIdentityFile->text().trimmed()));
    if (current.exists()) {
        start = current.absoluteFilePath();
    } else if (!current.filePath().isEmpty() && current.dir().exists()) {
        start = current.dir().absolutePath();
    } else if (!QDir(start).exists()) {
        start = QDir::homePath();
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Select Private Key File"), start, tr("All files (*)"));
    if (!path.isEmpty()) {
        ui->editIdentityFile->setText(QDir::toNativeSeparators(path));
    }
}

void SshProfilesDialog::onBrowseKnownHosts()
{
    QString start = QDir::homePath() + QStringLiteral("/.ssh");
    const QFileInfo current(QDir::fromNativeSeparators(ui->editKnownHosts->text().trimmed()));
    if (current.exists()) {
        start = current.absoluteFilePath();
    } else if (!current.filePath().isEmpty() && current.dir().exists()) {
        start = current.dir().absolutePath();
    } else if (!QDir(start).exists()) {
        start = QDir::homePath();
    }
    const QString path = QFileDialog::getOpenFileName(this, tr("Select known_hosts File"), start, tr("All files (*)"));
    if (!path.isEmpty()) {
        ui->editKnownHosts->setText(QDir::toNativeSeparators(path));
    }
}

void SshProfilesDialog::onAddForward()
{
    if (m_currentId.isEmpty()) {
        return;
    }
    const bool wasLoading = m_loading;
    m_loading = true;
    const int row = ui->tableForwards->rowCount();
    ui->tableForwards->insertRow(row);
    ui->tableForwards->setItem(row, ColLocalPort, portItem(8080));
    ui->tableForwards->setItem(row, ColRemoteHost, textItem(QStringLiteral("127.0.0.1")));
    ui->tableForwards->setItem(row, ColRemotePort, portItem(80));
    ui->tableForwards->setItem(row, ColBindAddress, textItem(QStringLiteral("127.0.0.1")));
    m_loading = wasLoading;
    ui->tableForwards->setCurrentCell(row, ColLocalPort);
    onFieldChanged();
}

void SshProfilesDialog::onRemoveForward()
{
    const int row = ui->tableForwards->currentRow();
    if (row < 0) {
        return;
    }
    ui->tableForwards->removeRow(row);
    onFieldChanged();
}
