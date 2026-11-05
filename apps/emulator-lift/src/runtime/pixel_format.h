// emulator-lift · **像素格式的唯一实现**（掩码换算 + load/store + D3DCOLOR 打包）
//
// ★ 为什么单列一份：font / d3d9 / d3d9_pipeline / d3dx9_sprite / ddraw_surface 各自抄过一套
//   `maskShift/maskBits/expandChannel/packChannel`，五份的舍入与 mask==0 语义还**不一致**
//   （其中两份在 5/6 位通道上会差 1）。这里钉死成一份，其它文件只许调用或做薄转发。
//
// ★ 语义（与既有 d3d9 / 流水线路径一致，既有 565 手算断言不变）：
//   · `expandChannel`：按掩码取位，5/6 位用**四舍五入**展开到 8 位；mask==0 ⇒ 255（"无该通道"
//     在 alpha 上等价于不透明，调用方若要区分请自己判 mask）。
//   · `packChannel`：8 位**截断高位**到目标位宽（`v >> (8-bits)`）；mask==0 ⇒ 0。
//     ⛔ 不要改成按 max 缩放：16bpp 的既有期望值（如品红 0xF800F8 ⇒ 0xF81F）会变。
//   · `loadPixel/storePixel`：只认 16 / 32bpp（与 DDraw / D3D 的表面格式一致）。
//
// ★ 热路径：掩码函数都是 `constexpr` + `inline`（无分支循环在编译期可折叠）；调用点不会被这层拖慢。
#pragma once

#include <cstdint>
#include <cstring>

#include "runtime/surface.h"

namespace lift {
namespace pixel_format {

constexpr unsigned maskShift(std::uint32_t mask) {
    unsigned shift = 0;
    while (mask != 0 && (mask & 1u) == 0) {
        mask >>= 1;
        ++shift;
    }
    return shift;
}

// ⚠ 数**所有**置位（掩码不一定是 LSB 对齐的连续段：0x00FF0000 也要数出 8）
constexpr unsigned maskBits(std::uint32_t mask) {
    unsigned bits = 0;
    while (mask != 0) {
        bits += mask & 1u;
        mask >>= 1;
    }
    return bits;
}

constexpr unsigned expandChannel(std::uint32_t raw, std::uint32_t mask) {
    if (mask == 0) return 255u;
    const unsigned bits = maskBits(mask);
    if (bits == 0 || bits > 8) return 255u;
    const std::uint32_t v = (raw & mask) >> maskShift(mask);
    if (bits == 8) return v;
    const unsigned maxv = (1u << bits) - 1u;
    return (v * 255u + maxv / 2u) / maxv;
}

constexpr std::uint32_t packChannel(unsigned v8, std::uint32_t mask) {
    if (mask == 0) return 0;
    const unsigned bits = maskBits(mask);
    if (bits == 0 || bits > 8) return 0;
    unsigned v = v8 > 255u ? 255u : v8;
    if (bits < 8) v >>= (8 - bits);
    return (v << maskShift(mask)) & mask;
}

// ---- GDI 文本路径的 16bpp 兼容口径（旧 font.cpp 的逐位语义；⛔ 不是 D3D 的舍入口径）----
// 旧实现：展开按 `v*255/max` **向下取整**，打包按 `(v*max+127)/255` **四舍五入**。
// 保留它是为了不让 16bpp 文本像素因为"统一像素工具"而改 1 LSB；32bpp 下两者等价。
constexpr unsigned expandChannelGdi(std::uint32_t raw, std::uint32_t mask) {
    if (mask == 0) return 0;
    const unsigned bits = maskBits(mask);
    if (bits == 0) return 0;
    const std::uint64_t maxv = (bits >= 32) ? 0xFFFFFFFFull : ((1ull << bits) - 1ull);
    const std::uint64_t v = (static_cast<std::uint64_t>(raw & mask)) >> maskShift(mask);
    return static_cast<unsigned>(v * 255ull / maxv);
}

constexpr std::uint32_t packChannelGdi(unsigned v8, std::uint32_t mask) {
    if (mask == 0) return 0;
    const unsigned bits = maskBits(mask);
    if (bits == 0) return 0;
    unsigned v = v8 > 255u ? 255u : v8;
    const std::uint64_t maxv = (bits >= 32) ? 0xFFFFFFFFull : ((1ull << bits) - 1ull);
    const std::uint64_t scaled = (static_cast<std::uint64_t>(v) * maxv + 127ull) / 255ull;
    return (static_cast<std::uint32_t>(scaled) << maskShift(mask)) & mask;
}

inline std::uint32_t packColor(const SurfaceFormat& fmt, unsigned r, unsigned g, unsigned b, unsigned a) {
    return packChannel(r, fmt.rmask) | packChannel(g, fmt.gmask) | packChannel(b, fmt.bmask) |
           packChannel(a, fmt.amask);
}

// D3DCOLOR（0xAARRGGBB）→ 目标格式的像素位（按掩码，⛔ 不按 bpp 猜 565/8888）
inline std::uint32_t pixelFromColor(const SurfaceFormat& fmt, std::uint32_t argb) {
    return packColor(fmt, (argb >> 16) & 0xFFu, (argb >> 8) & 0xFFu, argb & 0xFFu, (argb >> 24) & 0xFFu);
}

// 像素读写（宿主侧直接改 guest 像素；bpp 只支持 16 / 32）
inline std::uint32_t loadPixel(const std::uint8_t* p, unsigned bpp) {
    if (bpp == 32) {
        std::uint32_t v = 0;
        std::memcpy(&v, p, 4);
        return v;
    }
    std::uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
}

inline void storePixel(std::uint8_t* p, unsigned bpp, std::uint32_t v) {
    if (bpp == 32) {
        std::memcpy(p, &v, 4);
        return;
    }
    const std::uint16_t v16 = static_cast<std::uint16_t>(v & 0xFFFFu);
    std::memcpy(p, &v16, 2);
}

// 给定 bpp 的标准格式（32 = BGRA8888；16 = 565）
SurfaceFormat formatForBpp(unsigned bpp);

}  // namespace pixel_format
}  // namespace lift
