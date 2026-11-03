// emulator-lift · 自有指令表示（解码器的输出 / 语义层的输入）
//
// ★ 边界：Zydis 只**解码**，语义全在这里定义的 `Op` 上（D2：不生成代码，运行时解码 + 块缓存）。
// ★ `Insn` 里只放**已经归一化**的东西：操作数（寄存器 / 立即数 / 内存）、有效宽度、前缀。
//   语义层因此不需要知道任何 x86 编码细节（ModRM / SIB / 前缀顺序）。
// ⛔ 没有 `Op` 就执行不了 —— 那是 `Op::Other`，dispatcher 必须**响亮失败**并打印助记符与 EA，
//   ⛔ 不许 no-op（README §1.1 / SCOPE §3）。
#pragma once

#include <cstdint>
#include <vector>

#include "runtime/regs.h"

namespace lift {

// 指令语义的**闭集合**：这里没有的助记符一律 `Op::Other`（执行即响亮失败）。
enum class Op : std::uint16_t {
    Invalid = 0,

    // ---- 数据搬运 ----
    Mov, Movzx, Movsx, Lea, Xchg, Push, Pop, Pushad, Popad, Pushfd, Popfd,
    Xadd, Cmpxchg,

    // ---- 算术 / 逻辑 ----
    Add, Or, And, Sub, Xor, Cmp, Test, Inc, Dec, Neg, Not, Adc, Sbb,

    // ---- 移位 / 循环移位 ----
    Shl, Shr, Sar, Rol, Ror, Rcl, Rcr, Shld, Shrd,

    // ---- 乘除 / 符号扩展 ----
    Mul, Imul, Div, Idiv, Cdq, Cwd, Cwde, Cbw, Bswap,

    // ---- 位测试 ----
    Bt, Bts, Btr, Btc, Bsf, Bsr,

    // ---- 控制流 ----
    Jmp, Jcc, Call, Ret, Leave, Nop, Hlt, Int3, Int, Setcc, Cmovcc,
    Cpuid, Sahf, Lahf, Pause,

    // ---- 串操作（`rep` 前缀见 Insn::rep）----
    Movs, Stos, Lods, Scas, Cmps,

    // ---- x87（M1a 只做最小子集；其余 M1b）----
    Fld, Fst, Fstp, Fild, Fist, Fistp,
    Fadd, Fsub, Fsubr, Fmul, Fdiv, Fdivr,
    Faddp, Fsubp, Fsubrp, Fmulp, Fdivp, Fdivrp,
    Fiadd, Fisub, Fisubr, Fimul, Fidiv, Fidivr,
    Fcom, Fcomp, Fucom, Fucomp, Fcompp, Fucompp,
    Fnstsw, Fldcw, Fnstcw, Fxch, Ffree, Fld1, Fldz, Fchs, Fabs, Fninit, Fwait,
    Ftst, Fsqrt, Frndint, Fprem, Fscale, Fsin, Fcos, Fptan, Fpatan, Fyl2x,
    Fldln2, Fldl2e, Fldlg2, Fldl2t, Fldpi, F2xm1,

    Other,  // 解出来了但没实现 —— 执行必须响亮失败
};

// 条件码（Jcc / Setcc / Cmovcc）。别名（jz/je、jnb/jae/jnc …）**在解码时**归一到这里。
enum class Cond : std::uint8_t {
    None = 0,
    O,   // 溢出（jo）
    NO,  // jno
    B,   // 低于（jb / jc / jnae）
    AE,  // 高于或等于（jae / jnb / jnc）
    E,   // 等于（je / jz）
    NE,  // 不等于（jne / jnz）
    BE,  // 低于或等于（jbe / jna）
    A,   // 高于（ja / jnbe）
    S,   // 符号（js）
    NS,  // jns
    P,   // 奇偶（jp / jpe）
    NP,  // jnp / jpo
    L,   // 小于（jl / jnge）
    GE,  // jge / jnl
    LE,  // jle / jng
    G,   // jg / jnle
};

enum class OpKind : std::uint8_t { None = 0, Reg, Imm, Mem };

// 内存操作数：有效地址 = base + index*scale + disp（没有 base/index 时用 disp 作绝对地址）
struct MemOperand {
    Reg base = Reg::None;   // 32 位 GPR 或 None
    Reg index = Reg::None;  // 32 位 GPR 或 None
    std::uint8_t scale = 1; // 1 / 2 / 4 / 8
    std::uint8_t size = 4;  // 访问宽度（字节）
    bool absolute = false;  // true ⇒ 只有 disp（绝对地址，如 `[0051DA0C]`）
    bool seg_fs = false;    // fs: 前缀（TEB / TLS）
    bool seg_gs = false;    // gs: 前缀（本作基本不用）
    std::uint32_t disp = 0; // 32 位补码，按无符号保存
};

struct Operand {
    OpKind kind = OpKind::None;
    Reg reg = Reg::None;
    std::uint8_t size = 0;  // 字节宽度（1/2/4/8/10）
    std::uint64_t imm = 0;  // 立即数（已按操作数宽度截断）
    MemOperand mem;
};

struct Insn {
    std::uint32_t ea = 0;      // 这条指令在 guest 里的 EA
    std::uint8_t length = 0;   // 编码长度（A3 的"边界"判据就是它）
    std::uint8_t opsz = 4;     // 有效操作数宽度（2 或 4）—— 16 位前缀在 32 位代码里几乎不出现
    std::uint8_t rep = 0;      // 0 = 无；0xF3 = rep/repe；0xF2 = repne
    bool lock = false;
    bool operand16 = false;    // 0x66 前缀
    Op op = Op::Invalid;
    Cond cc = Cond::None;      // Jcc / Setcc / Cmovcc 用
    Operand ops[3];
    const char* mnemonic = ""; // 规范助记符（静态字符串，与 .lst 对照用）
    std::uint16_t zydis_mnemonic = 0;  // 仅诊断用

    const Operand& op0() const { return ops[0]; }
    const Operand& op1() const { return ops[1]; }
    const Operand& op2() const { return ops[2]; }
};

const char* opName(Op op);
const char* condName(Cond cc);
// 条件码是否满足（读 X86::eflags —— 由调用方传入，避免反向依赖）
bool condHolds(Cond cc, std::uint32_t eflags);

// 一个基本块：从块首开始解码，遇到控制流指令（jcc/jmp/call/ret/int…）**含**它结束。
// ★ 它只是解码缓存，不是执行单位的所有权 —— 执行仍回 dispatcher（README §2.2）。
struct Block {
    std::uint32_t ea = 0;
    std::vector<Insn> insns;
    bool terminated = false;   // 最后一条是不是控制流
    // shim 块：这个 EA 是导入桩页里的一个唯一地址（S5），执行时不走指令
    bool is_shim = false;
    std::uint32_t shim_id = 0;
};

}  // namespace lift
