# Doctype native plugin.
#
# Used for Android (and would serve Windows/Linux unchanged). macOS is built by
# build_macos.sh, which drives clang directly so that Xcode is the only
# prerequisite there.

cmake_minimum_required(VERSION 3.22)
project(Doctype LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(LITEHTML ${CMAKE_CURRENT_SOURCE_DIR}/third_party/litehtml)

if(NOT EXISTS ${LITEHTML}/CMakeLists.txt)
    message(FATAL_ERROR "litehtml is missing. Run: git clone --depth 1 https://github.com/litehtml/litehtml third_party/litehtml")
endif()

file(GLOB LITEHTML_SOURCES ${LITEHTML}/src/*.cpp)
file(GLOB GUMBO_SOURCES ${LITEHTML}/src/gumbo/*.c)
file(GLOB WRAPPER_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)

add_library(Doctype SHARED
    ${LITEHTML_SOURCES}
    ${GUMBO_SOURCES}
    ${WRAPPER_SOURCES}
)

# litehtml and gumbo include their own headers unqualified ("url.h",
# "attribute.h"), matching the PRIVATE include dirs in their own CMakeLists.
target_include_directories(Doctype PRIVATE
    ${LITEHTML}/include
    ${LITEHTML}/include/litehtml
    ${LITEHTML}/src
    ${LITEHTML}/src/gumbo/include
    ${LITEHTML}/src/gumbo/include/gumbo
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Only the lhu_* entry points are exported; everything else stays internal so
# litehtml's symbols cannot collide with anything else in the player.
set_target_properties(Doctype PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_compile_options(Doctype PRIVATE -fexceptions -frtti)

if(ANDROID)
    target_link_libraries(Doctype PRIVATE log m)
    # Shrink the binary: litehtml pulls in a lot that a given page never uses.
    target_link_options(Doctype PRIVATE -Wl,--gc-sections -Wl,--exclude-libs,ALL)
    target_compile_options(Doctype PRIVATE -ffunction-sections -fdata-sections)
endif()

# Test binaries, for CI and for anyone without a Mac. build_macos.sh remains
# the local path on macOS; these targets exist so a plain Linux runner can
# build and run the same checks. They compile the sources again instead of
# linking the shared library because the tests exercise everything at once and
# hidden visibility keeps the .so's internals unreachable from outside.
option(LHU_BUILD_TESTS "Build lhu_harness and lhu_verify_quadcache" OFF)

if(LHU_BUILD_TESTS)
    set(LHU_TEST_INCLUDES
        ${LITEHTML}/include
        ${LITEHTML}/include/litehtml
        ${LITEHTML}/src
        ${LITEHTML}/src/gumbo/include
        ${LITEHTML}/src/gumbo/include/gumbo
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests
    )

    add_executable(lhu_harness
        tests/harness.cpp
        ${LITEHTML}/containers/test/lodepng.cpp
        ${LITEHTML_SOURCES}
        ${GUMBO_SOURCES}
        ${WRAPPER_SOURCES}
    )
    target_include_directories(lhu_harness PRIVATE
        ${LHU_TEST_INCLUDES}
        ${LITEHTML}/containers/test
    )

    add_executable(lhu_verify_quadcache
        tests/verify_quadcache.cpp
        ${LITEHTML_SOURCES}
        ${GUMBO_SOURCES}
        ${WRAPPER_SOURCES}
    )
    target_include_directories(lhu_verify_quadcache PRIVATE ${LHU_TEST_INCLUDES})
endif()
