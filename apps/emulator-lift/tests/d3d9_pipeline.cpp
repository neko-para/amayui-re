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
//   ⑧ **每次 Draw 的状态快照**（偏差 #4）：纹理级/采样器状态只读一遍 ⇒ `stage_snapshots == draws`，
//      ⛔ 不随三角形/像素增长；像素结果与逐像素读 guest 时完全一致。
//   ⑨ **六平面裁剪**（偏差 #5）：远平面 / 左 / 右 / 上 / 下平面外的三角形一个像素都不写，
//      完全在内的三角形像素不变；预变换（XYZRHW）顶点按视口矩形裁 x/y。
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
constexpr std::uint32_t kOffSetViewport = 0xBCu;
constexpr std::uint32_t kOffSetRenderTarget = 0x94u;         // ★ 0x4A778B
constexpr std::uint32_t kOffTextureGetSurfaceLevel = 0x48u;  // ★ 0x4A7755
constexpr std::uint32_t kOffRelease = 0x08u;                 // IUnknown::Release（所有 COM 对象同槽）

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
        // ★ 白名单 = "调用返回了 EAX"（不是"成功"）；新增原因必须在内，见 tests/d3dx9.cpp 同款注释。
        return r.reason == lift::StopReason::Stopped || r.reason == lift::StopReason::Unimplemented ||
                       r.reason == lift::StopReason::Trap || r.reason == lift::StopReason::ResourceExhausted
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
    std::uint32_t setTextureStage(std::uint32_t stage, std::uint32_t type, std::uint32_t value) {
        return method(device.object, kOffSetTextureStageState, {stage, type, value});
    }
    // D3DVIEWPORT9：X/Y/Width/Height 是 DWORD，MinZ/MaxZ 是 float（共 24 B）
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
        wr(16u, 0x00000000u);  // MinZ = 0.0f
        wr(20u, 0x3F800000u);  // MaxZ = 1.0f
        return method(device.object, kOffSetViewport, {placeBytes(vp)});
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

// ---------------------------------------------------------------------------------------
// ⑧ 每像素读 guest 的消除：纹理级/采样器状态每次 Draw 快照一次（偏差 #4）
// ---------------------------------------------------------------------------------------
void caseDrawStateSnapshot() {
    lift_test::g_current_case = "d3d9p/draw-state-snapshot";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;

    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);
    // 让像素循环里真的有两级纹理状态：级 1 也开 MODULATE(CURRENT × TEX)。没有绑定纹理 ⇒
    // TEX 是白色 ⇒ 结果 = CURRENT，**像素结果不变**；但旧实现会为每个像素读多次 texStageOf/samplerOf。
    LIFT_CHECK_EQ_U32(r.f.setTextureStage(1u, lift::d3d9::kTssColorOp, lift::d3d9::kTopModulate), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTextureStage(1u, lift::d3d9::kTssAlphaOp, lift::d3d9::kTopModulate), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTextureStage(1u, lift::d3d9::kTssColorArg1, lift::d3d9::kTaCurrent), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTextureStage(1u, lift::d3d9::kTssColorArg2, lift::d3d9::kTaTexture), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTextureStage(1u, lift::d3d9::kTssAlphaArg1, lift::d3d9::kTaCurrent), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTextureStage(1u, lift::d3d9::kTssAlphaArg2, lift::d3d9::kTaTexture), 0u);

    // 16 个三角形 / 48 个顶点（一次 Draw）：4×4 网格，每个格子画左下直角三角形
    const std::uint32_t tri_color = 0xFF00FF00u;
    const std::uint32_t tri_pixel = 0x0000FF00u;
    std::vector<std::array<float, 4>> verts;
    for (int t = 0; t < 16; ++t) {
        const float x = static_cast<float>((t % 4) * 8);
        const float y = static_cast<float>((t / 4) * 8);
        verts.push_back({x, y, 0.0f, 1.0f});
        verts.push_back({x + 4.0f, y, 0.0f, 1.0f});
        verts.push_back({x, y + 4.0f, 0.0f, 1.0f});
    }
    const std::uint32_t data = r.f.placeRhwDiffuse(verts, std::vector<std::uint32_t>(verts.size(), tri_color));

    auto drawAll = [&]() {
        LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
        LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 16u, data, 20u), 0u);
        LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    };
    drawAll();

    lift::d3d9::PipelineInfo info;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    // ★ 结构判据：快照次数 == Draw 次数（⛔ 不随三角形/像素增长）。
    //   把像素循环改回 texStageOf/samplerOf（删掉 prepareContext 里的快照）⇒ 这里恒 0 ⇒ 红。
    LIFT_CHECK_EQ_U32(info.draws, 1u);
    LIFT_CHECK_EQ_U32(info.triangles, 16u);
    LIFT_CHECK_EQ_U32(info.stage_snapshots, 1u);
    // 快照没有改像素结果：三角形内部 = 顶点色，对角边另一侧 / 格子外 = 清屏色
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), tri_pixel);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(9, 9), tri_pixel);   // (9.5,9.5)：格子 (8,8) 的三角形内（x+y=19 < 20）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(2, 6), kClearPixel);  // 同一格子的 (2.5,6.5)：x+y=9 > 4 ⇒ 三角形外
    LIFT_CHECK_EQ_U32(r.f.pixelAt(6, 2), kClearPixel);

    // 第二次 Draw ⇒ 快照再 +1（失败/未进入光栅的调用不会假装快照过 —— 这里两次都成功）
    drawAll();
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.draws, 2u);
    LIFT_CHECK_EQ_U32(info.triangles, 32u);
    LIFT_CHECK_EQ_U32(info.stage_snapshots, 2u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(9, 9), tri_pixel);
}

// ---------------------------------------------------------------------------------------
// ⑨ 六平面裁剪（偏差 #5）：远 / 左 / 右 / 上 / 下平面外的三角形不写像素；
//    完全在内的三角形像素不变；预变换顶点的 x/y 裁剪 = 视口矩形。
//    ★ 视口故意设成 32×32 @ (16,16)、后缓冲仍是 64×64 ⇒ 被侧平面裁掉的几何**落在后缓冲内**，
//      旧实现（只裁近平面 + scissor 兜底）会写像素；新实现必须一个像素都不写。
// ---------------------------------------------------------------------------------------
void drawXyzTri(Rig& r, const std::array<std::array<float, 3>, 3>& xyz, std::uint32_t color) {
    const std::uint32_t data = r.f.placeXyzDiffuse({xyz[0], xyz[1], xyz[2]}, color);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzDiffuse), 0u);
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 1u, data, 16u), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
}

void caseClipPlanes() {
    lift_test::g_current_case = "d3d9p/clip-planes";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;

    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);  // 远平面要能"本来会画"才可观测
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setViewport(16u, 16u, 32u, 32u), 0u);

    const std::uint32_t in_color = 0xFF00FF00u;   // 绿：完全在内的三角形
    const std::uint32_t out_color = 0xFFFF0000u;  // 红：被裁掉的三角形（只要画了就能看出来）
    const std::uint32_t in_px = 0x0000FF00u;
    const std::uint32_t out_px = 0x00FF0000u;
    // 完全在内：x,y ∈ [-0.5,0.5] ⇒ 屏幕 (24,40)-(40,40)-(40,24)（视口内）
    const std::array<std::array<float, 3>, 3> inside = {
        {{-0.5f, -0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 0.0f}}};

    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    drawXyzTri(r, inside, in_color);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(38, 26), in_px);   // (38.5,26.5)：x+y=65 > 64 ⇒ 斜边内侧
    LIFT_CHECK_EQ_U32(r.f.pixelAt(39, 39), in_px);   // (39.5,39.5)：直角附近
    LIFT_CHECK_EQ_U32(r.f.pixelAt(30, 30), kClearPixel);  // (30.5,30.5)：x+y=61 < 64 ⇒ 三角形外

    // ① 远平面：z=1.5（w=1）⇒ w-z=-0.5。ZENABLE=0 时旧实现会把它画在**同一个位置**（红色）。
    drawXyzTri(r, {{{-0.5f, -0.5f, 1.5f}, {0.5f, -0.5f, 1.5f}, {0.5f, 0.5f, 1.5f}}}, out_color);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(38, 26), in_px);  // 绿还在 ⇒ 远平面外的三角形连像素都没写
    // ② 右平面：x/w ∈ [1.2,1.8] ⇒ 屏幕 x≈51..61（后缓冲内、视口外）
    drawXyzTri(r, {{{1.2f, -0.2f, 0.0f}, {1.8f, -0.2f, 0.0f}, {1.2f, 0.2f, 0.0f}}}, out_color);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(53, 32), kClearPixel);
    // ③ 左平面：x/w ∈ [-1.8,-1.2] ⇒ 屏幕 x≈3..13
    drawXyzTri(r, {{{-1.8f, -0.2f, 0.0f}, {-1.2f, -0.2f, 0.0f}, {-1.8f, 0.2f, 0.0f}}}, out_color);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(6, 32), kClearPixel);
    // ④ 上平面：y/w ∈ [1.2,1.8] ⇒ 屏幕 y≈3..13
    drawXyzTri(r, {{{-0.2f, 1.2f, 0.0f}, {0.2f, 1.2f, 0.0f}, {-0.2f, 1.8f, 0.0f}}}, out_color);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(32, 6), kClearPixel);
    // ⑤ 下平面：y/w ∈ [-1.8,-1.2] ⇒ 屏幕 y≈51..61
    drawXyzTri(r, {{{-0.2f, -1.8f, 0.0f}, {0.2f, -1.8f, 0.0f}, {-0.2f, -1.2f, 0.0f}}}, out_color);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(32, 56), kClearPixel);

    // 计数器：6 个三角形进装配，只有"在内的那个"真的进入光栅化（旧实现会是 6 ⇒ 红）
    lift::d3d9::PipelineInfo info;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.triangles, 6u);
    LIFT_CHECK_EQ_U32(info.rasterized, 1u);

    // ⑥ 部分越界：右平面把三角形**切开**（不是整块丢）—— 屏幕 x 40..48 的可见部分仍画，
    //    48..56 的被裁掉。视口内 (44,36) = 红；(52,38) 旧实现会画、新实现必须不画。
    drawXyzTri(r, {{{0.5f, -0.5f, 0.0f}, {1.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 0.0f}}}, out_color);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(44, 36), out_px);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(52, 38), kClearPixel);
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.triangles, 7u);
    LIFT_CHECK_EQ_U32(info.rasterized, 2u);  // 切开的三角形仍算"进入了光栅化"（只多了顶点）

    // ⑦ XYZRHW 的 x/y 裁剪 = **视口矩形**（齐次面不适用）：屏幕 x=52..60 在视口 16..48 外、
    //    后缓冲（64）内 ⇒ 旧实现会写红色；新实现必须裁掉。
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);
    std::vector<std::array<float, 4>> outside_quad = {
        {52.0f, 20.0f, 0.0f, 1.0f}, {60.0f, 20.0f, 0.0f, 1.0f}, {52.0f, 28.0f, 0.0f, 1.0f},
        {60.0f, 20.0f, 0.0f, 1.0f}, {60.0f, 28.0f, 0.0f, 1.0f}, {52.0f, 28.0f, 0.0f, 1.0f}};
    const std::uint32_t outside_data =
        r.f.placeRhwDiffuse(outside_quad, std::vector<std::uint32_t>(outside_quad.size(), out_color));
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, outside_data, 20u), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(55, 24), kClearPixel);

    // 视口内的同一块四边形仍然照画（证明挡住的是"视口外的部分"，不是整条预变换路径）
    std::vector<std::array<float, 4>> inside_quad = {
        {20.0f, 20.0f, 0.0f, 1.0f}, {28.0f, 20.0f, 0.0f, 1.0f}, {20.0f, 28.0f, 0.0f, 1.0f},
        {28.0f, 20.0f, 0.0f, 1.0f}, {28.0f, 28.0f, 0.0f, 1.0f}, {20.0f, 28.0f, 0.0f, 1.0f}};
    const std::uint32_t inside_data =
        r.f.placeRhwDiffuse(inside_quad, std::vector<std::uint32_t>(inside_quad.size(), out_color));
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, inside_data, 20u), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(24, 24), out_px);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(55, 24), kClearPixel);

    // 11 个三角形进装配；光栅化的 = 在内的 XYZ 三角形 1 + 部分越界（切开的）1 + 视口内四边形的 2
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.triangles, 11u);
    LIFT_CHECK_EQ_U32(info.rasterized, 4u);
}

// ---------------------------------------------------------------------------------------
// ⑩ 渲染目标的生命周期：纹理当 RT 之后**不换 target** 就 Release 它
//   （REQ-01M6X4N7C0PAF6XT7EAC172V85）
// ---------------------------------------------------------------------------------------
// 时序（照需求单造，也是"游戏数据决定的顺序"，⛔ 不是本仓能担保的不变量）：
//   绑定纹理 A 的表面为 RT → **不换 target** → Release 纹理 A → 再画一笔（Clear(TARGET)）。
// ★ 判据（每条都在破坏时红）：
//   ① 释放 A 之后流水线属性仍指 A 的像素块，且那块**还活着**（绑定 = 对像素块的语义 addRef）；
//   ② 同尺寸的新占用者 B 的像素一个都不许变（修前 arena 把 A 的地址复用给 B ⇒ Clear 写进 B）；
//   ③ Clear(TARGET) 落到仍在绑定的 RT（A 的像素块），⛔ 不是后缓冲；
//   ④ 换 target 时成对释放：旧的 owner + surface 一起放，A 的像素块回收（重复设同一 target 不多占一份）；
//   ⑤ 换回后缓冲时成对释放；
//   ⑥ 同一纹理的两个 surface 互切（guest 已放掉纹理）⇒ 新 owner 的 addRef 必须先于旧 owner 的 release
//     （否则旧 owner 当场到 0、状态块/像素块被回收，换绑直接失败）；
//   ⑦ 设备销毁时仍在绑定 ⇒ owner + surface 成对释放、像素块回收（⛔ 不是泄漏）。
// ★ 修前：① 的 `find(a_arena)` 为 null（`destroyTextureState` 已经把像素块回收了；surface 对象的
//   AddRef 只保住对象、保不住像素块）⇒ ② 的 Clear 写进拿到同一地址的 B ⇒ ② 红。
void caseRenderTargetLifetime() {
    lift_test::g_current_case = "d3d9p/render-target-lifetime";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;
    auto& mem = r.f.m.mem();

    constexpr std::uint32_t kTexel = 0x11223344u;  // A 的哨兵 texel（A8R8G8B8 ⇒ 原样存）
    constexpr std::uint32_t kRtClearColor = 0xFF102030u;
    constexpr std::uint32_t kRtClearPixel = 0xFF102030u;  // A8R8G8B8 ⇒ 保留 alpha 通道

    // ---- A：2×2 纹理，4 个 texel 写成哨兵（走 guest 的 LockRect / UnlockRect）----
    std::uint32_t tex_a = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex_a), 0u);
    LIFT_CHECK_NE_U32(tex_a, 0u);
    if (tex_a == 0) return;
    lift::d3d9::TextureInfo ta;
    LIFT_CHECK(lift::d3d9::textureInfo(r.f.m.cpu(), tex_a, &ta, nullptr));
    const std::uint32_t a_arena = ta.arena;
    LIFT_CHECK_NE_U32(a_arena, 0u);
    const std::uint32_t locked = r.f.placeBytes(std::vector<std::uint8_t>(8u, 0u));
    LIFT_CHECK_EQ_U32(r.f.method(tex_a, kOffTextureLockRect, {0u, locked, 0u, 0u}), 0u);
    const std::uint32_t a_pitch = mem.rd32(locked + 0u);
    const std::uint32_t a_bits = mem.rd32(locked + 4u);
    LIFT_CHECK_NE_U32(a_bits, 0u);
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            mem.wr32(a_bits + static_cast<std::uint32_t>(y) * a_pitch + static_cast<std::uint32_t>(x) * 4u,
                     kTexel);
        }
    }
    LIFT_CHECK_EQ_U32(r.f.method(tex_a, kOffTextureUnlockRect, {0u}), 0u);
    // 哨兵真的落在 A 的像素块里（否则后面"Clear 落到 A"断言恒真）
    {
        const lift::GuestSurface* gs = r.f.m.surfaces().find(a_arena);
        LIFT_CHECK(gs != nullptr);
        if (gs != nullptr) LIFT_CHECK_EQ_U32(mem.rd32(gs->ea), kTexel);
    }

    // ---- 绑定为 RT（GetSurfaceLevel(0) 与真实游戏的 ReadFrameTex 时序一致）----
    const std::uint32_t surface_out = r.f.placeU32(0u);
    LIFT_CHECK_EQ_U32(r.f.method(tex_a, kOffTextureGetSurfaceLevel, {0u, surface_out}), 0u);
    const std::uint32_t surface = mem.rd32(surface_out);
    LIFT_CHECK_NE_U32(surface, 0u);
    if (surface == 0) return;
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {surface, 0u}), 0u);
    lift::d3d9::PipelineInfo pi;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr));
    LIFT_CHECK_EQ_U32(pi.color_arena, a_arena);
    LIFT_CHECK_EQ_U32(pi.color_surface, surface);

    // ---- ★ 不换 target，直接 Release 纹理 A（guest 的那份引用）----
    r.f.method(tex_a, kOffRelease, {});

    // ① 流水线属性仍指 A 的像素块；那块像素必须**还活着**（绑定 = 语义 addRef）
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr));
    LIFT_CHECK_EQ_U32(pi.color_arena, a_arena);
    LIFT_CHECK_EQ_U32(pi.color_surface, surface);
    LIFT_CHECK(r.f.m.surfaces().find(a_arena) != nullptr);

    // ---- 逼 arena 复用同一地址：同尺寸的新占用者 B ----
    std::uint32_t tex_b = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex_b), 0u);
    LIFT_CHECK_NE_U32(tex_b, 0u);
    lift::d3d9::TextureInfo tb;
    LIFT_CHECK(lift::d3d9::textureInfo(r.f.m.cpu(), tex_b, &tb, nullptr));
    const std::uint32_t b_arena = tb.arena;
    LIFT_CHECK_NE_U32(b_arena, 0u);
    // 记下 B 的初始像素（4 个 texel 都应为 0），Clear 之后逐字比对
    std::uint32_t b_before[4] = {0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu};
    {
        const lift::GuestSurface* gs = r.f.m.surfaces().find(b_arena);
        LIFT_CHECK(gs != nullptr);
        if (gs != nullptr) {
            for (int y = 0; y < 2; ++y) {
                for (int x = 0; x < 2; ++x) {
                    b_before[y * 2 + x] =
                        mem.rd32(gs->ea + static_cast<std::uint32_t>(y) * gs->pitch +
                                 static_cast<std::uint32_t>(x) * 4u);
                }
            }
        }
    }

    // ---- 再画一笔：Clear(TARGET) 必须落到仍在绑定的 RT（A 的像素块）----
    const std::uint32_t backbuffer_before = r.f.pixelAt(0, 0);
    LIFT_CHECK_EQ_U32(r.f.clearTo(kRtClearColor), 0u);
    // ③ 后缓冲没被写（⛔ 不许把仍在绑定中的 RT 静默换成后缓冲）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), backbuffer_before);
    // ③ Clear 真的写进 A 的像素块
    {
        const lift::GuestSurface* gs = r.f.m.surfaces().find(a_arena);
        LIFT_CHECK(gs != nullptr);
        if (gs != nullptr) LIFT_CHECK_EQ_U32(mem.rd32(gs->ea), kRtClearPixel);
    }
    // ② 新占用者 B 的像素一个都没变
    {
        const lift::GuestSurface* gs = r.f.m.surfaces().find(b_arena);
        LIFT_CHECK(gs != nullptr);
        if (gs != nullptr) {
            for (int y = 0; y < 2; ++y) {
                for (int x = 0; x < 2; ++x) {
                    LIFT_CHECK_EQ_U32(mem.rd32(gs->ea + static_cast<std::uint32_t>(y) * gs->pitch +
                                               static_cast<std::uint32_t>(x) * 4u),
                                      b_before[y * 2 + x]);
                }
            }
        }
    }

    // ---- ④ 成对释放①：**换 target**（不经 0）⇒ 旧 owner + 旧 surface 一起放，A 的像素块回收 ----
    //   ★ 先让 guest 把 surface 的那份引用也放掉（绑定期间 surface 只剩流水线那一份）：
    //     "GPU 绑着、guest 两份引用都不在了" 是这条生命周期线最紧的时序。
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {surface, 0u}), 0u);  // 重复设同一 target
    r.f.method(surface, kOffRelease, {});
    LIFT_CHECK(r.f.m.cpu().com->find(surface));  // surface 仍被流水线持有（对象还在）
    std::uint32_t tex_c = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex_c), 0u);
    lift::d3d9::TextureInfo tc;
    LIFT_CHECK(lift::d3d9::textureInfo(r.f.m.cpu(), tex_c, &tc, nullptr));
    const std::uint32_t c_arena = tc.arena;
    const std::uint32_t c_surface_out = r.f.placeU32(0u);
    LIFT_CHECK_EQ_U32(r.f.method(tex_c, kOffTextureGetSurfaceLevel, {0u, c_surface_out}), 0u);
    const std::uint32_t c_surface = mem.rd32(c_surface_out);
    LIFT_CHECK_NE_U32(c_surface, 0u);
    // 直接 A 的表面 → C 的表面（不经后缓冲）：旧 owner（A 的纹理）与旧 surface 成对释放
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {c_surface, 0u}), 0u);
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr));
    LIFT_CHECK_EQ_U32(pi.color_arena, c_arena);
    LIFT_CHECK_EQ_U32(pi.color_surface, c_surface);
    LIFT_CHECK(r.f.m.surfaces().find(a_arena) == nullptr);   // A 的像素块随旧 owner 释放而回收
    LIFT_CHECK(r.f.m.surfaces().find(c_arena) != nullptr);   // C（新 RT）的像素块活着
    LIFT_CHECK(!r.f.m.cpu().com->find(surface));             // 旧 surface 对象已释放（guest 早放了它那份）

    // ---- ⑤ 成对释放②：换回后缓冲 ⇒ C 的像素块也回收 ----
    //   ★ 同 ④：先把 guest 的两份引用放掉，让流水线成为唯一持有者。
    r.f.method(tex_c, kOffRelease, {});
    r.f.method(c_surface, kOffRelease, {});
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, 0u}), 0u);
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr));
    LIFT_CHECK_EQ_U32(pi.color_arena, 0u);
    LIFT_CHECK_EQ_U32(pi.color_surface, 0u);
    LIFT_CHECK(r.f.m.surfaces().find(c_arena) == nullptr);
    LIFT_CHECK(!r.f.m.cpu().com->find(c_surface));

    // ---- ⑥ 同一纹理的两个 surface 互切（释放**顺序**不变量）----
    //   ★ 时序：guest 已经放掉纹理 E（只剩流水线那一份 owner 引用）⇒ 再绑 E 的**另一个** surface。
    //     如果实现先 release 旧 owner 再 addRef 新 owner，旧 owner 会当场降到 0 ⇒
    //     `destroyTextureState` 回收像素块 + 状态块 ⇒ 绑定直接失败（本用例红）。
    //     ⇒ 判据：绑 E2 必须成功、像素块还在、属性指向 E2 的 arena。
    std::uint32_t tex_e = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex_e), 0u);
    lift::d3d9::TextureInfo te;
    LIFT_CHECK(lift::d3d9::textureInfo(r.f.m.cpu(), tex_e, &te, nullptr));
    const std::uint32_t e_arena = te.arena;
    const std::uint32_t e1_out = r.f.placeU32(0u);
    const std::uint32_t e2_out = r.f.placeU32(0u);
    LIFT_CHECK_EQ_U32(r.f.method(tex_e, kOffTextureGetSurfaceLevel, {0u, e1_out}), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(tex_e, kOffTextureGetSurfaceLevel, {0u, e2_out}), 0u);
    const std::uint32_t e1 = mem.rd32(e1_out);
    const std::uint32_t e2 = mem.rd32(e2_out);
    LIFT_CHECK_NE_U32(e1, 0u);
    LIFT_CHECK_NE_U32(e2, 0u);
    LIFT_CHECK(e1 != e2);  // 每次 GetSurfaceLevel 都建一个新 surface 对象（同一 owner）
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {e1, 0u}), 0u);
    r.f.method(tex_e, kOffRelease, {});  // ← 只剩流水线那一份 owner 引用
    r.f.method(e1, kOffRelease, {});     //   旧 surface 只剩流水线那一份
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {e2, 0u}), 0u);
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr));
    LIFT_CHECK_EQ_U32(pi.color_surface, e2);
    LIFT_CHECK_EQ_U32(pi.color_arena, e_arena);
    LIFT_CHECK(r.f.m.surfaces().find(e_arena) != nullptr);  // owner 没被旧 owner 的 release 降到 0
    LIFT_CHECK(!r.f.m.cpu().com->find(e1));                 // 旧 surface 已随换绑释放
    r.f.method(e2, kOffRelease, {});                        // E2 也只剩流水线那一份
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, 0u}), 0u);
    LIFT_CHECK(r.f.m.surfaces().find(e_arena) == nullptr);
    LIFT_CHECK(!r.f.m.cpu().com->find(e2));

    // ---- ⑦ 成对释放③：设备销毁时仍在绑定 ⇒ owner + surface 都要放，像素块要回收 ----
    std::uint32_t tex_d = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex_d), 0u);
    lift::d3d9::TextureInfo td;
    LIFT_CHECK(lift::d3d9::textureInfo(r.f.m.cpu(), tex_d, &td, nullptr));
    const std::uint32_t d_arena = td.arena;
    const std::uint32_t d_surface_out = r.f.placeU32(0u);
    LIFT_CHECK_EQ_U32(r.f.method(tex_d, kOffTextureGetSurfaceLevel, {0u, d_surface_out}), 0u);
    const std::uint32_t d_surface = mem.rd32(d_surface_out);
    LIFT_CHECK_NE_U32(d_surface, 0u);
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {d_surface, 0u}), 0u);
    // guest 把两份引用都放掉（纹理 + surface），只剩流水线那一份
    r.f.method(tex_d, kOffRelease, {});
    r.f.method(d_surface, kOffRelease, {});
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffRelease, {}), 0u);  // 设备到 0 ⇒ destroyDeviceState
    LIFT_CHECK(r.f.m.surfaces().find(d_arena) == nullptr);  // D 的像素块随 RT 解绑回收（⛔ 不是泄漏）
    LIFT_CHECK(!r.f.m.cpu().com->find(d_surface));

    // 清理：B（guest 的引用；A/C/D/E 的引用在 ③④⑤⑥⑦ 里已经放掉 / 随 RT 回收）
    LIFT_CHECK_EQ_U32(r.f.method(tex_b, kOffRelease, {}), 0u);
}

// ---------------------------------------------------------------------------------------
// ⑪ 陈旧 owner：EA 被复用后，宿主侧缓存的那个 owner **不许**打到新对象
//   （REQ-01M6X4NFHCW1Y6YDJ4DYFA92PS）
// ---------------------------------------------------------------------------------------
// ⚠⚠ **本用例的 ① 是一条"已知偏离"，不是真机语义** —— 读断言之前先读这段：
//   · 真机 D3D9（引擎的口径，A 方案）：`GetSurfaceLevel` 建的 surface **替父纹理持一份资源引用**
//     ⇒ `Release(纹理)` 之后 surface 仍代表那张纹理，`SetRenderTarget(surface)` **应当成功**。
//   · 本线（**有意的偏离**）：surface **不持有**父纹理（`com.h` 的 `owner` 说明、`shims/d3dx9_surface.h`）
//     ⇒ guest 合法地把纹理 Release 到 0 之后，surface 缓存的 owner EA 已经死了 / 可以被复用，
//     所以本线**只能拒绝**绑定（`D3DERR_INVALIDCALL`）。跟踪单：`REQ-01M6ZZZ6PEVSFB93C6VJPKC04C`
//     （= 让 surface 持父纹理 / A 方案）。
//   · ⇒ **修 04C（若按 04C 正文的候选 A：surface 持有父纹理）之后，① 这条断言会翻面**
//     （同一时序应当**成功**、`color_arena` 指向该纹理的 arena）。
//     ⛔ 那时不要"改断言换绿"：按 04C 的判据把 ① 的期望值改成 kOk，并把症状描述留在 04C 里。
//   · 可达性：本仓**未在游戏路径观测到**该顺序（需求单 repro 也这么记："游戏路径里没观测到陈旧 Release"）；
//     本用例是用夹具按判据造出来的最小重现，**不**代表"游戏里必然发生"。
// ★ 判据（每条都在破坏时红）：
//   ① 同尺寸的新纹理 T2 会**同时**拿到 T1 的对象 EA **与** `'TEX1'` 状态块 EA（`kTxBytes` 是常量）
//      ⇒ 旧判据 `userOf(owner) != tex_state` 在这一格**通过**（它认对象靠 "EA + guest 派生状态"）
//      ⇒ 旧实现会**静默**把 RT 绑到 T2 的像素上；代际判据下必须是上面的 INVALIDCALL（已知偏离）。
//   ② 拒绝之后 T2 一个引用都不多、像素块不写、流水线属性停在"后缓冲"；
//   ③ 解绑时流水线缓存的 owner **走 `releaseIf`**：代际不符 ⇒ 拒绝，T2 的 refcount 不动、对象还在。
//      ⚠ ③ 要造出"绑定中 owner 已死"的状态 ⇒ 只能让 guest **越过自己的引用计数**释放（越界），
//      所以它是**防御纵深**，不是合法时序路径（合法路径见 ⑩⑥：绑定期的 addRef 让纹理活到解绑）。
void caseStaleOwnerGeneration() {
    // 用例名带 `known-deviation`：红的时候输出里就能看出"这条断言是本线的偏离，不是真机语义"。
    lift_test::g_current_case = "d3d9p/stale-owner-generation-known-deviation";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;
    auto& mem = r.f.m.mem();
    lift::ComRegistry* com = r.f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    // ---- 夹具：造一组"宿主侧缓存了 (EA, 代际)"的持有者 ----
    // 顺序是判据的一部分（guest 堆 = 首次适配 + `min_split`，见 `runtime/heap.cpp`）：
    //   owner 纹理 → **哑纹理** → owner 的 level-0 表面。
    // ★ 为什么要那张哑纹理：owner 释放后，它自己的三块（`'TEX1'` 状态块 112 B / vtable 96 B /
    //   对象 24 B）连成的洞是 232 B —— 下一个同尺寸纹理要先切走 112+96，剩下的 24 B **小于**
    //   `min_split`(32) ⇒ 对象块会落到堆顶、拿不到原 EA。哑纹理（同尺寸）紧跟在 owner 后面 ⇒
    //   两者一起释放时洞连成 464 B ⇒ 新纹理的三块**逐个回到** owner 的那三个地址。
    //   ⛔ 这不是"为了让断言变绿"：前提（EA 复用）本身在下面**硬断言**，不成立就红。
    //   ★ 若那几条"前提"断言红了：先按 `runtime/heap.cpp` 的分块/`min_split` **重造夹具**，
    //     ⛔ 不是删断言 —— 删了用例就恒真（`load-bearing` 的前提）。
    struct StaleOwnerSet {
        std::uint32_t owner = 0;        // owner 纹理的对象 EA（之后会被释放）
        std::uint32_t owner_gen = 0;    // 它的代际（建 surface 时固化进 surface 的那一份）
        std::uint32_t owner_state = 0;  // 它的 `'TEX1'` 状态块 EA
        std::uint32_t owner_arena = 0;  // 它的像素块（SurfaceArena）
        std::uint32_t dummy = 0;        // 撑开复用窗口的哑纹理
        std::uint32_t surface = 0;      // owner 的 level-0 表面（owner = 上面的 EA）
    };
    auto prepare = [&](StaleOwnerSet* s) -> bool {
        LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &s->owner), 0u);
        LIFT_CHECK_NE_U32(s->owner, 0u);
        if (s->owner == 0) return false;
        lift::d3d9::TextureInfo ti;
        LIFT_CHECK(lift::d3d9::textureInfo(r.f.m.cpu(), s->owner, &ti, nullptr));
        s->owner_state = ti.state;
        s->owner_arena = ti.arena;
        s->owner_gen = com->generationOf(s->owner);
        LIFT_CHECK_NE_U32(s->owner_gen, 0u);
        LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &s->dummy), 0u);
        LIFT_CHECK_NE_U32(s->dummy, 0u);
        const std::uint32_t out = r.f.placeU32(0u);
        LIFT_CHECK_EQ_U32(r.f.method(s->owner, kOffTextureGetSurfaceLevel, {0u, out}), 0u);
        s->surface = mem.rd32(out);
        LIFT_CHECK_NE_U32(s->surface, 0u);
        // surface 记下的是"我代表谁"，而且**不持有**它（`GetSurfaceLevel` 有意不替纹理持引用）
        LIFT_CHECK_EQ_U32(com->ownerOf(s->surface), s->owner);
        LIFT_CHECK_EQ_U32(com->ownerGenerationOf(s->surface), s->owner_gen);
        return s->dummy != 0 && s->surface != 0;
    };
    // 放掉哑纹理 + 对 owner 做 `releases` 次 Release（1 = 合法；2 = **越界**，把绑定中的那份也放掉）。
    // ⚠ 各次 Release 的返回值不看（第一次是 rc-1 的返回值、最后一次才是 0）：判据是"对象真的没了"。
    auto retire = [&](const StaleOwnerSet& s, std::uint32_t releases) {
        LIFT_CHECK_EQ_U32(r.f.method(s.dummy, kOffRelease, {}), 0u);
        for (std::uint32_t i = 0; i < releases; ++i) r.f.method(s.owner, kOffRelease, {});
        LIFT_CHECK(!com->find(s.owner));  // owner 真死了（EA 可被复用）
    };
    // 建一张同尺寸纹理，并**硬断言**它落在 owner 的那一格（对象 EA + `'TEX1'` 状态块 EA）
    auto reuseOwnerSlot = [&](const StaleOwnerSet& s) -> std::uint32_t {
        std::uint32_t tex = 0;
        LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
        LIFT_CHECK_NE_U32(tex, 0u);
        LIFT_CHECK_EQ_U32(tex, s.owner);                    // ← 前提 1：对象 EA 复用
        LIFT_CHECK_EQ_U32(com->userOf(tex), s.owner_state);  // ← 前提 2：状态块 EA 也复用（旧判据就是被这格骗过）
        return tex;
    };

    // ---- ① **已知偏离**（不是真机语义；见上面那段 ⚠⚠）：owner 与哑纹理一起被 guest 放掉，
    //   新纹理拿到同一个 EA；随后绑 S 必须被拒。真机（A 方案）这里**应当成功** ——
    //   修 REQ-01M6ZZZ6PEVSFB93C6VJPKC04C 之后，本段的 INVALIDCALL 要按 04C 翻成 kOk。
    StaleOwnerSet a;
    if (!prepare(&a)) return;
    retire(a, /*releases=*/1);
    const std::uint32_t tex2 = reuseOwnerSlot(a);
    if (tex2 == 0) return;
    const std::uint32_t tex2_gen = com->generationOf(tex2);
    LIFT_CHECK_NE_U32(tex2_gen, a.owner_gen);  // 同一个 EA、不同代际
    LIFT_CHECK(r.f.m.surfaces().find(a.owner_arena) != nullptr);  // 像素块已被 T2 接手

    // 绑 S：owner 的 EA 现在是 T2，但 surface 记住的代际是旧 owner 的 ⇒ 必须拒绝。
    //   ⚠ 旧判据（`userOf(owner) != tex_state`）在这一格**会通过** ⇒ 静默把 RT 绑到 T2 的像素上，
    //   还会对 T2 调 `markTextureContentUnknown`（内容版本降级）。
    //   ⚠ 这里的 `kInvalidCall` 是**本线的已知偏离**（surface 不持父纹理）⇒ 修 04C（候选 A）后翻面；
    //   ⛔ 别为了让它一直绿去改断言方向（见函数头那段）。
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {a.surface, 0u}),
                      lift::d3d9::kInvalidCall);
    LIFT_CHECK(com->find(tex2));
    LIFT_CHECK_EQ_U32(com->refCount(tex2), 1u);  // ⛔ 绑被拒绝 ⇒ 没有 addRef 到 T2
    LIFT_CHECK_EQ_U32(com->generationOf(tex2), tex2_gen);
    lift::d3d9::PipelineInfo pi;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr));
    LIFT_CHECK_EQ_U32(pi.color_surface, 0u);  // 没绑上
    LIFT_CHECK_EQ_U32(pi.color_arena, 0u);
    LIFT_CHECK_EQ_U32(r.f.method(tex2, kOffRelease, {}), 0u);   // T2 可以正常用：释放自己
    LIFT_CHECK(!com->find(tex2));
    LIFT_CHECK_EQ_U32(r.f.method(a.surface, kOffRelease, {}), 0u);  // S 的 guest 引用

    // ---- ② 防御纵深（**不是**合法时序）：绑定中 owner 被越界释放 ⇒ 解绑时 `releaseIf` 拒绝 ----
    //   合法 guest 走不到这里：绑定期那份 addRef 让 owner 活到解绑（见 ⑩ ⑥）。
    StaleOwnerSet b;
    if (!prepare(&b)) return;
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {b.surface, 0u}), 0u);  // 这时代际匹配
    // ⚠ `refCount == 2` 锁的是 `REQ-01M6X4N7C0PAF6XT7EAC172V85` **有意选择**的 guest 可见差异
    //   （设备把 RT 引用算在纹理的公开 refcount 上）—— 不是引擎语义；那条单改了这里要跟着改。
    LIFT_CHECK_EQ_U32(com->refCount(b.owner), 2u);  // guest 的 + 设备的（owner 语义引用）
    retire(b, /*releases=*/2);  // 越界：连设备那份也放掉 ⇒ owner 死
    const std::uint32_t tex4 = reuseOwnerSlot(b);
    if (tex4 == 0) return;
    const std::uint32_t tex4_gen = com->generationOf(tex4);
    LIFT_CHECK_NE_U32(tex4_gen, b.owner_gen);

    // 解绑 ⇒ 流水线释放它那两份引用：owner 代际不符 ⇒ **拒绝**；surface 照常释放
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, 0u}), 0u);
    LIFT_CHECK(com->find(tex4));                 // ⛔ 新占有者还在
    LIFT_CHECK_EQ_U32(com->refCount(tex4), 1u);  // ⛔ 一个引用都没被减（陈旧持有者打不到它）
    LIFT_CHECK_EQ_U32(com->generationOf(tex4), tex4_gen);
    LIFT_CHECK(com->ifaceOf(tex4) != nullptr);
    LIFT_CHECK_EQ_U32(com->refCount(b.surface), 1u);  // surface 的那份引用正常放掉（guest 的那份还在）
    LIFT_CHECK(com->find(b.surface));

    // 清理
    LIFT_CHECK_EQ_U32(r.f.method(tex4, kOffRelease, {}), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(b.surface, kOffRelease, {}), 0u);
    LIFT_CHECK(!com->find(tex4));
    LIFT_CHECK(!com->find(b.surface));
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
        caseDrawStateSnapshot();
        caseClipPlanes();
        caseRenderTargetLifetime();
        caseStaleOwnerGeneration();
        return finish("d3d9-pipeline");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "DIAG uncaught std::exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "DIAG uncaught non-std exception\n");
        return 1;
    }
}
