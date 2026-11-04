// emulator-lift · CP932 ↔ UTF-16（自带码表；plan §7.1 的"自带 CP932 表"方案）
//
// ★ 为什么不用宿主的 MultiByteToWideChar：那会让 Windows / macOS 各有一套表 ⇒
//   "同一份存档名在两台机器上编出不同字节"变成可能（plan §7.1 末段）。
// ★ 为什么不用 libiconv：LGPL 静态链接要处理再链接义务，且要加一个 vcpkg port；
//   而 CP932 是**固定的编码标准**，钉成入库的纯文本表就能三平台逐字节一致。
// ★ 表是派生文件 `cp932_table.h`（`node tools/lift-cp932-gen.mjs --write` 生成）；
//   双字节与 WHATWG shift_jis 逐对一致（9604 个已定义 pair），单字节按 Windows CP932 的
//   8 个差异修正 —— 与 `tools/lib/cp932.mjs` 同一份口径。
// ★ 不可解码 / 不可编码**不会被静默吞掉**：返回 0，调用方决定（shim 里按 Windows 语义记
//   `ERROR_NO_UNICODE_TRANSLATION` / 写 `?` 并计数，⛔ 不假装成功）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace lift {

// 解一个 CP932 字符；返回消耗的字节数（1/2），0 = 非法序列。
int cp932Decode(const std::uint8_t* p, std::size_t n, char16_t* out);
// 编一个 UTF-16 码元；返回写出的字节数（1/2），0 = 不可编码。
int cp932Encode(char16_t w, std::uint8_t out[2]);

// 整串转换：非法序列按 Windows 的做法处理（解码：跳过 1 字节并计 bad；编码：写 '?' 并计 bad）。
std::u16string cp932ToWide(std::string_view bytes, std::size_t* bad = nullptr);
std::string wideToCp932(std::u16string_view text, std::size_t* bad = nullptr);

// UTF-8 ↔ UTF-16（不是 CP932 的一部分，但同属"码页与字符串"的叶子设施；
// 路径 / W 版 API 的入参都要经过它）。非法字节 / 未配对代理 ⇒ '?' 并计 bad。
std::u16string utf8ToUtf16(std::string_view s, std::size_t* bad = nullptr);
std::string utf16ToUtf8(std::u16string_view w, std::size_t* bad = nullptr);

}  // namespace lift
