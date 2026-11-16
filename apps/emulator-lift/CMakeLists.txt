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
    # ★ 输入：`input.cpp` = SDL 真实事件与 `--input-script` 注入的**唯一**汇合点；
    #   `input_script.cpp` = 指令式时间线（`at frame N` / `at guest_ms T` / click / key ...）。
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
    # ★ 识别驱动的交互状态机（节点图 when → action → next；判定只读采样计数，动作经 InputScript 追加）。
    src/runtime/interaction_graph.cpp
    src/runtime/present.cpp
    src/runtime/present_sdl.cpp
    # ★ 画面 ↔ 窗口客户区的 letterbox 映射的**唯一一份**（鼠标事件入 + 宿主光标 warp 出共用；
    #   `--cursor-warp` 的几何判据在 `tests/cursor_warp.cpp` 的 `cursor-map/*`）。
    src/runtime/cursor_map.cpp
    # ★ 宿主光标原语（`--cursor-warp` 的唯一 OS 出口；单 TU 按 `#ifdef` 选平台实现）。
    src/runtime/host_cursor.cpp
    # ★ 窗口调试叠层（`--overlay`）：纯逻辑（5×7 位图字体 + 行格式化 + RGBA8 矩形绘制），无 SDL 类型。
    src/runtime/overlay.cpp
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

add_executable(emulator-lift src/main.cpp)
target_link_libraries(emulator-lift PRIVATE emulator-lift-runtime)

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
    "AOT IR 拆成几份：1 = 单份路径（今天的形状）；N>1 = 按分量根 EA 升序贪心分桶（N = 8 是裁决值）")
# ★ IR 的编译参数**唯一落点**（configure 期断言盯的就是这一行）。
set(LIFT_AOT_CLANG_FLAGS -c -O2)
if(LIFT_AOT_IR_CLANG AND EXISTS "${LIFT_AOT_AGE_EXE}" AND EXISTS "${LIFT_AOT_CLOSURE}")
    set(LIFT_AOT_TARGET_FLAGS "")
    if(WIN32)
        # 与预设 `windows-x64` 的 clang-cl 一致（`-c` 产出 COFF，链接器与 clang-cl 编的 TU 同源）
        set(LIFT_AOT_TARGET_FLAGS --target=x86_64-pc-windows-msvc)
    endif()

    # =================================================================================
    # ★ configure 期断言（守卫 3，⛔ 只断言**不相容**，不断言任何墙钟 / 字节数）：
    #   ① `LIFT_AOT_SPLIT > 1` 时 IR 的参数必须仍是 `-c -O2`；
    #   ② 出现 `-O1` / `-O0` / `-Os` / `-Oz` / `-flto` / `llc` ⇒ 直接失败；
    #   ③ `.obj` 的个数必须 == `LIFT_AOT_SPLIT`（下面 `target_sources` 之前复核）；
    #   ④ `LIFT_AOT_SINGLE_TU`（如将来有人加）与 `LIFT_AOT_SPLIT > 1` 互斥。
    #   为什么是 configure 期：把 `-O2` 降级 / 换 LTO 是**唯一**能让"拆分构建"在墙钟上
    #   好看却在运行期变慢的自欺路径；这类改动必须**当场**失败，而不是构建出来再比读数。
    # =================================================================================
    if(LIFT_AOT_SINGLE_TU AND LIFT_AOT_SPLIT GREATER 1)
        message(FATAL_ERROR "LIFT_AOT_SINGLE_TU 与 LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT} 互斥")
    endif()
    if(LIFT_AOT_SPLIT GREATER 100)
        message(FATAL_ERROR "LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT} 超出口令范围（≤ 100）")
    endif()
    if(LIFT_AOT_SPLIT GREATER 1)
        if(NOT "${LIFT_AOT_CLANG_FLAGS}" STREQUAL "-c;-O2")
            message(FATAL_ERROR
                "LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT} 时 AOT IR 的参数必须仍是 `-c -O2`，"
                "实际 = `${LIFT_AOT_CLANG_FLAGS}`（⛔ 不许降 -O1/-O0、⛔ 不许 LTO）")
        endif()
        foreach(_bad IN ITEMS -O0 -O1 -Os -Oz -flto -flto=thin llc)
            if("${LIFT_AOT_CLANG_FLAGS}" MATCHES "(^|;)${_bad}(;|$)")
                message(FATAL_ERROR
                    "LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT} 时 AOT IR 的参数里出现 `${_bad}`（⛔ 禁止）")
            endif()
        endforeach()
    endif()

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
        COMMENT "lift-aot: closure-gate + roots/PE -> out/aot/lift_aot*.ll（split=${LIFT_AOT_SPLIT}）+ lift_pin.h + lift_table.inc"
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
            COMMENT "lift-aot: ${_ll} -> ${_obj}（clang ${LIFT_AOT_CLANG_FLAGS}，无 llc）")
        set_source_files_properties("${_obj}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    endforeach()
    list(LENGTH LIFT_AOT_OBJS LIFT_AOT_OBJ_COUNT)
    if(LIFT_AOT_SPLIT GREATER 1 AND NOT LIFT_AOT_OBJ_COUNT EQUAL LIFT_AOT_SPLIT)
        # 守卫 3-③：obj 数 != 份数 ⇒ 链接期缺符号 / 多一份孤儿（⛔ 不许静默）。
        message(FATAL_ERROR
            "AOT 形状自相矛盾：LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT} 但 obj 数=${LIFT_AOT_OBJ_COUNT}")
    endif()
    target_sources(emulator-lift-aot PRIVATE
        src/aot/aot_dispatch.cpp
        src/aot/aot_run.cpp
        ${LIFT_AOT_OBJS})
    # ★ `aot_dispatch.cpp` `#include "lift_table.inc"`（生成物）⇒ 必须显式声明文件级依赖，
    #   否则并行构建可能在生成之前就编译它（clean build 才显形的那种红）。
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/src/aot/aot_dispatch.cpp"
        PROPERTIES OBJECT_DEPENDS "${LIFT_AOT_DIR}/lift_table.inc")
    target_include_directories(emulator-lift-aot PRIVATE "${LIFT_AOT_DIR}")
    # ★ 这个宏是"生成物在场"的唯一判据（`aot_dispatch.cpp` 用它决定表有没有内容；
    #   `aot_main.cpp` 用它决定"没有生成物"的响亮报告）。
    # ★ `LIFT_AOT_SHAPE` = 形状进二进制 + 启动横幅一行（判据档只能是 `single-O2` / `splitN-O2` 之一）。
    if(LIFT_AOT_SPLIT GREATER 1)
        set(LIFT_AOT_SHAPE "split${LIFT_AOT_SPLIT}-O2")
    else()
        set(LIFT_AOT_SHAPE "single-O2")
    endif()
    target_compile_definitions(emulator-lift-aot PRIVATE
        LIFT_AOT_HAS_GENERATED=1
        LIFT_AOT_SHAPE="${LIFT_AOT_SHAPE}"
        LIFT_AOT_SPLIT=${LIFT_AOT_SPLIT})
    message(STATUS "emulator-lift-aot: 执行源 = ${LIFT_AOT_OBJS}（shape=${LIFT_AOT_SHAPE}；clang = ${LIFT_AOT_IR_CLANG}）")
else()
    message(STATUS
        "emulator-lift-aot: 缺 clang / 执行源（${LIFT_AOT_AGE_EXE}）/ closure.txt ⇒ 替身 TU："
        "构建不断，但 AOT 启动时会响亮报告「没有生成物」并 exit 1")
    target_sources(emulator-lift-aot PRIVATE src/aot/aot_dispatch.cpp src/aot/aot_run.cpp)
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
# 测试（`LIFT_BUILD_TESTS`，默认 ON；`-DLIFT_BUILD_TESTS=OFF` ⇒ 只构建产品二进制）
# ★ 开关只包**测试 target 与 `add_test`**：产品二进制 `emulator-lift` / `emulator-lift-runtime`
#   与依赖库 `lift-pl-mpeg` 在上面的产品区（OFF 时必须照样能编出 `emulator-lift`）。
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
#   子进程实参 = `$<TARGET_FILE:emulator-lift>`。
# ★ 原 `lift/raster-flag-gone`（"`--raster` 必须继续不存在"）**已摘注册**：那条判的是**设计冻结**
#   （flag 已从代码里消失、没有静默失败路径；有人加回来 `git diff` 看得见）⇒ 按测试模型不配守卫。
add_test(NAME lift/no-gpu-refuses-start
    COMMAND lift-gpu-batch --no-gpu-refuses-start $<TARGET_FILE:emulator-lift>)
set_tests_properties(lift/no-gpu-refuses-start PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy"
    TIMEOUT 120)

# ---- pure：`--time-limit` 的 CLI 契约（`REQ-01M711JN018QDSWK4726Y1A0KV`）----
# 判据见 tests/cli_time_limit.cpp 的头注释：① `--time-limit 0` 与**省略该参数**行为等价
# （退出码 + 摘要里 stop / insns / blocks / trace-in / instr 五行逐字相同，且 stop 不是 time-limit）；
# ② 正数仍然生效；③ `-1` / `abc` 仍然 exit 2 且 stdout 没有运行摘要。
# ★ 纯档：执行源是**测试自己构造的最小 PE32** + `--max-insns 1` ⇒ 不需要 LFS / 游戏安装；
#   顶层无条件注册 ⇒ pure（判据 = 注册位置，见 AGENTS.md §5）。
# ★ 子进程的 cwd 由测试指到临时目录 ⇒ 默认的 `../../corpus/...` 执行源 / `.lst` 候选都找不到，
#   结果不随宿主 cwd 漂。
add_executable(lift-cli-time-limit tests/cli_time_limit.cpp)
target_link_libraries(lift-cli-time-limit PRIVATE emulator-lift-runtime)
add_test(NAME cli-time-limit COMMAND lift-cli-time-limit $<TARGET_FILE:emulator-lift>)
set_tests_properties(cli-time-limit PROPERTIES SKIP_RETURN_CODE 77)

# ---- pure：`SetCursorPos` 的宿主光标 warp（`--cursor-warp`，缺省关）----
# 判据见 tests/cursor_warp.cpp 的头注释：① 缺省档一次也不挪；② 开着时用**guest 客户区坐标**
# 调一次（非零窗口原点 ⇒ 忘掉"减窗口原点"当场红）；③ 挪不动 ⇒ FALSE；④ 没有 guest 窗口 ⇒
# 只改虚拟位置、TRUE；⑤ 全链不 clamp（负坐标 / 越界原样送进 warp）。
# ★ 夹具 = 裸 Machine + 假 PresentSink（**不建窗口、不碰真光标**）⇒ 顶层无条件注册 = pure 档。
add_executable(lift-cursor-warp tests/cursor_warp.cpp)
target_link_libraries(lift-cursor-warp PRIVATE emulator-lift-runtime)
add_test(NAME cursor-warp COMMAND lift-cursor-warp)

# ---- pure：`--cursor-warp` 的参数闸门（CLI 级 + 真子进程）----
# 判据见 tests/cli_cursor_warp.cpp：① 缺省呈现档（headless）+ 该 flag ⇒ 宿主 **exit 2**，
# stderr 点名 flag 与 `--present headless`，stdout 没有运行摘要；⛔ 不用 `WILL_FAIL TRUE`
# （那会让 abort / exit 3 也算过）；② 缺省档启动回声两处都报 `off`；③ 可见窗口下报 `on`
# （guest 只跑一条 `nop` ⇒ **不会**调 SetCursorPos，不挪任何人的光标）。
# ★ 顶层无条件注册 ⇒ pure 档；②③ 需要呈现后端，无 GPU 时由测试体自己 SKIP 77。
add_executable(lift-cli-cursor-warp tests/cli_cursor_warp.cpp)
target_link_libraries(lift-cli-cursor-warp PRIVATE emulator-lift-runtime)
add_test(NAME cli-cursor-warp COMMAND lift-cli-cursor-warp $<TARGET_FILE:emulator-lift>)
set_tests_properties(cli-cursor-warp PROPERTIES SKIP_RETURN_CODE 77)

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
add_test(NAME cli-aot-flags COMMAND lift-cli-aot-flags $<TARGET_FILE:emulator-lift-aot>)
set_tests_properties(cli-aot-flags PROPERTIES SKIP_RETURN_CODE 77)

# ---- pure：guest `ExitProcess(n)` / `TerminateProcess(_, n)` 的**宿主进程退出码**守卫 ----
# 判据见 tests/exit_code.cpp 文件头：① 纯映射（0 ⇒ 0；非 0 ⇒ 2 且**不透传** n）；
# ② **真子进程**（自造最小 PE32 导入 kernel32!ExitProcess，跑真实 emulator-lift）：
#    ExitProcess(3) ⇒ 宿主 exit 2 + 默认 warn 日志含 exit_code=3；ExitProcess(0) ⇒ 0；
# ③ TerminateProcess(_,3) 同一口径。
# ★ 纯档（顶层无条件注册）：执行源是测试自己构造的 PE，不碰 LFS / 游戏安装。
add_executable(lift-exit-code tests/exit_code.cpp)
target_link_libraries(lift-exit-code PRIVATE emulator-lift-runtime)
add_test(NAME exit-code COMMAND lift-exit-code $<TARGET_FILE:emulator-lift>)
set_tests_properties(exit-code PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

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

# ---- 默认：Windows 宿主编码契约（argv = UTF-8 / 输出字节 = UTF-8）----
# ★ 子进程用**文件**重定向（不走管道：受限沙箱里捕获管道会 EPERM）。
# ★ 起子进程时 cwd = 源码目录 ⇒ 默认的 `../../corpus/...` 执行源 / `.lst` 能按相对路径找到。
# ★ 它是**设计的合同** ⇒ **默认档（顶层无条件）**；⛔ 不要再用 `if(EXISTS …)` 决定它注册与否
#   （那会让"这条用例存不存在"随机器漂）。它要真启动 emulator-lift（会载入执行源）⇒ 缺 LFS 资产时
#   由测试体自己判（无 GPU ⇒ `LIFT_SKIP` 77；执行源缺失 ⇒ 它自己红）。
add_executable(lift-host-encoding tests/host_encoding.cpp)
target_link_libraries(lift-host-encoding PRIVATE emulator-lift-runtime)
add_test(NAME host-encoding COMMAND lift-host-encoding $<TARGET_FILE:emulator-lift>)
set_tests_properties(host-encoding PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    SKIP_RETURN_CODE 77)

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
    CACHE PATH "LIFT_STRESS 档的安装根（含 SYS4INI.BIN）")
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
        message(STATUS "输入盘点：present  ${_lift_abs}")
    else()
        message(STATUS "输入盘点：MISSING  ${_lift_abs}")
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
                "按用途显式打开了 LIFT_STRESS（它要真跑一遍安装树），但输入缺失：\n"
                "  MISSING  ${_lift_required}\n"
                "怎么补：\n"
                "  · 安装树：把真实游戏安装目录链到仓库根的 `links/install`（该目录含 SYS4INI.BIN），或\n"
                "    `cmake --preset <preset> -DLIFT_STRESS_GAME_DIR=<含 SYS4INI.BIN 的目录>`；\n"
                "  · LFS 语料：`git lfs install && git lfs pull`（清单见 `corpus/assets.json`）。\n"
                "⛔ 不要靠关掉输入让 configure 过：档位按用途，输入是假定在场的。")
        endif()
    endforeach()
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
# =====================================================================================

# ---- LIFT_STRESS：**feeder 臂**的段级 override 读数守卫（`bov= / bovd= / bovf=`；2026-11-13 降档）----
# REQ-01M75W6BB3PY3MF42ZTS23KTBS。判据见 tests/feeder_seg_e2e.cpp 文件头：
# ① 子进程 exit 0；② 日志里 `feeder=host-thread` + `device=ok`（缺 ⇒ SKIP 77：量错了臂）；
# ③ 至少一行 `bov>0`；④ 所有行 `bovd==0 && bovf==0`；⑤ `bov>0` 之后不许回到 0。
# ⛔ 不断言 `bov` 的具体值（读数进文档），⛔ 不断言 `ov` 上升/下降（段会吃掉 ftol2 的函数级命中）。
# ★ 语义证据在 `native-override-assets` 的 `pcm/*` 真字节两臂；这条只回答"feeder 里有没有接管"。
# ★ 真音频设备 / 窗口不可用 ⇒ 测试自己 `LIFT_SKIP`(77)（那不是判据红）。
# ★ **降档的理由是贵**：实测 30 s（`docs/03-emulator/observations.md`）⇒ 走 `LIFT_STRESS` opt-in
#   （判据 = **用途**：压力/效率面；⛔ 不是"资产在不在"）：
#     $env:LIFT_STRESS=1; ctest --test-dir out -R feeder-seg-e2e --output-on-failure
if(DEFINED ENV{LIFT_STRESS})
    add_executable(lift-feeder-seg-e2e tests/feeder_seg_e2e.cpp)
    target_link_libraries(lift-feeder-seg-e2e PRIVATE emulator-lift-runtime)
    add_test(NAME feeder-seg-e2e COMMAND lift-feeder-seg-e2e
        $<TARGET_FILE:emulator-lift>
        "${LIFT_STRESS_GAME_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/feeder-seg-e2e-work"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../graphs/se-eof.graph")
    set_tests_properties(feeder-seg-e2e PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        SKIP_RETURN_CODE 77
        TIMEOUT 900)

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
endif()

# ---- LIFT_STRESS 档：真机安装上的 LOGO 影片压力面（首帧 / completed / 堆平稳 / tex#1 差分）----
# 判据见 tests/movie_logo.cpp 的头注释。它要真机安装根（输入假定在场；缺 ⇒ 测试自己 SKIP 77）；
# GPU 不可用（raster 行不是 sdl-gpu）⇒ 测试自己 `LIFT_SKIP`(77)。
# ★ **opt-in 的理由是用途**：这条要真启模拟器、真放 LOGO 影片（子进程按**墙钟秒**计），是**全部用例里最贵**的
#   一条 ⇒ 默认门禁**不注册**它，必须显式打开（判据 = 条件里的 `DEFINED ENV{LIFT_STRESS}`）：
#     $env:LIFT_STRESS=1; ctest --test-dir out -R movie-logo --output-on-failure
#   ⛔ `DEFINED ENV{…}` 是**配置期**求值 ⇒ 改了环境变量必须重新 configure
#   （`cmake --preset windows-x64`），`ctest -N` 才会跟着变。
# ★ `add_executable` 也在同一个 if 里：没开 `LIFT_STRESS` 时连这个 target 都不编。
# ★ 不改测试名与文件路径（需求单 `01M6T07SC0XN81G1MZJYJXXGV9` 的 verify 指向它们）。
if(DEFINED ENV{LIFT_STRESS})
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
        "${LIFT_STRESS_GAME_DIR}"
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
# LIFT_STRESS：**压力/效率四场景（MVP，2026-11-14）** —— S1 / S2 / S3 / S9
# ★ 判据 = **读数 + 阈值**（帧时间 / 指令吞吐 / 堆字节 / 音频计数 / GPU 准入拒批计数）；
#   ⛔ 没有一条业务断言（"到了某界面 / 某 marker 出现 / 某帧 hash 等于 X" 一律不进判据）。
#   设计依据 `.tmp/test-model/stress-design.md` 的 §0.1（判据纪律）· §0.2（R1–R5 派生规则）·
#   §A1.0（口径 W / H · N 遍取中位 · 每次全新 `--appdata`）· §A2（场景）。
# ★ **每条阈值的推导写在对应 `tests/stress_*.cpp` 的文件头**：规则编号 + 依据读数 + 算出来的值。
#   ⛔ 不许先写一个好看的数字再让实现去凑（先测 — 再钉）。
# ★ 口径分两套、参数**不许混写**：`S1`/`S9` = **口径 H**（`--present headless`，虚拟时钟、确定性）；
#   `S2`/`S3` = **口径 W**（`--present window --present-hidden --present-vsync 0`，real-time 墙钟）。
#   `--present headless` 拒绝窗口参数（`src/main.cpp:639`）⇒ 混写会 exit 2。
# ★ 驱动 ≠ 判据：`scripts/bench/idle.txt` / `graphs/sn0000.graph` 只把画面推着走；
#   驱动失败（exit 5 / `FAIL :`）**不进判据**，只记进 JSON 的 `driver` 段。
# ★ 落盘：仓库根 `.cache/lift-stress/<场景>-<UTC 时间戳>.json`（逐遍原始值 + 中位 + exe sha256 +
#   `CMAKE_BUILD_TYPE` + 工作树指纹 + 负载档）。⛔ 不进 git（`.cache/` 已 gitignore）。
# ★ 为什么这四条只在 `LIFT_STRESS`：它们都跑**真游戏**（唯一允许跑真游戏的一档，`AGENTS.md` §5），
#   且每条 N=3 遍（S1 ~15 s / S2 ~150 s / S3 ~160 s / S9 ~470 s）。
# ★ 输入假定在场：显式开档而输入 MISSING ⇒ 上面的 `FATAL_ERROR` 已经挡过；测试自己也会 `LIFT_SKIP`(77)
#   （没有 GPU / 没有真音频设备 / 非 Windows 或非 RelWithDebInfo 树）—— ⛔ 不假绿、也⛔ 不静默缩水。
# =====================================================================================
if(DEFINED ENV{LIFT_STRESS})
    # 读数落点（仓库根，`out/` 之外 ⇒ 不进 git、也不会被 `cmake --build` 清掉）。
    set(LIFT_STRESS_JSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../.cache/lift-stress")

    # ---- S1：boot → TITLE 首帧（口径 H）。判据见 tests/stress_boot.cpp 文件头 ①②③④⑤ ----
    add_executable(lift-stress-boot tests/stress_boot.cpp)
    target_link_libraries(lift-stress-boot PRIVATE emulator-lift-runtime)
    add_test(NAME stress-boot COMMAND lift-stress-boot $<TARGET_FILE:emulator-lift>
        "${LIFT_STRESS_GAME_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/stress-boot-work"
        "${LIFT_STRESS_JSON_DIR}"
        "$<CONFIG>")
    set_tests_properties(stress-boot PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        SKIP_RETURN_CODE 77
        # 3 遍 × 实测 ~5 s + 进程启停余量（⛔ 不要压到接近实测值：超时会变成假红）。
        TIMEOUT 300)

    # ---- S2：TITLE 稳态帧时间（口径 W）。判据见 tests/stress_title_steady.cpp 文件头 ①②③④⑤ ----
    # ★ `LIFT_FRAME_TRACE=1` 由 **CTest 的 ENVIRONMENT** 注入（同 movie-logo 的 `LIFT_STOP_AFTER_MOVIE`）：
    #   测试自己再断言"真跑出 ≥2 条 n≥100 的 `[frame]` 行"⇒ 环境变量被删掉时**变红**，⛔ 不会静默变绿。
    add_executable(lift-stress-title-steady tests/stress_title_steady.cpp)
    target_link_libraries(lift-stress-title-steady PRIVATE emulator-lift-runtime)
    add_test(NAME stress-title-steady COMMAND lift-stress-title-steady $<TARGET_FILE:emulator-lift>
        "${LIFT_STRESS_GAME_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/stress-title-steady-work"
        "${LIFT_STRESS_JSON_DIR}"
        "$<CONFIG>"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/bench/idle.txt")
    set_tests_properties(stress-title-steady PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        SKIP_RETURN_CODE 77
        ENVIRONMENT "LIFT_FRAME_TRACE=1"
        # 3 遍 × 实测 ~49 s（脚本窗口 200M 条 + boot）⇒ 留 ~4× 余量。
        TIMEOUT 600)

    # ---- S3：ADV 一段 + 真音频（口径 W + `--audio sdl`）。判据见 tests/stress_adv_audio.cpp ①②③④⑤ ----
    # ★ 驱动 = 仓里**现成**的 `graphs/sn0000.graph`（⛔ 不新编坐标；图的判定结果不进判据）。
    add_executable(lift-stress-adv-audio tests/stress_adv_audio.cpp)
    target_link_libraries(lift-stress-adv-audio PRIVATE emulator-lift-runtime)
    add_test(NAME stress-adv-audio COMMAND lift-stress-adv-audio $<TARGET_FILE:emulator-lift>
        "${LIFT_STRESS_GAME_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/stress-adv-audio-work"
        "${LIFT_STRESS_JSON_DIR}"
        "$<CONFIG>"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../graphs/sn0000.graph")
    set_tests_properties(stress-adv-audio PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        SKIP_RETURN_CODE 77
        ENVIRONMENT "LIFT_FRAME_TRACE=1"
        # 3 遍 × 实测 ~52 s（图的时间线 ~44 s + boot）⇒ 留 ~5× 余量。
        TIMEOUT 900)

    # ---- S9：GPU 覆盖读数（口径 H，600 帧）。判据见 tests/stress_gpu_coverage.cpp 文件头 ①②③④⑤⑥ ----
    # ★ 这一条最贵（单遍实测 ~155 s）但最重要：GPU-only 之后每一处准入拒批都是**潜在缺画**。
    add_executable(lift-stress-gpu-coverage tests/stress_gpu_coverage.cpp)
    target_link_libraries(lift-stress-gpu-coverage PRIVATE emulator-lift-runtime)
    add_test(NAME stress-gpu-coverage COMMAND lift-stress-gpu-coverage $<TARGET_FILE:emulator-lift>
        "${LIFT_STRESS_GAME_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/stress-gpu-coverage-work"
        "${LIFT_STRESS_JSON_DIR}"
        "$<CONFIG>")
    set_tests_properties(stress-gpu-coverage PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        SKIP_RETURN_CODE 77
        # 3 遍 × 实测 124–155 s（本批两次真跑：373.8 s / 381.0 s 整条）⇒ 留 ~3× 余量
        #（这条最容易在慢机上超时，⛔ 不要压小）。
        TIMEOUT 1200)
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

# ---- 2026-11-14：`interaction-graph`（`tests/interaction_graph.cpp`）已按测试模型删除 ----
# 判据是"交互图走通 / 等一屏 → 点一下"这类**业务正确性**（测试模型单
# `REQ-01M7ATAB5A18W7XBCKFBEQS7HB` 点名取消）；它**不进测试**。复现命令见
# `docs/03-emulator/defects.md` BUG-16。

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
# ★ 顶层无条件注册 ⇒ pure 档（`AGENTS.md` §5：判据是注册位置）。
#   这条同时承载后缓冲 LockRect 的屏障用例（`caseBackBufferLockBarrier`）—— 只注册 `--gpu` 那条的话，
#   在无 GPU 机器上它会 SKIP 77 ⇒ "删屏障"的变异静默逃逸。
add_test(NAME gpu-batch-order COMMAND lift-gpu-batch)

# ---- 同一份夹具的真后端判据：Clear / 纹理 LockRect / SetRenderTarget 之后像素块对、队列空 ----
# dummy 视频下建不出 SDL_GPU 设备 ⇒ SKIP 77（⛔ 不拿 pure 档冒充真后端绿）。
add_test(NAME gpu-batch-barrier COMMAND lift-gpu-batch --gpu)
set_tests_properties(gpu-batch-barrier PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy" SKIP_RETURN_CODE 77)

# ---- pure：**默认 stdout 降噪**（`REQ-01M7AG06MXS6PK2B53B29QM76Z`）----
# 判据见 `tests/gpu_batch.cpp` 的 `caseStdoutQuiet` 头注释：默认档 ≤25 行 / 无 `  缺口 ` 明细 /
# 无 native-override per-entry / `stop`·`detail` 在前 10 行；`--report` 臂保留缺口明细；
# `--profile` 臂保留 per-entry。两臂成对 ⇒ "删掉"不能冒充"折叠"。
# ★ 执行源是测试自造的最小 PE + 假 `.lst` ⇒ 不碰 LFS / 游戏安装 ⇒ 顶层注册 = pure 档。
# ★ 默认 / `--profile` 臂要真 GPU（headless 也建隐藏窗口）⇒ 无 GPU 时 SKIP 77。
add_test(NAME stdout-quiet COMMAND lift-gpu-batch --stdout-quiet $<TARGET_FILE:emulator-lift>)
set_tests_properties(stdout-quiet PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300)

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
#   `base-coverage`（导入覆盖普查）已删 —— 改由 `emulator-lift --report` 的缺口清单承担。
# =====================================================================================

# ---- 默认：执行源（拆壳件）的真实 PE 载入 ----
add_executable(lift-loader tests/loader.cpp)
target_link_libraries(lift-loader PRIVATE emulator-lift-runtime)
add_test(NAME loader COMMAND lift-loader)
set_tests_properties(loader PROPERTIES
    ENVIRONMENT "LIFT_AGE_EXE=${AGE_SECTFIX_EXE};LIFT_AGE_LST=${AGE_LST}" SKIP_RETURN_CODE 77)

# ---- 2026-11-14：`base-coverage`（`tests/base_coverage.cpp`，A5 导入覆盖普查）已按测试模型删除 ----
# 判据是"跑到 L2 + 动态缺口为空"的业务结论；缺口**普查**改由 `emulator-lift --report` 的
# 缺口清单承担（⛔ 不新写工具），复现命令见 `docs/03-emulator/observations.md`。

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
# `--dump-frame` 的目录必须在开跑前建好（`REQ-01M73YBCC3DZ05Y9NSDTRKTMMY`）
# ★ 纯追加块：不改上面任何一行（并发编辑协议：另一个代理同时在改本文件）。
# ★ 判据见 tests/cli_dump_frame.cpp 头注释：
#   ① 不存在的目录 ⇒ 子进程 exit 0 **且目录真的建出来**；
#   ② 建不出来（中间被普通文件挡住）⇒ exit 2 + stderr 说清是 `--dump-frame` / 目录；
#   ③ 两条子进程都在 30 s 内结束（旧实现的缺陷形态 = 目录不存在时空转 546 s）。
# ★ 顶层无条件注册 ⇒ **pure** 档：执行源是测试自己构造的最小 PE32 + `--max-insns 1`
#   （⛔ 不碰 LFS / 游戏安装 / 窗口；与 cli-time-limit 同一口径）。
# ★ `tools/mutate-check.mjs` 的 `ctest:cli-dump-frame` 指向它（去掉 create_directories ⇒ 当场红）。
# =====================================================================================
add_executable(lift-cli-dump-frame tests/cli_dump_frame.cpp)
target_link_libraries(lift-cli-dump-frame PRIVATE emulator-lift-runtime)
add_test(NAME cli-dump-frame COMMAND lift-cli-dump-frame $<TARGET_FILE:emulator-lift>)
set_tests_properties(cli-dump-frame PROPERTIES SKIP_RETURN_CODE 77)

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
        COMMENT "lift-ir-proto: tests/ir/lift_436D00.ll -> .obj（clang -c -O2，无 llc）")
    set(LIFT_IR_SRC "${LIFT_IR_OBJ}")
else()
    message(STATUS "lift-ir-proto: 没找到 clang ⇒ 链 tests/ir/lift_stub.cpp（测试体 SKIP 77）")
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
        COMMENT "lift-ir-shim: tests/ir/lift_4D4B40.ll -> .obj（clang -c -O2，无 llc）")
    set(LIFT_IR_SHIM_SRC "${LIFT_IR_OBJ}" "${LIFT_IR_SHIM_OBJ}")
else()
    message(STATUS "lift-ir-shim: 没找到 clang ⇒ 链 tests/ir/lift_stub.cpp + 测试体内的 4D4B40 替身（SKIP 77）")
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
        COMMENT "lift-gen: roots + PE -> out/aot/golden_gen.ll（T1a；不 commit）"
        VERBATIM)
    add_custom_command(OUTPUT "${LIFT_GEN_DIR}/golden_gen.obj"
        COMMAND ${LIFT_IR_CLANG} -c -O2 ${LIFT_IR_TARGET_FLAGS} -Wno-override-module
                -o "${LIFT_GEN_DIR}/golden_gen.obj" "${LIFT_GEN_DIR}/golden_gen.ll"
        DEPENDS "${LIFT_GEN_DIR}/golden_gen.ll"
        COMMENT "lift-gen: golden_gen.ll -> .obj（clang -c -O2，无 llc）")
    set(LIFT_GEN_SRC "${LIFT_GEN_DIR}/golden_gen.obj")
    # ★ T1c：生成物在块边界发 `@lift_tick`（D2）⇒ lifted 臂与解释器**同一口径**记账；
    #   手写 golden（`tests/ir/lift_*.ll`）没有 tick ⇒ 两份测试体用这个宏区分"期望的 lifted 侧条数"。
    set(LIFT_GEN_DEFINE LIFT_IR_CLANG=1 LIFT_GEN_TICKS=1)
else()
    message(STATUS
        "lift-gen goldens: 缺 clang 或执行源（${LIFT_GEN_AGE_EXE}）⇒ 链 tests/ir/lift_stub.cpp（用例 SKIP 77）")
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
        COMMENT "lift-x87-diff: lift_aot.ll -> .obj（clang -c -O2）")
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
    message(STATUS "lift-x87-diff: 缺 clang 或执行源（${LIFT_GEN_AGE_EXE}）⇒ 不链生成物（用例 SKIP 77）")
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
        COMMENT "lift-ops-diff: lift_aot.ll -> .obj（clang -c -O2）")
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
    message(STATUS "lift-ops-diff: 缺 clang 或执行源（${LIFT_GEN_AGE_EXE}）⇒ 不链生成物（用例 SKIP 77）")
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
        COMMENT "lift-s7-structured: lift_aot.ll -> .obj（clang -c -O2）")
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
    message(STATUS "lift-s7-structured: 缺 clang 或执行源（${LIFT_GEN_AGE_EXE}）⇒ 不链生成物（用例 SKIP 77）")
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

endif() # LIFT_BUILD_TESTS
