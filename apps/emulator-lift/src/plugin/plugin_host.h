// emulator-lift · **`PluginHost`** —— QJS 单插件的宿主适配器
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.1（分层：`src/plugin/` 只做适配）/ §4.5 / §4.6。
// 最终形态（2026-11-28 父级裁决）：v1 兼容层（`TriggerRunner` / 工厂 / armed 集合 /
//   `onTrigger` / `ctx.hook` / `ctx.now` / `ctx.state` / `ctx.ui` / `ctx.log`）**整条删除**。
//
// ★ **本类同时是两个接口的实现**：
//   · `ReadPointObserver`：**唯一入口**（`input.h` 四个投递口 → 本类 → `IntentExecutor::atPoint`）；
//   · `ui::PluginCallHost`：`ctx.*` 的宿主方法。
//   执行循环文件（`aot_run.cpp` / `machine.cpp`）里⛔ **不出现**任何 pump 词
//   （守卫 `tools/test/input-plan-shape.test.mjs` 的 `LOOP_FORBIDDEN`）。
//
// ★ **本类不 include `quickjs.h`、不调 `input::` / `writeGlobal`**（合同 §6 行 11 / 22）：
//   语言面在 `ui/plugin.h`（接口），JS 面在 `ui/script_quickjs.cpp`（唯一 TU），
//   guest 写只经 `EditSink`（`GuestEditSink`）、guest 输入只经 `IntentQueue`。
//
// ★ **装配顺序**：`initFromFile`（编译 + 沙箱，⛔ 不调 start）→ `attachMemory`（Edit/Read）→
//   `windows->setReadPointObserver(&host)` → `run_opts.stop_requested = quitFlag()`。
//   第一个读点上：`executor_.atPoint` → `script_->pump` → `runStart`（= JS `start`）。
//   ⇒ `start` 里的 Input 落 `staged_`，**下一读点**才提升。
//
// ★ **Run 模型（唯一形状）**：
//   · 存活 = `hasPendingWork()`（task / once / waitAny / delay / at 订阅）；
//   · 收口 = `runTerminal() || (begun_ && !hasPendingWork())`；
//   · 预算 = 本类的 `budget_`（缺省 `Budget{}` = 2 ms / 100k；测试可 `setBudget`）。
//
// ★ **三个出口各有各的语义**（⛔ 不要合并）：
//   · `ctx.fail(name, reason)` → 本次 Run 失败（readout `run=failed`）；
//   · `ctx.finish(code, reason)` → 结束本次 Run + 记录 exit（**不改进程 exit**；R3）；
//   · `ctx.quit(reason)` → `stop_requested`（宿主在**下一个块边界**以 `Stopped` 收口，
//     `detail` 由 `AotRunOptions::stop_detail` 归因为 `plugin: quit`）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "plugin/plugin_appdata.h"
#include "plugin/plugin_config.h"
#include "runtime/edit_sink.h"
#include "runtime/intent_executor.h"
#include "runtime/intent_queue.h"
#include "runtime/read_point.h"
#include "ui/plugin.h"

namespace lift {

class GuestMemory;
class WindowManager;

namespace ui {

// `ctx.profile.*` 的既有桥（定义在 `ui/script.h`）。本头只存**指针** ⇒ ⛔ 不需要完整类型。
struct UiHostOps;

}  // namespace ui

}  // namespace lift

namespace lift::plugin {

class GuestEngineRead;

class PluginHost final : public ReadPointObserver,
                         public ui::PluginCallHost,
                         public ConfigBridge {
public:
    using Params = lift::ui::PluginParams;

    // 单 owner（C5 才多 owner）。⛔ 不要从 CLI / 参数里取它：owner 是 Run 级的账。
    static constexpr std::uint32_t kOwner = 1;
    // 全局 `log` 的有界上限（一行一条；到顶后只再打一条"已截断"）。
    static constexpr std::uint64_t kLogLineLimit = 100;

    PluginHost();
    // ★ 显式声明 + 在 .cpp 里 `= default`：`unique_ptr<GuestEngineRead>` 的析构要求**完整类型**，
    //   而本头只前向声明它（否则每个 include 本头的 TU 都得知道 `guest_read.h`）。
    ~PluginHost() override;

    // ---- 装配（⛔ 不调 start）----
    bool init(const std::string& source, const std::string& origin, const Params& params,
              const ui::UiHostOps* host_ops, std::string* err);
    bool initFromFile(const std::string& path, const Params& params, const ui::UiHostOps* host_ops,
                      std::string* err);
    // guest 内存接线：Edit = `GuestEditSink`（**立即**写），Read = `GuestEngineRead`。
    void attachMemory(lift::GuestMemory& mem);

    // `ctx.appdata` 的根（**在 `init` / `initFromFile` 之前**由 `aot_main` 调）。
    bool attachAppData(const std::string& appdata_dir, const std::string& install_dir,
                       std::string* err);

    // `ctx.config`：读 `<--appdata>/plugin-config.json`。
    // 返回 false ⇒ 配置文件坏（调用方 **exit 2**）；true 时 `available=false` 只是"没有 appdata"。
    bool attachConfig(std::string* err);

    // `ctx.draw.text` / `ctx.draw.measure` 的生产字体来源（**非拥有**）。`nullptr` = 没接。
    void setFont(lift::FontEngine* font);

    // ---- ReadPointObserver（唯一入口）----
    void onReadPoint(WindowManager& wm, InputPoint point) override;

    // ---- `ui::PluginCallHost`（`ctx.*` 的宿主侧；形状见 `ui/plugin.h`）----
    // ★ 三条**写类**装配线（Edit / Input / Read）各判各的，Edit/Input 还要求**正在 Run 回调里**
    //   （`in_callback_`）：回调外调用它们是实现错误 ⇒ JS 侧一律 TypeError（⛔ 不静默 no-op）。
    bool editWired() const override { return in_callback_ && edit_ != nullptr; }
    bool inputWired() const override { return in_callback_ && true; }
    bool readWired() const override { return read_ != nullptr; }
    // ★ `Tick.edit` / `Tick.input` 的**装配**门（⛔ 不含 `in_callback_`）。
    bool editAttached() const override { return edit_ != nullptr; }
    bool inputAttached() const override { return true; }
    // ★ render 期 `call_host` **非空**（config / draw 要用），但 `fail`/`finish`/`quit`/
    //   `profile.*` 是 **Run 回调**的出口 ⇒ 那些 trampoline 靠这一位拒绝"存下来的 RunCtx
    //   在 render 里用"。⛔ 不要把它也放松成恒 true。
    bool inCallback() const override { return in_callback_; }

    bool editGlobalSet(lift::GlobalKind kind, std::uint32_t id, const lift::GlobalValue& value,
                       const char** code) override;
    // ---- 脚本指令点订阅（`ctx.at` / `once` / `waitAny` / `Tick.patch` / `ctx.off`）----
    std::uint32_t instrHookAddWhere(const lift::InstrHookWhere& where, lift::InstrHookKind kind,
                                    const char** err) override;
    bool instrHookSetTickPatch(std::uint32_t handle, lift::InstrPatchKind kind, std::uint32_t index,
                               std::uint32_t value, const char** err) override;
    bool instrHookRemove(std::uint32_t handle) override;
    // ★ 测试 / 诊断便利口（⛔ 不在 `PluginCallHost` 接口里；JS 面没有对应成员）。
    std::vector<lift::InstrHookStatus> instrHookList();
    void instrHookClear();
    lift::plugin::Receipt inputClick(int x, int y, lift::MouseButton button) override;
    lift::plugin::Receipt inputMove(int x, int y) override;
    lift::plugin::Receipt inputDown(lift::MouseButton button) override;
    lift::plugin::Receipt inputUp(lift::MouseButton button) override;
    ui::PluginInputState inputStatus(std::uint64_t id) override;
    bool readGlobal(lift::GlobalKind kind, std::uint32_t id, lift::GlobalValue* out,
                    const char** code) override;
    bool readLocal(const lift::InstrLocalView& view, lift::InstrLocalKind kind, std::uint32_t index,
                   std::uint32_t* out) override;
    // ---- AppData（同步只读宿主 I/O；只 RunCtx）----
    bool appDataAvailable() const override { return appdata_.available(); }
    std::string appDataPath() const override { return appdata_.path(); }
    lift::plugin::AppDataStat appDataStat(const std::string& rel) override {
        return appdata_.stat(rel);
    }
    // ---- Config（同步宿主 I/O；**RunCtx 与 RenderCtx 两面都有**）----
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
    lift::plugin::ConfigBridge* config() override { return this; }
    void log(const char* level, const std::string& message) override;
    bool profileBegin(const std::string& label, std::string* err) override;
    bool profileEnd(std::string* err) override;
    bool profileEndText(std::string* text, std::string* err) override;
    void fail(const std::string& name, const std::string& reason) override;
    void finish(int exit_code, const std::string& reason) override;
    void quit(const std::string& reason) override;

    // ---- 读数 / 收口（readout 与用例的唯一入口；⛔ 不是性能判据）----
    bool begun() const { return begun_; }
    // ★ Run 存活 = 有 start 且没有待推工作（终态另算）。
    bool done() const { return begun_ && !hasPendingWork(); }
    bool failed() const override { return failed_; }
    // ★ 终态判据只有一份（failed / finished / quit）。
    bool runTerminal() const override { return failed_ || finished_ || quit_requested_; }
    const std::string& failName() const { return fail_name_; }
    const std::string& failReason() const { return fail_reason_; }
    bool finished() const { return finished_; }
    int finishCode() const { return finish_code_; }
    const std::string& finishReason() const { return finish_reason_; }
    bool quitRequested() const override { return quit_requested_; }

    // ★ F3（v2 延伸）：tick handler / 本入口 drain 里 `ctx.fail` / `ctx.quit` ⇒ 立刻丢已提交的
    //   input（与 Run 回调同一条裁决；`ctx.finish` 不丢）。
    void releaseAbortedInput() override { releaseRunInput(); }
    // 给 `AotRunOptions::stop_requested` 的**非拥有**指针（生命周期 = 本对象）。
    const bool* quitFlag() const { return &quit_requested_; }
    std::uint64_t callbacks() const { return callbacks_; }
    std::uint64_t logLines() const { return log_lines_; }
    const std::string& lastLogLine() const { return last_line_; }
    const std::string& lastLogLevel() const { return last_level_; }

    // ★ 预算（D5：2 ms / 100k）。测试可放宽（`Fixture` 构造器设 500 ms / 1e7）。
    void setBudget(const Budget& budget) { budget_ = budget; }
    Budget budget() const { return budget_; }

    IntentQueue& queue() { return queue_; }
    IntentExecutor& executor() { return executor_; }
    ui::PluginScript* script() { return script_.get(); }
    const ui::PluginScript* script() const { return script_.get(); }

    // 释放本 owner 的未完成的输入状态（⛔ 不做 `releaseProgram`：那是"全抬"）。
    std::size_t releaseOwner(WindowManager& wm);

    // 一次性 readout（stdout；⛔ 不周期；键名固定，CTest 按 `plugin    : ` / `run=` 抓）。
    std::string readout() const;

private:
    // 一次 `start` 的公共壳：预算 + 归因。
    void runStart();
    bool hasPendingWork() const;
    // ★ 终态异常（`fail` / `quit`）⇒ 立刻清掉本 owner 的 staged input / 按住
    //   （在 `executor_.atPoint` 的 drain **之前**）。`finish` / 正常 done **不**释放。
    void releaseRunInput();
    // ★ Run 的每一条收口路径都要释放本 Run 的 hook（`instrHookReleaseOwner`）。
    void releaseRunHooks();
    // 本 Run 是否已经收口（终态 || (已开始 && 没有待推工作)）。
    bool runClosed() const;
    // 四条 Input 的公共提交点（一读点一条 / owner / origin 只在这里定）。
    Receipt submitIntent(IntentKind kind, int x, int y, MouseButton button);
    // 失败收口：记错 + 一行 stderr。
    void markFailed(const std::string& reason);
    // 一行 stderr（⛔ 不周期、⛔ 不进 stdout）。前缀带**插件名**。
    void report(const char* what, const std::string& text);

    IntentQueue queue_;
    IntentExecutor executor_;
    std::unique_ptr<GuestEditSink> edit_;
    std::unique_ptr<GuestEngineRead> read_;
    std::unique_ptr<ui::PluginScript> script_;
    PluginAppData appdata_;
    PluginConfig config_;
    Budget budget_{};

    const ui::UiHostOps* host_ops_ = nullptr;
    Params params_;
    std::string origin_;
    // ★ 日志归属：插件入口文件名的 stem（拿不到路径 ⇒ `plugin`）。
    std::string name_ = "plugin";

    bool in_callback_ = false;
    bool begun_ = false;
    // ★ 本 Run 的序号（Run 开始 +1）。它只给 `ctx.at` / `once` 装的条目**归属**
    //   （`instrHookAddWhere(owner,…)`）⇒ Run 收口时可以只撤自己那批。
    std::uint64_t run_serial_ = 0;

    // 本次读点的窗口管理器（`onReadPoint` 存；`releaseRunInput` 要用它）。
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
    std::uint64_t log_lines_ = 0;
    std::string last_log_;   // 去重（连续相同 ⇒ 不重复打）
    std::string last_line_;  // 最后一条**已打出**的原文（只读观测，不参与去重）
    std::string last_level_; // 最后一条**已打出**的级别（同上）
};

}  // namespace lift::plugin
