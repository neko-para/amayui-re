// emulator-lift · **`IntentQueue`** —— 插件侧 Input 的唯一落点（C1c）
//
// 合同：`docs/03-emulator/plugin-runtime.md` §4.3 483–487（trigger 的 Input **一律下一读点**；
//       回执立即给；Owner；有界）、§4.5 703（`ctx.input.*` 进队列，一读点一条）。
// 施工图：`.tmp/plugin-runtime/c1-design-review.md` §4.1（staged / ready 两级 + 容量 64 +
//         `status()` 四态）、§4.2（为什么不插队 / 不借用下一拍预算）、§5.4（签名）。
//
// ★ **纯数据**（⛔ 不认识 `WindowManager` / 钟 / 预算）：
//   · 本类只回答"谁的、到没到可取的时机"；**什么时候能施加**（写预算 / 指针闸门）在
//     `PointerCore` / `IntentExecutor`；
//   · ⇒ 因此它可以在**回调里**被调用（`ctx.input->submit`），而不会重入施加路径：回调只写一个
//     `std::vector`，⛔ 不碰 guest、⛔ 不碰窗口。
//
// ★ **两级队列**（这是"trigger 的 Input 一律下一读点"的**唯一**实现点）：
//     submit  ⇒ `staged_`（本读点**看不见**）
//     beginReadPoint() ⇒ `staged_` 整段按 FIFO **追加**到 `ready_`（⛔ 不插队、⛔ 不重排）
//     取 / 施加 ⇒ 只看 `ready_`
//   `beginReadPoint()` 由 pump（`IntentExecutor::atPoint` 的第②步）每读点调**恰好一次**：
//   调两次 = 一条 intent 在同一读点里被"提前"看到；漏调 = 它永远不 ready（比"晚一拍"更糟）。
//
// ★ **容量 64**：`staged_ + ready_` 合计到 64 就 `accepted=false, code="queue_full"`（合同 §6 行 7：
//   ⛔ 不静默丢、⛔ 不扩容）。为什么与配方执行器的 `kImmediateClickCapacity` 同值却**不复用那个常量**：
//   core 不该 include 配方头（那正是"emulator 内 plan VM"的入口之一）；两个数各有各的上界语义。
//
// ★ **状态历史是有界的**（`kStatusHistory` = 256）：落地 / 丢弃的记录只留最近 256 条，更早的一律
//   `Unknown`。理由：`status(id)` 是**回执**用的（"我刚提交的那条到哪了"），不是审计日志 ——
//   无界历史会让一个长跑 Run 的内存随提交数线性增长，而调用方永远只会问最近的 id。
//   ⛔ 不要把它做成"查不到的当 Queued"：那会让队列满 / 被 drop 的条目永远像"还在排队"。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime/intent.h"

namespace lift::plugin {

class IntentQueue {
public:
    static constexpr std::size_t kCapacity = 64;
    // 落地 / 丢弃记录的保留条数（见头注；超出 ⇒ 老记录降级成 Unknown）。
    static constexpr std::size_t kStatusHistory = 256;

    // 提交一条：进 `staged_` 并**立即**回执（`accepted` / `code` / `seq` / `queued` / `id`）。
    Receipt submit(const Intent& intent);

    // 一个读点**恰好一次**：`staged_` 整段按 FIFO 追加到 `ready_`。
    void beginReadPoint();

    // 只看 `ready_` 的队首（⛔ 不看 staged：那正是"下一读点才可见"的意思）。
    bool peekFront(Intent* out) const;
    void popFront();

    std::size_t readyDepth() const { return ready_.size(); }
    std::size_t stagedDepth() const { return staged_.size(); }
    // ★ **含两级**（`staged_ + ready_`）：容量判据、pump 的早退判据都用它 —— 用 `readyDepth()`
    //   判早退会让"刚在回调里提交、还没 ready"的那一拍跳过 `beginReadPoint()`（intent 永远等下去）。
    std::size_t depth() const { return ready_.size() + staged_.size(); }

    enum class Status : std::uint8_t {
        Queued = 0,  // 还在 staged_ / ready_ 里（未落地）
        Applied,     // executor 已施加（`markApplied`）
        Dropped,     // `dropOwner` 抹掉（或从没进过队列）
        Unknown,     // 不认识的 id / 记录已被 `kStatusHistory` 上限挤掉
    };
    Status status(std::uint64_t id) const;

    // executor 施加成功之后调（⛔ 不自己调：施加语义不在本类）。
    void markApplied(std::uint64_t id);

    // 只丢**未落地**的（staged_ / ready_ 里该 owner 的全部）；返回丢了几条。
    // ⛔ 不动已按住的指针状态（那在 `PointerCore::releaseOwner`）。
    std::size_t dropOwner(std::uint32_t owner);

private:
    struct Record {
        std::uint64_t id = 0;
        Status status = Status::Unknown;
    };

    void remember(std::uint64_t id, Status status);
    bool queued(std::uint64_t id) const;

    std::vector<Intent> staged_;  // 本读点提交、**下一读点**才可见
    std::vector<Intent> ready_;   // 可被 `peekFront()` 取
    std::vector<Record> history_;
    std::uint64_t issued_ = 0;   // id 从 1 起（0 = "没分配"，即被拒的回执）
    std::uint64_t submits_ = 0;   // `Receipt::seq` 的单调来源
};

}  // namespace lift::plugin
