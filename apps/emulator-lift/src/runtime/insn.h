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

    // ---- x87（S4：语料里 47 个形态；算术全部走 SoftFloat `extF80`，见 plan §4 D3）----
    Fld, Fst, Fstp, Fild, Fist, Fistp, Fisttp,
    Fadd, Fsub, Fsubr, Fmul, Fdiv, Fdivr,
    Faddp, Fsubp, Fsubrp, Fmulp, Fdivp, Fdivrp,
    Fiadd, Fisub, Fisubr, Fimul, Fidiv, Fidivr,
    Fcom, Fcomp, Fucom, Fucomp, Fcompp, Fucompp,
    Fcomi, Fcomip, Fucomi, Fucomip,
    Fcmovb, Fcmove, Fcmovbe, Fcmovu, Fcmovnb, Fcmovne, Fcmovnbe, Fcmovnu,
    Fnstsw, Fldcw, Fnstcw, Fldenv, Fnstenv, Fsave, Fnsave, Frstor,
    Fxch, Ffree, Fdecstp, Fincstp, Fnop,
    Fld1, Fldz, Fldpi, Fldl2e, Fldl2t, Fldlg2, Fldln2,
    Fchs, Fabs, Fninit, Fclex, Fwait,
    Ftst, Fsqrt, Frndint, Fprem, Fprem1, Fscale, Fxam, Fxtract,
    Fsin, Fcos, Fsincos, Fptan, Fpatan, Fyl2x, Fyl2xp1, F2xm1,
    //  ↑ x87 到此为止（`exec.cpp` 的 `opIsX87` 用这段连续区间判定；新 x87 形态必须加在这条线以上）

    // ---- MMX / SSE（整数打包；子语义见 `PkOp`）----
    // ★ 语料里只有 MMX 形态（≈0.94k 条），所以这里登记的是"整数 lane"语义，不是浮点。
    Packed,

    // ---- SSE 标量单精度 + 128 位搬移 / 位运算（子语义见 `SseOp`）----
    // ★ 与 `Packed` **分开一族**：浮点形态带 NaN / 舍入 / (MXCSR 的) invalid 语义，
    //   塞进 `PkOp` 的整数语义里必然把"按 lane 的按位 / 整数"与"标量浮点"混成一套。
    // ★ 语料里这 15 条形态都是 0 条（`.lst` 里 `xmm<数字>` 操作数 = 0），但 A2 的别名表登记了它们；
    //   真撞到时按 Intel SDM 执行（不是 `Op::Other` 响亮失败）。
    // ★ `Movsd` 是**最后追加**的（不插中间：既有枚举值有别的消费方，⛔ 不许移动）。
    Sse,

    Other,  // 解出来了但没实现 —— 执行必须响亮失败
};

// `Op::Packed` 的子语义（Intel SDM Vol.2 的家族名；MMX 与 SSE2 整数形态共用一套实现，
// 区别只在执行宽度：MM 寄存器/内存 = 8 字节，XMM = 16 字节）。
enum class PkOp : std::uint8_t {
    None = 0,
    Movd, Movq,  // mm/xmm ↔ r/m32 / r/m64（写进向量寄存器时**零扩展**）
    Pand, Pandn, Por, Pxor,
    PaddB, PaddW, PaddD, PaddQ,
    PaddSb, PaddSw, PaddUsb, PaddUsw,
    PsubB, PsubW, PsubD, PsubQ,
    PsubSb, PsubSw, PsubUsb, PsubUsw,
    PcmpeqB, PcmpeqW, PcmpeqD,
    PcmpgtB, PcmpgtW, PcmpgtD,
    Punpcklbw, Punpcklwd, Punpckldq, Punpcklqdq,
    Punpckhbw, Punpckhwd, Punpckhdq, Punpckhqdq,
    Packsswb, Packssdw, Packuswb,
    Pmullw, Pmulhw, Pmulhuw, Pmuludq, Pmaddwd,
    Psllw, Pslld, Psllq, Psrlw, Psrld, Psrlq, Psraw, Psrad,
    Ems,
};

// `Op::Sse` 的子语义（Intel SDM Vol.2 的家族名；只登记 A2 别名表里那 15 条形态）。
// ★ 语义边界（逐条在 `exec_fpu.cpp` 的 `execSse()` 旁写清 SDM 依据）：
//   · 标量形态（movss / movsd / cvtsi2ss / addss · subss · mulss · divss）只改低 32（movsd = 低 64）位，
//     **高位原样保留**；例外是内存源形态 `movss xmm, m32` / `movsd xmm, m64` —— 高位**清 0**。
//   · `comiss` / `ucomiss` 写 EFLAGS（不是 x87 的 C0..C3）。
// ★ `Movsd` 必须留在**末尾**：解码器按操作数形态分派它（`F2 0F 10/11` vs 串操作 `A5`，见 decode.cpp）。
enum class SseOp : std::uint8_t {
    None = 0,
    Movaps, Movups, Movss, Movdqa, Movdqu,  // 搬移（对齐要求见 execSse）
    Xorps,                                  // 128 位**按位**异或（不是浮点运算）
    Cvtsi2ss, Cvttss2si,                    // 转换（cvttss2si 是**截断**）
    Addss, Subss, Mulss, Divss,             // 标量四则
    Comiss, Ucomiss,                        // 标量比较 → EFLAGS
    Movsd,                                  // ★ 追加（SSE2 标量 double；与串操作 `movsd` 助记符碰撞）
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

// ★ `Xmm` 是**独立**的一类：`Reg` 枚举里没有 XMM（那是 `regs.h` 的既有接口，S4 不改它），
//   所以 XMM 操作数用 `Operand::vec`（0..7）承载下标。
enum class OpKind : std::uint8_t { None = 0, Reg, Imm, Mem, Xmm };

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
    std::uint8_t size = 0;  // 字节宽度（1/2/4/8/10/16）
    std::uint64_t imm = 0;  // 立即数（已按操作数宽度截断）
    std::uint8_t vec = 0;   // `OpKind::Xmm` 用：xmm 下标 0..15（32 位 guest 只会有 0..7）
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
    PkOp pk = PkOp::None;      // `Op::Packed` 的子语义
    SseOp sse = SseOp::None;   // `Op::Sse` 的子语义
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
    // ★ profile（默认 0；只有 `--profile` 才累加）：本块被 dispatcher 取到的次数。
    //   块级计数是"零查找"的：加在缓存实体上，热路径只多一次自增（⛔ 不做 EA→map 查找）。
    std::uint64_t hits = 0;
    // ★ 墙钟（只有 --profile 才累加）：块执行耗时，供"总耗时 >1s 的函数"排序。
    std::uint64_t ns = 0;
};

}  // namespace lift
