#include "dialogs/AboutDialog.h"
#include "ui_AboutDialog.h"

#include <QEvent>
#include <QPixmap>

#include "Version.h"
#include "app/Logging.h"

AboutDialog::AboutDialog(QWidget* parent)
    : QDialog(parent)
    , ui(new Ui::AboutDialog)
{
    ui->setupUi(this);

    setWindowTitle(tr("About %1").arg(QStringLiteral(APP_DISPLAY_NAME)));
    setModal(false);
    setAttribute(Qt::WA_DeleteOnClose, false);

    setupContent();
    qCDebug(lcUi) << "AboutDialog created";
}

AboutDialog::~AboutDialog()
{
    delete ui;
}

void AboutDialog::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::LanguageChange) {
        ui->retranslateUi(this);
        setWindowTitle(tr("About %1").arg(QStringLiteral(APP_DISPLAY_NAME)));
        setupContent();
    }
    QDialog::changeEvent(event);
}

void AboutDialog::setupContent()
{
    const QPixmap logo(QStringLiteral(":/icons/buildai_64.png"));
    if (!logo.isNull()) {
        ui->labelLogo->setPixmap(logo);
    }

    ui->labelAppName->setText(QStringLiteral("<b>%1</b><br/><span style=\"font-size:10pt; font-weight:normal;\">%2</span>")
                                  .arg(QStringLiteral(APP_DISPLAY_NAME), tr("Version %1").arg(QStringLiteral(APP_VERSION))));

    const QString description =
        tr("<p>A multi-tab serial-port terminal for the boards BuildAI works with every day.</p>"
           "<p>It speaks to <b>Rockchip Linux boards</b> (U-Boot prompt and Linux console at 115200 or "
           "1500000 baud), to <b>MCU firmware shells</b> (STM32, ESP32, RP2040 via CH34x, CP210x, FTDI or "
           "J-Link CDC adapters) and to anything else that talks over a UART.</p>"
           "<p><b>Highlights</b></p>"
           "<ul>"
           "<li>VT100 / xterm terminal emulation with colours, scrollback and CJK support</li>"
           "<li>Auto-reconnect when a board reboots or a USB adapter is re-plugged</li>"
           "<li>Quick commands, paced file send, hex view and session logging</li>"
           "<li>Live line-parameter changes, DTR/RTS control and BREAK</li>"
           "</ul>"
           "<p>Homepage: <a href=\"%1\">%1</a></p>")
            .arg(QStringLiteral(APP_HOMEPAGE));
    ui->textDescription->setHtml(description);

    const QString credits = tr("<p><b>Copyright &copy; 2026 BuildAI</b></p>"
                               "<p>Developed by the BuildAI engineering team as a replacement for ad-hoc use of "
                               "PuTTY, minicom and SecureCRT.</p>"
                               "<p><b>Built with:</b></p>"
                               "<ul>"
                               "<li>Qt %1 - cross-platform application framework "
                               "(Core, Gui, Widgets, SerialPort)</li>"
                               "<li>C++20 - modern C++ standard</li>"
                               "<li>CMake and Ninja - build system</li>"
                               "</ul>"
                               "<p>Terminal emulation follows the VT500-series parser described at "
                               "<a href=\"https://vt100.net/emu/dec_ansi_parser\">vt100.net</a>.</p>")
                                .arg(QStringLiteral(QT_VERSION_STR));
    ui->textCredits->setHtml(credits);

    const QString license = tr("<p><b>License</b></p>"
                               "<p>Copyright &copy; 2026 BuildAI. All rights reserved.</p>"
                               "<p>This software is provided to BuildAI staff and partners for developing, "
                               "testing and servicing BuildAI hardware. Redistribution outside BuildAI requires "
                               "written permission.</p>"
                               "<p>Qt is used under the terms of the GNU Lesser General Public License v3. "
                               "See <a href=\"https://www.qt.io/licensing\">qt.io/licensing</a>.</p>");
    ui->textLicense->setHtml(license);
}
