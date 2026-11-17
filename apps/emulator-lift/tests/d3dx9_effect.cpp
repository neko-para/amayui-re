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
//
// ★ 输入：`LIFT_AGE_EXE`（CMake 注入 `${AGE_SECTFIX_EXE}`；缺 ⇒ LIFT_SKIP 77）。
//   ①② 不读语料 ⇒ 没有语料时它们**已经跑过**，`SKIP` 只表示剩下两条没跑。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/d3d9.h"
#include "runtime/d3d9_pipeline.h"
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

std::uint64_t effectRefusalCount(const std::string** reason_out) {
    const lift::d3d9::GpuCoverage& cov = lift::d3d9::gpuCoverage();
    std::uint64_t n = 0;
    for (const auto& kv : cov.skipped_reasons) {
        if (kv.first.find("尚未实现执行") != std::string::npos) {
            n = kv.second;
            if (reason_out != nullptr) *reason_out = &kv.first;
        }
    }
    return n;
}

void caseEffectDrawRefused(const lift::PeImage& img) {
    lift_test::g_current_case = "d3dx9-effect/effect-draw-refused";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    LIFT_CHECK(setupMachine(f));
    f.m.cpu().image = &img;

    const std::uint32_t proc3d = f.use("d3d9", "Direct3DCreate9");
    LIFT_CHECK_NE_U32(proc3d, 0u);
    const std::uint32_t d3d9 = f.call(proc3d, {lift::d3d9::kSdkVersion});
    LIFT_CHECK_NE_U32(d3d9, 0u);
    if (d3d9 == 0) return;
    std::uint32_t device = 0;
    LIFT_CHECK(createDevice(f, d3d9, &device));
    if (device == 0) return;
    const std::uint32_t dev_state = f.m.cpu().com->userOf(device);
    LIFT_CHECK_NE_U32(dev_state, 0u);

    // ① effect（用**真**设备 EA —— 只记状态，不校验；这里与 E2E 同形）
    const std::uint32_t proc = f.use("d3dx9_43", "D3DXCreateEffectFromResourceA");
    const std::uint32_t out_ea = f.u32data(0);
    LIFT_CHECK_EQ_U32(f.call(proc, {device, 0u, 201u, 0u, 0u, 0u, 0u, out_ea, 0u}),
                      lift::d3dx9::kOk);
    const std::uint32_t effect = f.m.mem().rd32(out_ea);
    LIFT_CHECK_NE_U32(effect, 0u);
    if (effect == 0) return;

    // ② 作用域：没 Begin ⇒ 不适用；Begin 但没 BeginPass ⇒ 仍不适用；BeginPass 之后才成立
    {
        std::string note;
        LIFT_CHECK(!lift::d3d9::effectDrawNotExecuted(&note));
        LIFT_CHECK(lift::d3d9::effectDrawScope().active == false);
    }
    const std::uint32_t s_tech = f.cstr("ZoomBlur");
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xE8u, {s_tech}), lift::d3dx9::kOk);
    const std::uint32_t passes_ea = f.u32data(0);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0xFCu, {passes_ea, 0u}), lift::d3dx9::kOk);
    {
        const lift::d3d9::EffectDrawScope sc = lift::d3d9::effectDrawScope();
        LIFT_CHECK(sc.active);
        LIFT_CHECK(!sc.in_pass);  // 只在 Begin 里、还没进 pass ⇒ 不算"这一批该由 shader 画"
        std::string note;
        LIFT_CHECK(!lift::d3d9::effectDrawNotExecuted(&note));
    }
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x100u, {0u}), lift::d3dx9::kOk);
    {
        const lift::d3d9::EffectDrawScope sc = lift::d3d9::effectDrawScope();
        LIFT_CHECK(sc.active && sc.in_pass);
        LIFT_CHECK_EQ_U32(sc.resource_id, 201u);
        LIFT_CHECK(sc.technique == "ZoomBlur");
        LIFT_CHECK_EQ_U32(sc.pass_index, 0u);
    }
    std::string note;
    LIFT_CHECK(lift::d3d9::effectDrawNotExecuted(&note));
    LIFT_CHECK(note.find("尚未实现执行") != std::string::npos);
    LIFT_CHECK(note.find("id=201") != std::string::npos);
    LIFT_CHECK(note.find("ZoomBlur") != std::string::npos);
    LIFT_CHECK(note.find("pass=0") != std::string::npos);
    std::printf("具名原因 = %s\n", note.c_str());

    // ③ 真提交一笔 draw（FVF = XYZRHW|DIFFUSE 0x0044；3 顶点 × 20 B）
    std::vector<std::uint8_t> vb(3u * 20u, 0);
    for (std::uint32_t i = 0; i < 3u; ++i) {
        const float xyzw[4] = {10.0f + 10.0f * static_cast<float>(i), 10.0f, 0.5f, 1.0f};
        std::memcpy(vb.data() + i * 20u, xyzw, sizeof(xyzw));
        const std::uint32_t color = 0xFFFFFFFFu;
        std::memcpy(vb.data() + i * 20u + 16u, &color, 4);
    }
    const std::uint32_t vtx = f.data(vb.data(), vb.size());
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0x164u, {0x0044u}), lift::d3d9::kOk);  // SetFVF
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA4u, {}), lift::d3d9::kOk);         // BeginScene
    const std::uint64_t skipped_before = lift::d3d9::gpuCoverage().skipped_draws;
    const std::uint32_t hr =
        callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleList, 1u, vtx, 20u});
    LIFT_CHECK_EQ_U32(hr, lift::d3d9::kInvalidCall);
    const lift::d3d9::GpuCoverage& cov = lift::d3d9::gpuCoverage();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(cov.skipped_draws),
                      static_cast<std::int64_t>(skipped_before + 1u));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(effectRefusalCount(nullptr)), 1);
    const std::string* reason = nullptr;
    effectRefusalCount(&reason);
    LIFT_CHECK(reason != nullptr);
    if (reason != nullptr) {
        LIFT_CHECK(reason->find("id=201") != std::string::npos);
        LIFT_CHECK(reason->find("ZoomBlur") != std::string::npos);
        LIFT_CHECK(reason->find("pass=0") != std::string::npos);
    }
    // ④ 记账位（bit44 = `kUnEffectShaderNotExecuted`）
    {
        lift::d3d9::PipelineInfo info;
        LIFT_CHECK(lift::d3d9::pipelineInfoOfDevice(f.m.cpu(), dev_state, &info, nullptr));
        constexpr std::uint32_t kEffectBit = 44u;
        LIFT_CHECK((info.unsupported[kEffectBit / 32u] & (1u << (kEffectBit % 32u))) != 0);
        std::printf("bit%u 置位（skip 原因 = %s）\n", kEffectBit, reason ? reason->c_str() : "?");
    }

    // ⑤ EndPass + End ⇒ 作用域关；同一笔 draw 不再记这条原因（拒绝是作用域相关的）
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x108u, {}), lift::d3dx9::kOk);
    LIFT_CHECK_EQ_U32(callMethod(f, effect, 0x10Cu, {}), lift::d3dx9::kOk);
    {
        std::string n2;
        LIFT_CHECK(!lift::d3d9::effectDrawNotExecuted(&n2));
        LIFT_CHECK(!lift::d3d9::effectDrawScope().active);
    }
    const std::uint32_t hr2 =
        callMethod(f, device, 0x14Cu, {lift::d3d9::kPtTriangleList, 1u, vtx, 20u});
    // 本夹具没有 GPU 后端 ⇒ 仍然拒，但**不是**我们这条原因（⛔ 作用域关了以后不许再记它）。
    //   ★ `gpuCoverage` 每次 `Machine::run()` 都 reset（夹具一次 guest 调用 = 一次 run）⇒ 这里
    //     读的是**这一笔 draw 所在 run** 的账：skipped=1（no-backend），我们的原因 = 0。
    LIFT_CHECK_EQ_U32(hr2, lift::d3d9::kInvalidCall);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::d3d9::gpuCoverage().skipped_draws), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(effectRefusalCount(nullptr)), 0);
    LIFT_CHECK_EQ_U32(callMethod(f, device, 0xA8u, {}), lift::d3d9::kOk);  // EndScene
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
    caseEffectDrawRefused(img);  // ⑤（P1 临时判据）
    return finish("d3dx9-effect");
}
