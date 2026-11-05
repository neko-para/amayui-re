// emulator-lift · M2 夹具：D3D9 **fixed-function 流水线**（设备状态机 + 软件光栅 → 第一帧）
//
// 判据（对照任务单「M2 · D3D9 fixed-function 流水线」）：
//   ① `SetTransform` 四种状态（VIEW=2 / PROJECTION=3 / WORLD=0x100 / TEXTURE0=16）存取**逐字节一致**；
//      非法 State（5）⇒ D3DERR_INVALIDCALL 且不改状态。
//   ② `BeginScene`/`EndScene` 配对：未 Begin 就 Draw ⇒ D3DERR_INVALIDCALL（真机语义）；嵌套 Begin / 空 End
//      同样 INVALIDCALL；没有 SetFVF 就 Draw ⇒ INVALIDCALL。
//   ③ 纯色三角形（XYZRHW + DIFFUSE，无纹理）⇒ **像素精确**：内部 = 三角形色，外部 = 清屏色；
//      对角边界上的像素中心按**左上填充规则**被排除（见用例里逐点的期望值）。
//   ④ WORLD 变换真的作用：同一三角形在 WORLD 平移 (0.25,0.25) 后画到别处（XYZ 路，身份 VIEW/PROJECTION）。
//   ⑤ 带纹理的四边形：`SetTexture` + `SetSamplerState(POINT)` ⇒ 4 个角像素 = 纹理 4 个角 texel。
//   ⑥ `ZENABLE` / `ZWRITEENABLE`：近的先画、远的后画 ⇒ 遮挡（远的不覆盖）/ 不遮挡（覆盖）各一条；
//      `ZWRITEENABLE=0` 时近的不写深度 ⇒ 远的仍然覆盖。
//   ⑦ `ALPHABLENDENABLE` + `SRCBLEND=SRCALPHA` / `DESTBLEND=INVSRCALPHA` ⇒ 逐通道精确混合值。
//
// ★ 所有方法调用都从 **guest 代码** 经 vtable 调（`mov eax,[vtbl]; push …; call [eax+off]`），
//   测的是"guest 可见的设备"，不是宿主直接调函数。
// ★ 这些用例**会红**（变异验证见交付说明）：SetTransform 不存值 ⇒ ① 红；Z 测试恒开 ⇒ ⑥ 的
//   "近的先画 + 远的后画" 会恒绿而 `ZWRITEENABLE=0` 那条红；混合公式写成 (src+dst)/2 ⇒ ⑦ 红；
//   填充改成"填包围盒" ⇒ ③ 的外部像素断言红。
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
#include "runtime/machine.h"
#include "runtime/surface.h"
#include "shims/register.h"

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

// 设备槽偏移（★ 见 shims/d3d9.cpp 的方法表注释：每条都有 `.lst` 站点或明说未取证）
constexpr std::uint32_t kOffPresent = 0x44u;
constexpr std::uint32_t kOffBeginScene = 0xA4u;
constexpr std::uint32_t kOffEndScene = 0xA8u;
constexpr std::uint32_t kOffClear = 0xACu;
constexpr std::uint32_t kOffSetTransform = 0xB0u;
constexpr std::uint32_t kOffGetTransform = 0xB4u;
constexpr std::uint32_t kOffSetRenderState = 0xE4u;
constexpr std::uint32_t kOffGetRenderState = 0xE8u;
constexpr std::uint32_t kOffSetTexture = 0x104u;
constexpr std::uint32_t kOffSetTextureStageState = 0x10Cu;
constexpr std::uint32_t kOffSetSamplerState = 0x114u;
constexpr std::uint32_t kOffDrawPrimitive = 0x144u;
constexpr std::uint32_t kOffDrawPrimitiveUP = 0x14Cu;
constexpr std::uint32_t kOffSetFVF = 0x164u;
constexpr std::uint32_t kOffSetStreamSource = 0x190u;
constexpr std::uint32_t kOffTextureLockRect = 0x4Cu;
constexpr std::uint32_t kOffTextureUnlockRect = 0x50u;

constexpr std::uint32_t kFvfXyzDiffuse = 0x0042u;              // D3DFVF_XYZ | D3DFVF_DIFFUSE
constexpr std::uint32_t kFvfXyzRhwDiffuse = 0x0044u;           // D3DFVF_XYZRHW | D3DFVF_DIFFUSE
constexpr std::uint32_t kFvfXyzRhwDiffuseTex1 = 0x0144u;       // + TEX1

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
    void movEaxMem(std::uint32_t ea) {  // mov eax, [imm32]
        u8(0xA1);
        u32(ea);
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

    bool boot() {
        if (!m.initBare(&err)) return false;
        if (!m.prepareStack(&err)) return false;
        if (!m.heap().init(&m.mem(), m.layout().heap_base, kFixtureHeapBytes)) return false;
        m.shims().configureStubPage(kStubPage, kStubSlots);
        lift::registerD3D9Shims(m.shims());
        lift::registerD3DX9Shims(m.shims());
        if (!fixStub("d3d9", "Direct3DCreate9", kProcDirect3DCreate9)) return false;
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
    std::uint32_t placeBytes(const std::vector<std::uint8_t>& v) { return placeData(v.data(), v.size()); }
    std::uint32_t placeFloats(const std::vector<float>& v) { return placeData(v.data(), v.size() * sizeof(float)); }

    std::uint32_t runAsmRaw(Asm& a, lift::RunResult* out) {
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
        if (out) *out = r;
        if (r.reason == lift::StopReason::UntilEa) return m.cpu().eax();
        return r.reason == lift::StopReason::Stopped || r.reason == lift::StopReason::Unimplemented
                   ? m.cpu().eax()
                   : 0;
    }
    std::uint32_t runAsm(Asm& a) { return runAsmRaw(a, nullptr); }

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
        std::string why;
        if (!lift::d3d9::deviceInfoOfState(m.cpu(), out->state, &out->info, &why)) {
            lift_test::failf(__FILE__, __LINE__, "deviceInfoOfState 失败：%s", why.c_str());
            return false;
        }
        return true;
    }

    // D3DXCreateTexture(device, w, h, 1, 0, format, POOL_DEFAULT=0, &out)
    std::uint32_t createTexture(std::uint32_t device, std::uint32_t w, std::uint32_t h, std::uint32_t format,
                                std::uint32_t* out_obj) {
        const std::uint32_t out_ea = placeU32(0xDEADBEEFu);
        Asm a;
        a.pushImm(out_ea);
        a.pushImm(0u);          // Pool
        a.pushImm(format);      // Format
        a.pushImm(0u);          // Usage
        a.pushImm(1u);          // MipLevels
        a.pushImm(h);
        a.pushImm(w);
        a.pushImm(device);
        a.callRel(code_ea, kProcD3DXCreateTexture);
        a.ret();
        const std::uint32_t hr = runAsm(a);
        if (out_obj) *out_obj = m.mem().rd32(out_ea);
        return hr;
    }

    // ---- 设备状态 ----
    std::uint32_t beginScene() { return method(device.object, kOffBeginScene, {}); }
    std::uint32_t endScene() { return method(device.object, kOffEndScene, {}); }
    std::uint32_t setRs(std::uint32_t state, std::uint32_t value) {
        return method(device.object, kOffSetRenderState, {state, value});
    }
    std::uint32_t getRs(std::uint32_t state) {
        const std::uint32_t out_ea = placeU32(0xDEADBEEFu);
        const std::uint32_t hr = method(device.object, kOffGetRenderState, {state, out_ea});
        return hr == 0u ? m.mem().rd32(out_ea) : 0xDEADBEEFu;
    }
    std::uint32_t setFvf(std::uint32_t fvf) { return method(device.object, kOffSetFVF, {fvf}); }
    std::uint32_t setSampler(std::uint32_t stage, std::uint32_t type, std::uint32_t value) {
        return method(device.object, kOffSetSamplerState, {stage, type, value});
    }
    std::uint32_t setTexture(std::uint32_t stage, std::uint32_t tex) {
        return method(device.object, kOffSetTexture, {stage, tex});
    }
    std::uint32_t setTransform(std::uint32_t state, const std::vector<float>& m16) {
        const std::uint32_t ea = placeFloats(m16);
        return method(device.object, kOffSetTransform, {state, ea});
    }
    std::uint32_t getTransform(std::uint32_t state, std::vector<float>* out) {
        std::vector<float> zero(16, 0.0f);
        const std::uint32_t buf = placeFloats(zero);
        const std::uint32_t hr = method(device.object, kOffGetTransform, {state, buf});
        out->assign(16, 0.0f);
        for (unsigned i = 0; i < 16; ++i) {
            std::uint32_t bits = m.mem().rd32(buf + i * 4u);
            float f = 0.0f;
            std::memcpy(&f, &bits, sizeof(f));
            (*out)[i] = f;
        }
        return hr;
    }
    std::uint32_t clearTo(std::uint32_t argb) {
        return method(device.object, kOffClear, {0u, 0u, 3u, argb, 0x3F800000u, 0u});
    }
    std::uint32_t drawUp(std::uint32_t prim, std::uint32_t prim_count, std::uint32_t data_ea, std::uint32_t stride) {
        return method(device.object, kOffDrawPrimitiveUP, {prim, prim_count, data_ea, stride});
    }

    std::uint32_t pixelAt(int x, int y) {
        return m.mem().rd32(device.info.arena + static_cast<std::uint32_t>(y) * device.info.pitch +
                            static_cast<std::uint32_t>(x) * 4u);
    }

    // 顶点：XYZRHW + DIFFUSE（20 B；x/y/z/rhw 四个 float + D3DCOLOR）
    std::uint32_t placeRhwDiffuse(const std::vector<std::array<float, 4>>& xyz_rhw,
                                  const std::vector<std::uint32_t>& colors) {
        std::vector<std::uint8_t> bytes;
        bytes.reserve(xyz_rhw.size() * 20u);
        for (std::size_t i = 0; i < xyz_rhw.size(); ++i) {
            for (int k = 0; k < 4; ++k) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &xyz_rhw[i][k], sizeof(bits));
                for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((bits >> (8 * b)) & 0xFF));
            }
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((colors[i] >> (8 * b)) & 0xFF));
        }
        return placeBytes(bytes);
    }

    // 顶点：XYZ + DIFFUSE（16 B；走 WORLD→VIEW→PROJECTION）
    std::uint32_t placeXyzDiffuse(const std::vector<std::array<float, 3>>& xyz, std::uint32_t color) {
        std::vector<std::uint8_t> bytes;
        for (const auto& v : xyz) {
            for (int k = 0; k < 3; ++k) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &v[k], sizeof(bits));
                for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((bits >> (8 * b)) & 0xFF));
            }
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((color >> (8 * b)) & 0xFF));
        }
        return placeBytes(bytes);
    }

    // 顶点：XYZRHW + DIFFUSE + TEX1（28 B）
    struct TexVertex {
        float x, y, z, rhw;
        std::uint32_t color;
        float u, v;
    };
    std::uint32_t placeTexVertices(const std::vector<TexVertex>& verts) {
        std::vector<std::uint8_t> bytes;
        for (const TexVertex& v : verts) {
            auto pushF = [&bytes](float f) {
                std::uint32_t bits = 0;
                std::memcpy(&bits, &f, sizeof(bits));
                for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((bits >> (8 * b)) & 0xFF));
            };
            pushF(v.x);
            pushF(v.y);
            pushF(v.z);
            pushF(v.rhw);
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((v.color >> (8 * b)) & 0xFF));
            pushF(v.u);
            pushF(v.v);
        }
        return placeBytes(bytes);
    }

    Device device;
};

// 一次"建 d3d9 + 64×64 设备"的公共前半段
struct Rig {
    Fixture f;
    bool up() {
        if (!f.boot()) return false;
        const std::uint32_t d3d9 = f.createDirect3D();
        if (d3d9 == 0) return false;
        if (!f.createDevice(d3d9, 64, 64, lift::d3d9::kFmtX8R8G8B8, &f.device)) return false;
        return f.device.object != 0;
    }
};

constexpr std::uint32_t kClearColor = 0xFF102030u;  // 清屏色（X8R8G8B8 ⇒ 存成 0x00102030）
constexpr std::uint32_t kClearPixel = 0x00102030u;

// ---------------------------------------------------------------------------------------
// ① SetTransform 存取一致 + 非法 State
// ---------------------------------------------------------------------------------------
void caseTransformRoundtrip() {
    lift_test::g_current_case = "d3d9p/state+transform";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;

    // 4 种状态各一份不同的矩阵：逐字节回来必须一致
    const std::vector<std::uint32_t> states = {lift::d3d9::kTsView, lift::d3d9::kTsProjection,
                                               lift::d3d9::kTsWorld, lift::d3d9::kTsTexture0};
    for (std::size_t s = 0; s < states.size(); ++s) {
        std::vector<float> m16(16);
        for (int i = 0; i < 16; ++i) m16[static_cast<std::size_t>(i)] = static_cast<float>(s * 100 + i) + 0.5f;
        LIFT_CHECK_EQ_U32(r.f.setTransform(states[s], m16), 0u);
        std::vector<float> got;
        LIFT_CHECK_EQ_U32(r.f.getTransform(states[s], &got), 0u);
        for (int i = 0; i < 16; ++i) LIFT_CHECK(got[static_cast<std::size_t>(i)] == m16[static_cast<std::size_t>(i)]);
    }
    // 非法状态（5）⇒ D3DERR_INVALIDCALL
    LIFT_CHECK_EQ_U32(r.f.setTransform(5u, std::vector<float>(16, 1.0f)), lift::d3d9::kInvalidCall);
    // 非法状态不被写坏：VIEW 还是上一轮的矩阵（第二组：s=0 ⇒ 0.5..15.5）
    std::vector<float> view_got;
    LIFT_CHECK_EQ_U32(r.f.getTransform(lift::d3d9::kTsView, &view_got), 0u);
    LIFT_CHECK(view_got[0] == 0.5f);

    // SetRenderState / GetRenderState 存取一致（含 0 与 1 两个方向）
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.getRs(lift::d3d9::kRsZEnable), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 1u), 0u);
    LIFT_CHECK_EQ_U32(r.f.getRs(lift::d3d9::kRsZEnable), 1u);
    // 非法枚举值 ⇒ D3DERR_INVALIDCALL（不静默存下）
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 7u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.getRs(lift::d3d9::kRsZEnable), 1u);
    // 采样器存取
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfLinear), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMagFilter, 99u), lift::d3d9::kInvalidCall);
    // FVF 存取
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);
    const std::uint32_t fvf_out = r.f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, 0x168u, {fvf_out}), 0u);
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(fvf_out), kFvfXyzRhwDiffuse);
    // 不支持的 FVF（位置类型 = 0x0E XYZB5 之外没有 ⇒ 用 0x1 = RESERVED0）⇒ INVALIDCALL
    LIFT_CHECK_EQ_U32(r.f.setFvf(0x0001u), lift::d3d9::kInvalidCall);
}

// ---------------------------------------------------------------------------------------
// ② BeginScene / EndScene 配对 + 缺 FVF 的 Draw
// ---------------------------------------------------------------------------------------
void caseScenePairing() {
    lift_test::g_current_case = "d3d9p/scene-pairing";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;

    // 没 BeginScene 就 Draw（FVF 先给上）⇒ D3DERR_INVALIDCALL
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);
    const std::uint32_t tri = r.f.placeRhwDiffuse({{0.0f, 0.0f, 0.0f, 1.0f}, {10.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 10.0f, 0.0f, 1.0f}},
                                                  {0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu});
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 1u, tri, 20u), lift::d3d9::kInvalidCall);
    // 没 SetFVF 就 Draw（在场景里）⇒ D3DERR_INVALIDCALL
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    // 清掉 FVF：SetFVF(0) 是合法调用（0 = 没有 FVF）⇒ 之后 Draw 必须失败
    LIFT_CHECK_EQ_U32(r.f.setFvf(0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 1u, tri, 20u), lift::d3d9::kInvalidCall);
    // BeginScene 嵌套 ⇒ INVALIDCALL；EndScene 一次 ⇒ S_OK；再 EndScene ⇒ INVALIDCALL
    LIFT_CHECK_EQ_U32(r.f.beginScene(), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), lift::d3d9::kInvalidCall);
    // 计数器（guest 状态块里的可观察量）
    lift::d3d9::PipelineInfo info;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.begins, 1u);
    LIFT_CHECK_EQ_U32(info.ends, 1u);
    LIFT_CHECK_EQ_U32(info.draws, 0u);
    LIFT_CHECK_EQ_U32(info.draw_calls, 2u);
}

// ---------------------------------------------------------------------------------------
// ③ 纯色三角形：像素精确（含左上填充规则的边界像素）
// ---------------------------------------------------------------------------------------
void caseSolidTriangle() {
    lift_test::g_current_case = "d3d9p/solid-triangle";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;

    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), kClearPixel);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);

    const std::uint32_t tri_color = 0xFFFF8000u;  // A=255 R=255 G=128 B=0（X8R8G8B8 ⇒ 存 0x00FF8000）
    const std::uint32_t tri = r.f.placeRhwDiffuse({{0.0f, 0.0f, 0.0f, 1.0f}, {10.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 10.0f, 0.0f, 1.0f}},
                                                  {tri_color, tri_color, tri_color});
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 1u, tri, 20u), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);

    const std::uint32_t tri_pixel = 0x00FF8000u;
    // 内部：像素中心严格在三条边之内
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), tri_pixel);   // 中心 (0.5,0.5)
    LIFT_CHECK_EQ_U32(r.f.pixelAt(1, 1), tri_pixel);   // (1.5,1.5)
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 8), tri_pixel);   // (0.5,8.5)：0.5+8.5=9 < 10
    LIFT_CHECK_EQ_U32(r.f.pixelAt(7, 1), tri_pixel);   // (7.5,1.5)：9 < 10
    // 外部：包围盒之外 / 对角边之外
    LIFT_CHECK_EQ_U32(r.f.pixelAt(9, 9), kClearPixel);  // 中心 (9.5,9.5)：19 > 10
    LIFT_CHECK_EQ_U32(r.f.pixelAt(20, 3), kClearPixel);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(3, 20), kClearPixel);
    // ★ 边界规则（写死）：对角边 x+y=10 上的像素中心被**排除**（它不是 top/left 边）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 9), kClearPixel);  // (0.5,9.5) 正好在边上
    LIFT_CHECK_EQ_U32(r.f.pixelAt(9, 0), kClearPixel);  // (9.5,0.5) 正好在边上
    LIFT_CHECK_EQ_U32(r.f.pixelAt(8, 1), kClearPixel);  // (8.5,1.5) 正好在边上
    // 而左边 / 上边（整数坐标）不经过任何像素中心 ⇒ 最外一圈内部像素仍然被画
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 1), tri_pixel);  // (0.5,1.5)

    // ★ 同一三角形走 XYZ 路 + WORLD 平移 (0.25,0.25) ⇒ 画到 (40,24)/(56,24)/(40,8) 一带
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    std::vector<float> world(16, 0.0f);
    world[0] = 1.0f;
    world[5] = 1.0f;
    world[10] = 1.0f;
    world[15] = 1.0f;
    world[12] = 0.25f;  // 行向量约定：v' = v*M ⇒ 平移在 M[3][0..2]
    world[13] = 0.25f;
    LIFT_CHECK_EQ_U32(r.f.setTransform(lift::d3d9::kTsWorld, world), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzDiffuse), 0u);
    const std::uint32_t tri_xyz =
        r.f.placeXyzDiffuse({{0.0f, 0.0f, 0.0f}, {0.5f, 0.0f, 0.0f}, {0.0f, 0.5f, 0.0f}}, tri_color);
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 1u, tri_xyz, 16u), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(41, 23), tri_pixel);  // 中心 (41.5,23.5) 在三角形内
    LIFT_CHECK_EQ_U32(r.f.pixelAt(41, 40), kClearPixel);  // 原 (0,0) 附近现在是背景
    LIFT_CHECK_EQ_U32(r.f.pixelAt(1, 1), kClearPixel);
    // 还原 WORLD（后面用例不依赖它，但保持确定性）
    LIFT_CHECK_EQ_U32(r.f.setTransform(lift::d3d9::kTsWorld, std::vector<float>(
        {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1})), 0u);
}

// ---------------------------------------------------------------------------------------
// ④ 带纹理的四边形：POINT 采样 ⇒ 角像素 = 纹理角 texel
// ---------------------------------------------------------------------------------------
void caseTexturedQuad() {
    lift_test::g_current_case = "d3d9p/textured-quad";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;

    // 2×2 纹理（A8R8G8B8 = 21）：对角四色，便于一眼看出采样对不对
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    const std::uint32_t locked = r.f.placeBytes(std::vector<std::uint8_t>(8, 0));
    LIFT_CHECK_EQ_U32(r.f.method(tex, kOffTextureLockRect, {0u, locked, 0u, 0u}), 0u);
    const std::uint32_t pitch = r.f.m.mem().rd32(locked + 0u);
    const std::uint32_t bits = r.f.m.mem().rd32(locked + 4u);
    LIFT_CHECK_NE_U32(bits, 0u);
    const std::uint32_t texels[4] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu};  // (0,0)(1,0)(0,1)(1,1)
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            r.f.m.mem().wr32(bits + static_cast<std::uint32_t>(y) * pitch + static_cast<std::uint32_t>(x) * 4u,
                             texels[y * 2 + x]);
        }
    }
    LIFT_CHECK_EQ_U32(r.f.method(tex, kOffTextureUnlockRect, {0u}), 0u);

    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, tex), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampAddressU, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampAddressV, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuseTex1), 0u);

    // 覆盖 [0,8)×[0,8)，UV 覆盖整个纹理（(0,0)-(8,8) ⇒ u/v 0..1）
    const std::uint32_t white = 0xFFFFFFFFu;
    std::vector<Fixture::TexVertex> quad = {
        {0.0f, 0.0f, 0.0f, 1.0f, white, 0.0f, 0.0f},
        {8.0f, 0.0f, 0.0f, 1.0f, white, 1.0f, 0.0f},
        {0.0f, 8.0f, 0.0f, 1.0f, white, 0.0f, 1.0f},
        {8.0f, 0.0f, 0.0f, 1.0f, white, 1.0f, 0.0f},
        {8.0f, 8.0f, 0.0f, 1.0f, white, 1.0f, 1.0f},
        {0.0f, 8.0f, 0.0f, 1.0f, white, 0.0f, 1.0f},
    };
    const std::uint32_t data = r.f.placeTexVertices(quad);
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, data, 28u), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);

    // 4 个角像素 = 纹理 4 个角 texel（POINT + 全白调制色）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), 0x00FF0000u);  // 左上 = 红
    LIFT_CHECK_EQ_U32(r.f.pixelAt(7, 0), 0x0000FF00u);  // 右上 = 绿
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 7), 0x000000FFu);  // 左下 = 蓝
    LIFT_CHECK_EQ_U32(r.f.pixelAt(7, 7), 0x00FFFFFFu);  // 右下 = 白
    // 四边形之外仍然是清屏色
    LIFT_CHECK_EQ_U32(r.f.pixelAt(8, 0), kClearPixel);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 8), kClearPixel);

    // 解绑纹理 ⇒ 后面的用例不受影响（也让 SetTexture(0,NULL) 的 Release 路径被走到）
    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 0u);  // 纹理 Release 到 0
}

// ---------------------------------------------------------------------------------------
// ④b MIN filter 真的参与采样（旧实现只看 MAG ⇒ 本用例会红）
// ---------------------------------------------------------------------------------------
void caseMinFilterSampling() {
    lift_test::g_current_case = "d3d9p/min-filter";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;

    // 2×2 纹理：左上红 / 右上绿 / 左下蓝 / 右下白（与 caseTexturedQuad 同一组颜色）
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    const std::uint32_t locked = r.f.placeBytes(std::vector<std::uint8_t>(8, 0));
    LIFT_CHECK_EQ_U32(r.f.method(tex, kOffTextureLockRect, {0u, locked, 0u, 0u}), 0u);
    const std::uint32_t pitch = r.f.m.mem().rd32(locked + 0u);
    const std::uint32_t bits = r.f.m.mem().rd32(locked + 4u);
    LIFT_CHECK_NE_U32(bits, 0u);
    const std::uint32_t texels[4] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFFFFu};
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            r.f.m.mem().wr32(bits + static_cast<std::uint32_t>(y) * pitch + static_cast<std::uint32_t>(x) * 4u,
                             texels[y * 2 + x]);
        }
    }
    LIFT_CHECK_EQ_U32(r.f.method(tex, kOffTextureUnlockRect, {0u}), 0u);

    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, tex), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampAddressU, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampAddressV, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuseTex1), 0u);

    const std::uint32_t white = 0xFFFFFFFFu;
    const std::vector<Fixture::TexVertex> quad = {
        {0.0f, 0.0f, 0.0f, 1.0f, white, 0.0f, 0.0f},
        {8.0f, 0.0f, 0.0f, 1.0f, white, 1.0f, 0.0f},
        {0.0f, 8.0f, 0.0f, 1.0f, white, 0.0f, 1.0f},
        {8.0f, 0.0f, 0.0f, 1.0f, white, 1.0f, 0.0f},
        {8.0f, 8.0f, 0.0f, 1.0f, white, 1.0f, 1.0f},
        {0.0f, 8.0f, 0.0f, 1.0f, white, 0.0f, 1.0f},
    };
    const std::uint32_t data = r.f.placeTexVertices(quad);

    auto drawQuad = [&]() {
        LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
        LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, data, 28u), 0u);
        LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    };

    // (a) MAG=POINT、MIN=LINEAR：像素 (2,2) 的中心 u=v=0.3125（texel 0.125 处）⇒ 双线性混合。
    //     旧实现只看 MAG ⇒ 会取 (0,0) 纯红；下面这条就是"MIN 不再被忽略"的像素级证据。
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfLinear), 0u);
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    drawQuad();
    // 手算：tx=ty=0.125
    //   r = 255*(0.875*0.875) + 255*(0.125*0.125) = 199.2 ⇒ 199 (0xC7)
    //   g = b = 255*(0.125*0.875) + 255*(0.125*0.125) = 31.9 ⇒ 32 (0x20)
    LIFT_CHECK_EQ_U32(r.f.pixelAt(2, 2), 0x00C72020u);

    // (b) 两档都 POINT：同一像素回到纯红（证明 (a) 的差异确实来自 MIN，而不是别的东西）
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    drawQuad();
    LIFT_CHECK_EQ_U32(r.f.pixelAt(2, 2), 0x00FF0000u);

    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 0u);
}

// ---------------------------------------------------------------------------------------
// ⑤ ZENABLE / ZWRITEENABLE：遮挡与不遮挡
// ---------------------------------------------------------------------------------------
void drawQuadAtScreen(Rig& r, float x0, float y0, float x1, float y1, float z, std::uint32_t color) {
    std::vector<Fixture::TexVertex> quad = {
        {x0, y0, z, 1.0f, color, 0.0f, 0.0f}, {x1, y0, z, 1.0f, color, 1.0f, 0.0f},
        {x0, y1, z, 1.0f, color, 0.0f, 1.0f}, {x1, y0, z, 1.0f, color, 1.0f, 0.0f},
        {x1, y1, z, 1.0f, color, 1.0f, 1.0f}, {x0, y1, z, 1.0f, color, 0.0f, 1.0f},
    };
    const std::uint32_t data = r.f.placeTexVertices(quad);
    r.f.setFvf(kFvfXyzRhwDiffuseTex1);
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, data, 28u), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
}

void caseZBuffer() {
    lift_test::g_current_case = "d3d9p/zenable-zwrite";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;

    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZFunc, lift::d3d9::kCmpLessEqual), 0u);
    const std::uint32_t green = 0xFF00FF00u;  // 近
    const std::uint32_t red = 0xFFFF0000u;    // 远
    const std::uint32_t green_px = 0x0000FF00u;
    const std::uint32_t red_px = 0x00FF0000u;

    // (a) ZENABLE=1 / ZWRITEENABLE=1：近的先画、远的后画 ⇒ 中心仍是近的（远的不覆盖）
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);  // 同时清深度（Flags=3）
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 1u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZWriteEnable, 1u), 0u);
    drawQuadAtScreen(r, 4.0f, 4.0f, 60.0f, 60.0f, 0.2f, green);  // 近
    drawQuadAtScreen(r, 4.0f, 4.0f, 60.0f, 60.0f, 0.8f, red);    // 远
    LIFT_CHECK_EQ_U32(r.f.pixelAt(32, 32), green_px);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(5, 5), green_px);

    // (b) ZENABLE=0：远的一定覆盖 ⇒ 中心是远的
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    drawQuadAtScreen(r, 4.0f, 4.0f, 60.0f, 60.0f, 0.2f, green);
    drawQuadAtScreen(r, 4.0f, 4.0f, 60.0f, 60.0f, 0.8f, red);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(32, 32), red_px);

    // (c) ZENABLE=1 / ZWRITEENABLE=0：近的不写深度 ⇒ 远的仍然通过（覆盖）
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 1u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZWriteEnable, 0u), 0u);
    drawQuadAtScreen(r, 4.0f, 4.0f, 60.0f, 60.0f, 0.2f, green);
    drawQuadAtScreen(r, 4.0f, 4.0f, 60.0f, 60.0f, 0.8f, red);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(32, 32), red_px);
    // ZWRITEENABLE 恢复 1（后面用例不依赖它，但保持确定）
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZWriteEnable, 1u), 0u);
}

// ---------------------------------------------------------------------------------------
// ⑥ 混合：SRCALPHA / INVSRCALPHA 的逐通道精确值
// ---------------------------------------------------------------------------------------
void caseAlphaBlend() {
    lift_test::g_current_case = "d3d9p/alphablend";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;

    // 目标 = 不透明蓝；源 = 半透明红（A=128）
    const std::uint32_t dst_argb = 0xFF0000FFu;
    LIFT_CHECK_EQ_U32(r.f.clearTo(dst_argb), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 1u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsSrcBlend, lift::d3d9::kBlendSrcAlpha), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsDestBlend, lift::d3d9::kBlendInvSrcAlpha), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsBlendOp, lift::d3d9::kBlendOpAdd), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);

    const std::uint32_t src = 0x80FF0000u;  // A=128 R=255 G=0 B=0
    const std::uint32_t tri =
        r.f.placeRhwDiffuse({{0.0f, 0.0f, 0.0f, 1.0f}, {20.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 20.0f, 0.0f, 1.0f}},
                            {src, src, src});
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 1u, tri, 20u), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);

    // 逐通道：out = (src*sf + dst*df + 127) / 255（8 位整数，⛔ 不是浮点也能"差不多"）
    //   sf = 128, df = 127
    //   R: (255*128 + 0*127 + 127)/255 = 128
    //   G: 0
    //   B: (0*128 + 255*127 + 127)/255 = 127
    //   A: X8R8G8B8 没有 alpha 通道 ⇒ 目标 alpha 按 255 读；写回时丢 alpha
    LIFT_CHECK_EQ_U32(r.f.pixelAt(1, 1), 0x0080007Fu);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(5, 2), 0x0080007Fu);
    // 三角形之外 = 清屏色（混合只作用在被覆盖的像素上）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(40, 40), 0x000000FFu);

    // 反例：DESTBLEND 非法值 ⇒ INVALIDCALL（不静默当默认）
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsDestBlend, 0u), lift::d3d9::kInvalidCall);
}

}  // namespace

int main() {
    using namespace lift_test;
    try {
        caseTransformRoundtrip();
        caseScenePairing();
        caseSolidTriangle();
        caseTexturedQuad();
        caseMinFilterSampling();
        caseZBuffer();
        caseAlphaBlend();
        return finish("d3d9-pipeline");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "DIAG uncaught std::exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "DIAG uncaught non-std exception\n");
        return 1;
    }
}
