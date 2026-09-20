#include "dialogs/VersionDialog.h"
#include "ui_VersionDialog.h"

#include <QApplication>
#include <QClipboard>
#include <QPushButton>
#include <QStringConverter>
#include <QSysInfo>
#include <QStringList>

#include "Version.h"
#include "app/Logging.h"

namespace {

struct VersionInfo
{
    QString appVersion;
    QString gitHash;
    QString buildDate;
    QString qtVersion;
    QString compiler;
    QString buildType;
    QString os;
    QString architecture;
    QString qtModules;
    QString codecs;
};

QString availableCodecNames()
{
    const QStringList codecs = QStringConverter::availableCodecs();
    return codecs.join(QStringLiteral(", "));
}

VersionInfo collectVersionInfo()
{
    VersionInfo info;
    info.appVersion = QStringLiteral(APP_VERSION);
    info.gitHash = QStringLiteral(APP_GIT_HASH);
    info.buildDate = QStringLiteral("%1 (%2 %3)").arg(QStringLiteral(APP_BUILD_DATE), QStringLiteral(__DATE__),
                                                     QStringLiteral(__TIME__));
    info.qtVersion = QStringLiteral("%1 (built with %2)").arg(QString::fromLatin1(qVersion()),
                                                             QStringLiteral(QT_VERSION_STR));
    info.compiler = VersionDialog::compilerInfo();
    info.buildType = VersionDialog::buildType();
    info.os = QSysInfo::prettyProductName();
    info.architecture = QStringLiteral("%1 (kernel %2 %3)").arg(QSysInfo::currentCpuArchitecture(),
                                                               QSysInfo::kernelType(), QSysInfo::kernelVersion());
    info.qtModules = QStringLiteral("Qt6::Core, Qt6::Gui, Qt6::Widgets, Qt6::SerialPort");
    info.codecs = availableCodecNames();
    return info;
}

QString richLine(const QString& label, const QString& value)
{
    return QStringLiteral("<b>%1</b> %2").arg(label, value.toHtmlEscaped());
}

} // namespace

VersionDialog::VersionDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::VersionDialog)
{
    ui->setupUi(this);

    setWindowTitle(tr("Version Information"));
    setModal(false);

    connect(ui->buttonCopy, &QPushButton::clicked, this, &VersionDialog::copyToClipboard);

    setupVersionInfo();
    qCDebug(lcUi) << "VersionDialog created";
}

VersionDialog::~VersionDialog()
{
    delete ui;
}

QString VersionDialog::compilerInfo()
{
#if defined(__clang__)
    return QStringLiteral("Clang %1.%2.%3").arg(__clang_major__).arg(__clang_minor__).arg(__clang_patchlevel__);
#elif defined(__GNUC__) || defined(__GNUG__)
    return QStringLiteral("GCC %1.%2.%3").arg(__GNUC__).arg(__GNUC_MINOR__).arg(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
    // _MSC_VER 1944 -> "19.44"; _MSC_FULL_VER carries the build number.
    return QStringLiteral("MSVC %1.%2 (_MSC_FULL_VER %3)")
        .arg(_MSC_VER / 100)
        .arg(_MSC_VER % 100, 2, 10, QLatin1Char('0'))
        .arg(_MSC_FULL_VER);
#else
    return QStringLiteral("Unknown compiler");
#endif
}

QString VersionDialog::buildType()
{
#if defined(QT_NO_DEBUG)
    return QStringLiteral("Release");
#else
    return QStringLiteral("Debug");
#endif
}

QString VersionDialog::plainTextReport() const
{
    const VersionInfo info = collectVersionInfo();
    QStringList lines;
    lines << QStringLiteral("%1 %2").arg(QStringLiteral(APP_DISPLAY_NAME), info.appVersion);
    lines << QStringLiteral("%1 %2").arg(tr("Application Version:"), info.appVersion);
    lines << QStringLiteral("%1 %2").arg(tr("Git Revision:"), info.gitHash);
    lines << QStringLiteral("%1 %2").arg(tr("Build Date:"), info.buildDate);
    lines << QStringLiteral("%1 %2").arg(tr("Qt Version:"), info.qtVersion);
    lines << QStringLiteral("%1 %2").arg(tr("Compiler:"), info.compiler);
    lines << QStringLiteral("%1 %2").arg(tr("Build Type:"), info.buildType);
    lines << QStringLiteral("%1 %2").arg(tr("Operating System:"), info.os);
    lines << QStringLiteral("%1 %2").arg(tr("Architecture:"), info.architecture);
    lines << QStringLiteral("%1 %2").arg(tr("Qt Modules:"), info.qtModules);
    lines << QStringLiteral("%1 %2").arg(tr("Available Codecs:"), info.codecs);
    return lines.join(QLatin1Char('\n'));
}

void VersionDialog::copyToClipboard()
{
    QClipboard* clipboard = QApplication::clipboard();
    if (clipboard == nullptr) {
        qCWarning(lcUi) << "No clipboard available";
        return;
    }
    clipboard->setText(plainTextReport());
    ui->buttonCopy->setText(tr("Copied"));
    qCInfo(lcUi) << "Version information copied to clipboard";
}

void VersionDialog::setupVersionInfo()
{
    const VersionInfo info = collectVersionInfo();

    ui->labelTitle->setText(QStringLiteral(APP_DISPLAY_NAME));
    ui->labelAppVersion->setText(richLine(tr("Application Version:"), info.appVersion));
    ui->labelGitHash->setText(richLine(tr("Git Revision:"), info.gitHash));
    ui->labelBuildDate->setText(richLine(tr("Build Date:"), info.buildDate));
    ui->labelQtVersion->setText(richLine(tr("Qt Version:"), info.qtVersion));
    ui->labelCompiler->setText(richLine(tr("Compiler:"), info.compiler));
    ui->labelBuildType->setText(richLine(tr("Build Type:"), info.buildType));
    ui->labelOS->setText(richLine(tr("Operating System:"), info.os));
    ui->labelArchitecture->setText(richLine(tr("Architecture:"), info.architecture));

    const QString additional = QStringLiteral("<h3>%1</h3><p>%2</p><h3>%3</h3><p>%4</p><h3>%5</h3><p>%6</p>")
                                   .arg(tr("Qt Modules"), info.qtModules.toHtmlEscaped(), tr("Available Text Codecs"),
                                        info.codecs.toHtmlEscaped(), tr("Description"),
                                        tr("Serial-port terminal for Rockchip Linux consoles, U-Boot and MCU shells. "
                                           "Use the Copy button to attach this information to a bug report."));
    ui->textAdditionalInfo->setHtml(additional);
}
