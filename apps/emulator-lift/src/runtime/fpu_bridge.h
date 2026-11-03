// emulator-lift · x87 ↔ Berkeley SoftFloat（`extF80`）的桥
//
// ★ D3 的落点：**算术内核**用 vendor 的 SoftFloat（逐位可复现），我们只负责
//   寄存器栈 / tag / 控制字 / 条件码 / 超越函数（后者 M1b）。
// ★ 值在 guest 寄存器里存 softfloat 的 `extFloat80_t` 原位字节（16 字节槽，前 10 字节有效）；
//   与 x86 的 10 字节内存格式互转只在本文件里做一次（`memToReg` / `regToMem`）。
// ⚠ 已知边界（plan §4 D3）：SoftFloat 不复现 x87 的 pseudo-denormal / unnormal 表示；
//   本作 x87 全在正规值路径上，命中概率极低 —— 真需要复现就在本文件加判断。
#pragma once

#include <cstdint>

namespace lift {

// 80 位扩展精度值的**不透明槽位**（16 字节；内容 = softfloat extFloat80_t）
using Ext80 = std::uint8_t[16];

// 内存 ↔ 寄存器
void x87MemToReg(const std::uint8_t* mem, Ext80 out, std::uint8_t size);  // size = 4 / 8 / 10
void x87RegToMem(const Ext80 in, std::uint8_t* mem, std::uint8_t size);

// 整数 ↔ ext80
void x87FromI16(std::int16_t v, Ext80 out);
void x87FromI32(std::int32_t v, Ext80 out);
void x87FromI64(std::int64_t v, Ext80 out);
void x87FromU32(std::uint32_t v, Ext80 out);
void x87FromU64(std::uint64_t v, Ext80 out);
// 舍入取整（按当前控制字）；`truncate` = 朝零截断（fisttp 语义）
std::int32_t x87ToI32(const Ext80 in, bool truncate);
std::int64_t x87ToI64(const Ext80 in, bool truncate);

// 常量
void x87One(Ext80 out);
void x87Zero(Ext80 out);
void x87Pi(Ext80 out);
void x87Ln2(Ext80 out);
void x87Lg2(Ext80 out);
void x87L2e(Ext80 out);
void x87L2t(Ext80 out);
void x87Ln10(Ext80 out);

// 算术：out = a op b（`out` 可以与 `a` / `b` 同一块内存）
void x87Add(const Ext80 a, const Ext80 b, Ext80 out);
void x87Sub(const Ext80 a, const Ext80 b, Ext80 out);
void x87Mul(const Ext80 a, const Ext80 b, Ext80 out);
void x87Div(const Ext80 a, const Ext80 b, Ext80 out);
void x87Sqrt(const Ext80 a, Ext80 out);
void x87RoundToInt(const Ext80 a, Ext80 out);
void x87Rem(const Ext80 a, const Ext80 b, Ext80 out);
void x87Scale(const Ext80 a, const Ext80 b, Ext80 out);
void x87ChangeSign(Ext80 v);
void x87Abs(Ext80 v);

// 比较：-1 (a<b) / 0 (相等) / 1 (a>b) / 2 (无序，NaN)
int x87Compare(const Ext80 a, const Ext80 b);
bool x87IsNan(const Ext80 a);
bool x87IsZero(const Ext80 a);

// 控制字 / 状态字的映射（PC = 精度控制，RC = 舍入控制）
void x87ApplyControlWord(std::uint16_t control);
std::uint8_t x87TakeExceptionFlags();  // 取走并清空（对应 x87 的"读状态字"）

}  // namespace lift
