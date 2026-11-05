// emulator-lift · 像素格式 / 采样过滤的 **pure** 夹具（CTest: pixel-format；`ctest -R pixel`）
//
// 判据（任务 B「提取 pixel_format」）：
//   ① 掩码换算：pack→expand 往返回到满量程；5/6 位与 8 位的舍入规则固定；
//   ② load/store：16bpp 只动 2 字节、32bpp 只动 4 字节（⛔ 不越界写）；
//   ③ pixelFromColor：按掩码打包（A8R8G8B8 / X8R8G8B8 / R5G6B5 三种）；既有 565 期望值不变；
//   ④ D3DCOLOR 解包只有一份（`unpackColor`）：位序错一位这里红；
//   ⑤ sampler 的 MIN/MAG **都参与**决策：`(POINT, LINEAR)` 必须走线性 —— 旧实现只看 MAG，
//      这条会红。
#include <cstdint>
#include <cstring>

#include "check.h"
#include "runtime/d3d9_pipeline.h"
#include "runtime/pixel_format.h"

namespace {

using lift::pixel_format::expandChannel;
using lift::pixel_format::expandChannelGdi;
using lift::pixel_format::formatForBpp;
using lift::pixel_format::loadPixel;
using lift::pixel_format::maskBits;
using lift::pixel_format::maskShift;
using lift::pixel_format::packChannel;
using lift::pixel_format::packChannelGdi;
using lift::pixel_format::packColor;
using lift::pixel_format::pixelFromColor;
using lift::pixel_format::storePixel;
using lift::pixel_format::unpackColor;

// ---------------------------------------------------------------------------------------
// ① 掩码算术
// ---------------------------------------------------------------------------------------
void caseMaskArithmetic() {
    lift_test::g_current_case = "pixel-format/mask";

    // 565：R 在 bit11（5 位）、G 在 bit5（6 位）、B 在 bit0（5 位）
    LIFT_CHECK_EQ_U32(maskShift(0xF800u), 11u);
    LIFT_CHECK_EQ_U32(maskBits(0xF800u), 5u);
    LIFT_CHECK_EQ_U32(maskShift(0x07E0u), 5u);
    LIFT_CHECK_EQ_U32(maskBits(0x07E0u), 6u);
    LIFT_CHECK_EQ_U32(maskShift(0x001Fu), 0u);
    LIFT_CHECK_EQ_U32(maskBits(0x001Fu), 5u);
    // ⚠ 数**所有**置位：非连续掩码也要数对（0x00FF0000 = 8 位）
    LIFT_CHECK_EQ_U32(maskBits(0x00FF0000u), 8u);

    // 满量程往返：pack(255) 落到掩码顶，再 expand 回 255
    LIFT_CHECK_EQ_U32(packChannel(255u, 0xF800u), 0xF800u);
    LIFT_CHECK_EQ_U32(expandChannel(0xF800u, 0xF800u), 255u);
    LIFT_CHECK_EQ_U32(packChannel(255u, 0x07E0u), 0x07E0u);
    LIFT_CHECK_EQ_U32(expandChannel(0x07E0u, 0x07E0u), 255u);
    LIFT_CHECK_EQ_U32(packChannel(255u, 0x1Fu), 0x1Fu);
    LIFT_CHECK_EQ_U32(expandChannel(0x1Fu, 0x1Fu), 255u);

    // 5 位中间值：pack(128) = 128>>3 = 16；expand(16) = (16*255 + 15)/31 = 132
    LIFT_CHECK_EQ_U32(packChannel(128u, 0x1Fu), 16u);
    LIFT_CHECK_EQ_U32(expandChannel(16u, 0x1Fu), 132u);
    // 6 位中间值：pack(128) = 128>>2 = 32；expand(32) = (32*255 + 31)/63 = 130
    LIFT_CHECK_EQ_U32(packChannel(128u, 0x07E0u), 32u << 5u);
    LIFT_CHECK_EQ_U32(expandChannel(32u << 5u, 0x07E0u), 130u);
    // 掩码为 0：pack ⇒ 0；expand ⇒ 255（"无该通道"在 alpha 上等价于不透明）
    LIFT_CHECK_EQ_U32(packChannel(255u, 0u), 0u);
    LIFT_CHECK_EQ_U32(expandChannel(0xFFFFFFFFu, 0u), 255u);

    // ★ GDI 文本路径保留旧 16bpp 口径（⛔ 不被 D3D 的舍入口径悄悄改 1 LSB）：
    //   expandGdi 向下取整、packGdi 按 max 四舍五入；与 D3D 路径在这些点上不同。
    LIFT_CHECK_EQ_U32(expandChannelGdi(3u << 11, 0xF800u), 24u);
    LIFT_CHECK_EQ_U32(expandChannel(3u << 11, 0xF800u), 25u);
    LIFT_CHECK_EQ_U32(packChannelGdi(5u, 0x1Fu), 1u);
    LIFT_CHECK_EQ_U32(packChannel(5u, 0x1Fu), 0u);

    // packColor：A8R8G8B8 四通道逐位对齐
    const lift::SurfaceFormat argb = formatForBpp(32);
    LIFT_CHECK_EQ_U32(packColor(argb, 1u, 2u, 3u, 4u), 0x04010203u);
    // X8R8G8B8（无 alpha）：a 被丢弃 ⇒ 只有 24 位
    lift::SurfaceFormat xrgb = argb;
    xrgb.amask = 0u;
    LIFT_CHECK_EQ_U32(packColor(xrgb, 1u, 2u, 3u, 4u), 0x00010203u);
}

// ---------------------------------------------------------------------------------------
// ② 16/32bpp 的 load / store
// ---------------------------------------------------------------------------------------
void caseLoadStore() {
    lift_test::g_current_case = "pixel-format/load-store";

    std::uint8_t buf16[4] = {0xAA, 0xAA, 0xAA, 0xAA};
    storePixel(buf16, 16, 0xABCD1234u);
    LIFT_CHECK_EQ_U32(buf16[0], 0x34u);  // 小端
    LIFT_CHECK_EQ_U32(buf16[1], 0x12u);
    // ★ 16bpp 只许动 2 字节（第 3/4 字节保持哨兵）
    LIFT_CHECK_EQ_U32(buf16[2], 0xAAu);
    LIFT_CHECK_EQ_U32(buf16[3], 0xAAu);
    LIFT_CHECK_EQ_U32(loadPixel(buf16, 16), 0x1234u);

    std::uint8_t buf32[4] = {0, 0, 0, 0};
    storePixel(buf32, 32, 0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(buf32[0], 0xEFu);
    LIFT_CHECK_EQ_U32(buf32[1], 0xBEu);
    LIFT_CHECK_EQ_U32(buf32[2], 0xADu);
    LIFT_CHECK_EQ_U32(buf32[3], 0xDEu);
    LIFT_CHECK_EQ_U32(loadPixel(buf32, 32), 0xDEADBEEFu);

    // store 后 load 往返（16 位被截到 16 位、32 位完整）
    std::uint8_t round16[2] = {0, 0};
    storePixel(round16, 16, 0xFFFF0000u | 0x8001u);
    LIFT_CHECK_EQ_U32(loadPixel(round16, 16), 0x8001u);
}

// ---------------------------------------------------------------------------------------
// ③ D3DCOLOR → 像素位（按掩码，不是按 bpp 猜）
// ---------------------------------------------------------------------------------------
void casePixelFromColor() {
    lift_test::g_current_case = "pixel-format/pixel-from-color";

    const lift::SurfaceFormat argb = formatForBpp(32);
    LIFT_CHECK_EQ_U32(pixelFromColor(argb, 0xAABBCCDDu), 0xAABBCCDDu);

    lift::SurfaceFormat xrgb = argb;
    xrgb.amask = 0u;  // D3DFMT_X8R8G8B8：顶字节不携带信息
    LIFT_CHECK_EQ_U32(pixelFromColor(xrgb, 0xFF112233u), 0x00112233u);

    const lift::SurfaceFormat rgb565 = formatForBpp(16);
    LIFT_CHECK_EQ_U32(pixelFromColor(rgb565, 0xFFFF0000u), 0xF800u);  // 纯红
    // 既有 d3dx9 手算断言：品红 0x00F800F8 ⇒ R=31 / G=0 / B=31 ⇒ 0xF81F
    // （⛔ 若把 packChannel 改成"按 max 缩放"，这里会变 0xF01E）
    LIFT_CHECK_EQ_U32(pixelFromColor(rgb565, 0x00F800F8u), 0xF81Fu);

    // formatForBpp 的两种标准格式
    LIFT_CHECK_EQ_U32(rgb565.bpp, 16u);
    LIFT_CHECK_EQ_U32(rgb565.rmask, 0xF800u);
    LIFT_CHECK_EQ_U32(rgb565.gmask, 0x07E0u);
    LIFT_CHECK_EQ_U32(rgb565.bmask, 0x001Fu);
    LIFT_CHECK_EQ_U32(rgb565.amask, 0u);
    const lift::SurfaceFormat bgra = formatForBpp(32);
    LIFT_CHECK_EQ_U32(bgra.bpp, 32u);
    LIFT_CHECK_EQ_U32(bgra.rmask, 0x00FF0000u);
    LIFT_CHECK_EQ_U32(bgra.gmask, 0x0000FF00u);
    LIFT_CHECK_EQ_U32(bgra.bmask, 0x000000FFu);
    LIFT_CHECK_EQ_U32(bgra.amask, 0xFF000000u);
    LIFT_CHECK_EQ_U32(bgra.bytesPerPixel(), 4u);
}

// ---------------------------------------------------------------------------------------
// ④ D3DCOLOR 的解包（位序只此一份：pixelFromColor / 顶点色 / 纹理级常量 / Sprite 调制色都走它）
// ---------------------------------------------------------------------------------------
void caseUnpackColor() {
    lift_test::g_current_case = "pixel-format/unpack-color";

    unsigned a = 0, r = 0, g = 0, b = 0;
    unpackColor(0xAABBCCDDu, a, r, g, b);
    LIFT_CHECK_EQ_U32(a, 0xAAu);
    LIFT_CHECK_EQ_U32(r, 0xBBu);
    LIFT_CHECK_EQ_U32(g, 0xCCu);
    LIFT_CHECK_EQ_U32(b, 0xDDu);

    // 与 packColor 的位序一致：A8R8G8B8 上 pixelFromColor 必须逐位等于原 D3DCOLOR，
    // 且 unpack→pack 往返回来（漂了任何一位这里都红）。
    const lift::SurfaceFormat argb = formatForBpp(32);
    LIFT_CHECK_EQ_U32(pixelFromColor(argb, 0xAABBCCDDu), 0xAABBCCDDu);
    LIFT_CHECK_EQ_U32(packColor(argb, r, g, b, a), 0xAABBCCDDu);
}

// ---------------------------------------------------------------------------------------
// ⑤ MIN/MAG 都参与采样决策（旧实现只看 MAG）
// ---------------------------------------------------------------------------------------
void caseSamplerDecision() {
    lift_test::g_current_case = "pixel-format/sampler-min-filter";
    using namespace lift::d3d9;

    LIFT_CHECK(!samplerUsesLinear(kTexfPoint, kTexfPoint));
    LIFT_CHECK(!samplerUsesLinear(kTexfNone, kTexfNone));
    // ★ 关键两行：**只在 MIN 上**要求 LINEAR 也必须生效（旧实现返回 POINT ⇒ 这条红）
    LIFT_CHECK(samplerUsesLinear(kTexfPoint, kTexfLinear));
    // MAG 单独要求 LINEAR 也生效
    LIFT_CHECK(samplerUsesLinear(kTexfLinear, kTexfPoint));
    // 各向异性按 LINEAR 近似（两档都算）
    LIFT_CHECK(samplerUsesLinear(kTexfAnisotropic, kTexfPoint));
    LIFT_CHECK(samplerUsesLinear(kTexfPoint, kTexfAnisotropic));
    // NONE 不要求线性
    LIFT_CHECK(!samplerUsesLinear(kTexfPoint, kTexfNone));
    LIFT_CHECK(!samplerUsesLinear(kTexfNone, kTexfPoint));
}

}  // namespace

int main() {
    using namespace lift_test;
    caseMaskArithmetic();
    caseLoadStore();
    casePixelFromColor();
    caseUnpackColor();
    caseSamplerDecision();
    return finish("pixel-format");
}
