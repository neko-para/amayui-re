// emulator-lift · `PluginHost` 的实现 —— 口径见头注（这里只写"这一段为什么这样写"）
#include "plugin/plugin_host.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>  // std::getenv（诊断开关；⛔ 不是 CLI 旋钮、不进 --help）
#include <cstring>  // std::strcmp（`no_injection` 的响亮分支）
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

// 读一个宿主文件（UTF-8 路径；失败 ⇒ false）。⛔ 不用 std::ifstream：Windows 上它按 ANSI
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

// 插件入口路径 → 日志归属名（文件名的 stem）：`.../field-point-visible.js` ⇒ `field-point-visible`。
std::string pluginNameFromPath(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = file.find_last_of('.');
    if (dot != std::string::npos && dot != 0) file.resize(dot);
    return file.empty() ? std::string("plugin") : file;
}

// readout 是**一行**：理由文本里的换行必须折成空格。
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

// `ctx.at` / `once` 的日志描述（`form` 决定读哪几个字段）。
std::string describeInstrWhere(const lift::InstrHookWhere& where) {
    char hex[16];
    switch (where.form) {
        case lift::InstrHookForm::Pos: {
            std::snprintf(hex, sizeof(hex), "0x%X", where.pos);
            return "script=" + where.script + " pos=" + hex;
        }
        case lift::InstrHookForm::Range: {
            char lo_hex[16];
            char hi_hex[16];
            std::snprintf(lo_hex, sizeof(lo_hex), "0x%X", where.lo);
            std::snprintf(hi_hex, sizeof(hi_hex), "0x%X", where.hi);
            return "script=" + where.script + " range=[" + lo_hex + "," + hi_hex + ")";
        }
        case lift::InstrHookForm::Script:
            return "script=" + where.script + "（整本）";
        case lift::InstrHookForm::Opcode: {
            std::snprintf(hex, sizeof(hex), "0x%X", where.opcode);
            return (where.script.empty() ? std::string() : "script=" + where.script + " ") +
                   "opcode=" + hex;
        }
    }
    return "?";
}

// =====================================================================================
// 回调耗时诊断（REQ-01M7VVDV69MJXRHNDGR50H7VHC 待查 #3）
//
// ★ 默认**零输出**、⛔ 不进 `--help`：只有 `LIFT_PLUGIN_CALLBACK_ELAPSED` 存在时才逐回调打一行。
//   唯一例外 = **超预算那条无论开关都打**。
// =====================================================================================
bool callbackElapsedDiagOn() {
    static const bool on = std::getenv("LIFT_PLUGIN_CALLBACK_ELAPSED") != nullptr;
    return on;
}

std::map<std::string, std::uint64_t>& callbackCalls() {
    static std::map<std::string, std::uint64_t> calls;
    return calls;
}

std::string diagName(const char* raw) {
    std::string out = raw != nullptr ? raw : "";
    if (out.empty()) out = "?";
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || c == ' ') c = '_';
    }
    return out;
}

void emitCallbackDiag(const lift::plugin::CallbackDiag& d, const char* raw_name, bool ok,
                      const lift::plugin::Budget& budget) {
    const bool js_late = budget.deadline_ns != 0 && d.js_cpu_ns >= budget.deadline_ns;
    const bool host_late =
        budget.deadline_ns != 0 && d.host_ns >= lift::plugin::hostSafetyNetNs(budget);
    const bool over = d.inner_overran || js_late || host_late;
    if (!over && !callbackElapsedDiagOn()) return;
    const std::string name = diagName(raw_name);
    const std::uint64_t calls = ++callbackCalls()[name];
    const std::uint64_t gap = d.host_ns > d.inner_ns ? d.host_ns - d.inner_ns : 0;
    const unsigned long long elapsed_us = static_cast<unsigned long long>(d.host_ns / 1000u);
    const unsigned long long build_us = static_cast<unsigned long long>(d.build_ns / 1000u);
    const unsigned long long js_us = static_cast<unsigned long long>(d.js_ns / 1000u);
    const unsigned long long js_cpu_us = static_cast<unsigned long long>(d.js_cpu_ns / 1000u);
    const unsigned long long paused_us = static_cast<unsigned long long>(d.host_pause_ns / 1000u);
    const unsigned long long collect_us = static_cast<unsigned long long>(d.collect_ns / 1000u);
    const unsigned long long gap_us = static_cast<unsigned long long>(gap / 1000u);
    const unsigned long long load_us = static_cast<unsigned long long>(d.module_load_ns / 1000u);
    if (!over) {
        std::fprintf(stderr,
                     "callback name=%s elapsed_us=%llu ok=%d calls=%llu build_us=%llu js_us=%llu"
                     " js_cpu_us=%llu paused_us=%llu collect_us=%llu gap_us=%llu module_load_us=%llu\n",
                     name.c_str(), elapsed_us, ok ? 1 : 0, static_cast<unsigned long long>(calls),
                     build_us, js_us, js_cpu_us, paused_us, collect_us, gap_us, load_us);
    } else {
        const char* how = d.inner_overran
                              ? (budget.deadline_ns != 0 && d.js_cpu_ns >= budget.deadline_ns ? "deadline"
                                                                                               : "ops")
                              : (js_late ? "js" : "host");
        std::fprintf(stderr,
                     "callback name=%s elapsed_us=%llu ok=%d calls=%llu build_us=%llu js_us=%llu"
                     " js_cpu_us=%llu paused_us=%llu collect_us=%llu gap_us=%llu module_load_us=%llu"
                     " budget_us=%llu over=%s\n",
                     name.c_str(), elapsed_us, ok ? 1 : 0, static_cast<unsigned long long>(calls),
                     build_us, js_us, js_cpu_us, paused_us, collect_us, gap_us, load_us,
                     static_cast<unsigned long long>(budget.deadline_ns / 1000u), how);
    }
    std::fflush(stderr);
}

}  // namespace

PluginHost::PluginHost() : executor_(queue_) {}

// `unique_ptr<GuestEngineRead>` 的析构要完整类型 ⇒ 定义放这里（见头注）。
PluginHost::~PluginHost() {
    // ★ 卸载 = Run 收口的一条路径 ⇒ 先撤本 owner 的 hook（未还原的写按 CAS 结算）。
    releaseRunHooks();
    // ★ `setCallHost` 是**非拥有**指针 ⇒ 析构时先撤掉（⛔ 不把悬垂的 `this` 留给脚本对象）。
    if (script_ != nullptr) script_->setCallHost(nullptr);
}

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
    // ★ config / draw 的宿主来路（非拥有；生命周期 = 本对象 > 脚本）。
    script_->setCallHost(this);
    return true;
}

bool PluginHost::initFromFile(const std::string& path, const Params& params, const ui::UiHostOps* host_ops,
                              std::string* err) {
    name_ = pluginNameFromPath(path);
    std::string source;
    if (!readHostSource(path, &source)) {
        if (err != nullptr) *err = path + ": 读不到插件文件";
        return false;
    }
    return init(source, path, params, host_ops, err);
}

void PluginHost::setFont(lift::FontEngine* font) {
    if (script_ != nullptr) script_->setFont(font);
}

void PluginHost::attachMemory(lift::GuestMemory& mem) {
    // ★ 两条都是**非拥有**引用：`mem` 归宿主。
    edit_ = std::make_unique<GuestEditSink>(mem);        // Edit：立即写（⛔ 不排队）
    read_ = std::make_unique<GuestEngineRead>(mem);      // EngineRead：同步只读（⛔ 不暂停）
}

bool PluginHost::attachAppData(const std::string& appdata_dir, const std::string& install_dir,
                               std::string* err) {
    return appdata_.attach(appdata_dir, install_dir, err);
}

bool PluginHost::attachConfig(std::string* err) {
    return config_.attach(appdata_.path(), err);
}

// =====================================================================================
// ReadPointObserver（唯一入口）
// =====================================================================================
void PluginHost::onReadPoint(WindowManager& wm, InputPoint point) {
    if (script_ == nullptr) return;
    current_wm_ = &wm;
    // ① executor（② `beginReadPoint` 已跑；没有可做的工作时早退）。
    executor_.atPoint(wm, point);
    // ★ S2b：async 泵（**没有独立 timer**）。位置有意：`runClosed()` 判断**之前** ——
    //   泵可能在**本读点**把最后一个 task settle 掉 / 把 once 超时处理掉 ⇒ 同一个读点就要能收口。
    //   ★ 已经到终态时**不再**跑用户 JS（Run 已收口；残余 job 交给 `Impl::shutdown` 的兜底 drain）。
    if (!runTerminal()) script_->pump(*this, budget_);
    // 已开始的 Run：收口判定。
    if (runClosed()) releaseRunHooks();
    if (begun_ || failed_ || quit_requested_) return;

    // ② 首个读点：`start`（只调一次）。
    begun_ = true;
    ++run_serial_;
    runStart();
    if (runClosed()) releaseRunHooks();
    // ★ `start` 里 `ctx.fail` / `ctx.quit` 是终态异常 ⇒ 已 staged 的 input 不许落地。
    if (failed_ || quit_requested_) releaseRunInput();
}

void PluginHost::releaseRunHooks() {
    lift::instrHookReleaseOwner(run_serial_);
    // ★ Run 收口也丢掉**不再推进**的脚本侧异步工作（`ctx.delay` / `waitAny` 的账）。
    if (script_ != nullptr) script_->onRunClosed();
}

bool PluginHost::hasPendingWork() const {
    return script_ != nullptr && script_->hasPendingWork();
}

bool PluginHost::runClosed() const {
    return runTerminal() || (begun_ && !hasPendingWork());
}

// =====================================================================================
// start
// =====================================================================================
void PluginHost::runStart() {
    if (!script_->hasStart()) return;  // render-only 插件没有 Run
    ++callbacks_;
    const std::uint64_t host_start_ns = pluginNowNs();
    lift::plugin::CallbackDiag& diag = lift::plugin::callbackDiag();
    diag = lift::plugin::CallbackDiag{};
    in_callback_ = true;
    std::string call_err;
    const Budget budget = budget_;
    bool ok = script_->start(*this, budget, &call_err);
    // ★ `ctx.config` 的持久化：**每个回调末** flush 一次（只写脏数据；原子替换）。
    {
        std::string flush_warn;
        config_.flushIfDirty(&flush_warn);
        if (!flush_warn.empty()) log("warn", flush_warn);
    }
    const std::uint64_t host_end_ns = pluginNowNs();
    diag.host_ns = host_end_ns >= host_start_ns ? host_end_ns - host_start_ns : 0;
    // 判据（2026-11-21 收窄）：2 ms 只罩用户 JS 本体，且用 CPU 钟；宿主段另有 20 ms 安全网。
    const bool js_late = budget.deadline_ns != 0 && diag.js_cpu_ns >= budget.deadline_ns;
    const bool host_late =
        budget.deadline_ns != 0 && diag.host_ns >= lift::plugin::hostSafetyNetNs(budget);
    if (ok && (js_late || host_late)) {
        ok = false;
        call_err = callbackBudgetReason("start", std::string(), /*host=*/host_late && !js_late);
    }
    emitCallbackDiag(diag, "start", ok, budget);
    in_callback_ = false;
    if (!ok) {
        const std::string why =
            !call_err.empty() ? call_err
                              : (script_->overran() ? "plugin: 回调超出预算（deadline / op_limit）"
                                                    : "plugin: 回调失败");
        markFailed(why);
        report("run failed", why);
    }
}

void PluginHost::markFailed(const std::string& reason) {
    failed_ = true;
    if (fail_reason_.empty()) fail_reason_ = reason;
    releaseRunHooks();
}

void PluginHost::report(const char* what, const std::string& text) {
    std::fprintf(stderr, "[plugin %s] %s: %s\n", name_.c_str(), what != nullptr ? what : "error",
                 oneLine(text).c_str());
    std::fflush(stderr);
}

// =====================================================================================
// PluginCallHost：Edit / Input / Read / tick patch / at 注册
// =====================================================================================
bool PluginHost::editGlobalSet(lift::GlobalKind kind, std::uint32_t id, const lift::GlobalValue& value,
                              const char** code) {
    if (edit_ == nullptr) {
        if (code != nullptr) *code = "not_wired";
        return false;
    }
    const Receipt receipt = edit_->globalSet(kind, id, value);
    if (code != nullptr) *code = receipt.code;
    if (!receipt.accepted) {
        // 合同 §4.3 490：失败 ⇒ receipt + **一行**（⛔ 不让核心 `exit 5`）。有界 ⇒ 不刷屏。
        log("error", std::string("ctx.edit.global.set 失败：") +
                          (receipt.code != nullptr ? receipt.code : "unknown"));
    }
    return receipt.accepted;
}

std::uint32_t PluginHost::instrHookAddWhere(const lift::InstrHookWhere& where, lift::InstrHookKind kind,
                                            const char** err) {
    const char* why = nullptr;
    const std::uint32_t handle = lift::instrHookAddEx(run_serial_, where, kind, &why);
    if (err != nullptr) *err = why;
    if (handle == 0) {
        // ★ `no_injection` 不是用户输入错，而是**这一趟构建没有注入站点**
        //   （`--no-s7` / 闭包没覆盖脚本派发 EA）⇒ 订阅永不命中。这条必须**响亮**。
        if (why != nullptr && std::strcmp(why, "no_injection") == 0) {
            report("ctx.at 不可用",
                   "no_injection：本构建在脚本派发点没有注入 lift_instr_hook"
                   "（--no-s7 或闭包没覆盖派发 EA）⇒ ctx.at / ctx.once 永不命中");
        } else {
            log("error", std::string(kind == lift::InstrHookKind::Sub ? "ctx.at " : "ctx.once ") +
                             "失败：" + (why != nullptr ? why : "unknown"));
        }
        return 0;
    }
    log("info", std::string(kind == lift::InstrHookKind::Sub ? "ctx.at " : "ctx.once ") +
                   describeInstrWhere(where) + " handle=" + std::to_string(handle));
    return handle;
}

bool PluginHost::instrHookSetTickPatch(std::uint32_t handle, lift::InstrPatchKind kind,
                                       std::uint32_t index, std::uint32_t value, const char** err) {
    const char* why = nullptr;
    const bool ok = lift::instrHookSetTickPatch(handle, kind, index, value, &why);
    if (err != nullptr) *err = why;
    // ★ 不 log 成功（tick 可能每拍都调用）；失败由 JS 回执 `code` 带走。
    return ok;
}

bool PluginHost::instrHookRemove(std::uint32_t handle) { return lift::instrHookRemove(handle); }

std::vector<lift::InstrHookStatus> PluginHost::instrHookList() { return lift::instrHookList(); }

void PluginHost::instrHookClear() { lift::instrHookClear(); }

Receipt PluginHost::submitIntent(IntentKind kind, int x, int y, MouseButton button) {
    Receipt receipt;
    // ★ `Tick.input` 在回调之外调用 ⇒ 用成员队列（与回调里是**同一条**）。
    Intent intent;
    intent.kind = kind;
    intent.cls = IntentClass::GuestWrite;
    intent.origin = Origin::QjsControl;  // 归因：这是插件回调提的（trace / 对账）
    intent.owner = kOwner;
    intent.x = x;
    intent.y = y;
    intent.button = button;
    return queue_.submit(intent);
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
    switch (queue_.status(id)) {
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

bool PluginHost::readLocal(const lift::InstrLocalView& view, lift::InstrLocalKind kind,
                           std::uint32_t index, std::uint32_t* out) {
    return read_ != nullptr && read_->readLocal(view, kind, index, out);
}

// =====================================================================================
// 观测 / 归因
// =====================================================================================
void PluginHost::log(const char* level, const std::string& message) {
    // ★ 有界 + 一行一条 + 连续重复去重（合同 §4.10；⛔ 不每读点刷屏）。
    if (log_lines_ >= kLogLineLimit) {
        if (log_lines_ == kLogLineLimit) {
            ++log_lines_;
            std::fprintf(stderr, "[plugin %s] log: 已达上限 %llu 行（后续丢弃）\n", name_.c_str(),
                         static_cast<unsigned long long>(kLogLineLimit));
            std::fflush(stderr);
        }
        return;
    }
    if (!message.empty() && message == last_log_) return;
    last_log_ = message;
    last_line_ = message;
    last_level_ = level != nullptr ? level : "info";
    ++log_lines_;
    std::fprintf(stderr, "[plugin %s] %s: %s\n", name_.c_str(), level != nullptr ? level : "info",
                 oneLine(message).c_str());
    std::fflush(stderr);
}

bool PluginHost::profileBegin(const std::string& label, std::string* err) {
    if (host_ops_ == nullptr || host_ops_->profile_begin == nullptr) {
        if (err != nullptr) *err = "宿主没有接 profile 桥（--plugin 的宿主能力表为空）";
        return false;
    }
    return host_ops_->profile_begin(host_ops_->ctx, label.c_str(), err);
}

bool PluginHost::profileEndText(std::string* text, std::string* err) {
    if (host_ops_ == nullptr || host_ops_->profile_end == nullptr) {
        if (err != nullptr) *err = "宿主没有接 profile 桥（--plugin 的宿主能力表为空）";
        if (text != nullptr) text->clear();
        return false;
    }
    std::string local_text;
    std::string local_err;
    const bool ok = host_ops_->profile_end(host_ops_->ctx, &local_text, &local_err);
    if (text != nullptr) *text = ok ? local_text : std::string();
    if (!ok && err != nullptr) {
        *err = local_err.empty() ? std::string("没有打开的 profile 窗口") : local_err;
    }
    return ok;
}

bool PluginHost::profileEnd(std::string* err) {
    std::string text;
    const bool ok = profileEndText(&text, err);
    if (!text.empty()) {
        std::fputs(text.c_str(), stdout);
        std::fflush(stdout);
    }
    return ok;
}

void PluginHost::fail(const std::string& name, const std::string& reason) {
    failed_ = true;
    if (!name.empty()) fail_name_ = name;
    if (!reason.empty()) fail_reason_ = reason;
    releaseRunHooks();
    report("fail", (name.empty() ? std::string("(unnamed)") : name) + ": " + reason);
}

void PluginHost::finish(int exit_code, const std::string& reason) {
    // R3：**不改进程 exit**；只结束本次 Run + 记录。
    finished_ = true;
    finish_code_ = exit_code;
    finish_reason_ = reason;
    releaseRunHooks();
}

void PluginHost::quit(const std::string& reason) {
    quit_requested_ = true;
    if (!reason.empty()) quit_reason_ = reason;
    releaseRunHooks();
    report("quit", quit_reason_.empty() ? std::string("(no reason)") : quit_reason_);
}

// =====================================================================================
// 收口 / readout
// =====================================================================================
std::size_t PluginHost::releaseOwner(WindowManager& wm) {
    releaseRunHooks();
    return executor_.releaseOwner(wm, kOwner);
}

void PluginHost::releaseRunInput() {
    if (current_wm_ == nullptr) return;
    // `IntentExecutor::releaseOwner`：真的抬起已按住的 / 丢掉队列里未落地的。
    executor_.releaseOwner(*current_wm_, kOwner);
}

std::string PluginHost::readout() const {
    std::string out = "plugin    : file=";
    out += origin_.empty() ? "(none)" : origin_;
    out += " entry=default";
    out += std::string(" render=") + (script_ != nullptr && script_->hasRender() ? "yes" : "no");
    out += std::string(" callbacks=") + (script_ != nullptr && script_->hasStart() ? "yes" : "no");
    out += " params=" + paramsText(params_);
    out += " calls=" + std::to_string(callbacks_);
    const char* state = "waiting";
    if (failed_) {
        state = "failed";
    } else if (quit_requested_) {
        state = "quit";
    } else if (finished_) {
        state = "finished";
    } else if (begun_ && !hasPendingWork()) {
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
