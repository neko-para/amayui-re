// emulator-lift · QuickJS 后端（A：QuickJS 内核）
//
// ★ `#include "quickjs.h"` **只**出现在私有头 `ui/quickjs_value.h` 里；本文件通过它拿到 C API，
//   并且**所有拥有所有权的 JSValue 局部一律是 `qjs::Value`**（用户 2026-11-18 裁决）：
//   一个 JSValue 一个所有者、拷贝 = JS_DupValue、move 置 undefined、析构 = JS_FreeValue。
//   ⛔ 本文件里不再出现"裸 JSValue 局部 + 手工 JS_FreeValue"（`qjs::Value` 实现内部除外）。
//
// 一、沙箱形状（context §2.3 的**实测反转**，与 `overlay-quickjs.md` §2.1 相反，以 context 为准）
//   1. `JS_NewContextRaw` + `JS_AddIntrinsicBaseObjects` 之后，**宿主**调 `JS_Eval` 会失败
//      （`TypeError: eval is not supported`）：`JS_Eval` 走 `JS_EvalInternal`，它要求
//      `ctx->eval_internal` 非空，而该字段**只由** `JS_AddIntrinsicEval` 设置（0.18.0
//      `quickjs.c` 的 `JS_AddIntrinsicEval` 在 `#ifndef QJS_DISABLE_PARSER` 里赋
//      `ctx->eval_internal = __JS_EvalInternal`，然后 `return 0`）。
//   2. ⇒ 必须 `JS_AddIntrinsicEval`（只为让**宿主**能编译脚本），随后把脚本可达的编译入口删掉：
//      `globalThis.eval` / `globalThis.Function` / `Function.prototype.constructor`。
//   3. ⛔ 只删 `globalThis.Function` **不够**：`(function(){}).constructor` 会从
//      `Function.prototype.constructor` 拿到同一个构造器，`('return 1')()` 即可重新编译源码。
//      ★ 删掉后 `(function(){}).constructor` **不是 undefined**（它会沿原型链落到
//      `Object.prototype.constructor` = `Object`，本文件不删它）；被钉住的是**行为**：
//      拿到的任何 `.constructor` 都不能把字符串编译成函数（调用 ⇒ TypeError）。
//   4. `Math.random` 也删（种子 = 宿主时间 ⇒ 不可复现）。`Date` / `RegExp` / `JSON` / `Proxy` /
//      `WeakRef` / TypedArrays / `performance` **没加**（`JS_NewContextRaw` 不装它们，本文件只加
//      BaseObjects + Eval + Promise）。
//      ★ 0.18.0 起 `BigInt` **不算"没加"**：`JS_AddIntrinsicBaseObjects` 自己会装它
//      （`quickjs.c:59170`）⇒ `typeof BigInt === "function"`、`1n` 字面量可用。
//   5. ★ `Promise` **加了、不删**（用户裁决 2026-11-19，反转切片的"先加后删"）：module 求值返回
//      Promise（本文件第五节），而且沙箱**不是安全边界**（用户裁决 2026-11-18：JS 没有宿主权限 ——
//      能力只来自注入的 `ui`/`game`，信任脚本）⇒ 先加再删只是"以后需要又得改回来"的额外特殊处理。
//      保留的删除只有**功能目的**：`Math.random`（确定性 / replay）、`eval` / `Function` /
//      `Function.prototype.constructor`（禁动态编译）。⛔ 不追删其它内建。
//      ⇒ `typeof Promise === "function"` 是新的**表面事实**（`tests/ui_script_quickjs.cpp` 记它）。
//
// 二、预算（context §5）
//   · 内存 16 MiB / 栈 512 KiB / 中断回调每 ~10000 条字节码一次（`JS_INTERRUPT_COUNTER_INIT`）。
//   · ★ **插件回调的 2 ms 用本线程 CPU 钟**（`pluginThreadCpuNs()`，2026-11-21 用户裁决）：
//     起点 = `call()` 里 `JS_Call` 之前，判据 = 中断回调拿**同一把 CPU 钟**比 —— 被 OS 抢占
//     不计（缺陷 `REQ-01M7VVDV69MJXRHNDGR50H7VHC`）。render 的 20 ms 仍用 `steadyNowNs()` 墙钟。
//   · ★ 超预算的报错不是我们抛的：0.18.0 的 `JS_ThrowInterrupted` 是 `quickjs.c` 内的 **static**
//     （`quickjs.h` 未导出）；上游约定是**中断回调返非 0** 时由引擎自己
//     `JS_ThrowInternalError("interrupted")` + `JS_SetUncatchableError`（`__js_poll_interrupts`）。
//     ⇒ 本文件的回调只做"查 deadline / op_limit，超了返 1"，得到的就是合同要的不可 catch 错误。
//   · op_limit 以"中断回调次数 × 10000"计（引擎不暴露精确字节码计数）：实际执行量落在
//     `[op_limit, op_limit + 10000)` 条之间 —— 这是**预算**，不是精确计量。
//
// 三、失败与异常（context §3 / §5）
//   · `JS_Eval` / `JS_Call` 返回 `JS_EXCEPTION` ⇒ **必须**取走（`qjs::takeException` 读 message +
//     stack），否则脏异常留在 ctx 上，下一次调用会读到上一次的异常。
//   · err 形状 = `<origin>: <message>\n<stack>`；`stack` 由引擎给 `<origin>:<line>:<col>`
//     （语法错与运行期错都带调用点）。同实例的下一次 `render` 必须在取走异常后照常工作。
//   · 宿主 C 函数不接 C++ 异常：`catch (...)` → `JS_ThrowTypeError` + 返 `JS_EXCEPTION`。
//
// 四、生命周期：`render` 的句柄在 `load` 时由 `qjs::Value` 持有；`shutdown` 先释放它（与 `game`），
//   再 `JS_FreeContext` / `JS_FreeRuntime`。`JS_DUMP_LEAKS` 只在 `#ifndef NDEBUG` 打开。
//
// 五、ES module（`REQ-01M7QF01A00F9946DX0GPH23JQ` 切片 2 前半）
//   · **检测**：先按 classic 试编译（`COMPILE_ONLY`，不执行）；能编 ⇒ **classic 路径原样**。
//     classic 编不过时才问 `JS_DetectModule`（它把源码当 module 编一次，成功或"could not load
//     module"都算 module）⇒ module 路径。★ 为什么不是"只看 `JS_DetectModule`"：0.18.0 的实现
//     对**普通 classic 脚本也返 true**（`quickjs.c:64939` 先置 `is_module = true`），只用它会把
//     顶层 `function render(){}` 变成 module 作用域（不再挂 globalThis）⇒ 旧 UiScript（内部 hud 回退）语义变了。
//   · **解析**：module 名 = 入口 `origin`（内部 hud 回退脚本 / 默认 hud 的**真实文件路径**）；相对
//     specifier 按**导入方的模块名**做**纯文本**归一化（`.`/`..`、`/`·`\` 都认），再用
//     "入口 origin 的目录 = UI 根"做前缀检查。缓存 = 引擎的 `ctx->loaded_modules`（按归一化名），
//     所以同一文件被两条路径导入只求值一次，且我们的归一化必须**确定性**（否则缓存失效）。
//   · **拒绝**（`moduleNormalize` 抛 SyntaxError）：bare specifier / 绝对路径 / `data:` 等协议 /
//     `..` 逃出 UI 根 / 入口不是文件路径。err 形状仍是 `<origin>: <message>\n    at <file>:<line>:<col>`
//     —— 定位由**扫描导入方源码里的 specifier 字面量**得到（引擎的 resolve 期错误没有 JS 栈）。
//   · **求值 / Promise**：`JS_Eval(MODULE)` 返回 **Promise**（module 顶层 await 也是它）。必须
//     `JS_ExecutePendingJob` drain 到无 job，然后查 `JS_PromiseState`：REJECTED ⇒ 把 rejection 的
//     reason 按同一 err 形状报出来（⛔ 不许静默吞）；PENDING ⇒ 顶层 await 没有可执行来源，报错。
//     成功路径不留 job / 不持有 promise（值走 `qjs::Value`）。
//   · ★ module loader **只在 module 路径**装：classic 脚本仍没有 `import()`（旧行为不变）。
//   · ★ 命中 / 捕获（切片 2 后半）：`ui.rect({key})` 把 key 挂进 `Element`（命中区 = resolved
//     rect，`ui/hit.h`）；`game.click`（一次性 `{key,x,y}`）/ `game.hover` / `game.active` 是三个
//     只读 getter，读宿主每帧注入的快照。⛔ 本文件不实现 `kit.js`（JS 组件库）与剪贴板 / profile
//     接线（切片 3）。
#include "ui/script_quickjs.h"

#include "ui/quickjs_value.h"  // ★ 唯一 include quickjs.h 的头（本文件是它唯一的消费者）

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "runtime/budget.h"          // Budget（插件回调的预算形状；⛔ 不复述 2 ms / 100k）
#include "runtime/trigger.h"         // Trigger / TriggerEvent / TriggerReadings（插件面）
#include "runtime/vfs.h"             // openHostFileUtf8（module 文件读取走 UTF-8 宿主路径）
#include "ui/plugin.h"               // PluginScript / PluginCallHost（语言无关接口）

#include "runtime/font.h"
#include "ui/element.h"
#include "ui/raster.h"

namespace lift::ui {
namespace {

using qjs::Value;

// =======================================================================================
// 预算常量（context §5；数字写死，⛔ 不做 CLI 旋钮）
// =======================================================================================
constexpr std::size_t kMemoryLimitBytes = 16u << 20;  // 16 MiB
constexpr std::size_t kStackLimitBytes = 512u << 10;  // 512 KiB
// module 求值 / shutdown 的 microtask 上限（防脚本自循环把 load / 关闭挂死）。drain 语义见第五节。
constexpr int kMaxPendingJobs = 100000;
// 引擎每 `JS_INTERRUPT_COUNTER_INIT` 条字节码调一次回调（0.18.0 `quickjs.c:558`，值仍是 10000）；
// op_limit 以它为步长。
constexpr std::uint64_t kOpcodesPerInterrupt = 10000;

std::uint64_t steadyNowNs() {
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(ns.count());
}

struct Impl;

Impl* implOf(JSContext* ctx) { return static_cast<Impl*>(JS_GetContextOpaque(ctx)); }

std::string toUtf8(JSContext* ctx, JSValueConst v) {
    std::string out;
    if (!qjs::toString(ctx, v, &out)) {
        qjs::clearException(ctx);
        return {};
    }
    return out;
}

// =======================================================================================
// Impl：一个脚本实例的全部状态（JS 值 + 本帧注入的读数 + 预算）。
// ★ 持有者 = `QuickJsScript`（`UiScript` 的实现），构造后不再移动 ⇒ 中断回调的 opaque 稳定。
// =======================================================================================
// ---- 插件（C2a）：JS 侧 trigger 对象 ↔ 宿主 `TriggerState` 的**对象表** ----
// ★ 为什么需要它（合同 §4.2 417–419 / 测绘 §6-⑧）：`maxFires` 属于**对象**、跨 arm 累计 ⇒
//   "同一个 JS 对象"必须映射到**同一个** `shared_ptr<TriggerState>`；"新对象"才是新预算。
//   ⇒ JS 对象只带一个 id（`TriggerRef`，`JS_GetOpaque`），真值在表里。
// ★ **有界**：`refs` 计的是"还有几个 JS 对象指向这个 id"；最后一个被 GC 时抹掉整条
//   （`triggerFinalizer`）。⇒ "每次 fire 新建 trigger"的插件（`examples/rearm.ts` 那种）不会线性涨内存。
//   ⛔ 不要改成"只追加不释放"（那正是测绘 §6-⑧ 点名的慢性泄漏）；也⛔ 不要改成"只留一个强引用"
//   （那会让事件对象与插件手里那个 `===` 不等，rearm 语义随实现漂）。
struct TriggerEntry {
    lift::plugin::Trigger trigger;
    int refs = 0;  // 指向同一 id 的活 JS 对象数
};

struct TriggerRef {
    Impl* impl = nullptr;
    std::uint32_t id = 0;
};

struct Impl {
    JSRuntime* rt = nullptr;
    JSContext* ctx = nullptr;
    Value render;        // globalThis.render（持有引用）
    Value game_obj;      // 全局 `game` 的 getter 返回它（⛔ 每帧不重建）
    std::string origin;  // 错误里的文件名（hud 回退脚本路径 / `builtin:hud`）

    // ---- ES module（只在 module 路径启用；classic 路径连 loader 都不装 ⇒ 旧语义不变）----
    std::string module_root;                           // UI 根 = 入口 origin 的目录（归一化）
    bool module_entry_is_path = false;                 // origin 是文件路径（而不是 `builtin:hud`）
    std::map<std::string, std::string> module_sources;    // 归一化 module 名 → 源码（只为错误定位）
    std::map<std::string, std::string> module_import_at;  // 归一化 module 名 → 首次导入点
    std::string module_error_at;                          // resolve 期拒绝的 `<file>:<line>:<col>`

    // ---- 插件（C2a；只有 `--plugin` 路径填）----
    Value plugin_default;    // `export default {…}`（持有；`initialState` 也从它读）
    Value plugin_render;     // default.render（C2b 才调用；C2a 只回答 `hasRender()`）
    Value plugin_start;      // default.start
    Value plugin_on_trigger; // default.onTrigger
    Value plugin_state;      // `ctx.state`（per-Run；第一次回调前建）
    Value state_seed;        // `initialState` 的**原值**（拷贝在 `makeStateObject` 里做）
    bool has_state_seed = false;
    Value plugin_params;     // `ctx.params`（装载期建；属性只读）
    Value ui_data;           // `ctx.ui.state` 的数据面（宿主本地 JS 状态；⛔ 不进 guest）
    bool plugin_has_render = false;
    bool plugin_has_run = false;
    std::map<std::uint32_t, TriggerEntry> triggers;  // id → (trigger, refs)
    std::uint32_t next_trigger_id = 1;               // 0 = "没分配"
    JSClassID trigger_class = 0;                     // 本 runtime 的 JS class（finalizer 用）
    std::set<std::string> ui_visible;                // `ctx.ui.visible(id)` 的宿主侧集合
    lift::ui::PluginCallHost* call_host = nullptr;   // 只在回调期间非空（`ctx.*` 的来路）
    // ---- 插件（C2b）：render 的 `ctx.read` / `ctx.log` 来路 ----
    // ★ **非拥有**；由 `PluginScript::setCallHost` 在 init 时设（生产 = `PluginHost`）。
    //   render 期间它临时成为 `call_host`（回调外没有别的 ctx）；回调期间由 `call()` 的形参覆盖。
    lift::ui::PluginCallHost* call_host_persistent = nullptr;
    // ---- 渲染面的 config 桥（D2，2026-11-25）----
    // ★ `render_bridge` = 本帧 render 用的桥（**非拥有**）。为什么留到 render 返回之后：
    //   `UiScript::flushConfig` 在 present_sdl 的"render 之后"调，那时 `current` / `call_host`
    //   都已经复位 ⇒ 桥必须在这里缓存（⛔ 不靠 `current` 再解析一次）。
    //   nullptr = 这一帧没有桥 ⇒ flush 是 no-op。
    // ★ 消费者 = **两条渲染面**：经典回退 HUD 的 `host.config` 与 `RenderCtx.config`（2026-11-26
    //   加回；父级裁决：它挡住 `--overlay` 下最基本的设置页 / 预配置）。两者**同一份**
    //   `makeConfigObject(impl, /*render=*/true)` ⇒ 同一套校验 / 转换 / 回执。
    lift::plugin::ConfigBridge* render_bridge = nullptr;

    // 本帧注入（getter 读它们；render 结束一律复位）
    UiBuilder* builder = nullptr;
    const UiFrameReadings* current = nullptr;
    FontEngine* font = nullptr;

    // 预算（`setBudget` 写入；interrupt 回调读）。★ 两把钟**分开存**（⛔ 混用会立刻误判）：
    //   · `deadline_ns` = **墙钟**绝对 deadline —— `UiScript::setBudget`（render）的口径，`steadyNowNs()`；
    //   · `cpu_deadline_ns` = **CPU** 绝对 deadline —— 插件回调的 2 ms（2026-11-21 用户裁决），
    //     `pluginThreadCpuNs()`；只在 `call()` 的 JS 体窗口内非 0，回调结束即清 0。
    std::uint64_t deadline_ns = 0;
    std::uint64_t cpu_deadline_ns = 0;
    // ★ **宿主调用暂停**（2026-11-25，`REQ-01M86VRT1YP53CJS0M885FCCJM`）：`ctx.profile.end` 这类
    //   **宿主能力**在用户 JS 本体里被调用，它们自己的 CPU（生成报告 + 写剪贴板）**不计入** 2 ms ——
    //   与"模块装载 / ctx 构造不计"同一原则（`runtime/budget.h` 头注）。每次 `call()` 归零；
    //   宿主调用前后各取 `pluginThreadCpuNs()`，差值累加到这里（`HostPause`）。
    //   ⛔ 只减**宿主段**：用户 JS 自己的时间一分不减（中断回调也要减同一个数，见 `interruptHandler`）。
    std::uint64_t paused_ns = 0;
    std::uint32_t op_limit = 0;
    std::uint64_t ops = 0;  // 已执行的"中断步长"数（× 10000 = 近似字节码条数）
    bool overran = false;   // 本帧是否被预算打断（`render` false 的原因之一）

    // ---- 回调耗时诊断（REQ-01M7VVDV69MJXRHNDGR50H7VHC 待查 #3；⛔ 默认不读、不进 --help）----
    std::uint64_t module_load_ns = 0;  // 模块装载（编译 + resolve + 求值 + drain）；**在预算起点之前**

    ~Impl() { shutdown(); }

    // 幂等：释放持有的 JS 值 → 释放 context → 释放 runtime。
    void shutdown() {
        if (ctx != nullptr) {
            // ★ microtask 必须在 `JS_FreeContext` 之前清空：module 求值已由 load drain；这里兜住
            //   render 期触发的动态 `import()` 一类残余 job（⛔ 不把 pending job 留给
            //   `JS_FreeRuntime`；0.18.0 实测不 assert，但"没有 pending job"才是显式不变量）。
            for (int i = 0; i < kMaxPendingJobs; ++i) {
                JSContext* job_ctx = nullptr;
                if (JS_ExecutePendingJob(rt, &job_ctx) == 0) break;
            }
            qjs::clearException(ctx);
            // ★ 先释放值、再忘掉 ctx：`Value` 析构时不能碰到已经被 Free 的 ctx。
            render.invalidate();
            game_obj.invalidate();
            // 插件面（C2a）：与上面同一时刻、同一个理由（顺序无关，全部在 FreeContext 之前）。
            plugin_default.invalidate();
            plugin_render.invalidate();
            plugin_start.invalidate();
            plugin_on_trigger.invalidate();
            plugin_state.invalidate();
            state_seed.invalidate();
            plugin_params.invalidate();
            ui_data.invalidate();
            JS_FreeContext(ctx);
            ctx = nullptr;
        }
        if (rt != nullptr) {
            JS_FreeRuntime(rt);
            rt = nullptr;
        }
        builder = nullptr;
        current = nullptr;
        call_host = nullptr;
        call_host_persistent = nullptr;
        render_bridge = nullptr;
        triggers.clear();
        ui_visible.clear();
        trigger_class = 0;
    }
};

// ---------------------------------------------------------------------------------------
// **宿主调用暂停**（RAII）：一个宿主能力的调用把本线程 CPU 时间加进 `Impl::paused_ns`。
//
// ★ 口径（`runtime/budget.h` 头注 / 用户裁决 2026-11-25）：2 ms 只罩**用户 JS 本体** ⇒ 用户 JS
//   **调用宿主**，宿主替它干活的那段 CPU（`ctx.profile.end` 生成报告 + 写剪贴板；以后同形的
//   宿主能力按同一原则接进来）不算用户 JS 的时间。⛔ 这不是"放宽 2 ms"：真正的 JS 忙循环
//   （不调宿主）一分不少地记在预算里（`plugin/callback-js-band` / `plugin/callback-cpu-budget`）。
// ★ 为什么必须在**中断回调里也减同一个数**：宿主调用期间没有字节码在跑 ⇒ 中断回调不会被调；
//   但宿主返回之后 `pluginThreadCpuNs()` 已经涨了 2.5 ms ⇒ 不减就会在下一次中断**误判**超时。
//   两处用的是同一个 `paused_ns`，所以"宿主段不计"是**一个**口径，不是两处各算一次。
// ★ 用 RAII 而不是手工成对：`ctx.*` 的宿主调用会 `throw`（`catch (...)` 在 trampoline 上），
//   手工写法在异常路径上会漏减 ⇒ 那次回调的宿主成本又算回用户 JS 头上。
// ---------------------------------------------------------------------------------------
struct HostPause {
    Impl* impl = nullptr;
    std::uint64_t start_ns = 0;
    bool active = true;
    explicit HostPause(Impl* i) : impl(i), start_ns(lift::plugin::pluginThreadCpuNs()) {}
    ~HostPause() { stop(); }
    // 中途让出（"这段其实会跑用户 JS"）：累计已 pause 的部分，之后 resume 重新起算。
    void stop() {
        if (!active) return;
        active = false;
        const std::uint64_t end_ns = lift::plugin::pluginThreadCpuNs();
        if (impl != nullptr && end_ns >= start_ns) impl->paused_ns += end_ns - start_ns;
    }
    void resume() {
        if (active) return;
        active = true;
        start_ns = lift::plugin::pluginThreadCpuNs();
    }
    HostPause(const HostPause&) = delete;
    HostPause& operator=(const HostPause&) = delete;
};

// ---------------------------------------------------------------------------------------
// 回调耗时诊断（`REQ-01M7VVDV69MJXRHNDGR50H7VHC` 待查 #3）
//
// ★ 本文件只**量**并填 `callbackDiag()`；**输出**在 `plugin_host.cpp` 的 `runCallback`
//   —— 那里量的 `host_ns` 是宿主段窗口，判据用的 `js_cpu_ns`（**CPU 钟**）是本文件切出来的
//   **用户 JS 本体**；`js_ns`（墙钟）只是对照读数（wall 大 / cpu 小 = 被 OS 抢占）。
// ★ 预算起点在 `call()` 的 `js_start_cpu_ns`（JS_Call 之前，`pluginThreadCpuNs()`）；
//   `inner_ns` 仍从进入 `call()` 量（build + js + collect），它只是读数、⛔ 不是预算窗。
// ---------------------------------------------------------------------------------------

// ---------------------------------------------------------------------------------------
// 中断回调：每 ~10000 条字节码一次。返非 0 ⇒ 引擎自己抛 "interrupted" 并置不可 catch
// （见文件头第二节；`JS_ThrowInterrupted` 在 0.18.0 仍是 static，宿主无法直接调）。
// ★ 0.18.0 的签名变更（release notes PR #1802）：`JSInterruptHandler` 的**第一个参数从
//   `JSRuntime*` 改成 `JSContext*`**（`quickjs.h:1192`，`JS_SetInterruptHandler` 在 `:1193`）。
//   我们只需要那个不透明的 `Impl*`，它由 `JS_SetInterruptHandler` 原样传回（第三个参数），
//   所以回调体一行不改 —— 语义（deadline / op_limit / overran）完全不变。
// ---------------------------------------------------------------------------------------
int interruptHandler(JSContext* /*ctx*/, void* opaque) {
    Impl* impl = static_cast<Impl*>(opaque);
    if (impl == nullptr) return 0;
    impl->ops += kOpcodesPerInterrupt;
    // ★ 两把钟各判各的（⛔ 不要用同一把 now 去比两个 deadline）：
    //   · `deadline_ns` = render 的**墙钟** deadline（`UiScript::setBudget`，`present_sdl` 给）；
    //   · `cpu_deadline_ns` = 插件回调的 **CPU** deadline（`call()` 里从 `pluginThreadCpuNs()` 起算）。
    //   2026-11-21：2 ms 改用 CPU 钟（缺陷 `REQ-01M7VVDV69MJXRHNDGR50H7VHC`）⇒ 若这里仍拿
    //   `steadyNowNs()` 去比 CPU 起点算出的 deadline，第一次中断就会误判（CPU 读数比墙钟小几个数量级）。
    if (impl->deadline_ns != 0 && steadyNowNs() >= impl->deadline_ns) {
        impl->overran = true;
        return 1;
    }
    if (impl->cpu_deadline_ns != 0) {
        // ★ 扣除**宿主调用暂停**（`Impl::paused_ns`；`ctx.profile.end` 一类宿主能力）：
        //   deadline = `js_start_cpu_ns + budget`，而 `paused_ns` 是从同一把 CPU 钟量出来的
        //   ⇒ 比较 `(now - paused) >= deadline` 等价于"把 deadline 按暂停顺延"。
        //   ⛔ 不减用户 JS 自己的时间：`paused_ns` 只在 `HostPause` 的构造→析构之间增长。
        const std::uint64_t cpu_now_ns = lift::plugin::pluginThreadCpuNs();
        const std::uint64_t js_cpu_now_ns =
            cpu_now_ns >= impl->paused_ns ? cpu_now_ns - impl->paused_ns : 0;
        if (js_cpu_now_ns >= impl->cpu_deadline_ns) {
            impl->overran = true;
            return 1;
        }
    }
    if (impl->op_limit != 0 && impl->ops >= impl->op_limit) {
        impl->overran = true;
        return 1;
    }
    return 0;
}

// =======================================================================================
// 宿主侧小工具（每个都查返回码；失败 = 返回 false + 填 err，且 ctx 上的异常已清）
// =======================================================================================
bool setProperty(JSContext* ctx, JSValueConst obj, const char* name, Value value, std::string* err) {
    if (!qjs::setProperty(ctx, obj, name, std::move(value))) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 写属性 ") + name + " 失败";
        return false;
    }
    return true;
}

// 删除属性。★ false 覆盖两种情况：删除抛错（<0）与"删不掉"（0，属性不可配置）。
// 「删不掉」也算失败 —— 沙箱的每一个删除都是**承重**的，⛔ 不能静默留下一个入口。
bool deleteProperty(JSContext* ctx, JSValueConst obj, const char* name, std::string* err) {
    const JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 无法创建属性名 ") + name;
        return false;
    }
    const int rc = JS_DeleteProperty(ctx, obj, atom, 0);
    JS_FreeAtom(ctx, atom);
    if (rc < 0) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 删除属性 ") + name + " 抛错";
        return false;
    }
    if (rc == 0) {
        if (err != nullptr) *err = std::string("quickjs: 属性 ") + name + " 删不掉（沙箱安装失败）";
        return false;
    }
    return true;
}

// 静态 C trampoline 注册（⛔ 不用 JS_NewCFunctionData 塞 std::function：那会把 C++ 生命周期
// 带进引擎的 func_data）。函数体从 `JS_GetContextOpaque` 拿 Impl。
bool setFunction(JSContext* ctx, JSValueConst obj, const char* name, JSCFunction* fn, int length,
                 std::string* err) {
    Value value(ctx, JS_NewCFunction(ctx, fn, name, length));
    if (value.isException()) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 创建函数 ") + name + " 失败";
        return false;
    }
    return setProperty(ctx, obj, name, std::move(value), err);
}

// =======================================================================================
// 沙箱安装（context §2.3 / §5）
// =======================================================================================
bool installSandbox(Impl& impl, std::string* err) {
    JSContext* ctx = impl.ctx;
    Value global = qjs::getGlobalObject(ctx);
    if (global.isException()) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = "quickjs: 取 globalThis 失败";
        return false;
    }
    bool ok = true;
    Value function_ctor = qjs::getProperty(ctx, global.raw(), "Function");
    Value function_proto = Value::undefined(ctx);
    if (function_ctor.isException()) {
        qjs::clearException(ctx);
        ok = false;
        if (err != nullptr) *err = "quickjs: 取 Function 失败（沙箱安装失败）";
    } else if (function_ctor.isObject()) {
        function_proto = qjs::getProperty(ctx, function_ctor.raw(), "prototype");
        if (function_proto.isException()) {
            qjs::clearException(ctx);
            function_proto = Value::undefined(ctx);
        }
        if (!function_proto.isObject()) {
            ok = false;
            if (err != nullptr) *err = "quickjs: 取 Function.prototype 失败（沙箱安装失败）";
        }
    } else {
        ok = false;
        if (err != nullptr) *err = "quickjs: BaseObjects 没有装 Function（沙箱安装失败）";
    }
    if (ok) {
        // ★ 只删 globalThis.Function 不够：`.constructor` 仍能拿到同一个构造器（文件头 §1.3）。
        //   变异条目 `ui-script-quickjs-fn-ctor` 就施加在这一行（删掉它 ⇒ 测试红）。
        if (!deleteProperty(ctx, function_proto.raw(), "constructor", err)) ok = false;
    }
    if (ok) ok = deleteProperty(ctx, global.raw(), "Function", err);
    if (ok) ok = deleteProperty(ctx, global.raw(), "eval", err);
    if (ok) {
        Value math = qjs::getProperty(ctx, global.raw(), "Math");
        if (math.isException()) {
            qjs::clearException(ctx);
            ok = false;
            if (err != nullptr) *err = "quickjs: 取 Math 失败（沙箱安装失败）";
        } else if (!math.isObject()) {
            ok = false;
            if (err != nullptr) *err = "quickjs: BaseObjects 没有装 Math（沙箱安装失败）";
        } else {
            ok = deleteProperty(ctx, math.raw(), "random", err);
        }
    }
    return ok;
}

// =======================================================================================
// ui.* 的参数解析（context §4）
// =======================================================================================
// `#RRGGBB` / `#RRGGBBAA`；其他 ⇒ false（调用方转 TypeError）。undefined ⇒ 默认色。
bool parseColor(JSContext* ctx, JSValueConst v, const Color& dflt, Color* out, std::string* why) {
    if (JS_IsUndefined(v)) {
        *out = dflt;
        return true;
    }
    if (!JS_IsString(v)) {
        *why = "颜色必须是 '#RRGGBB' / '#RRGGBBAA' 字符串";
        return false;
    }
    const std::string text = toUtf8(ctx, v);
    if (text.size() != 7 && text.size() != 9) {
        *why = "颜色必须是 '#RRGGBB' / '#RRGGBBAA'（含 # 共 7 或 9 字符）";
        return false;
    }
    if (text[0] != '#') {
        *why = "颜色必须以 # 开头";
        return false;
    }
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int bytes[4] = {0, 0, 0, 255};
    for (int i = 0; i < static_cast<int>(text.size() / 2); ++i) {
        const int hi = hex(text[1 + static_cast<std::size_t>(i) * 2]);
        const int lo = hex(text[2 + static_cast<std::size_t>(i) * 2]);
        if (hi < 0 || lo < 0) {
            *why = "颜色里有非十六进制字符";
            return false;
        }
        bytes[i] = (hi << 4) | lo;
    }
    *out = Color{static_cast<std::uint8_t>(bytes[0]), static_cast<std::uint8_t>(bytes[1]),
                 static_cast<std::uint8_t>(bytes[2]), static_cast<std::uint8_t>(bytes[3])};
    return true;
}

// 读一个整数属性：undefined ⇒ required ? 失败 : 默认值；非数字 / 非有限 ⇒ 失败。
// ★ 非有限必须显式拒绝：`JS_ToInt32(NaN) == 0`（ECMAScript ToInt32 的定义），静默变 0 是错。
bool readInt32(JSContext* ctx, JSValueConst obj, const char* name, bool required, std::int32_t dflt,
               std::int32_t* out, std::string* why) {
    Value v = qjs::getProperty(ctx, obj, name);
    if (v.isException()) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 读取失败";
        return false;
    }
    if (v.isUndefined()) {
        if (required) {
            *why = std::string(name) + " 缺失（必须是数字）";
            return false;
        }
        *out = dflt;
        return true;
    }
    if (!v.isNumber()) {
        *why = std::string(name) + " 必须是数字";
        return false;
    }
    double d = 0.0;
    if (!qjs::toDouble(ctx, v.raw(), &d) || !std::isfinite(d)) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 必须是有限数字";
        return false;
    }
    std::int32_t i = 0;
    if (!qjs::toInt32(ctx, v.raw(), &i)) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 无法转成 32 位整数";
        return false;
    }
    *out = i;
    return true;
}

bool readBool(JSContext* ctx, JSValueConst obj, const char* name, bool dflt, bool* out, std::string* why) {
    Value v = qjs::getProperty(ctx, obj, name);
    if (v.isException()) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 读取失败";
        return false;
    }
    if (v.isUndefined()) {
        *out = dflt;
        return true;
    }
    if (!qjs::toBool(ctx, v.raw(), out)) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 无法转成布尔";
        return false;
    }
    return true;
}

bool readText(JSContext* ctx, JSValueConst obj, const char* name, std::string* out, std::string* why) {
    Value v = qjs::getProperty(ctx, obj, name);
    if (v.isException()) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 读取失败";
        return false;
    }
    if (v.isUndefined()) {
        *why = std::string(name) + " 缺失（必须能转成 UTF-8 字符串）";
        return false;
    }
    if (!qjs::toString(ctx, v.raw(), out)) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 无法转成 UTF-8 字符串";
        return false;
    }
    return true;
}

// 可选文本（`ui.rect` 的 `key`）：缺省 / `undefined` ⇒ 空串（= 不可命中）；其余同 `readText`。
bool readOptionalText(JSContext* ctx, JSValueConst obj, const char* name, std::string* out, std::string* why) {
    Value v = qjs::getProperty(ctx, obj, name);
    if (v.isException()) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 读取失败";
        return false;
    }
    if (v.isUndefined() || v.isNull()) {
        out->clear();
        return true;
    }
    if (!qjs::toString(ctx, v.raw(), out)) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 无法转成 UTF-8 字符串";
        return false;
    }
    return true;
}

// =======================================================================================
// 宿主函数：ui.rect / ui.text / ui.measure（三个 static C trampoline，全部在边界上 catch(...)）
// =======================================================================================
JSValue jsUiRect(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->builder == nullptr) {
            return JS_ThrowTypeError(ctx, "ui.rect: 只能在 render() 内调用");
        }
        if (argc < 1 || !JS_IsObject(argv[0])) {
            return JS_ThrowTypeError(ctx, "ui.rect: 参数必须是一个对象");
        }
        const JSValueConst arg = argv[0];
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t w = 0;
        std::int32_t h = 0;
        std::int32_t z = 0;
        Color fill;
        std::string key;
        std::string why;
        bool ok = readInt32(ctx, arg, "x", true, 0, &x, &why);
        if (ok) ok = readInt32(ctx, arg, "y", true, 0, &y, &why);
        if (ok) ok = readInt32(ctx, arg, "w", true, 0, &w, &why);
        if (ok) ok = readInt32(ctx, arg, "h", true, 0, &h, &why);
        if (ok) ok = readInt32(ctx, arg, "z", false, 0, &z, &why);
        // ★ 命中键（切片 2 后半）：缺省 = 空 = 不可命中。字符串原样进 Element.key，命中区 =
        //   本元素 resolved rect（`ui/hit.h`）。⛔ 这里不做按钮：外观 / 状态在 `scripts/ui/kit.js`。
        if (ok) ok = readOptionalText(ctx, arg, "key", &key, &why);
        if (ok) {
            Value fill_v = qjs::getProperty(ctx, arg, "fill");
            if (fill_v.isException()) {
                qjs::clearException(ctx);
                why = "fill 读取失败";
                ok = false;
            } else {
                ok = parseColor(ctx, fill_v.raw(), Color{0, 0, 0, 255}, &fill, &why);
            }
        }
        if (!ok) return JS_ThrowTypeError(ctx, "ui.rect: %s", why.c_str());
        Element& element = impl->builder->rect(x, y, w, h, fill, z);
        element.key = std::move(key);
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ui.rect: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsUiText(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->builder == nullptr) {
            return JS_ThrowTypeError(ctx, "ui.text: 只能在 render() 内调用");
        }
        if (argc < 1 || !JS_IsObject(argv[0])) {
            return JS_ThrowTypeError(ctx, "ui.text: 参数必须是一个对象");
        }
        const JSValueConst arg = argv[0];
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t size = 16;
        std::int32_t z = 0;
        bool bold = false;
        std::string text;
        Color color;
        std::string why;
        bool ok = readInt32(ctx, arg, "x", true, 0, &x, &why);
        if (ok) ok = readInt32(ctx, arg, "y", true, 0, &y, &why);
        if (ok) ok = readText(ctx, arg, "text", &text, &why);
        if (ok) ok = readInt32(ctx, arg, "size", false, 16, &size, &why);
        if (ok) ok = readInt32(ctx, arg, "z", false, 0, &z, &why);
        if (ok) ok = readBool(ctx, arg, "bold", false, &bold, &why);
        if (ok) {
            Value color_v = qjs::getProperty(ctx, arg, "color");
            if (color_v.isException()) {
                qjs::clearException(ctx);
                why = "color 读取失败";
                ok = false;
            } else {
                ok = parseColor(ctx, color_v.raw(), Color{255, 255, 255, 255}, &color, &why);
            }
        }
        if (!ok) return JS_ThrowTypeError(ctx, "ui.text: %s", why.c_str());
        impl->builder->text(x, y, std::move(text), size, color, bold, z);
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ui.text: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsUiMeasure(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr) {
            return JS_ThrowTypeError(ctx, "ui.measure: 宿主未初始化");
        }
        if (impl->font == nullptr) {
            // context §4：font == nullptr ⇒ TypeError（⛔ 不返回 0 尺寸冒充可用）。
            return JS_ThrowTypeError(ctx, "ui.measure: 宿主字体不可用（font == nullptr）");
        }
        if (argc < 2) return JS_ThrowTypeError(ctx, "ui.measure: 需要 (text, size)");
        if (!JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "ui.measure: text 必须是字符串");
        if (!JS_IsNumber(argv[1])) return JS_ThrowTypeError(ctx, "ui.measure: size 必须是数字");
        std::string text;
        if (!qjs::toString(ctx, argv[0], &text)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ui.measure: text 无法转成 UTF-8 字符串");
        }
        double size_d = 0.0;
        if (!qjs::toDouble(ctx, argv[1], &size_d) || !std::isfinite(size_d)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ui.measure: size 必须是有限数字");
        }
        std::int32_t size = 0;
        if (!qjs::toInt32(ctx, argv[1], &size)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ui.measure: size 无法转成 32 位整数");
        }
        if (size <= 0) return JS_ThrowTypeError(ctx, "ui.measure: size 必须为正（参考像素）");
        int w = 0;
        int h = 0;
        std::string merr;
        if (!measureText(text, size, false, *impl->font, &w, &h, &merr)) {
            return JS_ThrowTypeError(ctx, "ui.measure: 宿主字体度量失败：%s", merr.c_str());
        }
        Value out = qjs::newObject(ctx);
        if (out.isException()) return JS_EXCEPTION;
        if (!qjs::setProperty(ctx, out.raw(), "w", qjs::newInt32(ctx, w)) ||
            !qjs::setProperty(ctx, out.raw(), "h", qjs::newInt32(ctx, h))) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ui.measure: 结果对象写入失败");
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ui.measure: 宿主异常（C++ exception at script boundary）");
    }
}

// =======================================================================================
// game.* 的 getter（context §4）：读数由 render 的 readings 当帧注入，getter 读 Impl::current。
// ★ 只在 render 期间有值；不可用 ⇒ undefined（⛔ 不编 0）；mouse / worst 每次调用新建对象。
// =======================================================================================
// ★ D2：经典回退 HUD 的 `host.config` getter（定义在 config 一节；这里先前置声明 ——
//   `installHostApi` 比它先用）。
JSValue jsHostConfig(JSContext*, JSValueConst, int, JSValueConst*);

enum class GameProp : int {
    Frame = 0,
    Fps = 1,
    Mouse = 2,
    Script = 3,
    Worst = 4,
    Click = 5,
    Hover = 6,
    Active = 7,
    // ★ D4（2026-11-25）：F9 的**单调按下计数**（无源 ⇒ undefined；⛔ 不编 0）。
    OverlayKeySeq = 8,
};

// ★ C2b-2：三键电平的 `{left,right,middle}` **唯一**装配点（经典 `game.mouse` 与 RenderCtx 的
//   `game.mouse()` 共用）。`mouse_buttons_ok == false` ⇒ **不安装**这个键（⛔ 不编 false：
//   "松开"与"没有源"必须是两件事；d.ts 的 `buttons?` 就是这条）。
bool setMouseButtons(JSContext* ctx, JSValueConst obj, const UiFrameReadings& r) {
    if (!r.mouse_buttons_ok) return true;
    Value buttons = qjs::newObject(ctx);
    if (buttons.isException()) {
        qjs::clearException(ctx);
        return false;
    }
    if (!qjs::setProperty(ctx, buttons.raw(), "left", qjs::newBool(ctx, r.mouse_left)) ||
        !qjs::setProperty(ctx, buttons.raw(), "right", qjs::newBool(ctx, r.mouse_right)) ||
        !qjs::setProperty(ctx, buttons.raw(), "middle", qjs::newBool(ctx, r.mouse_middle))) {
        qjs::clearException(ctx);
        return false;
    }
    if (!qjs::setProperty(ctx, obj, "buttons", std::move(buttons))) {
        qjs::clearException(ctx);
        return false;
    }
    return true;
}

Value makeMouseObject(JSContext* ctx, const UiFrameReadings& r) {
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) return obj;
    if (!qjs::setProperty(ctx, obj.raw(), "x", qjs::newInt32(ctx, r.mouse_x)) ||
        !qjs::setProperty(ctx, obj.raw(), "y", qjs::newInt32(ctx, r.mouse_y))) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    if (!setMouseButtons(ctx, obj.raw(), r)) return Value::exception(ctx);
    return obj;
}

// `game.click` 的**一次性**载荷 `{key,x,y}`（x/y = 抬起点，窗口像素）。
// ★ 一次性由**宿主**保证：每一帧的 `UiFrameReadings::click_ok` 只在这一帧为真（`takeClick`）。
//   本 getter 不缓存、不置空 —— 同一帧里读两次拿到的是同一份快照（两次都是新对象、值相同）。
Value makeClickObject(JSContext* ctx, const UiFrameReadings& r) {
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) return obj;
    if (!qjs::setProperty(ctx, obj.raw(), "key", qjs::newString(ctx, r.click_key)) ||
        !qjs::setProperty(ctx, obj.raw(), "x", qjs::newInt32(ctx, r.click_x)) ||
        !qjs::setProperty(ctx, obj.raw(), "y", qjs::newInt32(ctx, r.click_y))) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    return obj;
}

Value makeWorstObject(JSContext* ctx, const UiFrameReadings& r) {
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) return obj;
    if (!qjs::setProperty(ctx, obj.raw(), "wall_ms", qjs::newDouble(ctx, r.worst_wall_ms))) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    // guest 未计时 ⇒ **没有** guest_ms 键（context §4）。
    if (r.worst_guest_ok &&
        !qjs::setProperty(ctx, obj.raw(), "guest_ms", qjs::newDouble(ctx, r.worst_guest_ms))) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    if (!qjs::setProperty(ctx, obj.raw(), "shim_ms", qjs::newDouble(ctx, r.worst_shim_ms))) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    return obj;
}

// =======================================================================================
// `game.local_frames()`（E）：**方法**（不是 getter），返回活动帧数组（top-first）或 undefined。
// ★ 全对象**唯一**一个非 getter 的属性（报告「game.local_frames 的注册」）：宿主每帧不推表，
//   脚本**按需**查询；`JS_DefinePropertyValueStr(..., 0)` ⇒ 不可写 / 不可枚举 / 不可配置
//   （strict 下赋值 ⇒ TypeError）。
// ★ 取不到的口径：`frames == nullptr` / 宿主 `err` 非空 / 空表 ⇒ **undefined**
//   （⛔ 不编 0 / 空对象 / 空数组冒充读数）。字段 = 报告的最小版：slot / script_id / parent /
//   script / name_degraded / top / **pos（切片 3：只有 top 帧有，其余没有这个键）**；
//   `script_id` / `parent` 是 **uint32 原值**（`0xFFFFFFFF` 打印成 `4294967295`，⛔ 不转成 -1）。
// =======================================================================================
Value newArray(JSContext* ctx) { return Value(ctx, JS_NewArray(ctx)); }

// 数组元素赋值：`JS_SetPropertyUint32` 取得值所有权（与 qjs::setProperty 的契约一致）。
bool setElement(JSContext* ctx, JSValueConst arr, std::uint32_t index, Value value) {
    return JS_SetPropertyUint32(ctx, arr, index, value.take()) >= 0;
}

bool setFrameProperty(JSContext* ctx, JSValueConst obj, const char* name, Value value) {
    if (!qjs::setProperty(ctx, obj, name, std::move(value))) {
        qjs::clearException(ctx);
        return false;
    }
    return true;
}

Value makeFrameObject(JSContext* ctx, const UiFrameInfo& frame) {
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) return obj;
    // ★ uint32 原值 ⇒ 用 int64（`newInt32` 会把 0xFFFFFFFF 变成 -1，那是另一条口径）。
    if (!setFrameProperty(ctx, obj.raw(), "slot", qjs::newInt64(ctx, static_cast<std::int64_t>(frame.slot))) ||
        !setFrameProperty(ctx, obj.raw(), "script_id",
                          qjs::newInt64(ctx, static_cast<std::int64_t>(frame.script_id))) ||
        !setFrameProperty(ctx, obj.raw(), "parent", qjs::newInt64(ctx, static_cast<std::int64_t>(frame.parent))) ||
        !setFrameProperty(ctx, obj.raw(), "script", qjs::newString(ctx, frame.script)) ||
        !setFrameProperty(ctx, obj.raw(), "name_degraded", qjs::newBool(ctx, frame.name_degraded)) ||
        !setFrameProperty(ctx, obj.raw(), "top", qjs::newBool(ctx, frame.top))) {
        return Value::exception(ctx);
    }
    // ★ 位置读数（切片 3）：**只有宿主给了值**（top 帧且门都成立）才设 `pos` 键；否则**不设键**
    //   ⇒ 脚本侧 `undefined`（⛔ 不编 0 / 不拿 frames[0] 冒充）。普通数据属性（与 slot / script_id
    //   同形：可写、不是 getter —— 这一版不要求只读）。
    if (frame.pos_ok &&
        !setFrameProperty(ctx, obj.raw(), "pos", qjs::newInt64(ctx, static_cast<std::int64_t>(frame.pos)))) {
        return Value::exception(ctx);
    }
    return obj;
}

JSValue jsGameLocalFrames(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        const Impl* impl = implOf(ctx);
        // 只在 render 期间有值（`impl->current` 由 render 复位 ⇒ 这里不需要第二个"复位"字段）。
        if (impl == nullptr || impl->current == nullptr) return JS_UNDEFINED;
        const UiFrameReadings& r = *impl->current;
        if (r.frames == nullptr) return JS_UNDEFINED;
        std::string frame_err;
        const std::vector<UiFrameInfo> frames = r.frames->localFrames(&frame_err);
        if (!frame_err.empty() || frames.empty()) return JS_UNDEFINED;
        Value arr = newArray(ctx);
        if (arr.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "game.local_frames: 分配数组失败");
        }
        for (std::size_t i = 0; i < frames.size(); ++i) {
            Value item = makeFrameObject(ctx, frames[i]);
            if (item.isException()) {
                qjs::clearException(ctx);
                return JS_ThrowTypeError(ctx, "game.local_frames: 写帧对象失败");
            }
            if (!setElement(ctx, arr.raw(), static_cast<std::uint32_t>(i), std::move(item))) {
                qjs::clearException(ctx);
                return JS_ThrowTypeError(ctx, "game.local_frames: 写数组元素失败");
            }
        }
        return arr.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "game.local_frames: 宿主异常（C++ exception at script boundary）");
    }
}

// 全局 `game` 的 getter：返回持有的 game 对象（只读绑定；strict 下赋值 ⇒ TypeError）。
JSValue jsGameObject(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    const Impl* impl = implOf(ctx);
    if (impl == nullptr) return JS_UNDEFINED;
    return impl->game_obj.dup().take();
}

JSValue jsGameProperty(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/,
                       int magic) {
    try {
        const Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->current == nullptr) return JS_UNDEFINED;
        const UiFrameReadings& r = *impl->current;
        switch (static_cast<GameProp>(magic)) {
            case GameProp::Frame:
                return qjs::newInt64(ctx, static_cast<std::int64_t>(r.frame)).take();
            case GameProp::Fps:
                if (!r.fps_ok) return JS_UNDEFINED;
                return qjs::newDouble(ctx, r.fps).take();
            case GameProp::Mouse:
                if (!r.mouse_ok) return JS_UNDEFINED;
                return makeMouseObject(ctx, r).take();
            case GameProp::Script:
                // `script_ok && 非空`（context §4 的括号）——空载荷 ⇒ undefined，⛔ 不画空行。
                if (!r.script_ok || r.script.empty()) return JS_UNDEFINED;
                return qjs::newString(ctx, r.script).take();
            case GameProp::Worst:
                if (!r.worst_ok) return JS_UNDEFINED;
                return makeWorstObject(ctx, r).take();
            case GameProp::Click:
                // 一次性：宿主没交来 click 的这一帧 ⇒ undefined（⛔ 不编空对象）。
                if (!r.click_ok) return JS_UNDEFINED;
                return makeClickObject(ctx, r).take();
            case GameProp::Hover:
                // 只读 key：指针不在任何命中区上 ⇒ undefined（⛔ 不编空字符串）。
                if (r.hover_key.empty()) return JS_UNDEFINED;
                return qjs::newString(ctx, r.hover_key).take();
            case GameProp::Active:
                // 只读 key：当前被捕获的那个（没有捕获 ⇒ undefined）。
                if (r.active_key.empty()) return JS_UNDEFINED;
                return qjs::newString(ctx, r.active_key).take();
            case GameProp::OverlayKeySeq:
                // ★ D4：没有按键源（没有 WindowManager / headless）⇒ **缺键**（undefined），
                //   ⛔ 不编 0（"没按过"与"没有源"必须是两件事）。
                if (!r.overlay_key_ok) return JS_UNDEFINED;
                return qjs::newInt64(ctx, static_cast<std::int64_t>(r.overlay_key_seq)).take();
        }
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "game: 宿主异常（C++ exception at script boundary）");
    }
}

// =======================================================================================
// 宿主能力（切片 3）：`host.profileBegin(label)` / `host.profileEnd()`
// ★ 两层错误口径（与 `ui.*` 完全同构）：
//   - **用法错 ⇒ TypeError**（前缀 = 函数名 + 冒号）：不在 render 内 / label 不可转字符串 /
//     宿主没接线 / 宿主 C++ 异常；
//   - **领域失败 ⇒ 返回值**（不抛）：'已有一段开着' / '没有打开的窗口' / '剪贴板写失败' 都是
//     **用户可见的正常状态**（HUD 要显示"失败"并继续画）。抛异常会让 render 返回 false ⇒
//     叠层被**一枪**永久关掉（`present_sdl.cpp:disableOverlay`）。
// ★ 返回对象所有键**恒存在**：`{ok, error}` / `{ok, text, error}`（⛔ 不造 undefined 键）。
// ★ `host` 对象**恒存在**：缺接线是**调用时**的响亮错，而不是画 HUD 时的形状差异。
// =======================================================================================
Value makeProfileBeginResult(JSContext* ctx, bool ok, const std::string& error) {
    Value out = qjs::newObject(ctx);
    if (out.isException()) return out;
    if (!qjs::setProperty(ctx, out.raw(), "ok", qjs::newBool(ctx, ok)) ||
        !qjs::setProperty(ctx, out.raw(), "error", qjs::newString(ctx, error))) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    return out;
}

Value makeProfileEndResult(JSContext* ctx, bool ok, const std::string& text, const std::string& error) {
    Value out = qjs::newObject(ctx);
    if (out.isException()) return out;
    if (!qjs::setProperty(ctx, out.raw(), "ok", qjs::newBool(ctx, ok)) ||
        !qjs::setProperty(ctx, out.raw(), "text", qjs::newString(ctx, text)) ||
        !qjs::setProperty(ctx, out.raw(), "error", qjs::newString(ctx, error))) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    return out;
}

JSValue jsHostProfileBegin(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->current == nullptr) {
            return JS_ThrowTypeError(ctx, "host.profileBegin: 只能在 render() 内调用");
        }
        const UiHostOps* ops = impl->current->host;
        if (ops == nullptr || ops->profile_begin == nullptr) {
            return JS_ThrowTypeError(ctx,
                                     "host.profileBegin: 宿主没有接 profile 桥（--overlay 未接线 / 测试替身）");
        }
        if (argc < 1) {
            return JS_ThrowTypeError(ctx, "host.profileBegin: label 必须是可转成 UTF-8 的字符串");
        }
        std::string label;
        if (!qjs::toString(ctx, argv[0], &label)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "host.profileBegin: label 必须是可转成 UTF-8 的字符串");
        }
        std::string error;
        const bool ok = ops->profile_begin(ops->ctx, label.c_str(), &error);
        Value out = makeProfileBeginResult(ctx, ok, error);
        if (out.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "host.profileBegin: 结果对象写入失败");
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "host.profileBegin: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsHostProfileEnd(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->current == nullptr) {
            return JS_ThrowTypeError(ctx, "host.profileEnd: 只能在 render() 内调用");
        }
        const UiHostOps* ops = impl->current->host;
        if (ops == nullptr || ops->profile_end == nullptr) {
            return JS_ThrowTypeError(ctx, "host.profileEnd: 宿主没有接 profile 桥（--overlay 未接线 / 测试替身）");
        }
        std::string text;
        std::string error;
        // ★ ok=false ⇒ 没有开着的窗口（text 留空）；ok=true + error 非空 ⇒ 窗口关了、文本在，
        //   但附带一步失败（剪贴板）—— 文本**不回滚**（需求单明文"INVALID 仍复制"）。
        const bool ok = ops->profile_end(ops->ctx, &text, &error);
        Value out = makeProfileEndResult(ctx, ok, ok ? text : std::string(), error);
        if (out.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "host.profileEnd: 结果对象写入失败");
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "host.profileEnd: 宿主异常（C++ exception at script boundary）");
    }
}

// 在 obj 上装一个"只有 getter"的属性（⛔ 不用普通数据属性；strict 赋值 ⇒ TypeError）。
// `magic < 0` = 普通 4 参 trampoline（jsGameObject）；否则走 5 参 magic trampoline（jsGameProperty）。
bool defineGetterOnly(JSContext* ctx, JSValueConst obj, const char* name, JSCFunction* fn,
                      JSCFunctionMagic* magic_fn, int magic, std::string* err) {
    Value getter(ctx, magic < 0 ? JS_NewCFunction(ctx, fn, name, 0)
                                : JS_NewCFunctionMagic(ctx, magic_fn, name, 0, JS_CFUNC_generic_magic, magic));
    if (getter.isException()) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 创建 getter ") + name + " 失败";
        return false;
    }
    const JSAtom atom = JS_NewAtom(ctx, name);
    if (atom == JS_ATOM_NULL) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 属性名 ") + name + " 失败";
        return false;
    }
    // JS_DefinePropertyGetSet 取得 getter / setter 的所有权。
    const int rc = JS_DefinePropertyGetSet(ctx, obj, atom, getter.take(), JS_UNDEFINED,
                                           JS_PROP_CONFIGURABLE | JS_PROP_ENUMERABLE);
    JS_FreeAtom(ctx, atom);
    if (rc < 0) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = std::string("quickjs: 定义 getter ") + name + " 失败";
        return false;
    }
    return true;
}

// 装 ui（三个函数）+ game（只读 getter 绑定）。
bool installHostApi(Impl& impl, std::string* err) {
    JSContext* ctx = impl.ctx;
    Value global = qjs::getGlobalObject(ctx);
    if (global.isException()) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = "quickjs: 取 globalThis 失败";
        return false;
    }

    bool ok = true;
    Value ui = qjs::newObject(ctx);
    if (ui.isException()) {
        qjs::clearException(ctx);
        ok = false;
        if (err != nullptr) *err = "quickjs: 分配 ui 对象失败";
    }
    if (ok) ok = setFunction(ctx, ui.raw(), "rect", jsUiRect, 1, err);
    if (ok) ok = setFunction(ctx, ui.raw(), "text", jsUiText, 1, err);
    if (ok) ok = setFunction(ctx, ui.raw(), "measure", jsUiMeasure, 2, err);
    if (ok) ok = setProperty(ctx, global.raw(), "ui", std::move(ui), err);

    if (ok) {
        Value game = qjs::newObject(ctx);
        if (game.isException()) {
            qjs::clearException(ctx);
            ok = false;
            if (err != nullptr) *err = "quickjs: 分配 game 对象失败";
        }
        if (ok) {
            ok = defineGetterOnly(ctx, game.raw(), "frame", nullptr, jsGameProperty,
                                  static_cast<int>(GameProp::Frame), err);
        }
        if (ok) ok = defineGetterOnly(ctx, game.raw(), "fps", nullptr, jsGameProperty,
                                          static_cast<int>(GameProp::Fps), err);
        if (ok) ok = defineGetterOnly(ctx, game.raw(), "mouse", nullptr, jsGameProperty,
                                          static_cast<int>(GameProp::Mouse), err);
        if (ok) {
            ok = defineGetterOnly(ctx, game.raw(), "script", nullptr, jsGameProperty,
                                  static_cast<int>(GameProp::Script), err);
        }
        if (ok) ok = defineGetterOnly(ctx, game.raw(), "worst", nullptr, jsGameProperty,
                                          static_cast<int>(GameProp::Worst), err);
        // ★ 命中 / 捕获（切片 2 后半）：三个只读 getter，值与其它读数一样**每帧注入**。
        //   `click` 是**一次性**对象（宿主 `takeClick` 每帧只交一次）；`hover` / `active` 是 key。
        //   变异：把它们注册成普通数据属性 / 删掉注册 ⇒ `ui-script-quickjs` 的 key-on-rect 红。
        if (ok) ok = defineGetterOnly(ctx, game.raw(), "click", nullptr, jsGameProperty,
                                          static_cast<int>(GameProp::Click), err);
        if (ok) ok = defineGetterOnly(ctx, game.raw(), "hover", nullptr, jsGameProperty,
                                          static_cast<int>(GameProp::Hover), err);
        if (ok) ok = defineGetterOnly(ctx, game.raw(), "active", nullptr, jsGameProperty,
                                          static_cast<int>(GameProp::Active), err);
        // ★ D4：F9 的按下沿计数（`game.overlayKeySeq`；无源 ⇒ undefined）。
        if (ok) ok = defineGetterOnly(ctx, game.raw(), "overlayKeySeq", nullptr, jsGameProperty,
                                          static_cast<int>(GameProp::OverlayKeySeq), err);
        // ★ `local_frames` 是**方法**（全对象唯一的非 getter 属性）：按需查询帧列表，宿主不推表。
        //   `JS_DefinePropertyValueStr(..., 0)` ⇒ 不可写 / 不可枚举 / 不可配置（strict 赋值 ⇒ TypeError）。
        //   变异：把它改成普通数据属性 / 删掉注册 ⇒ `ui-frames` 的 JS 面用例红。
        if (ok) {
            Value local_frames(ctx, JS_NewCFunction(ctx, jsGameLocalFrames, "local_frames", 0));
            if (local_frames.isException()) {
                qjs::clearException(ctx);
                ok = false;
                if (err != nullptr) *err = "quickjs: 创建 game.local_frames 失败";
            } else if (JS_DefinePropertyValueStr(ctx, game.raw(), "local_frames", local_frames.take(), 0) < 0) {
                qjs::clearException(ctx);
                ok = false;
                if (err != nullptr) *err = "quickjs: 定义 game.local_frames 失败";
            }
        }
        if (ok) {
            impl.game_obj = std::move(game);  // 持有（getter 每次 dup 返回）
            if (!defineGetterOnly(ctx, global.raw(), "game", jsGameObject, nullptr, -1, err)) {
                impl.game_obj.reset();
                ok = false;
            }
        }
    }

    // ★ `host`（切片 3）：**方法表**（像 `ui`），不是读数（像 `game`）⇒ 普通函数属性。
    //   对象**恒存在**；ops 缺失是调用时的 TypeError（见 `jsHostProfileBegin`）。
    if (ok) {
        Value host = qjs::newObject(ctx);
        if (host.isException()) {
            qjs::clearException(ctx);
            ok = false;
            if (err != nullptr) *err = "quickjs: 分配 host 对象失败";
        }
        if (ok) ok = setFunction(ctx, host.raw(), "profileBegin", jsHostProfileBegin, 1, err);
        if (ok) ok = setFunction(ctx, host.raw(), "profileEnd", jsHostProfileEnd, 0, err);
        // ★ D2：`host.config` 是 **getter**（不是方法/数据属性）：每次访问按"这一帧有没有桥"
        //   现算 —— 有桥 ⇒ 与 `ctx.config` 同形的对象；没有桥 ⇒ **`undefined`**（⛔ 不抛：
        //   HUD 每帧都要读，不能让一个 optional 能力把整个叠层打黑）。
        if (ok) ok = defineGetterOnly(ctx, host.raw(), "config", jsHostConfig, nullptr, -1, err);
        if (ok) ok = setProperty(ctx, global.raw(), "host", std::move(host), err);
    }
    return ok;
}

// =======================================================================================
// ES module（第五节）：路径归一化 / specifier 分类 / normalize / loader / drain / rejection
// ★ 全部字符串都用 '/' 分隔（缓存键与根目录比较同一套；Windows 的 `\` 在入口就折成 '/'）。
// =======================================================================================

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string part;
    for (const char c : path) {
        if (c == '/' || c == '\\') {
            if (!part.empty()) {
                parts.push_back(part);
                part.clear();
            }
        } else {
            part.push_back(c);
        }
    }
    if (!part.empty()) parts.push_back(part);
    return parts;
}

// `E:` / `C:` 这种盘符段：`..` 不许把它弹掉（否则绝对路径会"逃出盘符"）。
bool isDrivePart(const std::string& part) {
    return part.size() == 2 && std::isalpha(static_cast<unsigned char>(part[0])) && part[1] == ':';
}

std::string joinPath(const std::vector<std::string>& parts, bool absolute) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out += '/';
        out += parts[i];
    }
    if (absolute) out = "/" + out;
    return out;
}

// **纯文本**归一化（⛔ 不查盘、不解析大小写）：折叠 `.` / `..`，分隔符统一成 '/'。
// `..` 在"根"上夹住（绝对路径不越界；相对路径保留前导 `..` 以便调用方判逃逸）。
std::string normalizePath(const std::string& path) {
    const bool absolute = !path.empty() && (path[0] == '/' || path[0] == '\\');
    std::vector<std::string> out;
    for (const std::string& part : splitPath(path)) {
        if (part == "." || part.empty()) continue;
        if (part == "..") {
            if (!out.empty() && out.back() != ".." && !isDrivePart(out.back())) {
                out.pop_back();
            } else if (!absolute && (out.empty() || out.back() == "..")) {
                out.push_back("..");
            }
            continue;  // 绝对路径 / 盘符根上：夹住（不越界）
        }
        out.push_back(part);
    }
    return joinPath(out, absolute);
}

std::string pathDirName(const std::string& path) {
    const std::size_t at = path.find_last_of("/\\");
    if (at == std::string::npos) return {};
    return normalizePath(path.substr(0, at + 1));
}

bool isRelativeSpecifier(const std::string& name) {
    return name.compare(0, 2, "./") == 0 || name.compare(0, 3, "../") == 0 ||
           name.compare(0, 2, ".\\") == 0 || name.compare(0, 3, "..\\") == 0;
}

bool isAbsoluteSpecifier(const std::string& name) {
    if (name.empty()) return false;
    if (name[0] == '/' || name[0] == '\\') return true;  // POSIX 根 / UNC
    return name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) && name[1] == ':';
}

// `data:` / `http:` / `file:` …（不是路径的方案前缀）。
bool hasScheme(const std::string& name) {
    const std::size_t colon = name.find(':');
    if (colon == std::string::npos || colon == 0) return false;
    for (std::size_t i = 0; i < colon; ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!(std::isalnum(c) || c == '+' || c == '-' || c == '.')) return false;
    }
    return true;
}

// 入口 origin 是否是**文件路径**（`builtin:hud` 这种名字不能当相对 import 的基准）。
bool isFilePathLike(const std::string& name) {
    if (name.empty()) return false;
    const std::size_t colon = name.find(':');
    const std::size_t slash = name.find_first_of("/\\");
    if (colon != std::string::npos && (slash == std::string::npos || colon < slash) &&
        !(colon == 1 && std::isalpha(static_cast<unsigned char>(name[0])))) {
        return false;
    }
    return true;
}

char asciiFold(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// resolved 是否**在 root 之下**（root 本身不算：它是目录）。空 root = "相对 cwd 的当前目录"。
bool pathWithinRoot(const std::string& path, const std::string& root) {
    if (path.empty()) return false;
    if (root.empty()) return !(path.size() >= 2 && path[0] == '.' && path[1] == '.') && !isAbsoluteSpecifier(path);
    if (root == "/") return path.size() > 1 && path[0] == '/';
    if (path.size() <= root.size()) return false;
#ifdef _WIN32
    for (std::size_t i = 0; i < root.size(); ++i) {
        if (asciiFold(path[i]) != asciiFold(root[i])) return false;
    }
#else
    if (path.compare(0, root.size(), root) != 0) return false;
#endif
    return path[root.size()] == '/';
}

// 在**导入方源码**里找 specifier 字面量的 `<line>:<col>`（1 基）。
// ★ 引擎的 resolve 期错误没有 JS 栈（我们的 normalize 抛的错发生在编译期），但"必须带
//   origin:line:col 形状"是合同 ⇒ 用源码里的字面量位置补上（只用于错误文本，不参与解析）。
std::string locateSpecifier(const std::map<std::string, std::string>& sources, const std::string& base,
                            const std::string& spec) {
    const auto it = sources.find(base);
    if (it == sources.end()) return {};
    const std::string& source = it->second;
    std::size_t at = std::string::npos;
    for (const char quote : {'\'', '"'}) {
        const std::string needle = std::string(1, quote) + spec + quote;
        const std::size_t hit = source.find(needle);
        if (hit != std::string::npos) {
            at = hit + 1;  // 指到 specifier 的第一个字符
            break;
        }
    }
    if (at == std::string::npos) at = source.find(spec);
    if (at == std::string::npos) return {};
    std::size_t line = 1;
    std::size_t line_start = 0;
    for (std::size_t i = 0; i < at; ++i) {
        if (source[i] == '\n') {
            ++line;
            line_start = i + 1;
        }
    }
    return base + ":" + std::to_string(line) + ":" + std::to_string(at - line_start + 1);
}

bool readHostFile(const std::string& path, std::string* out) {
    std::FILE* f = lift::openHostFileUtf8(path, "rb");
    if (f == nullptr) return false;
    char buf[8192];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    return !bad;
}

// module normalizer：`base_name` = 导入方的模块名（= 归一化后的文件路径），`module_name` = 源码里
// 的 specifier。成功返回 `js_malloc` 的归一化名（引擎用 `js_free` 释放）；失败抛异常返 nullptr。
char* moduleNormalize(JSContext* ctx, const char* base_name, const char* module_name, void* opaque) {
    Impl* impl = static_cast<Impl*>(opaque);
    const std::string base = base_name != nullptr ? base_name : "";
    const std::string name = module_name != nullptr ? module_name : "";
    const auto reject = [&](const std::string& why) -> char* {
        if (impl != nullptr) impl->module_error_at = locateSpecifier(impl->module_sources, base, name);
        JS_ThrowSyntaxError(ctx, "%s", why.c_str());
        return nullptr;
    };
    if (impl == nullptr) return reject("quickjs: module loader 未初始化");
    if (name.empty()) return reject("拒绝空 module specifier");
    if (!isRelativeSpecifier(name)) {
        if (isAbsoluteSpecifier(name)) {
            return reject("拒绝绝对路径 import '" + name + "'（只允许 './x' / '../x' 相对路径）");
        }
        if (hasScheme(name)) {
            return reject("拒绝协议 / data: URL import '" + name + "'（只允许 './x' / '../x' 相对路径）");
        }
        return reject("拒绝 bare specifier '" + name + "'（只允许 './x' / '../x' 相对路径）");
    }
    if (!impl->module_entry_is_path) {
        return reject("入口模块名不是文件路径（" + impl->origin + "）⇒ 不能解析相对 import '" + name + "'");
    }
    const std::string dir = pathDirName(base);
    const std::string resolved = normalizePath(dir.empty() ? name : dir + "/" + name);
    if (!pathWithinRoot(resolved, impl->module_root)) {
        return reject("拒绝 import '" + name + "'：解析结果越出 UI 根目录（" + impl->module_root + "）");
    }
    // 同一条 specifier 的首次导入点（只用于 loader 读文件失败时的错误定位）。
    if (impl->module_import_at.find(resolved) == impl->module_import_at.end()) {
        impl->module_import_at[resolved] = locateSpecifier(impl->module_sources, base, name);
    }
    return js_strdup(ctx, resolved.c_str());
}

// module loader：读文件 + 以 `JS_EVAL_TYPE_MODULE|COMPILE_ONLY` 编译 + 解析依赖。
// ★ 失败**不取走**异常：让引擎把同一条异常穿过 `JS_Eval(MODULE)` 还给宿主（err 形状统一）。
JSModuleDef* moduleLoad(JSContext* ctx, const char* module_name, void* opaque) {
    Impl* impl = static_cast<Impl*>(opaque);
    const std::string name = module_name != nullptr ? module_name : "";
    std::string source;
    if (!readHostFile(name, &source)) {
        if (impl != nullptr) {
            const auto it = impl->module_import_at.find(name);
            if (it != impl->module_import_at.end()) impl->module_error_at = it->second;
        }
        JS_ThrowReferenceError(ctx, "读不到模块 '%s'", name.c_str());
        return nullptr;
    }
    if (impl != nullptr) impl->module_sources[name] = source;  // 先记源码：嵌套 import 的定位要用
    Value fn(ctx, JS_Eval(ctx, source.c_str(), source.size(), name.c_str(),
                          JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY));
    if (fn.isException()) return nullptr;  // 异常留在 ctx 上（带被导入文件的 line:col）
    if (JS_ResolveModule(ctx, fn.raw()) < 0) {
        // ★ 不能 `JS_FreeValue` 一个 module 值：quickjs.c:7176 的 `case JS_TAG_MODULE` 是 `abort()`。
        //   module def 挂在 `ctx->loaded_modules` 上，由 `JS_FreeContext` 统一释放（上游 loader 同形）。
        fn.take();
        return nullptr;
    }
    JSModuleDef* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(fn.raw()));
    fn.take();  // 同上：交给 ctx 的 module 列表
    return module;
}

// drain 到无 pending job。失败（有 job 抛错）⇒ 把那条异常取走并返回 false。
bool drainPendingJobs(Impl& impl, std::string* err) {
    for (int i = 0; i < kMaxPendingJobs; ++i) {
        JSContext* job_ctx = nullptr;
        const int rc = JS_ExecutePendingJob(impl.rt, &job_ctx);
        if (rc == 0) return true;
        if (rc > 0) continue;
        qjs::takeException(job_ctx != nullptr ? job_ctx : impl.ctx, impl.origin, err);
        return false;
    }
    if (err != nullptr) {
        *err = impl.origin + ": module 求值的 microtask 数量超过上限（" + std::to_string(kMaxPendingJobs) + "）";
    }
    return false;
}

// 把"被 Promise reject 的 reason"格式化成与 `qjs::takeException` 相同的 err 形状：
// `<origin>: <message>\n<stack>`（`stack` 来自 module 里 throw 的那一行）。
void formatThrownValue(JSContext* ctx, const std::string& origin, JSValueConst value, std::string* err) {
    std::string text;
    try {
        std::string message;
        std::string stack;
        if (JS_IsObject(value)) {
            Value msg = qjs::getProperty(ctx, value, "message");
            if (msg.isException()) {
                qjs::clearException(ctx);
                msg = Value::undefined(ctx);
            }
            if (!msg.isUndefined() && !msg.isNull() && !qjs::toString(ctx, msg.raw(), &message)) {
                qjs::clearException(ctx);
            }
            Value stack_v = qjs::getProperty(ctx, value, "stack");
            if (stack_v.isException()) {
                qjs::clearException(ctx);
                stack_v = Value::undefined(ctx);
            }
            if (stack_v.isString() && !qjs::toString(ctx, stack_v.raw(), &stack)) {
                qjs::clearException(ctx);
            }
        }
        if (message.empty() && !qjs::toString(ctx, value, &message)) qjs::clearException(ctx);
        if (message.empty()) message = "(no message)";
        text = origin.empty() ? message : (origin + ": " + message);
        if (!stack.empty()) text += "\n" + stack;
    } catch (...) {
        text = origin.empty() ? "quickjs: 取 rejection 时宿主异常" : (origin + ": quickjs: 取 rejection 时宿主异常");
    }
    if (err != nullptr) *err = std::move(text);
}

// resolve 期拒绝（bare specifier / 绝对路径 / data: / `..` 逃逸 / 读不到被导入文件）**没有 JS 栈** ⇒
// 用源码里 specifier 字面量的位置补 `<origin>:<line>:<col>` 形状（只补错文本，不参与解析）。
// ★ 入口（内部 hud 回退的 module 脚本与 `--plugin`）共用这一份：两处各写一段会让形状慢慢分叉。
void appendModuleErrorAt(Impl& impl, std::string* err) {
    if (err == nullptr || impl.module_error_at.empty()) return;
    if (err->find(impl.module_error_at) != std::string::npos) return;
    *err += "\n    at " + impl.module_error_at;
}

// classic 能否**只编译**通过（不执行）：这是"旧语义优先"的探针 —— 能编就绝不走 module。
bool classicParses(JSContext* ctx, const std::string& source, const std::string& origin) {    Value fn(ctx, JS_Eval(ctx, source.c_str(), source.size(), origin.c_str(),
                          JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT | JS_EVAL_FLAG_COMPILE_ONLY));
    if (fn.isException()) {
        qjs::clearException(ctx);  // 探针失败不是错误（可能就是 module 语法），⛔ 不许把异常留到下一次
        return false;
    }
    return true;
}

// =======================================================================================
// UiScript / ScriptRuntime 的实现
// =======================================================================================
class QuickJsScript final : public UiScript {
public:
    explicit QuickJsScript(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
    ~QuickJsScript() override {
        if (impl_) impl_->shutdown();
    }

    bool render(UiBuilder& builder, const UiFrameReadings& readings, std::string* err) override {
        Impl& impl = *impl_;
        if (impl.ctx == nullptr) {
            if (err != nullptr) *err = "quickjs: 脚本已 shutdown";
            return false;
        }
        impl.builder = &builder;
        impl.current = &readings;
        impl.ops = 0;
        impl.overran = false;
        // ★ D2：经典回退 HUD 的 `host.config` 来路 = 本帧 readings 的 `UiHostOps::config`
        //   （非拥有；flushConfig 在 render 返回后还要用它，所以留在 impl 上）。
        impl.render_bridge = readings.host != nullptr ? readings.host->config : nullptr;
        bool ok = false;
        try {
            Value result(impl.ctx, JS_Call(impl.ctx, impl.render.raw(), JS_UNDEFINED, 0, nullptr));
            ok = !result.isException();
            if (!ok) qjs::takeException(impl.ctx, impl.origin, err);
        } catch (...) {
            if (err != nullptr) *err = impl.origin + ": 宿主异常（C++ exception at script boundary）";
            ok = false;
        }
        // ★ 无论成败都复位指针：下一次 render 不能看到上一次的 builder / readings。
        impl.builder = nullptr;
        impl.current = nullptr;
        return ok;
    }

    // ★ D2：经典回退 HUD 的 `host.config` 写盘口（render 之后由 present_sdl 调一次）。
    void flushConfig(std::string* warn) override {
        if (impl_ != nullptr && impl_->render_bridge != nullptr) {
            impl_->render_bridge->flushIfDirty(warn);
        }
    }
    // ★ D2：桥的实现（`PluginHost`）比本脚本**先析构** ⇒ 宿主在它失效前调这里清掉缓存，
    //   否则 shutdown 时会对已销毁的对象调虚函数（实测 = 宿主 0xC0000005）。
    void forgetConfigBridge() override {
        if (impl_ != nullptr) impl_->render_bridge = nullptr;
    }

    void setBudget(std::uint64_t deadline_ns, std::uint32_t op_limit) override {
        impl_->deadline_ns = deadline_ns;
        impl_->op_limit = op_limit;
    }

    bool overran() const override { return impl_->overran; }

    void shutdown() override { impl_->shutdown(); }

private:
    std::unique_ptr<Impl> impl_;
};

// =======================================================================================
// `createImpl`：一个实例的**全部语言无关装配**（runtime / context / 三个预算 / 沙箱 / 宿主 API）
// ★ 两条入口都走这里：内部 hud 回退的 classic/module 脚本（`QuickJsRuntime::load`）与 `--plugin` 的
//   ES module（`makeQuickJsPlugin`）。⛔ 不要各写一份 —— 那会让沙箱的删除项 / 内存上限 /
//   interrupt 回调在两条路上**静默分叉**（一个入口删了 `Function.prototype.constructor`、
//   另一个忘了，没有任何症状）。
// =======================================================================================
std::unique_ptr<Impl> createImpl(const std::string& origin, FontEngine* font, std::string* err) {
    auto impl = std::make_unique<Impl>();
    impl->origin = origin;
    impl->font = font;

    impl->rt = JS_NewRuntime();
    if (impl->rt == nullptr) {
        if (err != nullptr) *err = origin + ": quickjs: JS_NewRuntime 失败";
        return nullptr;
    }
    // ★ 三个预算在**建 context 之前**设（context §5）。
    JS_SetMemoryLimit(impl->rt, kMemoryLimitBytes);
    JS_SetMaxStackSize(impl->rt, kStackLimitBytes);
    JS_SetInterruptHandler(impl->rt, &interruptHandler, impl.get());
#ifndef NDEBUG
    // 开发构建：JS_FreeRuntime 时把泄漏的对象 / 字符串打出来；产品构建（NDEBUG）关。
    JS_SetDumpFlags(impl->rt, JS_DUMP_LEAKS);
#endif

    impl->ctx = JS_NewContextRaw(impl->rt);
    if (impl->ctx == nullptr) {
        if (err != nullptr) *err = origin + ": quickjs: JS_NewContextRaw 失败";
        return nullptr;
    }
    JS_SetContextOpaque(impl->ctx, impl.get());
    JS_AddIntrinsicBaseObjects(impl->ctx);
    // ★ 必须加：只为让**宿主**的 JS_Eval 能编译脚本（文件头 §1）；随后把脚本入口删掉。
    JS_AddIntrinsicEval(impl->ctx);
    // ★ module 求值返回 Promise（顶层 await 也是它）⇒ 必须加；**不删** globalThis.Promise
    //   （用户裁决 2026-11-19：沙箱不是安全边界，保留的删除只有功能目的；文件头 §1.5）。
    if (JS_AddIntrinsicPromise(impl->ctx) < 0) {
        if (err != nullptr) *err = origin + ": quickjs: JS_AddIntrinsicPromise 失败";
        return nullptr;
    }

    std::string setup_err;
    if (!installSandbox(*impl, &setup_err) || !installHostApi(*impl, &setup_err)) {
        // 沙箱 / 注入是**宿主**的失败：有引擎异常就经 takeException 取走（带 origin:line:col），
        // 没有就用我们自己点名的文本。⛔ 两条路径都必须让 ctx 干净地走出 load。
        if (JS_HasException(impl->ctx)) {
            qjs::takeException(impl->ctx, origin, err);
        } else if (err != nullptr) {
            *err = origin + ": " + setup_err;
        }
        return nullptr;
    }
    return impl;
}

class QuickJsRuntime final : public ScriptRuntime {
public:
    std::unique_ptr<UiScript> load(const std::string& source, const std::string& origin, FontEngine* font,
                                   std::string* err) override {
        std::unique_ptr<Impl> impl = createImpl(origin, font, err);
        if (impl == nullptr) return nullptr;

        // ---- 检测（文件头 §5）：classic 能编就绝不走 module（hud 回退的 classic 语义优先）；编不过才问
        //      `JS_DetectModule`（它把源码当 module 编一次）。★ 不能只看 DetectModule：0.18.0 对
        //      普通 classic 脚本也返 true（`quickjs.c:64939` 先置 `is_module = true`）。
        const bool classic_ok = classicParses(impl->ctx, source, origin);
        const bool is_module = !classic_ok && JS_DetectModule(source.c_str(), source.size());
        if (is_module) {
            impl->module_entry_is_path = isFilePathLike(origin);
            impl->module_root = pathDirName(origin);
            impl->module_sources[origin] = source;  // 入口源码：错误定位要用
            JS_SetModuleLoaderFunc(impl->rt, moduleNormalize, moduleLoad, impl.get());
        }

        Value eval_result(impl->ctx, JS_Eval(impl->ctx, source.c_str(), source.size(), origin.c_str(),
                                             is_module ? JS_EVAL_TYPE_MODULE
                                                       : (JS_EVAL_TYPE_GLOBAL | JS_EVAL_FLAG_STRICT)));
        if (eval_result.isException()) {
            qjs::takeException(impl->ctx, origin, err);
            // resolve 期拒绝（bare / 绝对 / data: / 逃逸 / 读不到文件）没有 JS 栈 ⇒ 用源码里
            // specifier 字面量的位置补形状（唯一实现在 `appendModuleErrorAt`）。
            if (is_module) appendModuleErrorAt(*impl, err);
            return nullptr;
        }
        if (is_module) {
            // module 求值返回 Promise ⇒ drain 到无 job，再查 rejection（⛔ 不许静默吞）。
            std::string drain_err;
            if (!drainPendingJobs(*impl, &drain_err)) {
                if (err != nullptr) *err = drain_err.empty() ? (origin + ": module 求值失败") : drain_err;
                return nullptr;
            }
            const JSPromiseStateEnum state = JS_PromiseState(impl->ctx, eval_result.raw());
            if (state == JS_PROMISE_REJECTED) {
                Value reason(impl->ctx, JS_PromiseResult(impl->ctx, eval_result.raw()));
                formatThrownValue(impl->ctx, origin, reason.raw(), err);
                return nullptr;
            }
            if (state == JS_PROMISE_PENDING) {
                if (err != nullptr) {
                    *err = origin + ": module 顶层 await 没有可执行的异步来源（module 求值未完成）";
                }
                return nullptr;
            }
        }

        Value global = qjs::getGlobalObject(impl->ctx);
        if (global.isException()) {
            qjs::takeException(impl->ctx, origin, err);
            return nullptr;
        }
        Value render = qjs::getProperty(impl->ctx, global.raw(), "render");
        if (render.isException()) {
            qjs::takeException(impl->ctx, origin, err);
            return nullptr;
        }
        if (!render.isFunction()) {
            if (err != nullptr) {
                *err = origin +
                       ": 脚本没有定义 `globalThis.render`（入口约定 = globalThis.render = function "
                       "render() { … }；它必须是函数）";
            }
            return nullptr;
        }
        impl->render = std::move(render);
        return std::make_unique<QuickJsScript>(std::move(impl));
    }
};

// =======================================================================================
// 插件（C2a/C2b）：`export default PluginModule` + `ctx`（RunCtx / RenderCtx）的绑定
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.4c（render 只画）/ §4.5 / §4.6 / §5 C2 /
//       §6 行 9·11·12·26·30·34。
// 父级裁决：`.tmp/plugin-runtime/c2-decisions.md` D-C2-3（不支持的 style ⇒ TypeError）、
//       D-C2-5（headless + render 只警告一次）、D-C2-6（`ctx.read.frameHash` **不安装**）、
//       D-C2-7（`ctx.now` 每回调新建值对象；`initialState` 浅拷贝；`ctx.run`/`ctx.dispatch`
//       不安装）、D-C2-9（`finish`/`quit` 的出口）。
//
// ★ **两条入口按 flag 选择**（⛔ 不按"有没有 default 猜"，测绘 §6-③）：
//   内部 hud 回退仍走 `QuickJsRuntime::load`（classic 优先，读 `globalThis.render`）；
//   `--plugin` 走 `makeQuickJsPlugin`（**强制 module**，只读 default 导出）。
//   ⇒ 一个同时带 `export default` 与 `globalThis.render` 的文件，行为不随实现漂。
//
// ★ **`ctx` 是每次回调新建的值对象**（`ctx.now` 更是每回调一个新快照，D-C2-7）：
//   RunCtx 的成员 = `edit/input/read/trigger/ui/profile/state/params/appdata/log/fail/finish/quit` + `now`；
//   ⛔ **没有** `draw`（C2b 只装在 RenderCtx 上；`onTrigger` 不画）、`run`/`dispatch`（C5）、
//   `sample`/`screenshot`（永远不做）。`api-surface` 用例断言这些**缺席**（不是"有但抛"）。
//   RenderCtx（C2b，**每帧新建**）= `ui/draw/read/game/params/log/profile/config`
//   （见下面的 `buildRenderCtx`；`config` = 2026-11-26 加回的第 8 个成员）。
//
// ★ **`ctx.ui` 是宿主本地的 JS 状态**：数据面 = `Impl::ui_data`（一个 JS 对象），可见性 =
//   `Impl::ui_visible`。⛔ 不进 guest / 不进 Edit / 不进存档。RunCtx 与 RenderCtx **共用**
//   这两个字段 ⇒ "onTrigger 写入、下一次 render 可见"是同一份数据的事实，不是同步机制。
//
// ★ **trigger 对象表**：JS 对象只带 id（`TriggerRef`），真值在 `Impl::triggers`；finalizer 扣引用。
//   同一个 `TriggerState` 在不同 JS 对象之间共享 ⇒ "同一对象重新 arm 续计 maxFires" 照样成立。
// =======================================================================================

// ---- 插件侧的 InputPoint 名字 ----
// ★ d.ts 是 **camelCase**（`keyLevel` / `peekMsg` / `msgWait` / `queueStatus`），而宿主侧的
//   `inputPointName()` 是 PascalCase（`KeyLevel` / `PeekMessage` …）。⛔ 不要复用那个函数：
//   它是 `--input-plan` trace 的读数面，两套名字各有消费者。
struct PluginPointName {
    const char* js;
    lift::InputPoint point;
};

constexpr PluginPointName kPluginPointNames[] = {
    {"keyLevel", lift::InputPoint::KeyLevel},       {"cursorPos", lift::InputPoint::CursorPos},
    {"peekMsg", lift::InputPoint::PeekMsg},         {"getMsg", lift::InputPoint::GetMsg},
    {"msgWait", lift::InputPoint::MsgWait},         {"waitMessage", lift::InputPoint::WaitMessage},
    {"queueStatus", lift::InputPoint::QueueStatus}, {"translate", lift::InputPoint::Translate},
    {"dispatch", lift::InputPoint::Dispatch},       {"setCursorPos", lift::InputPoint::SetCursorPos},
    // ★ F5-R2（父级裁决）：`fileRead` **也是**名字表的一员 —— `InputPoint::FileRead` 是 11 个投递点
    //   之一（`kernel32!ReadFile` 的"文件读完"事件源），d.ts 的并集已经在 F5-R1 里补上它
    //   ⇒ 运行期词表必须能**解析**它（否则 `atReadPoint("fileRead", …)` 与 `ctx.now.point` 的
    //   同一个名字一边能给、一边抛 TypeError）。⛔ 不要只让 `pluginPointName()`（输出侧）认它。
    {"fileRead", lift::InputPoint::FileRead},
};

const char* pluginPointName(lift::InputPoint point) {
    if (point == lift::InputPoint::Count) return "any";
    for (const PluginPointName& named : kPluginPointNames) {
        if (named.point == point) return named.js;
    }
    // 到不了（`kPluginPointNames` 现在覆盖了除 `Count` 以外的全部 11 个投递点）；兜底仍如实返回
    // 一个名字而不是冒充 `"any"`（那会让 trace 说谎）。
    return "fileRead";
}

bool parsePluginPoint(JSContext* ctx, JSValueConst value, lift::InputPoint* out, std::string* why) {
    std::string text;
    if (!JS_IsString(value) || !qjs::toString(ctx, value, &text)) {
        qjs::clearException(ctx);
        *why = "point 必须是字符串";
        return false;
    }
    if (text == "any") {
        *out = lift::InputPoint::Count;
        return true;
    }
    for (const PluginPointName& named : kPluginPointNames) {
        if (text == named.js) {
            *out = named.point;
            return true;
        }
    }
    *why = "point 不认识：'" + text + "'（any | keyLevel | cursorPos | peekMsg | getMsg | msgWait | " +
           "waitMessage | queueStatus | translate | dispatch | setCursorPos | fileRead）";
    return false;
}

// `ctx.edit/input/read.…` 的 `kind` 参数（d.ts 的 `GlobalKind` = "int" | "uint" | "float" | "string"）。
bool parseGlobalKindArg(JSContext* ctx, JSValueConst value, lift::GlobalKind* out, std::string* why) {
    std::string text;
    if (!JS_IsString(value) || !qjs::toString(ctx, value, &text)) {
        qjs::clearException(ctx);
        *why = "kind 必须是 'int' | 'uint' | 'float' | 'string'";
        return false;
    }
    if (text == "int") {
        *out = lift::GlobalKind::Int;
        return true;
    }
    if (text == "uint") {
        *out = lift::GlobalKind::Uint;
        return true;
    }
    if (text == "float") {
        *out = lift::GlobalKind::Float;
        return true;
    }
    if (text == "string") {
        *out = lift::GlobalKind::String;
        return true;
    }
    *why = "kind 必须是 'int' | 'uint' | 'float' | 'string'（收到 '" + text + "'）";
    return false;
}

// 宿主回调面（回调外恒 nullptr ⇒ 一律 TypeError，⛔ 不静默 no-op）。
lift::ui::PluginCallHost* callHost(JSContext* ctx) {
    Impl* impl = implOf(ctx);
    return impl != nullptr ? impl->call_host : nullptr;
}

// ---- `ctx.*` 的 trampoline 前置声明 ----
// ★ 为什么需要：`buildRunCtx` / `makeUiObject` 比它们的定义**先用**。这个匿名命名空间里没有
//   "头文件里统一声明"这回事 ⇒ 显式列一次（⛔ 不要靠挪定义顺序凑）。
//   函数体在下面（`ctx` 的成员构造之后），顺序与 d.ts 的成员顺序一致。
JSValue jsCtxEditGlobalSet(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxInputClick(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxInputMove(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxInputDown(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxInputUp(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxInputStatus(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxReadGlobalGet(JSContext*, JSValueConst, int, JSValueConst*);
// `ctx.hook`（脚本指令位置钩子；只 RunCtx）—— 触发 / 读 / 改是三个独立能力
JSValue jsCtxHookAt(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxHookList(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxHookClear(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxTriggerAfterScript(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxTriggerAfterPosition(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxTriggerAfterDelay(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxTriggerAtReadPoint(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxUiShow(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxUiHide(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxUiSet(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxUiVisible(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxUiGet(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxLog(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxProfileBegin(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxProfileEnd(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxFail(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxFinish(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxQuit(JSContext*, JSValueConst, int, JSValueConst*);
// ---- AppData（同步只读宿主 I/O；只 RunCtx；宿主段包 `HostPause`）----
JSValue jsCtxAppDataStat(JSContext*, JSValueConst, int, JSValueConst*);
// ---- Config（域式 JSON 配置；RunCtx 的 `ctx.config` 与经典回退 HUD 的 `host.config`）----
// ★ 一份实现体、两条门（`configCall` 判定）：Run 回调（`host->inCallback()`）与 render 帧
//   （`impl.current`/`builder` + 不可枚举的 `__render` 标记）。⛔ 不要复制第二套校验 / 值转换。
Value makeConfigObject(Impl&, bool render);
JSValue jsCtxConfigDomain(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxConfigGet(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxConfigHas(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxConfigSet(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxConfigDelete(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxConfigAll(JSContext*, JSValueConst, int, JSValueConst*);
// ---- C2b：`RenderCtx` 的 trampoline（`draw` / `read` / `game`；`ui` / `params` / `log` 复用 RunCtx 那几个）
JSValue jsCtxDrawRect(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxDrawText(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxDrawMeasure(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxReadFrames(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsCtxReadScriptPos(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsRenderGameMouse(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsRenderGameLocalFrames(JSContext*, JSValueConst, int, JSValueConst*);
// ★ S2：`RenderCtx.profile` 的 trampoline 与 RunCtx 的**分开两份**（`jsCtxProfile*` 仍只认
//   `inCallback()` ⇒ render 里拿一个存下来的 RunCtx 调 profile 仍 TypeError；见
//   `plugin/render-stored-runctx-rejected`）。render 的两个判 `impl.current != nullptr`。
JSValue jsRenderProfileBegin(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsRenderProfileEnd(JSContext*, JSValueConst, int, JSValueConst*);

// 小工具：读一个**必填**整数（`undefined` ⇒ 失败；非有限 ⇒ 失败，⛔ 不静默变 0）。
bool readPluginInt(JSContext* ctx, JSValueConst obj, const char* name, bool required, std::int64_t dflt,
                   std::int64_t* out, std::string* why) {
    Value v = qjs::getProperty(ctx, obj, name);
    if (v.isException()) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 读取失败";
        return false;
    }
    if (v.isUndefined()) {
        if (required) {
            *why = std::string(name) + " 缺失（必须是数字）";
            return false;
        }
        *out = dflt;
        return true;
    }
    if (!v.isNumber()) {
        *why = std::string(name) + " 必须是数字";
        return false;
    }
    double d = 0.0;
    if (!qjs::toDouble(ctx, v.raw(), &d) || !std::isfinite(d)) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 必须是有限数字";
        return false;
    }
    *out = static_cast<std::int64_t>(d);
    return true;
}

// 小工具：读一个可选字符串（缺省 ⇒ 空串；`null` 当缺省）。
bool readPluginText(JSContext* ctx, JSValueConst obj, const char* name, std::string* out, std::string* why) {
    Value v = qjs::getProperty(ctx, obj, name);
    if (v.isException()) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 读取失败";
        return false;
    }
    if (v.isUndefined() || v.isNull()) {
        out->clear();
        return true;
    }
    if (!qjs::toString(ctx, v.raw(), out)) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 无法转成 UTF-8 字符串";
        return false;
    }
    return true;
}

// `opts.name`（必填、非空）+ `opts.mode`（"once" | "reenter"）+ `opts.timeoutMs` / `opts.maxFires`。
bool readTriggerOptions(JSContext* ctx, JSValueConst opts, const char* fn, std::string* name,
                        lift::plugin::TriggerMode* mode, std::uint32_t* max_fires, std::uint64_t* timeout_ms,
                        std::string* why) {
    if (!JS_IsObject(opts)) {
        *why = std::string(fn) + ": 最后一个参数必须是 opts 对象 { name, mode?, timeoutMs?, maxFires? }";
        return false;
    }
    if (!readPluginText(ctx, opts, "name", name, why)) return false;
    if (name->empty()) {
        *why = std::string(fn) + ": opts.name 必填（它是插件侧的具名状态）";
        return false;
    }
    std::string mode_text = "once";
    if (!readPluginText(ctx, opts, "mode", &mode_text, why)) return false;
    if (mode_text.empty() || mode_text == "once") {
        *mode = lift::plugin::TriggerMode::Once;
    } else if (mode_text == "reenter") {
        *mode = lift::plugin::TriggerMode::Reenter;
    } else {
        *why = std::string(fn) + ": opts.mode 只能是 'once' | 'reenter'（收到 '" + mode_text + "'）";
        return false;
    }
    std::int64_t timeout = 0;
    if (!readPluginInt(ctx, opts, "timeoutMs", false, 0, &timeout, why)) return false;
    if (timeout < 0) {
        *why = std::string(fn) + ": opts.timeoutMs 不许为负（0 = 不设超时）";
        return false;
    }
    *timeout_ms = static_cast<std::uint64_t>(timeout);
    std::int64_t max_fires_out = 0;
    if (!readPluginInt(ctx, opts, "maxFires", false, 0, &max_fires_out, why)) return false;
    if (max_fires_out < 0) {
        *why = std::string(fn) + ": opts.maxFires 不许为负（0 = 不设硬上限）";
        return false;
    }
    *max_fires = static_cast<std::uint32_t>(max_fires_out);
    return true;
}

// =======================================================================================
// trigger 对象表：JS 对象 ⇄ `Trigger`（state 共享）
// =======================================================================================
// ★ finalizer 的**唯一**纪律：先取需要的东西、再 `delete ref`（F1 / C2a 复审）。
//   `delete ref` 之后读 `ref->id` 是 heap-use-after-free：正常构建无症状（`id` 在 offset 8，
//   刚释放的块通常还读得到），换了 allocator / sanitizer 就是 UB；更糟的是它**以为**自己在
//   "按 id 找条目"，实际可能减错对象的 `refs`（对象表丢条目）或永远找不到（条目永不释放）。
void triggerFinalizer(JSRuntime* /*rt*/, JSValueConst value) {
    auto* ref = static_cast<TriggerRef*>(JS_GetOpaque(value, JS_GetClassID(value)));
    if (ref == nullptr) return;
    Impl* impl = ref->impl;
    const std::uint32_t id = ref->id;  // ★ 必须在 `delete ref` **之前**取出来（见头注）
    delete ref;
    if (impl == nullptr) return;
    const auto it = impl->triggers.find(id);
    if (it == impl->triggers.end()) return;
    if (--it->second.refs <= 0) impl->triggers.erase(it);
}

// 把一个 `Trigger` 包成 JS 对象（同一个 state ⇒ 复用同一个 id；引用计数 +1）。
Value wrapTrigger(Impl& impl, const lift::plugin::Trigger& trigger) {
    JSContext* ctx = impl.ctx;
    std::uint32_t id = 0;
    for (const auto& kv : impl.triggers) {
        if (kv.second.trigger.state == trigger.state) {
            id = kv.first;
            break;
        }
    }
    if (id == 0) {
        if (impl.next_trigger_id == 0) impl.next_trigger_id = 1;  // 回绕保护（实际上到不了）
        id = impl.next_trigger_id++;
        TriggerEntry entry;
        entry.trigger = trigger;
        entry.refs = 0;
        impl.triggers[id] = std::move(entry);
    }
    Value obj(ctx, JS_NewObjectClass(ctx, impl.trigger_class));
    if (obj.isException()) return obj;
    auto* ref = new TriggerRef{&impl, id};
    if (JS_SetOpaque(obj.raw(), ref) < 0) {
        delete ref;
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    ++impl.triggers[id].refs;
    return obj;
}

// `start`/`onTrigger` 的一个元素 ⇒ `Trigger`。⛔ 只认**本 runtime 的** `ctx.trigger.*` 造出来的对象。
bool appendTriggerFromValue(Impl& impl, JSValueConst value, std::vector<lift::plugin::Trigger>* out,
                            std::string* err) {
    if (JS_IsPromise(value)) {
        *err = impl.origin + ": start/onTrigger 必须**同步**返回（拿到 Promise ⇒ async/await 不支持）";
        return false;
    }
    if (!JS_IsObject(value)) {
        *err = impl.origin + ": start/onTrigger 必须返回 Trigger | Trigger[] | null";
        return false;
    }
    auto* ref = static_cast<TriggerRef*>(JS_GetOpaque(value, impl.trigger_class));
    if (ref == nullptr || ref->impl != &impl) {
        *err = impl.origin + ": 返回的对象不是 ctx.trigger.* 造出来的 Trigger（⛔ 不认任意对象）";
        return false;
    }
    const auto it = impl.triggers.find(ref->id);
    if (it == impl.triggers.end()) {
        *err = impl.origin + ": 返回的 Trigger 已失效（对象表里没有这个 id）";
        return false;
    }
    out->push_back(it->second.trigger);
    return true;
}

// 返回值翻译：`null` / `undefined` / `[]` = done；单个对象 = 一个；数组 = 逐个。
bool collectTriggers(Impl& impl, JSValueConst value, std::vector<lift::plugin::Trigger>* out,
                     std::string* err) {
    if (JS_IsUndefined(value) || JS_IsNull(value)) return true;  // done（⛔ 不逼插件写 return null）
    if (JS_IsArray(value)) {
        std::int64_t len = 0;
        if (JS_GetLength(impl.ctx, value, &len) < 0) {
            qjs::clearException(impl.ctx);
            *err = impl.origin + ": 读取返回数组的长度失败";
            return false;
        }
        for (std::int64_t i = 0; i < len; ++i) {
            Value item(impl.ctx, JS_GetPropertyUint32(impl.ctx, value, static_cast<std::uint32_t>(i)));
            if (item.isException()) {
                qjs::takeException(impl.ctx, impl.origin, err);
                return false;
            }
            if (!appendTriggerFromValue(impl, item.raw(), out, err)) return false;
        }
        return true;
    }
    return appendTriggerFromValue(impl, value, out, err);
}

// =======================================================================================
// `ctx` 的成员构造
// =======================================================================================
Value makeReceiptObject(JSContext* ctx, bool accepted, bool queued, bool applied, const char* code,
                        std::uint64_t seq, std::uint64_t id) {
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) return obj;
    bool ok = qjs::setProperty(ctx, obj.raw(), "accepted", qjs::newBool(ctx, accepted)) &&
              qjs::setProperty(ctx, obj.raw(), "queued", qjs::newBool(ctx, queued)) &&
              qjs::setProperty(ctx, obj.raw(), "applied", qjs::newBool(ctx, applied)) &&
              qjs::setProperty(ctx, obj.raw(), "seq", qjs::newInt64(ctx, static_cast<std::int64_t>(seq))) &&
              qjs::setProperty(ctx, obj.raw(), "id", qjs::newInt64(ctx, static_cast<std::int64_t>(id)));
    if (ok && code != nullptr) ok = qjs::setProperty(ctx, obj.raw(), "code", qjs::newString(ctx, code));
    if (!ok) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    return obj;
}

Value makeNowObject(Impl& impl) {
    JSContext* ctx = impl.ctx;
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) return obj;
    const lift::plugin::TriggerReadings* now = impl.call_host != nullptr ? impl.call_host->now() : nullptr;
    bool ok = true;
    if (now != nullptr) {
        // ★ 有效才给值（⛔ 不编 0 / 空串）；`posValid` / `point` / `readIndex` 恒有。
        if (now->script_ok && ok) ok = qjs::setProperty(ctx, obj.raw(), "script", qjs::newString(ctx, now->script));
        if (now->pos_ok && ok) {
            ok = qjs::setProperty(ctx, obj.raw(), "pos", qjs::newInt64(ctx, static_cast<std::int64_t>(now->pos)));
        }
        if (ok) ok = qjs::setProperty(ctx, obj.raw(), "posValid", qjs::newBool(ctx, now->pos_ok));
        if (ok) {
            ok = qjs::setProperty(ctx, obj.raw(), "readIndex",
                                  qjs::newInt64(ctx, static_cast<std::int64_t>(now->read_index)));
        }
        if (ok) ok = qjs::setProperty(ctx, obj.raw(), "point", qjs::newString(ctx, pluginPointName(now->point)));
        if (now->mouse_ok && ok) {
            Value mouse = qjs::newObject(ctx);
            if (!mouse.isException()) {
                ok = qjs::setProperty(ctx, mouse.raw(), "x", qjs::newInt32(ctx, now->mouse_x)) &&
                     qjs::setProperty(ctx, mouse.raw(), "y", qjs::newInt32(ctx, now->mouse_y)) &&
                     qjs::setProperty(ctx, obj.raw(), "mouse", std::move(mouse));
            }
        }
    }
    if (!ok) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    return obj;
}

Value makeEventObject(Impl& impl, const lift::plugin::TriggerEvent& ev) {
    JSContext* ctx = impl.ctx;
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) return obj;
    bool ok = qjs::setProperty(ctx, obj.raw(), "kind", qjs::newString(ctx, ev.kind())) &&
              qjs::setProperty(ctx, obj.raw(), "name", qjs::newString(ctx, ev.name));
    if (ok) {
        // ★ `ev.trigger` = 与 runner **同源**的 state（rearm 的判据就在它上面）。
        Value trigger_obj = wrapTrigger(impl, ev.trigger);
        if (trigger_obj.isException()) {
            qjs::clearException(ctx);
            ok = false;
        } else {
            ok = qjs::setProperty(ctx, obj.raw(), "trigger", std::move(trigger_obj));
        }
    }
    if (ok && !ev.script.empty()) {
        ok = qjs::setProperty(ctx, obj.raw(), "script", qjs::newString(ctx, ev.script));
    }
    if (ok && ev.pos_ok) {
        ok = qjs::setProperty(ctx, obj.raw(), "pos", qjs::newInt64(ctx, static_cast<std::int64_t>(ev.pos)));
    }
    if (ok) ok = qjs::setProperty(ctx, obj.raw(), "posValid", qjs::newBool(ctx, ev.pos_ok));
    if (ok) ok = qjs::setProperty(ctx, obj.raw(), "point", qjs::newString(ctx, pluginPointName(ev.point)));
    if (ok) {
        ok = qjs::setProperty(ctx, obj.raw(), "readIndex",
                              qjs::newInt64(ctx, static_cast<std::int64_t>(ev.read_index)));
    }
    if (!ok) {
        qjs::clearException(ctx);
        return Value::exception(ctx);
    }
    return obj;
}

// `ctx.state`（per-Run；只有第一次回调前建一次）。`initialState` ⇒ `Object.assign({}, seed)`（浅拷贝）。
// ★ D-C2-7：沙箱里**没有 `JSON`** ⇒ 用引擎自己的 `Object.assign`（与合同写法逐字一致）。
bool ensureStateObject(Impl& impl) {
    JSContext* ctx = impl.ctx;
    if (impl.plugin_state.isObject()) return true;
    Value state = qjs::newObject(ctx);
    if (state.isException()) {
        qjs::clearException(ctx);
        return false;
    }
    if (impl.has_state_seed && impl.state_seed.isObject()) {
        Value global = qjs::getGlobalObject(ctx);
        Value object_ctor = global.isException() ? Value::exception(ctx) : qjs::getProperty(ctx, global.raw(), "Object");
        Value assign = object_ctor.isObject() ? qjs::getProperty(ctx, object_ctor.raw(), "assign")
                                              : Value::undefined(ctx);
        if (!assign.isException() && assign.isFunction()) {
            JSValueConst argv[2] = {state.raw(), impl.state_seed.raw()};
            Value ret(ctx, JS_Call(ctx, assign.raw(), JS_UNDEFINED, 2, argv));
            if (ret.isException()) {
                qjs::clearException(ctx);  // ⛔ 不让"状态种子"把整个回调打挂；空对象仍然可用
            }
        } else {
            qjs::clearException(ctx);
        }
    }
    impl.plugin_state = std::move(state);
    return true;
}

// 一个**只读**对象属性（`ctx.params` / `ctx.ui.state` 的口径；strict 下赋值 ⇒ TypeError）。
bool defineReadOnly(JSContext* ctx, JSValueConst obj, const char* name, Value value) {
    if (JS_DefinePropertyValueStr(ctx, obj, name, value.take(), JS_PROP_ENUMERABLE) < 0) {
        qjs::clearException(ctx);
        return false;
    }
    return true;
}

// `ctx.ui.state` 的数据对象（`ctx.ui` 的写入面都在它上面；C2b 才让 render 读）。
Value makeUiObject(Impl& impl) {
    JSContext* ctx = impl.ctx;
    Value ui = qjs::newObject(ctx);
    if (ui.isException()) return ui;
    bool ok = setFunction(ctx, ui.raw(), "show", jsCtxUiShow, 2, nullptr) &&
              setFunction(ctx, ui.raw(), "hide", jsCtxUiHide, 1, nullptr) &&
              setFunction(ctx, ui.raw(), "set", jsCtxUiSet, 2, nullptr) &&
              setFunction(ctx, ui.raw(), "visible", jsCtxUiVisible, 1, nullptr) &&
              setFunction(ctx, ui.raw(), "get", jsCtxUiGet, 1, nullptr);
    if (ok) {
        // `ctx.ui.state` = 数据面本身（同一个对象 ⇒ 插件侧直接读写看到的是一份）。
        ok = defineReadOnly(ctx, ui.raw(), "state", impl.ui_data.dup());
    }
    if (!ok) return Value::exception(ctx);
    return ui;
}

// 组一个 `ctx`（RunCtx）。`is_start` 只影响是否带 `now`（两者都有 —— `start` 也有读数）。
bool buildRunCtx(Impl& impl, Value* out) {
    JSContext* ctx = impl.ctx;
    if (!ensureStateObject(impl)) return false;
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) {
        qjs::clearException(ctx);
        return false;
    }
    bool ok = true;

    // ---- edit = { global: { set } } ----
    if (ok) {
        Value edit = qjs::newObject(ctx);
        Value edit_global = qjs::newObject(ctx);
        ok = !edit.isException() && !edit_global.isException();
        if (ok) ok = setFunction(ctx, edit_global.raw(), "set", jsCtxEditGlobalSet, 3, nullptr);
        if (ok) ok = qjs::setProperty(ctx, edit.raw(), "global", std::move(edit_global));
        if (ok) ok = qjs::setProperty(ctx, obj.raw(), "edit", std::move(edit));
        if (!ok) qjs::clearException(ctx);
    }
    // ---- input = { click, move, down, up, status } ----
    if (ok) {
        Value input = qjs::newObject(ctx);
        ok = !input.isException();
        if (ok) ok = setFunction(ctx, input.raw(), "click", jsCtxInputClick, 3, nullptr);
        if (ok) ok = setFunction(ctx, input.raw(), "move", jsCtxInputMove, 2, nullptr);
        if (ok) ok = setFunction(ctx, input.raw(), "down", jsCtxInputDown, 1, nullptr);
        if (ok) ok = setFunction(ctx, input.raw(), "up", jsCtxInputUp, 1, nullptr);
        if (ok) ok = setFunction(ctx, input.raw(), "status", jsCtxInputStatus, 1, nullptr);
        if (ok) ok = qjs::setProperty(ctx, obj.raw(), "input", std::move(input));
        if (!ok) qjs::clearException(ctx);
    }
    // ---- read = { global: { get } }（⛔ frameHash / frames / scriptPos 在 C2a 不存在）----
    if (ok) {
        Value read = qjs::newObject(ctx);
        Value read_global = qjs::newObject(ctx);
        ok = !read.isException() && !read_global.isException();
        if (ok) ok = setFunction(ctx, read_global.raw(), "get", jsCtxReadGlobalGet, 2, nullptr);
        if (ok) ok = qjs::setProperty(ctx, read.raw(), "global", std::move(read_global));
        if (ok) ok = qjs::setProperty(ctx, obj.raw(), "read", std::move(read));
        if (!ok) qjs::clearException(ctx);
    }
    // ---- hook = { at, list, clear }（脚本指令位置钩子；**只 RunCtx**）----
    //   ① `at(script,pos)` 只装"到点"（返回句柄）；② 句柄 `capture/stats` 读 local；③ `patch` 改值。
    //   三个能力彼此独立（触发 ≠ 修改）；实现 = `runtime/instr_hook.cpp`。
    if (ok) {
        Value hook = qjs::newObject(ctx);
        ok = !hook.isException();
        if (ok) ok = setFunction(ctx, hook.raw(), "at", jsCtxHookAt, 2, nullptr);
        if (ok) ok = setFunction(ctx, hook.raw(), "list", jsCtxHookList, 0, nullptr);
        if (ok) ok = setFunction(ctx, hook.raw(), "clear", jsCtxHookClear, 0, nullptr);
        if (ok) ok = qjs::setProperty(ctx, obj.raw(), "hook", std::move(hook));
        if (!ok) qjs::clearException(ctx);
    }    // ---- appdata = { available, path, stat }（同步只读宿主 I/O；**只 RunCtx**）----
    //   ★ `available=false` 时 `path=null`、`stat` 一律 `{exists:false, error:"no_appdata"}`
    //     （⛔ 不"有但抛"：前置检查要能无 try/catch 地读结果对象）。
    if (ok) {
        Value appdata = qjs::newObject(ctx);
        ok = !appdata.isException();
        lift::ui::PluginCallHost* host = impl.call_host;
        const bool available = host != nullptr && host->appDataAvailable();
        if (ok) ok = defineReadOnly(ctx, appdata.raw(), "available", qjs::newBool(ctx, available));
        if (ok) {
            const std::string path = available ? host->appDataPath() : std::string();
            ok = defineReadOnly(ctx, appdata.raw(), "path",
                                path.empty() ? Value::null(ctx) : qjs::newString(ctx, path));
        }
        if (ok) ok = setFunction(ctx, appdata.raw(), "stat", jsCtxAppDataStat, 1, nullptr);
        if (ok) ok = defineReadOnly(ctx, obj.raw(), "appdata", std::move(appdata));
        if (!ok) qjs::clearException(ctx);
    }
    // ---- config = { available, domain(id) }（域式 JSON 配置；**RunCtx**）----
    //   ★ `--overlay` 的经典回退 HUD 用同一个桥、同一份 JS 形状（`host.config`，D2）；
    //     ★ 2026-11-26 起 **`RenderCtx` 也有 `config`**（父级裁决：7→8 成员）——
    //     它与 `host.config` 复用**同一份** `makeConfigObject(impl, /*render=*/true)`。
    //   ★ 没有 `--appdata` ⇒ `available=false` 且 `domain()` 抛 `Error("no_appdata")`
    //     （⛔ 不做内存态：内建插件必须先看 `available` 再回退 params / 默认值）。
    //   ★ 域对象没有缓存：`domain(id)` 每次新建一个绑定同一 store 的对象（同一个 appdata 共享）。
    if (ok) {
        Value config = makeConfigObject(impl, /*render=*/false);
        if (config.isException()) {
            qjs::clearException(ctx);
            ok = false;
        } else {
            ok = defineReadOnly(ctx, obj.raw(), "config", std::move(config));
        }
    }
    // ---- trigger = { afterScript, afterPosition, afterDelay, atReadPoint } ----
    if (ok) {
        Value trigger = qjs::newObject(ctx);
        ok = !trigger.isException();
        if (ok) ok = setFunction(ctx, trigger.raw(), "afterScript", jsCtxTriggerAfterScript, 2, nullptr);
        if (ok) ok = setFunction(ctx, trigger.raw(), "afterPosition", jsCtxTriggerAfterPosition, 4, nullptr);
        if (ok) ok = setFunction(ctx, trigger.raw(), "afterDelay", jsCtxTriggerAfterDelay, 2, nullptr);
        if (ok) ok = setFunction(ctx, trigger.raw(), "atReadPoint", jsCtxTriggerAtReadPoint, 3, nullptr);
        if (ok) ok = qjs::setProperty(ctx, obj.raw(), "trigger", std::move(trigger));
        if (!ok) qjs::clearException(ctx);
    }
    // ---- profile = { begin, end } ----
    if (ok) {
        Value profile = qjs::newObject(ctx);
        ok = !profile.isException();
        if (ok) ok = setFunction(ctx, profile.raw(), "begin", jsCtxProfileBegin, 1, nullptr);
        if (ok) ok = setFunction(ctx, profile.raw(), "end", jsCtxProfileEnd, 0, nullptr);
        if (ok) ok = qjs::setProperty(ctx, obj.raw(), "profile", std::move(profile));
        if (!ok) qjs::clearException(ctx);
    }
    // ---- ui / state / params / log / fail / finish / quit ----
    if (ok) {
        Value ui = makeUiObject(impl);
        if (ui.isException()) {
            qjs::clearException(ctx);
            ok = false;
        } else {
            ok = qjs::setProperty(ctx, obj.raw(), "ui", std::move(ui));
        }
    }
    if (ok) ok = defineReadOnly(ctx, obj.raw(), "state", impl.plugin_state.dup());
    if (ok) ok = defineReadOnly(ctx, obj.raw(), "params", impl.plugin_params.dup());
    if (ok) ok = setFunction(ctx, obj.raw(), "log", jsCtxLog, 3, nullptr);
    if (ok) ok = setFunction(ctx, obj.raw(), "fail", jsCtxFail, 2, nullptr);
    if (ok) ok = setFunction(ctx, obj.raw(), "finish", jsCtxFinish, 2, nullptr);
    if (ok) ok = setFunction(ctx, obj.raw(), "quit", jsCtxQuit, 1, nullptr);
    // ---- now（**每次回调新建**的值快照；D-C2-7）----
    if (ok) {
        Value now = makeNowObject(impl);
        if (now.isException()) {
            qjs::clearException(ctx);
            ok = false;
        } else {
            ok = defineReadOnly(ctx, obj.raw(), "now", std::move(now));
        }
    }
    if (!ok) return false;
    *out = std::move(obj);
    return true;
}

// =======================================================================================
// `RenderCtx`（C2b）：**每帧新建**的值对象；成员 = `ui` / `draw` / `read` / `game` / `params` /
// `log` / `profile` / `config`
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.4c（`render` 只画 / `onTrigger` 只改 UI 状态）/
//       §4.6（`render(ctx: RenderCtx)`）/ §5 C2（`plugin/ui-state` / `plugin/ui-draw`）。
// 裁决：`.tmp/plugin-runtime/c2-decisions.md` D-C2-3（不支持的 style 字段 ⇒ **TypeError**，
//       ⛔ 不静默忽略）、D-C2-6（`read.frameHash` 不安装）、D-C2-7（`run`/`dispatch` 留 C5）。
//       ★ 2026-11-26 父级裁决：`config` **加回** `RenderCtx`（7→8）—— 当初撤它的理由是
//       "预设模式没有插件注册 schema"，但它挡住了 `--overlay` 下最基本的设置页 / 预配置；
//       与经典回退 HUD 的 `host.config` **同一份**实现（见 `makeConfigObject`）。
//
// ★ **成员表写死 = d.ts 的 `RenderCtx` 减 `run`**（`run`/`dispatch` 属 C5）：
//   ⛔ 没有 `edit`/`input`（RunCtx 的写面；render 不做 guest 写）、`state`/`now`/`trigger`/
//   `fail`/`finish`/`quit`（Run 语境），也⛔ 没有 `sample`/`screenshot`。
//   `plugin/render-surface` 用 `Object.keys(ctx)` 钉住**恰好**这八个成员（多一个也红）。
// ★ S2：`profile` 是**render 专用**的一份（`begin/end` 都是 `void`；trampoline 判 `impl.current`），
//   与 RunCtx 的 `ctx.profile` 同形但**不复用**它的门（存下来的 RunCtx 仍被拒）。
// ★ S2：`game` 除 `localFrames()/mouse()` 两个方法外，还装经典 `game` 的只读 getter
//   （frame/fps/script/worst/click/hover/active）—— `hud.js` 迁到插件后读数必须逐字等价。
// ★ 数据面：`ui` 与 RunCtx **共用** `Impl::ui_data` / `ui_visible`（同一个 JS 对象 / 同一个集合）
//   ⇒ "onTrigger 写入、下一次 render 可见"与"render 内写入本帧后续可见"是同一条事实。
// ★ `draw` 只碰 `UiBuilder`（1280×720 参考系；z 序 = 调用顺序 —— `z` 恒 0、绘制序按 `seq`）。
//   每帧 clear 由宿主（present_sdl）做，⛔ 脚本没有 clear 入口。
// =======================================================================================

// 位置参数里的整数（`ctx.draw.rect/text` 的 x/y/w/h）：非数字 / 非有限 ⇒ false（调用方转 TypeError）。
// ★ 先判有限：`JS_ToInt32(NaN) == 0`（ECMAScript ToInt32 的定义）⇒ 静默变 0 是错。
bool readDrawInt(JSContext* ctx, JSValueConst v, const char* name, std::int32_t* out, std::string* why) {
    if (!JS_IsNumber(v)) {
        *why = std::string(name) + " 必须是数字";
        return false;
    }
    double d = 0.0;
    if (!qjs::toDouble(ctx, v, &d) || !std::isfinite(d)) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 必须是有限数字";
        return false;
    }
    if (!qjs::toInt32(ctx, v, out)) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 无法转成 32 位整数";
        return false;
    }
    return true;
}

// `Element` 里**没有表示**的 style 字段 ⇒ 响亮 TypeError（D-C2-3；⛔ 不静默忽略）。
// ★ false ⇒ **已经**抛了 TypeError（调用方直接 `return JS_EXCEPTION`）。
// ★ 为什么这条要存在：d.ts 已经声明了 `stroke/width/alpha/align`；静默忽略会让"声明与实现"
//   长期说谎，而且**没有症状**（画出来的东西少一圈描边，没人知道是宿主吞了还是插件没写）。
bool rejectUnsupportedStyle(JSContext* ctx, JSValueConst style, const char* const* blocked) {
    for (std::size_t i = 0; blocked[i] != nullptr; ++i) {
        Value v = qjs::getProperty(ctx, style, blocked[i]);
        if (v.isException()) {
            qjs::clearException(ctx);
            JS_ThrowTypeError(ctx, "ctx.draw: style.%s 读取失败", blocked[i]);
            return false;
        }
        if (!v.isUndefined()) {
            JS_ThrowTypeError(ctx, "ctx.draw: style.%s is not supported", blocked[i]);
            return false;
        }
    }
    return true;
}

// `stroke` / `width` / `alpha` 在**两种** style 上都没有表示；`align` 只在 text/measure 的
// `TextStyle` 里出现（d.ts 的 `DrawStyle` 没有它）。★ S2：`key` 只对 rect 有意义（命中区），
// 在 text/measure 上传它 ⇒ 同样响亮 TypeError。
const char* const kRectBlockedStyle[] = {"stroke", "width", "alpha", nullptr};
const char* const kTextBlockedStyle[] = {"stroke", "width", "alpha", "align", "key", nullptr};

// `style.fill`（可选；缺省 = `dflt`）。★ 复用 `parseColor`（唯一色解析；⛔ 不写第二份）。
bool readStyleFill(JSContext* ctx, JSValueConst style, const Color& dflt, Color* out, std::string* why) {
    Value v = qjs::getProperty(ctx, style, "fill");
    if (v.isException()) {
        qjs::clearException(ctx);
        *why = "style.fill 读取失败";
        return false;
    }
    return parseColor(ctx, v.raw(), dflt, out, why);
}

// `style.size`（可选；缺省 16；必须是**正**有限整数 —— 与 `ui.measure` 同一条边界）。
bool readStyleSize(JSContext* ctx, JSValueConst style, std::int32_t* out, std::string* why) {
    Value v = qjs::getProperty(ctx, style, "size");
    if (v.isException()) {
        qjs::clearException(ctx);
        *why = "style.size 读取失败";
        return false;
    }
    if (v.isUndefined()) {
        *out = 16;
        return true;
    }
    if (!v.isNumber()) {
        *why = "style.size 必须是数字";
        return false;
    }
    double d = 0.0;
    if (!qjs::toDouble(ctx, v.raw(), &d) || !std::isfinite(d)) {
        qjs::clearException(ctx);
        *why = "style.size 必须是有限数字";
        return false;
    }
    std::int32_t size = 0;
    if (!qjs::toInt32(ctx, v.raw(), &size)) {
        qjs::clearException(ctx);
        *why = "style.size 无法转成 32 位整数";
        return false;
    }
    if (size <= 0) {
        *why = "style.size 必须为正（参考像素）";
        return false;
    }
    *out = size;
    return true;
}

// `ctx.draw.rect(x, y, w, h, style?)` → `UiBuilder::rect`（`style.fill` 映射；其余见上）。
JSValue jsCtxDrawRect(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->builder == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.draw.rect: 只能在 render() 内调用");
        }
        if (argc < 4) return JS_ThrowTypeError(ctx, "ctx.draw.rect(x, y, w, h, style?)");
        static const char* const kNames[4] = {"x", "y", "w", "h"};
        std::int32_t args[4] = {0, 0, 0, 0};
        std::string why;
        for (int i = 0; i < 4; ++i) {
            if (!readDrawInt(ctx, argv[i], kNames[i], &args[i], &why)) {
                return JS_ThrowTypeError(ctx, "ctx.draw.rect: %s", why.c_str());
            }
        }
        Color fill{0, 0, 0, 255};
        std::string key;
        if (argc >= 5 && !JS_IsUndefined(argv[4]) && !JS_IsNull(argv[4])) {
            if (!JS_IsObject(argv[4])) return JS_ThrowTypeError(ctx, "ctx.draw.rect: style 必须是对象");
            if (!rejectUnsupportedStyle(ctx, argv[4], kRectBlockedStyle)) return JS_EXCEPTION;
            if (!readStyleFill(ctx, argv[4], Color{0, 0, 0, 255}, &fill, &why)) {
                return JS_ThrowTypeError(ctx, "ctx.draw.rect: %s", why.c_str());
            }
            // ★ S2：命中键（与 `ui.rect({key})` 同一个 `Element.key`）。非空 ⇒ 这条 Rect 是命中区
            //   （resolved rect = 命中区）；⛔ 不在宿主侧做按钮（外观 / 状态在 kit.js）。
            if (!readOptionalText(ctx, argv[4], "key", &key, &why)) {
                return JS_ThrowTypeError(ctx, "ctx.draw.rect: %s", why.c_str());
            }
        }
        // z 恒 0 ⇒ 绘制序 = 调用顺序（`drawsBefore`：z 升序、同 z 按 seq）。⛔ 不读 `style.z`
        // （d.ts 的 `DrawStyle` 没有它；声明之外的东西不接受、也不假装接受）。
        Element& element = impl->builder->rect(args[0], args[1], args[2], args[3], fill, 0);
        element.key = std::move(key);
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.draw.rect: 宿主异常（C++ exception at script boundary）");
    }
}

// `ctx.draw.text(x, y, text, style?)` → `UiBuilder::text`（`style.size`/`style.fill` 映射；
// `align`/`stroke`/`width`/`alpha` ⇒ TypeError）。
JSValue jsCtxDrawText(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->builder == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.draw.text: 只能在 render() 内调用");
        }
        if (argc < 3) return JS_ThrowTypeError(ctx, "ctx.draw.text(x, y, text, style?)");
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::string why;
        if (!readDrawInt(ctx, argv[0], "x", &x, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.draw.text: %s", why.c_str());
        }
        if (!readDrawInt(ctx, argv[1], "y", &y, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.draw.text: %s", why.c_str());
        }
        if (!JS_IsString(argv[2])) return JS_ThrowTypeError(ctx, "ctx.draw.text: text 必须是字符串");
        std::string text;
        if (!qjs::toString(ctx, argv[2], &text)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.draw.text: text 无法转成 UTF-8 字符串");
        }
        std::int32_t size = 16;
        Color color{255, 255, 255, 255};
        bool bold = false;
        if (argc >= 4 && !JS_IsUndefined(argv[3]) && !JS_IsNull(argv[3])) {
            if (!JS_IsObject(argv[3])) return JS_ThrowTypeError(ctx, "ctx.draw.text: style 必须是对象");
            if (!rejectUnsupportedStyle(ctx, argv[3], kTextBlockedStyle)) return JS_EXCEPTION;
            if (!readStyleSize(ctx, argv[3], &size, &why)) {
                return JS_ThrowTypeError(ctx, "ctx.draw.text: %s", why.c_str());
            }
            if (!readStyleFill(ctx, argv[3], Color{255, 255, 255, 255}, &color, &why)) {
                return JS_ThrowTypeError(ctx, "ctx.draw.text: %s", why.c_str());
            }
            // ★ S2：`bold`（`TextStyle.bold`；缺省 false）。`kit.js` 的按钮标签是 14px bold ⇒
            //   与 `ui.text({bold})` 同一条渲染路径（否则 HUD 迁到插件后像素会变）。
            if (!readBool(ctx, argv[3], "bold", false, &bold, &why)) {
                return JS_ThrowTypeError(ctx, "ctx.draw.text: %s", why.c_str());
            }
        }
        impl->builder->text(x, y, std::move(text), size, color, bold, 0);
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.draw.text: 宿主异常（C++ exception at script boundary）");
    }
}

// `ctx.draw.measure(text, style?)` → 宿主字体度量 `{width, height}`。
// ★ **返回值键是 `width`/`height`**（d.ts；⛔ 不是 overlay 的 `ui.measure` 的 `{w,h}` —— 两套名字
//   是两个消费者，别合并）。
// ★ 没有 `FontEngine` ⇒ TypeError（⛔ 不返回 0 尺寸冒充可用；与 `ui.measure` 同一条）。
JSValue jsCtxDrawMeasure(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->builder == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.draw.measure: 只能在 render() 内调用");
        }
        if (impl->font == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.draw.measure: 宿主字体不可用（font == nullptr）");
        }
        if (argc < 1) return JS_ThrowTypeError(ctx, "ctx.draw.measure(text, style?)");
        if (!JS_IsString(argv[0])) return JS_ThrowTypeError(ctx, "ctx.draw.measure: text 必须是字符串");
        std::string text;
        if (!qjs::toString(ctx, argv[0], &text)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.draw.measure: text 无法转成 UTF-8 字符串");
        }
        std::int32_t size = 16;
        bool bold = false;
        std::string why;
        if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1])) {
            if (!JS_IsObject(argv[1])) return JS_ThrowTypeError(ctx, "ctx.draw.measure: style 必须是对象");
            if (!rejectUnsupportedStyle(ctx, argv[1], kTextBlockedStyle)) return JS_EXCEPTION;
            if (!readStyleSize(ctx, argv[1], &size, &why)) {
                return JS_ThrowTypeError(ctx, "ctx.draw.measure: %s", why.c_str());
            }
            // ★ S2 复审 minor 4：`bold` 会**真的**改宽度 / 高度 ⇒ ⛔ 不许静默按非粗体量字
            //   （D-C2-3 的同类病）。`fill` 不影响度量：接受但不用它。
            if (!readBool(ctx, argv[1], "bold", false, &bold, &why)) {
                return JS_ThrowTypeError(ctx, "ctx.draw.measure: %s", why.c_str());
            }
        }
        int width = 0;
        int height = 0;
        std::string merr;
        if (!measureText(text, size, bold, *impl->font, &width, &height, &merr)) {
            return JS_ThrowTypeError(ctx, "ctx.draw.measure: 宿主字体度量失败：%s", merr.c_str());
        }
        Value out = qjs::newObject(ctx);
        if (out.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.draw.measure: 结果对象写入失败");
        }
        if (!qjs::setProperty(ctx, out.raw(), "width", qjs::newInt32(ctx, width)) ||
            !qjs::setProperty(ctx, out.raw(), "height", qjs::newInt32(ctx, height))) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.draw.measure: 结果对象写入失败");
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.draw.measure: 宿主异常（C++ exception at script boundary）");
    }
}

// ---------------------------------------------------------------------------------------
// `read.frames()` / `read.scriptPos()` / `game.localFrames()`：**唯一**来路 = `UiFrameReadings::frames`
// ★ 口径（与 overlay 的 `game.local_frames()` 同一条）：没有源 / 宿主报 err / 空表 ⇒ `undefined`
//   （⛔ 不编空数组、⛔ 不拿 frames[0] 冒充栈顶）。
// ---------------------------------------------------------------------------------------
bool renderFrameList(const UiFrameReadings& r, std::vector<UiFrameInfo>* out) {
    if (r.frames == nullptr) return false;
    std::string frame_err;
    *out = r.frames->localFrames(&frame_err);
    return frame_err.empty();
}

const UiFrameInfo* topRenderFrame(const std::vector<UiFrameInfo>& frames) {
    for (const UiFrameInfo& frame : frames) {
        if (frame.top) return &frame;
    }
    return nullptr;  // `cur` 不活动 ⇒ 这一刻没有执行点（⛔ 不拿 frames[0] 顶替）
}

// d.ts `FrameInfo` = `{ index, script, pos?, hash?, presented? }`：
//   · `index` ↔ 宿主槽号（`UiFrameInfo::slot`；README 的"index"就是玩家的槽号）；
//   · `pos` **只有**宿主给了值才装（非 top 帧没有执行点 ⇒ 缺键，⛔ 不拿 0 冒充）；
//   · `hash` / `presented` 在 runtime 里**没有源**（唯一实现是宿主的 `Presenter::lastHash`）
//     ⇒ **不安装**（C2 不编 0 / false；d.ts 保留可选声明 —— C4 的策略同 `read.frameHash`）。
Value makeRenderFrameObject(JSContext* ctx, const UiFrameInfo& frame) {
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) return obj;
    if (!setFrameProperty(ctx, obj.raw(), "index",
                          qjs::newInt64(ctx, static_cast<std::int64_t>(frame.slot))) ||
        !setFrameProperty(ctx, obj.raw(), "script", qjs::newString(ctx, frame.script)) ||
        // ★ S2：`top` = 当前执行点（`slot == cur`）—— 与经典 `game.local_frames()` 的 `top` **同一个
        //   布尔**。HUD 的 `script` 行只认 top 项（⛔ 不拿 frames[0] 冒充栈顶）；没有它，插件侧
        //   只能靠 `pos` 猜，而 top 帧在位置读不到时也没有 `pos`。
        !setFrameProperty(ctx, obj.raw(), "top", qjs::newBool(ctx, frame.top))) {
        return Value::exception(ctx);
    }
    if (frame.pos_ok &&
        !setFrameProperty(ctx, obj.raw(), "pos", qjs::newInt64(ctx, static_cast<std::int64_t>(frame.pos)))) {
        return Value::exception(ctx);
    }
    return obj;
}

bool appendRenderFrame(JSContext* ctx, JSValueConst arr, std::uint32_t index, const UiFrameInfo& frame) {
    Value item = makeRenderFrameObject(ctx, frame);
    if (item.isException()) {
        qjs::clearException(ctx);
        return false;
    }
    if (!setElement(ctx, arr, index, std::move(item))) {
        qjs::clearException(ctx);
        return false;
    }
    return true;
}

Value makeRenderFramesArray(JSContext* ctx, const std::vector<UiFrameInfo>& frames) {
    Value arr = newArray(ctx);
    if (arr.isException()) return arr;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (!appendRenderFrame(ctx, arr.raw(), static_cast<std::uint32_t>(i), frames[i])) {
            return Value::exception(ctx);
        }
    }
    return arr;
}

JSValue jsCtxReadFrames(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        const Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->current == nullptr) return JS_UNDEFINED;
        std::vector<UiFrameInfo> frames;
        if (!renderFrameList(*impl->current, &frames) || frames.empty()) return JS_UNDEFINED;
        Value arr = makeRenderFramesArray(ctx, frames);
        if (arr.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.read.frames: 分配帧数组失败");
        }
        return arr.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.read.frames: 宿主异常（C++ exception at script boundary）");
    }
}

// `ctx.read.scriptPos()`（d.ts `ScriptPos = {script, pos?, valid}`）：
//   · 没有源 / err / 空表 / **没有 top 帧** ⇒ `undefined`（⛔ 不编 `{script:"", pos:0}`）；
//   · 有 top 帧 ⇒ `script` + `valid`（= `pos_ok`）恒有；`pos` **只有**真的读到才装
//     （`valid:false` 时缺键 —— 需求单口径"不要当成 0"）。
JSValue jsCtxReadScriptPos(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        const Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->current == nullptr) return JS_UNDEFINED;
        std::vector<UiFrameInfo> frames;
        if (!renderFrameList(*impl->current, &frames) || frames.empty()) return JS_UNDEFINED;
        const UiFrameInfo* top = topRenderFrame(frames);
        if (top == nullptr) return JS_UNDEFINED;
        Value obj = qjs::newObject(ctx);
        if (obj.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.read.scriptPos: 结果对象写入失败");
        }
        if (!setFrameProperty(ctx, obj.raw(), "script", qjs::newString(ctx, top->script)) ||
            !setFrameProperty(ctx, obj.raw(), "valid", qjs::newBool(ctx, top->pos_ok))) {
            return JS_ThrowTypeError(ctx, "ctx.read.scriptPos: 结果对象写入失败");
        }
        if (top->pos_ok &&
            !setFrameProperty(ctx, obj.raw(), "pos",
                              qjs::newInt64(ctx, static_cast<std::int64_t>(top->pos)))) {
            return JS_ThrowTypeError(ctx, "ctx.read.scriptPos: 结果对象写入失败");
        }
        return obj.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.read.scriptPos: 宿主异常（C++ exception at script boundary）");
    }
}

// `game.mouse()`（d.ts `GameRead.mouse()`）：**方法**（⛔ 不是 overlay 的 getter）；没有读数 ⇒ `undefined`。
// ★ C2b-2：三键电平**有读数源时**才安装 `buttons`（`setMouseButtons` 的唯一装配点）；没有源 ⇒
//   **缺键**（⛔ 不编 `{left:false,…}`：那会让"鼠标按着"与"没有源"长得一样）。
JSValue jsRenderGameMouse(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        const Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->current == nullptr) return JS_UNDEFINED;
        const UiFrameReadings& r = *impl->current;
        if (!r.mouse_ok) return JS_UNDEFINED;
        Value obj = qjs::newObject(ctx);
        if (obj.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "game.mouse: 结果对象写入失败");
        }
        if (!qjs::setProperty(ctx, obj.raw(), "x", qjs::newInt32(ctx, r.mouse_x)) ||
            !qjs::setProperty(ctx, obj.raw(), "y", qjs::newInt32(ctx, r.mouse_y))) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "game.mouse: 结果对象写入失败");
        }
        if (!setMouseButtons(ctx, obj.raw(), r)) {
            return JS_ThrowTypeError(ctx, "game.mouse: buttons 写入失败");
        }
        return obj.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "game.mouse: 宿主异常（C++ exception at script boundary）");
    }
}

// `game.localFrames()`（d.ts 的**方法**；与 `read.frames()` 同一份载荷、同一个"没有源 ⇒ undefined"）。
JSValue jsRenderGameLocalFrames(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/,
                                JSValueConst* /*argv*/) {
    try {
        const Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->current == nullptr) return JS_UNDEFINED;
        std::vector<UiFrameInfo> frames;
        if (!renderFrameList(*impl->current, &frames) || frames.empty()) return JS_UNDEFINED;
        Value arr = makeRenderFramesArray(ctx, frames);
        if (arr.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "game.localFrames: 分配帧数组失败");
        }
        return arr.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "game.localFrames: 宿主异常（C++ exception at script boundary）");
    }
}

// 组一个 `RenderCtx`（**每帧新建**）。成功 ⇒ `*out` 有值；失败 ⇒ false（异常已取走/清掉）。
bool buildRenderCtx(Impl& impl, Value* out) {
    JSContext* ctx = impl.ctx;
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) {
        qjs::clearException(ctx);
        return false;
    }
    bool ok = true;
    // ---- ui（读写；数据面与 RunCtx 共用 `Impl::ui_data` / `ui_visible`）----
    if (ok) {
        Value ui = makeUiObject(impl);
        if (ui.isException()) {
            qjs::clearException(ctx);
            ok = false;
        } else {
            ok = qjs::setProperty(ctx, obj.raw(), "ui", std::move(ui));
        }
        if (!ok) qjs::clearException(ctx);
    }
    // ---- draw = { rect, text, measure } ----
    if (ok) {
        Value draw = qjs::newObject(ctx);
        ok = !draw.isException();
        if (ok) ok = setFunction(ctx, draw.raw(), "rect", jsCtxDrawRect, 4, nullptr);
        if (ok) ok = setFunction(ctx, draw.raw(), "text", jsCtxDrawText, 3, nullptr);
        if (ok) ok = setFunction(ctx, draw.raw(), "measure", jsCtxDrawMeasure, 1, nullptr);
        if (ok) ok = qjs::setProperty(ctx, obj.raw(), "draw", std::move(draw));
        if (!ok) qjs::clearException(ctx);
    }
    // ---- read = { global: { get }, frames(), scriptPos() }（⛔ frameHash：C4；C2 不安装）----
    if (ok) {
        Value read = qjs::newObject(ctx);
        Value read_global = qjs::newObject(ctx);
        ok = !read.isException() && !read_global.isException();
        if (ok) ok = setFunction(ctx, read_global.raw(), "get", jsCtxReadGlobalGet, 2, nullptr);
        if (ok) ok = qjs::setProperty(ctx, read.raw(), "global", std::move(read_global));
        if (ok) ok = setFunction(ctx, read.raw(), "frames", jsCtxReadFrames, 0, nullptr);
        if (ok) ok = setFunction(ctx, read.raw(), "scriptPos", jsCtxReadScriptPos, 0, nullptr);
        if (ok) ok = qjs::setProperty(ctx, obj.raw(), "read", std::move(read));
        if (!ok) qjs::clearException(ctx);
    }
    // ---- game = { localFrames(), mouse(), frame/fps/script/worst/click/hover/active } ----
    //   ★ S2：`hud.js` 迁到插件后要**同一份读数**（行序 / 文案 / 交互不能变）⇒ 把经典 `game` 的
    //     只读 getter 也装到 `ctx.game` 上（**同一个** `jsGameProperty` 实现、同一份 `impl.current`）。
    //     `mouse` 仍是**方法**（插件契约；经典的是 getter），`localFrames` 沿用方法名。
    //   ★ 读数没有值 ⇒ `undefined`（⛔ 不编 0 / 空对象），与经典 `game.*` 逐条同口径。
    if (ok) {
        Value game = qjs::newObject(ctx);
        ok = !game.isException();
        if (ok) ok = setFunction(ctx, game.raw(), "localFrames", jsRenderGameLocalFrames, 0, nullptr);
        if (ok) ok = setFunction(ctx, game.raw(), "mouse", jsRenderGameMouse, 0, nullptr);
        if (ok) {
            ok = defineGetterOnly(ctx, game.raw(), "frame", nullptr, jsGameProperty,
                                  static_cast<int>(GameProp::Frame), nullptr);
        }
        if (ok) {
            ok = defineGetterOnly(ctx, game.raw(), "fps", nullptr, jsGameProperty,
                                  static_cast<int>(GameProp::Fps), nullptr);
        }
        if (ok) {
            ok = defineGetterOnly(ctx, game.raw(), "script", nullptr, jsGameProperty,
                                  static_cast<int>(GameProp::Script), nullptr);
        }
        if (ok) {
            ok = defineGetterOnly(ctx, game.raw(), "worst", nullptr, jsGameProperty,
                                  static_cast<int>(GameProp::Worst), nullptr);
        }
        if (ok) {
            ok = defineGetterOnly(ctx, game.raw(), "click", nullptr, jsGameProperty,
                                  static_cast<int>(GameProp::Click), nullptr);
        }
        if (ok) {
            ok = defineGetterOnly(ctx, game.raw(), "hover", nullptr, jsGameProperty,
                                  static_cast<int>(GameProp::Hover), nullptr);
        }
        if (ok) {
            ok = defineGetterOnly(ctx, game.raw(), "active", nullptr, jsGameProperty,
                                  static_cast<int>(GameProp::Active), nullptr);
        }
        // ★ D4：F9 的按下沿计数（`ctx.game.overlayKeySeq`；与经典 `game.overlayKeySeq` 同一个
        //   `jsGameProperty` / 同一份 readings；无源 ⇒ undefined）。
        if (ok) {
            ok = defineGetterOnly(ctx, game.raw(), "overlayKeySeq", nullptr, jsGameProperty,
                                  static_cast<int>(GameProp::OverlayKeySeq), nullptr);
        }
        if (ok) ok = qjs::setProperty(ctx, obj.raw(), "game", std::move(game));
        if (!ok) qjs::clearException(ctx);
    }
    // ---- profile = { begin, end }（S2；render 专用的 trampoline，见 jsRenderProfile*）----
    if (ok) {
        Value profile = qjs::newObject(ctx);
        ok = !profile.isException();
        if (ok) ok = setFunction(ctx, profile.raw(), "begin", jsRenderProfileBegin, 1, nullptr);
        if (ok) ok = setFunction(ctx, profile.raw(), "end", jsRenderProfileEnd, 0, nullptr);
        if (ok) ok = qjs::setProperty(ctx, obj.raw(), "profile", std::move(profile));
        if (!ok) qjs::clearException(ctx);
    }
    // ---- params（只读；与 RunCtx 同一个对象）/ log ----
    if (ok) ok = defineReadOnly(ctx, obj.raw(), "params", impl.plugin_params.dup());
    if (ok) ok = setFunction(ctx, obj.raw(), "log", jsCtxLog, 3, nullptr);
    // ---- config = { available, domain(id) }（★ 2026-11-26 加回：RenderCtx 第 8 个成员）----
    //   ★ **复用**经典 `host.config` 那一份 `makeConfigObject(impl, /*render=*/true)`：同一份
    //     校验 / 转换 / 回执（⛔ 不复制第二套）。门也同 `host.config`：config / domain 对象带
    //     `__render`，`configCall` 要求 `impl.current` + `builder` 非空 ⇒ 出 render 再调
    //     `domain()/get()/…` 一律 TypeError（存下 ctx 跨帧用也不行）。
    //   ★ 没有桥 / 没有 `--appdata` ⇒ `available=false`（⛔ 不是缺成员）：HUD 要靠
    //     "成员在不在"区分"宿主没接桥"与"接桥但没 appdata"（两种状态行文案）。
    if (ok) {
        Value config = makeConfigObject(impl, /*render=*/true);
        if (config.isException()) {
            qjs::clearException(ctx);
            ok = false;
        } else {
            ok = defineReadOnly(ctx, obj.raw(), "config", std::move(config));
        }
    }
    if (!ok) return false;
    *out = std::move(obj);
    return true;
}

// =======================================================================================
// `ctx.*` 的 trampoline（每个都：取 impl → 判装配 → catch(...)）
// =======================================================================================
JSValue jsCtxEditGlobalSet(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->editWired()) {
            return JS_ThrowTypeError(ctx, "ctx.edit.global.set: 宿主没有接 EditSink（⛔ 不静默 no-op）");
        }
        if (argc < 3) return JS_ThrowTypeError(ctx, "ctx.edit.global.set(kind, id, value)");
        lift::GlobalKind kind = lift::GlobalKind::Int;
        std::string why;
        if (!parseGlobalKindArg(ctx, argv[0], &kind, &why)) return JS_ThrowTypeError(ctx, "ctx.edit.global.set: %s", why.c_str());
        std::int64_t id = 0;
        double d = 0.0;
        if (!qjs::toDouble(ctx, argv[1], &d) || !std::isfinite(d)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.edit.global.set: id 必须是数字");
        }
        id = static_cast<std::int64_t>(d);
        if (id < 0 || id > 0xFFFFFFFFll) {
            return JS_ThrowTypeError(ctx, "ctx.edit.global.set: id 越界（0..0xFFFFFFFF）");
        }
        lift::GlobalValue value;
        value.kind = kind;
        if (kind == lift::GlobalKind::Float) {
            if (!qjs::toDouble(ctx, argv[2], &value.f64) || !std::isfinite(value.f64)) {
                qjs::clearException(ctx);
                return JS_ThrowTypeError(ctx, "ctx.edit.global.set: float 值必须是有限数字");
            }
        } else if (kind == lift::GlobalKind::String) {
            return JS_ThrowTypeError(ctx, "ctx.edit.global.set: 'string' 型 global 本实现未实现");
        } else {
            std::uint32_t u = 0;
            if (!qjs::toUint32(ctx, argv[2], &u)) {
                qjs::clearException(ctx);
                return JS_ThrowTypeError(ctx, "ctx.edit.global.set: int/uint 值必须是数字");
            }
            value.u32 = u;
        }
        const char* code = nullptr;
        const bool accepted = host->editGlobalSet(kind, static_cast<std::uint32_t>(id), value, &code);
        // Edit 是**立即**的：accepted = applied ⇒ receipt 里两者同值（⛔ 不是"排队成功"）。
        Value out = makeReceiptObject(ctx, accepted, false, accepted, code, 0, 0);
        if (out.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.edit.global.set: 回执对象写入失败");
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.edit.global.set: 宿主异常（C++ exception at script boundary）");
    }
}

// =====================================================================================
// =====================================================================================
// `ctx.hook` —— 脚本指令位置钩子（语言无关实现 = `runtime/instr_hook.cpp`）
//
// ★ 三种能力**彼此独立**（用户裁决 2026-11-26 晚：触发 ≠ 修改）：
//   ① 触发 `ctx.hook.at(script, pos)` → 句柄（**纯观察**：只计数，⛔ 不读不写 guest）；
//   ② 读   `handle.capture({ints, ptrs})` + `handle.stats()`（最近一次命中的 local 读数）；
//   ③ 改   `handle.patch(target, value, {maxHits})`（这条指令**读之前**覆盖一个 dword，
//          下一条脚本指令派发前恢复 ⇒ 净变化 0；⛔ 不是持久写，持久写是 `ctx.edit.global.set`）。
//   ⇒ 可以只触发、只读、或装好触发后再决定改什么（`patch` 可反复换 / 用 `{enabled:false}` 撤）。
//
// 句柄 = 普通对象 `{ id, patch, capture, stats, clear }`；函数从 `this.id` 取句柄（⛔ 不要解构后调用）。
// =====================================================================================
Value hookStatusObject(JSContext* ctx, const lift::InstrHookStatus& r);
Value hookStatusArray(JSContext* ctx, const std::vector<lift::InstrHookStatus>& rows);
JSValue jsHookPatch(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsHookCapture(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsHookStats(JSContext*, JSValueConst, int, JSValueConst*);
JSValue jsHookClear(JSContext*, JSValueConst, int, JSValueConst*);

bool readIndexArray(JSContext* ctx, JSValueConst obj, const char* name, std::vector<std::uint32_t>* out,
                    std::string* why) {
    Value v = qjs::getProperty(ctx, obj, name);
    if (v.isException()) {
        qjs::clearException(ctx);
        *why = std::string(name) + " 读取失败";
        return false;
    }
    if (v.isUndefined()) return true;
    if (!JS_IsArray(v.raw())) {
        *why = std::string(name) + " 必须是数组";
        return false;
    }
    Value len_v = qjs::getProperty(ctx, v.raw(), "length");
    if (len_v.isException()) {
        qjs::clearException(ctx);
        *why = std::string(name) + ".length 读取失败";
        return false;
    }
    std::uint32_t len = 0;
    if (!qjs::toUint32(ctx, len_v.raw(), &len)) {
        qjs::clearException(ctx);
        *why = std::string(name) + ".length 必须是数字";
        return false;
    }
    if (len > 8) {
        *why = std::string(name) + " 最多 8 项";
        return false;
    }
    for (std::uint32_t i = 0; i < len; i += 1) {
        Value item = Value(ctx, JS_GetPropertyUint32(ctx, v.raw(), i));
        if (item.isException()) {
            qjs::clearException(ctx);
            *why = std::string(name) + "[" + std::to_string(i) + "] 读取失败";
            return false;
        }
        std::uint32_t n = 0;
        if (!qjs::toUint32(ctx, item.raw(), &n)) {
            qjs::clearException(ctx);
            *why = std::string(name) + "[" + std::to_string(i) + "] 必须是非负整数";
            return false;
        }
        out->push_back(n);
    }
    return true;
}

/** `handle.id`（句柄函数的公共入口；⛔ 解构后调用会读不到 `this.id`）。 */
bool hookHandleId(JSContext* ctx, JSValueConst this_val, std::uint32_t* out, std::string* why) {
    Value v = qjs::getProperty(ctx, this_val, "id");
    if (v.isException()) {
        qjs::clearException(ctx);
        *why = "句柄没有 id（⛔ 不要解构后再调用，要用 `handle.patch(...)`）";
        return false;
    }
    if (!v.isNumber()) {
        *why = "句柄 id 不是数字";
        return false;
    }
    double d = 0.0;
    if (!qjs::toDouble(ctx, v.raw(), &d) || !std::isfinite(d) || d < 1.0) {
        qjs::clearException(ctx);
        *why = "句柄 id 非法";
        return false;
    }
    *out = static_cast<std::uint32_t>(d);
    return true;
}

/** 造一个句柄对象 `{id, patch, capture, stats, clear}`（`id` 只读）。 */
Value makeHookHandle(JSContext* ctx, std::uint32_t id) {
    Value handle = qjs::newObject(ctx);
    if (handle.isException()) {
        qjs::clearException(ctx);
        return handle;
    }
    if (!setFunction(ctx, handle.raw(), "patch", jsHookPatch, 3, nullptr) ||
        !setFunction(ctx, handle.raw(), "capture", jsHookCapture, 1, nullptr) ||
        !setFunction(ctx, handle.raw(), "stats", jsHookStats, 0, nullptr) ||
        !setFunction(ctx, handle.raw(), "clear", jsHookClear, 0, nullptr)) {
        return handle;
    }
    if (!defineReadOnly(ctx, handle.raw(), "id", qjs::newInt32(ctx, static_cast<std::int32_t>(id)))) {
        return handle;
    }
    return handle;
}

JSValue jsCtxHookAt(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inCallback()) {
            return JS_ThrowTypeError(ctx, "ctx.hook.at: 只用于 Run 回调（start / onTrigger）里");
        }
        if (argc < 2) return JS_ThrowTypeError(ctx, "ctx.hook.at(script, pos)");
        const char* script = JS_ToCString(ctx, argv[0]);
        if (script == nullptr) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.hook.at: script 必须是字符串（如 \"DRAWOBJ.BIN\"）");
        }
        std::string script_s(script);
        JS_FreeCString(ctx, script);
        double pos_d = 0.0;
        if (!qjs::toDouble(ctx, argv[1], &pos_d) || !std::isfinite(pos_d) || pos_d < 1.0 ||
            pos_d > 4294967295.0) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.hook.at: pos 必须是正数（label 坐标）");
        }
        const char* code = nullptr;
        const std::uint32_t handle = host->instrHookAdd(script_s, static_cast<std::uint32_t>(pos_d), &code);
        if (handle == 0) {
            // 失败 ⇒ `undefined`（宿主已打一行 error 点名 code）；⛔ 不编一个假句柄。
            return JS_UNDEFINED;
        }
        Value out = makeHookHandle(ctx, handle);
        if (out.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.hook.at: 句柄对象创建失败");
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.hook.at: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxHookList(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inCallback()) {
            return JS_ThrowTypeError(ctx, "ctx.hook.list: 只用于 Run 回调（start / onTrigger）里");
        }
        return hookStatusArray(ctx, host->instrHookList()).take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.hook.list: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxHookClear(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inCallback()) {
            return JS_ThrowTypeError(ctx, "ctx.hook.clear: 只用于 Run 回调（start / onTrigger）里");
        }
        host->instrHookClear();
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.hook.clear: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsHookPatch(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inCallback()) {
            return JS_ThrowTypeError(ctx, "handle.patch: ctx.hook 的句柄只用于 Run 回调（start / onTrigger）里");
        }
        std::string why;
        std::uint32_t handle = 0;
        if (!hookHandleId(ctx, this_val, &handle, &why)) {
            return JS_ThrowTypeError(ctx, "handle.patch: %s", why.c_str());
        }
        if (argc < 1) return JS_ThrowTypeError(ctx, "handle.patch(target, value, opts?)");
        std::int32_t ptr_local = -1;
        std::int32_t local_int = -1;
        std::int32_t global_id = -1;
        if (!readInt32(ctx, argv[0], "ptrLocal", false, -1, &ptr_local, &why) ||
            !readInt32(ctx, argv[0], "localInt", false, -1, &local_int, &why) ||
            !readInt32(ctx, argv[0], "globalId", false, -1, &global_id, &why)) {
            return JS_ThrowTypeError(ctx, "handle.patch: %s", why.c_str());
        }
        int targets = 0;
        if (ptr_local >= 0) targets += 1;
        if (local_int >= 0) targets += 1;
        if (global_id >= 0) targets += 1;
        lift::InstrPatchKind kind = lift::InstrPatchKind::PtrDword;
        std::uint32_t index = 0;
        bool enabled = true;
        if (targets > 1) {
            return JS_ThrowTypeError(ctx, "handle.patch: ptrLocal / localInt / globalId 只能给一个");
        }
        if (targets == 1) {
            if (ptr_local >= 0) {
                kind = lift::InstrPatchKind::PtrDword;
                index = static_cast<std::uint32_t>(ptr_local);
            } else if (local_int >= 0) {
                kind = lift::InstrPatchKind::LocalInt;
                index = static_cast<std::uint32_t>(local_int);
            } else {
                kind = lift::InstrPatchKind::GlobalInt;
                index = static_cast<std::uint32_t>(global_id);
            }
        } else {
            bool flag = false;
            if (argc < 3 || !readBool(ctx, argv[2], "enabled", false, &flag, &why) || flag) {
                return JS_ThrowTypeError(
                    ctx, "handle.patch: 要给 ptrLocal / localInt / globalId 之一（撤覆盖用 {enabled:false}）");
            }
            enabled = false;
        }
        std::int32_t value = 0;
        std::int32_t max_hits = 0;
        if (enabled) {
            if (argc < 2) return JS_ThrowTypeError(ctx, "handle.patch(target, value, opts?)");
            if (!JS_IsNumber(argv[1])) {
                return JS_ThrowTypeError(ctx, "handle.patch: value 必须是数字");
            }
            double value_d = 0.0;
            if (!qjs::toDouble(ctx, argv[1], &value_d) || !std::isfinite(value_d)) {
                qjs::clearException(ctx);
                return JS_ThrowTypeError(ctx, "handle.patch: value 必须是有限数字");
            }
            value = static_cast<std::int32_t>(value_d);
            if (argc >= 3 && !readInt32(ctx, argv[2], "maxHits", false, 0, &max_hits, &why)) {
                return JS_ThrowTypeError(ctx, "handle.patch: %s", why.c_str());
            }
        }
        const char* code = nullptr;
        const bool ok = host->instrHookSetPatch(handle, enabled, kind, index,
                                                static_cast<std::uint32_t>(value),
                                                max_hits > 0 ? static_cast<std::uint32_t>(max_hits) : 0u, &code);
        Value out = makeReceiptObject(ctx, ok, false, ok, code, 0, 0);
        if (out.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "handle.patch: 回执对象写入失败");
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "handle.patch: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsHookCapture(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inCallback()) {
            return JS_ThrowTypeError(ctx, "handle.capture: ctx.hook 的句柄只用于 Run 回调（start / onTrigger）里");
        }
        std::string why;
        std::uint32_t handle = 0;
        if (!hookHandleId(ctx, this_val, &handle, &why)) {
            return JS_ThrowTypeError(ctx, "handle.capture: %s", why.c_str());
        }
        if (argc < 1) return JS_ThrowTypeError(ctx, "handle.capture({ints?, ptrs?})");
        std::vector<std::uint32_t> ints;
        std::vector<std::uint32_t> ptrs;
        if (!readIndexArray(ctx, argv[0], "ints", &ints, &why) ||
            !readIndexArray(ctx, argv[0], "ptrs", &ptrs, &why)) {
            return JS_ThrowTypeError(ctx, "handle.capture: %s", why.c_str());
        }
        if (!host->instrHookSetCapture(handle, ints, ptrs)) {
            return JS_ThrowTypeError(ctx, "handle.capture: 句柄不存在（已 clear？）");
        }
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "handle.capture: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsHookStats(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inCallback()) {
            return JS_ThrowTypeError(ctx, "handle.stats: ctx.hook 的句柄只用于 Run 回调（start / onTrigger）里");
        }
        std::string why;
        std::uint32_t handle = 0;
        if (!hookHandleId(ctx, this_val, &handle, &why)) {
            return JS_ThrowTypeError(ctx, "handle.stats: %s", why.c_str());
        }
        lift::InstrHookStatus st;
        if (!host->instrHookStats(handle, &st)) {
            return JS_ThrowTypeError(ctx, "handle.stats: 句柄不存在（已 clear？）");
        }
        return hookStatusObject(ctx, st).take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "handle.stats: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsHookClear(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inCallback()) {
            return JS_ThrowTypeError(ctx, "handle.clear: ctx.hook 的句柄只用于 Run 回调（start / onTrigger）里");
        }
        std::string why;
        std::uint32_t handle = 0;
        if (!hookHandleId(ctx, this_val, &handle, &why)) {
            return JS_ThrowTypeError(ctx, "handle.clear: %s", why.c_str());
        }
        host->instrHookRemove(handle);
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "handle.clear: 宿主异常（C++ exception at script boundary）");
    }
}

Value hookStatusObject(JSContext* ctx, const lift::InstrHookStatus& r) {
    Value row = qjs::newObject(ctx);
    bool ok = !row.isException();
    if (ok) ok = defineReadOnly(ctx, row.raw(), "id", qjs::newInt32(ctx, static_cast<std::int32_t>(r.id)));
    if (ok) ok = defineReadOnly(ctx, row.raw(), "script", qjs::newString(ctx, r.script));
    if (ok) ok = defineReadOnly(ctx, row.raw(), "pos", qjs::newInt32(ctx, static_cast<std::int32_t>(r.pos)));
    if (ok) ok = defineReadOnly(ctx, row.raw(), "patch", qjs::newBool(ctx, r.patch));
    if (ok) ok = defineReadOnly(ctx, row.raw(), "hits", qjs::newInt64(ctx, static_cast<std::int64_t>(r.hits)));
    if (ok) ok = defineReadOnly(ctx, row.raw(), "restores",
                                qjs::newInt64(ctx, static_cast<std::int64_t>(r.restores)));
    // ★ S0（缺陷 2）：恢复时"当前值已不是我们写进去的那个"的次数（CAS 拒绝写回的次数）。
    //   名字与 `InstrHookStatus` / d.ts 的 `InstrHookStats` 逐字相同（父级命名：`restore_conflicts`）。
    if (ok) ok = defineReadOnly(ctx, row.raw(), "restore_conflicts",
                                qjs::newInt64(ctx, static_cast<std::int64_t>(r.restore_conflicts)));
    if (ok) ok = defineReadOnly(ctx, row.raw(), "skips", qjs::newInt64(ctx, static_cast<std::int64_t>(r.skips)));
    if (ok) {
        Value ints = newArray(ctx);
        ok = !ints.isException();
        for (std::size_t k = 0; ok && k < r.last_ints.size(); k += 1) {
            ok = JS_SetPropertyUint32(ctx, ints.raw(), static_cast<std::uint32_t>(k),
                                      qjs::newInt32(ctx, static_cast<std::int32_t>(r.last_ints[k])).take()) >= 0;
        }
        if (ok) ok = defineReadOnly(ctx, row.raw(), "ints", std::move(ints));
    }
    if (ok) {
        Value ptrs = newArray(ctx);
        ok = !ptrs.isException();
        for (std::size_t k = 0; ok && k < r.last_ptr_targets.size(); k += 1) {
            ok = JS_SetPropertyUint32(ctx, ptrs.raw(), static_cast<std::uint32_t>(k),
                                      qjs::newInt32(ctx, static_cast<std::int32_t>(r.last_ptr_targets[k])).take()) >= 0;
        }
        if (ok) ok = defineReadOnly(ctx, row.raw(), "ptrTargets", std::move(ptrs));
    }
    if (!ok) {
        qjs::clearException(ctx);
        return newArray(ctx);  // 空数组（调用方只在异常时丢弃）
    }
    return row;
}

Value hookStatusArray(JSContext* ctx, const std::vector<lift::InstrHookStatus>& rows) {
    Value arr = newArray(ctx);
    if (arr.isException()) {
        qjs::clearException(ctx);
        return arr;
    }
    for (std::size_t i = 0; i < rows.size(); i += 1) {
        Value row = hookStatusObject(ctx, rows[i]);
        if (row.isException()) {
            qjs::clearException(ctx);
            return newArray(ctx);
        }
        if (JS_SetPropertyUint32(ctx, arr.raw(), static_cast<std::uint32_t>(i), row.take()) < 0) {
            qjs::clearException(ctx);
            return newArray(ctx);
        }
    }
    return arr;
}

Value makeInputReceipt(JSContext* ctx, const lift::plugin::Receipt& receipt) {    return makeReceiptObject(ctx, receipt.accepted, receipt.accepted, false, receipt.code, receipt.seq,
                             receipt.id);
}

JSValue jsCtxInputClick(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inputWired()) {
            return JS_ThrowTypeError(ctx, "ctx.input.click: 宿主没有接 IntentQueue（⛔ 不静默 no-op）");
        }
        if (argc < 2) return JS_ThrowTypeError(ctx, "ctx.input.click(x, y, button?)");
        double x = 0.0;
        double y = 0.0;
        if (!qjs::toDouble(ctx, argv[0], &x) || !qjs::toDouble(ctx, argv[1], &y) || !std::isfinite(x) ||
            !std::isfinite(y)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.input.click: x / y 必须是有限数字");
        }
        lift::MouseButton button = lift::MouseButton::Left;
        if (argc >= 3 && !JS_IsUndefined(argv[2])) {
            std::string text;
            if (!qjs::toString(ctx, argv[2], &text)) {
                qjs::clearException(ctx);
                return JS_ThrowTypeError(ctx, "ctx.input.click: button 必须是 'left' | 'right' | 'middle'");
            }
            if (text == "left") {
                button = lift::MouseButton::Left;
            } else if (text == "right") {
                button = lift::MouseButton::Right;
            } else if (text == "middle") {
                button = lift::MouseButton::Middle;
            } else {
                return JS_ThrowTypeError(ctx, "ctx.input.click: button 只能是 'left' | 'right' | 'middle'");
            }
        }
        Value out = makeInputReceipt(ctx, host->inputClick(static_cast<int>(x), static_cast<int>(y), button));
        if (out.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.input.click: 回执对象写入失败");
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.input.click: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxInputMove(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inputWired()) {
            return JS_ThrowTypeError(ctx, "ctx.input.move: 宿主没有接 IntentQueue（⛔ 不静默 no-op）");
        }
        if (argc < 2) return JS_ThrowTypeError(ctx, "ctx.input.move(x, y)");
        double x = 0.0;
        double y = 0.0;
        if (!qjs::toDouble(ctx, argv[0], &x) || !qjs::toDouble(ctx, argv[1], &y) || !std::isfinite(x) ||
            !std::isfinite(y)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.input.move: x / y 必须是有限数字");
        }
        Value out = makeInputReceipt(ctx, host->inputMove(static_cast<int>(x), static_cast<int>(y)));
        if (out.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.input.move: 回执对象写入失败");
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.input.move: 宿主异常（C++ exception at script boundary）");
    }
}

bool parseButtonArg(JSContext* ctx, JSValueConst value, lift::MouseButton* out) {
    if (JS_IsUndefined(value)) return false;
    std::string text;
    if (!qjs::toString(ctx, value, &text)) {
        qjs::clearException(ctx);
        return false;
    }
    if (text == "left") {
        *out = lift::MouseButton::Left;
    } else if (text == "right") {
        *out = lift::MouseButton::Right;
    } else if (text == "middle") {
        *out = lift::MouseButton::Middle;
    } else {
        return false;
    }
    return true;
}

JSValue jsCtxInputDown(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inputWired()) {
            return JS_ThrowTypeError(ctx, "ctx.input.down: 宿主没有接 IntentQueue（⛔ 不静默 no-op）");
        }
        lift::MouseButton button = lift::MouseButton::Left;
        if (argc < 1 || !parseButtonArg(ctx, argv[0], &button)) {
            return JS_ThrowTypeError(ctx, "ctx.input.down(button)：button 只能是 'left' | 'right' | 'middle'");
        }
        Value out = makeInputReceipt(ctx, host->inputDown(button));
        if (out.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.input.down: 回执对象写入失败");
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.input.down: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxInputUp(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inputWired()) {
            return JS_ThrowTypeError(ctx, "ctx.input.up: 宿主没有接 IntentQueue（⛔ 不静默 no-op）");
        }
        lift::MouseButton button = lift::MouseButton::Left;
        if (argc < 1 || !parseButtonArg(ctx, argv[0], &button)) {
            return JS_ThrowTypeError(ctx, "ctx.input.up(button)：button 只能是 'left' | 'right' | 'middle'");
        }
        Value out = makeInputReceipt(ctx, host->inputUp(button));
        if (out.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.input.up: 回执对象写入失败");
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.input.up: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxInputStatus(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inputWired()) {
            return JS_ThrowTypeError(ctx, "ctx.input.status: 宿主没有接 IntentQueue（⛔ 不静默 no-op）");
        }
        if (argc < 1) return JS_ThrowTypeError(ctx, "ctx.input.status(id)");
        double d = 0.0;
        if (!qjs::toDouble(ctx, argv[0], &d) || !std::isfinite(d) || d < 0.0) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.input.status: id 必须是非负数字");
        }
        const auto state = host->inputStatus(static_cast<std::uint64_t>(d));
        return qjs::newString(ctx, lift::ui::pluginInputStateName(state)).take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.input.status: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxReadGlobalGet(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->readWired()) {
            return JS_ThrowTypeError(ctx, "ctx.read.global.get: 宿主没有接 EngineRead（⛔ 不静默 no-op）");
        }
        if (argc < 2) return JS_ThrowTypeError(ctx, "ctx.read.global.get(kind, id)");
        lift::GlobalKind kind = lift::GlobalKind::Int;
        std::string why;
        if (!parseGlobalKindArg(ctx, argv[0], &kind, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.read.global.get: %s", why.c_str());
        }
        double d = 0.0;
        if (!qjs::toDouble(ctx, argv[1], &d) || !std::isfinite(d) || d < 0.0 || d > 4294967295.0) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.read.global.get: id 必须是 0..0xFFFFFFFF 的数字");
        }
        lift::GlobalValue value;
        const char* code = nullptr;
        if (!host->readGlobal(kind, static_cast<std::uint32_t>(d), &value, &code)) {
            // ★ 读失败**响亮**（d.ts 没有错误通道 ⇒ TypeError 是唯一诚实的出口；⛔ 不返回 0 冒充读数）。
            return JS_ThrowTypeError(ctx, "ctx.read.global.get: 读失败（code=%s）",
                                     code != nullptr ? code : "unknown");
        }
        if (kind == lift::GlobalKind::Float) return qjs::newDouble(ctx, value.f64).take();
        return qjs::newInt64(ctx, static_cast<std::int64_t>(value.u32)).take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.read.global.get: 宿主异常（C++ exception at script boundary）");
    }
}

// trigger 工厂：宿主返回 `Trigger`（新 state）⇒ 包成本 runtime 的 JS 对象。
// ★ 返回**裸 JSValue**（不是 `qjs::Value`）：失败路径要直接 `JS_ThrowTypeError(...)`（它返回
//   `JS_EXCEPTION`），而 `Value` 没有从 `JSValue` 的隐式构造 —— 那会让这两条路径各写一份包装。
JSValue finishTrigger(Impl& impl, const lift::plugin::Trigger& trigger, const char* fn,
                      const std::string& why) {
    JSContext* ctx = impl.ctx;
    if (!why.empty()) return JS_ThrowTypeError(ctx, "%s: %s", fn, why.c_str());
    if (trigger.state == nullptr) {
        return JS_ThrowTypeError(ctx, "%s: 宿主没有接 trigger 工厂（⛔ 不静默 no-op）", fn);
    }
    Value obj = wrapTrigger(impl, trigger);
    if (obj.isException()) {
        qjs::clearException(ctx);
        return JS_ThrowTypeError(ctx, "%s: trigger 对象分配失败", fn);
    }
    return obj.take();
}

JSValue jsCtxTriggerAfterScript(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (impl == nullptr || host == nullptr || !host->triggerWired()) {
            return JS_ThrowTypeError(ctx, "ctx.trigger.afterScript: 宿主没有接 TriggerFactory");
        }
        if (argc < 2) return JS_ThrowTypeError(ctx, "ctx.trigger.afterScript(script, opts)");
        std::string script;
        if (!qjs::toString(ctx, argv[0], &script)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.trigger.afterScript: script 必须是字符串");
        }
        std::string name;
        std::string why;
        lift::plugin::TriggerMode mode = lift::plugin::TriggerMode::Once;
        std::uint32_t max_fires = 0;
        std::uint64_t timeout_ms = 0;
        if (!readTriggerOptions(ctx, argv[1], "ctx.trigger.afterScript", &name, &mode, &max_fires, &timeout_ms,
                                &why)) {
            return JS_ThrowTypeError(ctx, "ctx.trigger.afterScript: %s", why.c_str());
        }
        const lift::plugin::Trigger trigger =
            host->makeAfterScript(name, script, mode, max_fires, timeout_ms);
        return finishTrigger(*impl, trigger, "ctx.trigger.afterScript", std::string());
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.trigger.afterScript: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxTriggerAfterPosition(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (impl == nullptr || host == nullptr || !host->triggerWired()) {
            return JS_ThrowTypeError(ctx, "ctx.trigger.afterPosition: 宿主没有接 TriggerFactory");
        }
        if (argc < 4) return JS_ThrowTypeError(ctx, "ctx.trigger.afterPosition(script, lo, hi, opts)");
        std::string script;
        if (!qjs::toString(ctx, argv[0], &script)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.trigger.afterPosition: script 必须是字符串");
        }
        double lo = 0.0;
        double hi = 0.0;
        if (!qjs::toDouble(ctx, argv[1], &lo) || !qjs::toDouble(ctx, argv[2], &hi) || !std::isfinite(lo) ||
            !std::isfinite(hi) || lo < 0.0 || hi < 0.0 || lo > 4294967295.0 || hi > 4294967295.0) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.trigger.afterPosition: lo / hi 必须是 0..0xFFFFFFFF 的数字");
        }
        std::string name;
        std::string why;
        lift::plugin::TriggerMode mode = lift::plugin::TriggerMode::Once;
        std::uint32_t max_fires = 0;
        std::uint64_t timeout_ms = 0;
        if (!readTriggerOptions(ctx, argv[3], "ctx.trigger.afterPosition", &name, &mode, &max_fires,
                                &timeout_ms, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.trigger.afterPosition: %s", why.c_str());
        }
        const lift::plugin::Trigger trigger = host->makeAfterPosition(
            name, script, static_cast<std::uint32_t>(lo), static_cast<std::uint32_t>(hi), mode, max_fires,
            timeout_ms);
        return finishTrigger(*impl, trigger, "ctx.trigger.afterPosition", std::string());
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.trigger.afterPosition: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxTriggerAfterDelay(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (impl == nullptr || host == nullptr || !host->triggerWired()) {
            return JS_ThrowTypeError(ctx, "ctx.trigger.afterDelay: 宿主没有接 TriggerFactory");
        }
        if (argc < 2) return JS_ThrowTypeError(ctx, "ctx.trigger.afterDelay(ms, opts)");
        double ms = 0.0;
        if (!qjs::toDouble(ctx, argv[0], &ms) || !std::isfinite(ms) || ms < 0.0) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.trigger.afterDelay: ms 必须是非负有限数字");
        }
        std::string name;
        std::string why;
        lift::plugin::TriggerMode mode = lift::plugin::TriggerMode::Once;
        std::uint32_t max_fires = 0;
        std::uint64_t timeout_ms = 0;
        if (!readTriggerOptions(ctx, argv[1], "ctx.trigger.afterDelay", &name, &mode, &max_fires, &timeout_ms,
                                &why)) {
            return JS_ThrowTypeError(ctx, "ctx.trigger.afterDelay: %s", why.c_str());
        }
        // ★ `afterDelay` **不接受** `timeoutMs`（合同 §4.2 441：延迟本身就是时间量）。
        if (timeout_ms != 0) {
            return JS_ThrowTypeError(
                ctx, "ctx.trigger.afterDelay: 不接受 timeoutMs（延迟本身已是时间量；要超时就用别的 trigger）");
        }
        const lift::plugin::Trigger trigger =
            host->makeAfterDelay(name, static_cast<std::uint64_t>(ms), mode);
        return finishTrigger(*impl, trigger, "ctx.trigger.afterDelay", std::string());
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.trigger.afterDelay: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxTriggerAtReadPoint(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (impl == nullptr || host == nullptr || !host->triggerWired()) {
            return JS_ThrowTypeError(ctx, "ctx.trigger.atReadPoint: 宿主没有接 TriggerFactory");
        }
        if (argc < 3) return JS_ThrowTypeError(ctx, "ctx.trigger.atReadPoint(point, maxFires, opts)");
        lift::InputPoint point = lift::InputPoint::Count;
        std::string why;
        if (!parsePluginPoint(ctx, argv[0], &point, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.trigger.atReadPoint: %s", why.c_str());
        }
        double max_fires = 0.0;
        if (!qjs::toDouble(ctx, argv[1], &max_fires) || !std::isfinite(max_fires) || max_fires < 1.0) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.trigger.atReadPoint: maxFires 必须是 >= 1 的数字");
        }
        std::string name;
        lift::plugin::TriggerMode mode = lift::plugin::TriggerMode::Once;
        std::uint32_t opts_max_fires = 0;
        std::uint64_t timeout_ms = 0;
        if (!readTriggerOptions(ctx, argv[2], "ctx.trigger.atReadPoint", &name, &mode, &opts_max_fires,
                                &timeout_ms, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.trigger.atReadPoint: %s", why.c_str());
        }
        const lift::plugin::Trigger trigger =
            host->makeAtReadPoint(name, point, static_cast<std::uint32_t>(max_fires), mode, timeout_ms);
        return finishTrigger(*impl, trigger, "ctx.trigger.atReadPoint", std::string());
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.trigger.atReadPoint: 宿主异常（C++ exception at script boundary）");
    }
}

// ---- ctx.ui（宿主本地 JS 状态；C2a 只建写入面）----
bool uiIdArg(JSContext* ctx, JSValueConst value, std::string* out) {
    if (!qjs::toString(ctx, value, out)) {
        qjs::clearException(ctx);
        return false;
    }
    return !out->empty();
}

JSValue jsCtxUiShow(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || argc < 1) return JS_ThrowTypeError(ctx, "ctx.ui.show(id, data?)");
        std::string id;
        if (!uiIdArg(ctx, argv[0], &id)) return JS_ThrowTypeError(ctx, "ctx.ui.show: id 必须是非空字符串");
        impl->ui_visible.insert(id);
        if (argc >= 2 && !JS_IsUndefined(argv[1])) {
            if (JS_SetPropertyStr(ctx, impl->ui_data.raw(), id.c_str(), JS_DupValue(ctx, argv[1])) < 0) {
                qjs::clearException(ctx);
                return JS_ThrowTypeError(ctx, "ctx.ui.show: 写状态失败");
            }
        }
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.ui.show: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxUiHide(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || argc < 1) return JS_ThrowTypeError(ctx, "ctx.ui.hide(id)");
        std::string id;
        if (!uiIdArg(ctx, argv[0], &id)) return JS_ThrowTypeError(ctx, "ctx.ui.hide: id 必须是非空字符串");
        impl->ui_visible.erase(id);
        const JSAtom atom = JS_NewAtom(ctx, id.c_str());
        if (atom == JS_ATOM_NULL) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.ui.hide: 属性名创建失败");
        }
        const int rc = JS_DeleteProperty(ctx, impl->ui_data.raw(), atom, 0);
        JS_FreeAtom(ctx, atom);
        if (rc < 0) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.ui.hide: 删除状态失败");
        }
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.ui.hide: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxUiSet(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || argc < 2) return JS_ThrowTypeError(ctx, "ctx.ui.set(id, data)");
        std::string id;
        if (!uiIdArg(ctx, argv[0], &id)) return JS_ThrowTypeError(ctx, "ctx.ui.set: id 必须是非空字符串");
        if (JS_SetPropertyStr(ctx, impl->ui_data.raw(), id.c_str(), JS_DupValue(ctx, argv[1])) < 0) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.ui.set: 写状态失败");
        }
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.ui.set: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxUiVisible(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || argc < 1) return JS_ThrowTypeError(ctx, "ctx.ui.visible(id)");
        std::string id;
        if (!uiIdArg(ctx, argv[0], &id)) return JS_ThrowTypeError(ctx, "ctx.ui.visible: id 必须是非空字符串");
        return qjs::newBool(ctx, impl->ui_visible.count(id) != 0).take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.ui.visible: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxUiGet(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || argc < 1) return JS_ThrowTypeError(ctx, "ctx.ui.get(id)");
        std::string id;
        if (!uiIdArg(ctx, argv[0], &id)) return JS_ThrowTypeError(ctx, "ctx.ui.get: id 必须是非空字符串");
        // ★ 不存在 ⇒ `undefined`（d.ts 的 `T | undefined`；⛔ 不编 `null`）。
        return JS_GetPropertyStr(ctx, impl->ui_data.raw(), id.c_str());
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.ui.get: 宿主异常（C++ exception at script boundary）");
    }
}

// ---- ctx.log / profile / fail / finish / quit ----
JSValue jsCtxLog(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr) return JS_ThrowTypeError(ctx, "ctx.log: 不在插件回调里");
        if (argc < 2) return JS_ThrowTypeError(ctx, "ctx.log(level, message, data?)");
        std::string level;
        std::string message;
        if (!qjs::toString(ctx, argv[0], &level) || !qjs::toString(ctx, argv[1], &message)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.log: level / message 必须能转成字符串");
        }
        // ★ `data`（第三个参数）**不进日志行**：一行一条是硬要求（合同 §4.10）；结构化数据由插件
        //   自己决定要不要先 `String(...)`。⇒ 这里只记 message（有界 + 去重由宿主做）。
        host->log(level.c_str(), message);
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.log: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxProfileBegin(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        // ★ C2b 复审 F2：`profile` 是 **Run 回调**的出口（RenderCtx 有自己的一份
        //   `jsRenderProfileBegin`）⇒ 判"正在回调"，
        //   ⛔ 只判 `host != nullptr`（render 期 host 也非空，那样"存下来的 RunCtx"就能用）。
        if (host == nullptr || !host->inCallback()) {
            return JS_ThrowTypeError(ctx, "ctx.profile.begin: 不在插件回调里");
        }
        std::string label;
        if (argc >= 1 && !JS_IsUndefined(argv[0]) && !qjs::toString(ctx, argv[0], &label)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.profile.begin: label 必须能转成字符串");
        }
        std::string why;
        if (!host->profileBegin(label, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.profile.begin: %s", why.c_str());
        }
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.profile.begin: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxProfileEnd(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inCallback()) {
            return JS_ThrowTypeError(ctx, "ctx.profile.end: 不在插件回调里");
        }
        std::string why;
        // ★ **宿主调用暂停**（`REQ-01M86VRT1YP53CJS0M885FCCJM`）：`profile.end` 是**宿主能力** ——
        //   它在这里生成窗口报告文本并写系统剪贴板（`AotHost::endWindowText`），实测 2.5–2.9 ms CPU
        //   ⇒ 不算用户 JS 的 2 ms（`runtime/budget.h` 头注的同一原则）。RAII：异常路径也减。
        bool ok = false;
        {
            HostPause pause(implOf(ctx));
            ok = host->profileEnd(&why);
        }
        if (!ok) return JS_ThrowTypeError(ctx, "ctx.profile.end: %s", why.c_str());
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.profile.end: 宿主异常（C++ exception at script boundary）");
    }
}

// =======================================================================================
// S2 · `RenderCtx.profile`（`ctx.profile.begin/end` 在 **render** 里）
// ★ `begin(label?)` 是 `void`（失败 = TypeError，与 RunCtx 同形）；**`end()` 返回结果对象**
//   `{ok, text, error}`（与旧 UiScript 的 `host.profileEnd()` 同形）—— S2 复审 I3：HUD 要判报告
//   文本里的 `INVALID` token ⇒ 文本必须回 JS，⛔ 不能只写 stdout。
// ★ ⛔ 不复用 `jsCtxProfile*`：那两个靠 `inCallback()` 区分"存下来的 RunCtx"，放宽会破
//   `plugin/render-stored-runctx-rejected`。这里判 `impl.current != nullptr`（render 的判据）。
// ★ `end` 同样走 `HostPause`（宿主的报告 / 剪贴板成本不算用户 JS；同一口径）。
// =======================================================================================
JSValue jsRenderProfileBegin(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->current == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.profile.begin: 只能在 render() 内调用");
        }
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.profile.begin: 宿主没有接插件回调桥（⛔ 不静默 no-op）");
        }
        std::string label;
        if (argc >= 1 && !JS_IsUndefined(argv[0]) && !qjs::toString(ctx, argv[0], &label)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.profile.begin: label 必须能转成字符串");
        }
        std::string why;
        if (!host->profileBegin(label, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.profile.begin: %s", why.c_str());
        }
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.profile.begin: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsRenderProfileEnd(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->current == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.profile.end: 只能在 render() 内调用");
        }
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.profile.end: 宿主没有接插件回调桥（⛔ 不静默 no-op）");
        }
        std::string text;
        std::string why;
        bool ok = false;
        {
            HostPause pause(impl);
            // ★ S2 复审 I3：**不打印**的报告版本 ⇒ 文本回给 HUD（判 `INVALID` 那一态）。
            ok = host->profileEndText(&text, &why);
        }
        // ★ 与旧 UiScript 的 `host.profileEnd()` **同形**：`{ok,text,error}`（失败也是结果对象，
        //   ⛔ 不抛 —— "没有打开的窗口"是用户可见的正常状态；HUD 要画"失败"并继续）。
        Value out = makeProfileEndResult(ctx, ok, text, why);
        if (out.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.profile.end: 结果对象写入失败");
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.profile.end: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxFail(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inCallback()) {
            return JS_ThrowTypeError(ctx, "ctx.fail: 不在插件回调里");
        }
        std::string name;
        std::string reason;
        if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]) &&
            !qjs::toString(ctx, argv[0], &name)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.fail: name 必须能转成字符串");
        }
        if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) &&
            !qjs::toString(ctx, argv[1], &reason)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.fail: reason 必须能转成字符串");
        }
        host->fail(name, reason);
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.fail: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxFinish(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inCallback()) {
            return JS_ThrowTypeError(ctx, "ctx.finish: 不在插件回调里");
        }
        if (argc < 1) return JS_ThrowTypeError(ctx, "ctx.finish(exitCode, reason?)");
        double code = 0.0;
        if (!qjs::toDouble(ctx, argv[0], &code) || !std::isfinite(code)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.finish: exitCode 必须是有限数字");
        }
        std::string reason;
        if (argc >= 2 && !JS_IsUndefined(argv[1]) && !JS_IsNull(argv[1]) &&
            !qjs::toString(ctx, argv[1], &reason)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.finish: reason 必须能转成字符串");
        }
        host->finish(static_cast<int>(code), reason);
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.finish: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxQuit(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inCallback()) {
            return JS_ThrowTypeError(ctx, "ctx.quit: 不在插件回调里");
        }
        std::string reason;
        if (argc >= 1 && !JS_IsUndefined(argv[0]) && !JS_IsNull(argv[0]) &&
            !qjs::toString(ctx, argv[0], &reason)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.quit: reason 必须能转成字符串");
        }
        host->quit(reason);
        return JS_UNDEFINED;
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.quit: 宿主异常（C++ exception at script boundary）");
    }
}

// ★ `ctx.appdata.stat(rel)`：同步返回结果对象（`AppDataStat`）；`rel` 不是字符串 ⇒ TypeError。
//   ★ **宿主段包 `HostPause`**：stat 是宿主 I/O（网络盘 / 杀软可能 ms 级），⛔ 不算用户 JS 的 2 ms
//     （与 `ctx.profile.end` 同一原则）。RAII ⇒ trampoline 的异常路径也减。
//   ★ `exists / isFile / isDir / size` 是**可选字段**：只有 `exists=true` 才给 isFile/isDir；
//     `size` 只在 isFile 且 file_size 成功时给；`error` 只在非 None 时给（⛔ 不把 errno 漏出去）。
JSValue jsCtxAppDataStat(JSContext* ctx, JSValueConst /*this_val*/, int argc, JSValueConst* argv) {
    try {
        lift::ui::PluginCallHost* host = callHost(ctx);
        if (host == nullptr || !host->inCallback()) {
            return JS_ThrowTypeError(ctx, "ctx.appdata.stat: 不在插件回调里");
        }
        if (argc < 1 || !JS_IsString(argv[0])) {
            return JS_ThrowTypeError(ctx, "ctx.appdata.stat(rel): rel 必须是字符串（相对 appdata 根）");
        }
        std::string rel;
        if (!qjs::toString(ctx, argv[0], &rel)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.appdata.stat: rel 读取失败");
        }
        lift::plugin::AppDataStat st;
        {
            HostPause pause(implOf(ctx));
            st = host->appDataStat(rel);
        }
        Value out = qjs::newObject(ctx);
        if (out.isException()) return out.take();
        bool ok = defineReadOnly(ctx, out.raw(), "exists", qjs::newBool(ctx, st.exists));
        if (ok && st.exists) {
            ok = defineReadOnly(ctx, out.raw(), "isFile", qjs::newBool(ctx, st.is_file));
            if (ok) ok = defineReadOnly(ctx, out.raw(), "isDir", qjs::newBool(ctx, st.is_dir));
            if (ok && st.is_file && st.has_size) {
                ok = defineReadOnly(ctx, out.raw(), "size", qjs::newDouble(ctx, static_cast<double>(st.size)));
            }
        }
        if (ok && st.error != lift::plugin::AppDataStatError::None) {
            ok = defineReadOnly(ctx, out.raw(), "error",
                                qjs::newString(ctx, lift::plugin::appDataStatErrorName(st.error)));
        }
        if (!ok) {
            qjs::clearException(ctx);
            return JS_EXCEPTION;
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.appdata.stat: 宿主异常（C++ exception at script boundary）");
    }
}

// =======================================================================================
// `ctx.config`（2026-11-25）：域式 JSON 配置（`<--appdata>/plugin-config.json`）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.5 / §4.6 / §6 行 41–42；值类型与上限在
//       `runtime/config.h`（语言无关），宿主实现 = `src/plugin/plugin_config.cpp`（PIMPL）。
//
// ★ `ctx.config` 的宿主实现只有一份：RunCtx（`inCallback()`）与两条渲染面（`host.config` /
//   `RenderCtx.config`，`__render` + current/builder）的门都在 `configCall`。
//   render 可经宿主桥写 `plugin-config.json`，仍⛔ 不做 guest 写 / 任意路径 I/O
//   （`ctx.appdata` 仍只 RunCtx）。
// ★ 为什么值走**中立树**而不是 JSON 字符串：沙箱里没有 `JSON`（`JS_NewContextRaw` + BaseObjects），
//   而且"原生 JS 值"是 d.ts 的合同；转换只在这里（nlohmann 在 plugin_config.cpp 的 PIMPL 里）。
// ★ 每个宿主调用都包 `HostPause`（同 `ctx.appdata.stat`）：宿主段不计用户 2 ms。
// =======================================================================================

// `ConfigValue` → JS 原生值（对象成员用 **DefineProperty** 装：`__proto__` 只当普通数据键，
// ⛔ 不走 `[[Set]]` 的原型 setter）。★ `depth` 是**防御性护栏**：`attach` 已按同一上限拒绝深层
// 文件；这里再挡一次，⛔ 不让宿主递归爆栈（B1）。
Value configValueToJs(JSContext* ctx, const lift::plugin::ConfigValue& value, std::size_t depth = 1) {
    using lift::plugin::ConfigValueKind;
    if (depth > lift::plugin::kConfigMaxDepth) {
        JS_ThrowInternalError(ctx, "ctx.config: 值嵌套超过上限（%zu 层）",
                              lift::plugin::kConfigMaxDepth);
        return Value::exception(ctx);
    }
    switch (value.kind) {
        case ConfigValueKind::Null:
            return Value::null(ctx);
        case ConfigValueKind::Bool:
            return qjs::newBool(ctx, value.boolean);
        case ConfigValueKind::Number:
            return qjs::newDouble(ctx, value.number);
        case ConfigValueKind::String:
            return qjs::newString(ctx, value.text);
        case ConfigValueKind::Array: {
            Value out(ctx, JS_NewArray(ctx));
            if (out.isException()) return out;
            for (std::size_t i = 0; i < value.items.size(); ++i) {
                Value item = configValueToJs(ctx, value.items[i], depth + 1);
                if (item.isException()) return item;
                // ★ 快照 = **普通可写对象**（`all()` 的深拷贝；改它不影响 store，但也不是冻结值）。
                if (JS_DefinePropertyValueUint32(
                        ctx, out.raw(), static_cast<std::uint32_t>(i), item.take(),
                        JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
                    qjs::clearException(ctx);
                    return Value::exception(ctx);
                }
            }
            return out;
        }
        case ConfigValueKind::Object: {
            Value out = qjs::newObject(ctx);
            if (out.isException()) return out;
            for (const auto& member : value.members) {
                Value item = configValueToJs(ctx, member.second, depth + 1);
                if (item.isException()) return item;
                if (JS_DefinePropertyValueStr(
                        ctx, out.raw(), member.first.c_str(), item.take(),
                        JS_PROP_ENUMERABLE | JS_PROP_WRITABLE | JS_PROP_CONFIGURABLE) < 0) {
                    qjs::clearException(ctx);
                    return Value::exception(ctx);
                }
            }
            return out;
        }
    }
    return Value::null(ctx);
}

// JS 原生值 → `ConfigValue`。false ⇒ `*why` 是原因（调用方转 TypeError）。
// ★ 只收 d.ts 的六种；`undefined` / 函数 / symbol / bigint 一律**响亮拒绝**（⛔ 不静默丢）。
// ★ `pause`（可空）：枚举 / 建树是**宿主 CPU** ⇒ pause；`JS_GetProperty*` 可能执行用户 getter
//   ⇒ 那几处 `stop()`，让 getter 的 CPU 仍算用户时间（I3）。
// ★ `depth` 从 1（根值）起，节点深度 > `kConfigMaxDepth` ⇒ 拒绝（与 `PluginConfig::set` 的
//   `validateValue` 同一句话；⛔ 不要一边 `>` 一边 `>=`）。
bool jsToConfigValue(JSContext* ctx, JSValueConst value, std::size_t depth, std::size_t* nodes,
                     HostPause* pause, lift::plugin::ConfigValue* out, std::string* why) {
    if (depth > lift::plugin::kConfigMaxDepth) {
        *why = "值嵌套太深（上限 " + std::to_string(lift::plugin::kConfigMaxDepth) + " 层）";
        return false;
    }
    if (++(*nodes) > lift::plugin::kConfigMaxNodes) {
        *why = "值太大（节点上限 " + std::to_string(lift::plugin::kConfigMaxNodes) + "）";
        return false;
    }
    if (JS_IsNull(value)) {
        *out = lift::plugin::ConfigValue::null();
        return true;
    }
    if (JS_IsBool(value)) {
        bool b = false;
        if (!qjs::toBool(ctx, value, &b)) {
            qjs::clearException(ctx);
            *why = "布尔值读取失败";
            return false;
        }
        *out = lift::plugin::ConfigValue::fromBool(b);
        return true;
    }
    if (JS_IsNumber(value)) {
        double d = 0.0;
        if (!qjs::toDouble(ctx, value, &d)) {
            qjs::clearException(ctx);
            *why = "数字读取失败";
            return false;
        }
        if (!std::isfinite(d)) {
            *why = "数字必须是有限的（NaN / Infinity 不支持）";
            return false;
        }
        *out = lift::plugin::ConfigValue::fromNumber(d);
        return true;
    }
    if (JS_IsString(value)) {
        std::string text;
        if (!qjs::toString(ctx, value, &text)) {
            qjs::clearException(ctx);
            *why = "字符串读取失败";
            return false;
        }
        *out = lift::plugin::ConfigValue::fromString(std::move(text));
        return true;
    }
    if (JS_IsFunction(ctx, value)) {
        *why = "值不能是函数";
        return false;
    }
    if (JS_IsArray(value)) {
        Value length = qjs::getProperty(ctx, value, "length");
        std::uint32_t count = 0;
        if (length.isException() || !length.isNumber() ||
            !qjs::toUint32(ctx, length.raw(), &count)) {
            qjs::clearException(ctx);
            *why = "数组长度读取失败";
            return false;
        }
        lift::plugin::ConfigValue arr = lift::plugin::ConfigValue::array();
        for (std::uint32_t i = 0; i < count; ++i) {
            // ★ 元素读可能触发 getter / 访问器 ⇒ 这段算**用户 JS**。
            if (pause != nullptr) pause->stop();
            Value item(ctx, JS_GetPropertyUint32(ctx, value, i));
            if (pause != nullptr) pause->resume();
            if (item.isException()) {
                qjs::clearException(ctx);
                *why = "数组元素读取失败";
                return false;
            }
            lift::plugin::ConfigValue child;
            if (!jsToConfigValue(ctx, item.raw(), depth + 1, nodes, pause, &child, why)) return false;
            arr.items.push_back(std::move(child));
        }
        *out = std::move(arr);
        return true;
    }
    if (JS_IsObject(value)) {
        JSPropertyEnum* table = nullptr;
        std::uint32_t count = 0;
        if (JS_GetOwnPropertyNames(ctx, &table, &count, value,
                                   JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) {
            qjs::clearException(ctx);
            *why = "对象属性读取失败";
            return false;
        }
        lift::plugin::ConfigValue object = lift::plugin::ConfigValue::object();
        bool ok = true;
        for (std::uint32_t i = 0; i < count; ++i) {
            const char* raw_key = JS_AtomToCString(ctx, table[i].atom);
            if (raw_key == nullptr) {
                qjs::clearException(ctx);
                *why = "对象 key 读取失败";
                ok = false;
                break;
            }
            std::string key(raw_key);
            JS_FreeCString(ctx, raw_key);
            // ★ 同数组：属性值读可能跑用户 getter。
            if (pause != nullptr) pause->stop();
            Value item(ctx, JS_GetPropertyStr(ctx, value, key.c_str()));
            if (pause != nullptr) pause->resume();
            if (item.isException()) {
                qjs::clearException(ctx);
                *why = "对象属性读取失败";
                ok = false;
                break;
            }
            lift::plugin::ConfigValue child;
            if (!jsToConfigValue(ctx, item.raw(), depth + 1, nodes, pause, &child, why)) {
                ok = false;
                break;
            }
            object.members.emplace_back(std::move(key), std::move(child));
        }
        JS_FreePropertyEnum(ctx, table, count);
        if (!ok) return false;
        *out = std::move(object);
        return true;
    }
    *why = "值只能是 null / bool / 有限数字 / 字符串 / 数组 / 对象";
    return false;
}

// 抛一个**真正的 `Error`**（`ctx.config.domain()` 在 `available=false` 时是 `Error("no_appdata")`，
// ⛔ 不是 TypeError —— d.ts / 文档的口径就是这一句）。构造失败时退回 InternalError（仍是 throw）。
JSValue throwPlainError(JSContext* ctx, const std::string& message) {
    Value global = qjs::getGlobalObject(ctx);
    if (global.isException()) {
        qjs::clearException(ctx);
        return JS_ThrowInternalError(ctx, "%s", message.c_str());
    }
    Value ctor = qjs::getProperty(ctx, global.raw(), "Error");
    Value text = qjs::newString(ctx, message);
    if (ctor.isException() || text.isException() || !ctor.isFunction()) {
        qjs::clearException(ctx);
        return JS_ThrowInternalError(ctx, "%s", message.c_str());
    }
    JSValueConst argv[1] = {text.raw()};
    Value error(ctx, JS_Call(ctx, ctor.raw(), JS_UNDEFINED, 1, argv));
    if (error.isException()) {
        qjs::clearException(ctx);
        return JS_ThrowInternalError(ctx, "%s", message.c_str());
    }
    return JS_Throw(ctx, error.take());
}

// `domain` 对象把域 id 藏在**不可枚举**的 `__domain` 属性里（每次 `domain(id)` 新建一个对象；
// 同一个 store 共享）。⛔ 不用 `JS_NewCFunctionData`：它没有公开的 data 读口。
constexpr const char* kConfigDomainProp = "__domain";
// ★ D2：渲染面的不可枚举标记（RunCtx 的对象**没有**它）。经典 `host.config` 与
//   `RenderCtx.config` 的 config / domain 对象都带 `__render` ⇒ trampoline 走"正在 render"那条门；
//   RunCtx 的走 `inCallback()`。⛔ 不做帧令牌：经典 `host` 是**全局对象**（跨帧合法），
//   而 `__render` 挡的是"把对象存下来、离开 render 再调"（两条渲染面同一口径）。
constexpr const char* kConfigRenderProp = "__render";

// 在 config / domain 对象上装渲染标记（不可枚举、不可写、不可配置）。
bool markConfigObject(JSContext* ctx, JSValueConst obj, bool render) {
    if (!render) return true;
    if (JS_DefinePropertyValueStr(ctx, obj, kConfigRenderProp, JS_NewBool(ctx, true), 0) < 0) {
        qjs::clearException(ctx);
        return false;
    }
    return true;
}

// 本调用的桥：render 面（经典 `host.config`）= `UiHostOps::config`；RunCtx = `PluginCallHost::config()`。
// ★ 两个面都**只**经这一个函数取桥 ⇒ ⛔ 不出现第二份"从哪拿 config"的分支。
lift::plugin::ConfigBridge* configBridgeFor(const Impl& impl, bool render) {
    if (render) {
        return impl.current != nullptr && impl.current->host != nullptr ? impl.current->host->config
                                                                       : nullptr;
    }
    return impl.call_host != nullptr ? impl.call_host->config() : nullptr;
}

// 本调用的桥 + **门**（一份实现体，两条门）：
//   · render 面（对象带 `__render`）：必须正在 render（`current` / `builder` 非空）；
//   · Run 面（无标记）：必须正在 Run 回调（`host->inCallback()`）—— 存下来的 RunCtx 在 render
//     里调用、或存下来的 `host.config` domain 在回调外调用，都走这条 ⇒ TypeError。
// 返回 nullptr ⇒ `*why` 是原因（调用方转 TypeError）。
lift::plugin::ConfigBridge* configCall(JSContext* ctx, JSValueConst this_val, std::string* why,
                                       bool* render_face) {
    if (render_face != nullptr) *render_face = false;
    Impl* impl = implOf(ctx);
    if (impl == nullptr) {
        *why = "脚本引擎上下文不可用";
        return nullptr;
    }
    bool render_marked = false;
    if (JS_IsObject(this_val)) {
        Value flag = qjs::getProperty(ctx, this_val, kConfigRenderProp);
        if (flag.isException()) {
            qjs::clearException(ctx);
        } else if (!flag.isUndefined()) {
            render_marked = true;
        }
    }
    if (render_marked) {
        if (impl->current == nullptr || impl->builder == nullptr) {
            *why = "只能在 render() 内调用（这个对象是渲染面的）";
            return nullptr;
        }
        lift::plugin::ConfigBridge* bridge = configBridgeFor(*impl, /*render=*/true);
        if (bridge == nullptr) {
            *why = "宿主没有接 config 桥（渲染面需要 UiHostOps::config）";
            return nullptr;
        }
        if (render_face != nullptr) *render_face = true;
        return bridge;
    }
    lift::ui::PluginCallHost* host = impl->call_host;
    if (host == nullptr || !host->inCallback()) {
        *why = "不在插件回调里";
        return nullptr;
    }
    lift::plugin::ConfigBridge* bridge = host->config();
    if (bridge == nullptr) {
        *why = "宿主没有接 config 桥（PluginCallHost::config 返回 nullptr）";
        return nullptr;
    }
    return bridge;
}

JSValue makeConfigDomain(JSContext* ctx, const std::string& domain, bool render) {
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) return obj.take();
    bool ok = setFunction(ctx, obj.raw(), "get", jsCtxConfigGet, 2, nullptr) &&
              setFunction(ctx, obj.raw(), "has", jsCtxConfigHas, 1, nullptr) &&
              setFunction(ctx, obj.raw(), "set", jsCtxConfigSet, 2, nullptr) &&
              setFunction(ctx, obj.raw(), "delete", jsCtxConfigDelete, 1, nullptr) &&
              setFunction(ctx, obj.raw(), "all", jsCtxConfigAll, 0, nullptr);
    if (ok) {
        Value id = qjs::newString(ctx, domain);
        ok = !id.isException() &&
             JS_DefinePropertyValueStr(ctx, obj.raw(), kConfigDomainProp, id.take(), 0) >= 0;
        if (!ok) qjs::clearException(ctx);
    } else {
        qjs::clearException(ctx);
    }
    if (ok) ok = markConfigObject(ctx, obj.raw(), render);
    if (!ok) return JS_EXCEPTION;
    return obj.take();
}

// `render=false` ⇒ RunCtx 的 `ctx.config`；`render=true` ⇒ 经典回退 HUD 的 `host.config`
// **与** `RenderCtx.config`（★ 2026-11-26：两条渲染面共用这一份，⛔ 不复制第二套）。
Value makeConfigObject(Impl& impl, bool render) {
    JSContext* ctx = impl.ctx;
    Value obj = qjs::newObject(ctx);
    if (obj.isException()) return obj;
    lift::plugin::ConfigBridge* bridge = configBridgeFor(impl, render);
    const bool available = bridge != nullptr && bridge->available();
    bool ok = defineReadOnly(ctx, obj.raw(), "available", qjs::newBool(ctx, available));
    if (ok) ok = setFunction(ctx, obj.raw(), "domain", jsCtxConfigDomain, 1, nullptr);
    if (ok) ok = markConfigObject(ctx, obj.raw(), render);
    if (!ok) return Value::exception(ctx);
    return obj;
}

// 经典回退 HUD 的 `host.config`（D2）：**getter**，每次访问按"这一帧有没有桥"现算。
// ★ 没有桥 / 不在 render ⇒ `undefined`（⛔ 不抛：HUD 每帧都要读，optional 能力不能把叠层打黑）。
// ★ 与 `RenderCtx.config` 的区别只在**缺桥时**：`RenderCtx.config` 恒是一个对象
//   （`available=false`），因为它是 d.ts 的成员、HUD 要能区分"成员缺失"与"接桥但没 appdata"。
JSValue jsHostConfig(JSContext* ctx, JSValueConst /*this_val*/, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        Impl* impl = implOf(ctx);
        if (impl == nullptr || impl->current == nullptr) return JS_UNDEFINED;
        lift::plugin::ConfigBridge* bridge = configBridgeFor(*impl, /*render=*/true);
        if (bridge == nullptr) return JS_UNDEFINED;
        Value config = makeConfigObject(*impl, /*render=*/true);
        if (config.isException()) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "host.config: 分配结果对象失败");
        }
        return config.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "host.config: 宿主异常（C++ exception at script boundary）");
    }
}

// `this` 必须是 `domain(id)` 返回的那个对象（方法被摘下来单独调用 ⇒ TypeError，⛔ 不猜域）。
bool configDomainOf(JSContext* ctx, JSValueConst this_val, std::string* domain, std::string* why) {
    if (!JS_IsObject(this_val)) {
        *why = "必须作为 domain 对象的方法调用";
        return false;
    }
    Value v = qjs::getProperty(ctx, this_val, kConfigDomainProp);
    if (v.isException() || !v.isString() || !qjs::toString(ctx, v.raw(), domain)) {
        qjs::clearException(ctx);
        *why = "必须作为 domain 对象的方法调用";
        return false;
    }
    return true;
}

JSValue jsCtxConfigDomain(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    try {
        if (!JS_IsObject(this_val)) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain: 必须作为 ctx.config 的方法调用");
        }
        std::string why;
        bool render_face = false;
        lift::plugin::ConfigBridge* bridge = configCall(ctx, this_val, &why, &render_face);
        if (bridge == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain: %s", why.c_str());
        }
        if (argc < 1 || !JS_IsString(argv[0])) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain(id): id 必须是字符串");
        }
        std::string id;
        if (!qjs::toString(ctx, argv[0], &id)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.config.domain: id 读取失败");
        }
        // ★ 没有 --appdata ⇒ `Error("no_appdata")`（⛔ 不降级成内存态）。
        if (!bridge->available()) return throwPlainError(ctx, "no_appdata");
        {
            HostPause pause(implOf(ctx));
            if (!bridge->checkDomain(id, &why)) {
                return JS_ThrowTypeError(ctx, "ctx.config.domain('%s'): %s", id.c_str(), why.c_str());
            }
        }
        return makeConfigDomain(ctx, id, render_face);
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.config.domain: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxConfigGet(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    try {
        std::string why;
        lift::plugin::ConfigBridge* bridge = configCall(ctx, this_val, &why, nullptr);
        if (bridge == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain(...).get: %s", why.c_str());
        }
        std::string domain;
        if (!configDomainOf(ctx, this_val, &domain, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain(...).get: %s", why.c_str());
        }
        if (argc < 1 || !JS_IsString(argv[0])) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').get(key): key 必须是字符串",
                                     domain.c_str());
        }
        std::string key;
        if (!qjs::toString(ctx, argv[0], &key)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').get: key 读取失败",
                                     domain.c_str());
        }
        lift::plugin::ConfigValue value;
        bool found = false;
        bool ok = true;
        // ★ HostPause 罩住**整段**：store 调用 + `ConfigValue → JS` 的建树（纯宿主 CPU）。
        Value out = Value::undefined(ctx);
        {
            HostPause pause(implOf(ctx));
            ok = bridge->get(domain, key, &value, &found, &why);
            if (ok && found) out = configValueToJs(ctx, value);
        }
        if (!ok) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').get('%s'): %s", domain.c_str(),
                                     key.c_str(), why.c_str());
        }
        if (!found) return argc >= 2 ? JS_DupValue(ctx, argv[1]) : JS_UNDEFINED;
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.config.domain(...).get: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxConfigHas(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    try {
        std::string why;
        lift::plugin::ConfigBridge* bridge = configCall(ctx, this_val, &why, nullptr);
        if (bridge == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain(...).has: %s", why.c_str());
        }
        std::string domain;
        if (!configDomainOf(ctx, this_val, &domain, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain(...).has: %s", why.c_str());
        }
        if (argc < 1 || !JS_IsString(argv[0])) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').has(key): key 必须是字符串",
                                     domain.c_str());
        }
        std::string key;
        if (!qjs::toString(ctx, argv[0], &key)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').has: key 读取失败",
                                     domain.c_str());
        }
        bool found = false;
        bool ok = true;
        {
            HostPause pause(implOf(ctx));
            ok = bridge->has(domain, key, &found, &why);
        }
        if (!ok) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').has('%s'): %s", domain.c_str(),
                                     key.c_str(), why.c_str());
        }
        return JS_NewBool(ctx, found);
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.config.domain(...).has: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxConfigSet(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    try {
        std::string why;
        lift::plugin::ConfigBridge* bridge = configCall(ctx, this_val, &why, nullptr);
        if (bridge == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain(...).set: %s", why.c_str());
        }
        std::string domain;
        if (!configDomainOf(ctx, this_val, &domain, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain(...).set: %s", why.c_str());
        }
        if (argc < 2) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').set(key, value)", domain.c_str());
        }
        if (!JS_IsString(argv[0])) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').set(key, value): key 必须是字符串",
                                     domain.c_str());
        }
        std::string key;
        if (!qjs::toString(ctx, argv[0], &key)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').set: key 读取失败",
                                     domain.c_str());
        }
        lift::plugin::ConfigValue value;
        std::size_t nodes = 0;
        // ★ 从**这里**到 `configSet` 返回都在同一个 HostPause 里：枚举/建树是宿主 CPU；
        //   `jsToConfigValue` 会在 `JS_GetProperty*` 前后 stop/resume（getter 仍算用户时间）。
        HostPause pause(implOf(ctx));
        if (!jsToConfigValue(ctx, argv[1], 1, &nodes, &pause, &value, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').set('%s'): %s", domain.c_str(),
                                     key.c_str(), why.c_str());
        }
        lift::plugin::ConfigReceipt receipt;
        const bool ok = bridge->set(domain, key, value, &receipt, &why);
        if (!ok) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').set('%s'): %s", domain.c_str(),
                                     key.c_str(), why.c_str());
        }
        Value out = qjs::newObject(ctx);
        if (out.isException()) return out.take();
        bool wrote = defineReadOnly(ctx, out.raw(), "applied", qjs::newBool(ctx, receipt.applied));
        if (wrote && receipt.code != nullptr && receipt.code[0] != '\0') {
            wrote = defineReadOnly(ctx, out.raw(), "code", qjs::newString(ctx, receipt.code));
        }
        if (!wrote) {
            qjs::clearException(ctx);
            return JS_EXCEPTION;
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.config.domain(...).set: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxConfigDelete(JSContext* ctx, JSValueConst this_val, int argc, JSValueConst* argv) {
    try {
        std::string why;
        lift::plugin::ConfigBridge* bridge = configCall(ctx, this_val, &why, nullptr);
        if (bridge == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain(...).delete: %s", why.c_str());
        }
        std::string domain;
        if (!configDomainOf(ctx, this_val, &domain, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain(...).delete: %s", why.c_str());
        }
        if (argc < 1 || !JS_IsString(argv[0])) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').delete(key): key 必须是字符串",
                                     domain.c_str());
        }
        std::string key;
        if (!qjs::toString(ctx, argv[0], &key)) {
            qjs::clearException(ctx);
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').delete: key 读取失败",
                                     domain.c_str());
        }
        bool removed = false;
        bool ok = true;
        {
            HostPause pause(implOf(ctx));
            ok = bridge->erase(domain, key, &removed, &why);
        }
        if (!ok) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain('%s').delete('%s'): %s",
                                     domain.c_str(), key.c_str(), why.c_str());
        }
        return JS_NewBool(ctx, removed);
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.config.domain(...).delete: 宿主异常（C++ exception at script boundary）");
    }
}

JSValue jsCtxConfigAll(JSContext* ctx, JSValueConst this_val, int /*argc*/, JSValueConst* /*argv*/) {
    try {
        std::string why;
        lift::plugin::ConfigBridge* bridge = configCall(ctx, this_val, &why, nullptr);
        if (bridge == nullptr) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain(...).all: %s", why.c_str());
        }
        std::string domain;
        if (!configDomainOf(ctx, this_val, &domain, &why)) {
            return JS_ThrowTypeError(ctx, "ctx.config.domain(...).all: %s", why.c_str());
        }
        lift::plugin::ConfigValue value;
        Value out = Value::undefined(ctx);
        {
            // ★ 整段（store + 建树）都在 HostPause 里；大域的 `all()` 不许被记成用户 2 ms。
            HostPause pause(implOf(ctx));
            value = bridge->all(domain);
            out = configValueToJs(ctx, value);
        }
        return out.take();
    } catch (...) {
        return JS_ThrowTypeError(ctx, "ctx.config.domain(...).all: 宿主异常（C++ exception at script boundary）");
    }
}

// =======================================================================================
// `PluginScript` 的实现
// =======================================================================================
class QuickJsPluginScript final : public PluginScript {
public:
    explicit QuickJsPluginScript(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
    ~QuickJsPluginScript() override {
        if (impl_) impl_->shutdown();
    }

    bool hasRender() const override { return impl_->plugin_has_render; }
    bool hasRun() const override { return impl_->plugin_has_run; }

    bool start(PluginCallHost& host, const lift::plugin::Budget& budget, TriggerSet* out,
               std::string* err) override {
        return call(host, impl_->plugin_start, true, nullptr, budget, out, err);
    }

    bool onTrigger(PluginCallHost& host, const lift::plugin::TriggerEvent& ev,
                   const lift::plugin::Budget& budget, TriggerSet* out, std::string* err) override {
        return call(host, impl_->plugin_on_trigger, false, &ev, budget, out, err);
    }

    bool overran() const override { return impl_->overran; }

    void shutdown() override { impl_->shutdown(); }

    // ---- `UiScript`（C2b）：render = 调 JS `render(RenderCtx)`；没有 render ⇒ true + 空 list ----
    bool render(UiBuilder& builder, const UiFrameReadings& readings, std::string* err) override {
        return renderFrame(builder, readings, err);
    }
    // ★ D2：`ctx.config` 的写盘口 —— present_sdl 在**每帧 render 之后**调一次（与 Run 回调末的
    //   `PluginHost` flush 同一口径；⛔ 不进 2 ms 预算 —— 它不在 Run 回调里）。
    void flushConfig(std::string* warn) override {
        if (impl_ != nullptr && impl_->render_bridge != nullptr) {
            impl_->render_bridge->flushIfDirty(warn);
        }
    }
    // ★ D2 的生命周期：`PluginHost` 先于本脚本析构 ⇒ 宿主在桥失效前清缓存（见 `UiScript`）。
    void forgetConfigBridge() override {
        if (impl_ != nullptr) impl_->render_bridge = nullptr;
    }
    // `UiScript::setBudget` 来的是**绝对** deadline（steady_clock 纳秒；0 = 不限）⇒ 直接用，
    // ⛔ 不再加 `steadyNowNs()`（那是回调用**相对**预算的形状）。
    void setBudget(std::uint64_t deadline_ns, std::uint32_t op_limit) override {
        impl_->deadline_ns = deadline_ns;
        impl_->op_limit = op_limit;
    }
    void setCallHost(PluginCallHost* host) override { impl_->call_host_persistent = host; }
    // ★ C2b-2：`ctx.draw.text` / `ctx.draw.measure` 的宿主字体（非拥有）。装载时还不知道模块
    //   有没有 `render`（`PluginLoadOptions.font` 因此保留）；窗口装配在**得知有 render 之后**
    //   才接线，所以这条 late setter 必须与装载期那条写**同一个**字段（`Impl::font`）。
    void setFont(lift::FontEngine* font) override { impl_->font = font; }

private:
    bool renderFrame(UiBuilder& builder, const UiFrameReadings& readings, std::string* err) {
        Impl& impl = *impl_;
        if (impl.ctx == nullptr) {
            if (err != nullptr) *err = "quickjs: 插件已 shutdown";
            return false;
        }
        // ★ 这一帧没有桥 / 没有 render ⇒ flush 必须是 no-op（⛔ 不能拿上一帧的桥再 flush 一次）。
        impl.render_bridge = nullptr;
        // ★ 没有 `render` 的插件**合法**（纯 Run / headless）：present_sdl 只在挂了 overlay 时调
        //   render ⇒ 这里必须干净地返回 true + 空 list（⛔ 不是失败、也不逼插件画东西）。
        if (!impl.plugin_has_render) return true;
        if (!impl.plugin_render.isFunction()) {
            if (err != nullptr) *err = impl.origin + ": 这个插件没有 render";
            return false;
        }
        // ★ 三个指针与 RunCtx 的注入同一套纪律：无论成败都复位（下一次 render 不能看到上一次的）。
        impl.builder = &builder;
        impl.current = &readings;
        impl.ops = 0;
        impl.overran = false;
        // ★ D2：把本帧的 config 桥缓存到 `render_bridge`（flushConfig 在 render 返回之后还要用它
        //   —— 那时 current / call_host 已经复位）。消费者 = 经典 `host.config` **与**
        //   `RenderCtx.config`（两条渲染面同一份）。
        impl.render_bridge = configBridgeFor(impl, /*render=*/true);
        // ★ render 的预算是 `setBudget` 给的**墙钟** deadline（`deadline_ns`）⇒ 这里必须清掉
        //   上一次回调留下的 CPU deadline（否则过期的 CPU deadline 会让 render 第一次中断就误判）。
        impl.cpu_deadline_ns = 0;
        // ★ render 里 `ctx.read` / `ctx.log` 也要有来路：宿主在 init 时设的**非拥有**指针在这里
        //   临时成为 `call_host`（回调外没有别的 ctx；回调期间由 `call()` 的形参覆盖）。
        impl.call_host = impl.call_host_persistent;
        bool ok = true;
        std::string local_err;
        try {
            Value ctx_obj;
            if (!buildRenderCtx(impl, &ctx_obj)) {
                ok = false;
                local_err = impl.origin + ": 组 RenderCtx 失败";
            } else {
                JSValueConst argv[1] = {ctx_obj.raw()};
                Value ret(impl.ctx, JS_Call(impl.ctx, impl.plugin_render.raw(), JS_UNDEFINED, 1, argv));
                if (ret.isException()) {
                    qjs::takeException(impl.ctx, impl.origin, &local_err);
                    ok = false;
                }
            }
        } catch (...) {
            ok = false;
            local_err = impl.origin + ": 宿主异常（C++ exception at script boundary）";
        }
        impl.builder = nullptr;
        impl.current = nullptr;
        impl.call_host = nullptr;
        if (!ok) {
            if (impl.overran) {
                local_err += "\n(plugin: render 超出预算（deadline / op_limit）⇒ 已中断)";
            }
            if (err != nullptr) *err = local_err;
            return false;
        }
        return true;
    }
    bool call(PluginCallHost& host, const Value& fn, bool is_start, const lift::plugin::TriggerEvent* ev,
              const lift::plugin::Budget& budget, TriggerSet* out, std::string* err) {
        Impl& impl = *impl_;
        if (impl.ctx == nullptr) {
            if (err != nullptr) *err = "quickjs: 插件已 shutdown";
            return false;
        }
        if (!fn.isFunction()) {
            if (err != nullptr) *err = impl.origin + ": 这个插件没有 " + (is_start ? "start" : "onTrigger");
            return false;
        }
        // ★ 预算两件：deadline（**本线程 CPU 钟**；0 = 不限）+ op_limit（中断步长）；**每次回调重置**。
        //   ⛔ 不要沿用 render 的 20 ms / 1e6（那会让一次跑飞的回调卡住 guest 20 ms；合同 §4.5 D5）。
        // ★ CPU 钟的**首发频率标定**（Windows 主源需要，~0.8 ms）放在计时窗**之外**：
        //   它不属于任何回调。不预热的话，第一次回调的 `elapsed_us`（宿主安全网）会凭空多这一段，
        //   而 `js_cpu_ns` 又因起点在标定之后而不含它 ⇒ 读数与判据都会歪。
        (void)lift::plugin::pluginThreadCpuNs();
        // ★ 诊断起点 = 进入 `call()`（只给读数；⛔ 不是预算起点）。
        const std::uint64_t start_ns = steadyNowNs();
        // ★ **预算不在入口起算**（2026-11-21 收窄，用户裁决 D5 可改）：2 ms 只罩**用户 JS 本体**
        //   ⇒ 起点挪到 `JS_Call` 之前（见下面的 `js_start_cpu_ns`）。组 ctx / 事件对象期间没有 JS 在跑
        //   ⇒ 这里先把两个 deadline 都清 0（⛔ 不要让上一次回调的 deadline 在这段里误触）。
        impl.deadline_ns = 0;
        impl.cpu_deadline_ns = 0;
        impl.op_limit = budget.op_limit;
        impl.ops = 0;
        impl.overran = false;
        // ★ **宿主调用暂停的累计值每次回调归零**（`Impl::paused_ns`）：它只描述"本次回调里
        //   宿主替用户 JS 花了多少 CPU"，跨回调累计会让下一次的预算凭空变宽。
        impl.paused_ns = 0;
        impl.call_host = &host;

        bool ok = true;
        std::string local_err;
        // 诊断子阶段（与 `elapsed_us` 同一对墙钟切出来的三段；⛔ 不影响预算）：
        //   build = 组 ctx / 事件对象（每次回调新建 ~20 个 JS 对象 + C 函数）
        //   js    = 用户 JS 本体（墙钟 `js_ns` + **CPU** `js_cpu_ns`；含 `ctx.log` 的 stderr 写）
        //   collect = 返回值 → TriggerSet（造 shared_ptr / 查对象表）
        std::uint64_t build_ns = 0;
        std::uint64_t js_ns = 0;      // 墙钟（诊断对照；⛔ 不是 2 ms 判据）
        std::uint64_t js_cpu_ns = 0;  // CPU（**2 ms 判据**用的就是这一段）
        std::uint64_t collect_ns = 0;
        try {
            Value ctx_obj = Value::undefined(impl.ctx);
            Value ev_obj = Value::undefined(impl.ctx);
            {
                // `ctx` 是**每次回调新建**的值对象（D-C2-7）；`now` 里是这一拍的新鲜快照。
                Value built;
                if (!buildRunCtx(impl, &built)) {
                    ok = false;
                    local_err = impl.origin + ": 组 ctx 失败";
                } else {
                    ctx_obj = std::move(built);
                }
            }
            if (ok && !is_start && ev != nullptr) {
                ev_obj = makeEventObject(impl, *ev);
                if (ev_obj.isException()) {
                    qjs::clearException(impl.ctx);
                    ok = false;
                    local_err = impl.origin + ": 组事件对象失败";
                }
            }
            const std::uint64_t built_ns = steadyNowNs();
            build_ns = built_ns >= start_ns ? built_ns - start_ns : 0;
            if (ok) {
                JSValueConst argv[2] = {ctx_obj.raw(), ev_obj.raw()};
                const int argc = is_start ? 1 : 2;
                const std::uint64_t js_start_ns = steadyNowNs();  // 墙钟读数（诊断用；⛔ 不是判据）
                // ★ **预算起点 = 用户 JS 本体的 CPU 起点**（组 ctx / 事件对象之后、`JS_Call` 之前）。
                //   中断回调用 `pluginThreadCpuNs()` 比这个 deadline（⛔ 不是 `steadyNowNs()`：
                //   "CPU 起点 + 墙钟比较"会第一次中断就误判）。模块装载 / ctx 构造 / 收集 / 宿主调度
                //   **都不在** 2 ms 里（2026-11-21 收窄；缺陷 REQ-01M7VVDV69MJXRHNDGR50H7VHC）。
                const std::uint64_t js_start_cpu_ns = lift::plugin::pluginThreadCpuNs();
                impl.cpu_deadline_ns =
                    budget.deadline_ns != 0 ? js_start_cpu_ns + budget.deadline_ns : 0;
                Value ret(impl.ctx, JS_Call(impl.ctx, fn.raw(), JS_UNDEFINED, argc, argv));
                const std::uint64_t js_end_ns = steadyNowNs();
                js_ns = js_end_ns >= js_start_ns ? js_end_ns - js_start_ns : 0;
                // ★ 终点用**同一把 CPU 钟**；然后立刻撤掉 deadline（预算窗只到 JS 返回为止）。
                const std::uint64_t js_end_cpu_ns = lift::plugin::pluginThreadCpuNs();
                const std::uint64_t js_raw_cpu_ns =
                    js_end_cpu_ns >= js_start_cpu_ns ? js_end_cpu_ns - js_start_cpu_ns : 0;
                // ★ 扣掉**宿主调用暂停**（`ctx.profile.end` 一类宿主能力；见 `HostPause`）⇒ `js_cpu_ns`
                //   是"用户 JS 本体自己的 CPU"。诊断行的 `js_cpu_us` 是**扣除后**的值（真跑飞时
                //   它仍是真值：`paused_ns` 只含宿主段）。⛔ 不减用户 JS 的时间。
                js_cpu_ns = js_raw_cpu_ns >= impl.paused_ns ? js_raw_cpu_ns - impl.paused_ns : 0;
                impl.cpu_deadline_ns = 0;
                if (ret.isException()) {
                    qjs::takeException(impl.ctx, impl.origin, &local_err);
                    ok = false;
                } else {
                    const std::uint64_t collect_start_ns = steadyNowNs();
                    ok = collectTriggers(impl, ret.raw(), out, &local_err);
                    const std::uint64_t collect_end_ns = steadyNowNs();
                    collect_ns = collect_end_ns >= collect_start_ns ? collect_end_ns - collect_start_ns : 0;
                }
            }
        } catch (...) {
            ok = false;
            local_err = impl.origin + ": 宿主异常（C++ exception at script boundary）";
        }
        // ★ 终点与起点同一对墙钟；范围 = 组 ctx / 事件对象 + JS_Call + collectTriggers。
        //   ⚠ 这**不是**预算窗（预算窗只有 `js_cpu_ns` 那一段，CPU 钟）—— 它只给 `gap_us` 与 `overran` 归因用。
        const std::uint64_t end_ns = steadyNowNs();
        const std::uint64_t elapsed_ns = end_ns >= start_ns ? end_ns - start_ns : 0;
        // ⛔ 只填读数；输出（含"超预算无论开关都打"那条）在 `plugin_host.cpp` 的 `runCallback`。
        lift::plugin::CallbackDiag& diag = lift::plugin::callbackDiag();
        diag.inner_ns = elapsed_ns;
        diag.build_ns = build_ns;
        diag.js_ns = js_ns;
        diag.js_cpu_ns = js_cpu_ns;  // ★ 2 ms 判据读的是它（plugin_host.cpp 的 `js_late`）
        // 宿主调用暂停的**读数**（已从 `js_cpu_ns` 里扣掉的那一段；⛔ 不是判据，只给诊断行看
        // "这 2.5 ms 是谁的"）。`host_ns`（墙钟）与它不同源，⛔ 不要交叉相减。
        diag.host_pause_ns = impl.paused_ns;
        diag.collect_ns = collect_ns;
        diag.module_load_ns = impl.module_load_ns;
        diag.inner_overran = impl.overran;
        impl.call_host = nullptr;
        if (!ok) {
            if (impl.overran) {
                local_err += "\n(plugin: 回调超出预算（deadline / op_limit）⇒ 已中断)";
            }
            if (err != nullptr) *err = local_err;
            return false;
        }
        return true;
    }

    std::unique_ptr<Impl> impl_;
};

}  // namespace

std::unique_ptr<ScriptRuntime> makeQuickJsRuntime() { return std::make_unique<QuickJsRuntime>(); }

// =======================================================================================
// 插件装载（`--plugin` 的唯一入口；实现见上面的匿名命名空间）
//
// ★ **强制 module**：插件必须是 ES module（`export default`）⇒ 不走 classic 探测
//   （内部 hud 回退那条路才 classic 优先）。⇒ 一个 classic 文件用 `--plugin` 打开会得到
//   "插件必须 export default"这条明确错误，而不是被当成 HUD 静默跑起来。
// ★ 取 default 导出的路：`JS_Eval(MODULE|COMPILE_ONLY)` → `JS_ResolveModule` →
//   `JS_EvalFunction`（求值 + 返回 promise）→ drain → `JS_GetModuleNamespace(ctx, m)` →
//   `getProperty(ns, "default")`。
//   ⚠ **不能**从求值返回的 promise 里取：`js_evaluate_module` 用 `JS_UNDEFINED` resolve
//   （`quickjs.c` 的 `js_evaluate_module` 里 `resolving_funcs[0]` 的参数就是 `JS_UNDEFINED`）
//   ⇒ namespace 只能从 module def 拿（`JS_GetModuleNamespace` 是公开 API）。
// ⚠ **不能** `JS_FreeValue` 一个 module 值（`quickjs.c` 的 `case JS_TAG_MODULE` 是 `abort()`）：
//   `module_value.take()` 把所有权交给 `JS_EvalFunction`。
// =======================================================================================
std::unique_ptr<PluginScript> makeQuickJsPlugin(const std::string& source, const PluginLoadOptions& opts,
                                                std::string* err) {
    const std::string origin = opts.origin;
    std::unique_ptr<Impl> impl = createImpl(origin, opts.font, err);
    if (impl == nullptr) return nullptr;
    JSContext* ctx = impl->ctx;

    // ---- module loader：入口 = origin（绝对路径）；相对 import 的根 = 它的目录 ----
    impl->module_entry_is_path = isFilePathLike(origin);
    impl->module_root = pathDirName(origin);
    impl->module_sources[origin] = source;
    JS_SetModuleLoaderFunc(impl->rt, moduleNormalize, moduleLoad, impl.get());

    // ★ 诊断（`module_load_us`）：装载 = 编译 + resolve + 求值 + drain。它**在** `call()` 的预算起点
    //   **之前**完成 ⇒ 2 ms 不含装载；这个读数用来证明（而不是声称）这一点。
    const std::uint64_t module_load_start_ns = steadyNowNs();
    Value module_value(ctx, JS_Eval(ctx, source.c_str(), source.size(), origin.c_str(),
                                    JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY));
    if (module_value.isException()) {
        qjs::takeException(ctx, origin, err);
        appendModuleErrorAt(*impl, err);
        return nullptr;
    }
    if (JS_ResolveModule(ctx, module_value.raw()) < 0) {
        qjs::takeException(ctx, origin, err);
        appendModuleErrorAt(*impl, err);
        module_value.take();  // 见头注：module 值不能 FreeValue
        return nullptr;
    }
    JSModuleDef* module = static_cast<JSModuleDef*>(JS_VALUE_GET_PTR(module_value.raw()));
    Value eval_ret(ctx, JS_EvalFunction(ctx, module_value.take()));
    if (eval_ret.isException()) {
        qjs::takeException(ctx, origin, err);
        appendModuleErrorAt(*impl, err);
        return nullptr;
    }
    std::string drain_err;
    if (!drainPendingJobs(*impl, &drain_err)) {
        if (err != nullptr) *err = drain_err.empty() ? (origin + ": 插件求值失败") : drain_err;
        return nullptr;
    }
    const JSPromiseStateEnum promise_state = JS_PromiseState(ctx, eval_ret.raw());
    if (promise_state == JS_PROMISE_REJECTED) {
        Value reason(ctx, JS_PromiseResult(ctx, eval_ret.raw()));
        formatThrownValue(ctx, origin, reason.raw(), err);
        return nullptr;
    }
    if (promise_state == JS_PROMISE_PENDING) {
        if (err != nullptr) {
            *err = origin + ": 插件顶层 await 没有可执行的异步来源（module 求值未完成；回调必须同步）";
        }
        return nullptr;
    }
    impl->module_load_ns = steadyNowNs() - module_load_start_ns;  // 诊断：见上面的起点注

    // ---- default 导出 ----
    Value ns(ctx, JS_GetModuleNamespace(ctx, module));
    if (ns.isException()) {
        qjs::takeException(ctx, origin, err);
        return nullptr;
    }
    Value def = qjs::getProperty(ctx, ns.raw(), "default");
    if (def.isException()) {
        qjs::takeException(ctx, origin, err);
        return nullptr;
    }
    if (!def.isObject()) {
        if (err != nullptr) {
            *err = origin +
                   ": 插件必须 `export default { render?, start?, onTrigger? }`（读到 "
                   "default 不是对象；⛔ 不读 globalThis / 命名导出）";
        }
        return nullptr;
    }

    // ---- 三个可选成员 + 成对 / 至少一个（合同 §4.6 797–799）----
    bool has_render = false;
    bool has_start = false;
    bool has_trigger = false;
    const auto readMember = [&](const char* name, Value* out, bool* present) -> bool {
        Value v = qjs::getProperty(ctx, def.raw(), name);
        if (v.isException()) {
            qjs::takeException(ctx, origin, err);
            return false;
        }
        if (v.isUndefined() || v.isNull()) {
            *present = false;
            return true;
        }
        if (!v.isFunction()) {
            if (err != nullptr) {
                *err = origin + ": `export default` 的 `" + name + "` 必须是函数（`export default { " +
                       name + "(ctx) { … } }`）";
            }
            return false;
        }
        *present = true;
        *out = std::move(v);
        return true;
    };
    if (!readMember("render", &impl->plugin_render, &has_render)) return nullptr;
    if (!readMember("start", &impl->plugin_start, &has_start)) return nullptr;
    if (!readMember("onTrigger", &impl->plugin_on_trigger, &has_trigger)) return nullptr;
    if (has_start != has_trigger) {
        if (err != nullptr) {
            *err = origin +
                   ": `start` 与 `onTrigger` 必须**成对**（有 start 就必须有 onTrigger，反之亦然）："
                   "只有 render 的插件是合法的 UI-only 插件";
        }
        return nullptr;
    }
    if (!has_render && !has_start) {
        if (err != nullptr) *err = origin + ": 插件至少要有 render 或 start+onTrigger 之一（三个都缺席）";
        return nullptr;
    }
    impl->plugin_has_render = has_render;
    impl->plugin_has_run = has_start && has_trigger;

    // ---- initialState（可选；`ctx.state` 在第一次回调前由它浅拷贝）----
    Value seed = qjs::getProperty(ctx, def.raw(), "initialState");
    if (seed.isException()) {
        qjs::takeException(ctx, origin, err);
        return nullptr;
    }
    if (!seed.isUndefined() && !seed.isNull()) {
        impl->has_state_seed = true;
        impl->state_seed = std::move(seed);
    }

    // ---- `ctx.params`（只读：非 writable ⇒ strict 下赋值抛 TypeError）----
    Value params = qjs::newObject(ctx);
    if (params.isException()) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = origin + ": 分配 ctx.params 失败";
        return nullptr;
    }
    if (opts.params != nullptr) {
        for (const auto& kv : *opts.params) {
            if (!defineReadOnly(ctx, params.raw(), kv.first.c_str(), qjs::newString(ctx, kv.second))) {
                if (err != nullptr) *err = origin + ": 写 ctx.params['" + kv.first + "'] 失败";
                return nullptr;
            }
        }
    }
    impl->plugin_params = std::move(params);

    // ---- `ctx.ui.state` 的数据面（宿主本地 JS 状态；C2a 只建写入面）----
    impl->ui_data = qjs::newObject(ctx);
    if (impl->ui_data.isException()) {
        qjs::clearException(ctx);
        if (err != nullptr) *err = origin + ": 分配 ctx.ui.state 失败";
        return nullptr;
    }

    // ---- trigger 的 JS class（带 finalizer ⇒ 对象表有界）----
    JSClassID class_id = 0;
    class_id = JS_NewClassID(impl->rt, &class_id);
    JSClassDef class_def{};
    class_def.class_name = "LiftPluginTrigger";  // quickjs 要求纯 ASCII
    class_def.finalizer = &triggerFinalizer;
    if (class_id == 0 || JS_NewClass(impl->rt, class_id, &class_def) < 0) {
        if (err != nullptr) *err = origin + ": 注册 trigger 对象类失败";
        return nullptr;
    }
    impl->trigger_class = class_id;

    impl->plugin_default = std::move(def);
    return std::make_unique<QuickJsPluginScript>(std::move(impl));
}

}  // namespace lift::ui
