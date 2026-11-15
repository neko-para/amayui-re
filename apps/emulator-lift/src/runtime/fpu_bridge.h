// emulator-lift · x87 的**实数模型**（D49：统一用 `double`）
//
// ★ D49（用户裁决，2026-11-15）：「这里 x87 统一用 double 模拟实现。不追求 80bit 完全一致。」
//   于是本文件从"Berkeley SoftFloat `extF80` 的桥"降级成"一份 `double` 模型的纯函数集合"：
//   **一份模型，两臂共用** —— 解释器（`exec_fpu.cpp`）调这里的宿主函数；生成代码把同一套语义
//   发射成**普通 LLVM 浮点指令**（`src/aot/lift_gen_emit.cpp` 的 `emitX87*`）。
//   ⛔ 不许"AOT 用 double、解释器留 extF80"：那会造出两个 x87 模型，差分从此对 x87 失去意义。
//
// ★ 明确接受的偏差（D49 §3，写下来别当没发生）：
//   1. 精度：尾数 80 位 → **53 位**（`double`）。
//   2. **PC（精度控制）字段无效**：控制字 bit 8-9 照旧能读能写（guest 的 `fldcw`/`fnstcw`
//      会往返它），但它**不影响任何运算**。
//   3. **RC（舍入控制）**：四则 / 开方 / 取余**一律按宿主默认（就近偶数）**，⛔ 不逐 op
//      `fesetround`；但**整数转换**（`fist`/`fistp`）与 `frndint` 按 RC 分对四档
//      （就近 / 向下 / 向上 / 朝零）—— 那是 `(int)` 语义不整体偏移的前提。
//   4. pseudo-denormal / unnormal 表示、NaN 传播 payload、QNaN/SNaN 的部分细节不追。
//   5. **异常位建模 IE / ZE / OE / UE，⛔ PE 恒 0**（见 `kX87*`；任务 0 的裁决，2026-11-15）：
//      - OE（overflow）判据 = **结果是 ±inf 且两个操作数都有限**（见 `raiseOverflowUnderflow`）
//        ⇒ 能把"运算上溢到 inf"与"inf 传播 / 除零"分开，纯 `double` 结构判定即可；
//      - UE（underflow）判据 = **结果是次正规**（`|r| < DBL_MIN && r != 0`）。⚠ 真机还要
//        "且不精确"，本模型没有 PE ⇒ 按结构判（次正规即置），两臂同一份判据；
//      - **PE（inexact）恒 0**：`double` 下几乎每条运算都与真值不等，除软件浮点或每 op 读
//        MXCSR 外无法判定；⛔ 不为此把每条 x87 变回宿主调用。
//        ⇒ **任何按 PE 分支的 guest 路径都会走"精确"那一支**（这是 D49 明确接受的偏差，
//        不是"漏了"）。同理 **SF（stack fault 位）仍恒 0**（真机栈上溢会置 SF；本轮只补 ES）。
//      - **ES（异常汇总）= 状态字里任一异常位**（0x003F）非零，由 `syncStatus` / 发射器的
//        `x87OrStatus` 一起维护；`fnclex` / `fninit` 把它们一起清掉。
//      ★ 两臂（解释器 / 生成代码）必须**逐位相同**：判据只有一份（本文件的
//        `raiseOverflowUnderflow`），生成代码按同一份判据发 IR（`FnEmit::x87RaiseFloatFlags`）。
//
// ★ 谁在用它：`exec_fpu.cpp`（解释器臂）+ 生成代码的宿主 helper（`lift_x87_*`，见 `lift_abi.cpp`）。
//   ⛔ 别再往这里塞寄存器栈语义（push/pop/tag/状态字）：那些在 `exec_fpu.cpp` 与发射器里各有一份，
//     本文件只回答"值怎么算"。
#pragma once

#include <cstdint>

namespace lift {

// x87 的实数（D49：就是 double）
using X87Val = double;

// 舍入控制（控制字 bit 10-11；SDM Vol.1 §8.1.5 的 RC 编码）
enum class X87Round : std::uint8_t { Near = 0, Down = 1, Up = 2, Zero = 3 };

// 异常位（编码与旧 SoftFloat 的 `softfloat_flag_*` **逐位相同** ⇒ `syncStatus` 的映射不变）：
//   1 = PE（精度 / 不精确）—— ⚠ **本模型从不置**（见文件头 §5）
//   2 = UE（下溢）        —— 由 `raiseOverflowUnderflow` 置（结果是次正规）
//   4 = OE（上溢）        —— 由 `raiseOverflowUnderflow` 置（结果是 ±inf 且操作数都有限）
//   8 = ZE（除零）        —— 只由 `x87Div` 置（条件便宜，生成代码可逐位复刻）
//  16 = IE（无效运算）
enum : std::uint8_t { kX87PE = 1, kX87UE = 2, kX87OE = 4, kX87ZE = 8, kX87IE = 16 };

X87Round x87RoundOf(std::uint16_t control);

// ---- 内存 ↔ 寄存器（宽度 4 / 8 / 10）----
// ★ 10 字节是 x87 的扩展精度内存格式：寄存器是 double ⇒ 写出去要**加宽**（低 11 位尾数补 0），
//   读进来要**收窄**（就近偶数）。两个方向都精确（加宽无损；收窄就是 double 的舍入）。
X87Val x87LoadMem(const std::uint8_t* mem, unsigned size);
void x87StoreMem(X87Val v, std::uint8_t* mem, unsigned size);

// ---- 整数 ↔ double ----
X87Val x87FromI16(std::int16_t v);
X87Val x87FromI32(std::int32_t v);
X87Val x87FromI64(std::int64_t v);
// 整数转换：按 `rc` 舍入；越界 / NaN ⇒ 置 IE 并给"整数不定值"（i32 = 0x80000000、i64 = 0x8000…）
std::int32_t x87ToI32(X87Val v, X87Round rc);
std::int64_t x87ToI64(X87Val v, X87Round rc);
std::int32_t x87ToI32Trunc(X87Val v);  // `fisttp`：恒朝零（不看 RC）
std::int64_t x87ToI64Trunc(X87Val v);

// ---- 常量（x86 的 fld 常数族；D49 之后就是 double 的最近值）----
X87Val x87One();
X87Val x87Zero();
X87Val x87Pi();
X87Val x87Ln2();
X87Val x87Lg2();
X87Val x87L2e();
X87Val x87L2t();

// ---- 四则 / 开方 / 取整 / 取余 / 缩放 ----
X87Val x87Add(X87Val a, X87Val b);
X87Val x87Sub(X87Val a, X87Val b);
X87Val x87Mul(X87Val a, X87Val b);
X87Val x87Div(X87Val a, X87Val b);
X87Val x87Sqrt(X87Val a);
X87Val x87RoundToInt(X87Val v, X87Round rc);  // `frndint`：按 RC 取整（仍是浮点）
X87Val x87Rem(X87Val a, X87Val b);            // `fprem1`：IEEE 余数（商取最近偶数）
X87Val x87RemTrunc(X87Val a, X87Val b);       // `fprem`：商朝零截断（fmod 语义）
X87Val x87Scale(X87Val a, X87Val b);          // `fscale`：a · 2^trunc(b)
X87Val x87ChangeSign(X87Val v);               // 只翻符号位（NaN 也翻）
X87Val x87Abs(X87Val v);

// ★ 上溢 / 下溢的**唯一判据**（任务 0 / D55）：四则（`x87Add/Sub/Mul/Div`）与 `x87Scale` 调它。
//   生成代码按**同一份判据**发 IR（`FnEmit::x87RaiseFloatFlags`）—— 两臂逐位可比的前提是
//   "判据只有一份"，⛔ 不要在发射器里另写一套阈值。
//   OE ⇐ r 是 ±inf 且 a、b 都有限；UE ⇐ r 是次正规（`|r| < DBL_MIN && r != 0`）。
//   ⚠ `x87Div` 里 `b == 0` 时**不判 OE**（那是 ZE，不是上溢）。⛔ PE 不在这里（恒 0）。
void x87RaiseOverflowUnderflow(X87Val a, X87Val b, X87Val r);

// ---- 比较 / 分类 ----
// -1 (a<b) / 0 (相等) / 1 (a>b) / 2 (无序：任一操作数是 NaN)
// ★ `quiet = true`（`fucom*`）⇒ 只有 SNaN 置 IE；`quiet = false`（`fcom*`）⇒ 任何 NaN 都置 IE。
int x87Compare(X87Val a, X87Val b, bool quiet);
bool x87IsNan(X87Val v);
bool x87IsSnan(X87Val v);
bool x87IsZero(X87Val v);
bool x87IsInf(X87Val v);
bool x87SignBit(X87Val v);

// `fxam` 的分类（Empty 由调用方按 tag word 先判掉 —— 这一层看不见 tag）
enum class X87Class : std::uint8_t { Nan = 0, Infinity, Zero, Denormal, Normal };
X87Class x87Classify(X87Val v);

// `fxtract`：significand = [1,2) 的尾数、exponent = 带符号整数（都是 double）
void x87Extract(X87Val a, X87Val* significand, X87Val* exponent);

// ---- 超越函数（SDM 明说 implementation defined ⇒ 宿主 libm；D49 §3 允许）----
// ★ 这几个就是生成代码 `lift_x87_*` helper 的**同一份**实现（⛔ 不许在发射器里再写一遍公式）。
X87Val x87Sin(X87Val x);
X87Val x87Cos(X87Val x);
X87Val x87Tan(X87Val x);
X87Val x87Atan2(X87Val y, X87Val x);  // `fpatan`：atan2(ST1, ST0)
X87Val x87Log2(X87Val x);             // `fyl2x` 的 log2 部分
X87Val x87Log2p1(X87Val x);           // `fyl2xp1` 的 log2(1+x) 部分
X87Val x87Exp2m1(X87Val x);           // `f2xm1` 的 2^x − 1
bool x87TrigInRange(X87Val x);        // |x| < 2^63（`fsin`/`fcos` 的 C2 判据）

// ---- 浮点异常位（"挂账"通道：运算置位、`syncStatus` 取走并清空）----
// ★ **每线程一份**（`thread_local`）：音频 feeder 是第二条 guest 执行流，共享状态会互相偷 / 污染
//   x87 状态字的异常位（`REQ-01M76ZHN9V2WNCYT1ACJDJ94CW` 的守卫 `fpu-thread-flags`）。
std::uint8_t x87PeekExceptionFlags();
void x87ClearExceptionFlags();
void x87RaiseInvalid();
std::uint8_t x87TakeExceptionFlags();  // 取走并清空（对应 x87 的"读状态字"）

// 控制字 → RC（`x87ApplyControlWord` 的替代：PC 字段**无效**，只取 RC）
// ⚠ 保留这个名字的调用点语义：解释器在 `fldcw` 之后调它一次，生成代码则直接从控制字取 RC。

}  // namespace lift
