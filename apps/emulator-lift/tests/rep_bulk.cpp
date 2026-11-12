// emulator-lift · `rep movs*` / `rep stos*` 的宿主快路径：**两臂差分** + 闸门（pure 档）
//
// 规格：REQ-01M75XK0XRTSWCZ4G8A5W2GW8（已裁决：做；落点 `exec_string.cpp`；开关在 `X86` 上）。
// 与既有 `native_override.cpp` 的 `memcpy-ovl/*` 同形：**参考臂 = 今天的逐元素臂**
// （`X86::rep_bulk = false`），不另写一份语义。
//
// 四条判据（缺一条就不算证明）：
//   ① **逐字节**比目标区（整块 64 KiB 缓冲，两侧留哨兵 ⇒ 也抓越界多写）；
//   ② `ECX / ESI / EDI / EAX / EFLAGS` 逐位相同，`cpu.insns` 相同（快路径不许增删一条 `Insn`）；
//   ③ 诊断面 `g_access` 相同（元素臂最后一次写留下的值 —— 快路径收尾对齐它）；
//   ④ **命中计数**：闸门放行的那几类必须真的走了快路径（`rep_bulk_hits > 0`），
//      被拦的那几类必须退回（`rep_bulk_declines > 0`）—— 否则"快路径一直没被走到"也能让 ①②③ 全绿。
// 覆盖：`n = 0/1/3/4/7/8/64/4096` × {movsb,movsw,movsd,stosb,stosw,stosd,repne movsd}
//       × {DF=0,DF=1} × 重叠四组合（`dst<src` / `dst==src` / `dst>src` / 不重叠）。
//
// ★ 闸门里有两条（跨 4 GiB / safe-mode committed）**在 guest 可见状态上观察不到** —— 放它们过去时
//   逐元素臂会 fault 或读出界，"证明闸门在"的夹具自己先崩。所以那两条直接断言
//   `repBulkPlan` 的判定（见 `runtime/rep_bulk.h` 的说明）；方向 × 重叠那一条则由 ① 的差分抓。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/machine.h"
#include "runtime/rep_bulk.h"

namespace {

using lift::Insn;
using lift::Op;
using lift::RepBulkWhy;

constexpr std::uint32_t kCodeEa = 0x00401000u;  // 7 个 op 变体，每个占 0x10 字节
constexpr std::uint32_t kBufEa = 0x00480000u;   // 数据缓冲（64 KiB，全部已提交）
constexpr std::uint32_t kBufSize = 0x10000u;
constexpr std::uint32_t kSentinelEa = 0xFFFFFFFFu;
constexpr std::uint32_t kUncommittedEa = 0x30000000u;  // 4 GiB 预留里**没提交**的一段

// ---- op 变体（两臂放的是**同一份字节**）----
struct OpVariant {
    const char* name;
    const char* hex;
    std::uint8_t size;
    bool movs;
};
const OpVariant kOps[] = {
    {"movsb", "F3 A4 C3", 1, true},
    {"movsw", "66 F3 A5 C3", 2, true},
    {"movsd", "F3 A5 C3", 4, true},
    // ★ `F2`（repne）对 movs/stos 与 `F3` 是**同一条循环**（提前终止只对 cmps/scas 有意义，
    //   见 `exec_string.cpp` 的 `cmp_like`）⇒ 快路径必须给出同样的结果，这条钉住它。
    {"repne movsd", "F2 A5 C3", 4, true},
    {"stosb", "F3 AA C3", 1, false},
    {"stosw", "66 F3 AB C3", 2, false},
    {"stosd", "F3 AB C3", 4, false},
};
constexpr std::size_t kOpCount = sizeof(kOps) / sizeof(kOps[0]);

// 重叠四组合
enum Rel { kDstBelowSrc = 0, kSame = 1, kDstAboveSrc = 2, kFar = 3 };
constexpr const char* kRelName[] = {"dst<src", "dst==src", "dst>src", "far"};

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool parseHex(const char* hex, std::vector<std::uint8_t>* out) {
    out->clear();
    int hi = -1;
    for (const char* p = hex; *p; ++p) {
        if (*p == ' ' || *p == '\t') continue;
        const int v = hexNibble(*p);
        if (v < 0) return false;
        if (hi < 0) {
            hi = v;
        } else {
            out->push_back(static_cast<std::uint8_t>((hi << 4) | v));
            hi = -1;
        }
    }
    return hi < 0;
}

// 初值：定种子 xorshift32 ⇒ 两臂拿到逐字节相同的缓冲，且**不是常量图样**
// （常量图样会让"涂抹"看不出来 —— 那样方向闸门的变异照样绿）。
std::vector<std::uint8_t> makePattern() {
    std::vector<std::uint8_t> b(kBufSize);
    std::uint32_t x = 0x12345678u;
    for (std::size_t i = 0; i < b.size(); ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b[i] = static_cast<std::uint8_t>(x >> 24);
    }
    return b;
}

// 一条臂 = 一台 Machine（`rep_bulk` 在这种进程里可切，见 `X86::rep_bulk`）
struct Arm {
    lift::Machine m;
    bool ok = false;
    std::string err;

    explicit Arm(bool bulk) {
        ok = m.initBare(&err) && m.prepareStack(&err);
        if (!ok) return;
        m.cpu().rep_bulk = bulk;
        for (std::size_t i = 0; i < kOpCount; ++i) {
            std::vector<std::uint8_t> code;
            if (!parseHex(kOps[i].hex, &code)) {
                ok = false;
                err = std::string("bad hex: ") + kOps[i].hex;
                return;
            }
            if (!m.placeCode(kCodeEa + static_cast<std::uint32_t>(i) * 0x10u, code.data(), code.size(), &err)) {
                ok = false;
                return;
            }
        }
    }
};

struct Snap {
    bool reached = false;
    std::vector<std::uint8_t> mem;
    std::uint32_t r[8] = {};
    std::uint32_t eflags = 0;
    std::uint64_t insns = 0;
    std::uint32_t acc_ea = 0;
    std::uint8_t acc_size = 0;
    std::uint8_t acc_write = 0;
    std::uint64_t hits = 0;      // 本条 rep 的增量
    std::uint64_t declines = 0;  // 同上
};

struct Scenario {
    std::size_t op = 0;
    std::uint32_t n = 0;
    bool df = false;
    int rel = kFar;
    std::uint32_t src = 0;
    std::uint32_t dst = 0;
};

// 四个重叠组合的落点：DF=0 从低往高、DF=1 从高往低（两端各留 0x2000 余量，n 最大 16 KiB）
void placeRanges(const Scenario& s, std::uint32_t* src, std::uint32_t* dst) {
    const std::uint32_t anchor = kBufEa + (s.df ? 0xC000u : 0x2000u);
    switch (s.rel) {
        case kDstBelowSrc:
            *dst = anchor;
            *src = anchor + 1u;  // 重叠，位移 1（4 B 分块最容易涂抹的那一档）
            break;
        case kSame:
            *dst = anchor;
            *src = anchor;
            break;
        case kDstAboveSrc:
            *src = anchor;
            *dst = anchor + 1u;
            break;
        default:  // 不重叠：向行进方向错开 0x4000（n 最大 0x4000 ⇒ 恰好相邻、不交）
            *src = anchor;
            *dst = s.df ? (anchor - 0x4000u) : (anchor + 0x4000u);
            break;
    }
}

bool runScenario(Arm& arm, const Scenario& s, const std::vector<std::uint8_t>& pat, Snap* out) {
    std::string err;
    if (!arm.m.placeData(kBufEa, pat.data(), pat.size(), &err)) return false;
    lift::X86& c = arm.m.cpu();
    const std::uint64_t h0 = c.rep_bulk_hits;
    const std::uint64_t d0 = c.rep_bulk_declines;
    c.ecx() = s.n;
    c.esi() = s.src;
    c.edi() = s.dst;
    c.eax() = 0xDEADBEEFu;
    c.setDf(s.df);
    c.esp() = arm.m.layout().stack_top - 4u;
    arm.m.mem().wr32(c.esp(), kSentinelEa);
    c.eip = kCodeEa + static_cast<std::uint32_t>(s.op) * 0x10u;

    // ★ 两段跑：第一段 `max_insns = 1` ⇒ **只**执行那条 rep（判据：`machine.cpp` 的
    //   `max_insns` 检查在**执行之前**，`cpu_.insns = 0 < 1` ⇒ 这一条一定执行，随后在 `ret` 前停下）。
    //   这样 `g_access` 抓到的才是 **rep 自己**留下的诊断值；一次跑到底的话它会被 `ret` 的
    //   读栈覆盖，两臂永远"相同"，③ 就成了恒真断言。
    lift::RunOptions o;
    o.until_ea = kSentinelEa;
    o.max_insns = 1ull;
    const lift::RunResult r1 = arm.m.run(o);
    out->acc_ea = lift::g_access.ea;
    out->acc_size = lift::g_access.size;
    out->acc_write = lift::g_access.write;
    if (r1.reason != lift::StopReason::MaxInsns) {
        out->reached = false;
        return false;
    }
    // 第二段：跑完 `ret`（落到哨兵）
    o.max_insns = 0ull;
    const lift::RunResult r2 = arm.m.run(o);
    out->reached = (r2.reason == lift::StopReason::UntilEa);
    out->mem.assign(arm.m.mem().host(kBufEa), arm.m.mem().host(kBufEa) + kBufSize);
    for (int i = 0; i < 8; ++i) out->r[i] = c.r[i];
    out->eflags = c.eflags;
    out->insns = c.insns;
    out->hits = c.rep_bulk_hits - h0;
    out->declines = c.rep_bulk_declines - d0;
    return out->reached;
}

// 首个不同字节的下标（-1 = 全同）
long firstDiff(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b) {
    const std::size_t n = a.size() < b.size() ? a.size() : b.size();
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) return static_cast<long>(i);
    }
    return a.size() == b.size() ? -1 : static_cast<long>(n);
}

// 一条串操作实际访问的字节区间（**定义**，不是抄实现）：首个元素在起点，之后按 DF 走。
//   DF=0 ⇒ `[start, start+nb)`；DF=1 ⇒ `[start-nb+size, start+size)`（末元素的高端 = start+size）。
void blockOf(std::uint32_t start, std::uint32_t nb, std::uint8_t size, bool df, std::uint64_t* lo,
             std::uint64_t* hi) {
    if (df) {
        *hi = static_cast<std::uint64_t>(start) + size;
        *lo = *hi - nb;
    } else {
        *lo = start;
        *hi = static_cast<std::uint64_t>(start) + nb;
    }
}

// 快路径应不应该被**方向闸门**拦下（只针对 `movs`；`stos` 没有源）
bool expectReverseOverlap(const Scenario& s) {
    if (!kOps[s.op].movs || s.n == 0) return false;
    const std::uint8_t size = kOps[s.op].size;
    const std::uint32_t nb = s.n * size;
    std::uint64_t dlo = 0, dhi = 0, slo = 0, shi = 0;
    blockOf(s.dst, nb, size, s.df, &dlo, &dhi);
    blockOf(s.src, nb, size, s.df, &slo, &shi);
    const bool overlap = dlo < shi && slo < dhi;
    if (!overlap || dlo == slo) return false;
    return s.df ? (dlo < slo) : (dlo > slo);
}

// ---- ① 两臂差分（全矩阵）----
void runDifferential(const std::vector<std::uint8_t>& pat) {
    Arm bulk(true);
    Arm elem(false);
    if (!bulk.ok || !elem.ok) {
        LIFT_CHECK(bulk.ok);
        LIFT_CHECK(elem.ok);
        std::fprintf(stderr, "夹具 Machine 起不来：%s / %s\n", bulk.err.c_str(), elem.err.c_str());
        return;
    }
    const std::uint32_t kNs[] = {0u, 1u, 3u, 4u, 7u, 8u, 64u, 4096u};
    int scenarios = 0;
    int n_hit = 0;
    int n_declined = 0;
    int n_skipped = 0;
    for (std::size_t op = 0; op < kOpCount; ++op) {
        for (std::uint32_t n : kNs) {
            for (int dfi = 0; dfi < 2; ++dfi) {
                for (int rel = 0; rel < 4; ++rel) {
                    Scenario s;
                    s.op = op;
                    s.n = n;
                    s.df = dfi != 0;
                    s.rel = rel;
                    placeRanges(s, &s.src, &s.dst);
                    char label[160];
                    std::snprintf(label, sizeof(label), "diff/%s/n=%u/DF=%d/%s", kOps[op].name, n, dfi,
                                  kRelName[rel]);
                    lift_test::g_current_case = label;

                    Snap a;
                    Snap b;
                    LIFT_CHECK(runScenario(bulk, s, pat, &a));
                    LIFT_CHECK(runScenario(elem, s, pat, &b));
                    ++scenarios;

                    // ① 目标区逐字节（整块缓冲 ⇒ 含两侧哨兵：抓越界多写 / 少写）
                    if (std::memcmp(a.mem.data(), b.mem.data(), kBufSize) != 0) {
                        lift_test::failf(__FILE__, __LINE__,
                                         "两臂内存不同：首个差异偏移 0x%04lX（快路径 0x%02X / 元素臂 0x%02X）",
                                         firstDiff(a.mem, b.mem), static_cast<unsigned>(a.mem[firstDiff(a.mem, b.mem)]),
                                         static_cast<unsigned>(b.mem[firstDiff(a.mem, b.mem)]));
                    }
                    ++lift_test::g_checks;
                    // ② 寄存器（含 ECX/ESI/EDI）+ EFLAGS + 指令数
                    for (int i = 0; i < 8; ++i) LIFT_CHECK_EQ_U32(a.r[i], b.r[i]);
                    LIFT_CHECK_EQ_U32(a.eflags, b.eflags);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.insns), static_cast<std::uint32_t>(b.insns));
                    // ③ 诊断面
                    LIFT_CHECK_EQ_U32(a.acc_ea, b.acc_ea);
                    LIFT_CHECK_EQ_U32(a.acc_size, b.acc_size);
                    LIFT_CHECK_EQ_U32(a.acc_write, b.acc_write);
                    // ④ 命中计数：放行的必须真的走了快路径，被拦的必须真的退回了
                    const bool reverse = expectReverseOverlap(s);
                    const bool eligible = (n != 0);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(b.hits), 0u);  // 参考臂一条都不许走快路径
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(b.declines), 0u);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.hits), eligible && !reverse ? 1u : 0u);
                    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.declines), eligible && reverse ? 1u : 0u);
                    if (!eligible) {
                        ++n_skipped;
                    } else if (reverse) {
                        ++n_declined;
                    } else {
                        ++n_hit;
                    }
                }
            }
        }
    }
    std::printf("rep-bulk: diff matrix %d scenarios (%zu ops × 8 n × 2 DF × 4 重叠组合)\n", scenarios,
                kOpCount);
    std::printf("rep-bulk: 快路径 hits=%d / 方向闸门退回=%d / ECX=0 不适用=%d\n", n_hit, n_declined, n_skipped);
    // 三条分档都必须非空 —— 否则"矩阵"里有一档从来没被走到（换了夹具也照样绿）。
    LIFT_CHECK(n_hit > 0);
    LIFT_CHECK(n_declined > 0);
    LIFT_CHECK(n_skipped > 0);
}

// ---- ② 闸门：跨 4 GiB（64 位算）----
struct GateFixture {
    lift::Machine m;
    bool ok = false;
    std::string err;
    GateFixture() {
        ok = m.initBare(&err);
        // 把数据缓冲**提交**上：`gate/committed` 那条要能区分"已提交 ⇒ 放行"与"未提交 ⇒ 退回"
        // （否则"safe mode 下永远退回"也能让判据恒真）。
        if (ok) ok = m.mem().commit(kBufEa, kBufSize, lift::RegionKind::Image, "rep-bulk-gate");
    }
    lift::X86& cpu() { return m.cpu(); }
};

RepBulkWhy gateOf(GateFixture& g, Op op, std::uint32_t ecx, std::uint32_t esi, std::uint32_t edi,
                  std::uint8_t size, int rep, bool df, std::uint64_t* bytes = nullptr, bool* overlap = nullptr) {
    Insn in;
    in.op = op;
    in.opsz = size;
    in.rep = static_cast<std::uint8_t>(rep);
    g.cpu().ecx() = ecx;
    g.cpu().esi() = esi;
    g.cpu().edi() = edi;
    g.cpu().setDf(df);
    const lift::RepBulkPlan p = lift::repBulkPlan(g.cpu(), in, size);
    if (bytes) *bytes = p.bytes;
    if (overlap) *overlap = p.overlap;
    return p.why;
}

void runGates() {
    GateFixture g;
    if (!g.ok) {
        LIFT_CHECK(g.ok);
        return;
    }
    lift::X86& c = g.cpu();

    lift_test::g_current_case = "gate/mul-64bit";
    {
        // ★ `ECX=0x40000001, size=4 ⇒ n = 0x1_00000004`。**32 位**算会得 4（闸门失效）⇒ 这条钉住
        //    "`ECX × 元素宽度` 用 64 位算"。
        std::uint64_t bytes = 0;
        LIFT_CHECK(gateOf(g, Op::Movs, 0x40000001u, kBufEa, kBufEa, 4, 0xF3, false, &bytes) ==
                    RepBulkWhy::CrossBoundary);
        LIFT_CHECK(bytes == 0x100000004ull);
        // n 恰好 = 4 GiB：也不许快路径（宿主平坦映射只有 4 GiB）
        LIFT_CHECK(gateOf(g, Op::Movs, 0x40000000u, kBufEa, kBufEa, 4, 0xF3, false) ==
                   RepBulkWhy::CrossBoundary);
        // ★ `start == 0, n == 4 GiB`：区间 `[0, 4 GiB)` 在地址空间上是"合法"的 ⇒ 只有 `n < 4 GiB`
        //   这一条能挡住它；漏掉就会被截断成 `n = 0`（"什么都不搬、ECX 却清零"）。
        LIFT_CHECK(gateOf(g, Op::Stos, 0x40000000u, 0, 0u, 4, 0xF3, false) == RepBulkWhy::CrossBoundary);
        // `ECX=0x10000001, size=1`：n 本身 < 4 GiB，但 dst + n 跨界 ⇒ 退回
        LIFT_CHECK(gateOf(g, Op::Stos, 0x10000001u, 0, 0xFF000000u, 1, 0xF3, false) ==
                   RepBulkWhy::CrossBoundary);
        // 目标上界：dst + n > 4 GiB
        LIFT_CHECK(gateOf(g, Op::Stos, 0x100u, 0, 0xFFFFFF00u, 4, 0xF3, false) == RepBulkWhy::CrossBoundary);
        // 源上界：src + n > 4 GiB
        LIFT_CHECK(gateOf(g, Op::Movs, 0x10u, 0xFFFFFFF0u, kBufEa, 4, 0xF3, false) == RepBulkWhy::CrossBoundary);
        // 边界**含**：dst + n == 4 GiB 是合法的（最后一个字节在 4 GiB-1）
        LIFT_CHECK(gateOf(g, Op::Stos, 0x400u, 0, 0xFFFFF000u, 4, 0xF3, false) == RepBulkWhy::Ok);
    }

    lift_test::g_current_case = "gate/ecx-zero";
    {
        std::uint64_t bytes = 123;
        LIFT_CHECK(gateOf(g, Op::Movs, 0u, kBufEa, kBufEa, 4, 0xF3, false, &bytes) == RepBulkWhy::NotApplicable);
        LIFT_CHECK(bytes == 0ull);
        LIFT_CHECK(gateOf(g, Op::Stos, 0u, 0, kBufEa, 1, 0xF3, false) == RepBulkWhy::NotApplicable);
    }

    lift_test::g_current_case = "gate/committed";
    {
        // ★ safe mode + 未提交页 ⇒ 必须退回（写之前判：替 guest 搬等于把 fault 掩盖成"部分拷贝"）
        c.mem->setSafeMode(true);
        LIFT_CHECK(gateOf(g, Op::Stos, 64u, 0, kUncommittedEa, 4, 0xF3, false) == RepBulkWhy::Uncommitted);
        LIFT_CHECK(gateOf(g, Op::Movs, 64u, kUncommittedEa, kBufEa, 4, 0xF3, false) == RepBulkWhy::Uncommitted);
        // 未提交页落在跨度**中间**（不是首字节）：同样必须退回
        LIFT_CHECK(gateOf(g, Op::Stos, 0x2000u, 0, kUncommittedEa - 0x1000u, 4, 0xF3, false) ==
                   RepBulkWhy::Uncommitted);
        // 已提交（缓冲区内）：放行 —— 否则上面三条可能只是"safe mode 下永远退回"的恒真判据
        LIFT_CHECK(gateOf(g, Op::Stos, 64u, 0, kBufEa, 4, 0xF3, false) == RepBulkWhy::Ok);
        c.mem->setSafeMode(false);
        // ⚠ 与 `nativeMemcpy` 同口径：非 safe mode 下**不查** committed（同一地址 ⇒ 放行）
        LIFT_CHECK(gateOf(g, Op::Stos, 64u, 0, kUncommittedEa, 4, 0xF3, false) == RepBulkWhy::Ok);
    }

    lift_test::g_current_case = "gate/direction-overlap";
    {
        bool ov = false;
        // DF=0 + dst<src（重叠，位移 1）：可前向块搬
        LIFT_CHECK(gateOf(g, Op::Movs, 8u, kBufEa + 1u, kBufEa, 4, 0xF3, false, nullptr, &ov) == RepBulkWhy::Ok);
        LIFT_CHECK(ov);
        // DF=0 + dst>src（重叠）：逆向涂抹 ⇒ 退回
        LIFT_CHECK(gateOf(g, Op::Movs, 8u, kBufEa, kBufEa + 1u, 4, 0xF3, false) == RepBulkWhy::ReverseOverlap);
        // DF=1 + dst>src（重叠）：可后向块搬
        LIFT_CHECK(gateOf(g, Op::Movs, 8u, kBufEa, kBufEa + 1u, 4, 0xF3, true) == RepBulkWhy::Ok);
        // DF=1 + dst<src（重叠）：逆向涂抹 ⇒ 退回
        LIFT_CHECK(gateOf(g, Op::Movs, 8u, kBufEa + 1u, kBufEa, 4, 0xF3, true) == RepBulkWhy::ReverseOverlap);
        // dst == src：内存不变，两个方向都放行
        LIFT_CHECK(gateOf(g, Op::Movs, 8u, kBufEa, kBufEa, 4, 0xF3, false) == RepBulkWhy::Ok);
        LIFT_CHECK(gateOf(g, Op::Movs, 8u, kBufEa, kBufEa, 4, 0xF3, true) == RepBulkWhy::Ok);
        // 不重叠：方向无关（`dst>src` 也不该被拦 —— 拦了只是慢，但那是恒真的"永远退回"）
        LIFT_CHECK(gateOf(g, Op::Movs, 8u, kBufEa, kBufEa + 0x8000u, 4, 0xF3, false) == RepBulkWhy::Ok);
        LIFT_CHECK(gateOf(g, Op::Movs, 8u, kBufEa + 0x8000u, kBufEa, 4, 0xF3, true) == RepBulkWhy::Ok);
        // `stos` 没有源 ⇒ 方向闸门与它无关
        LIFT_CHECK(gateOf(g, Op::Stos, 8u, 0, kBufEa + 1u, 4, 0xF3, true) == RepBulkWhy::Ok);
    }

    lift_test::g_current_case = "gate/shape";
    {
        // 形态：不是 rep / 不是 movs·stos / 宽度非法 / 宿主开关关了 ⇒ 都不算"被闸门拦下"
        LIFT_CHECK(gateOf(g, Op::Movs, 8u, kBufEa, kBufEa, 4, 0x00, false) == RepBulkWhy::NotApplicable);
        LIFT_CHECK(gateOf(g, Op::Lods, 8u, kBufEa, 0, 4, 0xF3, false) == RepBulkWhy::NotApplicable);
        LIFT_CHECK(gateOf(g, Op::Scas, 8u, 0, kBufEa, 4, 0xF2, false) == RepBulkWhy::NotApplicable);
        LIFT_CHECK(gateOf(g, Op::Stos, 8u, 0, kBufEa, 3, 0xF3, false) == RepBulkWhy::NotApplicable);
        c.rep_bulk = false;
        LIFT_CHECK(gateOf(g, Op::Stos, 8u, 0, kBufEa, 4, 0xF3, false) == RepBulkWhy::NotApplicable);
        c.rep_bulk = true;
        LIFT_CHECK(gateOf(g, Op::Stos, 8u, 0, kBufEa, 4, 0xF3, false) == RepBulkWhy::Ok);
    }
}

// ---- ③ `ECX == 0` × 未提交页（safe mode）：一条都不执行、不 fault、寄存器不动 ----
void runEcxZeroUncommitted() {
    for (int arm_i = 0; arm_i < 2; ++arm_i) {
        const bool bulk = arm_i == 0;
        lift_test::g_current_case = bulk ? "ecx-zero-uncommitted/bulk" : "ecx-zero-uncommitted/element";
        Arm arm(bulk);
        LIFT_CHECK(arm.ok);
        if (!arm.ok) return;
        arm.m.mem().setSafeMode(true);
        lift::X86& c = arm.m.cpu();
        c.ecx() = 0u;
        c.esi() = kUncommittedEa;
        c.edi() = kUncommittedEa;
        c.eax() = 0xDEADBEEFu;
        c.setDf(false);
        c.esp() = arm.m.layout().stack_top - 4u;
        arm.m.mem().wr32(c.esp(), kSentinelEa);
        c.eip = kCodeEa;  // movsb
        lift::RunOptions o;
        o.until_ea = kSentinelEa;
        o.max_insns = 1000ull;
        const lift::RunResult r = arm.m.run(o);
        LIFT_CHECK(r.reason == lift::StopReason::UntilEa);  // ⛔ 不许 fault
        LIFT_CHECK_EQ_U32(c.ecx(), 0u);
        LIFT_CHECK_EQ_U32(c.esi(), kUncommittedEa);
        LIFT_CHECK_EQ_U32(c.edi(), kUncommittedEa);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(c.rep_bulk_hits), 0u);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(c.rep_bulk_declines), 0u);
    }
}

}  // namespace

int main() {
    using namespace lift_test;
    const std::vector<std::uint8_t> pat = makePattern();

    g_current_case = "diff-matrix";
    runDifferential(pat);

    runGates();

    runEcxZeroUncommitted();

    // 汇总（`finish` 在 0 条断言时也返回非 0：空夹具不算绿）
    std::printf("rep-bulk: bulk/fast-path differential done\n");
    return finish("rep-bulk");
}
