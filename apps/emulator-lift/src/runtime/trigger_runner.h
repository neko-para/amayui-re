// emulator-lift · **TriggerRunner** —— armed 集合的唯一边界与唯一 pump（C1b）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.2（具名 armed 集合 / 一读点最多一个 / 超时 /
//       owner / done·fail）、§4.11（`AfterDelay` 是宿主墙钟、NON-DIFFABLE）。
// 施工图：`.tmp/plugin-runtime/c1-design-review.md` §2.3（poll 顺序与"一读点一个事件"）、
//         §3.2（状态机全文）、§3.3（owner / timeout / budget 归谁）、§5.3（签名）。
//
// ★ **只有 `atPoint` 这一个 pump**（合同 §6 行 2）：`aot_run.cpp` / `machine.cpp` 里出现
//   `TriggerRunner` / `onTrigger(` 就是"第二 pump / 块边界触发"⇒ 由 `input-plan-shape.test.mjs` 的
//   禁用词当场红。⛔ 不要给本类加独立 timer / 线程 / `paused` 标志：
//   暂停 ⇒ guest 不执行 ⇒ 没有读点 ⇒ **结构上**不 poll；headless 只要跑 guest 就照样 poll。
//
// ★ **一读点最多一个事件（不只是"最多一个 fire"）**：两个事件会调 `onTrigger` **两次**，而每次的
//   返回值是"完整替换集合" ⇒ 第二次的替换会覆盖第一次的结果，且被覆盖掉的那个可能正是第二次要报的
//   事件对象 ⇒ 语义不可定义（合同 §4.2 413–415）。⇒ 命中即 fire 并 return，⛔ 不许继续扫成同点级联。
//   本拍**同时**到期多个 timeout：只报**注册顺序的第一个**（其余留到下一拍；契约没写这条，这是
//   `.tmp/plugin-runtime/c1-design-review.md` §2.3 的裁决点）。
//   **fire 优先于 timeout**：timeout 的语义是"等不到条件"；条件在同一拍成立就没有可等的了。
//   注册顺序靠前但已超时的那个**不阻断**后面的 fire（它下一拍才 timeout）。
//
// ★ 钟由**参数**传入（`atPoint(..., now_ms, ...)`）：runner 不持第二把钟 ⇒ 驱动方把"这一拍的
//   墙钟读数"给一次，`AfterDelay` / timeout / per-arm 重置全用同一个数。
//   `setClockForTest` / `setNsClockForTest` 只给 `begin()`（它没有 now_ms 参数）与 budget 用。
//   ⛔ 不接 `Clock` / `attachClock` / `pausedTotalMs()`（那是配方执行器的钟，**扣暂停**；
//   这里的墙钟**不扣**，两者语义相反 —— 见合同 §4.2 440–441 与 D3）。
//
// ★ 预算：**2 ms 只罩用户 JS 本体**（判据在 `PluginHost::runCallback`，靠 `CallbackDiag::js_cpu_ns`
//   —— **CPU 钟**，见 `budget.h`）。
//   本类量的 `start → end` 是整个宿主段 ⇒ 只留**宿主安全网** = 预算 × 10（2 ms ⇒ 20 ms），
//   **仍是墙钟**（`pluginNowNs()`）：超了清 armed + fail（`lastError()` 写清 `… (host)` + 哪个 trigger）。
//   ⛔ 不静默继续跑回调，⛔ 不把安全网换成 CPU 钟。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/budget.h"
#include "runtime/trigger.h"

namespace lift::plugin {

struct Context;  // 指针聚合上下文（`context.h`）

class TriggerRunner {
public:
    enum class Result : std::uint8_t {
        None = 0,   // 本读点没有事件
        Fired,      // 触发了一个（`ev->what == Fired`）
        TimedOut,   // 超时了一个（`ev->what == TimedOut`；⛔ 不计 `maxFires`）
        Done,       // 集合已空（`[]` / 从未 arm / 已消费完）
        Failed,     // fail（重复 name / 耗尽 / 预算超限 / 非法对象）
    };

    using NowFn = std::uint64_t (*)();    // 宿主**墙钟**毫秒（⛔ 不扣暂停）
    using NowNsFn = std::uint64_t (*)();  // budget 用的纳秒

    // 测试缝：nullptr = 生产路径（`steady_clock`）。⛔ 不在这里做暂停补偿（见头注）。
    void setClockForTest(NowFn now_ms) { now_fn_ = now_ms; }
    void setNsClockForTest(NowNsFn now_ns) { now_ns_fn_ = now_ns; }
    // install 时给每个 armed trigger **盖章**（回调伪造不了）。
    void setOwner(std::uint32_t owner) { owner_ = owner; }
    // 本切片的 handler 调用至少能设 / 放宽（`Budget{}` = 缺省 2 ms）。
    void setBudget(Budget budget) { budget_ = budget; }
    Budget budget() const { return budget_; }

    // 生命周期（C1 由夹具调；C2/C5 由 RunControl 调）。
    // `begin` 清掉上一次的 armed / done / failed / 错误，再调 `handler.start()` 并 install 它返回的集合。
    bool begin(TriggerHandler& handler, Context& ctx, std::string* err);

    // ★ 每个读点调**一次**（采样后、`releaseDue` 前；合同 §4.2 445）。返回 0 或 1 个事件。
    //   `ev` 非空时填"被触发 / 超时的那一个"（fail 路径也会填：事件确实发生了）。
    Result atPoint(const TriggerReadings& now, std::uint64_t now_ms, Context& ctx, TriggerEvent* ev,
                   std::string* err);

    // 只从 armed 里抹掉该 owner 的槽（返回抹掉几个）。⛔ **不**置 done：
    // "Run 结束 ⇒ 没有 armed"与"回调返回 `[]` ⇒ done"是两件事（后者的语义是正常收口）。
    std::size_t releaseOwner(std::uint32_t owner);

    // ---- 读数（`RunStatus` 的真源；⛔ 不是性能判据）----
    bool done() const { return done_; }
    bool failed() const { return failed_; }
    const std::string& failName() const { return fail_name_; }
    const std::string& lastError() const { return last_error_; }
    const std::vector<Trigger>& armed() const { return armed_; }  // 注册顺序
    std::vector<std::string> armedNames() const;                  // 派生，同序
    std::size_t armedCount() const { return armed_.size(); }
    // 某个对象已经 fire 了几次（**续计**的证据；timeout 不增）。
    std::uint32_t firesOf(const Trigger& trigger) const;

private:
    // per-arm 的槽元数据（`armed_ms` / `deadline_ms` 属于**这一次 arm**，⛔ 不进对象）。
    struct SlotMeta {
        std::uint64_t armed_ms = 0;     // AfterDelay 的计时起点（每次 arm 重置）
        std::uint64_t deadline_ms = 0;  // timeout 的到期点（0 = 不设；每次 arm 重置）
    };

    std::uint64_t nowMs() const;
    std::uint64_t nowNs() const;
    // 本读点这一槽满不满足（Reenter 的边沿在这里更新：**只有有效读数才写** last_inside）。
    bool satisfied(const Trigger& trigger, std::uint64_t armed_ms, std::uint64_t now_ms,
                   const TriggerReadings& now) const;
    // Once 已 fire / maxFires 已耗尽（⇒ 同一对象再放回 = fail，⛔ 不是静默重置）。
    static bool exhausted(const Trigger& trigger);
    // 校验 + **完整替换** armed 集合（`[]` = done）。失败 ⇒ failed_ + 点名 + lastError。
    bool install(TriggerHandler::TriggerSet next, std::uint64_t now_ms, std::string* err);
    // 统一 fail 收尾：清 armed（⛔ 不保留旧集合 —— 保留会让"同一拍继续触发"）+ 写 lastError。
    bool failRun(const std::string& message, const std::string& name, std::string* err);
    // 调一次 `handler.onTrigger` 并按**宿主安全网**（预算 × 10）判超限。
    bool invokeOnTrigger(Context& ctx, const TriggerEvent& ev, std::uint64_t now_ms, std::string* err);

    // ★ `armed_` 与 `armed_meta_` **同序同长**（唯一不变量；只在 install / releaseOwner 里一起改）。
    std::vector<Trigger> armed_;
    std::vector<SlotMeta> armed_meta_;
    TriggerHandler* handler_ = nullptr;
    bool done_ = false;
    bool failed_ = false;
    std::string fail_name_;
    std::string last_error_;
    std::uint32_t owner_ = 0;
    Budget budget_{};
    NowFn now_fn_ = nullptr;
    NowNsFn now_ns_fn_ = nullptr;
};

}  // namespace lift::plugin
