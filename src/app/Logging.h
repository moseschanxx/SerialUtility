#pragma once

#include <QLoggingCategory>

// Logging categories used throughout the application.
//
// Usage:   qCInfo(lcSerial) << "opened" << portName;
//          qCWarning(lcTerminal) << "unknown CSI" << final;
//
// All Qt messages (qDebug/qInfo/qWarning/qCritical, categorised or not) are routed by
// SystemLogViewer::installMessageHandler() into the in-app "System Log" dock as well as
// to the default handler (stderr / debugger output). Category names are prefixed with
// "buildai." so they can be filtered with QT_LOGGING_RULES, e.g.
//     QT_LOGGING_RULES="buildai.terminal.debug=true"

Q_DECLARE_LOGGING_CATEGORY(lcApp)       // "buildai.app"      application lifecycle, settings
Q_DECLARE_LOGGING_CATEGORY(lcSerial)    // "buildai.serial"   port enumeration, open/close, errors
Q_DECLARE_LOGGING_CATEGORY(lcTerminal)  // "buildai.terminal" parser / screen model
Q_DECLARE_LOGGING_CATEGORY(lcUi)        // "buildai.ui"       widgets, dialogs
