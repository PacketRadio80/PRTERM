cmake_minimum_required(VERSION 3.16)

project(prterm
    VERSION 0.1.0
    DESCRIPTION "PRTERM - CB & Amateurfunk Terminal (CGI)"
    LANGUAGES C)

# ---------------------------------------------------------------------------
# Portability baseline
#   Target platforms: Linux + FreeBSD, x86-64 + arm64 (maybe more later)
#
#   Strict C11 + POSIX.1-2008/XSI. Deliberately NO GNU extensions and no
#   _GNU_SOURCE - the code must compile unchanged on FreeBSD.
#   Specifically avoided:
#     cfcmakeraw()     -> own replacement (cfmakeraw is BSD/GNU, not POSIX)
#     flock()          -> fcntl() locks (F_SETLK) are portable
#     epoll/timerfd    -> poll() for everything
#     getrandom()      -> /dev/urandom
#     /proc/self/exe   -> not portable, paths come from the INI
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE "RelWithDebInfo" CACHE STRING "Build type" FORCE)
endif()

include(CTest)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(PRTERM_WERROR        "Treat compiler warnings as errors"  OFF)
option(PRTERM_SANITIZE      "Build with ASan/UBSan"              OFF)
option(PRTERM_BUILD_TOOLS   "Build shell helper tools"           ON)

set(PRTERM_RIG_SIM     ON  CACHE BOOL "Build the simulated rig (no hardware)")
set(PRTERM_RIG_TNC2    ON  CACHE BOOL "Build the TNC2-class driver (Landolt TNC2C, PK-TNC2)")
set(PRTERM_RIG_TMODEM  OFF CACHE BOOL "Build the T-Modem half-TNC driver")

# ---------------------------------------------------------------------------
# Compiler settings
# ---------------------------------------------------------------------------
set(PRTERM_WARN_FLAGS
    -Wall -Wextra -Wshadow -Wpointer-arith -Wcast-align
    -Wstrict-prototypes -Wmissing-prototypes -Wredundant-decls
    -Wwrite-strings -Wformat=2 -Wundef -Wvla)

if(PRTERM_WERROR)
    list(APPEND PRTERM_WARN_FLAGS -Werror)
endif()

# POSIX.1-2008 is the lower bound for Linux and FreeBSD. Also set in
# every .c via src/prterm_compat.h so that a manually run compiler
# invocation works too.
add_compile_definitions(_POSIX_C_SOURCE=200809L)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_compile_definitions(_DEFAULT_SOURCE=1)
endif()

if(PRTERM_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Host tool: embeds web/ assets into the binary
# ---------------------------------------------------------------------------
add_executable(prterm_embed tools/embed.c)
target_compile_options(prterm_embed PRIVATE ${PRTERM_WARN_FLAGS})

set(PRTERM_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
file(MAKE_DIRECTORY "${PRTERM_GEN_DIR}")

function(prterm_embed_asset asset symbol out_var)
    set(_in  "${CMAKE_CURRENT_SOURCE_DIR}/web/${asset}")
    set(_out "${PRTERM_GEN_DIR}/embed_${symbol}.c")
    if(NOT EXISTS "${_in}")
        message(FATAL_ERROR "missing asset: ${_in}")
    endif()
    add_custom_command(
        OUTPUT  "${_out}"
        COMMAND prterm_embed "${_in}" "${_out}" "${symbol}"
        DEPENDS prterm_embed "${_in}"
        COMMENT "ASSET  web/${asset} -> ${symbol}"
        VERBATIM)
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

prterm_embed_asset(prterm.css prterm_css PRTERM_CSS_SRC)
prterm_embed_asset(prterm.js  prterm_js  PRTERM_JS_SRC)

# Retro/alternative assets — same symbol names as modern (prterm_css, prterm_js)
# so html.c works unchanged, but different source files.
# The gen dir gets separate output files to avoid conflicts.
set(_alter_css_out "${PRTERM_GEN_DIR}/embed_alter_prterm_css.c")
set(_alter_js_out  "${PRTERM_GEN_DIR}/embed_alter_prterm_js.c")
add_custom_command(
    OUTPUT  "${_alter_css_out}"
    COMMAND prterm_embed "${CMAKE_CURRENT_SOURCE_DIR}/web/prterm-alter.css" "${_alter_css_out}" "prterm_css"
    DEPENDS prterm_embed "${CMAKE_CURRENT_SOURCE_DIR}/web/prterm-alter.css"
    COMMENT "ASSET  web/prterm-alter.css -> prterm_css (alter)"
    VERBATIM)
add_custom_command(
    OUTPUT  "${_alter_js_out}"
    COMMAND prterm_embed "${CMAKE_CURRENT_SOURCE_DIR}/web/prterm-alter.js" "${_alter_js_out}" "prterm_js"
    DEPENDS prterm_embed "${CMAKE_CURRENT_SOURCE_DIR}/web/prterm-alter.js"
    COMMENT "ASSET  web/prterm-alter.js -> prterm_js (alter)"
    VERBATIM)
set(PRTERM_ALTER_CSS_SRC "${_alter_css_out}")
set(PRTERM_ALTER_JS_SRC  "${_alter_js_out}")

# ---------------------------------------------------------------------------
# Core library (also linked by the tests)
# ---------------------------------------------------------------------------
set(PRTERM_CORE_SOURCES
    src/util.c
    src/trace.c
    src/sha256.c
    src/ini.c
    src/bands.c
    src/locator.c
    src/config.c
    src/lang.c
    src/arbiter.c
    src/callsign.c
    src/radio.c
    src/serial.c
    src/kiss.c
    src/probe.c
    src/state.c
    src/cgi.c
    src/session.c
    src/html.c
    src/admin.c
    src/selftest.c
    src/tncsock.c
    src/mailboxdsock.c
    src/inieditor.c
    src/pages.c
    "${PRTERM_CSS_SRC}"
    "${PRTERM_JS_SRC}")

if(PRTERM_RIG_SIM)
    list(APPEND PRTERM_CORE_SOURCES src/simrig.c)
endif()
if(PRTERM_RIG_TNC2)
    list(APPEND PRTERM_CORE_SOURCES src/tnc2.c)
endif()
if(PRTERM_RIG_TMODEM)
    list(APPEND PRTERM_CORE_SOURCES src/tmodem.c)
endif()

add_library(prterm_core STATIC ${PRTERM_CORE_SOURCES})
target_include_directories(prterm_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(prterm_core PRIVATE ${PRTERM_WARN_FLAGS})

foreach(_def IN ITEMS
        PRTERM_RIG_SIM=$<BOOL:${PRTERM_RIG_SIM}>
        PRTERM_RIG_TNC2=$<BOOL:${PRTERM_RIG_TNC2}>
        PRTERM_RIG_TMODEM=$<BOOL:${PRTERM_RIG_TMODEM}>)
    target_compile_definitions(prterm_core PRIVATE ${_def})
endforeach()

find_package(Threads REQUIRED)
target_link_libraries(prterm_core PUBLIC Threads::Threads)

# ---------------------------------------------------------------------------
# The CGI binary
# ---------------------------------------------------------------------------
add_executable(prterm src/main.c)
target_link_libraries(prterm PRIVATE prterm_core)
target_compile_options(prterm PRIVATE ${PRTERM_WARN_FLAGS})
set_target_properties(prterm PROPERTIES OUTPUT_NAME "prterm.cgi" PREFIX "")

# ---------------------------------------------------------------------------
# The retro/alternative CGI binary (prterm-alter.cgi)
#
# Same core as prterm.cgi but with a different page rendering layer
# (pages_alter.c instead of pages.c) and retro web assets.
# Works in text browsers (lynx, links, w3m) and on retro computers.
# ---------------------------------------------------------------------------
set(PRTERM_ALTER_CORE_SOURCES ${PRTERM_CORE_SOURCES})
list(REMOVE_ITEM PRTERM_ALTER_CORE_SOURCES src/pages.c "${PRTERM_CSS_SRC}" "${PRTERM_JS_SRC}")
list(APPEND PRTERM_ALTER_CORE_SOURCES src/pages_alter.c "${PRTERM_ALTER_CSS_SRC}" "${PRTERM_ALTER_JS_SRC}")

add_library(prterm_alter_core STATIC ${PRTERM_ALTER_CORE_SOURCES})
target_include_directories(prterm_alter_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(prterm_alter_core PRIVATE ${PRTERM_WARN_FLAGS})
target_link_libraries(prterm_alter_core PUBLIC Threads::Threads)
foreach(_def IN ITEMS
        PRTERM_RIG_SIM=$<BOOL:${PRTERM_RIG_SIM}>
        PRTERM_RIG_TNC2=$<BOOL:${PRTERM_RIG_TNC2}>
        PRTERM_RIG_TMODEM=$<BOOL:${PRTERM_RIG_TMODEM}>)
    target_compile_definitions(prterm_alter_core PRIVATE ${_def})
endforeach()

add_executable(prterm-alter src/main_alter.c)
target_link_libraries(prterm-alter PRIVATE prterm_alter_core)
target_compile_options(prterm-alter PRIVATE ${PRTERM_WARN_FLAGS})
set_target_properties(prterm-alter PROPERTIES OUTPUT_NAME "prterm-alter.cgi" PREFIX "")

# ---------------------------------------------------------------------------
# The device daemon - keeps the serial ports open and holds KISS
#
# A CGI process ends after every request. When the last file descriptor
# goes, USB serial adapters reset and the TNC leaves KISS. This daemon
# is what keeps that from happening.
# ---------------------------------------------------------------------------
add_executable(prterm-tncd src/tncd.c)
target_link_libraries(prterm-tncd PRIVATE prterm_core)
target_compile_options(prterm-tncd PRIVATE ${PRTERM_WARN_FLAGS})

# ---------------------------------------------------------------------------
# Shell helpers (optional)
# ---------------------------------------------------------------------------
if(PRTERM_BUILD_TOOLS)
    add_executable(prterm-ini tools/prterm_ini.c)
    target_link_libraries(prterm-ini PRIVATE prterm_core)
    target_compile_options(prterm-ini PRIVATE ${PRTERM_WARN_FLAGS})

    add_executable(prterm-probe tools/tnc_probe.c)
    target_link_libraries(prterm-probe PRIVATE prterm_core)
    target_compile_options(prterm-probe PRIVATE ${PRTERM_WARN_FLAGS})
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(BUILD_TESTING)
    foreach(_t ini bands callsign config cgi kiss probe state session arbiter ui trace)
        add_executable(test_${_t} tests/test_${_t}.c)
        target_link_libraries(test_${_t} PRIVATE prterm_core)
        target_compile_options(test_${_t} PRIVATE ${PRTERM_WARN_FLAGS})
        add_test(NAME ${_t} COMMAND test_${_t})
    endforeach()

    # Integration: prterm-tncd against a FAKE TNC on a pseudo terminal.
    # No hardware, no RF - the test plays the TNC2C and checks that the
    # daemon enters KISS once and holds it (see tests/test_tncd.c).
    add_executable(test_tncd tests/test_tncd.c)
    target_link_libraries(test_tncd PRIVATE prterm_core)
    target_compile_options(test_tncd PRIVATE ${PRTERM_WARN_FLAGS})
    add_dependencies(test_tncd prterm-tncd)
    add_test(NAME tncd COMMAND test_tncd $<TARGET_FILE:prterm-tncd>)
    set_tests_properties(tncd PROPERTIES TIMEOUT 120)
endif()

# ---------------------------------------------------------------------------
# Install: deliberately minimal - "no installation" also means that
# simply copying prterm.cgi + prterm.ini is enough.
# ---------------------------------------------------------------------------
install(TARGETS prterm RUNTIME DESTINATION bin)
install(FILES prterm.ini README.md DESTINATION share/prterm)

message(STATUS "PRTERM ${PROJECT_VERSION}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  rig drivers  : sim=${PRTERM_RIG_SIM} tmodem=${PRTERM_RIG_TMODEM}")
message(STATUS "  werror       : ${PRTERM_WERROR}")
message(STATUS "  sanitizers   : ${PRTERM_SANITIZE}")
