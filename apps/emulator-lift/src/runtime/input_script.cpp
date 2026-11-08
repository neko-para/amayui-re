#include "runtime/input_script.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "runtime/input.h"
#include "runtime/vfs.h"

namespace lift {
namespace {

std::string trimCopy(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

std::string lowerCopy(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool parseU64(const std::string& text, std::uint64_t* out) {
    if (text.empty()) return false;
    std::size_t i = 0;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        i = 2;
    }
    std::uint64_t value = 0;
    for (; i < text.size(); ++i) {
        const char c = text[i];
        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        if (digit < 0 || digit >= base) return false;
        value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(digit);
        if (value > 0xFFFFFFFFull) return false;
    }
    *out = value;
    return true;
}

bool parseButton(const std::string& text, MouseButton* out) {
    const std::string w = lowerCopy(text);
    if (w == "left" || w == "l") {
        *out = MouseButton::Left;
        return true;
    }
    if (w == "right" || w == "r") {
        *out = MouseButton::Right;
        return true;
    }
    if (w == "middle" || w == "m") {
        *out = MouseButton::Middle;
        return true;
    }
    return false;
}

const char* buttonName(MouseButton b) {
    switch (b) {
        case MouseButton::Left: return "left";
        case MouseButton::Right: return "right";
        case MouseButton::Middle: return "middle";
    }
    return "?";
}

// ★ 自动 hold 的**安全下限**（见 `input_script.h` 顶部）：= 一个采样周期，夹在这两条之间。
//   下限 1 ms：采样周期未知（还没采样过 / 都在同一毫秒里）时，"计数 +1"本身就是保证。
//   上限 200 ms：真实界面的输入采样不会比 5 Hz 更慢；更大的"周期"是加载 / 卡顿的伪样本，
//   拿它当下限会让每次点击都像挂住（⛔ 那不是保守，那是把上限顶掉）。
constexpr std::uint64_t kAutoHoldFloorMinMs = 1;
constexpr std::uint64_t kAutoHoldFloorMaxMs = 200;

// 单位词 → 触发条件 kind（`at` / `after` / `wait` 共用同一张表）
bool parseTriggerUnit(const std::string& unit, TriggerKind* out) {
    if (unit == "frame" || unit == "frames") {
        *out = TriggerKind::Frame;
        return true;
    }
    if (unit == "ms" || unit == "guest_ms") {
        *out = TriggerKind::GuestMs;
        return true;
    }
    if (unit == "insn" || unit == "insns" || unit == "instr" || unit == "instructions") {
        *out = TriggerKind::Insn;
        return true;
    }
    if (unit == "wall_ms" || unit == "wall" || unit == "host_ms") {
        *out = TriggerKind::WallMs;
        return true;
    }
    return false;
}

}  // namespace

const char* triggerKindName(TriggerKind k) {
    switch (k) {
        case TriggerKind::Frame: return "frame";
        case TriggerKind::GuestMs: return "guest_ms";
        case TriggerKind::Insn: return "insn";
        case TriggerKind::WallMs: return "wall_ms";
    }
    return "?";
}

const char* triggerUnitName(TriggerKind k) { return triggerKindName(k); }

bool Trigger::due(std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns,
                  std::uint64_t wall_ms) const {
    switch (kind) {
        case TriggerKind::Frame: return frame >= value;
        case TriggerKind::GuestMs: return guest_ms >= value;
        case TriggerKind::Insn: return insns >= value;
        case TriggerKind::WallMs: return wall_ms >= value;
    }
    return false;
}

void InputScript::clear() {
    source_.clear();
    actions_.clear();
    releases_.clear();
    presses_.clear();
    stats_ = ReleaseStats{};
    last_release_ = ReleaseInfo{};
    applied_ = 0;
    quit_requested_ = false;
    quit_posted_ = false;
    finish_logged_ = false;
    for (int i = 0; i < 4; ++i) have_anchor_[i] = false;
    anchor_frame_ = 0;
    anchor_guest_ms_ = 0;
    anchor_insn_ = 0;
    anchor_wall_ms_ = 0;
    have_pos_ = false;
    last_x_ = 0;
    last_y_ = 0;
    profile_actions_ = 0;
    profile_begin_pending_ = false;
    profile_end_pending_ = false;
    profile_begin_label_.clear();
    profile_end_label_.clear();
    stall_fired_ = false;
    saw_frame_ever_ = false;
    have_frame_sample_ = false;
    last_frame_seen_ = 0;
    last_frame_change_wall_ms_ = 0;
    frame_interval_sum_ms_ = 0;
    frame_interval_count_ = 0;
    pending_dirty_ = true;  // 动作集被清空 ⇒ 聚合缓存作废
}

bool InputScript::loadFile(const std::string& path, std::string* err) {
    std::FILE* f = openHostFileUtf8(path, "rb");
    if (f == nullptr) {
        if (err) *err = "打不开输入脚本：" + path;
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

bool InputScript::loadText(const std::string& text, const std::string& origin, std::string* err) {
    clear();
    source_ = origin;
    Trigger trigger;
    bool have_trigger = false;
    int line_no = 0;

    auto fail = [&](int ln, const std::string& why) {
        if (err) *err = origin + ":" + std::to_string(ln) + ": " + why;
        clear();
        return false;
    };

    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        ++line_no;
        // `#` 起注释（整行 / 行尾都吃）；`;` 也当分隔符（与既有输入脚本工具的写法兼容）。
        const std::size_t hash = raw.find('#');
        if (hash != std::string::npos) raw.resize(hash);
        std::vector<std::string> segments;
        std::size_t start = 0;
        for (;;) {
            const std::size_t semi = raw.find(';', start);
            segments.push_back(raw.substr(start, semi == std::string::npos ? std::string::npos : semi - start));
            if (semi == std::string::npos) break;
            start = semi + 1;
        }
        for (const std::string& segment : segments) {
            const std::string body = trimCopy(segment);
            if (body.empty()) continue;
            std::istringstream words(body);
            std::vector<std::string> tok;
            std::string w;
            while (words >> w) tok.push_back(w);
            const std::string head = lowerCopy(tok[0]);

            // ---- 时间线触发条件 / 相对推进 ----
            if (head == "at" || head == "after" || head == "wait") {
                // ★ 触发条件是**一个条件**（`Trigger{kind, value}`），不是"帧号"：
                //   `at <单位> <值>` / `after|wait <值> <单位>` 都只是候选写法之一。
                //   尾部可带 `stall MS`（只对 frame 有意义：帧停滞兜底）。
                std::vector<std::string> core;
                std::uint64_t stall_ms = 0;
                bool have_stall = false;
                for (std::size_t i = 0; i < tok.size();) {
                    if (lowerCopy(tok[i]) == "stall") {
                        if (i + 1 >= tok.size() || !parseU64(tok[i + 1], &stall_ms)) {
                            return fail(line_no, "`stall` 后面需要毫秒数");
                        }
                        have_stall = true;
                        i += 2;
                        continue;
                    }
                    core.push_back(tok[i]);
                    ++i;
                }
                if (core.size() != 3) {
                    return fail(line_no, body + " ⇒ 需要 3 个词（如 `at frame 600`）");
                }
                // ★ 词序不同：`at <单位> <值>`（at frame 600 / at insn 5000），而
                //   `after|wait <值> <单位>`（after 30 frames / wait 2 ms）——按 Win32 记法的习惯。
                const bool at_form = (head == "at");
                const std::string unit = lowerCopy(at_form ? core[1] : core[2]);
                std::uint64_t value = 0;
                if (!parseU64(at_form ? core[2] : core[1], &value)) {
                    return fail(line_no, "不是合法数字：" + (at_form ? core[2] : core[1]));
                }
                TriggerKind kind = TriggerKind::Frame;
                if (!parseTriggerUnit(unit, &kind)) {
                    return fail(line_no, "`" + head + "` 只认 frame(s) / ms / guest_ms / insn(s) / wall_ms（收到 " +
                                             unit + "）");
                }
                std::uint64_t* anchor = nullptr;
                bool* have = &have_anchor_[static_cast<int>(kind)];
                switch (kind) {
                    case TriggerKind::Frame: anchor = &anchor_frame_; break;
                    case TriggerKind::GuestMs: anchor = &anchor_guest_ms_; break;
                    case TriggerKind::Insn: anchor = &anchor_insn_; break;
                    case TriggerKind::WallMs: anchor = &anchor_wall_ms_; break;
                }
                if (at_form) {
                    *anchor = value;
                    *have = true;
                    trigger.kind = kind;
                    trigger.value = value;
                } else {
                    if (!*have) {
                        return fail(line_no, "`" + head + " " + core[1] + " " + core[2] +
                                                 "` 需要一个同单位基准（先写 `at " + triggerUnitName(kind) + "`）");
                    }
                    *anchor += value;
                    trigger.kind = kind;
                    trigger.value = *anchor;
                }
                trigger.stall_ms = have_stall ? stall_ms : 0;
                // ★ `wait` 同时是一个**动作**（no-op）：脚本要等它到期才收口 ⇒ 尾部 `wait` 能"保持按住"。
                //   `after` 只是给下一条动作换锚点（自己不产生动作）。
                if (head == "wait") {
                    Action w;
                    w.kind = Kind::Wait;
                    w.trigger = trigger;
                    w.text = body;
                    actions_.push_back(w);
                }
                // ⚠ `wait` 与 `after` 的锚点语义**相同**：两者都推进锚点并把它作为下一条动作的
                //    触发条件（区别只是 `wait` 自己不产生动作）。⛔ 不要在这里 `continue` —— 那会
                //    让 `wait 30 frames` 之后的动作落回**上一个**触发时间（一次全在旧帧触发）。
                have_trigger = true;
                continue;
            }

            // ---- 动作 ----
            if (!have_trigger) {
                return fail(line_no, body + " ⇒ 动作前必须先给时间（`at frame N` / `at insn N` / `at guest_ms T`）");
            }
            Action a;
            a.trigger = trigger;
            a.text = body;

            if (head == "mouse_move") {
                if (tok.size() != 3) return fail(line_no, "mouse_move 需要 X Y");
                std::uint64_t x = 0;
                std::uint64_t y = 0;
                if (!parseU64(tok[1], &x) || !parseU64(tok[2], &y)) return fail(line_no, "坐标不是合法数字");
                a.kind = Kind::MouseMove;
                a.x = static_cast<int>(x);
                a.y = static_cast<int>(y);
                last_x_ = a.x;
                last_y_ = a.y;
                have_pos_ = true;
            } else if (head == "mouse_down" || head == "mouse_up") {
                if (tok.size() > 2) return fail(line_no, head + " 最多带一个按钮名");
                MouseButton b = MouseButton::Left;
                if (tok.size() == 2 && !parseButton(tok[1], &b)) return fail(line_no, "未知按钮：" + tok[1]);
                if (!have_pos_) {
                    return fail(line_no, head + " 之前必须有 mouse_move（⛔ 不猜坐标）");
                }
                a.kind = head == "mouse_down" ? Kind::MouseDown : Kind::MouseUp;
                a.button = b;
                a.x = last_x_;
                a.y = last_y_;
            } else if (head == "click") {
                if (tok.size() < 3) return fail(line_no, "click 需要 X Y [left|right|middle] [hold N [frames|ms]]");
                std::uint64_t x = 0;
                std::uint64_t y = 0;
                if (!parseU64(tok[1], &x) || !parseU64(tok[2], &y)) return fail(line_no, "坐标不是合法数字");
                MouseButton b = MouseButton::Left;
                for (std::size_t i = 3; i < tok.size(); ++i) {
                    if (lowerCopy(tok[i]) == "hold") {
                        // ★ 显式 hold = **用户说了算**（帧 或 guest_ms；优先于自动值）。
                        //   不带单位时按帧（旧写法 `hold 2` 保持原义）。
                        if (i + 1 >= tok.size() || !parseU64(tok[i + 1], &a.hold_value)) {
                            return fail(line_no, "`hold` 后面需要帧数或毫秒数");
                        }
                        a.hold_explicit = true;
                        a.hold_unit = TriggerKind::Frame;
                        ++i;
                        if (i + 1 < tok.size()) {
                            const std::string unit = lowerCopy(tok[i + 1]);
                            if (unit == "ms" || unit == "guest_ms") {
                                a.hold_unit = TriggerKind::GuestMs;
                                ++i;
                            } else if (unit == "frame" || unit == "frames") {
                                ++i;
                            }
                        }
                        // 帧单位保持旧口径（至少 1 帧）；ms 允许 0（= 下一个 tick 抬手）。
                        if (a.hold_unit == TriggerKind::Frame && a.hold_value < 1) a.hold_value = 1;
                    } else if (!parseButton(tok[i], &b)) {
                        return fail(line_no, "未知参数：" + tok[i]);
                    }
                }
                a.kind = Kind::Click;
                a.x = static_cast<int>(x);
                a.y = static_cast<int>(y);
                a.button = b;
                last_x_ = a.x;
                last_y_ = a.y;
                have_pos_ = true;
            } else if (head == "key_down" || head == "key_up") {
                if (tok.size() != 2) return fail(line_no, head + " 需要一个 VK（1..255）");
                std::uint64_t vk = 0;
                if (!parseU64(tok[1], &vk) || vk == 0 || vk > 255) return fail(line_no, "VK 必须是 1..255");
                a.kind = head == "key_down" ? Kind::KeyDown : Kind::KeyUp;
                a.vk = static_cast<std::uint32_t>(vk);
            } else if (head == "profile_begin" || head == "profile_end") {
                // ★ 窗口边界 = **本动作的触发条件**（帧 / 指令数 / guest_ms / 将来的具名标记）。
                //   label 是单个词（用于配对与报错；⛔ 不参与语义）。
                if (tok.size() != 2) return fail(line_no, head + " 需要一个 label（单个词）");
                a.kind = head == "profile_begin" ? Kind::ProfileBegin : Kind::ProfileEnd;
                a.label = tok[1];
            } else if (head == "quit" || head == "exit") {
                if (tok.size() != 1) return fail(line_no, head + " 不带参数");
                a.kind = Kind::Quit;
            } else {
                return fail(line_no, "未知指令：" + tok[0]);
            }
            if (a.kind == Kind::ProfileBegin || a.kind == Kind::ProfileEnd) ++profile_actions_;
            actions_.push_back(a);
        }
    }

    if (actions_.empty()) return fail(line_no, "脚本里没有任何动作（只有注释 / 锚点）");
    pending_dirty_ = true;  // 动作集变了 ⇒ 聚合缓存作废
    return true;
}

bool InputScript::takeProfileBegin(std::string* label) {
    if (!profile_begin_pending_) return false;
    profile_begin_pending_ = false;
    if (label) *label = profile_begin_label_;
    profile_begin_label_.clear();
    return true;
}

bool InputScript::takeProfileEnd(std::string* label) {
    if (!profile_end_pending_) return false;
    profile_end_pending_ = false;
    if (label) *label = profile_end_label_;
    profile_end_label_.clear();
    return true;
}

void InputScript::noteFrameForAverage(std::uint64_t frame, std::uint64_t wall_ms) {
    if (frame > 0) saw_frame_ever_ = true;
    if (!have_frame_sample_) {
        have_frame_sample_ = true;
        last_frame_seen_ = frame;
        last_frame_change_wall_ms_ = wall_ms;
        return;
    }
    if (frame == last_frame_seen_) return;
    const std::uint64_t dt = wall_ms > last_frame_change_wall_ms_ ? wall_ms - last_frame_change_wall_ms_ : 0;
    // 只采"正常帧间隔"（> 1 s 的样本是卡顿 / 加载，拿它算均值会把兜底推得太远）
    if (dt > 0 && dt <= 1000) {
        frame_interval_sum_ms_ += dt;
        if (++frame_interval_count_ > 64) {
            frame_interval_sum_ms_ /= 2;
            frame_interval_count_ /= 2;
        }
    }
    last_frame_seen_ = frame;
    last_frame_change_wall_ms_ = wall_ms;
}

void InputScript::convertFrameTriggersToWall(std::uint64_t frame, std::uint64_t wall_ms) {
    const std::uint64_t avg = frame_interval_count_ == 0 ? 16ull : frame_interval_sum_ms_ / frame_interval_count_;
    const std::uint64_t period = avg < 1 ? 1 : (avg > 100 ? 100 : avg);
    for (Action& a : actions_) {
        if (a.applied) continue;
        if (a.trigger.kind != TriggerKind::Frame) continue;
        const std::uint64_t remaining = a.trigger.value > frame ? a.trigger.value - frame : 0;
        a.trigger.kind = TriggerKind::WallMs;
        a.trigger.value = wall_ms + remaining * period;
    }
    // 帧停了 ⇒ 显式 hold 的**帧单位**抬起也必须现在补发（否则按钮电平会卡到收口）。
    // ★ 自动 hold 的抬起**不看帧** ⇒ 这里不碰它（它等的是 guest 采样，跟帧号无关）。
    for (Release& r : releases_) {
        if (!r.auto_hold && r.unit == TriggerKind::Frame) r.due_value = frame;
    }
    pending_dirty_ = true;  // Frame→WallMs 折算改了动作集 ⇒ 聚合缓存作废
}

// ---- 按下 / 抬起（"一次按下一定被 guest 观察到"；口径见 input_script.h 顶部那一段）----

void InputScript::notePress(std::uint32_t id, const WindowManager& wm, std::uint64_t guest_ms) {
    PressInfo& p = presses_[id];
    p.samples = wm.inputSampleCount();
    p.guest_ms = guest_ms;
}

const InputScript::PressInfo* InputScript::findPress(std::uint32_t id) const {
    const auto it = presses_.find(id);
    return it == presses_.end() ? nullptr : &it->second;
}

void InputScript::dropPress(std::uint32_t id) { presses_.erase(id); }

bool InputScript::releaseQueued(std::uint32_t id) const {
    for (const Release& r : releases_) {
        const std::uint32_t rid = r.is_key ? r.vk : mousePressId(r.button);
        if (rid == id) return true;
    }
    return false;
}

std::uint64_t InputScript::autoHoldFloorMs(const WindowManager& wm) const {
    // ★ 安全下限 = **一个采样周期**（最近一次采样间隔，夹在 1..200 ms）：
    //   只等"计数 +1"会卡在采样边界的竞态上（引擎可能这一句只取了位置、下一句才读按钮电平）。
    //   样本不足（还没采样过，或都在同一毫秒里）⇒ 1 ms（那个 +1 本身已经是保证）。
    const std::uint64_t period = wm.inputSamplePeriodMs();
    if (period == 0) return kAutoHoldFloorMinMs;
    if (period < kAutoHoldFloorMinMs) return kAutoHoldFloorMinMs;
    if (period > kAutoHoldFloorMaxMs) return kAutoHoldFloorMaxMs;
    return period;
}

bool InputScript::releaseDue(Release& r, const WindowManager& wm, std::uint64_t frame, std::uint64_t guest_ms,
                             bool* bounded) {
    *bounded = false;
    if (!r.auto_hold) {
        // 显式 hold：用户说了算（帧 / guest_ms），**完全不看采样**。
        return r.unit == TriggerKind::Frame ? frame >= r.due_value : guest_ms >= r.due_value;
    }
    // 自动 hold：① guest 至少观察过一次按下（采样计数 +1）；
    //            ② 从**观察到的那一刻**起再等一个采样周期（安全下限：避开采样边界）。
    if (wm.inputSampleCount() > r.press_samples) {
        if (!r.observed) {
            r.observed = true;
            r.observe_ms = wm.lastInputSampleMs();
        }
        if (guest_ms >= r.observe_ms + autoHoldFloorMs(wm)) return true;
    }
    // ③ 兜底上限：guest 一直不采样（画面静止 / 在别的循环里）⇒ 到点仍然抬起 + 告警。
    if (max_auto_hold_ms_ != 0 && guest_ms >= r.press_ms + max_auto_hold_ms_) {
        *bounded = true;
        return true;
    }
    return false;
}

void InputScript::emitRelease(WindowManager& wm, const Release& r, std::uint64_t frame, std::uint64_t guest_ms,
                              std::uint32_t time_ms, bool bounded) {
    if (r.is_key) {
        input::key(wm, r.vk, false, 0u, InputSource::Script, time_ms);
    } else {
        input::mouseButton(wm, r.x, r.y, r.button, false, InputSource::Script, time_ms);
    }
    const std::uint64_t samples_now = wm.inputSampleCount();
    const std::uint64_t waited = samples_now > r.press_samples ? samples_now - r.press_samples : 0;
    const std::uint64_t hold_ms = guest_ms > r.press_ms ? guest_ms - r.press_ms : 0;
    ++stats_.releases;
    if (r.auto_hold) {
        ++stats_.auto_releases;
    } else {
        ++stats_.explicit_releases;
    }
    if (bounded) ++stats_.bounded_no_sample;
    if (waited > stats_.waited_samples_max) stats_.waited_samples_max = waited;
    if (hold_ms > stats_.hold_ms_max) stats_.hold_ms_max = hold_ms;
    last_release_ = ReleaseInfo{};
    last_release_.valid = true;
    last_release_.is_key = r.is_key;
    last_release_.vk = r.vk;
    last_release_.button = r.button;
    last_release_.auto_hold = r.auto_hold;
    last_release_.bounded = bounded;
    last_release_.press_frame = r.press_frame;
    last_release_.release_frame = frame;
    last_release_.press_ms = r.press_ms;
    last_release_.release_ms = guest_ms;
    last_release_.press_samples = r.press_samples;
    last_release_.release_samples = samples_now;
    last_release_.observe_ms = r.observe_ms;
    // ★ 可观测：这次抬起跨了多少 guest 时间 / 等了几次采样 ⇒ 以后判断"是不是又没点中"。
    //   `post_sample_ms` = 从"看到按下"的那次采样到抬起又等了多久（安全下限的证据）。
    const std::uint64_t post_sample_ms = (r.observed && guest_ms > r.observe_ms) ? guest_ms - r.observe_ms : 0;
    if (r.is_key) {
        std::printf("[input-script] frame=%llu guest_ms=%llu key-up 0x%02X auto=%d waited_samples=%llu "
                    "hold_ms=%llu post_sample_ms=%llu%s\n",
                    static_cast<unsigned long long>(frame), static_cast<unsigned long long>(guest_ms), r.vk,
                    r.auto_hold ? 1 : 0, static_cast<unsigned long long>(waited),
                    static_cast<unsigned long long>(hold_ms), static_cast<unsigned long long>(post_sample_ms),
                    bounded ? " bounded=1" : "");
    } else {
        std::printf("[input-script] frame=%llu guest_ms=%llu click-release at=(%d,%d) %s auto=%d "
                    "waited_samples=%llu hold_ms=%llu post_sample_ms=%llu%s\n",
                    static_cast<unsigned long long>(frame), static_cast<unsigned long long>(guest_ms), r.x, r.y,
                    buttonName(r.button), r.auto_hold ? 1 : 0, static_cast<unsigned long long>(waited),
                    static_cast<unsigned long long>(hold_ms), static_cast<unsigned long long>(post_sample_ms),
                    bounded ? " bounded=1" : "");
    }
    if (bounded) {
        std::fprintf(stderr,
                     "[input-script] ⚠ %s 的自动抬起到了上限 %llums（guest 期间一次输入采样都没有："
                     "samples=%llu）⇒ 仍然抬起，脚本继续（⛔ 不挂死）；要更长请加 --input-max-hold\n",
                     r.is_key ? "key_up" : "click", static_cast<unsigned long long>(max_auto_hold_ms_),
                     static_cast<unsigned long long>(waited));
    }
}

void InputScript::pushRelease(const Release& r) { releases_.push_back(r); }

void InputScript::apply(WindowManager& wm, Action& a, std::uint64_t frame, std::uint64_t guest_ms) {
    const std::uint32_t time_ms = static_cast<std::uint32_t>(guest_ms);
    switch (a.kind) {
        case Kind::MouseMove:
            input::mouseMove(wm, a.x, a.y, InputSource::Script, time_ms);
            break;
        case Kind::MouseDown:
            input::mouseButton(wm, a.x, a.y, a.button, true, InputSource::Script, time_ms);
            notePress(mousePressId(a.button), wm, guest_ms);
            break;
        case Kind::MouseUp: {
            // ★ 显式 up 也受同一条保证约束：按下之后 guest **一次也没采样过** ⇒ 推迟（不静默漏掉）。
            const std::uint32_t id = mousePressId(a.button);
            const PressInfo* p = findPress(id);
            if (p != nullptr && !releaseQueued(id) && wm.inputSampleCount() <= p->samples) {
                Release r;
                r.is_key = false;
                r.button = a.button;
                r.x = a.x;
                r.y = a.y;
                r.auto_hold = true;
                r.press_samples = p->samples;
                r.press_ms = p->guest_ms;
                r.press_frame = frame;
                r.text = a.text;
                pushRelease(r);
                ++stats_.deferred_ups;
                std::printf("[input-script]   mouse_up 推迟：按下后 guest 还没采样（press_samples=%llu）\n",
                            static_cast<unsigned long long>(p->samples));
                break;
            }
            input::mouseButton(wm, a.x, a.y, a.button, false, InputSource::Script, time_ms);
            dropPress(id);
            break;
        }
        case Kind::KeyDown:
            input::key(wm, a.vk, true, 0u, InputSource::Script, time_ms);
            notePress(a.vk, wm, guest_ms);
            break;
        case Kind::KeyUp: {
            const PressInfo* p = findPress(a.vk);
            if (p != nullptr && !releaseQueued(a.vk) && wm.inputSampleCount() <= p->samples) {
                Release r;
                r.is_key = true;
                r.vk = a.vk;
                r.auto_hold = true;
                r.press_samples = p->samples;
                r.press_ms = p->guest_ms;
                r.press_frame = frame;
                r.text = a.text;
                pushRelease(r);
                ++stats_.deferred_ups;
                std::printf("[input-script]   key_up 推迟：按下后 guest 还没采样（press_samples=%llu）\n",
                            static_cast<unsigned long long>(p->samples));
                break;
            }
            input::key(wm, a.vk, false, 0u, InputSource::Script, time_ms);
            dropPress(a.vk);
            break;
        }
        case Kind::Click: {
            input::mouseMove(wm, a.x, a.y, InputSource::Script, time_ms);
            input::mouseButton(wm, a.x, a.y, a.button, true, InputSource::Script, time_ms);
            notePress(mousePressId(a.button), wm, guest_ms);
            Release r;
            r.is_key = false;
            r.x = a.x;
            r.y = a.y;
            r.button = a.button;
            r.press_frame = frame;
            r.press_samples = wm.inputSampleCount();  // 两种路径都记（可观测 / 守卫用）
            r.press_ms = guest_ms;
            r.text = a.text;
            if (a.hold_explicit) {
                // 显式 hold：优先于自动值（用户说了算）。
                r.auto_hold = false;
                r.unit = a.hold_unit;
                r.due_value = a.hold_unit == TriggerKind::Frame ? frame + a.hold_value : guest_ms + a.hold_value;
                std::printf("[input-script]   click-release scheduled: hold=%llu %s ⇒ target=%llu at=(%d,%d) %s\n",
                            static_cast<unsigned long long>(a.hold_value),
                            a.hold_unit == TriggerKind::Frame ? "frames" : "ms",
                            static_cast<unsigned long long>(r.due_value), r.x, r.y, buttonName(r.button));
            } else {
                // 自动 hold：up 等到 guest 至少采样一次（+ 一个采样周期），到上限仍未采样则强制抬起。
                r.auto_hold = true;
                r.press_samples = wm.inputSampleCount();
                r.press_ms = guest_ms;
                std::printf("[input-script]   click-release scheduled: auto（等 guest 采样；上限 %llums）at=(%d,%d) %s\n",
                            static_cast<unsigned long long>(max_auto_hold_ms_), r.x, r.y, buttonName(r.button));
            }
            pushRelease(r);
            break;
        }
        case Kind::Wait:
            // no-op：它的作用只是让"脚本还没收口"持续到这个时刻（apply 里什么都不做）。
            break;
        case Kind::ProfileBegin:
            if (profile_begin_pending_) {
                // 同一次 tick 里两个 begin：⛔ 不静默覆盖（后者会丢掉前者的 label）。
                std::fprintf(stderr, "[profile-window] ⚠ 上一个 profile_begin(%s) 还没被消费，忽略 %s\n",
                             profile_begin_label_.c_str(), a.label.c_str());
                break;
            }
            profile_begin_pending_ = true;
            profile_begin_label_ = a.label;
            break;
        case Kind::ProfileEnd:
            profile_end_pending_ = true;
            profile_end_label_ = a.label;
            break;
        case Kind::Quit:
            quit_requested_ = true;
            break;
    }
}

// ★ 缓存"未应用动作"的聚合量。动作集只在三处变：动作被 apply、frame 条件被折算成 wall、
//   脚本重载/清空 ⇒ 那三处置 dirty，tick 里至多重算一次。等价性判据：`due()` 就是 `now >= value`
//   ⇒ "该单位至少有一条到期" ⇔ `min(value) <= now`。
void InputScript::rebuildPending() {
    pending_dirty_ = false;
    for (int k = 0; k < 4; ++k) {
        pend_min_[k] = 0;
        pend_count_[k] = 0;
    }
    pend_frame_limit_ = 0;
    for (const Action& a : actions_) {
        if (a.applied) continue;
        ++action_scans_;  // 可观测：这里是**真正的逐条比较**（每次动作集变化才发生）
        const int k = static_cast<int>(a.trigger.kind);
        if (pend_count_[k] == 0 || a.trigger.value < pend_min_[k]) pend_min_[k] = a.trigger.value;
        ++pend_count_[k];
        if (a.trigger.kind == TriggerKind::Frame) {
            const std::uint64_t lim = a.trigger.stall_ms != 0 ? a.trigger.stall_ms : default_stall_ms_;
            if (lim != 0 && (pend_frame_limit_ == 0 || lim < pend_frame_limit_)) pend_frame_limit_ = lim;
        }
    }
}

bool InputScript::tick(WindowManager& wm, std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns,
                       std::uint64_t wall_ms) {
    bool any = false;
    ++tick_count_;
    const std::uint32_t time_ms = static_cast<std::uint32_t>(guest_ms);
    noteFrameForAverage(frame, wall_ms);
    if (pending_dirty_) rebuildPending();

    // ★ 帧停滞兜底：只在"还有 frame 条件在等"且"帧号真的停住"时触发一次。
    //   ⛔ 帧正常推进时这一段**一次也不触发**（CTest 的 `stall-not-fired` 用例钉着）。
    //   ★ `limit` 走缓存（与"逐条扫描取最小非 0 阈值"等价）：没有 pending Frame 动作时 limit 恒 0
    //     ⇒ 整个分支可以整段跳过。
    if (!stall_fired_ && pend_count_[static_cast<int>(TriggerKind::Frame)] != 0) {
        const std::uint64_t limit = pend_frame_limit_;
        if (limit != 0 && have_frame_sample_) {
            // ★ 还没出现**第一帧**（boot）不算"停滞"：那一段本来就没有 Present。
            //   只有"曾经有帧、后来停住"才兜底（⚠ 否则长 boot 会被误判成停滞）。
            const bool stalled = saw_frame_ever_
                                     ? (wall_ms > last_frame_change_wall_ms_ + limit)
                                     : (default_boot_grace_ms_ != 0 &&
                                        wall_ms > default_boot_grace_ms_);
            if (stalled) {
                stall_fired_ = true;
                const std::uint64_t avg =
                    frame_interval_count_ == 0 ? 16ull : frame_interval_sum_ms_ / frame_interval_count_;
                std::fprintf(stderr,
                             "[input-script] ⚠ 帧停滞 %llums（n=%llu）⇒ 剩余 frame 触发条件按最近帧间隔均值 "
                             "(%llums/帧) 折算成 wall_ms 继续（⛔ 不挂死）；详见 README §4.2c\n",
                             static_cast<unsigned long long>(wall_ms - last_frame_change_wall_ms_),
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(avg));
                convertFrameTriggersToWall(frame, wall_ms);
                any = true;
            }
        }
    }

    if (pending_dirty_) rebuildPending();
    // ★ 快路径（O(1)）：四个单位各查一条"最小到期值" ⇒ 没有一条到期就直接返回。
    //   ⛔ 只跳过**扫描**，不跳过任何触发：到期的那一条走下面原来的逐条 `due()` 路径。
    const bool any_due =
        (pend_count_[static_cast<int>(TriggerKind::Frame)] != 0 &&
         frame >= pend_min_[static_cast<int>(TriggerKind::Frame)]) ||
        (pend_count_[static_cast<int>(TriggerKind::GuestMs)] != 0 &&
         guest_ms >= pend_min_[static_cast<int>(TriggerKind::GuestMs)]) ||
        (pend_count_[static_cast<int>(TriggerKind::Insn)] != 0 &&
         insns >= pend_min_[static_cast<int>(TriggerKind::Insn)]) ||
        (pend_count_[static_cast<int>(TriggerKind::WallMs)] != 0 &&
         wall_ms >= pend_min_[static_cast<int>(TriggerKind::WallMs)]);
    // 收口与延迟抬起也必须保住：两者都只在"有活可干"时才有意义。
    const bool finish_pending = !finish_logged_ && (finished() || quit_requested_);
    if (!any && !any_due && releases_.empty() && !finish_pending) return false;

    // ★ 延迟抬起**先**处理：它属于**上一帧 / 上一次按下**。若排在本 tick 的显式动作之后，
    //   同一帧里"上一 click 的 up + 本帧显式 mouse_down"会被读成一次 up（刚按下就被清掉）。
    //   两种到期判据见 `releaseDue`（显式 hold = 帧 / ms；自动 = guest 采样 + 一个周期 / 上限）。
    for (std::size_t i = 0; i < releases_.size();) {
        bool bounded = false;
        if (!releaseDue(releases_[i], wm, frame, guest_ms, &bounded)) {
            ++i;
            continue;
        }
        const Release r = releases_[i];
        releases_.erase(releases_.begin() + static_cast<std::ptrdiff_t>(i));
        emitRelease(wm, r, frame, guest_ms, time_ms, bounded);
        dropPress(r.is_key ? r.vk : mousePressId(r.button));
        any = true;
    }
    for (Action& a : actions_) {
        if (a.applied) continue;
        ++action_scans_;  // 可观测：到期的那一次才真正逐条比较
        if (quit_requested_) continue;
        if (!a.trigger.due(frame, guest_ms, insns, wall_ms)) continue;
        a.applied = true;
        ++applied_;
        markPendingDirty();
        std::printf("[input-script] frame=%llu guest_ms=%llu insns=%llu %s\n",
                    static_cast<unsigned long long>(frame), static_cast<unsigned long long>(guest_ms),
                    static_cast<unsigned long long>(insns), a.text.c_str());
        apply(wm, a, frame, guest_ms);
        any = true;
    }
    // ★ 收口（动作跑完 / `quit`）：把仍按着的键 / 按钮**按同一组 WM_* 抬起**再交还所有权。
    //   ⛔ 只清状态不发消息的话，guest 的 WndProc 收不到 WM_KEYUP / WM_LBUTTONUP（事件锁存会永远
    //   停在"按下"）；`quit` 时未到期的 release 也在这里一并抬起。
    if (!finish_logged_ && (finished() || quit_requested_)) {
        finish_logged_ = true;
        // ⛔ 未到期的 release 必须先清掉：否则收口补发一次 UP 之后，后续 tick 走到 due 会**再发一次**。
        releases_.clear();
        presses_.clear();
        releaseOwnedInputs(wm, time_ms);
        wm.releaseScriptOwnership();
        std::printf("[input-script] finished：%zu 条动作全部应用（脚本按下的键 / 按钮已抬起；"
                    "键 / 按钮 / 光标所有权交还 OS）\n",
                    actions_.size());
        // ★ 可观测账（见 `ReleaseStats`）：这是判断"这次点击到底有没有跨过采样"的入口。
        std::printf("[input-script] release  : total=%llu auto=%llu explicit=%llu deferred-ups=%llu "
                    "bounded-no-sample=%llu waited-samples-max=%llu hold-ms-max=%llu\n",
                    static_cast<unsigned long long>(stats_.releases),
                    static_cast<unsigned long long>(stats_.auto_releases),
                    static_cast<unsigned long long>(stats_.explicit_releases),
                    static_cast<unsigned long long>(stats_.deferred_ups),
                    static_cast<unsigned long long>(stats_.bounded_no_sample),
                    static_cast<unsigned long long>(stats_.waited_samples_max),
                    static_cast<unsigned long long>(stats_.hold_ms_max));
    }
    return any;
}

void InputScript::abandon(WindowManager& wm, std::uint32_t time_ms) {
    if (!active() && !quit_requested_) return;
    releaseOwnedInputs(wm, time_ms);
    wm.releaseScriptOwnership();
    clear();
}

void InputScript::releaseOwnedInputs(WindowManager& wm, std::uint32_t time_ms) {
    int x = 0;
    int y = 0;
    wm.cursorClientPos(wm.primaryWindow(), &x, &y);
    // 先按钮（它同时清 VK 电平），再剩下的键。
    for (std::size_t i = 0; i < kMouseButtonCount; ++i) {
        const MouseButton b = static_cast<MouseButton>(i);
        if (!wm.mouseButtonOwnedByScript(b)) continue;
        std::printf("[input-script]   release-at-finish: mouse %s\n", buttonName(b));
        input::mouseButton(wm, x, y, b, false, InputSource::Script, time_ms);
    }
    for (std::uint32_t vk = 1; vk < 256u; ++vk) {
        if (!wm.keyOwnedByScript(vk)) continue;
        std::printf("[input-script]   release-at-finish: key 0x%02X\n", vk);
        input::key(wm, vk, false, 0u, InputSource::Script, time_ms);
    }
}

}  // namespace lift
