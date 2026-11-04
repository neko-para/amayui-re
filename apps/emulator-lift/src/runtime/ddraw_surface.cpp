// emulator-lift · DirectDraw 的状态与像素层（实现；口径见 ddraw_surface.h）
//
// ★ 所有状态都在 guest 内存里：
//     · 设备状态：固定页 0x7FFFC000（1 页；只用了前 64 字节），首次 DirectDrawCreate 时提交。
//       ⛔ 不与 TEB/PEB/argv（0x7FFD0000..）/ kernel32_string 的 locale 暂存页（0x7FFFD000）重叠。
//     · 每个 surface：guest 堆上的状态块（块 EA 挂在 COM 对象 `obj+8`，见 com.h）。
//   ⇒ 一台 Machine 一份，生命周期跟着对象；⛔ 没有宿主侧静态表。
#include "runtime/ddraw_surface.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "runtime/com.h"
#include "runtime/font.h"
#include "runtime/handles.h"
#include "runtime/heap.h"
#include "runtime/machine.h"
#include "runtime/memory.h"
#include "runtime/present.h"

namespace lift {
namespace ddraw {
namespace {

// 设备状态页（固定 guest 地址；见文件头）
constexpr std::uint32_t kDeviceEa = 0x7FFFC000u;
constexpr std::uint32_t kDevicePageBytes = 0x1000u;
constexpr std::uint32_t kDeviceBytes = 64u;

// 设备状态块布局（**我们自己的**布局；⛔ 不是 Win32 结构体）
enum DevOff : std::uint32_t {
    kDevMagic = 0,    // 'DDV1'
    kDevWidth = 4,
    kDevHeight = 8,
    kDevBpp = 12,
    kDevRMask = 16,
    kDevGMask = 20,
    kDevBMask = 24,
    kDevAMask = 28,
    kDevPrimary = 32,   // primary surface 的**状态块** EA（0 = 没有）
    kDevCoopFlags = 36,
    kDevHwnd = 40,
    kDevSerial = 44,    // 已创建的 surface 序号（DC 句柄 / 诊断）
    kDevSurfaceHead = 48,  // surface 状态块链表的头
    kDevFlips = 52,
};
constexpr std::uint32_t kDevMagicValue = 0x31564444u;  // 'DDV1'

// 每个 surface 的状态块布局
enum SurfOff : std::uint32_t {
    kSfMagic = 0,   // 'DDS1'
    kSfArena = 4,   // SurfaceArena 的像素块 EA
    kSfObject = 8,  // COM 对象 EA
    kSfWidth = 12,
    kSfHeight = 16,
    kSfPitch = 20,
    kSfBpp = 24,
    kSfRMask = 28,
    kSfGMask = 32,
    kSfBMask = 36,
    kSfAMask = 40,
    kSfCaps = 44,
    kSfCkFlags = 48,
    kSfCkLow = 52,
    kSfCkHigh = 56,
    kSfHdc = 60,
    kSfDcCount = 64,
    kSfLockPtr = 68,
    kSfPrimary = 72,  // 1 = primary
    kSfNext = 76,     // 链表 next（状态块 EA）
    // ★ 嵌套锁的 lpSurface 栈（最多 8 层；与 arena 的 lock_count 对齐）：
    //   Unlock(lp) 必须命中**对应那一层**的指针（真机语义）。
    kSfLockStack = 80,
    kSfLockStackDepth = 8,
    kSfBytes = 112,
};
constexpr std::uint32_t kSfMagicValue = 0x31534444u;  // 'DDS1'

// P1→P2 的字体 DC 接缝：GetDC 直接用 P2 的 `gdiCreateDc` 建 DC、`fontAttachDcSurface` 绑像素
// （runtime/font.h 的合同：hdc 必须是 GDI DC 句柄，否则 attach 会明确失败并打日志）。
void ddrawLog(const char* what, const char* fmt, ...) {
    std::fprintf(stderr, "[ddraw] %s: ", what);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

bool requireMachine(X86& cpu, Machine** out, std::string* why) {
    Machine* m = activeMachine();
    if (!m || &m->cpu() != &cpu) {
        if (why) *why = "DDraw：activeMachine() 不是这台 cpu 的 Machine（装配问题）";
        return false;
    }
    if (!m->surfaces().ready()) {
        if (why) *why = "DDraw：SurfaceArena 未初始化";
        return false;
    }
    *out = m;
    return true;
}

GuestSurface* guestSurface(Machine& m, std::uint32_t arena_ea) { return m.surfaces().find(arena_ea); }

bool readSurfaceState(X86& cpu, std::uint32_t state, SurfaceInfo* out, std::string* why) {
    if (state == 0) {
        if (why) *why = "DDraw：surface 状态块 EA 为 0";
        return false;
    }
    if (!cpu.mem->committed(state, kSfBytes)) {
        if (why) *why = "DDraw：surface 状态块未提交（野指针？）";
        return false;
    }
    const std::uint32_t magic = cpu.mem->rd32(state + kSfMagic);
    if (magic != kSfMagicValue) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "DDraw：surface 状态块 0x%08X 的 magic 是 0x%08X（应为 'DDS1'）",
                      state, magic);
        if (why) *why = buf;
        return false;
    }
    SurfaceInfo s;
    s.state = state;
    s.arena = cpu.mem->rd32(state + kSfArena);
    s.object = cpu.mem->rd32(state + kSfObject);
    s.width = static_cast<int>(cpu.mem->rd32(state + kSfWidth));
    s.height = static_cast<int>(cpu.mem->rd32(state + kSfHeight));
    s.pitch = cpu.mem->rd32(state + kSfPitch);
    s.format.bpp = static_cast<std::uint16_t>(cpu.mem->rd32(state + kSfBpp));
    s.format.rmask = cpu.mem->rd32(state + kSfRMask);
    s.format.gmask = cpu.mem->rd32(state + kSfGMask);
    s.format.bmask = cpu.mem->rd32(state + kSfBMask);
    s.format.amask = cpu.mem->rd32(state + kSfAMask);
    s.caps = cpu.mem->rd32(state + kSfCaps);
    s.ck_flags = cpu.mem->rd32(state + kSfCkFlags);
    s.ck_low = cpu.mem->rd32(state + kSfCkLow);
    s.ck_high = cpu.mem->rd32(state + kSfCkHigh);
    s.hdc = cpu.mem->rd32(state + kSfHdc);
    s.dc_count = cpu.mem->rd32(state + kSfDcCount);
    s.lock_ptr = cpu.mem->rd32(state + kSfLockPtr);
    s.primary = cpu.mem->rd32(state + kSfPrimary) != 0;
    if (s.format.bpp != 16 && s.format.bpp != 32) {
        if (why) *why = "DDraw：surface 状态块里的 bpp 不是 16/32（状态块被写坏？）";
        return false;
    }
    *out = s;
    return true;
}

// 按 HDC 找 surface 状态块（GetDC / ReleaseDC 用；链表遍历）
std::uint32_t findStateByDc(X86& cpu, std::uint32_t hdc, std::uint32_t* dev) {
    std::uint32_t d = deviceState(cpu, nullptr);
    if (d == 0) return 0;
    std::uint32_t cur = cpu.mem->rd32(d + kDevSurfaceHead);
    std::uint32_t guard = 0;
    while (cur != 0 && guard++ < 256u) {
        if (cpu.mem->rd32(cur + kSfMagic) != kSfMagicValue) break;
        if (cpu.mem->rd32(cur + kSfHdc) == hdc) {
            if (dev) *dev = d;
            return cur;
        }
        cur = cpu.mem->rd32(cur + kSfNext);
    }
    return 0;
}

void writePixelFormat(X86& cpu, std::uint32_t desc_ea, const SurfaceFormat& fmt) {
    const std::uint32_t pf = desc_ea + kOffPixelFormat;
    cpu.mem->wr32(pf + kPfSize, kPixelFormatBytes);
    std::uint32_t flags = kPfRgb;
    if (fmt.amask != 0) flags |= kPfAlphaPixels;
    cpu.mem->wr32(pf + kPfFlags, flags);
    cpu.mem->wr32(pf + kPfFourCC, 0);
    cpu.mem->wr32(pf + kPfBitCount, fmt.bpp);
    cpu.mem->wr32(pf + kPfRMask, fmt.rmask);
    cpu.mem->wr32(pf + kPfGMask, fmt.gmask);
    cpu.mem->wr32(pf + kPfBMask, fmt.bmask);
    cpu.mem->wr32(pf + kPfAMask, fmt.amask);
}

void writeCommonDesc(X86& cpu, std::uint32_t desc_ea, int width, int height, std::uint32_t pitch,
                     const SurfaceFormat& fmt, std::uint32_t flags) {
    cpu.mem->wr32(desc_ea + kOffSize, kDescBytes);
    cpu.mem->wr32(desc_ea + kOffFlags, flags);
    cpu.mem->wr32(desc_ea + kOffHeight, static_cast<std::uint32_t>(height));
    cpu.mem->wr32(desc_ea + kOffWidth, static_cast<std::uint32_t>(width));
    cpu.mem->wr32(desc_ea + kOffPitch, pitch);
    writePixelFormat(cpu, desc_ea, fmt);
}

bool committedOut(X86& cpu, std::uint32_t ea, std::size_t n, const char* what, std::string* why) {
    if (ea == 0) {
        if (why) *why = std::string("DDraw：") + what + " 为 NULL";
        return false;
    }
    if (!cpu.mem->committed(ea, n)) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "DDraw：%s = 0x%08X 起 %u 字节未提交（guest 缓冲区非法）", what,
                      ea, static_cast<unsigned>(n));
        if (why) *why = buf;
        return false;
    }
    return true;
}

Rect clipRect(Rect r, int width, int height) {
    r.left = std::max(r.left, 0);
    r.top = std::max(r.top, 0);
    r.right = std::min(r.right, width);
    r.bottom = std::min(r.bottom, height);
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// 设备状态
// ---------------------------------------------------------------------------------------
std::uint32_t deviceState(X86& cpu, std::string* why) {
    if (!cpu.mem) {
        if (why) *why = "DDraw：cpu.mem 为空";
        return 0;
    }
    if (!cpu.mem->committed(kDeviceEa, 16)) {
        if (!cpu.mem->commitZeroed(kDeviceEa, kDevicePageBytes, RegionKind::Other, "ddraw-device")) {
            if (why) *why = "DDraw：提交设备状态页 0x7FFFC000 失败";
            return 0;
        }
    }
    if (cpu.mem->rd32(kDeviceEa + kDevMagic) != kDevMagicValue) {
        cpu.mem->fillBytes(kDeviceEa, 0, kDeviceBytes);
        cpu.mem->wr32(kDeviceEa + kDevMagic, kDevMagicValue);
        cpu.mem->wr32(kDeviceEa + kDevWidth, static_cast<std::uint32_t>(kDefaultWidth));
        cpu.mem->wr32(kDeviceEa + kDevHeight, static_cast<std::uint32_t>(kDefaultHeight));
        const SurfaceFormat fmt = formatForBpp(kDefaultBpp);
        cpu.mem->wr32(kDeviceEa + kDevBpp, kDefaultBpp);
        cpu.mem->wr32(kDeviceEa + kDevRMask, fmt.rmask);
        cpu.mem->wr32(kDeviceEa + kDevGMask, fmt.gmask);
        cpu.mem->wr32(kDeviceEa + kDevBMask, fmt.bmask);
        cpu.mem->wr32(kDeviceEa + kDevAMask, fmt.amask);
        ddrawLog("device", "设备状态页 0x%08X 初始化：默认显示模式 %dx%dx%u（plan D4：内部默认 32bpp）",
                 kDeviceEa, kDefaultWidth, kDefaultHeight, kDefaultBpp);
    }
    return kDeviceEa;
}

bool displayMode(X86& cpu, int* width, int* height, unsigned* bpp, SurfaceFormat* fmt) {
    const std::uint32_t d = deviceState(cpu, nullptr);
    if (d == 0) return false;
    if (width) *width = static_cast<int>(cpu.mem->rd32(d + kDevWidth));
    if (height) *height = static_cast<int>(cpu.mem->rd32(d + kDevHeight));
    const unsigned b = cpu.mem->rd32(d + kDevBpp);
    if (bpp) *bpp = b;
    if (fmt) {
        fmt->bpp = static_cast<std::uint16_t>(b);
        fmt->rmask = cpu.mem->rd32(d + kDevRMask);
        fmt->gmask = cpu.mem->rd32(d + kDevGMask);
        fmt->bmask = cpu.mem->rd32(d + kDevBMask);
        fmt->amask = cpu.mem->rd32(d + kDevAMask);
    }
    return true;
}

bool setDisplayMode(X86& cpu, int width, int height, unsigned bpp, std::string* why) {
    if (bpp != 16 && bpp != 32) {
        if (why) *why = "DDraw：只支持 16 / 32bpp（plan D4）";
        return false;
    }
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        if (why) *why = "DDraw：显示模式尺寸非法";
        return false;
    }
    const std::uint32_t d = deviceState(cpu, why);
    if (d == 0) return false;
    const SurfaceFormat fmt = formatForBpp(bpp);
    cpu.mem->wr32(d + kDevWidth, static_cast<std::uint32_t>(width));
    cpu.mem->wr32(d + kDevHeight, static_cast<std::uint32_t>(height));
    cpu.mem->wr32(d + kDevBpp, bpp);
    cpu.mem->wr32(d + kDevRMask, fmt.rmask);
    cpu.mem->wr32(d + kDevGMask, fmt.gmask);
    cpu.mem->wr32(d + kDevBMask, fmt.bmask);
    cpu.mem->wr32(d + kDevAMask, fmt.amask);
    ddrawLog("SetDisplayMode", "%dx%dx%u（pitch = width*%u；掩码 %08X/%08X/%08X）", width, height, bpp,
             fmt.bytesPerPixel(), fmt.rmask, fmt.gmask, fmt.bmask);
    return true;
}

void setCooperativeLevel(X86& cpu, std::uint32_t hwnd, std::uint32_t flags) {
    const std::uint32_t d = deviceState(cpu, nullptr);
    if (d == 0) return;
    cpu.mem->wr32(d + kDevHwnd, hwnd);
    cpu.mem->wr32(d + kDevCoopFlags, flags);
    // 偏差（SCOPE）：不建真窗口 / 不独占全屏 —— 只记下 HWND 与 flags（plan §10：windowed 映射）
    ddrawLog("SetCooperativeLevel", "hwnd=0x%08X flags=0x%08X（headless：不独占全屏，仅记录）", hwnd, flags);
}

std::uint32_t primarySurface(X86& cpu) {
    const std::uint32_t d = deviceState(cpu, nullptr);
    return d == 0 ? 0u : cpu.mem->rd32(d + kDevPrimary);
}

// ---------------------------------------------------------------------------------------
// surface 状态块
// ---------------------------------------------------------------------------------------
std::uint32_t createSurfaceState(X86& cpu, int width, int height, std::uint32_t caps, bool primary,
                                 unsigned bpp_override, std::string* why) {    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return 0;
    if (!cpu.heap || !cpu.heap->ready()) {
        if (why) *why = "DDraw：guest 堆未就绪（surface 状态块要放在 guest 堆上）";
        return 0;
    }
    if (width <= 0 || height <= 0 || width > 16384 || height > 16384) {
        if (why) *why = "DDraw：surface 尺寸非法";
        return 0;
    }
    const std::uint32_t d = deviceState(cpu, why);
    if (d == 0) return 0;
    if (primary && cpu.mem->rd32(d + kDevPrimary) != 0) {
        if (why) *why = "DDraw：primary surface 已经存在（DDERR_PRIMARYSURFACEALREADYEXISTS）";
        return 0;
    }
    unsigned bpp = 0;
    SurfaceFormat fmt;
    if (!displayMode(cpu, nullptr, nullptr, &bpp, &fmt)) {
        if (why) *why = "DDraw：读不到当前显示格式";
        return 0;
    }
    if (bpp_override != 0) {
        if (bpp_override != 16 && bpp_override != 32) {
            if (why) *why = "DDraw：只支持 16 / 32bpp 的 surface";
            return 0;
        }
        bpp = bpp_override;
        fmt = formatForBpp(bpp);
    }
    const std::uint32_t arena_ea = m->surfaces().create(width, height, fmt);
    if (arena_ea == 0) {
        if (why) *why = std::string("DDraw：SurfaceArena.create 失败：") + m->surfaces().lastError();
        return 0;
    }
    const GuestSurface* gs = m->surfaces().find(arena_ea);
    const std::uint32_t state = cpu.heap->alloc(kSfBytes, /*zero=*/true);
    if (state == 0) {
        m->surfaces().destroy(arena_ea);
        if (why) *why = std::string("DDraw：guest 堆分配 surface 状态块失败：") + cpu.heap->lastError();
        return 0;
    }
    const std::uint32_t serial = cpu.mem->rd32(d + kDevSerial) + 1u;
    cpu.mem->wr32(d + kDevSerial, serial);
    cpu.mem->wr32(state + kSfMagic, kSfMagicValue);
    cpu.mem->wr32(state + kSfArena, arena_ea);
    cpu.mem->wr32(state + kSfObject, 0);
    cpu.mem->wr32(state + kSfWidth, static_cast<std::uint32_t>(width));
    cpu.mem->wr32(state + kSfHeight, static_cast<std::uint32_t>(height));
    cpu.mem->wr32(state + kSfPitch, gs ? gs->pitch : 0u);
    cpu.mem->wr32(state + kSfBpp, bpp);
    cpu.mem->wr32(state + kSfRMask, fmt.rmask);
    cpu.mem->wr32(state + kSfGMask, fmt.gmask);
    cpu.mem->wr32(state + kSfBMask, fmt.bmask);
    cpu.mem->wr32(state + kSfAMask, fmt.amask);
    cpu.mem->wr32(state + kSfCaps, caps);
    cpu.mem->wr32(state + kSfHdc, 0);  // GetDC 时才建真正的 GDI DC（P1→P2 接缝）
    cpu.mem->wr32(state + kSfPrimary, primary ? 1u : 0u);
    // 头插链表（GetDC / ReleaseDC / 诊断要遍历）
    cpu.mem->wr32(state + kSfNext, cpu.mem->rd32(d + kDevSurfaceHead));
    cpu.mem->wr32(d + kDevSurfaceHead, state);
    if (primary) cpu.mem->wr32(d + kDevPrimary, state);
    return state;
}

bool surfaceInfoOfState(X86& cpu, std::uint32_t state, SurfaceInfo* out, std::string* why) {
    return readSurfaceState(cpu, state, out, why);
}

std::uint32_t stateOfObject(X86& cpu, std::uint32_t object) {
    if (!cpu.com || object == 0) return 0;
    const std::uint32_t user = cpu.com->userOf(object);
    if (user == 0 || !cpu.mem || !cpu.mem->committed(user, 4)) return 0;
    if (cpu.mem->rd32(user + kSfMagic) != kSfMagicValue) return 0;
    return user;
}

bool surfaceInfo(X86& cpu, std::uint32_t object, SurfaceInfo* out, std::string* why) {
    const std::uint32_t state = stateOfObject(cpu, object);
    if (state == 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "DDraw：0x%08X 不是我们建的 surface 对象（state=0）", object);
        if (why) *why = buf;
        return false;
    }
    return readSurfaceState(cpu, state, out, why);
}

void bindSurfaceObject(X86& cpu, std::uint32_t state, std::uint32_t object) {
    if (state && cpu.mem && cpu.mem->committed(state, kSfBytes))
        cpu.mem->wr32(state + kSfObject, object);
}

void destroySurfaceState(X86& cpu, std::uint32_t state) {
    if (state == 0 || !cpu.mem || !cpu.mem->committed(state, kSfBytes)) return;
    if (cpu.mem->rd32(state + kSfMagic) != kSfMagicValue) return;
    const std::uint32_t d = deviceState(cpu, nullptr);
    const std::uint32_t arena_ea = cpu.mem->rd32(state + kSfArena);
    // 从链表里摘掉
    if (d != 0) {
        std::uint32_t prev = 0;
        std::uint32_t cur = cpu.mem->rd32(d + kDevSurfaceHead);
        std::uint32_t guard = 0;
        while (cur != 0 && guard++ < 256u) {
            if (cur == state) {
                const std::uint32_t next = cpu.mem->rd32(cur + kSfNext);
                if (prev == 0) cpu.mem->wr32(d + kDevSurfaceHead, next);
                else cpu.mem->wr32(prev + kSfNext, next);
                break;
            }
            prev = cur;
            cur = cpu.mem->rd32(cur + kSfNext);
        }
        if (cpu.mem->rd32(d + kDevPrimary) == state) cpu.mem->wr32(d + kDevPrimary, 0);
    }
    cpu.mem->wr32(state + kSfMagic, 0);
    if (Machine* m = activeMachine(); m && &m->cpu() == &cpu) {
        if (arena_ea != 0) m->surfaces().destroy(arena_ea);
    }
    if (cpu.heap) cpu.heap->free(state);
}

// ---------------------------------------------------------------------------------------
// DDSURFACEDESC
// ---------------------------------------------------------------------------------------
bool fillSurfaceDesc(X86& cpu, std::uint32_t desc_ea, const SurfaceInfo& s, bool include_surface,
                     std::string* why) {
    if (!committedOut(cpu, desc_ea, kDescBytes, "lpDDSurfaceDesc", why)) return false;
    std::uint32_t flags = kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPitch | kDdsdPixelFormat;
    if (include_surface) flags |= kDdsdLpSurface;
    writeCommonDesc(cpu, desc_ea, s.width, s.height, s.pitch, s.format, flags);
    cpu.mem->wr32(desc_ea + kOffLpSurface, include_surface ? s.lock_ptr : 0u);
    cpu.mem->wr32(desc_ea + kOffCaps, s.caps);
    return true;
}

bool fillModeDesc(X86& cpu, std::uint32_t desc_ea, int width, int height, unsigned bpp,
                  const SurfaceFormat& fmt, std::uint32_t /*flags*/, std::string* why) {
    if (!committedOut(cpu, desc_ea, kDescBytes, "lpDDSurfaceDesc", why)) return false;
    const std::uint32_t pitch =
        static_cast<std::uint32_t>(width) * (bpp / 8u);  // 模式描述里的 lPitch（显示模式 = 线性）
    const std::uint32_t flags =
        kDdsdCaps | kDdsdHeight | kDdsdWidth | kDdsdPitch | kDdsdPixelFormat | kDdsdRefreshRate;
    writeCommonDesc(cpu, desc_ea, width, height, pitch, fmt, flags);
    cpu.mem->wr32(desc_ea + kOffRefreshRate, kRefreshRate);
    cpu.mem->wr32(desc_ea + kOffCaps, kCapsPrimarySurface | kCapsVisible);
    return true;
}

// ---------------------------------------------------------------------------------------
// 像素路径
// ---------------------------------------------------------------------------------------
std::uint32_t loadPixel(const std::uint8_t* p, unsigned bpp) {
    if (bpp == 32) {
        std::uint32_t v = 0;
        std::memcpy(&v, p, 4);
        return v;
    }
    std::uint16_t v = 0;
    std::memcpy(&v, p, 2);
    return v;
}

void storePixel(std::uint8_t* p, unsigned bpp, std::uint32_t v) {
    if (bpp == 32) {
        std::memcpy(p, &v, 4);
        return;
    }
    const std::uint16_t v16 = static_cast<std::uint16_t>(v & 0xFFFFu);
    std::memcpy(p, &v16, 2);
}

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

std::uint32_t blt(X86& cpu, const SurfaceInfo& dst, const Rect* dst_rect, const SurfaceInfo& src,
                  const Rect* src_rect, std::uint32_t flags, std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kGeneric;
    GuestSurface* d = guestSurface(*m, dst.arena);
    GuestSurface* s = guestSurface(*m, src.arena);
    if (!d || !s) {
        if (why) *why = "DDraw：Blt 的源/目标 surface 不在 arena 里";
        return kSurfaceLost;
    }
    const unsigned bpp = dst.format.bpp;
    Rect dr = dst_rect ? *dst_rect : Rect{0, 0, dst.width, dst.height};
    Rect sr = src_rect ? *src_rect : Rect{0, 0, src.width, src.height};
    const int sw = sr.right - sr.left;
    const int sh = sr.bottom - sr.top;
    const int dw = dr.right - dr.left;
    const int dh = dr.bottom - dr.top;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) {
        if (why) *why = "DDraw：Blt 的矩形为空";
        return kInvalidRect;
    }
    const Rect cdr = clipRect(dr, dst.width, dst.height);
    const Rect csr = clipRect(sr, src.width, src.height);
    if (cdr.empty() || csr.empty()) return kOk;  // 完全在表面外 ⇒ 没有像素要搬（⛔ 不越界写）

    std::uint8_t* dp = m->surfaces().pixels(*d);
    const std::uint8_t* sp = m->surfaces().pixels(*s);
    if (!dp || !sp) {
        if (why) *why = "DDraw：Blt 拿不到像素指针";
        return kGeneric;
    }
    const bool key = (flags & kBltKeySrc) != 0;
    const std::uint32_t bytes = bpp / 8u;
    // 齐次（源/目标尺寸相同）⇒ 1:1 精确裁剪；不同尺寸 ⇒ 最近邻拉伸（⛔ 不做滤波）
    if (sw == dw && sh == dh) {
        const int off_x = cdr.left - dr.left;  // 裁剪掉的列数
        const int off_y = cdr.top - dr.top;
        for (int y = 0; y < (cdr.bottom - cdr.top); ++y) {
            const int sy = sr.top + off_y + y;
            if (sy < 0 || sy >= s->height) continue;
            const std::uint8_t* srow = sp + static_cast<std::size_t>(sy) * s->pitch;
            std::uint8_t* drow = dp + static_cast<std::size_t>(cdr.top + y) * d->pitch;
            for (int x = 0; x < (cdr.right - cdr.left); ++x) {
                const int sx = sr.left + off_x + x;
                if (sx < 0 || sx >= s->width) continue;
                const std::uint32_t px = loadPixel(srow + static_cast<std::size_t>(sx) * bytes, bpp);
                if (key && colorKeyHit(src.ck_flags, src.ck_low, src.ck_high, px)) continue;
                storePixel(drow + static_cast<std::size_t>(cdr.left + x) * bytes, bpp, px);
            }
        }
        return kOk;
    }
    // 拉伸：目标像素 → 源像素（用**裁剪后**的两段做比例；越界部分本来就不该画）
    for (int y = cdr.top; y < cdr.bottom; ++y) {
        const int sy = csr.top + static_cast<int>(static_cast<long long>(y - cdr.top) *
                                                  (csr.bottom - csr.top) / (cdr.bottom - cdr.top));
        if (sy < 0 || sy >= s->height) continue;
        const std::uint8_t* srow = sp + static_cast<std::size_t>(sy) * s->pitch;
        std::uint8_t* drow = dp + static_cast<std::size_t>(y) * d->pitch;
        for (int x = cdr.left; x < cdr.right; ++x) {
            const int sx = csr.left + static_cast<int>(static_cast<long long>(x - cdr.left) *
                                                       (csr.right - csr.left) / (cdr.right - cdr.left));
            if (sx < 0 || sx >= s->width) continue;
            const std::uint32_t px = loadPixel(srow + static_cast<std::size_t>(sx) * bytes, bpp);
            if (key && colorKeyHit(src.ck_flags, src.ck_low, src.ck_high, px)) continue;
            storePixel(drow + static_cast<std::size_t>(x) * bytes, bpp, px);
        }
    }
    return kOk;
}

std::uint32_t bltFast(X86& cpu, const SurfaceInfo& dst, int x, int y, const SurfaceInfo& src,
                      const Rect* src_rect, std::uint32_t flags, std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kGeneric;
    GuestSurface* d = guestSurface(*m, dst.arena);
    GuestSurface* s = guestSurface(*m, src.arena);
    if (!d || !s) {
        if (why) *why = "DDraw：BltFast 的源/目标 surface 不在 arena 里";
        return kSurfaceLost;
    }
    const unsigned bpp = dst.format.bpp;
    const Rect sr = src_rect ? *src_rect : Rect{0, 0, src.width, src.height};
    if (sr.right <= sr.left || sr.bottom <= sr.top) {
        if (why) *why = "DDraw：BltFast 的源矩形为空";
        return kInvalidRect;
    }
    const Rect csr = clipRect(sr, src.width, src.height);
    if (csr.empty()) return kOk;

    std::uint8_t* dp = m->surfaces().pixels(*d);
    const std::uint8_t* sp = m->surfaces().pixels(*s);
    if (!dp || !sp) {
        if (why) *why = "DDraw：BltFast 拿不到像素指针";
        return kGeneric;
    }
    const bool key = (flags & kBltFastSrcColorKey) != 0;
    const std::uint32_t bytes = bpp / 8u;
    // 1:1；源矩形裁剪后按**同一偏移**映射到目标（⛔ 不越界写）
    for (int sy = csr.top; sy < csr.bottom; ++sy) {
        const int dy = y + (sy - sr.top);
        if (dy < 0 || dy >= d->height) continue;
        const std::uint8_t* srow = sp + static_cast<std::size_t>(sy) * s->pitch;
        std::uint8_t* drow = dp + static_cast<std::size_t>(dy) * d->pitch;
        for (int sx = csr.left; sx < csr.right; ++sx) {
            const int dx = x + (sx - sr.left);
            if (dx < 0 || dx >= d->width) continue;
            const std::uint32_t px = loadPixel(srow + static_cast<std::size_t>(sx) * bytes, bpp);
            if (key && colorKeyHit(src.ck_flags, src.ck_low, src.ck_high, px)) continue;
            storePixel(drow + static_cast<std::size_t>(dx) * bytes, bpp, px);
        }
    }
    return kOk;
}

// ---------------------------------------------------------------------------------------
// 锁 / DC
// ---------------------------------------------------------------------------------------
std::uint32_t surfaceLockCount(X86& cpu, const SurfaceInfo& s) {
    Machine* m = activeMachine();
    if (!m || &m->cpu() != &cpu) return 0;
    const GuestSurface* gs = m->surfaces().find(s.arena);
    return gs ? gs->lock_count : 0;
}

std::uint32_t lock(X86& cpu, const SurfaceInfo& s, const Rect* rect, std::uint32_t desc_ea,
                   std::uint32_t flags, std::uint32_t event, std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kGeneric;
    if (event != 0) {
        if (why) *why = "DDraw：Lock 的 hEvent 非 NULL（异步锁未实现）";
        return kUnsupported;
    }
    const std::uint32_t allowed = kLockWait | kLockReadOnly | kLockNoSysLock;
    if ((flags & ~allowed) != 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "DDraw：Lock 的 dwFlags=0x%08X 含未实现的位（支持 WAIT/READONLY/NOSYSLOCK）",
                      flags);
        if (why) *why = buf;
        return kUnsupported;
    }
    if (!committedOut(cpu, desc_ea, kDescBytes, "lpDDSurfaceDesc", why)) return kInvalidParams;
    if (s.dc_count != 0) {
        if (why) *why = "DDraw：surface 有未释放的 DC（GetDC 之后又 Lock）";
        return kSurfaceBusy;
    }
    Rect r = rect ? *rect : Rect{0, 0, s.width, s.height};
    const Rect c = clipRect(r, s.width, s.height);
    if (c.empty()) {
        if (why) *why = "DDraw：Lock 的矩形完全在 surface 外";
        return kInvalidRect;
    }
    const std::uint32_t bytes = s.format.bpp / 8u;
    const std::uint32_t ptr = s.arena + static_cast<std::uint32_t>(c.top) * s.pitch +
                              static_cast<std::uint32_t>(c.left) * bytes;
    if (!m->surfaces().lock(s.arena)) {
        if (why) *why = "DDraw：SurfaceArena.lock 失败（surface 不在 arena 里？）";
        return kGeneric;
    }
    SurfaceInfo copy = s;
    copy.lock_ptr = ptr;
    if (!fillSurfaceDesc(cpu, desc_ea, copy, /*include_surface=*/true, why)) {
        m->surfaces().unlock(s.arena);
        return kInvalidParams;
    }
    // 状态块：记下这一层的 lpSurface（Unlock 按层校验；GetSurfaceDesc 报最上层的）
    if (cpu.mem->committed(s.state, kSfBytes)) {
        const std::uint32_t depth = surfaceLockCount(cpu, s);  // 已经 +1
        cpu.mem->wr32(s.state + kSfLockPtr, ptr);
        if (depth >= 1 && depth <= kSfLockStackDepth)
            cpu.mem->wr32(s.state + kSfLockStack + (depth - 1) * 4u, ptr);
    }
    return kOk;
}

std::uint32_t unlock(X86& cpu, const SurfaceInfo& s, std::uint32_t lp_surface, std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kGeneric;
    const GuestSurface* gs = m->surfaces().find(s.arena);
    if (!gs || gs->lock_count == 0) {
        if (why) *why = "DDraw：Unlock 一个没有锁的 surface";
        return kNotLocked;
    }
    // 校验这是**对应那一层**的指针（嵌套锁时逐层匹配；真机也是这么判的）
    std::uint32_t expected = s.lock_ptr;
    if (gs->lock_count <= kSfLockStackDepth && cpu.mem->committed(s.state, kSfBytes))
        expected = cpu.mem->rd32(s.state + kSfLockStack + (gs->lock_count - 1u) * 4u);
    if (lp_surface != 0 && lp_surface != expected) {
        if (why) *why = "DDraw：Unlock 的 lpSurface 与对应那一层 Lock 返回的不一致";
        return kInvalidParams;
    }
    if (!m->surfaces().unlock(s.arena)) {
        if (why) *why = "DDraw：SurfaceArena.unlock 失败";
        return kGeneric;
    }
    if (cpu.mem->committed(s.state, kSfBytes) && surfaceLockCount(cpu, s) == 0)
        cpu.mem->wr32(s.state + kSfLockPtr, 0);
    return kOk;
}

std::uint32_t getDC(X86& cpu, const SurfaceInfo& s, std::uint32_t* out_hdc, std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kGeneric;
    if (!out_hdc) {
        if (why) *why = "DDraw：GetDC 的 lphDC 为 NULL";
        return kInvalidParams;
    }
    const GuestSurface* gs = m->surfaces().find(s.arena);
    if (!gs) {
        if (why) *why = "DDraw：GetDC 的 surface 不在 arena 里";
        return kSurfaceLost;
    }
    if (gs->lock_count != 0) {
        if (why) *why = "DDraw：surface 已 Lock，GetDC 失败（真机 DDERR_SURFACEBUSY）";
        return kSurfaceBusy;
    }
    std::uint32_t hdc = s.hdc;
    if (hdc == 0) {
        // ★ P1→P2 接缝：建一个**真正的 GDI DC**（P2 的 fontAttachDcSurface 只认 GDI DC 句柄），
        //   并把它的目标绑到这块 surface 的 guest 像素上。第一次 GetDC 时建，最后一次 ReleaseDC 时回收。
        if (!cpu.handles) {
            if (why) *why = "DDraw：cpu.handles 为空（GDI DC 建不出来）";
            return kGeneric;
        }
        hdc = gdiCreateDc(*cpu.handles, /*display=*/false, "DDRAW");
        if (hdc == 0) {
            if (why) *why = "DDraw：gdiCreateDc 失败（句柄表分配不出来）";
            return kGeneric;
        }
        if (!fontAttachDcSurface(hdc, s.arena, &m->surfaces())) {
            if (why) *why = "DDraw：fontAttachDcSurface 失败（P2 字体层没接上 ⇒ TextOutA 画不进 surface）";
            releasePayload(*cpu.handles, hdc);
            cpu.handles->close(hdc);
            return kGeneric;
        }
        if (cpu.mem->committed(s.state, kSfBytes)) cpu.mem->wr32(s.state + kSfHdc, hdc);
        ddrawLog("GetDC", "surface 0x%08X 建 surface DC 0x%08X 并绑定像素 0x%08X（%dx%d %dbpp）", s.object,
                 hdc, s.arena, s.width, s.height, s.format.bpp);
    }
    const std::uint32_t count = s.dc_count + 1u;
    if (cpu.mem->committed(s.state, kSfBytes)) cpu.mem->wr32(s.state + kSfDcCount, count);
    *out_hdc = hdc;
    ddrawLog("GetDC", "surface=0x%08X ⇒ HDC 0x%08X（第 %u 次；像素 0x%08X pitch=%u %dbpp）", s.object, hdc,
             count, s.arena, s.pitch, s.format.bpp);
    return kOk;
}

std::uint32_t releaseDC(X86& cpu, std::uint32_t hdc, std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kGeneric;
    if (hdc == 0) {
        if (why) *why = "DDraw：ReleaseDC 的 HDC 为 NULL";
        return kInvalidParams;
    }
    const std::uint32_t state = findStateByDc(cpu, hdc, nullptr);
    if (state == 0) {
        if (why) *why = "DDraw：ReleaseDC 的 HDC 不是本进程发出去的 surface DC";
        return kInvalidObject;
    }
    SurfaceInfo s;
    if (!readSurfaceState(cpu, state, &s, why)) return kInvalidObject;
    if (s.dc_count == 0) {
        if (why) *why = "DDraw：ReleaseDC 多于 GetDC";
        return kInvalidObject;
    }
    const std::uint32_t count = s.dc_count - 1u;
    cpu.mem->wr32(state + kSfDcCount, count);
    if (count == 0) {
        // 最后一次 ReleaseDC：解绑 + 回收 GDI DC（payload 的 destroy 由句柄表统一做）
        fontDetachDc(hdc);
        if (cpu.handles) {
            releasePayload(*cpu.handles, hdc);
            cpu.handles->close(hdc);
        }
        cpu.mem->wr32(state + kSfHdc, 0);
    }
    return kOk;
}

// ---------------------------------------------------------------------------------------
// 呈现
// ---------------------------------------------------------------------------------------
std::uint32_t flip(X86& cpu, const SurfaceInfo& s, std::uint32_t back_surface, std::uint32_t flags,
                   std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kGeneric;
    if (!s.primary) {
        if (why) *why = "DDraw：Flip 的对象不是 primary surface";
        return kNotFlippable;
    }
    if (back_surface != 0) {
        if (why) *why = "DDraw：Flip 指定了 back buffer（未实现：只翻 primary）";
        return kUnsupported;
    }
    const std::uint32_t allowed = kFlipWait | kFlipNoVsync;
    if ((flags & ~allowed) != 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "DDraw：Flip 的 dwFlags=0x%08X 含未实现的位（支持 WAIT/NOVSYNC）", flags);
        if (why) *why = buf;
        return kUnsupported;
    }
    GuestSurface* gs = guestSurface(*m, s.arena);
    if (!gs) {
        if (why) *why = "DDraw：Flip 的 primary 像素不在 arena 里";
        return kSurfaceLost;
    }
    const std::uint32_t d = deviceState(cpu, nullptr);
    std::string err;
    if (!m->presenter().present(m->surfaces(), *gs, std::string(), &err)) {
        if (why) *why = "DDraw：HeadlessPresenter.present 失败：" + err;
        return kGeneric;
    }
    if (d != 0) cpu.mem->wr32(d + kDevFlips, cpu.mem->rd32(d + kDevFlips) + 1u);
    ddrawLog("Flip", "帧 #%llu hash=0x%016llX（%dx%d %dbpp）",
             static_cast<unsigned long long>(m->presenter().frameCount()),
             static_cast<unsigned long long>(m->presenter().lastHash()), s.width, s.height,
             s.format.bpp);
    return kOk;
}

// ---------------------------------------------------------------------------------------
// GetCaps / SetColorKey / GetAvailableVidMem
// ---------------------------------------------------------------------------------------
std::uint32_t getCaps(X86& cpu, const SurfaceInfo& s, std::uint32_t caps_ea, std::string* why) {
    if (!committedOut(cpu, caps_ea, 4, "lpDDSCaps", why)) return kInvalidParams;
    std::uint32_t caps = s.caps;
    if (s.primary) caps |= kCapsPrimarySurface | kCapsVisible;
    cpu.mem->wr32(caps_ea, caps);
    return kOk;
}

std::uint32_t setColorKey(X86& cpu, const SurfaceInfo& s, std::uint32_t flags, std::uint32_t key_ea,
                          std::string* why) {
    const std::uint32_t allowed = kCkSrcBlt | kCkDestBlt;
    if ((flags & ~allowed) != 0) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "DDraw：SetColorKey 的 dwFlags=0x%08X 含未实现的位（支持 SRCBLT/DESTBLT）",
                      flags);
        if (why) *why = buf;
        return kUnsupported;
    }
    std::uint32_t low = 0;
    std::uint32_t high = 0;
    if (key_ea != 0) {
        if (!committedOut(cpu, key_ea, 8, "lpDDColorKey", why)) return kInvalidParams;
        low = cpu.mem->rd32(key_ea);
        high = cpu.mem->rd32(key_ea + 4);
        if (low > high) {
            if (why) *why = "DDraw：SetColorKey 的 low > high（非法区间）";
            return kInvalidParams;
        }
    }
    cpu.mem->wr32(s.state + kSfCkFlags, flags);
    cpu.mem->wr32(s.state + kSfCkLow, low);
    cpu.mem->wr32(s.state + kSfCkHigh, high);
    ddrawLog("SetColorKey", "surface=0x%08X flags=0x%08X key=%08X..%08X", s.object, flags, low, high);
    return kOk;
}

std::uint32_t availableVidMem(X86& cpu, std::uint32_t caps_ea, std::uint32_t total_ea,
                              std::uint32_t free_ea, std::string* why) {
    Machine* m = nullptr;
    if (!requireMachine(cpu, &m, why)) return kGeneric;
    if (!committedOut(cpu, total_ea, 4, "lpdwTotal", why)) return kInvalidParams;
    if (!committedOut(cpu, free_ea, 4, "lpdwFree", why)) return kInvalidParams;
    std::uint32_t caps = 0;
    if (caps_ea != 0) {
        if (!committedOut(cpu, caps_ea, 4, "lpDDSCaps", why)) return kInvalidParams;
        caps = cpu.mem->rd32(caps_ea);
    }
    const bool system = (caps & kCapsSystemMemory) != 0;
    const std::uint32_t total = system ? kTotalSystemMemory : kTotalVideoMemory;
    const std::uint32_t used = m->surfaces().bytesInUse();
    const std::uint32_t free_mem = used >= total ? 0u : total - used;
    cpu.mem->wr32(total_ea, total);
    cpu.mem->wr32(free_ea, free_mem);
    return kOk;
}

// ---------------------------------------------------------------------------------------
// P1→P2 的 DC 接缝
// ---------------------------------------------------------------------------------------
bool dcInfo(X86& cpu, std::uint32_t hdc, DcInfo* out) {
    if (hdc == 0 || !out) return false;
    const std::uint32_t state = findStateByDc(cpu, hdc, nullptr);
    if (state == 0) return false;
    SurfaceInfo s;
    if (!readSurfaceState(cpu, state, &s, nullptr)) return false;
    if (s.dc_count == 0) return false;  // 没有未释放的 DC ⇒ 这个 HDC 当前无效
    out->surface_object = s.object;
    out->pixels_ea = s.arena;
    out->pitch = s.pitch;
    out->width = s.width;
    out->height = s.height;
    out->format = s.format;
    return true;
}

}  // namespace ddraw
}  // namespace lift
