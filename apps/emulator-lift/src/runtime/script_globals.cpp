// emulator-lift · script global typed 读写的**AGE 引擎层实现**（控制层与将来的 QJS 侧共用同一份）
//
// ★ 口径见 `runtime/script_globals.h`（⚠ 是否被其它 AGE 作品复用 = 待证；本模块只承诺"可替换"）。
// ★ 三条硬纪律（在这个文件里逐条可见）：
//   ① **每次现读** `Engine` / 池基址 / 计数 / key —— `0x6d` 的 memflip 只对调 `+0/+4` 两个槽，
//      缓存基址就会在翻页后写错组（analysis §2.2 / §2.4）；
//   ② int/uint 槽里是 `ENC(value, key)`，**⛔ 不是明文**（写明文 ⇒ 引擎 DEC 出垃圾）；
//   ③ 所有 guest 读都先 `committed()`：`GuestMemory::rd32` 在非 safe mode 下**不检查**页是否提交
//      （`memory.h:331-337`）⇒ 少了这一句就是野指针崩溃，不是"读到 0"。
#include "runtime/script_globals.h"

#include <cfloat>
#include <cmath>
#include <cstring>

#include "runtime/engine_marker.h"
#include "runtime/instr_hook.h"  // ★ S0：持久写赢过瞬时覆盖（见下面三处 `instrHookCancelPendingAt`）
#include "runtime/memory.h"
#include "runtime/value_codec.h"

namespace lift {
namespace {

// 一次寻址的结果：槽地址 + 它所属的 Engine（int/uint 还要用 engine 读 key）。
struct SlotLocation {
    std::uint32_t addr = 0;
    std::uint32_t engine = 0;
};

// `Engine + 0x5D810` 是本模块用到的最大偏移（string 池基址；本实现不读它，但上界留足）。
constexpr std::uint32_t kMaxEngineOffset = kScriptGlobalIntKeyOffset;  // 0x5EC8C 更大，取它
static_assert(kMaxEngineOffset >= kScriptGlobalFloatBaseOffset, "上界必须覆盖所有偏移");

bool readSlot32(const GuestMemory& mem, std::uint32_t ea, std::uint32_t* out) {
    // ★ 非 safe mode 下 `rd32` 不查提交位（memory.h）⇒ 这里必须自己先查。
    if (!mem.committed(ea, 4u)) return false;
    *out = mem.rd32(ea);
    return true;
}

// 寻址：engine 未发布 / 池基址 0 / `id > count` / 地址回绕 / 槽未提交 ⇒ IdOutOfRange。
GlobalError locateSlot(const GuestMemory& mem, GlobalKind kind, std::uint32_t id, SlotLocation* out) {
    if (kind == GlobalKind::String) return GlobalError::TypeMismatch;  // 第二期未实现（布局不在 spec）

    std::uint32_t engine = 0;
    if (!readSlot32(mem, kEngineSingletonGlobal, &engine) || engine == 0u) {
        return GlobalError::IdOutOfRange;  // boot 早期 / 退出后：id 空间为空
    }
    // 加偏移可能把 uint32 加回绕 ⇒ 先拒（否则会读到低地址的无关内存）。
    if (engine > 0xFFFFFFFFu - kMaxEngineOffset) return GlobalError::IdOutOfRange;

    const std::uint32_t base_off =
        kind == GlobalKind::Float ? kScriptGlobalFloatBaseOffset : kScriptGlobalIntBaseOffset;
    const std::uint32_t count_off =
        kind == GlobalKind::Float ? kScriptGlobalFloatCountOffset : kScriptGlobalIntCountOffset;

    std::uint32_t base = 0;
    std::uint32_t count = 0;
    if (!readSlot32(mem, engine + base_off, &base) || !readSlot32(mem, engine + count_off, &count)) {
        return GlobalError::IdOutOfRange;
    }
    if (base == 0u) return GlobalError::IdOutOfRange;  // 池没分配 ⇒ id 空间为空

    // ★ 计数语义：从基址起可寻址 `0..count`（含 count；fill 循环按 count + 余量 + 1 分配）。
    if (id > count) return GlobalError::IdOutOfRange;

    const std::uint64_t offset = static_cast<std::uint64_t>(kScriptGlobalSlotBytes) * id;
    if (static_cast<std::uint64_t>(base) + offset > 0xFFFFFFFFull) return GlobalError::IdOutOfRange;
    const std::uint32_t addr = base + static_cast<std::uint32_t>(offset);
    if (!mem.committed(addr, 4u)) return GlobalError::IdOutOfRange;

    out->addr = addr;
    out->engine = engine;
    return GlobalError::None;
}

// int / uint 的编码 key：每个引擎实例一份、运行期生成；读不到 ⇒ 编 / 解不了码。
GlobalError readIntKey(const GuestMemory& mem, std::uint32_t engine, std::uint32_t* key) {
    if (!readSlot32(mem, engine + kScriptGlobalIntKeyOffset, key)) return GlobalError::EncodeFailed;
    return GlobalError::None;
}

}  // namespace

const char* globalKindName(GlobalKind kind) {
    switch (kind) {
        case GlobalKind::Int: return "int";
        case GlobalKind::Uint: return "uint";
        case GlobalKind::Float: return "float";
        case GlobalKind::String: return "string";
    }
    return "int";
}

const char* globalErrorName(GlobalError err) {
    switch (err) {
        case GlobalError::None: return "none";
        case GlobalError::IdOutOfRange: return "id_out_of_range";
        case GlobalError::TypeMismatch: return "type_mismatch";
        case GlobalError::EncodeFailed: return "encode_failed";
    }
    return "none";
}

const char* globalErrorDetail(GlobalError err) {
    switch (err) {
        case GlobalError::None:
            return "ok";
        case GlobalError::IdOutOfRange:
            return "index 超出该 kind 的运行期池容量（或引擎未发布 / 池基址为 0 / 槽未提交）";
        case GlobalError::TypeMismatch:
            return "kind 与该 id 的类型族不符（只在能判定时报；kind=string 本期未实现）";
        case GlobalError::EncodeFailed:
            return "值在该 kind 的表示范围内无法编码（或 int 编码 key 读不到）";
    }
    return "internal";
}

bool readGlobal(const GuestMemory& mem, std::uint32_t id, GlobalKind kind, GlobalValue* out,
                GlobalError* err) {
    auto set = [err](GlobalError e) {
        if (err != nullptr) *err = e;
        return e == GlobalError::None;
    };
    if (out == nullptr) return set(GlobalError::TypeMismatch);

    SlotLocation slot;
    const GlobalError located = locateSlot(mem, kind, id, &slot);
    if (located != GlobalError::None) return set(located);

    std::uint32_t raw = 0;
    if (!readSlot32(mem, slot.addr, &raw)) return set(GlobalError::IdOutOfRange);

    out->kind = kind;
    out->index = id;
    out->raw = raw;
    out->u32 = 0;
    out->f64 = 0.0;
    if (kind == GlobalKind::Float) {
        // float 不过编解码：槽里就是 IEEE-754 裸 32 位（`CORE-POOL-FLOAT-VALUE`）。
        float f = 0.0f;
        std::memcpy(&f, &raw, sizeof(f));
        out->u32 = raw;  // 位模式（诊断；与 raw 相同）
        out->f64 = static_cast<double>(f);
    } else {
        std::uint32_t key = 0;
        const GlobalError keyed = readIntKey(mem, slot.engine, &key);
        if (keyed != GlobalError::None) return set(keyed);
        out->u32 = decodeInt(raw, key);
    }
    return set(GlobalError::None);
}

bool writeGlobal(GuestMemory& mem, std::uint32_t id, GlobalKind kind, const GlobalValue& value,
                 GlobalError* err) {
    auto set = [err](GlobalError e) {
        if (err != nullptr) *err = e;
        return e == GlobalError::None;
    };
    if (value.kind != kind) return set(GlobalError::TypeMismatch);

    SlotLocation slot;
    const GlobalError located = locateSlot(mem, kind, id, &slot);
    if (located != GlobalError::None) return set(located);

    if (kind == GlobalKind::Float) {
        // 语义值写：非有限 / 超出 float32 一律拒（位级写入走 `writeGlobalFloatBits`）。
        if (!std::isfinite(value.f64) || std::fabs(value.f64) > static_cast<double>(FLT_MAX)) {
            return set(GlobalError::EncodeFailed);
        }
        const float f = static_cast<float>(value.f64);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &f, sizeof(bits));
        mem.wr32(slot.addr, bits);
        instrHookCancelPendingAt(slot.addr);  // ★ S0：持久写赢（同一地址的补丁不再写回）
        return set(GlobalError::None);
    }

    std::uint32_t key = 0;
    const GlobalError keyed = readIntKey(mem, slot.engine, &key);
    if (keyed != GlobalError::None) return set(keyed);
    mem.wr32(slot.addr, encodeInt(value.u32, key));  // ★ ENC，⛔ 不是明文
    // ★ S0（缺陷 2 的另一半）：这次是**持久写** ⇒ 若同一地址正挂着一条 hook 的临时覆盖，
    //   撤销那次待恢复（"持久写赢过瞬时覆盖"；⛔ 不要等下一拍 CAS 才保住这次写）。
    instrHookCancelPendingAt(slot.addr);
    return set(GlobalError::None);
}

bool writeGlobalFloatBits(GuestMemory& mem, std::uint32_t id, std::uint32_t bits, GlobalError* err) {
    auto set = [err](GlobalError e) {
        if (err != nullptr) *err = e;
        return e == GlobalError::None;
    };
    SlotLocation slot;
    const GlobalError located = locateSlot(mem, GlobalKind::Float, id, &slot);
    if (located != GlobalError::None) return set(located);
    mem.wr32(slot.addr, bits);  // float 是裸 32 位：逐位写，不过 codec
    instrHookCancelPendingAt(slot.addr);  // ★ S0：与 `writeGlobal` 同一条口径
    return set(GlobalError::None);
}

}  // namespace lift
