// emulator-lift · M2 夹具：D3D9 最小设备簇（pure 档：不走游戏 / 不碰 LFS）
//
// 判据（对照任务单 `data/requirements/` 的 D3D9 最小设备簇）：
//   ① `Direct3DCreate9(32)` 返回**guest 可见的 COM 对象**（非 0）、`QI(IID_IDirect3D9)` 成功、
//      `Release` 到 0 后对象真的被回收（对象表空）。
//   ② `GetAdapterDisplayMode` 返回 **1280×720 / D3DFMT_X8R8G8B8(22)**，且字段落在
//      **实测偏移**（+0 宽 / +4 高 / +8 刷新 / +0xC 格式）；16 字节之外不被写坏。
//   ③ `GetAdapterModeCount` / `EnumAdapterModes`：模式数 > 0、`iMode` 越界 ⇒ `D3DERR_INVALIDCALL`。
//   ④ `CreateDevice` 成功（首个组合 DeviceType=1 / BehaviorFlags=0x44）+ `GetDeviceCaps` 的
//      MaxTextureWidth/Height 非 0（+0x58/+0x5C）；**56 字节的 D3DPRESENT_PARAMETERS 一字节没被写坏**；
//      caps 的 0xD4 字节**全部被写**（两块不同初值的缓冲得到逐字节相同的结果）+ 0xD4 外不被写。
//   ⑤ `Clear(0,NULL,3,0,1.0f,0)`（以及带 D3DRECT 的 Clear）**真的**改后缓冲像素：颜色对、矩形外不变。
//   ⑥ `Present` 之后 `frameCount()==1`、`lastHash()` 与 arena 哈希一致；**两台新机器同输入 ⇒ 同帧哈希**。
//   ⑦ 未实现的槽 ⇒ **loud trap**（机器停 + `stop_why`）且日志输入里带 iface + 偏移（`r.detail` 里就有）
//      —— ⛔ 不许返回 0 继续跑。
//
// ★ 所有方法调用都从 **guest 代码** 经 vtable 调（`mov eax,[vtbl]; push …; call [eax+off]`），
//   测的是"guest 可见的对象"，不是宿主直接调函数。
// ★ 这些用例**会红**（变异验证见交付说明）：`Clear` 不清 ⇒ ⑤ 红；`GetAdapterDisplayMode` 报 0×0 ⇒ ② 红；
//   `fillCaps` 少写 MaxTexture* ⇒ ④ 红；把未实现槽改成"返回 0" ⇒ ⑦ 红。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/d3d9.h"
#include "runtime/d3d9_pipeline.h"
#include "runtime/ddraw_surface.h"
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
constexpr std::uint32_t kSentinel = 0xFFFFFFFFu;
constexpr std::uint32_t kFixtureHeapBytes = 0x00100000u;

struct Asm {
    std::vector<std::uint8_t> b;

    std::uint32_t ea(std::uint32_t base) const { return base + static_cast<std::uint32_t>(b.size()); }
    void u8(std::uint8_t v) { b.push_back(v); }
    void u16(std::uint16_t v) {
        u8(static_cast<std::uint8_t>(v & 0xFF));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
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
    void movMemEax(std::uint32_t ea) {  // mov [imm32], eax
        u8(0xA3);
        u32(ea);
    }
    void callRel(std::uint32_t base, std::uint32_t target) {
        const std::uint32_t at = ea(base);
        u8(0xE8);
        u32(static_cast<std::uint32_t>(static_cast<std::int32_t>(target - (at + 5))));
    }
    void callEax() {
        u8(0xFF);
        u8(0xD0);
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
        lift::ShimDef* d = m.shims().findMutable("d3d9", "Direct3DCreate9");
        if (!d) return false;
        d->stub_ea = kProcDirect3DCreate9;
        m.cache().installShim(kProcDirect3DCreate9, d->id);
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

    std::vector<std::uint8_t> bytesOf(std::uint32_t ea, std::size_t n) {
        std::vector<std::uint8_t> out(n, 0);
        for (std::size_t i = 0; i < n; ++i) out[i] = m.mem().rd8(ea + static_cast<std::uint32_t>(i));
        return out;
    }

    // 放一段 guest 代码并执行到 0xFFFFFFFF 哨兵；返回 EAX
    std::uint32_t runAsm(Asm& a) {
        lift::RunResult r;
        return runAsmRaw(a, &r);
    }
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

    // ① Direct3DCreate9(32)
    std::uint32_t createDirect3D() {
        Asm a;
        a.pushImm(lift::d3d9::kSdkVersion);
        a.callRel(kCodeBase, kProcDirect3DCreate9);
        a.ret();
        return runAsm(a);
    }

    // ④ CreateDevice：按**实测偏移**填 D3DPRESENT_PARAMETERS（56 B）
    struct Device {
        std::uint32_t object = 0;
        std::uint32_t state = 0;
        lift::d3d9::DeviceInfo info;
        std::uint32_t params_ea = 0;
    };

    bool createDevice(std::uint32_t d3d9_obj, std::uint32_t device_type, std::uint32_t behavior, int width,
                      int height, std::uint32_t format, Device* out) {
        std::vector<std::uint8_t> pp(lift::d3d9::kPresentParamsBytes, 0);
        auto wr = [&pp](std::uint32_t off, std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                pp[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        };
        wr(lift::d3d9::kPpBackBufferWidth, static_cast<std::uint32_t>(width));
        wr(lift::d3d9::kPpBackBufferHeight, static_cast<std::uint32_t>(height));
        wr(lift::d3d9::kPpBackBufferFormat, format);
        wr(lift::d3d9::kPpBackBufferCount, 1u);
        wr(lift::d3d9::kPpSwapEffect, 1u);            // D3DSWAPEFFECT_DISCARD
        wr(lift::d3d9::kPpHDeviceWindow, 0x00001234u);
        wr(lift::d3d9::kPpWindowed, 1u);
        wr(lift::d3d9::kPpEnableAutoDepthStencil, 1u);
        wr(lift::d3d9::kPpAutoDepthStencilFormat, 75u);  // D3DFMT_D24S8
        wr(lift::d3d9::kPpFlags, 1u);
        wr(lift::d3d9::kPpPresentationInterval, 0x80000000u);
        const std::uint32_t pp_ea = placeBytes(pp);
        const std::uint32_t out_ea = placeU32(0u);
        const std::vector<std::uint8_t> before = bytesOf(pp_ea, lift::d3d9::kPresentParamsBytes);
        const std::uint32_t hr = method(d3d9_obj, 0x40u,
                                        {0u, device_type, 0x00001234u, behavior, pp_ea, out_ea});
        LIFT_CHECK_EQ_U32(hr, lift::d3d9::kOk);
        // ★ 56 字节的入参结构一个字节都不许被写坏
        const std::vector<std::uint8_t> after = bytesOf(pp_ea, lift::d3d9::kPresentParamsBytes);
        LIFT_CHECK(before == after);
        out->object = m.mem().rd32(out_ea);
        out->params_ea = pp_ea;
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

    // 后缓冲像素（guest 内存；逐点读）
    std::uint32_t pixelAt(const lift::d3d9::DeviceInfo& d, int x, int y) {
        return m.mem().rd32(d.arena + static_cast<std::uint32_t>(y) * d.pitch +
                            static_cast<std::uint32_t>(x) * 4u);
    }
};

// ---------------------------------------------------------------------------------------
// ① Direct3DCreate9 返回 guest 可见对象 + QI + Release 到 0
// ---------------------------------------------------------------------------------------
void caseCreateAndQueryInterface() {
    lift_test::g_current_case = "d3d9/create+qi+release";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t obj = f.createDirect3D();
    LIFT_CHECK_NE_U32(obj, 0u);
    if (obj == 0) return;
    LIFT_CHECK_NE_U32(f.m.mem().rd32(obj), 0u);  // obj+0 = vtable（guest 看得见）
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(obj), 1u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(obj + 4u), 1u);  // obj+4 = refcount（guest 看到同一份）

    // QI(IID_IDirect3D9) 成功（夹具用与 shim 同一份 IID 常量；SDK d3d9.h 的 {81BDCBCA-…}）
    const std::uint8_t iid[16] = {0xCA, 0xCB, 0xBD, 0x81, 0xD4, 0x64, 0x6D, 0x42,
                                  0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C};
    const std::uint32_t iid_ea = f.placeBytes(std::vector<std::uint8_t>(iid, iid + 16));
    const std::uint32_t ppv = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(obj, 0x00u, {iid_ea, ppv}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(ppv), obj);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(obj), 2u);

    // 未知 IID ⇒ E_NOINTERFACE，且不写 ppv
    std::uint8_t bogus[16] = {0};
    bogus[0] = 0x42;
    const std::uint32_t bogus_ea = f.placeBytes(std::vector<std::uint8_t>(bogus, bogus + 16));
    const std::uint32_t ppv2 = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(obj, 0x00u, {bogus_ea, ppv2}), lift::d3d9::kNoInterface);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(ppv2), 0u);

    // Release 到 0：对象真的被回收（guest 堆 + 对象表）
    LIFT_CHECK_EQ_U32(f.method(obj, 0x08u, {}), 1u);
    LIFT_CHECK_EQ_U32(f.method(obj, 0x08u, {}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(obj));
    LIFT_CHECK_EQ_U32(f.m.cpu().com->objectCount(), 0u);

    // Direct3DCreate9 的版本校验：不是 32 ⇒ NULL（真机语义，不是"没实现"）
    Asm a;
    a.pushImm(31u);
    a.callRel(kCodeBase, kProcDirect3DCreate9);
    a.ret();
    LIFT_CHECK_EQ_U32(f.runAsm(a), 0u);
}

// ---------------------------------------------------------------------------------------
// ② GetAdapterDisplayMode = 1280×720×X8R8G8B8（实测偏移）+ 模式表
// ---------------------------------------------------------------------------------------
void caseAdapterDisplayMode() {
    lift_test::g_current_case = "d3d9/adapter-display-mode+modes";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t obj = f.createDirect3D();
    LIFT_CHECK_NE_U32(obj, 0u);
    if (obj == 0) return;

    // 16 字节的 D3DDISPLAYMODE + 4 字节 canary
    const std::uint32_t mode_ea = f.placeBytes(std::vector<std::uint8_t>(lift::d3d9::kDisplayModeBytes + 4u, 0xCC));
    LIFT_CHECK_EQ_U32(f.method(obj, 0x20u, {0u, mode_ea}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(mode_ea + lift::d3d9::kDmWidth), 1280u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(mode_ea + lift::d3d9::kDmHeight), 720u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(mode_ea + lift::d3d9::kDmRefreshRate), 60u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(mode_ea + lift::d3d9::kDmFormat), lift::d3d9::kFmtX8R8G8B8);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(mode_ea + lift::d3d9::kDisplayModeBytes), 0xCCCCCCCCu);  // 越界不写

    // 与 DDraw 的虚拟显示器**同一份**模式（⛔ 不是 D3D9 自己发明一个桌面）
    int dw = 0;
    int dh = 0;
    unsigned dbpp = 0;
    LIFT_CHECK(lift::ddraw::displayMode(f.m.cpu(), &dw, &dh, &dbpp, nullptr));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(dw), 1280u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(dh), 720u);
    LIFT_CHECK_EQ_U32(dbpp, 32u);
    // 反过来：DDraw 改了模式 ⇒ D3D9 报同一份
    std::string why;
    LIFT_CHECK(lift::ddraw::setDisplayMode(f.m.cpu(), 1024, 768, 32, &why));
    const std::uint32_t mode2 = f.placeBytes(std::vector<std::uint8_t>(lift::d3d9::kDisplayModeBytes, 0));
    LIFT_CHECK_EQ_U32(f.method(obj, 0x20u, {0u, mode2}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(mode2 + lift::d3d9::kDmWidth), 1024u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(mode2 + lift::d3d9::kDmHeight), 768u);

    // Adapter != 0 ⇒ D3DERR_INVALIDCALL（本模拟器只有适配器 0）
    LIFT_CHECK_EQ_U32(f.method(obj, 0x20u, {1u, mode2}), lift::d3d9::kInvalidCall);

    // #4 GetAdapterCount = 1
    LIFT_CHECK_EQ_U32(f.method(obj, 0x10u, {}), 1u);

    // #6 GetAdapterModeCount：X8R8G8B8 有模式；不支持的格式 0 条
    const std::uint32_t n = f.method(obj, 0x18u, {0u, lift::d3d9::kFmtX8R8G8B8});
    LIFT_CHECK_NE_U32(n, 0u);
    LIFT_CHECK(n >= 15u);
    LIFT_CHECK_EQ_U32(f.method(obj, 0x18u, {0u, 99u}), 0u);

    // #7 EnumAdapterModes：越界 ⇒ D3DERR_INVALIDCALL；合法 index ⇒ 宽/高落在 +0/+4
    const std::uint32_t bad = f.placeBytes(std::vector<std::uint8_t>(lift::d3d9::kDisplayModeBytes, 0xEE));
    LIFT_CHECK_EQ_U32(f.method(obj, 0x1Cu, {0u, lift::d3d9::kFmtX8R8G8B8, n, bad}),
                      lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(bad + lift::d3d9::kDmWidth), 0xEEEEEEEEu);  // 越界不写
    const std::uint32_t good = f.placeBytes(std::vector<std::uint8_t>(lift::d3d9::kDisplayModeBytes, 0));
    LIFT_CHECK_EQ_U32(f.method(obj, 0x1Cu, {0u, lift::d3d9::kFmtX8R8G8B8, 4u, good}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(good + lift::d3d9::kDmWidth), 1280u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(good + lift::d3d9::kDmHeight), 720u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(good + lift::d3d9::kDmFormat), lift::d3d9::kFmtX8R8G8B8);
}

// ---------------------------------------------------------------------------------------
// ③ CreateDevice + GetDeviceCaps + SetDialogBoxMode + CheckDeviceFormat
// ---------------------------------------------------------------------------------------
void caseCreateDeviceAndCaps() {
    lift_test::g_current_case = "d3d9/create-device+caps";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t obj = f.createDirect3D();
    LIFT_CHECK_NE_U32(obj, 0u);
    if (obj == 0) return;

    // 引擎的**第一个**重试组合：DeviceType=HAL(1) / BehaviorFlags=0x44
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(obj, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(dev.info.width), 1280u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(dev.info.height), 720u);
    LIFT_CHECK_EQ_U32(dev.info.format, lift::d3d9::kFmtX8R8G8B8);
    LIFT_CHECK_EQ_U32(dev.info.bpp, 32u);
    LIFT_CHECK_EQ_U32(dev.info.pitch, 1280u * 4u);
    LIFT_CHECK_EQ_U32(dev.info.device_type, 1u);
    LIFT_CHECK_EQ_U32(dev.info.behavior, 0x44u);
    LIFT_CHECK_EQ_U32(dev.info.hwnd, 0x00001234u);
    LIFT_CHECK_EQ_U32(dev.info.auto_depth_format, 75u);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(dev.object), 1u);

    // #20 SetDialogBoxMode(TRUE) ⇒ S_OK（headless：只记录）
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x50u, {1u}), 0u);

    // #7 GetDeviceCaps(+0x1C)：0xD4 字节**全部被写**（两块不同初值 ⇒ 逐字节相同），0xD4 外不写
    const std::uint32_t caps_a = f.placeBytes(std::vector<std::uint8_t>(lift::d3d9::kCapsBytes + 4u, 0x00));
    const std::uint32_t caps_b = f.placeBytes(std::vector<std::uint8_t>(lift::d3d9::kCapsBytes + 4u, 0xFF));
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x1Cu, {caps_a}), 0u);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x1Cu, {caps_b}), 0u);
    LIFT_CHECK(f.bytesOf(caps_a, lift::d3d9::kCapsBytes) == f.bytesOf(caps_b, lift::d3d9::kCapsBytes));
    // ★ 引擎唯一读的两个字段（实测偏移 +0x58 / +0x5C）
    const std::uint32_t max_w = f.m.mem().rd32(caps_a + lift::d3d9::kCapsMaxTextureWidth);
    const std::uint32_t max_h = f.m.mem().rd32(caps_a + lift::d3d9::kCapsMaxTextureHeight);
    LIFT_CHECK_NE_U32(max_w, 0u);
    LIFT_CHECK_NE_U32(max_h, 0u);
    LIFT_CHECK_EQ_U32(max_w, lift::d3d9::kMaxTextureWidth);
    LIFT_CHECK_EQ_U32(max_h, lift::d3d9::kMaxTextureHeight);
    // 0xD4 之外的 canary 没被写
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(caps_a + lift::d3d9::kCapsBytes), 0x00000000u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(caps_b + lift::d3d9::kCapsBytes), 0xFFFFFFFFu);

    // #10 CheckDeviceFormat ⇒ S_OK，**并记下问了什么**（诊断环形；⛔ 不是"什么都 S_OK 又没痕迹"）
    lift::d3d9::resetCheckFormatQueries();
    const std::uint32_t hr = f.method(obj, 0x28u, {0u, 1u, lift::d3d9::kFmtX8R8G8B8, 1u, 1u, 21u});
    LIFT_CHECK_EQ_U32(hr, 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::d3d9::checkFormatQueryCount()), 1u);
    lift::d3d9::CheckFormatQuery q;
    LIFT_CHECK(lift::d3d9::checkFormatQueryAt(0u, &q));
    LIFT_CHECK_EQ_U32(q.adapter, 0u);
    LIFT_CHECK_EQ_U32(q.device_type, 1u);
    LIFT_CHECK_EQ_U32(q.adapter_format, lift::d3d9::kFmtX8R8G8B8);
    LIFT_CHECK_EQ_U32(q.usage, 1u);
    LIFT_CHECK_EQ_U32(q.resource_type, 1u);
    LIFT_CHECK_EQ_U32(q.check_format, 21u);

    // 负例：DeviceType 不在 {HAL,REF} ⇒ D3DERR_INVALIDDEVICE（真机失败码，不是静默成功）
    const std::uint32_t out2 = f.placeU32(0xDEADBEEFu);
    const std::uint32_t pp2 = f.placeBytes(std::vector<std::uint8_t>(lift::d3d9::kPresentParamsBytes, 0));
    LIFT_CHECK_EQ_U32(f.method(obj, 0x40u, {0u, 3u, 0u, 0x44u, pp2, out2}),
                      lift::d3d9::kInvalidDevice);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(out2), 0u);  // 失败时出参被清 0

    // Release 到 0 ⇒ 设备状态块与后缓冲像素一起回收（surface 数回落）
    // ★ 2026-11-05 改：fixed-function 流水线落地后，一台设备占**两块** SurfaceArena 表面
    //   （后缓冲 + 深度缓冲）⇒ 回收后 surface 数少 2（判据仍是"真的回收了"）。
    const std::size_t surfaces_before = f.m.surfaces().surfaceCount();
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x08u, {}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(dev.object));
    LIFT_CHECK(f.m.surfaces().surfaceCount() == surfaces_before - 2);
}

// ---------------------------------------------------------------------------------------
// ④ Clear：真的改后缓冲像素（整块 + D3DRECT）
// ---------------------------------------------------------------------------------------
void caseClear() {
    lift_test::g_current_case = "d3d9/clear";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t obj = f.createDirect3D();
    LIFT_CHECK_NE_U32(obj, 0u);
    if (obj == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(obj, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    // 建好时后缓冲是全 0（SurfaceArena 清零）——先确认这块"画布"起点
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 0, 0), 0u);
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 1279, 719), 0u);

    // 引擎的那一次：Clear(0, NULL, 3, 0, 1.0f, 0) ⇒ 黑色
    const std::uint32_t zero_z = 0x3F800000u;  // 1.0f 的位模式
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0xACu, {0u, 0u, 3u, 0u, zero_z, 0u}), 0u);
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 0, 0), 0u);
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 1279, 719), 0u);

    // 再清成不透明的 0x00123456 ⇒ 每个像素都变成这个值（逐点 + 四角 + 中间）
    const std::uint32_t color = 0x00123456u;
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0xACu, {0u, 0u, 3u, color, zero_z, 0u}), 0u);
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 0, 0), color);
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 1279, 0), color);
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 0, 719), color);
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 640, 360), color);
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 1279, 719), color);

    // 带 D3DRECT 的 Clear：只清矩形内（D3DRECT = 4 个 LONG；右/下开区间）
    const std::uint32_t rect[4] = {2u, 3u, 10u, 12u};
    const std::uint32_t rect_ea = f.placeData(rect, sizeof(rect));
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0xACu, {1u, rect_ea, 1u, 0x00FFFFFFu, zero_z, 0u}), 0u);
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 2, 3), 0x00FFFFFFu);
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 9, 11), 0x00FFFFFFu);
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 10, 11), color);   // 右边界开区间
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 9, 12), color);    // 下边界开区间
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 1, 3), color);
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 2, 2), color);

    // 未定义的 flag 位 ⇒ D3DERR_INVALIDCALL（不静默接受）
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0xACu, {0u, 0u, 0x10u, color, zero_z, 0u}),
                      lift::d3d9::kInvalidCall);
    // 只有 TARGET 位才清颜色
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0xACu, {0u, 0u, 0u, 0x00ABCDEFu, zero_z, 0u}), 0u);
    LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, 640, 360), color);

    // 计数器（guest 状态块里）
    lift::d3d9::DeviceInfo after;
    LIFT_CHECK(lift::d3d9::deviceInfoOfState(f.m.cpu(), dev.state, &after, nullptr));
    LIFT_CHECK_EQ_U32(after.clears, 4u);

    // 16bpp 后缓冲：Clear 必须按 **R5G6B5** 换算（⛔ 不是把 32 位颜色硬塞进去）
    Fixture::Device dev16;
    LIFT_CHECK(f.createDevice(obj, 1u, 0x44u, 640, 480, lift::d3d9::kFmtR5G6B5, &dev16));
    if (dev16.object != 0) {
        LIFT_CHECK_EQ_U32(dev16.info.bpp, 16u);
        LIFT_CHECK_EQ_U32(dev16.info.pitch, 640u * 2u);
        LIFT_CHECK_EQ_U32(f.method(dev16.object, 0xACu, {0u, 0u, 1u, 0x00FF0000u, zero_z, 0u}), 0u);
        const std::uint32_t px = f.m.mem().rd16(dev16.info.arena + 0u);
        LIFT_CHECK_EQ_U32(px, 0xF800u);  // 纯红 = 565 的 0xF800
        const std::uint32_t px2 = f.m.mem().rd16(dev16.info.arena + 639u * 2u + 479u * dev16.info.pitch);
        LIFT_CHECK_EQ_U32(px2, 0xF800u);
    }
}

// ---------------------------------------------------------------------------------------
// ⑤ Present：frameCount == 1、lastHash 与 arena 一致、两台机器同输入 ⇒ 同哈希
// ---------------------------------------------------------------------------------------
struct PresentResult {
    bool ok = false;
    std::uint64_t frames = 0;
    std::uint64_t hash = 0;
    std::uint32_t presents = 0;
};

PresentResult runPresentFixture() {
    PresentResult out;
    Fixture f;
    if (!f.boot()) return out;
    const std::uint32_t obj = f.createDirect3D();
    if (obj == 0) return out;
    Fixture::Device dev;
    if (!f.createDevice(obj, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev)) return out;
    if (dev.object == 0) return out;
    const std::uint32_t zero_z = 0x3F800000u;
    if (f.method(dev.object, 0xACu, {0u, 0u, 3u, 0x00112233u, zero_z, 0u}) != 0u) return out;
    if (f.method(dev.object, 0x44u, {0u, 0u, 0u, 0u}) != 0u) return out;  // Present(NULL,NULL,NULL,NULL)
    const lift::GuestSurface* gs = f.m.surfaces().find(dev.info.arena);
    if (!gs) return out;
    out.frames = f.m.presenter().frameCount();
    out.hash = f.m.presenter().lastHash();
    out.ok = out.hash == f.m.surfaces().hash(*gs);
    lift::d3d9::DeviceInfo after;
    if (lift::d3d9::deviceInfoOfState(f.m.cpu(), dev.state, &after, nullptr)) out.presents = after.presents;
    return out;
}

void casePresent() {
    lift_test::g_current_case = "d3d9/present";
    const PresentResult a = runPresentFixture();
    LIFT_CHECK(a.ok);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(a.frames), 1u);
    LIFT_CHECK_NE_U32(static_cast<std::uint32_t>(a.hash), 0u);
    LIFT_CHECK_EQ_U32(a.presents, 1u);
    // ★ 确定性：两台新机器、同一串输入 ⇒ 同一帧哈希
    const PresentResult b = runPresentFixture();
    LIFT_CHECK(b.ok);
    LIFT_CHECK(a.frames == b.frames);
    LIFT_CHECK(a.hash == b.hash);
}

// ---------------------------------------------------------------------------------------
// ⑥ GetBackBuffer + SetRenderTarget：后缓冲 surface 可取得，SetRenderTarget(0) 恢复后缓冲
// ---------------------------------------------------------------------------------------
void caseGetBackBufferAndRenderTarget() {
    lift_test::g_current_case = "d3d9/backbuffer-and-render-target";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t obj = f.createDirect3D();
    LIFT_CHECK_NE_U32(obj, 0u);
    if (obj == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(obj, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    const std::uint32_t out = f.placeU32(0xDEADBEEFu);
    // GetBackBuffer(this, SwapChain=0, BackBuffer=0, Type=MONO(0), ppSurface)
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x48u, {0u, 0u, 0u, out}), 0u);
    const std::uint32_t surf = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(surf, 0u);
    LIFT_CHECK_NE_U32(surf, 0xDEADBEEFu);

    // 后缓冲 surface 的 GetDevice / GetDesc：设备对象与宽高都与设备一致
    const std::uint32_t dev_out = f.placeU32(0);
    LIFT_CHECK_EQ_U32(f.method(surf, 0x0Cu, {dev_out}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(dev_out), dev.object);
    const std::uint32_t desc = f.placeBytes(std::vector<std::uint8_t>(32u, 0));
    LIFT_CHECK_EQ_U32(f.method(surf, 0x30u, {desc}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(desc + 0x18u), 1280u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(desc + 0x1Cu), 720u);

    // SetRenderTarget(backbuffer_surface, 0) 成功；再恢复到后缓冲（0,0）
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x94u, {surf, 0u}), 0u);
    lift::d3d9::PipelineInfo pi;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfState(f.m.cpu(), dev.info.pipeline, &pi, nullptr));
    LIFT_CHECK_EQ_U32(pi.color_arena, dev.info.arena);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x94u, {0u, 0u}), 0u);
    LIFT_CHECK(lift::d3d9::pipelineInfoOfState(f.m.cpu(), dev.info.pipeline, &pi, nullptr));
    LIFT_CHECK_EQ_U32(pi.color_arena, 0u);
    LIFT_CHECK_EQ_U32(pi.color_surface, 0u);

    // 不是 surface 的对象 ⇒ D3DERR_INVALIDCALL（⛔ 不静默接受）
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x94u, {dev.object, 0u}), 0x8876086Cu);
    LIFT_CHECK_EQ_U32(f.method(surf, 0x08u, {}), 0u);  // Release
    LIFT_CHECK(!f.m.cpu().com->find(surf));
}

// ---------------------------------------------------------------------------------------
// ⑦ 未实现的槽 ⇒ loud trap（机器停 + 报 iface/偏移），⛔ 不许返回 0 继续跑
// ---------------------------------------------------------------------------------------
void caseUnimplementedSlotTraps() {
    // (a) IDirect3DDevice9 的未实现槽 #3 TestCooperativeLevel（+0x0C）
    // ★ 2026-11-05 改：原先探的是 #57 SetRenderState（+0xE4），它已被 M2 的 fixed-function
    //   流水线实现（见 tests/d3d9_pipeline.cpp）。本用例的判据是"**没填的槽** = loud trap"，
    //   所以探针换到仍然没实现的一槽；判据本身不变（⛔ 不许返回 0 继续跑）。
    {
        lift_test::g_current_case = "d3d9/trap-device-slot";
        Fixture f;
        LIFT_CHECK(f.boot());
        const std::uint32_t obj = f.createDirect3D();
        LIFT_CHECK_NE_U32(obj, 0u);
        if (obj == 0) return;
        Fixture::Device dev;
        LIFT_CHECK(f.createDevice(obj, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
        if (dev.object == 0) return;
        // vtable 里那条槽指向我们发的唯一 stub ⇒ 它的注册名就带 iface + 偏移（trap 的日志输入）
        const std::uint32_t vtbl = f.m.mem().rd32(dev.object);
        const std::uint32_t stub = f.m.mem().rd32(vtbl + 0x0Cu);
        const lift::ShimDef* def = f.m.shims().byStubEa(stub);
        LIFT_CHECK(def != nullptr);
        if (def) {
            LIFT_CHECK(def->name.find("IDirect3DDevice9") != std::string::npos);
            LIFT_CHECK(def->name.find("+0xC") != std::string::npos);
            // 没填的槽 ⇒ thunk 是 comUnsupported（loud trap），⛔ 不是"返回 0 的实现"
            LIFT_CHECK(def->fn == &lift::comUnsupported);
        }
        Asm a;
        a.movEaxImm(vtbl);
        a.pushImm(dev.object);
        a.callMemEaxDisp8(0x0Cu);
        a.ret();
        lift::RunResult r;
        f.runAsmRaw(a, &r);
        // ★ 机器必须**停**：不是跑到哨兵（UntilEa），也不是继续用 EAX=0 往下走
        LIFT_CHECK(r.reason != lift::StopReason::UntilEa);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.reason),
                          static_cast<std::uint32_t>(lift::StopReason::Stopped));
        LIFT_CHECK(f.m.cpu().running == false);
        LIFT_CHECK(std::string(f.m.cpu().stop_why ? f.m.cpu().stop_why : "") == "unimplemented COM vtable slot");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(f.m.cpu().shim.unimplemented_calls), 1u);
        LIFT_CHECK(r.detail.find("IDirect3DDevice9") != std::string::npos);
        LIFT_CHECK(r.detail.find("+0xC") != std::string::npos);
    }
    // (b) IDirect3D9 的未实现槽 #14 GetDeviceCaps（+0x38，静态 caps 查询——本线不实现）
    {
        lift_test::g_current_case = "d3d9/trap-d3d9-slot";
        Fixture f;
        LIFT_CHECK(f.boot());
        const std::uint32_t obj = f.createDirect3D();
        LIFT_CHECK_NE_U32(obj, 0u);
        if (obj == 0) return;
        Asm a;
        a.movEaxImm(f.m.mem().rd32(obj));
        a.pushImm(0u);  // caps*
        a.pushImm(1u);  // DeviceType
        a.pushImm(0u);  // Adapter
        a.pushImm(obj);
        a.callMemEaxDisp8(0x38u);
        a.ret();
        lift::RunResult r;
        f.runAsmRaw(a, &r);
        LIFT_CHECK(r.reason != lift::StopReason::UntilEa);
        LIFT_CHECK(f.m.cpu().running == false);
        LIFT_CHECK(r.detail.find("IDirect3D9") != std::string::npos);
        LIFT_CHECK(r.detail.find("+0x38") != std::string::npos);
    }
}

}  // namespace

int main() {
    using namespace lift_test;
    try {
        caseCreateAndQueryInterface();
        caseAdapterDisplayMode();
        caseCreateDeviceAndCaps();
        caseClear();
        casePresent();
        caseGetBackBufferAndRenderTarget();
        caseUnimplementedSlotTraps();
        return finish("d3d9");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "DIAG uncaught std::exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "DIAG uncaught non-std exception\n");
        return 1;
    }
}
