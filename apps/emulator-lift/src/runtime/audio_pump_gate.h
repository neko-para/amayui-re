// emulator-lift · 音频**泵闸门**（S3）的唯一一份纯判据 + 节流记账
//
// ★ 为什么抽这个头：S3 要回答的是"**这一块边界该不该调 `AudioEngine::pump()`**"，
//   而这条判据有三个很容易**悄悄错**的面：
//     ① **off 档必须一次都不进 pump**（⛔ 不是"进了 pump 立刻返回"）—— 默认档
//        （`--audio off`，即不带任何 `--audio`）的 stdout / stderr 必须逐字不变
//        （`REQ-01M7JZM1QQ728KX1C0G2F232T1` 最硬的一条）；把 `enabled` 判在 `pump()` **里面**
//        就等于每次块边界都去抢一次 audio mutex，虽然"看起来没输出"，但那是两回事。
//     ② **不是每块都 pump**：块边界是热路径（每一次派发都过一遍），音频只要 ~kCheckStride
//        条 guest 指令推进一次就够（与 `--time-limit` / 定时器泵同一个节流口径）。
//     ③ **嵌套 guest 调用进行中不许泵**：`pump()` 会经 `startNestedGuestCall` **再铺一个 guest 帧**，
//        那一刻正在跑另一段 guest 代码（GuestCallStack 帧 / winmm 定时器回调 / SEH 派发），
//        插一个 fill 帧会改坏仍然活着的上下文（真机上它们是两个线程）——
//        与解释器 `machine.cpp:1532` 的 `!guest_calls_.active() && !cpu_.shim.timer_firing && !seh_.busy()`
//        是**同一条**判据（AOT 臂的对应物是 `calls_.active()`）。
//   ⇒ 三者提成 3 行纯函数 + 一个纯状态机，可被 pure CTest 直接驱动
//     （`tests/aot_audio_pump.cpp`：off 档 20 万块边界一次都不 due、节流恰好每 stride 一次、
//      嵌套期间不 due 且**解除后立刻补上**、线程档不 due）。
//
// ★ 本头**纯**：只 `<cstdint>`，不 include audio.h / x86 / guest 头 ⇒ 无分配、无设备、无锁。
//   ⛔ 不要在这里 include `runtime/audio.h`（那会把 SDL / guest 头拖进 pure 用例）。
//
// ★ 与 S0 的关系：`thread_mode` 这一格就是 `audioThreadMode(mode, realTime)` 的输出
//   （`runtime/audio_policy.h`，两个事实；"宿主给没给 GuestLayout"由 `attach` 的引用在装配期钉死）。
//   AOT 在 `--audio sdl` + 墙钟 + 设备真的打开时**会**是真 feeder 线程（这是用户裁决的形状），
//   `--audio virtual` / 设备回退仍是主线程 D 泵。thread 档下喂料由 feeder 线程自己做，
//   主线程**不该**再泵（解释器里那也是 `pump()` 一进来就 `return Idle` 的无用功）。
//   ★ 本闸门**不表达** feeder 的失败面：thread 档 `due()` 恒假 ⇒ `pump()` 里的 `feeder_error_`
//     早退不可达 ⇒ AOT 的块边界必须直接看 `AudioEngine::feederFailed()`。
#pragma once

#include <cstdint>

namespace lift {

// ★ S3：块边界"要不要泵"的全部输入 —— 每一项都是**事实**，没有一项是选项。
struct AudioPumpFacts {
    // `AudioEngine::enabled()`（档位 != Off）。★ **这一格为假 ⇒ 一次都不许调 pump()**。
    bool enabled = false;
    // `AudioEngine::threadMode()`（= S0 谓词 `audioThreadMode(mode, realTime)` 的输出）。
    bool thread_mode = false;
    // 嵌套 guest 执行进行中：`calls_.active()`（GuestCallStack 帧）/ winmm 定时器回调 / SEH 派发。
    bool nested_guest_active = false;
    // 本块入口的 `cpu_.insns`（节流口径：**只数 guest 指令**，不含宿主时间）。
    std::uint64_t insns = 0;
};

// ★ S3 的**节流 + 记账**状态机（唯一一处）。它不碰时钟 / 设备 / guest 内存。
//
// ★ 口径（每一格都有对应的 pure 断言）：
//   · `due()` 为真 ⇒ 调用方**必须**调一次 `pump()`，然后 `noteCalled(insns)`；
//   · `due()` 为假 ⇒ 调用方**一次都不许**调 `pump()`（`calls()` 因此 == 真调 pump 的次数）；
//   · `enabled == false` ⇒ `due()` 恒假、`next_check_` **原地不动**、`calls()` 恒 0；
//   · 嵌套 guest 期间 `due()` 也恒假，但 `next_check_` **不推进** ⇒ 解除之后
//     **立刻**补一次（不用再等一个 stride：fill 迟到 4096 条指令以上会真的欠料）。
class AudioPumpGate {
public:
    // ★ 与 `aot_run.cpp` 的 `kCheckStride` / `machine.cpp` 的块边界节流**同一粒度**；
    //   两处相等由 `aot_run.cpp` 的 `static_assert` 钉住（⛔ 不要各写各的）。
    static constexpr std::uint64_t kStride = 4096u;

    bool due(const AudioPumpFacts& f) const {
        if (!f.enabled) return false;             // ★ off 档：一次都不进 pump（本单最硬的一条）
        if (f.thread_mode) return false;          // thread 档由 feeder 线程喂，主线程不泵
        if (f.nested_guest_active) return false;  // 嵌套 guest 中不铺新帧
        return f.insns >= next_check_;
    }

    // 真的调过 `pump()` 之后登记：记账 + 推进节流点（**只有真的调过才推进**）。
    void noteCalled(std::uint64_t insns) {
        ++calls_;
        next_check_ = insns + kStride;
    }

    // 真调 `pump()` 的次数（off 档必须恒 0；AOT 摘要 / `LIFT_AOT_AUDIO_TRACE` 的读数就是它）。
    std::uint64_t calls() const { return calls_; }
    // 下一次允许泵的 `insns`（0 = 还没泵过 ⇒ 第一个块边界就允许）。
    std::uint64_t nextCheck() const { return next_check_; }

private:
    std::uint64_t next_check_ = 0;
    std::uint64_t calls_ = 0;
};

}  // namespace lift
