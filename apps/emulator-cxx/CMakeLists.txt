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
    src/core/text_gdi.cpp
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
    src/window.cpp
    src/input_script.cpp
    src/play_headless.cpp)
target_link_libraries(emulator-cxx PRIVATE emulator-core)

enable_testing()
add_test(NAME emulator-cxx-smoke COMMAND emulator-cxx --smoke)

add_executable(emulator-core-selftest
    tests/core_selftest.cpp)
target_link_libraries(emulator-core-selftest PRIVATE emulator-core)
# ★ 自检要**读** shape 来判"实现的槽量 == shape 声明"（音效槽表 10 / 后端 15）：
#   把仓库里的文件路径注进去，**不拷贝**进构建目录 —— 拷贝会制造"文件在就算对"的假象。
target_compile_definitions(emulator-core-selftest PRIVATE
    EMULATOR_CXX_SHAPE_TYPES_JSON="${CMAKE_CURRENT_SOURCE_DIR}/../../data/shape/types.json")
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
# ★ 音效 / 配音的**子集**单独注册一条：spec 的 evidence 要点到"某条用例"，聚合的那一条点不出子集
#   （`CORE-OPS-AUDIO-WAV` / `-BGM-FADE` / `CORE-OPS-VOICE-PLAY` 的证据都指这里）。
add_test(NAME emulator-core-audio-voice COMMAND emulator-core-selftest --only-audio)
# ★ 两套字号（`0x75` = A 支 / `0x197` = B 支）单独一条：spec 的 `CORE-OPS-FORWARD-PROLOGUE` 证据指这里。
add_test(NAME emulator-core-font-size COMMAND emulator-core-selftest --only-font)

set(EMULATOR_CXX_INSTALL_DIR "" CACHE PATH "Optional game install dir for the boot-flow tests")
if(EMULATOR_CXX_INSTALL_DIR)
    # 唯一的游戏流程是 boot（根脚本 → LOGO → 影片 → TITLE）：没有"直接跳到 TITLE"的测试档。
    add_test(NAME emulator-core-boot-play
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}" --play-frames 1)
    # `--skip-logo` 走引擎的 ShowLogo 格（`0x130`），不是宿主捷径；仍必须到 TITLE。
    add_test(NAME emulator-core-boot-play-skip-logo
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}" --skip-logo --play-frames 1)
    # 端到端：关闭请求 → 保存 SYS4REG.INI；检查完整持久键（不是只测 writer）。
    add_test(NAME emulator-core-config-save-exit
        COMMAND ${CMAKE_COMMAND}
            -DEXE=$<TARGET_FILE:emulator-cxx>
            -DINSTALL=${EMULATOR_CXX_INSTALL_DIR}
            -DINI=${CMAKE_CURRENT_BINARY_DIR}/config-exit-save.ini
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_save_exit.cmake)
    # 影片：真 MPEG-1 program stream（LOGO.MPG）→ 门前服务 → 同槽纹理 → GPU 场景批次。
    # `--logo-frames 0` 只跑到收尾（判据在程序里：必须有帧、必须走到收尾）。
    add_test(NAME emulator-core-logo-movie
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}"
                --render-logo-gpu "${CMAKE_CURRENT_BINARY_DIR}/logo-movie" --logo-frames 0)
    add_test(NAME emulator-core-logo-gpu
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}"
                --render-logo-gpu "${CMAKE_CURRENT_BINARY_DIR}/logo-frames" --logo-stride 80 --logo-frames 2)
    # ★ TITLE → Option → CONFIG.BIN（设置界面）：宿主脚本化输入（`--input-script`）把光标搬到
    #   Option 按钮上并按一次左键，判据是**真的进了 CONFIG.BIN（统一文件 id 52）**。
    #   这是"打开设置界面的路径"的入口判据；⛔ 它不断言画面像素（没有像素 oracle）。
    add_test(NAME emulator-core-title-option
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}"
                --skip-logo --play-headless
                --input-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/input/title-option.txt"
                --anchor-script 21092 --expect-script 52 --tick-ms 16 --max-steps 400000)
    # ★ 再往前一步：CONFIG.BIN 会调起 **CONFIG1.BIN（统一文件 id 20940）** —— 设置界面的实际脚本。
    #   这条判据比上一条强：它要求 CONFIG.BIN 的装载段真的跑起来（不是"进了脚本就停"）。
    add_test(NAME emulator-core-title-option-config1
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}"
                --skip-logo --play-headless
                --input-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/input/title-option.txt"
                --anchor-script 21092 --expect-script 20940 --tick-ms 16 --max-steps 600000)
    # ★ 设置界面"真的在跑"：从 TITLE 点 Option 一路到 CONFIG1.BIN（20940）并**留在它的输入环里**，
    #   期间不许出现"未实现 / 非法 opcode / 内部错误"的停机。少实现任何一条这条都会红。
    add_test(NAME emulator-core-config-screen
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}"
                --skip-logo --play-headless
                --input-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/input/title-option.txt"
                --anchor-script 21092 --require-script 20940 --tick-ms 16 --max-steps 300000)
    # ★ 设置界面底部**示例文本**：文本层画布（纹理 29）必须存在、尺寸 = 824×120、而且**有墨**。
    #   这条是文本子系统（0x70/0x71/0x79/0x213 + 0x6e/0x196/0x6f）的资产档入口判据：
    #   任何一环断了（层矩形 / 画布创建 / 清空 / 文本追加 / 栅格化）都会红。
    add_test(NAME emulator-core-config-sample-text
        COMMAND ${CMAKE_COMMAND}
            -DEXE=$<TARGET_FILE:emulator-cxx>
            -DINSTALL=${EMULATOR_CXX_INSTALL_DIR}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests
            -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_sample_text.cmake)
    # ★ 设置界面**每一页**一条：点该 tab（判据 = 停在期望脚本 **且** 该页命中项数量对得上）+
    #   同一页滚 6 格（判据 = 一次都不许"未实现 / 非法 opcode"停机）。细节见 tests/config_pages.cmake。
    #   ★ 旧版是"每个 tab 一条 add_test + 只查还在 20940 里"：那个判据**恒真**（点错 tab 也仍在同一个
    #     脚本里），而坐标又整体偏了一个 tab ⇒ 一路绿却什么也没验。这里换成"数量 + 脚本 id + 滚动"。
    add_test(NAME emulator-core-config-pages
        COMMAND ${CMAKE_COMMAND}
            -DEXE=$<TARGET_FILE:emulator-cxx>
            -DINSTALL=${EMULATOR_CXX_INSTALL_DIR}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_pages.cmake)
    # ★ 设置界面的**命中查找**：光标在右侧 ON 按钮上 ⇒ 命中下标 ≥ 0；在列表正文里 ⇒ −1。
    #   两半一起才有效（只有一半会"恒绿"）。红 = 右列控件又命中不到了（悬停 / 点击会一起失效）。
    add_test(NAME emulator-core-config-hover-hit
        COMMAND ${CMAKE_COMMAND}
            -DEXE=$<TARGET_FILE:emulator-cxx>
            -DINSTALL=${EMULATOR_CXX_INSTALL_DIR}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_hit_test.cmake)
    # ★ 设置界面的**开关真的写配置**：点 ON 与点 OFF 的配置快照必须不同、且 OFF ⇒ 0。
    #   红 = 右列开关点了没反应（缺 handler / 写错键 / 走了早退却不写键）。
    add_test(NAME emulator-core-config-toggle
        COMMAND ${CMAKE_COMMAND}
            -DEXE=$<TARGET_FILE:emulator-cxx>
            -DINSTALL=${EMULATOR_CXX_INSTALL_DIR}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests
            -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_toggle.cmake)
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

