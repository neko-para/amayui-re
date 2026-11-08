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
//     after N frames|ms|insn  触发条件 = 上一个**同单位**锚点 + N
//     wait N frames|ms|insn   同上，并记一个 **no-op 动作**（脚本要等它到期才收口）
//     ... [stall MS]          （只对 frame 有效）帧停滞兜底：见下
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
//
// ★ 触发条件是**一个条件**，不是"帧号"：`Trigger{kind, value}` 只有四种 kind。
//   将来要加"场景 / 脚本标记"（引擎脚本真的被加载了）只需加一种 kind + 解析词
//   （`wait marker <name>`）—— `tick()` 与 profiler 的窗口边界都不用改形状。
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

#include "runtime/host_window.h"

namespace lift {

// 触发条件的**闭集合**。加一种 kind = 解析器多认一个词 + `due()` 多一个比较；
// `Machine` 的调度与 profiler 的窗口边界都只认"条件到期"，不认具体是哪一种。
enum class TriggerKind : std::uint8_t {
    Frame = 0,   // Present 计数
    GuestMs = 1, // guest 虚拟时钟
    Insn = 2,    // guest 累计指令数
    WallMs = 3,  // 宿主墙钟（自 run 起）
};

const char* triggerKindName(TriggerKind k);
const char* triggerUnitName(TriggerKind k);  // 脚本里写的单位（frame / guest_ms / insn / wall_ms）

struct Trigger {
    TriggerKind kind = TriggerKind::Frame;
    std::uint64_t value = 0;
    // 只对 Frame 有意义：帧停滞多少 ms 之后兜底。0 = 用全局缺省（`--input-stall-ms`）。
    std::uint64_t stall_ms = 0;

    bool due(std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns, std::uint64_t wall_ms) const;
};

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
    void clear();

    bool active() const { return !actions_.empty(); }
    bool finished() const { return active() && applied_ == actions_.size() && releases_.empty(); }
    bool quitRequested() const { return quit_requested_; }
    // `quit` 的 WM_QUIT 只投一次（guest 可能正阻塞在 GetMessageA）。由 `Machine::tickInputScript` 消费。
    bool quitPostPending() const { return quit_requested_ && !quit_posted_; }
    void markQuitPosted() { quit_posted_ = true; }
    // 换脚本 / 丢弃脚本：把仍按着的键 / 按钮按 WM_* 抬起、交还所有权，再清时间线
    //（⛔ 不静默留下"卡住的键"）。`clear()` 只清时间线，不发消息 —— 那是收口用的。
    void abandon(WindowManager& wm, std::uint32_t time_ms);
    std::size_t actionCount() const { return actions_.size(); }
    std::size_t appliedCount() const { return applied_; }
    const std::string& source() const { return source_; }
    const std::vector<Action>& actions() const { return actions_; }

    // ★ profiler 窗口的边界由脚本动作给出：这里只报告"本 tick 有没有要开/关的窗口"。
    //   ⛔ 解析器不认"帧号"，`Machine` 也不猜 —— 边界 = 该动作被触发的那一个 block 边界。
    bool hasProfileActions() const { return profile_actions_ > 0; }
    bool takeProfileBegin(std::string* label);
    bool takeProfileEnd(std::string* label);
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
    // 收口：把脚本仍按着的键 / 按钮按同一组 WM_* 抬起（⛔ 只清状态不发消息会让 guest 的事件锁存卡住）。
    void releaseOwnedInputs(WindowManager& wm, std::uint32_t time_ms);
    // 帧停滞兜底：把剩下的 Frame 条件折算成 WallMs（只做一次）。
    void convertFrameTriggersToWall(std::uint64_t frame, std::uint64_t wall_ms);
    void noteFrameForAverage(std::uint64_t frame, std::uint64_t wall_ms);
    // ★ 缓存"未应用动作"的聚合量（每单位的最小触发值 / 条数 / 帧停滞最小阈值）。
    //   动作集只在 apply / 折算 / 重载时变 ⇒ 每次 tick 只做 4 次整数比较就能判"一个都还没到期"。
    //   ⛔ 触发语义不变：`min <= now` 就是"该单位至少有一条到期"（due() 就是 `now >= value`）。
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
    bool finish_logged_ = false;
    // 解析期的相对基准（`after` / `wait` 用；`at` 会重置）
    std::uint64_t anchor_frame_ = 0;
    std::uint64_t anchor_guest_ms_ = 0;
    std::uint64_t anchor_insn_ = 0;
    std::uint64_t anchor_wall_ms_ = 0;
    bool have_anchor_[4] = {false, false, false, false};
    // 脚本自己的光标（**client** 坐标）：`mouse_down/up` 不带坐标时用它。
    bool have_pos_ = false;
    int last_x_ = 0;
    int last_y_ = 0;
    // profiler 窗口请求（由 `Machine::tickInputScript` 消费）
    int profile_actions_ = 0;
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
    std::uint64_t pend_min_[4] = {0, 0, 0, 0};
    std::uint32_t pend_count_[4] = {0, 0, 0, 0};
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
