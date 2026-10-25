// 缩略图容器（`.STH` 里的 BMP）。★ 只做容器与像素序，不碰文件系统（`SaveHost` 的活）。
//
// 写侧逐字契约：`BM` / 声明长度 = 像素字节 + 40 / 像素起始 54 / 40 字节信息头（24 位、无压缩）/
// 行自下而上、每像素 BGR、行末填充字节也写出（本实现写 0）。
// 读侧：只按开头 `BM` 判格式；位深 ∈ {1,4,8,24,32}（16 拒）；1/4/8 读调色板（计数 0 ⇒ 2/16/256）；
// 像素字节数按几何**算出**（不读 `biSizeImage`、不比较声明长度）；行指针反向 ⇒ 翻正；按通道取 BGR。
// ★ 短读属规范明说的 UB（残差授权"显式报错、不复刻宿主内存布局"）⇒ 这里一律返回 false。
#pragma once

#include "core/bytes.h"
#include "core/save_host.h"

#include <cstdint>

namespace core {

// 内存里的解码结果：**top-down RGBA8**。
struct ThumbImage {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    Bytes rgba;
};

// bottom-up BGR 表面 → BMP 字节。
Bytes encode_thumb_bmp(const SurfacePixels& surface);

// BMP 字节 → top-down RGBA；**不抛**（结构不够/位深不支持/调色板缺失都返回 false）。
bool decode_thumb_bmp(ByteSpan bytes, ThumbImage& image);

}  // namespace core
