// emulator-lift · **脚本指令位置钩子**（插件面能力；正式实现）
//
// 是什么：VM 的脚本派发是 3 个 `call eax/edx`（`aot_hook_ea.h` 的 `kScriptDispatchEas`），
//   生成器在这 3 个站点注入 `call void @lift_instr_hook(ptr %cpu)` ⇒ **每条脚本指令执行前**
//   宿主都能看一眼"现在要执行的是哪个脚本的哪个位置"，并可选择在**这条指令读之前**临时改一个
//   dword，指令执行完（下一条脚本指令派发前）再**逐字节恢复**。
//
// 为什么不是"改 guest 内存"：`eb5c1` 这类表是**持久**的（进存档）。本能力只在"那一瞬间"改，
//   结束后净变化 0 —— 语义上是"让本条指令读到一个不同的值"，不是"改游戏数据"。
//
// 三条纪律：
//   · **缺省零成本**：没有装任何 hook 时 `lift_instr_hook` 只读一个 latched atomic<bool>；
//   · **只读/写 guest 内存，不碰任何语义**：命中判据只有 `(脚本身份, pos)`，⛔ 没有"按 EA 特判"；
//   · **恢复是义务**：写入前保存 raw（含 ENC 编码位），下一次任一派发先恢复；`clear()` / Run 收口
//     也恢复。⛔ 不允许"写完不管"。
//
// 位置坐标：`pos = 0x3C + (pc − 脚本体基址)` —— 与 `dist/views/data/*.txt` 的 `label_%08x` 同坐标
//   （真源 `runtime/engine_marker.h` 的 `readScriptPositionAt`；本模块只共用公式，不复制判据）。
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

struct InstrHookSpec {
    std::string script;                 // marker 文件名（精确 `==`，含 `.BIN`，如 `DRAWOBJ.BIN`）
    std::uint32_t pos = 0;              // 脚本内位置（label 坐标）
    bool patch = true;                  // false = 只采样（capture），不写
    InstrPatchKind kind = InstrPatchKind::PtrDword;
    std::uint32_t index = 0;
    std::uint32_t value = 0;
    std::uint32_t maxHits = 0;          // 0 = 不限（写只在 maxHits 内；采样一直记最后一次）
    std::vector<std::uint32_t> capture_ints;  // 要采样的 int local 下标
    std::vector<std::uint32_t> capture_ptrs;  // 要采样的 ptr local 下标（值 = 指针本身）
};

struct InstrHookStatus {
    std::string script;
    std::uint32_t pos = 0;
    bool patch = true;
    InstrPatchKind kind = InstrPatchKind::PtrDword;
    std::uint32_t index = 0;
    std::uint32_t value = 0;
    std::uint64_t hits = 0;
    std::uint64_t restores = 0;
    std::uint64_t skips = 0;
    std::uint32_t last_id = 0;                      // 最近命中的脚本文件 id
    std::vector<std::uint32_t> last_ints;           // capture_ints 的语义值（最近一次命中）
    std::vector<std::uint32_t> last_ptrs;           // capture_ptrs 的指针值（最近一次命中）
    std::vector<std::uint32_t> last_ptr_targets;    // 上面每个指针指向的 dword（语义值；不可读 = 0）
};

/**
 * 装一条 hook。失败 ⇒ false + `*err` 静态字面量（`"script_empty"` / `"pos_zero"` / `"duplicate"` /
 * `"table_full"`）。同一 `(script,pos)` 只能装一条（⛔ 不静默覆盖）。
 */
bool instrHookInstall(const InstrHookSpec& spec, const char** err);

/** 撤掉所有 hook（并恢复未还原的写入）。Run 收口 / 插件 `clear()` 都走这里。 */
void instrHookClear();

/** 当前所有 hook 的读数（顺序 = 安装顺序）。 */
std::vector<InstrHookStatus> instrHookStatus();

/** 有没有装 hook（热路径闸门；`lift_instr_hook` 只读这一位）。 */
bool instrHooksActive();

/**
 * 派发点探针（**唯一调用方** = AOT 注入的 `lift_instr_hook`）。
 * 做的事：恢复上一次未还原的写 → 找命中的 hook → 采样 / 写覆盖并登记待恢复。
 */
void instrHookProbe(X86& cpu);

}  // namespace lift
