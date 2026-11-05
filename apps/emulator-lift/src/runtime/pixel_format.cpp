// emulator-lift · 像素格式的非常量部分（约定见 runtime/pixel_format.h）
#include "runtime/pixel_format.h"

namespace lift {
namespace pixel_format {

SurfaceFormat formatForBpp(unsigned bpp) {
    SurfaceFormat f;
    if (bpp == 16) {
        f.bpp = 16;
        f.rmask = 0xF800u;  // 565（plan D4）
        f.gmask = 0x07E0u;
        f.bmask = 0x001Fu;
        f.amask = 0u;
    } else {
        f.bpp = 32;
        f.rmask = 0x00FF0000u;
        f.gmask = 0x0000FF00u;
        f.bmask = 0x000000FFu;
        f.amask = 0xFF000000u;
    }
    return f;
}

}  // namespace pixel_format
}  // namespace lift
