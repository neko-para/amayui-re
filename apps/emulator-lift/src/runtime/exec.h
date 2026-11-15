// emulator-lift · 语义层的分发（每条指令一个 `Op`，按族落 `exec_*.cpp`）
//
// ★ 返回值：`false` = **停止执行**（已经打印了诊断）。控制流指令**不算**停止 ——
//   它们只改 `eip` 并让 dispatcher 重新取块（README §2.2）。
// ⛔ 没实现的操作码不许 no-op：`unimplemented()` 会打印 EA + 助记符 + 寄存器 + guest 返回栈，
//   然后返回 false（plan §6 S3 的"响亮失败"）。
#pragma once

#include <cstdint>

#include "runtime/insn.h"

namespace lift {

struct X86;

bool execInsn(X86& cpu, const Insn& in);

bool execInteger(X86& cpu, const Insn& in);   // 数据搬运 / 算术 / 逻辑 / 移位 / 乘除 / 位
bool execControl(X86& cpu, const Insn& in);   // jcc / jmp / call / ret / setcc / cmovcc / 中断
bool execString(X86& cpu, const Insn& in);    // movs / stos / lods / scas / cmps + rep
bool execFpu(X86& cpu, const Insn& in);       // x87（S4：语料里 47 个形态）
bool execPacked(X86& cpu, const Insn& in);    // MMX / SSE 整数打包（`Op::Packed`，实现落在 exec_fpu.cpp）
bool execSse(X86& cpu, const Insn& in);       // SSE 标量单精度 + 128 位搬移（`Op::Sse`，实现落在 exec_fpu.cpp）

// ★ 检查点 2（D49）：AOT 生成代码的 x87 宿主 helper 入口。
//   `lift_x87_sem(cpu, op)` = 对**无内存操作数**的 x87 op 调解释器的同一份实现
//   （`execFpu` 配一个只带 `op` 的 `Insn`）⇒ 超越函数 / fprem / fscale / fxtract / fxam 的
//   语义**只有一处**（⛔ 不在发射器里重写公式）。定义在 `exec_fpu.cpp`。
//   ⛔ 它不是"第二个 lift_one"：不解码、不过 dispatcher、不碰 eip；只有编译期已知的 op 码。
bool execX87NoOperand(X86& cpu, Op op);

// 控制流指令（块在这里收尾）
bool opIsControl(Op op);
// x87 家族（`Op::Fld .. Op::F2xm1` 的连续区间）
bool opIsX87(Op op);
// 是否已实现（`Op::Other` 与 M1b 的族返回 false）
bool opImplemented(Op op);

// 响亮失败：打印诊断并返回 false
bool unimplemented(X86& cpu, const Insn& in, const char* why);

// 标志位辅助（算术结果 → PF / ZF / SF / AF 的公共部分）
void setLogicFlags(X86& cpu, std::uint32_t result, std::uint8_t size);
void setAddFlags(X86& cpu, std::uint32_t a, std::uint32_t b, std::uint32_t result, std::uint8_t size, bool with_carry);
void setSubFlags(X86& cpu, std::uint32_t a, std::uint32_t b, std::uint32_t result, std::uint8_t size, bool with_borrow);

// PF 的**唯一一份**判据（SDM：低 8 位的 popcount 为偶 ⇒ true）。
// ★ 为什么提到这里（D10 P1）：原先仓里有三份 `0x6996` 表，其中两份（`exec_integer.cpp` 的
//   `parityFromLow` / `exec_string.cpp` 的 `par`)少了取反 ⇒ 移位 / `scas`·`cmps` 之后的 PF
//   是**奇**校验，与生成器（`lift_gen_emit.cpp` 的 `parity()`，SDM 偶校验）**相反**。
//   ⛔ 不要再在任何 `exec_*.cpp` 里另写一份。
bool parityEven(std::uint8_t v);

// 打印寄存器 + guest 返回栈（fault 诊断 / 未实现诊断共用；`depth` 是栈上回看的字数量）
void dumpRegisters(const X86& cpu, std::FILE* out);
void dumpGuestStack(const X86& cpu, std::FILE* out, int depth);
// 打印 EA 处指令（优先 `.lst` 文本；没有就现场反汇编）
void dumpAtEa(X86& cpu, std::uint32_t ea, std::FILE* out);

}  // namespace lift
