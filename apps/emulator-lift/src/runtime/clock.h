// emulator-lift · 时钟（**只有一个档：墙钟**）
//
// ★ 口径：guest 可见的时间 = **宿主墙钟**。`timeGetTime` / `GetTickCount` / `QueryPerformanceCounter`
//   全部读同一个 `steady_clock` 单调读数（见 `now100ns()`），⚠ **不承诺逐帧 / 逐字节确定性**：
//   同一条命令跑两次的 `insns` / trace 会因宿主调度而不同（2026-11-27 用户裁决：虚拟时钟移除）。
//   ⛔ 不要加回"虚拟档 / `--real-time` 开关 / 每条指令 +1 tick"那套：它与音频 feeder 线程、
//      输入时间线、墙钟超时互相打架（原 `VirtualClock` 的双态已整档删除）。
// ★ 全部 inline：时钟是叶子设施，为一个 getter 建一个 .cpp 不值得。
#pragma once

#include <chrono>
#include <cstdint>

namespace lift {

class Clock {
public:
    Clock() { captureWallBase(); }

    // =================================================================================
    // ★ 控制层暂停 = **冻结区间对 guest 不可见**（`design.md` §3.3）：
    //   冻结期间 guest 读到的 `now100ns()` / `nowMs()` 不前进；解冻后把整段暂停从 raw 里减掉。
    //   ⇒ guest 看到的时间序列与"没暂停"**逐位相同**（暂停前/后的读数也逐位相同）。
    // ★ 默认档（从不 freeze）：`paused_100ns_ == 0 && !frozen_` ⇒ `now100ns()` 就是 raw 读数。
    // =================================================================================
    // 进入冻结。
    void freeze() {
        if (frozen_) return;
        frozen_ = true;
        freeze_start_100ns_ = rawNow100ns();
    }
    // 离开冻结：把这次冻结区间累加进 `paused_100ns_`（单调时钟 ⇒ raw >= start）。
    void unfreeze() {
        if (!frozen_) return;
        const std::uint64_t raw = rawNow100ns();
        if (raw > freeze_start_100ns_) paused_100ns_ += raw - freeze_start_100ns_;
        frozen_ = false;
        freeze_start_100ns_ = 0;
    }
    bool frozen() const { return frozen_; }
    // ★ **已结算**暂停毫秒（= `paused_100ns_`，**不含**当前冻结区间）。
    //   控制层 `sample` 的快照用这个：同一暂停点连采两次必须**逐位相同**（判据 3b），
    //   而含当前区间的 `pausedTotalMs()` 在暂停中会一直涨。pause/resume 的 `res` 仍用后者（S1 口径）。
    std::uint64_t pausedCommittedMs() const { return paused_100ns_ / 10000ull; }
    // 累计暂停毫秒（**含当前冻结区间** ⇒ 暂停中读数也在涨）。缺省 0。
    std::uint64_t pausedTotalMs() const {
        std::uint64_t total = paused_100ns_;
        if (frozen_) {
            const std::uint64_t raw = rawNow100ns();
            if (raw > freeze_start_100ns_) total += raw - freeze_start_100ns_;
        }
        return total / 10000ull;
    }
    // 100 ns 单位（与 Win32 的 FILETIME 同源）
    std::uint64_t now100ns() const {
        const std::uint64_t raw = rawNow100ns();
        if (!frozen_) return raw - paused_100ns_;
        // 冻结中：时间停在 freeze_start_ - paused_（再取 raw 也不会前进）。
        return freeze_start_100ns_ - paused_100ns_;
    }
    // 毫秒（timeGetTime / GetTickCount）
    std::uint32_t nowMs() const { return static_cast<std::uint32_t>(now100ns() / 10000ull); }
    // QPC：仿真频率固定 3,579,545 Hz（Win32 常见值；不是宿主 TSC）。
    std::int64_t qpc() const {
        const std::uint64_t t = now100ns();
        return static_cast<std::int64_t>((t * 3579545ull) / 10000000ull);
    }
    std::int64_t qpcFrequency() const { return 3579545; }

    std::uint64_t epoch100ns() const { return epoch_100ns_; }  // FILETIME 起点（1601）之后的基础偏移
    void setEpoch100ns(std::uint64_t v) { epoch_100ns_ = v; }

    // =================================================================================
    // ★ 墙钟 vs uptime（BUG-30）：`now100ns()` / `nowMs()` / `qpc()` 是**单调 uptime**
    //   （零点 = 宿主启动），⛔ 不当 epoch 展开。
    //   墙钟是**另一条读数**，单位与 `now100ns()` 相同（100 ns），起点 **1970-01-01 00:00:00Z**：
    //
    //       wall100ns() = wall_base_100ns_ + now100ns()
    //
    //   · `wall_base_100ns_` 在构造 / 换时钟来源那一刻捕获为
    //     `system_clock::now() - rawNow100ns()`（= "单调时钟零点那一刻的墙钟"）
    //     ⇒ 之后只随**单调增量**走：宿主调时（NTP 回拨 / 手动改钟）既不会让墙钟倒退、也不会让它跳。
    //   ★ 冻结语义与 `now100ns()` **同源**：暂停区间对墙钟同样不可见（存档时间戳不因暂停漂）。
    //   ★ `system_clock` 的起点按 **Unix epoch（1970）** 处理（MSVC / libstdc++ / libc++ 实测如此；
    //     这是本读数的口径，不是标准保证）。
    // =================================================================================
    std::uint64_t wall100ns() const { return wall_base_100ns_ + now100ns(); }

    // ★ 测试缝：注入墙钟 base（= 单调零点那一刻的墙钟，1970 起算的 100 ns）。
    //   注入后**不再**由 `system_clock` 捕获 ⇒ 用例不必依赖"当前年份"。
    //   ⛔ 只给 CTest 用；生产路径从不调用。
    void setWallBaseForTest(std::uint64_t base_100ns) {
        wall_base_100ns_ = base_100ns;
        wall_base_pinned_ = true;
    }

    // ★ 测试缝：注入"原始墙钟读数"（100ns）。nullptr = `steady_clock`（生产路径）。
    //   ⛔ 只给 CTest 用：用例靠它把时间做成可控读数（不必真睡），不改任何生产判据。
    using RawNowFn = std::uint64_t (*)();
    void setRawNowForTest(RawNowFn fn) {
        raw_now_fn_ = fn;
        paused_100ns_ = 0;
        frozen_ = false;
        freeze_start_100ns_ = 0;
        captureWallBase();  // 换了单调读数来源 ⇒ 未注入的 base 要按新来源重算
    }

private:
    // 原始墙钟（100 ns）：未注入 ⇒ `steady_clock`。
    std::uint64_t rawNow100ns() const {
        if (raw_now_fn_ != nullptr) return raw_now_fn_();
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100);
    }

    // `system_clock` 读数（100 ns，1970 起算）。只用于**捕获** wall base，不进任何逐次读数。
    static std::int64_t systemNow100ns() {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100);
    }

    // 捕获墙钟 base = "单调零点那一刻的墙钟"（1970 起算）。
    // 测试已注入 ⇒ 保留注入值；⛔ 不在每次读数里取 `system_clock`（那会让墙钟随宿主调时跳）。
    void captureWallBase() {
        if (wall_base_pinned_) return;
        wall_base_100ns_ = 0;
        const std::int64_t sys = systemNow100ns();
        const std::int64_t raw = static_cast<std::int64_t>(rawNow100ns());
        wall_base_100ns_ = sys > raw ? static_cast<std::uint64_t>(sys - raw) : 0;
    }

    std::uint64_t epoch_100ns_ = 0;
    // 墙钟 base（1970 起算的 100 ns；见 `wall100ns()` 的口径）。
    std::uint64_t wall_base_100ns_ = 0;
    bool wall_base_pinned_ = false;  // 测试注入 ⇒ 不再由 system_clock 捕获
    // 控制层暂停（`freeze()` / `unfreeze()`）：冻结区间对 guest 不可见。
    bool frozen_ = false;
    std::uint64_t freeze_start_100ns_ = 0;  // 本次冻结开始时的 raw 读数
    std::uint64_t paused_100ns_ = 0;        // 已累计的暂停区间（100 ns）
    RawNowFn raw_now_fn_ = nullptr;         // 测试缝（nullptr = steady_clock）
};

}  // namespace lift
