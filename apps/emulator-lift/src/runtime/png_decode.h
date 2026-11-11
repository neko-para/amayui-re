// emulator-lift · PNG 解码（libpng **简化 API**；输入是宿主内存里的文件字节）
//
// ★ 为什么单开一个模块：`shims/d3dx9.cpp` 的 `D3DXCreateTextureFromFileInMemory` 原先只从 IHDR
//   取宽高（像素不解码 ⇒ 建出来是全 0 的 arena）。解码这一步与 guest / COM / D3D9 都无关：
//   它只吃"一段宿主内存里的文件字节"、只吐"一份 RGBA"，所以单独成文件（`decodeRgba8` 也能被
//   直接喂字节测，不必经 guest 内存 / 纹理 —— 见 `tests/d3dx9.cpp` 的 `d3dx9/png-decode`）。
// ★ 输入契约：`bytes` 是**宿主**指针，必须在本函数返回前一直有效（libpng 简化 API **不拷**输入）。
//   调用方**不需要**先自己拷一份：`shims/d3dx9.cpp` 直接传 guest 的平坦映射
//   （`GuestMemory::host(src_ea)`，先做整段 `committed` 检查）—— 理由是解码同步、期间不跑 guest 代码，
//   且 `base_` 进程内不变。⛔ 本模块不认识 guest EA / `GuestMemory` / 纹理。
// ★ 失败面：不是 PNG / 截断 / CRC 坏 / 尺寸 0 / 超上限 / 缓冲分配失败 ⇒ `false` + `why`。
//   ⛔ 不抛、⛔ 不 trap、⛔ 不建任何纹理、⛔ 不写 guest 内存。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lift {
namespace png {

// 解码结果（与 libpng 无关的纯数据）：**8-bit RGBA、行序 top-down**、像素内字节序 R,G,B,A。
// ★ 这是 PNG 的原生通道序，**不是** D3D 的 A8R8G8B8（guest 内存里是 B,G,R,A）——
//   换序在写纹理的那一步做（`shims/d3dx9.cpp` 的 `uploadRgbaToTexture`），⛔ 不要在这里偷偷换成 BGRA：
//   这个模块的合同就是"PNG 的 RGBA"。
struct RgbaImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;  // size == width*height*4
};

// 从文件字节解码。
// `max_side` > 0 ⇒ 任一边超过它就算失败（纹理上限在 d3d9 侧，这里只当**不可信输入**的闸门）。
// 成功：`*out` = 解码结果（rgba 非空、宽高非 0）；失败：`*out` 复位成空、`*why` 给一句可读原因。
bool decodeRgba8(const std::uint8_t* bytes, std::size_t size, std::uint32_t max_side, RgbaImage* out,
                 std::string* why);

}  // namespace png
}  // namespace lift
