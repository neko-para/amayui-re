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

// ---- marker 名字比对 ----
// ASCII 大小写不敏感（TOC 名字的大小写未取证，脚本作者也不该被大小写卡住）。
// ⛔ 不做 locale / Unicode 折叠：两个都来自 UTF-8 的 ASCII 段时这条就是全部规则。
bool asciiEqualsIgnoreCase(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return true;
}

std::string degradedIdName(std::uint32_t id) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "id:0x%08X", id);
    return std::string(buf);
}

// 两种形态**始终都接受**：解析出的名字 / `id:0x%08X`。
//   ★ 不对称会让"名字表某个时刻读不到"变成一条静默失明的链（父级裁决 2：降级仍按 id 触发）。
bool markerNameMatches(const std::string& want, const MarkerSample& s) {
    if (asciiEqualsIgnoreCase(want, s.name)) return true;
    return asciiEqualsIgnoreCase(want, degradedIdName(s.id));
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
        case TriggerKind::Marker: return "marker";
    }
    return "?";
}

const char* triggerUnitName(TriggerKind k) { return triggerKindName(k); }

// ---- ★ `--anchor-script` 的启动期校验（⛔ 不跑一半就退化成"0 点从 run 起算"）----
//   两条判据的**唯一实现**：`main` 用它 exit 2，pure CTest 用它断言"互斥 ⇒ 解析期报错"。
std::string validateOriginCli(bool anchor_requested, bool input_script_given, bool interaction_graph_given) {
    if (!anchor_requested) return std::string();
    // ① 与交互图**互斥**：图有自己的 actuator 与**绝对触发**（`at frame <F+1>`），
    //    再叠一个 0 点会把它的绝对触发整体抬 `origin.frame` 帧（语义直接对不上）。
    if (interaction_graph_given) {
        return "--anchor-script 与 --interaction-graph 互斥：图有自己的 actuator 与**绝对触发**"
               "（0 点重定基会把它们整体抬起）⇒ 二选一";
    }
    // ② 只有时间线认 0 点 ⇒ 没给 `--input-script` 时它是无意义的配置（⛔ 不静默接受）。
    if (!input_script_given) {
        return "--anchor-script 需要同时给 --input-script（它给的是那条时间线的 0 点）";
    }
    return std::string();
}

std::string validateOriginScript(bool anchor_requested, std::size_t action_count) {
    if (!anchor_requested) return std::string();
    // ③ 一个动作都没有 ⇒ 0 点没有可重定基的东西（解析器本身也会拒绝空脚本；这里是那道判据）
    if (action_count == 0) {
        return "--anchor-script 给了，但脚本里没有任何动作（0 点没有可重定基的触发 ⇒ 配置无意义）";
    }
    return std::string();
}

bool Trigger::due(std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns,
                  std::uint64_t wall_ms) const {
    switch (kind) {
        case TriggerKind::Frame: return frame >= value;
        case TriggerKind::GuestMs: return guest_ms >= value;
        case TriggerKind::Insn: return insns >= value;
        case TriggerKind::WallMs: return wall_ms >= value;
        // ★ Marker **不按数字到期**：它只由 `noteMarker()` 在边沿上应用。
        //   ⛔ 不要把它算进 `tick()` 的到期快路径（那会让它每一 tick 都触发）。
        case TriggerKind::Marker: return false;
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
    close_window_pending_ = false;
    finish_logged_ = false;
    for (int i = 0; i < kTriggerKindCount; ++i) have_anchor_[i] = false;
    anchor_frame_ = 0;
    anchor_guest_ms_ = 0;
    anchor_insn_ = 0;
    anchor_wall_ms_ = 0;
    have_pos_ = false;
    last_x_ = 0;
    last_y_ = 0;
    profile_actions_ = 0;
    // ★ marker：动作数 + 边沿 / 超时账（超时**配置** `marker_timeout_ms_` 不随脚本清空）。
    marker_actions_ = 0;
    resetMarkerState();
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

// ---- ★ 未应用动作 / 未完成抬起（run summary 的 `input-script: unfinished` 行读这三条）----
//   见 `input_script.h` 的声明处：它们只回答"还剩什么没跑"，⛔ 不改任何状态。
std::size_t InputScript::unfinishedActionCount() const {
    // `applied_` 正常恒 ≤ size（只用 push_back 追加）；越界按 0 处理，⛔ 不让减法下溢成大数。
    return applied_ < actions_.size() ? actions_.size() - applied_ : 0;
}

const std::string& InputScript::firstUnfinishedActionText() const {
    static const std::string kEmpty;
    // ★ ⛔ 不能取 `actions_[applied_]`：动作是**按到期 / 边沿**应用的（`tick()` 逐条判到期就应用、
    //   `noteMarker()` 按边沿应用）⇒ 应用顺序**不一定**是文件顺序，`applied_` 只是**计数**。
    //   实测反例：`at frame 50 / mouse_move 10 10 / at insn 30 / mouse_move 20 20` ⇒ 后一条先应用，
    //   `actions_[applied_]` 会指向**已应用**的 `20 20`。所以只能扫 `applied` 标志
    //   （与 `updateMarkerTimeout()` 找"文件序第一条未应用 Marker 动作"同一写法）。
    for (const Action& a : actions_) {
        if (!a.applied) return a.text;
    }
    return kEmpty;
}

const std::string& InputScript::firstPendingReleaseText() const {
    static const std::string kEmpty;
    return releases_.empty() ? kEmpty : releases_.front().text;
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
    // ★ 段标记（`REQ-01M710E6PV4PPHECPPCCTRF5RA`）：`before-anchor` 之后的触发是**绝对**的
    //   （不参与 `+= origin`、闩锁前不被抑制），`after-anchor` 之后恢复**相对**（= 今天的语义）。
    //   ⛔ 两者**必须成对**（父级裁决）：缺 `after-anchor` ⇒ 解析期错误 —— "绝对到文件尾"与
    //     "绝对止于闩锁"两种隐式边界都自圆其说，而脚本字面看不出来 ⇒ 不猜（静默歧义）。
    //   ⛔ 没有 `--anchor-script` 时两者都是解析期错误（否则它们静默无害 ⇒ 用户以为自己在用 0 点）。
    bool in_before_anchor = false;
    bool saw_before_anchor = false;
    int before_anchor_line = 0;
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

            // ---- 段标记（`before-anchor` / `after-anchor`；各自**独占一行**）----
            //   ★ 判据（四条解析期错误，全部带行号）：
            //     ① 没开 `--anchor-script` 却写了段标记；② 重复 `before-anchor`；
            //     ③ `after-anchor` 出现在 `before-anchor` 之前；④ `before-anchor` 段里出现 profiler 动作。
            if (head == "before-anchor" || head == "after-anchor") {
                if (tok.size() != 1) {
                    return fail(line_no, body + " ⇒ 段标记必须**独占一行**（不带参数 / 不跟在别的词后面）");
                }
                if (!origin_requested_) {
                    return fail(line_no, "`" + head + "` 需要 `--anchor-script <id>`（段标记只在 0 点存在时才有意义）");
                }
                if (head == "before-anchor") {
                    if (saw_before_anchor) {
                        return fail(line_no, "重复的 `before-anchor`（一条脚本只能有一个绝对段开头）");
                    }
                    saw_before_anchor = true;
                    before_anchor_line = line_no;
                    in_before_anchor = true;
                } else {
                    if (!saw_before_anchor) {
                        return fail(line_no, "`after-anchor` 出现在 `before-anchor` 之前（没有绝对段可以先结束）");
                    }
                    in_before_anchor = false;
                }
                continue;
            }

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
                    return fail(line_no, body + " ⇒ 需要 3 个词（如 `at frame 600` / `at marker LOGO.BIN`）");
                }
                // ★ 词序不同：`at <单位> <值>`（at frame 600 / at insn 5000 / at marker 名字），而
                //   `after|wait <值> <单位>`（after 30 frames / wait 2 ms）——按 Win32 记法的习惯。
                const bool at_form = (head == "at");
                const std::string unit = lowerCopy(at_form ? core[1] : core[2]);
                // ★ `marker` **不是**数字单位（⛔ 不进 `parseTriggerUnit`：那会污染
                //   `click … hold N` 的单位解析，hold 只认 frames / ms）。
                //   只在 `at` 分支单独吃一个名字 token；`after` / `wait` 没有意义（没有数值到期）。
                const bool marker_word = (lowerCopy(core[1]) == "marker" || lowerCopy(core[2]) == "marker");
                if (marker_word) {
                    if (!at_form || unit != "marker") {
                        return fail(line_no,
                                    "`marker` 只能写成 `at marker <名字>`（边沿触发、不按数字到期；"
                                    "`after` / `wait` 没有意义）");
                    }
                    if (have_stall) {
                        return fail(line_no, "`at marker` 不接受 `stall`（它只对 frame 有意义）");
                    }
                    trigger = Trigger{};
                    trigger.kind = TriggerKind::Marker;
                    trigger.name = core[2];
                    have_trigger = true;
                    continue;
                }
                std::uint64_t value = 0;
                if (!parseU64(at_form ? core[2] : core[1], &value)) {
                    return fail(line_no, "不是合法数字：" + (at_form ? core[2] : core[1]));
                }
                TriggerKind kind = TriggerKind::Frame;
                if (!parseTriggerUnit(unit, &kind)) {
                    return fail(line_no, "`" + head + "` 只认 frame(s) / ms / guest_ms / insn(s) / wall_ms；"
                                             "marker 只能写成 `at marker <名字>`（收到 " + unit + "）");
                }
                std::uint64_t* anchor = nullptr;
                bool* have = &have_anchor_[static_cast<int>(kind)];
                switch (kind) {
                    case TriggerKind::Frame: anchor = &anchor_frame_; break;
                    case TriggerKind::GuestMs: anchor = &anchor_guest_ms_; break;
                    case TriggerKind::Insn: anchor = &anchor_insn_; break;
                    case TriggerKind::WallMs: anchor = &anchor_wall_ms_; break;
                    case TriggerKind::Marker:
                        // 上面已单独处理（`marker_word`）⇒ 走到这里是矛盾；响亮失败，⛔ 不猜。
                        return fail(line_no, "内部错误：Marker 不该走到数值触发分支");
                }
                trigger.name.clear();  // ⛔ 上一条 `at marker X` 的名字不许漏进后面的数值触发
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
                    w.absolute = in_before_anchor;  // 段标记决定它属于绝对段还是相对段
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
            a.absolute = in_before_anchor;  // 段标记：`before-anchor` 之后 = 绝对（不重定基 / 闩锁前不抑制）

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
                //   ⛔ 绝对段里禁止 profiler：它的窗口边界读的是"从 run 起算"的时刻，而在 0 点
                //     之前那些时刻**不是同一把尺子**（0 点之后才与 guest 时间对齐）⇒ 会产出一张
                //     跨着两把尺子的表。解析期就拒绝（⛔ 不静默产出一张不可读的表）。
                if (in_before_anchor) {
                    return fail(line_no, "`" + head + "` 不能出现在 `before-anchor` 段里（0 点之前的时刻与"
                                             " 0 点之后不是同一把尺子 ⇒ 窗口边界没有意义）");
                }
                if (tok.size() != 2) return fail(line_no, head + " 需要一个 label（单个词）");
                a.kind = head == "profile_begin" ? Kind::ProfileBegin : Kind::ProfileEnd;
                a.label = tok[1];
            } else if (head == "quit" || head == "exit") {
                if (tok.size() != 1) return fail(line_no, head + " 不带参数");
                a.kind = Kind::Quit;
            } else if (head == "close_window") {
                if (tok.size() != 1) return fail(line_no, head + " 不带参数");
                a.kind = Kind::CloseWindow;
            } else {
                return fail(line_no, "未知指令：" + tok[0]);
            }
            if (a.kind == Kind::ProfileBegin || a.kind == Kind::ProfileEnd) ++profile_actions_;
            if (a.trigger.kind == TriggerKind::Marker) ++marker_actions_;
            actions_.push_back(a);
        }
    }

    // ★ 段标记**必须成对**（父级裁决）：`before-anchor` 到文件尾都没有 `after-anchor` ⇒ 解析期错误。
    //   ⛔ 两种"隐式边界"（绝对到文件尾 / 绝对止于闩锁）都自圆其说，而脚本字面看不出来
    //      ⇒ 不猜（本仓反复吃过的"静默歧义"）。报错指回 `before-anchor` 那一行。
    if (in_before_anchor) {
        return fail(before_anchor_line,
                    "`before-anchor` 必须配 `after-anchor`（到文件尾都没有配对）—— "
                    "绝对段在哪结束必须显式写出来");
    }
    if (actions_.empty()) return fail(line_no, "脚本里没有任何动作（只有注释 / 锚点）");
    pending_dirty_ = true;  // 动作集变了 ⇒ 聚合缓存作废
    return true;
}

bool InputScript::appendText(const std::string& text, const std::string& origin, std::string* err) {
    // 报错前缀：`origin` 是"哪一份文本"，`: append:` 说明是追加而不是首次解析
    //（`loadText` 的报错仍是 `origin:行: 原因`，两者一眼可分）。
    const std::string where = origin.empty() ? std::string("append") : origin + ": append";
    // ---- 前置 ①：现有动作全部已应用 ----
    if (applied_ != actions_.size()) {
        if (err) {
            *err = where + ": 现有动作还没全部应用（applied=" + std::to_string(applied_) + "/" +
                   std::to_string(actions_.size()) +
                   "）—— 追加会让新动作排在未应用动作之间，应用顺序就不再等于文件顺序";
        }
        return false;
    }
    // ---- 前置 ②：没有未完成的抬起 ----
    if (!releases_.empty()) {
        if (err) {
            *err = where + ": 还有 " + std::to_string(releases_.size()) +
                   " 个未完成的抬起（等 guest 采样 / 等 hold 到期）—— 现在按下会被上一次的 up 立刻清掉";
        }
        return false;
    }
    // ---- 前置 ②'：脚本已经收口（quit）⇒ 追加的动作**永远不会被应用** ----
    //   ⛔ 不静默接受：那会让调用方以为"动作排上了"，而 tick 里的 `if (quit_requested_) continue`
    //   一条都不会执行。这是"吵赢静默"的同一口径（见 input_script.h 顶部）。
    if (quit_requested_) {
        if (err) *err = where + ": 脚本已经请求 quit（追加的动作不会被应用）";
        return false;
    }
    // ---- 解析到临时脚本：复用 loadText 的全部语法与报错（⛔ 不复制一份解析器）----
    InputScript tmp;
    // ★ 段标记的解析期判据依赖"有没有 0 点配置"（`before-anchor` / `after-anchor` 需要它）⇒
    //   把本实例的配置交给临时解析器（⛔ 否则追加一段合法文本会因为"临时实例没配置"而报错）。
    tmp.origin_requested_ = origin_requested_;
    tmp.origin_id_ = origin_id_;
    if (!tmp.loadText(text, origin, err)) return false;
    // ---- 前置 ③：同单位单调（新动作的触发值不许早于现有最后一条同单位动作）----
    //   ★ Marker **跳过**这条检查：名字之间没有大小关系（`last_value` 对它没有意义）。
    std::uint64_t last_value[kTriggerKindCount] = {};
    bool have_unit[kTriggerKindCount] = {};
    for (const Action& a : actions_) {
        if (a.trigger.kind == TriggerKind::Marker) continue;
        const int k = static_cast<int>(a.trigger.kind);
        last_value[k] = a.trigger.value;  // 文件顺序 ⇒ 最后一条同单位的
        have_unit[k] = true;
    }
    for (const Action& a : tmp.actions_) {
        if (a.trigger.kind == TriggerKind::Marker) continue;
        const int k = static_cast<int>(a.trigger.kind);
        if (have_unit[k] && a.trigger.value < last_value[k]) {
            if (err) {
                *err = where + ": 触发值倒退（`" + a.text + "` 的 " + triggerUnitName(a.trigger.kind) + "=" +
                       std::to_string(a.trigger.value) + " 早于现有最后一条同单位动作的 " +
                       std::to_string(last_value[k]) + "）—— 倒序会让应用顺序与文件顺序不一致";
            }
            return false;
        }
    }
    // ---- 追加（⛔ 不 clear：已应用的账、按下记录、抬起账全部保留）----
    if (source_.empty()) source_ = origin;
    for (const Action& a : tmp.actions_) actions_.push_back(a);
    profile_actions_ += tmp.profile_actions_;
    marker_actions_ += tmp.marker_actions_;
    // 又有了活 ⇒ 收口那一遍（抬键 + 交还所有权 + 日志）要能再走一次。
    finish_logged_ = false;
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

void InputScript::applyAction(WindowManager& wm, Action& a, std::uint64_t frame, std::uint64_t guest_ms) {
    // ★ 一条动作只应用一次：账在这里记（tick 与 noteMarker 共用同一条路径；⛔ 裸调 apply()）。
    a.applied = true;
    ++applied_;
    markPendingDirty();
    apply(wm, a, frame, guest_ms);
}

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
        case Kind::CloseWindow:
            // 只置位；真正的 `PresentSink::requestClose()` 由 `Machine` 在同一块边界交付
            // （`apply()` 只拿得到 WindowManager，拿不到呈现后端）。
            close_window_pending_ = true;
            break;
    }
}

// ---- ★ 时间线的 0 点（`--anchor-script <id>`）----
//   语义：**首次观察到进入**脚本 `id` 的那一个有效样本 ⇒ 定基一次（闩锁），把未应用的数值触发
//   整体 `+= origin`。⛔ 不是 marker 的边沿：首样本也要判（"一开局就在 X 里"是合法的 0 点），
//   重进**不**重新定基（0 点只定一次）。
bool InputScript::noteIdentity(const MarkerSample& s, std::uint64_t frame, std::uint64_t guest_ms,
                               std::uint64_t insns, std::uint64_t wall_ms) {
    if (!origin_requested_) return false;
    if (!s.valid) return false;  // 无效样本（boot / cur 越界）不定义 0 点 —— ⛔ 不猜
    if (!origin_set_) {
        // 电平条件 + 首次：当前样本的 id 就是目标 ⇒ 立刻闩锁（⛔ 不要求"上一个值不同"）。
        if (s.id != origin_id_) {
            origin_reentry_seen_ = false;  // 离开目标段 ⇒ 下次再进要能再记一行 info
            return false;
        }
        origin_set_ = true;
        origin_.id = s.id;
        origin_.frame = frame;
        origin_.guest_ms = guest_ms;
        origin_.insns = insns;
        origin_.wall_ms = wall_ms;
        ++origin_latches_;
        origin_reentry_seen_ = true;
        // ★ 重定基**与抑制必须同时存在**（只重定基 ⇒ 闩锁前的 `at frame 1` 早跑了；
        //   只抑制 ⇒ 闩锁后触发值仍是"从 run 起算"）。抑制在 `tick()` 里按 `originSet()` 关掉。
        rebasePendingTriggers();
        // ★ 必须打这一行：重定基之后日志里 `at frame 120` 可能"在第 380 帧应用"（设计 §B'）。
        // ★ `sample=` = 这一闩锁发生在**第几个有效样本**上（`marker_samples_` 已在 `noteMarker`
        //   入口自增）⇒ 换档 / 换机器后，"0 点是不是同一个采样序"有得对；四单位只对数值没有序号。
        std::printf("[input-script] origin script=0x%08X cur=%u sample=%llu frame=%llu guest_ms=%llu "
                    "insns=%llu wall_ms=%llu ⇒ 0 点已定（未应用的数值触发整体 +origin；只定一次）\n",
                    s.id, s.cur, static_cast<unsigned long long>(marker_samples_),
                    static_cast<unsigned long long>(frame), static_cast<unsigned long long>(guest_ms),
                    static_cast<unsigned long long>(insns), static_cast<unsigned long long>(wall_ms));
        return true;
    }
    // 已闩锁：**重进不定基**（0 点只定一次）。只在"再次进入目标 id"的**这一条边沿**上记 info。
    if (s.id == origin_.id) {
        if (!origin_reentry_seen_) {
            origin_reentry_seen_ = true;
            ++origin_reentries_;
            std::printf("[input-script] origin 重进 script=0x%08X cur=%u frame=%llu（⛔ 不重新定基："
                        "0 点仍是 frame=%llu insns=%llu）\n",
                        s.id, s.cur, static_cast<unsigned long long>(frame),
                        static_cast<unsigned long long>(origin_.frame),
                        static_cast<unsigned long long>(origin_.insns));
        }
    } else {
        origin_reentry_seen_ = false;
    }
    return false;
}

void InputScript::rebasePendingTriggers() {
    for (Action& a : actions_) {
        if (a.applied) continue;  // ⛔ 不撤已发生的事（已应用的账 / 注入消息都不动）
        // ★ **绝对段豁免**（`before-anchor`）：那些触发按声明的 frame / insn / guest_ms / wall_ms
        //   原样执行 —— 它们本来就是"0 点之前的输入"，`+= origin` 会把它们推到 0 点之后。
        if (a.absolute) continue;
        switch (a.trigger.kind) {
            case TriggerKind::Frame: a.trigger.value += origin_.frame; break;
            case TriggerKind::GuestMs: a.trigger.value += origin_.guest_ms; break;
            case TriggerKind::Insn: a.trigger.value += origin_.insns; break;
            case TriggerKind::WallMs: a.trigger.value += origin_.wall_ms; break;
            // ★ Marker 是**名字**条件（没有数值可平移）；它的边沿语义与 0 点无关（裁决 M3）。
            case TriggerKind::Marker: break;
        }
    }
    // ⚠ 显式 hold 的 `Release::due_value` **不动**：它是"apply 那一刻 + N"算出来的绝对量，
    //   而 apply 已经发生在重定基之后（`convertFrameTriggersToWall` 的先例同理）。
    pending_dirty_ = true;  // 触发值改了 ⇒ 聚合缓存作废
}

std::string InputScript::originMissedDetail(std::uint64_t frame, std::uint64_t wall_ms) const {
    // ★ 三个数必须打：等的是哪个 id、实际看到的是什么、跑了多久（与 `marker-timeout:` 同形）。
    //   措辞只能是"没**观察到**进入"：0 点定义在首个可观察样本上，宿主可能漏掉"进又出"。
    char buf[384];
    const char* last = marker_seeded_ ? marker_last_name_.c_str() : "none";
    std::snprintf(buf, sizeof(buf),
                  "origin-missed: script=0x%08X（没**观察到**进入脚本 0x%08X） last=%s frames=%llu "
                  "wall_ms=%llu samples=%llu invalid=%llu",
                  origin_id_, origin_id_, last, static_cast<unsigned long long>(frame),
                  static_cast<unsigned long long>(wall_ms), static_cast<unsigned long long>(marker_samples_),
                  static_cast<unsigned long long>(marker_invalid_));
    return std::string(buf);
}

void InputScript::resetOriginState() {
    // ★ **状态**（每轮 run 清）：闩锁位 / 四个单位 / 计数 / 重进去重。
    //   ⛔ 配置（`origin_requested_` / `origin_id_`）不在这里清 —— 它跨 clear() / 跨 run 存活。
    origin_set_ = false;
    origin_ = TimelineOrigin{};
    origin_latches_ = 0;
    origin_reentries_ = 0;
    origin_reentry_seen_ = false;
}

// ★ 缓存"未应用动作"的聚合量。动作集只在三处变：动作被 apply、frame 条件被折算成 wall、
//   脚本重载/清空 ⇒ 那三处置 dirty，tick 里至多重算一次。等价性判据：`due()` 就是 `now >= value`
//   ⇒ "该单位至少有一条到期" ⇔ `min(value) <= now`。
// ★ marker 边沿与事件（父级裁决 1/2）：值变才发；同名重复不重复发；首个样本只播种。
//   ★ 动作的应用走 `applyAction()`（与 tick 的到期路径**同一条**）—— 否则 applied / 账 /
//     缓存作废全漏，表现为"同一名字每边沿重复发 + 动作永远 pending + 超时误报"。
bool InputScript::noteMarker(WindowManager& wm, const MarkerSample& s, std::uint64_t frame,
                             std::uint64_t guest_ms, std::uint64_t insns, std::uint64_t wall_ms) {
    if (!s.valid) {
        ++marker_invalid_;
        return false;
    }
    ++marker_samples_;
    // ★ 0 点的闩锁判定**先于** marker 的播种 / 边沿逻辑（设计 L2）：
    //   播种那一支在首个样本就 `return false`（见下），把闩锁放在它之后会让
    //   "一开局就在目标脚本里"（`--skip-logo` 的形状）**永远等不到** 0 点。
    noteIdentity(s, frame, guest_ms, insns, wall_ms);
    // 同身份重复采样：名字一定没变 ⇒ ⛔ 不做一次 std::string 拷贝（采样点 = 每条主循环指令）。
    //   ⚠ 唯一例外：降级状态变了（TOC 从"未装载"变成"已装载"）⇒ 名字要刷新（否则 timeout 的 `last=` 会停在 `id:…`）。
    if (s.cur != marker_last_cur_ || s.id != marker_last_id_ || !marker_seeded_ ||
        s.name_degraded != marker_last_name_degraded_) {
        marker_last_cur_ = s.cur;
        marker_last_id_ = s.id;
        marker_last_name_ = s.name;
        marker_last_name_degraded_ = s.name_degraded;
    }
    if (!marker_seeded_) {
        // 首个有效样本只播种：没有"上一个值"就没有边沿（⛔ 不发事件）。
        marker_seeded_ = true;
        marker_cur_ = s.cur;
        marker_id_ = s.id;
        return false;
    }
    if (s.cur == marker_cur_ && s.id == marker_id_) return false;  // 同一 (cur,id) 重复出现 ⇒ 0 事件
    marker_cur_ = s.cur;
    marker_id_ = s.id;
    ++marker_edges_;
    for (Action& a : actions_) {
        if (a.applied) continue;
        if (a.trigger.kind != TriggerKind::Marker) continue;
        if (quit_requested_) break;
        if (!markerNameMatches(a.trigger.name, s)) continue;
        std::printf("[input-script] frame=%llu guest_ms=%llu marker edge: cur=%u id=0x%08X name=%s%s ⇒ %s\n",
                    static_cast<unsigned long long>(frame), static_cast<unsigned long long>(guest_ms), s.cur, s.id,
                    s.name.c_str(), s.name_degraded ? "（降级）" : "", a.text.c_str());
        applyAction(wm, a, frame, guest_ms);
    }
    return true;
}

void InputScript::updateMarkerTimeout(std::uint64_t wall_ms) {
    if (marker_timeout_fired_) return;
    if (pend_count_[static_cast<int>(TriggerKind::Marker)] == 0) {
        marker_waiting_ = false;
        marker_wait_action_ = static_cast<std::size_t>(-1);
        marker_wait_name_.clear();
        return;
    }
    // 文件序第一条未应用 Marker 动作 = 这次"在等谁"（动作集只追加 ⇒ 下标稳定）。
    std::size_t first_idx = static_cast<std::size_t>(-1);
    const Action* first = nullptr;
    for (std::size_t i = 0; i < actions_.size(); ++i) {
        const Action& a = actions_[i];
        if (!a.applied && a.trigger.kind == TriggerKind::Marker) {
            first_idx = i;
            first = &a;
            break;
        }
    }
    if (first == nullptr) {
        marker_waiting_ = false;
        marker_wait_action_ = static_cast<std::size_t>(-1);
        return;
    }
    // ★ 基线一变就重置：① 前一条等到了、现在等**另一条**（哪怕同名 —— 同名两条各有各的预算）；
    //   ② 新追加了一条更早的（appendText 只会追加，所以正常不会）。⛔ 否则 B 的预算会被 A 吃掉。
    if (!marker_waiting_ || marker_wait_action_ != first_idx) {
        marker_waiting_ = true;
        marker_wait_action_ = first_idx;
        marker_wait_name_ = first->trigger.name;
        marker_wait_ms_ = wall_ms;
        return;
    }
    const std::uint64_t waited = wall_ms >= marker_wait_ms_ ? wall_ms - marker_wait_ms_ : 0;
    if (marker_timeout_ms_ != 0 && waited >= marker_timeout_ms_) {
        marker_timeout_fired_ = true;
        // ★ 三个数必须打：等的是哪个名字、实际看到的是什么、等了多久（父级裁决 3 的 detail 格式）。
        const std::string last = marker_seeded_ ? marker_last_name_ : std::string("none");
        marker_timeout_detail_ = "marker-timeout: waited=" + marker_wait_name_ + " last=" + last +
                                 " waited_ms=" + std::to_string(waited);
        std::fprintf(stderr, "[marker] ⚠ %s\n", marker_timeout_detail_.c_str());
    }
}

void InputScript::resetMarkerState() {
    marker_seeded_ = false;
    marker_cur_ = 0;
    marker_id_ = 0;
    marker_samples_ = 0;
    marker_edges_ = 0;
    marker_invalid_ = 0;
    marker_last_cur_ = 0;
    marker_last_id_ = 0;
    marker_last_name_.clear();
    marker_last_name_degraded_ = false;
    marker_timeout_fired_ = false;
    marker_timeout_posted_ = false;
    marker_timeout_detail_.clear();
    marker_waiting_ = false;
    marker_wait_action_ = static_cast<std::size_t>(-1);
    marker_wait_ms_ = 0;
    marker_wait_name_.clear();
    // ★ 0 点的**状态**跟着每轮 run 重置（配置保留）。放在这里是因为 `resetMarkerState()` 同时是
    //   "每轮 run 开头"与 `clear()` 的唯一重置点 —— 两处都要清状态、都不许清配置（K8）。
    resetOriginState();
}

void InputScript::rebuildPending() {
    pending_dirty_ = false;
    for (int k = 0; k < kTriggerKindCount; ++k) {
        pend_min_[k] = 0;
        pend_count_[k] = 0;
        pend_min_abs_[k] = 0;
        pend_count_abs_[k] = 0;
    }
    pend_frame_limit_ = 0;
    for (const Action& a : actions_) {
        if (a.applied) continue;
        ++action_scans_;  // 可观测：这里是**真正的逐条比较**（每次动作集变化才发生）
        const int k = static_cast<int>(a.trigger.kind);
        ++pend_count_[k];  // 存在性判据（帧停滞兜底 / marker 超时）按**全部**未应用动作算
        // ★ Marker 没有数值到期：只累条数，**不参与** `pend_min_` / `any_due`。
        //   ⛔ 别写成 `pend_min_[Marker] = 0` —— 那会让它每一 tick 都"到期"。
        if (a.trigger.kind != TriggerKind::Marker) {
            if (a.absolute) {
                // 绝对段：独立的最小值（闩锁 pending 期间它**不被抑制**）。
                if (pend_count_abs_[k] == 0 || a.trigger.value < pend_min_abs_[k]) pend_min_abs_[k] = a.trigger.value;
                ++pend_count_abs_[k];
            } else {
                // 相对段：`pend_count_[k] - 1` = 本条之前的总条数；它等于绝对条数 ⇒ 还没有相对条数。
                if (pend_count_[k] - 1 == pend_count_abs_[k] || a.trigger.value < pend_min_[k]) {
                    pend_min_[k] = a.trigger.value;
                }
            }
        }
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

    // ★ marker 超时：**必须放在下面那条快路径 `return false` 之前** —— 只有 marker 动作时
    //   `any_due` 为假、`releases_` 空、`finished()` 假，放后面就永远不检查（静默挂死）。
    //   ★ 基线用**宿主墙钟**：guest 阻塞在 `GetMessageA` 时 `guest_ms` 不推进。
    updateMarkerTimeout(wall_ms);

    // ★ 0 点**还没闩锁** ⇒ 抑制**相对段**的全部数值触发（裁决 10：不是"改小"，是整段不判）：
    //   闩锁前时钟还是"从 run 起算"，任何 `at frame 1` 都会在第 1 帧就到期 ⇒ 0 点假。
    //   ★ **绝对段**（`before-anchor`）不参与抑制：它的值本来就是"从 run 起算"的绝对量。
    //   ⛔ 只抑制数值 kind：marker 动作不等 0 点（裁决 M3）；已排队的抬起与收口也不受影响。
    const bool origin_pending = origin_requested_ && !origin_set_;

    // ★ 帧停滞兜底：只在"还有 frame 条件在等"且"帧号真的停住"时触发一次。
    //   ⛔ 帧正常推进时这一段**一次也不触发**（CTest 的 `stall-not-fired` 用例钉着）。
    //   ★ `limit` 走缓存（与"逐条扫描取最小非 0 阈值"等价）：没有 pending Frame 动作时 limit 恒 0
    //     ⇒ 整个分支可以整段跳过。
    //   ★ 0 点 pending 期间**禁用**（裁决 11）：折算一次就把 `at frame N` 永久折成墙钟条件 ——
    //     0 点还没定就折算，等于把"第 N 帧"换成"0 点前 N 帧的墙钟"，0 点从此是假的。
    if (!origin_pending && !stall_fired_ && pend_count_[static_cast<int>(TriggerKind::Frame)] != 0) {
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
    //   ★ 0 点 pending ⇒ **相对段**一条数值触发都不算到期（整段短路）；**绝对段**（`before-anchor`）
    //     不参与短路 —— 它存在的理由就是"0 点之前能跑"。
    //   ★ 相对段 / 绝对段各有自己的最小缓存（见 `rebuildPending`）；相对段的条数 = 总条数 − 绝对条数。
    const bool rel_any_due =
        (pend_count_[static_cast<int>(TriggerKind::Frame)] > pend_count_abs_[static_cast<int>(TriggerKind::Frame)] &&
         frame >= pend_min_[static_cast<int>(TriggerKind::Frame)]) ||
        (pend_count_[static_cast<int>(TriggerKind::GuestMs)] > pend_count_abs_[static_cast<int>(TriggerKind::GuestMs)] &&
         guest_ms >= pend_min_[static_cast<int>(TriggerKind::GuestMs)]) ||
        (pend_count_[static_cast<int>(TriggerKind::Insn)] > pend_count_abs_[static_cast<int>(TriggerKind::Insn)] &&
         insns >= pend_min_[static_cast<int>(TriggerKind::Insn)]) ||
        (pend_count_[static_cast<int>(TriggerKind::WallMs)] > pend_count_abs_[static_cast<int>(TriggerKind::WallMs)] &&
         wall_ms >= pend_min_[static_cast<int>(TriggerKind::WallMs)]);
    const bool abs_any_due =
        (pend_count_abs_[static_cast<int>(TriggerKind::Frame)] != 0 &&
         frame >= pend_min_abs_[static_cast<int>(TriggerKind::Frame)]) ||
        (pend_count_abs_[static_cast<int>(TriggerKind::GuestMs)] != 0 &&
         guest_ms >= pend_min_abs_[static_cast<int>(TriggerKind::GuestMs)]) ||
        (pend_count_abs_[static_cast<int>(TriggerKind::Insn)] != 0 &&
         insns >= pend_min_abs_[static_cast<int>(TriggerKind::Insn)]) ||
        (pend_count_abs_[static_cast<int>(TriggerKind::WallMs)] != 0 &&
         wall_ms >= pend_min_abs_[static_cast<int>(TriggerKind::WallMs)]);
    const bool any_due = abs_any_due || (!origin_pending && rel_any_due);
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
        // ★ 0 点 pending ⇒ **相对段**的数值动作一律不应用（与上面的 `any_due` 同一条判据）。
        //   ⛔ **绝对段豁免**：`before-anchor` 的触发按声明原样执行 —— 它就是"用输入把游戏推到某屏"
        //     的那一段（`REQ-01M710E6PV4PPHECPPCCTRF5RA`）。⛔ 不要把它一起 suppress（那等于本单没做）。
        //   `Marker` 在这条循环里本来就不会到期（`due()` 恒 false），它走 `noteMarker()`。
        if (origin_pending && !a.absolute) continue;
        ++action_scans_;  // 可观测：到期的那一次才真正逐条比较
        if (quit_requested_) continue;
        if (!a.trigger.due(frame, guest_ms, insns, wall_ms)) continue;
        std::printf("[input-script] frame=%llu guest_ms=%llu insns=%llu %s\n",
                    static_cast<unsigned long long>(frame), static_cast<unsigned long long>(guest_ms),
                    static_cast<unsigned long long>(insns), a.text.c_str());
        applyAction(wm, a, frame, guest_ms);
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
        // ★ 收口行必须**与事实一致**（`REQ-01M72WHFMCWQ0KEJDQG0XH21AH`）：`quit` 提前结束时
        //   只剩 `quit` 之前到期的动作被应用，旧文案打的是**总**动作数、说的是"全部应用"
        //   ⇒ 与摘要里的 `input-script: unfinished pending=…` 正好相反。这里按 `applied_` / 总数分支。
        //   ⚠ `finished()` 的语义不变（`applied_ == size && releases_.empty()`）；收口条件也不变。
        const std::size_t total = actions_.size();
        if (applied_ == total) {
            std::printf("[input-script] finished：%zu 条动作全部应用（脚本按下的键 / 按钮已抬起；"
                        "键 / 按钮 / 光标所有权交还 OS）\n",
                        total);
        } else {
            // 走这里只可能是 `quit`（`finished()` ⇒ `applied_ == total`）；仍写全"已应用 / 共 / 剩"三数。
            std::printf("[input-script] quit：已应用 %zu / 共 %zu 条动作（quit 提前结束，剩 %zu 条未应用；"
                        "脚本按下的键 / 按钮已抬起；键 / 按钮 / 光标所有权交还 OS）\n",
                        applied_, total, total - applied_);
        }
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
