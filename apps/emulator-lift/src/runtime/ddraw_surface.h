// emulator-lift · DirectDraw 的**状态与像素层**（P1 / M2 · B2）
//
// ★ 分工：本文件只做"guest 内存里的状态 + 像素搬运 + DC/锁/翻页"，
//   **不认 COM**（对象 / vtable / refcount 在 com.h，方法 shim 在 shims/ddraw.cpp）。
// ★ 状态放哪儿：DDraw 的设备状态与每个 surface 的状态都放在 **guest 内存**里
//   （设备状态在固定页 0x7FFFC000，surface 状态块从 guest 堆分配；块 EA 挂在对象 `obj+8`
//   的扩展位上，见 com.h 的 `createObject(..., user)`）。
//   这样"一台机器一份 + 生命周期跟着对象"是天然的：⛔ 不需要宿主侧静态表（那会在
//   Machine 反复建在同一栈地址时串台）。像素本身在 `SurfaceArena`（P0 / G4）。
//
// ★ DDSURFACEDESC 的偏移**不是凭记忆**：108 字节 / 字段偏移来自 Windows SDK 的 `ddraw.h`
//   （`_DDSURFACEDESC` + `DDSCAPS` 只有 1 个 DWORD ⇒ 104 处是 ddsCaps.dwCaps、末尾 108）
//   并与 `.lst` 的实际读写逐项对上（见 docs 与测试注释里的 EA）：
//     · 0x440B39 读 desc+0x0C(dwWidth)/+0x08(dwHeight)/+0x54(dwRGBBitCount)
//     · 0x43B323 读 desc+0x58/0x5C/0x60（R/G/B 掩码）
//     · 0x43C9B1 写 desc+0x00 = 0x6C(dwSize)、+0x04 = 7(dwFlags)、+0x68(ddsCaps.dwCaps)
//     · 0x43AFE7 读 desc+0x18(dwRefreshRate)
// ★ DirectDraw / Surface 的 vtable 偏移同样照 SDK 头 + `.lst` 复核；取证 EA 见 shims/ddraw.cpp。
#pragma once

#include <cstdint>
#include <string>

#include "runtime/surface.h"

namespace lift {

struct X86;
class Machine;

namespace ddraw {

// ---- Win32 形状常量（值来自 SDK ddraw.h；⛔ 不手抄"看起来像"的十六进制）----
constexpr std::uint32_t kDescBytes = 108;  // sizeof(DDSURFACEDESC)

// DDSURFACEDESC 的字段偏移
enum DescOff : std::uint32_t {
    kOffSize = 0,
    kOffFlags = 4,
    kOffHeight = 8,
    kOffWidth = 12,
    kOffPitch = 16,
    kOffBackBufferCount = 20,
    kOffRefreshRate = 24,
    kOffAlphaBitDepth = 28,
    kOffReserved = 32,
    kOffLpSurface = 36,
    kOffCkDestOverlay = 40,
    kOffCkDestBlt = 48,
    kOffCkSrcOverlay = 56,
    kOffCkSrcBlt = 64,
    kOffPixelFormat = 72,
    kOffCaps = 104,
};
enum PixelFormatOff : std::uint32_t {
    kPfSize = 0,
    kPfFlags = 4,
    kPfFourCC = 8,
    kPfBitCount = 12,  // ⇒ desc+0x54
    kPfRMask = 16,     // ⇒ desc+0x58
    kPfGMask = 20,     // ⇒ desc+0x5C
    kPfBMask = 24,     // ⇒ desc+0x60
    kPfAMask = 28,
};
constexpr std::uint32_t kPixelFormatBytes = 32;  // sizeof(DDPIXELFORMAT)

// DDSD_*
constexpr std::uint32_t kDdsdCaps = 0x00000001u;
constexpr std::uint32_t kDdsdHeight = 0x00000002u;
constexpr std::uint32_t kDdsdWidth = 0x00000004u;
constexpr std::uint32_t kDdsdPitch = 0x00000008u;
constexpr std::uint32_t kDdsdBackBufferCount = 0x00000020u;
constexpr std::uint32_t kDdsdLpSurface = 0x00000800u;
constexpr std::uint32_t kDdsdPixelFormat = 0x00001000u;
constexpr std::uint32_t kDdsdRefreshRate = 0x00040000u;

// DDSCAPS_*
constexpr std::uint32_t kCapsComplex = 0x00000008u;
constexpr std::uint32_t kCapsFlip = 0x00000010u;
constexpr std::uint32_t kCapsOffscreenPlain = 0x00000040u;
constexpr std::uint32_t kCapsPrimarySurface = 0x00000200u;
constexpr std::uint32_t kCapsSystemMemory = 0x00000800u;
constexpr std::uint32_t kCapsVideoMemory = 0x00004000u;
constexpr std::uint32_t kCapsVisible = 0x00008000u;

// DDPF_*
constexpr std::uint32_t kPfRgb = 0x00000040u;
constexpr std::uint32_t kPfAlphaPixels = 0x00000001u;

// DDCKEY_* / DDBLT_* / DDBLTFAST_* / DDLOCK_* / DDFLIP_*
constexpr std::uint32_t kCkDestBlt = 0x00000002u;
constexpr std::uint32_t kCkSrcBlt = 0x00000008u;
constexpr std::uint32_t kBltKeySrc = 0x00008000u;
constexpr std::uint32_t kBltWait = 0x01000000u;
constexpr std::uint32_t kBltAsync = 0x00000200u;
constexpr std::uint32_t kBltFastSrcColorKey = 0x00000001u;
constexpr std::uint32_t kBltFastDestColorKey = 0x00000002u;
constexpr std::uint32_t kBltFastWait = 0x00000010u;
constexpr std::uint32_t kLockWait = 0x00000001u;
constexpr std::uint32_t kLockReadOnly = 0x00000010u;
constexpr std::uint32_t kLockNoSysLock = 0x00000800u;
constexpr std::uint32_t kFlipWait = 0x00000001u;
constexpr std::uint32_t kFlipNoVsync = 0x00000008u;

// ---- HRESULT（值来自 SDK ddraw.h 的 MAKE_DDHRESULT：0x8876xxxx）----
constexpr std::uint32_t kOk = 0x00000000u;
constexpr std::uint32_t kInvalidObject = 0x88760082u;   // DDERR_INVALIDOBJECT
constexpr std::uint32_t kInvalidParams = 0x80070057u;   // DDERR_INVALIDPARAMS = E_INVALIDARG
constexpr std::uint32_t kInvalidRect = 0x88760096u;     // DDERR_INVALIDRECT
constexpr std::uint32_t kNotFound = 0x887600FFu;        // DDERR_NOTFOUND
constexpr std::uint32_t kSurfaceBusy = 0x887601AEu;     // DDERR_SURFACEBUSY
constexpr std::uint32_t kSurfaceLost = 0x887601C2u;     // DDERR_SURFACELOST
constexpr std::uint32_t kNotLocked = 0x88760248u;       // DDERR_NOTLOCKED
constexpr std::uint32_t kUnsupported = 0x80004001u;     // DDERR_UNSUPPORTED = E_NOTIMPL
constexpr std::uint32_t kGeneric = 0x80004005u;         // DDERR_GENERIC = E_FAIL
constexpr std::uint32_t kNoInterface = 0x80004002u;     // E_NOINTERFACE
constexpr std::uint32_t kInvalidCaps = 0x88760064u;     // DDERR_INVALIDCAPS
constexpr std::uint32_t kNotFlippable = 0x88760246u;    // DDERR_NOTFLIPPABLE

// 显示模式：内部默认 32bpp（plan D4）；16bpp 请求 ⇒ 565
constexpr int kDefaultWidth = 1280;
constexpr int kDefaultHeight = 720;
constexpr unsigned kDefaultBpp = 32;
// 虚拟显存（确定性常量；GetAvailableVidMem 用它算 total/free）
constexpr std::uint32_t kTotalVideoMemory = 0x10000000u;  // 256 MiB
constexpr std::uint32_t kTotalSystemMemory = 0x20000000u; // 512 MiB
constexpr unsigned kRefreshRate = 60;

// DDraw surface 的 GetDC 返回的是**真正的 GDI DC 句柄**（由 P2 的 gdiCreateDc 分配；
// 见 ddraw_surface.cpp 的 getDC —— 那里同时 fontAttachDcSurface 把它绑到 surface 像素）。

// 判定色键是否命中（DDCOLORKEY 是闭区间 [low, high]；引擎把 RGB 先按掩码转成目标格式）
inline bool colorKeyHit(std::uint32_t ck_flags, std::uint32_t low, std::uint32_t high,
                        std::uint32_t pixel) {
    if ((ck_flags & kCkSrcBlt) == 0) return false;
    return pixel >= low && pixel <= high;
}

// 一帧的矩形（left/top/right/bottom；右/下**开区间**，与 RECT 一致）
struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool empty() const { return right <= left || bottom <= top; }
};

// 我们保存的 surface 状态（从 guest 状态块读出来的**快照**）
struct SurfaceInfo {
    std::uint32_t object = 0;   // COM 对象 EA（0 = 还没绑）
    std::uint32_t state = 0;    // 状态块 EA
    std::uint32_t arena = 0;    // SurfaceArena 里的像素块 EA
    int width = 0;
    int height = 0;
    std::uint32_t pitch = 0;
    SurfaceFormat format;
    std::uint32_t caps = 0;
    std::uint32_t ck_flags = 0;
    std::uint32_t ck_low = 0;
    std::uint32_t ck_high = 0;
    std::uint32_t hdc = 0;
    std::uint32_t dc_count = 0;
    std::uint32_t lock_ptr = 0;  // 最近一次 Lock 返回的 lpSurface（Unlock 校验用）
    bool primary = false;
};

// surface DC 的像素视图（P1→P2 的接缝：字体层据此把字画进同一块 guest 像素）
struct DcInfo {
    std::uint32_t surface_object = 0;
    std::uint32_t pixels_ea = 0;  // 像素起点（guest EA）
    std::uint32_t pitch = 0;
    int width = 0;
    int height = 0;
    SurfaceFormat format;
};

// ---- 设备状态（每台 Machine 一份；固定 guest 页）----
// 取设备状态块 EA（首次调用时提交固定页并初始化默认显示模式）；0 = 失败（why 有点名原因）。
std::uint32_t deviceState(X86& cpu, std::string* why);
// 当前显示模式（宽 / 高 / bpp / 像素格式）
bool displayMode(X86& cpu, int* width, int* height, unsigned* bpp, SurfaceFormat* fmt);
// 设置显示模式（只接受 16 / 32bpp；调用方负责把不支持的 bpp 变成 DDERR_UNSUPPORTED）
bool setDisplayMode(X86& cpu, int width, int height, unsigned bpp, std::string* why);
void setCooperativeLevel(X86& cpu, std::uint32_t hwnd, std::uint32_t flags);
std::uint32_t primarySurface(X86& cpu);  // 0 = 还没有 primary

// ---- surface 状态块 ----
// 分配像素 + 状态块；返回状态块 EA（0 = 失败）。primary 会登记进设备状态。
// `bpp_override`：0 = 用当前显示格式；16/32 = 用例/调用方指定的像素格式（其它值 ⇒ 失败）。
std::uint32_t createSurfaceState(X86& cpu, int width, int height, std::uint32_t caps, bool primary,
                                 unsigned bpp_override, std::string* why);
// 从对象 EA / 状态块 EA 读快照；失败（不是我们的 surface / 状态块坏了）⇒ false + why。
bool surfaceInfo(X86& cpu, std::uint32_t object, SurfaceInfo* out, std::string* why);
bool surfaceInfoOfState(X86& cpu, std::uint32_t state, SurfaceInfo* out, std::string* why);
std::uint32_t stateOfObject(X86& cpu, std::uint32_t object);  // 0 = 不是我们的对象
void bindSurfaceObject(X86& cpu, std::uint32_t state, std::uint32_t object);
// 释放状态块 + 像素（对象本身由 ComRegistry::release 释放）；primary 登记一并清掉。
void destroySurfaceState(X86& cpu, std::uint32_t state);

// ---- DDSURFACEDESC ----
// 把 surface 的信息写进 guest 的 DDSURFACEDESC（108 字节；lpSurface = 0 或当前锁指针）。
bool fillSurfaceDesc(X86& cpu, std::uint32_t desc_ea, const SurfaceInfo& s, bool include_surface,
                     std::string* why);
// 把显示模式写进 guest 的 DDSURFACEDESC（EnumDisplayModes / GetDisplayMode 用）
bool fillModeDesc(X86& cpu, std::uint32_t desc_ea, int width, int height, unsigned bpp,
                  const SurfaceFormat& fmt, std::uint32_t flags, std::string* why);

// ---- 像素路径 ----
// Blt：返回 HRESULT。调用方先校验 flag / DDBLTFX（不认识的要 loud trap）。
std::uint32_t blt(X86& cpu, const SurfaceInfo& dst, const Rect* dst_rect, const SurfaceInfo& src,
                  const Rect* src_rect, std::uint32_t flags, std::string* why);
// BltFast：目标起点 (x,y)（有符号；越界由本函数夹紧）
std::uint32_t bltFast(X86& cpu, const SurfaceInfo& dst, int x, int y, const SurfaceInfo& src,
                      const Rect* src_rect, std::uint32_t flags, std::string* why);

// ---- 锁 / DC ----
std::uint32_t lock(X86& cpu, const SurfaceInfo& s, const Rect* rect, std::uint32_t desc_ea,
                   std::uint32_t flags, std::uint32_t event, std::string* why);
std::uint32_t unlock(X86& cpu, const SurfaceInfo& s, std::uint32_t lp_surface, std::string* why);
std::uint32_t getDC(X86& cpu, const SurfaceInfo& s, std::uint32_t* out_hdc, std::string* why);
std::uint32_t releaseDC(X86& cpu, std::uint32_t hdc, std::string* why);
std::uint32_t surfaceLockCount(X86& cpu, const SurfaceInfo& s);

// ---- 呈现 ----
// 把 primary 交给 HeadlessPresenter（frameCount++ / lastHash 更新）；返回 HRESULT。
std::uint32_t flip(X86& cpu, const SurfaceInfo& s, std::uint32_t back_surface, std::uint32_t flags,
                   std::string* why);

// ---- GetCaps / SetColorKey ----
std::uint32_t getCaps(X86& cpu, const SurfaceInfo& s, std::uint32_t caps_ea, std::string* why);
std::uint32_t setColorKey(X86& cpu, const SurfaceInfo& s, std::uint32_t flags, std::uint32_t key_ea,
                          std::string* why);

// ---- GetAvailableVidMem ----
std::uint32_t availableVidMem(X86& cpu, std::uint32_t caps_ea, std::uint32_t total_ea,
                              std::uint32_t free_ea, std::string* why);

// ---- P1→P2 的 DC 接缝 ----
// 给定一个 HDC：是 surface DC ⇒ 填 out 并返回 true。
bool dcInfo(X86& cpu, std::uint32_t hdc, DcInfo* out);
// ★ `GetDC` / `ReleaseDC` 直接与字体层对接（runtime/font.h）：
//   GetDC 用 `gdiCreateDc` 建一个**真正的 GDI DC**，再 `fontAttachDcSurface(hdc, surface_ea, arena)`
//   把它的目标绑到这块 surface 像素；ReleaseDC 里 `fontDetachDc(hdc)` + 回收句柄。
//   ⇒ P2 的 `TextOutA` 拿到的 HDC 既能查到 DC 状态，也能查到 surface 像素（同一块 guest 内存）。

// 像素读写（宿主侧直接改 guest 像素；bpp 只支持 16 / 32）
std::uint32_t loadPixel(const std::uint8_t* p, unsigned bpp);
void storePixel(std::uint8_t* p, unsigned bpp, std::uint32_t v);
// 给定 bpp 的标准格式（32 = BGRA8888；16 = 565）
SurfaceFormat formatForBpp(unsigned bpp);

}  // namespace ddraw
}  // namespace lift
