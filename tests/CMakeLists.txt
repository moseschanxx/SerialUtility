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
    su_set_compile_options(${name})
    su_copy_runtime_dlls(${name})
    su_configure_test(${name})
endfunction()

function(su_add_gui_test name)
    qt_add_executable(${name} ${ARGN} "${CMAKE_SOURCE_DIR}/resources/resources.qrc")
    target_link_libraries(${name} PRIVATE su_app Qt6::Test)
    su_set_compile_options(${name})
    su_copy_runtime_dlls(${name})
    su_configure_test(${name})
    # GUI suites drive real widgets with timers; give them room on slow CI runners.
    set_tests_properties(${name} PROPERTIES TIMEOUT 600)
endfunction()

# ---- Test support: in-process SSH server (libssh server API) ---------------------------
qt_add_library(su_testsupport STATIC
    support/TestSshServer.h
    support/TestSshServer.cpp
    support/TestSftpHandler.h
    support/TestSftpHandler.cpp)
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
su_add_test(tst_sshprofile        tst_sshprofile.cpp)
su_add_test(tst_secretstore       tst_secretstore.cpp)
# SSH end to end against the in-process server (tests/support/TestSshServer.h).
su_add_test(tst_testsshserver     tst_testsshserver.cpp)
target_link_libraries(tst_testsshserver PRIVATE su_testsupport)
su_add_test(tst_sshconnection     tst_sshconnection.cpp)
target_link_libraries(tst_sshconnection PRIVATE su_testsupport)
set_tests_properties(tst_testsshserver tst_sshconnection PROPERTIES TIMEOUT 600)

# ---- GUI (offscreen) -----------------------------------------------------------------
su_add_gui_test(tst_terminalwidget tst_terminalwidget.cpp)
su_add_gui_test(tst_sessionwidget  tst_sessionwidget.cpp)
# One SessionWidget case connects to the in-process SSH server (tests/support/TestSshServer.h).
target_link_libraries(tst_sessionwidget PRIVATE su_testsupport)
su_add_gui_test(tst_mainwindow     tst_mainwindow.cpp)
su_add_gui_test(tst_dialogs        tst_dialogs.cpp)
su_add_gui_test(tst_sshdialogs     tst_sshdialogs.cpp)
su_add_gui_test(tst_sshsession     tst_sshsession.cpp)
target_link_libraries(tst_sshsession PRIVATE su_testsupport)
# RX throughput / responsiveness (prints the measured MB/s; asserts generous floors only).
su_add_gui_test(tst_terminalperf   tst_terminalperf.cpp)
