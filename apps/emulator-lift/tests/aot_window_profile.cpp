// emulator-lift · **AOT 窗口 profile**的 pure 守卫（顶层无条件注册 ⇒ pure 档）
//
// ★ 守的四件事（每条都钉在"坏了会有什么后果"上）：
//   ① **闭式**：`wall == lift + boundary + shim + partial + instrument + unaccounted`。
//      红了意味着：某个桶被漏加 / 被重复加 ⇒ 表里那几行百分比互相矛盾，
//      读者会据此挑错优化对象（比"没有 profile"更糟）。
//   ② **份额来自采样、绝对量来自包络**：采样桶按 `iter/sampled` 外推后只用于定 `lift:boundary`
//      的**比值**；绝对量取精确包络 `wall − shim − partial − instrument`。
//      红了意味着：直接把采样均值当绝对时间 —— 区间里的取时税会被外推 64×（实测 1.77 s vs 墙钟 1.62 s）。
//   ③ **作废阈看 `coverage`**（= 采样外推 / 精确包络）：`∉ [0.5, 2]` ⇒ `suspect` ⇒ `valid() == false`。
//      `unaccounted` 是**定义式残差**、归一后结构上恒 ~0：它照打，但**不是**作废判据。
//      红了意味着：覆盖率再离谱也不作废 ⇒ 容差成了摆设（"假 profile"）。
//   ④ **没开窗 = 什么都没有**：`reset()` 之后所有桶为 0、`totals().wall == 0`；`rankOf` 的三档阈值
//      写死在结构体上（⛔ 不许各处各写一套）。
// ★ 不碰 GPU / 不起进程 / 不读 LFS / 不取时：纯算术夹具（时钟成本记为 0 ⇒ 闭式只考桶本身）。
#include <cmath>
#include <cstdint>
#include <cstdio>

#include "aot/window_prof.h"
#include "check.h"

namespace {

using lift::aot::AotWindowProf;

constexpr double kEps = 1.0;  // ns 级容差（夹具里全是整数 ns）

// ⚠ 不要叫 `near`：Windows 头把 `near` 定义成空宏（legacy calling convention）⇒ 调用会被
//   静默改写成逗号表达式（实测：编译过、链接时说没有 main）。
bool approx(double a, double b, double eps = kEps) { return std::fabs(a - b) <= eps; }

void openAt(AotWindowProf& w, std::uint64_t t0_ns) {
    w.reset();
    w.on = true;
    w.t_open_ns = t0_ns;
    w.clock_read_ns = 0.0;  // 夹具不模拟仪器成本 ⇒ 闭式只考桶
}

}  // namespace

int main() {
    // ★ 注册名 = 用例名（§12.4 的守卫：没有 `--flag` 的 `add_test` 必须与源文件里的用例名对得上）。
    //   下面每个子用例仍各自设 `g_current_case`（红了要点名是哪一条）。
    ::lift_test::g_current_case = "aot-window-profile";

    // ---- ① 精确档（iter == sampled）：五项之和必须等于 wall ----
    {
        ::lift_test::g_current_case = "window-profile-closed-form";
        AotWindowProf w;
        const std::uint64_t t0 = 0;
        const std::uint64_t wall_ns = 953'000'000ull;  // 953 ms
        openAt(w, t0);
        w.iter = 1000;
        w.sampled = 1000;
        // 一个窗口里的真实形状：boundary（块顶 → 分派前）→ 要么 shim、要么 lift。
        // boundary 100 ms / shim 50 ms / lift 750 ms ⇒ 加起来 900 ms（⛔ shim 与 lift 不重叠）
        w.noteBoundary(t0, t0 + 100'000'000ull);
        w.noteShim(0u, 50'000'000ull);
        w.noteLift(t0 + 150'000'000ull, t0 + 900'000'000ull);
        // 收窗：t_close 比 last_c 晚 53 ms ⇒ 这 53 ms 必须落进 `partial`（边沿 sliver），
        // ⛔ 不许静默算进 `unaccounted`（那会把一个精确量当成缺口）。
        w.closeEdges(t0 + wall_ns);
        const AotWindowProf::Totals t = w.totals();
        LIFT_CHECK(approx(t.wall, static_cast<double>(wall_ns)));
        LIFT_CHECK(approx(t.boundary, 100'000'000.0));
        LIFT_CHECK(approx(t.lift, 750'000'000.0));
        LIFT_CHECK(approx(t.shim, 50'000'000.0));
        LIFT_CHECK(approx(t.partial, 53'000'000.0));
        LIFT_CHECK(approx(t.instrument, 0.0));
        // 闭式：五项 + 残差 == wall（这条就是"表里的百分比互相成立"）
        LIFT_CHECK(approx(t.lift + t.boundary + t.shim + t.partial + t.instrument + t.unaccounted, t.wall));
        LIFT_CHECK(approx(t.unaccounted, 0.0));
        LIFT_CHECK(t.valid());
    }

    // ---- ② 采样档：`iter/sampled` 外推（1/64 采样不能把读数当成总量）----
    {
        ::lift_test::g_current_case = "window-profile-extrapolates-samples";
        AotWindowProf w;
        const std::uint64_t t0 = 0;
        openAt(w, t0);
        w.iter = 6400;    // 窗口内真实派发迭代数
        w.sampled = 100;  // 每 64 个采一次 ⇒ 100 个样本
        // 每个被采样的迭代：boundary 1 ms + lift 8 ms；样本落在第 0、64、128… 个迭代上
        // （迭代周期 10 ms ⇒ 样本间隔 640 ms）。
        for (int i = 0; i < 100; ++i) {
            const std::uint64_t a = t0 + static_cast<std::uint64_t>(i) * 640'000'000ull;
            w.noteBoundary(a, a + 1'000'000ull);
            w.noteLift(a + 1'000'000ull, a + 9'000'000ull);
        }
        // wall = 6400 × 10 ms = 64 s ⇒ t_close 比最后一个样本的终点晚 631 ms（边沿 sliver）
        w.closeEdges(t0 + 64'000'000'000ull);
        const AotWindowProf::Totals t = w.totals();
        // ★ 采样只决定 `lift:boundary` 的**比值**；绝对量按精确包络归一：
        //   envelope = wall − shim − partial − instrument = 64 − 0 − 0.631 s
        //   比值 = 51.2 : 6.4 = 8 : 1 ⇒ lift = 63.369 × 8/9、boundary = 63.369 × 1/9
        LIFT_CHECK(approx(t.lift, 63'369'000'000.0 * 8.0 / 9.0, 1000.0));
        LIFT_CHECK(approx(t.boundary, 63'369'000'000.0 / 9.0, 1000.0));
        LIFT_CHECK(approx(t.lift / t.boundary, 8.0, 1e-6));
        LIFT_CHECK(approx(t.wall, 64'000'000'000.0, 1000.0));
        LIFT_CHECK(approx(t.partial, 631'000'000.0, 1000.0));
        // 归一之后闭式恒成立（残差只剩浮点噪声）⇒ 这就是"表里的百分比互相成立"
        LIFT_CHECK(approx(t.lift + t.boundary + t.shim + t.partial + t.instrument + t.unaccounted, t.wall,
                          1000.0));
        LIFT_CHECK(t.unaccounted == 0.0);
        // `coverage` = 采样外推 / 包络 = 57.6 / 63.369 = 0.909 ⇒ 诊断行，不作判据
        LIFT_CHECK(approx(t.coverage, 57'600'000'000.0 / 63'369'000'000.0, 1e-6));
        LIFT_CHECK(!t.suspect);
    }

    // ---- ③ 作废阈：残差 > 3% ⇒ 本次读数不可用 ----
    // ★ 夹具形状：1/64 采样但**每个样本只走了窗口的一小截**（= 仪器漏账的真实后果）。
    //   精确档（iter == sampled）在结构上不可能有残差（边沿 sliver 精确补上）⇒ 作废档必须用采样档。
    {
        ::lift_test::g_current_case = "window-profile-invalid-over-3pct";
        AotWindowProf w;
        const std::uint64_t t0 = 0;
        openAt(w, t0);
        w.iter = 6400;
        w.sampled = 100;
        for (int i = 0; i < 100; ++i) {
            const std::uint64_t a = t0 + static_cast<std::uint64_t>(i) * 640'000'000ull;
            w.noteBoundary(a, a + 1'000'000ull);
            w.noteLift(a + 1'000'000ull, a + 2'000'000ull);  // 每个样本只记 2 ms
        }
        w.closeEdges(t0 + 64'000'000'000ull);  // wall = 64 s，样本外推只有 12.8 s
        const AotWindowProf::Totals t = w.totals();
        // 外推只有包络的 20% ⇒ `coverage` 离谱（<0.5）⇒ 连份额都救不回来 ⇒ 作废
        LIFT_CHECK(t.coverage < 0.5);
        LIFT_CHECK(t.suspect);
        LIFT_CHECK(!t.valid());  // ⇒ 本次读数作废
    }

    // ---- ④ 没开窗 = 什么都没有；三档阈值只有一份 ----
    {
        ::lift_test::g_current_case = "window-profile-off-is-empty";
        AotWindowProf w;
        const AotWindowProf::Totals t = w.totals();
        LIFT_CHECK(!w.on);
        LIFT_CHECK(t.wall == 0.0);
        LIFT_CHECK(t.lift == 0.0 && t.boundary == 0.0 && t.shim == 0.0 && t.partial == 0.0);
        LIFT_CHECK(t.unaccounted == 0.0);
        LIFT_CHECK(t.valid());  // 0/0 不算作废（没有窗口就没有读数，但也不是"坏读数"）
        // 三档：≥10% 候选 / 3–10% 观察 / <3% 噪声（⛔ 不许在调用点另写一套数字）
        LIFT_CHECK(AotWindowProf::rankOf(0.10) == AotWindowProf::Rank::Candidate);
        LIFT_CHECK(AotWindowProf::rankOf(0.099) == AotWindowProf::Rank::Watch);
        LIFT_CHECK(AotWindowProf::rankOf(0.03) == AotWindowProf::Rank::Watch);
        LIFT_CHECK(AotWindowProf::rankOf(0.029) == AotWindowProf::Rank::Noise);
        // 采样是**乘性散列**（平均 1/64），⛔ 不许写 `iter % 64`：那会与 guest 循环周期对齐
        // （实测：FELLOW init 里恒定采到同相位 ⇒ 样本均值高 1.6× ⇒ 桶之和超过墙钟）。
        int due = 0;
        for (std::uint64_t i = 0; i < 64000; ++i) {
            w.iter = i;
            if (w.sampleDue()) ++due;
        }
        LIFT_CHECK(due > 700 && due < 1300);  // 期望 1000（±30%）
    }

    // ---- ⑥ 两个边沿：开窗那一刀落在 boundary 段里 ⇒ 必须夹到窗口起点；桶超额要报出来 ----
    {
        ::lift_test::g_current_case = "window-profile-edge-clamp-and-overshoot";
        AotWindowProf w;
        openAt(w, 1000);  // t_open = 1000
        w.iter = 64;
        w.sampled = 1;
        // 本迭代的 boundary 从 500 开始（开窗发生在它中间）⇒ 只许计 1000→2000
        w.noteBoundary(500, 2000);
        LIFT_CHECK(w.boundary_ns == 1000);
        LIFT_CHECK(w.first_a_ns == 1000);
        // 整段都在窗口之前 ⇒ 不计
        w.noteBoundary(100, 200);
        LIFT_CHECK(w.boundary_ns == 1000);
        // 桶外推远大于包络（外推偏高）⇒ 归一、`suspect` 置位（coverage 128× ⇒ 离谱）
        w.lift_ns = 100'000'000;  // 外推后 ×64 = 6.4 s
        w.noteLift(2000, 49'000'000);  // 最后一个被计时区间到 49 ms ⇒ 边沿 sliver 只有 1 ms
        w.closeEdges(50'000'000);      // wall ≈ 50 ms
        const AotWindowProf::Totals t = w.totals();
        LIFT_CHECK(t.suspect);
        LIFT_CHECK(t.unaccounted == 0.0);
        LIFT_CHECK(t.coverage > 2.0);
    }

    // ---- ⑤ present 子桶：累加而不是覆盖（一次 shim 里多次 Present 也要计数）----
    {
        ::lift_test::g_current_case = "window-profile-present-subtally";
        AotWindowProf w;
        openAt(w, 0);
        lift::PresentBreakdown pb;
        pb.readback_ns = 1'000'000;
        pb.hash_ns = 200'000;
        pb.sink_ns = 300'000;
        w.notePresent(pb);
        w.notePresent(pb, 2);  // 一次 shim 里 2 帧（拆分只有最后一帧 ⇒ 只增计数）
        LIFT_CHECK(w.present_calls == 3);
        LIFT_CHECK(w.present_readback_ns == 2'000'000);
        LIFT_CHECK(w.present_hash_ns == 400'000);
        LIFT_CHECK(w.present_sink_ns == 600'000);
    }

    std::printf("[aot-window-profile] %d checks, %d failures\n", ::lift_test::g_checks,
                ::lift_test::g_failures);
    return ::lift_test::g_failures == 0 ? 0 : 1;
}
