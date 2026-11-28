// emulator-lift · **插件面**（语言无关）：`PluginScript` = 插件回调的宿主侧形状，
//                 `PluginCallHost` = 回调能调的宿主操作（`ctx.*` 的唯一来路）。
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.5 / §4.6 / §5 C2。
// 最终形态（2026-11-28 父级裁决）：**只剩 v2** —— RunCtx = `at/once/waitAny/task/off/finish/
//   quit/fail/config/appdata/read/params/profile`；Tick = `now/local/edit/input/patch`
//   （guest 写**只**走 Tick）；RenderCtx = `draw/ui/config/game`。
//   v1 兼容层（`onTrigger` / Trigger 工厂 / armed 集合 / `ctx.hook` / `ctx.now` / `ctx.state` /
//   `ctx.ui` / `ctx.log` / `ctx.edit` / `ctx.input`）已整条删除。
//
// ★ 本头是**语言无关**的：`quickjs.h` ⛔ **不许**出现在这里（唯一 TU 是 `ui/script_quickjs.cpp`，
//   守卫 `tools/test/core-shape.test.mjs` 行 11）。⇒ 本头只认 `Receipt` / `GlobalValue` /
//   `InputPoint` 这些**核心值类型**，不认任何 JS 引擎类型。
//
// ★ 为什么 `PluginScript` **继承** `UiScript`（C2b 定稿）：`render` 是插件的可选成员，
//   而**唯一的**渲染出口是 `UiScript::render(builder, readings)`（present_sdl 的叠层路径只认
//   这个接口，`adoptOverlayScript` 的形参就是它）。⇒ **同一个实例**既跑 Run（每读点）又画本帧
//   （每帧），⛔ 不需要第二个 QJS context / 第二个实例。
//   ★ 两条面的失败口径一致（`err` 形状 `<origin>: <message>\n<stack>`），但预算两档
//   （Run 回调 2 ms/100k vs render 20 ms/1e6）**不在这里合并**。
//
// ★ **`ctx` 不是全局**：每次入口由 `PluginScript` 新建一个值对象。⇒ 本头没有"当前 ctx"这种状态：
//   `PluginCallHost` 的每个方法都**隐含**"现在正在那次入口里"（宿主在入口外调用它们是实现错误，
//   由宿主自己守）。
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "runtime/appdata.h"         // AppDataStat（ctx.appdata.stat 的结果对象；语言无关）
#include "runtime/budget.h"          // Budget（每次 Run 回调的 CPU + op 上限；只有一个真源）
#include "runtime/config.h"          // ConfigValue / ConfigReceipt（ctx.config 的值与回执；语言无关）
#include "runtime/instr_hook.h"      // InstrHookStatus / InstrPatchKind（Tick/at 的语言无关值）
#include "runtime/input_device.h"    // MouseButton
#include "runtime/input_point.h"     // InputPoint
#include "runtime/intent.h"          // Receipt（Input 的回执；与 Edit 共用同一份形状）
#include "runtime/script_globals.h"  // GlobalKind / GlobalValue（EngineRead / EditSink 的词汇）
#include "ui/script.h"               // UiScript / UiFrameReadings（render 的**唯一**出口）

namespace lift {
class FontEngine;
}  // namespace lift

namespace lift::ui {

using PluginParams = std::map<std::string, std::string>;

// `ctx.input.status(id)` 的四态（d.ts 的 `"queued" | "applied" | "failed" | "unknown"`）。
// ★ `IntentQueue::Status::Dropped` 映射成 `"failed"`。
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
//   · **Edit**（`editGlobalSet`）：**立即**同步写，不排队；失败走 receipt 的 `code`；
//   · **Input**（`inputClick/Move/Down/Up`）：进 `IntentQueue`，**下一读点**才可能落地；
//   · **EngineRead**（`readGlobal`）：同步只读、不暂停；失败走 `code`；
//   · **Tick 写面**（`instrHookSetTickPatch`）：只作用于本拍命中的条目（one-shot）。
//
// ★ `read` 在 `render` 里也会被调？—— 最终形态的 `RenderCtx` **没有 `read`**（只有
//   `draw/ui/config/game`）⇒ `read` 只服务 RunCtx / Tick 语境，绑 `in_callback_` 与否由实现定。
//
// ★ 三条装配线（Edit / Input / Read）各判各的：
//   false ⇒ 宿主没接这条操作类 ⇒ JS 侧一律 **TypeError**（⛔ 不静默 no-op、⛔ 不编一个假回执）。
// =====================================================================================
class PluginCallHost {
public:
    virtual ~PluginCallHost() = default;

    // ---- 装配检查（都是**实现错误**，不是用户输入）----
    virtual bool editWired() const = 0;
    virtual bool inputWired() const = 0;
    virtual bool readWired() const = 0;

    // ★ `Tick.edit` / `Tick.input` 的**装配**检查 —— **不含** `in_callback_`，也**不含**
    //   "正在 Run 回调里"。tick 入口 `in_callback_` 恒 false，而写面不是"把 in_callback_ 打开"。
    //   ⇒ tick 的写 trampoline 用**这两条**（装配）+ `tickRefOf`（这一拍没 stale）。
    virtual bool editAttached() const = 0;
    virtual bool inputAttached() const = 0;

    // 现在是否在 **Run 回调**（`start` / task continuation 之外的同步回调）里；render / tick 期间
    //  恒 false。★ 为什么需要这一位：RunCtx 的写面（`edit` / `input`）与 `fail`/`finish`/`quit` 的
    //  门都靠它；render 期间 `call_host` **非空**（config/draw 要用），那把 RunCtx 存进模块变量就能
    //  在 render 里调这些出口 —— 这个门是"成员表不是画出来的门面"的落点。
    virtual bool inCallback() const = 0;

    // ★ 本 Run 是否**已经**失败。泵 / task settle 用它做"不重复 fail"。
    virtual bool failed() const { return false; }

    // ★ 本 Run 是否**请求停机**（`ctx.quit`）。`runInstrHits` 的收口用它判"要不要丢已提交的 input"。
    virtual bool quitRequested() const { return false; }

    /**
     * ★ F3 裁决的 v2 延伸：`ctx.fail` / `ctx.quit` 从**任意 Run 侧入口**（Run 回调 / task
     * continuation / tick handler）返回后，本入口已提交的 input 必须被丢掉（⛔ 不许在下一读点落地）。
     * 默认 no-op（没有输入面的实现）；`PluginHost` 覆盖为 `releaseRunInput()`。
     */
    virtual void releaseAbortedInput() {}

    // ★ 本 Run 是否**已经到终态**（`failed` / `finished` / `quit`）。
    //   为什么不能只判 `failed()`：`ctx.finish()` / `ctx.quit()` **不**置 `failed_` ⇒ 一个在
    //   finish/quit 之后同一入口里拒绝的 task 会把 `run=finished exit=N` **翻成 failed**。
    //   ⇒ 泵 / task settle 的"Run 已收口"判据一律用它。
    //   ⛔ 不把"没有待推工作"算进来：正常 done 的 Run 仍要每帧 render。
    virtual bool runTerminal() const = 0;

    // ---- Edit（立即）----
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

    // ---- Tick.local（**只读 guest**，只 tick 入口用）----
    // ★ 纯虚：唯一实现者 = `PluginHost`。⛔ 不给默认实现 —— 那会让"没接线"静默退化成 `undefined`。
    // ★ 语义值口径：int / ptrDword = DEC(raw, key)；ptr = 槽里原样（地址不解码）。
    //   越界 / 未提交 / 指针 0 / 没有 key ⇒ **false** ⇒ JS 侧 `undefined`（⛔ 不编 0）。
    virtual bool readLocal(const lift::InstrLocalView& view, lift::InstrLocalKind kind,
                           std::uint32_t index, std::uint32_t* out) = 0;

    // ---- 脚本指令点订阅（`ctx.at` / `once` / `waitAny` / `Tick.patch`；**只 RunCtx**）----
    // ★ `instrHookAddWhere` 是 `ctx.at` / `once` / `waitAny` 的注册口；`instrHookSetTickPatch` 是
    //   `Tick.patch` 的 one-shot 覆盖；`instrHookRemove` 是 `ctx.off`。
    //   实现 = `runtime/instr_hook.h`（唯一一份）；本接口只是语言无关的那层。
    virtual std::uint32_t instrHookAddWhere(const lift::InstrHookWhere& where, lift::InstrHookKind kind,
                                            const char** err) = 0;
    virtual bool instrHookSetTickPatch(std::uint32_t handle, lift::InstrPatchKind kind,
                                       std::uint32_t index, std::uint32_t value,
                                       const char** err) = 0;
    virtual bool instrHookRemove(std::uint32_t handle) = 0;

    // ---- AppData（同步只读宿主 I/O；**只 RunCtx**）----
    // ★ 没有 `appDataWired()`：`ctx.appdata` 在 RunCtx 里**恒在**（普通对象），没接根时
    //   `available=false` + `path=null` + `stat` 一律 `no_appdata`。
    virtual bool appDataAvailable() const = 0;
    virtual std::string appDataPath() const = 0;  // 空串 = null（不可用）
    virtual lift::plugin::AppDataStat appDataStat(const std::string& rel) = 0;

    // ---- Config（同步宿主 I/O；**`RunCtx` 与 `RenderCtx` 都有** `ctx.config`）----
    // ★ 形状 = `{ available, domain(id) }`；域对象 =
    //   `{ get(key, fallback?), has(key), set(key, value), delete(key), all() }`。
    // ★ 配置文件 = `<--appdata>/plugin-config.json`（appdata 根；⛔ 不在 `SAVE/`）。
    //   `set` / `delete` 只改内存 + 标脏，宿主在**每个 Run 回调末**与**每帧 render 之后**各 flush 一次。
    //   nullptr = 没接桥 ⇒ RunCtx 里调 `domain()` 是 TypeError（⛔ 不是静默 no-op）。
    virtual lift::plugin::ConfigBridge* config() { return nullptr; }

    // ---- 观测 / 副作用（HostAction 的既有桥）----
    // 一行一条、有界、去重（全局 `log`；合同 §4.10）。⛔ 不每读点刷屏（实现方自己限量）。
    virtual void log(const char* level, const std::string& message) = 0;
    // `ctx.profile.begin/end`：复用宿主既有的 `ui::UiHostOps`（D-C2-4）；false ⇒ `*err` 是原因。
    virtual bool profileBegin(const std::string& label, std::string* err) = 0;
    virtual bool profileEnd(std::string* err) = 0;
    /**
     * ★ S2 复审 I3：**不打印**的报告版本（`RenderCtx.profile.end()` 的回传路）。
     * `true` ⇒ `*text` = 报告全文（宿主的窗口已关）；`false` ⇒ `*err` = 原因（`text` 留空）。
     */
    virtual bool profileEndText(std::string* text, std::string* err) = 0;

    // ---- 归因（三个出口各有各的语义；⛔ 不要合并）----
    // `fail` = 当前 Run 失败（点名 name / reason）；
    // `finish` = 结束 Run + 记录 exit code（**不改进程 exit**；R3）；
    // `quit` = 请求宿主停机（`stop_requested` + detail 归因；D-C2-9）。
    virtual void fail(const std::string& name, const std::string& reason) = 0;
    virtual void finish(int exit_code, const std::string& reason) = 0;
    virtual void quit(const std::string& reason) = 0;
};

// =====================================================================================
// `PluginScript`：一个**已装载**的插件（ES module 的 default 导出）
//
// ★ 两个可选成员：`render` / `start`（`start(ctx): void`）。
//   `render` 可单独存在；`start` 可单独存在（v2 用 `ctx.at` / `once` / `task` 驱动，
//   `start` 只做注册）。两个都缺席 ⇒ 装载失败。
//   `render` 的面 = 基类 `UiScript`：`render(builder, readings, err)` 每次 render 调 JS
//   `render(RenderCtx)`；没有 `render` 的插件返回 true + 空 list。
// ★ `start` 必须**同步**返回 `undefined`（返回 Promise / 其它值 ⇒ 明确错误）：Run 的存活与收口
//   完全由 `hasPendingWork()` / 终态账驱动，不再有 armed 集合。
// =====================================================================================
class PluginScript : public UiScript {
public:
    ~PluginScript() override = default;

    virtual bool hasRender() const = 0;
    // 有 `start`（`render` 可单独存在）。
    virtual bool hasStart() const = 0;

    // 调 JS `start(ctx)`。false ⇒ `*err` 非空（Run fail）。
    virtual bool start(PluginCallHost& host, const lift::plugin::Budget& budget, std::string* err) = 0;

    // 上一次**入口**是否被预算（deadline / op_limit）打断 —— 是 false 的原因之一。
    bool overran() const override = 0;

    // ★ Run 存活 = **没有待推的异步工作**。三个来源（语言无关的"有没有"）：
    //   ① `ctx.task` 登记且未 settle 的协程；② 未命中 / 未超时的 `ctx.once` / `waitAny` / `delay`；
    //   ③ `ctx.at` 的同步订阅。
    virtual bool hasPendingWork() const = 0;

    // ★ **非 tick 入口的 async 泵**（没有独立 timer）。语义：① `ctx.once` 的 deadline 到期 ⇒
    //   reject（先不 fail）；② 用同一 CPU 预算窗 drain 到静止；③ drain 之后检查 task settle；
    //   ④ 若有 timeout ⇒ **现在** fail Run（让 `catch` 先有机会 log）。
    //   调用点只有两个（⛔ 不要新增第三个泵概念）：
    //   · `PluginHost::onReadPoint`：`runClosed()` 判断**之前**；
    //   · `PluginScript::render` 的开头（render 边界；`call_host` 用 persistent 那份）。
    virtual void pump(PluginCallHost& host, const lift::plugin::Budget& budget) = 0;

    /**
     * ★ **Run 收口**的脚本侧钩子（`PluginHost::releaseRunHooks` 是唯一调用点）。
     * 语义 = 丢掉**不再推进**的异步工作：目前只有 `ctx.delay` 的 resolve 表与 `waitAny` 的组账。
     * ⛔ 不清 `task` / `once`：那两样由 instr hook 的 owner 收口与 `Impl::shutdown` 兜底
     * （`once` 被撤时必须 reject —— 正在 await 它的 task 才能 settle；那条语义不能动）。
     * ★ 幂等；实现方自己判 `ctx == nullptr`。
     */
    virtual void onRunClosed() = 0;

    void shutdown() override = 0;

    // 全局 `log` / `ctx.config` 的宿主来路（**非拥有**）。
    // ★ 由 `PluginHost` 在 `init` 时设（生命周期 = host > script）；nullptr = 没接线 ⇒ 相关口
    //   在 JS 里一律 TypeError（⛔ 不静默 no-op）。
    virtual void setCallHost(PluginCallHost* host) = 0;

    // `ctx.draw.text` / `ctx.draw.measure` 的**宿主字体**（非拥有；C2b-2）。
    // ★ `nullptr` = 没接（测试夹具 / headless）⇒ `measure` / `text` 抛 TypeError（⛔ 不返回 0 尺寸）。
    virtual void setFont(lift::FontEngine* font) = 0;
};

// 装载参数（`origin` = `--plugin <path>` 原样：错误定位与 module 解析根都用它）。
struct PluginLoadOptions {
    std::string origin;
    const PluginParams* params = nullptr;
    // `ctx.draw.measure` 用（度量走宿主字体，`ui/raster.h` 的 `measureText`）；
    // noexcept：nullptr ⇒ `measure` 抛 TypeError（⛔ 不返回 0 尺寸冒充可用）。
    FontEngine* font = nullptr;
};

// 装载一个插件（实现 = `ui/script_quickjs.cpp` 的唯一 QJS TU）。
//   · 失败 ⇒ 返回 nullptr 且 `*err` 非空（形状 `<origin>: <message>\n<stack>`）；
//   · **只读 ES module 的 default 导出**（⛔ 不读 `globalThis` / 命名导出）；
//   · `render` / `start` 都可选，**至少一个**；类型不是函数 / 出现已删除的 v1 成员
//     （`onTrigger` / 大写 `initialState`）⇒ 错误（带 `file:line`）。
//   · `render` 的 JS 契约 = `render(RenderCtx)`（C2b）；装载后由 `UiScript::render` 驱动。
//   · `setCallHost` 不在装载参数里：宿主在 `init` 之后自己设（`PluginHost::init` 就会）。
std::unique_ptr<PluginScript> makeQuickJsPlugin(const std::string& source, const PluginLoadOptions& opts,
                                                std::string* err);

}  // namespace lift::ui
