// emulator-lift · **脚本指令位置钩子**（`runtime/instr_hook.*`）—— S0 的 pure 守卫
//
// ★ 判据（红了意味着什么）：
//   ① `instr-hook/patch-restore-cas`：覆盖窗口里**别人写了同一地址** ⇒ 恢复必须**不写回**
//      （当前值仍是别人的），并计 `restore_conflicts`（`restores` 不动）。红了 = 无条件写回
//      把别人的写静默抹掉（S0 缺陷 2 复发）。
//   ② `instr-hook/patch-restore-clean`：窗口里没人写 ⇒ 恢复成**原 raw**、`restores == 1`、
//      `restore_conflicts == 0`。红了 = CAS 把正常恢复也拒了（净变化不再是 0）。
//   ③ `instr-hook/no-injection`：`instrHookSetInjectionSites(0)`（= `--no-s7` / 闭包没覆盖派发 EA）
//      ⇒ `instrHookAdd` **失败**且 `err == "no_injection"`；恢复非 0 后又能装。
//      红了 = 返回一个"永远 hits=0"的假句柄（S0 缺陷 3 复发：插件只看到"什么都没发生"）。
//   ④ `instr-hook/owner-release`：`instrHookReleaseOwner(a)` 只删 a 的（b 还在）；
//      `instrHookClear()` 全清。红了 = Run 收口无法只撤自己的 hook（S0 缺陷 1 的机制面）。
//   ⑤ `instr-hook/patch-edit-cancels-pending`：窗口里走**持久写**（`GuestEditSink::globalSet`，
//      即 `ctx.edit.global.set` 的唯一实现）写同一地址 ⇒ pending 被**主动撤销**：下一拍既不写回、
//      也不记 `restores` / `restore_conflicts`，地址保持持久写的值。
//      红了 = 持久写被瞬时覆盖的恢复盖掉（"持久写赢过瞬时覆盖"只在 CAS 下"碰巧"成立）。
//
// ★ 夹具形状：一块假 `GuestMemory`（假 Engine / 帧记录 / 名字表 / int 池 / ptr local）+
//   一个只填了 `mem` 的 `X86` + 直接调 `instrHookProbe` —— ⛔ 不需要 AOT、不需要游戏安装 / LFS。
//   地址与 `tests/input_marker.cpp` / `tests/trigger_runner.cpp` 同一块 `[0x480000, 0x580000)`。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "check.h"
#include "runtime/edit_sink.h"       // ★ ctx.edit 的唯一实现（持久写那条判据）
#include "runtime/engine_marker.h"   // Engine 单例 / 帧记录 / 名字表（与实现同一份偏移）
#include "runtime/instr_hook.h"
#include "runtime/memory.h"
#include "runtime/script_globals.h"  // GlobalValue（edit 的实参）
#include "runtime/value_codec.h"     // encodeInt（⛔ 不写死编码后的数）
#include "runtime/x86.h"

namespace {

namespace L = lift;

// 假 Engine 落在 [0x00480000, 0x00580000)：覆盖单例全局 + cur + 帧记录 + FileDB + 名字表 + 池。
constexpr std::uint32_t kBase = 0x00480000u;
constexpr std::uint32_t kBaseSize = 0x00100000u;
constexpr std::uint32_t kEngineGlobal = 0x0055E1BCu;  // `kEngineSingletonGlobal`
constexpr std::uint32_t kEngine = kBase;
constexpr std::uint32_t kCurEa = kBase + L::kEngineCurOffset;   // 0x004DD880
constexpr std::uint32_t kFrameEa = kBase + L::kEngineFrameBase;  // 0x004DD894（第 0 条记录）

// 脚本体 / 名字表 / int 池 / ptr local。
constexpr std::uint32_t kBody = 0x004C0000u;      // `frame+0x00` 指向它（必须已提交）
constexpr std::uint32_t kNameBase = 0x004A0000u;  // 名字数组（80 字节一条）
constexpr std::uint32_t kIntPool = 0x004B0800u;   // int 池（`frame+0x20`）
constexpr std::uint32_t kIntCount = 0x100u;
constexpr std::uint32_t kPtrSlot = 0x004B0000u;   // `frame+0x2C`：local-ptr 0 的槽
constexpr std::uint32_t kTarget = 0x004B0100u;    // ptrLocal 0 指向的 dword（被覆盖的地址）
constexpr std::uint32_t kKey = 0x1234ABCDu;       // 运行期 ENC/DEC key（⛔ 不是缺省 0）
constexpr std::uint32_t kScriptId = 3u;
constexpr std::uint32_t kGlobalIndex = 4u;        // GlobalInt 的 id（池里第 4 个槽）
constexpr std::uint32_t kPosA = 0x0100u;          // 被 hook 的那条指令
constexpr std::uint32_t kPosB = 0x0108u;          // "下一条脚本指令"（⛔ 不命中 hook）

// `sampleFrame` 读的帧内偏移（真源 = `runtime/instr_hook.cpp` 的 kLocal*；这里照抄编号，
// ⛔ 不从被测实现里取：夹具要能独立判"我摆的格子对不对"）。
constexpr std::uint32_t kFrameBodyOff = 0x00u;
constexpr std::uint32_t kFramePcOff = 0x04u;
constexpr std::uint32_t kFrameIntCountOff = 0x08u;
constexpr std::uint32_t kFramePtrCountOff = 0x14u;
constexpr std::uint32_t kFrameIntBaseOff = 0x20u;
constexpr std::uint32_t kFramePtrBaseOff = 0x2Cu;
constexpr std::uint32_t kFrameIdentityOff = 0x3Cu;
// FileDB 根条目（`engine + kFileIndexOffset` 之后）。
constexpr std::uint32_t kFileDbEa = kBase + L::kFileIndexOffset;

const char kScript[] = "FIELD.BIN";

bool put32(L::GuestMemory& mem, std::uint32_t ea, std::uint32_t value) {
    if (!mem.committed(ea, 4u)) return false;
    mem.wr32(ea, value);
    return true;
}

std::uint32_t enc(std::uint32_t value) { return L::encodeInt(value, kKey); }

// 名字条目（80 字节 / 条，NUL 结尾）。
bool putName(L::GuestMemory& mem, std::uint32_t name_base, std::uint32_t id, const char* name) {
    const std::uint32_t entry = name_base + L::kFileNameStride * id;
    const std::size_t n = std::strlen(name);
    if (n >= L::kFileNameMaxBytes) return false;
    for (std::size_t i = 0; i <= n; ++i) {
        if (!mem.committed(entry + static_cast<std::uint32_t>(i), 1u)) return false;
        mem.wr8(entry + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>(name[i]));
    }
    return true;
}

// 假 Engine：单例 + cur=0 的帧记录 + 名字表 + int 池 + local-ptr。
bool makeFakeEngine(L::GuestMemory& mem) {
    if (!mem.reserve()) return false;
    if (!mem.commitZeroed(kBase, kBaseSize, L::RegionKind::Heap, "instr-hook-fake-engine")) return false;
    if (!put32(mem, kEngineGlobal, kEngine)) return false;
    if (!put32(mem, kCurEa, 0u)) return false;
    const std::uint32_t frame = kFrameEa;
    if (!put32(mem, frame + kFrameBodyOff, kBody)) return false;
    if (!put32(mem, frame + kFrameIntCountOff, 8u)) return false;
    if (!put32(mem, frame + kFramePtrCountOff, 2u)) return false;
    if (!put32(mem, frame + kFrameIntBaseOff, kIntPool)) return false;
    if (!put32(mem, frame + kFramePtrBaseOff, kPtrSlot)) return false;
    if (!put32(mem, frame + kFrameIdentityOff, kScriptId)) return false;
    if (!put32(mem, kPtrSlot, kTarget)) return false;  // local-ptr 0 → kTarget
    // 名字表（根条目）：count / 基址 / 第 kScriptId 条的名字。
    if (!put32(mem, kFileDbEa + L::kFileRootCountOffset, 8u)) return false;
    if (!put32(mem, kFileDbEa + L::kFileRootNameBaseOffset, kNameBase)) return false;
    if (!putName(mem, kNameBase, kScriptId, kScript)) return false;
    // int 池 + ENC key（`ctx.edit` 与 `GlobalInt` 覆盖共用这一份几何）。
    if (!put32(mem, kEngine + L::kScriptGlobalIntBaseOffset, kIntPool)) return false;
    if (!put32(mem, kEngine + L::kScriptGlobalIntCountOffset, kIntCount)) return false;
    if (!put32(mem, kEngine + L::kScriptGlobalIntKeyOffset, kKey)) return false;
    // 被覆盖的 dword 的初值：语义 3（⛔ 不是 0 —— 0 与"没写"分不开，CAS 判据会变弱）。
    return put32(mem, kTarget, enc(3u));
}

// 一次脚本派发（= AOT 在 3 个派发点注入的那次调用）：先把 PC 摆到 `pos`，再探测。
void dispatchAt(L::GuestMemory& mem, L::X86& cpu, std::uint32_t pos) {
    LIFT_CHECK(put32(mem, kFrameEa + kFramePcOff, kBody + (pos - L::kScriptHeaderLen)));
    L::instrHookProbe(cpu);
}

// 一次"另一条执行流直接写 guest"（模拟引擎自己 / 音频 feeder）。★ 故意**不走** `writeGlobal`：
// 那条路是**持久写**，会主动撤销 pending（见 case ⑤），而这条判的是 CAS 的"别人写"。
void foreignWrite(L::GuestMemory& mem, std::uint32_t ea, std::uint32_t raw) {
    LIFT_CHECK(put32(mem, ea, raw));
}

std::uint32_t addHook(std::uint64_t owner, std::uint32_t pos, bool* ok) {
    const char* err = nullptr;
    const std::uint32_t h = L::instrHookAdd(owner, kScript, pos, &err);
    if (ok != nullptr) *ok = (h != 0 && err == nullptr);
    if (h == 0) std::fprintf(stderr, "  instrHookAdd err=%s\n", err != nullptr ? err : "(null)");
    return h;
}

std::uint32_t targetGlobalAddr() {
    return kIntPool + L::kScriptGlobalSlotBytes * kGlobalIndex;
}

// =====================================================================================
// ① 窗口里"别人写" ⇒ CAS 拒绝写回 + 记 restore_conflicts
// =====================================================================================
void casePatchRestoreCas() {
    lift_test::g_current_case = "instr-hook/patch-restore-cas";
    L::instrHookClear();
    L::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    L::X86 cpu;
    cpu.mem = &mem;

    bool added = false;
    const std::uint32_t h = addHook(1u, kPosA, &added);
    LIFT_CHECK(added);
    const char* err = nullptr;
    LIFT_CHECK(L::instrHookSetPatch(h, true, L::InstrPatchKind::PtrDword, 0u, 7u, 0u, &err));

    // 命中一次：地址变成我写进去的编码值。
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK_EQ_U32(mem.rd32(kTarget), enc(7u));
    L::InstrHookStatus st;
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.hits), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.restores), 0);

    // 窗口内：另一条执行流把同一地址写成 9（明文 9 也行 —— CAS 只比 4 字节原样）。
    foreignWrite(mem, kTarget, 9u);
    // 下一条脚本指令派发 ⇒ 先恢复上一拍的覆盖。
    dispatchAt(mem, cpu, kPosB);
    // ★ 判据：别人的写还在，而且账上记的是"冲突"，不是"恢复"。
    LIFT_CHECK_EQ_U32(mem.rd32(kTarget), 9u);
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.restores), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.restore_conflicts), 1);
    L::instrHookReleaseOwner(1u);
    LIFT_CHECK(L::instrHookList().empty());
}

// =====================================================================================
// ② 窗口里没人写 ⇒ 恢复原 raw（净变化 0 的那一半）
// =====================================================================================
void casePatchRestoreClean() {
    lift_test::g_current_case = "instr-hook/patch-restore-clean";
    L::instrHookClear();
    L::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    L::X86 cpu;
    cpu.mem = &mem;

    bool added = false;
    const std::uint32_t h = addHook(2u, kPosA, &added);
    LIFT_CHECK(added);
    const char* err = nullptr;
    LIFT_CHECK(L::instrHookSetPatch(h, true, L::InstrPatchKind::PtrDword, 0u, 7u, 0u, &err));

    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK_EQ_U32(mem.rd32(kTarget), enc(7u));
    dispatchAt(mem, cpu, kPosB);
    LIFT_CHECK_EQ_U32(mem.rd32(kTarget), enc(3u));  // 原 raw
    L::InstrHookStatus st;
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.restores), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.restore_conflicts), 0);
    L::instrHookReleaseOwner(2u);
    LIFT_CHECK(L::instrHookList().empty());
}

// =====================================================================================
// ③ 没有注入站点 ⇒ at() 当场失败（不是"永远 hits=0"）
// =====================================================================================
void caseNoInjection() {
    lift_test::g_current_case = "instr-hook/no-injection";
    L::instrHookClear();
    L::instrHookSetInjectionSites(0u);
    const char* err = nullptr;
    const std::uint32_t denied = L::instrHookAdd(3u, kScript, kPosA, &err);
    LIFT_CHECK_EQ_U32(denied, 0u);
    LIFT_CHECK(err != nullptr && std::strcmp(err, "no_injection") == 0);
    LIFT_CHECK(L::instrHookList().empty());
    LIFT_CHECK(!L::instrHooksActive());

    // 恢复非 0 ⇒ 又能装（"默认视作可用"只在没人调时成立；调过 0 之后必须显式恢复）。
    L::instrHookSetInjectionSites(3u);
    bool added = false;
    const std::uint32_t h = addHook(3u, kPosA, &added);
    LIFT_CHECK(added);
    LIFT_CHECK(L::instrHooksActive());
    L::instrHookReleaseOwner(3u);
    LIFT_CHECK(L::instrHookList().empty());
}

// =====================================================================================
// ④ owner 释放：只撤自己那批；`instrHookClear()` 全清
// =====================================================================================
void caseOwnerRelease() {
    lift_test::g_current_case = "instr-hook/owner-release";
    L::instrHookClear();
    L::instrHookSetInjectionSites(1u);
    bool a_ok = false;
    bool b_ok = false;
    const std::uint32_t a = addHook(0xAu, kPosA, &a_ok);
    const std::uint32_t b = addHook(0xBu, kPosB, &b_ok);
    LIFT_CHECK(a_ok);
    LIFT_CHECK(b_ok);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(L::instrHookList().size()), 2);

    L::instrHookReleaseOwner(0xAu);
    {
        const std::vector<L::InstrHookStatus> rows = L::instrHookList();
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rows.size()), 1);
        if (rows.size() == 1) {
            LIFT_CHECK_EQ_U32(rows[0].id, b);
            LIFT_CHECK(rows[0].id != a);
        }
    }
    LIFT_CHECK(L::instrHooksActive());  // b 还在 ⇒ 热路径闸门仍开

    // 幂等：再释放一次不会把 b 也带走。
    L::instrHookReleaseOwner(0xAu);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(L::instrHookList().size()), 1);

    L::instrHookClear();
    LIFT_CHECK(L::instrHookList().empty());
    LIFT_CHECK(!L::instrHooksActive());
}

// =====================================================================================
// ⑤ 持久写（`ctx.edit.global.set`）赢过瞬时覆盖：pending 被主动撤销
// =====================================================================================
void caseEditCancelsPending() {
    lift_test::g_current_case = "instr-hook/patch-edit-cancels-pending";
    L::instrHookClear();
    L::instrHookSetInjectionSites(1u);
    L::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    L::X86 cpu;
    cpu.mem = &mem;

    bool added = false;
    const std::uint32_t h = addHook(4u, kPosA, &added);
    LIFT_CHECK(added);
    const char* err = nullptr;
    // 目标 = `globalId 4`（与 `ctx.edit.global.set("int", 4, …)` 同一个 id 空间）。
    LIFT_CHECK(L::instrHookSetPatch(h, true, L::InstrPatchKind::GlobalInt, kGlobalIndex, 7u, 0u, &err));
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK_EQ_U32(mem.rd32(targetGlobalAddr()), enc(7u));

    // 窗口内走**持久写**（`ctx.edit` 的唯一实现）写同一地址。
    L::plugin::GuestEditSink sink(mem);
    L::GlobalValue value;
    value.kind = L::GlobalKind::Int;
    value.u32 = 9u;
    const L::plugin::Receipt receipt = sink.globalSet(L::GlobalKind::Int, kGlobalIndex, value);
    LIFT_CHECK(receipt.accepted);
    LIFT_CHECK_EQ_U32(mem.rd32(targetGlobalAddr()), enc(9u));

    // 下一条脚本指令派发：恢复**不该**发生（持久写赢），也不该记 restore / conflict。
    dispatchAt(mem, cpu, kPosB);
    LIFT_CHECK_EQ_U32(mem.rd32(targetGlobalAddr()), enc(9u));
    L::InstrHookStatus st;
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.restores), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.restore_conflicts), 0);
    L::instrHookReleaseOwner(4u);
    LIFT_CHECK(L::instrHookList().empty());
}

}  // namespace

int main() {
    // 缺省 = 视作可用（纯 runtime 场景）；① 之后把它摆回原状，避免用例间串味。
    lift::instrHookSetInjectionSites(1u);
    casePatchRestoreCas();
    casePatchRestoreClean();
    caseNoInjection();
    caseOwnerRelease();
    caseEditCancelsPending();
    return lift_test::finish("instr-hook");
}
