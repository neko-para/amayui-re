// emulator-lift · Phase-0 令牌读数的唯一实现（解释器 `Machine` 与 AOT `AotHost` 共用）
//
// 口径（细节见 `guest_token.h` 的 Phase-0 那一组 —— ⛔ 这里不复述第二份）：
//   · `hold` = 持令牌的累计墙钟；`span` = 本轮**首个持有 → 最后一次释放**；
//     `duty` = hold / span ⇒ 需求单第 1 项"持有占空"。⛔ 分母是**令牌观测窗口**，
//     不是 `run()` 的墙钟（后者在 `--profile` 的窗口表里；两者差 boot 前后那一点点）。
//   · `max` = **最长一次不间断持有**（`Yield` 会把一段切成两段）⇒ 回答"一次交接最坏等多久"。
//   · 五桶 = <1 / 1–5 / 5–20 / 20–50 / >50 **ms**（宿主墙钟）。
//   · 角色 0 = 调 `arm()` 的那条线程（`Machine::run` / `AotHost::run` 的主循环），
//          1 = 其它执行流（音频 feeder / 夹具线程）。
//   · `shimmax` = `ShimRegistry::invoke` 记的**最长一次** shim 实现自身墙钟（不是累计；累计见
//     `--profile` 的 shims 表）。
#include "runtime/token_phase0_report.h"

#include <algorithm>
#include <cstdint>
#include <string>

#include "runtime/guest_token.h"
#include "runtime/shims.h"

namespace lift {

void dumpTokenPhase0(std::FILE* out, const std::deque<ShimDef>& shims) {
    if (out == nullptr) return;
    const GuestToken::Phase0Readings mainr = guestToken().phase0Readings(GuestToken::kPhase0RoleMain);
    const GuestToken::Phase0Readings other = guestToken().phase0Readings(GuestToken::kPhase0RoleOther);
    const std::uint64_t span_ns = guestToken().phase0SpanNs();
    const double duty =
        span_ns != 0 ? 100.0 * static_cast<double>(mainr.hold_ns) / static_cast<double>(span_ns) : 0.0;
    std::fprintf(out,
                 "tokenhold : main hold %.1f ms / span %.1f ms = %.1f%% max %.3f ms segs %llu | "
                 "other hold %.1f ms max %.3f ms segs %llu\n",
                 static_cast<double>(mainr.hold_ns) / 1e6, static_cast<double>(span_ns) / 1e6, duty,
                 static_cast<double>(mainr.hold_max_ns) / 1e6, (unsigned long long)mainr.hold_count,
                 static_cast<double>(other.hold_ns) / 1e6, static_cast<double>(other.hold_max_ns) / 1e6,
                 (unsigned long long)other.hold_count);
    std::fprintf(out, "holdhist  : main <1ms %llu | 1-5ms %llu | 5-20ms %llu | 20-50ms %llu | >50ms %llu\n",
                 (unsigned long long)mainr.hold_hist[0], (unsigned long long)mainr.hold_hist[1],
                 (unsigned long long)mainr.hold_hist[2], (unsigned long long)mainr.hold_hist[3],
                 (unsigned long long)mainr.hold_hist[4]);
    std::fprintf(out,
                 "waithist  : main <1ms %llu | 1-5ms %llu | 5-20ms %llu | 20-50ms %llu | >50ms %llu | "
                 "other <1ms %llu | 1-5ms %llu | 5-20ms %llu | 20-50ms %llu | >50ms %llu\n",
                 (unsigned long long)mainr.wait_hist[0], (unsigned long long)mainr.wait_hist[1],
                 (unsigned long long)mainr.wait_hist[2], (unsigned long long)mainr.wait_hist[3],
                 (unsigned long long)mainr.wait_hist[4], (unsigned long long)other.wait_hist[0],
                 (unsigned long long)other.wait_hist[1], (unsigned long long)other.wait_hist[2],
                 (unsigned long long)other.wait_hist[3], (unsigned long long)other.wait_hist[4]);
    // 第 4 项：每个 shim 的**最长一次**宿主墙钟（点出"哪一类 shim 是长活"）。
    // 排序与 `--profile` 的 shim 表同口径（max desc → calls desc → module asc → name asc），
    // 只列有读数的项（没被调用过的是 0，没有解释价值）。
    struct ShimMaxRow {
        const ShimDef* def = nullptr;
        std::uint64_t max_ns = 0;
        std::uint64_t calls = 0;
    };
    std::vector<ShimMaxRow> rows;
    rows.reserve(shims.size());
    for (const ShimDef& d : shims) {
        const std::uint64_t mx = guestToken().shimMaxNs(d.id);
        if (mx == 0) continue;
        rows.push_back(ShimMaxRow{&d, mx, guestToken().shimMaxCalls(d.id)});
    }
    std::sort(rows.begin(), rows.end(), [](const ShimMaxRow& a, const ShimMaxRow& b) {
        if (a.max_ns != b.max_ns) return a.max_ns > b.max_ns;
        if (a.calls != b.calls) return a.calls > b.calls;
        if (a.def->module != b.def->module) return a.def->module < b.def->module;
        return a.def->name < b.def->name;
    });
    constexpr std::size_t kShimMaxTop = 8;
    const std::size_t shown = rows.size() < kShimMaxTop ? rows.size() : kShimMaxTop;
    std::fprintf(out, "shimmax   : top %zu / %zu shims by max_ns（shim 实现自身；累计见 --profile 的 shims 表）\n",
                 shown, rows.size());
    for (std::size_t i = 0; i < shown; ++i) {
        const ShimMaxRow& row = rows[i];
        const std::string label = row.def->module + "!" + row.def->name;
        std::fprintf(out, "            %2zu. %-46s max %9.3f ms calls=%llu\n", i + 1, label.c_str(),
                     static_cast<double>(row.max_ns) / 1e6, (unsigned long long)row.calls);
    }
    const std::uint64_t overflow = guestToken().shimMaxOverflow();
    if (overflow != 0) {
        // ⛔ 不静默丢：超出记账表容量的 id 在这里点名（正常工况恒 0）。
        std::fprintf(out, "            overflow %llu（shim id 超出记账表容量 ⇒ 这几条没记到 max）\n",
                     (unsigned long long)overflow);
    }
}

}  // namespace lift
