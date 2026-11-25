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

    // 现在是否在 **Run 回调**（`start` / `onTrigger`）里；`render` 期间恒 false（C2b 复审 F2）。
    // ★ 为什么需要这一位：render 期间 `read` / `log` 也要能用 ⇒ 宿主（`call_host`）在 render 期
    //   **非空**；于是"只判 `call_host != nullptr`"的出口（`fail` / `finish` / `quit` / `profile.*`）
    //   会从 render 可达 —— 插件在 `onTrigger` 里把 `ctx` 存进模块变量，下一帧 `saved.fail(...)`
    //   就绕过了 `RenderCtx` 的成员表。这 5 个 trampoline 必须判这一位。
    //   ⛔ 这不是安全边界、guest 也不会坏（写面另有 `*Wired()` 把关），而是"成员表不是画出来的门面"。
    virtual bool inCallback() const = 0;

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
// ★ 三个可选成员：`render` / `start` / `onTrigger`（`start` 与 `onTrigger` 成对；`render` 可单独
//   存在）。`render` 的面 = 基类 `UiScript`（C2b 起多继承）：`render(builder, readings, err)` 每次
//   render 调 JS `render(RenderCtx)`；没有 `render` 的插件返回 true + 空 list。
// ★ `start` / `onTrigger` 必须**同步**返回 `Trigger | Trigger[] | null`；返回 Promise / async
//   ⇒ 明确错误（合同 §4.6 805）。返回值翻译成 `std::vector<Trigger>`：
//   空 = done（`null` / `[]` / `undefined`）；重复 `name` 由 `TriggerRunner::install` 判 fail。
// =====================================================================================
class PluginScript : public UiScript {
public:
    using TriggerSet = std::vector<lift::plugin::Trigger>;

    ~PluginScript() override = default;

    virtual bool hasRender() const = 0;
    // 有 `start` + `onTrigger`（两者必须成对；`render` 可单独存在）。
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
//   · `render`/`start`/`onTrigger` 都可选，**至少一个**；有 `start` 就必须有 `onTrigger`（反之亦然）；
//     类型不是函数 ⇒ 错误（带 `file:line`）。
//   · `render` 的 JS 契约 = `render(RenderCtx)`（C2b）；装载后由 `UiScript::render` 驱动。
//   · `setCallHost` 不在装载参数里：宿主在 `init` 之后自己设（`PluginHost::init` 就会）。
std::unique_ptr<PluginScript> makeQuickJsPlugin(const std::string& source, const PluginLoadOptions& opts,
                                                std::string* err);

}  // namespace lift::ui
