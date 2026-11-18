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

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace lift::aot {

namespace {

// 一行文本的**纯**格式化（`vsnprintf` 写内存，不碰 stdout）。⭐ 用与旧 `printf` 同一套格式串 ⇒
// 同一条 C 运行库把同一个 double 渲染成同一串字节（这就是"stdout 逐字不变"的机械基础）。
std::string formatLine(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    const int need = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (need <= 0) {
        va_end(ap2);
        return std::string();
    }
    std::vector<char> buf(static_cast<std::size_t>(need) + 1, '\0');
    std::vsnprintf(buf.data(), buf.size(), fmt, ap2);
    va_end(ap2);
    return std::string(buf.data(), static_cast<std::size_t>(need));
}

}  // namespace

bool AotWindowProf::beginWindow() {
    // ★ 已开 ⇒ false + **一个字节都不动**（⛔ 不静默 reset）。未开才清上一轮的账。
    if (on) return false;
    reset();
    on = true;
    return true;
}

bool AotWindowProf::closeWindow() {
    if (!on) return false;
    on = false;
    return true;
}

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

std::string formatWindowReportText(const AotWindowProf& w, const ShimNameFn& shimName, int top) {
    // ★ `kMaxReportLines` 是**硬**上限（见头注）：具名行 + `(rest)` 都在预算内。
    constexpr int kMaxReportLines = 20;
    // 固定行 = header / wall / lift / boundary / shim / partial / instrument / unaccounted /
    //          coverage / lift_dyn / verdict（11）＋ 条件行 present（1）。
    int fixed_lines = 11;
    if (w.present_calls != 0) ++fixed_lines;

    const AotWindowProf::Totals t = w.totals();
    const auto ms = [](double ns) { return ns / 1.0e6; };
    const auto pct = [&t](double v) { return t.wall > 0.0 ? (v / t.wall) * 100.0 : 0.0; };
    const auto nameOf = [&shimName](std::uint32_t id) {
        if (!shimName) return std::string("(unknown)");
        std::string n = shimName(id);
        if (n.empty()) n = "(unknown)";
        return n;
    };

    // ---- 具名行（按精确 ns 降序；与旧 printf 块同一条判据）----
    struct Row {
        double ns = 0.0;
        std::uint64_t calls = 0;
        std::string name;
    };
    std::vector<Row> rows;
    rows.reserve(w.per_shim_ns.size());
    for (std::size_t id = 0; id < w.per_shim_ns.size(); ++id) {
        if (w.per_shim_calls[id] == 0) continue;
        rows.push_back(Row{static_cast<double>(w.per_shim_ns[id]), w.per_shim_calls[id],
                           nameOf(static_cast<std::uint32_t>(id))});
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.ns > b.ns; });
    const auto shareOf = [&t](const Row& r) { return t.wall > 0.0 ? r.ns / t.wall : 0.0; };
    int above = 0;  // ≥0.5% 的具名行数（排序后连续；余下的都进 `(rest)`）
    for (const Row& r : rows) {
        if (shareOf(r) < 0.005) break;
        ++above;
    }
    // ★ 行数预算：先按 `top` 打，超 20 行时**先减具名行**（余下并进 `(rest)`）。
    int named = std::min(top < 0 ? 0 : top, above);
    bool has_rest = static_cast<int>(rows.size()) > named;
    while (named > 0 && fixed_lines + named + (has_rest ? 1 : 0) > kMaxReportLines) {
        --named;
        has_rest = static_cast<int>(rows.size()) > named;
    }

    std::string out;
    out += formatLine("=== profile window: %s ===\n", w.label.c_str());
    out += formatLine(
        "wall        : %10.3f ms   iter=%llu sampled=%llu（stride %llu%s）insns=%llu frames=%llu\n",
        ms(t.wall), static_cast<unsigned long long>(w.iter),
        static_cast<unsigned long long>(w.sampled),
        static_cast<unsigned long long>(AotWindowProf::kStride),
        w.iter < AotWindowProf::kLowSampleBelow ? "，⚠ 窗口短 ⇒ 样本少" : "",
        static_cast<unsigned long long>(w.insns_close - w.insns_open),
        static_cast<unsigned long long>(w.frames_close - w.frames_open));
    out += formatLine("lift        : %10.3f ms  %5.1f%%  est\n", ms(t.lift), pct(t.lift));
    out += formatLine("boundary    : %10.3f ms  %5.1f%%  est\n", ms(t.boundary), pct(t.boundary));
    out += formatLine("shim        : %10.3f ms  %5.1f%%  exact(N=%llu)\n", ms(t.shim), pct(t.shim),
                      static_cast<unsigned long long>(w.shim_calls));
    // present 子桶（⊂ shim；⛔ 不加进闭式）
    if (w.present_calls != 0) {
        const double psum = static_cast<double>(w.present_readback_ns + w.present_hash_ns +
                                                w.present_freeze_ns + w.present_sink_ns +
                                                w.present_stats_ns);
        out += formatLine(
            "  present   : %10.3f ms  %5.1f%%  n=%llu（readback %.3f / hash %.3f / freeze %.3f / "
            "sink %.3f / stats %.3f ms）\n",
            ms(psum), pct(psum), static_cast<unsigned long long>(w.present_calls),
            ms(static_cast<double>(w.present_readback_ns)),
            ms(static_cast<double>(w.present_hash_ns)),
            ms(static_cast<double>(w.present_freeze_ns)),
            ms(static_cast<double>(w.present_sink_ns)),
            ms(static_cast<double>(w.present_stats_ns)));
    }
    // 每个 shim 的精确行（降序；只打前 `named` 条 ≥0.5% 的，余下合并成 `(rest)`）
    {
        double rest_ns = 0.0;
        std::uint64_t rest_calls = 0;
        int printed = 0;
        for (const Row& r : rows) {
            if (shareOf(r) < 0.005 || printed >= named) {
                rest_ns += r.ns;
                rest_calls += r.calls;
                continue;
            }
            out += formatLine("  shim:%-34s %10.3f ms  %5.1f%%  N=%llu\n", r.name.c_str(), ms(r.ns),
                              pct(r.ns), static_cast<unsigned long long>(r.calls));
            ++printed;
        }
        if (rest_calls != 0) {
            out += formatLine("  shim:(rest)                            %10.3f ms  %5.1f%%  N=%llu\n",
                              ms(rest_ns), pct(rest_ns),
                              static_cast<unsigned long long>(rest_calls));
        }
    }
    out += formatLine("partial     : %10.3f ms  %5.1f%%  exact\n", ms(t.partial), pct(t.partial));
    out += formatLine("instrument  : %10.3f ms  %5.1f%%  （%.1f ns/取时 × %llu 次）\n",
                      ms(t.instrument), pct(t.instrument), w.clock_read_ns,
                      static_cast<unsigned long long>(w.sampled * 3 + w.shim_calls * 2));
    // ★ 作废判据的三种形态都必须让文本里有 `INVALID`（需求单设计第 1 条的边界；"仍复制"是切片 3）：
    //   sampled==0 / coverage 出 [0.5, 2] / unaccounted 超 3%。正常读数仍是旧的 `ok(≤3%)` 逐字不变。
    const char* unaccounted_why = "ok(≤3%)";
    if (w.sampled == 0) {
        unaccounted_why = "INVALID: sampled==0（窗口内没有采到样）⇒ 本次读数作废";
    } else if (t.suspect) {
        unaccounted_why = "INVALID: 五项之和偏离墙钟 >15% ⇒ 本次读数作废（见 coverage）";
    } else if (!t.valid()) {
        unaccounted_why = "INVALID: unaccounted 超 3% ⇒ 本次读数作废";
    }
    out += formatLine("unaccounted : %10.3f ms  %5.1f%%  %s\n", ms(t.unaccounted),
                      t.wall > 0.0 ? (t.unaccounted / t.wall) * 100.0 : 0.0, unaccounted_why);
    out += formatLine("coverage    : %.2f×  （五项之和 / 墙钟；1.00 = 外推与墙钟吻合）\n", t.coverage);
    // `lift_dyn`：只报次数（尾调用 ⇒ 区间含目标体，⛔ 不进闭式）
    out += formatLine("lift_dyn    : %llu 次（计数精确；⛔ 不进闭式：尾调用 ⇒ 区间含目标体）\n",
                      static_cast<unsigned long long>(w.liftdyn_close - w.liftdyn_open));
    // ★ 结论行（写死的单口径，§5）：最高**具名行** ≥10% 才是优化候选。
    {
        double best_shim_ns = 0.0;
        std::string best_name = "(none)";
        for (std::size_t id = 0; id < w.per_shim_ns.size(); ++id) {
            if (w.per_shim_calls[id] == 0) continue;
            const double v = static_cast<double>(w.per_shim_ns[id]);
            if (v > best_shim_ns) {
                best_shim_ns = v;
                best_name = nameOf(static_cast<std::uint32_t>(id));
            }
        }
        // `boundary` 也是具名行；`lift` 聚合**不进候选**（M-12 §5.2）
        const bool boundary_wins = t.boundary > best_shim_ns;
        const double best = boundary_wins ? t.boundary : best_shim_ns;
        const double share = t.wall > 0.0 ? best / t.wall : 0.0;
        const AotWindowProf::Rank rank = AotWindowProf::rankOf(share);
        const char* verdict = rank == AotWindowProf::Rank::Candidate
                                  ? "候选（≥10%）"
                                  : rank == AotWindowProf::Rank::Watch ? "观察名单（3–10%，需两次复现）"
                                                                       : "噪声（<3%）";
        out += formatLine("verdict     : 最高具名行 = %s %.1f%% ⇒ %s%s\n",
                          boundary_wins ? "boundary" : best_name.c_str(), share * 100.0, verdict,
                          rank == AotWindowProf::Rank::Candidate ? "" : "（无单点值得优化 ⇒ 记「均匀分布」）");
    }
    return out;
}

}  // namespace lift::aot
