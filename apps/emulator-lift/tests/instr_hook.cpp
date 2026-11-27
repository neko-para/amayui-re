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
//   ⑥ `instr-hook/sink-hit-fields`（S2a）：命中后 `InstrHookSink::on_hits` 收到的
//      handle / script / pos / opcode / hits / local counts 逐项正确；pos 过滤仍有效；
//      注销 sink 之后不再分发、但 `stats().hits` 照旧（计数不依赖 sink）。
//      红了 = 指令点 → ui 的反向通道断了或字段漂（`handle.onTick` 永远不跑 / 参数错）。
//   ⑦ `instr-hook/sink-release-events`（S2a）：remove / releaseOwner / clear 摘条目时
//      `on_released` 各发**恰好一次**、按安装顺序、互不重复；不存在的句柄不发。
//      红了 = ui 层的 JS handler 泄漏（条目没了、JS 值还在）。
//   ⑧ `instr-hook/sink-hit-batch`（S2c）：同一点 3 条订阅 ⇒ 一次派发 `on_hits` **恰好一次**、
//      `count == 3`、顺序 = handle 升序、脚本名 = **本次派发名**；另一个 pos 不命中。
//      红了 = 分发退回逐条（链拿不到"这批就是全部"）或批量只发第一条。
//   ⑨ `instr-hook/add-ex-shared`（S2c）：`duplicate` **只对 V1At**；Sub / Once 同点可重复；
//      `{Range}` + `{Pos}` 同拍各命中一次；`range_empty` / `bad_where` 两条形状校验。
//      红了 = v2 同点多订阅被 v1 去重拒掉（`at-multi-same-pos` 会连带红）或形状校验缺失。
//   ⑩ `instr-hook/v1-patch-does-not-starve-sub`（S2c-1 复审 F1）：同点 V1At+patch 写成功之后，
//      后面的 v2 `Sub` 仍必须 `hits += 1` 并进 sink 的批量；写仍只有一次（先注册者赢）。
//      红了 = probe 又退回"写成功就 break"⇒ 同点后面的 `ctx.at` 静默不跑（S2 复发）。
//   ⑪ `instr-hook/add-ex-opcode`（S2c-1 复审 F6）：`{opcode}` 真按 opcode 匹配；撤掉 opcode 条目后
//      `has_opcode_entry` 归 false ⇒ 普通 probe **不读 pc**（用 `g_access` 判）。
//      红了 = opcode 匹配是假面 / 闸门恒 true（每拍多付一次 `rd32(pc)`）。
//   ⑫ `instr-hook/add-ex-range`（S2c-2）：`[lo,hi)` 四个边界（lo-1 / lo / hi-1 / hi）+ 脚本过滤。
//      红了 = Range 变闭区间 / 只判 lo / 不判脚本。
//   ⑬ `instr-hook/add-ex-script`（S2c-2）：`{script}` 命中整本脚本的任意指令、换脚本 ⇒ 0。
//      红了 = 脚本过滤被忽略（别的脚本也命中）。
//   ⑭ `instr-hook/add-ex-opcode-gate`（S2c-2）：`has_opcode_entry` 闸门的正/负控制（`g_access`）+
//      `{script,opcode}` 的脚本过滤 + 未提交 pc **不**命中。红了 = 闸门恒 true/false 或 opcode 编 0。
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
// ★ S2c-2：第二个脚本身份（名字表 id 4）—— `{Script}` / `{Script,Opcode}` 的负控制。
constexpr std::uint32_t kOtherScriptId = 4u;
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
    if (!putName(mem, kNameBase, kOtherScriptId, "OTHER.BIN")) return false;
    // int 池 + ENC key（`ctx.edit` 与 `GlobalInt` 覆盖共用这一份几何）。
    if (!put32(mem, kEngine + L::kScriptGlobalIntBaseOffset, kIntPool)) return false;
    if (!put32(mem, kEngine + L::kScriptGlobalIntCountOffset, kIntCount)) return false;
    if (!put32(mem, kEngine + L::kScriptGlobalIntKeyOffset, kKey)) return false;
    // 被覆盖的 dword 的初值：语义 3（⛔ 不是 0 —— 0 与"没写"分不开，CAS 判据会变弱）。
    return put32(mem, kTarget, enc(3u));
}

// 一次脚本派发（= AOT 在 3 个派发点注入的那次调用）：先把 PC 摆到 `pos`，再探测。
// `script_id`：帧 +0x3C 的 file id（缺省 = kScriptId；负控制换脚本身份用）。
void dispatchAt(L::GuestMemory& mem, L::X86& cpu, std::uint32_t pos,
                std::uint32_t script_id = kScriptId) {
    LIFT_CHECK(put32(mem, kFrameEa + kFramePcOff, kBody + (pos - L::kScriptHeaderLen)));
    LIFT_CHECK(put32(mem, kFrameEa + kFrameIdentityOff, script_id));
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

// S2c：任意形状的订阅（Pos: a=pos / Range: a=lo,b=hi / Opcode: a=opcode / Script: script only）。
std::uint32_t addExHook(std::uint64_t owner, L::InstrHookForm form, const std::string& script,
                        std::uint32_t a, std::uint32_t b, L::InstrHookKind kind, bool* ok) {
    L::InstrHookWhere where;
    where.form = form;
    where.script = script;
    if (form == L::InstrHookForm::Pos) where.pos = a;
    if (form == L::InstrHookForm::Range) {
        where.lo = a;
        where.hi = b;
    }
    if (form == L::InstrHookForm::Opcode) where.opcode = a;
    const char* err = nullptr;
    const std::uint32_t h = L::instrHookAddEx(owner, where, kind, &err);
    if (ok != nullptr) *ok = (h != 0 && err == nullptr);
    if (h == 0) std::fprintf(stderr, "  instrHookAddEx err=%s\n", err != nullptr ? err : "(null)");
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

// =====================================================================================
// ⑥ S2a · sink 的命中字段（`instr-hook/sink-hit-fields`）
//
//   红 = 指令点命中后 ui 层收不到"现场"（或收到错的值）⇒ `handle.onTick` 永远不跑 / 参数漂。
//   判据：一次 dispatch ⇒ 恰好 1 条 hit，且 handle / script / pos / opcode / hits / counts 逐项相等；
//   再 dispatch ⇒ hits 递增；**另一个 pos** ⇒ 0 条新 hit（pos 过滤没被破坏）；
//   注销 sink 之后 ⇒ 0 条新 hit，但 `stats().hits` 照旧 +1（**计数不依赖 sink**）。
//   ★ opcode 必须摆在 `pc = kBody + (pos - 0x3C)` 上（`sampleFrame` 的 pc 公式）。
// =====================================================================================
constexpr std::uint32_t kOpcode = 114u;
constexpr std::uint32_t kPcA = kBody + (kPosA - L::kScriptHeaderLen);

struct RecordingSink {
    std::vector<L::InstrHit> hits;
    std::vector<std::uint32_t> released;
    std::vector<std::string> scripts;
    int batches = 0;         // `on_hits` 被调了几次（一次派发必须一次）
    std::size_t last_count = 0;  // 最近一次的 count
};

void recordingOnHits(void* user, const L::InstrHit* hits, std::size_t count) {
    auto* sink = static_cast<RecordingSink*>(user);
    sink->batches += 1;
    sink->last_count = count;
    for (std::size_t i = 0; i < count; ++i) {
        sink->hits.push_back(hits[i]);
        sink->scripts.push_back(hits[i].script != nullptr ? std::string(hits[i].script)
                                                         : std::string("<null>"));
    }
}

void recordingOnReleased(void* user, std::uint32_t handle) {
    static_cast<RecordingSink*>(user)->released.push_back(handle);
}

void caseSinkHitFields() {
    lift_test::g_current_case = "instr-hook/sink-hit-fields";
    L::instrHookClear();
    L::instrHookSetInjectionSites(1u);
    RecordingSink sink;
    L::InstrHookSink hooks;
    hooks.user = &sink;
    hooks.on_hits = &recordingOnHits;
    hooks.on_released = &recordingOnReleased;
    L::instrHookAddSink(hooks);
    LIFT_CHECK(L::instrHookHasSink());

    L::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    L::X86 cpu;
    cpu.mem = &mem;
    LIFT_CHECK(put32(mem, kPcA, kOpcode));

    bool added = false;
    const std::uint32_t h = addHook(5u, kPosA, &added);
    LIFT_CHECK(added);

    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.hits.size()), 1);
    if (sink.hits.size() == 1) {
        const L::InstrHit& hit = sink.hits[0];
        LIFT_CHECK_EQ_U32(hit.handle, h);
        LIFT_CHECK_EQ_U32(hit.script_id, kScriptId);
        LIFT_CHECK(hit.script != nullptr && std::strcmp(hit.script, kScript) == 0);
        LIFT_CHECK_EQ_U32(hit.pos, kPosA);
        LIFT_CHECK(hit.opcode_ok);
        LIFT_CHECK_EQ_U32(hit.opcode, kOpcode);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(hit.hits), 1);
        LIFT_CHECK_EQ_U32(hit.int_count, 8u);
        LIFT_CHECK_EQ_U32(hit.ptr_count, 2u);
        LIFT_CHECK_EQ_U32(hit.int_base, kIntPool);
        LIFT_CHECK_EQ_U32(hit.ptr_base, kPtrSlot);
        LIFT_CHECK_EQ_U32(hit.codec_key, kKey);
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.scripts.size()), 1);

    // 第二次命中 ⇒ 累计命中随 `stats()` 一起涨。
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.hits.size()), 2);
    if (sink.hits.size() == 2) LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.hits[1].hits), 2);
    // 另一个位置 ⇒ 不命中（pos 过滤没坏）。
    dispatchAt(mem, cpu, kPosB);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.hits.size()), 2);

    // 反向自检：sink 注销后不再分发，但 runtime 的计数照旧（计数不依赖 sink）。
    L::instrHookRemoveSink(&sink);
    LIFT_CHECK(!L::instrHookHasSink());
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.hits.size()), 2);
    L::InstrHookStatus st;
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.hits), 3);
    L::instrHookReleaseOwner(5u);
    LIFT_CHECK(L::instrHookList().empty());
    L::instrHookRemoveSink(&sink);  // 幂等（重复注销不炸）
}

// =====================================================================================
// ⑦ S2a · `on_released` 的三个调用点（`instr-hook/sink-release-events`）
//
//   红 = 摘条目时 ui 层收不到通知 ⇒ `Impl::hook_handlers` 里的 JSValue 泄漏 / 一直跑到下一个 Run。
//   判据：remove / releaseOwner / clear 各发**恰好一次**、互不重复；不存在的句柄不发。
//   ★ 顺序也判：releaseOwner 与 clear 都是**安装顺序**（entries 顺序）。
// =====================================================================================
void caseSinkReleaseEvents() {
    lift_test::g_current_case = "instr-hook/sink-release-events";
    L::instrHookClear();
    L::instrHookSetInjectionSites(1u);
    RecordingSink sink;
    L::InstrHookSink hooks;
    hooks.user = &sink;
    hooks.on_hits = &recordingOnHits;
    hooks.on_released = &recordingOnReleased;
    L::instrHookAddSink(hooks);

    bool a_ok = false;
    bool b_ok = false;
    const std::uint32_t a = addHook(0xAu, kPosA, &a_ok);
    const std::uint32_t b = addHook(0xBu, kPosB, &b_ok);
    LIFT_CHECK(a_ok);
    LIFT_CHECK(b_ok);
    LIFT_CHECK(sink.released.empty());

    L::instrHookReleaseOwner(0xAu);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.released.size()), 1);
    if (sink.released.size() == 1) LIFT_CHECK_EQ_U32(sink.released[0], a);
    // 不存在的句柄 ⇒ 不发（"摘不到"不是"释放了一个"）。
    LIFT_CHECK(!L::instrHookRemove(999u));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.released.size()), 1);

    LIFT_CHECK(L::instrHookRemove(b));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.released.size()), 2);
    if (sink.released.size() == 2) LIFT_CHECK_EQ_U32(sink.released[1], b);

    // clear：按安装顺序逐条发（两个不同 owner）。
    bool c_ok = false;
    bool d_ok = false;
    const std::uint32_t c = addHook(0xCu, kPosA, &c_ok);
    const std::uint32_t d = addHook(0xDu, kPosB, &d_ok);
    LIFT_CHECK(c_ok);
    LIFT_CHECK(d_ok);
    L::instrHookClear();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.released.size()), 4);
    if (sink.released.size() == 4) {
        LIFT_CHECK_EQ_U32(sink.released[2], c);
        LIFT_CHECK_EQ_U32(sink.released[3], d);
    }
    // 幂等：空表再 clear 不发。
    L::instrHookClear();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.released.size()), 4);
    LIFT_CHECK(L::instrHookList().empty());
    L::instrHookRemoveSink(&sink);
}

// =====================================================================================
// ⑧ S2c · `instr-hook/sink-hit-batch`：一次派发**一次** `on_hits`，批次 = 全部命中
//
//   红 = 分发退回逐条（链拿不到"这批就是全部"）/ 批量只发第一条 / 顺序不是注册序。
//   判据：同点 3 条（V1At + Sub + Once）⇒ `batches==1`、`count==3`、handle 升序、脚本名 = 派发名；
//   另一个 pos ⇒ 不再调用。
// =====================================================================================
void caseSinkHitBatch() {
    lift_test::g_current_case = "instr-hook/sink-hit-batch";
    L::instrHookClear();
    L::instrHookSetInjectionSites(1u);
    RecordingSink sink;
    L::InstrHookSink hooks;
    hooks.user = &sink;
    hooks.on_hits = &recordingOnHits;
    hooks.on_released = &recordingOnReleased;
    L::instrHookAddSink(hooks);

    L::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    L::X86 cpu;
    cpu.mem = &mem;
    LIFT_CHECK(put32(mem, kPcA, kOpcode));

    bool v1_ok = false;
    bool sub_ok = false;
    bool once_ok = false;
    const std::uint32_t v1 = addHook(0x21u, kPosA, &v1_ok);
    const std::uint32_t sub = addExHook(0x21u, L::InstrHookForm::Pos, kScript, kPosA, 0,
                                        L::InstrHookKind::Sub, &sub_ok);
    const std::uint32_t once = addExHook(0x21u, L::InstrHookForm::Pos, kScript, kPosA, 0,
                                         L::InstrHookKind::Once, &once_ok);
    LIFT_CHECK(v1_ok);
    LIFT_CHECK(sub_ok);
    LIFT_CHECK(once_ok);
    LIFT_CHECK(v1 < sub && sub < once);  // 注册序 = handle 升序（runtime 只 push_back）

    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.batches), 0);
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.batches), 1);  // ★ 一次派发**一次**
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.last_count), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.hits.size()), 3);
    if (sink.hits.size() == 3) {
        LIFT_CHECK_EQ_U32(sink.hits[0].handle, v1);
        LIFT_CHECK_EQ_U32(sink.hits[1].handle, sub);
        LIFT_CHECK_EQ_U32(sink.hits[2].handle, once);
        for (const L::InstrHit& hit : sink.hits) {
            // ★ `script` = **本次派发的脚本名**（不是"订阅注册名"；`{script,pos}` 下两者相等）。
            LIFT_CHECK(hit.script != nullptr && std::strcmp(hit.script, kScript) == 0);
            LIFT_CHECK_EQ_U32(hit.script_id, kScriptId);
            LIFT_CHECK_EQ_U32(hit.pos, kPosA);
            LIFT_CHECK(hit.opcode_ok);
            LIFT_CHECK_EQ_U32(hit.opcode, kOpcode);
            LIFT_CHECK(hit.codec_ok);
            LIFT_CHECK_EQ_U32(hit.codec_key, kKey);
        }
    }
    for (const std::uint32_t h : {v1, sub, once}) {
        L::InstrHookStatus st;
        LIFT_CHECK(L::instrHookStats(h, &st));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.hits), 1);
    }
    // 另一个位置 ⇒ 不再调用（pos 过滤没坏）。
    dispatchAt(mem, cpu, kPosB);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.batches), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.hits.size()), 3);
    L::instrHookClear();
    L::instrHookRemoveSink(&sink);
}

// =====================================================================================
// ⑨ S2c · `instr-hook/add-ex-shared`：去重只对 V1At；四种 form 的准入
//
//   红 = v2 同点多订阅被 v1 的 duplicate 拒掉（`at-multi-same-pos` 连带红）/ 形状校验缺失。
//   判据：V1At 同点第二条 ⇒ `duplicate`；Sub / Once 各两条都成功且 handle 互异；
//         `{Range,[0x100,0x108)}` + `{Pos,0x100}` 同拍各命中一次；`lo==hi` ⇒ `range_empty`；
//         `{Script}` + 空 script ⇒ `bad_where`。
// =====================================================================================
void caseAddExShared() {
    lift_test::g_current_case = "instr-hook/add-ex-shared";
    L::instrHookClear();
    L::instrHookSetInjectionSites(1u);
    L::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    L::X86 cpu;
    cpu.mem = &mem;
    LIFT_CHECK(put32(mem, kPcA, kOpcode));

    // ---- ① V1At 的 duplicate 逐字保留（v1 已发布语义）----
    bool a_ok = false;
    const std::uint32_t v1 = addHook(0x31u, kPosA, &a_ok);
    LIFT_CHECK(a_ok);
    L::InstrHookWhere pos_where;
    pos_where.form = L::InstrHookForm::Pos;
    pos_where.script = kScript;
    pos_where.pos = kPosA;
    const char* err = nullptr;
    LIFT_CHECK_EQ_U32(L::instrHookAddEx(0x31u, pos_where, L::InstrHookKind::V1At, &err), 0u);
    LIFT_CHECK(err != nullptr && std::strcmp(err, "duplicate") == 0);

    // ---- ② Sub / Once 同点可重复（各自 handle 互异；去重不看它们）----
    bool s1_ok = false;
    bool s2_ok = false;
    bool o1_ok = false;
    bool o2_ok = false;
    const std::uint32_t s1 = addExHook(0x31u, L::InstrHookForm::Pos, kScript, kPosA, 0,
                                       L::InstrHookKind::Sub, &s1_ok);
    const std::uint32_t s2 = addExHook(0x31u, L::InstrHookForm::Pos, kScript, kPosA, 0,
                                       L::InstrHookKind::Sub, &s2_ok);
    const std::uint32_t o1 = addExHook(0x31u, L::InstrHookForm::Pos, kScript, kPosA, 0,
                                       L::InstrHookKind::Once, &o1_ok);
    const std::uint32_t o2 = addExHook(0x31u, L::InstrHookForm::Pos, kScript, kPosA, 0,
                                       L::InstrHookKind::Once, &o2_ok);
    LIFT_CHECK(s1_ok && s2_ok && o1_ok && o2_ok);
    LIFT_CHECK(s1 != s2);
    LIFT_CHECK(o1 != o2);
    LIFT_CHECK(v1 < s1 && o2 > o1);

    // ---- ③ Range + Pos 在同一拍各命中一次；hi 是开区间 ----
    L::instrHookClear();
    bool r_ok = false;
    bool p_ok = false;
    const std::uint32_t range_h = addExHook(0x32u, L::InstrHookForm::Range, kScript, kPosA,
                                            kPosA + 8u, L::InstrHookKind::Sub, &r_ok);
    const std::uint32_t pos_h = addExHook(0x32u, L::InstrHookForm::Pos, kScript, kPosA, 0,
                                          L::InstrHookKind::Sub, &p_ok);
    LIFT_CHECK(r_ok);
    LIFT_CHECK(p_ok);
    dispatchAt(mem, cpu, kPosA);
    L::InstrHookStatus rs;
    L::InstrHookStatus ps;
    LIFT_CHECK(L::instrHookStats(range_h, &rs));
    LIFT_CHECK(L::instrHookStats(pos_h, &ps));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.hits), 1);  // lo 命中
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(ps.hits), 1);
    dispatchAt(mem, cpu, kPosA + 4u);
    LIFT_CHECK(L::instrHookStats(range_h, &rs));
    LIFT_CHECK(L::instrHookStats(pos_h, &ps));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.hits), 2);  // 区间内命中
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(ps.hits), 1);
    dispatchAt(mem, cpu, kPosA + 8u);  // = kPosB：hi 是开区间 ⇒ 不命中
    LIFT_CHECK(L::instrHookStats(range_h, &rs));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(rs.hits), 2);

    // ---- ④ 形状校验 ----
    err = nullptr;
    L::InstrHookWhere bad;
    bad.form = L::InstrHookForm::Range;
    bad.script = kScript;
    bad.lo = 0x200u;
    bad.hi = 0x200u;
    LIFT_CHECK_EQ_U32(L::instrHookAddEx(0x33u, bad, L::InstrHookKind::Sub, &err), 0u);
    LIFT_CHECK(err != nullptr && std::strcmp(err, "range_empty") == 0);
    err = nullptr;
    bad = L::InstrHookWhere{};
    bad.form = L::InstrHookForm::Script;  // script 空 = "所有脚本每一拍" ⇒ 不是可用形状
    LIFT_CHECK_EQ_U32(L::instrHookAddEx(0x33u, bad, L::InstrHookKind::Sub, &err), 0u);
    LIFT_CHECK(err != nullptr && std::strcmp(err, "bad_where") == 0);
    // 反向自检：`{Script}` 带非空脚本是**合法**的（runtime 面写全，JS 面 S2c-2 才放开）。
    bool script_ok = false;
    const std::uint32_t script_h = addExHook(0x33u, L::InstrHookForm::Script, kScript, 0, 0,
                                             L::InstrHookKind::Sub, &script_ok);
    LIFT_CHECK(script_ok);
    dispatchAt(mem, cpu, kPosA + 0x10u);  // 整本脚本：任意 pos 都命中
    L::InstrHookStatus ss;
    LIFT_CHECK(L::instrHookStats(script_h, &ss));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(ss.hits), 1);
    L::instrHookClear();
}

// =====================================================================================
// ⑩ S2c-1 复审 F1 · `instr-hook/v1-patch-does-not-starve-sub`
//
//   红 = v1 的 patch 写成功后 `break` ⇒ 同点**后面**的 v2 订阅既不 `hits += 1`、也进不了
//   `hit_ids`（`ctx.at` handler 静默不跑、`hook.list().hits` 恒 0）。
//   判据：同点 V1At+patch + Sub+patch 各一条 ⇒ 一次 probe 后两条 hits 都是 1、sink 恰好 2 条；
//   地址 = **先注册的 v1** 的 patch 值（"一条指令最多一次写、先注册者赢"没变）。
// =====================================================================================
void caseV1PatchDoesNotStarveSub() {
    lift_test::g_current_case = "instr-hook/v1-patch-does-not-starve-sub";
    L::instrHookClear();
    L::instrHookSetInjectionSites(1u);
    RecordingSink sink;
    L::InstrHookSink hooks;
    hooks.user = &sink;
    hooks.on_hits = &recordingOnHits;
    L::instrHookAddSink(hooks);

    L::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    L::X86 cpu;
    cpu.mem = &mem;
    LIFT_CHECK(put32(mem, kPcA, kOpcode));

    bool v1_ok = false;
    bool sub_ok = false;
    const std::uint32_t v1 = addHook(0x41u, kPosA, &v1_ok);
    LIFT_CHECK(v1_ok);
    const char* err = nullptr;
    LIFT_CHECK(L::instrHookSetPatch(v1, true, L::InstrPatchKind::PtrDword, 0u, 7u, 0u, &err));
    const std::uint32_t sub = addExHook(0x41u, L::InstrHookForm::Pos, kScript, kPosA, 0,
                                        L::InstrHookKind::Sub, &sub_ok);
    LIFT_CHECK(sub_ok);
    LIFT_CHECK(v1 < sub);
    // 第二条也装 patch（值不同）⇒ "sub 不写"有读数：地址必须还是 v1 写进去的 enc(7)。
    LIFT_CHECK(L::instrHookSetPatch(sub, true, L::InstrPatchKind::PtrDword, 0u, 9u, 0u, &err));

    dispatchAt(mem, cpu, kPosA);
    L::InstrHookStatus v1_st;
    L::InstrHookStatus sub_st;
    LIFT_CHECK(L::instrHookStats(v1, &v1_st));
    LIFT_CHECK(L::instrHookStats(sub, &sub_st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(v1_st.hits), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sub_st.hits), 1);  // ★ 修前 = 0（被 break 饿死）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.batches), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.last_count), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sink.hits.size()), 2);
    if (sink.hits.size() == 2) {
        LIFT_CHECK_EQ_U32(sink.hits[0].handle, v1);  // 注册序 = handle 升序
        LIFT_CHECK_EQ_U32(sink.hits[1].handle, sub);
    }
    LIFT_CHECK_EQ_U32(mem.rd32(kTarget), enc(7u));  // v1 的 7 生效、sub 的 9 没写
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(sub_st.skips), 0);  // 跳过写 ≠ 尝试失败
    // 窗口结束：下一拍恢复 v1 原本的 raw（sub 没留下任何东西）。
    dispatchAt(mem, cpu, kPosB);
    LIFT_CHECK_EQ_U32(mem.rd32(kTarget), enc(3u));
    L::instrHookClear();
    L::instrHookRemoveSink(&sink);
}

// =====================================================================================
// ⑪ S2c-1 复审 F6 · `instr-hook/add-ex-opcode`
//
//   红 = `{opcode}` 的匹配 / `has_opcode_entry` 闸门是"写了但永不生效"的假面：
//   · Opcode=X 的条目：pc 上是 X ⇒ 命中；换成 Y ⇒ 不命中（判据是**按 opcode 匹配**，不是"装上了"）；
//   · 没有 `{opcode}` 订阅时 probe **不读 pc**：用 `lift::g_access`（最近一次 guest 访存的 EA）判。
//     ② 里同一仪器读出"读了 pc"，③ 撤掉 opcode 条目之后读出"没读" ⇒ 这不是恒真断言：
//     闸门恒 true / `recomputeGates` 不维护，都会当场红。
// =====================================================================================
void caseAddExOpcode() {
    lift_test::g_current_case = "instr-hook/add-ex-opcode";
    L::instrHookClear();
    L::instrHookSetInjectionSites(1u);
    L::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    L::X86 cpu;
    cpu.mem = &mem;
    constexpr std::uint32_t kOpcodeX = 0x72u;
    constexpr std::uint32_t kOpcodeY = 0x73u;
    LIFT_CHECK(put32(mem, kPcA, kOpcodeX));

    // ① 没有 opcode 订阅：普通 Pos 订阅先跑两拍（第二拍 script_id 已缓存 ⇒ 之后没有名字解析的读）。
    bool pos_ok = false;
    const std::uint32_t pos_h = addExHook(0x51u, L::InstrHookForm::Pos, kScript, kPosA, 0,
                                          L::InstrHookKind::Sub, &pos_ok);
    LIFT_CHECK(pos_ok);
    dispatchAt(mem, cpu, kPosA);
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK(L::g_access.ea != kPcA);  // ★ 无 `{opcode}` 订阅 ⇒ probe 不读 pc

    // ② `{opcode, X}`：pc 上是 X ⇒ 命中（同时证明闸门在"有 opcode 订阅"时真的读 pc）。
    bool opx_ok = false;
    const std::uint32_t op_x = addExHook(0x51u, L::InstrHookForm::Opcode, std::string(), kOpcodeX, 0,
                                         L::InstrHookKind::Sub, &opx_ok);
    LIFT_CHECK(opx_ok);
    dispatchAt(mem, cpu, kPosA);
    L::InstrHookStatus ox;
    L::InstrHookStatus ps;
    LIFT_CHECK(L::instrHookStats(op_x, &ox));
    LIFT_CHECK(L::instrHookStats(pos_h, &ps));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(ox.hits), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(ps.hits), 3);
    LIFT_CHECK(ox.form == L::InstrHookForm::Opcode);
    LIFT_CHECK_EQ_U32(ox.opcode, kOpcodeX);
    LIFT_CHECK_EQ_U32(L::g_access.ea, kPcA);  // ★ 正控制：同一仪器确实能看见 pc 读

    // ③ pc 换成 Y ⇒ 不命中（按 opcode 匹配，不是"装了就算"）。
    LIFT_CHECK(put32(mem, kPcA, kOpcodeY));
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK(L::instrHookStats(op_x, &ox));
    LIFT_CHECK(L::instrHookStats(pos_h, &ps));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(ox.hits), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(ps.hits), 4);

    // ④ 撤掉 opcode 条目 ⇒ `has_opcode_entry` 归 false（`recomputeGates` 真维护）⇒ 不再读 pc。
    LIFT_CHECK(L::instrHookRemove(op_x));
    LIFT_CHECK(!L::instrHookStats(op_x, &ox));
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK(L::g_access.ea != kPcA);
    LIFT_CHECK(L::instrHookStats(pos_h, &ps));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(ps.hits), 5);
    L::instrHookClear();
}

// =====================================================================================
// ⑫ S2c-2 · `instr-hook/add-ex-range`：`[lo,hi)` 的四个边界 + 脚本过滤
//
//   红 = Range 的 matcher 写成闭区间（hi 命中）/ 只比 lo / 不判脚本：
//   判据：lo-1 不中、lo 中、hi-1 中、hi 不中、区间内再中；换脚本 ⇒ 不中。
// =====================================================================================
void caseAddExRange() {
    lift_test::g_current_case = "instr-hook/add-ex-range";
    L::instrHookClear();
    L::instrHookSetInjectionSites(1u);
    L::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    L::X86 cpu;
    cpu.mem = &mem;

    bool ok = false;
    const std::uint32_t h = addExHook(0x61u, L::InstrHookForm::Range, kScript, kPosA, kPosA + 8u,
                                      L::InstrHookKind::Sub, &ok);
    LIFT_CHECK(ok);
    L::InstrHookStatus st;
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK(st.form == L::InstrHookForm::Range);
    LIFT_CHECK_EQ_U32(st.lo, kPosA);
    LIFT_CHECK_EQ_U32(st.hi, kPosA + 8u);
    LIFT_CHECK_EQ_U32(st.pos, 0u);  // form != Pos ⇒ pos 保持 0

    dispatchAt(mem, cpu, kPosA - 1u);  // lo-1：不命中
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.hits), 0);
    dispatchAt(mem, cpu, kPosA);  // lo：命中
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.hits), 1);
    dispatchAt(mem, cpu, kPosA + 7u);  // hi-1：命中
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.hits), 2);
    dispatchAt(mem, cpu, kPosA + 8u);  // hi：**开区间** ⇒ 不命中
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.hits), 2);
    dispatchAt(mem, cpu, kPosA + 4u);  // 区间内：命中
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.hits), 3);
    // 换脚本（同一个 pos）⇒ 不命中（脚本过滤是 Range 的一部分）。
    dispatchAt(mem, cpu, kPosA + 4u, kOtherScriptId);
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.hits), 3);
    L::instrHookClear();
}

// =====================================================================================
// ⑬ S2c-2 · `instr-hook/add-ex-script`：`{Script}` = 整本脚本的每条指令
//
//   红 = `{Script}` 被当成"任意脚本都命中"（换脚本仍中）或只能命中一个 pos：
//   判据：同一脚本两条不同 pos 都中；换脚本 ⇒ 不中；回到原脚本 ⇒ 又中。
// =====================================================================================
void caseAddExScript() {
    lift_test::g_current_case = "instr-hook/add-ex-script";
    L::instrHookClear();
    L::instrHookSetInjectionSites(1u);
    L::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    L::X86 cpu;
    cpu.mem = &mem;

    bool ok = false;
    const std::uint32_t h = addExHook(0x62u, L::InstrHookForm::Script, kScript, 0, 0,
                                      L::InstrHookKind::Sub, &ok);
    LIFT_CHECK(ok);
    L::InstrHookStatus st;
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK(st.form == L::InstrHookForm::Script);
    LIFT_CHECK(st.script == kScript);
    LIFT_CHECK_EQ_U32(st.pos, 0u);
    LIFT_CHECK_EQ_U32(st.lo, 0u);
    LIFT_CHECK_EQ_U32(st.hi, 0u);
    LIFT_CHECK_EQ_U32(st.opcode, 0u);

    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.hits), 1);
    dispatchAt(mem, cpu, kPosA + 0x10u);  // 另一条指令（同一个脚本）
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.hits), 2);
    dispatchAt(mem, cpu, kPosA, kOtherScriptId);  // 换脚本 ⇒ 0 命中
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.hits), 2);
    dispatchAt(mem, cpu, kPosA + 0x20u);  // 回到原脚本 ⇒ 又命中（反面自检）
    LIFT_CHECK(L::instrHookStats(h, &st));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(st.hits), 3);
    L::instrHookClear();
}

// =====================================================================================
// ⑭ S2c-2 · `instr-hook/add-ex-opcode-gate`：`{opcode}` 的匹配 + `has_opcode_entry` 闸门
//
//   红 = ① 没有 `{opcode}` 订阅时仍每拍读 `rd32(pc)`（闸门恒 true）；② 有订阅时**不**读
//        （闸门恒 false / `recomputeGates` 不维护）；③ 不判 opcode / 不判脚本；
//        ④ 读不到 opcode 时编 0（未提交 pc 被当成 opcode 0 命中）。
//   ★ 仪器 = `lift::g_access`（最近一次 guest 访存的 EA）；②的正控制证明"没读"不是仪器坏了。
// =====================================================================================
void caseAddExOpcodeGate() {
    lift_test::g_current_case = "instr-hook/add-ex-opcode-gate";
    L::instrHookClear();
    L::instrHookSetInjectionSites(1u);
    L::GuestMemory mem;
    LIFT_CHECK(makeFakeEngine(mem));
    L::X86 cpu;
    cpu.mem = &mem;
    constexpr std::uint32_t kOpX = 0x72u;
    constexpr std::uint32_t kOpY = 0x73u;
    LIFT_CHECK(put32(mem, kPcA, kOpX));

    // 一条普通 Pos 订阅（无 `{opcode}`）：跑两拍把 `script_id` 缓存下来 ⇒ 之后没有名字解析的读。
    bool pos_ok = false;
    const std::uint32_t pos_h = addExHook(0x63u, L::InstrHookForm::Pos, kScript, kPosA, 0,
                                          L::InstrHookKind::Sub, &pos_ok);
    LIFT_CHECK(pos_ok);
    dispatchAt(mem, cpu, kPosA);
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK(L::g_access.ea != kPcA);  // ① 没有 {opcode} 订阅 ⇒ 不读 pc

    // ② `{script,opcode,X}`：pc=X ⇒ 命中；同一个仪器读出"读了 pc"（正控制）。
    //   ★ 跑两拍：第一拍 `matchEntry` 要解析脚本名（`frameName`，最后访问的是名字表），
    //     第二拍 `Entry::script_id` 已缓存 ⇒ 本拍最后一次访存就是闸门读的 `rd32(pc)`。
    bool op_ok = false;
    const std::uint32_t op_h = addExHook(0x63u, L::InstrHookForm::Opcode, kScript, kOpX, 0,
                                         L::InstrHookKind::Sub, &op_ok);
    LIFT_CHECK(op_ok);
    dispatchAt(mem, cpu, kPosA);
    dispatchAt(mem, cpu, kPosA);
    L::InstrHookStatus os;
    LIFT_CHECK(L::instrHookStats(op_h, &os));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(os.hits), 2);
    LIFT_CHECK(os.form == L::InstrHookForm::Opcode);
    LIFT_CHECK(os.script == kScript);
    LIFT_CHECK_EQ_U32(os.opcode, kOpX);
    LIFT_CHECK_EQ_U32(L::g_access.ea, kPcA);  // ★ 闸门为真时确实读 pc

    // ③ pc=Y ⇒ 不命中（按 opcode 匹配）。
    LIFT_CHECK(put32(mem, kPcA, kOpY));
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK(L::instrHookStats(op_h, &os));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(os.hits), 2);

    // ④ `{script,opcode}` 的 script 过滤：同一个 opcode、换脚本 ⇒ 不命中。
    LIFT_CHECK(put32(mem, kPcA, kOpX));
    dispatchAt(mem, cpu, kPosA, kOtherScriptId);
    LIFT_CHECK(L::instrHookStats(op_h, &os));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(os.hits), 2);

    // ⑤ 未提交 pc（`rd32` 读不到）⇒ **不命中**（⛔ 不编 0：0 是合法 opcode）。
    constexpr std::uint32_t kUncommittedPos = L::kScriptHeaderLen + 0xC0000u;
    dispatchAt(mem, cpu, kUncommittedPos);
    LIFT_CHECK(L::instrHookStats(op_h, &os));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(os.hits), 2);

    // ⑥ 撤掉 `{opcode}` 条目 ⇒ `has_opcode_entry` 归 false（`recomputeGates` 真维护）⇒ 不再读 pc。
    LIFT_CHECK(L::instrHookRemove(op_h));
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK(L::g_access.ea != kPcA);
    L::InstrHookStatus ps;
    LIFT_CHECK(L::instrHookStats(pos_h, &ps));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(ps.hits), 6);  // ①2 + ②2 + ③1 + ⑥1（④⑤不中）

    // ⑦ `releaseOwner` 也要重算：装一条全局 `{opcode}`（空 script ⇒ 不解析名字，正控制干净）⇒ 读 pc；
    //   释放 owner ⇒ 归 false ⇒ 不再读。
    bool rel_ok = false;
    addExHook(0x63u, L::InstrHookForm::Opcode, std::string(), kOpX, 0, L::InstrHookKind::Sub, &rel_ok);
    LIFT_CHECK(rel_ok);
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK_EQ_U32(L::g_access.ea, kPcA);
    L::instrHookReleaseOwner(0x63u);
    LIFT_CHECK(L::instrHookList().empty());  // pos_h 也是这个 owner
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK(L::g_access.ea != kPcA);

    // ⑧ `clear()` 也要重算：再装一条 `{opcode}` ⇒ 读 pc；`instrHookClear()` ⇒ 不再读。
    bool clr_ok = false;
    addExHook(0x64u, L::InstrHookForm::Opcode, std::string(), kOpX, 0, L::InstrHookKind::Sub, &clr_ok);
    LIFT_CHECK(clr_ok);
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK_EQ_U32(L::g_access.ea, kPcA);
    L::instrHookClear();
    dispatchAt(mem, cpu, kPosA);
    LIFT_CHECK(L::g_access.ea != kPcA);
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
    // ---- S2a（反向 sink：指令点命中 → ui 层；摘条目 → on_released）----
    caseSinkHitFields();
    caseSinkReleaseEvents();
    // ---- S2c（批量 sink；addEx 的 kind / form 准入）----
    caseSinkHitBatch();
    caseAddExShared();
    // ---- S2c-1 复审收口（F1 的 break 饿死 / F6 的 opcode 路径）----
    caseV1PatchDoesNotStarveSub();
    caseAddExOpcode();
    // ---- S2c-2（where 过滤的 C++ 面：Range / Script / opcode 闸门）----
    caseAddExRange();
    caseAddExScript();
    caseAddExOpcodeGate();
    return lift_test::finish("instr-hook");
}
