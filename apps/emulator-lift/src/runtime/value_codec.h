// emulator-lift · 脚本 **int 值编解码**的唯一 C++ 实现（与 TS 的 `value-codec.mts` 同一公式）
//
// ★ spec：`SPC-VALUE-INT-ENCODED` @ `data/spec/chapters/40-pools.md:293-322`
//     DEC(x) = ror32(rol32(x, 11) ^ key, 25)
//     ENC(v) = rol32(ror32(v, 7) ^ key, 21)        ← DEC 的逆（两段旋转 7 + 25 = 32）
//   key 是**每个引擎实例一份、运行期生成**的（`Engine+0x5EC8C`；唯一写点 EA `0x417359`）⇒
//   这两个函数只吃调用方从 guest 读到的 key，⛔ 没有"默认 key = 0"这条路径。
//
// ★ 为什么单独一份头：`native_overrides.cpp` 的 `sortKeyOf()`（插入排序段的键 `K(k)`）逐字就是 DEC；
//   控制层 / QJS 侧的 script global 读写也要 DEC/ENC。⇒ **两处共用这一份**（`sortKeyOf` 已改为转发
//   `decodeInt`），⛔ 不再各写一份公式（那会分叉语义、且只有一处会被变异测到）。
//
// ★ 纯 `constexpr` / 零 include 依赖（只 `<cstdint>`）⇒ pure CTest 可直接驱动。
#pragma once

#include <cstdint>

namespace lift {

// 32 位循环移位（`n` 只取 7 / 11 / 21 / 25 这几个非零值；⛔ 不传 0 —— 那在 C++ 里是 UB）。
constexpr std::uint32_t rol32(std::uint32_t v, unsigned n) {
    return (v << n) | (v >> (32u - n));
}
constexpr std::uint32_t ror32(std::uint32_t v, unsigned n) {
    return (v >> n) | (v << (32u - n));
}

// 槽里的编码位模式 → 语义值（32 位逐位；符号解释由调用方按 kind 做）。
constexpr std::uint32_t decodeInt(std::uint32_t raw, std::uint32_t key) {
    return ror32(rol32(raw, 11u) ^ key, 25u);
}

// 语义值 → 槽里的编码位模式（DEC 的逆；两段都是 32 位，⛔ 中间结果不许留 64 位）。
constexpr std::uint32_t encodeInt(std::uint32_t value, std::uint32_t key) {
    return rol32(ror32(value, 7u) ^ key, 21u);
}

}  // namespace lift
