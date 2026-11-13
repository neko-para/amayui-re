// emulator-lift · GPU 光栅化「入队 + 一次 flush」守卫（合批 / 屏障）
//
// 判据（对应性能改造的三条语义要点 + 屏障清单）：
//   ① `rasterDirty()` = 队列非空 **或** 已提交未读回；N 次 Draw 只 flush **1** 次；
//   ② Clear(TARGET) / 纹理 LockRect / SetRenderTarget 在队列非空时都会把队列排空
//      （`rasterDirty()==false`，且读回的像素 = 期望）；
//   ③ 目标身份：同尺寸换 surface 也能被后端认出来（Prepare 带 guest 像素基址）。
//
// ★ 两档：
//   · 缺省（`gpu-batch-order`，pure）：**Mock GpuRaster** + D3D9 夹具，不碰 SDL/GPU。
//     它守的是「调用序」：N 次 Draw ⇒ queue N 次、flush 1 次；每个屏障 ⇒ flush + sync 各 1 次。
//     ⛔ 这条会在"把 flush 去掉"时当场红（队列不清空 ⇒ rasterDirty 仍为 true）。
//   · `--gpu`（`gpu-batch-barrier`）：真的 SDL_GPU 后端（dummy 视频 + 隐藏窗口），
//     用真实的 Clear / LockRect / SetRenderTarget 验证**像素**；没有 GPU 设备 ⇒ SKIP 77。
//     ⛔ 不拿"夹具绿了"冒充真后端绿。
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "check.h"
#include "runtime/d3d9.h"
#include "runtime/d3d9_pipeline.h"
#include "runtime/d3dx9_sprite.h"
#include "runtime/gpu_raster.h"
#include "runtime/host_encoding.h"
#include "runtime/machine.h"
#include "runtime/present_sdl.h"
#include "runtime/surface.h"
#include "shims/register.h"
#include "subprocess_skip.h"

#include <SDL3/SDL.h>

namespace {

constexpr std::uint32_t kCodeBase = 0x00401000u;
constexpr std::uint32_t kCodeStride = 0x1000u;
constexpr std::uint32_t kDataBase = 0x00420000u;
constexpr std::uint32_t kDataStride = 0x1000u;
constexpr std::uint32_t kStubPage = 0x7FF80000u;
constexpr std::uint32_t kStubSlots = 0x1000u;
constexpr std::uint32_t kProcDirect3DCreate9 = 0x7FF81000u;
constexpr std::uint32_t kProcD3DXCreateSprite = 0x7FF81020u;
constexpr std::uint32_t kProcD3DXCreateTexture = 0x7FF81040u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
constexpr std::uint32_t kFixtureHeapBytes = 0x00100000u;

// 设备槽偏移（与 shims/d3d9.cpp 的方法表一致）
constexpr std::uint32_t kOffPresent = 0x44u;
constexpr std::uint32_t kOffSetRenderTarget = 0x94u;
constexpr std::uint32_t kOffBeginScene = 0xA4u;
constexpr std::uint32_t kOffEndScene = 0xA8u;
constexpr std::uint32_t kOffClear = 0xACu;
constexpr std::uint32_t kOffSetViewport = 0xBCu;
constexpr std::uint32_t kOffSetRenderState = 0xE4u;
constexpr std::uint32_t kOffSetTexture = 0x104u;
constexpr std::uint32_t kOffSetSamplerState = 0x114u;
constexpr std::uint32_t kOffDrawPrimitiveUP = 0x14Cu;
constexpr std::uint32_t kOffSetFVF = 0x164u;
constexpr std::uint32_t kOffTextureGetSurfaceLevel = 0x48u;
constexpr std::uint32_t kOffTextureLockRect = 0x4Cu;
constexpr std::uint32_t kOffTextureUnlockRect = 0x50u;
// ID3DXSprite 的槽偏移（`d3dx9_sprite.h` 的取证表：Begin=+0x20 / Draw=+0x24 / End=+0x2C）
constexpr std::uint32_t kOffSpriteBegin = 0x20u;
constexpr std::uint32_t kOffSpriteDraw = 0x24u;
constexpr std::uint32_t kOffSpriteEnd = 0x2Cu;

constexpr std::uint32_t kFvfXyzRhwDiffuse = 0x0044u;  // D3DFVF_XYZRHW | D3DFVF_DIFFUSE
constexpr std::uint32_t kFvfXyzRhwDiffuseTex1 = 0x0144u;  // + D3DFVF_TEX1（顶点 28 B）

struct Asm {
    std::vector<std::uint8_t> b;
    std::uint32_t ea(std::uint32_t base) const { return base + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void movEaxImm(std::uint32_t v) {
        u8(0xB8);
        u32(v);
    }
    void callRel(std::uint32_t base, std::uint32_t target) {
        const std::uint32_t at = ea(base);
        u8(0xE8);
        u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(target - (at + 5))));
    }
    void callMemEaxDisp8(std::uint8_t d) {
        u8(0xFF);
        u8(0x50);
        u8(d);
    }
    void callMemEaxDisp32(std::uint32_t d) {
        u8(0xFF);
        u8(0x90);
        u32(d);
    }
    void ret() { u8(0xC3); }
};

struct Fixture {
    lift::Machine m;
    std::string err;
    std::uint32_t code_ea = kCodeBase;
    std::uint32_t data_ea = kDataBase;
    lift::d3d9::DeviceInfo device_info;

    bool boot(bool with_d3dx) {
        if (!m.initBare(&err)) return false;
        if (!m.prepareStack(&err)) return false;
        if (!m.heap().init(&m.mem(), m.layout().heap_base, kFixtureHeapBytes)) return false;
        m.shims().configureStubPage(kStubPage, kStubSlots);
        lift::registerD3D9Shims(m.shims());
        if (!fixStub("d3d9", "Direct3DCreate9", kProcDirect3DCreate9)) return false;
        if (with_d3dx) {
            lift::registerD3DX9Shims(m.shims());
            if (!fixStub("d3dx9_43", "D3DXCreateSprite", kProcD3DXCreateSprite)) return false;
            if (!fixStub("d3dx9_43", "D3DXCreateTexture", kProcD3DXCreateTexture)) return false;
        }
        return true;
    }

    bool fixStub(const char* module, const char* name, std::uint32_t ea) {
        lift::ShimDef* d = m.shims().findMutable(module, name);
        if (!d) return false;
        d->stub_ea = ea;
        m.cache().installShim(ea, d->id);
        return true;
    }

    std::uint32_t placeData(const void* p, std::size_t n) {
        const std::uint32_t ea = data_ea;
        const std::uint32_t aligned = (static_cast<std::uint32_t>(n) + 15u) & ~15u;
        if (!m.placeData(ea, p, n, &err)) return 0;
        data_ea += (aligned < kDataStride) ? aligned : kDataStride;
        return ea;
    }
    std::uint32_t placeU32(std::uint32_t v) { return placeData(&v, 4); }
    std::uint32_t placeBytes(const std::vector<std::uint8_t>& v) { return placeData(v.data(), v.size()); }
    std::uint32_t placeFloats(const std::vector<float>& v) { return placeData(v.data(), v.size() * sizeof(float)); }
    std::uint32_t placeRect(std::int32_t l, std::int32_t t, std::int32_t r, std::int32_t b) {
        const std::int32_t v[4] = {l, t, r, b};
        return placeData(v, sizeof(v));
    }
    std::uint32_t placeVec3(float x, float y, float z) {
        const float v[3] = {x, y, z};
        return placeData(v, sizeof(v));
    }

    std::uint32_t runAsm(Asm& a) {
        const std::uint32_t ea = code_ea;
        if (!m.placeCode(ea, a.b.data(), a.b.size(), &err)) return 0;
        code_ea += kCodeStride;
        m.cpu().eip = ea;
        m.cpu().esp() = m.layout().stack_top - 4u;
        m.mem().wr32(m.cpu().esp(), kSentinel);
        m.cpu().running = true;
        lift::RunOptions o;
        o.until_ea = kSentinel;
        o.max_insns = 200000;
        const lift::RunResult r = m.run(o);
        if (r.reason == lift::StopReason::UntilEa) return m.cpu().eax();
        // ★ 白名单 = "调用返回了 EAX"（不是"成功"）；新增原因必须在内，见 tests/d3dx9.cpp 同款注释。
        if (r.reason == lift::StopReason::Stopped || r.reason == lift::StopReason::Unimplemented ||
            r.reason == lift::StopReason::Trap || r.reason == lift::StopReason::ResourceExhausted) {
            return m.cpu().eax();
        }
        return 0;
    }

    // guest 侧的 COM 调用：`mov eax,[vtbl]; push args…; push obj; call [eax+off]`
    std::uint32_t method(std::uint32_t obj, std::uint32_t offset, const std::vector<std::uint32_t>& args) {
        const std::uint32_t vtbl = m.mem().rd32(obj);
        Asm a;
        a.movEaxImm(vtbl);
        for (std::size_t i = args.size(); i > 0; --i) a.pushImm(args[i - 1]);
        a.pushImm(obj);
        if (offset <= 0x7Fu) a.callMemEaxDisp8(static_cast<std::uint8_t>(offset));
        else a.callMemEaxDisp32(offset);
        a.ret();
        return runAsm(a);
    }

    std::uint32_t createDirect3D() {
        Asm a;
        a.pushImm(lift::d3d9::kSdkVersion);
        a.callRel(code_ea, kProcDirect3DCreate9);
        a.ret();
        return runAsm(a);
    }

    struct Device {
        std::uint32_t object = 0;
        std::uint32_t state = 0;
        lift::d3d9::DeviceInfo info;
    };

    bool createDevice(std::uint32_t d3d9_obj, int width, int height, std::uint32_t format, Device* out) {
        std::vector<std::uint8_t> pp(lift::d3d9::kPresentParamsBytes, 0);
        auto wr = [&pp](std::uint32_t off, std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                pp[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        };
        wr(lift::d3d9::kPpBackBufferWidth, static_cast<std::uint32_t>(width));
        wr(lift::d3d9::kPpBackBufferHeight, static_cast<std::uint32_t>(height));
        wr(lift::d3d9::kPpBackBufferFormat, format);
        wr(lift::d3d9::kPpBackBufferCount, 1u);
        wr(lift::d3d9::kPpSwapEffect, 1u);
        wr(lift::d3d9::kPpHDeviceWindow, 0x00001234u);
        wr(lift::d3d9::kPpWindowed, 1u);
        wr(lift::d3d9::kPpAutoDepthStencilFormat, 75u);
        // ★ 真机实测值（`0x498F1B mov edx,1` → `0x498F98 [esi+440h]`）：后缓冲必须可锁，
        //   否则 `IDirect3DSurface9::LockRect` 按契约答 D3DERR_INVALIDCALL。
        wr(lift::d3d9::kPpFlags, lift::d3d9::kPresentFlagLockableBackbuffer);
        wr(lift::d3d9::kPpPresentationInterval, 0x80000000u);
        const std::uint32_t pp_ea = placeBytes(pp);
        const std::uint32_t out_ea = placeU32(0u);
        const std::uint32_t hr = method(d3d9_obj, 0x40u, {0u, 1u, 0x00001234u, 0x44u, pp_ea, out_ea});
        LIFT_CHECK_EQ_U32(hr, lift::d3d9::kOk);
        out->object = m.mem().rd32(out_ea);
        LIFT_CHECK_NE_U32(out->object, 0u);
        if (out->object == 0) return false;
        out->state = m.cpu().com->userOf(out->object);
        LIFT_CHECK_NE_U32(out->state, 0u);
        if (out->state == 0) return false;
        std::string why;
        if (!lift::d3d9::deviceInfoOfState(m.cpu(), out->state, &out->info, &why)) {
            lift_test::failf(__FILE__, __LINE__, "deviceInfoOfState 失败：%s", why.c_str());
            return false;
        }
        device_info = out->info;
        return true;
    }

    // D3DXCreateTexture(device, w, h, 1, Usage, format, POOL_DEFAULT=0, &out)
    // ★ `usage` 缺省 0：RTT 用例要能分别覆盖"给了 D3DUSAGE_RENDERTARGET(=1)"与"没给（只记账放行）"
    //   两条路（`REQ-01M79MSMWGFEKF8J7C7VYWS4YP`；引擎实测那张缩略图 RT 是 Usage=0x1）。
    std::uint32_t createTexture(std::uint32_t device, std::uint32_t w, std::uint32_t h, std::uint32_t format,
                                std::uint32_t* out_obj, std::uint32_t usage = 0u) {
        const std::uint32_t out_ea = placeU32(0xDEADBEEFu);
        Asm a;
        a.pushImm(out_ea);
        a.pushImm(0u);
        a.pushImm(format);
        a.pushImm(usage);
        a.pushImm(1u);
        a.pushImm(h);
        a.pushImm(w);
        a.pushImm(device);
        a.callRel(code_ea, kProcD3DXCreateTexture);
        a.ret();
        const std::uint32_t hr = runAsm(a);
        if (out_obj) *out_obj = m.mem().rd32(out_ea);
        return hr;
    }

    // D3DXCreateSprite(device, &out)（+0x20 Begin / +0x24 Draw / +0x2C End）
    std::uint32_t createSprite(std::uint32_t device, std::uint32_t* out_obj) {
        const std::uint32_t out_ea = placeU32(0xDEADBEEFu);
        Asm a;
        a.pushImm(out_ea);
        a.pushImm(device);
        a.callRel(code_ea, kProcD3DXCreateSprite);
        a.ret();
        const std::uint32_t hr = runAsm(a);
        if (out_obj) *out_obj = m.mem().rd32(out_ea);
        return hr;
    }
    std::uint32_t spriteBegin(std::uint32_t sprite, std::uint32_t flags) {
        return method(sprite, kOffSpriteBegin, {flags});
    }
    // ID3DXSprite::Draw(pTexture, pSrcRect, pCenter, pPosition, Color)
    std::uint32_t spriteDraw(std::uint32_t sprite, std::uint32_t texture, std::uint32_t rect_ea,
                             std::uint32_t center_ea, std::uint32_t pos_ea, std::uint32_t color) {
        return method(sprite, kOffSpriteDraw, {texture, rect_ea, center_ea, pos_ea, color});
    }
    std::uint32_t spriteEnd(std::uint32_t sprite) { return method(sprite, kOffSpriteEnd, {}); }

    // IDirect3DDevice9::CreateOffscreenPlainSurface(w, h, format, D3DPOOL_SYSTEMMEM=2, &out, NULL)
    // ★ 读回目标（`GetRenderTargetData` 的 pDestSurface 必须是 离屏 plain surface + 同尺寸/格式）。
    std::uint32_t createOffscreenPlainSurface(std::uint32_t w, std::uint32_t h, std::uint32_t format) {
        const std::uint32_t out_ea = placeU32(0xDEADBEEFu);
        const std::uint32_t hr = method(device_info.object, 0x90u, {w, h, format, 2u, out_ea, 0u});
        LIFT_CHECK_EQ_U32(hr, 0u);
        return m.mem().rd32(out_ea);
    }
    // IDirect3DDevice9::GetRenderTargetData(pRenderTarget, pDestSurface)（+0x80，引擎 0x4A05D2）
    std::uint32_t getRenderTargetData(std::uint32_t src_surface, std::uint32_t dst_surface) {
        return method(device_info.object, 0x80u, {src_surface, dst_surface});
    }

    std::uint32_t textureSurface(std::uint32_t texture) {
        const std::uint32_t out_ea = placeU32(0xDEADBEEFu);
        const std::uint32_t hr = method(texture, kOffTextureGetSurfaceLevel, {0u, out_ea});
        LIFT_CHECK_EQ_U32(hr, 0u);
        return m.mem().rd32(out_ea);
    }

    std::uint32_t setFvf(std::uint32_t fvf) { return method(device_info.object, kOffSetFVF, {fvf}); }
    std::uint32_t setRs(std::uint32_t state, std::uint32_t value) {
        return method(device_info.object, kOffSetRenderState, {state, value});
    }
    // XYZRHW|DIFFUSE（20 B）= x,y,z,rhw + D3DCOLOR：点/线守卫与 α 聚合守卫共用这一份。
    std::uint32_t placeXyzRhwDiffuse(const std::vector<std::array<float, 4>>& pos, std::uint32_t color) {
        std::vector<std::uint8_t> bytes;
        for (const auto& p : pos) {
            for (int k = 0; k < 4; ++k) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &p[k], sizeof(bits));
                for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((bits >> (8 * b)) & 0xFF));
            }
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((color >> (8 * b)) & 0xFF));
        }
        return placeBytes(bytes);
    }
    std::uint32_t drawPrimitiveUP(std::uint32_t prim, std::uint32_t prim_count, std::uint32_t data,
                                  std::uint32_t stride) {
        return method(device_info.object, kOffDrawPrimitiveUP, {prim, prim_count, data, stride});
    }
    std::uint32_t setTexture(std::uint32_t stage, std::uint32_t texture) {
        return method(device_info.object, kOffSetTexture, {stage, texture});
    }
    std::uint32_t setSampler(std::uint32_t stage, std::uint32_t type, std::uint32_t value) {
        return method(device_info.object, kOffSetSamplerState, {stage, type, value});
    }
    std::uint32_t beginScene() { return method(device_info.object, kOffBeginScene, {}); }
    std::uint32_t endScene() { return method(device_info.object, kOffEndScene, {}); }

    std::uint32_t setViewport(std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
        std::vector<std::uint8_t> vp(24, 0);
        auto wr = [&vp](std::uint32_t off, std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                vp[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        };
        wr(0u, x);
        wr(4u, y);
        wr(8u, w);
        wr(12u, h);
        wr(16u, 0x00000000u);
        wr(20u, 0x3F800000u);
        return method(device_info.object, kOffSetViewport, {placeBytes(vp)});
    }

    std::uint32_t drawTriangle(float scale, std::uint32_t color) {
        const std::vector<std::array<float, 4>> xyz = {
            {0.0f, 0.0f, 0.0f, 1.0f}, {scale, 0.0f, 0.0f, 1.0f}, {0.0f, scale, 0.0f, 1.0f}};
        std::vector<std::uint8_t> bytes;
        for (std::size_t i = 0; i < xyz.size(); ++i) {
            for (int k = 0; k < 4; ++k) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &xyz[i][k], sizeof(bits));
                for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((bits >> (8 * b)) & 0xFF));
            }
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((color >> (8 * b)) & 0xFF));
        }
        const std::uint32_t data = placeBytes(bytes);
        return method(device_info.object, kOffDrawPrimitiveUP, {lift::d3d9::kPtTriangleList, 1u, data, 20u});
    }

    // XYZRHW|DIFFUSE|TEX1：x,y,z,rhw,diffuse,u,v = 28 B（与 d3d9_pipeline 夹具同形）
    struct TexVertex {
        float x, y, z, rhw;
        std::uint32_t color;
        float u, v;
    };
    std::uint32_t placeTexVertices(const std::vector<TexVertex>& verts) {
        std::vector<std::uint8_t> bytes;
        auto pushF = [&bytes](float f) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &f, sizeof(bits));
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((bits >> (8 * b)) & 0xFF));
        };
        auto pushU = [&bytes](std::uint32_t v) {
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((v >> (8 * b)) & 0xFF));
        };
        for (const TexVertex& v : verts) {
            pushF(v.x);
            pushF(v.y);
            pushF(v.z);
            pushF(v.rhw);
            pushU(v.color);
            pushF(v.u);
            pushF(v.v);
        }
        return placeBytes(bytes);
    }
    // 覆盖屏幕 [0,8)×[0,8) 的纹理四边形（UV 0..1）；数据由 placeTexVertices 给。
    std::uint32_t drawTexturedQuad(std::uint32_t data) {
        return method(device_info.object, kOffDrawPrimitiveUP, {lift::d3d9::kPtTriangleList, 2u, data, 28u});
    }

    // 屏幕空间四边形（XYZRHW|DIFFUSE，2 个三角形）；**z 由调用方给** —— 深度守卫要它。
    std::uint32_t drawQuadZ(float x0, float y0, float x1, float y1, float z, std::uint32_t color) {
        const float xs[6] = {x0, x1, x0, x1, x1, x0};
        const float ys[6] = {y0, y0, y1, y0, y1, y1};
        std::vector<std::uint8_t> bytes;
        auto pushF = [&bytes](float f) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &f, sizeof(bits));
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((bits >> (8 * b)) & 0xFF));
        };
        for (int i = 0; i < 6; ++i) {
            pushF(xs[i]);
            pushF(ys[i]);
            pushF(z);
            pushF(1.0f);
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((color >> (8 * b)) & 0xFF));
        }
        const std::uint32_t data = placeBytes(bytes);
        return method(device_info.object, kOffDrawPrimitiveUP, {lift::d3d9::kPtTriangleList, 2u, data, 20u});
    }

    std::uint32_t clearTo(std::uint32_t color) {
        return method(device_info.object, kOffClear, {0u, 0u, 3u, color, 0x3F800000u, 0u});
    }

    // 只清颜色（D3DCLEAR_TARGET，**不带** ZBUFFER）——用来证明"清深度请求不是无条件的"。
    std::uint32_t clearTargetOnly(std::uint32_t color) {
        return method(device_info.object, kOffClear, {0u, 0u, 1u, color, 0x3F800000u, 0u});
    }

    // 只清深度（D3DCLEAR_ZBUFFER）——D3D9 里这是**唯一**会清 GPU 深度附件的那条路。
    std::uint32_t clearDepthOnly() {
        return method(device_info.object, kOffClear, {0u, 0u, 2u, 0u, 0x3F800000u, 0u});
    }

    // 只清一块矩形（D3DRECT = 4 个 LONG）
    std::uint32_t clearRect(int x0, int y0, int x1, int y1, std::uint32_t color) {
        std::vector<std::uint8_t> rect(16, 0);
        const int vals[4] = {x0, y0, x1, y1};
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t v = static_cast<std::uint32_t>(vals[i]);
            for (int b = 0; b < 4; ++b) rect[static_cast<std::size_t>(i) * 4u + static_cast<std::size_t>(b)] =
                static_cast<std::uint8_t>((v >> (8 * b)) & 0xFF);
        }
        const std::uint32_t ea = placeBytes(rect);
        return method(device_info.object, kOffClear, {1u, ea, 3u, color, 0x3F800000u, 0u});
    }

    std::uint32_t present() { return method(device_info.object, kOffPresent, {0u, 0u, 0u, 0u}); }

    // ★ 实参是 **D3D9** 的 `(RenderTargetIndex, pRenderTarget)`：RT0 + 表面；`surface == 0` ⇒ 后缓冲。
    //   ⛔ 不是 D3D8 的 `(pRenderTarget, pNewZStencil)`（REQ-01M780VKKRFHJEX6VM74DRCYBM）。
    std::uint32_t setRenderTarget(std::uint32_t surface) {
        return method(device_info.object, kOffSetRenderTarget, {0u, surface});
    }

    // 后缓冲 surface（GetBackBuffer(0,0,MONO,&out)）
    std::uint32_t backBufferSurface() {
        const std::uint32_t out = placeU32(0xDEADBEEFu);
        const std::uint32_t hr = method(device_info.object, 0x48u, {0u, 0u, 0u, out});
        LIFT_CHECK_EQ_U32(hr, 0u);
        return m.mem().rd32(out);
    }
    // IDirect3DSurface9::LockRect(this, pLockedRect, pRect, Flags)；`args[0]` 是第一个实参。
    std::uint32_t surfaceLockRect(std::uint32_t surface, std::uint32_t rect_ea, std::uint32_t* locked_rect_ea) {
        const std::uint32_t lr = placeBytes(std::vector<std::uint8_t>(8u, 0));
        const std::uint32_t hr = method(surface, 0x34u, {lr, rect_ea, 0u});
        if (locked_rect_ea) *locked_rect_ea = lr;
        return hr;
    }
    std::uint32_t surfaceUnlockRect(std::uint32_t surface) { return method(surface, 0x38u, {}); }

    std::uint32_t textureLockRect(std::uint32_t texture, std::uint32_t* locked_rect_ea) {
        const std::vector<std::uint8_t> zero(8, 0);
        const std::uint32_t lr = placeBytes(zero);  // D3DLOCKED_RECT = { INT Pitch; void* pBits; }
        const std::uint32_t hr = method(texture, kOffTextureLockRect, {0u, lr, 0u, 0u});
        if (locked_rect_ea) *locked_rect_ea = lr;
        return hr;
    }
    std::uint32_t textureUnlockRect(std::uint32_t texture) {
        return method(texture, kOffTextureUnlockRect, {0u});
    }

    // 带 RECT 的 LockRect（方案 C 的"写区"输入就是它）
    std::uint32_t textureLockRectRect(std::uint32_t texture, int x0, int y0, int x1, int y1,
                                      std::uint32_t* locked_rect_ea) {
        std::vector<std::uint8_t> rect(16, 0);
        const int vals[4] = {x0, y0, x1, y1};
        for (int i = 0; i < 4; ++i) {
            const std::uint32_t v = static_cast<std::uint32_t>(vals[i]);
            for (int b = 0; b < 4; ++b)
                rect[static_cast<std::size_t>(i) * 4u + static_cast<std::size_t>(b)] =
                    static_cast<std::uint8_t>((v >> (8 * b)) & 0xFF);
        }
        const std::uint32_t rect_ea = placeBytes(rect);
        const std::vector<std::uint8_t> zero(8, 0);
        const std::uint32_t lr = placeBytes(zero);
        const std::uint32_t hr = method(texture, kOffTextureLockRect, {0u, lr, rect_ea, 0u});
        if (locked_rect_ea) *locked_rect_ea = lr;
        return hr;
    }

    // 把整张 32bpp 纹理填成一个 BGRA 颜色（B,G,R,X）
    bool fillTexture(std::uint32_t texture, std::uint8_t b, std::uint8_t g, std::uint8_t r) {
        lift::d3d9::TextureInfo ti;
        std::string why;
        if (!lift::d3d9::textureInfo(m.cpu(), texture, &ti, &why)) return false;
        lift::GuestSurface* gs = m.surfaces().find(ti.arena);
        if (gs == nullptr || gs->format.bytesPerPixel() != 4u) return false;
        std::uint8_t* px = m.surfaces().pixels(*gs);
        for (std::uint32_t yy = 0; yy < gs->height; ++yy) {
            for (std::uint32_t xx = 0; xx < gs->width; ++xx) {
                std::uint8_t* p = px + static_cast<std::size_t>(yy) * gs->pitch +
                                  static_cast<std::size_t>(xx) * 4u;
                p[0] = b;
                p[1] = g;
                p[2] = r;
                p[3] = 0;
            }
        }
        return true;
    }

    // 生产者视角的 GpuRasterTexture（含方案 C 的写区/版本链）——与 d3dx9_sprite / d3d9_pipeline
    // 的填充口径一致；⛔ 测试里不复制 d3d9 的内部偏移。
    bool textureView(std::uint32_t texture, lift::GpuRasterTexture* out) {
        lift::d3d9::TextureInfo ti;
        std::string why;
        if (!lift::d3d9::textureInfo(m.cpu(), texture, &ti, &why)) return false;
        lift::GuestSurface* gs = m.surfaces().find(ti.arena);
        if (gs == nullptr) return false;
        *out = lift::GpuRasterTexture{};
        out->pixels = m.surfaces().pixels(*gs);
        out->pitch = gs->pitch;
        out->width = static_cast<int>(gs->width);
        out->height = static_cast<int>(gs->height);
        out->format = gs->format;
        out->key = out->pixels;
        out->generation = ti.version;
        out->serial = ti.serial;
        out->content_change_known = ti.write_version != 0 && ti.write_version == ti.version;
        out->content_change_rect = lift::GpuTexelRect{ti.write_rect[0], ti.write_rect[1],
                                                      ti.write_rect[2] - ti.write_rect[0],
                                                      ti.write_rect[3] - ti.write_rect[1]};
        out->content_change_version = ti.write_version;
        out->content_change_prev_version = ti.prev_write_version;
        return true;
    }

    std::uint32_t pixelAt(int x, int y) {
        return m.mem().rd32(device_info.arena + static_cast<std::uint32_t>(y) * device_info.pitch +
                            static_cast<std::uint32_t>(x) * 4u);
    }
};

// ---------------------------------------------------------------------------------------
// Mock 后端：只记调用序，不画像素（pure 档；不依赖 SDL / GPU）
// ---------------------------------------------------------------------------------------
// ★ 方案 C 的切段判据在这里**与真后端共用同一份纯函数**（`gpuContentDirtyRegion` +
//   `gpuDirtyRectNeedsCut`）：mock 的唯一额外职责是记住"本段已入队、采样同一纹理的 draw"。
class MockRaster final : public lift::GpuRaster {
public:
    bool rasterActive() const override { return true; }
    const char* rasterName() const override { return "mock"; }

    bool rasterPrepare(const lift::SurfaceFormat&, int, int, std::string*, const std::uint8_t* color_base) override {
        ++prepare_count;
        if (color_base != nullptr) base_ = color_base;
        return true;
    }
    bool rasterSyncFromGuest(const std::uint8_t* pixels, std::uint32_t, int, int, std::string*) override {
        // 与真后端一致：队列非空 ⇒ guest 不是权威 ⇒ 拒绝（⛔ 静默覆盖）
        if (!queue_.empty()) return false;
        base_ = pixels;
        dirty_ = false;
        ++sync_from_count;
        return true;
    }
    bool rasterSyncToGuest(std::uint8_t*, std::uint32_t, int, int, std::string* err) override {
        // ★ 真后端的 rasterSyncToGuest 内部先 flush；这里照抄这个序，测试才能断言"flush 在 sync 前"。
        if (!rasterFlush(err)) return false;
        ++sync_to_count;
        dirty_ = false;
        return true;
    }
    // ★ 旧接口按**头文件契约** = 入队 + 立即 flush（不是只入队）。这样 production 若退回
    //   rasterDraw/rasterDrawTextured（本次要防的正是"每条 draw 各自提交"），断言会当场变红。
    bool rasterDraw(const lift::GpuRasterVertex* v, std::size_t n, const lift::GpuRasterState& st,
                    std::string* err) override {
        if (!rasterQueue(v, n, nullptr, st, err)) return false;
        return rasterFlush(err);
    }
    bool rasterDrawTextured(const lift::GpuRasterVertex* v, std::size_t n, const lift::GpuRasterTexture& t,
                            const lift::GpuRasterState& st, std::string* err) override {
        if (!rasterQueue(v, n, &t, st, err)) return false;
        return rasterFlush(err);
    }
    bool rasterQueue(const lift::GpuRasterVertex* vertices, std::size_t count, const lift::GpuRasterTexture* tex,
                     const lift::GpuRasterState&, std::string*) override {
        // 模拟"后端在入队那一刻就失败"（`REQ-01M78BF1FNNH6K8N8GEY6GQYXX` 的显式拒绝臂要它）。
        if (fail_queue) return false;
        ++queued_draws;
        // ★ 覆盖账本的批号（`REQ-01M73PZN6R3AC3RCQ8WZEA0Y0M`）：与真后端同一口径 —— 在**入队那一刻**
        //   取 `gpuRasterBatchTag()`，flush 里按条目回报（⛔ 不在入队时回报）。
        const std::uint64_t tag = lift::gpuRasterBatchTag();
        if (tex != nullptr) {
            const std::uint64_t key = lift::gpuTextureCacheKey(*tex, tex->width, tex->height);
            const lift::GpuTexelRect sample = lift::gpuDrawSampleRect(vertices, count, tex->width, tex->height);
            auto it = recorded_generation_.find(key);
            const std::uint32_t recorded = it != recorded_generation_.end() ? it->second : 0u;
            if (!(recorded != 0 && tex->generation != 0 && recorded == tex->generation)) {
                // 内容变了（或从没上传过）⇒ 与真后端走同一套脏区/切段判据
                const lift::GpuDirtyRegion dirty =
                    lift::gpuContentDirtyRegion(*tex, recorded, tex->width, tex->height);
                std::vector<lift::GpuTexelRect> samples;
                for (std::size_t i = segment_first_; i < queued_.size(); ++i) {
                    if (queued_[i].key == key) samples.push_back(queued_[i].sample);
                }
                if (lift::gpuDirtyRectNeedsCut(dirty.rect, samples.data(), samples.size())) {
                    ++cut_count;
                    segment_first_ = queued_.size();
                }
                ++upload_count;
                recorded_generation_[key] = tex->generation;
            }
            queued_.push_back(Queued{key, sample, tag, count});
        } else {
            queued_.push_back(Queued{0, lift::GpuTexelRect{}, tag, count});
        }
        queue_.push_back(tag);
        return true;
    }
    bool rasterFlush(std::string*) override {
        if (fail_flush) return false;
        ++flush_count;
        if (!queue_.empty()) {
            // ★ 只有 backend 真的"录进 render pass"才回报（`record_draws=false` 模拟
            //   "入队成功但没进 pass" ⇒ 账本的 `empty` 必须留着）。
            if (record_draws) {
                for (const std::uint64_t tag : queue_) {
                    if (tag != 0) lift::gpuLedgerNoteRecordedDraw(tag);
                }
            }
            queue_.clear();
            dirty_ = true;
        }
        queued_.clear();
        segment_first_ = 0;
        return true;
    }
    bool rasterDirty() const override { return dirty_ || !queue_.empty(); }
    const void* rasterTargetBase() const override { return base_; }
    std::size_t rasterQueuedDraws() const override { return queue_.size(); }
    void rasterDiscardTarget() override {
        ++discard_count;
        queue_.clear();
        dirty_ = false;
        base_ = nullptr;
    }
    // ★ `REQ-01M73P3GXW22Y467YGYPQSM571`：这条就是 `Clear(...,D3DCLEAR_ZBUFFER)` 的落点。
    //   转发壳（`SdlWindow`）曾经**整条没转发**，于是本计数恒 0、D32 永不再清、GPU 臂整层丢角色。
    void rasterRequestDepthClear() override { ++depth_clear_count; }
    std::size_t flush_count = 0;
    std::size_t sync_to_count = 0;
    std::size_t sync_from_count = 0;
    std::size_t prepare_count = 0;
    std::size_t discard_count = 0;
    std::size_t depth_clear_count = 0;
    std::size_t cut_count = 0;      // 方案 C：内容变更边界切段次数
    std::size_t upload_count = 0;   // 方案 C：纹理上传块数
    std::size_t queued_draws = 0;   // 累计入队 draw 条数
    bool fail_flush = false;
    bool fail_queue = false;  // 入队即失败（模拟后端拒绝该批；⛔ 不再 CPU 重放）
    // ★ 覆盖账本：true = flush 时把"真的进过 render pass"回报给账本（与真后端同口径）；
    //   false = **入队成功但不记录**（本单要能看见的那种形态）。
    bool record_draws = true;

private:
    struct Queued {
        std::uint64_t key = 0;
        lift::GpuTexelRect sample;
        std::uint64_t tag = 0;
        std::size_t vertices = 0;
    };
    std::vector<std::uint64_t> queue_;  // 每个条目的批号（与真后端的 `QueuedDraw::batch_tag` 同义）
    std::vector<Queued> queued_;
    std::size_t segment_first_ = 0;
    std::map<std::uint64_t, std::uint32_t> recorded_generation_;
    bool dirty_ = false;
    const void* base_ = nullptr;
};

// ---------------------------------------------------------------------------------------
// pure：调用序（N 次 draw → 1 次 flush；每个屏障 → flush + sync 各 1 次）
// ---------------------------------------------------------------------------------------
void caseOrder() {
    lift_test::g_current_case = "gpu-batch/order";
    Fixture f;
    LIFT_CHECK(f.boot(false));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    f.m.setGpuRaster(&mock);

    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0, 0, 64, 64), 0u);

    // ---- ① N 次 Draw ⇒ 队列 N、flush 0（Draw 只入队）----
    for (int i = 0; i < 3; ++i) LIFT_CHECK_EQ_U32(f.drawTriangle(10.0f, 0xFFFF0000u), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.rasterQueuedDraws()), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count), 0);
    LIFT_CHECK(mock.rasterDirty());  // 队列非空 ⇒ 脏

    // ---- 屏障 1：Clear(TARGET) ⇒ flush 1 次 + syncToGuest 1 次（⛔ 不是每条 draw 一次）----
    LIFT_CHECK_EQ_U32(f.clearTo(0xFF00FF00u), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.rasterQueuedDraws()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.sync_to_count), 1);
    LIFT_CHECK(!mock.rasterDirty());

    // ---- 屏障 2：Present ⇒ 再一次 flush + sync ----
    for (int i = 0; i < 2; ++i) LIFT_CHECK_EQ_U32(f.drawTriangle(10.0f, 0xFFFF0000u), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count), 1);
    LIFT_CHECK_EQ_U32(f.present(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.sync_to_count), 2);
    LIFT_CHECK(!mock.rasterDirty());

    // ---- 屏障 3：换 target（同尺寸、不同 guest 像素基址）⇒ flush + 回读旧目标 ----
    // 说明：SetRenderTarget 的真路径需要 IDirect3DSurface9 对象（纹理工厂），pure 档不建 D3DX9，
    // 所以这里直接验证后端的"身份变化"契约；SetRenderTarget 本身在 --gpu 档用真纹理覆盖。
    // （SetRenderTarget 的真路径在 --gpu 档覆盖。）
    for (int i = 0; i < 2; ++i) LIFT_CHECK_EQ_U32(f.drawTriangle(10.0f, 0xFFFF0000u), 0u);
    std::vector<std::uint8_t> other(64u * 64u * 4u, 0);
    std::string err;
    LIFT_CHECK(mock.rasterPrepare(lift::SurfaceFormat{}, 64, 64, &err, other.data()));
    const std::size_t flushes_before = mock.flush_count;
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(flushes_before), 2);
    // 模拟后端在"身份变化"时的做法：先 flush，再回读旧目标。Mock 的 Prepare 没实现这个，
    // 所以这里直接调一次 syncToGuest 并断言序（flush 在 sync 之前）。
    LIFT_CHECK(mock.rasterDirty());
    LIFT_CHECK(mock.rasterSyncToGuest(other.data(), 64u * 4u, 64, 64, &err));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count), 3);  // 2 → 3
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.sync_to_count), 3);
    LIFT_CHECK(!mock.rasterDirty());

    // ---- 屏障 4：Draw → Clear → Draw → Clear ⇒ 两次 flush（"只在最后 flush 一次"会红）----
    LIFT_CHECK_EQ_U32(f.drawTriangle(10.0f, 0xFFFF0000u), 0u);
    LIFT_CHECK_EQ_U32(f.clearTo(0xFF0000FFu), 0u);
    LIFT_CHECK_EQ_U32(f.drawTriangle(10.0f, 0xFFFF0000u), 0u);
    LIFT_CHECK_EQ_U32(f.clearTo(0xFF00FF00u), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count), 5);  // 3 → 5
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.sync_to_count), 5);

    // ---- syncFromGuest 在队列非空时必须拒绝（⛔ 静默覆盖）----
    LIFT_CHECK_EQ_U32(f.drawTriangle(10.0f, 0xFFFF0000u), 0u);
    LIFT_CHECK(!mock.rasterSyncFromGuest(nullptr, 0, 64, 64, &err));

    // ---- flush 失败必须**保留队列**（⛔ 不静默丢像素）----
    const std::size_t flush_before_fail = mock.flush_count;
    mock.fail_flush = true;
    std::string ferr;
    LIFT_CHECK(!mock.rasterFlush(&ferr));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.rasterQueuedDraws()), 1);
    LIFT_CHECK(mock.rasterDirty());
    mock.fail_flush = false;
    LIFT_CHECK(mock.rasterFlush(&ferr));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.rasterQueuedDraws()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count - flush_before_fail), 1);

    LIFT_CHECK_EQ_U32(f.endScene(), 0u);
    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch-order: driver=mock\n");
}

// ---------------------------------------------------------------------------------------
// pure：后缓冲 LockRect 前的 **GPU 屏障**（`MockRaster` 记 flush / syncToGuest）
//
// 判据（红了意味着什么）：
//   · 后缓冲**就是**当前镜像目标且有未读回的绘制 ⇒ 锁前必须 `rasterSyncToGuest`（内部先 flush）
//     —— 少了它 guest 拿到的是过期像素（备份图/画面错，且没有任何报错）；
//   · 锁的像素**不是**当前目标（当前目标是纹理 RT）⇒ 只 flush 队列，**不**把别的 target 读回来
//     —— 多读回会把纹理镜像写进后缓冲（静默写错 surface）。
//   ★ `MockRaster::rasterSyncToGuest` 内部先 `rasterFlush`（与真后端同序）⇒ "flush 在 sync 前"可断言。
//   ★ 这条**必须**在 pure CTest（`gpu-batch-order`）里：`--gpu` 那条在无 GPU 机器上 SKIP 77，
//     M5（删屏障）会静默逃逸。
void caseBackBufferLockBarrier() {
    lift_test::g_current_case = "gpu-batch/backbuffer-lock-barrier";
    Fixture f;
    LIFT_CHECK(f.boot(false));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    f.m.setGpuRaster(&mock);
    lift::GuestSurface* back = f.m.surfaces().find(dev.info.arena);
    LIFT_CHECK(back != nullptr);
    if (back == nullptr) return;
    std::uint8_t* back_px = f.m.surfaces().pixels(*back);
    std::string err;
    LIFT_CHECK(mock.rasterPrepare(back->format, back->width, back->height, &err, back_px));
    LIFT_CHECK(mock.rasterSyncFromGuest(back_px, back->pitch, back->width, back->height, &err));

    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0, 0, 64, 64), 0u);
    const std::uint32_t surf = f.backBufferSurface();
    LIFT_CHECK_NE_U32(surf, 0u);

    // ---- ① 后缓冲 = 当前镜像目标：锁前必须 flush + 读回（syncToGuest）----
    for (int i = 0; i < 2; ++i) LIFT_CHECK_EQ_U32(f.drawTriangle(10.0f, 0xFFFF0000u), 0u);
    LIFT_CHECK(mock.rasterDirty());
    const std::size_t flush0 = mock.flush_count;
    const std::size_t sync0 = mock.sync_to_count;
    std::uint32_t lr = 0;
    LIFT_CHECK_EQ_U32(f.surfaceLockRect(surf, 0u, &lr), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.sync_to_count - sync0), 1);  // ★ 读回真的发生了
    LIFT_CHECK(mock.flush_count > flush0);                                        // ★ 读回内部先 flush
    LIFT_CHECK(!mock.rasterDirty());
    LIFT_CHECK_EQ_U32(f.surfaceUnlockRect(surf), 0u);

    // ---- ② 锁的像素**不是**当前目标：只 flush 队列，不把别的 target 读回来 ----
    // ★ 这是白盒分支判据：本线的 GPU 目标只有默认后缓冲（纹理 RT 走 CPU 回退）⇒ 生产路径上
    //   "锁后缓冲时目标又是纹理"不出现；这里直接把 mock 的目标身份换成另一块缓冲，验证分支方向
    //   （⛔ 不读回别的 target —— 那会把那块镜像写进后缓冲，静默写错 surface）。
    for (int i = 0; i < 2; ++i) LIFT_CHECK_EQ_U32(f.drawTriangle(10.0f, 0xFF00FF00u), 0u);
    LIFT_CHECK(mock.rasterDirty());
    std::vector<std::uint8_t> other(64u * 64u * 4u, 0);
    LIFT_CHECK(mock.rasterPrepare(back->format, back->width, back->height, &err, other.data()));
    const std::size_t flush1 = mock.flush_count;
    const std::size_t sync1 = mock.sync_to_count;
    LIFT_CHECK_EQ_U32(f.surfaceLockRect(surf, 0u, &lr), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count - flush1), 1);   // 队列被提交
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.sync_to_count - sync1), 0);  // ⛔ 不读回别的 target
    LIFT_CHECK_EQ_U32(f.surfaceUnlockRect(surf), 0u);

    LIFT_CHECK_EQ_U32(f.endScene(), 0u);
    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch-backbuffer-lock-barrier: sync=%llu flush=%llu\n",
                static_cast<unsigned long long>(mock.sync_to_count),
                static_cast<unsigned long long>(mock.flush_count));
}

// ---------------------------------------------------------------------------------------
// pure：采样器过滤的**准入**（`MAG != MIN` ⇒ 不接受该批 + 记账）
//
// 红了意味着什么：
//   `MAG=POINT + MIN=LINEAR` 这一批**必须**不进 GPU 队列，且 `fallback_reasons` 里出现
//   「MAG==MIN」那一条。CPU 参考把两个过滤**合成一条**决策（`samplerUsesLinear`：任一 LINEAR ⇒
//   双线性；像素级证据在 `d3d9-pipeline` 的 `d3d9p/min-filter`），而 GPU 的 sampler 把 MAG / MIN
//   分开用 ⇒ 两边都"成功"、整批像素却不同，且 fallback 账**看不见**（它不是 fallback）。
//   按「CPU=oracle / GPU=product」：**要么一致，要么不接受该批** ⇒ 准入只放行 MAG == MIN。
//   ★ 同一批状态在 MAG==MIN 时**必须**进 GPU（(b)/(c)）—— 否则这条守卫可能"因为别的原因被拒"
//     而假绿：红与绿都由**过滤这一项**决定。
//   ★ 删掉 `gpuEligible` 里那条准入 ⇒ (a) 的 `queued_draws` 变成 1、(b) 的 fallback 账变空 ⇒ 当场红。
void caseSamplerFilterEligibility() {
    lift_test::g_current_case = "gpu-batch/sampler-filter-eligibility";
    lift::d3d9::gpuCoverageReset();
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    f.m.setGpuRaster(&mock);

    // 2×2 A8R8G8B8 纹理（颜色本身不参与判定：这条守的是准入，不是像素）
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    std::uint32_t locked = 0;
    LIFT_CHECK_EQ_U32(f.textureLockRect(tex, &locked), 0u);
    const std::uint32_t pitch = f.m.mem().rd32(locked + 0u);
    const std::uint32_t bits = f.m.mem().rd32(locked + 4u);
    LIFT_CHECK_NE_U32(bits, 0u);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            f.m.mem().wr32(bits + static_cast<std::uint32_t>(y) * pitch + static_cast<std::uint32_t>(x) * 4u,
                           0xFF808080u);
        }
    }
    LIFT_CHECK_EQ_U32(f.textureUnlockRect(tex), 0u);

    // 其余状态与 `d3d9p/min-filter` 相同（那一批在 CPU 上是可画的 8×8 四边形）
    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuseTex1), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0u, 0u, 64u, 64u), 0u);
    LIFT_CHECK_EQ_U32(f.setTexture(0u, tex), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampAddressU, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampAddressV, lift::d3d9::kAddressClamp), 0u);

    const std::vector<Fixture::TexVertex> quad = {
        {0.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
        {8.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {0.0f, 8.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
        {8.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {8.0f, 8.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
        {0.0f, 8.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
    };
    const std::uint32_t data = f.placeTexVertices(quad);

    // ---- (a) MAG=POINT / MIN=LINEAR：不接受该批（CPU 会按双线性采样，GPU 做不到同一条决策）----
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfLinear), 0u);
    LIFT_CHECK_EQ_U32(f.drawTexturedQuad(data), 0u);
    // ★ 覆盖账本**按 `Machine::run` 重算**（`machine.cpp`：每轮 `gpuCoverageReset()`）⇒ 夹具的
    //   每次 guest COM 调用都是一轮 ⇒ 只可能在**紧接着那次 Draw 之后**读这一批的账，之后任何一次
    //   `method()` 都会把它清零。所以这里当场取值、后面断言取到的快照。
    const std::uint64_t fallback_a = lift::d3d9::gpuCoverage().fallback_draws;
    const std::size_t reasons_a = lift::d3d9::gpuCoverage().fallback_reasons.size();
    std::string why_a;
    for (const auto& [reason, count] : lift::d3d9::gpuCoverage().fallback_reasons) {
        (void)count;
        why_a = reason;
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 0);  // ★ 一个像素都没交给 GPU
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fallback_a), 1);         // ★ 记了 1 次回退
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(reasons_a), 1);          // ★ 且只有一条原因
    LIFT_CHECK(why_a.find("MAG==MIN") != std::string::npos);

    // ---- (b) 同一批、MAG==MIN==POINT：必须进 GPU（⇒ (a) 的拒绝只来自"两个过滤不一致"）----
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(f.drawTexturedQuad(data), 0u);
    const std::uint64_t fallback_b = lift::d3d9::gpuCoverage().fallback_draws;
    const std::uint64_t gpu_b = lift::d3d9::gpuCoverage().gpu_draws;
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fallback_b), 0);  // 没多记回退
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gpu_b), 1);       // 这一批真的交给 GPU 了

    // ---- (c) MAG==MIN==LINEAR 也接受（两边都是双线性，与 CPU 的合成决策一致）----
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfLinear), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfLinear), 0u);
    LIFT_CHECK_EQ_U32(f.drawTexturedQuad(data), 0u);
    const std::uint64_t fallback_c = lift::d3d9::gpuCoverage().fallback_draws;
    const std::uint64_t gpu_c = lift::d3d9::gpuCoverage().gpu_draws;
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fallback_c), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gpu_c), 1);

    LIFT_CHECK_EQ_U32(f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setTexture(0u, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 0u);  // Release
    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch-sampler-filter-eligibility: queued=%llu fallback=%llu reason=\"%s\"\n",
                static_cast<unsigned long long>(mock.queued_draws),
                static_cast<unsigned long long>(fallback_a), why_a.c_str());
}

// ---------------------------------------------------------------------------------------
// pure：`SetRenderTarget(0, <GetBackBuffer 得到的表面>)` 必须与"绑定隐式后缓冲"**等价**
//       （`REQ-01M780VKKRFHJEX6VM74DRCYBM` 的连带缺陷）
// ---------------------------------------------------------------------------------------
// 为什么这条属于本文件：`gpuEligible` 的准入判定只有**这里**能在 pure 档观察（`MockRaster` +
//   `gpuCoverage` 账本）；`tests/d3d9_pipeline.cpp` 没有 GPU 后端，只能看流水线属性。
//
// 判据（会红的情形）：
//   ① 绑**后缓冲表面**（引擎 `0x4A521B` 那站就是这么传的 —— 一个 surface 对象，不是 NULL）
//      ⇒ 流水线把它**规范化**成 `color_surface == 0`（`color_arena` = 设备后缓冲），
//      且这一批**进 GPU**：`queued_draws` 1 / `gpu_draws` 1 / `fallback_draws` 0。
//      ★ 修前（准入只看"目标是不是 surface"）：`color_surface != 0` 恒成立 ⇒ 每一批都退回 CPU 光栅
//      （实测 `gpu=0 fallback=125`、≈3 s/帧、`save-graph` 在 5 s 窗口里只够 2 帧）⇒ ① 当场红。
//   ② **同尺寸（64×64 = 设备的尺寸）的纹理表面** ⇒ `color_surface != 0`、**不**被规范化，
//      且这一批**进 GPU**、镜像的目标基址 = **纹理像素**（`REQ-01M79MSMWGFEKF8J7C7VYWS4YP` 解禁）。
//      ★ 这条同时钉住"判据是**身份**（像素块与后缓冲同址），不是**外观**（尺寸 / 格式相同）"：
//      把 ① 的判据换成"尺寸相同" ⇒ ② 的 `rasterTargetBase() == 纹理像素` 红（目标被当成后缓冲）。
//   ★ 为什么不再有"非纹理表面"那一段：`setRenderTarget` 只让**纹理 level 0 / 后缓冲表面**绑进来
//     （后缓冲表面会被规范化），离屏 plain surface 直接被它拒成 `D3DERR_INVALIDCALL`
//     ⇒ 准入层不需要（也不该）再立一条永远为假的闸。
void caseRenderTargetBackBufferIdentity() {
    lift_test::g_current_case = "gpu-batch/render-target-backbuffer-identity";
    lift::d3d9::gpuCoverageReset();
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    f.m.setGpuRaster(&mock);

    // 纹理与后缓冲**同尺寸**（64×64）：② 才是"外观相同、身份不同"的最小对照。
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 64u, 64u, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    std::uint32_t locked = 0;
    LIFT_CHECK_EQ_U32(f.textureLockRect(tex, &locked), 0u);
    const std::uint32_t tpitch = f.m.mem().rd32(locked + 0u);
    const std::uint32_t tbits = f.m.mem().rd32(locked + 4u);
    LIFT_CHECK_NE_U32(tbits, 0u);
    if (tbits == 0) return;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            f.m.mem().wr32(tbits + static_cast<std::uint32_t>(y) * tpitch + static_cast<std::uint32_t>(x) * 4u,
                           0xFF808080u);
        }
    }
    LIFT_CHECK_EQ_U32(f.textureUnlockRect(tex), 0u);

    // 状态 = `gpu-batch/sampler-filter-eligibility` 的 (b)（MAG==MIN==POINT 的可画批次）。
    // ★ 四边形铺满 64×64 视口、UV 0..1 对**同尺寸**（64×64）的纹理 ⇒ 1:1 采样（无缩小）
    //   —— 否则会命中"`MIPFILTER=NONE` 且有缩小采样"那条**别的**准入项，① 就测不到目标身份了。
    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuseTex1), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0u, 0u, 64u, 64u), 0u);
    LIFT_CHECK_EQ_U32(f.setTexture(0u, tex), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampAddressU, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampAddressV, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfPoint), 0u);
    const std::vector<Fixture::TexVertex> quad = {
        {0.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
        {64.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {0.0f, 64.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
        {64.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {64.0f, 64.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
        {0.0f, 64.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
    };
    const std::uint32_t data = f.placeTexVertices(quad);

    // ---- ① 后缓冲**表面**（不是 NULL）⇒ 规范化成隐式后缓冲 + 这一批进 GPU ----
    const std::uint32_t bb = f.backBufferSurface();
    LIFT_CHECK_NE_U32(bb, 0u);
    if (bb == 0) return;
    LIFT_CHECK_EQ_U32(f.setRenderTarget(bb), 0u);
    lift::d3d9::PipelineInfo pi;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(f.m.cpu(), dev.state, &pi, nullptr));
    // ★ 规范化后的表示 = "隐式后缓冲"：`color_surface == 0` **且** `color_arena == 0`
    //   （0 = 后缓冲 ⇒ 像素与尺寸由 `DeviceInfo` 解析，见 `prepareContext` / `renderTargetArena`）。
    LIFT_CHECK_EQ_U32(pi.color_surface, 0u);
    LIFT_CHECK_EQ_U32(pi.color_arena, 0u);
    LIFT_CHECK_EQ_U32(f.drawTexturedQuad(data), 0u);
    // ★ 账本按 `Machine::run` 重算 ⇒ 只能在紧接这次 Draw 之后读（后面任何一次 `method()` 都会清零）。
    const std::uint64_t fallback_bb = lift::d3d9::gpuCoverage().fallback_draws;
    const std::uint64_t gpu_bb = lift::d3d9::gpuCoverage().gpu_draws;
    std::string why_bb;
    for (const auto& [reason, count] : lift::d3d9::gpuCoverage().fallback_reasons) {
        (void)count;
        why_bb = reason;
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 1);  // 真的入队 GPU
    if (mock.queued_draws == 0 && !why_bb.empty()) lift_test::failf(__FILE__, __LINE__, "① 后缓冲表面这一批没进 GPU：%s", why_bb.c_str());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gpu_bb), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fallback_bb), 0);  // ⛔ 不许退回 CPU 光栅

    // ---- ② 同尺寸的**纹理**表面 ⇒ 不规范化（`color_surface != 0`）且**进 GPU** ----
    // ★ 先解绑 stage 0 的采样：否则命中"同批把 RT 当采样纹理"那条新闸（那是下面
    //   `d3d9p/render-to-texture-feedback` 用例的判据）。② 要测的是**目标身份**。
    LIFT_CHECK_EQ_U32(f.setTexture(0u, 0u), 0u);
    const std::uint32_t tex_surface = f.textureSurface(tex);
    LIFT_CHECK_NE_U32(tex_surface, 0u);
    if (tex_surface == 0) return;
    LIFT_CHECK_EQ_U32(f.setRenderTarget(tex_surface), 0u);
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(f.m.cpu(), dev.state, &pi, nullptr));
    LIFT_CHECK_EQ_U32(pi.color_surface, tex_surface);  // 纹理 RT 保持原样（⛔ 不被规范化）
    LIFT_CHECK_EQ_U32(f.drawTexturedQuad(data), 0u);
    const std::uint64_t fallback_tex = lift::d3d9::gpuCoverage().fallback_draws;
    const std::uint64_t gpu_tex = lift::d3d9::gpuCoverage().gpu_draws;
    const void* const gpu_base_tex = mock.rasterTargetBase();
    std::string why_tex;
    for (const auto& [reason, count] : lift::d3d9::gpuCoverage().fallback_reasons) {
        (void)count;
        why_tex = reason;
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 2);  // 真的又入队了一条
    if (mock.queued_draws == 1 && !why_tex.empty())
        lift_test::failf(__FILE__, __LINE__, "② 同尺寸纹理 RT 这一批没进 GPU：%s", why_tex.c_str());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gpu_tex), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fallback_tex), 0);
    // ★ 身份判据：镜像的目标基址 = **纹理**像素（若把"同址"判成"同尺寸"，这里会是后缓冲像素）
    lift::d3d9::TextureInfo ti_rt;
    LIFT_CHECK(lift::d3d9::textureInfo(f.m.cpu(), tex, &ti_rt, nullptr));
    lift::GuestSurface* tex_gs = f.m.surfaces().find(ti_rt.arena);
    LIFT_CHECK(tex_gs != nullptr);
    if (tex_gs == nullptr) return;
    const void* const tex_pixels = f.m.surfaces().pixels(*tex_gs);
    LIFT_CHECK(gpu_base_tex != nullptr && gpu_base_tex == tex_pixels);
    (void)tbits;

    LIFT_CHECK_EQ_U32(f.endScene(), 0u);
    // 清理：先解绑（放掉流水线那两份引用）⇒ 再放 surface / 纹理（surface 替父纹理持一份，见 `owns_owner`）
    LIFT_CHECK_EQ_U32(f.setRenderTarget(0u), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex_surface, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 0u);
    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch-render-target-backbuffer-identity: queued=%llu fallback_tex=%llu reason=\"%s\"\n",
                static_cast<unsigned long long>(mock.queued_draws),
                static_cast<unsigned long long>(fallback_tex), why_tex.c_str());
}

// ---------------------------------------------------------------------------------------
// pure：**纹理当渲染目标（RTT）**的准入 + 目标绑定（`REQ-01M79MSMWGFEKF8J7C7VYWS4YP`）
// ---------------------------------------------------------------------------------------
// 红了意味着什么：
//   · 准入又把"纹理表面当渲染目标"拒回 CPU 光栅（修前读数：`queued_draws=0 / fallback_draws=1`、
//     reason = 「GPU 只支持默认后缓冲（当前渲染目标是 IDirect3DSurface9）」）⇒ 存档缩略图那条路
//     （引擎 `0x4A778B` / `0x4A5160` 把 1280×720 纹理当 RT）只剩 CPU 兜底；
//   · 或者准入放行了、但 target 没按**目标纹理像素**的身份绑（`rasterTargetBase()` 不是纹理像素）
//     ⇒ GPU 会把 RT 画进后缓冲的镜像（像素静默写错 surface，且没有任何报错）。
//   ★ 这条纹理用 **Usage=0**：缺 `D3DUSAGE_RENDERTARGET` 只**记账放行**（引擎真值未取证，
//     `.tmp/rtt/design.md` §1）⇒ 顺带钉住"没有新增一条静默回退"。
//   ★ 真后端臂（像素 + `GetRenderTargetData` 读回 + 同尺寸换 target 不重建 depth）在 `--gpu` 的
//     `caseGpuRenderToTexture`；pure 档没有 SDL_GPU 设备，只能看"入队 / 账本 / 目标身份"。
void caseRenderToTexture() {
    lift_test::g_current_case = "d3d9p/render-to-texture";
    lift::d3d9::gpuCoverageReset();
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    f.m.setGpuRaster(&mock);

    std::uint32_t tex = 0;
    // ★ 刻意 Usage=0（见上面 ★）：缺 D3DUSAGE_RENDERTARGET 也必须放行。
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 64u, 64u, lift::d3d9::kFmtA8R8G8B8, &tex, 0u), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    lift::d3d9::TextureInfo ti;
    LIFT_CHECK(lift::d3d9::textureInfo(f.m.cpu(), tex, &ti, nullptr));
    lift::GuestSurface* ts = f.m.surfaces().find(ti.arena);
    LIFT_CHECK(ts != nullptr);
    if (ts == nullptr) return;
    const void* const tex_pixels = f.m.surfaces().pixels(*ts);
    const std::uint32_t tex_surface = f.textureSurface(tex);
    LIFT_CHECK_NE_U32(tex_surface, 0u);
    if (tex_surface == 0) return;

    // ---- 绑定 + 一批无纹理三角形（`gpu-batch/sampler-filter-eligibility` 同一份可画状态）----
    LIFT_CHECK_EQ_U32(f.setRenderTarget(tex_surface), 0u);
    lift::d3d9::PipelineInfo pi;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(f.m.cpu(), dev.state, &pi, nullptr));
    LIFT_CHECK_EQ_U32(pi.color_surface, tex_surface);
    LIFT_CHECK_EQ_U32(pi.color_arena, ti.arena);

    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0u, 0u, 64u, 64u), 0u);
    LIFT_CHECK_EQ_U32(f.drawTriangle(63.0f, 0xFFFF0000u), 0u);
    // ★ 账本按 `Machine::run` 重算 ⇒ 只能在**紧接这次 Draw** 之后读（后面任何一次 `method()` 都会清零）。
    const std::uint64_t queued = mock.queued_draws;
    const std::uint64_t fallback = lift::d3d9::gpuCoverage().fallback_draws;
    const std::uint64_t gpu_draws = lift::d3d9::gpuCoverage().gpu_draws;
    const void* const gpu_base = mock.rasterTargetBase();
    std::string why;
    for (const auto& [reason, count] : lift::d3d9::gpuCoverage().fallback_reasons) {
        (void)count;
        why = reason;
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(queued), 1);       // ★ 真的入队了
    if (queued == 0) lift_test::failf(__FILE__, __LINE__, "RTT 这一批没进 GPU：%s", why.c_str());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fallback), 0);     // ⛔ 不许退回 CPU 光栅
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gpu_draws), 1);
    LIFT_CHECK(gpu_base != nullptr && gpu_base == tex_pixels);     // ★ 目标 = 纹理像素（不是后缓冲）

    LIFT_CHECK_EQ_U32(f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderTarget(0u), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex_surface, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 0u);
    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch-render-to-texture: queued=%llu fallback=%llu base=%p tex=%p\n",
                static_cast<unsigned long long>(queued), static_cast<unsigned long long>(fallback),
                gpu_base, tex_pixels);
}

// ---------------------------------------------------------------------------------------
// pure：D3DXSprite 的合成目标 = **当前绑定纹理 RT 的像素块**
//       （`REQ-01M79V2TVJH98BKH0C0WQV2FET`）
// ---------------------------------------------------------------------------------------
// 红了意味着什么：
//   `compositeBatch` 硬取 `dev.arena`（后缓冲）⇒ 绑 RT 时 sprite 的像素全进后缓冲：RT 读回是
//   全 0（save 缩略图 `SAVE00.STH` 全黑的最后一环），且**没有任何报错**。修前这里
//   `rasterTargetBase() == 后缓冲像素` ⇒ 第一条断言红。
//   ★ 真后端臂（像素 + `GetRenderTargetData` + 后缓冲不变）在 `--gpu` 的
//     `caseGpuSpriteRenderTarget`；pure 档只能看"合成目标身份 / 入队 / 账本"。
void caseSpriteRenderTarget() {
    lift_test::g_current_case = "d3d9p/sprite-render-target";
    lift::d3d9::gpuCoverageReset();
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    f.m.setGpuRaster(&mock);

    lift::GuestSurface* back = f.m.surfaces().find(dev.info.arena);
    LIFT_CHECK(back != nullptr);
    if (back == nullptr) return;
    const void* const back_pixels = f.m.surfaces().pixels(*back);

    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 64u, 64u, lift::d3d9::kFmtX8R8G8B8, &tex, 0x1u), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    lift::d3d9::TextureInfo ti;
    LIFT_CHECK(lift::d3d9::textureInfo(f.m.cpu(), tex, &ti, nullptr));
    lift::GuestSurface* ts = f.m.surfaces().find(ti.arena);
    LIFT_CHECK(ts != nullptr);
    if (ts == nullptr) return;
    const void* const tex_pixels = f.m.surfaces().pixels(*ts);
    const std::uint32_t tex_surface = f.textureSurface(tex);
    LIFT_CHECK_NE_U32(tex_surface, 0u);
    if (tex_surface == 0) return;

    LIFT_CHECK_EQ_U32(f.setRenderTarget(tex_surface), 0u);
    std::uint32_t sprite = 0;
    LIFT_CHECK_EQ_U32(f.createSprite(dev.object, &sprite), 0u);
    LIFT_CHECK_NE_U32(sprite, 0u);
    if (sprite == 0) return;

    // ---- 绑 RT 时画一个 sprite（4×4 白源 + 调制色）----
    const std::uint32_t src4 = f.placeRect(0, 0, 4, 4);
    const std::uint32_t zero3 = f.placeVec3(0.0f, 0.0f, 0.0f);
    const std::uint32_t pos = f.placeVec3(10.0f, 20.0f, 0.0f);
    LIFT_CHECK_EQ_U32(f.spriteBegin(sprite, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.spriteDraw(sprite, 0u, src4, zero3, pos, 0xFFFF0000u), 0u);
    LIFT_CHECK_EQ_U32(f.spriteEnd(sprite), 0u);
    // ★ 判据：合成目标 = RT 的像素块（修前 = 后缓冲像素）。
    const void* const composite_base = mock.rasterTargetBase();
    LIFT_CHECK(composite_base != nullptr && composite_base == tex_pixels);
    if (!(composite_base != nullptr && composite_base == tex_pixels))
        lift_test::failf(__FILE__, __LINE__,
                         "sprite 合成目标不是绑定 RT 的像素块：base=%p rt=%p back=%p（修前 = 后缓冲）",
                         composite_base, tex_pixels, back_pixels);
    LIFT_CHECK(composite_base != back_pixels);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 1);  // 真的交给 GPU 了
    // ★ 账本：sprite 这一条必须记成 GPU，⛔ 不是 fallback（否则"目标对"也可能是 CPU 写出来的）。
    const lift::d3d9::GpuCoverage cov = lift::d3d9::gpuCoverage();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.sprite_draws), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.sprite_gpu_draws), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.sprite_fallback_draws), 0);

    // ---- 解绑后同一批必须回到后缓冲（控制组：目标不是"永远是 RT"）----
    LIFT_CHECK_EQ_U32(f.setRenderTarget(0u), 0u);
    const std::uint32_t pos2 = f.placeVec3(30.0f, 30.0f, 0.0f);
    LIFT_CHECK_EQ_U32(f.spriteBegin(sprite, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.spriteDraw(sprite, 0u, src4, zero3, pos2, 0xFFFF0000u), 0u);
    LIFT_CHECK_EQ_U32(f.spriteEnd(sprite), 0u);
    LIFT_CHECK(mock.rasterTargetBase() == back_pixels);

    LIFT_CHECK_EQ_U32(f.method(sprite, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex_surface, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 0u);
    f.m.setGpuRaster(nullptr);
    std::printf("d3d9p/sprite-render-target: base=%p rt=%p back=%p queued=%zu\n", composite_base,
                tex_pixels, back_pixels, mock.queued_draws);
}

// ---------------------------------------------------------------------------------------
// pure：**反馈环闸** —— 同一批里把 RT 纹理当采样源 ⇒ 响亮拒绝 + 记账
//       （`REQ-01M79MSMWGFEKF8J7C7VYWS4YP`）
// ---------------------------------------------------------------------------------------
// 红了意味着什么：
//   本后端采样走**纹理上传缓存**（不是 target 本身）⇒ 同一批里"采样这张 RT"读到的是本批**之前**
//   的内容：不挂死、不报错、像素是旧帧（静默）。所以准入必须拒这一批（`recordUnsupported(bit26)`
//   + fallback 账），⛔ 不能放过去蒙一个"看起来画了"。
//   ★ 控制组：**解绑 stage 0 采样**后同一批必须进 GPU ⇒ 拒绝只因"反馈环"这一条闸，
//     不是被别的准入项（目标 / 尺寸 / 格式）挡下来的巧合。
//   ★ 变异：把这条闸删掉（`tools/mutate-check.mjs` 的同名条目）⇒ 第一条断言当场红。
void caseRenderToTextureFeedback() {
    lift_test::g_current_case = "d3d9p/render-to-texture-feedback";
    lift::d3d9::gpuCoverageReset();
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    f.m.setGpuRaster(&mock);

    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 64u, 64u, lift::d3d9::kFmtA8R8G8B8, &tex, 0x1u), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    const std::uint32_t tex_surface = f.textureSurface(tex);
    LIFT_CHECK_NE_U32(tex_surface, 0u);
    if (tex_surface == 0) return;

    LIFT_CHECK_EQ_U32(f.setRenderTarget(tex_surface), 0u);
    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuseTex1), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0u, 0u, 64u, 64u), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampAddressU, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampAddressV, lift::d3d9::kAddressClamp), 0u);
    // ★ RT 与采样源是**同一张纹理**（`textures[0].pixels == color_base`）—— 反馈环。
    LIFT_CHECK_EQ_U32(f.setTexture(0u, tex), 0u);
    const std::vector<Fixture::TexVertex> quad = {
        {0.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
        {64.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {0.0f, 64.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
        {64.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {64.0f, 64.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
        {0.0f, 64.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
    };
    const std::uint32_t data = f.placeTexVertices(quad);
    LIFT_CHECK_EQ_U32(f.drawTexturedQuad(data), 0u);
    const std::uint64_t queued_a = mock.queued_draws;
    const std::uint64_t fallback_a = lift::d3d9::gpuCoverage().fallback_draws;
    const std::uint64_t gpu_a = lift::d3d9::gpuCoverage().gpu_draws;
    std::string why_a;
    for (const auto& [reason, count] : lift::d3d9::gpuCoverage().fallback_reasons) {
        (void)count;
        why_a = reason;
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(queued_a), 0);   // ★ 一条都没交给 GPU
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gpu_a), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fallback_a), 1);  // ★ 但账上必须看见
    LIFT_CHECK(why_a.find("被采样") != std::string::npos);
    if (why_a.find("被采样") == std::string::npos)
        lift_test::failf(__FILE__, __LINE__, "反馈环拒绝的原因不是那条闸：%s", why_a.c_str());

    // ---- 控制组：解绑采样 ⇒ 同一批必须进 GPU ----
    LIFT_CHECK_EQ_U32(f.setTexture(0u, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.drawTexturedQuad(data), 0u);
    const std::uint64_t queued_b = mock.queued_draws;
    const std::uint64_t fallback_b = lift::d3d9::gpuCoverage().fallback_draws;
    const std::uint64_t gpu_b = lift::d3d9::gpuCoverage().gpu_draws;
    std::string why_b;
    for (const auto& [reason, count] : lift::d3d9::gpuCoverage().fallback_reasons) {
        (void)count;
        why_b = reason;
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(queued_b), 1);
    if (queued_b == 0) lift_test::failf(__FILE__, __LINE__, "解绑采样后仍没进 GPU：%s", why_b.c_str());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fallback_b), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gpu_b), 1);

    LIFT_CHECK_EQ_U32(f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderTarget(0u), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex_surface, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 0u);
    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch-render-to-texture-feedback: queued_a=%llu fallback_a=%llu reason=\"%s\" queued_b=%llu\n",
                static_cast<unsigned long long>(queued_a), static_cast<unsigned long long>(fallback_a),
                why_a.c_str(), static_cast<unsigned long long>(queued_b));
}

// ---------------------------------------------------------------------------------------
// pure：覆盖账本把"入队成功"与"真的进过 render pass"拆开
//       （`REQ-01M73PZN6R3AC3RCQ8WZEA0Y0M`）
// ---------------------------------------------------------------------------------------
// 判据（会红的情形）：
//   ① 把 `recorded` 判成 `rasterQueue` 的返回值 ⇒ 下面"入队成功但后端不记录"的批会被算成
//      recorded（期望 `empty=1` 得 0）⇒ 红；
//   ② `empty` 恒 0（`tools/mutate-check.mjs` 的变异条目）⇒ ①的同一处断言红；
//   ③ `queued`/`fragments` 被记成"准入通过"而不是"真的入队" ⇒ `queued` 期望 1 仍成立，但
//      第二批（顶点全被裁掉的空批）那条 `queued==0 且 empty==1` 的断言红。
//
// ★ 为什么走 `drawPrimitiveUP` 的**直接调用**（而不是夹具的 `method()`）：
//   每次 guest COM 调用 = 一次 `Machine::run`，而 `Machine::run` 开头会 `gpuCoverageReset()`
//   ⇒ 一个 run 里只能看到**一轮**的账。"入队 → flush → 回报"必须落在同一轮里才可断言。
void caseLedgerQueuedVsRecorded() {
    lift_test::g_current_case = "gpu-batch/ledger-queued-vs-recorded";
    Fixture f;
    LIFT_CHECK(f.boot(false));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    f.m.setGpuRaster(&mock);
    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0, 0, 64, 64), 0u);

    // 20 B/顶点的 XYZRHW|DIFFUSE 三角形（与 `Fixture::drawTriangle` 同一份数据）
    const std::vector<std::array<float, 4>> xyz = {
        {0.0f, 0.0f, 0.0f, 1.0f}, {8.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 8.0f, 0.0f, 1.0f}};
    std::vector<std::uint8_t> bytes;
    for (const auto& v : xyz) {
        for (int k = 0; k < 4; ++k) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v[k], sizeof(bits));
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((bits >> (8 * b)) & 0xFF));
        }
        for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((0xFFFF0000u >> (8 * b)) & 0xFF));
    }
    const std::uint32_t tri = f.placeBytes(bytes);
    std::string why;
    std::string err;

    // ---- ① 入队成功、后端**不记录**（flush 成功也不回报）⇒ queued=1 recorded=0 empty=1 ----
    lift::d3d9::gpuCoverageReset();
    mock.record_draws = false;
    LIFT_CHECK_EQ_U32(
        lift::d3d9::drawPrimitiveUP(f.m.cpu(), dev.state, lift::d3d9::kPtTriangleList, 1u, tri, 20u, &why),
        lift::d3d9::kOk);
    {
        const lift::d3d9::GpuCoverage& cov = lift::d3d9::gpuCoverage();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.gpu_draws), 1);   // 修前的"覆盖"账不变（本单不改它）
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.queued), 1);      // 真的入了队
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.recorded), 0);    // 但一条 render pass 都没录
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.empty), 1);       // ★ 这批必须计入 empty
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.fragments), 3);   // 3 个顶点交出去了
    }
    // flush **成功**但后端没回报 ⇒ empty 必须留着（⛔ 不许"flush 过就算覆盖"）
    LIFT_CHECK(mock.rasterFlush(&err));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::gpuCoverage().recorded), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::gpuCoverage().empty), 1);

    // ---- ② 同一批、后端正常回报（命令缓冲提交成功之后）⇒ recorded=1 empty=0 ----
    lift::d3d9::gpuCoverageReset();
    mock.record_draws = true;
    LIFT_CHECK_EQ_U32(
        lift::d3d9::drawPrimitiveUP(f.m.cpu(), dev.state, lift::d3d9::kPtTriangleList, 1u, tri, 20u, &why),
        lift::d3d9::kOk);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::gpuCoverage().empty), 1);  // flush 前：还没录
    LIFT_CHECK(mock.rasterFlush(&err));
    {
        const lift::d3d9::GpuCoverage& cov = lift::d3d9::gpuCoverage();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.queued), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.recorded), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.empty), 0);  // ★ 回报后挪出 empty
    }

    // ---- ③ 空批（顶点全被裁掉 ⇒ 连队都没入）：账上仍 gpu=1，但 queued=0 / empty=1 ----
    lift::d3d9::gpuCoverageReset();
    // 完全在视口外（64×64）的三角形 ⇒ 裁剪后 0 个三角形 ⇒ `gpu_vertices` 为空
    std::vector<std::uint8_t> off;
    for (const auto& v : std::vector<std::array<float, 4>>{{200.0f, 200.0f, 0.0f, 1.0f},
                                                           {216.0f, 200.0f, 0.0f, 1.0f},
                                                           {200.0f, 216.0f, 0.0f, 1.0f}}) {
        for (int k = 0; k < 4; ++k) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &v[k], sizeof(bits));
            for (int b = 0; b < 4; ++b) off.push_back(static_cast<std::uint8_t>((bits >> (8 * b)) & 0xFF));
        }
        for (int b = 0; b < 4; ++b) off.push_back(static_cast<std::uint8_t>((0xFFFF0000u >> (8 * b)) & 0xFF));
    }
    LIFT_CHECK_EQ_U32(lift::d3d9::drawPrimitiveUP(f.m.cpu(), dev.state, lift::d3d9::kPtTriangleList, 1u,
                                                  f.placeBytes(off), 20u, &why),
                      lift::d3d9::kOk);
    {
        const lift::d3d9::GpuCoverage& cov = lift::d3d9::gpuCoverage();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.gpu_draws), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.queued), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.recorded), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.empty), 1);  // ★ 比"入队成功但没录"更坏的一种
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.fragments), 0);
    }

    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch-ledger-queued-vs-recorded: queue=%zu flush=%zu\n", mock.queued_draws, mock.flush_count);
}

// ---------------------------------------------------------------------------------------
// --gpu：真 SDL_GPU 后端 + 真 Clear / LockRect / SetRenderTarget 的像素判据
// ---------------------------------------------------------------------------------------
bool queueQuad(lift::SdlWindow& win, float x0, float y0, float x1, float y1, float r, float g, float b,
               std::string* err) {
    const lift::GpuRasterVertex v[6] = {
        {x0, y0, 0.0f, r, g, b, 1.0f, 0.0f, 0.0f}, {x1, y0, 0.0f, r, g, b, 1.0f, 0.0f, 0.0f},
        {x0, y1, 0.0f, r, g, b, 1.0f, 0.0f, 0.0f}, {x1, y0, 0.0f, r, g, b, 1.0f, 0.0f, 0.0f},
        {x1, y1, 0.0f, r, g, b, 1.0f, 0.0f, 0.0f}, {x0, y1, 0.0f, r, g, b, 1.0f, 0.0f, 0.0f}};
    lift::GpuRasterState st;
    st.viewport_w = 64;
    st.viewport_h = 64;
    st.scissor_x1 = 64;
    st.scissor_y1 = 64;
    st.depth_test = false;
    st.depth_write = false;
    st.blend = false;
    st.color_write = 0xFu;
    return win.rasterQueue(v, 6, nullptr, st, err);
}

int caseGpuBarriers() {
    lift_test::g_current_case = "gpu-batch/barriers";
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return 0;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return 0;

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift gpu-batch test";
    opt.driver_list = "software";
    opt.hidden = true;
    opt.raster_gpu = true;
    std::string err;
    if (!win.init(opt, &err)) LIFT_SKIP(err.c_str());
    if (!win.rasterActive()) {
        win.shutdown();
        LIFT_SKIP("rasterActive == false");
    }
    f.m.setGpuRaster(win.gpuRaster());

    lift::GuestSurface* back_surface = f.m.surfaces().find(dev.info.arena);
    LIFT_CHECK(back_surface != nullptr);
    if (back_surface == nullptr) return 0;
    std::uint8_t* back = f.m.surfaces().pixels(*back_surface);
    const lift::SurfaceFormat fmt = back_surface->format;
    const int w = back_surface->width;
    const int h = back_surface->height;
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(w), 64u);
    LIFT_CHECK(win.rasterPrepare(fmt, w, h, &err, back));
    LIFT_CHECK(win.rasterSyncFromGuest(back, back_surface->pitch, w, h, &err));

    // ---- ① Clear(TARGET) 屏障：队列里有 3 条 draw，Clear 之后必须已提交且读回 ----
    for (int i = 0; i < 3; ++i) LIFT_CHECK(queueQuad(win, 0.0f, 0.0f, 32.0f, 32.0f, 1.0f, 0.0f, 0.0f, &err));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(win.rasterQueuedDraws()), 3u);
    LIFT_CHECK(win.rasterDirty());
    // 只清右下半区（32,32..64,64）：左上半区必须还是队列里画的红块 ⇒ 同时证明"先 flush 再读回"
    LIFT_CHECK_EQ_U32(f.clearRect(32, 32, 64, 64, 0xFF0000FFu), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(win.rasterQueuedDraws()), 0u);
    LIFT_CHECK(!win.rasterDirty());
    LIFT_CHECK_EQ_U32(f.pixelAt(16, 16), 0x00FF0000u);  // GPU 画的红块（X8R8G8B8 ⇒ A 字节 0）
    LIFT_CHECK_EQ_U32(f.pixelAt(48, 48), 0x000000FFu);  // CPU 清成蓝
    // 再走一次 rasterSyncToGuest（目标就是当前镜像的 guest 像素）：读回后 dirty=false，像素不变。
    LIFT_CHECK(win.rasterSyncToGuest(back, back_surface->pitch, w, h, &err));
    LIFT_CHECK(!win.rasterDirty());
    LIFT_CHECK_EQ_U32(f.pixelAt(16, 16), 0x00FF0000u);
    // ★ 基址不符的读回必须被拒绝（⛔ 不把镜像写进别的 surface）
    std::vector<std::uint8_t> other(static_cast<std::size_t>(w) * h * 4u, 0);
    LIFT_CHECK(!win.rasterSyncToGuest(other.data(), static_cast<std::uint32_t>(w) * 4u, w, h, &err));

    // ---- ② LockRect 屏障：纹理当 RT 且队列非空 ⇒ Lock 前必须 flush + 读回 ----
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 64, 64, lift::d3d9::kFmtX8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return 0;
    lift::d3d9::TextureInfo ti;
    std::string tex_why;
    LIFT_CHECK(lift::d3d9::textureInfo(f.m.cpu(), tex, &ti, &tex_why));
    lift::GuestSurface* tex_surface = f.m.surfaces().find(ti.arena);
    LIFT_CHECK(tex_surface != nullptr);
    if (tex_surface == nullptr) return 0;
    std::uint8_t* tex_pixels = f.m.surfaces().pixels(*tex_surface);
    const std::uint32_t tex_surface_obj = f.textureSurface(tex);
    LIFT_CHECK_NE_U32(tex_surface_obj, 0u);
    if (tex_surface_obj == 0) return 0;
    LIFT_CHECK_EQ_U32(f.setRenderTarget(tex_surface_obj), 0u);
    LIFT_CHECK(win.rasterPrepare(fmt, w, h, &err, tex_pixels));
    LIFT_CHECK(win.rasterSyncFromGuest(tex_pixels, tex_surface->pitch, w, h, &err));
    for (int i = 0; i < 2; ++i) LIFT_CHECK(queueQuad(win, 0.0f, 0.0f, 32.0f, 32.0f, 0.0f, 1.0f, 0.0f, &err));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(win.rasterQueuedDraws()), 2u);
    std::uint32_t locked_rect_ea = 0;
    LIFT_CHECK_EQ_U32(f.textureLockRect(tex, &locked_rect_ea), 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(win.rasterQueuedDraws()), 0u);
    LIFT_CHECK(!win.rasterDirty());
    const std::uint32_t tex16 = f.m.mem().rd32(ti.arena + 16u * tex_surface->pitch + 16u * 4u);
    LIFT_CHECK_EQ_U32(tex16, 0x0000FF00u);  // 队列里的绿块已经在纹理像素里
    LIFT_CHECK_EQ_U32(f.textureUnlockRect(tex), 0u);

    // ---- ③ SetRenderTarget 屏障：切回后缓冲 ⇒ 旧目标（后缓冲）必须收到队列里的蓝块 ----
    LIFT_CHECK_EQ_U32(f.setRenderTarget(0u), 0u);
    LIFT_CHECK(win.rasterPrepare(fmt, w, h, &err, back));
    LIFT_CHECK(win.rasterSyncFromGuest(back, back_surface->pitch, w, h, &err));
    for (int i = 0; i < 2; ++i) LIFT_CHECK(queueQuad(win, 0.0f, 0.0f, 32.0f, 32.0f, 0.0f, 0.0f, 1.0f, &err));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(win.rasterQueuedDraws()), 2u);
    LIFT_CHECK_EQ_U32(f.setRenderTarget(tex_surface_obj), 0u);  // 换 target 前 sync 旧目标
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(win.rasterQueuedDraws()), 0u);
    LIFT_CHECK(!win.rasterDirty());
    LIFT_CHECK_EQ_U32(f.pixelAt(16, 16), 0x000000FFu);  // 蓝块已经落在后缓冲
    LIFT_CHECK_EQ_U32(f.setRenderTarget(0u), 0u);

    // ---- ④ 同尺寸换 guest 像素基址（rasterPrepare 的身份屏障）----
    // 队列非空时把 target 换成另一块**同尺寸**的 guest 像素：后端必须先 flush + 回读**旧**基址，
    // 再把镜像切到新基址（⛔ 不能把旧像素留在即将被复用的镜像里）。
    LIFT_CHECK(win.rasterPrepare(fmt, w, h, &err, back));
    LIFT_CHECK(win.rasterSyncFromGuest(back, back_surface->pitch, w, h, &err));
    for (int i = 0; i < 2; ++i) LIFT_CHECK(queueQuad(win, 0.0f, 0.0f, 32.0f, 32.0f, 1.0f, 0.0f, 1.0f, &err));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(win.rasterQueuedDraws()), 2u);
    std::vector<std::uint8_t> other_target(static_cast<std::size_t>(w) * h * 4u, 0);
    LIFT_CHECK(win.rasterPrepare(fmt, w, h, &err, other_target.data()));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(win.rasterQueuedDraws()), 0u);
    LIFT_CHECK(!win.rasterDirty());
    LIFT_CHECK_EQ_U32(f.pixelAt(16, 16), 0x00FF00FFu);  // 洋红块已经回读到**旧**基址（后缓冲）
    LIFT_CHECK(win.rasterTargetBase() == other_target.data());  // 镜像已切到新基址

    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch-barrier: driver=%s present=%s\n", win.rendererName(), win.describe().c_str());
    win.shutdown();
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    return 0;
}

// ---------------------------------------------------------------------------------------
// `--gpu`：**纹理当渲染目标 + 读回**（`REQ-01M79MSMWGFEKF8J7C7VYWS4YP` 的真后端臂）
// ---------------------------------------------------------------------------------------
// 判据（红了意味着什么）：
//   ① 纹理 RT 那一批**没有**退回 CPU：`fallback_draws==0`、`queued>=1`（修前：fallback=1 ⇒ 红）；
//   ② **像素**：`Clear(TARGET)` 清屏蓝 + 红三角 (0,0)(63,0)(0,63) ⇒ `GetRenderTargetData` 读回的
//      离屏表面里，(16,16) 是红、(56,56) 是清屏蓝。只判**粗粒度容差**（±16/通道），
//      ⛔ 不与 CPU 光栅逐像素比（CPU 回退正往同一块 arena 写 ⇒ 逐像素比会假绿）。
//   ③ 换 target 的**开销**：同尺寸换 target **不重建 depth**、`pipelines_` 不清
//      （`gpuRasterStats()` 的 targets/depths/pipelines 差值）；而 depth 只在**尺寸变**时重建。
//      修前：`destroyTargets()` 连 depth + 全部 pipeline 一起毁 ⇒ 第三条当场红。
//   ★ 没有 SDL_GPU 设备 ⇒ `LIFT_SKIP(77)`（本文件 `main` 对 `--gpu` 臂清掉 `SDL_VIDEODRIVER=dummy`，
//     因为 dummy 视频下建不出 GPU 设备；真没有显示系统时 `win.init` 失败 ⇒ 这里照样 SKIP，⛔ 不假绿）。
int caseGpuRenderToTexture() {
    lift_test::g_current_case = "d3d9p/render-to-texture";
    lift::d3d9::gpuCoverageReset();
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return 0;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return 0;

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift gpu-batch rtt test";
    opt.driver_list = "software";
    opt.hidden = true;
    opt.raster_gpu = true;
    std::string err;
    if (!win.init(opt, &err)) LIFT_SKIP(err.c_str());
    if (!win.rasterActive()) {
        win.shutdown();
        LIFT_SKIP("rasterActive == false（建不出 SDL_GPU 设备 / 没有渲染后端）");
    }
    f.m.setGpuRaster(win.gpuRaster());

    // 两张同尺寸（64×64 = 后缓冲尺寸）的 32bpp 纹理 RT：A 是判据目标，B 用来验"换 target 复用 depth"。
    std::uint32_t tex_a = 0;
    std::uint32_t tex_b = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 64u, 64u, lift::d3d9::kFmtX8R8G8B8, &tex_a, 0x1u), 0u);
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 64u, 64u, lift::d3d9::kFmtX8R8G8B8, &tex_b, 0x1u), 0u);
    if (tex_a == 0 || tex_b == 0) return 0;
    const std::uint32_t surf_a = f.textureSurface(tex_a);
    const std::uint32_t surf_b = f.textureSurface(tex_b);
    if (surf_a == 0 || surf_b == 0) return 0;

    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0u, 0u, 64u, 64u), 0u);

    // ---- ① 绑 A → 清屏蓝 → 画红三角（(0,0)(63,0)(0,63)）----
    const lift::GpuRasterStats stats_before = lift::gpuRasterStats();
    LIFT_CHECK_EQ_U32(f.setRenderTarget(surf_a), 0u);
    LIFT_CHECK_EQ_U32(f.clearTo(0xFF0000FFu), 0u);
    LIFT_CHECK_EQ_U32(f.drawTriangle(63.0f, 0xFFFF0000u), 0u);
    // ★ 账本按 `Machine::run` 重算 ⇒ 只能在紧接这次 Draw 之后读。
    const std::uint64_t fallback_a = lift::d3d9::gpuCoverage().fallback_draws;
    const std::uint64_t queued_a = lift::d3d9::gpuCoverage().queued;
    const lift::GpuRasterStats stats_a = lift::gpuRasterStats();
    std::string why_a;
    for (const auto& [reason, count] : lift::d3d9::gpuCoverage().fallback_reasons) {
        (void)count;
        why_a = reason;
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fallback_a), 0);
    if (fallback_a != 0) lift_test::failf(__FILE__, __LINE__, "纹理 RT 这一批退回了 CPU：%s", why_a.c_str());
    LIFT_CHECK(queued_a >= 1);
    // A 是本次用例第一个 target ⇒ 建了 1 个 color target、1 份 depth
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(stats_a.targets_created - stats_before.targets_created), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(stats_a.depths_created - stats_before.depths_created), 1);

    // ---- ② 同尺寸换 target（B）⇒ depth 复用、pipeline 不重建 ----
    LIFT_CHECK_EQ_U32(f.setRenderTarget(surf_b), 0u);
    LIFT_CHECK_EQ_U32(f.drawTriangle(63.0f, 0xFF00FF00u), 0u);
    const std::uint64_t fallback_b = lift::d3d9::gpuCoverage().fallback_draws;
    const std::uint64_t queued_b = lift::d3d9::gpuCoverage().queued;
    const lift::GpuRasterStats stats_b = lift::gpuRasterStats();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fallback_b), 0);
    LIFT_CHECK(queued_b >= 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(stats_b.targets_created - stats_a.targets_created), 1);
    // ★ 同尺寸 ⇒ **不**重建 depth（D3D9 的深度属设备、不随 color target 变）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(stats_b.depths_created - stats_a.depths_created), 0);
    // ★ 同一份状态 ⇒ pipeline 命中缓存（换 target 若清了 `pipelines_`，这里会 +1）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(stats_b.pipelines_created - stats_a.pipelines_created), 0);

    // ---- ③ 切回 A，用 GetRenderTargetData 读回像素 ----
    LIFT_CHECK_EQ_U32(f.setRenderTarget(surf_a), 0u);
    const std::uint32_t offs = f.createOffscreenPlainSurface(64u, 64u, lift::d3d9::kFmtX8R8G8B8);
    LIFT_CHECK_NE_U32(offs, 0u);
    if (offs == 0) return 0;
    LIFT_CHECK_EQ_U32(f.getRenderTargetData(surf_a, offs), 0u);
    std::uint32_t locked_rect = 0;
    LIFT_CHECK_EQ_U32(f.surfaceLockRect(offs, 0u, &locked_rect), 0u);
    const std::uint32_t pitch = f.m.mem().rd32(locked_rect + 0u);
    const std::uint32_t bits = f.m.mem().rd32(locked_rect + 4u);
    LIFT_CHECK_EQ_U32(pitch, 64u * 4u);
    LIFT_CHECK_NE_U32(bits, 0u);
    if (pitch == 64u * 4u && bits != 0u) {
        // 像素字节序 = B,G,R,X（X8R8G8B8）
        const std::uint32_t inside = f.m.mem().rd32(bits + 16u * pitch + 16u * 4u);
        const std::uint32_t outside = f.m.mem().rd32(bits + 56u * pitch + 56u * 4u);
        const unsigned in_b = inside & 0xFFu, in_g = (inside >> 8) & 0xFFu, in_r = (inside >> 16) & 0xFFu;
        const unsigned out_b = outside & 0xFFu, out_g = (outside >> 8) & 0xFFu,
                       out_r = (outside >> 16) & 0xFFu;
        LIFT_CHECK(in_r >= 240u && in_g <= 16u && in_b <= 16u);   // 三角内 = 红
        if (!(in_r >= 240u && in_g <= 16u && in_b <= 16u))
            lift_test::failf(__FILE__, __LINE__, "三角内 (16,16) 不是红：R=%u G=%u B=%u（0x%08X）", in_r, in_g,
                             in_b, inside);
        LIFT_CHECK(out_b >= 240u && out_g <= 16u && out_r <= 16u);  // 三角外 = 清屏蓝
        if (!(out_b >= 240u && out_g <= 16u && out_r <= 16u))
            lift_test::failf(__FILE__, __LINE__, "三角外 (56,56) 不是清屏蓝：R=%u G=%u B=%u（0x%08X）", out_r,
                             out_g, out_b, outside);
        LIFT_CHECK(in_r != out_r || in_b != out_b);  // 两处必须真的不同（防"都读到同一块"）
    }
    LIFT_CHECK_EQ_U32(f.surfaceUnlockRect(offs), 0u);

    LIFT_CHECK_EQ_U32(f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderTarget(0u), 0u);
    LIFT_CHECK_EQ_U32(f.method(offs, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(surf_a, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(surf_b, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex_a, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex_b, 0x08u, {}), 0u);
    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch-render-to-texture: driver=%s fallback=0 depths_created +%lld pipelines +%lld\n",
                win.rendererName(),
                static_cast<long long>(stats_a.depths_created - stats_before.depths_created),
                static_cast<long long>(stats_b.pipelines_created - stats_a.pipelines_created));
    win.shutdown();
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    return 0;
}

// ---------------------------------------------------------------------------------------
// `--gpu`：D3DXSprite 写进**绑定的纹理 RT**（`REQ-01M79V2TVJH98BKH0C0WQV2FET` 的真后端臂）
// ---------------------------------------------------------------------------------------
// 判据（红了意味着什么）：
//   ① sprite 的像素真的在 RT 里：`GetRenderTargetData` 读回後 (17,17) ≈ 红（±16/通道）、
//      (48,48) = 清屏黑；修前 sprite 硬取 `dev.arena` ⇒ RT 读回**全 0** ⇒ 红；
//   ② 后缓冲**不变**：sprite 画之前后缓冲清成蓝，画完仍是蓝（修前会被写红）⇒ 第二条断言红。
//   ③ 账本：`sprite_gpu_draws >= 1` 且 `sprite_fallback_draws == 0` —— sprite 真的进了 GPU 批、
//      **没有静默回退 CPU**（回退也写同一个 RT ⇒ 只有 ① ② 抓不到"假 GPU"）。
//   ★ 没有 SDL_GPU 设备 / 没有显示系统 ⇒ SKIP 77。
int caseGpuSpriteRenderTarget() {
    lift_test::g_current_case = "d3d9p/sprite-render-target";
    lift::d3d9::gpuCoverageReset();
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return 0;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return 0;

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift gpu-batch sprite-rt test";
    opt.driver_list = "software";
    opt.hidden = true;
    opt.raster_gpu = true;
    std::string err;
    if (!win.init(opt, &err)) LIFT_SKIP(err.c_str());
    if (!win.rasterActive()) {
        win.shutdown();
        LIFT_SKIP("rasterActive == false（建不出 SDL_GPU 设备 / 没有渲染后端）");
    }
    f.m.setGpuRaster(win.gpuRaster());

    // 后缓冲先清成蓝：sprite 若（错误地）写进后缓冲，这条基线会被破坏。
    LIFT_CHECK_EQ_U32(f.clearTo(0xFF0000FFu), 0u);
    LIFT_CHECK_EQ_U32(f.pixelAt(17, 17), 0x000000FFu);

    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 64u, 64u, lift::d3d9::kFmtX8R8G8B8, &tex, 0x1u), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return 0;
    lift::d3d9::TextureInfo ti;
    LIFT_CHECK(lift::d3d9::textureInfo(f.m.cpu(), tex, &ti, nullptr));
    lift::GuestSurface* ts = f.m.surfaces().find(ti.arena);
    LIFT_CHECK(ts != nullptr);
    if (ts == nullptr) return 0;
    const std::uint32_t surf = f.textureSurface(tex);
    LIFT_CHECK_NE_U32(surf, 0u);
    if (surf == 0) return 0;
    LIFT_CHECK_EQ_U32(f.setRenderTarget(surf), 0u);
    // RT 清成黑（**不带** ZBUFFER 也无所谓：sprite 的 GPU 路径关深度测试/写）。
    LIFT_CHECK_EQ_U32(f.clearTo(0x00000000u), 0u);

    std::uint32_t sprite = 0;
    LIFT_CHECK_EQ_U32(f.createSprite(dev.object, &sprite), 0u);
    LIFT_CHECK_NE_U32(sprite, 0u);
    if (sprite == 0) return 0;
    const std::uint32_t src8 = f.placeRect(0, 0, 8, 8);
    const std::uint32_t zero3 = f.placeVec3(0.0f, 0.0f, 0.0f);
    const std::uint32_t pos = f.placeVec3(16.0f, 16.0f, 0.0f);
    LIFT_CHECK_EQ_U32(f.spriteBegin(sprite, 0u), 0u);
    // pTexture=0 ⇒ 白色源（D3D9 固定功能）；调制色红 ⇒ 8×8 的红块 [16,24)×[16,24)。
    LIFT_CHECK_EQ_U32(f.spriteDraw(sprite, 0u, src8, zero3, pos, 0xFFFF0000u), 0u);
    LIFT_CHECK_EQ_U32(f.spriteEnd(sprite), 0u);

    // ③ 账本（复审补强）：sprite 必须**真的走了 GPU 批**、且**没有静默回退 CPU**。
    //   ⛔ 只判像素/后缓冲抓不到这一条：CPU 回退也把像素写进**同一个** RT ⇒ 像素对、账是 fallback。
    //   读法与 pure 段 `d3d9p/sprite-render-target` 一致（`lift::d3d9::gpuCoverage()`）。
    const lift::d3d9::GpuCoverage cov = lift::d3d9::gpuCoverage();
    LIFT_CHECK(cov.sprite_gpu_draws >= 1);
    if (cov.sprite_gpu_draws < 1)
        lift_test::failf(__FILE__, __LINE__, "sprite 没走 GPU 批：gpu_draws=%llu fallback=%llu",
                         (unsigned long long)cov.sprite_gpu_draws,
                         (unsigned long long)cov.sprite_fallback_draws);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.sprite_fallback_draws), 0);

    // 切回后缓冲 ⇒ 屏障先把 sprite 的队列画进 RT 并读回 guest。
    LIFT_CHECK_EQ_U32(f.setRenderTarget(0u), 0u);

    // ② 后缓冲不变（修前 sprite 合成到后缓冲 ⇒ 这里变红）。
    LIFT_CHECK_EQ_U32(f.pixelAt(17, 17), 0x000000FFu);
    if (f.pixelAt(17, 17) != 0x000000FFu)
        lift_test::failf(__FILE__, __LINE__, "sprite 写了后缓冲（期望仍是清屏蓝 0x000000FF，实得 0x%08X）",
                         f.pixelAt(17, 17));

    // ① 从 RT 读回：非全 0 且颜色在 ±16 容差内。
    const std::uint32_t offs = f.createOffscreenPlainSurface(64u, 64u, lift::d3d9::kFmtX8R8G8B8);
    LIFT_CHECK_NE_U32(offs, 0u);
    if (offs == 0) return 0;
    LIFT_CHECK_EQ_U32(f.getRenderTargetData(surf, offs), 0u);
    std::uint32_t locked_rect = 0;
    LIFT_CHECK_EQ_U32(f.surfaceLockRect(offs, 0u, &locked_rect), 0u);
    const std::uint32_t pitch = f.m.mem().rd32(locked_rect + 0u);
    const std::uint32_t bits = f.m.mem().rd32(locked_rect + 4u);
    LIFT_CHECK_EQ_U32(pitch, 64u * 4u);
    LIFT_CHECK_NE_U32(bits, 0u);
    if (pitch == 64u * 4u && bits != 0u) {
        const std::uint32_t inside = f.m.mem().rd32(bits + 20u * pitch + 20u * 4u);
        const std::uint32_t outside = f.m.mem().rd32(bits + 48u * pitch + 48u * 4u);
        const unsigned in_b = inside & 0xFFu, in_g = (inside >> 8) & 0xFFu, in_r = (inside >> 16) & 0xFFu;
        const unsigned out_b = outside & 0xFFu, out_g = (outside >> 8) & 0xFFu,
                       out_r = (outside >> 16) & 0xFFu;
        LIFT_CHECK(in_r >= 240u && in_g <= 16u && in_b <= 16u);
        if (!(in_r >= 240u && in_g <= 16u && in_b <= 16u))
            lift_test::failf(__FILE__, __LINE__, "sprite 在 RT 里 (20,20) 不是红：R=%u G=%u B=%u（0x%08X）",
                             in_r, in_g, in_b, inside);
        LIFT_CHECK(out_r <= 16u && out_g <= 16u && out_b <= 16u);  // 矩形外 = 清屏黑
        LIFT_CHECK(inside != 0u);                                    // 非全 0
    }
    LIFT_CHECK_EQ_U32(f.surfaceUnlockRect(offs), 0u);

    LIFT_CHECK_EQ_U32(f.method(sprite, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(offs, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(surf, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 0u);
    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch-sprite-render-target: driver=%s\n", win.rendererName());
    win.shutdown();
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    return 0;
}

// ---------------------------------------------------------------------------------------
// `--gpu`：**深度跨 target 切换存活**（D3D9：深度属设备，不随 color target 变）
// ---------------------------------------------------------------------------------------
// 红了意味着什么（复审"必须改 ②"的像素级守卫）：
//   把 `need_depth_clear_` 改成"每次换 target 都置位"（模拟"每次换 target 清 depth"的旧行为）⇒
//   A 上写的 z=0.5 在切到 B 后被清成 1.0 ⇒ B 上 z=0.7 的片元被错误接受（B 从蓝变绿）⇒ 第一条断言红。
//   ★ 反向断言防"过度收紧"：切 target 后**显式** `Clear(ZBUFFER)` 一次，z=0.7 必须画出来
//     （⛔ 不是"永远拒绝"就能绿）。
//   ★ 无 SDL_GPU 设备 / 无显示系统 ⇒ SKIP 77。
int caseGpuDepthRetainedAcrossTargetSwitch() {
    lift_test::g_current_case = "d3d9p/depth-retained-across-target-switch";
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return 0;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return 0;

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift gpu-batch depth-retained test";
    opt.driver_list = "software";
    opt.hidden = true;
    opt.raster_gpu = true;
    std::string err;
    if (!win.init(opt, &err)) LIFT_SKIP(err.c_str());
    if (!win.rasterActive()) {
        win.shutdown();
        LIFT_SKIP("rasterActive == false（建不出 SDL_GPU 设备 / 没有渲染后端）");
    }
    f.m.setGpuRaster(win.gpuRaster());

    // 两张同尺寸（64×64 = 后缓冲尺寸）的 32bpp 纹理 RT ⇒ 换 target 时 depth 只按尺寸复用。
    std::uint32_t tex_a = 0;
    std::uint32_t tex_b = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 64u, 64u, lift::d3d9::kFmtX8R8G8B8, &tex_a, 0x1u), 0u);
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 64u, 64u, lift::d3d9::kFmtX8R8G8B8, &tex_b, 0x1u), 0u);
    if (tex_a == 0 || tex_b == 0) return 0;
    const std::uint32_t surf_a = f.textureSurface(tex_a);
    const std::uint32_t surf_b = f.textureSurface(tex_b);
    if (surf_a == 0 || surf_b == 0) return 0;
    lift::d3d9::TextureInfo ti_a;
    lift::d3d9::TextureInfo ti_b;
    LIFT_CHECK(lift::d3d9::textureInfo(f.m.cpu(), tex_a, &ti_a, nullptr));
    LIFT_CHECK(lift::d3d9::textureInfo(f.m.cpu(), tex_b, &ti_b, nullptr));
    lift::GuestSurface* sa = f.m.surfaces().find(ti_a.arena);
    lift::GuestSurface* sb = f.m.surfaces().find(ti_b.arena);
    LIFT_CHECK(sa != nullptr && sb != nullptr);
    if (sa == nullptr || sb == nullptr) return 0;
    // RT 的 guest 像素（A 是 red 的期望值来源；⛔ 不用 `pixelAt` —— 那是后缓冲）。
    auto rt_pixel = [&](const lift::GuestSurface& s, std::uint32_t arena, int x, int y) {
        return f.m.mem().rd32(arena + static_cast<std::uint32_t>(y) * s.pitch +
                              static_cast<std::uint32_t>(x) * 4u);
    };

    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsZEnable, 1u), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsZWriteEnable, 1u), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsZFunc, lift::d3d9::kCmpLess), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0u, 0u, 64u, 64u), 0u);

    // ---- A：清蓝 + 清深度到 1.0；z=0.5 写红（深度写 0.5）----
    LIFT_CHECK_EQ_U32(f.setRenderTarget(surf_a), 0u);
    LIFT_CHECK_EQ_U32(f.clearTo(0xFF0000FFu), 0u);
    LIFT_CHECK_EQ_U32(f.drawQuadZ(0.0f, 0.0f, 32.0f, 32.0f, 0.5f, 0xFFFF0000u), 0u);
    // 切到 B ⇒ 屏障把 A 的队列画进 A 并读回（A 是红）。
    LIFT_CHECK_EQ_U32(f.setRenderTarget(surf_b), 0u);
    const std::uint32_t a16 = rt_pixel(*sa, ti_a.arena, 16, 16);
    LIFT_CHECK_EQ_U32(a16, 0x00FF0000u);

    // ---- B：只清颜色（**不清深度**）；z=0.7 必须被 A 留下的 0.5 拒绝 ----
    LIFT_CHECK_EQ_U32(f.clearTargetOnly(0xFF0000FFu), 0u);
    LIFT_CHECK_EQ_U32(f.drawQuadZ(0.0f, 0.0f, 32.0f, 32.0f, 0.7f, 0xFF00FF00u), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderTarget(0u), 0u);
    const std::uint32_t b16 = rt_pixel(*sb, ti_b.arena, 16, 16);
    LIFT_CHECK_EQ_U32(b16, 0x000000FFu);  // 仍是蓝 ⇒ z=0.7 被深度 0.5 拒绝
    if (b16 != 0x000000FFu)
        lift_test::failf(__FILE__, __LINE__,
                         "切 target 后深度被清掉了：B 的 (16,16) 是 0x%08X（期望仍是清屏蓝 0x000000FF；"
                         "z=0.7 不该画出来）",
                         b16);

    // ---- 反向：切回 B 后**显式 Clear(Z)**，z=0.7 必须画出来（防"永远拒绝"假绿）----
    LIFT_CHECK_EQ_U32(f.setRenderTarget(surf_b), 0u);
    LIFT_CHECK_EQ_U32(f.clearDepthOnly(), 0u);
    LIFT_CHECK_EQ_U32(f.drawQuadZ(0.0f, 0.0f, 32.0f, 32.0f, 0.7f, 0xFF00FF00u), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderTarget(0u), 0u);
    const std::uint32_t b16b = rt_pixel(*sb, ti_b.arena, 16, 16);
    LIFT_CHECK_EQ_U32(b16b, 0x0000FF00u);  // 显式清深度后 z=0.7 通过 ⇒ 绿
    if (b16b != 0x0000FF00u)
        lift_test::failf(__FILE__, __LINE__,
                         "显式 Clear(ZBUFFER) 之后 z=0.7 仍没画出来：B 的 (16,16) 是 0x%08X（期望绿 0x0000FF00）",
                         b16b);

    LIFT_CHECK_EQ_U32(f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(f.method(surf_a, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(surf_b, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex_a, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex_b, 0x08u, {}), 0u);
    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch-depth-retained: driver=%s a=0x%08X b=0x%08X after-clear-z=0x%08X\n",
                win.rendererName(), a16, b16, b16b);
    win.shutdown();
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    return 0;
}

// ---------------------------------------------------------------------------------------
// 夹具助手：带纹理的四边形（pure 与 --gpu 两档共用）
// ---------------------------------------------------------------------------------------
void queueTexturedEx(lift::GpuRaster& gpu, const lift::GpuRasterTexture& tex, float u0, float v0, float u1,
                     float v1, float x0, float y0, float x1, float y1, float z, bool depth_test,
                     std::string* err) {
    const lift::GpuRasterVertex v[6] = {
        {x0, y0, z, 1.0f, 1.0f, 1.0f, 1.0f, u0, v0}, {x1, y0, z, 1.0f, 1.0f, 1.0f, 1.0f, u1, v0},
        {x0, y1, z, 1.0f, 1.0f, 1.0f, 1.0f, u0, v1}, {x1, y0, z, 1.0f, 1.0f, 1.0f, 1.0f, u1, v0},
        {x1, y1, z, 1.0f, 1.0f, 1.0f, 1.0f, u1, v1}, {x0, y1, z, 1.0f, 1.0f, 1.0f, 1.0f, u0, v1}};
    lift::GpuRasterState st;
    st.viewport_w = 64;
    st.viewport_h = 64;
    st.scissor_x1 = 64;
    st.scissor_y1 = 64;
    st.depth_test = depth_test;
    st.depth_write = depth_test;
    st.blend = false;
    st.color_write = 0xFu;
    LIFT_CHECK(gpu.rasterQueue(v, 6, &tex, st, err));
}

void queueTextured(lift::GpuRaster& gpu, const lift::GpuRasterTexture& tex, float u0, float v0, float u1,
                   float v1, float x0, float y0, float x1, float y1, std::string* err) {
    queueTexturedEx(gpu, tex, u0, v0, u1, v1, x0, y0, x1, y1, 0.0f, false, err);
}

// ---------------------------------------------------------------------------------------
// pure：方案 C 的脏矩形判据（不相交 ⇒ **不切段**；相交/写区未知 ⇒ 切段）+ 与真后端同一份纯函数
// ---------------------------------------------------------------------------------------
// ★ 判据会红的情形：
//   · 把"脏区 × 采样区"判成恒真（例如退回旧的 `pending || !queue_.empty()` 一律切） ⇒ cut_count 变 2；
//   · 把判据方向写反（不相交才切）⇒ 第二条断言就红；
//   · d3d9::lockRect/unlockRect 不再记录写区（`content_change_known` 恒 false）⇒ 退化成整幅 ⇒ 切段数变 2。
void caseDirtyRect() {
    lift_test::g_current_case = "gpu-batch/dirty-rect";
    // ---- 纯函数本身（穷举边界；右/下开区间）----
    {
        const lift::GpuTexelRect a{0, 0, 8, 8};
        LIFT_CHECK(lift::gpuTexelRectsIntersect(a, lift::GpuTexelRect{4, 4, 8, 8}));
        LIFT_CHECK(lift::gpuTexelRectsIntersect(a, lift::GpuTexelRect{0, 0, 1, 1}));
        LIFT_CHECK(!lift::gpuTexelRectsIntersect(a, lift::GpuTexelRect{8, 0, 8, 8}));  // 右边界相接不算
        LIFT_CHECK(!lift::gpuTexelRectsIntersect(a, lift::GpuTexelRect{0, 8, 8, 8}));  // 下边界相接不算
        LIFT_CHECK(!lift::gpuTexelRectsIntersect(a, lift::GpuTexelRect{}));            // 空矩形
        const lift::GpuTexelRect samples[2] = {lift::GpuTexelRect{60, 0, 4, 4},
                                               lift::GpuTexelRect{0, 0, 9, 9}};
        LIFT_CHECK(!lift::gpuDirtyRectNeedsCut(lift::GpuTexelRect{40, 40, 16, 16}, samples, 2));
        LIFT_CHECK(lift::gpuDirtyRectNeedsCut(lift::GpuTexelRect{0, 0, 8, 8}, samples, 2));
        LIFT_CHECK(!lift::gpuDirtyRectNeedsCut(lift::GpuTexelRect{0, 0, 8, 8}, nullptr, 0));
        // 采样矩形：u/v 越界（WRAP 会采到整幅）⇒ 保守整幅
        const lift::GpuRasterVertex over[6] = {{0, 0, 0, 1, 1, 1, 1, 1.2f, 0.0f}};
        LIFT_CHECK_EQ_I64(lift::gpuDrawSampleRect(over, 1, 64, 64).w, 64);
    }

    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    f.m.setGpuRaster(&mock);

    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 64, 64, lift::d3d9::kFmtX8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;

    std::string err;
    // ---- 写 A=(0,0)-(8,8)：d3d9 必须把写区记下来（方案 C 的输入）----
    std::uint32_t lr = 0;
    LIFT_CHECK_EQ_U32(f.textureLockRectRect(tex, 0, 0, 8, 8, &lr), 0u);
    LIFT_CHECK(f.fillTexture(tex, 0x00, 0x00, 0xFF));  // 红（BGRA 内存布局）
    LIFT_CHECK_EQ_U32(f.textureUnlockRect(tex), 0u);
    lift::GpuRasterTexture tv1;
    LIFT_CHECK(f.textureView(tex, &tv1));
    LIFT_CHECK(tv1.content_change_known);
    LIFT_CHECK_EQ_I64(tv1.content_change_rect.x, 0);
    LIFT_CHECK_EQ_I64(tv1.content_change_rect.y, 0);
    LIFT_CHECK_EQ_I64(tv1.content_change_rect.w, 8);
    LIFT_CHECK_EQ_I64(tv1.content_change_rect.h, 8);
    LIFT_CHECK_NE_U32(tv1.content_change_prev_version, tv1.content_change_version);

    // 第一条：采样 A（u/v 正好覆盖 (0,0)-(8,8)）
    queueTextured(mock, tv1, 0.0f, 0.0f, 0.125f, 0.125f, 0.0f, 0.0f, 16.0f, 16.0f, &err);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.upload_count), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.cut_count), 0);

    // ---- 写 B=(40,40)-(56,56)：与"已入队那条 draw 的采样区"**不相交** ⇒ 不切段 ----
    LIFT_CHECK_EQ_U32(f.textureLockRectRect(tex, 40, 40, 56, 56, &lr), 0u);
    LIFT_CHECK(f.fillTexture(tex, 0xFF, 0x00, 0x00));  // 蓝
    LIFT_CHECK_EQ_U32(f.textureUnlockRect(tex), 0u);
    lift::GpuRasterTexture tv2;
    LIFT_CHECK(f.textureView(tex, &tv2));
    LIFT_CHECK(tv2.content_change_known);
    LIFT_CHECK_EQ_I64(tv2.content_change_rect.x, 40);
    LIFT_CHECK_EQ_I64(tv2.content_change_prev_version, tv1.content_change_version);
    queueTextured(mock, tv2, 0.625f, 0.625f, 0.875f, 0.875f, 16.0f, 16.0f, 32.0f, 32.0f, &err);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.cut_count), 0);  // ★ 不相交 ⇒ 0 次切段
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.upload_count), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count), 0);  // 切段 ≠ 提交

    // ---- 再写 A（与第一条 draw 的采样区**相交**）⇒ 必须切段 ----
    LIFT_CHECK_EQ_U32(f.textureLockRectRect(tex, 0, 0, 8, 8, &lr), 0u);
    LIFT_CHECK(f.fillTexture(tex, 0x00, 0xFF, 0x00));  // 绿
    LIFT_CHECK_EQ_U32(f.textureUnlockRect(tex), 0u);
    lift::GpuRasterTexture tv3;
    LIFT_CHECK(f.textureView(tex, &tv3));
    queueTextured(mock, tv3, 0.0f, 0.0f, 0.125f, 0.125f, 0.0f, 0.0f, 16.0f, 16.0f, &err);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.cut_count), 1);  // ★ 相交 ⇒ 1 次切段
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.upload_count), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count), 0);

    // ---- 版本没变 ⇒ 不重传、也不切 ----
    lift::GpuRasterTexture tv3b = tv3;
    queueTextured(mock, tv3b, 0.0f, 0.0f, 0.125f, 0.125f, 0.0f, 0.0f, 16.0f, 16.0f, &err);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.cut_count), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.upload_count), 3);

    // ---- 写区未知（`content_change_known=false`）⇒ 整幅 ⇒ 必切（⛔ 不猜）----
    lift::GpuRasterTexture unknown = tv3;
    unknown.content_change_known = false;
    unknown.generation = tv3.generation + 1;
    queueTextured(mock, unknown, 0.0f, 0.0f, 0.125f, 0.125f, 0.0f, 0.0f, 16.0f, 16.0f, &err);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.cut_count), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.upload_count), 4);

    // ---- 最后**一次** flush 收尾（切段不提交；只有这里提交）----
    LIFT_CHECK(mock.rasterFlush(&err));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 5);
    // 已提交但还没读回 ⇒ 仍然是"脏"（与真后端 rasterDirty 的语义一致）
    LIFT_CHECK(mock.rasterDirty());

    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch-dirty-rect: mock cuts=%zu uploads=%zu\n", mock.cut_count, mock.upload_count);
}

// ---------------------------------------------------------------------------------------
// --gpu：真 SDL_GPU 后端的像素 oracle（写红 → 画 → 写绿 → 画）
// ---------------------------------------------------------------------------------------
// ★ 判据会红的情形：
//   · 把纹理上传**推到帧末/提交时**再读 guest 像素 ⇒ 左下角变绿（先画后写被污染）；
//   · 退回"每条 draw 各自 submit" ⇒ `submits` 变 3..4（今天就会红）；
//   · 切段时 color 用 CLEAR（或 CLEAR 代 LOAD）⇒ 左下角被清掉。
int caseGpuContentChange() {
    lift_test::g_current_case = "gpu-batch/content-change";
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return 0;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return 0;

    if (!SDL_Init(SDL_INIT_VIDEO)) LIFT_SKIP("SDL_Init(VIDEO) 失败");
    std::string err;
    std::unique_ptr<lift::GpuRaster> gpu = lift::createSdlGpuRaster(&err);
    if (gpu == nullptr) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        LIFT_SKIP(err.c_str());
    }
    lift::GuestSurface* back_surface = f.m.surfaces().find(dev.info.arena);
    LIFT_CHECK(back_surface != nullptr);
    if (back_surface == nullptr) return 0;
    std::uint8_t* back = f.m.surfaces().pixels(*back_surface);
    const lift::SurfaceFormat fmt = back_surface->format;
    const std::uint32_t pitch = back_surface->pitch;
    LIFT_CHECK(gpu->rasterPrepare(fmt, 64, 64, &err, back));
    LIFT_CHECK(gpu->rasterSyncFromGuest(back, pitch, 64, 64, &err));

    // ---- 4×1 纹理：写红 → 画 → 写绿 → 画 ----
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 4, 1, lift::d3d9::kFmtX8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return 0;
    std::uint32_t lr = 0;
    const lift::GpuRasterStats s0 = lift::gpuRasterStats();
    LIFT_CHECK_EQ_U32(f.textureLockRect(tex, &lr), 0u);  // rect=NULL ⇒ 整幅
    LIFT_CHECK(f.fillTexture(tex, 0x00, 0x00, 0xFF));    // 红
    LIFT_CHECK_EQ_U32(f.textureUnlockRect(tex), 0u);
    lift::GpuRasterTexture red;
    LIFT_CHECK(f.textureView(tex, &red));
    queueTextured(*gpu, red, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 32.0f, 32.0f, &err);
    LIFT_CHECK_EQ_U32(f.textureLockRect(tex, &lr), 0u);
    LIFT_CHECK(f.fillTexture(tex, 0x00, 0xFF, 0x00));  // 绿
    LIFT_CHECK_EQ_U32(f.textureUnlockRect(tex), 0u);
    lift::GpuRasterTexture green;
    LIFT_CHECK(f.textureView(tex, &green));
    queueTextured(*gpu, green, 0.0f, 0.0f, 1.0f, 1.0f, 32.0f, 32.0f, 64.0f, 64.0f, &err);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gpu->rasterQueuedDraws()), 2);
    LIFT_CHECK(gpu->rasterDirty());

    // 读回 = flush + download ⇒ **2** 次提交（⛔ 不是每条 draw 一次）
    LIFT_CHECK(gpu->rasterSyncToGuest(back, pitch, 64, 64, &err));
    const lift::GpuRasterStats s1 = lift::gpuRasterStats();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(s1.cuts - s0.cuts), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(s1.submits - s0.submits), 2);
    // 像素 oracle：先画后写 ⇒ 左下仍是红；写后画 ⇒ 右上是绿
    LIFT_CHECK_EQ_U32(f.pixelAt(0, 0), 0x00FF0000u);
    LIFT_CHECK_EQ_U32(f.pixelAt(32, 32), 0x0000FF00u);
    LIFT_CHECK(!gpu->rasterDirty());

    gpu.reset();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    std::printf("gpu-batch-content-change: submits=%llu cuts=%llu\n",
                static_cast<unsigned long long>(s1.submits - s0.submits),
                static_cast<unsigned long long>(s1.cuts - s0.cuts));
    return 0;
}

// ---------------------------------------------------------------------------------------
// --gpu：**子矩形上传**（真后端像素 oracle；派生 mip 之后 level 0 的脏区口径仍不变）
// ---------------------------------------------------------------------------------------
// ★ 判据会红的情形：
//   · 子矩形上传的 dst 偏移/行距（256 字节行对齐）写错 ⇒ 矩形**外**的像素被改（trace 里 A/C 变红）；
//   · 只传脏区却把整幅当最新（或反之）⇒ B 不是红 / A、C 不是蓝；
//   · 派生级不再随内容重传（`uploads` 少一档）⇒ 差量断言红；
//   · 把派生级也记成"整幅"（`full_uploads` 多于 1）⇒ 红。
int caseGpuSubRectUpload() {
    lift_test::g_current_case = "gpu-batch/subrect-upload";
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return 0;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return 0;

    if (!SDL_Init(SDL_INIT_VIDEO)) LIFT_SKIP("SDL_Init(VIDEO) 失败");
    std::string err;
    std::unique_ptr<lift::GpuRaster> gpu = lift::createSdlGpuRaster(&err);
    if (gpu == nullptr) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        LIFT_SKIP(err.c_str());
    }
    lift::GuestSurface* back_surface = f.m.surfaces().find(dev.info.arena);
    LIFT_CHECK(back_surface != nullptr);
    if (back_surface == nullptr) return 0;
    std::uint8_t* back = f.m.surfaces().pixels(*back_surface);
    const lift::SurfaceFormat fmt = back_surface->format;
    const std::uint32_t pitch = back_surface->pitch;
    LIFT_CHECK(gpu->rasterPrepare(fmt, 64, 64, &err, back));
    LIFT_CHECK(gpu->rasterSyncFromGuest(back, pitch, 64, 64, &err));

    // 8×8 纹理：整幅写蓝（B=FF ⇒ 像素值 0x000000FF）
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 8, 8, lift::d3d9::kFmtX8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return 0;
    std::uint32_t lr = 0;
    const lift::GpuRasterStats s0 = lift::gpuRasterStats();
    LIFT_CHECK_EQ_U32(f.textureLockRect(tex, &lr), 0u);  // rect=NULL ⇒ 整幅
    LIFT_CHECK(f.fillTexture(tex, 0xFF, 0x00, 0x00));    // 蓝
    LIFT_CHECK_EQ_U32(f.textureUnlockRect(tex), 0u);
    lift::GpuRasterTexture blue;
    LIFT_CHECK(f.textureView(tex, &blue));
    // A：采样左上 2×2（texel 0..2）画到 (0,0)-(16,16)
    queueTextured(*gpu, blue, 0.0f, 0.0f, 0.25f, 0.25f, 0.0f, 0.0f, 16.0f, 16.0f, &err);
    // 写红到 (6,6)-(8,8)：与 A 的采样区（texel 0..3）**不相交**
    LIFT_CHECK_EQ_U32(f.textureLockRectRect(tex, 6, 6, 8, 8, &lr), 0u);
    LIFT_CHECK(f.fillTexture(tex, 0x00, 0x00, 0xFF));  // 红
    LIFT_CHECK_EQ_U32(f.textureUnlockRect(tex), 0u);
    lift::GpuRasterTexture red;
    LIFT_CHECK(f.textureView(tex, &red));
    LIFT_CHECK(red.content_change_known);
    LIFT_CHECK_EQ_I64(red.content_change_rect.x, 6);
    LIFT_CHECK_EQ_I64(red.content_change_rect.y, 6);
    LIFT_CHECK_EQ_I64(red.content_change_rect.w, 2);
    // B：采样 (6,6)-(8,8) 画到 (32,32)-(48,48) ⇒ 只上传这 2×2 个 texel
    queueTextured(*gpu, red, 0.75f, 0.75f, 1.0f, 1.0f, 32.0f, 32.0f, 48.0f, 48.0f, &err);
    // C：再采样左上 2×2 画到 (48,48)-(64,64) ⇒ 证明矩形**外**的像素没被这次上传动过
    queueTextured(*gpu, red, 0.0f, 0.0f, 0.25f, 0.25f, 48.0f, 48.0f, 64.0f, 64.0f, &err);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gpu->rasterQueuedDraws()), 3);
    LIFT_CHECK(gpu->rasterSyncToGuest(back, pitch, 64, 64, &err));
    const lift::GpuRasterStats s1 = lift::gpuRasterStats();
    // ★ 派生 mip（`REQ-01M73G2724479XAVS176WBZ65Q`）改了这里的**上传条数**与**切段**口径：
    //   · 每次内容变更 = level 0 一块 + **逐级整级**的派生级（8×8 ⇒ level 1/2/3 共 3 块）；
    //   · 派生级是"整张的其余级"，内容一变就等于整张变 ⇒ 一律切段（⛔ 不再只按脏区判交）。
    //   ⛔ 但**level 0 的脏区口径没变**：一块整幅 + 一块 2×2 子矩形 ⇒ 这一条仍由下面的
    //      `full_uploads` 与 B/C 像素（矩形外没被动过）守着。
    const std::int64_t mips = static_cast<std::int64_t>(lift::d3d9::mipLevelCount(8u, 8u)) - 1;
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(s1.cuts - s0.cuts), 1);  // ★ 有 mip 链 ⇒ 内容变更必须切段
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(s1.full_uploads - s0.full_uploads), 1);  // level 0 只有"整幅"一次
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(s1.uploads - s0.uploads), 2 + 2 * mips);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(s1.submits - s0.submits), 2);
    LIFT_CHECK_EQ_U32(f.pixelAt(0, 0), 0x000000FFu);    // A：蓝（未被动过）
    LIFT_CHECK_EQ_U32(f.pixelAt(32, 32), 0x00FF0000u);  // B：红（子矩形上传真的落对了位置）
    LIFT_CHECK_EQ_U32(f.pixelAt(48, 48), 0x000000FFu);  // C：蓝（矩形外像素不变）

    gpu.reset();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    std::printf("gpu-batch-subrect-upload: cuts=%llu uploads=%llu mips=%lld\n",
                static_cast<unsigned long long>(s1.cuts - s0.cuts),
                static_cast<unsigned long long>(s1.uploads - s0.uploads), static_cast<long long>(mips));
    return 0;
}

// ---------------------------------------------------------------------------------------
// --gpu：**多段渲染**后的深度守卫（现有用例全是 depth_test=false ⇒ 今天覆盖为零）
// ---------------------------------------------------------------------------------------
// ★ 判据会红的情形：切段时**每段都 CLEAR 深度**（或忘了"一帧只清一次"）⇒ 第二段的 draw
//   会被错误接受（红 → 绿），本用例当场红。
int caseGpuDepthSegments() {
    lift_test::g_current_case = "gpu-batch/depth-segments";
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return 0;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return 0;

    if (!SDL_Init(SDL_INIT_VIDEO)) LIFT_SKIP("SDL_Init(VIDEO) 失败");
    std::string err;
    std::unique_ptr<lift::GpuRaster> gpu = lift::createSdlGpuRaster(&err);
    if (gpu == nullptr) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        LIFT_SKIP(err.c_str());
    }
    lift::GuestSurface* back_surface = f.m.surfaces().find(dev.info.arena);
    LIFT_CHECK(back_surface != nullptr);
    if (back_surface == nullptr) return 0;
    std::uint8_t* back = f.m.surfaces().pixels(*back_surface);
    const lift::SurfaceFormat fmt = back_surface->format;
    const std::uint32_t pitch = back_surface->pitch;
    LIFT_CHECK(gpu->rasterPrepare(fmt, 64, 64, &err, back));
    LIFT_CHECK(gpu->rasterSyncFromGuest(back, pitch, 64, 64, &err));  // ⇒ 深度需要清一次

    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 4, 1, lift::d3d9::kFmtX8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return 0;
    std::uint32_t lr = 0;
    auto write = [&](std::uint8_t b, std::uint8_t g, std::uint8_t r, lift::GpuRasterTexture* out) {
        LIFT_CHECK_EQ_U32(f.textureLockRect(tex, &lr), 0u);
        LIFT_CHECK(f.fillTexture(tex, b, g, r));
        LIFT_CHECK_EQ_U32(f.textureUnlockRect(tex), 0u);
        LIFT_CHECK(f.textureView(tex, out));
    };
    const lift::GpuRasterStats s0 = lift::gpuRasterStats();
    lift::GpuRasterTexture t1;
    lift::GpuRasterTexture t2;
    lift::GpuRasterTexture t3;
    // 段 1：z=0.25 写深度（红）
    write(0x00, 0x00, 0xFF, &t1);
    queueTexturedEx(*gpu, t1, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 32.0f, 32.0f, 0.25f, true, &err);
    // 段 2：同一区域 z=0.5（更远）⇒ 必须被深度拒绝；写绿以强制切段
    write(0x00, 0xFF, 0x00, &t2);
    queueTexturedEx(*gpu, t2, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 32.0f, 32.0f, 0.5f, true, &err);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(gpu->rasterQueuedDraws()), 2);
    LIFT_CHECK(gpu->rasterSyncToGuest(back, pitch, 64, 64, &err));
    // ★ 第二段没有把深度清掉 ⇒ z=0.5 被拒绝 ⇒ 仍是红（每段都清 ⇒ 变绿）
    LIFT_CHECK_EQ_U32(f.pixelAt(16, 16), 0x00FF0000u);
    // 段 3：z=0.1（更近）⇒ 必须通过（证明深度测试本身还在工作，不是"全拒绝"）
    write(0xFF, 0x00, 0x00, &t3);  // 蓝
    queueTexturedEx(*gpu, t3, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 32.0f, 32.0f, 0.1f, true, &err);
    LIFT_CHECK(gpu->rasterSyncToGuest(back, pitch, 64, 64, &err));
    LIFT_CHECK_EQ_U32(f.pixelAt(16, 16), 0x000000FFu);
    // 段 4（跨一次 flush）：z=0.9（更远）⇒ 深度必须**还在**（0.1 那一笔），照样被拒
    lift::GpuRasterTexture t4;
    write(0xFF, 0xFF, 0xFF, &t4);  // 白
    queueTexturedEx(*gpu, t4, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 32.0f, 32.0f, 0.9f, true, &err);
    LIFT_CHECK(gpu->rasterSyncToGuest(back, pitch, 64, 64, &err));
    LIFT_CHECK_EQ_U32(f.pixelAt(16, 16), 0x000000FFu);  // 仍是蓝 ⇒ 深度跨 flush 存活
    // 段 5：**颜色同步不许顺带清深度**（生产者在 !rasterDirty() 时每批都会 rasterSyncFromGuest）。
    //   旧实现无条件 need_depth_clear_=true ⇒ 这里 z=0.5 会被错误接受（白）。
    LIFT_CHECK(gpu->rasterSyncFromGuest(back, pitch, 64, 64, &err));
    lift::GpuRasterTexture t5;
    write(0x00, 0xFF, 0xFF, &t5);  // 黄
    queueTexturedEx(*gpu, t5, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 32.0f, 32.0f, 0.5f, true, &err);
    LIFT_CHECK(gpu->rasterSyncToGuest(back, pitch, 64, 64, &err));
    LIFT_CHECK_EQ_U32(f.pixelAt(16, 16), 0x000000FFu);  // 仍是蓝 ⇒ 颜色同步没清深度
    const lift::GpuRasterStats s1 = lift::gpuRasterStats();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(s1.cuts - s0.cuts), 1);  // 只有跨过 flush 的那一次变更要切

    gpu.reset();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    std::printf("gpu-batch-depth-segments: red-then-blue ok\n");
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// --gpu：**纹理宽度**不是 256 字节整数倍时的上传行距（真后端像素 oracle）
// ---------------------------------------------------------------------------------------
// ★ 判据会红的情形（实测过一次）：上传的 staging 行距按 256 对齐（`(w*4+255)&~255`），而
//   SDL_GPU / Metal 实际按**目标区域的宽度**算源行距 ⇒ 每行错开 (对齐后 − w*4) 字节，
//   整张纹理被削成横条（1120x672 的角色层就是这样，房间/OPTION 看上去像"背景撕裂"）。
//   ★ 1280 / 1664 / 2048 宽的纹理 `w*4` 恰好是 256 的倍数 ⇒ 这个坑对它们**不可见**；
//   ★ 纹理**必须带逐像素图案**：纯色纹理即使被削也看不出来（旧用例 `subrect-upload` 的盲点）。
int caseGpuUnalignedRowPitch() {
    lift_test::g_current_case = "gpu-batch/unaligned-row-pitch";
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return 0;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return 0;

    if (!SDL_Init(SDL_INIT_VIDEO)) LIFT_SKIP("SDL_Init(VIDEO) 失败");
    std::string err;
    std::unique_ptr<lift::GpuRaster> gpu = lift::createSdlGpuRaster(&err);
    if (gpu == nullptr) {
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        LIFT_SKIP(err.c_str());
    }
    lift::GuestSurface* back_surface = f.m.surfaces().find(dev.info.arena);
    LIFT_CHECK(back_surface != nullptr);
    if (back_surface == nullptr) return 0;
    std::uint8_t* back = f.m.surfaces().pixels(*back_surface);
    const lift::SurfaceFormat fmt = back_surface->format;
    const std::uint32_t pitch = back_surface->pitch;
    LIFT_CHECK(gpu->rasterPrepare(fmt, 64, 64, &err, back));
    LIFT_CHECK(gpu->rasterSyncFromGuest(back, pitch, 64, 64, &err));

    // 40x20 ⇒ 40*4 = 160 字节/行（**不是** 256 的倍数）
    constexpr int kTw = 40;
    constexpr int kTh = 20;
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, kTw, kTh, lift::d3d9::kFmtX8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return 0;
    {
        lift::d3d9::TextureInfo ti;
        std::string why;
        LIFT_CHECK(lift::d3d9::textureInfo(f.m.cpu(), tex, &ti, &why));
        lift::GuestSurface* tgs = f.m.surfaces().find(ti.arena);
        LIFT_CHECK(tgs != nullptr);
        if (tgs == nullptr) return 0;
        std::uint8_t* px = f.m.surfaces().pixels(*tgs);
        for (int yy = 0; yy < kTh; ++yy) {
            for (int xx = 0; xx < kTw; ++xx) {
                std::uint8_t* p = px + static_cast<std::size_t>(yy) * tgs->pitch +
                                  static_cast<std::size_t>(xx) * 4u;
                p[0] = static_cast<std::uint8_t>(0x10 + xx * 5);                 // B
                p[1] = static_cast<std::uint8_t>(0x20 + yy * 7);                 // G
                p[2] = static_cast<std::uint8_t>(0x30 + (xx + yy) * 3);          // R
                p[3] = 0;
            }
        }
    }
    lift::GpuRasterTexture view;
    LIFT_CHECK(f.textureView(tex, &view));
    // 1:1 画到左上角（uv 0..1 ⇒ 目标像素 == 纹理像素；顶点色白 ⇒ MODULATE 不改变它）
    queueTextured(*gpu, view, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, static_cast<float>(kTw),
                  static_cast<float>(kTh), &err);
    LIFT_CHECK(gpu->rasterSyncToGuest(back, pitch, 64, 64, &err));

    int mismatches = 0;
    for (int yy = 0; yy < kTh; ++yy) {
        for (int xx = 0; xx < kTw; ++xx) {
            const std::uint32_t got = f.pixelAt(xx, yy) & 0x00FFFFFFu;
            const std::uint32_t want =
                ((static_cast<std::uint32_t>(0x30 + (xx + yy) * 3) & 0xFFu) << 16) |
                ((static_cast<std::uint32_t>(0x20 + yy * 7) & 0xFFu) << 8) |
                (static_cast<std::uint32_t>(0x10 + xx * 5) & 0xFFu);
            if (got != want) {
                if (mismatches == 0) {
                    std::fprintf(stderr, "[unaligned-row-pitch] 第一处不同 (%d,%d)：got=0x%06X want=0x%06X\n", xx, yy,
                                 got, want);
                }
                ++mismatches;
            }
        }
    }
    LIFT_CHECK_EQ_I64(mismatches, 0);
    LIFT_CHECK_EQ_U32(f.pixelAt(kTw + 2, 2) & 0x00FFFFFFu, 0u);  // 纹理**外**没被写

    gpu.reset();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    std::printf("gpu-batch-unaligned-row-pitch: 40x20 纹理逐像素一致（%d 处不同）\n", mismatches);
    return 0;
}

// ---------------------------------------------------------------------------------------
// pure：诊断探针的**门控与 texel 选择**（`LIFT_GPU_FF_TEX`；`REQ-01M73P3GXW22Y467YGYPQSM571`）
//
// 红了意味着什么：
//   · env 关闭时探针**一条都不采**（关掉就零开销、零输出）——否则"诊断开不开"会悄悄改变
//     一次运行里多做几次整级下载 / 多几次 fence 等待；
//   · 只采"**带批号**的**纹理** draw"，且只采第一条 —— 否则会在 D3DXSprite / 夹具上误报
//     （它们没有批号，不属于本单的 FF 批）；
//   · texel = `floor(u*W), floor(v*H)` 并钳进 `[0,W-1]×[0,H-1]`，uv 非有限 / 尺寸非法 ⇒ 放弃。
//     这是"GPU 那份 level 0 与 guest 同一 texel 吗"的**定位**判据：算错就等于拿两个不同像素
//     对打（结论会假红或假绿，且看起来都"有读数"）；
//   · alpha 归一：guest 是 X8R8G8B8（`amask == 0`）时上传会把 X 字节写成 255（CPU 采样同口径）
//     ⇒ 比较**不能**拿 guest 的 X 字节当 alpha。
//   ★ 这条必须是 pure CTest（`gpu-batch-order`）：`--gpu` 那条在没有 GPU 设备的机器上 SKIP 77，
//     而探针判据本身与设备无关（真后端只是它的调用方）。
void caseFfTexProbe() {
    lift_test::g_current_case = "gpu-batch/ff-tex-probe";
    // ---- ① 门控 ----
    LIFT_CHECK(!lift::gpuFfTexProbeWanted(nullptr, false, 7u, true));  // env 关 ⇒ 零开销
    LIFT_CHECK(!lift::gpuFfTexProbeWanted("", false, 7u, true));       // 空串 = 关
    LIFT_CHECK(lift::gpuFfTexProbeWanted("1", false, 7u, true));       // 开 + 带批号的纹理 draw
    LIFT_CHECK(!lift::gpuFfTexProbeWanted("1", true, 7u, true));       // 已采过 ⇒ 不再采
    LIFT_CHECK(!lift::gpuFfTexProbeWanted("1", false, 0u, true));      // 无批号（sprite/夹具）⇒ 不采
    LIFT_CHECK(!lift::gpuFfTexProbeWanted("1", false, 7u, false));     // 无纹理 ⇒ 不采
    // ---- ② texel 选择 ----
    int x = -1;
    int y = -1;
    // ★ 用二进制可精确表示的 uv（0.25 / 0.75）：`0.7f * 200` 是 139.9999976 ⇒ floor 得 139，
    //   拿它当"应该 140"会把浮点表示当成判据（探针要判的是 floor+钳制，不是十进制取整）。
    LIFT_CHECK(lift::gpuFfTexProbeTexel(0.25f, 0.75f, 100, 200, &x, &y));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(x), 25);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(y), 150);
    // 右/下边界（u==1.0）钳到最后一列/行 —— ⛔ 不是"放弃"，也不是 0
    LIFT_CHECK(lift::gpuFfTexProbeTexel(1.0f, 1.0f, 100, 200, &x, &y));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(x), 99);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(y), 199);
    // 负 uv（WRAP 之外）钳到 0
    LIFT_CHECK(lift::gpuFfTexProbeTexel(-0.5f, -0.25f, 100, 200, &x, &y));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(x), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(y), 0);
    LIFT_CHECK(!lift::gpuFfTexProbeTexel(std::nanf(""), 0.5f, 100, 200, &x, &y));  // 非有限 ⇒ 放弃
    LIFT_CHECK(!lift::gpuFfTexProbeTexel(0.5f, 0.5f, 0, 200, &x, &y));            // 尺寸非法 ⇒ 放弃
    // ---- ③ 同一 texel 的比较口径 ----
    const std::uint8_t a[4] = {10, 20, 30, 255};
    const std::uint8_t same[4] = {10, 20, 30, 255};
    LIFT_CHECK(lift::gpuFfTexProbeSameTexel(a, same, true));
    const std::uint8_t rgb_diff[4] = {11, 20, 30, 255};
    LIFT_CHECK(!lift::gpuFfTexProbeSameTexel(a, rgb_diff, true));
    // X8R8G8B8：X 字节不是 alpha ⇒ guest 的 X=0 与 GPU 的 255 必须算"同一像素"
    const std::uint8_t x8_guest[4] = {10, 20, 30, 0};
    const std::uint8_t x8_gpu[4] = {10, 20, 30, 255};
    LIFT_CHECK(lift::gpuFfTexProbeSameTexel(x8_guest, x8_gpu, false));
    LIFT_CHECK(!lift::gpuFfTexProbeSameTexel(x8_guest, x8_gpu, true));  // A8R8G8B8：alpha 必须比
    std::printf("gpu-batch/ff-tex-probe: 门控 / texel 选择 / alpha 口径一致\n");
}

// ---------------------------------------------------------------------------------------
// pure：**转发壳**必须把 `Clear(ZBUFFER)` 的清深度请求送到后端
// （`REQ-01M73P3GXW22Y467YGYPQSM571`；`gpu-batch/depth-clear-forwarded`）
//
// 红了意味着什么：
//   `IDirect3DDevice9::Clear(..., D3DCLEAR_ZBUFFER, ...)` 之后，**后端**（`GpuRaster`）必须收到
//   一次 `rasterRequestDepthClear()`。少了它，D32 只在"建 target"时清过一次 ⇒ 上一帧末尾那张
//   全屏 **z=0** 的 quad 把整屏深度写成 0 ⇒ 下一帧所有 `z>0` 的批（角色整层）都被 LESSEQUAL 判掉，
//   而 `fallback` 仍是 0、画面只是"少一层"（**静默**）。
//   ★ 这条错就出在**转发壳**（`SdlWindow` 没 override ⇒ 调到基类空实现），所以断言必须**穿过壳**：
//     用 `setGpuRasterForTest(&mock)` 把后端换成替身，再让 `d3d9::clear` 走 `SdlWindow`。
//     直接 `m.setGpuRaster(&mock)` 是**抓不到**这个缺陷的（那条路本来就直连后端）。
//   ★ 反向也要断言：`Clear(TARGET)`（不带 ZBUFFER）**不许**发清深度请求 —— 否则"每帧都清深度"
//     会把跨帧深度（以及 D3D9 的"Clear(TARGET) 不动深度"契约）一起掩盖掉。
//   ★ 顺带逐条走一遍**有副作用**的转发：漏一条转发是同一类错，而它同样"不报错、不改像素、测试全绿"。
//     ⚠ 未覆盖：`directPresenter`（基类默认返回 nullptr，与"转发到 null"不可区分；它只在
//     `--present window` 的直出集成用例里有意义 ⇒ 留给那条路径，不在这里假装覆盖）。
void caseDepthClearForwarded() {
    lift_test::g_current_case = "gpu-batch/depth-clear-forwarded";
    Fixture f;
    LIFT_CHECK(f.boot(false));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    lift::SdlWindow window;  // ★ 不 init：本用例只测**转发壳**这一层，不建 device / 不 claim 窗口
    // ★ 先取后缓冲：`setGpuRasterForTest` 只在 init 前可用（实现里有 assert），这里建完 device
    //   才注入，所以下面任何早退都必须在注入**之前**。
    lift::GuestSurface* back = f.m.surfaces().find(dev.info.arena);
    LIFT_CHECK(back != nullptr);
    if (back == nullptr) return;
    std::uint8_t* back_px = f.m.surfaces().pixels(*back);
    window.setGpuRasterForTest(&mock);
    // 缝生效：机器看到的是"壳"，不是替身（否则 d3d9::clear 会绕过要守的那一层）。
    LIFT_CHECK(window.gpuRaster() == &window);
    LIFT_CHECK(window.rasterActive());
    f.m.setGpuRaster(&window);

    // ---- ① Clear(TARGET|ZBUFFER) ⇒ 后端**必须**收到 1 次清深度请求 ----
    LIFT_CHECK_EQ_U32(f.clearTo(0xFF102030u), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.depth_clear_count), 1);

    // ---- ② Clear(TARGET)（不带 ZBUFFER）⇒ **不许**再发（+1 也红：说明转发成了无条件）----
    LIFT_CHECK_EQ_U32(f.clearTargetOnly(0xFF405060u), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.depth_clear_count), 1);

    // ---- ③ 其余转发逐条（漏一条 ⇒ 对应计数/返回值不变 ⇒ 红）----
    // ★ 目标身份用**哨兵**（不是后缓冲指针）：断言"转发后拿到的是后端存下的那个值"，
    //   ⛔ 不依赖 Mock::prepare 之外的副作用。
    std::uint8_t sentinel[4] = {0, 0, 0, 0};
    std::string err;
    LIFT_CHECK(window.rasterPrepare(back->format, back->width, back->height, &err, sentinel));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.prepare_count), 1);
    LIFT_CHECK(window.rasterTargetBase() == static_cast<const void*>(sentinel));

    lift::GpuRasterVertex tri[3];
    tri[0].x = 0.0f; tri[0].y = 0.0f;
    tri[1].x = 8.0f; tri[1].y = 0.0f;
    tri[2].x = 0.0f; tri[2].y = 8.0f;
    lift::GpuRasterState st;
    st.viewport_w = 64;
    st.viewport_h = 64;
    LIFT_CHECK(window.rasterQueue(tri, 3, nullptr, st, &err));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(window.rasterQueuedDraws()), 1);
    LIFT_CHECK(window.rasterDirty());  // 入队 ⇒ 脏（走的是后端的 rasterDirty）
    LIFT_CHECK(window.rasterFlush(&err));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count), 1);
    // 旧接口 = 入队 + 立即 flush（契约不变）
    LIFT_CHECK(window.rasterDraw(tri, 3, st, &err));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 2);
    // 带纹理的旧接口也必须到后端（同一类"漏转发"）：Mock 只数入队条数，不会真去采样。
    lift::GpuRasterTexture tex{};  // pixels=nullptr / 0×0 ⇒ Mock 走"无采样矩形"的分支，只记一条 draw
    LIFT_CHECK(window.rasterDrawTextured(tri, 3, tex, st, &err));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.flush_count), 3);
    LIFT_CHECK(window.rasterSyncFromGuest(back_px, back->pitch, back->width, back->height, &err));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.sync_from_count), 1);
    LIFT_CHECK(window.rasterSyncToGuest(back_px, back->pitch, back->width, back->height, &err));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.sync_to_count), 1);
    window.rasterDiscardTarget();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.discard_count), 1);
    LIFT_CHECK(window.rasterTargetBase() == nullptr);  // 后端的目标身份已被丢掉

    f.m.setGpuRaster(nullptr);
    window.setGpuRasterForTest(nullptr);
    std::printf("gpu-batch/depth-clear-forwarded: ZBUFFER=%llu 次转发、TARGET-only 未转发\n",
                static_cast<unsigned long long>(mock.depth_clear_count));
}

// ---------------------------------------------------------------------------------------
// pure：点 / 线**不再**走"只画 CPU"的旁路（`REQ-01M78BF1FNNH6K8N8GEY6GQYXX`）
// ---------------------------------------------------------------------------------------
// 红了意味着什么：
//   · 点 / 线图元重新被当成"GPU 画不了 ⇒ 回退 CPU"（修前就是这样）⇒ `queued_draws` 0（点/线一条都
//     没入队）+ `fallback_draws>0` + `fallback_reasons` 非空 ⇒ 第一条断言红；
//   · 展开出来的三角形数变了（点 = 1px quad = 2 个三角形；每秒线 = 1px 宽的 quad = 2 个三角形）⇒
//     `fragments`（入队顶点数）不是 12 ⇒ 第二条红。⛔ 不锁"精确像素"（裁决：不追求像素一致）。
//   ★ 这条是 **pure**（Mock 后端）⇒ 无 GPU 的机器上也跑得动；真后端的像素判据在 `--gpu` 的
//     `gpu-batch/lines-points` 里（那条没有 GPU 设备就 SKIP 77，⛔ 不拿这条冒充它）。
void caseLinesPointsAdmission() {
    lift_test::g_current_case = "gpu-batch/lines-points-admission";
    lift::d3d9::gpuCoverageReset();
    Fixture f;
    LIFT_CHECK(f.boot(false));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    f.m.setGpuRaster(&mock);
    lift::GuestSurface* back = f.m.surfaces().find(dev.info.arena);
    LIFT_CHECK(back != nullptr);
    if (back == nullptr) return;
    std::uint8_t* back_px = f.m.surfaces().pixels(*back);
    std::string err;
    LIFT_CHECK(mock.rasterPrepare(back->format, back->width, back->height, &err, back_px));
    LIFT_CHECK(mock.rasterSyncFromGuest(back_px, back->pitch, back->width, back->height, &err));

    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0, 0, 64, 64), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);

    // ★ 覆盖账**每次 `Machine::run` 重算**（`machine.cpp` 的 `gpuCoverageReset()`）⇒ 一次 COM 调用
    //   （= 一次 guest run）之后就要读，⛔ 不能攒到最后一次读（那只看到最后一条 draw）。
    // 线：一秒（2 个顶点，y=20，x=8..56）。
    const std::uint32_t line = f.placeXyzRhwDiffuse({{8.0f, 20.0f, 0.0f, 1.0f}, {56.0f, 20.0f, 0.0f, 1.0f}},
                                                    0xFFFF0000u);
    LIFT_CHECK_EQ_U32(f.drawPrimitiveUP(lift::d3d9::kPtLineList, 1u, line, 20u), 0u);
    {
        const lift::d3d9::GpuCoverage cov = lift::d3d9::gpuCoverage();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.queued), 1);          // ★ 线真的进了 GPU 队列
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.fallback_draws), 0);  // ★ 没有 CPU 旁路
        LIFT_CHECK(cov.fallback_reasons.empty());
        if (!cov.fallback_reasons.empty())
            lift_test::failf(__FILE__, __LINE__, "线仍被当成 CPU 回退：首个原因=%s",
                             cov.fallback_reasons.begin()->first.c_str());
        // 一秒线 = 1px 宽的 quad = 2 个三角形 = 6 个顶点（展开成三角形的机械判据；⛔ 不判像素）。
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.fragments), 6);
    }
    // 点：1 个顶点 ⇒ 1px quad ⇒ 6 个顶点。
    const std::uint32_t point = f.placeXyzRhwDiffuse({{32.0f, 40.0f, 0.0f, 1.0f}}, 0xFF00FF00u);
    LIFT_CHECK_EQ_U32(f.drawPrimitiveUP(lift::d3d9::kPtPointList, 1u, point, 20u), 0u);
    const lift::d3d9::GpuCoverage cov = lift::d3d9::gpuCoverage();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.queued), 1);          // ★ 点也真的进了 GPU 队列
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.fallback_draws), 0);  // ★ 没有 CPU 旁路
    LIFT_CHECK(cov.fallback_reasons.empty());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.fragments), 6);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 2);   // Mock 侧：两次入队都到了后端

    LIFT_CHECK_EQ_U32(f.endScene(), 0u);
    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch/lines-points-admission: queued=%llu fragments=%llu fallback=%llu\n",
                static_cast<unsigned long long>(cov.queued),
                static_cast<unsigned long long>(cov.fragments),
                static_cast<unsigned long long>(cov.fallback_draws));
}

// ---------------------------------------------------------------------------------------
// pure：`[draw] fa=` 的 **GPU 版**（α 聚合在批提交时统计，`REQ-01M78BF1FNNH6K8N8GEY6GQYXX`）
// ---------------------------------------------------------------------------------------
// 红了意味着什么：
//   · GPU 批的 α 聚合没被填（恒 0 / `from_gpu=false`）⇒ 一个 α=128 的四边形算不出 128 ⇒ 第一条红。
//     修前正是这样：`fa=` 只在 CPU 的 `rasterTriangle` 里累加，GPU 批恒 `0:0/255/0`。
//   · 统计口径错（拿顶点色以外的量 / 漏乘 255 / 把 α 当 0..1）⇒ 均值落在 128±8 之外 ⇒ 红。
//   ★ 容差 ±8 而不是锁 128：量化（8 bit 顶点色 → float → 再量化）不该被锁死；⛔ 不锁精确值。
//   ★ 这条是 pure（Mock）⇒ 无 GPU 的机器上也跑；它不证明 GPU 画对了像素（那是别的守卫的事）。
void caseAlphaAggregation() {
    lift_test::g_current_case = "gpu-batch/alpha-aggregation";
    lift::d3d9::gpuCoverageReset();
    Fixture f;
    LIFT_CHECK(f.boot(false));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    f.m.setGpuRaster(&mock);
    lift::GuestSurface* back = f.m.surfaces().find(dev.info.arena);
    LIFT_CHECK(back != nullptr);
    if (back == nullptr) return;
    std::uint8_t* back_px = f.m.surfaces().pixels(*back);
    std::string err;
    LIFT_CHECK(mock.rasterPrepare(back->format, back->width, back->height, &err, back_px));
    LIFT_CHECK(mock.rasterSyncFromGuest(back_px, back->pitch, back->width, back->height, &err));

    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0, 0, 64, 64), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    // 开混合（SRCALPHA/INVSRCALPHA）：α 这才"真正参与混合"（口径与 `[draw] fa=` 一致）。
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsAlphaBlendEnable, 1u), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsSrcBlend, lift::d3d9::kBlendSrcAlpha), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsDestBlend, lift::d3d9::kBlendInvSrcAlpha), 0u);

    // 一个 α=128 的四边形（2 个三角形，6 个顶点，都是顶点色 α=128）。
    const std::uint32_t data = f.placeXyzRhwDiffuse(
        {{8.0f, 8.0f, 0.0f, 1.0f}, {56.0f, 8.0f, 0.0f, 1.0f}, {8.0f, 56.0f, 0.0f, 1.0f},
         {56.0f, 8.0f, 0.0f, 1.0f}, {56.0f, 56.0f, 0.0f, 1.0f}, {8.0f, 56.0f, 0.0f, 1.0f}},
        0x80FF0000u);
    LIFT_CHECK_EQ_U32(f.drawPrimitiveUP(lift::d3d9::kPtTriangleList, 2u, data, 20u), 0u);

    const lift::d3d9::DrawAlphaAgg agg = lift::d3d9::lastDrawAlphaAgg();
    LIFT_CHECK(agg.from_gpu);  // ★ 这一批真的走 GPU 批（否则这条判据在数 CPU 的像素）
    if (!agg.from_gpu) lift_test::failf(__FILE__, __LINE__, "α 聚合不是 GPU 批的（from_gpu=false）");
    LIFT_CHECK(agg.n > 0);
    LIFT_CHECK(std::fabs(agg.mean - 128.0) <= 8.0);
    if (std::fabs(agg.mean - 128.0) > 8.0)
        lift_test::failf(__FILE__, __LINE__, "GPU 批 α 均值不在 128±8：n=%llu mean=%.2f (%u..%u)",
                         static_cast<unsigned long long>(agg.n), agg.mean, agg.mn, agg.mx);
    LIFT_CHECK(agg.mn >= 120u && agg.mx <= 136u);

    LIFT_CHECK_EQ_U32(f.endScene(), 0u);
    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch/alpha-aggregation: n=%llu mean=%.1f %u..%u from_gpu=%d\n",
                static_cast<unsigned long long>(agg.n), agg.mean, agg.mn, agg.mx, agg.from_gpu ? 1 : 0);
}

// ---------------------------------------------------------------------------------------
// pure：四处"CPU 兜底"旁路的**显式拒绝 + 记账**（`REQ-01M78BF1FNNH6K8N8GEY6GQYXX`）
// ---------------------------------------------------------------------------------------
// 红的含义（每条都是"删了 CPU 光栅之后会静默不画"的那一类）：
//   (a) `invw != 1`（XYZRHW + rhw=2 ⇒ invw=2）：**不再** CPU 重放 ⇒ Draw 返回 D3DERR_INVALIDCALL，
//       `unsupported` bit21 置位，且 `fallback_draws == 0`（⛔ 不是"回退 CPU 画了"）。
//       修前：这一批由 CPU 重放 ⇒ 返回 kOk、bit21 不置位、fallback=1 ⇒ 红。
//   (b) `MIPFILTER=NONE` 且**缩小采样**（2×2 纹理铺到 8×8 像素 ⇒ LOD≥1）：同上，bit22。
//       修前：CPU 重放 ⇒ 返回 kOk、fallback=1 ⇒ 红。
//   (c) GPU 后端**入队失败**：bit23 置位 + `gpu_failures == 1` + Draw 返回 INVALIDCALL、
//       且 `fallback_draws == 0`。修前：CPU 重放这一批 ⇒ 返回 kOk、fallback=1 ⇒ 红。
//   ★ 三条都**只**用 Mock 后端 ⇒ pure 档（无 GPU 的机器上也跑）。
//   ★ 反向条款：(a)(b) 的拒绝必须**只**由那一条判据触发 —— 同状态、rhw=1 / mip 不缩小的时候
//     必须进 GPU（否则"永远拒绝"也能绿）。
bool bitIsSet(const lift::d3d9::PipelineInfo& pi, std::uint32_t bit) {
    return (pi.unsupported[bit / 32u] & (1u << (bit % 32u))) != 0u;
}

void caseGpuRejectAccounting() {
    lift_test::g_current_case = "gpu-batch/reject-accounting";
    lift::d3d9::gpuCoverageReset();
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    f.m.setGpuRaster(&mock);
    lift::GuestSurface* back = f.m.surfaces().find(dev.info.arena);
    LIFT_CHECK(back != nullptr);
    if (back == nullptr) return;
    std::uint8_t* back_px = f.m.surfaces().pixels(*back);
    std::string err;
    LIFT_CHECK(mock.rasterPrepare(back->format, back->width, back->height, &err, back_px));
    LIFT_CHECK(mock.rasterSyncFromGuest(back_px, back->pitch, back->width, back->height, &err));

    auto infoBits = [&](std::uint32_t bit) {
        lift::d3d9::PipelineInfo pi;
        if (!lift::d3d9::pipelineInfoOfDevice(f.m.cpu(), dev.state, &pi, nullptr)) return false;
        return bitIsSet(pi, bit);
    };

    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0u, 0u, 64u, 64u), 0u);

    // ---- (a) invw != 1（rhw=2 ⇒ invw=2）⇒ 拒绝 + bit21 + ⛔ 不回退 CPU ----
    const std::uint32_t persp = f.placeXyzRhwDiffuse(
        {{0.0f, 0.0f, 0.0f, 2.0f}, {32.0f, 0.0f, 0.0f, 2.0f}, {0.0f, 32.0f, 0.0f, 2.0f}}, 0xFFFF0000u);
    LIFT_CHECK_EQ_U32(f.drawPrimitiveUP(lift::d3d9::kPtTriangleList, 1u, persp, 20u),
                      lift::d3d9::kInvalidCall);
    LIFT_CHECK(infoBits(21));  // kUnGpuPerspective
    {
        const lift::d3d9::GpuCoverage cov = lift::d3d9::gpuCoverage();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.fallback_draws), 0);  // ⛔ 没有 CPU 重放
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.queued), 0);          // ⛔ 也没进 GPU 队列
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 0);

    // ---- 反向：同一状态、rhw=1 ⇒ 必须进 GPU（证明 (a) 的拒绝只来自 invw）----
    const std::uint32_t flat = f.placeXyzRhwDiffuse(
        {{0.0f, 0.0f, 0.0f, 1.0f}, {32.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 32.0f, 0.0f, 1.0f}}, 0xFFFF0000u);
    LIFT_CHECK_EQ_U32(f.drawPrimitiveUP(lift::d3d9::kPtTriangleList, 1u, flat, 20u), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 1);

    // ---- (b) MIPFILTER=NONE 且缩小采样（2×2 纹理 → 8×8 像素）⇒ 拒绝 + bit22 ----
    // ★ 8×8 纹理铺到 **4×4 像素** ⇒ 每像素 2 texel（LOD=1 ⇒ 真的在缩小采样）。
    //   ⛔ 不要用"2×2 纹理铺 8×8 像素"：那是**放大**（footprint 0.25 texel/px ⇒ LOD<0），
    //   `triangleMipLevel` 合法地返回 0 ⇒ 这条判据永远不触发（实测踩过）。
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, 8u, 8u, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    {
        std::uint32_t locked = 0;
        LIFT_CHECK_EQ_U32(f.textureLockRect(tex, &locked), 0u);
        const std::uint32_t pitch = f.m.mem().rd32(locked + 0u);
        const std::uint32_t bits = f.m.mem().rd32(locked + 4u);
        for (int y = 0; y < 8; ++y)
            for (int x = 0; x < 8; ++x)
                f.m.mem().wr32(bits + static_cast<std::uint32_t>(y) * pitch +
                                   static_cast<std::uint32_t>(x) * 4u,
                               0xFF808080u);
        LIFT_CHECK_EQ_U32(f.textureUnlockRect(tex), 0u);
    }
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuseTex1), 0u);
    LIFT_CHECK_EQ_U32(f.setTexture(0u, tex), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampAddressU, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampAddressV, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfPoint), 0u);
    const std::vector<Fixture::TexVertex> mip_quad = {
        {0.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
        {4.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {0.0f, 4.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
        {4.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {4.0f, 4.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
        {0.0f, 4.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
    };
    const std::uint32_t quad = f.placeTexVertices(mip_quad);
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampMipFilter, lift::d3d9::kTexfNone), 0u);
    LIFT_CHECK_EQ_U32(f.drawTexturedQuad(quad), lift::d3d9::kInvalidCall);
    LIFT_CHECK(infoBits(22));  // kUnGpuMipDivergence
    {
        const lift::d3d9::GpuCoverage cov = lift::d3d9::gpuCoverage();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.fallback_draws), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.queued), 0);
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 1);

    // ---- 反向：MIPFILTER=POINT（三线性近似可表达）⇒ 同一批**必须**进 GPU ----
    LIFT_CHECK_EQ_U32(f.setSampler(0u, lift::d3d9::kSampMipFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(f.drawTexturedQuad(quad), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 2);

    // ---- (c) GPU 后端入队失败 ⇒ 拒绝 + bit23 + gpu_failures，⛔ 不 CPU 重放 ----
    mock.fail_queue = true;
    LIFT_CHECK_EQ_U32(f.drawTexturedQuad(quad), lift::d3d9::kInvalidCall);
    LIFT_CHECK(infoBits(23));  // kUnGpuBackendFailure
    {
        const lift::d3d9::GpuCoverage cov = lift::d3d9::gpuCoverage();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.gpu_failures), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.fallback_draws), 0);
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 2);  // 失败的那一条没算作入队
    mock.fail_queue = false;

    LIFT_CHECK_EQ_U32(f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setTexture(0u, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 0u);  // Release
    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch/reject-accounting: invw=%d mip=%d backend=%d（均拒绝 + 记账，⛔ 不回退 CPU）\n",
                infoBits(21) ? 1 : 0, infoBits(22) ? 1 : 0, infoBits(23) ? 1 : 0);
}

// ---------------------------------------------------------------------------------------
// pure：**跳过账本**（`REQ-01M7AAXVGY9SG75XCREHPX641Z`，M1）
// ---------------------------------------------------------------------------------------
// 红了意味着什么：
//   · 一批被 `gpuEligible` 拒（这里用 `ALPHATESTENABLE=1` ⇒ bit30）却**没**记进 `skipped`
//     ⇒ `skipped_draws == 1` 红（修前根本没有 `skipped_draws` 这个字段 / 摘要里没有 `skipped=`）；
//   · 原因没进 `skipped top:` ⇒ `gpuSkippedTopLine()` 里找不到那句话 ⇒ 红；
//   · 具名位没进 `unsupported :` ⇒ `unsupportedBitsOfRun()` 里没有 `bit30=…` ⇒ 红；
//   · **反向条款**：真实放行的批也被算成"跳过" ⇒ 第一段的 `skipped_draws == 0` 红
//     （防"永远记跳过"也能绿）。
//   ★ `fallback` 与 `skipped` 是**两个口径**，本单故意让它们同时可见：
//     · `skipped` = GPU 没画（准入拒绝）；
//     · `fallback` = CPU 真写了像素（本单不改像素行为 ⇒ 被拒的批**仍由 CPU 补画** ⇒ 也是 1）。
//     M3 翻成"跳过"之后 `fallback` 才会归 0（那时改的是这条断言，不是把两个数合流）。
//   ★ pure（Mock）：无 GPU 的机器上也跑得动。
void caseGpuSkipLedger() {
    lift_test::g_current_case = "gpu-batch/skip-ledger";
    lift::d3d9::gpuCoverageReset();
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    MockRaster mock;
    f.m.setGpuRaster(&mock);
    lift::GuestSurface* back = f.m.surfaces().find(dev.info.arena);
    LIFT_CHECK(back != nullptr);
    if (back == nullptr) return;
    std::uint8_t* back_px = f.m.surfaces().pixels(*back);
    std::string err;
    LIFT_CHECK(mock.rasterPrepare(back->format, back->width, back->height, &err, back_px));
    LIFT_CHECK(mock.rasterSyncFromGuest(back_px, back->pitch, back->width, back->height, &err));

    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsAlphaTestEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0u, 0u, 64u, 64u), 0u);

    const std::uint32_t tri = f.placeXyzRhwDiffuse(
        {{0.0f, 0.0f, 0.0f, 1.0f}, {32.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 32.0f, 0.0f, 1.0f}}, 0xFFFF0000u);

    // ---- ① 反向条款：真实放行的批 ⇒ `skipped == 0`（不是"永远记跳过"）----
    LIFT_CHECK_EQ_U32(f.drawPrimitiveUP(lift::d3d9::kPtTriangleList, 1u, tri, 20u), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 1);
    {
        const lift::d3d9::GpuCoverage cov = lift::d3d9::gpuCoverage();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.gpu_draws), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.skipped_draws), 0);
        LIFT_CHECK(cov.skipped_reasons.empty());
        LIFT_CHECK(lift::d3d9::gpuSkippedTopLine().empty());  // 没有跳过 ⇒ 不打 `skipped top:`
    }

    // ---- ② `gpuEligible` 拒批（ALPHATESTENABLE=1）⇒ skipped=1 + 具名位 + 原因进 top ----
    // ★ 返回码**本单不改**：准入拒绝今天仍由 CPU 补画 ⇒ `submitPrimitives` 走完 ⇒ Draw 返回 S_OK
    //   （M3 翻成"跳过"时才变 `D3DERR_INVALIDCALL`；这里锁的是"本步不改行为"）。
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsAlphaTestEnable, 1u), 0u);
    LIFT_CHECK_EQ_U32(f.drawPrimitiveUP(lift::d3d9::kPtTriangleList, 1u, tri, 20u), 0u);
    const std::string reason = "GPU 不支持 alpha test（ALPHATESTENABLE=1）";
    std::uint64_t skipped_seen = 0;
    {
        const lift::d3d9::GpuCoverage cov = lift::d3d9::gpuCoverage();
        // ★ 这一条是"修前没有 skipped 字段 ⇒ 红"的判据。
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.skipped_draws), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.skipped_reasons.size()), 1);
        if (cov.skipped_reasons.size() == 1) {
            LIFT_CHECK(cov.skipped_reasons.begin()->first == reason);
            LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.skipped_reasons.begin()->second), 1);
        }
        // ★ 两个口径**分开**：CPU 仍补画了这一批（本单不改像素行为）⇒ fallback 也 +1，gpu 仍是 0。
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.fallback_draws), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.gpu_draws), 0);
        skipped_seen = cov.skipped_draws;
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 1);  // 被拒的批没入队
    {
        const std::string top = lift::d3d9::gpuSkippedTopLine();
        LIFT_CHECK(!top.empty());
        LIFT_CHECK(top.find("skipped top:") != std::string::npos);
        LIFT_CHECK(top.find("[1×] ") != std::string::npos);
        LIFT_CHECK(top.find(reason) != std::string::npos);
        if (top.find(reason) == std::string::npos)
            lift_test::failf(__FILE__, __LINE__, "skipped top 里没有该原因：%s", top.c_str());
    }
    // 具名位也真的进了 `unsupported :` 的可读名字表（bit30）。
    {
        const std::string bits = lift::d3d9::unsupportedBitsOfRun();
        LIFT_CHECK(bits.find("bit30=GPU 不支持 alpha test") != std::string::npos);
        if (bits.find("bit30=GPU 不支持 alpha test") == std::string::npos)
            lift_test::failf(__FILE__, __LINE__, "unsupported 里没有 bit30 的具名位：%s", bits.c_str());
    }

    // ---- ③ 同状态改回可表达值 ⇒ 放行（证明跳过只来自那一条判据）----
    // ★ 覆盖账**每次 `Machine::run` 重算**（`machine.cpp` 的 `gpuCoverageReset()`）⇒ 这一次读到的
    //   只反映这一批：`skipped == 0`、`gpu == 1`（⛔ 不要拿累计值断言）。
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsAlphaTestEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.drawPrimitiveUP(lift::d3d9::kPtTriangleList, 1u, tri, 20u), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.queued_draws), 2);
    {
        const lift::d3d9::GpuCoverage cov = lift::d3d9::gpuCoverage();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.skipped_draws), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.gpu_draws), 1);
        LIFT_CHECK(cov.skipped_reasons.empty());
    }

    LIFT_CHECK_EQ_U32(f.endScene(), 0u);
    f.m.setGpuRaster(nullptr);
    std::printf("gpu-batch/skip-ledger: skipped=%llu top=[%s]（放行批 skipped=0）\n",
                static_cast<unsigned long long>(skipped_seen), reason.c_str());
}

// ---------------------------------------------------------------------------------------
// `--gpu`：点 / 线在**真后端**上真的画进后缓冲（`REQ-01M78BF1FNNH6K8N8GEY6GQYXX` 的像素臂）
// ---------------------------------------------------------------------------------------
// 红了意味着什么：
//   ① 线（LINELIST，一秒，红色）读回后该行**有非背景像素**。修前点/线只走 CPU ⇒ 这一批回退，
//      `fallback_draws>0`（第 ③ 条）+ 线上像素取决于 CPU 是否被同步读过 ⇒ 两条都红；
//   ② 点（POINTLIST，(32,40)，绿色）那一个像素**非背景**（点必须落成 1px quad，⛔ 不能"准入通过但 0 像素"）；
//   ③ 账本：`fallback_draws == 0`、`queued >= 2`、`gpu_failures == 0` —— 真的走 GPU 批、没回退；
//   ④ 反向：`CULLMODE` 保持默认（CCW）也不能让线消失 —— 展开出来的 quad 必须绕过剔除。
//   ★ 没有 SDL_GPU 设备 / 没有显示系统 ⇒ SKIP 77（⛔ 不拿 pure 的 admission 用例冒充"画出来了"）。
int caseGpuLinesPoints() {
    lift_test::g_current_case = "gpu-batch/lines-points";
    lift::d3d9::gpuCoverageReset();
    Fixture f;
    LIFT_CHECK(f.boot(true));
    const std::uint32_t d3d = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d, 0u);
    if (d3d == 0) return 0;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return 0;

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift gpu-batch lines-points test";
    opt.driver_list = "software";
    opt.hidden = true;
    opt.raster_gpu = true;
    std::string err;
    if (!win.init(opt, &err)) LIFT_SKIP(err.c_str());
    if (!win.rasterActive()) {
        win.shutdown();
        LIFT_SKIP("rasterActive == false（建不出 SDL_GPU 设备 / 没有渲染后端）");
    }
    f.m.setGpuRaster(win.gpuRaster());

    lift::GuestSurface* back = f.m.surfaces().find(dev.info.arena);
    LIFT_CHECK(back != nullptr);
    if (back == nullptr) return 0;
    std::uint8_t* back_px = f.m.surfaces().pixels(*back);
    const lift::SurfaceFormat fmt = back->format;
    const int w = back->width;
    const int h = back->height;
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(w), 64u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(h), 64u);
    LIFT_CHECK(win.rasterPrepare(fmt, w, h, &err, back_px));
    LIFT_CHECK(win.rasterSyncFromGuest(back_px, back->pitch, w, h, &err));

    LIFT_CHECK_EQ_U32(f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(f.setViewport(0, 0, 64, 64), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    // ★ ④ 剔除保持默认（CULLMODE=3/CCW）：线的 quad 朝向由端点顺序决定 ⇒ 若展开的三角形被剔除，
    //   一种朝向的线就消失（这里正好只有一条线，所以"消失"= 线上 0 个非背景像素）。
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullCcw), 0u);
    LIFT_CHECK_EQ_U32(f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.clearTo(0xFF0000FFu), 0u);  // 背景 = 蓝

    // ★ 覆盖账**每次 `Machine::run` 重算**（`machine.cpp` 的 `gpuCoverageReset()`）⇒ 每条 Draw
    //   之后立刻读（⛔ 不能攒到最后：那只看到最后一条 draw，"线有没有回退"就丢了）。
    // 线：y=20 那一行，x=8..56，红。
    const std::uint32_t line = f.placeXyzRhwDiffuse({{8.0f, 20.0f, 0.0f, 1.0f}, {56.0f, 20.0f, 0.0f, 1.0f}},
                                                    0xFFFF0000u);
    LIFT_CHECK_EQ_U32(f.drawPrimitiveUP(lift::d3d9::kPtLineList, 1u, line, 20u), 0u);
    {
        const lift::d3d9::GpuCoverage line_cov = lift::d3d9::gpuCoverage();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(line_cov.fallback_draws), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(line_cov.queued), 1);
        if (line_cov.fallback_draws != 0)
            lift_test::failf(__FILE__, __LINE__, "线仍在回退 CPU：fallback_draws=%llu",
                             static_cast<unsigned long long>(line_cov.fallback_draws));
    }
    // 点：(32,40)，绿。
    const std::uint32_t point = f.placeXyzRhwDiffuse({{32.0f, 40.0f, 0.0f, 1.0f}}, 0xFF00FF00u);
    LIFT_CHECK_EQ_U32(f.drawPrimitiveUP(lift::d3d9::kPtPointList, 1u, point, 20u), 0u);
    const lift::d3d9::GpuCoverage cov = lift::d3d9::gpuCoverage();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.fallback_draws), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.queued), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.gpu_failures), 0);

    // 读回：点/线是**排队**的（合批）⇒ 必须显式 sync 才能从 guest 像素看见。
    LIFT_CHECK(win.rasterSyncToGuest(back_px, back->pitch, w, h, &err));

    const std::uint32_t background = 0x000000FFu;  // X8R8G8B8：清屏蓝（X 字节 = 0）
    LIFT_CHECK_EQ_U32(f.pixelAt(0, 0), background);  // 基线：读回真的落地了
    // ① 线上非背景（整行统计：⛔ 不锁"某列必须红"——展开是 1px quad，端点吸附/量化允许差一两列）
    int line_px = 0;
    for (int x = 0; x < w; ++x) {
        if (f.pixelAt(x, 20) != background) ++line_px;
    }
    LIFT_CHECK(line_px >= 8);
    if (line_px < 8)
        lift_test::failf(__FILE__, __LINE__, "线上的非背景像素只有 %d 个（y=20 那一行；点/线没画到 GPU？）",
                         line_px);
    // ② 点那一个像素非背景（绿：R/B 低、G 高 —— 用通道判，⛔ 不锁精确字节）
    const std::uint32_t dot = f.pixelAt(32, 40);
    const unsigned dr = (dot >> 16) & 0xFFu, dg = (dot >> 8) & 0xFFu, db = dot & 0xFFu;
    LIFT_CHECK(dot != background);
    LIFT_CHECK(dg >= 200u && dr <= 60u && db <= 60u);
    if (dot == background)
        lift_test::failf(__FILE__, __LINE__, "点 (32,40) 还是背景色 0x%08X", dot);
    // ③ 账本：两条 draw 各自读到 `fallback=0 / queued=1`（上面已断言）⇒ 真的走 GPU 批、没有回退。

    f.m.setGpuRaster(nullptr);
    LIFT_CHECK_EQ_U32(f.endScene(), 0u);
    std::printf("gpu-batch/lines-points: line_px=%d dot=0x%08X fallback=%llu queued=%llu\n", line_px, dot,
                static_cast<unsigned long long>(cov.fallback_draws),
                static_cast<unsigned long long>(cov.queued));
    win.shutdown();
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    return 0;
}

// =======================================================================================
// pure：**启动期**守卫（`REQ-01M78BF1FNNH6K8N8GEY6GQYXX`）
// =======================================================================================
// 两条判据都落在**进程边界**上（退出码 + stderr 文案）⇒ 必须起真子进程：
//   CTest 的 `WILL_FAIL` 只看退出码，看不见"文案有没有指路"。
// ★ 输出走**文件重定向**，⛔ 不走管道：受限沙箱里管道会 EPERM（AGENTS.md §5 环境口径 ①）。
struct ChildOutput {
    bool started = false;
    int exit_code = -1;
    std::string text;  // stdout + stderr 合并
};

ChildOutput runCaptured(const std::filesystem::path& exe, const std::vector<std::string>& args) {
    namespace fs = std::filesystem;
    ChildOutput r;
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec || dir.empty()) dir = fs::current_path(ec);
    const fs::path out_path = dir / "lift-guard-child.log";
    std::error_code rmec;
    fs::remove(out_path, rmec);

#if defined(_WIN32)
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    const HANDLE h = ::CreateFileW(out_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return r;
    std::wstring joined;
    for (const std::string& a : args) joined += L" \"" + lift::utf8ToWide(a) + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = h;
    si.hStdError = h;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe.wstring() + L"\"" + joined;
    const BOOL ok = ::CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                     nullptr, nullptr, &si, &pi);
    ::CloseHandle(h);
    if (!ok) return r;
    r.started = true;
    const DWORD waited = ::WaitForSingleObject(pi.hProcess, 120000u);
    DWORD code = 0xFFFFFFFFu;
    ::GetExitCodeProcess(pi.hProcess, &code);
    if (waited != WAIT_OBJECT_0) {
        ::TerminateProcess(pi.hProcess, 991u);
        ::WaitForSingleObject(pi.hProcess, 5000u);
        code = 991u;
    }
    r.exit_code = static_cast<int>(code);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
#else
    const pid_t pid = ::fork();
    if (pid < 0) return r;
    if (pid == 0) {
        const int fd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd >= 0) {
            ::dup2(fd, 1);
            ::dup2(fd, 2);
            if (fd > 2) ::close(fd);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(exe.c_str()));
        for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        ::execv(exe.c_str(), argv.data());
        ::_exit(127);
    }
    r.started = true;
    int status = 0;
    ::waitpid(pid, &status, 0);
    r.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

    std::ifstream in(out_path, std::ios::binary);
    if (in) r.text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    fs::remove(out_path, rmec);
    return r;
}

// ① 无 GPU ⇒ **拒绝启动**（非 0 退出 + stderr 明确指路）。
//    ★ 环境强制"没有 GPU"：`SDL_VIDEODRIVER=dummy` 下 `SDL_CreateGPUDevice` 必然失败
//      （⛔ 不靠真拔显卡）。后端在**执行源定位之前**建 ⇒ 这条不需要任何游戏资产。
//    红了意味着什么：又把"GPU 不可用"降级成**静默继续**（下一套光栅 / 空光栅）⇒
//    子进程会一路走到"找不到执行源"或真的开跑，stderr 里没有那句 GPU 拒绝文案。
void caseNoGpuRefusesStart(const std::filesystem::path& exe) {
    lift_test::g_current_case = "lift/no-gpu-refuses-start";
#if defined(_WIN32)
    _putenv_s("SDL_VIDEODRIVER", "dummy");
#else
    ::setenv("SDL_VIDEODRIVER", "dummy", 1);
#endif
    const ChildOutput r = runCaptured(exe, {"--present", "headless"});
    LIFT_CHECK(r.started);
    if (!r.started) return;
    LIFT_CHECK(r.exit_code != 0);
    // 文案必须指路：既说清"没有 GPU 所以不启动"，也给出驱动/无桌面的方向。
    LIFT_CHECK(r.text.find("拒绝启动") != std::string::npos);
    LIFT_CHECK(r.text.find("GPU") != std::string::npos);
    std::printf("lift/no-gpu-refuses-start: exit=%d（stderr 含 GPU 拒绝文案）\n", r.exit_code);
}

// ② 已删的参数 `--raster`（连同它的 `cpu` 值）⇒ 必须是**未知参数**（exit 2），⛔ 不是静默忽略。
//    红了意味着什么：`--raster` 的解析被加回来了 ⇒ 那个"显式 CPU 后端"开关又活了。
void caseRasterFlagGone(const std::filesystem::path& exe) {
    lift_test::g_current_case = "lift/raster-flag-gone";
    const ChildOutput r = runCaptured(exe, {"--raster", "cpu"});
    LIFT_CHECK(r.started);
    if (!r.started) return;
    LIFT_CHECK_EQ_I64(r.exit_code, 2);
    LIFT_CHECK(r.text.find("未知参数") != std::string::npos);
    LIFT_CHECK(r.text.find("--raster") != std::string::npos);
    std::printf("lift/raster-flag-gone: exit=%d（未知参数 --raster）\n", r.exit_code);
}

// ③ `--present headless` 也必须是**隐藏窗口**，并且日志里要打出 `present : … window=hidden`。
//    ★ 隐藏标志在 `present_sdl.cpp` 的 `SDL_CreateWindow(SDL_WINDOW_HIDDEN)`；日志值来自
//      `windowHidden()`（读 `SDL_GetWindowFlags`）—— 所以这条**同时**守"窗口真的隐藏"与
//      "headless 也打 present 行"两件事。红了 ≈ 那一行又被塞回 `if (present_mode != "headless")`，
//      或隐藏标志丢了（`window=visible`）。
//    ★ 需要真实 GPU（headless 也要 GPU 光栅）：无 GPU ⇒ 子进程拒绝启动 ⇒ `LIFT_SKIP(77)`。
//      执行源自造最小 PE（一节 `nop`，`--max-insns 1`）⇒ 不需要游戏资产；`present` 行在
//      **载入之后、运行之前**打印，所以判定不依赖真跑两帧（`--max-frames 2` 保留原命令形状）。
//    ★ CTest 宿主：`gpu-batch-barrier`（`--gpu` 臂，已 SKIP 77；它先 unset `SDL_VIDEODRIVER`，
//      子进程因此拿到真实视频驱动）。守卫名 = `gpu-batch/headless-hidden-window`。
std::vector<std::uint8_t> minimalPe() {
    const std::size_t kPeOff = 0x40;
    const std::size_t kOpt = kPeOff + 24;
    const std::size_t kOptSize = 0xE0;
    const std::size_t kSec = kOpt + kOptSize;
    std::vector<std::uint8_t> b(0x400, 0);
    auto w16 = [&b](std::size_t off, std::uint16_t v) { std::memcpy(b.data() + off, &v, sizeof(v)); };
    auto w32 = [&b](std::size_t off, std::uint32_t v) { std::memcpy(b.data() + off, &v, sizeof(v)); };
    b[0] = 'M';
    b[1] = 'Z';
    w32(0x3C, static_cast<std::uint32_t>(kPeOff));
    b[kPeOff + 0] = 'P';
    b[kPeOff + 1] = 'E';
    w16(kPeOff + 4, 0x014Cu);   // Machine = i386
    w16(kPeOff + 6, 1u);        // NumberOfSections
    w16(kPeOff + 20, static_cast<std::uint16_t>(kOptSize));
    w16(kPeOff + 22, 0x0102u);  // EXECUTABLE_IMAGE | 32BIT_MACHINE
    w16(kOpt + 0, 0x010Bu);     // Magic = PE32
    w32(kOpt + 16, 0x1000u);    // AddressOfEntryPoint
    w32(kOpt + 28, 0x00400000u);
    w32(kOpt + 56, 0x2000u);    // SizeOfImage
    w32(kOpt + 60, 0x200u);     // SizeOfHeaders
    w32(kOpt + 72, 0x100000u);  // SizeOfStackReserve
    std::memcpy(b.data() + kSec, ".text", 5);
    w32(kSec + 8, 0x1000u);
    w32(kSec + 12, 0x1000u);
    w32(kSec + 16, 0x200u);
    w32(kSec + 20, 0x200u);
    w32(kSec + 36, 0x60000020u);
    b[0x200] = 0x90u;           // nop
    return b;
}

int caseHeadlessHiddenWindow(const std::filesystem::path& exe) {
    namespace fs = std::filesystem;
    lift_test::g_current_case = "gpu-batch/headless-hidden-window";
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec);
    if (ec || dir.empty()) dir = fs::current_path(ec);
    const fs::path pe = dir / "lift-headless-hidden-window.exe";
    {
        std::ofstream out(pe, std::ios::binary | std::ios::trunc);
        const std::vector<std::uint8_t> bytes = minimalPe();
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            lift_test::failf(__FILE__, __LINE__, "写不了临时 PE：%s", pe.string().c_str());
            return 1;
        }
    }
    const ChildOutput r = runCaptured(exe, {"--exe", pe.string(), "--game-dir", dir.string(), "--lst",
                                            (dir / "missing-headless.lst").string(), "--present", "headless",
                                            "--max-frames", "2", "--max-insns", "1", "--skip-logo", "--audio",
                                            "off"});
    std::error_code rmec;
    fs::remove(pe, rmec);
    LIFT_CHECK(r.started);
    if (!r.started) return 1;
    LIFT_SKIP_IF_NO_GPU(r.exit_code, r.text);  // 无 GPU：这是环境缺失，不是判据红
    LIFT_CHECK_EQ_I64(r.exit_code, 0);
    const std::size_t pos = r.text.find("present   :");
    LIFT_CHECK(pos != std::string::npos);
    if (pos == std::string::npos) {
        std::fprintf(stderr, "没有 present 行；子进程日志前 2000 字节：\n%.2000s\n", r.text.c_str());
        return 1;
    }
    const std::size_t eol = r.text.find('\n', pos);
    const std::string line = r.text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
    std::printf("gpu-batch/headless-hidden-window: %s\n", line.c_str());
    LIFT_CHECK(line.find("window=hidden") != std::string::npos);
    LIFT_CHECK(line.find("window=visible") == std::string::npos);
    // 值必须来自实际后端（⛔ 不是写死的字符串）：driver / gpu / present 三栏都要在。
    LIFT_CHECK(line.find("driver=") != std::string::npos);
    LIFT_CHECK(line.find("gpu=") != std::string::npos);
    LIFT_CHECK(line.find("present=") != std::string::npos);
    return 0;
}

int main(int argc, char** argv) {
    // ★ 启动期守卫（pure）：CMake 把 `$<TARGET_FILE:emulator-lift>` 传成 argv[2]。
    if (argc > 2 && std::string(argv[1]) == "--no-gpu-refuses-start") {
        caseNoGpuRefusesStart(std::filesystem::path(argv[2]));
        return lift_test::finish("lift/no-gpu-refuses-start");
    }
    if (argc > 2 && std::string(argv[1]) == "--raster-flag-gone") {
        caseRasterFlagGone(std::filesystem::path(argv[2]));
        return lift_test::finish("lift/raster-flag-gone");
    }
    const bool gpu = argc > 1 && std::string(argv[1]) == "--gpu";
    if (gpu) {
        // ★ CTest 给 `gpu-batch-barrier` 设了 `SDL_VIDEODRIVER=dummy`（无显示器的机器上别去建窗口），
        //   但 **dummy 视频下建不出 SDL_GPU 设备**（实测 `No supported SDL_GPU backend found`
        //   ⇒ 整条 `--gpu` 臂永远 SKIP 77、真后端判据一条都不跑）。本文件所有 `--gpu` 用例都用
        //   **隐藏窗口**（`SdlWindowOptions::hidden=true`）⇒ 这里把那个 env 让开，让真实视频驱动
        //   （macOS=cocoa / Windows=d3d11）建窗口与 GPU 设备。真没有显示系统 ⇒ `win.init` 失败 ⇒
        //   各自 SKIP 77（⛔ 不把"没跑"记成"跑绿了"）。
#if defined(_WIN32)
        _putenv_s("SDL_VIDEODRIVER", "");
#else
        ::unsetenv("SDL_VIDEODRIVER");
#endif
        int rc = caseGpuBarriers();
        if (rc != 0) return rc;  // SKIP 77 / 夹具失败
        rc = caseGpuContentChange();
        if (rc != 0) return rc;
        rc = caseGpuSubRectUpload();
        if (rc != 0) return rc;
        rc = caseGpuDepthSegments();
        if (rc != 0) return rc;
        rc = caseGpuUnalignedRowPitch();
        if (rc != 0) return rc;
        rc = caseGpuRenderToTexture();
        if (rc != 0) return rc;
        // ★ `REQ-01M79V2TVJH98BKH0C0WQV2FET`：sprite 的像素必须进绑定的 RT（后缓冲不变）。
        rc = caseGpuSpriteRenderTarget();
        if (rc != 0) return rc;
        // ★ RTT 复审"必须改 ②"：同尺寸换 target 时深度必须存活（旧行为每换必清 ⇒ 第一条断言红）。
        rc = caseGpuDepthRetainedAcrossTargetSwitch();
        if (rc != 0) return rc;
        // ★ `REQ-01M78BF1FNNH6K8N8GEY6GQYXX`：点/线在真后端上**真的画出来**（无 GPU ⇒ SKIP 77）。
        rc = caseGpuLinesPoints();
        if (rc != 0) return rc;
        // ★ headless 的 `present : window=hidden` 判据（子进程跑真 CLI；无 GPU ⇒ SKIP 77）。
        //   产品二进制 = 测试可执行旁边的 `../emulator-lift`（`out/tests/lift-gpu-batch` ⇒ `out/`）。
        std::filesystem::path emulator = std::filesystem::path(argv[0]).parent_path().parent_path() /
                                          "emulator-lift";
#if defined(_WIN32)
        emulator += ".exe";
#endif
        LIFT_CHECK(std::filesystem::exists(emulator));
        if (std::filesystem::exists(emulator)) {
            rc = caseHeadlessHiddenWindow(emulator);
            if (rc != 0) return rc;  // SKIP 77 / 判据红
        }
        return lift_test::finish("gpu-batch-barrier");
    }
    caseOrder();
    caseBackBufferLockBarrier();
    caseSamplerFilterEligibility();
    caseRenderTargetBackBufferIdentity();
    caseRenderToTexture();
    caseRenderToTextureFeedback();
    caseSpriteRenderTarget();
    caseFfTexProbe();
    caseDepthClearForwarded();
    caseLedgerQueuedVsRecorded();
    caseDirtyRect();
    // ★ `REQ-01M78BF1FNNH6K8N8GEY6GQYXX`：点/线不再有 CPU 旁路 + `[draw] fa=` 在 GPU 批上可见。
    caseLinesPointsAdmission();
    caseAlphaAggregation();
    caseGpuRejectAccounting();
    // ★ `REQ-01M7AAXVGY9SG75XCREHPX641Z`（M1）：`skipped` 账本（被拒批可见、放行批不计）。
    caseGpuSkipLedger();
    return lift_test::finish("gpu-batch-order");
}
