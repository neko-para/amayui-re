// emulator-lift · **AOT 窗口 profile**（`profile_begin` / `profile_end` 之间的分桶账）
//
// ★ 为什么需要它：AOT 的 `--profile` 有四套归因（派发 EA / shim 名 / 指令 op / Present 子项），
//   但**全部钉在整轮**。而"打开 FELLOW"那一段是 **0.2 s 转场 + 1.39 s「零 Present」的密集计算**
//   （2026-11-17 实测，见 `docs/03-emulator/measurement.md`）⇒ 整轮口径会被别的段稀释
//   （整轮 shim 占 1.4% 不代表窗口内也是 1.4%）。窗口 = 把同一批桶**限在一段时间里**。
//
// ★ 闭式（必须成立；`unaccounted` 是**定义式**，恒有数、恒打印）：
//     wall = lift + boundary + shim + partial + instrument + unaccounted
//   · `unaccounted/wall > 3%` ⇒ 本次读数**作废**（打印 `INVALID`）—— 阈值来历见 M-12：
//     仪器残余 ≈1.6% ⇒ 3% 是 2× 余量。
//
// ★ 三条读法红线（⛔ 违反会让闭式立刻破）：
//   ① `present` ⊂ `shim`（`D3D9::Present` 是 shim，`present()` 在它内部被调）⇒ 只作子行。
//   ② `wait`（`Sleep` / `MsgWait*` / `GetMessage*` / `WaitForSingleObject` 名族）⊂ `shim`，
//      而且**没有 CPU** ⇒ 它是"guest 在等"的唯一机械证据，⛔ 不要与"guest 在算"混读。
//   ③ `lift_dyn` **不进闭式**：它是**尾调用**（`runtime/lift_abi.cpp` 的 `return fn(...)`），
//      区间含目标函数体 ⇒ 计时会与 `lift` 重复计 ⇒ 只报**次数**。
//
// ★ 成本（缺省档一行都不执行；护栏 = `on`）：
//   · shim：每次调用 2 次取时（窗口内**精确**；shim 只占派发 ~1.4% ⇒ <0.1%）
//   · lift / boundary：每 `kStride` 个派发取 3 次（≈1 ns/派发，<2%）；窗口内派发数
//     < `kFullSampleBelow` 时**自动全量**（夹具 / 小窗口要精确闭式）
//   · present 子桶：**零**（`presentBreakdown()` 本来就一直在计时）
//   · lift_dyn：一次 `++`（闸门是一个 bool）
//
// ★ 计数器与墙钟分栏（M-09）：`insns` 是"guest 指令条数"，跨构建可比；墙钟只在本机同构建内比。
//   ⇒ 每个窗口表都把 `insns` 打进表头，⚠ 判"这段有没有变慢"**只看 insns**。
#ifndef LIFT_AOT_WINDOW_PROF_H
#define LIFT_AOT_WINDOW_PROF_H

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/present.h"  // `PresentBreakdown`（present 子桶的唯一来源）

namespace lift::aot {

// 窗口账的单调时钟（ns）。★ 与 `profileTicks()` 的 TSC **不同一把尺**：窗口桶要和
// `presentBreakdown()`（本来就是 steady_clock 纳秒）直接相加，混两把尺会让闭式差一个频率因子。
inline std::uint64_t windowNowNs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

struct AotWindowProf {
    // ---- 采样率（会打进输出头，⛔ 不把数字抄进散文）----
    // ★ **全窗口一个 stride**，⛔ 不"前 N 个全量、之后 1/64"：那种混合抽样会让均值被**开头那段**
    //   主导（实测 FELLOW init：前 20 万迭代是装载期、后面是轻轮询 ⇒ 混起来外推**超过墙钟**）。
    static constexpr std::uint64_t kStride = 64;
    // ★ 样本数告警阈（只影响表头的提示，**不改变抽样方式**）：窗口太短时样本少 ⇒ 残差会自己变大，
    //   由 `unaccounted` 那一行如实暴露，⛔ 不要靠"自动全量"掩盖。
    static constexpr std::uint64_t kLowSampleBelow = 200000;
    // ★ 作废阈（单一数字，写在这里；理由见 `measurement.md` M-12）
    static constexpr double kInvalidRatio = 0.03;

    bool on = false;
    std::string label;

    // ---- 窗口两端的快照（由宿主在标记处填）----
    std::uint64_t t_open_ns = 0;
    std::uint64_t t_close_ns = 0;
    std::uint64_t insns_open = 0;
    std::uint64_t insns_close = 0;
    std::uint64_t frames_open = 0;
    std::uint64_t frames_close = 0;
    std::uint64_t liftdyn_open = 0;
    std::uint64_t liftdyn_close = 0;

    // ---- 迭代 / 采样 ----
    std::uint64_t iter = 0;      // 窗口内经过的派发迭代数（lift + shim）
    std::uint64_t sampled = 0;   // 真正取了时的迭代数（外推分母）

    // ---- 桶（ns）----
    std::uint64_t lift_ns = 0;      // 采样：`fn(...)` 区间
    std::uint64_t boundary_ns = 0;  // 采样：块顶 → 分派前（闸门 / 定时器泵 / Present 泵 / 派发表查找）
    std::uint64_t shim_ns = 0;      // **精确**：每次 `lift_shim` 的整段
    std::uint64_t shim_calls = 0;
    std::uint64_t partial_ns = 0;   // 两个窗口边沿的 sliver（精确）
    // 首个被计时的迭代起点 / 最后一个被计时迭代的终点（算 partial 用）
    std::uint64_t first_a_ns = 0;
    std::uint64_t last_c_ns = 0;
    bool have_first = false;

    // ---- 每个 shim 的精确账（按 `ShimDef::id` 索引；打印时由宿主解析名字）----
    std::vector<std::uint64_t> per_shim_ns;
    std::vector<std::uint64_t> per_shim_calls;

    // ---- present 子桶（ns 累计；直接来自 `presentBreakdown()`，零额外成本）----
    std::uint64_t present_calls = 0;
    std::uint64_t present_readback_ns = 0;
    std::uint64_t present_hash_ns = 0;
    std::uint64_t present_freeze_ns = 0;
    std::uint64_t present_sink_ns = 0;
    std::uint64_t present_stats_ns = 0;

    // 一次取时的实测开销（ns）——从 `instrument` 那行扣出来，别让它混进被测量
    double clock_read_ns = 0.0;
    // ★ **一个被计时的区间**里含的那一次取时（实测：`t0=now(); t1=now()` 的 `t1-t0` 的均值）。
    //   为什么不用 `clock_read_ns`：那量的是"调 now() 一圈"（含循环体），与"区间里的那一刀"不同量级；
    //   实测差别足以把 FELLOW init 的外推顶到 1.36×（⇒ coverage 1.29 红）。
    double span_overhead_ns = 0.0;

    void reset() { *this = AotWindowProf{}; }

    // 一次实测区间的**净工作**：区间里含一次取时（时间戳在取时调用里读，剩下的调用开销
    // 落在区间内）⇒ 扣掉一次 `clock_read_ns`。
    // ★ 为什么必须扣（2026-11-17 实测）：不扣的话那 ~25 ns 会被 `iter/sampled` 外推
    //   64×（FELLOW init：外推和 3.1 s > 墙钟 2.0 s，`bias` 2.08×）。
    std::uint64_t netSpan(std::uint64_t from_ns, std::uint64_t to_ns) const {
        const std::uint64_t span = to_ns - from_ns;
        const auto read = static_cast<std::uint64_t>(span_overhead_ns);
        return span > read ? span - read : 0;
    }

    // 窗口内第 `iter` 个派发是否取时。
    // ★ 用**乘性散列**取高 6 位 ⇒ 平均 1/64，且**不与 guest 循环周期对齐**。
    //   ⛔ 不要写 `iter % kStride == 0`：实测 FELLOW init 里那样会恒定采到同相位
    //   （1.7 s 的窗口里样本均值比真实均值高 1.6× ⇒ 桶之和 3.1 s > 墙钟）。
    bool sampleDue() const { return ((iter * 0x9E3779B97F4A7C15ull) >> 58) == 0; }

    void noteBoundary(std::uint64_t a_ns, std::uint64_t b_ns) {
        // ★ 开窗那一刀可能落在"本迭代的 boundary 段里"（`profile_begin` 在读点里执行）⇒
        //   这一段的 `a` 比窗口起点还早。必须**夹到窗口起点**，否则那截会被算进窗口
        //   （实测症状：`unaccounted` 变成负数，且 `partial` 无符号下溢成 1.8e13 ms）。
        if (a_ns < t_open_ns) a_ns = t_open_ns;
        if (b_ns <= a_ns) return;
        boundary_ns += netSpan(a_ns, b_ns);
        if (!have_first) {
            have_first = true;
            first_a_ns = a_ns;
        }
    }
    void noteLift(std::uint64_t b_ns, std::uint64_t c_ns) {
        if (c_ns <= b_ns) return;
        lift_ns += netSpan(b_ns, c_ns);
        last_c_ns = c_ns;
    }
    // 一次 shim 调用（`dt` 是精确区间）。`id` 越界（不该发生）只记总量、不记明细。
    void noteShim(std::uint64_t id, std::uint64_t dt_ns) {
        const std::uint64_t net = dt_ns > static_cast<std::uint64_t>(clock_read_ns)
                                      ? dt_ns - static_cast<std::uint64_t>(clock_read_ns)
                                      : 0;
        if (id < per_shim_ns.size()) {
            per_shim_ns[id] += net;
            ++per_shim_calls[id];
        }
        shim_ns += net;
        ++shim_calls;
    }
    void notePresent(const PresentBreakdown& pb, std::uint64_t n = 1) {
        present_calls += n;
        present_readback_ns += pb.readback_ns;
        present_hash_ns += pb.hash_ns;
        present_freeze_ns += pb.freeze_ns;
        present_sink_ns += pb.sink_ns;
        present_stats_ns += pb.stats_ns;
    }

    // 收口时算两个边沿的 sliver（**精确**；窗口开/关都发生在读点里，未必正好在迭代边界上）。
    void closeEdges(std::uint64_t t_close) {
        t_close_ns = t_close;
        if (t_close_ns < t_open_ns) t_close_ns = t_open_ns;
        std::uint64_t first = have_first ? first_a_ns : t_close_ns;
        std::uint64_t last = have_first ? last_c_ns : t_open_ns;
        if (first < t_open_ns) first = t_open_ns;
        if (first > t_close_ns) first = t_close_ns;
        if (last < t_open_ns) last = t_open_ns;
        if (last > t_close_ns) last = t_close_ns;
        partial_ns = (first - t_open_ns) + (t_close_ns - last);
    }

    struct Totals {
        double wall = 0.0;        // 标记 → 标记
        double lift = 0.0;        // 外推（每个采样区间已扣一次取时）
        double boundary = 0.0;    // 同上
        double shim = 0.0;        // 精确（也扣了取时）
        double partial = 0.0;     // 精确
        double instrument = 0.0;  // 仪器自身（单列；⛔ 不塞进别的桶）
        double unaccounted = 0.0; // 定义式 = wall − 上面五项
        // ★ 自检行：`采样外推 / 精确包络`。1.00 = 采样均值与真实均值一致。
        //   ⚠ 实测在 1.0–1.3 间波动（采样恒带仪器税、且短窗样本少）⇒ **只作诊断，不作判据**；
        //   只有离谱到 [0.5, 2] 之外才判作废（那时份额也救不回来了）。
        double coverage = 1.0;
        bool suspect = false;
        static constexpr double kMinCoverage = 0.5;
        static constexpr double kMaxCoverage = 2.0;
        double ratio() const { return wall > 0.0 ? unaccounted / wall : 0.0; }
        bool valid() const { return !suspect && ratio() <= AotWindowProf::kInvalidRatio; }
    };

    // ★ 闭式的唯一出口。⛔ 不要在别处另算一套。
    //
    // ★ 为什么"份额来自采样、绝对值来自包络"（2026-11-17 实测教训）：
    //   被计时的区间里恒含仪器税（一次取时），而外推要乘 `iter/sampled`（≈64）⇒ 那 30 ns 会被
    //   放大 64×。实测 FELLOW init：直接外推得 1.77 s，而墙钟只有 1.62 s（且减去 shim 后只剩 1.34 s）。
    //   ⇒ 绝对量用**精确包络** `wall − shim − partial − instrument`，采样只决定 `lift:boundary` 的**比值**。
    //   代价：`unaccounted` 在结构上恒 ~0（归一的必然）⇒ 取而代之的自检是 `coverage` 那一行。
    Totals totals() const {
        Totals t;
        t.wall = static_cast<double>(t_close_ns > t_open_ns ? t_close_ns - t_open_ns : 0);
        const double scale = sampled != 0 ? static_cast<double>(iter) / static_cast<double>(sampled) : 0.0;
        const double lift_est = static_cast<double>(lift_ns) * scale;
        const double boundary_est = static_cast<double>(boundary_ns) * scale;
        t.shim = static_cast<double>(shim_ns);
        t.partial = static_cast<double>(partial_ns);
        const double reads = static_cast<double>(sampled) * 2.0 + static_cast<double>(shim_calls) * 2.0;
        t.instrument = reads * clock_read_ns;
        double envelope = t.wall - t.shim - t.partial - t.instrument;
        if (envelope < 0.0) envelope = 0.0;
        const double covered = lift_est + boundary_est;
        t.coverage = envelope > 0.0 ? covered / envelope : (covered > 0.0 ? 1.0e9 : 1.0);
        t.suspect = t.coverage < Totals::kMinCoverage || t.coverage > Totals::kMaxCoverage;
        if (covered > 0.0) {
            t.lift = lift_est / covered * envelope;
            t.boundary = boundary_est / covered * envelope;
        }
        t.unaccounted = t.wall - (t.lift + t.boundary + t.shim + t.partial + t.instrument);
        if (t.unaccounted < 1.0) t.unaccounted = 0.0;  // 浮点噪声（归一后恒 0）
        return t;
    }

    // 具名行的判定档（`measurement.md` M-12 §5 的单一阈值）
    enum class Rank { Noise = 0, Watch = 1, Candidate = 2 };
    static Rank rankOf(double share) {
        if (share >= 0.10) return Rank::Candidate;
        if (share >= 0.03) return Rank::Watch;
        return Rank::Noise;
    }
};

}  // namespace lift::aot

#endif  // LIFT_AOT_WINDOW_PROF_H
