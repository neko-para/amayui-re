// emulator-lift · GPU 路线 P0/P1：effect 资源字节 + `D3DXCreateEffectFromResourceA` + ID3DXEffect 9 槽
//
// ★ 判据（红了意味着什么）：
//   ① `d3dx9-effect/caps-ps-version`（P4）：`GetDeviceCaps` 写进 guest 的**四个 shader 字段**是
//      固定值 —— `PixelShaderVersion=0xFFFF0200` / `VertexShaderVersion=0xFFFE0100` /
//      `MaxVertexShaderConst=96` / `MaxPixelShaderValue=8.0f`。P1 的开发 opt-in（读 env 那个）已删 ⇒
//      这里不再有"值域 / 档位"，只有"声明 == 真实实现能力"。
//   ①b `d3dx9-effect/caps-programs-guard`（P4）：caps 会让引擎建的 effect program 集必须**全部**
//      在实现集里（硬等式）。从实现集里拿掉 202 的 `SnowPS`（或 `SnowVS`）⇒ **启动即 Trap**，
//      且 `why` 点名 missing（⛔ 不是 warn / gap / 白名单）。
//   ② `d3dx9-effect/unrecorded-loud-stop`：**自造**一张最小 PE（RT_RCDATA id=201 + 不在收录集的字节）
//      ⇒ `readEffectSource` 必须答 `Unrecorded`，`why` 里必须同时出现 **id / 字节的 sha256 / 前 32 字节**；
//      经 shim 走一遍 ⇒ 机器**停**（`Trap` + `unimplemented_calls` +1）且**不留** COM 对象
//      （⛔ 不返回看起来成功的句柄）。★ 这条不需要语料。
//   ③ `d3dx9-effect/resources-pinned`（要 `LIFT_AGE_EXE`）：拆壳件的 RT_RCDATA **恰好 4 项**
//      （200/201/202/203；多一项/少一项都红），每项的**大小 + 首两字节 `//` + sha256** 等于
//      本文件的期望值（期望值是**独立抄的**读数，⛔ 不从实现里取 —— 否则断言恒真）。
//   ④ `d3dx9-effect/object-slots`（要 `LIFT_AGE_EXE`）：`D3DXCreateEffectFromResourceA(pSrcResource=201)`
//      ⇒ S_OK + 非 0 句柄；SetFloat/SetTexture/SetTechnique 记进状态；Begin/BeginPass/CommitChanges/
//      EndPass/End 只推进计数并写 `*pPasses = 1`；Release 到 0 ⇒ 对象 + 状态块 + 参数表全部回收
//      （guest 堆回到基线）；QueryInterface(IUnknown) 成功、别的 IID ⇒ E_NOINTERFACE；
//      **没取证的槽**（+0x110 GetDevice）⇒ `comUnsupported` 的 loud trap（机器停 + 具名原因）。
//   ★ P2（`REQ-01M7MSGRRHCA23RAX1QGDAYJ3X`）追加的判据：
//   ⑤ `d3dx9-effect/fx-parse`：4 份 FX 文本的 technique/pass/profile/常量/sampler/形参语义逐条读出，
//      **没建模的 pass 状态 / sampler_state 键 ⇒ `UnsupportedState` + 点名**。
//   ⑥ `d3dx9-effect/effect-draw-recorded`：Fade（201）的 draw **不再被拒绝** ⇒ 后端收到一条 effect
//      队列条目（程序 / 常量表 / 两张纹理 / 第二套 UV 都是文本说的那一份）；出 pass 后回到固定功能。
//      ★ 这是 P1 用例 `d3dx9-effect/effect-draw-refused` 的**翻面**（那条"必须拒绝"的断言已删）。
//   ⑦（已退役）P2/P3 的 `d3dx9-effect/effect-draw-unimplemented`：202 当时**具名拒绝**（bit 44）。
//      ★ A 单把这条整条翻面成 `effect-draw-snow-recorded`（见 ⑪）——202 真的画。
//      ★ P4 又把"运行期撞到未实现 program"整个改成 `pipelineTrap()`（见 ⑦b）：⛔ 位 44 不再
//      承担这条语义。
//   ⑦b `d3dx9-effect/effect-draw-unimplemented-traps`（P4）：运行期 effect draw 撞到**未实现的
//      program** ⇒ `pipelineTrap()` 响亮停（stderr 现场含资源 / technique / entry / profile），
//      ⛔ 不再走 P1 的 "warn + 0 像素 + 位 44 继续跑"。
//   ⑧ `d3dx9-effect/fade-pixels`（要 GPU）：两张已知纹理 + `Alpha ∈ {0,0.5,1}` 的像素 oracle
//      （容差 ≤1 LSB/通道）+ 「两图全透明 ⇒ `ret.a==0` ⇒ 目标逐字节不变」的**不可观测**档。
//   ★ P3（同一张单，203 的两个 `ps_2_0` technique）追加的判据：
//   ⑨ `d3dx9-effect/effect-draw-blur-recorded`（pure）：203 的 SlideBlur / ZoomBlur **不再被拒**
//      ——后端收到的程序 / 6 个常量（`Length,Width,Height,Angle,CenterU,CenterV` 的**声明顺序**）/
//      1 张纹理 / TEXCOORD0 / sampler = Clamp+Linear 都是文本说的那一份。
//   ⑩ `d3dx9-effect/blur-pixels`（要 GPU）：16×16 已知纹理 + 真 GPU + RT 读回，**两个 technique**
//      的输出与 C++ 参考（逐句照 203 的源文本：SlideBlur **21 tap** × 0.0476、ZoomBlur **10 tap**
//      × `ratio[10]`，双线性 + CLAMP）比（容差 ≤1 LSB/通道）；Angle=0/90 两档分别钉住
//      **步长方向**（cos/sin）与 **uv 步长**（`Length/max(W,H)*0.05`）。
//   ★ A 单（`REQ-01M7NAW2QT3TG9RCFMKR052C83`：202 = `SetMatrix("Proj")` + `SetVector("Color")`
//     + CPU `vs_1_0` + `ps_1_0 SnowPS`）追加的判据：
//   ⑪ `d3dx9-effect/effect-draw-snow-recorded`（pure）：202 ⇒ recorded（`has_vs` / `SnowPs10` /
//      `Color` 展开成 4 个常量 / `Proj` 的 16 个 float 进 `vs_matrix`）+ `vs_2_0` 的 pass 仍具名拒绝。
//   ⑫ `d3dx9-effect/snow-vertices`（pure，Mock 记录顶点）：单位 `Proj` 与固定功能的单位变换**逐 float
//      相同**；两档 `Proj` ⇒ 落点按 `mul(pos, Proj)` 变；光照开着也不改顶点色（VS 路无光照）。
//   ⑬ `d3dx9-effect/snow-pixels`（要 GPU）：已知 16×16 纹理 × `Color` 三档 + 换 uv 一档
//      ⇒ 逐通道 `texel*Color`（容差 ≤1 LSB）；`Color={0,0,0,1}` 是控制行（全 0）。
//
// ★ 输入：`LIFT_AGE_EXE`（CMake 注入 `${AGE_SECTFIX_EXE}`；缺 ⇒ LIFT_SKIP 77）。
//   ①② 不读语料 ⇒ 没有语料时它们**已经跑过**，`SKIP` 只表示剩下两条没跑。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>  // _dup / _dup2 / _fileno / _close / _read / _write（stderr tee 用）
#else
#include <unistd.h>  // dup / dup2 / fileno / close / read / write
#endif

#include "check.h"
#include "fixture.h"
#include "runtime/d3d9.h"
#include "runtime/d3d9_effect_gpu.h"
#include "runtime/d3d9_pipeline.h"
#include "runtime/gpu_raster.h"
#include "runtime/math.h"  // `perspectiveFovLH`（用例里要"真的透视矩阵"，⛔ 不手抄 16 个数）
#include "runtime/present_sdl.h"
#include "runtime/d3dx9_sprite.h"  // HRESULT 常量（kOk / kInvalidCall / kNoInterface / kFail）
#include "runtime/exec.h"
#include "runtime/image.h"
#include "runtime/machine.h"
#include "runtime/pe_resource.h"
#include "runtime/sha256.h"
#include "shims/d3dx9_effect.h"
#include "shims/register.h"

namespace {

using lift::d3dx9::EffectResourcePin;
using lift::d3dx9::EffectSource;
using lift::d3dx9::EffectSourceStatus;

// ---- 期望值：独立抄自 S0 §2.3 的读数（大小 / sha256 前 16 位来自报告，完整 sha256 为
//      `shasum -a 256 .tmp/gpu-roadmap/res_200..203.bin` 的输出）。⛔ 不从实现里读。----
struct Expect {
    std::uint32_t id;
    std::uint32_t size;
    const char* sha256;
};

constexpr Expect kExpected[] = {
    {200u, 892u, "678dd662f4d7bc98eb4042261dd20571f11ea70045e66f38c8548d17197f8518"},
    {201u, 1077u, "fb76f089c287e69a0928ef640ed15e1044f87ff19e3e739be2192ee2a46d419b"},
    {202u, 899u, "a948d29bc5aab3e0fa62ec551903a0f102ad56e3cd5e5943d0a8c0c4bd0473ac"},
    {203u, 1776u, "0ec494f035f7a6678b0b8aea1ec25a3ed8b4967b6084b8e4469a3e94ec5847bd"},
};

// ---------------------------------------------------------------------------------------
// 自造最小 PE32 + 一节 `.rsrc`：type RT_RCDATA(10) → id 201 → lang 1041 → data。
//   ★ 只为了"未收录 ⇒ 响亮停"这条判据（不需要语料 / GPU / Machine 的装配）。
// ---------------------------------------------------------------------------------------
constexpr std::uint32_t kPeOff = 0x40;
constexpr std::uint32_t kOptOff = kPeOff + 24;
constexpr std::uint32_t kSecOff = kOptOff + 224;
constexpr std::uint32_t kRsrcRva = 0x2000;
constexpr std::uint32_t kRsrcRaw = 0x400;

void put16(std::vector<std::uint8_t>& b, std::size_t off, std::uint16_t v) {
    std::memcpy(b.data() + off, &v, 2);
}
void put32(std::vector<std::uint8_t>& b, std::size_t off, std::uint32_t v) {
    std::memcpy(b.data() + off, &v, 4);
}

// 目录项：`rel` 相对 `.rsrc` 起点
void setDir(std::vector<std::uint8_t>& b, std::uint32_t rel, std::uint16_t named, std::uint16_t ids,
            const std::vector<std::pair<std::uint32_t, std::uint32_t>>& entries) {
    const std::size_t o = kRsrcRaw + rel;
    put16(b, o + 12, named);
    put16(b, o + 14, ids);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        put32(b, o + 16 + i * 8, entries[i].first);
        put32(b, o + 20 + i * 8, entries[i].second);
    }
}

void setDataEntry(std::vector<std::uint8_t>& b, std::uint32_t rel, std::uint32_t rva,
                  std::uint32_t size) {
    const std::size_t o = kRsrcRaw + rel;
    put32(b, o, rva);
    put32(b, o + 4, size);
    put32(b, o + 8, 1041u);  // CodePage
    put32(b, o + 12, 0u);
}

constexpr std::uint32_t kRootDir = 0x000;
constexpr std::uint32_t kTypeDir = 0x030;
constexpr std::uint32_t kLangDir = 0x060;
constexpr std::uint32_t kDataEntry = 0x090;

std::vector<std::uint8_t> makeRcDataPe(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> b(0x800, 0);
    b[0] = 'M';
    b[1] = 'Z';
    put32(b, 0x3C, kPeOff);
    b[kPeOff] = 'P';
    b[kPeOff + 1] = 'E';
    put16(b, kPeOff + 6, 1);             // NumberOfSections
    put16(b, kPeOff + 20, 224);          // SizeOfOptionalHeader
    put16(b, kOptOff, 0x010B);           // PE32
    put32(b, kOptOff + 16, 0x2000);      // AddressOfEntryPoint
    put32(b, kOptOff + 28, 0x00400000);  // ImageBase
    put32(b, kOptOff + 56, 0x3000);      // SizeOfImage
    put32(b, kOptOff + 60, 0x400);       // SizeOfHeaders
    put32(b, kOptOff + 72, 0x100000);    // SizeOfStackReserve
    // DataDirectory[2] = 资源目录（RVA 0x2000 / size 0x400）；导入表目录保持 0
    put32(b, kOptOff + 96 + 8 * 2, kRsrcRva);
    put32(b, kOptOff + 96 + 8 * 2 + 4, 0x400);
    std::memcpy(b.data() + kSecOff, ".rsrc", 5);
    put32(b, kSecOff + 8, 0x400);         // VirtualSize
    put32(b, kSecOff + 12, kRsrcRva);     // VirtualAddress
    put32(b, kSecOff + 16, 0x400);        // SizeOfRawData
    put32(b, kSecOff + 20, kRsrcRaw);     // PointerToRawData
    put32(b, kSecOff + 36, 0x40000040);   // INITIALIZED_DATA | READ

    // 三层：type 10 → id 201 → lang 1041 → data entry
    setDir(b, kRootDir, 0, 1, {{lift::d3dx9::kRtRcData, 0x80000000u | kTypeDir}});
    setDir(b, kTypeDir, 0, 1, {{201u, 0x80000000u | kLangDir}});
    setDir(b, kLangDir, 0, 1, {{1041u, kDataEntry}});
    const std::uint32_t payload_rel = 0x200u;
    setDataEntry(b, kDataEntry, kRsrcRva + payload_rel, static_cast<std::uint32_t>(payload.size()));
    std::memcpy(b.data() + kRsrcRaw + payload_rel, payload.data(), payload.size());
    return b;
}

// ---------------------------------------------------------------------------------------
// guest 侧的 COM 调用：`mov eax,[vtbl]; push args…; push obj; call [eax+off]; ret`
// ---------------------------------------------------------------------------------------
struct GuestAsm {
    std::vector<std::uint8_t> b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFFu));
    }
    void pushImm(std::uint32_t v) {
        u8(0x68);
        u32(v);
    }
    void movEaxImm(std::uint32_t v) {
        u8(0xB8);
        u32(v);
    }
    void callMemEax(std::uint32_t off) {
        if (off <= 0x7Fu) {
            u8(0xFF);
            u8(0x50);
            u8(static_cast<std::uint8_t>(off));
        } else {
            u8(0xFF);
            u8(0x90);
            u32(off);
        }
    }
    void ret() { u8(0xC3); }
};

std::uint32_t placeAndRun(lift_fixture::Fixture& f, GuestAsm& a, bool* stopped_ok = nullptr) {
    const std::uint32_t code_ea = f.code_cursor;
    f.code_cursor += 0x100u;
    if (!f.m.placeCode(code_ea, a.b.data(), a.b.size(), &f.err)) {
        f.ok = false;
        return 0;
    }
    f.rearmStack();
    f.m.cpu().eip = code_ea;
    lift::RunOptions o;
    o.until_ea = lift_fixture::kSentinel;
    o.max_insns = 200000;
    const lift::RunResult r = f.m.run(o);
    if (stopped_ok != nullptr) {
        *stopped_ok = r.reason != lift::StopReason::UntilEa;
    } else if (r.reason != lift::StopReason::UntilEa) {
        f.ok = false;
        f.err = std::string("意外停机：") + lift::stopReasonName(r.reason) + " — " + r.detail;
    }
    return f.m.cpu().eax();
}

std::uint32_t callMethod(lift_fixture::Fixture& f, std::uint32_t obj, std::uint32_t off,
                         std::initializer_list<std::uint32_t> args) {
    const std::uint32_t vtbl = f.m.mem().rd32(obj);
    GuestAsm a;
    a.movEaxImm(vtbl);
    const std::vector<std::uint32_t> v(args);
    for (auto it = v.rbegin(); it != v.rend(); ++it) a.pushImm(*it);
    a.pushImm(obj);
    a.callMemEax(off);
    a.ret();
    return placeAndRun(f, a);
}

std::uint32_t floatBits(float v) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    return bits;
}

// ---- stderr **tee**（"响亮停真的把 program / technique / entry / profile 打出来"的判据用）----
// 形态照 `tests/d3d9_pipeline.cpp` / `tests/dsound.cpp`：只捕获不回显会让中途 trap 的现场全丢；
// `finish()` 把新增的一段同时写进内存 log 与真 stderr。
struct StderrTee {
#ifdef _WIN32
    using FdT = int;
    static FdT dupFd(int fd) { return _dup(fd); }
    static int dup2Fd(int src, int dst) { return _dup2(src, dst); }
    static int filenoOf(std::FILE* f) { return _fileno(f); }
    static void closeFd(FdT fd) { _close(fd); }
    static int writeFd(FdT fd, const void* buf, unsigned n) { return _write(fd, buf, n); }
#else
    using FdT = int;
    static FdT dupFd(int fd) { return ::dup(fd); }
    static int dup2Fd(int src, int dst) { return ::dup2(src, dst); }
    static int filenoOf(std::FILE* f) { return ::fileno(f); }
    static void closeFd(FdT fd) { ::close(fd); }
    static int writeFd(FdT fd, const void* buf, unsigned n) {
        return static_cast<int>(::write(fd, buf, n));
    }
#endif
    std::FILE* tmp = nullptr;
    FdT saved = -1;
    bool active = false;
    std::string log;

    bool begin() {
        tmp = std::tmpfile();
        if (!tmp) return false;
        std::fflush(stderr);
        saved = dupFd(2);
        if (saved < 0) return false;
        if (dup2Fd(filenoOf(tmp), 2) < 0) return false;
        active = true;
        return true;
    }
    std::string finish() {
        if (!active) return log;
        std::fflush(stderr);
        std::fseek(tmp, 0, SEEK_END);
        const long size = std::ftell(tmp);
        if (size > 0) {
            std::fseek(tmp, 0, SEEK_SET);
            std::vector<char> chunk(static_cast<std::size_t>(size));
            const std::size_t got = std::fread(chunk.data(), 1, chunk.size(), tmp);
            log.append(chunk.data(), got);
            std::size_t off = 0;
            while (off < got) {
                const int n = writeFd(saved, chunk.data() + off, static_cast<unsigned>(got - off));
                if (n <= 0) break;
                off += static_cast<std::size_t>(n);
            }
        }
        dup2Fd(saved, 2);
        closeFd(saved);
        saved = -1;
        active = false;
        std::fclose(tmp);
        tmp = nullptr;
        return log;
    }
};

// 一套"能建 COM 对象"的最小夹具（heap + 桩页 + d3dx9 注册）
bool setupMachine(lift_fixture::Fixture& f) {
    if (!f.ok) return false;
    if (!f.m.heap().init(&f.m.mem(), f.m.layout().heap_base, 0x00100000u)) return false;
    f.m.shims().configureStubPage(0x7FF80000u, 0x1000u);
    lift::registerD3D9Shims(f.m.shims());
    lift::registerD3DX9Shims(f.m.shims());
    return f.m.shims().findMutable("d3dx9_43", "D3DXCreateEffectFromResourceA") != nullptr &&
           f.m.shims().findMutable("d3d9", "Direct3DCreate9") != nullptr;
}

// 定义在 ⑤ 之后（这里先声明：caps 用例也要走 createDevice → GetDeviceCaps 的真路径）。
bool createDevice(lift_fixture::Fixture& f, std::uint32_t d3d9, std::uint32_t* out);

// =======================================================================================
// ① caps 的 shader 声明（P4）：**固定四个值，没有 env / 没有档位**
//
// 红了意味着什么：`fillCaps` 又一次与真实实现能力脱节 —— 要么少报（引擎走软件模糊 / 转场 40+ s
//   黑屏；缺省 caps=0 的形态），要么多报（引擎去建本线兑付不了的 shader）。P4 已把
//   P1 的开发 opt-in（读 env 那个）已整条删掉 ⇒ 这条不再测"值域 / 映射"，只测**写进 guest 的四个字段**。
// 判据：
//   ① `GetDeviceCaps` 返回 S_OK、机器没停（caps 声明与实现集的等式由 `caps-programs-guard` 判）；
//   ② +0xCC / +0xC4 / +0xC8 / +0xD0 逐字段等于 `d3d9.h` 的声明值（⛔ 测试不另抄数字），
//      且与 D3DPS_VERSION(2,0) / D3DVS_VERSION(1,0) 的字面值一致（这是**产品口径**，不是读数）；
//   ③ caps 块之外的 canary 不被写。
// =======================================================================================
void caseCapsPsVersion() {
    lift_test::g_current_case = "d3dx9-effect/caps-ps-version";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    LIFT_CHECK(setupMachine(f));
    const std::uint32_t proc3d = f.use("d3d9", "Direct3DCreate9");
    LIFT_CHECK_NE_U32(proc3d, 0u);
    if (proc3d == 0) return;
    const std::uint32_t d3d9 = f.call(proc3d, {lift::d3d9::kSdkVersion});
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    std::uint32_t device = 0;
    LIFT_CHECK(createDevice(f, d3d9, &device));
    if (device == 0) return;

    // 0xD4 之后留 4 B canary（两次不同初值 ⇒ 既能看"全写"也能看"越界不写"）
    const std::vector<std::uint8_t> caps_init(lift::d3d9::kCapsBytes + 4u, 0xCCu);
    const std::uint32_t caps_ea = f.data(caps_init.data(), caps_init.size());
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x1Cu, {caps_ea}), lift::d3d9::kOk);
    LIFT_CHECK(f.m.cpu().running);

    const std::uint32_t ps = f.m.mem().rd32(caps_ea + lift::d3d9::kCapsPixelShaderVersion);
    const std::uint32_t vs = f.m.mem().rd32(caps_ea + lift::d3d9::kCapsVertexShaderVersion);
    const std::uint32_t max_vs_const = f.m.mem().rd32(caps_ea + lift::d3d9::kCapsMaxVertexShaderConst);
    const std::uint32_t max_ps_bits = f.m.mem().rd32(caps_ea + lift::d3d9::kCapsMaxPixelShaderValue);
    float max_ps = 0.0f;
    std::memcpy(&max_ps, &max_ps_bits, sizeof(max_ps));

    // ② 逐字段 == 声明值（唯一一份在 `d3d9.h`）
    LIFT_CHECK_EQ_U32(ps, lift::d3d9::kCapsPixelShaderVersionClaim);
    LIFT_CHECK_EQ_U32(vs, lift::d3d9::kCapsVertexShaderVersionClaim);
    LIFT_CHECK_EQ_U32(max_vs_const, lift::d3d9::kCapsMaxVertexShaderConstClaim);
    LIFT_CHECK(max_ps == lift::d3d9::kCapsMaxPixelShaderValueClaim);
    // 字面值（D3D 版本常量的产品口径；⛔ 不是"当前读数"）
    LIFT_CHECK_EQ_U32(ps, 0xFFFF0200u);   // D3DPS_VERSION(2,0)
    LIFT_CHECK_EQ_U32(vs, 0xFFFE0100u);   // D3DVS_VERSION(1,0)
    LIFT_CHECK_EQ_U32(max_vs_const, 96u); // vs_1_0 的 c0..c95（自洽值，caps.md §7 第 1 条）
    LIFT_CHECK(max_ps == 8.0f);           // ps_2_0 的常见上限（自洽值，caps.md §7 第 4 条）
    // ③ 越界不写
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(caps_ea + lift::d3d9::kCapsBytes), 0xCCCCCCCCu);
    std::printf("caps（P4 固定）：PixelShaderVersion=0x%08X VertexShaderVersion=0x%08X "
                "MaxVertexShaderConst=%u MaxPixelShaderValue=%g\n",
                ps, vs, max_vs_const, static_cast<double>(max_ps));
}

// =======================================================================================
// ② 未收录 ⇒ 响亮停（自造 PE；不读语料）
// =======================================================================================
void caseUnrecorded() {
    lift_test::g_current_case = "d3dx9-effect/unrecorded-loud-stop";
    const std::string payload =
        "// NOT a recorded effect: this is a self-made RT_RCDATA for the loud-stop guard\n";
    const std::vector<std::uint8_t> bytes(payload.begin(), payload.end());
    lift::PeImage img;
    img.bytes = makeRcDataPe(bytes);
    std::string err;
    LIFT_CHECK(lift::parsePeImage(img, &err));
    if (!err.empty()) std::printf("parsePeImage: %s\n", err.c_str());

    // ① 读取器本身
    EffectSource src;
    std::string why;
    const EffectSourceStatus st = lift::d3dx9::readEffectSource(img, 201u, &src, &why);
    LIFT_CHECK(st == EffectSourceStatus::Unrecorded);
    LIFT_CHECK_EQ_U32(src.size, static_cast<std::uint32_t>(bytes.size()));
    LIFT_CHECK(!src.pin);
    const std::string want_sha = lift::toHex(lift::sha256(src.bytes));
    LIFT_CHECK(src.sha256 == want_sha);
    LIFT_CHECK(why.find("id=201") != std::string::npos);
    LIFT_CHECK(why.find(want_sha) != std::string::npos);
    LIFT_CHECK(why.find("前 32 字节:") != std::string::npos);
    LIFT_CHECK(why.find("2f 2f 20 4e 4f 54") != std::string::npos);  // "// NOT"
    std::printf("unrecorded why = %s\n", why.c_str());
    // 缺 id 也是 Missing（不是 Unrecorded；找不到字节就没有 sha 可打）
    EffectSource missing;
    std::string why2;
    LIFT_CHECK(lift::d3dx9::readEffectSource(img, 200u, &missing, &why2) == EffectSourceStatus::Missing);
    LIFT_CHECK(why2.find("id=200") != std::string::npos);

    // ② 经 shim 走一遍：机器必须停、对象表必须空、unimplemented 计数 +1
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    LIFT_CHECK(setupMachine(f));
    f.m.cpu().image = &img;
    const std::uint32_t proc = f.use("d3dx9_43", "D3DXCreateEffectFromResourceA");
    LIFT_CHECK_NE_U32(proc, 0u);
    const std::uint32_t out_ea = f.u32data(0xDEADBEEFu);
    const std::uint32_t hr = f.call(proc, {0x101C68A8u, 0u, 201u, 0u, 0u, 0u, 0u, out_ea, 0u});
    LIFT_CHECK_EQ_U32(hr, lift::d3dx9::kFail);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(out_ea), 0u);  // ⛔ 不留"看起来成功"的句柄
    LIFT_CHECK(!f.m.cpu().running);
    LIFT_CHECK(f.m.cpu().stop_why != nullptr && std::strcmp(f.m.cpu().stop_why, "unimplemented D3DX effect") == 0);
    LIFT_CHECK(f.m.cpu().stop_kind == lift::HostStopKind::Trap);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(f.m.cpu().shim.unimplemented_calls), 1u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(f.m.cpu().com->objectCount()), 0u);
}

// =======================================================================================
// ③ 4 份资源的字节（拆壳件；要 LIFT_AGE_EXE）
// =======================================================================================
void caseResources(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/resources-pinned";
    lift::PeResources res;
    std::string err;
    LIFT_CHECK(res.init(img, &err));
    if (!res.hasResources()) {
        std::fprintf(stderr, "资源目录解析失败：%s\n", err.c_str());
        return;
    }
    // ① RT_RCDATA **恰好 4 项**，且就是 200/201/202/203（多一项 ⇒ "4 个 id 之外还有 RCDATA"）
    std::vector<lift::PeResourceName> names;
    LIFT_CHECK(res.listNames(lift::d3dx9::kRtRcData, &names, &err));
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(names.size()), 4u);
    std::printf("RT_RCDATA 共 %zu 项：", names.size());
    for (const lift::PeResourceName& n : names) std::printf(" %u", n.id);
    std::printf("\n");
    for (const Expect& e : kExpected) {
        bool found = false;
        for (const lift::PeResourceName& n : names) {
            if (!n.is_name && n.id == e.id) found = true;
        }
        LIFT_CHECK(found);
    }
    for (const lift::PeResourceName& n : names) {
        bool known = false;
        for (const Expect& e : kExpected) {
            if (!n.is_name && n.id == e.id) known = true;
        }
        LIFT_CHECK(known);  // ⛔ 4 个 id 之外不许有 RCDATA
    }
    // ② 每个 id 的字节：大小 / 首两字节 `//` / sha256
    for (const Expect& e : kExpected) {
        EffectSource src;
        std::string why;
        const EffectSourceStatus st = lift::d3dx9::readEffectSource(img, e.id, &src, &why);
        LIFT_CHECK(st == EffectSourceStatus::Ok);
        if (st != EffectSourceStatus::Ok) {
            std::fprintf(stderr, "id=%u: %s\n", e.id, why.c_str());
            continue;
        }
        LIFT_CHECK_EQ_U32(src.size, e.size);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(src.bytes.size()), e.size);
        LIFT_CHECK(src.size >= 2u && src.bytes[0] == '/' && src.bytes[1] == '/');
        LIFT_CHECK(src.sha256 == e.sha256);
        // 再从字节算一遍（同一条判据的**两个独立来源**：实现路径 vs 直接 sha256）
        LIFT_CHECK(lift::toHex(lift::sha256(src.bytes)) == e.sha256);
        LIFT_CHECK(src.pin != nullptr);
        if (src.pin != nullptr) {
            LIFT_CHECK_EQ_U32(src.pin->id, e.id);
            LIFT_CHECK_EQ_U32(src.pin->size, e.size);
        }
        std::printf("id=%u %u B sha256=%s ✓\n", e.id, src.size, src.sha256.c_str());
    }
    // ③ 收录表与期望值一致（4 条 + 终止项）+ 不认识的 id 必须 Missing
    const EffectResourcePin* pins = lift::d3dx9::effectResourcePins();
    LIFT_CHECK(pins != nullptr);
    if (pins != nullptr) {
        for (std::size_t i = 0; i < 4; ++i) {
            LIFT_CHECK_EQ_U32(pins[i].id, kExpected[i].id);
            LIFT_CHECK_EQ_U32(pins[i].size, kExpected[i].size);
            LIFT_CHECK(std::string(pins[i].sha256) == kExpected[i].sha256);
        }
        LIFT_CHECK_EQ_U32(pins[4].id, 0u);  // 终止项
    }
    for (std::uint32_t bad : {199u, 204u, 0xFFFFu}) {
        EffectSource s;
        std::string why;
        LIFT_CHECK(lift::d3dx9::readEffectSource(img, bad, &s, &why) == EffectSourceStatus::Missing);
    }
}

// =======================================================================================
// ⑤ effect 绑定 + shader 未执行 ⇒ **整批拒绝 + 具名记账**（P1 的临时判据；P2 执行 shader 后翻面）
//
// ★ 为什么这条必须存在：不拒的话这笔 draw 会落到固定功能管线并**写出错误像素**，而账本上
//   **一个具名位都没有**（= 本仓最贵的静默错）。实测依据：E2E 里 203 的 blur draw 只绑 Tex0
//   ⇒ bit33 不触发 ⇒ 修前它被"接受"。
// 判据：① draw 返回 D3DERR_INVALIDCALL；② `skipped_draws +1` 且**原因串**含 id/technique/pass；
//   ③ `PipelineInfo::unsupported` 的 bit44 置位；④ EndPass/End 之后同一笔 draw **不再**记这条原因
//   （⇒ 拒绝是**作用域相关**的，不是"所有 draw 都拒"）。
// =======================================================================================
bool createDevice(lift_fixture::Fixture& f, std::uint32_t d3d9, std::uint32_t* out) {
    std::vector<std::uint8_t> pp(lift::d3d9::kPresentParamsBytes, 0);
    auto wr = [&pp](std::uint32_t off, std::uint32_t v) {
        for (int i = 0; i < 4; ++i) pp[off + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    };
    wr(lift::d3d9::kPpBackBufferWidth, 320u);
    wr(lift::d3d9::kPpBackBufferHeight, 240u);
    wr(lift::d3d9::kPpBackBufferFormat, lift::d3d9::kFmtX8R8G8B8);
    wr(lift::d3d9::kPpBackBufferCount, 1u);
    wr(lift::d3d9::kPpSwapEffect, 1u);
    wr(lift::d3d9::kPpHDeviceWindow, 0x00001234u);
    wr(lift::d3d9::kPpWindowed, 1u);
    wr(lift::d3d9::kPpEnableAutoDepthStencil, 1u);
    wr(lift::d3d9::kPpAutoDepthStencilFormat, 75u);
    wr(lift::d3d9::kPpFlags, lift::d3d9::kPresentFlagLockableBackbuffer);
    wr(lift::d3d9::kPpPresentationInterval, 0x80000000u);
    const std::uint32_t pp_ea = f.data(pp.data(), pp.size());
    const std::uint32_t out_ea = f.u32data(0u);
    const std::uint32_t hr = callMethod(f, d3d9, 0x40u, {0u, 1u, 0x00001234u, 0x44u, pp_ea, out_ea});
    if (hr != lift::d3d9::kOk) return false;
    *out = f.m.mem().rd32(out_ea);
    return *out != 0;
}

// =======================================================================================
// ①b caps ⇒ 实现集的**硬等式**（P4）：缺一个 ⇒ **启动即 Trap**，且 `why` 点名 missing
//
// 红了意味着什么：caps 声明了"我会 ps_2_0 / vs_1_0"，而实现集里少一条 ⇒ 引擎会在运行期去建 /
//   提交一个本线画不出来的 program（声明与能力分叉）。这条必须**启动时**红，⛔ 不能拖到某一帧
//   才停、⛔ 也不能 warn 之后继续跑。
// 判据：
//   ① **生产路径**（真 `GetDeviceCaps` + 真表）⇒ S_OK、机器仍在跑、守卫返回 true；
//   ② 从实现集里拿掉 202 的 `SnowPS / ps_1_0` ⇒ 守卫返回 false、`stop_kind == Trap`、
//      `stop_why` = "caps 实现集守卫"，`why` 里**点名** `missing` / id=202 / technique=Snow /
//      SnowPS / ps_1_0；
//   ③ 拿掉 `SnowVS / vs_1_0` ⇒ 同样 Trap 且点名 `vertex vs_1_0 SnowVS`（VS 那一半也钉住）；
//   ④ ②③ 的 required 都来自**同一份 pin 资源字节**（要 LIFT_AGE_EXE）⇒ ⛔ 不是手抄的期望表。
// =======================================================================================
void caseCapsProgramsGuard(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/caps-programs-guard";
    // 实现集快照（唯一来源 = 运行时那两张表；⛔ 用例不手抄门牌）
    const lift::d3dx9::EffectCapsImplementedForTest full = lift::d3dx9::effectCapsImplemented();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(full.pixel.size()), 5);   // 200/201/202/203×2
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(full.vertex.size()), 1);  // SnowVS/vs_1_0

    // ① 生产路径：真 `GetDeviceCaps`（`fillCaps` 之后紧跟守卫）⇒ 不 Trap
    {
        lift_fixture::Fixture f;
        LIFT_CHECK(f.ok);
        if (!f.ok) return;
        LIFT_CHECK(setupMachine(f));
        f.m.cpu().image = &img;
        const std::uint32_t proc3d = f.use("d3d9", "Direct3DCreate9");
        const std::uint32_t d3d9 = f.call(proc3d, {lift::d3d9::kSdkVersion});
        LIFT_CHECK_NE_U32(d3d9, 0u);
        if (d3d9 == 0) return;
        std::uint32_t device = 0;
        LIFT_CHECK(createDevice(f, d3d9, &device));
        if (device == 0) return;
        const std::uint32_t caps_ea = f.zeroData(lift::d3d9::kCapsBytes);
        LIFT_CHECK_EQ_U32(callMethod(f, device, 0x1Cu, {caps_ea}), lift::d3d9::kOk);
        LIFT_CHECK(f.m.cpu().running);
        std::string why;
        LIFT_CHECK(lift::d3dx9::effectCapsGuard(f.m.cpu(), caps_ea, &why));
        LIFT_CHECK(f.m.cpu().running);
        LIFT_CHECK(why.empty());
    }

    // ② 拿掉 202 的 PS binding ⇒ 启动 Trap + 点名 missing
    {
        lift_fixture::Fixture f;
        LIFT_CHECK(f.ok);
        if (!f.ok) return;
        LIFT_CHECK(setupMachine(f));
        f.m.cpu().image = &img;
        const std::uint32_t caps_ea = f.zeroData(lift::d3d9::kCapsBytes);
        std::string why;
        LIFT_CHECK_EQ_U32(lift::d3d9::fillCaps(f.m.cpu(), caps_ea, &why), lift::d3d9::kOk);
        lift::d3dx9::EffectCapsImplementedForTest impl = full;
        bool removed = false;
        for (std::size_t i = 0; i < impl.pixel.size(); ++i) {
            if (impl.pixel[i].resource_id == lift::d3dx9::kEffectIdSnowPs10) {
                impl.pixel.erase(impl.pixel.begin() + static_cast<std::ptrdiff_t>(i));
                removed = true;
                break;
            }
        }
        LIFT_CHECK(removed);
        LIFT_CHECK(!lift::d3dx9::effectCapsGuardForTest(f.m.cpu(), caps_ea, impl, &why));
        LIFT_CHECK(!f.m.cpu().running);
        LIFT_CHECK(f.m.cpu().stop_kind == lift::HostStopKind::Trap);
        LIFT_CHECK(f.m.cpu().stop_why != nullptr &&
                   std::strcmp(f.m.cpu().stop_why, "caps 实现集守卫") == 0);
        LIFT_CHECK(why.find("missing") != std::string::npos);
        LIFT_CHECK(why.find("id=202") != std::string::npos);
        LIFT_CHECK(why.find("Snow") != std::string::npos);
        LIFT_CHECK(why.find("SnowPS") != std::string::npos);
        LIFT_CHECK(why.find("ps_1_0") != std::string::npos);
        std::printf("caps 守卫（缺 202 PS）的 missing = %s\n", why.c_str());
    }

    // ③ 拿掉 SnowVS（VS 那一半）⇒ 同样 Trap 且点名 vertex
    {
        lift_fixture::Fixture f;
        LIFT_CHECK(f.ok);
        if (!f.ok) return;
        LIFT_CHECK(setupMachine(f));
        f.m.cpu().image = &img;
        const std::uint32_t caps_ea = f.zeroData(lift::d3d9::kCapsBytes);
        std::string why;
        LIFT_CHECK_EQ_U32(lift::d3d9::fillCaps(f.m.cpu(), caps_ea, &why), lift::d3d9::kOk);
        lift::d3dx9::EffectCapsImplementedForTest impl = full;
        impl.vertex.clear();
        LIFT_CHECK(!lift::d3dx9::effectCapsGuardForTest(f.m.cpu(), caps_ea, impl, &why));
        LIFT_CHECK(!f.m.cpu().running);
        LIFT_CHECK(f.m.cpu().stop_kind == lift::HostStopKind::Trap);
        LIFT_CHECK(why.find("vertex") != std::string::npos);
        LIFT_CHECK(why.find("vs_1_0") != std::string::npos);
        LIFT_CHECK(why.find("SnowVS") != std::string::npos);
        std::printf("caps 守卫（缺 SnowVS）的 missing = %s\n", why.c_str());
    }
}


// =======================================================================================
// ⑥ P2 · FX 文本解析（资源 200/201/202/203；逐条对文本的读数）
//
// 红了意味着什么：解析器把**文本里没有的东西**读出来了（或把有的东西读丢了）——
//   之后 technique/pass/常量/纹理的绑定就全跟着错，而且是静默的（像素只是"看着不对"）。
// 判据：
//   ① 4 份资源的 technique / pass / profile / entry 与源文本逐条一致；
//   ② 常量表**按声明顺序**（201 = [Alpha]；203 = [Length,Width,Height,Angle,CenterU,CenterV]）；
//   ③ sampler_state 的六项（Texture/AddressU/AddressV/Min/Mag/Mip）读了出来（201 = LINEAR×3）；
//   ④ shader 形参的语义（TEXCOORD0/1）读了出来 —— 纹理用哪套 UV 就靠它；
//   ⑤ **没建模的 pass/sampler_state 状态名 ⇒ `UnsupportedState` + 点名**（⛔ 不静默忽略）。
// =======================================================================================
void caseFxParse(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/fx-parse";
    const auto module_of = [&img](std::uint32_t id) -> lift::d3dx9::FxModule {
        lift::d3dx9::EffectSource src;
        std::string why;
        lift::d3dx9::FxModule m;
        LIFT_CHECK(lift::d3dx9::readEffectSource(img, id, &src, &why) ==
                   lift::d3dx9::EffectSourceStatus::Ok);
        const std::string text(reinterpret_cast<const char*>(src.bytes.data()), src.bytes.size());
        const lift::d3dx9::FxParseStatus st = lift::d3dx9::parseEffectSource(text, &m, &why);
        if (st != lift::d3dx9::FxParseStatus::Ok) {
            lift_test::failf(__FILE__, __LINE__, "id=%u 解析失败：%s", id, why.c_str());
        }
        return m;
    };

    // ---- 201：Fade / ps_2_0 ----
    {
        const lift::d3dx9::FxModule m = module_of(201);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.constants.size()), 3u);  // Alpha / Tex0 / Tex1
        LIFT_CHECK(m.constants[0].name == "Alpha" && m.constants[0].type == "float");
        LIFT_CHECK(m.constants[1].name == "Tex0" && m.constants[1].type == "texture");
        LIFT_CHECK(m.constants[2].name == "Tex1" && m.constants[2].type == "texture");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.samplers.size()), 2u);
        LIFT_CHECK(m.samplers[0].name == "Tex0Smp" && m.samplers[0].texture == "Tex0");
        LIFT_CHECK(m.samplers[1].name == "Tex1Smp" && m.samplers[1].texture == "Tex1");
        for (const auto& smp : m.samplers) {
            LIFT_CHECK(smp.mag_filter == "LINEAR" && smp.min_filter == "LINEAR" && smp.mip_filter == "LINEAR");
            LIFT_CHECK(smp.address_u == "Wrap" && smp.address_v == "Wrap");  // 文本没写 ⇒ D3D9 缺省
        }
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.shaders.size()), 1u);
        const auto& sh = m.shaders[0];
        LIFT_CHECK(sh.name == "FadePS" && sh.return_semantic == "COLOR");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(sh.params.size()), 3u);
        LIFT_CHECK(sh.params[0].name == "Col" && sh.params[0].semantic == "COLOR0");
        LIFT_CHECK(sh.params[1].name == "Tex0UV" && sh.params[1].semantic == "TEXCOORD0");
        LIFT_CHECK(sh.params[2].name == "Tex1UV" && sh.params[2].semantic == "TEXCOORD1");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.techniques.size()), 1u);
        LIFT_CHECK(m.techniques[0].name == "Fade");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.techniques[0].passes.size()), 1u);
        const auto& pass = m.techniques[0].passes[0];
        LIFT_CHECK(pass.name == "P0");
        LIFT_CHECK(pass.has_pixel_shader && !pass.has_vertex_shader);
        LIFT_CHECK(pass.pixel_entry == "FadePS" && pass.pixel_profile == "ps_2_0");
        LIFT_CHECK(lift::d3dx9::fxPassAt(m, "Fade", 0u) != nullptr);
        LIFT_CHECK(lift::d3dx9::fxPassAt(m, "Fade", 1u) == nullptr);
        LIFT_CHECK(lift::d3dx9::fxPassAt(m, "ZoomBlur", 0u) == nullptr);
        std::printf("201: constants=%zu samplers=%zu shaders=%zu techniques=%zu（Fade/ps_2_0）\n",
                    m.constants.size(), m.samplers.size(), m.shaders.size(), m.techniques.size());
    }
    // ---- 200：同一个 Fade，ps_1_0 ----
    {
        const lift::d3dx9::FxModule m = module_of(200);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.techniques.size()), 1u);
        LIFT_CHECK(m.techniques[0].passes[0].pixel_profile == "ps_1_0");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.constants.size()), 3u);
    }
    // ---- 202：Snow（vs_1_0 + ps_1_0）----
    {
        const lift::d3dx9::FxModule m = module_of(202);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.constants.size()), 3u);  // Proj / Color / Tex
        LIFT_CHECK(m.constants[0].name == "Proj" && m.constants[0].type == "float4x4");
        LIFT_CHECK(m.constants[1].name == "Color" && m.constants[1].type == "float4");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.shaders.size()), 2u);
        LIFT_CHECK(m.shaders[0].name == "SnowVS" && m.shaders[1].name == "SnowPS");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.techniques.size()), 1u);
        const auto& pass = m.techniques[0].passes[0];
        LIFT_CHECK(pass.has_vertex_shader && pass.has_pixel_shader);
        LIFT_CHECK(pass.vertex_profile == "vs_1_0" && pass.pixel_profile == "ps_1_0");
    }
    // ---- 203：SlideBlur / ZoomBlur（两个 technique，各自一个 pass）----
    {
        const lift::d3dx9::FxModule m = module_of(203);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.constants.size()), 7u);  // 6 个 float + Tex0
        const char* want[] = {"Length", "Width", "Height", "Angle", "CenterU", "CenterV", "Tex0"};
        for (std::size_t i = 0; i < 7; ++i) LIFT_CHECK(m.constants[i].name == want[i]);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.samplers.size()), 1u);
        LIFT_CHECK(m.samplers[0].name == "SrcTexSampler" && m.samplers[0].texture == "Tex0");
        LIFT_CHECK(m.samplers[0].address_u == "Clamp" && m.samplers[0].address_v == "Clamp");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(m.techniques.size()), 2u);
        LIFT_CHECK(m.techniques[0].name == "SlideBlur" && m.techniques[1].name == "ZoomBlur");
        LIFT_CHECK(m.techniques[0].passes[0].pixel_entry == "SlideBlurPS");
        LIFT_CHECK(m.techniques[1].passes[0].pixel_entry == "ZoomBlurPS");
        std::printf("203: constants=%zu techniques=%zu（SlideBlur/ZoomBlur）\n", m.constants.size(),
                    m.techniques.size());
    }
    // ---- ⑤ 没建模的状态名 ⇒ 点名拒绝（三条：pass 状态 / sampler_state 键 / 不认识的顶层类型）----
    {
        const std::string with_pass_state =
            "float Alpha;\ntechnique T { pass P0 { ZEnable = 1; PixelShader=compile ps_2_0 F(); } }\n";
        lift::d3dx9::FxModule m;
        std::string why;
        LIFT_CHECK(lift::d3dx9::parseEffectSource(with_pass_state, &m, &why) ==
                   lift::d3dx9::FxParseStatus::UnsupportedState);
        LIFT_CHECK(why.find("ZEnable") != std::string::npos);
        LIFT_CHECK(why.find("尚未建模") != std::string::npos);

        const std::string with_sampler_key =
            "texture Tex0;\nsampler S = sampler_state { Texture=<Tex0>; AddressW = Clamp; };\n"
            "technique T { pass P0 { PixelShader=compile ps_2_0 F(); } }\n";
        LIFT_CHECK(lift::d3dx9::parseEffectSource(with_sampler_key, &m, &why) ==
                   lift::d3dx9::FxParseStatus::UnsupportedState);
        LIFT_CHECK(why.find("AddressW") != std::string::npos);

        const std::string with_unknown_type =
            "Texture2D Tex0;\ntechnique T { pass P0 { PixelShader=compile ps_2_0 F(); } }\n";
        LIFT_CHECK(lift::d3dx9::parseEffectSource(with_unknown_type, &m, &why) ==
                   lift::d3dx9::FxParseStatus::Error);
        LIFT_CHECK(why.find("Texture2D") != std::string::npos);
        std::printf("未建模状态点名：%s\n", why.c_str());
    }
}

// =======================================================================================
// ⑦ P2 · effect draw **真的执行**（翻面 P1 的"必须拒绝"）
//
// 判据（红了意味着什么）：
//   ① 在 `ID3DXEffect::Begin/BeginPass` 之内、technique=Fade（资源 201）时提交的 draw
//      **不再被拒绝**：HRESULT = S_OK、`skipped_draws` 不涨、后端收到**一条 effect 队列条目**；
//   ② 后端收到的东西**正是文本说的那一份**：program = FadePs20、常量表 = SetFloat("Alpha") 的值、
//      两张纹理 = SetTexture 记的 EA（换成像素/尺寸）、第二套 UV = 顶点的 TEXCOORD1；
//   ③ 离开 BeginPass..EndPass 之后**回到固定功能**（同一个 FVF 的 draw 不再是 effect 档）。
// ★ 用 Mock 后端（不碰 SDL/GPU）⇒ pure 档；像素正确性在 ⑧（要真 GPU）。
// =======================================================================================
class EffectMockRaster final : public lift::GpuRaster, public lift::GpuEffectRaster {
public:
    bool rasterActive() const override { return true; }
    const char* rasterName() const override { return "mock-effect"; }
    bool rasterPrepare(const lift::SurfaceFormat&, int, int, std::string*, const std::uint8_t*) override {
        return true;
    }
    bool rasterSyncFromGuest(const std::uint8_t*, std::uint32_t, int, int, std::string*) override {
        return tags.empty();
    }
    bool rasterSyncToGuest(std::uint8_t*, std::uint32_t, int, int, std::string* err) override {
        return rasterFlush(err);
    }
    bool rasterDraw(const lift::GpuRasterVertex*, std::size_t, const lift::GpuRasterState&,
                    std::string*) override {
        ++plain_draws;
        return true;
    }
    bool rasterDrawTextured(const lift::GpuRasterVertex*, std::size_t, const lift::GpuRasterTexture&,
                            const lift::GpuRasterState&, std::string*) override {
        ++plain_draws;
        return true;
    }
    bool rasterQueue(const lift::GpuRasterVertex* vertices, std::size_t count,
                     const lift::GpuRasterTexture*, const lift::GpuRasterState&, std::string*) override {
        ++plain_queued;
        // ★ A 单（`snow-vertices`）：固定功能那一批**真的交给 GPU 的顶点**也要能读
        //   （与 effect 批逐 float 对照 ⇒ 不抄绝对屏幕坐标）。
        plain_vertices.assign(vertices, vertices + count);
        tags.push_back(lift::gpuRasterBatchTag());
        return true;
    }
    bool rasterQueueEffect(const lift::GpuRasterVertex* vertices, std::size_t count, const float* uv1,
                           const lift::GpuEffectDraw& effect, const lift::GpuRasterState& state,
                           std::string*) override {
        ++effect_queued;
        last = effect;
        last_state = state;
        vertex_count = count;
        effect_vertices.assign(vertices, vertices + count);
        uv1_values.assign(uv1, uv1 + count * 2u);
        tags.push_back(lift::gpuRasterBatchTag());
        return true;
    }
    bool rasterFlush(std::string*) override {
        for (std::uint64_t tag : tags) {
            if (tag != 0) lift::gpuLedgerNoteRecordedDraw(tag);
        }
        tags.clear();
        flush_count += 1;
        return true;
    }
    bool rasterDirty() const override { return !tags.empty(); }
    void rasterDiscardTarget() override {}
    void rasterRequestDepthClear(float) override {}
    lift::GpuEffectDraw last;
    lift::GpuRasterState last_state;
    std::vector<float> uv1_values;
    std::vector<lift::GpuRasterVertex> effect_vertices;  // 最近一条 effect 批的**屏幕空间**顶点
    std::vector<lift::GpuRasterVertex> plain_vertices;   // 最近一条固定功能批的顶点
    std::size_t vertex_count = 0;
    std::uint32_t effect_queued = 0;
    std::uint32_t plain_queued = 0;
    std::uint32_t plain_draws = 0;
    std::uint32_t flush_count = 0;
    std::vector<std::uint64_t> tags;
};

// 一张 32bpp 纹理（D3DXCreateTexture：w/h/1 level/usage/format/pool 0/&out）
std::uint32_t createTexture(lift_fixture::Fixture& f, std::uint32_t proc, std::uint32_t device, std::uint32_t w,
                            std::uint32_t h, std::uint32_t usage, std::uint32_t format, std::uint32_t* out) {
    const std::uint32_t out_ea = f.u32data(0xDEADBEEFu);
    const std::uint32_t hr = f.call(proc, {device, w, h, 1u, usage, format, 0u, out_ea});
    *out = f.m.mem().rd32(out_ea);
    return hr;
}

// 把这张纹理的 level-0 像素拷成 BGRA 行（w*4 紧密），只读
std::vector<std::uint8_t> texturePixels(lift_fixture::Fixture& f, std::uint32_t texture, int* w, int* h) {
    lift::d3d9::TextureInfo ti;
    std::vector<std::uint8_t> out;
    if (!lift::d3d9::textureInfo(f.m.cpu(), texture, &ti, nullptr)) return out;
    lift::GuestSurface* gs = f.m.surfaces().find(ti.arena);
    if (gs == nullptr) return out;
    *w = static_cast<int>(gs->width);
    *h = static_cast<int>(gs->height);
    const std::uint8_t* px = f.m.surfaces().pixels(*gs);
    out.assign(px, px + static_cast<std::size_t>(gs->pitch) * gs->height);
    return out;
}

// 纹理 level 0 的 surface（IDirect3DTexture9::GetSurfaceLevel，槽 +0x48）
std::uint32_t textureSurface(lift_fixture::Fixture& f, std::uint32_t texture) {
    const std::uint32_t out_ea = f.u32data(0xDEADBEEFu);
    const std::uint32_t hr = callMethod(f, texture, 0x48u, {0u, out_ea});
    LIFT_CHECK_EQ_U32(hr, 0u);
    return f.m.mem().rd32(out_ea);
}

void caseEffectDrawRecorded(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/effect-draw-recorded";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    LIFT_CHECK(setupMachine(f));
    f.m.cpu().image = &img;

    const std::uint32_t proc3d = f.use("d3d9", "Direct3DCreate9");
    const std::uint32_t d3d9 = f.call(proc3d, {lift::d3d9::kSdkVersion});
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    std::uint32_t device = 0;
    LIFT_CHECK(createDevice(f, d3d9, &device));
    if (device == 0) return;

    EffectMockRaster mock;
    f.m.setGpuRaster(&mock);

    // 两张纹理（内容只要是非空 32bpp；像素值在 ⑧ 的 oracle 里才是判据）
    const std::uint32_t proc_tex = f.use("d3dx9_43", "D3DXCreateTexture");
    std::uint32_t tex0 = 0;
    std::uint32_t tex1 = 0;
    LIFT_CHECK_EQ_U32(createTexture(f, proc_tex, device, 2u, 1u, 0u, lift::d3d9::kFmtA8R8G8B8, &tex0),
                      lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(createTexture(f, proc_tex, device, 2u, 1u, 0u, lift::d3d9::kFmtA8R8G8B8, &tex1),
                      lift::d3dx9::kOk);
    LIFT_CHECK_NE_U32(tex0, 0u);
    LIFT_CHECK_NE_U32(tex1, 0u);
    if (tex0 == 0 || tex1 == 0) return;

    // effect（201 = Fade ps_2_0）
    const std::uint32_t proc_fx = f.use("d3dx9_43", "D3DXCreateEffectFromResourceA");
    const std::uint32_t out_ea = f.u32data(0);
    LIFT_CHECK_EQ_U32(f.call(proc_fx, {device, 0u, 201u, 0u, 0u, 0u, 0u, out_ea, 0u}), lift::d3dx9::kOk);
    const std::uint32_t effect = f.m.mem().rd32(out_ea);
    LIFT_CHECK_NE_U32(effect, 0u);
    if (effect == 0) return;

    const std::uint32_t s_fade = f.cstr("Fade");
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xE8u, {s_fade}), lift::d3dx9::kOk);
    const std::uint32_t s_alpha = f.cstr("Alpha");
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_alpha, floatBits(0.5f)}), lift::d3dx9::kOk);
    const std::uint32_t s_tex0 = f.cstr("Tex0");
    const std::uint32_t s_tex1 = f.cstr("Tex1");
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xD0u, {s_tex0, tex0}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xD0u, {s_tex1, tex1}), lift::d3dx9::kOk);
    // Begin 报**解析出来的** pass 数（Fade 文本里恰好 1 个 pass）
    const std::uint32_t passes_ea = f.u32data(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xFCu, {passes_ea, 0u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(passes_ea), 1u);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x100u, {0u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x104u, {}), lift::d3dx9::kOk);

    // 作用域里上报的画法快照：程序 + 常量 + 两张纹理 + UV 集
    {
        const lift::d3d9::EffectDrawScope sc = lift::d3d9::effectDrawScope();
        LIFT_CHECK(sc.active && sc.in_pass);
        if (!sc.program.implemented) {
            std::fprintf(stderr, "（未实现的原因）= %s\n", sc.program.reason.c_str());
        }
        LIFT_CHECK(sc.program.implemented);
        LIFT_CHECK(sc.program.program == lift::d3d9::EffectProgram::FadePs20);
        LIFT_CHECK_EQ_U32(sc.program.constant_count, 1u);
        LIFT_CHECK(sc.program.constants[0] == 0.5f);
        LIFT_CHECK_EQ_U32(sc.program.texture_count, 2u);
        LIFT_CHECK_EQ_U32(sc.program.textures[0], tex0);
        LIFT_CHECK_EQ_U32(sc.program.textures[1], tex1);
        LIFT_CHECK_EQ_U32(sc.program.uv_set[0], 0u);
        LIFT_CHECK_EQ_U32(sc.program.uv_set[1], 1u);
        // 文本写 LINEAR×3 ⇒ D3D9 的 D3DTEXF_LINEAR(2)；Address 没写 ⇒ WRAP(1)
        LIFT_CHECK_EQ_U32(sc.program.mag_filter[0], 2u);
        LIFT_CHECK_EQ_U32(sc.program.min_filter[1], 2u);
        LIFT_CHECK_EQ_U32(sc.program.mip_filter[0], 2u);
        LIFT_CHECK_EQ_U32(sc.program.address_u[0], 1u);
    }

    // FVF = XYZRHW(0x004) | DIFFUSE(0x040) | TEX2(0x200) = 0x244 ⇒ 36 B/顶点
    std::vector<std::uint8_t> vb(3u * 36u, 0);
    const float uv0[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    const float uv1[3][2] = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.0f, 0.5f}};
    for (std::uint32_t i = 0; i < 3u; ++i) {
        // 三个**不共线**的顶点（共线 ⇒ 退化三角形 ⇒ 一个顶点都进不了 GPU）
        const float pos[4] = {10.0f + 10.0f * static_cast<float>(i),
                              (i == 2u) ? 20.0f : 10.0f, 0.5f, 1.0f};
        std::uint8_t* base = vb.data() + i * 36u;
        std::memcpy(base + 0u, pos, sizeof(pos));
        const std::uint32_t color = 0xFFFFFFFFu;
        std::memcpy(base + 16u, &color, 4);
        std::memcpy(base + 20u, uv0[i], sizeof(uv0[i]));  // TEXCOORD0
        std::memcpy(base + 28u, uv1[i], sizeof(uv1[i]));  // TEXCOORD1
    }
    const std::uint32_t vtx = f.data(vb.data(), vb.size());
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x164u, {0x0244u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA4u, {}), lift::d3dx9::kOk);   // BeginScene
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x94u, {0u, 0u}), lift::d3dx9::kOk);  // SetRenderTarget(0, NULL)
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsZEnable, 0u}), lift::d3dx9::kOk);
    // ★ 剔除：夹具的三角形绕序与 D3D9 缺省（CCW 剔除）相反 ⇒ 不关剔除就一个顶点都进不了 GPU。
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsCullMode, lift::d3d9::kCullNone}),
                      lift::d3dx9::kOk);

    const std::uint64_t skipped_before = lift::d3d9::gpuCoverage().skipped_draws;
    const std::uint32_t hr =
        callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleList, 1u, vtx, 36u});
    // ① 不再被拒绝
    LIFT_CHECK_EQ_U32(hr, lift::d3dx9::kOk);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::gpuCoverage().skipped_draws),
                      static_cast<std::int64_t>(skipped_before));
    LIFT_CHECK_EQ_U32(mock.effect_queued, 1u);
    LIFT_CHECK_EQ_U32(mock.plain_queued, 0u);
    // ② 后端收到的是文本说的那一份
    LIFT_CHECK(mock.last.program == lift::GpuEffectProgram::FadePs20);
    LIFT_CHECK_EQ_U32(mock.last.constant_count, 1u);
    LIFT_CHECK(mock.last.constants[0] == 0.5f);
    LIFT_CHECK_EQ_U32(mock.last.texture_count, 2u);
    LIFT_CHECK_EQ_U32(mock.last.textures[0].uv_set, 0u);
    LIFT_CHECK_EQ_U32(mock.last.textures[1].uv_set, 1u);
    LIFT_CHECK_EQ_U32(mock.last.textures[0].texture.width, 2);
    LIFT_CHECK(mock.last.textures[0].texture.sampler.mag_filter == lift::GpuFilter::Linear);
    LIFT_CHECK(mock.last.textures[0].texture.sampler.mipmap);  // MipFilter=LINEAR ⇒ 有 mip
    LIFT_CHECK_EQ_U32(mock.vertex_count, 3u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(mock.uv1_values.size()), 6u);
    LIFT_CHECK(mock.uv1_values[0] == 0.0f && mock.uv1_values[1] == 0.0f);
    LIFT_CHECK(mock.uv1_values[2] == 0.5f && mock.uv1_values[3] == 0.0f);  // 第二套 UV 真的传下来了
    LIFT_CHECK(mock.uv1_values[4] == 0.0f && mock.uv1_values[5] == 0.5f);
    std::printf("effect draw recorded：program=FadePs20 Alpha=%g textures=%u uv1=(%g,%g)\n",
                static_cast<double>(mock.last.constants[0]), mock.last.texture_count,
                static_cast<double>(mock.uv1_values[2]), static_cast<double>(mock.uv1_values[3]));

    // ③ 出 pass 之后回到固定功能（同一个 draw 不再是 effect 档）
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x108u, {}), lift::d3dx9::kOk);  // EndPass
    const std::uint32_t hr_plain =
        callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleList, 1u, vtx, 36u});
    LIFT_CHECK_EQ_U32(hr_plain, lift::d3dx9::kOk);  // 固定功能：无纹理 stage0 ⇒ 顶点色
    LIFT_CHECK_EQ_U32(mock.effect_queued, 1u);
    LIFT_CHECK_EQ_U32(mock.plain_queued, 1u);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x10Cu, {}), lift::d3dx9::kOk);  // End
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA8u, {}), lift::d3dx9::kOk);   // EndScene
    f.m.setGpuRaster(nullptr);
}

// =======================================================================================
// ⑨ P3 · 203 的两个 technique **真的执行**（SlideBlur / ZoomBlur；pure：Mock 后端）
//
// 红了意味着什么：203 的某一条翻面翻错了 —— 要么仍被拒（`尚未实现执行`），要么送进后端的
//   程序/常量/纹理/采样器不是 FX 文本说的那一份（那会"画得出来但画面是错的"，且账上无声）。
// 判据：
//   ① 作用域里的画法快照：`implemented` ∧ 程序号（SlideBlurPs20 / ZoomBlurPs20）∧
//      `constant_count == 6` 且**按声明顺序** `Length,Width,Height,Angle,CenterU,CenterV`；
//   ② 纹理 1 张、`uv_set == 0`（源文本 `Tex0UV : TEXCOORD0`）、sampler = **Clamp × Linear × mip**；
//   ③ draw 被接受（S_OK、skipped 不涨）且后端收到的就是 ①②；
//   ④ `EffectProgramBinding` 表里 203 的两条**逐字**对上（resource/technique/entry/profile）。
// ★ 为什么用 Mock：这条判"这一批该怎么画"，不判像素（像素在 ⑩，要真 GPU）。
// =======================================================================================
void caseEffectDrawBlurRecorded(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/effect-draw-blur-recorded";
    // ④ 先对**唯一那张表**（最后一跳）：203 的两条与文本的 technique/entry/profile 逐字一致。
    {
        const lift::d3dx9::EffectProgramBinding* b = lift::d3dx9::effectProgramBindings();
        int seen = 0;
        bool slide = false;
        bool zoom = false;
        for (; b->resource_id != 0; ++b) {
            ++seen;
            if (b->resource_id != 203u) continue;
            if (std::strcmp(b->technique, "SlideBlur") == 0) {
                slide = true;
                LIFT_CHECK(std::strcmp(b->pixel_entry, "SlideBlurPS") == 0);
                LIFT_CHECK(std::strcmp(b->pixel_profile, "ps_2_0") == 0);
                LIFT_CHECK(b->program == lift::d3d9::EffectProgram::SlideBlurPs20);
            } else if (std::strcmp(b->technique, "ZoomBlur") == 0) {
                zoom = true;
                LIFT_CHECK(std::strcmp(b->pixel_entry, "ZoomBlurPS") == 0);
                LIFT_CHECK(std::strcmp(b->pixel_profile, "ps_2_0") == 0);
                LIFT_CHECK(b->program == lift::d3d9::EffectProgram::ZoomBlurPs20);
            }
        }
        LIFT_CHECK(slide && zoom);
        LIFT_CHECK_EQ_I64(seen, 5);  // 200/201 的 Fade + 202 的 Snow + 203 的两条（终止项不算）
    }

    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    LIFT_CHECK(setupMachine(f));
    f.m.cpu().image = &img;
    const std::uint32_t proc3d = f.use("d3d9", "Direct3DCreate9");
    const std::uint32_t d3d9 = f.call(proc3d, {lift::d3d9::kSdkVersion});
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    std::uint32_t device = 0;
    LIFT_CHECK(createDevice(f, d3d9, &device));
    if (device == 0) return;

    EffectMockRaster mock;
    f.m.setGpuRaster(&mock);

    // 一张 16×16 A8R8G8B8 纹理（这里只看"送到后端的是哪张"，内容无关）
    const std::uint32_t proc_tex = f.use("d3dx9_43", "D3DXCreateTexture");
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(createTexture(f, proc_tex, device, 16u, 16u, 0u, lift::d3d9::kFmtA8R8G8B8, &tex),
                      lift::d3dx9::kOk);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;

    const std::uint32_t proc_fx = f.use("d3dx9_43", "D3DXCreateEffectFromResourceA");
    const std::uint32_t out_ea = f.u32data(0);
    LIFT_CHECK_EQ_U32(f.call(proc_fx, {device, 0u, 203u, 0u, 0u, 0u, 0u, out_ea, 0u}), lift::d3dx9::kOk);
    const std::uint32_t effect = f.m.mem().rd32(out_ea);
    LIFT_CHECK_NE_U32(effect, 0u);
    if (effect == 0) return;

    const std::uint32_t s_tex0 = f.cstr("Tex0");
    const std::uint32_t s_len = f.cstr("Length");
    const std::uint32_t s_w = f.cstr("Width");
    const std::uint32_t s_h = f.cstr("Height");
    const std::uint32_t s_angle = f.cstr("Angle");
    const std::uint32_t s_cu = f.cstr("CenterU");
    const std::uint32_t s_cv = f.cstr("CenterV");
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xD0u, {s_tex0, tex}), lift::d3dx9::kOk);

    struct Tech {
        const char* name;
        lift::d3d9::EffectProgram program;
        lift::GpuEffectProgram gpu_program;
    };
    const Tech techs[2] = {
        {"SlideBlur", lift::d3d9::EffectProgram::SlideBlurPs20, lift::GpuEffectProgram::SlideBlurPs20},
        {"ZoomBlur", lift::d3d9::EffectProgram::ZoomBlurPs20, lift::GpuEffectProgram::ZoomBlurPs20},
    };

    // 三个**不共线**的顶点（共线 ⇒ 退化三角形 ⇒ 一个顶点都进不了 GPU）；FVF = 0x244 ⇒ 36 B/顶点
    std::vector<std::uint8_t> vb(3u * 36u, 0);
    const float uv0[3][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}};
    const float uv1[3][2] = {{0.0f, 0.0f}, {0.5f, 0.0f}, {0.0f, 0.5f}};
    for (std::uint32_t i = 0; i < 3u; ++i) {
        const float pos[4] = {10.0f + 10.0f * static_cast<float>(i), (i == 2u) ? 20.0f : 10.0f, 0.5f, 1.0f};
        std::uint8_t* base = vb.data() + i * 36u;
        std::memcpy(base + 0u, pos, sizeof(pos));
        const std::uint32_t color = 0xFFFFFFFFu;
        std::memcpy(base + 16u, &color, 4);
        std::memcpy(base + 20u, uv0[i], sizeof(uv0[i]));  // TEXCOORD0
        std::memcpy(base + 28u, uv1[i], sizeof(uv1[i]));  // TEXCOORD1（203 不用它）
    }
    const std::uint32_t vtx = f.data(vb.data(), vb.size());

    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x164u, {0x0244u}), lift::d3dx9::kOk);  // SetFVF(TEX2)
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA4u, {}), lift::d3dx9::kOk);           // BeginScene
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x94u, {0u, 0u}), lift::d3dx9::kOk);     // SetRenderTarget(0,NULL)
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsZEnable, 0u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsCullMode, lift::d3d9::kCullNone}),
                      lift::d3dx9::kOk);

    for (const Tech& t : techs) {
        const std::uint32_t s_tech = f.cstr(t.name);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xE8u, {s_tech}), lift::d3dx9::kOk);
        // ★ 6 个常量**按声明顺序**（不是按字母、不是按 203 的哪个 technique）
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_len, floatBits(20.0f)}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_w, floatBits(16.0f)}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_h, floatBits(16.0f)}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_angle, floatBits(90.0f)}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_cu, floatBits(0.15625f)}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_cv, floatBits(0.03125f)}), lift::d3dx9::kOk);
        const std::uint32_t passes_ea = f.u32data(0);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xFCu, {passes_ea, 0u}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(f.m.mem().rd32(passes_ea), 1u);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x100u, {0u}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x104u, {}), lift::d3dx9::kOk);
        {
            const lift::d3d9::EffectDrawScope sc = lift::d3d9::effectDrawScope();
            LIFT_CHECK(sc.active && sc.in_pass);
            if (!sc.program.implemented) {
                std::fprintf(stderr, "（%s 未实现的原因）= %s\n", t.name, sc.program.reason.c_str());
            }
            LIFT_CHECK(sc.program.implemented);
            LIFT_CHECK(sc.program.program == t.program);
            LIFT_CHECK_EQ_U32(sc.program.constant_count, 6u);
            LIFT_CHECK(sc.program.constants[0] == 20.0f);
            LIFT_CHECK(sc.program.constants[1] == 16.0f);
            LIFT_CHECK(sc.program.constants[2] == 16.0f);
            LIFT_CHECK(sc.program.constants[3] == 90.0f);
            LIFT_CHECK(sc.program.constants[4] == 0.15625f);
            LIFT_CHECK(sc.program.constants[5] == 0.03125f);
            LIFT_CHECK_EQ_U32(sc.program.texture_count, 1u);
            LIFT_CHECK_EQ_U32(sc.program.textures[0], tex);
            LIFT_CHECK_EQ_U32(sc.program.uv_set[0], 0u);  // `Tex0UV : TEXCOORD0`
            // 203 的 sampler_state：Clamp/Clamp/Linear×3（D3D9 枚举原值 3/3/2/2/2）
            LIFT_CHECK_EQ_U32(sc.program.address_u[0], 3u);
            LIFT_CHECK_EQ_U32(sc.program.address_v[0], 3u);
            LIFT_CHECK_EQ_U32(sc.program.mag_filter[0], 2u);
            LIFT_CHECK_EQ_U32(sc.program.min_filter[0], 2u);
            LIFT_CHECK_EQ_U32(sc.program.mip_filter[0], 2u);
        }
        mock.effect_queued = 0;
        mock.plain_queued = 0;
        mock.uv1_values.clear();
        const std::uint64_t skipped_before = lift::d3d9::gpuCoverage().skipped_draws;
        const std::uint32_t hr =
            callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleList, 1u, vtx, 36u});
        LIFT_CHECK_EQ_U32(hr, lift::d3dx9::kOk);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::gpuCoverage().skipped_draws),
                          static_cast<std::int64_t>(skipped_before));
        LIFT_CHECK_EQ_U32(mock.effect_queued, 1u);
        LIFT_CHECK_EQ_U32(mock.plain_queued, 0u);
        LIFT_CHECK(mock.last.program == t.gpu_program);
        LIFT_CHECK_EQ_U32(mock.last.constant_count, 6u);
        LIFT_CHECK(mock.last.constants[0] == 20.0f && mock.last.constants[3] == 90.0f);
        LIFT_CHECK(mock.last.constants[4] == 0.15625f && mock.last.constants[5] == 0.03125f);
        LIFT_CHECK_EQ_U32(mock.last.texture_count, 1u);
        LIFT_CHECK_EQ_U32(mock.last.textures[0].uv_set, 0u);
        LIFT_CHECK_EQ_U32(mock.last.textures[0].texture.width, 16u);
        LIFT_CHECK(mock.last.textures[0].texture.sampler.mag_filter == lift::GpuFilter::Linear);
        LIFT_CHECK(mock.last.textures[0].texture.sampler.mipmap);  // MipFilter=Linear ⇒ 有 mip
        LIFT_CHECK(mock.last.textures[0].texture.sampler.address_u == lift::GpuAddress::Clamp);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(mock.uv1_values.size()), 6u);
        // 单纹理 ⇒ `uv1` 与 `uv0` 同一套（后端仍然收到**第二套数组**，只是值相同）
        LIFT_CHECK(mock.uv1_values[0] == 0.0f && mock.uv1_values[1] == 0.0f);
        LIFT_CHECK(mock.uv1_values[2] == 1.0f && mock.uv1_values[3] == 0.0f);
        std::printf("effect draw recorded（203）：technique=%s program=%u constants=%u texels=%u\n", t.name,
                    static_cast<unsigned>(mock.last.program), mock.last.constant_count,
                    mock.last.texture_count);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x108u, {}), lift::d3dx9::kOk);  // EndPass
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x10Cu, {}), lift::d3dx9::kOk);  // End
    }
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA8u, {}), lift::d3dx9::kOk);  // EndScene
    f.m.setGpuRaster(nullptr);
}

// =======================================================================================
// ⑧ A 单 · 202（Snow）的 draw **翻面成 recorded**（+ `vs_2_0` 负控）
//
// 红了意味着什么：
//   ① 202 仍被拒 / 仍按固定功能画 —— 雪景画错位置，而账上一个位都没有（本仓最贵的一类错）；
//   ② `SetMatrix("Proj")` / `SetVector("Color")` 没真的进 program（矩阵 16 float / `Color` 4 float），
//      或矩阵被默认成单位阵（那会掩盖"引擎没设"这条真话）；
//   ③ "带 VertexShader ⇒ 一律拒绝"退化成"凡 vs_1_0 都放行"（未知 profile 也会被接受）。
// 判据：
//   · 作用域快照：`implemented ∧ has_vs ∧ program == SnowPs10`、`constant_count == 4` 且就是
//     `SetVector("Color")` 的 4 个值、`vs_matrix` 的 16 个 float **逐个等于** `SetMatrix("Proj")`
//     的值、纹理 1 张 / `uv_set == 0` / sampler = Linear×3 + Wrap（202 的 `sampler_state` 原文）；
//   · draw 被接受（S_OK、`skipped_draws` 不涨）且后端收到的程序号 = `GpuEffectProgram::SnowPs10`；
//   · 唯一那张 `kBindings` 表里有 202 的一条，门牌逐字对上（表长 = 5）；
//   · **负控**：VS 实现集按 (entry, profile) 查表（`vs_2_0` / `vs_3_0` / 别的 entry 一律不命中）；
//     再用一段**自造 FX 文本**（`VertexShader=compile vs_2_0 …`）建一个状态块，`effectDrawProgram`
//     必须 `implemented == false` 且原因**点名 vs_2_0 / VertexShader**。
// ★ 这是 P3 的 `effect-draw-unimplemented`（"202 必须被拒"）的**翻面**：断言方向整个反过来。
// =======================================================================================
void caseEffectDrawSnowRecorded(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/effect-draw-snow-recorded";
    // ---- ① `kBindings` 的 202 一条（门牌逐字）+ 表长
    {
        const lift::d3dx9::EffectProgramBinding* b = lift::d3dx9::effectProgramBindings();
        int seen = 0;
        bool snow = false;
        for (; b->resource_id != 0; ++b) {
            ++seen;
            if (b->resource_id != 202u) continue;
            snow = true;
            LIFT_CHECK(std::strcmp(b->technique, "Snow") == 0);
            LIFT_CHECK(std::strcmp(b->pixel_entry, "SnowPS") == 0);
            LIFT_CHECK(std::strcmp(b->pixel_profile, "ps_1_0") == 0);
            LIFT_CHECK(b->program == lift::d3d9::EffectProgram::SnowPs10);
        }
        LIFT_CHECK(snow);
        LIFT_CHECK_EQ_I64(seen, 5);  // 200/201 的 Fade + 202 的 Snow + 203 的两条（终止项不算）
    }
    // ---- ② VS 实现集 = 按 (entry, profile) 查表，⛔ 不是"凡 vs_1_0 都放行"
    LIFT_CHECK(lift::d3dx9::effectVertexProgramSupported("SnowVS", "vs_1_0"));
    LIFT_CHECK(!lift::d3dx9::effectVertexProgramSupported("SnowVS", "vs_2_0"));
    LIFT_CHECK(!lift::d3dx9::effectVertexProgramSupported("SnowVS", "vs_3_0"));
    LIFT_CHECK(!lift::d3dx9::effectVertexProgramSupported("SnowVS", "ps_1_0"));
    LIFT_CHECK(!lift::d3dx9::effectVertexProgramSupported("SnowVP", "vs_1_0"));

    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    LIFT_CHECK(setupMachine(f));
    f.m.cpu().image = &img;
    const std::uint32_t proc3d = f.use("d3d9", "Direct3DCreate9");
    const std::uint32_t d3d9 = f.call(proc3d, {lift::d3d9::kSdkVersion});
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    std::uint32_t device = 0;
    LIFT_CHECK(createDevice(f, d3d9, &device));
    if (device == 0) return;
    EffectMockRaster mock;
    f.m.setGpuRaster(&mock);

    // 一张 16×16 A8R8G8B8 纹理（202 的 `Tex`；内容无关，这里只看"哪张 / 哪套 UV / 什么采样器"）
    const std::uint32_t proc_tex = f.use("d3dx9_43", "D3DXCreateTexture");
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(createTexture(f, proc_tex, device, 16u, 16u, 0u, lift::d3d9::kFmtA8R8G8B8, &tex),
                      lift::d3dx9::kOk);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;

    const std::uint32_t proc_fx = f.use("d3dx9_43", "D3DXCreateEffectFromResourceA");
    const std::uint32_t out_ea = f.u32data(0);
    LIFT_CHECK_EQ_U32(f.call(proc_fx, {device, 0u, 202u, 0u, 0u, 0u, 0u, out_ea, 0u}), lift::d3dx9::kOk);
    const std::uint32_t effect = f.m.mem().rd32(out_ea);
    LIFT_CHECK_NE_U32(effect, 0u);
    if (effect == 0) return;

    // `Proj`：真透视（`D3DXMatrixPerspectiveFovLH` 的口径）—— 用 runtime/math 的生成器 + 同一个
    // 布局适配点转成 guest 的 D3DX 行主序，⛔ 不在用例里手抄 16 个数（手抄会与实现犯同一个错）。
    float proj16[16] = {};
    lift::math::toD3DLayout(lift::math::perspectiveFovLH(0.7853982f, 4.0f / 3.0f, 0.1f, 100.0f), proj16);
    const std::uint32_t proj_ea = f.data(proj16, sizeof(proj16));
    const float color[4] = {1.0f, 1.0f, 1.0f, 0.5f};
    const std::uint32_t color_ea = f.data(color, sizeof(color));

    const std::uint32_t s_tech = f.cstr("Snow");
    const std::uint32_t s_proj = f.cstr("Proj");
    const std::uint32_t s_color = f.cstr("Color");
    const std::uint32_t s_tex = f.cstr("Tex");
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xE8u, {s_tech}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xD0u, {s_tex, tex}), lift::d3dx9::kOk);
    // ★ N3：`SetMatrix("Proj")` 发生在 `Begin` **之前**（`0x4B5716` vs `0x4B55C4`）
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x98u, {s_proj, proj_ea}), lift::d3dx9::kOk);
    // 每粒子在 Begin **之前**设 `Color`（`0x4B55AD`）
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x88u, {s_color, color_ea}), lift::d3dx9::kOk);
    // 状态块里两槽都记下了（同名覆盖 + 16/4 个 float 的宽度）
    {
        lift::d3dx9::EffectInfo info;
        std::string why;
        LIFT_CHECK(lift::d3dx9::effectInfo(f.m.cpu(), effect, &info, &why));
        LIFT_CHECK_EQ_U32(info.set_matrices, 1u);
        LIFT_CHECK_EQ_U32(info.set_vectors, 1u);
        LIFT_CHECK_EQ_U32(info.vector_param_count, 1u);
        LIFT_CHECK(info.matrix_set);
        float got_m[16] = {};
        std::string mname;
        bool mset = false;
        LIFT_CHECK(lift::d3dx9::effectMatrixParamAt(f.m.cpu(), info.state, &mname, got_m, &mset));
        LIFT_CHECK(mset);
        LIFT_CHECK(mname == "Proj");
        for (int i = 0; i < 16; ++i) LIFT_CHECK(got_m[i] == proj16[i]);
        float got_v[4] = {};
        std::string vname;
        LIFT_CHECK(lift::d3dx9::effectVectorParamAt(f.m.cpu(), info.state, 0u, &vname, got_v));
        LIFT_CHECK(vname == "Color");
        for (int i = 0; i < 4; ++i) LIFT_CHECK(got_v[i] == color[i]);
        // 同名覆盖：再设一次不同的值 ⇒ 仍然是**一项**，值是新的
        const float color2[4] = {0.25f, 0.5f, 0.75f, 1.0f};
        const std::uint32_t color2_ea = f.data(color2, sizeof(color2));
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x88u, {s_color, color2_ea}), lift::d3dx9::kOk);
        LIFT_CHECK(lift::d3dx9::effectInfo(f.m.cpu(), effect, &info, &why));
        LIFT_CHECK_EQ_U32(info.set_vectors, 2u);
        LIFT_CHECK_EQ_U32(info.vector_param_count, 1u);  // ⛔ 不是第二项
        LIFT_CHECK(lift::d3dx9::effectVectorParamAt(f.m.cpu(), info.state, 0u, &vname, got_v));
        for (int i = 0; i < 4; ++i) LIFT_CHECK(got_v[i] == color2[i]);
        // 换回这一批要用的那份
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x88u, {s_color, color_ea}), lift::d3dx9::kOk);
    }

    const std::uint32_t passes_ea = f.u32data(0);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xFCu, {passes_ea, 0u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(passes_ea), 1u);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x100u, {0u}), lift::d3dx9::kOk);
    {
        const lift::d3d9::EffectDrawScope sc = lift::d3d9::effectDrawScope();
        LIFT_CHECK(sc.active && sc.in_pass);
        if (!sc.program.implemented) std::fprintf(stderr, "（202 未实现的原因）= %s\n", sc.program.reason.c_str());
        LIFT_CHECK(sc.program.implemented);
        LIFT_CHECK(sc.program.has_vs);
        LIFT_CHECK(sc.program.program == lift::d3d9::EffectProgram::SnowPs10);
        LIFT_CHECK(sc.program.vs_matrix_set);
        for (int i = 0; i < 16; ++i) LIFT_CHECK(sc.program.vs_matrix[i] == proj16[i]);
        // `float4x4 Proj` **不进** constants[]；`float4 Color` 展开成 4 个 float
        LIFT_CHECK_EQ_U32(sc.program.constant_count, 4u);
        for (int i = 0; i < 4; ++i) LIFT_CHECK(sc.program.constants[i] == color[i]);
        LIFT_CHECK_EQ_U32(sc.program.texture_count, 1u);
        LIFT_CHECK_EQ_U32(sc.program.textures[0], tex);
        LIFT_CHECK_EQ_U32(sc.program.uv_set[0], 0u);  // `TexSmp` 采 `UV : TEXCOORD0`
        // 202 的 `sampler_state`：`MipFilter/MinFilter/MagFilter = LINEAR`，Address 没写 ⇒ WRAP(1)
        LIFT_CHECK_EQ_U32(sc.program.mag_filter[0], 2u);
        LIFT_CHECK_EQ_U32(sc.program.min_filter[0], 2u);
        LIFT_CHECK_EQ_U32(sc.program.mip_filter[0], 2u);
        LIFT_CHECK_EQ_U32(sc.program.address_u[0], 1u);
        LIFT_CHECK_EQ_U32(sc.program.address_v[0], 1u);
    }

    // FVF = XYZ(0x002) | TEX1(0x100) = 0x102 ⇒ 20 B/顶点（与引擎的 `push 102h` / `push 14h` 一致）。
    // 顶点是**视图空间**（引擎已乘过 world×view）⇒ z 由 `Proj` 变成 clip.w。
    // ★ 这里取 z = 1 ⇒ `clip.w = 1`：`submitPrimitives` 有一条**既有**的 `kUnGpuPerspective` 闸
    //   （`invw != 1` ⇒ 整批具名拒绝，透视校正插值要另开单）⇒ 真距离（z=10）的批今天到不了后端，
    //   本条用例判的是"这一批该怎么画 / 后端收到什么"，所以取 w=1 的那一档。
    std::vector<std::uint8_t> vb(4u * 20u, 0);
    const float pos[4][3] = {{-0.3f, -0.2f, 1.0f}, {0.3f, -0.2f, 1.0f}, {-0.3f, 0.2f, 1.0f}, {0.3f, 0.2f, 1.0f}};
    const float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    for (std::uint32_t i = 0; i < 4u; ++i) {
        std::uint8_t* base = vb.data() + i * 20u;
        std::memcpy(base + 0u, pos[i], sizeof(pos[i]));
        std::memcpy(base + 12u, uv[i], sizeof(uv[i]));
    }
    const std::uint32_t vtx = f.data(vb.data(), vb.size());
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x164u, {0x0102u}), lift::d3dx9::kOk);  // SetFVF(XYZ|TEX1)
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA4u, {}), lift::d3dx9::kOk);           // BeginScene
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x94u, {0u, 0u}), lift::d3dx9::kOk);     // SetRenderTarget(0,NULL)
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsZEnable, 0u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsCullMode, lift::d3d9::kCullNone}),
                      lift::d3dx9::kOk);
    const std::uint64_t skipped_before = lift::d3d9::gpuCoverage().skipped_draws;
    // 引擎的形状：`DrawPrimitiveUP(TRIANGLESTRIP, 2, particle+2Ch, 20)`
    const std::uint32_t hr =
        callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleStrip, 2u, vtx, 20u});
    LIFT_CHECK_EQ_U32(hr, lift::d3dx9::kOk);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::gpuCoverage().skipped_draws),
                      static_cast<std::int64_t>(skipped_before));
    LIFT_CHECK_EQ_U32(mock.effect_queued, 1u);
    LIFT_CHECK_EQ_U32(mock.plain_queued, 0u);
    LIFT_CHECK(mock.last.program == lift::GpuEffectProgram::SnowPs10);
    LIFT_CHECK_EQ_U32(mock.last.constant_count, 4u);
    for (int i = 0; i < 4; ++i) LIFT_CHECK(mock.last.constants[i] == color[i]);
    LIFT_CHECK_EQ_U32(mock.last.texture_count, 1u);
    LIFT_CHECK_EQ_U32(mock.last.textures[0].uv_set, 0u);
    LIFT_CHECK_EQ_U32(mock.last.textures[0].texture.width, 16u);
    LIFT_CHECK_EQ_U32(mock.vertex_count, 6u);  // 2 个三角形 × 3
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(mock.effect_vertices.size()), 6u);
    std::printf("effect draw recorded（202）：program=%u constants=%u texels=%u verts=%zu\n",
                static_cast<unsigned>(mock.last.program), mock.last.constant_count,
                mock.last.texture_count, mock.effect_vertices.size());

    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x108u, {}), lift::d3dx9::kOk);  // EndPass
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x10Cu, {}), lift::d3dx9::kOk);  // End
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA8u, {}), lift::d3dx9::kOk);   // EndScene
    f.m.setGpuRaster(nullptr);

    // ---- ③ 负控：`vs_2_0` 的 pass ⇒ 仍**具名拒绝**（自造 FX 文本 → 状态块 → effectDrawProgram）
    {
        const std::string text =
            "float4 VS(float4 pos : POSITION) : POSITION { return pos; }\n"
            "float4 PS(float2 uv : TEXCOORD0) : COLOR { return float4(1,1,1,1); }\n"
            "technique T { pass P0 { VertexShader=compile vs_2_0 VS(); "
            "PixelShader=compile ps_2_0 PS(); } }\n";
        lift::d3dx9::EffectSource src;
        src.id = 900u;
        src.bytes.assign(text.begin(), text.end());
        src.size = static_cast<std::uint32_t>(src.bytes.size());
        src.sha256 = lift::toHex(lift::sha256(src.bytes));
        std::string why;
        const std::uint32_t state = lift::d3dx9::createEffectState(f.m.cpu(), device, src, &why);
        LIFT_CHECK_NE_U32(state, 0u);
        if (state != 0) {
            LIFT_CHECK_EQ_U32(lift::d3dx9::effectSetTechnique(f.m.cpu(), state, "T", &why), lift::d3dx9::kOk);
            LIFT_CHECK_EQ_U32(lift::d3dx9::effectBeginPass(f.m.cpu(), state, 0u, &why), lift::d3dx9::kOk);
            lift::d3d9::EffectDrawProgram prog;
            LIFT_CHECK(lift::d3dx9::effectDrawProgram(f.m.cpu(), state, &prog, &why));
            LIFT_CHECK(!prog.implemented);
            LIFT_CHECK(!prog.has_vs);
            LIFT_CHECK(prog.reason.find("vs_2_0") != std::string::npos);
            LIFT_CHECK(prog.reason.find("VertexShader") != std::string::npos);
            LIFT_CHECK(prog.reason.find("VS 实现集") != std::string::npos);
            std::printf("vs_2_0 负控的具名原因 = %s\n", prog.reason.c_str());
            lift::d3dx9::destroyEffectState(f.m.cpu(), state);
        }
    }
}


// =======================================================================================
// ⑦b 运行期闸（P4）：effect draw 撞到**未实现的 program** ⇒ `pipelineTrap()` 响亮停
//
// 红了意味着什么：这条路径又退回了 P1 的"warn + 0 像素 + 位 44 继续跑"——画面会静默缺一层
//   （P4 的判据是"声明 == 实现"：启动守卫已保证 caps 会让引擎建的每个 program 都在实现集里，
//   运行期还撞到它只可能是守卫漏了 / guest 换了 FX 文本）。⛔ 不许降级成 warn / 跳过。
// 判据：
//   ① 自造 FX（`ps_2_0 NopePS`，不在 `kBindings` 里）⇒ BeginPass 后作用域 `implemented == false`；
//   ② Draw ⇒ 返回 D3DERR_INVALIDCALL + `stop_kind == Trap` + `stop_why == "unimplemented effect
//      program"`；stderr 现场含 `D3D9 pipeline trap` / 资源 id=901 / technique="T" / entry
//      `NopePS` / profile `ps_2_0`（⛔ 不是"只停不说"）；
//   ③ **负控**：EndPass/End 之后同一份几何照常接受（S_OK、机器仍在跑）⇒ Trap 是**作用域相关**的，
//      不是"所有 draw 都停"；已实现的 program（202 那一档的 `effect-draw-snow-recorded`）也是负控。
// =======================================================================================
void caseEffectDrawProgramTraps(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/effect-draw-unimplemented-traps";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    LIFT_CHECK(setupMachine(f));
    f.m.cpu().image = &img;
    const std::uint32_t proc3d = f.use("d3d9", "Direct3DCreate9");
    const std::uint32_t d3d9 = f.call(proc3d, {lift::d3d9::kSdkVersion});
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    std::uint32_t device = 0;
    LIFT_CHECK(createDevice(f, d3d9, &device));
    if (device == 0) return;
    EffectMockRaster mock;
    f.m.setGpuRaster(&mock);

    // 自造 FX：只有一个 PS pass，entry/profile 故意不在实现集里（`kBindings` 查不到 ⇒ unimplemented）
    const std::string text =
        "float4 NopePS(float2 uv : TEXCOORD0) : COLOR { return float4(1,1,1,1); }\n"
        "technique T { pass P0 { PixelShader=compile ps_2_0 NopePS(); } }\n";
    lift::d3dx9::EffectSource src;
    src.id = 901u;
    src.bytes.assign(text.begin(), text.end());
    src.size = static_cast<std::uint32_t>(src.bytes.size());
    src.sha256 = lift::toHex(lift::sha256(src.bytes));
    std::string why;
    const std::uint32_t state = lift::d3dx9::createEffectState(f.m.cpu(), device, src, &why);
    LIFT_CHECK_NE_U32(state, 0u);
    if (state == 0) return;
    LIFT_CHECK_EQ_U32(lift::d3dx9::effectSetTechnique(f.m.cpu(), state, "T", &why), lift::d3dx9::kOk);
    const std::uint32_t passes_ea = f.u32data(0u);
    LIFT_CHECK_EQ_U32(lift::d3dx9::effectBegin(f.m.cpu(), state, passes_ea, 0u, &why),
                      lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(lift::d3dx9::effectBeginPass(f.m.cpu(), state, 0u, &why), lift::d3dx9::kOk);
    // ★ 作用域的**唯一**上报口是 `noteEffectDraw*`（shim 的 `Begin`/`BeginPass` 在调完 C++ 之后
    //   发布；那条路径由 `effect-draw-*-recorded` 覆盖）。本用例的被测面是**运行期闸**，所以按
    //   `publishDrawProgram` 的同一顺序直接铺作用域 —— ⛔ 不绕开"program == unimplemented"这个前提。
    lift::d3d9::noteEffectDrawBegin(state, 0u, 901u, "T");
    lift::d3d9::noteEffectDrawBeginPass(0u);
    {
        lift::d3d9::EffectDrawProgram prog;
        LIFT_CHECK(lift::d3dx9::effectDrawProgram(f.m.cpu(), state, &prog, &why));
        LIFT_CHECK(!prog.implemented);
        lift::d3d9::noteEffectDrawProgram(prog);
    }
    {
        const lift::d3d9::EffectDrawScope sc = lift::d3d9::effectDrawScope();
        LIFT_CHECK(sc.active && sc.in_pass);
        LIFT_CHECK(!sc.program.implemented);
        LIFT_CHECK(sc.program.reason.find("NopePS") != std::string::npos);
        // ★ 本用例句柄只喂作用域（不走 COM 表）；`destroyEffectState` 在 End 之后回收。
    }

    // 一份最小几何：FVF = XYZ(0x002)（12 B/顶点），TRIANGLELIST ×1
    const float xyz[3][3] = {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.0f, 0.5f, 0.5f}};
    const std::uint32_t vtx = f.data(xyz, sizeof(xyz));
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x164u, {0x002u}), lift::d3dx9::kOk);  // SetFVF(XYZ)
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA4u, {}), lift::d3dx9::kOk);         // BeginScene
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x94u, {0u, 0u}), lift::d3dx9::kOk);   // SetRenderTarget(0,NULL)
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsZEnable, 0u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsCullMode, lift::d3d9::kCullNone}),
                      lift::d3dx9::kOk);

    // ② 未实现的 program ⇒ pipelineTrap（⛔ 不是 warn + 0 像素 + 位 44）
    StderrTee tee;
    LIFT_CHECK(tee.begin());
    const std::uint32_t hr =
        callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleList, 1u, vtx, 12u});
    const std::string err = tee.finish();
    LIFT_CHECK_EQ_U32(hr, lift::d3dx9::kInvalidCall);
    LIFT_CHECK(!f.m.cpu().running);
    LIFT_CHECK(f.m.cpu().stop_kind == lift::HostStopKind::Trap);
    LIFT_CHECK(f.m.cpu().stop_why != nullptr &&
               std::strcmp(f.m.cpu().stop_why, "unimplemented effect program") == 0);
    LIFT_CHECK(err.find("D3D9 pipeline trap") != std::string::npos);
    LIFT_CHECK(err.find("资源 id=901") != std::string::npos);
    LIFT_CHECK(err.find("technique=\"T\"") != std::string::npos);
    LIFT_CHECK(err.find("NopePS") != std::string::npos);
    LIFT_CHECK(err.find("ps_2_0") != std::string::npos);
    std::printf("effect draw trap 的停因 = %s（stderr %zu B）\n",
                f.m.cpu().stop_why ? f.m.cpu().stop_why : "?", err.size());

    // ③ 负控：Trap 是**作用域相关**的 —— 出 pass/出 effect 后同一份几何照常接受。
    //    先续上：`effectEndPass` / `effectEnd` 都只推进状态（trap 已把 running 置 false，但它们
    //    不跑机器、只写状态块 ⇒ 可以直接调），作用域由同一批 `noteEffectDraw*` 收尾。
    LIFT_CHECK_EQ_U32(lift::d3dx9::effectEndPass(f.m.cpu(), state, &why), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(lift::d3dx9::effectEnd(f.m.cpu(), state, &why), lift::d3dx9::kOk);
    lift::d3d9::noteEffectDrawEndPass();
    lift::d3d9::noteEffectDrawEnd();
    LIFT_CHECK(!lift::d3d9::effectDrawScope().in_pass);
    f.m.cpu().running = true;  // 只为了跑负控那一次 Draw（trap 已判过 stop_kind）
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleList, 1u, vtx, 12u}),
                      lift::d3dx9::kOk);
    LIFT_CHECK(f.m.cpu().running);
    lift::d3dx9::destroyEffectState(f.m.cpu(), state);
    f.m.setGpuRaster(nullptr);
}


// =======================================================================================
// ⑪ A 单 · `snow-vertices`：CPU `vs_1_0` 的**顶点性质**（pure：Mock 后端记录顶点）
//
// 红了意味着什么：`vs_1_0` 被写成了别的东西 —— 又乘了设备的 WORLD/VIEW（二次变换）、除过 `w`、
//   走了固定功能的 T&L（那会把光照/材质色写进顶点色），或 `SetMatrix("Proj")` 根本没参与变换。
// 判据全部是**性质**（⛔ 不抄绝对屏幕坐标、⛔ 不重抄实现里的算术）：
//   ① 单位 `Proj` ⇒ 交给 GPU 的顶点与「固定功能 + 单位 world/view/proj」那一批**逐 float 相同**
//      （两条路共用同一个 clip→屏幕映射 ⇒ 相等就等价于"`clip == 输入顶点`，逐 float"）；
//   ② 两档不同 `Proj` ⇒ 两组不同落点，且方向正是 `mul(pos, Proj)` 说的那一侧
//      （`Proj = scale(0.5)` ⇒ 每个顶点沿 x **向视口中心移动一半**，y/z/uv 不动）；
//   ③ 把同一份 `Proj` 交给固定功能的 PROJECTION，world/view 保持单位 ⇒ **逐 float 相同**
//      —— 这一条同时证伪"乘了 world/view"与"做了 w 除法"（两者都会改变落点），也证伪 uv 没直通；
//   ④ 光照开着（`LIGHTING=1` + 材质漫反射 = 红）：固定功能那批的顶点色变成**材质色**，
//      VS 那批**仍是顶点里的白** ⇒ VS 路不光照（⛔ 不是"走了另一条固定功能分支"）。
// =======================================================================================
void caseSnowVertices(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/snow-vertices";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    LIFT_CHECK(setupMachine(f));
    f.m.cpu().image = &img;
    const std::uint32_t proc3d = f.use("d3d9", "Direct3DCreate9");
    const std::uint32_t d3d9 = f.call(proc3d, {lift::d3d9::kSdkVersion});
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    std::uint32_t device = 0;
    LIFT_CHECK(createDevice(f, d3d9, &device));
    if (device == 0) return;
    EffectMockRaster mock;
    f.m.setGpuRaster(&mock);

    // 纹理：202 的 program 要绑 `Tex`（否则 `implemented=false`，这批根本到不了顶点阶段）
    const std::uint32_t proc_tex = f.use("d3dx9_43", "D3DXCreateTexture");
    std::uint32_t tex = 0;
    LIFT_CHECK_EQ_U32(createTexture(f, proc_tex, device, 16u, 16u, 0u, lift::d3d9::kFmtA8R8G8B8, &tex),
                      lift::d3dx9::kOk);
    LIFT_CHECK_NE_U32(tex, 0u);
    if (tex == 0) return;
    const std::uint32_t proc_fx = f.use("d3dx9_43", "D3DXCreateEffectFromResourceA");
    const std::uint32_t out_ea = f.u32data(0);
    LIFT_CHECK_EQ_U32(f.call(proc_fx, {device, 0u, 202u, 0u, 0u, 0u, 0u, out_ea, 0u}), lift::d3dx9::kOk);
    const std::uint32_t effect = f.m.mem().rd32(out_ea);
    LIFT_CHECK_NE_U32(effect, 0u);
    if (effect == 0) return;
    const std::uint32_t s_tech = f.cstr("Snow");
    const std::uint32_t s_proj = f.cstr("Proj");
    const std::uint32_t s_color = f.cstr("Color");
    const std::uint32_t s_tex = f.cstr("Tex");
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xE8u, {s_tech}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xD0u, {s_tex, tex}), lift::d3dx9::kOk);
    const float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    const std::uint32_t color_ea = f.data(color, sizeof(color));
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x88u, {s_color, color_ea}), lift::d3dx9::kOk);

    // 两批顶点（都是 FVF `0x102` = XYZ|TEX1 ⇒ 20 B/顶点，与引擎的 `push 102h` / `push 14h` 一致）：
    //   · `ndc`：z = 0.5 ⇒ 单位 `Proj` 下 clip 深度落在 [0,1]（不裁剪）；
    //   · `persp`：z = 1 ⇒ 真透视 `Proj` 下 `clip.w = z = 1`。
    //   ★ 为什么 `persp` 取 w=1 而不是"真实距离"（z=10）：`submitPrimitives` 有一条**既有**的
    //     `kUnGpuPerspective` 闸（`invw != 1` ⇒ 整批具名拒绝，`w != 1` 就需要着色器里做透视校正
    //     插值，那是另一条单的活）⇒ 管线级看不到 `w != 1` 的顶点。`w` 没被除这一条因此走
    //     `clipVs10` 的纯函数判据（见下面 ⑤）。
    const float xyz_ndc[4][3] = {{-0.8f, -0.5f, 0.5f}, {0.8f, -0.5f, 0.5f}, {-0.8f, 0.5f, 0.5f}, {0.8f, 0.5f, 0.5f}};
    const float xyz_persp[4][3] = {{-0.3f, -0.2f, 1.0f}, {0.3f, -0.2f, 1.0f}, {-0.3f, 0.2f, 1.0f}, {0.3f, 0.2f, 1.0f}};
    const float uv[4][2] = {{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f}};
    const auto make_strip = [&f, &uv](const float xyz[4][3]) -> std::uint32_t {
        std::vector<std::uint8_t> vb(4u * 20u, 0);
        for (std::uint32_t i = 0; i < 4u; ++i) {
            std::uint8_t* base = vb.data() + i * 20u;
            std::memcpy(base + 0u, xyz[i], sizeof(xyz[i]));
            std::memcpy(base + 12u, uv[i], sizeof(uv[i]));
        }
        return f.data(vb.data(), vb.size());
    };
    const std::uint32_t vtx_ndc = make_strip(xyz_ndc);
    const std::uint32_t vtx_persp = make_strip(xyz_persp);

    constexpr double kViewportW = 320.0;  // `createDevice` 的后缓冲 = 320×240；视口由下面 SetViewport 定
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x164u, {0x0102u}), lift::d3dx9::kOk);  // SetFVF(XYZ|TEX1)
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA4u, {}), lift::d3dx9::kOk);           // BeginScene
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x94u, {0u, 0u}), lift::d3dx9::kOk);     // SetRenderTarget(0,NULL)
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xBCu,
                                 {f.data(std::array<std::uint32_t, 6>{0u, 0u, 320u, 240u, 0x3F800000u, 0u}.data(),
                                         24u)}),
                      lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsZEnable, 0u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsCullMode, lift::d3d9::kCullNone}),
                      lift::d3dx9::kOk);

    // 固定功能那一批（**出 effect 作用域** ⇒ 走 `transformVertex`：world/view/proj 由设备状态给）
    const auto draw_plain = [&](std::uint32_t vtx) {
        LIFT_CHECK_EQ_U32(callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleStrip, 2u, vtx, 20u}),
                          lift::d3dx9::kOk);
    };
    // effect 那一批（`SetMatrix` 在 `Begin` 之前，与引擎的次序一致；N3）
    const auto draw_vs = [&](std::uint32_t vtx, const float m[16]) {
        const std::uint32_t m_ea = f.data(m, 16u * sizeof(float));
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x98u, {s_proj, m_ea}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xFCu, {0u, 0u}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x100u, {0u}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleStrip, 2u, vtx, 20u}),
                          lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x108u, {}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x10Cu, {}), lift::d3dx9::kOk);
    };
    // 逐 float 比两批顶点（x/y/z/r/g/b/a/u/v 全比 —— ⛔ 不比绝对屏幕坐标，只比"两条路一样")
    const auto same_vertices = [](const std::vector<lift::GpuRasterVertex>& a,
                                  const std::vector<lift::GpuRasterVertex>& b, const char* what) {
        if (a.size() != b.size() || a.empty()) {
            lift_test::failf(__FILE__, __LINE__, "%s：顶点数不同（%zu vs %zu）", what, a.size(), b.size());
            return false;
        }
        bool ok = true;
        for (std::size_t i = 0; i < a.size() && ok; ++i) {
            const float lhs[9] = {a[i].x, a[i].y, a[i].z, a[i].r, a[i].g, a[i].b, a[i].a, a[i].u, a[i].v};
            const float rhs[9] = {b[i].x, b[i].y, b[i].z, b[i].r, b[i].g, b[i].b, b[i].a, b[i].u, b[i].v};
            for (int k = 0; k < 9; ++k) {
                if (lhs[k] != rhs[k]) {
                    lift_test::failf(__FILE__, __LINE__, "%s：顶点 #%zu 的第 %d 个 float 不同（%.9g vs %.9g）",
                                     what, i, k, static_cast<double>(lhs[k]), static_cast<double>(rhs[k]));
                    ok = false;
                    break;
                }
            }
        }
        return ok;
    };

    // ---- ① 单位 Proj ⇒ 与「固定功能 + 单位 world/view/proj」逐 float 相同 ----
    draw_plain(vtx_ndc);
    const std::vector<lift::GpuRasterVertex> plain_identity = mock.plain_vertices;
    LIFT_CHECK_EQ_U32(mock.plain_queued, 1u);
    const float identity16[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                  0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    draw_vs(vtx_ndc, identity16);
    const std::vector<lift::GpuRasterVertex> vs_identity = mock.effect_vertices;
    LIFT_CHECK_EQ_U32(mock.effect_queued, 1u);
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(vs_identity.size()), 6u);  // 2 个三角形 × 3
    LIFT_CHECK(same_vertices(plain_identity, vs_identity, "单位 Proj vs 固定功能（单位 world/view/proj）"));

    // ---- ② 两档不同 Proj ⇒ 不同落点，且 x 向视口中心收一半（y / z / uv 不动）----
    const float half_x16[16] = {0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    draw_vs(vtx_ndc, half_x16);
    const std::vector<lift::GpuRasterVertex> vs_half = mock.effect_vertices;
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(vs_half.size()), 6u);
    {
        const double cx = 0.5 * kViewportW;
        bool moved = false;
        for (std::size_t i = 0; i < vs_half.size(); ++i) {
            const double a = static_cast<double>(vs_identity[i].x) - cx;
            const double b = static_cast<double>(vs_half[i].x) - cx;
            if (std::fabs(b - 0.5 * a) > 1e-3) {
                lift_test::failf(__FILE__, __LINE__,
                                 "Proj 的 x 缩 0.5：顶点 #%zu 的 x 偏移没有减半（相对中心 %.6f → %.6f）", i, a, b);
            }
            if (std::fabs(b) < std::fabs(a) - 1e-6) moved = true;
            LIFT_CHECK(vs_half[i].y == vs_identity[i].y);
            LIFT_CHECK(vs_half[i].z == vs_identity[i].z);
            LIFT_CHECK(vs_half[i].u == vs_identity[i].u && vs_half[i].v == vs_identity[i].v);
            LIFT_CHECK(vs_half[i].a == vs_identity[i].a);
        }
        LIFT_CHECK(moved);  // 两组**确实不同**（不是恒等）
    }

    // ---- ③ 光照开着也不改顶点色（VS 路无光照）----
    //   材质漫反射 = 红；`0x102` 既没法线也没顶点 diffuse ⇒ 固定功能那条**必须**变成材质色，
    //   而 VS 那条必须保持顶点里的白（⛔ 否则说明它走了固定功能的 T&L）。
    //   ★ 这一档必须在"设备 PROJ 还是单位阵"时做（见下面 ④ 会改设备 PROJ）。
    std::vector<std::uint8_t> material(68u, 0);
    const float red[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    std::memcpy(material.data() + 0u, red, sizeof(red));  // D3DMATERIAL9::Diffuse
    const std::uint32_t material_ea = f.data(material.data(), material.size());
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xC4u, {material_ea}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsLighting, 1u}), lift::d3dx9::kOk);
    draw_plain(vtx_ndc);
    const std::vector<lift::GpuRasterVertex> plain_lit = mock.plain_vertices;
    draw_vs(vtx_ndc, identity16);
    const std::vector<lift::GpuRasterVertex> vs_lit = mock.effect_vertices;
    LIFT_CHECK(!plain_lit.empty());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plain_lit.size()), static_cast<std::int64_t>(vs_lit.size()));
    for (std::size_t i = 0; i < plain_lit.size() && i < vs_lit.size(); ++i) {
        // 固定功能：材质漫反射（红）
        LIFT_CHECK(plain_lit[i].r == 1.0f && plain_lit[i].g == 0.0f && plain_lit[i].b == 0.0f);
        // VS 路：顶点色（FVF 0x102 没有 diffuse ⇒ 白），⛔ 不受光照/材质影响
        LIFT_CHECK(vs_lit[i].r == 1.0f && vs_lit[i].g == 1.0f && vs_lit[i].b == 1.0f);
    }

    // ---- ④ 纯函数：`clip = mul(vec4(pos,1), Proj)` 的三条不变量 ----
    //   ⛔ 不抄算术：只断言"单位阵 ⇒ 恒等 / 缩放只作用在 x / 透视矩阵下 `w` 原样保留"。
    //   ★ `w` 那一条只能在这里验（`w != 1` 的批会被 `kUnGpuPerspective` 整批拒，管线级看不见）。
    float proj16[16] = {};
    lift::math::toD3DLayout(lift::math::perspectiveFovLH(0.7853982f, 4.0f / 3.0f, 0.1f, 100.0f), proj16);
    {
        const lift::math::Vec4d a = lift::d3d9::clipVs10(identity16, 0.25, -0.5, 0.75);
        LIFT_CHECK(a.x == 0.25 && a.y == -0.5 && a.z == 0.75 && a.w == 1.0);
        const lift::math::Vec4d b = lift::d3d9::clipVs10(half_x16, 0.25, -0.5, 0.75);
        LIFT_CHECK(b.x == 0.125 && b.y == -0.5 && b.z == 0.75 && b.w == 1.0);
        // 平移档（第二档仿射 `Proj`）：`w` 行仍是 (0,0,0,1) ⇒ w 不变，x/y/z 各加平移量
        const float shift16[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                   0.0f, 0.0f, 1.0f, 0.0f, 0.25f, -0.5f, 0.125f, 1.0f};
        const lift::math::Vec4d s = lift::d3d9::clipVs10(shift16, 0.25, -0.5, 0.75);
        LIFT_CHECK(s.x == 0.5 && s.y == -1.0 && s.z == 0.875 && s.w == 1.0);
        // 透视：`Proj` 的 `w` 行 = (0,0,1,0) ⇒ `clip.w = z`（**原样保留**）。若实现里除了 w，
        //   `clip.w` 会变成 1；若乘了 world/view，x/y 会再变一次（这里只钉 w 与"非恒等"）。
        const lift::math::Vec4d c = lift::d3d9::clipVs10(proj16, 0.5, -0.25, 3.0);
        LIFT_CHECK(c.w == 3.0);
        LIFT_CHECK(c.x != 0.5 && c.y != -0.25);   // 真的乘了矩阵
        // `clip.xy` 按 `mul(vec4, Proj)`：同 y/z 下 `x` 加倍 ⇒ `clip.x` 加倍（`Proj` 的 x 行没有交叉项）；
        //   y 方向同理 ⇒ 这两条一起钉住"**行向量、Proj 在右**"（列向量约定下 x 会串到 w 上）。
        const lift::math::Vec4d c2 = lift::d3d9::clipVs10(proj16, 1.0, -0.25, 3.0);
        LIFT_CHECK(c2.x == 2.0 * c.x);
        LIFT_CHECK(c2.y == c.y && c2.w == c.w);
        const lift::math::Vec4d c3 = lift::d3d9::clipVs10(proj16, 0.5, -0.5, 3.0);
        LIFT_CHECK(c3.y == 2.0 * c.y);
        LIFT_CHECK(c3.w == c.w);
        // 近/远平面各出来一个 z：`clip.z / clip.w` 必须落在 [0,1]（D3D 的裁剪空间口径）
        LIFT_CHECK(c.z > 0.0 && c.z < c.w);
        const lift::math::Vec4d near_z = lift::d3d9::clipVs10(proj16, 0.0, 0.0, 0.1);
        LIFT_CHECK(near_z.z >= 0.0 && near_z.z <= near_z.w + 1e-9);
    }

    // ---- ⑤ 同一份透视 Proj 交给固定功能的 PROJECTION ⇒ 逐 float 相同 ----
    //   ★ 这一条把"乘了设备的 WORLD/VIEW"证伪：多乘一次 world/view 就会与固定功能那条路分开。
    //   （顶点取 `w == 1` 的那一批，原因见文件头那条 `kUnGpuPerspective` 说明。）
    //   ★ 先把光照关回去：上一档故意把材质设成红，那条状态会让**固定功能**的顶点色变红，
    //     与"两条路的几何一样"这条判据无关（关掉之后两条路又都是白，逐 float 才能比）。
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsLighting, 0u}), lift::d3dx9::kOk);
    const std::uint32_t proj_ea = f.data(proj16, sizeof(proj16));
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xB0u, {lift::d3d9::kTsProjection, proj_ea}), lift::d3dx9::kOk);
    draw_plain(vtx_persp);
    const std::vector<lift::GpuRasterVertex> plain_persp = mock.plain_vertices;
    draw_vs(vtx_persp, proj16);
    const std::vector<lift::GpuRasterVertex> vs_persp = mock.effect_vertices;
    LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(vs_persp.size()), 6u);
    LIFT_CHECK(same_vertices(plain_persp, vs_persp, "透视 Proj vs 固定功能（device PROJ = 同一份 Proj）"));
    // 与单位 Proj 那一档**不同**（否则"乘了矩阵"这件事本身没有证据）
    LIFT_CHECK(
        std::fabs(static_cast<double>(vs_persp[0].x) - static_cast<double>(vs_identity[0].x)) > 1.0);

    std::printf("snow-vertices：单位 Proj 与固定功能逐 float 相同（%zu 顶点）；透视 Proj 亦相同；"
                "透视下 clip.w 原样保留；光照下 VS 顶点色仍是白\n",
                vs_identity.size());

    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA8u, {}), lift::d3dx9::kOk);  // EndScene
    f.m.setGpuRaster(nullptr);
}

// =======================================================================================
// ⑨ P2 · **像素 oracle**：两张已知纹理 + `Alpha ∈ {0, 0.5, 1}`（真 GPU + RTT 读回）
//
// 判据（红了意味着什么）：
//   ① 读回的像素与**C++ 参考实现**（照 ps_2_0 的源文本逐句）一致（容差 ≤ 1 LSB/通道）；
//   ② 两张纹理**各自的 texel** 真的被采到（四张 texel 两两不同 ⇒ 用错 UV 必红）；
//   ③ **NaN 那一档**（两图全透明 ⇒ `ret.a == 0` ⇒ `ColRate = 0/0`）：输出缓冲与**未画前逐字节相同**。
//      ★ 这**不是**"我们定义了 NaN 语义"，而是**不可观测**：引擎设 `ALPHABLENDENABLE=1` +
//        `SRCALPHA/INVSRCALPHA`，`ret.a == 0` ⇒ 源因子 = 0、目标因子 = 1 ⇒ 该像素对目标**没有任何
//        贡献**（ps_2_0 的 NaN 位模式与 Metal 的 clamp 是否一致，因此在画面是**观测不到**的）。
//        ⛔ 我们不猜 ps_2_0 的 NaN 位模式，也不在 shader 里为它加特判（那是"定义语义"）。
//      ★ 为了让"逐字节相同"不是恒真，同一档还跑一条**控制行**：同样的 blend / 同样的路径，
//        但纹理不透明 ⇒ 目标**必须**变化（否则说明这条 draw 压根没画）。
//   ★ 没有 SDL_GPU 设备 / 没有显示系统 ⇒ SKIP 77（⛔ 不拿纯档夹具冒充像素判据）。
// =======================================================================================
namespace {

struct Bgra8 {
    std::uint8_t b = 0, g = 0, r = 0, a = 255;
};

Bgra8 pixelAtBgra(const std::vector<std::uint8_t>& px, std::uint32_t pitch, int x, int y) {
    Bgra8 out;
    const std::size_t off = static_cast<std::size_t>(y) * pitch + static_cast<std::size_t>(x) * 4u;
    if (off + 3 < px.size()) {
        out.b = px[off + 0];
        out.g = px[off + 1];
        out.r = px[off + 2];
        out.a = px[off + 3];
    }
    return out;
}

// ps_2_0 的参考实现（逐句照资源 201 的源文本）：
//   ret.a = c0.a*(1-Alpha) + c1.a*Alpha
//   ColRate = clamp(c1.a*Alpha/ret.a, 0, 1)
//   ret.rgb = c0.rgb*(1-ColRate) + c1.rgb*ColRate
Bgra8 referenceFade(const Bgra8& c0, const Bgra8& c1, float alpha) {
    const double a0 = c0.a / 255.0;
    const double a1 = c1.a / 255.0;
    const double A = static_cast<double>(alpha);
    const double ra = a0 * (1.0 - A) + a1 * A;
    // ★ `ra == 0` 时**不**在这里定义 NaN 行为：这一档由"不可观测"用例覆盖（见上）。
    double col_rate = 0.0;
    if (ra != 0.0) {
        col_rate = (a1 * A) / ra;
        if (col_rate < 0.0) col_rate = 0.0;
        if (col_rate > 1.0) col_rate = 1.0;
    }
    const auto ch = [&](std::uint8_t x0, std::uint8_t x1) {
        const double v = (x0 / 255.0) * (1.0 - col_rate) + (x1 / 255.0) * col_rate;
        double q = v * 255.0;
        q = q < 0.0 ? 0.0 : (q > 255.0 ? 255.0 : q);
        return static_cast<std::uint8_t>(q + 0.5);
    };
    Bgra8 out;
    out.b = ch(c0.b, c1.b);
    out.g = ch(c0.g, c1.g);
    out.r = ch(c0.r, c1.r);
    out.a = static_cast<std::uint8_t>((ra <= 0.0 ? 0.0 : (ra >= 1.0 ? 1.0 : ra)) * 255.0 + 0.5);
    return out;
}

int channelDiff(std::uint8_t a, std::uint8_t b) {
    const int d = static_cast<int>(a) - static_cast<int>(b);
    return d < 0 ? -d : d;
}

}  // namespace

int caseFadePixels(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/fade-pixels";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return 1;
    LIFT_CHECK(setupMachine(f));
    f.m.cpu().image = &img;

    const std::uint32_t proc3d = f.use("d3d9", "Direct3DCreate9");
    const std::uint32_t d3d9 = f.call(proc3d, {lift::d3d9::kSdkVersion});
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return 1;
    std::uint32_t device = 0;
    LIFT_CHECK(createDevice(f, d3d9, &device));
    if (device == 0) return 1;

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift d3dx9 fade oracle";
    opt.driver_list = "software";
    opt.hidden = true;
    opt.raster_gpu = true;
    std::string werr;
    if (!win.init(opt, &werr)) LIFT_SKIP(werr.c_str());
    if (!win.rasterActive()) {
        win.shutdown();
        LIFT_SKIP("rasterActive == false（建不出 SDL_GPU 设备）");
    }
    f.m.setGpuRaster(win.gpuRaster());

    const std::uint32_t proc_tex = f.use("d3dx9_43", "D3DXCreateTexture");
    const int kW = 64;
    const int kH = 64;
    // 两张 2×1 纹理：左右 texel 不同 ⇒ "用错 UV 套"一定算错像素。
    //   uv0 = 0.25（左 texel）、uv1 = 0.75（右 texel）
    std::uint32_t tex0 = 0;
    std::uint32_t tex1 = 0;
    LIFT_CHECK_EQ_U32(createTexture(f, proc_tex, device, 2u, 1u, 0u, lift::d3d9::kFmtA8R8G8B8, &tex0),
                      lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(createTexture(f, proc_tex, device, 2u, 1u, 0u, lift::d3d9::kFmtA8R8G8B8, &tex1),
                      lift::d3dx9::kOk);
    std::uint32_t rt = 0;
    LIFT_CHECK_EQ_U32(createTexture(f, proc_tex, device, static_cast<std::uint32_t>(kW),
                                    static_cast<std::uint32_t>(kH), 0x1u, lift::d3d9::kFmtA8R8G8B8, &rt),
                      lift::d3dx9::kOk);
    LIFT_CHECK_NE_U32(tex0, 0u);
    LIFT_CHECK_NE_U32(tex1, 0u);
    LIFT_CHECK_NE_U32(rt, 0u);
    if (tex0 == 0 || tex1 == 0 || rt == 0) return 1;

    // 左 texel = 红 α=128；右 texel = 绿（⛔ 与 tex1 的不同 ⇒ UV 一定要用对）
    const Bgra8 t0_left{0, 0, 255, 128};
    const Bgra8 t0_right{0, 255, 0, 255};
    // tex1：左 texel = 蓝；右 texel = 黄（R=255,G=255）
    const Bgra8 t1_left{255, 0, 0, 255};
    const Bgra8 t1_right{0, 255, 255, 255};

    // ---- effect（201 = Fade ps_2_0）+ 两张纹理 ----
    const std::uint32_t proc_fx = f.use("d3dx9_43", "D3DXCreateEffectFromResourceA");
    const std::uint32_t out_ea = f.u32data(0);
    LIFT_CHECK_EQ_U32(f.call(proc_fx, {device, 0u, 201u, 0u, 0u, 0u, 0u, out_ea, 0u}), lift::d3dx9::kOk);
    const std::uint32_t effect = f.m.mem().rd32(out_ea);
    LIFT_CHECK_NE_U32(effect, 0u);
    if (effect == 0) return 1;
    const std::uint32_t s_fade = f.cstr("Fade");
    const std::uint32_t s_alpha = f.cstr("Alpha");
    const std::uint32_t s_tex0 = f.cstr("Tex0");
    const std::uint32_t s_tex1 = f.cstr("Tex1");
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xE8u, {s_fade}), lift::d3dx9::kOk);
    // ★ 每一行**新建**两张纹理再绑：直接改 guest 像素**不会**推进内容版本（`TextureInfo::version`），
    //   GPU 的上传缓存按版本走快路径 ⇒ 改了像素也看不到（第一版实现踩过：NaN 档读到的还是旧内容）。
    const auto bind_texels = [&](Bgra8 l0, Bgra8 r0, Bgra8 l1, Bgra8 r1) -> bool {
        std::uint32_t a = 0;
        std::uint32_t b = 0;
        if (createTexture(f, proc_tex, device, 2u, 1u, 0u, lift::d3d9::kFmtA8R8G8B8, &a) != lift::d3dx9::kOk) {
            return false;
        }
        if (createTexture(f, proc_tex, device, 2u, 1u, 0u, lift::d3d9::kFmtA8R8G8B8, &b) != lift::d3dx9::kOk) {
            return false;
        }
        const auto fill = [&f](std::uint32_t texture, Bgra8 left, Bgra8 right) {
            lift::d3d9::TextureInfo ti;
            if (!lift::d3d9::textureInfo(f.m.cpu(), texture, &ti, nullptr)) return false;
            lift::GuestSurface* gs = f.m.surfaces().find(ti.arena);
            if (gs == nullptr) return false;
            std::uint8_t* px = f.m.surfaces().pixels(*gs);
            const Bgra8 texels[2] = {left, right};
            for (int x = 0; x < 2; ++x) {
                std::uint8_t* dst = px + static_cast<std::size_t>(x) * 4u;
                dst[0] = texels[x].b;
                dst[1] = texels[x].g;
                dst[2] = texels[x].r;
                dst[3] = texels[x].a;
            }
            return true;
        };
        if (!fill(a, l0, r0) || !fill(b, l1, r1)) return false;
        return callMethod(f, effect, 0xD0u, {s_tex0, a}) == lift::d3dx9::kOk &&
               callMethod(f, effect, 0xD0u, {s_tex1, b}) == lift::d3dx9::kOk;
    };

    const std::uint32_t rt_surface = textureSurface(f, rt);
    LIFT_CHECK_NE_U32(rt_surface, 0u);
    if (rt_surface == 0) return 1;

    // 全屏四边形（屏幕空间，覆盖 64×64）：uv0 = 0.25（左 texel）/ uv1 = 0.75（右 texel）
    std::vector<std::uint8_t> vb(6u * 36u, 0);
    const float corners[6][2] = {{0, 0}, {64, 0}, {64, 64}, {0, 0}, {64, 64}, {0, 64}};
    for (int i = 0; i < 6; ++i) {
        const float pos[4] = {corners[i][0], corners[i][1], 0.0f, 1.0f};
        std::uint8_t* base = vb.data() + static_cast<std::size_t>(i) * 36u;
        std::memcpy(base + 0u, pos, sizeof(pos));
        const std::uint32_t color = 0xFFFFFFFFu;
        std::memcpy(base + 16u, &color, 4);
        const float uvs[2][2] = {{0.25f, 0.5f}, {0.75f, 0.5f}};
        std::memcpy(base + 20u, uvs[0], sizeof(uvs[0]));  // TEXCOORD0
        std::memcpy(base + 28u, uvs[1], sizeof(uvs[1]));  // TEXCOORD1
    }
    const std::uint32_t vtx = f.data(vb.data(), vb.size());

    // ---- 读回：GetRenderTargetData → 离屏 surface → LockRect ----
    // 读回：CreateOffscreenPlainSurface(+0x90, SYSTEMMEM) → GetRenderTargetData(+0x80) → LockRect(+0x34)
    const auto readback = [&](std::vector<std::uint8_t>* out, std::uint32_t* pitch_out) {
        const std::uint32_t offs_out = f.u32data(0xDEADBEEFu);
        if (callMethod(f, device, 0x90u,
                       {static_cast<std::uint32_t>(kW), static_cast<std::uint32_t>(kH),
                        lift::d3d9::kFmtA8R8G8B8, 2u, offs_out, 0u}) != 0) {
            return false;
        }
        const std::uint32_t offs = f.m.mem().rd32(offs_out);
        if (offs == 0) return false;
        if (callMethod(f, device, 0x80u, {rt_surface, offs}) != 0) return false;
        const std::uint32_t lr_ea = f.zeroData(8u);
        if (callMethod(f, offs, 0x34u, {lr_ea, 0u, 0u}) != 0) return false;
        const std::uint32_t pitch = f.m.mem().rd32(lr_ea + 0u);
        const std::uint32_t bits = f.m.mem().rd32(lr_ea + 4u);
        if (bits == 0 || pitch == 0) return false;
        out->assign(static_cast<std::size_t>(pitch) * static_cast<std::size_t>(kH), 0u);
        f.m.mem().readBytes(bits, out->data(), out->size());
        callMethod(f, offs, 0x38u, {});
        *pitch_out = pitch;
        return true;
    };

    const auto draw_fade = [&](float alpha) {
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_alpha, floatBits(alpha)}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xFCu, {0u, 0u}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x100u, {0u}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x104u, {}), lift::d3dx9::kOk);
        const std::uint32_t dhr = callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleList, 2u, vtx, 36u});
        if (dhr != lift::d3dx9::kOk) {
            for (const auto& kv : lift::d3d9::gpuCoverage().skipped_reasons) {
                std::fprintf(stderr, "[oracle-skip] %llu × %s\n", (unsigned long long)kv.second,
                             kv.first.c_str());
            }
        }
        LIFT_CHECK_EQ_U32(dhr, lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x108u, {}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x10Cu, {}), lift::d3dx9::kOk);
    };

    // 一批"公共状态"：不透明覆盖（blend 关）⇒ 读回的就是 shader 输出
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA4u, {}), lift::d3dx9::kOk);          // BeginScene
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x94u, {0u, rt_surface}), lift::d3dx9::kOk);  // 绑 RT
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xBCu, {f.data(std::array<std::uint32_t, 6>{0u, 0u, 64u, 64u, 0x3F800000u, 0u}.data(), 24u)}),
                      lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x164u, {0x0244u}), lift::d3dx9::kOk);  // SetFVF(TEX2)
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsZEnable, 0u}), lift::d3dx9::kOk);
    // ★ 剔除：夹具的三角形绕序与 D3D9 缺省（CCW 剔除）相反 ⇒ 不关剔除就一个顶点都进不了 GPU。
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsCullMode, lift::d3d9::kCullNone}),
                      lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsAlphaBlendEnable, 0u}), lift::d3dx9::kOk);

    // ---- ①② 三档 Alpha 的像素 oracle ----
    const float alphas[3] = {0.0f, 0.5f, 1.0f};
    int worst = -1;
    for (int i = 0; i < 3; ++i) {
        LIFT_CHECK(bind_texels(t0_left, t0_right, t1_left, t1_right));
        LIFT_CHECK_EQ_U32(callMethod(f, device, 0xACu, {0u, 0u, 1u, 0xFF000000u, 0x3F800000u, 0u}),
                          lift::d3dx9::kOk);  // Clear(TARGET)
        draw_fade(alphas[i]);
        std::vector<std::uint8_t> px;
        std::uint32_t pitch = 0;
        LIFT_CHECK(readback(&px, &pitch));
        const Bgra8 got = pixelAtBgra(px, pitch, kW / 2, kH / 2);
        const Bgra8 want = referenceFade(t0_left, t1_right, alphas[i]);
        const int d = std::max({channelDiff(got.r, want.r), channelDiff(got.g, want.g),
                                channelDiff(got.b, want.b), channelDiff(got.a, want.a)});
        if (d > worst) worst = d;
        if (d > 1) {
            lift_test::failf(__FILE__, __LINE__,
                             "Alpha=%g 像素不符：got(B=%u G=%u R=%u A=%u) want(B=%u G=%u R=%u A=%u) 误差=%d",
                             static_cast<double>(alphas[i]), got.b, got.g, got.r, got.a, want.b, want.g,
                             want.r, want.a, d);
        }
        std::printf("Alpha=%.1f：像素 B=%u G=%u R=%u A=%u（参考 B=%u G=%u R=%u A=%u，误差 %d）\n",
                    static_cast<double>(alphas[i]), got.b, got.g, got.r, got.a, want.b, want.g, want.r,
                    want.a, d);
    }
    LIFT_CHECK(worst >= 0 && worst <= 1);
    std::printf("像素 oracle：三档最大误差 = %d LSB/通道\n", worst);

    // ---- ③ NaN 档：两图全透明 ⇒ ret.a == 0 ⇒ 对目标**没有贡献**（不可观测）----
    //   引擎的混合设置：ALPHABLENDENABLE=1 + SRCBLEND=SRCALPHA + DESTBLEND=INVSRCALPHA。
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsAlphaBlendEnable, 1u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsSrcBlend, lift::d3d9::kBlendSrcAlpha}),
                      lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsDestBlend, lift::d3d9::kBlendInvSrcAlpha}),
                      lift::d3dx9::kOk);
    // ★ 控制行：同样的混合 / 同样的路径，但纹理**不透明** ⇒ 目标必须变化（否则"逐字节相同"是恒真）
    LIFT_CHECK(bind_texels(Bgra8{10, 20, 30, 255}, Bgra8{40, 50, 60, 255}, Bgra8{70, 80, 90, 255},
                           Bgra8{100, 110, 120, 255}));
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xACu, {0u, 0u, 1u, 0xFF112233u, 0x3F800000u, 0u}),
                      lift::d3dx9::kOk);
    std::vector<std::uint8_t> before;
    std::uint32_t before_pitch = 0;
    LIFT_CHECK(readback(&before, &before_pitch));
    draw_fade(0.5f);
    std::vector<std::uint8_t> after_opaque;
    LIFT_CHECK(readback(&after_opaque, &before_pitch));
    LIFT_CHECK(after_opaque != before);  // 控制行：真的画上了

    // 现在把两张纹理换成**全透明**：ret.a == 0 ⇒ 目标必须与"未画前"逐字节相同。
    LIFT_CHECK(bind_texels(Bgra8{10, 20, 30, 0}, Bgra8{40, 50, 60, 0}, Bgra8{70, 80, 90, 0},
                           Bgra8{100, 110, 120, 0}));
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xACu, {0u, 0u, 1u, 0xFF112233u, 0x3F800000u, 0u}),
                      lift::d3dx9::kOk);
    std::vector<std::uint8_t> nan_before;
    std::uint32_t nan_pitch = 0;
    LIFT_CHECK(readback(&nan_before, &nan_pitch));
    draw_fade(0.5f);
    std::vector<std::uint8_t> nan_after;
    LIFT_CHECK(readback(&nan_after, &nan_pitch));
    // ★ 判据 = 逐字节相同（原因见文件头：**不可观测**，⛔ 不是我们定义了 NaN 语义）
    bool same = nan_after.size() == nan_before.size();
    std::size_t first_diff = 0;
    for (std::size_t i = 0; same && i < nan_before.size(); ++i) {
        if (nan_before[i] != nan_after[i]) {
            same = false;
            first_diff = i;
        }
    }
    if (!same) {
        lift_test::failf(__FILE__, __LINE__,
                         "ret.a==0 的那一档改动了目标（第一处差异 = 字节 %zu）：引擎的 SRCALPHA/INVSRCALPHA 下"
                         "源因子=0 ⇒ 该像素对目标没有贡献（不可观测）",
                         first_diff);
    }
    LIFT_CHECK(same);
    const Bgra8 nan_px = pixelAtBgra(nan_before, nan_pitch, kW / 2, kH / 2);
    std::printf("NaN 档（两图全透明 + Alpha=0.5）：输出缓冲与未画前 %s（采样点 B=%u G=%u R=%u A=%u）\n",
                same ? "逐字节相同" : "不同", nan_px.b, nan_px.g, nan_px.r, nan_px.a);

    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA8u, {}), lift::d3dx9::kOk);  // EndScene
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x94u, {0u, 0u}), lift::d3dx9::kOk);  // 解绑 RT
    f.m.setGpuRaster(nullptr);
    win.shutdown();
    return 0;
}

// =======================================================================================
// ⑩ P3 · **203 的像素 oracle**：两个 technique × 已知 16×16 纹理（真 GPU + RT 读回）
//
// 红了意味着什么：SlideBlur / ZoomBlur 的**算术**被写坏了 —— tap 数、权重、uv 步长、
//   方向（cos/sin）、CLAMP 语义、常量绑定，任何一处错都会让读回值偏离参考实现。
// 判据（每档）：读回像素与 C++ 参考（**逐句照源文本**）的差 ≤ 1 LSB/通道。
//   · SlideBlur：`for( float i = -10.0f; i <= 10.0f; i++ )` ⇒ **21 个 tap**，每 tap `0.0476f`，
//     `tLen = Length / max(Width, Height) * 0.05f`，`tmpUV = (cosT, sinT) * tLen * i`；
//   · ZoomBlur：`for( float i = 0.0f; i < 10.0f; i++ )` ⇒ **10 个 tap**，权重 = 文本的 `ratio[10]`，
//     `offset = normalize(dir) * texelSize * Length * length(dir)`，`texelSize = 1/1024`。
//
// ★ 为什么采样点都落在 **texel 中心**：参数按"步长 = 整数个 texel"挑（16 宽的纹理 + `tLen = 1/16`、
//   ZoomBlur 用 `Length = 1024` 使 `texelSize*Length = 1`）⇒ 每 tap 的双线性权重是 (1,0)，
//   参考实现不必与各 GPU 的**亚纹素精度**赛跑（那会有几 LSB 的实现差异，容差就不再是 1）。
//   代价：CLAMP 臂仍然被测到（越界的 tap 落在边缘 texel 上）。
// ★ 方向：Angle=0 只在 **x** 上走、Angle=90 只在 **y** 上走；纹理 R 随 x 变、G 随 y 变
//   ⇒ cos/sin 写反或漏乘一定红（单看"有没有画出来"抓不到）。
// ★ 没有 SDL_GPU 设备 / 没有显示系统 ⇒ SKIP 77（⛔ 不拿 Mock 后端冒充像素判据）。
// =======================================================================================
namespace {

// 归一化颜色（0..1）—— 参考实现全程用 double，最后才量化到 8 bit。
struct Bgra8f {
    double b = 0.0, g = 0.0, r = 0.0, a = 1.0;
};

// D3D9 的 `CLAMP` + `LINEAR`：先把 uv 钳到 [0,1]，再按 **texel 中心** 做双线性（越界邻居取边缘）。
//   ★ 与 D3D9 的 `D3DTADDRESS_CLAMP` 一致：钳的是**地址**，所以 uv<0 采到的就是第一个 texel。
Bgra8f sampleClampLinear(const std::vector<Bgra8>& tex, int w, int h, double u, double v) {
    const double uu = u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
    const double vv = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
    const double fx = uu * static_cast<double>(w) - 0.5;
    const double fy = vv * static_cast<double>(h) - 0.5;
    const double x0f = std::floor(fx);
    const double y0f = std::floor(fy);
    const double tx = fx - x0f;
    const double ty = fy - y0f;
    const int x0 = static_cast<int>(x0f);
    const int y0 = static_cast<int>(y0f);
    const auto at = [&](int x, int y) -> const Bgra8& {
        x = x < 0 ? 0 : (x >= w ? w - 1 : x);
        y = y < 0 ? 0 : (y >= h ? h - 1 : y);
        return tex[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                   static_cast<std::size_t>(x)];
    };
    const Bgra8& c00 = at(x0, y0);
    const Bgra8& c10 = at(x0 + 1, y0);
    const Bgra8& c01 = at(x0, y0 + 1);
    const Bgra8& c11 = at(x0 + 1, y0 + 1);
    const auto ch = [&](std::uint8_t a00, std::uint8_t a10, std::uint8_t a01, std::uint8_t a11) {
        const double top = (a00 / 255.0) * (1.0 - tx) + (a10 / 255.0) * tx;
        const double bot = (a01 / 255.0) * (1.0 - tx) + (a11 / 255.0) * tx;
        return top * (1.0 - ty) + bot * ty;
    };
    Bgra8f out;
    out.b = ch(c00.b, c10.b, c01.b, c11.b);
    out.g = ch(c00.g, c10.g, c01.g, c11.g);
    out.r = ch(c00.r, c10.r, c01.r, c11.r);
    out.a = ch(c00.a, c10.a, c01.a, c11.a);
    return out;
}

// 归一化 → 8 bit（ps_2_0 写到 A8R8G8B8 目标：钳到 [0,1] 后四舍五入）
Bgra8 quantizeBgra(double b, double g, double r, double a) {
    const auto q = [](double v) {
        const double c = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
        const double x = c * 255.0 + 0.5;
        return static_cast<std::uint8_t>(x > 255.0 ? 255.0 : x);
    };
    Bgra8 out;
    out.b = q(b);
    out.g = q(g);
    out.r = q(r);
    out.a = q(a);
    return out;
}

// 资源 203 `SlideBlurPS` 的参考实现（21 tap × 0.0476）
Bgra8 referenceSlideBlur(const std::vector<Bgra8>& tex, int w, int h, double u0, double v0,
                         double length, double width, double height, double angle) {
    const double kDegToRad = 0.017453292519943295;  // 源文本的 `radians(Angle)`
    const double sin_t = std::sin(angle * kDegToRad);
    const double cos_t = std::cos(angle * kDegToRad);
    const double t_len = length / std::max(width, height) * 0.05;
    double acc_b = 0.0, acc_g = 0.0, acc_r = 0.0, acc_a = 0.0;
    for (int tap = -10; tap <= 10; ++tap) {
        const double fi = static_cast<double>(tap);
        const double du = cos_t * t_len * fi;
        const double dv = sin_t * t_len * fi;
        const Bgra8f c = sampleClampLinear(tex, w, h, u0 + du, v0 + dv);
        acc_b += c.b * 0.0476;
        acc_g += c.g * 0.0476;
        acc_r += c.r * 0.0476;
        acc_a += c.a * 0.0476;
    }
    return quantizeBgra(acc_b, acc_g, acc_r, acc_a);
}

// 资源 203 `ZoomBlurPS` 的参考实现（10 tap × ratio[10]）
Bgra8 referenceZoomBlur(const std::vector<Bgra8>& tex, int w, int h, double u0, double v0, double length,
                        double center_u, double center_v) {
    const double texel_size = 0.0009765625;  // 源文本的 `static const float2 texelSize`
    const double ratio[10] = {0.19, 0.17, 0.15, 0.13, 0.11, 0.09, 0.07, 0.05, 0.03, 0.01};
    const double dx = center_u - u0;
    const double dy = center_v - v0;
    const double len = std::sqrt(dx * dx + dy * dy);
    // 源文本：`normalize( dir ) * texelSize * Length * length( dir )`。
    //   ★ `dir == 0` 时 `normalize` 在 ps_2_0 与 Metal 上都是**未定义**的 ⇒ 本用例只测 dir != 0。
    const double nx = len != 0.0 ? dx / len : 0.0;
    const double ny = len != 0.0 ? dy / len : 0.0;
    const double ox = nx * texel_size * length * len;
    const double oy = ny * texel_size * length * len;
    double acc_b = 0.0, acc_g = 0.0, acc_r = 0.0, acc_a = 0.0;
    for (int tap = 0; tap < 10; ++tap) {
        const double fi = static_cast<double>(tap);
        const Bgra8f c = sampleClampLinear(tex, w, h, u0 + ox * fi, v0 + oy * fi);
        acc_b += c.b * ratio[tap];
        acc_g += c.g * ratio[tap];
        acc_r += c.r * ratio[tap];
        acc_a += c.a * ratio[tap];
    }
    return quantizeBgra(acc_b, acc_g, acc_r, acc_a);
}

}  // namespace

int caseBlurPixels(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/blur-pixels";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return 1;
    LIFT_CHECK(setupMachine(f));
    f.m.cpu().image = &img;

    const std::uint32_t proc3d = f.use("d3d9", "Direct3DCreate9");
    const std::uint32_t d3d9 = f.call(proc3d, {lift::d3d9::kSdkVersion});
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return 1;
    std::uint32_t device = 0;
    LIFT_CHECK(createDevice(f, d3d9, &device));
    if (device == 0) return 1;

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift d3dx9 blur oracle";
    opt.driver_list = "software";
    opt.hidden = true;
    opt.raster_gpu = true;
    std::string werr;
    if (!win.init(opt, &werr)) LIFT_SKIP(werr.c_str());
    if (!win.rasterActive()) {
        win.shutdown();
        LIFT_SKIP("rasterActive == false（建不出 SDL_GPU 设备）");
    }
    f.m.setGpuRaster(win.gpuRaster());

    const std::uint32_t proc_tex = f.use("d3dx9_43", "D3DXCreateTexture");
    const int kW = 64;
    const int kH = 64;
    const int kTexW = 16;
    const int kTexH = 16;
    // 已知 texel：R = 17x、G = 17y、B = 255-17x、A = 255（x/y 各 16 档 ⇒ 两个方向都可分辨）
    std::vector<Bgra8> texels(static_cast<std::size_t>(kTexW) * static_cast<std::size_t>(kTexH));
    for (int y = 0; y < kTexH; ++y) {
        for (int x = 0; x < kTexW; ++x) {
            Bgra8 t;
            t.r = static_cast<std::uint8_t>(17 * x);
            t.g = static_cast<std::uint8_t>(17 * y);
            t.b = static_cast<std::uint8_t>(255 - 17 * x);
            t.a = 255;
            texels[static_cast<std::size_t>(y) * static_cast<std::size_t>(kTexW) +
                   static_cast<std::size_t>(x)] = t;
        }
    }
    std::uint32_t rt = 0;
    LIFT_CHECK_EQ_U32(createTexture(f, proc_tex, device, static_cast<std::uint32_t>(kW),
                                    static_cast<std::uint32_t>(kH), 0x1u, lift::d3d9::kFmtA8R8G8B8, &rt),
                      lift::d3dx9::kOk);
    LIFT_CHECK_NE_U32(rt, 0u);
    if (rt == 0) return 1;

    const std::uint32_t proc_fx = f.use("d3dx9_43", "D3DXCreateEffectFromResourceA");
    const std::uint32_t out_ea = f.u32data(0);
    LIFT_CHECK_EQ_U32(f.call(proc_fx, {device, 0u, 203u, 0u, 0u, 0u, 0u, out_ea, 0u}), lift::d3dx9::kOk);
    const std::uint32_t effect = f.m.mem().rd32(out_ea);
    LIFT_CHECK_NE_U32(effect, 0u);
    if (effect == 0) return 1;

    const std::uint32_t s_tex0 = f.cstr("Tex0");
    const std::uint32_t s_len = f.cstr("Length");
    const std::uint32_t s_w = f.cstr("Width");
    const std::uint32_t s_h = f.cstr("Height");
    const std::uint32_t s_angle = f.cstr("Angle");
    const std::uint32_t s_cu = f.cstr("CenterU");
    const std::uint32_t s_cv = f.cstr("CenterV");

    const std::uint32_t rt_surface = textureSurface(f, rt);
    LIFT_CHECK_NE_U32(rt_surface, 0u);
    if (rt_surface == 0) return 1;

    // 全屏四边形（屏幕空间，覆盖 64×64）：uv **恒定** = 目标像素算出来的就是同一个值。
    //   uv0 = (0.03125, 0.03125) = 16×16 纹理里 (0,0) 那个 texel 的中心。
    const float kU0 = 0.03125f;
    const float kV0 = 0.03125f;
    std::vector<std::uint8_t> vb(6u * 36u, 0);
    const float corners[6][2] = {{0, 0}, {64, 0}, {64, 64}, {0, 0}, {64, 64}, {0, 64}};
    for (int i = 0; i < 6; ++i) {
        const float pos[4] = {corners[i][0], corners[i][1], 0.0f, 1.0f};
        std::uint8_t* base = vb.data() + static_cast<std::size_t>(i) * 36u;
        std::memcpy(base + 0u, pos, sizeof(pos));
        const std::uint32_t color = 0xFFFFFFFFu;
        std::memcpy(base + 16u, &color, 4);
        const float uvs[2][2] = {{kU0, kV0}, {kU0, kV0}};
        std::memcpy(base + 20u, uvs[0], sizeof(uvs[0]));  // TEXCOORD0
        std::memcpy(base + 28u, uvs[1], sizeof(uvs[1]));  // TEXCOORD1（203 不用）
    }
    const std::uint32_t vtx = f.data(vb.data(), vb.size());

    const auto readback = [&](std::vector<std::uint8_t>* out, std::uint32_t* pitch_out) {
        const std::uint32_t offs_out = f.u32data(0xDEADBEEFu);
        if (callMethod(f, device, 0x90u,
                       {static_cast<std::uint32_t>(kW), static_cast<std::uint32_t>(kH),
                        lift::d3d9::kFmtA8R8G8B8, 2u, offs_out, 0u}) != 0) {
            return false;
        }
        const std::uint32_t offs = f.m.mem().rd32(offs_out);
        if (offs == 0) return false;
        if (callMethod(f, device, 0x80u, {rt_surface, offs}) != 0) return false;
        const std::uint32_t lr_ea = f.zeroData(8u);
        if (callMethod(f, offs, 0x34u, {lr_ea, 0u, 0u}) != 0) return false;
        const std::uint32_t pitch = f.m.mem().rd32(lr_ea + 0u);
        const std::uint32_t bits = f.m.mem().rd32(lr_ea + 4u);
        if (bits == 0 || pitch == 0) return false;
        out->assign(static_cast<std::size_t>(pitch) * static_cast<std::size_t>(kH), 0u);
        f.m.mem().readBytes(bits, out->data(), out->size());
        callMethod(f, offs, 0x38u, {});
        *pitch_out = pitch;
        return true;
    };

    // 一批"公共状态"：不透明覆盖（blend 关）⇒ 读回的就是 shader 输出
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA4u, {}), lift::d3dx9::kOk);                // BeginScene
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x94u, {0u, rt_surface}), lift::d3dx9::kOk);  // 绑 RT
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xBCu, {f.data(std::array<std::uint32_t, 6>{0u, 0u, 64u, 64u,
                                                                                        0x3F800000u, 0u}.data(),
                                                          24u)}),
                      lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x164u, {0x0244u}), lift::d3dx9::kOk);  // SetFVF(TEX2)
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsZEnable, 0u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsCullMode, lift::d3d9::kCullNone}),
                      lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsAlphaBlendEnable, 0u}), lift::d3dx9::kOk);

    struct Case {
        const char* name;
        float length;
        float width;
        float height;
        float angle;
        float center_u;
        float center_v;
        bool zoom;
    };
    const Case cases[3] = {
        {"SlideBlur Angle=0", 20.0f, 16.0f, 16.0f, 0.0f, 0.0f, 0.0f, false},
        {"SlideBlur Angle=90", 20.0f, 16.0f, 16.0f, 90.0f, 0.0f, 0.0f, false},
        // ZoomBlur：`texelSize*Length = 1` ⇒ 步长 = 整数 texel（采样落在 texel 中心）；
        //   center 取 (0.15625, 0.03125) = texel (2,0) 的中心 ⇒ 方向纯 x、dir = 0.125
        {"ZoomBlur", 1024.0f, 0.0f, 0.0f, 0.0f, 0.15625f, 0.03125f, true},
    };

    int worst = -1;
    for (const Case& c : cases) {
        // ★ 每一档**新建**一张纹理再绑：直接改 guest 像素**不推进内容版本**（P2 §5.2 的坑）
        std::uint32_t tex = 0;
        LIFT_CHECK_EQ_U32(createTexture(f, proc_tex, device, static_cast<std::uint32_t>(kTexW),
                                        static_cast<std::uint32_t>(kTexH), 0u, lift::d3d9::kFmtA8R8G8B8,
                                        &tex),
                          lift::d3dx9::kOk);
        LIFT_CHECK_NE_U32(tex, 0u);
        if (tex == 0) return 1;
        {
            lift::d3d9::TextureInfo ti;
            LIFT_CHECK(lift::d3d9::textureInfo(f.m.cpu(), tex, &ti, nullptr));
            lift::GuestSurface* gs = f.m.surfaces().find(ti.arena);
            LIFT_CHECK(gs != nullptr);
            if (gs == nullptr) return 1;
            std::uint8_t* px = f.m.surfaces().pixels(*gs);
            for (int y = 0; y < kTexH; ++y) {
                for (int x = 0; x < kTexW; ++x) {
                    const Bgra8& t = texels[static_cast<std::size_t>(y) * kTexW + static_cast<std::size_t>(x)];
                    std::uint8_t* dst = px + static_cast<std::size_t>(y) * gs->pitch +
                                        static_cast<std::size_t>(x) * 4u;
                    dst[0] = t.b;
                    dst[1] = t.g;
                    dst[2] = t.r;
                    dst[3] = t.a;
                }
            }
        }
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xD0u, {s_tex0, tex}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xE8u, {f.cstr(c.zoom ? "ZoomBlur" : "SlideBlur")}),
                          lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_len, floatBits(c.length)}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_w, floatBits(c.width)}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_h, floatBits(c.height)}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_angle, floatBits(c.angle)}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_cu, floatBits(c.center_u)}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_cv, floatBits(c.center_v)}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, device, 0xACu, {0u, 0u, 1u, 0xFF000000u, 0x3F800000u, 0u}),
                          lift::d3dx9::kOk);  // Clear(TARGET)
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xFCu, {0u, 0u}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x100u, {0u}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x104u, {}), lift::d3dx9::kOk);
        const std::uint32_t dhr = callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleList, 2u, vtx, 36u});
        if (dhr != lift::d3dx9::kOk) {
            for (const auto& kv : lift::d3d9::gpuCoverage().skipped_reasons) {
                std::fprintf(stderr, "[blur-oracle-skip] %llu × %s\n", (unsigned long long)kv.second,
                             kv.first.c_str());
            }
        }
        LIFT_CHECK_EQ_U32(dhr, lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x108u, {}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x10Cu, {}), lift::d3dx9::kOk);

        std::vector<std::uint8_t> px;
        std::uint32_t pitch = 0;
        LIFT_CHECK(readback(&px, &pitch));
        const Bgra8 got = pixelAtBgra(px, pitch, kW / 2, kH / 2);
        const Bgra8 want = c.zoom
                               ? referenceZoomBlur(texels, kTexW, kTexH, kU0, kV0, c.length, c.center_u,
                                                   c.center_v)
                               : referenceSlideBlur(texels, kTexW, kTexH, kU0, kV0, c.length, c.width,
                                                    c.height, c.angle);
        const int d = std::max({channelDiff(got.r, want.r), channelDiff(got.g, want.g),
                                channelDiff(got.b, want.b), channelDiff(got.a, want.a)});
        if (d > worst) worst = d;
        if (d > 1) {
            lift_test::failf(__FILE__, __LINE__,
                             "%s 像素不符：got(B=%u G=%u R=%u A=%u) want(B=%u G=%u R=%u A=%u) 误差=%d",
                             c.name, got.b, got.g, got.r, got.a, want.b, want.g, want.r, want.a, d);
        }
        std::printf("%s：像素 B=%u G=%u R=%u A=%u（参考 B=%u G=%u R=%u A=%u，误差 %d）\n", c.name, got.b,
                    got.g, got.r, got.a, want.b, want.g, want.r, want.a, d);
    }
    LIFT_CHECK(worst >= 0 && worst <= 1);
    std::printf("203 像素 oracle：三档最大误差 = %d LSB/通道\n", worst);

    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA8u, {}), lift::d3dx9::kOk);  // EndScene
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x94u, {0u, 0u}), lift::d3dx9::kOk);  // 解绑 RT
    f.m.setGpuRaster(nullptr);
    win.shutdown();
    return 0;
}

// =======================================================================================
// ⑫ A 单 · `snow-pixels`：`ps_1_0 SnowPS` 的**像素 oracle**（真 GPU + 纹理 RT 读回）
//
// 红了意味着什么：`SnowPS` 的算术被写坏了 —— 漏乘 / 加错 `Color`、`Color` 读成了顶点色、
//   uv 没直通、或者把 `Proj` 塞进了 uniform（那会与 CPU 侧的矩阵抢同一个槽）。
// 判据（每档）：读回像素 == `texel * Color`（逐通道，容差 ≤1 LSB）；用**恒定 uv** 采样
//   **texel 中心**（双线性权重 (1,0)、导数 0 ⇒ LOD 落到 0 级）⇒ 参考值与各 GPU 的亚纹素精度无关。
//   · `Color = {1,1,1,1}` 与 `{0.5,0.25,0.75,1}` 两档 ⇒ 逐通道乘法的**两个不同**结果；
//   · `Color = {0,0,0,1}` 是**控制行**：输出必须逐通道为 0（证明"乘法真的在乘"，
//     而不是一条恒等的 shader）；
//   · 另一张纹理 / 另一个 uv ⇒ 采到的是**另一个 texel**（证明 uv 真的直通，⛔ 不是常量）。
// ★ 没有 SDL_GPU 设备 / 没有显示系统 ⇒ SKIP 77（⛔ 不拿 Mock 后端冒充像素判据）。
// =======================================================================================
namespace {

// 位图内容 + 读回采样点：`Color` 三档 × 两个 texel（同一张 16×16 纹理里两块不同颜色）
struct SnowPixelCase {
    const char* name;
    float color[4];
    Bgra8 texel;   // 采样点那个 texel 的 BGRA
    float u;       // 恒定 uv（texel 中心）
    float v;
};

}  // namespace

int caseSnowPixels(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/snow-pixels";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return 1;
    LIFT_CHECK(setupMachine(f));
    f.m.cpu().image = &img;
    const std::uint32_t proc3d = f.use("d3d9", "Direct3DCreate9");
    const std::uint32_t d3d9 = f.call(proc3d, {lift::d3d9::kSdkVersion});
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return 1;
    std::uint32_t device = 0;
    LIFT_CHECK(createDevice(f, d3d9, &device));
    if (device == 0) return 1;

    lift::SdlWindow win;
    lift::SdlWindowOptions opt;
    opt.title = "emulator-lift d3dx9 snow oracle";
    opt.driver_list = "software";
    opt.hidden = true;
    opt.raster_gpu = true;
    std::string werr;
    if (!win.init(opt, &werr)) LIFT_SKIP(werr.c_str());
    if (!win.rasterActive()) {
        win.shutdown();
        LIFT_SKIP("rasterActive == false（建不出 SDL_GPU 设备）");
    }
    f.m.setGpuRaster(win.gpuRaster());

    const int kW = 64;
    const int kH = 64;
    const int kTexW = 16;
    const int kTexH = 16;
    const std::uint32_t proc_tex = f.use("d3dx9_43", "D3DXCreateTexture");
    std::uint32_t rt = 0;
    LIFT_CHECK_EQ_U32(createTexture(f, proc_tex, device, static_cast<std::uint32_t>(kW),
                                    static_cast<std::uint32_t>(kH), 0x1u, lift::d3d9::kFmtA8R8G8B8, &rt),
                      lift::d3dx9::kOk);
    LIFT_CHECK_NE_U32(rt, 0u);
    if (rt == 0) return 1;

    const std::uint32_t proc_fx = f.use("d3dx9_43", "D3DXCreateEffectFromResourceA");
    const std::uint32_t out_ea = f.u32data(0);
    LIFT_CHECK_EQ_U32(f.call(proc_fx, {device, 0u, 202u, 0u, 0u, 0u, 0u, out_ea, 0u}), lift::d3dx9::kOk);
    const std::uint32_t effect = f.m.mem().rd32(out_ea);
    LIFT_CHECK_NE_U32(effect, 0u);
    if (effect == 0) return 1;
    const std::uint32_t s_tech = f.cstr("Snow");
    const std::uint32_t s_proj = f.cstr("Proj");
    const std::uint32_t s_color = f.cstr("Color");
    const std::uint32_t s_tex = f.cstr("Tex");
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xE8u, {s_tech}), lift::d3dx9::kOk);

    // 单位 `Proj`（顶点已经是 clip 空间的 [-1,1] 四边形 ⇒ 铺满视口；w = 1 ⇒ 屏幕映射是 D3D 那一份）
    const float identity16[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                  0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    const std::uint32_t proj_ea = f.data(identity16, sizeof(identity16));
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x98u, {s_proj, proj_ea}), lift::d3dx9::kOk);

    // 全屏四边形（clip 空间，两个三角形）：FVF `0x102`（XYZ|TEX1）⇒ 20 B/顶点；**恒定 uv**
    const auto make_quad = [&f](float u, float v) -> std::uint32_t {
        const float corners[6][3] = {{-1.0f, -1.0f, 0.5f}, {1.0f, -1.0f, 0.5f}, {1.0f, 1.0f, 0.5f},
                                     {-1.0f, -1.0f, 0.5f}, {1.0f, 1.0f, 0.5f}, {-1.0f, 1.0f, 0.5f}};
        std::vector<std::uint8_t> vb(6u * 20u, 0);
        for (int i = 0; i < 6; ++i) {
            std::uint8_t* base = vb.data() + static_cast<std::size_t>(i) * 20u;
            std::memcpy(base + 0u, corners[i], sizeof(corners[i]));
            const float uv[2] = {u, v};
            std::memcpy(base + 12u, uv, sizeof(uv));
        }
        return f.data(vb.data(), vb.size());
    };

    const std::uint32_t rt_surface = textureSurface(f, rt);
    LIFT_CHECK_NE_U32(rt_surface, 0u);
    if (rt_surface == 0) return 1;
    const auto readback = [&](std::vector<std::uint8_t>* out, std::uint32_t* pitch_out) {
        const std::uint32_t offs_out = f.u32data(0xDEADBEEFu);
        if (callMethod(f, device, 0x90u,
                       {static_cast<std::uint32_t>(kW), static_cast<std::uint32_t>(kH),
                        lift::d3d9::kFmtA8R8G8B8, 2u, offs_out, 0u}) != 0) {
            return false;
        }
        const std::uint32_t offs = f.m.mem().rd32(offs_out);
        if (offs == 0) return false;
        if (callMethod(f, device, 0x80u, {rt_surface, offs}) != 0) return false;
        const std::uint32_t lr_ea = f.zeroData(8u);
        if (callMethod(f, offs, 0x34u, {lr_ea, 0u, 0u}) != 0) return false;
        const std::uint32_t pitch = f.m.mem().rd32(lr_ea + 0u);
        const std::uint32_t bits = f.m.mem().rd32(lr_ea + 4u);
        if (bits == 0 || pitch == 0) return false;
        out->assign(static_cast<std::size_t>(pitch) * static_cast<std::size_t>(kH), 0u);
        f.m.mem().readBytes(bits, out->data(), out->size());
        callMethod(f, offs, 0x38u, {});
        *pitch_out = pitch;
        return true;
    };

    // 公共状态：不透明覆盖（blend 关）⇒ 读回的就是 shader 输出
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA4u, {}), lift::d3dx9::kOk);                // BeginScene
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x94u, {0u, rt_surface}), lift::d3dx9::kOk);  // 绑 RT
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xBCu,
                                 {f.data(std::array<std::uint32_t, 6>{0u, 0u, 64u, 64u, 0x3F800000u, 0u}.data(),
                                         24u)}),
                      lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x164u, {0x0102u}), lift::d3dx9::kOk);  // SetFVF(XYZ|TEX1)
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsZEnable, 0u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsCullMode, lift::d3d9::kCullNone}),
                      lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xE4u, {lift::d3d9::kRsAlphaBlendEnable, 0u}), lift::d3dx9::kOk);

    // 16×16 纹理：整张 = `texel_a`；再把 (12,12) 那一格涂成 `texel_b`（用于"uv 真的直通"那一行）。
    const Bgra8 texel_a{0, 0, 255, 255};  // 红（B=0 G=0 R=255）
    const Bgra8 texel_b{255, 0, 0, 255};  // 蓝（B=255 G=0 R=0）
    const float kUa = (2.0f + 0.5f) / 16.0f;    // texel (2,2) 的中心
    const float kUb = (12.0f + 0.5f) / 16.0f;   // texel (12,12) 的中心
    const SnowPixelCase cases[4] = {
        {"Color={1,1,1,1}", {1.0f, 1.0f, 1.0f, 1.0f}, texel_a, kUa, kUa},
        {"Color={0.5,0.25,0.75,1}", {0.5f, 0.25f, 0.75f, 1.0f}, texel_a, kUa, kUa},
        {"Color={0,0,0,1}（控制行）", {0.0f, 0.0f, 0.0f, 1.0f}, texel_a, kUa, kUa},
        {"uv 换到 texel(12,12)", {1.0f, 1.0f, 1.0f, 1.0f}, texel_b, kUb, kUb},
    };

    int worst = -1;
    for (const SnowPixelCase& c : cases) {
        // ★ 每一档**新建**一张纹理再绑（直接改 guest 像素不推进内容版本 ⇒ 上传缓存会走快路径）
        std::uint32_t tex = 0;
        LIFT_CHECK_EQ_U32(createTexture(f, proc_tex, device, static_cast<std::uint32_t>(kTexW),
                                        static_cast<std::uint32_t>(kTexH), 0u, lift::d3d9::kFmtA8R8G8B8, &tex),
                          lift::d3dx9::kOk);
        LIFT_CHECK_NE_U32(tex, 0u);
        if (tex == 0) return 1;
        {
            lift::d3d9::TextureInfo ti;
            LIFT_CHECK(lift::d3d9::textureInfo(f.m.cpu(), tex, &ti, nullptr));
            lift::GuestSurface* gs = f.m.surfaces().find(ti.arena);
            LIFT_CHECK(gs != nullptr);
            if (gs == nullptr) return 1;
            std::uint8_t* px = f.m.surfaces().pixels(*gs);
            for (int y = 0; y < kTexH; ++y) {
                for (int x = 0; x < kTexW; ++x) {
                    const Bgra8 t = (x == 12 && y == 12) ? texel_b : texel_a;
                    std::uint8_t* dst = px + static_cast<std::size_t>(y) * gs->pitch +
                                        static_cast<std::size_t>(x) * 4u;
                    dst[0] = t.b;
                    dst[1] = t.g;
                    dst[2] = t.r;
                    dst[3] = t.a;
                }
            }
        }
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xD0u, {s_tex, tex}), lift::d3dx9::kOk);
        const std::uint32_t color_ea = f.data(c.color, sizeof(c.color));
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x88u, {s_color, color_ea}), lift::d3dx9::kOk);
        const std::uint32_t vtx = make_quad(c.u, c.v);
        // ★ Clear 色特意**不是**任何一条期望值：控制行（`Color={0,0,0,1}` ⇒ 全 0）才有意义
        //   （否则"shader 压根没画"也会看起来对）。
        LIFT_CHECK_EQ_U32(callMethod(f, device, 0xACu, {0u, 0u, 1u, 0xFF112233u, 0x3F800000u, 0u}),
                          lift::d3dx9::kOk);  // Clear(TARGET)
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xFCu, {0u, 0u}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x100u, {0u}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x104u, {}), lift::d3dx9::kOk);
        const std::uint32_t dhr = callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleList, 2u, vtx, 20u});
        if (dhr != lift::d3dx9::kOk) {
            for (const auto& kv : lift::d3d9::gpuCoverage().skipped_reasons) {
                std::fprintf(stderr, "[snow-oracle-skip] %llu × %s\n", (unsigned long long)kv.second,
                             kv.first.c_str());
            }
        }
        LIFT_CHECK_EQ_U32(dhr, lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x108u, {}), lift::d3dx9::kOk);
        LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x10Cu, {}), lift::d3dx9::kOk);

        std::vector<std::uint8_t> px;
        std::uint32_t pitch = 0;
        LIFT_CHECK(readback(&px, &pitch));
        const Bgra8 got = pixelAtBgra(px, pitch, kW / 2, kH / 2);
        // 参考：`return tex2D(TexSmp, UV) * Color;` ⇒ 逐通道 `texel*Color`（钳到 [0,1] 后量化）。
        // ★ D3D 的 `float4 Color` = (x,y,z,w) = (**R**, **G**, **B**, **A**) ⇒ B 通道乘 `color[2]`。
        const Bgra8 want = quantizeBgra((c.texel.b / 255.0) * c.color[2], (c.texel.g / 255.0) * c.color[1],
                                        (c.texel.r / 255.0) * c.color[0], (c.texel.a / 255.0) * c.color[3]);
        const int d = std::max({channelDiff(got.r, want.r), channelDiff(got.g, want.g),
                                channelDiff(got.b, want.b), channelDiff(got.a, want.a)});
        if (d > worst) worst = d;
        if (d > 1) {
            lift_test::failf(__FILE__, __LINE__,
                             "%s 像素不符：got(B=%u G=%u R=%u A=%u) want(B=%u G=%u R=%u A=%u) 误差=%d", c.name,
                             got.b, got.g, got.r, got.a, want.b, want.g, want.r, want.a, d);
        }
        std::printf("%s：像素 B=%u G=%u R=%u A=%u（参考 B=%u G=%u R=%u A=%u，误差 %d）\n", c.name, got.b,
                    got.g, got.r, got.a, want.b, want.g, want.r, want.a, d);
    }
    LIFT_CHECK(worst >= 0 && worst <= 1);
    std::printf("202 像素 oracle：四档最大误差 = %d LSB/通道\n", worst);

    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA8u, {}), lift::d3dx9::kOk);        // EndScene
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x94u, {0u, 0u}), lift::d3dx9::kOk);  // 解绑 RT
    f.m.setGpuRaster(nullptr);
    win.shutdown();
    return 0;
}

// =======================================================================================
// ④ D3DXCreateEffectFromResourceA + 9 槽（只记状态）
// =======================================================================================
void caseObjectSlots(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/object-slots";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    LIFT_CHECK(setupMachine(f));
    f.m.cpu().image = &img;
    const std::uint32_t proc = f.use("d3dx9_43", "D3DXCreateEffectFromResourceA");
    LIFT_CHECK_NE_U32(proc, 0u);

    const std::uint32_t heap_before = f.m.heap().bytesInUse();
    const std::uint32_t out_ea = f.u32data(0xDEADBEEFu);
    // 实参形状 = S0 §2.1（pSrcResource = 201 = 引擎效果等级 2）
    const std::uint32_t hr = f.call(proc, {0x101C68A8u, 0u, 201u, 0u, 0u, 0u, 0u, out_ea, 0u});
    LIFT_CHECK_EQ_U32(hr, lift::d3dx9::kOk);
    const std::uint32_t effect = f.m.mem().rd32(out_ea);
    LIFT_CHECK_NE_U32(effect, 0u);
    if (effect == 0) return;
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(effect), 1u);
    LIFT_CHECK(f.m.cpu().com->find(effect));

    lift::d3dx9::EffectInfo info;
    std::string why;
    LIFT_CHECK(lift::d3dx9::effectInfo(f.m.cpu(), effect, &info, &why));
    LIFT_CHECK_EQ_U32(info.resource_id, 201u);
    LIFT_CHECK_EQ_U32(info.source_size, 1077u);
    LIFT_CHECK(info.source_sha256 == kExpected[1].sha256);
    LIFT_CHECK_EQ_U32(info.device, 0x101C68A8u);
    LIFT_CHECK(!info.active && !info.in_pass);
    LIFT_CHECK(info.technique.empty());

    // QueryInterface：IUnknown 成功（refcount 2）；别的 IID ⇒ E_NOINTERFACE
    const std::uint8_t kIidUnknown[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                          0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46};
    // IID_ID3DXSprite（随手拿一个"别的 IID"，只为证明 QI 不是恒成功）
    const std::uint8_t kIidOther[16] = {0x2D, 0x76, 0x0B, 0xBA, 0x28, 0x7D, 0xEC, 0x43,
                                        0xB9, 0xDC, 0x2F, 0x84, 0x44, 0x3B, 0x06, 0x14};
    const std::uint32_t iid_unknown = f.data(kIidUnknown, sizeof(kIidUnknown));
    const std::uint32_t iid_other = f.data(kIidOther, sizeof(kIidOther));
    const std::uint32_t ppv = f.u32data(0);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x00u, {iid_unknown, ppv}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(ppv), effect);
    LIFT_CHECK_EQ_U32(f.m.cpu().com->refCount(effect), 2u);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x00u, {iid_other, ppv}), lift::d3dx9::kNoInterface);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(ppv), 0u);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x08u, {}), 1u);  // 把那份额外引用还掉

    // SetTechnique / SetFloat / SetTexture
    const std::uint32_t s_fade = f.cstr("Fade");
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xE8u, {s_fade}), lift::d3dx9::kOk);
    const std::uint32_t s_alpha = f.cstr("Alpha");
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_alpha, floatBits(0.5f)}), lift::d3dx9::kOk);
    // 同名再设 ⇒ 覆盖同一项（参数表不许无限增长）
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x78u, {s_alpha, floatBits(0.75f)}), lift::d3dx9::kOk);
    const std::uint32_t s_tex0 = f.cstr("Tex0");
    const std::uint32_t s_tex1 = f.cstr("Tex1");
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xD0u, {s_tex0, 0u}), lift::d3dx9::kOk);
    // 纹理只**记录** EA（P1 不校验对象；P2 真的绑 stage0/1 时才需要它）
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xD0u, {s_tex1, 0xDEADBEEFu}), lift::d3dx9::kOk);

    // Begin / BeginPass / CommitChanges / EndPass / End
    const std::uint32_t passes_ea = f.u32data(0xDEADBEEFu);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xFCu, {passes_ea, 0u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(passes_ea), 1u);  // 引擎的 pass 循环必须至少跑一次
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x100u, {0u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x104u, {}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x108u, {}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x10Cu, {}), lift::d3dx9::kOk);

    LIFT_CHECK(lift::d3dx9::effectInfo(f.m.cpu(), effect, &info, &why));
    LIFT_CHECK(info.technique == "Fade");
    LIFT_CHECK(!info.active && !info.in_pass);
    LIFT_CHECK_EQ_U32(info.passes_reported, 1u);
    LIFT_CHECK_EQ_U32(info.pass_index, 0u);
    LIFT_CHECK_EQ_U32(info.set_techniques, 1u);
    LIFT_CHECK_EQ_U32(info.set_floats, 2u);
    LIFT_CHECK_EQ_U32(info.set_textures, 2u);
    LIFT_CHECK_EQ_U32(info.begins, 1u);
    LIFT_CHECK_EQ_U32(info.begin_passes, 1u);
    LIFT_CHECK_EQ_U32(info.commits, 1u);
    LIFT_CHECK_EQ_U32(info.end_passes, 1u);
    LIFT_CHECK_EQ_U32(info.ends, 1u);
    LIFT_CHECK_EQ_U32(info.float_param_count, 1u);
    LIFT_CHECK_EQ_U32(info.texture_count, 2u);
    std::string name;
    float value = 0.0f;
    LIFT_CHECK(lift::d3dx9::effectFloatParamAt(f.m.cpu(), info.state, 0u, &name, &value));
    LIFT_CHECK(name == "Alpha");
    LIFT_CHECK(value == 0.75f);
    std::string tname;
    std::uint32_t texture = 0;
    LIFT_CHECK(lift::d3dx9::effectTextureAt(f.m.cpu(), info.state, 0u, &tname, &texture));
    LIFT_CHECK(tname == "Tex0");
    LIFT_CHECK_EQ_U32(texture, 0u);
    LIFT_CHECK(lift::d3dx9::effectTextureAt(f.m.cpu(), info.state, 1u, &tname, &texture));
    LIFT_CHECK(tname == "Tex1");
    LIFT_CHECK_EQ_U32(texture, 0xDEADBEEFu);

    // Release 到 0 ⇒ 对象 + 状态块 + 参数表 + 名字串全部回收
    const std::uint32_t state = info.state;
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x08u, {}), 0u);
    LIFT_CHECK(!f.m.cpu().com->find(effect));
    LIFT_CHECK_EQ_U32(f.m.heap().bytesInUse(), heap_before);
    LIFT_CHECK(f.m.heap().allocCount() > 0);
    (void)state;  // 状态块已随 Release 还回堆（判据是 heap 回到基线，⛔ 不读已释放块的内容）

    // ★ 没取证的槽（+0x110 GetDevice / +0x114 / +0x118）⇒ comUnsupported 的 loud trap。
    //   用第二台夹具（第一台的 heap 已回到基线、对象已释放）。
    lift_fixture::Fixture g;
    LIFT_CHECK(g.ok);
    if (!g.ok) return;
    LIFT_CHECK(setupMachine(g));
    g.m.cpu().image = &img;
    const std::uint32_t proc2 = g.use("d3dx9_43", "D3DXCreateEffectFromResourceA");
    const std::uint32_t out2 = g.u32data(0u);
    LIFT_CHECK_EQ_U32(g.call(proc2, {0x101C68A8u, 0u, 201u, 0u, 0u, 0u, 0u, out2, 0u}),
                      lift::d3dx9::kOk);
    const std::uint32_t effect2 = g.m.mem().rd32(out2);
    LIFT_CHECK_NE_U32(effect2, 0u);
    if (effect2 == 0) return;
    const std::uint32_t dev_out = g.u32data(0xDEADBEEFu);
    bool stopped = false;
    // 直接手搓 guest 调用 + 允许"停在 trap"（不用 callMethod，它会把这个停机当失败）
    {
        const std::uint32_t vtbl = g.m.mem().rd32(effect2);
        GuestAsm a;
        a.movEaxImm(vtbl);
        a.pushImm(dev_out);
        a.pushImm(effect2);
        a.callMemEax(0x110u);
        a.ret();
        placeAndRun(g, a, &stopped);
    }
    LIFT_CHECK(stopped);
    LIFT_CHECK(!g.m.cpu().running);
    LIFT_CHECK(g.m.cpu().stop_why != nullptr && std::strcmp(g.m.cpu().stop_why, "unimplemented COM vtable slot") == 0);
    LIFT_CHECK(g.m.cpu().stop_kind == lift::HostStopKind::Trap);
}

}  // namespace

int main() {
    using namespace lift_test;

    caseCapsPsVersion();   // ① 纯函数
    caseUnrecorded();      // ② 自造 PE（无资产）

    const char* exe = std::getenv("LIFT_AGE_EXE");
    if (exe == nullptr || *exe == '\0') exe = std::getenv("AMAYUI_AGE_EXE");
    if (exe == nullptr || *exe == '\0') {
        LIFT_SKIP("没有 LIFT_AGE_EXE / AMAYUI_AGE_EXE（要 corpus 里的拆壳件；①② 已跑过）");
    }

    g_current_case = "d3dx9-effect/age-load";
    lift::PeImage img;
    std::string err;
    if (!lift::loadPeFile(exe, &img, &err)) {
        std::fprintf(stderr, "loadPeFile(%s): %s\n", exe, err.c_str());
        return 1;
    }
    std::printf("AGE 镜像：%s（%zu 字节，sha256=%s）\n", exe, img.bytes.size(), img.sha256.c_str());
    LIFT_CHECK(img.resource_rva != 0);

    caseResources(img);      // ③
    caseCapsProgramsGuard(img);  // ①b（P4：caps ⇒ 实现集的硬等式，缺一个启动 Trap）
    caseObjectSlots(img);    // ④
    caseFxParse(img);        // ⑤（P2：FX 文本解析）
    caseEffectDrawRecorded(img);       // ⑥（P2：翻面 P1 的"必须拒绝"）
    caseEffectDrawBlurRecorded(img);   // ⑨（P3：203 的 SlideBlur / ZoomBlur 翻面）
    caseEffectDrawSnowRecorded(img);   // ⑧（A 单：202 翻面成 recorded + `vs_2_0` 负控）
    caseEffectDrawProgramTraps(img);   // ⑦b（P4：未实现 program 的 draw ⇒ pipelineTrap）
    caseSnowVertices(img);             // ⑪（A 单：CPU `vs_1_0` 的顶点性质；Mock 后端）
    if (caseFadePixels(img) != 0) return 1;  // ⑩（P2：像素 oracle；没有 GPU ⇒ SKIP 77）
    if (caseBlurPixels(img) != 0) return 1;  // ⑫（P3：203 的像素 oracle；没有 GPU ⇒ SKIP 77）
    if (caseSnowPixels(img) != 0) return 1;  // ⑬（A 单：202 的像素 oracle；没有 GPU ⇒ SKIP 77）
    return finish("d3dx9-effect");
}
