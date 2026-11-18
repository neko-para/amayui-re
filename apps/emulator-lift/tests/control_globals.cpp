// emulator-lift · **script global typed 读写**的 pure 守卫（顶层无条件注册 ⇒ pure 档）
//
// 分两层，夹具都**不碰** Control / socket / JSON 之外的传输（引擎层连 JSON 都不碰）：
//   ① AGE 引擎层（`runtime/script_globals.h`）：假 Engine + 假池基址 + 假 key 直接驱动
//      `readGlobal` / `writeGlobal`。这一层是**控制层用例**与**将来 QJS 用例**的共用底座
//      （⚠ 共用的是**同一份引擎层实现**，不是"跨引擎通用的原语"）。
//   ② 控制层形状（`runtime/control_global.h`）：JSON 请求 → 参数校验（含十六进制口径）→ 模块 →
//      `data.global` 文本。只断言形状 / 往返，⛔ 不断言"某个 global 当前的值"（AGENTS §9）。
//
// 判据（红了意味着什么）：
//   ① int 的 ENC→DEC 逐位回原值（含 0xFFFFFFFF / 0x7FFFFFFF / 0x80000000）——**槽里不是明文**；
//   ② float 往返：-0.0 / NaN 只断言**位模式**（⛔ 不断言 NaN == NaN）；
//   ③ 越界 id（容量边界 ±1 / engine 未发布 / 池基址 0 / 地址回绕）一律 `id_out_of_range`；
//   ④ kind 与"能判定的东西"不符时的行为：kind=String 未实现 ⇒ `type_mismatch`；
//      index / int-uint value 的**纯十进制**一律拒（只认 0x 十六进制，父级 2026-11-19 裁决）；
//   ⑤ JSON 请求 / 答复往返：`{kind,index,value,raw}` 的 value/raw 是 0x 位模式，不是十进制数。
//
// ★ 为什么这些能红：把 `script_globals.cpp` 的 ENC 换成明文写 ⇒ ① 的往返立刻红（见 mutate-check
//   的 `script-globals-int-enc-plaintext`）；把十六进制解析改回"自动识别十进制" ⇒ ④ 的十进制拒绝红
//   （`control-global-hex-only`）。
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "check.h"
#include "runtime/control_global.h"
#include "runtime/engine_marker.h"
#include "runtime/memory.h"
#include "runtime/native_overrides.h"  // `sortKeyOf`（DEC 的既有消费者；一份公式两处共用）
#include "runtime/script_globals.h"
#include "runtime/value_codec.h"

namespace {

// =====================================================================================
// 夹具：假 Engine + 假池基址 + 假 key（全部合成；⛔ 不读游戏 / LFS）
//   ★ 布局照 `tests/control_snapshot.cpp:32-73`：`kEngineSingletonGlobal`（0x55E1BC）本身
//     落在提交区里 ⇒ 写"Engine 指针"与"Engine 结构"可以用同一块假内存。
// =====================================================================================
constexpr std::uint32_t kBase = 0x00480000u;
constexpr std::uint32_t kBaseSize = 0x00100000u;  // [0x480000, 0x580000)
constexpr std::uint32_t kEngine = kBase;
constexpr std::uint32_t kIntPool = 0x004A0000u;    // 假 int 池（4 B / 元素）
constexpr std::uint32_t kFloatPool = 0x004B0000u;  // 假 float 池（4 B / 元素）
constexpr std::uint32_t kKey = 0x1234ABCDu;
constexpr std::uint32_t kKey2 = 0x0BADF00Du;

// ★ 计数语义：fill 循环按 `count + 余量 + 1` 分配 ⇒ 从基址起可寻址 `0..count`（含 count）。
//   ⇒ 边界用例用 `index == count`（合法）/ `index == count + 1`（越界）。
//   count 取 `0xB1B6`（= 45494）是为了同一份夹具能端到端跑脚本里的真实 id：
//   池区 [0x4A0000, 0x4A0000 + 4*0xB1B6) = [0x4A0000, 0x4CC6D8) 仍在提交区 [0x480000, 0x580000) 内。
constexpr std::uint32_t kIntCount = 0xB1B6u;
constexpr std::uint32_t kFloatCount = 0xB1B6u;

bool put32(lift::GuestMemory& mem, std::uint32_t ea, std::uint32_t v) {
    if (!mem.committed(ea, 4u)) return false;
    mem.wr32(ea, v);
    return true;
}

bool makeFakeEngine(lift::GuestMemory& mem) {
    if (!mem.reserve()) return false;
    if (!mem.commitZeroed(kBase, kBaseSize, lift::RegionKind::Heap, "fake-script-globals")) return false;
    if (!put32(mem, lift::kEngineSingletonGlobal, kEngine)) return false;
    if (!put32(mem, kEngine + lift::kScriptGlobalIntBaseOffset, kIntPool)) return false;
    if (!put32(mem, kEngine + lift::kScriptGlobalIntCountOffset, kIntCount)) return false;
    if (!put32(mem, kEngine + lift::kScriptGlobalFloatBaseOffset, kFloatPool)) return false;
    if (!put32(mem, kEngine + lift::kScriptGlobalFloatCountOffset, kFloatCount)) return false;
    return put32(mem, kEngine + lift::kScriptGlobalIntKeyOffset, kKey);
}

std::uint32_t intAddr(std::uint32_t index) { return kIntPool + lift::kScriptGlobalSlotBytes * index; }
std::uint32_t floatAddr(std::uint32_t index) { return kFloatPool + lift::kScriptGlobalSlotBytes * index; }

void expectError(bool ok, lift::GlobalError err, lift::GlobalError want, const char* what) {
    if (ok || err != want) {
        std::printf("[err] %s: ok=%d err=%s (want %s)\n", what, ok ? 1 : 0, lift::globalErrorName(err),
                    lift::globalErrorName(want));
    }
    LIFT_CHECK(!ok);
    LIFT_CHECK(err == want);
}

// =====================================================================================
// ① int 的 ENC → DEC 逐位回原值；② key 是运行期的、不是默认 0
// =====================================================================================
void caseIntCodec() {
    ::lift_test::g_current_case = "int-enc-dec-roundtrip";
    const std::vector<std::uint32_t> values = {
        0u, 1u, 2u, 0x10u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x80000000u, 0xDEADBEEFu, 0x0000B1B6u,
    };
    for (std::uint32_t v : values) {
        const std::uint32_t enc = lift::encodeInt(v, kKey);
        LIFT_CHECK_EQ_U32(lift::decodeInt(enc, kKey), v);
        // ★ 既有实现（`native_overrides.cpp::sortKeyOf`）= DEC：两处必须是同一份公式。
        LIFT_CHECK_EQ_U32(lift::sortKeyOf(kKey, v), lift::decodeInt(v, kKey));
    }
    // ★ "不是明文写"的机制面：key 非 0 时码字必须与明文不同；换 key ⇒ 码字 / 解码都变。
    LIFT_CHECK_NE_U32(lift::encodeInt(0x10u, kKey), 0x10u);
    LIFT_CHECK_NE_U32(lift::encodeInt(0u, kKey), 0u);
    LIFT_CHECK_NE_U32(lift::encodeInt(0x10u, kKey), lift::encodeInt(0x10u, kKey2));
    LIFT_CHECK_NE_U32(lift::decodeInt(0x10u, kKey), lift::decodeInt(0x10u, kKey2));
    std::printf("[codec] enc/dec round-trip over %zu values; key-sensitive=1\n", values.size());
}

// =====================================================================================
// ③ 通过假 guest 内存写 int：槽里必须是 ENC，不是明文；读回原值
// =====================================================================================
void caseIntViaGuest() {
    ::lift_test::g_current_case = "int-via-guest";
    lift::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));

    // ---- 0xFFFFFFFF（int 语义 = -1 的二补码位模式）----
    lift::GlobalValue w;
    w.kind = lift::GlobalKind::Int;
    w.u32 = 0xFFFFFFFFu;
    lift::GlobalError err = lift::GlobalError::None;
    LIFT_CHECK(lift::writeGlobal(mem, 3u, lift::GlobalKind::Int, w, &err));
    LIFT_CHECK(err == lift::GlobalError::None);
    const std::uint32_t want_enc = lift::encodeInt(0xFFFFFFFFu, kKey);
    LIFT_CHECK_MEM_U32(mem, intAddr(3u), want_enc);  // ★ 槽里是 ENC(value, key)，⛔ 不是 0xFFFFFFFF
    LIFT_CHECK_NE_U32(want_enc, 0xFFFFFFFFu);

    lift::GlobalValue r;
    LIFT_CHECK(lift::readGlobal(mem, 3u, lift::GlobalKind::Int, &r, &err));
    LIFT_CHECK_EQ_U32(r.raw, want_enc);
    LIFT_CHECK_EQ_U32(r.u32, 0xFFFFFFFFu);
    std::printf("[int] index=3 raw=0x%08X value=0x%08X (plaintext would be 0xFFFFFFFF)\n", r.raw, r.u32);

    // ---- 边界 0x7FFFFFFF / 0x80000000（符号不参与编码）----
    for (std::uint32_t v : {0x7FFFFFFFu, 0x80000000u}) {
        w.u32 = v;
        LIFT_CHECK(lift::writeGlobal(mem, 4u, lift::GlobalKind::Int, w, &err));
        lift::GlobalValue rr;
        LIFT_CHECK(lift::readGlobal(mem, 4u, lift::GlobalKind::Int, &rr, &err));
        LIFT_CHECK_EQ_U32(rr.u32, v);
        LIFT_CHECK_EQ_U32(rr.raw, lift::encodeInt(v, kKey));
    }

    // ---- uint 与 int 是**同一个槽 / 同一份编码**（不是新类型）----
    w.kind = lift::GlobalKind::Uint;
    w.u32 = 0x80000000u;
    LIFT_CHECK(lift::writeGlobal(mem, 5u, lift::GlobalKind::Uint, w, &err));
    lift::GlobalValue ri;
    LIFT_CHECK(lift::readGlobal(mem, 5u, lift::GlobalKind::Int, &ri, &err));
    LIFT_CHECK_EQ_U32(ri.u32, 0x80000000u);
    LIFT_CHECK_EQ_U32(ri.raw, lift::encodeInt(0x80000000u, kKey));

    // ---- 容量边界：count 本身合法、count + 1 越界 ----
    w.kind = lift::GlobalKind::Int;
    w.u32 = 0x1234u;
    LIFT_CHECK(lift::writeGlobal(mem, kIntCount, lift::GlobalKind::Int, w, &err));
    err = lift::GlobalError::None;
    const bool over_write = lift::writeGlobal(mem, kIntCount + 1u, lift::GlobalKind::Int, w, &err);
    expectError(over_write, err, lift::GlobalError::IdOutOfRange, "int index > count (write)");
    lift::GlobalValue rr;
    err = lift::GlobalError::None;
    const bool over_read = lift::readGlobal(mem, kIntCount + 1u, lift::GlobalKind::Int, &rr, &err);
    expectError(over_read, err, lift::GlobalError::IdOutOfRange, "int index > count (read)");

    // ---- engine 未发布（空 GuestMemory）⇒ id 空间为空 ----
    lift::GuestMemory empty;
    lift::GlobalValue none;
    err = lift::GlobalError::None;
    const bool no_engine = lift::readGlobal(empty, 0u, lift::GlobalKind::Int, &none, &err);
    expectError(no_engine, err, lift::GlobalError::IdOutOfRange, "engine unpublished");

    // ---- 池基址为 0 ⇒ id 空间为空（⛔ 不要读 base+4*i 去撞未提交页）----
    lift::GuestMemory mem2;
    LIFT_CHECK(makeFakeEngine(mem2));
    LIFT_CHECK(put32(mem2, kEngine + lift::kScriptGlobalIntBaseOffset, 0u));
    err = lift::GlobalError::None;
    const bool no_base = lift::readGlobal(mem2, 0u, lift::GlobalKind::Int, &none, &err);
    expectError(no_base, err, lift::GlobalError::IdOutOfRange, "pool base == 0");

    // ---- 计数被写成 0xFFFFFFFF 且 id 也极大 ⇒ 地址回绕必须拒（⛔ 不允许静默回绕到低地址）----
    lift::GuestMemory mem3;
    LIFT_CHECK(makeFakeEngine(mem3));
    LIFT_CHECK(put32(mem3, kEngine + lift::kScriptGlobalIntCountOffset, 0xFFFFFFFFu));
    err = lift::GlobalError::None;
    const bool overflow = lift::readGlobal(mem3, 0xFFFFFFFFu, lift::GlobalKind::Int, &none, &err);
    expectError(overflow, err, lift::GlobalError::IdOutOfRange, "address overflow");
}

// =====================================================================================
// ② float 往返：-0.0 / NaN 只断言位模式（⛔ 不断言 NaN 相等）
// =====================================================================================
void caseFloatBits() {
    ::lift_test::g_current_case = "float-bits-roundtrip";
    lift::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    lift::GlobalError err = lift::GlobalError::None;

    struct Case {
        std::uint32_t bits;
        bool finite;
        double value;  // finite 时的期望语义值
    };
    const std::vector<Case> cases = {
        {0x00000000u, true, 0.0},                      // +0.0
        {0x80000000u, true, -0.0},                     // -0.0（符号位必须留住）
        {0x3F800000u, true, 1.0},                      // 1.0
        {0xBF800000u, true, -1.0},                     // -1.0
        {0x3FC00000u, true, 1.5},                      // 1.5
        {0x7F7FFFFFu, true, 3.4028234663852886e38},    // FLT_MAX
        {0x7FC00001u, false, 0.0},                     // NaN（只断言位模式）
    };
    for (const Case& c : cases) {
        // 写：走**裸位模式**入口（JSON 层另有 finite 校验；模块这一层不许丢位）。
        LIFT_CHECK(lift::writeGlobalFloatBits(mem, 0u, c.bits, &err));
        LIFT_CHECK(err == lift::GlobalError::None);
        LIFT_CHECK_MEM_U32(mem, floatAddr(0u), c.bits);  // float 不过编解码：槽里就是 IEEE 位模式
        lift::GlobalValue r;
        LIFT_CHECK(lift::readGlobal(mem, 0u, lift::GlobalKind::Float, &r, &err));
        LIFT_CHECK_EQ_U32(r.raw, c.bits);
        if (c.finite) {
            LIFT_CHECK(r.f64 == c.value);
            if (c.bits == 0x80000000u) LIFT_CHECK(std::signbit(r.f64));  // -0.0 的符号位
            if (c.bits == 0x00000000u) LIFT_CHECK(!std::signbit(r.f64));
        } else {
            LIFT_CHECK(std::isnan(r.f64));  // ★ 只到这里：⛔ 不断言 NaN == NaN
        }
    }
    // 写有限值走语义入口（JSON 层将来用这条）：1.5 → 0x3FC00000。
    lift::GlobalValue v;
    v.kind = lift::GlobalKind::Float;
    v.f64 = 1.5;
    LIFT_CHECK(lift::writeGlobal(mem, 1u, lift::GlobalKind::Float, v, &err));
    LIFT_CHECK_MEM_U32(mem, floatAddr(1u), 0x3FC00000u);

    // 容量边界同 int：count 合法、count + 1 越界。
    LIFT_CHECK(lift::writeGlobalFloatBits(mem, kFloatCount, 0u, &err));
    err = lift::GlobalError::None;
    const bool over = lift::writeGlobalFloatBits(mem, kFloatCount + 1u, 0u, &err);
    expectError(over, err, lift::GlobalError::IdOutOfRange, "float index > count");
    std::printf("[float] %zu bit patterns round-trip; NaN asserted by bits only\n", cases.size());
}

// =====================================================================================
// ④ kind 的可判定部分 + 十六进制口径
// =====================================================================================
void caseKindAndHex() {
    ::lift_test::g_current_case = "kind-and-hex-literals";
    lift::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    lift::GlobalError err = lift::GlobalError::None;

    // kind=String：第二期未实现 ⇒ type_mismatch（⛔ 不猜布局、⛔ 不返回成功）。
    lift::GlobalValue r;
    err = lift::GlobalError::None;
    const bool str_read = lift::readGlobal(mem, 0u, lift::GlobalKind::String, &r, &err);
    expectError(str_read, err, lift::GlobalError::TypeMismatch, "kind=string read");
    lift::GlobalValue w;
    w.kind = lift::GlobalKind::String;
    err = lift::GlobalError::None;
    const bool str_write = lift::writeGlobal(mem, 0u, lift::GlobalKind::String, w, &err);
    expectError(str_write, err, lift::GlobalError::TypeMismatch, "kind=string write");

    // ★ 十六进制口径：index 与 int/uint 的 value **只认 0x 前缀**（父级 2026-11-19 追加裁决）。
    std::uint32_t out = 0;
    LIFT_CHECK(lift::parseGlobalIndexLiteral("0xb1b6", &out) == lift::GlobalError::None);
    LIFT_CHECK_EQ_U32(out, 0xB1B6u);
    LIFT_CHECK(lift::parseGlobalIndexLiteral("0xB1B6", &out) == lift::GlobalError::None);
    LIFT_CHECK_EQ_U32(out, 0xB1B6u);
    LIFT_CHECK(lift::parseGlobalIndexLiteral("0x0000b1b6", &out) == lift::GlobalError::None);
    LIFT_CHECK_EQ_U32(out, 0xB1B6u);
    LIFT_CHECK(lift::parseGlobalIndexLiteral("0xFFFFFFFF", &out) == lift::GlobalError::None);
    LIFT_CHECK_EQ_U32(out, 0xFFFFFFFFu);
    // ⛔ 纯十进制一律拒（**这条就是追加裁决的守卫**）
    LIFT_CHECK(lift::parseGlobalIndexLiteral("45494", &out) == lift::GlobalError::TypeMismatch);
    LIFT_CHECK(lift::parseGlobalIndexLiteral("b1b6", &out) == lift::GlobalError::TypeMismatch);
    LIFT_CHECK(lift::parseGlobalIndexLiteral("0x", &out) == lift::GlobalError::TypeMismatch);
    LIFT_CHECK(lift::parseGlobalIndexLiteral("0xGG", &out) == lift::GlobalError::TypeMismatch);
    LIFT_CHECK(lift::parseGlobalIndexLiteral("0x100000000", &out) == lift::GlobalError::TypeMismatch);
    LIFT_CHECK(lift::parseGlobalIndexLiteral("", &out) == lift::GlobalError::TypeMismatch);

    LIFT_CHECK(lift::parseGlobalValueLiteral("0x10", &out) == lift::GlobalError::None);
    LIFT_CHECK_EQ_U32(out, 0x10u);
    LIFT_CHECK(lift::parseGlobalValueLiteral("16", &out) == lift::GlobalError::TypeMismatch);
    std::printf("[hex] only 0x-prefixed literals accepted (decimal rejected)\n");
}

// =====================================================================================
// ⑤ JSON 请求 / 答复往返
// =====================================================================================
void caseJsonRoundTrip() {
    ::lift_test::g_current_case = "json-request-reply-roundtrip";

    // ---- 请求：index 是**字符串**（0x 十六进制；线上没有"JSON number 的十进制坐标"这回事）----
    lift::ControlGlobalRequest get =
        lift::parseControlGlobalRequest(R"({"op":"global-get","kind":"int","index":"0xb1b6"})", false);
    LIFT_CHECK(get.error == lift::GlobalError::None);
    LIFT_CHECK(get.kind == lift::GlobalKind::Int);
    LIFT_CHECK_EQ_U32(get.index, 0xB1B6u);
    LIFT_CHECK(!get.has_value);

    lift::ControlGlobalRequest dec =
        lift::parseControlGlobalRequest(R"({"op":"global-get","kind":"int","index":"45494"})", false);
    LIFT_CHECK(dec.error == lift::GlobalError::TypeMismatch);  // ★ 纯十进制 ⇒ bad_request
    LIFT_CHECK(!dec.detail.empty());
    std::printf("[json] decimal index rejected: %s\n", dec.detail.c_str());

    lift::ControlGlobalRequest set = lift::parseControlGlobalRequest(
        R"({"op":"global-set","kind":"int","index":"0xb1b6","value":"0x80000010"})", true);
    LIFT_CHECK(set.error == lift::GlobalError::None);
    LIFT_CHECK(set.has_value);
    LIFT_CHECK_EQ_U32(set.u32, 0x80000010u);
    LIFT_CHECK_EQ_U32(set.index, 0xB1B6u);

    // int 的 value 是字符串 hex；JSON number / 十进制字符串都拒。
    LIFT_CHECK(lift::parseControlGlobalRequest(
                   R"({"op":"global-set","kind":"int","index":"0x1","value":16})", true).error ==
               lift::GlobalError::TypeMismatch);
    LIFT_CHECK(lift::parseControlGlobalRequest(
                   R"({"op":"global-set","kind":"uint","index":"0x1","value":"16"})", true).error ==
               lift::GlobalError::TypeMismatch);

    // float 的 value 是**十进制 JSON number**（唯一例外）；字符串 / 溢出 / 非有限都拒。
    lift::ControlGlobalRequest f = lift::parseControlGlobalRequest(
        R"({"op":"global-set","kind":"float","index":"0x2","value":1.5})", true);
    LIFT_CHECK(f.error == lift::GlobalError::None);
    LIFT_CHECK(f.kind == lift::GlobalKind::Float);
    LIFT_CHECK(f.f64 == 1.5);
    LIFT_CHECK(lift::parseControlGlobalRequest(
                   R"({"op":"global-set","kind":"float","index":"0x2","value":"1.5"})", true).error ==
               lift::GlobalError::TypeMismatch);
    LIFT_CHECK(lift::parseControlGlobalRequest(
                   R"({"op":"global-set","kind":"float","index":"0x2","value":1e40})", true).error ==
               lift::GlobalError::EncodeFailed);

    // kind 缺失 / 未知 / string 未实现；index 缺失 / 不是字符串。
    LIFT_CHECK(lift::parseControlGlobalRequest(R"({"op":"global-get","index":"0x1"})", false).error ==
               lift::GlobalError::TypeMismatch);
    LIFT_CHECK(lift::parseControlGlobalRequest(R"({"op":"global-get","kind":"int32","index":"0x1"})", false).error ==
               lift::GlobalError::TypeMismatch);
    LIFT_CHECK(lift::parseControlGlobalRequest(R"({"op":"global-get","kind":"string","index":"0x1"})", false).error ==
               lift::GlobalError::TypeMismatch);
    LIFT_CHECK(lift::parseControlGlobalRequest(R"({"op":"global-get","kind":"int"})", false).error ==
               lift::GlobalError::TypeMismatch);
    LIFT_CHECK(lift::parseControlGlobalRequest(R"({"op":"global-get","kind":"int","index":1})", false).error ==
               lift::GlobalError::TypeMismatch);
    LIFT_CHECK(lift::parseControlGlobalRequest(R"({"op":"global-set","kind":"int","index":"0x1"})", true).error ==
               lift::GlobalError::TypeMismatch);

    // ---- 端到端：解析 → 写（ENC）→ 读（DEC）→ `data.global` 文本 ----
    lift::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    lift::GlobalValue wv;
    wv.kind = lift::GlobalKind::Int;
    wv.u32 = set.u32;
    lift::GlobalError err = lift::GlobalError::None;
    LIFT_CHECK(lift::writeGlobal(mem, set.index, set.kind, wv, &err));
    lift::GlobalValue rv;
    LIFT_CHECK(lift::readGlobal(mem, set.index, set.kind, &rv, &err));
    const std::string text = lift::controlGlobalJson(rv);
    std::printf("[json] data.global = %s\n", text.c_str());
    LIFT_CHECK(text.find("\"kind\":\"int\"") != std::string::npos);
    LIFT_CHECK(text.find("\"index\":\"0x0000B1B6\"") != std::string::npos);
    LIFT_CHECK(text.find("\"value\":\"0x80000010\"") != std::string::npos);  // ★ 往返回原值（hex）
    LIFT_CHECK(text.find("\"raw\":\"0x") != std::string::npos);
    LIFT_CHECK(text.find("\"value\":2147483664") == std::string::npos);  // ⛔ 不是十进制数

    // float 的答复：value 是 number，raw 是 0x 位模式。
    lift::GlobalValue fv;
    fv.kind = lift::GlobalKind::Float;
    fv.f64 = 1.5;
    LIFT_CHECK(lift::writeGlobal(mem, 2u, lift::GlobalKind::Float, fv, &err));
    lift::GlobalValue fr;
    LIFT_CHECK(lift::readGlobal(mem, 2u, lift::GlobalKind::Float, &fr, &err));
    const std::string ft = lift::controlGlobalJson(fr);
    std::printf("[json] float data.global = %s\n", ft.c_str());
    LIFT_CHECK(ft.find("\"value\":1.5") != std::string::npos);
    LIFT_CHECK(ft.find("\"raw\":\"0x3FC00000\"") != std::string::npos);
}

}  // namespace

int main() {
    caseIntCodec();
    caseIntViaGuest();
    caseFloatBits();
    caseKindAndHex();
    caseJsonRoundTrip();
    return lift_test::finish("lift-control-globals");
}
