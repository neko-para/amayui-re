// emulator-lift · 虚拟时钟（确定性判据 A4 的基础）
//
// ★ 默认**虚拟时钟**：`timeGetTime` / `GetTickCount` / `QueryPerformanceCounter` 同源，
//   每执行一条指令走固定的 100ns 数，⛔ 不许把真时钟进 trace（plan §8.3-1）。
//   `--real-time` 才接宿主时钟，此时 A4 的逐字节断言**自动跳过并明说**。
// ★ 全部 inline：时钟是叶子设施，为一个 getter 建一个 .cpp 不值得。
#pragma once

#include <chrono>
#include <cstdint>

namespace lift {

class VirtualClock {
public:
    void setTicksPerInsn(std::uint64_t ticks) { ticks_per_insn_ = ticks ? ticks : 1; }
    std::uint64_t ticksPerInsn() const { return ticks_per_insn_; }
    void setRealTime(bool on) { real_ = on; }
    bool realTime() const { return real_; }

    void tick(std::uint64_t insns = 1) { now_100ns_ += ticks_per_insn_ * insns; }
    void advance100ns(std::uint64_t v) { now_100ns_ += v; }

    // =================================================================================
    // ★ 控制层暂停 = **冻结区间对 guest 不可见**（`design.md` §3.3）：
    //   冻结期间 guest 读到的 `now100ns()` / `nowMs()` 不前进；解冻后把整段暂停从 raw 里减掉。
    //   ⇒ guest 看到的时间序列与"没暂停"**逐位相同**（暂停前/后的读数也逐位相同）。
    //
    // ★ 默认档（从不 freeze）：`paused_100ns_ == 0 && !frozen_` ⇒ `now100ns()` 与改动前**逐位等价**
    //   （raw - 0）。⛔ 这不是"新增的时钟档位"：虚拟档（`real_ == false`）一个分支都不多。
    // ★ 本头是**两臂共享面**（解释器 `Machine::clock()` 也用它）⇒ 未冻结路径的行为不许变。
    // =================================================================================
    // 进入冻结。`real_ == false`（虚拟时钟档）⇒ 无操作：那一档的时间由 `tick()` 推进，不读墙钟。
    void freeze() {
        if (!real_ || frozen_) return;
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
        if (!real_) return now_100ns_;
        const std::uint64_t raw = rawNow100ns();
        if (!frozen_) return raw - paused_100ns_;
        // 冻结中：时间停在 freeze_start_ - paused_（再取 raw 也不会前进）。
        return freeze_start_100ns_ - paused_100ns_;
    }
    // 毫秒（timeGetTime / GetTickCount）
    std::uint32_t nowMs() const { return static_cast<std::uint32_t>(now100ns() / 10000ull); }
    // QPC：仿真频率固定 ⇒ 可复现
    std::int64_t qpc() const {
        const std::uint64_t t = now100ns();
        return static_cast<std::int64_t>((t * 3579545ull) / 10000000ull);
    }
    std::int64_t qpcFrequency() const { return 3579545; }

    std::uint64_t epoch100ns() const { return epoch_100ns_; }  // FILETIME 起点（1601）之后的基础偏移
    void setEpoch100ns(std::uint64_t v) { epoch_100ns_ = v; }
    void reset() { now_100ns_ = 0; }

    // ★ 测试缝：注入"原始墙钟读数"（100ns）。nullptr = `steady_clock`（生产路径）。
    //   ⛔ 只给 CTest 用：它只影响 `real_ == true` 档，且不改任何判据（冻结语义的判据靠它才不需要真睡）。
    using RawNowFn = std::uint64_t (*)();
    void setRawNowForTest(RawNowFn fn) {
        raw_now_fn_ = fn;
        paused_100ns_ = 0;
        frozen_ = false;
        freeze_start_100ns_ = 0;
    }

private:
    // 原始墙钟（100 ns）：未注入 ⇒ `steady_clock`（与改动前**同一个**取时表达式）。
    std::uint64_t rawNow100ns() const {
        if (raw_now_fn_ != nullptr) return raw_now_fn_();
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100);
    }

    std::uint64_t ticks_per_insn_ = 1;
    std::uint64_t now_100ns_ = 0;
    std::uint64_t epoch_100ns_ = 0;
    bool real_ = false;
    // 控制层暂停（`freeze()` / `unfreeze()`）：冻结区间对 guest 不可见。
    bool frozen_ = false;
    std::uint64_t freeze_start_100ns_ = 0;  // 本次冻结开始时的 raw 读数
    std::uint64_t paused_100ns_ = 0;        // 已累计的暂停区间（100 ns）
    RawNowFn raw_now_fn_ = nullptr;         // 测试缝（nullptr = steady_clock）
};

}  // namespace lift
