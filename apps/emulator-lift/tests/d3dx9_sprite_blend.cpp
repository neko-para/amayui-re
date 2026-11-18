// emulator-lift · D3DXSprite 的混合：因子来自设备状态，且 Begin(ALPHABLEND) 像真 D3DX 那样设/恢复
//
// 两条缺陷单叠在同一条路上（⛔ 不是回退关系）：
//   · `REQ-01M7R1ABAP86QVMNBZ5S4EDKD5`（BUG-33）：`ID3DXSprite` 的合成曾经只从 `Begin` 的 flags
//     取一个 bool、**因子 / op 一个都没赋** ⇒ 落到 `GpuRasterState` 的缺省 `SrcAlpha / InvSrcAlpha`。
//     引擎用**加性**（`SRCALPHA / ONE`）画那颗 sprite 时，它的（几乎全黑的）背景就被不透明地盖成
//     黑块（SAVE05 的 120×120 黑框）。修法 = 因子**从设备状态读**。
//   · `REQ-01M7R3YPQPNPBZ327H89Q8RTEE`（本单）：真 D3DX9 的 `Begin(ALPHABLEND)` **自己**把
//     `ALPHABLENDENABLE=1` / `SRCBLEND=SRCALPHA(5)` / `DESTBLEND=INVSRCALPHA(6)` 设好，并在 `End`
//     把它改过的三项恢复。漏了这一步 ⇒ 普通批次读到的是引擎留在设备里的 `ONE(2)/ZERO(1)`
//     ⇒ 不透明拷贝 ⇒ 源图里 alpha=0 的黑色 texel 写进后缓冲（走廊石地 / 对话框底板全黑）。
// ★ 措辞纪律（用户 2026-11-19 口径）：这件事**与影片没有关系**；同帧那条 dshow 影片只是相关读数
//   （120×120 强相关、未直读），⛔ 不得当成因果断言。
//
// 判据（红了意味着什么）：
//   ① **纯映射**（BUG-33 的判定本身）：加性 `{src=SrcAlpha(5), dst=One(2), op=Add(1)}` + `ALPHABLEND`
//      ⇒ `out.blend==true`、`src_blend==SrcAlpha`、`dst_blend==One`；`ALPHABLEND=0` ⇒ `blend==false`
//      且因子不被使用；separate alpha 两态；表达不了的因子 ⇒ false + why（⛔ 不许静默成功）。
//   ② **接管与恢复**（本单的核心）：`Begin(ALPHABLEND)` 之后设备状态块里 `27==1`、`19==5`、`20==6`；
//      `End` 之后**逐字**回到 Begin 之前那一对（夹具给非 5/6 的 19=3 / 20=9，27=0），第二次
//      Begin/End 复验可重复。红 = 没设 5/6（本轮回归的形态）或 End 把引擎的状态吞了 / 恢复错值。
//   ③ **像素回归**：16×16 源图（alpha=0 的黑 texel 铺满，中心一个 alpha=255 红点）走真实的
//      `Begin/Draw/End`，交进后端的混合必须是 5/6；再按 D3D9 的混合方程把它作用到**纯色后缓冲**上
//      ⇒ 背景 255 个 texel 一个都不许变成黑、只有中心红点被写下。红 = 透明黑 texel 把目标写成黑
//      （这正是本轮回归的可见后果）。附负例：同样的判据作用到修前的 `ONE/ZERO` 上会把透明黑写成黑
//      ⇒ 证明这条判据不是恒真。
//   ④ **接线**（BUG-33 仍然成立 + 引擎覆盖的边界）：引擎在 `Begin` **之后**、`End` 之前写
//      `19=5 / 20=2` ⇒ `End` 读到 5/2（加性）；`ALPHABLEND=0` ⇒ `blend=false`；表达不了的因子
//      （`Begin` 之后写 `20=12`）⇒ 这一条**不入队** + `sprite_skipped` 涨（⛔ 不静默退回缺省因子）。
//      红 = 加性被当成别的混合 / 合成不再读状态 / 静默退回缺省。
//
// 纯判定部分不建机器；接线部分只建裸 `Machine`（自造 guest 代码 + 假 GpuRaster，⛔ 不要 SDL / GPU 设备）
// ⇒ 顶层 `if(LIFT_BUILD_TESTS)` 注册 = pure 档（AGENTS.md §12.4：判据 = 注册位置）。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
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
// ① 纯判定：`spriteBlendState` = `Begin` 的 flags + 设备状态快照 ⇒ `GpuRasterState`
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
    garbage.dst_color = lift::d3d9::kBlendBothInvSrcAlpha;   // 13：表达不了
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
// 夹具：裸 Machine + 自造 guest 代码（与既有 d3dx9-sprite-blend 接线用例同一形状）
// =====================================================================================
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

// 设备 vtable：`IDirect3DDevice9::SetRenderState` = +0xE4（与 tests/d3d9_pipeline.cpp 同一取证）。
constexpr std::uint32_t kOffDeviceSetRenderState = 0xE4u;
// `IDirect3DTexture9::LockRect` = +0x4C / `UnlockRect` = +0x50（与 tests/gpu_batch.cpp 同一取证）。
constexpr std::uint32_t kOffTextureLockRect = 0x4Cu;
constexpr std::uint32_t kOffTextureUnlockRect = 0x50u;
// `ID3DXSprite` 的槽（文件头 vtable 表）：Begin +0x20 / Draw +0x24 / End +0x2C。
constexpr std::uint32_t kOffSpriteBegin = 0x20u;
constexpr std::uint32_t kOffSpriteDraw = 0x24u;
constexpr std::uint32_t kOffSpriteEnd = 0x2Cu;

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

// 只记状态 / 顶点 / 纹理，不画像素、不碰 SDL 的假后端。
// 判据 = **交给 rasterQueue 的那一份** GpuRasterState（+ 像素回归用的顶点与纹理）。
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
    bool rasterQueue(const lift::GpuRasterVertex* v, std::size_t count, const lift::GpuRasterTexture* t,
                     const lift::GpuRasterState& st, std::string*) override {
        states.push_back(st);
        vertices.emplace_back(v, v + count);
        textures.push_back(t != nullptr ? *t : lift::GpuRasterTexture{});
        queued_vertices += count;
        return true;
    }
    bool rasterFlush(std::string*) override { return true; }
    bool rasterDirty() const override { return false; }

    std::vector<lift::GpuRasterState> states;
    std::vector<std::vector<lift::GpuRasterVertex>> vertices;
    std::vector<lift::GpuRasterTexture> textures;
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
        if (!fixStub("d3dx9_43", "D3DXCreateTexture", kProcD3DXCreateTexture)) return false;
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

    // D3DXCreateTexture(device, w, h, 1, Usage=0, format, POOL_DEFAULT=0, &out)
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

    std::uint32_t textureLockRect(std::uint32_t texture, std::uint32_t* locked_rect_ea) {
        const std::uint32_t lr = placeData(std::vector<std::uint8_t>(8u, 0).data(), 8u);
        const std::uint32_t hr = method(texture, kOffTextureLockRect, {0u, lr, 0u, 0u});
        if (locked_rect_ea) *locked_rect_ea = lr;
        return hr;
    }
    std::uint32_t textureUnlockRect(std::uint32_t texture) {
        return method(texture, kOffTextureUnlockRect, {0u});  // Level=0
    }

    std::uint32_t setRenderState(const Device& dev, std::uint32_t state, std::uint32_t value) {
        return method(dev.object, kOffDeviceSetRenderState, {state, value});
    }
    std::uint32_t spriteBegin(std::uint32_t sprite, std::uint32_t flags) {
        return method(sprite, kOffSpriteBegin, {flags});
    }
    std::uint32_t spriteDraw(std::uint32_t sprite, std::uint32_t texture, std::uint32_t rect_ea,
                             std::uint32_t pos_ea, std::uint32_t color) {
        return method(sprite, kOffSpriteDraw, {texture, rect_ea, 0u, pos_ea, color});
    }
    std::uint32_t spriteEnd(std::uint32_t sprite) { return method(sprite, kOffSpriteEnd, {}); }
};

// 读**这台设备**当前生效的 27/19/20（与 sprite 合成路同一个读法）。
bool readBlend(Fixture& f, const Fixture::Device& dev, lift::d3d9::BlendStateSnapshot* out) {
    std::string why;
    if (!lift::d3d9::readBlendState(f.m.cpu(), dev.state, out, &why)) {
        lift_test::failf(__FILE__, __LINE__, "readBlendState 失败：%s", why.c_str());
        return false;
    }
    return true;
}

// =====================================================================================
// ② 接管与恢复：`Begin(ALPHABLEND)` 设 1/5/6，`End` 逐字恢复 Begin 之前那一对
// =====================================================================================
void caseBeginRestoresBlendState() {
    lift_test::g_current_case = "d3dx9-sprite-blend/begin-restores-blend-state";
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

    // 夹具给一对**非** 5/6 的初值（引擎在 Begin 之前的残留就是这个形状：不是 D3DX 的 5/6）。
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsSrcBlend, lift::d3d9::kBlendSrcColor), 0u);   // 3
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsDestBlend, lift::d3d9::kBlendDestColor), 0u);  // 9

    // 第一次：Begin 之后 1/5/6；End 之后逐字回 0/3/9。
    LIFT_CHECK_EQ_U32(f.spriteBegin(sprite, lift::d3dx9::kSpriteAlphaBlend), 0u);
    {
        lift::d3d9::BlendStateSnapshot after_begin;
        if (readBlend(f, dev, &after_begin)) {
            LIFT_CHECK(after_begin.blend);  // 27 == 1
            LIFT_CHECK_EQ_U32(after_begin.src_color, lift::d3d9::kBlendSrcAlpha);    // 19 == 5
            LIFT_CHECK_EQ_U32(after_begin.dst_color, lift::d3d9::kBlendInvSrcAlpha);  // 20 == 6
        }
    }
    LIFT_CHECK_EQ_U32(f.spriteEnd(sprite), 0u);
    {
        lift::d3d9::BlendStateSnapshot after_end;
        if (readBlend(f, dev, &after_end)) {
            LIFT_CHECK(!after_end.blend);  // 27 == 0（Begin 之前那位）
            LIFT_CHECK_EQ_U32(after_end.src_color, lift::d3d9::kBlendSrcColor);    // 逐字回 3
            LIFT_CHECK_EQ_U32(after_end.dst_color, lift::d3d9::kBlendDestColor);   // 逐字回 9
        }
    }

    // 第二次：同一对初值 ⇒ 同样的接管/恢复（可重复，⛔ 不是"只有第一次碰巧"）。
    LIFT_CHECK_EQ_U32(f.spriteBegin(sprite, lift::d3dx9::kSpriteAlphaBlend), 0u);
    {
        lift::d3d9::BlendStateSnapshot after_begin2;
        if (readBlend(f, dev, &after_begin2)) {
            LIFT_CHECK(after_begin2.blend);
            LIFT_CHECK_EQ_U32(after_begin2.src_color, lift::d3d9::kBlendSrcAlpha);
            LIFT_CHECK_EQ_U32(after_begin2.dst_color, lift::d3d9::kBlendInvSrcAlpha);
        }
    }
    LIFT_CHECK_EQ_U32(f.spriteEnd(sprite), 0u);
    {
        lift::d3d9::BlendStateSnapshot after_end2;
        if (readBlend(f, dev, &after_end2)) {
            LIFT_CHECK(!after_end2.blend);
            LIFT_CHECK_EQ_U32(after_end2.src_color, lift::d3d9::kBlendSrcColor);
            LIFT_CHECK_EQ_U32(after_end2.dst_color, lift::d3d9::kBlendDestColor);
        }
    }
}

// =====================================================================================
// ③ 像素回归：透明黑 texel **不许**把纯色后缓冲写成黑
//
// ★ 为什么 mock 不画像素也能判：mock 截下的是**真的交给后端的那一份** `GpuRasterState` 与
//   源纹理像素。D3D9 的混合方程（`C = Csrc·Fsrc + Cdst·Fdst`）就是"后端会写什么"的判据 ——
//   这里把它作用到纯色后缓冲上，逐 texel 断言。红 = 透明 texel 被不透明地写成黑（本轮回归）。
//   ★ 附负例：同样的判据换回修前的 `ONE / ZERO` 会把透明黑写成黑 ⇒ 判据不是恒真。
// =====================================================================================
struct Rgba {
    double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
};

bool blendFactorValue(lift::GpuBlendFactor f, const Rgba& src, double* out) {
    switch (f) {
        case lift::GpuBlendFactor::One: *out = 1.0; return true;
        case lift::GpuBlendFactor::Zero: *out = 0.0; return true;
        case lift::GpuBlendFactor::SrcAlpha: *out = src.a / 255.0; return true;
        case lift::GpuBlendFactor::InvSrcAlpha: *out = 1.0 - src.a / 255.0; return true;
        default: return false;
    }
}

double clamp255(double v) { return v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v); }

// D3D9 的混合方程（本线在用的 4 个因子；其它因子 ⇒ false，⛔ 不猜）。
bool blendOver(const Rgba& dst, const Rgba& src, const lift::GpuRasterState& st, Rgba* out) {
    if (!st.blend) return false;
    if (st.blend_op != lift::GpuBlendOp::Add) return false;
    double fs = 0.0, fd = 0.0, fas = 0.0, fad = 0.0;
    if (!blendFactorValue(st.src_blend, src, &fs)) return false;
    if (!blendFactorValue(st.dst_blend, src, &fd)) return false;
    if (st.separate_alpha_blend) {
        if (!blendFactorValue(st.src_blend_alpha, src, &fas)) return false;
        if (!blendFactorValue(st.dst_blend_alpha, src, &fad)) return false;
    } else {
        fas = fs;
        fad = fd;
    }
    out->r = clamp255(src.r * fs + dst.r * fd);
    out->g = clamp255(src.g * fs + dst.g * fd);
    out->b = clamp255(src.b * fs + dst.b * fd);
    out->a = clamp255(src.a * fas + dst.a * fad);
    return true;
}

bool nearlyEqual(double a, double b) { return (a > b ? a - b : b - a) < 1e-6; }

void casePixelTransparentTexel() {
    lift_test::g_current_case = "d3dx9-sprite-blend/pixel-transparent-texel";
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

    // ---- 16×16 源图：alpha=0 的黑 texel 铺满，中心 (8,8) 一个 alpha=255 的红点 ----
    constexpr std::uint32_t kTexW = 16u;
    constexpr std::uint32_t kTexH = 16u;
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(f.createTexture(dev.object, kTexW, kTexH, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    {
        std::uint32_t locked = 0;
        LIFT_CHECK_EQ_U32(f.textureLockRect(tex, &locked), 0u);
        const std::uint32_t pitch = f.m.mem().rd32(locked);
        const std::uint32_t bits = f.m.mem().rd32(locked + 4u);
        LIFT_CHECK_NE_U32(bits, 0u);
        LIFT_CHECK_NE_U32(pitch, 0u);
        for (std::uint32_t y = 0; y < kTexH; ++y) {
            for (std::uint32_t x = 0; x < kTexW; ++x) {
                // A8R8G8B8（内存 BGRA，小端 0xAARRGGBB）：透明黑 = 0；中心红点 = 0xFFFF0000。
                const std::uint32_t v = (x == 8u && y == 8u) ? 0xFFFF0000u : 0x00000000u;
                f.m.mem().wr32(bits + y * pitch + x * 4u, v);
            }
        }
        LIFT_CHECK_EQ_U32(f.textureUnlockRect(tex), 0u);
    }

    // 引擎在 Begin 之前留在设备里的就是 ONE/ZERO（e2e 普查：383/383 批次 = 2/1）。
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsSrcBlend, lift::d3d9::kBlendOne), 0u);   // 2
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsDestBlend, lift::d3d9::kBlendZero), 0u);  // 1

    CaptureRaster mock;
    f.m.setGpuRaster(&mock);

    const std::uint32_t rect_ea = f.placeRect(0, 0, static_cast<std::int32_t>(kTexW),
                                              static_cast<std::int32_t>(kTexH));
    const std::uint32_t pos_ea = f.placeVec3(0.0f, 0.0f, 0.0f);
    LIFT_CHECK_EQ_U32(f.spriteBegin(sprite, lift::d3dx9::kSpriteAlphaBlend), 0u);
    LIFT_CHECK_EQ_U32(f.spriteDraw(sprite, tex, rect_ea, pos_ea, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(f.spriteEnd(sprite), 0u);

    LIFT_CHECK_EQ_U32(mock.states.size(), 1u);
    LIFT_CHECK_EQ_U32(mock.textures.size(), 1u);
    if (mock.states.size() != 1u || mock.textures.size() != 1u) return;
    const lift::GpuRasterState& st = mock.states[0];
    const lift::GpuRasterTexture& gt = mock.textures[0];
    // 交进后端的混合必须是 D3DX 的默认因子（不是引擎残留的 2/1）。
    LIFT_CHECK(st.blend);
    LIFT_CHECK(st.src_blend == lift::GpuBlendFactor::SrcAlpha);
    LIFT_CHECK(st.dst_blend == lift::GpuBlendFactor::InvSrcAlpha);
    LIFT_CHECK(!st.separate_alpha_blend);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(gt.width), kTexW);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(gt.height), kTexH);
    LIFT_CHECK(gt.pixels != nullptr);
    if (gt.pixels == nullptr) return;

    // 四边形正好覆盖 (0,0)–(16,16)、uv 0..1 ⇒ 每个 texel 落到同坐标的目标像素（1:1）。
    {
        const std::vector<lift::GpuRasterVertex>& v = mock.vertices[0];
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(v.size()), 6u);
        LIFT_CHECK(nearlyEqual(v[0].x, 0.0) && nearlyEqual(v[0].y, 0.0));
        LIFT_CHECK(nearlyEqual(v[1].x, 16.0) && nearlyEqual(v[1].y, 0.0));
        LIFT_CHECK(nearlyEqual(v[2].x, 0.0) && nearlyEqual(v[2].y, 16.0));
        LIFT_CHECK(nearlyEqual(v[0].u, 0.0f) && nearlyEqual(v[0].v, 0.0f));
        LIFT_CHECK(nearlyEqual(v[4].u, 1.0f) && nearlyEqual(v[4].v, 1.0f));
    }

    // 目标 = 纯色后缓冲（mock 不画像素；这里按 D3D9 的混合方程逐 texel 算"后端会写什么"）。
    const Rgba background{40.0, 80.0, 120.0, 255.0};
    std::uint32_t unchanged = 0, blackened = 0, red_written = 0;
    for (std::uint32_t y = 0; y < kTexH; ++y) {
        for (std::uint32_t x = 0; x < kTexW; ++x) {
            const std::uint8_t* p = gt.pixels + static_cast<std::size_t>(y) * gt.pitch + x * 4u;
            const Rgba src{static_cast<double>(p[2]), static_cast<double>(p[1]),
                           static_cast<double>(p[0]), static_cast<double>(p[3])};  // BGRA → RGBA
            Rgba out;
            LIFT_CHECK(blendOver(background, src, st, &out));
            const bool center = (x == 8u && y == 8u);
            if (center) {
                // alpha=255 的红点：SRC*1 + DST*0 ⇒ 目标写成红（只有这一格变）。
                if (out.r > 254.0 && out.g < 1.0 && out.b < 1.0) ++red_written;
            } else {
                // alpha=0 的黑 texel：SRC*0 + DST*1 ⇒ 目标**逐字不变**（⛔ 不许变成黑）。
                if (nearlyEqual(out.r, background.r) && nearlyEqual(out.g, background.g) &&
                    nearlyEqual(out.b, background.b)) {
                    ++unchanged;
                }
                if (out.r < 1.0 && out.g < 1.0 && out.b < 1.0) ++blackened;
            }
        }
    }
    LIFT_CHECK_EQ_U32(blackened, 0u);     // ← 本轮的可见回归就是这一条变红（透明黑 texel 写成黑）
    LIFT_CHECK_EQ_U32(unchanged, kTexW * kTexH - 1u);
    LIFT_CHECK_EQ_U32(red_written, 1u);

    // ---- 负例守护：判据换回修前的 ONE/ZERO 时，同一个透明黑 texel 会把背景写成黑 ----
    //   ⇒ 上面那条"一个都不许变黑"不是恒真（它真的能区分 5/6 与 2/1）。
    {
        lift::GpuRasterState pre_fix;
        pre_fix.blend = true;
        pre_fix.src_blend = lift::GpuBlendFactor::One;
        pre_fix.dst_blend = lift::GpuBlendFactor::Zero;
        pre_fix.blend_op = lift::GpuBlendOp::Add;
        pre_fix.separate_alpha_blend = false;
        Rgba out;
        const Rgba transparent_black{0.0, 0.0, 0.0, 0.0};
        LIFT_CHECK(blendOver(background, transparent_black, pre_fix, &out));
        LIFT_CHECK(out.r < 1.0 && out.g < 1.0 && out.b < 1.0);
    }

    // End 已经把 Begin 之前那对 2/1 恢复（像素判据的现场也顺带钉住恢复）。
    {
        lift::d3d9::BlendStateSnapshot after_end;
        if (readBlend(f, dev, &after_end)) {
            LIFT_CHECK(!after_end.blend);
            LIFT_CHECK_EQ_U32(after_end.src_color, lift::d3d9::kBlendOne);
            LIFT_CHECK_EQ_U32(after_end.dst_color, lift::d3d9::kBlendZero);
        }
    }
}

// =====================================================================================
// ④ 接线：真实 `Begin/Draw/End` + 假 GpuRaster（BUG-33 仍成立 + 引擎覆盖的边界）
// =====================================================================================
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

    // 引擎在 Begin 之前留在设备里的状态（e2e 普查的 2/1）。
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsSrcBlend, lift::d3d9::kBlendOne), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsDestBlend, lift::d3d9::kBlendZero), 0u);

    CaptureRaster mock;
    f.m.setGpuRaster(&mock);

    // ---- ① 加性批次：引擎在 `Begin` **之后**写 19=5 / 20=2 ⇒ End 读到的必须是 5/2 ----
    //   ★ 这正是 BUG-33 的判据：如果合成路不再读设备状态（退回写死 5/6），这一条当场红。
    LIFT_CHECK_EQ_U32(f.spriteBegin(sprite, lift::d3dx9::kSpriteAlphaBlend), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsSrcBlend, lift::d3d9::kBlendSrcAlpha), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsDestBlend, lift::d3d9::kBlendOne), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsBlendOp, lift::d3d9::kBlendOpAdd), 0u);
    LIFT_CHECK_EQ_U32(f.spriteDraw(sprite, 0u, rect_ea, pos_ea, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(f.spriteEnd(sprite), 0u);
    LIFT_CHECK_EQ_U32(mock.states.size(), 1u);
    if (mock.states.size() == 1u) {
        const lift::GpuRasterState& got = mock.states[0];
        LIFT_CHECK(got.blend);
        LIFT_CHECK(got.src_blend == lift::GpuBlendFactor::SrcAlpha);
        LIFT_CHECK(got.dst_blend == lift::GpuBlendFactor::One);  // ← 缺省 InvSrcAlpha 会让这条红
        LIFT_CHECK(got.blend_op == lift::GpuBlendOp::Add);
        // sprite 自己的那三项仍然由本模块覆盖（深度关、viewport/scissor = 目标尺寸）。
        LIFT_CHECK(!got.depth_test);
        LIFT_CHECK(!got.depth_write);
    }
    // `End` 恢复的是 **Begin 之前**那一对（2/1），⛔ 不是批次里引擎写过的 5/2。
    {
        lift::d3d9::BlendStateSnapshot after_end;
        if (readBlend(f, dev, &after_end)) {
            LIFT_CHECK(!after_end.blend);
            LIFT_CHECK_EQ_U32(after_end.src_color, lift::d3d9::kBlendOne);
            LIFT_CHECK_EQ_U32(after_end.dst_color, lift::d3d9::kBlendZero);
        }
    }

    // ---- ② 普通批次：Begin 自己设的 5/6 必须被合成路读到（⛔ 引擎残留的 2/1 不得漏进来）----
    LIFT_CHECK_EQ_U32(f.spriteBegin(sprite, lift::d3dx9::kSpriteAlphaBlend), 0u);
    LIFT_CHECK_EQ_U32(f.spriteDraw(sprite, 0u, rect_ea, pos_ea, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(f.spriteEnd(sprite), 0u);
    LIFT_CHECK_EQ_U32(mock.states.size(), 2u);
    if (mock.states.size() == 2u) {
        const lift::GpuRasterState& got = mock.states[1];
        LIFT_CHECK(got.blend);
        LIFT_CHECK(got.src_blend == lift::GpuBlendFactor::SrcAlpha);
        LIFT_CHECK(got.dst_blend == lift::GpuBlendFactor::InvSrcAlpha);
    }

    // ---- ③ ALPHABLEND=0 ⇒ blend=false（因子不被使用，也不许还开着混合）----
    LIFT_CHECK_EQ_U32(f.spriteBegin(sprite, 0u), 0u);
    LIFT_CHECK_EQ_U32(f.spriteDraw(sprite, 0u, rect_ea, pos_ea, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(f.spriteEnd(sprite), 0u);
    LIFT_CHECK_EQ_U32(mock.states.size(), 3u);
    if (mock.states.size() == 3u) {
        const lift::GpuRasterState& got = mock.states[2];
        LIFT_CHECK(!got.blend);
        LIFT_CHECK(!got.separate_alpha_blend);
    }

    // ---- ④ 表达不了的因子（Begin 之后写 20=12）⇒ 这一条**不入队** + 账本 skipped 涨 ----
    //   ★ 必须写在 Begin 之后：真 D3DX 的 Begin 会把它改回 6（这正是本单的语义），
    //     写在 Begin 之前等于测"Begin 会不会吞掉引擎的写"，不是"表达不了会不会静默"。
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsSrcBlend, lift::d3d9::kBlendSrcColor), 0u);   // 3
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsDestBlend, lift::d3d9::kBlendDestColor), 0u);  // 9
    const std::uint64_t skipped_before = lift::d3d9::gpuCoverage().sprite_skipped;
    const std::size_t states_before = mock.states.size();
    LIFT_CHECK_EQ_U32(f.spriteBegin(sprite, lift::d3dx9::kSpriteAlphaBlend), 0u);
    LIFT_CHECK_EQ_U32(f.setRenderState(dev, lift::d3d9::kRsDestBlend, lift::d3d9::kBlendBothSrcAlpha), 0u);
    LIFT_CHECK_EQ_U32(f.spriteDraw(sprite, 0u, rect_ea, pos_ea, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(f.spriteEnd(sprite), 0u);
    LIFT_CHECK_EQ_U32(mock.states.size(), states_before);  // 没交给后端
    LIFT_CHECK(lift::d3d9::gpuCoverage().sprite_skipped > skipped_before);
    // 恢复的仍然是 Begin 之前那对 3/9（批次里引擎写过的 5/12 不参与恢复）。
    {
        lift::d3d9::BlendStateSnapshot after_end;
        if (readBlend(f, dev, &after_end)) {
            LIFT_CHECK(!after_end.blend);
            LIFT_CHECK_EQ_U32(after_end.src_color, lift::d3d9::kBlendSrcColor);
            LIFT_CHECK_EQ_U32(after_end.dst_color, lift::d3d9::kBlendDestColor);
        }
    }
}

}  // namespace

int main() {
    using namespace lift_test;
    try {
        casePureMapping();
        caseBeginRestoresBlendState();
        casePixelTransparentTexel();
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
