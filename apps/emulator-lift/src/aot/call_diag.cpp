// emulator-lift · 库函数候选的 per-function 动态计数（临时诊断；开关 `LIFT_CALL_DIAG=1`）
//
// ★ 只做计数：读派发 EA / `cpu_.insns` 前后差 / 入口栈上的 cdecl 实参，外加 S7 宿主调用路径
//   （`lift_dyn`）的一次 EA 命中。⛔ 不写 guest 状态、⛔ 不改任何既有计数器、⛔ 不影响派发表。
//
// ★ 候选表（`REQ-01M86VSP8CXH998HTJTD50MEGC` 的 M1）：EA / 名字 / 字节数都来自 `.lst` 的
//   `COLLAPSED FUNCTION` 声明（`.tmp/reveq/intrinsics/candidates.csv` 同一份来源）。
//   这里是**测量面**：它不决定谁进 `aot/intrinsics.txt`。
//
// ★ 两条调用路径（2026-11-25 实测：这是本诊断最重要的一条口径）：
//   ① **派发（trampoline）**：`aot_run.cpp` 的派发循环 ⇒ `calls`（+ `dispatches` / `insns` 归因）。
//   ② **S7 宿主调用**：`call` 站点发成 `call @lift_dyn` + return-check（`lift_abi.cpp`），
//      由 `aot_dispatch.cpp` 的 `lookupUncounted(ea)` 查表 ⇒ **不回派发循环**。
//      ⇒ `host_calls`（只有次数，没有 insns）。
//   实测（整跑窗口，SAVE11→FIELD→20 s→click→2.5 s）：`_memcpy` 派发可见 50,099 次，同一次 run 的
//   `intrinsics: fast+decline` = 26,721,505 ⇒ **99.81% 的调用只在 ② 上**。⛔ 所以"派发份额"
//   既不是时间份额、也不是调用总数；两个计数分开打，总和才是 calls。
//
// ★ 输出形状（一次性；缺省档一行都没有）：
//     call-diag: window "seg2-click2p5"（LIFT_CALL_DIAG=1；只读，不改 guest）
//     call-diag:   window dispatches=<N> insns=<N>
//     call-diag: _memcpy calls=<N>
//     call-diag:   ea=004E74B0 dispatch=<N> host=<N> dispatches=<N> insns=<N> avg_insns=<f> bytes=<N> avg_n=<f> max_n=<N> n_ok=<N>
#include "aot/call_diag.h"

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace lift {
namespace call_diag {

// ★ 定义在匿名 namespace 之外：头文件用 inline `active()` 读它（派发热路径）。
bool g_active_state = false;

namespace {

// ★ 表：`{ea, end, name, has_n}`。`end` = ea + `.lst` 的 COLLAPSED 字节数（半开区间）。
//   `has_n`：入口处 `[esp+12]` 是长度 —— `_memcpy` / `_memcpy_0`（cdecl dst,src,n）与 `_memset`
//   （cdecl dst,c,n）都是同一个栈槽。
const Candidate kCandidates[] = {
    {0x004E74B0u, 0x004E7811u, "_memcpy", true},      // 0x361 B；已内建（同口径对照）
    {0x004E78E0u, 0x004E7C41u, "_memcpy_0", true},    // 0x361 B（与 0x4E74B0 同体，见报告）
    {0x004E7400u, 0x004E747Au, "_memset", true},      // 0x7A B
    {0x004E82C0u, 0x004E82DCu, "__ftol2_sse", false}, // 0x1C B
    {0x004E8A19u, 0x004E8A37u, "_sprintf_s", false},  // 0x1E B
    {0x004EA11Du, 0x004EA1D2u, "_strncpy_s", false},  // 0xB5 B
    {0x004E7C41u, 0x004E7CA0u, "_strcpy_s", false},   // 0x5F B
    {0x004E8BF1u, 0x004E8C12u, "_rand", false},       // 0x21 B
    {0x004F8434u, 0x004F9906u, "_memcmp", false},     // 0x14D2 B
};
constexpr int kN = static_cast<int>(sizeof(kCandidates) / sizeof(kCandidates[0]));

const bool kOn = [] {
    const char* v = std::getenv("LIFT_CALL_DIAG");
    return v != nullptr && v[0] != '\0';
}();

// `LIFT_CALL_DIAG_WINDOW`：只收集这个 `profile.begin` 窗口；缺省 `*` = 所有窗口。
const char* targetWindow() {
    const char* v = std::getenv("LIFT_CALL_DIAG_WINDOW");
    return (v != nullptr && v[0] != '\0') ? v : "*";
}

struct CandStat {
    std::uint64_t calls = 0;       // 派发到入口 EA 的次数（trampoline 可见）
    std::uint64_t host_calls = 0;  // `lift_dyn` 宿主调用路径到入口 EA 的次数（S7/D3 之后的大头）
    std::uint64_t dispatches = 0;  // 派发落在 [ea,end) 的次数
    std::uint64_t insns = 0;       // 上述派发的 guest 指令数之和（⛔ 不含 host 调用路径）
    std::uint64_t n_ok = 0;        // 入口处实参读出来的次数
    std::uint64_t bytes = 0;
    std::uint64_t max_n = 0;
};

struct Window {
    std::string label;
    std::uint64_t dispatches = 0;  // 落在候选区间内的派发总数
    std::uint64_t insns = 0;
    CandStat cand[kN];
};

Window g_cur;
std::vector<Window> g_done;

}  // namespace

bool on() { return kOn; }

std::size_t candidateCount() { return static_cast<std::size_t>(kN); }

const Candidate& candidate(std::size_t i) { return kCandidates[i]; }

int rangeIndex(std::uint32_t ea) {
    for (int i = 0; i < kN; ++i) {
        if (ea >= kCandidates[i].ea && ea < kCandidates[i].end) return i;
    }
    return -1;
}

void noteDispatch(int idx, std::uint32_t ea, std::uint64_t insns, bool n_ok, std::uint32_t n) {
    if (!g_active_state) return;
    ++g_cur.dispatches;
    g_cur.insns += insns;
    if (idx < 0 || idx >= kN) return;
    CandStat& c = g_cur.cand[idx];
    ++c.dispatches;
    c.insns += insns;
    if (ea != kCandidates[idx].ea) return;  // 同函数内部块 ⇒ 不是一次新的调用
    ++c.calls;
    if (!kCandidates[idx].has_n || !n_ok) return;
    ++c.n_ok;
    c.bytes += n;
    if (n > c.max_n) c.max_n = n;
}

// ★ S7 宿主调用路径（`lift_dyn` → `lookupUncounted`）：只记"到入口 EA 的一次调用"，
//   ⛔ 没有 insns / n（目标是被 tail-call 进去的，返回后控制流已不在本宿主帧）。
void noteHostCall(std::uint32_t ea) {
    if (!g_active_state) return;
    const int idx = rangeIndex(ea);
    if (idx < 0 || ea != kCandidates[idx].ea) return;
    ++g_cur.cand[idx].host_calls;
}

void beginWindow(const char* label) {
    if (!kOn) return;
    const std::string name = (label != nullptr && *label != '\0') ? label : "(unnamed)";
    const char* want = targetWindow();
    if (std::strcmp(want, "*") != 0 && name != want) return;
    g_cur = Window{};
    g_cur.label = name;
    g_active_state = true;
}

void endWindow() {
    if (!g_active_state) return;
    g_done.push_back(std::move(g_cur));
    g_cur = Window{};
    g_active_state = false;
}

void print(std::FILE* out) {
    if (!kOn || out == nullptr) return;
    if (g_active_state) endWindow();
    if (g_done.empty()) {
        std::fprintf(out, "call-diag: on（LIFT_CALL_DIAG=1）但目标窗口 \"%s\" 没有出现\n", targetWindow());
        return;
    }
    for (const Window& w : g_done) {
        std::fprintf(out, "call-diag: window \"%s\"（LIFT_CALL_DIAG=1；只读，不改 guest）\n", w.label.c_str());
        std::fprintf(out, "call-diag:   候选区间内 dispatches=%llu insns=%llu\n",
                     static_cast<unsigned long long>(w.dispatches),
                     static_cast<unsigned long long>(w.insns));
        for (int i = 0; i < kN; ++i) {
            const CandStat& c = w.cand[i];
            // ★ 这一行的前缀是评测单点名的形状：`call-diag: <name> calls=N`（N = dispatch + host）。
            std::fprintf(out, "call-diag: %s calls=%llu\n", kCandidates[i].name,
                         static_cast<unsigned long long>(c.calls + c.host_calls));
            const double avg_insns =
                c.calls != 0 ? static_cast<double>(c.insns) / static_cast<double>(c.calls) : 0.0;
            std::fprintf(out,
                         "call-diag:   ea=%08X dispatch=%llu host=%llu dispatches=%llu insns=%llu "
                         "avg_insns=%.2f bytes=%llu avg_n=%.2f max_n=%llu n_ok=%llu\n",
                         kCandidates[i].ea, static_cast<unsigned long long>(c.calls),
                         static_cast<unsigned long long>(c.host_calls),
                         static_cast<unsigned long long>(c.dispatches),
                         static_cast<unsigned long long>(c.insns), avg_insns,
                         static_cast<unsigned long long>(c.bytes),
                         c.n_ok != 0 ? static_cast<double>(c.bytes) / static_cast<double>(c.n_ok) : 0.0,
                         static_cast<unsigned long long>(c.max_n),
                         static_cast<unsigned long long>(c.n_ok));
        }
    }
}

}  // namespace call_diag
}  // namespace lift
