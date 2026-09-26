# Copyright (c) 2026 LG Electronics, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# SPDX-License-Identifier: Apache-2.0
#
# Exploit-mitigation and diagnostic build options for LunaDownloadMgr.
#
# Every flag is probed with check_cxx_compiler_flag / check_cxx_source_compiles
# before use, so this file is safe to include for any of the targets this
# component is built for (armv7, aarch64, x86-64) and for both gcc and clang.
# A distro that already supplies its own hardening flags (OpenEmbedded does)
# keeps them; these are additive and de-duplicated by the compiler.

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)
include(CMakeDependentOption)

option(ENABLE_HARDENING "Add exploit-mitigation compiler and linker flags" ON)
option(ENABLE_WERROR    "Promote compiler warnings to errors" OFF)
set(SANITIZER "" CACHE STRING
    "Sanitizers to build with, as passed to -fsanitize (e.g. address, undefined, \"address,undefined\", thread). Empty disables them.")

# Accumulates a human-readable summary printed at the end of configuration.
set(_hardening_summary "")

# These are macros, not functions, on purpose. webos_add_compiler_flags() is
# itself a macro that ends in a plain set(CMAKE_CXX_FLAGS ...), so calling it
# from inside a function() would write a function-local variable and the flag
# would be silently dropped on return - the configure log would still list it.
# Same for CMAKE_EXE_LINKER_FLAGS. Macros share the caller's scope, so the
# assignments land where they are needed.
macro(_ldm_try_add_cxx_flag flag)
    string(MAKE_C_IDENTIFIER "HAVE_CXX${flag}" _ldm_probe_var)
    check_cxx_compiler_flag("${flag}" ${_ldm_probe_var})
    if(${_ldm_probe_var})
        webos_add_compiler_flags(ALL ${flag})
        set(_hardening_summary "${_hardening_summary} ${flag}")
    endif()
    unset(_ldm_probe_var)
endmacro()

macro(_ldm_try_add_link_flag flag)
    string(MAKE_C_IDENTIFIER "HAVE_LD${flag}" _ldm_probe_var)
    set(CMAKE_REQUIRED_LINK_OPTIONS ${flag})
    check_cxx_source_compiles("int main(void){return 0;}" ${_ldm_probe_var})
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(${_ldm_probe_var})
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${flag}")
        set(_hardening_summary "${_hardening_summary} ${flag}")
    endif()
    unset(_ldm_probe_var)
endmacro()

macro(ldm_apply_hardening)
    if(ENABLE_HARDENING)
        # Stack protection. -fstack-clash-protection is unavailable on some
        # arm configurations, hence the probe.
        _ldm_try_add_cxx_flag(-fstack-protector-strong)
        _ldm_try_add_cxx_flag(-fstack-clash-protection)

        # Control-flow integrity: x86 only, ignored elsewhere.
        _ldm_try_add_cxx_flag(-fcf-protection=full)

        # Glibc's fortified string/stdio wrappers. Level 3 needs gcc 12/clang 16
        # and only takes effect when optimising; -U first because the SDK may
        # already define it and redefining warns.
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            check_cxx_compiler_flag("-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3 -O1" HAVE_FORTIFY_3)
            if(HAVE_FORTIFY_3)
                webos_add_compiler_flags(ALL -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3)
                set(_hardening_summary "${_hardening_summary} -D_FORTIFY_SOURCE=3")
            else()
                webos_add_compiler_flags(ALL -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2)
                set(_hardening_summary "${_hardening_summary} -D_FORTIFY_SOURCE=2")
            endif()
        endif()

        # A format string that is not a literal is how the %lu/uint64_t class of
        # bug gets reintroduced; make it fatal rather than advisory.
        _ldm_try_add_cxx_flag(-Wformat)
        _ldm_try_add_cxx_flag(-Wformat-security)
        _ldm_try_add_cxx_flag(-Werror=format-security)

        # Tentative definitions in separate TUs silently aliasing each other.
        _ldm_try_add_cxx_flag(-fno-common)

        # Position-independent executable, so ASLR applies to the image itself.
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
        _ldm_try_add_link_flag(-pie)

        # Full RELRO, non-executable stack, no over-linking.
        _ldm_try_add_link_flag("-Wl,-z,relro")
        _ldm_try_add_link_flag("-Wl,-z,now")
        _ldm_try_add_link_flag("-Wl,-z,noexecstack")
        _ldm_try_add_link_flag("-Wl,--as-needed")
    endif()

    # Warnings worth having on all the time. -Wall is already set by the caller.
    foreach(_w -Wextra -Wshadow -Wpointer-arith -Wcast-align
               -Wno-unused-parameter -Wimplicit-fallthrough)
        _ldm_try_add_cxx_flag(${_w})
    endforeach()

    if(ENABLE_WERROR)
        _ldm_try_add_cxx_flag(-Werror)
    endif()

    if(NOT SANITIZER STREQUAL "")
        # Sanitizers need frame pointers and unwind tables to symbolise, and
        # -fno-omit-frame-pointer also makes the stress runs profileable.
        set(_san "-fsanitize=${SANITIZER}")
        set(CMAKE_REQUIRED_LINK_OPTIONS ${_san})
        check_cxx_compiler_flag("${_san}" HAVE_REQUESTED_SANITIZER)
        unset(CMAKE_REQUIRED_LINK_OPTIONS)
        if(NOT HAVE_REQUESTED_SANITIZER)
            message(FATAL_ERROR "SANITIZER=${SANITIZER} is not supported by ${CMAKE_CXX_COMPILER}")
        endif()
        webos_add_compiler_flags(ALL ${_san} -fno-omit-frame-pointer -fno-sanitize-recover=all -g)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${_san}")
        set(_hardening_summary "${_hardening_summary} ${_san}")
        # ASan's own redzones conflict with fortified wrappers' assumptions.
        if(SANITIZER MATCHES "address")
            webos_add_compiler_flags(ALL -U_FORTIFY_SOURCE)
        endif()
    endif()

    message(STATUS "LunaDownloadMgr hardening:${_hardening_summary}")
endmacro()
