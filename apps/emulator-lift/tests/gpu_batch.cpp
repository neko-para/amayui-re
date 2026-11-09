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
constexpr std::uint32_t kOffDrawPrimitiveUP = 0x14Cu;
constexpr std::uint32_t kOffSetFVF = 0x164u;
constexpr std::uint32_t kOffTextureGetSurfaceLevel = 0x48u;
constexpr std::uint32_t kOffTextureLockRect = 0x4Cu;
constexpr std::uint32_t kOffTextureUnlockRect = 0x50u;

constexpr std::uint32_t kFvfXyzRhwDiffuse = 0x0044u;  // D3DFVF_XYZRHW | D3DFVF_DIFFUSE

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

    std::uint32_t clearTo(std::uint32_t color) {
        return method(device_info.object, kOffClear, {0u, 0u, 3u, color, 0x3F800000u, 0u});
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
            queued_.push_back(Queued{key, sample});
        } else {
            queued_.push_back(Queued{0, lift::GpuTexelRect{}});
        }
        queue_.push_back(1u);
        return true;
    }
    bool rasterFlush(std::string*) override {
        if (fail_flush) return false;
        ++flush_count;
        if (!queue_.empty()) {
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
        queue_.clear();
        dirty_ = false;
        base_ = nullptr;
    }

    std::size_t flush_count = 0;
    std::size_t sync_to_count = 0;
    std::size_t sync_from_count = 0;
    std::size_t cut_count = 0;      // 方案 C：内容变更边界切段次数
    std::size_t upload_count = 0;   // 方案 C：纹理上传块数
    std::size_t queued_draws = 0;   // 累计入队 draw 条数
    bool fail_flush = false;

private:
    struct Queued {
        std::uint64_t key = 0;
        lift::GpuTexelRect sample;
    };
    std::vector<std::uint32_t> queue_;
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
// --gpu：**子矩形上传 + 脏区不相交 ⇒ 不切段**（真后端像素 oracle）
// ---------------------------------------------------------------------------------------
// ★ 判据会红的情形：
//   · 脏区判据恒真（"版本一变就切"）⇒ cuts 变 1；
//   · 子矩形上传的 dst 偏移/行距（256 字节行对齐）写错 ⇒ 矩形**外**的像素被改（trace 里 A/C 变红）；
//   · 只传脏区却把整幅当最新（或反之）⇒ B 不是红 / A、C 不是蓝。
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
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(s1.cuts - s0.cuts), 0);       // ★ 不相交 ⇒ 0 次切段
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(s1.uploads - s0.uploads), 2);  // 整幅 + 一块脏矩形
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(s1.submits - s0.submits), 2);
    LIFT_CHECK_EQ_U32(f.pixelAt(0, 0), 0x000000FFu);    // A：蓝（未被动过）
    LIFT_CHECK_EQ_U32(f.pixelAt(32, 32), 0x00FF0000u);  // B：红（子矩形上传真的落对了位置）
    LIFT_CHECK_EQ_U32(f.pixelAt(48, 48), 0x000000FFu);  // C：蓝（矩形外像素不变）

    gpu.reset();
    SDL_QuitSubSystem(SDL_INIT_VIDEO);
    LIFT_CHECK_EQ_U32(SDL_WasInit(SDL_INIT_VIDEO), 0u);
    std::printf("gpu-batch-subrect-upload: cuts=%llu uploads=%llu\n",
                static_cast<unsigned long long>(s1.cuts - s0.cuts),
                static_cast<unsigned long long>(s1.uploads - s0.uploads));
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
    caseDirtyRect();
    return lift_test::finish("gpu-batch-order");
}
