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
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/d3d9.h"
#include "runtime/d3d9_pipeline.h"
#include "runtime/gpu_raster.h"
#include "runtime/machine.h"
#include "runtime/present_sdl.h"
#include "runtime/surface.h"
#include "shims/register.h"

#include <SDL3/SDL.h>

namespace {

constexpr std::uint32_t kCodeBase = 0x00401000u;
constexpr std::uint32_t kCodeStride = 0x1000u;
constexpr std::uint32_t kDataBase = 0x00420000u;
constexpr std::uint32_t kDataStride = 0x1000u;
constexpr std::uint32_t kStubPage = 0x7FF80000u;
constexpr std::uint32_t kStubSlots = 0x1000u;
constexpr std::uint32_t kProcDirect3DCreate9 = 0x7FF81000u;
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

    // D3DXCreateTexture(device, w, h, 1, 0, format, POOL_DEFAULT=0, &out)
    std::uint32_t createTexture(std::uint32_t device, std::uint32_t w, std::uint32_t h, std::uint32_t format,
                                std::uint32_t* out_obj) {
        const std::uint32_t out_ea = placeU32(0xDEADBEEFu);
        Asm a;
        a.pushImm(out_ea);
        a.pushImm(0u);
        a.pushImm(format);
        a.pushImm(0u);
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

    std::uint32_t clearTo(std::uint32_t color) {
        return method(device_info.object, kOffClear, {0u, 0u, 3u, color, 0x3F800000u, 0u});
    }

    // 只清颜色（D3DCLEAR_TARGET，**不带** ZBUFFER）——用来证明"清深度请求不是无条件的"。
    std::uint32_t clearTargetOnly(std::uint32_t color) {
        return method(device_info.object, kOffClear, {0u, 0u, 1u, color, 0x3F800000u, 0u});
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

    std::uint32_t setRenderTarget(std::uint32_t surface) {
        return method(device_info.object, kOffSetRenderTarget, {surface, 0u});
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
    // ★ 可配：让"壳级 A/B && 后端能力"里的后端那一项**可观测**（默认 true 会让断言恒真）。
    bool rasterSpritesEnabled() const override { return sprites_enabled; }

    std::size_t flush_count = 0;
    std::size_t sync_to_count = 0;
    std::size_t sync_from_count = 0;
    std::size_t prepare_count = 0;
    std::size_t discard_count = 0;
    std::size_t depth_clear_count = 0;
    bool sprites_enabled = true;
    std::size_t cut_count = 0;      // 方案 C：内容变更边界切段次数
    std::size_t upload_count = 0;   // 方案 C：纹理上传块数
    std::size_t queued_draws = 0;   // 累计入队 draw 条数
    bool fail_flush = false;
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
//   分开用 ⇒ 两边都"成功"、整批像素却不同，且 `--gpu-strict` **看不见**（它不是 fallback）。
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
    // ★ 让"壳级 A/B && 后端能力"里的**后端那一项**可观测：`rasterSpritesEnabled` 不是纯转发，
    //   只有把后端置 false 才能证明"后端那一项真的被问了"（否则默认 true 会让断言恒真）。
    mock.sprites_enabled = false;
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
    LIFT_CHECK(!window.rasterSpritesEnabled());  // 后端 false ⇒ 壳的合成必须反映它（⛔ 不是恒真）
    window.rasterDiscardTarget();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(mock.discard_count), 1);
    LIFT_CHECK(window.rasterTargetBase() == nullptr);  // 后端的目标身份已被丢掉

    f.m.setGpuRaster(nullptr);
    window.setGpuRasterForTest(nullptr);
    std::printf("gpu-batch/depth-clear-forwarded: ZBUFFER=%llu 次转发、TARGET-only 未转发\n",
                static_cast<unsigned long long>(mock.depth_clear_count));
}

int main(int argc, char** argv) {
    const bool gpu = argc > 1 && std::string(argv[1]) == "--gpu";
    if (gpu) {
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
        return lift_test::finish("gpu-batch-barrier");
    }
    caseOrder();
    caseBackBufferLockBarrier();
    caseSamplerFilterEligibility();
    caseFfTexProbe();
    caseDepthClearForwarded();
    caseLedgerQueuedVsRecorded();
    caseDirtyRect();
    return lift_test::finish("gpu-batch-order");
}
