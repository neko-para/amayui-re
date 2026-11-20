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
// ★ **两条臂**（`REQ-01M7A74YG08R65NJBFXF7DEJDK`：CPU 光栅已删）：
//   · 缺省（`d3d9-pipeline`）：①②⑧ 的状态/配对/快照合同 + 绑定生命周期 + 拒绝类合同（不读像素）；
//   · `--gpu`（`d3d9-pipeline-gpu`，真 GPU 后端 + 读回）：③⑤⑥⑦ 里**保留**的像素断言
//     （见各用例头；没有 GPU 设备 ⇒ SKIP 77，⛔ 不假绿）。
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
#include "gpu_arm.h"
#include "runtime/d3d9.h"
#include "runtime/d3d9_pipeline.h"
#include "runtime/log.h"
#include "runtime/machine.h"
#include "runtime/math.h"  // `perspectiveFovLH` / `toD3DLayout`（用例里要"真的透视矩阵"，⛔ 不手抄 16 个数）
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
constexpr std::uint32_t kOffValidateDevice = 0x11Cu;  // ★ #71（`.lst` 无引擎站点；见 shims 方法表）
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
// ★ 声明路（Live2D 在 `sub_4C1BE5` 用 +0x158 建、随后 +0x15C 绑定；见 shims/d3d9.cpp 方法表 #86/#87）
constexpr std::uint32_t kOffCreateVertexDeclaration = 0x158u;
constexpr std::uint32_t kOffSetVertexDeclaration = 0x15Cu;
constexpr std::uint32_t kOffCreateOffscreenPlainSurface = 0x90u;  // ★ 0x4A0570
// ★ IDirect3DSurface9 的 LockRect / UnlockRect 是 **+0x34 / +0x38**（3 参 / 0 参）——
//   ⛔ 不是纹理的 +0x4C / +0x50（4 参 / 1 参，带 Level）；见 `shims/d3dx9.cpp` 的 `surfaceMethods`。
constexpr std::uint32_t kOffSurfaceLockRect = 0x34u;
constexpr std::uint32_t kOffSurfaceUnlockRect = 0x38u;

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
    std::uint32_t drawIndexed(std::uint32_t prim, std::int32_t base_vertex, std::uint32_t min_index,
                              std::uint32_t num_vertices, std::uint32_t start_index,
                              std::uint32_t prim_count) {
        // ★ BaseVertexIndex 的 guest ABI 是 INT：按位传给 shim，由 shim 做 int32 解释。
        return method(device.object, kOffDrawIndexedPrimitive,
                      {prim, static_cast<std::uint32_t>(base_vertex), min_index, num_vertices, start_index,
                       prim_count});
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
    lift_test::GpuArm arm;
    bool up() {
        if (!f.boot()) return false;
        const std::uint32_t d3d9 = f.createDirect3D();
        if (d3d9 == 0) return false;
        if (!f.createDevice(d3d9, 64, 64, lift::d3d9::kFmtX8R8G8B8, &f.device)) return false;
        return f.device.object != 0;
    }
    // 真 GPU 臂（CPU 光栅已删 ⇒ 要画像素的用例必须接后端；否则 Draw 一律 INVALIDCALL）
    bool upGpu(std::string* why) {
        if (!up()) {
            if (why) *why = "夹具启动失败";
            return false;
        }
        return arm.attach(f.m, f.device.info.arena, why);
    }
    // 读像素前的**唯一**入口（Draw 只入队）
    bool syncPixel(std::string* why) { return arm.sync(f.m, why); }
};

constexpr std::uint32_t kClearColor = 0xFF102030u;  // 清屏色（X8R8G8B8 ⇒ 存成 0x00102030）
constexpr std::uint32_t kClearPixel = 0x00102030u;

// `unsupported` 位图（`d3d9_pipeline.cpp` 的 `enum UnsupportedBit`）与一条取位助手。
// ★ 拒绝类判据（"这一批被**哪条**准入挡掉"）落在位上，与 `tests/gpu_batch.cpp` 的 `infoBits` 同形。
bool bitIsSet(const lift::d3d9::PipelineInfo& pi, std::uint32_t bit) {
    return (pi.unsupported[bit / 32u] & (1u << (bit % 32u))) != 0u;
}
bool deviceInfoBit(Fixture& f, std::uint32_t bit) {
    lift::d3d9::PipelineInfo pi;
    if (!lift::d3d9::pipelineInfoOfDevice(f.m.cpu(), f.device.state, &pi, nullptr)) return false;
    return bitIsSet(pi, bit);
}

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
    //   ★ 2026-11-14 起 Get 的接受域 = 官方 `D3DRENDERSTATETYPE`（`REQ-01M7A3M73R3XYMM7J4KNQ1VNV7`）：
    //     0..255 里**不是** D3DRS 的那些项读回 `getRs` 的失败哨兵 0xDEADBEEF（= INVALIDCALL）。
    //     本用例的判据是"越界 Set 之后**逐项**相同"，所以哨兵项照样是基线的一部分。
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
int caseSolidTriangle() {
    lift_test::g_current_case = "d3d9p/solid-triangle";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    // ★ `[present-readback]` Phase B（`REQ-01M7W2FR9EJEMG2QDMZNG6N7WV`）起：全幅 Clear 的目标就是
    //   当前 GPU 镜像时，清色走 GPU 侧 `LOADOP_CLEAR` ⇒ **guest 后缓冲当场不再是这一帧**，只有
    //   读点 / 诊断点才会就地材质化。⇒ 原来这里那条"Clear 直写 guest"（`pixelAt(0,0)==kClearPixel`）
    //   不再成立（那是被本单**有意拿掉**的行为，不是判据放宽）：清色的像素判据搬到下面
    //   `syncPixel()` 之后 —— `pixelAt(9,9) == kClearPixel` 仍是"清色真的落地"的像素断言。
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
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());

    const std::uint32_t tri_pixel = 0x00FF8000u;
    // 内部 = 顶点色（像素中心严格在三条边之内）；外部 = 清屏色。
    // ★ 这两条是**用户可见**的判据（三角形画不出来 / 糊到包围盒），⛔ 不是边界像素的填充规则。
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 1), tri_pixel);      // (0.5,1.5)
    LIFT_CHECK_EQ_U32(r.f.pixelAt(9, 9), kClearPixel);    // (9.5,9.5)：19 > 10

    // ★ 同一三角形走 XYZ 路 + WORLD 平移 (0.25,0.25) ⇒ 画到 (41,23) 一带
    //   （非预变换顶点真的过了 WORLD/VIEW/PROJECTION + 裁剪 + 屏幕映射）。
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
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(41, 23), tri_pixel);   // 中心 (41.5,23.5) 在平移后的三角形内
    // 还原 WORLD（后面用例不依赖它，但保持确定性）
    LIFT_CHECK_EQ_U32(r.f.setTransform(lift::d3d9::kTsWorld, std::vector<float>(
        {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1})), 0u);
    return 0;
}

// ---------------------------------------------------------------------------------------
// ④ 带纹理的四边形：POINT 采样 ⇒ 角像素 = 纹理角 texel
// ---------------------------------------------------------------------------------------
int caseTexturedQuad() {
    lift_test::g_current_case = "d3d9p/textured-quad";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

    // 2×2 纹理（A8R8G8B8 = 21）：对角四色，便于一眼看出采样对不对
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return 0;
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
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());

    // 2×2 纹理的**对角**角像素 = 纹理对角 texel（POINT + CLAMP ⇒ 纯最近邻，容差 0）。
    // ★ 用户可见的判据：纹理采样把 u/v 映射错（贴图翻转 / 整块单色）⇒ 花屏。
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), 0x00FF0000u);  // 左上 = 红 (u,v)=(0,0)
    LIFT_CHECK_EQ_U32(r.f.pixelAt(7, 7), 0x00FFFFFFu);  // 右下 = 白 (u,v)=(1,1)

    // 解绑纹理 ⇒ 后面的用例不受影响（也让 SetTexture(0,NULL) 的 Release 路径被走到）
    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 0u);  // 纹理 Release 到 0
    return 0;
}

// ---------------------------------------------------------------------------------------
// ④b MAG / MIN 两档过滤**各自参与**、且 `MAG != MIN` 的批**必须照画**
//   （`REQ-01M7AZ68J05PMVWVY7A589TQ6Y`：以前这条批被 `kUnGpuFilterSplit` 整批拒绝）
//   · (a) 放大臂（2×2 纹理 → 8×8 像素）：放大用 `MAGFILTER` ⇒ MAG=POINT 纯 texel、
//     MAG=LINEAR 双线性（两值必须不同）；
//   · (b) 缩小臂（8×8 纹理 → 2×2 像素，`MIPFILTER=NONE` ⇒ 只采 level 0）：缩小用 `MINFILTER`
//     ⇒ MIN=POINT 取最近 texel、MIN=LINEAR 取双线性（手算值，两值必须不同）。
//   ★ 判据落在**像素**上（不是"我们拒绝了"）：过滤真的分开生效。
// ---------------------------------------------------------------------------------------
int caseMinFilterSampling() {
    lift_test::g_current_case = "d3d9p/min-filter";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

    // 2×2 纹理：左上红 / 右上绿 / 左下蓝 / 右下白（与 caseTexturedQuad 同一组颜色）
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return 0;
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

    // 清屏 + Begin + Draw + **当场取覆盖账** + End：`gpuCoverage()` 每次 `Machine::run` 都会清零，
    //   而 begin/endScene 各是一轮 ⇒ 必须在**紧接这次 Draw 之后**读（否则读到的是 endScene 那一轮）。
    auto drawQuad = [&](lift::d3d9::GpuCoverage* cov) {
        LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);  // 每臂从清屏开始：像素都是"这一臂画的"
        LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
        const std::uint32_t hr = r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, data, 28u);
        if (cov != nullptr) *cov = lift::d3d9::gpuCoverage();
        LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
        return hr;
    };

    // ===== (a) 放大臂（2×2 纹理铺 8×8 像素 = 4:1 **放大**）=====
    // D3D9 放大采样用 `MAGFILTER`（MINFILTER 不参与）：
    //   (a1) MAG=POINT + MIN=LINEAR ⇒ (2,2) = texel(0,0) 纯红 `0x00FF0000`；
    //   (a2) MAG=LINEAR + MIN=POINT ⇒ (2,2) = 双线性 `0x00C72020`。
    // ⛔ 旧期望 `0x00C72020` 属于 (a2)，不属于 MAG=POINT 那一臂 —— 旧用例把它写在 MAG=POINT 上，
    //    断的其实是"CPU 把 MIN 用在放大上"这条**不合规范**的行为
    //    （`REQ-01M7AZ68J05PMVWVY7A589TQ6Y`，⛔ 不要恢复那条）。
    //   两臂都带 MAG!=MIN ⇒ 同时证明"准入不再整批拒绝"。
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfLinear), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint), 0u);
    lift::d3d9::GpuCoverage cov{};
    LIFT_CHECK_EQ_U32(drawQuad(&cov), lift::d3d9::kOk);  // MAG!=MIN 不再整批拒绝
    LIFT_CHECK(cov.queued >= 1u);                        // 真的进了 GPU 队列
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.skipped_draws), 0);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    const std::uint32_t mag_point_px = r.f.pixelAt(2, 2);
    LIFT_CHECK_EQ_U32(mag_point_px, 0x00FF0000u);

    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfLinear), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(drawQuad(&cov), lift::d3d9::kOk);
    LIFT_CHECK(cov.queued >= 1u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.skipped_draws), 0);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    const std::uint32_t mag_linear_px = r.f.pixelAt(2, 2);
    LIFT_CHECK(lift_test::pxNear(mag_linear_px, 0x00C72020u, 1));  // 双线性（unorm 舍入 ±1）
    LIFT_CHECK(mag_point_px != mag_linear_px);  // 反向条款：MAG 两档必须产生不同像素

    // ===== (b) 缩小臂（8×8 纹理铺 2×2 像素 = 4:1 **缩小**；MIPFILTER=NONE ⇒ 只采 level 0）=====
    // 纹理：R = 10*x（每行相同）、A=255 ⇒ 只有 x 方向有梯度，手算简单：
    //   (0,0) 像素中心 ⇒ u = 0.05 + 0.25*0.9 = 0.275 ⇒ level-0 texel 坐标 2.2：
    //     MIN=POINT  ⇒ texel(2,·)：R = 20 ⇒ `0x00140000`
    //     MIN=LINEAR ⇒ taps x=1/2（权重 0.3/0.7）：R = 0.3*10 + 0.7*20 = 17 ⇒ `0x00110000`
    //   ★ 两个值**必须不同**：这才是"MINFILTER 真的参与缩小采样"的判据（旧用例缺的就是这一臂）。
    std::uint32_t shrink_tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 8u, 8u, lift::d3d9::kFmtA8R8G8B8, &shrink_tex), 0u);
    LIFT_CHECK_NE_U32(shrink_tex, 0u);
    if (shrink_tex == 0) return 0;
    {
        const std::uint32_t locked8 = r.f.placeBytes(std::vector<std::uint8_t>(8, 0));
        LIFT_CHECK_EQ_U32(r.f.method(shrink_tex, kOffTextureLockRect, {0u, locked8, 0u, 0u}), 0u);
        const std::uint32_t pitch8 = r.f.m.mem().rd32(locked8 + 0u);
        const std::uint32_t bits8 = r.f.m.mem().rd32(locked8 + 4u);
        for (int y = 0; y < 8; ++y) {
            for (int x = 0; x < 8; ++x) {
                const std::uint32_t rch = static_cast<std::uint32_t>(10 * x);
                r.f.m.mem().wr32(bits8 + static_cast<std::uint32_t>(y) * pitch8 +
                                     static_cast<std::uint32_t>(x) * 4u,
                                 0xFF000000u | (rch << 16));
            }
        }
        LIFT_CHECK_EQ_U32(r.f.method(shrink_tex, kOffTextureUnlockRect, {0u}), 0u);
    }
    const std::vector<Fixture::TexVertex> shrink_quad = {
        {0.0f, 0.0f, 0.0f, 1.0f, white, 0.05f, 0.05f},
        {2.0f, 0.0f, 0.0f, 1.0f, white, 0.95f, 0.05f},
        {0.0f, 2.0f, 0.0f, 1.0f, white, 0.05f, 0.95f},
        {2.0f, 0.0f, 0.0f, 1.0f, white, 0.95f, 0.05f},
        {2.0f, 2.0f, 0.0f, 1.0f, white, 0.95f, 0.95f},
        {0.0f, 2.0f, 0.0f, 1.0f, white, 0.05f, 0.95f},
    };
    const std::uint32_t shrink_data = r.f.placeTexVertices(shrink_quad);
    auto drawShrink = [&](lift::d3d9::GpuCoverage* cov) {
        LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
        LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
        const std::uint32_t hr = r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, shrink_data, 28u);
        if (cov != nullptr) *cov = lift::d3d9::gpuCoverage();
        LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
        return hr;
    };
    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, shrink_tex), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMipFilter, lift::d3d9::kTexfNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(drawShrink(&cov), lift::d3d9::kOk);
    LIFT_CHECK(cov.queued >= 1u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.skipped_draws), 0);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    const std::uint32_t shrink_point = r.f.pixelAt(0, 0);
    LIFT_CHECK_EQ_U32(shrink_point, 0x00140000u);

    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfLinear), 0u);
    LIFT_CHECK_EQ_U32(drawShrink(&cov), lift::d3d9::kOk);
    LIFT_CHECK(cov.queued >= 1u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.skipped_draws), 0);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    const std::uint32_t shrink_linear = r.f.pixelAt(0, 0);
    LIFT_CHECK(lift_test::pxNear(shrink_linear, 0x00110000u, 1));
    LIFT_CHECK(shrink_point != shrink_linear);  // 反向条款：MIN 两档必须产生不同像素

    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(shrink_tex, 0x08u, {}), 0u);  // Release
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 0u);
    return 0;
}

// ---------------------------------------------------------------------------------------
// ⑤b 派生 mip 的**选级**（`REQ-01M73G2724479XAVS176WBZ65Q`；缩小采样必须有 mip）
//
// 判据（红了意味着什么）—— 全部落在**选级之后的像素**上（GPU 的级号由驱动按 UV 导数算，
//   纯函数守不到）：
//   · `MIPFILTER=POINT`：2:1 缩小 ⇒ level 1 的 box 值 `[3,16;114,0]`；4:1 ⇒ level 2 = 33；
//     1:1 ⇒ 逐 texel 等于 level 0；
//   · `MIPFILTER=NONE`（`D3DTEXF_NONE` = "disables mipmapping"）⇒ **只采 level 0**：4:1 时
//     (0,0) = level 0 的 texel(2,2) = 0（⛔ 不是 level 2 的 33）；
//   · 反向条款：同一几何下 NONE 与 POINT 必须产生**不同**像素（0 vs 33）—— 判别力在这里，
//     ⛔ 不在"我们拒绝了"上。
//   · 派生链的**内容 / 取整**仍由 `tests/d3d9_mip.cpp`（pure）守；本用例守"选级后的像素"。
//   · 修前这一条是"拒绝 + 记账"（bit22）；那个拒绝位已随真实现（第二个 sampler：`max_lod`
//     收紧到 (0,0.5)，只采 level 0 且 MINFILTER 仍按缩小生效）删除。
// ---------------------------------------------------------------------------------------
int caseMipLevels() {
    lift_test::g_current_case = "d3d9p/mip-levels";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 4u, 4u, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return 0;

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
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuseTex1), 0u);

    // 三档几何都由同一个 lambda 生成；UV 稍微内缩（0.06..1.0），让采样点避开 texel 边界的
    //   半整数（⛔ 不靠"恰好落在边界上"的舍入行为当判据）。级号仍由 GPU 按 UV 导数选。
    const std::uint32_t white = 0xFFFFFFFFu;
    const float kU0 = 0.06f;
    const float kU1 = 1.0f;
    auto makeQuad = [&](float extent) {
        const std::vector<Fixture::TexVertex> quad = {
            {0.0f, 0.0f, 0.0f, 1.0f, white, kU0, kU0},
            {extent, 0.0f, 0.0f, 1.0f, white, kU1, kU0},
            {0.0f, extent, 0.0f, 1.0f, white, kU0, kU1},
            {extent, 0.0f, 0.0f, 1.0f, white, kU1, kU0},
            {extent, extent, 0.0f, 1.0f, white, kU1, kU1},
            {0.0f, extent, 0.0f, 1.0f, white, kU0, kU1},
        };
        return r.f.placeTexVertices(quad);
    };
    // 2:1 缩小（2×2 像素覆盖 4×4 纹理 ⇒ 足迹 1.88 texel/px ⇒ LOD≈0.91 ⇒ level 1）；
    // 4:1 用 0.9 像素（LOD≈2.06 ⇒ level 2）；1:1 用 4 像素（LOD<0 ⇒ level 0）。
    const std::uint32_t data_2x = makeQuad(2.0f);
    const std::uint32_t data_4x = makeQuad(0.9f);
    const std::uint32_t data_1x = makeQuad(4.0f);

    auto drawQuad = [&](std::uint32_t data, lift::d3d9::GpuCoverage* cov) {
        LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
        LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
        const std::uint32_t hr = r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, data, 28u);
        if (cov != nullptr) *cov = lift::d3d9::gpuCoverage();  // 紧接 Draw（下一轮会清零）
        LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
        return hr;
    };
    lift::d3d9::GpuCoverage cov{};
    // ★ 返回 bool 而不是在 lambda 里 `LIFT_SKIP`：那条宏展开成 `return 77;`，写在 lambda 里是
    //   **return 给调用点丢掉**（进程照跑、skip 静默失效）。⇒ 跳过只能由调用点做。
    auto drawAndSync = [&](std::uint32_t data) {
        LIFT_CHECK_EQ_U32(drawQuad(data, &cov), lift::d3d9::kOk);  // 缩小采样不再整批拒绝
        LIFT_CHECK(cov.queued >= 1u);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.skipped_draws), 0);
        return r.syncPixel(&why);
    };

    // ---- ① MIPFILTER=POINT：2:1 缩小 ⇒ level 1 的 box 值 [[3,16],[114,0]] ----
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMipFilter, lift::d3d9::kTexfPoint), 0u);
    if (!drawAndSync(data_2x)) LIFT_SKIP(why.c_str());
    // 手算值 = 3 / 16 / 114 / 0（写死，⛔ 不要用 `texels[]` 现算 —— 那会把"期望抄成实际"）：
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), 0x00030000u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(1, 0), 0x00100000u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 1), 0x00720000u);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(1, 1), 0x00000000u);

    // ---- ② MIPFILTER=POINT：4:1 缩小 ⇒ level 2 = 33 ----
    if (!drawAndSync(data_4x)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), 0x00210000u);

    // ---- ③ MIPFILTER=POINT：1:1 ⇒ 逐 texel 等于 level 0（第 0 行与第 3 行）----
    if (!drawAndSync(data_1x)) LIFT_SKIP(why.c_str());
    for (int x = 0; x < 4; ++x) LIFT_CHECK_EQ_U32(r.f.pixelAt(x, 0), texels[x] << 16);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 3), texels[12] << 16);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(1, 3), texels[13] << 16);

    // ---- ④ MIPFILTER=NONE：4:1 仍**只采 level 0**（`D3DTEXF_NONE` = 关掉 mipmap）----
    //   采样点落在 level-0 texel(2,2) = 0；若后端错用 mip 链 ⇒ 会读到 level2 的 33。
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMipFilter, lift::d3d9::kTexfNone), 0u);
    if (!drawAndSync(data_4x)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), 0x00000000u);

    // ---- ⑤ 反向条款：同一几何切回 MIPFILTER=POINT ⇒ level 2 = 33（两档必须不同）----
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMipFilter, lift::d3d9::kTexfPoint), 0u);
    if (!drawAndSync(data_4x)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), 0x00210000u);

    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 0u);  // 纹理 Release 到 0
    return 0;
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

int caseZBuffer() {
    lift_test::g_current_case = "d3d9p/zenable-zwrite";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZFunc, lift::d3d9::kCmpLessEqual), 0u);
    const std::uint32_t green = 0xFF00FF00u;  // 近
    const std::uint32_t red = 0xFFFF0000u;    // 远
    const std::uint32_t green_px = 0x0000FF00u;
    const std::uint32_t red_px = 0x00FF0000u;

    // (a) ZENABLE=1 / ZWRITEENABLE=1：近的先画、远的后画 ⇒ 中心仍是近的（远的不覆盖）
    //     ★ 用户可见的错：遮挡关系反了 ⇒ 该被挡住的图层画在最前面（深度穿帮）。
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);  // 同时清深度（Flags=3）
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 1u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZWriteEnable, 1u), 0u);
    drawQuadAtScreen(r, 4.0f, 4.0f, 60.0f, 60.0f, 0.2f, green);  // 近
    drawQuadAtScreen(r, 4.0f, 4.0f, 60.0f, 60.0f, 0.8f, red);    // 远
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(32, 32), green_px);

    // (b) ZENABLE=0：远的一定覆盖 ⇒ 中心是远的（状态真的被映射成"不做深度测试"）
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    drawQuadAtScreen(r, 4.0f, 4.0f, 60.0f, 60.0f, 0.2f, green);
    drawQuadAtScreen(r, 4.0f, 4.0f, 60.0f, 60.0f, 0.8f, red);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(32, 32), red_px);

    // (c) ZENABLE=1 / ZWRITEENABLE=0：近的不写深度 ⇒ 远的仍然通过（覆盖）
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 1u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZWriteEnable, 0u), 0u);
    drawQuadAtScreen(r, 4.0f, 4.0f, 60.0f, 60.0f, 0.2f, green);
    drawQuadAtScreen(r, 4.0f, 4.0f, 60.0f, 60.0f, 0.8f, red);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(32, 32), red_px);
    // ZWRITEENABLE 恢复 1（后面用例不依赖它，但保持确定）
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZWriteEnable, 1u), 0u);
    return 0;
}

// ---------------------------------------------------------------------------------------
// ⑥ 混合：SRCALPHA / INVSRCALPHA 的通道值
// ---------------------------------------------------------------------------------------
int caseAlphaBlend() {
    lift_test::g_current_case = "d3d9p/alphablend";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

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
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());

    // D3D9 的 blend 语义：out = src*sf + dst*df（sf=SRCALPHA=128、df=INVSRCALPHA=127）⇒
    //   R = (255*128)/255 = 128、B = (255*127)/255 = 127。
    // ★ 用户可见的错：半透明图层显示成实心 / 颜色发黑。
    // ★ GPU 的 unorm 舍入与 D3D9 的 8-bit 参考允许每通道 ≤1 LSB（`tests/gpu_raster.cpp` 的先例）。
    LIFT_CHECK(lift_test::pxNear(r.f.pixelAt(1, 1), 0x0080007Fu, 1));
    // 三角形之外 = 清屏色（混合只作用在被覆盖的像素上；Clear 直写 guest，不经光栅）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(40, 40), 0x000000FFu);

    // 反例：DESTBLEND 非法值 ⇒ INVALIDCALL（不静默当默认）
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsDestBlend, 0u), lift::d3d9::kInvalidCall);
    return 0;
}

// ---------------------------------------------------------------------------------------
// ⑧ 每次 Draw 的**状态快照**一次（`stage_snapshots == draws`；偏差 #4）
//
// ★ CPU 光栅已删 ⇒ 本用例只剩**结构判据**（快照次数按 Draw 走、不按三角形/像素走）。
//   原来那 5 条像素断言是 `d3d9p/solid-triangle` 的"同一份填充语义铺到网格上"的重复 ⇒ 删。
// ★ 级 1 的纹理状态（两级都在用）**不再是"像素不变"的旁路**：多 stage 现在是准入拒绝
//   （`kUnGpuMultiStage`）。这里把它单独断言掉 —— 那正是"两 stage 的批现在怎么处置"的真话。
// ---------------------------------------------------------------------------------------
int caseDrawStateSnapshot() {
    lift_test::g_current_case = "d3d9p/draw-state-snapshot";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);

    // 16 个三角形 / 48 个顶点（一次 Draw）：4×4 网格，每个格子画左下直角三角形
    const std::uint32_t tri_color = 0xFF00FF00u;
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
        LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 16u, data, 20u), lift::d3d9::kOk);
        LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    };
    drawAll();

    lift::d3d9::PipelineInfo info;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    // ★ 结构判据：快照次数 == 成功 Draw 次数（⛔ 不随三角形/像素增长）。
    //   把 prepareContext 里的快照删掉 ⇒ 这里恒 0 ⇒ 红。
    LIFT_CHECK_EQ_U32(info.draws, 1u);
    LIFT_CHECK_EQ_U32(info.triangles, 16u);
    LIFT_CHECK_EQ_U32(info.stage_snapshots, info.draws);  // ⛔ 不写死 1

    // 第二次 Draw ⇒ 快照再 +1
    drawAll();
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.draws, 2u);
    LIFT_CHECK_EQ_U32(info.triangles, 32u);
    LIFT_CHECK_EQ_U32(info.stage_snapshots, info.draws);

    // ---- 反向：级 1 真的活跃（COLOROP/ALPHAOP != DISABLE）⇒ 准入**拒绝**该批（bit33）----
    //   ★ 这是 CPU 光栅时代那条"两级都在跑、像素不变"的旁路现在的真话：多 stage 表达不了 ⇒
    //     `D3DERR_INVALIDCALL` + 具名位 + 不进队列 + 不回退 CPU。
    LIFT_CHECK_EQ_U32(r.f.setTextureStage(1u, lift::d3d9::kTssColorOp, lift::d3d9::kTopModulate), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTextureStage(1u, lift::d3d9::kTssAlphaOp, lift::d3d9::kTopModulate), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTextureStage(1u, lift::d3d9::kTssColorArg1, lift::d3d9::kTaCurrent), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTextureStage(1u, lift::d3d9::kTssColorArg2, lift::d3d9::kTaTexture), 0u);
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 16u, data, 20u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    LIFT_CHECK(deviceInfoBit(r.f, 33));  // kUnGpuMultiStage
    {
        const lift::d3d9::GpuCoverage cov = lift::d3d9::gpuCoverage();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.queued), 0);
    }
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.draws, 2u);  // 被拒的 Draw 不进 draws
    return 0;
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

int caseClipPlanes() {
    lift_test::g_current_case = "d3d9p/clip-planes";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

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

    // ① 远平面：z=1.5（w=1）⇒ w-z=-0.5（一个像素都不许写）
    drawXyzTri(r, {{{-0.5f, -0.5f, 1.5f}, {0.5f, -0.5f, 1.5f}, {0.5f, 0.5f, 1.5f}}}, out_color);
    // ② 右平面：x/w ∈ [1.2,1.8] ⇒ 屏幕 x≈51..61（后缓冲内、视口外）
    drawXyzTri(r, {{{1.2f, -0.2f, 0.0f}, {1.8f, -0.2f, 0.0f}, {1.2f, 0.2f, 0.0f}}}, out_color);
    // ③ 左平面：x/w ∈ [-1.8,-1.2] ⇒ 屏幕 x≈3..13
    drawXyzTri(r, {{{-1.8f, -0.2f, 0.0f}, {-1.2f, -0.2f, 0.0f}, {-1.8f, 0.2f, 0.0f}}}, out_color);
    // ④ 上平面：y/w ∈ [1.2,1.8] ⇒ 屏幕 y≈3..13
    drawXyzTri(r, {{{-0.2f, 1.2f, 0.0f}, {0.2f, 1.2f, 0.0f}, {-0.2f, 1.8f, 0.0f}}}, out_color);
    // ⑤ 下平面：y/w ∈ [-1.8,-1.2] ⇒ 屏幕 y≈51..61
    drawXyzTri(r, {{{-0.2f, -1.8f, 0.0f}, {0.2f, -1.8f, 0.0f}, {-0.2f, -1.2f, 0.0f}}}, out_color);

    // 计数器：6 个三角形进装配，只有"在内的那个"真的进入光栅化（裁剪坏了会是 6 ⇒ 红）。
    // ★ 五个平面外的三角形**不需要逐像素断言**：`rasterized` 不增就是"一个像素都没进光栅"。
    lift::d3d9::PipelineInfo info;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.triangles, 6u);
    LIFT_CHECK_EQ_U32(info.rasterized, 1u);

    // ⑥ 部分越界：右平面把三角形**切开**（不是整块丢）⇒ 切开的仍算"进入了光栅化"。
    drawXyzTri(r, {{{0.5f, -0.5f, 0.0f}, {1.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 0.0f}}}, out_color);
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.triangles, 7u);
    LIFT_CHECK_EQ_U32(info.rasterized, 2u);

    // ⑦ XYZRHW 的 x/y 裁剪 = **视口矩形**（齐次面不适用）：屏幕 x=52..60 在视口 16..48 外、
    //    后缓冲（64）内 ⇒ 一个像素都不许进光栅（用户可见的错：几何画到视口外 = 画面边缘花掉）。
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);
    std::vector<std::array<float, 4>> outside_quad = {
        {52.0f, 20.0f, 0.0f, 1.0f}, {60.0f, 20.0f, 0.0f, 1.0f}, {52.0f, 28.0f, 0.0f, 1.0f},
        {60.0f, 20.0f, 0.0f, 1.0f}, {60.0f, 28.0f, 0.0f, 1.0f}, {52.0f, 28.0f, 0.0f, 1.0f}};
    const std::uint32_t outside_data =
        r.f.placeRhwDiffuse(outside_quad, std::vector<std::uint32_t>(outside_quad.size(), out_color));
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, outside_data, 20u), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);

    // 视口内的同一块四边形仍然照画（证明挡住的是"视口外的部分"，不是整条预变换路径）
    std::vector<std::array<float, 4>> inside_quad = {
        {20.0f, 20.0f, 0.0f, 1.0f}, {28.0f, 20.0f, 0.0f, 1.0f}, {20.0f, 28.0f, 0.0f, 1.0f},
        {28.0f, 20.0f, 0.0f, 1.0f}, {28.0f, 28.0f, 0.0f, 1.0f}, {20.0f, 28.0f, 0.0f, 1.0f}};
    const std::uint32_t inside_data =
        r.f.placeRhwDiffuse(inside_quad, std::vector<std::uint32_t>(inside_quad.size(), out_color));
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, inside_data, 20u), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);

    // 11 个三角形进装配；光栅化的 = 在内的 XYZ 三角形 1 + 部分越界（切开的）1 + 视口内四边形的 2
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.triangles, 11u);
    LIFT_CHECK_EQ_U32(info.rasterized, 4u);

    // ---- ⑧ 非全幅视口（16,16,32,32）的像素判据：屏幕→NDC 必须是**视口矩形**的逆 ----
    //   判据是**几何**（不是像素值精度）：
    //     · ① 的 `inside` 三角形：视口内的那一片落在视口内像素、斜边外侧不落；
    //     · ⑥ 被右平面切开的三角形：可见部分落像素、视口外（仍在后缓冲内）不落；
    //     · ⑦ 视口外的 RHW 四边形不落、视口内那块落。
    //   ★ 修前（屏幕→NDC 用 target 尺寸）视口被施加两次 ⇒ `x_win = 0.5*sx + 16`；
    //     (44,36) 会落到 (38,34)、(52,38) 会被压进视口 ⇒ 下面几条当场红。
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(38, 26), in_px);        // (38.5,26.5)：x+y=65 ≥ 64，且在视口内
    LIFT_CHECK_EQ_U32(r.f.pixelAt(39, 39), in_px);        // 近视口右下角，仍在斜边内侧
    LIFT_CHECK_EQ_U32(r.f.pixelAt(30, 30), kClearPixel);  // x+y=61 < 64 ⇒ 斜边外侧
    LIFT_CHECK_EQ_U32(r.f.pixelAt(44, 36), out_px);       // ⑥ 切开的可见部分
    LIFT_CHECK_EQ_U32(r.f.pixelAt(52, 38), kClearPixel);  // ⑥ 被裁掉的部分（仍在后缓冲内）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(55, 24), kClearPixel);  // ⑦ 视口外的 RHW 四边形
    LIFT_CHECK_EQ_U32(r.f.pixelAt(24, 24), out_px);       // ⑦ 视口内的 RHW 四边形

    // ---- ⑨ 反向条款：同一批顶点、**全幅**视口 (0,0,64,64) 重画 ----
    //   `inside` 映射到 (16,48)-(48,48)-(48,16)（`x=(ndc.x*0.5+0.5)*64`、`y=(-ndc.y*0.5+0.5)*64`）
    //   ⇒ 同一几何判据：(40,40) 在斜边内侧、(30,30) 在外、(0,0) 在外。
    //   ★ 两条视口下的断言**同时**绿，才证明修的是"屏幕→NDC 的换算"，
    //     而不是"挑一个让断言成立的视口"。
    LIFT_CHECK_EQ_U32(r.f.setViewport(0u, 0u, 64u, 64u), 0u);
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    drawXyzTri(r, inside, in_color);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(40, 40), in_px);        // 40.5+40.5=81 ≥ 64；x,y<48
    LIFT_CHECK_EQ_U32(r.f.pixelAt(30, 30), kClearPixel);  // 61 < 64
    LIFT_CHECK_EQ_U32(r.f.pixelAt(0, 0), kClearPixel);
    return 0;
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
//     顶点。判据（两条，缺一不可）：
//       · **正向对照**：新句柄的 Draw **真的进 GPU 批**（`D3D_OK` + `draws==1` + `queued>=1` +
//         像素真的出现）⇒ 排除"因为别的原因（没后端 / 状态不支持）被拒"的伪证；
//       · **陈旧句柄**：同一个槽（没重新 `SetStreamSource` / `SetIndices`）的 Draw 被拒
//         （`D3DERR_INVALIDCALL`）且 `draws` **不增**，**且**目标像素里没有"别人的顶点"画出来的东西。
//   ★ 为什么必须搬进 `--gpu` 臂：pure 臂**没装后端** ⇒ `gpuEligible` 的第一道闸
//     （`ctx.gpu == nullptr`）会把**任何** Draw 拒成 `kUnGpuNoBackend` ⇒ "`INVALIDCALL` + `draws==0`"
//     恒真，没有正向对照 ⇒ 陈旧句柄校验整套失效（复审报告 §2.2.1）。
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

int caseStaleStreamBinding() {
    lift_test::g_current_case = "d3d9p/stale-stream-binding";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());  // 真正的环境缺失（无 GPU 设备 / 建不出窗口）⇒ SKIP 77
    lift::ComRegistry* com = r.f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return 0;

    constexpr std::uint32_t kLen = 60u;  // 3 个 XYZRHW+DIFFUSE 顶点（20 B 一个）
    const std::uint32_t red = 0xFFFF0000u;
    const std::uint32_t blue = 0xFF0000FFu;

    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);

    // ---- ①-A 顶点缓冲：绑 → **正向对照**（新句柄真的画）→ 放自己那份 → 越界再放一次（对象死）
    //      → 新缓冲拿回同一对 EA ⇒ 槽里的代际变陈旧 ----
    std::uint32_t vb1 = 0;
    std::uint32_t vb_dummy = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(kLen, kFvfXyzRhwDiffuse, &vb1), 0u);
    LIFT_CHECK_NE_U32(vb1, 0u);
    if (vb1 == 0) return 0;
    writeRhwTriangle(vb1, 0.0f, 0.0f, red, r.f);
    const std::uint32_t vb1_state = com->userOf(vb1);
    const std::uint32_t vb1_gen = com->generationOf(vb1);
    LIFT_CHECK_NE_U32(vb1_state, 0u);
    LIFT_CHECK_NE_U32(vb1_gen, 0u);
    // 哑缓冲（同长度）：两者都释放后空洞连成一片 ⇒ 新缓冲能把对象 / vtable / 状态块**逐个**搬回原址
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(kLen, kFvfXyzRhwDiffuse, &vb_dummy), 0u);
    LIFT_CHECK_NE_U32(vb_dummy, 0u);

    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, vb1, 0u, 20u), 0u);

    // ★ **正向对照**：此刻 vb1 活着 ⇒ 这一笔必须真的进 GPU 批、真的画出像素。
    //   ⛔ 没有这一条，"陈旧句柄被拒"可能只是"没后端 / 状态不支持"的另一种说法（恒真式断言的来源）。
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawPrimitive(lift::d3d9::kPtTriangleList, 0u, 1u), 0u);
    // ★ 覆盖账在**每次 `Machine::run`（= 每次 guest COM 调用）开始时清零**（`machine.cpp:936`）
    //   ⇒ 必须在 `endScene` 之前读，否则读到的是"EndScene 那一轮"的空账，不是入队证据。
    LIFT_CHECK(lift::d3d9::gpuCoverage().queued >= 1u);  // 而且**真的入队**（⛔ 不是"准入通过"而已）
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    {
        lift::d3d9::PipelineInfo pi;
        LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr));
        LIFT_CHECK_EQ_U32(pi.draws, 1u);   // 正向那一笔真的执行了
    }
    // ★ **立刻读回**（下面就要复用 vb1 的 guest 像素块）：GPU 批在 flush 时才真的录命令/读顶点，
    //   不先 flush 的话，正向那一笔可能读到 vb2 写进同一块内存的顶点（时序假象）。
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(1, 1), 0x00FF0000u);  // 红三角真的落在目标上（不只是"入队"）

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
    // ★ 判据：同一个槽（**没有**重新 `SetStreamSource`）的 Draw **被拒** —— 返回码 + `draws` 不增 +
    //   vb2 那一片像素**没出现**（若陈旧校验被撤掉，这一笔会画 vb2 的蓝色三角形）。
    LIFT_CHECK_EQ_U32(r.f.drawPrimitive(lift::d3d9::kPtTriangleList, 0u, 1u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    {
        lift::d3d9::PipelineInfo pi;
        LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr));
        LIFT_CHECK_EQ_U32(pi.draws, 1u);  // **仍是正向那一笔**（被拒的批不增）
    }
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(41, 41), kClearPixel);  // 别人的顶点一个都没画（(41,41) 在 vb2 的三角形内）

    // ---- ①-B 索引缓冲：同一判据的第二个身份槽（`DrawIndexedPrimitive`）----
    std::uint32_t vb3 = 0;
    std::uint32_t ib1 = 0;
    std::uint32_t ib_dummy = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(kLen, kFvfXyzRhwDiffuse, &vb3), 0u);
    if (vb3 == 0) return 0;
    // ★ 画在 (40,40) 的蓝三角：陈旧索引若被放行 ⇒ 目标 (41,41) 会变蓝 ⇒ 像素判据能抓住
    //   （不能先 `Clear` 再读回：Clear 只写 guest，GPU target 的内容会在 sync 时**覆盖**回来）。
    writeRhwTriangle(vb3, 40.0f, 40.0f, blue, r.f);
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, vb3, 0u, 20u), 0u);
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(12u, 101u, &ib1), 0u);
    LIFT_CHECK_NE_U32(ib1, 0u);
    if (ib1 == 0) return 0;
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
    {
        lift::d3d9::PipelineInfo pi;
        LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr));
        LIFT_CHECK_EQ_U32(pi.draws, 1u);  // 仍只有正向那一笔（陈旧索引句柄的批不增）
    }
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(41, 41), kClearPixel);  // 修前：用 IB2 的索引画 VB3 的蓝三角
    return 0;
}

// =======================================================================================
// ⑬ Draw 读纹理槽的**身份校验**（`REQ-01M701NYF2B3YV1YFSZBMPQZ41` 段 4）
// =======================================================================================
// 缺陷形态：`prepareContext` 只按槽里的 EA 解 `'TEX1'`（只验 magic）。guest 堆复用 EA ⇒
// 纹理 A 被越界释放、同尺寸的纹理 B 拿回**同一个对象 EA + 同一个 `'TEX1'` 状态块 EA** ⇒
// 旧槽在 Draw 时"看起来完全合法"，于是**静默采样 B 的像素**（A 的那笔绘制变成 B 的内容）。
// 判据（写死一条）：同一个槽（没重新 SetTexture）的第二次 Draw 被拒（`D3DERR_INVALIDCALL`）
// 且 `draws` 不增（这次绘制被整个跳过，⛔ 不是"画了一半"）；正向那一笔**真的入队**（证明这个槽
// 参与采样，否则"不许采 B"恒真）。★ CPU 光栅已删 ⇒ 正向那一笔要真 GPU 后端（`upGpu`）。
// 告警走 `pipeWarn`（`tests/check.h` 抓不到日志 ⇒ 断言落在行为上）。
// ★ 夹具前提（EA 复用）**硬断言**，不成立就红 —— ⛔ 不是删断言；若红了先按 `runtime/heap.cpp`
//   的分块 / `min_split` 重造夹具（同 `caseStaleStreamBinding` 的口径）。
int caseStaleTextureBindingInDraw() {
    lift_test::g_current_case = "d3d9p/stale-texture-binding-in-draw";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());
    lift::ComRegistry* com = r.f.m.cpu().com;
    LIFT_CHECK(com != nullptr);
    if (com == nullptr) return 0;

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
        if (bits == 0) return;  // 纹理锁失败 ⇒ 后续断言自然红
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
    if (tex_a == 0) return 0;
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
    if (tex_b == 0) return 0;
    LIFT_CHECK_EQ_U32(tex_b, tex_a);                     // 前提 1：对象 EA 复用
    LIFT_CHECK_EQ_U32(com->userOf(tex_b), a_state);       // 前提 2：`'TEX1'` 状态块 EA 也复用 ⇒ magic 校验必过
    LIFT_CHECK_NE_U32(com->generationOf(tex_b), a_gen);   // 前提 3：身份换了
    fill2x2(tex_b, kTexelB);

    // ---- ③ 同一个槽（**没有**重新 SetTexture）再 Draw ⇒ 必须跳过，⛔ 不许采 B 的蓝色 ----
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, quad_ea, 28u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &info, nullptr));
    LIFT_CHECK_EQ_U32(info.draws, 1u);  // 这一次绘制**没有被执行**（这个槽的批整个跳过）

    // 清理：放掉 B 自己那份（到 0 ⇒ 像素块 / 状态块回收）
    LIFT_CHECK_EQ_U32(r.f.method(tex_b, kOffRelease, {}), 0u);
    LIFT_CHECK(!com->find(tex_b));
    return 0;
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

// =======================================================================================
// ⑲ `d3d9p/indexed-range`（`REQ-01M79PP9S4BFBPKDPBQBDN5GGR`）
// =======================================================================================
// 红了意味着什么：`BaseVertexIndex` 被当成 u32 / `MinVertexIndex` 被丢掉 ⇒
//   (a)(b) 负 base 的合法调用被**假拒**（INVALIDCALL，一个像素都不画）；
//   (c) 索引 < Min 的调用**静默画错**（按顶点 0/1/3 画出像素）；
//   (d) `Base + idx` 回绕成 0xFFFFFFFF ⇒ 越界读（修前可能 SIGSEGV，红了不可读）。
// ★ (d) 放在最后：修前它会先把进程打崩，前面的红证据必须已经打印。
// =======================================================================================
void writeRhwPoints(std::uint32_t vb, const std::vector<std::array<float, 4>>& pts,
                    const std::vector<std::uint32_t>& colors, Fixture& f) {
    LIFT_CHECK(pts.size() == colors.size());
    std::uint32_t base = 0;
    LIFT_CHECK_EQ_U32(f.lockBuffer(vb, &base), 0u);
    LIFT_CHECK_NE_U32(base, 0u);
    if (base == 0) return;
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const std::uint32_t at = base + static_cast<std::uint32_t>(i) * 20u;
        for (int k = 0; k < 4; ++k) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &pts[i][static_cast<std::size_t>(k)], sizeof(bits));
            f.m.mem().wr32(at + static_cast<std::uint32_t>(k) * 4u, bits);
        }
        f.m.mem().wr32(at + 16u, colors[i]);
    }
    LIFT_CHECK_EQ_U32(f.unlockBuffer(vb), 0u);
}

void writeIndex16(std::uint32_t ib, const std::vector<std::uint16_t>& indices, Fixture& f) {
    std::uint32_t base = 0;
    LIFT_CHECK_EQ_U32(f.lockBuffer(ib, &base), 0u);
    LIFT_CHECK_NE_U32(base, 0u);
    if (base == 0) return;
    for (std::size_t i = 0; i < indices.size(); ++i)
        f.m.mem().wr16(base + static_cast<std::uint32_t>(i) * 2u, indices[i]);
    LIFT_CHECK_EQ_U32(f.unlockBuffer(ib), 0u);
}

// ---- 声明路（`D3DVERTEXELEMENT9`）夹具 ----
// 8 B/项：Stream u16 / Offset u16 / Type u8 / Method u8 / Usage u8 / UsageIndex u8；`D3DDECL_END` = Stream 0xFF。
// ★ 只造本线支持集里的元素（POSITION/FLOAT3、TEXCOORD0/FLOAT2、COLOR0/D3DCOLOR，见 `declLayoutOf`）。
struct DeclElement {
    std::uint16_t stream = 0;
    std::uint16_t offset = 0;
    std::uint8_t type = 0;   // D3DDECLTYPE
    std::uint8_t usage = 0;  // D3DDECLUSAGE
};
constexpr std::uint8_t kDeclTypeFloat2 = 1;
constexpr std::uint8_t kDeclTypeFloat3 = 2;
constexpr std::uint8_t kDeclTypeD3dColor = 4;
constexpr std::uint8_t kDeclUsagePosition = 0;
constexpr std::uint8_t kDeclUsageTexCoord = 5;
constexpr std::uint8_t kDeclUsageColor = 10;

std::vector<std::uint8_t> declElementBytes(const std::vector<DeclElement>& elems) {
    std::vector<std::uint8_t> out;
    auto push16 = [&out](std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
        out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    };
    for (const DeclElement& e : elems) {
        push16(e.stream);
        push16(e.offset);
        out.push_back(e.type);
        out.push_back(0u);  // Method（缺省 = 0）
        out.push_back(e.usage);
        out.push_back(0u);  // UsageIndex
    }
    push16(0x00FFu);  // D3DDECL_END().Stream = 0xFF
    push16(0u);
    out.push_back(0u);
    out.push_back(0u);
    out.push_back(0u);
    out.push_back(0u);
    return out;
}

// CreateVertexDeclaration(+0x158) ⇒ 对象 EA（0 = 失败；失败原因由 shim 打日志）。
std::uint32_t createVertexDeclaration(Fixture& f, const std::vector<DeclElement>& elems) {
    const std::uint32_t elems_ea = f.placeBytes(declElementBytes(elems));
    const std::uint32_t out_ea = f.placeU32(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(f.method(f.device.object, kOffCreateVertexDeclaration, {elems_ea, out_ea}), 0u);
    return f.m.mem().rd32(out_ea);
}

void pushF32(std::vector<std::uint8_t>* out, float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<std::uint8_t>((bits >> (8 * i)) & 0xFFu));
}
void pushU32(std::vector<std::uint8_t>* out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out->push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
}

// 把一段字节写进缓冲（Lock → 宿主映射 → memcpy → Unlock）
void writeBufferBytes(std::uint32_t buf, const std::vector<std::uint8_t>& bytes, Fixture& f) {
    std::uint32_t base = 0;
    LIFT_CHECK_EQ_U32(f.lockBuffer(buf, &base), 0u);
    LIFT_CHECK_NE_U32(base, 0u);
    if (base == 0) return;
    std::uint8_t* dst = f.m.mem().host(base);
    LIFT_CHECK(dst != nullptr);
    if (dst != nullptr && !bytes.empty()) std::memcpy(dst, bytes.data(), bytes.size());
    LIFT_CHECK_EQ_U32(f.unlockBuffer(buf), 0u);
}

int caseIndexedRange() {
    lift_test::g_current_case = "d3d9p/indexed-range";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(r.f.setViewport(0u, 0u, 64u, 64u), 0u);

    // 5 个点：x=10/20/30/40/50，y=10；颜色互不相同（点光栅 = 1px，顶点色**精确**写进像素）。
    constexpr std::uint32_t kPoint0 = 0x00FF0000u;  // 红
    constexpr std::uint32_t kPoint1 = 0x0000FF00u;  // 绿
    constexpr std::uint32_t kPoint2 = 0x000000FFu;  // 蓝
    constexpr std::uint32_t kPoint3 = 0x00FFFFFFu;  // 白（(c) 误画时会看见）
    constexpr std::uint32_t kPoint4 = 0x00FFFF00u;  // 黄
    std::uint32_t vb = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(5u * 20u, kFvfXyzRhwDiffuse, &vb), 0u);
    LIFT_CHECK_NE_U32(vb, 0u);
    if (vb == 0) return 0;
    const std::vector<std::array<float, 4>> pts = {
        {10.5f, 10.5f, 0.0f, 1.0f}, {20.5f, 10.5f, 0.0f, 1.0f}, {30.5f, 10.5f, 0.0f, 1.0f},
        {40.5f, 10.5f, 0.0f, 1.0f}, {50.5f, 10.5f, 0.0f, 1.0f}};
    writeRhwPoints(vb, pts, {kPoint0, kPoint1, kPoint2, kPoint3, kPoint4}, r.f);
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, vb, 0u, 20u), 0u);

    std::uint32_t ib = 0;
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(12u, 101u, &ib), 0u);
    LIFT_CHECK_NE_U32(ib, 0u);
    if (ib == 0) return 0;
    LIFT_CHECK_EQ_U32(r.f.setIndices(ib), 0u);

    auto expect_points012 = [&r](bool drawn) {
        LIFT_CHECK_EQ_U32(r.f.pixelAt(10, 10), drawn ? kPoint0 : kClearPixel);
        LIFT_CHECK_EQ_U32(r.f.pixelAt(20, 10), drawn ? kPoint1 : kClearPixel);
        LIFT_CHECK_EQ_U32(r.f.pixelAt(30, 10), drawn ? kPoint2 : kClearPixel);
    };
    auto draw_case = [&](std::int32_t base, std::uint32_t min_index, std::uint32_t num_vertices,
                         const std::vector<std::uint16_t>& indices) {
        LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
        writeIndex16(ib, indices, r.f);
        return r.f.drawIndexed(lift::d3d9::kPtPointList, base, min_index, num_vertices, 0u, 3u);
    };

    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);

    // (a) base=-2, min=2, n=3, IB{2,3,4} ⇒ 顶点 0/1/2。修前：idx=4 >= n ⇒ 假 INVALIDCALL。
    LIFT_CHECK_EQ_U32(draw_case(-2, 2u, 3u, {2u, 3u, 4u}), 0u);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    // ★ 点图元真的落成像素（少画一类图元 = 用户看得见）；位置 = floor(10.5) 那一格。
    LIFT_CHECK_EQ_U32(r.f.pixelAt(10, 10), kPoint0);

    // (b) base=-1, min=1, n=3, IB{1,2,3} ⇒ 顶点 0/1/2。修前：idx=3 >= n ⇒ 假 INVALIDCALL。
    LIFT_CHECK_EQ_U32(draw_case(-1, 1u, 3u, {1u, 2u, 3u}), 0u);

    // (c) base=0, min=2, n=4, IB{0,1,3} ⇒ idx=0/1 < min ⇒ INVALIDCALL（⛔ 不按 0/1/3 画）。
    //     修前：S_OK 且像素 10/20/40 被画出来（静默画错）。
    LIFT_CHECK_EQ_U32(draw_case(0, 2u, 4u, {0u, 1u, 3u}), lift::d3d9::kInvalidCall);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    // ★ 违约索引对应的顶点（顶点 3 的白点，x=40）**不许**被画出来（错位杂点 = 用户可见）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(40, 10), kClearPixel);

    // (d) base=-1, min=0, n=1, IB{0,1,2} ⇒ 首个索引 0 在 [0,1) 内，但 vi=-1 ⇒ INVALIDCALL。
    //     修前：`vertex_index+1u` 回绕成 0 ⇒ 越过检查，随即按 0xFFFFFFFF*stride 做指针加（越界读）。
    LIFT_CHECK_EQ_U32(draw_case(-1, 0u, 1u, {0u, 1u, 2u}), lift::d3d9::kInvalidCall);

    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    return 0;
}

// =======================================================================================
// ⑲b `d3d9p/indexed-range-decl`（`REQ-01M7A3M7E59JX74G897Q7W2RVD` 的第 1 条判据）
// =======================================================================================
// 红了意味着什么：声明路（`kPlVertexDecl` + 两条 stream）的顶点区间映射与 FVF 路**不一致** ——
//   (a)(b) 负 `BaseVertexIndex` 的合法调用被假拒（INVALIDCALL，一个像素都不画）或映射错位
//     （点画到别的 x 上）；
//   (c) 索引 < `MinVertexIndex` 的调用**静默画错**（顶点 3 的白点被画出来）。
//
// ★ 声明：stream0 = POSITION FLOAT3 @0 + COLOR0 D3DCOLOR @12（stride 16）·
//          stream1 = TEXCOORD0 FLOAT2 @0（stride 8）⇒ 走的就是 Live2D 那套
//          `declLayoutOf` / `buildDeclVertices` + `relative = idx - min` 映射（没绑纹理，UV 只为凑第二条流）。
// ★ 每段 `NumVertices` = 5（FVF 路是 3）：声明缓冲按 `[first, first+NumVertices)` 建，
//   "变异点 `relative` 不减 min" 因此读到的是**已建出来的**顶点 2/3/4（蓝/白/黄，x=30/40/50），
//   ⛔ 不越界读 ⇒ 变异红得可复现（不是悬空的 UB）。
// =======================================================================================
int caseIndexedRangeDecl() {
    lift_test::g_current_case = "d3d9p/indexed-range-decl";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setViewport(0u, 0u, 64u, 64u), 0u);

    // 5 个点：x = 10/20/30/40/50 的**像素中心**（NDC：x = (px+0.5)/32 - 1；y = 1 - (10.5/32)）。
    // 颜色互不相同（点光栅 = 1px，顶点色**精确**写进像素）。
    constexpr std::uint32_t kPoint0 = 0x00FF0000u;  // 红
    constexpr std::uint32_t kPoint1 = 0x0000FF00u;  // 绿
    constexpr std::uint32_t kPoint2 = 0x000000FFu;  // 蓝
    constexpr std::uint32_t kPoint3 = 0x00FFFFFFu;  // 白（(c) 误画时会看见）
    constexpr std::uint32_t kPoint4 = 0x00FFFF00u;  // 黄
    constexpr std::uint32_t kVertices = 7u;         // 前 5 个有数据；(c) 要读到顶点 5 也不越界

    const std::uint32_t decl = createVertexDeclaration(
        r.f, {{0u, 0u, kDeclTypeFloat3, kDeclUsagePosition},
              {0u, 12u, kDeclTypeD3dColor, kDeclUsageColor},
              {1u, 0u, kDeclTypeFloat2, kDeclUsageTexCoord}});
    LIFT_CHECK_NE_U32(decl, 0u);
    if (decl == 0) return 0;
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetVertexDeclaration, {decl}), 0u);

    std::uint32_t vb0 = 0;
    std::uint32_t vb1 = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(kVertices * 16u, 0u, &vb0), 0u);
    LIFT_CHECK_NE_U32(vb0, 0u);
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(kVertices * 8u, 0u, &vb1), 0u);
    LIFT_CHECK_NE_U32(vb1, 0u);
    if (vb0 == 0 || vb1 == 0) return 0;

    const std::vector<float> xs = {-0.671875f, -0.359375f, -0.046875f, 0.265625f, 0.578125f};
    const std::vector<std::uint32_t> colors = {kPoint0, kPoint1, kPoint2, kPoint3, kPoint4};
    std::vector<std::uint8_t> stream0;
    std::vector<std::uint8_t> stream1;
    for (std::uint32_t i = 0; i < kVertices; ++i) {
        const float px = i < xs.size() ? xs[i] : 0.0f;
        const std::uint32_t color = i < colors.size() ? colors[i] : 0u;
        pushF32(&stream0, px);   // x
        pushF32(&stream0, 0.671875f);  // y（屏幕 y=10 那一行的像素中心）
        pushF32(&stream0, 0.0f);       // z
        pushU32(&stream0, color);      // COLOR0
        pushF32(&stream1, 0.0f);       // u
        pushF32(&stream1, 0.0f);       // v
    }
    writeBufferBytes(vb0, stream0, r.f);
    writeBufferBytes(vb1, stream1, r.f);
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, vb0, 0u, 16u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(1u, vb1, 0u, 8u), 0u);

    std::uint32_t ib = 0;
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(12u, 101u, &ib), 0u);
    LIFT_CHECK_NE_U32(ib, 0u);
    if (ib == 0) return 0;
    LIFT_CHECK_EQ_U32(r.f.setIndices(ib), 0u);

    auto draw_case = [&](std::int32_t base, std::uint32_t min_index, std::uint32_t num_vertices,
                         const std::vector<std::uint16_t>& indices) {
        LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
        writeIndex16(ib, indices, r.f);
        return r.f.drawIndexed(lift::d3d9::kPtPointList, base, min_index, num_vertices, 0u, 3u);
    };

    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);

    // (a) base=-2, min=2, n=5, IB{2,3,4} ⇒ 顶点 0/1/2 的红/绿/蓝点。
    //     修前（`relative` 不减 min）：读到顶点 2/3/4 ⇒ 像素落在 30/40/50（或越界）⇒ 下面三条红。
    LIFT_CHECK_EQ_U32(draw_case(-2, 2u, 5u, {2u, 3u, 4u}), 0u);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(10, 10), kPoint0);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(20, 10), kPoint1);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(30, 10), kPoint2);

    // (b) base=-1, min=1, n=5, IB{1,2,3} ⇒ 同样是顶点 0/1/2（first 仍为 0）。
    LIFT_CHECK_EQ_U32(draw_case(-1, 1u, 5u, {1u, 2u, 3u}), 0u);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(10, 10), kPoint0);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(20, 10), kPoint1);
    LIFT_CHECK_EQ_U32(r.f.pixelAt(30, 10), kPoint2);

    // (c) base=0, min=2, n=4, IB{0,1,3} ⇒ idx=0/1 < min ⇒ INVALIDCALL（⛔ 不按 0/1/3 画）。
    LIFT_CHECK_EQ_U32(draw_case(0, 2u, 4u, {0u, 1u, 3u}), lift::d3d9::kInvalidCall);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    // 违约索引对应的顶点（顶点 3 的白点，x=40）**不许**被画出来（错位杂点 = 用户可见）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(40, 10), kClearPixel);

    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    return 0;
}

// =======================================================================================
// ⑲c `d3d9p/indexed-range-start-wrap`（`REQ-01M7A3M7E59JX74G897Q7W2RVD` 的第 2 条判据）
// =======================================================================================
// 红了意味着什么：`start_index + index_count` 退回 **u32 加法** ⇒ `0xFFFFFFF0 + 0x20` 回绕成 `0x10`，
//   恰好 **≤** 16 个索引的缓冲（`16 > 16` 为假）⇒ 越过上界检查，随后按 `0xFFFFFFF0 * 2 ≈ 8.6 GB`
//   的偏移读索引 ⇒ **越界读**（修前实测：进程崩在那次 memcpy 上，⛔ 不是 INVALIDCALL）。
// ★ 正控（同一条用例内）：同一批缓冲、`start_index=0` 的 Draw 必须 S_OK 且真的画出点 ——
//   否则"INVALIDCALL"会因为"pure 臂里任何 Draw 都 INVALIDCALL"而假绿 ⇒ 这条必须在 GPU 臂。
// =======================================================================================
int caseIndexedRangeStartWrap() {
    lift_test::g_current_case = "d3d9p/indexed-range-start-wrap";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(r.f.setViewport(0u, 0u, 64u, 64u), 0u);

    constexpr std::uint32_t kPoint0 = 0x00FF0000u;
    std::uint32_t vb = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(5u * 20u, kFvfXyzRhwDiffuse, &vb), 0u);
    LIFT_CHECK_NE_U32(vb, 0u);
    if (vb == 0) return 0;
    const std::vector<std::array<float, 4>> pts = {
        {10.5f, 10.5f, 0.0f, 1.0f}, {20.5f, 10.5f, 0.0f, 1.0f}, {30.5f, 10.5f, 0.0f, 1.0f},
        {40.5f, 10.5f, 0.0f, 1.0f}, {50.5f, 10.5f, 0.0f, 1.0f}};
    writeRhwPoints(vb, pts, {kPoint0, kPoint0, kPoint0, kPoint0, kPoint0}, r.f);
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, vb, 0u, 20u), 0u);

    // 16 个索引（32 B）—— 上界 = 16。"回绕后恰好等于上界"才是旧加法真的**越过**检查的那一格：
    //   0xFFFFFFF0 + 0x20 = 0x10（u32）= 16 ⇒ `16 > 16` 为假。
    std::uint32_t ib = 0;
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(32u, 101u, &ib), 0u);
    LIFT_CHECK_NE_U32(ib, 0u);
    if (ib == 0) return 0;
    std::vector<std::uint16_t> indices(16u, 0u);
    indices[0] = 0u;
    indices[1] = 1u;
    indices[2] = 2u;
    writeIndex16(ib, indices, r.f);
    LIFT_CHECK_EQ_U32(r.f.setIndices(ib), 0u);

    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);

    // ★ 正控：start_index=0、同一批缓冲 ⇒ S_OK 且真的画出第一个点（LIFT_SKIP 里的 syncPixel 之后断言）
    LIFT_CHECK_EQ_U32(r.f.drawIndexed(lift::d3d9::kPtPointList, 0, 0u, 5u, 0u, 3u), 0u);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(10, 10), kPoint0);

    // 越界：`start_index + index_count` = 0xFFFFFFF0 + 0x20（int64 = 4294967312 > 16）⇒ INVALIDCALL。
    // 修前（u32 回绕）这条会走进去读 8.6 GB 之外的索引 ⇒ 进程崩（红）。
    LIFT_CHECK_EQ_U32(r.f.drawIndexed(lift::d3d9::kPtPointList, 0, 0u, 5u, 0xFFFFFFF0u, 0x20u),
                      lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    return 0;
}

// 后缓冲里"非清屏色"的像素个数（粗粒度：证明这一批 draw 真的写了像素，⛔ 不是只看返回码）。
int countNonBackground(Fixture& f, std::uint32_t background) {
    int n = 0;
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            if (f.pixelAt(x, y) != background) ++n;
    return n;
}

// =======================================================================================
// ⑲d `d3d9p/vertex-decl-draw`（`REQ-01M79P02VYRJ3V0C0V5VHXVE3H`）
// =======================================================================================
// 红了意味着什么（声明式 Draw = Live2D 那条路，一旦回归表现是"模型整层消失"）：
//   ① `DrawIndexedPrimitive` 返回 `D3DERR_INVALIDCALL`（`drawPrecheck` 把 `kPlVertexDecl != 0`
//     当成"这一条 Draw 不支持声明"的旧行为）⇒ 后缓冲一个像素都没有 ⇒ `painted >= 4000` 红；
//   ② 顶点取错 / 流布局错（`buildDeclVertices` 的 stream1 偏移差 1 字节 = UV 采样错）⇒
//      四角 texel 的颜色对不上（红/绿/蓝/白各错位）⇒ 四条颜色断言红；
//   ③ 声明没真的绑上（FVF 路偷偷接住）⇒ `vertex_decl == 0` 红。
// ★ 判据里没有 `fallback_draws`：CPU 光栅整族已删（`REQ-01M7A74YG08R65NJBFXF7DEJDK`）⇒
//   "像素出现"本身就等价于"GPU 准入层收下了这一批"，不存在 CPU 回退把用例刷绿的可能。
// =======================================================================================
int caseVertexDeclDraw() {
    lift_test::g_current_case = "d3d9p/vertex-decl-draw";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);

    // 2×2 纹理（A8R8G8B8）：四角四色 ⇒ 采样错会**换颜色**（不是"少画几个像素"）
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 2u, 2u, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return 0;
    const std::uint32_t locked = r.f.placeBytes(std::vector<std::uint8_t>(8u, 0u));
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
    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, tex), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampAddressU, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampAddressV, lift::d3d9::kAddressClamp), 0u);

    // 声明：stream0 = POSITION FLOAT3 @0 + COLOR0 D3DCOLOR @12（stride 16）·
    //       stream1 = TEXCOORD0 FLOAT2 @0（stride 8）—— 与 Live2D 同形（两条流、UV 在第二条流上）
    const std::uint32_t decl = createVertexDeclaration(
        r.f, {{0u, 0u, kDeclTypeFloat3, kDeclUsagePosition},
              {0u, 12u, kDeclTypeD3dColor, kDeclUsageColor},
              {1u, 0u, kDeclTypeFloat2, kDeclUsageTexCoord}});
    LIFT_CHECK_NE_U32(decl, 0u);
    if (decl == 0) return 0;
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetVertexDeclaration, {decl}), 0u);

    // 全幅 quad（两个三角形）。NDC(-1,+1) = 屏幕 (0,0)（上左）⇒ 该顶点 UV = (0,0)。
    const std::uint32_t kWhite = 0xFFFFFFFFu;
    const std::vector<std::array<float, 3>> pos = {{-1.0f, 1.0f, 0.0f},
                                                   {1.0f, 1.0f, 0.0f},
                                                   {-1.0f, -1.0f, 0.0f},
                                                   {1.0f, -1.0f, 0.0f}};
    const std::vector<std::array<float, 2>> uv = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    std::vector<std::uint8_t> stream0;
    std::vector<std::uint8_t> stream1;
    for (std::size_t i = 0; i < pos.size(); ++i) {
        pushF32(&stream0, pos[i][0]);
        pushF32(&stream0, pos[i][1]);
        pushF32(&stream0, pos[i][2]);
        pushU32(&stream0, kWhite);
        pushF32(&stream1, uv[i][0]);
        pushF32(&stream1, uv[i][1]);
    }
    std::uint32_t vb0 = 0;
    std::uint32_t vb1 = 0;
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(4u * 16u, 0u, &vb0), 0u);
    LIFT_CHECK_EQ_U32(r.f.createVertexBuffer(4u * 8u, 0u, &vb1), 0u);
    LIFT_CHECK_NE_U32(vb0, 0u);
    LIFT_CHECK_NE_U32(vb1, 0u);
    if (vb0 == 0 || vb1 == 0) return 0;
    writeBufferBytes(vb0, stream0, r.f);
    writeBufferBytes(vb1, stream1, r.f);
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(0u, vb0, 0u, 16u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setStreamSource(1u, vb1, 0u, 8u), 0u);

    std::uint32_t ib = 0;
    LIFT_CHECK_EQ_U32(r.f.createIndexBuffer(12u, 101u, &ib), 0u);
    LIFT_CHECK_NE_U32(ib, 0u);
    if (ib == 0) return 0;
    writeIndex16(ib, {0u, 1u, 2u, 1u, 3u, 2u}, r.f);
    LIFT_CHECK_EQ_U32(r.f.setIndices(ib), 0u);

    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawIndexed(lift::d3d9::kPtTriangleList, 0, 0u, 4u, 0u, 2u), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());

    // ① 真的画了像素（整幅 quad ⇒ 几乎全部非清屏色）
    const int painted = countNonBackground(r.f, kClearPixel);
    LIFT_CHECK(painted >= 4000);

    // ② 四角 = 纹理四个角 texel（UV 来自 stream1；偏移错 1 字节 ⇒ 颜色当场换掉）
    LIFT_CHECK_EQ_U32(r.f.pixelAt(2, 2), 0x00FF0000u);    // (0,0) 红
    LIFT_CHECK_EQ_U32(r.f.pixelAt(61, 2), 0x0000FF00u);   // (1,0) 绿
    LIFT_CHECK_EQ_U32(r.f.pixelAt(2, 61), 0x000000FFu);   // (0,1) 蓝
    LIFT_CHECK_EQ_U32(r.f.pixelAt(61, 61), 0x00FFFFFFu);  // (1,1) 白

    // ③ 声明真的绑在设备上（这一批不是被 FVF 路接住的）
    lift::d3d9::PipelineInfo pi;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr));
    LIFT_CHECK_NE_U32(pi.vertex_decl, 0u);
    LIFT_CHECK_NE_U32(pi.draws, 0u);
    return 0;
}

// =======================================================================================
// ⑲e `d3d9p/set-render-target-offscreen-refused`（`REQ-01M79WE8JA7V92RNS537CJB0V6`）
// =======================================================================================
// 红了意味着什么：`CreateOffscreenPlainSurface` 的 surface 被**静默**绑成 RT0 ——
//   ① `SetRenderTarget` 不再 `D3DERR_INVALIDCALL`；② 流水线的渲染目标变成那块离屏表面
//   （`color_surface / color_arena` 不再指后缓冲）；③ 随后一批 draw 落进离屏表面、后缓冲一个像素都没有
//   （或离屏表面被写坏）—— 而它正是 `GetRenderTargetData` 的**读回目标**，坏了不可见。
// ★ 反向断言（同一用例内，防过度收紧）：**纹理** level 0 仍可绑成 RT（否则"拒绝"退化成"什么都绑不上"）。
// ★ 修前绿是正常的：今天这条拒绝是 `setRenderTarget` 的 ownerless 判据的**副产物**
//   （离屏表面有 `'TEX1'` 状态块、但建 surface 时 owner 传 0）⇒ 本用例守的是**将来**
//   （有人放宽 ownerless 绑定 / 删掉那条判据时，GPU 准入层已经不再有 `color_is_texture` 闸）。
// =======================================================================================
int caseSetRenderTargetOffscreenRefused() {
    lift_test::g_current_case = "d3d9p/set-render-target-offscreen-refused";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());
    auto& mem = r.f.m.mem();

    // ① 建**离屏 plain surface**（`CreateOffscreenPlainSurface`，池 = SYSTEMMEM = 2）
    const std::uint32_t out_ea = r.f.placeU32(0u);
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffCreateOffscreenPlainSurface,
                                 {64u, 64u, lift::d3d9::kFmtX8R8G8B8, 2u, out_ea, 0u}),
                      0u);
    const std::uint32_t off_surf = mem.rd32(out_ea);
    LIFT_CHECK_NE_U32(off_surf, 0u);
    if (off_surf == 0) return 0;

    // 像素预填哨兵（走 guest 的 LockRect / UnlockRect —— 与 GetRenderTargetData 的读回目标同一条路）
    constexpr std::uint32_t kSentinel = 0x5A5A5A5Au;
    const std::uint32_t locked = r.f.placeBytes(std::vector<std::uint8_t>(8u, 0u));
    LIFT_CHECK_EQ_U32(r.f.method(off_surf, kOffSurfaceLockRect, {locked, 0u, 0u}), 0u);
    const std::uint32_t off_pitch = mem.rd32(locked + 0u);
    const std::uint32_t off_bits = mem.rd32(locked + 4u);
    LIFT_CHECK_NE_U32(off_bits, 0u);
    if (off_bits == 0) return 0;
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            mem.wr32(off_bits + static_cast<std::uint32_t>(y) * off_pitch + static_cast<std::uint32_t>(x) * 4u,
                     kSentinel);
        }
    }
    LIFT_CHECK_EQ_U32(r.f.method(off_surf, kOffSurfaceUnlockRect, {}), 0u);
    // 哨兵真的落在像素里（否则"没被写"会退化成"读的是空内存"）
    LIFT_CHECK_EQ_U32(mem.rd32(off_bits), kSentinel);

    auto surfaceBytes = [&]() {
        std::vector<std::uint8_t> out;
        for (int y = 0; y < 64; ++y) {
            for (int x = 0; x < 64; ++x) {
                const std::uint32_t at = off_bits + static_cast<std::uint32_t>(y) * off_pitch +
                                         static_cast<std::uint32_t>(x) * 4u;
                for (int b = 0; b < 4; ++b) out.push_back(mem.rd8(at + static_cast<std::uint32_t>(b)));
            }
        }
        return out;
    };

    // ② 绑到 RT0 ⇒ D3DERR_INVALIDCALL（D3D9：离屏 plain surface 不能当 render target）
    LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, off_surf}),
                      lift::d3d9::kInvalidCall);

    // ③ 渲染目标**不变**：仍是（隐式）后缓冲
    lift::d3d9::PipelineInfo pi;
    LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr));
    LIFT_CHECK_EQ_U32(pi.color_surface, 0u);
    LIFT_CHECK_EQ_U32(pi.color_arena, 0u);

    // ④ 随后一批 draw 仍进后缓冲：清 → 画一个点 → 读回后缓冲 / 离屏表面逐字节比对
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    const std::vector<std::uint8_t> off_before = surfaceBytes();
    constexpr std::uint32_t kDot = 0x00FF8000u;
    const std::uint32_t dot = r.f.placeRhwDiffuse({{30.5f, 40.5f, 0.0f, 1.0f}}, {kDot});
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtPointList, 1u, dot, 20u), 0u);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    if (!r.syncPixel(&why)) LIFT_SKIP(why.c_str());
    LIFT_CHECK_EQ_U32(r.f.pixelAt(30, 40), kDot);       // 后缓冲真的被画了
    LIFT_CHECK_EQ_U32(r.f.pixelAt(10, 10), kClearPixel);  // 只有那一个点
    LIFT_CHECK(surfaceBytes() == off_before);           // 离屏表面一个字节都没动

    // ⑤ 反向断言（防过度收紧）：**纹理** level 0 仍可绑成 RT
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, 64u, 64u, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex != 0) {
        const std::uint32_t ts_out = r.f.placeU32(0u);
        LIFT_CHECK_EQ_U32(r.f.method(tex, kOffTextureGetSurfaceLevel, {0u, ts_out}), 0u);
        const std::uint32_t ts = mem.rd32(ts_out);
        LIFT_CHECK_NE_U32(ts, 0u);
        if (ts != 0) {
            LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, ts}), 0u);
            LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr));
            LIFT_CHECK_EQ_U32(pi.color_surface, ts);
            LIFT_CHECK_NE_U32(pi.color_arena, 0u);
            // 回到后缓冲（设备销毁时不留绑定）
            LIFT_CHECK_EQ_U32(r.f.method(r.f.device.object, kOffSetRenderTarget, {0u, 0u}), 0u);
        }
    }
    return 0;
}

// =======================================================================================
// ⑳ `d3d9p/render-state-policy`（`REQ-01M79PPA10JCKG4C9HY6QMV6PT`）
// =======================================================================================
// 红了意味着什么：
//   (1) 引擎实测的 16 个渲染状态 / 取值（AGE.EXE + AGERC.DLL 的 [reg+0E4h] 普查）有一个被拒 ⇒
//       白名单漏项（或默认分支又把未知状态静默存下）；
//   (2) B 档样本（FOGCOLOR）必须 S_OK + 可读回（⛔ 把"只存"也拒掉）；
//   (3) 210..255 / 1 / 138 不是 D3DRS ⇒ INVALIDCALL 且**不写**（整块 guest 堆逐字节不变）。
//
// ★ 已删的判据（`[合同]` 只剩上面三条）：C 档（值敏感）的"有效值 ⇒ 拒绝 + 记 bit26"、
//   benign（bit11 MIPFILTER = 已知近似）集合、`unsupported : 0x…` / `bit26=未分类 D3DRS` 摘要 ——
//   它们都是**我方政策 / 我方记账位 / 我方摘要文案**，外部不可观测（`AGENTS.md` §10）。
//   非 D3DRS 的拒绝面（下面 (3)）就是这一族留下的代表。
// ★ 修 2026-11-14（`REQ-01M7A3M73R3XYMM7J4KNQ1VNV7`）：记账位（bit26 / bit41）的"命中"判据
//   **另有**用例守 —— `d3d9p/render-state-domain` 的"显式拒绝档 / B 档"两条，那是需求单明确要求的
//   **接受域**判据（不是本用例的合同）。本用例只管上面三条。摘要文案仍然没人断言。
// =======================================================================================
// 引擎实测的 16 个 D3DRS 及其实测取值（`[reg+0E4h]` 普查；`REQ-01M79PPA10JCKG4C9HY6QMV6PT` §4）。
// ★ 逐条必须是官方 `D3DRENDERSTATETYPE` 枚举里有的值（1/138 不是：1 是扫描伪值、138 不存在，
//   ⛔ 不许因为"引擎好像设过"就收进白名单）。
struct EngineRenderState {
    std::uint32_t state;
    std::vector<std::uint32_t> values;
};
const std::vector<EngineRenderState>& engineRenderStates() {
    static const std::vector<EngineRenderState> kStates = {
        {7u, {0u, 1u}},
        {14u, {0u, 1u}},
        {15u, {0u}},
        {19u, {2u, 5u, 9u}},
        {20u, {1u, 2u, 6u}},
        {22u, {3u}},
        {27u, {1u}},
        {137u, {1u}},
        {139u, {0xFFFFFFFFu}},
        {145u, {0u}},
        {151u, {0u, 0x100u}},
        {167u, {0u, 1u}},
        {171u, {1u, 3u}},
        {206u, {1u}},
        {207u, {2u}},
        {208u, {2u, 6u}},
    };
    return kStates;
}

void caseRenderStatePolicy() {
    lift_test::g_current_case = "d3d9p/render-state-policy";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;

    // ---- (1) 引擎实测的 16 个状态 + 取值逐个 Set：全部 S_OK ----
    const std::vector<EngineRenderState>& engine = engineRenderStates();
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(engine.size()), 16u);
    for (const EngineRenderState& s : engine)
        for (std::uint32_t v : s.values) LIFT_CHECK_EQ_U32(r.f.setRs(s.state, v), lift::d3d9::kOk);

    // ---- (2) B 档样本：FOGCOLOR 合法且必须"只存"（S_OK + 可读回）----
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsFogColor, 0xFF123456u), lift::d3d9::kOk);
    LIFT_CHECK_EQ_U32(r.f.getRs(lift::d3d9::kRsFogColor), 0xFF123456u);

    // ---- (3) 210..255 / 1 / 138 / 2 不是 D3DRS：INVALIDCALL + 不写 ----
    //   ★ 两条分支都要覆盖：210..255 走上界；2 这种 0..209 的空洞走 `default`。
    //     （只测 214 会漏掉"default 又静默存下"——上界先把它挡了。）
    auto heapBytes = [&r]() {
        const std::uint32_t base = r.f.m.layout().heap_base;
        std::vector<std::uint8_t> out(kFixtureHeapBytes, 0u);
        for (std::uint32_t i = 0; i < kFixtureHeapBytes; ++i) out[i] = r.f.m.mem().rd8(base + i);
        return out;
    };
    const std::vector<std::uint8_t> heap_before = heapBytes();
    LIFT_CHECK_EQ_U32(r.f.setRs(214u, 1u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.setRs(210u, 1u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.setRs(255u, 1u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.setRs(2u, 1u), lift::d3d9::kInvalidCall);
    // ★ **白名单不许收非 D3DRS 值**：1 与 138 都不在官方 `D3DRENDERSTATETYPE` 里
    //   （枚举从 ZENABLE=7 起，138 这一段也不存在）。1 是扫描伪值 ——
    //   `.lst 0x4C1ED2 push 3 / 0x4C1ED6 push 1 / 0x4C1ED8 push 16h` 实际是
    //   state `0x16`(=22)、值 `1|3`，不是 state=1。它们不是"未分类的 D3DRS"，
    //   而是根本不在 D3D9 枚举里（与 2 / 214 同类）。改回 `case 1`/`case 138` ⇒ 这两条红。
    LIFT_CHECK_EQ_U32(r.f.setRs(1u, 3u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.setRs(1u, 0u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.setRs(138u, 1u), lift::d3d9::kInvalidCall);
    LIFT_CHECK(heap_before == heapBytes());
    // Get 与 Set **同域**（`REQ-01M7A3M73R3XYMM7J4KNQ1VNV7`）：210+ 与 0..209 的空洞都读不到
    //   （也⛔ 不是"存了但读不到"的假成功）。
    //   ⚠ 改判据：修前这里钉的是 `Get(2) == 0`（"按数组下标直接读、任何 0..209 都回 0"）——
    //   那条**没有文档依据**：`State` 文档写"can be any member of the D3DRENDERSTATETYPE
    //   enumerated type"、返回值"D3DERR_INVALIDCALL if one of the arguments is invalid"
    //   <https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-getrenderstate>
    //   ⇒ 2 不是枚举成员 ⇒ 与 Set 同判 INVALIDCALL（理由写在交付报告里）。
    LIFT_CHECK_EQ_U32(r.f.getRs(214u), 0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(r.f.getRs(2u), 0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(r.f.getRs(1u), 0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(r.f.getRs(138u), 0xDEADBEEFu);
}

// =======================================================================================
// ㉑ `d3d9p/render-state-domain`（`REQ-01M7A3M73R3XYMM7J4KNQ1VNV7`）
//
// 红了意味着什么（= 接受域不再等于"官方 `D3DRENDERSTATETYPE` 枚举成员"）：
//   (1) 某个**合法 D3DRS** 被 `Set` 拒（白名单漏项 / 又把"本线没实现"当成"非法"）；
//   (2) 某个**非 D3DRS** 值被 `Set` 接受（default 又静默存下）或给 `Set` 记了账；
//   (3) `Get` 的接受域与 `Set` 不一致（0..209 的空洞 / 越界值能读回，或合法值读不到）；
//   (4) B 档（合法、本线不实现 ⇒ 应当"只存 + 记账"）不再进 `recordUnsupported` 的具名位 41；
//   (5) C 档（值敏感）的"有效值 ⇒ INVALIDCALL + 记 bit26"退化。
//
// 逐值依据（⛔ 不是"本线收了多少个"）：
//   · 官方枚举（Syntax 段是**全集**；7..209 **不是**连续区间，`D3DRS_FORCE_DWORD` 不是状态）：
//     <https://learn.microsoft.com/en-us/windows/win32/direct3d9/d3drenderstatetype>
//   · `GetRenderState` 的 `State`："can be any member of the D3DRENDERSTATETYPE enumerated type"、
//     返回值："D3DERR_INVALIDCALL if one of the arguments is invalid" ⇒ `Set` / `Get` **同域**：
//     <https://learn.microsoft.com/en-us/windows/win32/api/d3d9/nf-d3d9-idirect3ddevice9-getrenderstate>
//
// ⛔ 本用例**不**写"209 个值里只有 X 个"这类取值断言（换实现就红、红得没意义）：
//   它只断两条**全域**命题 —— 每个枚举成员 ⇒ 接受（或显式拒绝并记账）；每个非成员 ⇒ 拒绝且不记账。
// =======================================================================================
enum class RsTier {
    Implemented,          // A 档：有 case / 有读点
    Stored,               // B 档（既有）：合法、只存（副作用被已记账的 master 门控，或真机不读）
    StoredUnimplemented,  // B 档（本单新增）：合法、只存 **+** `recordUnsupported(bit41)` 记账
    ValueSensitive,       // C 档：合法但值敏感（有效值 ⇒ INVALIDCALL + bit26）
};
struct RsDomain {
    std::uint32_t state;
    RsTier tier;
    std::uint32_t value;  // 用来 Set 的值（C 档给一个"有效值"，用来验显式拒绝）
};
constexpr std::uint32_t kStoredRsValue = 0x5A5A5A5Au;  // ⛔ 不等于 `getRs` 的失败哨兵 0xDEADBEEF

// ★ 这张表**就是**判据（逐值对照上面的官方枚举页）：表里没写的 0..255 值一律当"非 D3DRS"。
const std::vector<RsDomain>& d3dRsDomain() {
    static const std::vector<RsDomain> kDomain = {
        // ---- A 档：有读点 / 有 case（取值必须是该状态**合法**的值，否则红在本线自己的值域上）----
        {7u, RsTier::Implemented, 1u},           // ZENABLE = D3DZB_TRUE
        {8u, RsTier::Implemented, 3u},           // FILLMODE = D3DFILL_SOLID
        {9u, RsTier::Implemented, 2u},           // SHADEMODE = D3DSHADE_GOURAUD
        {14u, RsTier::Implemented, 1u},          // ZWRITEENABLE
        {15u, RsTier::Implemented, 0u},          // ALPHATESTENABLE
        {16u, RsTier::Implemented, 1u},          // LASTPIXEL
        {19u, RsTier::Implemented, 2u},          // SRCBLEND = D3DBLEND_ONE
        {20u, RsTier::Implemented, 1u},          // DESTBLEND = D3DBLEND_ZERO
        {22u, RsTier::Implemented, 3u},          // CULLMODE = D3DCULL_CCW
        {23u, RsTier::Implemented, 4u},          // ZFUNC = D3DCMP_LESSEQUAL
        {24u, RsTier::Implemented, 0x7Fu},       // ALPHAREF（文档：低 8 位）
        {25u, RsTier::Implemented, 8u},          // ALPHAFUNC = D3DCMP_ALWAYS
        {26u, RsTier::Implemented, 0u},          // DITHERENABLE
        {27u, RsTier::Implemented, 1u},          // ALPHABLENDENABLE
        {28u, RsTier::Implemented, 0u},          // FOGENABLE
        {29u, RsTier::Implemented, 0u},          // SPECULARENABLE
        {52u, RsTier::Implemented, 0u},          // STENCILENABLE（case 里有）
        {60u, RsTier::Implemented, 0xFFFFFFFFu}, // TEXTUREFACTOR
        {136u, RsTier::Implemented, 1u},         // CLIPPING
        {137u, RsTier::Implemented, 1u},         // LIGHTING
        {139u, RsTier::Implemented, 0u},         // AMBIENT
        {141u, RsTier::Implemented, 0u},         // COLORVERTEX
        {145u, RsTier::Implemented, 0u},         // DIFFUSEMATERIALSOURCE
        {146u, RsTier::Implemented, 0u},         // SPECULARMATERIALSOURCE（本单开始真的读）
        {147u, RsTier::Implemented, 0u},         // AMBIENTMATERIALSOURCE
        {148u, RsTier::Implemented, 0u},         // EMISSIVEMATERIALSOURCE
        {151u, RsTier::Implemented, 0u},         // VERTEXBLEND
        {152u, RsTier::Implemented, 0u},         // CLIPPLANEENABLE
        {154u, RsTier::Implemented, 0x3F800000u},  // POINTSIZE = 1.0f
        {155u, RsTier::Implemented, 0x3F800000u},  // POINTSIZE_MIN = 1.0f
        {156u, RsTier::Implemented, 0u},         // POINTSPRITEENABLE
        {157u, RsTier::Implemented, 0u},         // POINTSCALEENABLE
        {158u, RsTier::Implemented, 0x3F800000u},  // POINTSCALE_A
        {159u, RsTier::Implemented, 0u},         // POINTSCALE_B
        {160u, RsTier::Implemented, 0u},         // POINTSCALE_C
        {161u, RsTier::Implemented, 0u},         // MULTISAMPLEANTIALIAS
        {162u, RsTier::Implemented, 0xFFFFFFFFu}, // MULTISAMPLEMASK
        {166u, RsTier::Implemented, 0x42800000u},  // POINTSIZE_MAX = 64.0f
        {167u, RsTier::Implemented, 0u},         // INDEXEDVERTEXBLENDENABLE
        {168u, RsTier::Implemented, 0xFu},       // COLORWRITEENABLE
        {170u, RsTier::Implemented, 0u},         // TWEENFACTOR = 0.0f
        {171u, RsTier::Implemented, 1u},         // BLENDOP = D3DBLENDOP_ADD
        {174u, RsTier::Implemented, 0u},         // SCISSORTESTENABLE
        {206u, RsTier::Implemented, 1u},         // SEPARATEALPHABLENDENABLE
        {207u, RsTier::Implemented, 2u},         // SRCBLENDALPHA = D3DBLEND_ONE
        {208u, RsTier::Implemented, 1u},         // DESTBLENDALPHA = D3DBLEND_ZERO
        {209u, RsTier::Implemented, 1u},         // BLENDOPALPHA = D3DBLENDOP_ADD
        // ---- B 档（既有）：合法、只存（不记账：master 已记账 / 真机不读）----
        {34u, RsTier::Stored, kStoredRsValue},   // FOGCOLOR
        {35u, RsTier::Stored, kStoredRsValue},   // FOGTABLEMODE
        {36u, RsTier::Stored, kStoredRsValue},   // FOGSTART
        {37u, RsTier::Stored, kStoredRsValue},   // FOGEND
        {38u, RsTier::Stored, kStoredRsValue},   // FOGDENSITY
        {53u, RsTier::Stored, kStoredRsValue},   // STENCILFAIL
        {54u, RsTier::Stored, kStoredRsValue},   // STENCILZFAIL
        {55u, RsTier::Stored, kStoredRsValue},   // STENCILPASS
        {56u, RsTier::Stored, kStoredRsValue},   // STENCILFUNC
        {57u, RsTier::Stored, kStoredRsValue},   // STENCILREF
        {58u, RsTier::Stored, kStoredRsValue},   // STENCILMASK
        {59u, RsTier::Stored, kStoredRsValue},   // STENCILWRITEMASK
        {128u, RsTier::Stored, kStoredRsValue},  // WRAP0
        {129u, RsTier::Stored, kStoredRsValue},  // WRAP1
        {130u, RsTier::Stored, kStoredRsValue},  // WRAP2
        {131u, RsTier::Stored, kStoredRsValue},  // WRAP3
        {132u, RsTier::Stored, kStoredRsValue},  // WRAP4
        {133u, RsTier::Stored, kStoredRsValue},  // WRAP5
        {134u, RsTier::Stored, kStoredRsValue},  // WRAP6
        {135u, RsTier::Stored, kStoredRsValue},  // WRAP7
        {140u, RsTier::Stored, kStoredRsValue},  // FOGVERTEXMODE
        {142u, RsTier::Stored, kStoredRsValue},  // LOCALVIEWER
        {143u, RsTier::Stored, kStoredRsValue},  // NORMALIZENORMALS
        {185u, RsTier::Stored, kStoredRsValue},  // TWOSIDEDSTENCILMODE
        {186u, RsTier::Stored, kStoredRsValue},  // CCW_STENCILFAIL
        {187u, RsTier::Stored, kStoredRsValue},  // CCW_STENCILZFAIL
        {188u, RsTier::Stored, kStoredRsValue},  // CCW_STENCILPASS
        {189u, RsTier::Stored, kStoredRsValue},  // CCW_STENCILFUNC
        {190u, RsTier::Stored, kStoredRsValue},  // COLORWRITEENABLE1
        {191u, RsTier::Stored, kStoredRsValue},  // COLORWRITEENABLE2
        {192u, RsTier::Stored, kStoredRsValue},  // COLORWRITEENABLE3
        {193u, RsTier::Stored, kStoredRsValue},  // BLENDFACTOR
        // ---- B 档（本单新增，`REQ-01M7A3M73R3XYMM7J4KNQ1VNV7`）：合法、本线不实现 ⇒
        //      存下 + bit41 记账（**新收的一批统一记账**；修前它们落进 default 的 INVALIDCALL）----
        {48u, RsTier::StoredUnimplemented, kStoredRsValue},   // RANGEFOGENABLE
        {163u, RsTier::StoredUnimplemented, kStoredRsValue},  // PATCHEDGESTYLE
        {165u, RsTier::StoredUnimplemented, kStoredRsValue},  // DEBUGMONITORTOKEN
        {172u, RsTier::StoredUnimplemented, kStoredRsValue},  // POSITIONDEGREE
        {173u, RsTier::StoredUnimplemented, kStoredRsValue},  // NORMALDEGREE
        {178u, RsTier::StoredUnimplemented, kStoredRsValue},  // MINTESSELLATIONLEVEL
        {179u, RsTier::StoredUnimplemented, kStoredRsValue},  // MAXTESSELLATIONLEVEL
        {180u, RsTier::StoredUnimplemented, kStoredRsValue},  // ADAPTIVETESS_X
        {181u, RsTier::StoredUnimplemented, kStoredRsValue},  // ADAPTIVETESS_Y
        {182u, RsTier::StoredUnimplemented, kStoredRsValue},  // ADAPTIVETESS_Z
        {183u, RsTier::StoredUnimplemented, kStoredRsValue},  // ADAPTIVETESS_W
        {198u, RsTier::StoredUnimplemented, kStoredRsValue},  // WRAP8
        {199u, RsTier::StoredUnimplemented, kStoredRsValue},  // WRAP9
        {200u, RsTier::StoredUnimplemented, kStoredRsValue},  // WRAP10
        {201u, RsTier::StoredUnimplemented, kStoredRsValue},  // WRAP11
        {202u, RsTier::StoredUnimplemented, kStoredRsValue},  // WRAP12
        {203u, RsTier::StoredUnimplemented, kStoredRsValue},  // WRAP13
        {204u, RsTier::StoredUnimplemented, kStoredRsValue},  // WRAP14
        {205u, RsTier::StoredUnimplemented, kStoredRsValue},  // WRAP15
        // ---- C 档：合法但值敏感（有效值 ⇒ INVALIDCALL + bit26；值判断在实现里的 case）----
        {175u, RsTier::ValueSensitive, 1u},  // SLOPESCALEDEPTHBIAS
        {176u, RsTier::ValueSensitive, 1u},  // ANTIALIASEDLINEENABLE
        {184u, RsTier::ValueSensitive, 1u},  // ENABLEADAPTIVETESSELLATION
        {194u, RsTier::ValueSensitive, 1u},  // SRGBWRITEENABLE
        {195u, RsTier::ValueSensitive, 1u},  // DEPTHBIAS
    };
    return kDomain;
}

void caseRenderStateDomain() {
    lift_test::g_current_case = "d3d9p/render-state-domain";
    Rig r;
    LIFT_CHECK(r.up());
    if (r.f.device.object == 0) return;

    // ★ 本用例要 ~5 百次 guest 调用（每次一块 4 KiB 的 code 页），而 Set 调用只推 `code_ea`、
    //   不推 `data_ea`（两者起点只差 31 页）⇒ 跑下去 data/code 会落进同一页，出参缓冲被当指令写掉。
    //   于是把数据区挪到 code 区够不到的地方（本用例 code 顶 ≈ 0x00600000）。
    r.f.data_ea = 0x00A00000u;
    const std::uint32_t out_ea = r.f.placeU32(0xDEADBEEFu);
    auto getHr = [&r, out_ea](std::uint32_t state) {
        r.f.m.mem().wr32(out_ea, 0xDEADBEEFu);
        return r.f.method(r.f.device.object, kOffGetRenderState, {state, out_ea});
    };
    auto getValue = [&r, out_ea](std::uint32_t state) {
        const std::uint32_t hr = r.f.method(r.f.device.object, kOffGetRenderState, {state, out_ea});
        return hr == lift::d3d9::kOk ? r.f.m.mem().rd32(out_ea) : 0xDEADBEEFu;
    };
    const std::vector<RsDomain>& domain = d3dRsDomain();
    auto isLegal = [&domain](std::uint32_t state) {
        for (const RsDomain& e : domain)
            if (e.state == state) return true;
        return false;
    };

    // ---- (1) 先做**非 D3DRS**：此刻 bit26 / bit41 都还是 0 ⇒ "记了账"一定会被抓 ----
    LIFT_CHECK(!deviceInfoBit(r.f, 26));  // 前置：kUnRenderStateUnknown（C 档）
    LIFT_CHECK(!deviceInfoBit(r.f, 41));  // 前置：kUnRenderStateStored（B 档新增）
    std::uint32_t illegal_seen = 0;
    for (std::uint32_t v = 0; v <= 255u; ++v) {
        if (isLegal(v)) continue;
        ++illegal_seen;
        LIFT_CHECK_EQ_U32(r.f.setRs(v, 1u), lift::d3d9::kInvalidCall);
        LIFT_CHECK_EQ_U32(getHr(v), lift::d3d9::kInvalidCall);
    }
    // 越界 / 回绕：>209 与 0xFFFFFFFF 同样**不是** D3DRS 值 ⇒ Set / Get 都拒。
    LIFT_CHECK_EQ_U32(r.f.setRs(256u, 1u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(getHr(256u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.setRs(0xFFFFu, 1u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(getHr(0xFFFFu), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(r.f.setRs(0xFFFFFFFFu, 1u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_U32(getHr(0xFFFFFFFFu), lift::d3d9::kInvalidCall);
    LIFT_CHECK(illegal_seen != 0u);  // 全域判据别退化成空循环
    LIFT_CHECK(!deviceInfoBit(r.f, 26));  // "非法"**不记账**
    LIFT_CHECK(!deviceInfoBit(r.f, 41));

    // ---- (2) 合法 D3DRS：逐值验"接受（或显式拒绝并记账）+ Get 同域 + 原样读回" ----
    for (const RsDomain& e : domain) {
        switch (e.tier) {
            case RsTier::ValueSensitive:
                // C 档：有效值 ⇒ 显式拒绝（⛔ 不静默存下）+ 记账 bit26
                LIFT_CHECK_EQ_U32(r.f.setRs(e.state, e.value), lift::d3d9::kInvalidCall);
                LIFT_CHECK(deviceInfoBit(r.f, 26));
                break;
            case RsTier::Implemented:
            case RsTier::Stored:
            case RsTier::StoredUnimplemented:
                // A / B 档：**不许**返回 INVALIDCALL（B 档的判据就是"接受并存下"）
                LIFT_CHECK_EQ_U32(r.f.setRs(e.state, e.value), lift::d3d9::kOk);
                if (e.tier == RsTier::StoredUnimplemented) LIFT_CHECK(deviceInfoBit(r.f, 41));
                break;
        }
        // Get 与 Set **同域**：合法 D3DRS 一定读得到（C 档读回的是默认 0）
        LIFT_CHECK_EQ_U32(getHr(e.state), lift::d3d9::kOk);
        // "原样存下"：A / B 档的 Set 值必须逐位读回（C 档没存，不参与）
        if (e.tier != RsTier::ValueSensitive) LIFT_CHECK_EQ_U32(getValue(e.state), e.value);
    }
    // 两档记账各至少出现过一次（⛔ 不接受"静默收下"）
    LIFT_CHECK(deviceInfoBit(r.f, 26));  // C 档的"值敏感 ⇒ 拒绝"有痕迹
    LIFT_CHECK(deviceInfoBit(r.f, 41));  // B 档新增的"合法但没实现"有痕迹
}

// =======================================================================================
// ㉒ `d3d9p/validate-device-one-pass`（`REQ-01M79PP9H5XZRWFJD4BGS19S54`）
//
// 红了意味着什么：`ValidateDevice` 偏离了**已裁决的声明**"恒 `S_OK, *pNumPasses=1`"：
//   (i) 返回码不是 S_OK / `*pNumPasses != 1`（引擎侧若用它决定要不要重画，会直接错）；
//   (ii) 它有**副作用**（256 项 `GetRenderState` 或 `PipelineInfo` 的
//        `draws/draw_calls/begins/ends` 变了）—— `ValidateDevice` 只查不改；
//   (iii) 引擎实测的 16 个状态组合下它仍必须 `S_OK,1`，且紧接着的探针 Draw 仍 `S_OK`
//        （状态组合的变化不许把 ValidateDevice / draw 带坏）。
//
// ★ **这条用例是防漂移**（修前不会红）：`validateDevice` 今天就是恒 `S_OK,1` ⇒ ⛔ 不是缺陷复现，
//   也⛔ 不许把它当成"修好了"的凭据。1 pass 的**依据**与**翻面条件**写在
//   `d3d9_pipeline.cpp` 的 `validateDevice` 注释里（**翻面条件见实现注释**：CPU 兜底删除
//   且需要覆盖"设备状态组合不能用"时才翻面；⛔ 不是"RTT 落地"）—— 这条用例只盯"声明不许悄悄变"。
// =======================================================================================
int caseValidateDeviceOnePass() {
    lift_test::g_current_case = "d3d9p/validate-device-one-pass";
    constexpr std::uint32_t kHrOk = lift::d3d9::kOk;
    constexpr std::uint32_t kHrInvalid = lift::d3d9::kInvalidCall;
    Rig r;
    std::string why;
    // ★ 本用例要真的 Draw 成功（`before.draws != 0`）⇒ CPU 光栅已删 ⇒ 必须装真 GPU 后端。
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

    auto validate = [&r](std::uint32_t passes_ea) {
        return r.f.method(r.f.device.object, kOffValidateDevice, {passes_ea});
    };
    auto info = [&r]() {
        lift::d3d9::PipelineInfo pi;
        LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr));
        return pi;
    };

    // ---- (i) 恒 S_OK + `*pNumPasses == 1`（出参真的被写）----
    {
        const std::uint32_t passes_ea = r.f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(validate(passes_ea), kHrOk);
        LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(passes_ea), 1u);
    }
    // 出参指针非法 ⇒ INVALIDCALL（"恒 1 pass"不等于"不校验指针"）
    LIFT_CHECK_EQ_U32(validate(0u), kHrInvalid);

    // 先让计数器/状态都非 0（否则"不变"退化成"全 0 对比全 0"）
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), kHrOk);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), kHrOk);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsFillMode, 3u), kHrOk);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsColorWriteEnable, 0xFu), kHrOk);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), kHrOk);
    const std::uint32_t tri = r.f.placeRhwDiffuse(
        {{0.0f, 0.0f, 0.0f, 1.0f}, {10.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 10.0f, 0.0f, 1.0f}},
        {0xFFFF8000u, 0xFFFF8000u, 0xFFFF8000u});
    LIFT_CHECK_EQ_U32(r.f.beginScene(), kHrOk);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 1u, tri, 20u), kHrOk);

    // ---- (ii) 无副作用：256 项 GetRenderState + 场景计数器 ----
    std::vector<std::uint32_t> before_rs(256u, 0u);
    for (std::uint32_t i = 0; i < 256u; ++i) before_rs[i] = r.f.getRs(i);
    const lift::d3d9::PipelineInfo before = info();
    LIFT_CHECK_NE_U32(before.draws, 0u);
    LIFT_CHECK_NE_U32(before.draw_calls, 0u);
    LIFT_CHECK_NE_U32(before.begins, 0u);

    for (int k = 0; k < 3; ++k) {
        const std::uint32_t passes_ea = r.f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(validate(passes_ea), kHrOk);
        LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(passes_ea), 1u);
    }

    for (std::uint32_t i = 0; i < 256u; ++i) LIFT_CHECK_EQ_U32(r.f.getRs(i), before_rs[i]);
    const lift::d3d9::PipelineInfo after = info();
    LIFT_CHECK_EQ_U32(after.draws, before.draws);
    LIFT_CHECK_EQ_U32(after.draw_calls, before.draw_calls);
    LIFT_CHECK_EQ_U32(after.begins, before.begins);
    LIFT_CHECK_EQ_U32(after.ends, before.ends);

    // ---- (iii) 引擎实测的 16 个状态组合下仍 S_OK,1；紧接着的 Draw 也 S_OK ----
    for (const EngineRenderState& s : engineRenderStates()) {
        for (std::uint32_t v : s.values) LIFT_CHECK_EQ_U32(r.f.setRs(s.state, v), kHrOk);
        const std::uint32_t passes_ea = r.f.placeU32(0xDEADBEEFu);
        LIFT_CHECK_EQ_U32(validate(passes_ea), kHrOk);
        LIFT_CHECK_EQ_U32(r.f.m.mem().rd32(passes_ea), 1u);
    }
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 1u, tri, 20u), kHrOk);
    LIFT_CHECK_EQ_U32(r.f.endScene(), kHrOk);
    return 0;
}

// =======================================================================================
// ㉓ `d3d9p/material-source`（`REQ-01M7460MRJ6D82PWG6P6MX9WG9`）
//
// 红了意味着什么：`D3DRS_COLORVERTEX(141)` 不再门控材质来源 ⇒ `COLORVERTEX=0` 时顶点色
// **仍然**参与光照（本单的缺陷形态：顶点色红 + 材质 diffuse 蓝 ⇒ 输出偏红而不是材质蓝）。
//
// ★ 判据为什么是**纯函数**级：CPU 光栅已删，pure 臂没有 GPU 后端 ⇒ 任何 Draw 都是
//   `D3DERR_INVALIDCALL`，读不到像素。这里断的是 `applyLighting` 与 `transformVertex`
//   **共用的唯一一份**选择逻辑（`materialSourceColor`）—— 它决定的正是"哪一份颜色进光照方程"，
//   ⛔ 不是复述实现：换一份实现后"COLORVERTEX=0 ⇒ 材质 / =1 ⇒ 顶点色"仍然必须成立。
//   `D3DMCS_COLOR1` = 顶点 diffuse、`D3DMCS_COLOR2` = 顶点 specular（FVF 的第二颜色）。
// =======================================================================================
void caseMaterialSource() {
    lift_test::g_current_case = "d3d9p/material-source";
    // 单位与调用方一致：材质 0..1，顶点色 /255。
    const double material_blue[4] = {0.0, 0.0, 1.0, 1.0};  // 材质 diffuse = 蓝
    const double vertex_red[4] = {1.0, 0.0, 0.0, 1.0};     // 顶点 diffuse（COLOR1）= 红
    const double vertex_green[4] = {0.0, 1.0, 0.0, 1.0};   // 顶点 specular（COLOR2）= 绿
    double out[4] = {0.0, 0.0, 0.0, 0.0};

    // ① COLORVERTEX=0 ⇒ 顶点色不参与：即使 DIFFUSEMATERIALSOURCE=COLOR1，输出仍是材质蓝。
    //   ★ 修前（读法短路）这里会拿到红 ⇒ 本单的第一条判据当场红。
    lift::d3d9::materialSourceColor(0u, lift::d3d9::kDmcsColor1, material_blue, vertex_red, vertex_green, out);
    LIFT_CHECK(out[0] == 0.0 && out[1] == 0.0 && out[2] == 1.0 && out[3] == 1.0);

    // ② COLORVERTEX=1 + DIFFUSEMATERIALSOURCE=D3DMCS_COLOR1 ⇒ 输出受顶点色影响（红）。
    lift::d3d9::materialSourceColor(1u, lift::d3d9::kDmcsColor1, material_blue, vertex_red, vertex_green, out);
    LIFT_CHECK(out[0] == 1.0 && out[1] == 0.0 && out[2] == 0.0 && out[3] == 1.0);

    // ③ COLOR2 取**第二颜色**（顶点 specular），而不是顶点 diffuse。
    lift::d3d9::materialSourceColor(1u, lift::d3d9::kDmcsColor2, material_blue, vertex_red, vertex_green, out);
    LIFT_CHECK(out[0] == 0.0 && out[1] == 1.0 && out[2] == 0.0 && out[3] == 1.0);

    // ④ MATERIAL / 顶点色缺失 ⇒ 回退材质（⛔ 不把"没有 COLOR1"当成白色或黑色）。
    lift::d3d9::materialSourceColor(1u, lift::d3d9::kDmcsMaterial, material_blue, vertex_red, vertex_green, out);
    LIFT_CHECK(out[2] == 1.0 && out[0] == 0.0 && out[1] == 0.0);
    lift::d3d9::materialSourceColor(1u, lift::d3d9::kDmcsColor1, material_blue, nullptr, vertex_green, out);
    LIFT_CHECK(out[2] == 1.0 && out[0] == 0.0 && out[1] == 0.0);
}

// =======================================================================================
// ㉔ `d3d9p/perspective-interpolation`（B 单 `REQ-01M7NC21S484PFYDAE7YTNX02J`）
//
// 红了意味着什么：GPU 批把**逐顶点 `1/w`** 丢了（或片元里没除回来）⇒ 透视批在屏幕空间**线性**
//   插值 `uv/w` / `color/w`，画出来就是"仿射贴图"（近处拉长、纹理爬行）；反过来 `w == 1` 的批
//   也会被除错（固定功能的既有像素用例会红）。
//
// 几何（**两个深度不同的三角形** + 已知 uv / 颜色梯度）：
//   · 视图空间四边形：下边 `z = 1`（近）、上边 `z = 4`（远）；`PROJECTION = perspectiveFovLH`、
//     WORLD / VIEW = 单位阵 ⇒ `clip.w = z_view` 跨 **1..0.25 的 1/w**；
//   · 屏幕落点用 `clipVs10`（= 实现里 `mul(vec4(pos,1), Proj)` 的**同一份**算式）算 ⇒ 透视批在
//     屏幕上是一个**梯形**（近大远小）；
//   · 顶点属性：uv 从 (0,0)/(1,0) 到 (1,1)/(0,1)；顶点色 近=红 / 远=蓝。
//
// 四批 draw（同一个四边形；每批画完读回整幅像素）：
//   ① XYZ + TEX1（透视，w 跨 1..4）⇒ 逐像素 == 参考 `(Σλ·uv/w)/(Σλ·1/w)` 的**那个 texel**（≤1 LSB）；
//   ② ①的**仿射版**（XYZRHW, rhw=1 ⇒ `invw == 1`，屏幕几何与 ① 逐 float 相同）⇒ 逐像素 == 仿射参考；
//   ③ XYZ + DIFFUSE（透视）⇒ 逐像素 == 透视校正后的颜色（≤1 LSB）；
//   ④ ③的**仿射版**（XYZRHW）。
// 控制行（证明不是恒真）：②与"①的参考"在**同一批像素**上必须差 > 32 LSB 的至少 20 个；
//   ④与"③的参考"同理。②/④ 就是"故意不做透视校正"的那一档 —— 不是 bug，是本用例的控制行。
//
// ★ 采样只取**三角形内部**（重心权重 ≥ 0.03）且**远离 texel 边界**（0.06 texel）的像素：
//   前者避开光栅化填充规则的边界、后者让 POINT 采样的参考是**精确的某个 texel**（与 float 抖动无关）。
// ★ 参考 texel 由纹理内容**唯一**决定（texel(x,y) 的 R=x*16 / G=y*16 / B=255-x*16）⇒ 错一个 texel 就红。
// ★ 没有 GPU 设备 / 没有显示系统 ⇒ SKIP 77（⛔ 不拿 Mock 冒充像素判据）。
// =======================================================================================
namespace {

// 参考用的屏幕空间三角形 —— 顶点坐标就是**真正交给光栅器的那组 float**（⛔ 不另算一份几何）。
struct RefTri {
    float x[3];
    float y[3];
    double invw[3];  // 1/w（仿射档全 1）
    double u[3];
    double v[3];
    double r[3], g[3], b[3];  // 0..1
};

// p 对 tri 的重心坐标；三个都 >= margin（= 只采内部点）⇒ true。
bool refBary(const RefTri& t, double px, double py, double l[3], double margin) {
    const double ax = static_cast<double>(t.x[0]);
    const double ay = static_cast<double>(t.y[0]);
    const double bx = static_cast<double>(t.x[1]);
    const double by = static_cast<double>(t.y[1]);
    const double cx = static_cast<double>(t.x[2]);
    const double cy = static_cast<double>(t.y[2]);
    const double det = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (std::fabs(det) < 1e-9) return false;
    l[1] = ((px - ax) * (cy - ay) - (py - ay) * (cx - ax)) / det;
    l[2] = ((bx - ax) * (py - ay) - (by - ay) * (px - ax)) / det;
    l[0] = 1.0 - l[1] - l[2];
    return l[0] >= margin && l[1] >= margin && l[2] >= margin;
}

// 透视校正插值：`Σ λ·attr/w ÷ Σ λ·(1/w)`。`invw == 1` ⇒ 退化成屏幕空间线性（控制行就是它）。
double refAttr(const RefTri& t, const double l[3], const double* attr) {
    double num = 0.0;
    double den = 0.0;
    for (int i = 0; i < 3; ++i) {
        num += l[i] * attr[i] * t.invw[i];
        den += l[i] * t.invw[i];
    }
    return den != 0.0 ? num / den : 0.0;
}

int refChannelDiff(std::uint32_t a, std::uint32_t b) {
    int worst = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        const int d = static_cast<int>((a >> sh) & 0xFFu) - static_cast<int>((b >> sh) & 0xFFu);
        const int ad = d < 0 ? -d : d;
        if (ad > worst) worst = ad;
    }
    return worst;
}

std::uint8_t refQuantize(double c) {
    if (!(c > 0.0)) return 0u;
    if (c >= 1.0) return 255u;
    return static_cast<std::uint8_t>(c * 255.0 + 0.5);
}

}  // namespace

int casePerspectiveInterpolation() {
    lift_test::g_current_case = "d3d9p/perspective-interpolation";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

    constexpr int kW = 64;
    constexpr int kH = 64;
    constexpr int kTexW = 16;
    constexpr int kTexH = 16;

    // ---- 16×16 纹理：texel(x,y) 的 R=x*16 / G=y*16 / B=255-x*16（每个 texel 唯一）----
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(r.f.createTexture(r.f.device.object, kTexW, kTexH, lift::d3d9::kFmtA8R8G8B8, &tex), 0u);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return 0;
    {
        const std::uint32_t locked = r.f.placeBytes(std::vector<std::uint8_t>(8, 0));
        LIFT_CHECK_EQ_U32(r.f.method(tex, kOffTextureLockRect, {0u, locked, 0u, 0u}), 0u);
        const std::uint32_t pitch = r.f.m.mem().rd32(locked + 0u);
        const std::uint32_t bits = r.f.m.mem().rd32(locked + 4u);
        LIFT_CHECK_NE_U32(bits, 0u);
        for (int y = 0; y < kTexH; ++y) {
            for (int x = 0; x < kTexW; ++x) {
                const std::uint32_t rr = static_cast<std::uint32_t>(x * 16);
                const std::uint32_t gg = static_cast<std::uint32_t>(y * 16);
                const std::uint32_t bb = 255u - rr;
                // A8R8G8B8 的内存布局 = BGRA（低字节是 B）⇒ 按 D3DCOLOR(ARGB) 写字面值
                const std::uint32_t argb = 0xFF000000u | (rr << 16) | (gg << 8) | bb;
                r.f.m.mem().wr32(bits + static_cast<std::uint32_t>(y) * pitch +
                                     static_cast<std::uint32_t>(x) * 4u,
                                 argb);
            }
        }
        LIFT_CHECK_EQ_U32(r.f.method(tex, kOffTextureUnlockRect, {0u}), 0u);
    }

    // ---- 几何：视图空间四边形（下边 z=1 / 上边 z=4）+ 透视 PROJECTION ----
    float proj16[16] = {};
    lift::math::toD3DLayout(lift::math::perspectiveFovLH(0.7853982f, 1.0f, 0.1f, 100.0f), proj16);
    const float view_pos[4][3] = {{-0.35f, -0.35f, 1.0f},
                                  {0.35f, -0.35f, 1.0f},
                                  {0.35f, 0.35f, 4.0f},
                                  {-0.35f, 0.35f, 4.0f}};
    const float vert_uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}};
    // D3DCOLOR = ARGB：近边红 / 远边蓝
    const std::uint32_t vert_col[4] = {0xFFFF0000u, 0xFFFF0000u, 0xFF0000FFu, 0xFF0000FFu};

    float scr[4][2] = {};
    double invw[4] = {};
    for (int i = 0; i < 4; ++i) {
        const lift::math::Vec4d c =
            lift::d3d9::clipVs10(proj16, static_cast<double>(view_pos[i][0]),
                                 static_cast<double>(view_pos[i][1]), static_cast<double>(view_pos[i][2]));
        invw[i] = 1.0 / c.w;
        scr[i][0] = static_cast<float>((c.x * invw[i] * 0.5 + 0.5) * static_cast<double>(kW));
        scr[i][1] = static_cast<float>((-c.y * invw[i] * 0.5 + 0.5) * static_cast<double>(kH));
    }
    // 两个**深度不同**的三角形：T0=(0,1,2) / T1=(0,2,3)。近边 1/w=1、远边 1/w=0.25。
    const int tri_idx[2][3] = {{0, 1, 2}, {0, 2, 3}};
    RefTri tri[2] = {};
    RefTri tri_affine[2] = {};
    for (int ti = 0; ti < 2; ++ti) {
        for (int k = 0; k < 3; ++k) {
            const int i = tri_idx[ti][k];
            tri[ti].x[k] = scr[i][0];
            tri[ti].y[k] = scr[i][1];
            tri[ti].invw[k] = invw[i];
            tri[ti].u[k] = static_cast<double>(vert_uv[i][0]);
            tri[ti].v[k] = static_cast<double>(vert_uv[i][1]);
            tri[ti].r[k] = static_cast<double>((vert_col[i] >> 16) & 0xFFu) / 255.0;
            tri[ti].g[k] = static_cast<double>((vert_col[i] >> 8) & 0xFFu) / 255.0;
            tri[ti].b[k] = static_cast<double>(vert_col[i] & 0xFFu) / 255.0;
        }
        tri_affine[ti] = tri[ti];
        tri_affine[ti].invw[0] = 1.0;  // 控制行：故意**不做**透视校正
        tri_affine[ti].invw[1] = 1.0;
        tri_affine[ti].invw[2] = 1.0;
    }
    LIFT_CHECK(tri[0].invw[0] == 1.0 && std::fabs(tri[0].invw[2] - 0.25) < 1e-9);

    // 像素中心处的参考（透视 / 仿射共用一份算式，只有 `invw` 不同）。
    const auto sampleRef = [&](bool perspective, int x, int y, double* u, double* v, double* cr,
                               double* cg, double* cb) -> bool {
        const double px = static_cast<double>(x) + 0.5;
        const double py = static_cast<double>(y) + 0.5;
        for (int ti = 0; ti < 2; ++ti) {
            double l[3] = {0.0, 0.0, 0.0};
            if (!refBary(tri[ti], px, py, l, 0.03)) continue;
            const RefTri& t = perspective ? tri[ti] : tri_affine[ti];
            *u = refAttr(t, l, t.u);
            *v = refAttr(t, l, t.v);
            *cr = refAttr(t, l, t.r);
            *cg = refAttr(t, l, t.g);
            *cb = refAttr(t, l, t.b);
            return true;
        }
        return false;
    };
    // uv → texel（POINT + CLAMP + level 0）。`safe` = 离 texel 边界 > 0.06 texel（参考不受 float 抖动影响）。
    const auto texelOf = [&](double u, double v, int* tx, int* ty) -> bool {
        const double fu = u * static_cast<double>(kTexW);
        const double fv = v * static_cast<double>(kTexH);
        const int ix = static_cast<int>(std::floor(fu));
        const int iy = static_cast<int>(std::floor(fv));
        const double du = fu - static_cast<double>(ix);
        const double dv = fv - static_cast<double>(iy);
        if (ix < 0 || ix >= kTexW || iy < 0 || iy >= kTexH) return false;
        if (!(du > 0.06 && du < 0.94 && dv > 0.06 && dv < 0.94)) return false;
        *tx = ix;
        *ty = iy;
        return true;
    };
    const auto texelPixel = [&](int tx, int ty) -> std::uint32_t {
        return (static_cast<std::uint32_t>(tx * 16) << 16) | (static_cast<std::uint32_t>(ty * 16) << 8) |
               (255u - static_cast<std::uint32_t>(tx * 16));
    };
    const auto snapshot = [&]() {
        std::vector<std::uint32_t> out(static_cast<std::size_t>(kW) * kH, 0u);
        for (int y = 0; y < kH; ++y) {
            for (int x = 0; x < kW; ++x) {
                out[static_cast<std::size_t>(y) * kW + x] = r.f.pixelAt(x, y) & 0x00FFFFFFu;
            }
        }
        return out;
    };

    // ---- 公共状态 ----
    const std::vector<float> identity16 = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTransform(lift::d3d9::kTsWorld, identity16), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTransform(lift::d3d9::kTsView, identity16), 0u);
    LIFT_CHECK_EQ_U32(r.f.setTransform(lift::d3d9::kTsProjection,
                                       std::vector<float>(proj16, proj16 + 16)),
                      0u);
    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, tex), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMagFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMinFilter, lift::d3d9::kTexfPoint), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampMipFilter, lift::d3d9::kTexfNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampAddressU, lift::d3d9::kAddressClamp), 0u);
    LIFT_CHECK_EQ_U32(r.f.setSampler(0u, lift::d3d9::kSampAddressV, lift::d3d9::kAddressClamp), 0u);

    // 顶点缓冲：顶点顺序 = 两个三角形（{0,1,2} / {0,2,3}）。
    const int order[6] = {0, 1, 2, 0, 2, 3};
    const auto pushF = [](std::vector<std::uint8_t>* out, float f) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        for (int b = 0; b < 4; ++b) out->push_back(static_cast<std::uint8_t>((bits >> (8 * b)) & 0xFFu));
    };
    const auto pushU32 = [](std::vector<std::uint8_t>* out, std::uint32_t v) {
        for (int b = 0; b < 4; ++b) out->push_back(static_cast<std::uint8_t>((v >> (8 * b)) & 0xFFu));
    };
    // XYZ + TEX1（20 B）
    const auto build_xyz_tex = [&]() {
        std::vector<std::uint8_t> vb;
        for (int k = 0; k < 6; ++k) {
            const int i = order[k];
            pushF(&vb, view_pos[i][0]);
            pushF(&vb, view_pos[i][1]);
            pushF(&vb, view_pos[i][2]);
            pushF(&vb, vert_uv[i][0]);
            pushF(&vb, vert_uv[i][1]);
        }
        return r.f.placeBytes(vb);
    };
    // XYZRHW + DIFFUSE + TEX1（28 B；rhw=1 ⇒ 屏幕坐标就是顶点坐标）
    const auto build_rhw_tex = [&]() {
        std::vector<std::uint8_t> vb;
        for (int k = 0; k < 6; ++k) {
            const int i = order[k];
            pushF(&vb, scr[i][0]);
            pushF(&vb, scr[i][1]);
            pushF(&vb, 0.0f);
            pushF(&vb, 1.0f);
            pushU32(&vb, 0xFFFFFFFFu);
            pushF(&vb, vert_uv[i][0]);
            pushF(&vb, vert_uv[i][1]);
        }
        return r.f.placeBytes(vb);
    };
    // XYZ + DIFFUSE（16 B）
    const auto build_xyz_diffuse = [&]() {
        std::vector<std::uint8_t> vb;
        for (int k = 0; k < 6; ++k) {
            const int i = order[k];
            pushF(&vb, view_pos[i][0]);
            pushF(&vb, view_pos[i][1]);
            pushF(&vb, view_pos[i][2]);
            pushU32(&vb, vert_col[i]);
        }
        return r.f.placeBytes(vb);
    };
    // XYZRHW + DIFFUSE（20 B）
    const auto build_rhw_diffuse = [&]() {
        std::vector<std::uint8_t> vb;
        for (int k = 0; k < 6; ++k) {
            const int i = order[k];
            pushF(&vb, scr[i][0]);
            pushF(&vb, scr[i][1]);
            pushF(&vb, 0.0f);
            pushF(&vb, 1.0f);
            pushU32(&vb, vert_col[i]);
        }
        return r.f.placeBytes(vb);
    };
    const auto draw_and_read = [&](std::uint32_t fvf, std::uint32_t stride, std::uint32_t vb,
                                   std::vector<std::uint32_t>* out) -> bool {
        LIFT_CHECK_EQ_U32(r.f.setFvf(fvf), 0u);
        LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
        LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
        const std::uint32_t hr = r.f.drawUp(lift::d3d9::kPtTriangleList, 2u, vb, stride);
        if (hr != 0u) {
            std::fprintf(stderr, "[perspective] draw 被拒 hr=0x%08X fvf=0x%X\n", hr, fvf);
            return false;
        }
        LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
        if (!r.syncPixel(&why)) return false;
        *out = snapshot();
        return true;
    };

    // ---- ① 透视纹理批 / ② 同一屏幕几何的仿射纹理批 ----
    std::vector<std::uint32_t> px_tex_persp;
    std::vector<std::uint32_t> px_tex_affine;
    const std::uint32_t vb_xyz_tex = build_xyz_tex();
    if (!draw_and_read(0x0102u, 20u, vb_xyz_tex, &px_tex_persp)) {
        LIFT_SKIP(why.c_str());
    }
    const std::uint32_t vb_rhw_tex = build_rhw_tex();
    if (!draw_and_read(0x0144u, 28u, vb_rhw_tex, &px_tex_affine)) {
        LIFT_SKIP(why.c_str());
    }

    int tex_sampled = 0;
    int tex_worst_persp = 0;
    int tex_worst_affine = 0;
    int tex_control_pixels = 0;
    int tex_control_max = 0;
    bool texel_seen[kTexW * kTexH] = {};
    int texel_seen_count = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            double pu = 0.0, pv = 0.0, pr = 0.0, pg = 0.0, pb = 0.0;
            if (!sampleRef(true, x, y, &pu, &pv, &pr, &pg, &pb)) continue;
            int ptx = 0, pty = 0;
            if (!texelOf(pu, pv, &ptx, &pty)) continue;  // 靠近 texel 边界 ⇒ 不参与
            double au = 0.0, av = 0.0, ar = 0.0, ag = 0.0, ab = 0.0;
            if (!sampleRef(false, x, y, &au, &av, &ar, &ag, &ab)) continue;
            int atx = 0, aty = 0;
            if (!texelOf(au, av, &atx, &aty)) continue;  // 仿射参考也要"远离边界"才能比
            const std::size_t at = static_cast<std::size_t>(y) * kW + x;
            const std::uint32_t want_persp = texelPixel(ptx, pty);
            const std::uint32_t want_affine = texelPixel(atx, aty);
            const int dp = refChannelDiff(px_tex_persp[at], want_persp);
            const int da = refChannelDiff(px_tex_affine[at], want_affine);
            if (dp > tex_worst_persp) tex_worst_persp = dp;
            if (da > tex_worst_affine) tex_worst_affine = da;
            const int dc = refChannelDiff(px_tex_affine[at], want_persp);
            if (dc > tex_control_max) tex_control_max = dc;
            if (dc > 32) ++tex_control_pixels;
            const int idx = pty * kTexW + ptx;
            if (!texel_seen[idx]) {
                texel_seen[idx] = true;
                ++texel_seen_count;
            }
            ++tex_sampled;
        }
    }
    LIFT_CHECK(tex_sampled >= 100);
    LIFT_CHECK(texel_seen_count >= 16);   // 参考真的在扫过多个 texel（⛔ 不是一块单色）
    LIFT_CHECK(tex_worst_persp <= 1);     // ① 与 `uv/w ÷ 1/w` 的参考逐像素 ≤1 LSB
    LIFT_CHECK(tex_worst_affine <= 1);    // ② 仿射版与仿射参考一致（同一条光栅路径）
    LIFT_CHECK(tex_control_pixels >= 20); // 控制行：不做透视校正 ⇒ 至少 20 个像素差 > 32 LSB
    std::printf(
        "perspective/uv：采样 %d 像素（%d 个 texel）；透视批最大误差 %d LSB；仿射批 %d LSB；"
        "控制行 %d 像素差 >32（最大 %d）\n",
        tex_sampled, texel_seen_count, tex_worst_persp, tex_worst_affine, tex_control_pixels,
        tex_control_max);

    // ---- ③ 透视颜色批 / ④ 同一屏幕几何的仿射颜色批（无纹理 ⇒ 输出 = 插值后的顶点色）----
    LIFT_CHECK_EQ_U32(r.f.setTexture(0u, 0u), 0u);
    std::vector<std::uint32_t> px_col_persp;
    std::vector<std::uint32_t> px_col_affine;
    const std::uint32_t vb_xyz_col = build_xyz_diffuse();
    if (!draw_and_read(0x0042u, 16u, vb_xyz_col, &px_col_persp)) {
        LIFT_SKIP(why.c_str());
    }
    const std::uint32_t vb_rhw_col = build_rhw_diffuse();
    if (!draw_and_read(0x0044u, 20u, vb_rhw_col, &px_col_affine)) {
        LIFT_SKIP(why.c_str());
    }

    int col_sampled = 0;
    int col_worst_persp = 0;
    int col_worst_affine = 0;
    int col_control_pixels = 0;
    int col_control_max = 0;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            double pu = 0.0, pv = 0.0, pr = 0.0, pg = 0.0, pb = 0.0;
            if (!sampleRef(true, x, y, &pu, &pv, &pr, &pg, &pb)) continue;
            double au = 0.0, av = 0.0, ar = 0.0, ag = 0.0, ab = 0.0;
            if (!sampleRef(false, x, y, &au, &av, &ar, &ag, &ab)) continue;
            const std::size_t at = static_cast<std::size_t>(y) * kW + x;
            const std::uint32_t want_persp =
                (static_cast<std::uint32_t>(refQuantize(pr)) << 16) |
                (static_cast<std::uint32_t>(refQuantize(pg)) << 8) | refQuantize(pb);
            const std::uint32_t want_affine =
                (static_cast<std::uint32_t>(refQuantize(ar)) << 16) |
                (static_cast<std::uint32_t>(refQuantize(ag)) << 8) | refQuantize(ab);
            const int dp = refChannelDiff(px_col_persp[at], want_persp);
            const int da = refChannelDiff(px_col_affine[at], want_affine);
            if (dp > col_worst_persp) col_worst_persp = dp;
            if (da > col_worst_affine) col_worst_affine = da;
            const int dc = refChannelDiff(px_col_affine[at], want_persp);
            if (dc > col_control_max) col_control_max = dc;
            if (dc > 24) ++col_control_pixels;
            ++col_sampled;
        }
    }
    LIFT_CHECK(col_sampled >= 100);
    LIFT_CHECK(col_worst_persp <= 1);
    LIFT_CHECK(col_worst_affine <= 1);
    LIFT_CHECK(col_control_pixels >= 20);
    std::printf("perspective/color：采样 %d 像素；透视批最大误差 %d LSB；仿射批 %d LSB；"
                "控制行 %d 像素差 >24（最大 %d）\n",
                col_sampled, col_worst_persp, col_worst_affine, col_control_pixels, col_control_max);

    // 解绑 / 释放纹理（后面的用例不再依赖它）
    LIFT_CHECK_EQ_U32(r.f.method(tex, 0x08u, {}), 0u);
    return 0;
}

// =======================================================================================
// ㉕ `d3d9p/vertex-w-refused`（B 单的**具名拒绝**面）
//
// 红了意味着什么：`w <= 0` 的顶点不再被具名拒绝 —— 要么又被"凡 `w != 1` 就拒"的旧闸盖住（透视批也画
//   不了，另一种红在 `perspective-interpolation`），要么被当成 `1/w`（`w = 0` 时静默丢、`w < 0` 时
//   镜像到错误位置）。
//
// 判据：XYZRHW 的 `rhw = -1` ⇒ `w = -1` ⇒ Draw 回 `D3DERR_INVALIDCALL`、位 21 `kUnGpuVertexW` 置位、
//   `skipped_draws +1`（一个像素都不写 ⇒ 必须有账）；同一三角形的 `rhw = +1` 是**正控**（S_OK、
//   `skipped` 不涨、位 21 不置位）。
// ★ 这一档只在 `--gpu` 臂：pure 臂里"任何 Draw 都 INVALIDCALL"（`kUnGpuNoBackend`）会让断言恒真。
// =======================================================================================
int caseVertexWRefused() {
    lift_test::g_current_case = "d3d9p/vertex-w-refused";
    Rig r;
    std::string why;
    if (!r.upGpu(&why)) LIFT_SKIP(why.c_str());

    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsZEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsCullMode, lift::d3d9::kCullNone), 0u);
    LIFT_CHECK_EQ_U32(r.f.setRs(lift::d3d9::kRsAlphaBlendEnable, 0u), 0u);
    LIFT_CHECK_EQ_U32(r.f.setFvf(kFvfXyzRhwDiffuse), 0u);
    const auto device_bit21 = [&]() {
        lift::d3d9::PipelineInfo pi;
        if (!lift::d3d9::pipelineInfoOfDevice(r.f.m.cpu(), r.f.device.state, &pi, nullptr)) return true;
        return (pi.unsupported[21u / 32u] & (1u << (21u % 32u))) != 0u;
    };

    // ---- 正控：rhw = 1（w = 1）⇒ 正常画、skipped 不涨、位 21 不置位 ----
    //   ★ 覆盖账在**每次 `Machine::run` 开头**重算（`gpuCoverageReset`）⇒ 必须在"Draw 是这一跑
    //     最后一次 guest 调用"时读：`clearTo` / `beginScene` / `drawUp` 各是一跑，读完再 `endScene`。
    const std::uint32_t ok_tri = r.f.placeRhwDiffuse(
        {{0.0f, 0.0f, 0.0f, 1.0f}, {20.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 20.0f, 0.0f, 1.0f}},
        {0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u});
    LIFT_CHECK_EQ_U32(r.f.clearTo(kClearColor), 0u);
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 1u, ok_tri, 20u), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::gpuCoverage().skipped_draws), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::gpuCoverage().gpu_draws), 1);
    LIFT_CHECK(!device_bit21());
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);

    // ---- 负控：rhw = -1（w = -1 ⇒ `1/w` 无意义）⇒ 具名拒绝 ----
    const std::uint32_t bad_tri = r.f.placeRhwDiffuse(
        {{0.0f, 0.0f, 0.0f, -1.0f}, {20.0f, 0.0f, 0.0f, -1.0f}, {0.0f, 20.0f, 0.0f, -1.0f}},
        {0xFFFF0000u, 0xFFFF0000u, 0xFFFF0000u});
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 1u, bad_tri, 20u), lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::gpuCoverage().skipped_draws), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::gpuCoverage().gpu_draws), 0);
    LIFT_CHECK(device_bit21());
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    // 拒绝之后同一份几何（w 合法）照旧能画（⛔ 不是把整条路关掉）
    LIFT_CHECK_EQ_U32(r.f.beginScene(), 0u);
    LIFT_CHECK_EQ_U32(r.f.drawUp(lift::d3d9::kPtTriangleList, 1u, ok_tri, 20u), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::gpuCoverage().skipped_draws), 0);
    LIFT_CHECK_EQ_U32(r.f.endScene(), 0u);
    return 0;
}

}  // namespace

// ★ 两条臂（`REQ-01M7A74YG08R65NJBFXF7DEJDK`）：CPU 光栅已删 ⇒ 要**画像素**的用例必须装真 GPU。
//   · 缺省（pure 臂）：状态机 / 变换 / 绑定生命周期 / 拒绝类合同（不画像素或只断言"没画"）；
//   · `--gpu`（`d3d9-pipeline-gpu` 这条 CTest）：保留的像素断言，隐藏窗口 + SDL_GPU + 读回。
//   没有 GPU 设备 ⇒ `--gpu` 臂 SKIP 77（`SKIP_RETURN_CODE`），⛔ 不假绿。
int main(int argc, char** argv) {
    using namespace lift_test;
    const bool gpu_arm = argc > 1 && std::string(argv[1]) == "--gpu";
    try {
        if (gpu_arm) {
            // ★ CTest 给 `d3d9-pipeline-gpu` 设了 `SDL_VIDEODRIVER=dummy`（无显示器的机器上别去建窗口），
            //   但 dummy 视频下建不出 SDL_GPU 设备 ⇒ 这里让开，让真实视频驱动建隐藏窗口与 GPU 设备。
#if defined(_WIN32)
            _putenv_s("SDL_VIDEODRIVER", "");
#else
            ::unsetenv("SDL_VIDEODRIVER");
#endif
            int rc = caseSolidTriangle();
            if (rc != 0) return rc;  // SKIP 77 / 夹具失败
            rc = caseTexturedQuad();
            if (rc != 0) return rc;
            rc = caseMinFilterSampling();
            if (rc != 0) return rc;
            rc = caseMipLevels();
            if (rc != 0) return rc;
            rc = caseZBuffer();
            if (rc != 0) return rc;
            rc = caseAlphaBlend();
            if (rc != 0) return rc;
            rc = caseDrawStateSnapshot();
            if (rc != 0) return rc;
            rc = caseClipPlanes();
            if (rc != 0) return rc;
            rc = caseStaleTextureBindingInDraw();
            if (rc != 0) return rc;
            // ★ 陈旧**流 / 索引**句柄也要真后端：pure 臂里"任何 Draw 都 INVALIDCALL"使判据恒真
            //   （复审报告 §2.2.1）⇒ 必须在 `--gpu` 臂、带正向对照。
            rc = caseStaleStreamBinding();
            if (rc != 0) return rc;
            rc = caseIndexedRange();
            if (rc != 0) return rc;
            // ★ 声明路（Live2D）× 两 stream：区间映射 / start_index int64 / 真的画出像素
            //   （`REQ-01M7A3M7E59JX74G897Q7W2RVD` · `REQ-01M79P02VYRJ3V0C0V5VHXVE3H`）
            rc = caseIndexedRangeDecl();
            if (rc != 0) return rc;
            rc = caseIndexedRangeStartWrap();
            if (rc != 0) return rc;
            rc = caseVertexDeclDraw();
            if (rc != 0) return rc;
            // ★ 离屏 plain surface 不能当 RT（拒绝 + 渲染目标不变 + 纹理 RT 仍可绑）
            rc = caseSetRenderTargetOffscreenRefused();
            if (rc != 0) return rc;
            rc = caseValidateDeviceOnePass();
            if (rc != 0) return rc;
            // ★ B 单：GPU 批的透视校正插值（uv/w · color/w · 逐顶点 1/w）+ 仿射控制行
            rc = casePerspectiveInterpolation();
            if (rc != 0) return rc;
            // ★ B 单：`w <= 0` 的具名拒绝（旧闸拆掉之后**只剩**这一类真拒绝）
            rc = caseVertexWRefused();
            if (rc != 0) return rc;
            return finish("d3d9-pipeline-gpu");
        }
        caseTransformRoundtrip();
        caseRenderStateBound();
        caseScenePairing();
        caseRenderTargetLifetime();
        caseSetRenderTargetViewport();
        caseOwnerLifetimeAndStaleGeneration();
        casePipeWarnCategoriesAndSummary();
        caseBindingLifetime();
        caseRenderStatePolicy();
        caseRenderStateDomain();
        caseMaterialSource();
        return finish("d3d9-pipeline");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "DIAG uncaught std::exception: %s\n", e.what());
        return 1;
    } catch (...) {
        std::fprintf(stderr, "DIAG uncaught non-std exception\n");
        return 1;
    }
}
