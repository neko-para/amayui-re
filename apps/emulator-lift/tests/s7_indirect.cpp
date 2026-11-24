// emulator-lift · S7（D53）**间接 `call` 放宽**（D4）的**形状级差分**（CTest: lift-s7-indirect）
//
// 红了意味着什么（判据）：
//   ① **生成期**：`tests/ir/s7_indirect_roots.txt` 的 F 必须被判为 **structured**。
//      读的是**生成那一趟**的 stderr（构建期由 `tests/cmake/lift_gen_capture.cmake` 落到
//      `s7indirect/criteria.txt`）：判据回退成"有间接 `call` 就判死"⇒ 那里会出现
//      `[S7 downgrade] <F> reason=间接 call …`，`structurable` 也会从 2 掉到 1 ⇒ 红。
//      ⛔ 不要把它改成"只要测试能跑完就算绿"：放宽改的正是**判据**，这里必须钉判据。
//   ② **运行期**：F 在受控状态下跑
//        F（0x452FA0）：… `call ecx`（目标 = G）… `pop esi` / `mov esp,ebp` / `pop ebp` / `retn 4`
//        G（0x47CC80）：`xor eax,eax` / `retn 8`   ← **`ret imm16` 清 8 字节**
//      而 F 在调用点只压了 **1 个实参**（4 字节）⇒ callee 比 cdecl 口径**多清 4 字节**。
//      ⇒ 运行期必须与解释器（`Machine`，`tests/*` 的语义 oracle）臂**逐位相同**：
//        8 GPR / EFLAGS / 出口 EIP / 栈窗 / 数据窗 / ESP，
//        且 F 的 `retn 4` 弹回的返回地址 = 栈哨兵（栈上那一格从没被写过）。
//   ③ **反 vacuity**：间接 `call` 真的走了 `lift_dyn`（`lift_dyn lookups == 1`）——
//      ⛔ 否则"F 根本没走到那条路"或"两臂都在解释器里跑"都会让 ② 恒绿。
//
// ★ 形状为什么是"放宽后才存在"：F 的静态 esp 数据流在间接 `call` 之后**不可知**
//   （callee 的 `ret imm16` 字节数运行期才知道），只有"函数自己用可跟踪的 `mov esp,ebp`
//   把 esp 重建回入口值"才允许放行。判据回退 ⇒ F 不是 structured ⇒ ① 红（先红后绿的"红"）。
// ★ 为什么选这两个 EA（可复核）：
//   · F `0x452FA0`：`push ebp / mov ebp,esp` 标准帧；两条间接 `call`（`call ecx` / `call edx`）；
//     两条出口都以 `mov esp,ebp` 恢复帧，最后 `retn 4`；**没有静态 callee**（不会把传染面卷进来）。
//   · G `0x47CC80`：2 条指令的函数（`xor eax,eax / retn 8`），无内存访问、无副作用 ⇒ 差分只看
//     "调用点 + 帧恢复 + ret imm16 的栈效果"，不掺别的。
// ★ 缺 clang / 缺执行源 ⇒ 不链生成物、不定义 `LIFT_S7_INDIRECT` ⇒ 测试体返回 77（SKIP）。
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>
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

// ★ 与 `tests/ir/s7_indirect_roots.txt` 一一对应（⛔ 改一处就要改另一处）。
constexpr std::uint32_t kCallerEa = LIFT_S7_INDIRECT_CALLER_EA;
constexpr std::uint32_t kCalleeEa = LIFT_S7_INDIRECT_CALLEE_EA;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
// F 的 `this`：`[+4]` 非零（走 loc_452FB6 那条路）、`[+8]` / `[+0Ch]` = 两条间接 call 的目标。
constexpr std::uint32_t kStruct = 0x00440000u;
constexpr std::uint32_t kStructBytes = 0x100u;
constexpr std::uint32_t kStructProbe = 0x40u;
// F 的 `arg_0`：进入前 `[ebp-4]` 被写成 1，调用后 `cmp [ebp-4],arg_0` ⇒ 必须相等才走退出块
// （= 只执行**一个**间接 call；第二条 `call edx` 在另一条路上，静态分析仍然看得到它）。
constexpr std::uint32_t kArg0 = 1u;
constexpr std::uint32_t kStackProbe = 0x80u;

struct Arm {
    lift::Machine m;
    std::string err;
    bool init(const std::string& pe_path) {
        if (!m.initBare(&err) || !m.prepareStack(&err)) return false;
        lift::PeImage image;
        if (!lift::loadPeFile(pe_path, &image, &err)) return false;
        if (!lift::mapPeImage(m.mem(), image, &err)) return false;
        std::vector<std::uint8_t> zero(kStructBytes, 0);
        return m.placeData(kStruct, zero.data(), zero.size(), &err);
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
    cpu.r[0] = 0x0BADF00Du;  // EAX
    cpu.r[1] = kStruct;      // ECX = this（F 的 `mov esi,ecx`）
    cpu.r[2] = 0x0D15EA5Eu;  // EDX
    cpu.r[3] = 0x11111111u;  // EBX
    cpu.r[4] = a.m.layout().stack_top - 0x100u;  // ESP
    cpu.r[5] = 0x22222222u;  // EBP（F 自己重建）
    cpu.r[6] = 0x33333333u;  // ESI
    cpu.r[7] = 0x44444444u;  // EDI
    cpu.eflags = 0x202u;
    cpu.running = true;
    // 夹具数据：`[+4]` 非零、两条间接 call 的目标都指向 G。
    a.m.mem().wr32(kStruct + 4u, 1u);
    a.m.mem().wr32(kStruct + 8u, kCalleeEa);
    a.m.mem().wr32(kStruct + 0x0Cu, kCalleeEa);
    // 入口帧：[ESP] = 返回地址（哨兵），[ESP+4] = arg_0。
    a.m.mem().wr32(cpu.r[4], kSentinel);
    a.m.mem().wr32(cpu.r[4] + 4u, kArg0);
}

struct Snap {
    std::uint32_t r[8] = {};
    std::uint32_t eflags = 0;
    std::uint32_t esp = 0;
    std::vector<std::uint8_t> mem;   // 栈窗
    std::vector<std::uint8_t> data;  // 夹具数据窗
};

Snap snap(const Arm& a, std::uint32_t esp_before) {
    Snap s;
    const lift::X86& cpu = a.m.cpu();
    for (int i = 0; i < 8; ++i) s.r[i] = cpu.r[i];
    s.eflags = cpu.eflags;
    s.esp = cpu.r[4];
    const std::uint8_t* st = a.m.mem().host(esp_before - kStackProbe / 2u);
    s.mem.assign(st, st + kStackProbe);
    const std::uint8_t* dt = a.m.mem().host(kStruct);
    s.data.assign(dt, dt + kStructProbe);
    return s;
}

// 自检：从 F 的入口走一遍块图，找到**第一条间接 call**，给出它的 EA 与 resume EA。
// （F 的间接 call 在 `jnz` 之后，线性扫描看不到 ⇒ 按 `jcc`/`jmp` 的静态目标走。）
bool scanIndirectCall(Arm& a, std::uint32_t* site, std::uint32_t* resume) {
    lift::Decoder dec;
    std::vector<std::uint32_t> work{kCallerEa};
    std::set<std::uint32_t> seen;
    int steps = 0;
    while (!work.empty() && steps < 4096) {
        std::uint32_t ea = work.back();
        work.pop_back();
        if (ea < kCallerEa || ea > kCallerEa + 0x200u) continue;  // 只在本单元的字节范围内找
        if (!seen.insert(ea).second) continue;
        for (int i = 0; i < 512; ++i) {
            lift::Insn in;
            std::string why;
            if (!dec.decode(a.m.mem(), ea, &in, &why)) return false;
            ++steps;
            if (in.op == lift::Op::Call && in.op0().kind != lift::OpKind::Imm) {
                *site = in.ea;
                *resume = in.ea + in.length;
                return true;
            }
            if (in.op == lift::Op::Ret) break;
            if (in.op == lift::Op::Jmp || in.op == lift::Op::Jcc) {
                if (in.op0().kind == lift::OpKind::Imm) {
                    const std::uint32_t target = static_cast<std::uint32_t>(
                        static_cast<std::int64_t>(in.ea) + in.length +
                        static_cast<std::int64_t>(static_cast<std::int32_t>(
                            static_cast<std::uint32_t>(in.op0().imm))));
                    work.push_back(target);
                }
                if (in.op == lift::Op::Jcc) work.push_back(in.ea + in.length);
                break;
            }
            ea = in.ea + in.length;
        }
    }
    return false;
}

// `criteria.txt` 的一行是否包含 `needle`；命中时把整行抄进 `out`（失败诊断用）。
bool findLine(const std::string& text, const std::string& needle, std::string* out) {
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t nl = text.find('\n', at);
        const std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        if (line.find(needle) != std::string::npos) {
            if (out != nullptr) *out = line;
            return true;
        }
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace lift_test;
    (void)argc;
    (void)argv;

#if !defined(LIFT_S7_INDIRECT)
    std::printf("lift-s7-indirect: 没有生成物（缺 clang / 缺 AGE.EXE）⇒ SKIP\n");
    return 77;
#else
    const std::string pe_path = LIFT_S7_INDIRECT_PE_PATH;
    const std::string criteria_path = LIFT_S7_INDIRECT_CRITERIA;
    g_current_case = "s7-indirect";

    // =================================================================================
    // ① 生成期判据：F 必须被判为 structured（读**生成那一趟**的 stderr）
    // =================================================================================
    std::ifstream in(criteria_path, std::ios::binary);
    if (!in) {
        lift_test::failf(__FILE__, __LINE__, "打不开生成期读数 `%s`（构建产物缺失？）",
                         criteria_path.c_str());
        return finish("lift-s7-indirect");
    }
    const std::string criteria((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string criteria_line;
    std::string site_line;
    const bool has_criteria = findLine(criteria, "lift-gen: [S7 criteria] ", &criteria_line);
    const bool has_sites = findLine(criteria, "lift-gen: [S7 criteria/site accounts] ", &site_line);
    std::printf("lift-s7-indirect: criteria: %s\n", has_criteria ? criteria_line.c_str() : "(缺行)");
    std::printf("lift-s7-indirect: sites   : %s\n", has_sites ? site_line.c_str() : "(缺行)");
    LIFT_CHECK(has_criteria);
    LIFT_CHECK(has_sites);
    // 根集 = {F, G}，两个都不调静态 callee ⇒ 恰好 2 个单元根；放宽后**两个都**可结构化。
    // （判据回退 ⇒ structurable 掉到 1，并且下面那条 downgrade 断言的诊断行会点出 F。）
    if (has_criteria) {
        LIFT_CHECK(criteria_line.find("unit roots=2 self-pass=2 structurable=2") != std::string::npos);
    }
    if (has_sites) {
        // ret 站点 = F 的 2 条 `retn 4` + G 的 1 条 `retn 8`；放宽后 3 条全在可结构化单元里。
        LIFT_CHECK(site_line.find("ret=3 (in structured units=3)") != std::string::npos);
        // 间接 call 站点 = 2（F 的 `call ecx` / `call edx`），都落 `lift_dyn`。
        LIFT_CHECK(site_line.find("call=2 (static=0 indirect=2)") != std::string::npos);
        LIFT_CHECK(site_line.find("lift_dyn=2") != std::string::npos);
    }
    {
        char tag[32];
        std::snprintf(tag, sizeof(tag), "%08X", kCallerEa);
        std::string why;
        LIFT_CHECK(!findLine(criteria, std::string("[S7 downgrade] ") + tag, &why));
    }

    // =================================================================================
    // ② 运行期差分：同一份 PE / 同一初始状态，lifted 臂 vs 解释器臂
    // =================================================================================
    Arm li;
    Arm ia;
    if (!li.init(pe_path) || !ia.init(pe_path)) {
        lift_test::failf(__FILE__, __LINE__, "夹具初始化失败：%s / %s", li.err.c_str(), ia.err.c_str());
        return finish("lift-s7-indirect");
    }
    LIFT_CHECK(lift::aot::lookup(kCallerEa, nullptr) != nullptr);
    LIFT_CHECK(lift::aot::lookup(kCalleeEa, nullptr) != nullptr);
    lift::liftDynLookup = &lookupForTest;

    setState(li);
    setState(ia);
    const std::uint32_t esp_before = li.m.cpu().r[4];
    LIFT_CHECK(esp_before == ia.m.cpu().r[4]);

    // 自检（别静默跳过那条路）：F 里真的有一条间接 call，且它在 `jnz` 之后。
    std::uint32_t call_site = 0;
    std::uint32_t call_resume = 0;
    if (!scanIndirectCall(ia, &call_site, &call_resume)) {
        lift_test::failf(__FILE__, __LINE__, "自检失败：从 %08X 走块图没找到间接 `call`（根集写错？）",
                         kCallerEa);
        return finish("lift-s7-indirect");
    }

    // ---- lifted 臂：**一次**表查找跑完 F（G 由 F 的 `lift_dyn` 进、回到 F 的同一个宿主帧）----
    lift::aot::resetDispatchStats();
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
        if (lift::aot::lookup(ea, nullptr) == nullptr) break;  // 表外 EA = 栈哨兵（正常出口）
    }
    lift::liftRegsToCpu(regs, li.m.cpu());
    const std::uint64_t lifted_insns = li.m.cpu().insns - insns0;
    const lift::aot::DispatchStats dyn = lift::aot::dispatchStats();

    // ---- 解释器臂：同一起点跑到栈哨兵 ----
    const std::uint64_t interp0 = ia.m.cpu().insns;
    lift::RunOptions o;
    o.until_ea = kSentinel;
    o.max_insns = interp0 + 100000u;
    ia.m.cpu().eip = kCallerEa;
    (void)ia.m.run(o);
    const std::uint64_t interp_insns = ia.m.cpu().insns - interp0;

    const Snap sl = snap(li, esp_before);
    const Snap si = snap(ia, esp_before);

    // ---- 读数（判据的可见面；⛔ 不是"日志"）----
    std::printf(
        "lift-s7-indirect: F=%08X G=%08X 间接call@%08X（resume=%08X）· 外层次数=%d · lift_dyn 查找=%llu · "
        "insns=%llu/%llu · esp %08X→%08X/%08X · [esp]=%08X（哨兵 %08X）· [call-4]=%08X（压入的 resume）\n",
        kCallerEa, kCalleeEa, call_site, call_resume, lookups,
        static_cast<unsigned long long>(dyn.lift_dyn_lookups),
        static_cast<unsigned long long>(lifted_insns), static_cast<unsigned long long>(interp_insns),
        esp_before, sl.r[4], si.r[4], li.m.mem().rd32(esp_before), kSentinel,
        li.m.mem().rd32(esp_before - 20u));

    // ---- ③ 反 vacuity ----
    LIFT_CHECK(lookups == 1);                    // F 从头跑到尾只回一次派发（G 在同一个宿主帧里）
    LIFT_CHECK(lifted_insns >= 1u);
    LIFT_CHECK(dyn.lift_dyn_lookups == 1);       // 间接 call 真的走了 lift_dyn（不是"没走到"）
    LIFT_CHECK(li.m.cpu().running);
    // ---- 出口的栈口径 ----
    // 入口帧 E：[E]=返回地址（哨兵）、[E+4]=arg_0。F 的 `retn 4` = 弹返回地址 + 清 4 字节
    // ⇒ ESP = E + 8；`[E]` 是**没被写过**的哨兵。
    LIFT_CHECK_EQ_U32(sl.r[4], esp_before + 8u);
    LIFT_CHECK_EQ_U32(si.r[4], esp_before + 8u);
    LIFT_CHECK_EQ_U32(li.m.mem().rd32(esp_before), kSentinel);
    LIFT_CHECK_EQ_U32(ia.m.mem().rd32(esp_before), kSentinel);
    // 间接 call 压的 resume 必须在栈上原样留着（F 的 call 在 E-16 处压栈 ⇒ 返回地址在 E-20）；
    // G 的 `retn 8` 从那里弹走它，不写它。
    LIFT_CHECK_EQ_U32(li.m.mem().rd32(esp_before - 20u), call_resume);
    LIFT_CHECK_EQ_U32(ia.m.mem().rd32(esp_before - 20u), call_resume);
    // ---- ② 与解释器逐位相同 ----
    static const char* kNames[8] = {"EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"};
    bool same = true;
    for (int i = 0; i < 8; ++i) {
        if (sl.r[i] != si.r[i]) {
            same = false;
            lift_test::failf(__FILE__, __LINE__, "%s: lifted=0x%08X interp=0x%08X", kNames[i], sl.r[i],
                             si.r[i]);
        }
    }
    if (sl.eflags != si.eflags) {
        same = false;
        lift_test::failf(__FILE__, __LINE__, "EFLAGS: lifted=0x%08X interp=0x%08X", sl.eflags, si.eflags);
    }
    if (sl.mem != si.mem) {
        same = false;
        std::size_t i = 0;
        while (i < sl.mem.size() && sl.mem[i] == si.mem[i]) ++i;
        lift_test::failf(__FILE__, __LINE__,
                         "栈窗不同：first diff @+0x%zX lifted=0x%02X interp=0x%02X", i,
                         i < sl.mem.size() ? sl.mem[i] : 0u, i < si.mem.size() ? si.mem[i] : 0u);
    }
    if (sl.data != si.data) {
        same = false;
        lift_test::failf(__FILE__, __LINE__, "夹具数据窗不同（[%08X,%08X)）", kStruct,
                         kStruct + kStructProbe);
    }
    if (lifted_insns != interp_insns) {
        same = false;
        lift_test::failf(__FILE__, __LINE__, "两臂指令数不同：lifted=%llu interp=%llu",
                         static_cast<unsigned long long>(lifted_insns),
                         static_cast<unsigned long long>(interp_insns));
    }
    // 出口 EIP：lifted 臂的返回值（= 下一个 EA）与解释器臂的 eip 都是栈哨兵。
    LIFT_CHECK_EQ_U32(ea, kSentinel);
    LIFT_CHECK_EQ_U32(ia.m.cpu().eip, kSentinel);
    g_current_case = "(none)";
    if (!same) lift_test::failf(__FILE__, __LINE__, "两臂状态不同（上面逐项点名）");
    return finish("lift-s7-indirect");
#endif
}
