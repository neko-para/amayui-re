// emulator-lift · `TriggerRunner` 的实现 —— 口径见 `trigger_runner.h` 的头注（合同 §4.2 / §6 行 31–33、36–37）。
//
// ★ 状态机只有这一段（`.tmp/plugin-runtime/c1-design-review.md` §3.2）：begin → install →
//   per-point（fire 优先 / timeout 次之 / 至多一个事件）→ install。⛔ 不要在这里长出别的状态
//   （配方 VM 那套"当前步 / 已施加 / 步骤种类"一个都不许进来 —— 由 `tools/test/core-shape.test.mjs`
//   的禁用词表钉住）。
#include "runtime/trigger_runner.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "runtime/context.h"  // Context（handler 的参数类型）
#include "runtime/trigger.h"

namespace lift::plugin {
namespace {

// 生产路径的宿主墙钟（毫秒；⛔ 不是 guest 虚拟时钟，⛔ 不扣暂停）。
std::uint64_t steadyNowMs() {
    using Clock = std::chrono::steady_clock;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch());
    return static_cast<std::uint64_t>(ms.count());
}

// 事件里的"本读点读数"（timeout 用的是**当前**读数，不是 arm 时的 —— 回调据此判断"在哪儿等不到"）。
void fillFromReadings(TriggerEvent* ev, const TriggerReadings& now) {
    ev->script = now.script;
    ev->pos = now.pos;
    ev->pos_ok = now.pos_ok;
    ev->point = now.point;
    ev->read_index = now.read_index;
}

}  // namespace

std::uint64_t TriggerRunner::nowMs() const {
    if (now_fn_ != nullptr) return now_fn_();
    return steadyNowMs();
}

std::uint64_t TriggerRunner::nowNs() const {
    if (now_ns_fn_ != nullptr) return now_ns_fn_();
    return pluginNowNs();
}

std::uint32_t TriggerRunner::firesOf(const Trigger& trigger) const {
    return trigger.state != nullptr ? trigger.state->fires : 0;
}

std::vector<std::string> TriggerRunner::armedNames() const {
    std::vector<std::string> names;
    names.reserve(armed_.size());
    for (const Trigger& trigger : armed_) names.push_back(trigger.name);
    return names;
}

// =====================================================================================
// 判据：这一槽在这一拍满不满足（§2.2 的四种 kind）
// =====================================================================================
bool TriggerRunner::satisfied(const Trigger& trigger, std::uint64_t armed_ms, std::uint64_t now_ms,
                              const TriggerReadings& now) const {
    switch (trigger.kind) {
        case TriggerKind::AfterScript: {
            // 纯电平门：只比脚本名（**无区间**）；无效读数**不改变状态**（§2.2）。
            if (!now.script_ok) return false;
            const bool inside = now.script == trigger.script;
            if (trigger.mode == TriggerMode::Once) return inside;
            const bool edge = inside && !trigger.state->last_inside;
            trigger.state->last_inside = inside;  // ★ 只有有效读数才写
            return edge;
        }
        case TriggerKind::AfterPosition: {
            // 半开区间 [lo, hi) + 脚本名；**任何**无效读数（位置读不到 / 脚本名读不到）都**不改变状态**
            // （合同 §4.2 438）。★ 必须同时判 `script_ok`：只判 `pos_ok` 时，一次"名字读不到、位置可读"
            // 的读数会被当成"有效的区间外"⇒ 把 `last_inside` 写成 false ⇒ 下一条**有效** inside 被当成
            // 新的外→内 ⇒ Reenter **多触发一次**（静默，正是 `after-position-once` (c) 要抓的形态）。
            if (!now.pos_ok || !now.script_ok) return false;
            const bool inside = now.script == trigger.script && now.pos >= trigger.lo && now.pos < trigger.hi;
            if (trigger.mode == TriggerMode::Once) return inside;
            const bool edge = inside && !trigger.state->last_inside;
            trigger.state->last_inside = inside;  // ★ 只有有效读数才写
            return edge;
        }
        case TriggerKind::AfterDelay: {
            // 宿主墙钟累计（暂停不冻结）；`armed_ms` 是**本次** arm 的那一刻 ⇒ 计时从头。
            if (now_ms < armed_ms) return false;  // 钟回退：不算到期
            return now_ms - armed_ms >= trigger.delay_ms;
        }
        case TriggerKind::AtReadPoint:
            // 下一个匹配读点（Count = 任意）；`max_fires` 的必填校验在 install。
            return trigger.point_filter == InputPoint::Count || trigger.point_filter == now.point;
    }
    return false;  // 到不了（install 已经拒掉未知 kind）
}

bool TriggerRunner::exhausted(const Trigger& trigger) {
    if (trigger.state == nullptr) return false;
    // Once 触发一次就耗尽（合同 §4.2 430：同一对象再放回 ⇒ **fail**，⛔ 不是静默重置）。
    if (trigger.mode == TriggerMode::Once && trigger.state->fires >= 1) return true;
    if (trigger.max_fires > 0 && trigger.state->fires >= trigger.max_fires) return true;
    return false;
}

// =====================================================================================
// fail 收尾
// =====================================================================================
bool TriggerRunner::failRun(const std::string& message, const std::string& name, std::string* err) {
    failed_ = true;
    done_ = false;
    // ★ 清 armed（⛔ 不保留旧集合：保留会让"同一拍/下一拍还继续触发"）。
    armed_.clear();
    armed_meta_.clear();
    last_error_ = message;
    if (!name.empty()) fail_name_ = name;
    if (err != nullptr) *err = message;
    return false;
}

// =====================================================================================
// install：校验 + **完整替换**（`[]` = done）
// =====================================================================================
bool TriggerRunner::install(TriggerHandler::TriggerSet next, std::uint64_t now_ms, std::string* err) {
    if (next.empty()) {
        // `null` / `[]` = done（⛔ 空集合不是"永远等"）；⛔ 不自动 finish / quit。
        armed_.clear();
        armed_meta_.clear();
        done_ = true;
        return true;
    }

    std::vector<Trigger> new_armed;
    std::vector<SlotMeta> new_meta;
    new_armed.reserve(next.size());
    new_meta.reserve(next.size());

    for (Trigger& trigger : next) {
        // ① 没有运行期状态：手搓的 `Trigger{}` 造不出预算 / 边沿 ⇒ 响亮失败（⛔ 不替它补一个）。
        if (trigger.state == nullptr) return failRun("trigger without state: " + trigger.name, trigger.name, err);
        // ② name 必填（它是插件侧状态名，也是 fail 点名的那个标识）。
        if (trigger.name.empty()) return failRun("trigger without name", std::string(), err);
        // ③ 同一 armed 集合内 name 唯一（重复 ⇒ fail 并点名；⛔ 不静默覆盖）。
        for (const Trigger& seen : new_armed) {
            if (seen.name == trigger.name) {
                return failRun("duplicate trigger name: " + trigger.name, trigger.name, err);
            }
        }
        // ④ kind 必须是本切片支持的那四种（`Frame` 留给 C2）。
        switch (trigger.kind) {
            case TriggerKind::AfterScript:
            case TriggerKind::AfterPosition:
            case TriggerKind::AfterDelay:
            case TriggerKind::AtReadPoint:
                break;
            default:
                // ★ ⚠ **C2 加 `TriggerKind::Frame` 时必须回到这里加一个显式 case**（并实现 Frame 的判据）。
                //   本 `default:` 吃掉了 `-Wswitch` ⇒ 新增枚举值**不会**编译红，只会运行期
                //   "unsupported trigger kind"（一次很贵的静默：C2 以为装了 Frame、其实每次 install 都 fail）。
                return failRun("unsupported trigger kind: " + trigger.name, trigger.name, err);
        }
        // ⑤ AtReadPoint 的 `max_fires` 必填 > 0（否则"每个读点都触发"= 无限灌输入）。
        if (trigger.kind == TriggerKind::AtReadPoint && trigger.max_fires == 0) {
            return failRun("atReadPoint requires maxFires > 0: " + trigger.name, trigger.name, err);
        }
        // ⑥ 已耗尽（Once 已 fire / maxFires 已满）⇒ fail（⛔ 不是静默重置预算）。
        if (exhausted(trigger)) return failRun("trigger exhausted: " + trigger.name, trigger.name, err);

        // ★ owner 由 runner **盖章**（工厂那个值只是初值 ⇒ 回调伪造不了）。
        trigger.owner = owner_;
        SlotMeta meta;
        meta.armed_ms = now_ms;                                               // per-arm：每次 arm 重置
        meta.deadline_ms = trigger.timeout_ms != 0 ? now_ms + trigger.timeout_ms : 0;
        new_armed.push_back(std::move(trigger));
        new_meta.push_back(meta);
    }

    armed_ = std::move(new_armed);
    armed_meta_ = std::move(new_meta);
    return true;
}

// =====================================================================================
// begin
// =====================================================================================
bool TriggerRunner::begin(TriggerHandler& handler, Context& ctx, std::string* err) {
    handler_ = &handler;
    done_ = false;
    failed_ = false;
    fail_name_.clear();
    last_error_.clear();
    armed_.clear();
    armed_meta_.clear();
    if (err != nullptr) err->clear();

    const std::uint64_t start_ns = nowNs();
    TriggerHandler::TriggerSet next = handler.start(ctx, err);
    const std::uint64_t end_ns = nowNs();
    // ★ 2026-11-21 收窄（用户裁决 D5 可改）：**2 ms 只罩用户 JS 本体**，判据在 handler 侧
    //   （`PluginHost` 用 `CallbackDiag::js_ns`）。本函数量的 `start → end` 是**整个宿主段**
    //   （组 ctx / 收 TriggerSet / 调度都在内）⇒ 这里只留**宿主安全网** = 预算 × 10（2 ms ⇒ 20 ms）：
    //   宿主段被卡死时照样响亮失败，但 reason 带 `(host)`（⛔ 不与用户 JS 超限混淆）。
    const Budget host_net{hostSafetyNetNs(budget_), budget_.op_limit};
    if (budgetExceeded(host_net, start_ns, end_ns)) {
        return failRun(callbackBudgetReason("start", std::string(), /*host=*/true), std::string(), err);
    }
    return install(std::move(next), nowMs(), err);
}

// =====================================================================================
// onTrigger（一次调用 + 预算判据）
// =====================================================================================
bool TriggerRunner::invokeOnTrigger(Context& ctx, const TriggerEvent& ev, std::uint64_t now_ms,
                                    std::string* err) {
    const std::uint64_t start_ns = nowNs();
    TriggerHandler::TriggerSet next = handler_->onTrigger(ctx, ev, err);
    const std::uint64_t end_ns = nowNs();
    // ★ 同 `begin`：2 ms 判据在 handler 侧（用户 JS 本体）；这里只留 10× 的宿主安全网。
    const Budget host_net{hostSafetyNetNs(budget_), budget_.op_limit};
    if (budgetExceeded(host_net, start_ns, end_ns)) {
        return failRun(callbackBudgetReason("onTrigger", ev.name, /*host=*/true), ev.name, err);
    }
    return install(std::move(next), now_ms, err);
}

// =====================================================================================
// atPoint：一个读点最多一个事件（fire 优先于 timeout）
// =====================================================================================
TriggerRunner::Result TriggerRunner::atPoint(const TriggerReadings& now, std::uint64_t now_ms,
                                             Context& ctx, TriggerEvent* ev, std::string* err) {
    if (done_) return Result::Done;     // done 之后不再 poll（合同 §4.2 413）
    if (failed_) return Result::Failed;
    if (armed_.empty()) return Result::None;
    if (handler_ == nullptr) {
        // 到不了（armed 非空 ⇒ 一定 begin 过）；响亮点，⛔ 不要静默当 None。
        failRun("atPoint without a handler", std::string(), err);
        return Result::Failed;
    }

    // ---- ① fire（**注册顺序**；一读点最多一个）----
    // ★ **合同行为（⚠ 不是 bug）**：命中就在下面 `return Result::Fired` ⇒ 排在它**后面**的 trigger 本拍
    //   **不**参与 `satisfied` ⇒ 它们的 Reenter 边沿 `last_inside` 也**不更新**（只有被扫到的那些才写）。
    //   后果：两个 trigger 同拍都在电平内时，第二个（注册顺序靠后）要**晚一个读点**才触发
    //   （`tests/trigger_runner.cpp` 的 `named-set` 把"B 晚一个读点"写成了断言）。
    //   ⛔ 不要"顺手修"成"先扫完所有槽再 fire"：那会让一读点产生多个事件，而每个回调的返回值是
    //   **完整替换集合** ⇒ 第二次替换覆盖第一次，语义不可定义（合同 §4.2 413–415）。
    for (std::size_t i = 0; i < armed_.size(); ++i) {
        // ★ 拷一份再动 armed_：install 会整组替换 ⇒ 引用/指针会悬垂（state 是共享的，计数照样对）。
        const Trigger fired = armed_[i];
        if (!satisfied(fired, armed_meta_[i].armed_ms, now_ms, now)) continue;

        TriggerEvent event;
        event.what = TriggerEvent::What::Fired;
        event.name = fired.name;
        event.trigger_kind = fired.kind;
        fillFromReadings(&event, now);
        if (fired.state != nullptr) ++fired.state->fires;  // ★ 计数属于对象（跨 arm 续计）
        event.trigger = fired;                             // state 共享 ⇒ 与 runner 同源
        if (ev != nullptr) *ev = event;

        // ★ **一读点最多一个事件**：命中即 fire 并 return（⛔ 不许继续扫成同点级联）。
        if (!invokeOnTrigger(ctx, event, now_ms, err)) return Result::Failed;
        return Result::Fired;
    }

    // ---- ② timeout（fire 全部落空之后；一拍最多报**注册顺序的第一个**）----
    for (std::size_t i = 0; i < armed_.size(); ++i) {
        if (armed_meta_[i].deadline_ms == 0 || now_ms < armed_meta_[i].deadline_ms) continue;

        const Trigger timed_out = armed_[i];
        TriggerEvent event;
        event.what = TriggerEvent::What::TimedOut;
        event.name = timed_out.name;
        event.trigger_kind = timed_out.kind;
        fillFromReadings(&event, now);
        event.trigger = timed_out;  // ⛔ **不** ++fires：timeout 不是 fire

        // 先从 armed 移除（"超时 ⇒ 从集合移除"），再回调；回调返回的集合再整体替换。
        armed_.erase(armed_.begin() + static_cast<std::ptrdiff_t>(i));
        armed_meta_.erase(armed_meta_.begin() + static_cast<std::ptrdiff_t>(i));
        if (ev != nullptr) *ev = event;
        if (!invokeOnTrigger(ctx, event, now_ms, err)) return Result::Failed;
        return Result::TimedOut;
    }

    return Result::None;
}

// =====================================================================================
// releaseOwner：只抹该 owner 的 armed 槽（intent / 按住的键是另一侧的账）
// =====================================================================================
std::size_t TriggerRunner::releaseOwner(std::uint32_t owner) {
    std::size_t released = 0;
    for (std::size_t i = 0; i < armed_.size();) {
        if (armed_[i].owner != owner) {
            ++i;
            continue;
        }
        armed_.erase(armed_.begin() + static_cast<std::ptrdiff_t>(i));
        armed_meta_.erase(armed_meta_.begin() + static_cast<std::ptrdiff_t>(i));
        ++released;
    }
    return released;
}

}  // namespace lift::plugin
