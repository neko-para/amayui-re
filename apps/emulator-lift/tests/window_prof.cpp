// emulator-lift · **AOT 窗口报告文本** + **窗口状态位**的 pure 守卫（顶层无条件注册 ⇒ pure 档）
//
// 判据（每条都钉在"坏了会有什么后果"上）：
//   ① `aot-window-report-text/golden`：`formatWindowReportText()` 的输出与**旧 `endWindow()` 的 printf
//      块**在同一份夹具上产出的金样**逐字节相同**（金样在 `tests/` 里是字面量，不是"当前实现的输出"）。
//      红了意味着：抽函数改变了 stdout ⇒ 已有存档 / 剪贴板文本（切片 3）失去可比性。
//   ② `aot-window-report-text/sections`：每个段落（wall / lift / boundary / shim / partial / instrument /
//      unaccounted / coverage / lift_dyn / verdict + 具名行）都出现。红了意味着：某一段被静默吞掉。
//   ③ `aot-window-report-text/top-n-rest`：`top=8` ⇒ 最多 8 条具名行 + 一条 `(rest)`（余下合并）。
//      红了意味着：具名行无界（一张表变成几十行），或者 `(rest)` 不再代表"没打出来的那些"。
//   ④ `aot-window-report-text/invalid`：`sampled == 0` / `coverage` 出 [0.5, 2] ⇒ 文本含 `INVALID`
//      （需求单已裁决"短窗口仍复制"）。红了意味着：一份作废的读数会被当成有效结论复制出去。
//   ⑤ `aot-window-report-text/line-budget`：任何形状（0/8/12 条具名行、有/无 present）总行数 ≤ 20。
//   ⑥ `aot-window-state`：`beginWindow()` 后 `profileOpen()` 为真、重复 `beginWindow()` 返回 false
//      且**不 reset**（⛔ 不静默重开）、`closeWindow()` 幂等。红了意味着：重复 `profile_begin` 会把上一
//      段的账静默抹掉（用户看到的是"第二段覆盖第一段"，没有任何信号）。
// ★ 不碰 GPU / 不起进程 / 不读资产 / 不取时：纯算术夹具 + 纯文本比较。
#include <cstdio>
#include <cstring>
#include <string>

#include "aot/window_prof.h"
#include "check.h"

namespace {

using lift::aot::AotWindowProf;
using lift::aot::formatWindowReportText;

// 旧 `endWindow()` 里 `shims_.byId(id)` 的替身（名字表由夹具注入；`aot-window-report-text` 不碰 registry）。
std::string shimName(std::uint32_t id) {
    switch (id) {
        case 0: return "D3D9!Present";
        case 1: return "user32!PeekMessageA";
        case 2: return "winmm!timeGetTime";
        case 3: return "gdi32!DeleteObject";
        default: return "(unknown)";
    }
}

bool contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}

int countLines(const std::string& text) {
    int n = 0;
    for (char c : text) {
        if (c == '\n') ++n;
    }
    return n;
}

// 具名 shim 行的条数（⛔ 不数 `(rest)`：那不是具名行）。
int countNamedShimRows(const std::string& text) {
    int n = 0;
    std::size_t pos = 0;
    while ((pos = text.find("\n  shim:", pos)) != std::string::npos) {
        ++pos;  // 跳过句首换行，下一次从 "  shim:" 里继续找
        if (text.compare(pos, 13, "  shim:(rest)") != 0) ++n;
    }
    return n;
}

// ---- 金样夹具（与旧 printf 取证程序 `.tmp/slice1/golden.cpp` 的字段**逐项相同**）----
AotWindowProf makeGolden() {
    AotWindowProf w;
    w.on = true;
    w.label = "golden";
    w.t_open_ns = 0;
    w.t_close_ns = 10'000'000;  // wall = 10.000 ms
    w.insns_open = 1000;
    w.insns_close = 1000 + 123456;
    w.frames_open = 3;
    w.frames_close = 5;
    w.liftdyn_open = 7;
    w.liftdyn_close = 9;
    w.iter = 640;
    w.sampled = 640;            // iter == sampled ⇒ 不外推
    w.clock_read_ns = 1000.0;   // instrument = (640*2 + 35*2) * 1000 = 1.35 ms
    w.lift_ns = 3'000'000;
    w.boundary_ns = 1'740'000;
    w.shim_ns = 3'510'000;
    w.shim_calls = 35;
    w.partial_ns = 400'000;
    w.per_shim_ns = {2'000'000, 1'000'000, 500'000, 10'000};
    w.per_shim_calls = {4, 10, 20, 1};
    return w;
}

// ★ 金样 = **旧 `endWindow()` 的 printf 块**（`.tmp/slice1/golden.cpp`，逐字复制旧实现）在同一份
//   `makeGolden()` 夹具上的输出。⛔ 不要用 `formatWindowReportText()` 重新生成它 —— 那就成了自我证明。
const char* kGolden =
R"GOLDEN(=== profile window: golden ===
wall        :     10.000 ms   iter=640 sampled=640（stride 64，⚠ 窗口短 ⇒ 样本少）insns=123456 frames=2
lift        :      3.000 ms   30.0%  est
boundary    :      1.740 ms   17.4%  est
shim        :      3.510 ms   35.1%  exact(N=35)
  shim:D3D9!Present                            2.000 ms   20.0%  N=4
  shim:user32!PeekMessageA                     1.000 ms   10.0%  N=10
  shim:winmm!timeGetTime                       0.500 ms    5.0%  N=20
  shim:(rest)                                 0.010 ms    0.1%  N=1
partial     :      0.400 ms    4.0%  exact
instrument  :      1.350 ms   13.5%  （1000.0 ns/取时 × 1990 次）
unaccounted :      0.000 ms    0.0%  ok(≤3%)
coverage    : 1.00×  （五项之和 / 墙钟；1.00 = 外推与墙钟吻合）
lift_dyn    : 2 次（计数精确；⛔ 不进闭式：尾调用 ⇒ 区间含目标体）
verdict     : 最高具名行 = D3D9!Present 20.0% ⇒ 候选（≥10%）
)GOLDEN";

// 12 条都 ≥0.5% 的具名行（Top-N 预算用；`present_calls == 0` ⇒ 11 个固定行 + 8 具名 + (rest) = 20）。
AotWindowProf makeManyShims() {
    AotWindowProf w;
    w.on = true;
    w.label = "many";
    w.t_open_ns = 0;
    w.t_close_ns = 1'000'000'000;  // wall = 1 s
    w.iter = 1000;
    w.sampled = 1000;
    w.clock_read_ns = 0.0;
    w.lift_ns = 300'000'000;   // covered = 1000 条样本 × (300k + 100k) = 400M
    w.boundary_ns = 100'000'000;
    w.shim_ns = 600'000'000;
    w.shim_calls = 12;
    for (int i = 0; i < 12; ++i) {
        w.per_shim_ns.push_back(50'000'000);
        w.per_shim_calls.push_back(1);
    }
    return w;
}

void runReportText() {
    // ---- ① 金样逐字节相等（= "stdout 未变"的机械证据）----
    {
        ::lift_test::g_current_case = "aot-window-report-text/golden";
        const std::string text = formatWindowReportText(makeGolden(), &shimName);
        if (text != kGolden) {
            std::fprintf(stderr, "--- actual (%zu bytes) ---\n%s--- golden (%zu bytes) ---\n%s",
                         text.size(), text.c_str(), std::strlen(kGolden), kGolden);
        }
        LIFT_CHECK(text == kGolden);
        LIFT_CHECK(countLines(text) == 15);
    }

    // ---- ② 段落闭合：每一段都在（缺一段就意味着"表里少了一行"，读者会读错）----
    {
        ::lift_test::g_current_case = "aot-window-report-text/sections";
        const std::string text = formatWindowReportText(makeGolden(), &shimName);
        const char* const sections[] = {
            "=== profile window: golden ===", "\nwall        :",     "\nlift        :",
            "\nboundary    :",                "\nshim        :",     "\npartial     :",
            "\ninstrument  :",                "\nunaccounted :",     "\ncoverage    :",
            "\nlift_dyn    :",                "\nverdict     :",     "\n  shim:D3D9!Present",
            "\n  shim:(rest)",
        };
        for (const char* s : sections) {
            if (!contains(text, s)) std::fprintf(stderr, "缺段落: %s\n", s);
            LIFT_CHECK(contains(text, s));
        }
    }

    // ---- ③ Top-N + (rest)：top=8 时**恰好** 8 条具名 + 1 条 (rest)；top=2 时恰好 2 + 1 ----
    {
        ::lift_test::g_current_case = "aot-window-report-text/top-n-rest";
        const std::string top8 = formatWindowReportText(makeManyShims(), &shimName);
        LIFT_CHECK(countNamedShimRows(top8) == 8);
        LIFT_CHECK(contains(top8, "\n  shim:(rest)"));
        LIFT_CHECK(countLines(top8) <= 20);  // 20 是 spec 上限（硬不变量），不是"当前取值"
        const std::string top2 = formatWindowReportText(makeManyShims(), &shimName, 2);
        LIFT_CHECK(countNamedShimRows(top2) == 2);
        LIFT_CHECK(contains(top2, "\n  shim:(rest)"));
    }

    // ---- ④ INVALID：sampled==0 / coverage 出 [0.5,2] 时文本必须含 INVALID ----
    {
        ::lift_test::g_current_case = "aot-window-report-text/invalid-sampled-zero";
        AotWindowProf w;
        w.on = true;
        w.label = "short";
        w.t_open_ns = 0;
        w.t_close_ns = 5'000'000;  // 5 ms，但一次都没采到样
        w.iter = 100;
        w.sampled = 0;
        w.shim_ns = 1'000'000;
        w.shim_calls = 1;
        w.per_shim_ns = {1'000'000};
        w.per_shim_calls = {1};
        const std::string text = formatWindowReportText(w, &shimName);
        LIFT_CHECK(contains(text, "INVALID"));
        LIFT_CHECK(countLines(text) <= 20);
    }
    {
        ::lift_test::g_current_case = "aot-window-report-text/invalid-coverage";
        AotWindowProf w;
        w.on = true;
        w.label = "bad-coverage";
        w.t_open_ns = 0;
        w.t_close_ns = 5'000'000;  // 5 ms
        w.iter = 6400;
        w.sampled = 100;  // 外推 64×，但样本只覆盖 2 ms ⇒ coverage < 0.5
        w.lift_ns = 1'000;
        w.boundary_ns = 1'000;
        const std::string text = formatWindowReportText(w, &shimName);
        LIFT_CHECK(contains(text, "INVALID"));
        LIFT_CHECK(countLines(text) <= 20);
    }
    {
        ::lift_test::g_current_case = "aot-window-report-text/invalid-empty";
        AotWindowProf w;  // 没开窗（全 0）也走 sampled==0 ⇒ 文本里仍有 INVALID
        const std::string text = formatWindowReportText(w, &shimName);
        LIFT_CHECK(contains(text, "INVALID"));
        LIFT_CHECK(countLines(text) <= 20);
    }

    // ---- ⑤ 行数上限：有 present 子行时预算会先减具名行（⛔ 不许靠"少打一段"省行）----
    {
        ::lift_test::g_current_case = "aot-window-report-text/line-budget-present";
        AotWindowProf w = makeManyShims();
        w.present_calls = 3;
        w.present_readback_ns = 1'000'000;
        w.present_sink_ns = 2'000'000;
        const std::string text = formatWindowReportText(w, &shimName);
        LIFT_CHECK(contains(text, "\n  present   :"));
        LIFT_CHECK(countLines(text) <= 20);
        LIFT_CHECK(countNamedShimRows(text) == 7);  // 12 固定 + 7 具名 + (rest) = 20
        LIFT_CHECK(contains(text, "\n  shim:(rest)"));
        // 具名行上限不许被"预算"放大：top=8 仍是上界。
        LIFT_CHECK(countNamedShimRows(text) <= 8);
    }
    {
        ::lift_test::g_current_case = "aot-window-report-text/line-budget-small";
        LIFT_CHECK(countLines(formatWindowReportText(AotWindowProf{}, &shimName)) <= 20);
        LIFT_CHECK(countLines(formatWindowReportText(makeGolden(), &shimName, 0)) <= 20);
    }
}

void runState() {
    // ① 未开 ⇒ false；开 ⇒ true + `profileOpen()` 为真
    ::lift_test::g_current_case = "aot-window-state/open";
    AotWindowProf w;
    LIFT_CHECK(!w.profileOpen());
    LIFT_CHECK(w.beginWindow());
    LIFT_CHECK(w.profileOpen());

    // ② 重复开：返回 false 且**一个字节都不动**（⛔ 不静默 reset）
    ::lift_test::g_current_case = "aot-window-state/reopen-refused";
    w.iter = 123;
    w.sampled = 9;
    w.label = "first";
    LIFT_CHECK(!w.beginWindow());
    LIFT_CHECK(w.profileOpen());
    LIFT_CHECK(w.iter == 123);
    LIFT_CHECK(w.sampled == 9);
    LIFT_CHECK(w.label == "first");

    // ③ 关窗幂等：第一次 true、第二次 false
    ::lift_test::g_current_case = "aot-window-state/close-idempotent";
    LIFT_CHECK(w.closeWindow());
    LIFT_CHECK(!w.profileOpen());
    LIFT_CHECK(!w.closeWindow());

    // ④ 重开（已关）⇒ 清掉上一轮的账
    ::lift_test::g_current_case = "aot-window-state/reopen-clears";
    LIFT_CHECK(w.beginWindow());
    LIFT_CHECK(w.iter == 0);
    LIFT_CHECK(w.sampled == 0);
    LIFT_CHECK(w.profileOpen());
}

}  // namespace

int main(int argc, char** argv) {
    // 两个 CTest 共用一个可执行文件：无参 = 报告文本；第二个实参 `state` = 状态位（都是 pure）。
    if (argc > 1 && std::strcmp(argv[1], "state") == 0) {
        ::lift_test::g_current_case = "aot-window-state";
        runState();
        return ::lift_test::finish("aot-window-state");
    }
    ::lift_test::g_current_case = "aot-window-report-text";
    runReportText();
    return ::lift_test::finish("aot-window-report-text");
}
