// emulator-lift-aot · 入口 / CLI（Shape A）
//
// ★ 本文件是**新的** main：⛔ 不 include `machine.h`、⛔ 不实例化 `Machine`、⛔ 不调用
//   `Machine::run` / `BlockCache::get`。它只做四件事：
//     ① 定位执行源（**照已退役解释器臂 `main.cpp` 的默认 / 环境变量口径**，⛔ 不自己编路径）；
//     ② 调 `AotHost` 做平台 bring-up（映射 PE、建 guest 栈 / 堆 / TEB、装配宿主服务、注册并 bind shim）；
//        —— 其中 `loadImage` **先核对构建期 pin**（D1：sha256 + entry），不一致 ⇒ 打印两个哈希 + exit 2；
//     ③ 调 `AotHost::run` 跑到停机判据（`--max-frames` / `--max-insns` / `--until` / 停因）；
//     ④ 打印读数与运行摘要，按 `ok()` 给退出码（0 / 2，与解释器臂同一口径）。
//
// ★ 为什么执行源要跟解释器臂同一套解析：读数（base / entry / 节表 / 导入数）必须与
//   `emulator-lift` 报的是**同一份字节**，否则两边对不上时无法判断"是 lift 的问题还是喂了别的 exe"。
// ★ `getenv` 在 MSVC 口径里是 deprecated 警告（不是问题）；与已退役解释器臂 `main.cpp` 同一条处置。
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "aot/aot_dispatch.h"
#include "aot/aot_host.h"
#include "plugin/plugin_host.h"      // C2a：QJS 单插件（--plugin）的宿主适配器
#include "plugin/plugin_search.h"    // S1：`--plugin <id|path>` 的**纯**解析器（id 只查两个白名单根）
#include "plugin/readings.h"         // C2a：生产 ReadingsSource（marker + 鼠标）
#include "runtime/capability.h"      // ★ 能力面（`caps=` 的**唯一真源**：与 hello 同一张表）
#include "runtime/control.h"          // 控制层端点（`--control-root`；PIMPL，⛔ 不含 beast / nlohmann）
#include "runtime/guest_exit.h"       // guest 退出码 → 宿主退出码（与解释器臂**同一份**映射；纯头）
#include "runtime/font.h"             // `--baseline-lift` / `--baseline-lift-by-size`（任何 FontEngine 之前）
#include "runtime/host_cursor.h"      // `--cursor-warp` 的平台闸门（与解释器臂同一条判据）
#include "runtime/host_encoding.h"
#include "runtime/input_plan.h"       // `--input-plan`（S4：AOT 臂唯一的输入控制面）
#include "runtime/log.h"              // `--log-level`（缺省 warn；AOT 臂过去没有这个开关）
#include "runtime/lift_intrinsics.h"  // `--no-intrinsics` + `kMemcpyFastMaxBytes`（启动 readout）
#include "runtime/memory.h"           // setGuestFaultTextHook（fault 路径的 `input-plan` 摘要；C）
#include "runtime/present_flags.h"    // `--cursor-warp` / `--sound` 的**唯一**一份判据 + 文案
#include "runtime/present_sdl.h"
#include "runtime/vfs.h"  // openHostFileUtf8（Windows 的 UTF-8 路径）
#include "runtime/window_scale.h"  // `--scale <auto|数值>` 的**唯一**一份口径（尺寸 / DPI / readout 文案）

namespace {

// ★ 与已退役解释器臂的 `fileExists` 同一口径：Windows 上窄 `fopen` 按 ANSI 码页解释路径，
//   日文目录会打不开 ⇒ 必须走宽字符版。
bool fileExists(const std::string& p) {
    std::FILE* f = lift::openHostFileUtf8(p, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// `argv[0]`（UTF-8）的**目录部分**，含结尾分隔符；没有分隔符（PATH 搜索 / 只有文件名）⇒ 空串。
// ★ 只做字符串切分：这里要的是"**发行布局**里脚本在哪"这一跳，⛔ 不解析符号链接 / 不查 cwd
//   （aot 臂拿到的 argv[0] 是 `$<TARGET_FILE:…>` 那种绝对路径，测试与发行都覆盖）。
std::string exeDirOf(const std::string& argv0) {
    const std::size_t at = argv0.find_last_of("/\\");
    if (at == std::string::npos) return std::string();
    return argv0.substr(0, at + 1);
}

std::string envOr(const char* name) {
    const char* v = std::getenv(name);
    return v ? v : "";
}

// `--input-plan` 的 fault 摘要钩子（`memory.h` 的 `FaultTextHook`；`ctx` = `lift::InputPlan*`）：
// ★ guest fault 走 `std::_Exit(3)`，`host.run()` 不会返回 ⇒ 正常收口那一行摘要读不到。
//   这个钩子让 fault 文本里出现**同一份** `steps/fired/pending` + `outcome`（C 的要求）。
// ★ 零分配：`printFaultSummary` 只用定长缓冲（fault 在信号上下文里，见 `memory.h`）。
void faultPlanSummary(void* ctx, std::FILE* out) {
    auto* plan = static_cast<lift::InputPlan*>(ctx);
    if (plan != nullptr) plan->printFaultSummary(out);
}

// `--input-plan-trace` 的 `state_seq` 源：唯一生产者 = 控制层（没开 `--control-root` ⇒ 不装 ⇒ trace 打 `-`）。
std::uint64_t controlStateSeq(void* ctx) {
    auto* control = static_cast<lift::Control*>(ctx);
    return control != nullptr ? control->stateSeq() : 0;
}

// 插件 `IntentExecutor` 的**帧线**（与 `--input-plan` 同一条）：`presenter.frameCount()` = 已完成
// guest 回调数（`present.cpp` 在 `++frame_count_` 同一刻推 `PlanEventKind::Frame`）。
// ★ 它与 `InputPlan::presenting()` 是**同一个计数源** —— ⛔ 不另建第二份帧账、⛔ 不按插件/场景特判。
std::uint64_t presentFrameCount(void* ctx) {
    auto* host = static_cast<lift::aot::AotHost*>(ctx);
    return host != nullptr ? host->presenter().frameCount() : 0;
}

void usage() {
    std::printf(
        "emulator-lift-aot —— Shape A：**执行**构建期 lift 出来的代码（⛔ 不构造 Machine）\n"
        "  --exe <path>        执行源（缺省：env LIFT_AGE_EXE / AMAYUI_AGE_EXE → 仓库内\n"
        "                      corpus/assets/age/AGE.EXE__dumped.sectfix.EXE；口径与 emulator-lift 相同）\n"
        "                      ★ 必须与构建期 pin 的 sha256 + entry 一致（D1）⇒ 不一致 exit 2\n"
        "  --game-dir <path>   guest 文件系统的唯一根（缺省 env AMAYUI_GAME_DIR / AMAYUI_INSTALL）\n"
        "  --guest-root <path> guest 眼里的安装根文本（缺省同 emulator-lift：E:\\Games\\Eushully\\天結）\n"
        "  --appdata <path>    伪造 AppData 的宿主目录（特殊目录映射；缺省 = 不用 AppData）\n"
        "  --vfs-cache-mb N    ★ 只读安装树**内容缓存**的字节预算（MiB；缺省 256，0 = 关）。\n"
        "                      只在有 `--appdata` 时生效（那时安装树按合同只读）；只对\n"
        "                      \"纯读 + OPEN_EXISTING + 解析结果在安装树之下\"的打开生效。\n"
        "                      小文件整块进堆、大文件只读 mmap；每次命中用一次 stat 校验 size+mtime。\n"
        "  --vfs-dir-index 0|1 ★ 安装树**目录索引**（缺省 1；0 = 关，供 A/B 与守卫）。\n"
        "                      只在有 `--appdata` 时生效（那时安装树按合同只读）；⛔ 不是语义开关 ——\n"
        "                      开 / 关两档的解析结论（ok / host / error）必须逐字节相同。\n"
        "  --max-frames N      Present 到第 N 帧后停（0 = 不限；缺省 0）\n"
        "  --max-insns N       guest 指令数绝对上限（0 = 不限）\n"
        "  --until EA          跑到该 EA（块边界）后停（hex，缺省关）\n"
        "  --time-limit SEC    墙钟上限（每 4096 条查一次；0 = 不限）\n"
        "  --skip-logo         一次性 latch：T_Command + 0x5EB5C 写 0（口径同 emulator-lift；\n"
        "                      钩子点 AOT 用 0x004BAA33；⛔ 不跳过 boot、不改 eip/寄存器）\n"
        "  --audio off|sdl     音频后端（缺省 off：不开设备 / 不跑混音；判据只有一份 =\n"
        "                      runtime/audio_policy.h）。sdl = 真设备（打不开 ⇒ **没有音频输出**：\n"
        "                      不跑混音、摘要 device=none；⛔ 没有第二个档位可退）\n"
        "  --audio-dump <path> 把 48k/16/2ch 交付缓冲写成 WAV（缺省不给 = 不写）；它**不是档位**：\n"
        "                      给了它而 --audio off ⇒ 走设备无关的 dump 路径（档位仍写 off、\n"
        "                      device=none，虚拟时钟）\n"
        "                      文件 = 44 字节头 + 这一跑**真交付**的字节（没有真交付 ⇒ 只有 44 字节头，\n"
        "                      data=0；有没有 PCM 看摘要的 fills / fill_bytes / pushed）\n"
        "                      ★ **abort() 之后 dump 不可信**：heap / handles / shims 的 std::abort()\n"
        "                      路径跳过收口的 end() ⇒ 头里两个长度字段不回填（文件大小照旧，字段是 0）\n"
        "  --input-plan <file> 组装输入配方（缺省不给 = 没有任何输入注入，行为与本参数无关）：\n"
        "                      文件格式 = **唯一一份配方语法**（`runtime/recipe.h`；解释器臂的\n"
        "                      `--interaction-graph` 已退役 ⇒ 本臂是**唯一**的配方入口；\n"
        "                      ⛔ 没有\"本入口不认\"的指令）：\n"
        "                        await script <名字> [timeout <ms>]\n"
        "                                              电平门：当前引擎脚本身份 == 名字即通过\n"
        "                                              （ASCII 大小写不敏感、两侧都忽略 .bin）；\n"
        "                                              行尾 `timeout` = **这个门**的守卫（最多等多久）\n"
        "                        await position <名字> <FROM> <TO> [timeout <ms>]\n"
        "                                              电平门：script 匹配 **且** FROM <= pos < TO\n"
        "                                              （半开区间；FROM/TO 是带 `0x` 前缀的 hex；\n"
        "                                              pos = 脚本体内绝对字节偏移，与清单 label_%%08x\n"
        "                                              同一坐标系；不满足就继续等，不报错/不超时）\n"
        "                        wait <ms>             宿主单调时钟毫秒（正整数；非阻塞；**固定时长**）\n"
        "                                              ⛔ 不带 timeout（它没有可等待的东西）\n"
        "                        click <x> <y> [left|right]\n"
        "                                              x/y = **1280×720 参考系**，投递前按客户区缩放；\n"
        "                                              按钮词裸写，省略 = left（不收 =left；⛔ 不带 timeout）\n"
        "                        mouse_move <x> <y>\n"
        "                                              只写光标（同 click 的第一段；同一参考系 / 缩放）\n"
        "                        mouse_down [left|right]\n"
        "                                              用**当前设备光标位置**按下（⛔ 不带 x/y）；必须有一条\n"
        "                                              更早的 mouse_move；轻闸门 = 它之后完成过一次位置读点，\n"
        "                                              或 kPressFallbackMs 墙钟兜底（不成立就不消费该步）\n"
        "                        mouse_up [left|right]\n"
        "                                              用**当前设备位置**抬起（可与 down 不同点 ⇒ 拖拽不回跳）\n"
        "                        profile_begin|end <label>\n"
        "                                              区间读数（本臂没有分项 profiler ⇒ begin/end 各一行\n"
        "                                              宿主墙钟 + 读点数，可与 --profile 对齐）\n"
        "                        quit                  投 WM_QUIT 结束本次运行（后续指令不再推进）\n"
        "                      ★ `timeout <ms>` 只能写在上面两条 `await …` 的**行尾**：单独一行\n"
        "                      （旧的\"守上一条指令\"写法）或挂在 wait/click/mouse_move/mouse_down/\n"
        "                      mouse_up/profile_*/quit 上 ⇒ exit 2；\n"
        "                      推进只发生在 **guest 读点**（flush-before-read），⛔ 没有帧精度保证；\n"
        "                      旧指令（`await always` / `await presents <n>` / `key`）/ 图语法 ⇒ exit 2；\n"
        "                      跑完打一行 `input-plan: steps=… fired=… pending=…` 摘要（一次性）；\n"
        "                      ★ **完成度是判据**：pending != 0 或某个**门**的 timeout 到期 ⇒ stderr 点名\n"
        "                      `文件:行` + 门/动作 并 **exit 5**（⛔ 不许只警告仍 exit 0）；\n"
        "                      ★ `wait` 没有守卫（固定时长）⇒ 卡住由 --time-limit / --max-frames 报。\n"
        "                      解析失败 ⇒ exit 2 + 行号\n"
        "  --input-plan-trace  **opt-in**（缺省关）：每条 step **成立 / 施加**时打一行\n"
        "                      `plan:<文件>:<行> applied kind=… script=… pos=… frame=… state_seq=… reads=…`，\n"
        "                      click 的抬起再打一行（`reason=keylevel|wallclock` ⇒ guest 到底有没有机会\n"
        "                      看到按下）；`frame` = 已 Present 帧数，`state_seq` 只有开 --control-root\n"
        "                      才有（否则 `-`）。⛔ 不每帧 / 不每块打；缺省档一条都不打\n"
        "                      （缺 --input-plan 时本参数无输出，⛔ 不报错）\n"
        "%s"
        "  --log-level <L>     日志级别：quiet|error|warn|info|debug|trace（**缺省 warn** ⇒ 默认档输出\n"
        "                      逐字不变）。★ `[audio] mode=… feeder=… lead=…` 与 `LIFT_AUDIO_TRACE` 的\n"
        "                      `play state=…` 都是 **info** 级 ⇒ 要看它们就给 `--log-level info`。\n"
        "  --dump-frame <path> 每次 Present 导出一份 guest 后缓冲 BMP（{frame} = 4 位帧号）\n"
        "  --profile           采样式内部 profile（**缺省关**）：每帧时间戳 + 派发 / lift_one / shim\n"
        "                      归因 + 机器频率校准；采样率 = dispatch 1/4096、lift_one 1/1024、\n"
        "                      shim 1/64（每个 shim 各自的调用序号；也写在输出头）。★ 采样本身会\n"
        "                      轻微影响时序 ⇒ 性能结论请用**关档**跑（⛔ 不拿 --profile 的墙钟当结论）；\n"
        "                      六项判据开/关一致\n"
        "  --profile-slow-frames <ms>\n"
        "                      **必须与 --profile 同时给**（否则 exit 2）；**缺省 0 = 关**。>0 ⇒ 每帧\n"
        "                      结束时若该帧宿主墙钟 ≥ <ms>，就把**这一帧**的 shim 时间按名字分桶\n"
        "                      （含调用次数）存下来，run 结束时打**一张**表：\n"
        "                        frame / wall_ms / guest insns / top-8 shim（名字 + ms + 次数）\n"
        "                      有界：慢帧最多 32 条、每帧最多 8 个名字，超出只报数（⛔ 不静默）。\n"
        "                      ★ wall_ms = 该帧 `frame-ms` 的 t_ms 差（同一把尺）；shim 时间复用窗口\n"
        "                      profile 那条精确取时，**只统计主线程**（音频 feeder 的调用不计）。\n"
        "                      诊断档 ⇒ 多出来的取时会轻微影响时序，⛔ 不进任何性能判据\n"
        "  --frame-log         每次 Present 打一行 `[frame] n=<帧号> hash=<guest 后缓冲哈希>`（**缺省关**）：\n"
        "                      逐帧 hash 是画面序列读数（在动 / 卡住同一帧），不是判据；末帧 hash 看摘要\n"
        "                      `present :` 行。`--profile` 档也会打这一条（已有 profile 存档的可比面）\n"
        "  --region-log        启动 readout 末尾打 guest 区域表（`regions   :` 一行 + 每段一行，共 28 行；\n"
        "                      **缺省关**）：那是**静态布局快照**（谁占了哪一段），一次 run 里不变，\n"
        "                      属噪音 ⇒ 缺省档不打。⛔ 只影响这一块，别的 readout（exe / PE / 导入 /\n"
        "                      地址与服务面 / pin / 缺口 / 停机原因）一格都不收\n"
        "  --no-intrinsics      **只给 A/B 与诊断**：关掉构建期库内建闸门（缺省 = 开）。IR 里的闸门块 /\n"
        "                      快路径仍在（构建期决定），闸门返回 false ⇒ 回退原 lifted 函数体；\n"
        "                      ⛔ 不是运行期 override（不替换任何执行来源）\n"
        "\n"
        "★ 控制层（停住 / 继续 / 采样 / 截图 / 移光标 / 投递一条 click / typed global 读写 / 收口；\n"
        "  **缺省完全不启动** ⇒ 不给 --control-root 时行为与逐字节输出都不变）：\n"
        "  --control-root <dir>  端点文件目录（**必须显式给**）；起一个只绑 127.0.0.1 的 WS 端点，\n"
        "                        写 `<dir>/ctl.<pid>.<nonce8>.json`（url / pid / port / nonce / token /\n"
        "                        session / exe_sha256 / shape / started_utc），正常退出删\n"
        "  --control-port N      端口（0 = 由系统给，缺省 0）；被占用 ⇒ **exit 2 并点名端口**（⛔ 不换端口）\n"
        "  --control-idle-timeout S   暂停态且无命令超过 S 秒 ⇒ 端点自己 quit（缺省 300）\n"
        "  --control-orphan-timeout S 客户端断了但端点还暂停 ⇒ 自行 quit（缺省 10；⛔ 不抱着 GPU 停着）\n"
        "  --control-predicate-timeout S  resume --for-ms/--for frames 等不到谓词 ⇒ 回 timeout（缺省 30；\n"
        "                             客户端据此 exit 3；谓词当场取消，⛔ 之后不会再「自己停住」）\n"
        "                        能力面（本轮）= pause / resume / sample / screenshot / move / click /\n"
        "                        global-get / global-set / quit；\n"
        "                        协议 schema = emu-ctl/2\n"
        "\n"
        "★ 呈现档位（口径与 emulator-lift 相同；缺省 headless）：\n"
        "  --present headless|window|auto\n"
        "                      headless = **隐藏**窗口 + GPU device（不是\"没有窗口\"：3D 的唯一光栅\n"
        "                      实现是 GPU，没有它一个像素都不画）；window = 可见窗口；\n"
        "                      auto = 可见窗口打不开 ⇒ 退到隐藏窗口\n"
        "  --present-driver LIST     SDL 渲染驱动列表（只对 window/auto 生效）\n"
        "  --present-vsync 0|1       垂直同步（缺省 0）\n"
        "  --present-hidden          强制隐藏窗口（仍必须显式 --present window/auto）\n"
        "  --overlay                 左上角调试 HUD（游戏内小窗 / 帧读数；只进窗口像素与\n"
        "                            --present-screenshot，⛔ 不进 --dump-frame / arena.hash）。\n"
        "                            HUD 由**随发行分发的脚本** `scripts/ui/hud.js` 经 QuickJS 同步\n"
        "                            产出 display list（脚本外置：改脚本不用重编）；文本走宿主字体\n"
        "                            （支持中文）；字体缺失 / 脚本加载失败 ⇒ 拒绝启动（R5）\n"
        "  --ui <path>               用这份 JS 脚本当 HUD（**隐含 --overlay**；与 --overlay 同给\n"
        "                            时以 --ui 为准）；脚本错误点名 `<path>:<行>:<列>`。\n"
        "                            字体缺失 / 脚本编译失败 ⇒ 拒绝启动（exit 2）\n"
        "                            ★ **已弃用**（S1）：请改用 `--plugin <path>`；本轮只打一行 deprecation，\n"
        "                              行为**不变**（`--ui` 仍只吃路径、⛔ 不吃 id；下一轮 S2/S3 删）\n"
        "  --ui-input=consume|pass   注入点击（--input-plan / 控制层）是否也过叠层的命中路由：\n"
        "                            consume（缺省）= 命中即吞（down 吞 ⇒ up 吞，guest 收不到该\n"
        "                            点击的任何消息）；pass = 逐位与没有 UI 相同。也接受\n"
        "                            `--ui-input consume|pass`。只在 --overlay / --ui 时生效；\n"
        "                            窗口参数（--present headless 拒绝）；非法值 exit 2\n"
        "  --plugin <id|path>        用这份 **ES module** 当插件：`export default { render?, start?,\n"
        "                            onTrigger?, initialState? }`（宿主只读 default 导出）。\n"
        "                            `start/onTrigger` 是 Run 的回调链（写只经 ctx.edit / ctx.input）；\n"
        "                            `render` 可选：**窗口档**同一个插件实例被接进叠层 host\n"
        "                            （ctx.draw 的 display list 每帧上屏；⛔ 不跑第二套 UI 运行时）\n"
        "                            ⇒ 有 render 时装，不需要 --overlay；与 `--ui` 是**同一\n"
        "                            字段**（同一路径允许，不同路径 exit 2）；⛔ 不隐含 --overlay\n"
        "                            ⇒ `--present headless` 也能跑（没有窗口就没有 render）。\n"
        "                            与 --input-plan / --control-root 互斥（两套输入写预算）⇒ exit 2\n"
        "                            ★ **id 语法**：`^[A-Za-z0-9_-]{1,64}$`（文件名 stem）。\n"
        "                              含 `.` / `/` / `\\` 的写法一律当**路径**（所以 `x.js` 天然是路径，\n"
        "                              行为与旧版逐字相同：cwd 相对 / 绝对 / 装载失败 exit 2）。\n"
        "                              ★ 显式空串（`--plugin \"\"`）⇒ exit 2：空串不是 id、也不是可装载\n"
        "                              路径；⛔ 不静默当成\"没有插件\"。\n"
        "                            ★ **id 只查两处非递归根**（⛔ 不查 `lib/`、⛔ 不递归子目录）：\n"
        "                              ① `<exe 目录>/scripts/plugins/<id>.js`（发行）\n"
        "                              ② 编译期 `LIFT_PLUGIN_DIR/<id>.js`（开发；= 源码 scripts/plugins）\n"
        "                              两处都命中 ⇒ exit 2 并列出全部候选；都不在 ⇒ exit 2 并列出搜过的\n"
        "                              目录与每个候选路径。命中 ⇒ 打一行 `--plugin id=<id> ⇒ <绝对路径>`\n"
        "                              并用绝对路径装载（一次性，⛔ 不周期）。\n"
        "  --plugin-param k=v        给插件的只读参数（`ctx.params`；可重复，**重复 key ⇒ exit 2**）\n"
        "                            ★ 内建插件优先读 `<--appdata>/plugin-config.json`（ctx.config）；\n"
        "                            **没有 appdata 或没有该 key** 才回退到这里（文件坏 ⇒ exit 2）\n"
        "  --present-screenshot <path>      每次可见窗口 Present 后存一张窗口 BMP（{frame} 可用）\n"
        "  --present-screenshot-frame N     只回读第 N 帧（1 起；0 = 每帧都写）\n"
        "  --scale auto|数值  窗口客户区 = round(1280×s) × round(720×s)。★ **只改窗口、不改帧**：\n"
        "                      guest 画面恒为 1280×720 ⇒ 放大由宿主完成（选哪个滤波器见\n"
        "                      `--scale-filter`，缺省 linear = 与真机 150%% DPI 同档的平滑放大）\n"
        "                      ⇒ ⛔ 不做高分辨率重渲染 / 不改帧尺寸 / 不加 upscale pass。缺省 1.0\n"
        "                      = 1280×720，逐字节不变。auto = 按显示 DPI（SDL 的 display content\n"
        "                      scale）取，取不到 ⇒ 回 1.0 并打一行 readout；启动固定打一行\n"
        "                      `scale: …（window WxH，image 1280x720；filter=…；…）`\n"
        "                      （缺省 1.0 也打，一次性；s<1 说\"缩小\"）。★ `--present headless`\n"
        "                      对 `--scale` **无副作用**：接受但不生效（隐藏离屏窗口仍是\n"
        "                      1280×720，也不查 DPI）。非 auto 且非正数 / NaN / 垃圾 ⇒ exit 2\n"
        "                      并点名本参数\n"
        "  --scale-filter nearest|linear\n"
        "                      放大时的采样滤波器（缺省 **linear**）：linear = 与真机 150%% DPI\n"
        "                      同档的平滑放大 / nearest = 硬边（1.5× 这类非整数倍率会有锯齿；\n"
        "                      像素党 / A/B 对照用，与换滤波器之前逐字节相同）。★ 只影响宿主\n"
        "                      呈现的那次放大：⛔ 不改帧尺寸、⛔ 不改 guest 像素、⛔ 不改游戏内容\n"
        "                      的纹理采样；清晰度仍由源分辨率（image 1280×720）决定。\n"
        "                      未知值（含大小写不符）⇒ exit 2 并点名本参数\n"
        "  --baseline-lift <N> 宿主基线补偿**默认字号的值**像素（0..64；0 = 关；缺省 = 编译期\n"
        "                      `kBaselineLift`）。★ 宿主视觉策略（⛔ 不是引擎语义）：≤16px 按表给 1，\n"
        "                      同一字号下所有字形位移完全一致（⛔ 不做逐字型夹取）。\n"
        "  --baseline-lift-by-size <0|1>\n"
        "                      1（缺省）= 按像素字号查表；0 = 旧行为（全字号恒 `--baseline-lift`，\n"
        "                      A/B 对照用）。`--baseline-lift 0` 恒关。\n"
        "%s"
        "  env LIFT_FAULT_DUMP=<spec>\n"
        "                      **opt-in**（缺省不设 = fault 文本一条 dump 行都不多）：guest fault 时按\n"
        "                      spec dump 内存。每段 = `EA:LEN`（EA 纯 hex，0x 可省；LEN 十进制，\n"
        "                      0x 前缀 = hex）或 `EA:*LEN`（先读 EA 处 4 字节当 guest 指针再 dump；\n"
        "                      `EA:4*LEN` 同义）或 `EA:*<delta>:LEN`（跟随并把指针加有符号 delta，\n"
        "                      |delta| ≤ 1 MiB，如 `*-8:16` 从 ptr-8 起 dump），逗号分隔。上限 =\n"
        "                      8 段 / 每段 1..256 B / 总 ≤1024 B / spec ≤1024 字符，超限 ⇒ **exit 2**\n"
        "                      （⛔ 不静默截断）。\n"
        "                      例：LIFT_FAULT_DUMP=0x1005AB58:64,0x10050000:*32,0x1005AB58:*-8:16\n"
        "  env LIFT_WATCH=<spec>\n"
        "                      **opt-in**（缺省不设 = 一条 watch 行都不打，零输出）：观察 guest 侧\n"
        "                      写入（`lift_note_write`）与 shim 侧写入（writeBytes / fillBytes /\n"
        "                      wr8..wr64）。每段 = `EA:LEN`（EA 纯 hex，0x 可省；LEN 十进制 1..16）\n"
        "                      或 `EA:LEN>follow`（仅 LEN=4：命中后把**下一次写**开头读到的指针 P 的\n"
        "                      [P-8,P+8) 加进观察集 —— 用来点名\"谁在 P-8 写了那 16 字节\"），\n"
        "                      逗号分隔。上限 = 8 段 / spec ≤1024 字符，超限 ⇒ **exit 2**（⛔ 不静默\n"
        "                      截断）。命中打一行 `watch      : WRITE 0x<EA> n=<N> block=lift_cc_<EA>`\n"
        "                      （guest）或 `... shim=<名字>`（shim 侧；名字由 shim 调用点设，未设 ⇒ `?`）。\n"
        "                      例：LIFT_WATCH=0x1005AB58:4>follow\n"
        "  --help              这份说明\n"
        "\n"
        "★ 执行模型：入口 EA → 构建期生成的派发表（`lift_cc_<EA>`）；shim 桩 EA → `lift_shim`；\n"
        "  两者都不是 ⇒ **响亮停机并打印 EA**（⛔ 不设\"返回哨兵 = 结束\"的约定）。\n",
        lift::kAotSoundHelp, lift::kCursorWarpHelp);
}

// ★ 执行源定位**照抄**已退役解释器臂的四处顺序（--exe → env → 仓库内入库件 → 拒绝启动）。
//   ⛔ 不回退到游戏安装目录里的原版 exe（那是加壳件，节表未还原）。
bool resolveExePath(const std::string& explicit_path, std::string* out, std::vector<std::string>* tried) {
    if (!explicit_path.empty()) {
        *out = explicit_path;
        tried->push_back("--exe");
        return fileExists(*out);
    }
    // ★ main.cpp 的写法就是两个 env **拼起来**（只有同时设了两个才与"先后取一个"不同）；
    //   照抄是为了两处解析逐字一致。
    const std::string env = envOr("LIFT_AGE_EXE") + envOr("AMAYUI_AGE_EXE");
    if (!env.empty()) {
        *out = env;
        tried->push_back("env LIFT_AGE_EXE / AMAYUI_AGE_EXE");
        return fileExists(*out);
    }
    const char* candidates[] = {
        "corpus/assets/age/AGE.EXE__dumped.sectfix.EXE",
        "../../corpus/assets/age/AGE.EXE__dumped.sectfix.EXE",
    };
    for (const char* c : candidates) {
        tried->push_back(c);
        if (fileExists(c)) {
            *out = c;
            return true;
        }
    }
    return false;
}

bool parseU32Hex(const std::string& s, std::uint32_t* out) {
    const char* p = s.c_str();
    char* end = nullptr;
    const unsigned long long v = std::strtoull(p, &end, 16);
    if (end == p || *end != '\0' || v > 0xFFFFFFFFull) return false;
    *out = static_cast<std::uint32_t>(v);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    // ★ 进程入口三件事，顺序与已退役解释器臂 `main.cpp` 相同（理由见 runtime/memory.h / host_encoding.h）：
    //   ① 关掉 Windows 上"崩了就弹模态框"的默认行为（无人值守的整轮会被挂死）；
    //   ② 控制台输出码页 → UTF-8；
    //   ③ argv 一律按 UTF-8 取（CRT 的 char** 是 ANSI 码页）。
    lift::suppressCrashDialogsOnce();
    lift::initConsoleUtf8();
    const std::vector<std::string> args = lift::argvUtf8(argc, argv);

    std::string exe_path;
    std::string game_dir;
    std::string appdata_dir;
    // ★ C7：`--vfs-cache-mb`（缺省 256 MiB；0 = 关）。`_set` 单独记：值 0 也是"给了这个 flag"。
    std::uint64_t vfs_cache_mb = 256;
    bool vfs_cache_mb_set = false;
    // ★ C5：`--vfs-dir-index 0|1`（缺省 1 = 开；0 只供 A/B 与守卫，⛔ 不是语义开关）。
    bool vfs_dir_index = true;
    std::string guest_root;
    // ★ `--input-plan <file>`（S4）：AOT 臂的**唯一**输入控制面（`runtime/input_plan.h`）。
    //   缺省空 = 不给 ⇒ 输出与行为逐字节不变（含那 57 行 stdout）。
    std::string input_plan_path;
    // ★ `--input-plan-trace`（opt-in，缺省关；`REQ-01M7PG086DG4BBPA40VCVZTP4X` B）：只影响
    //   `InputPlan` 的 step 事件输出（`plan:<文件>:<行> …`），⛔ 不改任何 guest 可见状态。
    bool input_plan_trace = false;
    std::string audio_mode = "off";  // ★ 本臂缺省 off（不开设备 / 不跑混音：与改动前逐位相同）
    // ★ S1：`--audio-dump <path>`（缺省空 = 不写 WAV）。它**不是档位**：给了它而档位是 off ⇒
    //   引擎走"设备无关的 dump 路径"（虚拟时钟；档位仍写 off、摘要 device=none）。
    //   ⛔ `--audio` 的值域只有 off / sdl（`virtual` 档已于 2026-11-22 移除）。
    std::string audio_dump;
    // ★ `--sound`（缺省关）：**真的**进 `AudioEngine` 的静音门（`mute_ = !sound`）—— 不再是
    //   `aot_host.cpp` 里的硬写 false（b-3）。文案与口径见 `runtime/present_flags.h`。
    bool sound = false;
    // ★ `--baseline-lift <N>` / `--baseline-lift-by-size <0|1>`（宿主视觉策略；⛔ 不是引擎语义）：
    //   随解释器退役被删过一次，本轮装回（`REQ-01M7Q9C16JR9ATFJK11A873B26`）。
    //   ★ 缺省**不覆盖**：`FontConfig` 的缺省值来自 `fontDefaultConfig()`（`kBaselineLift` + by_size=true）
    //     ⇒ 不给这两个参数时行为与"只改代码、不动配置"逐位相同。
    //   ★ 值在 `SdlWindow::init` / `AotHost` 之前落地（HUD 文本也会走 `FontEngine::glyph`）：
    //     见下面的 `fontSetConfig` 调用，位置在 GPU bring-up 之前。
    int baseline_lift = 0;
    bool baseline_lift_set = false;
    bool baseline_lift_by_size = true;
    bool baseline_lift_by_size_set = false;
    // ★ `--profile-slow-frames <ms>`（缺省不给 = 关；**必须与 `--profile` 同给** ⇒ 见下面的校验）。
    //   为什么单独记一个 bool：值 0 也是"给了这个 flag"（= 显式关），⛔ 不要靠 `!= 0` 判"给没给"。
    bool profile_slow_frames_set = false;
    lift::aot::AotRunOptions run_opts;
    // ---- 呈现档位（取值 / 缺省 / 使用门槛**照已退役解释器臂 `main.cpp` 的同一条顺序**）----
    std::string present_mode = "headless";  // 与解释器同缺省
    std::string present_driver = lift::SdlWindowOptions{}.driver_list;
    bool present_driver_set = false;
    std::uint32_t present_vsync = 0;
    bool present_vsync_set = false;
    bool present_hidden = false;
    // ★ `--overlay`（M0；缺省关）：AOT 臂第一次有调试 HUD。只改**宿主送显的窗口像素**
    //   （`--present-screenshot` 含它；`--dump-frame` / `arena.hash` 不含）。HUD 由**脚本**
    //   （默认**外置** `scripts/ui/hud.js`，或 `--ui <path>`）经 QuickJS 产出 display list；文本走宿主字体
    //   （`FontEngine`；支持中文）⇒ 字体缺失 / 脚本加载失败时 `SdlWindow::init` 硬失败（R5）。
    bool overlay = false;
    // ★ `--overlay` 的**显式**位（C2a）：`--plugin` 与 `--ui` 是同一字段 ⇒ 同一路径时 `--ui` 那一半
    //   要让位给插件语义（插件**不**隐含 overlay，D-C2-5）。⇒ overlay 在解析**之后**统一合成：
    //   `overlay = overlay_flag || !ui_script.empty()`（`--ui` 单给时结果与旧行为逐字相同）。
    bool overlay_flag = false;
    // ★ `--ui <path>`（B；缺省空 = 默认外置 hud.js）：**隐含** `--overlay`（脚本是叠层的实现）。
    //   ★ 与 `--overlay` 同给时以 `--ui` 为准（就是这一个字段：非空 ⇒ 用这份脚本）。
    std::string ui_script;
    // ★ `--plugin <path>`（C2a；缺省空 = 不装载插件）：ES module 插件。
    //   · 与 `--ui` **同一字段**（D-C2-2）：两份**不同**路径 ⇒ exit 2；同一路径 ⇒ 只按插件语义；
    //   · ⛔ 不隐含 `overlay`（是否开窗由 `--present` 决定；`--present headless` 也能跑）；
    //   · 与 `--input-plan` / `--control-root` 互斥 ⇒ 启动即 exit 2（两套输入写预算不能共存）。
    std::string plugin_path;
    // ★ `--plugin-param k=v`（可重复；`ctx.params` 只读；**重复 key ⇒ exit 2**，D-C2-9）。
    std::map<std::string, std::string> plugin_params;
    // ★ S9 `--ui-input=consume|pass`（D2 / D6 / R3；缺省 Consume）：注入点击是否也过叠层的命中
    //   路由。值域的唯一一份在 `runtime/ui_input.h` 的 `parseUiInputMode`（⛔ 不在这里另写比较）。
    //   `ui_input_set` 只用于"它是窗口参数"这条判据（`--present headless` 与 `--overlay` 同一条拒绝）。
    lift::UiInputMode ui_input_mode = lift::UiInputMode::Consume;
    bool ui_input_set = false;
    // ★ 宿主真光标（`--cursor-warp`，缺省关；与已退役解释器臂同一个值）：只加宿主副作用，
    //   ⛔ 不改 guest 可见状态。只在 `--present window/auto` 且窗口可见时才真的挪（见
    //   `runtime/present_flags.h` 的 `cursorWarpState`）。
    bool cursor_warp = false;
    std::string present_screenshot;
    bool present_screenshot_set = false;
    std::uint64_t present_screenshot_frame = 0;
    // ★ `--scale <auto|数值>`（缺省 Fixed 1.0 = 1280×720，逐字节不变）+ `--scale-filter nearest|linear`
    //   （缺省 linear）：取值 / 拒绝 / readout 文案的唯一一份在 `runtime/window_scale.h`；
    //   本文件只做"解析 + 交给 sink + 打印"。两个 flag 落进**同一个** `ScaleSpec`（滤波器不是第二份真源）。
    lift::ScaleSpec scale_spec;
    std::string frame_dump;  // --dump-frame（guest 后缓冲 BMP；`cpu.shim.frame_dump`）
    // ★ 控制层（`--control-*`；缺省全不给 ⇒ 控制层**完全不启动**，默认档逐字节不变）。
    std::string control_root;
    std::uint32_t control_port = 0;
    bool control_port_set = false;
    double control_idle_s = 300.0;
    double control_orphan_s = 10.0;
    double control_predicate_s = 30.0;
    bool control_idle_set = false;
    bool control_orphan_set = false;
    bool control_predicate_set = false;

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto needValue = [&](const char* flag, std::string* dst) {
            if (i + 1 >= args.size()) {
                std::fprintf(stderr, "emulator-lift-aot: %s 需要一个值\n", flag);
                return false;
            }
            *dst = args[++i];
            return true;
        };
        if (a == "--exe") {
            if (!needValue("--exe", &exe_path)) return 2;
        } else if (a == "--game-dir") {
            if (!needValue("--game-dir", &game_dir)) return 2;
        } else if (a == "--appdata") {
            if (!needValue("--appdata", &appdata_dir)) return 2;
        } else if (a == "--vfs-cache-mb") {
            // ★ C7：只读安装树内容缓存的字节预算（MiB；0 = 关；缺省 256）。
            //   **只收十进制非负整数**：`strtoull("-1")` 会静默回绕成巨大值 ⇒ 负号 / 小数 / 垃圾一律 exit 2。
            std::string v;
            if (!needValue("--vfs-cache-mb", &v)) return 2;
            bool digits_only = !v.empty();
            for (const char c : v) {
                if (c < '0' || c > '9') {
                    digits_only = false;
                    break;
                }
            }
            if (!digits_only) {
                std::fprintf(stderr,
                             "emulator-lift-aot: --vfs-cache-mb 需要非负整数（MiB，0 = 关）：`%s`\n",
                             v.c_str());
                return 2;
            }
            vfs_cache_mb = std::strtoull(v.c_str(), nullptr, 10);
            vfs_cache_mb_set = true;
        } else if (a == "--vfs-dir-index") {
            // ★ C5：安装树目录索引的开 / 关（只认 0 / 1；⛔ 缺省 1）。
            std::string v;
            if (!needValue("--vfs-dir-index", &v)) return 2;
            if (v != "0" && v != "1") {
                std::fprintf(stderr, "emulator-lift-aot: --vfs-dir-index 需要 0 或 1：`%s`\n", v.c_str());
                return 2;
            }
            vfs_dir_index = (v == "1");
        } else if (a == "--guest-root") {
            if (!needValue("--guest-root", &guest_root)) return 2;
        } else if (a == "--input-plan") {
            if (!needValue("--input-plan", &input_plan_path)) return 2;
        } else if (a == "--input-plan-trace") {
            // ★ 纯开关（不带值）。没有 `--input-plan` 时本参数只是"没有 step 事件可打"——
            //   ⛔ 不报错、⛔ 不静默改成别的语义（配方是唯一的生产者，见 `InputPlan::setTrace`）。
            input_plan_trace = true;
        } else if (a == "--max-frames") {
            std::string v;
            if (!needValue("--max-frames", &v)) return 2;
            char* endp = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &endp, 10);
            if (endp == v.c_str() || *endp != '\0') {
                std::fprintf(stderr, "emulator-lift-aot: --max-frames 需要非负整数：`%s`\n", v.c_str());
                return 2;
            }
            run_opts.max_frames = static_cast<std::uint32_t>(n);
        } else if (a == "--max-insns") {
            std::string v;
            if (!needValue("--max-insns", &v)) return 2;
            char* endp = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &endp, 10);
            if (endp == v.c_str() || *endp != '\0') {
                std::fprintf(stderr, "emulator-lift-aot: --max-insns 需要非负整数：`%s`\n", v.c_str());
                return 2;
            }
            run_opts.max_insns = n;
        } else if (a == "--until") {
            std::string v;
            if (!needValue("--until", &v)) return 2;
            if (!parseU32Hex(v, &run_opts.until_ea)) {
                std::fprintf(stderr, "emulator-lift-aot: --until 需要一个 hex EA：`%s`\n", v.c_str());
                return 2;
            }
        } else if (a == "--time-limit") {
            std::string v;
            if (!needValue("--time-limit", &v)) return 2;
            char* endp = nullptr;
            const double d = std::strtod(v.c_str(), &endp);
            if (endp == v.c_str() || *endp != '\0' || d < 0.0) {
                std::fprintf(stderr, "emulator-lift-aot: --time-limit 需要非负数：`%s`\n", v.c_str());
                return 2;
            }
            run_opts.time_limit_sec = d;
        } else if (a == "--audio") {
            if (!needValue("--audio", &audio_mode)) return 2;
        } else if (a == "--audio-dump") {
            if (!needValue("--audio-dump", &audio_dump)) return 2;
            // ★ 空串不是"没有 dump"：argv 里给了 `""` 时静默当成没给会把"我要落盘"变成静默失效。
            if (audio_dump.empty()) {
                std::fprintf(stderr, "emulator-lift-aot: --audio-dump 需要一个非空路径\n");
                return 2;
            }
        } else if (a == "--sound") {
            // ★ `--sound` 只置一个 bool：它经 `host.configureAudio(...)` 进 `AudioEngine` 的静音门
            //   （`mute_ = !sound`），并由下面那条一次性说明与代码同形地报出来。
            sound = true;
        } else if (a == "--baseline-lift") {
            // ★ 0..64 的十进制整数（范围与 `docs/03-emulator/fonts.md` 的文档口径一致）；
            //   非数字 / 越界 ⇒ exit 2 并**点名本参数**（⛔ 不静默当成缺省）。
            std::string v;
            if (!needValue("--baseline-lift", &v)) return 2;
            bool digits_only = !v.empty();
            for (const char c : v) {
                if (c < '0' || c > '9') {
                    digits_only = false;
                    break;
                }
            }
            char* endp = nullptr;
            const unsigned long long n = digits_only ? std::strtoull(v.c_str(), &endp, 10) : 0ull;
            if (!digits_only || endp == v.c_str() || *endp != '\0' || n > 64ull) {
                std::fprintf(stderr,
                             "emulator-lift-aot: --baseline-lift 需要 0..64 的整数：`%s`\n",
                             v.c_str());
                return 2;
            }
            baseline_lift = static_cast<int>(n);
            baseline_lift_set = true;
        } else if (a == "--baseline-lift-by-size") {
            // ★ 只收 0 / 1（= 关 / 开按字号查表）；其它值 ⇒ exit 2 并点名（⛔ 不把垃圾当真）。
            std::string v;
            if (!needValue("--baseline-lift-by-size", &v)) return 2;
            if (v != "0" && v != "1") {
                std::fprintf(stderr,
                             "emulator-lift-aot: --baseline-lift-by-size 只收 0 / 1：`%s`\n",
                             v.c_str());
                return 2;
            }
            baseline_lift_by_size = v == "1";
            baseline_lift_by_size_set = true;
        } else if (a == "--skip-logo") {
            // ★ 与已退役解释器臂同一条：只置一个 bool（一次性 latch，语义在 aot_run.cpp）。
            run_opts.skip_logo = true;
        } else if (a == "--dump-frame") {
            // ★ 与已退役解释器臂同一个字段（`cpu.shim.frame_dump`）；D3D9 Present 消费。
            if (!needValue("--dump-frame", &frame_dump)) return 2;
        } else if (a == "--log-level") {
            // ★ 日志级别（**缺省 warn** ⇒ 默认档输出逐字不变）。AOT 臂过去没有这个开关，
            //   于是 `[audio] mode=…`（Info）与 `LIFT_AUDIO_TRACE` 的 `play state=` 在 AOT 上
            //   永远看不见 —— 那会让音频验收的"读数面"假绿。值域 = `runtime/log.h` 的
            //   `logParseLevel`（唯一一份），⛔ 不在这里另写一份字符串表。
            std::string level;
            if (!needValue("--log-level", &level)) return 2;
            if (!lift::logSetLevel(level.c_str())) {
                std::fprintf(stderr,
                             "emulator-lift-aot: --log-level 的值非法：`%s`"
                             "（只能是 quiet / error / warn / info / debug / trace）\n",
                             level.c_str());
                return 2;
            }
        } else if (a == "--profile") {
            // ★ 采样式内部 profile（缺省关；采样率与三条硬口径见 `aot_host.h` 的 `AotProfile`）。
            run_opts.profile = true;
        } else if (a == "--profile-slow-frames") {
            // ★ 按帧的 shim 分桶（缺省关；口径见 `aot/frame_bucket.h`）。**只收十进制非负整数**：
            //   `strtoull("-1")` 会静默回绕成巨大值 ⇒ 负号 / 小数 / 垃圾一律 exit 2 并点名。
            std::string v;
            if (!needValue("--profile-slow-frames", &v)) return 2;
            bool digits_only = !v.empty();
            for (const char c : v) {
                if (c < '0' || c > '9') {
                    digits_only = false;
                    break;
                }
            }
            char* endp = nullptr;
            const unsigned long long n = digits_only ? std::strtoull(v.c_str(), &endp, 10) : 0ull;
            if (!digits_only || endp == v.c_str() || *endp != '\0') {
                std::fprintf(stderr,
                             "emulator-lift-aot: --profile-slow-frames 需要非负整数（毫秒，0 = 关）：`%s`\n",
                             v.c_str());
                return 2;
            }
            run_opts.profile_slow_frames_ms = n;
            profile_slow_frames_set = true;
        } else if (a == "--frame-log") {
            // ★ 逐帧 `[frame] n=… hash=…`（**缺省关**；理由见 `AotRunOptions::frame_log`）。
            run_opts.frame_log = true;
        } else if (a == "--region-log") {
            // ★ 启动 readout 末尾的 guest 区域表 28 行（**缺省关**；理由见 `AotRunOptions::region_log`）。
            run_opts.region_log = true;
        } else if (a == "--no-intrinsics") {
            // ★ 构建期库内建（v1：`_memcpy`）只关运行期闸门（A/B 与诊断）。缺省 = 开。
            run_opts.no_intrinsics = true;
        } else if (a == "--present") {
            if (!needValue("--present", &present_mode)) return 2;
        } else if (a == "--control-root") {
            if (!needValue("--control-root", &control_root)) return 2;
        } else if (a == "--control-port") {
            std::string v;
            if (!needValue("--control-port", &v)) return 2;
            char* endp = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &endp, 10);
            if (endp == v.c_str() || *endp != '\0' || n > 65535ull) {
                std::fprintf(stderr, "emulator-lift-aot: --control-port 需要 0..65535：`%s`\n", v.c_str());
                return 2;
            }
            control_port = static_cast<std::uint32_t>(n);
            control_port_set = true;
        } else if (a == "--control-idle-timeout") {
            std::string v;
            if (!needValue("--control-idle-timeout", &v)) return 2;
            char* endp = nullptr;
            const double d = std::strtod(v.c_str(), &endp);
            if (endp == v.c_str() || *endp != '\0' || d < 0.0) {
                std::fprintf(stderr, "emulator-lift-aot: --control-idle-timeout 需要非负数：`%s`\n", v.c_str());
                return 2;
            }
            control_idle_s = d;
            control_idle_set = true;
        } else if (a == "--control-orphan-timeout") {
            std::string v;
            if (!needValue("--control-orphan-timeout", &v)) return 2;
            char* endp = nullptr;
            const double d = std::strtod(v.c_str(), &endp);
            if (endp == v.c_str() || *endp != '\0' || d < 0.0) {
                std::fprintf(stderr, "emulator-lift-aot: --control-orphan-timeout 需要非负数：`%s`\n",
                             v.c_str());
                return 2;
            }
            control_orphan_s = d;
            control_orphan_set = true;
        } else if (a == "--control-predicate-timeout") {
            std::string v;
            if (!needValue("--control-predicate-timeout", &v)) return 2;
            char* endp = nullptr;
            const double d = std::strtod(v.c_str(), &endp);
            if (endp == v.c_str() || *endp != '\0' || !(d > 0.0)) {
                std::fprintf(stderr, "emulator-lift-aot: --control-predicate-timeout 需要正数：`%s`\n",
                             v.c_str());
                return 2;
            }
            control_predicate_s = d;
            control_predicate_set = true;
        } else if (a == "--present-driver") {
            if (!needValue("--present-driver", &present_driver)) return 2;
            present_driver_set = true;
        } else if (a == "--present-vsync") {
            std::string v;
            if (!needValue("--present-vsync", &v)) return 2;
            char* endp = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &endp, 10);
            if (endp == v.c_str() || *endp != '\0' || n > 0xFFFFFFFFull) {
                std::fprintf(stderr, "emulator-lift-aot: --present-vsync 需要 0 / 1：`%s`\n", v.c_str());
                return 2;
            }
            present_vsync = static_cast<std::uint32_t>(n);
            present_vsync_set = true;
        } else if (a == "--present-hidden") {
            present_hidden = true;
        } else if (a == "--overlay") {
            // ★ M0：窗口参数（`--present headless` 下由 `present_window_flag` 拒绝）。
            overlay_flag = true;
        } else if (a == "--ui") {
            // ★ B：`--ui <path>` 隐含 `--overlay`（总开关仍是 overlay）。也是窗口参数。
            //   ★ C2a（D-C2-2）：`--plugin` 与它是**同一字段** ⇒ 两份**不同**路径当场 exit 2。
            //   ★ S1：**只 deprecated、不删**（下一轮 S2/S3 删）。每给一次打**一行** warn：
            //     前缀必须是 `emulator-lift-aot:`（⛔ 不用 `[ui]` —— `tests/aot_overlay.cpp` 在数
            //     `[ui]` / `[overlay]` 的行数）。⛔ 不给 `--ui` 加 id 语义（它仍只吃路径）。
            std::string v;
            if (!needValue("--ui", &v)) return 2;
            std::fputs("emulator-lift-aot: --ui 已弃用，请改用 --plugin <path>\n", stderr);
            if (!plugin_path.empty() && plugin_path != v) {
                std::fprintf(stderr,
                             "emulator-lift-aot: --ui 与 --plugin 是同一个字段，但给了两份不同的路径"
                             "（--ui %s / --plugin %s）⇒ exit 2\n",
                             v.c_str(), plugin_path.c_str());
                return 2;
            }
            ui_script = v;
        } else if (a == "--plugin") {
            // ★ C2a：ES module 插件（`export default { render?, start?, onTrigger? }`）。
            //   · 与 `--ui` 同一字段：两份不同路径 ⇒ exit 2（同一路径 ⇒ 解析完之后按插件语义收敛）；
            //   · **重复 `--plugin` 两份不同路径 ⇒ exit 2**（F6：与 `--ui` 的"不静默只取一个"对称；
            //     同一路径重复给是幂等的，允许）；
            //   · ⛔ **不**设 `overlay`（D-C2-5：是否开窗由 `--present` 决定 ⇒ headless 也能跑）；
            //   · ⛔ **不**进 `present_window_flag`（见下面的注释）。
            std::string v;
            if (!needValue("--plugin", &v)) return 2;
            // ★ S1 复审：显式空串 ⇒ **exit 2**（⛔ 不静默当成"没有插件"）。
            //   空串既不是 id（`^[A-Za-z0-9_-]{1,64}$` 要求至少 1 个字符）也不是可装载的路径；
            //   旧实现会因 `plugin_path.empty()` 整块跳过 ⇒ "我明明给了 --plugin 却没生效"。
            if (v.empty()) {
                std::fprintf(stderr,
                             "emulator-lift-aot: --plugin 需要 id 或路径（收到空串）⇒ exit 2"
                             "（空串不是 id：id 只匹配 ^[A-Za-z0-9_-]{1,64}$；路径见 --help）\n");
                return 2;
            }
            if (!plugin_path.empty() && plugin_path != v) {
                std::fprintf(stderr,
                             "emulator-lift-aot: --plugin 给了两份不同的 spec"
                             "（%s / %s）⇒ exit 2（⛔ 不静默只取一个）\n",
                             plugin_path.c_str(), v.c_str());
                return 2;
            }
            if (!ui_script.empty() && ui_script != v) {
                std::fprintf(stderr,
                             "emulator-lift-aot: --plugin 与 --ui 是同一个字段，但给了两份不同的 spec"
                             "（--plugin %s / --ui %s）⇒ exit 2\n",
                             v.c_str(), ui_script.c_str());
                return 2;
            }
            plugin_path = v;
        } else if (a == "--plugin-param") {
            // ★ `k=v`（可重复；`ctx.params` 只读）。**重复 key ⇒ exit 2**（D-C2-9）：
            //   ⛔ 不静默"后一个赢"——那是"我明明传了，插件读到的却是别的"这种无症状的坑。
            std::string v;
            if (!needValue("--plugin-param", &v)) return 2;
            const std::size_t eq = v.find('=');
            if (eq == std::string::npos || eq == 0) {
                std::fprintf(stderr, "emulator-lift-aot: --plugin-param 需要 k=v 形式（收到 `%s`）\n",
                             v.c_str());
                return 2;
            }
            const std::string key = v.substr(0, eq);
            const std::string value = v.substr(eq + 1);
            if (!plugin_params.emplace(key, value).second) {
                std::fprintf(stderr, "emulator-lift-aot: --plugin-param 的 key 重复：`%s` ⇒ exit 2\n",
                             key.c_str());
                return 2;
            }
        } else if (a == "--ui-input" || a.rfind("--ui-input=", 0) == 0) {
            // ★ S9 / D2：注入点击的叠层路由模式。两种写法都收（help 里写明）：
            //   `--ui-input=consume|pass` 与 `--ui-input consume|pass`。
            //   值域的唯一一份在 `runtime/ui_input.h`（⛔ 这里不另写 consume/pass 比较）。
            std::string v;
            if (a == "--ui-input") {
                if (!needValue("--ui-input", &v)) return 2;
            } else {
                v = a.substr(std::string("--ui-input=").size());
            }
            const lift::UiInputModeParse parsed = lift::parseUiInputMode(v.c_str());
            if (!parsed.ok) {
                std::fprintf(stderr, "emulator-lift-aot: --ui-input 的值非法：`%s`（%s）\n", v.c_str(),
                             parsed.error != nullptr ? parsed.error : lift::kUiInputModeValues);
                return 2;
            }
            ui_input_mode = parsed.mode;
            ui_input_set = true;
        } else if (a == "--cursor-warp") {
            // ★ 与已退役解释器臂同一条：只置一个 bool。窗口参数（headless 下 exit 2
            //   —— 没有可见窗口可映射）。
            cursor_warp = true;
        } else if (a == "--present-screenshot") {
            if (!needValue("--present-screenshot", &present_screenshot)) return 2;
            present_screenshot_set = true;
        } else if (a == "--present-screenshot-frame") {
            std::string v;
            if (!needValue("--present-screenshot-frame", &v)) return 2;
            char* endp = nullptr;
            const unsigned long long n = std::strtoull(v.c_str(), &endp, 10);
            if (endp == v.c_str() || *endp != '\0') {
                std::fprintf(stderr,
                             "emulator-lift-aot: --present-screenshot-frame 需要非负整数：`%s`\n", v.c_str());
                return 2;
            }
            present_screenshot_frame = n;
        } else if (a == "--scale") {
            // ★ 解析即判据（GPU bring-up 之前）：非 `auto` 且非正数 / NaN / Inf / 垃圾 ⇒ exit 2。
            //   拒绝口径与文案的唯一一份在 `runtime/window_scale.h`（⛔ 不在这里另写一套数字判断）。
            std::string v;
            if (!needValue("--scale", &v)) return 2;
            const lift::ScaleParse parsed = lift::parseScaleSpec(v.c_str());
            if (!parsed.ok) {
                std::fprintf(stderr, "emulator-lift-aot: --scale 的值非法：`%s`（%s）\n", v.c_str(),
                             parsed.error != nullptr ? parsed.error : "只能是 auto 或正数");
                return 2;
            }
            scale_spec = parsed.spec;
        } else if (a == "--scale-filter") {
            // ★ 取值 / 拒绝口径的唯一一份在 `runtime/window_scale.h`（`parseScaleFilterSpec`）：
            //   `linear` / `nearest`、大小写敏感、整串精确匹配；其余 ⇒ exit 2 + 点名本参数。
            //   ⛔ 不放宽成"认不出就 linear"：那会让"以为在用 nearest 对照"变成静默的 linear。
            std::string v;
            if (!needValue("--scale-filter", &v)) return 2;
            const lift::ScaleFilterParse parsed = lift::parseScaleFilterSpec(v.c_str());
            if (!parsed.ok) {
                std::fprintf(stderr, "emulator-lift-aot: --scale-filter 的值非法：`%s`（%s）\n", v.c_str(),
                             parsed.error != nullptr ? parsed.error : "只能是 nearest 或 linear");
                return 2;
            }
            scale_spec.filter = parsed.filter;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "emulator-lift-aot: 未知参数：%s\n", a.c_str());
            usage();
            return 2;
        }
    }
    // ★ S1：`--audio` 的值域判据**只有一份**（`runtime/audio_policy.h` 的 `parseAudioMode`）。
    //   ⛔ 不放宽成"认不出就当 off"：非法值仍 exit 2，并**点名合法值域**（人照它改）。
    const lift::AudioModeParse audio_parse = lift::parseAudioMode(audio_mode.c_str());
    if (!audio_parse.ok) {
        std::fprintf(stderr, "emulator-lift-aot: --audio 的值非法：`%s`（%s）\n", audio_mode.c_str(),
                     audio_parse.error != nullptr ? audio_parse.error : "值域 = off / sdl");
        return 2;
    }

    // ---- `--profile-slow-frames`：**必须与 `--profile` 一起给**（⛔ 不静默忽略）----
    //   ★ 为什么是硬错而不是"自动打开 --profile"：那会让"我只想要这一张表"变成"整轮都背上采样
    //     profile 的取时税"，而这条表的墙钟读数是拿 `frame-ms`（`--profile` 的产物）当尺的
    //     ⇒ 两档不可分。⛔ 也不静默当没给：那正是"以为在量、其实没量"的失效形态。
    //   ★ 值 0 也算"给了"：显式关（与不给等价）仍必须带 `--profile`，判据只有一条。
    if (profile_slow_frames_set && !run_opts.profile) {
        std::fprintf(stderr,
                     "emulator-lift-aot: --profile-slow-frames 需要同时给 --profile"
                     "（它的 wall_ms 就是 --profile 的 `frame-ms` 帧间差；0 = 关）\n");
        return 2;
    }

    // ---- 控制层参数校验：`--control-*` 只要给了一个就必须有 `--control-root` ----
    //   ★ 缺省（一个都不给）⇒ 控制层**完全不启动** ⇒ 默认档逐字节不变（判据 1）。
    //   ⛔ 不静默忽略 `--control-port`：那会让人以为在控制、其实端点没起来。
    const bool control_any = !control_root.empty() || control_port_set || control_idle_set ||
                             control_orphan_set || control_predicate_set;
    if (control_any && control_root.empty()) {
        std::fprintf(stderr,
                     "emulator-lift-aot: --control-port / --control-idle-timeout / --control-orphan-timeout / "
                     "--control-predicate-timeout 需要同时给 --control-root <dir>\n");
        return 2;
    }

    // =====================================================================================
    // `--plugin`（C2a）：互斥 + 与 `--ui` 的同一字段收敛 + overlay 合成
    //   ★ 三条都**启动即 exit 2**（不是运行期拒绝）：两条命令行的冲突必须在 GPU bring-up 之前判，
    //     否则用户会先看到一整屏 readout 再看到拒绝（测绘 §3.1 第 4 条 / D-C2-2）。
    // =====================================================================================
    if (!plugin_path.empty()) {
        if (!input_plan_path.empty()) {
            std::fprintf(stderr,
                         "emulator-lift-aot: --plugin 与 --input-plan 互斥（两套输入写预算不能共存；"
                         "--plugin %s / --input-plan %s）⇒ exit 2\n",
                         plugin_path.c_str(), input_plan_path.c_str());
            return 2;
        }
        if (control_any) {
            std::fprintf(stderr,
                         "emulator-lift-aot: --plugin 与 --control-root 互斥（控制层的 click/move 也走"
                         "输入写预算；C4 统一控制面之后再放开）⇒ exit 2\n");
            return 2;
        }
        // ★ 与 `--ui` **同一字段**（D-C2-2）：同一路径 ⇒ 只按**插件**语义（插件不隐含 overlay；
        //   `--ui` 的"隐含 --overlay"在这里必须让位，否则 `--present headless` 会被窗口参数拒绝）。
        if (ui_script == plugin_path) ui_script.clear();
    } else if (!plugin_params.empty()) {
        // ★ F6：`--plugin-param` 是给插件的 ⇒ 没有 `--plugin` 时它**没有消费者**。
        //   ⛔ 不静默忽略（那会让"我明明传了参数、插件却没收到"变成无症状的错）。
        std::fprintf(stderr,
                     "emulator-lift-aot: --plugin-param 需要同时给 --plugin <id|path>"
                     "（收到 %zu 个参数但没有插件）⇒ exit 2\n",
                     plugin_params.size());
        return 2;
    }
    // =====================================================================================
    // S1：`--plugin <id|path>` 的解析（**parse 之后、GPU bring-up 之前**）
    //   ★ 三条行为逐字保留：路径形式原样交给 `initFromFile`（cwd 相对 / 绝对 / 装载失败 exit 2）；
    //     `--plugin` 与 `--ui` 的原字符串比较已经在上面做完；⛔ id 不给 `--ui`。
    //   ★ id 只查两处**非递归**根（`<exeDir>scripts/plugins/` 与编译期 `LIFT_PLUGIN_DIR`），
    //     ⛔ 不查 `lib/`、⛔ 不递归子目录（用户口径 2026-11-25，比设计报告 A2 更硬）。
    //   ★ 歧义 / 找不到都 exit 2（⛔ 不静默取第一个）；命中 ⇒ 换成**绝对路径**并打**一行**日志。
    // =====================================================================================
    if (!plugin_path.empty()) {
        lift::plugin::PluginResolveRoots roots;
        // ★ argv[0] 不含分隔符（PATH 搜索 / 裸命令名）⇒ `exeDirOf` 返回空 ⇒ **发行根整根跳过**，
        //   只查 `LIFT_PLUGIN_DIR`（开发回退；它由 CMake 注入、与 cwd 无关）。
        //   ⛔ 不回退到 cwd（那会让"从哪跑就装哪个脚本"变成隐变量）；纯解析器的
        //   `resolve/id-empty-exe-root` 用例钉住"空 exe 根 = 只剩 dev 根"。
        roots.exe_dir = exeDirOf(args.empty() ? std::string() : args[0]);
        roots.dev_dir = LIFT_PLUGIN_DIR;
        const lift::plugin::PluginResolveResult resolved =
            lift::plugin::resolvePluginSpec(plugin_path, roots);
        if (resolved.status == lift::plugin::PluginResolveResult::Status::Ambiguous) {
            std::fprintf(stderr,
                         "emulator-lift-aot: --plugin id `%s` 命中 %zu 处（exit 2，⛔ 不静默取第一个）：\n",
                         resolved.id.c_str(), resolved.hits.size());
            for (const std::string& hit : resolved.hits) std::fprintf(stderr, "  %s\n", hit.c_str());
            std::fputs("  ⇒ 用路径形式指定唯一一份（含 `.` / `/` / `\\` 的写法一律按路径）\n", stderr);
            return 2;
        }
        if (resolved.status == lift::plugin::PluginResolveResult::Status::NotFound) {
            std::fprintf(stderr,
                         "emulator-lift-aot: --plugin 找不到 id `%s`（exit 2）。搜索过"
                         "（非递归；⛔ 不查 lib/、不递归子目录）：\n",
                         resolved.id.c_str());
            for (const std::string& candidate : resolved.searched) {
                std::fprintf(stderr, "  %s\n", candidate.c_str());
            }
            std::fputs("  （id 只匹配 ^[A-Za-z0-9_-]{1,64}$；含 `.` / `/` / `\\` 的写法一律按路径，"
                       "`.js` 后缀天然是路径）\n",
                       stderr);
            return 2;
        }
        if (resolved.kind == lift::plugin::PluginSpecKind::Id) {
            // ★ 一次性（⛔ 不周期）。打印的是**绝对路径**（解析结果，不重拼字符串）。
            std::fprintf(stderr, "emulator-lift-aot: --plugin id=%s ⇒ %s\n", resolved.id.c_str(),
                         resolved.path.c_str());
            plugin_path = resolved.path;
        }
    }

    // ★ `overlay` 的唯一合成点：显式 `--overlay` **或** 有 HUD 脚本。`--ui` 单给时与旧行为逐字相同
    //   （旧代码在 `--ui` 分支里写 `overlay = true`，等价于这里 `!ui_script.empty()`）。
    overlay = overlay_flag || !ui_script.empty();

    // ---- `--sound`：**接受** + 一条一次性说明（⛔ 不 exit 2、⛔ 不静默）----
    //   ★ 文案与硬事实只在 `runtime/present_flags.h` 的 `kAotSoundNotice`：`--sound` 现在**真的**
    //     进 `AudioEngine` 的静音门（`configure(…, sound)`，见下面的 configureAudio）⇒ 那条话
    //     必须与代码同形（⛔ 不许再说"本臂没有音频管线"）。
    //   ★ 打印点**只有这里**（parse 之后、GPU bring-up 之前）⇒ 一次，且命令行一给就能判
    //     （守卫 `cli-aot-flags` 因此不需要 GPU）。
    if (sound) std::fputs(lift::kAotSoundNotice, stderr);

    // ---- `LIFT_FAULT_DUMP`：**解析在启动期**（早于 `AotHost::init` / 音频线程；fault 上下文只允许
    //   fprintf + committed() + memcpy）----
    // ★ 超限 ⇒ exit 2（⛔ 不静默截断：截断会让人以为 dump 完整）。未设 / 空 ⇒ count=0 ⇒ 零新增输出。
    {
        char dump_err[256] = {};
        if (!lift::initFaultDumpFromEnv(dump_err, sizeof(dump_err))) {
            std::fprintf(stderr, "emulator-lift-aot: %s\n", dump_err);
            return 2;
        }
    }

    // ---- `LIFT_WATCH`：同一条启动期口径（**早于** `AotHost::init` / 音频线程）----
    // ★ 超限 / 语法错 ⇒ exit 2 并点名第几段与原因（⛔ 不静默截断 / 不静默当成普通段）。
    //   未设 / 空 ⇒ count=0 ⇒ 一条 watch 行都不打（热路径只剩一次 load + 分支）。
    {
        char watch_err[256] = {};
        if (!lift::initFaultWatchFromEnv(watch_err, sizeof(watch_err))) {
            std::fprintf(stderr, "emulator-lift-aot: %s\n", watch_err);
            return 2;
        }
    }

    // ---- `--input-plan`：**解析在 GPU bring-up 之前**（CLI 判据不需要 GPU）----
    //   ★ 文件不存在 / 语法错 / 门种类不支持 ⇒ **exit 2 + 点名行号与原因**（⛔ 不许静默忽略）。
    lift::InputPlan input_plan;
    if (!input_plan_path.empty()) {
        std::string plan_err;
        if (!input_plan.loadFile(input_plan_path, &plan_err)) {
            std::fprintf(stderr, "emulator-lift-aot: --input-plan 载入失败：%s\n", plan_err.c_str());
            return 2;
        }
    }

    // ---- 呈现后端参数校验（口径**逐条**照已退役解释器臂的参数校验；⛔ 不发明第三种语义）----
    // ★ `--cursor-warp` 是**平台**能力：Linux / 其它平台没有实现 ⇒ 启动即拒绝（exit 2）。
    //   判据与文案与解释器臂**同一处**（`runtime/present_flags.h`），⛔ 不在这里另写一句。
    if (cursor_warp && !lift::hostCursorPlatformSupported()) {
        std::fprintf(stderr, lift::kCursorWarpUnsupportedFormat, "emulator-lift-aot",
                     lift::hostCursorPlatformName());
        return 2;
    }
    if (present_mode != "headless" && present_mode != "window" && present_mode != "auto") {
        std::fprintf(stderr, "emulator-lift-aot: --present 只能是 headless / window / auto（收到 %s）\n",
                     present_mode.c_str());
        return 2;
    }
    const bool present_window_flag = present_driver_set || present_vsync_set || present_hidden ||
                                     present_screenshot_set || cursor_warp || overlay || !ui_script.empty() ||
                                     ui_input_set;
    if (present_screenshot_frame != 0 && present_screenshot.empty()) {
        std::fprintf(stderr, "--present-screenshot-frame 需要同时给 --present-screenshot <path>\n");
        return 2;
    }
    if (present_mode == "headless" && present_window_flag) {
        std::fprintf(stderr, lift::kHeadlessRejectsWindowFlagsFormat, lift::kWindowFlagsAot);
        return 2;
    }
    if (present_mode != "headless" && present_driver.find("software") != std::string::npos) {
        std::fprintf(stderr,
                     "emulator-lift-aot: ⚠ --present-driver 含 software ⇒ 这是**软件渲染**，不是 GPU 路径"
                     "（只用于测试 / 诊断）\n");
    }

    // ★ B：`--overlay` 的默认 HUD 是**外置脚本**（⛔ 不编进 exe：脚本是用户可自行发挥的 feature）。
    //   解析顺序：① `<exe 所在目录>/scripts/ui/hud.js`（发行布局，**这里**做）
    //             ② `LIFT_UI_SCRIPT_DIR/hud.js`（开发回退，`SdlWindow::loadUiScript` 做）
    //             ③ 都不在 ⇒ 启动硬错（err 点名两个路径；⛔ 不静默回退空 HUD）。
    //   ★ `--ui <path>` 显式给的一律优先：它已经被解析进 `ui_script`，这一步就跳过。
    if (overlay && ui_script.empty()) {
        const std::string candidate = exeDirOf(args.empty() ? std::string() : args[0]) + "scripts/ui/hud.js";
        if (fileExists(candidate)) ui_script = candidate;
    }

    // ---- 宿主字体配置：`--baseline-lift` / `--baseline-lift-by-size` **落在这里** ----
    //   ★ 必须在**任何** `FontEngine` / `SdlWindow` 使用之前：`SdlWindow::init` 会为 `--overlay`
    //     建 HUD 的 `FontEngine`（R5），`AotHost` 里也有引擎 ⇒ 晚于它就不再是"缺省配置"。
    //   ★ 缺省（两个参数都不给）⇒ **不调用** `fontSetConfig`：进程里生效的就是 `fontDefaultConfig()`
    //     那一份（`kBaselineLift` + by_size=true），与"只改代码不动配置"逐位相同。
    //   ★ `fontSetConfig` 自己 bump 代际 ⇒ 已存在的引擎会在下次取字 / 度量时清缓存（`syncConfigEpoch`）。
    if (baseline_lift_set || baseline_lift_by_size_set) {
        lift::FontConfig cfg = lift::fontDefaultConfig();
        if (baseline_lift_set) cfg.baseline_lift = baseline_lift;
        if (baseline_lift_by_size_set) cfg.baseline_lift_by_size = baseline_lift_by_size;
        lift::fontSetConfig(cfg);
    }

    // =====================================================================================
    // 显示 / GPU 光栅化后端（真源是已退役解释器臂的**同一条**路径）
    //
    // ★ `--present headless` **不是"没有窗口"**：解释器也建一个**隐藏**窗口来拿 GPU device
    //   （已退役解释器臂的同一条），因为 GPU 是**唯一**的光栅实现 —— 拿不到它，`d3d9_pipeline.cpp:3983`
    //   会整批拒批，3D 一个像素都不画（W0 C10 Ⓐ）。⇒ 本臂两个档位都建窗口，只有 visible/hidden 之分。
    // ★ 无 GPU ⇒ **拒绝启动**（与解释器同一条硬失败，⛔ 没有"这一轮先不画"的降级）。
    // =====================================================================================
    std::unique_ptr<lift::SdlWindow> display;
    {
        lift::SdlWindowOptions wopt;
        wopt.title = "emulator-lift-aot";
        wopt.driver_list = present_driver;
        wopt.vsync = present_vsync != 0;
        // headless / --present-hidden ⇒ 隐藏窗口（`--present headless` 不出现可见窗口）。
        wopt.hidden = present_mode == "headless" || present_hidden;
        wopt.screenshot_path = present_screenshot;
        wopt.screenshot_frame = present_screenshot_frame;
        // ★ 与解释器臂同一个字段（已退役解释器臂同一个字段）：缺这一行 ⇒ sink 恒 `cursor_warp=false`
        //   ⇒ `hostCursorWarpEnabled()` 恒假 ⇒ shim 只改虚拟光标（用户看到的"不会移动鼠标"）。
        wopt.cursor_warp = cursor_warp;
        // ★ M0：`--overlay` 的唯一接线点（`SdlWindow` 内部两条呈现路径统一到 `presentOverlay`）。
        //   ★ B：`ui_script` 非空 = 用 `--ui <path>` 那份脚本；空 = 默认外置 `scripts/ui/hud.js`。
        //   `--present headless` 已在上面按 `present_window_flag` 拒绝。
        wopt.overlay = overlay;
        wopt.ui_script = ui_script;
        // ★ S9：注入点击的叠层路由模式（`--ui-input=consume|pass`；缺省 Consume）。
        //   没有 `--overlay` / `--ui` ⇒ sink 那边根本不装 router ⇒ 两种模式都是无害 no-op。
        wopt.ui_input = ui_input_mode;
        // ★ `--scale` 只交给**窗口档**：`--present headless` 的隐藏离屏窗口保持 1280×720
        //   （= `--scale` 对 headless 无副作用；不设它 ⇒ 不查 DPI、不改窗口尺寸）。
        if (present_mode != "headless") wopt.scale = scale_spec;
        wopt.raster_gpu = true;
        auto sdl = std::make_unique<lift::SdlWindow>();
        std::string werr;
        bool ok = sdl->init(wopt, &werr);
        if (!ok && present_mode == "auto") {
            // `--present auto`：可见窗口打不开（无桌面）⇒ 退到隐藏的离屏窗口（GPU 仍必须成立）。
            std::fprintf(stderr,
                         "emulator-lift-aot: --present auto：可见窗口打不开（%s）⇒ 改用隐藏的离屏窗口\n",
                         werr.c_str());
            if (!present_screenshot.empty()) {
                std::fprintf(stderr, "  ⚠ --present-screenshot 不会产出（没有可见窗口可抓）\n");
            }
            wopt.hidden = true;
            werr.clear();
            ok = sdl->init(wopt, &werr);
        }
        // ★ "我开了但没生效"必须响亮一次（与解释器臂同一句话、同一判据；见
        //   `runtime/present_flags.h`）：`--present-hidden` 与 auto 的隐藏回退走同一条 stderr warn。
        //   ⛔ 不改成 exit 2：隐藏窗口是有意的无窗口复现档（已退役解释器臂的同一裁决）。
        if (ok && lift::cursorWarpState(cursor_warp, lift::hostCursorPlatformSupported(), wopt.hidden) ==
                      lift::CursorWarpState::OnButHidden) {
            std::fputs(lift::kCursorWarpHiddenWarn, stderr);
        }
        if (!ok || !sdl->rasterActive()) {
            // ★ 标题按 `ok` 分（原因行照打，⛔ 不吞）：
            //   · `!ok` = `SdlWindow::init` 拒绝 ⇒ 窗口 / 字体（`--overlay` 的宿主字体，R5）/ GPU device；
            //   · `ok && !rasterActive` = device 建出来了但光栅后端没激活 ⇒ 这才是"没有可用的 GPU 后端"。
            //   ⛔ 不改 exit 2、不改判据（`lift/no-gpu-refuses-start` 仍靠"拒绝启动" + 原因里的 GPU 文案）。
            if (!ok) {
                std::fprintf(stderr,
                             "emulator-lift-aot: 拒绝启动 —— 窗口/字体初始化失败\n"
                             "  原因：%s\n"
                             "  请检查：\n"
                             "    · `--overlay` / `--ui` 需要宿主字体（`LIFT_HOST_FONT_PATH`，缺 ⇒ 硬错、\n"
                             "      ⛔ 不静默画空白）与可用的 HUD 脚本（`--ui` 的语法错 / 找不到入口\n"
                             "      也走这一处）；\n"
                             "    · 窗口 / GPU device 是否建得出来（Windows: D3D12 / Vulkan；macOS: Metal）；\n"
                             "    · 是否在**没有桌面**的环境里跑（远程会话 / 服务 / `SDL_VIDEODRIVER=dummy`）。\n",
                             werr.empty() ? "窗口初始化失败（SDL 没有给出原因）" : werr.c_str());
                return 2;
            }
            std::fprintf(stderr,
                         "emulator-lift-aot: 拒绝启动 —— 没有可用的 GPU 光栅化后端\n"
                         "  原因：GPU device 建出来了，但光栅化后端未激活（`rasterActive()==false`）\n"
                         "  本线只有 GPU 一个光栅实现（CPU 光栅已退休）；没有 GPU 就不启动。\n"
                         "  请检查：\n"
                         "    · 图形后端 / 驱动是否可用（Windows: D3D12 / Vulkan；macOS: Metal）；\n"
                         "    · 是否在**没有桌面**的环境里跑（远程会话 / 服务 / `SDL_VIDEODRIVER=dummy`）：\n"
                         "      SDL 必须先初始化视频子系统才能建 GPU device ⇒ 这种环境请换到有桌面的会话里跑。\n"
                         "  ⛔ 本线不提供无 GPU 时的降级路径（`--present headless` 也要 GPU）。\n");
            return 2;
        }
        display = std::move(sdl);
    }
    // ★ 指向那个窗口对象的**稳定**指针：window 档位稍后把 `display` move 进 presenter 的 sink，
    //   但对象本身仍活着（所有权转给 presenter）⇒ describe() / attachWindowManager / 事件泵都走它。
    lift::SdlWindow* const sdl_display = display.get();

    // ---- 定位执行源（与 emulator-lift 同一套顺序；⛔ 不回退到安装树里的加壳原件）----
    std::vector<std::string> tried;
    {
        std::string resolved;
        if (!resolveExePath(exe_path, &resolved, &tried)) {
            std::fprintf(stderr, "emulator-lift-aot: 找不到执行源（拆壳件的字节）。找过：\n");
            for (const std::string& t : tried) std::fprintf(stderr, "  - %s\n", t.c_str());
            std::fprintf(stderr, "  用 --exe <path> 指定，或先 `git lfs pull`（入库件在 corpus/assets/age/）。\n");
            return 2;
        }
        exe_path = resolved;
    }

    // ---- guest 文件系统的根（口径同 main.cpp：--game-dir → env → 本机安装树探测）----
    if (game_dir.empty()) game_dir = envOr("AMAYUI_GAME_DIR");
    if (game_dir.empty()) game_dir = envOr("AMAYUI_INSTALL");
    if (game_dir.empty()) {
        const char* fallback = "E:\\Games\\Eushully\\天結";
        if (fileExists(std::string(fallback) + "\\AGE.EXE")) game_dir = fallback;
    }

    lift::aot::AotHost host;
    std::string err;

    // ---- ① 平台 + guest 地址空间 + 宿主服务装配 ----
    if (!host.init(&err)) {
        std::fprintf(stderr, "emulator-lift-aot: 宿主装配失败：%s\n", err.c_str());
        return 1;
    }
    // ---- ①' S1/S2：CLI 的音频三参数落到音频引擎（`init` 里的缺省是 `configure(off,"",false)`）----
    //   ★ 位置：`init()` 之后、`run()` 之前（`AudioEngine::configure` 要求"还没 begin"）。
    //   ★ 缺省档（不给 `--audio` / `--audio-dump` / `--sound`）⇒ 这里是 `configure(off,"",false)`，
    //     与 `init()` 里那次逐位相同 ⇒ 默认档输出逐字不变。
    if (!host.configureAudio(audio_parse.mode, audio_dump, sound, &err)) {
        std::fprintf(stderr, "emulator-lift-aot: 音频后端 configure 失败：%s\n", err.c_str());
        return 1;
    }
    // ★ GPU 光栅化后端：**必须在第一条 guest 指令之前**接到宿主服务上（口径已退役解释器臂的
    //   `machine.setGpuRaster(display->gpuRaster())`）。d3d9 / DDraw 的第一件事就是查它；
    //   拿不到 ⇒ `d3d9_pipeline.cpp:3983` 整批拒批（3D 一个像素都不画，`~4544` 不再 CPU 重放）。
    host.setGpuRaster(sdl_display->gpuRaster());
    // ★ VFS 的三条配置必须在 loadImage/prepare 之前（prepare 里的 `setupProcessStructures` 会用
    //   `vfs.guestExePath()` 与 `vfs.guestEnvironment()` 造 guest 的命令行 / 环境块）。
    if (!game_dir.empty()) host.vfs().setGameDir(game_dir);
    if (!appdata_dir.empty()) host.vfs().setAppDataDir(appdata_dir);
    if (!guest_root.empty()) host.vfs().setGuestRoot(guest_root);
    // ★ C7：只读安装树内容缓存（口径见 `runtime/vfs.h` 的 C7 段）。`--vfs-cache-mb` 只改预算；
    //   "这条路径该不该缓存"的判据在 shim（纯读 + OPEN_EXISTING + 写覆盖层 + 安装树之下）。
    //   ⚠ 没有 `--appdata` 时缓存**天然不生效**（安装树那时是可写目标，只读前提不成立）。
    if (vfs_cache_mb_set) host.vfs().setReadCacheBudgetMb(vfs_cache_mb);  // 缺省已在 Vfs 里 = 256 MiB
    // ★ C5：安装树目录索引（口径见 `runtime/vfs.h` 的 C5 段）。`--vfs-dir-index 0` 只供 A/B 与守卫；
    //   "能不能用"还要看 `--appdata`（安装树只读那条前提在 `Vfs::dirIndexUsable()` 里）。
    host.vfs().setDirIndexEnabled(vfs_dir_index);

    // ---- ② 载入 PE（**先核对 D1 的 pin**，再映射进 guest）----
    if (!host.loadImage(exe_path, &err)) {
        if (host.pinMismatch()) {
            // ★ D1：打印两个哈希 + exit 2（⛔ 不降级、不警告后继续）。
            std::fprintf(stderr, "emulator-lift-aot: exe pin 核对失败\n%s\n", err.c_str());
            return 2;
        }
        std::fprintf(stderr, "emulator-lift-aot: 载入 PE 失败（%s）：%s\n", exe_path.c_str(), err.c_str());
        return 1;
    }

    // ---- ③ 装配 guest 进程结构 + shim + cpu 初始寄存器 ----
    if (!host.prepare(&err)) {
        std::fprintf(stderr, "emulator-lift-aot: 平台 bring-up 失败：%s\n", err.c_str());
        return 1;
    }

    // ---- ③'' `--input-plan` 装配（`WindowManager` = 唯一状态源；`presenter` 只为摘要读数留）----
    //   ★ 两处都只是**非拥有指针**。计划不给**且**控制层不开 ⇒ 两处都不装配 ⇒ 读点只付一条判空、
    //     输出逐字节不变。
    //   ★ **M1 起多一个装配条件**：控制层开着就必须装 —— `click` 的施加点是 `InputPlan::atPoint`
    //     （同一个 `InputPlan`），而"没有任何配方"的运行也必须能投一条点击。
    //     代价：那种运行里每个输入读点多两次计数器自增（⛔ 不打印、不改 guest 行为）。
    //   ★ ⛔ 不装在 `aot_run.cpp` / `machine.cpp` 里（守卫 ① 钉住那 6 个词 = 0 命中）。
    //   ★ 身份来源（`await script` 的门）在这里挂：`host.mem()` 只有在 `init()` 之后才可读
    //     （解析在 bring-up 之前 ⇒ 解析期⛔ 不采身份）。`atPoint()` 签名不变。
    if (input_plan.armed() || control_any) {
        input_plan.attachGuestMemory(&host.mem());
        // ★ 暂停补偿（S1）：配方的每步守卫钟 = 宿主单调毫秒 − 累计暂停毫秒。
        //   缺省（没有控制层 / 没暂停过）`pausedTotalMs() == 0` ⇒ 与改动前逐位相同。
        input_plan.attachClock(&host.clock());
        // ★ `--input-plan-trace`（opt-in）：缺省 `nullptr` ⇒ 一条都不打（默认档输出逐字节不变）。
        if (input_plan_trace) input_plan.setTrace(stderr);
        // ★ fault 路径的摘要（C）：只有**有配方**时才装 ⇒ 没有配方的 fault 文本逐字不变。
        //   位置与其余装配点同处（`guestFault` 在本进程里随时可能发生；run 之前装好）。
        if (input_plan.armed()) {
            lift::setGuestFaultTextHook(&input_plan, &faultPlanSummary);
        }
        host.cpu().windows->setInputPlan(&input_plan);
        // `present.cpp` 的 `note(Frame)` 保留：它只是摘要 `presents=` 的读数，⛔ 不再是门。
        host.presenter().setInputPlan(&input_plan);
        // ★ **回调边界**（2026-11-21）：`note(Frame)` 这条线在本臂是**活的** ⇒ 抬起还必须在按下之后
        //   跨过一个完整的 guest 回调（`PointerCore::kCallbackFramesBeforeRelease`；证据见
        //   `pointer_core.h` 的闸门 ②'）。⛔ 纯夹具 / 插件 executor 没有这条线 ⇒ 不置位 = 旧行为。
        input_plan.requireCallbackBoundary();
        // ★ 窗口 profile 的标记钩子（`profile_begin/end` ⇒ 开/收窗 + 打桶表）。
        //   装配位置与其余 `attach*` 同处：`AotHost` 已在作用域里，钩子只在**有配方**时才有意义。
        //   ⛔ 不改配方语法：钩子只消费既有的 `profile_begin` / `profile_end` 两步。
        input_plan.setProfileMarkHook(&host, &lift::aot::AotHost::windowMarkThunk);
        // ★ `quit` 的**宿主停止请求**（唯一生产者 = 配方里的 `quit`）：run 循环每个块边界读它 ⇒
        //   本次运行以 `Stopped` 收口（与解释器臂 `Machine::run` 读 `actuator().quitRequested()`
        //   同一条口径）。⛔ 不等 guest 自己退出（它的停机路径可能不在闭包里）。
        run_opts.stop_requested = input_plan.quitFlag();
    }

    // ---- ③''' `--plugin` 装配（C2a）：编译 + 沙箱 → guest 内存 → 读点观察者 → 停止请求 ----
    //   ★ 位置与 `--input-plan` 的装配同一处（`init()` / `prepare()` 之后、`run()` 之前）：
    //     两条命令面互斥（D-C2-2），所以这里只有一条会装配。
    //   ★ 顺序照测绘 §3.3：`init`（= 编译 + 沙箱，⛔ 不调 start）→ `attachMemory`（Edit/Read）→
    //     `setReadPointObserver`（装配点必须在**循环外**）→ `stop_requested`。
    //   ★ ⛔ 不装在 `aot_run.cpp` / `machine.cpp`（守卫：执行循环里 0 命中 pump 词）。
    std::unique_ptr<lift::plugin::PluginReadings> plugin_readings;
    std::unique_ptr<lift::plugin::PluginHost> plugin_host;
    if (!plugin_path.empty()) {
        // 生产读数源：marker 身份 / 位置 + 鼠标（每读点一次；与 `InputPlan` 的采样互斥）。
        plugin_readings = std::make_unique<lift::plugin::PluginReadings>(host.mem(), *host.cpu().windows);
        plugin_host = std::make_unique<lift::plugin::PluginHost>(*plugin_readings);
        // ★ `ctx.appdata`（2026-11-25）：根由宿主绝对化（`Vfs::setAppDataDir` 存的是 CLI 原串）。
        //   没有 `--appdata` ⇒ `available=false`（正常）；根不可用 / 落在安装树 ⇒ 一行 warn + 继续
        //   —— 插件能力缺失不是启动失败（⛔ 不 exit 2）。
        {
            std::string appdata_err;
            if (!plugin_host->attachAppData(appdata_dir, game_dir, &appdata_err)) {
                std::fprintf(stderr, "emulator-lift-aot: [plugin] ctx.appdata 不可用：%s\n",
                             appdata_err.c_str());
            }
        }
        // ★ `ctx.config`（2026-11-25）：`<--appdata>/plugin-config.json`（appdata 根；⛔ 不在 SAVE/）。
        //   必须在 `initFromFile` **之前**读：配置文件的形状坏 = **装载失败**（exit 2，与
        //   `--plugin-param` 形状错误同口径），⛔ 不像 appdata 那样降级。没有 `--appdata` ⇒
        //   `available=false`（正常；插件自己回退到 params / 默认值）。
        {
            std::string config_err;
            if (!plugin_host->attachConfig(&config_err)) {
                std::fprintf(stderr, "emulator-lift-aot: --plugin 配置装载失败（exit 2）：%s\n",
                             config_err.c_str());
                return 2;
            }
        }
        std::string plugin_err;
        if (!plugin_host->initFromFile(plugin_path, plugin_params, host.uiHostOps(), &plugin_err)) {
            std::fprintf(stderr, "emulator-lift-aot: --plugin 装载失败（exit 2）：%s\n", plugin_err.c_str());
            return 2;
        }
        // ★ Edit / EngineRead：`host.cpu().mem`（`init()` 之后才可读；`--input-plan` 的 attach 同处）。
        plugin_host->attachMemory(*host.cpu().mem);
        // ★ 读点观察者（C2a 的唯一泵；`input.h` 四个投递口 → `PluginHost::onReadPoint`）。
        host.cpu().windows->setReadPointObserver(plugin_host.get());
        // ★ **回调边界**（2026-11-22）：插件 executor 与 `--input-plan` 走**同一条**帧线 ——
        //   `presenter.frameCount()`（`present.cpp` 在 `++frame_count_` 同一刻推 `Frame`）。
        //   ⇒ `ctx.input.click` 的抬起同样要跨过一个完整 guest 回调（闸门 ②′；阈值只有
        //   `PointerCore::kCallbackFramesBeforeRelease` 一份）。⛔ 不按插件 / 场景开特判。
        plugin_host->executor().setFrameSource(&host, &presentFrameCount);
        // ★ `ctx.quit()` 的归因（D-C2-9）：run 循环在下一个块边界看到 `Stopped` + `plugin: quit`。
        run_opts.stop_requested = plugin_host->quitFlag();
        run_opts.stop_detail = "plugin: quit";
        // ★ D-C2-5：有 `render` 但 headless ⇒ **允许**执行（`render` 永不调用），一次性 stderr 说明。
        //   ⛔ 不 exit 2：CLI 阶段还不知道模块有没有 render；启动后知道了也只警告一次。
        // ★ C2b-2：窗口档的 adopt **不在这里** —— 它要等 ⑤ 的 `setSink` / `attachWindowManager`
        //   之后（那时 sink 才真的能画、命中路由才有 `WindowManager`）。见 ⑤ 的本单块。
        if (present_mode == "headless" && plugin_host->script() != nullptr &&
            plugin_host->script()->hasRender()) {
            std::fputs("[plugin] render is defined but no window; it will not be called\n", stderr);
            std::fflush(stderr);
        }
    }

    // ---- ③' AGERC.DLL：真实 guest 模块（口径同已退役解释器臂 `main.cpp`：`--game-dir` 下那份，走 VFS 解析）----
    //   ★ 缺它不是"可选项"：guest 启动早期就 `LoadLibraryA("AGERC.DLL")`，没有镜像 ⇒ 返回 0 + 126
    //     ⇒ guest 走错误对话框路径（实测停在 `poly:005C3627 = j_MessageBoxA_15`）。
    //   ⛔ 与解释器臂同口径：**降级但不假装成功**（打一行 warn，LoadLibrary 自己会返回 0 + 126）。
    if (host.vfs().ready()) {
        std::string agerc_host;
        std::string verr;
        if (host.vfs().resolve("AGERC.DLL", &agerc_host, &verr, true, true)) {
            if (!host.loadAgercModule(agerc_host, &err)) {
                std::fprintf(stderr,
                             "emulator-lift-aot: ⚠ AGERC.DLL 装载失败 ⇒ 继续（降级为\"没有 AGERC 镜像\"）\n"
                             "  %s\n  ⇒ LoadLibrary(\"AGERC.DLL\") 会返回 0 + 错误 126\n",
                             err.c_str());
            }
        } else {
            std::fprintf(stderr,
                         "emulator-lift-aot: ⚠ --game-dir 下没有可解析的 AGERC.DLL（%s）⇒ "
                         "LoadLibrary(\"AGERC.DLL\") 会返回 0 + 错误 126\n",
                         verr.c_str());
        }
    }

    // ---- ④ 生成物在不在场：**响亮**报告（⛔ 不许静默跑成空壳）----
    if (!lift::aot::generatedPresent()) {
        std::fprintf(stderr,
                     "\n"
                     "⛔ emulator-lift-aot: **没有生成物**\n"
                     "   构建期缺 `clang` / 执行源（corpus/assets/age/AGE.EXE__dumped.sectfix.EXE）/\n"
                     "   `aot/closure.txt` 的任一项 ⇒ 这个 exe 里**没有任何 lifted 函数**，派发表为空\n"
                     "   （构建仍然成功，但**不能执行**）。⛔ 不静默跑成空壳：只打 bring-up 读数后 exit 1。\n"
                     "   修：装上 clang（`clang --version`）并确认执行源在场后重新 configure / build。\n\n");
        std::printf("emulator-lift-aot: Shape A —— bring-up 读数（**没有生成物，未执行任何 guest 指令**）\n");
        host.printReadout(stdout, run_opts.region_log);
        std::fflush(stdout);
        return 1;
    }

    // ---- ④' AOT **形状**进二进制 + 启动第一屏一行（设计 §4.1-4）----
    //   `LIFT_AOT_SHAPE` 由 CMake 从 `LIFT_AOT_SPLIT` + `-O2`（+ LTO 档的 `-lto` / `-abi` …）派生：
    //   OFF 档是 `single-O2` / `splitN-O2`；LTO 档带 `-lto` 后缀（默认 mac = `split8-O2-lto-abi`）。
    //   ⇒ 横幅的 `-O2` 之后按 shape 里有没有 `-lto` 派生 ` · LTO`（⛔ 不写死，否则 LTO 档会变半句假话）。
    //   将来真有 dev 档，它的值必须带 `-NOT-A-JUDGE` ⇒ 这里同时打警示，让人**无法**在 dev 档上得出性能结论。
    {
        const char* shape = LIFT_AOT_SHAPE;
        const bool lto = std::strstr(shape, "-lto") != nullptr;
        std::printf("emulator-lift-aot: AOT 形状 LIFT_AOT_SHAPE=\"%s\"（split=%d · -O2%s）\n", shape,
                    static_cast<int>(LIFT_AOT_SPLIT), lto ? " · LTO" : "");
        if (std::strstr(shape, "-NOT-A-JUDGE") != nullptr) {
            std::printf("⚠ 非判据档（⛔ 性能结论不可用）\n");
        }
    }

    // ---- 宿主诊断配置：帧导出 / 帧数上限（口径同已退役解释器臂 `main.cpp`；不进 guest 语义）----
    host.cpu().shim.frame_dump = frame_dump;
    host.cpu().shim.max_frames = run_opts.max_frames;
    if (!frame_dump.empty()) {
        std::printf("frame-dump: %s（每次 Present 导出 guest 后缓冲 BMP；{frame} = 4 位帧号）\n",
                    frame_dump.c_str());
    }

    // ---- ⑤ 窗口 sink 装配（只在 `--present window/auto`；口径同已退役解释器臂 `main.cpp`）----
    //   ★ 三件事缺一不可（W0 B7）：
    //     ① `presenter().setSink(...)`：像素真的送到窗口；
    //     ② `attachWindowManager`：SDL 事件才翻得成 guest 的 `WM_*`（没有它事件只更新 stop）；
    //     ③ `setHostEventPump`：guest 阻塞在 `GetMessageA`（空队列）时代替它等事件 ——
    //        ⛔ 不装 ⇒ `user32_window.cpp:589` 的 `shimAbort` → `std::abort()`（不可归因停机）。
    bool present_window_active = false;
    if (present_mode != "headless") {
        if (!sdl_display->isGpuRenderer()) {
            std::printf("present   : 注意：driver=%s 是 GPU API 路径，但不是 SDL_GPU（gpu driver）\n",
                        sdl_display->rendererName());
        }
        host.presenter().setSink(std::move(display));  // 所有权转给 presenter（与已退役解释器臂同）
        sdl_display->attachWindowManager(host.cpu().windows);
        // ---- ⑤' C2b-2：把插件的 `render` 接进**已有**的叠层 host（⛔ 不新开第二套 UI 运行时）----
        //   ★ 位置：必须在 `setSink` / `attachWindowManager` **之后**（sink 真能画、命中路由有 wm），
        //     在读数接线之前（`if (sdl_display->overlayEnabled())` 才能看到 adopt 打开的那一档）。
        //   ★ 同一个 `PluginScript` 实例既跑 Run 回调、又画本帧（它实现 `UiScript`）；`adopt` 会丢掉
        //     `--overlay` 先装的 hud.js ⇒ 同一时刻只有一个 QJS context（合同 §6 行 9 / 11）。
        //   ★ `--plugin`（有 render）**不隐含** `--overlay`（D-C2-5：headless 照样能跑）⇒ 窗口档由
        //     这里打开叠层；headless 走上面的 D-C2-5 警告，不 adopt、不建字体。
        if (plugin_host != nullptr && plugin_host->script() != nullptr &&
            plugin_host->script()->hasRender()) {
            std::string font_err;
            lift::FontEngine* const font = sdl_display->ensureOverlayFont(&font_err);
            if (font == nullptr) {
                std::fprintf(stderr,
                             "emulator-lift-aot: --plugin 的 render 需要宿主字体（exit 2）：%s\n",
                             font_err.c_str());
                return 2;
            }
            // ★ 字体**在装载之后**才接线（装载时还不知道模块有没有 render）⇒ late setter 写的是
            //   与 `PluginLoadOptions::font` 同一个字段（`PluginScript::setFont`）。
            plugin_host->setFont(font);
            std::string adopt_err;
            if (!sdl_display->adoptOverlayScript(*plugin_host->script(), &adopt_err)) {
                std::fprintf(stderr, "emulator-lift-aot: --plugin 的 render 接不进叠层（exit 2）：%s\n",
                             adopt_err.c_str());
                return 2;
            }
            // ★ `--overlay` 同时给了 ⇒ 用户以为看的是 hud.js，实际被插件 render 取代（同一个叠层
            //   host）。一次性说清，⛔ 不静默换内容。
            if (overlay) {
                std::fputs("[plugin] --overlay 的 HUD 已被插件的 render 取代（同一个叠层 host；"
                           "⛔ 不跑第二套 UI 运行时）\n",
                           stderr);
                std::fflush(stderr);
            }
        }
        // ★ 帧列表读数源（`game.local_frames()`）：叠层**真的开着**时接一次（脚本按需查询，⛔ 不每帧推表）。
        //   ★ 判据用 sink 自己的状态（`overlayEnabled()`）而不是 CLI 的 `overlay`：C2b-2 的 adopt
        //     会在窗口档打开叠层（`--plugin` 有 render、没给 `--overlay`）⇒ 那一刻它必须也接上
        //     帧列表 / 宿主能力表，否则同一份 RenderCtx 的成员表按 CLI 旗标分叉。
        //   `sink()` 可能为空（`setSink` 失败 / 被换掉）⇒ 判空。
        if (sdl_display->overlayEnabled()) {
            if (lift::PresentSink* sink = host.presenter().sink()) {
                sink->setUiFrameSource(host.frameSource());
                // ★ 宿主能力表（`host.profileBegin/End`；切片 3）：与 frame source 同一装配点。
                //   生命周期 = `AotHost` 成员；sink 由 `presenter()` 拥有 ⇒ 先于 host 析构。
                sink->setUiHostOps(host.uiHostOps());
            }
        }
        // ★ AOT 没有输入脚本 / 交互图（那是解释器臂的宿主控制面）⇒ 泵里只有 SDL 事件。
        host.cpu().windows->setHostEventPump(
            [sdl_display](lift::X86& /*cpu*/) { sdl_display->pumpHostEvents(5); });
        present_window_active = true;
    }

    std::printf("emulator-lift-aot: Shape A —— bring-up 完成，开始执行（派发表 %zu 项）\n",
                lift::aot::tableSize());
    // ★ 启动回声：两行都是**读数**（⛔ 不写死）：
    //   · `raster` = 真正的光栅后端名（GPU 是唯一实现）；
    //   · `present` = sink 的 `describe()`，其中 `window=visible|hidden` 是"这轮看得见吗"的机械判据
    //     （取自 `SDL_GetWindowFlags`；headless 也建隐藏窗口 ⇒ 这一行**两个档位都打**）。
    std::printf("raster    : %s%s\n", host.gpuRaster() != nullptr ? host.gpuRaster()->rasterName() : "(nullptr)",
                present_mode == "headless" ? "（headless 离屏；**隐藏**窗口，不出现可见窗口）" : "");
    std::printf("present   : %s\n", sdl_display->describe().c_str());
    // ★ C5/C8 的**一次性**启动 readout（§12.8 第 3 条：一次性输出 ⛔ 不许收；缺省档也打）：
    //   "这一趟路径解析的两层记忆是开还是关"（读数**从 Vfs 自己的状态取**，⛔ 不抄 CLI 变量）。
    //   `dir-index` 的 on/off 是**开关**；它是否真的生效还要看写覆盖层（`--appdata`）。
    {
        const lift::Vfs& vfs = host.vfs();
        std::printf("vfs-paths : dir-index=%s resolve-cache=%s（安装树目录索引 / 正解析缓存；"
                    "A/B: --vfs-dir-index 0）\n",
                    vfs.dirIndexEnabled() ? "on" : "off", vfs.positiveCacheEnabled() ? "on" : "off");
    }
    // ★ `--cursor-warp` 的**有效状态**一行（缺省关）：`on` / `off` / `on-but-hidden` / `unsupported`。
    //   与 `describe()` 里的 `cursor-warp=on|off`（sink 自己的开关）分工不同 —— 这一行回答
    //   "这轮真的会不会挪宿主光标"（含"开关开着但窗口是隐藏的"这种静默失效）。
    //   ★ 判据与四个状态名只有一份（`runtime/present_flags.h`），与解释器臂逐字相同。
    {
        const lift::CursorWarpState st =
            lift::cursorWarpState(cursor_warp, lift::hostCursorPlatformSupported(), sdl_display->windowHidden());
        std::printf("cursor-warp: %s\n", lift::cursorWarpStateName(st));
    }
    // ★ `--scale` 的**一次性**启动 readout（§12.8 第 3 条：一次性输出 ⛔ 不许收；缺省 1.0 也打）。
    //   · 窗口档：事实来自 sink —— 窗口**建完之后** `SDL_GetWindowSize` 回读的客户区尺寸
    //     （⛔ 不是把请求值抄回来；窗口管理器可能约束尺寸）；
    //   · headless：`--scale` **接受但不生效**（隐藏离屏窗口仍 1280×720、也不查 DPI）⇒ 把
    //     `applied` 置 false，由 `runtime/window_scale.h` 的**同一份**文案打出"不生效"。
    //   · `auto` 取不到 display content scale ⇒ 同一行里写明"SDL 返回 0 ⇒ 回 1.0"（⛔ 不静默）。
    {
        lift::ScaleReport scale_report = sdl_display->scaleReport();
        if (present_mode == "headless") {
            scale_report.applied = false;
            scale_report.auto_requested = scale_spec.kind == lift::ScaleSpec::Kind::Auto;
            scale_report.scale = scale_spec.value;
            scale_report.filter = scale_spec.filter;  // 不生效但仍如实填（⚠ readout 不印这一格）
            scale_report.window_w = lift::kBaseWindowW;
            scale_report.window_h = lift::kBaseWindowH;
        }
        std::printf("%s\n", lift::scaleReadoutLine(scale_report).c_str());
    }
    host.printReadout(stdout, run_opts.region_log);
    // ★ 构建期库内建的**一次性**启动 readout（§12.8 第 3 条：一次性输出 ⛔ 不许收）：
    //   这一趟的二进制到底带了哪几个内建 EA（来自 `lift_pin.h` 的已校验列表，⛔ 不在运行期读表文件）。
    //   `--no-intrinsics` 只关闸门；IR 里的闸门块 / 快路径是构建期决定的 ⇒ 这里如实说 "off"。
    {
        const std::size_t ni = lift::aot::pinnedIntrinsicCount();
        std::string line = "intrinsics: ";
        if (ni == 0) {
            line += "none（本二进制没有构建期内建表）";
        } else {
            line += run_opts.no_intrinsics ? "off(--no-intrinsics)" : "on";
            line += "  table=";
            for (std::size_t i = 0; i < ni; ++i) {
                const lift::aot::AotIntrinsic it = lift::aot::pinnedIntrinsic(i);
                char b[96];
                std::snprintf(b, sizeof(b), "%s0x%08X/%s", i == 0 ? "" : ",",
                              static_cast<unsigned>(it.ea), it.name);
                line += b;
            }
            char b[64];
            std::snprintf(b, sizeof(b), "  fast-max=%u B  [A/B: --no-intrinsics]",
                          static_cast<unsigned>(lift::kMemcpyFastMaxBytes));
            line += b;
        }
        std::printf("%s\n", line.c_str());
    }
    std::fflush(stdout);

    // =====================================================================================
    // ⑤' 控制层端点（`--control-root`；缺省**完全不启动** ⇒ 默认档逐字节不变）
    //   ★ 起在 GPU bring-up / prepare / 生成物检查**之后**：没有可跑的东西就不该监听端口。
    //   ★ `exe_sha256` 取 `host.image().sha256`（与 pin 同源）；`present` 取 `windowHidden()`
    //     （⛔ 不由 `--present` 推：`auto` 会静默回落隐藏窗 —— `design-review.md` §2-2）。
    //   ★ 监听失败 ⇒ **exit 2 + 点名端口**（判据 12）；⛔ 不静默换端口、⛔ 不降级成"没有控制层"。
    // =====================================================================================
    lift::Control control;
    bool control_started = false;
    if (control_any) {
        lift::Control::Options copts;
        copts.root = control_root;
        copts.port = static_cast<std::uint16_t>(control_port);
        copts.idle_timeout_s = control_idle_s;
        copts.orphan_timeout_s = control_orphan_s;
        copts.predicate_timeout_s = control_predicate_s;
        copts.exe_sha256 = host.image().sha256;
        copts.shape = LIFT_AOT_SHAPE;
        copts.present = sdl_display->windowHidden() ? "hidden" : "visible";
        std::string ctl_err;
        if (!control.start(copts, &ctl_err)) {
            std::fprintf(stderr, "emulator-lift-aot: 控制端点启动失败（exit 2）：%s\n", ctl_err.c_str());
            return 2;
        }
        control_started = true;
        host.setControl(&control);
        // ★ `--input-plan-trace` 的 `state_seq=` 来源（唯一生产者 = 控制层）。装在这里是因为
        //   `control` 要到上面 `start()` 之后才存在；没开控制层 ⇒ 不装 ⇒ trace 打 `-`（⛔ 不编造）。
        if (input_plan_trace) input_plan.setStateSeqSource(&control, &controlStateSeq);
        // ★ `present` 的事实来源只有一处（`windowHidden()`，⛔ 不由 `--present` 推：`auto` 会静默
        //   回落隐藏窗）。`hello.present` 与 `sample.present` 都读它。
        host.setPresentState(copts.present);
        // 一次性 readout（§12.8 第 3 条：一次性输出可以；⛔ 不是周期性）。
        // ★ `caps=` 从 `runtime/capability.h` 的**同一张表**生成（与 `hello.capabilities` 同源）：
        //   它曾经是手写字面量 ⇒ 长期只有 6 项（漏 move/global-get/global-set），而没有任何守卫看得见。
        //   （具名变量只是为了可读；临时 `std::string` 活到 full-expression 结束，直接传 `c_str()` 也安全。）
        const std::string control_caps = lift::plugin::controlCapabilitiesCsv();
        std::printf(
            "control   : ws://%s  file=%s  idle=%.1fs orphan=%.1fs predicate=%.1fs  caps=%s\n",
            control.localEndpoint().c_str(), control.controlFilePath().c_str(), control_idle_s,
            control_orphan_s, control_predicate_s, control_caps.c_str());
        std::fflush(stdout);
    }

    // ---- ⑥ 执行 ----
    const lift::aot::AotRunResult r = host.run(run_opts);
    // ★ 与已退役解释器臂同一条收口：先摘事件泵，再打摘要（⛔ 不让泵在收口后碰窗口）。
    if (present_window_active) host.cpu().windows->clearHostEventPump();
    // ---- ⑥'' 控制层收口：**摘要之前**（design-ws §2.3 第 6 条：⛔ 不先打摘要再 join，
    //   那会让"端点已消失"这句话晚于摘要出去，人就会读到假象）。
    //   `bye.reason` = run 的停机原因（⛔ 不等于命令名；`requested_by` 另有一格区分 client/idle/orphan）
    //   `bye` 先于摘要 ⇒ 判据 14 的"bye 先于摘要"。
    host.setControl(nullptr);
    if (control_started) {
        // ★ `bye` 两个字段各管一件事（父级裁决）：`reason` = run 为什么结束（窗口类停机用
        //   `window-closed`，⛔ 不等于命令名）；`requested_by` = 谁请求的（窗口关闭 = `client`）。
        const std::string run_reason =
            r.host_stop_reason.empty() ? lift::aot::aotStopName(r.reason) : r.host_stop_reason;
        const char* requested_by = r.host_stop_reason == "window-closed" ? "client" : nullptr;
        control.shutdown(run_reason, requested_by);
    }
    // ---- ⑥' `--input-plan` 收口：**跑结束前必须抬**（否则下一次 run 会看到"按钮一直按着"）----
    //   `releaseAll()` 只抬起待办的按钮，不是 step、不推进入口；返回个数只用于摘要。
    //   ★ 完成度是**判据**（`REQ-01M7HP2Q1N0N004M8R4RPABWSV` 的前置）：`pending != 0`（任务没走完）
    //     或某一步的 `timeout` 到期 ⇒ **exit 5** + stderr 点名 `文件:行` + 门/动作。
    //     ⛔ 不许只打一行 stderr 警告仍 exit 0 —— 那正是"图 → 配方"改写会把 CTest 变成假绿的缝。
    //   ★ 判据先算、摘要照打（读数面不因失败而消失）；退出码在 `printRunSummary` 之后取，
    //     保证"跑失败（2）"与"配方没走完（5）"两个数都能在 stdout 上看到原因。
    const lift::RecipeOutcome plan_outcome =
        input_plan.armed() ? input_plan.outcome() : lift::RecipeOutcome::Done;
    if (input_plan.armed()) {
        input_plan.releaseAll(*host.cpu().windows);
        printf("input-plan: %s\n", input_plan.summary().c_str());
        std::fflush(stdout);
    }
    // ---- ⑥'` `--plugin` 收口：**一次性** readout（⛔ 不周期；§12.8 第 3 条）----
    //   ★ 顺序：先摘读点观察者（run 之后⛔ 不再有读点回调），再释放本 owner 的武装 / 输入状态，
    //     最后打一行。`releaseOwner` 只抬本 owner 的（⛔ 不 `releaseProgram` 全抬）。
    //   ★ 这一行是 C2a 的**可判面**：`run=done|failed|finished|quit` 与 `render=` / `callbacks=`。
    //   ★ C2b-2：adopt 是**非拥有**指针（插件 host 的析构早于窗口对象）⇒ run 收口后立刻从叠层
    //     host 摘掉（⛔ 不让窗口留一个指向已销毁 `PluginScript` 的指针）。之后不再提交帧。
    if (plugin_host != nullptr) {
        host.cpu().windows->setReadPointObserver(nullptr);
        plugin_host->releaseOwner(*host.cpu().windows);
        if (present_window_active) sdl_display->releaseOverlayScript();
        std::printf("%s\n", plugin_host->readout().c_str());
        std::fflush(stdout);
    }
    host.printRunSummary(r, stdout);
    if (r.reason == lift::aot::AotStop::DecodeError) {
        // ★ "EA 不在表里 / 不是 shim" 这一类**必须**在 stderr 上也响亮一次（stdout 的摘要可能被管道吞）。
        std::fprintf(stderr, "emulator-lift-aot: ⛔ 停在一个派发表里没有的 EA：%s\n", r.detail.c_str());
    }
    std::fflush(stdout);
    if (plan_outcome != lift::RecipeOutcome::Done) {
        // 多行报告（点名 `文件:行` + 门/动作 + 进度）由 `InputPlan` 自己算（同一个结论只在一个出口）。
        const std::string report = input_plan.failureReport();
        std::fputs(report.empty() ? "input-plan: FAIL（配方没有走完，但没有可点名的指令）\n" : report.c_str(),
                   stderr);
        std::fflush(stderr);
        return lift::recipeOutcomeExitCode(plan_outcome);  // = 5（recipe.h 的 kRecipeIncompleteExitCode）
    }
    std::fflush(stderr);
    // ★ 退出码口径与解释器臂逐字相同（`machine.cpp` 的 `exitCodeForRun` + `runtime/guest_exit.h`）：
    //   ① run 本身没有正常结束 ⇒ 2（= `exitCodeForRun`）；
    //   ② run 正常停下 ⇒ 再看 guest 自己有没有 `ExitProcess(n)` / `TerminateProcess(_, n)`：
    //      n == 0 ⇒ 0，n != 0 ⇒ 2。⛔ 漏掉 ② 就是 "guest 调 ExitProcess(3) 而宿主仍 exit 0"。
    //   本臂读 `host.cpu().shim.exit_code`（与解释器 `machine.guestExitCode()` 同一个字段）。
    if (!r.ok()) return 2;
    return lift::exitCodeForGuestExit(host.cpu().shim.exit_code);
}
