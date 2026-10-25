#pragma once

#include <filesystem>
#include <string>

namespace core {

// 引擎里的字符串（ALF 名字 / 配置值 / 路径）是 **CP932（ANSI）字节串**，
// 不是 UTF-8。核心内部保持原始字节；只在**显示**和**变成宿主路径**时解码。
// ★ 这是显式口径：⛔ 不要把 CP932 字节直接塞进 `printf` 或 `std::filesystem::path`。
std::string cp932_to_utf8(const std::string& bytes);

// UTF-8 → 宿主路径；Windows 上走 UTF-16，其它平台按 UTF-8。
std::filesystem::path path_from_utf8(const std::string& utf8);

// 引擎给的路径字节串（CP932）→ 宿主路径。
std::filesystem::path path_from_engine(const std::string& cp932);

// 宿主路径 → UTF-8（用于日志 / 错误信息）。
std::string path_to_utf8(const std::filesystem::path& path);

}  // namespace core
