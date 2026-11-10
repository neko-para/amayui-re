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
//   ②a `D3DXCreateTextureFromFileInMemory`（TITLE Live2D 的真实入口）：PNG 头 → 同尺寸纹理；
//      未知容器 ⇒ E_FAIL + 出参 0（不 trap）。
//   ⑤ 纹理簇（`D3DXCreateTexture` → IDirect3DTexture9）：GetLevelDesc 的 D3DSURFACE_DESC 字段
//      与 `.lst` 0x48AD2A/0x48AD2D 的读法一致；LockRect 给出 guest 指针；**纹理 × 调制色**
//      的像素级断言；源矩形越界裁剪；纹理在批次里 AddRef / 合成后对称 Release。
//   ⑥ D3DX math：Multiply/Scaling/Translation/RotationAxis/RotationYawPitchRoll/OrthoLH/
//      PerspectiveFovLH/LookAtLH/Vec4Transform/Vec3Normalize 的**手算值**断言。
//   ⑦ 资源耗尽 **≠** unimplemented：把夹具自己那 1 MiB guest 堆占满到
//      "状态块能成、批次数组不够"（⛔ 不改任何默认尺寸 / 夹具堆尺寸）⇒ 出参仍是 NULL、
//      `stop_why` 是独立的"D3DX 资源耗尽"、`unimplemented_calls` 不涨、释放占块后堆用量回基线。
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
constexpr std::uint32_t kProcD3DXCreateTextureFromFileInMemory = 0x7FF811A0u;
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
        if (!fixStub("d3dx9_43", "D3DXCreateTextureFromFileInMemory",
                     kProcD3DXCreateTextureFromFileInMemory))
            return false;
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
        // ★ 白名单 = "调用返回了 EAX"的停机原因（不是"成功"）。新增的 `Trap` /
        //   `ResourceExhausted` 必须在内：否则 `create-sprite-oom` 的 `hr`（= 这里返回的 eax）
        //   会从 shim 写的 `kFail` 静默变成 0，把"调用真的发生了"这条判据弄坏。
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

    // D3DXCreateSprite(device, &out) + 把 RunResult 交出来（上面的 createSprite 只返回 eax，
    // 看不出"停在哪"）。⑦ 用它区分"资源耗尽硬停"与"调用失败"。
    std::uint32_t createSpriteRaw(std::uint32_t device, std::uint32_t* out_obj, lift::RunResult* rr) {
        // ★ 哨兵：出参槽预填 0xDEADBEEF ⇒ "调用根本没发生"（槽还是哨兵）不会伪装成"被写成 0"。
        const std::uint32_t out_ea = placeU32(0xDEADBEEFu);
        Asm a;
        a.pushImm(out_ea);
        a.pushImm(device);
        a.callRel(code_ea, kProcD3DXCreateSprite);
        a.ret();
        const std::uint32_t hr = runAsmRaw(a, rr);
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
// ②a D3DXCreateTextureFromFileInMemory（TITLE Live2D 纹理的真实入口；`.lst` 0x478398）
//   本层只读文件头：PNG 2048×2048 ×3 在实跑里会出现；这里用 7×5 的合成 PNG 断言尺寸/格式。
//   ★ 像素仍未解码（stub）⇒ 不在这里断言颜色；下一层接 PNG 解码时再加像素级用例。
// ---------------------------------------------------------------------------------------
void caseTextureFromMemory() {
    lift_test::g_current_case = "d3dx9/texture-from-memory";
    Fixture f;
    LIFT_CHECK(f.boot());
    if (!f.boot()) return;
    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d9, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    std::vector<std::uint8_t> png(33u, 0u);
    const std::uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    std::memcpy(png.data(), sig, 8);
    png[12] = 'I'; png[13] = 'H'; png[14] = 'D'; png[15] = 'R';
    png[16] = 0; png[17] = 0; png[18] = 0; png[19] = 7;   // width 7
    png[20] = 0; png[21] = 0; png[22] = 0; png[23] = 5;   // height 5
    png[24] = 8;   // bit depth
    png[25] = 6;   // color type RGBA
    const std::uint32_t png_ea = f.placeBytes(png);
    const std::size_t surfaces_before = f.m.surfaces().surfaceCount();
    const std::uint32_t out = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(
        f.callExport(kProcD3DXCreateTextureFromFileInMemory, {dev.object, png_ea, 33u, out}), 0u);
    const std::uint32_t tex = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    LIFT_CHECK_EQ_U32(f.m.surfaces().surfaceCount(), surfaces_before + 1);

    LIFT_CHECK_EQ_U32(f.method(tex, 0x34u, {}), 1u);  // GetLevelCount
    const std::uint32_t desc = f.placeBytes(std::vector<std::uint8_t>(32u, 0xEE));
    LIFT_CHECK_EQ_U32(f.method(tex, 0x44u, {0u, desc}), 0u);  // GetLevelDesc
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(desc + 0x00), lift::d3d9::kFmtA8R8G8B8);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(desc + 0x18), 7u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(desc + 0x1C), 5u);

    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(tex));
    LIFT_CHECK_EQ_U32(f.m.surfaces().surfaceCount(), surfaces_before);

    // 未知容器 ⇒ E_FAIL + 出参归零（调用方 `0x47839D jns` 会走失败分支；不 trap）
    const std::vector<std::uint8_t> junk(32u, 0x5A);
    const std::uint32_t junk_ea = f.placeBytes(junk);
    const std::uint32_t out2 = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(
        f.callExport(kProcD3DXCreateTextureFromFileInMemory, {dev.object, junk_ea, 32u, out2}),
        lift::d3d9::kFail);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(out2), 0u);
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

    // ---- +0x48 GetSurfaceLevel：返回 IDirect3DSurface9；GetDesc 复用纹理的 level 0 描述 ----
    const std::uint32_t surf_out = r.f.placeU32(0xDEADBEEFu);
    const std::uint32_t surf_desc = r.f.placeData(std::vector<std::uint8_t>(32u, 0).data(), 32u);
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x48u, {0u, surf_out}), lift::d3d9::kOk);
    const std::uint32_t surf = r.f.m.mem().rd32(surf_out);
    LIFT_CHECK_NE_U32(surf, 0u);
    LIFT_CHECK_NE_U32(surf, 0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(r.f.method(surf, 0x30u, {surf_desc}), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(surf_desc + 0u), 21u);  // Format = D3DFMT_A8R8G8B8
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(surf_desc + 0x18u), 4u);  // Width
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(surf_desc + 0x1Cu), 4u);  // Height
    LIFT_CHECK_EQ_U32(r.f.method(surf, 0x08u, {}), 0u);         // Release ⇒ 0
    LIFT_CHECK(!r.f.m.cpu().com->find(surf));

    // Release 到 0（refcount 2 ⇒ 两次）⇒ 像素块一起回收
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 1u);
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 0u);
    LIFT_CHECK(!r.f.m.cpu().com->find(tex));
    LIFT_CHECK_EQ_U32(r.f.m.surfaces().surfaceCount(), surfaces_before);
}

// ---------------------------------------------------------------------------------------
// ②d 纹理 surface 的**非 RT 寿命**（REQ-01M6ZZZ6PEVSFB93C6VJPKC04C，A 方案 = surface 持有父纹理）
//
// 时序（本单的判据场景）：建纹理 → `GetSurfaceLevel` 拿 surface → **guest 先 Release 纹理** →
//   仍用 surface 做 `GetDesc` / `LockRect` / 写像素 → 再 Release surface。
// ★ 修前（缺陷态）：纹理 refcount 到 0 ⇒ `destroyTextureState` 当场把 `'TEX1'` 状态块 + arena
//   像素块回收 ⇒ surface 的 `user` 变成已 free / 可被复用的 EA（`find(tex)` 已经是 false）。
// ★ A 方案：surface 建的时候就替父纹理 addRef 了一份 ⇒ 纹理 refcount 是 1，**没被回收**；
//   surface 到 0 时由 `ComRegistry::releaseImpl`（`owns_owner`）成对还回去 ⇒ 纹理此刻才到 0。
//
// 判据（每条在破坏时都会红）：
//   ① 纹理 Release 之后**对象还在**、refcount == 1（= surface 那一份）⇒ A 方案的直接可观测结果；
//   ② surface 的 `GetDesc` 仍给出同一张纹理的尺寸 / 格式（状态块没被 free、没被复用）；
//   ③ surface 的 `LockRect` 给出的 pBits 仍落在**同一块** arena 上（像素块没被回收 / 换人），
//      写进去的哨兵读得回来；再锁一次仍拿到同一个基址；
//   ④ 再 Release surface ⇒ 纹理此刻到 0 ⇒ 纹理对象 + arena 一起回收，且 guest 堆（`bytesInUse`
//      与块数）**回到基线** —— ⛔ 只测 ①~③ 会漏掉"只 addRef 不 release"（那正是漏堆），
//      这条断言就是防它的。
// ---------------------------------------------------------------------------------------
void caseSurfaceKeepsParentAlive() {
    lift_test::g_current_case = "d3dx9/surface-keeps-parent-alive";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.dev.object == 0) return;
    auto& mem = r.f.m.mem();
    auto& h = r.f.m.heap();
    lift::ComRegistry* com = r.f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    constexpr std::uint32_t kTexel = 0xA1B2C3D4u;
    const std::size_t surfaces_before = r.f.m.surfaces().surfaceCount();
    const std::uint32_t heap_before = h.bytesInUse();
    const std::size_t blocks_before = h.blockCount();

    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.dev.object, 4, 4, 1, 0, lift::d3d9::kFmtA8R8G8B8, 1, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    lift::d3d9::TextureInfo ti;
    LIFT_CHECK(lift::d3d9::textureInfo(r.f.m.cpu(), tex, &ti, nullptr));
    LIFT_CHECK_EQ_U32(ti.width, 4u);
    LIFT_CHECK_EQ_U32(ti.height, 4u);
    LIFT_CHECK_EQ_U32(com->refCount(tex), 1u);

    const std::uint32_t out = r.f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x48u, {0u, out}), lift::d3d9::kOk);  // GetSurfaceLevel
    const std::uint32_t surf = mem.rd32(out);
    LIFT_CHECK_NE_U32(surf, 0u);
    if (surf == 0) return;
    LIFT_CHECK_EQ_U32(com->refCount(tex), 2u);  // ← A 方案：surface 替父纹理持一份

    // ---- guest 先放掉自己那份纹理引用（surface 仍在手）----
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 1u);  // 2 → 1（= surface 那一份）
    // ① 纹理**没被回收**：对象还在，refcount 停在 surface 那一份上
    LIFT_CHECK(com->find(tex));
    LIFT_CHECK_EQ_U32(com->refCount(tex), 1u);
    LIFT_CHECK(r.f.m.surfaces().find(ti.arena) != nullptr);

    // ② 状态块仍归它：GetDesc 给出同一张纹理的尺寸 / 格式
    const std::uint32_t desc = r.f.placeBytes(std::vector<std::uint8_t>(32u, 0xEE));
    LIFT_CHECK_EQ_U32(r.f.method(surf, 0x30u, {desc}), lift::d3d9::kOk);  // GetDesc
    LIFT_CHECK_EQ_U32(mem.rd32(desc + 0x00u), lift::d3d9::kFmtA8R8G8B8);
    LIFT_CHECK_EQ_U32(mem.rd32(desc + 0x18u), 4u);  // Width
    LIFT_CHECK_EQ_U32(mem.rd32(desc + 0x1Cu), 4u);  // Height

    // ③ 像素块仍归它：LockRect 的 pBits 还在同一块 arena 上，写进去的哨兵读得回来
    const std::uint32_t lr = r.f.placeBytes(std::vector<std::uint8_t>(8u, 0xEE));
    LIFT_CHECK_EQ_U32(r.f.method(surf, 0x34u, {lr, 0u, 0u}), lift::d3d9::kOk);  // LockRect
    LIFT_CHECK_EQ_U32(mem.rd32(lr + 0u), ti.pitch);
    LIFT_CHECK_EQ_U32(mem.rd32(lr + 4u), ti.arena);  // 整幅锁定 ⇒ pBits = 像素块基址（没换人）
    mem.wr32(mem.rd32(lr + 4u), kTexel);
    LIFT_CHECK_EQ_U32(r.f.method(surf, 0x38u, {}), lift::d3d9::kOk);  // UnlockRect
    LIFT_CHECK_EQ_U32(mem.rd32(ti.arena), kTexel);
    // 再锁一次仍拿到同一份：状态块没被复用成别人的（否则这里会给出别的 arena / pitch）
    LIFT_CHECK_EQ_U32(r.f.method(surf, 0x34u, {lr, 0u, 0u}), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(mem.rd32(lr + 4u), ti.arena);
    LIFT_CHECK_EQ_U32(r.f.method(surf, 0x38u, {}), lift::d3d9::kOk);

    // ---- ④ 成对释放：surface 到 0 ⇒ 纹理此刻才到 0 ⇒ 对象 + arena 一起回收，堆回基线 ----
    LIFT_CHECK_EQ_U32(r.f.method(surf, 0x08u, {}), 0u);  // Release(surface) ⇒ 0
    LIFT_CHECK(!com->find(surf));
    LIFT_CHECK(!com->find(tex));  // 纹理到 0 是**因为** surface 还了那一份
    LIFT_CHECK(r.f.m.surfaces().find(ti.arena) == nullptr);
    LIFT_CHECK_EQ_U32(r.f.m.surfaces().surfaceCount(), surfaces_before);
    LIFT_CHECK_EQ_U32(h.bytesInUse(), heap_before);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(h.blockCount()),
                      static_cast<std::uint32_t>(blocks_before));
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
                          static_cast<std::uint32_t>(lift::StopReason::Trap));
        // ★ 已注册的槽走到 loud trap ⇒ **非 ok**（本单之前这里落 `Stopped`(ok) ⇒ 进程 exit 0）。
        LIFT_CHECK(!rr.ok());
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
                          static_cast<std::uint32_t>(lift::StopReason::Trap));
        // ★ 已注册的槽走到 loud trap ⇒ **非 ok**（本单之前这里落 `Stopped`(ok) ⇒ 进程 exit 0）。
        LIFT_CHECK(!rr.ok());
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
                          static_cast<std::uint32_t>(lift::StopReason::Trap));
        // ★ 已注册的槽走到 loud trap ⇒ **非 ok**（本单之前这里落 `Stopped`(ok) ⇒ 进程 exit 0）。
        LIFT_CHECK(!rr.ok());
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
                          static_cast<std::uint32_t>(lift::StopReason::Trap));
        // ★ 已注册的槽走到 loud trap ⇒ **非 ok**（本单之前这里落 `Stopped`(ok) ⇒ 进程 exit 0）。
        LIFT_CHECK(!rr.ok());
        LIFT_CHECK(std::string(r.f.m.cpu().stop_why ? r.f.m.cpu().stop_why : "") == "unimplemented D3DX9 call");
        LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(out_ea), 0u);  // 失败时出参清 0
    }
}

// ---------------------------------------------------------------------------------------
// ⑦ 资源耗尽 ≠ unimplemented：把**夹具自己**那 1 MiB guest 堆占满到
//    "状态块（128 B 载荷 → 136 B 块）能成、批次数组（1024 B → 1032 B 块）不够"的窗口。
//    · ⛔ 不改 `kFixtureHeapBytes` / `heap().init(...)`：占满的对象就是 guest 侧用的**同一个**
//      `GuestHeap` ⇒ `createSpriteState` 里的 `cpu.heap->alloc` 真的失败（不是伪造前提）。
//    · 取窗口下沿 `L = need_state`（那 136 B 正好被状态块吃满 ⇒ 批次数组一个字节都不剩）：
//      再宽一点会把"失败路径上多了一次小分配"静默吸收掉 ⇒ `allocCount()+1` 把"多一次就红"变成判据。
// ---------------------------------------------------------------------------------------
void caseCreateSpriteOom() {
    lift_test::g_current_case = "d3dx9/create-sprite-oom";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.dev.object == 0) return;

    auto& h = r.f.m.heap();

    // 一块 payload 在 guest 堆里**真正占**多少（块总大小 = 块头 + 对齐后的载荷）。
    // ★ 不在这里重抄块头 / 对齐（`heap.h` 的 Options 默认值）：用 `bytesInUse()` 的差值实测
    //   ⇒ 分配器改了对齐 / 块头，这里跟着走（口径只留在 `heap.cpp` 一处）。
    auto footprint = [&](std::uint32_t payload) -> std::uint32_t {
        const std::uint32_t b0 = h.bytesInUse();
        const std::uint32_t p = h.alloc(payload);
        if (p == 0) return 0;
        const std::uint32_t used = h.bytesInUse() - b0;
        h.free(p);
        return used;
    };

    const std::uint32_t baseline = h.bytesInUse();
    // 载荷 = `createSpriteState` 真会申请的两次：
    //   状态块 `kSpBytes` = 128（`d3dx9_sprite.cpp` 的 `kSpBytes`）；
    //   批次数组 `kInitialBatchCapacity`(16) × `kBatchEntryBytes`（`d3dx9_sprite.h`）。
    //   ★ 批次载荷写成**算式**（段 3 的 ⑥）：原先这里写死 `1152`，于是条目一变大（72 → 80 B）
    //     夹具仍在按 16×72 造窗口 —— 窗口的**上沿**与代码真实申请的字节数脱钩，载荷增长不会被发现。
    //     算式与 `kBatchEntryBytes` 同源：改常量 ⇒ 窗口跟着动 ⇒ 下面的范围断言 / 覆盖率警戒线红。
    //     ⛔ 常量 16 是 `d3dx9_sprite.cpp` 里 `kInitialBatchCapacity` 的值（内部量，不导出）；
    //     它若变大，`need_batch < 堆` 与失败位置会一起变，用例当场红而不是静默换窗口。
    const std::uint32_t need_state = footprint(128u);
    const std::uint32_t need_batch = footprint(16u * lift::d3dx9::kBatchEntryBytes);
    const std::uint32_t header = footprint(0u);
    LIFT_CHECK_EQ_U32(h.bytesInUse(), baseline);  // 探针没留下块
    LIFT_CHECK_NE_U32(need_state, 0u);
    LIFT_CHECK(need_batch > need_state);  // 前提：批次块比状态块大
    LIFT_CHECK(header >= 8u);             // 前提：块头至少 8 B

    // 占满：反复从**最大空闲块**里切，直到它落进 [need_state, need_batch)。
    // ★ 只依赖 `largestFreeBlock()`（不假设"堆里只有一个空闲块"）：每轮切掉 `need_state` 之外的
    //   全部（`biggest - need_state`，最少 `need_batch - need_state` = 896 B）⇒ 收敛。
    // ★ 窗口的**保证**是下面两条范围断言，不是这个循环：多碎片时最大空闲块可能落在
    //   (need_state, need_batch) 而不是正好 need_state。
    std::vector<std::uint32_t> fillers;
    for (int guard = 0; guard < 64; ++guard) {
        const std::uint32_t biggest = h.largestFreeBlock();
        if (biggest < need_batch) break;            // 已落进窗口
        if (biggest <= need_state + header) break;  // 切不动（下面的范围断言会报）
        const std::uint32_t p = h.alloc(biggest - need_state - header);
        LIFT_CHECK_NE_U32(p, 0u);
        if (p == 0) break;
        fillers.push_back(p);
    }
    LIFT_CHECK_NE_U32(static_cast<std::uint32_t>(fillers.size()), 0u);
    LIFT_CHECK(h.largestFreeBlock() >= need_state);  // 状态块放得下
    LIFT_CHECK(h.largestFreeBlock() < need_batch);   // 批次数组放不下

    const std::uint32_t filled = h.bytesInUse();
    const std::uint32_t allocs_before = h.allocCount();
    const std::uint32_t peak_before = h.peak();
    const std::uint32_t traps_before = r.f.m.cpu().shim.unimplemented_calls;

    lift::RunResult rr;
    std::uint32_t obj = 0xDEADBEEFu;
    const std::uint32_t hr = r.f.createSpriteRaw(r.dev.object, &obj, &rr);

    // ★ 从 `fillers.push_back` 之后**不许 early return**：`LIFT_CHECK` 不中止，占块必须被释放，
    //   否则"回到基线"那条会变成假红。
    // ① 出参 ppSprite 仍是 NULL（`shims/d3dx9.cpp` 在分配之前就把它清了 0）
    LIFT_CHECK_EQ_U32(obj, 0u);
    // ② 停机口径是**独立**的资源耗尽，不是 unimplemented，也不是正常结局
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(rr.reason),
                      static_cast<std::uint32_t>(lift::StopReason::ResourceExhausted));
    LIFT_CHECK(!rr.ok());  // ★ 非 ok ⇒ `exitCodeForRun` 给 2（本单之前 `Stopped`(ok) ⇒ 0）
    LIFT_CHECK(std::string(lift::stopReasonName(rr.reason)) == "resource-exhausted");
    // ★ 映射级"真实停机 ⇒ 非 0"：不是只断言枚举值，退出码也一起钉住。
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::exitCodeForRun(rr)), 2u);
    LIFT_CHECK(r.f.m.cpu().running == false);
    LIFT_CHECK(std::string(r.f.m.cpu().stop_why ? r.f.m.cpu().stop_why : "") == "D3DX 资源耗尽");
    LIFT_CHECK(rr.detail.find("D3DX 资源耗尽") != std::string::npos);  // 运行摘要也带这个标签
    // ③ 缺口计数**没有**增加（"误诊回归"的唯一机械判据）
    LIFT_CHECK_EQ_U32(r.f.m.cpu().shim.unimplemented_calls, traps_before);
    // ⑤ 分支真的走对了：状态块成功（正好 +1 次成功分配）→ 批次数组失败 ⇒ `free(state)`
    //    ★ 没有这一条，若状态块自己就失败，④ 会因为"一个字节都没动"而恒真。
    //    ★ 这是本用例的**覆盖率警戒线**（夹具前提），不是堆的策略断言：它只读 `allocCount()` /
    //      `peak()`，不读块表形状。失败路径上将来若多/少一次成功分配 ⇒ 它红是**信号**
    //      （那说明窗口要按失败路径上的分配数量重推），不是产品回归。
    LIFT_CHECK_EQ_U32(h.allocCount(), allocs_before + 1u);
    LIFT_CHECK(h.peak() > peak_before);
    LIFT_CHECK_EQ_U32(hr, lift::d3dx9::kFail);  // 辅助：两支都给 kFail ⇒ 它本身不是判据
    // ④ 失败路径没留半建：状态块建了又还 ⇒ 调用一返回就等于占满后的量
    LIFT_CHECK_EQ_U32(h.bytesInUse(), filled);

    // 释放占块 ⇒ `bytesInUse` 逐字节回到基线
    for (const std::uint32_t p : fillers) LIFT_CHECK(h.free(p));
    LIFT_CHECK_EQ_U32(h.bytesInUse(), baseline);

    // （行为，不是策略）现在建得出来 ⇒ 刚才的红是"堆满了"，不是"堆坏了"
    std::uint32_t obj2 = 0;
    LIFT_CHECK_EQ_U32(r.f.createSprite(r.dev.object, &obj2), 0u);
    LIFT_CHECK_NE_U32(obj2, 0u);
    if (obj2 != 0) LIFT_CHECK_EQ_U32(r.f.method(obj2, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(h.bytesInUse(), baseline);
}

// ---------------------------------------------------------------------------------------
// ⑧ `IDirect3DSurface9::GetDevice` 借出的是**一份新引用**（`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 段 3 的 ①）
//
// 判据（红了意味着什么）：`IDirect3DSurface9` 继承 `IDirect3DResource9` ⇒ `GetDevice` 的文档与
//   纹理 / VB / IB / 顶点声明是**同一条**（"increase the internal reference count … failure to
//   Release results in a memory leak"）。⇒ 每次调用后设备 refcount **+1**、把那一份 Release 掉回到
//   基线；出参非法时**不**加引用（先校验出参再 AddRef）。
//   ★ 修前这个 shim 是**裸借出** ⇒ `base + 1` 那条红（got = base）。
//   ★ 只造"借出 +1 / 归还"这一条判据：设备对象是**纹理表面**（`GetSurfaceLevel`）而不是后缓冲 ——
//     后者在 `tests/d3d9.cpp` 的 backbuffer 用例里另有一条同形断言。
// ---------------------------------------------------------------------------------------
void caseSurfaceGetDeviceRefcount() {
    lift_test::g_current_case = "d3dx9/surface-getdevice-refcount";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.dev.object == 0) return;
    lift::ComRegistry* com = r.f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.dev.object, 4u, 4u, 1u, 0u, lift::d3d9::kFmtA8R8G8B8, 1u, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    const std::uint32_t out_surf = r.f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x48u, {0u, out_surf}), 0u);  // GetSurfaceLevel(0)
    const std::uint32_t surf = r.f.m.mem().rd32(out_surf);
    LIFT_CHECK_NE_U32(surf, 0u);
    if (surf == 0) return;

    // 基线 = 设备当前引用计数（不从 1 写死：设备自己持引用时用例不用改）
    const std::uint32_t base = com->refCount(r.dev.object);
    const std::uint32_t out_dev = r.f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(r.f.method(surf, 0x0Cu, {out_dev}), 0u);
    LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(out_dev), r.dev.object);
    LIFT_CHECK_EQ_U32(com->refCount(r.dev.object), base + 1u);  // ← 修前 = base
    // 归还借出的那份 ⇒ 回基线（⛔ 不是"再借一次"）
    if (com->refCount(r.dev.object) == base + 1u)
        LIFT_CHECK_EQ_U32(r.f.method(r.dev.object, 0x08u, {}), base);
    LIFT_CHECK_EQ_U32(com->refCount(r.dev.object), base);

    // 出参 = NULL ⇒ D3DERR_INVALIDARG，且**不**给设备加引用（顺序判据）
    LIFT_CHECK_EQ_U32(r.f.method(surf, 0x0Cu, {0u}), lift::d3d9::kInvalidArg);
    LIFT_CHECK_EQ_U32(com->refCount(r.dev.object), base);

    // 收尾：surface 释放（它替父纹理持的那份由 `owns_owner` 还回）⇒ 再放纹理 ⇒ 堆回基线
    LIFT_CHECK_EQ_U32(r.f.method(surf, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 0u);
    LIFT_CHECK(!com->find(tex));
}

// ---------------------------------------------------------------------------------------
// ⑨ 未提交批次里的纹理：`OnLostDevice` / Sprite `Release` 必须按**身份**释放（段 3 的 ④）
//
// 判据（红了意味着什么）：批次条目只缓存对象 EA；guest 堆会**复用** EA ⇒ 纹理在批次还活着时被
//   （越界）Release、EA 被**另一张**纹理拿走之后，按裸 EA `release` 减的是**新占有者**。
//   Draw 时已记下 serial（进程级单调、永不复用）⇒ 销毁 / 设备丢失时 serial 不符就**不释放**、
//   只清"我们持引用"的标记（⛔ 一个引用都不减），并打默认级别可见的告警。
//   ★ 修前 `releaseQueuedTextures` 只按 EA Release ⇒ ② 的 refCount(t2) 期望 1 得 0（对象被减死）。
//   ★ ③ 是**正向对照**：没被替换的纹理仍然在 OnLostDevice 时被释放（证明 ② 不是"整条路径关掉"）。
// ---------------------------------------------------------------------------------------
void caseSpriteReleaseQueuedStaleTexture() {
    lift_test::g_current_case = "d3dx9/sprite-release-queued-stale-texture";
    Rig r;
    LIFT_CHECK(r.withSprite());
    if (r.sprite == 0) return;
    lift::ComRegistry* com = r.f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    auto fillTexture = [&](std::uint32_t tex, std::uint32_t value) {
        const std::uint32_t lr = r.f.placeBytes(std::vector<std::uint8_t>(8u, 0xEE));
        LIFT_CHECK_EQ_U32(r.f.method(tex, 0x4Cu, {0u, lr, 0u, 0u}), 0u);
        const std::uint32_t pitch = r.f.m.mem().rd32(lr + 0u);
        const std::uint32_t bits = r.f.m.mem().rd32(lr + 4u);
        LIFT_CHECK_NE_U32(bits, 0u);
        if (bits == 0) return;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                r.f.m.mem().wr32(bits + static_cast<std::uint32_t>(y) * pitch +
                                     static_cast<std::uint32_t>(x) * 4u,
                                 value);
        LIFT_CHECK_EQ_U32(r.f.method(tex, 0x50u, {0u}), 0u);
    };
    auto makeTexture = [&](std::uint32_t* out) {
        LIFT_CHECK_EQ_U32(r.f.createTexture(r.dev.object, 4u, 4u, 1u, 0u, lift::d3d9::kFmtA8R8G8B8, 1u, out),
                          0u);
        LIFT_CHECK_NE_U32(*out, 0u);
    };

    // ② 被替换的那一张：Draw 时 sprite 替它持一份引用（refCount 2），guest 放掉自己那份（1），
    //    再越界放一次 ⇒ 把 sprite 那份也抵掉 ⇒ 对象死、EA 被另一张纹理复用。
    std::uint32_t t1 = 0;
    std::uint32_t t_dummy = 0;
    makeTexture(&t1);
    if (t1 == 0) return;
    fillTexture(t1, 0xFF0000FFu);
    const std::uint32_t t1_gen = com->generationOf(t1);
    LIFT_CHECK_NE_U32(t1_gen, 0u);
    makeTexture(&t_dummy);  // 同尺寸哑纹理：两者释放后空洞连成一片 ⇒ EA 复用
    if (t_dummy == 0) return;

    LIFT_CHECK_EQ_U32(r.begin(0u), 0u);
    const std::uint32_t pos0 = r.f.placeVec3(4.0f, 4.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.drawTex(t1, 0u, 0u, pos0, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(t1), 2u);  // guest 1 + 批次 1（Draw 的 AddRef）
    LIFT_CHECK_EQ_U32(r.f.method(t1, 0x08u, {}), 1u);  // guest 放掉自己那份
    if (com->find(t1)) LIFT_CHECK_EQ_U32(r.f.method(t1, 0x08u, {}), 0u);  // 越界：批次那份被抵掉
    LIFT_CHECK(!com->find(t1));
    LIFT_CHECK_EQ_U32(r.f.method(t_dummy, 0x08u, {}), 0u);

    std::uint32_t t2 = 0;
    makeTexture(&t2);
    if (t2 == 0) return;
    LIFT_CHECK_EQ_U32(t2, t1);                        // 前提 1：对象 EA 复用
    LIFT_CHECK_NE_U32(com->generationOf(t2), t1_gen);  // 前提 2：身份确实换了
    fillTexture(t2, 0xFFFF0000u);

    // 丢弃未提交批次 ⇒ serial 不符 ⇒ 不减新占有者（修前这里把 t2 减到 0 / 对象消失）
    LIFT_CHECK_EQ_U32(r.f.method(r.sprite, 0x30u, {}), 0u);  // OnLostDevice
    LIFT_CHECK_EQ_U32(com->refCount(t2), 1u);                 // ← 修前 = 0
    LIFT_CHECK(com->find(t2));
    // OnLostDevice 丢弃批次但**不**关 Begin（真机也是"设备丢了、批次没了"）⇒ 关掉再开新批次。
    LIFT_CHECK_EQ_U32(r.end(), 0u);

    // ③ 正向对照：没被替换的纹理照旧在 OnLostDevice 时释放（堆回基线）
    const std::uint32_t heap_before = r.f.m.heap().bytesInUse();
    std::uint32_t t3 = 0;
    makeTexture(&t3);
    if (t3 == 0) return;
    fillTexture(t3, 0xFF00FF00u);
    LIFT_CHECK_EQ_U32(r.begin(0u), 0u);
    const std::uint32_t pos1 = r.f.placeVec3(8.0f, 8.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.drawTex(t3, 0u, 0u, pos1, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(t3), 2u);
    LIFT_CHECK_EQ_U32(r.f.method(t3, 0x08u, {}), 1u);  // 只剩批次那份
    LIFT_CHECK_EQ_U32(r.f.method(r.sprite, 0x30u, {}), 0u);  // OnLostDevice ⇒ 还真回去
    LIFT_CHECK(!com->find(t3));
    LIFT_CHECK_EQ_U32(r.f.m.heap().bytesInUse(), heap_before);

    // 收尾
    LIFT_CHECK_EQ_U32(r.f.method(t2, 0x08u, {}), 0u);
    LIFT_CHECK(!com->find(t2));
    LIFT_CHECK_EQ_U32(r.f.method(r.sprite, 0x08u, {}), 0u);
}

// ---------------------------------------------------------------------------------------
// ⑩ `compositeBatch` 的**早退分支**也必须按纹理身份释放（`REQ-01M70SHFT6RZA7RRY1R051Z2PB` #1）
//
// 判据（红了意味着什么）：`compositeBatch` 有 4 个**在合成主路径身份校验之前**的早退出口
//   （无 src rect / 空矩形 / 四边形退化 / 完全在屏外）。它们以前对该条目裸 `releaseEntry`
//   ⇒ 纹理在批次还活着时被（越界）Release、同一对象 EA 被**另一张**纹理拿走之后，End 走早退
//   分支就把**新纹理**减到 0（对象被减死）。修法 = 把 serial 校验放进 releaseEntry（所有出口共用）。
// ★ 本用例走"完全在屏外"这个早退口（位置 5000,5000 ⇒ 后缓冲 1280×720 裁剪后为空）。
// ★ ③ 是**正向对照**：没被替换的纹理仍然在正常出口被释放（证明 ② 不是"把 release 全关掉"）。
// ★ 变异（`if (!entryTextureIsCurrent(...)) return;` 改成丢弃返回值）⇒ ② 的 refcount 期望 1 得 0。
// ---------------------------------------------------------------------------------------
void caseSpriteEarlyExitStaleTexture() {
    lift_test::g_current_case = "d3dx9/sprite-early-exit-stale-texture";
    Rig r;
    LIFT_CHECK(r.withSprite());
    if (r.sprite == 0) return;
    lift::ComRegistry* com = r.f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    auto fillTexture = [&](std::uint32_t tex, std::uint32_t value) {
        const std::uint32_t lr = r.f.placeBytes(std::vector<std::uint8_t>(8u, 0xEE));
        LIFT_CHECK_EQ_U32(r.f.method(tex, 0x4Cu, {0u, lr, 0u, 0u}), 0u);
        const std::uint32_t pitch = r.f.m.mem().rd32(lr + 0u);
        const std::uint32_t bits = r.f.m.mem().rd32(lr + 4u);
        LIFT_CHECK_NE_U32(bits, 0u);
        if (bits == 0) return;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                r.f.m.mem().wr32(bits + static_cast<std::uint32_t>(y) * pitch +
                                     static_cast<std::uint32_t>(x) * 4u,
                                 value);
        LIFT_CHECK_EQ_U32(r.f.method(tex, 0x50u, {0u}), 0u);
    };
    auto makeTexture = [&](std::uint32_t* out) {
        LIFT_CHECK_EQ_U32(r.f.createTexture(r.dev.object, 4u, 4u, 1u, 0u, lift::d3d9::kFmtA8R8G8B8, 1u, out),
                          0u);
        LIFT_CHECK_NE_U32(*out, 0u);
    };

    // ① 批次里替 T1 持一份引用（refCount 2）；guest 放掉自己那份、再越界放一次 ⇒ 对象死、EA 可复用。
    std::uint32_t t1 = 0;
    std::uint32_t t_dummy = 0;
    makeTexture(&t1);
    if (t1 == 0) return;
    fillTexture(t1, 0xFF0000FFu);
    const std::uint32_t t1_gen = com->generationOf(t1);
    LIFT_CHECK_NE_U32(t1_gen, 0u);
    makeTexture(&t_dummy);  // 同尺寸哑纹理：两者释放后空洞连成一片 ⇒ EA 复用
    if (t_dummy == 0) return;

    LIFT_CHECK_EQ_U32(r.begin(0u), 0u);
    // ★ 位置在**后缓冲之外**：这个条目会从"完全屏外"那个早退出口收尾（身份校验之前）
    const std::uint32_t pos0 = r.f.placeVec3(5000.0f, 5000.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.drawTex(t1, 0u, 0u, pos0, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(t1), 2u);  // guest 1 + 批次 1（Draw 的 AddRef）
    LIFT_CHECK_EQ_U32(r.f.method(t1, 0x08u, {}), 1u);
    if (com->find(t1)) LIFT_CHECK_EQ_U32(r.f.method(t1, 0x08u, {}), 0u);  // 越界：批次那份被抵掉
    LIFT_CHECK(!com->find(t1));
    LIFT_CHECK_EQ_U32(r.f.method(t_dummy, 0x08u, {}), 0u);

    // ② T2 拿回 T1 的对象 EA（前提硬断言：不成立先按 heap 的分块重造夹具，⛔ 不是删断言）
    std::uint32_t t2 = 0;
    makeTexture(&t2);
    if (t2 == 0) return;
    LIFT_CHECK_EQ_U32(t2, t1);
    LIFT_CHECK_NE_U32(com->generationOf(t2), t1_gen);
    fillTexture(t2, 0xFFFF0000u);

    // 合成走"完全在屏外"早退分支 ⇒ serial 不符 ⇒ 新占有者一个引用都不减（修前 = 0）
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(t2), 1u);  // ← 修前 = 0
    LIFT_CHECK(com->find(t2));

    // ③ 正向对照：没被替换的纹理在正常出口照旧被释放
    LIFT_CHECK_EQ_U32(r.begin(0u), 0u);
    const std::uint32_t pos1 = r.f.placeVec3(8.0f, 8.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.drawTex(t2, 0u, 0u, pos1, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(t2), 2u);
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(t2), 1u);  // 正常出口真的把批次那份还回去

    // 收尾（判活再调：变异下 T2 可能已经被减死）
    if (com->find(t2)) LIFT_CHECK_EQ_U32(r.f.method(t2, 0x08u, {}), 0u);
    LIFT_CHECK(!com->find(t2));
}

}  // namespace

// ---------------------------------------------------------------------------------------
// ②e `D3DXSPRITE_DO_NOT_ADDREF_TEXTURE` 批次的**纹理身份**（`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 段 2 的 ②）
//
// ★ flag 的语义**不改**（微软原文："Disables calling AddRef() on every draw, and Release() on Flush()"
//   ⇒ Draw 不 AddRef、End/Flush 不 Release 是**文档语义**，不是缺陷）。真残余是"没有身份戳"：
//   批次条目只存对象 EA，而 guest 堆会复用 EA ⇒ 纹理在批次还活着时被释放、EA 被**另一张**纹理拿走，
//   `End()` 只按 EA + `'TEX1'` magic 重解析会**成功**并静默采别人的像素。
//
// 判据（红了意味着什么）：
//   ① `DO_NOT_ADDREF` 下 Draw **不**改纹理 refcount（文档语义；修后仍成立）；
//   ② 批次里的纹理被释放、EA + `'TEX1'` 状态块 EA 被同尺寸新纹理**同时**复用之后，`End()` 必须
//      **跳过这个精灵**（批次的其余部分不受影响）⇒ 目标像素**一个都没变**、新纹理的 refcount
//      不被误减、对象还在。★ 判据落在**像素 + refcount** 上（日志抓不到；告警是默认级别 warn）。
//   ③ 没被替换的 `DO_NOT_ADDREF` 精灵仍然正常合成（证明②的判据不是"把这条路径整个关掉"）。
// ★ EA 复用的**前提**硬断言（对象 EA + 状态块 EA 都要落回原址）；不成立就红 —— ⛔ 不是删断言，
//   先按 `runtime/heap.cpp` 的分块 / `min_split` 重造夹具（同 `caseSurfaceKeepsParentAlive` 的口径）。
// ---------------------------------------------------------------------------------------
void caseDoNotAddRefStaleTexture() {
    lift_test::g_current_case = "d3dx9/sprite-donotaddref-stale-texture";
    Rig r;
    LIFT_CHECK(r.withSprite());
    if (r.sprite == 0) return;
    const lift::d3d9::DeviceInfo& d = r.dev.info;
    LIFT_CHECK_EQ_U32(r.f.clearTo(r.dev, 0x004080C0u), 0u);
    lift::ComRegistry* com = r.f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    auto fillTexture = [&](std::uint32_t tex, std::uint32_t value) {
        const std::uint32_t lr = r.f.placeBytes(std::vector<std::uint8_t>(8u, 0xEE));
        LIFT_CHECK_EQ_U32(r.f.method(tex, 0x4Cu, {0u, lr, 0u, 0u}), 0u);
        const std::uint32_t pitch = r.f.m.mem().rd32(lr + 0u);
        const std::uint32_t bits = r.f.m.mem().rd32(lr + 4u);
        LIFT_CHECK_NE_U32(bits, 0u);
        if (bits == 0) return;
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                r.f.m.mem().wr32(bits + static_cast<std::uint32_t>(y) * pitch + static_cast<std::uint32_t>(x) * 4u,
                                 value);
            }
        }
        LIFT_CHECK_EQ_U32(r.f.method(tex, 0x50u, {0u}), 0u);
    };
    auto makeTexture = [&](std::uint32_t* out) {
        LIFT_CHECK_EQ_U32(r.f.createTexture(r.dev.object, 4, 4, 1, 0, lift::d3d9::kFmtA8R8G8B8, 1, out), 0u);
        LIFT_CHECK_NE_U32(*out, 0u);
    };

    std::uint32_t t0 = 0;
    std::uint32_t t1 = 0;
    makeTexture(&t1);
    if (t1 == 0) return;
    fillTexture(t1, 0xFF0000FFu);  // 不透明蓝（A8R8G8B8 的 D3DCOLOR 位序：A R G B）
    const std::uint32_t t1_state = com->userOf(t1);
    const std::uint32_t t1_gen = com->generationOf(t1);
    LIFT_CHECK_NE_U32(t1_state, 0u);
    LIFT_CHECK_NE_U32(t1_gen, 0u);
    makeTexture(&t0);  // 哑纹理：同尺寸 ⇒ 一起释放后空洞连成一片（EA 复用的前提）
    if (t0 == 0) return;

    const std::uint32_t flags = lift::d3dx9::kSpriteAlphaBlend | lift::d3dx9::kSpriteDoNotAddRefTexture;
    LIFT_CHECK_EQ_U32(r.begin(flags), 0u);
    const std::uint32_t pos0 = r.f.placeVec3(10.0f, 10.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.drawTex(t1, 0u, 0u, pos0, 0xFFFFFFFFu), 0u);
    // ① 文档语义：这一位就是"不 AddRef" ⇒ Draw 之后 refcount 仍是 1
    LIFT_CHECK_EQ_U32(com->refCount(t1), 1u);
    // guest 放掉自己那份 ⇒ 因为没钉住，对象当场到 0（这正是 flag 的语义，不是缺陷）
    LIFT_CHECK_EQ_U32(r.f.method(t1, 0x08u, {}), 0u);
    LIFT_CHECK(!com->find(t1));
    LIFT_CHECK_EQ_U32(r.f.method(t0, 0x08u, {}), 0u);

    std::uint32_t t2 = 0;
    makeTexture(&t2);
    if (t2 == 0) return;
    LIFT_CHECK_EQ_U32(t2, t1);                       // 前提 1：对象 EA 复用
    LIFT_CHECK_EQ_U32(com->userOf(t2), t1_state);     // 前提 2：`'TEX1'` 状态块 EA 也复用 ⇒ magic 校验必过
    LIFT_CHECK_NE_U32(com->generationOf(t2), t1_gen);
    fillTexture(t2, 0xFFFF0000u);  // 不透明红：若静默采错，下面的像素会变成红

    // ② 合成：身份不符 ⇒ 跳过这个精灵（⛔ 不采 T2 的像素）
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 10, 10), 0x004080C0u);  // 清屏色原样（修前 = 0x00FF0000）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 11, 11), 0x004080C0u);
    LIFT_CHECK_EQ_U32(com->refCount(t2), 1u);  // ⛔ 新占有者一个引用都不被减
    LIFT_CHECK(com->find(t2));

    // ③ 没被替换的 DO_NOT_ADDREF 精灵照常合成（否则 ② 就成了"把这条路径整个关掉"）
    LIFT_CHECK_EQ_U32(r.begin(flags), 0u);
    const std::uint32_t pos1 = r.f.placeVec3(20.0f, 20.0f, 0.0f);
    LIFT_CHECK_EQ_U32(r.drawTex(t2, 0u, 0u, pos1, 0xFFFFFFFFu), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(t2), 1u);  // 仍然不 AddRef（文档语义没变）
    LIFT_CHECK_EQ_U32(r.end(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 20, 20), 0x00FF0000u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(d, 23, 23), 0x00FF0000u);
    LIFT_CHECK_EQ_U32(com->refCount(t2), 1u);

    // 收尾：guest 那份释放 ⇒ 对象 + 状态块 + 像素块一起回收
    LIFT_CHECK_EQ_U32(r.f.method(t2, 0x08u, {}), 0u);
    LIFT_CHECK(!com->find(t2));
}

int main() {
    using namespace lift_test;
    try {
        caseCreateQueryRelease();
        caseDrawEndPixels();
        caseTextureFromMemory();
        caseTextureDraw();
        caseSurfaceKeepsParentAlive();
        caseSurfaceGetDeviceRefcount();
        caseSpriteReleaseQueuedStaleTexture();
        caseSpriteEarlyExitStaleTexture();
        caseDoNotAddRefStaleTexture();
        caseMatrix();
        caseTraps();
        caseCreateSpriteOom();
        return finish("d3dx9");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "DIAG uncaught std::exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "DIAG uncaught non-std exception\n");
        return 1;
    }
}
