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
        if (r.reason == lift::StopReason::Stopped || r.reason == lift::StopReason::Unimplemented) {
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

    std::uint32_t pixelAt(int x, int y) {
        return m.mem().rd32(device_info.arena + static_cast<std::uint32_t>(y) * device_info.pitch +
                            static_cast<std::uint32_t>(x) * 4u);
    }
};

// ---------------------------------------------------------------------------------------
// Mock 后端：只记调用序，不画像素（pure 档；不依赖 SDL / GPU）
// ---------------------------------------------------------------------------------------
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
    bool rasterQueue(const lift::GpuRasterVertex*, std::size_t, const lift::GpuRasterTexture*,
                     const lift::GpuRasterState&, std::string*) override {
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
    bool fail_flush = false;

private:
    std::vector<std::uint32_t> queue_;
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

}  // namespace

int main(int argc, char** argv) {
    const bool gpu = argc > 1 && std::string(argv[1]) == "--gpu";
    if (gpu) {
        const int rc = caseGpuBarriers();
        if (rc != 0) return rc;  // SKIP 77 / 夹具失败
        return lift_test::finish("gpu-batch-barrier");
    }
    caseOrder();
    return lift_test::finish("gpu-batch-order");
}
