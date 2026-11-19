// emulator-lift · `IntentExecutor` 的实现 —— 口径见 `intent_executor.h` 的头注（七步顺序 / 早退）。
//
// ★ 施加输入**每一处**都走 `PointerCore`（`armMoveAt` / `armClickAt` / `pressDue` / `releaseDue`），
//   而 core 的每一处又都走 `runtime/input.h` 的既有 `input::*`。⇒ 本文件**没有**第二套坐标映射、
//   第二套闸门、第二个 `input::` 调用点（`--input-plan` 与本 executor 共用同一份实现）。
// ★ 本文件也**没有** `InputPlan` / 配方状态的任何痕迹（`tools/test/core-shape.test.mjs` 的禁用词）。
#include "runtime/intent_executor.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include "runtime/budget.h"        // pluginNowNs（宿主单调钟；⛔ 不是 guest 虚拟时钟）
#include "runtime/edit_sink.h"     // EditSink（`ctx.edit` 的指向类型；本文件不用它的实现）
#include "runtime/engine_read.h"   // EngineRead（同上）
#include "runtime/host_window.h"   // WindowManager（施加输入要它）
#include "runtime/input.h"         // input::releaseProgram（会话收口）
#include "runtime/script_globals.h"  // （`EditSink`/`EngineRead` 的词汇在它们的头里；这里不直接用）
#include "runtime/trigger_runner.h"  // TriggerRunner（唯一 pump 的调用点）

namespace lift::plugin {

IntentExecutor::IntentExecutor(IntentQueue& queue, ReadingsSource& source)
    : queue_(queue), source_(source) {
    wireBus();
}

void IntentExecutor::wireBus() {
    // ★ `now` 每读点都要重新指（本类不会换 `readings_` 的地址，但显式写一次更便宜读）。
    ctx_.now = &readings_;
    ctx_.edit = edit_;
    ctx_.input = &queue_;
    ctx_.read = read_;
    // `ctx_.trigger` **不在这里填**：那是驱动方 / C2 的账（本切片的 `atPoint` 只填 now/edit/input/read）。
}

std::uint64_t IntentExecutor::nowMs() const {
    if (now_fn_ != nullptr) return now_fn_();
    // ★ 宿主单调毫秒：与 `TriggerRunner` 的缺省钟同源（`pluginNowNs`），⛔ 不扣暂停。
    return pluginNowNs() / 1'000'000ull;
}

std::uint64_t IntentExecutor::deliverCalls(InputPoint point) const {
    const std::size_t i = static_cast<std::size_t>(point);
    return i < deliver_calls_.size() ? deliver_calls_[i] : 0;
}

void IntentExecutor::atPoint(WindowManager& wm, InputPoint point) {
    // ---- ① 读数 + 写预算清零（与配方执行器的 `atPoint` 同形）----
    const std::size_t i = static_cast<std::size_t>(point);
    if (i < deliver_calls_.size()) ++deliver_calls_[i];
    ++read_points_;
    // ★ 写预算清零（`PointerCore` 的 protected 成员；配方执行器的 `atPoint` 同形）。
    writes_this_point_ = 0;
    // ★ 闸门③的刻度 = "**本读点之前**完成过几个 CursorPos 读点"：本读点自己的读还没发生，
    //   所以若是位置读点，得把刚 +1 的那个数扣掉（否则"move 与 down 同读点"又能满足它）。
    std::uint64_t cursor_done = deliverCalls(InputPoint::CursorPos);
    if (point == InputPoint::CursorPos && cursor_done > 0) --cursor_done;

    // ---- ② staged → ready（**每读点恰好一次**；在 poll 之前 ⇒ 本读点提交的看不见）----
    queue_.beginReadPoint();

    // ---- 早退（放在 ② **之后**：这样"每读点恰好一次 beginReadPoint"是字面成立的）----
    //   判据必须含 armed trigger（否则装了 trigger 也早退 ⇒ 永不触发 / 永不超时），
    //   队列深度必须用 `depth()`（含两级；② 之后 staged 恒空，所以此刻它等于 ready 深度）。
    const bool polling = runner_ != nullptr && runner_->armedCount() != 0;
    if (!polling && queue_.depth() == 0 && pending_.empty() && armed_.empty()) return;

    const std::uint64_t now = nowMs();
    const PointerPoint pt{point, now, deliverCalls(InputPoint::KeyLevel), cursor_done, read_points_, 0};

    // ---- ③ 采样 → poll（合同 §4.2 445：采样后、releaseDue 前；0 或 1 个事件）----
    if (polling) {
        readings_ = source_.sample(point);
        readings_.point = point;          // ★ 本读点（采样源不许编）
        readings_.read_index = read_points_;
        ctx_.now = &readings_;
        std::string err;
        TriggerEvent ev;
        runner_->atPoint(readings_, now, ctx_, &ev, &err);
        // ★ 失败的细节（`lastError()` / `failName()`）在 runner 上 —— 本类不复制一份错误状态。
    }

    // ---- ④ 到期的抬起（闸门① / 墙钟兜底）；⑤ 到期的按下（闸门③）----
    //   ⛔ 顺序不许反：先按下再抬起会让"新的一次 click + 上一次的抬起"在同一读点发生 ⇒ guest 只看到 up。
    releaseDue(wm, pt);
    pressDue(wm, pt);

    // ---- ⑥ 队列落地（本读点最多 1 条 guest 写；Edit 不走这里）----
    drainQueue(wm, pt);

    // ---- ⑦ 会话收口（**末尾**：本读点的 up 已入队，guest 下个读点能取到）----
    closeSessionIfDone(wm);
}

void IntentExecutor::sampleNow(InputPoint point) {
    // ★ 与 `atPoint` 的第 ③ 步**同一段采样代码**（⛔ 不要在这里另写一份读数组装）：
    //   采样源只给"引擎侧读数"，`point` / `read_index` 由本类覆盖成本读点的真值。
    //   `read_points_` 已经由本读点的 `atPoint` +1 ⇒ 这里**不再**自增（D-C2-10：不加读点计数）。
    readings_ = source_.sample(point);
    readings_.point = point;
    readings_.read_index = read_points_;
    ctx_.now = &readings_;
}

void IntentExecutor::drainQueue(WindowManager& wm, const PointerPoint& point) {
    // ★ 一次读点最多落**一条**写动作（S7 / Q2 的既有语义）：预算被 ④/⑤ 先占 ⇒ 本拍一条也不落，
    //   `ready_` 原样留着等下一拍（⛔ 不"借用下一拍的预算"：那会把两拍合成一拍）。
    if (writesThisPoint() != 0) return;
    Intent intent;
    if (!queue_.peekFront(&intent)) return;
    if (intent.cls != IntentClass::GuestWrite) return;  // HostEffect ⇒ C4（C1 连枚举值都没有）

    switch (intent.kind) {
        case IntentKind::Move: {
            queue_.popFront();
            armMoveAt(wm, intent.x, intent.y, point);
            break;
        }
        case IntentKind::Click: {
            // ★ 连点串行化（Q8-A）：这个按钮还按着（上一次的 up 没落地）⇒ **停在队首**，
            //   而且**整条队列**都等它（不同按钮也会被挡住 —— 这是形状，不是 bug；见头注）。
            if (buttonPending(intent.button)) return;
            queue_.popFront();
            armClickAt(wm, intent.x, intent.y, intent.button, point, intent.owner);
            break;
        }
        case IntentKind::MouseDown:
        case IntentKind::MouseUp:
            // ★ C1 **不施加**显式 down/up（`input.h` 的四投递口 / `mouse_down` 的轻闸门是 C4）。
            //   停住队首而**不是**丢掉：丢掉会把回执标成 Applied 而 guest 一点变化都没有（假成功）。
            return;
    }
    // ★ 施加上界：一条 intent = 一条写动作（`armMoveAt` / `armClickAt` 都做了 `input::*`）。
    // ★ **写预算要记账**（与配方执行器的 `atPoint` 同形）：`writesThisPoint()` 的语义是"本读点已施加
    //   几条写动作"，先前恒 0 ⇒ 与语义不符，且 C4 一旦在 ⑥ 之后再加写路径（`input.h` 的四投递口 /
    //   control click）就会**同拍写两条**而不被 ⑥ 挡住（合同 §6 行 4）。今天不改也不会立刻错（⑥ 每拍
    //   最多一条、⑦ 不看预算）—— 这条是为"第二个写路径"留的闸门。
    writes_this_point_ = 1;
    queue_.markApplied(intent.id);
}

void IntentExecutor::closeSessionIfDone(WindowManager& wm) {
    // 与配方执行器的 `closeSessionIfDone` **同一条收口**（§3.4 第 6 条）：交还 Program 的 hold
    // （`input::releaseProgram` 清 Program 抬起的电平并逐条 post up）。⛔ 不在这里做 owner 化：
    // C1 单 owner；C5 多 owner 时要不要按 owner 化 `releaseProgram` 是 C5 的裁决。
    if (session_closed_) return;
    if (!pending_.empty() || !armed_.empty()) return;
    if (queue_.depth() != 0) return;  // 含 staged（否则刚提交的 intent 会被"收口"抢在前面）
    input::releaseProgram(wm, kPointerMessageTimeMs);
    session_closed_ = true;
}

std::size_t IntentExecutor::releaseOwner(WindowManager& wm, std::uint32_t owner) {
    // 三个地方各有一份"该 owner 还没完成的事"：
    //   ① 指针状态（已按住 ⇒ **真的** post up；臂上 ⇒ 丢掉，⛔ 不伪造一次 up）；
    //   ② 队列里未落地的（staged_ + ready_ 都算 —— 漏 staged_ 会让它下个读点落地）；
    //   ③ runner 里该 owner 的 armed 槽。
    const std::size_t released = PointerCore::releaseOwner(wm, owner);
    queue_.dropOwner(owner);
    if (runner_ != nullptr) runner_->releaseOwner(owner);
    return released;
}

}  // namespace lift::plugin
