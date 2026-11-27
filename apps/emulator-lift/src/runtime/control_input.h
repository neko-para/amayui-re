// emulator-lift · 控制层输入队列（暂停态 `click` / `move` 的唯一落地面）
//
// ★ 来历（2026-11-27 用户裁决 `REQ-01M875NDPPNN68VCE3VMCT1`）：本类是旧 `InputPlan`
//   （配方驱动器，`--input-plan` / `.plan`）里**控制层立即队列**那一半的搬迁。
//   配方 / 门 / 完成度 / `wait` / `profile_*` / `quit` / trace / fault 摘要**全部退场**；
//   留下的是控制层真正需要的那一块：
//     · `PointerCore`（click 两段 / 三个闸门 / 写预算 / 待抬起队列 / 连点串行化）—— 共享实现，
//       插件 `IntentExecutor` 用的是同一份（⛔ 这里不复制第二套坐标映射 / 闸门）；
//     · 立即队列（`enqueueClick` / `enqueueMove`），施加点 = **guest 读点**（`atPoint`），
//       ⛔ 控制线程 / `Control::serve()` 都不碰 guest 状态。
//   ⛔ 不要在这里恢复配方语义（回潮入口 = `tools/test/input-plan-shape.test.mjs` ①）。
//
// ★ 三条不变量（改之前先读一遍）：
//   1. **本类不拥有任何 guest 可见状态**。唯一状态源仍是 `WindowManager`（`host_window.h` 的
//      `InputSource` 段）；施加输入只经 `runtime/input.h` 既有的 `input::*`（与 SDL 真人输入
//      **同一段代码**）。⇒ 不存在"注入只发消息、电平没动"的第二份状态。
//   2. **本类不在执行循环里被调用**。允许的调用点只有 guest 自己的输入读点（`input.h` 的四个
//      投递入口）。⛔ `src/runtime/machine.cpp` / `src/aot/aot_run.cpp` 里出现
//      `input::` / `deliver(` / `atPoint(` 就是回归（守卫 `tools/test/input-plan-shape.test.mjs` ①）。
//   3. **一个读点最多一条写动作**（S7 / Q2）：`releaseDue` → `pressDue` → `drainImmediate`
//      三段都尊重 `writes_this_point_`；同一按钮还没抬起 ⇒ 队首的 click **不消费**
//      （连点串行化 ⇒ guest 能看到每一条 down / up 的 DUDU 顺序）。
//
// ★ 与旧 `InputPlan` 的**行为差**只有一处，且是裁决要求的：控制会话**不按配方收口**
//   （旧 `closeSessionIfDone` 在队列全空后调 `input::releaseProgram` 把租约交还 OS；现在
//   会话的所有权保持到 `aot_main` 退出时的 `releaseAll`）。click / move 的三段时序、三个闸门、
//   写预算、墙钟兜底**逐位不变**。
//
// ★ 时钟：缺省 = `std::chrono::steady_clock`；控制层装配点 `attachClock(&host.clock())`
//   ⇒ 守卫钟 = 宿主单调毫秒 − 累计暂停毫秒（暂停不该把兜底窗口走掉）。
//
// ★ 帧线（回调边界 ②′）：装配点用 `setFrameSource(&host, &presentFrameCount)` 把
//   `presenter.frameCount()`（Present 完成计数）接进来。⛔ 不再向 presenter **推** `note(Frame)`：
//   那是旧 plan 的记账口；现在是拉取，presenter 不依赖输入层。没有帧线 ⇒ `frames=0` /
//   `callback_boundary=false`（纯夹具的旧行为）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime/input_point.h"   // 11 个投递点的闭集合
#include "runtime/pointer_core.h"  // PointerCore（click 两段 / 三个闸门 / 写预算的唯一实现）

namespace lift {

class WindowManager;
class Clock;

class ControlInput : public PointerCore {
public:
    // ★ **立即队列**（控制层 `click` / `move` 的唯一落点；M1）
    //   · 生产者 = 控制层（**主线程**的 `Control::serve()`，只在暂停态）；
    //   · 消费者 = 本层：下一个 guest 读点（`atPoint`）；
    //   · **有界**：满了 `enqueueClick` / `enqueueMove` 返回 false ⇒ 控制层回 `bad_request`
    //     （⛔ 静默丢）。
    //   ★ 没有控制层时这条队列**恒空** ⇒ `atPoint` 的早退条件与默认档逐字节不变。
    static constexpr std::size_t kImmediateClickCapacity = 64;
    // `ref_x` / `ref_y` = **1280×720 参考坐标**（与旧配方 `click x y` 同一口径；已由控制层校验）。
    // 返回 false = 队列已满（⛔ 不排队、不丢旧的）。
    bool enqueueClick(int ref_x, int ref_y, MouseButton button);
    // ★ **纯 move**（不按键）：控制层的 `move <x> <y>`（`tools/emu-ctl.mjs move`）。
    //   走**同一段** `input::mouseMove`（⛔ 不复制坐标映射 / 不复制施加路径），同样在下一个读点落地。
    bool enqueueMove(int ref_x, int ref_y);
    // 立即队列深度 = 还没在 guest 读点上落地的 click + move 条数（回执的 `queued`）。
    std::size_t queuedClicks() const { return immediate_.size(); }

    // ★ **唯一**能施加输入动作的入口（也只经 `runtime/input.h` 的四个投递入口到达）。
    //   flush-before-read：先把到期的动作落地到 `WindowManager`，**再**让 shim 去读。
    //   ★ 同一个读点内的顺序：待抬起 → 到期的按下 → 立即队列。
    //   ★ **一个读点最多一条写动作**（见 `PointerCore` 的写预算）。
    void atPoint(WindowManager& wm, InputPoint point);

    // ★ **回调边界**（装配点与 `pointer_core.h` 的闸门 ②′ 同一处）：
    //   供给帧线时，抬起还必须在**按下之后的下一个完整 guest 回调**之后才允许。
    //   ⛔ 缺省 false = 没有帧线（纯夹具）⇒ 保持旧行为（只有 ①/②）。
    void requireCallbackBoundary(bool required = true) { callback_boundary_ = required; }

    // ★ **帧线**（2026-11-22 的形状，照 `IntentExecutor::setFrameSource`）：
    //   拉取 `presenter.frameCount()`（Present 完成计数）⇒ `PointerPoint::frames` 是活的、
    //   `callback_boundary=true`。缺省 `fn == nullptr` ⇒ `frames=0` / 不判断②′。
    using FrameCountFn = std::uint64_t (*)(void* ctx);
    void setFrameSource(void* ctx, FrameCountFn fn) {
        frame_ctx_ = ctx;
        frame_fn_ = fn;
    }

    // ★ 守卫钟的暂停补偿：`clock` = 宿主那份 `Clock`（`pausedTotalMs()` 是唯一读数）；
    //   nullptr = 不减（缺省档逐位不变）。⛔ 不在这里另存一份暂停账。
    void attachClock(const Clock* clock);

    // ★ 收尾：把**所有**待抬起的按钮抬起 + 把 Program 的电平交还 OS（⚠ 不是 step）。
    //   调用点只有一次性的收口（`aot_main` 跑完之后）。返回抬起个数。
    std::size_t releaseAll(WindowManager& wm);

    // 读数（守卫 / 诊断用；⛔ 不是性能判据）。
    std::uint64_t deliverCalls(InputPoint point) const;
    std::uint64_t readPoints() const { return read_points_; }

private:
    // 立即队列的一条（click 或 move）：**1280×720 参考坐标**（⛔ 还没映射 —— 映射只发生在
    // 落地那一刻的那一次）。
    struct ImmediateOp {
        bool is_move = false;
        int x = 0;
        int y = 0;
        MouseButton button = MouseButton::Left;
    };

    std::uint64_t nowMs() const;
    // 立即队列消费（同一次读点内、待抬起 / 按下之后；⛔ 不碰写预算以外的账）。
    void drainImmediate(WindowManager& wm, const PointerPoint& pt);

    // 控制层投进来的 click / move（**唯一**生产者；有界，见 `kImmediateClickCapacity`）。
    std::vector<ImmediateOp> immediate_;
    const Clock* clock_ = nullptr;  // 暂停补偿（nullptr = 不减；见 attachClock）
    // ★ `fired_` / `released_` / `ui_consumed_` / `cursor_reads_done_` / `writes_this_point_` /
    //   `last_move_*` / `releases_pending_` / `session_closed_` / `pending_` / `armed_` 是
    //   `PointerCore` 的成员（`runtime/pointer_core.h`；本类经继承读写同一份）。
    std::uint64_t read_points_ = 0;  // 读点序号（`PointerPoint::read_index`）
    // ★ **回调边界**（见 `requireCallbackBoundary`）。
    bool callback_boundary_ = false;
    // ★ 帧线（见 `setFrameSource`；缺省 nullptr ⇒ 每个读点一次判空）。
    void* frame_ctx_ = nullptr;
    FrameCountFn frame_fn_ = nullptr;
    std::uint64_t deliver_calls_[kInputPointCount] = {};
};

}  // namespace lift
