cmake_minimum_required(VERSION 3.24)

project(emulator-cxx
    VERSION 0.1.0
    DESCRIPTION "Amayui clean-room emulator (native C++ host)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(EMULATOR_CXX_WITH_BGFX "Link bgfx (vcpkg feature: renderer)" OFF)
option(EMULATOR_CXX_WITH_SHADER "Link SDL_shadercross (vcpkg feature: shader)" OFF)
option(EMULATOR_CXX_WITH_TEXT "Link FreeType/HarfBuzz (vcpkg feature: text)" OFF)
option(EMULATOR_CXX_WITH_VIDEO "Link FFmpeg (vcpkg feature: video)" OFF)

# vcpkg 通过 toolchain 注入前缀；这里只做显式检查，缺依赖时给出可读的失败信息。
if(NOT DEFINED VCPKG_TARGET_TRIPLET AND NOT DEFINED ENV{VCPKG_ROOT})
    message(WARNING
        "没有检测到 vcpkg toolchain。请通过 CMakePresets.json 配置 VCPKG_ROOT，"
        "或用 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake。")
endif()

find_package(SDL3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

add_library(emulator-core STATIC
    src/core/lzss.cpp
    src/core/alf.cpp
    src/core/agf.cpp
    src/core/config.cpp
    src/core/script.cpp
    src/core/opcode_table.cpp
    src/core/vm.cpp
    src/core/install.cpp)
target_include_directories(emulator-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(emulator-core PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(emulator-core PUBLIC cxx_std_20)

add_executable(emulator-cxx
    src/main.cpp)
target_link_libraries(emulator-cxx PRIVATE emulator-core)

enable_testing()
add_test(NAME emulator-cxx-smoke COMMAND emulator-cxx --smoke)

add_executable(emulator-core-selftest
    tests/core_selftest.cpp)
target_link_libraries(emulator-core-selftest PRIVATE emulator-core)
add_test(NAME emulator-core-selftest COMMAND emulator-core-selftest)

target_compile_features(emulator-cxx PRIVATE cxx_std_20)
target_link_libraries(emulator-cxx PRIVATE SDL3::SDL3)

if(EMULATOR_CXX_WITH_SHADER)
    find_package(SDL3_shadercross CONFIG REQUIRED)
    target_compile_definitions(emulator-cxx PRIVATE EMULATOR_CXX_HAVE_SHADERCROSS=1)
    target_link_libraries(emulator-cxx PRIVATE SDL3_shadercross::SDL3_shadercross)
endif()

if(EMULATOR_CXX_WITH_BGFX)
    find_package(bgfx CONFIG REQUIRED)
    target_compile_definitions(emulator-cxx PRIVATE EMULATOR_CXX_HAVE_BGFX=1)
    target_link_libraries(emulator-cxx PRIVATE bgfx::bgfx)
endif()

if(EMULATOR_CXX_WITH_TEXT)
    find_package(Freetype CONFIG REQUIRED)
    find_package(harfbuzz CONFIG REQUIRED)
    target_compile_definitions(emulator-cxx PRIVATE EMULATOR_CXX_HAVE_TEXT=1)
    target_link_libraries(emulator-cxx PRIVATE Freetype::Freetype harfbuzz::harfbuzz)
endif()

if(EMULATOR_CXX_WITH_VIDEO)
    find_package(FFMPEG REQUIRED)
    target_compile_definitions(emulator-cxx PRIVATE EMULATOR_CXX_HAVE_FFMPEG=1)
    target_link_libraries(emulator-cxx PRIVATE FFMPEG::avformat FFMPEG::avcodec FFMPEG::swscale)
endif()

if(MSVC)
    target_compile_options(emulator-cxx PRIVATE /W4 /utf-8 /permissive-)
else()
    target_compile_options(emulator-cxx PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_custom_command(TARGET emulator-cxx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/../../data/spec/format/opcode-table.json
        $<TARGET_FILE_DIR:emulator-cxx>/opcode-table.json)
add_custom_command(TARGET emulator-cxx POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${CMAKE_CURRENT_SOURCE_DIR}/../../data/spec/format/config-keys.json
        $<TARGET_FILE_DIR:emulator-cxx>/config-keys.json)
