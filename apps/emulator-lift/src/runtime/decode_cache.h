// emulator-lift · **EA → Insn 的解码缓存**（自带原始字节校验；AOT 臂 `lift_one` 的唯一加速层）
//
// ★ 为什么需要它（读数，不是猜测）：`--profile` 的四桶归因实测 `lift_one` 的
//   **B 桶（`Insn`/`std::string` 构造 + `Decoder::decode`）占 79.3%**（878.6 ns / 1107.4 ns，
//   46047 个样本），而 A 换入 34.7 / C 执行 160.6 / D 回读 34.5 —— 地板**不在 x87 语义里，
//   在解码里**。块会被重复进入几百万次，同一 EA 的解码结果只取决于那段字节 ⇒ 可以把结果留下。
//
// ★ 为什么**必须**自带字节校验（AOT 臂的已知 SMC 缺口）：`lift_note_write` 在 AOT 里是 no-op
//   （`lift_abi.cpp`：`cpu.cache == nullptr` ⇒ `noteGuestWrite(nullptr, …)` 不做事），所以
//   "guest 写代码页"今天**没有任何钩子**。⇒ 缓存不能假设"代码不自我修改"：
//   每个条目存**该指令的原始字节 + 长度**，命中时与 `cpu.mem` 现状**逐字节**比较；
//   不一致就重新解码并覆盖。于是：
//     · 缓存**不可能**给出陈旧答案（字节一变就重解）；
//     · 顺手把 SMC 缺口在**解码层面**变成自愈的（但它仍不解决"已发射的 lifted 块"那层 SMC
//       —— 本缓存只保证"要执行的那条指令的 Insn 是当前的字节解出来的"，⛔ 不冒充块缓存）。
//   ⛔ 不要为了省一次比较而删掉逐字节校验：那正是"静默跑旧代码"的形状。
//
// ★ 为什么只缓存"**15 字节窗口全可读**"的解码结果：`Decoder::decode` 的可读字节数会随
//   `committed()` 变化（跨页指令只读到当前页末尾），而 `9B`（WAIT）作为前缀的**合并**
//   （`decode.cpp` 的 `fstsw` 一族）会因此给出**不同长度 / 不同 op** 的结果。⇒ 只有
//   "窗口完整"的那一类解码是**只由这 15 个字节决定**的，才允许留成条目；其余走
//   `uncached` 直通（语义与不装缓存逐字相同，只是不加速）。
//
// ★ 线程：缓存**不是**线程安全的（普通数组 + 计数器）。`lift_one` 的调用者只有一个线程
//   （AOT 主线程；音频 feeder 走的是解释器块缓存，⛔ 不经过 `lift_one`）⇒ `lift_abi.cpp` 里
//   用的是 `thread_local`，即使将来多线程也不会共享同一个实例。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "runtime/decode.h"
#include "runtime/insn.h"
#include "runtime/memory.h"

namespace lift {

// 槽数必须是 2 的幂。512 槽 × 152 B ≈ 78 KiB/线程（实测命中率见 `--profile` 的 `decode-cache:`）。
inline constexpr std::size_t kDecodeCacheSlots = 512;
// x86 单条指令的编码长度上限（Intel SDM：15 字节）。
inline constexpr std::uint8_t kDecodeCacheMaxLen = 15;

// ★ POD（**没有**默认成员初始化器）：`len == 0` 即"空槽"⇒ 整张表是零初始化的
//   `thread_local` 静态存储，**不需要**动态初始化 / 初始化守卫。
struct DecodeCacheEntry {
    std::uint32_t ea;                        // 这条指令的 EA（`len == 0` ⇒ 空槽）
    std::uint8_t len;                        // 编码长度（= `Insn::length`；0 ⇒ 空槽）
    std::uint8_t bytes[kDecodeCacheMaxLen];  // 该指令的原始字节（命中时逐字节比对）
    Insn insn;                               // 解出来的结果（`ea` / `length` 与上面一致）
};

struct DecodeCache {
    DecodeCacheEntry slots[kDecodeCacheSlots];
    // 计数（只作读数；⛔ 不进任何判据）
    std::uint64_t hits = 0;         // 命中（EA 相同 + 字节逐字节相同）
    std::uint64_t misses = 0;       // 空槽 / EA 不同
    std::uint64_t invalidated = 0;  // EA 相同但字节变了 ⇒ 重新解码并覆盖（**SMC 自愈**的证据）
    std::uint64_t uncached = 0;     // 解出来了但没有缓存条件（15 字节窗口不完整）
};

// 取 `ea` 的 `Insn`：语义**逐字等于** `dec.decode(mem, ea, out, why)`。
//   返回值 / `*out` / `*why` 与直接调 `Decoder::decode` 完全一致；差别只有"要不要真去解码"。
// ★ 缓存命中只可能返回"同一份字节在同一个 EA 上解出来的**同一个** `Insn`"：命中前逐字节校验，
//   不一致就重解并覆盖（⛔ 不存在"命中但返回旧结果"的路径）。
bool decodeCached(DecodeCache& cache, const Decoder& dec, const GuestMemory& mem, std::uint32_t ea,
                  Insn* out, std::string* why);

// `lift_one` 用的那一份（`lift_abi.cpp` 里的 `thread_local`）—— **只给读数**（`--profile` 的
// `decode-cache:` 行 / 诊断）。⛔ 不要拿它绕过 `decodeCached` 直接读 Insn：那会跳过字节校验。
const DecodeCache& liftOneDecodeCache();

}  // namespace lift
