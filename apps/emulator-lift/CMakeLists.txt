cmake_minimum_required(VERSION 3.24)

project(emulator-lift
    VERSION 0.1.0
    DESCRIPTION "Run AGE.EXE's real machine code on a guest-state x86 interpreter (SDL3 host)"
    LANGUAGES C CXX)

# ★ **目录级** `/utf-8`：源码里有中日文注释 / 字面量；MSVC 默认按本地码页（中文 Windows = 936）
#   读源文件并生成窄字符串字面量 ⇒ 输出双重乱码。放在 `project()` 之后、任何 target / 子目录之前，
#   这样以后新增的 target（以及 vendored C）都不会漏；只加 `/utf-8`，不动 `/W4 /WX`。
if(MSVC)
    add_compile_options(/utf-8)
endif()

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

# SDL_GetGPURendererDevice / SDL_CreateRenderer("gpu") 需要 SDL >= 3.4；
# vcpkg baseline 当前 pin 3.4.18；这里显式写版本下限，换 baseline 时不会静默退回 3.2/3.3。
find_package(SDL3 3.4 CONFIG REQUIRED)
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
    src/runtime/audio.cpp
    src/runtime/host_window.cpp
    src/runtime/pe_resource.cpp
    src/runtime/surface.cpp
    src/runtime/present.cpp
    src/runtime/present_sdl.cpp
    src/runtime/frame_blit.cpp
    src/runtime/d3d9_gpu.cpp
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
    src/runtime/host_encoding.cpp
    src/runtime/native_overrides.cpp
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
    # ★ M4 起步：DirectShow / AMultiMediaStream 形状层（CoCreateInstance 不再 loud trap）
    src/shims/quartz_video.cpp
    src/shims/kernel32_bootstrap.cpp)
target_include_directories(emulator-lift-runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(emulator-lift-runtime
    PUBLIC SDL3::SDL3
    PRIVATE Zydis::Zydis berkeley-softfloat)
if(WIN32)
    # host_encoding.cpp 的 CommandLineToArgvW（argv 的 UTF-8 重建）
    target_link_libraries(emulator-lift-runtime PRIVATE shell32)
    # d3d9_gpu.cpp 用 D3DCompile 在运行时把 HLSL 编成 DXBC
    target_link_libraries(emulator-lift-runtime PRIVATE d3dcompiler)
endif()
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
    target_compile_options(emulator-lift-runtime PRIVATE /W4 /WX /permissive-)
    target_compile_definitions(emulator-lift-runtime PRIVATE _CRT_SECURE_NO_WARNINGS)
    # ★ `/utf-8` 已在 `project()` 之后用目录级 `add_compile_options` 覆盖所有 target
    #   （见文件顶部）；这里不再重复，也不动 `/W4 /WX`（那是另一条口径）。
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

# ---- pure：`--gpu-strict` 的机械判据自检（不需要游戏）----
# `--gpu-strict` 与显式 CPU 参考后端矛盾 ⇒ 必须在载入执行源之前就拒绝（非 0）。
# 真正的 fallback>0 ⇒ exit 3 需要真实 boot，见 README §4.2b / 报告里的真机验收。
add_test(NAME gpu-strict-requires-gpu COMMAND emulator-lift --raster cpu --gpu-strict)
set_tests_properties(gpu-strict-requires-gpu PROPERTIES WILL_FAIL TRUE)
# strict 要求 sprite 也走 GPU：显式 `--raster-sprites 0` 与 strict 自相矛盾 ⇒ 必须拒绝。
add_test(NAME gpu-strict-rejects-no-sprites
    COMMAND emulator-lift --present window --raster gpu --raster-sprites 0 --gpu-strict)
set_tests_properties(gpu-strict-rejects-no-sprites PROPERTIES WILL_FAIL TRUE)

# ---- pure：guest 函数级 native override（差分 / 回退 / ABI）----
add_executable(lift-native-override tests/native_override.cpp)
target_link_libraries(lift-native-override PRIVATE emulator-lift-runtime)
add_test(NAME native-override COMMAND lift-native-override)

# ---- Windows 宿主编码契约（argv = UTF-8 / 输出字节 = UTF-8）----
# ★ 子进程用**文件**重定向（不走管道：受限沙箱里捕获管道会 EPERM）。
# ★ 起子进程时 cwd = 源码目录 ⇒ 默认的 `../../corpus/...` 执行源 / `.lst` 能按相对路径找到。
# ★ 测试要真的启动 emulator-lift（它会载入执行源）⇒ 属于 **assets** 档：缺 LFS 资产时
#   **不注册**（与 loader / boot-trace 同一判据；⛔ 不能让它挂成"pure 却红"）。
add_executable(lift-host-encoding tests/host_encoding.cpp)
target_link_libraries(lift-host-encoding PRIVATE emulator-lift-runtime)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE")
    add_test(NAME host-encoding COMMAND lift-host-encoding $<TARGET_FILE:emulator-lift>)
    set_tests_properties(host-encoding PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        SKIP_RETURN_CODE 77)
endif()

# ---- assets：真机安装上的 LOGO 影片必须"真的放完"（首帧 / completed / 堆平稳 / tex#1 差分）----
# 判据见 tests/movie_logo.cpp 的头注释。缺游戏安装 ⇒ 不注册（assets 档的判据 = 注册位置）；
# GPU 不可用（raster 行不是 sdl-gpu）⇒ 测试自己 `LIFT_SKIP`(77)。
# ★ **opt-in**：这条要真启模拟器、真放 LOGO 影片（子进程按**墙钟秒**计），是**全部用例里最贵**的
#   一条 ⇒ 默认门禁**不注册**它，必须显式打开（判据 = 条件里的 `DEFINED ENV{LIFT_ASSETS}`）：
#     $env:LIFT_ASSETS=1; ctest --test-dir out -R movie-logo --output-on-failure
#   ⛔ `DEFINED ENV{…}` 是**配置期**求值 ⇒ 改了环境变量必须重新 configure
#   （`cmake --preset windows-x64`），`ctest -N` 才会跟着变。
# ★ `add_executable` 也在同一个 if 里：纯构建环境（没有游戏安装 / 没开 LIFT_ASSETS）连这个 target 都不编。
# ★ 不改测试名与文件路径（需求单 `01M6T07SC0XN81G1MZJYJXXGV9` 的 verify 指向它们）。
if(EXISTS "E:/Games/Eushully/天結/install/SYS4INI.BIN" AND DEFINED ENV{LIFT_ASSETS})
    add_executable(lift-movie-logo tests/movie_logo.cpp)
    target_link_libraries(lift-movie-logo PRIVATE emulator-lift-runtime)
    # ★ 真机影片只在 RelWithDebInfo 树里跑（用户口径：Debug 解释器慢到无法使用；
    #   2026-11-07 起 preset 只有 RelWithDebInfo）。万一有人手工配 Debug，测试自己 `LIFT_SKIP`(77)。
    target_compile_definitions(lift-movie-logo PRIVATE
        LIFT_MOVIE_LOGO_IS_DEBUG=$<IF:$<CONFIG:Debug>,1,0>)
    # ★ 第 5 个参数 `--expect-early-stop`：把"钩子必须生效"钉在 **CMake 侧**（与上面的 ENVIRONMENT
    #   成对）—— 若只让测试拿 env 变量自证，将来删掉 ENVIRONMENT 就会静默退回 45 s 仍然绿。
    add_test(NAME movie-logo COMMAND lift-movie-logo $<TARGET_FILE:emulator-lift>
        "${CMAKE_CURRENT_SOURCE_DIR}/../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE"
        "E:/Games/Eushully/天結/install"
        "${CMAKE_CURRENT_BINARY_DIR}/movie-logo-work"
        --expect-early-stop)
    set_tests_properties(movie-logo PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        SKIP_RETURN_CODE 77
        # ★ **事件驱动提前停**（缺省关；这里显式打开）：影片放完（图停机链看到 `completed_`）就结束
        #   本次运行，不再空转到 `--time-limit`。`--time-limit` 仍保留作**兜底**（⛔ 不要改小）。
        # ★ ENVIRONMENT 是**分号分隔的列表**：这里只有一项；要加别的写成 "A=1;B=2"，
        #   ⛔ 不要用第二个 `set_tests_properties` 把它覆盖掉（那会盖掉整条属性）。
        ENVIRONMENT "LIFT_STOP_AFTER_MOVIE=1"
        # 子进程自带 `--time-limit` 兜底（boot + 影片）；墙钟上限留余量。
        TIMEOUT 180)
endif()

# =====================================================================================
# 测试分级（AGENTS.md §5「CTest 那一档不写 pragma，判据是注册位置」）：#   - **顶层无条件注册** ⇒ 默认 ctest 就跑（不需要游戏 / LFS）⇒ pure
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

# ---- pure：GetMessageA + 宿主事件源（P3；假事件源，不建 SDL 窗口）----
add_executable(lift-window-events tests/window_events.cpp)
target_link_libraries(lift-window-events PRIVATE emulator-lift-runtime)
add_test(NAME window-events COMMAND lift-window-events)

# ---- pure：`--skip-logo` 的字段写入（T_Command+0x5EB5C；锚 0x004BAA37）----
add_executable(lift-skip-logo tests/skip_logo.cpp)
target_link_libraries(lift-skip-logo PRIVATE emulator-lift-runtime)
add_test(NAME skip-logo COMMAND lift-skip-logo)

add_executable(lift-surface-present tests/surface_present.cpp)
target_link_libraries(lift-surface-present PRIVATE emulator-lift-runtime)
add_test(NAME surface-present COMMAND lift-surface-present)

# ---- pure：PresentSink 契约（headless 默认不变 / sink 失败不污染 guest）----
add_executable(lift-present-sink tests/present_sink.cpp)
target_link_libraries(lift-present-sink PRIVATE emulator-lift-runtime)
add_test(NAME present-sink COMMAND lift-present-sink)

# ---- pure：guest 后缓冲格式选择（六种 masks，不按 bpp 猜）----
add_executable(lift-frame-blit tests/frame_blit.cpp)
target_link_libraries(lift-frame-blit PRIVATE emulator-lift-runtime)
add_test(NAME frame-blit COMMAND lift-frame-blit)

# ---- SDL 窗口 / 呈现（需要窗口系统；无桌面 ⇒ SKIP 77）----
# dummy + software：测窗口/纹理/上传/渲染/回读的机械正确性（明确打印“不是 GPU”）。
# --gpu：默认 GPU 驱动列表；必须 SDL_GetGPURendererDevice()!=NULL 才算通过（不许 software 假绿）。
add_executable(lift-present-window tests/present_window.cpp)
target_link_libraries(lift-present-window PRIVATE emulator-lift-runtime)
add_test(NAME present-window COMMAND lift-present-window)
set_tests_properties(present-window PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy" SKIP_RETURN_CODE 77)
add_test(NAME present-window-gpu COMMAND lift-present-window --gpu)
set_tests_properties(present-window-gpu PROPERTIES SKIP_RETURN_CODE 77)

# ---- GPU 光栅化（--raster gpu）：独立 SDL_GPUDevice + 离屏 target + 读回 ----
# 两档都要求真的 GPU 设备：dummy 视频下建不出设备 ⇒ 返回 77（SKIP），⛔ 不是"跑绿"。
add_executable(lift-gpu-raster tests/gpu_raster.cpp)
target_link_libraries(lift-gpu-raster PRIVATE emulator-lift-runtime)
add_test(NAME gpu-raster COMMAND lift-gpu-raster)
set_tests_properties(gpu-raster PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy" SKIP_RETURN_CODE 77)
add_test(NAME gpu-raster-gpu COMMAND lift-gpu-raster --gpu)
set_tests_properties(gpu-raster-gpu PROPERTIES SKIP_RETURN_CODE 77)

# ---- GPU 直出（`--present window --raster gpu`）：窗口 claim + target→swapchain ----
# 判据是**窗口像素 == 送进去的后缓冲**（含 16bpp 转换 / 尺寸变化 / 连续两帧）。
# 窗口 claim 不成立（dummy 视频 / 无桌面）⇒ SKIP 77：⛔ 不拿 renderer 路径冒充直出。
# ★ 顶层无条件注册（不需要游戏 / LFS）⇒ pure 档；缺窗口系统时按 SKIP 处理。
add_executable(lift-gpu-direct-present tests/gpu_direct_present.cpp)
target_link_libraries(lift-gpu-direct-present PRIVATE emulator-lift-runtime)
add_test(NAME gpu-direct-present COMMAND lift-gpu-direct-present)
set_tests_properties(gpu-direct-present PROPERTIES SKIP_RETURN_CODE 77)

# ---- pure：GPU 准入 / 状态映射（TITLE 的 blend 5/6 + separate 2/6、stage0 MODULATE(TEX,TFACTOR)）----
# 不建机器、不碰 SDL：只是 d3d9::gpuBlendStateFor / gpuTextureModeFor 的期望值。
add_executable(lift-gpu-mapping tests/gpu_mapping.cpp)
target_link_libraries(lift-gpu-mapping PRIVATE emulator-lift-runtime)
add_test(NAME gpu-mapping COMMAND lift-gpu-mapping)

# ---- GPU 合批（入队 + 一次 flush）：pure 档（Mock 后端，不碰 SDL / GPU）----
# 判据：N 次 Draw 只 flush 1 次；Clear(TARGET) / Present / 换 target 各触发 flush + syncToGuest 1 次，
# 且 syncFromGuest 在队列非空时被拒绝（⛔ 静默覆盖）。去掉 flush ⇒ 队列不清空 ⇒ 当场红。
add_executable(lift-gpu-batch tests/gpu_batch.cpp)
target_link_libraries(lift-gpu-batch PRIVATE emulator-lift-runtime)
add_test(NAME gpu-batch-order COMMAND lift-gpu-batch)

# ---- 同一份夹具的真后端判据：Clear / 纹理 LockRect / SetRenderTarget 之后像素对、队列空 ----
# dummy 视频下建不出 SDL_GPU 设备 ⇒ SKIP 77（⛔ 不拿 pure 档冒充真后端绿）。
add_test(NAME gpu-batch-barrier COMMAND lift-gpu-batch --gpu)
set_tests_properties(gpu-batch-barrier PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy" SKIP_RETURN_CODE 77)

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

# ---- pure：方案 D 的音频设备模型（逐缓冲格式 / 通知次序 / dump / 喂料身份；不碰 SDL 设备）----
add_executable(lift-audio tests/audio.cpp)
target_link_libraries(lift-audio PRIVATE emulator-lift-runtime)
add_test(NAME audio-device COMMAND lift-audio)

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
add_executable(lift-log-policy tests/log_policy.cpp)
target_link_libraries(lift-log-policy PRIVATE emulator-lift-runtime)
add_test(NAME log-policy COMMAND lift-log-policy)

add_executable(lift-quartz-video tests/quartz_video.cpp)
target_link_libraries(lift-quartz-video PRIVATE emulator-lift-runtime)
add_test(NAME quartz-video COMMAND lift-quartz-video)

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
    # ---- assets：**真 guest 字节**的 native override 差分（解释器 vs override；缺资产 ⇒ SKIP 77）----
    # 判据见 tests/native_override_assets.cpp 文件头：同一份夹具，真字节跑解释器 vs native 实现，
    # 比较内存 / EAX / ESP / 标号寄存器 / EIP（48AE90 还比整张纹理逐字节 + 锁记账）。
    add_executable(lift-native-override-assets tests/native_override_assets.cpp)
    target_link_libraries(lift-native-override-assets PRIVATE emulator-lift-runtime)
    add_test(NAME native-override-assets COMMAND lift-native-override-assets)
    set_tests_properties(native-override-assets PROPERTIES
        ENVIRONMENT "LIFT_AGE_EXE=${AGE_SECTFIX_EXE}" SKIP_RETURN_CODE 77)

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

# =====================================================================================
# `--profile` 的宿主 shim 墙钟表（pure：裸 Machine + 一个已知 shim；不碰游戏 / LFS / SDL）
# ★ 判据见 tests/profile_shims.cpp 文件头：`RunOptions{profile=true}` ⇒ `=== profile: shims`
#   表里出现 `test!slow` 且 `calls=1`；删掉 `machine.cpp` 的 `|| opts.profile` ⇒ 该行消失 ⇒ 红。
# ★ 顶层无条件注册 ⇒ pure 档。
# =====================================================================================
add_executable(lift-profile-shims tests/profile_shims.cpp)
target_link_libraries(lift-profile-shims PRIVATE emulator-lift-runtime)
add_test(NAME profile-shims COMMAND lift-profile-shims)

# =====================================================================================
# 宿主侧 MPEG-1 影片解码（vendor pl_mpeg；无 COM / 无 guest / 无 SDL / 无线程）
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
# ★ pl_mpeg 是单文件库：`third_party/pl_mpeg/pl_mpeg_impl.cpp` 是**唯一**定义
#   PL_MPEG_IMPLEMENTATION 的 TU；其余代码只看见 `src/runtime/video/movie_decoder.h`
#   （公共头不含 pl_mpeg.h，见那边注释）。
# ★ 三处 pl_mpeg.h 必须逐字节相同（本处 + emulator-cxx + emulator-new），
#   守卫：`tools/test/emulator-cxx-pl-mpeg-sync.test.mjs`。
# =====================================================================================
add_library(lift-pl-mpeg STATIC third_party/pl_mpeg/pl_mpeg_impl.cpp)
target_include_directories(lift-pl-mpeg PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/third_party/pl_mpeg)
# 解码是第三方代码、且是热点：Debug 构建也对它开优化（与 emulator-cxx 的 movie.cpp 同一口径）。
if(MSVC)
    target_compile_options(lift-pl-mpeg PRIVATE /O2)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(lift-pl-mpeg PRIVATE -O2)
endif()

target_sources(emulator-lift-runtime PRIVATE
    src/runtime/video/movie_decoder.cpp)
target_link_libraries(emulator-lift-runtime PRIVATE lift-pl-mpeg)

# ---- pure：影片解码器（自造字节 + fake 帧源；不碰游戏 / LFS）----
# 判据见 tests/movie_decoder.cpp 文件头：坏输入不崩、时间推进单调/不重复、ended 只报一次、
# Frame 指针在下一次 decode 前稳定、seek/rewind 后重新起解。
add_executable(lift-movie-decoder tests/movie_decoder.cpp)
target_link_libraries(lift-movie-decoder PRIVATE emulator-lift-runtime)
add_test(NAME movie-decoder COMMAND lift-movie-decoder)

# ---- pure：影片音频的同步机制（前导 / 迟到丢弃 / underrun / 音量 / 重采样）----
add_executable(lift-movie-audio tests/movie_audio.cpp)
target_link_libraries(lift-movie-audio PRIVATE emulator-lift-runtime)
add_test(NAME movie-audio COMMAND lift-movie-audio)

# =====================================================================================
# M4 · 纹理影片 = 真 DirectShow 服务端的**协议核心**（契约：docs/movie-dshow-protocol.md）
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
# ★ 本段只落**新模块 + pure 测试**：⛔ 还没接 `src/shims/quartz_video.cpp`
#   （下一段才把 IGraphBuilder / IMediaControl / IMediaPosition 的槽接过来）。
#   `runtime/dshow/` = 图状态机（filter 表 / 接线 / 控制 / 位置 / 泵）+ 宿主侧
#   IPin / IMemAllocator / IMediaSample 的 guest 可见 COM 对象 + AM_MEDIA_TYPE 构造。
# =====================================================================================
target_sources(emulator-lift-runtime PRIVATE
    src/runtime/dshow/audio_render.cpp
    src/runtime/dshow/graph.cpp
    src/runtime/dshow/media_type.cpp
    src/runtime/dshow/objects.cpp
    src/runtime/dshow/guest_invoke.cpp
    src/runtime/dshow/decoder_feed.cpp)

# ---- pure：协议核心（假引擎 filter + 假帧源 + 假时钟；不碰游戏 / LFS）----
# 判据见 tests/dshow_graph.cpp 文件头：D1 接线（含 QI/NotifyAllocator）、媒体类型逐字段、
# 控制顺序（先 Pause 后 Run；渲染器保持 Paused）、泵的 33/66 ms 与自底向上、分配器忙不阻塞、
# 以及「会红」自检（坏接线 + 逐字段改坏都必红）。
add_executable(lift-dshow-graph tests/dshow_graph.cpp)
target_link_libraries(lift-dshow-graph PRIVATE emulator-lift-runtime)
add_test(NAME dshow-graph COMMAND lift-dshow-graph)
