// emulator-lift · **AOT 窗口 profile** 的闭式实现（`window_prof.h` 的唯一 out-of-line 部分）
//
// ★ 为什么单独一个 `.cpp`（而不是全塞在纯头里）：
//   `window_prof.h` 被 `aot_host.h` 包含 ⇒ 它的任何改动都会重编**整条 AOT 臂**。
//   而"闭式的算术"正是 `tools/mutate-check.mjs` 要施加变异的地方 ⇒ 头里变异 = 一条变异
//   两次全量重编（实测 12 分钟）。搬到这里之后，变异只重编 **1 个 TU + 链接**（几十秒）。
//   ⛔ 不要把 `totals()` / `netSpan()` 搬回头里 —— 那会把单条变异的时间重新变成分钟级。
//
// ★ 归属：本文件编进 **`emulator-lift-runtime`**（`lift-aot-window-profile` 只链那个 target，
//   AOT 臂也链它）⇒ 一份实现两处用。⛔ 不要只放进 AOT target（测试会链接失败）。
//
// ★ 口径（三条，详见 `docs/03-emulator/measurement.md` M-12）：
//   ① **绝对量来自精确包络** `wall − shim − partial − instrument`；采样只决定 `lift:boundary` 的**比值**。
//      为什么：被计时区间里恒含一次取时，外推要乘 `iter/sampled`（≈64）⇒ 那 30 ns 会被放大 64×
//      （实测 FELLOW init：直接外推 1.77 s vs 墙钟 1.62 s）。
//   ② `unaccounted` 是**定义式残差**，归一之后**结构上恒 ~0** ⇒ 它会一直打印（设计如此），
//      但它**不是**作废判据。**真正会作废的是 `coverage`**（`∉ [0.5, 2]` ⇒ `suspect`）。
//   ③ `kInvalidRatio`（3%）那道闸门仍在 `valid()` 里，只是残差恒 ~0 时它几乎不会触发。
#include "aot/window_prof.h"

namespace lift::aot {

std::uint64_t AotWindowProf::netSpan(std::uint64_t from_ns, std::uint64_t to_ns) const {
    const std::uint64_t span = to_ns - from_ns;
    const auto read = static_cast<std::uint64_t>(span_overhead_ns);
    return span > read ? span - read : 0;
}

AotWindowProf::Totals AotWindowProf::totals() const {
    Totals t;
    t.wall = static_cast<double>(t_close_ns > t_open_ns ? t_close_ns - t_open_ns : 0);
    const double scale = sampled != 0 ? static_cast<double>(iter) / static_cast<double>(sampled) : 0.0;
    const double lift_est = static_cast<double>(lift_ns) * scale;
    const double boundary_est = static_cast<double>(boundary_ns) * scale;
    t.shim = static_cast<double>(shim_ns);
    t.partial = static_cast<double>(partial_ns);
    // 仪器：每个采样迭代 2 次取时（`a`/`b` 或 `b`/`c` 各一次），每次 shim 调用 2 次
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

}  // namespace lift::aot
