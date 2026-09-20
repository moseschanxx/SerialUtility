#pragma once

#include <QDialog>

QT_BEGIN_NAMESPACE
namespace Ui { class AboutDialog; }
QT_END_NAMESPACE

/**
 * "About BuildAI Serial Utility": logo (:/icons/buildai_64.png), app name + version,
 * a short description (what the tool is for: Rockchip Linux consoles, U-Boot, MCU shells),
 * credits (Qt version, C++20) and a copyright / license text
 * ("Copyright © 2026 BuildAI. All rights reserved."). Modeless, like the reference tool.
 * Layout in AboutDialog.ui: labelLogo, labelAppName, textDescription (QTextBrowser,
 * openExternalLinks), textCredits, textLicense, buttonBox (Close).
 * Retranslates itself on QEvent::LanguageChange (it can stay open across a language switch).
 */
class AboutDialog : public QDialog
{
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);
    ~AboutDialog() override;

protected:
    void changeEvent(QEvent* event) override;   ///< LanguageChange -> retranslateUi + setupContent()

private:
    void setupContent();
    Ui::AboutDialog* ui;
};
