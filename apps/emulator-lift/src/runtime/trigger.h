// emulator-lift · 插件运行时的 **trigger 值类型 + handler 接口 + 工厂**（C1b）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.2（具名 armed 集合；一读点最多一个）、
//       §4.5（`Context::now` 的读数形状）、§5 C1（判据）、§6 行 31–33 / 36–37（守卫）。
// 施工图：`.tmp/plugin-runtime/c1-design-review.md` §2.2（四种 kind）/ §3.1–3.2（状态机）/ §5.2（签名）。
//
// ★ 为什么在 `lift::plugin`（⛔ 不是 `lift`）：`lift::TriggerKind` / `lift::Trigger` **已经存在**
//   （`runtime/input_script.h`，那是 `--input-script` 的机器内 actuator）。同名不同义 ⇒ 必须隔离；
//   ⛔ 不要改名那些既有类型（`input_anchor.cpp` / `input_marker.cpp` 都在用）。
//
// ★ **运行期状态挂在对象共享的句柄上**（`TriggerState`），⛔ 不是值字段（这是本切片最容易做错的一处）：
//   `maxFires` 属于**对象**、跨 arm 累计。若把 `fires` 放进 `Trigger` 的值字段，runner 递增的是
//   **它自己那份拷贝**，回调手里那份仍是 0 ⇒ 回调把手里那份返回 = runner 看到 `fires==0` ⇒
//   **静默重置预算**（合同 §4.2 417–419 点名禁止），而且**无法与"新对象"区分**（两者字段完全一样）。
//   ⇒ `Trigger` 仍可拷贝，`state` 指向同一份计数："同一对象" = 同一个 `state`；
//     "新对象" = 工厂新造的 `state`（`fires=0`，= 新预算 / 新边沿）。
//   ⛔ 不要把 `state` 暴露成可变共享：它是**计数 + 边沿**，语义归 `TriggerRunner` 写。
//
// ★ `owner` 由 `TriggerRunner` 在 install 时**盖章**（`t.owner = owner_`）⇒ 回调伪造不了 owner。
//   工厂的 `owner()` 只是"建出来的对象带这个值"，运行期以 runner 的为准。
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "runtime/input_point.h"  // InputPoint（11 个读点的闭集合；C1a 已抽到头）

namespace lift::plugin {

struct Context;  // `context.h` 的**指针聚合**上下文（本头只前向声明 ⇒ 不成环）

// 四种 trigger（`Frame` 留给 C2 的 overlay；合同 §4.2 443、§8 的"不做"清单）。
enum class TriggerKind : std::uint8_t {
    AfterScript = 0,  // 纯电平门：当前脚本名 == `script`（**无区间**）
    AfterPosition,    // `[lo,hi)` 半开区间 + 脚本名；Once 首个有效 inside / Reenter 有效外→内
    AfterDelay,       // 宿主**墙钟**累计 ≥ `delay_ms`（暂停不冻结；NON-DIFFABLE）
    AtReadPoint,      // 下一个匹配读点（`point_filter == Count` = 任意读点）
};

enum class TriggerMode : std::uint8_t {
    Once = 0,     // 触发一次即耗尽（同一对象再放回 ⇒ fail，⛔ 不是静默重置）
    Reenter = 1,  // 边沿触发；重新 arm = 边沿从头（`last_inside` **不**清零 ⇒ 要新的外→内）
};

// 运行期状态（对象级；寿命 = 最后一个拷贝）。`armed_ms` / `deadline_ms` **不在**这里：
// 它们是 per-arm，归 runner 的槽（重新 arm 自动重置，`fires` 不动）。
struct TriggerState {
    std::uint32_t fires = 0;     // ★ 跨 arm 累计（Once 耗尽 / maxFires 耗尽 / 续计的证据）
    bool last_inside = false;    // Reenter 的边沿（有效读数才写；无效读数不改变状态）
};

// 一个 trigger（可拷贝；拷贝 = 同一个运行期对象）。
struct Trigger {
    std::shared_ptr<TriggerState> state;  // ★ 由 `TriggerFactory` 建；拷贝共享 ⇒ 续计
    std::string name;                     // ★ 必填；同一 armed 集合内唯一（重复 ⇒ fail 并点名）
    TriggerKind kind = TriggerKind::AfterScript;
    TriggerMode mode = TriggerMode::Once;
    std::string script;                   // AfterScript / AfterPosition 的脚本名
    std::uint32_t lo = 0;                 // AfterPosition：[lo, hi)
    std::uint32_t hi = 0;
    std::uint64_t delay_ms = 0;           // AfterDelay：延迟本身（宿主墙钟）
    InputPoint point_filter = InputPoint::Count;  // AtReadPoint：Count = 任意
    std::uint32_t max_fires = 0;          // AtReadPoint 必填 > 0；其余 0 = 不设硬上限
    std::uint64_t timeout_ms = 0;         // 0 = 不设超时（⛔ 不是"立即"）；AfterDelay 不暴露
    std::uint32_t owner = 0;              // runner 盖章（工厂那个值只是初值）
};

// 一个读点的**读数快照**（纯值；== `Context::now` 的指向目标）。⛔ 里面没有可变宿主 / guest 句柄。
// ★ `script_ok` / `pos_ok` 是"这条读数**有没有**"：无效读数**不改变状态**（合同 §4.2 438）。
struct TriggerReadings {
    std::string script;                                       // 当前引擎脚本身份名
    bool script_ok = false;
    std::uint32_t pos = 0;                                    // 脚本内位置（marker 体系）
    bool pos_ok = false;
    int mouse_x = 0;
    int mouse_y = 0;
    bool mouse_ok = false;
    InputPoint point = InputPoint::Count;                     // 本读点
    std::uint64_t read_index = 0;                             // 读点序号（trace / 断言读数）
};

// 交给 handler 的那个事件（fire 或 timeout）。`trigger` = 被触发 / 超时的**那个对象**
// （`state` 共享 ⇒ 计数与边沿与 runner 同源）。
struct TriggerEvent {
    enum class What : std::uint8_t {
        Fired = 0,   // 条件成立（`kind()` == "trigger"）
        TimedOut,    // 超时（`kind()` == "timeout"；⛔ **不是** fire，不计 `maxFires`）
    };
    What what = What::Fired;
    const char* kind() const { return what == What::Fired ? "trigger" : "timeout"; }
    std::string name;                                    // 被触发 / 超时的 trigger name（必填）
    TriggerKind trigger_kind = TriggerKind::AfterScript;
    // 本读点的读数（与 `TriggerReadings` 同源；timeout 时是**当前**读数，不是 arm 时的）
    std::string script;
    std::uint32_t pos = 0;
    bool pos_ok = false;
    InputPoint point = InputPoint::Count;
    std::uint64_t read_index = 0;
    Trigger trigger;  // 那个对象本体（state 共享）
};

// 插件侧的唯一回调面（合同 §4.2 398–405）。同步、有预算；可 Edit / EngineRead；input 只入队。
// ★ 返回数组 = **完整的新 armed 集合（替换）**：旧集合里没出现在返回数组里的（**包括刚触发的那个**）
//   全部消失；`[]` = done（⛔ 不是"永远等"）。
class TriggerHandler {
public:
    using TriggerSet = std::vector<Trigger>;
    virtual TriggerSet start(Context& ctx, std::string* err) = 0;
    virtual TriggerSet onTrigger(Context& ctx, const TriggerEvent& ev, std::string* err) = 0;
    virtual ~TriggerHandler() = default;
};

// ★ **工厂**（⛔ 不是 runner）：`Context::trigger` 指向它，所以回调**造得出**新对象、
//   但**拿不到唯一 pump**（否则回调里能递归 `atPoint` / 绕过"返回值替换集合"的语义）。
//   每次调用都造一个新的 `TriggerState` ⇒ "新对象 = 新预算 / 新边沿"。
class TriggerFactory {
public:
    explicit TriggerFactory(std::uint32_t owner = 0) : owner_(owner) {}
    void setOwner(std::uint32_t o) { owner_ = o; }
    std::uint32_t owner() const { return owner_; }

    Trigger afterScript(std::string name, std::string script, TriggerMode mode,
                        std::uint32_t max_fires = 0, std::uint64_t timeout_ms = 0);
    Trigger afterPosition(std::string name, std::string script, std::uint32_t lo, std::uint32_t hi,
                          TriggerMode mode, std::uint32_t max_fires = 0, std::uint64_t timeout_ms = 0);
    // ⛔ `afterDelay` **不接受** `timeout_ms`（合同 §4.2 441：延迟本身就是时间量；要超时用别的 trigger）。
    Trigger afterDelay(std::string name, std::uint64_t ms, TriggerMode mode);
    Trigger atReadPoint(std::string name, InputPoint filter, std::uint32_t max_fires,
                        TriggerMode mode = TriggerMode::Once);

private:
    std::uint32_t owner_ = 0;
};

}  // namespace lift::plugin
