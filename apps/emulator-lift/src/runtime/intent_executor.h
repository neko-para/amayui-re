// emulator-lift · **`IntentExecutor`** —— 插件侧 Input 的唯一 pump（C1c）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.3（IntentQueue + Executor：click 两段 / 一读点
//       一条 guest 写 / trigger 的 Input 下一读点 / owner）、§4.4（`read → edit → input` 同点组合）、
//       §4.2 445（trigger poll 在**采样后、releaseDue 前**）、§5 C1 判据。
// 施工图：`.tmp/plugin-runtime/c1-design-review.md` §2.1（七步顺序）/ §2.5（早退条件）/
//         §4.2（队首头阻）/ §4.4（releaseOwner）/ §5.6（签名）。
//
// ★ **七步顺序写死**（`atPoint` 的唯一形状；⛔ 不许增删步骤）：
//     ① 写预算清零（+ 读点计数）
//     ② `queue.beginReadPoint()`：上一读点提交的 Input 在这一刻才可被取
//     ③ `source.sample(point)` → `runner.atPoint(...)`（**采样后、releaseDue 前**；0 或 1 个事件）
//     ④ `releaseDue`（到期的抬起；占写预算）
//     ⑤ `pressDue`（到期的按下；占写预算）
//     ⑥ `drainQueue`（本读点**最多 1 条** intent）
//     ⑦ `closeSessionIfDone`（三队列全空 ⇒ `input::releaseProgram`）
//
// ★ **② 必须在 ③ 之前**：否则本读点回调里 `ctx.input.*` 会在**同一个读点**就落地 —— 那正是
//   合同 §4.3 483–485 禁止的"trigger 的 Input 与 fire 同拍"。压在 ③ 之前 ⇒ 新提交的进 `staged_`，
//   本读点看不见（下一读点才提升）。
// ★ **③ 在 ④ 之前**（合同 §4.2 445）：这是唯一能让"回调里读到的 `released()` 还是旧值"可被观测的
//   位置。`owner-timeout` 用它做**可判**断言（回调内 `released()==0`、pump 返回后 `==1`），
//   ⛔ 不要换成"headless ⇒ 不超时"那类恒真断言。
// ★ **⑦ 放末尾**（`InputPlan` 放开头）：本读点的 up 已经入队 ⇒ guest 下个读点能取到；且避免
//   "刚提交的 intent 还没落地就把 Program 电平收了"。
//
// ★ **早退条件必须含 `runner_ && runner_->armedCount() > 0`**：配方执行器那一份 `atPoint` 的早退**不含**
//   armed trigger ⇒ 装了 trigger 也会早退 ⇒ 永不触发 / 永不超时。本类**没有**配方状态，所以早退
//   只由"三件事都空"决定：没有 armed trigger、队列空、没有按住的 / 臂上的指针。
//   · **放在 ② 之后** ⇒ "每读点恰好一次 `beginReadPoint()`"字面成立（⛔ 不是"空闲读点跳过提升、
//     反正 staged 是空的"那种要靠论证的写法）。
//   · 队列深度用 `queue_.depth()`（含两级）而**不是** `readyDepth()`：② 之前 staged 可以非空，
//     用 ready 深度会让"刚提交、还没提升"的那一拍早退 ⇒ 那条 intent 永远等不到下一拍。
//
// ★ **钟 = 宿主墙钟**（`pluginNowNs() / 1e6`），⛔ **不接** `VirtualClock` / `attachClock` /
//   `pausedTotalMs()`：这里是墙钟，扣暂停是配方执行器（`InputPlan`）的口径 —— 两者**故意不同**
//   （与 `TriggerRunner::atPoint` 收到的那个 `now_ms` 是同一个数）。
//
// ★ **两条 `PointerCore` 语义照抄、不重写**：click 的两段 + 三个闸门（`armClickAt` / `pressDue` /
//   `releaseDue`）、"同按钮未抬起 ⇒ 停队首"（连点串行化）。⇒ 本文件**没有**第二套坐标映射 / 闸门。
//   ★ **闸门 ②′（回调边界）也走同一份**：装配点用 `setFrameSource` 把 `presenter.frameCount()`
//   （= `InputPlan::presenting()` 的同一个计数）接进来 ⇒ `PointerPoint::frames` 是活的、
//   `callback_boundary=true` ⇒ 插件 `ctx.input.click` 的抬起同样要跨过一个完整 guest 回调。
//   ⛔ 阈值只有 `PointerCore::kCallbackFramesBeforeRelease` 一份（不在本类复制）。
//
// ★ **队首头阻是形状，不是 bug**（施工图 §4.2）：`drainQueue` 遇到队首是"该按钮仍在 `pending_`"的
//   click 就**停住整条队列**（不同按钮也会被挡住）。`one-write-per-read-point` 断言它。
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "runtime/context.h"       // Context（完整定义：本头是它的持有者）
#include "runtime/intent.h"
#include "runtime/intent_queue.h"
#include "runtime/pointer_core.h"  // PointerCore / PointerPoint / kInputPointCount
#include "runtime/trigger.h"       // TriggerReadings（采样源的返回类型）

namespace lift {

class WindowManager;

}  // namespace lift

namespace lift::plugin {

class EditSink;
class EngineRead;
class TriggerRunner;

// ★ **可注入的采样源**（C1 的测试缝；C2/C4 接 `MarkerSampler` + 鼠标位置）。
//   为什么是接口而不是"在本类里读 guest"：C1 的判据是 trigger / intent / edit 的**语义**，
//   ⛔ 不该为了它造一个假 Engine / `Machine`（施工图 §6.1）。⇒ 采样是一根可换的线，
//   而 `TriggerReadings` 是纯值 ⇒ 触发语义可以在**零 guest**下被判。
class ReadingsSource {
public:
    virtual ~ReadingsSource() = default;
    // 本读点的读数（`point` / `read_index` 由 executor **覆盖**成本读点的真值）。
    virtual TriggerReadings sample(InputPoint point) = 0;
};

class IntentExecutor : public PointerCore {
public:
    using NowFn = std::uint64_t (*)();  // 宿主**墙钟**毫秒（nullptr = `pluginNowNs()/1e6`）

    IntentExecutor(IntentQueue& queue, ReadingsSource& source);

    // 装配点（⛔ 都可以留空：C1 的 trigger 用例不需要 Edit / Read）。
    void setTriggerRunner(TriggerRunner* runner) { runner_ = runner; }
    void setBus(EditSink* edit, EngineRead* read) {
        edit_ = edit;
        read_ = read;
        wireBus();
    }
    // ★ 测试缝：注入墙钟（⛔ 生产路径用它读 `pluginNowNs()`）。同一个数要给 `runner.atPoint` 的
    //   `now_ms` 参数用 —— 驱动方负责让两者一致（`tests/trigger_runner.cpp` 的 `pump()` 就是这么做的）。
    void setClockForTest(NowFn now_ms) { now_fn_ = now_ms; }

    // ★ **帧线**（2026-11-22）：与 `InputPlan::presenting()` 是**同一个计数源** —— 宿主**已完成**的
    //   guest 回调数（`present.cpp` 在 `++frame_count_` 同一刻推 `PlanEventKind::Frame`；装配点给的是
    //   `presenter.frameCount()`）。供给之后 `PointerPoint::callback_boundary=true` ⇒ 抬起还必须在
    //   **按下之后的下一个完整 guest 回调**之后（`PointerCore::kCallbackFramesBeforeRelease`；
    //   闸门 ②′ 在 `PointerCore::releaseDue`，⛔ 本类不复制阈值、不另造一套）。
    //   · 缺省 `fn == nullptr` ⇒ `frames=0` / `callback_boundary=false` = **旧行为**（纯夹具 /
    //     没有 presenter 的驱动）。⛔ 不是"恒 false ⇒ 插件永远不受闸门②′"—— 生产装配点会接上。
    //   · 形状与 `InputPlan::setProfileMarkHook` / `setStateSeqSource` 同一条（裸函数指针 + `void*`；
    //     缺省 nullptr ⇒ 每个读点多一次判空，⛔ 不用 `std::function`）。
    using FrameCountFn = std::uint64_t (*)(void* ctx);
    void setFrameSource(void* ctx, FrameCountFn fn) {
        frame_ctx_ = ctx;
        frame_fn_ = fn;
    }

    // ★ **唯一 pump**（每读点一次；七步顺序见头注）。
    void atPoint(WindowManager& wm, InputPoint point);

    // ★ **只采样**（C2a；父级裁决 `c2-decisions.md` **D-C2-10**）：采样源 → `readings_` / `ctx.now`；
    //   **不碰**队列 / 写预算 / 七步 / 任何计数。⛔ 不计读点（那是 `atPoint` 的账）。
    //   为什么需要它：`atPoint` 的早退在 **② 之后**（没有 armed trigger 时**根本不会采样**）⇒
    //   "第一个读点 `atPoint` 返回后调 `runner.begin`"会让 `start` 里的 `ctx.now` 是空的。
    //   调用方（`PluginHost::onReadPoint`）在 `atPoint` **之后**、`begin` **之前**调一次：
    //   这一刻 `read_points_` 已由 `atPoint` +1 ⇒ `readings_.read_index` 就是本读点序号。
    //   ⛔ 不改 C1 的 `atPoint` 语义（它一行不动）。
    void sampleNow(InputPoint point);

    // ★ 释放一个 owner 的**全部**未完成的输入状态（C1 单 owner；C5 才多 owner）：
    //   按住的（真的抬 up）/ 臂上的 / 队列里未落地的，外加 runner 里该 owner 的 armed 槽。
    //   ⛔ 不在这里调 `input::releaseProgram`（那是**全抬**，会清掉别的 owner 的电平）——
    //   收口只由 `closeSessionIfDone`（三队列全空）做。
    std::size_t releaseOwner(WindowManager& wm, std::uint32_t owner);

    // ---- 读数（用例 / trace 用；⛔ 不是性能判据）----
    std::uint64_t deliverCalls(InputPoint point) const;
    std::uint64_t readPoints() const { return read_points_; }
    // `Context`（驱动方可以往 `trigger` 里塞工厂 —— 本切片的 `atPoint` 只填 now/edit/input/read）。
    Context& context() { return ctx_; }
    const TriggerReadings& readings() const { return readings_; }

private:
    std::uint64_t nowMs() const;
    // 装配 `Context` 的三根线（`now` 每个 pump 都指回自己的 `readings_`）。
    void wireBus();
    // ⑥ 本读点最多落地**一条** intent（写预算；队首头阻见头注）。
    void drainQueue(WindowManager& wm, const PointerPoint& point);
    // ⑦ 三队列全空 ⇒ 交还 Program 电平（一次）。
    void closeSessionIfDone(WindowManager& wm);

    IntentQueue& queue_;
    ReadingsSource& source_;
    TriggerRunner* runner_ = nullptr;
    EditSink* edit_ = nullptr;
    EngineRead* read_ = nullptr;
    // ★ 读点计数是**本类**的账（`PointerCore` 不持有读数源 —— 见 `pointer_core.h` 的 W2 边界）。
    std::array<std::uint64_t, kInputPointCount> deliver_calls_{};
    std::uint64_t read_points_ = 0;
    NowFn now_fn_ = nullptr;  // 测试缝（nullptr = `pluginNowNs()/1e6`）
    // ★ **帧线**（缺省 nullptr ⇒ `frames=0` / `callback_boundary=false` = 旧行为；见 `setFrameSource`）。
    //   `ctx` 是**非拥有**的（生产 = `AotHost*`）；本类只调它取"已完成 guest 回调数"。
    void* frame_ctx_ = nullptr;
    FrameCountFn frame_fn_ = nullptr;
    TriggerReadings readings_;  // 本读点的读数（`ctx_.now` 恒指它）
    // ★ `Context` 在 `trigger.h` 里只有前向声明（两个头不能互相 include）⇒ 持有它的**只有本头**
    //   （`intent_executor.h` include `context.h`，反向不成环）。回调面 = `context()`。
    Context ctx_;
};

}  // namespace lift::plugin
