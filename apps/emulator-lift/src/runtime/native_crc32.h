// emulator-lift · native override `sub_436D00` / `sub_436D50` 的纯核（CRC-32 的两个变体）
//
// ★ 契约：**表由调用方给** —— 本文件里没有多项式常量（`0x4C11DB7` / `0xEDB88320` 一个都不许写）。
//   那两张表是 **guest 的 `[this, this+0x800)`**（`+0x000` 非反射 / `+0x400` 反射，由 `sub_436BD0` /
//   `sub_436C60` 建）：⛔ 不许在 native 里重建 / 硬编码 —— 表一旦被别的路径重建过，硬编码就会
//   **静默分叉**，而 CRC 分叉的表现是"存档写出去、读回来对不上"，全程没有报错。
//   ⇒ 纯核只做"查表 + 移位"，"表从哪来"是调用方的事（`nativeCrc436D00/436D50` 从 guest 读）。
// ★ 逐字依据（`.lst` 的两段循环体；EA 是锚，⛔ 不抄行号）：
//   0x436D31..0x436D45（`sub_436D00`，反射，每字节 8 条）：
//     `movzx edi, byte ptr [ecx]` / `movzx ebx, al` / `xor edi, ebx` / `shr eax, 8` /
//     `xor eax, [esi+edi*4+400h]` / `inc ecx` / `dec edx` / `jns`
//   0x436D81..0x436D93（`sub_436D50`，非反射，每字节 9 条）：
//     `movzx ebx, byte ptr [ecx]` / `mov edi, eax` / `shr edi, 18h` / `xor edi, ebx` /
//     `shl eax, 8` / `xor eax, [esi+edi*4]` / `inc ecx` / `dec edx` / `jns`
//   ⇒ `reflected == true` ⇔ 查 **`+0x400`** 那张表 + `crc >> 8` + 索引用**低**字节；
//     `reflected == false` ⇔ 查 **`+0`** 那张表 + `crc << 8` + 索引用**高**字节（`crc >> 24`）。
//   ★ 两个变体只差这三处（查哪张表 / 索引高低字节 / 移位方向），别再加参数化维度。
// ★ 初值 / 收尾：入口 `or eax, -1`（`0x436D26` / `0x436D76`）= `0xFFFFFFFF`，收尾 `not eax`
//   （`0x436D49` / `0x436D97`）⇒ 返回 `~crc`。`n == 0` 时返回 `~0xFFFFFFFF == 0`（与 guest 同值）。
// ★ 只读输入、零写、无分支依赖（没有 host 端状态）⇒ 纯函数，测试可直接喂合成表。
#pragma once

#include <cstdint>

namespace lift {

// 对 `buf[0..n)` 跑一遍查表 CRC（`n == 0` ⇒ 返回 0；⛔ 不读 `buf`）。
// `table` = 256 个 dword 的**宿主视图**；`reflected` 见文件头的三处差异。
std::uint32_t crc32Bytes(const std::uint32_t* table, const std::uint8_t* buf, std::uint32_t n, bool reflected);

}  // namespace lift
