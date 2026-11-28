// emulator-lift · **`IntentExecutor`** —— 插件侧 Input 的唯一 pump
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.3（IntentQueue + Executor：click 两段 / 一读点
//       一条 guest 写 / Input 下一读点 / owner）、§4.4（`read → edit → input` 同点组合）、§5 C1 判据。
//
// ★ **六步顺序写死**（`atPoint` 的唯一形状；⛔ 不许增删步骤）：
//     ① 写预算清零（+ 读点计数）
//     ② `queue.beginReadPoint()`：上一读点提交的 Input 在这一刻才可被取
//     ③ `releaseDue`（到期的抬起；占写预算）
//     ④ `pressDue`（到期的按下；占写预算）
//     ⑤ `drainQueue`（本读点**最多 1 条** intent）
//     ⑥ `closeSessionIfDone`（三队列全空 ⇒ `input::releaseProgram`）
//
// ★ **② 必须在 ③ 之前**：否则本读点回调里 `ctx.input.*` 会在**同一个读点**就落地 —— 那正是
//   合同 §4.3 483–485 禁止的"回调的 Input 与回调同拍"。压在 ③ 之前 ⇒ 新提交的进 `staged_`，
//   本读点看不见（下一读点才提升）。
//
// ★ **早退条件必须含"队列 / 指针"**（本类没有 trigger 状态）：放在 ② 之后 ⇒ "每读点恰好一次
//   `beginReadPoint()`"字面成立。队列深度用 `queue_.depth()`（含两级）而**不是** `readyDepth()`。
//
// ★ **钟 = 宿主墙钟**（`pluginNowNs() / 1e6`），⛔ **不接** `Clock` / `attachClock` /
//   `pausedTotalMs()`：这里是墙钟，扣暂停是控制层输入队列（`ControlInput`）的口径 —— 两者**故意不同**。
//
// ★ **两条 `PointerCore` 语义照抄、不重写**：click 的两段 + 三个闸门（`armClickAt` / `pressDue` /
//   `releaseDue`）、"同按钮未抬起 ⇒ 停队首"（连点串行化）。⇒ 本文件**没有**第二套坐标映射 / 闸门。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "runtime/intent.h"
#include "runtime/intent_queue.h"
#include "runtime/pointer_core.h"  // PointerCore / PointerPoint / kInputPointCount

namespace lift {

class WindowManager;

}  // namespace lift

namespace lift::plugin {

class IntentExecutor : public PointerCore {
public:
    using NowFn = std::uint64_t (*)();  // 宿主**墙钟**毫秒（nullptr = `pluginNowNs()/1e6`）

    explicit IntentExecutor(IntentQueue& queue);

    // ★ 测试缝：注入墙钟（⛔ 生产路径用它读 `pluginNowNs()`）。
    void setClockForTest(NowFn now_ms) { now_fn_ = now_ms; }

    // ★ **帧线**：与 `ControlInput::setFrameSource` 是**同一个计数源** —— 宿主**已完成**的
    //   guest 回调数。供给之后 `PointerPoint::callback_boundary=true` ⇒ 抬起还必须在
    //   **按下之后的下一个完整 guest 回调**之后（阈值只有 `PointerCore::kCallbackFramesBeforeRelease`）。
    //   缺省 `fn == nullptr` ⇒ `frames=0` / `callback_boundary=false` = 纯夹具 / 没有 presenter。
    using FrameCountFn = std::uint64_t (*)(void* ctx);
    void setFrameSource(void* ctx, FrameCountFn fn) {
        frame_ctx_ = ctx;
        frame_fn_ = fn;
    }

    // ★ **唯一 pump**（每读点一次；六步顺序见头注）。
    void atPoint(WindowManager& wm, InputPoint point);

    // ★ 释放一个 owner 的**全部**未完成的输入状态：按住的（真的抬 up）/ 臂上的 / 队列里未落地的。
    std::size_t releaseOwner(WindowManager& wm, std::uint32_t owner);

    // ---- 读数（用例 / trace 用；⛔ 不是性能判据）----
    std::uint64_t deliverCalls(InputPoint point) const;
    std::uint64_t readPoints() const { return read_points_; }

private:
    std::uint64_t nowMs() const;
    // ⑤ 本读点最多落地**一条** intent（写预算；队首头阻见头注）。
    void drainQueue(WindowManager& wm, const PointerPoint& point);
    // ⑥ 三队列全空 ⇒ 交还 Program 电平（一次）。
    void closeSessionIfDone(WindowManager& wm);

    IntentQueue& queue_;
    // ★ 读点计数是**本类**的账。
    std::array<std::uint64_t, kInputPointCount> deliver_calls_{};
    std::uint64_t read_points_ = 0;
    NowFn now_fn_ = nullptr;  // 测试缝（nullptr = `pluginNowNs()/1e6`）
    // ★ **帧线**（缺省 nullptr ⇒ `frames=0` / `callback_boundary=false`）。
    void* frame_ctx_ = nullptr;
    FrameCountFn frame_fn_ = nullptr;
};

}  // namespace lift::plugin
