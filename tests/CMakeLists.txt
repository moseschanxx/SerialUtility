# Unit tests for the UI-free parts of BuildAI Serial Utility (su_core).
# Each tst_*.cpp is a self-contained Qt Test executable.

function(su_add_test name)
    qt_add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE su_core Qt6::Test)
    su_set_compile_options(${name})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(WIN32 AND DEFINED QT6_INSTALL_PREFIX)
        # Make the Qt DLLs reachable when ctest launches the test binary.
        set_property(TEST ${name} APPEND PROPERTY
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
    endif()
endfunction()

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
