cmake_minimum_required(VERSION 3.22)
# 3.22: ENVIRONMENT_MODIFICATION test property (tests/CMakeLists.txt puts Qt's bin dir on PATH for
# ctest on Windows). CMakePresets.json (schema v6) separately requires 3.25.

project(SerialUtility
    VERSION 0.7.0
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

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Svg SerialPort Network LinguistTools)
if(SU_BUILD_TESTS)
    find_package(Qt6 6.8 REQUIRED COMPONENTS Test)
endif()

qt_standard_project_setup()   # AUTOMOC / AUTOUIC, CMAKE_INCLUDE_CURRENT_DIR
# qt_standard_project_setup() does not enable AUTORCC; resources/resources.qrc is listed as a
# plain source of the executables (SerialUtility and the GUI tests), so it must be on for
# :/icons/* to exist at run time.
set(CMAKE_AUTORCC ON)

# ---------------------------------------------------------------------------
# libssh 0.11.1 (SSH sessions), vendored via FetchContent, OpenSSL crypto backend.
# Built as a shared library (ssh.dll / libssh.so.4) that lives next to the application;
# OpenSSL's libcrypto is its only external runtime dependency and is deployed alongside.
# ---------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL ${OPENSSL_VERSION}: ${OPENSSL_CRYPTO_LIBRARY}")

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)   # URL downloads: file timestamps = extraction time
endif()
set(SU_LIBSSH_VERSION 0.11.1)
set(SU_LIBSSH_URL "https://www.libssh.org/files/0.11/libssh-${SU_LIBSSH_VERSION}.tar.xz")
set(SU_LIBSSH_SHA256 14b7dcc72e91e08151c58b981a7b570ab2663f630e7d2837645d5a9c612c1b79)
# libssh's options are cache variables; FORCE so a reconfigure can never flip them. Our own
# libraries say STATIC explicitly, so BUILD_SHARED_LIBS only affects libssh.
set(BUILD_SHARED_LIBS ON CACHE BOOL "" FORCE)
set(WITH_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WITH_GSSAPI OFF CACHE BOOL "" FORCE)
set(WITH_ZLIB OFF CACHE BOOL "" FORCE)
set(WITH_NACL OFF CACHE BOOL "" FORCE)
set(WITH_PCAP OFF CACHE BOOL "" FORCE)
set(WITH_SERVER ON CACHE BOOL "" FORCE)     # the in-process test server (tests/support)
set(WITH_SFTP ON CACHE BOOL "" FORCE)
set(UNIT_TESTING OFF CACHE BOOL "" FORCE)
set(WITH_SYMBOL_VERSIONING OFF CACHE BOOL "" FORCE)
# libssh installs itself under our prefix: keep lib/ (not lib64/ or lib/x86_64-linux-gnu/).
set(CMAKE_INSTALL_LIBDIR lib CACHE PATH "" FORCE)
# The installed libssh.so must find the bundled libcrypto next to it (dist/lib): a shared
# library's own dependencies resolve through *its* RUNPATH, not the executable's.
set(_su_saved_install_rpath "${CMAKE_INSTALL_RPATH}")
set(CMAKE_INSTALL_RPATH "$ORIGIN")
FetchContent_Declare(libssh
    URL "${SU_LIBSSH_URL}"
    URL_HASH SHA256=${SU_LIBSSH_SHA256})
FetchContent_MakeAvailable(libssh)
set(CMAKE_INSTALL_RPATH "${_su_saved_install_rpath}")
# qt_standard_project_setup() turned AUTOMOC/AUTOUIC on for the whole tree and we enable AUTORCC
# below; libssh is a plain C library and must not get an (empty) mocs_compilation.cpp compiled
# with its C-only flags ("-Wstrict-prototypes is valid for C..." noise under GCC).
set_target_properties(ssh PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

# The OpenSSL DLL to ship on Windows (Shining Light and Qt "OpenSSL Toolkit" layouts).
set(SU_OPENSSL_CRYPTO_DLL "")
if(WIN32)
    get_filename_component(_su_ssl_root "${OPENSSL_INCLUDE_DIR}" DIRECTORY)
    file(GLOB _su_crypto_dlls
        "${_su_ssl_root}/bin/libcrypto-3-x64.dll"
        "${_su_ssl_root}/libcrypto-3-x64.dll"
        "${_su_ssl_root}/bin/libcrypto*.dll")
    if(_su_crypto_dlls)
        list(GET _su_crypto_dlls 0 SU_OPENSSL_CRYPTO_DLL)
        message(STATUS "libcrypto DLL: ${SU_OPENSSL_CRYPTO_DLL}")
    else()
        message(WARNING "libcrypto DLL not found under ${_su_ssl_root}; the application will not start without it")
    endif()
endif()

# Windows has no rpath: copy the shared libraries a target needs next to it after linking.
function(su_copy_runtime_dlls target)
    if(WIN32)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ssh> $<TARGET_FILE_DIR:${target}>
            VERBATIM)
        if(SU_OPENSSL_CRYPTO_DLL)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SU_OPENSSL_CRYPTO_DLL}" $<TARGET_FILE_DIR:${target}>
                VERBATIM)
        endif()
    endif()
endfunction()

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
    src/core/Transport.h
    src/core/Transport.cpp
    src/core/BaudRateDetector.h
    src/core/BaudRateDetector.cpp

    src/ssh/LibsshInclude.h
    src/ssh/SshProfile.h
    src/ssh/SshProfile.cpp
    src/ssh/SecretStore.h
    src/ssh/SecretStore.cpp
    src/ssh/SshConnection.h
    src/ssh/SshConnection.cpp
    src/ssh/SshWorker.h
    src/ssh/SshWorker.cpp

    # Firmware tab core (v0.5): external tools, Rockchip flashing, the CC1310 updater, the image pull,
    # (v0.5.1) FirmwareJob, the whole chain without widgets, (v0.6.0) FirmwareCli, the su-fw
    # command line on top of it, and (v0.7.0) FleetFlasher / FleetReport, several boards at once
    src/firmware/ToolRunner.h
    src/firmware/ToolRunner.cpp
    src/firmware/AdbTool.h
    src/firmware/AdbTool.cpp
    src/firmware/RockchipTool.h
    src/firmware/RockchipTool.cpp
    src/firmware/UsbProbe.h
    src/firmware/UsbProbe.cpp
    src/firmware/CcProtocol.h
    src/firmware/CcProtocol.cpp
    src/firmware/CcPackage.h
    src/firmware/CcPackage.cpp
    src/firmware/McuUpdater.h
    src/firmware/McuUpdater.cpp
    src/firmware/McuLink.h
    src/firmware/McuLink.cpp
    src/firmware/SocFlasher.h
    src/firmware/SocFlasher.cpp
    src/firmware/FleetFlasher.h
    src/firmware/FleetFlasher.cpp
    src/firmware/FleetReport.h
    src/firmware/FleetReport.cpp
    src/firmware/ImagePuller.h
    src/firmware/ImagePuller.cpp
    src/firmware/FirmwareJob.h
    src/firmware/FirmwareJob.cpp
    src/firmware/FirmwareCli.h
    src/firmware/FirmwareCli.cpp

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
target_link_libraries(su_core PUBLIC Qt6::Core Qt6::Gui Qt6::SerialPort Qt6::Network ssh)
if(WIN32)
    target_link_libraries(su_core PUBLIC crypt32 setupapi)   # SecretStore: DPAPI; UsbProbe: SetupAPI
endif()
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
    src/ui/SshConnectionBar.h
    src/ui/SshConnectionBar.cpp
    src/ui/FirmwareTab.h
    src/ui/FirmwareTab.cpp

    src/dialogs/HostKeyDialog.h
    src/dialogs/HostKeyDialog.cpp
    src/dialogs/HostKeyDialog.ui
    src/dialogs/AuthPromptDialog.h
    src/dialogs/AuthPromptDialog.cpp
    src/dialogs/AuthPromptDialog.ui
    src/dialogs/SshProfilesDialog.h
    src/dialogs/SshProfilesDialog.cpp
    src/dialogs/SshProfilesDialog.ui
    src/dialogs/RemoteFileDialog.h
    src/dialogs/RemoteFileDialog.cpp
    src/dialogs/RemoteFileDialog.ui

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
# Qt6::Svg: the SVG icon engine (iconengines/qsvgicon) is deployed by windeployqt / the Linux
# tarball only when the module is linked, and :/icons/ssh.svg needs it to render.
target_link_libraries(su_app PUBLIC su_core Qt6::Widgets Qt6::Svg)
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
su_copy_runtime_dlls(SerialUtility)

# su-fw (v0.6.0): the Firmware tab's operations for a command shell, scripts and AI agents
# (docs/firmware-update/CLI_PLAN.md). A console program of its own - no WIN32 keyword - because the
# GUI executable is a Windows-subsystem program that cannot write to the shell that started it. It
# links su_core only (QCoreApplication, no widgets) and lives next to the GUI executable, so it
# finds the same tools/ folder; installed and deployed exactly like it (below).
qt_add_executable(su_fw src/cli/main.cpp)
set_target_properties(su_fw PROPERTIES OUTPUT_NAME "su-fw")
target_link_libraries(su_fw PRIVATE su_core)
su_set_compile_options(su_fw)
su_copy_runtime_dlls(su_fw)

# ---------------------------------------------------------------------------
# Bundled flashing tools (Firmware tab): third_party/ -> <executable dir>/tools/
# ---------------------------------------------------------------------------
# The Firmware tab starts Rockchip's upgrade_tool and adb as separate processes and looks for them in
# tools/ next to the executable (tools/upgrade_tool[.exe], tools/adb.exe,
# tools/DriverAssistant/DriverInstall.exe). The folder is assembled after every link of SerialUtility, so
# a build tree runs the tab as it is, and `cmake --install` ships the very same folder as bin/tools.
# su-fw sits in the same folder and uses the same tools/ (in a build tree: once SerialUtility was built;
# the copy is not repeated for su_fw, whose link may run in parallel and would race for the same files).
# The sources are plain files under third_party/ (provenance and licences: third_party/README.md).
set(SU_THIRD_PARTY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
set(SU_TOOLS_DIR "$<TARGET_FILE_DIR:SerialUtility>/tools")
set(SU_TOOL_FILES "")
set(SU_TOOL_DRIVER_DIR "")
if(WIN32)
    list(APPEND SU_TOOL_FILES
        "${SU_THIRD_PARTY_DIR}/rockchip/windows/upgrade_tool.exe"
        "${SU_THIRD_PARTY_DIR}/rockchip/windows/license.txt"
        "${SU_THIRD_PARTY_DIR}/rockchip/windows/revison.log"
        "${SU_THIRD_PARTY_DIR}/adb/adb.exe"
        "${SU_THIRD_PARTY_DIR}/adb/AdbWinApi.dll"
        "${SU_THIRD_PARTY_DIR}/adb/AdbWinUsbApi.dll")

    # Rockchip DriverAssistant (rockusb + ADB WinUSB drivers): kept as one zip in the source tree, extracted
    # once at configure time (again when the zip changes) and copied flat to tools/DriverAssistant, so that
    # tools/DriverAssistant/DriverInstall.exe exists whatever the folder inside the zip is called.
    set(SU_DRIVER_ZIP "${SU_THIRD_PARTY_DIR}/DriverAssistant/DriverAssitant_v5.14.zip")
    set(SU_DRIVER_EXTRACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/DriverAssistant-extracted")
    set(SU_DRIVER_STAMP "${CMAKE_CURRENT_BINARY_DIR}/DriverAssistant-extracted.stamp")
    if(EXISTS "${SU_DRIVER_ZIP}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SU_DRIVER_ZIP}")
        if(NOT EXISTS "${SU_DRIVER_STAMP}" OR "${SU_DRIVER_ZIP}" IS_NEWER_THAN "${SU_DRIVER_STAMP}")
            file(REMOVE_RECURSE "${SU_DRIVER_EXTRACT_DIR}")
            file(ARCHIVE_EXTRACT INPUT "${SU_DRIVER_ZIP}" DESTINATION "${SU_DRIVER_EXTRACT_DIR}")
            file(TOUCH "${SU_DRIVER_STAMP}")
        endif()
        file(GLOB_RECURSE _su_driver_installer "${SU_DRIVER_EXTRACT_DIR}/DriverInstall.exe")
        if(_su_driver_installer)
            list(GET _su_driver_installer 0 _su_driver_installer)
            get_filename_component(SU_TOOL_DRIVER_DIR "${_su_driver_installer}" DIRECTORY)
        else()
            message(WARNING "DriverInstall.exe not found in ${SU_DRIVER_ZIP}: the Rockchip drivers will not be bundled")
        endif()
    else()
        message(WARNING "${SU_DRIVER_ZIP} is missing: the Rockchip drivers will not be bundled")
    endif()
elseif(UNIX AND NOT APPLE)
    list(APPEND SU_TOOL_FILES
        "${SU_THIRD_PARTY_DIR}/rockchip/linux/upgrade_tool"
        "${SU_THIRD_PARTY_DIR}/rockchip/linux/config.ini"
        "${SU_THIRD_PARTY_DIR}/rockchip/linux/88-rockusb.rules"
        "${SU_THIRD_PARTY_DIR}/rockchip/windows/license.txt")   # the one licence text Rockchip ships with the tool
endif()

# A missing file must not break the build of a partial checkout; the CI jobs check the installed folder.
set(_su_tool_files_present "")
foreach(_su_tool IN LISTS SU_TOOL_FILES)
    if(EXISTS "${_su_tool}")
        list(APPEND _su_tool_files_present "${_su_tool}")
    else()
        message(WARNING "bundled tool file missing: ${_su_tool}")
    endif()
endforeach()
set(SU_TOOL_FILES ${_su_tool_files_present})

if(SU_TOOL_FILES OR SU_TOOL_DRIVER_DIR)
    # copy_directory_if_different needs CMake 3.26; the older copy_directory always copies (10+ MB per link).
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.26)
        set(_su_copy_dir copy_directory_if_different)
    else()
        set(_su_copy_dir copy_directory)
    endif()
    # Replacing a file in third_party/ relinks the executable, which runs the copy steps below again.
    set(_su_tool_deps ${SU_TOOL_FILES})
    if(SU_TOOL_DRIVER_DIR)
        list(APPEND _su_tool_deps "${SU_DRIVER_ZIP}")
    endif()
    set_property(TARGET SerialUtility APPEND PROPERTY LINK_DEPENDS ${_su_tool_deps})
    add_custom_command(TARGET SerialUtility POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${SU_TOOLS_DIR}"
        VERBATIM)
    if(SU_TOOL_FILES)
        add_custom_command(TARGET SerialUtility POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${SU_TOOL_FILES} "${SU_TOOLS_DIR}"
            VERBATIM)
    endif()
    if(SU_TOOL_DRIVER_DIR)
        add_custom_command(TARGET SerialUtility POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E ${_su_copy_dir} "${SU_TOOL_DRIVER_DIR}" "${SU_TOOLS_DIR}/DriverAssistant"
            VERBATIM)
    endif()
    if(UNIX AND NOT APPLE AND EXISTS "${SU_THIRD_PARTY_DIR}/rockchip/linux/upgrade_tool")
        # Git on Windows may not have recorded the executable bit of the vendor binary.
        add_custom_command(TARGET SerialUtility POST_BUILD
            COMMAND chmod 755 "${SU_TOOLS_DIR}/upgrade_tool"
            VERBATIM)
    endif()
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
set(SU_TRANSLATION_TARGETS SerialUtility)
if(SU_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    # GUI tests that exercise the language menu / translated group names need the embedded .qm files too.
    list(APPEND SU_TRANSLATION_TARGETS tst_mainwindow tst_dialogs tst_sessionwidget tst_sshsession)
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
install(TARGETS SerialUtility su_fw
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION bin)
install(FILES README.md VERSION.txt DESTINATION .)
# The flashing tools assembled next to the executable above (generator expression: the config's own folder
# for multi-config generators). USE_SOURCE_PERMISSIONS keeps the executable bit of the Linux upgrade_tool.
if(SU_TOOL_FILES OR SU_TOOL_DRIVER_DIR)
    install(DIRECTORY "${SU_TOOLS_DIR}/" DESTINATION bin/tools USE_SOURCE_PERMISSIONS)
endif()
# libssh installs its own ssh.dll / libssh.so.4 (bin/ resp. lib/) through its install rules;
# ship the OpenSSL crypto library it depends on the same way.
if(WIN32 AND SU_OPENSSL_CRYPTO_DLL)
    install(FILES "${SU_OPENSSL_CRYPTO_DLL}" DESTINATION bin)
elseif(UNIX AND NOT APPLE)
    file(REAL_PATH "${OPENSSL_CRYPTO_LIBRARY}" _su_crypto_real)
    get_filename_component(_su_crypto_dir "${_su_crypto_real}" DIRECTORY)
    file(GLOB _su_crypto_so "${_su_crypto_dir}/libcrypto.so.*")   # libcrypto.so.3
    install(FILES ${_su_crypto_so} DESTINATION lib)
endif()

qt_generate_deploy_app_script(
    TARGET SerialUtility
    OUTPUT_SCRIPT su_deploy_script
    NO_UNSUPPORTED_PLATFORM_ERROR
    NO_TRANSLATIONS)
# su-fw gets its own run of the same deploy tool (windeployqt / the Linux deploy), so the package
# carries whatever it needs even where the GUI would not (its libraries are a subset of the GUI's).
qt_generate_deploy_app_script(
    TARGET su_fw
    OUTPUT_SCRIPT su_fw_deploy_script
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
install(SCRIPT "${su_fw_deploy_script}")

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
