cmake_minimum_required(VERSION 3.24)

project(emulator-lift
    VERSION 0.1.0
    DESCRIPTION "Run AGE.EXE's real machine code on a guest-state x86 interpreter (SDL3 host)"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# vcpkg 由 CMakePresets 的 toolchain 注入；这里只做显式检查，缺依赖时给可读的失败信息。
if(NOT DEFINED VCPKG_TARGET_TRIPLET AND NOT DEFINED ENV{VCPKG_ROOT})
    message(WARNING
        "没有检测到 vcpkg toolchain。请用 CMakePresets.json（跨目录复用 ../emulator-cxx/vcpkg），"
        "或用 -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake。")
endif()

find_package(SDL3 CONFIG REQUIRED)
# Zydis 只**解码**，语义是我们自己的（plan §7.1）。
find_package(Zydis CONFIG REQUIRED)

# ---- 第三方：Berkeley SoftFloat 3e（D3；vendor，不进 vcpkg）----
add_subdirectory(third_party/berkeley-softfloat)

# ---- 运行时 ----
add_library(emulator-lift-runtime STATIC
    src/runtime/memory.cpp
    src/runtime/sha256.cpp
    src/runtime/image.cpp
    src/runtime/mnemonics.cpp
    src/runtime/decode.cpp
    src/runtime/x86.cpp
    src/runtime/guest_call.cpp
    src/runtime/host_window.cpp
    src/runtime/pe_resource.cpp
    src/runtime/surface.cpp
    src/runtime/present.cpp
    src/runtime/ddraw_surface.cpp
    src/runtime/com.cpp
    src/runtime/machine.cpp
    src/runtime/exec.cpp
    src/runtime/exec_integer.cpp
    src/runtime/exec_control.cpp
    src/runtime/exec_string.cpp
    src/runtime/exec_fpu.cpp
    src/runtime/fpu_bridge.cpp
    src/runtime/cp932.cpp
    src/runtime/vfs.cpp
    src/runtime/handles.cpp
    src/runtime/heap.cpp
    src/runtime/shims.cpp
    src/corpus/lst_index.cpp
    src/shims/register.cpp
    src/shims/register_import_gaps.cpp
    src/shims/kernel32_sync.cpp
    src/shims/kernel32_mem.cpp
    src/shims/kernel32_error_tls.cpp
    src/shims/kernel32_process.cpp
    src/shims/kernel32_file.cpp
    src/shims/kernel32_time.cpp
    src/shims/kernel32_string.cpp
    src/shims/kernel32_agerc.cpp
    src/shims/kernel32_version.cpp
    src/shims/user32_menu.cpp
    src/shims/user32_window.cpp
    src/shims/user32_input.cpp
    src/shims/user32_resource.cpp
    src/shims/gdi32_boot.cpp
    src/shims/ddraw.cpp
    src/shims/dynamic_exports.cpp
    src/shims/shell32.cpp
    # ★ P4 小族（只追加源文件）：shlwapi 路径族 + imagehlp 递归建目录
    src/shims/shlwapi.cpp
    src/shims/imagehlp.cpp
    src/shims/advapi32_registry.cpp
    src/shims/winmm_audio.cpp
    # ★ M2 · DirectSound silent 层（SCOPE §1「音频 = 延后实现（silent）」）：
    #   `LoadLibraryA("DSOUND.DLL")` 的假句柄 + `DirectSoundCreate` 动态导出 + IDirectSound8 /
    #   IDirectSoundBuffer8（`Lock` 给真 guest 内存，`Play`/`Set*` 记账）。⛔ 不静默 no-op。
    src/shims/dsound.cpp
    src/shims/winmm_joystick.cpp
    src/shims/mf_video.cpp
    src/shims/kernel32_bootstrap.cpp)
target_include_directories(emulator-lift-runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(emulator-lift-runtime
    PUBLIC SDL3::SDL3
    PRIVATE Zydis::Zydis berkeley-softfloat)
target_compile_features(emulator-lift-runtime PUBLIC cxx_std_20)

# ---- 警告即错误：按**驱动**分档，⛔ 不能写一行 `-Wall -Wextra -Werror` 了事 ----
# ★ 为什么（2026-11-04 实测 + 用户确认）：mac 侧那批**更强的警告**是靠 GNU 驱动的 `-Wall -Wextra` 表达的；
#   而 **clang-cl 把 `-Wall` 当 MSVC 的 `/Wall`**，那等于 `-Weverything`（`-Wc++98-compat` /
#   `-Wold-style-cast` / `-Wdeprecated-declarations` 全开）⇒ 同一行 flag 在 Windows 上把整棵树编挂。
# ★ 于是：MSVC 口径（`cl` 与 `clang-cl`）用 `/W4 /WX`（等价档，且 `/WX` 是真的"警告即错误"，
#   不像 `-Werror` 会被 `cl` 当 D9002 静默忽略）；GNU 口径仍 `-Wall -Wextra -Werror`。
#   ⛔ 不要为了"能编过"把 `-WX` / `-Werror` 删掉 —— 那会让 `chore: enable werror` 的意图归零。
if(MSVC)
    # `/utf-8`：源码里有中日文注释；MSVC 默认按本地码页读，会把它们读坏（clang-cl 同理）
    # `_CRT_SECURE_NO_WARNINGS`：`getenv` / `fopen` 这类**可移植**用法在 MSVC 里被标成 deprecated；
    #   这不是本仓要治的问题，显式关掉比逐个加 pragma 干净。
    target_compile_options(emulator-lift-runtime PRIVATE /W4 /WX /permissive- /utf-8)
    target_compile_definitions(emulator-lift-runtime PRIVATE _CRT_SECURE_NO_WARNINGS)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        # clang-cl 认 GNU 形态的 `-Wno-*`：把 GNU 那行**原本就关着**的两条照旧关掉（口径对齐，不是放宽）
        target_compile_options(emulator-lift-runtime PRIVATE
            -Wno-unused-variable -Wno-unused-const-variable)
    endif()
else()
    target_compile_options(emulator-lift-runtime PRIVATE -Wall -Wextra -Werror
        -Wno-unused-variable -Wno-unused-const-variable)
endif()

add_executable(emulator-lift src/main.cpp)
target_link_libraries(emulator-lift PRIVATE emulator-lift-runtime)

enable_testing()

# ---- pure：宿主层 smoke（不建窗口、不需要游戏）----
add_test(NAME emulator-lift-smoke COMMAND emulator-lift --smoke)

# =====================================================================================
# 测试分级（AGENTS.md §5「CTest 那一档不写 pragma，判据是注册位置」）：
#   - **顶层无条件注册** ⇒ 默认 ctest 就跑（不需要游戏 / LFS）⇒ pure
#   - 落在 if(...) / foreach / function 里 ⇒ 需要 LFS 资产或游戏安装 ⇒ assets
# ⛔ 不要把 pure 的测试塞进 if()：那会让"没跑"与"跑绿了"在输出里长得一样。
# =====================================================================================

# ---- pure：自造机器码字节，不碰游戏 / LFS ----
add_executable(lift-conformance tests/conformance.cpp tests/conformance_cases.cpp)
target_link_libraries(lift-conformance PRIVATE emulator-lift-runtime)
add_test(NAME conformance COMMAND lift-conformance)

# ---- pure：A1 的第二遍 —— 同一批语义夹具在 LIFT_SAFE_MEMORY=1（逐页提交检查）下重跑 ----
# ★ 以前只有 README 里一句人工命令 ⇒ 这一档没有门禁（plan §5 A1 要求"复跑全绿"）。
add_test(NAME conformance-safe COMMAND lift-conformance)
set_tests_properties(conformance-safe PROPERTIES ENVIRONMENT "LIFT_SAFE_MEMORY=1")

add_executable(lift-dispatch tests/dispatch.cpp)
target_link_libraries(lift-dispatch PRIVATE emulator-lift-runtime)
add_test(NAME dispatch COMMAND lift-dispatch)

# ---- pure：guest 页提交 / 退回（宿主页可能大于 guest 页；Apple Silicon = 16 KiB）----
add_executable(lift-memory-pages tests/memory_pages.cpp)
target_link_libraries(lift-memory-pages PRIVATE emulator-lift-runtime)
add_test(NAME memory-pages COMMAND lift-memory-pages)

# ---- pure：PE 载入的坏输入（自造最小 PE；坏节 / 坏导入必须拒绝启动）----
add_executable(lift-pe-loader tests/pe_loader.cpp)
target_link_libraries(lift-pe-loader PRIVATE emulator-lift-runtime)
add_test(NAME pe-loader COMMAND lift-pe-loader)

# ---- pure：fault 归因（宿主 AV 不许被报成"上一次 guest 访存"）----
# ★ 顶层无条件注册 ⇒ pure 档；两平台都**真跑**（POSIX = fork + 管道，Windows = 同一个 exe 再启动一次
#   + 临时文件，平台差异在 tests/fault_report.cpp 内部用 `#ifdef` 处理）。
#   ⛔ 不 LIFT_SKIP、⛔ 不因为平台差异把它降成 assets（REQ-01M6JFZFN15MBPG7Y89WNZACFW #1）。
add_executable(lift-fault-report tests/fault_report.cpp)
target_link_libraries(lift-fault-report PRIVATE emulator-lift-runtime)
add_test(NAME fault-report COMMAND lift-fault-report)

add_executable(lift-shim-abi tests/shim_abi.cpp)
target_link_libraries(lift-shim-abi PRIVATE emulator-lift-runtime)
add_test(NAME shim-abi COMMAND lift-shim-abi)

# ---- pure：P0 嵌套 guest 回调栈 + headless 窗口 / 消息队列 ----
add_executable(lift-guest-call tests/guest_call.cpp)
target_link_libraries(lift-guest-call PRIVATE emulator-lift-runtime)
add_test(NAME guest-call COMMAND lift-guest-call)

add_executable(lift-host-window tests/host_window.cpp)
target_link_libraries(lift-host-window PRIVATE emulator-lift-runtime)
add_test(NAME host-window COMMAND lift-host-window)

add_executable(lift-user32-window tests/user32_window.cpp)
target_link_libraries(lift-user32-window PRIVATE emulator-lift-runtime)
add_test(NAME user32-window COMMAND lift-user32-window)

add_executable(lift-surface-present tests/surface_present.cpp)
target_link_libraries(lift-surface-present PRIVATE emulator-lift-runtime)
add_test(NAME surface-present COMMAND lift-surface-present)

add_executable(lift-mechanism tests/mechanism.cpp)
target_link_libraries(lift-mechanism PRIVATE emulator-lift-runtime)
add_test(NAME mechanism COMMAND lift-mechanism)

# ---- pure：P1 / B2 —— DirectDraw 2D 核心（动态导出 + COM 对象 + surface/Blt/Lock/Flip/枚举）----
add_executable(lift-ddraw tests/ddraw.cpp)
target_link_libraries(lift-ddraw PRIVATE emulator-lift-runtime)
add_test(NAME ddraw COMMAND lift-ddraw)

# ---- pure：M2 —— DirectSound **silent 层**（SCOPE §1「音频 = 延后实现（silent）」）----
# 判据：`LoadLibraryA("DSOUND.DLL")` + `GetProcAddress("DirectSoundCreate")` → 假模块句柄 + 动态导出；
#       IDirectSound8 / IDirectSoundBuffer8 的取证槽；`Lock` 给**真 guest 内存**（引擎往里写 PCM）、
#       `Unlock` / `Play` 记账，日志里能断言"累计 PCM 字节 / Play 了几次"（见 tests/dsound.cpp）。
add_executable(lift-dsound tests/dsound.cpp)
target_link_libraries(lift-dsound PRIVATE emulator-lift-runtime)
add_test(NAME dsound COMMAND lift-dsound)

add_executable(lift-pe-resource tests/pe_resource.cpp)
target_link_libraries(lift-pe-resource PRIVATE emulator-lift-runtime)
add_test(NAME pe-resource COMMAND lift-pe-resource)
set_tests_properties(pe-resource PROPERTIES SKIP_RETURN_CODE 77)

add_executable(lift-agerc-module tests/agerc_module.cpp)
target_link_libraries(lift-agerc-module PRIVATE emulator-lift-runtime)
add_test(NAME agerc-module COMMAND lift-agerc-module)
set_tests_properties(agerc-module PROPERTIES SKIP_RETURN_CODE 77)

add_executable(lift-mnemonics tests/mnemonics.cpp)
target_link_libraries(lift-mnemonics PRIVATE emulator-lift-runtime)
add_test(NAME mnemonics COMMAND lift-mnemonics)

# ---- pure：码页 / 字符串（wsprintfA varargs + CP932 表）----
add_executable(lift-strfmt tests/strfmt.cpp)
target_link_libraries(lift-strfmt PRIVATE emulator-lift-runtime)
add_test(NAME strfmt COMMAND lift-strfmt)

# ---- pure：码页族（MultiByteToWideChar / WideCharToMultiByte 走真 CP932 表）----
add_executable(lift-codepage tests/codepage.cpp)
target_link_libraries(lift-codepage PRIVATE emulator-lift-runtime)
add_test(NAME codepage COMMAND lift-codepage)

# ---- pure：明确 no-op 的三族（注册表 / 音频 / 视频；SCOPE §1）----
add_executable(lift-noop tests/noop_shims.cpp)
target_link_libraries(lift-noop PRIVATE emulator-lift-runtime)
add_test(NAME noop COMMAND lift-noop)

# ---- pure：文件与路径族（S6；临时目录，不碰游戏）----
add_executable(lift-file-io tests/file_io.cpp)
target_link_libraries(lift-file-io PRIVATE emulator-lift-runtime)
add_test(NAME file-io COMMAND lift-file-io)

# ---- pure：本轮从 stub 变真实现的四族（M1c）----
# ★ 判据是 A5（七族内的导入不许是 stub）：每族一组行为夹具，见 tests/*_ext.cpp 的文件头。
add_executable(lift-sync-ext tests/sync_ext.cpp)
target_link_libraries(lift-sync-ext PRIVATE emulator-lift-runtime)
add_test(NAME sync-ext COMMAND lift-sync-ext)

add_executable(lift-file-path-ext tests/file_path_ext.cpp)
target_link_libraries(lift-file-path-ext PRIVATE emulator-lift-runtime)
add_test(NAME file-path-ext COMMAND lift-file-path-ext)

add_executable(lift-locale-ext tests/locale_ext.cpp)
target_link_libraries(lift-locale-ext PRIVATE emulator-lift-runtime)
add_test(NAME locale-ext COMMAND lift-locale-ext)

add_executable(lift-process-ext tests/process_ext.cpp)
target_link_libraries(lift-process-ext PRIVATE emulator-lift-runtime)
add_test(NAME process-ext COMMAND lift-process-ext)

add_executable(lift-timer-ext tests/timer_ext.cpp)
target_link_libraries(lift-timer-ext PRIVATE emulator-lift-runtime)
add_test(NAME timer-ext COMMAND lift-timer-ext)

# ---- pure：S6 前三族的**核心行为**（内存 / 错误码·TLS / 同步）----
# ★ 补的是"最常走的那条路"没有可再校验观察的缺口（审计单 REQ-01M6JFZ73GTZE9F6967RRSH6CE）；
#   判据见 tests/shims_core.cpp 的文件头（每条至少两个断言，含宿主可见状态）。
add_executable(lift-shims-core tests/shims_core.cpp)
target_link_libraries(lift-shims-core PRIVATE emulator-lift-runtime)
add_test(NAME shims-core COMMAND lift-shims-core)

# ---- pure：吞吐基准（S8；只测解释器分发，不设性能门槛 ⇒ 数字进 tools/lift.mjs report）----
add_executable(lift-bench tests/bench.cpp)
target_link_libraries(lift-bench PRIVATE emulator-lift-runtime)
add_test(NAME bench COMMAND lift-bench)

# ---- assets：要 corpus 里的二进制 / `.lst`（不在仓库里，见 .gitignore）----
set(AGE_SECTFIX_EXE "${CMAKE_CURRENT_SOURCE_DIR}/../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE")
set(AGE_LST "${CMAKE_CURRENT_SOURCE_DIR}/../../corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst")

if(EXISTS "${AGE_SECTFIX_EXE}" OR DEFINED ENV{AMAYUI_AGE_EXE})
    add_executable(lift-loader tests/loader.cpp)
    target_link_libraries(lift-loader PRIVATE emulator-lift-runtime)
    add_test(NAME loader COMMAND lift-loader)
    set_tests_properties(loader PROPERTIES
        ENVIRONMENT "LIFT_AGE_EXE=${AGE_SECTFIX_EXE};LIFT_AGE_LST=${AGE_LST}" SKIP_RETURN_CODE 77)

    # pe-resource 的 pure 部分总是跑；有资产时补 assets 部分（否则整个测试 SKIP 77）。
    set_tests_properties(pe-resource PROPERTIES
        ENVIRONMENT "LIFT_AGE_EXE=${AGE_SECTFIX_EXE}" SKIP_RETURN_CODE 77)

    add_executable(lift-boot-trace tests/boot_trace.cpp)
    target_link_libraries(lift-boot-trace PRIVATE emulator-lift-runtime)
    add_test(NAME boot-trace COMMAND lift-boot-trace)
    set_tests_properties(boot-trace PROPERTIES
        ENVIRONMENT "LIFT_AGE_EXE=${AGE_SECTFIX_EXE};LIFT_AGE_LST=${AGE_LST}" SKIP_RETURN_CODE 77)
endif()

if(EXISTS "${AGE_LST}")
    add_executable(lift-decode-crosscheck tests/decode_crosscheck.cpp)
    target_link_libraries(lift-decode-crosscheck PRIVATE emulator-lift-runtime)
    add_test(NAME decode-crosscheck COMMAND lift-decode-crosscheck)
    set_tests_properties(decode-crosscheck PROPERTIES
        ENVIRONMENT "LIFT_AGE_EXE=${AGE_SECTFIX_EXE};LIFT_AGE_LST=${AGE_LST}" SKIP_RETURN_CODE 77)

    add_executable(lift-corpus-mnemonics tests/corpus_mnemonics.cpp)
    target_link_libraries(lift-corpus-mnemonics PRIVATE emulator-lift-runtime)
    add_test(NAME corpus-mnemonics COMMAND lift-corpus-mnemonics)
    set_tests_properties(corpus-mnemonics PROPERTIES
        ENVIRONMENT "LIFT_AGE_EXE=${AGE_SECTFIX_EXE};LIFT_AGE_LST=${AGE_LST}" SKIP_RETURN_CODE 77)

    add_executable(lift-base-coverage tests/base_coverage.cpp)
    target_link_libraries(lift-base-coverage PRIVATE emulator-lift-runtime)
    add_test(NAME base-coverage COMMAND lift-base-coverage)
    set_tests_properties(base-coverage PROPERTIES
        ENVIRONMENT "LIFT_AGE_EXE=${AGE_SECTFIX_EXE};LIFT_AGE_LST=${AGE_LST}" SKIP_RETURN_CODE 77)
endif()

# =====================================================================================
# P2 · GDI 字体 / 文本（宿主 Amayui CN + 三档 GGO + 自洽）
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
# =====================================================================================
find_package(Freetype REQUIRED)
target_sources(emulator-lift-runtime PRIVATE
    src/runtime/font.cpp
    src/shims/gdi32_object.cpp
    src/shims/gdi32_font.cpp)
target_link_libraries(emulator-lift-runtime PRIVATE Freetype::Freetype)
# 宿主字体路径由 CMake 注入（照 emulator-cxx 的 EMULATOR_CXX_FONT_PATH）：
#   ⛔ 源码里不硬写相对路径；CLI 的 `--font` 是调试覆盖。
set(LIFT_HOST_FONT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../corpus/assets/fonts/Amayui-CN_cnjp.ttf")
target_compile_definitions(emulator-lift-runtime PRIVATE
    LIFT_HOST_FONT_PATH="${LIFT_HOST_FONT_PATH}")

# =====================================================================================
# P4 · GDI region（矩形表 / 椭圆 / 多边形 + CombineRgn + RGNDATA）+ 窗口无效区
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
# =====================================================================================
target_sources(emulator-lift-runtime PRIVATE
    src/runtime/region.cpp
    src/shims/gdi32_region.cpp)

# ---- pure：对象表 / DC 状态 / SelectObject 旧值恢复 / GGO 格式算术 / 自洽（合成字形源）----
add_executable(lift-gdi tests/gdi.cpp)
target_link_libraries(lift-gdi PRIVATE emulator-lift-runtime)
add_test(NAME gdi COMMAND lift-gdi)

# ---- assets：真 Amayui CN（缺字体文件时整个测试不注册 ⇒ ⛔ 不假绿）----
if(EXISTS "${LIFT_HOST_FONT_PATH}")
    add_executable(lift-gdi-font tests/gdi_font.cpp)
    target_link_libraries(lift-gdi-font PRIVATE emulator-lift-runtime)
    add_test(NAME gdi-font COMMAND lift-gdi-font)
    set_tests_properties(gdi-font PROPERTIES SKIP_RETURN_CODE 77)
endif()

# =====================================================================================
# M2 · D3D9 最小设备簇（用户 2026-11-05 裁决"确认需要实现 D3D 接口"）
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
#   `d3d9!Direct3DCreate9` 从 stub 变真实现（register.cpp 里的 addUnimplemented 已删）；
#   IDirect3D9 / IDirect3DDevice9 只填**取证到的那几个槽**，其余 = comUnsupported 的 loud trap。
# =====================================================================================
target_sources(emulator-lift-runtime PRIVATE
    src/runtime/d3d9.cpp
    src/shims/d3d9.cpp)

# ---- pure：D3D9 设备簇（自造 guest 代码经 vtable 调；不碰游戏 / LFS）----
add_executable(lift-d3d9 tests/d3d9.cpp)
target_link_libraries(lift-d3d9 PRIVATE emulator-lift-runtime)
add_test(NAME d3d9 COMMAND lift-d3d9)

# =====================================================================================
# M2 · D3DX9 Sprite 簇（D3DXCreateSprite → 第一帧；用户 2026-11-05 裁决"确认需要实现 D3D 接口"）
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
#   `d3dx9_43!D3DXCreateSprite` 从 stub 变真实现（register_import_gaps.cpp 里那一行随
#   `pnpm tools lift sync --write` 消失）；ID3DXSprite 的 14 槽里 +0x18/+0x1C = loud trap。
# =====================================================================================
target_sources(emulator-lift-runtime PRIVATE
    src/runtime/d3dx9_sprite.cpp
    src/shims/d3dx9.cpp)

# ---- pure：D3DX9 Sprite（像素级：调制色 / ALPHABLEND / 裁剪 / 变换 / End 幂等）----
add_executable(lift-d3dx9 tests/d3dx9.cpp)
target_link_libraries(lift-d3dx9 PRIVATE emulator-lift-runtime)
add_test(NAME d3dx9 COMMAND lift-d3dx9)

# =====================================================================================
# M2 · D3D9 fixed-function 流水线（设备状态机 + 软件光栅 → 第一帧）
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
#   `runtime/d3d9_pipeline.{h,cpp}` = 变换 / 渲染状态 / 顶点缓冲 / 光栅 / 深度 / 纹理采样；
#   `shims/d3d9.cpp` 里补的槽见那边的注释（每条都带 `.lst` 站点 EA）。
# =====================================================================================
target_sources(emulator-lift-runtime PRIVATE
    src/runtime/d3d9_pipeline.cpp)

# ---- pure：fixed-function 流水线（像素精确：纯色三角形 / 纹理四边形 / Z / 混合 / 状态存取）----
add_executable(lift-d3d9-pipeline tests/d3d9_pipeline.cpp)
target_link_libraries(lift-d3d9-pipeline PRIVATE emulator-lift-runtime)
add_test(NAME d3d9-pipeline COMMAND lift-d3d9-pipeline)

# =====================================================================================
# M2 · 公共数学层（D3DX 行主序/行向量 ↔ GLM 列主序/列向量 的唯一适配点）
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
#   `runtime/math.h` 把 `<glm/glm.hpp>` 带进公开头，所以 include dir 必须 PUBLIC。
# =====================================================================================
find_package(glm CONFIG REQUIRED)
target_sources(emulator-lift-runtime PRIVATE
    src/runtime/math.cpp
    src/runtime/pixel_format.cpp)
target_link_libraries(emulator-lift-runtime PUBLIC glm::glm)

# ---- pure：数学适配（行/列主序乘法顺序、LH 投影/视图、零向量归一化）----
add_executable(lift-math tests/math.cpp)
target_link_libraries(lift-math PRIVATE emulator-lift-runtime)
add_test(NAME math COMMAND lift-math)

# ---- pure：像素格式（掩码往返、16/32bpp load/store、颜色打包、min_filter 决策）----
add_executable(lift-pixel-format tests/pixel_format.cpp)
target_link_libraries(lift-pixel-format PRIVATE emulator-lift-runtime)
add_test(NAME pixel-format COMMAND lift-pixel-format)
