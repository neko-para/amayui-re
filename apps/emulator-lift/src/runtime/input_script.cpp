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

}  // namespace

void InputScript::clear() {
    source_.clear();
    actions_.clear();
    releases_.clear();
    applied_ = 0;
    quit_requested_ = false;
    quit_posted_ = false;
    finish_logged_ = false;
    have_frame_anchor_ = false;
    have_ms_anchor_ = false;
    anchor_frame_ = 0;
    anchor_ms_ = 0;
    have_pos_ = false;
    last_x_ = 0;
    last_y_ = 0;
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

            // ---- 时间线锚点 / 相对推进 ----
            if (head == "at" || head == "after" || head == "wait") {
                if (tok.size() != 3) return fail(line_no, body + " ⇒ 需要 3 个词（如 `at frame 600`）");
                // ★ 词序不同：`at <单位> <值>`（at frame 600 / at guest_ms 2500），而
                //   `after|wait <值> <单位>`（after 30 frames / wait 2 ms）——按 Win32 记法的习惯。
                const bool at_form = (head == "at");
                const std::string unit = lowerCopy(at_form ? tok[1] : tok[2]);
                std::uint64_t value = 0;
                if (!parseU64(at_form ? tok[2] : tok[1], &value)) {
                    return fail(line_no, "不是合法数字：" + (at_form ? tok[2] : tok[1]));
                }
                if (at_form) {
                    if (unit == "frame") {
                        trigger = Trigger{false, value};
                        have_frame_anchor_ = true;
                        anchor_frame_ = value;
                    } else if (unit == "guest_ms" || unit == "ms") {
                        trigger = Trigger{true, value};
                        have_ms_anchor_ = true;
                        anchor_ms_ = value;
                    } else {
                        return fail(line_no, "`at` 只认 frame / guest_ms（收到 " + tok[1] + "）");
                    }
                } else {
                    const bool by_ms = (unit == "ms" || unit == "guest_ms");
                    if (unit != "frame" && unit != "frames" && !by_ms) {
                        return fail(line_no, "`" + head + "` 只认 frames / ms（收到 " + unit + "）");
                    }
                    if (by_ms) {
                        if (!have_ms_anchor_) {
                            return fail(line_no, "`" + head + " " + tok[1] + " ms` 需要一个毫秒基准（先写 `at guest_ms`）");
                        }
                        anchor_ms_ += value;
                        trigger = Trigger{true, anchor_ms_};
                    } else {
                        if (!have_frame_anchor_) {
                            return fail(line_no,
                                        "`" + head + " " + tok[1] + " frames` 需要一个帧基准（先写 `at frame`）");
                        }
                        anchor_frame_ += value;
                        trigger = Trigger{false, anchor_frame_};
                    }
                    // ★ `wait` 同时是一个**动作**（no-op）：脚本要等它到期才收口 ⇒ 尾部 `wait` 能"保持按住"。
                    //   `after` 只是给下一条动作换锚点（自己不产生动作）。
                    if (head == "wait") {
                        Action w;
                        w.kind = Kind::Wait;
                        w.by_ms = trigger.by_ms;
                        w.at_frame = trigger.by_ms ? 0u : trigger.value;
                        w.at_ms = trigger.by_ms ? trigger.value : 0u;
                        w.text = body;
                        actions_.push_back(w);
                    }
                    // ⚠ `wait` 与 `after` 的锚点语义**相同**：两者都推进锚点并把它作为下一条动作的
                    //    触发时间（区别只是 `wait` 自己不产生动作）。⛔ 不要在这里 `continue` —— 那会
                    //    让 `wait 30 frames` 之后的动作落回**上一个**触发时间（一次全在旧帧触发）。
                }
                have_trigger = true;
                continue;
            }

            // ---- 动作 ----
            if (!have_trigger) {
                return fail(line_no, body + " ⇒ 动作前必须先给时间（`at frame N` / `at guest_ms T`）");
            }
            Action a;
            a.by_ms = trigger.by_ms;
            a.at_frame = trigger.by_ms ? 0u : trigger.value;
            a.at_ms = trigger.by_ms ? trigger.value : 0u;
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
                if (tok.size() < 3) return fail(line_no, "click 需要 X Y [left|right|middle] [hold N]");
                std::uint64_t x = 0;
                std::uint64_t y = 0;
                if (!parseU64(tok[1], &x) || !parseU64(tok[2], &y)) return fail(line_no, "坐标不是合法数字");
                MouseButton b = MouseButton::Left;
                std::uint64_t hold = 1;
                for (std::size_t i = 3; i < tok.size(); ++i) {
                    if (lowerCopy(tok[i]) == "hold") {
                        if (i + 1 >= tok.size() || !parseU64(tok[i + 1], &hold)) {
                            return fail(line_no, "`hold` 后面需要帧数");
                        }
                        ++i;
                    } else if (!parseButton(tok[i], &b)) {
                        return fail(line_no, "未知参数：" + tok[i]);
                    }
                }
                a.kind = Kind::Click;
                a.x = static_cast<int>(x);
                a.y = static_cast<int>(y);
                a.button = b;
                a.hold_frames = static_cast<std::uint32_t>(hold < 1 ? 1 : hold);
                last_x_ = a.x;
                last_y_ = a.y;
                have_pos_ = true;
            } else if (head == "key_down" || head == "key_up") {
                if (tok.size() != 2) return fail(line_no, head + " 需要一个 VK（1..255）");
                std::uint64_t vk = 0;
                if (!parseU64(tok[1], &vk) || vk == 0 || vk > 255) return fail(line_no, "VK 必须是 1..255");
                a.kind = head == "key_down" ? Kind::KeyDown : Kind::KeyUp;
                a.vk = static_cast<std::uint32_t>(vk);
            } else if (head == "quit" || head == "exit") {
                if (tok.size() != 1) return fail(line_no, head + " 不带参数");
                a.kind = Kind::Quit;
            } else {
                return fail(line_no, "未知指令：" + tok[0]);
            }
            actions_.push_back(a);
        }
    }

    if (actions_.empty()) return fail(line_no, "脚本里没有任何动作（只有注释 / 锚点）");
    return true;
}

void InputScript::apply(WindowManager& wm, Action& a, std::uint64_t frame, std::uint32_t time_ms) {
    switch (a.kind) {
        case Kind::MouseMove:
            input::mouseMove(wm, a.x, a.y, InputSource::Script, time_ms);
            break;
        case Kind::MouseDown:
            input::mouseButton(wm, a.x, a.y, a.button, true, InputSource::Script, time_ms);
            break;
        case Kind::MouseUp:
            input::mouseButton(wm, a.x, a.y, a.button, false, InputSource::Script, time_ms);
            break;
        case Kind::KeyDown:
            input::key(wm, a.vk, true, 0u, InputSource::Script, time_ms);
            break;
        case Kind::KeyUp:
            input::key(wm, a.vk, false, 0u, InputSource::Script, time_ms);
            break;
        case Kind::Click: {
            input::mouseMove(wm, a.x, a.y, InputSource::Script, time_ms);
            input::mouseButton(wm, a.x, a.y, a.button, true, InputSource::Script, time_ms);
            Release r;
            r.due_frame = frame + a.hold_frames;
            r.x = a.x;
            r.y = a.y;
            r.button = a.button;
            releases_.push_back(r);
            std::printf("[input-script]   click-release scheduled: frame=%llu at=(%d,%d) %s\n",
                        static_cast<unsigned long long>(r.due_frame), r.x, r.y, buttonName(r.button));
            break;
        }
        case Kind::Wait:
            // no-op：它的作用只是让"脚本还没收口"持续到这个时刻（apply 里什么都不做）。
            break;
        case Kind::Quit:
            quit_requested_ = true;
            break;
    }
}

bool InputScript::tick(WindowManager& wm, std::uint64_t frame, std::uint64_t now_100ns) {
    bool any = false;
    const std::uint64_t now_ms = msFrom100ns(now_100ns);
    const std::uint32_t time_ms = static_cast<std::uint32_t>(now_ms);
    // ★ 延迟抬起（click）**先**处理：它属于**上一帧**的点击。若排在本 tick 的显式动作之后，
    //   同一帧里"上一 click 的 up + 本帧显式 mouse_down"会被读成一次 up（刚按下就被清掉）。
    for (std::size_t i = 0; i < releases_.size();) {
        if (frame < releases_[i].due_frame) {
            ++i;
            continue;
        }
        const Release r = releases_[i];
        releases_.erase(releases_.begin() + static_cast<std::ptrdiff_t>(i));
        std::printf("[input-script] frame=%llu guest_ms=%llu click-release at=(%d,%d) %s\n",
                    static_cast<unsigned long long>(frame), static_cast<unsigned long long>(now_ms), r.x, r.y,
                    buttonName(r.button));
        input::mouseButton(wm, r.x, r.y, r.button, false, InputSource::Script, time_ms);
        any = true;
    }
    for (Action& a : actions_) {
        if (a.applied || quit_requested_) continue;
        const bool due = a.by_ms ? (now_ms >= a.at_ms) : (frame >= a.at_frame);
        if (!due) continue;
        a.applied = true;
        ++applied_;
        std::printf("[input-script] frame=%llu guest_ms=%llu %s\n", static_cast<unsigned long long>(frame),
                    static_cast<unsigned long long>(now_ms), a.text.c_str());
        apply(wm, a, frame, time_ms);
        any = true;
    }
    // ★ 收口（动作跑完 / `quit`）：把仍按着的键 / 按钮**按同一组 WM_* 抬起**再交还所有权。
    //   ⛔ 只清状态不发消息的话，guest 的 WndProc 收不到 WM_KEYUP / WM_LBUTTONUP（事件锁存会永远
    //   停在"按下"）；`quit` 时 click 未到期的 release 也在这里一并抬起。
    if (!finish_logged_ && (finished() || quit_requested_)) {
        finish_logged_ = true;
        // ⛔ 未到期的 click-release 必须先清掉：否则收口补发一次 UP 之后，后续 tick 走到 due 会**再发一次**。
        releases_.clear();
        releaseOwnedInputs(wm, time_ms);
        wm.releaseScriptOwnership();
        std::printf("[input-script] finished：%zu 条动作全部应用（脚本按下的键 / 按钮已抬起；"
                    "键 / 按钮 / 光标所有权交还 OS）\n",
                    actions_.size());
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
