// emulator-lift · **`PluginHost`** —— QJS 单插件的宿主适配器（C2a）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.1（分层：`src/plugin/` 只做适配）、
//       §4.5（`Context` 的操作类）、§4.6（JS 契约）、§5 C2（判据）、§6 行 2 / 5 / 9 / 11 / 26 / 30。
// 父级裁决：`.tmp/plugin-runtime/c2-decisions.md`
//       D-C2-1（读点观察者 + `onReadPoint` 的顺序）、D-C2-9（`fail`/`finish`/`quit` 三个出口）、
//       D-C2-10（`start` 之前补一次 `sampleNow`）。
// 测绘：`.tmp/plugin-runtime/c2-survey.md` §3.2（装配点）/ §3.3（生命周期顺序）/ §6-⑧（trigger 对象表）。
//
// ★ **本类同时是两个接口的实现**：
//   · `TriggerHandler`：JS 回调 → armed 集合（`TriggerRunner` 是唯一 pump，⛔ 本类不 poll）；
//   · `ReadPointObserver`：**唯一入口**（`input.h` 四个投递口 → 本类 → `IntentExecutor::atPoint`）。
//   执行循环文件（`aot_run.cpp` / `machine.cpp`）里⛔ **不出现**任何 pump 词
//   （守卫 `tools/test/input-plan-shape.test.mjs` 的 `LOOP_FORBIDDEN`）。
//
// ★ **本类不 include `quickjs.h`、不调 `input::` / `writeGlobal`**（合同 §6 行 11 / 22）：
//   语言面在 `ui/plugin.h`（接口），JS 面在 `ui/script_quickjs.cpp`（唯一 TU），
//   guest 写只经 `EditSink`（`GuestEditSink`）、guest 输入只经 `IntentQueue`。
//
// ★ **装配顺序**（与测绘 §3.3 一致；`aot_main` 照此接线）：
//   `initFromFile`（编译 + 沙箱，⛔ 不调 start）→ `attachMemory`（Edit/Read）→
//   `windows->setReadPointObserver(&host)` → `run_opts.stop_requested = quitFlag()`。
//   第一个读点上：`executor_.atPoint`（② 已跑，无 armed 时早退）→ `sampleNow`（D-C2-10）→
//   `runner_.begin`（= JS `start`）。⇒ `start` 里的 Input 落 `staged_`，**下一读点**才提升。
//
// ★ **`start` 的时机与"第一个读点只采样、不 poll"**：`begin` 发生在 `atPoint` 返回**之后** ——
//   executor 一行不改（测绘 §3.3 的第三行）。代价：第一个读点不 poll；从第二个读点起正常 poll。
//   用例必须断言"`start` 之后同一 position 区间仍能命中"（⛔ 不靠"区间够长"）。
//
// ★ **`render`（C2b）不属于本类的循环**：`script()` 返回**同一个** `PluginScript`（它实现了
//   `UiScript`），窗口叠层路径每帧调 `render(builder, readings)`（C2b-2 的 `adopt` 才接线）。
//   本类只负责两件事：`init` 时把 `this` 设进 `PluginScript::setCallHost`（render 的
//   `ctx.read` / `ctx.log` 的来路），以及让 `readWired()` 不绑 `in_callback_`（见下）。
//   ⇒ ⛔ 本类不 include 任何 UI 实现头、不持有 `UiBuilder` / `FontEngine`。
//
// ★ **三个出口各有各的语义**（⛔ 不要合并）：
//   · `ctx.fail(name, reason)` → 本次 Run 失败（清 armed + readout `run=failed`）；
//   · `ctx.finish(code, reason)` → 结束本次 Run + 记录 exit（**不改进程 exit**；R3）；
//   · `ctx.quit(reason)` → `stop_requested`（宿主在**下一个块边界**以 `Stopped` 收口，
//     `detail` 由 `AotRunOptions::stop_detail` 归因为 `plugin: quit`；D-C2-9）。
//   ★ 后两者**都不**直接改 guest：一个只结束回调链，一个只请求宿主停机。
//
// ★ **回调失败 = 返回 `[]`（done）+ 本类记错**（测绘 §6-⑤ 的第二种做法）：`TriggerRunner::failRun`
//   会**覆盖** `*err`（= "trigger without state" 之类），把 JS 的真实错误吞掉 ⇒ 这里不走那条路：
//   我们清 armed（返回空集合 ⇒ `install` 置 done）、自己记 `failed_` / `fail_reason_`，并打一行
//   stderr。`runner_.failed()` 因此为 false —— **判据在本类**（readout / `failed()`）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "runtime/context.h"
#include "runtime/edit_sink.h"
#include "plugin/plugin_appdata.h"
#include "plugin/plugin_config.h"
#include "runtime/intent_executor.h"
#include "runtime/intent_queue.h"
#include "runtime/read_point.h"
#include "runtime/trigger.h"
#include "runtime/trigger_runner.h"
#include "ui/plugin.h"

namespace lift {

class GuestMemory;
class WindowManager;

namespace ui {

// `ctx.profile.*` 的既有桥（定义在 `ui/script.h`）。本头只存**指针** ⇒ ⛔ 不需要完整类型
// （否则每个 include 本头的 TU 都被拖进 UI 脚本层；`.cpp` 里才 include `ui/script.h`）。
struct UiHostOps;

}  // namespace ui

}  // namespace lift

namespace lift::plugin {

class GuestEngineRead;

class PluginHost final : public TriggerHandler,
                         public ReadPointObserver,
                         public ui::PluginCallHost,
                         public ConfigBridge {
public:
    using Params = lift::ui::PluginParams;

    // C2a 单 owner（C5 才多 owner）。⛔ 不要从 CLI / 参数里取它：owner 是 Run 级的账。
    static constexpr std::uint32_t kOwner = 1;
    // `ctx.log` 的有界上限（一行一条；到顶后只再打一条"已截断"）。
    static constexpr std::uint64_t kLogLineLimit = 100;

    // `source` = 生产用 `PluginReadings`（`src/plugin/readings.h`）；测试夹具可以注入替身。
    explicit PluginHost(ReadingsSource& source);
    // ★ 显式声明 + 在 .cpp 里 `= default`：`unique_ptr<GuestEngineRead>` 的析构要求**完整类型**，
    //   而本头只前向声明它（否则每个 include 本头的 TU 都得知道 `guest_read.h`）。
    ~PluginHost() override;

    // ---- 装配（⛔ 不调 start）----
    // 编译 + 沙箱 + 读 default 导出。`origin` = `--plugin <path>` 原样（错误定位 / module 解析根）。
    bool init(const std::string& source, const std::string& origin, const Params& params,
              const ui::UiHostOps* host_ops, std::string* err);
    // 读文件（UTF-8 宿主路径）+ `init`；失败点名路径。
    bool initFromFile(const std::string& path, const Params& params, const ui::UiHostOps* host_ops,
                      std::string* err);
    // guest 内存接线：Edit = `GuestEditSink`（`runtime/edit_sink.h` 的**立即**写），
    // Read = `GuestEngineRead`（`src/plugin/guest_read.h`）。两者都不拥有 `mem`。
    void attachMemory(lift::GuestMemory& mem);

    // ★ `ctx.appdata` 的根（**在 `init` / `initFromFile` 之前**由 `aot_main` 调；测试夹具可以不调
    //   ⇒ `available=false`，与生产路径的差别是显式的）。
    //   `appdata_dir` = `--appdata` 的**原字符串**（绝对化在这里做；`Vfs::setAppDataDir` 不做）；
    //   `install_dir` = `--game-dir`（只用来自检"appdata 落在只读安装树里"）。
    //   返回 false ⇒ `*err` 非空、`ctx.appdata.available=false`（调用方 **warn + 继续**：插件能力
    //   缺失不是启动失败；⛔ 不要把它变成 exit 2）。
    bool attachAppData(const std::string& appdata_dir, const std::string& install_dir,
                       std::string* err);

    // ★ `ctx.config`（2026-11-25）：读 `<--appdata>/plugin-config.json`（域式 JSON 配置）。
    //   **必须在 `attachAppData` 之后、`init` / `initFromFile` 之前**调（复用 appdata 的根；
    //   没有 `--appdata` ⇒ `available=false`，正常）。
    //   返回 false ⇒ `*err` 非空 = **配置文件坏**（坏 JSON / 顶层形状 / version / 域值 / 太大）⇒
    //   调用方 **exit 2**（⛔ 不要像 appdata 那样降级）；返回 true 时 `available=false` 只是"没有
    //   appdata"，`config.domain()` 会在 JS 侧抛 `Error("no_appdata")`。
    bool attachConfig(std::string* err);

    // ★ C2b-2：`ctx.draw.text` / `ctx.draw.measure` 的生产字体来源（**非拥有**；转发给
    //   `PluginScript::setFont`）。调用时机 = 窗口装配**得知模块有 `render` 之后**（装载时还
    //   不知道 ⇒ 不走 `PluginLoadOptions::font`）。`nullptr` = 没接（headless / 测试夹具）⇒
    //   那两个口抛 TypeError（⛔ 不返回 0 尺寸冒充可用）。必须在 `init` **之后**调。
    void setFont(lift::FontEngine* font);

    // ---- TriggerHandler（`TriggerRunner` 是唯一 pump；本类只被它调）----
    TriggerSet start(Context& ctx, std::string* err) override;
    TriggerSet onTrigger(Context& ctx, const TriggerEvent& ev, std::string* err) override;

    // ---- ReadPointObserver（C2a 的唯一入口）----
    void onReadPoint(WindowManager& wm, InputPoint point) override;

    // ---- `ui::PluginCallHost`（`ctx.*` 的宿主侧；形状见 `ui/plugin.h`）----
    // ★ 三条**写类**装配线（Edit / Input / Trigger 工厂）各判各的，且**必须正在回调里**
    //   （`in_callback_`）：回调外调用它们是实现错误 ⇒ JS 侧一律 TypeError（⛔ 不静默 no-op）。
    // ★ **`read` 不绑 `in_callback_`**（C2b）：`ctx.read.global.get` 在 `RenderCtx` 里也要能用
    //   （d.ts 的 `ReadApi` 同时出现在 RunCtx 与 RenderCtx）；EngineRead 本是**只读**、无暂停
    //   （合同 §4.5），把它绑在"正在 Run 回调"上会让 render 的读数面静默缺失。
    //   ⛔ 不要顺手把 `edit`/`input`/`trigger` 也放开 —— 那是"render 不做 guest 写"（§4.4c）的落点。
    bool editWired() const override { return in_callback_ && edit_ != nullptr; }
    bool inputWired() const override { return in_callback_ && ctx_input() != nullptr; }
    bool readWired() const override { return read_ != nullptr; }
    bool triggerWired() const override { return in_callback_ && ctx_trigger() != nullptr; }
    // ★ S2c-2：`Tick.edit` / `Tick.input` 的**装配**门（⛔ 不含 `in_callback_`）：tick 入口
    //   `in_callback_` 恒 false，而写面不是"把 in_callback_ 打开"。两条实现都只看**接线**：
    //   · Edit = `GuestEditSink`（`attachMemory` 建）；
    //   · Input = `IntentExecutor` 的 `Context::input`（构造时接的 `IntentQueue&`；回调外 `ctx_` 是
    //     nullptr，所以⛔ 不能复用 `ctx_input()`）。
    bool editAttached() const override { return edit_ != nullptr; }
    bool inputAttached() const override { return executor_.context().input != nullptr; }
    // ★ C2b 复审 F2：render 期 `call_host` **非空**（read/log 要用），但 `fail`/`finish`/`quit`/
    //   `profile.*` 是 **Run 回调**的出口 ⇒ 那 5 个 trampoline 靠这一位拒绝"存下来的 RunCtx
    //   在 render 里用"。⛔ 不要把它也放松成恒 true（那等于成员表只是画出来的门面）。
    bool inCallback() const override { return in_callback_; }

    bool editGlobalSet(lift::GlobalKind kind, std::uint32_t id, const lift::GlobalValue& value,
                       const char** code) override;
    // ---- 脚本指令位置钩子（`ctx.hook`；唯一实现 = `runtime/instr_hook.cpp`）----
    std::uint32_t instrHookAdd(const std::string& script, std::uint32_t pos, const char** err) override;
    // ★ S2c：`ctx.at(where, handler)` 的注册口（owner = `run_serial_`，与 `instrHookAdd` 同一份）。
    std::uint32_t instrHookAddWhere(const lift::InstrHookWhere& where, lift::InstrHookKind kind,
                                    const char** err) override;
    bool instrHookSetPatch(std::uint32_t handle, bool enabled, lift::InstrPatchKind kind, std::uint32_t index,
                           std::uint32_t value, std::uint32_t max_hits, const char** err) override;
    // ★ S2c-2：`Tick.patch` 的 one-shot 覆盖（只作用于本拍命中的条目；静默 —— 每拍都 log 会刷屏）。
    bool instrHookSetTickPatch(std::uint32_t handle, lift::InstrPatchKind kind, std::uint32_t index,
                               std::uint32_t value, const char** err) override;
    bool instrHookSetCapture(std::uint32_t handle, const std::vector<std::uint32_t>& ints,
                             const std::vector<std::uint32_t>& ptrs) override;
    bool instrHookStats(std::uint32_t handle, lift::InstrHookStatus* out) override;
    bool instrHookRemove(std::uint32_t handle) override;
    void instrHookClear() override;
    std::vector<lift::InstrHookStatus> instrHookList() override;
    lift::plugin::Receipt inputClick(int x, int y, lift::MouseButton button) override;
    lift::plugin::Receipt inputMove(int x, int y) override;
    lift::plugin::Receipt inputDown(lift::MouseButton button) override;
    lift::plugin::Receipt inputUp(lift::MouseButton button) override;
    ui::PluginInputState inputStatus(std::uint64_t id) override;
    bool readGlobal(lift::GlobalKind kind, std::uint32_t id, lift::GlobalValue* out,
                    const char** code) override;
    // ★ S2c：`Tick.local.*` 的转发（唯一实现 = `GuestEngineRead::readLocal`；本头只前向声明它）。
    bool readLocal(const lift::InstrLocalView& view, lift::InstrLocalKind kind, std::uint32_t index,
                   std::uint32_t* out) override;
    // ---- AppData（同步只读宿主 I/O；只 RunCtx）----
    bool appDataAvailable() const override { return appdata_.available(); }
    std::string appDataPath() const override { return appdata_.path(); }
    lift::plugin::AppDataStat appDataStat(const std::string& rel) override {
        return appdata_.stat(rel);
    }
    // ---- Config（同步宿主 I/O；**RunCtx 与 RenderCtx 两面都有**）----
    // ★ 实现面 = `lift::plugin::ConfigBridge`（本类**同时是**那个桥：`config()` 返回 `this`）。
    //   薄适配 `PluginConfig`：值 / 上限 / `HostPause` 都在 `script_quickjs.cpp` 的 trampoline 里，
    //   本类只转发（⛔ 不在这里复制第二套校验）。
    bool available() const override { return config_.available(); }
    bool checkDomain(const std::string& id, std::string* why) const override {
        return PluginConfig::checkDomain(id, why);
    }
    bool get(const std::string& domain, const std::string& key, lift::plugin::ConfigValue* out,
             bool* found, std::string* why) override {
        return config_.get(domain, key, out, found, why);
    }
    bool has(const std::string& domain, const std::string& key, bool* found,
             std::string* why) override {
        return config_.has(domain, key, found, why);
    }
    bool set(const std::string& domain, const std::string& key,
             const lift::plugin::ConfigValue& value, lift::plugin::ConfigReceipt* receipt,
             std::string* why) override {
        return config_.set(domain, key, value, receipt, why);
    }
    bool erase(const std::string& domain, const std::string& key, bool* removed,
               std::string* why) override {
        return config_.erase(domain, key, removed, why);
    }
    lift::plugin::ConfigValue all(const std::string& domain) override { return config_.all(domain); }
    void flushIfDirty(std::string* warn) override { config_.flushIfDirty(warn); }
    // `PluginCallHost::config()`：RunCtx 的 `ctx.config` 来路（与 `UiHostOps::config` 指向**同一个**
    //   桥，只是两个面各有一条取用路径；两条渲染面 = 经典 `host.config` + `RenderCtx.config`）。
    lift::plugin::ConfigBridge* config() override { return this; }
    lift::plugin::Trigger makeAfterScript(const std::string& name, const std::string& script,
                                         lift::plugin::TriggerMode mode, std::uint32_t max_fires,
                                         std::uint64_t timeout_ms) override;
    lift::plugin::Trigger makeAfterPosition(const std::string& name, const std::string& script,
                                           std::uint32_t lo, std::uint32_t hi,
                                           lift::plugin::TriggerMode mode, std::uint32_t max_fires,
                                           std::uint64_t timeout_ms) override;
    lift::plugin::Trigger makeAfterDelay(const std::string& name, std::uint64_t delay_ms,
                                        lift::plugin::TriggerMode mode) override;
    lift::plugin::Trigger makeAtReadPoint(const std::string& name, lift::InputPoint filter,
                                         std::uint32_t max_fires, lift::plugin::TriggerMode mode,
                                         std::uint64_t timeout_ms) override;
    const lift::plugin::TriggerReadings* now() const override;
    void log(const char* level, const std::string& message) override;
    bool profileBegin(const std::string& label, std::string* err) override;
    bool profileEnd(std::string* err) override;
    bool profileEndText(std::string* text, std::string* err) override;
    void fail(const std::string& name, const std::string& reason) override;
    void finish(int exit_code, const std::string& reason) override;
    void quit(const std::string& reason) override;

    // ---- 读数 / 收口（readout 与用例的唯一入口；⛔ 不是性能判据）----
    bool begun() const { return begun_; }
    bool done() const { return runner_.done(); }
    bool failed() const override { return failed_; }
    // ★ S2b-1（复审 S2-1）：Run 终态 = failed / finished / quit / runner failed。
    //   泵 / task settle 用它当"Run 已收口 ⇒ 静默丢弃"的判据；⛔ 不含 `runner_.done()`。
    bool runTerminal() const override {
        return failed_ || finished_ || quit_requested_ || runner_.failed();
    }
    const std::string& failName() const { return fail_name_; }
    const std::string& failReason() const { return fail_reason_; }
    bool finished() const { return finished_; }
    int finishCode() const { return finish_code_; }
    const std::string& finishReason() const { return finish_reason_; }
    bool quitRequested() const { return quit_requested_; }
    // 给 `AotRunOptions::stop_requested` 的**非拥有**指针（生命周期 = 本对象）。
    const bool* quitFlag() const { return &quit_requested_; }
    std::uint64_t callbacks() const { return callbacks_; }
    std::uint64_t fired() const { return fired_; }
    std::uint64_t timeouts() const { return timeouts_; }
    std::uint64_t logLines() const { return log_lines_; }
    // ★ S2c-1 复审 F7（S3）：**最后一条已打出**日志的原文（测试 / readout 判"消息内容"用）。
    //   ⛔ 不改日志行为：`log()` 照旧 dedup / 上限 / `oneLine()` 打印；这里只是把同一条消息存一份。
    //   为什么需要：`logLines()` 只数行数 ⇒ `plugin/at-handler-error` 只能判"恰好 +1"，判不了
    //   消息里有没有点名的 `at #N` / `hit #M`（plan 的判据）。
    const std::string& lastLogLine() const { return last_line_; }

    TriggerRunner& runner() { return runner_; }
    const TriggerRunner& runner() const { return runner_; }
    TriggerFactory& factory() { return factory_; }
    IntentQueue& queue() { return queue_; }
    IntentExecutor& executor() { return executor_; }
    // ★ 非 const 那一份给 **render** 用（C2b）：`PluginScript` 也是 `UiScript`，
    //   叠层路径要在**同一个实例**上调 `render(builder, readings)`（⛔ 不是第二个 QJS context）。
    ui::PluginScript* script() { return script_.get(); }
    const ui::PluginScript* script() const { return script_.get(); }

    // 释放本 owner 的 armed + 未完成的输入状态（⛔ 不做 `releaseProgram`：那是"全抬"）。
    std::size_t releaseOwner(WindowManager& wm);

    // 一次性 readout（stdout；⛔ 不周期；键名固定，CTest 按 `plugin    : ` / `run=` 抓）。
    std::string readout() const;

private:
    IntentQueue* ctx_input() const { return ctx_ != nullptr ? ctx_->input : nullptr; }
    TriggerFactory* ctx_trigger() const { return ctx_ != nullptr ? ctx_->trigger : nullptr; }

    // 一次回调的公共壳（start / onTrigger 共用）：预算 + 归因 + 返回值翻译。
    TriggerSet runCallback(Context& ctx, bool is_start, const TriggerEvent* ev, std::string* err);
    // ★ F3（父级裁决）：`fail` / `quit` 是**终态异常** ⇒ 立刻清掉本 owner 的
    //   staged input / 按住 / armed（在 `executor_.atPoint` 的第 ⑥ 步**之前**）。
    //   `finish` / `return []` **不**释放（正常收口：本读点之后那次 click 仍要落地）。
    void releaseRunInput();
    // ★ S0（缺陷 1）：**Run 的每一条收口路径**都要释放本 Run 的 hook（`instrHookReleaseOwner`,
    //   不是 `instrHookClear`：后者会把别人的也撤掉）。调用点见 `.cpp` 的收口表。
    //   为什么必须是 owner 而不是 clear：hook 表是进程级的，而"本 Run 装的"是唯一正确的范围；
    //   一次 `clear()` 在将来多 Run 时会静默撤掉另一个 Run 的 hook。
    void releaseRunHooks();
    // 本 Run 是否已经收口（失败 / finish / quit / runner failed / runner done）。
    // ★ 读点入口用它兜住"发生在 `onTrigger` **返回之后**"的两类收口：`install([])` 的 done
    //   与 install 拒绝的 failed（那时回调栈已经拿不到控制权）。
    bool runClosed() const;
    // ★ F2：`TriggerRunner` 在 `onTrigger` 返回的集合上 `install` 失败（重复 name / Once 回放 /
    //   非法 kind）时，`failed_` 只有 runner 知道 ⇒ 这里把它同步进本类（readout 才能说 `run=failed`）。
    void syncRunnerFailure();
    // 四条 Input 的公共提交点（一读点一条 / owner / origin 只在这里定）。
    Receipt submitIntent(IntentKind kind, int x, int y, MouseButton button);
    // 失败收口：记错 + 一行 stderr（真源在本类；⛔ 不写 runner 的 `lastError`）。
    void markFailed(const std::string& reason);
    // 一行 stderr（⛔ 不周期、⛔ 不进 stdout）。★ 前缀带**插件名**（`initFromFile` 记下的 stem），
    //   所以它是实例方法（⛔ 不在每条日志现场算路径）。
    void report(const char* what, const std::string& text);

    IntentQueue queue_;
    TriggerFactory factory_;
    TriggerRunner runner_;
    IntentExecutor executor_;
    std::unique_ptr<GuestEditSink> edit_;
    std::unique_ptr<GuestEngineRead> read_;
    std::unique_ptr<ui::PluginScript> script_;
    PluginAppData appdata_;
    PluginConfig config_;

    const ui::UiHostOps* host_ops_ = nullptr;
    Params params_;
    std::string origin_;
    // ★ 日志归属：插件入口文件名的 stem（`scripts/plugins/field-point-visible.js` ⇒
    //   `field-point-visible`；内置 `hud` 预设 ⇒ `hud`）。`initFromFile` 记一次；拿不到路径 ⇒ `plugin`。
    std::string name_ = "plugin";

    // 回调期间的上下文（`ctx.*` 的来路）；回调外恒 nullptr ⇒ `inputWired()` 等为 false。
    Context* ctx_ = nullptr;
    bool in_callback_ = false;
    bool begun_ = false;
    // ★ S0（缺陷 1）：本 Run 的序号（Run 开始 +1）。它只用来给 `ctx.hook.at` 装的条目**归属**
    //   （`instrHookAdd(owner,…)`）⇒ Run 收口时可以只撤自己那批。
    //   v1 一个进程只有一个 Run，但**语义**必须已经是"at() 只属于本 Run"：v2 的多 Run 会直接建在它上面。
    std::uint64_t run_serial_ = 0;

    // ★ F3：本次读点的窗口管理器（`onReadPoint` 存；`onTrigger` 里 `releaseRunInput` 要用它）。
    //   生命周期 = 宿主的 `AotHost::cpu().windows`（活到 run 之后）⇒ 非拥有指针足够；
    //   ⛔ 回调里拿不到它（`Context` 里没有 `WindowManager`，合同 §4.5 711–713）。
    lift::WindowManager* current_wm_ = nullptr;

    bool failed_ = false;
    std::string fail_name_;
    std::string fail_reason_;
    bool finished_ = false;
    int finish_code_ = 0;
    std::string finish_reason_;
    bool quit_requested_ = false;
    std::string quit_reason_;

    std::uint64_t callbacks_ = 0;
    std::uint64_t fired_ = 0;
    std::uint64_t timeouts_ = 0;
    std::uint64_t log_lines_ = 0;
    std::string last_log_;  // 去重（连续相同 ⇒ 不重复打）
    std::string last_line_;  // ★ 最后一条**已打出**的原文（`lastLogLine()`；只读观测，不参与去重）
};

}  // namespace lift::plugin
