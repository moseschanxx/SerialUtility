cmake_minimum_required(VERSION 3.21)

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
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets SerialPort LinguistTools)
if(SU_BUILD_TESTS)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
endif()

qt_standard_project_setup()   # AUTOMOC / AUTOUIC / AUTORCC, CMAKE_INCLUDE_CURRENT_DIR

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
set(SU_APP_SOURCES
    src/main.cpp

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
target_link_libraries(SerialUtility PRIVATE su_core Qt6::Widgets)
su_set_compile_options(SerialUtility)

# ---------------------------------------------------------------------------
# Translations (embedded as resources under :/translations/<lang>.qm)
# ---------------------------------------------------------------------------
set(SU_TS_FILES
    translations/zh_CN.ts
    translations/en_US.ts
)
qt_add_translations(SerialUtility
    TS_FILES ${SU_TS_FILES}
    RESOURCE_PREFIX "/translations"
    LUPDATE_OPTIONS -no-obsolete -locations relative)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(SU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
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
