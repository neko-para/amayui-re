// emulator-lift · **插件面**（语言无关）：`PluginScript` = 插件回调的宿主侧形状，
//                 `PluginCallHost` = 回调能调的宿主操作（`ctx.*` 的唯一来路）。
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.5（`Context` 的操作类）、§4.6（JS 单插件契约）、
//       §5 C2（判据）。
// 父级裁决：`.tmp/plugin-runtime/c2-decisions.md` D-C2-5（`render` 在 headless 下的形状）、
//       D-C2-6（`ctx.read.frameHash` 不安装）、D-C2-7（`ctx.now` 每次回调新建值对象）、
//       D-C2-9（`finish` 只结束 Run；`quit` 走第二条 stop 源）。
//
// ★ 本头是**语言无关**的：`quickjs.h` ⛔ **不许**出现在这里（唯一 TU 是 `ui/script_quickjs.cpp`，
//   守卫 `tools/test/core-shape.test.mjs` 行 11）。⇒ 本头只认 `Trigger` / `Receipt` /
//   `GlobalValue` / `InputPoint` 这些**核心值类型**，不认任何 JS 引擎类型。
//
// ★ 为什么 `PluginScript` **继承** `UiScript`（C2b 定稿）：C2a 时它还不认识 `render`，所以保持独立；
//   C2b 起 `render` 是插件的第三个可选成员（d.ts `PluginModule.render?`），而**唯一的**渲染出口是
//   `UiScript::render(builder, readings)`（present_sdl 的叠层路径只认这个接口，`adoptOverlayScript`
//   的形参就是它）。⇒ 多继承一个 `UiScript` 是形状最省的选择：**同一个实例**既跑回调链（每读点）
//   又画本帧（每帧），⛔ 不需要第二个 QJS context / 第二个实例（合同 §6 行 9 / 11）。
//   ★ 两条面的失败口径一致（`err` 形状 `<origin>: <message>\n<stack>`），但预算两档
//   （回调 2 ms/100k vs render 20 ms/1e6）**不在这里合并**：`UiScript::setBudget` 是 render 的入口，
//   回调预算由 `start/onTrigger` 的 `Budget` 形参给。
//   ★ `render` 是**可选**的：没有 `render` 的插件（纯 Run / headless）实现 `UiScript::render` 时
//   返回 true + 空 list（⛔ 不是失败 —— present_sdl 只在挂了 overlay 时调它）。
//
// ★ **`ctx` 不是全局**：每次回调由 `PluginScript` 新建一个值对象（`ctx.now` 更是每回调一个新值
//   快照，D-C2-7）。⇒ 本头没有"当前 ctx"这种状态：`PluginCallHost` 的每个方法都**隐含**"现在正在
//   那次回调里"（宿主在回调外调用它们是实现错误，由宿主自己守）。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "runtime/budget.h"          // Budget（每次回调的墙钟 + op 上限；只有一个真源）
#include "runtime/appdata.h"         // AppDataStat（ctx.appdata.stat 的结果对象；语言无关）
#include "runtime/config.h"          // ConfigValue / ConfigReceipt（ctx.config 的值与回执；语言无关）
#include "runtime/instr_hook.h"      // InstrHookStatus / InstrPatchKind（ctx.hook 的语言无关值）
#include "runtime/input_device.h"    // MouseButton
#include "runtime/input_point.h"     // InputPoint
#include "runtime/script_globals.h"  // GlobalKind / GlobalValue（EngineRead / EditSink 的词汇）
#include "runtime/trigger.h"         // Trigger / TriggerEvent（armed 集合与事件）
#include "runtime/intent.h"          // Receipt（Input 的回执；与 Edit 共用同一份形状）
#include "ui/script.h"               // UiScript / UiFrameReadings（C2b：render 的**唯一**出口）

namespace lift {
class FontEngine;
}  // namespace lift

namespace lift::ui {

using PluginParams = std::map<std::string, std::string>;

// `ctx.input.status(id)` 的四态（d.ts 的 `"queued" | "applied" | "failed" | "unknown"`）。
// ★ `IntentQueue::Status::Dropped` 映射成 `"failed"`（`c2-survey.md` §2 的逐项表）。
enum class PluginInputState : std::uint8_t {
    Queued = 0,
    Applied = 1,
    Failed = 2,
    Unknown = 3,
};

inline const char* pluginInputStateName(PluginInputState state) {
    switch (state) {
        case PluginInputState::Queued:
            return "queued";
        case PluginInputState::Applied:
            return "applied";
        case PluginInputState::Failed:
            return "failed";
        case PluginInputState::Unknown:
            return "unknown";
    }
    return "unknown";
}

// =====================================================================================
// `ctx.*` 的宿主侧（唯一实现 = `src/plugin/plugin_host.cpp` 的 `PluginHost`）
//
// ★ 四条操作类语义（⛔ 不要在这里合并成一个"exec"）：
//   · **Edit**（`editGlobalSet`）：**立即**同步写，不排队；失败走 receipt 的 `code`
//     （`globalErrorName()` 的单一词表），⛔ 不 `exit 5`；
//   · **Input**（`inputClick/Move/Down/Up`）：进 `IntentQueue`，**下一读点**才可能落地；
//   · **EngineRead**（`readGlobal`）：同步只读、不暂停；失败走 `code`（同一份词表）；
//   · **Trigger**（`makeAfter*`）：每次调用造**新对象**（= 新预算 / 新边沿；合同 §4.2 417–419）。
//   ★ 另有 **AppData**（`appDataStat` / `appDataAvailable` / `appDataPath`）：同步只读宿主 I/O，
//     **只 appdata 子树**、只装 RunCtx；它的宿主段必须包 `HostPause`（不计进用户 2 ms）。
//
// ★ `now()` 是本读点的读数（`Context::now` 的同一个指针）；⛔ 回调外没有"当前读数"。
//
// ★ **`read` / `log` 在 `render` 里也会被调**（C2b 的 `RenderCtx.read.global.get` / `ctx.log`）：
//   ⇒ 实现方**不许**把这两个口绑在"必须正在 Run 回调里"（`PluginHost` 的 `in_callback_`）上；
//   而**写类**（Edit / Input / Trigger 工厂）仍必须只在回调里可用 —— 那是"render 不做 guest 写"
//   （合同 §4.4c）的唯一落点。`profile` **两边都有**（S2：RenderCtx 有自己的一份 trampoline，
//   且 `RenderCtx.profile.end()` 返回 `{ok,text,error}` —— 见 `profileEndText`）。
// =====================================================================================
class PluginCallHost {
public:
    virtual ~PluginCallHost() = default;

    // ---- 装配检查（都是**实现错误**，不是用户输入）----
    // false ⇒ 宿主没接这条操作类 ⇒ JS 侧一律 **TypeError**（⛔ 不静默 no-op、⛔ 不编一个假回执）。
    // 为什么分成四个而不是一个 `wired()`：`ctx.edit/input/read/trigger` 是四条独立装配线
    // （测试夹具只接其中两条是合法的；`C1` 的 trigger 用例就只接 `now` + `trigger`）。
    virtual bool editWired() const = 0;
    virtual bool inputWired() const = 0;
    virtual bool readWired() const = 0;
    virtual bool triggerWired() const = 0;

    // ★ S2c-2：`Tick.edit` / `Tick.input` 的**装配**检查 —— **不含** `in_callback_`，也**不含**
    //   "正在 Run 回调里"。为什么必须与上面那两条分开：tick 入口 `in_callback_` 恒 false（那是
    //   S2a/S2b 刻意留的"只读面"），而写面又不是"把 in_callback_ 打开"（那会一次放开
    //   `ctx.fail/finish/quit/trigger/hook.*/profile/config` 七个副作用）。
    //   ⇒ tick 的写 trampoline 用**这两条**（装配）+ `tickRefOf`（这一拍没 stale）；v1 的
    //   `ctx.edit` / `ctx.input` 仍走 `editWired()` / `inputWired()`（含 `in_callback_`）。
    //   ⛔ 不要用这两条去放宽 v1 的门（`render 不做 guest 写` 的落点仍是 `editWired`）。
    virtual bool editAttached() const = 0;
    virtual bool inputAttached() const = 0;

    // 现在是否在 **Run 回调**（`start` / `onTrigger`）里；`render` 期间恒 false（C2b 复审 F2）。
    // ★ 为什么需要这一位：render 期间 `read` / `log` 也要能用 ⇒ 宿主（`call_host`）在 render 期
    //   **非空**；于是"只判 `call_host != nullptr`"的出口（`fail` / `finish` / `quit` / `profile.*`）
    //   会从 render 可达 —— 插件在 `onTrigger` 里把 `ctx` 存进模块变量，下一帧 `saved.fail(...)`
    //   就绕过了 `RenderCtx` 的成员表。这 5 个 trampoline 必须判这一位。
    //   ⛔ 这不是安全边界、guest 也不会坏（写面另有 `*Wired()` 把关），而是"成员表不是画出来的门面"。
    virtual bool inCallback() const = 0;

    // ★ S2b：本 Run 是否**已经**失败。泵 / task settle 用它做"不重复 fail"——`fail()` 会覆盖
    //   `reason`，后到的那一条会把真正的失败原因（例如 `once timeout`）换成无关文案。
    //   默认 false（测试替身 / 未来的其它宿主不必被迫实现）。
    virtual bool failed() const { return false; }

    // ★ S2b-1（复审 S2-1）：本 Run 是否**已经到终态**（`failed` / `finished` / `quit` / runner failed）。
    //   为什么不能只判 `failed()`：`ctx.finish()` / `ctx.quit()` **不**置 `failed_` ⇒ 一个在
    //   finish/quit 之后同一入口里拒绝的 task 会把 `run=finished exit=N` **翻成 failed**、
    //   并把 `exit` 读数覆盖掉。⇒ 泵 / task settle 的"Run 已收口"判据一律用它：
    //   已到终态 ⇒ **静默丢弃**待推工作（不 fail、不覆盖读数）；否则 rejected task ⇒ `fail("task", …)`。
    //   ⛔ 不把 `runner.done()` 算进来：正常 done 的 Run 仍要每帧 render（与 `onReadPoint` 的泵门同口径）。
    virtual bool runTerminal() const = 0;

    // ---- Edit（立即）----
    // 返回 false ⇒ `*code` = `globalErrorName()` 认的那三个之一（⛔ 不是新词表）。
    virtual bool editGlobalSet(lift::GlobalKind kind, std::uint32_t id, const lift::GlobalValue& value,
                              const char** code) = 0;

    // ---- Input（串行；下一读点 / 一读点一条）----
    virtual lift::plugin::Receipt inputClick(int x, int y, lift::MouseButton button) = 0;
    virtual lift::plugin::Receipt inputMove(int x, int y) = 0;
    virtual lift::plugin::Receipt inputDown(lift::MouseButton button) = 0;
    virtual lift::plugin::Receipt inputUp(lift::MouseButton button) = 0;
    virtual PluginInputState inputStatus(std::uint64_t id) = 0;

    // ---- EngineRead（同步只读 / 无暂停）----
    virtual bool readGlobal(lift::GlobalKind kind, std::uint32_t id, lift::GlobalValue* out,
                            const char** code) = 0;

    // ---- Tick.local（S2c；**只读 guest**，只 tick 入口用）----
    // ★ 纯虚：唯一实现者 = `PluginHost`。⛔ 不给默认实现 —— 那会让"没接线"静默退化成 `undefined`
    //   （与 `readWired()` 的口径冲突：装配错误必须响亮）。
    // ★ 语义值口径：int / ptrDword = DEC(raw, key)（`value_codec.h`）；ptr = 槽里原样（地址不解码）。
    //   越界 / 未提交 / 指针 0 / 没有 key ⇒ **false** ⇒ JS 侧 `undefined`（⛔ 不编 0）。
    virtual bool readLocal(const lift::InstrLocalView& view, lift::InstrLocalKind kind,
                           std::uint32_t index, std::uint32_t* out) = 0;

    // ---- 脚本指令位置钩子（`ctx.hook`；**只 RunCtx**）----
    // ★ 三种能力**彼此独立**（用户裁决 2026-11-26 晚：触发 ≠ 修改）：
    //   ① 触发 `instrHookAdd(script,pos)`（纯观察，不读不写；返回句柄 id）；
    //   ② 读   `instrHookSetCapture(handle, ints, ptrs)` + `instrHookStats`（采样最近一次命中）；
    //   ③ 改   `instrHookSetPatch(handle, …)`（在这条指令读之前覆盖一个 dword，指令后恢复 ⇒ 净变化 0）。
    // ★ 实现 = `runtime/instr_hook.h`（唯一一份）；本接口只是语言无关的那层。
    virtual std::uint32_t instrHookAdd(const std::string& script, std::uint32_t pos, const char** err) = 0;
    // ★ S2c：`ctx.at(where, handler)` 的注册口 —— 每条订阅一条 runtime Entry（各自 handle、
    //   同一个 owner）。`instrHookAdd` 是它的 v1 薄包装（Pos + V1At，去重只对 V1At）。
    virtual std::uint32_t instrHookAddWhere(const lift::InstrHookWhere& where, lift::InstrHookKind kind,
                                            const char** err) = 0;
    virtual bool instrHookSetPatch(std::uint32_t handle, bool enabled, lift::InstrPatchKind kind,
                                   std::uint32_t index, std::uint32_t value, std::uint32_t max_hits,
                                   const char** err) = 0;
    // ★ S2c-2：`Tick.patch(target, value)` 的 one-shot 覆盖（runtime 侧只对**本拍命中**的条目生效，
    //   在 `on_hits` 返回之后应用）。与 v1 的 `instrHookSetPatch` 是**两条独立的路**：
    //   后者是持久覆盖（每拍写、窗口恢复），前者只作用于当前这一次派发。
    virtual bool instrHookSetTickPatch(std::uint32_t handle, lift::InstrPatchKind kind,
                                       std::uint32_t index, std::uint32_t value,
                                       const char** err) = 0;
    virtual bool instrHookSetCapture(std::uint32_t handle, const std::vector<std::uint32_t>& ints,
                                     const std::vector<std::uint32_t>& ptrs) = 0;
    virtual bool instrHookStats(std::uint32_t handle, lift::InstrHookStatus* out) = 0;
    virtual bool instrHookRemove(std::uint32_t handle) = 0;
    virtual void instrHookClear() = 0;
    virtual std::vector<lift::InstrHookStatus> instrHookList() = 0;

    // ---- AppData（同步只读宿主 I/O；**只 RunCtx**）----
    // ★ 没有 `appDataWired()`：`ctx.appdata` 在 RunCtx 里**恒在**（普通对象），没接根时
    //   `available=false` + `path=null` + `stat` 一律 `no_appdata`（⛔ 不"有但抛"，也⛔ 不静默 no-op）。
    // ★ `stat` 的宿主段要包 `HostPause`（宿主 I/O 不计进用户 2 ms；见 `script_quickjs.cpp`）。
    // ★ ⛔ 不进 `RenderCtx`（没有这个成员；render 只经 config 桥写 `plugin-config.json`，
    //   不碰 appdata 子树 / 任意路径 I/O）。
    virtual bool appDataAvailable() const = 0;
    virtual std::string appDataPath() const = 0;  // 空串 = null（不可用）
    virtual lift::plugin::AppDataStat appDataStat(const std::string& rel) = 0;

    // ---- Config（同步宿主 I/O；**`RunCtx` 与 `RenderCtx` 都有** `ctx.config`）----
    // ★ `ctx.config` 的形状 = `{ available, domain(id) }`；域对象 =
    //   `{ get(key, fallback?), has(key), set(key, value), delete(key), all() }`（d.ts 的
    //   `ConfigApi` / `ConfigDomain` / `ConfigReceipt`）。
    // ★ 配置文件 = `<--appdata>/plugin-config.json`（appdata 根；⛔ 不在 `SAVE/`）。**没有**
    //   `available` ⇒ `domain()` 抛 `Error("no_appdata")`（⛔ 不做内存态）。
    // ★ 语义：`set` / `delete` 只改内存 + 标脏，宿主在**每个 Run 回调末**与**每帧 render 之后**
    //   各 flush 一次（原子替换）；校验错误（域 id / key / 值类型 / 超限 / 非法 UTF-8）由宿主返回
    //   false + `why` ⇒ JS 侧 TypeError；`ConfigReceipt.applied` = **内存已改**（⛔ 不是"已落盘"）。
    // ★ 值的实现面只有一份 = `lift::plugin::ConfigBridge`（`runtime/config.h`）：
    //   RunCtx 的 `ctx.config` 走**这里**；两条渲染面（经典 `host.config` 与 **`RenderCtx.config`**）
    //   走 `UiHostOps::config`（同一个桥、同一份 `makeConfigObject(impl, /*render=*/true)`）。
    //   ★ 2026-11-26 父级裁决：`config` **加回 `RenderCtx`**（7→8 成员）—— 它挡住 `--overlay`
    //   下最基本的设置页 / 预配置；render 可经宿主桥写 `plugin-config.json`，仍⛔ 不做 guest 写 /
    //   任意路径 I/O（`ctx.appdata` 仍只 RunCtx）。
    //   nullptr = 没接桥 ⇒ RunCtx 里调 `domain()` 是 TypeError（⛔ 不是静默 no-op）。
    virtual lift::plugin::ConfigBridge* config() { return nullptr; }

    // ---- Trigger 工厂（每次调用 = 新对象）----
    virtual lift::plugin::Trigger makeAfterScript(const std::string& name, const std::string& script,
                                                 lift::plugin::TriggerMode mode, std::uint32_t max_fires,
                                                 std::uint64_t timeout_ms) = 0;
    virtual lift::plugin::Trigger makeAfterPosition(const std::string& name, const std::string& script,
                                                   std::uint32_t lo, std::uint32_t hi,
                                                   lift::plugin::TriggerMode mode, std::uint32_t max_fires,
                                                   std::uint64_t timeout_ms) = 0;
    virtual lift::plugin::Trigger makeAfterDelay(const std::string& name, std::uint64_t delay_ms,
                                                lift::plugin::TriggerMode mode) = 0;
    virtual lift::plugin::Trigger makeAtReadPoint(const std::string& name, lift::InputPoint filter,
                                                 std::uint32_t max_fires, lift::plugin::TriggerMode mode,
                                                 std::uint64_t timeout_ms) = 0;

    // 本读点的读数（`Context::now`；`nullptr` = 不在读点上 —— 那时 `ctx.now` 的每个字段都缺席）。
    virtual const lift::plugin::TriggerReadings* now() const = 0;

    // ---- 观测 / 副作用（HostAction 的既有桥）----
    // 一行一条、有界、去重（`ctx.log`；合同 §4.10）。⛔ 不每读点刷屏（实现方自己限量）。
    virtual void log(const char* level, const std::string& message) = 0;
    // `ctx.profile.begin/end`：复用宿主既有的 `ui::UiHostOps`（D-C2-4）；false ⇒ `*err` 是原因。
    virtual bool profileBegin(const std::string& label, std::string* err) = 0;
    virtual bool profileEnd(std::string* err) = 0;
    /**
     * ★ S2 复审 I3：**不打印**的报告版本（`RenderCtx.profile.end()` 的回传路）。
     * `true` ⇒ `*text` = 报告全文（宿主的窗口已关）；`false` ⇒ `*err` = 原因（`text` 留空）。
     * `profileEnd()` 仍保留给 **RunCtx**（`void` 契约：报告只能写 stdout；D-C2-4）。
     */
    virtual bool profileEndText(std::string* text, std::string* err) = 0;

    // ---- 归因（三个出口各有各的语义；⛔ 不要合并）----
    // `fail` = 当前 Run 失败（点名 name / reason；清 armed）；
    // `finish` = 结束 Run + 记录 exit code（**不改进程 exit**；R3）；
    // `quit` = 请求宿主停机（`stop_requested` + detail 归因；D-C2-9）。
    virtual void fail(const std::string& name, const std::string& reason) = 0;
    virtual void finish(int exit_code, const std::string& reason) = 0;
    virtual void quit(const std::string& reason) = 0;
};

// =====================================================================================
// `PluginScript`：一个**已装载**的插件（ES module 的 default 导出）
//
// ★ 三个可选成员：`render` / `start` / `onTrigger`（`render` 可单独存在；`start` 可单独存在 ——
//   S2b 的 v2 兼容层第一处放宽：`ctx.task` / `ctx.once` 驱动的插件不需要 `onTrigger`；
//   `onTrigger` **不能**单独存在（没有 `start` 就永远没人调它）。
//   `render` 的面 = 基类 `UiScript`（C2b 起多继承）：`render(builder, readings, err)` 每次
//   render 调 JS `render(RenderCtx)`；没有 `render` 的插件返回 true + 空 list。
// ★ `start` / `onTrigger` 必须**同步**返回 `Trigger | Trigger[] | null`；返回 Promise / async
//   ⇒ 明确错误（合同 §4.6 805）。返回值翻译成 `std::vector<Trigger>`：
//   空 = done（`null` / `[]` / `undefined`）；重复 `name` 由 `TriggerRunner::install` 判 fail。
//   ★ S2b 的 `start` 单独存在时，返回 `null` 即可：Run 由 task/once（`hasPendingWork`）驱动，
//   收口发生在所有异步工作 settle 之后。
// =====================================================================================
class PluginScript : public UiScript {
public:
    using TriggerSet = std::vector<lift::plugin::Trigger>;

    ~PluginScript() override = default;

    virtual bool hasRender() const = 0;
    // 有 `start`（其 `onTrigger` 可缺省；`render` 可单独存在）。
    virtual bool hasRun() const = 0;

    // 调 JS `start(ctx)`。false ⇒ `*err` 非空（Run fail，且 `*out` 无意义）。
    virtual bool start(PluginCallHost& host, const lift::plugin::Budget& budget, TriggerSet* out,
                       std::string* err) = 0;
    // 调 JS `onTrigger(ctx, ev)`。`ev.kind()` 是 `"trigger"` / `"timeout"`。
    virtual bool onTrigger(PluginCallHost& host, const lift::plugin::TriggerEvent& ev,
                           const lift::plugin::Budget& budget, TriggerSet* out, std::string* err) = 0;

    // 上一次**回调**是否被预算（deadline / op_limit）打断 —— 是 false 的 `start`/`onTrigger` 的原因之一。
    // （render 的预算打断由基类的 `overran()` 报；同一个标志位。）
    bool overran() const override = 0;

    // ★ S2b：Run 存活 = **没有待推的异步工作**。三个来源（语言无关的"有没有"）：
    //   ① `ctx.task` 登记且未 settle 的协程；② 未命中 / 未超时的 `ctx.once`；
    //   ③ `handle.onTick` 的同步订阅（S2a 的 `hook_handlers`）。
    //   ⛔ 普通 `ctx.hook.at` **不算**：它是 v1 观察（`hits` 只增），把它当"Run 还活着"会让
    //   `return []` 永远收不了口（`plugin/hook-release-on-done` 会在当天红）。
    virtual bool hasPendingWork() const = 0;

    // ★ S2b：**非 tick 入口的 async 泵**（没有独立 timer；唯一实现 = `script_quickjs.cpp` 的 QJS TU）。
    // 语义：① `ctx.once` 的 deadline 到期 ⇒ reject（先不 fail）；② 用同一 CPU 预算窗 drain 到静止；
    // ③ drain 之后检查 task settle；④ 若有 timeout ⇒ **现在** fail Run（让 `catch` 先有机会 log）。
    // 调用点只有两个（⛔ 不要新增第三个泵概念）：
    //   · `PluginHost::onReadPoint`：`syncRunnerFailure()` 之后、`runClosed()` 之前；
    //   · `PluginScript::render` 的开头（render 边界；`call_host` 用 persistent 那份）。
    // ★ 泵入口的 `inCallback()` 仍保持 false（那一位在 `PluginHost` 手里），但 ★ P0 起泵是
    //   **Run 侧入口**（`Impl::in_run_entry`）⇒ 里面的 continuation 可以 `once` / `at` / `task` /
    //   `finish` / `fail` / `quit` / `profile` / `config*` / `appdata.stat`；v1 写面
    //   （`ctx.edit` / `ctx.input` / `ctx.trigger` / `ctx.hook.*`）的 `inCallback` 门**没动**。
    virtual void pump(PluginCallHost& host, const lift::plugin::Budget& budget) = 0;

    /**
     * ★ P0：**Run 收口**的脚本侧钩子（`PluginHost::releaseRunHooks` 是唯一调用点）。
     * 语义 = 丢掉**不再推进**的异步工作：目前只有 `ctx.delay` 的 resolve 表（清掉、**不 resolve**、
     * 也不再跑 JS）。为什么要它：`delay` 的 `resolve` 是 JS 引用（必须在 `JS_FreeContext` 之前释放），
     * 而且"Run 已收口还在账上"会让 `hasPendingWork()` 永远真。
     * ★ ⛔ 不清 `task` / `once`：那两样由 instr hook 的 owner 收口与 `Impl::shutdown` 兜底
     * （`once` 被撤时必须 reject —— 正在 await 它的 task 才能 settle；那条语义不能动）。
     * ★ 幂等（和 `releaseRunHooks` 一样可能被多次调用）；实现方自己判 `ctx == nullptr`。
     */
    virtual void onRunClosed() = 0;

    void shutdown() override = 0;

    // `render` 期间 `RenderCtx.read` / `ctx.log` 的宿主来路（**非拥有**）。
    // ★ 由 `PluginHost` 在 `init` 时设（生命周期 = host > script）；nullptr = 没接线 ⇒ 那两个口
    //   在 render 里一律 TypeError（⛔ 不静默 no-op）。
    // ★ 为什么是"装载时设一次"而不是每帧塞进 `UiFrameReadings`：渲染的 readings 是**只读读数**
    //   （`ui/script.h` 头注），而 `read`/`log` 是**操作类**（回调与外设同一份宿主账）。放进 readings
    //   会多一条"这一帧能不能写日志取决于谁填了 readings"的隐性口径。
    virtual void setCallHost(PluginCallHost* host) = 0;

    // `ctx.draw.text` / `ctx.draw.measure` 的**宿主字体**（非拥有；C2b-2）。
    // ★ 为什么是**装载之后**的 setter 而不是只有 `PluginLoadOptions::font`：装载时（`--plugin`
    //   的 CLI 阶段）还不知道模块有没有 `render`，而字体闸门（R5）只对"真的要画"的路径有意义
    //   ⇒ 窗口装配在 `hasRender()` 为真之后才建字体 / 接线。两条路写的是**同一个**字段，
    //   ⛔ 不要各留一份（"装载期有字体、render 期没有"会静默变成 TypeError）。
    // ★ `nullptr` = 没接（测试夹具 / headless）⇒ `measure` / `text` 抛 TypeError（⛔ 不返回 0 尺寸）。
    virtual void setFont(lift::FontEngine* font) = 0;
};

// 装载参数（`origin` = `--plugin <path>` 原样：错误定位与 module 解析根都用它）。
struct PluginLoadOptions {
    std::string origin;
    const PluginParams* params = nullptr;
    // C2b 的 `ctx.draw.measure` 用（度量走宿主字体，`ui/raster.h` 的 `measureText`）；
    // noexcept：nullptr ⇒ `measure` 抛 TypeError（⛔ 不返回 0 尺寸冒充可用）。
    // ★ 窗口装配走 **late setter**（`PluginScript::setFont`）：装载时还不知道模块有没有 `render`。
    //   直接装载（测试夹具）仍可在这里给；两条写同一个字段。
    FontEngine* font = nullptr;
};

// 装载一个插件（实现 = `ui/script_quickjs.cpp` 的唯一 QJS TU）。
//   · 失败 ⇒ 返回 nullptr 且 `*err` 非空（形状 `<origin>: <message>\n<stack>`）；
//   · **只读 ES module 的 default 导出**（⛔ 不读 `globalThis` / 命名导出）；
//   · `render`/`start`/`onTrigger` 都可选，**至少一个**；`onTrigger` 不能单独存在（没有 `start`
//     就永远没人调它）；`start` 可单独存在（S2b 的 v2 兼容层：`ctx.task` / `ctx.once` 驱动）；
//     类型不是函数 ⇒ 错误（带 `file:line`）。
//   · `render` 的 JS 契约 = `render(RenderCtx)`（C2b）；装载后由 `UiScript::render` 驱动。
//   · `setCallHost` 不在装载参数里：宿主在 `init` 之后自己设（`PluginHost::init` 就会）。
std::unique_ptr<PluginScript> makeQuickJsPlugin(const std::string& source, const PluginLoadOptions& opts,
                                                std::string* err);

}  // namespace lift::ui
