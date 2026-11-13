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

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

#include "check.h"
#include "runtime/d3d9.h"
#include "runtime/d3d9_pipeline.h"
#include "runtime/log.h"
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

// ---- stderr **tee**（"默认级别能看见告警"的判据用；形态照 `tests/dsound.cpp`）----
// ★ 只捕获不回显会让中途 abort 的现场全丢；`pump()` 把新增的一段同时写进内存 log 与真 stderr。
#ifdef _WIN32
using FdT = int;
int dupFd(int fd) { return _dup(fd); }
int dup2Fd(int src, int dst) { return _dup2(src, dst); }
int filenoOf(std::FILE* f) { return _fileno(f); }
void closeFd(int fd) { _close(fd); }
int readFd(int fd, void* buf, unsigned n) { return _read(fd, buf, n); }
int writeFd(int fd, const void* buf, unsigned n) { return _write(fd, buf, n); }
#else
using FdT = int;
int dupFd(int fd) { return ::dup(fd); }
int dup2Fd(int src, int dst) { return ::dup2(src, dst); }
int filenoOf(std::FILE* f) { return ::fileno(f); }
void closeFd(int fd) { ::close(fd); }
int readFd(int fd, void* buf, unsigned n) { return static_cast<int>(::read(fd, buf, n)); }
int writeFd(int fd, const void* buf, unsigned n) { return static_cast<int>(::write(fd, buf, n)); }
#endif

struct StderrTee {
    std::FILE* tmp = nullptr;
    FdT saved = -1;
    bool active = false;
    std::size_t moved = 0;
    std::string log;

    bool begin() {
        tmp = std::tmpfile();
        if (!tmp) return false;
        std::fflush(stderr);
        saved = static_cast<FdT>(dupFd(2));
        if (saved < 0) return false;
        if (dup2Fd(filenoOf(tmp), 2) < 0) return false;
        active = true;
        return true;
    }

    void pump() {
        if (!active || !tmp) return;
        std::fflush(stderr);
        std::fseek(tmp, 0, SEEK_END);
        const long size = std::ftell(tmp);
        if (size < 0) return;
        const std::size_t total = static_cast<std::size_t>(size);
        if (total <= moved) return;
        std::fseek(tmp, static_cast<long>(moved), SEEK_SET);
        std::vector<char> chunk(total - moved);
        const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), tmp);
        if (got == 0) return;
        log.append(chunk.data(), got);
        if (saved >= 0) {
            std::size_t off = 0;
            while (off < got) {
                const int n = writeFd(saved, chunk.data() + off, static_cast<unsigned>(got - off));
                if (n <= 0) break;
                off += static_cast<std::size_t>(n);
            }
        }
        moved += got;
    }

    std::string finish() {
        if (!active) return log;
        pump();
        dup2Fd(saved, 2);
        closeFd(saved);
        saved = -1;
        active = false;
        if (tmp) {
            std::fclose(tmp);
            tmp = nullptr;
        }
        return log;
    }
};

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

std::size_t countOccurrences(const std::string& hay, const std::string& needle) {
    if (needle.empty()) return 0;
    std::size_t n = 0;
    for (std::size_t at = hay.find(needle); at != std::string::npos; at = hay.find(needle, at + needle.size()))
        ++n;
    return n;
}

// 运行摘要 → 字符串（`dumpRunSummary` 写 `std::FILE*`）
std::string captureRunSummary(lift::Machine& m) {
    std::FILE* tmp = std::tmpfile();
    if (!tmp) return {};
    lift::RunResult rr;
    rr.reason = lift::StopReason::UntilEa;  // 不打印"最近执行的指令"块
    m.dumpRunSummary(rr, tmp);
    std::fflush(tmp);
    std::fseek(tmp, 0, SEEK_SET);
    std::string out;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), tmp)) > 0) out.append(buf, n);
    std::fclose(tmp);
    return out;
}

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
constexpr std::uint32_t kOffSetIndices = 0x1A0u;
constexpr std::uint32_t kOffCreateVertexBuffer = 0x68u;
constexpr std::uint32_t kOffCreateIndexBuffer = 0x6Cu;
constexpr std::uint32_t kOffVertexBufferLock = 0x2Cu;    // ★ IDirect3DVertexBuffer9::Lock
constexpr std::uint32_t kOffVertexBufferUnlock = 0x30u;  // ★ IDirect3DVertexBuffer9::Unlock
constexpr std::uint32_t kOffDrawIndexedPrimitive = 0x148u;
constexpr std::uint32_t kOffTextureLockRect = 0x4Cu;
constexpr std::uint32_t kOffTextureUnlockRect = 0x50u;
constexpr std::uint32_t kOffSetViewport = 0xBCu;
constexpr std::uint32_t kOffGetViewport = 0xC0u;             // SetViewport(+0xBC) 的下一个槽
// ★ `SetRenderTarget(+0x94)` 的实参形态是 **D3D9** 的：`(RenderTargetIndex, pRenderTarget)`。
//   ⛔ 不是 D3D8 的 `(pRenderTarget, pNewZStencil)` —— 按那个读会把索引 0 当成 target（= 后缓冲），
//   「渲染到纹理」的绑定被静默丢弃（REQ-01M780VKKRFHJEX6VM74DRCYBM）。
//   ⇒ 本文件的夹具一律写 `{0u, <surface>}`；`{0u, 0u}` 才是"回到后缓冲"。
constexpr std::uint32_t kOffSetRenderTarget = 0x94u;         // ★ 0x4A778B
constexpr std::uint32_t kOffGetBackBuffer = 0x48u;           // ★ 0x4A51DF（回程 0x4A521B 用它取后缓冲表面）
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
    // GetViewport：只读回 X/Y/W/H（MinZ/MaxZ 也在同一块里，但本线不消费它们 ⇒ 不断言）
    std::uint32_t getViewport(std::uint32_t* xywh) {
        const std::vector<std::uint8_t> vp(24u, 0xEEu);
        const std::uint32_t ea = placeBytes(vp);
        const std::uint32_t hr = method(device.object, kOffGetViewport, {ea});
        for (int i = 0; i < 4; ++i) xywh[i] = m.mem().rd32(ea + static_cast<std::uint32_t>(i) * 4u);
        return hr;
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
    std::uint32_t drawPrimitive(std::uint32_t prim, std::uint32_t start_vertex, std::uint32_t prim_count) {
        return method(device.object, kOffDrawPrimitive, {prim, start_vertex, prim_count});
    }
    std::uint32_t drawIndexed(std::uint32_t prim, std::uint32_t base_vertex, std::uint32_t min_index,
                              std::uint32_t num_vertices, std::uint32_t start_index,
                              std::uint32_t prim_count) {
        return method(device.object, kOffDrawIndexedPrimitive,
                      {prim, base_vertex, min_index, num_vertices, start_index, prim_count});
    }
    // CreateVertexBuffer(this, Length, Usage, FVF, Pool, ppVB, pSharedHandle=0)（Usage=8 = WRITEONLY）
    std::uint32_t createVertexBuffer(std::uint32_t length, std::uint32_t fvf, std::uint32_t* out_obj) {
        const std::uint32_t out_ea = placeU32(0xDEADBEEFu);
        const std::uint32_t hr = method(device.object, kOffCreateVertexBuffer, {length, 8u, fvf, 1u, out_ea, 0u});
        if (out_obj) *out_obj = m.mem().rd32(out_ea);
        return hr;
    }
    // CreateIndexBuffer(this, Length, Usage, Format(101=INDEX16), Pool, ppIB, pSharedHandle=0)
    std::uint32_t createIndexBuffer(std::uint32_t length, std::uint32_t format, std::uint32_t* out_obj) {
        const std::uint32_t out_ea = placeU32(0xDEADBEEFu);
        const std::uint32_t hr = method(device.object, kOffCreateIndexBuffer, {length, 8u, format, 1u, out_ea, 0u});
        if (out_obj) *out_obj = m.mem().rd32(out_ea);
        return hr;
    }
    std::uint32_t lockBuffer(std::uint32_t obj, std::uint32_t* data_ea) {
        const std::uint32_t out_ea = placeU32(0u);
        const std::uint32_t hr = method(obj, kOffVertexBufferLock, {0u, 0u, out_ea, 0u});
        if (data_ea) *data_ea = m.mem().rd32(out_ea);
        return hr;
    }
    std::uint32_t unlockBuffer(std::uint32_t obj) { return method(obj, kOffVertexBufferUnlock, {}); }
    std::uint32_t setStreamSource(std::uint32_t stream, std::uint32_t vb, std::uint32_t offset,
                                  std::uint32_t stride) {
        return method(device.object, kOffSetStreamSource, {stream, vb, offset, stride});
    }
    std::uint32_t setIndices(std::uint32_t ib) { return method(device.object, kOffSetIndices, {ib}); }

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
// ①b `SetRenderState` 的 State 上界（`REQ-01M7372P560TMY51NJ3ARG4ENG` 第 1 处）
//
// 判据（红了意味着什么）：
//   ① `SetRenderState(0xFFFF, 1)` / `SetRenderState(256, 1)` / `SetRenderState(0xFFFFFFFF, 1)`
//      ⇒ `D3DERR_INVALIDCALL`（修前：直接算 `kPlRenderStates + state*4` ⇒ 返回 S_OK 并**写穿**
//      20.7 KB 的状态块，state=0xFFFF 时写到块外 ≈256 KB 的 guest 堆里）；
//   ② 那 256 项渲染状态**逐项不变**（guest 可见路径 `GetRenderState` 回读，修前越界写不在这 256 项里，
//      但"Set 失败却改了状态"这一类错会当场红）；
//   ③ 整个 guest 堆**逐字节不变**（修前那次写落在相邻堆空间里，256 项回读看不见 ⇐ 这条才抓它）；
//   ④ Set / Get 对称：`GetRenderState(256)` 早就 INVALIDCALL，Set 现在同口径。
// ---------------------------------------------------------------------------------------
void caseRenderStateBound() {
    lift_test::g_current_case = "d3d9p/render-state-bound";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;

    // 先写几项**非零**状态：否则"字节不变"退化成"全 0 对比全 0"，越界写也看不出来。
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsFillMode, 3u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsShadeMode, 2u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, 3u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsColorWriteEnable, 0xFu), 0u);

    // ① 256 项基线（全部走 guest 可见的 GetRenderState）
    std::vector<std::uint32_t> before(256u, 0xDEADBEEFu);
    for (std::uint32_t i = 0; i < 256u; ++i) before[i] = r.f.getRs(i);
    LIFT_CHECK_NE_U32(before[lift::d3d9::kRsColorWriteEnable], 0xDEADBEEFu);

    // ② 整个 guest 堆的字节基线（状态块就在这块堆里 ⇒ 覆盖"256 项"且能抓块外写）
    auto heapBytes = [&r]() {
        const std::uint32_t base = r.f.m.layout().heap_base;
        std::vector<std::uint8_t> out(kFixtureHeapBytes, 0u);
        for (std::uint32_t i = 0; i < kFixtureHeapBytes; ++i)
            out[i] = r.f.m.mem().rd8(base + i);
        return out;
    };
    const std::vector<std::uint8_t> heap_before = heapBytes();

    // ③ 越界 Set：三条都必须 INVALIDCALL（边界 256 / 典型 0xFFFF / 回绕 0xFFFFFFFF）
    LIFT_CHECK_EQ_U32(r.f.setRs(256u, 1u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.setRs(0xFFFFu, 1u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.setRs(0xFFFFFFFFu, 1u), lift::d3d9::kInvalidCall);

    // ④ 一项都没变（先看 256 项，再看整块堆）
    for (std::uint32_t i = 0; i < 256u; ++i) LIFT_CHECK_EQ_U32(r.f.getRs(i), before[i]);
    const std::vector<std::uint8_t> heap_after = heapBytes();
    LIFT_CHECK(heap_before.size() == heap_after.size());
    LIFT_CHECK(heap_before == heap_after);

    // ⑤ Set/Get 对称：256 时 Get 的既有口径就是 INVALIDCALL
    const std::uint32_t out = r.f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffGetRenderState, {256u, out}), lift::d3d9::kInvalidCall);

    // ⑥ 非法调用之后，合法状态照旧可写可读（⛔ 不是把整条路关掉）
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 1u), 0u);
    LIFT_CHECK_EQ_U32(r.f.getRs(lift::d3d9::kRsZEnable), 1u);
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
// ⑤b 派生 mip 的**选级**（`REQ-01M73G2724479XAVS176WBZ65Q`；缩小采样必须有 mip）
//
// 判据（红了意味着什么）：
//   ① 同一张 4×4 纹理在 **1:1** 下（且 MIPFILTER 已开）四角/网格像素**必须等于 level 0 的 texel**
//      —— 这是"不改变已经正确的情形"的判据；LOD 公式符号错 / 取整偏一档，这条当场红；
//   ② **2:1** 下必须等于 level 1 的 box filter 值；**4:1** 下必须等于 level 2（整张平均）；
//   ③ `MIPFILTER = NONE` ⇒ 关掉 mipmap，4:1 也必须只采 level 0（D3D9 语义）；
//   ④ Unlock 改了像素 ⇒ 下次再画必须用**新**链（旧链不许留在缓存里）；
//   ⑤ 纹理 Release 之后链缓存里那一条必须没了（`mipCacheStats().chains` 归零）。
// ★ 期望值全部是**手算**的：4×4 的 R 通道 = 行优先 0,2,10,14 / 4,6,18,22 / 100,108,0,0 / 120,128,0,0
//   ⇒ level1（2×2）= [3, 16; 114, 0]（每块 4 个数取 (sum+2)/4），level2 = (3+16+114+0)/4 = 33。
// ---------------------------------------------------------------------------------------
void caseMipLevels() {
    lift_test::g_current_case = "d3d9p/mip-levels";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;

    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 4u, 4u, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;

    const std::uint32_t texels[16] = {0u, 2u, 10u, 14u, 4u, 6u, 18u, 22u, 100u, 108u, 0u, 0u, 120u, 128u, 0u, 0u};
    auto writeTexels = [&](const std::uint32_t values[16]) -> bool {
        const std::uint32_t locked = r.f.placeBytes(std::vector<std::uint8_t>(8, 0));
        if (r.f.method(tex, kOffTextureLockRect, {0u, locked, 0u, 0u}) != 0u) return false;
        const std::uint32_t pitch = r.f.m.mem().rd32(locked + 0u);
        const std::uint32_t bits = r.f.m.mem().rd32(locked + 4u);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                r.f.m.mem().wr32(bits + static_cast<std::uint32_t>(y) * pitch + static_cast<std::uint32_t>(x) * 4u,
                                 0xFF000000u | (values[y * 4 + x] << 16));
            }
        }
        return r.f.method(tex, kOffTextureUnlockRect, {0u}) == 0u;
    };
    LIFT_CHECK(writeTexels(texels));

    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, tex), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampAddressU, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampAddressV, lift::d3d9::kAddressClamp), 0u);
    // ★ MIPFILTER 缺省是 NONE（= 关掉 mipmap）⇒ 想测选级就必须显式打开。
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMipFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuseTex1), 0u);

    const std::uint32_t white = 0xFFFFFFFFu;
    auto drawQuad = [&](float size) {
        const std::vector<Fixture::TexVertex> quad = {
            {0.0f, 0.0f, 0.0f, 1.0f, white, 0.0f, 0.0f},
            {size, 0.0f, 0.0f, 1.0f, white, 1.0f, 0.0f},
            {0.0f, size, 0.0f, 1.0f, white, 0.0f, 1.0f},
            {size, 0.0f, 0.0f, 1.0f, white, 1.0f, 0.0f},
            {size, size, 0.0f, 1.0f, white, 1.0f, 1.0f},
            {0.0f, size, 0.0f, 1.0f, white, 0.0f, 1.0f},
        };
        const std::uint32_t data = r.f.placeTexVertices(quad);
        LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
        LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
        LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, data, 28u), 0u);
        LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    };

    // ---- ② 先做 2:1 / 4:1（这一步才**建**链），这样随后的 1:1 是"链已存在但仍然取 level 0"----
    drawQuad(2.0f);  // 2:1 ⇒ level 1（[3,16;114,0]）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), 0x00030000u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(1, 0), 0x00100000u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 1), 0x00720000u);  // 114 = 0x72
    LIFT_CHECK_EQ_U32(r.f.pixelAt(1, 1), 0x00000000u);

    drawQuad(1.0f);  // 4:1 ⇒ level 2（整张平均 = 33 = 0x21）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), 0x00210000u);

    // ---- ③ MIPFILTER=NONE ⇒ 只 level 0：4:1 也取 level 0 的 texel(2,2) = 0（黑）----
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMipFilter, lift::d3d9::kTexfNone), 0u);
    drawQuad(1.0f);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), 0x00000000u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMipFilter, lift::d3d9::kTexfPoint), 0u);

    // ---- ① 1:1（链已存在）⇒ 必须逐 texel 等于 level 0 ----
    drawQuad(4.0f);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            LIFT_CHECK_EQ_U32(r.f.pixelAt(x, y), (texels[y * 4 + x] << 16));
        }
    }

    // ---- ④ Unlock 换像素 ⇒ 同一 4:1 画必须用新链（全部 200 ⇒ 200，⛔ 不是旧的 33）----
    const std::uint32_t builds_before = static_cast<std::uint32_t>(lift::d3d9::mipCacheStats().builds);
    std::uint32_t all200[16];
    for (int i = 0; i < 16; ++i) all200[i] = 200u;
    LIFT_CHECK(writeTexels(all200));
    drawQuad(1.0f);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), 0x00C80000u);  // 200 = 0xC8
    LIFT_CHECK(lift::d3d9::mipCacheStats().builds > builds_before);

    // ---- ⑤ Release 之后链缓存里那条必须没了 ----
    const std::uint64_t chains_before = lift::d3d9::mipCacheStats().chains;
    LIFT_CHECK(chains_before >= 1u);
    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 0u);  // 纹理 Release 到 0
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::mipCacheStats().chains),
                      static_cast<std::int64_t>(chains_before) - 1);
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
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, surface}), 0u);
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
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, surface}), 0u);  // 重复设同一 target
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
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, c_surface}), 0u);
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
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, e1}), 0u);
    r.f.method(tex_e, kOffRelease, {});  // ← 只剩流水线那一份 owner 引用
    r.f.method(e1, kOffRelease, {});     //   旧 surface 只剩流水线那一份
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, e2}), 0u);
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
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, d_surface}), 0u);
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
// ⑩-B SetRenderTarget 把 viewport 回写成**新目标的全幅**（REQ-01M7372FBT0WBXAS2Z1NESBRFG）
//    D3D9（IDirect3DDevice9::SetRenderTarget 的 Remarks）：绑定新 render target 会把 viewport
//    设成新目标的全幅。引擎只在 Reset 后设一次全屏 viewport（`0x4988D1-0x498910`），之后三处
//    渲染到纹理（`0x4A778B` / `0x4A515E` / `0x4A521B`）都靠这条 ⇒ 不回写就按**旧**视口映射/裁剪。
// ★ 修前红点：`setRenderTarget` 只写 `kPlColorSurface` / `kPlColorArena` ⇒ ② 里 `GetViewport`
//   仍是 guest 设的 48×48（不是纹理的 32×16）。
// ★ 断言里**不碰** MinZ/MaxZ：本线只把它们原样存取、光栅从不消费 ⇒ 锁住"不动"等于把无证据的
//   实现取舍写进 test（本单只要求 X/Y/W/H）。
// ---------------------------------------------------------------------------------------
void caseSetRenderTargetViewport() {
    lift_test::g_current_case = "d3d9p/set-render-target-viewport";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;
    auto& mem = r.f.m.mem();

    constexpr std::uint32_t kTexW = 32u;
    constexpr std::uint32_t kTexH = 16u;  // 两个维度都与后缓冲（64×64）不同
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, kTexW, kTexH, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    const std::uint32_t surface_out = r.f.placeU32(0u);
    LIFT_CHECK_EQ_U32(r.f.method(tex, kOffTextureGetSurfaceLevel, {0u, surface_out}), 0u);
    const std::uint32_t surface = mem.rd32(surface_out);
    LIFT_CHECK_NE_U32(surface, 0u);
    if (surface == 0) return;

    // 前提：初始 viewport = 后缓冲全幅（`initDefaults` 造的就是 Reset 后引擎自己设的那个）
    std::uint32_t vp[4] = {0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu, 0xDEADBEEFu};
    LIFT_CHECK_EQ_U32(r.f.getViewport(vp), 0u);
    LIFT_CHECK_EQ_U32(vp[0], 0u);
    LIFT_CHECK_EQ_U32(vp[1], 0u);
    LIFT_CHECK_EQ_U32(vp[2], 64u);
    LIFT_CHECK_EQ_U32(vp[3], 64u);

    // ① guest 自己设一个子视口（与纹理、后缓冲的尺寸都不同）
    LIFT_CHECK_EQ_U32(r.f.setViewport(8u, 8u, 48u, 48u), 0u);
    LIFT_CHECK_EQ_U32(r.f.getViewport(vp), 0u);
    LIFT_CHECK_EQ_U32(vp[0], 8u);
    LIFT_CHECK_EQ_U32(vp[1], 8u);
    LIFT_CHECK_EQ_U32(vp[2], 48u);
    LIFT_CHECK_EQ_U32(vp[3], 48u);

    // ② 绑定纹理表面 ⇒ viewport 必须是 (0,0,纹理宽,纹理高)　★ 修前这里读到 (8,8,48,48) ⇒ 红
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, surface}), 0u);
    LIFT_CHECK_EQ_U32(r.f.getViewport(vp), 0u);
    LIFT_CHECK_EQ_U32(vp[0], 0u);
    LIFT_CHECK_EQ_U32(vp[1], 0u);
    LIFT_CHECK_EQ_U32(vp[2], kTexW);
    LIFT_CHECK_EQ_U32(vp[3], kTexH);

    // ③ 重复设**同一个** target（不换目标）：这条副作用不依赖"目标换没换"
    LIFT_CHECK_EQ_U32(r.f.setViewport(4u, 4u, 8u, 8u), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, surface}), 0u);
    LIFT_CHECK_EQ_U32(r.f.getViewport(vp), 0u);
    LIFT_CHECK_EQ_U32(vp[2], kTexW);
    LIFT_CHECK_EQ_U32(vp[3], kTexH);

    // ④ target == 0 ⇒ 回到后缓冲尺寸
    //    ★ "NULL 当成后缓冲"是**本线契约**（真机 SetRenderTarget(NULL, 0) 是 D3DERR_INVALIDCALL），
    //      这里断的是本线契约 + 回写副作用，⛔ 不宣称这是 D3D9 语义。
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, 0u}), 0u);
    LIFT_CHECK_EQ_U32(r.f.getViewport(vp), 0u);
    LIFT_CHECK_EQ_U32(vp[0], 0u);
    LIFT_CHECK_EQ_U32(vp[1], 0u);
    LIFT_CHECK_EQ_U32(vp[2], 64u);
    LIFT_CHECK_EQ_U32(vp[3], 64u);

    // ⑤ 引擎的真实回程（`0x4A521B`）：传的是 `GetBackBuffer` 的**后缓冲 surface 对象**（不是 NULL）
    //    ⇒ 尺寸走 `surfaceInfoOfState` 的 DeviceInfo 分支，仍然必须是后缓冲尺寸。
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, surface}), 0u);
    const std::uint32_t bb_out = r.f.placeU32(0u);
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffGetBackBuffer, {0u, 0u, 0u, bb_out}), 0u);
    const std::uint32_t bb_surface = mem.rd32(bb_out);
    LIFT_CHECK_NE_U32(bb_surface, 0u);
    if (bb_surface == 0) return;
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, bb_surface}), 0u);
    LIFT_CHECK_EQ_U32(r.f.getViewport(vp), 0u);
    LIFT_CHECK_EQ_U32(vp[0], 0u);
    LIFT_CHECK_EQ_U32(vp[1], 0u);
    LIFT_CHECK_EQ_U32(vp[2], 64u);
    LIFT_CHECK_EQ_U32(vp[3], 64u);
}

// ---------------------------------------------------------------------------------------
// ⑪ owner 的**寿命**与**陈旧代际**（REQ-01M6X4NFHCW1Y6YDJ4DYFA92PS；04C 修好后 ① 翻面）
// ---------------------------------------------------------------------------------------
// ★ 本用例有两条方向**相反**的判据，`①` 正向、`②` 负向 —— 读断言之前先读这段：
//   · **①（正向 / A 方案，`REQ-01M6ZZZ6PEVSFB93C6VJPKC04C` 已修）**：`GetSurfaceLevel` 建的 surface
//     **替父纹理持一份引用**（`shims/d3dx9_surface.h` / `com.h` 的 `owns_owner`）⇒ guest 只放掉
//     自己那份 `Release(纹理)` 之后，纹理**仍然活着**（对象 / `'TEX1'` 状态块 / arena 像素块都在）
//     ⇒ `SetRenderTarget(surface)` **应当成功**，`color_arena` 就是该纹理的 arena。
//     ⚠ 修 04C **之前**这里被拒成 `D3DERR_INVALIDCALL`（当时 surface 不持父纹理）；那条"已知偏离"
//     已随 04C 消失，本段现在是**正向**判据 —— ⛔ 不要改回"必须失败"，也不要为了让它变绿去删断言。
//   · **②（负向 / 防御纵深）**：guest **越过自己的引用计数**把 surface 那一份也放掉 ⇒ owner 死、
//     EA 被下一个同尺寸纹理复用。这时 surface 缓存的 owner 已经指向**别人**：
//     `SetRenderTarget(surface)` 必须继续拒成 `D3DERR_INVALIDCALL`（代际判据），解绑时流水线缓存的
//     owner 也必须走 `releaseIf` 拒绝 ⇒ 新占有者一个引用都不被减、对象还在。
//     ⛔ **这条不变量必须留着**：它防的是"静默把 RT 绑到别人的像素上 + 对别人
//     `markTextureContentUnknown`"。合法时序下 owner 死不了（① 就是它在保命）⇒ ② 只能用越界释放造。
//   · 可达性：本仓**未在游戏路径观测到**该顺序（需求单 repro 也这么记："游戏路径里没观测到陈旧 Release"）；
//     本用例是用夹具按判据造出来的最小重现，**不**代表"游戏里必然发生"。
// ★ 判据（每条都在破坏时红）：
//   ① surface 持父纹理 ⇒ guest 只放自己那份之后 owner 仍活、arena 仍在、绑定成功且指向该 arena；
//      再 Release surface ⇒ owner 才到 0、arena 才回收（成对释放；⛔ 不是"只 addRef 不 release"——
//      那样 arena 与堆会停在"建完之后"，① 的清理段会红）。
//   ② 越界释放之后，新纹理 T4 会**同时**拿到 owner 的对象 EA **与** `'TEX1'` 状态块 EA（`kTxBytes`
//      是常量）⇒ 旧判据 `userOf(owner) != tex_state` 在这一格**通过**（它认对象靠 "EA + guest 派生状态"）
//      ⇒ 陈旧 owner 必须被代际判据拒成 INVALIDCALL；拒绝之后 T4 一个引用都不多、流水线属性不动。
//   ③ 解绑时流水线缓存的 owner **走 `releaseIf`**：代际不符 ⇒ 拒绝，T4 的 refcount 不动、对象还在。
void caseOwnerLifetimeAndStaleGeneration() {
    // ★ 用例名不再带 `known-deviation`：那条偏离（surface 不持父纹理）已随 04C 修掉，
    //   现在是"①正向 + ②负向"两条真判据（见函数头）。
    lift_test::g_current_case = "d3d9p/owner-lifetime-and-stale-generation";
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
        // surface 记下的是"我代表谁"，**并且替它持一份引用**（A 方案 / `owns_owner`，04C）
        LIFT_CHECK_EQ_U32(com->ownerOf(s->surface), s->owner);
        LIFT_CHECK_EQ_U32(com->ownerGenerationOf(s->surface), s->owner_gen);
        // ★ 前提：建 surface 时父纹理被 addRef 了一份（guest 1 + surface 1）—— 坏掉这里后面全红。
        LIFT_CHECK_EQ_U32(com->refCount(s->owner), 2u);
        return s->dummy != 0 && s->surface != 0;
    };
    // 放掉哑纹理 + 对 owner 做 `releases` 次 Release。**合法** guest 只该放 1 次（自己那份）；
    // 多出来的每一次都是**越界**（把 surface / 设备 RT 那些引用也放掉）⇒ 只有 ② 用得到。
    // ★ ⛔ 这里**不**断言 owner 已死：① 要的正是"放掉 guest 那份之后 owner 还活着"（A 方案）。
    // ⚠ 各次 Release 的返回值不看（第一次是 rc-1 的返回值、最后一次才是 0）：判据是对象还在不在。
    auto releaseOwnerTimes = [&](const StaleOwnerSet& s, std::uint32_t releases) {
        LIFT_CHECK_EQ_U32(r.f.method(s.dummy, kOffRelease, {}), 0u);
        for (std::uint32_t i = 0; i < releases; ++i) r.f.method(s.owner, kOffRelease, {});
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

    // ---- ① **正向**（A 方案 / 04C）：guest 只放掉自己那份纹理引用 ⇒ surface 保着父纹理，
    //   纹理与它的 arena 都还在 ⇒ 绑 S 必须**成功**、`color_arena` 指向该纹理自己的像素块。
    //   ⚠ 修 04C 之前这里被拒成 INVALIDCALL（surface 不持父纹理）；现在它反过来证明"surface 真的持着"。
    StaleOwnerSet a;
    if (!prepare(&a)) return;
    releaseOwnerTimes(a, /*releases=*/1);  // 合法：只放 guest 那份（surface 那份是 surface 自己在持）
    // ★ 修前（04C 之前）这四条当场红：owner 已死、状态块 / arena 已回收、EA 被 T2 接手
    LIFT_CHECK(com->find(a.owner));
    LIFT_CHECK_EQ_U32(com->refCount(a.owner), 1u);  // 只剩 surface 那一份
    LIFT_CHECK_EQ_U32(com->generationOf(a.owner), a.owner_gen);  // 同一个对象、代际没变
    LIFT_CHECK(r.f.m.surfaces().find(a.owner_arena) != nullptr);  // 像素块没被回收 / 没被 T2 接手

    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, a.surface}),
                      lift::d3d9::kOk);
    lift::d3d9::PipelineInfo piA;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &piA, nullptr));
    LIFT_CHECK_EQ_U32(piA.color_surface, a.surface);
    LIFT_CHECK_EQ_U32(piA.color_arena, a.owner_arena);  // ← 该纹理自己的像素块（不是别人的）
    // 设备那份 RT 引用叠在 surface 那份上（`REQ-01M6X4N7C0PAF6XT7EAC172V85` 的语义引用）
    LIFT_CHECK_EQ_U32(com->refCount(a.owner), 2u);

    // 清理：解绑 ⇒ surface 的 guest 引用 ⇒ 纹理此刻才到 0（arena 随它回收）
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, 0u}), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(com->refCount(a.surface), 1u);  // 流水线那份已放，guest 那份还在
    LIFT_CHECK_EQ_U32(r.f.method(a.surface, kOffRelease, {}), 0u);
    LIFT_CHECK(!com->find(a.surface));
    LIFT_CHECK(!com->find(a.owner));  // 纹理到 0 = surface 还了它那一份（A 方案的成对释放）
    LIFT_CHECK(r.f.m.surfaces().find(a.owner_arena) == nullptr);

    // ---- ② 防御纵深（**不是**合法时序）：绑定中 owner 被越界释放 ⇒ 陈旧 owner 不许绑、也不许解绑 ----
    //   合法 guest 走不到这里：surface + 绑定期各持一份引用，owner 会在解绑时才到 0（见 ① 与 ⑩ ⑥）。
    StaleOwnerSet b;
    if (!prepare(&b)) return;
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, b.surface}), 0u);  // 这时代际匹配
    // ⚠ `refCount == 3` 锁的是 **A 方案下**的三份引用：guest 的 + surface 的（04C）+ 设备的（RT 语义引用，
    //   `REQ-01M6X4N7C0PAF6XT7EAC172V85` 有意选择的 guest 可见差异）—— 不是引擎语义。
    LIFT_CHECK_EQ_U32(com->refCount(b.owner), 3u);
    releaseOwnerTimes(b, /*releases=*/3);  // 越界：连 surface 与设备那两份也放掉
    LIFT_CHECK(!com->find(b.owner));       // owner 真死了 ⇒ EA 可被复用（② 的前提）
    const std::uint32_t tex4 = reuseOwnerSlot(b);
    if (tex4 == 0) return;
    const std::uint32_t tex4_gen = com->generationOf(tex4);
    LIFT_CHECK_NE_U32(tex4_gen, b.owner_gen);

    // ②-a 解绑 ⇒ 流水线释放它那两份引用：owner 代际不符 ⇒ **拒绝**；surface 照常释放
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, 0u}), 0u);
    LIFT_CHECK(com->find(tex4));                 // ⛔ 新占有者还在
    LIFT_CHECK_EQ_U32(com->refCount(tex4), 1u);  // ⛔ 一个引用都没被减（陈旧持有者打不到它）
    LIFT_CHECK_EQ_U32(com->generationOf(tex4), tex4_gen);
    LIFT_CHECK(com->ifaceOf(tex4) != nullptr);
    LIFT_CHECK_EQ_U32(com->refCount(b.surface), 1u);  // surface 的那份引用正常放掉（guest 的那份还在）
    LIFT_CHECK(com->find(b.surface));

    // ②-b 陈旧 owner **不许绑**：surface 缓存的 owner EA 现在归 T4（代际不符）⇒ 必须 INVALIDCALL，
    //   且 T4 一个引用都不多、像素不被标记、流水线属性不动（⛔ 不许把 RT 静默绑到 T4 的像素上）。
    //   ⚠ 这段是那条"短路 SetRenderTarget 的 owner 代际校验"变异条目的**唯一**判据。
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, b.surface}),
                      lift::d3d9::kInvalidCall);
    LIFT_CHECK(com->find(tex4));
    LIFT_CHECK_EQ_U32(com->refCount(tex4), 1u);
    LIFT_CHECK_EQ_U32(com->generationOf(tex4), tex4_gen);
    lift::d3d9::PipelineInfo piB;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &piB, nullptr));
    LIFT_CHECK_EQ_U32(piB.color_surface, 0u);  // 仍停在"后缓冲"（拒绝没有悄悄换 target）
    LIFT_CHECK_EQ_U32(piB.color_arena, 0u);

    // 清理
    LIFT_CHECK_EQ_U32(r.f.method(tex4, kOffRelease, {}), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(b.surface, kOffRelease, {}), 0u);
    LIFT_CHECK(!com->find(tex4));
    LIFT_CHECK(!com->find(b.surface));
}

// =======================================================================================
// ⑫ 绑定槽的**身份**与**绑定引用**（`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 段 2 的 ① 与 ③）
// =======================================================================================
// 两份判据分开写、各自会红：
//   · ① **身份**：流槽 / 索引槽里存 (对象 EA, 绑定时代际)。同长度新缓冲会**同时**复用对象 EA 与
//     `'VB09'`/`'IB09'` 状态块 EA（首次适配，确定性）⇒ 只验 magic 的旧口径"假匹配"、静默改用别人的
//     顶点。判据（写死一条）：旧句柄的 Draw **被拒**（`D3DERR_INVALIDCALL`）且目标像素**一个都没变**；
//     另有一行默认级别（warn）可见的告警（`pipeWarn`，`tests/check.h` 抓不到日志 ⇒ 断言落在行为上）。
//   · ③ **绑定引用**：`SetStreamSource` / `SetIndices` 之后设备替 guest 持一份（与 `SetTexture` 同口径）
//     ⇒ guest 放掉自己那份时 refCount 从 1 归 0（⛔ 不是 0）；解绑 ⇒ 设备那份还回去、`on_zero` 回收
//     状态块 ⇒ guest 堆回到建缓冲之前的基线。
// ★ 夹具前提（EA 复用）**硬断言**，不成立就红 —— ⛔ 不是删断言；若红了先按 `runtime/heap.cpp` 的
//   分块 / `min_split` 重造夹具（同 `caseOwnerLifetimeAndStaleGeneration` 的口径）。
void writeRhwTriangle(std::uint32_t vb, float ox, float oy, std::uint32_t color, Fixture& f) {
    auto& mem = f.m.mem();
    std::uint32_t base = 0;
    LIFT_CHECK_EQ_U32(f.lockBuffer(vb, &base), 0u);
    LIFT_CHECK_NE_U32(base, 0u);
    if (base == 0) return;
    const float pts[3][2] = {{ox, oy}, {ox + 10.0f, oy}, {ox, oy + 10.0f}};
    for (int i = 0; i < 3; ++i) {
        const std::uint32_t at = base + static_cast<std::uint32_t>(i) * 20u;
        for (int k = 0; k < 2; ++k) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &pts[i][k], sizeof(bits));
            mem.wr32(at + static_cast<std::uint32_t>(k) * 4u, bits);
        }
        mem.wr32(at + 8u, 0u);            // z = 0.0f
        mem.wr32(at + 12u, 0x3F800000u);  // rhw = 1.0f
        mem.wr32(at + 16u, color);        // D3DCOLOR
    }
    LIFT_CHECK_EQ_U32(f.unlockBuffer(vb), 0u);
}

void caseStaleStreamBinding() {
    lift_test::g_current_case = "d3d9p/stale-stream-binding";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;
    lift::ComRegistry* com = r.f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    constexpr std::uint32_t kLen = 60u;  // 3 个 XYZRHW+DIFFUSE 顶点（20 B 一个）
    const std::uint32_t red = 0xFFFF0000u;
    const std::uint32_t blue = 0xFF0000FFu;

    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);

    // ---- ①-A 顶点缓冲：绑 → 放自己那份 → 越界再放一次（对象死）→ 新缓冲拿回同一对 EA ----
    std::uint32_t vb1 = 0;
    std::uint32_t vb_dummy = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(kLen, kFvfXyzRhwDiffuse, &vb1), 0u);
    LIFT_CHECK_NE_U32(vb1, 0u);
    if (vb1 == 0) return;
    writeRhwTriangle(vb1, 0.0f, 0.0f, red, r.f);
    const std::uint32_t vb1_state = com->userOf(vb1);
    const std::uint32_t vb1_gen = com->generationOf(vb1);
    LIFT_CHECK_NE_U32(vb1_state, 0u);
    LIFT_CHECK_NE_U32(vb1_gen, 0u);
    // 哑缓冲（同长度）：两者都释放后空洞连成一片 ⇒ 新缓冲能把对象 / vtable / 状态块**逐个**搬回原址
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(kLen, kFvfXyzRhwDiffuse, &vb_dummy), 0u);
    LIFT_CHECK_NE_U32(vb_dummy, 0u);

    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, vb1, 0u, 20u), 0u);
    // ③（同一时序的前半段）：设备持绑定引用 ⇒ guest 放掉自己那份之后 refCount = 1
    LIFT_CHECK_EQ_U32(r.f.method(vb1, kOffRelease, {}), 1u);
    LIFT_CHECK(com->find(vb1));
    // 越界再放一次：把设备那份也抵掉 ⇒ 对象死、`'VB09'` 状态块回收。**只有越界释放**能造出陈旧句柄
    //   （合法时序里 ③ 的引用钉着它）—— 这一格就是"引用记账不是身份校验"的那条缝。
    //   ★ 判活再调（同 `caseVertexDeclaration` 的说明）：③ 被破坏时对象在上一条就归 0 了，
    //     再按 `this` 调方法会读已释放对象的 vtable 并打到未提交页 —— 那会把其余断言一起吞掉。
    if (com->find(vb1)) LIFT_CHECK_EQ_U32(r.f.method(vb1, kOffRelease, {}), 0u);
    LIFT_CHECK(!com->find(vb1));
    LIFT_CHECK_EQ_U32(r.f.method(vb_dummy, kOffRelease, {}), 0u);

    std::uint32_t vb2 = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(kLen, kFvfXyzRhwDiffuse, &vb2), 0u);
    LIFT_CHECK_EQ_U32(vb2, vb1);                     // 前提 1：对象 EA 复用
    LIFT_CHECK_EQ_U32(com->userOf(vb2), vb1_state);   // 前提 2：状态块 EA 也复用 ⇒ magic 校验必过
    LIFT_CHECK_NE_U32(com->generationOf(vb2), vb1_gen);
    writeRhwTriangle(vb2, 40.0f, 40.0f, blue, r.f);

    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    // ★ 判据：Draw **被拒**（⛔ 不是"画了别人的顶点"）
    LIFT_CHECK_EQ_U32(r.f.drawPrimitive(lift::d3d9::kPtTriangleList, 0u, 1u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(41, 41), kClearPixel);  // 蓝三角形**没有**被画（修前这里 = 蓝）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(1, 1), kClearPixel);    // 旧位置也没画出任何东西

    // ---- ①-B 索引缓冲：同一判据的第二个身份槽（`DrawIndexedPrimitive`）----
    std::uint32_t vb3 = 0;
    std::uint32_t ib1 = 0;
    std::uint32_t ib_dummy = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(kLen, kFvfXyzRhwDiffuse, &vb3), 0u);
    if (vb3 == 0) return;
    writeRhwTriangle(vb3, 0.0f, 0.0f, red, r.f);
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, vb3, 0u, 20u), 0u);
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(12u, 101u, &ib1), 0u);
    LIFT_CHECK_NE_U32(ib1, 0u);
    if (ib1 == 0) return;
    const std::uint32_t ib1_state = com->userOf(ib1);
    const std::uint32_t ib1_gen = com->generationOf(ib1);
    LIFT_CHECK_NE_U32(ib1_state, 0u);
    LIFT_CHECK_NE_U32(ib1_gen, 0u);
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(12u, 101u, &ib_dummy), 0u);
    LIFT_CHECK_NE_U32(ib_dummy, 0u);
    {
        std::uint32_t ibase = 0;
        LIFT_CHECK_EQ_U32(r.f.lockBuffer(ib1, &ibase), 0u);
        LIFT_CHECK_NE_U32(ibase, 0u);
        if (ibase != 0) {
            for (int i = 0; i < 3; ++i) r.f.m.mem().wr16(ibase + static_cast<std::uint32_t>(i) * 2u, static_cast<std::uint16_t>(i));
        }
        LIFT_CHECK_EQ_U32(r.f.unlockBuffer(ib1), 0u);
    }
    LIFT_CHECK_EQ_U32(r.f.setIndices(ib1), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(ib1, kOffRelease, {}), 1u);  // ③：设备持绑定引用
    if (com->find(ib1)) LIFT_CHECK_EQ_U32(r.f.method(ib1, kOffRelease, {}), 0u);  // 越界：对象死
    LIFT_CHECK(!com->find(ib1));
    LIFT_CHECK_EQ_U32(r.f.method(ib_dummy, kOffRelease, {}), 0u);
    std::uint32_t ib2 = 0;
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(12u, 101u, &ib2), 0u);
    LIFT_CHECK_EQ_U32(ib2, ib1);                    // 前提 1：对象 EA 复用
    LIFT_CHECK_EQ_U32(com->userOf(ib2), ib1_state);  // 前提 2：`'IB09'` 状态块 EA 也复用
    LIFT_CHECK_NE_U32(com->generationOf(ib2), ib1_gen);

    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawIndexed(lift::d3d9::kPtTriangleList, 0u, 0u, 3u, 0u, 1u),
                      lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(1, 1), kClearPixel);  // 修前：用 IB2 的索引画 VB3 的红三角
}

// =======================================================================================
// ⑬ Draw 读纹理槽的**身份校验**（`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 段 4）
// =======================================================================================
// 缺陷形态：`prepareContext` 只按槽里的 EA 解 `'TEX1'`（只验 magic）。guest 堆复用 EA ⇒
// 纹理 A 被越界释放、同尺寸的纹理 B 拿回**同一个对象 EA + 同一个 `'TEX1'` 状态块 EA** ⇒
// 旧槽在 Draw 时"看起来完全合法"，于是**静默采样 B 的像素**（A 的那笔绘制变成 B 的内容）。
// 判据（写死一条）：同一个槽（没重新 SetTexture）的第二次 Draw 被拒（`D3DERR_INVALIDCALL`）
// 且目标像素**还是清屏色**；`draws` 不增（这次绘制被整个跳过，⛔ 不是"画了一半"）。
// 告警走 `pipeWarn`（`tests/check.h` 抓不到日志 ⇒ 断言落在行为上）。
// ★ 夹具前提（EA 复用）**硬断言**，不成立就红 —— ⛔ 不是删断言；若红了先按 `runtime/heap.cpp`
//   的分块 / `min_split` 重造夹具（同 `caseStaleStreamBinding` 的口径）。
void caseStaleTextureBindingInDraw() {
    lift_test::g_current_case = "d3d9p/stale-texture-binding-in-draw";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;
    lift::ComRegistry* com = r.f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    // ---- 状态：纯纹理四边形（POINT + CLAMP），无混合 / 无 Z ----
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampAddressU, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampAddressV, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuseTex1), 0u);

    // 2×2 纹理整块填成同一个 texel ⇒ 四边形内每个像素都能读出"这是哪张纹理"
    auto fill2x2 = [&](std::uint32_t tex, std::uint32_t argb) {
        const std::uint32_t locked = r.f.placeBytes(std::vector<std::uint8_t>(8u, 0u));
        LIFT_CHECK_EQ_U32(r.f.method(tex, kOffTextureLockRect, {0u, locked, 0u, 0u}), 0u);
        const std::uint32_t pitch = r.f.m.mem().rd32(locked + 0u);
        const std::uint32_t bits = r.f.m.mem().rd32(locked + 4u);
        LIFT_CHECK_NE_U32(bits, 0u);
        if (bits == 0) return;
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                r.f.m.mem().wr32(bits + static_cast<std::uint32_t>(y) * pitch +
                                     static_cast<std::uint32_t>(x) * 4u,
                                 argb);
            }
        }
        LIFT_CHECK_EQ_U32(r.f.method(tex, kOffTextureUnlockRect, {0u}), 0u);
    };

    constexpr std::uint32_t kTexelA = 0xFFFF0000u;  // A：红
    constexpr std::uint32_t kTexelB = 0xFF0000FFu;  // B：蓝

    // ---- 夹具：A → 哑纹理（两者一起释放后空洞连成一片 ⇒ B 能把三块**逐个**搬回 A 的地址）----
    std::uint32_t tex_a = 0;
    std::uint32_t tex_dummy = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex_a), 0u);
    LIFT_CHECK_NE_U32(tex_a, 0u);
    if (tex_a == 0) return;
    lift::d3d9::TextureInfo ta;
    LIFT_CHECK(lift::d3d9::textureInfo(r.f.m.cpu(), tex_a, &ta, nullptr));
    const std::uint32_t a_state = ta.state;
    const std::uint32_t a_gen = com->generationOf(tex_a);
    LIFT_CHECK_NE_U32(a_state, 0u);
    LIFT_CHECK_NE_U32(a_gen, 0u);
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex_dummy), 0u);
    LIFT_CHECK_NE_U32(tex_dummy, 0u);
    fill2x2(tex_a, kTexelA);

    // ---- ① 正向：A 绑定 + Draw ⇒ 四边形是 A 的红色（证明这个槽真的参与采样）----
    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, tex_a), 0u);
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    const std::vector<Fixture::TexVertex> quad = {
        {0.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 0.0f},
        {8.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {0.0f, 8.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
        {8.0f, 0.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 0.0f},
        {8.0f, 8.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 1.0f, 1.0f},
        {0.0f, 8.0f, 0.0f, 1.0f, 0xFFFFFFFFu, 0.0f, 1.0f},
    };
    const std::uint32_t quad_ea = r.f.placeTexVertices(quad);
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, quad_ea, 28u), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), 0x00FF0000u);  // 后缓冲 X8R8G8B8 ⇒ alpha 丢掉
    LIFT_CHECK_EQ_U32(r.f.pixelAt(7, 7), 0x00FF0000u);
    lift::d3d9::PipelineInfo info;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.draws, 1u);

    // ---- 越界释放 A（guest 那份 + 设备替绑定持的那份）----
    //   ★ 合法 guest 只放 1 次；第二次是**越界**（把设备那份也抵掉）⇒ 对象死、`'TEX1'` 与像素块回收。
    //     只有越界释放能造出"槽里是陈旧 EA"（合法时序里绑定引用钉着它 —— 见 `d3d9p/binding-lifetime`）。
    //   ★ 判活再调（同 `caseStaleStreamBinding` 的说明）：绑定引用那条被破坏时对象在上一条就归 0 了，
    //     再按 this 调方法会读已释放对象的 vtable 并打到未提交页 —— 那会把其余断言一起吞掉。
    LIFT_CHECK_EQ_U32(r.f.method(tex_a, kOffRelease, {}), 1u);  // 设备那份还在
    if (com->find(tex_a)) LIFT_CHECK_EQ_U32(r.f.method(tex_a, kOffRelease, {}), 0u);  // 越界：对象死
    LIFT_CHECK(!com->find(tex_a));
    LIFT_CHECK_EQ_U32(r.f.method(tex_dummy, kOffRelease, {}), 0u);

    // ---- ② B 拿回同一对 EA（前提硬断言）----
    std::uint32_t tex_b = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex_b), 0u);
    LIFT_CHECK_NE_U32(tex_b, 0u);
    if (tex_b == 0) return;
    LIFT_CHECK_EQ_U32(tex_b, tex_a);                     // 前提 1：对象 EA 复用
    LIFT_CHECK_EQ_U32(com->userOf(tex_b), a_state);       // 前提 2：`'TEX1'` 状态块 EA 也复用 ⇒ magic 校验必过
    LIFT_CHECK_NE_U32(com->generationOf(tex_b), a_gen);   // 前提 3：身份换了
    fill2x2(tex_b, kTexelB);

    // ---- ③ 同一个槽（**没有**重新 SetTexture）再 Draw ⇒ 必须跳过，⛔ 不许采 B 的蓝色 ----
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, quad_ea, 28u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), kClearPixel);  // 修前：B 的蓝 0x000000FF
    LIFT_CHECK_EQ_U32(r.f.pixelAt(7, 7), kClearPixel);
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.draws, 1u);  // 这一次绘制**没有被执行**（快照 / 像素都没进光栅）

    // 清理：放掉 B 自己那份（到 0 ⇒ 像素块 / 状态块回收）
    LIFT_CHECK_EQ_U32(r.f.method(tex_b, kOffRelease, {}), 0u);
    LIFT_CHECK(!com->find(tex_b));
}

void caseBindingLifetime() {
    lift_test::g_current_case = "d3d9p/binding-lifetime";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;
    lift::ComRegistry* com = r.f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    // ---- ③-A 顶点缓冲：SetStreamSource 持一份；解绑后才到 0（堆回基线）----
    const std::uint32_t heap_before = r.f.m.heap().bytesInUse();
    std::uint32_t vb = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(60u, kFvfXyzRhwDiffuse, &vb), 0u);
    LIFT_CHECK_NE_U32(vb, 0u);
    if (vb == 0) return;
    const std::uint32_t vb_state = com->userOf(vb);
    LIFT_CHECK_NE_U32(vb_state, 0u);
    LIFT_CHECK_EQ_U32(com->refCount(vb), 1u);  // 只有 guest 那份
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, vb, 0u, 20u), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(vb), 2u);  // + 设备绑定那份
    // ★ 判据：guest 放掉自己那份 ⇒ **不是 0**（修前 = 0，对象与 `'VB09'` 状态块当场没）
    LIFT_CHECK_EQ_U32(r.f.method(vb, kOffRelease, {}), 1u);
    LIFT_CHECK(com->find(vb));
    LIFT_CHECK_EQ_U32(com->userOf(vb), vb_state);
    // ★ 这里**不再** Release 一次：此刻只剩设备那一份，再 Release 就是越界（③ 的判据正是
    //   "合法时序不需要第二次 Release"）。设备那份由下面的解绑放掉。
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, 0u, 0u, 0u), 0u);  // 解绑
    LIFT_CHECK(!com->find(vb));                                  // 设备那份还回去 ⇒ on_zero 回收
    LIFT_CHECK_EQ_U32(r.f.m.heap().bytesInUse(), heap_before);    // 堆回到建缓冲之前的基线

    // ---- ③-B 索引缓冲：同形 ----
    std::uint32_t ib = 0;
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(12u, 101u, &ib), 0u);
    LIFT_CHECK_NE_U32(ib, 0u);
    if (ib == 0) return;
    LIFT_CHECK_EQ_U32(com->refCount(ib), 1u);
    LIFT_CHECK_EQ_U32(r.f.setIndices(ib), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(ib), 2u);
    LIFT_CHECK_EQ_U32(r.f.method(ib, kOffRelease, {}), 1u);  // 修前 = 0
    LIFT_CHECK(com->find(ib));
    LIFT_CHECK_EQ_U32(r.f.setIndices(0u), 0u);  // 解绑
    LIFT_CHECK(!com->find(ib));
    LIFT_CHECK_EQ_U32(r.f.m.heap().bytesInUse(), heap_before);

    // ---- ③-D 同一 EA 的**新顶点缓冲**再 `SetStreamSource`（`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 段 3 的 ③）----
    //   ★ 槽里存 (EA, 代际)。同 EA 不同代际 = 复用 EA 的**新对象** ⇒ 必须 addRef 新的、旧绑定按
    //     (EA, 旧代际) 判还（`releaseIf` 会拒绝 ⇒ 只出声）。旧判据只比 EA ⇒ "同 EA"被当成
    //     "重复 Set 同一个" ⇒ 新缓冲**没有**绑定引用，guest 一放就归 0（对象 + `'VB09'` 当场没）。
    //   ★ 修前：`refCount(vbd2)` 期望 2 得 1、`find(vbd2)` 假、堆基线不符。
    std::uint32_t vbd1 = 0;
    std::uint32_t vbd_dummy = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(60u, kFvfXyzRhwDiffuse, &vbd1), 0u);
    if (vbd1 == 0) return;
    const std::uint32_t vbd1_state = com->userOf(vbd1);
    const std::uint32_t vbd1_gen = com->generationOf(vbd1);
    LIFT_CHECK_NE_U32(vbd1_state, 0u);
    LIFT_CHECK_NE_U32(vbd1_gen, 0u);
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(60u, kFvfXyzRhwDiffuse, &vbd_dummy), 0u);
    if (vbd_dummy == 0) return;
    writeRhwTriangle(vbd1, 0.0f, 0.0f, 0xFFFF0000u, r.f);
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, vbd1, 0u, 20u), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(vbd1), 2u);
    LIFT_CHECK_EQ_U32(r.f.method(vbd1, kOffRelease, {}), 1u);
    if (com->find(vbd1)) LIFT_CHECK_EQ_U32(r.f.method(vbd1, kOffRelease, {}), 0u);  // 越界：对象死
    LIFT_CHECK(!com->find(vbd1));
    LIFT_CHECK_EQ_U32(r.f.method(vbd_dummy, kOffRelease, {}), 0u);

    std::uint32_t vbd2 = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(60u, kFvfXyzRhwDiffuse, &vbd2), 0u);
    LIFT_CHECK_EQ_U32(vbd2, vbd1);                      // 前提 1：对象 EA 复用
    LIFT_CHECK_EQ_U32(com->userOf(vbd2), vbd1_state);    // 前提 2：`'VB09'` 状态块 EA 也复用
    LIFT_CHECK_NE_U32(com->generationOf(vbd2), vbd1_gen);  // 前提 3：身份换了
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, vbd2, 0u, 20u), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(vbd2), 2u);        // ← 修前 = 1（被当成"重复 Set 同一个"）
    LIFT_CHECK_EQ_U32(r.f.method(vbd2, kOffRelease, {}), 1u);  // guest 放掉自己那份
    LIFT_CHECK(com->find(vbd2));                               // ← 修前：对象当场归 0
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, 0u, 0u, 0u), 0u);  // 解绑 ⇒ 设备那份还回
    LIFT_CHECK(!com->find(vbd2));
    LIFT_CHECK_EQ_U32(r.f.m.heap().bytesInUse(), heap_before);

    // ---- ③-E 索引缓冲：同形判据（`SetIndices` 的槽）----
    std::uint32_t ibd1 = 0;
    std::uint32_t ibd_dummy = 0;
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(12u, 101u, &ibd1), 0u);
    if (ibd1 == 0) return;
    const std::uint32_t ibd1_state = com->userOf(ibd1);
    const std::uint32_t ibd1_gen = com->generationOf(ibd1);
    LIFT_CHECK_NE_U32(ibd1_state, 0u);
    LIFT_CHECK_NE_U32(ibd1_gen, 0u);
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(12u, 101u, &ibd_dummy), 0u);
    if (ibd_dummy == 0) return;
    LIFT_CHECK_EQ_U32(r.f.setIndices(ibd1), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(ibd1), 2u);
    LIFT_CHECK_EQ_U32(r.f.method(ibd1, kOffRelease, {}), 1u);
    if (com->find(ibd1)) LIFT_CHECK_EQ_U32(r.f.method(ibd1, kOffRelease, {}), 0u);
    LIFT_CHECK(!com->find(ibd1));
    LIFT_CHECK_EQ_U32(r.f.method(ibd_dummy, kOffRelease, {}), 0u);

    std::uint32_t ibd2 = 0;
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(12u, 101u, &ibd2), 0u);
    LIFT_CHECK_EQ_U32(ibd2, ibd1);                       // 前提 1：对象 EA 复用
    LIFT_CHECK_EQ_U32(com->userOf(ibd2), ibd1_state);     // 前提 2：`'IB09'` 状态块 EA 也复用
    LIFT_CHECK_NE_U32(com->generationOf(ibd2), ibd1_gen);  // 前提 3：身份换了
    LIFT_CHECK_EQ_U32(r.f.setIndices(ibd2), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(ibd2), 2u);          // ← 修前 = 1
    LIFT_CHECK_EQ_U32(r.f.method(ibd2, kOffRelease, {}), 1u);
    LIFT_CHECK(com->find(ibd2));                          // ← 修前：对象当场归 0
    LIFT_CHECK_EQ_U32(r.f.setIndices(0u), 0u);            // 解绑 ⇒ 设备那份还回
    LIFT_CHECK(!com->find(ibd2));
    LIFT_CHECK_EQ_U32(r.f.m.heap().bytesInUse(), heap_before);

    // ---- ③-F 设备销毁：设备替 guest 持的那份也必须成对还回去（★ 放最后：它会销毁设备）----
    //   ★ 判据：销毁之后缓冲**还在**（只剩 guest 那份，refCount = 1）—— 若流水线把自己的绑定引用
    //     也当成"guest 的"减掉，这里会看到对象已经没了；若**不**还，缓冲就永久钉在对象表里。
    std::uint32_t vb2 = 0;
    std::uint32_t ib2 = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(60u, kFvfXyzRhwDiffuse, &vb2), 0u);
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(12u, 101u, &ib2), 0u);
    if (vb2 == 0 || ib2 == 0) return;
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, vb2, 0u, 20u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setIndices(ib2), 0u);
    LIFT_CHECK_EQ_U32(com->refCount(vb2), 2u);
    LIFT_CHECK_EQ_U32(com->refCount(ib2), 2u);
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffRelease, {}), 0u);  // 设备到 0 ⇒ destroyForDevice
    LIFT_CHECK(com->find(vb2));
    LIFT_CHECK(com->find(ib2));
    LIFT_CHECK_EQ_U32(com->refCount(vb2), 1u);  // 设备那份已还
    LIFT_CHECK_EQ_U32(com->refCount(ib2), 1u);
    LIFT_CHECK_EQ_U32(r.f.method(vb2, kOffRelease, {}), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(ib2, kOffRelease, {}), 0u);
    LIFT_CHECK(!com->find(vb2));
    LIFT_CHECK(!com->find(ib2));
}

// =======================================================================================
// ⑭ `pipeWarn` 的配额 key = (调用点, 类别) + 每类拒绝次数进运行摘要
//    （`REQ-01M70SHFT6RZA7RRY1R051Z2PB` #2）
// =======================================================================================
// 缺陷形态：配额 key 只有**调用点名** ⇒ 同一个 `DrawIndexedPrimitive` 上"顶点流陈旧"每帧刷屏会把
//   "纹理陈旧"的可见配额吃光（第 5 次起纹理那条只打 debug），且拒绝次数**不进** `dumpRunSummary`
//   ⇒ 默认跑法里那条信号既看不见也数不到。
// 判据（红了意味着什么）：
//   ① 两类各造 6 次拒绝（**同一个调用点** `DrawIndexedPrimitive`），默认级别（warn）下 stderr 里
//      `[warn] [d3d9] DrawIndexedPrimitive:` ≥ 10 行（每类 4 条全量 + 1 条"打满"行；共用桶时只有 5 行）；
//   ② 运行摘要的 `pipe-warn` 行里**两类计数都在**（各 6），且 index / other 的增量为 0；
//   ③ 两类各自的告警文本都出现过（证明不是一类把另一类顶掉）。
// ★ 变异（key 退回只用调用点名）⇒ ① 的 10 行降到 5 行 ⇒ 红。
// ★ 触发路径都是**真 API**（SetStreamSource / SetIndices / SetTexture / DrawIndexedPrimitive）；
//   计数与限流桶同一份（进程级 ⇒ 桶的选择要避开更早用例用过的那个）。
// =======================================================================================
void casePipeWarnCategoriesAndSummary() {
    lift_test::g_current_case = "d3d9p/pipe-warn-categories";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;
    lift::ComRegistry* com = r.f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return;

    // 默认级别；下面的断言就是"默认跑法看得见 / 数得到"
    lift::logSetLevel("warn");
    LIFT_CHECK(lift::logConfig().level == lift::LogLevel::Warn);

    const lift::d3d9::PipeWarnCounts before = lift::d3d9::pipeWarnCounts();
    auto delta = [&before](lift::d3d9::PipeWarnCategory cat) -> long long {
        return static_cast<long long>(lift::d3d9::pipeWarnCounts().of(cat) -
                                      before.of(cat));
    };

    // 状态：无混合 / 无 Z，FVF = XYZRHW + DIFFUSE + TEX1（28 B/顶点）
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuseTex1), 0u);

    std::uint32_t vb = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(3u * 28u, kFvfXyzRhwDiffuseTex1, &vb), 0u);
    LIFT_CHECK_NE_U32(vb, 0u);
    if (vb == 0) return;
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, vb, 0u, 28u), 0u);
    // 索引缓冲（3 个 INDEX16）—— 两类拒绝都走**同一个调用点** `DrawIndexedPrimitive`：
    //   ★ 为什么不用 `DrawPrimitive`：本进程里更早的用例已经用过那个桶（配额是进程级的），
    //     这里要的是"两类各自从 0 开始"的干净桶；`DrawIndexedPrimitive` 在本文件里只被
    //     `d3d9p/stale-stream-binding` 用过一次，且那次是**索引**类（第三个桶），不污染这两个。
    std::uint32_t ib = 0;
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(12u, 101u, &ib), 0u);
    LIFT_CHECK_NE_U32(ib, 0u);
    if (ib == 0) return;
    {
        std::uint32_t ibase = 0;
        LIFT_CHECK_EQ_U32(r.f.lockBuffer(ib, &ibase), 0u);
        LIFT_CHECK_NE_U32(ibase, 0u);
        if (ibase != 0)
            for (int i = 0; i < 3; ++i)
                r.f.m.mem().wr16(ibase + static_cast<std::uint32_t>(i) * 2u, static_cast<std::uint16_t>(i));
        LIFT_CHECK_EQ_U32(r.f.unlockBuffer(ib), 0u);
    }
    LIFT_CHECK_EQ_U32(r.f.setIndices(ib), 0u);

    std::uint32_t tex_a = 0;
    std::uint32_t tex_dummy = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex_a), 0u);
    LIFT_CHECK_NE_U32(tex_a, 0u);
    if (tex_a == 0) return;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex_dummy), 0u);
    LIFT_CHECK_NE_U32(tex_dummy, 0u);
    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, tex_a), 0u);
    // 越界释放 A（guest 那份 + 设备替绑定持的那份）⇒ 对象死；同尺寸 B 拿回同一对象 EA
    LIFT_CHECK_EQ_U32(r.f.method(tex_a, kOffRelease, {}), 1u);  // 设备那份还在
    if (com->find(tex_a)) LIFT_CHECK_EQ_U32(r.f.method(tex_a, kOffRelease, {}), 0u);
    LIFT_CHECK(!com->find(tex_a));
    LIFT_CHECK_EQ_U32(r.f.method(tex_dummy, kOffRelease, {}), 0u);
    std::uint32_t tex_b = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex_b), 0u);
    LIFT_CHECK_NE_U32(tex_b, 0u);
    if (tex_b == 0) return;
    LIFT_CHECK_EQ_U32(tex_b, tex_a);  // 前提：对象 EA 复用

    StderrTee cap;
    LIFT_CHECK(cap.begin());

    // ---- 类别 ①：纹理陈旧（同一个调用点 DrawIndexedPrimitive，6 次）----
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    for (int i = 0; i < 6; ++i)
        LIFT_CHECK_EQ_U32(r.f.drawIndexed(lift::d3d9::kPtTriangleList, 0u, 0u, 3u, 0u, 1u),
                          lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);

    // ---- 类别 ②：顶点流陈旧（**同一个**调用点，6 次）----
    //   先越界放掉顶点缓冲（guest 那份 + 设备绑定那份）⇒ 槽里的 (EA, 代际) 已不是对象
    LIFT_CHECK_EQ_U32(r.f.method(vb, kOffRelease, {}), 1u);
    if (com->find(vb)) LIFT_CHECK_EQ_U32(r.f.method(vb, kOffRelease, {}), 0u);
    LIFT_CHECK(!com->find(vb));
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    for (int i = 0; i < 6; ++i)
        LIFT_CHECK_EQ_U32(r.f.drawIndexed(lift::d3d9::kPtTriangleList, 0u, 0u, 3u, 0u, 1u),
                          lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);

    const std::string log = cap.finish();

    // ① 默认级别下，两类都**超过**每桶 4 条的配额后仍各留一行（第 5 条是"打满"汇总行）
    LIFT_CHECK(contains(log, "纹理级 0 的槽里"));
    LIFT_CHECK(contains(log, "已不是绑定时那张纹理"));
    LIFT_CHECK(contains(log, "绑定身份不符：槽里的顶点缓冲"));
    LIFT_CHECK(countOccurrences(log, "[warn] [d3d9] DrawIndexedPrimitive:") >= 10u);

    // ② 每类拒绝次数都进了运行摘要（各 6；index / other 未被污染）
    LIFT_CHECK_EQ_I64(delta(lift::d3d9::PipeWarnCategory::VertexStream), 6);
    LIFT_CHECK_EQ_I64(delta(lift::d3d9::PipeWarnCategory::Texture), 6);
    LIFT_CHECK_EQ_I64(delta(lift::d3d9::PipeWarnCategory::Index), 0);
    LIFT_CHECK_EQ_I64(delta(lift::d3d9::PipeWarnCategory::Other), 0);
    const lift::d3d9::PipeWarnCounts& wc = lift::d3d9::pipeWarnCounts();
    char want[256];
    std::snprintf(want, sizeof(want), "pipe-warn : vertex-stream=%llu index=%llu texture=%llu other=%llu",
                  (unsigned long long)wc.of(lift::d3d9::PipeWarnCategory::VertexStream),
                  (unsigned long long)wc.of(lift::d3d9::PipeWarnCategory::Index),
                  (unsigned long long)wc.of(lift::d3d9::PipeWarnCategory::Texture),
                  (unsigned long long)wc.of(lift::d3d9::PipeWarnCategory::Other));
    const std::string summary = captureRunSummary(r.f.m);
    LIFT_CHECK(contains(summary, want));

    // 收尾：B 的 guest 那份（判活再调：EA 复用下这里总是活着的）
    if (com->find(tex_b)) LIFT_CHECK_EQ_U32(r.f.method(tex_b, kOffRelease, {}), 0u);
    LIFT_CHECK(!com->find(tex_b));
}

}  // namespace

int main() {
    using namespace lift_test;
    try {
        caseTransformRoundtrip();
        caseRenderStateBound();
        caseScenePairing();
        caseSolidTriangle();
        caseTexturedQuad();
        caseMinFilterSampling();
        caseMipLevels();
        caseZBuffer();
        caseAlphaBlend();
        caseDrawStateSnapshot();
        caseClipPlanes();
        caseRenderTargetLifetime();
        caseSetRenderTargetViewport();
        caseOwnerLifetimeAndStaleGeneration();
        caseStaleStreamBinding();
        caseStaleTextureBindingInDraw();
        casePipeWarnCategoriesAndSummary();
        caseBindingLifetime();
        return finish("d3d9-pipeline");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "DIAG uncaught std::exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "DIAG uncaught non-std exception\n");
        return 1;
    }
}
