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

# macOS **不需要** shadercross：Metal 后端收 MSL **源码**（`SDL_GPU_SHADERFORMAT_MSL`），
# 由 Metal 自己在运行时编译，所以本仓在 macOS 上直接喂 MSL（见 src/gpu_quad.cpp 顶部）。
# 反过来说 shadercross 在 macOS 上装不上：它依赖 directx-dxc，而那个 port 只支持 windows / linux-x64。
# ⇒ 打开开关时给一句人话，而不是让 find_package 抛一句难懂的错。
if(APPLE AND EMULATOR_CXX_WITH_SHADER)
    message(WARNING
        "macOS 走 MSL 源（Metal 运行时编译），不需要 shadercross；"
        "已忽略 EMULATOR_CXX_WITH_SHADER（sdl3-shadercross 依赖 directx-dxc，只在 windows / linux-x64 有）。")
    set(EMULATOR_CXX_WITH_SHADER OFF)
endif()

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
    src/core/encoding.cpp
    src/core/script.cpp
    src/core/scene_raster.cpp
    src/core/text_raster.cpp
    src/core/movie.cpp
    src/core/opcode_table.cpp
    src/core/vm.cpp
    src/core/vm_value.cpp
    src/core/vm_timing.cpp
    src/core/vm_movie.cpp
    src/core/vm_ops_flow.cpp
    src/core/vm_ops_texture.cpp
    src/core/vm_ops_scene.cpp
    src/core/vm_ops_render.cpp
    src/core/vm_ops_engine.cpp
    src/core/vm_ops_save.cpp
    src/core/save_bmp.cpp
    src/core/vm_ops_numeric.cpp
    src/core/vm_ops_text.cpp
    src/core/vm_ops_io.cpp
    src/core/install.cpp)
target_include_directories(emulator-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# pl_mpeg 是第三方单文件解码器（见 third_party/pl_mpeg/README.md）；只有 movie.cpp 编译它。
target_include_directories(emulator-core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pl_mpeg)
target_link_libraries(emulator-core PUBLIC nlohmann_json::nlohmann_json)
target_compile_features(emulator-core PUBLIC cxx_std_20)

# 解码是第三方代码、且是热点：Debug 构建也按优化编译它（引擎逻辑仍在 /Od 下单步）。
if(MSVC)
    set_source_files_properties(src/core/movie.cpp PROPERTIES COMPILE_OPTIONS "/O2")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    set_source_files_properties(src/core/movie.cpp PROPERTIES COMPILE_OPTIONS "-O2")
endif()

add_executable(emulator-cxx
    src/main.cpp
    src/gpu_device.cpp
    src/gpu_quad.cpp
    src/gpu_scene.cpp
    src/smoke.cpp
    src/boot.cpp
    src/window.cpp)
target_link_libraries(emulator-cxx PRIVATE emulator-core)

enable_testing()
add_test(NAME emulator-cxx-smoke COMMAND emulator-cxx --smoke)

add_executable(emulator-core-selftest
    tests/core_selftest.cpp)
target_link_libraries(emulator-core-selftest PRIVATE emulator-core)
# 自检脚本把十几台 `Machine` 放在同一个栈帧里（Debug /Od 不复用栈槽）；
# 默认 1 MiB 栈在夹具变多之后会撞上（0xC00000FD）。这是**测试**的显式预算，不是产品设置。
if(WIN32)
    target_link_options(emulator-core-selftest PRIVATE "/STACK:8388608")
elseif(APPLE)
    # ★ ld64 不认 GNU ld 的 `-z`（实测 `ld: unknown options: -z`）；macOS 的写法是 `-stack_size`。
    # 主线程在 macOS 上默认就是 8 MiB，这里只是把预算**钉住**，不靠平台默认值。
    target_link_options(emulator-core-selftest PRIVATE "-Wl,-stack_size,0x800000")
else()
    target_link_options(emulator-core-selftest PRIVATE "-Wl,-z,stack-size=8388608")
endif()
add_test(NAME emulator-core-selftest COMMAND emulator-core-selftest)

set(EMULATOR_CXX_INSTALL_DIR "" CACHE PATH "Optional game install dir for the boot-flow tests")
if(EMULATOR_CXX_INSTALL_DIR)
    # 唯一的游戏流程是 boot（根脚本 → LOGO → 影片 → TITLE）：没有"直接跳到 TITLE"的测试档。
    add_test(NAME emulator-core-boot-play
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}" --play-frames 1)
    # 影片：真 MPEG-1 program stream（LOGO.MPG）→ 门前服务 → 同槽纹理 → GPU 场景批次。
    # `--logo-frames 0` 只跑到收尾（判据在程序里：必须有帧、必须走到收尾）。
    add_test(NAME emulator-core-logo-movie
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}"
                --render-logo-gpu "${CMAKE_CURRENT_BINARY_DIR}/logo-movie" --logo-frames 0)
    add_test(NAME emulator-core-logo-gpu
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}"
                --render-logo-gpu "${CMAKE_CURRENT_BINARY_DIR}/logo-frames" --logo-stride 80 --logo-frames 2)
endif()

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
    find_package(Freetype REQUIRED)
    target_compile_definitions(emulator-core PRIVATE
        EMULATOR_CXX_HAVE_TEXT=1
        EMULATOR_CXX_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../../corpus/assets/fonts/Amayui-CN_cnjp.ttf")
    target_link_libraries(emulator-core PRIVATE Freetype::Freetype)
    if(NOT WIN32)
        # Windows 用 MultiByteToWideChar(932)；其它平台用系统 iconv（glibc / macOS 自带）。
        find_package(Iconv REQUIRED)
        target_compile_definitions(emulator-core PRIVATE EMULATOR_CXX_HAVE_ICONV=1)
        target_link_libraries(emulator-core PRIVATE Iconv::Iconv)
    endif()
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

