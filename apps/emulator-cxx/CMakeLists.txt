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
# ★ `0x2bf`（音效通道登记）：活动 / 起点 ← 0 / 时长 / 交值 / 待办 五格逐项，通道 ≥10 一格不写，
#   交付两次（未到 / 到期）⇒ 到期交出交值并清活动；0x2bf → 0xb6 清活动而不动另三格。
#   登记时写错字段（通道 / 交值 / 时长）/ 不置待办 / 0xb6 只清表不清活动 ⇒ 本条红。
add_test(NAME spec_se_registration_2bf COMMAND emulator-core-selftest --only-se-registration)
# ★ `0xb6`（按通道停止）：后端不存在（接缝缺省）⇒ 不等于"照常清格"：两格原样、不发请求；
#   接上后端 ⇒ 先请求、再无条件清「播放名」格与活动；起点 / 时长 / 交值不动。
#   没有后端前置（总是清）/ 清了表却没清活动 ⇒ 本条红。
add_test(NAME spec_se_stop_b6 COMMAND emulator-core-selftest --only-se-stop)
# ★ `0x143` 的投递源 = **文件库的 256 格槽位表**（不是 0x143 私有的登记表）：装载按文件自带的号登记，
#   执行时逐槽（1..255）投 `号 << 24`；文件库析构逐槽清 0 之后不再投。
#   不登记装载（旧实现：产品路径恒 0）/ 循环从 0 起 / 析构不清 ⇒ 本条红。
add_test(NAME spec_event_143_file_library COMMAND emulator-core-selftest --only-event-library)
# ★ 输出对象三项（`45-objects`）：追加原样 / 按位置追加加 `(<脚本名>：<行号>行目) ` 前缀 /
#   报告写 `\r\n致命的エラー[文本]\r\n` 再交根对象的错误处理（窗口句柄那一项）。
#   报告只进 `errors_`（错误通道）而不发日志 seam / 前缀拼错 ⇒ 本条红。
add_test(NAME spec_output_object_report COMMAND emulator-core-selftest --only-output-object)
# ★ 渲染原语：数值文本 `0x205`（格式化 / 三个对齐档 / 全角化 / 奇数格宽）与清屏 `0x20E`
#   （目标是纹理槽 ⇒ 清该表面；后缓冲无可写表面；⛔ 不清非目标纹理 / 不清纹理表）。
#   交接单第 3 项的两个空档；把格式位反过来、把格宽先除半、或让 `0x20E` 提前 return ⇒ 本条红。
add_test(NAME emulator-core-render-prims COMMAND emulator-core-selftest --only-render-prims)
# ★ 两套字号（`0x75` = A 支 / `0x197` = B 支）单独一条：spec 的 `CORE-OPS-FORWARD-PROLOGUE` 证据指这里。
add_test(NAME emulator-core-font-size COMMAND emulator-core-selftest --only-font)
# ★ `0x1a5` / `0x2fe` 的**两对描述**（`CORE-OPS-FORWARD-PROLOGUE`）：`0x1a5` 只写 A 对、`0x2fe` 只写 B 对；
#   每对两份变体（名原样 / '@' + 名）、高 ← −本支字号、下一格 ← −(本支字号/2)，且各请求一次重建字体。
#   把两条并成同一个 case（都写 A 对）⇒ 本条红。
add_test(NAME spec_ops_1a5_2fe_font_pairs COMMAND emulator-core-selftest --only-font-pairs)
# ★ 建字体缩放（`60-ops-forward`「字号是"未缩放"的量」）：比例只在窗口态 ∧ 虚拟全屏类型 = 2 时按
#   视口 ÷ 配置客户区重算，其余 1.0；建字体高度 = trunc(高 × 比例 + 0.5)（向零截断）；十槽两格被抄；
#   两个分量都相等 ⇒ 整趟不写。改四舍五入 / 忽略比例 / 去掉相等短路 ⇒ 本条红。
add_test(NAME spec_font_build_scale COMMAND emulator-core-selftest --only-font-scale)
# ★ `0x10c` 的**三张表**（`CORE-OPS-SCALAR-TABLE-10C`）：键→动作不碰、动作→槽只读、槽→键写；
#   值按无符号 > 31 抛 `SetKeyMultiの引数が不正です．`（全角句点、**无**回车换行）且不改状态；读序 2 → 1。
#   写 `indirect_[动作]`（旧实现）/ 用有符号比较 / 先读第 1 个 ⇒ 本条红。
add_test(NAME spec_ops_10c_key_tables COMMAND emulator-core-selftest --only-key-tables)
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
# ★ `0x21c` 谓词 A 的**「待处理」闩**（引擎 `+0B5B4`）：`0x202` 排窗置 1、窗推进时每帧再置 1、
#   渲染步入（`0x4B40B0` ⇒ `end_render()`）清 0。判据：`0x1fb` → `0x202 delay 0 dur 200` → `0x21c`，
#   等待位 `0x400` 在 `start + 199` 仍置起、**`start + 200`（相等）就放行** —— 窗的到期判据是
#   `现在 >= 起点 + 延迟 + 持续时间`（引擎 `0x4B17F9` 的 `jge`：**相等已到期**），⛔ 不是 `start + 201`。
#   把闩写回恒 false（旧模型：放行只看"计时器 + 待画格 / 打字态"）⇒ 下一拍就放行 ⇒ 本条红。
#   ★ 它同时是 TITLE 的 mesh 200000 在**任何一帧渲染前**被 `0x1f7` 拆掉的成因。
add_test(NAME emulator-core-wait-colour-window COMMAND emulator-core-selftest --only-wait-colour-window)
# ★ `0x1ce` 的领域身份是「**逐格揭示当前文本**」（`CORE-OPS-TEXT-CELL-REVEAL-1CE`），不是乱数启用：
#   非 0 ⇒ 活动位（第 30 位）+ 格号归 0 + 节拍器相位重置（起点 = 本迭代宿主毫秒时钟）；0 ⇒ 只在活动位
#   已置时清位，**格号与相位都不复位**；主循环每拍**只画一格**、格号 ← (格号+1) mod 格数，格数 0 不推进。
#   ★ 每条的判据都要**会红**：格号归 0 / 相位重置 / 停用不动格号相位 / 取模 / 格数 0 不推进之外，
#   还咬住「请求画的是**推进前**的格号」（`text_cell_last_draw_cursor()`）、**掉拍不补**（跨 3 拍也只画一格）、
#   「开关格**两条支都写**」（停用后必须读到 0）、「节拍周期**不被本条复位**」（播种 300 后仍是 300）、
#   以及 `0x72` 尾段的四个写点（格数 / 格号 / 相位起点 / 节拍计数——删掉任何一行本条红）。
add_test(NAME emulator-core-text-cell-reveal COMMAND emulator-core-selftest --only-cell-reveal)
# ★ 位号越界 = **错误通道**，不是异常（`CORE-OPS-BIT-SET-RESET`，锚 `0x42F8DA` / `0x42F94A` 的
#   `cmp edi,1Fh` + `ja`）：目标预置 −1，位号 32 与 −1 ⇒ 目标仍是 −1、不抛、错误通道恰一条固定文本
#   （含 CRLF）；位号 31 的对照必须真的置位 / 清位。把上界 1Fh 放宽、或删掉错误文本 ⇒ 本条红。
add_test(NAME spec_ops_136_rembit_arg_invalid COMMAND emulator-core-selftest --only-rembit-arg-invalid)
add_test(NAME spec_ops_135_setbit_arg_invalid COMMAND emulator-core-selftest --only-setbit-arg-invalid)
# ★ `0x13f`（`CORE-OPS-BIT-TEST`）与上面两条**同形**：位号 32 / −1 ⇒ 落点保持原值（不写 0）、
#   **源也不被读**（用例把源写成"读整型必抛"的类型 2 ⇒ 读一次就停）、错误通道恰一条
#   `GetBitの引数が不正です．` + CRLF；界内对照把第 30 位读成 1、第 0 位读成 0（归一，不是原值）。
#   去掉错误文本 / 让越界支读源 ⇒ 本条红。
add_test(NAME spec_ops_13f_getbit_arg_invalid COMMAND emulator-core-selftest --only-getbit-arg-invalid)
# ★ `0x60`（`CORE-OPS-RANDOM-60`）范围 = 0：落点**先**写 0、**随后**抛显示消息型异常，
#   文本逐字 `RANDOMの最大値が0になっています．`、随机数恰取一次（不回退）；范围 = −1 **不抛**。
#   删掉固定文本 / 不写 0 / 把判据写成 ≤ 0 ⇒ 本条红。
add_test(NAME spec_ops_60_random_range_zero COMMAND emulator-core-selftest --only-random-range-zero)
# ★ `0x64`（`CORE-OPS-ARRAY-COPY`）块越界 = spec 的**未定义行为**（不作断言）⇒ 本模型选择
#   **响亮失败 + 诊断**（`array-block-past-code`）：块号越界 / 为负 / count 越过代码区末尾 ⇒ 停机且
#   目标一格都不写；合法块（count = 3，数据 0 / 1 / −1）必须照常复制。改成静默 / 夹紧 ⇒ 本条红。
add_test(NAME spec_ops_64_array_block_bounds COMMAND emulator-core-selftest --only-array-block-bounds)
# ★ 数组**写区间**的实现侧边界（`RES-ARRAY-FILL-ZERO-OOB` / `-SET-TO-OOB` / `-ARRAY-COPY-OOB` 的
#   impact：「实现必须拒绝或先记诊断再拒绝，不许静默写坏相邻池」）：目标取局部整型池窗口的**最后一格**、
#   count = 2 ⇒ 第 2 格会落进相邻的浮点族窗口 ⇒ 三条写指令（0x6c / 0x2d8 / 0x64）都必须拒绝 + 诊断，
#   且**一格都不写**（目标格预置 1234、相邻浮点格预置 1.5 咬住）；窗口内的对照必须照常写满。
#   拿掉池窗口判据 / 把 count 截断成容量 ⇒ 本条红。
add_test(NAME spec_ops_array_write_past_pool COMMAND emulator-core-selftest --only-array-write-bounds)
# ★ `0x6c` / `0x2d8` 的**字符串族落点**：元素是 28 字节记录，写入仍是"逐 4 字节写 count 个 32 位字"
#   （不是按 28 字节步进）。判据经 `0x1b0` 裸池拷贝把记录里的字读回局部整型池：0/0 与 7/7。
#   把 `store_raw` 的字符串族分支删掉（退回"抛异常"）⇒ 本条红。
add_test(NAME spec_ops_6c_2d8_string_record_raw COMMAND emulator-core-selftest --only-string-record-raw)
# ★ **错误通道**的平台写请求（`60-ops-numeric` 的「错误通道」）：一次触发 = **一个**请求（文本 + 长度）；
#   两个抑制条件（输出句柄无效 / 宿主过滤器返回非 0）⇒ 不发请求但**诊断仍保留**。绕开接缝
#   （`report_error` 退回只 push_back）或忽略过滤器 ⇒ 本条红。
add_test(NAME spec_error_channel_write COMMAND emulator-core-selftest --only-error-channel)
# ★ `0x12f`（`CORE-OPS-ARRAY-SORT-12F`）的**写序**：A 与 B 别名（A = B、N = 3）时，右键必须
#   **每次比较重算** `B[i] + C[i]`。按写序 A = [1,2,0]；把右键缓存成 `key_j`（旧实现）得到 [2,1,0] ⇒ 本条红。
add_test(NAME spec_ops_12f_alias_write_order COMMAND emulator-core-selftest --only-12f-alias)
# ★ `0x88`（`CORE-OPS-SCALAR-PAIR-88`）的落点身份：同一个值写模式族的 **C / D**，非 0 支写 **G**，
#   为 0 支清**状态选择字第 27 位**；`0x19c` 的第一个判据必须读 `0x88` 写出来的 C，
#   `0x1cc` 读的状态格必须就是 `0x19c` 的 **E**。把落点改回任何死格（旧 `scalar_88a_`）⇒ 本条红。
add_test(NAME spec_ops_88_mode_cells COMMAND emulator-core-selftest --only-88-mode-cells)
# ★ 第 27 位服务体（`CORE-VM-GATE` / `CORE-VM-STATE` 位序表第 16 行）：置起后进入**一次消息处理**
#   （seam 计数 +1）→ 让出一拍 → 窥视 → 交回迭代头 ⇒ 本迭代 `ip` 不变、被挡的脚本指令不执行；
#   清位后必须恢复派发。删 `case 27`（落 default 虽同样交回迭代头）⇒ 消息处理计数不 +1 ⇒ 本条红。
add_test(NAME spec_vm_gate_bit27_service COMMAND emulator-core-selftest --only-bit27-service)
# ★ 第 27 位的**产品置点**：`0x71` 的文本子系统调用返回非 0 / `0x6e` 的追加前置支
#   （`message:ReadTextSkip` ∧ 已读命中 ⇒ 置位并**不置打字位**、同帧抽干）/ `0x305` ② 同一判据。
#   删 `0x71` 的置点 ⇒ 第一段红；把置位挪进打字支 / 不置位 ⇒ 后两段红。
add_test(NAME spec_adv_text_bit27_set_points COMMAND emulator-core-selftest --only-adv-text-bit27)
# ★ `0x2bd`（`CORE-OPS-SCALAR-VARIANT-PROLOGUE`）：操作数非 0 ⇒ 字体对象**两格**都写 700；0 ⇒ 都写 0
#   （0x42622A 的 `mov eax,2BCh` / 0x426243 的 0 支）。把 700 改成别的常量 ⇒ 本条红。
add_test(NAME emulator_core_font_weight_2bd_both_cells
    COMMAND emulator-core-selftest --only-font-weight-2bd)

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
    # ★ 点击节奏（**程序侧基线，不是引擎要求**）：同一段时间线里 80 次点击（每 300 ms、按住 100 ms），
    #   在这份脚本 + 这份配置下**实测**推进 37 次正文（`text-appends`）；阈值取 30 留余量。
    #   守的是"点击大量被吞"——等待位不看颜色窗闩时只推进 0–13 次。
    #   ⚠ 脚本 / 配置一变就要重算这个基线（它由 `0x238` / `0x202` 的操作数决定）；
    #   **引擎不变量**不在这条里，见 `emulator-core-wait-colour-window`。
    #   阈值与实测值也写在 tests/adv_click_rate.cmake 里 —— 两处必须是同一组数字，日志留在构建目录。
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

