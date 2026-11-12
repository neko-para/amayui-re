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
//   ⑦ CreateVertexBuffer 返回对象，SetStreamSource 接收同一对象（内部存**对象 EA + 绑定时代际**， //      Draw 时按身份校验换算成状态块）。
//   ⑦b CreateVertexDeclaration / IDirect3DVertexDeclaration9（Live2D 的 3 元素声明）：GetDeclaration
//      逐字节回读、Set/Get 往返、非本对象拒绝、SetFVF 清声明、Release 回堆基线。
//   ⑧ GetBackBuffer / SetRenderTarget / 后缓冲 surface 的 GetDesc 可回查；`--dump-frame` 的
//      `{frame}` 模板真的写出 BMP（头两字节 BM）。
//   ⑨ 未实现的槽 ⇒ **loud trap**（机器停 + `stop_why`）且日志输入里带 iface + 偏移（`r.detail` 里就有）
//      —— ⛔ 不许返回 0 继续跑。
//
// ★ 所有方法调用都从 **guest 代码** 经 vtable 调（`mov eax,[vtbl]; push …; call [eax+off]`），
//   测的是"guest 可见的对象"，不是宿主直接调函数。
// ★ 这些用例**会红**（变异验证见交付说明）：`Clear` 不清 ⇒ ⑤ 红；`GetAdapterDisplayMode` 报 0×0 ⇒ ② 红；
//   `fillCaps` 少写 MaxTexture* ⇒ ④ 红；把未实现槽改成"返回 0" ⇒ ⑦ 红。
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
// D3DXCreateTexture 的固定桩地址（与 tests/d3dx9.cpp / d3d9_pipeline.cpp 同一约定）
constexpr std::uint32_t kProcD3DXCreateTexture = 0x7FF81040u;
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
    // 16 B 的 RECT（4×i32；右/下开区间）
    std::uint32_t placeRect(std::uint32_t left, std::uint32_t top, std::uint32_t right, std::uint32_t bottom) {
        std::vector<std::uint8_t> v(16, 0);
        const std::uint32_t fields[4] = {left, top, right, bottom};
        for (std::size_t i = 0; i < 4; ++i) {
            for (int k = 0; k < 4; ++k)
                v[i * 4 + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>((fields[i] >> (8 * k)) & 0xFF);
        }
        return placeBytes(v);
    }

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
        // ★ 白名单 = "调用返回了 EAX"（不是"成功"）；新增原因必须在内，见 tests/d3dx9.cpp 同款注释。
        return r.reason == lift::StopReason::Stopped || r.reason == lift::StopReason::Unimplemented ||
                       r.reason == lift::StopReason::Trap || r.reason == lift::StopReason::ResourceExhausted
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

    // `present_flags` / `multisample_type` 缺省 = 真机实测值（Flags=1 = LOCKABLE_BACKBUFFER、MSAA=NONE）；
    // 后缓冲锁的失败面守卫用非缺省值构造"不可锁的后缓冲"。
    bool createDevice(std::uint32_t d3d9_obj, std::uint32_t device_type, std::uint32_t behavior, int width,
                      int height, std::uint32_t format, Device* out,
                      std::uint32_t present_flags = lift::d3d9::kPresentFlagLockableBackbuffer,
                      std::uint32_t multisample_type = 0u) {
        std::vector<std::uint8_t> pp(lift::d3d9::kPresentParamsBytes, 0);
        auto wr = [&pp](std::uint32_t off, std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
                pp[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
        };
        wr(lift::d3d9::kPpBackBufferWidth, static_cast<std::uint32_t>(width));
        wr(lift::d3d9::kPpBackBufferHeight, static_cast<std::uint32_t>(height));
        wr(lift::d3d9::kPpBackBufferFormat, format);
        wr(lift::d3d9::kPpBackBufferCount, 1u);
        wr(lift::d3d9::kPpMultiSampleType, multisample_type);
        wr(lift::d3d9::kPpSwapEffect, 1u);            // D3DSWAPEFFECT_DISCARD
        wr(lift::d3d9::kPpHDeviceWindow, 0x00001234u);
        wr(lift::d3d9::kPpWindowed, 1u);
        wr(lift::d3d9::kPpEnableAutoDepthStencil, 1u);
        wr(lift::d3d9::kPpAutoDepthStencilFormat, 75u);  // D3DFMT_D24S8
        wr(lift::d3d9::kPpFlags, present_flags);
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
    // ★ 真值判定（RType/Usage 的组合、DXT1 / D24S8 / 未知格式要答 no）见下面的
    //   `caseCheckDeviceFormatTruthful`；这里只要求"支持集内的组合仍然 S_OK + 记录字段对"。
    lift::d3d9::resetCheckFormatQueries();
    const std::uint32_t hr = f.method(obj, 0x28u, {0u, 1u, lift::d3d9::kFmtX8R8G8B8, 0u, 3u, 21u});
    LIFT_CHECK_EQ_U32(hr, 0u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::d3d9::checkFormatQueryCount()), 1u);
    lift::d3d9::CheckFormatQuery q;
    LIFT_CHECK(lift::d3d9::checkFormatQueryAt(0u, &q));
    LIFT_CHECK_EQ_U32(q.adapter, 0u);
    LIFT_CHECK_EQ_U32(q.device_type, 1u);
    LIFT_CHECK_EQ_U32(q.adapter_format, lift::d3d9::kFmtX8R8G8B8);
    LIFT_CHECK_EQ_U32(q.usage, 0u);
    LIFT_CHECK_EQ_U32(q.resource_type, 3u);
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
// ③b CheckDeviceFormat 必须**答真话**
//
// 判据（红了意味着什么）：
//   · 引擎 boot 那一次真实查询（`.lst` 0x49918D：Adapter=0 / RType=3(TEXTURE) / Usage=0 /
//     CheckFormat=0x16=22=X8R8G8B8）**仍然** S_OK —— 修法不许把真查询答成"不支持"；
//   · 本线真的不支持的组合**必须** `D3DERR_NOTAVAILABLE`（修前恒 S_OK ⇒ 假支持）：
//     ① DXT1(0x31545844)：压缩格式，不在纹理支持集（`formatForD3dFormat` 不认）；
//     ② D24S8(75) 当**纹理**：它是深度格式（本线只把它当深度面，见 ④）；
//     ③ `D3DUSAGE_RENDERTARGET` 配 DXT1 / 16bpp —— 本线 RT 只在 32bpp 上工作；
//   · 深度侧只认**真的建的** D24S8(75)：D16(80)（16bpp 深度面本线不建）/ Usage=0 都不支持；
//   · 其它 RType / 联合 Usage 位 / 未知格式(0) 一律不支持（⛔ 不"放行未知"）；
//   · 不支持**仍要留诊断**（`recordCheckFormatQuery` 不因为"答了 no"就不记）。
// ---------------------------------------------------------------------------------------
void caseCheckDeviceFormatTruthful() {
    lift_test::g_current_case = "d3d9/checkdeviceformat-truthful";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t obj = f.createDirect3D();
    LIFT_CHECK_NE_U32(obj, 0u);
    if (obj == 0) return;

    constexpr std::uint32_t kDxt1 = 0x31545844u;  // D3DFMT_DXT1（FOURCC 'DXT1'）
    constexpr std::uint32_t kD16 = 80u;           // D3DFMT_D16
    const std::uint32_t rt = lift::d3d9::kUsageRenderTarget;
    const std::uint32_t ds = lift::d3d9::kUsageDepthStencil;
    const std::uint32_t tex = lift::d3d9::kRTypeTexture;
    const std::uint32_t surf = lift::d3d9::kRTypeSurface;
    // 实参序 = (Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat)；AdapterFormat 固定 X8R8G8B8。
    auto check = [&](std::uint32_t usage, std::uint32_t rtype, std::uint32_t format) {
        return f.method(obj, 0x28u, {0u, 1u, lift::d3d9::kFmtX8R8G8B8, usage, rtype, format});
    };

    // ① 引擎那一次真实查询（0x49918D）⇒ 必须仍然 S_OK
    LIFT_CHECK_EQ_U32(check(0u, tex, lift::d3d9::kFmtX8R8G8B8), lift::d3d9::kOk);
    // ② TEXTURE + Usage=0：支持集内的（21/22）通过；DXT1 / D24S8 / 未知(0) 都必须答 no
    LIFT_CHECK_EQ_U32(check(0u, tex, lift::d3d9::kFmtA8R8G8B8), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(check(0u, tex, kDxt1), lift::d3d9::kNotAvailable);
    LIFT_CHECK_EQ_U32(check(0u, tex, lift::d3d9::kFmtD24S8), lift::d3d9::kNotAvailable);
    LIFT_CHECK_EQ_U32(check(0u, tex, lift::d3d9::kFmtUnknown), lift::d3d9::kNotAvailable);
    // ③ TEXTURE + RENDERTARGET：32bpp（21/22）通过；DXT1 / 16bpp(23) 不支持
    LIFT_CHECK_EQ_U32(check(rt, tex, lift::d3d9::kFmtX8R8G8B8), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(check(rt, tex, lift::d3d9::kFmtA8R8G8B8), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(check(rt, tex, kDxt1), lift::d3d9::kNotAvailable);
    LIFT_CHECK_EQ_U32(check(rt, tex, lift::d3d9::kFmtR5G6B5), lift::d3d9::kNotAvailable);
    // ④ SURFACE + DEPTHSTENCIL：只认真的建的 D24S8；D16 / 纹理格式都不支持
    LIFT_CHECK_EQ_U32(check(ds, surf, lift::d3d9::kFmtD24S8), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(check(ds, surf, kD16), lift::d3d9::kNotAvailable);
    LIFT_CHECK_EQ_U32(check(ds, surf, lift::d3d9::kFmtX8R8G8B8), lift::d3d9::kNotAvailable);
    // ⑤ D24S8 当**纹理**问（含 RT）⇒ 不支持（与 ④ 成对：同一格式的两种 RType 不能都答 yes）
    LIFT_CHECK_EQ_U32(check(rt, tex, lift::d3d9::kFmtD24S8), lift::d3d9::kNotAvailable);
    // ⑥ SURFACE 不带 DEPTHSTENCIL / 其它 RType / 联合 Usage 位 ⇒ 不支持
    LIFT_CHECK_EQ_U32(check(0u, surf, lift::d3d9::kFmtX8R8G8B8), lift::d3d9::kNotAvailable);
    LIFT_CHECK_EQ_U32(check(0u, 4u, lift::d3d9::kFmtX8R8G8B8), lift::d3d9::kNotAvailable);
    LIFT_CHECK_EQ_U32(check(rt | ds, tex, lift::d3d9::kFmtX8R8G8B8), lift::d3d9::kNotAvailable);

    // ⑦ 诊断**没被删**：每次调用留一条（答 no 的也记），字段就是 guest 传的那一份
    lift::d3d9::resetCheckFormatQueries();
    LIFT_CHECK_EQ_U32(check(0u, tex, kDxt1), lift::d3d9::kNotAvailable);
    LIFT_CHECK_EQ_U32(check(rt | ds, tex, lift::d3d9::kFmtX8R8G8B8), lift::d3d9::kNotAvailable);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(lift::d3d9::checkFormatQueryCount()), 2u);
    lift::d3d9::CheckFormatQuery q;
    LIFT_CHECK(lift::d3d9::checkFormatQueryAt(0u, &q));  // 最新 = 上面那条联合 Usage
    LIFT_CHECK_EQ_U32(q.adapter, 0u);
    LIFT_CHECK_EQ_U32(q.device_type, 1u);
    LIFT_CHECK_EQ_U32(q.adapter_format, lift::d3d9::kFmtX8R8G8B8);
    LIFT_CHECK_EQ_U32(q.usage, rt | ds);
    LIFT_CHECK_EQ_U32(q.resource_type, tex);
    LIFT_CHECK_EQ_U32(q.check_format, lift::d3d9::kFmtX8R8G8B8);
    LIFT_CHECK(lift::d3d9::checkFormatQueryAt(1u, &q));  // 上一条 = DXT1 / Usage=0
    LIFT_CHECK_EQ_U32(q.usage, 0u);
    LIFT_CHECK_EQ_U32(q.resource_type, tex);
    LIFT_CHECK_EQ_U32(q.check_format, kDxt1);
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
// ⑥ 帧导出：`{frame}` 替换成 4 位帧号，BMP 真落盘（头两字节 BM）
// ---------------------------------------------------------------------------------------
void caseFrameDump() {
    lift_test::g_current_case = "d3d9/frame-dump";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t obj = f.createDirect3D();
    LIFT_CHECK_NE_U32(obj, 0u);
    if (obj == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(obj, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    const char* path = "lift-frame-{frame}.bmp";
    std::remove("lift-frame-0001.bmp");
    f.m.cpu().shim.frame_dump = path;
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x44u, {0u, 0u, 0u, 0u}), 0u);  // Present
    std::FILE* fp = std::fopen("lift-frame-0001.bmp", "rb");
    LIFT_CHECK(fp != nullptr);
    if (fp) {
        unsigned char hdr[2] = {0, 0};
        const std::size_t n = std::fread(hdr, 1, sizeof(hdr), fp);
        std::fclose(fp);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(n), 2u);
        LIFT_CHECK_EQ_U32(hdr[0], 'B');
        LIFT_CHECK_EQ_U32(hdr[1], 'M');
    }
    std::remove("lift-frame-0001.bmp");
    f.m.cpu().shim.frame_dump.clear();
}

// ---------------------------------------------------------------------------------------
// ⑦ CreateVertexBuffer 返回对象，SetStreamSource 接收的也是**对象**（内部存对象 EA + 绑定时代际）
// ---------------------------------------------------------------------------------------
void caseVertexStreamBinding() {
    lift_test::g_current_case = "d3d9/vertex-stream-binding";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t obj = f.createDirect3D();
    LIFT_CHECK_NE_U32(obj, 0u);
    if (obj == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(obj, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    const std::uint32_t out = f.placeU32(0xDEADBEEFu);
    // CreateVertexBuffer(this, Length, Usage, FVF, Pool, ppVB)
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x68u, {12u, 8u, 0x0044u, 1u, out, 0u}), 0u);
    const std::uint32_t vb = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(vb, 0u);
    LIFT_CHECK_NE_U32(vb, 0xDEADBEEFu);
    // 旧实现把对象 EA 当状态块 EA ⇒ 这里会 D3DERR_INVALIDCALL；修复后应 S_OK。
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x190u, {0u, vb, 0u, 12u}), 0u);
    // 非本模块对象仍必须拒绝
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x190u, {0u, 0xDEADBEEFu, 0u, 12u}), 0x8876086Cu);
    // ★ 期望值由 0 改成 **1**：段 2（`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 的 ③）之后**设备持绑定引用**
    //   （与 `SetTexture` 的既有口径一致）⇒ guest 放掉自己那份之后还剩设备那一份。
    //   ⛔ 这条不是"放宽断言"：它钉的正是"绑定期对象不能死"（修前返回 0 = 设备没持，合法时序会崩）。
    LIFT_CHECK_EQ_U32(f.method(vb, 0x08u, {}), 1u);  // Release 自己那份 ⇒ 设备那份还在
    // 解绑 ⇒ 设备那份由流水线还回去 ⇒ 对象与 'VB09' 状态块一起回收（⛔ 不是靠测试补 Release）
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x190u, {0u, 0u, 0u, 0u}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(vb));
}

// ---------------------------------------------------------------------------------------
// ⑦b CreateVertexDeclaration / IDirect3DVertexDeclaration9（Live2D 在 `sub_4C1BE5` 用）
//   声明 = 元素 0: Stream=0 Offset=0 Type=FLOAT3(2) Usage=POSITION(0)
//          元素 1: Stream=1 Offset=0 Type=FLOAT2(1) Usage=TEXCOORD(5)
//          结束:   Stream=0xFF
// ---------------------------------------------------------------------------------------
void caseVertexDeclaration() {
    lift_test::g_current_case = "d3d9/vertex-declaration";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t obj = f.createDirect3D();
    LIFT_CHECK_NE_U32(obj, 0u);
    if (obj == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(obj, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    std::vector<std::uint8_t> decl(24u, 0u);
    auto set = [&decl](std::size_t off, std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i) decl[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    };
    set(0u, 0u, 2);   // Stream 0
    set(2u, 0u, 2);   // Offset 0
    set(4u, 2u, 1);   // D3DDECLTYPE_FLOAT3
    set(6u, 0u, 1);   // Usage POSITION
    set(8u, 1u, 2);   // Stream 1
    set(10u, 0u, 2);  // Offset 0
    set(12u, 1u, 1);  // D3DDECLTYPE_FLOAT2
    set(14u, 5u, 1);  // Usage TEXCOORD
    set(16u, 0xFFu, 2);  // D3DDECL_END().Stream
    const std::uint32_t decl_ea = f.placeBytes(decl);
    const std::uint32_t out = f.placeU32(0xDEADBEEFu);
    const std::uint32_t heap_before = f.m.heap().bytesInUse();

    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x158u, {decl_ea, out}), 0u);
    const std::uint32_t vdecl = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(vdecl, 0u);
    LIFT_CHECK_NE_U32(vdecl, 0xDEADBEEFu);
    if (vdecl == 0) return;

    // GetDevice(+0x0C)：返回同一台设备，且**借出一份新引用**（`Get*` 家族口径：调用方必须 Release）
    const std::uint32_t dev_ref = f.m.cpu().com->refCount(dev.object);
    const std::uint32_t dev_out = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(vdecl, 0x0Cu, {dev_out}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(dev_out), dev.object);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(dev.object), dev_ref + 1u);
    // 只在"确实借出了那份"时还它：缺陷态（没 AddRef）下再 Release 会把设备降到 0 ⇒
    // 后面全变成"在已释放的设备上调方法"，平台异常会把其余断言一起吞掉（红就看不出来了）。
    if (f.m.cpu().com->refCount(dev.object) == dev_ref + 1u)
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x08u, {}), dev_ref);  // 还掉 GetDevice 借出的那份

    // GetDeclaration(+0x10)：逐字节回读（含结束元素）
    const std::uint32_t copy = f.placeBytes(std::vector<std::uint8_t>(24u, 0xEE));
    LIFT_CHECK_EQ_U32(f.method(vdecl, 0x10u, {copy}), 0u);
    LIFT_CHECK(f.m.mem().rd16(copy + 0u) == 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd16(copy + 2u), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd8(copy + 4u), 2u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd16(copy + 8u), 1u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd8(copy + 12u), 1u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd16(copy + 16u), 0x00FFu);

    // SetVertexDeclaration(+0x15C) / GetVertexDeclaration(+0x160)
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x15Cu, {vdecl}), 0u);
    const std::uint32_t got = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x160u, {got}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(got), vdecl);
    // 非本模块对象必须拒绝
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x15Cu, {0xDEADBEEFu}), 0x8876086Cu);
    // SetFVF 会清掉声明（D3D 二选一）
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x164u, {0x0044u}), 0u);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x160u, {got}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(got), 0u);

    // 上面 `GetVertexDeclaration` **借出过一份**（`Get*` 家族口径）⇒ 先还它，再放自己那份。
    //   ★ 少了"先还借出那份"：这里会 expected 1 got 0、`find(vdecl)` 仍真、堆高出
    //     `'VDCL'` 那一块 —— 那不是缺陷，是测试自己少 Release 一次。
    LIFT_CHECK_EQ_U32(f.method(vdecl, 0x08u, {}), 1u);
    // Release 到 0 ⇒ 对象与声明状态块都回收
    //   ★ 先判活再调：`Fixture::method` 要读对象的 vtable（`rd32(obj)`），对象若已在上一条归零，
    //     这里会 `call [0+8]` 打到未提交页（平台异常 0xC0000005）—— 那会把后面的用例一起吞掉，
    //     红的地方就看不出来了。
    if (f.m.cpu().com->find(vdecl)) LIFT_CHECK_EQ_U32(f.method(vdecl, 0x08u, {}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(vdecl));
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_before);
}

// ---------------------------------------------------------------------------------------
// ⑧ GetBackBuffer + SetRenderTarget：后缓冲 surface 可取得，SetRenderTarget(0) 恢复后缓冲
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
    // ★ `GetDevice` 借出的是**一份新引用**（`IDirect3DResource9` 文档；段 3 的 ①）⇒ +1 后再还回
    //   （纹理表面的同形判据在 `d3dx9/surface-getdevice-refcount`）。
    const std::uint32_t dev_base = f.m.cpu().com ? f.m.cpu().com->refCount(dev.object) : 0u;
    const std::uint32_t dev_out = f.placeU32(0);
    LIFT_CHECK_EQ_U32(f.method(surf, 0x0Cu, {dev_out}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(dev_out), dev.object);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(dev.object), dev_base + 1u);  // ← 修前 = dev_base
    if (f.m.cpu().com->refCount(dev.object) == dev_base + 1u)
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x08u, {}), dev_base);  // 还掉借出的那份
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
// ⑬ 离屏普通表面 + GetRenderTargetData（存档缩略图路径；`.lst` 0x4A0570 / 0x4A05D2）
//
// 判据（红了意味着什么）：
//   ① `CreateOffscreenPlainSurface(+0x90)` 建出**可锁的** IDirect3DSurface9：GetDesc 的
//      宽 / 高 / 格式 / 池（SYSTEMMEM=2）与入参一致；
//   ② 纹理 RT（`GetSurfaceLevel` 的 surface）`Clear` ⇒ `GetRenderTargetData(+0x80)` ⇒ 离屏表面
//      `LockRect(+0x34)` 读到的像素**逐像素等于 Clear 色**（含最后一行，行距按 Pitch）；
//   ③ 尺寸 / 格式不匹配 ⇒ `D3DERR_INVALIDCALL`（⛔ 不隐式缩放 / 格式转换）；
//   ④ 源不是本模块的 surface ⇒ `D3DERR_INVALIDCALL`；
//   ⑤ 共享句柄（pSharedHandle != 0）⇒ `D3DERR_INVALIDCALL`（本线不支持共享表面）。
// ★ 为什么这条是 assets 档之外的纯档判据：真机链里"纹理 RT → 离屏表面 → 锁读像素"就是
//   `sub_4A0430` 的全部 D3D 调用；这里用 guest 代码经 vtable 走同一条，不跑游戏。
std::uint32_t callD3DXCreateTexture(Fixture& f, std::uint32_t device, std::uint32_t w, std::uint32_t h,
                                    std::uint32_t format, std::uint32_t out_ea);

void caseOffscreenSurfaceRenderTargetData() {
    lift_test::g_current_case = "d3d9/offscreen-surface-rt-data";
    Fixture f;
    LIFT_CHECK(f.boot());
    lift::registerD3DX9Shims(f.m.shims());
    lift::ShimDef* dx = f.m.shims().findMutable("d3dx9_43", "D3DXCreateTexture");
    LIFT_CHECK(dx != nullptr);
    if (!dx) return;
    dx->stub_ea = kProcD3DXCreateTexture;
    f.m.cache().installShim(kProcD3DXCreateTexture, dx->id);

    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    // 源：64×32 的 RT 纹理（Usage=1 = D3DUSAGE_RENDERTARGET）→ level 0 surface
    const std::uint32_t tex_out = f.placeU32(0);
    LIFT_CHECK_EQ_U32(callD3DXCreateTexture(f, dev.object, 64u, 32u, lift::d3d9::kFmtX8R8G8B8, tex_out), 0u);
    const std::uint32_t tex = f.m.mem().rd32(tex_out);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    const std::uint32_t src_out = f.placeU32(0);
    LIFT_CHECK_EQ_U32(f.method(tex, 0x48u, {0u, src_out}), 0u);  // GetSurfaceLevel(0, ppSurface)
    const std::uint32_t src = f.m.mem().rd32(src_out);
    LIFT_CHECK_NE_U32(src, 0u);
    if (src == 0) return;
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x94u, {src, 0u}), 0u);  // SetRenderTarget(src, 0)

    // 清成已知颜色（TARGET 位）
    const std::uint32_t color = 0x00123456u;
    const std::uint32_t one_z = 0x3F800000u;  // 1.0f
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0xACu, {0u, 0u, 1u, color, one_z, 0u}), 0u);

    // ① CreateOffscreenPlainSurface(Width, Height, Format, Pool=SYSTEMMEM(2), &ppSurface, NULL)
    const std::uint32_t off_out = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(
        f.method(dev.object, 0x90u, {64u, 32u, lift::d3d9::kFmtX8R8G8B8, 2u, off_out, 0u}), 0u);
    const std::uint32_t offs = f.m.mem().rd32(off_out);
    LIFT_CHECK_NE_U32(offs, 0u);
    LIFT_CHECK_NE_U32(offs, 0xDEADBEEFu);
    if (offs == 0) return;
    const std::uint32_t desc = f.placeBytes(std::vector<std::uint8_t>(32u, 0u));
    LIFT_CHECK_EQ_U32(f.method(offs, 0x30u, {desc}), 0u);  // GetDesc
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(desc + 0x00u), lift::d3d9::kFmtX8R8G8B8);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(desc + 0x0Cu), 2u);  // D3DPOOL_SYSTEMMEM
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(desc + 0x18u), 64u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(desc + 0x1Cu), 32u);

    // ② GetRenderTargetData(src, offs) ⇒ S_OK；锁离屏表面读像素
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x80u, {src, offs}), 0u);
    const std::uint32_t lr = f.placeBytes(std::vector<std::uint8_t>(8u, 0xCC));
    LIFT_CHECK_EQ_U32(f.method(offs, 0x34u, {lr, 0u, 0u}), 0u);  // LockRect(NULL, 0)
    const std::uint32_t pitch = f.m.mem().rd32(lr + 0u);
    const std::uint32_t bits = f.m.mem().rd32(lr + 4u);
    LIFT_CHECK_EQ_U32(pitch, 64u * 4u);
    LIFT_CHECK_NE_U32(bits, 0xCCCCCCCCu);
    if (pitch == 64u * 4u && bits != 0xCCCCCCCCu) {
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(bits + 0u * pitch + 0u), color);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(bits + 0u * pitch + 63u * 4u), color);  // 最后一列
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(bits + 31u * pitch + 0u), color);       // 最后一行
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(bits + 31u * pitch + 63u * 4u), color);
    }
    LIFT_CHECK_EQ_U32(f.method(offs, 0x38u, {}), 0u);  // UnlockRect

    // ②b UpdateSurface：整幅 + 子矩形→目标点（第二个颜色，证明"复制了哪块"）
    const std::uint32_t off2_out = f.placeU32(0);
    LIFT_CHECK_EQ_U32(
        f.method(dev.object, 0x90u, {64u, 32u, lift::d3d9::kFmtX8R8G8B8, 2u, off2_out, 0u}), 0u);
    const std::uint32_t off2 = f.m.mem().rd32(off2_out);
    LIFT_CHECK_NE_U32(off2, 0u);
    if (off2 != 0) {
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x78u, {src, 0u, off2, 0u}), 0u);  // UpdateSurface(全幅)
        const std::uint32_t lr2 = f.placeBytes(std::vector<std::uint8_t>(8u, 0xCC));
        LIFT_CHECK_EQ_U32(f.method(off2, 0x34u, {lr2, 0u, 0u}), 0u);
        const std::uint32_t p2 = f.m.mem().rd32(lr2 + 0u);
        const std::uint32_t b2 = f.m.mem().rd32(lr2 + 4u);
        if (p2 == 64u * 4u && b2 != 0xCCCCCCCCu) {
            LIFT_CHECK_EQ_U32(f.m.mem().rd32(b2 + 31u * p2 + 63u * 4u), color);  // 全幅含最后一行
        }
        LIFT_CHECK_EQ_U32(f.method(off2, 0x38u, {}), 0u);

        const std::uint32_t color2 = 0x00654321u;
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0xACu, {0u, 0u, 1u, color2, one_z, 0u}), 0u);
        const std::uint32_t src_rect = f.placeRect(10u, 5u, 30u, 20u);  // 20×15
        std::vector<std::uint8_t> pt(8u, 0u);
        pt[0] = 2u;  // x = 2
        pt[4] = 3u;  // y = 3
        const std::uint32_t pt_ea = f.placeBytes(pt);
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x78u, {src, src_rect, off2, pt_ea}), 0u);
        LIFT_CHECK_EQ_U32(f.method(off2, 0x34u, {lr2, 0u, 0u}), 0u);
        const std::uint32_t p3 = f.m.mem().rd32(lr2 + 0u);
        const std::uint32_t b3 = f.m.mem().rd32(lr2 + 4u);
        if (p3 == 64u * 4u && b3 != 0xCCCCCCCCu) {
            LIFT_CHECK_EQ_U32(f.m.mem().rd32(b3 + 3u * p3 + 2u * 4u), color2);   // 目标左上
            LIFT_CHECK_EQ_U32(f.m.mem().rd32(b3 + 17u * p3 + 21u * 4u), color2);  // 目标右下
            LIFT_CHECK_EQ_U32(f.m.mem().rd32(b3 + 2u * p3 + 2u * 4u), color);     // 上一行仍是旧色
            LIFT_CHECK_EQ_U32(f.m.mem().rd32(b3 + 17u * p3 + 22u * 4u), color);   // 右边一列仍是旧色
        }
        LIFT_CHECK_EQ_U32(f.method(off2, 0x38u, {}), 0u);

        // 越界源矩形 / 目标放不下 ⇒ D3DERR_INVALIDCALL（⛔ 不隐式裁剪）
        const std::uint32_t bad_src = f.placeRect(10u, 5u, 70u, 20u);
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x78u, {src, bad_src, off2, 0u}), lift::d3d9::kInvalidCall);
        std::vector<std::uint8_t> pt_far(8u, 0u);
        pt_far[0] = 50u;  // x = 50 ⇒ 50 + 64 > 64
        pt_far[4] = 30u;
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x78u, {src, 0u, off2, f.placeBytes(pt_far)}),
                          lift::d3d9::kInvalidCall);
    }

    // ③ 尺寸不匹配 ⇒ D3DERR_INVALIDCALL（不隐式缩放）
    const std::uint32_t small_out = f.placeU32(0);
    LIFT_CHECK_EQ_U32(
        f.method(dev.object, 0x90u, {32u, 32u, lift::d3d9::kFmtX8R8G8B8, 2u, small_out, 0u}), 0u);
    const std::uint32_t small_surf = f.m.mem().rd32(small_out);
    if (small_surf != 0) {
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x80u, {src, small_surf}), lift::d3d9::kInvalidCall);
        // ③b 格式不匹配（同尺寸、16bpp）⇒ D3DERR_INVALIDCALL
        const std::uint32_t fmt_out = f.placeU32(0);
        LIFT_CHECK_EQ_U32(
            f.method(dev.object, 0x90u, {64u, 32u, lift::d3d9::kFmtR5G6B5, 2u, fmt_out, 0u}), 0u);
        const std::uint32_t fmt_surf = f.m.mem().rd32(fmt_out);
        if (fmt_surf != 0) {
            LIFT_CHECK_EQ_U32(f.method(dev.object, 0x80u, {src, fmt_surf}), lift::d3d9::kInvalidCall);
            // UpdateSurface 的格式不同 / 源不是 surface ⇒ D3DERR_INVALIDCALL
            LIFT_CHECK_EQ_U32(f.method(dev.object, 0x78u, {src, 0u, fmt_surf, 0u}),
                              lift::d3d9::kInvalidCall);
            LIFT_CHECK_EQ_U32(f.method(fmt_surf, 0x08u, {}), 0u);
        }
        LIFT_CHECK_EQ_U32(f.method(small_surf, 0x08u, {}), 0u);
    }

    // ④ 源不是本模块的 surface ⇒ D3DERR_INVALIDCALL
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x80u, {dev.object, offs}), lift::d3d9::kInvalidCall);

    // ⑤ 共享表面句柄 ⇒ D3DERR_INVALIDCALL（⛔ 不假装共享成功）
    const std::uint32_t shared_out = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(
        f.method(dev.object, 0x90u, {64u, 32u, lift::d3d9::kFmtX8R8G8B8, 2u, shared_out, 0x1234u}),
        lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(shared_out), 0xDEADBEEFu);  // 失败不写出参

    // 收尾：解绑 RT，再按 refcount 逐份释放
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x94u, {0u, 0u}), 0u);
    if (off2 != 0) LIFT_CHECK_EQ_U32(f.method(off2, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(offs, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(src, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(offs));
    LIFT_CHECK(!f.m.cpu().com->find(src));
    LIFT_CHECK(!f.m.cpu().com->find(tex));
}

// ---------------------------------------------------------------------------------------
// ⑨ 未实现的槽 ⇒ loud trap（机器停 + 报 iface/偏移），⛔ 不许返回 0 继续跑
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
                          static_cast<std::uint32_t>(lift::StopReason::Trap));
        // ★ 已注册的槽走到 loud trap ⇒ **非 ok**（本单之前落 `Stopped`(ok) ⇒ exit 0）。
        LIFT_CHECK(!r.ok());
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

// D3DXCreateTexture(pDevice, Width, Height, MipLevels, Usage, Format, Pool, ppTexture)
// 走 guest 代码 + 已注册的 D3DX 导出桩（stdcall 参数从右到左压栈）。
std::uint32_t callD3DXCreateTexture(Fixture& f, std::uint32_t device, std::uint32_t w, std::uint32_t h,
                                    std::uint32_t format, std::uint32_t out_ea) {
    Asm a;
    a.pushImm(out_ea);  // ppTexture
    a.pushImm(0u);      // Pool = D3DPOOL_DEFAULT
    a.pushImm(format);
    a.pushImm(1u);  // Usage = D3DUSAGE_RENDERTARGET
    a.pushImm(1u);  // MipLevels
    a.pushImm(h);
    a.pushImm(w);
    a.pushImm(device);
    a.callRel(f.code_ea, kProcD3DXCreateTexture);
    a.ret();
    return f.runAsm(a);
}

// 最小合法顶点声明（16 B）：元素 0 = Stream 0 / Offset 0 / FLOAT3(2) / POSITION(0)，紧跟 D3DDECL_END。
std::vector<std::uint8_t> positionOnlyDeclaration() {
    std::vector<std::uint8_t> d(16u, 0u);
    auto set = [&d](std::size_t off, std::uint32_t v, int bytes) {
        for (int i = 0; i < bytes; ++i)
            d[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    };
    set(0u, 0u, 2);    // Stream 0
    set(2u, 0u, 2);    // Offset 0
    set(4u, 2u, 1);    // D3DDECLTYPE_FLOAT3
    set(6u, 0u, 1);    // D3DDECLUSAGE_POSITION
    set(8u, 0xFFu, 2);  // D3DDECL_END().Stream
    return d;
}

// ⑩ 纹理生命周期：**最后一次 Release 是内部裸 release**（设备销毁解绑）时，也必须回收
//    arena 像素块。
//
// 判据（红了意味着什么）：同一台机器上"建设备 → 释放设备"跑两遍；第二遍中间多一张
//   **已从采样器解绑**的 1280×1152 纹理（= 复现里分配失败的那个尺寸 5,898,240 B）。
//   两遍结束后的 arena 必须**逐字节相同**（surfaceCount / bytesInUse）。
//   ★ 修前：纹理对象由 `ComRegistry::release` 到 0 时只 free vtable+obj（无析构）⇒
//     像素块留在 arena，第二遍比第一遍多 5,898,248 B、surfaceCount 多 1。
//   ★ 这里刻意让**不是** iface Release shim 的那次 release 当"最后一根"：
//     `SetTexture` AddRef（1→2）→ guest `Release`（2→1）→ 设备销毁里裸 `cpu.com->release`（1→0）。
//     ⛔ 别改成"只走 shim"的写法，那测不到所有权挂在调用点上的那个洞。
void caseTextureBoundThenGuestRelease() {
    lift_test::g_current_case = "d3d9/texture-bound-then-guest-release";
    Fixture f;
    LIFT_CHECK(f.boot());
    // d3dx9 的导出登记进同一份 ShimRegistry（本用例自己用一个 Machine，不影响其它用例）
    lift::registerD3DX9Shims(f.m.shims());
    lift::ShimDef* dx = f.m.shims().findMutable("d3dx9_43", "D3DXCreateTexture");
    LIFT_CHECK(dx != nullptr);
    if (!dx) return;
    dx->stub_ea = kProcD3DXCreateTexture;
    f.m.cache().installShim(kProcD3DXCreateTexture, dx->id);

    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;

    // 基线 = **还没建设备**的空 arena（设备销毁后必须回到这里）
    const std::size_t surfaces_base = f.m.surfaces().surfaceCount();
    const std::uint32_t bytes_base = f.m.surfaces().bytesInUse();

    // ---- 第 1 遍：只有设备（后缓冲 + 深度）----
    Fixture::Device dev1;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev1));
    if (dev1.object == 0) return;
    LIFT_CHECK_EQ_U32(f.method(dev1.object, 0x08u, {}), 0u);
    const std::size_t surfaces_no_tex = f.m.surfaces().surfaceCount();
    const std::uint32_t bytes_no_tex = f.m.surfaces().bytesInUse();

    // ---- 第 2 遍：设备 + 纹理 ----
    Fixture::Device dev2;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev2));
    if (dev2.object == 0) return;
    const std::size_t surfaces_before_tex = f.m.surfaces().surfaceCount();
    const std::uint32_t bytes_before_tex = f.m.surfaces().bytesInUse();
    const std::uint32_t out_ea = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(callD3DXCreateTexture(f, dev2.object, 1280u, 1152u, lift::d3d9::kFmtA8R8G8B8, out_ea),
                      0u);
    const std::uint32_t tex = f.m.mem().rd32(out_ea);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    LIFT_CHECK_EQ_U32(f.m.surfaces().surfaceCount(), surfaces_before_tex + 1);
    LIFT_CHECK(f.m.surfaces().bytesInUse() >= bytes_before_tex + 5898240u);
    // 绑到采样器 ⇒ AddRef（1→2）；guest 自己 Release（2→1）
    LIFT_CHECK_EQ_U32(f.method(dev2.object, 0x104u, {0u, tex}), 0u);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(tex), 2u);
    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 1u);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(tex), 1u);
    // 设备销毁 ⇒ 流水线 destroyForDevice 做**最后一次** release（1→0，内部裸 release）
    LIFT_CHECK_EQ_U32(f.method(dev2.object, 0x08u, {}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(tex));
    // ★ 判据：像素块必须跟着对象一起回收 ⇒ 两遍之后完全一致，且回到空 arena 基线
    LIFT_CHECK_EQ_U32(f.m.surfaces().surfaceCount(), surfaces_no_tex);
    LIFT_CHECK_EQ_U32(f.m.surfaces().bytesInUse(), bytes_no_tex);
    LIFT_CHECK_EQ_U32(f.m.surfaces().surfaceCount(), surfaces_base);
    LIFT_CHECK_EQ_U32(f.m.surfaces().bytesInUse(), bytes_base);
}

// ⑪ 回收资格与 `activeMachine()` 解耦：**另一台 Machine 抢走 g_active** 之后，
//    这台 cpu 的纹理 / 设备销毁仍必须把 arena 像素块与状态块收回去。
//
// 判据（红了意味着什么）：
//   `destroyTextureState` / `destroyDeviceState`（含流水线的 depth）以前是"**先清 magic 再判**
//   `activeMachine()==这台 cpu 的 Machine`"⇒ 判据不成立时 magic 已经没了、状态块已经 free 了，
//   像素块**再也没有第二次回收入口**（本单 REQ-01M6X4MSCMMFEXYXCSK6V61QR2）。
//   ★ 这里刻意造出"判据不成立"的局面（不是绕过它）：
//     Machine A 建 d3d9 / 设备 / 纹理 → Machine B `initBare`（`Machine::init` 会把 g_active 改成 B）
//     → 用 **A 的 cpu** 走销毁路径。
//   ⇒ 修后：纹理释放后 arena 的 surfaceCount / bytesInUse 与 guest 堆 bytesInUse 都回到
//     "只有设备"的基线；设备再销毁回到空 arena 基线。
//   ★ 修前：**arena 的 surfaceCount / bytesInUse 停在高位**（纹理 5,898,240 B 的像素块 +
//     后缓冲 + 深度永不回收）；两条 `heap().bytesInUse()` 断言修前**也能过** —— 状态块无论如何
//     都会被 free，它们只防"回收了像素却漏了状态块"的回归。这正是"④⑥ 干了、⑤ 没干"的证据。
void caseArenaReclaimUnderOtherMachine() {
    lift_test::g_current_case = "d3d9/arena-reclaim-under-other-machine";
    Fixture f;
    LIFT_CHECK(f.boot());
    lift::registerD3DX9Shims(f.m.shims());
    lift::ShimDef* dx = f.m.shims().findMutable("d3dx9_43", "D3DXCreateTexture");
    LIFT_CHECK(dx != nullptr);
    if (!dx) return;
    dx->stub_ea = kProcD3DXCreateTexture;
    f.m.cache().installShim(kProcD3DXCreateTexture, dx->id);

    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;

    // 空 arena 基线（设备销毁后必须回到这里）
    const std::size_t surfaces_base = f.m.surfaces().surfaceCount();
    const std::uint32_t bytes_base = f.m.surfaces().bytesInUse();
    const std::uint32_t heap_base = f.m.heap().bytesInUse();

    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;
    // "设备还在"的基线：纹理释放后 arena / guest 堆都必须回到这里
    const std::size_t surfaces_dev = f.m.surfaces().surfaceCount();
    const std::uint32_t bytes_dev = f.m.surfaces().bytesInUse();
    const std::uint32_t heap_dev = f.m.heap().bytesInUse();
    LIFT_CHECK(surfaces_dev > surfaces_base);  // 后缓冲 + 深度
    LIFT_CHECK(heap_dev > heap_base);          // 设备 / 流水线状态块

    const std::uint32_t out_ea = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(callD3DXCreateTexture(f, dev.object, 1280u, 1152u, lift::d3d9::kFmtA8R8G8B8, out_ea), 0u);
    const std::uint32_t tex = f.m.mem().rd32(out_ea);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    LIFT_CHECK_EQ_U32(f.m.surfaces().surfaceCount(), surfaces_dev + 1);
    LIFT_CHECK(f.m.surfaces().bytesInUse() >= bytes_dev + 1280u * 1152u * 4u);
    LIFT_CHECK(f.m.heap().bytesInUse() > heap_dev);  // 纹理对象 + 状态块

    // ★ 第二台 Machine 抢走 g_active —— 这就是本单"Machine 判据不成立"的那个局面
    lift::Machine other;
    std::string other_err;
    LIFT_CHECK(other.initBare(&other_err));
    LIFT_CHECK(lift::activeMachine() == &other);

    // 用 **A 的 cpu** 释放纹理（最后一次 release ⇒ ComRegistry 的析构钩子 destroyTextureState）
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(tex), 1u);
    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(tex));
    // ★ 判据：纹理的 arena 像素块 + 状态块都必须真的回收（回到"只有设备"的基线）
    LIFT_CHECK_EQ_U32(f.m.surfaces().surfaceCount(), surfaces_dev);
    LIFT_CHECK_EQ_U32(f.m.surfaces().bytesInUse(), bytes_dev);
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_dev);

    // 兄弟路径：设备状态块同样是"先清后判" ⇒ 后缓冲 / 深度 / 状态块也都必须回收
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.m.surfaces().surfaceCount(), surfaces_base);
    LIFT_CHECK_EQ_U32(f.m.surfaces().bytesInUse(), bytes_base);
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_base);
}

// ---------------------------------------------------------------------------------------
// ⑫ 后缓冲 surface 的 LockRect / UnlockRect（D3D9 契约；真机 `0x4A4F8B` 调用点）
//
// 判据（红了意味着什么）：
//   ① `Pitch` = **整幅**行距、`pBits` 偏到**矩形左上**（真机逐行 memcpy 的源行距就是 Pitch，
//      `0x4A5018 add edi,[ebp+var_44]`；写错 ⇒ guest 备份图错行）；
//   ② 子矩形**不改** Pitch；③ 写 `pBits` 真的穿透到后缓冲像素（⛔ 不是假指针）；
//   ④ 已锁再锁 / 未锁就解锁 / 越界 / DISCARD / 无 LOCKABLE / MSAA ⇒ `D3DERR_INVALIDCALL` 且**不占锁**；
//   ⑤ 解锁后可再锁（`rect=NULL` ⇒ `pBits` = arena 基址）；
//   ⑥ 这条路**不再 trap**（修前 `unimplemented_calls` = 1 ⇒ 本用例当场红）。
// ★ 参数顺序：`IDirect3DSurface9::LockRect(this, pLockedRect, pRect, Flags)`；`Fixture::method` 的
//   `args[0]` 是第一个实参（逆序压栈）⇒ `{lr_ea, rect_ea, flags}`（⛔ 不是 `{rect, flags, lr}`）。
void caseBackBufferLockRect() {
    lift_test::g_current_case = "d3d9/backbuffer-lockrect";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    const std::uint32_t out = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x48u, {0u, 0u, 0u, out}), 0u);  // GetBackBuffer
    const std::uint32_t surf = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(surf, 0u);
    if (surf == 0) return;

    const std::uint32_t L = 17, T = 23, R = 48, B = 32;  // ★ 子矩形（不是整幅）
    const std::uint32_t rect_ea = f.placeRect(L, T, R, B);
    const std::uint32_t lr_ea = f.placeBytes(std::vector<std::uint8_t>(8u, 0xCC));  // 初值非 0

    // ① 锁子矩形 ⇒ S_OK，Pitch = 整幅行距，pBits 偏到矩形左上
    LIFT_CHECK_EQ_U32(f.method(surf, 0x34u, {lr_ea, rect_ea, 0u}), 0u);
    const std::uint32_t pitch = f.m.mem().rd32(lr_ea + 0u);
    const std::uint32_t bits = f.m.mem().rd32(lr_ea + 4u);
    LIFT_CHECK_EQ_U32(pitch, dev.info.pitch);  // ★ 锁小矩形**不改** Pitch
    LIFT_CHECK_EQ_U32(bits, dev.info.arena + T * pitch + L * 4u);

    // ② 像素穿透：按 pBits 写一个像素，后缓冲对应位置必须读回同一个值；相邻像素不动
    //   ★ 先确认 pBits 在范围内再写：锁失败时出参是 guest 的初值（0xCC…），盲写会把机器打成
    //     guest memory fault，后面的断言就看不到了（修前的 repro 正是这样）。
    if (bits == dev.info.arena + T * pitch + L * 4u) {
        f.m.mem().wr32(bits, 0x00123456u);
        LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, static_cast<int>(L), static_cast<int>(T)), 0x00123456u);
        LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, static_cast<int>(L + 1), static_cast<int>(T)), 0u);
        // ★ 下一行必须隔**整幅行距**：真机的逐行 memcpy 正是按 Pitch 推进源指针（0x4A5018）。
        f.m.mem().wr32(bits + pitch, 0x00654321u);
        LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, static_cast<int>(L), static_cast<int>(T + 1)), 0x00654321u);
        LIFT_CHECK_EQ_U32(f.pixelAt(dev.info, static_cast<int>(L), static_cast<int>(T)), 0x00123456u);
    }

    // ③ 已锁再锁 ⇒ D3DERR_INVALIDCALL（⛔ 不许静默重入）；失败的锁**不写出参**
    const std::uint32_t lr2 = f.placeBytes(std::vector<std::uint8_t>(8u, 0xCC));
    LIFT_CHECK_EQ_U32(f.method(surf, 0x34u, {lr2, 0u, 0u}), 0x8876086Cu);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(lr2 + 0u), 0xCCCCCCCCu);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(lr2 + 4u), 0xCCCCCCCCu);

    // ④ 解锁 ⇒ S_OK；未锁再解锁 ⇒ D3DERR_INVALIDCALL
    LIFT_CHECK_EQ_U32(f.method(surf, 0x38u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.method(surf, 0x38u, {}), 0x8876086Cu);

    // ⑤ Unlock 之后能再锁（这次整幅 rect=NULL）⇒ pBits = arena、Pitch 仍 = 整幅行距
    LIFT_CHECK_EQ_U32(f.method(surf, 0x34u, {lr2, 0u, 0u}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(lr2 + 0u), dev.info.pitch);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(lr2 + 4u), dev.info.arena);
    LIFT_CHECK_EQ_U32(f.method(surf, 0x38u, {}), 0u);

    // ⑥ 越界矩形 ≠ 静默夹取；DISCARD 对非 dynamic 的后缓冲非法；面没有 NOOVERWRITE；
    //    pLockedRect 不可写；这些失败后**仍能正常锁**（失败不占锁）
    const std::uint32_t bad = f.placeRect(0u, 0u, 1281u, 720u);
    LIFT_CHECK_EQ_U32(f.method(surf, 0x34u, {lr2, bad, 0u}), 0x8876086Cu);
    LIFT_CHECK_EQ_U32(f.method(surf, 0x34u, {lr2, rect_ea, lift::d3d9::kLockDiscard}), 0x8876086Cu);
    LIFT_CHECK_EQ_U32(f.method(surf, 0x34u, {lr2, rect_ea, 0x00001000u}), 0x8876086Cu);  // NOOVERWRITE
    LIFT_CHECK_EQ_U32(f.method(surf, 0x34u, {0u, 0u, 0u}), 0x8876086Cu);                // pLockedRect=NULL
    LIFT_CHECK_EQ_U32(f.method(surf, 0x34u, {lr2, 0u, 0u}), 0u);
    LIFT_CHECK_EQ_U32(f.method(surf, 0x38u, {}), 0u);

    // ⑥b 没有 D3DPRESENTFLAG_LOCKABLE_BACKBUFFER 的后缓冲 ⇒ 锁必失败（真机 `0x498F98` 明确设了 1）
    Fixture::Device no_lock;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 640, 480, lift::d3d9::kFmtX8R8G8B8, &no_lock, 0u));
    const std::uint32_t out2 = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(no_lock.object, 0x48u, {0u, 0u, 0u, out2}), 0u);
    const std::uint32_t surf2 = f.m.mem().rd32(out2);
    const std::uint32_t lr3 = f.placeBytes(std::vector<std::uint8_t>(8u, 0xCC));
    LIFT_CHECK_EQ_U32(f.method(surf2, 0x34u, {lr3, 0u, 0u}), 0x8876086Cu);

    // ⑥c multisample != NONE ⇒ 锁必失败。本线 CreateDevice 会先拒绝 MSAA（D3DERR_NOTAVAILABLE），
    //     所以正常路径构造不出这种设备 ⇒ 这里直接置像素块上的字段，验证 LockRect 侧的**独立**检查。
    Fixture::Device msaa;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 64, 64, lift::d3d9::kFmtX8R8G8B8, &msaa));
    lift::GuestSurface* ms = f.m.surfaces().find(msaa.info.arena);
    LIFT_CHECK(ms != nullptr);
    if (ms != nullptr) {
        ms->d3d9_multisample = 2u;  // D3DMULTISAMPLE_2_SAMPLES
        const std::uint32_t out3 = f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.method(msaa.object, 0x48u, {0u, 0u, 0u, out3}), 0u);
        const std::uint32_t surf3 = f.m.mem().rd32(out3);
        const std::uint32_t lr4 = f.placeBytes(std::vector<std::uint8_t>(8u, 0xCC));
        LIFT_CHECK_EQ_U32(f.method(surf3, 0x34u, {lr4, 0u, 0u}), 0x8876086Cu);
    }

    // ⑦ ★ 这条路径不再 trap：修好前这里会是 Trap + `unimplemented_calls=1`
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(f.m.cpu().shim.unimplemented_calls), 0u);
}

// ---------------------------------------------------------------------------------------
// ⑫ `GetDevice` 的**引用计数**（REQ-01M701NYF2B3YV1YFSZBMPQZ41 · 第 1 段）
//
// 判据（红了意味着什么）：微软文档对 `IDirect3DResource9::GetDevice` 明确写"调用会增加
//   `IDirect3DDevice9` 的引用计数、调用方必须 Release"（纹理 / VB / IB 都继承它）；
//   `IDirect3DVertexDeclaration9::GetDevice` 的页面没写，本仓按 `Get*` 家族一致性办。
//   ⇒ 四处都必须是"返回同一台设备 + 设备 refcount **+1**"，把那一份 Release 掉回到基线。
//   ★ 修前这四个 shim 都是**裸借出** ⇒ 四条 `base + 1` 断言全红（got = base）。
//   ★ 反过来它们也盯"加到陌生对象上"：出参没校验时 `out_dev` 仍会是 guest 的初值、
//     基线断言也会在**前一条**（out == dev.object）先红。
// ---------------------------------------------------------------------------------------
void caseResourceGetDeviceRefcount() {
    lift_test::g_current_case = "d3d9/resource-getdevice-refcount";
    Fixture f;
    LIFT_CHECK(f.boot());
    lift::registerD3DX9Shims(f.m.shims());
    lift::ShimDef* dx = f.m.shims().findMutable("d3dx9_43", "D3DXCreateTexture");
    LIFT_CHECK(dx != nullptr);
    if (!dx) return;
    dx->stub_ea = kProcD3DXCreateTexture;
    f.m.cache().installShim(kProcD3DXCreateTexture, dx->id);

    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    // 基线 = CreateDevice 给的那一份（不从 1 写死：以后设备自己再持引用时用例不用改）
    const std::uint32_t base = f.m.cpu().com->refCount(dev.object);
    const std::uint32_t out_dev = f.placeU32(0xDEADBEEFu);
    const std::uint32_t out = f.placeU32(0xDEADBEEFu);
    const std::uint32_t decl_ea = f.placeBytes(positionOnlyDeclaration());

    // 每次 `GetDevice` 之后：设备 refcount 必须 +1（借出）、把它还掉回到基线。
    // ★ 只在"确实借出了那份"时再 Release：缺陷态（没 AddRef）下 Release 会把设备降到 0，
    //   后面的子块就全成了"在已释放的设备上调方法"，平台异常会把其余断言一起吞掉 ⇒
    //   一处缺陷只应该红一处，而不是让整个用例组崩掉。
    auto giveBackBorrowedDeviceRef = [&]() {
        LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(dev.object), base + 1u);  // ← 修前 = base
        if (f.m.cpu().com->refCount(dev.object) == base + 1u)
            LIFT_CHECK_EQ_U32(f.method(dev.object, 0x08u, {}), base);
        LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(dev.object), base);
    };

    // ① 纹理（`IDirect3DTexture9`，继承 IDirect3DResource9 ⇒ 文档明确 AddRef）
    LIFT_CHECK_EQ_U32(callD3DXCreateTexture(f, dev.object, 4u, 4u, lift::d3d9::kFmtA8R8G8B8, out), 0u);
    const std::uint32_t tex = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(tex, 0u);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(tex), 1u);
    LIFT_CHECK_EQ_U32(f.method(tex, 0x0Cu, {out_dev}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(out_dev), dev.object);
    giveBackBorrowedDeviceRef();

    // ② 顶点缓冲（IDirect3DVertexBuffer9）
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x68u, {12u, 8u, 0x0044u, 1u, out, 0u}), 0u);
    const std::uint32_t vb = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(vb, 0u);
    LIFT_CHECK_EQ_U32(f.method(vb, 0x0Cu, {out_dev}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(out_dev), dev.object);
    giveBackBorrowedDeviceRef();
    LIFT_CHECK_EQ_U32(f.method(vb, 0x08u, {}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(vb));

    // ③ 索引缓冲（IDirect3DIndexBuffer9；Format=101 = D3DFMT_INDEX16）
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x6Cu, {12u, 8u, 101u, 1u, out, 0u}), 0u);
    const std::uint32_t ib = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(ib, 0u);
    LIFT_CHECK_EQ_U32(f.method(ib, 0x0Cu, {out_dev}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(out_dev), dev.object);
    giveBackBorrowedDeviceRef();
    LIFT_CHECK_EQ_U32(f.method(ib, 0x08u, {}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(ib));

    // ④ 顶点声明（IDirect3DVertexDeclaration9；文档未写 ⇒ 家族一致裁决）
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x158u, {decl_ea, out}), 0u);
    const std::uint32_t vdecl = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(vdecl, 0u);
    LIFT_CHECK_EQ_U32(f.method(vdecl, 0x0Cu, {out_dev}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(out_dev), dev.object);
    giveBackBorrowedDeviceRef();
    LIFT_CHECK_EQ_U32(f.method(vdecl, 0x08u, {}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(vdecl));

    // 收尾：纹理那份 guest 引用也放掉（它没绑过，直接归 0）
    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 0u);
}

// ---------------------------------------------------------------------------------------
// ⑬ `GetVertexDeclaration` 的引用计数 + VD 的 `on_zero`
//
// 判据（红了意味着什么）：
//   ① 绑定后 `GetVertexDeclaration` 借出的是**新的一份** ⇒ refCount 1 → 2（修前仍是 1）；
//   ② guest 放掉自己那份之后 **对象仍在**、`GetDeclaration` 仍逐字节可读（修前它会当场归 0，
//      对象与 `'VDCL'` 状态块一起没了 —— 这就是"借出裸 EA"的实际危害）；
//   ③ **最后一份**由宿主侧收敛点（`ComRegistry::releaseImpl`，不是 iface 的 Release shim）放掉时，
//      `'VDCL'` 状态块必须由 `on_zero` 回收 ⇒ guest 堆回到建声明之前的基线。
//      ★ 修前 VD 的 `createObject` 没有 `on_zero` ⇒ 这一条红（漏 28 B：12 B 头 + 16 B 声明）。
// ---------------------------------------------------------------------------------------
void caseVertexDeclarationGetRefcount() {
    lift_test::g_current_case = "d3d9/vertex-decl-get-refcount";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    const std::uint32_t decl_ea = f.placeBytes(positionOnlyDeclaration());
    const std::uint32_t out = f.placeU32(0xDEADBEEFu);
    const std::uint32_t got = f.placeU32(0xDEADBEEFu);
    const std::uint32_t heap_before = f.m.heap().bytesInUse();

    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x158u, {decl_ea, out}), 0u);
    const std::uint32_t vdecl = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(vdecl, 0u);
    if (vdecl == 0) return;
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(vdecl), 1u);

    // ① 没绑定时 GetVertexDeclaration 返回 0，且**不**给 0 加引用（AddRef 不许发生在"没有对象"上）
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x160u, {got}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(got), 0u);

    // ② 绑定后借出新的一份
    //   ★ 段 2（`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 的 ③）之后设备**自己**也持一份绑定引用
    //     （与 `SetTexture` 同口径）⇒ 这里的三份 = guest 1 + 绑定期 1 + GetVertexDeclaration 借出的 1。
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x15Cu, {vdecl}), 0u);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x160u, {got}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(got), vdecl);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(vdecl), 3u);  // ← 修前 = 1（既没绑定引用也没借出）

    // ③ guest 放掉自己那份 ⇒ 只剩"绑定期 + Get* 借出"两份：对象仍在、声明内容完好
    LIFT_CHECK_EQ_U32(f.method(vdecl, 0x08u, {}), 2u);
    LIFT_CHECK(f.m.cpu().com->find(vdecl));
    //   ★ 判活再调（同 `caseVertexDeclaration` 的说明）：对象若已经没了，`Fixture::method` 读 vtable
    //     会打到未提交页，把整个用例组吞掉。
    if (f.m.cpu().com->find(vdecl)) {
        const std::uint32_t copy = f.placeBytes(std::vector<std::uint8_t>(16u, 0xEE));
        LIFT_CHECK_EQ_U32(f.method(vdecl, 0x10u, {copy}), 0u);
        LIFT_CHECK_EQ_U32(f.m.mem().rd8(copy + 4u), 2u);  // D3DDECLTYPE_FLOAT3（状态块没被提前回收）
    }

    // ④ 解绑 ⇒ 设备那份绑定引用由流水线还回去（不是 guest 还）；然后**最后一份**（借出的那份）
    //   走**宿主侧收敛点**（等价于任何内部持有者 Release）：on_zero 必须回收 'VDCL'
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x15Cu, {0u}), 0u);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(vdecl), 1u);
    LIFT_CHECK(f.m.cpu().com->release(f.m.cpu(), vdecl));
    LIFT_CHECK(!f.m.cpu().com->find(vdecl));
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_before);  // ← 修前漏 'VDCL'
}

// ---------------------------------------------------------------------------------------
// ⑭ `GetTexture` 的引用计数（文档明确 AddRef）
//
// 判据（红了意味着什么）：
//   ① 绑定期设备持一份（`setTexture` 的 addRef，既有口径）；guest 放掉自己那份后是 1；
//      `GetTexture` 借出 ⇒ 2（**修前 = 1**）；
//   ② 还掉借出的那份后对象仍在、`'TEX1'` 状态块仍完好（`GetLevelCount` 可读）；
//   ③ 解绑 ⇒ 最后一次是**内部裸 release** ⇒ 纹理的 `on_zero`（`destroyTextureState`）回收
//      状态块 + arena 像素块 ⇒ guest 堆与 arena 都回到建纹理之前的基线。
// ---------------------------------------------------------------------------------------
void caseGetTextureRefcount() {
    lift_test::g_current_case = "d3d9/gettexture-refcount";
    Fixture f;
    LIFT_CHECK(f.boot());
    lift::registerD3DX9Shims(f.m.shims());
    lift::ShimDef* dx = f.m.shims().findMutable("d3dx9_43", "D3DXCreateTexture");
    LIFT_CHECK(dx != nullptr);
    if (!dx) return;
    dx->stub_ea = kProcD3DXCreateTexture;
    f.m.cache().installShim(kProcD3DXCreateTexture, dx->id);

    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    const std::uint32_t heap_before = f.m.heap().bytesInUse();
    const std::size_t surfaces_before = f.m.surfaces().surfaceCount();
    const std::uint32_t bytes_before = f.m.surfaces().bytesInUse();

    const std::uint32_t out = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(callD3DXCreateTexture(f, dev.object, 4u, 4u, lift::d3d9::kFmtA8R8G8B8, out), 0u);
    const std::uint32_t tex = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(tex), 1u);

    // 绑定 ⇒ 设备持一份（SetTexture 的 AddRef，既有口径）；guest 放掉自己那份 ⇒ 1
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x104u, {0u, tex}), 0u);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(tex), 2u);
    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 1u);

    // ① GetTexture 借出新的一份
    const std::uint32_t got = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x108u, {0u, got}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(got), tex);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(tex), 2u);  // ← 修前 = 1

    // ② 还掉借出的那份 ⇒ 只剩绑定那份，对象与状态块都还在
    LIFT_CHECK_EQ_U32(f.method(tex, 0x08u, {}), 1u);
    LIFT_CHECK(f.m.cpu().com->find(tex));
    //   ★ 判活再调：借出的那份若没加上（缺陷态），对象此刻已经归 0，`Fixture::method` 读 vtable
    //     会打到未提交页（把后面的断言一起吞掉）。
    if (f.m.cpu().com->find(tex)) LIFT_CHECK_EQ_U32(f.method(tex, 0x34u, {}), 1u);  // GetLevelCount

    // ③ 解绑 ⇒ 内部裸 release 归 0 ⇒ on_zero 回收像素块 + 'TEX1'，堆 / arena 回基线
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x104u, {0u, 0u}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(tex));
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_before);
    LIFT_CHECK_EQ_U32(f.m.surfaces().surfaceCount(), surfaces_before);
    LIFT_CHECK_EQ_U32(f.m.surfaces().bytesInUse(), bytes_before);
}

// ---------------------------------------------------------------------------------------
// ⑮ VB / IB / VD 的 `on_zero`：**最后一次引用由宿主侧内部 release 放掉**时状态块也必须回收
//
// 判据（红了意味着什么）：照 `SetTexture` 的形态让宿主替资源持一份引用（`com->addRef`），
//   guest 放掉自己那份，最后一份走 `com->release`（**不是** iface 的 Release shim）⇒
//   `'VB09'` / `'IB09'` / `'VDCL'` 状态块必须由 `on_zero` 回收 ⇒ guest 堆逐次回到基线。
//   ★ 修前这三类 `createObject` 都没传 `on_zero` ⇒ 三条 `bytesInUse()` 逐条红（各漏 40 / 40 / 28 B）。
//   ★ iface Release shim 那条老路径照旧（`destroy*State` 幂等），所以既有用例不会因此变红 ——
//     本用例专门盯"内部"那条收敛点。
// ---------------------------------------------------------------------------------------
void caseResourceOnZeroInternalRelease() {
    lift_test::g_current_case = "d3d9/resource-onzero-internal-release";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    const std::uint32_t heap_base = f.m.heap().bytesInUse();
    const std::uint32_t out = f.placeU32(0xDEADBEEFu);
    const std::uint32_t decl_ea = f.placeBytes(positionOnlyDeclaration());

    // ① 顶点缓冲
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x68u, {12u, 8u, 0x0044u, 1u, out, 0u}), 0u);
    const std::uint32_t vb = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(vb, 0u);
    LIFT_CHECK(f.m.cpu().com->addRef(f.m.cpu(), vb));   // 宿主侧持一份（= SetTexture 的形态）
    LIFT_CHECK_EQ_U32(f.method(vb, 0x08u, {}), 1u);      // guest 放掉自己那份
    LIFT_CHECK(f.m.cpu().com->find(vb));
    LIFT_CHECK(f.m.cpu().com->release(f.m.cpu(), vb));   // 最后一次：内部裸 release
    LIFT_CHECK(!f.m.cpu().com->find(vb));
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_base);  // ← 修前漏 'VB09'

    // ② 索引缓冲
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x6Cu, {12u, 8u, 101u, 1u, out, 0u}), 0u);
    const std::uint32_t ib = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(ib, 0u);
    LIFT_CHECK(f.m.cpu().com->addRef(f.m.cpu(), ib));
    LIFT_CHECK_EQ_U32(f.method(ib, 0x08u, {}), 1u);
    LIFT_CHECK(f.m.cpu().com->find(ib));
    LIFT_CHECK(f.m.cpu().com->release(f.m.cpu(), ib));
    LIFT_CHECK(!f.m.cpu().com->find(ib));
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_base);  // ← 修前漏 'IB09'

    // ③ 顶点声明
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x158u, {decl_ea, out}), 0u);
    const std::uint32_t vdecl = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(vdecl, 0u);
    LIFT_CHECK(f.m.cpu().com->addRef(f.m.cpu(), vdecl));
    LIFT_CHECK_EQ_U32(f.method(vdecl, 0x08u, {}), 1u);
    LIFT_CHECK(f.m.cpu().com->find(vdecl));
    LIFT_CHECK(f.m.cpu().com->release(f.m.cpu(), vdecl));
    LIFT_CHECK(!f.m.cpu().com->find(vdecl));
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_base);  // ← 修前漏 'VDCL'
}

// ---------------------------------------------------------------------------------------
// ⑯ `GetTexture` 的**身份校验**（`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 段 3 的 ②）
//
// 判据（红了意味着什么）：纹理绑定时必须把**对象代际**一起记进流水线（`kPlTextureGens`，与
//   `kPlVertexDeclGen` 同口径）；`GetTexture` 借出前必须同时证明"iface 还是 IDirect3DTexture9"
//   与"代际还是绑定时那个"。只 addRef 不校验的旧口径会把**新纹理** +1 交回 guest：
//   ① 本用例的 `GetTexture` 期望 D3DERR_INVALIDCALL，却返回 S_OK + 新纹理 EA（= 别人的对象）；
//   ② 新纹理的 refcount 被莫名 +1（期望 1 得 2），guest 之后 Release 它时减的是**自己的**那份。
//   ★ 正向对照（③）钉住"没有退化成一概拒绝"：槽里还是同一张纹理时 GetTexture 必须借出 +1。
// ★ 夹具前提（对象 EA 复用 + `'TEX1'` 状态块 EA 复用）硬断言，不成立就红 —— 同
//   `d3d9p/stale-stream-binding` 的口径（先按 `runtime/heap.cpp` 的分块重造夹具，⛔ 不删断言）。
// ---------------------------------------------------------------------------------------
void caseGetTextureStaleGeneration() {
    lift_test::g_current_case = "d3d9/gettexture-stale-generation";
    Fixture f;
    LIFT_CHECK(f.boot());
    lift::registerD3DX9Shims(f.m.shims());
    lift::ShimDef* dx = f.m.shims().findMutable("d3dx9_43", "D3DXCreateTexture");
    LIFT_CHECK(dx != nullptr);
    if (!dx) return;
    dx->stub_ea = kProcD3DXCreateTexture;
    f.m.cache().installShim(kProcD3DXCreateTexture, dx->id);

    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;
    lift::ComRegistry* com = f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    auto makeTexture = [&](std::uint32_t* out) {
        const std::uint32_t out_ea = f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(callD3DXCreateTexture(f, dev.object, 4u, 4u, lift::d3d9::kFmtA8R8G8B8, out_ea), 0u);
        *out = f.m.mem().rd32(out_ea);
        LIFT_CHECK_NE_U32(*out, 0u);
    };

    std::uint32_t t1 = 0;
    std::uint32_t t_dummy = 0;
    makeTexture(&t1);
    if (t1 == 0) return;
    const std::uint32_t t1_state = com->userOf(t1);
    const std::uint32_t t1_gen = com->generationOf(t1);
    LIFT_CHECK_NE_U32(t1_state, 0u);
    LIFT_CHECK_NE_U32(t1_gen, 0u);
    makeTexture(&t_dummy);  // 同尺寸哑纹理：释放后空洞连成一片 ⇒ 新纹理能拿回同一对 EA
    if (t_dummy == 0) return;

    // 绑 Stage 0 ⇒ 设备持一份（2）；guest 放掉自己那份（1）；越界再放一次 ⇒ 对象死、EA 可复用。
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x104u, {0u, t1}), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(t1), 2u);
    LIFT_CHECK_EQ_U32(f.method(t1, 0x08u, {}), 1u);
    if (com->find(t1)) LIFT_CHECK_EQ_U32(f.method(t1, 0x08u, {}), 0u);  // 越界：把设备那份也抵掉
    LIFT_CHECK(!com->find(t1));
    LIFT_CHECK_EQ_U32(f.method(t_dummy, 0x08u, {}), 0u);

    std::uint32_t t2 = 0;
    makeTexture(&t2);
    if (t2 == 0) return;
    LIFT_CHECK_EQ_U32(t2, t1);                           // 前提 1：对象 EA 复用
    LIFT_CHECK_EQ_U32(com->userOf(t2), t1_state);        // 前提 2：`'TEX1'` 状态块 EA 也复用
    LIFT_CHECK_NE_U32(com->generationOf(t2), t1_gen);    // 前提 3：身份确实换了

    // ② 判据：槽里的 (EA, 代际) 已不是绑定时那张纹理 ⇒ 拒绝借出（⛔ 不把新占有者交回 guest）
    const std::uint32_t got = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x108u, {0u, got}), 0x8876086Cu);  // ← 修前 = 0
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(got), 0xDEADBEEFu);                      // 出参没被写
    LIFT_CHECK_EQ_U32(com->refCount(t2), 1u);                                 // ← 修前 = 2（别人的对象被 +1）
    LIFT_CHECK(com->find(t2));

    // ③ 正向对照：重新绑同一张（EA 与代际都对上）⇒ GetTexture 必须借出 +1
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x104u, {0u, t2}), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(t2), 2u);  // 设备那份（旧绑定的 releaseIf 被代际拒绝 ⇒ 不减）
    const std::uint32_t got2 = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x108u, {0u, got2}), 0u);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(got2), t2);
    LIFT_CHECK_EQ_U32(com->refCount(t2), 3u);
    LIFT_CHECK_EQ_U32(f.method(t2, 0x08u, {}), 2u);  // 还掉借出的那份
    // ★ 重复 Set **同一份绑定**（EA + 代际都对）⇒ 两边都不动 ⇒ 计数不漂（不是"每次 +1"）
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x104u, {0u, t2}), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(t2), 2u);
    // 解绑 ⇒ 设备那份还回；guest 最后那份释放 ⇒ 对象 / `'TEX1'` / 像素块一起回收
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x104u, {0u, 0u}), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(t2), 1u);
    LIFT_CHECK_EQ_U32(f.method(t2, 0x08u, {}), 0u);
    LIFT_CHECK(!com->find(t2));
}

// ---------------------------------------------------------------------------------------
// ⑰ `SetFVF` 两个清声明的分支都必须**还回绑定引用**（`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 段 3 的 ⑤）
//
// 判据（红了意味着什么）：`FVF` 与顶点声明是二选一 ⇒ `SetFVF(0)`（合法清除）与 `SetFVF(有效值)`
//   都要把设备替那句声明持的引用还回去。漏掉任一支 = 声明永远到不了 0（`'VDCL'` 留在堆上）⇒
//   对象仍 `find()` 得到、堆基线高出声明块。
//   ★ 复审实测过：把 `fvf == 0` 那一支改成"只清槽不还引用"，旧用例仍全绿 —— 所以这里两支各一条判据。
//   ★ 修前这一支没有守卫（`SetFVF(0)` 在整套 CTest 里一次都没被调用过）。
// ---------------------------------------------------------------------------------------
void caseSetFVFReleasesDecl() {
    lift_test::g_current_case = "d3d9/setfvf-releases-decl";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;
    lift::ComRegistry* com = f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    const std::uint32_t heap_before = f.m.heap().bytesInUse();
    const std::uint32_t decl_ea = f.placeBytes(positionOnlyDeclaration());
    const std::uint32_t out = f.placeU32(0xDEADBEEFu);

    // ① `SetFVF(0)`（合法清除）：绑定引用必须还回 ⇒ guest 那份一放就归 0
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x158u, {decl_ea, out}), 0u);
    const std::uint32_t vdecl = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(vdecl, 0u);
    if (vdecl == 0) return;
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x15Cu, {vdecl}), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(vdecl), 2u);       // guest 1 + 设备绑定 1
    LIFT_CHECK_EQ_U32(f.method(vdecl, 0x08u, {}), 1u);  // guest 放掉自己那份
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x164u, {0u}), 0u);  // SetFVF(0)
    LIFT_CHECK(!com->find(vdecl));                              // ← 修前：对象还活着（绑定引用没还）
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_before);    // ← 修前：`'VDCL'` 留在堆上

    // ② `SetFVF(有效值)`：同一判据（这一支原本由 `d3d9/vertex-declaration` 的堆基线守着）
    const std::uint32_t out2 = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x158u, {decl_ea, out2}), 0u);
    const std::uint32_t vdecl2 = f.m.mem().rd32(out2);
    LIFT_CHECK_NE_U32(vdecl2, 0u);
    if (vdecl2 == 0) return;
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x15Cu, {vdecl2}), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(vdecl2), 2u);
    LIFT_CHECK_EQ_U32(f.method(vdecl2, 0x08u, {}), 1u);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x164u, {0x0044u}), 0u);  // SetFVF(XYZ|DIFFUSE? 0x44)
    LIFT_CHECK(!com->find(vdecl2));
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_before);
}

// ---------------------------------------------------------------------------------------
// ⑱ 同一 EA 的**新顶点声明**再 `SetVertexDeclaration`（段 3 的 ③，声明这一格）
//
// 判据（红了意味着什么）：槽里同时存 (EA, 代际)。guest 堆复用 EA ⇒ 同 EA 不同代际是**新对象**：
//   必须给它 addRef（设备绑定引用）、并把旧绑定按 (EA, 旧代际) 判还（`releaseIf` 会拒绝 ⇒ 只出声）。
//   旧判据只比 EA ⇒ "同 EA"被当"重复 Set 同一个" ⇒ 新声明**没有**绑定引用，guest 一放就归 0
//   （对象 + `'VDCL'` 当场没；修前 `expected 1, got 0`）。
//   ★ 三处 Set*（声明 / 流 / 索引）同形；流与索引的对应判据在 `d3d9p/binding-lifetime` 的 ③-D/E。
// ---------------------------------------------------------------------------------------
void caseVertexDeclarationStaleRebind() {
    lift_test::g_current_case = "d3d9/vertex-decl-stale-rebind";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 1280, 720, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;
    lift::ComRegistry* com = f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    const std::uint32_t heap_before = f.m.heap().bytesInUse();
    const std::uint32_t decl_ea = f.placeBytes(positionOnlyDeclaration());
    const std::uint32_t out = f.placeU32(0xDEADBEEFu);

    // 声明 1 + 哑声明（同尺寸 ⇒ 一起释放后空洞连成一片，新声明能拿回同一对 EA）
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x158u, {decl_ea, out}), 0u);
    const std::uint32_t v1 = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(v1, 0u);
    if (v1 == 0) return;
    const std::uint32_t v1_state = com->userOf(v1);
    const std::uint32_t v1_gen = com->generationOf(v1);
    LIFT_CHECK_NE_U32(v1_state, 0u);
    LIFT_CHECK_NE_U32(v1_gen, 0u);
    const std::uint32_t out_dummy = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x158u, {decl_ea, out_dummy}), 0u);
    const std::uint32_t v_dummy = f.m.mem().rd32(out_dummy);
    LIFT_CHECK_NE_U32(v_dummy, 0u);
    if (v_dummy == 0) return;

    // 绑定 ⇒ 设备持一份（2）；guest 放掉自己那份（1）；越界再放一次 ⇒ 对象死、EA 可复用
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x15Cu, {v1}), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(v1), 2u);
    LIFT_CHECK_EQ_U32(f.method(v1, 0x08u, {}), 1u);
    if (com->find(v1)) LIFT_CHECK_EQ_U32(f.method(v1, 0x08u, {}), 0u);
    LIFT_CHECK(!com->find(v1));
    LIFT_CHECK_EQ_U32(f.method(v_dummy, 0x08u, {}), 0u);

    // 新声明拿回同一对 EA（对象 EA + `'VDCL'` 状态块 EA），代际不同
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x158u, {decl_ea, out}), 0u);
    const std::uint32_t v2 = f.m.mem().rd32(out);
    LIFT_CHECK_EQ_U32(v2, v1);                        // 前提 1：对象 EA 复用
    LIFT_CHECK_EQ_U32(com->userOf(v2), v1_state);      // 前提 2：状态块 EA 也复用
    LIFT_CHECK_NE_U32(com->generationOf(v2), v1_gen);  // 前提 3：身份换了

    // ③ 判据：同 EA 不同代际 ⇒ 视为**新对象** ⇒ 正常 addRef 新的、旧绑定按代际判还
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x15Cu, {v2}), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(v2), 2u);  // ← 修前 = 1（被当成"重复 Set 同一个"）
    // ★ 重复 Set **同一份绑定**（EA + 代际都对）⇒ 不再 addRef（计数不漂）
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x15Cu, {v2}), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(v2), 2u);
    LIFT_CHECK_EQ_U32(f.method(v2, 0x08u, {}), 1u);  // guest 放掉自己那份 ⇒ 设备那份还在
    LIFT_CHECK(com->find(v2));                       // ← 修前：对象当场归 0
    // 解绑 ⇒ 设备那份还回 ⇒ 对象 / `'VDCL'` / 堆一起回基线
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x15Cu, {0u}), 0u);
    LIFT_CHECK(!com->find(v2));
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_before);
}

// ---------------------------------------------------------------------------------------
// ⑳ IDirect3DStateBlock9：CreateStateBlock / Capture / Apply **真的**快照 / 写回
//   （`REQ-01M7372BPVGBD247KAGHB53YK7`；`.lst` 0x47843A–0x47847B = 引擎每帧 Live2D 的形状：
//    `[+0xEC](1=ALL, &blk)` → draw → `blk[+0x14]`(Apply) → `blk[+8]`(Release)）
//
// 判据（红了意味着什么）：全部是**关系**断言（⛔ 不断言任何"当前取值"式的数字）——
//   ① 建块(ALL) → 改 渲染状态 / 纹理 / FVF / 变换 → Apply ⇒ 五项都回到**建块那一刻**的值
//      （修前：Apply 空转 ⇒ 五项停在"改后"的值）；
//   ② 再改 → Capture → 再改 → Apply ⇒ 回到 **Capture 那一刻**（修前：Capture 空转）；
//      ★ 这一段的渲染状态取值刻意与 ① 的 t0 **不同** ⇒ "回到 Capture"与"回到建块"能区分开；
//   ③ 掩码**有意义**：PIXEL 块 Apply 只回滚像素侧（渲染状态 / 纹理），⛔ 不动 FVF / 变换；
//      VERTEX 块相反（FVF / 变换回滚，渲染状态 / 纹理保持"改后"）。
//   ④ 快照替 guest 持一份绑定引用：块活着时 guest 放掉自己那份，纹理仍然活着、Apply 还能写回；
//      Release 块 ⇒ 快照那份成对还回 ⇒ 解绑后对象销毁、guest 堆回到建纹理之前的基线。
// ---------------------------------------------------------------------------------------
void caseStateBlockCaptureApply() {
    lift_test::g_current_case = "d3d9/stateblock-capture-apply";
    Fixture f;
    LIFT_CHECK(f.boot());
    lift::registerD3DX9Shims(f.m.shims());
    lift::ShimDef* dx = f.m.shims().findMutable("d3dx9_43", "D3DXCreateTexture");
    LIFT_CHECK(dx != nullptr);
    if (!dx) return;
    dx->stub_ea = kProcD3DXCreateTexture;
    f.m.cache().installShim(kProcD3DXCreateTexture, dx->id);

    const std::uint32_t d3d9 = f.createDirect3D();
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(d3d9, 1u, 0x44u, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;
    lift::ComRegistry* com = f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    const std::uint32_t heap_before_tex = f.m.heap().bytesInUse();

    // 两张纹理：A = 建块时绑的、B = 改后绑的（⛔ 不用同一张来冒充"恢复"）。
    auto makeTexture = [&]() {
        const std::uint32_t out = f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(callD3DXCreateTexture(f, dev.object, 4u, 4u, lift::d3d9::kFmtA8R8G8B8, out), 0u);
        const std::uint32_t t = f.m.mem().rd32(out);
        LIFT_CHECK_NE_U32(t, 0u);
        return t;
    };
    const std::uint32_t tex_a = makeTexture();
    const std::uint32_t tex_b = makeTexture();
    if (tex_a == 0 || tex_b == 0) return;
    LIFT_CHECK_NE_U32(tex_a, tex_b);

    // 64 B 的 D3DMATRIX（D3DX 行主序布局；对角 = diag）—— 三份互不相同的矩阵。
    auto matrixOf = [](float diag) {
        std::vector<std::uint8_t> m(64u, 0u);
        for (int i = 0; i < 16; ++i) {
            const float v = (i % 5 == 0) ? diag : 0.0f;
            std::memcpy(&m[static_cast<std::size_t>(i) * 4u], &v, sizeof(v));
        }
        return m;
    };
    const std::vector<std::uint8_t> mat_a = matrixOf(2.0f);
    const std::vector<std::uint8_t> mat_b = matrixOf(3.0f);
    const std::vector<std::uint8_t> mat_c = matrixOf(4.0f);

    const std::uint32_t kRsZ = lift::d3d9::kRsZEnable;              // 像素侧（在掩码表里）
    const std::uint32_t kRsBlend = lift::d3d9::kRsAlphaBlendEnable;  // 同上（第二项，防"只恢复一项"）
    const std::uint32_t kFvfA = 0x244u;  // XYZRHW | DIFFUSE | TEX1（`.lst` 0x4A3610 的实测值）
    const std::uint32_t kFvfB = 0x142u;  // XYZ | DIFFUSE | TEX1
    const std::uint32_t kTs = lift::d3d9::kTsWorld;  // 256

    auto setRs = [&](std::uint32_t state, std::uint32_t v) {
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0xE4u, {state, v}), 0u);
    };
    auto getRs = [&](std::uint32_t state) {
        const std::uint32_t out = f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0xE8u, {state, out}), 0u);
        return f.m.mem().rd32(out);
    };
    auto setTex = [&](std::uint32_t t) {
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x104u, {0u, t}), 0u);
    };
    auto getTex = [&]() {
        const std::uint32_t out = f.placeU32(0xDEADBEEFu);
        const std::uint32_t hr = f.method(dev.object, 0x108u, {0u, out});
        LIFT_CHECK_EQ_U32(hr, 0u);
        // ★ 失败时出参还是 `0xDEADBEEF` 哨兵 ⇒ ⛔ 不能拿它去 Release（会读未提交页、整个进程崩，
        //   把"守卫红了"伪装成"模拟器崩了"）。
        if (hr != 0u) return 0xDEADBEEFu;
        const std::uint32_t t = f.m.mem().rd32(out);
        // ★ `GetTexture` **借出**一份引用（微软文档明确 AddRef）⇒ 读回后必须还回去。
        if (t != 0) f.method(t, 0x08u, {});
        return t;
    };
    auto setFvf = [&](std::uint32_t v) { LIFT_CHECK_EQ_U32(f.method(dev.object, 0x164u, {v}), 0u); };
    auto getFvf = [&]() {
        const std::uint32_t out = f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x168u, {out}), 0u);
        return f.m.mem().rd32(out);
    };
    auto setMatrix = [&](const std::vector<std::uint8_t>& m) {
        const std::uint32_t ea = f.placeBytes(m);
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0xB0u, {kTs, ea}), 0u);
    };
    auto getMatrix = [&]() {
        const std::uint32_t ea = f.placeBytes(std::vector<std::uint8_t>(64u, 0xCCu));
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0xB4u, {kTs, ea}), 0u);
        return f.bytesOf(ea, 64u);
    };
    auto createBlock = [&](std::uint32_t type) {
        const std::uint32_t out = f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0xECu, {type, out}), 0u);
        return f.m.mem().rd32(out);
    };
    // 采样器 / 纹理级 / 视口 / 裁剪矩形：掩码里另外 4 组标量，**每组都要有 Get\* 往返**。
    // ★ 为什么必须补（复审实测）：只断言 渲染状态 / 纹理 / FVF / 变换 时，把
    //   `sbApplyRange(kPlSamplers)` **整条删掉** 守卫仍全绿 ⇒ 采样器 / 纹理级 / 视口 / 裁剪矩形
    //   整段漏调（或偏移写错）会**静默**泄漏。这四个 Get* 都在既有 shim 里（+0x110/0x118/0xC0/0x130）。
    auto setSampler = [&](std::uint32_t type, std::uint32_t v) {
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x114u, {0u, type, v}), 0u);
    };
    auto getSampler = [&](std::uint32_t type) {
        const std::uint32_t out = f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x118u, {0u, type, out}), 0u);
        return f.m.mem().rd32(out);
    };
    auto setTexStage = [&](std::uint32_t type, std::uint32_t v) {
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x10Cu, {0u, type, v}), 0u);
    };
    auto getTexStage = [&](std::uint32_t type) {
        const std::uint32_t out = f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x110u, {0u, type, out}), 0u);
        return f.m.mem().rd32(out);
    };
    auto viewportBytes = [](std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
        std::vector<std::uint8_t> v(24u, 0u);
        const std::uint32_t ints[4] = {x, y, w, h};
        for (std::size_t i = 0; i < 4; ++i) std::memcpy(&v[i * 4u], &ints[i], sizeof(std::uint32_t));
        const float zs[2] = {0.0f, 1.0f};
        std::memcpy(&v[16], &zs[0], sizeof(float));
        std::memcpy(&v[20], &zs[1], sizeof(float));
        return v;
    };
    auto setViewport = [&](std::uint32_t x, std::uint32_t y, std::uint32_t w, std::uint32_t h) {
        const std::uint32_t ea = f.placeBytes(viewportBytes(x, y, w, h));
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0xBCu, {ea}), 0u);
    };
    auto getViewport = [&]() {
        const std::uint32_t ea = f.placeBytes(std::vector<std::uint8_t>(24u, 0xCCu));
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0xC0u, {ea}), 0u);
        return f.bytesOf(ea, 24u);
    };
    auto rectBytes = [](std::uint32_t l, std::uint32_t t, std::uint32_t r, std::uint32_t b) {
        std::vector<std::uint8_t> v(16u, 0u);
        const std::uint32_t f4[4] = {l, t, r, b};
        for (std::size_t i = 0; i < 4; ++i) std::memcpy(&v[i * 4u], &f4[i], sizeof(std::uint32_t));
        return v;
    };
    auto setScissor = [&](std::uint32_t l, std::uint32_t t, std::uint32_t r, std::uint32_t b) {
        const std::uint32_t ea = f.placeBytes(rectBytes(l, t, r, b));
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x12Cu, {ea}), 0u);
    };
    auto getScissor = [&]() {
        const std::uint32_t ea = f.placeBytes(std::vector<std::uint8_t>(16u, 0xCCu));
        LIFT_CHECK_EQ_U32(f.method(dev.object, 0x130u, {ea}), 0u);
        return f.bytesOf(ea, 16u);
    };

    // ---- ① Create 那一刻就捕获（ALL）----
    setRs(kRsZ, 0u);
    setRs(kRsBlend, 0u);
    setTex(tex_a);
    setFvf(kFvfA);
    setMatrix(mat_a);
    setSampler(lift::d3d9::kSampMagFilter, lift::d3d9::kTexfLinear);
    setTexStage(lift::d3d9::kTssColorOp, lift::d3d9::kTopSelectArg1);
    setViewport(0u, 0u, 64u, 64u);
    setScissor(0u, 0u, 64u, 64u);
    const std::uint32_t blk_all = createBlock(1u);  // D3DSBT_ALL
    LIFT_CHECK_NE_U32(blk_all, 0u);
    if (blk_all == 0) return;
    // 建块那一刻的值（之后只按**关系**比回来）
    const std::uint32_t t0_z = getRs(kRsZ);
    const std::uint32_t t0_blend = getRs(kRsBlend);
    const std::uint32_t t0_tex = getTex();
    const std::uint32_t t0_fvf = getFvf();
    const std::vector<std::uint8_t> t0_mat = getMatrix();
    const std::uint32_t t0_sampler = getSampler(lift::d3d9::kSampMagFilter);
    const std::uint32_t t0_texstage = getTexStage(lift::d3d9::kTssColorOp);
    const std::vector<std::uint8_t> t0_viewport = getViewport();
    const std::vector<std::uint8_t> t0_scissor = getScissor();
    // 改掉这九项
    setRs(kRsZ, 1u);
    setRs(kRsBlend, 1u);
    setTex(tex_b);
    setFvf(kFvfB);
    setMatrix(mat_b);
    setSampler(lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint);
    setTexStage(lift::d3d9::kTssColorOp, lift::d3d9::kTopModulate);
    setViewport(8u, 8u, 32u, 32u);
    setScissor(4u, 4u, 20u, 20u);
    // 前提：真的改掉了（否则下面的"回到"可能只是"根本没改"）
    LIFT_CHECK_NE_U32(getRs(kRsZ), t0_z);
    LIFT_CHECK_NE_U32(getRs(kRsBlend), t0_blend);
    LIFT_CHECK_NE_U32(getTex(), t0_tex);
    LIFT_CHECK_NE_U32(getFvf(), t0_fvf);
    LIFT_CHECK(getMatrix() != t0_mat);
    LIFT_CHECK_NE_U32(getSampler(lift::d3d9::kSampMagFilter), t0_sampler);
    LIFT_CHECK_NE_U32(getTexStage(lift::d3d9::kTssColorOp), t0_texstage);
    LIFT_CHECK(getViewport() != t0_viewport);
    LIFT_CHECK(getScissor() != t0_scissor);
    // Apply ⇒ 回到建块那一刻（修前：九项全停在"改后"）
    LIFT_CHECK_EQ_U32(f.method(blk_all, 0x14u, {}), 0u);
    LIFT_CHECK_EQ_U32(getRs(kRsZ), t0_z);
    LIFT_CHECK_EQ_U32(getRs(kRsBlend), t0_blend);
    LIFT_CHECK_EQ_U32(getTex(), t0_tex);
    LIFT_CHECK_EQ_U32(getFvf(), t0_fvf);
    LIFT_CHECK(getMatrix() == t0_mat);
    LIFT_CHECK_EQ_U32(getSampler(lift::d3d9::kSampMagFilter), t0_sampler);
    LIFT_CHECK_EQ_U32(getTexStage(lift::d3d9::kTssColorOp), t0_texstage);
    LIFT_CHECK(getViewport() == t0_viewport);
    LIFT_CHECK(getScissor() == t0_scissor);

    // ---- ② Capture = 重捕：回到 **Capture 那一刻** ----
    setRs(kRsZ, 0u);
    setRs(kRsBlend, 1u);  // ★ 与 t0 的 (Z=0, BLEND=0) 不同 ⇒ 能区分"回到 Capture"与"回到建块"
    setTex(tex_b);
    setFvf(kFvfB);
    setMatrix(mat_b);
    const std::uint32_t cap_z = getRs(kRsZ);
    const std::uint32_t cap_blend = getRs(kRsBlend);
    const std::uint32_t cap_tex = getTex();
    const std::uint32_t cap_fvf = getFvf();
    const std::vector<std::uint8_t> cap_mat = getMatrix();
    LIFT_CHECK_EQ_U32(f.method(blk_all, 0x10u, {}), 0u);  // Capture
    setRs(kRsZ, 1u);
    setRs(kRsBlend, 0u);
    setTex(tex_a);
    setFvf(kFvfA);
    setMatrix(mat_c);
    LIFT_CHECK_EQ_U32(f.method(blk_all, 0x14u, {}), 0u);  // Apply
    LIFT_CHECK_EQ_U32(getRs(kRsZ), cap_z);
    LIFT_CHECK_EQ_U32(getRs(kRsBlend), cap_blend);
    LIFT_CHECK_EQ_U32(getTex(), cap_tex);
    LIFT_CHECK_EQ_U32(getFvf(), cap_fvf);
    LIFT_CHECK(getMatrix() == cap_mat);
    LIFT_CHECK_NE_U32(cap_blend, t0_blend);  // 这一段的取值确实与 t0 不同
    LIFT_CHECK_EQ_U32(f.method(blk_all, 0x08u, {}), 0u);  // Release 到 0
    LIFT_CHECK(!com->find(blk_all));

    // ---- ③ 掩码：PIXEL 只回滚像素侧 ----
    setRs(kRsZ, 0u);
    setTex(tex_a);
    setFvf(kFvfA);
    setMatrix(mat_a);
    setSampler(lift::d3d9::kSampMagFilter, lift::d3d9::kTexfLinear);
    setTexStage(lift::d3d9::kTssColorOp, lift::d3d9::kTopSelectArg1);
    setViewport(0u, 0u, 64u, 64u);
    const std::uint32_t blk_pixel = createBlock(2u);  // D3DSBT_PIXELSTATE
    LIFT_CHECK_NE_U32(blk_pixel, 0u);
    if (blk_pixel == 0) return;
    const std::uint32_t pix_z = getRs(kRsZ);
    const std::uint32_t pix_tex = getTex();
    const std::uint32_t pix_sampler = getSampler(lift::d3d9::kSampMagFilter);
    const std::uint32_t pix_texstage = getTexStage(lift::d3d9::kTssColorOp);
    setRs(kRsZ, 1u);
    setTex(tex_b);
    setFvf(kFvfB);
    setMatrix(mat_b);
    setSampler(lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint);
    setTexStage(lift::d3d9::kTssColorOp, lift::d3d9::kTopModulate);
    setViewport(8u, 8u, 32u, 32u);
    const std::vector<std::uint8_t> keep_viewport = getViewport();
    LIFT_CHECK_EQ_U32(f.method(blk_pixel, 0x14u, {}), 0u);
    LIFT_CHECK_EQ_U32(getRs(kRsZ), pix_z);   // 像素侧：回来了
    LIFT_CHECK_EQ_U32(getTex(), pix_tex);
    LIFT_CHECK_EQ_U32(getSampler(lift::d3d9::kSampMagFilter), pix_sampler);
    LIFT_CHECK_EQ_U32(getTexStage(lift::d3d9::kTssColorOp), pix_texstage);
    LIFT_CHECK_EQ_U32(getFvf(), kFvfB);      // 顶点侧：⛔ PIXEL 块不许碰
    LIFT_CHECK(getMatrix() == mat_b);
    LIFT_CHECK(getViewport() == keep_viewport);  // 视口归 VERTEX ⇒ PIXEL 块不许碰
    LIFT_CHECK_EQ_U32(f.method(blk_pixel, 0x08u, {}), 0u);

    // ---- ③b 掩码：VERTEX 只回滚顶点侧 ----
    setRs(kRsZ, 0u);
    setTex(tex_a);
    setFvf(kFvfA);
    setMatrix(mat_a);
    setSampler(lift::d3d9::kSampMagFilter, lift::d3d9::kTexfLinear);
    setViewport(0u, 0u, 64u, 64u);
    setScissor(0u, 0u, 64u, 64u);
    const std::uint32_t blk_vertex = createBlock(3u);  // D3DSBT_VERTEXSTATE
    LIFT_CHECK_NE_U32(blk_vertex, 0u);
    if (blk_vertex == 0) return;
    const std::uint32_t vtx_fvf = getFvf();
    const std::vector<std::uint8_t> vtx_mat = getMatrix();
    const std::vector<std::uint8_t> vtx_viewport = getViewport();
    const std::vector<std::uint8_t> vtx_scissor = getScissor();
    setRs(kRsZ, 1u);
    setTex(tex_b);
    setFvf(kFvfB);
    setMatrix(mat_b);
    setSampler(lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint);
    setViewport(8u, 8u, 32u, 32u);
    setScissor(4u, 4u, 20u, 20u);
    const std::uint32_t keep_z = getRs(kRsZ);
    const std::uint32_t keep_tex = getTex();
    const std::uint32_t keep_sampler = getSampler(lift::d3d9::kSampMagFilter);
    LIFT_CHECK_EQ_U32(f.method(blk_vertex, 0x14u, {}), 0u);
    LIFT_CHECK_EQ_U32(getFvf(), vtx_fvf);  // 顶点侧：回来了
    LIFT_CHECK(getMatrix() == vtx_mat);
    LIFT_CHECK(getViewport() == vtx_viewport);
    LIFT_CHECK(getScissor() == vtx_scissor);
    LIFT_CHECK_EQ_U32(getRs(kRsZ), keep_z);  // 像素侧：⛔ VERTEX 块不许碰
    LIFT_CHECK_EQ_U32(getTex(), keep_tex);
    LIFT_CHECK_EQ_U32(getSampler(lift::d3d9::kSampMagFilter), keep_sampler);
    LIFT_CHECK_EQ_U32(f.method(blk_vertex, 0x08u, {}), 0u);

    // ---- ④ 快照替 guest 持一份绑定引用 + 块销毁时成对还回 ----
    setTex(tex_a);
    const std::uint32_t rc_before = com->refCount(tex_a);  // guest 自己 1 + 设备绑定 1
    const std::uint32_t blk_hold = createBlock(1u);
    LIFT_CHECK_NE_U32(blk_hold, 0u);
    if (blk_hold == 0) return;
    LIFT_CHECK_EQ_U32(com->refCount(tex_a), rc_before + 1u);   // ← 快照也持一份
    LIFT_CHECK_EQ_U32(f.method(tex_a, 0x08u, {}), rc_before);  // guest 放掉自己那份
    LIFT_CHECK(com->find(tex_a));                              // 快照 + 设备仍然钉住它
    // 块活着 ⇒ Apply 还能把"已经不在 guest 手里"的那张纹理写回去
    setTex(tex_b);
    LIFT_CHECK_EQ_U32(f.method(blk_hold, 0x14u, {}), 0u);
    LIFT_CHECK_EQ_U32(getTex(), tex_a);
    // Release 块 ⇒ 快照那份成对还回 ⇒ 只剩设备绑定那份
    LIFT_CHECK_EQ_U32(f.method(blk_hold, 0x08u, {}), 0u);
    LIFT_CHECK(!com->find(blk_hold));
    LIFT_CHECK(com->find(tex_a));
    // 解绑 ⇒ tex_a 的最后一份还回 ⇒ 对象 / `'TEX1'` / 像素块一起回收
    setTex(0u);
    LIFT_CHECK(!com->find(tex_a));
    // tex_b 的 guest 那份也放掉 ⇒ 两张纹理都回收，guest 堆回到建纹理之前的基线
    LIFT_CHECK_EQ_U32(f.method(tex_b, 0x08u, {}), 0u);
    LIFT_CHECK(!com->find(tex_b));
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_before_tex);
}

// ---------------------------------------------------------------------------------------
// ㉑ IDirect3DStateBlock9 的 vtable **上界哨兵**（`REQ-01M7372P560TMY51NJ3ARG4ENG` 第 2 处）
//
// 判据（红了意味着什么）：`IDirect3DStateBlock9` 的真实 vtable 到 `GetDescription`（+0x18）为止。
//   方法表只填到 +0x14 时 vtable 只有 6 槽 ⇒ guest `call [vtbl+0x18]` 读到的是**堆里的下一个 DWORD**
//   （不是我们发的 stub）⇒ **间接跳到任意 EA**。修后 +0x18 是显式哨兵（fn=nullptr）：
//   ① vtable[0x18] 指向的 stub 注册名 = `IDirect3DStateBlock9::GetDescription+0x18`；
//   ② `ShimDef::fn == &comUnsupported`（loud trap，⛔ 不是"返回 0 的实现"、⛔ 不是野跳）；
//   ③ 从 guest 真的 call 它 ⇒ 机器停（`Trap` / 非 ok），`stop_why` 与 `detail` 都带 iface + 偏移。
// ---------------------------------------------------------------------------------------
void caseStateBlockVtableSentinel() {
    lift_test::g_current_case = "d3d9/stateblock-vtable-sentinel";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t obj = f.createDirect3D();
    LIFT_CHECK_NE_U32(obj, 0u);
    if (obj == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(obj, 1u, 0x44u, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    // 建一个状态块（ALL）；vtable 由 createObject 发，槽数 = max_offset/4 + 1
    const std::uint32_t out = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0xECu, {1u, out}), 0u);
    const std::uint32_t blk = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(blk, 0u);
    LIFT_CHECK_NE_U32(blk, 0xDEADBEEFu);
    if (blk == 0 || blk == 0xDEADBEEFu) return;

    const std::uint32_t vtbl = f.m.mem().rd32(blk);
    LIFT_CHECK_NE_U32(vtbl, 0u);
    // ① +0x18 必须是**我们发的**哨兵 stub，不是 vtable 之外的堆字节
    const std::uint32_t stub = f.m.mem().rd32(vtbl + 0x18u);
    const lift::ShimDef* def = f.m.shims().byStubEa(stub);
    LIFT_CHECK(def != nullptr);
    if (def) {
        LIFT_CHECK(def->name.find("IDirect3DStateBlock9") != std::string::npos);
        LIFT_CHECK(def->name.find("+0x18") != std::string::npos);
        LIFT_CHECK(def->name.find("GetDescription") != std::string::npos);
        // ② loud trap 口径：⛔ 不野跳、⛔ 不静默返回 0
        LIFT_CHECK(def->fn == &lift::comUnsupported);
    }

    // ③ 从 guest 调 `blk[+0x18](&type)`：必须停在 loud trap 上
    const std::uint32_t type_ea = f.placeU32(0xDEADBEEFu);
    Asm a;
    a.movEaxImm(vtbl);
    a.pushImm(type_ea);
    a.pushImm(blk);
    a.callMemEaxDisp8(0x18u);
    a.ret();
    lift::RunResult r;
    f.runAsmRaw(a, &r);
    LIFT_CHECK(r.reason != lift::StopReason::UntilEa);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.reason), static_cast<std::uint32_t>(lift::StopReason::Trap));
    LIFT_CHECK(!r.ok());
    LIFT_CHECK(f.m.cpu().running == false);
    LIFT_CHECK(std::string(f.m.cpu().stop_why ? f.m.cpu().stop_why : "") == "unimplemented COM vtable slot");
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(f.m.cpu().shim.unimplemented_calls), 1u);
    LIFT_CHECK(r.detail.find("IDirect3DStateBlock9") != std::string::npos);
    LIFT_CHECK(r.detail.find("+0x18") != std::string::npos);
    // trap 之前不许写坏出参（trap 不是"实现"，没有写 pType 的资格）
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(type_ea), 0xDEADBEEFu);
}

// ---------------------------------------------------------------------------------------
// ㉒ VB/IB `Lock` 的 `OffsetToLock + SizeToLock` 越界（`REQ-01M7372P560TMY51NJ3ARG4ENG` 第 3 处）
//
// 判据（红了意味着什么）：修前越界只把 size **夹到剩余长度**、照样 `kOk`（还置了锁）⇒ guest 以为
//   "锁了 16 字节"，之后写到缓冲末尾之外、踩相邻 guest 堆，而返回值装作成功。
//   ① `CreateVertexBuffer(32)` + `Lock(24,16)` ⇒ `D3DERR_INVALIDCALL`（24+16 > 32）；
//   ② 失败**不置锁**：紧接着 `Lock(24,0)` 仍 `S_OK`（`size == 0` = 锁到尾部，合法）；
//   ③ `Lock(0,32)`（正好整条）仍 `S_OK`；
//   ④ `Lock(32,4)`（offset == 长度 + 非 0 长度）/ `Lock(33,0)`（offset 越界）⇒ INVALIDCALL；
//   ⑤ 索引缓冲同口径（旧实现**完全不看** size）。
// ---------------------------------------------------------------------------------------
void caseLockOverrunInvalidCall() {
    lift_test::g_current_case = "d3d9/lock-overrun-invalidcall";
    Fixture f;
    LIFT_CHECK(f.boot());
    const std::uint32_t obj = f.createDirect3D();
    LIFT_CHECK_NE_U32(obj, 0u);
    if (obj == 0) return;
    Fixture::Device dev;
    LIFT_CHECK(f.createDevice(obj, 1u, 0x44u, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev));
    if (dev.object == 0) return;

    const std::uint32_t heap_base = f.m.heap().bytesInUse();

    // Lock(this, OffsetToLock, SizeToLock, ppbData, Flags) —— 返回 hr，*data_out = ppbData
    auto lock = [&f](std::uint32_t target, std::uint32_t offset, std::uint32_t size, std::uint32_t* data_out) {
        const std::uint32_t out_ea = f.placeU32(0xDEADBEEFu);
        const std::uint32_t hr = f.method(target, 0x2Cu, {offset, size, out_ea, 0u});
        if (data_out) *data_out = f.m.mem().rd32(out_ea);
        return hr;
    };
    auto unlock = [&f](std::uint32_t target) { return f.method(target, 0x30u, {}); };

    // ---- 顶点缓冲：CreateVertexBuffer(32) ----
    const std::uint32_t out = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x68u, {32u, 8u, 0x0044u, 1u, out, 0u}), 0u);
    const std::uint32_t vb = f.m.mem().rd32(out);
    LIFT_CHECK_NE_U32(vb, 0u);
    if (vb == 0) return;

    // 基准：offset 0 的 ppbData（后面用它核对 offset 24 的指针，⛔ 不硬编码布局偏移）
    std::uint32_t data0 = 0;
    LIFT_CHECK_EQ_U32(lock(vb, 0u, 0u, &data0), lift::d3d9::kOk);
    LIFT_CHECK_NE_U32(data0, 0u);
    LIFT_CHECK_EQ_U32(unlock(vb), lift::d3d9::kOk);

    // ① 越界 ⇒ INVALIDCALL（修前：clamp 成 8 并返回 S_OK）
    LIFT_CHECK_EQ_U32(lock(vb, 24u, 16u, nullptr), lift::d3d9::kInvalidCall);
    // ② 失败不置锁 ⇒ 紧接着的合法 Lock 必须成功（修前上面那笔已置锁 ⇒ 这里 INVALIDCALL）
    std::uint32_t data24 = 0;
    LIFT_CHECK_EQ_U32(lock(vb, 24u, 0u, &data24), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(data24, data0 + 24u);  // size == 0 = 锁到尾部，起点仍然是 data+offset
    LIFT_CHECK_EQ_U32(unlock(vb), lift::d3d9::kOk);
    // ③ 正好整条仍合法
    LIFT_CHECK_EQ_U32(lock(vb, 0u, 32u, nullptr), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(unlock(vb), lift::d3d9::kOk);
    // ④ 边界：offset == length 但 size != 0 / offset > length
    LIFT_CHECK_EQ_U32(lock(vb, 32u, 4u, nullptr), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(lock(vb, 33u, 0u, nullptr), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(f.method(vb, 0x08u, {}), 0u);  // Release

    // ---- 索引缓冲：同口径（CreateIndexBuffer(32, D3DFMT_INDEX16=101)）----
    const std::uint32_t out_ib = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(dev.object, 0x6Cu, {32u, 8u, 101u, 1u, out_ib, 0u}), 0u);
    const std::uint32_t ib = f.m.mem().rd32(out_ib);
    LIFT_CHECK_NE_U32(ib, 0u);
    if (ib == 0) return;
    LIFT_CHECK_EQ_U32(lock(ib, 24u, 16u, nullptr), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(lock(ib, 24u, 0u, nullptr), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(unlock(ib), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(lock(ib, 0u, 32u, nullptr), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(unlock(ib), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(f.method(ib, 0x08u, {}), 0u);
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_base);  // 两个缓冲的状态块都回堆
}

// =======================================================================================
// D3D9 点/线光栅守卫（`REQ-01M7372JNFAC2RWX2FXVV6YM5C`）
//
// 修前：`submitPrimitives` 对 POINTLIST/LINELIST/LINESTRIP 只 `recordUnsupported(bit16)` 然后
// `break` ⇒ 不写像素、不报错、Draw 返回 S_OK（假成功）。而引擎有两条实测线站点
// （0x48E16D = LINELIST 1 段、0x48E1F5 = LINESTRIP 16 段，FVF 都是 0x42 = XYZ|DIFFUSE）。
//
// 判据（红了意味着什么）：
//   ① `d3d9/linelist-writes-pixels`：两个 XYZ|DIFFUSE 顶点映射到屏幕 (8.5,32.5)–(56.5,32.5)，
//      Draw(LINELIST,1) 之后后缓冲上必须出现 **49** 个非背景像素（主轴每列 1px、含两端），
//      且 (20,32)/(44,32) 的像素色 = 两端点色在 t=0.25/0.75 处的插值；row 31/33 仍是背景
//      （线宽 1px）；x=7/57 仍是背景（端点列归属）。
//      把线分支短路/步进改坏 ⇒ 像素数掉到 0 / 25 ⇒ 红。
//   ② `d3d9/linestrip-16`：LINESTRIP 17 顶点（16 段阶梯，每段 x+3 / y+2）⇒ 不同像素数在
//      [40,80]（16 段 × 3 列 + 1 ≈ 49；给区间，⛔ 不写死绝对值），且首/末顶点像素都被画到。
//   ③ `d3d9/line-textured-loud-reject`：COLOROP=SELECTARG1(TEXTURE)（无纹理绑定时 CPU 会取
//      白色，≠ 顶点色）⇒ `D3DERR_INVALIDCALL` 且一个像素都不写；绑了真纹理（默认
//      MODULATE(TEXTURE,CURRENT)）⇒ 同样拒绝。恢复成"无纹理默认态"⇒ 同一批线能画。
//      ⛔ 不许"画成纯色"糊过去（修前这一支是假 S_OK 且不画）。
// =======================================================================================
constexpr std::uint32_t kDevBeginScene = 0xA4u;
constexpr std::uint32_t kDevEndScene = 0xA8u;
constexpr std::uint32_t kDevClear = 0xACu;
constexpr std::uint32_t kDevSetTexture = 0x104u;
constexpr std::uint32_t kDevDrawPrimitiveUP = 0x14Cu;
constexpr std::uint32_t kDevSetFVF = 0x164u;
constexpr std::uint32_t kFvfXyzDiffuse = 0x0042u;
constexpr std::uint32_t kLineClearArgb = 0xFF102030u;  // X8R8G8B8 ⇒ 存成 0x00102030
constexpr std::uint32_t kLineClearPixel = 0x00102030u;

// XYZ 顶点 + D3DCOLOR（FVF 0x42；stride 可 > 16，尾部补 0）
std::uint32_t placeXyzDiffuseVerts(Fixture& f, const std::vector<std::array<float, 3>>& xyz,
                                   const std::vector<std::uint32_t>& colors, std::uint32_t stride) {
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 0; i < xyz.size(); ++i) {
        for (int k = 0; k < 3; ++k) {
            std::uint32_t bits = 0;
            const float fv = xyz[i][static_cast<std::size_t>(k)];
            std::memcpy(&bits, &fv, sizeof(bits));
            for (int b = 0; b < 4; ++b) bytes.push_back(static_cast<std::uint8_t>((bits >> (8 * b)) & 0xFF));
        }
        for (int b = 0; b < 4; ++b)
            bytes.push_back(static_cast<std::uint8_t>((colors[i] >> (8 * b)) & 0xFF));
        for (std::uint32_t pad = 16u; pad < stride; ++pad) bytes.push_back(0u);
    }
    return f.placeBytes(bytes);
}

// 像素色逐通道容差 ±1（float→8bit 的取整边界；⛔ 不是"随便给个范围"）
void checkPixelNear(Fixture& f, const lift::d3d9::DeviceInfo& d, int x, int y, unsigned er, unsigned eg,
                    unsigned eb) {
    const std::uint32_t raw = f.pixelAt(d, x, y);
    const unsigned r = (raw >> 16) & 0xFFu;
    const unsigned g = (raw >> 8) & 0xFFu;
    const unsigned b = raw & 0xFFu;
    ++lift_test::g_checks;
    const bool ok = r <= er + 1u && r + 1u >= er && g <= eg + 1u && g + 1u >= eg && b <= eb + 1u &&
                    b + 1u >= eb;
    if (!ok) {
        lift_test::failf(__FILE__, __LINE__, "pixel(%d,%d) expected ≈(%u,%u,%u), got (%u,%u,%u)", x, y,
                         er, eg, eb, r, g, b);
    }
}

// 一次"64×64 设备 + 清屏 + BeginScene + SetFVF(0x42)"（点/线站点同款）
struct LineRig {
    Fixture f;
    Fixture::Device dev;

    bool up() {
        if (!f.boot()) return false;
        const std::uint32_t d3d9 = f.createDirect3D();
        if (d3d9 == 0) return false;
        if (!f.createDevice(d3d9, 1u, 0x44u, 64, 64, lift::d3d9::kFmtX8R8G8B8, &dev)) return false;
        if (dev.object == 0) return false;
        LIFT_CHECK_EQ_U32(f.method(dev.object, kDevClear, {0u, 0u, 3u, kLineClearArgb, 0x3F800000u, 0u}),
                          lift::d3d9::kOk);
        LIFT_CHECK_EQ_U32(f.method(dev.object, kDevBeginScene, {}), lift::d3d9::kOk);
        LIFT_CHECK_EQ_U32(f.method(dev.object, kDevSetFVF, {kFvfXyzDiffuse}), lift::d3d9::kOk);
        return true;
    }
    std::uint32_t drawUp(std::uint32_t prim, std::uint32_t prim_count, std::uint32_t data,
                         std::uint32_t stride) {
        return f.method(dev.object, kDevDrawPrimitiveUP, {prim, prim_count, data, stride});
    }
    std::uint32_t endScene() { return f.method(dev.object, kDevEndScene, {}); }
    int countNonBackground() {
        int count = 0;
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                if (f.pixelAt(dev.info, x, y) != kLineClearPixel) ++count;
        return count;
    }
};

// 屏幕坐标 → 裁剪坐标（缺省 viewport = 64×64、身份 WORLD/VIEW/PROJECTION）：
//   sx = (x*0.5+0.5)*64 = 32x+32、sy = (-y*0.5+0.5)*64 = 32-32y
std::array<float, 3> clipFromScreen(double sx, double sy, double z) {
    return {static_cast<float>(sx / 32.0 - 1.0), static_cast<float>(1.0 - sy / 32.0),
            static_cast<float>(z)};
}

void caseLineListWritesPixels() {
    lift_test::g_current_case = "d3d9/linelist-writes-pixels";
    LineRig r;
    LIFT_CHECK(r.up());
    if (r.dev.object == 0) return;
    // 水平线：屏幕 (8.5,32.5) → (56.5,32.5)；z 0.25 → 0.75；色 (10,20,30) → (210,220,230)
    const std::uint32_t data = placeXyzDiffuseVerts(
        r.f, {clipFromScreen(8.5, 32.5, 0.25), clipFromScreen(56.5, 32.5, 0.75)},
        {0xFF0A141Eu, 0xFFD2DCE6u}, 16u);
    LIFT_CHECK_EQ_U32(r.drawUp(lift::d3d9::kPtLineList, 1u, data, 16u), lift::d3d9::kOk);

    // ① 主轴像素 = floor(8.5)..floor(56.5) 含两端 = 49；线宽 1px ⇒ row 31/33 仍是背景。
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.countNonBackground()), 49u);
    for (int x = 7; x <= 57; ++x) {
        LIFT_CHECK_EQ_U32(r.f.pixelAt(r.dev.info, x, 31), kLineClearPixel);
        LIFT_CHECK_EQ_U32(r.f.pixelAt(r.dev.info, x, 33), kLineClearPixel);
    }
    // ② 两端点列归属：x=7 / x=57 都不该被画（含 floor(起点) 与 floor(终点)，不含邻列）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(r.dev.info, 7, 32), kLineClearPixel);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(r.dev.info, 57, 32), kLineClearPixel);
    // ③ 颜色 = 两端点色按 t 的插值（t = (x+0.5−8.5)/48）：t=0.25 ⇒ (60,70,80)；t=0.75 ⇒ (160,170,180)
    checkPixelNear(r.f, r.dev.info, 20, 32, 60u, 70u, 80u);
    checkPixelNear(r.f, r.dev.info, 44, 32, 160u, 170u, 180u);
    // ④ z 也按 t 线性插值（深度缓冲里是 float；0.25→0.75 在 t=0.5 ⇒ 0.5）：用线中点像素核对。
    lift::d3d9::PipelineInfo pi;
    std::string why;
    if (lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.dev.state, &pi, &why)) {
        const float* depth = reinterpret_cast<const float*>(
            r.f.m.mem().host(pi.depth_arena));
        LIFT_CHECK(depth != nullptr);
        if (depth != nullptr) {
            const float z = depth[static_cast<std::size_t>(32) * 64u + 32u];
            LIFT_CHECK(z > 0.49f && z < 0.51f);  // t=(32.5-8.5)/48=0.5 ⇒ 0.25+0.5*0.5
        }
    } else {
        lift_test::failf(__FILE__, __LINE__, "pipelineInfoOfDevice 失败：%s", why.c_str());
    }
    // ⑤ 深度测试**共用**：同一段线用更远的 z（0.9）再画一次 ⇒ 一个像素都不许改
    //    （缺省 ZFUNC = LESSEQUAL；第一遍的 z ∈ [0.25,0.75] 全比 0.9 近）。
    const std::uint32_t far_data = placeXyzDiffuseVerts(
        r.f, {clipFromScreen(8.5, 32.5, 0.9), clipFromScreen(56.5, 32.5, 0.9)},
        {0xFFFF0000u, 0xFFFF0000u}, 16u);
    LIFT_CHECK_EQ_U32(r.drawUp(lift::d3d9::kPtLineList, 1u, far_data, 16u), lift::d3d9::kOk);
    checkPixelNear(r.f, r.dev.info, 20, 32, 60u, 70u, 80u);
    checkPixelNear(r.f, r.dev.info, 44, 32, 160u, 170u, 180u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.countNonBackground()), 49u);
    // ⑥ 混合**共用**：开 ALPHABLEND（SRCALPHA / INVSRCALPHA），在清屏色 (16,32,48) 上画
    //    α=128 的 (200,100,0) ⇒ (200*128 + 16*127 + 127)/255 = 108、g = 66、b = 24
    //    （与三角形走同一个 `applyBlendOp`；⛔ 不是线的私有公式）。
    LIFT_CHECK_EQ_U32(r.f.method(r.dev.object, 0xE4u, {27u, 1u}), lift::d3d9::kOk);  // ALPHABLENDENABLE
    LIFT_CHECK_EQ_U32(r.f.method(r.dev.object, 0xE4u, {19u, 5u}), lift::d3d9::kOk);  // SRCBLEND=SRCALPHA
    LIFT_CHECK_EQ_U32(r.f.method(r.dev.object, 0xE4u, {20u, 6u}), lift::d3d9::kOk);  // DESTBLEND=INVSRCALPHA
    const std::uint32_t blend_data = placeXyzDiffuseVerts(
        r.f, {clipFromScreen(8.5, 40.5, 0.5), clipFromScreen(56.5, 40.5, 0.5)},
        {0x80C86400u, 0x80C86400u}, 16u);
    LIFT_CHECK_EQ_U32(r.drawUp(lift::d3d9::kPtLineList, 1u, blend_data, 16u), lift::d3d9::kOk);
    checkPixelNear(r.f, r.dev.info, 20, 40, 108u, 66u, 24u);
    LIFT_CHECK_EQ_U32(r.endScene(), lift::d3d9::kOk);
}

void caseLineStrip16() {
    lift_test::g_current_case = "d3d9/linestrip-16";
    LineRig r;
    LIFT_CHECK(r.up());
    if (r.dev.object == 0) return;
    // 17 个顶点 = 16 段阶梯（每段屏幕 x+3 / y+2）：每段主轴（x）3 列 + 两端共享 ⇒ ≈ 16*3+1
    std::vector<std::array<float, 3>> xyz;
    std::vector<std::uint32_t> colors;
    for (int k = 0; k <= 16; ++k) {
        xyz.push_back(clipFromScreen(4.0 + 3.0 * static_cast<double>(k),
                                     4.0 + 2.0 * static_cast<double>(k), 0.5));
        colors.push_back(0xFFFFFFFFu);
    }
    const std::uint32_t data = placeXyzDiffuseVerts(r.f, xyz, colors, 16u);
    LIFT_CHECK_EQ_U32(r.drawUp(lift::d3d9::kPtLineStrip, 16u, data, 16u), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(r.endScene(), lift::d3d9::kOk);

    // 判据是**区间**（⛔ 不写死绝对值）：16 段 × 约 3 列 + 1 ≈ 49；掉 1 段即掉出区间。
    const int count = r.countNonBackground();
    LIFT_CHECK(count >= 40);
    LIFT_CHECK(count <= 80);
    // 首段起点与末段终点都必须画到 ⇒ 证明 16 段都走了（不是只画了第一段）
    LIFT_CHECK_NE_U32(r.f.pixelAt(r.dev.info, 4, 4), kLineClearPixel);
    LIFT_CHECK_NE_U32(r.f.pixelAt(r.dev.info, 52, 36), kLineClearPixel);
}

// 主轴 / 方向 / 45° 三条分支（`rasterizeLineSegment` 的实现细节里最容易写错的三处）：
//   ① 竖线 ⇒ 主轴是 y（`|dy| > |dx|`），像素数 = 主轴跨度；② 右→左的 LINELIST ⇒ dir=-1 也必须
//   每列一个像素；③ 45° 平局 ⇒ 主轴取 x，对角一个像素、不能 2px。
void caseLineAxesAndDirection() {
    lift_test::g_current_case = "d3d9/line-axes-and-direction";
    LineRig r;
    LIFT_CHECK(r.up());
    if (r.dev.object == 0) return;
    // ① 竖线 (32.5, 8.5) → (32.5, 56.5)：主轴 y = 8..56 ⇒ 49 px
    const std::uint32_t v = placeXyzDiffuseVerts(
        r.f, {clipFromScreen(32.5, 8.5, 0.5), clipFromScreen(32.5, 56.5, 0.5)},
        {0xFFFFFFFFu, 0xFFFFFFFFu}, 16u);
    LIFT_CHECK_EQ_U32(r.drawUp(lift::d3d9::kPtLineList, 1u, v, 16u), lift::d3d9::kOk);
    // ② **反向** LINELIST（右→左）：(56.5, 48.5) → (8.5, 48.5)，主轴向负 ⇒ 仍 49 px
    const std::uint32_t rev = placeXyzDiffuseVerts(
        r.f, {clipFromScreen(56.5, 48.5, 0.5), clipFromScreen(8.5, 48.5, 0.5)},
        {0xFFD2DCE6u, 0xFF0A141Eu}, 16u);  // 右端 B、左端 A
    LIFT_CHECK_EQ_U32(r.drawUp(lift::d3d9::kPtLineList, 1u, rev, 16u), lift::d3d9::kOk);
    // ③ 45°（|dx| == |dy|）：(4.5,4.5) → (24.5,24.5)，每列一个像素 ⇒ 21 px
    const std::uint32_t diag = placeXyzDiffuseVerts(
        r.f, {clipFromScreen(4.5, 4.5, 0.5), clipFromScreen(24.5, 24.5, 0.5)},
        {0xFFFFFFFFu, 0xFFFFFFFFu}, 16u);
    LIFT_CHECK_EQ_U32(r.drawUp(lift::d3d9::kPtLineList, 1u, diag, 16u), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(r.endScene(), lift::d3d9::kOk);

    // ① 竖线：49 px、列 31/33 是背景（1px），(32,20) 被画
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.f.pixelAt(r.dev.info, 32, 20)), 0x00FFFFFFu);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(r.dev.info, 31, 20), kLineClearPixel);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(r.dev.info, 33, 20), kLineClearPixel);
    // ② 反向水平线：方向不影响 t 的插值 ⇒ (20,48) 仍是左端 A→右端 B 的 t=0.25
    checkPixelNear(r.f, r.dev.info, 20, 48, 60u, 70u, 80u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(r.dev.info, 7, 48), kLineClearPixel);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(r.dev.info, 57, 48), kLineClearPixel);
    // ③ 45°：对角相邻行必须是背景（若主轴/次轴写反会变成 2px 或缺口）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(r.dev.info, 10, 10), 0x00FFFFFFu);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(r.dev.info, 10, 11), kLineClearPixel);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(r.dev.info, 24, 24), 0x00FFFFFFu);
    // 总数 = 49 + 49 + 21（三条线互不重叠：列 32 与行 48 的交点 (32,48) 是同一像素、被写两次）
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.countNonBackground()), 49u + 49u + 21u - 1u);
}

void caseLineTexturedLoudReject() {
    lift_test::g_current_case = "d3d9/line-textured-loud-reject";
    LineRig r;
    LIFT_CHECK(r.up());
    if (r.dev.object == 0) return;
    const std::uint32_t data = placeXyzDiffuseVerts(
        r.f, {clipFromScreen(8.5, 32.5, 0.5), clipFromScreen(56.5, 32.5, 0.5)},
        {0xFFFFFFFFu, 0xFFFFFFFFu}, 16u);

    // ① COLOROP = SELECTARG1(TEXTURE)、无纹理绑定：CPU 会取白色（≠ 顶点色）⇒ 必须**响亮拒绝**。
    LIFT_CHECK_EQ_U32(r.f.method(r.dev.object, 0x10Cu, {0u, 1u, lift::d3d9::kTopSelectArg1}),
                      lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(r.f.method(r.dev.object, 0x10Cu, {0u, 2u, lift::d3d9::kTaTexture}),
                      lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(r.drawUp(lift::d3d9::kPtLineList, 1u, data, 16u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.countNonBackground()), 0u);  // ⛔ 不是"画成纯色"

    // ② 恢复 stage0 默认 MODULATE(TEXTURE, CURRENT)（无纹理 ⇒ 恒等于顶点色）⇒ 同一批线能画。
    LIFT_CHECK_EQ_U32(r.f.method(r.dev.object, 0x10Cu, {0u, 1u, lift::d3d9::kTopModulate}),
                      lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(r.f.method(r.dev.object, 0x10Cu, {0u, 2u, lift::d3d9::kTaTexture}),
                      lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(r.drawUp(lift::d3d9::kPtLineList, 1u, data, 16u), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.countNonBackground()), 49u);

    // ③ 绑了真纹理（即使 op 仍是"无纹理时恒等于顶点色"的 MODULATE(TEXTURE,CURRENT)）⇒ 拒绝。
    lift::registerD3DX9Shims(r.f.m.shims());
    lift::ShimDef* dx = r.f.m.shims().findMutable("d3dx9_43", "D3DXCreateTexture");
    LIFT_CHECK(dx != nullptr);
    if (dx == nullptr) return;
    dx->stub_ea = kProcD3DXCreateTexture;
    r.f.m.cache().installShim(kProcD3DXCreateTexture, dx->id);
    const std::uint32_t out_ea = r.f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(callD3DXCreateTexture(r.f, r.dev.object, 4u, 4u, lift::d3d9::kFmtA8R8G8B8, out_ea),
                      lift::d3d9::kOk);
    const std::uint32_t tex = r.f.m.mem().rd32(out_ea);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    LIFT_CHECK_EQ_U32(r.f.method(r.dev.object, kDevSetTexture, {0u, tex}), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(r.drawUp(lift::d3d9::kPtLineList, 1u, data, 16u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(r.countNonBackground()), 49u);  // 第二次那 49 px 还在
    // 解绑 ⇒ 又能画（证明拒绝是**状态相关**的，不是"所有线都拒"）
    LIFT_CHECK_EQ_U32(r.f.method(r.dev.object, kDevSetTexture, {0u, 0u}), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(r.drawUp(lift::d3d9::kPtLineList, 1u, data, 16u), lift::d3d9::kOk);
    LIFT_CHECK(r.countNonBackground() >= 49);
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 0u);  // guest 自己那份 Release
}

}  // namespace

int main() {
    using namespace lift_test;
    try {
        caseCreateAndQueryInterface();
        caseAdapterDisplayMode();
        caseCreateDeviceAndCaps();
        caseCheckDeviceFormatTruthful();
        caseClear();
        casePresent();
        caseFrameDump();
        caseVertexStreamBinding();
        caseVertexDeclaration();
        caseGetBackBufferAndRenderTarget();
        caseBackBufferLockRect();
        caseOffscreenSurfaceRenderTargetData();
        caseTextureBoundThenGuestRelease();
        caseArenaReclaimUnderOtherMachine();
        caseUnimplementedSlotTraps();
        caseResourceGetDeviceRefcount();
        caseVertexDeclarationGetRefcount();
        caseGetTextureRefcount();
        caseResourceOnZeroInternalRelease();
        caseGetTextureStaleGeneration();
        caseSetFVFReleasesDecl();
        caseVertexDeclarationStaleRebind();
        caseStateBlockCaptureApply();
        caseStateBlockVtableSentinel();
        caseLockOverrunInvalidCall();
        caseLineListWritesPixels();
        caseLineStrip16();
        caseLineAxesAndDirection();
        caseLineTexturedLoudReject();
        return finish("d3d9");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "DIAG uncaught std::exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "DIAG uncaught non-std exception\n");
        return 1;
    }
}
