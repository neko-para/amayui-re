// emulator-lift · M2 夹具：D3DX9 Sprite 簇（pure 档：不碰游戏 / LFS）
//
// 判据（对照任务单「M2 · D3DX9 Sprite 簇（D3DXCreateSprite → 第一帧）」）：
//   ① `D3DXCreateSprite(device, &ppSprite)` 返回 S_OK(0) + 非 0 的 guest 可见 ID3DXSprite；
//      `QueryInterface(IID_ID3DXSprite)` / `QueryInterface(IID_IUnknown)` 成功；`GetDevice` 返回**同一台**
//      device；`Release` 到 0 ⇒ 对象 + 状态块 + 批次数组全部回收（guest 堆用量回到建之前）。
//   ② `Begin`/`Draw`/`End` 的**像素级**语义：
//      · ALPHABLEND：目标像素 = (源 × Color.alpha + 目标 × (1-alpha))，按 8 位通道逐个算（精确值断言）；
//      · 无 ALPHABLEND：调制色**覆盖**（不混合）；
//      · 裁剪：四边形越过后缓冲边界时只写可见部分（越界不写 / 不崩）；
//      · `SetTransform` 真的作用在**Draw 时**生成的四角上（2×3 缩放 ⇒ 4×4 的源矩形变成 8×12 像素）；
//      · `Flush` 合成后**保持** Begin 状态（后续 Draw 仍能进同一批次），`End` 关闭批次；
//      · `End` 幂等：第二次 End（空批次）**逐字节不改**后缓冲（整块 arena 哈希相等）。
//   ③ 未实现的槽（+0x18 SetWorldViewRH / 纹理 +0x48 GetSurfaceLevel）⇒ **loud trap**
//      （`comUnsupported`：机器停 + 名字带 iface/偏移）。`Draw` 给一个**不是**本模块纹理的指针
//      ⇒ loud trap；`Draw(NULL,NULL)` 无尺寸来源 ⇒ trap。
//   ④ 反例：`Draw` 在 `Begin` 之外 / `Begin` 嵌套 ⇒ D3DERR_INVALIDCALL（⛔ 不静默吞掉）。
//   ⑤ 纹理簇（`D3DXCreateTexture` → IDirect3DTexture9）：GetLevelDesc 的 D3DSURFACE_DESC 字段
//      与 `.lst` 0x48AD2A/0x48AD2D 的读法一致；LockRect 给出 guest 指针；**纹理 × 调制色**
//      的像素级断言；源矩形越界裁剪；纹理在批次里 AddRef / 合成后对称 Release。
//   ⑥ D3DX math：Multiply/Scaling/Translation/RotationAxis/RotationYawPitchRoll/OrthoLH/
//      PerspectiveFovLH/LookAtLH/Vec4Transform/Vec3Normalize 的**手算值**断言。
//
// ★ 所有方法调用都从 **guest 代码** 经 vtable 调（`mov eax,[vtbl]; push …; call [eax+off]`），
//   测的是"guest 可见的对象"，不是宿主直接调函数。
// ★ 变异验证（本次实测过，见交付说明）：
//   · 把 End 的合成改成"只计数不写像素" ⇒ ② 全红；
//   · 调制色不乘（直接用白色 / 直接用 texel） ⇒ ② 与 ②b 的精确颜色断言红；
//   · ALPHABLEND 退化成覆盖 ⇒ ② 的半透明用例红；
//   · 变换不在 Draw 时应用 ⇒ ② 的 8×12 缩放用例红；
//   · 纹理源矩形不裁（越界采样） ⇒ ②b 的 (22,20)/(20,22) 断言红。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/d3d9.h"
#include "runtime/d3dx9_sprite.h"
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
constexpr std::uint32_t kProcD3DXCreateSprite = 0x7FF81020u;
constexpr std::uint32_t kProcD3DXCreateTexture = 0x7FF81040u;
constexpr std::uint32_t kProcD3DXMatrixMultiply = 0x7FF81060u;
constexpr std::uint32_t kProcD3DXMatrixScaling = 0x7FF81080u;
constexpr std::uint32_t kProcD3DXMatrixTranslation = 0x7FF810A0u;
constexpr std::uint32_t kProcD3DXVec4Transform = 0x7FF810C0u;
constexpr std::uint32_t kProcD3DXVec3Normalize = 0x7FF810E0u;
constexpr std::uint32_t kProcD3DXMatrixPerspectiveFovLH = 0x7FF81100u;
constexpr std::uint32_t kProcD3DXMatrixLookAtLH = 0x7FF81120u;
constexpr std::uint32_t kProcD3DXMatrixRotationAxis = 0x7FF81140u;
constexpr std::uint32_t kProcD3DXMatrixRotationYawPitchRoll = 0x7FF81160u;
constexpr std::uint32_t kProcD3DXMatrixOrthoLH = 0x7FF81180u;
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
constexpr std::uint32_t kFixtureHeapBytes = 0x00100000u;

// IID_IDirect3DTexture9 = {85C31227-3DE5-4F00-9B3A-F11AC38C18B5}
constexpr std::uint8_t kIidTexture[16] = {0x27, 0x12, 0xC3, 0x85, 0xE5, 0x3D, 0x00, 0x4F,
                                          0x9B, 0x3A, 0xF1, 0x1A, 0xC3, 0x8C, 0x18, 0xB5};

// IID_ID3DXSprite = {BA0B762D-7D28-43EC-B9DC-2F84443B0614}（与 shims/d3dx9.cpp 同一份常量）
constexpr std::uint8_t kIidSprite[16] = {0x2D, 0x76, 0x0B, 0xBA, 0x28, 0x7D, 0xEC, 0x43,
                                         0xB9, 0xDC, 0x2F, 0x84, 0x44, 0x3B, 0x06, 0x14};
// IID_IUnknown = {00000000-0000-0000-C000-000000000046}
constexpr std::uint8_t kIidUnknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};

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
        if (!fixStub("d3dx9_43", "D3DXMatrixMultiply", kProcD3DXMatrixMultiply)) return false;
        if (!fixStub("d3dx9_43", "D3DXMatrixScaling", kProcD3DXMatrixScaling)) return false;
        if (!fixStub("d3dx9_43", "D3DXMatrixTranslation", kProcD3DXMatrixTranslation)) return false;
        if (!fixStub("d3dx9_43", "D3DXVec4Transform", kProcD3DXVec4Transform)) return false;
        if (!fixStub("d3dx9_43", "D3DXVec3Normalize", kProcD3DXVec3Normalize)) return false;
        if (!fixStub("d3dx9_43", "D3DXMatrixPerspectiveFovLH", kProcD3DXMatrixPerspectiveFovLH)) return false;
        if (!fixStub("d3dx9_43", "D3DXMatrixLookAtLH", kProcD3DXMatrixLookAtLH)) return false;
        if (!fixStub("d3dx9_43", "D3DXMatrixRotationAxis", kProcD3DXMatrixRotationAxis)) return false;
        if (!fixStub("d3dx9_43", "D3DXMatrixRotationYawPitchRoll", kProcD3DXMatrixRotationYawPitchRoll))
            return false;
        if (!fixStub("d3dx9_43", "D3DXMatrixOrthoLH", kProcD3DXMatrixOrthoLH)) return false;
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
    std::uint32_t placeRect(std::int32_t l, std::int32_t t, std::int32_t r, std::int32_t b) {
        const std::int32_t v[4] = {l, t, r, b};
        return placeData(v, sizeof(v));
    }
    std::uint32_t placeVec3(float x, float y, float z) {
        const float v[3] = {x, y, z};
        return placeData(v, sizeof(v));
    }
    std::uint32_t placeMatrix(const float m16[16]) { return placeData(m16, 16u * sizeof(float)); }

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
        // ★ 相对调用的 base 必须是**这次真正放置的** EA（code_ea），不是常量 kCodeBase：
        //   夹具每段代码隔 0x1000，用错 base 会把相对位移算错 0x1000 的整数倍。
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
        const std::uint32_t hr =
            method(d3d9_obj, 0x40u, {0u, 1u, 0x00001234u, 0x44u, pp_ea, out_ea});
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

    // D3DXCreateSprite(device, &out)
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

    // 任意 D3DX 导出（stdcall）：按 args 顺序压栈
    std::uint32_t callExport(std::uint32_t proc, const std::vector<std::uint32_t>& args) {
        Asm a;
        for (std::size_t i = args.size(); i > 0; --i) a.pushImm(args[i - 1]);
        a.callRel(code_ea, proc);
        a.ret();
        return runAsm(a);
    }

    // D3DXCreateTexture(device, w, h, levels, usage, format, pool, &out)
    std::uint32_t createTexture(std::uint32_t device, std::uint32_t w, std::uint32_t h,
                                std::uint32_t levels, std::uint32_t usage, std::uint32_t format,
                                std::uint32_t pool, std::uint32_t* out_obj) {
        const std::uint32_t out_ea = placeU32(0xDEADBEEFu);
        const std::uint32_t hr =
            callExport(kProcD3DXCreateTexture, {device, w, h, levels, usage, format, pool, out_ea});
        if (out_obj) *out_obj = m.mem().rd32(out_ea);
        return hr;
    }

    std::uint32_t pixelAt(const lift::d3d9::DeviceInfo& d, int x, int y) {
        return m.mem().rd32(d.arena + static_cast<std::uint32_t>(y) * d.pitch +
                            static_cast<std::uint32_t>(x) * 4u);
    }
    std::uint64_t surfaceHash(const lift::d3d9::DeviceInfo& d) {
        const lift::GuestSurface* gs = m.surfaces().find(d.arena);
        return gs ? m.surfaces().hash(*gs) : 0u;
    }
    std::uint32_t clearTo(const Device& dev, std::uint32_t argb) {
        return method(dev.object, 0xACu, {0u, 0u, 1u, argb, 0x3F800000u, 0u});
    }
};

// 一次"建 d3d9 + device + sprite"的公共前半段
struct Rig {
    Fixture f;
    std::uint32_t d3d9 = 0;
    Fixture::Device dev;
    std::uint32_t sprite = 0;
    bool up() {
        if (!f.boot()) return false;
        d3d9 = f.createDirect3D();
        if (d3d9 == 0) return false;
        if (!f.createDevice(d3d9, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev)) return false;
        return dev.object != 0;
    }
    bool withSprite() {
        if (!up()) return false;
        return f.createSprite(dev.object, &sprite) == 0u && sprite != 0;
    }
    std::uint32_t begin(std::uint32_t flags) { return f.method(sprite, 0x20u, {flags}); }
    std::uint32_t draw(std::uint32_t rect_ea, std::uint32_t center_ea, std::uint32_t pos_ea,
                       std::uint32_t color) {
        return f.method(sprite, 0x24u, {0u, rect_ea, center_ea, pos_ea, color});
    }
    std::uint32_t drawTex(std::uint32_t tex, std::uint32_t rect_ea, std::uint32_t center_ea,
                          std::uint32_t pos_ea, std::uint32_t color) {
        return f.method(sprite, 0x24u, {tex, rect_ea, center_ea, pos_ea, color});
    }
    std::uint32_t end() { return f.method(sprite, 0x2Cu, {}); }
};

// ---------------------------------------------------------------------------------------
// ① CreateSprite + QI + GetDevice + 变换 + Release 到 0
// ---------------------------------------------------------------------------------------
void caseCreateQueryRelease() {
    lift_test::g_current_case = "d3dx9/create+qi+getdevice+release";
    Rig r;
    LIFT_CHECK(r.up());
    if (!r.up()) return;
    const std::uint32_t heap_before = r.f.m.heap().bytesInUse();
    const std::uint32_t sprite = [&] {
        std::uint32_t obj = 0;
        const std::uint32_t hr = r.f.createSprite(r.dev.object, &obj);
        LIFT_CHECK_EQ_U32(hr, 0u);
        return obj;
    }();
    LIFT_CHECK_NE_U32(sprite, 0u);
    if (sprite == 0) return;
    LIFT_CHECK(r.f.m.heap().bytesInUse() > heap_before);
    // guest 看得见：[0] = vtable，[4] = refcount
    LIFT_CHECK_NE_U32(r.f.m.mem().rd32(sprite), 0u);
    LIFT_CHECK_EQ_U32(r.f.m.cpu().com->refCount(sprite), 1u);
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(sprite + 4u), 1u);
    const std::uint32_t state = r.f.m.cpu().com->userOf(sprite);
    LIFT_CHECK_NE_U32(state, 0u);
    lift::d3dx9::SpriteInfo info;
    LIFT_CHECK(lift::d3dx9::spriteInfo(r.f.m.cpu(), sprite, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.device, r.dev.object);

    // QI(IID_ID3DXSprite) / QI(IID_IUnknown) 成功；未知 IID ⇒ E_NOINTERFACE 且 ppv 清 0
    const std::uint32_t iid_ea = r.f.placeBytes(std::vector<std::uint8_t>(kIidSprite, kIidSprite + 16));
    const std::uint32_t unk_ea = r.f.placeBytes(std::vector<std::uint8_t>(kIidUnknown, kIidUnknown + 16));
    const std::uint32_t ppv = r.f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(r.f.method(sprite, 0x00u, {iid_ea, ppv}), 0u);
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(ppv), sprite);
    LIFT_CHECK_EQ_U32(r.f.m.cpu().com->refCount(sprite), 2u);
    const std::uint32_t ppv2 = r.f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(r.f.method(sprite, 0x00u, {unk_ea, ppv2}), 0u);
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(ppv2), sprite);
    LIFT_CHECK_EQ_U32(r.f.m.cpu().com->refCount(sprite), 3u);
    std::uint8_t bogus[16] = {0};
    bogus[0] = 0x42;
    const std::uint32_t bogus_ea = r.f.placeBytes(std::vector<std::uint8_t>(bogus, bogus + 16));
    const std::uint32_t ppv3 = r.f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(r.f.method(sprite, 0x00u, {bogus_ea, ppv3}), lift::d3dx9::kNoInterface);
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(ppv3), 0u);

    // GetDevice(+0x0C) 返回**同一台** device（且不 AddRef）
    const std::uint32_t dev_out = r.f.placeU32(0u);
    LIFT_CHECK_EQ_U32(r.f.method(sprite, 0x0Cu, {dev_out}), 0u);
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(dev_out), r.dev.object);
    LIFT_CHECK_EQ_U32(r.f.m.cpu().com->refCount(r.dev.object), 1u);

    // GetTransform 默认单位阵；SetTransform 真的改；GetTransform 读回来一致
    const std::uint32_t mat_out = r.f.placeBytes(std::vector<std::uint8_t>(64, 0xEE));
    LIFT_CHECK_EQ_U32(r.f.method(sprite, 0x10u, {mat_out}), 0u);
    for (int i = 0; i < 16; ++i) {
        const std::uint32_t bits = r.f.m.mem().rd32(mat_out + static_cast<std::uint32_t>(i) * 4u);
        float v = 0.0f;
        std::memcpy(&v, &bits, 4);
        LIFT_CHECK(v == (i % 5 == 0 ? 1.0f : 0.0f));
    }
    float mat[16] = {0};
    mat[0] = 2.0f;
    mat[5] = 3.0f;
    mat[10] = 1.0f;
    mat[15] = 1.0f;
    mat[12] = 7.0f;
    const std::uint32_t mat_ea = r.f.placeMatrix(mat);
    LIFT_CHECK_EQ_U32(r.f.method(sprite, 0x14u, {mat_ea}), 0u);
    LIFT_CHECK(lift::d3dx9::spriteInfo(r.f.m.cpu(), sprite, &info, nullptr));
    LIFT_CHECK(info.matrix[0] == 2.0f);
    LIFT_CHECK(info.matrix[5] == 3.0f);
    LIFT_CHECK(info.matrix[12] == 7.0f);
    const std::uint32_t mat_out2 = r.f.placeBytes(std::vector<std::uint8_t>(64, 0xEE));
    LIFT_CHECK_EQ_U32(r.f.method(sprite, 0x10u, {mat_out2}), 0u);
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(mat_out2 + 0u), r.f.m.mem().rd32(mat_ea + 0u));
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(mat_out2 + 48u), r.f.m.mem().rd32(mat_ea + 48u));
    // SetTransform(NULL) ⇒ 单位阵（D3DX 语义）
    LIFT_CHECK_EQ_U32(r.f.method(sprite, 0x14u, {0u}), 0u);
    LIFT_CHECK(lift::d3dx9::spriteInfo(r.f.m.cpu(), sprite, &info, nullptr));
    LIFT_CHECK(info.matrix[0] == 1.0f && info.matrix[12] == 0.0f);

    // OnLostDevice / OnResetDevice 记录（headless 没有真设备丢失）
    LIFT_CHECK_EQ_U32(r.f.method(sprite, 0x30u, {}), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(sprite, 0x34u, {}), 0u);
    LIFT_CHECK(lift::d3dx9::spriteInfo(r.f.m.cpu(), sprite, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.lost, 1u);
    LIFT_CHECK_EQ_U32(info.resets, 1u);

    // Release：3 → 2 → 1 → 0；到 0 时对象 + 状态块 + 批次全部回收
    LIFT_CHECK_EQ_U32(r.f.method(sprite, 0x08u, {}), 2u);
    LIFT_CHECK_EQ_U32(r.f.method(sprite, 0x08u, {}), 1u);
    LIFT_CHECK_EQ_U32(r.f.method(sprite, 0x08u, {}), 0u);
    LIFT_CHECK(!r.f.m.cpu().com->find(sprite));
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(state + 0u), 0u);  // 状态块 magic 被抹
    LIFT_CHECK_EQ_U32(r.f.m.heap().bytesInUse(), heap_before);  // 没有泄漏
}

// ---------------------------------------------------------------------------------------
// ② Begin / Draw / End 的像素语义
// ---------------------------------------------------------------------------------------
void caseDrawEndPixels() {
    lift_test::g_current_case = "d3dx9/draw+end-pixels";
    Rig r;
    LIFT_CHECK(r.withSprite());
    if (r.sprite == 0) return;
    const lift::d3d9::DeviceInfo& d = r.dev.info;
    // 基准背景：0x004080C0（r=64 g=128 b=192；X 格式 ⇒ alpha 无意义）
    LIFT_CHECK_EQ_U32(r.f.clearTo(r.dev, 0x004080C0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 0, 0), 0x004080C0u);

    const std::uint32_t src4 = r.f.placeRect(0, 0, 4, 4);
    const std::uint32_t zero3 = r.f.placeVec3(0.0f, 0.0f, 0.0f);

    // ---- ALPHABLEND：0x80FF0000（半透明纯红）叠在 0x004080C0 上 ----
    LIFT_CHECK_EQ_U32(r.begin(lift::d3dx9::kSpriteAlphaBlend), 0u);
    const std::uint32_t pos = r.f.placeVec3(10.0f, 20.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.draw(src4, zero3, pos, 0x80FF0000u), 0u);
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    // 期望：r=(255*128+64*127+127)/255=160(0xA0)；g=(0*128+128*127+127)/255=64(0x40)；
    //       b=(0*128+192*127+127)/255=96(0x60)。4×4 全部命中，右/下开区间。
    for (int y = 20; y < 24; ++y)
        for (int x = 10; x < 14; ++x) LIFT_CHECK_EQ_U32(r.f.pixelAt(d, x, y), 0x00A04060u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 9, 20), 0x004080C0u);   // 左边界外
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 14, 20), 0x004080C0u);  // 右边界开区间
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 10, 24), 0x004080C0u);  // 下边界开区间

    // ---- 无 ALPHABLEND：调制色**覆盖**（X8R8G8B8 丢掉 alpha） ----
    LIFT_CHECK_EQ_U32(r.begin(0u), 0u);
    const std::uint32_t pos2 = r.f.placeVec3(20.0f, 20.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.draw(src4, zero3, pos2, 0xFF00FF00u), 0u);
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 20, 20), 0x0000FF00u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 23, 23), 0x0000FF00u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 24, 20), 0x004080C0u);

    // ---- 裁剪：四边形 (-2,-2)..(2,2)，只该写 (0,0)..(1,1) ----
    LIFT_CHECK_EQ_U32(r.begin(0u), 0u);
    const std::uint32_t pos3 = r.f.placeVec3(-2.0f, -2.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.draw(src4, zero3, pos3, 0xFF0000FFu), 0u);
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 0, 0), 0x000000FFu);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 1, 1), 0x000000FFu);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 2, 2), 0x004080C0u);  // 四边形外（u=4 已经越界）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 2, 0), 0x004080C0u);

    // ---- 变换：SetTransform(缩放 2×3 + 平移 100,50) ⇒ 4×4 的源矩形变成 8×12 像素 ----
    // ★ 注意 D3DX 的语义：变换作用于**顶点**，所以 position 也在矩阵里（这里 position=0，
    //   平移由矩阵的 m[12]/m[13] 给）。⛔ 不是"只缩放尺寸、位置照搬"。
    float scale[16] = {0};
    scale[0] = 2.0f;
    scale[5] = 3.0f;
    scale[10] = 1.0f;
    scale[15] = 1.0f;
    scale[12] = 100.0f;
    scale[13] = 50.0f;
    const std::uint32_t scale_ea = r.f.placeMatrix(scale);
    LIFT_CHECK_EQ_U32(r.f.method(r.sprite, 0x14u, {scale_ea}), 0u);
    LIFT_CHECK_EQ_U32(r.begin(0u), 0u);
    const std::uint32_t pos4 = r.f.placeVec3(0.0f, 0.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.draw(src4, zero3, pos4, 0xFF00FFFFu), 0u);
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 100, 50), 0x0000FFFFu);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 107, 61), 0x0000FFFFu);  // 8×12 的右下角（开区间内）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 108, 50), 0x004080C0u);  // 右边越界
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 100, 62), 0x004080C0u);  // 下边越界
    LIFT_CHECK_EQ_U32(r.f.method(r.sprite, 0x14u, {0u}), 0u);  // 恢复单位阵

    // ---- Flush：合成但**保持** Begin；后续 Draw 仍进同一批次 ----
    LIFT_CHECK_EQ_U32(r.begin(lift::d3dx9::kSpriteAlphaBlend), 0u);
    const std::uint32_t pos5 = r.f.placeVec3(200.0f, 10.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.draw(src4, zero3, pos5, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(r.sprite, 0x28u, {}), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 200, 10), 0x00FFFFFFu);
    lift::d3dx9::SpriteInfo info;
    LIFT_CHECK(lift::d3dx9::spriteInfo(r.f.m.cpu(), r.sprite, &info, nullptr));
    LIFT_CHECK(info.drawing);
    LIFT_CHECK_EQ_U32(info.batch_count, 0u);
    const std::uint32_t pos6 = r.f.placeVec3(210.0f, 10.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.draw(src4, zero3, pos6, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 210, 10), 0x00FFFFFFu);
    LIFT_CHECK(lift::d3dx9::spriteInfo(r.f.m.cpu(), r.sprite, &info, nullptr));
    LIFT_CHECK(!info.drawing);
    LIFT_CHECK_EQ_U32(info.flushes, 1u);
    LIFT_CHECK_EQ_U32(info.ends, 5u);

    // ---- End 幂等：第二次 End 逐字节不改后缓冲 ----
    const std::uint64_t before = r.f.surfaceHash(d);
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    LIFT_CHECK(r.f.surfaceHash(d) == before);

    // ---- 空 src rect ⇒ S_OK 但什么也不画（真机语义） ----
    const std::uint32_t empty = r.f.placeRect(5, 5, 5, 9);
    LIFT_CHECK_EQ_U32(r.begin(0u), 0u);
    LIFT_CHECK_EQ_U32(r.draw(empty, 0u, 0u, 0xFFFF0000u), 0u);
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    LIFT_CHECK(r.f.surfaceHash(d) == before);

    // ---- 反例：Draw 在 Begin 之外 / Begin 嵌套 ⇒ D3DERR_INVALIDCALL ----
    LIFT_CHECK_EQ_U32(r.draw(src4, zero3, zero3, 0xFFFFFFFFu), lift::d3dx9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.begin(0u), 0u);
    LIFT_CHECK_EQ_U32(r.begin(0u), lift::d3dx9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.end(), 0u);

    // ---- 16bpp 后缓冲：合成按 R5G6B5 换算（复用同一台设备簇的第二种格式） ----
    Fixture::Device dev16;
    LIFT_CHECK(r.f.createDevice(r.d3d9, 640, 480, lift::d3d9::kFmtR5G6B5, &dev16));
    if (dev16.object != 0) {
        std::uint32_t sprite16 = 0;
        LIFT_CHECK_EQ_U32(r.f.createSprite(dev16.object, &sprite16), 0u);
        if (sprite16 != 0) {
            LIFT_CHECK_EQ_U32(r.f.method(dev16.object, 0xACu, {0u, 0u, 1u, 0x00000000u, 0x3F800000u, 0u}), 0u);
            LIFT_CHECK_EQ_U32(r.f.method(sprite16, 0x20u, {0u}), 0u);
            const std::uint32_t pos = r.f.placeVec3(3.0f, 4.0f, 0.0f);
            LIFT_CHECK_EQ_U32(r.f.method(sprite16, 0x24u, {0u, src4, zero3, pos, 0x00F800F8u}), 0u);
            LIFT_CHECK_EQ_U32(r.f.method(sprite16, 0x2Cu, {}), 0u);
            const std::uint32_t px = r.f.m.mem().rd16(dev16.info.arena + 4u * dev16.info.pitch + 3u * 2u);
            // 品红 0xF800F8 落在 R5G6B5 的 R=31 / G=0 / B=31 ⇒ 0xF81F
            LIFT_CHECK_EQ_U32(px, 0xF81Fu);
        }
    }
}

// ---------------------------------------------------------------------------------------
// ②b 纹理簇：D3DXCreateTexture + GetLevelDesc + LockRect/UnlockRect + 纹理 × 调制色
// ---------------------------------------------------------------------------------------
void caseTextureDraw() {
    lift_test::g_current_case = "d3dx9/texture+draw";
    Rig r;
    LIFT_CHECK(r.withSprite());
    if (r.sprite == 0) return;
    const lift::d3d9::DeviceInfo& d = r.dev.info;
    LIFT_CHECK_EQ_U32(r.f.clearTo(r.dev, 0x004080C0u), 0u);

    const std::size_t surfaces_before = r.f.m.surfaces().surfaceCount();
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.dev.object, 4, 4, 1, 0, lift::d3d9::kFmtA8R8G8B8, 1, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    LIFT_CHECK_EQ_U32(r.f.m.surfaces().surfaceCount(), surfaces_before + 1);
    LIFT_CHECK_EQ_U32(r.f.m.cpu().com->refCount(tex), 1u);
    lift::d3d9::TextureInfo ti;
    LIFT_CHECK(lift::d3d9::textureInfo(r.f.m.cpu(), tex, &ti, nullptr));
    LIFT_CHECK_EQ_U32(ti.width, 4u);
    LIFT_CHECK_EQ_U32(ti.height, 4u);
    LIFT_CHECK_EQ_U32(ti.format, lift::d3d9::kFmtA8R8G8B8);
    LIFT_CHECK_EQ_U32(ti.pitch, 16u);
    LIFT_CHECK_EQ_U32(ti.levels, 1u);

    // QI(IID_IDirect3DTexture9)
    const std::uint32_t iid_ea = r.f.placeBytes(std::vector<std::uint8_t>(kIidTexture, kIidTexture + 16));
    const std::uint32_t ppv = r.f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x00u, {iid_ea, ppv}), 0u);
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(ppv), tex);
    LIFT_CHECK_EQ_U32(r.f.m.cpu().com->refCount(tex), 2u);

    // GetLevelDesc(+0x44)：D3DSURFACE_DESC 32 B（Format+0 / Type+4 / Usage+8 / Pool+0xC /
    // Width+0x18 / Height+0x1C —— 与 .lst 0x48AD2A/0x48AD2D 的读法一致）
    const std::uint32_t desc = r.f.placeBytes(std::vector<std::uint8_t>(32, 0xEE));
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x44u, {0u, desc}), 0u);
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(desc + 0x00), lift::d3d9::kFmtA8R8G8B8);
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(desc + 0x04), lift::d3d9::kRTypeSurface);
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(desc + 0x08), 0u);   // Usage
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(desc + 0x0C), 1u);   // Pool = MANAGED
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(desc + 0x18), 4u);   // Width
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(desc + 0x1C), 4u);   // Height
    // 越界 level ⇒ 失败（不是静默写 0）
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(desc + 0x18), 4u);

    // LockRect(+0x4C) / UnlockRect(+0x50)：拿 guest 指针写纹理像素
    const std::uint32_t lr = r.f.placeBytes(std::vector<std::uint8_t>(8, 0xEE));
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x4Cu, {0u, lr, 0u, 0u}), 0u);
    const std::uint32_t pitch = r.f.m.mem().rd32(lr + 0);
    const std::uint32_t bits = r.f.m.mem().rd32(lr + 4);
    LIFT_CHECK_EQ_U32(pitch, 16u);
    LIFT_CHECK_NE_U32(bits, 0u);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            std::uint32_t v = 0xFF000000u;                  // 其余 texel：不透明黑
            if (x == 0 && y == 0) v = 0xFF804020u;          // a=255 r=0x80 g=0x40 b=0x20
            if (x == 1 && y == 0) v = 0x80FF0000u;          // a=0x80 纯红（半透明）
            r.f.m.mem().wr32(bits + static_cast<std::uint32_t>(y) * pitch +
                                 static_cast<std::uint32_t>(x) * 4u,
                             v);
        }
    }
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x50u, {0u}), 0u);

    // ---- 整张纹理（pSrcRect=NULL）+ ALPHABLEND + 白色调制 ----
    LIFT_CHECK_EQ_U32(r.begin(lift::d3dx9::kSpriteAlphaBlend), 0u);
    const std::uint32_t pos0 = r.f.placeVec3(0.0f, 0.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.drawTex(tex, 0u, 0u, pos0, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 0, 0), 0x00804020u);  // 不透明 texel 原样
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 1, 0), 0x00A04060u);  // 半透明红叠在 0x004080C0 上
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 2, 0), 0x00000000u);  // 不透明黑
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 3, 3), 0x00000000u);
    // 批次里 AddRef 的纹理已对称 Release
    LIFT_CHECK_EQ_U32(r.f.m.cpu().com->refCount(tex), 2u);

    // ---- 纹理 × 调制色（无 ALPHABLEND）：texel(0x80,0x40,0x20) × Color(0x80,0x80,0x80) ----
    LIFT_CHECK_EQ_U32(r.begin(0u), 0u);
    const std::uint32_t src1 = r.f.placeRect(0, 0, 1, 1);
    const std::uint32_t pos1 = r.f.placeVec3(10.0f, 10.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.drawTex(tex, src1, 0u, pos1, 0x80808080u), 0u);
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    // mul255(0x80,0x80)=64(0x40)；mul255(0x40,0x80)=32(0x20)；mul255(0x20,0x80)=16(0x10)
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 10, 10), 0x00402010u);

    // ---- 源矩形越界裁剪：src=(2,2,6,6) 在 4×4 纹理上只剩 (2,2)-(4,4) 那一截 ----
    LIFT_CHECK_EQ_U32(r.begin(0u), 0u);
    const std::uint32_t src2 = r.f.placeRect(2, 2, 6, 6);
    const std::uint32_t pos2 = r.f.placeVec3(20.0f, 20.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.drawTex(tex, src2, 0u, pos2, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 20, 20), 0x00000000u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 21, 21), 0x00000000u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 22, 20), 0x004080C0u);  // 被裁掉（texel 越界）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 20, 22), 0x004080C0u);

    // ---- 无 ALPHABLEND：X8R8G8B8 目标把 alpha 丢掉，颜色原样 ----
    LIFT_CHECK_EQ_U32(r.begin(0u), 0u);
    const std::uint32_t pos3 = r.f.placeVec3(30.0f, 30.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.drawTex(tex, 0u, 0u, pos3, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 30, 30), 0x00804020u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 31, 30), 0x00FF0000u);

    // ---- 未实现的槽（+0x48 GetSurfaceLevel）⇒ loud trap ----
    const std::uint32_t vtbl = r.f.m.mem().rd32(tex);
    const std::uint32_t stub = r.f.m.mem().rd32(vtbl + 0x48u);
    const lift::ShimDef* def = r.f.m.shims().byStubEa(stub);
    LIFT_CHECK(def != nullptr);
    if (def) {
        LIFT_CHECK(def->name.find("IDirect3DTexture9") != std::string::npos);
        LIFT_CHECK(def->name.find("+0x48") != std::string::npos);
        LIFT_CHECK(def->fn == &lift::comUnsupported);
    }

    // Release 到 0（refcount 2 ⇒ 两次）⇒ 像素块一起回收
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 1u);
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 0u);
    LIFT_CHECK(!r.f.m.cpu().com->find(tex));
    LIFT_CHECK_EQ_U32(r.f.m.surfaces().surfaceCount(), surfaces_before);
}

// ---------------------------------------------------------------------------------------
// ②c D3DX math（本线实现的那几条；行向量约定）
// ---------------------------------------------------------------------------------------
void caseMatrix() {
    lift_test::g_current_case = "d3dx9/matrix";
    Fixture f;
    LIFT_CHECK(f.boot());
    auto bits = [](float v) {
        std::uint32_t b = 0;
        std::memcpy(&b, &v, 4);
        return b;
    };
    auto rd = [&f](std::uint32_t ea, int i) {
        const std::uint32_t b = f.m.mem().rd32(ea + static_cast<std::uint32_t>(i) * 4u);
        float v = 0.0f;
        std::memcpy(&v, &b, 4);
        return v;
    };
    float a[16] = {0};
    a[0] = 2.0f;
    a[5] = 3.0f;
    a[10] = 4.0f;
    a[15] = 1.0f;
    const std::uint32_t a_ea = f.placeMatrix(a);
    float out[16] = {0};
    const std::uint32_t out_ea = f.placeMatrix(out);
    // Scaling(2,3,4)
    LIFT_CHECK_EQ_U32(f.callExport(kProcD3DXMatrixScaling,
                                   {out_ea, bits(2.0f), bits(3.0f), bits(4.0f)}),
                      out_ea);
    LIFT_CHECK(rd(out_ea, 0) == 2.0f && rd(out_ea, 5) == 3.0f && rd(out_ea, 10) == 4.0f &&
               rd(out_ea, 15) == 1.0f);
    LIFT_CHECK(rd(out_ea, 1) == 0.0f && rd(out_ea, 4) == 0.0f);
    // Translation(10,20,30)
    const std::uint32_t tr_ea = f.placeMatrix(out);
    LIFT_CHECK_EQ_U32(f.callExport(kProcD3DXMatrixTranslation,
                                   {tr_ea, bits(10.0f), bits(20.0f), bits(30.0f)}),
                      tr_ea);
    LIFT_CHECK(rd(tr_ea, 12) == 10.0f && rd(tr_ea, 13) == 20.0f && rd(tr_ea, 14) == 30.0f);
    LIFT_CHECK(rd(tr_ea, 0) == 1.0f && rd(tr_ea, 15) == 1.0f);
    // Multiply(S, T) = S*T（行向量：先 S 后 T）⇒ 对角 2/3/4 + 平移行 (10,20,30)
    const std::uint32_t prod_ea = f.placeMatrix(out);
    LIFT_CHECK_EQ_U32(f.callExport(kProcD3DXMatrixMultiply, {prod_ea, a_ea, tr_ea}), prod_ea);
    LIFT_CHECK(rd(prod_ea, 0) == 2.0f && rd(prod_ea, 5) == 3.0f && rd(prod_ea, 10) == 4.0f);
    LIFT_CHECK(rd(prod_ea, 12) == 10.0f && rd(prod_ea, 13) == 20.0f && rd(prod_ea, 14) == 30.0f);
    LIFT_CHECK(rd(prod_ea, 15) == 1.0f);
    // Vec4Transform：v=(1,2,3,1) 过 Translation(10,20,30) ⇒ (11,22,33,1)
    float v[4] = {1.0f, 2.0f, 3.0f, 1.0f};
    const std::uint32_t v_ea = f.placeData(v, sizeof(v));
    const std::uint32_t vo_ea = f.placeData(v, sizeof(v));
    LIFT_CHECK_EQ_U32(f.callExport(kProcD3DXVec4Transform, {vo_ea, v_ea, tr_ea}), vo_ea);
    LIFT_CHECK(rd(vo_ea, 0) == 11.0f && rd(vo_ea, 1) == 22.0f && rd(vo_ea, 2) == 33.0f);
    LIFT_CHECK(rd(vo_ea, 3) == 1.0f);
    // Vec3Normalize：(3,4,0) ⇒ (0.6,0.8,0)，返回 5.0f
    float n[3] = {3.0f, 4.0f, 0.0f};
    const std::uint32_t n_ea = f.placeData(n, sizeof(n));
    const std::uint32_t no_ea = f.placeData(n, sizeof(n));
    const std::uint32_t ret = f.callExport(kProcD3DXVec3Normalize, {no_ea, n_ea});
    LIFT_CHECK_EQ_U32(ret, bits(5.0f));
    LIFT_CHECK(std::fabs(rd(no_ea, 0) - 0.6f) < 1e-6f);
    LIFT_CHECK(std::fabs(rd(no_ea, 1) - 0.8f) < 1e-6f);
    LIFT_CHECK(std::fabs(rd(no_ea, 2) - 0.0f) < 1e-6f);

    // ---- 3D 相机 / 投影（左手；行向量）----
    // PerspectiveFovLH(fovy=1.0, aspect=2.0, zn=1, zf=1001)
    const std::uint32_t persp_ea = f.placeMatrix(out);
    LIFT_CHECK_EQ_U32(f.callExport(kProcD3DXMatrixPerspectiveFovLH,
                                   {persp_ea, bits(1.0f), bits(2.0f), bits(1.0f), bits(1001.0f)}),
                      persp_ea);
    {
        const double h = 1.0 / std::tan(0.5);
        LIFT_CHECK(std::fabs(rd(persp_ea, 5) - h) < 1e-5);
        LIFT_CHECK(std::fabs(rd(persp_ea, 0) - h / 2.0) < 1e-5);
        LIFT_CHECK(std::fabs(rd(persp_ea, 10) - 1001.0f / 1000.0f) < 1e-5);
        LIFT_CHECK(rd(persp_ea, 11) == 1.0f);
        LIFT_CHECK(std::fabs(rd(persp_ea, 14) - (-1001.0f / 1000.0f)) < 1e-5);
        LIFT_CHECK(rd(persp_ea, 15) == 0.0f);
    }
    // LookAtLH(eye=(0,0,-10), at=(0,0,0), up=(0,1,0)) ⇒ 单位旋转 + 平移 z=+10
    const std::uint32_t look_ea = f.placeMatrix(out);
    const std::uint32_t eye_ea = f.placeVec3(0.0f, 0.0f, -10.0f);
    const std::uint32_t at_ea = f.placeVec3(0.0f, 0.0f, 0.0f);
    const std::uint32_t up_ea = f.placeVec3(0.0f, 1.0f, 0.0f);
    LIFT_CHECK_EQ_U32(f.callExport(kProcD3DXMatrixLookAtLH, {look_ea, eye_ea, at_ea, up_ea}), look_ea);
    LIFT_CHECK(rd(look_ea, 0) == 1.0f && rd(look_ea, 5) == 1.0f && rd(look_ea, 10) == 1.0f);
    LIFT_CHECK(rd(look_ea, 14) == 10.0f);
    LIFT_CHECK(rd(look_ea, 12) == 0.0f && rd(look_ea, 13) == 0.0f);
    // RotationAxis((0,0,1), π/2) ⇒ [0,1,0; -1,0,0; 0,0,1]（与 D3DXMatrixRotationZ 同一形式）
    const std::uint32_t axis_ea = f.placeVec3(0.0f, 0.0f, 1.0f);
    const std::uint32_t rot_ea = f.placeMatrix(out);
    LIFT_CHECK_EQ_U32(f.callExport(kProcD3DXMatrixRotationAxis, {rot_ea, axis_ea, bits(3.14159265f / 2.0f)}),
                      rot_ea);
    LIFT_CHECK(std::fabs(rd(rot_ea, 0)) < 1e-5f);
    LIFT_CHECK(std::fabs(rd(rot_ea, 1) - 1.0f) < 1e-5f);
    LIFT_CHECK(std::fabs(rd(rot_ea, 4) + 1.0f) < 1e-5f);
    LIFT_CHECK(std::fabs(rd(rot_ea, 5)) < 1e-5f);
    // YawPitchRoll(0,0,0) = 单位阵
    const std::uint32_t ypr_ea = f.placeMatrix(out);
    LIFT_CHECK_EQ_U32(
        f.callExport(kProcD3DXMatrixRotationYawPitchRoll, {ypr_ea, bits(0.0f), bits(0.0f), bits(0.0f)}),
        ypr_ea);
    for (int i = 0; i < 16; ++i) LIFT_CHECK(rd(ypr_ea, i) == (i % 5 == 0 ? 1.0f : 0.0f));
    // OrthoLH(2,4,1,101) ⇒ 2/w=1, 2/h=0.5, 1/(zf-zn)=0.01, -zn/(zf-zn)=-0.01
    const std::uint32_t ortho_ea = f.placeMatrix(out);
    LIFT_CHECK_EQ_U32(f.callExport(kProcD3DXMatrixOrthoLH,
                                   {ortho_ea, bits(2.0f), bits(4.0f), bits(1.0f), bits(101.0f)}),
                      ortho_ea);
    LIFT_CHECK(rd(ortho_ea, 0) == 1.0f);
    LIFT_CHECK(rd(ortho_ea, 5) == 0.5f);
    LIFT_CHECK(std::fabs(rd(ortho_ea, 10) - 0.01f) < 1e-7f);
    LIFT_CHECK(std::fabs(rd(ortho_ea, 14) + 0.01f) < 1e-7f);
}

// ---------------------------------------------------------------------------------------
// ③ loud trap：未实现槽 / 非 0 纹理 / NULL 纹理 + NULL src rect
// ---------------------------------------------------------------------------------------
void caseTraps() {
    // (a) +0x18 SetWorldViewRH 是未实现槽 ⇒ comUnsupported
    {
        lift_test::g_current_case = "d3dx9/trap-slot";
        Rig r;
        LIFT_CHECK(r.withSprite());
        if (r.sprite == 0) return;
        const std::uint32_t vtbl = r.f.m.mem().rd32(r.sprite);
        const std::uint32_t stub = r.f.m.mem().rd32(vtbl + 0x18u);
        const lift::ShimDef* def = r.f.m.shims().byStubEa(stub);
        LIFT_CHECK(def != nullptr);
        if (def) {
            LIFT_CHECK(def->name.find("ID3DXSprite") != std::string::npos);
            LIFT_CHECK(def->name.find("+0x18") != std::string::npos);
            LIFT_CHECK(def->fn == &lift::comUnsupported);  // 没填 ⇒ loud trap
        }
        Asm a;
        a.movEaxImm(vtbl);
        a.pushImm(0u);  // pView
        a.pushImm(0u);  // pWorld
        a.pushImm(r.sprite);
        a.callMemEaxDisp8(0x18u);
        a.ret();
        lift::RunResult rr;
        r.f.runAsmRaw(a, &rr);
        LIFT_CHECK(rr.reason != lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(rr.reason),
                          static_cast<std::uint32_t>(lift::StopReason::Stopped));
        LIFT_CHECK(r.f.m.cpu().running == false);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.f.m.cpu().shim.unimplemented_calls), 1u);
        LIFT_CHECK(rr.detail.find("ID3DXSprite") != std::string::npos);
        LIFT_CHECK(rr.detail.find("+0x18") != std::string::npos);
    }
    // (b) Draw(pTexture != 0) ⇒ loud trap（纹理簇未实现）
    {
        lift_test::g_current_case = "d3dx9/trap-texture-draw";
        Rig r;
        LIFT_CHECK(r.withSprite());
        if (r.sprite == 0) return;
        LIFT_CHECK_EQ_U32(r.begin(lift::d3dx9::kSpriteAlphaBlend), 0u);
        const std::uint32_t src = r.f.placeRect(0, 0, 4, 4);
        Asm a;
        a.movEaxImm(r.f.m.mem().rd32(r.sprite));
        a.pushImm(0xFFFFFFFFu);       // Color
        a.pushImm(r.f.placeVec3(0, 0, 0));  // pPosition
        a.pushImm(r.f.placeVec3(0, 0, 0));  // pCenter
        a.pushImm(src);               // pSrcRect
        a.pushImm(0x12345678u);       // pTexture（不是我们的 IDirect3DTexture9）
        a.pushImm(r.sprite);
        a.callMemEaxDisp8(0x24u);
        a.ret();
        lift::RunResult rr;
        r.f.runAsmRaw(a, &rr);
        LIFT_CHECK(rr.reason != lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(rr.reason),
                          static_cast<std::uint32_t>(lift::StopReason::Stopped));
        LIFT_CHECK(r.f.m.cpu().running == false);
        LIFT_CHECK(std::string(r.f.m.cpu().stop_why ? r.f.m.cpu().stop_why : "") == "unimplemented D3DX9 call");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.f.m.cpu().shim.unimplemented_calls), 1u);
        LIFT_CHECK(rr.detail.find("ID3DXSprite::Draw") != std::string::npos);
    }
    // (c) Draw(pTexture=0, pSrcRect=NULL) ⇒ 没有尺寸来源 ⇒ loud trap
    {
        lift_test::g_current_case = "d3dx9/trap-null-size";
        Rig r;
        LIFT_CHECK(r.withSprite());
        if (r.sprite == 0) return;
        LIFT_CHECK_EQ_U32(r.begin(0u), 0u);
        Asm a;
        a.movEaxImm(r.f.m.mem().rd32(r.sprite));
        a.pushImm(0xFFFFFFFFu);
        a.pushImm(0u);
        a.pushImm(0u);
        a.pushImm(0u);
        a.pushImm(0u);
        a.pushImm(r.sprite);
        a.callMemEaxDisp8(0x24u);
        a.ret();
        lift::RunResult rr;
        r.f.runAsmRaw(a, &rr);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(rr.reason),
                          static_cast<std::uint32_t>(lift::StopReason::Stopped));
        LIFT_CHECK(std::string(r.f.m.cpu().stop_why ? r.f.m.cpu().stop_why : "") == "unimplemented D3DX9 call");
    }
    // (d) 非设备对象当 pDevice ⇒ loud trap（⛔ 不静默返回 S_OK）
    {
        lift_test::g_current_case = "d3dx9/trap-bad-device";
        Rig r;
        LIFT_CHECK(r.up());
        if (r.dev.object == 0) return;
        const std::uint32_t out_ea = r.f.placeU32(0xDEADBEEFu);
        Asm a;
        a.pushImm(out_ea);
        a.pushImm(0xDEADBEEFu);  // 不是 D3D 设备
        a.callRel(r.f.code_ea, kProcD3DXCreateSprite);
        a.ret();
        lift::RunResult rr;
        r.f.runAsmRaw(a, &rr);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(rr.reason),
                          static_cast<std::uint32_t>(lift::StopReason::Stopped));
        LIFT_CHECK(std::string(r.f.m.cpu().stop_why ? r.f.m.cpu().stop_why : "") == "unimplemented D3DX9 call");
        LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(out_ea), 0u);  // 失败时出参清 0
    }
}

}  // namespace

int main() {
    using namespace lift_test;
    try {
        caseCreateQueryRelease();
        caseDrawEndPixels();
        caseTextureDraw();
        caseMatrix();
        caseTraps();
        return finish("d3dx9");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "DIAG uncaught std::exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "DIAG uncaught non-std exception\n");
        return 1;
    }
}
