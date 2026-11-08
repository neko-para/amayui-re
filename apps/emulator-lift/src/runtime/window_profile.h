// emulator-lift · **可定界的区间 profiler**（`profile_begin` / `profile_end`）
//
// ★ 为什么有它（2026-11-08）：`--profile` 只给**整轮**的热点（块命中 × 指令数、shim 总墙钟），
//   `LIFT_FRAME_TRACE=1` 只给**逐帧**的粗分。要回答"点进 EU-ROOM 的那 4 s 里谁吃的"，
//   需要一段**由脚本定界**的墙钟，并且分项相加要能闭合到这段墙钟。
//
// ★ 闭式（唯一判据）：
//       wall = guest + shim + native + interp + host + unattributed
//   各项**互斥**（都在 dispatcher 的同一条时间轴上切段），`unattributed = wall - 其余之和`。
//   差额恒等于"没被任何一段覆盖"的时间 ⇒ 允许微小的正/负值（打印出来，⛔ 不吞掉）。
//
// ★ 分项口径（每一项都答"为什么它不会和别的项重复"）：
//   - `guest`   = **块执行**墙钟（dispatcher 里 `for (Insn&)` 那一段）。只记在块循环里。
//   - `shim`    = `ShimRegistry::invoke` 的整段墙钟（**含** Present / 像素循环 / GPU submit）。
//                 shim 里不会再跑 guest 块（嵌套 guest 回调走 `GuestCallStack`，回 dispatcher 执行），
//                 所以与 `guest` 不重叠。`gpu` 那一栏是 Present 的**内部拆解**（⊂ shim，不重复计入）。
//   - `native`  = 函数级 / 段级 native override 的宿主实现（替代了 guest 指令，所以不算 guest）。
//   - `host`    = 音频泵 / 事件泵 / 输入脚本 tick / 定时器（宿主侧、且不是"解释器"）。
//   - `interp`  = 一次 dispatcher 迭代里**剩下的全部**（块缓存查找 / 时钟 / 令牌 / profile 记账 …）。
//                 由 RAII 迭代守卫按"迭代墙钟 − 上面已记的段"算出 ⇒ 覆盖"解释器自身"。
//   - `unattributed` = wall − Σ其它。★ 它不是零：嵌套/异常路径会漏计或重计，所以**必须打印**。
//
// ★ 包容 vs 自身（guest 函数表）：
//   - **self** = 该函数的块执行墙钟（块级计时，按 `.lst` 函数入口聚合）。**互斥**：所有 self 之和 = `guest`。
//   - **inclusive** = 该函数在**栈上**的墙钟（`call` 进入 → `ret` 离开）。按"同一函数只算最外层一帧"
//     累计 ⇒ 递归不会把时间算两遍。inclusive 是**树**（父包含子），⛔ 不能相加，⛔ 不参与上面的闭式。
//   - 影子栈在 `call`（`Op::Call`）压帧、`ret`（`Op::Ret`）弹帧；对不上就计数（`unbalanced`）并
//     **不猜** —— 数会打印出来。
//
// ★ 触发源是**条件**，不是"帧号"（见 input_script.h 的 TriggerKind）：
//   窗口边界 = 脚本动作被触发的那一个 **block 边界**。因此 `at frame` / `at insn` / `at guest_ms`
//   都能当边界；将来加"场景/脚本标记"只需给 Trigger 加一种 kind，本类不用改。
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lift {

class BlockCache;
class ShimRegistry;

// 窗口内的一行"guest 函数"记账。self 互斥（求和 = guest）；inclusive 是树（父子重叠，不可相加）。
struct WindowFnRow {
    std::uint32_t key = 0;
    std::uint64_t self_ns = 0;
    std::uint64_t inclusive_ns = 0;
    std::uint64_t calls = 0;  // 只数"最外层"帧的进入次数
};

struct WindowShimRow {
    std::uint32_t id = 0;
    std::uint64_t ns = 0;
    std::uint64_t calls = 0;
};

struct WindowReport {
    std::string label;
    std::uint64_t wall_ns = 0;
    std::uint64_t guest_ns = 0;
    std::uint64_t shim_ns = 0;
    std::uint64_t native_ns = 0;
    std::uint64_t interp_ns = 0;
    std::uint64_t host_ns[4] = {0, 0, 0, 0};
    std::uint64_t host_total_ns = 0;
    // unattributed = wall − (guest + shim + native + interp + host)
    std::int64_t unattributed_ns = 0;
    std::uint64_t begin_frame = 0;
    std::uint64_t end_frame = 0;
    std::uint64_t begin_insns = 0;
    std::uint64_t end_insns = 0;
    std::uint64_t begin_guest_ms = 0;
    std::uint64_t end_guest_ms = 0;
    std::vector<WindowFnRow> fns;      // 按 self 降序
    std::vector<WindowShimRow> shims;  // 按 ns 降序
    std::uint64_t gpu_readback_ns = 0;
    std::uint64_t gpu_hash_ns = 0;
    std::uint64_t gpu_stats_ns = 0;
    std::uint64_t gpu_sink_ns = 0;
    std::uint64_t gpu_frames = 0;   // 窗口内 D3D9 Present 次数
    std::uint64_t unbalanced = 0;   // ret 对不上帧的次数（inclusive 的可信度指标）
    std::uint64_t open_at_end = 0;  // 窗口结束时仍在栈上的帧数
    std::uint64_t iterations = 0;
};

class WindowProfiler {
public:
    using Clock = std::chrono::steady_clock;

    // 宿主侧归因的固定槽位（闭集合：加一项就改这里 + 打印）
    enum HostSlot : int {
        HostAudio = 0,      // 音频设备推进 / 喂料泵
        HostEventPump = 1,  // presenter 的宿主事件泵（SDL 事件 → WM_*）
        HostInput = 2,      // 输入脚本 tick（注入本身）
        HostTimer = 3,      // 到期定时器的宿主侧开销（guest 回调体算 guest）
        HostSlotCount = 4,
    };
    static const char* hostSlotName(int slot);

    // ---- 开关 ----
    bool armed() const { return armed_; }
    void setArmed(bool on) { armed_ = on; }
    bool active() const { return active_; }
    bool finished() const { return finished_; }
    const std::string& label() const { return label_; }

    // ---- 窗口（由 Machine 在 block 边界调用）----
    void begin(const std::string& label, std::uint64_t frame, std::uint64_t insns, std::uint64_t guest_ms);
    void end(std::uint64_t frame, std::uint64_t insns, std::uint64_t guest_ms);
    // end 之后由 Machine 调用：把"全局累计量"换成"窗口内增量"
    void finishBlocks(const BlockCache& cache);
    void finishShims(const ShimRegistry& shims);
    void finishPresent();
    // begin 时的基线快照（Machine 调用；必须在任何一段执行之前）
    void snapshotBlocks(const BlockCache& cache);
    void snapshotShims(const ShimRegistry& shims);
    void snapshotPresent();
    // 取走结果（打印一次后清掉）
    bool takeReport(const std::function<std::uint32_t(std::uint32_t)>& fn_key, WindowReport* out);

    // ---- 段记账（窗口外一律 no-op）----
    void addGuest(std::uint64_t ns);
    void addShim(std::uint64_t ns);
    void addNative(std::uint64_t ns);
    void addHost(int slot, std::uint64_t ns);
    // dispatcher 迭代边界：`iterationEnd` 把"没被上面任何一段覆盖的墙钟"记进 interp。
    void iterationBegin(Clock::time_point now);
    void iterationEnd(Clock::time_point now);

    // ---- guest 影子调用栈（inclusive；窗口外 no-op）----
    void pushCall(std::uint32_t fn_key, std::uint32_t ret_ea);
    void popRet(std::uint32_t eip);
    void popNoReturn(std::uint32_t fn_key);  // native override / shim 调用：没有 ret 指令

private:
    struct Frame {
        std::uint32_t fn_key = 0;
        std::uint32_t ret_ea = 0;
        Clock::time_point start{};
    };
    void attributeFrame(const Frame& f, Clock::time_point now);
    void closeAllFrames(Clock::time_point now, bool count_open);

    bool armed_ = false;
    bool active_ = false;
    bool finished_ = false;

    std::string label_;
    Clock::time_point begin_wall_{};
    Clock::time_point end_wall_{};
    std::uint64_t begin_frame_ = 0;
    std::uint64_t begin_insns_ = 0;
    std::uint64_t begin_guest_ms_ = 0;
    std::uint64_t end_frame_ = 0;
    std::uint64_t end_insns_ = 0;
    std::uint64_t end_guest_ms_ = 0;

    std::uint64_t guest_ns_ = 0;
    std::uint64_t shim_ns_ = 0;
    std::uint64_t native_ns_ = 0;
    std::uint64_t interp_ns_ = 0;
    std::uint64_t host_ns_[HostSlotCount] = {0, 0, 0, 0};
    std::uint64_t iterations_ = 0;

    // 一次 dispatcher 迭代：守卫用"迭代墙钟 − 已记段"补 interp
    Clock::time_point iter_start_{};
    std::uint64_t iter_attributed_ns_ = 0;
    // 上一个迭代的终点：下一个迭代的起点从它算起（⇒ 迭代之间的间隙也进账，不落进 unattributed）
    Clock::time_point prev_end_{};
    bool prev_end_valid_ = false;

    // 快照（全局累计量 → 窗口内增量）
    std::unordered_map<std::uint32_t, std::uint64_t> block_ns_at_begin_;
    std::unordered_map<std::uint32_t, std::uint64_t> block_ns_delta_;
    std::vector<std::uint64_t> shim_ns_at_begin_;
    std::vector<std::uint64_t> shim_calls_at_begin_;
    std::vector<WindowShimRow> shim_rows_;
    std::uint64_t present_at_begin_[4] = {0, 0, 0, 0};
    std::uint64_t gpu_readback_ns_ = 0;
    std::uint64_t gpu_hash_ns_ = 0;
    std::uint64_t gpu_stats_ns_ = 0;
    std::uint64_t gpu_sink_ns_ = 0;

    // guest 影子调用栈（inclusive）
    std::vector<Frame> stack_;
    std::unordered_map<std::uint32_t, std::uint64_t> incl_ns_;
    std::unordered_map<std::uint32_t, std::uint32_t> top_calls_;   // 当前栈上该函数的帧数（排重用）
    std::unordered_map<std::uint32_t, std::uint64_t> calls_by_fn_;  // 最外层进入次数
    std::uint64_t unbalanced_ = 0;
    std::uint64_t open_at_end_ = 0;
};

}  // namespace lift
