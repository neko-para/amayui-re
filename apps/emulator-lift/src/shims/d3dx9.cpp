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
#include <vector>

#include "runtime/com.h"
#include "runtime/log.h"
#include "runtime/d3d9.h"
#include "runtime/d3dx9_sprite.h"
#include "runtime/exec.h"
#include "runtime/math.h"
#include "runtime/memory.h"
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

void d3dxLog(const char* method, const char* fmt, ...) {
    char prefix[256];
    std::snprintf(prefix, sizeof(prefix), "[d3dx9] %s: ", method ? method : "?");
    va_list ap;
    va_start(ap, fmt);
    logV(LogLevel::Info, LogD3dx9, prefix, fmt, ap);
    va_end(ap);
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
    const std::uint32_t user = cpu.com->userOf(self);
    const bool last = cpu.com->refCount(self) == 1u;
    if (!cpu.com->release(cpu, self)) {
        call.result = 0;
        return;
    }
    if (last && user != 0) d3d9::destroyTextureState(cpu, user);
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
    const std::uint32_t surface = createD3dxSurfaceObject(cpu, state);
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
        // 后缓冲 surface 的 state 就是设备状态块；⛔ 不能按纹理锁字段去写（会改坏设备状态）。
        d3dxTrap(cpu, "IDirect3DSurface9::LockRect(+0x34)",
                 "后缓冲 surface 的 LockRect 尚未实现（⛔ 不把锁字段写进设备状态块）");
        call.result = d3d9::kInvalidCall;
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
        d3dxTrap(cpu, "IDirect3DSurface9::UnlockRect(+0x38)",
                 "后缓冲 surface 的 UnlockRect 尚未实现（⛔ 不把锁字段写进设备状态块）");
        call.result = d3d9::kInvalidCall;
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
    const std::uint32_t state =
        d3d9::createTextureState(cpu, device, width, height, levels, usage, format, pool, &why);
    if (state == 0) {
        d3dxTrap(cpu, "D3DXCreateTexture", "%s", why.c_str());
        call.result = d3d9::kFail;
        return;
    }
    const std::uint32_t obj = cpu.com->createObject(cpu, "IDirect3DTexture9", textureMethods(), state);
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
    const std::uint32_t state = d3dx9::createSpriteState(cpu, device, &why);
    if (state == 0) {
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

std::uint32_t createD3dxSurfaceObject(X86& cpu, std::uint32_t user) {
    if (!cpu.com) return 0;
    return cpu.com->createObject(cpu, "IDirect3DSurface9", surfaceMethods(), user);
}

void registerD3DX9Shims(ShimRegistry& shims) {
    // 本安装的 d3dx9 导入表（`.data` 0x51D498 起 19 条）：本簇实现下面这些。
    // ★ 静态导入 ⇒ 走 `add`（IAT 桩页），⛔ 不是 dynamic_exports 那条动态解析路。
    //   其余条目仍在 register_import_gaps.cpp（派生文件，由 `pnpm tools lift sync --write` 重生）。
    shims.add("d3dx9_43", "D3DXCreateSprite", Conv::Stdcall, 2, &shimD3DXCreateSprite);
    // ---- 纹理簇（M2；boot 越过 sprite 后的下一条 unimplemented）----
    shims.add("d3dx9_43", "D3DXCreateTexture", Conv::Stdcall, 8, &shimD3DXCreateTexture);
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
