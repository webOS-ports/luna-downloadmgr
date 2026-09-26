# Copyright (c) 2026 LG Electronics, Inc.
#
# SPDX-License-Identifier: Apache-2.0
#
# libFuzzer harnesses for the parsers that see untrusted input: the download URL,
# HTTP response headers, stored history records, and downloadManager.conf values.
#
# Build these with clang and a sanitizer, e.g.
#   cmake -DENABLE_FUZZERS=ON -DSANITIZE=address,undefined \
#         -DCMAKE_CXX_COMPILER=clang++ ...

include(CheckCXXSourceCompiles)

set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=fuzzer")
check_cxx_source_compiles(
    "extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, unsigned long){return 0;}"
    HAVE_LIBFUZZER)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)

if(NOT HAVE_LIBFUZZER)
    message(FATAL_ERROR
        "ENABLE_FUZZERS=ON but ${CMAKE_CXX_COMPILER} cannot build -fsanitize=fuzzer "
        "targets. Use clang++, or configure with -DENABLE_FUZZERS=OFF.")
endif()

set(FUZZERS
    fuzz_urlrep
    fuzz_header
    fuzz_memstring
    fuzz_historyjson)

foreach(f ${FUZZERS})
    add_executable(${f} ${f}.cpp)
    target_include_directories(${f} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_options(${f} PRIVATE -fsanitize=fuzzer)
    target_link_options(${f} PRIVATE -fsanitize=fuzzer)
    target_link_libraries(${f} LunaDownloadMgrCore ${LDM_LIBRARIES})

    # A short deterministic pass over the checked-in corpus, so `ctest` catches
    # regressions without anyone having to run a fuzzing campaign.
    #
    # libFuzzer writes newly discovered units into the *first* directory it is
    # given and treats the rest as read-only inputs, so the generated corpus goes
    # to the build tree. Passing the source corpus first would grow it by a few
    # hundred files on every run.
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${f})
        file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/corpus-out/${f})
        add_test(NAME ${f}_corpus
                 COMMAND ${f} -runs=2000 -max_total_time=20
                         ${CMAKE_CURRENT_BINARY_DIR}/corpus-out/${f}
                         ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${f})
    endif()
endforeach()
