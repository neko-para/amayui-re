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

#include <cstddef>
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

// ---- 订阅形状（S2c）----
// ★ 为什么 form 是枚举而不是"一个 pos + lo/hi 的联合"：v1 的 `pos` 语义必须逐字保留
//   （`InstrHookStatus.pos` / `hook.list().pos` / `duplicate` 判据都读它）。
enum class InstrHookForm : std::uint8_t {
    Pos = 0,     // {script, pos}
    Range = 1,   // {script, lo, hi} —— [lo, hi)
    Script = 2,  // {script}（整本脚本，每一拍都命中）
    Opcode = 3,  // {opcode} / {script, opcode}
};

inline const char* instrHookFormName(InstrHookForm form) {
    switch (form) {
        case InstrHookForm::Pos:
            return "pos";
        case InstrHookForm::Range:
            return "range";
        case InstrHookForm::Script:
            return "script";
        case InstrHookForm::Opcode:
            return "opcode";
    }
    return "?";
}

// ★ 三种**用途**（决定去重、链参与、Run 存活），⛔ 不要合并成 bool：
//   V1At = `ctx.hook.at`（v1：按 (script,pos) 全局去重、不参与链、不算 hasPendingWork）
//   Sub  = `ctx.at`（v2：允许重复、参与链、算 hasPendingWork）
//   Once = `ctx.once` 的临时 hook（v2：允许重复、被动、算 hasPendingWork）
enum class InstrHookKind : std::uint8_t { V1At = 0, Sub = 1, Once = 2 };

inline const char* instrHookKindName(InstrHookKind kind) {
    switch (kind) {
        case InstrHookKind::V1At:
            return "v1";
        case InstrHookKind::Sub:
            return "sub";
        case InstrHookKind::Once:
            return "once";
    }
    return "?";
}

/** 一条订阅的匹配条件。`form` 决定哪一个字段有效；`script` 空 = 不按脚本过滤（只允许 Opcode）。 */
struct InstrHookWhere {
    InstrHookForm form = InstrHookForm::Pos;
    std::string script;
    std::uint32_t pos = 0;     // Pos
    std::uint32_t lo = 0;      // Range
    std::uint32_t hi = 0;      // Range
    std::uint32_t opcode = 0;  // Opcode（form == Opcode 时总参与）
};

/** `Tick.local.*` 的三类读（与 `InstrPatchKind` 同一族概念：都是"local 的哪一格"）。 */
enum class InstrLocalKind : std::uint8_t { Int = 0, Ptr = 1, PtrDword = 2 };

/** 一次命中现场里 local 的**取址视图**（由 `InstrHit` 的散字段装配；值本身按需读）。
 *  ★ 为什么带 `has_key`：语义值 = DEC(raw, key)（`value_codec.h`）；没有 key 就不能声称是语义值。 */
struct InstrLocalView {
    std::uint32_t int_base = 0;
    std::uint32_t int_count = 0;
    std::uint32_t ptr_base = 0;
    std::uint32_t ptr_count = 0;
    std::uint32_t codec_key = 0;
    bool has_key = false;
};

struct InstrHookStatus {
    std::uint32_t id = 0;                 // 句柄（`ctx.hook.at` 返回的那个）
    std::string script;
    std::uint32_t pos = 0;
    // ★ S2c 的观测面：用例要能判"装进去的是什么形状"（`hook.list()`），否则只能判 hits。
    //   ⛔ 复用不了 `kind`（下面那个是 **patch** 的种类，v1 语义）；这两个是新字段。
    //   `form != Pos` 时 `pos` 保持 0（form 决定哪个字段有意义，pos==0 不是错）。
    InstrHookKind hook_kind = InstrHookKind::V1At;
    InstrHookForm form = InstrHookForm::Pos;
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    std::uint32_t opcode = 0;
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
 * 一次命中现场的**只读值视图**（S2a 的反向 sink：指令点命中 → ui 层 JS 回调）。
 * ★ 为什么是值而不是 `X86&` / `GuestMemory*`：
 *   ① 方向是 `src/runtime` → `src/ui`；把可变 guest 句柄交出去 = 在 `EditSink` 之外开出**第二条
 *      guest 写路径**（合同 §6 行 5/22 的口径）；
 *   ② probe 由 AOT 注入的 `extern "C" lift_instr_hook` 调 ⇒ 传引用要担心生命周期，传纯值不会悬垂；
 *   ③ `Tick.local`（S3）要的是"按需读"的**能力**，不是 `X86&`。
 * ★ `int_base/int_count/ptr_base/ptr_count/codec_key` 是 S2a **不消费**的字段：它们已经在
 *   `sampleFrame` 的 10 次读里付过钱了，留给 S3 的 `Tick.local` / capture 的按需读。
 */
struct InstrHit {
    std::uint32_t handle = 0;       // 命中的 hook 句柄（== InstrHookStatus::id）
    std::uint32_t script_id = 0;    // 本次命中的脚本文件 id（frame+0x3C）
    // ★ S2c：`script` 的语义从"订阅注册名"收紧成"**本次派发的脚本名**"（`Frame::name`）——
    //   `{opcode}` / `{script}` 形态的注册名可能是空串或与当前脚本无关，而 `Tick.now.script`
    //   必须说"这一拍在哪"。两者在 `{script,pos}` 下构造上相等（只有名字相等才会命中）⇒ v1 无感。
    //   生命周期 = **本次 `on_hits` 调用**（指向 probe 局部的 `Frame::name`）；消费者要拷贝。
    const char* script = nullptr;
    std::uint32_t pos = 0;          // label 坐标 = 0x3C + (pc − body)
    std::uint32_t opcode = 0;       // PC 上的 opcode（`mem.rd32(pc)`）
    bool opcode_ok = false;         // false ⇒ 读不到（⛔ 不编 0；JS 侧缺键）
    std::uint64_t hits = 0;         // **本次命中之后**的累计命中（== `stats().hits`）
    // ---- local 的取址视图（`Tick.local` 的按需读；S2a 只有值、没有读者）----
    std::uint32_t int_base = 0;
    std::uint32_t int_count = 0;
    std::uint32_t ptr_base = 0;
    std::uint32_t ptr_count = 0;
    std::uint32_t codec_key = 0;
    // ★ S2c：= `Frame::has_key`（`local.*` 没有 key ⇒ undefined）。与 `opcode_ok` 同一条诚实口径。
    bool codec_ok = false;
};

/**
 * 反向通道（runtime → ui）。★ 签名里**一个 JS 类型都没有**：`quickjs.h` 只许出现在
 * `ui/quickjs_value.h`（守卫 `tools/test/core-shape.test.mjs`）⇒ runtime 只能给函数指针。
 * · `on_hits`：一次**派发**一次调用，`hits[0..count)` 按**注册序**（= entries 顺序 = handle 升序），
 *   `count >= 1`。★ 为什么是批量而不是逐条：`ctx.at` 的链要"这批命中的全部、且这就是最后一条"这个
 *   事实；逐条回调下 ui 无法知道批次边界（要么加 begin/end 两个钩子 = 两套口径）。
 *   回调里改 hook 表是**允许**的（分发在 entries 遍历之后，且 hits 已经物化成值）⇒ 之后 `ctx.off` /
 *   `hook.clear` 都安全；`hits[i].script` 的生命周期 = **本次 `on_hits` 调用**。
 * · `on_released`：条目被 **remove / releaseOwner / clear** 摘掉时逐条调（ui 释放自己那份 JS 值）。
 */
struct InstrHookSink {
    void* user = nullptr;
    void (*on_hits)(void* user, const InstrHit* hits, std::size_t count) = nullptr;
    void (*on_released)(void* user, std::uint32_t handle) = nullptr;
};

/**
 * 注册 / 注销。同一个 `user` 幂等（重复注册 = 替换）；
 * 多个 sink 全部收事件（生产 1 个；测试夹具可能同时活 2 个 `Impl`）。
 * ★ `instrHookAddSink` 不做线程同步：与 hook 表同一口径（S0 的 U2 未解，见施工图 §未证）。
 * ★ 生命周期：ui 层在实例创建后注册、`shutdown` **第一行**注销（之后任何 probe / release
 *   都不许回调进一个正在拆的 Impl）。
 */
void instrHookAddSink(const InstrHookSink& sink);
void instrHookRemoveSink(void* user);
/** 热路径闸门：probe 只在"有 sink"时才收集命中 id（避免每次命中都 push 一个临时 vector）。 */
bool instrHookHasSink();

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
 * 装一条**任意形状**的订阅（S2c）。`owner` = 本条订阅的归属（`PluginHost` 传 `run_serial_`）。
 * 失败 ⇒ 0 + `*err` 静态字面量：
 *   `script_empty` / `pos_zero` / `range_empty` / `bad_where` / `duplicate`（**只有 V1At**）/ `no_injection`。
 * ★ `duplicate` 只对 `kind == V1At`：v1 的「同一 (script,pos) 只能一条」是**已发布语义**
 *   （合同 §4.5）；v2 的 `Sub` / `Once` 是**新面**，同点多条是它的用法（多 handler / 链）。
 * ★ 每条订阅 = 一条 Entry（各自 handle、同一个 owner）。
 */
std::uint32_t instrHookAddEx(std::uint64_t owner, const InstrHookWhere& where, InstrHookKind kind,
                             const char** err);

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
 * ★ S2c-2：pending 是**多槽**的（`State::pendings`）⇒ 本函数取消**所有** `addr` 相同的槽
 *   （同址合并后本来就只可能有一条，但语义按"所有"写 —— 别让将来的实现依赖"至多一条"）。
 */
void instrHookCancelPendingAt(std::uint32_t addr);

/** 有没有装 hook（热路径闸门；`lift_instr_hook` 只读这一位）。 */
bool instrHooksActive();

/**
 * ★ S2c-2：`Tick.patch`（**本条指令**的 one-shot 覆盖）。
 *
 * 语义 = 只在**本次派发**（`instrHookProbe` 的这一次 `on_hits`）生效：handler 里设、`on_hits`
 * 返回之后由 probe 应用（⛔ 不早于 handler —— handler 必须看到"打补丁前"的读数），下一拍派发前
 * 按 CAS 恢复。`handle` 必须是**本拍命中**的条目（`hit #` 与当前 probe 的序号相等），否则 false。
 *
 * 失败 ⇒ false + `*err` 静态字面量：
 *   `no_such_hook`（条目已撤）/ `not_hit`（不是本拍命中 / 不在派发中）/
 *   `once_tick`（`once` 的临时条目在 resolve 之后立刻撤 ⇒ 这次覆盖无处落地，⛔ 不假装收下）/
 *   `bad_target`（目标不可解析：下标越界 / 指针 0 / 全局池读不到）/
 *   `no_key`（本帧没有 ENC key ⇒ 不能把语义值写成 dword）/
 *   `too_many_slots`（本拍计划中的不同地址已到 `kMaxPatchSlots`）。
 * ★ 一个条目只有**一个** one-shot 位（同一 handler 多次调用 = 最后一次赢）；不同 handler 各自
 *   归到自己那条订阅（ui 层的 `tick_writer`），所以"同一 Tick 对象给多个 handler 用"不会互踩。
 */
bool instrHookSetTickPatch(std::uint32_t handle, InstrPatchKind kind, std::uint32_t index,
                           std::uint32_t value, const char** err);

/** 本拍 pending 槽的上限（同址合并后仍按"不同地址"计；超出 ⇒ 该 entry `skips += 1`）。 */
inline constexpr std::size_t kMaxPatchSlots = 8;

/**
 * 派发点探针（**唯一调用方** = AOT 注入的 `lift_instr_hook`）。
 * 做的事：恢复上一次未还原的**全部**写 → 找命中的 hook → 采样 / 每条 entry 各执行一次覆盖并登记
 * 待恢复 → 一次 `on_hits` → 应用本拍 `tick.patch` 的 one-shot 覆盖（再各登记一个槽）。
 */
void instrHookProbe(X86& cpu);

}  // namespace lift
