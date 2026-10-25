// Machine 各实现文件共用的小工具（**不是**引擎语义，只是 C++ 侧的位/文本转换）。
//
// ★ 为什么单独一个头：`vm.cpp` 原先在一个匿名命名空间里定义它们，拆分后
//   `vm_value.cpp` / `vm_ops_*.cpp` 都要用 ⇒ 改成 `inline`（每处一份，链接器合并），
//   ⛔ 不要把这些东西塞进 `vm.h`（那是 Machine 的类型与状态，不该被无关 TU 拉进来）。
#pragma once

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace core {

inline std::uint32_t rol32(std::uint32_t value, int amount) {
    return std::rotl(value, amount);
}

inline std::uint32_t ror32(std::uint32_t value, int amount) {
    return std::rotr(value, amount);
}

inline std::int32_t as_i32(std::uint32_t value) {
    return static_cast<std::int32_t>(value);
}

inline std::uint32_t as_u32(std::int32_t value) {
    return static_cast<std::uint32_t>(value);
}

// 文本 → 整数：十进制；解析不出一个字符就按 0（与引擎的容忍口径一致，见 `CORE-OPS-*` 的读文本族）。
inline std::int32_t parse_int_text(const std::string& text) {
    const char* begin = text.c_str();
    char* end = nullptr;
    const long value = std::strtol(begin, &end, 10);
    return end == begin ? 0 : static_cast<std::int32_t>(value);
}

inline double parse_float_text(const std::string& text) {
    const char* begin = text.c_str();
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    return end == begin ? 0.0 : value;
}

}  // namespace core
