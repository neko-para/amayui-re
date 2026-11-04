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
bool x87IsInf(const Ext80 a);
bool x87SignBit(const Ext80 a);

// `fxam` 的分类（Empty 由调用方按 tag word 先判掉 —— 桥这层看不见 tag）
enum class X87Class : std::uint8_t { Nan = 0, Infinity, Zero, Denormal, Normal };
X87Class x87Classify(const Ext80 a);

// `fxtract`：significand = [1,2) 的尾数（指数域 = bias），exponent = 带符号整数（extF80 表示）
void x87Extract(const Ext80 a, Ext80 significand, Ext80 exponent);
// `fprem`：**商截断**的余数（IEEE 的 `extF80_rem` 是商取最近偶数 ⇒ 那是 `fprem1`）
void x87RemTrunc(const Ext80 a, const Ext80 b, Ext80 out);

// 超越函数 / 宿主 libm 的唯一进出口（见 .cpp 顶部的 1 ulp 登记）
double x87ToDouble(const Ext80 a);
void x87FromDouble(double d, Ext80 out);

// 异常标志（`extF80_*` 只置 `softfloat_exceptionFlags`；取走 / 清空由我们这层做）
std::uint8_t x87PeekExceptionFlags();
void x87ClearExceptionFlags();
void x87RaiseInvalid();

// 控制字 / 状态字的映射（PC = 精度控制，RC = 舍入控制）
void x87ApplyControlWord(std::uint16_t control);
std::uint8_t x87TakeExceptionFlags();  // 取走并清空（对应 x87 的"读状态字"）

}  // namespace lift
