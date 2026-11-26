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
    std::uint64_t skips = 0;              // 到点了但没能写（目标不可读 / 没 key / 超过 maxHits）
    std::uint32_t last_id = 0;            // 最近命中的脚本文件 id
    std::vector<std::uint32_t> last_ints;       // `capture({ints})` 的语义值（最近一次命中）
    std::vector<std::uint32_t> last_ptrs;       // `capture({ptrs})` 的指针值
    std::vector<std::uint32_t> last_ptr_targets;// 上面每个指针指向的 dword（语义值；不可读 = 0）
};

/**
 * ① 触发：装一条"到点"的 hook（**纯观察**：不写、不读 guest 内存）。
 * 成功 ⇒ 返回句柄 id（≥1）；失败 ⇒ 0 + `*err` 静态字面量（`script_empty` / `pos_zero` / `duplicate`）。
 */
std::uint32_t instrHookAdd(const std::string& script, std::uint32_t pos, const char** err);

/**
 * ③ 改：给句柄装 / 换 / 撤覆盖。`value` = 语义值；`maxHits` = 0 表示不限（只在前 N 次命中写）。
 * 传 `enabled=false` ⇒ 撤掉覆盖（hook 仍在，继续计数）。失败 ⇒ false + `*err`（`no_such_hook` / `bad_target`）。
 */
bool instrHookSetPatch(std::uint32_t handle, bool enabled, InstrPatchKind kind, std::uint32_t index,
                       std::uint32_t value, std::uint32_t max_hits, const char** err);

/** ② 读：给句柄设采样清单（最多 8 个 int / 8 个 ptr；空表 = 不采样）。 */
bool instrHookSetCapture(std::uint32_t handle, const std::vector<std::uint32_t>& ints,
                         const std::vector<std::uint32_t>& ptrs);

/** 读一条句柄的读数（`hits` / `restores` / `skips` / 最近一次采样）；没有该句柄 ⇒ false。 */
bool instrHookStats(std::uint32_t handle, InstrHookStatus* out);

/** 撤一条 hook（并把未还原的那次覆盖写回）。没有该句柄 ⇒ false。 */
bool instrHookRemove(std::uint32_t handle);

/** 撤掉所有 hook（并把未还原的写恢复）。Run 收口 / 插件 `clear()` 都走这里。 */
void instrHookClear();

/** 当前所有 hook 的读数（安装顺序）。 */
std::vector<InstrHookStatus> instrHookList();

/** 有没有装 hook（热路径闸门；`lift_instr_hook` 只读这一位）。 */
bool instrHooksActive();

/**
 * 派发点探针（**唯一调用方** = AOT 注入的 `lift_instr_hook`）。
 * 做的事：恢复上一次未还原的写 → 找命中的 hook → 采样 / 执行覆盖并登记待恢复。
 */
void instrHookProbe(X86& cpu);

}  // namespace lift
