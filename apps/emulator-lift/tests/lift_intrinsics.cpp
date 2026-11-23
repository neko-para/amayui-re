// emulator-lift · 构建期库内建（v1：静态 CRT 的 `_memcpy` @ 0x004E74B0）的契约测试
//
// 判据（红了意味着什么）：
//   ① **闸门**（`memcpyGatePlan` / C 入口）逐条：`n == 0` 放行；`n <= 16` 且两段都在 4 GiB 内且
//      committed 才放行；`n > 16` / 跨 4 GiB / 未提交页 / `cpu.mem == nullptr` / `--no-intrinsics`
//      必须 **decline**。闸门**不写任何 guest 状态**（8 GPR + EFLAGS 前后逐位相同）。
//   ② **执行**（`lift_intrinsic_memcpy_do`）逐字节等于参考 `std::memmove`：
//      `size ∈ {0,1,3,4,7,8,15,16}` × 对齐（dst/src 各 mod 4 / mod 16）×
//      {不重叠 / dst<src 重叠 / dst>src 重叠}；dst 之外的哨兵不被写；helper 不碰 8 GPR + EFLAGS。
//   ③ **生成物端到端**（`LIFT_INTRINSICS_GEN`；缺 clang / 缺 PE 时只跑 ①/② 并 SKIP 这一段）：
//      按 `lift_table.inc` 的最小 trampoline 派发 `lift_cc_4E74B0`，断言闸门 Ok ⇒ `EAX = dst`、
//      `ESP += 4`、`EBX/ESI/EDI/EBP` 不变、DF=0、`ret` 回 `[esp]`；闸门拒绝（`n > 16`）⇒ 走原函数体，
//      与 `--no-intrinsics`（闸门关）逐位相同（8 GPR + EFLAGS + insns + 整段内存）。这条同时是
//      A/B 的**夹具级**差分。
//
// ⛔ 不断言 ECX / EDX 的残留值：cdecl 调用者不依赖被调方留下的它们（显式裁决，见
//    `runtime/lift_intrinsics.h` 头注）。⛔ 也不用 test 记住历史读数（AGENTS §9）。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/image.h"
#include "runtime/lift_abi.h"
#include "runtime/lift_intrinsics.h"
#include "runtime/machine.h"
#include "runtime/memory.h"
#include "runtime/x86.h"

#if defined(LIFT_INTRINSICS_GEN)
// 生成物的**派发表**（`lift-gen --table`）：`EA → lift_cc_<root>`。本夹具自己实现一个最小
// trampoline（与 AOT 的 trampoline 同形态）：`_memcpy` 的单元根被切成多个分量（入口 + 跳表目标），
// 函数返回的 EA 可能不是调用者的返回地址 ⇒ 必须按表继续派发，直到回到调用者的返回地址。
#define LIFT_AOT_ENTRY(ea, name) \
    extern "C" std::uint32_t name(void*, std::uint8_t*, std::uint32_t, void*);
#include "lift_table.inc"
#undef LIFT_AOT_ENTRY

#define LIFT_AOT_ENTRY(ea, name) {ea, &name},
struct FixtureEntry {
    std::uint32_t ea;
    lift::LiftFn fn;
};
constexpr FixtureEntry kFixtureTable[] = {
#include "lift_table.inc"
};
#undef LIFT_AOT_ENTRY

lift::LiftFn fixtureLookup(std::uint32_t ea) {
    for (const auto& row : kFixtureTable) {
        if (row.ea == ea) return row.fn;
    }
    return nullptr;
}
#endif

namespace {

using lift::IntrinsicWhy;

constexpr std::uint32_t kMemcpyEa = 0x004E74B0u;
constexpr std::uint32_t kRetEa = 0x00401234u;
// ★ 0x00600000 在 PE 镜像之外（`ops_diff` 的 scratch 同一段），避让 `mapPeImage` 的 0x400000 起。
constexpr std::uint32_t kAreaEa = 0x00600000u;   // 16 KiB 全部提交（数据 + 夹具栈）
constexpr std::uint32_t kAreaSize = 0x4000u;
constexpr std::uint32_t kBufEa = kAreaEa;
constexpr std::uint32_t kBufSize = 0x3000u;      // dst/src 都在这段里（最大 n = 4096）
constexpr std::uint32_t kAnchor = kBufEa + 0x800u;
constexpr std::uint32_t kStackEa = kAreaEa + 0x3800u;

// 定种子 xorshift32：不是常量图样（常量图样会让"涂抹"看不出来）。
std::vector<std::uint8_t> makePattern(std::size_t n) {
    std::vector<std::uint8_t> b(n);
    std::uint32_t x = 0x89ABCDEFu;
    for (std::size_t i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b[i] = static_cast<std::uint8_t>(x >> 24);
    }
    return b;
}

struct Fixture {
    lift::Machine m;
    std::string err;
    bool ok = false;
    std::vector<std::uint8_t> pattern;

    Fixture() {
        ok = m.initBare(&err) && m.prepareStack(&err);
        if (!ok) return;
#if defined(LIFT_INTRINSICS_GEN)
        // ★ 生成物端到端要执行 `_memcpy` 的**原函数体**（闸门拒绝 / `--no-intrinsics` 那两条路）：
        //   它的块里有跳表读取（0x4E75B4 起的 `.text` dword）⇒ 必须把 PE 映射进 guest（与
        //   `tests/ops_diff.cpp` 的 Arm 同一套）。纯 helper 段不需要它，但映射一次不贵。
        lift::PeImage image;
        if (!lift::loadPeFile(LIFT_INTRINSICS_PE_PATH, &image, &err) ||
            !lift::mapPeImage(m.mem(), image, &err)) {
            ok = false;
            return;
        }
#endif
        pattern = makePattern(kAreaSize);
        if (!m.placeData(kAreaEa, pattern.data(), pattern.size(), &err)) {
            ok = false;
            return;
        }
        // 夹具栈：`[esp]=返回地址 / [esp+4]=dst / [esp+8]=src / [esp+12]=n`（cdecl 调用现场）。
        m.mem().wr32(kStackEa, 0xDEADBEEFu);
    }

    lift::X86& cpu() { return m.cpu(); }
    lift::GuestMemory& mem() { return m.mem(); }

    void resetPattern() {
        m.mem().writeBytes(kBufEa, pattern.data(), kBufSize);
    }
};

struct RegSnap {
    std::uint32_t r[8] = {};
    std::uint32_t eflags = 0;
    std::uint64_t insns = 0;
};

RegSnap snap(const lift::X86& cpu) {
    RegSnap s;
    for (int i = 0; i < 8; ++i) s.r[i] = cpu.r[i];
    s.eflags = cpu.eflags;
    s.insns = cpu.insns;
    return s;
}

bool regsEqual(const RegSnap& a, const RegSnap& b) {
    for (int i = 0; i < 8; ++i) {
        if (a.r[i] != b.r[i]) return false;
    }
    return a.eflags == b.eflags;
}

// 参考臂：同一份初始字节上做 `std::memmove`（宿主真值）。
std::vector<std::uint8_t> referenceMove(const std::vector<std::uint8_t>& before, std::uint32_t dst,
                                        std::uint32_t src, std::uint32_t n) {
    std::vector<std::uint8_t> want = before;
    if (n != 0) {
        std::memmove(want.data() + (dst - kBufEa), want.data() + (src - kBufEa),
                     static_cast<std::size_t>(n));
    }
    return want;
}

// ①+②：闸门 Ok 的每一组都把结果与参考 memmove 逐字节比；并断言"dst 之外一个字节都没动"
//     （参考比整段已经覆盖，这里再点名哨兵，红了能直接看出是越界写）。
void checkFastCase(Fixture& f, std::uint32_t dst, std::uint32_t src, std::uint32_t n,
                   const char* label) {
    (void)label;
    f.resetPattern();
    std::vector<std::uint8_t> before(kAreaSize);
    std::memcpy(before.data(), f.mem().host(kAreaEa), kAreaSize);

    lift::X86& cpu = f.cpu();
    lift::setIntrinsicsEnabled(true);
    const RegSnap pre = snap(cpu);
    const lift::MemcpyGatePlan plan = lift::memcpyGatePlan(cpu, dst, src, n);
    LIFT_CHECK(plan.why == IntrinsicWhy::Ok);
    const RegSnap after_plan = snap(cpu);
    LIFT_CHECK(regsEqual(pre, after_plan));  // 闸门不写 guest 状态

    const lift::IntrinsicCounters before_c = lift::intrinsicCounters();
    const bool ok = lift_intrinsic_memcpy_gate(&cpu, lift::liftRegsView(cpu), dst, src, n);
    LIFT_CHECK(ok);
    const RegSnap after_gate = snap(cpu);
    LIFT_CHECK(regsEqual(pre, after_gate));
    if (n != 0) {
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::intrinsicCounters().fast),
                          static_cast<std::int64_t>(before_c.fast + 1));
    }

    const lift::IntrinsicCounters before_do = lift::intrinsicCounters();
    lift_intrinsic_memcpy_do(&cpu, dst, src, n);
    const RegSnap after_do = snap(cpu);
    LIFT_CHECK(regsEqual(pre, after_do));  // helper 本身不碰 8 GPR + EFLAGS（EAX/ESP 由 IR 写）
    (void)before_do;

    const std::vector<std::uint8_t> want = referenceMove(before, dst, src, n);
    const std::uint8_t* got = f.mem().host(kAreaEa);
    bool same = std::memcmp(want.data(), got, kAreaSize) == 0;
    LIFT_CHECK(same);
    if (!same) {
        for (std::uint32_t i = 0; i < kAreaSize; ++i) {
            if (want[i] != got[i]) {
                std::fprintf(stderr, "  first diff @ %08X (want %02X got %02X) n=%u dst=%08X src=%08X\n",
                             kAreaEa + i, want[i], got[i], n, dst, src);
                break;
            }
        }
    }
    // 点名哨兵：`[dst-8, dst)` 与 `[dst+n, dst+n+8)` 必须逐字节等于 before。
    for (std::uint32_t k = 1; k <= 8; ++k) {
        const std::uint32_t at = dst - k;
        if (at >= kBufEa && at <= kBufEa + kBufSize - 1u) {
            LIFT_CHECK_EQ_U32(got[at - kAreaEa], before[at - kAreaEa]);
        }
    }
    for (std::uint32_t k = 0; k < 8; ++k) {
        const std::uint32_t at = dst + n + k;
        if (at >= kBufEa && at <= kBufEa + kBufSize - 1u) {
            LIFT_CHECK_EQ_U32(got[at - kAreaEa], before[at - kAreaEa]);
        }
    }
}

void checkDecline(Fixture& f, std::uint32_t dst, std::uint32_t src, std::uint32_t n,
                  IntrinsicWhy want, const char* what, bool enabled = true) {
    lift::X86& cpu = f.cpu();
    lift::setIntrinsicsEnabled(enabled);
    f.resetPattern();
    const std::vector<std::uint8_t> before(f.mem().host(kAreaEa),
                                           f.mem().host(kAreaEa) + kAreaSize);
    const RegSnap pre = snap(cpu);
    const lift::MemcpyGatePlan plan = lift::memcpyGatePlan(cpu, dst, src, n);
    if (plan.why != want) {
        // ★ 这句就是变异条目点名的那一行：把 `committed` 检查去掉时，未提交页会被判成 Ok。
        lift_test::failf(__FILE__, __LINE__,
                         "gate reason mismatch (%s): got why=%d want=%d n=%u dst=%08X src=%08X",
                         what, static_cast<int>(plan.why), static_cast<int>(want), n, dst, src);
    }
    LIFT_CHECK(plan.why == want);
    const bool ok = lift_intrinsic_memcpy_gate(&cpu, lift::liftRegsView(cpu), dst, src, n);
    LIFT_CHECK(!ok);
    LIFT_CHECK(regsEqual(pre, snap(cpu)));
    LIFT_CHECK(std::memcmp(before.data(), f.mem().host(kAreaEa), kAreaSize) == 0);  // 拒绝不写内存
    lift::setIntrinsicsEnabled(true);
}

// ② 覆盖矩阵：size × 对齐 × 重叠关系。
void contractMatrix(Fixture& f) {
    const std::uint32_t sizes[] = {0, 1, 3, 4, 7, 8, 15, 16};
    const std::uint32_t dst_mod4[] = {0, 1, 2, 3};
    const std::uint32_t src_mod4[] = {0, 1, 2, 3};
    const std::uint32_t dst_mod16[] = {0, 4, 8, 12};
    const std::uint32_t src_mod16[] = {0, 4, 8, 12};
    for (const std::uint32_t n : sizes) {
        // 不重叠：dst 在 0x800 附近，src 在 0x1800 附近（最大 16 B，绝不交叠）
        for (const std::uint32_t dm : dst_mod4) {
            for (const std::uint32_t sm : src_mod4) {
                const std::uint32_t dst = kAnchor + dm;
                const std::uint32_t src = kBufEa + 0x1800u + sm;
                checkFastCase(f, dst, src, n, "no-overlap/mod4");
            }
        }
        for (const std::uint32_t dm : dst_mod16) {
            for (const std::uint32_t sm : src_mod16) {
                const std::uint32_t dst = kAnchor + dm;
                const std::uint32_t src = kBufEa + 0x1800u + sm;
                checkFastCase(f, dst, src, n, "no-overlap/mod16");
            }
        }
        // dst < src 重叠（位移 1 / 3）：memmove 必须等价于"前向逐字节"（不能按块涂抹）
        for (const std::uint32_t delta : {1u, 3u}) {
            const std::uint32_t dst = kAnchor;
            const std::uint32_t src = dst + delta;
            checkFastCase(f, dst, src, n, "dst<src overlap");
        }
        // dst > src 重叠：memmove 必须等价于"后向逐字节"
        for (const std::uint32_t delta : {1u, 3u}) {
            const std::uint32_t src = kAnchor;
            const std::uint32_t dst = src + delta;
            checkFastCase(f, dst, src, n, "dst>src overlap");
        }
        // 首尾对齐到 16 的（mod 16 = 0）— 与上面的 mod16 组合不同：这里 dst/src 同余
        checkFastCase(f, kBufEa + 0x1000u, kBufEa + 0x2000u, n, "aligned16");
    }
}

void declineMatrix(Fixture& f) {
    // `n > 16`：即使 committed、即使 dst/src 对齐，也必须退。
    for (const std::uint32_t n : {17u, 32u, 128u, 4096u}) {
        checkDecline(f, kBufEa + 0x100u, kBufEa + 0x1000u, n, IntrinsicWhy::SizeTooLarge,
                     "n > fast-max");
    }
    // 跨 4 GiB：`dst+n` 或 `src+n` 回绕/越界（32 位加法会把它算成小值 ⇒ 必须 64 位判）。
    checkDecline(f, 0xFFFFFFF8u, kBufEa, 16u, IntrinsicWhy::CrossBoundary, "dst+n > 4 GiB");
    checkDecline(f, kBufEa, 0xFFFFFFF8u, 16u, IntrinsicWhy::CrossBoundary, "src+n > 4 GiB");
    checkDecline(f, 0xFFFFFFFFu, kBufEa, 2u, IntrinsicWhy::CrossBoundary, "dst+n wraps");
    // 未提交页：`GuestLayout` 的 arena 上界之外（与 `tests/rep_bulk.cpp` 同一份判据来源）。
    const lift::GuestLayout layout;
    const std::uint32_t uncommitted = layout.arena_base + layout.arena_size;
    LIFT_CHECK(!f.mem().committed(uncommitted, 16u));
    checkDecline(f, uncommitted, kBufEa, 16u, IntrinsicWhy::Uncommitted,
                 "未提交页（dst）没被 decline");
    checkDecline(f, kBufEa, uncommitted, 16u, IntrinsicWhy::Uncommitted,
                 "未提交页（src）没被 decline");
    // `cpu.mem == nullptr`：装配错误（⛔ 不许当成功）。
    lift::X86 bare;
    bare.mem = nullptr;
    LIFT_CHECK(lift::memcpyGatePlan(bare, kBufEa, kBufEa, 4u).why == IntrinsicWhy::NoMemory);
    LIFT_CHECK(!lift_intrinsic_memcpy_gate(&bare, nullptr, kBufEa, kBufEa, 4u));
    // `--no-intrinsics`：n == 0 也 decline（A/B 必须逐位可比）。
    checkDecline(f, kBufEa, kBufEa, 0u, IntrinsicWhy::Disabled, "n==0 but --no-intrinsics", false);
    checkDecline(f, kBufEa, kBufEa, 4u, IntrinsicWhy::Disabled, "--no-intrinsics", false);
    // n == 0 且 dst/src 是垃圾地址：仍然 Ok（一个字节都不碰）。
    lift::setIntrinsicsEnabled(true);
    const lift::MemcpyGatePlan zero =
        lift::memcpyGatePlan(f.cpu(), 0xFFFFFFFFu, 0xFFFFFFF0u, 0u);
    LIFT_CHECK(zero.why == IntrinsicWhy::Ok);
}

// ③ 生成物端到端（只在 `LIFT_INTRINSICS_GEN` 下编译 / 运行）。
void generatedEndToEnd(Fixture& f) {
#if defined(LIFT_INTRINSICS_GEN)
    lift::X86& cpu = f.cpu();
    // 与 AOT trampoline 同形态：按 `EA → 函数` 派发，直到回到调用者的返回地址（`kRetEa`）。
    const auto runGuest = [&]() -> std::uint32_t {
        std::uint32_t ea = kMemcpyEa;
        for (int step = 0; step < 4096; ++step) {
            if (ea == kRetEa) return ea;
            const lift::LiftFn fn = fixtureLookup(ea);
            if (fn == nullptr) return ea;  // 不在表里 = 这就是调用者栈上的返回 EA
            ea = fn(lift::liftRegsView(cpu), f.mem().host(0), ea, &cpu);
        }
        return ea;
    };
    const std::uint32_t dst = kBufEa + 0x900u;
    const std::uint32_t src = kBufEa + 0x1900u;
    const std::uint32_t n = 7u;

    auto armCall = [&](std::uint32_t d, std::uint32_t s, std::uint32_t len) {
        f.resetPattern();
        cpu.eax() = 0xAAAAAAAAu;
        cpu.ecx() = 0xCCCCCCCCu;
        cpu.edx() = 0xDDDDDDDDu;
        cpu.ebx() = 0x11111111u;
        cpu.esp() = kStackEa;
        cpu.ebp() = 0x22222222u;
        cpu.esi() = 0x33333333u;
        cpu.edi() = 0x44444444u;
        cpu.eflags = 0u;  // DF=0
        cpu.insns = 0;
        f.mem().wr32(kStackEa, kRetEa);
        f.mem().wr32(kStackEa + 4u, d);
        f.mem().wr32(kStackEa + 8u, s);
        f.mem().wr32(kStackEa + 12u, len);
    };

    // ---- 闸门 Ok：快路径 ----
    armCall(dst, src, n);
    const std::vector<std::uint8_t> before(f.mem().host(kAreaEa), f.mem().host(kAreaEa) + kAreaSize);
    const std::uint32_t ebx0 = cpu.ebx(), esi0 = cpu.esi(), edi0 = cpu.edi(), ebp0 = cpu.ebp();
    lift::setIntrinsicsEnabled(true);
    lift::resetIntrinsicCounters();
    const std::uint32_t ret =
        runGuest();
    LIFT_CHECK_EQ_U32(ret, kRetEa);
    LIFT_CHECK_EQ_U32(cpu.eax(), dst);
    LIFT_CHECK_EQ_U32(cpu.esp(), kStackEa + 4u);
    LIFT_CHECK_EQ_U32(cpu.ebx(), ebx0);
    LIFT_CHECK_EQ_U32(cpu.esi(), esi0);
    LIFT_CHECK_EQ_U32(cpu.edi(), edi0);
    LIFT_CHECK_EQ_U32(cpu.ebp(), ebp0);
    LIFT_CHECK_EQ_U32(cpu.eflags & 0x400u, 0u);  // DF = 0
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::intrinsicCounters().fast), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cpu.insns), 0);  // 快路径一条 guest 指令都不记账
    const std::vector<std::uint8_t> want = referenceMove(before, dst, src, n);
    LIFT_CHECK(std::memcmp(want.data(), f.mem().host(kAreaEa), kBufSize) == 0);
    const RegSnap fast = snap(cpu);

    // ---- `--no-intrinsics`：原 lifted 函数体 ----
    armCall(dst, src, n);
    lift::setIntrinsicsEnabled(false);
    lift::resetIntrinsicCounters();
    const std::uint32_t ret_slow =
        runGuest();
    LIFT_CHECK_EQ_U32(ret_slow, kRetEa);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::intrinsicCounters().disabled), 1);
    LIFT_CHECK(cpu.insns > 0);  // 原函数体真的执行了（⛔ 不是"快路径假关"）
    LIFT_CHECK_EQ_U32(cpu.eax(), fast.r[0]);
    LIFT_CHECK_EQ_U32(cpu.esp(), fast.r[4]);
    LIFT_CHECK_EQ_U32(cpu.ebx(), fast.r[3]);
    LIFT_CHECK_EQ_U32(cpu.ebp(), fast.r[5]);
    LIFT_CHECK_EQ_U32(cpu.esi(), fast.r[6]);
    LIFT_CHECK_EQ_U32(cpu.edi(), fast.r[7]);
    LIFT_CHECK(std::memcmp(want.data(), f.mem().host(kAreaEa), kBufSize) == 0);

    // ---- 应 decline 的 n=17：开/关闸门必须都走原函数体，逐位相同（含 ECX/EDX/EFLAGS/insns） ----
    const std::uint32_t n17 = 17u;
    armCall(dst, src, n17);
    lift::setIntrinsicsEnabled(true);
    const std::uint32_t ret_decl =
        runGuest();
    const RegSnap decl = snap(cpu);
    const std::vector<std::uint8_t> decl_mem(f.mem().host(kAreaEa), f.mem().host(kAreaEa) + kAreaSize);
    LIFT_CHECK_EQ_U32(ret_decl, kRetEa);

    armCall(dst, src, n17);
    lift::setIntrinsicsEnabled(false);
    const std::uint32_t ret_off =
        runGuest();
    LIFT_CHECK_EQ_U32(ret_off, kRetEa);
    LIFT_CHECK(regsEqual(decl, snap(cpu)));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(decl.insns), static_cast<std::int64_t>(cpu.insns));
    LIFT_CHECK(std::memcmp(decl_mem.data(), f.mem().host(kAreaEa), kBufSize) == 0);

    // ---- n=0 快路径：一个字节都不写；EAX=dst；ESP+=4 ----
    armCall(dst, src, 0u);
    lift::setIntrinsicsEnabled(true);
    const std::vector<std::uint8_t> before0(f.mem().host(kAreaEa), f.mem().host(kAreaEa) + kAreaSize);
    const std::uint32_t ret0 =
        runGuest();
    LIFT_CHECK_EQ_U32(ret0, kRetEa);
    LIFT_CHECK_EQ_U32(cpu.eax(), dst);
    LIFT_CHECK_EQ_U32(cpu.esp(), kStackEa + 4u);
    LIFT_CHECK(std::memcmp(before0.data(), f.mem().host(kAreaEa), kBufSize) == 0);
    lift::setIntrinsicsEnabled(true);
#endif
}

}  // namespace

int main() {
    Fixture f;
    if (!f.ok) {
        LIFT_SKIP(("fixture init failed: " + f.err).c_str());
    }
    lift::setIntrinsicsEnabled(true);
    contractMatrix(f);
    declineMatrix(f);
#if defined(LIFT_INTRINSICS_GEN)
    generatedEndToEnd(f);
#else
    // 缺 clang / 缺 PE：① ② 已跑完（它们不需要生成物）；③ 跳过。
    std::printf("SKIP: generated end-to-end (no clang / no execution source at configure time)\n");
#endif
    return lift_test::finish("lift-intrinsics");
}
