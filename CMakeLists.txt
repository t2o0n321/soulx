cmake_minimum_required(VERSION 3.16)

# SoulX builds for macOS on Apple Silicon only: the soulx CLI, its window (AppKit + Metal) and Lua automation.
if(NOT APPLE OR (CMAKE_SYSTEM_NAME AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
    message(FATAL_ERROR "SoulX builds on macOS (Apple Silicon) only")
endif()

# ===========================================================================
# macOS (Apple Silicon): soulx CLI + SoulX's window (AppKit + Metal) + Lua automation
# ===========================================================================

# Native Apple Silicon binary unless the caller asks for something else
if(NOT CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64" CACHE STRING "Target architectures (Apple Silicon)" FORCE)
endif()
# Oldest macOS the binary runs on (docs/USAGE.md: macOS 12 or later). It has to be set before project(),
# which would otherwise record the build host's version (a binary built on macOS 26 refused to start on 14).
# An explicit -DCMAKE_OSX_DEPLOYMENT_TARGET=... or $MACOSX_DEPLOYMENT_TARGET wins.
if(NOT CMAKE_OSX_DEPLOYMENT_TARGET AND "$ENV{MACOSX_DEPLOYMENT_TARGET}" STREQUAL "")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "Minimum macOS version" FORCE)
endif()

# Single source of truth for the version: SOULX_VERSION_STRING in soulx_android.h
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/source/core/soulx_android.h" SOULX_VERSION_LINE
     REGEX "^#define SOULX_VERSION_STRING ")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" SOULX_VERSION "${SOULX_VERSION_LINE}")
if(NOT SOULX_VERSION)
    message(FATAL_ERROR "cannot read SOULX_VERSION_STRING from source/core/soulx_android.h")
endif()

project(SoulXAndroid VERSION ${SOULX_VERSION} LANGUAGES C OBJC)

if(NOT APPLE)
    message(FATAL_ERROR "This configuration builds the macOS port, but the toolchain targets "
                        "${CMAKE_SYSTEM_NAME}. For a cross build pass -DCMAKE_SYSTEM_NAME=<system> "
                        "on the cmake command line.")
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type: Release, Debug, RelWithDebInfo or MinSizeRel" FORCE)
endif()
# Release builds use -O2 (CMake's default is -O3)
string(REPLACE "-O3" "-O2" CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
string(REPLACE "-O3" "-O2" CMAKE_OBJC_FLAGS_RELEASE "${CMAKE_OBJC_FLAGS_RELEASE}")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
# Objective-C (SoulX's window): C11 plus the usual Apple extensions, ARC
set(CMAKE_OBJC_STANDARD 11)
set(CMAKE_OBJC_STANDARD_REQUIRED ON)

message(STATUS "Building SoulX Android ${PROJECT_VERSION} for macOS ${CMAKE_OSX_DEPLOYMENT_TARGET}+ "
               "(${CMAKE_OSX_ARCHITECTURES}, ${CMAKE_BUILD_TYPE})")

set(SOULX_SRC ${CMAKE_CURRENT_SOURCE_DIR}/source)
set(SOULX_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wformat=2)
set(SOULX_OBJC_OPTIONS $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
# SoulX's window: AppKit, Metal (MTKView, runtime-compiled shaders), Core Animation layers for the keymap
# overlay, display timing, file types for the open/save panels. Vision + CoreGraphics: on-device OCR (read_text).
set(SOULX_FRAMEWORKS
    "-framework Cocoa"
    "-framework Metal"
    "-framework MetalKit"
    "-framework QuartzCore"
    "-framework CoreVideo"
    "-framework UniformTypeIdentifiers"
    "-framework Vision"
    "-framework CoreGraphics"
)

# Lua 5.4 (vendored, unmodified): its own static library, compiled the way Lua's makefile
# does it (gnu99) and without warnings. LUA_USE_POSIX, not LUA_USE_MACOSX: the latter also
# enables dlopen(), and scripts must never load native code (package.loadlib, C modules;
# soulx_lua.c removes those from the script state as well).
file(GLOB SOULX_LUA_SOURCES CONFIGURE_DEPENDS ${SOULX_SRC}/third_party/lua/src/*.c)
list(FILTER SOULX_LUA_SOURCES EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")
if(NOT SOULX_LUA_SOURCES)
    message(FATAL_ERROR "Lua 5.4 sources not found in source/third_party/lua/src")
endif()
add_library(soulx_lua STATIC ${SOULX_LUA_SOURCES})
target_include_directories(soulx_lua PUBLIC ${SOULX_SRC}/third_party/lua/src)
target_compile_definitions(soulx_lua PUBLIC LUA_USE_POSIX)
set_target_properties(soulx_lua PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
target_compile_options(soulx_lua PRIVATE -w)

# soulx_core: the macOS backend (source/platform/macos), SoulX's window (source/platform/macos/window,
# C and Objective-C) and the script engine (source/script)
file(GLOB SOULX_CORE_SOURCES CONFIGURE_DEPENDS
    ${SOULX_SRC}/platform/macos/*.c
    ${SOULX_SRC}/platform/macos/*.m
    ${SOULX_SRC}/platform/macos/window/*.c
    ${SOULX_SRC}/platform/macos/window/*.m
    ${SOULX_SRC}/script/*.c
)
if(NOT SOULX_CORE_SOURCES)
    message(FATAL_ERROR "no C sources found in source/platform/macos or source/script")
endif()
add_library(soulx_core STATIC ${SOULX_CORE_SOURCES})
target_include_directories(soulx_core PUBLIC
    ${SOULX_SRC}/core
    ${SOULX_SRC}/platform/macos
    ${SOULX_SRC}/script
    ${SOULX_SRC}/third_party/lua/src
    ${SOULX_SRC}/third_party/stb
)
target_compile_options(soulx_core PRIVATE ${SOULX_WARNINGS} ${SOULX_OBJC_OPTIONS})
target_link_libraries(soulx_core PUBLIC soulx_lua m ${SOULX_FRAMEWORKS})

# soulx: the command line tool, which is also the window app ('soulx window NAME')
add_executable(soulx ${SOULX_SRC}/native/main_macos.c)
target_compile_options(soulx PRIVATE ${SOULX_WARNINGS})
target_link_libraries(soulx PRIVATE soulx_core)

install(TARGETS soulx RUNTIME DESTINATION bin)

# Unit tests: one executable and one CTest test per tests/macos/test_*.c (or .m), linked with soulx_core.
# 'cmake --build build/macos --target soulx_tests' builds them all; 'ctest' runs them.
option(SOULX_BUILD_TESTS "Build the macOS unit tests" ON)
if(SOULX_BUILD_TESTS)
    enable_testing()
    file(GLOB SOULX_TEST_SOURCES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/macos/test_*.c
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/macos/test_*.m
    )
    add_custom_target(soulx_tests)
    foreach(SOULX_TEST_SRC ${SOULX_TEST_SOURCES})
        get_filename_component(SOULX_TEST_NAME ${SOULX_TEST_SRC} NAME_WE)
        add_executable(${SOULX_TEST_NAME} ${SOULX_TEST_SRC})
        target_compile_options(${SOULX_TEST_NAME} PRIVATE ${SOULX_WARNINGS} ${SOULX_OBJC_OPTIONS})
        target_link_libraries(${SOULX_TEST_NAME} PRIVATE soulx_core)
        set_target_properties(${SOULX_TEST_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests)
        add_test(NAME ${SOULX_TEST_NAME} COMMAND ${SOULX_TEST_NAME})
        # Safety net: a test that forgets to point SOULX_HOME at its own temporary folder still
        # never touches the real ~/.soulx-android
        set_tests_properties(${SOULX_TEST_NAME} PROPERTIES
            ENVIRONMENT "SOULX_HOME=${CMAKE_BINARY_DIR}/tests/soulx-home"
            TIMEOUT 120)
        add_dependencies(soulx_tests ${SOULX_TEST_NAME})
    endforeach()
endif()
