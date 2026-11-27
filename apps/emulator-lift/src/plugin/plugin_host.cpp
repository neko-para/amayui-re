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

// 插件入口路径 → 日志归属名（文件名的 stem）：`.../field-point-visible.js` ⇒ `field-point-visible`。
// ★ 只在 `initFromFile` 调一次（⛔ 不在每条日志现场算路径）；分隔符认 `/` 与 `\\`（两平台）。
// ★ 拿不到文件名的场合（空路径 / 只有分隔符）⇒ `plugin`。
std::string pluginNameFromPath(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
    const std::size_t dot = file.find_last_of('.');
    if (dot != std::string::npos && dot != 0) file.resize(dot);
    return file.empty() ? std::string("plugin") : file;
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

// =====================================================================================
// 回调耗时诊断（REQ-01M7VVDV69MJXRHNDGR50H7VHC 待查 #3）
//
// ★ 默认**零输出**、⛔ 不进 `--help`：只有 `LIFT_PLUGIN_CALLBACK_ELAPSED` 存在时才逐回调打一行。
//   唯一例外 = **超预算那条无论开关都打**（失败本来就该有读数；否则只剩一句 reason）。
// ★ 起点 = `runCallback` 入口 → `host_ns` = **宿主段窗口**（墙钟；不是 2 ms 判据；判据见下面的
//   `js_cpu_ns`）。内层读数由 `script_quickjs.cpp` 填进同一个槽；`js_cpu_ns`（**CPU 钟**）才是
//   "2 ms 用在哪"。`js_ns`（墙钟）保留为对照：`elapsed_us` 远大于 `js_cpu_us` = 被 OS 抢占。
// =====================================================================================
bool callbackElapsedDiagOn() {
    static const bool on = std::getenv("LIFT_PLUGIN_CALLBACK_ELAPSED") != nullptr;
    return on;
}

// 回调名 → 调用序号（诊断行的 `calls=`；单线程 guest 循环 ⇒ 一个文件级表就够）。
std::map<std::string, std::uint64_t>& callbackCalls() {
    static std::map<std::string, std::uint64_t> calls;
    return calls;
}

// 名字进 stderr 前把控制字符 / 空格换成 `_`：一行一条是读数的**形状**（否则无法配对）。
std::string diagName(const char* raw) {
    std::string out = raw != nullptr ? raw : "";
    if (out.empty()) out = "?";
    for (char& c : out) {
        if (static_cast<unsigned char>(c) < 0x20 || c == ' ') c = '_';
    }
    return out;
}

// `over` 的来路必须可分辨（三类）：
//   `deadline` / `ops` = QJS 中断回调真的打断过（`inner_overran`）；
//   `js`   = JS 本体跑完、但 `js_cpu_ns >= 2 ms`（**CPU 钟**；没被打断的那种边界）；
//   `host` = **宿主段**（ctx 构造 / 收集 / 调度）越过 10×（20 ms）安全网 —— 墙钟，时间花在插件代码之外。
// ★ 2026-11-21 收窄后 `over` **不再**等于"2 ms 判据失败"：2 ms 只看 `js_cpu_ns`（见 `runCallback`）。
// ★ `paused_us` = **宿主调用暂停**的读数（`ctx.profile.end` 一类宿主能力的 CPU；`script_quickjs.cpp`
//   的 `HostPause` 量、已从 `js_cpu_us` 里扣掉）。`js_cpu_us` 小、`paused_us` 大 ⇒ 这段时间是宿主
//   替用户 JS 付的，不是用户 JS 跑飞（口径见 `runtime/budget.h` 头注）。
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
    // 宿主调用暂停（`ctx.profile.end` 一类宿主能力的 CPU；**已从** `js_cpu_us` 里扣掉）。
    // ★ 与 `js_cpu_us` 同一把 CPU 钟 ⇒ 两者可以直接对照（⛔ `host_ns` 是墙钟，别混）。
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
PluginHost::~PluginHost() {
    // ★ S0（缺陷 1）：卸载 = Run 收口的一条路径 ⇒ 先撤本 owner 的 hook（未还原的写按 CAS 结算）。
    //   顺序无所谓（`instrHookReleaseOwner` 不碰 `script_`），但必须在**本对象还活着**时做 ——
    //   它只需要 `instr_hook.cpp` 的内部 static 表 + 上次 probe 记下的 guest 内存。
    releaseRunHooks();
    // ★ C2b：`setCallHost` 是**非拥有**指针 ⇒ 析构时先撤掉（⛔ 不把悬垂的 `this` 留给脚本对象）。
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
    // ★ C2b：`RenderCtx.read` / `ctx.log` 的宿主来路（非拥有；生命周期 = 本对象 > 脚本）。
    //   ⛔ 不要改成"每帧从 readings 塞进去"（`UiFrameReadings` 是**只读读数**；读写两条面分开）。
    script_->setCallHost(this);
    return true;
}

bool PluginHost::initFromFile(const std::string& path, const Params& params, const ui::UiHostOps* host_ops,
                              std::string* err) {
    // ★ 日志归属在这里定**一次**（`[plugin <name>]`）；`init` 不碰它（直接 `init` 的夹具走缺省 `plugin`）。
    name_ = pluginNameFromPath(path);
    std::string source;
    if (!readHostSource(path, &source)) {
        if (err != nullptr) *err = path + ": 读不到插件文件";
        return false;
    }
    return init(source, path, params, host_ops, err);
}

void PluginHost::setFont(lift::FontEngine* font) {
    // ★ 只转发：字体是**渲染面**的依赖，不是 Run 的（`PluginScript` 同时是 `UiScript`）。
    //   `script_ == nullptr`（init 之前 / 装载失败）⇒ 什么也不做（⛔ 不编一个"以后再用"的暂存变量：
    //   那会让"接线顺序错了"变成静默可用）。
    if (script_ != nullptr) script_->setFont(font);
}

void PluginHost::attachMemory(lift::GuestMemory& mem) {
    // ★ 两条都是**非拥有**引用：`mem` 归宿主（`AotHost::mem()`；这里只在 init/prepare 之后接）。
    edit_ = std::make_unique<GuestEditSink>(mem);        // Edit：立即写（⛔ 不排队）
    read_ = std::make_unique<GuestEngineRead>(mem);      // EngineRead：同步只读（⛔ 不暂停）
    executor_.setBus(edit_.get(), read_.get());
}

bool PluginHost::attachAppData(const std::string& appdata_dir, const std::string& install_dir,
                               std::string* err) {
    // ★ 只转发给 `PluginAppData`：绝对化 / appdata-only / escape 判据都在那里
    //   （`src/plugin/plugin_appdata.cpp`；本类不碰 `std::filesystem`）。
    return appdata_.attach(appdata_dir, install_dir, err);
}

bool PluginHost::attachConfig(std::string* err) {
    // ★ 复用 appdata 的**绝对化根**（`PluginAppData::path()`）；没有 --appdata ⇒ 空串。
    //   配置文件位置由 `PluginConfig` 定（`<根>/plugin-config.json`）；本类不拼路径。
    return config_.attach(appdata_.path(), err);
}

// ★ Config（`ConfigBridge`）的方法全是头文件里的薄转发（`plugin_host.h`）：本类**同时是**那个桥，
//   `config()` 返回 `this`（D2：`RenderCtx.config` 与经典 `host.config` 共用一份实现）。

// =====================================================================================
// ReadPointObserver（C2a 的唯一入口）
// =====================================================================================
void PluginHost::onReadPoint(WindowManager& wm, InputPoint point) {
    if (script_ == nullptr) return;
    // ★ F3：本读点的 wm 留给 `onTrigger`（终态异常要在这里释放本 owner 的输入状态）。
    current_wm_ = &wm;
    // ① 七步（② `beginReadPoint` 已跑；没有 armed 时早退 —— 这是 C1 的既有语义，⛔ 不改）。
    executor_.atPoint(wm, point);
    // ★ F2：`onTrigger` 返回的集合由 **runner** `install`；失败（重复 name / Once 回放 / 非法 kind）
    //   只会写进 runner 的账 ⇒ 不同步出来的话 readout 会说 `run=waiting`（生产上只能等 --time-limit）。
    syncRunnerFailure();
    // ★ S0（缺陷 1）：`onTrigger` 返回 `[]` ⇒ `install` 把 runner 置 done，**这发生在回调返回之后**
    //   ⇒ `onTrigger` / `runCallback` 自己看不到。这里是读点入口里唯一能看到那个状态的地方。
    if (runClosed()) releaseRunHooks();
    if (begun_ || failed_ || quit_requested_) return;

    // ② 首点：只采样 + `begin`（D-C2-10）。`sampleNow` 让 `start` 里的 `ctx.now` 有值；
    //    ⛔ 它不加读点计数 / 不碰队列（那两笔账在 `atPoint` 里，已经记过）。
    begun_ = true;
    // ★ S0（缺陷 1）：Run 从这一刻开始 ⇒ 新 owner。`start` 里的 `ctx.hook.at` 必须落到它上面
    //   （所以这一行必须在 `runner_.begin` **之前**）。
    ++run_serial_;
    executor_.sampleNow(point);
    std::string err;
    runner_.begin(*this, executor_.context(), &err);
    if (!err.empty() && !failed_) {
        // 两种来路：① 本类的 `start` 记了 JS 的错（`runner_.begin` 只清 err、不看清）；
        //           ② runner 自己判的（重复 name / 无 state / 预算）⇒ 由 `lastError()` 点名。
        // ★ F7：`runCallback` 已经为 JS 失败打过一行（并置 `failed_`）⇒ 这里**只**补 runner 那一路，
        //   否则同一条 "run failed" 会打两遍（收口时那是唯一消息面）。
        const std::string why = runner_.failed() && !runner_.lastError().empty() ? runner_.lastError() : err;
        markFailed(why);
        report("run failed", why);
    }
    // ★ S0（缺陷 1）：`start` 直接 `return []`（正常 done）/ install 拒绝 ⇒ 同一个读点里就收口。
    if (runClosed()) releaseRunHooks();
    // ★ F3（start 侧）：`start` 里 `ctx.fail` / `ctx.quit` 也是终态异常 ⇒ 已 staged 的 input 不许落地。
    if (failed_ || quit_requested_) releaseRunInput();
}

void PluginHost::releaseRunHooks() { lift::instrHookReleaseOwner(run_serial_); }

bool PluginHost::runClosed() const {
    return failed_ || finished_ || quit_requested_ || runner_.failed() || runner_.done();
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
    TriggerSet out = runCallback(ctx, false, &ev, err);
    // ★ F3（父级裁决，写死）：`fail` / `quit` 是**终态异常** ⇒ 就在 JS 返回之后、`executor_.atPoint`
    //   的第 ⑥ 步（`drainQueue`）**之前**清掉本 owner 的 staged input / 按住 / armed ——
    //   本读点之后**不允许**再落地。`finish` / `return []` 不释放（正常收口：本读点提交的那次
    //   `ctx.input.click` 仍要落地；见 `plugin/finish-lands-input`）。
    if (failed_ || quit_requested_) releaseRunInput();
    return out;
}

void PluginHost::releaseRunInput() {
    if (current_wm_ == nullptr) return;
    // `IntentExecutor::releaseOwner` 做三件事：真的抬起已按住的（post up）/ 丢掉队列里未落地的
    // （staged + ready）/ 抹掉 runner 里该 owner 的 armed 槽。⇒ 一个入口，⛔ 不在这里手搓第二份。
    executor_.releaseOwner(*current_wm_, kOwner);
}

void PluginHost::syncRunnerFailure() {
    if (failed_ || !runner_.failed()) return;
    // ★ F2：`lastError()` 是 runner 点名的（"duplicate trigger name: x" / "trigger exhausted: x" /
    //   "unsupported trigger kind: x" / 预算）⇒ 原样进 readout 的 `reason=`，⛔ 不编第二套文案。
    markFailed(runner_.lastError().empty() ? std::string("plugin: run failed（runner 未给原因）")
                                           : runner_.lastError());
    report("run failed", fail_reason_);
    // ★ **F3-R1**（复审第二轮）：runner 侧的失败（`install` 拒了回调返回的集合）**也是终态** ⇒
    //   与 `ctx.fail` / `ctx.quit` 同一条口径：同一个回调里已经 staged 的 input 必须丢掉。
    //   为什么必须在这里（而不是 `onTrigger` 里）：`install` 的失败发生在 `onTrigger` **返回之后**
    //   （`TriggerRunner::invokeOnTrigger` 里），那时 `onTrigger` 的归因位还看不到 ⇒ 只有这一处能兜。
    //   时机仍安全：回调里的 `ctx.input.*` 一律进 `staged_`，而本读点的第 ⑥ 步只 drain `ready_`
    //   ⇒ 这一拍它还没落地；下一次读点第 ② 步提升之前就会被 `dropOwner` 抹掉。
    releaseRunInput();
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
    // ★ 诊断：起点 = 本函数入口（`host_ns` = 宿主段窗口；⛔ 不是 2 ms 判据）。
    const std::uint64_t host_start_ns = pluginNowNs();
    lift::plugin::CallbackDiag& diag = lift::plugin::callbackDiag();
    diag = lift::plugin::CallbackDiag{};  // 复位：内层读数由 `script_quickjs.cpp` 的 `call()` 填
    ctx_ = &ctx;
    in_callback_ = true;
    TriggerSet out;
    std::string call_err;
    // ★ 预算 = **runner 的那一份**（`Budget{}` 缺省 2 ms / 100k；⛔ 不在这里再写一次数字）。
    const Budget budget = runner_.budget();
    bool ok = is_start ? script_->start(*this, budget, &out, &call_err)
                       : script_->onTrigger(*this, *ev, budget, &out, &call_err);
    // ★ `ctx.config` 的持久化（2026-11-25）：**每个回调末** flush 一次（只写脏数据；原子替换）。
    //   ⛔ 不放进 `js_cpu_ns` 的窗口（它在 JS 返回之后）；⛔ 不每个 set 都写盘。
    //   第一次写盘失败 ⇒ 经 `ctx.log("warn")` 说一次，之后不再重试（`PluginConfig` 的 gave_up）。
    //   ★ 2026-11-25 复审 I12：flush 在 `host_end_ns` **之前**，所以它落进宿主安全网（20 ms 墙钟）
    //     与诊断行的 `elapsed` —— 慢盘卡 5 s 时不再"只表现为 guest 停住"。
    {
        std::string flush_warn;
        config_.flushIfDirty(&flush_warn);
        if (!flush_warn.empty()) log("warn", flush_warn);
    }
    const std::uint64_t host_end_ns = pluginNowNs();
    diag.host_ns = host_end_ns >= host_start_ns ? host_end_ns - host_start_ns : 0;
    // =================================================================================
    // 判据（2026-11-21 收窄，用户裁决 D5 可改）：**2 ms 只罩用户 JS 本体，且用 CPU 钟**
    //   · `js_cpu_ns` = 用户 JS 本体的**本线程 CPU 时间**（QJS 侧从"即将调用插件函数"量到 JS 返回）；
    //     ⛔ 不要用 `js_ns`（墙钟）判 —— 被 OS 抢占 / 等 I/O 会让一次正常的回调偶发红
    //     （缺陷单 REQ-01M7VVDV69MJXRHNDGR50H7VHC）；`js_ns` 只作 wall/cpu 对照读数。
    //   · 模块装载（`call()` 之前）、ctx / 事件对象构造、返回值→TriggerSet 收集、宿主调度
    //     **都不计** —— 这些是宿主替用户代码付的成本。
    //   · **宿主调用暂停**（2026-11-25）：用户 JS 里调宿主能力（`ctx.profile.end` 生成报告 + 写剪贴板）
    //     的那段 CPU 也**不计** —— `diag.js_cpu_ns` 已经是**扣除后**的值（`paused_us` 是扣掉的那段）。
    //     ⛔ 这不是放宽：用户 JS 自己的 CPU 一分不减（`plugin/callback-js-band` / `callback-cpu-budget`）。
    //   · **宿主段另有安全网** = 10×（20 ms），**仍是墙钟**：防"宿主被卡死 ⇒ guest 被拖住"。超了照样
    //     响亮失败，但 reason 带 `(host)`（⛔ 不与用户 JS 超限混成同一条）。
    //   · 用户 JS 真的跑飞时 QJS 中断回调会先打断（`inner_overran` ⇒ 脚本返回 false + 已中断文案）。
    // =================================================================================
    const bool js_late = budget.deadline_ns != 0 && diag.js_cpu_ns >= budget.deadline_ns;
    const bool host_late =
        budget.deadline_ns != 0 && diag.host_ns >= lift::plugin::hostSafetyNetNs(budget);
    if (ok && (js_late || host_late)) {
        ok = false;
        // 两侧都可分辨：JS 本体越线 ⇒ 无后缀；只有宿主段越线 ⇒ `(host)`。
        call_err = callbackBudgetReason(
            is_start ? "start" : "onTrigger",
            is_start ? std::string() : (ev != nullptr ? ev->name : std::string()),
            /*host=*/host_late && !js_late);
    }
    emitCallbackDiag(diag, is_start ? "start" : (ev != nullptr ? ev->name.c_str() : "onTrigger"), ok,
                     budget);
    in_callback_ = false;
    ctx_ = nullptr;

    if (!ok) {
        const std::string why =
            !call_err.empty() ? call_err
                              : (script_->overran() ? "plugin: 回调超出预算（deadline / op_limit）"
                                                    : "plugin: 回调失败");
        // ★ S0（缺陷 1）：预算超限 / JS 抛错 / 返回非法值 都走这一条 ⇒ Run 收口。
        //   `markFailed` 内部就 `releaseRunHooks()`（失败路径唯一的收口点）。
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
    // ★ S0（缺陷 1）：失败 = Run 收口 ⇒ 本 Run 的 hook 一条都不许活过这一行
    //   （覆盖 `ctx.fail` / 预算超限 / 宿主异常 / runner install 拒绝 / `script_ == nullptr`）。
    releaseRunHooks();
}

void PluginHost::report(const char* what, const std::string& text) {
    std::fprintf(stderr, "[plugin %s] %s: %s\n", name_.c_str(), what != nullptr ? what : "error",
                 oneLine(text).c_str());
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

std::uint32_t PluginHost::instrHookAdd(const std::string& script, std::uint32_t pos, const char** err) {
    const char* why = nullptr;
    // ★ S0：owner = **本 Run 的序号**（JS 面不变：`ctx.hook.at(script,pos)`）。
    const std::uint32_t handle = lift::instrHookAdd(run_serial_, script, pos, &why);
    if (err != nullptr) *err = why;
    if (handle == 0) {
        // ★ S0（缺陷 3）：`no_injection` 不是用户输入错，而是**这一趟构建没有注入站点**
        //   （`--no-s7` / 闭包没覆盖脚本派发 EA）⇒ `ctx.hook.*` 永不命中。
        //   这一条必须**响亮**（report 而不是普通 log）：否则唯一症状是"装上了、hits 永远 0"。
        if (why != nullptr && std::strcmp(why, "no_injection") == 0) {
            report("ctx.hook 不可用",
                   "no_injection：本构建在脚本派发点没有注入 lift_instr_hook"
                   "（--no-s7 或闭包没覆盖派发 EA）⇒ ctx.hook.* 永不命中");
        } else {
            log("error", std::string("ctx.hook.at 失败：") + (why != nullptr ? why : "unknown"));
        }
        return 0;
    }
    char pos_hex[16];
    std::snprintf(pos_hex, sizeof(pos_hex), "0x%X", pos);
    log("info", std::string("ctx.hook.at script=") + script + " pos=" + pos_hex + " handle=" +
                   std::to_string(handle));
    return handle;
}

bool PluginHost::instrHookSetPatch(std::uint32_t handle, bool enabled, lift::InstrPatchKind kind,
                                   std::uint32_t index, std::uint32_t value, std::uint32_t max_hits,
                                   const char** err) {
    const char* why = nullptr;
    const bool ok = lift::instrHookSetPatch(handle, enabled, kind, index, value, max_hits, &why);
    if (err != nullptr) *err = why;
    if (!ok) {
        log("error", std::string("ctx.hook.patch 失败：") + (why != nullptr ? why : "unknown"));
        return false;
    }
    if (enabled) {
        log("info", "ctx.hook.patch handle=" + std::to_string(handle) + " kind=" +
                        lift::instrPatchKindName(kind) + " index=" + std::to_string(index) +
                        " value=" + std::to_string(value));
    }
    return true;
}

bool PluginHost::instrHookSetCapture(std::uint32_t handle, const std::vector<std::uint32_t>& ints,
                                     const std::vector<std::uint32_t>& ptrs) {
    return lift::instrHookSetCapture(handle, ints, ptrs);
}

bool PluginHost::instrHookStats(std::uint32_t handle, lift::InstrHookStatus* out) {
    return lift::instrHookStats(handle, out);
}

bool PluginHost::instrHookRemove(std::uint32_t handle) { return lift::instrHookRemove(handle); }

void PluginHost::instrHookClear() { lift::instrHookClear(); }

std::vector<lift::InstrHookStatus> PluginHost::instrHookList() { return lift::instrHookList(); }

Receipt PluginHost::submitIntent(IntentKind kind, int x, int y, MouseButton button) {    Receipt receipt;
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
            std::fprintf(stderr, "[plugin %s] log: 已达上限 %llu 行（后续丢弃）\n", name_.c_str(),
                         static_cast<unsigned long long>(kLogLineLimit));
            std::fflush(stderr);
        }
        return;
    }
    if (!message.empty() && message == last_log_) return;
    last_log_ = message;
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
    // 与 `jsHostProfileEnd` 同口径：失败 ⇒ 没有报告文本（⛔ 把半截文本回给调用方）。
    if (text != nullptr) *text = ok ? local_text : std::string();
    if (!ok && err != nullptr) {
        *err = local_err.empty() ? std::string("没有打开的 profile 窗口") : local_err;
    }
    return ok;
}

bool PluginHost::profileEnd(std::string* err) {
    // ★ D-C2-4（RunCtx 的 `void` 契约）：报告只能走 **stdout**（与 `AotHost::endWindow()` 逐字同形）。
    //   ★ S2 复审 I3：`RenderCtx.profile.end()` **不**走这里 —— 它拿 `profileEndText` 的文本回 JS
    //   （HUD 要判 `INVALID` 那一个 token），那条路径不再多打一遍 stdout。
    std::string text;
    const bool ok = profileEndText(&text, err);
    if (!text.empty()) {
        std::fputs(text.c_str(), stdout);
        std::fflush(stdout);
    }
    return ok;
}

void PluginHost::fail(const std::string& name, const std::string& reason) {
    // `ctx.fail(name?, reason?)`：点名状态（name 可空）+ 原因；清 armed 由 `runCallback` 返回 {}。
    failed_ = true;
    if (!name.empty()) fail_name_ = name;
    if (!reason.empty()) fail_reason_ = reason;
    // ★ S0（缺陷 1）：`ctx.fail` 是 Run 收口 ⇒ 立刻撤本 Run 的 hook（⛔ 不留给"下一条脚本指令"）。
    releaseRunHooks();
    report("fail", (name.empty() ? std::string("(unnamed)") : name) + ": " + reason);
}

void PluginHost::finish(int exit_code, const std::string& reason) {
    // R3：**不改进程 exit**；只结束本次 Run + 记录（readout 里 `run=finished exit=N reason=…`）。
    finished_ = true;
    finish_code_ = exit_code;
    finish_reason_ = reason;
    // ★ Run 收口 ⇒ 撤掉本 Run 的脚本指令位置钩子（并把未还原的那次覆盖按 CAS 结算：净变化 0）。
    //   ⛔ 不要退回 `instrHookClear()`：它会撤掉**别人的** hook（v2 的多 Run 上就是静默越权）。
    releaseRunHooks();
}

void PluginHost::quit(const std::string& reason) {
    // D-C2-9：`stop_requested` + detail `plugin: quit`（`aot_run.cpp` 的归因位 = `stop_detail`）。
    quit_requested_ = true;
    if (!reason.empty()) quit_reason_ = reason;
    // ★ S0（缺陷 1）：quit 也是终态 ⇒ 本 Run 的 hook 立刻撤（同 `finish`）。
    releaseRunHooks();
    report("quit", quit_reason_.empty() ? std::string("(no reason)") : quit_reason_);
}

// =====================================================================================
// 收口 / readout
// =====================================================================================
std::size_t PluginHost::releaseOwner(WindowManager& wm) {
    // ★ S0（缺陷 1）："卸载 / 宿主 stop" 也是 Run 收口的一条路径 ⇒ hook 与输入一起撤。
    releaseRunHooks();
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
    // ★ F2 的 fallback：`failed_` 是本类记的（JS 失败 / `ctx.fail` / 同步过来的 runner 失败），
    //   `runner_.failed()` 是 install 判的 ⇒ **两者任一**都必须让 readout 说 `run=failed`，
    //   ⛔ 不许出现"runner 已经 failed、readout 还写 waiting"（那在生产上只能等 --time-limit）。
    const bool run_failed = failed_ || runner_.failed();
    const char* state = "waiting";
    if (run_failed) {
        state = "failed";
    } else if (quit_requested_) {
        state = "quit";
    } else if (finished_) {
        state = "finished";
    } else if (runner_.done()) {
        state = "done";
    }
    out += std::string(" run=") + state;
    // 名字 / 原因也从 runner 兜底（`syncRunnerFailure` 通常已经把 `reason` 搬过来了）。
    const std::string name_text = !fail_name_.empty() ? fail_name_ : (run_failed ? runner_.failName() : std::string());
    if (!name_text.empty()) out += " name=" + oneLine(name_text);
    if (finished_) out += " exit=" + std::to_string(finish_code_);
    if (run_failed) {
        const std::string reason =
            !fail_reason_.empty() ? fail_reason_ : (runner_.failed() ? runner_.lastError() : std::string());
        if (!reason.empty()) out += " reason=" + oneLine(reason);
    } else if (quit_requested_) {
        if (!quit_reason_.empty()) out += " reason=" + oneLine(quit_reason_);
    } else if (finished_ && !finish_reason_.empty()) {
        out += " reason=" + oneLine(finish_reason_);
    }
    return out;
}

}  // namespace lift::plugin
