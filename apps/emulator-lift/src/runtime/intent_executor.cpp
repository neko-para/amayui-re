// emulator-lift · `IntentExecutor` 的实现 —— 口径见 `intent_executor.h` 的头注（六步顺序 / 早退）。
//
// ★ 施加输入**每一处**都走 `PointerCore`（`armMoveAt` / `armClickAt` / `pressDue` / `releaseDue`），
//   而 core 的每一处又都走 `runtime/input.h` 的既有 `input::*`。⇒ 本文件**没有**第二套坐标映射、
//   第二套闸门、第二个 `input::` 调用点。
// ★ 本文件也**没有**配方状态的任何痕迹（`tools/test/core-shape.test.mjs` 的禁用词）。
#include "runtime/intent_executor.h"

#include <cstddef>
#include <cstdint>

#include "runtime/budget.h"       // pluginNowNs（宿主单调钟；⛔ 不是 guest 虚拟时钟）
#include "runtime/host_window.h"  // WindowManager（施加输入要它）
#include "runtime/input.h"        // input::releaseProgram（会话收口）

namespace lift::plugin {

IntentExecutor::IntentExecutor(IntentQueue& queue) : queue_(queue) {}

std::uint64_t IntentExecutor::nowMs() const {
    if (now_fn_ != nullptr) return now_fn_();
    // ★ 宿主单调毫秒（`pluginNowNs`），⛔ 不扣暂停。
    return pluginNowNs() / 1'000'000ull;
}

std::uint64_t IntentExecutor::deliverCalls(InputPoint point) const {
    const std::size_t i = static_cast<std::size_t>(point);
    return i < deliver_calls_.size() ? deliver_calls_[i] : 0;
}

void IntentExecutor::atPoint(WindowManager& wm, InputPoint point) {
    // ---- ① 读数 + 写预算清零 ----
    const std::size_t i = static_cast<std::size_t>(point);
    if (i < deliver_calls_.size()) ++deliver_calls_[i];
    ++read_points_;
    writes_this_point_ = 0;
    // ★ 闸门③的刻度 = "**本读点之前**完成过几个 CursorPos 读点"。
    std::uint64_t cursor_done = deliverCalls(InputPoint::CursorPos);
    if (point == InputPoint::CursorPos && cursor_done > 0) --cursor_done;

    // ---- ② staged → ready（**每读点恰好一次**）----
    queue_.beginReadPoint();

    // ---- 早退（放在 ② **之后**：这样"每读点恰好一次 beginReadPoint"是字面成立的）----
    if (queue_.depth() == 0 && pending_.empty() && armed_.empty()) return;

    const std::uint64_t now = nowMs();
    // ★ **帧线**：与控制层输入队列同一条计数（宿主**已完成**的 guest 回调数）。
    const std::uint64_t frames = frame_fn_ != nullptr ? frame_fn_(frame_ctx_) : 0;
    const PointerPoint pt{point, now, deliverCalls(InputPoint::KeyLevel), cursor_done, read_points_, 0,
                          frames, frame_fn_ != nullptr};

    // ---- ③ 到期的抬起（闸门① / 墙钟兜底）；④ 到期的按下（闸门③）----
    //   ⛔ 顺序不许反：先按下再抬起会让"新的一次 click + 上一次的抬起"在同一读点发生。
    releaseDue(wm, pt);
    pressDue(wm, pt);

    // ---- ⑤ 队列落地（本读点最多 1 条 guest 写；Edit 不走这里）----
    drainQueue(wm, pt);

    // ---- ⑥ 会话收口（**末尾**：本读点的 up 已入队，guest 下个读点能取到）----
    closeSessionIfDone(wm);
}

void IntentExecutor::drainQueue(WindowManager& wm, const PointerPoint& point) {
    // ★ 一次读点最多落**一条**写动作：预算被 ③/④ 先占 ⇒ 本拍一条也不落。
    if (writesThisPoint() != 0) return;
    Intent intent;
    if (!queue_.peekFront(&intent)) return;
    if (intent.cls != IntentClass::GuestWrite) return;

    switch (intent.kind) {
        case IntentKind::Move: {
            queue_.popFront();
            armMoveAt(wm, intent.x, intent.y, point);
            break;
        }
        case IntentKind::Click: {
            // ★ 连点串行化：这个按钮还按着 ⇒ **停在队首**，整条队列都等它（形状，不是 bug）。
            if (buttonPending(intent.button)) return;
            queue_.popFront();
            armClickAt(wm, intent.x, intent.y, intent.button, point, intent.owner);
            break;
        }
        case IntentKind::MouseDown:
        case IntentKind::MouseUp:
            // ★ 本层**不施加**显式 down/up。停住队首而**不是**丢掉。
            return;
    }
    writes_this_point_ = 1;
    queue_.markApplied(intent.id);
}

void IntentExecutor::closeSessionIfDone(WindowManager& wm) {
    if (session_closed_) return;
    if (!pending_.empty() || !armed_.empty()) return;
    if (queue_.depth() != 0) return;  // 含 staged（否则刚提交的 intent 会被"收口"抢在前面）
    input::releaseProgram(wm, kPointerMessageTimeMs);
    session_closed_ = true;
}

std::size_t IntentExecutor::releaseOwner(WindowManager& wm, std::uint32_t owner) {
    // 两个地方各有一份"该 owner 还没完成的事"：指针状态 + 队列里未落地的。
    const std::size_t released = PointerCore::releaseOwner(wm, owner);
    queue_.dropOwner(owner);
    return released;
}

}  // namespace lift::plugin
