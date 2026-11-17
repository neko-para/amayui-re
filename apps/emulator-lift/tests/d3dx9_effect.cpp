// emulator-lift · GPU 路线 P0/P1：effect 资源字节 + `D3DXCreateEffectFromResourceA` + ID3DXEffect 9 槽
//
// ★ 判据（红了意味着什么）：
//   ① `d3dx9-effect/caps-ps-version`：`LIFT_D3D9_PS_VERSION` 的**值域**与 caps 映射只有 4 档
//      （0 / 0x100 / 0x200 / 0x300）；别的值必须被判非法（⛔ 不"认不出就当 0"）。纯函数，不读 env。
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
//   ⑦ `d3dx9-effect/effect-draw-unimplemented`：**202 的 Snow**（`vs_1_0`，本线不做 GPU 顶点着色）
//      **仍然具名拒绝**（bit 44 + 原因串含 id/technique/pass）—— 翻面没有把"没实现的"变成"静默接受"。
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

#include "check.h"
#include "fixture.h"
#include "runtime/d3d9.h"
#include "runtime/d3d9_effect_gpu.h"
#include "runtime/d3d9_pipeline.h"
#include "runtime/gpu_raster.h"
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

// =======================================================================================
// ① caps opt-in 的值域 / 映射（纯函数；不读 env）
// =======================================================================================
void caseCapsPsVersion() {
    lift_test::g_current_case = "d3dx9-effect/caps-ps-version";
    struct Row {
        std::uint32_t request;
        bool valid;
        std::uint32_t caps;
        float max_value;
    };
    const Row rows[] = {
        {0u, true, 0u, 0.0f},
        {0x100u, true, 0xFFFF0100u, 1.0f},
        {0x200u, true, 0xFFFF0200u, 8.0f},
        {0x300u, true, 0xFFFF0300u, 8.0f},
        // 非法：0x1FF / 0x400 / D3DCAPS9 的**整个字段值**（env 只收低 16 位）/ 0xFFFF0000
        {0x1FFu, false, 0u, 0.0f},
        {0x400u, false, 0u, 0.0f},
        {0xFFFF0200u, false, 0u, 0.0f},
        {0xFFFF0000u, false, 0u, 0.0f},
        {0x40u, false, 0u, 0.0f},
    };
    for (const Row& r : rows) {
        std::uint32_t caps = 0xDEADBEEFu;
        float max_value = -1.0f;
        const bool ok = lift::d3d9::shaderCapsForPixelShaderVersion(r.request, &caps, &max_value);
        LIFT_CHECK_EQ_U32(ok ? 1u : 0u, r.valid ? 1u : 0u);
        if (ok) {
            LIFT_CHECK_EQ_U32(caps, r.caps);
            LIFT_CHECK(max_value == r.max_value);
        }
    }
    // 出参可空（实现内部有一次只用它做值域判定）
    LIFT_CHECK(lift::d3d9::shaderCapsForPixelShaderVersion(0x200u, nullptr, nullptr));
    LIFT_CHECK(!lift::d3d9::shaderCapsForPixelShaderVersion(0x201u, nullptr, nullptr));
    std::printf("caps 值域：0 / 0x100 / 0x200 / 0x300 合法，其余非法\n");
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
    bool rasterQueue(const lift::GpuRasterVertex*, std::size_t, const lift::GpuRasterTexture*,
                     const lift::GpuRasterState&, std::string*) override {
        ++plain_queued;
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
        LIFT_CHECK_EQ_I64(seen, 4);  // 200/201 的 Fade + 203 的两条（终止项不算）
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
// ⑧ P3 · **没实现的 pass 程序仍然具名拒绝**（bit 44 覆盖到实现集之外）
//
// 红了意味着什么：202 的 Snow（`vs_1_0`）被"静默接受"了 —— 那会按固定功能写出错误像素
//   而账上一个位都没有（本仓最贵的一类错）。
// 判据：HRESULT = D3DERR_INVALIDCALL ∧ `skipped_draws +1` ∧ 原因串含 **id=202 / Snow / pass=0 /
//   VertexShader / vs_1_0**（P3 把每一条具名原因都补上了资源上下文）∧ bit44 置位。
// ★ 为什么用 202 而不是 203：203 的两个 technique 在 P3 **真的画**（⑨/⑩ 钉住）；202 的
//   `vs_1_0` 是这条线故意不做的那一半（T&L 留在 CPU，design-notes 的 L3-tl-01）。
// =======================================================================================
void caseEffectDrawUnimplemented(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/effect-draw-unimplemented";
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
    const std::uint32_t dev_state = f.m.cpu().com->userOf(device);
    EffectMockRaster mock;
    f.m.setGpuRaster(&mock);

    const std::uint32_t proc_fx = f.use("d3dx9_43", "D3DXCreateEffectFromResourceA");
    const std::uint32_t out_ea = f.u32data(0);
    LIFT_CHECK_EQ_U32(f.call(proc_fx, {device, 0u, 202u, 0u, 0u, 0u, 0u, out_ea, 0u}), lift::d3dx9::kOk);
    const std::uint32_t effect = f.m.mem().rd32(out_ea);
    LIFT_CHECK_NE_U32(effect, 0u);
    if (effect == 0) return;
    const std::uint32_t s_tech = f.cstr("Snow");
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xE8u, {s_tech}), lift::d3dx9::kOk);
    const std::uint32_t passes_ea = f.u32data(0);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xFCu, {passes_ea, 0u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(f.m.mem().rd32(passes_ea), 1u);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x100u, {0u}), lift::d3dx9::kOk);
    {
        const lift::d3d9::EffectDrawScope sc = lift::d3d9::effectDrawScope();
        LIFT_CHECK(sc.active && sc.in_pass);
        LIFT_CHECK(!sc.program.implemented);
        // ★ 具名原因必须说清"哪一批"：资源 id / technique / pass 都在串里（P3 补的上下文）
        LIFT_CHECK(sc.program.reason.find("id=202") != std::string::npos);
        LIFT_CHECK(sc.program.reason.find("Snow") != std::string::npos);
        LIFT_CHECK(sc.program.reason.find("pass=0") != std::string::npos);
        LIFT_CHECK(sc.program.reason.find("VertexShader") != std::string::npos);
        LIFT_CHECK(sc.program.reason.find("vs_1_0") != std::string::npos);
        std::printf("具名原因 = %s\n", sc.program.reason.c_str());
    }
    std::vector<std::uint8_t> vb(3u * 20u, 0);
    for (std::uint32_t i = 0; i < 3u; ++i) {
        const float xyzw[4] = {10.0f + 10.0f * static_cast<float>(i), 10.0f, 0.5f, 1.0f};
        std::memcpy(vb.data() + i * 20u, xyzw, sizeof(xyzw));
        const std::uint32_t color = 0xFFFFFFFFu;
        std::memcpy(vb.data() + i * 20u + 16u, &color, 4);
    }
    const std::uint32_t vtx = f.data(vb.data(), vb.size());
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x164u, {0x0044u}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA4u, {}), lift::d3dx9::kOk);
    const std::uint64_t skipped_before = lift::d3d9::gpuCoverage().skipped_draws;
    const std::uint32_t hr = callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleList, 1u, vtx, 20u});
    LIFT_CHECK_EQ_U32(hr, lift::d3dx9::kInvalidCall);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::gpuCoverage().skipped_draws),
                      static_cast<std::int64_t>(skipped_before + 1u));
    LIFT_CHECK_EQ_U32(mock.effect_queued, 0u);
    std::string reason;
    std::uint64_t count = 0;
    for (const auto& kv : lift::d3d9::gpuCoverage().skipped_reasons) {
        if (kv.first.find("VertexShader") != std::string::npos) {
            reason = kv.first;
            count = kv.second;
        }
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(count), 1);
    LIFT_CHECK(reason.find("id=202") != std::string::npos);
    LIFT_CHECK(reason.find("Snow") != std::string::npos);
    LIFT_CHECK(reason.find("pass=0") != std::string::npos);
    {
        lift::d3d9::PipelineInfo info;
        LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(f.m.cpu(), dev_state, &info, nullptr));
        constexpr std::uint32_t kEffectBit = 44u;
        LIFT_CHECK((info.unsupported[kEffectBit / 32u] & (1u << (kEffectBit % 32u))) != 0);
    }
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x108u, {}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x10Cu, {}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA8u, {}), lift::d3dx9::kOk);
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
    caseObjectSlots(img);    // ④
    caseFxParse(img);        // ⑤（P2：FX 文本解析）
    caseEffectDrawRecorded(img);       // ⑥（P2：翻面 P1 的"必须拒绝"）
    caseEffectDrawBlurRecorded(img);   // ⑨（P3：203 的 SlideBlur / ZoomBlur 翻面）
    caseEffectDrawUnimplemented(img);  // ⑦（P3：202 仍然具名拒绝）
    if (caseFadePixels(img) != 0) return 1;  // ⑧（P2：像素 oracle；没有 GPU ⇒ SKIP 77）
    if (caseBlurPixels(img) != 0) return 1;  // ⑩（P3：203 的像素 oracle；没有 GPU ⇒ SKIP 77）
    return finish("d3dx9-effect");
}
