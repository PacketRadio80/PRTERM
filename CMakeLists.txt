cmake_minimum_required(VERSION 3.16)

project(prterm
    VERSION 0.1.0
    DESCRIPTION "PRTERM - CB & Amateurfunk Terminal (CGI)"
    LANGUAGES C)

# ---------------------------------------------------------------------------
# Portability baseline
#   Zielplattformen: Linux + FreeBSD, x86-64 + arm64 (spaeter ggf. mehr)
#
#   Strict C11 + POSIX.1-2008/XSI. Bewusst KEINE GNU-Extensions und kein
#   _GNU_SOURCE - der Code muss auf FreeBSD unveraendert compilieren.
#   Konkret vermieden:
#     cfcmakeraw()     -> eigener Ersatz (cfmakeraw ist BSD/GNU, nicht POSIX)
#     flock()          -> fcntl()-Locks (F_SETLK) sind portabel
#     epoll/timerfd    -> poll() fuer alles
#     getrandom()      -> /dev/urandom
#     /proc/self/exe   -> nicht portabel, Pfade kommen aus der INI
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
set(PRTERM_RIG_MAX25   OFF CACHE BOOL "Build the MAX25-Stack TNC driver")

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

# POSIX.1-2008 ist die Untergrenze fuer Linux und FreeBSD. Wird zusaetzlich
# in jeder .c ueber src/prterm_compat.h gesetzt, damit auch ein von Hand
# ausgefuehrter Compiler-Lauf funktioniert.
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

# ---------------------------------------------------------------------------
# Core library (also linked by the tests)
# ---------------------------------------------------------------------------
set(PRTERM_CORE_SOURCES
    src/util.c
    src/sha256.c
    src/ini.c
    src/bands.c
    src/config.c
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
if(PRTERM_RIG_MAX25)
    list(APPEND PRTERM_CORE_SOURCES src/max25.c)
endif()

add_library(prterm_core STATIC ${PRTERM_CORE_SOURCES})
target_include_directories(prterm_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(prterm_core PRIVATE ${PRTERM_WARN_FLAGS})

foreach(_def IN ITEMS
        PRTERM_RIG_SIM=$<BOOL:${PRTERM_RIG_SIM}>
        PRTERM_RIG_TNC2=$<BOOL:${PRTERM_RIG_TNC2}>
        PRTERM_RIG_TMODEM=$<BOOL:${PRTERM_RIG_TMODEM}>
        PRTERM_RIG_MAX25=$<BOOL:${PRTERM_RIG_MAX25}>)
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
    foreach(_t ini bands callsign config cgi kiss probe state)
        add_executable(test_${_t} tests/test_${_t}.c)
        target_link_libraries(test_${_t} PRIVATE prterm_core)
        target_compile_options(test_${_t} PRIVATE ${PRTERM_WARN_FLAGS})
        add_test(NAME ${_t} COMMAND test_${_t})
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Install: bewusst minimal - "keine Installation" heisst auch, dass ein
# einfaches Kopieren von prterm.cgi + prterm.ini genuegt.
# ---------------------------------------------------------------------------
install(TARGETS prterm RUNTIME DESTINATION bin)
install(FILES prterm.ini README.md DESTINATION share/prterm)

message(STATUS "PRTERM ${PROJECT_VERSION}")
message(STATUS "  build type   : ${CMAKE_BUILD_TYPE}")
message(STATUS "  rig drivers  : sim=${PRTERM_RIG_SIM} tmodem=${PRTERM_RIG_TMODEM} max25=${PRTERM_RIG_MAX25}")
message(STATUS "  werror       : ${PRTERM_WERROR}")
message(STATUS "  sanitizers   : ${PRTERM_SANITIZE}")
