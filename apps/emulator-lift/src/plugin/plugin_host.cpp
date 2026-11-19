// emulator-lift · `PluginHost` 的实现 —— 口径见头注（这里只写"这一段为什么这样写"）
#include "plugin/plugin_host.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

#include "plugin/guest_read.h"     // GuestEngineRead（ctx.read.global.get 的生产实现）
#include "runtime/budget.h"        // Budget（回调预算的唯一真源）
#include "runtime/host_window.h"   // WindowManager（onReadPoint / releaseOwner）
#include "runtime/memory.h"        // GuestMemory（attachMemory 的形参）
#include "runtime/vfs.h"           // openHostFileUtf8（UTF-8 宿主路径）
#include "ui/script.h"             // UiHostOps（ctx.profile.* 的既有桥）

namespace lift::plugin {
namespace {

// 读一个宿主文件（UTF-8 路径；失败 ⇒ false）。⛔ 不用 `std::ifstream`：Windows 上它按 ANSI
// 代码页解释路径，而本仓路径一律 UTF-8（游戏目录本身是日文名）。
bool readHostSource(const std::string& path, std::string* out) {
    std::FILE* f = lift::openHostFileUtf8(path, "rb");
    if (f == nullptr) return false;
    char buf[8192];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    return !bad;
}

// readout 是**一行**：理由文本里的换行必须折成空格（否则 `run=` 之后的字段会跑到下一行）。
std::string oneLine(std::string text) {
    for (char& c : text) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return text;
}

std::string paramsText(const std::map<std::string, std::string>& params) {
    if (params.empty()) return "-";
    std::string out;
    for (const auto& kv : params) {
        if (!out.empty()) out += ',';
        out += kv.first;
        out += '=';
        out += kv.second;
    }
    return out;
}

}  // namespace

PluginHost::PluginHost(ReadingsSource& source) : executor_(queue_, source) {
    factory_.setOwner(kOwner);
    runner_.setOwner(kOwner);
    // ★ **必须**把 runner 交给 executor：本类是 runner 的 handler，但**每个读点的 poll 是 executor
    //   做的事**（`atPoint` ③ → `runner_.atPoint`）。漏了这一行 ⇒ `atPoint` 的 `polling` 恒 false
    //   ⇒ trigger 永远不 fire、只有 `start` 会跑（而 `install` 仍然生效 ⇒ 症状是"armed 有、不触发"，
    //   静默、只有用例能看出来）。与 `tests/trigger_runner.cpp` 的 `exec.setTriggerRunner(&runner)` 同一条。
    executor_.setTriggerRunner(&runner_);
    // ★ `ctx.trigger` 是**工厂**（⛔ 不是 runner）：回调造得出新对象、拿不到唯一 pump
    //   （合同 §4.5 711–713 / `context.h` 头注 ②）。
    executor_.context().trigger = &factory_;
}

// `unique_ptr<GuestEngineRead>` 的析构要完整类型 ⇒ 定义放这里（见头注）。
PluginHost::~PluginHost() = default;

bool PluginHost::init(const std::string& source, const std::string& origin, const Params& params,
                      const ui::UiHostOps* host_ops, std::string* err) {
    origin_ = origin;
    params_ = params;
    host_ops_ = host_ops;
    lift::ui::PluginLoadOptions opts;
    opts.origin = origin;
    opts.params = &params_;
    std::string load_err;
    script_ = lift::ui::makeQuickJsPlugin(source, opts, &load_err);
    if (script_ == nullptr) {
        if (err != nullptr) *err = load_err.empty() ? (origin + ": 插件装载失败") : load_err;
        return false;
    }
    return true;
}

bool PluginHost::initFromFile(const std::string& path, const Params& params, const ui::UiHostOps* host_ops,
                              std::string* err) {
    std::string source;
    if (!readHostSource(path, &source)) {
        if (err != nullptr) *err = path + ": 读不到插件文件";
        return false;
    }
    return init(source, path, params, host_ops, err);
}

void PluginHost::attachMemory(lift::GuestMemory& mem) {
    // ★ 两条都是**非拥有**引用：`mem` 归宿主（`AotHost::mem()`；这里只在 init/prepare 之后接）。
    edit_ = std::make_unique<GuestEditSink>(mem);        // Edit：立即写（⛔ 不排队）
    read_ = std::make_unique<GuestEngineRead>(mem);      // EngineRead：同步只读（⛔ 不暂停）
    executor_.setBus(edit_.get(), read_.get());
}

// =====================================================================================
// ReadPointObserver（C2a 的唯一入口）
// =====================================================================================
void PluginHost::onReadPoint(WindowManager& wm, InputPoint point) {
    if (script_ == nullptr) return;
    // ① 七步（② `beginReadPoint` 已跑；没有 armed 时早退 —— 这是 C1 的既有语义，⛔ 不改）。
    executor_.atPoint(wm, point);
    if (begun_ || failed_ || quit_requested_) return;

    // ② 首点：只采样 + `begin`（D-C2-10）。`sampleNow` 让 `start` 里的 `ctx.now` 有值；
    //    ⛔ 它不加读点计数 / 不碰队列（那两笔账在 `atPoint` 里，已经记过）。
    begun_ = true;
    executor_.sampleNow(point);
    std::string err;
    runner_.begin(*this, executor_.context(), &err);
    if (!err.empty()) {
        // 两种来路：① 本类的 `start` 记了 JS 的错（`runner_.begin` 只清 err、不看清）；
        //           ② runner 自己判的（重复 name / 无 state / 预算）⇒ 由 `lastError()` 点名。
        const std::string why = runner_.failed() && !runner_.lastError().empty() ? runner_.lastError() : err;
        if (!failed_) markFailed(why);
        report("run failed", why);
    }
}

// =====================================================================================
// TriggerHandler（start / onTrigger）
// =====================================================================================
PluginHost::TriggerSet PluginHost::start(Context& ctx, std::string* err) {
    // ★ UI-only 插件（只有 `render`）**没有 Run**：`TriggerRunner::begin` 仍会被调一次（首读点），
    //   这里必须**干净地**返回空集合（done），⛔ 不能当成"缺 start 回调"的失败。
    if (script_ != nullptr && !script_->hasRun()) return {};
    return runCallback(ctx, true, nullptr, err);
}

PluginHost::TriggerSet PluginHost::onTrigger(Context& ctx, const TriggerEvent& ev, std::string* err) {
    if (script_ != nullptr && !script_->hasRun()) return {};
    if (ev.what == TriggerEvent::What::Fired) {
        ++fired_;
    } else {
        ++timeouts_;
    }
    return runCallback(ctx, false, &ev, err);
}

PluginHost::TriggerSet PluginHost::runCallback(Context& ctx, bool is_start, const TriggerEvent* ev,
                                               std::string* err) {
    if (script_ == nullptr) {
        const std::string why = "plugin: 脚本未装载";
        markFailed(why);
        if (err != nullptr) *err = why;
        return {};
    }
    ++callbacks_;
    ctx_ = &ctx;
    in_callback_ = true;
    TriggerSet out;
    std::string call_err;
    // ★ 预算 = **runner 的那一份**（`Budget{}` 缺省 2 ms / 100k；⛔ 不在这里再写一次数字）。
    const Budget budget = runner_.budget();
    const bool ok = is_start ? script_->start(*this, budget, &out, &call_err)
                             : script_->onTrigger(*this, *ev, budget, &out, &call_err);
    in_callback_ = false;
    ctx_ = nullptr;

    if (!ok) {
        const std::string why =
            !call_err.empty() ? call_err
                              : (script_->overran() ? "plugin: 回调超出预算（deadline / op_limit）"
                                                    : "plugin: 回调失败");
        markFailed(why);
        if (err != nullptr) *err = why;
        report("run failed", why);
        return {};
    }
    // ★ 三个出口（fail / finish / quit）都让**本次 Run 收口**：返回空集合 ⇒ `install` 置 done
    //   （⛔ 不保留 armed；⛔ 不自动 quit 进程 —— 那是 `quit` 那条路的账）。
    if (failed_ || finished_ || quit_requested_) return {};
    return out;
}

void PluginHost::markFailed(const std::string& reason) {
    failed_ = true;
    if (fail_reason_.empty()) fail_reason_ = reason;
}

void PluginHost::report(const char* what, const std::string& text) {
    std::fprintf(stderr, "[plugin] %s: %s\n", what != nullptr ? what : "error", oneLine(text).c_str());
    std::fflush(stderr);
}

// =====================================================================================
// PluginCallHost：Edit / Input / Read / Trigger 工厂 / now
// =====================================================================================
bool PluginHost::editGlobalSet(lift::GlobalKind kind, std::uint32_t id, const lift::GlobalValue& value,
                              const char** code) {
    if (ctx_ == nullptr || ctx_->edit == nullptr) {
        if (code != nullptr) *code = "not_wired";
        return false;
    }
    const Receipt receipt = ctx_->edit->globalSet(kind, id, value);
    if (code != nullptr) *code = receipt.code;
    if (!receipt.accepted) {
        // 合同 §4.3 490：失败 ⇒ receipt + **一行**（⛔ 不让核心 `exit 5`）。有界 ⇒ 不刷屏。
        log("error", std::string("ctx.edit.global.set 失败：") +
                          (receipt.code != nullptr ? receipt.code : "unknown"));
    }
    return receipt.accepted;
}

Receipt PluginHost::submitIntent(IntentKind kind, int x, int y, MouseButton button) {
    Receipt receipt;
    if (ctx_ == nullptr || ctx_->input == nullptr) {
        receipt.code = "not_wired";
        return receipt;
    }
    Intent intent;
    intent.kind = kind;
    intent.cls = IntentClass::GuestWrite;
    intent.origin = Origin::QjsControl;  // 归因：这是插件回调提的（trace / 对账）
    intent.owner = kOwner;
    intent.x = x;
    intent.y = y;
    intent.button = button;
    return ctx_->input->submit(intent);
}

Receipt PluginHost::inputClick(int x, int y, MouseButton button) {
    return submitIntent(IntentKind::Click, x, y, button);
}
Receipt PluginHost::inputMove(int x, int y) {
    return submitIntent(IntentKind::Move, x, y, MouseButton::Left);
}
Receipt PluginHost::inputDown(MouseButton button) {
    return submitIntent(IntentKind::MouseDown, 0, 0, button);
}
Receipt PluginHost::inputUp(MouseButton button) {
    return submitIntent(IntentKind::MouseUp, 0, 0, button);
}

ui::PluginInputState PluginHost::inputStatus(std::uint64_t id) {
    if (ctx_ == nullptr || ctx_->input == nullptr) return ui::PluginInputState::Unknown;
    switch (ctx_->input->status(id)) {
        case IntentQueue::Status::Queued:
            return ui::PluginInputState::Queued;
        case IntentQueue::Status::Applied:
            return ui::PluginInputState::Applied;
        case IntentQueue::Status::Dropped:
            return ui::PluginInputState::Failed;  // d.ts 的四态没有 "dropped"
        case IntentQueue::Status::Unknown:
            return ui::PluginInputState::Unknown;
    }
    return ui::PluginInputState::Unknown;
}

bool PluginHost::readGlobal(lift::GlobalKind kind, std::uint32_t id, lift::GlobalValue* out,
                            const char** code) {
    if (read_ == nullptr) {
        if (code != nullptr) *code = "not_wired";
        return false;
    }
    GlobalError err = GlobalError::None;
    const bool ok = read_->globalGet(kind, id, out, &err);
    if (code != nullptr) *code = ok ? nullptr : lift::globalErrorName(err);
    return ok;
}

Trigger PluginHost::makeAfterScript(const std::string& name, const std::string& script, TriggerMode mode,
                                    std::uint32_t max_fires, std::uint64_t timeout_ms) {
    if (ctx_trigger() == nullptr) return Trigger{};
    return ctx_trigger()->afterScript(name, script, mode, max_fires, timeout_ms);
}

Trigger PluginHost::makeAfterPosition(const std::string& name, const std::string& script, std::uint32_t lo,
                                      std::uint32_t hi, TriggerMode mode, std::uint32_t max_fires,
                                      std::uint64_t timeout_ms) {
    if (ctx_trigger() == nullptr) return Trigger{};
    return ctx_trigger()->afterPosition(name, script, lo, hi, mode, max_fires, timeout_ms);
}

Trigger PluginHost::makeAfterDelay(const std::string& name, std::uint64_t delay_ms, TriggerMode mode) {
    if (ctx_trigger() == nullptr) return Trigger{};
    return ctx_trigger()->afterDelay(name, delay_ms, mode);
}

Trigger PluginHost::makeAtReadPoint(const std::string& name, lift::InputPoint filter, std::uint32_t max_fires,
                                    TriggerMode mode, std::uint64_t timeout_ms) {
    if (ctx_trigger() == nullptr) return Trigger{};
    Trigger trigger = ctx_trigger()->atReadPoint(name, filter, max_fires, mode);
    // ★ `TriggerFactory::atReadPoint` **没有** `timeout_ms` 形参（C1 的签名）；
    //   `AtReadPointTrigger` 在 d.ts 里有 `timeoutMs` ⇒ 在这里补上（工厂只造值、install 才校验，
    //   所以补一个字段不绕过任何判据）。
    trigger.timeout_ms = timeout_ms;
    return trigger;
}

const TriggerReadings* PluginHost::now() const { return ctx_ != nullptr ? ctx_->now : nullptr; }

// =====================================================================================
// 观测 / 归因
// =====================================================================================
void PluginHost::log(const char* level, const std::string& message) {
    // ★ 有界 + 一行一条 + 连续重复去重（合同 §4.10；⛔ 不每读点刷屏）。
    if (log_lines_ >= kLogLineLimit) {
        if (log_lines_ == kLogLineLimit) {
            ++log_lines_;
            std::fprintf(stderr, "[plugin] log: 已达上限 %llu 行（后续丢弃）\n",
                         static_cast<unsigned long long>(kLogLineLimit));
            std::fflush(stderr);
        }
        return;
    }
    if (!message.empty() && message == last_log_) return;
    last_log_ = message;
    ++log_lines_;
    std::fprintf(stderr, "[plugin] %s: %s\n", level != nullptr ? level : "info", oneLine(message).c_str());
    std::fflush(stderr);
}

bool PluginHost::profileBegin(const std::string& label, std::string* err) {
    if (host_ops_ == nullptr || host_ops_->profile_begin == nullptr) {
        if (err != nullptr) *err = "宿主没有接 profile 桥（--plugin 的宿主能力表为空）";
        return false;
    }
    return host_ops_->profile_begin(host_ops_->ctx, label.c_str(), err);
}

bool PluginHost::profileEnd(std::string* err) {
    if (host_ops_ == nullptr || host_ops_->profile_end == nullptr) {
        if (err != nullptr) *err = "宿主没有接 profile 桥（--plugin 的宿主能力表为空）";
        return false;
    }
    // ★ D-C2-4：复用 `AotHost::endWindowText()`（同一份报告文本 + 同一份剪贴板行为）。
    //   报告走 **stdout**（与 `AotHost::endWindow()` 逐字同形）—— 插件的 `profile.end()` 没有
    //   HUD 这条回传路（d.ts 是 `void`），所以这里就是它的唯一出口。
    std::string text;
    std::string local_err;
    const bool ok = host_ops_->profile_end(host_ops_->ctx, &text, &local_err);
    if (!text.empty()) {
        std::fputs(text.c_str(), stdout);
        std::fflush(stdout);
    }
    if (!ok && err != nullptr) *err = local_err.empty() ? std::string("没有打开的 profile 窗口") : local_err;
    return ok;
}

void PluginHost::fail(const std::string& name, const std::string& reason) {
    // `ctx.fail(name?, reason?)`：点名状态（name 可空）+ 原因；清 armed 由 `runCallback` 返回 {}。
    failed_ = true;
    if (!name.empty()) fail_name_ = name;
    if (!reason.empty()) fail_reason_ = reason;
    report("fail", (name.empty() ? std::string("(unnamed)") : name) + ": " + reason);
}

void PluginHost::finish(int exit_code, const std::string& reason) {
    // R3：**不改进程 exit**；只结束本次 Run + 记录（readout 里 `run=finished exit=N reason=…`）。
    finished_ = true;
    finish_code_ = exit_code;
    finish_reason_ = reason;
}

void PluginHost::quit(const std::string& reason) {
    // D-C2-9：`stop_requested` + detail `plugin: quit`（`aot_run.cpp` 的归因位 = `stop_detail`）。
    quit_requested_ = true;
    if (!reason.empty()) quit_reason_ = reason;
    report("quit", quit_reason_.empty() ? std::string("(no reason)") : quit_reason_);
}

// =====================================================================================
// 收口 / readout
// =====================================================================================
std::size_t PluginHost::releaseOwner(WindowManager& wm) {
    return executor_.releaseOwner(wm, kOwner);
}

std::string PluginHost::readout() const {
    // ★ 一次性（一次 run 一行）；键名固定：`plugin    : ` / `run=` / `render=` / `callbacks=`。
    //   为什么 `run=` 是**结局**而不是"有没有 start/onTrigger"：D-C2-9 点名 `readout run=failed`，
    //   而"有没有回调"是另一件事 ⇒ 它有自己的键（`callbacks=yes|no`）。
    std::string out = "plugin    : file=";
    out += origin_.empty() ? "(none)" : origin_;
    out += " entry=default";
    out += std::string(" render=") + (script_ != nullptr && script_->hasRender() ? "yes" : "no");
    out += std::string(" callbacks=") + (script_ != nullptr && script_->hasRun() ? "yes" : "no");
    out += " params=" + paramsText(params_);
    out += " calls=" + std::to_string(callbacks_);
    const char* state = "waiting";
    if (failed_) {
        state = "failed";
    } else if (quit_requested_) {
        state = "quit";
    } else if (finished_) {
        state = "finished";
    } else if (runner_.done()) {
        state = "done";
    }
    out += std::string(" run=") + state;
    if (!fail_name_.empty()) out += " name=" + oneLine(fail_name_);
    if (finished_) out += " exit=" + std::to_string(finish_code_);
    if (failed_) {
        if (!fail_reason_.empty()) out += " reason=" + oneLine(fail_reason_);
    } else if (quit_requested_) {
        if (!quit_reason_.empty()) out += " reason=" + oneLine(quit_reason_);
    } else if (finished_ && !finish_reason_.empty()) {
        out += " reason=" + oneLine(finish_reason_);
    }
    return out;
}

}  // namespace lift::plugin
