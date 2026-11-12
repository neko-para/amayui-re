// emulator-lift · 指令式输入时间线（`--input-script`）
//
// ★ 目的（2026-11-08）：把「在某个时刻点一下 OPTION / 按一下键」从**手工 PowerShell 动作**
//   （SetForegroundWindow → 挪真实光标 → SendInput → 等 250~350 ms）变成**可复现的一次运行**。
//   注入不走 SDL、不读 OS 状态：它只调用 `runtime/input.h` 的那一段应用代码，与真人输入
//   汇进 `WindowManager` 的**同一份**状态（影子键电平 / 鼠标按钮位图 / 虚拟光标）。
//
// ★ 语法（一行一条；`#` 起注释；换行或 `;` 分隔）：
//
//     at frame N              触发条件 = 第 N 个 Present（1 起；-present headless 也计数）
//     at guest_ms T           触发条件 = guest 虚拟时钟 T 毫秒
//     at insn N               触发条件 = guest 累计执行指令数 N（**帧内也能切**；确定性）
//     at wall_ms T            触发条件 = 宿主墙钟 T 毫秒（自 run 起）
//     at marker <名字>        触发条件 = 引擎**进入**某个脚本（脚本身份 (cur, id) 的**边沿**）
//     after N frames|ms|insn  触发条件 = 上一个**同单位**锚点 + N
//     wait N frames|ms|insn   同上，并记一个 **no-op 动作**（脚本要等它到期才收口）
//     ... [stall MS]          （只对 frame 有效）帧停滞兜底：见下
//
//     before-anchor           ★ 段标记（独占一行；**必须**配 `--anchor-script`）：之后的触发 = **绝对**
//                             （按声明的 frame / insn / guest_ms / wall_ms 原样执行：不参与 `+= origin`
//                              重定基，且 0 点闩锁之前**不被抑制**）⇒ 用输入把游戏推到某屏、再锚那一刻
//     after-anchor            ★ 段标记（独占一行）：之后的触发恢复**相对**（参与重定基 / 闩锁前被抑制）。
//                             ★ **必须成对**：缺 `after-anchor` ⇒ 解析期错误（⛔ 不猜"隐式边界"：
//                               "绝对到文件尾"与"绝对止于闩锁"两种都能自圆其说，脚本字面看不出来）
//     ⛔ 解析期错误（都带行号）：没开 `--anchor-script` 却写段标记 / 重复 `before-anchor` /
//        `after-anchor` 在 `before-anchor` 之前 / `before-anchor` 缺配对的 `after-anchor` /
//        绝对段里出现 `profile_begin` / `profile_end`（窗口 profiler 的 0 点语义与锚点冲突：
//        0 点之前的时刻与 0 点之后不是同一把尺子）
//
//     mouse_move X Y          guest 客户区坐标（= 引擎 1280×720 虚拟屏），顺带把光标给脚本
//     mouse_down [left|right|middle]     用脚本上一次的坐标
//     mouse_up   [left|right|middle]
//     click X Y [left|right|middle] [hold N [frames|ms]]   move + down 在触发点；up 见下
//     key_down VK / key_up VK Win32 虚拟键码（十进制或 0x 十六进制，1..255）
//
// ★ **一次按下一定被 guest 观察到**（2026-11-08；这是注入层自己的性质，不是写脚本的人要算的）
//   实测症状：`click` 把 down / up 放在同一 tick（相隔 20k 条指令 ≈ 0.7 ms guest 时间），而该
//   界面的输入采样只有 ~20 Hz（`GetCursorPos` 880 次 / 43.96 s）⇒ **按下被完全漏掉**
//   （`.tmp/exitstall/report.md`：frames 100–108 逐字节相同，只有 hover 重绘）。
//   规则（`Release`）：
//   · `click` 不带 `hold` ⇒ **自动**：up 的最早时刻 = ① guest 的输入采样计数至少 +1
//     （`WindowManager::inputSampleCount`，读点见 `host_window.h`）；② 并且已经过了**一个采样
//     周期**（最近一次采样间隔，夹在 1..200 ms）—— ② 是安全下限，避免"刚好在采样边界上"：
//     引擎可能先取位置、下一句才读按钮电平。
//   · 显式 `hold N`（帧）/ `hold N ms`（guest 时间）⇒ **照用户说的做**，完全不看采样。
//   · 显式 `mouse_up` / `key_up`：若对应的按下**还没被采样过**，把它推迟到同一判据满足时
//     （`deferred`）—— 这样"手写 down/up 对"也不会再被漏。
//   · **上限**：`max_auto_hold_ms_`（`--input-max-hold`，缺省 1000 ms guest 时间；0 = 不设限）。
//     guest 一直不采样（画面静止 / 在别的循环里）⇒ 到上限仍然抬起 + **告警**（⛔ 不静默、⛔ 不挂死）。
//   · ⛔ 一次 `click` 仍然只是"按下 + 抬起"：不重试、不变两次、不改 guest 采样率。
//   · 可观测：每次抬起打 `auto=… waited_samples=… hold_ms=…`，run summary 的 `inputstat`
//     行给出累计（`release` 行：auto / explicit / deferred / bounded-no-sample / 最长 hold）。
//     profile_begin <label>   打开一个**区间 profiler** 窗口（边界 = 本动作的触发条件）
//     profile_end <label>     关闭窗口并打印一次表（label 只用于对齐/报错，必须是同一个词）
//     quit                    本动作触发时结束这次运行（`RunResult.reason = Stopped`）
//     close_window            ★ 宿主动作：像用户点了窗口 X 一样请求关窗（只作测试/自动化；headless no-op）
//
// ★ 触发条件是**一个条件**，不是"帧号"：`Trigger{kind, value, name}` 有多种 kind（帧 / guest_ms /
//   insn / wall_ms / **marker**）。`tick()` 与 profiler 的窗口边界都只认"条件到期"。
//   ★ marker = 引擎脚本身份的**边沿**（`(cur, id)` 变化，取样点 = VM 循环头；见 `engine_marker.h`）：
//   `at marker <名字>` 等一个脚本被**进入**；同名重复出现不重复发，首个样本只播种。
//   ⛔ 它不按数字到期 ⇒ `after marker` / `wait marker` 一律解析失败（没有意义）。
//
// ★ **帧停滞兜底**（为什么必须有）：`at frame` / `wait N frames` 的前提是"帧在推进"。
//   实测（2026-11-08）EU-ROOM 界面稳定后**不再 Present**（帧号停住），纯帧锚点会**永远不触发**。
//   规则：有未到期的 frame 动作、且帧号已 `stall` ms 没变 ⇒ 打一条**告警**，把剩下所有
//   frame 触发条件按"最近帧间隔均值（夹在 1..100 ms）"折算成 `wall_ms` 条件后继续。
//   `stall` 可逐条给（`wait 5 frames stall 8000`），缺省取 `--input-stall-ms`（缺省 10000；
//   0 = 关闭兜底 ⇒ 帧不推进时脚本可能挂死，那是显式选择）。
//   ⛔ 兜底只在"帧真的停住"时生效：帧正常推进时它**一次也不触发**（判据见 CTest）。
//
// ★ 三条口径（都由"注入必须与真人输入同一层"推出）：
//   1. **一条动作只应用一次**。同一 tick 里：`click` 的延迟抬起先处理（它属于上一帧），到期的明动作
//      按**文件顺序**应用。⇒ 同一帧里"上一 click 的 up + 本帧显式 mouse_down"读作 down。
//   2. `at frame` 的"帧"= `HeadlessPresenter::frameCount()`（每次 D3D9 Present 自增）。触发发生在
//      Present 之后的块边界 ⇒ guest 在**下一帧**的取指里看到新状态。**一帧内 down+up 不算点击**
//      （guest 可能一次也采不到"按下"）⇒ `click` 缺省**等 guest 采样**（见文件上方的自动 hold
//      一段），显式 `hold N` 时按用户给的帧 / ms 数。
//   3. `at guest_ms` 读虚拟时钟（缺省 = 每条指令 100 ns）。`--present window` 是 real-time 时钟
//      ⇒ 那一档的毫秒不可复现，**用帧 / 指令数**；headless（缺省）两档都确定性。
//   ★ 收口（动作跑完 / `quit`）：脚本仍按着的键 / 按钮会**按同一组 WM_* 抬起**再交还所有权；未到期
//     的 `click` 抬起也在这一刻补发（否则 guest 的 WndProc / 事件锁存会停在"按下"）。
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "runtime/engine_marker.h"
#include "runtime/host_window.h"

namespace lift {

// 触发条件的**闭集合**。加一种 kind = 解析器多认一个词 + `due()` 多一个比较；
// `Machine` 的调度与 profiler 的窗口边界都只认"条件到期"，不认具体是哪一种。
enum class TriggerKind : std::uint8_t {
    Frame = 0,   // Present 计数
    GuestMs = 1, // guest 虚拟时钟
    Insn = 2,    // guest 累计指令数
    WallMs = 3,  // 宿主墙钟（自 run 起）
    // ★ 引擎脚本身份的**边沿**（`(cur, id)` 变化）：追加在末尾（⛔ 不重排既有值）。
    //   它不是"按数字到期"：`due()` 恒 false，`tick()` 永远不应用它 —— 只有
    //   `noteMarker()`（由 `Machine::run` 在 VM 循环头的块入口采样）会。
    Marker = 4,
};
// 触发条件的个数（`have_anchor_` / `pend_min_` / `pend_count_` 的格数）。
// ⚠ 漏改任何一处按 4 写的循环 ⇒ 第 5 格越界写 / 永不清零（UB 或静默失明）。
inline constexpr int kTriggerKindCount = static_cast<int>(TriggerKind::Marker) + 1;

const char* triggerKindName(TriggerKind k);
const char* triggerUnitName(TriggerKind k);  // 脚本里写的单位（frame / guest_ms / insn / wall_ms）

struct Trigger {
    TriggerKind kind = TriggerKind::Frame;
    std::uint64_t value = 0;
    // 只对 Frame 有意义：帧停滞多少 ms 之后兜底。0 = 用全局缺省（`--input-stall-ms`）。
    std::uint64_t stall_ms = 0;
    // 只对 Marker 有意义：要等的脚本名（单个 token；比对时 ASCII 大小写不敏感）。
    //   ★ 名字表降级时事件名是 `id:0x%08X`；两种形态**始终都接受**（见 `noteMarker`）。
    std::string name;

    bool due(std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns, std::uint64_t wall_ms) const;
};

// ★ **时间线的 0 点**（`--anchor-script <id>`）：**首次观察到进入**引擎脚本 `id` 的那一个可观察样本。
//   ⛔ 与 `InputScript` 里的 `anchor_*`（`after` / `wait` 的**解析期**基准 = "上一条同单位动作"）
//      是两个概念 —— 那个负责相对推进，这个负责"run 的 0 时刻"。名字分开是为了让下一个人改不错。
//   ★ 措辞：一律写"**观察到**进入脚本 X" —— 0 点只定义在可观测样本上（采样点 = VM 循环头，
//      切换指令本身没有观察口；见 `runtime/engine_marker.h`）。
//   ★ `id` 是 guest 真值（记录 `+0x3C` 的原始 32 位），**整数、≥ 0**；`0` 是**合法的根脚本 id**，
//      ⛔ 不许用 `0` 当"关闭"哨兵（"没给 `--anchor-script`"才是关闭）。
struct TimelineOrigin {
    std::uint32_t id = 0;
    // 闩锁那个样本的**四个单位**（0 点作用于全部四个：frame / insn / guest_ms / wall_ms）。
    std::uint64_t frame = 0;
    std::uint64_t guest_ms = 0;
    std::uint64_t insns = 0;
    std::uint64_t wall_ms = 0;
};

// ★ `--anchor-script` 的**启动期**校验（解析期报错，⛔ 不跑一半就退化成"0 点从 run 起算"）。
//   两条判据分开是因为它们的信息不同：① 只依赖 CLI 组合（argv 解析完就能判），
//   ② 依赖脚本已解析（"一个触发 / 动作都没有"的脚本让 0 点无从谈起）。
//   抽成**纯函数**是为了让 pure CTest 能断言"与 `--interaction-graph` 同给 ⇒ 解析期报错" ——
//   `main` 的进程退出码在 CTest 里不可达，这里是同一段判据的**唯一实现**（⛔ 不复制一份到 test 里）。
//   返回空串 = 通过；非空 = 用法错误的原因（调用方 `exit 2`）。
std::string validateOriginCli(bool anchor_requested, bool input_script_given, bool interaction_graph_given);
std::string validateOriginScript(bool anchor_requested, std::size_t action_count);

class InputScript {
public:
    enum class Kind : std::uint8_t {
        MouseMove,
        MouseDown,
        MouseUp,
        KeyDown,
        KeyUp,
        Click,
        Wait,  // no-op 动作：让脚本**等到**这个时刻才收口（尾部 `wait` 可以保持按住）
        ProfileBegin,
        ProfileEnd,
        Quit,
        // ★ 宿主动作（`close_window`）：像用户点了窗口 X 一样请求关窗（`PresentSink::requestClose`）。
        //   用途 = 关窗二次确认（AGERC `_ShowDialog@12`）的**可复现触发**；headless 无 sink ⇒ no-op。
        CloseWindow,
    };

    struct Action {
        Kind kind = Kind::Quit;
        Trigger trigger;
        int x = 0;
        int y = 0;
        MouseButton button = MouseButton::Left;
        std::uint32_t vk = 0;
        // ★ Click 的 hold：显式给了就按它（帧 / guest_ms）；没给 ⇒ 自动（等 guest 采样 + 一个周期）。
        bool hold_explicit = false;
        TriggerKind hold_unit = TriggerKind::Frame;
        std::uint64_t hold_value = 0;
        std::string label;              // ProfileBegin/End
        std::string text;               // 原始行（日志 / 报错定位）
        bool applied = false;
        // ★ 段标记 `before-anchor` 之后的动作 = **绝对**段（`REQ-01M710E6PV4PPHECPPCCTRF5RA`）：
        //   触发值**按声明原样**执行（⛔ 不参与 `+= origin` 重定基），且闩锁前**不被抑制**
        //   （抑制只压相对段 —— 它的存在理由正是"让输入在 0 点之前能跑"）。
        //   ⛔ 不是"anchor 可选"：没进入目标脚本仍然响亮失败（`origin-missed:`）。
        bool absolute = false;
    };

    // "抬起"是**延迟动作**，单独排队；否则一次 tick 里 down+up 会被 guest 漏掉。
    // 两种触发方式（`auto_hold`）：
    //   · true  = 等 guest 采样：`inputSampleCount() > press_samples` 且已过一个采样周期；
    //             到 `max_auto_hold_ms_` 仍未采样 ⇒ 强制抬起 + 告警（`bounded`）。
    //   · false = 显式 hold（用户说了算）：到 `due_value` 帧 / guest_ms 就抬，**不看采样**。
    struct Release {
        bool is_key = false;                    // 键盘（key_up）还是鼠标按钮
        std::uint32_t vk = 0;                   // is_key
        MouseButton button = MouseButton::Left; // !is_key
        int x = 0;
        int y = 0;
        bool auto_hold = true;
        TriggerKind unit = TriggerKind::Frame;  // 显式 hold 的单位（Frame / GuestMs）
        std::uint64_t due_value = 0;            // 显式 hold 的目标（帧号 / guest_ms）
        std::uint64_t press_samples = 0;        // 自动 hold：按下时的采样计数
        std::uint64_t press_ms = 0;             // 自动 hold：按下时的 guest_ms（上限从它算）
        std::uint64_t press_frame = 0;          // 按下时的帧号（可观测 / 守卫用）
        // 自动 hold：**第一次观察到"采样计数 +1"的那一刻**（guest_ms）。安全下限从它算 ——
        // 只等"计数 +1"会卡在采样边界的竞态上（引擎可能刚取完位置、下一句才读按钮电平）。
        bool observed = false;
        std::uint64_t observe_ms = 0;
        std::string text;                       // 原动作行（日志）
    };

    // ★ 可观测账（守卫与 run summary 读它；⛔ 不是墙钟）：
    //   `releases` 总抬起次数 · `auto`/`explicit` 两种触发方式各多少 · `deferred_ups` 显式 up
    //   因为"还没被采样"被推迟的次数 · `bounded_no_sample` 到上限仍没采样 ⇒ 强制抬起（且告警）
    //   · `waited_samples_max` 单次最多等了几次采样 · `hold_ms_max` 最长"按下→抬起"跨度（guest ms）。
    struct ReleaseStats {
        std::uint64_t releases = 0;
        std::uint64_t auto_releases = 0;
        std::uint64_t explicit_releases = 0;
        std::uint64_t deferred_ups = 0;
        std::uint64_t bounded_no_sample = 0;
        std::uint64_t waited_samples_max = 0;
        std::uint64_t hold_ms_max = 0;
    };
    // 最近一次抬起的逐项账（守卫断言"down 与 up 之间跨过了采样点"用）。
    struct ReleaseInfo {
        bool valid = false;
        bool is_key = false;
        std::uint32_t vk = 0;
        MouseButton button = MouseButton::Left;
        bool auto_hold = true;
        bool bounded = false;  // 1 = 这次是"到上限仍没采样"的强制抬起
        std::uint64_t press_frame = 0;
        std::uint64_t release_frame = 0;
        std::uint64_t press_ms = 0;
        std::uint64_t release_ms = 0;
        std::uint64_t press_samples = 0;
        std::uint64_t release_samples = 0;
        std::uint64_t observe_ms = 0;  // 自动 hold：第一次看到"采样 +1"的时刻（0 = 没观察到 / 显式 hold）
    };

    // 读脚本文件（UTF-8；Windows 走 `openHostFileUtf8`）并解析。失败 ⇒ false + err（带 行号）。
    bool loadFile(const std::string& path, std::string* err);
    // 直接解析文本（测试 / 内嵌用）；origin 只进报错与日志。
    bool loadText(const std::string& text, const std::string& origin, std::string* err);
    // ★ 追加：把 `text` 解析成动作接到**末尾**（⛔ 不 clear、⛔ 不动已应用的账）。
    //   为什么需要它：交互状态机（`runtime/interaction_graph.h`）在**运行中**才知道"下一个动作在哪个
    //   时刻"（块边界上的 frame / guest_ms），而动作的注入与"一次按下一定被 guest 观察到"的语义
    //   已经全在 `InputScript` 里 —— 状态机只追加，不另写一套按下 / 抬起。
    //   前置（全部可由本类自己的账判定；违反 ⇒ false + err 带原因）：
    //     ① 现有动作**全部已应用**（`applied_ == actions_.size()`）—— 否则新动作会排在未应用动作
    //        之间，"应用顺序 == 文件顺序"这条不再成立；
    //     ② **没有未完成的抬起**（`releases_` 为空）—— 否则新按下会被上一次的 up 立刻清掉；
    //     ③ 新动作的触发值**不早于**现有最后一条**同单位**动作（`now >= value` 的到期语义 ⇒
    //        倒序会让后面的动作先到期，与文件顺序不一致）。
    //   ⚠ 追加段里的 `after` / `wait` 用的是**追加段自己**的锚点（不是整条脚本的）：要接在前面
    //     已有时刻之后，请写绝对 `at <单位> <值>`（状态机就是这么写的）。
    //   返回值语义与 `loadText` 一致（同一个解析器 / 同一份报错格式）。
    bool appendText(const std::string& text, const std::string& origin, std::string* err);
    void clear();

    bool active() const { return !actions_.empty(); }
    bool finished() const { return active() && applied_ == actions_.size() && releases_.empty(); }
    bool quitRequested() const { return quit_requested_; }
    // ★ 还有**没交付完**的抬起（`click` 的自动 hold 等 guest 采样 / 显式 down 的延迟 up）。
    //   "动作后"对一次点击要算到抬起为止：观测窗若在这之前到期，会把一次**合法**点击判成
    //   "画面没变"（自动 hold 会花掉若干个采样）。`--expect-change-*` 的判据读它。
    std::size_t pendingReleaseCount() const { return releases_.size(); }
    // `quit` 的 WM_QUIT 只投一次（guest 可能正阻塞在 GetMessageA）。由 `Machine::tickInputScript` 消费。
    bool quitPostPending() const { return quit_requested_ && !quit_posted_; }
    void markQuitPosted() { quit_posted_ = true; }
    // ★ `close_window` 动作的**交付位**：动作应用时只置位（`apply()` 不碰窗口），由 `Machine` 在
    //   同一个块边界把它转成 `PresentSink::requestClose()`（与 quit 的 WM_QUIT 同一条形状）。
    //   `take` = 读并清（一次动作只交付一次）。
    bool takeCloseWindowPending() {
        const bool pending = close_window_pending_;
        close_window_pending_ = false;
        return pending;
    }
    // 换脚本 / 丢弃脚本：把仍按着的键 / 按钮按 WM_* 抬起、交还所有权，再清时间线
    //（⛔ 不静默留下"卡住的键"）。`clear()` 只清时间线，不发消息 —— 那是收口用的。
    void abandon(WindowManager& wm, std::uint32_t time_ms);
    std::size_t actionCount() const { return actions_.size(); }
    std::size_t appliedCount() const { return applied_; }
    // ★ **否定证据**（`REQ-01M72QSNQCH3GHH3158A7YX96V` / BUG-20）：run 结束时"哪些动作没跑"。
    //   为什么需要：`finished` 那条日志在 `finished()` **或 `quit`** 时打（`quit` 路径报的是**总**动作数
    //   ⇒ ⛔ 别当"应用数"读），所以"没打那行"同时意味着"脚本没跑完 / 条件写错 / run 提前停了"
    //   ⇒ 三者当前不可区分。这三个访问器把那份否定证据变成**正面**读数（几条没跑 + 第一条是什么）。
    //   口径：动作**按到期 / 边沿**应用（`tick()` 逐条判到期、`noteMarker()` 按边沿）⇒ 应用顺序
    //   **不一定**是文件顺序，`applied_` 只是计数 ⇒ "第一条未应用"只能扫 `applied` 标志。
    //   `first…Text()` = **原动作行**（`Action::text`，⛔ 不是触发条件行）；没有对应项 ⇒ 空串。
    //   ⛔ 不暴露可写引用（只给 `const std::string&`；越界一律空串）。
    std::size_t unfinishedActionCount() const;
    const std::string& firstUnfinishedActionText() const;
    const std::string& firstPendingReleaseText() const;
    const std::string& source() const { return source_; }
    const std::vector<Action>& actions() const { return actions_; }

    // ★ profiler 窗口的边界由脚本动作给出：这里只报告"本 tick 有没有要开/关的窗口"。
    //   ⛔ 解析器不认"帧号"，`Machine` 也不猜 —— 边界 = 该动作被触发的那一个 block 边界。
    bool hasProfileActions() const { return profile_actions_ > 0; }
    bool takeProfileBegin(std::string* label);
    bool takeProfileEnd(std::string* label);

    // ---- ★ marker 事件源（`TriggerKind::Marker`；取样见 `runtime/engine_marker.h`）----
    // 脚本里有没有 marker 动作：`Machine::run` 的块入口钩子只在它为真时才采样（**零成本**）。
    bool hasMarkerActions() const { return marker_actions_ > 0; }
    // 喂一次采样（由 `Machine::sampleMarker()` 在 VM 循环头的块入口调用；纯档夹具也直接调它）。
    //   返回 true = 这次样本产生了**一个边沿**（有新事件），不代表有动作被应用。
    //   · 首个有效样本只**播种**（记下 (cur,id)，不发事件）；
    //   · `(cur,id)` 与上次相同 ⇒ 0 事件（⛔ 不重复发）；
    //   · 变化 ⇒ 扫未应用的 Marker 动作，名字 / `id:0x%08X` 任一形态匹配即走**同一条** apply 路径。
    //   ★ 两个可选参数（`insns` / `wall_ms`）只给 0 点重定基用：`Machine::sampleMarker()` 两个调用点
    //     都传四个单位；只给 frame / guest_ms 的老夹具（默认 0）行为不变。
    bool noteMarker(WindowManager& wm, const MarkerSample& s, std::uint64_t frame, std::uint64_t guest_ms,
                    std::uint64_t insns = 0, std::uint64_t wall_ms = 0);
    // 超时（缺省 30000 **宿主墙钟** ms；0 = 不设限 ⇒ 显式选择"宁可挂死"）。
    //   ★ 用墙钟而不是 guest_ms：guest 阻塞在 `GetMessageA` 时虚拟时钟不推进，guest_ms 超时等于永不触发。
    void setMarkerTimeoutMs(std::uint64_t ms) { marker_timeout_ms_ = ms; }
    std::uint64_t markerTimeoutMs() const { return marker_timeout_ms_; }
    bool markerTimedOut() const { return marker_timeout_fired_; }
    const std::string& markerTimeoutDetail() const { return marker_timeout_detail_; }
    // ★ 超时还要**唤醒**可能阻塞在 `GetMessageA` 的 guest（与 `quit` 同一条理由：只置标志时
    //   `Machine::run` 的检查点永远到不了）⇒ `pumpScript` 消费这条一次性请求投 WM_QUIT。
    bool markerTimeoutPostPending() const { return marker_timeout_fired_ && !marker_timeout_posted_; }
    void markMarkerTimeoutPosted() { marker_timeout_posted_ = true; }
    // 可观测账（run summary / CTest）：有效样本 / 边沿 / 无效采样 / 最近一次身份。
    std::uint64_t markerSamples() const { return marker_samples_; }
    std::uint64_t markerEdges() const { return marker_edges_; }
    std::uint64_t markerInvalidSamples() const { return marker_invalid_; }
    std::uint32_t lastMarkerCur() const { return marker_last_cur_; }
    std::uint32_t lastMarkerId() const { return marker_last_id_; }
    const std::string& lastMarkerName() const { return marker_last_name_; }
    bool lastMarkerNameDegraded() const { return marker_last_name_degraded_; }
    // 每轮 run 重置 seed / 边沿 / 计数 / 超时（同一台 Machine 可以跑多轮，⛔ 不许把上轮 (cur,id) 当基线）。
    //   ★ 顺带重置 0 点的**状态**（`resetOriginState()`）；0 点的**配置**（目标 id）不在这里清
    //     —— 与 `marker_timeout_ms_` 同一口径。
    void resetMarkerState();

    // ---- ★ 时间线的 0 点（`--anchor-script <id>`；语义见文件上方的 `TimelineOrigin`）----
    // 配置：**跨 `clear()` / 跨 run 存活**（照 `marker_timeout_ms_` 的先例）。`0` 是合法 id。
    void setOriginScript(std::uint32_t id) {
        origin_requested_ = true;
        origin_id_ = id;
    }
    bool originRequested() const { return origin_requested_; }
    std::uint32_t originScriptId() const { return origin_id_; }
    bool originSet() const { return origin_set_; }
    const TimelineOrigin& origin() const { return origin_; }
    // 闩锁次数（正常 = 0 或 1；> 1 = "重进又定基"那个 bug）与观察到的**重进**次数（只记 info，⛔ 不定基）。
    std::uint64_t originLatches() const { return origin_latches_; }
    std::uint64_t originReentries() const { return origin_reentries_; }
    // ★ 采样门控（`Machine::run` 的块入口钩子只问它）：marker 动作**或** 0 点配置 ⇒ 必须采样。
    //   ⛔ 退回 `hasMarkerActions()` 会让"只有 anchor、没有 marker 动作"的脚本**一次也不采样**
    //      ⇒ 0 点永不闩锁 ⇒ 收尾假报 `origin-missed:`（这是本单最容易漏的一条）。
    bool needsIdentitySamples() const { return marker_actions_ > 0 || origin_requested_; }
    // ★ 喂一个**有效**样本 ⇒ 先判 0 点闩锁（电平 + 首次），再走 marker 的播种 / 边沿逻辑。
    //   由 `noteMarker()` 在入口（`s.valid` 之后、**播种之前**）调用 —— ⛔ 不要另开一条采样路径。
    //   返回 true = **本次**刚闩锁。四个单位都要给（0 点作用于全部四个）。
    bool noteIdentity(const MarkerSample& s, std::uint64_t frame, std::uint64_t guest_ms,
                      std::uint64_t insns, std::uint64_t wall_ms);
    // 收尾的**响亮失败** detail（前缀 `origin-missed:`；⛔ 不新增 `StopReason`）。
    //   措辞必须写"没**观察到**进入脚本 X"：0 点定义在首个可观察样本上，宿主可能漏掉"进又出"。
    std::string originMissedDetail(std::uint64_t frame, std::uint64_t wall_ms) const;
    // 每轮 run 重置 0 点的**状态**（配置保留）。
    void resetOriginState();
    // 帧停滞兜底是否已经生效（测试 / 诊断用）
    bool stallFallbackFired() const { return stall_fired_; }
    // 缺省帧停滞阈值（ms；0 = 关闭兜底）。由 main 从 `--input-stall-ms` 设。
    void setDefaultStallMs(std::uint64_t ms) {
        default_stall_ms_ = ms;
        pending_dirty_ = true;  // 停滞阈值参与聚合缓存 ⇒ 换了就重算
    }
    std::uint64_t defaultStallMs() const { return default_stall_ms_; }
    // 还没出现**第一帧**时的宽限（ms；0 = 立刻按停滞处理）。boot 期间没有 Present 不等于"卡住"。
    void setDefaultBootGraceMs(std::uint64_t ms) { default_boot_grace_ms_ = ms; }
    std::uint64_t defaultBootGraceMs() const { return default_boot_grace_ms_; }

    // 每个块边界调用（`Machine::run`，以及 guest 阻塞在 GetMessageA 时的宿主事件泵）。
    //   `frame` = Present 计数；`guest_ms` = 虚拟时钟；`insns` = 累计指令数；`wall_ms` = 宿主墙钟。
    // 返回 true = 本次 tick 应用了动作。
    bool tick(WindowManager& wm, std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns,
              std::uint64_t wall_ms);

    // ★ 可观测计数（A/B 与守卫用；不是墙钟）：
    //   `actionScans()` = tick 里**逐条动作**比较触发条件的次数。快路径（没有任何条件到期）应当
    //   一次也不扫；它只随"动作集变化"（apply / 折算）增长，**不随块数增长**。
    std::uint64_t tickCount() const { return tick_count_; }
    std::uint64_t actionScans() const { return action_scans_; }
    // ★ 自动 hold 的**上限**（guest ms；0 = 不设限 ⇒ 显式选择"宁可挂死"）。由 main 从
    //   `--input-max-hold` 设（缺省 1000）。到上限仍没被采样 ⇒ 强制抬起 + 告警。
    void setMaxAutoHoldMs(std::uint64_t ms) { max_auto_hold_ms_ = ms; }
    std::uint64_t maxAutoHoldMs() const { return max_auto_hold_ms_; }
    // ★ 抬起账（守卫 / run summary 用；语义见上面 `ReleaseStats` / `ReleaseInfo`）。
    const ReleaseStats& releaseStats() const { return stats_; }
    const ReleaseInfo& lastRelease() const { return last_release_; }

    static std::uint64_t msFrom100ns(std::uint64_t now_100ns) { return now_100ns / 10000ull; }

private:
    void apply(WindowManager& wm, Action& a, std::uint64_t frame, std::uint64_t guest_ms);
    // ★ 应用一条动作的**公共记账**（applied / applied_ / 缓存作废 / apply）。`tick()` 与 `noteMarker()`
    //   必须走同一条：⛔ 裸调 `apply()` 会让动作永远 pending（重复发 / 超时误报 / finished() 永假）。
    void applyAction(WindowManager& wm, Action& a, std::uint64_t frame, std::uint64_t guest_ms);
    // marker 超时：每 tick 算一次"文件序第一条未应用 Marker 动作"的等待基线（基线一变就重置）。
    void updateMarkerTimeout(std::uint64_t wall_ms);
    // ★ 0 点的一次性重定基：把**未应用的数值触发**整体 `+= origin`（只做一次；照
    //   `convertFrameTriggersToWall()` 的先例：就地改触发值 + 作废聚合缓存）。
    //   ⛔ 跳过 Marker（名字条件没有数值可平移）与已应用的（⛔ 不撤已发生的事）。
    void rebasePendingTriggers();
    // 收口：把脚本仍按着的键 / 按钮按同一组 WM_* 抬起（⛔ 只清状态不发消息会让 guest 的事件锁存卡住）。
    void releaseOwnedInputs(WindowManager& wm, std::uint32_t time_ms);
    // 帧停滞兜底：把剩下的 Frame 条件折算成 WallMs（只做一次）。
    void convertFrameTriggersToWall(std::uint64_t frame, std::uint64_t wall_ms);
    void noteFrameForAverage(std::uint64_t frame, std::uint64_t wall_ms);
    // ★ 缓存"未应用动作"的聚合量（每单位的最小触发值 / 条数 / 帧停滞最小阈值）。
    //   动作集只在 apply / 折算 / 重载时变 ⇒ 每次 tick 只做 4 次整数比较就能判"一个都还没到期"。
    //   ⛔ 触发语义不变：`min <= now` 就是"该单位至少有一条到期"（due() 就是 `now >= value`）。
    //   ★ **相对段 / 绝对段分开缓存**（`before-anchor`）：闩锁 pending 期间只有相对段被抑制，
    //     绝对段照常到期 ⇒ 两类的 `min` 必须分开算（⛔ 混一个 min 会让绝对段被相对段的最小值掩盖）。
    //     条数仍按"全部（含绝对）"累计：`pend_count_` 是帧停滞兜底 / marker 超时的存在性判据。
    void rebuildPending();
    void markPendingDirty() { pending_dirty_ = true; }

    // ---- 按下 / 抬起（"一次按下一定被 guest 观察到"；见文件上方那一段）----
    // 每个键 / 按钮一个按下记录（key = VK；鼠标按钮用 `0x100 + 序号`）。它回答"按下之后 guest
    // 采样过吗"，⇒ 显式 `mouse_up` / `key_up` 该不该推迟。
    struct PressInfo {
        std::uint64_t samples = 0;
        std::uint64_t guest_ms = 0;
    };
    static std::uint32_t mousePressId(MouseButton b) { return 0x100u + static_cast<std::uint32_t>(b); }
    void notePress(std::uint32_t id, const WindowManager& wm, std::uint64_t guest_ms);
    const PressInfo* findPress(std::uint32_t id) const;
    void dropPress(std::uint32_t id);
    bool releaseQueued(std::uint32_t id) const;
    // 自动 hold 的**安全下限** = 最近一个采样周期（夹在 1..200 ms；样本不足 ⇒ 1 ms）。
    std::uint64_t autoHoldFloorMs(const WindowManager& wm) const;
    // 这条 pending 抬起现在该落地吗？`bounded` 置 1 = "到上限仍没采样"的强制抬起。
    // ⚠ 会写 `r.observed` / `r.observe_ms`（第一次看到"采样 +1"的那一刻）。
    bool releaseDue(Release& r, const WindowManager& wm, std::uint64_t frame, std::uint64_t guest_ms,
                    bool* bounded);
    // 真正抬起 + 记账 + 打日志（`bounded` ⇒ stderr 告警）。
    void emitRelease(WindowManager& wm, const Release& r, std::uint64_t frame, std::uint64_t guest_ms,
                     std::uint32_t time_ms, bool bounded);
    void pushRelease(const Release& r);

    std::string source_;
    std::vector<Action> actions_;
    std::vector<Release> releases_;
    std::map<std::uint32_t, PressInfo> presses_;
    std::size_t applied_ = 0;
    bool quit_requested_ = false;
    bool quit_posted_ = false;
    bool close_window_pending_ = false;
    bool finish_logged_ = false;
    // 解析期的相对基准（`after` / `wait` 用；`at` 会重置）
    std::uint64_t anchor_frame_ = 0;
    std::uint64_t anchor_guest_ms_ = 0;
    std::uint64_t anchor_insn_ = 0;
    std::uint64_t anchor_wall_ms_ = 0;
    bool have_anchor_[kTriggerKindCount] = {};
    // 脚本自己的光标（**client** 坐标）：`mouse_down/up` 不带坐标时用它。
    bool have_pos_ = false;
    int last_x_ = 0;
    int last_y_ = 0;
    // profiler 窗口请求（由 `Machine::tickInputScript` 消费）
    int profile_actions_ = 0;
    // ★ marker：动作数（零成本门控）+ 边沿状态 + 超时账。
    int marker_actions_ = 0;
    bool marker_seeded_ = false;
    std::uint32_t marker_cur_ = 0;
    std::uint32_t marker_id_ = 0;
    std::uint64_t marker_samples_ = 0;   // 有效采样（含播种那一次）
    std::uint64_t marker_edges_ = 0;     // 边沿次数（= 事件数）
    std::uint64_t marker_invalid_ = 0;   // 采样器给出 valid=false 的次数（boot / cur 越界 / 记录不可用）
    std::uint32_t marker_last_cur_ = 0;
    std::uint32_t marker_last_id_ = 0;
    std::string marker_last_name_;
    bool marker_last_name_degraded_ = false;
    std::uint64_t marker_timeout_ms_ = 30000;  // 宿主墙钟 ms；0 = 不设限
    bool marker_timeout_fired_ = false;
    bool marker_timeout_posted_ = false;  // WM_QUIT 只投一次（唤醒 GetMessageA）
    std::string marker_timeout_detail_;
    bool marker_waiting_ = false;
    std::size_t marker_wait_action_ = static_cast<std::size_t>(-1);  // 基线对应的"第一条未应用 Marker 动作"下标
    std::uint64_t marker_wait_ms_ = 0;  // 等待基线（墙钟；第一次看见这条 pending 时记）
    std::string marker_wait_name_;
    // ★ 0 点（`--anchor-script`）：`origin_requested_` / `origin_id_` 是**配置**（跨 clear / 跨 run），
    //   其余是**状态**（每轮 run 重置）。⛔ 两类别混在同一个重置点里（会有一边错）。
    bool origin_requested_ = false;
    std::uint32_t origin_id_ = 0;
    bool origin_set_ = false;
    TimelineOrigin origin_;
    std::uint64_t origin_latches_ = 0;
    std::uint64_t origin_reentries_ = 0;
    // 重进日志去重：上一次样本的 id 已经是目标 ⇒ 同一段停留只记一行 info（⛔ 不每样本刷屏）。
    bool origin_reentry_seen_ = false;
    bool profile_begin_pending_ = false;
    bool profile_end_pending_ = false;
    std::string profile_begin_label_;
    std::string profile_end_label_;
    // 帧停滞兜底
    std::uint64_t default_stall_ms_ = 10000;
    std::uint64_t default_boot_grace_ms_ = 30000;
    bool stall_fired_ = false;
    bool saw_frame_ever_ = false;
    bool have_frame_sample_ = false;
    std::uint64_t last_frame_seen_ = 0;
    std::uint64_t last_frame_change_wall_ms_ = 0;
    std::uint64_t frame_interval_sum_ms_ = 0;
    std::uint32_t frame_interval_count_ = 0;
    // ★ 未应用动作的聚合缓存（见 rebuildPending）。`pending_dirty_` 在动作集变化时置位。
    //   `pend_min_` / 条数 = **相对段**（参与重定基 / 闩锁前被抑制）；`pend_min_abs_` / `pend_count_abs_`
    //   = **绝对段**（`before-anchor` 之后；不重定基、闩锁前照常到期）。
    std::uint64_t pend_min_[kTriggerKindCount] = {};
    std::uint32_t pend_count_[kTriggerKindCount] = {};
    std::uint64_t pend_min_abs_[kTriggerKindCount] = {};
    std::uint32_t pend_count_abs_[kTriggerKindCount] = {};
    std::uint64_t pend_frame_limit_ = 0;  // pending Frame 动作里最小的非 0 停滞阈值；0 = 没有
    bool pending_dirty_ = true;
    // 可观测计数（tick 次数 / 逐条动作比较次数）
    std::uint64_t tick_count_ = 0;
    std::uint64_t action_scans_ = 0;
    // 自动 hold 的上限（guest ms；0 = 不设限）与抬起账
    std::uint64_t max_auto_hold_ms_ = 1000;
    ReleaseStats stats_;
    ReleaseInfo last_release_;
};

}  // namespace lift
