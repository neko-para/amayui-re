// emulator-lift · `TriggerFactory` 的实现 —— 口径见 `trigger.h` 的头注。
//
// ★ 每次调用都造一个**新的** `TriggerState`：这是"新对象 = 新预算 / 新边沿"的唯一落点
//   （合同 §4.2 416–419、425）。⛔ 不要在工厂里缓存 / 复用 state（那会让两个"新对象"共享计数，
//   而"同一对象"与"新对象"就再也分不开了）。
// ★ 工厂**只造值**、不校验合法性：`name` 唯一 / `AtReadPoint.max_fires > 0` / 耗尽 这些校验
//   一律在 `TriggerRunner::install` 里做（一处判据，⛔ 不要两头各判一次）。
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "runtime/trigger.h"

namespace lift::plugin {
namespace {

// 新对象 = 新的运行期状态（`fires=0`、`last_inside=false`）+ 工厂的 owner 初值。
// ★ owner 只是初值：install 时由 runner 盖章覆盖。
Trigger makeTrigger() {
    Trigger trigger;
    trigger.state = std::make_shared<TriggerState>();
    return trigger;
}

}  // namespace

Trigger TriggerFactory::afterScript(std::string name, std::string script, TriggerMode mode,
                                    std::uint32_t max_fires, std::uint64_t timeout_ms) {
    Trigger trigger = makeTrigger();
    trigger.name = std::move(name);
    trigger.kind = TriggerKind::AfterScript;
    trigger.mode = mode;
    trigger.script = std::move(script);
    trigger.max_fires = max_fires;
    trigger.timeout_ms = timeout_ms;
    trigger.owner = owner_;
    return trigger;
}

Trigger TriggerFactory::afterPosition(std::string name, std::string script, std::uint32_t lo,
                                      std::uint32_t hi, TriggerMode mode, std::uint32_t max_fires,
                                      std::uint64_t timeout_ms) {
    Trigger trigger = makeTrigger();
    trigger.name = std::move(name);
    trigger.kind = TriggerKind::AfterPosition;
    trigger.mode = mode;
    trigger.script = std::move(script);
    trigger.lo = lo;
    trigger.hi = hi;
    trigger.max_fires = max_fires;
    trigger.timeout_ms = timeout_ms;
    trigger.owner = owner_;
    return trigger;
}

Trigger TriggerFactory::afterDelay(std::string name, std::uint64_t ms, TriggerMode mode) {
    Trigger trigger = makeTrigger();
    trigger.name = std::move(name);
    trigger.kind = TriggerKind::AfterDelay;
    trigger.mode = mode;
    trigger.delay_ms = ms;
    // ⛔ 没有 `timeout_ms` 参数（合同 §4.2 441：延迟本身就是时间量）。
    trigger.timeout_ms = 0;
    trigger.owner = owner_;
    return trigger;
}

Trigger TriggerFactory::atReadPoint(std::string name, InputPoint filter, std::uint32_t max_fires,
                                    TriggerMode mode) {
    Trigger trigger = makeTrigger();
    trigger.name = std::move(name);
    trigger.kind = TriggerKind::AtReadPoint;
    trigger.mode = mode;
    trigger.point_filter = filter;
    trigger.max_fires = max_fires;  // 0 会被 install 拒掉（必填 > 0）
    trigger.owner = owner_;
    return trigger;
}

}  // namespace lift::plugin
