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
# ★ 渲染原语：数值文本 `0x205`（格式化 / 三个对齐档 / 全角化 / 奇数格宽）与清屏 `0x20E`
#   （目标是纹理槽 ⇒ 清该表面；后缓冲无可写表面；⛔ 不清非目标纹理 / 不清纹理表）。
#   交接单第 3 项的两个空档；把格式位反过来、把格宽先除半、或让 `0x20E` 提前 return ⇒ 本条红。
add_test(NAME emulator-core-render-prims COMMAND emulator-core-selftest --only-render-prims)
# ★ 两套字号（`0x75` = A 支 / `0x197` = B 支）单独一条：spec 的 `CORE-OPS-FORWARD-PROLOGUE` 证据指这里。
add_test(NAME emulator-core-font-size COMMAND emulator-core-selftest --only-font)
# ★ 字形 AA 档（1 位 / GRAY4 / GRAY8 的**选择**与 17 / 65 级换算 ⇒ 满覆盖 240 / 251）：
#   "设置界面文字比真机亮一档"的算术那一半。拆回 2^n（16 / 64）⇒ 满覆盖 255 ⇒ 本条红。
add_test(NAME emulator-core-glyph-alpha COMMAND emulator-core-selftest --only-glyph)
# ★ 玩家配置的根（**本地**应用数据，⛔ 不是漫游）+ 退时保存的宿主闸（真实玩家配置默认不写）。
#   把根换回 `%APPDATA%` 或去掉保存闸 ⇒ 本条红。
add_test(NAME emulator-core-config-root COMMAND emulator-core-selftest --only-config)
# ★ 逐字符显现（typewriter）：追加那一步只写布局格（条目已排、画布仍全透明），绘制侧**每帧推进一格**。
#   把"追加即全画"写回去 ⇒ 本条红。
add_test(NAME emulator-core-adv-typewriter COMMAND emulator-core-selftest --only-typewriter)
# ★ 振り仮名的横向定位：窄假名 × 宽格 ⇒ 两端各留一份步进、假名等间距；宽假名 × 窄格 ⇒ 以格中心对齐。
#   退回"左对齐 + 整串一个条目" ⇒ 本条红。
add_test(NAME emulator-core-adv-ruby COMMAND emulator-core-selftest --only-ruby)
# ★ 抽干：宿主报"有输入在按下" ⇒ 剩余待画格**同一帧**画完（无输入时仍然一帧一格）。
#   把抽干写成无条件 ⇒ "一帧画了不止一个" 那条红；把抽干去掉 ⇒ "没有抽干" 那条红。
add_test(NAME emulator-core-adv-drain COMMAND emulator-core-selftest --only-drain)
# ★ 打字节流：按每字间隔（`Font+0x560`）**每 D 毫秒一格、每帧最多一格**；间隔 = 0 ⇒ 追加时立即抽干。
#   退回"每帧一格" ⇒ 间隔 100ms 的用例在头 5 帧就画字 ⇒ 本条红。
add_test(NAME emulator-core-adv-pacing COMMAND emulator-core-selftest --only-pacing)
# ★ 文本记录的**字号支**：位 2 只在振り仮名 run（及层模式 = 1 的逐字支）的 flags 上置起；消费口径只有
#   `record_font_size` 一处（位 2 ⇒ B 支、否则 A 支）。主线 run 置位 / 振り仮名不置位 ⇒ 本条红。
add_test(NAME emulator-core-text-record-flags COMMAND emulator-core-selftest --only-record-flags)
# ★ 文本落点可以是指针族（`0x192` 的 tag 8 / 14）：引擎落点分派（EA 0x433310）接受 5 / 8 / 11 / 14，
#   四条汇到同一个记录级写入（EA 0x43362F）。tag 14 = 槽里装**记录地址** ⇒ 解引用后写那条记录。
#   退回"只认 5 / 11" ⇒ 本条红（实测：TITLE / GAME START 的 `0x192` 直接 halt）。
#   同时守 halt 帧链报告的字段（script / ip / file / opcode）。
add_test(NAME emulator-core-text-pointer-write COMMAND emulator-core-selftest --only-text-pointer)
# ★ `0x300` 的**循环演示闸**：设置界面下方那段 ADV 示例文本要"逐格揭示 → 停「期限」→ 清绘制条目 →
#   从第一格重来"。旧实现只把 0x300/0x301 写成两张无人读的 map ⇒ 只展示一次（用户口径"不会刷新"）。
#   `0x301` 只清条目 + 游标，不清画布像素、不删字符格。
add_test(NAME emulator-core-text-gate-replay COMMAND emulator-core-selftest --only-text-gate)
# ★ 文本比较族（`0x194` 相等 / `0x195` 不等）：同一对操作数上两条必须**互补**（1/0 互换），
#   都把推进格数写成 7（EA 0x42CF4F / 0x42D04F），落点给文本族标签必须抛。
#   0x195 是 SETFATE.BIN（新游戏第 0 章初始化）的第一条缺失指令 ⇒ 方向写反 ⇒ 本条红。
add_test(NAME emulator-core-text-compare-ops COMMAND emulator-core-selftest --only-text-compare)
# ★ `0x229`（场景自身变换）：先**重置**再写「作用键区间 + 三元平移」，消费者只对**键落在区间内**的
#   绘制项加 X / Y 平移。新游戏返回路径的第一条就是 `0x229 0,0,0,0,0`（区间空 ⇒ 不动任何像素）——
#   退回"不实现" ⇒ 本条红（该路径直接 halt）。
add_test(NAME emulator-core-scene-self-transform COMMAND emulator-core-selftest --only-scene-self)
# ★ `0x258` / `0x259`（1000 项表的两项标志）：按位掩码写「位 0 / 位 1」，`0x259` 全清 1000 项，
#   **不置脏**。第 0 章剧本（SC0000.BIN）开头就是 `0x259` + 一大串 `0x258 i 1` ⇒ 不实现就 halt。
add_test(NAME emulator-core-scene-table-flags COMMAND emulator-core-selftest --only-scene-table)
# ★ `0x90`（选项追加）+ `0x97`（按矩形写附加整数）：第 0 章剧本开场就是「追加一条选项 + 逐个设附加整数」，
#   不实现就 halt。容量门槛是 100（不是引擎错误文本写的 20），满表必须响亮失败。
add_test(NAME emulator-core-adv-choice-add COMMAND emulator-core-selftest --only-adv-choice)
# ★ 选项窗口（`0x93` 清表+关窗 / `0x94` 开窗 / `0x91`·`0x92` 等待）：**推进格数 = 0 是唯一的阻塞手段**，
#   0x91/0x92 未拿到闩锁时把格数改回 0（下一拍重跑同一条），拿到才推进 3 / 5。每个场景的选项窗都要过 0x93/0x94。
add_test(NAME emulator-core-adv-choice-window COMMAND emulator-core-selftest --only-choice-window)
# ★ `0x1bc`（语音全部停止）：后端已挂 ⇒ 声道 12/13/14 各一次停止请求；未挂 ⇒ 0 次。两支都要清 12 格 + 「正在播」。
add_test(NAME emulator-core-voice-all-stop COMMAND emulator-core-selftest --only-voice-all-stop)
# ★ 0x1b1 / 0x238 / 0x32a / 0x32d：写固定格 / 清两格 / 销毁效果槽 / 场景颜色请求（透明度夹 255、每次执行都发）。
add_test(NAME emulator-core-misc-cells COMMAND emulator-core-selftest --only-misc-cells)
# ★ `0x214`（场景记录的交换 / 搬运）：两个键都有记录 ⇒ **交换**；只有一个 ⇒ 搬到另一边；两个都没有 ⇒
#   不建记录但**仍标脏**（引擎三条出口都写 B5AC←1）。读序 2→1（0x423AFB push 2 先读）。退回"只搬 SceneRecord
#   或只搬一边" ⇒ 本条红。
add_test(NAME emulator-core-scene-relocate COMMAND emulator-core-selftest --only-scene-relocate)
# ★ `0x304` / `0x305`（消息块）：`0x6e` / `0x196` 成功追加时置臂位第 16 位（0x41EB7D / 0x41FCD8）；武装后
#   0x305 回卷到块开头、揭示全部、写等待三格 (1,0,0)、清臂位；未武装 / 空串只清臂位。不置 bit16 ⇒ 本条红。
add_test(NAME emulator-core-msg-block COMMAND emulator-core-selftest --only-msg-block)
# ★ `0x1fb` / `0x219` / `0x220` / `0x242`（场景记录的两半）：引擎的 +0x24..+0x2C / 窗位 / +0x2D0 在模型里
#   被拆成 SceneRecord 与 DrawItem；消费者读 DrawItem。只写一半 ⇒ 本条红（窗不生效 / 0x21a 读回 0）。
add_test(NAME emulator-core-scene-records COMMAND emulator-core-selftest --only-scene-records)
# ★ 0x193 字符串拼接 / 0x2e9 固定格 / 0x2f7 语音选项格 / 0x32 纹理缩放拷贝 / 0x10a 光标请求 / 0x20a 重绘：
#   复审点名的零 CTest opcode 各一条最小行为断言。
add_test(NAME emulator-core-misc-ops COMMAND emulator-core-selftest --only-misc-ops)
# ★ 文本层的**键基数兜底**：本作 ADV 的消息层**不调** `0x213`（残差 `ADV-TEXT-LAYER-DRAW-KEY`）⇒
#   `0x70` 必须补宿主基数，绘制条目必须真的出现。把"脚本写过 `0x213`"当"有键基数"⇒ 消息层
#   一个字都不显示（表现为"点一下屏幕什么都不变"）⇒ 本条红。
add_test(NAME emulator-core-text-layer-fallback COMMAND emulator-core-selftest --only-text-layer-fallback)
# ★ `0x72` 的「等推进」闸：门位置起后**一条指令都不派发**；只有**左键**（位 0）放行，右键不算；
#   放行的那一步就要执行下一条。放行看错位 / 提前派发 ⇒ 本条红。
add_test(NAME emulator-core-adv-wait COMMAND emulator-core-selftest --only-adv-wait)
# ★ `0x32` 缩放拷贝：放大 = **双线性**（4 邻域加权 +0.5 截断）、缩小 = **面积加权平均**
#   （0x4A3FA6/0x4A3FCB 求 sx、0x4A3FE3 截断、0x4A400C 夹索引、0x49EE00 加权；缩小 0x4A42C0）。
#   退回最近邻 ⇒ (1,1) 会是源 (0,0)、本条红。
add_test(NAME emulator-core-texture-stretch COMMAND emulator-core-selftest --only-texture-stretch)
# ★ 自动消息：`0x1b7` 开启后 `0x72` 武装计时器，到期**放行同一对位**（不换行、不取下一行）；
#   时长 = max(100, Time1 + Pitch1 × (N − 1 − 0x2e9 的格))。不武装 / 永不放行 / Pitch 不进公式 ⇒ 本条红。
add_test(NAME emulator-core-auto-message COMMAND emulator-core-selftest --only-auto-message)
# ★ 选项消费侧：窗口开着 + 左键 + 命中带决定入口的条 ⇒ 执行位置 ← 该入口（jump）、清表、关窗。
#   不跳 / 不关窗 / 不清表 ⇒ 本条红。★ 只验机制，"何时消费"的次序仍是残差。
add_test(NAME emulator-core-choice-consumer COMMAND emulator-core-selftest --only-choice-consumer)
# ★ `0x21c` 谓词 A：`0x238` 的"起点+时长"窗内挡住（相等仍算窗内）、到期放行；`0x243` 中止后立刻放行。
#   不看这一对格（比如只按"没有待画格"放行）⇒ 本条红。
add_test(NAME emulator-core-wait-timer COMMAND emulator-core-selftest --only-wait-timer)

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
    # ★ 路由（`data/routes/screens.json` 的**可执行**出口）：每条 = 一段脚本化点击时间线 +
    #   必须到达的脚本 id。它们以前只被"元数据校验"覆盖，**从没真跑过** —— 于是"点击丢了"
    #   这类问题不会被任何门禁发现（实测：等待位被颜色窗卡住时，本组会红在 chapter0 上）。
    add_test(NAME emulator-core-route-title
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}" --skip-logo --play-headless
                --input-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/input/route-title.txt"
                --anchor-script 21092 --expect-script 21092 --tick-ms 16 --max-steps 200000)
    add_test(NAME emulator-core-route-gamestart
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}" --skip-logo --play-headless
                --input-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/input/route-gamestart.txt"
                --anchor-script 21092 --expect-script 21099 --tick-ms 16 --max-steps 400000)
    add_test(NAME emulator-core-route-gamestart-newgame
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}" --skip-logo --play-headless
                --input-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/input/route-gamestart-newgame.txt"
                --anchor-script 21092 --expect-script 20964 --tick-ms 16 --max-steps 400000)
    # ★ 点击节奏：同一段时间线里 80 次点击（每 300 ms、按住 100 ms）必须推进 ≥ 60 次正文。
    #   守的是"点击大量被吞"——等待位被颜色窗卡住时只会推进十几次（实测 13）。阈值在
    #   tests/adv_click_rate.cmake 里，日志留在构建目录。
    add_test(NAME emulator-core-adv-click-rate
        COMMAND ${CMAKE_COMMAND}
            -DEXE=$<TARGET_FILE:emulator-cxx>
            -DINSTALL=${EMULATOR_CXX_INSTALL_DIR}
            -DSRC=${CMAKE_CURRENT_SOURCE_DIR}/tests
            -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/adv_click_rate.cmake)
    add_test(NAME emulator-core-route-newgame-chapter0
        COMMAND emulator-cxx --install "${EMULATOR_CXX_INSTALL_DIR}" --skip-logo --play-headless
                --input-script "${CMAKE_CURRENT_SOURCE_DIR}/tests/input/route-newgame-chapter0.txt"
                --anchor-script 21092 --expect-script 115 --tick-ms 16 --max-steps 600000)
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

