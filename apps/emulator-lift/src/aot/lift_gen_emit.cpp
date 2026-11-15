// emulator-lift · Shape A / T1a：guest 基本块 → LLVM IR 文本（`lift-gen` 的发射器）
//
// ★ 语义真源 = 解释器臂的 `src/runtime/exec*.cpp` + `x86.cpp`：
//   · 寄存器写回/合并（8 位高半、16 位）照 `X86::writeReg`；
//   · 有效地址照 `X86::eaOf`（disp + base + index*scale，32 位回绕）；
//   · flags 照 `exec.cpp` 的 `setLogicFlags` / `setAddFlags` / `setSubFlags` 与 `exec_integer.cpp`
//     的逐条实现（CF/PF/AF/ZF/SF/OF 的公式**逐位**一致；其它位原样保留）；
//   · `push/pop` 照 `X86::push32/pop32`（**不**走 `writeMem` ⇒ **不**调 `lift_note_write`）；
//   · 唯一的数据写通道照 `X86::writeMem`：先 `lift_note_write(cpu, ea, n)` 再写。
//   ⛔ 一处都不"顺手优化"成另一套语义：与解释器逐位一致是 AOT 唯一的正确性判据。
//
// ★ 寄存器状态**留在 `%st`（LiftRegs）里**：每条指令读 `%st`、算、写回 `%st`。
//   于是不需要跨块 SSA / phi，控制流直接 `br`；这也是"语义等价、但**不**逐字节等于手写 golden"
//   的原因（golden 是 SSA 形态，本生成器是内存形态）。⛔ 不改 golden 去迁就生成器。
//
// ★ 入口函数形状（父级已裁）：
//   `define i32 @lift_cc_<EA>(ptr %st, ptr %mem, i32 %ea, ptr %cpu)`；
//   `switch i32 %ea` 表外 ⇒ `bad:` = `llvm.trap` + `unreachable`（⛔ 不返回哨兵）。
//   · 目标**不是**入口 EA 的 fallthrough / jcc rel32 / jmp rel32 ⇒ 宿主 `br`；
//   · 目标**是**入口 EA ⇒ `ret i32 <目标 EA>`（交回 trampoline）；
//   · `call` ⇒ 压返回 EA 到 guest 栈，然后 `ret i32 <被调方 EA>`（rel32 是常量；
//     reg/mem 从操作数**先读**再压栈 —— 与 `exec_control.cpp` 的顺序一致）；
//   · `ret`/`retn imm16` ⇒ 弹 EA、`esp += 4 [+ imm]`、`ret i32 <弹出的 EA>`；
//   · `jmp reg/mem` ⇒ 读目标后 `ret i32 <目标>`（运行期才知道，必须回 trampoline）。
//
// ★ 本轮**不 inline** 的指令：**单指令宿主原语** `lift_one(cpu, st, ea)` —— 一条指令一次宿主
//   调用，语义交给既有的解释器（`Decoder::decode` + `execInsn`，与 dispatcher 同一对），
//   生成器**不编第二份语义**。
//   这条默认路径覆盖：x87 全族 / `fs:`·`gs:` / 串操作 / `Div`·`Idiv` / 循环移位 / 寄存器计数移位 /
//   `Imul` / `Shld` / `Cmpxchg` / `Cpuid` / `Int`·`Int3`·`Hlt` / `Other` / `Invalid` / MMX / SSE /
//   段寄存器操作数（P2）…… 正确性由解释器保证（D10-P2 / D11 / D12）。
//   ⛔ **控制转移**（`jmp/jcc/call/ret`）不走这条路：那些由生成器自己 inline / 回 trampoline；
//   它们掉进默认路径 ⇒ 仍然 `llvm.trap`（那是生成器内部不一致，⛔ 不许静默）。
//   ★ 计数（D2）：`lift_tick(cpu, n)` 在**块边界**发一次静态条数、在**任何宿主调用之前**把挂账的
//   先 flush 掉 ⇒ 观测点看到的 `insns` 精确；`rep` 按 1 条算（与解释器同一口径）。
#include "aot/lift_gen_emit.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

#include "runtime/exec.h"     // opIsControl / opIsX87（与解释器同一份判据）
#include "runtime/lift_abi.h"  // kLiftHalt（停机哨兵；`lift_one` 失败 ⇒ ret i32 kLiftHalt）

namespace lift {
namespace aot {
namespace {

// EFLAGS 位（`regs.h` 的同一组常量；这里只用于生成常量掩码）
constexpr std::uint32_t kCf = 0x00000001u;
constexpr std::uint32_t kPf = 0x00000004u;
constexpr std::uint32_t kAf = 0x00000010u;
constexpr std::uint32_t kZf = 0x00000040u;
constexpr std::uint32_t kSf = 0x00000080u;
constexpr std::uint32_t kDf = 0x00000400u;
constexpr std::uint32_t kOf = 0x00000800u;

// 与 golden 同一张"比较集合"（`kDiffFlags` = 0x8D5）：CF|PF|AF|ZF|SF|OF。
constexpr std::uint32_t kSixFlags = kCf | kPf | kAf | kZf | kSf | kOf;
static_assert(kSixFlags == 0x8D5u, "六位比较集合必须与 tests 的 kDiffFlags 一致");

std::uint32_t maskOfSize(unsigned size) {
    return size == 1 ? 0xFFu : (size == 2 ? 0xFFFFu : 0xFFFFFFFFu);
}
unsigned bitsOfSize(unsigned size) { return size == 1 ? 8u : (size == 2 ? 16u : 32u); }
std::uint32_t signOfSize(unsigned size) {
    return size == 1 ? 0x80u : (size == 2 ? 0x8000u : 0x80000000u);
}

std::string i32c(std::uint32_t v) { return std::to_string(static_cast<std::int32_t>(v)); }
std::string i64c(std::uint64_t v) { return std::to_string(static_cast<std::int64_t>(v)); }
std::string hexEa(std::uint32_t ea) {
    char b[16];
    std::snprintf(b, sizeof(b), "%X", ea);
    return b;
}

// =====================================================================================
// 一个入口函数的发射器
// =====================================================================================
class FnEmit {
public:
    FnEmit(const GenComponent& comp, const std::set<std::uint32_t>& entries,
           const std::set<std::uint32_t>& dropped_blocks, GenEmitStats* stats,
           std::vector<GenUnsupported>* unsupported)
        : comp_(comp), entries_(entries), dropped_(dropped_blocks), stats_(stats),
          unsupported_(unsupported) {}

    std::string run();

private:
    // ---- 基础 ----
    std::string tmp() { return "%t" + std::to_string(tmp_++); }
    void add(const std::string& s) { body_ += "  " + s + "\n"; }
    void put(const std::string& s) { body_ += s; }

    std::string bin(const char* op, const char* ty, const std::string& a, const std::string& b) {
        std::string r = tmp();
        add(r + " = " + op + " " + ty + " " + a + ", " + b);
        return r;
    }
    std::string bin1(const char* op, const std::string& a, const std::string& b) {
        std::string r = tmp();
        add(r + " = " + op + " i1 " + a + ", " + b);
        return r;
    }
    std::string not1(const std::string& a) { return bin1("xor", a, "true"); }
    std::string and1(const std::string& a, const std::string& b) { return bin1("and", a, b); }
    std::string or1(const std::string& a, const std::string& b) { return bin1("or", a, b); }
    std::string select1(const std::string& c, const char* ty, const std::string& a,
                        const std::string& b) {
        std::string r = tmp();
        add(r + " = select i1 " + c + ", " + ty + " " + a + ", " + ty + " " + b);
        return r;
    }
    std::string andv(const std::string& a, std::uint32_t mask) {
        return bin("and", "i32", a, i32c(mask));
    }
    std::string maskVal(const std::string& v, unsigned size) {
        return size >= 4 ? v : andv(v, maskOfSize(size));
    }
    std::string orList(std::initializer_list<std::string> xs) {
        std::string acc;
        for (const std::string& x : xs) acc = acc.empty() ? x : bin("or", "i32", acc, x);
        return acc;
    }

    // ---- 寄存器 / 内存 ----
    std::string loadReg(int index);
    void storeReg(int index, const std::string& v);
    std::string readRegValue(Reg reg);
    void writeRegValue(Reg reg, const std::string& v);
    std::string eaOf(const MemOperand& m);
    std::string loadMemValue(const MemOperand& m);
    void storeMemValue(const MemOperand& m, const std::string& v);
    std::string loadAt(const std::string& ea);
    std::string memPtrFor(const std::string& ea);
    std::string readValue(const Operand& op);
    void writeDest(const Operand& op, const std::string& v);

    // ---- 栈（照 X86::push32 / pop32；⛔ 不调 lift_note_write）----
    void pushValue(const std::string& v);
    std::string popValue();

    // ---- flags ----
    std::string loadEflags();
    void storeEflags(const std::string& v);
    std::string flagCond(const std::string& eflags, std::uint32_t bit);
    std::string parity(const std::string& v);
    std::string computeLogicFlags(const std::string& eflags, const std::string& result, unsigned size);
    std::string computeAddFlags(const std::string& eflags, const std::string& a, const std::string& b,
                                const std::string& result, unsigned size, bool with_carry);
    std::string computeSubFlags(const std::string& eflags, const std::string& a, const std::string& b,
                                const std::string& result, unsigned size, bool with_borrow);
    std::string condValue(Cond cc, const std::string& eflags);

    // ---- 控制流 ----
    std::string labelFor(std::uint32_t ea);
    void emitEdge(std::uint32_t ea);

    // ---- 指令计数（D2）/ 单指令宿主原语（T1c）----
    // flushTick：把**已经执行完**的条数一次 `lift_tick` 发出去（块边界 + 任何宿主调用之前）。
    void flushTick();
    void emitLiftOne(const Insn& in, bool terminating);

    // ---- 指令 ----
    void emitBlock(const GenBlock& block);
    void emitInsn(const Insn& in);
    void emitAlu(const Insn& in);
    void emitIncDec(const Insn& in);
    void emitShift(const Insn& in);
    void emitMul(const Insn& in);
    void emitBitTest(const Insn& in);
    void emitBitScan(const Insn& in);
    void emitTerminator(const Insn& in);
    void emitCall(const Insn& in);
    void emitJcc(const Insn& in);
    void emitRet(const Insn& in);
    void emitTrap(const Insn& in, const char* reason);
    const char* unsupportedReason(const Insn& in) const;

    const GenComponent& comp_;
    const std::set<std::uint32_t>& entries_;
    const std::set<std::uint32_t>& dropped_;  // 被丢弃的派生 leader（没有块 ⇒ 边发 `ret i32`）
    GenEmitStats* stats_;
    std::vector<GenUnsupported>* unsupported_;
    std::string body_;
    int tmp_ = 0;
    std::set<std::uint32_t> exit_ea_;  // 需要单独 `ret` 块的入口 EA（jcc 的两个出口）
    // ★ 挂账的**已执行**条数（D2）：每发射完一条 guest 指令 +1，在块边界 / 宿主调用之前 flush。
    //   `lift_one` 的那一条只在**成功**路径上算（失败 ⇒ 停机，与解释器"失败的这条不算执行过"同口径）。
    int pending_ticks_ = 0;
    // ★ `lift_one` 的两个出口：`lo_ok_<n>` 在块内继续，`lo_halt_<n>` 收集起来在**函数尾**统一发
    //   （不能就地发：`ret` 之后的指令会被 LLVM 当成不可达块的新指令）。序号 = 发射顺序 ⇒ 确定性。
    std::vector<std::string> halt_labels_;
    int label_id_ = 0;
    // ★ `emitTrap` 之后**必须**停止发射本块（P5：`unreachable` 后面再发指令 = 非法 IR）。
    //   机械判据（而不是"今天到不了"的隐式不变量）。
    bool trapped_ = false;
};

// ---------------------------------------------------------------------------------------
// 基础：寄存器 / 内存
// ---------------------------------------------------------------------------------------
std::string FnEmit::loadReg(int index) {
    std::string g = tmp();
    add(g + " = getelementptr i8, ptr %st, i64 " + std::to_string(4 * index));
    std::string v = tmp();
    add(v + " = load i32, ptr " + g + ", align 4");
    return v;
}

void FnEmit::storeReg(int index, const std::string& v) {
    std::string g = tmp();
    add(g + " = getelementptr i8, ptr %st, i64 " + std::to_string(4 * index));
    add("store i32 " + v + ", ptr " + g + ", align 4");
}

std::string FnEmit::readRegValue(Reg reg) {
    const RegSpec spec = regSpec(reg);
    const std::string full = loadReg(spec.index);
    switch (spec.cls) {
        case RegClass::Gpr32: return full;
        case RegClass::Gpr16: return andv(full, 0xFFFFu);
        case RegClass::Gpr8: {
            if (spec.byte_off == 0) return andv(full, 0xFFu);
            std::string hi = bin("lshr", "i32", full, "8");
            return andv(hi, 0xFFu);
        }
        default: return "0";
    }
}

// 照 `X86::writeReg`：32 位整写；16 位保留高 16；8 位低/高半各自只改自己的字节。
void FnEmit::writeRegValue(Reg reg, const std::string& v) {
    const RegSpec spec = regSpec(reg);
    switch (spec.cls) {
        case RegClass::Gpr32: storeReg(spec.index, v); break;
        case RegClass::Gpr16: {
            const std::string cur = loadReg(spec.index);
            storeReg(spec.index, bin("or", "i32", andv(cur, 0xFFFF0000u), andv(v, 0xFFFFu)));
            break;
        }
        case RegClass::Gpr8: {
            const unsigned shift = 8u * spec.byte_off;
            const std::string cur = loadReg(spec.index);
            std::string cleared = andv(cur, ~(0xFFu << shift));
            std::string part = andv(v, 0xFFu);
            if (shift != 0) part = bin("shl", "i32", part, i32c(shift));
            storeReg(spec.index, bin("or", "i32", cleared, part));
            break;
        }
        default: break;
    }
}

// 照 `X86::eaOf`：disp 起头，+ base，+ index*scale（32 位回绕）。
std::string FnEmit::eaOf(const MemOperand& m) {
    if (m.base == Reg::None && m.index == Reg::None) return i32c(m.disp);
    std::string ea = i32c(m.disp);
    if (m.base != Reg::None) {
        ea = bin("add", "i32", ea, loadReg(regSpec(m.base).index));
    }
    if (m.index != Reg::None) {
        std::string ix = loadReg(regSpec(m.index).index);
        if (m.scale != 1) ix = bin("mul", "i32", ix, i32c(m.scale));
        ea = bin("add", "i32", ea, ix);
    }
    return ea;
}

std::string FnEmit::loadMemValue(const MemOperand& m) {
    const std::string ea = eaOf(m);
    const std::string e64 = tmp();
    add(e64 + " = zext i32 " + ea + " to i64");
    const std::string p = tmp();
    add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
    const std::string v = tmp();
    // ★ 一律 align 1：guest 地址不保证对齐（夹具里 kBuf 就是故意非对齐的）；
    //   `align 4` 在 LLVM 里是"我保证对齐"的承诺，违约是 UB。语义优先。
    switch (m.size) {
        case 1: {
            add(v + " = load i8, ptr " + p + ", align 1");
            const std::string z = tmp();
            add(z + " = zext i8 " + v + " to i32");
            return z;
        }
        case 2: {
            add(v + " = load i16, ptr " + p + ", align 1");
            const std::string z = tmp();
            add(z + " = zext i16 " + v + " to i32");
            return z;
        }
        default: add(v + " = load i32, ptr " + p + ", align 1"); return v;
    }
}

// 照 `X86::writeMem`：先 `lift_note_write`（SMC 失效）再写。`push/pop` 不走这里。
void FnEmit::storeMemValue(const MemOperand& m, const std::string& v) {
    // ★ 宿主调用之前先把挂账的条数发出去（D2 的 ②）：`lift_note_write` 也是宿主调用。
    //   本条自己**还没**执行完 ⇒ 不在这批里（pending 只含之前的指令）。
    flushTick();
    const std::string ea = eaOf(m);
    add("call void @lift_note_write(ptr %cpu, i32 " + ea + ", i32 " + std::to_string(m.size) + ")");
    stats_->used_note_write = true;
    const std::string e64 = tmp();
    add(e64 + " = zext i32 " + ea + " to i64");
    const std::string p = tmp();
    add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
    switch (m.size) {
        case 1: {
            const std::string t = tmp();
            add(t + " = trunc i32 " + v + " to i8");
            add("store i8 " + t + ", ptr " + p + ", align 1");
            break;
        }
        case 2: {
            const std::string t = tmp();
            add(t + " = trunc i32 " + v + " to i16");
            add("store i16 " + t + ", ptr " + p + ", align 1");
            break;
        }
        default: add("store i32 " + v + ", ptr " + p + ", align 1"); break;
    }
}

// 从显式 EA 读一个 i32（`ret` 的返回地址 / `leave` 的保存 EBP 用）。
std::string FnEmit::loadAt(const std::string& ea) {
    const std::string e64 = tmp();
    add(e64 + " = zext i32 " + ea + " to i64");
    const std::string p = tmp();
    add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
    const std::string v = tmp();
    add(v + " = load i32, ptr " + p + ", align 1");
    return v;
}

// 照 `rd(cpu, op) = readOp(op) & maskOf(op.size)`。
std::string FnEmit::readValue(const Operand& op) {
    const unsigned size = op.size != 0 ? op.size : 4u;
    switch (op.kind) {
        case OpKind::Reg: return maskVal(readRegValue(op.reg), size);
        case OpKind::Imm: return i32c(static_cast<std::uint32_t>(op.imm) & maskOfSize(size));
        case OpKind::Mem: return maskVal(loadMemValue(op.mem), size);
        default: return "0";
    }
}

// 照 `wr(cpu, op, v)`：先按 op.size 截断，再按 Reg（合并）/ Mem（writeMem）写。
void FnEmit::writeDest(const Operand& op, const std::string& v) {
    const unsigned size = op.size != 0 ? op.size : 4u;
    const std::string x = maskVal(v, size);
    switch (op.kind) {
        case OpKind::Reg: writeRegValue(op.reg, x); break;
        case OpKind::Mem: storeMemValue(op.mem, x); break;
        default: break;
    }
}

void FnEmit::pushValue(const std::string& v) {
    const std::string esp = loadReg(4);
    const std::string esp2 = bin("sub", "i32", esp, "4");
    storeReg(4, esp2);
    const std::string p_ea = memPtrFor(esp2);
    add("store i32 " + v + ", ptr " + p_ea + ", align 1");
}

// `pushValue` 的地址计算抽出来（避免两条语句里各写一遍 zext/gep 的样板）。
std::string FnEmit::memPtrFor(const std::string& ea) {
    const std::string e64 = tmp();
    add(e64 + " = zext i32 " + ea + " to i64");
    const std::string p = tmp();
    add(p + " = getelementptr i8, ptr %mem, i64 " + e64);
    return p;
}

std::string FnEmit::popValue() {
    const std::string esp = loadReg(4);
    const std::string v = loadAt(esp);
    const std::string esp2 = bin("add", "i32", esp, "4");
    storeReg(4, esp2);
    return v;
}

// ---------------------------------------------------------------------------------------
// flags（逐位照 exec.cpp / exec_integer.cpp）
// ---------------------------------------------------------------------------------------
std::string FnEmit::loadEflags() { return loadReg(8); }
void FnEmit::storeEflags(const std::string& v) { storeReg(8, v); }

std::string FnEmit::flagCond(const std::string& eflags, std::uint32_t bit) {
    return bin("icmp ne", "i32", andv(eflags, bit), "0");
}

std::string FnEmit::parity(const std::string& v) {
    const std::string low = andv(v, 0xFFu);
    const std::string pc = tmp();
    add(pc + " = call i32 @llvm.ctpop.i32(i32 " + low + ")");
    stats_->used_ctpop = true;
    const std::string odd = andv(pc, 1u);
    return bin("icmp eq", "i32", odd, "0");
}

// setLogicFlags：CF/OF/AF=0；ZF/SF/PF 按结果。其它位保留。
std::string FnEmit::computeLogicFlags(const std::string& eflags, const std::string& result,
                                      unsigned size) {
    const std::uint32_t mask = maskOfSize(size);
    const std::uint32_t sign = signOfSize(size);
    const std::string r = andv(result, mask);
    const std::string zf = bin("icmp eq", "i32", r, "0");
    const std::string sf = bin("icmp ne", "i32", andv(r, sign), "0");
    const std::string pf = parity(r);
    const std::string six = orList({
        select1(zf, "i32", i32c(kZf), "0"),
        select1(sf, "i32", i32c(kSf), "0"),
        select1(pf, "i32", i32c(kPf), "0"),
    });
    return bin("or", "i32", andv(eflags, ~kSixFlags), six);
}

// setAddFlags：full = (a&mask)+(b&mask)+carry（i64，避免丢进位）
std::string FnEmit::computeAddFlags(const std::string& eflags, const std::string& a,
                                    const std::string& b, const std::string& result, unsigned size,
                                    bool with_carry) {
    const std::uint64_t mask = maskOfSize(size);
    const std::uint32_t sign = signOfSize(size);
    const std::string a64 = tmp();
    add(a64 + " = zext i32 " + a + " to i64");
    const std::string b64 = tmp();
    add(b64 + " = zext i32 " + b + " to i64");
    std::string full = bin("add", "i64", a64, b64);
    if (with_carry) {
        const std::string carry = select1(flagCond(eflags, kCf), "i64", "1", "0");
        full = bin("add", "i64", full, carry);
    }
    const std::string cf = bin("icmp ugt", "i64", full, i64c(mask));
    const std::string r = andv(result, static_cast<std::uint32_t>(mask));
    const std::string sr = bin("icmp ne", "i32", andv(r, sign), "0");
    const std::string sa = bin("icmp ne", "i32", andv(a, sign), "0");
    const std::string sb = bin("icmp ne", "i32", andv(b, sign), "0");
    const std::string of = and1(bin("icmp eq", "i1", sa, sb), bin("icmp ne", "i1", sr, sa));
    const std::string f32 = tmp();
    add(f32 + " = trunc i64 " + full + " to i32");
    const std::string afv = bin("xor", "i32", bin("xor", "i32", a, b), f32);
    const std::string af = bin("icmp ne", "i32", andv(afv, kAf), "0");
    const std::string zf = bin("icmp eq", "i32", r, "0");
    const std::string pf = parity(r);
    const std::string six = orList({
        select1(cf, "i32", i32c(kCf), "0"),
        select1(of, "i32", i32c(kOf), "0"),
        select1(af, "i32", i32c(kAf), "0"),
        select1(zf, "i32", i32c(kZf), "0"),
        select1(sr, "i32", i32c(kSf), "0"),
        select1(pf, "i32", i32c(kPf), "0"),
    });
    return bin("or", "i32", andv(eflags, ~kSixFlags), six);
}

// setSubFlags：CF = av < bv + borrow（无符号）；AF = (av^bv^r)&0x10。
std::string FnEmit::computeSubFlags(const std::string& eflags, const std::string& a,
                                    const std::string& b, const std::string& result, unsigned size,
                                    bool with_borrow) {
    const std::uint32_t mask = maskOfSize(size);
    const std::uint32_t sign = signOfSize(size);
    const std::string av = andv(a, mask);
    const std::string bv = andv(b, mask);
    const std::string r = andv(result, mask);
    const std::string av64 = tmp();
    add(av64 + " = zext i32 " + av + " to i64");
    const std::string bv64 = tmp();
    add(bv64 + " = zext i32 " + bv + " to i64");
    std::string borrow = "0";
    if (with_borrow) borrow = select1(flagCond(eflags, kCf), "i64", "1", "0");
    std::string bp;
    if (with_borrow) bp = bin("add", "i64", bv64, borrow);
    else bp = bv64;
    const std::string cf = bin("icmp ult", "i64", av64, bp);
    const std::string sr = bin("icmp ne", "i32", andv(r, sign), "0");
    const std::string sa = bin("icmp ne", "i32", andv(av, sign), "0");
    const std::string sb = bin("icmp ne", "i32", andv(bv, sign), "0");
    const std::string of = and1(bin("icmp ne", "i1", sa, sb), bin("icmp ne", "i1", sr, sa));
    const std::string afv = bin("xor", "i32", bin("xor", "i32", av, bv), r);
    const std::string af = bin("icmp ne", "i32", andv(afv, kAf), "0");
    const std::string zf = bin("icmp eq", "i32", r, "0");
    const std::string pf = parity(r);
    const std::string six = orList({
        select1(cf, "i32", i32c(kCf), "0"),
        select1(of, "i32", i32c(kOf), "0"),
        select1(af, "i32", i32c(kAf), "0"),
        select1(zf, "i32", i32c(kZf), "0"),
        select1(sr, "i32", i32c(kSf), "0"),
        select1(pf, "i32", i32c(kPf), "0"),
    });
    return bin("or", "i32", andv(eflags, ~kSixFlags), six);
}

std::string FnEmit::condValue(Cond cc, const std::string& eflags) {
    const std::string cf = flagCond(eflags, kCf);
    const std::string pf = flagCond(eflags, kPf);
    const std::string zf = flagCond(eflags, kZf);
    const std::string sf = flagCond(eflags, kSf);
    const std::string of = flagCond(eflags, kOf);
    switch (cc) {
        case Cond::O: return of;
        case Cond::NO: return not1(of);
        case Cond::B: return cf;
        case Cond::AE: return not1(cf);
        case Cond::E: return zf;
        case Cond::NE: return not1(zf);
        case Cond::BE: return or1(cf, zf);
        case Cond::A: return not1(or1(cf, zf));
        case Cond::S: return sf;
        case Cond::NS: return not1(sf);
        case Cond::P: return pf;
        case Cond::NP: return not1(pf);
        case Cond::L: return bin1("xor", sf, of);
        case Cond::GE: return not1(bin1("xor", sf, of));
        case Cond::LE: return or1(zf, bin1("xor", sf, of));
        case Cond::G: return and1(not1(zf), not1(bin1("xor", sf, of)));
        default: return "false";
    }
}

// ---------------------------------------------------------------------------------------
// 控制流
// ---------------------------------------------------------------------------------------
std::string FnEmit::labelFor(std::uint32_t ea) {
    // 入口 EA 或被丢弃的派生 leader（没有块）⇒ 统一的 `bb_exit_<EA>` 出口块（`ret i32 <EA>`）。
    if (entries_.count(ea) != 0 || dropped_.count(ea) != 0) {
        exit_ea_.insert(ea);
        return "%bb_exit_" + hexEa(ea);
    }
    return "%bb_" + hexEa(ea);
}

void FnEmit::emitEdge(std::uint32_t ea) {
    if (entries_.count(ea) != 0 || dropped_.count(ea) != 0) {
        // 目标是入口 EA ⇒ 交回 trampoline（状态已经在 %st 里）。
        // 目标是被丢弃的派生 leader ⇒ 同样交回 trampoline（它没有块；解释器会自己解那段字节）。
        add("ret i32 " + i32c(ea));
        return;
    }
    add("br label %bb_" + hexEa(ea));
}

void FnEmit::emitCall(const Insn& in) {
    const std::uint32_t ret = in.ea + in.length;
    const Operand& op = in.op0();
    // ★ 先读目标（`call [esp+x]` 依赖压栈前的 ESP；与 exec_control.cpp 同序），再压栈。
    std::string dyn;
    if (op.kind != OpKind::Imm) dyn = readValue(op);
    pushValue(i32c(ret));
    if (op.kind == OpKind::Imm) add("ret i32 " + i32c(genBranchTarget(in)));
    else add("ret i32 " + dyn);
}

void FnEmit::emitJcc(const Insn& in) {
    const std::uint32_t target = genBranchTarget(in);
    const std::uint32_t fall = in.ea + in.length;
    if (target == fall) {
        emitEdge(target);
        return;
    }
    const std::string eflags = loadEflags();
    const std::string cond = condValue(in.cc, eflags);
    add("br i1 " + cond + ", label " + labelFor(target) + ", label " + labelFor(fall));
}

void FnEmit::emitRet(const Insn& in) {
    const std::string ret = popValue();
    if (in.op0().kind == OpKind::Imm) {
        const std::uint32_t imm = static_cast<std::uint32_t>(in.op0().imm) & 0xFFFFu;
        if (imm != 0) storeReg(4, bin("add", "i32", loadReg(4), i32c(imm)));
    }
    add("ret i32 " + ret);
}

void FnEmit::emitTerminator(const Insn& in) {
    switch (in.op) {
        case Op::Call: emitCall(in); break;
        case Op::Jcc: emitJcc(in); break;
        case Op::Ret: emitRet(in); break;
        case Op::Jmp: {
            if (in.op0().kind == OpKind::Imm) emitEdge(genBranchTarget(in));
            else add("ret i32 " + readValue(in.op0()));
            break;
        }
        default: emitTrap(in, "控制流指令本轮不支持");
    }
}

void FnEmit::emitTrap(const Insn& in, const char* reason) {
    add("; unsupported @ " + hexEa(in.ea) + " (" + (in.mnemonic ? in.mnemonic : "?") + ") — " + reason);
    add("call void @llvm.trap()");
    add("unreachable");
    ++stats_->traps_unsupported;
    unsupported_->push_back(GenUnsupported{in.ea, opName(in.op), reason});
    trapped_ = true;  // ⛔ 之后不许再发指令（P5：`unreachable` 后面跟指令 = 非法 IR）
}

// ---------------------------------------------------------------------------------------
// 指令计数（D2）与单指令宿主原语（T1c）
// ---------------------------------------------------------------------------------------
void FnEmit::flushTick() {
    if (pending_ticks_ <= 0) return;
    add("call void @lift_tick(ptr %cpu, i32 " + std::to_string(pending_ticks_) + ")");
    stats_->used_tick = true;
    pending_ticks_ = 0;
}

// 一条**不 inline** 的指令 ⇒ 一次 `lift_one`，成功继续、失败回 trampoline 报停机。
//   形状（父级裁决）：
//     %ok = call i1 @lift_one(ptr %cpu, ptr %st, i32 <EA>)
//     br i1 %ok, label %lo_ok_N, label %lo_halt_N
//   lo_ok_N:   …后续指令…
//   lo_halt_N: ret i32 kLiftHalt      ← 收集到函数尾（见 run()）
void FnEmit::emitLiftOne(const Insn& in, bool terminating) {
    // ★ 宿主调用前 flush（D2）：`lift_one` 执行之后才算这一条，所以先只结算之前的。
    flushTick();
    const std::string id = std::to_string(label_id_++);
    const std::string ok_label = "lo_ok_" + id;
    const std::string halt_label = "lo_halt_" + id;
    add("; lift_one @ " + hexEa(in.ea) + " (" + (in.mnemonic ? in.mnemonic : "?") + " / " + opName(in.op) +
        ") — 单指令宿主原语：语义在解释器里，⛔ 这里不编");
    const std::string ok = tmp();
    add(ok + " = call i1 @lift_one(ptr %cpu, ptr %st, i32 " + i32c(in.ea) + ")");
    stats_->used_lift_one = true;
    ++stats_->lift_one_sites;
    ++stats_->lift_one_by_op[opName(in.op)];
    add("br i1 " + ok + ", label %" + ok_label + ", label %" + halt_label);
    put(ok_label + ":\n");
    pending_ticks_ = 1;  // 这一条已经执行（成功路径）；失败路径直接停机 ⇒ 不计数
    halt_labels_.push_back(halt_label);
    if (terminating) {
        // `Hlt/Int3/Int` 在解释器里**恒**返回 false（`exec_control.cpp:62-72`：置停 / trapInt）
        // ⇒ 成功路径不可达。真到这里说明解释器口径变了 ⇒ 响亮 trap（⛔ 不静默落空）。
        add("; ⚠ 停机类指令（hlt/int3/int）lift_one 返回 true = 解释器口径变了（生成器内部不一致）");
        add("call void @llvm.trap()");
        add("unreachable");
        trapped_ = true;
    }
}

// ---------------------------------------------------------------------------------------
// 本轮不支持的判定（父级裁定 + 本单的 (L) 缺口，都是**响亮**路）
// ---------------------------------------------------------------------------------------
const char* FnEmit::unsupportedReason(const Insn& in) const {
    if (in.op == Op::Invalid) return "Invalid（解码不出语义）";
    if (in.op == Op::Other) return "Other（助记符认识但 M1a 未实现）";
    if (opIsX87(in.op)) return "x87（状态不在 LiftRegs，本轮无宿主原语）";
    if (in.op == Op::Packed) return "MMX/Packed（mm/xmm 状态不在 LiftRegs）";
    if (in.op == Op::Sse) return "SSE（xmm/mxcsr 状态不在 LiftRegs）";
    if (in.op == Op::Div || in.op == Op::Idiv) return "Div/Idiv（#DE 停机通道不可表达）";
    if (in.op == Op::Hlt) return "Hlt（停机通道不可表达）";
    if (in.op == Op::Int3 || in.op == Op::Int) return "Int/Int3（中断通道不可表达）";
    if (in.op == Op::Movs || in.op == Op::Stos || in.op == Op::Lods || in.op == Op::Scas ||
        in.op == Op::Cmps)
        return "串操作（T1a 不生成 rep 循环）";
    if (in.op == Op::Rol || in.op == Op::Ror || in.op == Op::Rcl || in.op == Op::Rcr)
        return "循环移位（T1a 未实现）";
    if (in.op == Op::Shld || in.op == Op::Shrd) return "双精度移位（T1a 未实现）";
    if (in.op == Op::Imul) return "Imul（T1a 未实现，避免编出错语义）";
    if (in.op == Op::Cmpxchg) return "Cmpxchg（条件写内存，T1a 未实现）";
    if (in.op == Op::Cpuid) return "Cpuid（虚构 CPU 常量表未搬进生成器）";
    if ((in.op == Op::Shl || in.op == Op::Shr || in.op == Op::Sar) &&
        in.op1().kind != OpKind::Imm)
        return "移位计数在寄存器（T1a 只支持立即数计数）";
    if (in.op == Op::Setcc && in.op0().kind == OpKind::Mem)
        return "Setcc 写内存（T1a 未实现条件写）";
    if (in.op == Op::Lea && in.op1().kind != OpKind::Mem) return "Lea 的第二操作数不是内存";
    for (int i = 0; i < 3; ++i) {
        const Operand& op = in.ops[i];
        if (op.kind == OpKind::Xmm) return "XMM 操作数（xmm 状态不在 LiftRegs）";
        // ★ P2（D10）：段寄存器 / x87 寄存器操作数。`LiftRegs` 里没有 `seg[]` / `st[]`
        //   ⇒ 生成器的 `readRegValue` 会把它**静默读成 0**、`writeRegValue` 会**静默丢写**
        //   （解释器 `x86.cpp:31/55` 是真读真写）。⛔ 不给它编语义：让它走 `lift_one`，
        //   由解释器读/写真的 `cpu.seg[]` / `cpu.fpu`。
        if (op.kind == OpKind::Reg) {
            const RegClass cls = regSpec(op.reg).cls;
            if (cls == RegClass::Seg) return "段寄存器操作数（LiftRegs 没有 seg[]）⇒ lift_one";
            if (cls == RegClass::X87) return "x87 寄存器操作数（LiftRegs 没有 st[]）⇒ lift_one";
        }
        if (op.kind == OpKind::Mem) {
            if (op.mem.seg_fs || op.mem.seg_gs) return "fs:/gs: 段覆盖（LiftFn 没有段基址）";
            if (op.mem.size != 1 && op.mem.size != 2 && op.mem.size != 4)
                return "64/80/128 位内存操作数（T1a 未实现）";
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------
// 单条指令
// ---------------------------------------------------------------------------------------
void FnEmit::emitAlu(const Insn& in) {
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const std::string a = readValue(in.op0());
    const std::string b = readValue(in.op1());
    const std::string eflags = loadEflags();
    switch (in.op) {
        case Op::Add: {
            const std::string r = bin("add", "i32", a, b);
            storeEflags(computeAddFlags(eflags, a, b, r, size, false));
            writeDest(in.op0(), r);
            return;
        }
        case Op::Adc: {
            const std::string cin = select1(flagCond(eflags, kCf), "i32", "1", "0");
            const std::string r = bin("add", "i32", bin("add", "i32", a, b), cin);
            storeEflags(computeAddFlags(eflags, a, b, r, size, true));
            writeDest(in.op0(), r);
            return;
        }
        case Op::Sub: {
            const std::string r = bin("sub", "i32", a, b);
            storeEflags(computeSubFlags(eflags, a, b, r, size, false));
            writeDest(in.op0(), r);
            return;
        }
        case Op::Sbb: {
            const std::string bin_ = select1(flagCond(eflags, kCf), "i32", "1", "0");
            const std::string r = bin("sub", "i32", bin("sub", "i32", a, b), bin_);
            storeEflags(computeSubFlags(eflags, a, b, r, size, true));
            writeDest(in.op0(), r);
            return;
        }
        case Op::Cmp: {
            const std::string r = bin("sub", "i32", a, b);
            storeEflags(computeSubFlags(eflags, a, b, r, size, false));
            return;
        }
        case Op::Test: {
            const std::string r = bin("and", "i32", a, b);
            storeEflags(computeLogicFlags(eflags, r, size));
            return;
        }
        case Op::And: {
            const std::string r = bin("and", "i32", a, b);
            storeEflags(computeLogicFlags(eflags, r, size));
            writeDest(in.op0(), r);
            return;
        }
        case Op::Or: {
            const std::string r = bin("or", "i32", a, b);
            storeEflags(computeLogicFlags(eflags, r, size));
            writeDest(in.op0(), r);
            return;
        }
        case Op::Xor: {
            const std::string r = bin("xor", "i32", a, b);
            storeEflags(computeLogicFlags(eflags, r, size));
            writeDest(in.op0(), r);
            return;
        }
        default: emitTrap(in, "算逻族未实现的 op");
    }
}

void FnEmit::emitIncDec(const Insn& in) {
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const std::string a = readValue(in.op0());
    const std::string r = in.op == Op::Inc ? bin("add", "i32", a, "1") : bin("sub", "i32", a, "1");
    const std::string eflags = loadEflags();
    const std::string updated = in.op == Op::Inc
                                    ? computeAddFlags(eflags, a, "1", r, size, false)
                                    : computeSubFlags(eflags, a, "1", r, size, false);
    // inc/dec 不动 CF（`exec_integer.cpp` 的 saved_cf）。
    storeEflags(bin("or", "i32", andv(updated, ~kCf), andv(eflags, kCf)));
    writeDest(in.op0(), r);
}

// 移位：只支持**立即数计数**（T1a）。语义逐行照 `doShift`（`exec_integer.cpp:68-158`）。
void FnEmit::emitShift(const Insn& in) {
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const unsigned bits = bitsOfSize(size);
    const std::uint32_t mask = maskOfSize(size);
    const std::uint32_t sign = signOfSize(size);
    if (in.op1().kind != OpKind::Imm) {
        emitTrap(in, "移位计数在寄存器");
        return;
    }
    const std::uint8_t c = static_cast<std::uint8_t>(static_cast<std::uint32_t>(in.op1().imm) & 0x1Fu);
    if (c == 0) return;  // 计数 0：什么都不改（含 flags）
    const std::string a = readValue(in.op0());
    const std::string eflags = loadEflags();

    std::string r;
    std::string cf;      // 空 = 常量 false
    std::string of;      // 空 = **保留** OF（Shr c>1）；非空 = 替换为这个 i1
    bool of_replaced = true;
    switch (in.op) {
        case Op::Shl:
            if (c < bits) {
                r = andv(bin("shl", "i32", a, i32c(c)), mask);
                cf = bin("icmp ne", "i32",
                         andv(bin("lshr", "i32", a, i32c(bits - c)), 1u), "0");
            } else {
                r = "0";
                if (c == bits) cf = bin("icmp ne", "i32", andv(a, 1u), "0");
            }
            // OF = MSB(result) != CF
            {
                const std::string msb = bin("icmp ne", "i32", andv(r, sign), "0");
                if (cf.empty()) of = msb;
                else of = bin1("xor", msb, cf);
            }
            break;
        case Op::Shr:
            if (c < bits) {
                r = andv(bin("lshr", "i32", a, i32c(c)), mask);
                cf = bin("icmp ne", "i32",
                         andv(bin("lshr", "i32", a, i32c(c - 1)), 1u), "0");
            } else {
                r = "0";
                if (c == bits) cf = bin("icmp ne", "i32", andv(a, sign), "0");
            }
            if (c == 1) of = bin("icmp ne", "i32", andv(a, sign), "0");
            else of_replaced = false;  // 保留原 OF
            break;
        case Op::Sar: {
            std::string sa = a;
            if (size != 4) {
                const std::string shifted = bin("shl", "i32", a, i32c(32u - bits));
                sa = bin("ashr", "i32", shifted, i32c(32u - bits));
            }
            const unsigned sh = std::min<unsigned>(c, bits);
            cf = bin("icmp ne", "i32", andv(bin("ashr", "i32", sa, i32c(sh - 1)), 1u), "0");
            r = andv(bin("ashr", "i32", sa, i32c(sh)), mask);
            of = "false";  // doShift 对 Sar 无条件 of = false
            break;
        }
        default: emitTrap(in, "移位族未实现的 op"); return;
    }

    std::uint32_t replaced = kZf | kSf | kPf | kCf;  // 这几位置总是重算
    if (of_replaced) replaced |= kOf;
    std::string six = orList({
        select1(bin("icmp eq", "i32", r, "0"), "i32", i32c(kZf), "0"),
        select1(bin("icmp ne", "i32", andv(r, sign), "0"), "i32", i32c(kSf), "0"),
        select1(parity(r), "i32", i32c(kPf), "0"),
        cf.empty() ? std::string("0") : select1(cf, "i32", i32c(kCf), "0"),
        of_replaced ? select1(of, "i32", i32c(kOf), "0") : std::string("0"),
    });
    storeEflags(bin("or", "i32", andv(eflags, ~replaced), six));
    writeDest(in.op0(), r);
}

// `mul r/m`：照 `doMul`（`exec_integer.cpp:188-215`）。
void FnEmit::emitMul(const Insn& in) {
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const std::uint32_t mask = maskOfSize(size);
    const std::string src = readValue(in.op0());
    const std::string eax = loadReg(0);
    const std::string eflags = loadEflags();
    if (size == 4) {
        const std::string a64 = tmp();
        add(a64 + " = zext i32 " + eax + " to i64");
        const std::string b64 = tmp();
        add(b64 + " = zext i32 " + src + " to i64");
        const std::string prod = bin("mul", "i64", a64, b64);
        const std::string lo = tmp();
        add(lo + " = trunc i64 " + prod + " to i32");
        const std::string hi64 = bin("lshr", "i64", prod, "32");
        const std::string hi = tmp();
        add(hi + " = trunc i64 " + hi64 + " to i32");
        storeReg(0, lo);
        storeReg(2, hi);
        const std::string nz = bin("icmp ne", "i32", hi, "0");
        storeEflags(bin("or", "i32", andv(eflags, ~(kCf | kOf)),
                        bin("or", "i32", select1(nz, "i32", i32c(kCf), "0"),
                            select1(nz, "i32", i32c(kOf), "0"))));
        return;
    }
    const std::string al = andv(eax, mask);
    const std::string bv = andv(src, mask);
    const std::string prod = bin("mul", "i32", al, bv);
    const std::string hi = andv(bin("lshr", "i32", prod, i32c(bitsOfSize(size))),
                                size == 2 ? 0xFFFFu : 0xFFu);
    if (size == 2) {
        writeRegValue(Reg::AX, andv(prod, 0xFFFFu));
        writeRegValue(Reg::DX, hi);
    } else {
        writeRegValue(Reg::AX, andv(prod, 0xFFFFu));
    }
    const std::string nz = bin("icmp ne", "i32", hi, "0");
    std::string six = bin("or", "i32", select1(nz, "i32", i32c(kCf), "0"),
                          select1(nz, "i32", i32c(kOf), "0"));
    std::uint32_t replaced = kCf | kOf;
    if (size == 1) {
        // ★ 照抄 `doMul` 的 size==1 分支：ZF = (r & 0xFFFF)==0、SF = msbOf(r, 2)（bit15）。
        const std::string zf = bin("icmp eq", "i32", andv(prod, 0xFFFFu), "0");
        const std::string sf = bin("icmp ne", "i32", andv(prod, 0x8000u), "0");
        six = bin("or", "i32", six,
                  bin("or", "i32", select1(zf, "i32", i32c(kZf), "0"),
                      select1(sf, "i32", i32c(kSf), "0")));
        replaced |= kZf | kSf;
    }
    storeEflags(bin("or", "i32", andv(eflags, ~replaced), six));
}

// `bt/bts/btr/btc`：只动 CF（+ 条件写目的操作数）。
void FnEmit::emitBitTest(const Insn& in) {
    const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
    const unsigned bits = bitsOfSize(size);
    const std::string idx = readValue(in.op1());
    const std::string bit = andv(idx, bits - 1u);
    const std::string v = readValue(in.op0());
    const std::string eflags = loadEflags();
    const std::string cf = bin("icmp ne", "i32", andv(bin("lshr", "i32", v, bit), 1u), "0");
    storeEflags(bin("or", "i32", andv(eflags, ~kCf), select1(cf, "i32", i32c(kCf), "0")));
    if (in.op == Op::Bt) return;
    const std::string one = bin("shl", "i32", "1", bit);
    std::string nv;
    switch (in.op) {
        case Op::Bts: nv = bin("or", "i32", v, one); break;
        case Op::Btr: nv = bin("and", "i32", v, bin("xor", "i32", one, i32c(0xFFFFFFFFu))); break;
        default: nv = bin("xor", "i32", v, one); break;
    }
    writeDest(in.op0(), nv);
}

// `bsf/bsr`：只动 ZF；v==0 时不写目的寄存器（用 select 写回原值 ⇒ 观察不到差别）。
void FnEmit::emitBitScan(const Insn& in) {
    const unsigned size = in.op1().size != 0 ? in.op1().size : 4u;
    const std::string v = readValue(in.op1());
    const std::string eflags = loadEflags();
    const std::string zf = bin("icmp eq", "i32", v, "0");
    storeEflags(bin("or", "i32", andv(eflags, ~kZf), select1(zf, "i32", i32c(kZf), "0")));
    const std::string nz = not1(zf);
    std::string idx;
    if (in.op == Op::Bsf) {
        idx = tmp();
        add(idx + " = call i32 @llvm.cttz.i32(i32 " + v + ", i1 false)");
        stats_->used_cttz = true;
    } else {
        const std::string lz = tmp();
        add(lz + " = call i32 @llvm.ctlz.i32(i32 " + v + ", i1 false)");
        stats_->used_ctlz = true;
        idx = bin("sub", "i32", "31", lz);
    }
    const std::string cur = readRegValue(in.op0().reg);
    writeDest(in.op0(), select1(nz, "i32", idx, cur));
}

void FnEmit::emitInsn(const Insn& in) {
    switch (in.op) {
        case Op::Nop:
        case Op::Pause: return;
        case Op::Mov: writeDest(in.op0(), readValue(in.op1())); return;
        case Op::Movzx: writeDest(in.op0(), readValue(in.op1())); return;
        case Op::Movsx: {
            const unsigned size = in.op1().size != 0 ? in.op1().size : 4u;
            std::string v = readValue(in.op1());
            if (size == 1) {
                const std::string t = tmp();
                add(t + " = trunc i32 " + v + " to i8");
                const std::string s = tmp();
                add(s + " = sext i8 " + t + " to i32");
                v = s;
            } else if (size == 2) {
                const std::string t = tmp();
                add(t + " = trunc i32 " + v + " to i16");
                const std::string s = tmp();
                add(s + " = sext i16 " + t + " to i32");
                v = s;
            }
            writeDest(in.op0(), v);
            return;
        }
        case Op::Lea: writeDest(in.op0(), eaOf(in.op1().mem)); return;
        case Op::Xchg: {
            const std::string a = readValue(in.op0());
            const std::string b = readValue(in.op1());
            writeDest(in.op0(), b);
            writeDest(in.op1(), a);
            return;
        }
        case Op::Push: pushValue(readValue(in.op0())); return;
        case Op::Pop: {
            const std::string v = popValue();
            writeDest(in.op0(), v);
            return;
        }
        case Op::Pushad: {
            const std::string esp0 = loadReg(4);
            for (int i = 0; i < 8; ++i) pushValue(i == 4 ? esp0 : loadReg(i));
            return;
        }
        case Op::Popad: {
            for (int i = 7; i >= 0; --i) {
                const std::string v = popValue();
                if (i != 4) storeReg(i, v);
            }
            return;
        }
        case Op::Pushfd: pushValue(loadEflags()); return;
        case Op::Popfd: {
            const std::string v = popValue();
            // X86::popFlags：eflags = pop32() | FLAG_IF
            storeEflags(bin("or", "i32", v, i32c(0x200u)));
            return;
        }
        case Op::Xadd: {
            const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
            const std::string a = readValue(in.op0());
            const std::string b = readValue(in.op1());
            const std::string r = bin("add", "i32", a, b);
            const std::string eflags = loadEflags();
            storeEflags(computeAddFlags(eflags, a, b, r, size, false));
            writeDest(in.op0(), r);
            writeDest(in.op1(), a);
            return;
        }
        case Op::Add:
        case Op::Or:
        case Op::And:
        case Op::Sub:
        case Op::Xor:
        case Op::Cmp:
        case Op::Test:
        case Op::Adc:
        case Op::Sbb: emitAlu(in); return;
        case Op::Inc:
        case Op::Dec: emitIncDec(in); return;
        case Op::Neg: {
            const unsigned size = in.op0().size != 0 ? in.op0().size : 4u;
            const std::string a = readValue(in.op0());
            const std::string r = bin("sub", "i32", "0", a);
            const std::string eflags = loadEflags();
            storeEflags(computeSubFlags(eflags, "0", a, r, size, false));
            writeDest(in.op0(), r);
            return;
        }
        case Op::Not: {
            writeDest(in.op0(), bin("xor", "i32", readValue(in.op0()), i32c(0xFFFFFFFFu)));
            return;
        }
        case Op::Shl:
        case Op::Shr:
        case Op::Sar: emitShift(in); return;
        case Op::Mul: emitMul(in); return;
        case Op::Cdq: {
            const std::string eax = loadReg(0);
            const std::string neg = bin("icmp ne", "i32", andv(eax, 0x80000000u), "0");
            storeReg(2, select1(neg, "i32", "-1", "0"));
            return;
        }
        case Op::Cwd: {
            const std::string ax = readRegValue(Reg::AX);
            const std::string neg = bin("icmp ne", "i32", andv(ax, 0x8000u), "0");
            writeRegValue(Reg::DX, select1(neg, "i32", i32c(0xFFFFu), "0"));
            return;
        }
        case Op::Cwde: {
            const std::string eax = loadReg(0);
            const std::string neg = bin("icmp ne", "i32", andv(eax, 0x8000u), "0");
            const std::string hi = select1(neg, "i32", i32c(0xFFFF0000u), "0");
            storeReg(0, bin("or", "i32", hi, andv(eax, 0xFFFFu)));
            return;
        }
        case Op::Cbw: {
            const std::string eax = loadReg(0);
            const std::string neg = bin("icmp ne", "i32", andv(eax, 0x80u), "0");
            const std::string hi = select1(neg, "i32", i32c(0xFF00u), "0");
            writeRegValue(Reg::AX, bin("or", "i32", hi, andv(eax, 0xFFu)));
            return;
        }
        case Op::Bswap: {
            const Reg reg = in.op0().reg;
            const std::string v = loadReg(regSpec(reg).index);
            const std::string b0 = bin("shl", "i32", andv(v, 0xFFu), "24");
            const std::string b1 = bin("shl", "i32", andv(v, 0xFF00u), "8");
            const std::string b2 = bin("lshr", "i32", andv(v, 0xFF0000u), "8");
            const std::string b3 = bin("lshr", "i32", andv(v, 0xFF000000u), "24");
            writeRegValue(reg, orList({b0, b1, b2, b3}));
            return;
        }
        case Op::Bt:
        case Op::Bts:
        case Op::Btr:
        case Op::Btc: emitBitTest(in); return;
        case Op::Bsf:
        case Op::Bsr: emitBitScan(in); return;
        case Op::Sahf: {
            const std::string ah = andv(bin("lshr", "i32", loadReg(0), "8"), 0xFFu);
            const std::string eflags = loadEflags();
            // eflags = (eflags & ~0xD5) | (ah & 0xD5) | 0x2
            storeEflags(bin("or", "i32",
                            bin("or", "i32", andv(eflags, ~0xD5u), andv(ah, 0xD5u)),
                            "2"));
            return;
        }
        case Op::Lahf: {
            const std::string v = bin("or", "i32", andv(loadEflags(), 0xD5u), "2");
            writeRegValue(Reg::AH, v);
            return;
        }
        case Op::Cld: storeEflags(andv(loadEflags(), ~kDf)); return;
        case Op::Std: storeEflags(bin("or", "i32", loadEflags(), i32c(kDf))); return;
        case Op::Setcc: {
            const std::string eflags = loadEflags();
            const std::string cond = condValue(in.cc, eflags);
            writeDest(in.op0(), select1(cond, "i32", "1", "0"));
            return;
        }
        case Op::Cmovcc: {
            const std::string eflags = loadEflags();
            const std::string cond = condValue(in.cc, eflags);
            const std::string cur = readRegValue(in.op0().reg);
            const std::string src = readValue(in.op1());
            writeDest(in.op0(), select1(cond, "i32", src, cur));
            return;
        }
        case Op::Leave: {
            const std::string ebp = loadReg(5);
            const std::string saved = loadAt(ebp);
            storeReg(5, saved);
            storeReg(4, bin("add", "i32", ebp, "4"));
            return;
        }
        default: emitTrap(in, "T1a 未实现的 op");
    }
}

void FnEmit::emitBlock(const GenBlock& block) {
    // ★ G3 修的一个**既有**缺陷：`trapped_` 的语义是"**本块**里已经发过 `unreachable`，后面不许再发
    //   指令"（P5：`unreachable` 之后跟指令 = 非法 IR）。但它原来是**跨块**留着的 ⇒ 只要某个块以
    //   停机类指令（`hlt/int3/int` 的 `lift_one(terminating=true)`）收尾，同一函数里**后面每个块**
    //   都会在 `if (trapped_) return;` 处提前返回 ⇒ 块没有终结指令 ⇒ clang 报
    //   `expected instruction opcode`（G3 实测：`bb_50C5C6` 是 `int`，紧随其后的 `bb_50C694` 就断了）。
    //   每个基本块是独立的 IR 块 ⇒ 这里按块复位。
    trapped_ = false;
    put("bb_" + hexEa(block.ea) + ":\n");
    for (std::size_t i = 0; i < block.insns.size(); ++i) {
        const Insn& in = block.insns[i];
        const bool last = (i + 1 == block.insns.size());
        if (last && block.terminated) {
            // ---- 控制流收尾 ----
            // ★ 控制转移（`jmp/jcc/call/ret`）由生成器自己 inline / 回 trampoline。
            //   停机类（`hlt/int3/int`）**不是**控制转移 ⇒ 交给 `lift_one`（解释器在那里可报停机）。
            if (in.op == Op::Hlt || in.op == Op::Int3 || in.op == Op::Int) {
                emitLiftOne(in, /*terminating=*/true);
                return;
            }
            if (const char* reason = unsupportedReason(in)) {
                // 控制转移走不到自己的 inline 路径 ⇒ 生成器内部不一致 ⇒ 仍然**响亮 trap**
                // （父级裁决：⛔ 不许让控制流借 `lift_one` 绕过去 —— 那会丢掉 eip 的变化）。
                flushTick();
                emitTrap(in, reason);
                return;
            }
            ++pending_ticks_;  // 控制转移指令本身也是一条已执行指令
            flushTick();       // 块边界 ⇒ 把本块挂账的静态条数一次发出去
            emitTerminator(in);
            return;
        }
        if (const char* reason = unsupportedReason(in)) {
            // ★ T1c 的**默认路径**：不 inline 的 op ⇒ 单指令宿主原语（语义只有解释器那一份）。
            (void)reason;
            emitLiftOne(in, /*terminating=*/false);
            if (trapped_) return;
            continue;
        }
        emitInsn(in);
        ++pending_ticks_;
        if (trapped_) return;  // `emitInsn` 内部的 trap（生成器不一致）⇒ 该块就地收尾
    }
    // 没以控制流收尾 ⇒ 落空到下一个 leader（它一定在 blocks 里）。
    const Insn& last = block.insns.back();
    flushTick();  // 块边界：把本块挂账的静态条数一次发出去
    emitEdge(last.ea + last.length);
}

std::string FnEmit::run() {
    std::vector<std::uint32_t> entries = comp_.entries;
    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());

    put("define i32 @" + genFunctionName(comp_.root) + "(ptr %st, ptr %mem, i32 %ea, ptr %cpu) {\n");
    put("entry:\n");
    for (int i = 0; i < 9; ++i) {
        put("  %p_s" + std::to_string(i) + " = getelementptr i8, ptr %st, i64 " +
            std::to_string(4 * i) + "\n");
    }
    put("  switch i32 %ea, label %bad [\n");
    for (std::uint32_t ea : entries) {
        put("    i32 " + i32c(ea) + ", label %bb_" + hexEa(ea) + "\n");
    }
    put("  ]\n\n");

    put("bad:\n");
    add("call void @llvm.trap()");
    add("unreachable");
    ++stats_->traps_bad;

    for (const auto& kv : comp_.blocks) {
        put("\n");
        emitBlock(kv.second);
    }
    for (std::uint32_t ea : exit_ea_) {
        put("\nbb_exit_" + hexEa(ea) + ":\n");
        add("ret i32 " + i32c(ea));
    }
    // ★ `lift_one` 的停机出口：`lift_one` 返回 false ⇒ `ret i32 kLiftHalt`
    //   （trampoline 见到它就按 `cpu.stop_kind` / `stop_why` 报停机，⛔ 不许当成"跑完了"）。
    //   统一在函数尾发：块中间发 `ret` 会让后面的指令变成新块的第一条。
    for (const std::string& label : halt_labels_) {
        put("\n" + label + ":\n");
        add("ret i32 " + i32c(::kLiftHalt));
    }
    put("}\n");
    return body_;
}

}  // namespace

// =====================================================================================
// 块的后继边（分区与发射共用；唯一一份）
// =====================================================================================
std::string genFunctionName(std::uint32_t root) { return "lift_cc_" + hexEa(root); }

std::uint32_t genBranchTarget(const lift::Insn& insn) {
    if (insn.op0().kind != OpKind::Imm) return 0;
    return static_cast<std::uint32_t>(static_cast<std::int64_t>(insn.ea) + insn.length +
                                      static_cast<std::int64_t>(
                                          static_cast<std::int32_t>(static_cast<std::uint32_t>(
                                              insn.op0().imm))));
}

std::vector<GenEdge> genBlockEdges(const GenBlock& block) {
    std::vector<GenEdge> out;
    if (block.insns.empty()) return out;
    const lift::Insn& last = block.insns.back();
    if (!block.terminated) {
        out.push_back(GenEdge{last.ea + last.length, false, false});
        return out;
    }
    switch (last.op) {
        case Op::Call:
            out.push_back(GenEdge{last.ea + last.length, true, false});
            break;
        case Op::Jmp:
            if (last.op0().kind == OpKind::Imm) out.push_back(GenEdge{genBranchTarget(last), false, false});
            else out.push_back(GenEdge{0, false, true});
            break;
        case Op::Jcc:
            out.push_back(GenEdge{genBranchTarget(last), false, false});
            out.push_back(GenEdge{last.ea + last.length, false, false});
            break;
        case Op::Ret: out.push_back(GenEdge{0, false, true}); break;
        default: break;  // Hlt/Int3/Int：没有静态后继（本轮也只会生成 trap）
    }
    return out;
}

bool emitModule(const std::vector<GenComponent>& components,
                const std::set<std::uint32_t>& entry_set,
                const std::set<std::uint32_t>& dropped_blocks, const GenClosureStats& closure,
                std::string* out, GenEmitStats* stats,
                std::vector<GenUnsupported>* unsupported) {
    if (out == nullptr || stats == nullptr || unsupported == nullptr) return false;
    std::string body;
    for (const GenComponent& comp : components) {
        FnEmit emit(comp, entry_set, dropped_blocks, stats, unsupported);
        body += emit.run();
        body += "\n";
    }
    std::string text;
    text += "; emulator-lift · Shape A / T1c —— 由 `emulator-lift-gen` 生成，⛔ 不手改、不 commit。\n";
    text += "; 契约：%st = LiftRegs*（r[0..7] + eflags@+32）；%mem = guest EA 0 的宿主基址；\n";
    text += ";       %ea = resume EA；%cpu = X86*；返回值 = 下一个 guest EA。\n";
    text += "; 不 inline 的指令 = 一次 @lift_one（语义在解释器里）；\n";
    text += "; 指令计数 = 块边界 / 宿主调用前的 @lift_tick（D2 / D2b：只记条数，不推进虚拟时间）。\n";
    // ★ D14：静态调用闭包的读数写进产物头部 —— 拿到一份 `.ll` 就能看出它收了多少软根、
    //   丢了多少（⛔ 不让"生成了什么"只存在于 stderr 里）。数字全部由输入确定 ⇒ 同输入同字节。
    text += "; 静态调用闭包（D14；--no-close-calls 关）= ";
    text += closure.close_calls ? "on" : "off";
    text += "：soft-roots=" + std::to_string(closure.soft_roots);
    text += "；丢弃 越界=" + std::to_string(closure.soft_outside);
    text += " 解不出=" + std::to_string(closure.soft_undecodable);
    text += " 重叠=" + std::to_string(closure.soft_overlap);
    text += "；shim 目标跳过=" + std::to_string(closure.shim_targets) + "\n";
    // ★ D15：第二类软根（写内存 mov 的 imm32）的读数**独立一行**，三档丢弃同样各一个数
    //   （⛔ 不与 D14 那行合并：两类软根的账要能分别读）。
    text += "; 代码指针立即数（D15；--no-close-stores 关）= ";
    text += closure.close_stores ? "on" : "off";
    text += "：soft-roots=" + std::to_string(closure.store_soft_roots);
    text += "；丢弃 越界=" + std::to_string(closure.store_outside);
    text += " 解不出=" + std::to_string(closure.store_undecodable);
    text += " 重叠=" + std::to_string(closure.store_overlap);
    text += "；shim 目标跳过=" + std::to_string(closure.store_shim_targets);
    text += "；写内存 mov 的 imm32 站点=" + std::to_string(closure.store_imm_sites);
    text += "（imm 落在可执行节内=" + std::to_string(closure.store_imm_in_exec) + "）\n";
    // ★ D17：第三类软根（非可执行节里静态初始化的代码指针 = `.data` 指针表）的读数**独立一行**，
    //   三档丢弃同样各一个数（⛔ 不与 D14/D15 那两行合并：三类的账要能分别读）。
    text += "; 静态代码指针（D17；--no-close-data 关）= ";
    text += closure.close_data ? "on" : "off";
    text += "：soft-roots=" + std::to_string(closure.data_soft_roots);
    text += "；丢弃 越界=" + std::to_string(closure.data_outside);
    text += " 解不出=" + std::to_string(closure.data_undecodable);
    text += " 重叠=" + std::to_string(closure.data_overlap);
    text += "（其中收之前就挡下=" + std::to_string(closure.data_overlap_pre) + "）";
    text += "；shim 目标跳过=" + std::to_string(closure.data_shim_targets);
    text += "；非可执行节的 4 字节对齐 dword 站点=" + std::to_string(closure.data_ptr_sites);
    text += "（值落在可执行节内=" + std::to_string(closure.data_ptr_in_exec);
    text += "，不同候选=" + std::to_string(closure.data_ptr_distinct);
    text += "，已在入口集/已判过=" + std::to_string(closure.data_ptr_skipped) + "）\n";
    // ★ G3：分区收敛的护栏读数（⛔ 不是软根）：入口 EA 里**没有块**、因而不可能成为 unit 根的那些
    //   （落在别人块的字节区间内 / 解不出）⇒ 退出入口集，指向它们的边发 `ret i32`。
    //   这一行是为了"⛔ 不静默"：不写它的话，覆盖面缩小这件事在产物里看不见。
    text += "; 入口集收敛护栏（G3）：没有块的入口 EA=" + std::to_string(closure.unbuildable_entries);
    text += "（⇒ 退出入口集、指向它的边发 ret i32）\n";
    // ★ D21（第四类）：可执行节里的跳表 dword = 已建块首 EA、但不在入口集 ⇒ **只加 entries**
    //   （switch case + 派发表行），⛔ 0 新块 / 0 新指令。这一行让"加了几条"在产物里看得见。
    text += "; 跳表入口（D21）：added=" + std::to_string(closure.jump_table_added);
    text += "（只加 entries；⛔ 0 新块 / 0 新指令）";
    text += "；可执行节 4 字节对齐 dword 站点=" + std::to_string(closure.jump_table_sites);
    text += "（值=已建块首=" + std::to_string(closure.jump_table_hits);
    text += "，不同目标=" + std::to_string(closure.jump_table_distinct) + "）";
    text += "；已在入口集=" + std::to_string(closure.jump_table_skipped);
    text += "；孤儿提升=" + std::to_string(closure.jump_table_unclaimed) + "\n";
    // ★ D26（第五类）：可执行节里"值指向**尚未解码**的代码"的 dword ⇒ **新增软根 = 新增块**
    //   （与上一条 D21 相反：D21 只加 entry、0 新块）。候选面五档相加 = `不同候选`。
    text += "; 可执行节跳表目标（D26；--no-close-jmptab 关）= ";
    text += closure.close_jmptab ? "on" : "off";
    text += "：soft-roots=" + std::to_string(closure.jumptab_soft_roots);
    text += "（新增 unit 根 = **会新增块**；最终仍在入口集=" + std::to_string(closure.jumptab_live_roots) + "）";
    text += "；丢弃 越界=" + std::to_string(closure.jumptab_outside);
    text += " 解不出=" + std::to_string(closure.jumptab_undecodable);
    text += " 重叠=" + std::to_string(closure.jumptab_overlap);
    text += "；shim 目标跳过=" + std::to_string(closure.jumptab_shim_targets);
    text += "；不动点轮数=" + std::to_string(closure.jumptab_rounds);
    text += "；候选面：站点=" + std::to_string(closure.jumptab_sites);
    text += "（值在可执行节内=" + std::to_string(closure.jumptab_in_exec);
    text += "，不同候选=" + std::to_string(closure.jumptab_distinct);
    text += "，已在入口集=" + std::to_string(closure.jumptab_in_entries);
    text += "，已是块首=" + std::to_string(closure.jumptab_block_starts);
    text += "，可解码=" + std::to_string(closure.jumptab_decodable);
    text += "，解不出=" + std::to_string(closure.jumptab_undecodable_pre);
    text += "，被重叠挡下=" + std::to_string(closure.jumptab_overlap_pre);
    text += "，shim 桩=" + std::to_string(closure.jumptab_shim) + "）\n";
    text += "\n";
    if (stats->used_note_write) text += "declare void @lift_note_write(ptr, i32, i32)\n";
    if (stats->used_tick) text += "declare void @lift_tick(ptr, i32)\n";
    if (stats->used_lift_one) text += "declare i1 @lift_one(ptr, ptr, i32)\n";
    text += "declare void @llvm.trap()\n";
    if (stats->used_ctpop) text += "declare i32 @llvm.ctpop.i32(i32)\n";
    if (stats->used_cttz) text += "declare i32 @llvm.cttz.i32(i32, i1)\n";
    if (stats->used_ctlz) text += "declare i32 @llvm.ctlz.i32(i32, i1)\n";
    text += "\n";
    text += body;
    *out = std::move(text);
    return true;
}

}  // namespace aot
}  // namespace lift
