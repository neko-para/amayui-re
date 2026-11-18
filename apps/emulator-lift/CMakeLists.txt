cmake_minimum_required(VERSION 3.24)

project(emulator-lift
    VERSION 0.1.0
    DESCRIPTION "Run AGE.EXE's real machine code on a guest-state x86 interpreter (SDL3 host)"
    LANGUAGES C CXX)

# ---- 测试开关：`OFF` ⇒ 只构建产品二进制（不编测试 target / 不注册 CTest）----
# ★ 默认 ON：现有门禁（`ctest --test-dir out`）不变。
option(LIFT_BUILD_TESTS "Build emulator-lift tests / CTest suite" ON)

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
        "no vcpkg toolchain detected. Use CMakePresets.json (it reuses ../emulator-cxx/vcpkg across directories),"
        "or pass -DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake.")
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
    # ★ AOT 臂的 guest fault 现场（`runtime/fault_state.h` 的唯一实现）：`guestFault` 的钩子在这里，
    #   打印 eip / 当前块 EA / 8 GPR + eflags / 32 字 guest 栈。归属本 target 而不是 AOT target：
    #   纯 CTest `fault-report` 只链本 target（合成 fault 直接驱动打印函数），AOT 臂也链它
    #   ⇒ **一份实现两处用**（⛔ 只放进 AOT target 会让测试链接失败）。
    src/runtime/fault_state.cpp
    # ★ 窗口 profile 的**闭式实现**（`aot/window_prof.h` 的唯一 out-of-line 部分）。
    #   归属本 target 而不是 AOT target：纯 CTest `lift-aot-window-profile` 只链本 target，
    #   AOT 臂也链它 ⇒ **一份实现两处用**。⛔ 只放进 AOT target 会让测试链接失败。
    #   ★ 为什么不做成纯头：头被 `aot_host.h` 包含 ⇒ 头里施加变异 = 整条 AOT 臂重编
    #     （一条变异 12 分钟）；搬成 TU 后单条变异只重编 1 个 TU（见 `tools/mutate-check.mjs` 那条）。
    src/aot/window_prof.cpp
    # ★ 按帧 shim 分桶（`--profile-slow-frames`）的**纯逻辑实现**（`aot/frame_bucket.h` 的
    #   out-of-line 部分：名字合并 + 有界 top-N + 有界慢帧表）。归属本 target 的理由与
    #   `window_prof.cpp` 相同：纯 CTest `lift-aot-frame-bucket` 只链本 target，AOT 臂也链它
    #   ⇒ **一份实现两处用**（⛔ 只放进 AOT target 会让测试链接失败）。
    src/aot/frame_bucket.cpp
    src/runtime/sha256.cpp
    src/runtime/image.cpp
    src/runtime/mnemonics.cpp
    src/runtime/decode.cpp
    src/runtime/x86.cpp
    src/runtime/guest_call.cpp
    src/runtime/audio.cpp
    src/runtime/host_window.cpp
    # ★ 输入：`input.cpp` = SDL 真实事件与（机器内）注入的**唯一**汇合点；
    #   `input_script.cpp` = 指令式时间线（`at frame N` / `at guest_ms T` / click / key ...）。
    #   ⚠ 2026-11-16 起时间线的 **CLI 入口已退役**（`--input-script` 整族）；它只剩机器内 actuator
    #     与 pure CTest 夹具两个消费者（见 docs/03-emulator/input-plan.md §7）。
    src/runtime/input.cpp
    src/runtime/input_script.cpp
    #   ★ 输入**投递层**（S1：骨架 + 计数，没有配方）：`input_plan.h` 的三条铁律是形状真源。
    #     ⛔ 它不在执行循环里被调用（守卫 `tools/test/input-plan-shape.test.mjs` ①）。
    src/runtime/input_plan.cpp
    #   `engine_marker.cpp` = 脚本身份（marker）采样器：`TriggerKind::Marker` 的**唯一**取数口
    #   （VM 循环头 `0x4123F0` / 消息泵回来的块入口 `0x4123E5`；常量与 EA 锚在头文件）。
    src/runtime/engine_marker.cpp
    src/runtime/pe_resource.cpp
    src/runtime/surface.cpp
    # ★ 画面特征采样（交互模型的 `screen` 源）：采样点 = Present。
    src/runtime/screen_sample.cpp
    # ★ 输入**配方**（一套语法、一份解析器）：`recipe.cpp` = 解析器 + 顺序模型 + 门判定（**唯一一份**）；
    #   `interaction_graph.cpp` = 机器内驱动器（块边界推进、动作经 InputScript 追加；
    #     ⚠ 2026-11-16 起**没有 CLI 入口** —— `--interaction-graph` 已退役，只剩 pure 夹具用它）；
    #   `input_plan.cpp` = **唯一的用户入口**（AOT 臂 `--input-plan`；guest 读点推进、动作直接写 WindowManager）。
    src/runtime/recipe.cpp
    src/runtime/interaction_graph.cpp
    src/runtime/present.cpp
    src/runtime/present_sdl.cpp
    # ★ 画面 ↔ 窗口客户区的 letterbox 映射的**唯一一份**（鼠标事件入 + 宿主光标 warp 出共用；
    #   `--cursor-warp` 的几何判据在 `tests/cursor_warp.cpp` 的 `cursor-map/*`）。
    src/runtime/cursor_map.cpp
    # ★ `--scale <auto|数值>` 的**唯一一份**口径（窗口客户区尺寸 / auto 的 DPI→scale / 启动
    #   readout 文案）。`aot_main.cpp`（解析 + 打印）与 `present_sdl.cpp`（建窗口）都只调它；
    #   pure CTest = `tests/window_scale.cpp`（用例 `window-scale/*`）。
    src/runtime/window_scale.cpp
    # ★ 宿主光标原语（`--cursor-warp` 的唯一 OS 出口；单 TU 按 `#ifdef` 选平台实现）。
    src/runtime/host_cursor.cpp
    # ★ M0 / B：UI display list 的最终模型子集（`ui/element.h` 的 POD + `UiBuilder`）、宿主字体文本
    #   栅格化（`ui/raster.cpp`；唯一外部依赖 FontEngine / FreeType，支持中文）、统一合成入口
    #   （`ui/present_ui.cpp` → `PresentSink::presentOverlay`）。⛔ 不另开 HUD 专用渲染路径（D12）。
    #   ★ HUD 的**内容**不再在 C++ 里：它由外置脚本 `scripts/ui/hud.js` 产出（`--overlay` / `--ui`）。
    src/ui/raster.cpp
    # ★ B：唯一的"脚本 → 窗口像素"入口（几何 → 画布原点 → `UiScript::render` → 栅格化 → sink）。
    src/ui/present_ui.cpp
    # ★ 宿主自绘模态框（MessageBox；`REQ-01M75S724...` / `REQ-01M75T14...`）：
    #   纯逻辑（uType→spec / 布局 / 命中 / 状态机）+ 唯一一处模态循环 + CPU 合成一整幅 RGBA8。
    src/runtime/message_box.cpp
    src/runtime/frame_blit.cpp
    src/runtime/d3d9_gpu.cpp
    src/runtime/ddraw_surface.cpp
    src/runtime/com.cpp
    src/runtime/machine.cpp
    # ★ 单令牌（guest 执行互斥）：主线程按块取、音频 feeder 每块取；违规计数见 guest_token.h
    src/runtime/guest_token.cpp
    # ★ Phase-0 四行读数（tokenhold / holdhist / waithist / shimmax）的**唯一**实现：
    #   解释器 `Machine::dumpRunSummary` 与 AOT `AotHost::printRunSummary` 都调它
    #   （2026-11-18：AOT 臂以前根本没有这四行 —— 而音频 feeder 只在 AOT 上跑）。
    src/runtime/token_phase0_report.cpp
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
    # ★ native override：机制 / 已知表在 `native_overrides.cpp`；各热点实现按域拆在 `native_*.cpp`
    #   （2026-11-08，纯搬运；契约见 native_overrides.h）
    src/runtime/native_overrides.cpp
    src/runtime/native_crt.cpp
    src/runtime/native_lzss.cpp
    src/runtime/native_image_upload.cpp
    src/runtime/native_texture_fill.cpp
    src/runtime/native_glyph.cpp
    src/runtime/native_map.cpp
    src/runtime/native_movie.cpp
    src/runtime/native_block.cpp
    # ★ 段级 · `sub_48D610` 的 PCM 转换循环（entry 0x48D767 / resume 0x48D89A；
    #   REQ-01M75W6BB3PY3MF42ZTS23KTBS；契约与闸门 G1–G10 见 native_pcm.cpp 的头注）
    src/runtime/native_pcm.cpp
    src/runtime/native_affine.cpp
    # ★ 每帧固定税 `sub_4BBAB0`（Voice::Tick）的 stateless 快路径
    #   （REQ-01M6XVZF3VDC5NSBJ20J2451VJ；契约见 native_overrides.h）
    src/runtime/native_voice.cpp
    # ★ libvorbis `mdct_backward`（`sub_4E0B40`）= 语音起播那一下的 FFT/MDCT 内核
    #   （**数值容忍档**，契约与已知偏差见 native_mdct.h）
    src/runtime/native_mdct.cpp
    # ★ Huffman 子树的两片叶子（`sub_4E1570` = look/首表/二分/adv、`sub_4E1950` = 码本向量）
    #   —— REQ-01M75WED0VFTEYXX43KGHNPPP0 的 S2；契约与逐字依据见 native_huffman.h
    src/runtime/native_huffman.cpp
    # ★ 存档 CRC-32 的两个变体（`sub_436D00` 反射 / `sub_436D50` 非反射）：表从 guest 的
    #   `[this, this+0x800)` 读，⛔ 不硬编码多项式。契约见 native_overrides.h 的 `CrcWhy` 段
    #   （REQ-01M7BY4D1ZWA4HB97T8M8TVEYQ）
    src/runtime/native_crc32.cpp
    src/runtime/shims.cpp
    src/runtime/window_profile.cpp
    # ★ SEH / C++ EH 的两个 OS 原语（`RaiseException` / `RtlUnwind` 的派发状态机；
    #   契约与取证见 runtime/seh.h 的头注）。shim 只搬参数，派发在 Machine 的块边界。
    src/runtime/seh.cpp
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
if(APPLE)
    # host_cursor.cpp：CGWarpMouseCursorPosition / CGAssociateMouseAndMouseCursorPosition /
    # CGEventCreate+CGEventGetLocation（CoreGraphics）都在 ApplicationServices 里。
    # ★ 只有 macOS 需要；Windows 走 user32 的 SetCursorPos（本文件不需要额外库）。
    target_link_libraries(emulator-lift-runtime PRIVATE "-framework ApplicationServices")
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

# =====================================================================================
# Shape A · 构建期静态 lift 的 **T0** 落点：`emulator-lift-aot`（只做平台 bring-up）
# ★ 为什么是**独立 target**、不是给 `emulator-lift` 加一个 flag：Shape A 的产物是
#   "lift 出来的代码 + runtime/shims 链接进**同一个二进制**，解释器只留在测试构建里"（父级裁决）。
#   新 target 的源在 `src/aot/`：⛔ 不 include `machine.h`、⛔ 不实例化 `Machine`、
#   ⛔ 不碰 `BlockCache` / 解码器。T0 只做：映射 PE、建 guest 栈 / 堆 / TEB、注册并 bind shim、
#   设好 cpu 初始寄存器，**执行 0 条 guest 指令**，打印读数后 exit 0。
# ★ linker map 是 T0 那个读数的**证据**（"到底有哪些只属于解释器的 .obj 被拽进来了"）——
#   固定文件名落在构建目录（preset 的 binaryDir = `out/`）⇒ 可复核、不靠猜。
# ★ 告警口径跟既有 target 走：本块**不加**任何 `-W` / `/W` 覆盖（`/W4 /WX` 属于
#   `emulator-lift-runtime`，⛔ 不在这里放宽、也不另立一套）。
# =====================================================================================
add_executable(emulator-lift-aot src/aot/aot_main.cpp src/aot/aot_host.cpp)
target_link_libraries(emulator-lift-aot PRIVATE emulator-lift-runtime)
if(MSVC)
    # clang-cl 也走这一支（MSVC=TRUE）：`/MAP` 交给 lld-link。
    target_link_options(emulator-lift-aot PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/emulator-lift-aot.map")
    # ★ S7（D53）：**宿主栈请求 32 MB**（只预留虚拟地址空间，按需提交）。
    #   为什么：S7 之后每个 guest `call` = 一个宿主 `call` ⇒ 宿主栈深度跟 guest 调用深度同阶。
    #   缺省 1 MB 对深递归的 guest 代码是**新引入的风险**（见 S7 报告）。按 objdump 实测：\n    #   宿主帧 80 B/层（5 个 callee-saved + 0x20 + 返回地址），而 guest 栈 1 MB、每层 guest 帧 ≥4 B\n    #   ⇒ 最坏 1 MB / 4 B × 80 B = 20 MB ⇒ 32 MB 覆盖"guest 自己能装下"的全部深度。与"我们自己
    #   CreateThread 时会给的大小"同一档（D53 §①）。⛔ 这不是语义改动：只影响栈上限。
    target_link_options(emulator-lift-aot PRIVATE "/STACK:33554432")
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    # ★ linker map 的驱动 flag **必须按平台分**：Apple ld64 只认小写 `-map`，
    #   GNU ld / ld.lld（ELF）只认大写 `-Map`，大小写**不通用**（Apple 侧给 `-Map` 会
    #   `ld: unknown options: -Map` 直接链接失败）。
    # ★ 这是**链接器驱动的语法差异**，⛔ 不是告警口径 / 不是语义放宽：两支产出的是同一份
    #   map，路径也同一个（上面 T0 的口径不变）。
    if(APPLE)
        target_link_options(emulator-lift-aot PRIVATE "-Wl,-map,${CMAKE_CURRENT_BINARY_DIR}/emulator-lift-aot.map")
    else()
        target_link_options(emulator-lift-aot PRIVATE "-Wl,-Map,${CMAKE_CURRENT_BINARY_DIR}/emulator-lift-aot.map")
    endif()
    # macOS/Linux：主线程栈由系统决定（通常 8 MB）⇒ 只需把"请求值"写下来，不改（非 Windows 目标
    # 只做兼容性验证，不阻塞本轮）。
endif()

# =====================================================================================
# Shape A · T1a：`emulator-lift-gen`（**构建期 host tool**）
# ★ 只做"入口 EA 集合 + PE 字节 → `.ll`"：⛔ 不做 census、⛔ 不建 `closure.txt`、
#   ⛔ 不把批量 IR 链进 AOT（那是 T1c）。
# ★ 位置：`if(LIFT_BUILD_TESTS)` **之前** —— 那个块会把 `CMAKE_RUNTIME_OUTPUT_DIRECTORY`
#   改成 `out/tests`，产品 / 工具 target 必须落在它之前（与 `emulator-lift` / `emulator-lift-aot`
#   同一口径）。★ 只插入，⛔ 不改上面任何一行。
# ★ 允许链 `emulator-lift-runtime`：解码 / 语义只有一份（"产品二进制里没有解释器"是 T3 的约束）。
# ★ 告警口径与上面两个 target 一致：本块**不加**任何 `-W` / `/W` 覆盖。
# =====================================================================================
add_executable(emulator-lift-gen src/aot/lift_gen.cpp src/aot/lift_gen_emit.cpp
                                src/aot/lift_struct.cpp)
target_link_libraries(emulator-lift-gen PRIVATE emulator-lift-runtime)

# =====================================================================================
# Shape A · T2b：`emulator-lift-aot` 的**执行源**（生成物 + 派发表）
#
# ★ 只插入：本块**不改上面任何一行**，也不改 `if(LIFT_BUILD_TESTS)` 里的任何一行。
# ★ 位置为什么必须在 `if(LIFT_BUILD_TESTS)` **之前**：那个块把 `CMAKE_RUNTIME_OUTPUT_DIRECTORY`
#   改成 `out/tests`（产品二进制会被塞进测试目录）；且既有 `find_program(LIFT_IR_CLANG)` 也在那块里
#   —— 产品 target 不能依赖"测试开关打开时才找到 clang"。
# ★ 顺序（D12 / T1c）：`lift-gen --check aot/closure.txt`（**构建期硬闸**，坏闭包当场红）
#   → `lift-gen --roots aot/closure.txt --out out/aot/lift_aot.ll --pin … --table …`
#   → `clang -c -O2` → `out/aot/lift_aot.obj`。
# ★ 缺 clang / 缺执行源 / 缺 closure.txt 的处置：**替身 TU**（不加 `.obj`、不定义
#   `LIFT_AOT_HAS_GENERATED`）⇒ configure / build **永不断**；AOT 启动时**响亮报告"没有生成物"**
#   并 exit 1（⛔ 不许静默跑成空壳）。与既有 `lift-ir-*` / `lift-ir-gen-*` 的处置同一套口径。
# ★ 告警口径：本块**不加**任何 `-W` / `/W` 覆盖（`/W4 /WX` 属于 `emulator-lift-runtime`）。
# =====================================================================================
set(LIFT_AOT_DIR "${CMAKE_CURRENT_BINARY_DIR}/aot")
set(LIFT_AOT_CLOSURE "${CMAKE_CURRENT_SOURCE_DIR}/aot/closure.txt")
set(LIFT_AOT_AGE_EXE "${CMAKE_CURRENT_SOURCE_DIR}/../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE")
find_program(LIFT_AOT_IR_CLANG NAMES clang clang.exe)
# =====================================================================================
# ★ AOT IR 的**形状**（唯一一处）：N 份 `.ll` + 每份 `clang -c -O2`。
#
#   · `LIFT_AOT_SPLIT=1` ⇒ **今天的单份路径**（`--out out/aot/lift_aot.ll`，产物路径一字不变，
#     逐字节同产物）。回退只需要 `-DLIFT_AOT_SPLIT=1`，⛔ 不用回滚源码。
#   · `LIFT_AOT_SPLIT=N>1` ⇒ `lift-gen --split N --out-dir out/aot` 一次全局分析、输出分 N 份
#     （按分量根 EA 升序贪心分桶），每份一条 `clang -c -O2`，N 个 `.obj` 全链进 AOT。
#   · **只有一份形状**：release 与 dev 都是这一个形状（⛔ 不搞两套 preset、⛔ 不搞 `-O1`/`-O0` 档）。
#     依据与读数：`.tmp/aot/irsplit/design.md` §3.3 / `.tmp/aot/irsplit/impl-report.md`。
# =====================================================================================
set(LIFT_AOT_SPLIT 8 CACHE STRING
    "How many pieces to split the AOT IR into: 1 = single-file path (today's shape); N>1 = greedy buckets over component-root EA ascending (N = 8 is the decided value)")
# =====================================================================================
# ★ LTO 档：**APPLE + WIN32 默认 ON**（2026-11-17 用户裁决：只要有可见优化就接受默认开；2026-11-18 win32 验证通过），
#   Linux 默认 OFF（未验证）。
#
#   为什么 mac 可以默认开：253cbe0 让生成物带上 `target-cpu` / `target-features` **函数属性**，
#   LLVM 的 `areInlineCompatible()` 那道闸被解开 ⇒ `lift_tick` / `lift_note_write` 真被内联。
#   实测（2026-11-17 / mac arm64 / Apple clang 17 + ld64）：FELLOW 进入窗口墙钟中位 **−12.61%**
#   （10 次交错 run，5/5 对全负）、`_lift_tick` 站点 **−86.4%**、`_lift_note_write` **−100%**、
#   语义指纹 10/10 逐位相同。代价：AOT 链接 **37.42 s / 3.03 GB**（OFF 0.16 s / 250 MB）、
#   `__text` **+3.14%**。读数与复现配方：`docs/03-emulator/aot-lto-evaluation.md` §10。
#
#   ★ win32 也已实测（2026-11-18 / clang-cl + lld-link 22.1.2）：窗口墙钟中位 **−9.25%**（5/5 对同向）、
#   `lift_tick` 调用 **104,750 → 0**、`lift_note_write` **39,075 → 0**（两者的函数体在反汇编里已不存在）、
#   `lift_dyn` +0.17%（不内联，与 mac 同向）、`__text` **−3.70%**、语义指纹 10/10 相同、
#   ctest 全绿。代价：AOT 链接 **100.1 s / 927 MB**（OFF 0.71 s）。读数：同文档 §12。
#   ⚠ **win32 有一个结构性障碍**：RelWithDebInfo 的 MSVC 默认 flags 带 `/Ob1`（"只内联标了 inline 的函数"），
#     clang-cl 把它落成函数级 **`noinline`** ⇒ `lift_tick` / `lift_note_write` / `lift_dyn` 的 IR 定义带
#     `noinline`，LTO **结构上**内联不了它们（实测：不解这一档时调用 104,750 → 105,617，一条没少、窗口 +2.75%）。
#     ⇒ `LIFT_AOT_LTO_ABI` 那条单点给 `lift_abi.cpp` 追加 `/Ob2`（**只在 WIN32**：clang 不认 `/Ob` 系列）。
#     这一段是"为什么 win32 上 LTO 真的有效"的唯一原因，⛔ 不要删。
#
#   ⛔ Linux 默认 OFF：LTO 只在 mac（Apple clang 17 + ld64）与 win32（clang-cl + lld-link 22.1.2）实测过，
#   Linux（ld.lld）未验证。要在 Linux 上试：`-DLIFT_AOT_LTO=ON`（未验证）。
#   回退（任一平台遇到问题）：`-DLIFT_AOT_LTO=OFF`（一行，⛔ 不用回滚源码）。
#
#   OFF ⇒ 逐字节 = 旧形状：`-c -O2` · `-flto` 被 configure 期硬闸拒绝 · shape = `splitN-O2`。
#   ON  ⇒ `LIFT_AOT_CLANG_FLAGS` 变 `-c -O2 -flto`；`-flto` 不再被硬闸拒绝（仍拒
#         `-O0/-O1/-Os/-Oz/-flto=thin/llc`）；`emulator-lift-aot` 的**链接行**也带 `-flto`
#         （lld-link 会打一条 `ignoring unknown argument '-flto'` 的 **warning** 并忽略它 —— 它
#         **自动**识别 bitcode 跑 LTO；ld64 / ld.lld 则需要这个参数才跑 LTO 后端）。
#   `LIFT_AOT_LTO_ABI`（仅 ON 时有效）= `src/runtime/lift_abi.cpp` 也编成 bitcode ⇒
#     `lift_tick` / `lift_note_write` / `lift_dyn` 才有机会被内联进 AOT（WIN32 再加 `/Ob2`，见上）。
#   `LIFT_AOT_LTO_RUNTIME`（仅 ON 时有效）= 整个 `emulator-lift-runtime`（含 shim）都编 bitcode。
#     ⛔ 默认 OFF：B2 那轮实测整库 LTO 只让 runtime 变大、无收益。
#
#   ⚠ 这是**默认形状**（不是实验档）：改默认值 / 改形状前先读 §10（mac）与 §12（win32）的读数与代价。
# =====================================================================================
if(APPLE OR WIN32)
    set(_LIFT_AOT_LTO_DEFAULT ON)
else()
    set(_LIFT_AOT_LTO_DEFAULT OFF)
endif()
option(LIFT_AOT_LTO "AOT IR + final link use -flto (APPLE / WIN32 default ON; Linux default OFF = unverified)"
       ${_LIFT_AOT_LTO_DEFAULT})
option(LIFT_AOT_LTO_ABI "Under LTO, also compile src/runtime/lift_abi.cpp as bitcode (tick/note/dyn can be inlined)" ON)
option(LIFT_AOT_LTO_RUNTIME "Under LTO, compile the whole emulator-lift-runtime (incl. shims) as bitcode" OFF)
if(NOT APPLE AND NOT WIN32 AND NOT LIFT_AOT_LTO)
    message(STATUS
        "emulator-lift-aot: LTO defaults to OFF on Linux (unverified); enable it with -DLIFT_AOT_LTO=ON (unverified)")
endif()

# ★ IR 的编译参数**唯一落点**（configure 期断言盯的就是这一行；OFF 档一字不动）。
set(LIFT_AOT_CLANG_FLAGS -c -O2)
if(LIFT_AOT_LTO)
    set(LIFT_AOT_CLANG_FLAGS -c -O2 -flto)
endif()
# ★ 本单：IR 的 target 函数属性（唯一的两个值）。**缺省空** = 不发属性（产物与今天逐字节相同）；
#   有 clang 的那条路上由下面的**探针**填（⛔ 不在这里写死任何 CPU 名）。
set(LIFT_AOT_IR_TARGET_CPU "")
set(LIFT_AOT_IR_TARGET_FEATURES "")
if(LIFT_AOT_IR_CLANG AND EXISTS "${LIFT_AOT_AGE_EXE}" AND EXISTS "${LIFT_AOT_CLOSURE}")
    set(LIFT_AOT_TARGET_FLAGS "")
    if(WIN32)
        # 与预设 `windows-x64` 的 clang-cl 一致（`-c` 产出 COFF，链接器与 clang-cl 编的 TU 同源）
        set(LIFT_AOT_TARGET_FLAGS --target=x86_64-pc-windows-msvc)
    endif()

    # =================================================================================
    # ★ configure 期断言（守卫 3，⛔ 只断言**不相容**，不断言任何墙钟 / 字节数）：
    #   ① `LIFT_AOT_SPLIT > 1` 时 IR 的参数必须**恰好**是档位值：OFF 档 `-c -O2`；ON 档 `-c -O2 -flto`；
    #   ② 出现 `-O1` / `-O0` / `-Os` / `-Oz` / `llc` / `-flto=thin` ⇒ 直接失败；
    #      `-flto` 只在 OFF 档失败（ON 档它是期望值的一部分）；
    #   ③ `.obj` 的个数必须 == `LIFT_AOT_SPLIT`（下面 `target_sources` 之前复核）；
    #   ④ `LIFT_AOT_SINGLE_TU`（如将来有人加）与 `LIFT_AOT_SPLIT > 1` 互斥。
    #   为什么是 configure 期：把 `-O2` 降级 / 换 LTO 是**唯一**能让"拆分构建"在墙钟上
    #   好看却在运行期变慢的自欺路径；这类改动必须**当场**失败，而不是构建出来再比读数。
    # =================================================================================
    if(LIFT_AOT_SINGLE_TU AND LIFT_AOT_SPLIT GREATER 1)
        message(FATAL_ERROR "LIFT_AOT_SINGLE_TU is mutually exclusive with LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT}")
    endif()
    if(LIFT_AOT_SPLIT GREATER 100)
        message(FATAL_ERROR "LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT} is out of range (<= 100)")
    endif()
    if(LIFT_AOT_SPLIT GREATER 1)
        # ★ 硬闸按档位分叉（两档都要断言）：OFF ⇒ `-c -O2` 且 `-flto/-flto=thin` 一律失败；
        #   ON ⇒ 必须恰好 `-c -O2 -flto`，仍拒 -O0/-O1/-Os/-Oz/-flto=thin/llc。
        if(LIFT_AOT_LTO)
            set(_LIFT_AOT_EXPECT "-c;-O2;-flto")
            set(_LIFT_AOT_BAD -O0 -O1 -Os -Oz -flto=thin llc)
        else()
            set(_LIFT_AOT_EXPECT "-c;-O2")
            set(_LIFT_AOT_BAD -O0 -O1 -Os -Oz -flto -flto=thin llc)
        endif()
        if(NOT "${LIFT_AOT_CLANG_FLAGS}" STREQUAL "${_LIFT_AOT_EXPECT}")
            message(FATAL_ERROR
                "with LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT} the AOT IR flags must still be `${_LIFT_AOT_EXPECT}`,"
                "actual = `${LIFT_AOT_CLANG_FLAGS}` (no -O1/-O0 downgrades; no LTO in the OFF profile)")
        endif()
        foreach(_bad IN ITEMS ${_LIFT_AOT_BAD})
            if("${LIFT_AOT_CLANG_FLAGS}" MATCHES "(^|;)${_bad}(;|$)")
                message(FATAL_ERROR
                    "with LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT} the AOT IR flags contain `${_bad}` (forbidden)")
            endif()
        endforeach()
    endif()

    # =================================================================================
    # ★ configure 期**探针**：IR 的 target 函数属性（`"target-cpu"` / `"target-features"`）。
    #   这是这两个值的**唯一来源** —— 用**同一份** clang（同一个 `LIFT_AOT_TARGET_FLAGS`）编一个
    #   极小的 C 文件，把前端写进函数属性的那一串读出来。⛔ 不写死 `apple-m1`：win 上是
    #   `x86-64` + 空 feature 集（`.tmp/codegen/report.md` §1.3 的实测），写死就等于把 mac 的裁决
    #   带进别的平台。
    #
    #   为什么需要"和运行库同一份"（本单的判据，⛔ 不是"让 codegen 用上更多指令"）：
    #   · `clang -###` 摊开看，AOT `.ll` 与 C++ TU 的 **codegen 参数逐项相同**（同一 `-target-cpu`
    #     + 同一串 feature）⇒ codegen 早就一致；差异只在**函数属性**这一层（C++ 前端会写，
    #     生成物过去一条都不写）。
    #   · LLVM 的 LTO 内联判据 `areInlineCompatible()` 比的是**函数属性**（callee 的 feature 必须是
    #     caller 的子集）⇒ 一侧缺属性 = 不相容 ⇒ `lift_tick` / `lift_note_write` 一条都内联不进 AOT
    #     （`.tmp/lto/report.md` §5.2/§5.3）。
    #   · 给 `.ll` 的 driver 加 `-mcpu=...` **无效**：实测产物逐字节相同（`-mcpu` 只影响 codegen，
    #     ⛔ 不给函数补属性）。
    #   · 补属性是 **codegen 中性** 的：手工把 C++ TU 的属性串注进 part00 后 `.obj` 与默认逐字节
    #     相同（`.tmp/codegen/report.md`）⇒ 于是"8 个 obj 的 sha256 逐一不变"能当本单的判据。
    #   ★ mac 的期望读数（与 `.tmp/codegen/attrs-min.txt` **逐字**相同）：
    #       target-cpu      = apple-m1
    #       target-features = +aes,+altnzcv,+bti,+ccdp,+ccidx,+complxnum,+crc,+dit,+dotprod,+flagm,
    #                         +fp-armv8,+fp16fml,+fptoint,+fullfp16,+jsconv,+lse,+neon,+pauth,
    #                         +perfmon,+predres,+ras,+rcpc,+rdm,+sb,+sha2,+sha3,+specrestrict,
    #                         +ssbs,+v8.1a,+v8.2a,+v8.3a,+v8.4a,+v8.5a,+v8a,+zcm,+zcz（36 个）
    #     ⚠ 读数会随 clang / 机器变（这正是"不能写死"的理由）⇒ 本块只断言"非空"，不断言具体值。
    # =================================================================================
    file(MAKE_DIRECTORY "${LIFT_AOT_DIR}")
    file(WRITE "${LIFT_AOT_DIR}/ir_target_probe.c" "int f(void) { return 0; }\n")
    execute_process(
        COMMAND ${LIFT_AOT_IR_CLANG} ${LIFT_AOT_TARGET_FLAGS} -S -emit-llvm -O2
                -o "${LIFT_AOT_DIR}/ir_target_probe.ll" "${LIFT_AOT_DIR}/ir_target_probe.c"
        RESULT_VARIABLE LIFT_AOT_PROBE_RC
        OUTPUT_VARIABLE _LIFT_AOT_PROBE_OUT   # 只为不把 clang 的输出混进 configure 日志
        ERROR_VARIABLE LIFT_AOT_PROBE_ERR)
    if(NOT LIFT_AOT_PROBE_RC EQUAL 0)
        message(FATAL_ERROR
            "IR target probe failed to compile (clang=${LIFT_AOT_IR_CLANG} rc=${LIFT_AOT_PROBE_RC}):"
            "`${LIFT_AOT_PROBE_ERR}`")
    endif()
    file(READ "${LIFT_AOT_DIR}/ir_target_probe.ll" _LIFT_AOT_PROBE_LL)
    #   两段正则各自只留一个捕获组 ⇒ 取 `CMAKE_MATCH_1`（探针 .ll 里的属性组形如
    #   `attributes #0 = { … "target-cpu"="<cpu>" "target-features"="<feat>" }`）。
    string(REGEX MATCH "\"target-cpu\"=\"([^\"]*)\"" _LIFT_AOT_PROBE_CPU
           "${_LIFT_AOT_PROBE_LL}")
    set(LIFT_AOT_IR_TARGET_CPU "${CMAKE_MATCH_1}")
    string(REGEX MATCH "\"target-features\"=\"([^\"]*)\"" _LIFT_AOT_PROBE_FEAT
           "${_LIFT_AOT_PROBE_LL}")
    set(LIFT_AOT_IR_TARGET_FEATURES "${CMAKE_MATCH_1}")
    if(NOT LIFT_AOT_IR_TARGET_CPU OR NOT LIFT_AOT_IR_TARGET_FEATURES)
        # ★ 这里必须**当场**失败：探针没拿到 target 属性 ⇒ 生成物与运行库的 LTO 属性会对不上 ⇒
        #   LTO 静默不内联（构建照绿、产物照跑，只是没有收益）。那是**没有任何症状**的失效。
        message(FATAL_ERROR
            "IR target probe got no target attributes (cpu=`${LIFT_AOT_IR_TARGET_CPU}`;"
            "features=`${LIFT_AOT_IR_TARGET_FEATURES}`) => generated IR and runtime LTO attributes would mismatch =>"
            "areInlineCompatible reports incompatibility and lift_tick / lift_note_write silently never inline."
            "probe output: `${LIFT_AOT_DIR}/ir_target_probe.ll` (clang=${LIFT_AOT_IR_CLANG})")
    endif()
    message(STATUS "emulator-lift-aot: IR target attributes = cpu `${LIFT_AOT_IR_TARGET_CPU}` / features `${LIFT_AOT_IR_TARGET_FEATURES}` (configure-time probe)")

    # ① 生成物：一次 custom command 写全部 `.ll`（N=1 = 一份）+ 分片清单（N>1）+ pin + 派发表。
    #    ★ `--closure-gate` = **构建期硬闸**（`--check` 的五条校验、同一行汇总）在**生成这一趟**
    #      里做 —— 不再有第二条独立的 `--check` 命令（那会把同一个 `buildAll` 跑两遍，省 48.6 s）。
    set(LIFT_AOT_IR_OUTPUTS "")
    set(LIFT_AOT_OBJS "")
    set(LIFT_AOT_OBJ_INPUTS "")
    if(LIFT_AOT_SPLIT GREATER 1)
        set(LIFT_AOT_GEN_OUTS "")
        foreach(_i RANGE 0 99)
            if(_i LESS ${LIFT_AOT_SPLIT})
                if(_i LESS 10)
                    set(_idx "0${_i}")
                else()
                    set(_idx "${_i}")
                endif()
                list(APPEND LIFT_AOT_GEN_OUTS "${LIFT_AOT_DIR}/lift_aot.part${_idx}.ll")
                list(APPEND LIFT_AOT_OBJS "${LIFT_AOT_DIR}/lift_aot.part${_idx}.obj")
            endif()
        endforeach()
        list(APPEND LIFT_AOT_GEN_OUTS "${LIFT_AOT_DIR}/lift_aot.split.txt")
        set(LIFT_AOT_SPLIT_MANIFEST "${LIFT_AOT_DIR}/lift_aot.split.txt")
        set(LIFT_AOT_GEN_STEP2
            --exe "${LIFT_AOT_AGE_EXE}" --roots "${LIFT_AOT_CLOSURE}"
            --closure-gate --split ${LIFT_AOT_SPLIT} --split-prefix lift_aot
            --out-dir "${LIFT_AOT_DIR}"
            --ir-target-cpu "${LIFT_AOT_IR_TARGET_CPU}"
            --ir-target-features "${LIFT_AOT_IR_TARGET_FEATURES}"
            --pin "${LIFT_AOT_DIR}/lift_pin.h"
            --table "${LIFT_AOT_DIR}/lift_table.inc")
    else()
        set(LIFT_AOT_GEN_OUTS "${LIFT_AOT_DIR}/lift_aot.ll")
        set(LIFT_AOT_OBJS "${LIFT_AOT_DIR}/lift_aot.obj")
        set(LIFT_AOT_SPLIT_MANIFEST "")
        set(LIFT_AOT_GEN_STEP2
            --exe "${LIFT_AOT_AGE_EXE}" --roots "${LIFT_AOT_CLOSURE}"
            --closure-gate
            --out "${LIFT_AOT_DIR}/lift_aot.ll"
            --ir-target-cpu "${LIFT_AOT_IR_TARGET_CPU}"
            --ir-target-features "${LIFT_AOT_IR_TARGET_FEATURES}"
            --pin "${LIFT_AOT_DIR}/lift_pin.h"
            --table "${LIFT_AOT_DIR}/lift_table.inc")
    endif()
    add_custom_command(
        OUTPUT ${LIFT_AOT_GEN_OUTS} "${LIFT_AOT_DIR}/lift_pin.h"
               "${LIFT_AOT_DIR}/lift_table.inc"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LIFT_AOT_DIR}"
        # ★ 一条命令做完"硬闸 + 生成"：`--closure-gate` 与 `--check` 共用同一份五条校验实现。
        COMMAND $<TARGET_FILE:emulator-lift-gen> ${LIFT_AOT_GEN_STEP2}
        DEPENDS emulator-lift-gen "${LIFT_AOT_CLOSURE}" "${LIFT_AOT_AGE_EXE}"
        COMMENT "lift-aot: closure-gate + roots/PE -> out/aot/lift_aot*.ll (split=${LIFT_AOT_SPLIT}) + lift_pin.h + lift_table.inc"
        VERBATIM)
    # ② `clang -c -O2` 每份一条（无 llc；`-Wno-override-module` = 这份 IR 是目标无关的，triple 交给驱动）
    #    ★ 这些 custom command **互不依赖**（只依赖自己的 `.ll`）⇒ ninja 天然并行、没有假串行。
    foreach(_obj ${LIFT_AOT_OBJS})
        string(REGEX REPLACE "\\.obj$" ".ll" _ll "${_obj}")
        add_custom_command(
            OUTPUT "${_obj}"
            COMMAND ${LIFT_AOT_IR_CLANG} ${LIFT_AOT_CLANG_FLAGS} ${LIFT_AOT_TARGET_FLAGS}
                    -Wno-override-module -o "${_obj}" "${_ll}"
            DEPENDS "${_ll}"
            COMMENT "lift-aot: ${_ll} -> ${_obj} (clang ${LIFT_AOT_CLANG_FLAGS}; no llc)")
        set_source_files_properties("${_obj}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    endforeach()
    list(LENGTH LIFT_AOT_OBJS LIFT_AOT_OBJ_COUNT)
    if(LIFT_AOT_SPLIT GREATER 1 AND NOT LIFT_AOT_OBJ_COUNT EQUAL LIFT_AOT_SPLIT)
        # 守卫 3-③：obj 数 != 份数 ⇒ 链接期缺符号 / 多一份孤儿（⛔ 不许静默）。
        message(FATAL_ERROR
            "AOT shape is self-contradictory: LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT} but obj count=${LIFT_AOT_OBJ_COUNT}")
    endif()
    target_sources(emulator-lift-aot PRIVATE
        src/aot/aot_dispatch.cpp
        src/aot/aot_feeder.cpp
        src/aot/aot_run.cpp
        ${LIFT_AOT_OBJS})
    # ★ `aot_dispatch.cpp` `#include "lift_table.inc"`（生成物）⇒ 必须显式声明文件级依赖，
    #   否则并行构建可能在生成之前就编译它（clean build 才显形的那种红）。
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/aot/aot_dispatch.cpp"
        PROPERTIES OBJECT_DEPENDS "${LIFT_AOT_DIR}/lift_table.inc")
    target_include_directories(emulator-lift-aot PRIVATE "${LIFT_AOT_DIR}")
    # ★ 这个宏是"生成物在场"的唯一判据（`aot_dispatch.cpp` 用它决定表有没有内容；
    #   `aot_main.cpp` 用它决定"没有生成物"的响亮报告）。
    # ★ `LIFT_AOT_SHAPE` = 形状进二进制 + 启动横幅一行（OFF 档 = `single-O2` / `splitN-O2`；
    #   LTO 档必须可辨识：后缀 `-lto`，再按档位加 `-parts`（只有 8 份 IR 是 bitcode）/
    #   `-abi`（+ lift_abi.cpp）/ `-rt`（+ 整个 runtime）⇒ 默认 mac 形状 = `split8-O2-lto-abi`）。
    if(LIFT_AOT_SPLIT GREATER 1)
        set(LIFT_AOT_SHAPE "split${LIFT_AOT_SPLIT}-O2")
    else()
        set(LIFT_AOT_SHAPE "single-O2")
    endif()
    if(LIFT_AOT_LTO)
        set(LIFT_AOT_SHAPE "${LIFT_AOT_SHAPE}-lto")
        if(LIFT_AOT_LTO_ABI)
            set(LIFT_AOT_SHAPE "${LIFT_AOT_SHAPE}-abi")
        else()
            set(LIFT_AOT_SHAPE "${LIFT_AOT_SHAPE}-parts")
        endif()
        if(LIFT_AOT_LTO_RUNTIME)
            set(LIFT_AOT_SHAPE "${LIFT_AOT_SHAPE}-rt")
        endif()
    endif()
    target_compile_definitions(emulator-lift-aot PRIVATE
        LIFT_AOT_HAS_GENERATED=1
        LIFT_AOT_SHAPE="${LIFT_AOT_SHAPE}"
        LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT})
    message(STATUS "emulator-lift-aot: execution sources = ${LIFT_AOT_OBJS} (shape=${LIFT_AOT_SHAPE}; clang = ${LIFT_AOT_IR_CLANG})")
else()
    message(STATUS
        "emulator-lift-aot: missing clang / execution source (${LIFT_AOT_AGE_EXE}) / closure.txt => stub TU:"
        "the build continues, but the AOT exe will loudly report 'no generated artifacts' at startup and exit 1")
    target_sources(emulator-lift-aot PRIVATE src/aot/aot_dispatch.cpp src/aot/aot_feeder.cpp src/aot/aot_run.cpp)
endif()

# =====================================================================================
# ★ LTO 档：运行库一侧的接线（⛔ 只在 `LIFT_AOT_LTO=ON` 时执行；OFF 档一行都不加）。
#
#   为什么必须单独接线：只给 8 份 `.ll` 加 `-flto` 时，链接器只看得到**这 8 份**的 IR ⇒
#   能内联跨 part 的 `lift_cc_*`，但看不到 `lift_tick` / `lift_note_write` / `lift_dyn`
#   （它们在 `src/runtime/lift_abi.cpp`，是另一个 TU，且被静态库 `emulator-lift-runtime`
#   收着）。要让它们进 LTO，那个 TU 必须也是 bitcode；要连 shim 一起内联，整个库都必须
#   是 bitcode（`LIFT_AOT_LTO_RUNTIME`，默认 OFF）。
#   ⚠ `-flto` 在**链接行**：一旦链进去的静态库成员里有 bitcode，ld64 / ld.lld 不加它就不跑 LTO 后端 ⇒
#     这里用 `INTERFACE` 让所有链 `emulator-lift-runtime` 的 target（`emulator-lift-gen` / 测试 exe / AOT 臂）
#     自动继承。**lld-link 相反**：它自动识别 bitcode 跑 LTO，`-flto` 只会打一条
#     `warning: ignoring unknown argument '-flto'`（warning，不是 error）并忽略 —— 保留它是无害的、
#     也是 mac / Linux 需要的形状，⛔ 不要按平台收窄（那会改动已被实测的形状而没有收益）。
#   ⚠ Linux（ld.lld）未验证；显式 `-DLIFT_AOT_LTO=ON` 属未验证路径 ⇒ 只告警，不拦
#     （硬闸仍按 ON 档断言 `-c -O2 -flto`）。
# =====================================================================================
if(LIFT_AOT_LTO)
    if(NOT APPLE AND NOT WIN32)
        message(WARNING
            "LIFT_AOT_LTO=ON is UNVERIFIED on Linux (ld.lld): linking will very likely fail."
            "the default profile only enables it on mac / win32; continuing because you asked for it explicitly.")
    endif()
    if(LIFT_AOT_LTO_ABI)
        # ★ win32 的结构性障碍与修正（2026-11-18，读数见 `docs/03-emulator/aot-lto-evaluation.md` §12）：
        #   RelWithDebInfo 的 MSVC 默认 flags 带 `/Ob1`（"只内联标了 inline 的函数"），clang-cl 把它落成
        #   函数级 **`noinline`** ⇒ `lift_tick` / `lift_note_write` / `lift_dyn` 的 IR 定义带 `noinline`，
        #   LTO **结构上**内联不了它们（实测：104,750 条 `lift_tick` 调用一条没少、窗口反而 +2.75%）。
        #   `/Ob2` 追加在 `-flto` 之后（clang-cl 后 `/Ob` 覆盖前 `/Ob`，3 行小文件验过）⇒ `noinline` 消失、
        #   tick/note 调用归零、`__text` −3.70%、窗口中位 −9.25%。**只 WIN32 加**：clang 不认 `/Ob` 系列。
        if(WIN32)
            set(_LIFT_AOT_ABI_COMPILE_OPTS "-flto;/Ob2")
        else()
            set(_LIFT_AOT_ABI_COMPILE_OPTS "-flto")
        endif()
        set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/runtime/lift_abi.cpp"
            PROPERTIES COMPILE_OPTIONS "${_LIFT_AOT_ABI_COMPILE_OPTS}")
    endif()
    if(LIFT_AOT_LTO_RUNTIME)
        target_compile_options(emulator-lift-runtime PRIVATE -flto)
    endif()
    if(LIFT_AOT_LTO_ABI OR LIFT_AOT_LTO_RUNTIME)
        # 运行库里出现 bitcode ⇒ 每个链它的 target 的链接行都要 `-flto`。
        target_link_options(emulator-lift-runtime INTERFACE -flto)
    endif()
    # AOT 臂自己的链接行：8 份 `.obj` 是 bitcode ⇒ 必须带 `-flto`。
    target_link_options(emulator-lift-aot PRIVATE -flto)
    message(STATUS
        "LIFT_AOT_LTO: ON (ABI=${LIFT_AOT_LTO_ABI} RUNTIME=${LIFT_AOT_LTO_RUNTIME}; shape=${LIFT_AOT_SHAPE})")
endif()

# =====================================================================================
# Shape A · T1b：`emulator-lift-closure`（**构建期 host tool**：census + PE → `closure.txt`）
# ★ 职责只有一件：把"实际执行过的块 EA 集合"（census）+ PE 字节 ⇒ **分量根**（入库件
#   `aot/closure.txt`）。⛔ 不发射 IR、⛔ 不改解释器、⛔ 不跑游戏（census 由既有解释器产出）。
# ★ 为什么与 `emulator-lift-gen` 分开：生成 `closure.txt` 是**改入库件**的写路径，只该在
#   "重新跑一次 census"时发生；`lift-gen` 是每次构建都跑的读路径（roots → `.ll`）。
#   两者共用 `.text` 判据与 hex 文法（`src/aot/lift_aot_text.h`），⛔ 不写第二份。
# ★ 位置与告警口径同上面两个 target（`if(LIFT_BUILD_TESTS)` 之前、不加任何 `-W` / `/W` 覆盖）。
# ★ 为什么要带 `lift_gen_emit.cpp`：`genBranchTarget`（`jmp/jcc/call rel32` 目标 = ea+len+imm32 回绕）
#   只有**一份**实现，就在那份文件里。⛔ 不许在本工具里再写一遍（两份判据必然漂）。
# =====================================================================================
add_executable(emulator-lift-closure src/aot/closure_build.cpp src/aot/lift_gen_emit.cpp)
target_link_libraries(emulator-lift-closure PRIVATE emulator-lift-runtime)


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
# =====================================================================================
# M2 · D3D9 最小设备簇（用户 2026-11-05 裁决"确认需要实现 D3D 接口"）
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
#   `d3d9!Direct3DCreate9` 从 stub 变真实现（register.cpp 里的 addUnimplemented 已删）；
#   IDirect3D9 / IDirect3DDevice9 只填**取证到的那几个槽**，其余 = comUnsupported 的 loud trap。
# =====================================================================================
target_sources(emulator-lift-runtime PRIVATE
    src/runtime/d3d9.cpp
    src/shims/d3d9.cpp)
# =====================================================================================
# M2 · D3DX9 Sprite 簇（D3DXCreateSprite → 第一帧；用户 2026-11-05 裁决"确认需要实现 D3D 接口"）
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
#   `d3dx9_43!D3DXCreateSprite` 从 stub 变真实现（register_import_gaps.cpp 里那一行随
#   `pnpm tools lift sync --write` 消失）；ID3DXSprite 的 14 槽里 +0x18/+0x1C = loud trap。
# =====================================================================================
target_sources(emulator-lift-runtime PRIVATE
    src/runtime/d3dx9_sprite.cpp
    src/shims/d3dx9.cpp)
# ★ GPU 路线 P0/P1：`d3dx9.cpp` 的 `registerD3DX9Shims` **无条件**调
#   `d3dx9::registerD3DX9EffectShims` ⇒ 实现必须**无条件**进 runtime。
#   ⛔ 不能只挂在 `LIFT_BUILD_TESTS` 里：体验版 `-DLIFT_BUILD_TESTS=OFF` 会在链接
#   `emulator-lift-aot` 时 `undefined symbol: lift::d3dx9::registerD3DX9EffectShims`
#   （2026-11-18 实测；`out-user` 的 runtime lib 里只有 `d3dx9.cpp.obj`，没有 effect 的 obj）。
target_sources(emulator-lift-runtime PRIVATE
    src/shims/d3dx9_effect.cpp)
# =====================================================================================
# M2 · D3D9 fixed-function 流水线（设备状态机 + 软件光栅 → 第一帧）
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
#   `runtime/d3d9_pipeline.{h,cpp}` = 变换 / 渲染状态 / 顶点缓冲 / 光栅 / 深度 / 纹理采样；
#   `shims/d3d9.cpp` 里补的槽见那边的注释（每条都带 `.lst` 站点 EA）。
# =====================================================================================
target_sources(emulator-lift-runtime PRIVATE
    src/runtime/d3d9_pipeline.cpp)
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
# =====================================================================================
# M2 · DirectXMath（**只在 `runtime/math.cpp` 里用**：`XMMatrixDecompose` 是
#   `D3DXMatrixDecompose` 的语义真源 —— 134 例与 native d3dx9_43 对拍一致，
#   见 `docs/03-emulator/d3dx-math.md`。⛔ `XMVECTOR` / DirectXMath 头**不准**进任何公共头
#   （`runtime/math.h` 继续只暴露 glm）⇒ 链接一律 PRIVATE。
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
# =====================================================================================
find_package(directxmath CONFIG REQUIRED)
target_link_libraries(emulator-lift-runtime PRIVATE Microsoft::DirectXMath)
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
# =====================================================================================
# M2 · PNG 解码（`D3DXCreateTextureFromFileInMemory` 的真解码 + 上传）
#   `REQ-01M738VYG0DP3TNJKYDSA49J7H` —— 三张 2048² Live2D 图集页（`texserial=21/22/23`）
#   原先"只读文件头、像素全 0"。`runtime/png_decode.cpp` = libpng **简化 API** 的解码层，
#   `shims/d3dx9.cpp` 把它解出来的 RGBA 经**既有** `d3d9::lockRect/unlockRect` 写进纹理 arena。
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
# ★ libpng 本来就躺在 vcpkg 安装树里（freetype 的传递依赖）—— 但那是**幽灵依赖**：
#   不显式 find_package + 链接，换一次依赖树或去掉 freetype 就会在链接期炸。
#   `vcpkg.json` 的 `dependencies` 里也已显式加 `libpng`（端口已装 ⇒ 不触发重建）。
# ★ 判据见 `tests/d3dx9.cpp` 的 `d3dx9/texture-from-memory` / `d3dx9/texture-from-memory-png`
#   （8×8 内嵌 PNG 的逐像素 + 脏区/版本不变量 + 坏输入不 trap）/ `d3dx9/png-decode`
#   （模块级：RGBA / 调色板 / 灰度 + 坏输入矩阵）/ `d3dx9/texture-upload-rollback`
#   （上传失败的失败面：arena + 状态块回滚）。
# =====================================================================================
find_package(PNG REQUIRED)
target_sources(emulator-lift-runtime PRIVATE
    src/runtime/png_decode.cpp)
target_link_libraries(emulator-lift-runtime PRIVATE PNG::PNG)

# =====================================================================================
# 控制层（`--control-root`）：WebSocket over `boost::asio` / `boost::beast` + `nlohmann::json`
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
# ★ 依赖从**清单**来（`vcpkg.json`；⛔ 不手工 vcpkg install、⛔ 不改 out/vcpkg_installed/）：
#   `boost-asio`（`"default-features": false`）+ `boost-beast` + `nlohmann-json`。
#   ⚠ 实测：`boost-beast` 依赖 `boost-asio` 时**没有**写 default-features ⇒ vcpkg 的"特性可加"模型
#     会把 asio 的默认特性重新打开 ⇒ 仍然装了 `boost-context` / `boost-date-time`（各一个静态库）。
#     这是**依赖账的事实**，记在报告里（design-ws §1.2 的"35 个 header-only 端口"因此不成立）。
# ★ `control_ws.cpp` 是**唯一** include beast / nlohmann 的 TU（`runtime/control.h` 用 PIMPL 挡住）；
#   别的 TU 只 include `runtime/control.h`（零第三方头 ⇒ 编译时间不受影响）。
# =====================================================================================
find_package(Boost CONFIG REQUIRED COMPONENTS asio beast)
find_package(nlohmann_json CONFIG REQUIRED)
target_sources(emulator-lift-runtime PRIVATE
    # `control_snapshot.cpp` = 快照的**主线程取数**（位置走 engine_marker 的唯一公式；⛔ 不碰 GPU）。
    #   它**不** include beast / nlohmann ⇒ pure CTest 可以直接驱动它。
    src/runtime/control_snapshot.cpp
    src/runtime/control_ws.cpp)
target_link_libraries(emulator-lift-runtime PRIVATE
    Boost::asio Boost::beast nlohmann_json::nlohmann_json)

enable_testing()

# =====================================================================================
# ★ AOT 形状守卫 ①：**分片清单自洽**（顶层无条件 ⇒ `pure` 档，默认 `ctest` 就跑）
#   判据在**被测二进制** `emulator-lift-gen --verify-split`（Σfunctions == PIN_FUNCTIONS / 符号集合
#   两两不交且并集 == 派发表 / 每份 ≤ 20 MB / 份数 == LIFT_AOT_SPLIT / 清单 sha256 与文件一致）。
#   ⛔ 不断言任何"当前数字"：全部由 `out/aot/` 的产物自己算。
#   没有生成物（缺 clang / 执行源 / closure.txt）时 `--verify-split` 自己 skip（exit 0，不红）。
#   ★ 2026-11-16：原来这条跑的是 `tests/cmake/lift_aot_split.cmake`（COMMAND ${CMAKE_COMMAND}）——
#     那不是 target、没有 `main(`，`cmake-test-entry` 的"注册名 ↔ 源文件"判据判不了它
#     （那条守卫防的是"空壳用例 ctest 照样绿"）。现在判据落在被测二进制自己身上，一点没弱。
# =====================================================================================
add_test(NAME lift-aot-split-manifest
         COMMAND emulator-lift-gen --verify-split "${LIFT_AOT_DIR}" --split ${LIFT_AOT_SPLIT})

# =====================================================================================
# ★ AOT 形状守卫 ②：**IR 的 target 函数属性与运行库对齐**（顶层无条件 ⇒ `pure` 档）。
#   判据在**被测二进制** `emulator-lift-gen --verify-ir-target`：每份 `.ll` 恰好一行
#   `attributes #0 = {`（两个值逐字等于 configure 期探针的读数）+ 每个 `define i32 @lift_cc_`
#   行都以 ` #0 {` 结尾。
#   为什么这是判据（⛔ 不是风格检查）：LLVM 的 LTO 内联器 `areInlineCompatible()` 比的是**函数属性**
#   —— 一侧缺属性 ⇒ `lift_tick` / `lift_note_write` 一条都不内联，而**没有任何东西会喊**
#   （构建成功、产物正常、运行结果一样，只是没有收益）。`.tmp/lto/report.md` §5.2/§5.3。
#   ★ 期望值就是"生成命令用的那两个变量"（同一份），⛔ 不从 `.ll` 里反读（那是自证）。
#   ★ `LIFT_AOT_SPLIT=1` 也在这同一处注册：份数由 `LIFT_AOT_SPLIT` 决定，产物名口径与
#     `--verify-split` 一致（N=1 ⇒ `lift_aot.ll`；N>1 ⇒ `lift_aot.partNN.ll`）。
#   没有生成物（缺 clang / 执行源 / closure.txt）时 `--verify-ir-target` 自己 skip（exit 0，不红）。
# =====================================================================================
add_test(NAME lift-aot-ir-target-attrs
         COMMAND emulator-lift-gen --verify-ir-target "${LIFT_AOT_DIR}" --split ${LIFT_AOT_SPLIT}
                 --ir-target-cpu "${LIFT_AOT_IR_TARGET_CPU}"
                 --ir-target-features "${LIFT_AOT_IR_TARGET_FEATURES}")

# =====================================================================================
# 测试（`LIFT_BUILD_TESTS`，默认 ON；`-DLIFT_BUILD_TESTS=OFF` ⇒ 只构建产品二进制）
# ★ 开关只包**测试 target 与 `add_test`**：产品二进制 `emulator-lift-aot`（+ 构建期工具
#   `emulator-lift-gen` / `-closure`）与依赖库 `lift-pl-mpeg` 在上面的产品区（OFF 时照样能编）。
# ★ 测试可执行文件统一落到 `${CMAKE_BINARY_DIR}/tests`（= `out/tests/`）；产品仍在 `${CMAKE_BINARY_DIR}`。
#   multi-config 生成器要为每个配置各设一次（Ninja 是单配置，用不带后缀那个）。
# ★ 本块**不缩进**：整块缩进会把 diff 变成全文件重写；CMake 不要求缩进。
# ★ **档位口径（2026-11-14 测试模型重做；判据 = 注册位置，⛔ 不建清单文件）**：
#   ① **接口合同 / 默认档**：顶层无条件 `add_test` ⇒ 默认 `ctest` 就跑；
#      判据 = **微软文档 / 公开规范**（返回值、错误码、边界、引用计数、所有权、pitch/stride、caps、
#      状态机）。⛔ 业务正确性不作判据（"到达某界面 / 存档链走通 / 播完了"是游戏的事）。
#   ② **压力/效率档（`LIFT_STRESS`）**：`if(DEFINED ENV{LIFT_STRESS})` ⇒ 只在 `LIFT_STRESS=1` 时注册；
#      判据 = **读数 + 阈值**（fps / 帧时间 p95 / 堆峰值 / 音频 underrun / 启动耗时 / 长跑稳定）。
#      **它是唯一允许跑真游戏的一档**；⛔ 判据里不许出现业务断言（稳定性除外：长跑不崩、guest fault = 0）。
#   ★ `repro` 档**已取消**（2026-11-14）：缺陷复现件**不进测试**，只作复现命令留在
#     `docs/03-emulator/defects.md` / `observations.md`（口径见 `REQ-01M7ATAB5A18W7XBCKFBEQS7HB`）。
# ★ **档位按用途，⛔ 不按"资产在不在"**（2026-11-13 用户裁决：「资产总是在场，总是用 `links/install`」）：
#   `if(EXISTS …)` 当**档位开关**是错的轴 —— 在我们的机器上它恒真，唯一效果是让"哪条用例存在"
#   随机器漂移（并让 README 变成假话）。⇒ `add_executable` / `add_test` 的注册位置**只**由
#   "这是合同 / 压力与效率"决定。**本文件里剩下的 `if(EXISTS …)` 只有"打印 STATUS
#   盘点"这一类，⛔ 不决定注册**。
# ★ **输入假定在场**：`LIFT_STRESS_GAME_DIR`（缺省 `links/install`）、LFS 语料（执行源 + `.lst`）、
#   两份宿主字体（`corpus/assets/fonts/Amayui-CN_cnjp{, -Bold}.ttf`）。
#   configure 期**总是**打一行 STATUS 盘点（每个输入 present / MISSING + 全路径）；
#   `LIFT_STRESS` **被显式打开**而它要的输入 MISSING ⇒ `message(FATAL_ERROR …)`；
#   默认档**不** FATAL —— 缺了就是那条用例自己红 / `LIFT_SKIP`(77)（由测试体自己判，⛔ 不缩水）。
# =====================================================================================
# Shape A 原型阶段 A · IR ↔ runtime 的 ABI 薄层（只追加源文件：⛔ 不改上面的源列表）
# ★ 落点 `src/runtime/lift_abi.{h,cpp}`：唯一一份 `extern "C"` 面（LiftRegs / 搬运函数 /
#   lift_note_write）；契约见 `.tmp/irshape/runtime-contract.md` §7 与 `src/runtime/lift_abi.h`。
# ★ 它**不属于**任何测试：产品二进制也要链它（将来 lifted 代码是产品的一部分）。
# =====================================================================================
target_sources(emulator-lift-runtime PRIVATE src/runtime/lift_abi.cpp)
# ★ EA → Insn 解码缓存（`lift_one` 的唯一加速层；契约见 `src/runtime/decode_cache.h`）：
#   四桶归因实测解码占 `lift_one` 的 79%（见该文件头注）⇒ 缓存 + **逐字节**校验。
target_sources(emulator-lift-runtime PRIVATE src/runtime/decode_cache.cpp)

# =====================================================================================
if(LIFT_BUILD_TESTS)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/tests")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/tests")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/tests")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/tests")

# ---- pure：**GPU 是唯一光栅实现**（`REQ-01M78BF1FNNH6K8N8GEY6GQYXX`）----
# ① 无 GPU ⇒ **拒绝启动**（非 0 退出 + stderr 明确指路）。用 `SDL_VIDEODRIVER=dummy` 强制
#    "建不出 GPU device"（⛔ 不靠真拔显卡）；后端在**执行源定位之前**建 ⇒ 这条不需要游戏资产。
# ★ 判据落在进程边界（退出码 + stderr 文案）上 ⇒ 由 `tests/gpu_batch.cpp` 起真子进程收输出；
#   子进程实参 = `$<TARGET_FILE:emulator-lift-aot>`（2026-11-16 从解释器臂换到 AOT 臂：两臂的
#   拒绝点与文案同源，实测同为 9 checks / 0 failures；⛔ 判据一条不变）。
# ★ 原 `lift/raster-flag-gone`（"`--raster` 必须继续不存在"）**已摘注册**：那条判的是**设计冻结**
#   （flag 已从代码里消失、没有静默失败路径；有人加回来 `git diff` 看得见）⇒ 按测试模型不配守卫。
add_test(NAME lift/no-gpu-refuses-start
    COMMAND lift-gpu-batch --no-gpu-refuses-start $<TARGET_FILE:emulator-lift-aot>)
set_tests_properties(lift/no-gpu-refuses-start PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy"
    TIMEOUT 120)

# ---- pure：`SetCursorPos` 的宿主光标 warp（`--cursor-warp`，缺省关）----
# 判据见 tests/cursor_warp.cpp 的头注释：① 缺省档一次也不挪；② 开着时用**guest 客户区坐标**
# 调一次（非零窗口原点 ⇒ 忘掉"减窗口原点"当场红）；③ 挪不动 ⇒ FALSE；④ 没有 guest 窗口 ⇒
# 只改虚拟位置、TRUE；⑤ 全链不 clamp（负坐标 / 越界原样送进 warp）。
# ★ 夹具 = 裸 Machine + 假 PresentSink（**不建窗口、不碰真光标**）⇒ 顶层无条件注册 = pure 档。
add_executable(lift-cursor-warp tests/cursor_warp.cpp)
target_link_libraries(lift-cursor-warp PRIVATE emulator-lift-runtime)
add_test(NAME cursor-warp COMMAND lift-cursor-warp)

# ---- pure：`--scale <auto|数值>` 的窗口尺寸 / DPI / readout / 鼠标映射 ----
# 判据见 tests/window_scale.cpp 的头注释：① 缺省 1.0 = 1280×720（readout 逐字）；
# ② 数值倍率 round(1280×s)×round(720×s)（1.25 / 1.5 / 1.33 的取整与非 16:9 残差）；
# ③ 鼠标映射只走 `cursor_map.h` 的既有公式（与 control_endpoint 判据 4 的参考点互逆）；
# ④ auto 的 DPI→scale 纯函数（取不到 ⇒ 1.0，readout 写明原因）；⑤ parse 的拒绝口径；
# ⑥ `--present headless` 下 readout 明说"不生效"。
# ★ 夹具 = 纯头（`window_scale.h` / `cursor_map.h`）：不建窗口、不碰 GPU、不读资产
#   ⇒ 顶层无条件注册 = pure 档（判据 = 注册位置，见 AGENTS.md §12.4）。
add_executable(lift-window-scale tests/window_scale.cpp)
target_link_libraries(lift-window-scale PRIVATE emulator-lift-runtime)
add_test(NAME window-scale COMMAND lift-window-scale)

# ---- pure：**AOT 臂**的 `--cursor-warp` / `--sound` 参数面（CLI 级 + 真子进程）----
# 判据见 tests/cli_aot_flags.cpp：① `--help` 里两个 flag 都在；② `--cursor-warp --present headless`
# ⇒ exit 2 且文案是"headless 不接受窗口参数"（⛔ 不是 `未知参数`）；③ 错拼 `--cursor-wrap`
# **仍然**是 `未知参数`（钉住 ② 不是靠"接受任何参数"）；④ `--sound` 被**接受**并打**恰好一条**
# 说明（含理由"没有音频管线" + 跟踪单占位 `TODO(REQ)`，⛔ 不是拒绝启动）；⑤ 对照组：不给
# `--sound` 时那条说明不出现。
# ★ 五条全部判在 **GPU bring-up 之前**（`aot_main.cpp` 的参数校验早于 `SdlWindow::init`）
#   ⇒ 不需要 GPU、不需要 LFS / 游戏安装 ⇒ **没有** `LIFT_SKIP_IF_NO_GPU`。
# ★ 顶层无条件注册 ⇒ pure 档（判据 = 注册位置，见 AGENTS.md §12.4）。
add_executable(lift-cli-aot-flags tests/cli_aot_flags.cpp)
target_link_libraries(lift-cli-aot-flags PRIVATE emulator-lift-runtime)
# ★ **必须显式依赖产品 exe**（`REQ-01M7K04EY6D3GQNB5DRWP0DYJ8` 洞 7 的连带发现）：
#   本用例**spawn** `emulator-lift-aot` 去读它的 `--help` / 退出码，而变异自检的
#   `buildTarget: 'lift-cli-aot-flags'` 只重建守卫 exe ⇒ 产品 exe 会停在**变异前** ⇒ 那条变异**假绿**。
#   加这一行之后 `cmake --build --target lift-cli-aot-flags` 会把产品 exe 一起重编重链。
#   ⚠ **任何 spawn 产品 exe 的 CTest 都要有这一行**（对照：`lift-aot-audio-wiring` 同形）。
#   ⛔ 不要把它当"顺手加的依赖"删掉 —— 删了不会红，只会让变异自检**静默**变成声称。
add_dependencies(lift-cli-aot-flags emulator-lift-aot)
add_test(NAME cli-aot-flags COMMAND lift-cli-aot-flags $<TARGET_FILE:emulator-lift-aot>)
set_tests_properties(cli-aot-flags PROPERTIES SKIP_RETURN_CODE 77)

# ---- pure：guest 退出码 → **宿主进程**退出码的纯映射（`REQ-01M7HP2YRZG222E0NE2H6MW6NK`）----
# 判据见 tests/guest_exit.cpp 文件头：0 ⇒ 0；非 0（含 127/128/255/256/259/0xFFFFFFFF 边界）⇒ 2
# 且**不透传** n。映射本身在 `src/runtime/guest_exit.h`（AOT 臂与解释器臂**同一份**）。
# ★ 顶层无条件注册 ⇒ pure 档（判据 = 注册位置，见 AGENTS.md §12.4）。
# ★ 本用例**故意只 include `runtime/guest_exit.h`**（不 include `machine.h`）——它同时是
#   "AOT 臂拿得到这条纯函数、不必把 `Machine` 拖进来"的编译期证据。
# ⚠ **弱守卫**：AOT 的 e2e 进程退出码面结构性测不到（`exit-code` 的夹具是合成 PE，AOT 只跑
#   被 pin 的 PE）⇒ 这里钉的是映射，钉不到 `aot_main` 的接线；接线靠代码审阅 + 手工读数。
# ★ 用例名 = `lift-guest-exit`：`tools/mutate-check.mjs` 的 `ctest:lift-guest-exit` 指向它。
add_executable(lift-guest-exit tests/guest_exit.cpp)
target_link_libraries(lift-guest-exit PRIVATE emulator-lift-runtime)
add_test(NAME lift-guest-exit COMMAND lift-guest-exit)

# ---- pure：guest 函数级 native override（差分 / 回退 / ABI）----
add_executable(lift-native-override tests/native_override.cpp)
target_link_libraries(lift-native-override PRIVATE emulator-lift-runtime)
add_test(NAME native-override COMMAND lift-native-override)

# ---- pure：`sub_4BBAB0`（Voice::Tick）的 stateless 快路径判据 ----
# 判据见 tests/voice_poll.cpp 的头注释（红了意味着什么 / k = 60 怎么来的）。
# ★ **顶层无条件**注册 ⇒ pure 档（判据 = 注册位置，见 AGENTS.md §5）。
# ★ 用例名 = `lift-voice-poll`：`tools/mutate-check.mjs` 的 `ctest:lift-voice-poll` 指向它。
add_executable(lift-voice-poll tests/voice_poll.cpp)
target_link_libraries(lift-voice-poll PRIVATE emulator-lift-runtime)
add_test(NAME lift-voice-poll COMMAND lift-voice-poll)

# ---- pure：SEH / C++ EH 的两个 OS 原语（`RaiseException` / `RtlUnwind` 的派发状态机）----
# 判据见 tests/seh.cpp 的头注释（A 参数逐位/时机、B "未处理必须响亮"、C RtlUnwind 端点与 esp 口径、
# D 影子栈展开丢帧）。红了意味着：`room-exit` 那条 C++ throw 的退出路径又断了，或者异常变成静默 no-op。
# ★ **顶层无条件**注册 ⇒ pure 档（判据 = 注册位置，见 AGENTS.md §5）。
# ★ 用例名 = `seh`：`tools/mutate-check.mjs` 的 `ctest:seh` 指向它。
add_executable(lift-seh tests/seh.cpp)
target_link_libraries(lift-seh PRIVATE emulator-lift-runtime)
add_test(NAME seh COMMAND lift-seh)

# ---- pure：**EA → Insn 解码缓存**的等价判据（`lift_one` 的 B 桶就是它）----
# 判据见 tests/decode_cache.cpp 的头注释：命中 / 未命中 / 字节被改后重解 / 页尾窗口不完整，
# 四种情形都必须与"每次都重新解码"给出**逐字段相同**的 Insn（含 op / 长度 / 操作数）。
# 红了意味着：缓存在**自我修改的代码**上给出了陈旧答案（静默跑旧指令；AOT 臂的
# `lift_note_write` 是 no-op ⇒ 逐字节校验是这条缺口唯一的自愈机制）。
# ★ **顶层无条件**注册 ⇒ pure 档（判据 = 注册位置，见 AGENTS.md §5）：只构造 GuestMemory + Decoder，
#   不载 PE、不读 LFS、不碰游戏安装。
# ★ 用例名 = `lift-decode-cache`：`tools/mutate-check.mjs` 的 `ctest:lift-decode-cache` 指向它。
add_executable(lift-decode-cache tests/decode_cache.cpp)
target_link_libraries(lift-decode-cache PRIVATE emulator-lift-runtime)
add_test(NAME lift-decode-cache COMMAND lift-decode-cache)

# ---- pure：**控制层端点**（`--control-root`；S0 传输 + S1 闸门/时钟）----
# 判据见 tests/control_endpoint.cpp 的头注释：监听失败点名端口 / 只绑 loopback /
# 错 token 与带 Origin 被拒 / hello 与 `ctl.<pid>.<nonce8>.json` 逐字段一致 /
# 未知 op ⇒ unknown_op 且 res.nonce 回显 / 收口只删自己那个文件 /
# 暂停 2 s 的 guest_ms 差 < 5 ms（判据 6）且配方守卫钟同冻。
# 红了意味着：端点能被人从非 loopback 连上、或"暂停对 guest 不可见"不成立、
# 或 hello 与端点文件各写了一份身份（两个来源漂）。
# ★ **顶层无条件**注册 ⇒ pure 档：自带裸 WS 客户端 + 注入时钟，⛔ 不 spawn 子进程、
#   不载 PE、不读 LFS、不碰游戏安装。
# ★ 用例名 = `lift-control-endpoint`：`tools/mutate-check.mjs` 的 `ctest:lift-control-endpoint` 指向它。
# ★ `WORKING_DIRECTORY` 钉在构建树：用例用相对路径建 `control-endpoint-work/`，
#   缺了它从源码树直接跑 exe 就会往 `apps/emulator-lift/` 里丢 untracked 目录（⛔ 不用 .gitignore 掩盖）。
add_executable(lift-control-endpoint tests/control_endpoint.cpp)
target_link_libraries(lift-control-endpoint PRIVATE emulator-lift-runtime
    $<$<PLATFORM_ID:Windows>:ws2_32>)
add_test(NAME lift-control-endpoint COMMAND lift-control-endpoint
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

# ---- pure：**控制层快照**（S3）----
# 判据见 tests/control_snapshot.cpp 的头注释：① 读不到时 `why` ∈ engine_marker 既有 8 个字面量
# （判据 3d）；② 位置只走 `readScriptPositionSample` 的唯一公式、`header_len == 0x3C`、`pos % 4 == 0`
# （判据 3a 的机制面）；③ `script_sha` 覆盖脚本体（文件 +4 字节 ⇒ sha 变；解析不到 ⇒ 枚举理由，判据 3c）；
# ④ 同一份快照序列化两次逐字节相同且**没有墙钟 / 自增字段**（判据 3b 的机制面）。
# 红了意味着：`sample` 的某格开始随"采样时刻"漂（同一暂停点两次不一样），或位置公式被第二份实现接管。
# ★ **顶层无条件**注册 ⇒ pure 档：只构造 GuestMemory + 临时目录里的 TITLE.BIN，⛔ 不碰 GPU / LFS。
add_executable(lift-control-snapshot tests/control_snapshot.cpp)
target_link_libraries(lift-control-snapshot PRIVATE emulator-lift-runtime)
add_test(NAME lift-control-snapshot COMMAND lift-control-snapshot)

# ---- pure：`HostServices::ownsCpu` 的**归属**判据（"这份 X86 是谁的"）----# 判据见 tests/host_services_owns_cpu.cpp 的头注释：① 真 owner 的那份 cpu ⇒ true；
# ② `X86 copy = machine.cpu()`（= `audio.cpp` 的 `audio_cpu_ = *cpu_`）⇒ `host` 非空
# 但 `ownsCpu(copy) == false`；③ 真实入口 `ddraw::createSurfaceState`（第一句就是
# `requireMachine(...)`）对副本**拒绝**、对 owner 成功。
# 红了意味着：`ownsCpu` 退化成"只判 host 非空" ⇒ 音频 feeder 的私有 `X86` 副本能从**非主线程**
# 碰无锁的 `SurfaceArena` / `GpuRaster`（静默数据竞争，见 runtime/host_services.h）。
# ★ **顶层无条件**注册 ⇒ pure 档（判据 = 注册位置，见 AGENTS.md §5）：`initBare()` 不载 PE、
#   不读 LFS、不碰游戏安装。
# ★ 用例名 = `lift-host-services-owns-cpu`：`tools/mutate-check.mjs` 的
#   `ctest:lift-host-services-owns-cpu` 指向它（变异 = `Machine::ownsCpu` 恒真）。
add_executable(lift-host-services-owns-cpu tests/host_services_owns_cpu.cpp)
target_link_libraries(lift-host-services-owns-cpu PRIVATE emulator-lift-runtime)
add_test(NAME lift-host-services-owns-cpu COMMAND lift-host-services-owns-cpu)
set_tests_properties(lift-host-services-owns-cpu PROPERTIES SKIP_RETURN_CODE 77)

# =====================================================================================
# 输入（C）：**路径只在这里定义一次**，下面的 STATUS 盘点与各 `add_test` 都引用它们。
# ★ 存在性**不**决定档位（见上面的档位口径）——它只进"盘点行"与"显式开档而缺 ⇒ FATAL"。
# =====================================================================================
# 真机安装根：仓库根的 `links/install`（本机符号链接，总指向真实安装树）。
# ★ 为什么是变量而不是字面量：本仓在 macOS 上也要跑要真机安装的那些用例（`tests/option_roundtrip.cpp`
#   等 e2e 的 POSIX 分支就是为它写的）。安装根写死成某个盘符会让 macOS / 换机器的用例**指着不存在的
#   路径** —— 而"没跑"与"跑绿了"在 `ctest` 输出里长得一样（AGENTS.md §5 的分级判据正是防这个）。
# ★ 落点只有一处：仓库根的 `links/install`（Windows / macOS 都是它，见 `.gitignore` 的 `/raw`、`/install`）。
#   ⛔ 它只决定"去哪找安装树"，**不决定档位**：档位只由用途（接口合同 / 压力与效率）决定。
#   显式覆盖：`cmake --preset <preset> -DLIFT_STRESS_GAME_DIR=<含 SYS4INI.BIN 的目录>`。
set(LIFT_STRESS_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../links/install"
    CACHE PATH "install root for the LIFT_STRESS profile (must contain SYS4INI.BIN)")
# LFS 语料：执行源（拆壳件）+ 反汇编 `.lst`（符号 / 对照 / 覆盖真源）。
set(AGE_SECTFIX_EXE "${CMAKE_CURRENT_SOURCE_DIR}/../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE")
set(AGE_LST "${CMAKE_CURRENT_SOURCE_DIR}/../../corpus/disasm/files/AGE.EXE__dumped.sectfix.EXE.lst")
# 两份宿主字体：常规字体的路径在产品区已由 `LIFT_HOST_FONT_PATH` 注入；粗体走同一条派生规则
# （`src/runtime/font.cpp` 的 `boldPathFor`：换掉 `.ttf` 后缀为 `-Bold.ttf`）——⛔ 不要写第二份字面量。
string(REGEX REPLACE "\\.ttf$" "-Bold.ttf" LIFT_HOST_FONT_BOLD_PATH "${LIFT_HOST_FONT_PATH}")
set(LIFT_INSTALL_SYS4INI "${LIFT_STRESS_GAME_DIR}/SYS4INI.BIN")

# ---- C：configure 期**总是**打这几行盘点（每个输入 present / MISSING + 全路径）----
foreach(_lift_input
        "${LIFT_INSTALL_SYS4INI}"
        "${AGE_SECTFIX_EXE}"
        "${AGE_LST}"
        "${LIFT_HOST_FONT_PATH}"
        "${LIFT_HOST_FONT_BOLD_PATH}")
    # 盘点行要写**全路径**（`..` 也归一化掉）：出问题时能直接复制去查。
    get_filename_component(_lift_abs "${_lift_input}" ABSOLUTE)
    if(EXISTS "${_lift_abs}")
        message(STATUS "input inventory: present  ${_lift_abs}")
    else()
        message(STATUS "input inventory: MISSING  ${_lift_abs}")
    endif()
endforeach()

# ---- C：**显式**开了 LIFT_STRESS 而它要的输入 MISSING ⇒ 响亮地失败 ----
# ★ 默认档**不** FATAL：缺输入时那条用例自己红 / SKIP 77（⛔ 不静默缩水成"没这条用例"）。
if(DEFINED ENV{LIFT_STRESS})
    foreach(_lift_required
            "${LIFT_INSTALL_SYS4INI}"
            "${AGE_SECTFIX_EXE}"
            "${AGE_LST}")
        if(NOT EXISTS "${_lift_required}")
            message(FATAL_ERROR
                "LIFT_STRESS was explicitly enabled for its purpose (it really runs the install tree), but inputs are missing:\n"
                "  MISSING  ${_lift_required}\n"
                "how to fix:\n"
                "  - install tree: link the real game install dir to `links/install` at the repo root (it must contain SYS4INI.BIN), or\n"
                "    `cmake --preset <preset> -DLIFT_STRESS_GAME_DIR=<dir containing SYS4INI.BIN>`;\n"
                "  - LFS corpus: `git lfs install && git lfs pull` (manifest: `corpus/assets.json`).\n"
                "do not disable inputs to make configure pass: the profile is chosen by purpose, and its inputs are assumed present.")
        endif()
    endforeach()
    # ★ TITLE→Quit 的 E2E 要的是**原版**安装树（`links/raw`），不是 `LIFT_STRESS_GAME_DIR`（缺省
    #   `links/install` = 汉化修订版，脚本偏移会漂）。⇒ 这一份输入**另判**，但⛔ 不新增全局输入变量：
    #   路径就是仓库根的 `links/raw`（与 `links/install` 同一条 `.gitignore` 口径）。
    set(_LIFT_STRESS_RAW_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../links/raw")
    if(NOT EXISTS "${_LIFT_STRESS_RAW_GAME_DIR}/AGE.EXE")
        message(FATAL_ERROR
            "LIFT_STRESS was explicitly enabled for its purpose, but the TITLE->Quit E2E needs the ORIGINAL install tree:\n"
            "  MISSING  ${_LIFT_STRESS_RAW_GAME_DIR}/AGE.EXE\n"
            "how to fix: link the original game install dir to `links/raw` at the repo root (not `links/install`: that one is the localized build,\n"
            "        its script offsets drift, so the exit-path coordinates/bytes are not comparable).\n"
            "do not disable inputs to make configure pass: the stress profile's inputs are assumed present.")
    endif()
endif()

# =====================================================================================
# 默认档：**真机安装根**上的设计合同（安装树**假定在场**；缺 ⇒ 用例自己 SKIP 77 / 红）
# ★ 2026-11-13 用户裁决：档位按**用途**，⛔ 不按"资产在不在" —— 这条是设计的合同（游戏自带字体的
#   缺字后备），就注册在**默认档**；⛔ 不要再用 `if(EXISTS …/SYS4INI.BIN)` 让它随机器漂。
# ★ 2026-11-13 CTest 瘦身：`overlay-window-smoke` 已删（叠层的像素判据由 pure 的 `overlay` ③④
#   覆盖；"带 `--overlay` 的真 boot 不崩"由其它起子进程的 CTest 覆盖）。
# =====================================================================================

# ---- 默认：游戏自带 `AGE-EXTEND.TTF` 的 U+E010（`REQ-01M79N1ZV391BCM7MYSKK0GVV0`）----
# 判据见 tests/gdi_font_fallback.cpp 的 `runAssets`（同一个可执行文件的 `--assets` 臂；先例 =
# `lift-overlay` 的 pure / assets 两次注册）：从 `SYS4INI.BIN` 的 ALF 索引按名字取出
# `AGE-EXTEND.TTF`（族名 `AGE Extend`）⇒ ①`FT_Get_Char_Index(face, 0xE010) != 0` 且
# `FT_Load_Char` 位图非空；②宿主 Amayui CN 下 `glyph(0xE010)` = `.notdef`（notdef +1）；
# ③登记成缺字后备之后同一码位由 `AGE Extend` 提供（fallback +1、mask 有墨迹、advance>0）；
# ④移除后备 ⇒ 回到 `.notdef`（notdef +1）。
# ★ 缺安装 / 缺宿主字体 / 索引里没这一条 ⇒ 测试自己 `LIFT_SKIP`(77) + 原因（⛔ 不假绿）；
#   ⛔ 不要为它加存在性闸（那就又回到"档位按资产在不在"的老路）。
add_test(NAME gdi-font-fallback-assets COMMAND lift-gdi-font-fallback --assets
    "${LIFT_STRESS_GAME_DIR}")
set_tests_properties(gdi-font-fallback-assets PROPERTIES
    SKIP_RETURN_CODE 77
    TIMEOUT 300)

# =====================================================================================
# 压力/效率档（`LIFT_STRESS=1`）：**唯一允许跑真游戏的一档**；判据 = **读数 + 阈值**
# ★ 判据口径（2026-11-14，测试模型单 `REQ-01M7ATAB5A18W7XBCKFBEQS7HB`）：
#   ⛔ 业务正确性不作判据（"到达某界面 / 存档链走通 / 播完了"是游戏的事）；
#   压力档只回答"模拟器自身"的量：fps / 帧时间 p95 / 堆峰值 / 音频 underrun / 启动耗时 / 长跑稳定。
# ★ `repro` 档**已取消**：缺陷复现件不进测试，只作复现命令留在
#   `docs/03-emulator/defects.md` / `observations.md`。
# ★ `DEFINED ENV{…}` 是**配置期**求值 ⇒ 改了环境变量必须重新 configure（`cmake --preset windows-x64`），
#   `ctest -N` 才会跟着变；`add_executable` 也在同一个 `if` 里 ⇒ 没开这档连 target 都不编。
# ★ **2026-11-16 退役了 6 条**（用户裁决：AOT 速度已远超解释器，"测试目标已不存在"）：
#   `feeder-seg-e2e` / `movie-logo` / `stress-boot` / `stress-title-steady` / `stress-adv-audio` /
#   `stress-gpu-coverage`（+ 6 个独占源文件），登记单 `REQ-01M7HQ57XV66FZQA5ECGHWD71N`。
#   ⇒ 那 6 条都是"带 `--native-overrides` 的解释器压力/效率读数"，AOT 结构上没有 override。
#   ⚠ **空窗期**：端到端压力档在 AOT 侧还没建（新档见 `REQ-01M7HQJEEENYDJ91FB5DAC4K23`）⇒
#     这一段里端到端速度只能人手跑（`scripts/recipes/*.plan` + `emulator-lift-aot --input-plan`），
#     ⛔ 不许把"没有读数"说成"没有退化"。
# =====================================================================================
if(DEFINED ENV{LIFT_STRESS})
    # ---- LIFT_STRESS：boot trace（A4）的**读数 + 阈值**面（2026-11-14 从默认档移入）----
    # ★ 为什么不在默认档：它跑**真游戏**（真镜像 + `.lst` + 真 boot 到 L2），按测试模型不属于
    #   "要真游戏"的接口合同档（默认档只测 shim 合同 + 执行器核心，不需要游戏安装）。
    # ★ 判据（tests/boot_trace.cpp 文件头）：同一条命令跑两次 ⇒ 停因 / eip / 指令数 / 状态摘要哈希
    #   三组读数逐字节相同（虚拟时钟与"宿主侧不许引入随机源"），另比 `--trace-shims` 的 trace 字节。
    #   ⇒ 是**稳定性读数**（确定性），不是"能到 L2 就算业务做对了"。
    # ★ 缺语料 ⇒ 测试自己 `LIFT_SKIP` 77（⛔ 不假绿）。
    add_executable(lift-boot-trace tests/boot_trace.cpp)
    target_link_libraries(lift-boot-trace PRIVATE emulator-lift-runtime)
    add_test(NAME boot-trace COMMAND lift-boot-trace)
    set_tests_properties(boot-trace PROPERTIES
        ENVIRONMENT "LIFT_AGE_EXE=${AGE_SECTFIX_EXE};LIFT_AGE_LST=${AGE_LST}" SKIP_RETURN_CODE 77)

    # ---- LIFT_STRESS：派发层固定税（b / c 的**计时读数**；2026-11-14 从默认档移入）----
    # ★ 移档理由（终裁 3）：①/② 的判据是**计时的量级带**（`kDeltaBTolNs` / `kLockCostFloorRatio`）
    #   = 效率读数，不是对外合同 ⇒ 默认门禁不跑它；③④⑤ 诊断开关三块已删（我方摘要文案 + 计数器）。
    # ★ 读数进 `docs/03-emulator/observations.md`（OBS-20261114-01）：
    #     $env:LIFT_STRESS=1; ctest --test-dir out -R dispatch-tax --output-on-failure
    # ★ ⛔ 默认档不留"也跑一遍"的壳：`add_executable` 与 `add_test` 都在这个 `if` 里。
    # ★ 需求单 `REQ-01M70S2WR4S8MN2JDWJRJD6WFD` 的 verify 仍是 `dispatch_tax.cpp#dispatch-tax`
    #   （字面量在文件里），只是它现在只在开档时跑（档位变化由台账注记说明）。
    add_executable(lift-dispatch-tax tests/dispatch_tax.cpp)
    target_link_libraries(lift-dispatch-tax PRIVATE emulator-lift-runtime)
    add_test(NAME dispatch-tax COMMAND lift-dispatch-tax)

    # ---- LIFT_STRESS：TITLE→Quit 退出路径的**端到端复现件**（2026-11-17）----
    # ★ 为什么是 stress 而不是默认档：它**真跑原版安装树**（`links/raw` + TITLE 界面 + 135 帧），
    #   判据是"guest 自己的退出路径走完"（`Command_Exit_Exception` 的 catch 续延 → `ExitProcess`），
    #   ⛔ 不是某个 shim 接口的合同 ⇒ 按用途只能进"唯一允许跑真游戏"的 stress 档。
    # ★ 默认档的**机制守卫**是 `lift-closure-imm32`（候选集 + 派发表里必须有三行
    #   `LIFT_AOT_ENTRY`）—— 它在顶层无条件注册，跑的是扫描 + 生成物，不跑游戏。
    #   本 E2E 是它的**端到端复现件**，不是门禁；缺 `links/raw` ⇒ 测试自己 `LIFT_SKIP(77)`。
    # ★ 输入检查：configure 期已在上面把 `links/raw/AGE.EXE` 缺省判成 `FATAL_ERROR`（stress 档口径），
    #   ⛔ 不新增第二个全局输入变量。
    add_executable(lift-aot-quit-exit tests/aot_quit_exit.cpp)
    target_link_libraries(lift-aot-quit-exit PRIVATE emulator-lift-runtime)
    # ★ 读的是**产品 target 的产物**（`emulator-lift-aot`）⇒ 显式依赖：否则 `--target
    #   lift-aot-quit-exit` 只编守卫 exe、产品 exe 停在变异前 ⇒ 变异自检假绿。
    add_dependencies(lift-aot-quit-exit emulator-lift-aot)
    add_test(NAME aot-quit-exit
        COMMAND lift-aot-quit-exit
                "$<TARGET_FILE:emulator-lift-aot>" "${_LIFT_STRESS_RAW_GAME_DIR}"
                "${_LIFT_STRESS_RAW_GAME_DIR}/AGE.EXE" "${AGE_SECTFIX_EXE}")
    set_tests_properties(aot-quit-exit PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 420)
endif()

# =====================================================================================
# 测试分级（AGENTS.md §5「CTest 那一档不写 pragma，判据是注册位置」）：
#   - **顶层无条件注册** ⇒ 默认 ctest 就跑（**按 shim 接口设计的接口合同**）⇒ pure
#   - `if(DEFINED ENV{LIFT_STRESS})` 里 ⇒ **压力/效率档**（判据 = 读数 + 阈值；唯一允许跑真游戏的一档）
# ★ 档位只看**用途**，⛔ 不看"资产在不在"（见文件顶部的档位口径）；输入假定在场。
# ⛔ 不要用 `if(EXISTS …)` 决定 `add_executable` / `add_test` 注册：那会让"哪条用例存在"随机器漂，
#   并让"没跑"与"跑绿了"在输出里长得一样。
# ★ `repro` 档已取消（2026-11-14）：缺陷复现件不进测试，只作复现命令留在
#   `docs/03-emulator/defects.md` / `observations.md`（口径见 `REQ-01M7ATAB5A18W7XBCKFBEQS7HB`）。
# =====================================================================================

# ---- pure：自造机器码字节，不碰游戏 / LFS ----
add_executable(lift-conformance tests/conformance.cpp tests/conformance_cases.cpp)
target_link_libraries(lift-conformance PRIVATE emulator-lift-runtime)
add_test(NAME conformance COMMAND lift-conformance)

# ---- pure：A1 的第二遍 —— 同一批语义夹具在 LIFT_SAFE_MEMORY=1（逐页提交检查）下重跑 ----
# ★ 以前只有 README 里一句人工命令 ⇒ 这一档没有门禁（plan §5 A1 要求"复跑全绿"）。
# ★ 2026-11-13 回到 **pure**（顶层无条件注册）：**实测 0.53 s（2026-11-13，本机 RelWithDebInfo）**
#   ⇒ 它不是"贵用例"，没有理由走 `LIFT_STRESS` opt-in（那会让"没跑"与"跑绿了"长得一样）。
#   判据 = 注册位置（`AGENTS.md` §5）：顶层 ⇒ 默认 `ctest` 就跑。
#     ctest --test-dir out -R conformance-safe --output-on-failure
add_test(NAME conformance-safe COMMAND lift-conformance)
set_tests_properties(conformance-safe PROPERTIES ENVIRONMENT "LIFT_SAFE_MEMORY=1")

# ---- pure（默认档）：恒等判据的**负控** —— 空心夹具必须被 conformance 判红 ----
# ★ 判据（红了意味着什么）：`LIFT_CONFORMANCE_SELFCHECK=1` 先要求真用例表 **0 红**，再要求一条
#   "能到哨兵但什么都没改"的夹具**只**因恒等判据失败（总失败恰好 +1、identity 恰好 +1）。
#   ⇒ 把 `sameStateIgnoringSentinelRet` 的 `esp+4` 规范化退回 `== before`，这条当场红 ——
#   那是"恒等判据真的会红"的机械证据（⛔ 不是"判据恒假但一直绿"）。
# ★ 2026-11-13 用户裁决：它是**身份判据的负控**、**实测 0.5 s**、**不需要安装树**（conformance 是
#   自造字节）⇒ 按**用途**回**默认档**（顶层无条件注册）：它回答的是"整张用例表的恒等判据还有效吗"
#   —— 判据失效时**每一条 conformance 用例**都在假绿，所以它必须每次提交都跑。
#   ⛔ 不要拿 `if(EXISTS …)` / `if(DEFINED ENV{LIFT_STRESS})` 把它藏起来（那正是"档位按错轴"）。
add_test(NAME conformance-rejects-hollow-case COMMAND lift-conformance)
set_tests_properties(conformance-rejects-hollow-case PROPERTIES
    ENVIRONMENT "LIFT_CONFORMANCE_SELFCHECK=1")

# ---- pure：`rep movs*` / `rep stos*` 宿主快路径的两臂差分 + 闸门 ----
# ★ 判据（`tests/rep_bulk.cpp` 的头注）：同一份 guest 字节、`X86::rep_bulk` 开/关两臂 ⇒
#   目标区逐字节 + ECX/ESI/EDI/EAX/EFLAGS + insns + `g_access` 全同；闸门逐条（跨 4 GiB 的
#   64 位乘 / safe-mode committed / ECX==0 / 方向×重叠）另用 `repBulkPlan` 直接断言；
#   命中计数保证"快路径真的被走到"（否则全绿也可能是快路径一直没生效）。
# 规格：REQ-01M75XK0XRTSWCZ4G8A5W2GW8。
add_executable(lift-rep-bulk tests/rep_bulk.cpp)
target_link_libraries(lift-rep-bulk PRIVATE emulator-lift-runtime)
add_test(NAME rep-bulk COMMAND lift-rep-bulk)

add_executable(lift-dispatch tests/dispatch.cpp)
target_link_libraries(lift-dispatch PRIVATE emulator-lift-runtime)
add_test(NAME dispatch COMMAND lift-dispatch)

add_executable(lift-input-tick tests/input_tick.cpp)
target_link_libraries(lift-input-tick PRIVATE emulator-lift-runtime)
add_test(NAME input-tick COMMAND lift-input-tick)

# ---- pure：脚本身份 marker 事件源（`TriggerKind::Marker`）----
# ★ 顶层无条件注册 ⇒ pure 档：假 guest 内存布置 Engine 单例 / cur / frame[cur].+0x3C / 名字表；
#   另有一条真 `Machine::run` 的集成用例（自造字节放在 `0x4123F0` / `0x4123E5` 两个采样 EA），
#   钉住"钩子真的在循环头采样"。⛔ 不需要游戏安装 / LFS。
add_executable(lift-input-marker tests/input_marker.cpp)
target_link_libraries(lift-input-marker PRIVATE emulator-lift-runtime)
add_test(NAME input-marker COMMAND lift-input-marker)

# ---- pure：时间线的 **0 点**（`--anchor-script <id>` = 首次**观察到**进入引擎脚本 <id>）----
# ★ 顶层无条件注册 ⇒ pure 档：前半是纯 `InputScript`（四个单位直接喂），后半是裸 `Machine` +
#   自造字节摆在 VM 循环头（⛔ 不依赖游戏安装 / LFS / 真实窗口）。
#   红 = 0 点语义坏了（首样本不闩锁 / 重进又定基 / 闩锁前没抑制 / pending 期间被折算 /
#   四单位只重定基了俩 / 没观察到时静默 exit 0 / 与交互图不互斥）。
add_executable(lift-input-anchor tests/input_anchor.cpp)
target_link_libraries(lift-input-anchor PRIVATE emulator-lift-runtime)
add_test(NAME input-anchor COMMAND lift-input-anchor)

# ---- pure：输入**投递层** S1 的关系断言（`REQ-01M7G7EMX3JR272D8FVM7JFY9D`）----
# ★ 判据（红了意味着什么）：每个读点调一次 shim ⇒ `InputPlan::deliverCalls(point)` 与
#   `ShimDef::call_count` **逐点相等**。两个数来自**两个独立来源**（计划自己的账 vs shim 框架
#   唯一的 `++` 点 `shims.cpp`）⇒ 红了 = 有一条读点没经过投递入口 / 投递入口被挪走了。
# ★ 顶层无条件注册 ⇒ **pure** 档（夹具 = `initBare` + 自造机器码，⛔ 不需要游戏 / LFS / 窗口）。
add_executable(lift-input-plan-read-points tests/input_plan.cpp)
target_link_libraries(lift-input-plan-read-points PRIVATE emulator-lift-runtime)
add_test(NAME input-plan-read-points COMMAND lift-input-plan-read-points)

# ---- pure：**注入真的能被引擎看见**（2026-11-19 裁决；`tests/input_inject_visible.cpp`）----
# ★ 判据（红了意味着什么）：
#   ① click 的**两段形状**回来了吗（move 与 down 同读点 = 引擎用的还是 move 之前的锁存位置）；
#   ② 抬起闸门是不是"按下之后至少 N 个 **KeyLevel** 读点"（换回 `inputSampleCount()` 就红 ——
#      那个计数把 `GetCursorPos` 也算样本，位置读点能满足它而按键一次都没被读到，2026-11-19 的根因）；
#   ③ 两条墙钟兜底（不读位置 ⇒ 一定按下；不读按键 ⇒ 一定抬起，且到期之前不许动作）；
#   ④ 注入会话结束（配方走完 / `releaseAll`）⇒ 光标所有权交还 OS（真鼠标重新生效）。
# ★ 顶层无条件注册 ⇒ **pure** 档（不建 SDL 窗口、不需要游戏 / LFS）。
# ⚠ "引擎会不会命中那个宝藏"不在这里（那是人工 SAVE05 的事）；本用例只判注入层的时序与所有权。
add_executable(lift-input-inject-visible tests/input_inject_visible.cpp)
target_link_libraries(lift-input-inject-visible PRIVATE emulator-lift-runtime)
add_test(NAME input-inject-visible COMMAND lift-input-inject-visible)

# ---- 2026-11-14：`interaction-graph`（`tests/interaction_graph.cpp`）已按测试模型删除 ----
# 判据是"交互图走通 / 等一屏 → 点一下"这类**业务正确性**（测试模型单
# `REQ-01M7ATAB5A18W7XBCKFBEQS7HB` 点名取消）；它**不进测试**。复现命令见
# `docs/03-emulator/defects.md` BUG-16。
# ★ 2026-11-16：**入口本身也退役了**（`--interaction-graph` / `--interaction-trace` /
#   `--interaction-fail-dump` 三个 CLI 参数 + 仓根 `graphs/` + `scripts/interaction/`）；
#   配方只经 AOT 臂 `--input-plan`（落点 `scripts/recipes/*.plan`）。登记单
#   `REQ-01M7HQ57XV66FZQA5ECGHWD71N`。`runtime/interaction_graph.*` 保留为机器内驱动器。

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

# =====================================================================================
# pure：**注入通道不依赖 OS**（2026-11-08）
# ★ 判据（红了意味着什么）：一份 `--input-script` 时间线在**没有 SDL 视频、没有窗口焦点、
#   没有任何人挪过真实光标**的进程里跑完之后，guest 通过它自己的读点看到注入：
#   `GetAsyncKeyState` 返回 0x8000、`GetCursorPos` 返回注入坐标 + 窗口原点、`PeekMessageA`
#   取到 WM_LBUTTONDOWN / WM_KEYDOWN / WM_LBUTTONUP；而 `click` 的按下与抬起跨了一帧
#   （guest 必须两态都采到）。
# ★ 顶层无条件注册 ⇒ **pure** 档（不建 SDL 窗口、不需要游戏 / LFS）。
# ⛔ 不许给它 `SDL_VIDEODRIVER=dummy` 之类的环境：那会掩盖"注入其实靠 SDL 事件泵"的回归。
# =====================================================================================
add_executable(lift-input-injection tests/input_injection.cpp)
target_link_libraries(lift-input-injection PRIVATE emulator-lift-runtime)
add_test(NAME input-injection COMMAND lift-input-injection)

# =====================================================================================
# pure：时间线的**效果级判据**（`--expect-change-after` / `--expect-change-window`）
# ★ 判据（红了意味着什么）：动作被应用之后，运行期必须在 N 个**采样**内看到画面变化；
#   到点没变 / 一次采样都没有 / 跑完未判定 ⇒ `InteractionFailed`（`main` 走 exit 5）。
#   ⛔ 不许"发完不管却 exit 0"——BUG-16 就是这么漏掉的。
# ★ 夹具：裸 Machine + 自造 guest 循环 + 采样注入 shim（⛔ 不依赖游戏安装 / 窗口 / LFS）。
# ★ 顶层无条件注册 ⇒ **pure** 档。
# =====================================================================================
add_executable(lift-input-expect tests/input_expect.cpp)
target_link_libraries(lift-input-expect PRIVATE emulator-lift-runtime)
add_test(NAME input-expect COMMAND lift-input-expect)

# =====================================================================================
# pure：输入脚本的**否定证据**（`input-script: unfinished`；BUG-20）
# ★ 判据（红了意味着什么）：条件永不满足的动作在 run 结束时**必须**留下可解析的读数
#   （`pending=<n>` + `first="<原动作行>"`）；全部已应用 / 脚本自己 `quit` 时**不许**有那一行。
# ★ 夹具：裸 Machine + 自造 guest 循环（**一次 Present 都没有** ⇒ `frame` 恒 0）+ 捕获 stdout。
#   ⛔ 不依赖游戏安装 / 窗口 / LFS。★ 顶层无条件注册 ⇒ **pure** 档。
# =====================================================================================
add_executable(lift-input-unfinished tests/input_unfinished.cpp)
target_link_libraries(lift-input-unfinished PRIVATE emulator-lift-runtime)
add_test(NAME input-unfinished COMMAND lift-input-unfinished)

# ---- 2026-11-14：`skip-logo`（`tests/skip_logo.cpp`）已按测试模型删除 ----
# 它断的是**我方钩子的内部形态**（只写 `T_Command+0x5EB5C` 这一格、邻居不动）⇒ BUG-14 反模式；
# 口径留在 `apps/emulator-lift/README.md` §4.1b 与 `docs/03-emulator/defects.md`，
# 复现命令见 `docs/03-emulator/observations.md`。

# ---- pure：MessageBox（uType→返回值表 / headless 不阻塞 / 模态状态机 / Yield 单令牌）----
# ★ 判据见 `tests/messagebox.cpp` 文件头。顶层无条件注册 ⇒ **pure** 档（不需要安装 / 窗口 / LFS）：
#   假 sink 覆盖系统框缝（`showSystemMessageBox`）；真闭窗链是**缺陷复现件**，不进测试
#   （复现命令见 `docs/03-emulator/observations.md`；测试模型单 `REQ-01M7ATAB5A18W7XBCKFBEQS7HB`）。
add_executable(lift-messagebox tests/messagebox.cpp)
target_link_libraries(lift-messagebox PRIVATE emulator-lift-runtime)
add_test(NAME messagebox COMMAND lift-messagebox)

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

# =====================================================================================
# 窗口调试叠层（`--overlay`，默认关）—— 判据见 `tests/overlay.cpp` 文件头
# =====================================================================================
# ★ 顶层无条件注册 ⇒ **pure** 档（字体 / 格式化 / 只写矩形 + software 渲染器上的可见性）。
#   dummy 视频下只测窗口/纹理/上传/回读的机械正确性；`--gpu` 那条测**直出**（无 GPU ⇒ SKIP 77）。
#   ★ `overlay-gpu` 必须不带 SDL_VIDEODRIVER=dummy：直出要真的 claim 窗口。
add_executable(lift-overlay tests/overlay.cpp)
target_link_libraries(lift-overlay PRIVATE emulator-lift-runtime)
add_test(NAME overlay COMMAND lift-overlay)
set_tests_properties(overlay PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy" SKIP_RETURN_CODE 77)
add_test(NAME overlay-gpu COMMAND lift-overlay --gpu)
set_tests_properties(overlay-gpu PROPERTIES SKIP_RETURN_CODE 77)

# =====================================================================================
# M0 · UI 画布原点（`ui/canvas.h`）+ 宿主字体文本栅格化（`ui/raster.cpp` / `ui/hud.cpp`）
# =====================================================================================
# ★ `ui-canvas`：纯函数 `uiCanvasOrigin`（B2 的唯一一份算术）—— 顶层无条件注册 ⇒ **pure** 档。
#   红了 = 16:9 偏移非 0 / 非 16:9 的居中偏移丢了（HUD 会落到黑边里或贴边）。
add_executable(lift-ui-canvas tests/ui_canvas.cpp)
target_link_libraries(lift-ui-canvas PRIVATE emulator-lift-runtime)
add_test(NAME ui-canvas COMMAND lift-ui-canvas)

# ★ `ui-text`：中文（`天結`）经宿主字体出字 + display list 栅格化 + 字体缺失 = 硬失败。
#   顶层无条件注册 ⇒ pure 档；**缺字体文件 ⇒ SKIP 77**（与 `gdi-font` 同一档，⛔ 不假绿）。
add_executable(lift-ui-text tests/ui_text.cpp)
target_link_libraries(lift-ui-text PRIVATE emulator-lift-runtime)
add_test(NAME ui-text COMMAND lift-ui-text)
set_tests_properties(ui-text PROPERTIES SKIP_RETURN_CODE 77)

# ---- GPU 光栅化（唯一实现）：独立 SDL_GPUDevice + 离屏 target + 读回 ----
# 两档都要求真的 GPU 设备：dummy 视频下建不出设备 ⇒ 返回 77（SKIP），⛔ 不是"跑绿"。
add_executable(lift-gpu-raster tests/gpu_raster.cpp)
target_link_libraries(lift-gpu-raster PRIVATE emulator-lift-runtime)
add_test(NAME gpu-raster COMMAND lift-gpu-raster)
set_tests_properties(gpu-raster PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy" SKIP_RETURN_CODE 77)
add_test(NAME gpu-raster-gpu COMMAND lift-gpu-raster --gpu)
set_tests_properties(gpu-raster-gpu PROPERTIES SKIP_RETURN_CODE 77)

# ---- GPU 直出（`--present window`）：窗口 claim + target→swapchain ----
# 判据是**窗口像素 == 送进去的后缓冲**（含 16bpp 转换 / 尺寸变化 / 连续两帧）。
# 窗口 claim 不成立（dummy 视频 / 无桌面）⇒ SKIP 77：⛔ 不拿 renderer 路径冒充直出。
# ★ 顶层无条件注册（不需要游戏 / LFS）⇒ pure 档；缺窗口系统时按 SKIP 处理。
add_executable(lift-gpu-direct-present tests/gpu_direct_present.cpp)
target_link_libraries(lift-gpu-direct-present PRIVATE emulator-lift-runtime)
add_test(NAME gpu-direct-present COMMAND lift-gpu-direct-present)
set_tests_properties(gpu-direct-present PROPERTIES SKIP_RETURN_CODE 77)

# ---- GPU 合批（入队 + 一次 flush）：pure 档（Mock 后端，不碰 SDL / GPU）----
# 判据：N 次 Draw 只 flush 1 次；Clear(TARGET) / Present / 换 target 各触发 flush + syncToGuest 1 次，
# 且 syncFromGuest 在队列非空时被拒绝（⛔ 静默覆盖）。去掉 flush ⇒ 队列不清空 ⇒ 当场红。
add_executable(lift-gpu-batch tests/gpu_batch.cpp)
target_link_libraries(lift-gpu-batch PRIVATE emulator-lift-runtime)
# ★ **必须显式依赖产品 exe**（`REQ-01M7K2J7VXKJMK1BJ3H7YCVJFJ`）：`lift/no-gpu-refuses-start`
#   （`tests/gpu_batch.cpp` 的子进程臂，见上方 `:650`）**spawn** `$<TARGET_FILE:emulator-lift-aot>`
#   去读它的退出码与 stderr；而变异自检的 `buildTarget: 'lift-gpu-batch'` 只重建**守卫** exe
#   ⇒ 落在 `src/aot/aot_main.cpp` 的那个变异会让产品 exe 停在**变异前** ⇒ 守卫跑的是旧二进制
#   ⇒ 那条变异**假绿**（`cli-aot-flags` / `aot-audio-wiring` 的同形坑）。
#   加这一行之后 `cmake --build out --target lift-gpu-batch` 会把产品 exe 一起重编重链。
#   ⛔ 删了不会红，只会让「无 GPU 时不再拒绝启动」这条变异自检**静默**变成声称。
add_dependencies(lift-gpu-batch emulator-lift-aot)
# ★ 顶层无条件注册 ⇒ pure 档（`AGENTS.md` §5：判据是注册位置）。
#   这条同时承载后缓冲 LockRect 的屏障用例（`caseBackBufferLockBarrier`）—— 只注册 `--gpu` 那条的话，
#   在无 GPU 机器上它会 SKIP 77 ⇒ "删屏障"的变异静默逃逸。
add_test(NAME gpu-batch-order COMMAND lift-gpu-batch)

# ---- 同一份夹具的真后端判据：Clear / 纹理 LockRect / SetRenderTarget 之后像素块对、队列空 ----
# dummy 视频下建不出 SDL_GPU 设备 ⇒ SKIP 77（⛔ 不拿 pure 档冒充真后端绿）。
add_test(NAME gpu-batch-barrier COMMAND lift-gpu-batch --gpu)
set_tests_properties(gpu-batch-barrier PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy" SKIP_RETURN_CODE 77)

add_executable(lift-mechanism tests/mechanism.cpp)
target_link_libraries(lift-mechanism PRIVATE emulator-lift-runtime)
add_test(NAME mechanism COMMAND lift-mechanism)

# ---- pure：`ShimRegistry` 的桩 EA 解析 —— **槽位索引 ≡ 线性扫描**（S1 性能修的等价合同）----
# 判据见 `tests/shim_stub_index.cpp` 头注释：① 每个已登记 shim 都由索引直接命中（`stubIndexSlowPathHits`
# 不动，⛔ 不是墙钟断言）；② 桩页**逐槽**（含从未发号的那些）`byStubEa`/`idForStub` 与"扫 defs_ 取
# 第一条"的定义相同；③ 页外/未对齐 ⇒ nullptr；④ addRuntime 追加后旧槽映射仍指向同一条（存 id 不存
# 指针）；⑤ 夹具直写 stub_ea 后仍等于线性扫描（慢路径兜底）。
# ★ 不需要游戏资产 / GPU ⇒ 顶层无条件注册 = pure 档。
add_executable(lift-shim-stub-index tests/shim_stub_index.cpp)
target_link_libraries(lift-shim-stub-index PRIVATE emulator-lift-runtime)
add_test(NAME lift-shim-stub-index COMMAND lift-shim-stub-index)

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

# ---- pure 的机械部分总是跑；`LIFT_AGE_EXE` **无条件**注入（语料假定在场）----
# ★ 2026-11-13：以前这条 injection 只在 `if(EXISTS …sectfix.EXE)` 里做（存在性闸）⇒ 去掉：
#   资产部分由测试自己判（`LIFT_AGE_EXE` 指向的文件缺失 ⇒ 它自己红；变量缺失 ⇒ SKIP 77）。
add_executable(lift-pe-resource tests/pe_resource.cpp)
target_link_libraries(lift-pe-resource PRIVATE emulator-lift-runtime)
add_test(NAME pe-resource COMMAND lift-pe-resource)
set_tests_properties(pe-resource PROPERTIES
    ENVIRONMENT "LIFT_AGE_EXE=${AGE_SECTFIX_EXE}" SKIP_RETURN_CODE 77)

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

add_executable(lift-log-policy tests/log_policy.cpp)
target_link_libraries(lift-log-policy PRIVATE emulator-lift-runtime)
add_test(NAME log-policy COMMAND lift-log-policy)

add_executable(lift-quartz-video tests/quartz_video.cpp)
target_link_libraries(lift-quartz-video PRIVATE emulator-lift-runtime)
add_test(NAME quartz-video COMMAND lift-quartz-video)

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

# ---- pure：**墙钟 vs uptime** 的拆分（BUG-30；判据见 tests/kernel32_time.cpp 文件头）----
# ★ 修前的形状：GetLocalTime / GetSystemTime 把宿主 uptime 当 1970 epoch 展开（存档显示 1970/1/6）。
#   这里同时钉两条：uptime 仍是单调 uptime（GetTickCount），墙钟 / 本地时间 / FILETIME 走真实 epoch；
#   时区通过 `shims/time_zone.h` 的测试缝注入（⛔ 不依赖本机 TZ、不断言当前年份）。
add_executable(lift-kernel32-time tests/kernel32_time.cpp)
target_link_libraries(lift-kernel32-time PRIVATE emulator-lift-runtime)
add_test(NAME kernel32-time COMMAND lift-kernel32-time)

# ---- pure：S6 前三族的**核心行为**（内存 / 错误码·TLS / 同步）----
# ★ 补的是"最常走的那条路"没有可再校验观察的缺口（审计单 REQ-01M6JFZ73GTZE9F6967RRSH6CE）；
#   判据见 tests/shims_core.cpp 的文件头（每条至少两个断言，含宿主可见状态）。
add_executable(lift-shims-core tests/shims_core.cpp)
target_link_libraries(lift-shims-core PRIVATE emulator-lift-runtime)
add_test(NAME shims-core COMMAND lift-shims-core)

# ---- pure：advapi32 注册表 no-op 的**参数校验**（API 合同，REQ-01M7ATN3WPB14TVZJWZXTPQW3V）----
# ★ 只锁"参数非法 ⇒ 87 / 合法路径逐字不变"；⛔ 不锁取值口径（注册表整体 no-op，SCOPE §1）。
#   依据 = 本机实测（Win11 x64 / P/Invoke），见 tests/advapi32_registry.cpp 的文件头。
add_executable(lift-advapi32-registry tests/advapi32_registry.cpp)
target_link_libraries(lift-advapi32-registry PRIVATE emulator-lift-runtime)
add_test(NAME advapi32-registry COMMAND lift-advapi32-registry)

# ---- pure：吞吐基准（S8；只测解释器分发，不设性能门槛 ⇒ 数字进 tools/lift.mjs report）----
add_executable(lift-bench tests/bench.cpp)
target_link_libraries(lift-bench PRIVATE emulator-lift-runtime)

# =====================================================================================
# 默认档：要 **LFS 语料**（执行源 / `.lst`，不在仓库里，见 `.gitignore`）的**接口合同**
# ★ 2026-11-13 用户裁决：档位按**用途**，⛔ 不按"语料在不在" —— 下面这些都是接口合同，
#   一律**顶层无条件注册**：`loader` / `native-override-assets` /
#   `decode-crosscheck` / `corpus-mnemonics`。
# ★ 缺语料时**不缩水**：这些用例自己判（`LIFT_SKIP` 77 / 红），configure 期的盘点行会把
#   MISSING 与全路径写出来。⛔ 不要再用 `if(EXISTS …)` 决定它们注册与否（那就又回到老路）。
# ★ 语料路径（`AGE_SECTFIX_EXE` / `AGE_LST`）已在测试区开头定义一次，这里只引用。
# ★ `boot-trace` 已移进 `LIFT_STRESS`（它跑真游戏，按测试模型不属于合同档）；它是**读数档**
#   （同输入两次 trace 逐字节相同 + 稳定 hash），**变异清单不覆盖它**（2026-11-14 摘条目：
#   默认机器上它恒为占位，⛔ 留一条永远不跑的变异条目只会让清单显得"覆盖了"）。
#   `base-coverage`（导入覆盖普查）已删 —— 原由 `emulator-lift --report` 承担；该入口 2026-11-16 随解释器退役 ⇒ 今天无替代。
# =====================================================================================

# ---- 默认：执行源（拆壳件）的真实 PE 载入 ----
add_executable(lift-loader tests/loader.cpp)
target_link_libraries(lift-loader PRIVATE emulator-lift-runtime)
add_test(NAME loader COMMAND lift-loader)
set_tests_properties(loader PROPERTIES
    ENVIRONMENT "LIFT_AGE_EXE=${AGE_SECTFIX_EXE};LIFT_AGE_LST=${AGE_LST}" SKIP_RETURN_CODE 77)

# ---- 2026-11-14：`base-coverage`（`tests/base_coverage.cpp`，A5 导入覆盖普查）已按测试模型删除 ----
# 判据是"跑到 L2 + 动态缺口为空"的业务结论；缺口**普查**原由 `emulator-lift --report` 承担，
# 该入口 2026-11-16 随解释器退役 ⇒ 今天无替代（⛔ 不新写工具）；复现命令见 `docs/03-emulator/observations.md`。

# ---- 默认：**真 guest 字节**的 native override 差分（解释器 vs override；缺资产 ⇒ SKIP 77）----
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

# ---- pure：对象表 / DC 状态 / SelectObject 旧值恢复 / GGO 格式算术 / 自洽（合成字形源）----
add_executable(lift-gdi tests/gdi.cpp)
target_link_libraries(lift-gdi PRIVATE emulator-lift-runtime)
add_test(NAME gdi COMMAND lift-gdi)

# ---- pure：`FontEngine::metrics` 的度量缓存 —— **已删（2026-11-13，BUG-14）** ----
# ★ 原来整条在钉"缓存键 / 失效 / hit-miss 计数"（`tests/gdi_font_metrics_cache.cpp`），那是**内部实现
#   取舍**（键 = 字号 / bold / 字体路径；见 `docs/03-emulator/design-notes.md` 的 L3-gdi-01），
#   换一份实现就不需要成立 ⇒ 按 BUG-14 的裁决删除（含 `tools/mutate-check.mjs` 的对应变异条目）。
# ★ 它的**真 GDI 合同**（换字号后字度量必须变）已搬进上面的 `gdi-font`：用例 `metrics-size-change`。

# ---- pure：缺字形 fallback（`REQ-01M79N1ZV391BCM7MYSKK0GVV0`；用假源，不读字体文件）----
# 判据见 tests/gdi_font_fallback.cpp 文件头：①`gdiCp932Char(0xF050) == 0xE010`（⛔ 不是 U+2661）；
# ②主 face 缺字 ⇒ 交给后备提供者、返回**后备的 mask**；③缓存命中不重复记账；④主 face 有字时
# 不碰后备；⑤都缺 ⇒ 返回 true + `.notdef` 且 notdef 计数 +1；⑥重装后备来源 ⇒ 旧条目作废。
# ★ 顶层无条件注册 ⇒ pure 档（判据 = 注册位置；见 AGENTS.md §5）。
# ★ 同一个 exe 的 `--assets` 臂在下面的**默认档**块里另注册一条（`LZSS` 解压复用生产实现
#   `lift::lzssDecode`；链接 `Freetype::Freetype` 是为了直接问 FT 的断言 —— runtime 是 PRIVATE 链接）。
add_executable(lift-gdi-font-fallback tests/gdi_font_fallback.cpp)
target_link_libraries(lift-gdi-font-fallback PRIVATE emulator-lift-runtime Freetype::Freetype)
add_test(NAME gdi-font-fallback COMMAND lift-gdi-font-fallback)

# ---- 默认档：真 Amayui CN（**宿主字体假定在场**；缺 ⇒ 测试自己 SKIP 77，⛔ 不缩水）----
# ★ 2026-11-13 用户裁决：档位按**用途**，⛔ 不按"资产在不在" —— `gdi-font` 是**接口合同**
#   （真 GDI 度量）⇒ **顶层无条件注册**，⛔ 不要再包 `if(EXISTS …)`
#   （那会让"这条用例存不存在"随机器漂、并让 README 变成假话）。
# ★ 缺字体时由测试体自己判（`fontFileAvailable()` 失败 ⇒ `LIFT_SKIP` 77，见各自文件头）。
# ★ `gdi-font` 走的是**生产** `FontEngine::metrics` 缓存（换字号 / 换 bold 都必须给出不同度量）——
#   它是这条真 GDI 合同的守卫，`tools/mutate-check.mjs` 的 `ctest:gdi-font` 指向它。
# ★ `text-baseline-lift`（宿主基线补偿 = 我方视觉策略 `baseline_lift==2`）**已按测试模型删除**
#   （2026-11-14；⛔ 业务/策略正确性不作判据）⇒ 口径留在 `docs/03-emulator/fonts.md`，
#   复现命令见 `docs/03-emulator/defects.md` BUG-08。
add_executable(lift-gdi-font tests/gdi_font.cpp)
target_link_libraries(lift-gdi-font PRIVATE emulator-lift-runtime)
add_test(NAME gdi-font COMMAND lift-gdi-font)
set_tests_properties(gdi-font PROPERTIES SKIP_RETURN_CODE 77)

# ---- pure：D3D9 设备簇（自造 guest 代码经 vtable 调；不碰游戏 / LFS）----
add_executable(lift-d3d9 tests/d3d9.cpp)
target_link_libraries(lift-d3d9 PRIVATE emulator-lift-runtime)
add_test(NAME d3d9 COMMAND lift-d3d9)

# ---- 真 GPU 臂：点/线与 caps 的**像素见证**（CPU 光栅已删 ⇒ 必须装隐藏窗口的 SDL_GPU 后端）----
# ★ 顶层无条件注册 ⇒ pure 档（判据是**注册位置**，见 `AGENTS.md` §5）；没有 GPU 设备 ⇒ SKIP 77
#   （`--gpu` 臂里让开 CTest 给的 `SDL_VIDEODRIVER=dummy`：dummy 视频下建不出 SDL_GPU 设备）。
add_test(NAME d3d9-gpu COMMAND lift-d3d9 --gpu)
set_tests_properties(d3d9-gpu PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy" SKIP_RETURN_CODE 77)

# ---- pure：D3DX9 Sprite（对象 / 状态 / 批次协议 / 纹理上传 / trap；**不判像素**）----
# ★ 像素断言在下面的 `d3dx9-gpu` 真 GPU 臂（`REQ-01M7AAXNJY2PACYJHYVRV620Q5`：sprite 的 CPU
#   逐像素合成已删 ⇒ 缺省臂里 sprite 是"记账 + 跳过"，一个像素都不写）。
add_executable(lift-d3dx9 tests/d3dx9.cpp)
target_link_libraries(lift-d3dx9 PRIVATE emulator-lift-runtime)
add_test(NAME d3dx9 COMMAND lift-d3dx9)

# ---- 真 GPU 臂：sprite 的**像素断言**（隐藏窗口 + SDL_GPU；无 GPU 设备 ⇒ SKIP 77）----
# ★ 顶层无条件注册 ⇒ pure 档（判据 = 注册位置）；⛔ 不把"没跑"记成"跑绿了"。
add_test(NAME d3dx9-gpu COMMAND lift-d3dx9 --gpu)
set_tests_properties(d3dx9-gpu PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy" SKIP_RETURN_CODE 77)

# ---- pure：fixed-function 流水线（状态机 / 变换 / 绑定生命周期 / 拒绝类合同）----
add_executable(lift-d3d9-pipeline tests/d3d9_pipeline.cpp)
target_link_libraries(lift-d3d9-pipeline PRIVATE emulator-lift-runtime)
add_test(NAME d3d9-pipeline COMMAND lift-d3d9-pipeline)

# ---- 真 GPU 臂：保留的**像素断言**（纯色三角形 / 纹理采样 / 深度 / 混合 / 裁剪）----
# ★ 同上：顶层无条件注册 ⇒ pure 档；没有 GPU 设备 ⇒ SKIP 77（⛔ 不假绿）。
add_test(NAME d3d9-pipeline-gpu COMMAND lift-d3d9-pipeline --gpu)
set_tests_properties(d3d9-pipeline-gpu PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy" SKIP_RETURN_CODE 77)

# ---- pure：数学适配（行/列主序乘法顺序、LH 投影/视图、零向量归一化）----
add_executable(lift-math tests/math.cpp)
target_link_libraries(lift-math PRIVATE emulator-lift-runtime)
add_test(NAME math COMMAND lift-math)

# =====================================================================================
# pure：SoftFloat 的 x87 异常位必须**每线程一份**（`REQ-01M76ZHN9V2WNCYT1ACJDJ94CW`）
# ★ 判据（红了意味着什么，见 tests/fpu_thread.cpp 文件头）：两条宿主线程各 20000 轮，用
#   `std::barrier`（C++20）**逐轮强制交错** —— A 触发 inexact、B 触发 invalid，各自
#   `x87TakeExceptionFlags()` 取到的标志里**只能有自己的那一位**（既不被对方偷走、也不串进来）；
#   另有单线程 sanity 钉住"两条臂各自真的置位"。
# ★ 变异：把 `third_party/berkeley-softfloat/platform.h` 的 `THREAD_LOCAL` 改回空（进程级共享）
#   ⇒ barrier ② 之后两家的位同时在场 ⇒ `tools/mutate-check.mjs` 的 `ctest:fpu-thread-flags`
#   **每次必红**（⛔ 不是"堆 2 万轮碰运气"：那有 ~10% 假绿）。
# ★ 顶层无条件注册 ⇒ **pure** 档（只调 `runtime/fpu_bridge.h`；不碰 guest / 游戏 / LFS）。
#   链接 `berkeley-softfloat` 只为拿它的 include 目录与 PUBLIC 编译定义
#   （`softfloat_flag_inexact` / `softfloat_flag_invalid` 常量在 `softfloat.h` 里）。
# =====================================================================================
add_executable(lift-fpu-thread tests/fpu_thread.cpp)
target_link_libraries(lift-fpu-thread PRIVATE emulator-lift-runtime berkeley-softfloat)
add_test(NAME fpu-thread-flags COMMAND lift-fpu-thread)

# =====================================================================================
# pure：消费方 TU 必须拿到**真 TLS** 的 SoftFloat 状态（`REQ-01M773EFSYYE97YKEA6TQEQ53J`）
# ★ 判据（见 tests/softfloat_tls.cpp 文件头）：本 TU 的**第一条 include** 就是 `<softfloat.h>`、
#   ⛔ 不 include platform.h —— 正是要守的那条路径（"下一个 include 就会中"）。三条断言：
#   ① 任何 include 之前就看得见 `THREAD_LOCAL` / `LITTLEENDIAN`（= 前置 include 真的先跑了）；
#   ② 库的 `softfloat_raiseFlags()` 置的位在**本 TU** 里读得到（= 同一个 TLS 对象）；
#   ③ 两条线程各写不同值、barrier 之后各读各的，且 `&softfloat_exceptionFlags` 不同
#      （= 每线程一份；共享态下"最后写入者赢"⇒ 至少一条断言必红）。
# ★ 它守的是 `third_party/berkeley-softfloat/CMakeLists.txt` 里那条
#   `target_compile_options(berkeley-softfloat INTERFACE /FIplatform.h)`（非 MSVC：`-include platform.h`）
#   ⇒ 把 `INTERFACE` 改成 `PRIVATE`（消费者拿不到）⇒ 本用例红
#   （`tools/mutate-check.mjs` 的 `ctest:softfloat-tls` 施加这一处）。
# ★ 顶层无条件注册 ⇒ **pure** 档（只调 SoftFloat 公开面；不碰 guest / 游戏 / LFS）。
#   链接 `berkeley-softfloat` 既拿 include 目录 / PUBLIC 定义，也拿那条 **INTERFACE** 编译选项
#   （= 本用例的被测对象本身）。
# =====================================================================================
add_executable(lift-softfloat-tls tests/softfloat_tls.cpp)
target_link_libraries(lift-softfloat-tls PRIVATE emulator-lift-runtime berkeley-softfloat)
add_test(NAME softfloat-tls COMMAND lift-softfloat-tls)

# ---- pure：像素格式（掩码往返、16/32bpp load/store、颜色打包、min_filter 决策）----
add_executable(lift-pixel-format tests/pixel_format.cpp)
target_link_libraries(lift-pixel-format PRIVATE emulator-lift-runtime)
add_test(NAME pixel-format COMMAND lift-pixel-format)

# =====================================================================================
# 可定界的区间 profiler（`profile_begin` / `profile_end`；pure：裸 Machine + 自造字节 + 一个已知 shim）
# ★ 判据见 tests/profile_window.cpp 文件头：① 分项 + unattributed == 窗口墙钟；
#   ② 窗口外的 shim 调用不计入（窗口内/外各烧一次，量级差 10×）；③ 未闭合 ⇒ 自动收口 + 告警；
#   ④ `profile-instrumentation-invariant`：四臂（全关 / 仅 --profile / 仅窗口 / 两者）的 insns
#      必须全等 ⇒ **仪器不许改执行流**（口径不变量；⛔ 不断言耗时）。
# ★ 顶层无条件注册 ⇒ pure 档。
# =====================================================================================
add_executable(lift-profile-window tests/profile_window.cpp)
target_link_libraries(lift-profile-window PRIVATE emulator-lift-runtime)
add_test(NAME profile-window COMMAND lift-profile-window)

# =====================================================================================
# **AOT 窗口 profile**（`profile_begin` / `profile_end` 之间的分桶账；pure：纯算术夹具）
# ★ 判据见 tests/aot_window_profile.cpp 文件头：① 闭式 `wall = lift + boundary + shim +
#   partial + instrument + unaccounted`；② 采样只决定 `lift:boundary` 的**比值**，绝对量取自
#   精确包络（1/64 采样直接外推会把取时税放大 64×，实测 1.77 s vs 墙钟 1.62 s）；
#   ③ **作废看 `coverage`**（采样外推 / 包络，`∉ [0.5, 2]` ⇒ `suspect`）—— `unaccounted` 是
#   定义式残差、归一后恒 ~0，它照打但**不是**作废判据（3% 那道闸门仍在 `valid()` 里）；
#   ④ 没开窗 ⇒ 全 0、三档阈值只有一份。
# ★ 与上面那条 `profile-window` **不是一回事**：那是解释器 `WindowProfiler` 的分项（块缓存 /
#   迭代器 / 派发循环），AOT 一个都没有 ⇒ 这里重新定义桶（`interp → boundary`、无 `native`）。
# ★ 顶层无条件注册 ⇒ pure 档（判据 = 注册位置，AGENTS.md §12.4）。
# =====================================================================================
add_executable(lift-aot-window-profile tests/aot_window_profile.cpp)
target_link_libraries(lift-aot-window-profile PRIVATE emulator-lift-runtime)
add_test(NAME aot-window-profile COMMAND lift-aot-window-profile)

# =====================================================================================
# **AOT 窗口报告文本 + 状态位**（`formatWindowReportText` / `AotWindowProf::beginWindow` 的 pure 判据）
# ★ 判据见 tests/window_prof.cpp 文件头：① 文本与**旧 `endWindow()` printf 块**的金样逐字节相同
#   （= "抽函数不改 stdout"的机械证据）；② 段落闭合（wall/lift/.../verdict 每段都在）；
#   ③ Top-8 + `(rest)`；④ `sampled==0` / `coverage` 越界 ⇒ 文本含 `INVALID`；⑤ 总行数 ≤ 20；
#   ⑥ 重复 `beginWindow()` ⇒ false 且**不 reset**（⛔ 不静默重开）。
# ★ 顶层无条件注册 ⇒ pure 档（判据 = 注册位置，AGENTS.md §12.4）；不碰 GPU / 资产 / 真进程。
# =====================================================================================
add_executable(lift-aot-window-report tests/window_prof.cpp)
target_link_libraries(lift-aot-window-report PRIVATE emulator-lift-runtime)
add_test(NAME aot-window-report-text COMMAND lift-aot-window-report)
add_test(NAME aot-window-state COMMAND lift-aot-window-report state)

# =====================================================================================
# **AOT 按帧 shim 分桶**（`--profile-slow-frames <ms>`；pure：纯算术夹具 + 名字表，不取时）
# ★ 判据见 tests/aot_frame_bucket.cpp 文件头：① 名字合并（同名 id 相加）+ 有界 top-N；
#   ② 阈值门（**只**存 ≥ 阈值的帧，且每帧的账不跨帧漏）；
#   ③ 上界（慢帧 ≤ 32、每帧名字 ≤ 8；超出只计数 ⇒ `frames_dropped`，⛔ 不静默）；
#   ④ 关档（阈值 0 / 没开）⇒ 一次不存、`on == false`。
# ★ 与 `aot-window-profile` **不是一回事**：那是窗口桶 + 闭式；这里是**单帧**的 shim 具名分桶
#   （wipe 剖面第二段卡的仪器缺口，见 docs/03-emulator/wipe-profile.md）。
# ★ 顶层无条件注册 ⇒ pure 档（判据 = 注册位置，AGENTS.md §12.4）。
# =====================================================================================
add_executable(lift-aot-frame-bucket tests/aot_frame_bucket.cpp)
target_link_libraries(lift-aot-frame-bucket PRIVATE emulator-lift-runtime)
add_test(NAME aot-frame-bucket COMMAND lift-aot-frame-bucket)

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

# ---- pure：协议核心（假引擎 filter + 假帧源 + 假时钟；不碰游戏 / LFS）----
# 判据见 tests/dshow_graph.cpp 文件头：D1 接线（含 QI/NotifyAllocator）、媒体类型逐字段、
# 控制顺序（先 Pause 后 Run；渲染器保持 Paused）、泵的 33/66 ms 与自底向上、分配器忙不阻塞、
# 以及「会红」自检（坏接线 + 逐字段改坏都必红）。
add_executable(lift-dshow-graph tests/dshow_graph.cpp)
target_link_libraries(lift-dshow-graph PRIVATE emulator-lift-runtime)
add_test(NAME dshow-graph COMMAND lift-dshow-graph)

# =====================================================================================
# 并发契约：**单令牌**（`src/runtime/guest_token.h`）
# ★ 判据（红了意味着什么，见 tests/guest_token.cpp 文件头）：两条宿主线程各 200000 次
#   guest 堆 alloc/free（真实 `GuestHeap` + 真实令牌）⇒ 不崩、`in_use == 0`、
#   `violations == 0`、`max-concurrent == 1`。
# ★ 把 `GuestToken::acquire/release` 短路成空操作 ⇒ 违规计数 / 并发持有者数当场红
#   （2026-11-08 实测，见任务记录的 mutation）。
# ★ 顶层无条件注册 ⇒ **pure** 档（不碰游戏 / LFS / SDL）。
# =====================================================================================
add_executable(lift-guest-token tests/guest_token.cpp)
target_link_libraries(lift-guest-token PRIVATE emulator-lift-runtime)
add_test(NAME guest-token-stress COMMAND lift-guest-token)

# =====================================================================================
# `WindowManager` 的**主线程亲和性**（`REQ-01M76SXW0Y3R8QX1F94AB85M9H` 的 2026-11-12 裁决）
# ★ 这不是令牌：wm 按 M0 的口径是**主线程独占宿主对象**（⛔ 不进令牌容器清单）——
#   块边界的 harness tick 与 `GetMessageA` 的宿主事件泵都在合法让出令牌的窗口里写它。
# ★ 判据（红了意味着什么，见 tests/wm_token.cpp 文件头）：① 主线程正常用 ⇒ `offThread()==0`；
#   ② worker 线程调 `find` ⇒ `offThread()>=1` 且 first site 含 `window.find`（且返回值不变）；
#   ③ reset 之后两个不同入口 ⇒ `>=2`（站点粒度真的是 `window.<op>`）；④ 没有 owner 的实例
#   ⇒ 不计数（不制造假红）。
# ★ 顶层无条件注册 ⇒ **pure** 档（裸 `WindowManager` + `std::thread`；不碰游戏 / LFS / guest 内存）。
# ★ `tools/mutate-check.mjs` 的 `ctest:window-manager-token` 指向它（删掉 `find` 入口的
#   `checkMainThread("window.find")` ⇒ ② 当场红）。
# =====================================================================================
add_executable(lift-wm-token tests/wm_token.cpp)
target_link_libraries(lift-wm-token PRIVATE emulator-lift-runtime)
add_test(NAME window-manager-token COMMAND lift-wm-token)

# =====================================================================================
# 吞吐的**派发层固定税**（`REQ-01M70S2WR4S8MN2JDWJRJD6WFD`）+ 诊断开关守卫
# ★ 判据见 `tests/dispatch_tax.cpp` 文件头：
#   ① 微基准的**量级不变量**（未竞争 mutex lock+unlock / steady_clock::now；⛔ 不断言具体数字）；
#   ② 块长扫描 k=1/3/8 条/块（自造字节 ⇒ 不改 `tests/bench.cpp` 的合成体）拟合 `t=b+c·k`；
#   ⑥ 诊断臂里 `check()` 的违规计数照旧 ⇒ `--assert-guest-token` 的可信度不变；
#   ⑦ 运行摘要 `present-tail` 行的**打印门** + 逐字格式；
#   ⑧ `stress/skip-report`：**S9 的 skip 报表**（纯函数 `tests/stress_skip_report.h`）三组合成文本
#      负控 —— `skipped=0` ⇒ 通过 · `skipped=3` + 三行 `skipped top:` ⇒ 失败且**逐条原样**含那三行 ·
#      `skipped=1` 但原因表缺失 ⇒ 失败且消息说"原因缺失"。这就是"红的时候真的会列原因"的机械证据。
# ★ ③④⑤（`LIFT_TOKEN_DIAG_SKIP_LOCK` 的默认关 / 打开 / "真的绕过 mutex"）**已删**：判据面是
#   `token-diag : skip-lock=` 摘要文案 + `maxConcurrent()`，属我方诊断面（`AGENTS.md` §10）。
# ★ **档位 = `LIFT_STRESS`（效率读数）**：①/② 是计时量级带（`kDeltaBTolNs` / `kLockCostFloorRatio`），
#   终裁 3 判定"计时的量级带是效率读数、不是对外合同"⇒ `add_executable` / `add_test` 整块移入
#   `if(DEFINED ENV{LIFT_STRESS})`，默认档**不留**"也跑一遍"的壳。读数（`b=… c=… lock+unlock=…`）
#   进 `docs/03-emulator/observations.md`（OBS-20261114-01）。⚠ 需求单 `REQ-01M70S2WR4S8MN2JDWJRJD6WFD`
#   的 verify 锚点仍是 `dispatch_tax.cpp#dispatch-tax`（字面量在），但它现在只在开档时跑。
# ★ ⑧ 也随之只在 `LIFT_STRESS=1` 时跑（它断言的是解析 + 报错格式，与真游戏无关，但同在一个 exe 里）。
# ★ 顶层注册时曾是 **pure** 档（裸 `Machine` + 自造字节；不碰游戏 / LFS / GPU / 音频设备），移档后
#   仍是"不需要游戏安装"——移它的理由是**用途**（效率读数），⛔ 不是"资产在不在"。
# =====================================================================================

# =====================================================================================
# **读数**：五类 guest 指令形状的「每条指令墙钟（ns/条）」（不是合同、是测量）
# ★ 判据见 `tests/insn_cost.cpp` 的文件头：只断言**测量前提**（每遍 `cpu.insns` 实测 == 请求的
#   12,000,000 条、停因 `MaxInsns`、每轮条数整除 N），⛔ **不断言任何 ns 数值**。
#   用途：回答「真实负载 ~25 ns/条 里派发 / 访存 / 调用 / x87 / shim 各占多少」——所以它必须
#   每次门禁都跑（读数变了要看得见），并且读数进 `docs/03-emulator/observations.md`，不进断言。
# ★ 顶层无条件注册 ⇒ **pure** 档（`AGENTS.md` §5：判据 = 注册位置）：裸 `Machine` + `placeCode`
#   自造字节 + `std::chrono`（可移植；Windows x64 与 macOS arm64 同一份代码，无 arch 专属指令）。
#   不碰游戏 / LFS / GPU / 音频设备；实测 ~3 s（7 形状 × 3 遍 × 12M 条）。
# ★ 为什么不用 `LIFT_STRESS` 门控：压/效率档的判据是"读数 + 阈值"；这里没有阈值，若藏进 opt-in，
#   "没跑"与"跑过了"在 ctest 输出里就长得一样（那正是分级要消灭的东西）。
# =====================================================================================
add_executable(lift-insn-cost tests/insn_cost.cpp)
target_link_libraries(lift-insn-cost PRIVATE emulator-lift-runtime)
add_test(NAME insn-cost COMMAND lift-insn-cost)

# =====================================================================================
# M2 · 派生 mip 链（`REQ-01M73G2724479XAVS176WBZ65Q`：缩小采样必须有 mip，否则走样）
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
# ★ 判据见 tests/d3d9_mip.cpp 文件头：① 2×2→1×1 的 box filter 逐通道；② 奇数尺寸按覆盖范围；
#   ③ 链的级数/尺寸/行距/偏移/总字节；④ LOD 在 1:1 / 2:1 / 4:1 的级号（★ 1:1 必须 level 0）；
#   ⑤ 屏幕空间 UV 导数必须是**透视校正**的（w≠1 的三角形上与手算值一致、且不等于仿射梯度）。
# ★ 顶层无条件注册 ⇒ **pure** 档。`tools/mutate-check.mjs` 的 `ctest:d3d9-mip` 指向它
#   （把 LOD 恒取 0 ⇒ 第 ④ 条当场红）。
# =====================================================================================
add_executable(lift-d3d9-mip tests/d3d9_mip.cpp)
target_link_libraries(lift-d3d9-mip PRIVATE emulator-lift-runtime)
add_test(NAME d3d9-mip COMMAND lift-d3d9-mip)

# =====================================================================================
# M2 · `d3dx9_43!D3DXLoadSurfaceFromSurface`（2D surface 拷贝；SC0000 首句的门）
# ★ 判据见 tests/d3dx9_load_surface.cpp 文件头：① 半开矩形 / 目标偏移 / NULL = 整幅；
#   ② 越界 / 负坐标 / 空矩形 / rect 指针不可读 ⇒ D3DERR_INVALIDCALL + 目标一字节不变（⛔ 不隐式裁剪）；
#   ③ 源/目标矩形尺寸不等 ⇒ 响亮停（只做 1:1）；④ colorKey != 0 ⇒ 响亮停；
#   ⑤ 格式：同 D3DFORMAT / 同序 32bpp ⇒ 恒等；RGBA↔BGRA ⇒ 只换 R/B；其它 ⇒ 拒绝。
# ★ 顶层无条件注册 ⇒ **pure** 档（自造 guest 字节 + 运行时建的真 surface，不碰游戏 / LFS）。
#   ⛔ 它曾是 `register_import_gaps.cpp` 里「M3 · 3D 不做」的一条；按微软文档是 2D blit ⇒ 真实现。
# =====================================================================================
add_executable(lift-d3dx9-load-surface tests/d3dx9_load_surface.cpp)
target_link_libraries(lift-d3dx9-load-surface PRIVATE emulator-lift-runtime)
add_test(NAME d3dx9-load-surface COMMAND lift-d3dx9-load-surface)

# ---- pure：帧导出**每帧失败**不拖住 Present / 计数 / warn 只一次 / 成功路径照旧 ----
# ★ 判据见 tests/frame_dump_failure.cpp 头注释。裸 GuestMemory + SurfaceArena（⛔ 不碰游戏）。
# ★ 顶层无条件注册 ⇒ pure 档。
add_executable(lift-frame-dump-failure tests/frame_dump_failure.cpp)
target_link_libraries(lift-frame-dump-failure PRIVATE emulator-lift-runtime)
add_test(NAME frame-dump-failure COMMAND lift-frame-dump-failure)
set_tests_properties(frame-dump-failure PROPERTIES SKIP_RETURN_CODE 77)

# =====================================================================================
# 2026-11-14：`interaction-script-e2e`（`tests/interaction_script_e2e.cpp`）已按测试模型删除
# （`REQ-01M7ATAB5A18W7XBCKFBEQS7HB`）—— 判据是"等一屏 → 点一下"这类**业务正确性**；
# ⛔ 不进测试。复现命令见 `docs/03-emulator/defects.md` BUG-16。
# =====================================================================================

# =====================================================================================
# Shape A 原型阶段 A（`.tmp/irshape/design-review.md` §6 的最小版）：
#   ① `lift-abi-probe`：**只钉 ABI**（sizeof / offsetof / 逐字段搬运 / lift_note_write → SMC），
#      ⛔ 不碰 clang / `.ll` ⇒ 没有 clang 的机器也照样绿（顶层无条件注册 = pure）。
#   ② `lift-ir-proto`：`tests/ir/lift_436D00.ll` 由 clang 编成 obj，与 runtime **链进同一个 exe**；
#      trampoline 按 resume EA 调它，冷路径把两个建表器**交给解释器**（混合模式），
#      再与解释器臂逐位差分；`--lifted-only` 子进程是"解释器不在执行路径上"的负控（exit 9）。
# ★ **缺 clang 的处置**（复审 §3.4）：`add_custom_command` 只放在 `if(LIFT_IR_CLANG)` 里，
#   否则改用 `tests/ir/lift_stub.cpp`（同名符号返回哨兵）⇒ configure / 链接**永不断**；
#   测试体自己 `LIFT_SKIP(77)`。⛔ `add_test` 在**最外层**（不嵌进任何 `if`）—— 档位 = pure。
# ★ 编译用 `clang` 驱动（不是 clang-cl）：`.ll` 输入只有 clang 认；obj 与 clang-cl 编的 TU
#   是同一份 LLVM / 同一套 COFF ABI，链接实测可行（`runtime-contract.md` §6.3）。
# ★ `-Wno-override-module`：clang 22 对"IR 没写 target triple、命令行给了 --target"会 warn；
#   这个 IR 是**目标无关**的（只做 i32 运算），triple 交给驱动。
# =====================================================================================
add_executable(lift-abi-probe tests/lift_abi_probe.cpp)
target_link_libraries(lift-abi-probe PRIVATE emulator-lift-runtime)
add_test(NAME lift-abi-probe COMMAND lift-abi-probe)

find_program(LIFT_IR_CLANG NAMES clang clang.exe)
if(LIFT_IR_CLANG)
    set(LIFT_IR_OBJ "${CMAKE_CURRENT_BINARY_DIR}/lift_436D00.obj")
    set(LIFT_IR_TARGET_FLAGS "")
    if(WIN32)
        # 与预设 `windows-x64` 的 clang-cl 一致
        set(LIFT_IR_TARGET_FLAGS --target=x86_64-pc-windows-msvc)
    endif()
    add_custom_command(OUTPUT "${LIFT_IR_OBJ}"
        COMMAND ${LIFT_IR_CLANG} -c -O2 ${LIFT_IR_TARGET_FLAGS} -Wno-override-module
                -o "${LIFT_IR_OBJ}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/lift_436D00.ll"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/lift_436D00.ll"
        COMMENT "lift-ir-proto: tests/ir/lift_436D00.ll -> .obj (clang -c -O2; no llc)")
    set(LIFT_IR_SRC "${LIFT_IR_OBJ}")
else()
    message(STATUS "lift-ir-proto: clang not found => link tests/ir/lift_stub.cpp (test body SKIPs 77)")
    set(LIFT_IR_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/lift_stub.cpp")
endif()
add_executable(lift-ir-proto tests/lift_ir_proto.cpp ${LIFT_IR_SRC})
target_link_libraries(lift-ir-proto PRIVATE emulator-lift-runtime)
if(LIFT_IR_CLANG)
    target_compile_definitions(lift-ir-proto PRIVATE LIFT_IR_CLANG=1)
endif()
target_compile_definitions(lift-ir-proto PRIVATE LIFT_IR_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
# `getenv`（读 PROCESSOR_IDENTIFIER 打 CPU 读数）在 MSVC 口径里是 deprecated 警告，不是问题
target_compile_definitions(lift-ir-proto PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME lift-ir-proto COMMAND lift-ir-proto $<TARGET_FILE:lift-ir-proto>)
set_tests_properties(lift-ir-proto PROPERTIES SKIP_RETURN_CODE 77)

# =====================================================================================
# Shape A 原型阶段 B（`.tmp/irshape/impl-stage-b.md`）：**shim 的利用方式**
#   `lift-ir-shim`：`tests/ir/lift_4D4B40.ll` + `tests/ir/lift_436D00.ll` 两份 obj 与 runtime
#   链进**同一个** exe。载荷 `sub_4D4B40` 里 ③ 是 `call eax`（槽值 = 一条 lifted EA 0x436D00）、
#   ④ 是 `call ds:[0051D450]`（真 IAT 槽 → shim 桩）⇒ 两条分派路都被真 guest 代码走到。
#   探针把 `timeGetTime` 换成固定返回（⛔ 不用真虚拟时钟：那会让"比 [E+0xFC]"退化成比记账）。
# ★ 缺 clang 的处置与阶段 A 同一套（复审 §3.4）：`add_custom_command` 只放在 `if(LIFT_IR_CLANG)`
#   里；否则链 `tests/ir/lift_stub.cpp`（提供 436D00 替身），4D4B40 的替身由测试体内联提供
#   （`#ifndef LIFT_IR_CLANG`）⇒ configure / 链接**永不断**，测试体自己 `LIFT_SKIP(77)`。
# ⛔ `add_test` 在**最外层**（不嵌进任何 `if`）—— 档位 = pure。
# =====================================================================================
if(LIFT_IR_CLANG)
    set(LIFT_IR_SHIM_OBJ "${CMAKE_CURRENT_BINARY_DIR}/lift_4D4B40.obj")
    add_custom_command(OUTPUT "${LIFT_IR_SHIM_OBJ}"
        COMMAND ${LIFT_IR_CLANG} -c -O2 ${LIFT_IR_TARGET_FLAGS} -Wno-override-module
                -o "${LIFT_IR_SHIM_OBJ}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/lift_4D4B40.ll"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/lift_4D4B40.ll"
        COMMENT "lift-ir-shim: tests/ir/lift_4D4B40.ll -> .obj (clang -c -O2; no llc)")
    set(LIFT_IR_SHIM_SRC "${LIFT_IR_OBJ}" "${LIFT_IR_SHIM_OBJ}")
else()
    message(STATUS "lift-ir-shim: clang not found => link tests/ir/lift_stub.cpp + the in-test 4D4B40 stand-in (SKIP 77)")
    set(LIFT_IR_SHIM_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/lift_stub.cpp")
endif()
add_executable(lift-ir-shim tests/lift_ir_shim.cpp ${LIFT_IR_SHIM_SRC})
target_link_libraries(lift-ir-shim PRIVATE emulator-lift-runtime)
if(LIFT_IR_CLANG)
    target_compile_definitions(lift-ir-shim PRIVATE LIFT_IR_CLANG=1)
endif()
target_compile_definitions(lift-ir-shim PRIVATE LIFT_IR_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
target_compile_definitions(lift-ir-shim PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME lift-ir-shim COMMAND lift-ir-shim)
set_tests_properties(lift-ir-shim PROPERTIES SKIP_RETURN_CODE 77)

# =====================================================================================
# Shape A / T1a：`emulator-lift-gen` 生成物的**语义等价**验证（只插入新 target / 新 add_test）
# ★ 判据分两层（父级要求分开报）：
#   ① 逐字节 vs `tests/ir/*.ll` —— 生成器是"内存形态"（状态留在 `%st`），手写 golden 是 SSA 形态
#      ⇒ 预期**不同**；不为了让 diff 消失去改 golden。
#   ② 语义等价 —— 用**同一份测试体**（`tests/lift_ir_proto.cpp` / `tests/lift_ir_shim.cpp`），
#      只把链接的 `.ll` 从手写 golden 换成生成物 ⇒ 既有 CTest 的逐位差分判据逐条不变。
# ★ 缺 clang / 缺执行源 ⇒ 链 `tests/ir/lift_stub.cpp`（与上面 `lift-ir-*` 同一套处置），
#   测试体自己 `LIFT_SKIP(77)`；⛔ `add_test` 仍在**最外层**（不嵌进任何 if）⇒ 档位 = pure。
# =====================================================================================
set(LIFT_GEN_ROOTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/roots_golden.txt")
set(LIFT_GEN_AGE_EXE "${CMAKE_CURRENT_SOURCE_DIR}/../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE")
set(LIFT_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/aot")
if(LIFT_IR_CLANG AND EXISTS "${LIFT_GEN_AGE_EXE}")
    # ★ 夹具口径：这两个用例只测 `tests/ir/roots_golden.txt` 里那两个手写根 ⇒ 软根全关，
    #   回到"只按 --roots 展开"的旧口径。否则第三类（`.data` 指针表，PE 全局、不受 `--roots`
    #   约束）会把 golden_gen.ll 从 125 KB 涨到 ~162 MB、每次构建多编 ~2 分钟（G3 §11 的读数）；
    #   第五类（D26，可执行节的 dword）同理且更甚：它不受 `--roots` 约束，开着会把整个 `.text`
    #   的候选面都收成软根。⇒ 四个 `--no-close-*` 一个不能少。
    add_custom_command(OUTPUT "${LIFT_GEN_DIR}/golden_gen.ll"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LIFT_GEN_DIR}"
        COMMAND $<TARGET_FILE:emulator-lift-gen>
                --exe "${LIFT_GEN_AGE_EXE}" --roots "${LIFT_GEN_ROOTS}"
                --out "${LIFT_GEN_DIR}/golden_gen.ll"
                --no-close-calls --no-close-stores --no-close-data --no-close-jmptab --no-s7
        DEPENDS emulator-lift-gen "${LIFT_GEN_ROOTS}" "${LIFT_GEN_AGE_EXE}"
        COMMENT "lift-gen: roots + PE -> out/aot/golden_gen.ll (T1a; not committed)"
        VERBATIM)
    add_custom_command(OUTPUT "${LIFT_GEN_DIR}/golden_gen.obj"
        COMMAND ${LIFT_IR_CLANG} -c -O2 ${LIFT_IR_TARGET_FLAGS} -Wno-override-module
                -o "${LIFT_GEN_DIR}/golden_gen.obj" "${LIFT_GEN_DIR}/golden_gen.ll"
        DEPENDS "${LIFT_GEN_DIR}/golden_gen.ll"
        COMMENT "lift-gen: golden_gen.ll -> .obj (clang -c -O2; no llc)")
    set(LIFT_GEN_SRC "${LIFT_GEN_DIR}/golden_gen.obj")
    # ★ T1c：生成物在块边界发 `@lift_tick`（D2）⇒ lifted 臂与解释器**同一口径**记账；
    #   手写 golden（`tests/ir/lift_*.ll`）没有 tick ⇒ 两份测试体用这个宏区分"期望的 lifted 侧条数"。
    set(LIFT_GEN_DEFINE LIFT_IR_CLANG=1 LIFT_GEN_TICKS=1)
else()
    message(STATUS
        "lift-gen goldens: missing clang or execution source (${LIFT_GEN_AGE_EXE}) => link tests/ir/lift_stub.cpp (test SKIPs 77)")
    set(LIFT_GEN_SRC "${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/lift_stub.cpp")
    set(LIFT_GEN_DEFINE "")
endif()
add_executable(lift-ir-gen-proto tests/lift_ir_proto.cpp ${LIFT_GEN_SRC})
target_link_libraries(lift-ir-gen-proto PRIVATE emulator-lift-runtime)
target_compile_definitions(lift-ir-gen-proto PRIVATE ${LIFT_GEN_DEFINE})
# ★ 同一份测试体（`tests/lift_ir_proto.cpp`）在两条 target 下报**各自的**用例名：
#   这条定义让 `finish()` 打出 `lift-ir-gen-proto`（缺省 = `lift-ir-proto`）。
target_compile_definitions(lift-ir-gen-proto PRIVATE LIFT_IR_CASE_GEN=1)
target_compile_definitions(lift-ir-gen-proto PRIVATE LIFT_IR_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
target_compile_definitions(lift-ir-gen-proto PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME lift-ir-gen-proto COMMAND lift-ir-gen-proto $<TARGET_FILE:lift-ir-gen-proto>)
set_tests_properties(lift-ir-gen-proto PROPERTIES SKIP_RETURN_CODE 77)

add_executable(lift-ir-gen-shim tests/lift_ir_shim.cpp ${LIFT_GEN_SRC})
target_link_libraries(lift-ir-gen-shim PRIVATE emulator-lift-runtime)
target_compile_definitions(lift-ir-gen-shim PRIVATE ${LIFT_GEN_DEFINE})
# ★ 同 `lift-ir-gen-proto`：共用源文件、用例名跟着 target 走（`lift-ir-gen-shim`）。
target_compile_definitions(lift-ir-gen-shim PRIVATE LIFT_IR_CASE_GEN=1)
target_compile_definitions(lift-ir-gen-shim PRIVATE LIFT_IR_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
target_compile_definitions(lift-ir-gen-shim PRIVATE _CRT_SECURE_NO_WARNINGS)
add_test(NAME lift-ir-gen-shim COMMAND lift-ir-gen-shim)
set_tests_properties(lift-ir-gen-shim PROPERTIES SKIP_RETURN_CODE 77)

# =====================================================================================
# 检查点 2 / D49：**x87 两臂差分**（`lift-x87-diff`）
# ★ 判据（红了意味着什么）：`tests/ir/x87_roots.txt` 里 23 个真实 x87 站点（= 300 帧直方图
#   99.6% 的 x87 动态调用）逐个"生成代码 vs 解释器"逐位相同 —— 8 个 double 槽的**位型**、
#   top、tag、状态字、控制字、8 GPR、EFLAGS、一段 guest 内存。详见 `tests/x87_diff.cpp` 头注。
# ★ 为什么单独一份生成物：根集只有这 23 个 EA ⇒ 模块小、构建快，且**每个根就是一个 x87 指令**
#   （生成器的块首 = 根）⇒ 差分从"那条 op"开始，而不是从某个函数入口开始。
# ★ 缺 clang / 缺执行源 ⇒ 不链生成物、不定义 `LIFT_X87_DIFF` ⇒ 测试体自己返回 77（SKIP），
#   与 `lift-ir-gen-*` 同一套处置。⛔ `add_test` 仍在**最外层**（不嵌进 if / foreach）⇒ 档位 = pure。
# =====================================================================================
set(LIFT_X87_DIR "${CMAKE_CURRENT_BINARY_DIR}/x87diff")
set(LIFT_X87_ROOTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/x87_roots.txt")
if(LIFT_IR_CLANG AND EXISTS "${LIFT_GEN_AGE_EXE}")
    add_custom_command(
        OUTPUT "${LIFT_X87_DIR}/lift_aot.ll" "${LIFT_X87_DIR}/lift_pin.h"
               "${LIFT_X87_DIR}/lift_table.inc"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LIFT_X87_DIR}"
        COMMAND $<TARGET_FILE:emulator-lift-gen>
                --exe "${LIFT_GEN_AGE_EXE}" --roots "${LIFT_X87_ROOTS}"
                --out "${LIFT_X87_DIR}/lift_aot.ll"
                --pin "${LIFT_X87_DIR}/lift_pin.h"
                --table "${LIFT_X87_DIR}/lift_table.inc"
                --no-close-calls --no-close-stores --no-close-data --no-close-jmptab --no-s7
        DEPENDS emulator-lift-gen "${LIFT_X87_ROOTS}" "${LIFT_GEN_AGE_EXE}"
        COMMENT "lift-x87-diff: roots + PE -> x87diff/lift_aot.ll + pin + table"
        VERBATIM)
    add_custom_command(
        OUTPUT "${LIFT_X87_DIR}/lift_aot.obj"
        COMMAND ${LIFT_IR_CLANG} -c -O2 ${LIFT_IR_TARGET_FLAGS} -Wno-override-module
                -o "${LIFT_X87_DIR}/lift_aot.obj" "${LIFT_X87_DIR}/lift_aot.ll"
        DEPENDS "${LIFT_X87_DIR}/lift_aot.ll"
        COMMENT "lift-x87-diff: lift_aot.ll -> .obj (clang -c -O2)")
    set_source_files_properties("${LIFT_X87_DIR}/lift_aot.obj"
        PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_executable(lift-x87-diff tests/x87_diff.cpp src/aot/aot_dispatch.cpp
                   "${LIFT_X87_DIR}/lift_aot.obj")
    # ★ `aot_dispatch.cpp` 靠这个宏决定"表里有没有内容"（与 `emulator-lift-aot` 同一判据）
    target_compile_definitions(lift-x87-diff PRIVATE LIFT_AOT_HAS_GENERATED=1 LIFT_X87_DIFF=1
        LIFT_X87_PE_PATH="${LIFT_GEN_AGE_EXE}" _CRT_SECURE_NO_WARNINGS)
    target_include_directories(lift-x87-diff PRIVATE "${LIFT_X87_DIR}")
    # `aot_dispatch.cpp` `#include "lift_table.inc"` ⇒ 显式文件级依赖（clean build 的并行坑）
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/aot/aot_dispatch.cpp"
        PROPERTIES OBJECT_DEPENDS "${LIFT_X87_DIR}/lift_table.inc")
else()
    message(STATUS "lift-x87-diff: missing clang or execution source (${LIFT_GEN_AGE_EXE}) => generated output is not linked (test SKIPs 77)")
    add_executable(lift-x87-diff tests/x87_diff.cpp)
endif()
target_link_libraries(lift-x87-diff PRIVATE emulator-lift-runtime)
add_test(NAME lift-x87-diff COMMAND lift-x87-diff)
set_tests_properties(lift-x87-diff PROPERTIES SKIP_RETURN_CODE 77)

# =====================================================================================
# 2026-11-15 · **把 `lift_one` 调用点降到 0** 时新增的 inline 族：两臂差分（`lift-ops-diff`）
# ★ 判据（红了意味着什么）：`tests/ir/ops_roots.txt` 的每个站点，从同一份受控状态出发，
#   "生成代码 vs 解释器"逐位相同 —— 8 GPR / EFLAGS / 8 个 x87 槽（double 位型）/ top / tag /
#   状态字 / 控制字 / 8 个 xmm / mxcsr / 一段 guest 内存；停机类还要求 `stop_why` 字符串相同。
#   详见 `tests/ops_diff.cpp` 头注。
# ★ 为什么单独一份生成物：根集只含这些站点 ⇒ 模块小、构建快，且每个根就是一条指令
#   （生成器的块首 = 根）⇒ 差分从"那条 op"开始。
# ★ 缺 clang / 缺执行源 ⇒ 不链生成物、不定义 `LIFT_OPS_DIFF` ⇒ 测试体自己返回 77（SKIP），
#   与 `lift-ir-gen-*` 同一套处置。⛔ `add_test` 仍在**最外层** ⇒ 档位 = pure（§12.4）。
# =====================================================================================
set(LIFT_OPS_DIR "${CMAKE_CURRENT_BINARY_DIR}/opsdiff")
set(LIFT_OPS_ROOTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/ops_roots.txt")
if(LIFT_IR_CLANG AND EXISTS "${LIFT_GEN_AGE_EXE}")
    add_custom_command(
        OUTPUT "${LIFT_OPS_DIR}/lift_aot.ll" "${LIFT_OPS_DIR}/lift_pin.h"
               "${LIFT_OPS_DIR}/lift_table.inc"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LIFT_OPS_DIR}"
        COMMAND $<TARGET_FILE:emulator-lift-gen>
                --exe "${LIFT_GEN_AGE_EXE}" --roots "${LIFT_OPS_ROOTS}"
                --out "${LIFT_OPS_DIR}/lift_aot.ll"
                --pin "${LIFT_OPS_DIR}/lift_pin.h"
                --table "${LIFT_OPS_DIR}/lift_table.inc"
                --no-close-calls --no-close-stores --no-close-data --no-close-jmptab --no-s7
        DEPENDS emulator-lift-gen "${LIFT_OPS_ROOTS}" "${LIFT_GEN_AGE_EXE}"
        COMMENT "lift-ops-diff: roots + PE -> opsdiff/lift_aot.ll + pin + table"
        VERBATIM)
    add_custom_command(
        OUTPUT "${LIFT_OPS_DIR}/lift_aot.obj"
        COMMAND ${LIFT_IR_CLANG} -c -O2 ${LIFT_IR_TARGET_FLAGS} -Wno-override-module
                -o "${LIFT_OPS_DIR}/lift_aot.obj" "${LIFT_OPS_DIR}/lift_aot.ll"
        DEPENDS "${LIFT_OPS_DIR}/lift_aot.ll"
        COMMENT "lift-ops-diff: lift_aot.ll -> .obj (clang -c -O2)")
    set_source_files_properties("${LIFT_OPS_DIR}/lift_aot.obj"
        PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_executable(lift-ops-diff tests/ops_diff.cpp src/aot/aot_dispatch.cpp
                   "${LIFT_OPS_DIR}/lift_aot.obj")
    target_compile_definitions(lift-ops-diff PRIVATE LIFT_AOT_HAS_GENERATED=1 LIFT_OPS_DIFF=1
        LIFT_OPS_PE_PATH="${LIFT_GEN_AGE_EXE}" _CRT_SECURE_NO_WARNINGS)
    target_include_directories(lift-ops-diff PRIVATE "${LIFT_OPS_DIR}")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/aot/aot_dispatch.cpp"
        PROPERTIES OBJECT_DEPENDS "${LIFT_OPS_DIR}/lift_table.inc")
else()
    message(STATUS "lift-ops-diff: missing clang or execution source (${LIFT_GEN_AGE_EXE}) => generated output is not linked (test SKIPs 77)")
    add_executable(lift-ops-diff tests/ops_diff.cpp)
endif()
target_link_libraries(lift-ops-diff PRIVATE emulator-lift-runtime)
add_test(NAME lift-ops-diff COMMAND lift-ops-diff)
set_tests_properties(lift-ops-diff PROPERTIES SKIP_RETURN_CODE 77)


# =====================================================================================
# S7 / D53：**宿主 `ret` / 宿主 `call` 与 guest 栈的一致性**（`lift-s7-structured`）
# ★ 判据（红了意味着什么）：`tests/ir/s7_roots.txt` 的调用者 F 在受控状态下
#   一次表查找就跑完 F → `call G` → G 返回 → F 返回；返回后 guest `esp` 回到调用前、
#   `[esp]` 仍是没被写过的栈哨兵；且与解释器臂逐位相同。详见 `tests/s7_structured.cpp` 头注。
# ★ 为什么单独一份生成物：只需要一对（F, G，都是单元根）⇒ 模块小、构建快，差分从函数入口开始。
# ★ 缺 clang / 缺执行源 ⇒ 不链生成物、不定义 `LIFT_S7_STRUCTURED` ⇒ 测试体返回 77（SKIP）；
#   ⛔ `add_test` 仍在**最外层**（档位 = pure，判据 = 注册位置 §12.4）。
# =====================================================================================
set(LIFT_S7_DIR "${CMAKE_CURRENT_BINARY_DIR}/s7diff")
set(LIFT_S7_ROOTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/ir/s7_roots.txt")
# ★ 与 `tests/ir/s7_roots.txt` **一一对应**（测试体用它们做"这真的是一次宿主 call/ret"的自检）。
set(LIFT_S7_CALLER_EA 0x004C0980)
set(LIFT_S7_CALLEE_EA 0x004C0210)
if(LIFT_IR_CLANG AND EXISTS "${LIFT_GEN_AGE_EXE}")
    add_custom_command(
        OUTPUT "${LIFT_S7_DIR}/lift_aot.ll" "${LIFT_S7_DIR}/lift_pin.h"
               "${LIFT_S7_DIR}/lift_table.inc"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${LIFT_S7_DIR}"
        COMMAND $<TARGET_FILE:emulator-lift-gen>
                --exe "${LIFT_GEN_AGE_EXE}" --roots "${LIFT_S7_ROOTS}"
                --out "${LIFT_S7_DIR}/lift_aot.ll"
                --pin "${LIFT_S7_DIR}/lift_pin.h"
                --table "${LIFT_S7_DIR}/lift_table.inc"
                --no-close-stores --no-close-data --no-close-jmptab
        DEPENDS emulator-lift-gen "${LIFT_S7_ROOTS}" "${LIFT_GEN_AGE_EXE}"
        COMMENT "lift-s7-structured: roots + PE -> s7diff/lift_aot.ll + pin + table"
        VERBATIM)
    add_custom_command(
        OUTPUT "${LIFT_S7_DIR}/lift_aot.obj"
        COMMAND ${LIFT_IR_CLANG} -c -O2 ${LIFT_IR_TARGET_FLAGS} -Wno-override-module
                -o "${LIFT_S7_DIR}/lift_aot.obj" "${LIFT_S7_DIR}/lift_aot.ll"
        DEPENDS "${LIFT_S7_DIR}/lift_aot.ll"
        COMMENT "lift-s7-structured: lift_aot.ll -> .obj (clang -c -O2)")
    set_source_files_properties("${LIFT_S7_DIR}/lift_aot.obj"
        PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    add_executable(lift-s7-structured tests/s7_structured.cpp src/aot/aot_dispatch.cpp
                   "${LIFT_S7_DIR}/lift_aot.obj")
    target_compile_definitions(lift-s7-structured PRIVATE LIFT_AOT_HAS_GENERATED=1
        LIFT_S7_STRUCTURED=1 LIFT_S7_PE_PATH="${LIFT_GEN_AGE_EXE}"
        LIFT_S7_CALLER_EA=${LIFT_S7_CALLER_EA} LIFT_S7_CALLEE_EA=${LIFT_S7_CALLEE_EA}
        _CRT_SECURE_NO_WARNINGS)
    target_include_directories(lift-s7-structured PRIVATE "${LIFT_S7_DIR}")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/aot/aot_dispatch.cpp"
        PROPERTIES OBJECT_DEPENDS "${LIFT_S7_DIR}/lift_table.inc")
else()
    message(STATUS "lift-s7-structured: missing clang or execution source (${LIFT_GEN_AGE_EXE}) => generated output is not linked (test SKIPs 77)")
    add_executable(lift-s7-structured tests/s7_structured.cpp)
    target_compile_definitions(lift-s7-structured PRIVATE LIFT_S7_CALLER_EA=0 LIFT_S7_CALLEE_EA=0)
endif()
target_link_libraries(lift-s7-structured PRIVATE emulator-lift-runtime)
add_test(NAME lift-s7-structured COMMAND lift-s7-structured)
set_tests_properties(lift-s7-structured PROPERTIES SKIP_RETURN_CODE 77)

# =====================================================================================
# BUG-24 / `REQ-01M7FS59VTS7X4TGVK6Z7MTKJG`：**跳表项必须可派发 + 表区间内不许有块首**
# （`lift-jmptab-dispatch`）
# ★ 判据（红了意味着什么）：`emulator-lift-gen --jmptab-audit` 的**退出码非 0** —— 审计模式把
#   P-1..P-4 四条**性质**逐张表当场判掉：
#     P-1 `[ea, ea+4n)` 落在同一个可执行节内；
#     P-2 `[ea, ea+4n)` 内**不存在任何已发射块的首 EA**（⇒ 表字节没有被当指令解码）；
#     P-3 每个"在可执行节内 ∧ 能解码"的表项都**可派发**（`dispatch.has(item)`）；
#     P-4 表项无自指（`items_selfref == 0` ∧ `in_section == 1`）。
#   失败时必须能**只靠日志定位到 EA**：不过的性质逐条打到 stderr
#   （`[jmptab/P-2] ea=… 表区间 […] 内有块首 0x…` / `[jmptab/P-3] ea=… 有 N 个表项…不可派发`）。
# ★ 为什么是 `pure` 档：`add_test` 在**最外层**（不嵌进 `if` / `foreach` / `function`）⇒ 默认
#   `ctest` 就跑（AGENTS.md §12.4：档位判据 = 注册位置，⛔ 不建清单文件）。
# ★ 输入只有 `--exe <AGE.EXE>` + `--roots aot/closure.txt`：⛔ 不需要 `--out` / `--pin` / `--table`
#   （审计模式跑 ①–③ + 分区 + D21 到 `dispatch_entries` 定型后**提前返回**，不发射 IR、不写 `.ll`）；
#   ⛔ **不依赖 `LIFT_AGE_LST`**（产品路径不许读 `.lst`，守卫也不许）。
# ★ `COMMAND` 用**裸 target 名**（`emulator-lift-gen`，CMake 会替换成可执行路径）而不是
#   `$<TARGET_FILE:…>`：`tools/test/cmake-test-entry.test.mjs` 的解析器要求 COMMAND 的首 token
#   能映射到 `add_executable` 的源文件（否则"每个 --flag 都在源文件里有字面量"这条守卫判不了）。
# ★ ⛔ 不断言任何当前数字（不写 `tables == 62` / `items == 663`）：判据只有性质 + 退出码。
#   BUG-24 修复前的实测（`--no-jmptab-exclude --no-close-jmptab-items`）：P-2 红在 15 张表
#   （34 个块首）、P-3 红在 5 张表（30 项）—— 这些是**读数**，不是断言（见
#   `.tmp/aot/bug24/impl-report.md`）。
# =====================================================================================
add_test(NAME lift-jmptab-dispatch
    COMMAND emulator-lift-gen --jmptab-audit
            --exe "${LIFT_AOT_AGE_EXE}" --roots "${LIFT_AOT_CLOSURE}")

# =====================================================================================
# D26 的第二条扫描：**可执行节里任意偏移的指令 imm32**（`push imm32` / `mov reg, imm32`）
#
# 判据（红了意味着什么）见 `tests/closure_imm32.cpp` 文件头：
#   ① `0x004F80E0` / `0x00513B90` / `0x0041389A` 三个"只有未对齐 imm32 引用"的入口
#      都必须出现在**候选集**里；② `push` 与 `mov` 两半的输入面计数自洽；
#   ③ `0x004F80E0` 的旧手工根已从 `aot/closure.txt` 删掉（所以派发表里有它只可能来自扫描）；
#   ④ 生成物里真有它：派发表 3 行 `LIFT_AOT_ENTRY(0x…u, …)` + `.ll` 的
#      `define i32 @lift_cc_<EA>(`。
# ★ 守卫与生成器**共用同一份**扫描实现（`src/aot/closure_scan.h`，inline）⇒ 它绿才说明
#   生成器收得到；两处各写一套的话，守卫绿只是"另一份扫描绿"。
# ★ 输入（PE / closure.txt / out/aot 生成物）假定在场；生成物不在场 ⇒ `LIFT_SKIP(77)`。
# ★ 顶层无条件注册 ⇒ **pure** 档（`AGENTS.md` §12.4：判据 = 注册位置）。
# ★ `add_dependencies(… emulator-lift-aot)`：它读的是**产品 target 的生成物**
#   （`lift_table.inc` / `lift_aot*.ll`）。没有这一行时 `cmake --build out --target
#   lift-closure-imm32` 只编守卫 exe ⇒ 读到的可能是变异前的旧表 ⇒ 变异自检**假绿**
#   （本仓 `cli-aot-flags` / `aot-audio-wiring` 的同形坑）。
# =====================================================================================
add_executable(lift-closure-imm32 tests/closure_imm32.cpp)
target_link_libraries(lift-closure-imm32 PRIVATE emulator-lift-runtime)
add_dependencies(lift-closure-imm32 emulator-lift-aot)
add_test(NAME lift-closure-imm32
    COMMAND lift-closure-imm32
            "${LIFT_AOT_AGE_EXE}" "${LIFT_AOT_CLOSURE}"
            "${LIFT_AOT_DIR}/lift_table.inc" "${LIFT_AOT_DIR}")
set_tests_properties(lift-closure-imm32 PROPERTIES SKIP_RETURN_CODE 77)

# =====================================================================================
# AOT 音频接线 S0–S2：**定档谓词 + 参数面**（pure）与**真音频设备档**（配置期 opt-in）
#
# ★ ① `aot-audio-flags`：纯头 `runtime/audio_policy.h` 的判据（S0 真值表 / S1 值域）
#   + `AudioEngine::configure` 的静音门与 dump 提升 + 缺省档 begin/end 是 no-op。
#   输入只有"自造字节的 Machine 夹具"（`tests/audio.cpp` 同一种 `initBare`/`prepareStack`），
#   ⛔ 不读 LFS / 安装树、⛔ 不碰 SDL 设备 ⇒ `add_test` 在**最外层**（§12.4 判据 = 注册位置）。
# ★ ② `audio-sdl-device`：只在"这台机器真有音频设备"时才该跑。本机可能没有（无桌面 / 远程会话 /
#   服务账号）⇒ **配置期 opt-in**（`if(DEFINED ENV{LIFT_AUDIO_SDL_DEVICE})`，档位 = assets，§12.4）
#   + 运行期 `LIFT_SKIP(77)`（CMake 侧 `SKIP_RETURN_CODE 77`）。
#   ⛔ 不要把它挪到最外层"让默认 ctest 也跑一下"：那会在没有声卡的机器上变成一条**永远 skip**
#   的用例（§14.5 的口径：缺输入的症状不该被"看起来跑了"掩盖）。
#   打开方式：`$env:LIFT_AUDIO_SDL_DEVICE=1; cmake --preset windows-x64`，之后
#   `ctest --test-dir out -R audio-sdl-device --output-on-failure`。
# =====================================================================================
add_executable(lift-aot-audio-flags tests/aot_audio_flags.cpp)
target_link_libraries(lift-aot-audio-flags PRIVATE emulator-lift-runtime)
add_test(NAME aot-audio-flags COMMAND lift-aot-audio-flags)

# ---- S3（`REQ-01M7JZM1QQ728KX1C0G2F232T1` 的 ③）：**泵闸门**的纯判据 ----------------
# ★ 判据 = `runtime/audio_pump_gate.h`（纯头，只 `<cstdint>`）：off 档 20 万块边界一次都不 due
#   （= `pump()` 调用次数 0）、节流恰好每 kCheckStride 一次、嵌套 guest 期间不 due 且解除后立刻补、
#   thread 档不 due。红了意味着什么见 `tests/aot_audio_pump.cpp` 的文件头。
# ★ 为什么单独一条 CTest 而不是塞进 `aot-audio-flags`：它是**另一条判据线**（热路径的调用许可 +
#   节流 + 记账），红了要先看"闸门"而不是"参数面"；且变异条目要能点名它。
# ★ 顶层无条件注册 ⇒ **pure** 档（§12.4 判据 = 注册位置）：不碰设备 / 资产 / guest 内存。
add_executable(lift-aot-audio-pump tests/aot_audio_pump.cpp)
target_link_libraries(lift-aot-audio-pump PRIVATE emulator-lift-runtime)
add_test(NAME aot-audio-pump COMMAND lift-aot-audio-pump)

# ---- `--sound` / `--audio` / `--audio-dump` 的**接线**（洞 4/5/6/7 的机械判据）-------------
# ★ 判据 = **引擎自己打的读数**（`audio     : … muted=0|1`）+ WAV 的结构，⛔ 不是 `kAotSoundNotice`
#   那句文案（旧 `cli-aot-flags` 的 `sound-accepted-with-once-notice` 只断言自己的文案 ⇒ 自证式）。
#   判据清单与"红了意味着什么"见 `tests/aot_audio_wiring.cpp` 的文件头；核心一条：
#   **删掉 `aot_main` / `AotHost::configureAudio` 里 `--sound` 的接线 ⇒ 这条用例红**。
# ★ 为什么是**真 run**（4 次 `--max-frames 5`，本机 CTest 读数 2.1 s）：那三段是接线，纯夹具只钉得住最后一段
#   （`aot-audio-flags` 的 ③ 已钉 `AudioEngine` 自己）。
# ★ 缺 GPU / 缺生成物 / 缺执行源 ⇒ `LIFT_SKIP(77)`（CMake 侧 `SKIP_RETURN_CODE 77`）；
#   ⛔ 不挪到 `if()` 里当"只在有资产时才注册"——那会让"这台机器上它从不证明任何事"变成静默事实。
add_executable(lift-aot-audio-wiring tests/aot_audio_wiring.cpp)
target_link_libraries(lift-aot-audio-wiring PRIVATE emulator-lift-runtime)
# ★ 守卫**通过子进程**跑产品 exe ⇒ 必须声明这条依赖：否则 `cmake --build out --target
#   lift-aot-audio-wiring`（变异自检 `buildTarget` 的用法）只重建守卫 exe、产品 exe 停在**变异前**
#   ⇒ 变异施加了却读的是旧二进制 ⇒ 那条变异条目会**假绿**（本仓 `cli-aot-flags` 的同形坑）。
add_dependencies(lift-aot-audio-wiring emulator-lift-aot)
add_test(NAME aot-audio-wiring
    COMMAND lift-aot-audio-wiring $<TARGET_FILE:emulator-lift-aot> "${AGE_SECTFIX_EXE}")
set_tests_properties(aot-audio-wiring PROPERTIES SKIP_RETURN_CODE 77)

# ---- M0：AOT `--overlay` 的端到端（真子进程 + 窗口截图 + guest 帧对照）-----------------------
# ★ 判据：`--present window --present-hidden --overlay --max-frames 2` ⇒ exit 0；窗口截图在
#   **画布原点**出现 HUD 底色；同一命令关掉 `--overlay` ⇒ 那个像素是 guest 色；
#   `--dump-frame` 两次逐字节相同（⛔ UI 不进 guest 后缓冲）；`present : … lastHash=` 相同；
#   stderr 里 `[overlay]` / `[ui]` 至多一条（一次性收口行允许）。
# ★ 缺 GPU / 生成物 / 执行源 / 宿主字体 ⇒ 测试体自己 `LIFT_SKIP(77)`；`add_test` 在最外层
#   ⇒ 档位 = pure（`AGENTS.md` §12.4：输入假定在场，缺了不改变档位）。
# ★ 子进程 spawn 产品 exe ⇒ 必须带 `add_dependencies`（否则 `--target lift-aot-overlay` 只重建
#   守卫 exe，产品 exe 停在变异前 ⇒ 变异自检假绿；同形坑见 `aot-audio-wiring`）。
add_executable(lift-aot-overlay tests/aot_overlay.cpp)
target_link_libraries(lift-aot-overlay PRIVATE emulator-lift-runtime)
add_dependencies(lift-aot-overlay emulator-lift-aot)
add_test(NAME aot-overlay
    COMMAND lift-aot-overlay $<TARGET_FILE:emulator-lift-aot> "${AGE_SECTFIX_EXE}" "${LIFT_STRESS_GAME_DIR}")
set_tests_properties(aot-overlay PROPERTIES SKIP_RETURN_CODE 77)

if(DEFINED ENV{LIFT_AUDIO_SDL_DEVICE})
    add_executable(lift-audio-sdl-device tests/audio_sdl_device.cpp)
    target_link_libraries(lift-audio-sdl-device PRIVATE emulator-lift-runtime)
    add_test(NAME audio-sdl-device COMMAND lift-audio-sdl-device)
    set_tests_properties(audio-sdl-device PROPERTIES SKIP_RETURN_CODE 77)
endif()

# =====================================================================================
# GPU 路线 P0/P1 · D3DX effect（RT_RCDATA 字节 + `D3DXCreateEffectFromResourceA` + ID3DXEffect 9 槽）
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
# ★ 实现 = `src/shims/d3dx9_effect.cpp`（读 RT_RCDATA 走 `X86::image` 的**既有** PeResources；
#   ⛔ 不新写 PE 解析、⛔ 不从 guest 可写内存重解析）。取法与"为什么是确定的"见该文件头。
# ★ CTest 顶层无条件注册 ⇒ pure 档（§12.4 判据 = 注册位置）。语料路径由 CMake 注入；缺了由用例
#   自己判（caps 映射 / 自造 PE 的响亮停两条**不读语料**；4 份字节 / COM 面两条 SKIP 77）。
# ★ 变异清单条目见 `tools/mutate-check.mjs` 的 `--match 'd3dx9_effect'`（资源读取器 id 改坏 ⇒ 红）。
# ★ 实现已**无条件**加进 runtime（见上面的 d3dx9 簇）；这里只注册 CTest。
#   ⛔ 不要把 `target_sources(... d3dx9_effect.cpp)` 放回 `if(LIFT_BUILD_TESTS)`：
#      tests OFF 的体验版会在链接 `emulator-lift-aot` 时 undefined（2026-11-18 实测）。
# =====================================================================================
add_executable(lift-d3dx9-effect tests/d3dx9_effect.cpp)
target_link_libraries(lift-d3dx9-effect PRIVATE emulator-lift-runtime)
add_test(NAME d3dx9-effect COMMAND lift-d3dx9-effect)
set_tests_properties(d3dx9-effect PROPERTIES
    ENVIRONMENT "LIFT_AGE_EXE=${AGE_SECTFIX_EXE}" SKIP_RETURN_CODE 77)

endif() # LIFT_BUILD_TESTS

# =====================================================================================
# A · UI 脚本内核（QuickJS-ng v0.18.0 vendor + 语言无关适配器）
# ★ 纯追加块：不改上面任何一行，也不改 runtime 的既有 source 列表（本单的边界）。
# ★ 接口 = `src/ui/script.h`（⛔ 不 include quickjs.h）；唯一 include 它的 TU =
#   `src/ui/script_quickjs.cpp`；PIMPL 边界见 `src/ui/script_quickjs.h`。
# ★ vendor 的原样文件 / commit / 升级口径见 `third_party/quickjs-ng/README.md`；它的
#   `CMakeLists.txt` 建 `lift-qjs`（4 个 C 源、C11、`_GNU_SOURCE` + `QUICKJS_NG_BUILD`、
#   WIN32 的 `WIN32_LEAN_AND_MEAN` / `_WIN32_WINNT=0x0601`、非 MSVC 的 `-funsigned-char`）。
# =====================================================================================
add_subdirectory(third_party/quickjs-ng)

# ★ runtime 的 source 列表在上面（且归并发编辑协议的"别改"区）⇒ 用 `target_sources` 追加；
#   `PUBLIC lift-qjs` 让最终 exe（AOT 臂 / 本单的 CTest）自动链上静态库。
target_sources(emulator-lift-runtime PRIVATE src/ui/script_quickjs.cpp)
target_link_libraries(emulator-lift-runtime PUBLIC lift-qjs)

# ★ `ui-script-quickjs`：语言无关脚本适配器的 pure 判据（沙箱 / 预算 / api / readings /
#   error / entry）。**顶层**注册 ⇒ pure 档（AGENTS.md §12.4：判据 = 注册位置）。
#   ★ 外面这层 `if(LIFT_BUILD_TESTS)` 只包**测试 target 本身**（`OFF` 的语义 = 不编测试 target，
#     与文件顶部的开关一致）；`cmakeTestTier()` 对"唯一条件 `if(LIFT_BUILD_TESTS)`"是**透明**的
#     （`tools/lib/ledger.mjs:718`）⇒ 档位仍是 pure，不是 assets。
#   ★ `add_subdirectory` / `target_sources` / runtime 的 `PUBLIC lift-qjs` 留在**外面**：
#     产品构建（tests OFF）照样编脚本内核并链进 exe。
# ★ 变异：`tools/mutate-check.mjs` 的 `ui-script-quickjs-fn-ctor` 删掉
#   `script_quickjs.cpp` 里对 `Function.prototype.constructor` 的清除 ⇒ 本用例当场红。
if(LIFT_BUILD_TESTS)
    add_executable(lift-ui-script-quickjs tests/ui_script_quickjs.cpp)
    target_link_libraries(lift-ui-script-quickjs PRIVATE emulator-lift-runtime)
    add_test(NAME ui-script-quickjs COMMAND lift-ui-script-quickjs)
endif()

# =====================================================================================
# B · `--overlay` 的 HUD 迁到**外置脚本**（`scripts/ui/hud.js`，QuickJS 同步产出 display list）
# ★ 纯追加块：不改上面任何一行（除 B 的边界：runtime 源列表里的 overlay.cpp / hud.cpp 已换成
#   `src/ui/present_ui.cpp`）。
# ★ 为什么脚本**外置**（用户 2026-11-18 裁决）：JS 是**用户可自行发挥的 feature**，
#   `scripts/ui/ui.d.ts` 随发行分发 ⇒ ⛔ 不把 `hud.js` 编进 exe（否则用户改了也不生效）。
#   解析顺序（`--overlay` 且没给 `--ui`）：
#     ① `<exe 所在目录>/scripts/ui/hud.js`（发行布局；`aot_main` 从 argv[0] 取目录来解析）
#     ② `LIFT_UI_SCRIPT_DIR/hud.js`（**开发回退**，本文件注入的源码树路径；`SdlWindow` 做）
#     ③ 都不在 ⇒ **启动硬错**（err 点名试过的两个路径，⛔ 不静默回退空 HUD）
#   ⇒ `scripts/ui/hud.js` 是**运行时读**的文件：改它**不需要重编**（只重跑）。
# ★ `--ui <path>` 显式优先，且隐含 `--overlay`（与 `--overlay` 同给时 `--ui` 赢）。
# =====================================================================================
target_compile_definitions(emulator-lift-runtime PUBLIC
    LIFT_UI_SCRIPT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/scripts/ui")

# ★ `ui-hud-script`：默认 HUD 脚本（`scripts/ui/hud.js`）的**内容判据** —— 行序（mouse / script /
#   fps / frame / worst）、⛔ 没有 raster / present 两行、颜色精确值、缺省项整行省略。
#   直接读 `LIFT_UI_SCRIPT_DIR "/hud.js"` ⇒ 改脚本就会让这条用例看见（改错 ⇒ 红）。
#   **顶层** `if(LIFT_BUILD_TESTS)` ⇒ `cmakeTestTier()` 透明 ⇒ pure 档（AGENTS.md §12.4）。
#   缺宿主字体 ⇒ 用例自己 `LIFT_SKIP(77)`（`SKIP_RETURN_CODE 77`）。
# ★ 变异：`tools/mutate-check.mjs` 的 `ui-hud-script-no-frame` 删掉 `frame` 那一行 ⇒ 本用例当场红。
if(LIFT_BUILD_TESTS)
    add_executable(lift-ui-hud-script tests/ui_hud_script.cpp)
    target_link_libraries(lift-ui-hud-script PRIVATE emulator-lift-runtime)
    add_test(NAME ui-hud-script COMMAND lift-ui-hud-script)
    set_tests_properties(ui-hud-script PROPERTIES SKIP_RETURN_CODE 77)
endif()

# =====================================================================================
# E · `game.local_frames()`（帧列表 / 栈顶脚本名）
# ★ 语义真源 = `.tmp/ui-qjs/frames-research.md`（C1–C14 已证 / U1–U8 未证）：
#   活动 = `record+0x00 != 0` 且已提交；`top = (slot == cur)`；顺序 = top-first
#   （`cur` 若活动第一，其余按槽号降序；最小版**不走 parent 链**）。
# ★ 落点：`src/runtime/engine_frames.{h,cpp}`（`EngineFrameSource`，只读 guest）+
#   `src/ui/script.h` 的 `UiFrameInfo` / `UiFrameSource` + `script_quickjs.cpp` 的**方法**注入
#   （`game` 上唯一不用 getter 的属性）+ `PresentSink::setUiFrameSource`（非 owning 指针）。
# ★ 为什么用 `target_sources` 追加而不是改上面的 source 列表：与 A 的 `script_quickjs.cpp` 同一条
#   并发编辑口径（大列表是"别改"区；本单只在文件末尾追加自己的块）。
# ★ `ui-frames`：**顶层** `if(LIFT_BUILD_TESTS)` ⇒ `cmakeTestTier()` 透明 ⇒ pure 档
#   （AGENTS.md §12.4：判据 = 注册位置）。夹具 = 裸 `GuestMemory` + `MarkerSampler` 合成 Engine，
#   ⛔ 不依赖游戏安装 / LFS / 真实窗口。
# ★ 变异：`tools/mutate-check.mjs` 末尾那条把 `top = (slot == cur)` 改成 `top = false` ⇒ 本用例红。
# =====================================================================================
target_sources(emulator-lift-runtime PRIVATE src/runtime/engine_frames.cpp)

if(LIFT_BUILD_TESTS)
    add_executable(lift-ui-frames tests/ui_frames.cpp)
    target_link_libraries(lift-ui-frames PRIVATE emulator-lift-runtime)
    add_test(NAME ui-frames COMMAND lift-ui-frames)
endif()

# =====================================================================================
# G · UI 脚本层的 ES module（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 2 前半）
# ★ 判据 = `tests/ui_script_modules.cpp`：相对 import / 嵌套 / 拒绝 bare + 绝对 + `data:` +
#   `..` 逃逸 / module 顶层抛错不被吞 / classic 路径不变 / 缓存 / 混合 load（见该文件头的 ①–⑩）。
# ★ 夹具 = `tests/ui_modules/`（跟着测试走；⛔ 不是发行的 `scripts/ui/`；切片 2 后半才放发行件）。
#   绝对路径由 CMake 注入（`LIFT_UI_MODULES_DIR`），用例不依赖 ctest 的 cwd。
# ★ **顶层** `if(LIFT_BUILD_TESTS)` ⇒ `cmakeTestTier()` 透明 ⇒ pure 档（AGENTS.md §12.4）。
# ★ 变异：`tools/mutate-check.mjs` 末尾那条把 module 求值的 rejection 检查改成恒 false
#   ⇒ module 顶层的异常被静默吞掉 ⇒ 本用例当场红。
# =====================================================================================
if(LIFT_BUILD_TESTS)
    add_executable(lift-ui-script-modules tests/ui_script_modules.cpp)
    target_link_libraries(lift-ui-script-modules PRIVATE emulator-lift-runtime)
    target_compile_definitions(lift-ui-script-modules PRIVATE
        LIFT_UI_MODULES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_modules")
    add_test(NAME ui-script-modules COMMAND lift-ui-script-modules)
endif()

# =====================================================================================
# F · `emulator-lift-gen` 的输出字节恒为 ASCII（构建期工具 ⇒ 构建日志的编码不可知）
# ★ 不变量：`lift-gen` 在**不依赖资产**的入口路径上（`--help` / 参数错误 / skip），stdout +
#   stderr 的每个字节都 < 0x80。红了 ⇒ ninja 捕获回放 / 管道 / 重定向消费者按本地码页解码就是
#   乱码（本单的缺陷面：构建期 UTF-8 被 936 解）。
# ★ 为什么是**运行时**守卫（而不是扫 `lift_gen.cpp` 的字面量）：断言的是**字节**，不是源码形状；
#   源码里写 ASCII 但经 `%s` 带进非 ASCII 时扫描看不出来。重定向走**文件**（⛔ 不用管道：
#   本仓有 `spawn EPERM` 先例）。
# ★ 不读 AGE.EXE / LFS / 安装树 ⇒ 顶层 `if(LIFT_BUILD_TESTS)` 注册 = **pure** 档
#   （AGENTS.md §12.4：判据 = 注册位置）。
# ★ `add_dependencies(... emulator-lift-gen)` 是**变异不会假绿**的前提：本用例 spawn 的是产品
#   exe（`$<TARGET_FILE:...>`）⇒ `cmake --build --target lift-gen-ascii-output` 必须把它一起重建，
#   否则变异（改回中文）落在产品 TU 里、守卫跑的是变异前的二进制。
# ★ 变异：`tools/mutate-check.mjs` 的 `gen-ascii-output-help-cjk` 把 usage 首行改回中文 ⇒ 本用例红。
# =====================================================================================
if(LIFT_BUILD_TESTS)
    add_executable(lift-gen-ascii-output tests/lift_gen_ascii_output.cpp)
    target_link_libraries(lift-gen-ascii-output PRIVATE emulator-lift-runtime)
    add_dependencies(lift-gen-ascii-output emulator-lift-gen)
    add_test(NAME gen-ascii-output COMMAND lift-gen-ascii-output $<TARGET_FILE:emulator-lift-gen>)
endif()

# =====================================================================================
# G · `D3DPRESENT_PARAMETERS.PresentationInterval` 的白名单（不变量：只接受 IMMEDIATE）
#
# ★ 不变量：`0x80000000 (D3DPRESENT_INTERVAL_IMMEDIATE)` 通过；`0 / 1 / 2 / 4 / 8 / 其余任意值`
#   全拒（裁决 2026-11-19 · `REQ-01M7QVQR3ENQW3ZWNR4VFTPYJY`）。红了意味着：我们把 guest 的
#   "等 vblank" 请求静默当成 IMMEDIATE 放行了（真机会被 vblank 挡住、我们会跑得更快），
#   或者白名单本身被改成了别的值 —— 两种都是**静默的背离**，没有运行时症状。
# ★ 判据是纯函数（`runtime/d3d9.h` 的 `presentIntervalSupported`）⇒ 不读 AGE.EXE / LFS /
#   安装树 ⇒ 顶层 `if(LIFT_BUILD_TESTS)` 注册 = **pure** 档（AGENTS.md §12.4：判据 = 注册位置）。
# ★ 变异：`tools/mutate-check.mjs` 末尾那条把判定改成恒真（`return true`）⇒ 本用例当场红。
# =====================================================================================
if(LIFT_BUILD_TESTS)
    add_executable(lift-present-interval tests/present_interval.cpp)
    target_link_libraries(lift-present-interval PRIVATE emulator-lift-runtime)
    add_test(NAME present-interval COMMAND lift-present-interval)
endif()

# =====================================================================================
# H · script global 的 **typed 读写**（`runtime/script_globals.{h,cpp}` = AGE 引擎层）
#
# ★ 不变量：`global-get` / `global-set` 的「寻址 + ENC/DEC + 类型编码」只有**一份**实现，且它在
#   `src/runtime/` 的 **AGE 引擎层**（控制层与将来的 QJS 侧共用同一份；⚠ 是否被其它 AGE 作品复用
#   = 待证，本模块只承诺"可替换"）。⛔ 不焊在控制层、⛔ 不依赖 quickjs。
#   红了意味着：int 槽里被写成了**明文**（引擎 DEC 出来是垃圾 ⇒ "写了但没生效"）、或 id 越界被静默
#   放过、或十六进制口径退回"自动识别十进制"（`0xB1B6` 的歧义回来）。
# ★ **顶层** `if(LIFT_BUILD_TESTS)` ⇒ `cmakeTestTier()` 透明 ⇒ pure 档（AGENTS.md §12.4）。
#   夹具 = 裸 `GuestMemory` + 假 Engine / 假池基址 / 假 key，⛔ 不读游戏安装 / LFS / 不起 socket。
# ★ 变异：`tools/mutate-check.mjs` 末尾两条 ——
#   ① `script-globals-int-enc-plaintext`：ENC → 明文写 ⇒ 往返用例红；
#   ② `control-global-hex-only`：十六进制解析改回自动识别十进制 ⇒ 十进制拒绝用例红。
# =====================================================================================
target_sources(emulator-lift-runtime PRIVATE src/runtime/script_globals.cpp)

if(LIFT_BUILD_TESTS)
    add_executable(lift-control-globals tests/control_globals.cpp)
    target_link_libraries(lift-control-globals PRIVATE emulator-lift-runtime)
    add_test(NAME lift-control-globals COMMAND lift-control-globals)
endif()

# =====================================================================================
# I · UI 的**命中原语 / 捕获路由**（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 2 后半）
#
# ★ 裁决：C++ 只做原语 —— `key` 挂在 `ui.rect` 上（⛔ 没有 C++ `ui.button`，⛔ 没有 JS 的
#   `ui.hit` 模块）；按钮外观 / 布局 / 状态 / 主题全在发行件 `scripts/ui/kit.js`。
#   落点 = `src/ui/hit.{h,cpp}`（纯状态机）+ `src/ui/raster.cpp` 的 `resolveRectPixels` /
#   `drawsBefore`（命中区与 `rasterize` **同一份**算术）+ `src/ui/script.h` 的三个只读读数
#   （click 一次性 / hover / active）+ `src/ui/script_quickjs.cpp` 的 getter 与 `ui.rect` 的 key。
# ★ 判据（顶层 `if(LIFT_BUILD_TESTS)` ⇒ `cmakeTestTier()` 透明 ⇒ pure 档，AGENTS.md §12.4）：
#   · `ui-hit`          逆序命中 / resolved rect 边界（1 px 内/外）/ 无成功帧不命中 / key 为空不可命中；
#   · `ui-click-router` down 吞+capture / up 无条件吞 / 同 rect 抬起才发 click / 拖出取消 /
#                       motion·wheel·右键放行 / 新 down 清 capture / click 一次性；
#   · `ui-kit-script`   经**现有 module loader** 加载发行件 `scripts/ui/kit.js`，断言 Button 的
#                       idle / hover / active 三态（display list + hit list 的形状），⛔ 不靠截图。
#   ★ `ui-script-quickjs` 增 `key-on-rect`：`ui.rect({key})` → 命中表；`game.click` 一次性；
#     `game.hover` / `game.active` 只读。
# ★ 变异（`tools/mutate-check.mjs` 末尾四条，都带 `buildTarget`）：
#   ① 命中改正序 ⇒ `ui-hit` 红；② 捕获时 up 未吞 ⇒ `ui-click-router` 红；
#   ③ click 消费后不清 ⇒ `ui-click-router` 红；④ kit 的 hover 高亮分支失效 ⇒ `ui-kit-script` 红。
# =====================================================================================
target_sources(emulator-lift-runtime PRIVATE src/ui/hit.cpp)

if(LIFT_BUILD_TESTS)
    add_executable(lift-ui-hit tests/ui_hit.cpp)
    target_link_libraries(lift-ui-hit PRIVATE emulator-lift-runtime)
    add_test(NAME ui-hit COMMAND lift-ui-hit)

    add_executable(lift-ui-click-router tests/ui_click_router.cpp)
    target_link_libraries(lift-ui-click-router PRIVATE emulator-lift-runtime)
    add_test(NAME ui-click-router COMMAND lift-ui-click-router)

    add_executable(lift-ui-kit-script tests/ui_kit_script.cpp)
    target_link_libraries(lift-ui-kit-script PRIVATE emulator-lift-runtime)
    add_test(NAME ui-kit-script COMMAND lift-ui-kit-script)
endif()

# =====================================================================================
# J · D3DXSprite 的混合：因子来自**设备渲染状态**，且 `Begin(ALPHABLEND)` 像真 D3DX 那样设/恢复
#     （`REQ-01M7R1ABAP86QVMNBZ5S4EDKD5` + `REQ-01M7R3YPQPNPBZ327H89Q8RTEE`）
#
# ★ 不变量：
#   ① `ID3DXSprite::Begin` 的 `ALPHABLEND` 位**只决定开不开**混合；因子 / op / separate-alpha
#      一律来自 `D3DRS_SRCBLEND` / `DESTBLEND` / `BLENDOP`（与 fixed-function 同一份读法
#      `d3d9::readBlendState` + 同一份映射 `gpuBlendStateFor`）。
#   ② 真 D3DX 的 `Begin(ALPHABLEND)` **自己**把 `ALPHABLENDENABLE=1` / `SRCBLEND=SRCALPHA` /
#      `DESTBLEND=INVSRCALPHA` 写进设备、`End` 恢复（`Flush` 不恢复）。
#   红了意味着：引擎的加性（`SRCALPHA / ONE`）被当成别的混合（缺省 `SrcAlpha / InvSrcAlpha`
#   会把一颗 sprite 的（几乎全黑的）背景不透明地盖成黑块 = SAVE05 的 120×120 黑框），**或**
#   普通批次读到引擎残留的 `ONE / ZERO`（透明黑 texel 写进后缓冲 = 走廊 / 对话框底板变黑）。
#   ★ 措辞纪律：这件事**与影片没有关系**；同帧的 dshow 影片只是相关读数（强相关、未直读）。
# ★ 判据见 `tests/d3dx9_sprite_blend.cpp`：纯映射（加性 / ALPHABLEND=0 / separate alpha
#   三态 / 表达不了 ⇒ false+why）+ **Begin/End 接管与恢复**（Begin 后 1/5/6，End 逐字回原值，
#   第二次可重复）+ **像素回归**（16×16 透明黑底 + 红点 ⇒ 背景一个 texel 都不许变黑）+
#   **接线**（引擎在 Begin 之后写 5/2 ⇒ 仍是加性；表达不了时**不入队**且账本 skipped 涨）。
# ★ 夹具只建裸 `Machine` + 假 `GpuRaster`（⛔ 不要 SDL / GPU 设备）⇒ 顶层 `if(LIFT_BUILD_TESTS)`
#   注册 = pure 档（AGENTS.md §12.4：判据 = 注册位置）。
# ★ 变异：`tools/mutate-check.mjs` 末尾 `sprite-begin-alphablend-state-writes` 把 `Begin` 里那三句
#   状态写禁掉 ⇒ 本用例的"接管与恢复"与"像素回归"当场红。
# =====================================================================================
if(LIFT_BUILD_TESTS)
    add_executable(lift-d3dx9-sprite-blend tests/d3dx9_sprite_blend.cpp)
    target_link_libraries(lift-d3dx9-sprite-blend PRIVATE emulator-lift-runtime)
    add_test(NAME d3dx9-sprite-blend COMMAND lift-d3dx9-sprite-blend)
endif()
