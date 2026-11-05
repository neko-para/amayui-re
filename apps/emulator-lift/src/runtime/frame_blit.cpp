#include "runtime/frame_blit.h"

namespace lift {
namespace frame_blit {

HostPixelFormat chooseHostFormat(const SurfaceFormat& f) {
    if (f.bpp == 32) {
        if (f.rmask == 0x00FF0000u && f.gmask == 0x0000FF00u && f.bmask == 0x000000FFu) {
            if (f.amask == 0x00000000u) return HostPixelFormat::Xrgb8888;
            if (f.amask == 0xFF000000u) return HostPixelFormat::Argb8888;
        }
        return HostPixelFormat::Unsupported;
    }
    if (f.bpp == 16) {
        if (f.rmask == 0xF800u && f.gmask == 0x07E0u && f.bmask == 0x001Fu && f.amask == 0u) {
            return HostPixelFormat::Rgb565;
        }
        if (f.rmask == 0x7C00u && f.gmask == 0x03E0u && f.bmask == 0x001Fu) {
            if (f.amask == 0x0000u) return HostPixelFormat::Xrgb1555;
            if (f.amask == 0x8000u) return HostPixelFormat::Argb1555;
        }
        if (f.rmask == 0x0F00u && f.gmask == 0x00F0u && f.bmask == 0x000Fu && f.amask == 0xF000u) {
            return HostPixelFormat::Argb4444;
        }
        return HostPixelFormat::Unsupported;
    }
    return HostPixelFormat::Unsupported;
}

const char* hostPixelFormatName(HostPixelFormat format) {
    switch (format) {
        case HostPixelFormat::Unsupported: return "unsupported";
        case HostPixelFormat::Xrgb8888: return "XRGB8888";
        case HostPixelFormat::Argb8888: return "ARGB8888";
        case HostPixelFormat::Rgb565: return "RGB565";
        case HostPixelFormat::Xrgb1555: return "XRGB1555";
        case HostPixelFormat::Argb1555: return "ARGB1555";
        case HostPixelFormat::Argb4444: return "ARGB4444";
    }
    return "?";
}

}  // namespace frame_blit
}  // namespace lift
