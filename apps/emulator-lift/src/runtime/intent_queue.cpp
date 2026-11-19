// emulator-lift · `IntentQueue` 的实现 —— 口径见 `intent_queue.h` 的头注。
//
// ★ 本文件只有四个动作：追加（staged）/ 提升（staged→ready）/ 取走（popFront）/ 记账（history）。
//   ⛔ 这里**不许**出现任何"能不能施加"的判断（写预算 / 指针闸门 / 窗口）：那是 `PointerCore` 的事。
#include "runtime/intent_queue.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace lift::plugin {

Receipt IntentQueue::submit(const Intent& intent) {
    Receipt receipt;
    receipt.seq = ++submits_;
    // ★ 有界（合同 §6 行 7）：满了就拒，⛔ 不静默丢、⛔ 不扩容。`depth()` 含两级 —— 只看 ready_
    //   会让"同读点连提 100 条"绕过上界（它们都还在 staged_）。
    if (depth() >= kCapacity) {
        receipt.accepted = false;
        receipt.code = "queue_full";
        receipt.queued = depth();
        return receipt;
    }
    Intent copy = intent;
    copy.id = ++issued_;  // ★ id 由队列分配（⛔ 不信调用方填的那个）
    staged_.push_back(std::move(copy));
    receipt.accepted = true;
    receipt.code = nullptr;
    receipt.queued = depth();
    receipt.id = issued_;
    return receipt;
}

void IntentQueue::beginReadPoint() {
    // ★ 整段按 FIFO 追加（⛔ 不插队、⛔ 不排序）：ready_ 里已有的条目先落地，新提升的排在它们后面
    //   —— 这是"串行"的唯一含义（同一 owner 的两条 click 顺序 = 提交顺序）。
    ready_.insert(ready_.end(), staged_.begin(), staged_.end());
    staged_.clear();
}

bool IntentQueue::peekFront(Intent* out) const {
    if (ready_.empty()) return false;
    if (out != nullptr) *out = ready_.front();
    return true;
}

void IntentQueue::popFront() {
    if (ready_.empty()) return;  // 到不了（调用方先 peekFront）；响亮点，⛔ 不越界
    ready_.erase(ready_.begin());
}

bool IntentQueue::queued(std::uint64_t id) const {
    for (const Intent& intent : staged_) {
        if (intent.id == id) return true;
    }
    for (const Intent& intent : ready_) {
        if (intent.id == id) return true;
    }
    return false;
}

IntentQueue::Status IntentQueue::status(std::uint64_t id) const {
    if (id == 0) return Status::Unknown;  // 0 = 从没被分配（被拒的回执）
    if (queued(id)) return Status::Queued;  // 队列里的状态**永远**从两级里现算（⛔ 不信历史）
    // 最近一条优先（同一个 id 只会有一条终态记录；倒序找只是为了让"重复记录"取最新的）。
    for (std::size_t i = history_.size(); i > 0; --i) {
        if (history_[i - 1].id == id) return history_[i - 1].status;
    }
    return Status::Unknown;
}

void IntentQueue::remember(std::uint64_t id, Status status) {
    if (id == 0) return;
    history_.push_back(Record{id, status});
    // ★ 有界（见头注）：只留最近 `kStatusHistory` 条；被挤掉的老 id 降级成 Unknown（不是 Queued）。
    if (history_.size() > kStatusHistory) {
        history_.erase(history_.begin(), history_.begin() + static_cast<std::ptrdiff_t>(history_.size() - kStatusHistory));
    }
}

void IntentQueue::markApplied(std::uint64_t id) { remember(id, Status::Applied); }

std::size_t IntentQueue::dropOwner(std::uint32_t owner) {
    std::size_t dropped = 0;
    // ★ 两个队列都要扫：漏掉 staged_ ⇒ 下个读点 `beginReadPoint()` 会把"已释放的 owner"的 intent
    //   提升上来并落地 —— 那正是"releaseOwner 之后还动了一下"的静默 bug。
    for (std::size_t i = 0; i < staged_.size();) {
        if (staged_[i].owner != owner) {
            ++i;
            continue;
        }
        remember(staged_[i].id, Status::Dropped);
        staged_.erase(staged_.begin() + static_cast<std::ptrdiff_t>(i));
        ++dropped;
    }
    for (std::size_t i = 0; i < ready_.size();) {
        if (ready_[i].owner != owner) {
            ++i;
            continue;
        }
        remember(ready_[i].id, Status::Dropped);
        ready_.erase(ready_.begin() + static_cast<std::ptrdiff_t>(i));
        ++dropped;
    }
    return dropped;
}

}  // namespace lift::plugin
