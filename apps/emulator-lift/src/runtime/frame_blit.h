// emulator-lift · guest 后缓冲格式 → 宿主纹理格式的选择（纯函数，无 SDL）
//
// ★ 判据来自 **masks**，不是 bpp：D3D9 的 X8R8G8B8 与 A8R8G8B8 都是 32bpp，但 amask 不同；
//   DDraw 的 32bpp 默认带 0xFF000000（`formatForBpp`），所以也不能按 bpp 猜。
// ★ 只映射 arena 真正会建的六种标准格式（D3D9 的 21/22/23..26）；其它 ⇒ Unsupported
//   （presenter 会回退 headless，⛔ 不猜颜色）。
#pragma once

#include <cstdint>

#include "runtime/surface.h"

namespace lift {
namespace frame_blit {

enum class HostPixelFormat : std::uint8_t {
    Unsupported = 0,
    Xrgb8888,  // 32bpp, amask == 0（D3D9 X8R8G8B8；alpha 无意义）
    Argb8888,  // 32bpp, amask == 0xFF000000（D3D9 A8R8G8B8 / DDraw 默认）
    Rgb565,    // 16bpp 565
    Xrgb1555,  // 16bpp 555, amask == 0
    Argb1555,  // 16bpp 555, amask == 0x8000
    Argb4444,  // 16bpp 4444
};

HostPixelFormat chooseHostFormat(const SurfaceFormat& format);
const char* hostPixelFormatName(HostPixelFormat format);

}  // namespace frame_blit
}  // namespace lift
