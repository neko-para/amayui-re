// emulator-lift · 输入投递层（S1 骨架 / S2′ 门 / S4 控制面）—— 口径见 `input_plan.h` 的头注。
//
// ⛔ 本文件**不是**第二份输入状态：状态只有 `WindowManager` 一份。这里只有 step 表 + 计数 +
//   待抬起队列；施加输入的**每一处**都走 `runtime/input.h` 既有的 `input::*`（与 SDL / 旧脚本
//   同一条路径）。⛔ 不取墙钟、不碰容器以外的时序。
#include "runtime/input_plan.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <utility>

#include "runtime/input.h"  // input::mouseMove / mouseButton / key（唯一施加路径）
#include "runtime/vfs.h"    // openHostFileUtf8（Windows 的 UTF-8 路径）

namespace lift {
namespace {

// 待抬起的**兜底**刻度：连续这么多个读点都没有 `inputSampleCount` 前进（队列类读点不采样）
// ⇒ 仍然抬起。⛔ 不用毫秒：墙钟不许参与"哪一步生效"（铁律 3）。
constexpr std::uint64_t kForceReleaseReads = 16;

// 计划消息的 `MSG.time`：**没有** guest 时钟来源（plan 不在 tick 上下文里被调用，⛔ 也不为此
// 在读点上取墙钟）⇒ 恒 0。引擎不读它（`interaction-model.md` 的 `script` 源不碰 MSG.time）。
constexpr std::uint32_t kPlanMessageTimeMs = 0;

}  // namespace

const char* inputPointName(InputPoint point) {
    switch (point) {
        case InputPoint::KeyLevel: return "KeyLevel";
        case InputPoint::CursorPos: return "CursorPos";
        case InputPoint::PeekMsg: return "PeekMessage";
        case InputPoint::GetMsg: return "GetMessage";
        case InputPoint::MsgWait: return "MsgWait";
        case InputPoint::WaitMessage: return "WaitMessage";
        case InputPoint::QueueStatus: return "QueueStatus";
        case InputPoint::Translate: return "Translate";
        case InputPoint::Dispatch: return "Dispatch";
        case InputPoint::SetCursorPos: return "SetCursorPos";
        case InputPoint::FileRead: return "FileRead";
        case InputPoint::Count: break;
    }
    return "?";
}

void InputPlan::clear() {
    steps_.clear();
    pending_.clear();
    origin_.clear();
    next_ = 0;
    applied_ = 0;
    released_ = 0;
    read_points_ = 0;
    releases_pending_ = 0;
    deliver_calls_.fill(0);
    event_calls_.fill(0);
}

bool InputPlan::loadFile(const std::string& path, std::string* err) {
    std::FILE* f = openHostFileUtf8(path, "rb");
    if (f == nullptr) {
        if (err) *err = "打不开输入计划：" + path;
        clear();
        return false;
    }
    std::string text;
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    std::fclose(f);
    return loadText(text, path, err);
}

bool InputPlan::loadText(const std::string& text, const std::string& origin, std::string* err) {
    clear();
    // ★ **复用** `--interaction-graph` 的**同一份**解析器（`allow_plan_conds` 只多放行
    //   `presents <n>` 一个词）。⛔ 不造第二种格式：本函数只把 `Node` IR 投影成"门 + 动作"。
    InteractionGraph graph;
    if (!graph.loadText(text, origin, err, /*allow_plan_conds=*/true)) return false;
    std::vector<PlanStep> steps;
    for (const Node& node : graph.nodes()) {
        auto bad = [&](const std::string& why) {
            if (err) *err = origin + ":" + std::to_string(node.line) + ": " + why;
            return false;
        };
        switch (node.when.kind) {
            case CondKind::Always:
            case CondKind::Presents: break;
            case CondKind::ScreenHash:
            case CondKind::ScreenRect:
            case CondKind::ScreenChanged:
            case CondKind::Script:
                return bad(std::string("门种类不支持：`") + condKindName(node.when.kind) +
                           "`（本单只做 Always / Presents）");
        }
        if (node.has_expect) {
            return bad("`--input-plan` 不收 `expect`（plan 只表达「到点按」，不判画面）");
        }
        switch (node.action.kind) {
            case GraphActionKind::Click:
            case GraphActionKind::Key: break;
            case GraphActionKind::ClickCenter:
            case GraphActionKind::Wait:
            case GraphActionKind::ProfileBegin:
            case GraphActionKind::ProfileEnd:
            case GraphActionKind::Quit:
                return bad(std::string("动作不支持：`") + node.action.text +
                           "`（本单只做 click / key —— 每个 step 一个动作）");
        }
        PlanStep s;
        s.gate = node.when.kind;
        s.presents = node.when.presents;
        s.action = node.action;
        s.line = node.line;
        steps.push_back(std::move(s));
    }
    if (steps.empty()) {
        if (err) *err = origin + ": 文件里没有任何 step（至少要一行 `await always` / 一行动作）";
        return false;
    }
    steps_ = std::move(steps);
    origin_ = origin;
    return true;
}

bool InputPlan::gateHolds(const PlanStep& s) const {
    switch (s.gate) {
        case CondKind::Always: return true;
        case CondKind::Presents:
            // ★ **绝对**累计帧数（guest 可见、确定；`present.cpp` 推）。
            return presenting() >= s.presents;
        default:
            // `loadText` 已拒绝其余门 ⇒ 走到这里说明有人放宽了解析闸门：不成立（⛔ 不假装成立）。
            return false;
    }
}

void InputPlan::atPoint(WindowManager& wm, InputPoint point) {
    const std::size_t i = static_cast<std::size_t>(point);
    if (i < deliver_calls_.size()) ++deliver_calls_[i];
    ++read_points_;
    if (steps_.empty() && pending_.empty()) return;
    // ① 到期 / 兜底的**待抬起**先落地（同一次读点内、读之前）。
    releaseDue(wm);
    // ② flush-before-read：按文件顺序把到期 step 施加到 `WindowManager`（⛔ 不越过未到期的 step）。
    while (next_ < steps_.size()) {
        const PlanStep& s = steps_[next_];
        if (!gateHolds(s)) break;
        applyStep(wm, s);
        ++applied_;
        ++next_;
    }
}

void InputPlan::applyStep(WindowManager& wm, const PlanStep& s) {
    switch (s.action.kind) {
        case GraphActionKind::Click: {
            // ★ 与 `input.cpp` 的脚本路径同一段代码：先光标（消息 `WM_MOUSEMOVE` + 电平），
            //   再按下（`WM_LBUTTONDOWN` + 按钮位）。
            input::mouseMove(wm, s.action.x, s.action.y, InputSource::Script, kPlanMessageTimeMs);
            PendingRelease r;
            r.is_key = false;
            r.button = s.action.button;
            r.x = s.action.x;
            r.y = s.action.y;
            r.press_samples = wm.inputSampleCount();
            r.press_reads = read_points_;
            input::mouseButton(wm, r.x, r.y, r.button, /*down=*/true, InputSource::Script,
                               kPlanMessageTimeMs);
            pending_.push_back(r);
            ++releases_pending_;
            return;
        }
        case GraphActionKind::Key: {
            PendingRelease r;
            r.is_key = true;
            r.vk = s.action.vk;
            r.press_samples = wm.inputSampleCount();
            r.press_reads = read_points_;
            input::key(wm, r.vk, /*down=*/true, /*scancode=*/0u, InputSource::Script,
                       kPlanMessageTimeMs);
            pending_.push_back(r);
            ++releases_pending_;
            return;
        }
        default:
            // `loadText` 已拒绝其余动作 ⇒ 走到这里是不可能的（⛔ 静默忽略会让"计划生效"变成假象）。
            std::fprintf(stderr, "[input-plan] ⚠ 第 %d 行的动作没有被支持：这是实现 bug\n", s.line);
            return;
    }
}

void InputPlan::releaseDue(WindowManager& wm) {
    for (std::size_t i = 0; i < pending_.size();) {
        const PendingRelease& r = pending_[i];
        // ① guest 至少又**采样**过一拍（与 `input_script` 的 auto hold 同一条"一次按下一定被
        //    观察到"的判据，但**只用计数**、⛔ 不看毫秒）。+1 是"观察过按下"之后再过一拍。
        const bool sampled = wm.inputSampleCount() > r.press_samples + 1;
        // ② 兜底：队列类读点不采样 ⇒ 连续 kForceReleaseReads 个读点后仍然抬起（定数，非时钟）。
        const bool forced = read_points_ > r.press_reads + kForceReleaseReads;
        if (!sampled && !forced) {
            ++i;
            continue;
        }
        emitRelease(wm, r);
        pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(i));
        --releases_pending_;
    }
}

void InputPlan::emitRelease(WindowManager& wm, const PendingRelease& r) {
    if (r.is_key) {
        input::key(wm, r.vk, /*down=*/false, /*scancode=*/0u, InputSource::Script, kPlanMessageTimeMs);
    } else {
        input::mouseButton(wm, r.x, r.y, r.button, /*down=*/false, InputSource::Script,
                           kPlanMessageTimeMs);
    }
    ++released_;
}

std::size_t InputPlan::releaseAll(WindowManager& wm) {
    std::size_t n = 0;
    for (const PendingRelease& r : pending_) {
        emitRelease(wm, r);
        ++n;
    }
    pending_.clear();
    releases_pending_ = 0;
    return n;
}

void InputPlan::note(PlanEventKind kind) {
    // ★ 只记账。⛔ 不接受 `WindowManager` ⇒ 结构上不可能从这里施加 step（见头注铁律 3）。
    const std::size_t i = static_cast<std::size_t>(kind);
    if (i < event_calls_.size()) ++event_calls_[i];
}

std::uint64_t InputPlan::deliverCalls(InputPoint point) const {
    const std::size_t i = static_cast<std::size_t>(point);
    return i < deliver_calls_.size() ? deliver_calls_[i] : 0;
}

std::uint64_t InputPlan::noteCalls(PlanEventKind kind) const {
    const std::size_t i = static_cast<std::size_t>(kind);
    return i < event_calls_.size() ? event_calls_[i] : 0;
}

std::string InputPlan::summary() const {
    // 一行、**一次性**（⛔ 不许每帧打：`AGENTS.md` §12.8 第 3 条）。
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "steps=%llu fired=%llu presents=%llu released=%llu held=%zu（门 = Always/Presents）",
                  static_cast<unsigned long long>(steps_.size()),
                  static_cast<unsigned long long>(applied_),
                  static_cast<unsigned long long>(presenting()),
                  static_cast<unsigned long long>(released_), pending_.size());
    return std::string(buf);
}

}  // namespace lift
