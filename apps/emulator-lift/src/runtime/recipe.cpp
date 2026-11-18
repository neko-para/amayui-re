// emulator-lift · 输入配方的**解析器 + 门判定**（唯一一份；两个入口共用）
//
// ★ 规格 / 行写法 / 两个入口的能力面 ⇒ `runtime/recipe.h` 的文件头（这里只实现，⛔ 不复述）。
// ★ 两条纪律：
//   1. **报错一律带 `origin:行:`**（CLI 层据此 exit 2）：行号是"人怎么修"的唯一线索。
//   2. **门的判定只读观测值**（`MarkerSample` + `ScriptPositionSample`）：本文件不读内存、不看时钟。
#include "runtime/recipe.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "runtime/vfs.h"  // openHostFileUtf8（与其它 loader 同一份宿主路径口径）

namespace lift {
namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string trimAscii(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string lowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::vector<std::string> splitWords(const std::string& body) {
    std::vector<std::string> tok;
    std::size_t i = 0;
    while (i < body.size()) {
        while (i < body.size() && isSpace(body[i])) ++i;
        std::size_t b = i;
        while (i < body.size() && !isSpace(body[i])) ++i;
        if (i > b) tok.push_back(body.substr(b, i - b));
    }
    return tok;
}

// 十进制无符号（⛔ 不收前缀 / 负数 / 单位后缀）。
bool parseU64(const std::string& t, std::uint64_t* out) {
    if (t.empty()) return false;
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

// `await position` 的 FROM / TO：**只认带 `0x` 前缀的 hex**（与清单 `label_%08x` 对照方便）。
// ⛔ 不收十进制 / 负数 / 裸 hex（`39C`）—— 那是"看起来像偏移、其实单位不明"的静默陷阱。
bool parseHexU32(const std::string& t, std::uint32_t* out) {
    if (t.size() < 3 || t[0] != '0' || (t[1] != 'x' && t[1] != 'X')) return false;
    std::uint64_t v = 0;
    for (std::size_t i = 2; i < t.size(); ++i) {
        const char c = t[i];
        unsigned digit = 0;
        if (c >= '0' && c <= '9') {
            digit = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            digit = static_cast<unsigned>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            digit = static_cast<unsigned>(c - 'A' + 10);
        } else {
            return false;
        }
        v = v * 16ull + digit;
        if (v > 0xFFFFFFFFull) return false;  // 超出 u32
    }
    *out = static_cast<std::uint32_t>(v);
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

std::string hex8(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%08X", static_cast<unsigned>(v));
    return buf;
}

std::string u64s(std::uint64_t v) { return std::to_string(static_cast<unsigned long long>(v)); }

// 词表提示（报错用）。**唯一一份**：指令加了一条就只改这里。
// ★ 没有"按入口裁剪"的参数：六条指令 + 行尾 `timeout` 两臂都认（能力面只有一份，见 `recipe.h`）。
std::string instructionList() {
    return std::string(
        "`await script <名字> [timeout <ms>]` / `await position <名字> <FROM> <TO> [timeout <ms>]` / "
        "`wait <ms>` / `click <x> <y> [left|right]` / `profile_begin <label>` / "
        "`profile_end <label>` / `quit`");
}

// ---- `script` 名字口径 ---------------------------------------------------------------
// ASCII 大小写不敏感（⛔ 不做 Unicode 折叠）· **两侧**各剥一次大小写不敏感的 `.BIN` ·
// 解析出的名字与 `id:0x%08X` 退化名**始终都接受**（名字表某一刻读不到不该让门静默失明）。
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

std::string degradedIdName(std::uint32_t id) { return "id:" + hex8(id); }

std::string stripBinSuffix(const std::string& name) {
    const std::string suffix = ".BIN";
    if (name.size() > suffix.size() &&
        asciiEqualsIgnoreCase(name.substr(name.size() - suffix.size()), suffix)) {
        return name.substr(0, name.size() - suffix.size());
    }
    return name;
}

bool scriptNameHits(const std::string& want, const std::string& got) {
    return asciiEqualsIgnoreCase(stripBinSuffix(want), stripBinSuffix(got));
}

}  // namespace

bool scriptNameMatches(const std::string& want, const MarkerSample& s) {
    if (scriptNameHits(want, s.name)) return true;
    return scriptNameHits(want, degradedIdName(s.id));
}

const char* stepKindName(StepKind k) {
    switch (k) {
        case StepKind::AwaitScript: return "await script";
        case StepKind::AwaitPosition: return "await position";
        case StepKind::Wait: return "wait";
        case StepKind::Click: return "click";
        case StepKind::ProfileBegin: return "profile_begin";
        case StepKind::ProfileEnd: return "profile_end";
        case StepKind::Quit: return "quit";
    }
    return "?";
}

std::string stepText(const Step& s) {
    switch (s.kind) {
        case StepKind::AwaitScript:
            return "await script " + s.script;
        case StepKind::AwaitPosition:
            return "await position " + s.script + " " + hex8(s.pos_from) + " " + hex8(s.pos_to);
        case StepKind::Wait:
            return "wait " + u64s(s.wait_ms);
        case StepKind::Click: {
            std::string out = "click " + std::to_string(s.x) + " " + std::to_string(s.y);
            if (s.button == MouseButton::Right) out += " right";
            return out;
        }
        case StepKind::ProfileBegin:
            return "profile_begin " + s.label;
        case StepKind::ProfileEnd:
            return "profile_end " + s.label;
        case StepKind::Quit:
            return "quit";
    }
    return "?";
}

bool stepGateHolds(const Step& s, const MarkerSample* identity, const ScriptPositionSample& position) {
    switch (s.kind) {
        case StepKind::AwaitScript:
            // ★ **电平**：当前身份 == X 成立，⛔ 不等新事件、⛔ 不看边沿计数。
            //   身份读不到（base 还没发布 / 没采样）⇒ 不成立（不是"总是成立"）。
            if (identity == nullptr || !identity->valid) return false;
            return scriptNameMatches(s.script, *identity);
        case StepKind::AwaitPosition:
            // 同一层两个条件：身份匹配 **且** `FROM <= pos < TO`（半开区间：TO 不含）。
            if (identity == nullptr || !identity->valid) return false;
            if (!scriptNameMatches(s.script, *identity)) return false;
            if (!position.valid) return false;
            return position.pos >= s.pos_from && position.pos < s.pos_to;
        case StepKind::Wait:
        case StepKind::Click:
        case StepKind::ProfileBegin:
        case StepKind::ProfileEnd:
        case StepKind::Quit:
            return true;  // 非门：没有条件
    }
    return true;
}

std::uint64_t stepTimeoutMs(const Step& s, std::uint64_t default_ms) {
    if (s.timeout_ms != 0) return s.timeout_ms;  // 显式 `timeout` 恒覆盖（解析期保证只在门上）
    // ★ `wait <ms>` 是**固定时长**、到点就走 ⇒ **没有守卫**（⛔ 不再有 `ms + 缺省` 的补丁：
    //   那份补丁是图节点上的 timeout 拍平成线性写法时被拖到 `wait` 上的，固定等待不需要"等它成立"）。
    if (s.kind == StepKind::Wait) return 0;
    return default_ms;  // 没写 ⇒ 用驱动器给的缺省（0 = 这个驱动器不给缺省守卫）
}

bool stepWindowExpired(std::uint64_t elapsed_ms, std::uint64_t guard_ms) {
    // ★ 严格大于：`timeout 2000` 在"刚好等满 2000 ms"那一刻还没到期（与解释器臂的 `windowExpired`
    //   同一条口径：`guest_ms − wait_start_guest_ms > timeout_ms`）。0 = 没有窗口 ⇒ 永不到期。
    return guard_ms != 0 && elapsed_ms > guard_ms;
}

int recipeOutcomeExitCode(RecipeOutcome o) {
    // ★ 唯一一份退出码口径：`Done` 不覆盖调用方的运行结论（AOT 臂还有"跑失败 ⇒ 2"那一层）；
    //   其余两种（没走完 / 超时）一律 5，与解释器臂对齐。
    return o == RecipeOutcome::Done ? 0 : kRecipeIncompleteExitCode;
}

const char* recipeOutcomeName(RecipeOutcome o) {
    switch (o) {
        case RecipeOutcome::Running: return "running";
        case RecipeOutcome::Done: return "done";
        case RecipeOutcome::TimedOut: return "timedout";
    }
    return "?";
}

void Recipe::clear() {
    steps_.clear();
    origin_.clear();
}

bool Recipe::loadFile(const std::string& path, std::string* err) {
    std::FILE* f = openHostFileUtf8(path, "rb");
    if (f == nullptr) {
        if (err) *err = "打不开配方：" + path;
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

bool Recipe::loadText(const std::string& text, const std::string& origin, std::string* err) {
    clear();
    // ★ 先解析到**局部**容器，全部成功才提交（失败时旧配方已被清掉：不留半截状态）。
    std::vector<Step> steps;
    auto fail = [&](int line, const std::string& why) {
        if (err) *err = origin + ":" + std::to_string(line) + ": " + why;
        return false;
    };
    const std::string list = instructionList();

    // 一行 = 一条指令（`#` 到行尾是注释；空行忽略，但行号照数）。
    // ★ `tok` 按值收：行尾 `timeout <ms>` 会被剥掉，剩下的才是"指令 + 参数"。
    auto parseLine = [&](int line, const std::string& body, std::vector<std::string> tok) -> bool {
        const std::string head = lowerAscii(tok[0]);
        auto push = [&](Step s) {
            s.line = line;
            s.id = "step" + std::to_string(steps.size() + 1);
            steps.push_back(std::move(s));
        };
        // ---- 行尾 `timeout <ms>`（**只有门**能带；⛔ 旧的"守上一条指令"写法已删除，不留两种）----
        //   · 单独一行 `timeout` ⇒ 硬错，文案直接说"写到它守的那一行末尾"；
        //   · `wait` 是固定时长 ⇒ 没有可等待的东西（文案给出改回的正确写法）；
        //   · `click` / `profile_*` / `quit` 同样没有可等待的东西。
        //   · 其余未知指令 ⇒ 交给下面统一报"未知指令"（回显整行）。
        if (head == "timeout") {
            return fail(line,
                        "`timeout <ms>` 不能单独一行（⛔ 没有\"守上一条指令\"的写法）：把它写到它守的那个门"
                        "**那一行的末尾**，例 `await script TITLE timeout 30000`");
        }
        std::uint64_t inline_timeout = 0;
        for (std::size_t i = 1; i < tok.size(); ++i) {
            if (lowerAscii(tok[i]) != "timeout") continue;
            if (i + 1 == tok.size()) {
                return fail(line, "`timeout` 后面要跟一个毫秒数（收到 0 个词）");
            }
            if (i + 2 != tok.size()) {
                return fail(line, "`timeout <ms>` 只能写在**行尾**（`timeout` 后面还有 " +
                                      std::to_string(tok.size() - i - 1) + " 个词）");
            }
            const bool on_gate = head == "await" && tok.size() >= 3 &&
                                 (lowerAscii(tok[1]) == "script" || lowerAscii(tok[1]) == "position");
            if (!on_gate) {
                if (head == "wait") {
                    const std::string fixed = i == 1 ? std::string("wait <ms>") : "wait " + tok[1];
                    return fail(line, "`wait` 是**固定时长**（到点就走），没有可等待的东西 ⇒ 去掉 `timeout`，"
                                      "把它写回 `" + fixed + "`");
                }
                if (head == "click" || head == "profile_begin" || head == "profile_end" || head == "quit" ||
                    head == "exit") {
                    return fail(line, "`" + tok[0] +
                                          "` 没有可等待的东西 ⇒ `timeout <ms>` 只能挂在 `await script` / "
                                          "`await position` 那一行的末尾");
                }
                break;  // 未知指令：下面统一报"未知指令"
            }
            std::uint64_t ms = 0;
            if (!parseU64(tok[i + 1], &ms) || ms == 0) {
                return fail(line, "`timeout` 的毫秒必须是**正整数**（收到 `" + tok[i + 1] + "`）");
            }
            inline_timeout = ms;
            tok.resize(i);  // 剥掉行尾 `timeout <ms>`，下面按普通指令解析
            break;
        }
        if (head == "await") {
            const std::string sub = tok.size() > 1 ? lowerAscii(tok[1]) : std::string();
            if (sub == "script") {
                if (tok.size() != 3) {
                    return fail(line, "`await script` 需要一个脚本名（收到 " +
                                          std::to_string(tok.size() > 2 ? tok.size() - 2 : 0) + " 个词）");
                }
                Step s;
                s.kind = StepKind::AwaitScript;
                s.script = tok[2];
                s.timeout_ms = inline_timeout;  // 行尾 `timeout <ms>`（0 = 没写）
                push(std::move(s));
                return true;
            }
            if (sub == "position") {
                // ★ 语法：`await position <SCRIPT> <FROM> <TO>`；FROM 含、TO 不含；解析期校验 FROM < TO。
                //   ⛔ 脚本名**不在这里**校验（名字匹配是运行期的事：`scriptNameMatches`）。
                if (tok.size() != 5) {
                    return fail(line, "`await position` 需要 `<脚本名> <FROM> <TO>`（收到 " +
                                          std::to_string(tok.size() > 2 ? tok.size() - 2 : 0) +
                                          " 个词；FROM/TO 是带 `0x` 前缀的 hex）");
                }
                std::uint32_t from = 0;
                std::uint32_t to = 0;
                if (!parseHexU32(tok[3], &from) || !parseHexU32(tok[4], &to)) {
                    return fail(line, "`await position` 的 FROM/TO 必须是带 `0x` 前缀的 32 位 hex（收到 `" +
                                          tok[3] + "` `" + tok[4] + "`）");
                }
                if (from >= to) {
                    return fail(line, "`await position` 要求 FROM < TO（半开区间 `[FROM, TO)`；收到 `" + tok[3] +
                                          "` `" + tok[4] + "`）");
                }
                Step s;
                s.kind = StepKind::AwaitPosition;
                s.script = tok[2];
                s.pos_from = from;
                s.pos_to = to;
                s.timeout_ms = inline_timeout;  // 行尾 `timeout <ms>`（0 = 没写）
                push(std::move(s));
                return true;
            }
            // `await always` / `await presents <n>` / `await marker …` 全落这里。
            return fail(line, "未知指令：`" + body + "`（只认 " + list + "）");
        }
        if (head == "wait") {
            if (tok.size() != 2) {
                return fail(line, "`wait` 只收一个毫秒数（收到 " + std::to_string(tok.size() - 1) + " 个词）");
            }
            std::uint64_t ms = 0;
            if (!parseU64(tok[1], &ms) || ms == 0) {
                return fail(line, "`wait` 的毫秒必须是**正整数**（收到 `" + tok[1] + "`）");
            }
            Step s;
            s.kind = StepKind::Wait;
            s.wait_ms = ms;
            push(std::move(s));
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
            Step s;
            s.kind = StepKind::Click;
            s.x = x;
            s.y = y;
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
            push(std::move(s));
            return true;
        }
        if (head == "profile_begin" || head == "profile_end") {
            if (tok.size() != 2) {
                return fail(line, "`" + tok[0] + "` 需要一个 label（单个词）");
            }
            Step s;
            s.kind = head == "profile_begin" ? StepKind::ProfileBegin : StepKind::ProfileEnd;
            s.label = tok[1];
            push(std::move(s));
            return true;
        }
        if (head == "quit" || head == "exit") {
            if (tok.size() != 1) return fail(line, "`quit` 不带参数");
            Step s;
            s.kind = StepKind::Quit;
            push(std::move(s));
            return true;
        }
        return fail(line, "未知指令：`" + body + "`（只认 " + list + "）");
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
        if (err) *err = origin + ": 文件里没有任何指令（只认 " + list + "）";
        return false;
    }
    steps_ = std::move(steps);
    origin_ = origin;
    return true;
}

}  // namespace lift
