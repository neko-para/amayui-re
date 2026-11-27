// emulator-lift · **脚本指令位置钩子**（插件面能力；正式实现）
//
// 三种能力是**彼此独立**的（用户裁决 2026-11-26 晚：触发 ≠ 修改）：
//   ① **触发**：`ctx.hook.at(script, pos)` 装一条"到点"的观察 —— 只计数，⛔ 不做任何写；
//   ② **读**：`handle.capture({ints, ptrs})` 采样该点的 local（最近一次命中，`handle.stats()` 读）；
//   ③ **改**：`handle.patch(target, value, {maxHits})` 在**这条指令读之前**覆盖一个 dword，
//      下一条脚本指令派发前逐字节恢复 ⇒ **净变化 0**（⛔ 不是持久写；持久写是 `ctx.edit`）。
//   ⇒ 插件可以只触发不读、只读不改、或装好触发后再决定改什么（`patch` 可反复覆盖/撤回）。
//
// 入口（谁在什么时刻调本模块）：VM 的脚本派发是 3 个 `call eax/edx`（`aot/aot_hook_ea.h` 的
//   `kScriptDispatchEas`），生成器在这 3 个站点注入 `call void @lift_instr_hook(ptr %cpu)`
//   ⇒ **每条脚本指令执行前**宿主都能看一眼"现在要执行的是哪个脚本的哪个位置"。
//   ⛔ 不要在 `lift_dyn` 里用 `[esp]` 回溯返回地址判定：那会让每次间接跳转（≈54.8k/帧）都付费，
//   而且"我是谁"靠猜。
//
// 三条纪律：
//   · **缺省零成本**：一条 hook 都没装时 `lift_instr_hook` 只读一个 latched atomic<bool>；
//   · **判据只有 (脚本身份, pos)**：⛔ 没有"按 EA 特判"；
//   · **恢复是义务**：覆盖前保存 raw（含 ENC 编码位），下一次任一派发先恢复；`clear()` / Run 收口也恢复。
//
// 位置坐标：`pos = 0x3C + (pc − 脚本体基址)` —— 与 `dist/views/data/*.txt` 的 `label_%08x` 同坐标。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace lift {

struct X86;

/** 覆盖目标（`value` 一律是**语义值**；写回时按引擎 key 做 ENC）。 */
enum class InstrPatchKind : std::uint8_t {
    PtrDword = 0,   // `index` = 指针 local 下标（如 `local-ptr 2`）⇒ 改它**指向**的 dword
    LocalInt = 1,   // `index` = int local 下标（`local-int N`）
    GlobalInt = 2,  // `index` = 全局池 id（与 `ctx.edit.global.set` 同一个 id 空间）
};

const char* instrPatchKindName(InstrPatchKind kind);

struct InstrHookStatus {
    std::uint32_t id = 0;                 // 句柄（`ctx.hook.at` 返回的那个）
    std::string script;
    std::uint32_t pos = 0;
    bool patch = false;                   // 这条 hook 现在**是否**装了覆盖
    InstrPatchKind kind = InstrPatchKind::PtrDword;
    std::uint32_t index = 0;
    std::uint32_t value = 0;
    std::uint32_t max_hits = 0;           // 0 = 不限
    std::uint64_t hits = 0;
    std::uint64_t restores = 0;           // 已经恢复过多少次（每次覆盖之后 +1）
    // ★ S0（缺陷 2）：恢复时**当前值已经不是我们写进去的那个** ⇒ 不写回、只记这一笔。
    //   语义：窗口里有人写过同一地址（引擎自己的 guest 写 / 另一条执行流）⇒ **别人的写赢**，
    //   ⛔ 不把过期的 raw 盖回去。★ 宿主侧的**持久写**（`ctx.edit` / 控制层 `global-set`）不走这一笔：
    //   它在写的那一刻就把 pending 撤销了（见 `instrHookCancelPendingAt`）—— 那是"持久写赢"，
    //   不是"冲突"。
    std::uint64_t restore_conflicts = 0;
    std::uint64_t skips = 0;              // 到点了但没能写（目标不可读 / 没 key / 超过 maxHits）
    std::uint32_t last_id = 0;            // 最近命中的脚本文件 id
    std::vector<std::uint32_t> last_ints;       // `capture({ints})` 的语义值（最近一次命中）
    std::vector<std::uint32_t> last_ptrs;       // `capture({ptrs})` 的指针值
    std::vector<std::uint32_t> last_ptr_targets;// 上面每个指针指向的 dword（语义值；不可读 = 0）
};

/**
 * ① 触发：装一条"到点"的 hook（**纯观察**：不写、不读 guest 内存）。
 * `owner` = 这条 hook 的**归属**（`PluginHost` 传本 Run 的序号；`0` = 无主，只有纯 runtime / 测试用）。
 *   ★ 为什么需要 owner（S0 缺陷 1）：hook 表是**进程级**的，Run 结束必须能把"本 Run 装的"一次撤干净；
 *     没有 owner 就只能 `instrHookClear()`（会把别人的也撤掉），而 v1 的收口路径大多**连 clear 都没调**
 *     ⇒ Run 失败 / done 之后 guest 内存仍被按条改写。
 * 成功 ⇒ 返回句柄 id（≥1）；失败 ⇒ 0 + `*err` 静态字面量
 *   （`script_empty` / `pos_zero` / `duplicate` / `no_injection`）。
 */
std::uint32_t instrHookAdd(std::uint64_t owner, const std::string& script, std::uint32_t pos, const char** err);

/**
 * ③ 改：给句柄装 / 换 / 撤覆盖。`value` = 语义值；`maxHits` = 0 表示不限（只在前 N 次命中写）。
 * 传 `enabled=false` ⇒ 撤掉覆盖（hook 仍在，继续计数）。失败 ⇒ false + `*err`（`no_such_hook` / `bad_target`）。
 */
bool instrHookSetPatch(std::uint32_t handle, bool enabled, InstrPatchKind kind, std::uint32_t index,
                       std::uint32_t value, std::uint32_t max_hits, const char** err);

/** ② 读：给句柄设采样清单（最多 8 个 int / 8 个 ptr；空表 = 不采样）。 */
bool instrHookSetCapture(std::uint32_t handle, const std::vector<std::uint32_t>& ints,
                         const std::vector<std::uint32_t>& ptrs);

/** 读一条句柄的读数（`hits` / `restores` / `restore_conflicts` / `skips` / 最近一次采样）；没有该句柄 ⇒ false。 */
bool instrHookStats(std::uint32_t handle, InstrHookStatus* out);

/** 撤一条 hook（并把未还原的那次覆盖恢复）。没有该句柄 ⇒ false。 */
bool instrHookRemove(std::uint32_t handle);

/**
 * 释放**某个 owner** 的全部 hook，并把属于它的未还原覆盖结算掉（CAS：见 `restorePendingLocked`）。
 * ★ 这是 Run 收口的唯一入口（`PluginHost::releaseRunHooks`）：`fail` / `finish` / `quit` /
 *   预算超限 / 回调返回 `[]`（done）/ install 拒绝 / 卸载 / 进程退出都走它。
 */
void instrHookReleaseOwner(std::uint64_t owner);

/** 撤掉所有 hook（= 释放**全部** owner；并把未还原的写恢复）。进程级收口 / 纯测试夹具用。 */
void instrHookClear();

/** 当前所有 hook 的读数（安装顺序）。 */
std::vector<InstrHookStatus> instrHookList();

/**
 * ★ S0（缺陷 3）：告诉 runtime"这一趟构建到底在几个脚本派发点注入了 `lift_instr_hook`"。
 * 由 **AOT 的初始化路径**调一次（`aot_dispatch.cpp` 的 `ensureDispatchTable()` 把
 * `lift_pin.h` 的 `LIFT_AOT_PIN_INSTR_HOOK_SITES` 传进来）。`n == 0` ⇒ 注入面不存在
 * （`--no-s7` / 闭包没覆盖脚本派发 EA）⇒ `instrHookAdd` 返回 `no_injection` 而**不是**一个
 * "装上了但永远 hits=0"的假句柄。
 * ★ **没人调** = 视作可用（纯 runtime / 测试夹具没有 AOT 生成形状可言；⛔ 不要让测试被迫先"打开"）。
 */
void instrHookSetInjectionSites(std::uint32_t n);

/**
 * ★ S0（缺陷 2 的另一半）：宿主侧的**持久写**落在当前 pending 覆盖的同一地址 ⇒ 主动撤销那次待恢复。
 * 语义 = "持久写赢过瞬时覆盖"（`ctx.edit.global.set` / 控制层 `global-set` 都是这一条）。
 * 由 `writeGlobal` 的唯一写点调（`runtime/script_globals.cpp`）⇒ 不要求调用方知道地址。
 * 为什么还要它（CAS 已经不会把别人的写盖回去了）：撤销让 pending **不再**挂着一个过时的 `raw`，
 * 否则下一次恢复会白读一次、而且 `hits/restores` 的账会显示"恢复过"（那是假读数）。
 */
void instrHookCancelPendingAt(std::uint32_t addr);

/** 有没有装 hook（热路径闸门；`lift_instr_hook` 只读这一位）。 */
bool instrHooksActive();

/**
 * 派发点探针（**唯一调用方** = AOT 注入的 `lift_instr_hook`）。
 * 做的事：恢复上一次未还原的写 → 找命中的 hook → 采样 / 执行覆盖并登记待恢复。
 */
void instrHookProbe(X86& cpu);

}  // namespace lift
