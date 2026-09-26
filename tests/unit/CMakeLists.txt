# Copyright (c) 2026 LG Electronics, Inc.
#
# SPDX-License-Identifier: Apache-2.0

find_package(Threads REQUIRED)

# gtest ships as a static library plus sources in the webOS sysroot; prefer the
# library and fall back to compiling src/gtest-all.cc when only sources exist.
find_path(GTEST_INCLUDE_DIR gtest/gtest.h)
find_library(GTEST_LIBRARY NAMES gtest)
find_library(GTEST_MAIN_LIBRARY NAMES gtest_main)

if(NOT GTEST_INCLUDE_DIR OR NOT GTEST_LIBRARY)
    message(FATAL_ERROR
        "ENABLE_TESTS=ON but gtest was not found. Install gtest into the "
        "sysroot (OE: DEPENDS += \"gtest\") or configure with -DENABLE_TESTS=OFF.")
endif()

set(UNIT_TESTS
    test_urlrep
    test_pathutils
    test_settings
    test_taskjson
    test_fsstatus
    test_upload)

foreach(t ${UNIT_TESTS})
    add_executable(${t} ${t}.cpp test_support.cpp)
    target_include_directories(${t} PRIVATE ${GTEST_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${t}
                          LunaDownloadMgrCore
                          ${LDM_LIBRARIES}
                          ${GTEST_LIBRARY}
                          Threads::Threads)
    if(GTEST_MAIN_LIBRARY)
        target_link_libraries(${t} ${GTEST_MAIN_LIBRARY})
    endif()
    add_test(NAME ${t} COMMAND ${t})
    # The tests must not inherit a half-configured device: point the schema
    # loader and the history db at the build tree.
    set_tests_properties(${t} PROPERTIES
        ENVIRONMENT "ASAN_OPTIONS=detect_leaks=1:abort_on_error=1;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
endforeach()
