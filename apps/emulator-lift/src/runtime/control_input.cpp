// emulator-lift · 控制层输入队列 —— 口径见 `control_input.h` 的头注。
//
// ⛔ 本文件**不是**第二份输入状态：状态只有 `WindowManager` 一份。这里只有"到点了没有"的推进 +
//   计数 + 待抬起队列；施加输入的**每一处**都走 `runtime/input.h` 既有的 `input::*`（与 SDL /
//   旧脚本同一条路径）。
#include "runtime/control_input.h"

#include <chrono>
#include <cstddef>
#include <cstdint>

#include "runtime/clock.h"       // Clock（暂停补偿的唯一读数：pausedTotalMs）
#include "runtime/host_window.h"  // WindowManager
#include "runtime/input.h"       // input::* （唯一施加路径）

namespace lift {
namespace {

// 宿主单调时钟（与 `present.cpp` 的 `nowNs()` 同源；⛔ 不是 `clock.h` 的 guest 时钟 ——
// 那是 `steady_clock` 的墙钟读数，两者只在"单调"这一点上一致）。
std::uint64_t hostNowMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

}  // namespace

void ControlInput::attachClock(const Clock* clock) { clock_ = clock; }

std::uint64_t ControlInput::nowMs() const {
    const std::uint64_t raw = hostNowMs();
    if (clock_ == nullptr) return raw;  // 缺省档：逐位不变（没有控制层就没有暂停）
    // ★ 暂停区间**不计入**守卫钟（裁决：`body-m0.md` 末节）。
    const std::uint64_t paused = clock_->pausedTotalMs();
    return raw > paused ? raw - paused : 0;
}

void ControlInput::atPoint(WindowManager& wm, InputPoint point) {
    const std::size_t i = static_cast<std::size_t>(point);
    if (i < kInputPointCount) ++deliver_calls_[i];
    ++read_points_;
    // ★ S7 / Q2：**本读点的写预算**（下面三段都在写过一条之后停）。
    writes_this_point_ = 0;
    // ★ 闸门 ③ 的刻度 = "**本读点之前**完成过几个 CursorPos 读点"：本读点自己的读还没发生，
    //   所以若是位置读点，得把刚 +1 的那个数扣掉（否则"move 与 down 同读点"又能满足它）。
    cursor_reads_done_ = deliverCalls(InputPoint::CursorPos);
    if (point == InputPoint::CursorPos && cursor_reads_done_ > 0) --cursor_reads_done_;
    // ★ 早退条件多了 **armed_ 非空** 这一项：光标已落地、还差一拍才按下的那些也是"有待办"。
    if (pending_.empty() && armed_.empty() && immediate_.empty()) return;
    // ★ **本读点的读数**（core 不持有钟 / 读点计数 / 投递计数 ⇒ 值参数传进去）。
    //   一次读点只取一次钟（与 C1a 的口径一致）。
    const std::uint64_t frames = frame_fn_ != nullptr ? frame_fn_(frame_ctx_) : 0;
    const PointerPoint pt{point,
                          nowMs(),
                          deliverCalls(InputPoint::KeyLevel),
                          cursor_reads_done_,
                          read_points_,
                          0,
                          frames,
                          callback_boundary_};
    // ① 到期的**抬起**先落地（同一次读点内、读之前）。
    releaseDue(wm, pt);
    // ② 再把到期的 `armed_` **按下**（阶段 ②）。
    //   ⛔ 顺序不许反：先按下再抬起会让"新的一次 click + 上一次的抬起"在同一个读点里发生
    //   ⇒ guest 只看到 up（第二次按下被吞掉）。
    pressDue(wm, pt);
    // ③ **立即队列**在这里落地：走**同一段** `armClickAt` / `input::mouseMove`、计入 `fired_`。
    drainImmediate(wm, pt);
}

bool ControlInput::enqueueClick(int ref_x, int ref_y, MouseButton button) {
    // ★ **有界**：满了就返回 false，由控制层回 `bad_request`。⛔ 不丢旧的、⛔ 不静默扩容器。
    //   ⚠ 这里**不做映射**：参考坐标原样排队，映射只在落地那一刻由 `armClickAt` 做**一次**
    //   （否则"排队时的客户端尺寸"与"落地时的客户端尺寸"会变成两套口径）。
    if (immediate_.size() >= kImmediateClickCapacity) return false;
    ImmediateOp c;
    c.is_move = false;
    c.x = ref_x;
    c.y = ref_y;
    c.button = button;
    immediate_.push_back(c);
    session_closed_ = false;  // 新的注入工作 ⇒ 所有权要重新握在脚本手里
    return true;
}

bool ControlInput::enqueueMove(int ref_x, int ref_y) {
    // ★ 纯 move：与 click **同一条**立即队列 / 同一个上界 / 同一个落点（下一个 guest 读点），
    //   只走 `input::mouseMove`（⛔ 不碰按钮电平、⛔ 不进 armed_）。
    if (immediate_.size() >= kImmediateClickCapacity) return false;
    ImmediateOp m;
    m.is_move = true;
    m.x = ref_x;
    m.y = ref_y;
    immediate_.push_back(m);
    session_closed_ = false;
    return true;
}

void ControlInput::drainImmediate(WindowManager& wm, const PointerPoint& pt) {
    // ★ 只消费立即队列。每条 click +1 `fired_`（与配方 click 同一读数口径）；move 不是"动作"
    //   ⇒ 不计 `fired_`（它没有按下 / 抬起，⛔ 不冒充一次点击）。
    // ★ **S7 / Q2**：一次读点最多落**一条**写动作 ⇒ 剩下的**留在队首**（`queuedClicks()` 非 0）；
    //   同一按钮还没抬起（`pending_`）⇒ 停在队首（连点串行化），⛔ 不在这里强制抬起。
    while (!immediate_.empty()) {
        if (writes_this_point_ != 0) return;  // 本读点的写预算已经用完 ⇒ 其余留队
        const ImmediateOp c = immediate_.front();
        if (!c.is_move && buttonPending(c.button)) return;  // 队首的 click 等这一次抬起
        immediate_.erase(immediate_.begin());
        if (c.is_move) {
            armMoveAt(wm, c.x, c.y, pt);
        } else {
            armClickAt(wm, c.x, c.y, c.button, pt);
            ++fired_;
        }
        writes_this_point_ = 1;
    }
}

std::size_t ControlInput::releaseAll(WindowManager& wm) { return releaseAllPending(wm); }

std::uint64_t ControlInput::deliverCalls(InputPoint point) const {
    const std::size_t i = static_cast<std::size_t>(point);
    return i < kInputPointCount ? deliver_calls_[i] : 0;
}

}  // namespace lift
