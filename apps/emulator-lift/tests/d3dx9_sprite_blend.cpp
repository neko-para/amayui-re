// emulator-lift · D3DXSprite 批次必须按**引擎实际设的混合状态**合成
//
// 缺陷：`REQ-01M7R1ABAP86QVMNBZ5S4EDKD5`（🐞S2）。`ID3DXSprite` 的合成路曾经只从 `Begin` 的 flags
// 取一个 bool（`gs_state.blend = blend;`），**因子 / op 一个都没赋** ⇒ 落到 `GpuRasterState` 的
// 缺省 `SrcAlpha / InvSrcAlpha`。引擎用**加性**（`SRCALPHA / ONE`）画那颗 sprite 时，那个 sprite
// 的（几乎全黑的）背景就被不透明地盖成黑块。
// ★ 措辞纪律（用户 2026-11-19 口径）：这件事**与影片没有关系**；同帧那条 dshow 影片只是相关读数
//   （120×120 强相关、未直读），⛔ 不得当成因果断言。
//
// 判据（红了意味着什么）：
//   ① 加性：`{src=SrcAlpha(5), dst=One(2), op=Add(1)}` + `ALPHABLEND` ⇒ `out.blend==true`、
//      `src_blend==SrcAlpha`、`dst_blend==One`。红的含义 = 引擎的加性被当成别的混合
//      （缺省 `InvSrcAlpha` 就是本单的黑块）。
//   ② `ALPHABLEND=0` ⇒ `blend==false`，而且**因子不被使用**（把因子设成表达不了的组合也不该失败）。
//   ③ separate alpha：开 ⇒ alpha 段取 `SRCBLENDALPHA` / `DESTBLENDALPHA` / `BLENDOPALPHA`；
//      关 ⇒ alpha 段**沿用 color 段**（D3D 语义）。
//   ④ 表达不了的因子（例如 `D3DBLEND_BOTHSRCALPHA=12`）⇒ 返回 false 且 `why` 非空
//      （⛔ 不许静默成功）。
//   ⑤ **接线守卫**（缺了它"忘了调"仍会静默）：驱动真实的 `ID3DXSprite::Begin/Draw/End`，用一个小
//      mock 后端在 `rasterQueue` 里截下**交进后端的那一份** `GpuRasterState`：设备状态是
//      `State=19⇒5` + `State=20⇒2` 时必须看到 `SrcAlpha / One`；把 `DESTBLEND` 改成 `InvSrcAlpha`
//      之后必须跟着变；`ALPHABLEND=0` 时必须 `blend=false`；表达不了的状态必须**不入队**
//      （`sprite_skipped` 涨、队列不涨 = 没有静默退回缺省因子）。
//
// 纯判定部分不建机器；接线部分只建裸 `Machine`（自造 guest 代码 + 假 GpuRaster，⛔ 不要 SDL / GPU 设备）
// ⇒ 顶层 `if(LIFT_BUILD_TESTS)` 注册 = pure 档（AGENTS.md §12.4：判据 = 注册位置）。
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/d3d9.h"
#include "runtime/d3d9_pipeline.h"
#include "runtime/d3dx9_sprite.h"
#include "runtime/gpu_raster.h"
#include "runtime/machine.h"
#include "shims/register.h"

namespace {

// =====================================================================================
// ①–④ 纯判定：`spriteBlendState` = `Begin` 的 flags + 设备状态快照 ⇒ `GpuRasterState`
// =====================================================================================
void casePureMapping() {
    using lift::GpuBlendFactor;
    using lift::GpuBlendOp;
    using lift::GpuRasterState;
    using lift::d3d9::BlendStateSnapshot;

    // ---- ① 加性：SRCALPHA / ONE + Add + ALPHABLEND ⇒ 因子必须原样落到 GpuRasterState ----
    lift_test::g_current_case = "d3dx9-sprite-blend/pure-additive";
    BlendStateSnapshot rs;
    rs.blend = true;  // 设备那一位；⛔ 不参与 spriteBlendState 的"开不开"判定（见头文件）
    rs.src_color = lift::d3d9::kBlendSrcAlpha;  // 5
    rs.dst_color = lift::d3d9::kBlendOne;       // 2
    rs.color_op = lift::d3d9::kBlendOpAdd;      // 1
    rs.separate_alpha = false;
    rs.src_alpha = lift::d3d9::kBlendOne;
    rs.dst_alpha = lift::d3d9::kBlendZero;
    rs.alpha_op = lift::d3d9::kBlendOpAdd;

    GpuRasterState out;
    std::string why;
    LIFT_CHECK(lift::d3dx9::spriteBlendState(rs, true, &out, &why));
    LIFT_CHECK(out.blend);
    LIFT_CHECK(out.src_blend == GpuBlendFactor::SrcAlpha);
    LIFT_CHECK(out.dst_blend == GpuBlendFactor::One);
    LIFT_CHECK(out.blend_op == GpuBlendOp::Add);
    // separate alpha 关 ⇒ alpha 段沿用 color 段（D3D 语义）
    LIFT_CHECK(out.src_blend_alpha == GpuBlendFactor::SrcAlpha);
    LIFT_CHECK(out.dst_blend_alpha == GpuBlendFactor::One);
    LIFT_CHECK(out.blend_op_alpha == GpuBlendOp::Add);
    LIFT_CHECK(why.empty());

    // ---- ② ALPHABLEND=0 ⇒ 关混合；因子**不被使用**（连表达不了的组合也不该失败）----
    lift_test::g_current_case = "d3dx9-sprite-blend/pure-alphablend-off";
    BlendStateSnapshot garbage = rs;
    garbage.src_color = lift::d3d9::kBlendBothSrcAlpha;      // 12：表达不了
    garbage.dst_color = lift::d3d9::kBlendBothInvSrcAlpha;    // 13：表达不了
    garbage.color_op = 0x77u;                                // 也不是合法 BLENDOP
    GpuRasterState no_blend;
    no_blend.blend = true;  // 先置真：证明输出真的被重置，而不是"碰巧是缺省"
    no_blend.separate_alpha_blend = true;
    why.clear();
    LIFT_CHECK(lift::d3dx9::spriteBlendState(garbage, false, &no_blend, &why));
    LIFT_CHECK(!no_blend.blend);
    LIFT_CHECK(!no_blend.separate_alpha_blend);
    LIFT_CHECK(why.empty());

    // ---- ③ separate alpha：开 ⇒ alpha 段独立；关 ⇒ 沿用 color 段 ----
    lift_test::g_current_case = "d3dx9-sprite-blend/pure-separate-alpha";
    BlendStateSnapshot sep = rs;
    sep.separate_alpha = true;
    sep.src_alpha = lift::d3d9::kBlendZero;   // 1
    sep.dst_alpha = lift::d3d9::kBlendOne;    // 2
    sep.alpha_op = lift::d3d9::kBlendOpAdd;   // 1
    GpuRasterState sep_out;
    why.clear();
    LIFT_CHECK(lift::d3dx9::spriteBlendState(sep, true, &sep_out, &why));
    LIFT_CHECK(sep_out.blend);
    LIFT_CHECK(sep_out.separate_alpha_blend);
    LIFT_CHECK(sep_out.src_blend == GpuBlendFactor::SrcAlpha);  // color 段不受影响
    LIFT_CHECK(sep_out.dst_blend == GpuBlendFactor::One);
    LIFT_CHECK(sep_out.src_blend_alpha == GpuBlendFactor::Zero);
    LIFT_CHECK(sep_out.dst_blend_alpha == GpuBlendFactor::One);
    LIFT_CHECK(sep_out.blend_op_alpha == GpuBlendOp::Add);
    // separate alpha 开 + alpha 段表达不了 ⇒ 也必须是 false + why（⛔ 只查 color 段就放行）
    sep.src_alpha = lift::d3d9::kBlendBothSrcAlpha;
    GpuRasterState sep_bad;
    why.clear();
    LIFT_CHECK(!lift::d3dx9::spriteBlendState(sep, true, &sep_bad, &why));
    LIFT_CHECK(!why.empty());

    // ---- ④ 表达不了的 color 因子 ⇒ false + why 非空（⛔ 不许静默成功）----
    lift_test::g_current_case = "d3dx9-sprite-blend/pure-unsupported-factor";
    BlendStateSnapshot bad = rs;
    bad.dst_color = lift::d3d9::kBlendBothSrcAlpha;  // 12 = D3DBLEND_BOTHSRCALPHA
    GpuRasterState bad_out;
    why.clear();
    LIFT_CHECK(!lift::d3dx9::spriteBlendState(bad, true, &bad_out, &why));
    LIFT_CHECK(!why.empty());
    // 表达不了的 BLENDOP 同样拒绝
    bad = rs;
    bad.color_op = 0x77u;
    why.clear();
    LIFT_CHECK(!lift::d3dx9::spriteBlendState(bad, true, &bad_out, &why));
    LIFT_CHECK(!why.empty());
}

// =====================================================================================
// ⑤ 接线守卫：真实的 `ID3DXSprite::Begin/Draw/End` + 假 GpuRaster（截下 rasterQueue 的状态）
// =====================================================================================
constexpr std::uint32_t kCodeBase = 0x00401000u;
constexpr std::uint32_t kCodeStride = 0x1000u;
constexpr std::uint32_t kDataBase = 0x00420000u;
constexpr std::uint32_t kDataStride = 0x1000u;
constexpr std::uint32_t kStubPage = 0x7FF80000u;
constexpr std::uint32_t kStubSlots = 0x1000u;
constexpr std::uint32_t kProcDirect3DCreate9 = 0x7FF81000u;
constexpr std::uint32_t kProcD3DXCreateSprite = 0x7FF81020u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
constexpr std::uint32_t kFixtureHeapBytes = 0x00100000u;

// 设备 vtable：`IDirect3DDevice9::SetRenderState` = +0xE4（与 tests/d3d9_pipeline.cpp 同一取证）。
constexpr std::uint32_t kOffDeviceSetRenderState = 0xE4u;

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

// 只记状态、不画像素、不碰 SDL 的假后端。判据 = **交给 rasterQueue 的那一份** GpuRasterState。
class CaptureRaster final : public lift::GpuRaster {
public:
    bool rasterActive() const override { return true; }
    const char* rasterName() const override { return "capture"; }

    bool rasterPrepare(const lift::SurfaceFormat&, int, int, std::string*, const std::uint8_t* base) override {
        ++prepare_calls;
        base_ = base;
        return true;
    }
    bool rasterSyncFromGuest(const std::uint8_t*, std::uint32_t, int, int, std::string*) override {
        ++sync_from_calls;
        return true;
    }
    bool rasterSyncToGuest(std::uint8_t*, std::uint32_t, int, int, std::string*) override { return true; }
    bool rasterDraw(const lift::GpuRasterVertex* v, std::size_t n, const lift::GpuRasterState& st,
                    std::string* err) override {
        return rasterQueue(v, n, nullptr, st, err);
    }
    bool rasterDrawTextured(const lift::GpuRasterVertex* v, std::size_t n, const lift::GpuRasterTexture& t,
                            const lift::GpuRasterState& st, std::string* err) override {
        return rasterQueue(v, n, &t, st, err);
    }
    bool rasterQueue(const lift::GpuRasterVertex*, std::size_t count, const lift::GpuRasterTexture*,
                     const lift::GpuRasterState& st, std::string*) override {
        states.push_back(st);
        queued_vertices += count;
        return true;
    }
    bool rasterFlush(std::string*) override { return true; }
    bool rasterDirty() const override { return false; }

    std::vector<lift::GpuRasterState> states;
    std::size_t queued_vertices = 0;
    int prepare_calls = 0;
    int sync_from_calls = 0;
    const std::uint8_t* base_ = nullptr;
};

struct Fixture {
    lift::Machine m;
    std::string err;
    std::uint32_t code_ea = kCodeBase;
    std::uint32_t data_ea = kDataBase;

    bool boot() {
        if (!m.initBare(&err)) return false;
        if (!m.prepareStack(&err)) return false;
        if (!m.heap().init(&m.mem(), m.layout().heap_base, kFixtureHeapBytes)) return false;
        m.shims().configureStubPage(kStubPage, kStubSlots);
        lift::registerD3D9Shims(m.shims());
        lift::registerD3DX9Shims(m.shims());
        if (!fixStub("d3d9", "Direct3DCreate9", kProcDirect3DCreate9)) return false;
        if (!fixStub("d3dx9_43", "D3DXCreateSprite", kProcD3DXCreateSprite)) return false;
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
        return r.reason == lift::StopReason::Stopped || r.reason == lift::StopReason::Unimplemented ||
                       r.reason == lift::StopReason::Trap || r.reason == lift::StopReason::ResourceExhausted
                   ? m.cpu().eax()
                   : 0;
    }

    std::uint32_t method(std::uint32_t obj, std::uint32_t offset, const std::vector<std::uint32_t>& args) {
        const std::uint32_t vtbl = m.mem().rd32(obj);
        Asm a;
        a.movEaxImm(vtbl);
        for (std::size_t i = args.size(); i > 0; --i) a.pushImm(args[i - 1]);
        a.pushImm(obj);
        if (offset <= 0x7Fu) {
            a.callMemEaxDisp8(static_cast<std::uint8_t>(offset));
        } else {
            a.callMemEaxDisp32(offset);
        }
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
        const std::uint32_t pp_ea = placeData(pp.data(), pp.size());
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
        return true;
    }

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

    std::uint32_t setRenderState(const Device& dev, std::uint32_t state, std::uint32_t value) {
        return method(dev.object, kOffDeviceSetRenderState, {state, value});
    }
};

void caseWiring() {
    lift_test::g_current_case = "d3dx9-sprite-blend/wiring";
    Fixture f;
    if (!f.boot()) {
        lift_test::failf(__FILE__, __LINE__, "夹具 boot 失败：%s", f.err.c_str());
        return;
    }
    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    Fixture::Device dev;
    if (!f.createDevice(d3d9, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev)) return;
    std::uint32_t sprite = 0;
    LIFT_CHECK_EQ_U32(f.createSprite(dev.object, &sprite), 0u);
    LIFT_CHECK_NE_U32(sprite, 0u);
    if (sprite == 0) return;

    const std::uint32_t rect_ea = f.placeRect(0, 0, 8, 8);
    const std::uint32_t pos_ea = f.placeVec3(0.0f, 0.0f, 0.0f);
    LIFT_CHECK_NE_U32(rect_ea, 0u);
    LIFT_CHECK_NE_U32(pos_ea, 0u);

    // 引擎现场（取证 `docs/03-emulator/save05-black-rect.md`）：加性 = SRCALPHA / ONE。
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsSrcBlend, lift::d3d9::kBlendSrcAlpha), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsDestBlend, lift::d3d9::kBlendOne), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsBlendOp, lift::d3d9::kBlendOpAdd), 0u);

    // 共享读状态入口：读的必须是**这台设备**刚设的那一份。
    {
        lift::d3d9::BlendStateSnapshot snap;
        std::string why;
        LIFT_CHECK(lift::d3d9::readBlendState(f.m.cpu(), dev.state, &snap, &why));
        LIFT_CHECK_EQ_U32(snap.src_color, lift::d3d9::kBlendSrcAlpha);
        LIFT_CHECK_EQ_U32(snap.dst_color, lift::d3d9::kBlendOne);
        LIFT_CHECK_EQ_U32(snap.color_op, lift::d3d9::kBlendOpAdd);
        LIFT_CHECK(!snap.separate_alpha);
    }

    CaptureRaster mock;
    f.m.setGpuRaster(&mock);

    // ---- ① 加性 + ALPHABLEND ⇒ 交进后端的那一份必须是 SrcAlpha / One ----
    auto beginDrawEnd = [&](std::uint32_t flags) -> std::uint32_t {
        const std::uint32_t hr = f.method(sprite, 0x20u, {flags});
        if (hr != 0u) return hr;
        const std::uint32_t dhr = f.method(sprite, 0x24u, {0u, rect_ea, 0u, pos_ea, 0xFFFFFFFFu});
        if (dhr != 0u) return dhr;
        return f.method(sprite, 0x2Cu, {});
    };
    LIFT_CHECK_EQ_U32(beginDrawEnd(lift::d3dx9::kSpriteAlphaBlend), 0u);
    LIFT_CHECK_EQ_U32(mock.states.size(), 1u);
    if (mock.states.size() == 1u) {
        const lift::GpuRasterState& got = mock.states.back();
        LIFT_CHECK(got.blend);
        LIFT_CHECK(got.src_blend == lift::GpuBlendFactor::SrcAlpha);
        LIFT_CHECK(got.dst_blend == lift::GpuBlendFactor::One);  // ← 缺省 InvSrcAlpha 会让这条红
        LIFT_CHECK(got.blend_op == lift::GpuBlendOp::Add);
        // sprite 自己的那三项仍然由本模块覆盖（深度关、viewport/scissor = 目标尺寸）。
        LIFT_CHECK(!got.depth_test);
        LIFT_CHECK(!got.depth_write);
    }

    // ---- ② 设备状态改成 SrcAlpha / InvSrcAlpha ⇒ 交进后端的那一份必须跟着变 ----
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsDestBlend, lift::d3d9::kBlendInvSrcAlpha), 0u);
    LIFT_CHECK_EQ_U32(beginDrawEnd(lift::d3dx9::kSpriteAlphaBlend), 0u);
    LIFT_CHECK_EQ_U32(mock.states.size(), 2u);
    if (mock.states.size() == 2u) {
        const lift::GpuRasterState& got = mock.states.back();
        LIFT_CHECK(got.blend);
        LIFT_CHECK(got.src_blend == lift::GpuBlendFactor::SrcAlpha);
        LIFT_CHECK(got.dst_blend == lift::GpuBlendFactor::InvSrcAlpha);
    }

    // ---- ③ ALPHABLEND=0 ⇒ blend=false（因子不被使用，也不许还开着混合）----
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsDestBlend, lift::d3d9::kBlendOne), 0u);
    LIFT_CHECK_EQ_U32(beginDrawEnd(0u), 0u);
    LIFT_CHECK_EQ_U32(mock.states.size(), 3u);
    if (mock.states.size() == 3u) {
        const lift::GpuRasterState& got = mock.states.back();
        LIFT_CHECK(!got.blend);
        LIFT_CHECK(!got.separate_alpha_blend);
    }

    // ---- ④ 表达不了的因子 ⇒ 这一条**不入队** + 账本记 skipped（⛔ 不静默退回缺省因子）----
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsDestBlend, lift::d3d9::kBlendBothSrcAlpha), 0u);
    const std::uint64_t skipped_before = lift::d3d9::gpuCoverage().sprite_skipped;
    const std::size_t states_before = mock.states.size();
    LIFT_CHECK_EQ_U32(beginDrawEnd(lift::d3dx9::kSpriteAlphaBlend), 0u);
    LIFT_CHECK_EQ_U32(mock.states.size(), states_before);  // 没交给后端
    LIFT_CHECK(lift::d3d9::gpuCoverage().sprite_skipped > skipped_before);
}

}  // namespace

int main() {
    using namespace lift_test;
    try {
        casePureMapping();
        caseWiring();
        return finish("d3dx9-sprite-blend");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "DIAG uncaught std::exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "DIAG uncaught non-std exception\n");
        return 1;
    }
}
