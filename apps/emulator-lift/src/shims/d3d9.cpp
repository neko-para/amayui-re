// emulator-lift · D3D9 的导出 + COM 方法 shim（M2 · 第一帧的前置）
//
// ★ 入口是**静态导入**：`d3d9!Direct3DCreate9`（本安装**唯一**的 d3d9 导入；`.lst` 0x498D0F）。
//   它由 `registerD3D9Shims` 登记进来（IAT 桩页），⛔ 不是动态导出表。
//
// ★ 方法表（偏移 = 槽序号*4；槽 0/1/2 = IUnknown）。**每一条都有 `.lst` 站点或明说"未取证"**：
//   IDirect3D9（SDK 顺序：IUnknown 3 + 14 个方法 ⇒ 17 槽，最后一个 CreateDevice 在 +0x40）
//     +0x00 QueryInterface / +0x04 AddRef / +0x08 Release   （0x498D01 用的是 [vtable+8] Release）
//     +0x10 GetAdapterCount                    ← 未取证（引擎不调；为"适配器数 = 1"给真值）
//     +0x18 GetAdapterModeCount   ★ 0x498462（`mov eax,[ecx+18h]`）→ 0x498468 call
//     +0x1C EnumAdapterModes      ★ 0x498486（`mov eax,[ecx+1Ch]`）→ 0x498489 call
//     +0x20 GetAdapterDisplayMode ★ 0x498D41（`mov eax,[ecx+20h]`）→ 0x498D44 call
//     +0x28 CheckDeviceFormat     ★ 0x49918D（文档 EA；本段 boot 还没走到）
//     +0x40 CreateDevice          ★ 0x497F44（`mov edx,[esi+40h]`）→ 0x497F47 call
//   IDirect3DDevice9（IUnknown 3 + 116 个方法 ⇒ 119 槽，最后 CreateQuery 在 +0x1D8）
//     +0x1C GetDeviceCaps         ★ 0x499123（`mov eax,[ecx+1Ch]`）→ 0x499126 call（只读 caps+0x58/0x5C）
//     +0x44 Present               ← 取证文档列的槽号（本段 boot 还没到；第一帧判据）
//     +0x50 SetDialogBoxMode      ★ 0x4990C5（`mov eax,[edx+50h]`）→ 0x4990C8 call（参数 = TRUE）
//     +0xAC Clear                 ★ 0x498B6D（`mov edx,[ecx+0ACh]`）→ 0x498B82 call
//                                  （push 序 = Count=0 / pRects=NULL / Flags=3 / Color=0 / Z=1.0f / Stencil=0）
//
// ★ 没填的槽 = `comUnsupported`（loud trap：打印 iface + 偏移 + 寄存器后停机器）。
//   ⛔ 绝不静默返回 0 —— 那会把"方法没实现"伪装成"方法返回 0 / D3DERR"。
// ★ **本文件不"假装成功"**：不支持的多重采样 / 交换效应 / 后缓冲数都返回**真机的失败码**并打日志；
//   没有深度缓冲时 Clear 的 ZBUFFER/STENCIL 位只记录（不谎称清了，见 runtime/d3d9.cpp）。
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "runtime/com.h"
#include "runtime/d3d9.h"
#include "runtime/d3d9_pipeline.h"
#include "runtime/exec.h"
#include "runtime/memory.h"
#include "runtime/x86.h"
#include "shims/register.h"

namespace lift {
namespace {

// ---- IID（16 字节，内存里的字节序）----
// ★ 来源 = SDK d3d9.h（**按头文件推断**）：本段 boot 引擎**不做** QI（0x498D39 直接用返回的指针），
//   所以这两份 GUID 无法用 `.lst` 交叉验证。夹具用同一份常量做 QI（正反两向：本 IID 成功、别的 E_NOINTERFACE）。
// IID_IDirect3D9 = {81BDCBCA-64D4-426D-AE8D-AD0147F4275C}
constexpr std::uint8_t kIidDirect3D9[16] = {0xCA, 0xCB, 0xBD, 0x81, 0xD4, 0x64, 0x6D, 0x42,
                                            0xAE, 0x8D, 0xAD, 0x01, 0x47, 0xF4, 0x27, 0x5C};
// IID_IDirect3DDevice9 = {D0223B96-BF7A-43FD-92BD-A43B0D82B9EB}
constexpr std::uint8_t kIidDirect3DDevice9[16] = {0x96, 0x3B, 0x22, 0xD0, 0x7A, 0xBF, 0xFD, 0x43,
                                                  0x92, 0xBD, 0xA4, 0x3B, 0x0D, 0x82, 0xB9, 0xEB};
// IID_IUnknown = {00000000-0000-0000-C000-000000000046}
constexpr std::uint8_t kIidUnknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
// IID_IDirect3DVertexBuffer9 = {B64BB1B5-FD70-4DF6-BF91-19D0A12455E3}（SDK d3d9.h）
constexpr std::uint8_t kIidVertexBuffer9[16] = {0xB5, 0xB1, 0x4B, 0xB6, 0x70, 0xFD, 0xF6, 0x4D,
                                                0xBF, 0x91, 0x19, 0xD0, 0xA1, 0x24, 0x55, 0xE3};
// IID_IDirect3DIndexBuffer9 = {7C9DD65E-D3F7-4529-ACEE-785830ACDE35}（SDK d3d9.h）
constexpr std::uint8_t kIidIndexBuffer9[16] = {0x5E, 0xD6, 0x9D, 0x7C, 0xF7, 0xD3, 0x29, 0x45,
                                               0xAC, 0xEE, 0x78, 0x58, 0x30, 0xAC, 0xDE, 0x35};

// IDirect3DDevice9 的最后一个槽（#118 CreateQuery，+0x1D8 = 118*4）。
// ★ 为什么需要它：`ComRegistry::createObject` 只按**最大偏移**决定 vtable 有多少槽；
//   不显式占到 119 槽的话，引擎一旦调 +0xB0 之后的槽就会读到 vtable **之外**的堆字节
//   （那不是 loud trap，是野指针）。⇒ 用一条 `fn = nullptr` 的哨兵把 vtable 撑到接口的**真实大小**。
constexpr std::uint32_t kLastDeviceSlotOffset = 0x1D8u;

void d3d9Log(const char* method, const char* fmt, ...) {
    std::fprintf(stderr, "[d3d9] %s: ", method);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

// ★ loud trap：状态崩坏 / 参数形态没实现 ⇒ 打印现场并停机器（⛔ 不静默返回 0）。
void d3d9Trap(X86& cpu, const char* method, const char* fmt, ...) {
    std::fprintf(stderr, "\n=== unimplemented D3D9 call ===\n  %s\n  reason: ", method);
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "\n  （⛔ 不静默返回 0；槽级缺口由 comUnsupported 报 iface+偏移）\n");
    dumpRegisters(cpu, stderr);
    dumpGuestStack(cpu, stderr, 24);
    cpu.running = false;
    cpu.stop_why = "unimplemented D3D9 call";
    ++cpu.shim.unimplemented_calls;
}

bool sameGuid(X86& cpu, std::uint32_t ea, const std::uint8_t (&want)[16]) {
    if (ea == 0 || !cpu.mem->committed(ea, 16)) return false;
    std::uint8_t got[16];
    cpu.mem->readBytes(ea, got, sizeof(got));
    for (int i = 0; i < 16; ++i)
        if (got[i] != want[i]) return false;
    return true;
}

// ---------------------------------------------------------------------------------------
// IUnknown
// ---------------------------------------------------------------------------------------
void shimQueryInterface(X86& cpu, ShimCall& call, const std::uint8_t (&face_iid)[16],
                        const char* face_name) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t riid = call.u32(1);
    const std::uint32_t ppv = call.u32(2);
    if (ppv == 0 || !cpu.mem->committed(ppv, 4)) {
        d3d9Log(face_name, "QueryInterface: ppvObj 为 NULL / 未提交 ⇒ E_INVALIDARG");
        call.result = d3d9::kInvalidArg;
        return;
    }
    cpu.mem->wr32(ppv, 0);
    if (riid == 0 || !cpu.mem->committed(riid, 16)) {
        d3d9Log(face_name, "QueryInterface: riid 为 NULL / 未提交 ⇒ E_INVALIDARG");
        call.result = d3d9::kInvalidArg;
        return;
    }
    const bool wanted = sameGuid(cpu, riid, face_iid) || sameGuid(cpu, riid, kIidUnknown);
    if (!wanted) {
        std::uint8_t got[16];
        cpu.mem->readBytes(riid, got, sizeof(got));
        d3d9Log(face_name,
                "QueryInterface: riid={%08X-%04X-%04X-...} 不是 %s 的接口 ⇒ E_NOINTERFACE",
                static_cast<unsigned>(got[0] | (got[1] << 8) | (got[2] << 16) |
                                      (static_cast<unsigned>(got[3]) << 24)),
                static_cast<unsigned>(got[4] | (got[5] << 8)),
                static_cast<unsigned>(got[6] | (got[7] << 8)), face_name);
        call.result = d3d9::kNoInterface;
        return;
    }
    if (!cpu.com || !cpu.com->addRef(cpu, self)) {
        d3d9Log(face_name, "QueryInterface: this=0x%08X 不是 ComRegistry 建出来的对象", self);
        call.result = d3d9::kNoInterface;
        return;
    }
    cpu.mem->wr32(ppv, self);
    call.result = d3d9::kOk;
}

void shimQueryInterface9(X86& cpu, ShimCall& call) {
    shimQueryInterface(cpu, call, kIidDirect3D9, "IDirect3D9");
}
void shimQueryInterfaceDevice9(X86& cpu, ShimCall& call) {
    shimQueryInterface(cpu, call, kIidDirect3DDevice9, "IDirect3DDevice9");
}
void shimQueryInterfaceVertexBuffer9(X86& cpu, ShimCall& call) {
    shimQueryInterface(cpu, call, kIidVertexBuffer9, "IDirect3DVertexBuffer9");
}
void shimQueryInterfaceIndexBuffer9(X86& cpu, ShimCall& call) {
    shimQueryInterface(cpu, call, kIidIndexBuffer9, "IDirect3DIndexBuffer9");
}

void shimAddRef(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (!cpu.com || !cpu.com->addRef(cpu, self)) {
        call.result = 0;
        return;
    }
    call.result = cpu.com->refCount(self);
}

// Release：到 0 时顺手回收设备状态块（后缓冲像素 + 状态块；对象本身由 ComRegistry 释放）
void shimRelease(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (!cpu.com || !cpu.com->find(self)) {
        d3d9Log("Release", "this=0x%08X 不是本 ComRegistry 的对象（重复 Release / 野指针）⇒ 返回 0", self);
        call.result = 0;
        return;
    }
    const std::uint32_t user = cpu.com->userOf(self);
    const bool last = cpu.com->refCount(self) == 1u;
    if (!cpu.com->release(cpu, self)) {
        call.result = 0;
        return;
    }
    if (last && user != 0) d3d9::destroyDeviceState(cpu, user);
    call.result = last ? 0u : cpu.com->refCount(self);
}

// ---------------------------------------------------------------------------------------
// IDirect3D9
// ---------------------------------------------------------------------------------------
const std::vector<ComMethod>& direct3D9Methods();
const std::vector<ComMethod>& device9Methods();

void shimDirect3DCreate9(X86& cpu, ShimCall& call);

void shimGetAdapterCount(X86& cpu, ShimCall& call) {
    (void)cpu;
    call.result = 1u;  // 本模拟器只有一台虚拟显示设备
}

void shimGetAdapterModeCount(X86& cpu, ShimCall& call) {
    (void)cpu;  // 只用参数查表（模式表是宿主常量，不读 guest 内存）
    const std::uint32_t adapter = call.u32(1);
    const std::uint32_t format = call.u32(2);
    if (adapter != 0) {
        d3d9Log("GetAdapterModeCount", "Adapter=%u（本模拟器只有适配器 0）⇒ D3DERR_INVALIDCALL", adapter);
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t n = d3d9::adapterModeCount(format);
    d3d9Log("GetAdapterModeCount", "Adapter=0 D3DFORMAT=%u ⇒ %u 个模式", format, n);
    call.result = n;
}

void shimEnumAdapterModes(X86& cpu, ShimCall& call) {
    const std::uint32_t adapter = call.u32(1);
    const std::uint32_t format = call.u32(2);
    const std::uint32_t mode = call.u32(3);
    const std::uint32_t out_ea = call.u32(4);
    if (adapter != 0) {
        d3d9Log("EnumAdapterModes", "Adapter=%u ⇒ D3DERR_INVALIDCALL", adapter);
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9::AdapterMode m;
    if (!d3d9::adapterModeAt(format, mode, &m)) {
        d3d9Log("EnumAdapterModes", "Mode=%u D3DFORMAT=%u 越界 / 该格式没有模式 ⇒ D3DERR_INVALIDCALL", mode,
                format);
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    if (!d3d9::writeDisplayMode(cpu, out_ea, m, &why)) {
        d3d9Trap(cpu, "IDirect3D9::EnumAdapterModes(+0x1C)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9Log("EnumAdapterModes", "Mode=%u ⇒ %dx%d（刷新 %u，D3DFORMAT=%u）", mode, m.width, m.height,
            m.refresh, m.format);
    call.result = d3d9::kOk;
}

void shimGetAdapterDisplayMode(X86& cpu, ShimCall& call) {
    const std::uint32_t adapter = call.u32(1);
    const std::uint32_t out_ea = call.u32(2);
    if (adapter != 0) {
        d3d9Log("GetAdapterDisplayMode", "Adapter=%u ⇒ D3DERR_INVALIDCALL", adapter);
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9::AdapterMode m;
    std::string why;
    if (!d3d9::adapterDisplayMode(cpu, &m, &why)) {
        d3d9Trap(cpu, "IDirect3D9::GetAdapterDisplayMode(+0x20)", "%s", why.c_str());
        call.result = d3d9::kFail;
        return;
    }
    if (!d3d9::writeDisplayMode(cpu, out_ea, m, &why)) {
        d3d9Trap(cpu, "IDirect3D9::GetAdapterDisplayMode(+0x20)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9Log("GetAdapterDisplayMode", "Adapter=0 ⇒ %dx%d（刷新 %u，D3DFORMAT=%u）→ guest 0x%08X", m.width,
            m.height, m.refresh, m.format, out_ea);
    call.result = d3d9::kOk;
}

void shimCheckDeviceFormat(X86& cpu, ShimCall& call) {
    (void)cpu;  // 本线只用参数做记录（不真的枚举格式）
    d3d9::CheckFormatQuery q;
    q.adapter = call.u32(1);
    q.device_type = call.u32(2);
    q.adapter_format = call.u32(3);
    q.usage = call.u32(4);
    q.resource_type = call.u32(5);
    q.check_format = call.u32(6);
    // ★ 本线不真的枚举格式 ⇒ 一律 S_OK，但**必须**留下"问了什么"（诊断环形 + 一行日志）。
    d3d9::recordCheckFormatQuery(q);
    d3d9Log("CheckDeviceFormat",
            "Adapter=%u DeviceType=%u AdapterFormat=%u Usage=0x%08X RType=%u CheckFormat=%u ⇒ S_OK"
            "（本线不枚举格式，只记录）",
            q.adapter, q.device_type, q.adapter_format, q.usage, q.resource_type, q.check_format);
    call.result = d3d9::kOk;
}

void shimCreateDevice(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t adapter = call.u32(1);
    const std::uint32_t device_type = call.u32(2);
    const std::uint32_t hwnd = call.u32(3);
    const std::uint32_t behavior = call.u32(4);
    const std::uint32_t pp_ea = call.u32(5);
    const std::uint32_t out_ea = call.u32(6);
    if (!cpu.com || !cpu.com->find(self)) {
        d3d9Trap(cpu, "IDirect3D9::CreateDevice(+0x40)", "this=0x%08X 不是本模块建的 IDirect3D9 对象", self);
        call.result = d3d9::kInvalidCall;
        return;
    }
    if (out_ea == 0 || !cpu.mem->committed(out_ea, 4)) {
        d3d9Trap(cpu, "IDirect3D9::CreateDevice(+0x40)", "ppReturnedDeviceInterface=0x%08X 为 NULL / 未提交",
                 out_ea);
        call.result = d3d9::kInvalidCall;
        return;
    }
    cpu.mem->wr32(out_ea, 0);  // 真机在失败时也保证出参为 NULL
    if (adapter != 0) {
        d3d9Log("CreateDevice", "Adapter=%u（本模拟器只有适配器 0）⇒ D3DERR_INVALIDCALL", adapter);
        call.result = d3d9::kInvalidCall;
        return;
    }
    if (device_type != d3d9::kDevTypeHal && device_type != d3d9::kDevTypeRef) {
        d3d9Log("CreateDevice", "DeviceType=%u（只认 HAL=1 / REF=2）⇒ D3DERR_INVALIDDEVICE", device_type);
        call.result = d3d9::kInvalidDevice;
        return;
    }
    d3d9::PresentParams pp;
    std::string why;
    if (!d3d9::readPresentParams(cpu, pp_ea, &pp, &why)) {
        d3d9Trap(cpu, "IDirect3D9::CreateDevice(+0x40)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    if (pp.multi_sample_type != 0) {
        // ★ 本线不实现 MSAA：真机对"设备不支持的多重采样类型"正是这个失败码（⛔ 不假装成功）。
        d3d9Log("CreateDevice", "MultiSampleType=%u：本线不实现多重采样 ⇒ D3DERR_NOTAVAILABLE",
                pp.multi_sample_type);
        call.result = d3d9::kNotAvailable;
        return;
    }
    if (pp.swap_effect != 1u && pp.swap_effect != 3u) {
        d3d9Log("CreateDevice", "SwapEffect=%u（只认 DISCARD=1 / COPY=3）⇒ D3DERR_INVALIDCALL", pp.swap_effect);
        call.result = d3d9::kInvalidCall;
        return;
    }
    if (pp.back_buffer_count > 1u) {
        d3d9Log("CreateDevice", "BackBufferCount=%u（本线只建 1 个后缓冲）⇒ D3DERR_INVALIDCALL",
                pp.back_buffer_count);
        call.result = d3d9::kInvalidCall;
        return;
    }
    d3d9::CreateRequest req;
    req.adapter = adapter;
    req.device_type = device_type;
    req.hfocus_window = hwnd;
    req.behavior_flags = behavior;
    req.present = pp;
    const std::uint32_t state = d3d9::createDeviceState(cpu, req, &why);
    if (state == 0) {
        d3d9Trap(cpu, "IDirect3D9::CreateDevice(+0x40)", "%s", why.c_str());
        call.result = d3d9::kFail;
        return;
    }
    const std::uint32_t obj = cpu.com->createObject(cpu, "IDirect3DDevice9", device9Methods(), state);
    if (obj == 0) {
        d3d9::destroyDeviceState(cpu, state);
        d3d9Trap(cpu, "IDirect3D9::CreateDevice(+0x40)",
                 "在 guest 内存里建 IDirect3DDevice9 对象失败（见上一行 com 日志）");
        call.result = d3d9::kFail;
        return;
    }
    cpu.mem->wr32(out_ea, obj);
    d3d9Log("CreateDevice",
            "Adapter=0 DeviceType=%u hFocusWindow=0x%08X BehaviorFlags=0x%X ⇒ S_OK，IDirect3DDevice9 对象 "
            "0x%08X（状态块 0x%08X）",
            device_type, hwnd, behavior, obj, state);
    call.result = d3d9::kOk;
}

// ---------------------------------------------------------------------------------------
// IDirect3DDevice9
// ---------------------------------------------------------------------------------------
// 从 `this` 取设备状态；不是我们的对象 ⇒ loud trap（返回 false，调用方直接 return）
bool requireDevice(X86& cpu, ShimCall& call, const char* method, d3d9::DeviceInfo* out) {
    const std::uint32_t self = call.u32(0);
    std::string why;
    if (!d3d9::deviceInfo(cpu, self, out, &why)) {
        d3d9Trap(cpu, method, "%s", why.c_str());
        return false;
    }
    return true;
}

void shimGetDeviceCaps(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t caps_ea = call.u32(1);
    std::string why;
    const std::uint32_t hr = d3d9::fillCaps(cpu, caps_ea, &why);
    if (hr != d3d9::kOk) {
        d3d9Trap(cpu, "IDirect3DDevice9::GetDeviceCaps(+0x1C)", "%s（this=0x%08X）", why.c_str(), self);
    }
    call.result = hr;
}

void shimSetDialogBoxMode(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetDialogBoxMode(+0x50)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t enable = call.u32(1);
    // headless：没有真窗口，D3D9 的对话框模式没有可观察效果 ⇒ 接受并记录（不是"假装做了"）
    d3d9Log("SetDialogBoxMode", "bEnableDialogs=%u（headless：没有真窗口 ⇒ 只记录）", enable);
    call.result = d3d9::kOk;
}

void shimClear(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::Clear(+0xAC)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t count = call.u32(1);
    const std::uint32_t rects = call.u32(2);
    const std::uint32_t flags = call.u32(3);
    const std::uint32_t color = call.u32(4);
    const std::uint32_t z_bits = call.u32(5);
    const std::uint32_t stencil = call.u32(6);
    float z = 1.0f;
    std::memcpy(&z, &z_bits, sizeof(z));
    std::string why;
    const std::uint32_t hr = d3d9::clear(cpu, dev, count, rects, flags, color, z, stencil, &why);
    if (hr != d3d9::kOk) {
        d3d9Log("Clear", "失败：%s ⇒ 0x%08X", why.c_str(), hr);
    }
    call.result = hr;
}

void shimPresent(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::Present(+0x44)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    // pSourceRect / pDestRect / hDestWindowOverride / pDirtyRegion：本线不建模"部分呈现"
    const std::uint32_t src_rect = call.u32(1);
    const std::uint32_t dst_rect = call.u32(2);
    const std::uint32_t window_override = call.u32(3);
    const std::uint32_t dirty = call.u32(4);
    if (src_rect != 0 || dst_rect != 0 || dirty != 0) {
        // 真机会据此只拷一块 / 只刷脏区；本线**整帧呈现** ⇒ 必须出声（否则"看起来一样"掩盖差异）
        d3d9Log("Present", "pSourceRect=0x%08X pDestRect=0x%08X pDirtyRegion=0x%08X 被忽略（本线整帧呈现）",
                src_rect, dst_rect, dirty);
    }
    if (window_override != 0 && window_override != dev.hwnd) {
        d3d9Log("Present", "hDestWindowOverride=0x%08X 被忽略（headless 只有一台虚拟显示）", window_override);
    }
    std::string why;
    const std::uint32_t hr = d3d9::present(cpu, dev, &why);
    if (hr != d3d9::kOk) d3d9Log("Present", "失败：%s ⇒ 0x%08X", why.c_str(), hr);
    call.result = hr;
}

// ---------------------------------------------------------------------------------------
// IDirect3DDevice9 · fixed-function 流水线（M2 第一帧；语义在 runtime/d3d9_pipeline.cpp）
// ★ 槽号 / 偏移的取证：见文件头的表；`+0x144 DrawPrimitive` / `+0x164 SetFVF` /
//   `+0x190 SetStreamSource` 的站点 EA 也在那里（⚠ 实测纠正了"SetFVF=+0x124"的旧记法）。
// ---------------------------------------------------------------------------------------
const std::vector<ComMethod>& vertexBufferMethods();
const std::vector<ComMethod>& indexBufferMethods();

void shimBeginScene(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::BeginScene(+0xA4)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::beginScene(cpu, dev.state, &why);
    if (hr != d3d9::kOk) d3d9Log("BeginScene", "失败：%s ⇒ 0x%08X（this=0x%08X）", why.c_str(), hr, self);
    call.result = hr;
}

void shimEndScene(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::EndScene(+0xA8)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::endScene(cpu, dev.state, &why);
    if (hr != d3d9::kOk) d3d9Log("EndScene", "失败：%s ⇒ 0x%08X（this=0x%08X）", why.c_str(), hr, self);
    call.result = hr;
}

const char* d3d9_transformStateName(std::uint32_t state) {
    switch (state) {
        case 2u: return "VIEW";
        case 3u: return "PROJECTION";
        case 16u: return "TEXTURE0";
        case 17u: return "TEXTURE1";
        case 18u: return "TEXTURE2";
        case 19u: return "TEXTURE3";
        case 256u: return "WORLD";
        case 257u: return "WORLD1";
        case 258u: return "WORLD2";
        case 259u: return "WORLD3";
        default: return "?";
    }
}

void shimSetTransform(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetTransform(+0xB0)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t state = call.u32(1);
    const std::uint32_t matrix = call.u32(2);
    std::string why;
    const std::uint32_t hr = d3d9::setTransform(cpu, dev.state, state, matrix, &why);
    if (hr != d3d9::kOk) d3d9Log("SetTransform", "State=%u 失败：%s", state, why.c_str());
    else d3d9Log("SetTransform", "State=%u（%s）⇒ 64 B 已存", state, d3d9_transformStateName(state));
    call.result = hr;
}

void shimGetTransform(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::GetTransform(+0xB4)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::getTransform(cpu, dev.state, call.u32(1), call.u32(2), &why);
    if (hr != d3d9::kOk) d3d9Log("GetTransform", "失败：%s", why.c_str());
    call.result = hr;
}

void shimMultiplyTransform(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::MultiplyTransform(+0xB8)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::multiplyTransform(cpu, dev.state, call.u32(1), call.u32(2), &why);
    if (hr != d3d9::kOk) d3d9Log("MultiplyTransform", "失败：%s", why.c_str());
    call.result = hr;
}

void shimSetRenderState(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetRenderState(+0xE4)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t state = call.u32(1);
    const std::uint32_t value = call.u32(2);
    std::string why;
    const std::uint32_t hr = d3d9::setRenderState(cpu, dev.state, state, value, &why);
    if (hr != d3d9::kOk) {
        d3d9Log("SetRenderState", "State=%u Value=0x%X 失败：%s", state, value, why.c_str());
    } else {
        d3d9Log("SetRenderState", "State=%u（0x%X）⇒ 0x%X", state, state, value);
    }
    call.result = hr;
}

void shimGetRenderState(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::GetRenderState(+0xE8)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    call.result = d3d9::getRenderState(cpu, dev.state, call.u32(1), call.u32(2), &why);
}

void shimSetSamplerState(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetSamplerState(+0x114)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t stage = call.u32(1);
    const std::uint32_t type = call.u32(2);
    const std::uint32_t value = call.u32(3);
    std::string why;
    const std::uint32_t hr = d3d9::setSamplerState(cpu, dev.state, stage, type, value, &why);
    if (hr != d3d9::kOk) d3d9Log("SetSamplerState", "Stage=%u Type=%u 失败：%s", stage, type, why.c_str());
    else d3d9Log("SetSamplerState", "Stage=%u Type=%u ⇒ 0x%X", stage, type, value);
    call.result = hr;
}

void shimGetSamplerState(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::GetSamplerState(+0x118)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    call.result = d3d9::getSamplerState(cpu, dev.state, call.u32(1), call.u32(2), call.u32(3), &why);
}

void shimSetTextureStageState(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetTextureStageState(+0x10C)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t stage = call.u32(1);
    const std::uint32_t type = call.u32(2);
    const std::uint32_t value = call.u32(3);
    std::string why;
    const std::uint32_t hr = d3d9::setTextureStageState(cpu, dev.state, stage, type, value, &why);
    if (hr != d3d9::kOk) {
        d3d9Log("SetTextureStageState", "Stage=%u Type=%u 失败：%s", stage, type, why.c_str());
    } else {
        d3d9Log("SetTextureStageState", "Stage=%u Type=%u ⇒ 0x%X", stage, type, value);
    }
    call.result = hr;
}

void shimGetTextureStageState(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::GetTextureStageState(+0x110)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    call.result = d3d9::getTextureStageState(cpu, dev.state, call.u32(1), call.u32(2), call.u32(3), &why);
}

void shimSetTexture(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetTexture(+0x104)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::setTexture(cpu, dev.state, call.u32(1), call.u32(2), &why);
    if (hr != d3d9::kOk) d3d9Log("SetTexture", "失败：%s", why.c_str());
    call.result = hr;
}

void shimGetTexture(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::GetTexture(+0x108)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    call.result = d3d9::getTexture(cpu, dev.state, call.u32(1), call.u32(2), &why);
}

void shimSetFVF(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetFVF(+0x164)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t fvf = call.u32(1);
    std::string why;
    const std::uint32_t hr = d3d9::setFVF(cpu, dev.state, fvf, &why);
    if (hr != d3d9::kOk) d3d9Log("SetFVF", "FVF=0x%08X 失败：%s", fvf, why.c_str());
    else d3d9Log("SetFVF", "FVF=0x%08X", fvf);
    call.result = hr;
}

void shimGetFVF(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::GetFVF(+0x168)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    call.result = d3d9::getFVF(cpu, dev.state, call.u32(1), &why);
}

void shimSetVertexDeclaration(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetVertexDeclaration(+0x15C)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::setVertexDeclaration(cpu, dev.state, call.u32(1), &why);
    if (hr != d3d9::kOk) d3d9Log("SetVertexDeclaration", "失败：%s", why.c_str());
    call.result = hr;
}

void shimGetVertexDeclaration(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::GetVertexDeclaration(+0x160)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    call.result = d3d9::getVertexDeclaration(cpu, dev.state, call.u32(1), &why);
}

void shimSetViewport(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetViewport(+0xBC)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::setViewport(cpu, dev.state, call.u32(1), &why);
    if (hr != d3d9::kOk) d3d9Log("SetViewport", "失败：%s", why.c_str());
    call.result = hr;
}

void shimGetViewport(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::GetViewport(+0xC0)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    call.result = d3d9::getViewport(cpu, dev.state, call.u32(1), &why);
}

void shimSetScissorRect(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetScissorRect(+0x12C)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::setScissorRect(cpu, dev.state, call.u32(1), &why);
    if (hr != d3d9::kOk) d3d9Log("SetScissorRect", "失败：%s", why.c_str());
    call.result = hr;
}

void shimGetScissorRect(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::GetScissorRect(+0x130)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    call.result = d3d9::getScissorRect(cpu, dev.state, call.u32(1), &why);
}

void shimSetMaterial(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetMaterial(+0xC4)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::setMaterial(cpu, dev.state, call.u32(1), &why);
    if (hr != d3d9::kOk) d3d9Log("SetMaterial", "失败：%s", why.c_str());
    call.result = hr;
}

void shimSetLight(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetLight(+0xCC)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::setLight(cpu, dev.state, call.u32(1), call.u32(2), &why);
    if (hr != d3d9::kOk) d3d9Log("SetLight", "Index=%u 失败：%s", call.u32(1), why.c_str());
    call.result = hr;
}

void shimLightEnable(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::LightEnable(+0xD4)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::lightEnable(cpu, dev.state, call.u32(1), call.u32(2), &why);
    if (hr != d3d9::kOk) d3d9Log("LightEnable", "Index=%u 失败：%s", call.u32(1), why.c_str());
    call.result = hr;
}

void shimValidateDevice(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::ValidateDevice(+0x11C)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    call.result = d3d9::validateDevice(cpu, dev.state, call.u32(1), &why);
}

void shimSetStreamSource(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetStreamSource(+0x190)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr =
        d3d9::setStreamSource(cpu, dev.state, call.u32(1), call.u32(2), call.u32(3), call.u32(4), &why);
    if (hr != d3d9::kOk) d3d9Log("SetStreamSource", "失败：%s", why.c_str());
    else {
        d3d9Log("SetStreamSource", "Stream=%u VB=0x%08X Offset=%u Stride=%u", call.u32(1), call.u32(2), call.u32(3),
                call.u32(4));
    }
    call.result = hr;
}

void shimSetIndices(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::SetIndices(+0x1A0)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::setIndices(cpu, dev.state, call.u32(1), &why);
    if (hr != d3d9::kOk) d3d9Log("SetIndices", "失败：%s", why.c_str());
    call.result = hr;
}

void shimDrawPrimitive(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::DrawPrimitive(+0x144)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::drawPrimitive(cpu, dev.state, call.u32(1), call.u32(2), call.u32(3), &why);
    if (hr != d3d9::kOk) d3d9Log("DrawPrimitive", "失败：%s", why.c_str());
    call.result = hr;
}

void shimDrawIndexedPrimitive(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::DrawIndexedPrimitive(+0x148)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr = d3d9::drawIndexedPrimitive(cpu, dev.state, call.u32(1), call.u32(2), call.u32(3),
                                                        call.u32(4), call.u32(5), call.u32(6), &why);
    if (hr != d3d9::kOk) d3d9Log("DrawIndexedPrimitive", "失败：%s", why.c_str());
    call.result = hr;
}

void shimDrawPrimitiveUP(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::DrawPrimitiveUP(+0x14C)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr =
        d3d9::drawPrimitiveUP(cpu, dev.state, call.u32(1), call.u32(2), call.u32(3), call.u32(4), &why);
    if (hr != d3d9::kOk) d3d9Log("DrawPrimitiveUP", "失败：%s", why.c_str());
    call.result = hr;
}

void shimDrawIndexedPrimitiveUP(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::DrawIndexedPrimitiveUP(+0x150)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t hr =
        d3d9::drawIndexedPrimitiveUP(cpu, dev.state, call.u32(1), call.u32(2), call.u32(3), call.u32(4),
                                     call.u32(5), call.u32(6), call.u32(7), call.u32(8), &why);
    if (hr != d3d9::kOk) d3d9Log("DrawIndexedPrimitiveUP", "失败：%s", why.c_str());
    call.result = hr;
}

// ---- CreateVertexBuffer / CreateIndexBuffer（对象 + 方法表）----
void shimCreateVertexBuffer(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::CreateVertexBuffer(+0x68)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t length = call.u32(1);
    const std::uint32_t usage = call.u32(2);
    const std::uint32_t fvf = call.u32(3);
    const std::uint32_t pool = call.u32(4);
    const std::uint32_t out_ea = call.u32(5);
    const std::uint32_t shared = call.u32(6);
    if (out_ea == 0 || !cpu.mem->committed(out_ea, 4)) {
        d3d9Trap(cpu, "IDirect3DDevice9::CreateVertexBuffer(+0x68)", "ppVertexBuffer=0x%08X 为 NULL / 未提交", out_ea);
        call.result = d3d9::kInvalidCall;
        return;
    }
    cpu.mem->wr32(out_ea, 0);
    if (shared != 0) {
        d3d9Log("CreateVertexBuffer", "pSharedHandle=0x%08X：本线不支持共享句柄 ⇒ D3DERR_INVALIDCALL", shared);
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t state = d3d9::createVertexBufferState(cpu, call.u32(0), length, usage, fvf, pool, &why);
    if (state == 0) {
        d3d9Trap(cpu, "IDirect3DDevice9::CreateVertexBuffer(+0x68)", "%s", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t obj = cpu.com->createObject(cpu, "IDirect3DVertexBuffer9", vertexBufferMethods(), state);
    if (obj == 0) {
        d3d9::destroyVertexBufferState(cpu, state);
        d3d9Trap(cpu, "IDirect3DDevice9::CreateVertexBuffer(+0x68)", "建 COM 对象失败（见上一行 com 日志）");
        call.result = d3d9::kFail;
        return;
    }
    cpu.mem->wr32(out_ea, obj);
    d3d9Log("CreateVertexBuffer",
            "Length=%u Usage=0x%X FVF=0x%X Pool=%u ⇒ S_OK，IDirect3DVertexBuffer9 0x%08X（状态块 0x%08X）", length,
            usage, fvf, pool, obj, state);
    call.result = d3d9::kOk;
}

void shimCreateIndexBuffer(X86& cpu, ShimCall& call) {
    d3d9::DeviceInfo dev;
    if (!requireDevice(cpu, call, "IDirect3DDevice9::CreateIndexBuffer(+0x6C)", &dev)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t length = call.u32(1);
    const std::uint32_t usage = call.u32(2);
    const std::uint32_t format = call.u32(3);
    const std::uint32_t pool = call.u32(4);
    const std::uint32_t out_ea = call.u32(5);
    const std::uint32_t shared = call.u32(6);
    if (out_ea == 0 || !cpu.mem->committed(out_ea, 4)) {
        d3d9Trap(cpu, "IDirect3DDevice9::CreateIndexBuffer(+0x6C)", "ppIndexBuffer=0x%08X 为 NULL / 未提交", out_ea);
        call.result = d3d9::kInvalidCall;
        return;
    }
    cpu.mem->wr32(out_ea, 0);
    if (shared != 0) {
        d3d9Log("CreateIndexBuffer", "pSharedHandle=0x%08X：本线不支持共享句柄 ⇒ D3DERR_INVALIDCALL", shared);
        call.result = d3d9::kInvalidCall;
        return;
    }
    std::string why;
    const std::uint32_t state = d3d9::createIndexBufferState(cpu, call.u32(0), length, usage, format, pool, &why);
    if (state == 0) {
        d3d9Log("CreateIndexBuffer", "%s ⇒ D3DERR_INVALIDCALL", why.c_str());
        call.result = d3d9::kInvalidCall;
        return;
    }
    const std::uint32_t obj = cpu.com->createObject(cpu, "IDirect3DIndexBuffer9", indexBufferMethods(), state);
    if (obj == 0) {
        d3d9::destroyIndexBufferState(cpu, state);
        d3d9Log("CreateIndexBuffer", "建 COM 对象失败（见上一行 com 日志）⇒ E_FAIL");
        call.result = d3d9::kFail;
        return;
    }
    cpu.mem->wr32(out_ea, obj);
    d3d9Log("CreateIndexBuffer", "Length=%u Usage=0x%X Format=%u Pool=%u ⇒ S_OK，IDirect3DIndexBuffer9 0x%08X", length,
            usage, format, pool, obj);
    call.result = d3d9::kOk;
}

// ---- 顶点 / 索引缓冲对象的方法（槽序 = IUnknown 3 + IDirect3DResource9 8 + Lock/Unlock/GetDesc）----
void shimVertexBufferGetType(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call;
    call.result = 6u;  // D3DRTYPE_VERTEXBUFFER
}

void shimIndexBufferGetType(X86& cpu, ShimCall& call) {
    (void)cpu;
    (void)call;
    call.result = 7u;  // D3DRTYPE_INDEXBUFFER
}

void shimVertexBufferGetDevice(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t user = cpu.com ? cpu.com->userOf(self) : 0;
    const std::uint32_t device = d3d9::vertexBufferDevice(cpu, user);
    const std::uint32_t out_ea = call.u32(1);
    if (device == 0 || out_ea == 0 || !cpu.mem->committed(out_ea, 4)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    cpu.mem->wr32(out_ea, device);
    call.result = d3d9::kOk;
}

void shimIndexBufferGetDevice(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t user = cpu.com ? cpu.com->userOf(self) : 0;
    const std::uint32_t device = d3d9::indexBufferDevice(cpu, user);
    const std::uint32_t out_ea = call.u32(1);
    if (device == 0 || out_ea == 0 || !cpu.mem->committed(out_ea, 4)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    cpu.mem->wr32(out_ea, device);
    call.result = d3d9::kOk;
}

void shimVertexBufferLock(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t user = cpu.com ? cpu.com->userOf(self) : 0;
    std::string why;
    const std::uint32_t hr =
        d3d9::vertexBufferLock(cpu, user, call.u32(1), call.u32(2), call.u32(3), call.u32(4), &why);
    if (hr != d3d9::kOk) d3d9Log("VertexBuffer::Lock", "失败：%s", why.c_str());
    call.result = hr;
}

void shimVertexBufferUnlock(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t user = cpu.com ? cpu.com->userOf(self) : 0;
    std::string why;
    const std::uint32_t hr = d3d9::vertexBufferUnlock(cpu, user, &why);
    if (hr != d3d9::kOk) d3d9Log("VertexBuffer::Unlock", "失败：%s", why.c_str());
    call.result = hr;
}

void shimIndexBufferLock(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t user = cpu.com ? cpu.com->userOf(self) : 0;
    std::string why;
    const std::uint32_t hr =
        d3d9::indexBufferLock(cpu, user, call.u32(1), call.u32(2), call.u32(3), call.u32(4), &why);
    if (hr != d3d9::kOk) d3d9Log("IndexBuffer::Lock", "失败：%s", why.c_str());
    call.result = hr;
}

void shimIndexBufferUnlock(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t user = cpu.com ? cpu.com->userOf(self) : 0;
    std::string why;
    const std::uint32_t hr = d3d9::indexBufferUnlock(cpu, user, &why);
    if (hr != d3d9::kOk) d3d9Log("IndexBuffer::Unlock", "失败：%s", why.c_str());
    call.result = hr;
}

// GetDesc：D3DVERTEXBUFFER_DESC = { Format, Type, Usage, Pool, Size, FVF }（24 B）
void shimVertexBufferGetDesc(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t user = cpu.com ? cpu.com->userOf(self) : 0;
    const std::uint32_t out_ea = call.u32(1);
    std::uint32_t length = 0;
    std::uint32_t fvf = 0;
    std::string why;
    if (!d3d9::vertexBufferInfoOfState(cpu, user, &length, nullptr, &fvf, &why)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    if (out_ea == 0 || !cpu.mem->committed(out_ea, 24)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    cpu.mem->fillBytes(out_ea, 0, 24);
    cpu.mem->wr32(out_ea + 0, 100u);  // D3DFMT_VERTEXDATA
    cpu.mem->wr32(out_ea + 4, 6u);    // D3DRTYPE_VERTEXBUFFER
    cpu.mem->wr32(out_ea + 16, length);
    cpu.mem->wr32(out_ea + 20, fvf);
    call.result = d3d9::kOk;
}

void shimIndexBufferGetDesc(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    const std::uint32_t user = cpu.com ? cpu.com->userOf(self) : 0;
    const std::uint32_t out_ea = call.u32(1);
    std::uint32_t length = 0;
    std::uint32_t format = 0;
    std::string why;
    if (!d3d9::indexBufferInfoOfState(cpu, user, &length, nullptr, &format, &why)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    if (out_ea == 0 || !cpu.mem->committed(out_ea, 20)) {
        call.result = d3d9::kInvalidCall;
        return;
    }
    cpu.mem->fillBytes(out_ea, 0, 20);
    cpu.mem->wr32(out_ea + 0, format);
    cpu.mem->wr32(out_ea + 4, 7u);  // D3DRTYPE_INDEXBUFFER
    cpu.mem->wr32(out_ea + 16, length);
    call.result = d3d9::kOk;
}

// Release：按状态块 magic 决定回收哪一类（设备 / 顶点缓冲 / 索引缓冲）
void shimResourceRelease(X86& cpu, ShimCall& call) {
    const std::uint32_t self = call.u32(0);
    if (!cpu.com || !cpu.com->find(self)) {
        d3d9Log("Release", "this=0x%08X 不是本 ComRegistry 的对象（重复 Release / 野指针）⇒ 返回 0", self);
        call.result = 0;
        return;
    }
    const std::uint32_t user = cpu.com->userOf(self);
    const bool last = cpu.com->refCount(self) == 1u;
    if (!cpu.com->release(cpu, self)) {
        call.result = 0;
        return;
    }
    if (last && user != 0 && cpu.mem->committed(user, 4)) {
        const std::uint32_t magic = cpu.mem->rd32(user);
        if (magic == 0x30394256u) {  // 'VB09'
            d3d9::destroyVertexBufferState(cpu, user);
        } else if (magic == 0x30394249u) {  // 'IB09'
            d3d9::destroyIndexBufferState(cpu, user);
        } else if (magic == 0x31443944u) {  // 'D9D1'
            d3d9::destroyDeviceState(cpu, user);
        }
    }
    call.result = last ? 0u : cpu.com->refCount(self);
}

// ---------------------------------------------------------------------------------------
// 导出 + 方法表
// ---------------------------------------------------------------------------------------
void shimDirect3DCreate9(X86& cpu, ShimCall& call) {
    const std::uint32_t sdk_version = call.u32(0);
    if (sdk_version != d3d9::kSdkVersion) {
        // ★ 真机语义：版本不符 ⇒ 返回 NULL（不是"我们没实现"）⇒ 如实返回 + 一行日志
        d3d9Log("Direct3DCreate9", "SDKVersion=%u（真机要求 D3D_SDK_VERSION=%u）⇒ 返回 NULL", sdk_version,
                d3d9::kSdkVersion);
        call.result = 0;
        return;
    }
    if (!cpu.com) {
        d3d9Trap(cpu, "Direct3DCreate9", "cpu.com 为空（装配问题）⇒ 拿不到 guest 可见的 COM 对象");
        call.result = 0;
        return;
    }
    const std::uint32_t obj = cpu.com->createObject(cpu, "IDirect3D9", direct3D9Methods(), 0);
    if (obj == 0) {
        d3d9Trap(cpu, "Direct3DCreate9", "在 guest 内存里建 IDirect3D9 对象失败（见上一行 com 日志）");
        call.result = 0;
        return;
    }
    d3d9Log("Direct3DCreate9", "SDKVersion=%u ⇒ IDirect3D9 对象 0x%08X", sdk_version, obj);
    call.result = obj;
}

// IDirect3D9：17 槽（IUnknown 3 + 14），最后一个方法 CreateDevice 在 +0x40（★ 0x497F44 实测）
const std::vector<ComMethod>& direct3D9Methods() {
    static const std::vector<ComMethod> methods = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &shimQueryInterface9},
        {0x04u, "AddRef", Conv::Stdcall, 0, &shimAddRef},
        {0x08u, "Release", Conv::Stdcall, 0, &shimRelease},
        {0x10u, "GetAdapterCount", Conv::Stdcall, 0, &shimGetAdapterCount},             // #4
        {0x18u, "GetAdapterModeCount", Conv::Stdcall, 2, &shimGetAdapterModeCount},     // #6 ★实测
        {0x1Cu, "EnumAdapterModes", Conv::Stdcall, 4, &shimEnumAdapterModes},           // #7 ★实测
        {0x20u, "GetAdapterDisplayMode", Conv::Stdcall, 2, &shimGetAdapterDisplayMode},  // #8 ★实测
        {0x28u, "CheckDeviceFormat", Conv::Stdcall, 6, &shimCheckDeviceFormat},         // #10 ★取证
        {0x40u, "CreateDevice", Conv::Stdcall, 6, &shimCreateDevice},                   // #16 ★实测
    };
    return methods;
}

// ---- 顶点缓冲 / 索引缓冲的 vtable（IDirect3DResource9 8 槽 + Lock/Unlock/GetDesc；SDK 顺序）----
// ★ 槽位实测：`vb->Lock` 在 **+0x2C**（0x4A361E `mov eax,[ecx+2Ch]`，4 个参数 =
//   OffsetToLock/SizeToLock/ppbData/Flags）；Unlock +0x30；GetDesc +0x34。
const std::vector<ComMethod>& vertexBufferMethods() {
    static const std::vector<ComMethod> methods = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &shimQueryInterfaceVertexBuffer9},
        {0x04u, "AddRef", Conv::Stdcall, 0, &shimAddRef},
        {0x08u, "Release", Conv::Stdcall, 0, &shimResourceRelease},
        {0x0Cu, "GetDevice", Conv::Stdcall, 1, &shimVertexBufferGetDevice},
        {0x28u, "GetType", Conv::Stdcall, 0, &shimVertexBufferGetType},
        {0x2Cu, "Lock", Conv::Stdcall, 4, &shimVertexBufferLock},
        {0x30u, "Unlock", Conv::Stdcall, 0, &shimVertexBufferUnlock},
        {0x34u, "GetDesc", Conv::Stdcall, 1, &shimVertexBufferGetDesc},
    };
    return methods;
}

const std::vector<ComMethod>& indexBufferMethods() {
    static const std::vector<ComMethod> methods = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &shimQueryInterfaceIndexBuffer9},
        {0x04u, "AddRef", Conv::Stdcall, 0, &shimAddRef},
        {0x08u, "Release", Conv::Stdcall, 0, &shimResourceRelease},
        {0x0Cu, "GetDevice", Conv::Stdcall, 1, &shimIndexBufferGetDevice},
        {0x28u, "GetType", Conv::Stdcall, 0, &shimIndexBufferGetType},
        {0x2Cu, "Lock", Conv::Stdcall, 4, &shimIndexBufferLock},
        {0x30u, "Unlock", Conv::Stdcall, 0, &shimIndexBufferUnlock},
        {0x34u, "GetDesc", Conv::Stdcall, 1, &shimIndexBufferGetDesc},
    };
    return methods;
}

// IDirect3DDevice9：119 槽（IUnknown 3 + 116），最后一个方法 CreateQuery 在 +0x1D8。
// ★ 槽号 = 偏移/4；**每一个非哨兵槽都有 `.lst` 站点或明说"未取证"**：
//   #7  +0x1C  GetDeviceCaps              ★ 0x499123
//   #17 +0x44  Present                    ★ 第一帧判据（0x498AD6：4 个参数）
//   #20 +0x50  SetDialogBoxMode           ★ 0x4990C5
//   #26 +0x68  CreateVertexBuffer         ★ 0x4A35B4（Length=0x90 Usage=8 FVF=0x244 Pool=1）
//   #27 +0x6C  CreateIndexBuffer          ← 未取证（SDK 顺序）
//   #41 +0xA4  BeginScene                 ★ 0x497F6F（0 参数）
//   #42 +0xA8  EndScene                   ★ 0x497FC8（0 参数）
//   #43 +0xAC  Clear                      ★ 0x498B6D（6 参数）
//   #44 +0xB0  SetTransform               ★ 0x4A1D79（State=3/2/0x100）
//   #45 +0xB4  GetTransform               ← 未取证（SDK 顺序；夹具用）
//   #46 +0xB8  MultiplyTransform          ← 未取证（SDK 顺序）
//   #47 +0xBC  SetViewport                ★ 0x498907（1 参数 = pViewport）
//   #48 +0xC0  GetViewport                ← 未取证（SDK 顺序；夹具用）
//   #49 +0xC4  SetMaterial                ★ 0x48E156（1 参数；前一站 memset 0x44 = sizeof(D3DMATERIAL9)）
//   #51 +0xCC  SetLight                   ★ 0x4A1E4D（2 参数；表项 0x68 = sizeof(D3DLIGHT9)）
//   #53 +0xD4  LightEnable                ★ 0x4A1E5A（Index, 1）
//   #57 +0xE4  SetRenderState             ★ 0x4A1DFD（ZENABLE=7 等）
//   #58 +0xE8  GetRenderState             ← 未取证
//   #65 +0x104 SetTexture                 ★ 0x479E12（Stage, pTexture）
//   #66 +0x108 GetTexture                 ← 未取证
//   #67 +0x10C SetTextureStageState       ★ 0x479AFA（Stage,Type,Value 各 1 组 = COLOROP/ARG）
//   #68 +0x110 GetTextureStageState       ← 未取证
//   #69 +0x114 SetSamplerState            ★ 0x479ACE（MINFILTER/MAGFILTER=LINEAR）
//   #70 +0x118 GetSamplerState            ← 未取证
//   #71 +0x11C ValidateDevice             ← 未取证
//   #75 +0x12C SetScissorRect             ← 未取证
//   #76 +0x130 GetScissorRect             ← 未取证
//   #81 +0x144 DrawPrimitive              ★ 0x4A37C5（TRIANGLESTRIP,0,2 夹在 ZENABLE 开关之间）
//   #82 +0x148 DrawIndexedPrimitive       ★ 0x4C39A9（6 参数）
//   #83 +0x14C DrawPrimitiveUP            ★ 0x48E16D（LINELIST,1,data,0x1C）
//   #84 +0x150 DrawIndexedPrimitiveUP     ← 未取证（SDK 顺序）
//   #87 +0x15C SetVertexDeclaration       ← 未取证（本线只支持 FVF；非空 ⇒ INVALIDCALL）
//   #88 +0x160 GetVertexDeclaration       ← 未取证
//   #89 +0x164 SetFVF                     ★ 0x4A3610（FVF=0x244）· 0x48E0A3（FVF=0x42）
//   #90 +0x168 GetFVF                     ← 未取证
//   #100 +0x190 SetStreamSource           ★ 0x4A35FE（Stream=0, vb, 0, Stride=0x24）
//   #104 +0x1A0 SetIndices                ← 未取证
const std::vector<ComMethod>& device9Methods() {
    static const std::vector<ComMethod> methods = {
        {0x00u, "QueryInterface", Conv::Stdcall, 2, &shimQueryInterfaceDevice9},
        {0x04u, "AddRef", Conv::Stdcall, 0, &shimAddRef},
        {0x08u, "Release", Conv::Stdcall, 0, &shimRelease},
        {0x1Cu, "GetDeviceCaps", Conv::Stdcall, 1, &shimGetDeviceCaps},           // #7
        {0x44u, "Present", Conv::Stdcall, 4, &shimPresent},                       // #17 第一帧判据
        {0x50u, "SetDialogBoxMode", Conv::Stdcall, 1, &shimSetDialogBoxMode},     // #20
        {0x68u, "CreateVertexBuffer", Conv::Stdcall, 6, &shimCreateVertexBuffer},  // #26
        {0x6Cu, "CreateIndexBuffer", Conv::Stdcall, 6, &shimCreateIndexBuffer},    // #27
        {0xA4u, "BeginScene", Conv::Stdcall, 0, &shimBeginScene},                 // #41
        {0xA8u, "EndScene", Conv::Stdcall, 0, &shimEndScene},                     // #42
        {0xACu, "Clear", Conv::Stdcall, 6, &shimClear},                           // #43
        {0xB0u, "SetTransform", Conv::Stdcall, 2, &shimSetTransform},             // #44
        {0xB4u, "GetTransform", Conv::Stdcall, 2, &shimGetTransform},             // #45
        {0xB8u, "MultiplyTransform", Conv::Stdcall, 2, &shimMultiplyTransform},   // #46
        {0xBCu, "SetViewport", Conv::Stdcall, 1, &shimSetViewport},               // #47
        {0xC0u, "GetViewport", Conv::Stdcall, 1, &shimGetViewport},               // #48
        {0xC4u, "SetMaterial", Conv::Stdcall, 1, &shimSetMaterial},               // #49
        {0xCCu, "SetLight", Conv::Stdcall, 2, &shimSetLight},                     // #51
        {0xD4u, "LightEnable", Conv::Stdcall, 2, &shimLightEnable},               // #53
        {0xE4u, "SetRenderState", Conv::Stdcall, 2, &shimSetRenderState},         // #57
        {0xE8u, "GetRenderState", Conv::Stdcall, 2, &shimGetRenderState},         // #58
        {0x104u, "SetTexture", Conv::Stdcall, 2, &shimSetTexture},                // #65
        {0x108u, "GetTexture", Conv::Stdcall, 2, &shimGetTexture},                // #66
        {0x10Cu, "SetTextureStageState", Conv::Stdcall, 3, &shimSetTextureStageState},  // #67
        {0x110u, "GetTextureStageState", Conv::Stdcall, 3, &shimGetTextureStageState},  // #68
        {0x114u, "SetSamplerState", Conv::Stdcall, 3, &shimSetSamplerState},      // #69
        {0x118u, "GetSamplerState", Conv::Stdcall, 3, &shimGetSamplerState},      // #70
        {0x11Cu, "ValidateDevice", Conv::Stdcall, 1, &shimValidateDevice},        // #71
        {0x12Cu, "SetScissorRect", Conv::Stdcall, 1, &shimSetScissorRect},        // #75
        {0x130u, "GetScissorRect", Conv::Stdcall, 1, &shimGetScissorRect},        // #76
        {0x144u, "DrawPrimitive", Conv::Stdcall, 3, &shimDrawPrimitive},          // #81
        {0x148u, "DrawIndexedPrimitive", Conv::Stdcall, 6, &shimDrawIndexedPrimitive},  // #82
        {0x14Cu, "DrawPrimitiveUP", Conv::Stdcall, 4, &shimDrawPrimitiveUP},      // #83
        {0x150u, "DrawIndexedPrimitiveUP", Conv::Stdcall, 8, &shimDrawIndexedPrimitiveUP},  // #84
        {0x15Cu, "SetVertexDeclaration", Conv::Stdcall, 1, &shimSetVertexDeclaration},  // #87
        {0x160u, "GetVertexDeclaration", Conv::Stdcall, 1, &shimGetVertexDeclaration},  // #88
        {0x164u, "SetFVF", Conv::Stdcall, 1, &shimSetFVF},                        // #89 ★0x4A3610
        {0x168u, "GetFVF", Conv::Stdcall, 1, &shimGetFVF},                        // #90
        {0x190u, "SetStreamSource", Conv::Stdcall, 4, &shimSetStreamSource},      // #100 ★0x4A35FE
        {0x1A0u, "SetIndices", Conv::Stdcall, 1, &shimSetIndices},                // #104
        // ★ 哨兵（fn = nullptr ⇒ comUnsupported）：把 vtable 撑到接口的真实大小，
        //   让还没填的槽也变成"打印 iface+偏移再停机器"，而不是读 vtable 之外的堆字节。
        {kLastDeviceSlotOffset, "unsupported+0x1D8", Conv::Stdcall, 0, nullptr},
    };
    return methods;
}

}  // namespace

void registerD3D9Shims(ShimRegistry& shims) {
    // 本安装**唯一**的 d3d9 导入（`.lst` 0x498D0F `call j_Direct3DCreate9`，参数 0x20）。
    // ★ 静态导入 ⇒ 走 `add`（IAT 桩页），⛔ 不是 dynamic_exports 那条动态解析路。
    shims.add("d3d9", "Direct3DCreate9", Conv::Stdcall, 1, &shimDirect3DCreate9);
}

}  // namespace lift
