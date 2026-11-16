// emulator-lift · 输入投递层（三条指令的 AOT 输入配方）—— 口径见 `input_plan.h` 的头注。
//
// ⛔ 本文件**不是**第二份输入状态：状态只有 `WindowManager` 一份。这里只有指令表 + 计数 +
//   待抬起队列；施加输入的**每一处**都走 `runtime/input.h` 既有的 `input::*`（与 SDL / 旧脚本
//   同一条路径）。
// ★ 墙钟只出现在 **`wait`** 一处（宿主单调时钟，非阻塞 deadline）；`await script` 的门只看
//   引擎身份（guest 内存），⛔ 不看时钟、不看帧号。
#include "runtime/input_plan.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "runtime/cursor_map.h"  // referenceToClientPixel（1280×720 参考系 → 客户区，唯一公式）
#include "runtime/input.h"       // input::mouseMove / mouseButton（唯一施加路径）
#include "runtime/memory.h"      // GuestMemory（身份采样只经 MarkerSampler 读它）
#include "runtime/vfs.h"         // openHostFileUtf8（Windows 的 UTF-8 路径）

namespace lift {
namespace {

// 待抬起的**兜底**刻度：连续这么多个读点都没有 `inputSampleCount` 前进（队列类读点不采样）
// ⇒ 仍然抬起。定数、非时钟（`wait` 是唯一读墙钟的指令）。
constexpr std::uint64_t kForceReleaseReads = 16;

// 计划消息的 `MSG.time`：**没有** guest 时钟来源（plan 不在 tick 上下文里被调用）⇒ 恒 0。
// 引擎不读它（`interaction-model.md` 的 `script` 源不碰 MSG.time）。
constexpr std::uint32_t kPlanMessageTimeMs = 0;

// 报错口径与 `interaction_graph.cpp` 一致：`<origin>:<行号>: <原因>`（CLI 层据此 exit 2）。
std::string errAt(const std::string& origin, int line, const std::string& why) {
    return origin + ":" + std::to_string(line) + ": " + why;
}

bool isWordSpace(char c) { return c == ' ' || c == '\t' || c == '\r'; }

std::string trimAscii(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && isWordSpace(s[b])) ++b;
    while (e > b && isWordSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string lowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::vector<std::string> splitWords(const std::string& s) {
    std::vector<std::string> out;
    std::size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && isWordSpace(s[i])) ++i;
        if (i >= s.size()) break;
        const std::size_t start = i;
        while (i < s.size() && !isWordSpace(s[i])) ++i;
        out.push_back(s.substr(start, i - start));
    }
    return out;
}

// 只认十进制非负整数（⛔ 不用 strtoull：它对 "-5" 会回绕成一个大正数）。
bool parseU64(const std::string& t, std::uint64_t* out) {
    if (t.empty() || t.size() > 20) return false;
    std::uint64_t v = 0;
    for (char c : t) {
        if (c < '0' || c > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (v > (UINT64_MAX - digit) / 10ull) return false;
        v = v * 10ull + digit;
    }
    *out = v;
    return true;
}

// `click` 的参考系坐标：允许负数（人手可能量到边缘外），但必须是完整整数。
bool parseInt(const std::string& t, int* out) {
    const bool negative = !t.empty() && t[0] == '-';
    const std::string digits = negative ? t.substr(1) : t;
    std::uint64_t v = 0;
    if (!parseU64(digits, &v)) return false;
    if (negative) {
        if (v > 2147483648ull) return false;
        *out = static_cast<int>(-static_cast<std::int64_t>(v));
    } else {
        if (v > 2147483647ull) return false;
        *out = static_cast<int>(v);
    }
    return true;
}

// 与 `present.cpp` 的 `nowNs()` 同源：宿主单调时钟（⛔ 不是 `clock.h` 的 guest 虚拟时钟）。
std::uint64_t hostNowMs() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

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
    identity_ = MarkerSample{};
    next_ = 0;
    applied_ = 0;
    fired_ = 0;
    released_ = 0;
    read_points_ = 0;
    releases_pending_ = 0;
    deliver_calls_.fill(0);
    event_calls_.fill(0);
}

void InputPlan::attachGuestMemory(const GuestMemory* mem) {
    // 只换来源、不重置计数：装配点在解析之后、`host.init()` 之后（见头注 H7）。
    mem_ = mem;
    identity_ = MarkerSample{};
    marker_sampler_.resetCounters();
}

void InputPlan::setClockForTest(NowFn now_ms) { now_fn_ = now_ms; }

std::uint64_t InputPlan::nowMs() const { return now_fn_ != nullptr ? now_fn_() : hostNowMs(); }

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

// ★ **专用平铺解析器**：只认三条指令（`await script <名字>` / `wait <ms>` / `click <x> <y>
//   [left|right]`）。⛔ 不复用 `InteractionGraph::loadText`：那个模型是"一个门 + 恰好一个动作"，
//   表达不了 `wait`，也表达不了"门的下面紧跟着一条等待"（见头注 H1）。
bool InputPlan::loadText(const std::string& text, const std::string& origin, std::string* err) {
    clear();
    std::vector<PlanStep> steps;
    auto fail = [&](int line, const std::string& why) {
        if (err) *err = errAt(origin, line, why);
        return false;
    };
    // 一行 = 一条指令（`#` 到行尾是注释；空行忽略，但行号照数）。
    auto parseLine = [&](int line, const std::string& body, const std::vector<std::string>& tok) -> bool {
        const std::string head = lowerAscii(tok[0]);
        if (head == "await") {
            const std::string sub = tok.size() > 1 ? lowerAscii(tok[1]) : std::string();
            if (sub != "script") {
                // `await always` / `await presents <n>` / `await marker …` 全落这里。
                return fail(line,
                            "未知指令：`" + body +
                                "`（只认 `await script <名字>` / `wait <ms>` / `click <x> <y> [left|right]`）");
            }
            if (tok.size() != 3) {
                return fail(line, "`await script` 需要一个脚本名（收到 " +
                                      std::to_string(tok.size() > 2 ? tok.size() - 2 : 0) + " 个词）");
            }
            PlanStep s;
            s.kind = InstrKind::AwaitScript;
            s.script = tok[2];
            s.line = line;
            steps.push_back(std::move(s));
            return true;
        }
        if (head == "wait") {
            if (tok.size() != 2) {
                return fail(line, "`wait` 只收一个毫秒数（收到 " + std::to_string(tok.size() - 1) + " 个词）");
            }
            std::uint64_t ms = 0;
            if (!parseU64(tok[1], &ms) || ms == 0) {
                return fail(line, "`wait` 的毫秒必须是**正整数**（收到 `" + tok[1] + "`）");
            }
            PlanStep s;
            s.kind = InstrKind::Wait;
            s.wait_ms = ms;
            s.line = line;
            steps.push_back(std::move(s));
            return true;
        }
        if (head == "click") {
            if (tok.size() < 3) {
                return fail(line, "`click` 缺参数：要 `<x> <y> [left|right]`（收到 " +
                                      std::to_string(tok.size() - 1) + " 个词）");
            }
            if (tok.size() > 4) {
                return fail(line, "`click` 参数过多：最多 4 个词（收到 " + std::to_string(tok.size()) + " 个）");
            }
            int x = 0;
            int y = 0;
            if (!parseInt(tok[1], &x) || !parseInt(tok[2], &y)) {
                return fail(line, "`click` 的坐标必须是整数（收到 `" + tok[1] + "` `" + tok[2] + "`）");
            }
            PlanStep s;
            s.kind = InstrKind::Click;
            s.x = x;
            s.y = y;
            s.line = line;
            if (tok.size() == 4) {
                const std::string word = lowerAscii(tok[3]);
                if (word == "left") {
                    s.button = MouseButton::Left;
                } else if (word == "right") {
                    s.button = MouseButton::Right;
                } else {
                    return fail(line, "`click` 的按钮词只认**裸写**的 `left` / `right`（收到 `" + tok[3] + "`）");
                }
            }
            steps.push_back(std::move(s));
            return true;
        }
        // `key` / 图那套 `node` `when` `action` `next` `expect` `timeout` / 任何别的词。
        return fail(line, "未知指令：`" + body +
                              "`（只认 `await script <名字>` / `wait <ms>` / `click <x> <y> [left|right]`）");
    };

    int line_no = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string raw = text.substr(start, end - start);
        ++line_no;
        const std::size_t hash = raw.find('#');
        if (hash != std::string::npos) raw.resize(hash);
        const std::string body = trimAscii(raw);
        const std::vector<std::string> tok = splitWords(body);
        if (!tok.empty() && !parseLine(line_no, body, tok)) return false;
        if (end == text.size()) break;
        start = end + 1;
    }

    if (steps.empty()) {
        if (err) *err = origin + ": 文件里没有任何指令（只认 `await script <名字>` / `wait <ms>` / `click …`）";
        return false;
    }
    steps_ = std::move(steps);
    origin_ = origin;
    return true;
}

// 同 `(cur,id)` + 名字表签名不变 ⇒ 复用缓存的名字（否则每个读点都要付一次 CP932 解析）。
void InputPlan::refreshIdentity() {
    if (mem_ == nullptr) {
        identity_ = MarkerSample{};
        return;
    }
    const MarkerSample ident = marker_sampler_.sampleIdentity(*mem_);
    if (ident.valid && identity_.valid && ident.cur == identity_.cur && ident.id == identity_.id &&
        identity_.name_table_sig == marker_sampler_.nameTableSignature(*mem_, ident.id)) {
        return;  // 同一身份 ⇒ 上次解析出的名字仍有效
    }
    MarkerSample s = ident;
    if (s.valid) marker_sampler_.resolveName(*mem_, &s);
    identity_ = std::move(s);
}

void InputPlan::atPoint(WindowManager& wm, InputPoint point) {
    const std::size_t i = static_cast<std::size_t>(point);
    if (i < deliver_calls_.size()) ++deliver_calls_[i];
    ++read_points_;
    if (steps_.empty() && pending_.empty()) return;
    // ① 到期 / 兜底的**待抬起**先落地（同一次读点内、读之前）。
    releaseDue(wm);
    // ② flush-before-read：按文件顺序把到期指令消费到 `WindowManager`（⛔ 不越过未到期的指令）。
    bool identity_fresh = false;
    while (next_ < steps_.size()) {
        PlanStep& s = steps_[next_];
        if (s.kind == InstrKind::AwaitScript) {
            // ★ **电平**门：身份当前就是它 ⇒ 通过；身份读不到（base==0 / valid=false）⇒ 不成立。
            if (mem_ == nullptr) return;  // 没有身份来源 ⇒ 继续等（⛔ 不报错）
            if (!identity_fresh) {
                refreshIdentity();
                identity_fresh = true;
            }
            if (!identity_.valid) return;
            if (!scriptNameMatches(s.script, identity_)) return;
        } else if (s.kind == InstrKind::Wait) {
            // ★ **非阻塞**：只记 deadline（起点 = 第一次到达这条指令的读点）；到期由下一个读点消费。
            const std::uint64_t now = nowMs();
            if (!s.wait_started) {
                s.wait_started = true;
                s.wait_deadline_ms = now + s.wait_ms;
            }
            if (now < s.wait_deadline_ms) return;
        } else {  // Click
            applyStep(wm, s);
            ++fired_;
        }
        ++applied_;
        ++next_;
    }
}

void InputPlan::applyStep(WindowManager& wm, const PlanStep& s) {
    // `loadText` 只产生 Click 一种"会施加的"指令 ⇒ 走到这里 kind 一定是 Click。
    // ★ 坐标**算一次**：`mouseMove` / `mouseButton(down)` / 之后抬起都读同一对值
    //   （⛔ 两处各算一次 ⇒ move 到 A、消息 lParam 是 B 的静默漂移）。
    int client_x = 0;
    int client_y = 0;
    int target_w = 0;
    int target_h = 0;
    if (const HostWindow* w = wm.find(wm.primaryWindow())) {
        target_w = w->client_w;  // 0 / 没有窗口 ⇒ referenceToClientPixel 退回 1280×720
        target_h = w->client_h;
    }
    referenceToClientPixel(target_w, target_h, s.x, s.y, &client_x, &client_y);
    // ★ 与 `input.cpp` 的脚本路径同一段代码：先光标（消息 `WM_MOUSEMOVE` + 电平），再按下。
    input::mouseMove(wm, client_x, client_y, InputSource::Script, kPlanMessageTimeMs);
    PendingRelease r;
    r.button = s.button;
    r.x = client_x;
    r.y = client_y;
    r.press_samples = wm.inputSampleCount();
    r.press_reads = read_points_;
    input::mouseButton(wm, r.x, r.y, r.button, /*down=*/true, InputSource::Script, kPlanMessageTimeMs);
    pending_.push_back(r);
    ++releases_pending_;
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
    // 抬起复用**按下时存下的同一对**（已缩放的）客户区坐标。
    input::mouseButton(wm, r.x, r.y, r.button, /*down=*/false, InputSource::Script, kPlanMessageTimeMs);
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
    // ★ `presents=` 只是**读数**（`present.cpp` 推的 guest 累计 Present 数），⛔ 不再是门。
    char buf[256];
    std::snprintf(buf, sizeof(buf), "steps=%llu fired=%llu presents=%llu released=%llu held=%zu",
                  static_cast<unsigned long long>(steps_.size()),
                  static_cast<unsigned long long>(fired_),
                  static_cast<unsigned long long>(presenting()),
                  static_cast<unsigned long long>(released_), pending_.size());
    return std::string(buf);
}

}  // namespace lift
