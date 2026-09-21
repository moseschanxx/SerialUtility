cmake_minimum_required(VERSION 3.22)
# 3.22: ENVIRONMENT_MODIFICATION test property (tests/CMakeLists.txt puts Qt's bin dir on PATH for
# ctest on Windows). CMakePresets.json (schema v6) separately requires 3.25.

project(SerialUtility
    VERSION 0.1.0
    DESCRIPTION "BuildAI Serial Utility - a serial-port terminal for Rockchip Linux boards and MCU shells"
    HOMEPAGE_URL "https://build.ai"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(SU_BUILD_TESTS "Build the unit tests (Qt Test)" ON)
option(SU_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

set(SU_APP_OUTPUT_NAME "BuildAI-SerialUtility")

# ---------------------------------------------------------------------------
# Qt
# ---------------------------------------------------------------------------
# Qt location. Precedence: -DCMAKE_PREFIX_PATH / -DQt6_DIR on the command line (build.ps1,
# CI) > QT_ROOT in the environment (CMakePresets.json forwards it via $penv{}) > the default
# install on the Windows dev machines. Linux relies on the distro Qt (nothing to set).
if(NOT CMAKE_PREFIX_PATH AND NOT Qt6_DIR)
    if(DEFINED ENV{QT_ROOT} AND NOT "$ENV{QT_ROOT}" STREQUAL "")
        set(CMAKE_PREFIX_PATH "$ENV{QT_ROOT}")
    elseif(WIN32 AND EXISTS "D:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6")
        set(CMAKE_PREFIX_PATH "D:/Qt/6.8.3/msvc2022_64")
    endif()
endif()

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets SerialPort LinguistTools)
if(SU_BUILD_TESTS)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
endif()

qt_standard_project_setup()   # AUTOMOC / AUTOUIC, CMAKE_INCLUDE_CURRENT_DIR
# qt_standard_project_setup() does not enable AUTORCC; resources/resources.qrc is listed as a
# plain source of the executables (SerialUtility and the GUI tests), so it must be on for
# :/icons/* to exist at run time.
set(CMAKE_AUTORCC ON)

# ---------------------------------------------------------------------------
# Version header (git hash + project version)
# ---------------------------------------------------------------------------
set(APP_GIT_HASH "unknown")
find_package(Git QUIET)
if(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _su_git_hash
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _su_git_rc)
    if(_su_git_rc EQUAL 0 AND NOT _su_git_hash STREQUAL "")
        set(APP_GIT_HASH "${_su_git_hash}")
    endif()

    # Re-run the configure step whenever HEAD moves (commit/checkout/reset/merge append to the
    # reflog), so `cmake --build` / Qt Creator incremental builds never ship a stale hash.
    # Ask git for the path so worktree / submodule layouts (.git as a file) resolve correctly.
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --git-path logs/HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _su_git_reflog
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_su_git_reflog AND NOT IS_ABSOLUTE "${_su_git_reflog}")
        set(_su_git_reflog "${CMAKE_CURRENT_SOURCE_DIR}/${_su_git_reflog}")
    endif()
    if(EXISTS "${_su_git_reflog}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_su_git_reflog}")
    endif()
endif()
string(TIMESTAMP APP_BUILD_DATE "%Y-%m-%d" UTC)

set(SU_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(src/app/Version.h.in "${SU_GENERATED_DIR}/Version.h" @ONLY)

# ---------------------------------------------------------------------------
# Compiler options helper
# ---------------------------------------------------------------------------
function(su_set_compile_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive- /Zc:__cplusplus /EHsc /MP)
        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
        if(SU_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wshadow)
        if(SU_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
    target_compile_definitions(${target} PRIVATE QT_DEPRECATED_WARNINGS)
endfunction()

# ---------------------------------------------------------------------------
# su_core : UI-free logic (serial I/O, terminal emulation model, settings)
#           Shared by the application and the unit tests.
# ---------------------------------------------------------------------------
set(SU_CORE_SOURCES
    src/app/Logging.h
    src/app/Logging.cpp
    src/app/AppSettings.h
    src/app/AppSettings.cpp

    src/core/LineEnding.h
    src/core/LineEnding.cpp
    src/core/HexUtils.h
    src/core/HexUtils.cpp
    src/core/CommandHistory.h
    src/core/CommandHistory.cpp
    src/core/QuickCommand.h
    src/core/QuickCommand.cpp
    src/core/SerialPortEnumerator.h
    src/core/SerialPortEnumerator.cpp
    src/core/SerialConnection.h
    src/core/SerialConnection.cpp
    src/core/FileSender.h
    src/core/FileSender.cpp
    src/core/SessionLogger.h
    src/core/SessionLogger.cpp
    src/core/DeviceSimulator.h
    src/core/DeviceSimulator.cpp
    src/core/LogReplayer.h
    src/core/LogReplayer.cpp

    src/terminal/TerminalTypes.h
    src/terminal/CharWidth.h
    src/terminal/CharWidth.cpp
    src/terminal/TerminalScreen.h
    src/terminal/TerminalScreen.cpp
    src/terminal/AnsiParser.h
    src/terminal/AnsiParser.cpp
    src/terminal/TerminalTheme.h
    src/terminal/TerminalTheme.cpp
)

qt_add_library(su_core STATIC ${SU_CORE_SOURCES})
target_include_directories(su_core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${SU_GENERATED_DIR}")
target_link_libraries(su_core PUBLIC Qt6::Core Qt6::Gui Qt6::SerialPort)
su_set_compile_options(su_core)

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
# su_app: every widget and dialog, as a static library so the GUI test executables can
# link the real application code (tests/tst_terminalwidget, tst_sessionwidget, ...).
set(SU_APP_LIB_SOURCES
    src/terminal/TerminalWidget.h
    src/terminal/TerminalWidget.cpp

    src/ui/MainWindow.h
    src/ui/MainWindow.cpp
    src/ui/MainWindow.ui
    src/ui/SessionWidget.h
    src/ui/SessionWidget.cpp
    src/ui/ConnectionBar.h
    src/ui/ConnectionBar.cpp
    src/ui/CommandInput.h
    src/ui/CommandInput.cpp
    src/ui/QuickCommandBar.h
    src/ui/QuickCommandBar.cpp
    src/ui/SystemLogViewer.h
    src/ui/SystemLogViewer.cpp
    src/ui/HexDumpView.h
    src/ui/HexDumpView.cpp

    src/dialogs/AboutDialog.h
    src/dialogs/AboutDialog.cpp
    src/dialogs/AboutDialog.ui
    src/dialogs/VersionDialog.h
    src/dialogs/VersionDialog.cpp
    src/dialogs/VersionDialog.ui
    src/dialogs/PreferencesDialog.h
    src/dialogs/PreferencesDialog.cpp
    src/dialogs/PreferencesDialog.ui
    src/dialogs/QuickCommandsDialog.h
    src/dialogs/QuickCommandsDialog.cpp
    src/dialogs/QuickCommandsDialog.ui
    src/dialogs/SendFileDialog.h
    src/dialogs/SendFileDialog.cpp
    src/dialogs/SendFileDialog.ui
)

qt_add_library(su_app STATIC ${SU_APP_LIB_SOURCES})
target_link_libraries(su_app PUBLIC su_core Qt6::Widgets)
su_set_compile_options(su_app)

set(SU_APP_SOURCES
    src/main.cpp
    resources/resources.qrc
)
if(WIN32)
    list(APPEND SU_APP_SOURCES resources/resources.rc)
endif()

qt_add_executable(SerialUtility WIN32 MACOSX_BUNDLE ${SU_APP_SOURCES})
set_target_properties(SerialUtility PROPERTIES
    OUTPUT_NAME "${SU_APP_OUTPUT_NAME}"
    MACOSX_BUNDLE_BUNDLE_NAME "BuildAI Serial Utility"
    MACOSX_BUNDLE_GUI_IDENTIFIER "ai.build.serialutility"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
target_link_libraries(SerialUtility PRIVATE su_app)
su_set_compile_options(SerialUtility)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
set(SU_TRANSLATION_TARGETS SerialUtility)
if(SU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    # GUI tests that exercise the language menu / translated group names need the embedded .qm files too.
    list(APPEND SU_TRANSLATION_TARGETS tst_mainwindow tst_dialogs tst_sessionwidget)
endif()

# ---------------------------------------------------------------------------
# Translations (embedded as resources under :/translations/<lang>.qm)
# lupdate scans su_app + su_core + the executable; lrelease output is embedded into
# every target listed in SU_TRANSLATION_TARGETS.
# ---------------------------------------------------------------------------
set(SU_TS_FILES
    translations/zh_CN.ts
    translations/en_US.ts
)
qt_add_translations(
    TARGETS ${SU_TRANSLATION_TARGETS}
    SOURCE_TARGETS su_app su_core SerialUtility
    TS_FILES ${SU_TS_FILES}
    RESOURCE_PREFIX "/translations"
    LUPDATE_OPTIONS -no-obsolete -locations relative)

# Qt's own widget/dialog strings for Chinese, embedded so a deployed (windeployqt
# NO_TRANSLATIONS) build does not depend on a Qt SDK being present. qtbase_zh_CN.qm
# covers QMessageBox/QFileDialog/QInputDialog/QFontDialog/QColorDialog; nothing in
# QtSerialPort is translatable.
set(SU_QTBASE_QM "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}/qtbase_zh_CN.qm")
if(EXISTS "${SU_QTBASE_QM}")
    foreach(_t IN LISTS SU_TRANSLATION_TARGETS)
        qt_add_resources(${_t} "su_qtbase_translations"
            PREFIX "/translations"
            BASE   "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}"
            FILES  "${SU_QTBASE_QM}")
    endforeach()
else()
    message(WARNING "qtbase_zh_CN.qm not found in ${QT6_INSTALL_PREFIX}/${QT6_INSTALL_TRANSLATIONS}; "
                    "Qt dialogs will stay English in the Chinese UI")
endif()

# ---------------------------------------------------------------------------
# Install / deploy  (cmake --install <build> --prefix <dist>)
# ---------------------------------------------------------------------------
install(TARGETS SerialUtility
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin)
install(FILES README.md VERSION.txt DESTINATION .)

qt_generate_deploy_app_script(
    TARGET SerialUtility
    OUTPUT_SCRIPT su_deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
    NO_TRANSLATIONS)
# `cmake --install <build> --prefix dist` with a RELATIVE prefix (as CI does) keeps
# CMAKE_INSTALL_PREFIX relative inside the install script; CMake's own file(INSTALL)
# copes, but Qt's deploy support insists on an absolute prefix ("Given qt.conf path is
# not an absolute path"). Resolve it against the working directory of the install step.
install(CODE [[
    if(NOT IS_ABSOLUTE "${CMAKE_INSTALL_PREFIX}")
        file(REAL_PATH "${CMAKE_INSTALL_PREFIX}" CMAKE_INSTALL_PREFIX)
        message(STATUS "Install prefix resolved to ${CMAKE_INSTALL_PREFIX}")
    endif()
]])
if(WIN32)
    # Classic windeployqt layout: Qt plugins next to the executable
    # (bin/platforms/qwindows.dll, bin/styles/..., bin/imageformats/...) instead of
    # a sibling "plugins" directory. QtDeploySupport only sets a default when the
    # variable is undefined at install time, so this install(CODE) is the override hook.
    install(CODE "set(QT_DEPLOY_PLUGINS_DIR \"bin\")")
endif()
install(SCRIPT "${su_deploy_script}")

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "BuildAI Serial Utility ${PROJECT_VERSION} (${APP_GIT_HASH})")
message(STATUS "  Qt            : ${Qt6_VERSION} (${Qt6_DIR})")
message(STATUS "  Compiler      : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Build type    : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Tests         : ${SU_BUILD_TESTS}")
message(STATUS "")
