#pragma once

#include <QDialog>

QT_BEGIN_NAMESPACE
namespace Ui { class VersionDialog; }
QT_END_NAMESPACE

/**
 * "Version Information": application version + git hash + build date (from Version.h),
 * Qt runtime vs build version, compiler (MSVC _MSC_VER / GCC / Clang), build type,
 * operating system (QSysInfo::prettyProductName), CPU architecture, and a text browser
 * listing the Qt modules used (Core, Gui, Widgets, SerialPort) and available text codecs
 * (QStringConverter::availableCodecs()). A "Copy" button copies everything as plain text
 * for bug reports.
 * Layout in VersionDialog.ui: labelAppVersion, labelGitHash, labelBuildDate, labelQtVersion,
 * labelCompiler, labelBuildType, labelOS, labelArchitecture, textAdditionalInfo,
 * buttonCopy, buttonBox (Close).
 * Retranslates itself on QEvent::LanguageChange (it can stay open across a language switch).
 */
class VersionDialog : public QDialog
{
    Q_OBJECT
public:
    explicit VersionDialog(QWidget* parent = nullptr);
    ~VersionDialog() override;

    static QString compilerInfo();
    static QString buildType();
    QString plainTextReport() const;

protected:
    void changeEvent(QEvent* event) override;   ///< LanguageChange -> retranslateUi + setupVersionInfo()

private slots:
    void copyToClipboard();

private:
    void setupVersionInfo();
    Ui::VersionDialog* ui;
};
