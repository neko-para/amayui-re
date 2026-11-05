// emulator-lift · Unicorn CPU 后端（实验 POC）夹具
//
// ★ 注册位置：CMakeLists 的 `if(LIFT_ENABLE_UNICORN)` 里 ⇒ **默认构建（out）不注册这条测试**，
//   默认 `ctest` 仍是 41/41；只有 `out`（macos-arm64 preset）才有它。
// ★ 判据（每条都独立会红）：
//   ① guest 代码真的在 Unicorn 里执行，结果能读回来；**同一条字节序列**在 interp 上给出同样的
//      寄存器 / 指令数（interp 是 reference）；
//   ② 一次**已有的** shim 调用走通：桩地址 → UC_HOOK_CODE 停在执行前 → `ShimRegistry::invoke`
//      （与解释器同一个函数）→ 回 guest，EAX / ESP / call_count 都对；
//   ③ fault **不是静默**：读未提交页 ⇒ `StopReason::MemoryFault`，detail 带 EA；
//   ④ `--max-insns` **精确**（Unicorn 的计数 hook）：停在下一条要执行的指令上，与 interp 同 eip；
//   ⑤ `until_ea` 停在指定 EA，且它前面的指令真的执行了、后面的没有；
//   ⑥ x87 / XMM 在每个切片边界**双向同步**（x87 与 interp 对照；XMM 只有 Unicorn 有 ——
//      解释器还没实现 `movss`，⛔ 这里不假装两边一样）。
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/cpu_unicorn.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kCodeEa = 0x00401000u;
constexpr std::uint32_t kDataEa = 0x00410000u;
constexpr std::uint32_t kStubPage = 0x7FF80000u;  // 与 tests/mechanism.cpp 同一个裸夹具桩页
constexpr std::uint32_t kStubSlots = 0x1000u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;  // 栈顶返回地址哨兵（跑到它 = 正常结束）

// 只做本文件要用的几条编码；位移由 `at()` 算，⛔ 不手填
struct Asm {
    std::vector<std::uint8_t> b;
    std::uint32_t at(std::uint32_t base) const { return base + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void movEax(std::uint32_t v) {
        u8(0xB8);
        u32(v);
    }
    void movEbx(std::uint32_t v) {
        u8(0xBB);
        u32(v);
    }
    void movEcx(std::uint32_t v) {
        u8(0xB9);
        u32(v);
    }
    void addEaxEbx() {
        u8(0x01);
        u8(0xD8);
    }
    void incEax() { u8(0x40); }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void callRel(std::uint32_t base, std::uint32_t target) {
        u8(0xE8);
        u32(target - (at(base) + 4u));
    }
    void movEaxMem(std::uint32_t ea) {
        u8(0xA1);
        u32(ea);
    }
    // mov eax, dword ptr fs:[0]（A1 + fs: 前缀。moffs 形式，方便直接验段基址）
    void movEaxFs0() {
        u8(0x64);
        u8(0xA1);
        u32(0);
    }
    void fld1() {
        u8(0xD9);
        u8(0xE8);
    }
    // movss xmm0, dword ptr [ea]（F3 0F 10 05 cd）——★ 解释器还没实现它，所以只有 Unicorn 会跑这条
    void movssXmm0Mem(std::uint32_t ea) {
        u8(0xF3);
        u8(0x0F);
        u8(0x10);
        u8(0x05);
        u32(ea);
    }
    void ret() { u8(0xC3); }
};

struct Rig {
    lift::Machine m;
    std::string err;
    bool ok = true;

    bool init() {
        if (!m.initBare(&err)) return false;
        if (!m.prepareStack(&err)) return false;
        if (!m.heap().init(&m.mem(), m.layout().heap_base, 0x00100000u)) return false;
        m.shims().configureStubPage(kStubPage, kStubSlots);
        // ★ Unicorn 会**真的 fetch** 桩地址上那条指令（在 code hook 里停下之前要翻译它），
        //   所以桩页必须提交；解释器把桩当块缓存条目、不需要内存。两边给同一份，便于对照。
        return m.mem().commitZeroed(kStubPage, 0x1000u, lift::RegionKind::ShimPage, "fixture-shim");
    }
    void place(std::uint32_t ea, const Asm& a) {
        if (!m.placeCode(ea, a.b.data(), a.b.size(), &err)) ok = false;
    }
    std::uint32_t putData(const void* p, std::size_t n) {
        if (!m.placeData(kDataEa, p, n, &err)) ok = false;
        return kDataEa;
    }
    // 每次开跑前把夹具栈 / 指令计数 / eip 复位（上一段把哨兵弹掉了）
    void reset(std::uint32_t code_ea) {
        m.cpu().running = true;
        m.cpu().eip = code_ea;
        m.cpu().insns = 0;
        // x87 / XMM 也复位：两个后端跑**同一段夹具**时初值必须一致，否则 top 会叠加两次
        m.cpu().fpu.reset();
        std::memset(m.cpu().xmm, 0, sizeof(m.cpu().xmm));
        m.cpu().esp() = m.layout().stack_top - 4u;
        m.mem().wr32(m.cpu().esp(), kSentinel);
    }
    lift::RunResult run(lift::CpuBackend backend, std::uint32_t until, std::uint64_t max_insns = 0) {
        lift::RunOptions o;
        o.cpu = backend;
        o.until_ea = until;
        o.max_insns = max_insns;
        return m.run(o);
    }
};

// ① guest 代码执行 + 两个后端同结果
void caseGuestCode() {
    lift_test::g_current_case = "unicorn/guest-code";
    Rig r;
    LIFT_CHECK(r.init());
    if (!r.ok) return;
    Asm a;
    a.movEax(42);
    a.movEbx(7);
    a.addEaxEbx();
    a.ret();
    r.place(kCodeEa, a);
    for (const lift::CpuBackend backend : {lift::CpuBackend::Interp, lift::CpuBackend::Unicorn}) {
        r.reset(kCodeEa);
        const lift::RunResult res = r.run(backend, kSentinel);
        LIFT_CHECK(res.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(res.ea, kSentinel);
        LIFT_CHECK_EQ_U32(r.m.cpu().eax(), 49u);
        LIFT_CHECK_EQ_U32(r.m.cpu().ebx(), 7u);
        LIFT_CHECK(res.cpu == backend);
        LIFT_CHECK(res.insns_counted);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(res.insns), 4);  // mov/mov/add/ret
    }
}

// ② 一次已有的 shim 调用（kernel32!lstrlenA）：桩地址 → hook 停 → invoke → 回 guest
void caseShimCall() {
    lift_test::g_current_case = "unicorn/shim-call";
    Rig r;
    LIFT_CHECK(r.init());
    if (!r.ok) return;
    lift::registerStringShims(r.m.shims());
    lift::ShimDef* def = r.m.shims().findMutable("kernel32", "lstrlenA");
    LIFT_CHECK(def != nullptr);
    if (!def) return;
    const std::uint32_t stub = kStubPage + 0x100u;  // 桩页里的一个槽（避开页首，验区间 hook）
    def->stub_ea = stub;
    r.m.cache().installShim(stub, def->id);  // 解释器需要；Unicorn 不需要（它按 stub_ea 认）
    const char* text = "hello";
    const std::uint32_t str_ea = r.putData(text, 6);
    Asm a;
    a.pushImm(str_ea);
    a.callRel(kCodeEa, stub);
    a.ret();
    r.place(kCodeEa, a);
    for (const lift::CpuBackend backend : {lift::CpuBackend::Interp, lift::CpuBackend::Unicorn}) {
        r.reset(kCodeEa);
        def->call_count = 0;
        const lift::RunResult res = r.run(backend, kSentinel);
        LIFT_CHECK(res.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(r.m.cpu().eax(), 5u);  // lstrlenA("hello") = 5
        // push 参数 + call（压返回地址）→ invoke（弹返回地址 + stdcall 弹参）→ guest ret（弹哨兵）
        LIFT_CHECK_EQ_U32(r.m.cpu().esp(), r.m.layout().stack_top);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(def->call_count), 1);
        LIFT_CHECK_EQ_U32(def->stub_ea, stub);
    }
}

// ③ 未提交页的读 ⇒ MemoryFault（带 EA），⛔ 不静默、⛔ 不返回 0
void caseMemoryFault() {
    lift_test::g_current_case = "unicorn/memory-fault";
    Rig r;
    LIFT_CHECK(r.init());
    if (!r.ok) return;
    constexpr std::uint32_t kUncommitted = 0x58000000u;  // 不落在 image/heap/stack/shim/surface 任何区域
    Asm a;
    a.movEaxMem(kUncommitted);
    a.ret();
    r.place(kCodeEa, a);
    r.reset(kCodeEa);
    const lift::RunResult res = r.run(lift::CpuBackend::Unicorn, kSentinel);
    LIFT_CHECK(res.reason == lift::StopReason::MemoryFault);
    LIFT_CHECK(!res.ok());
    LIFT_CHECK(res.detail.find("58000000") != std::string::npos);
    LIFT_CHECK(res.detail.find("READ") != std::string::npos);
    std::printf("  memory-fault detail: %s\n", res.detail.c_str());
}

// ④ --max-insns 精确（计数 hook）：与 interp 同 eip / 同 insns / 同寄存器
void caseMaxInsns() {
    lift_test::g_current_case = "unicorn/max-insns";
    Rig r;
    LIFT_CHECK(r.init());
    if (!r.ok) return;
    Asm a;
    a.movEax(0);
    a.incEax();
    a.incEax();
    a.incEax();
    a.ret();
    r.place(kCodeEa, a);
    std::uint32_t interp_eip = 0;
    std::uint64_t interp_insns = 0;
    for (const lift::CpuBackend backend : {lift::CpuBackend::Interp, lift::CpuBackend::Unicorn}) {
        r.reset(kCodeEa);
        const lift::RunResult res = r.run(backend, 0, 3);
        LIFT_CHECK(res.reason == lift::StopReason::MaxInsns);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(res.insns), 3);
        LIFT_CHECK_EQ_U32(r.m.cpu().eax(), 2u);           // 只跑了两条 inc
        LIFT_CHECK_EQ_U32(r.m.cpu().eip, kCodeEa + 7u);   // 停在第 4 条（第 3 个 inc）
        if (backend == lift::CpuBackend::Interp) {
            interp_eip = r.m.cpu().eip;
            interp_insns = res.insns;
        } else {
            LIFT_CHECK_EQ_U32(r.m.cpu().eip, interp_eip);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(res.insns), static_cast<std::int64_t>(interp_insns));
        }
    }
}

// ⑤ until_ea：前面的指令真的执行了，后面的没有
void caseUntilEa() {
    lift_test::g_current_case = "unicorn/until-ea";
    Rig r;
    LIFT_CHECK(r.init());
    if (!r.ok) return;
    Asm a;
    a.movEax(1);
    a.movEbx(2);
    a.movEcx(3);
    a.ret();
    r.place(kCodeEa, a);
    const std::uint32_t until = kCodeEa + 10u;  // 第三条（mov ecx,3）的地址
    for (const lift::CpuBackend backend : {lift::CpuBackend::Interp, lift::CpuBackend::Unicorn}) {
        r.reset(kCodeEa);
        const lift::RunResult res = r.run(backend, until);
        LIFT_CHECK(res.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(res.ea, until);
        LIFT_CHECK_EQ_U32(r.m.cpu().eax(), 1u);
        LIFT_CHECK_EQ_U32(r.m.cpu().ebx(), 2u);
        LIFT_CHECK_EQ_U32(r.m.cpu().ecx(), 0u);
    }
}

// ⑦ FS 段基址（TEB）：`mov eax, fs:[0]` 必须取到 FS base 指向的那一页
//   ★ 这是 32 位 Windows guest 的**命门**：Unicorn 2.1.4 在 32 位模式下**不支持**写 FS_BASE
//     （qemu/target/i386/unicorn.c 里那两行被注释掉了），后端改用 GDT 描述子 + 选择子装载。
//     本用例就是钉住这条：两个后端都必须读到 TEB 里的标记。
void caseFsBase() {
    lift_test::g_current_case = "unicorn/fs-base";
    Rig r;
    LIFT_CHECK(r.init());
    if (!r.ok) return;
    constexpr std::uint32_t kTebEa = 0x7FFD0000u;  // 与 GuestLayout::teb_ea 一致
    LIFT_CHECK(r.m.mem().commitZeroed(kTebEa, 0x1000u, lift::RegionKind::Teb, "fixture-teb"));
    constexpr std::uint32_t kMarker = 0x5A5AA5A5u;
    r.m.mem().wr32(kTebEa, kMarker);
    Asm a;
    a.movEaxFs0();
    a.ret();
    r.place(kCodeEa, a);
    for (const lift::CpuBackend backend : {lift::CpuBackend::Interp, lift::CpuBackend::Unicorn}) {
        r.reset(kCodeEa);
        r.m.cpu().fs_base = kTebEa;
        const lift::RunResult res = r.run(backend, kSentinel);
        LIFT_CHECK(res.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(r.m.cpu().eax(), kMarker);
        LIFT_CHECK_EQ_U32(r.m.cpu().fs_base, kTebEa);
    }
}

// ⑥ x87 / XMM 在切片边界同步回来
//   ★ x87 与 interp 对照（interp 有 fld1）；XMM 只有 Unicorn（interp 没有 movss）
void caseFpuSync() {
    lift_test::g_current_case = "unicorn/fpu-sync";
    Rig r;
    LIFT_CHECK(r.init());
    if (!r.ok) return;
    const float f = 2.5f;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &f, 4);
    const std::uint32_t data_ea = r.putData(&bits, 4);
    Asm a;
    a.fld1();
    a.movssXmm0Mem(data_ea);
    a.ret();
    r.place(kCodeEa, a);
    for (const lift::CpuBackend backend : {lift::CpuBackend::Interp, lift::CpuBackend::Unicorn}) {
        if (backend == lift::CpuBackend::Interp) {
            // 解释器还没实现 movss（decode 里没有）⇒ 只跑 fld1 那半段做 x87 对照
            Asm x87;
            x87.fld1();
            x87.ret();
            r.place(kCodeEa, x87);
        } else {
            r.place(kCodeEa, a);  // 带 movss 的完整段（上一轮把同一 EA 覆盖成了 x87-only）
        }
        r.reset(kCodeEa);
        const lift::RunResult res = r.run(backend, kSentinel);
        LIFT_CHECK(res.reason == lift::StopReason::UntilEa);
        const lift::X86& cpu = r.m.cpu();
        // fld1 ⇒ TOP 从 0 挪到 7，物理槽 7 是 valid，值 = 1.0（extFloat80: signif=0x8000…, signExp=0x3FFF）
        LIFT_CHECK_EQ_U32(cpu.fpu.top, 7u);
        const int slot = cpu.fpu.phys(0);
        std::uint16_t sign_exp = 0;
        std::uint64_t signif = 0;
        std::memcpy(&signif, cpu.fpu.regs[slot], 8);
        std::memcpy(&sign_exp, cpu.fpu.regs[slot] + 8, 2);
        LIFT_CHECK_EQ_U32(sign_exp, 0x3FFFu);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(signif >> 32), 0x80000000u);
        LIFT_CHECK_EQ_U32((cpu.fpu.tag >> (2 * slot)) & 3u, 0u);  // 0 = valid
        LIFT_CHECK_EQ_U32(cpu.fpu.control, 0x037Fu);
        if (backend == lift::CpuBackend::Unicorn) {
            // movss 只在 Unicorn 里执行过；XMM0 的低 dword 必须是 2.5f
            LIFT_CHECK_EQ_U32(cpu.xmm[0][0], bits);
            LIFT_CHECK_EQ_U32(bits, 0x40200000u);
        }
    }
}

// ⑧ 吞吐对比：同一个二进制（out = Release）里，两个后端跑**同一段**紧循环。
//   ★ 判据不是 MIPS 数字（那会随机器变 ⇒ ⛔ 不设性能门槛），而是：① 两个后端的 insns 必须相同；
//     ② 数字能打出来（进报告用）。循环体 4 条指令：xor / add / cmp / jb。
void caseThroughput() {
    lift_test::g_current_case = "unicorn/throughput";
    Rig r;
    LIFT_CHECK(r.init());
    if (!r.ok) return;
    constexpr std::uint32_t kIter = 5000000u;
    Asm a;
    a.u8(0x31);
    a.u8(0xC0);  // xor eax,eax
    const std::uint32_t loop = a.at(kCodeEa);
    a.u8(0x83);
    a.u8(0xC0);
    a.u8(0x01);  // add eax,1
    a.u8(0x3D);
    a.u32(kIter);  // cmp eax,imm32
    a.u8(0x0F);
    a.u8(0x82);  // jb rel32
    const std::uint32_t at = a.at(kCodeEa);
    a.u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(loop) - static_cast<std::int32_t>(at + 4)));
    a.ret();
    r.place(kCodeEa, a);
    const std::uint64_t budget = static_cast<std::uint64_t>(kIter) * 4ull + 1000ull;
    std::uint64_t insns[2] = {0, 0};
    double mips[2] = {0.0, 0.0};
    int i = 0;
    for (const lift::CpuBackend backend : {lift::CpuBackend::Interp, lift::CpuBackend::Unicorn}) {
        r.reset(kCodeEa);
        const auto t0 = std::chrono::steady_clock::now();
        const lift::RunResult res = r.run(backend, kSentinel, budget);
        const auto t1 = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(t1 - t0).count();
        LIFT_CHECK(res.reason == lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(r.m.cpu().eax(), kIter);
        insns[i] = res.insns;
        mips[i] = secs > 0.0 ? static_cast<double>(res.insns) / secs / 1e6 : 0.0;
        ++i;
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(insns[0]), static_cast<std::int64_t>(insns[1]));
    std::printf("  throughput: interp=%.1f MIPS  unicorn=%.1f MIPS  (insns=%llu each)\n", mips[0], mips[1],
                static_cast<unsigned long long>(insns[0]));
}

}  // namespace

int main() {
    LIFT_CHECK(lift::unicornAvailable());
    LIFT_CHECK(lift::kUnicornCompiled);
    caseGuestCode();
    caseShimCall();
    caseMemoryFault();
    caseMaxInsns();
    caseUntilEa();
    caseFsBase();
    caseFpuSync();
    caseThroughput();
    return lift_test::finish("unicorn");
}
