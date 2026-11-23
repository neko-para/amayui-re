// emulator-lift · `_memcpy` 调用画像（临时诊断；开关 `LIFT_MEMCPY_DIAG=1`）
//
// ★ 口径：
//   * 调用次数 / `n` 直方图 / p50 / p90 / max / 总字节：在**派发到 `0x4E74B0` 的那一次**
//     读 `guest[esp+4/8/12]`（cdecl 的 dst/src/n；`esp` 指向返回地址）。
//   * `dst` / `src` 的 mod 16 / mod 4：同一次读数。
//   * `rep_bulk`：按**指令 EA** 归属到 `_memcpy` 区间（`repBulkTry` 里累加，见 `runtime/rep_bulk.h`），
//     ⛔ 不按"当前在哪个派发块"归属 —— `_memcpy` 的 rep 站点在入口块与三个 tail 块里都有。
//   * `_memcpy` 的 guest insns：每次派发 EA 落在区间里时，读 `cpu_.insns` 的前后差再求和。
//     （`lift_tick` 在块边界 / 宿主调用前结算 ⇒ 前后差就是该派发执行的条数。）
//   * 每帧：Present 之后按累计值做差，报告只打 top-N（⛔ 不每帧输出）。
#include "aot/memcpy_diag.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/rep_bulk.h"

namespace lift {
namespace memcpy_diag {

// ★ 定义在匿名 namespace 之外：头文件用 inline `active()` 读它（派发热路径）。
bool g_active_state = false;

namespace {

const bool kOn = [] {
    const char* v = std::getenv("LIFT_MEMCPY_DIAG");
    return v != nullptr && v[0] != '\0';
}();

const char* targetWindow() {
    const char* v = std::getenv("LIFT_MEMCPY_DIAG_WINDOW");
    return (v != nullptr && v[0] != '\0') ? v : "move11";
}

// ---- 2 的幂桶（报告口径）----
const char* const kBucketName[11] = {"0",     "1-3",    "4-7",   "8-15",  "16-31", "32-63",
                                     "64-127", "128-255", "256-1023", "1024-4095", ">=4096"};
unsigned bucketOf(std::uint32_t n) {
    if (n == 0) return 0;
    if (n <= 3) return 1;
    if (n <= 7) return 2;
    if (n <= 15) return 3;
    if (n <= 31) return 4;
    if (n <= 63) return 5;
    if (n <= 127) return 6;
    if (n <= 255) return 7;
    if (n <= 1023) return 8;
    if (n <= 4095) return 9;
    return 10;
}

struct SizeProfile {
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
    std::uint64_t max_n = 0;
    std::uint64_t hist[11] = {};
    std::vector<std::uint32_t> sizes;  // 精确分位数（p50/p90）；只在目标窗口里收
    void add(std::uint32_t n) {
        ++calls;
        bytes += n;
        if (n > max_n) max_n = n;
        ++hist[bucketOf(n)];
        sizes.push_back(n);
    }
};

struct FrameRow {
    std::uint64_t frame = 0;
    std::uint64_t calls = 0;
    std::uint64_t insns = 0;
    std::uint64_t bytes = 0;
};

struct Window {
    std::string label;
    std::uint64_t entry_dispatches = 0;  // 派发到 0x4E74B0 的次数（含读不出参数的）
    std::uint64_t bad_args = 0;          // 栈参数读不出来 / 越界的次数
    std::uint32_t sample_sp[3] = {};
    std::uint32_t sample_dst[3] = {};
    std::uint32_t sample_src[3] = {};
    std::uint32_t sample_n[3] = {};
    SizeProfile memcpy;
    SizeProfile memset;
    std::uint64_t dst_mod16[16] = {};
    std::uint64_t src_mod16[16] = {};
    std::uint64_t dst_mod4[4] = {};
    std::uint64_t src_mod4[4] = {};
    std::uint64_t br_sse = 0;   // n>=128 && sse 开关 && (dst&15)==(src&15) ⇒ 0x4F111F
    std::uint64_t br_rep = 0;   // 预测会走到某个 `rep movsd` 站点
    std::uint64_t br_tail = 0;  // 其余（小拷贝 / 对齐后的尾部）
    std::uint64_t mc_insns = 0; // `_memcpy` 区间内的 guest 指令数（按派发增量求和）
    std::uint64_t mc_dispatches = 0;
    std::uint64_t mc_disp_entry = 0;  // 0x4E74B0
    std::uint64_t mc_disp_7530 = 0;   // 0x4E7530（非对齐/小拷贝分派）
    std::uint64_t mc_disp_7617 = 0;   // 0x4E7617（32B 循环尾 / 尾部表）
    std::uint64_t mc_disp_other = 0;
    RepBulkDiag rep_base{};
    RepBulkDiag rep{};  // 窗口内 = 现在 − begin 时的快照
    bool has_rep_base = false;
    std::vector<FrameRow> frames;
};

std::uint32_t g_sse_switch = 1;
Window g_cur;
std::vector<Window> g_done;
bool g_have_frame = false;
std::uint64_t g_last_frame = 0;
std::uint64_t g_last_calls = 0;
std::uint64_t g_last_insns = 0;
std::uint64_t g_last_bytes = 0;

RepBulkDiag diffRep(const RepBulkDiag& now, const RepBulkDiag& base) {
    RepBulkDiag d;
    d.on = false;
    d.hits = now.hits - base.hits;
    d.declines = now.declines - base.declines;
    d.not_applicable = now.not_applicable - base.not_applicable;
    d.bytes_ok = now.bytes_ok - base.bytes_ok;
    d.bytes_declined = now.bytes_declined - base.bytes_declined;
    for (int i = 0; i < 5; ++i) d.why[i] = now.why[i] - base.why[i];
    d.mc_sites = now.mc_sites - base.mc_sites;
    d.mc_hits = now.mc_hits - base.mc_hits;
    d.mc_declines = now.mc_declines - base.mc_declines;
    d.mc_not_applicable = now.mc_not_applicable - base.mc_not_applicable;
    d.mc_bytes_ok = now.mc_bytes_ok - base.mc_bytes_ok;
    d.mc_bytes_declined = now.mc_bytes_declined - base.mc_bytes_declined;
    for (int i = 0; i < 5; ++i) d.mc_why[i] = now.mc_why[i] - base.mc_why[i];
    for (int i = 0; i < 8; ++i) d.mc_site[i] = now.mc_site[i] - base.mc_site[i];
    return d;
}

std::uint32_t percentile(SizeProfile& p, double q) {
    if (p.sizes.empty()) return 0;
    const std::size_t k = static_cast<std::size_t>(
        static_cast<double>(p.sizes.size() - 1) * q / 100.0);
    std::nth_element(p.sizes.begin(), p.sizes.begin() + static_cast<std::ptrdiff_t>(k), p.sizes.end());
    return p.sizes[k];
}

void printProfile(std::FILE* out, const char* name, SizeProfile& p) {
    const double avg = p.calls != 0 ? static_cast<double>(p.bytes) / static_cast<double>(p.calls) : 0.0;
    const std::uint32_t p50 = percentile(p, 50.0);
    const std::uint32_t p90 = percentile(p, 90.0);
    std::fprintf(out,
                 "memcpy-diag: %s calls=%llu bytes=%llu avg=%.2f p50=%u p90=%u max=%llu\n",
                 name, static_cast<unsigned long long>(p.calls),
                 static_cast<unsigned long long>(p.bytes), avg, p50, p90,
                 static_cast<unsigned long long>(p.max_n));
    std::fprintf(out, "memcpy-diag:   size hist:");
    for (int i = 0; i < 11; ++i) {
        std::fprintf(out, " %s=%llu", kBucketName[i], static_cast<unsigned long long>(p.hist[i]));
    }
    std::fprintf(out, "\n");
}

void printMods(std::FILE* out, const char* name, const std::uint64_t* v, int n) {
    std::fprintf(out, "memcpy-diag:   %s=", name);
    for (int i = 0; i < n; ++i) std::fprintf(out, "%s%llu", i == 0 ? "" : ",", static_cast<unsigned long long>(v[i]));
    std::fprintf(out, "\n");
}

}  // namespace

bool on() { return kOn; }

void setSseSwitch(std::uint32_t value) { g_sse_switch = value; }

void beginWindow(const char* label) {
    if (!kOn) return;
    const char* want = targetWindow();
    const std::string name = (label != nullptr && *label != '\0') ? label : "(unnamed)";
    if (std::strcmp(want, "*") != 0 && name != want) return;
    g_cur = Window{};
    g_cur.label = name;
    g_cur.memcpy.sizes.reserve(4u * 1024u * 1024u);
    g_cur.memset.sizes.reserve(64u * 1024u);
    g_cur.rep_base = g_rep_bulk_diag;
    g_cur.has_rep_base = true;
    g_active_state = true;
    g_have_frame = false;
    g_last_frame = 0;
    g_last_calls = 0;
    g_last_insns = 0;
    g_last_bytes = 0;
}

void endWindow() {
    if (!g_active_state) return;
    g_cur.rep = diffRep(g_rep_bulk_diag, g_cur.rep_base);
    g_done.push_back(std::move(g_cur));
    g_cur = Window{};
    g_active_state = false;
    g_have_frame = false;
}

void noteMemcpyEntry(bool ok, std::uint32_t sp, std::uint32_t dst, std::uint32_t src, std::uint32_t n) {
    if (!g_active_state) return;
    ++g_cur.entry_dispatches;
    if (!ok) {
        ++g_cur.bad_args;
        return;
    }
    if (g_cur.memcpy.calls < 3) {
        const std::size_t i = static_cast<std::size_t>(g_cur.memcpy.calls);
        g_cur.sample_sp[i] = sp;
        g_cur.sample_dst[i] = dst;
        g_cur.sample_src[i] = src;
        g_cur.sample_n[i] = n;
    }
    noteMemcpyCall(dst, src, n);
}

void noteMemcpyCall(std::uint32_t dst, std::uint32_t src, std::uint32_t n) {
    if (!g_active_state) return;
    g_cur.memcpy.add(n);
    ++g_cur.dst_mod16[dst & 15u];
    ++g_cur.src_mod16[src & 15u];
    ++g_cur.dst_mod4[dst & 3u];
    ++g_cur.src_mod4[src & 3u];
    // 分支预测：判据抄自 `0x4E74D0`..`0x4E7505`（只看**选哪条路**，不改 guest 任何值）。
    const bool sse = n >= 0x80u && g_sse_switch != 0 && ((dst ^ src) & 15u) == 0;
    const std::uint32_t align_need = (4u - (dst & 3u)) & 3u;  // 0/3/2/1
    const bool rep = !sse && n >= 32u + align_need;
    if (sse) {
        ++g_cur.br_sse;
    } else if (rep) {
        ++g_cur.br_rep;
    } else {
        ++g_cur.br_tail;
    }
}

void noteMemsetCall(std::uint32_t n) {
    if (!g_active_state) return;
    g_cur.memset.add(n);
}

void noteDispatch(std::uint32_t ea, std::uint64_t insns) {
    if (!g_active_state) return;
    ++g_cur.mc_dispatches;
    g_cur.mc_insns += insns;
    switch (ea) {
        case 0x004E74B0u: ++g_cur.mc_disp_entry; break;
        case 0x004E7530u: ++g_cur.mc_disp_7530; break;
        case 0x004E7617u: ++g_cur.mc_disp_7617; break;
        default: ++g_cur.mc_disp_other; break;
    }
}

void notePresent(std::uint64_t frame_no) {
    if (!g_active_state) return;
    if (!g_have_frame) {
        g_have_frame = true;
        g_last_frame = frame_no;
        g_last_calls = g_cur.memcpy.calls;
        g_last_insns = g_cur.mc_insns;
        g_last_bytes = g_cur.memcpy.bytes;
        return;
    }
    if (frame_no <= g_last_frame) return;
    FrameRow row;
    row.frame = frame_no;
    row.calls = g_cur.memcpy.calls - g_last_calls;
    row.insns = g_cur.mc_insns - g_last_insns;
    row.bytes = g_cur.memcpy.bytes - g_last_bytes;
    if (row.calls != 0 || row.insns != 0) g_cur.frames.push_back(row);
    g_last_frame = frame_no;
    g_last_calls = g_cur.memcpy.calls;
    g_last_insns = g_cur.mc_insns;
    g_last_bytes = g_cur.memcpy.bytes;
}

void print(std::FILE* out) {
    if (!kOn || out == nullptr) return;
    if (g_active_state) endWindow();
    if (g_done.empty()) {
        std::fprintf(out, "memcpy-diag: on（LIFT_MEMCPY_DIAG=1）但目标窗口 \"%s\" 没有出现\n", targetWindow());
        return;
    }
    for (Window& w : g_done) {
        std::fprintf(out,
                     "memcpy-diag: window \"%s\"（LIFT_MEMCPY_DIAG=1；目标窗口=\"%s\"；只读，不改 guest）\n",
                     w.label.c_str(), targetWindow());
        printProfile(out, "_memcpy", w.memcpy);
        std::fprintf(out,
                     "memcpy-diag:   entry 派发=%llu（参数可读=%llu，读不出=%llu；差 = 入口 EA 不全是 cdecl 调用）\n",
                     static_cast<unsigned long long>(w.entry_dispatches),
                     static_cast<unsigned long long>(w.memcpy.calls),
                     static_cast<unsigned long long>(w.bad_args));
        std::fprintf(out,
                     "memcpy-diag:   sample: sp=%08X/%08X/%08X dst=%08X/%08X/%08X "
                     "src=%08X/%08X/%08X n=%u/%u/%u\n",
                     w.sample_sp[0], w.sample_sp[1], w.sample_sp[2], w.sample_dst[0], w.sample_dst[1],
                     w.sample_dst[2], w.sample_src[0], w.sample_src[1], w.sample_src[2], w.sample_n[0],
                     w.sample_n[1], w.sample_n[2]);
        const double disp_per_call =
            w.memcpy.calls != 0 ? static_cast<double>(w.mc_dispatches) / static_cast<double>(w.memcpy.calls) : 0.0;
        std::fprintf(out,
                     "memcpy-diag:   _memcpy dispatches=%llu（%.2f/次调用）: entry=%llu 4E7530=%llu "
                     "4E7617=%llu other=%llu\n",
                     static_cast<unsigned long long>(w.mc_dispatches), disp_per_call,
                     static_cast<unsigned long long>(w.mc_disp_entry),
                     static_cast<unsigned long long>(w.mc_disp_7530),
                     static_cast<unsigned long long>(w.mc_disp_7617),
                     static_cast<unsigned long long>(w.mc_disp_other));
        printMods(out, "dst%16", w.dst_mod16, 16);
        printMods(out, "src%16", w.src_mod16, 16);
        printMods(out, "dst%4", w.dst_mod4, 4);
        printMods(out, "src%4", w.src_mod4, 4);
        std::fprintf(out,
                     "memcpy-diag:   branch(预测): SSE=%llu rep=%llu tail=%llu（sse_switch=%u；"
                     "SSE=n>=128 且 (dst^src)&15==0；rep=n>=32+(4-dst%%4)%%4）\n",
                     static_cast<unsigned long long>(w.br_sse),
                     static_cast<unsigned long long>(w.br_rep),
                     static_cast<unsigned long long>(w.br_tail), g_sse_switch);
        const double avg_insns =
            w.memcpy.calls != 0 ? static_cast<double>(w.mc_insns) / static_cast<double>(w.memcpy.calls) : 0.0;
        std::fprintf(out,
                     "memcpy-diag:   _memcpy 内 guest insns=%llu（avg=%.2f/次调用；按 EA 区间 "
                     "[0x4E74B0,0x4E7811) 的派发增量求和）\n",
                     static_cast<unsigned long long>(w.mc_insns), avg_insns);
        const RepBulkDiag& r = w.rep;
        std::fprintf(out,
                     "memcpy-diag:   rep_bulk(_memcpy 区间): sites=%llu hits=%llu declines=%llu n/a=%llu "
                     "｜ why[Ok=%llu,NA=%llu,CB=%llu,UC=%llu,RO=%llu] ｜ bytes: ok=%llu declined=%llu\n",
                     static_cast<unsigned long long>(r.mc_sites),
                     static_cast<unsigned long long>(r.mc_hits),
                     static_cast<unsigned long long>(r.mc_declines),
                     static_cast<unsigned long long>(r.mc_not_applicable),
                     static_cast<unsigned long long>(r.mc_why[0]),
                     static_cast<unsigned long long>(r.mc_why[1]),
                     static_cast<unsigned long long>(r.mc_why[2]),
                     static_cast<unsigned long long>(r.mc_why[3]),
                     static_cast<unsigned long long>(r.mc_why[4]),
                     static_cast<unsigned long long>(r.mc_bytes_ok),
                     static_cast<unsigned long long>(r.mc_bytes_declined));
        std::fprintf(out,
                     "memcpy-diag:   rep 站点: 4E7507=%llu 4E7564=%llu 4E758A=%llu 4E75A8=%llu "
                     "4E768C=%llu 4E76E7=%llu 4E7711=%llu 4E7743=%llu\n",
                     static_cast<unsigned long long>(r.mc_site[0]),
                     static_cast<unsigned long long>(r.mc_site[1]),
                     static_cast<unsigned long long>(r.mc_site[2]),
                     static_cast<unsigned long long>(r.mc_site[3]),
                     static_cast<unsigned long long>(r.mc_site[4]),
                     static_cast<unsigned long long>(r.mc_site[5]),
                     static_cast<unsigned long long>(r.mc_site[6]),
                     static_cast<unsigned long long>(r.mc_site[7]));
        std::fprintf(out,
                     "memcpy-diag:   rep_bulk(全窗，含别的 rep): hits=%llu declines=%llu n/a=%llu bytes_ok=%llu\n",
                     static_cast<unsigned long long>(r.hits), static_cast<unsigned long long>(r.declines),
                     static_cast<unsigned long long>(r.not_applicable),
                     static_cast<unsigned long long>(r.bytes_ok));
        // 每帧 top-8（按 `_memcpy` insns 降序；同数按帧号升序）
        std::vector<FrameRow> rows = w.frames;
        std::sort(rows.begin(), rows.end(), [](const FrameRow& a, const FrameRow& b) {
            if (a.insns != b.insns) return a.insns > b.insns;
            return a.frame < b.frame;
        });
        const std::size_t shown = rows.size() < 8 ? rows.size() : 8;
        std::fprintf(out, "memcpy-diag:   frame top（按 _memcpy insns）:\n");
        for (std::size_t i = 0; i < shown; ++i) {
            std::fprintf(out,
                         "memcpy-diag:     n=%llu calls=%llu insns=%llu bytes=%llu\n",
                         static_cast<unsigned long long>(rows[i].frame),
                         static_cast<unsigned long long>(rows[i].calls),
                         static_cast<unsigned long long>(rows[i].insns),
                         static_cast<unsigned long long>(rows[i].bytes));
        }
        printProfile(out, "_memset", w.memset);
    }
}

}  // namespace memcpy_diag
}  // namespace lift
