// emulator-lift · SHA-256 + 十六进制 / 大小写工具
// ★ 只为一件事：证明"执行的字节"与"用来看的语料"是同一份（plan §4 D1 / A3 ⓪）。
//   自带实现而不是引第三方加密库：这是**启动前置条件**，不该把"能不能启动"绑到另一个包上。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lift {

std::array<std::uint8_t, 32> sha256(const void* data, std::size_t size);
std::array<std::uint8_t, 32> sha256(const std::vector<std::uint8_t>& data);
std::string toHex(const std::uint8_t* bytes, std::size_t n);
std::string toHex(const std::array<std::uint8_t, 32>& d);
bool equalsIgnoreCase(const std::string& a, const std::string& b);
std::string toLower(std::string s);
std::string toUpper(std::string s);

}  // namespace lift
