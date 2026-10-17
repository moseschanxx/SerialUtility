# Tests for BuildAI Serial Utility.
#  - su_add_test:     UI-free suites linked against su_core (QTEST_GUILESS_MAIN)
#  - su_add_gui_test: widget/dialog suites linked against su_app (QTEST_MAIN, run offscreen)
# ctest sets QT_QPA_PLATFORM=offscreen and puts the Qt bin directory on PATH.

function(su_configure_test name)
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32 AND DEFINED QT6_INSTALL_PREFIX)
        # Make the Qt DLLs reachable when ctest launches the test binary.
        set_property(TEST ${name} APPEND PROPERTY
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
    endif()
endfunction()

function(su_add_test name)
    qt_add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE su_core Qt6::Test)
    # Known-answer vectors and sample files (tests/data/...): SU_TEST_DATA_DIR/firmware/vectors.json
    target_compile_definitions(${name} PRIVATE SU_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    su_set_compile_options(${name})
    su_copy_runtime_dlls(${name})
    su_configure_test(${name})
endfunction()

function(su_add_gui_test name)
    qt_add_executable(${name} ${ARGN} "${CMAKE_SOURCE_DIR}/resources/resources.qrc")
    target_link_libraries(${name} PRIVATE su_app Qt6::Test)
    target_compile_definitions(${name} PRIVATE SU_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    su_set_compile_options(${name})
    su_copy_runtime_dlls(${name})
    su_configure_test(${name})
    # GUI suites drive real widgets with timers; give them room on slow CI runners.
    set_tests_properties(${name} PROPERTIES TIMEOUT 600)
endfunction()

# ---- Test support: in-process SSH server (libssh server API), fake tools and a fake CC1310 ----
qt_add_library(su_testsupport STATIC
    support/TestSshServer.h
    support/TestSshServer.cpp
    support/TestSftpHandler.h
    support/TestSftpHandler.cpp
    support/FakeToolRunner.h
    support/FakeToolRunner.cpp
    support/FakeMcu.h
    support/FakeMcu.cpp
    support/FirmwareFixtures.h
    support/FirmwareFixtures.cpp
    support/FakeUsbBus.h
    support/FakeUsbBus.cpp)
target_include_directories(su_testsupport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(su_testsupport PUBLIC su_core Qt6::Network)
su_set_compile_options(su_testsupport)

# ---- UI-free -------------------------------------------------------------------------
su_add_test(tst_hexutils          tst_hexutils.cpp)
su_add_test(tst_lineending        tst_lineending.cpp)
su_add_test(tst_commandhistory    tst_commandhistory.cpp)
su_add_test(tst_quickcommand      tst_quickcommand.cpp)
su_add_test(tst_charwidth         tst_charwidth.cpp)
su_add_test(tst_terminalscreen    tst_terminalscreen.cpp)
su_add_test(tst_ansiparser        tst_ansiparser.cpp)
su_add_test(tst_serialsettings    tst_serialsettings.cpp)
su_add_test(tst_devicesimulator   tst_devicesimulator.cpp)
su_add_test(tst_logreplayer       tst_logreplayer.cpp)
su_add_test(tst_sessionlogger     tst_sessionlogger.cpp)
su_add_test(tst_bauddetector      tst_bauddetector.cpp)
su_add_test(tst_sshprofile        tst_sshprofile.cpp)
su_add_test(tst_secretstore       tst_secretstore.cpp)
# SSH end to end against the in-process server (tests/support/TestSshServer.h).
su_add_test(tst_testsshserver     tst_testsshserver.cpp)
target_link_libraries(tst_testsshserver PRIVATE su_testsupport)
su_add_test(tst_sshconnection     tst_sshconnection.cpp)
target_link_libraries(tst_sshconnection PRIVATE su_testsupport)
set_tests_properties(tst_testsshserver tst_sshconnection PROPERTIES TIMEOUT 600)
# Firmware tab core (v0.5)
su_add_test(tst_ccprotocol        tst_ccprotocol.cpp)
su_add_test(tst_mcuupdater        tst_mcuupdater.cpp)
target_link_libraries(tst_mcuupdater PRIVATE su_testsupport)
su_add_test(tst_firmwaretools     tst_firmwaretools.cpp)
target_link_libraries(tst_firmwaretools PRIVATE su_testsupport)
su_add_test(tst_imagepuller       tst_imagepuller.cpp)
target_link_libraries(tst_imagepuller PRIVATE su_testsupport)
# The firmware chain without widgets (v0.5.1): FirmwareJob on fake tools, a fake CC1310 and the
# in-process SSH server - what the Firmware tab and the su-fw console tool both drive.
su_add_test(tst_firmwarejob       tst_firmwarejob.cpp)
target_link_libraries(tst_firmwarejob PRIVATE su_testsupport)
# su-fw (v0.6.0), the console tool on top of FirmwareJob: FirmwareCli in-process on the same fakes,
# plus the built su-fw executable itself through QProcess (--help, --version, a usage error).
su_add_test(tst_firmwarecli       tst_firmwarecli.cpp)
target_link_libraries(tst_firmwarecli PRIVATE su_testsupport)
add_dependencies(tst_firmwarecli su_fw)
target_compile_definitions(tst_firmwarecli PRIVATE SU_FW_EXECUTABLE="$<TARGET_FILE:su_fw>")
set_tests_properties(tst_mcuupdater tst_imagepuller tst_firmwarejob tst_firmwarecli PROPERTIES TIMEOUT 600)
# (v0.7.0) Parallel SoC flashing: FleetFlasher / FleetReport on FakeToolRunner and FakeUsbBus (the
# seven bench boards, every misbehaviour of the mapping; no adb, no upgrade_tool, no board).
su_add_test(tst_fleetflasher      tst_fleetflasher.cpp)
target_link_libraries(tst_fleetflasher PRIVATE su_testsupport)
set_tests_properties(tst_fleetflasher PROPERTIES TIMEOUT 600)
# (v0.8.0) Board profiles: the Node / Stacked table, the detection order and the rules' texts.
su_add_test(tst_boardprofile      tst_boardprofile.cpp)
target_link_libraries(tst_boardprofile PRIVATE su_testsupport)
# (v0.8.0) The bench inventory: the file (lock, atomic write, corrupt file) and its recorder on a
# FirmwareJob over FakeToolRunner / FakeUsbBus / the in-process SSH server.
su_add_test(tst_inventory         tst_inventory.cpp)
target_link_libraries(tst_inventory PRIVATE su_testsupport)
set_tests_properties(tst_inventory PROPERTIES TIMEOUT 600)

# ---- GUI (offscreen) -----------------------------------------------------------------
su_add_gui_test(tst_terminalwidget tst_terminalwidget.cpp)
su_add_gui_test(tst_sessionwidget  tst_sessionwidget.cpp)
# One SessionWidget case connects to the in-process SSH server (tests/support/TestSshServer.h).
target_link_libraries(tst_sessionwidget PRIVATE su_testsupport)
su_add_gui_test(tst_mainwindow     tst_mainwindow.cpp)
# The keyboard rules on a connected SSH tab are checked against the in-process server too.
target_link_libraries(tst_mainwindow PRIVATE su_testsupport)
su_add_gui_test(tst_dialogs        tst_dialogs.cpp)
su_add_gui_test(tst_sshdialogs     tst_sshdialogs.cpp)
su_add_gui_test(tst_sshsession     tst_sshsession.cpp)
target_link_libraries(tst_sshsession PRIVATE su_testsupport)
# The Firmware tab with fake tools, a fake CC1310 and the in-process SSH server (v0.5).
su_add_gui_test(tst_firmwaretab    tst_firmwaretab.cpp)
target_link_libraries(tst_firmwaretab PRIVATE su_testsupport)
# RX throughput / responsiveness (prints the measured MB/s; asserts generous floors only).
su_add_gui_test(tst_terminalperf   tst_terminalperf.cpp)
