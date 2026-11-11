// emulator-lift · D3DX9 的 Sprite 导出 + COM 方法 shim（M2 · 第一帧）
//
// ★ 入口是**静态导入**：`d3dx9_43!D3DXCreateSprite`（`.lst` 0x4A2BD0 `call j_D3DXCreateSprite`，
//   返回地址 0x4A2BD5，调用点 sub_4A2BA0）。它由 `registerD3DX9Shims` 登记进来（IAT 桩页），
//   ⛔ 不是动态导出表。
//
// ★ 槽表 = `runtime/d3dx9_sprite.h` 文件头那张（取证 EA 全在那里；这里只写偏移 / 方法名 / argc）。
//   已取证的 7 个槽：+0x08 Release / +0x14 SetTransform / +0x20 Begin / +0x24 Draw / +0x2C End /
//   +0x30 OnLostDevice / +0x34 OnResetDevice；未取证的 4 个（+0x0C GetDevice / +0x10 GetTransform /
//   +0x28 Flush 与两个 SetWorldView）按 SDK 同一份 13 槽布局补 —— **推断**，不是实测。
//
// ★ 未实现的槽（SetWorldViewRH +0x18 / SetWorldViewLH +0x1C）= `comUnsupported` 的 loud trap
//   （打印 iface + 偏移 + 寄存器后停机器）。⛔ 绝不静默返回 0。
// ★ `Draw` 的纹理：`pTexture` 必须是本模块建的 `IDirect3DTexture9`（`D3DXCreateTexture` 建的，
//   像素在 guest SurfaceArena）；`pTexture=NULL` 走 D3D9 固定功能"没有绑定纹理 ⇒ 采样白色"的语义
//   （几何尺寸来自 pSrcRect）。不是我们的纹理对象 / 两个指针都 NULL ⇒ loud trap（⛔ 不猜像素）。
//   `Draw` 会对纹理 AddRef（除非 Begin 给了 D3DXSPRITE_DO_NOT_ADDREF_TEXTURE），End/Flush 对称 Release。
// ★ 本文件还承载 M2 纹理簇的另外几条 d3dx9 导出：D3DXCreateTexture +
//   D3DXMatrixMultiply/Scaling/Translation/RotationAxis/RotationYawPitchRoll/OrthoLH/
//   PerspectiveFovLH/LookAtLH + D3DXVec4Transform/Vec3Normalize（公式见各自函数头）。
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "runtime/com.h"
#include "runtime/log.h"
#include "runtime/d3d9.h"
#include "runtime/d3dx9_sprite.h"
#include "runtime/exec.h"
#include "runtime/math.h"
#include "runtime/memory.h"
#include "runtime/png_decode.h"
#include "runtime/x86.h"
#include "shims/d3dx9_surface.h"
#include "shims/register.h"

namespace lift {
namespace {

// IID_ID3DXSprite = {BA0B762D-7D28-43EC-B9DC-2F84443B0614}
// ★ 来源 = SDK d3dx9core.h 的 DEFINE_GUID（宿主 `C:\msys64\ucrt64\include\d3dx9core.h:51` 的字节级对照）。
//   本段 boot 引擎**不做** QI（0x4A2BD5 直接用返回的指针）⇒ 这份 GUID 无法用 `.lst` 交叉验证；
//   夹具用同一份常量做正反两向 QI（本 IID / IUnknown 成功、随便一个 ⇒ E_NOINTERFACE）。
constexpr std::uint8_t kIidID3DXSprite[16] = {0x2D, 0x76, 0x0B, 0xBA, 0x28, 0x7D, 0xEC, 0x43,
                                              0xB9, 0xDC, 0x2F, 0x84, 0x44, 0x3B, 0x06, 0x14};
// IID_IUnknown = {00000000-0000-0000-C000-000000000046}
constexpr std::uint8_t kIidUnknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

void d3dxWarn(const char* method, const char* fmt, ...) {
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "[d3dx9] %s: ", method ? method : "?");
    va_list ap;
    va_start(ap, fmt);
    logV(LogLevel::Warn, LogD3dx9, prefix, fmt, ap);
    va_end(ap);
}

void d3dxLog(const char* method, const char* fmt, ...) {
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "[d3dx9] %s: ", method ? method : "?");
    va_list ap;
    va_start(ap, fmt);
    logV(LogLevel::Info, LogD3dx9, prefix, fmt, ap);
    va_end(ap);
}

// 诊断：把 "w×h / D3DFORMAT / 预计像素字节" 拼成一句（所有建纹理失败路径共用）。
// ★ 字节数用与 `SurfaceArena::create` **同一条**公式（`(w*bpp+3)&~3` 后乘 h），
//   这样 trap / OOM 日志里的数字能与 `surface.cpp` / `heap.cpp` 的输出直接对账。
//   格式不在支持集 ⇒ bpp 未知，只报 w×h / format（⛔ 不猜字节数）。
std::string textureShapeNote(std::uint32_t width, std::uint32_t height, std::uint32_t format) {
    char buf[192];
    SurfaceFormat fmt;
    if (d3d9::surfaceFormatForD3DFormat(format, &fmt)) {
        const std::uint64_t row = (static_cast<std::uint64_t>(width) * fmt.bytesPerPixel() + 3u) & ~3ull;
        const std::uint64_t bytes = row * static_cast<std::uint64_t>(height);
        std::snprintf(buf, sizeof(buf), "%ux%u D3DFORMAT=%u（%ubpp，像素 %llu 字节）", width, height, format,
                      fmt.bpp, static_cast<unsigned long long>(bytes));
    } else {
        std::snprintf(buf, sizeof(buf), "%ux%u D3DFORMAT=%u（格式不在支持集 ⇒ 像素字节未知）", width, height,
                      format);
    }
    return std::string(buf);
}

// loud trap：参数形态 / 状态崩坏 ⇒ 打印现场并停机器（与 d3d9 的 d3d9Trap 同形）
void d3dxTrap(X86& cpu, const char* method, const char* fmt, ...) {
    std::fprintf(stderr, "\n=== unimplemented D3DX9 call ===\n  %s\n  reason: ", method);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n  （⛔ 不静默返回 0；槽级缺口由 comUnsupported 报 iface+偏移）\n");
    dumpRegisters(cpu, stderr);
    dumpGuestStack(cpu, stderr, 24);
    cpu.running = false;
    cpu.stop_why = "unimplemented D3DX9 call";
    // ★ 停机种类（纯宿主）：已注册的导出 / 槽走到 loud trap ⇒ 非正常结局（`StopReason::Trap`）。
    cpu.stop_kind = HostStopKind::Trap;
    ++cpu.shim.unimplemented_calls;
}

// 资源耗尽（**不是** unimplemented）：我们这边的 arena / guest 堆不够。
// ★ 为什么单独一个函数而不是复用 d3dxTrap：误诊链就在这里 —— 同一句
//   "=== unimplemented D3DX9 call ===" 会让"容量/碎片问题"看起来像"guest 用了没实现的导出"，
//   并把 `unimplemented_calls` 的账做脏。这里：独立标签 + 独立 `stop_why` + **不计数**。
// ★ 为什么仍然是硬停（不返回真机的 E_OUTOFMEMORY 继续跑）——实测（2026-11-08，见需求单 repro）：
//   返回 E_OUTOFMEMORY 后 guest 确实走失败分支（0x48AD0C `test eax,eax / jnz`），但随后在
//   `0x4E8AB6` 处 `throw`（RaiseException 参数里是 C++ 异常码 0xE06D7363）⇒ 撞上**未实现**
//   `kernel32!RaiseException`（SEH 展开不在本线）⇒ 停在一个更假的 gap 上、进程 exit 2。
//   ⇒ 保留硬停：至少停因是真的（我们没内存），而不是"没实现 RaiseException"。
void d3dxResourceStop(X86& cpu, const char* method, const char* fmt, ...) {
    std::fprintf(stderr, "\n=== D3DX 资源耗尽（不是 unimplemented）===\n  %s\n  reason: ", method);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr,
                 "\n  （真机语义 = E_OUTOFMEMORY；本线在此硬停：guest 的失败分支会 throw C++ 异常"
                 "（0xE06D7363）⇒ 撞上未实现的 kernel32!RaiseException）\n");
    dumpRegisters(cpu, stderr);
    dumpGuestStack(cpu, stderr, 24);
    cpu.running = false;
    cpu.stop_why = "D3DX 资源耗尽";
    // ★ 停机种类（纯宿主）：容量/碎片问题，⛔ 与上一族（`Trap`）合并 —— 修法完全不同。
    cpu.stop_kind = HostStopKind::ResourceExhausted;
    // ⛔ 不 ++cpu.shim.unimplemented_calls：这不是缺口。
}

bool sameGuid(X86& cpu, std::uint32_t ea, const std::uint8_t (&want)[16]) {
    if (ea == 0 || !cpu.mem->committed(ea, 16)) return false;
    std::uint8_t got[16];
    cpu.mem->readBytes(ea, got, sizeof(got));
    for (int i = 0; i < 16; ++i)
        if (got[i] != want[i]) return false;
    return true;
}

// ---------------------------------------------------------------------------------------
// 从 `this` 取 Sprite 状态块；不是我们的对象 ⇒ loud trap（返回 0，调用方直接 return）
// ---------------------------------------------------------------------------------------
std::uint32_t requireSprite(X86& cpu, ShimCall& call, const char* method) {
    const std::uint32_t self = call.u32(0);
    if (!cpu.com || !cpu.com->find(self)) {
        d3dxTrap(cpu, method, "this=0x%08X 不是本模块建的 ID3DXSprite 对象", self);
        return 0;
    }
    const std::uint32_t state = cpu.com->userOf(self);
    if (state == 0) {
        d3dxTrap(cpu, method, "ID3DXSprite 对象 0x%08X 的 obj+8 没有状态块", self);
        return 0;
    }
    return state;
}

// ---------------------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------------------
void shimSpriteQueryInterface(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t riid = call.u32(1);
    const std::uint32_t ppv = call.u32(2);
    if (ppv == 0 || !cpu.mem->committed(ppv, 4)) {
        d3dxLog("ID3DXSprite::QueryInterface", "ppvObj 为 NULL / 未提交 ⇒ E_INVALIDARG");
        call.result = d3dx9::kInvalidArg;
        return;
    }
    cpu.mem->wr32(ppv, 0);
    if (riid == 0 || !cpu.mem->committed(riid, 16)) {
        d3dxLog("ID3DXSprite::QueryInterface", "riid 为 NULL / 未提交 ⇒ E_INVALIDARG");
        call.result = d3dx9::kInvalidArg;
        return;
    }
    const bool wanted = sameGuid(cpu, riid, kIidID3DXSprite) || sameGuid(cpu, riid, kIidUnknown);
    if (!wanted) {
        std::uint8_t got[16];
        cpu.mem->readBytes(riid, got, sizeof(got));
        d3dxLog("ID3DXSprite::QueryInterface",
                "riid={%08X-%04X-%04X-...} 不是 ID3DXSprite 的接口 ⇒ E_NOINTERFACE",
                static_cast<unsigned>(got[0] | (got[1] << 8) | (got[2] << 16) |
                                      (static_cast<unsigned>(got[3]) << 24)),
                static_cast<unsigned>(got[4] | (got[5] << 8)),
                static_cast<unsigned>(got[6] | (got[7] << 8)));
        call.result = d3dx9::kNoInterface;
        return;
    }
    if (!cpu.com || !cpu.com->addRef(cpu, self)) {
        d3dxLog("ID3DXSprite::QueryInterface", "this=0x%08X 不是 ComRegistry 建出来的对象", self);
        call.result = d3dx9::kNoInterface;
        return;
    }
    cpu.mem->wr32(ppv, self);
    call.result = d3dx9::kOk;
}

void shimSpriteAddRef(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (!cpu.com || !cpu.com->addRef(cpu, self)) {
        call.result = 0;
        return;
    }
    call.result = cpu.com->refCount(self);
}

// Release：到 0 时顺手回收 Sprite 状态块 + 批次数组（对象本身由 ComRegistry 释放）
void shimSpriteRelease(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (!cpu.com || !cpu.com->find(self)) {
        d3dxLog("ID3DXSprite::Release", "this=0x%08X 不是本 ComRegistry 的对象（重复 Release / 野指针）⇒ 返回 0",
                self);
        call.result = 0;
        return;
    }
    const std::uint32_t user = cpu.com->userOf(self);
    const bool last = cpu.com->refCount(self) == 1u;
    if (!cpu.com->release(cpu, self)) {
        call.result = 0;
        return;
    }
    if (last && user != 0) d3dx9::destroySpriteState(cpu, user);
    call.result = last ? 0u : cpu.com->refCount(self);
}

// ---------------------------------------------------------------------------------------
// ID3DXSprite（槽表见 runtime/d3dx9_sprite.h 文件头；+0x18 / +0x1C 故意不填 ⇒ comUnsupported）
// ---------------------------------------------------------------------------------------
void shimSpriteGetDevice(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireSprite(cpu, call, "ID3DXSprite::GetDevice(+0x0C)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    const std::uint32_t out_ea = call.u32(1);
    if (out_ea == 0 || !cpu.mem->committed(out_ea, 4)) {
        d3dxTrap(cpu, "ID3DXSprite::GetDevice(+0x0C)", "ppDevice=0x%08X 为 NULL / 未提交", out_ea);
        call.result = d3dx9::kInvalidArg;
        return;
    }
    // ★ GetDevice **不** AddRef（D3DX 文档：返回的指针由 sprite 自己持有；调用方不要 Release）
    const std::uint32_t dev = d3dx9::spriteDevice(cpu, state);
    cpu.mem->wr32(out_ea, dev);
    call.result = d3dx9::kOk;
}

void shimSpriteGetTransform(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireSprite(cpu, call, "ID3DXSprite::GetTransform(+0x10)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3dx9::getTransform(cpu, state, call.u32(1), &why);
    if (hr != d3dx9::kOk) d3dxTrap(cpu, "ID3DXSprite::GetTransform(+0x10)", "%s", why.c_str());
    call.result = hr;
}

void shimSpriteSetTransform(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireSprite(cpu, call, "ID3DXSprite::SetTransform(+0x14)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3dx9::setTransform(cpu, state, call.u32(1), &why);
    if (hr != d3dx9::kOk) d3dxTrap(cpu, "ID3DXSprite::SetTransform(+0x14)", "%s", why.c_str());
    call.result = hr;
}

void shimSpriteBegin(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireSprite(cpu, call, "ID3DXSprite::Begin(+0x20)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3dx9::begin(cpu, state, call.u32(1), &why);
    if (hr != d3dx9::kOk) d3dxLog("ID3DXSprite::Begin", "失败：%s ⇒ 0x%08X", why.c_str(), hr);
    call.result = hr;
}

void shimSpriteDraw(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t state = requireSprite(cpu, call, "ID3DXSprite::Draw(+0x24)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    const std::uint32_t texture = call.u32(1);
    const std::uint32_t rect = call.u32(2);
    const std::uint32_t center = call.u32(3);
    const std::uint32_t position = call.u32(4);
    const std::uint32_t color = call.u32(5);
    // ★ 非 0 的 pTexture 必须是**本模块建的** IDirect3DTexture9（M2 纹理簇；
    //   `D3DXCreateTexture` / `D3DXCreateTextureFromFileInMemory` 建出来的对象）。
    //   不是的话 ⇒ loud trap（⛔ 不猜像素）。
    if (texture != 0) {
        d3d9::TextureInfo ti;
        std::string twhy;
        if (!d3d9::textureInfo(cpu, texture, &ti, &twhy)) {
            d3dxTrap(cpu, "ID3DXSprite::Draw(+0x24)",
                     "pTexture=0x%08X 不是本模块的 IDirect3DTexture9（%s）· pSrcRect=0x%08X pCenter=0x%08X "
                     "pPosition=0x%08X Color=0x%08X · this=0x%08X",
                     texture, twhy.c_str(), rect, center, position, color, self);
            call.result = d3dx9::kFail;
            return;
        }
    }
    if (texture == 0 && rect == 0) {
        d3dxTrap(cpu, "ID3DXSprite::Draw(+0x24)",
                 "pTexture=NULL 且 pSrcRect=NULL ⇒ 没有尺寸来源（NULL 纹理的 D3D9 语义是采样白色，"
                 "但四边形的大小只能来自 pSrcRect）· this=0x%08X Color=0x%08X",
                 self, color);
        call.result = d3dx9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3dx9::draw(cpu, state, texture, rect, center, position, color, &why);
    if (hr != d3dx9::kOk) d3dxLog("ID3DXSprite::Draw", "失败：%s ⇒ 0x%08X", why.c_str(), hr);
    call.result = hr;
}

void shimSpriteFlush(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireSprite(cpu, call, "ID3DXSprite::Flush(+0x28)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3dx9::flush(cpu, state, &why);
    if (hr != d3dx9::kOk) d3dxLog("ID3DXSprite::Flush", "失败：%s ⇒ 0x%08X", why.c_str(), hr);
    call.result = hr;
}

void shimSpriteEnd(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireSprite(cpu, call, "ID3DXSprite::End(+0x2C)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3dx9::end(cpu, state, &why);
    if (hr != d3dx9::kOk) d3dxLog("ID3DXSprite::End", "失败：%s ⇒ 0x%08X", why.c_str(), hr);
    call.result = hr;
}

void shimSpriteOnLostDevice(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireSprite(cpu, call, "ID3DXSprite::OnLostDevice(+0x30)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3dx9::onLostDevice(cpu, state, &why);
    if (hr != d3dx9::kOk) d3dxTrap(cpu, "ID3DXSprite::OnLostDevice(+0x30)", "%s", why.c_str());
    call.result = hr;
}

void shimSpriteOnResetDevice(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireSprite(cpu, call, "ID3DXSprite::OnResetDevice(+0x34)");
    if (state == 0) {
        call.result = d3dx9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3dx9::onResetDevice(cpu, state, &why);
    if (hr != d3dx9::kOk) d3dxTrap(cpu, "ID3DXSprite::OnResetDevice(+0x34)", "%s", why.c_str());
    call.result = hr;
}

// ---------------------------------------------------------------------------------------
// IDirect3DTexture9（M2 纹理簇；D3DXCreateTexture 建出来的对象）
//
// 槽序 = SDK d3d9.h 的 **22 槽**（IUnknown 3 + Resource 8 + BaseTexture 6 = 17 ⇒
//        GetLevelDesc 第 18 槽 +0x44 / GetSurfaceLevel +0x48 / LockRect +0x4C /
//        UnlockRect +0x50 / AddDirtyRect +0x54）。
// ★ 取证：`.lst` 0x48AD18 `mov edx,[eax+44h]` → 0x48AD22 call（this, Level=0, pDesc），
//   desc+0x18/+0x1C 被当宽/高读（0x48AD2A / 0x48AD2D）⇒ +0x44 = GetLevelDesc，与 SDK 逐槽一致。
// ★ 本线只实现**引擎会走到的**几个槽；其余（SetPrivateData / SetLOD / SetAutoGenFilterType /
//   GenerateMipSubLevels / AddDirtyRect …）= comUnsupported 的 loud trap。
// ★ GetSurfaceLevel(+0x48) 是 2026-11-05 boot 的新卡点（0x004A7755，ReadFrameTex 取渲染目标）。
//   surface 的状态直接复用纹理状态块（本线只有 level 0）⇒ 不另建一块 guest 状态。
// ---------------------------------------------------------------------------------------
constexpr std::uint8_t kIidDirect3DTexture9[16] = {0x27, 0x12, 0xC3, 0x85, 0xE5, 0x3D, 0x00, 0x4F,
                                                   0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5};
constexpr std::uint8_t kIidDirect3DSurface9[16] = {0x3A, 0xAF, 0xFB, 0x0C, 0xF6, 0x9F, 0x99, 0x42,
                                                   0x96, 0x74, 0x1D, 0x1A, 0x4A, 0x8F, 0x0F, 0xDD};

const std::vector<ComMethod>& textureMethods();
const std::vector<ComMethod>& surfaceMethods();

std::uint32_t requireTexture(X86& cpu, ShimCall& call, const char* method) {
    const std::uint32_t self = call.u32(0);
    if (!cpu.com || !cpu.com->find(self)) {
        d3dxTrap(cpu, method, "this=0x%08X 不是本模块建的 IDirect3DTexture9 对象", self);
        return 0;
    }
    const std::uint32_t state = cpu.com->userOf(self);
    if (state == 0) {
        d3dxTrap(cpu, method, "纹理对象 0x%08X 的 obj+8 没有状态块", self);
        return 0;
    }
    return state;
}

void shimTextureQueryInterface(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t riid = call.u32(1);
    const std::uint32_t ppv = call.u32(2);
    if (ppv == 0 || !cpu.mem->committed(ppv, 4)) {
        d3dxLog("IDirect3DTexture9::QueryInterface", "ppvObj 为 NULL / 未提交 ⇒ E_INVALIDARG");
        call.result = d3d9::kInvalidArg;
        return;
    }
    cpu.mem->wr32(ppv, 0);
    if (riid == 0 || !cpu.mem->committed(riid, 16)) {
        call.result = d3d9::kInvalidArg;
        return;
    }
    if (!sameGuid(cpu, riid, kIidDirect3DTexture9) && !sameGuid(cpu, riid, kIidUnknown)) {
        call.result = d3d9::kNoInterface;
        return;
    }
    if (!cpu.com || !cpu.com->addRef(cpu, self)) {
        call.result = d3d9::kNoInterface;
        return;
    }
    cpu.mem->wr32(ppv, self);
    call.result = d3d9::kOk;
}

void shimTextureAddRef(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (!cpu.com || !cpu.com->addRef(cpu, self)) {
        call.result = 0;
        return;
    }
    call.result = cpu.com->refCount(self);
}

void shimTextureRelease(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (!cpu.com || !cpu.com->find(self)) {
        d3dxLog("IDirect3DTexture9::Release", "this=0x%08X 不是本 ComRegistry 的对象（重复 Release / 野指针）⇒ 0",
                self);
        call.result = 0;
        return;
    }
    // ★ 状态块 + arena 像素块的回收**不在这里**：`ComRegistry::release` 到 0 时会跑对象的
    //   析构钩子（`createObject` 时登记 = `destroyTextureState`）。以前挂在这里 ⇒ 内部裸
    //   release 走到 0 时像素块永久留在 arena（OPTION 往返一次漏一屏的量）。
    const bool last = cpu.com->refCount(self) == 1u;
    if (!cpu.com->release(cpu, self)) {
        call.result = 0;
        return;
    }
    call.result = last ? 0u : cpu.com->refCount(self);
}

void shimTextureGetDevice(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireTexture(cpu, call, "IDirect3DTexture9::GetDevice(+0x0C)");
    if (state == 0) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9::TextureInfo ti;
    std::string why;
    if (!d3d9::textureInfoOfState(cpu, state, &ti, &why)) {
        d3dxTrap(cpu, "IDirect3DTexture9::GetDevice(+0x0C)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t out_ea = call.u32(1);
    if (out_ea == 0 || !cpu.mem->committed(out_ea, 4)) {
        d3dxTrap(cpu, "IDirect3DTexture9::GetDevice(+0x0C)", "ppDevice=0x%08X 为 NULL / 未提交", out_ea);
        call.result = d3d9::kInvalidArg;
        return;
    }
    // ★ `IDirect3DResource9::GetDevice` 的文档明确："Calling this method will increase the internal
    //   reference count on the IDirect3DDevice9 interface. Failure to call IUnknown::Release when
    //   finished ... results in a memory leak." ⇒ 借出的是一份**新引用**。
    //   · guest 不 Release ⇒ 设备对象永远到不了 0：`destroyDeviceState` 不跑（后缓冲 / 深度 /
    //     流水线块 + 绑定的纹理全漏，`caseTextureBoundThenGuestRelease` 那种按 arena 基线判的用例会红）；
    //   · guest 多 Release ⇒ 减掉的是设备自己那份（CreateDevice 给的），设备会被提前回收。
    //   ⛔ 顺序：先校验出参再 AddRef（绝不把引用加到"还没确认能给出去"的对象上）。
    if (ti.device != 0 && !cpu.com->addRef(cpu, ti.device)) {
        d3dxWarn("IDirect3DTexture9::GetDevice", "设备 0x%08X 借不出引用（已不是本模块的对象）", ti.device);
        call.result = d3d9::kInvalidCall;
        return;
    }
    cpu.mem->wr32(out_ea, ti.device);
    call.result = d3d9::kOk;
}

void shimTextureGetType(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireTexture(cpu, call, "IDirect3DTexture9::GetType(+0x28)");
    if (state == 0) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    call.result = d3d9::kRTypeTexture;
}

void shimTextureGetLevelCount(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireTexture(cpu, call, "IDirect3DTexture9::GetLevelCount(+0x34)");
    if (state == 0) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9::TextureInfo ti;
    std::string why;
    if (!d3d9::textureInfoOfState(cpu, state, &ti, &why)) {
        d3dxTrap(cpu, "IDirect3DTexture9::GetLevelCount(+0x34)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    call.result = ti.levels;  // 本线只建 level 0 ⇒ 1
}

void shimTextureGetLevelDesc(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireTexture(cpu, call, "IDirect3DTexture9::GetLevelDesc(+0x44)");
    if (state == 0) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9::TextureInfo ti;
    std::string why;
    if (!d3d9::textureInfoOfState(cpu, state, &ti, &why)) {
        d3dxTrap(cpu, "IDirect3DTexture9::GetLevelDesc(+0x44)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t level = call.u32(1);
    const std::uint32_t desc_ea = call.u32(2);
    if (!d3d9::writeTextureDesc(cpu, ti, level, desc_ea, &why)) {
        d3dxTrap(cpu, "IDirect3DTexture9::GetLevelDesc(+0x44)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    // ★ 取证用（Debug）：影片渲染器 `0x488A80` 用 `desc.Format` 判 21/26，否则 VFW_E_TYPE_NOT_ACCEPTED。
    d3dxLog("IDirect3DTexture9::GetLevelDesc", "texture=0x%08X level=%u ⇒ %ux%u D3DFORMAT=%u（desc=0x%08X）",
            call.u32(0), level, ti.width, ti.height, ti.format, desc_ea);
    call.result = d3d9::kOk;
}

void shimTextureLockRect(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireTexture(cpu, call, "IDirect3DTexture9::LockRect(+0x4C)");
    if (state == 0) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9::TextureInfo ti;
    std::string why;
    if (!d3d9::textureInfoOfState(cpu, state, &ti, &why)) {
        d3dxTrap(cpu, "IDirect3DTexture9::LockRect(+0x4C)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t level = call.u32(1);
    const std::uint32_t locked_ea = call.u32(2);
    const std::uint32_t rect_ea = call.u32(3);
    const std::uint32_t flags = call.u32(4);
    if (!d3d9::lockRect(cpu, ti, level, locked_ea, rect_ea, flags, &why)) {
        d3dxTrap(cpu, "IDirect3DTexture9::LockRect(+0x4C)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    call.result = d3d9::kOk;
}

void shimTextureUnlockRect(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireTexture(cpu, call, "IDirect3DTexture9::UnlockRect(+0x50)");
    if (state == 0) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9::TextureInfo ti;
    std::string why;
    if (!d3d9::textureInfoOfState(cpu, state, &ti, &why)) {
        d3dxTrap(cpu, "IDirect3DTexture9::UnlockRect(+0x50)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    if (!d3d9::unlockRect(cpu, ti, call.u32(1), &why)) {
        d3dxTrap(cpu, "IDirect3DTexture9::UnlockRect(+0x50)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    call.result = d3d9::kOk;
}

void shimTextureGetSurfaceLevel(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);  // 父纹理对象（surface 的像素块 owner）
    const std::uint32_t state =
        requireTexture(cpu, call, "IDirect3DTexture9::GetSurfaceLevel(+0x48)");
    if (state == 0) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t level = call.u32(1);
    const std::uint32_t out_ea = call.u32(2);
    if (level != 0) {
        d3dxTrap(cpu, "IDirect3DTexture9::GetSurfaceLevel(+0x48)",
                 "Level=%u（本线只有 level 0）⇒ D3DERR_INVALIDCALL", level);
        call.result = d3d9::kInvalidCall;
        return;
    }
    if (out_ea == 0 || !cpu.mem->committed(out_ea, 4)) {
        d3dxTrap(cpu, "IDirect3DTexture9::GetSurfaceLevel(+0x48)",
                 "ppSurfaceLevel=0x%08X 为 NULL / 未提交", out_ea);
        call.result = d3d9::kInvalidArg;
        return;
    }
    cpu.mem->wr32(out_ea, 0);
    if (!cpu.com) {
        d3dxTrap(cpu, "IDirect3DTexture9::GetSurfaceLevel(+0x48)", "cpu.com 为空（装配问题）");
        call.result = d3d9::kFail;
        return;
    }
    // ★ owner = 父纹理对象（`self`）：像素块 + 'TEX1' 状态块的寿命挂在纹理对象的 refcount 上。
    //   `createD3dxSurfaceObject` 会**替父纹理持一份引用**（A 方案 / REQ-01M6ZZZ6PEVSFB93C6VJPKC04C）
    //   ⇒ guest 先 Release 纹理、仍持这个 surface 时，状态块与像素块都还在（surface 到 0 才回收）。
    //   绑定为 RT 时 `SetRenderTarget` 还会**另**加一份（绑定期的语义引用，见 d3d9_pipeline.cpp）。
    const std::uint32_t surface = createD3dxSurfaceObject(cpu, state, self);
    if (surface == 0) {
        d3dxTrap(cpu, "IDirect3DTexture9::GetSurfaceLevel(+0x48)", "在 guest 内存里建 surface 对象失败");
        call.result = d3d9::kFail;
        return;
    }
    cpu.mem->wr32(out_ea, surface);
    d3dxLog("IDirect3DTexture9::GetSurfaceLevel", "Level=0 ⇒ IDirect3DSurface9 对象 0x%08X（复用纹理状态块 0x%08X）",
            surface, state);
    call.result = d3d9::kOk;
}

// ---------------------------------------------------------------------------------------
// IDirect3DSurface9（纹理表面；状态 = 纹理状态块，level 恒 0）
// ---------------------------------------------------------------------------------------
std::uint32_t requireSurfaceState(X86& cpu, ShimCall& call, const char* method) {
    const std::uint32_t self = call.u32(0);
    const char* iface = cpu.com ? cpu.com->ifaceOf(self) : nullptr;
    if (iface == nullptr || std::strcmp(iface, "IDirect3DSurface9") != 0) {
        d3dxTrap(cpu, method, "this=0x%08X 不是本模块建的 IDirect3DSurface9 对象", self);
        return 0;
    }
    const std::uint32_t state = cpu.com->userOf(self);
    if (state == 0) {
        d3dxTrap(cpu, method, "surface 对象 0x%08X 的 obj+8 没有纹理状态块", self);
        return 0;
    }
    return state;
}

void shimSurfaceQueryInterface(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t riid = call.u32(1);
    const std::uint32_t ppv = call.u32(2);
    if (ppv == 0 || !cpu.mem->committed(ppv, 4)) {
        call.result = d3d9::kInvalidArg;
        return;
    }
    cpu.mem->wr32(ppv, 0);
    if (riid == 0 || !cpu.mem->committed(riid, 16)) {
        call.result = d3d9::kInvalidArg;
        return;
    }
    if (!sameGuid(cpu, riid, kIidDirect3DSurface9) && !sameGuid(cpu, riid, kIidUnknown)) {
        call.result = d3d9::kNoInterface;
        return;
    }
    if (!cpu.com || !cpu.com->addRef(cpu, self)) {
        call.result = d3d9::kNoInterface;
        return;
    }
    cpu.mem->wr32(ppv, self);
    call.result = d3d9::kOk;
}

void shimSurfaceAddRef(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (!cpu.com || !cpu.com->addRef(cpu, self)) {
        call.result = 0;
        return;
    }
    call.result = cpu.com->refCount(self);
}

void shimSurfaceRelease(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (!cpu.com || !cpu.com->find(self)) {
        d3dxLog("IDirect3DSurface9::Release", "this=0x%08X 不是本模块的 COM 对象（重复 Release / 野指针）⇒ 0",
                self);
        call.result = 0;
        return;
    }
    const bool last = cpu.com->refCount(self) == 1u;
    if (!cpu.com->release(cpu, self)) {
        call.result = 0;
        return;
    }
    call.result = last ? 0u : cpu.com->refCount(self);
}

void shimSurfaceGetDevice(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireSurfaceState(cpu, call, "IDirect3DSurface9::GetDevice(+0x0C)");
    if (state == 0) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9::TextureInfo ti;
    std::string why;
    if (!d3d9::surfaceInfoOfState(cpu, state, &ti, &why)) {
        d3dxTrap(cpu, "IDirect3DSurface9::GetDevice(+0x0C)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t out_ea = call.u32(1);
    if (out_ea == 0 || !cpu.mem->committed(out_ea, 4)) {
        call.result = d3d9::kInvalidArg;
        return;
    }
    // ★ `IDirect3DSurface9` 继承 `IDirect3DResource9` ⇒ `GetDevice` 的文档与纹理 / VB / IB / 顶点声明
    //   是**同一条**："Calling this method will increase the internal reference count on the
    //   IDirect3DDevice9 interface. Failure to call IUnknown::Release ... results in a memory leak."
    //   ⇒ 借出的是一份**新引用**（与段 1 已统一的 `IDirect3DResource9::GetDevice` 家族一致）。
    //   ⛔ 顺序：先校验出参再 AddRef（绝不把引用加到"还没确认能给出去"的对象上）。
    if (ti.device != 0 && !cpu.com->addRef(cpu, ti.device)) {
        d3dxWarn("IDirect3DSurface9::GetDevice", "设备 0x%08X 借不出引用（已不是本模块的对象）", ti.device);
        call.result = d3d9::kInvalidCall;
        return;
    }
    cpu.mem->wr32(out_ea, ti.device);
    call.result = d3d9::kOk;
}

void shimSurfaceGetType(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireSurfaceState(cpu, call, "IDirect3DSurface9::GetType(+0x28)");
    if (state == 0) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    call.result = d3d9::kRTypeSurface;
}

void shimSurfaceGetDesc(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireSurfaceState(cpu, call, "IDirect3DSurface9::GetDesc(+0x30)");
    if (state == 0) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9::TextureInfo ti;
    std::string why;
    if (!d3d9::surfaceInfoOfState(cpu, state, &ti, &why)) {
        d3dxTrap(cpu, "IDirect3DSurface9::GetDesc(+0x30)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    if (!d3d9::writeTextureDesc(cpu, ti, 0, call.u32(1), &why)) {
        d3dxTrap(cpu, "IDirect3DSurface9::GetDesc(+0x30)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    call.result = d3d9::kOk;
}

void shimSurfaceLockRect(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireSurfaceState(cpu, call, "IDirect3DSurface9::LockRect(+0x34)");
    if (state == 0) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9::DeviceInfo dev;
    if (d3d9::deviceInfoOfState(cpu, state, &dev, nullptr)) {
        // ★ 后缓冲 surface 的 state 就是设备状态块（'D9D1'）：锁字段落在像素块 `GuestSurface` 上，
        //   ⛔ 不写状态块（64 B 全满）/ 纹理的 'TEX1'（+44/+48 会踩别人的）。
        // ★ 失败面：契约级 guest 错误 ⇒ D3DERR_INVALIDCALL + Warn（guest 自己对 `0x4A4F8D test eax,eax`
        //   做了分支）；本仓状态崩坏（`broken`）⇒ 照旧 loud trap。
        // ★ 参数序号（真机 `0x4A4F73 push 0` / `0x4A4F89 push &var_44` / `0x4A4F8A push eax`）：
        //   u32(1)=pLockedRect、u32(2)=pRect、u32(3)=Flags。
        bool broken = false;
        std::string why;
        if (!d3d9::lockBackBuffer(cpu, state, call.u32(1), call.u32(2), call.u32(3), &broken, &why)) {
            if (broken) {
                d3dxTrap(cpu, "IDirect3DSurface9::LockRect(+0x34)", "%s", why.c_str());
            } else {
                d3dxWarn("IDirect3DSurface9::LockRect(+0x34)", "%s", why.c_str());
            }
            call.result = d3d9::kInvalidCall;
            return;
        }
        call.result = d3d9::kOk;
        return;
    }
    d3d9::TextureInfo ti;
    std::string why;
    if (!d3d9::surfaceInfoOfState(cpu, state, &ti, &why)) {
        d3dxTrap(cpu, "IDirect3DSurface9::LockRect(+0x34)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    if (!d3d9::lockRect(cpu, ti, 0, call.u32(1), call.u32(2), call.u32(3), &why)) {
        d3dxTrap(cpu, "IDirect3DSurface9::LockRect(+0x34)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    call.result = d3d9::kOk;
}

void shimSurfaceUnlockRect(X86& cpu, ShimCall& call) {
    const std::uint32_t state = requireSurfaceState(cpu, call, "IDirect3DSurface9::UnlockRect(+0x38)");
    if (state == 0) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9::DeviceInfo dev;
    if (d3d9::deviceInfoOfState(cpu, state, &dev, nullptr)) {
        // 同 LockRect：契约级失败（未锁就解锁）答 D3DERR_INVALIDCALL；本仓状态崩坏才 trap。
        bool broken = false;
        std::string why;
        if (!d3d9::unlockBackBuffer(cpu, state, &broken, &why)) {
            if (broken) {
                d3dxTrap(cpu, "IDirect3DSurface9::UnlockRect(+0x38)", "%s", why.c_str());
            } else {
                d3dxWarn("IDirect3DSurface9::UnlockRect(+0x38)", "%s", why.c_str());
            }
            call.result = d3d9::kInvalidCall;
            return;
        }
        call.result = d3d9::kOk;
        return;
    }
    d3d9::TextureInfo ti;
    std::string why;
    if (!d3d9::surfaceInfoOfState(cpu, state, &ti, &why)) {
        d3dxTrap(cpu, "IDirect3DSurface9::UnlockRect(+0x38)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    if (!d3d9::unlockRect(cpu, ti, 0, &why)) {
        d3dxTrap(cpu, "IDirect3DSurface9::UnlockRect(+0x38)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    call.result = d3d9::kOk;
}

const std::vector<ComMethod>& surfaceMethods() {
    static const std::vector<ComMethod> methods = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &shimSurfaceQueryInterface},
        {0x04u, "AddRef", Conv::Stdcall, 0, &shimSurfaceAddRef},
        {0x08u, "Release", Conv::Stdcall, 0, &shimSurfaceRelease},
        {0x0Cu, "GetDevice", Conv::Stdcall, 1, &shimSurfaceGetDevice},
        {0x28u, "GetType", Conv::Stdcall, 0, &shimSurfaceGetType},
        {0x30u, "GetDesc", Conv::Stdcall, 1, &shimSurfaceGetDesc},
        {0x34u, "LockRect", Conv::Stdcall, 3, &shimSurfaceLockRect},
        {0x38u, "UnlockRect", Conv::Stdcall, 0, &shimSurfaceUnlockRect},
        // 哨兵：IDirect3DSurface9 最后一个方法 ReleaseDC 在 +0x40（17 槽）
        {0x40u, "ReleaseDC", Conv::Stdcall, 1, nullptr},
    };
    return methods;
}

const std::vector<ComMethod>& textureMethods() {
    static const std::vector<ComMethod> methods = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &shimTextureQueryInterface},
        {0x04u, "AddRef", Conv::Stdcall, 0, &shimTextureAddRef},
        {0x08u, "Release", Conv::Stdcall, 0, &shimTextureRelease},
        {0x0Cu, "GetDevice", Conv::Stdcall, 1, &shimTextureGetDevice},        // #3
        {0x28u, "GetType", Conv::Stdcall, 0, &shimTextureGetType},            // #10
        {0x34u, "GetLevelCount", Conv::Stdcall, 0, &shimTextureGetLevelCount},  // #13
        {0x44u, "GetLevelDesc", Conv::Stdcall, 2, &shimTextureGetLevelDesc},  // #18 ★ 0x48AD18
        {0x48u, "GetSurfaceLevel", Conv::Stdcall, 2, &shimTextureGetSurfaceLevel},  // #19 ★ 0x4A7755
        {0x4Cu, "LockRect", Conv::Stdcall, 4, &shimTextureLockRect},          // #20
        {0x50u, "UnlockRect", Conv::Stdcall, 1, &shimTextureUnlockRect},      // #20
        // ★ 哨兵：把 vtable 撑到 22 槽（最后一个 AddDirtyRect 在 +0x54）
        {0x54u, "AddDirtyRect", Conv::Stdcall, 1, nullptr},                   // #21 未取证 ⇒ loud trap
    };
    return methods;
}

// ---------------------------------------------------------------------------------------
// D3DXCreateTextureFromFileInMemory：读文件头建纹理 + **解 PNG 像素并上传**
//
// ★ 这条导出是 TITLE 的 Live2D 图集页入口（`.lst` 0x478398，调用点 `sub_478370`；
//   参数 = pDevice / pSrcData / SrcDataSize / ppTexture）。它**没有尺寸参数**：宽高来自图像头。
// ★ 历史（`REQ-01M738VYG0DP3TNJKYDSA49J7H`）：本 shim 原先只从 PNG 的 IHDR 取宽高、像素不解码
//   ⇒ 建出来的是一块全 0 的 arena（三张 2048² 图集页 `texserial=21/22/23` 因此永远不可见；
//   根因定案见 `.tmp/d3d9/a4-creator.md`）。现在 PNG 走 libpng 简化 API 解成 8-bit RGBA，
//   再**走既有的 `d3d9::lockRect` / `unlockRect`** 写进纹理 arena —— 与 native upload 同一份
//   语义（版本号 / 脏区 / GPU 上传缓存都挂在那条路上，⛔ 不另造像素写路径）。
// ★ 支持的容器：PNG（**真解码**，一律 A8R8G8B8）/ BMP / DDS（仅未压缩 16/24/32bpp，仍**只读头**）。
//   其它格式返回 E_FAIL（调用方本来就会处理 HRESULT 失败），并且 Warn 里点名首 8 字节 —— ⛔ 不猜尺寸。
// ★ 失败面：坏 PNG / 截断 / 尺寸超上限 / 上传失败 ⇒ `E_FAIL`（**可继续**，调用方 `0x47839D` 有失败分支），
//   ⛔ 不 trap；且**不留半张纹理**（解码在 createTextureState 之前，上传失败则回收状态块 + arena ——
//   两条失败面都有用例：`d3dx9/texture-from-memory-png` 的 ③ 与 `d3dx9/texture-upload-rollback`）。
//   BMP / DDS 的像素仍是清零的 arena（**故意的 stub**，见需求单的"未做"）。
// ---------------------------------------------------------------------------------------
enum class ImageContainer { kPng, kBmp, kDds };

struct ImageInfo {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t format = 0;
    const char* kind = "?";
    ImageContainer container = ImageContainer::kPng;
};

std::uint32_t rdBe32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}
std::uint32_t rdLe32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint16_t rdLe16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8));
}

bool parseImageHeader(const std::uint8_t* b, std::size_t n, ImageInfo* out, std::string* why) {
    static const std::uint8_t kPngSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (n >= 8 && std::memcmp(b, kPngSig, 8) == 0) {
        if (n < 33 || std::memcmp(b + 12, "IHDR", 4) != 0) {
            if (why) *why = "PNG：缺 IHDR（或文件 < 33 B）";
            return false;
        }
        const std::uint32_t w = rdBe32(b + 16);
        const std::uint32_t h = rdBe32(b + 20);
        if (w == 0 || h == 0) {
            if (why) *why = "PNG：IHDR 的宽/高为 0";
            return false;
        }
        // D3DX 对 PNG 走 32bpp；带 alpha 的模型贴图必须有 alpha 通道，否则整张图会被当成不透明。
        out->kind = "PNG";
        out->container = ImageContainer::kPng;
        out->width = w;
        out->height = h;
        out->format = d3d9::kFmtA8R8G8B8;
        return true;
    }
    if (n >= 34 && b[0] == 'B' && b[1] == 'M') {
        const std::uint16_t bpp = rdLe16(b + 28);
        const std::uint32_t compression = rdLe32(b + 30);
        std::int32_t h = static_cast<std::int32_t>(rdLe32(b + 22));
        if (h < 0) h = -h;
        const std::uint32_t w = rdLe32(b + 18);
        if (w == 0 || h == 0) {
            if (why) *why = "BMP：宽/高为 0";
            return false;
        }
        if (compression != 0) {
            if (why) *why = "BMP：只支持 BI_RGB（compression=0）";
            return false;
        }
        std::uint32_t fmt = 0;
        if (bpp == 32) fmt = d3d9::kFmtX8R8G8B8;       // BI_RGB 的 alpha 字节通常无定义
        else if (bpp == 24) fmt = d3d9::kFmtX8R8G8B8;  // D3DX 会补成 32bpp
        else if (bpp == 16) fmt = d3d9::kFmtR5G6B5;
        else {
            if (why) *why = "BMP：只支持 16/24/32bpp";
            return false;
        }
        out->kind = "BMP";
        out->container = ImageContainer::kBmp;
        out->width = w;
        out->height = static_cast<std::uint32_t>(h);
        out->format = fmt;
        return true;
    }
    if (n >= 128 && std::memcmp(b, "DDS ", 4) == 0) {
        if (rdLe32(b + 4) != 124) {
            if (why) *why = "DDS：header size != 124";
            return false;
        }
        const std::uint32_t h = rdLe32(b + 12);
        const std::uint32_t w = rdLe32(b + 16);
        const std::uint32_t pf_size = rdLe32(b + 76);
        const std::uint32_t pf_flags = rdLe32(b + 80);
        const std::uint32_t fourcc = rdLe32(b + 84);
        const std::uint32_t bitcount = rdLe32(b + 88);
        const std::uint32_t amask = rdLe32(b + 104);
        if (pf_size != 32) {
            if (why) *why = "DDS：pixel format size != 32";
            return false;
        }
        if (fourcc != 0) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "DDS：压缩格式 FourCC=0x%08X 本线不支持", fourcc);
            if (why) *why = buf;
            return false;
        }
        if ((pf_flags & 0x40u) == 0) {  // DDPF_RGB
            if (why) *why = "DDS：pixel format 不是 RGB";
            return false;
        }
        std::uint32_t fmt = 0;
        if (bitcount == 32) fmt = amask ? d3d9::kFmtA8R8G8B8 : d3d9::kFmtX8R8G8B8;
        else if (bitcount == 24) fmt = d3d9::kFmtX8R8G8B8;
        else if (bitcount == 16) fmt = d3d9::kFmtR5G6B5;
        else {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "DDS：%u bpp 本线不支持", bitcount);
            if (why) *why = buf;
            return false;
        }
        if (w == 0 || h == 0) {
            if (why) *why = "DDS：宽/高为 0";
            return false;
        }
        out->kind = "DDS";
        out->container = ImageContainer::kDds;
        out->width = w;
        out->height = h;
        out->format = fmt;
        return true;
    }
    if (why) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "未知图像容器（首 8 字节 %02X %02X %02X %02X %02X %02X %02X %02X）",
                      n > 0 ? b[0] : 0, n > 1 ? b[1] : 0, n > 2 ? b[2] : 0, n > 3 ? b[3] : 0,
                      n > 4 ? b[4] : 0, n > 5 ? b[5] : 0, n > 6 ? b[6] : 0, n > 7 ? b[7] : 0);
        *why = buf;
    }
    return false;
}

// ---------------------------------------------------------------------------------------
// 解码结果 → 纹理 arena：**只经** `d3d9::lockRect` / `unlockRect`（版本号 / 脏区 / GPU 上传缓存
// 全挂在那条路上）。返回 false 时纹理**已解锁**，调用方负责回收状态块（⛔ 不留半张）。
//
// ★ 为什么不用 `esp - 8` 放 `D3DLOCKED_RECT`（native_image_upload.cpp:144 的写法）：那是
//   "native override 在函数入口整体替掉 guest 函数、它的序言从不执行"这条特定理由；本 shim 是
//   **导出调用**（guest 自己 `call` 进来的），没有那条前提 ⇒ 放 guest 堆上，用完立刻 free。
// ---------------------------------------------------------------------------------------
bool uploadRgbaToTexture(X86& cpu, std::uint32_t state, const png::RgbaImage& image, std::string* why) {
    auto bad = [why](std::string text) {
        if (why != nullptr) *why = std::move(text);
        return false;
    };
    d3d9::TextureInfo tex;
    if (!d3d9::textureInfoOfState(cpu, state, &tex, why)) return false;
    if (tex.format != d3d9::kFmtA8R8G8B8 || tex.bpp != 32u) return bad("目标纹理不是 A8R8G8B8/32bpp");
    if (tex.width != image.width || tex.height != image.height) {
        return bad("目标纹理尺寸与解码结果不一致");
    }
    if (tex.locked) return bad("目标纹理已处于锁定态");
    if (cpu.heap == nullptr) return bad("guest 堆未就绪（放不下 D3DLOCKED_RECT）");
    const std::uint32_t locked_rect_ea = cpu.heap->alloc(8u, /*zero=*/true);
    if (locked_rect_ea == 0) return bad("guest 堆分配 D3DLOCKED_RECT 失败");

    bool ok = true;
    std::string why_text;
    if (!d3d9::lockRect(cpu, tex, 0u, locked_rect_ea, 0u, 0u, &why_text)) {
        ok = false;  // 没锁上 ⇒ 不需要解锁
    } else {
        const std::uint32_t pitch = cpu.mem->rd32(locked_rect_ea + 0u);
        const std::uint32_t bits = cpu.mem->rd32(locked_rect_ea + 4u);
        const std::uint64_t need = static_cast<std::uint64_t>(pitch) * image.height;
        if (pitch < 4u * image.width) {
            why_text = "Lock 的 Pitch 小于 4*width（目标布局不是 32bpp top-down）";
            ok = false;
        } else if (bits == 0 || static_cast<std::uint64_t>(bits) + need > GuestMemory::kSpace ||
                   !cpu.mem->committed(bits, static_cast<std::size_t>(need))) {
            why_text = "Lock 返回的像素区间不可写（pBits 越界 / 未提交）";
            ok = false;
        } else {
            // 逐行按 Lock 的 Pitch 写（⛔ 不按 width 推行距）；行序 top-down（PNG 与 D3D 一致）。
            std::uint8_t* const dst = cpu.mem->host(bits);
            for (std::uint32_t y = 0; y < image.height; ++y) {
                const std::uint8_t* const src =
                    image.rgba.data() + static_cast<std::size_t>(y) * image.width * 4u;
                std::uint8_t* const row = dst + static_cast<std::size_t>(y) * pitch;
                for (std::uint32_t x = 0; x < image.width; ++x) {
                    // PNG 的 R,G,B,A ⇒ guest 的 A8R8G8B8（**内存序 B,G,R,A**，最低字节 = 蓝）。
                    row[x * 4u + 0u] = src[x * 4u + 2u];
                    row[x * 4u + 1u] = src[x * 4u + 1u];
                    row[x * 4u + 2u] = src[x * 4u + 0u];
                    row[x * 4u + 3u] = src[x * 4u + 3u];
                }
            }
        }
        // ★ 解锁**必须**发生（哪怕上面已判失败）：`lockRect` 已把状态块置成"已锁 / 内容未知"，
        //   不解锁 ⇒ 下一次 Lock 撞 D3DERR_INVALIDCALL，且版本号永远停在 0（GPU 每次都重传 + 哈希）。
        // ★ 快照必须**重取**：`lockRect` / `unlockRect` 只吃调用方给的 `TextureInfo`（其中的
        //   `locked` 是**调用前**的旧值）⇒ 拿旧快照去解锁会被以"未锁"拒绝（实测口径见 d3d9.cpp:1137）。
        d3d9::TextureInfo fresh;
        std::string unlock_why;
        if (!d3d9::textureInfoOfState(cpu, state, &fresh, nullptr)) {
            if (ok) {
                why_text = "解锁前重取纹理状态块失败";
                ok = false;
            }
        } else if (!d3d9::unlockRect(cpu, fresh, 0u, &unlock_why) && ok) {
            why_text = "解锁失败：" + unlock_why;
            ok = false;
        }
    }
    cpu.heap->free(locked_rect_ea);
    if (!ok) return bad(std::move(why_text));
    return true;
}

void shimD3DXCreateTextureFromFileInMemory(X86& cpu, ShimCall& call) {
    const std::uint32_t device = call.u32(0);
    const std::uint32_t src_ea = call.u32(1);
    const std::uint32_t src_size = call.u32(2);
    const std::uint32_t out_ea = call.u32(3);
    if (!cpu.com) {
        d3dxTrap(cpu, "D3DXCreateTextureFromFileInMemory", "cpu.com 为空（装配问题）");
        call.result = d3d9::kFail;
        return;
    }
    if (out_ea == 0 || !cpu.mem->committed(out_ea, 4)) {
        d3dxTrap(cpu, "D3DXCreateTextureFromFileInMemory", "ppTexture=0x%08X 为 NULL / 未提交", out_ea);
        call.result = d3d9::kInvalidArg;
        return;
    }
    cpu.mem->wr32(out_ea, 0);
    if (device == 0) {
        d3dxLog("D3DXCreateTextureFromFileInMemory", "pDevice=NULL ⇒ E_INVALIDARG");
        call.result = d3d9::kInvalidArg;
        return;
    }
    if (src_ea == 0 || src_size < 8 || !cpu.mem->committed(src_ea, 8)) {
        d3dxTrap(cpu, "D3DXCreateTextureFromFileInMemory",
                 "pSrcData=0x%08X SrcDataSize=%u 为 NULL / 太小 / 未提交", src_ea, src_size);
        call.result = d3d9::kInvalidArg;
        return;
    }
    const std::size_t probe = src_size < 128u ? static_cast<std::size_t>(src_size) : 128u;
    if (!cpu.mem->committed(src_ea, probe)) {
        d3dxTrap(cpu, "D3DXCreateTextureFromFileInMemory", "pSrcData=0x%08X 起 %zu 字节未提交", src_ea, probe);
        call.result = d3d9::kInvalidArg;
        return;
    }
    std::uint8_t header[128] = {};
    cpu.mem->readBytes(src_ea, header, probe);
    ImageInfo info;
    std::string why;
    if (!parseImageHeader(header, probe, &info, &why)) {
        // 调用方对 HRESULT 失败有分支（0x47839D / 0x47992D / 0x479EF4），所以这里返回 E_FAIL 是
        // **可继续**的失败；⛔ 不 trap，也⛔ 不假装成功建一张尺寸未知的纹理。
        d3dxWarn("D3DXCreateTextureFromFileInMemory", "%s（SrcDataSize=%u）⇒ E_FAIL", why.c_str(), src_size);
        call.result = d3d9::kFail;
        return;
    }
    // ── PNG：**在 createTextureState 之前**解码 ───────────────────────────────────────────────
    // ★ 顺序的唯一理由 = 失败面：解码失败 ⇒ 什么都不建（⛔ 不留一张"建好但像素全 0"的纹理）；
    //   上传失败 ⇒ 只需 `destroyTextureState`（arena + 状态块一起回，见下面）。
    // ★ 输入 = **guest 内存里的文件字节**：libpng 简化 API 不拷输入（只存指针，直到
    //   `png_image_finish_read` 返回都可能读它）。这里直接把 guest 平坦映射交给它，依据：
    //   ① `GuestMemory::host(ea) = base_ + ea`，`base_` 进程内不变；② 解码是同步的、期间不跑
    //   guest 代码、不分配 guest 内存；③ 下面先做整段 `committed` 检查。⛔ 不用 `FILE*`。
    png::RgbaImage decoded;
    if (info.container == ImageContainer::kPng) {
        // ★ 整段检查（不是仅前 128 B）：跨未提交页的读会被判成 guest fault，而 `g_access` 会带着
        //   **上一次**访存的地址，归因会张冠李戴（见 memory.h 的 head 注释）。
        if (static_cast<std::uint64_t>(src_ea) + static_cast<std::uint64_t>(src_size) > GuestMemory::kSpace ||
            !cpu.mem->committed(src_ea, src_size)) {
            d3dxWarn("D3DXCreateTextureFromFileInMemory",
                     "PNG 的 SrcDataSize=%u 字节不全在已提交内存里（pSrcData=0x%08X）⇒ E_FAIL", src_size,
                     src_ea);
            call.result = d3d9::kFail;
            return;
        }
        std::string decode_why;
        if (!png::decodeRgba8(cpu.mem->host(src_ea), src_size, d3d9::kMaxTextureWidth, &decoded,
                              &decode_why)) {
            // ★ 坏 PNG / 截断 / 超上限 = **可继续**的失败（真机 D3DX 同样返回 E_FAIL；调用方
            //   `0x47839D` 有失败分支）⇒ ⛔ 不 trap、⛔ 不建纹理。
            d3dxWarn("D3DXCreateTextureFromFileInMemory", "%s（SrcDataSize=%u）⇒ E_FAIL", decode_why.c_str(),
                     src_size);
            call.result = d3d9::kFail;
            return;
        }
        if (decoded.width != info.width || decoded.height != info.height) {
            // 两份读数来自**同一份 IHDR**，不一致说明解析路径有问题 ⇒ 以解码结果为准（那是像素的真实尺寸）。
            d3dxWarn("D3DXCreateTextureFromFileInMemory",
                     "PNG 头宽高 %ux%u 与解码结果 %ux%u 不一致 ⇒ 按解码结果建纹理", info.width, info.height,
                     decoded.width, decoded.height);
            info.width = decoded.width;
            info.height = decoded.height;
        }
    }
    std::string make_why;
    bool out_of_memory = false;
    const std::uint32_t state = d3d9::createTextureState(cpu, device, info.width, info.height, 1u, 0u,
                                                         info.format, 1u /*D3DPOOL_MANAGED*/, &make_why,
                                                         &out_of_memory);
    if (state == 0) {
        if (out_of_memory) {
            // ★ 我们自己的容量问题 ≠ "这条导出没实现"：独立措辞 + 独立 stop_why，⛔ 不记缺口。
            d3dxResourceStop(cpu, "D3DXCreateTextureFromFileInMemory", "%s %s：%s", info.kind,
                             textureShapeNote(info.width, info.height, info.format).c_str(),
                             make_why.c_str());
            call.result = d3d9::kOutOfMemory;
            return;
        }
        // ★ 失败点必须自带 w×h / format / 预计字节数：`why` 只说"分配失败"时，
        //   不跑第二次就没法把 5898240 这类数字对回到具体一张纹理。
        d3dxTrap(cpu, "D3DXCreateTextureFromFileInMemory", "%s %s：%s", info.kind,
                 textureShapeNote(info.width, info.height, info.format).c_str(), make_why.c_str());
        call.result = d3d9::kFail;
        return;
    }
    // ── 上传：**走既有的 lock/unlock 路径**（与 native upload 同一份语义）───────────────────
    // ★ 失败 ⇒ 回收状态块 + arena（`destroyTextureState`），返回 E_FAIL。
    //   这是"不留半张纹理"的唯一保证：此时 COM 对象还没建，只欠这一个清理动作。
    if (info.container == ImageContainer::kPng) {
        std::string upload_why;
        if (!uploadRgbaToTexture(cpu, state, decoded, &upload_why)) {
            d3d9::destroyTextureState(cpu, state);
            d3dxWarn("D3DXCreateTextureFromFileInMemory",
                     "PNG %ux%u 像素上传失败：%s ⇒ E_FAIL（状态块 / 像素块已回收）", decoded.width,
                     decoded.height, upload_why.c_str());
            call.result = d3d9::kFail;
            return;
        }
    }
    // ★ 析构钩子：纹理对象**独占**状态块 + arena 像素块 ⇒ 引用降到 0 时由 ComRegistry 的
    //   唯一收敛点回收（内部裸 release 也覆盖）。⛔ 不要再在 Release shim 里单独调一次。
    const std::uint32_t obj =
        cpu.com->createObject(cpu, "IDirect3DTexture9", textureMethods(), state, &d3d9::destroyTextureState);
    if (obj == 0) {
        d3d9::destroyTextureState(cpu, state);
        d3dxTrap(cpu, "D3DXCreateTextureFromFileInMemory", "在 guest 内存里建 IDirect3DTexture9 对象失败");
        call.result = d3d9::kFail;
        return;
    }
    cpu.mem->wr32(out_ea, obj);
    d3dxLog("D3DXCreateTextureFromFileInMemory",
            "%s %ux%u（SrcDataSize=%u）⇒ S_OK，IDirect3DTexture9 对象 0x%08X（%s）", info.kind, info.width,
            info.height, src_size, obj,
            info.container == ImageContainer::kPng ? "像素已解码，经 LockRect/UnlockRect 上传"
                                                   : "BMP/DDS 仍是 stub：只建不写");
    call.result = d3d9::kOk;
}

void shimD3DXCreateTexture(X86& cpu, ShimCall& call) {
    const std::uint32_t device = call.u32(0);
    const std::uint32_t width = call.u32(1);
    const std::uint32_t height = call.u32(2);
    const std::uint32_t levels = call.u32(3);
    const std::uint32_t usage = call.u32(4);
    const std::uint32_t format = call.u32(5);
    const std::uint32_t pool = call.u32(6);
    const std::uint32_t out_ea = call.u32(7);
    if (!cpu.com) {
        d3dxTrap(cpu, "D3DXCreateTexture", "cpu.com 为空（装配问题）");
        call.result = d3d9::kFail;
        return;
    }
    if (out_ea == 0 || !cpu.mem->committed(out_ea, 4)) {
        d3dxTrap(cpu, "D3DXCreateTexture", "ppTexture=0x%08X 为 NULL / 未提交", out_ea);
        call.result = d3d9::kInvalidArg;
        return;
    }
    cpu.mem->wr32(out_ea, 0);
    if (device == 0) {
        d3dxLog("D3DXCreateTexture", "pDevice=NULL ⇒ E_INVALIDARG");
        call.result = d3d9::kInvalidArg;
        return;
    }
    std::string why;
    bool out_of_memory = false;
    const std::uint32_t state =
        d3d9::createTextureState(cpu, device, width, height, levels, usage, format, pool, &why,
                                 &out_of_memory);
    if (state == 0) {
        if (out_of_memory) {
            // ★ 我们自己的容量问题 ≠ "这条导出没实现"：D3DX 真机在这里返回 E_OUTOFMEMORY，
            //   但本线返回它会让 guest 走去 throw C++ 异常（实测，见 d3dxResourceStop 的注释）
            //   ⇒ 独立措辞 + 独立 stop 原因硬停；⛔ 不写 "unimplemented"、⛔ 不 ++unimplemented_calls。
            d3dxResourceStop(cpu, "D3DXCreateTexture", "%s：%s",
                             textureShapeNote(width, height, format).c_str(), why.c_str());
            call.result = d3d9::kOutOfMemory;
            return;
        }
        d3dxTrap(cpu, "D3DXCreateTexture", "%s：%s", textureShapeNote(width, height, format).c_str(),
                 why.c_str());
        call.result = d3d9::kFail;
        return;
    }
    // ★ 析构钩子：见 D3DXCreateTextureFromFileInMemory 处的同一条说明。
    const std::uint32_t obj =
        cpu.com->createObject(cpu, "IDirect3DTexture9", textureMethods(), state, &d3d9::destroyTextureState);
    if (obj == 0) {
        d3d9::destroyTextureState(cpu, state);
        d3dxTrap(cpu, "D3DXCreateTexture", "在 guest 内存里建 IDirect3DTexture9 对象失败");
        call.result = d3d9::kFail;
        return;
    }
    cpu.mem->wr32(out_ea, obj);
    d3dxLog("D3DXCreateTexture", "%ux%u D3DFORMAT=%u ⇒ S_OK，IDirect3DTexture9 对象 0x%08X", width, height,
            format, obj);
    call.result = d3d9::kOk;
}

// ---------------------------------------------------------------------------------------
// D3DX math（本线只实现**确定能写对**的那几条；其余仍是 gap 里的 loud trap）
// ★ D3DXMATRIX = 16 个 float，行主序；D3DX 用**行向量**约定（v' = v * M）。
// ---------------------------------------------------------------------------------------
float rdF(X86& cpu, std::uint32_t ea) {
    const std::uint32_t bits = cpu.mem->rd32(ea);
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}
void wrF(X86& cpu, std::uint32_t ea, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    cpu.mem->wr32(ea, bits);
}
// 把 GLM 矩阵按 D3DX 的 16-float 布局写回 guest（⛔ guest ABI 不变）
void wrMat(X86& cpu, std::uint32_t ea, const math::Mat4& m) {
    float d3d[16];
    math::toD3DLayout(m, d3d);
    for (int i = 0; i < 16; ++i) wrF(cpu, ea + static_cast<std::uint32_t>(i) * 4u, d3d[i]);
}
float u32ToFloat(std::uint32_t bits) {
    float f = 0.0f;
    std::memcpy(&f, &bits, 4);
    return f;
}

void shimD3DXMatrixMultiply(X86& cpu, ShimCall& call) {
    const std::uint32_t out_ea = call.u32(0);
    const std::uint32_t a_ea = call.u32(1);
    const std::uint32_t b_ea = call.u32(2);
    if (!cpu.mem->committed(out_ea, 64) || !cpu.mem->committed(a_ea, 64) || !cpu.mem->committed(b_ea, 64)) {
        d3dxTrap(cpu, "D3DXMatrixMultiply", "pOut=0x%08X pM1=0x%08X pM2=0x%08X 有未提交的 64 B 矩阵", out_ea,
                 a_ea, b_ea);
        call.result = 0;
        return;
    }
    float a_in[16];
    float b_in[16];
    for (int i = 0; i < 16; ++i) {
        a_in[i] = rdF(cpu, a_ea + static_cast<std::uint32_t>(i) * 4u);
        b_in[i] = rdF(cpu, b_ea + static_cast<std::uint32_t>(i) * 4u);
    }
    // ★ 唯一的适配点：D3DX 的 A*B（行向量）在 GLM 里是 B^T*A^T ⇒ multiplyD3D 内部反转顺序。
    const math::Mat4 product = math::multiplyD3D(math::fromD3DLayout(a_in), math::fromD3DLayout(b_in));
    wrMat(cpu, out_ea, product);
    call.result = out_ea;
}

void shimD3DXMatrixScaling(X86& cpu, ShimCall& call) {
    const std::uint32_t out_ea = call.u32(0);
    if (!cpu.mem->committed(out_ea, 64)) {
        d3dxTrap(cpu, "D3DXMatrixScaling", "pOut=0x%08X 未提交", out_ea);
        call.result = 0;
        return;
    }
    // 参数是 3 个 float：sx / sy / sz（按 stdcall 的 4 字节栈槽原样读）
    const float sx = u32ToFloat(call.u32(1));
    const float sy = u32ToFloat(call.u32(2));
    const float sz = u32ToFloat(call.u32(3));
    wrMat(cpu, out_ea, math::scaling(sx, sy, sz));
    call.result = out_ea;
}

void shimD3DXMatrixTranslation(X86& cpu, ShimCall& call) {
    const std::uint32_t out_ea = call.u32(0);
    if (!cpu.mem->committed(out_ea, 64)) {
        d3dxTrap(cpu, "D3DXMatrixTranslation", "pOut=0x%08X 未提交", out_ea);
        call.result = 0;
        return;
    }
    const float x = u32ToFloat(call.u32(1));
    const float y = u32ToFloat(call.u32(2));
    const float z = u32ToFloat(call.u32(3));
    wrMat(cpu, out_ea, math::translation(x, y, z));
    call.result = out_ea;
}

void shimD3DXVec4Transform(X86& cpu, ShimCall& call) {
    const std::uint32_t out_ea = call.u32(0);
    const std::uint32_t v_ea = call.u32(1);
    const std::uint32_t m_ea = call.u32(2);
    if (!cpu.mem->committed(out_ea, 16) || !cpu.mem->committed(v_ea, 16) || !cpu.mem->committed(m_ea, 64)) {
        d3dxTrap(cpu, "D3DXVec4Transform", "pOut=0x%08X pV=0x%08X pM=0x%08X 有未提交的缓冲区", out_ea, v_ea,
                 m_ea);
        call.result = 0;
        return;
    }
    float v[4];
    for (int i = 0; i < 4; ++i) v[i] = rdF(cpu, v_ea + static_cast<std::uint32_t>(i) * 4u);
    float m_in[16];
    for (int i = 0; i < 16; ++i) m_in[i] = rdF(cpu, m_ea + static_cast<std::uint32_t>(i) * 4u);
    // v' = v*M（行向量）⇒ GLM 的 M^T * v；同一个适配点也供内部 T&L / Sprite 用。
    const math::Vec4 out = math::transformVec4D3D(math::fromD3DLayout(m_in), math::Vec4(v[0], v[1], v[2], v[3]));
    for (int i = 0; i < 4; ++i) wrF(cpu, out_ea + static_cast<std::uint32_t>(i) * 4u, out[i]);
    call.result = out_ea;
}

// ---- 3D 相机 / 投影矩阵（D3DX 的**左手**坐标系；行向量约定 v' = v * M）----
// ★ 公式只在 `runtime/math.cpp` 里存在一份；这里只负责读 guest 参数 / 把结果写回 D3DXMATRIX。
//   D3DXMatrixMultiply / Vec4Transform / RotationAxis / YawPitchRoll / PerspectiveFovLH /
//   LookAtLH / OrthoLH 全部走 `math::` 的同一套适配（见 runtime/math.h 文件头）。

void shimD3DXMatrixPerspectiveFovLH(X86& cpu, ShimCall& call) {
    const std::uint32_t out_ea = call.u32(0);
    if (!cpu.mem->committed(out_ea, 64)) {
        d3dxTrap(cpu, "D3DXMatrixPerspectiveFovLH", "pOut=0x%08X 未提交", out_ea);
        call.result = 0;
        return;
    }
    const float fovy = u32ToFloat(call.u32(1));
    const float aspect = u32ToFloat(call.u32(2));
    const float zn = u32ToFloat(call.u32(3));
    const float zf = u32ToFloat(call.u32(4));
    wrMat(cpu, out_ea, math::perspectiveFovLH(fovy, aspect, zn, zf));
    call.result = out_ea;
}

void shimD3DXMatrixLookAtLH(X86& cpu, ShimCall& call) {
    const std::uint32_t out_ea = call.u32(0);
    const std::uint32_t eye_ea = call.u32(1);
    const std::uint32_t at_ea = call.u32(2);
    const std::uint32_t up_ea = call.u32(3);
    if (!cpu.mem->committed(out_ea, 64) || !cpu.mem->committed(eye_ea, 12) ||
        !cpu.mem->committed(at_ea, 12) || !cpu.mem->committed(up_ea, 12)) {
        d3dxTrap(cpu, "D3DXMatrixLookAtLH", "pOut=0x%08X / eye=0x%08X / at=0x%08X / up=0x%08X 有未提交缓冲区",
                 out_ea, eye_ea, at_ea, up_ea);
        call.result = 0;
        return;
    }
    float eye[3], at[3], up[3];
    for (int i = 0; i < 3; ++i) {
        eye[i] = rdF(cpu, eye_ea + static_cast<std::uint32_t>(i) * 4u);
        at[i] = rdF(cpu, at_ea + static_cast<std::uint32_t>(i) * 4u);
        up[i] = rdF(cpu, up_ea + static_cast<std::uint32_t>(i) * 4u);
    }
    wrMat(cpu, out_ea, math::lookAtLH(eye, at, up));
    call.result = out_ea;
}

void shimD3DXMatrixOrthoLH(X86& cpu, ShimCall& call) {
    const std::uint32_t out_ea = call.u32(0);
    if (!cpu.mem->committed(out_ea, 64)) {
        d3dxTrap(cpu, "D3DXMatrixOrthoLH", "pOut=0x%08X 未提交", out_ea);
        call.result = 0;
        return;
    }
    const float w = u32ToFloat(call.u32(1));
    const float h = u32ToFloat(call.u32(2));
    const float zn = u32ToFloat(call.u32(3));
    const float zf = u32ToFloat(call.u32(4));
    wrMat(cpu, out_ea, math::orthoLH(w, h, zn, zf));
    call.result = out_ea;
}

void shimD3DXMatrixRotationAxis(X86& cpu, ShimCall& call) {
    const std::uint32_t out_ea = call.u32(0);
    const std::uint32_t axis_ea = call.u32(1);
    if (!cpu.mem->committed(out_ea, 64) || !cpu.mem->committed(axis_ea, 12)) {
        d3dxTrap(cpu, "D3DXMatrixRotationAxis", "pOut=0x%08X / pV=0x%08X 有未提交缓冲区", out_ea, axis_ea);
        call.result = 0;
        return;
    }
    const float angle = u32ToFloat(call.u32(2));
    const float x = rdF(cpu, axis_ea + 0);
    const float y = rdF(cpu, axis_ea + 4);
    const float z = rdF(cpu, axis_ea + 8);
    wrMat(cpu, out_ea, math::rotationAxis(x, y, z, angle));
    call.result = out_ea;
}

void shimD3DXMatrixRotationYawPitchRoll(X86& cpu, ShimCall& call) {
    const std::uint32_t out_ea = call.u32(0);
    if (!cpu.mem->committed(out_ea, 64)) {
        d3dxTrap(cpu, "D3DXMatrixRotationYawPitchRoll", "pOut=0x%08X 未提交", out_ea);
        call.result = 0;
        return;
    }
    const float yaw = u32ToFloat(call.u32(1));
    const float pitch = u32ToFloat(call.u32(2));
    const float roll = u32ToFloat(call.u32(3));
    wrMat(cpu, out_ea, math::rotationYawPitchRoll(yaw, pitch, roll));
    call.result = out_ea;
}

void shimD3DXVec3Normalize(X86& cpu, ShimCall& call) {
    const std::uint32_t out_ea = call.u32(0);
    const std::uint32_t v_ea = call.u32(1);
    if (!cpu.mem->committed(out_ea, 12) || !cpu.mem->committed(v_ea, 12)) {
        d3dxTrap(cpu, "D3DXVec3Normalize", "pOut=0x%08X pV=0x%08X 有未提交的 12 B 向量", out_ea, v_ea);
        call.result = 0;
        return;
    }
    const float in[3] = {rdF(cpu, v_ea + 0u), rdF(cpu, v_ea + 4u), rdF(cpu, v_ea + 8u)};
    float out[3];
    const float len = math::normalize3(in, out);  // 零向量 ⇒ (0,0,0)、长度 0（政策在 math 里唯一）
    wrF(cpu, out_ea + 0u, out[0]);
    wrF(cpu, out_ea + 4u, out[1]);
    wrF(cpu, out_ea + 8u, out[2]);
    std::uint32_t bits = 0;
    std::memcpy(&bits, &len, 4);
    call.result = bits;
}



// ---------------------------------------------------------------------------------------
// 导出 + 方法表
// ---------------------------------------------------------------------------------------
const std::vector<ComMethod>& spriteMethods();

void shimD3DXCreateSprite(X86& cpu, ShimCall& call) {
    const std::uint32_t device = call.u32(0);
    const std::uint32_t out_ea = call.u32(1);
    if (!cpu.com) {
        d3dxTrap(cpu, "D3DXCreateSprite", "cpu.com 为空（装配问题）⇒ 拿不到 guest 可见的 COM 对象");
        call.result = d3dx9::kFail;
        return;
    }
    if (out_ea == 0 || !cpu.mem->committed(out_ea, 4)) {
        d3dxTrap(cpu, "D3DXCreateSprite", "ppSprite=0x%08X 为 NULL / 未提交", out_ea);
        call.result = d3dx9::kInvalidArg;
        return;
    }
    cpu.mem->wr32(out_ea, 0);  // 真机在失败时也保证出参为 NULL
    if (device == 0) {
        d3dxLog("D3DXCreateSprite", "pDevice=NULL ⇒ E_INVALIDARG");
        call.result = d3dx9::kInvalidArg;
        return;
    }
    std::string why;
    bool out_of_memory = false;
    const std::uint32_t state = d3dx9::createSpriteState(cpu, device, &why, &out_of_memory);
    if (state == 0) {
        if (out_of_memory) {
            // ★ 同一口径：guest 堆不够 ≠ "D3DXCreateSprite 没实现"（⛔ 不记缺口、不写 unimplemented）。
            d3dxResourceStop(cpu, "D3DXCreateSprite", "%s", why.c_str());
            call.result = d3dx9::kFail;
            return;
        }
        d3dxTrap(cpu, "D3DXCreateSprite", "%s", why.c_str());
        call.result = d3dx9::kFail;
        return;
    }
    const std::uint32_t obj = cpu.com->createObject(cpu, "ID3DXSprite", spriteMethods(), state);
    if (obj == 0) {
        d3dx9::destroySpriteState(cpu, state);
        d3dxTrap(cpu, "D3DXCreateSprite",
                 "在 guest 内存里建 ID3DXSprite 对象失败（见上一行 com 日志）");
        call.result = d3dx9::kFail;
        return;
    }
    cpu.mem->wr32(out_ea, obj);
    d3dxLog("D3DXCreateSprite", "pDevice=0x%08X ⇒ S_OK，ID3DXSprite 对象 0x%08X（状态块 0x%08X）", device, obj,
            state);
    call.result = d3dx9::kOk;
}

// ID3DXSprite：14 槽（IUnknown 3 + 11），最后一个方法 OnResetDevice 在 +0x34。
// ★ +0x18 SetWorldViewRH / +0x1C SetWorldViewLH **故意不给实现**（fn=nullptr ⇒ comUnsupported）：
//   本线只做 2D 屏幕空间，SetWorldView 是 3D 路径；loud trap 比"接受但不用"诚实。
const std::vector<ComMethod>& spriteMethods() {
    static const std::vector<ComMethod> methods = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &shimSpriteQueryInterface},
        {0x04u, "AddRef", Conv::Stdcall, 0, &shimSpriteAddRef},
        {0x08u, "Release", Conv::Stdcall, 0, &shimSpriteRelease},
        {0x0Cu, "GetDevice", Conv::Stdcall, 1, &shimSpriteGetDevice},          // #3  ⚠ 未取证（SDK 推断）
        {0x10u, "GetTransform", Conv::Stdcall, 1, &shimSpriteGetTransform},    // #4  ⚠ 未取证（SDK 推断）
        {0x14u, "SetTransform", Conv::Stdcall, 1, &shimSpriteSetTransform},    // #5  ★ 0x4A30D3
        {0x18u, "SetWorldViewRH", Conv::Stdcall, 2, nullptr},                  // #6  未取证 ⇒ loud trap
        {0x1Cu, "SetWorldViewLH", Conv::Stdcall, 2, nullptr},                  // #7  未取证 ⇒ loud trap
        {0x20u, "Begin", Conv::Stdcall, 1, &shimSpriteBegin},                  // #8  ★ 0x4A30E5
        {0x24u, "Draw", Conv::Stdcall, 5, &shimSpriteDraw},                    // #9  ★ 0x48AE6E
        {0x28u, "Flush", Conv::Stdcall, 0, &shimSpriteFlush},                  // #10 ⚠ 未取证（SDK 推断）
        {0x2Cu, "End", Conv::Stdcall, 0, &shimSpriteEnd},                      // #11 ★ 0x4A00EC / 0x4A356A
        {0x30u, "OnLostDevice", Conv::Stdcall, 0, &shimSpriteOnLostDevice},    // #12 ★ 0x499C80
        {0x34u, "OnResetDevice", Conv::Stdcall, 0, &shimSpriteOnResetDevice},  // #13 ★ 0x4A8634
    };
    return methods;
}

}  // namespace

std::uint32_t createD3dxSurfaceObject(X86& cpu, std::uint32_t user, std::uint32_t owner) {
    if (!cpu.com) return 0;
    // ★ A 方案（`REQ-01M6ZZZ6PEVSFB93C6VJPKC04C`）：`owner != 0` 的 surface **替父纹理持一份引用**。
    //   ① 加引用必须在"父纹理 refcount 归零"**之前**：`GetSurfaceLevel` 是在纹理对象的方法里被调到的，
    //      那一刻它的 refcount ≥ 1 ⇒ 这里加完就再也不会归零，直到 surface 自己也到 0。
    //      ⛔ 不是"建完 surface 再补"：那个窗口里任何一次 Release 都会让 `destroyTextureState` 把像素块 +
    //      `'TEX1'` 状态块回收掉 ⇒ surface 的 `user` 当场变成悬空 EA（本单的缺陷本身）。
    //   ② 还回去的地点是 `ComRegistry::releaseImpl` 的唯一收敛点（`owns_owner`）：内部裸
    //      `cpu.com->release` 与 guest 走 iface `Release` 都会到那里。⛔ 不能只挂在 `shimSurfaceRelease`
    //      上 —— 那会让"流水线内部裸 release"漏掉这份引用（= 纹理永远到不了 0）。
    //   ③ `owner == 0`（后缓冲表面）什么都不做：它没有可持的父对象，布局 / refcount 一字不改。
    if (owner != 0 && !cpu.com->addRef(cpu, owner)) {
        d3dxWarn("createD3dxSurfaceObject", "对父对象 0x%08X addRef 失败（不在对象表里？）⇒ 不建 surface",
                 owner);
        return 0;
    }
    const std::uint32_t obj = cpu.com->createObject(cpu, "IDirect3DSurface9", surfaceMethods(), user, nullptr,
                                                    owner, /*owns_owner=*/owner != 0);
    // 建对象失败 ⇒ 把刚加的那一份还回去（否则父纹理凭空多 1 个引用，谁都减不掉 = 静默泄漏）。
    if (obj == 0 && owner != 0) cpu.com->release(cpu, owner);
    return obj;
}

void registerD3DX9Shims(ShimRegistry& shims) {
    // 本安装的 d3dx9 导入表（`.data` 0x51D498 起 19 条）：本簇实现下面这些。
    // ★ 静态导入 ⇒ 走 `add`（IAT 桩页），⛔ 不是 dynamic_exports 那条动态解析路。
    //   其余条目仍在 register_import_gaps.cpp（派生文件，由 `pnpm tools lift sync --write` 重生）。
    shims.add("d3dx9_43", "D3DXCreateSprite", Conv::Stdcall, 2, &shimD3DXCreateSprite);
    // ---- 纹理簇（M2；boot 越过 sprite 后的下一条 unimplemented）----
    shims.add("d3dx9_43", "D3DXCreateTexture", Conv::Stdcall, 8, &shimD3DXCreateTexture);
    // TITLE Live2D 纹理（`.lst` 0x478398）走这条；先做 header 级 stub，后续再解码像素。
    shims.add("d3dx9_43", "D3DXCreateTextureFromFileInMemory", Conv::Stdcall, 4,
              &shimD3DXCreateTextureFromFileInMemory);
    // ---- D3DX math：只登记**确定写对**的几条（行向量约定）；其余仍是派生缺口 ----
    shims.add("d3dx9_43", "D3DXMatrixMultiply", Conv::Stdcall, 3, &shimD3DXMatrixMultiply);
    shims.add("d3dx9_43", "D3DXMatrixScaling", Conv::Stdcall, 4, &shimD3DXMatrixScaling);
    shims.add("d3dx9_43", "D3DXMatrixTranslation", Conv::Stdcall, 4, &shimD3DXMatrixTranslation);
    shims.add("d3dx9_43", "D3DXMatrixRotationAxis", Conv::Stdcall, 3, &shimD3DXMatrixRotationAxis);
    shims.add("d3dx9_43", "D3DXMatrixRotationYawPitchRoll", Conv::Stdcall, 4,
              &shimD3DXMatrixRotationYawPitchRoll);
    shims.add("d3dx9_43", "D3DXMatrixOrthoLH", Conv::Stdcall, 5, &shimD3DXMatrixOrthoLH);
    shims.add("d3dx9_43", "D3DXMatrixPerspectiveFovLH", Conv::Stdcall, 5, &shimD3DXMatrixPerspectiveFovLH);
    shims.add("d3dx9_43", "D3DXMatrixLookAtLH", Conv::Stdcall, 4, &shimD3DXMatrixLookAtLH);
    shims.add("d3dx9_43", "D3DXVec4Transform", Conv::Stdcall, 3, &shimD3DXVec4Transform);
    shims.add("d3dx9_43", "D3DXVec3Normalize", Conv::Stdcall, 2, &shimD3DXVec3Normalize);
}

}  // namespace lift
