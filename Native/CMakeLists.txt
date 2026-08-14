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
