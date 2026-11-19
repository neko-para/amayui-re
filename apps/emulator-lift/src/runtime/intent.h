// emulator-lift · 插件运行时的 **Intent 值类型 + 回执**（C1c）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.3（IntentQueue + Executor：click 两段 / 一读点
//       一条 guest 写 / 回执 / owner）、§4.4a（`EditSink` 是 Edit，**不是** Input）、
//       §4.5（`Context::input` 只入队）。
// 施工图：`.tmp/plugin-runtime/c1-design-review.md` §4.1（staged 与写预算是两件东西）/
//         §4.2（click 两段为什么不会互相饿死 / 队首头阻）/ §5.4（本头就是那一段的落地）。
//
// ★ **`Intent` 是纯值**：没有 `WindowManager` / `GuestMemory` / 时间戳 / 回调。
//   它只说"要做什么"；"什么时候能做"是 `IntentQueue`（staged/ready）与 `PointerCore`（写预算）的事。
//   ⛔ 不要往这里塞 `std::function` / 目标窗口 / 已映射的客户区坐标：坐标一律是 **1280×720 参考系**，
//   映射只在落地那一刻由 `PointerCore::armClickAt / armMoveAt` 做**一次**（与配方 `click x y` 同一口径）。
//
// ★ **`IntentClass` 本切片只有 `GuestWrite`**：`HostEffect`（screenshot / profile / clipboard）是 C4
//   的 HostAction，⛔ 不在 C1 的闭集合里（合同 §4.3 / §8）。多一个空枚举值 = 一个没人实现的分支。
//
// ★ **`IntentKind` 的四个值 = 形状，不是四个实现**：`Move` / `Click` 由 `IntentExecutor::drainQueue`
//   在 C1 施加；`MouseDown` / `MouseUp` 的**施加**属于 C4（`input.h` 的四投递口 / `mouse_down` 的
//   轻闸门都在那边）⇒ C1 的 `drainQueue` 遇到它们**停在队首**（⛔ 不静默丢：丢掉会给"Applied"的
//   回执而 guest 一点变化都没有 —— 那是假成功）。`Key` / `Wheel` / `Profile*` 连枚举值都不建。
//
// ★ **`Receipt::code` 的单一词表**：成功 = `nullptr`；失败 = 已有的字符串常量（队列满 =
//   `"queue_full"`；Edit = `globalErrorName(err)` 的输出）⇒ ⛔ 不新造 `"encode_failed"` 之类的第二份。
#pragma once

#include <cstdint>

#include "runtime/input_device.h"  // MouseButton（不含窗口 / 不引 host_window ⇒ 不成环）

namespace lift::plugin {

// 要施加的**动作种类**（guest 写）。C4 追加 `Key` / `Wheel` / `ProfileBegin` / `ProfileEnd`。
enum class IntentKind : std::uint8_t {
    Move = 0,   // 纯移动光标（不碰按钮电平）
    Click,      // 两段：臂上（move）→ 后续读点按下 → 闸门①/兜底②之后抬起
    MouseDown,  // C4：显式按住（本切片只保留形状，见头注）
    MouseUp,    // C4：显式松开
};

// 动作的**类**：C1 只有"写 guest"。`HostEffect` = C4 的 HostAction（⛔ 不进写预算、⛔ 不进本队列）。
enum class IntentClass : std::uint8_t {
    GuestWrite = 0,
};

// 谁提的。只用于 trace / 归因（C1 不按 origin 分支）。
enum class Origin : std::uint8_t {
    QjsControl = 0,   // 插件回调（`ctx.input.*`）
    RecipeFrontend,   // C3 的 JS recipe driver
    Control,          // 控制面（`emu-ctl`；C4 接线）
};

// 一条 intent（纯值；⛔ 无宿主句柄、无时间戳）。
struct Intent {
    IntentKind kind = IntentKind::Move;
    IntentClass cls = IntentClass::GuestWrite;
    Origin origin = Origin::Control;
    // 提交者。`releaseOwner` / `dropOwner` 只认这一个数（C1 单 owner；C5 多 owner）。
    std::uint32_t owner = 0;
    // 队列分配的 id（`submit` 里填；调用方那份被忽略 —— ⛔ 不许自己编）。
    std::uint64_t id = 0;
    // **1280×720 参考坐标**（与配方 `click x y` 同一口径；映射在落地时做一次）。
    int x = 0;
    int y = 0;
    MouseButton button = MouseButton::Left;
};

// 一次提交的回执（合同 §4.3 475）。`accepted` / `queued` 在 `submit` 里**立即**给出；
// `id` 用 `IntentQueue::status(id)` 追落地结果（`std::move` 之后仍可查）。
struct Receipt {
    bool accepted = false;
    const char* code = nullptr;   // nullptr = 成功；失败 → 单一词表里的名字（见头注）
    std::uint64_t seq = 0;        // submit 的单调序号（trace / 对账；⛔ 不是性能读数）
    std::uint64_t queued = 0;     // 回执那一刻的深度（**含**本条所在的那一级）
    std::uint64_t id = 0;         // 本条 intent 的 id（0 = 没分配，即被拒）
};

}  // namespace lift::plugin
