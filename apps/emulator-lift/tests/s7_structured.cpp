// emulator-lift · S7（D53）的**活体判据**（CTest: lift-s7-structured）
//
// 红了意味着什么（判据）：
//   `tests/ir/s7_roots.txt` 里的调用者单元根 F 在受控状态下跑一段 **F → call G → G 返回 → F 返回**：
//     ① **一次派发表查找**就把整段跑完（`lookups == 1`）⇒ F 的 `call rel32` 是**宿主 `call`**、
//        G 的 `ret` 是**宿主 `ret`**（不是"两次派发"）。今天之前的形状必然要 ≥2 次（call 回 trampoline）。
//     ② guest 栈**逐位平衡**：返回后 `esp` 回到调用前、`[esp]` 仍是**没被写过**的栈哨兵
//        ⇒ "宿主栈上的返回地址 ↔ guest 栈上那一格返回 EA"一一对应（D53 的硬核要求 1）。
//     ③ 与解释器臂**逐位相同**（8 GPR / EFLAGS / 8 个 x87 槽 / top / tag / 状态字 / 控制字 /
//        1 KiB guest 内存）⇒ 宿主 call/ret 不改变任何 guest 可见状态。
//     ④ 自检（防空转绿）：从 F 线性解码到"`call rel32` 到 G"那一条，数出**到该站点为止**的
//        指令条数 `n_callsite`；断言 `lifted_insns > n_callsite`（真的执行过了这条 call 之后）
//        + `lookups == 1`（没有回派发器）⇒ 唯一解释是"G 在同一个宿主帧里跑完了"。
//
// ⚠ 口径（如实登记）：
//   · 这条用例**不**证明"所有函数都转成了宿主 call/ret"——覆盖率是生成期读数（`lift-gen` 的
//     `[S7 结构翻译]` 行 + `.ll` 头部），`lift-s7-coverage` 那条守卫钉的是"读数与形状一致"。
//   · 它证明的是"**判据一旦退化就红**"：把 structured 集合清零（或把 call 改回派发）⇒
//     `lookups` 变 ≥2 ⇒ 当场红（变异实测见报告）。
//   · 缺 `AGE.EXE` / clang ⇒ `LIFT_SKIP(77)`（与 `lift-x87-diff` 同一套处置）。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aot/aot_dispatch.h"
#include "check.h"
#include "runtime/decode.h"
#include "runtime/image.h"
#include "runtime/machine.h"

namespace {

// `lift_dyn` 的派发表查找（与 AOT 臂 `aot_run.cpp` 里那一个同形：都指向 `aot::lookup`）。
lift::LiftFn lookupForTest(std::uint32_t ea) { return lift::aot::lookupUncounted(ea); }

// ★ 与 `tests/ir/s7_roots.txt` 一一对应（⛔ 改一处就要改另一处；测试体跑之前会核对两者）。
//   F = 调用者（单元根）；G = 它 `call rel32` 的被调方（也是单元根 ⇒ 独立分量）。
constexpr std::uint32_t kCallerEa = LIFT_S7_CALLER_EA;
constexpr std::uint32_t kCalleeEa = LIFT_S7_CALLEE_EA;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
constexpr std::uint32_t kScratch = 0x00440000u;
constexpr std::uint32_t kScratchBytes = 0x00040000u;
constexpr std::uint32_t kMemProbe = 0x00000400u;

struct Arm {
    lift::Machine m;
    std::string err;
    bool init(const std::string& pe_path) {
        if (!m.initBare(&err) || !m.prepareStack(&err)) return false;
        lift::PeImage image;
        if (!lift::loadPeFile(pe_path, &image, &err)) return false;
        if (!lift::mapPeImage(m.mem(), image, &err)) return false;
        std::vector<std::uint8_t> zero(kScratchBytes, 0);
        return m.placeData(kScratch, zero.data(), zero.size(), &err);
    }
};

void setState(Arm& a) {
    lift::X86& cpu = a.m.cpu();
    const double vals[8] = {1.5, -2.25, 4.0, 0.0, -0.5, 1024.0, 3.5, -7.75};
    for (int i = 0; i < 8; ++i) cpu.fpu.regs[i] = vals[i];
    cpu.fpu.top = 0;
    cpu.fpu.tag = 0x0000u;
    cpu.fpu.control = 0x037Fu;
    cpu.fpu.status = 0;
    const std::uint32_t base = kScratch + 0x200u;
    cpu.eax() = base;
    cpu.ecx() = base;
    cpu.edx() = base;
    cpu.ebx() = base;
    cpu.ebp() = base;
    cpu.esi() = base;
    cpu.edi() = base;
    cpu.esp() = a.m.layout().stack_top - 0x40u;
    a.m.mem().wr32(cpu.esp(), kSentinel);  // 栈哨兵 = F 的返回地址
    cpu.eflags = 0x202u;
    cpu.running = true;
}

struct Snap {
    std::uint32_t r[8] = {};
    std::uint32_t eflags = 0;
    double fpu[8] = {};
    std::uint8_t top = 0;
    std::uint16_t tag = 0;
    std::uint16_t status = 0;
    std::uint16_t control = 0;
    std::vector<std::uint8_t> mem;
};

Snap snap(const Arm& a) {
    Snap s;
    const lift::X86& cpu = a.m.cpu();
    for (int i = 0; i < 8; ++i) s.r[i] = cpu.r[i];
    s.eflags = cpu.eflags;
    std::memcpy(s.fpu, cpu.fpu.regs, sizeof(s.fpu));
    s.top = cpu.fpu.top;
    s.tag = cpu.fpu.tag;
    s.status = cpu.fpu.status;
    s.control = cpu.fpu.control;
    const std::uint8_t* p = a.m.mem().host(kScratch);
    s.mem.assign(p, p + kMemProbe);
    return s;
}

// 自检：从 F 线性解码，找到"`call rel32` 目标 == G"的那一条；返回"到它为止执行过的条数"。
// `n_callsite` = 这条 call **之前**的条数（含函数序言）⇒ 若真的执行了它，`insns > n_callsite`。
bool findCallSite(Arm& a, std::uint32_t* n_callsite, std::uint32_t* call_ea) {
    lift::Decoder dec;
    std::uint32_t ea = kCallerEa;
    std::uint32_t n = 0;
    for (int i = 0; i < 4096; ++i) {
        lift::Insn in;
        std::string why;
        if (!dec.decode(a.m.mem(), ea, &in, &why)) return false;
        if (in.op == lift::Op::Call && in.op0().kind == lift::OpKind::Imm) {
            const std::uint32_t target = static_cast<std::uint32_t>(
                static_cast<std::int64_t>(in.ea) + in.length +
                static_cast<std::int64_t>(static_cast<std::int32_t>(
                    static_cast<std::uint32_t>(in.op0().imm))));
            if (target == kCalleeEa) {
                *n_callsite = n;
                *call_ea = in.ea;
                return true;
            }
        }
        // 线性扫描到 `ret` 就停（说明这条 call 不在直线路径上 ⇒ 自检失败，⛔ 不静默）
        if (in.op == lift::Op::Ret) return false;
        ++n;
        ea = in.ea + in.length;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    (void)argc;
    (void)argv;

#if !defined(LIFT_S7_STRUCTURED)
    std::printf("lift-s7-structured: 没有生成物（缺 clang / 缺 AGE.EXE）⇒ SKIP\n");
    return 77;
#else
    const std::string pe_path = LIFT_S7_PE_PATH;
    g_current_case = "s7-structured";
    // `lift_dyn` 的派发表查找（生成物里若含间接 jmp/call 站点就要它；与 AOT 臂同一个来源）。
    lift::liftDynLookup = &lookupForTest;    LIFT_CHECK(lift::aot::lookup(kCallerEa, nullptr) != nullptr);
    LIFT_CHECK(lift::aot::lookup(kCalleeEa, nullptr) != nullptr);

    Arm li;
    Arm ia;
    if (!li.init(pe_path) || !ia.init(pe_path)) {
        lift_test::failf(__FILE__, __LINE__, "夹具初始化失败：%s / %s", li.err.c_str(), ia.err.c_str());
        return finish("lift-s7-structured");
    }
    // ---- 自检：F 里真的有一条 `call rel32 G`（且到它的指令数可知）----
    std::uint32_t n_callsite = 0;
    std::uint32_t call_ea = 0;
    if (!findCallSite(ia, &n_callsite, &call_ea)) {
        lift_test::failf(__FILE__, __LINE__,
                         "自检失败：从 %08X 线性解码没找到 `call rel32 %08X`（根集写错？）", kCallerEa,
                         kCalleeEa);
        return finish("lift-s7-structured");
    }
    setState(li);
    setState(ia);
    const std::uint32_t esp_before = li.m.cpu().esp();

    // ---- ① lifted 臂：**一次**表查找跑完 F → G → 返回 ----
    LiftRegs regs{};
    lift::liftCpuToRegs(li.m.cpu(), regs);
    std::uint8_t* mem = li.m.mem().host(0);
    const std::uint64_t insns0 = li.m.cpu().insns;
    std::uint32_t ea = kCallerEa;
    int lookups = 0;
    while (lookups < 64) {
        lift::LiftFn fn = lift::aot::lookup(ea, nullptr);
        if (fn == nullptr) break;
        ++lookups;
        ea = fn(&regs, mem, ea, &li.m.cpu());
        if (!li.m.cpu().running) break;
        // 停止条件：回到"表外 EA"（= 栈哨兵 / 派发出口）。这里**不允许**再发生一次派发。
        if (lift::aot::lookup(ea, nullptr) == nullptr) break;
    }
    lift::liftRegsToCpu(regs, li.m.cpu());
    const std::uint64_t lifted_insns = li.m.cpu().insns - insns0;
    const std::uint32_t esp_after = li.m.cpu().esp();
    const std::uint32_t slot_after = li.m.mem().rd32(esp_before);
    // ★ `call` 必须把 **guest 返回 EA** 写进 `%mem[esp-4]`（guest 代码会读 `[esp]`）：
    //   F 的 call 压的是 0x4C0985（F 的第二块入口）。G 的 `ret` 读它、不写它 ⇒ 这里仍应是它。
    const std::uint32_t pushed_resume = li.m.mem().rd32(esp_before - 4u);

    // ---- ② 解释器臂：同一份状态、同样条数 ----
    const std::uint64_t interp0 = ia.m.cpu().insns;
    lift::RunOptions o;
    o.max_insns = interp0 + lifted_insns;
    ia.m.cpu().eip = kCallerEa;
    (void)ia.m.run(o);
    const std::uint64_t interp_insns = ia.m.cpu().insns - interp0;

    // ---- ③ 读数（判据的可见面；⛔ 不是"日志"）----
    std::printf(
        "lift-s7-structured: F=%08X G=%08X call@%08X（到它为止 %u 条）· 表查找=%d · insns=%llu · "
        "esp %08X→%08X · [esp]=%08X（哨兵 %08X）· [esp-4]=%08X（压入的 resume）\n",
        kCallerEa, kCalleeEa, call_ea, n_callsite, lookups,
        static_cast<unsigned long long>(lifted_insns), esp_before, esp_after, slot_after, kSentinel,
        pushed_resume);

    // ---- ④ 判据 ----
    LIFT_CHECK(lifted_insns >= 1u);                 // 两臂都真的跑了
    LIFT_CHECK(lookups == 1);                       // ① 一次表查找跑完 ⇒ 宿主 call + 宿主 ret
    LIFT_CHECK(lifted_insns > n_callsite);          // ④ 真的越过了 call 站点（不是提前 ret）
    // ② guest 栈与宿主栈一一对应：
    //   · F 的 `call` 把 resume 压到 `esp_before-4`（guest 代码会读 `[esp]`）⇒ 那一格仍是 resume；
    //   · G 的 `ret` 弹的正是那一格（⇒ 它回到 F 的 resume，见 `lookups == 1`）；
    //   · F 自己的 `ret` 弹的是**我们放的栈哨兵** ⇒ esp 比调用前**多 4**（返回值那一格被弹出）。
    //   ⛔ 不要断言 `esp_after == esp_before`：那会把"ret 弹掉返回地址"这件事算错。
    LIFT_CHECK_EQ_U32(esp_after, esp_before + 4u);
    LIFT_CHECK_EQ_U32(pushed_resume, kCallerEa + 5u);  // = 0x4C0985（F 的 resume 块）
    LIFT_CHECK_EQ_U32(slot_after, kSentinel);       // 返回槽没被写过（哨兵原样）
    LIFT_CHECK_EQ_U32(ea, kSentinel);               // F 的 ret 弹到的是栈哨兵
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lifted_insns),
                      static_cast<std::uint32_t>(interp_insns));  // 两臂条数一致
    const Snap sl = snap(li);
    const Snap si = snap(ia);
    for (int i = 0; i < 8; ++i) LIFT_CHECK_EQ_U32(sl.r[i], si.r[i]);
    LIFT_CHECK_EQ_U32(sl.eflags, si.eflags);
    for (int i = 0; i < 8; ++i) {
        std::uint64_t lb = 0;
        std::uint64_t ib = 0;
        std::memcpy(&lb, &sl.fpu[i], 8);
        std::memcpy(&ib, &si.fpu[i], 8);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lb ^ ib), 0u);
    }
    LIFT_CHECK_EQ_U32(sl.top, si.top);
    LIFT_CHECK_EQ_U32(sl.tag, si.tag);
    LIFT_CHECK_EQ_U32(sl.status, si.status);
    LIFT_CHECK_EQ_U32(sl.control, si.control);
    if (sl.mem != si.mem) lift_test::failf(__FILE__, __LINE__, "guest 内存区前 %u 字节不同",
                                           static_cast<unsigned>(kMemProbe));
    g_current_case = "(none)";
    return finish("lift-s7-structured");
#endif
}
