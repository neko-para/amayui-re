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

    // 100 ns 单位（与 Win32 的 FILETIME 同源）
    std::uint64_t now100ns() const {
        if (!real_) return now_100ns_;
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100);
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

private:
    std::uint64_t ticks_per_insn_ = 1;
    std::uint64_t now_100ns_ = 0;
    std::uint64_t epoch_100ns_ = 0;
    bool real_ = false;
};

}  // namespace lift
