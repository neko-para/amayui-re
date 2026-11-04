// emulator-lift · DirectDraw / DirectDrawSurface 的 COM 方法 shim（P1 / M2 · B2）
//
// ★ 入口是**动态导出**：`LoadLibraryA("DDRAW.DLL")` → `GetProcAddress("DirectDrawCreate")`
//   （`.lst` 0x43AE32..0x43AE79 取证）。G1 的转发表是**通用的**（module!name → ShimFn，
//   见 shims.h / kernel32_process.cpp 的 GetProcAddress）——本文件只是往那张表里登记一个名字，
//   ⛔ 没有给 DDraw 开特例。
//
// ★ 方法表（偏移**逐条**对过 Windows SDK `ddraw.h` 的接口声明顺序；`.call` 站点的 EA 见每条注释）：
//   IDirectDraw2（SDK：IUnknown 3 + IDirectDraw 20 + GetAvailableVidMem）
//     +0  QueryInterface        0x43C413（QI 成功后 0x43C45A Release 原对象）
//     +4  AddRef
//     +8  Release               0x43C44C / 0x43C45F
//     +24 CreateSurface         0x43C9FC / 0x440630
//     +32 EnumDisplayModes      0x43C482（dwFlags=1 = DDEDM_REFRESHRATES，回调 = 0x43BEA0）
//     +48 GetDisplayMode        0x43AF9F / 0x440AF2 / 0x440794
//     +80 SetCooperativeLevel   0x44050B（错误串「制御レベルの設定に失敗」）
//     +84 SetDisplayMode        0x44057B（**5 个参数**：w/h/bpp/refresh/flags —— SDK 的
//                               IDirectDraw2 正是 5 参数版，⛔ 不是 IDirectDraw 的 3 参数版）
//     +92 GetAvailableVidMem    0x43AF78 / 0x440A9C（3 个出参）
//   IDirectDrawSurface3（SDK 顺序；GetCaps 在 Surface3 里是**1 个参数**）
//     +0/4/8   IUnknown         0x43CA70（QI 到 IID_IDirectDrawSurface3 后 0x43CAC7 Release 原对象）
//     +20  Blt                  0x43D7A7（5 参数；flags 0x8000 = DDBLT_KEYSRC / 0 = 直拷）
//     +28  BltFast              0x43DA58
//     +44  Flip                 0x43E9A6（2 参数；0x43E95B 先 push 1 = DDFLIP_WAIT）
//     +56  GetCaps              0x43CB10（1 参数 —— SDK 的 Surface3::GetCaps(LPDDSCAPS)）
//     +68  GetDC                0x43B478（错误串「GetDC エラー」）
//     +88  GetSurfaceDesc       0x43B304（ddSetRBGINFO；desc+0x58/0x5C/0x60 = R/G/B 掩码）
//     +100 Lock                 0x43CC6C（4 参数：rect/desc/DDLOCK_WAIT/event）
//     +104 ReleaseDC            0x43B4DA
//     +116 SetColorKey          0x43B0C3（flags = 8 = DDCKEY_SRCBLT；0x43B070 的调用点）
//     +128 Unlock               0x43C304（1 参数 lpSurface）
//
// ★ 没填的槽 = P0 的 `comUnsupported`（loud trap：iface + 偏移 + 寄存器后停机）。
// ★ 未实现的 flag / DDBLTFX / back buffer = 本文件的 `ddrawTrap`（同样停机，⛔ 不静默）。
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "runtime/com.h"
#include "runtime/ddraw_surface.h"
#include "runtime/exec.h"
#include "runtime/guest_call.h"
#include "runtime/machine.h"
#include "runtime/memory.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// ---- IID（16 字节；与 AGE.EXE 里的常量逐字节一致，EA 见注释）----
// SDK ddraw.h：IID_IDirectDraw2 = {B3A6F3E0-2B43-11CF-A2DE-00AA00B93356}
constexpr std::uint8_t kIidDirectDraw2[16] = {0xE0, 0xF3, 0xA6, 0xB3, 0x43, 0x2B, 0xCF, 0x11,
                                              0xA2, 0xDE, 0x00, 0xAA, 0x00, 0xB9, 0x33, 0x56};
// SDK ddraw.h：IID_IDirectDrawSurface3 = {DA044E00-69B2-11D0-A1D5-00AA00B8DFBB}
constexpr std::uint8_t kIidDirectDrawSurface3[16] = {0x00, 0x4E, 0x04, 0xDA, 0xB2, 0x69, 0xD0, 0x11,
                                                     0xA1, 0xD5, 0x00, 0xAA, 0x00, 0xB8, 0xDF, 0xBB};
// IID_IUnknown = {00000000-0000-0000-C000-000000000046}
constexpr std::uint8_t kIidUnknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

// ---- EnumDisplayModes 的固定模式表（虚拟显示器；确定性顺序）----
// ★ 只列 16 / 32bpp：plan D4 明确不做 24bpp；引擎 0x440BD3 的"24 不可用 ⇒ 退 32"分支因此可走。
struct ModeEntry {
    int width;
    int height;
    unsigned bpp;
};
constexpr ModeEntry kModes[] = {
    {640, 480, 16},   {640, 480, 32},   {800, 600, 16},   {800, 600, 32},
    {1024, 768, 16},  {1024, 768, 32},  {1152, 864, 16},  {1152, 864, 32},
    {1280, 720, 16},  {1280, 720, 32},  {1280, 800, 16},  {1280, 800, 32},
    {1280, 960, 16},  {1280, 960, 32},  {1280, 1024, 16}, {1280, 1024, 32},
    {1366, 768, 16},  {1366, 768, 32},  {1440, 900, 16},  {1440, 900, 32},
    {1600, 900, 16},  {1600, 900, 32},  {1600, 1200, 16}, {1600, 1200, 32},
    {1680, 1050, 16}, {1680, 1050, 32}, {1920, 1080, 16}, {1920, 1080, 32},
    {1920, 1200, 16}, {1920, 1200, 32},
};
constexpr std::size_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);
constexpr std::uint32_t kDdedmRefreshRates = 0x00000001u;

// DDSURFACEDESC2 的 dwSize（引擎只用 DDSURFACEDESC 的 108；124 也接受以便诊断）
constexpr std::uint32_t kDescBytes2 = 124;

void ddrawLog(const char* method, const char* fmt, ...) {
    std::fprintf(stderr, "[ddraw] %s: ", method);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// ★ loud trap：未实现的 flag / 结构体 / 方法形态 ⇒ 打印现场并停机器（⛔ 不静默返回 0）。
void ddrawTrap(X86& cpu, const char* method, const char* fmt, ...) {
    std::fprintf(stderr, "\n=== unimplemented DDraw call ===\n  %s\n  reason: ", method);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n  （⛔ 不静默返回 0；见 docs/plan-m2-2d-text.md R7）\n");
    dumpRegisters(cpu, stderr);
    dumpGuestStack(cpu, stderr, 24);
    cpu.running = false;
    cpu.stop_why = "unimplemented DDraw call";
    ++cpu.shim.unimplemented_calls;
}

bool sameGuid(X86& cpu, std::uint32_t ea, const std::uint8_t (&want)[16]) {
    if (ea == 0 || !cpu.mem->committed(ea, 16)) return false;
    std::uint8_t got[16];
    cpu.mem->readBytes(ea, got, sizeof(got));
    for (int i = 0; i < 16; ++i)
        if (got[i] != want[i]) return false;
    return true;
}

std::uint32_t argU32(X86& cpu, ShimCall& call, int i) {
    (void)cpu;
    return call.u32(i);
}

// ---------------------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------------------
void shimQueryInterface(X86& cpu, ShimCall& call, bool surface_face) {
    const char* method = surface_face ? "IDirectDrawSurface3::QueryInterface"
                                      : "IDirectDraw2::QueryInterface";
    const std::uint32_t self = argU32(cpu, call, 0);
    const std::uint32_t riid = argU32(cpu, call, 1);
    const std::uint32_t ppv = argU32(cpu, call, 2);
    if (ppv == 0 || !cpu.mem->committed(ppv, 4)) {
        call.result = ddraw::kInvalidParams;
        ddrawLog(method, "ppvObj 为 NULL / 未提交 ⇒ DDERR_INVALIDPARAMS");
        return;
    }
    cpu.mem->wr32(ppv, 0);
    if (riid == 0 || !cpu.mem->committed(riid, 16)) {
        call.result = ddraw::kInvalidParams;
        ddrawLog(method, "riid 为 NULL / 未提交 ⇒ DDERR_INVALIDPARAMS");
        return;
    }
    const bool wanted_face = surface_face ? sameGuid(cpu, riid, kIidDirectDrawSurface3)
                                          : sameGuid(cpu, riid, kIidDirectDraw2);
    const bool wanted_unknown = sameGuid(cpu, riid, kIidUnknown);
    if (!wanted_face && !wanted_unknown) {
        std::uint8_t got[16];
        cpu.mem->readBytes(riid, got, sizeof(got));
        ddrawLog(method,
                 "riid={%08X-%04X-%04X-...} 不是本对象的接口（surface_face=%d）⇒ E_NOINTERFACE",
                 static_cast<unsigned>(got[0] | (got[1] << 8) | (got[2] << 16) |
                                       (static_cast<unsigned>(got[3]) << 24)),
                 static_cast<unsigned>(got[4] | (got[5] << 8)),
                 static_cast<unsigned>(got[6] | (got[7] << 8)), surface_face ? 1 : 0);
        call.result = ddraw::kNoInterface;
        return;
    }
    if (!cpu.com || !cpu.com->addRef(cpu, self)) {
        call.result = ddraw::kInvalidObject;
        ddrawLog(method, "this=0x%08X 不是 ComRegistry 建出来的对象 ⇒ DDERR_INVALIDOBJECT", self);
        return;
    }
    cpu.mem->wr32(ppv, self);
    call.result = ddraw::kOk;
}

void shimQueryInterfaceDevice(X86& cpu, ShimCall& call) { shimQueryInterface(cpu, call, false); }
void shimQueryInterfaceSurface(X86& cpu, ShimCall& call) { shimQueryInterface(cpu, call, true); }

void shimAddRef(X86& cpu, ShimCall& call) {
    const std::uint32_t self = argU32(cpu, call, 0);
    if (!cpu.com || !cpu.com->addRef(cpu, self)) {
        call.result = 0;
        return;
    }
    call.result = cpu.com->refCount(self);
}

// 释放 + 顺手回收 DDraw 的 surface 状态（对象到 0 时才做）
void shimRelease(X86& cpu, ShimCall& call) {
    const std::uint32_t self = argU32(cpu, call, 0);
    if (!cpu.com || !cpu.com->find(self)) {
        ddrawLog("Release", "this=0x%08X 不是本 ComRegistry 的对象（重复 Release / 野指针）⇒ 返回 0", self);
        call.result = 0;
        return;
    }
    const std::uint32_t user = cpu.com->userOf(self);
    const bool last = cpu.com->refCount(self) == 1u;
    if (!cpu.com->release(cpu, self)) {
        call.result = 0;
        return;
    }
    if (last && user != 0 && cpu.mem->committed(user, 4)) {
        // 设备状态块（'DDV1'）保留到机器结束；surface 状态块（'DDS1'）跟着对象回收
        if (cpu.mem->rd32(user) == 0x31534444u) ddraw::destroySurfaceState(cpu, user);
    }
    call.result = last ? 0u : cpu.com->refCount(self);
}

// ---------------------------------------------------------------------------------------
// IDirectDraw2
// ---------------------------------------------------------------------------------------
// 方法表（定义在文件末尾；CreateSurface / DirectDrawCreate 要拿它建对象）
const std::vector<ComMethod>& deviceMethods();
const std::vector<ComMethod>& surfaceMethods();

void shimDirectDrawCreate(X86& cpu, ShimCall& call);  // 动态导出（文件末尾）

void shimCreateSurface(X86& cpu, ShimCall& call);

void shimEnumDisplayModes(X86& cpu, ShimCall& call);

void shimGetDisplayMode(X86& cpu, ShimCall& call) {
    const std::uint32_t self = argU32(cpu, call, 0);
    const std::uint32_t desc = argU32(cpu, call, 1);
    (void)self;
    int w = 0;
    int h = 0;
    unsigned bpp = 0;
    SurfaceFormat fmt;
    if (!ddraw::displayMode(cpu, &w, &h, &bpp, &fmt)) {
        ddrawTrap(cpu, "DirectDraw2::GetDisplayMode(+48)", "设备状态页读不出来");
        call.result = ddraw::kGeneric;
        return;
    }
    std::string why;
    if (!ddraw::fillModeDesc(cpu, desc, w, h, bpp, fmt, 0, &why)) {
        ddrawTrap(cpu, "DirectDraw2::GetDisplayMode(+48)", "%s", why.c_str());
        call.result = ddraw::kInvalidParams;
        return;
    }
    call.result = ddraw::kOk;
}

void shimSetCooperativeLevel(X86& cpu, ShimCall& call) {
    const std::uint32_t self = argU32(cpu, call, 0);
    const std::uint32_t hwnd = argU32(cpu, call, 1);
    const std::uint32_t flags = argU32(cpu, call, 2);
    (void)self;
    ddraw::setCooperativeLevel(cpu, hwnd, flags);
    call.result = ddraw::kOk;
}

void shimSetDisplayMode(X86& cpu, ShimCall& call) {
    const std::uint32_t self = argU32(cpu, call, 0);
    const std::uint32_t width = argU32(cpu, call, 1);
    const std::uint32_t height = argU32(cpu, call, 2);
    const std::uint32_t bpp = argU32(cpu, call, 3);
    const std::uint32_t refresh = argU32(cpu, call, 4);
    const std::uint32_t flags = argU32(cpu, call, 5);
    (void)self;
    if (bpp != 16 && bpp != 32) {
        ddrawLog("DirectDraw2::SetDisplayMode(+84)",
                 "请求 %ux%ux%u：本模拟器只做 16 / 32bpp（plan D4 明确不做 8/24）⇒ DDERR_UNSUPPORTED",
                 width, height, bpp);
        call.result = ddraw::kUnsupported;
        return;
    }
    if (refresh != 0 || flags != 0) {
        ddrawLog("DirectDraw2::SetDisplayMode(+84)", "refresh=%u flags=0x%08X 不建模（虚拟固定 60Hz）",
                 refresh, flags);
    }
    std::string why;
    if (!ddraw::setDisplayMode(cpu, static_cast<int>(width), static_cast<int>(height), bpp, &why)) {
        ddrawLog("DirectDraw2::SetDisplayMode(+84)", "%s", why.c_str());
        call.result = ddraw::kInvalidParams;
        return;
    }
    call.result = ddraw::kOk;
}

void shimGetAvailableVidMem(X86& cpu, ShimCall& call) {
    const std::uint32_t self = argU32(cpu, call, 0);
    const std::uint32_t caps = argU32(cpu, call, 1);
    const std::uint32_t total = argU32(cpu, call, 2);
    const std::uint32_t free_ea = argU32(cpu, call, 3);
    (void)self;
    std::string why;
    const std::uint32_t hr = ddraw::availableVidMem(cpu, caps, total, free_ea, &why);
    if (hr != ddraw::kOk) {
        ddrawLog("DirectDraw2::GetAvailableVidMem(+92)", "%s", why.c_str());
    }
    call.result = hr;
}

// ---------------------------------------------------------------------------------------
// IDirectDrawSurface3
// ---------------------------------------------------------------------------------------
bool surfaceOf(X86& cpu, ShimCall& call, const char* method, ddraw::SurfaceInfo* out) {
    std::string why;
    if (!ddraw::surfaceInfo(cpu, argU32(cpu, call, 0), out, &why)) {
        ddrawTrap(cpu, method, "%s", why.c_str());
        return false;
    }
    return true;
}

void shimSurfaceBlt(X86& cpu, ShimCall& call) {
    const char* method = "IDirectDrawSurface3::Blt(+20)";
    const std::uint32_t self = argU32(cpu, call, 0);
    const std::uint32_t dst_rect = argU32(cpu, call, 1);
    const std::uint32_t src_obj = argU32(cpu, call, 2);
    const std::uint32_t src_rect = argU32(cpu, call, 3);
    const std::uint32_t flags = argU32(cpu, call, 4);
    const std::uint32_t fx = argU32(cpu, call, 5);
    // 支持的 flag：KEYSRC（0x8000，引擎 0x43D768 用到）+ WAIT + ASYNC
    const std::uint32_t allowed = ddraw::kBltKeySrc | ddraw::kBltWait | ddraw::kBltAsync;
    if ((flags & ~allowed) != 0) {
        ddrawTrap(cpu, method,
                  "dwFlags=0x%08X 含未实现的位（只支持 DDBLT_KEYSRC/WAIT/ASYNC）；"
                  "dstRect=0x%08X src=0x%08X srcRect=0x%08X",
                  flags, dst_rect, src_obj, src_rect);
        call.result = ddraw::kUnsupported;
        return;
    }
    if (fx != 0) {
        ddrawTrap(cpu, method, "lpDDBltFx=0x%08X（dwRop / bkColor / ddckSrcBlt 等未实现）", fx);
        call.result = ddraw::kUnsupported;
        return;
    }
    ddraw::SurfaceInfo dst;
    ddraw::SurfaceInfo src;
    if (!surfaceOf(cpu, call, method, &dst)) {
        call.result = ddraw::kInvalidObject;
        return;
    }
    std::string why;
    if (!ddraw::surfaceInfo(cpu, src_obj, &src, &why)) {
        ddrawTrap(cpu, method, "源 surface %s", why.c_str());
        call.result = ddraw::kInvalidObject;
        return;
    }
    if (dst.format.bpp != src.format.bpp) {
        ddrawTrap(cpu, method, "源/目标 bpp 不同（%u vs %u）：跨格式 Blt 未实现", src.format.bpp,
                  dst.format.bpp);
        call.result = ddraw::kUnsupported;
        return;
    }
    ddraw::Rect dr;
    ddraw::Rect sr;
    const ddraw::Rect* drp = nullptr;
    const ddraw::Rect* srp = nullptr;
    if (dst_rect != 0) {
        if (!cpu.mem->committed(dst_rect, 16)) {
            ddrawTrap(cpu, method, "lpDestRect=0x%08X 未提交", dst_rect);
            call.result = ddraw::kInvalidParams;
            return;
        }
        dr.left = static_cast<int>(cpu.mem->rd32(dst_rect));
        dr.top = static_cast<int>(cpu.mem->rd32(dst_rect + 4));
        dr.right = static_cast<int>(cpu.mem->rd32(dst_rect + 8));
        dr.bottom = static_cast<int>(cpu.mem->rd32(dst_rect + 12));
        drp = &dr;
    }
    if (src_rect != 0) {
        if (!cpu.mem->committed(src_rect, 16)) {
            ddrawTrap(cpu, method, "lpSrcRect=0x%08X 未提交", src_rect);
            call.result = ddraw::kInvalidParams;
            return;
        }
        sr.left = static_cast<int>(cpu.mem->rd32(src_rect));
        sr.top = static_cast<int>(cpu.mem->rd32(src_rect + 4));
        sr.right = static_cast<int>(cpu.mem->rd32(src_rect + 8));
        sr.bottom = static_cast<int>(cpu.mem->rd32(src_rect + 12));
        srp = &sr;
    }
    const std::uint32_t hr = ddraw::blt(cpu, dst, drp, src, srp, flags, &why);
    if (hr != ddraw::kOk) ddrawLog(method, "%s", why.c_str());
    call.result = hr;
    (void)self;
}

void shimSurfaceBltFast(X86& cpu, ShimCall& call) {
    const char* method = "IDirectDrawSurface3::BltFast(+28)";
    const std::uint32_t self = argU32(cpu, call, 0);
    const std::uint32_t x = argU32(cpu, call, 1);
    const std::uint32_t y = argU32(cpu, call, 2);
    const std::uint32_t src_obj = argU32(cpu, call, 3);
    const std::uint32_t src_rect = argU32(cpu, call, 4);
    const std::uint32_t flags = argU32(cpu, call, 5);
    (void)self;
    const std::uint32_t allowed =
        ddraw::kBltFastSrcColorKey | ddraw::kBltFastDestColorKey | ddraw::kBltFastWait;
    if ((flags & ~allowed) != 0) {
        ddrawTrap(cpu, method, "dwFlags=0x%08X 含未实现的位（SRCCOLORKEY/DESTCOLORKEY/WAIT）", flags);
        call.result = ddraw::kUnsupported;
        return;
    }
    if ((flags & ddraw::kBltFastDestColorKey) != 0) {
        ddrawTrap(cpu, method, "DDBLTFAST_DESTCOLORKEY（目标色键）未实现");
        call.result = ddraw::kUnsupported;
        return;
    }
    ddraw::SurfaceInfo dst;
    ddraw::SurfaceInfo src;
    if (!surfaceOf(cpu, call, method, &dst)) {
        call.result = ddraw::kInvalidObject;
        return;
    }
    std::string why;
    if (!ddraw::surfaceInfo(cpu, src_obj, &src, &why)) {
        ddrawTrap(cpu, method, "源 surface %s", why.c_str());
        call.result = ddraw::kInvalidObject;
        return;
    }
    if (dst.format.bpp != src.format.bpp) {
        ddrawTrap(cpu, method, "源/目标 bpp 不同（%u vs %u）", src.format.bpp, dst.format.bpp);
        call.result = ddraw::kUnsupported;
        return;
    }
    ddraw::Rect sr;
    const ddraw::Rect* srp = nullptr;
    if (src_rect != 0) {
        if (!cpu.mem->committed(src_rect, 16)) {
            ddrawTrap(cpu, method, "lpSrcRect=0x%08X 未提交", src_rect);
            call.result = ddraw::kInvalidParams;
            return;
        }
        sr.left = static_cast<int>(cpu.mem->rd32(src_rect));
        sr.top = static_cast<int>(cpu.mem->rd32(src_rect + 4));
        sr.right = static_cast<int>(cpu.mem->rd32(src_rect + 8));
        sr.bottom = static_cast<int>(cpu.mem->rd32(src_rect + 12));
        srp = &sr;
    }
    const std::uint32_t hr = ddraw::bltFast(cpu, dst, static_cast<int>(x), static_cast<int>(y), src, srp,
                                            flags, &why);
    if (hr != ddraw::kOk) ddrawLog(method, "%s", why.c_str());
    call.result = hr;
}

void shimSurfaceFlip(X86& cpu, ShimCall& call) {
    const char* method = "IDirectDrawSurface3::Flip(+44)";
    const std::uint32_t back = argU32(cpu, call, 1);
    const std::uint32_t flags = argU32(cpu, call, 2);
    ddraw::SurfaceInfo s;
    if (!surfaceOf(cpu, call, method, &s)) {
        call.result = ddraw::kInvalidObject;
        return;
    }
    const std::uint32_t allowed = ddraw::kFlipWait | ddraw::kFlipNoVsync;
    if ((flags & ~allowed) != 0) {
        ddrawTrap(cpu, method, "dwFlags=0x%08X 含未实现的位（只支持 DDFLIP_WAIT/NOVSYNC）", flags);
        call.result = ddraw::kUnsupported;
        return;
    }
    std::string why;
    const std::uint32_t hr = ddraw::flip(cpu, s, back, flags, &why);
    if (hr != ddraw::kOk) ddrawLog(method, "%s", why.c_str());
    call.result = hr;
}

void shimSurfaceGetCaps(X86& cpu, ShimCall& call) {
    const char* method = "IDirectDrawSurface3::GetCaps(+56)";
    ddraw::SurfaceInfo s;
    if (!surfaceOf(cpu, call, method, &s)) {
        call.result = ddraw::kInvalidObject;
        return;
    }
    std::string why;
    const std::uint32_t hr = ddraw::getCaps(cpu, s, argU32(cpu, call, 1), &why);
    if (hr != ddraw::kOk) ddrawLog(method, "%s", why.c_str());
    call.result = hr;
}

void shimSurfaceGetDC(X86& cpu, ShimCall& call) {
    const char* method = "IDirectDrawSurface3::GetDC(+68)";
    ddraw::SurfaceInfo s;
    if (!surfaceOf(cpu, call, method, &s)) {
        call.result = ddraw::kInvalidObject;
        return;
    }
    const std::uint32_t out = argU32(cpu, call, 1);
    if (out == 0 || !cpu.mem->committed(out, 4)) {
        ddrawTrap(cpu, method, "lphDC=0x%08X 为 NULL / 未提交", out);
        call.result = ddraw::kInvalidParams;
        return;
    }
    std::string why;
    std::uint32_t hdc = 0;
    const std::uint32_t hr = ddraw::getDC(cpu, s, &hdc, &why);
    if (hr == ddraw::kOk) cpu.mem->wr32(out, hdc);
    else ddrawLog(method, "%s", why.c_str());
    call.result = hr;
}

void shimSurfaceReleaseDC(X86& cpu, ShimCall& call) {
    const char* method = "IDirectDrawSurface3::ReleaseDC(+104)";
    (void)argU32(cpu, call, 0);  // this（按 HDC 反查即可；真机也不看 this 与 HDC 是否配对）
    std::string why;
    const std::uint32_t hr = ddraw::releaseDC(cpu, argU32(cpu, call, 1), &why);
    if (hr != ddraw::kOk) ddrawLog(method, "%s", why.c_str());
    call.result = hr;
}

void shimSurfaceGetSurfaceDesc(X86& cpu, ShimCall& call) {
    const char* method = "IDirectDrawSurface3::GetSurfaceDesc(+88)";
    ddraw::SurfaceInfo s;
    if (!surfaceOf(cpu, call, method, &s)) {
        call.result = ddraw::kInvalidObject;
        return;
    }
    const std::uint32_t desc = argU32(cpu, call, 1);
    std::string why;
    // 真机：lpSurface 只在锁着的时候有效 ⇒ 只有 lock_ptr != 0 才回填
    if (!ddraw::fillSurfaceDesc(cpu, desc, s, s.lock_ptr != 0, &why)) {
        ddrawTrap(cpu, method, "%s", why.c_str());
        call.result = ddraw::kInvalidParams;
        return;
    }
    call.result = ddraw::kOk;
}

void shimSurfaceLock(X86& cpu, ShimCall& call) {
    const char* method = "IDirectDrawSurface3::Lock(+100)";
    const std::uint32_t rect = argU32(cpu, call, 1);
    const std::uint32_t desc = argU32(cpu, call, 2);
    const std::uint32_t flags = argU32(cpu, call, 3);
    const std::uint32_t event = argU32(cpu, call, 4);
    ddraw::SurfaceInfo s;
    if (!surfaceOf(cpu, call, method, &s)) {
        call.result = ddraw::kInvalidObject;
        return;
    }
    if (desc != 0 && cpu.mem->committed(desc, 4)) {
        const std::uint32_t size = cpu.mem->rd32(desc);
        if (size != ddraw::kDescBytes && size != kDescBytes2) {
            ddrawTrap(cpu, method, "lpDDSurfaceDesc->dwSize=%u（只接受 108 / 124）", size);
            call.result = ddraw::kInvalidParams;
            return;
        }
    }
    ddraw::Rect r;
    const ddraw::Rect* rp = nullptr;
    if (rect != 0) {
        if (!cpu.mem->committed(rect, 16)) {
            ddrawTrap(cpu, method, "lpDestRect=0x%08X 未提交", rect);
            call.result = ddraw::kInvalidParams;
            return;
        }
        r.left = static_cast<int>(cpu.mem->rd32(rect));
        r.top = static_cast<int>(cpu.mem->rd32(rect + 4));
        r.right = static_cast<int>(cpu.mem->rd32(rect + 8));
        r.bottom = static_cast<int>(cpu.mem->rd32(rect + 12));
        rp = &r;
    }
    std::string why;
    const std::uint32_t hr = ddraw::lock(cpu, s, rp, desc, flags, event, &why);
    if (hr != ddraw::kOk) ddrawLog(method, "%s", why.c_str());
    call.result = hr;
}

void shimSurfaceUnlock(X86& cpu, ShimCall& call) {
    const char* method = "IDirectDrawSurface3::Unlock(+128)";
    ddraw::SurfaceInfo s;
    if (!surfaceOf(cpu, call, method, &s)) {
        call.result = ddraw::kInvalidObject;
        return;
    }
    std::string why;
    const std::uint32_t hr = ddraw::unlock(cpu, s, argU32(cpu, call, 1), &why);
    if (hr != ddraw::kOk) ddrawLog(method, "%s", why.c_str());
    call.result = hr;
}

void shimSurfaceSetColorKey(X86& cpu, ShimCall& call) {
    const char* method = "IDirectDrawSurface3::SetColorKey(+116)";
    ddraw::SurfaceInfo s;
    if (!surfaceOf(cpu, call, method, &s)) {
        call.result = ddraw::kInvalidObject;
        return;
    }
    const std::uint32_t flags = argU32(cpu, call, 1);
    const std::uint32_t key = argU32(cpu, call, 2);
    if ((flags & ~(ddraw::kCkSrcBlt | ddraw::kCkDestBlt)) != 0) {
        ddrawTrap(cpu, method, "dwFlags=0x%08X（只支持 DDCKEY_SRCBLT/DESTBLT）", flags);
        call.result = ddraw::kUnsupported;
        return;
    }
    std::string why;
    const std::uint32_t hr = ddraw::setColorKey(cpu, s, flags, key, &why);
    if (hr != ddraw::kOk) ddrawLog(method, "%s", why.c_str());
    call.result = hr;
}

// ---------------------------------------------------------------------------------------
// CreateSurface / EnumDisplayModes
// ---------------------------------------------------------------------------------------
void shimCreateSurface(X86& cpu, ShimCall& call) {
    const char* method = "IDirectDraw2::CreateSurface(+24)";
    const std::uint32_t self = argU32(cpu, call, 0);
    const std::uint32_t desc = argU32(cpu, call, 1);
    const std::uint32_t out = argU32(cpu, call, 2);
    const std::uint32_t outer = argU32(cpu, call, 3);
    (void)self;
    if (outer != 0) {
        ddrawTrap(cpu, method, "pUnkOuter=0x%08X（聚合未实现）", outer);
        call.result = ddraw::kInvalidParams;
        return;
    }
    if (desc == 0 || !cpu.mem->committed(desc, ddraw::kDescBytes)) {
        ddrawTrap(cpu, method, "lpDDSurfaceDesc=0x%08X 为 NULL / 未提交", desc);
        call.result = ddraw::kInvalidParams;
        return;
    }
    if (out == 0 || !cpu.mem->committed(out, 4)) {
        ddrawTrap(cpu, method, "lplpDDSurface=0x%08X 为 NULL / 未提交", out);
        call.result = ddraw::kInvalidParams;
        return;
    }
    const std::uint32_t size = cpu.mem->rd32(desc + ddraw::kOffSize);
    if (size != ddraw::kDescBytes && size != kDescBytes2) {
        ddrawTrap(cpu, method, "dwSize=%u（只接受 108 / 124）", size);
        call.result = ddraw::kInvalidParams;
        return;
    }
    const std::uint32_t flags = cpu.mem->rd32(desc + ddraw::kOffFlags);
    const std::uint32_t width = cpu.mem->rd32(desc + ddraw::kOffWidth);
    const std::uint32_t height = cpu.mem->rd32(desc + ddraw::kOffHeight);
    const std::uint32_t caps = cpu.mem->rd32(desc + ddraw::kOffCaps);
    const std::uint32_t back_buffers = cpu.mem->rd32(desc + ddraw::kOffBackBufferCount);
    if ((flags & (ddraw::kDdsdCaps | ddraw::kDdsdWidth | ddraw::kDdsdHeight)) !=
        (ddraw::kDdsdCaps | ddraw::kDdsdWidth | ddraw::kDdsdHeight)) {
        ddrawTrap(cpu, method, "dwFlags=0x%08X 缺 CAPS/WIDTH/HEIGHT（引擎只用 7）", flags);
        call.result = ddraw::kInvalidParams;
        return;
    }
    if (width == 0 || height == 0 || width > 16384 || height > 16384) {
        ddrawTrap(cpu, method, "尺寸 %ux%u 非法", width, height);
        call.result = ddraw::kInvalidParams;
        return;
    }
    std::uint32_t want_bpp = 0;
    if ((flags & ddraw::kDdsdPixelFormat) != 0) {
        want_bpp = cpu.mem->rd32(desc + ddraw::kOffPixelFormat + ddraw::kPfBitCount);
        const std::uint32_t r = cpu.mem->rd32(desc + ddraw::kOffPixelFormat + ddraw::kPfRMask);
        const std::uint32_t g = cpu.mem->rd32(desc + ddraw::kOffPixelFormat + ddraw::kPfGMask);
        const std::uint32_t b = cpu.mem->rd32(desc + ddraw::kOffPixelFormat + ddraw::kPfBMask);
        ddrawLog(method, "desc 指定 PIXELFORMAT：bpp=%u masks=%08X/%08X/%08X（本实现按 bpp 用标准掩码）",
                 want_bpp, r, g, b);
    }
    if ((flags & ddraw::kDdsdBackBufferCount) != 0 && back_buffers != 0) {
        // 偏差（SCOPE）：不建真正的 back buffer / 不交换前后台；Flip(NULL) 只呈现 primary。
        ddrawLog(method,
                 "dwBackBufferCount=%u 未建模（只建前台；GetAttachedSurface 未实现 ⇒ 调用会 loud trap）",
                 back_buffers);
    }
    const bool primary = (caps & ddraw::kCapsPrimarySurface) != 0;
    std::string why;
    const std::uint32_t state =
        ddraw::createSurfaceState(cpu, static_cast<int>(width), static_cast<int>(height), caps, primary,
                                  want_bpp, &why);
    if (state == 0) {
        if (why.find("already exists") != std::string::npos) {
            call.result = 0x88760234u;  // DDERR_PRIMARYSURFACEALREADYEXISTS = MAKE_DDHRESULT(564)
        } else if (why.find("尺寸非法") != std::string::npos) {
            call.result = ddraw::kInvalidParams;
        } else {
            call.result = ddraw::kGeneric;
        }
        ddrawLog(method, "%s", why.c_str());
        return;
    }
    std::uint32_t obj = 0;
    if (!cpu.com) {
        ddraw::destroySurfaceState(cpu, state);
        ddrawTrap(cpu, method, "cpu.com 为空（Machine 没装配 ComRegistry）");
        call.result = ddraw::kGeneric;
        return;
    }
    obj = cpu.com->createObject(cpu, "IDirectDrawSurface3", surfaceMethods(), state);
    if (obj == 0) {
        ddraw::destroySurfaceState(cpu, state);
        ddrawTrap(cpu, method, "在 guest 内存里建 IDirectDrawSurface3 对象失败（见上一行 com 日志）");
        call.result = ddraw::kGeneric;
        return;
    }
    ddraw::bindSurfaceObject(cpu, state, obj);
    cpu.mem->wr32(out, obj);
    ddrawLog(method, "建 surface：%ux%u caps=0x%08X ⇒ 对象 0x%08X（状态块 0x%08X）%s", width, height, caps,
             obj, state, primary ? " [primary]" : "");
    call.result = ddraw::kOk;
}

// EnumDisplayModes 的续延状态
struct EnumCtx {
    std::uint32_t desc = 0;
    std::uint32_t context = 0;
    std::uint32_t callback = 0;
    std::size_t index = 0;  // 当前**已经回调过**的模式下标
};

void shimEnumDisplayModes(X86& cpu, ShimCall& call) {
    const char* method = "IDirectDraw2::EnumDisplayModes(+32)";
    const std::uint32_t self = argU32(cpu, call, 0);
    const std::uint32_t flags = argU32(cpu, call, 1);
    const std::uint32_t desc = argU32(cpu, call, 2);
    const std::uint32_t context = argU32(cpu, call, 3);
    const std::uint32_t callback = argU32(cpu, call, 4);
    (void)self;
    if ((flags & ~kDdedmRefreshRates) != 0) {
        ddrawTrap(cpu, method, "dwFlags=0x%08X（只支持 DDEDM_REFRESHRATES=1）", flags);
        call.result = ddraw::kUnsupported;
        return;
    }
    if (callback == 0) {
        call.result = ddraw::kInvalidParams;
        ddrawLog(method, "lpEnumModesCallback 为 NULL ⇒ DDERR_INVALIDPARAMS");
        return;
    }
    if (!cpu.calls) {
        ddrawTrap(cpu, method, "cpu.calls 为空（Machine 没装配 GuestCallStack）");
        call.result = ddraw::kGeneric;
        return;
    }
    if (!cpu.mem->committed(callback, 1)) {
        ddrawTrap(cpu, method, "回调地址 0x%08X 未提交", callback);
        call.result = ddraw::kInvalidParams;
        return;
    }
    // 回调的 desc 放在 guest 堆上（枚举结束回收）
    const std::uint32_t scratch = cpu.heap ? cpu.heap->alloc(ddraw::kDescBytes, /*zero=*/true) : 0;
    if (scratch == 0) {
        ddrawTrap(cpu, method, "guest 堆分配 DDSURFACEDESC 失败");
        call.result = ddraw::kGeneric;
        return;
    }
    auto ctx = std::make_shared<EnumCtx>();
    ctx->desc = scratch;
    ctx->context = context;
    ctx->callback = callback;

    // ★ 续延在 shim 返回**之后**才跑 ⇒ 只能捕获 guest 对象的**指针值**（X86 活得比它久），
    //   ⛔ 不要捕获 shim 的栈引用。
    X86* const cp = &cpu;
    const std::uint32_t cleanup = 4u + 4u * static_cast<std::uint32_t>(call.argc);
    auto fill = [cp](const std::shared_ptr<EnumCtx>& c, std::size_t i) {
        const ModeEntry& m = kModes[i];
        std::string why;
        if (!ddraw::fillModeDesc(*cp, c->desc, m.width, m.height, m.bpp, ddraw::formatForBpp(m.bpp), 0,
                                 &why)) {
            ddrawLog("EnumDisplayModes", "写模式 desc 失败：%s", why.c_str());
        }
    };
    fill(ctx, 0);

    // ★ 语义：DDENUMRET_OK（1）⇒ 继续；DDENUMRET_CANCEL（0）⇒ 停止。
    //   取证：引擎的回调 0x43BEA0 → 0x43AFE0 在所有返回路径都是 `mov eax,1; retn 8`
    //   （0x43B024 / 0x43B061），即**永远返回 1 = 继续**；这个方向与 SDK ddraw.h 的
    //   DDENUMRET_* 一致。⛔ 任务书里"回调返回非 0 要提前终止"与此相反 —— 见交付说明。
    auto cont = [cp, fill, cleanup](std::uint32_t result, std::uint32_t* shim_result,
                                    const std::shared_ptr<EnumCtx>& c) -> bool {
        if (result == 0) {
            ddrawLog("EnumDisplayModes", "guest 回调返回 DDENUMRET_CANCEL(0) ⇒ 提前终止（已回调 %zu 条）",
                     c->index + 1);
            if (cp->heap) cp->heap->free(c->desc);
            *shim_result = ddraw::kOk;
            return true;
        }
        ++c->index;
        if (c->index >= kModeCount) {
            ddrawLog("EnumDisplayModes", "枚举完成：%zu 条模式（回调全部返回继续）", kModeCount);
            if (cp->heap) cp->heap->free(c->desc);
            *shim_result = ddraw::kOk;
            return true;
        }
        fill(c, c->index);
        std::string err;
        if (!cp->calls->start(*cp, c->callback, {c->desc, c->context}, cleanup,
                              "IDirectDraw2::EnumDisplayModes", nullptr, &err)) {
            ddrawLog("EnumDisplayModes", "铺下一个回调帧失败：%s", err.c_str());
            if (cp->heap) cp->heap->free(c->desc);
            *shim_result = ddraw::kGeneric;
            return true;
        }
        return false;  // 又交给 guest；resume() 会在下一次回调返回后继续进本续延
    };

    call.take_control = true;  // 控制权交给 guest 回调
    std::string err;
    const bool ok = cp->calls->start(
        *cp, callback, {scratch, context}, cleanup, "IDirectDraw2::EnumDisplayModes",
        [cont, ctx](X86&, std::uint32_t r, std::uint32_t* out) { return cont(r, out, ctx); }, &err);
    if (!ok) {
        ddrawLog(method, "铺回调帧失败：%s", err.c_str());
        if (cp->heap) cp->heap->free(scratch);
        call.take_control = false;
        call.result = ddraw::kGeneric;
        return;
    }
    call.result = ddraw::kOk;  // 真正的返回值由续延在收尾时写进 EAX
}

// ---------------------------------------------------------------------------------------
// DirectDrawCreate（动态导出）
// ---------------------------------------------------------------------------------------
void shimDirectDrawCreate(X86& cpu, ShimCall& call) {
    const std::uint32_t guid = argU32(cpu, call, 0);
    const std::uint32_t out = argU32(cpu, call, 1);
    const std::uint32_t outer = argU32(cpu, call, 2);
    if (outer != 0) {
        ddrawLog("DirectDrawCreate", "pUnkOuter=0x%08X 非 NULL（聚合）⇒ DDERR_INVALIDPARAMS", outer);
        call.result = ddraw::kInvalidParams;
        return;
    }
    if (out == 0 || !cpu.mem->committed(out, 4)) {
        ddrawLog("DirectDrawCreate", "lplpDD=0x%08X 为 NULL / 未提交 ⇒ DDERR_INVALIDPARAMS", out);
        call.result = ddraw::kInvalidParams;
        return;
    }
    cpu.mem->wr32(out, 0);
    // 引擎传 lpGUID ∈ {0, 2}（.lst 0x43C396：`and eax,1; add eax,eax`）——
    // 2 = DDCREATE_HARDWAREONLY，1 = DDCREATE_EMULATIONONLY（都是"特殊指针值"，⛔ 不解引用）。
    if (guid != 0 && guid != 1 && guid != 2) {
        if (!cpu.mem->committed(guid, 16)) {
            ddrawLog("DirectDrawCreate", "lpGUID=0x%08X 既不是 0/1/2 也不是可读的 GUID ⇒ 拒绝", guid);
            call.result = 0x88760233u;  // DDERR_NODIRECTDRAWHW = MAKE_DDHRESULT(563)
            return;
        }
        ddrawLog("DirectDrawCreate", "lpGUID=0x%08X 指向一个具体设备 GUID：本模拟器只有一台虚拟显示设备 ⇒ 按默认设备处理",
                 guid);
    }
    std::string why;
    const std::uint32_t dev = ddraw::deviceState(cpu, &why);
    if (dev == 0 || !cpu.com) {
        ddrawLog("DirectDrawCreate", "%s", dev == 0 ? why.c_str() : "cpu.com 为空");
        call.result = ddraw::kGeneric;
        return;
    }
    const std::uint32_t obj = cpu.com->createObject(cpu, "IDirectDraw2", deviceMethods(), dev);
    if (obj == 0) {
        ddrawTrap(cpu, "DirectDrawCreate", "在 guest 内存里建 IDirectDraw2 对象失败（见上一行 com 日志）");
        call.result = ddraw::kGeneric;
        return;
    }
    cpu.mem->wr32(out, obj);
    ddrawLog("DirectDrawCreate", "lpGUID=0x%08X ⇒ IDirectDraw2 对象 0x%08X（设备状态 0x%08X）", guid, obj, dev);
    call.result = ddraw::kOk;
}

// ---------------------------------------------------------------------------------------
// 方法表
// ---------------------------------------------------------------------------------------
const std::vector<ComMethod>& deviceMethods() {
    static const std::vector<ComMethod> methods = {
        {0u, "QueryInterface", Conv::Stdcall, 2, &shimQueryInterfaceDevice},
        {4u, "AddRef", Conv::Stdcall, 0, &shimAddRef},
        {8u, "Release", Conv::Stdcall, 0, &shimRelease},
        {24u, "CreateSurface", Conv::Stdcall, 3, &shimCreateSurface},
        {32u, "EnumDisplayModes", Conv::Stdcall, 4, &shimEnumDisplayModes},
        {48u, "GetDisplayMode", Conv::Stdcall, 1, &shimGetDisplayMode},
        {80u, "SetCooperativeLevel", Conv::Stdcall, 2, &shimSetCooperativeLevel},
        {84u, "SetDisplayMode", Conv::Stdcall, 5, &shimSetDisplayMode},
        {92u, "GetAvailableVidMem", Conv::Stdcall, 3, &shimGetAvailableVidMem},
    };
    return methods;
}

const std::vector<ComMethod>& surfaceMethods() {
    static const std::vector<ComMethod> methods = {
        {0u, "QueryInterface", Conv::Stdcall, 2, &shimQueryInterfaceSurface},
        {4u, "AddRef", Conv::Stdcall, 0, &shimAddRef},
        {8u, "Release", Conv::Stdcall, 0, &shimRelease},
        {20u, "Blt", Conv::Stdcall, 5, &shimSurfaceBlt},
        {28u, "BltFast", Conv::Stdcall, 5, &shimSurfaceBltFast},
        {44u, "Flip", Conv::Stdcall, 2, &shimSurfaceFlip},
        {56u, "GetCaps", Conv::Stdcall, 1, &shimSurfaceGetCaps},
        {68u, "GetDC", Conv::Stdcall, 1, &shimSurfaceGetDC},
        {88u, "GetSurfaceDesc", Conv::Stdcall, 1, &shimSurfaceGetSurfaceDesc},
        {100u, "Lock", Conv::Stdcall, 4, &shimSurfaceLock},
        {104u, "ReleaseDC", Conv::Stdcall, 1, &shimSurfaceReleaseDC},
        {116u, "SetColorKey", Conv::Stdcall, 2, &shimSurfaceSetColorKey},
        {128u, "Unlock", Conv::Stdcall, 1, &shimSurfaceUnlock},
    };
    return methods;
}

}  // namespace

void registerDdrawShims(ShimRegistry& shims) {
    // ★ 动态导出（G1 的通用表）：不是 PE 导入 ⇒ 只能用 `add`（bind() 时分配唯一桩地址），
    //   `GetProcAddress("DDRAW.DLL", "DirectDrawCreate")` 从 shim 注册表查到它。
    shims.add("ddraw", "DirectDrawCreate", Conv::Stdcall, 3, &shimDirectDrawCreate);
}

}  // namespace lift
