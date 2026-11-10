// emulator-lift · 识别驱动的交互状态机（解析 / 采样接线 / 状态机 / trace / 失败报告）
//
// ★ 语义真源是 `docs/03-emulator/interaction-model.md`，行格式的规格是 `interaction_graph.h` 的文件头。
//   本文件只做那两处**已经写下来**的事，⛔ 不复述 schema（两份 schema 必漂，见 `AGENTS.md` §10）。
// ★ 三条实现口径（都是为了"同一份观测 ⇒ 同一串判定"）：
//   1. **判定只读 `FrameSampler` 的两个单调计数与最近一次采样值**：本文件不碰像素、不看时钟性格。
//      `wall_ms` 只用于**停滞兜底**（guest 阻塞在 GetMessageA 时 guest 时钟冻住，否则整轮会挂死）。
//   2. **动作不在这里直接改窗口状态**：全部经 `actuator_.appendText()` 追加**绝对触发**，
//      由 `InputScript` 去发 WM_* 并保证"一次按下一定被 guest 观察到"（见 `input_script.h` 顶部）。
//      为什么用 `frame + 1`：本 tick 在块边界上，动作若落在同一帧就可能与本 tick 的注入顺序打架；
//      +1 让它必然在**下一次 Present 之后**生效 —— 确定性，且与调用方的 tick 顺序无关。
//   3. **失败一律响亮**：状态进 `Failed` 并算好一份多行报告（`failureReport()`），⛔ 不 warning-only。
#include "runtime/interaction_graph.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "runtime/vfs.h"

namespace lift {
namespace {

// ---- 词法小工具（与 `input_script.cpp` 同一口径：`#` 起注释、空白分词、大小写不敏感）----

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

std::string trimCopy(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && isSpace(s[b])) ++b;
    while (e > b && isSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::string lowerCopy(const std::string& s) {
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

int digitValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 十进制 / `0x` 十六进制；溢出即解析失败（⛔ 不静默截断 —— 那会把"写错的 hash"当成另一个 hash）。
bool parseUnsigned(const std::string& text, std::uint64_t* out) {
    if (text.empty()) return false;
    std::size_t i = 0;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        i = 2;
    }
    std::uint64_t value = 0;
    for (; i < text.size(); ++i) {
        const int d = digitValue(text[i]);
        if (d < 0 || d >= base) return false;
        if (value > (0xFFFFFFFFFFFFFFFFull - static_cast<std::uint64_t>(d)) / static_cast<std::uint64_t>(base)) {
            return false;
        }
        value = value * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
    }
    *out = value;
    return true;
}

// 坐标（`screen rect` 的 x/y / 以后的负偏移）：允许前导 `-`。范围给得宽松但**有限**，
// 免得一个笔误把矩形写到 2^31 之后（那种 rect 夹到边界后必然为空，只会让失败原因更难读）。
bool parseInt32(const std::string& text, std::int32_t* out) {
    bool negative = false;
    std::size_t start = 0;
    if (!text.empty() && (text[0] == '-' || text[0] == '+')) {
        negative = text[0] == '-';
        start = 1;
    }
    std::uint64_t magnitude = 0;
    if (!parseUnsigned(text.substr(start), &magnitude)) return false;
    constexpr std::uint64_t kLimit = 1000000ull;
    if (magnitude > kLimit) return false;
    *out = negative ? -static_cast<std::int32_t>(magnitude) : static_cast<std::int32_t>(magnitude);
    return true;
}

// trace / 报错里的规范化写法（⛔ 只在这里拼一次：别处都读 `GraphCond::text` / `GraphAction::text`）
std::string hex64(std::uint64_t v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%016llX", static_cast<unsigned long long>(v));
    return buf;
}

std::string hexRgb(std::uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%06X", static_cast<unsigned>(v & 0xFFFFFFu));
    return buf;
}

std::string u64s(std::uint64_t v) { return std::to_string(static_cast<unsigned long long>(v)); }

// 矩形均值的命中判据：**每通道**差 ≤ `kRectAvgTolerance`（与真源 §1 的"判定口径"一致）。
// ⛔ 不比较 `RectSample::hash`：那个是"区域指纹"，不是 `avg` 的判据；写进去等于偷偷加了第二个条件。
bool rectAvgHits(std::uint32_t got, std::uint32_t want) {
    for (int shift = 0; shift <= 16; shift += 8) {
        const std::uint32_t a = (got >> shift) & 0xFFu;
        const std::uint32_t b = (want >> shift) & 0xFFu;
        const std::uint32_t diff = a > b ? a - b : b - a;
        if (diff > kRectAvgTolerance) return false;
    }
    return true;
}

// 一行 = 一条语句；`#` 起注释（整行 / 行尾都吃）。空行忽略（但行号照数，报错要用它）。
struct SourceLine {
    int no = 0;
    std::string body;             // 去注释、去首尾空白后的原文（动作的"原始写法"就是它）
    std::vector<std::string> tok; // 分词结果
};

std::vector<SourceLine> splitLines(const std::string& text) {
    std::vector<SourceLine> out;
    int no = 0;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string raw = text.substr(start, end - start);
        ++no;
        const std::size_t hash = raw.find('#');
        if (hash != std::string::npos) raw.resize(hash);
        SourceLine line;
        line.no = no;
        line.body = trimCopy(raw);
        line.tok = splitWords(line.body);
        out.push_back(std::move(line));
        if (end == text.size()) break;
        start = end + 1;
    }
    return out;
}

}  // namespace

const char* condKindName(CondKind k) {
    switch (k) {
        case CondKind::Always: return "always";
        case CondKind::ScreenHash: return "screen hash";
        case CondKind::ScreenRect: return "screen rect";
        case CondKind::ScreenChanged: return "screen changed";
        case CondKind::Script: return "script";
    }
    return "?";
}

const char* graphFailureName(GraphFailure f) {
    switch (f) {
        case GraphFailure::None: return "none";
        case GraphFailure::Timeout: return "timeout";
        case GraphFailure::NoSample: return "no-sample";
        case GraphFailure::ExpectChange: return "expect-change";
    }
    return "?";
}

// ---------------------------------------------------------------------------------------
// 条件判定：**只读**采样器的单调计数与最近一次采样值（⛔ 不采样、不读像素）
// ---------------------------------------------------------------------------------------
bool condHolds(const GraphCond& c, const FrameSampler& sampler, std::uint64_t samples_at_start,
               std::uint64_t changes_at_start) {
    // `samples_at_start` 不参与条件本身：它是调用方判 `no-sample` 的尺（窗口里到底有没有样本）。
    // ★ 判据与"有没有**新**样本"分开是**有意的**：画面已经停在目标那一屏时 `when` 应当立刻成立，
    //   ⛔ 不要求"再变一帧"（帧停住时那等于永远不成立 —— 正是真源 §5 说的那个坑）。
    (void)samples_at_start;
    switch (c.kind) {
        case CondKind::Always:
            return true;
        case CondKind::ScreenHash:
            // "该样本的整帧 hash 相等"：用最近一次采样的 hash（没有样本 ⇒ 不成立）。
            return sampler.last().valid && sampler.last().hash == c.hash;
        case CondKind::ScreenRect: {
            if (c.rect_index < 0) return false;  // 没 bindSampler ⇒ 没登记 ⇒ 永远不命中（失败会响亮）
            const std::vector<RectSample>& vals = sampler.rectValues();
            const std::size_t idx = static_cast<std::size_t>(c.rect_index);
            if (idx >= vals.size() || !vals[idx].valid) return false;
            return rectAvgHits(vals[idx].avg_rgb, c.avg_rgb);
        }
        case CondKind::ScreenChanged:
            // ★ 判据是"自进入这次等待以来变化**发生过**"（`changeCount` 增长），
            //   ⛔ 不是 `last().changed`（两次判定之间帧会推进好几帧，只看最后两帧会漏掉中间那次）。
            return sampler.changeCount() > changes_at_start;
        case CondKind::Script:
            return false;  // 解析期就拒绝了；留一条"永不成立"而不是静默退化成 always
    }
    return false;
}

namespace {

// ---- `CondKind` / `GraphActionKind` 的分类谓词 ------------------------------------------
// 每个谓词只回答"这一类值在某个落点上要不要做某事"（要产框 / 要登记矩形 / 要盯整帧 /
// expect 是否必须等新样本 / 动作要不要框 / 动作是否一发即收口）。
// ★ 为什么是**不带 `default:` 的 `switch`**：这些判断原先散在 6 处 `if (k == X || k == Y)` 里，
//   加一个枚举值而忘了表态**不会红**（静默漏改）。本工程开着 `-Wall -Wextra -Werror`（MSVC 侧
//   `/W4 /WX`）⇒ 写成穷举 `switch` 后，漏一个 case 就是**编译错误**。
// ⛔ 不许补 `default:`，也不许拆回 `if` —— 那会把这里唯一的完备性护栏关掉。
// ⛔ 也不要升级成"一张每 kind 一行的表"：表是另一层东西（需求单已判不建表，理由见 `design-notes`
//   的 `L3-graphfmt-01` 与需求单的裁决记录）。

// `when` 命中时是否**产出框**（`click center` 的唯一坐标来源）。
bool condYieldsBox(CondKind k) {
    switch (k) {
        case CondKind::ScreenRect: return true;
        case CondKind::Always:
        case CondKind::ScreenHash:
        case CondKind::ScreenChanged:
        case CondKind::Script: return false;
    }
    return false;
}

// 是否要在采样器里**登记矩形**（不登记 ⇒ 判定时 `rect_index` 恒 -1 ⇒ 永不命中）。
bool condNeedsRectBinding(CondKind k) {
    switch (k) {
        case CondKind::ScreenRect: return true;
        case CondKind::Always:
        case CondKind::ScreenHash:
        case CondKind::ScreenChanged:
        case CondKind::Script: return false;
    }
    return false;
}

// 是否要 `setWatchFrame(true)`：这张图用不用整帧量（整帧 hash 由 Present 无条件算，这只是记账）。
// 是否要 `setWatchFrame(true)`：这张图用不用整帧量（整帧 hash 由 Present 无条件算，这只是记账）。
// ⚠ 今天这一格**不可观测**：`watchFrame()` / `watchesAnything()` 全仓 0 调用者 ⇒ 写反了不会有任何
//   行为差异、也不会有用例红。保留它是因为 `FrameSampler::setWatchFrame` 的记账口径属于采样器
//   （`machine.cpp` 另有一处写它），**不是**本文件的判据。
bool condNeedsFrameWatch(CondKind k) {
    switch (k) {
        case CondKind::ScreenHash:
        case CondKind::ScreenChanged: return true;
        case CondKind::Always:
        case CondKind::ScreenRect:
        case CondKind::Script: return false;
    }
    return false;
}

// `expect` 是否**必须**等到动作之后的新样本（见 `expectMayMatch` 的理由）。
bool condNeedsNewSample(CondKind k) {
    switch (k) {
        case CondKind::Always:
        case CondKind::ScreenChanged: return false;  // 纯推进 / 计数型：不要求新样本
        case CondKind::ScreenHash:
        case CondKind::ScreenRect: return true;
        case CondKind::Script:
            // ⚠ 今天**不可达**（解析期就拒绝）。这一格刻意沿用"要新样本"，只是为了**保持今天的行为**：
            //   真落地 `when script` 时它很可能必须改成 false —— 脚本源的判据是 **marker 边沿计数**，
            //   不是 `sampleCount()`（房间空闲时帧会停、Present 计数不涨 ⇒ 等"新样本"等于永远不成立，
            //   正是 `interaction-model.md` §5 那个坑）。⛔ 不要照抄这一格。
            return true;
    }
    return false;
}

// 这个动作是否需要 `when` 产出的框（今天只有 `click center`）。
bool actionNeedsBox(GraphActionKind k) {
    switch (k) {
        case GraphActionKind::ClickCenter: return true;
        case GraphActionKind::Click:
        case GraphActionKind::Key:
        case GraphActionKind::Wait:
        case GraphActionKind::ProfileBegin:
        case GraphActionKind::ProfileEnd:
        case GraphActionKind::Quit: return false;
    }
    return false;
}

// 这个动作**一发出去就等于任务收口**（`quit`）：`issueAction` 要立刻记"已应用 + 完成"，
// 不等 actuator 的 applied 计数（`Machine::run` 在同一个块边界就会停 ⇒ 状态机没有下一次推进）。
bool actionFinishesTask(GraphActionKind k) {
    switch (k) {
        case GraphActionKind::Quit: return true;
        case GraphActionKind::Click:
        case GraphActionKind::ClickCenter:
        case GraphActionKind::Key:
        case GraphActionKind::Wait:
        case GraphActionKind::ProfileBegin:
        case GraphActionKind::ProfileEnd: return false;
    }
    return false;
}

// 只看文件名：trace 是**可 diff** 的产物（真源 §4），⛔ 不能把宿主的 checkout 路径写进去
// （换一台机器 / 换一个目录就不同 ⇒ 同一路径的两次运行 diff 出差异）。
std::string baseNameOf(const std::string& p) {
    const std::size_t slash = p.find_last_of("/\\");
    return slash == std::string::npos ? p : p.substr(slash + 1);
}

// `expect` 问的是"动作**之后**变了没有"：state 型条件（整帧 hash / 矩形）必须看见动作之后的
// **新样本**才允许命中 —— 否则"动作前的旧样本本来就等于目标值"会让 expect 恒真（`screen changed`
// 与 `always` 不受这条限制：前者本来就按计数判，后者是纯推进）。
bool expectMayMatch(const GraphCond& c, const FrameSampler& sampler, std::uint64_t samples_at_start) {
    if (!condNeedsNewSample(c.kind)) return true;
    return sampler.sampleCount() > samples_at_start;
}

}  // namespace

// ---------------------------------------------------------------------------------------
// 解析
// ---------------------------------------------------------------------------------------
bool InteractionGraph::loadText(const std::string& text, const std::string& origin, std::string* err) {
    clear();
    // ★ 先解析到**局部**容器，全部成功才提交：失败时旧图已被清掉（`failed` 的图不许留在半截状态）。
    std::vector<Node> nodes;
    struct BlockLines {
        int node = 0;
        int when = 0;
        int action = 0;
        int next = 0;
        int expect = 0;
        int timeout = 0;
    };
    std::vector<BlockLines> blocks;
    std::string entry;
    int entry_line = 0;
    bool have_entry = false;
    int last_line = 0;

    auto failAt = [&](int ln, const std::string& why) {
        if (err) *err = origin + ":" + std::to_string(ln) + ": " + why;
        return false;
    };

    // ---- 条件项（`when` / `expect` 共用）：tokens[0] 是键名 ----
    // ★ 分两段：① 词 → kind（**本文件唯一允许的手写字符串分派**；未知词必须带行号响亮报错）；
    //   ② `switch (kind)` 落值 / 校验元数（**不带 `default:`** ⇒ 加一个 `CondKind` 而忘了在这里
    //   处理它的词数 = 编译期红）。⛔ 不要把这两段合回一串 `if (head == …)`：那就又变成"漏了不红"。
    // ⚠ 报错串的口径**刻意不统一**（不是笔误）：`key` / `head` 是小写化的，而"未知条件" /
    //   "未知的 screen 条件"用**原文** token ⇒ 人写 `FOO` 时能在报错里看到 `FOO`。
    //   ⛔ 不要"顺手统一规范化"：现有测试与三份真图全是小写，改成小写化**没有任何用例会红**。
    auto condKindFrom = [&](const std::vector<std::string>& tok, int ln, CondKind* out) -> bool {
        const std::string key = lowerCopy(tok[0]);
        const std::string head = tok.size() > 1 ? lowerCopy(tok[1]) : std::string();
        if (head.empty()) return failAt(ln, "`" + key + "` 后面没有条件（只认 always / screen … / script …）");
        if (head == "always") {
            *out = CondKind::Always;
            return true;
        }
        if (head == "script") {
            *out = CondKind::Script;
            return true;
        }
        if (head != "screen") {
            return failAt(ln, "未知条件：" + tok[1] +
                                  "（只认 always / screen hash / screen rect / screen changed / script）");
        }
        if (tok.size() < 3) return failAt(ln, "`screen` 后面要写 hash / rect / changed");
        const std::string sub = lowerCopy(tok[2]);
        if (sub == "changed") {
            *out = CondKind::ScreenChanged;
            return true;
        }
        if (sub == "hash") {
            *out = CondKind::ScreenHash;
            return true;
        }
        if (sub == "rect") {
            *out = CondKind::ScreenRect;
            return true;
        }
        return failAt(ln, "未知的 screen 条件：" + tok[2] + "（只认 hash / rect / changed）");
    };

    auto parseCond = [&](const std::vector<std::string>& tok, int ln, GraphCond* out) -> bool {
        CondKind kind = CondKind::Always;
        if (!condKindFrom(tok, ln, &kind)) return false;
        out->kind = kind;
        switch (kind) {
            case CondKind::Always:
                if (tok.size() != 2) return failAt(ln, "`always` 不带参数");
                out->text = "always";
                return true;
            case CondKind::Script:
                // ⛔ 不静默退化成 always（真源 §6：引擎脚本身份源未实现）。
                return failAt(ln, "`script` 条件今天不可用：见 interaction-model.md §6（未实现的识别源，"
                                  "解析期拒绝而不是退化成 always）");
            case CondKind::ScreenChanged:
                if (tok.size() != 3) return failAt(ln, "`screen changed` 不带参数");
                out->text = "screen changed";
                return true;
            case CondKind::ScreenHash: {
                if (tok.size() != 4) return failAt(ln, "`screen hash` 需要一个指纹（0x 十六进制，最多 16 位）");
                std::uint64_t hash = 0;
                if (!parseUnsigned(tok[3], &hash)) return failAt(ln, "不是合法数字：" + tok[3]);
                out->hash = hash;
                out->text = "screen hash " + hex64(hash);
                return true;
            }
            case CondKind::ScreenRect: {
                // when screen rect <x> <y> <w> <h> avg <0xRRGGBB>
                if (tok.size() != 9) {
                    return failAt(ln, "`screen rect` 需要 `x y w h avg 0xRRGGBB` 六个词（收到 " +
                                          std::to_string(tok.size() - 3) + " 个）");
                }
                std::int32_t x = 0;
                std::int32_t y = 0;
                std::int32_t w = 0;
                std::int32_t h = 0;
                if (!parseInt32(tok[3], &x) || !parseInt32(tok[4], &y) || !parseInt32(tok[5], &w) ||
                    !parseInt32(tok[6], &h)) {
                    return failAt(ln, "矩形坐标 / 尺寸不是合法数字");
                }
                if (w <= 0 || h <= 0) return failAt(ln, "矩形宽高必须为正（w>0, h>0）");
                if (lowerCopy(tok[7]) != "avg") {
                    return failAt(ln, "`screen rect` 的第五个量必须是 `avg`（收到 " + tok[7] + "）");
                }
                std::uint64_t rgb = 0;
                if (!parseUnsigned(tok[8], &rgb)) return failAt(ln, "不是合法数字：" + tok[8]);
                if (rgb > 0xFFFFFFull) return failAt(ln, "均值必须是 0xRRGGBB（收到 " + tok[8] + "）");
                out->rect.x = x;
                out->rect.y = y;
                out->rect.w = w;
                out->rect.h = h;
                out->avg_rgb = static_cast<std::uint32_t>(rgb);
                out->text = "screen rect " + std::to_string(x) + " " + std::to_string(y) + " " +
                            std::to_string(w) + " " + std::to_string(h) + " avg " + hexRgb(out->avg_rgb);
                return true;
            }
        }
        return false;
    };

    // `click` 的按钮词：只收 `left` / `right`（省略 = left）。词表真源是
    // `docs/03-emulator/interaction-model.md` §1 —— 这里只实现，⛔ 不复述它的语义。
    auto parseButtonWord = [&](const std::string& word, int ln, MouseButton* out) -> bool {
        const std::string w = lowerCopy(word);
        if (w == "left") {
            *out = MouseButton::Left;
            return true;
        }
        if (w == "right") {
            *out = MouseButton::Right;
            return true;
        }
        return failAt(ln, "`click` 的按钮词只认 `left` / `right`（收到 `" + word + "`）");
    };

    // ---- 动作（闭集合）。`text` 由调用方填原始行（日志 / 报错定位）----
    // ★ 与 `parseCond` 同形：① 词 → kind（唯一手写字符串分派）② `switch (kind)` 落值（无 `default:`）。
    //   注意 `click` 的两条写法是**两个 kind**（`ClickCenter` 打命中框中心、`Click` 打固定点），
    //   所以"是不是 center"这一步判在 ①（它是在**选 kind**，不是在落值）。
    auto actionKindFrom = [&](const std::vector<std::string>& tok, int ln, GraphActionKind* out) -> bool {
        const std::string head = lowerCopy(tok[0]);
        if (head == "click") {
            *out = (tok.size() >= 2 && lowerCopy(tok[1]) == "center") ? GraphActionKind::ClickCenter
                                                                     : GraphActionKind::Click;
            return true;
        }
        if (head == "key") {
            *out = GraphActionKind::Key;
            return true;
        }
        if (head == "wait") {
            *out = GraphActionKind::Wait;
            return true;
        }
        if (head == "profile_begin") {
            *out = GraphActionKind::ProfileBegin;
            return true;
        }
        if (head == "profile_end") {
            *out = GraphActionKind::ProfileEnd;
            return true;
        }
        if (head == "quit" || head == "exit") {
            *out = GraphActionKind::Quit;
            return true;
        }
        return failAt(ln, "未知动作：" + tok[0] +
                              "（只认 click / key / wait / profile_begin / profile_end / quit）");
    };

    auto parseAction = [&](const std::vector<std::string>& tok, int ln, GraphAction* out) -> bool {
        GraphActionKind kind = GraphActionKind::Quit;
        if (!actionKindFrom(tok, ln, &kind)) return false;
        out->kind = kind;
        switch (kind) {
            case GraphActionKind::Click:
            case GraphActionKind::ClickCenter: {
                // `click center [left|right]`（2–3 个词）或 `click <x> <y> [left|right]`（3–4 个词）。
                // ★ 按钮词收进来，`hold` 仍不收 —— 两者不是同一类东西：
                //   · 按钮 = **输入目标**：换个按钮再点一次，同一张图多一条同构节点就够了，
                //     发到时间线也只是那行多一个词（`click x y right`）；
                //   · `hold` = **时序语义**：按下与抬起之间隔多久。图只在块边界上"发一次动作"，
                //     抬起由注入层的自动 hold 决定（`input_script.h`）；把 hold 收进图里等于把时间线
                //     的时序旋钮搬进来，而图的语法是时间线的**真子集**（真源 §1）。
                const bool center = kind == GraphActionKind::ClickCenter;
                const std::size_t base = center ? 2u : 3u;  // 不带按钮词时的词数
                if (tok.size() != base && tok.size() != base + 1) {
                    return failAt(ln, "`click` 需要 `X Y` 或 `center`，后面可选按钮词 `left|right`"
                                      "（⛔ 不猜坐标、也不收 hold）");
                }
                MouseButton button = MouseButton::Left;  // 省略按钮词 = left（= `GraphAction::button` 的缺省值）
                if (tok.size() == base + 1 && !parseButtonWord(tok[base], ln, &button)) return false;
                if (!center) {
                    std::int32_t x = 0;
                    std::int32_t y = 0;
                    if (!parseInt32(tok[1], &x) || !parseInt32(tok[2], &y)) {
                        return failAt(ln, "坐标不是合法数字");
                    }
                    out->x = x;
                    out->y = y;
                }
                out->button = button;
                return true;
            }
            case GraphActionKind::Key: {
                if (tok.size() != 2) return failAt(ln, "`key` 需要一个 VK（1..255）");
                std::uint64_t vk = 0;
                if (!parseUnsigned(tok[1], &vk)) return failAt(ln, "不是合法数字：" + tok[1]);
                if (vk == 0 || vk > 255) return failAt(ln, "VK 必须是 1..255（收到 " + tok[1] + "）");
                out->vk = static_cast<std::uint32_t>(vk);
                return true;
            }
            case GraphActionKind::Wait: {
                if (tok.size() != 3) return failAt(ln, "`wait` 需要 `N frames|ms`");
                std::uint64_t value = 0;
                if (!parseUnsigned(tok[1], &value)) return failAt(ln, "不是合法数字：" + tok[1]);
                const std::string unit = lowerCopy(tok[2]);
                TriggerKind unit_kind = TriggerKind::Frame;
                if (unit == "frame" || unit == "frames") {
                    unit_kind = TriggerKind::Frame;
                } else if (unit == "ms" || unit == "guest_ms") {
                    unit_kind = TriggerKind::GuestMs;
                } else {
                    return failAt(ln, "`wait` 的单位只认 `frames` / `ms`（收到 " + tok[2] + "）");
                }
                out->wait_value = value;
                out->wait_unit = unit_kind;
                return true;
            }
            case GraphActionKind::ProfileBegin:
            case GraphActionKind::ProfileEnd: {
                // ⚠ 报错串里的词与 `lowerCopy` 后的 head 等价（这两条只可能是小写词进来的）。
                const char* word = kind == GraphActionKind::ProfileBegin ? "profile_begin" : "profile_end";
                if (tok.size() != 2) return failAt(ln, std::string("`") + word + "` 需要一个 label（单个词）");
                out->label = tok[1];
                return true;
            }
            case GraphActionKind::Quit:
                // ⚠ 串里写死 `quit`：`exit` 也走这条（今天就是这样，⛔ 不为"更准确"改字节）。
                if (tok.size() != 1) return failAt(ln, "`quit` 不带参数");
                return true;
        }
        return false;
    };

    // 块收尾：必填键齐了没有、`click center` 的坐标来源合不合法
    auto closeBlock = [&]() -> bool {
        if (nodes.empty()) return true;
        Node& n = nodes.back();
        const BlockLines& b = blocks.back();
        if (b.when == 0) return failAt(b.node, "节点 `" + n.id + "` 缺 `when`（每个键恰好一次）");
        if (b.action == 0) return failAt(b.node, "节点 `" + n.id + "` 缺 `action`（每个键恰好一次）");
        if (b.next == 0) return failAt(b.node, "节点 `" + n.id + "` 缺 `next`（要到任务结束就写一个空的 `next`）");
        if (actionNeedsBox(n.action.kind) && !condYieldsBox(n.when.kind)) {
            // ⚠ 报错串里写死 `click center`：今天只有它需要框。若将来出现第二个"要框的动作"，
            //   这句要跟着泛化（⛔ 不要为了这个把 `actionNeedsBox` 拆回 if —— 见谓词处的理由）。
            return failAt(b.action, "`click center` 只在 `when screen rect …` 下合法（框中心是它唯一的坐标来源；"
                                    "本节点的 when 是 `" + n.when.text + "`）");
        }
        return true;
    };

    const std::vector<SourceLine> lines = splitLines(text);
    for (const SourceLine& line : lines) {
        if (line.tok.empty()) continue;
        last_line = line.no;  // 只认**有内容**的行（行尾空行不是"最后一行"，报错指它没有意义）
        const std::string head = lowerCopy(line.tok[0]);

        if (head == "entry") {
            // `entry` 写在块后面也是合法的（顶层语句相遇即收块）—— 但块得先通过收尾检查
            if (!closeBlock()) return false;
            if (have_entry) return failAt(line.no, "`entry` 只能出现一次");
            if (line.tok.size() != 2) return failAt(line.no, "`entry` 需要恰好一个节点 id");
            have_entry = true;
            entry = line.tok[1];
            entry_line = line.no;
            continue;
        }
        if (head == "node") {
            if (!closeBlock()) return false;
            if (line.tok.size() != 2) return failAt(line.no, "`node` 需要恰好一个节点 id");
            for (const Node& existing : nodes) {
                if (existing.id == line.tok[1]) {
                    return failAt(line.no, "重复的 node id：`" + line.tok[1] + "`（行 " +
                                               std::to_string(existing.line) + " 已定义过）");
                }
            }
            Node n;
            n.id = line.tok[1];
            n.line = line.no;
            n.timeout_ms = kDefaultNodeTimeoutMs;  // 省略 ⇒ 固定缺省值（trace 里标 `(default)`）
            n.timeout_explicit = false;
            nodes.push_back(std::move(n));
            BlockLines b;
            b.node = line.no;
            blocks.push_back(b);
            continue;
        }

        // 其余都是块内键
        if (nodes.empty()) {
            return failAt(line.no, "`" + line.tok[0] + "` 必须写在 `node <id>` 块里");
        }
        Node& n = nodes.back();
        BlockLines& b = blocks.back();
        auto duplicate = [&](int seen_line) {
            return failAt(line.no, "块内重复键：`" + line.tok[0] + "`（行 " + std::to_string(seen_line) +
                                       " 已经写过一次；每个键恰好一次）");
        };
        if (head == "when") {
            if (b.when != 0) return duplicate(b.when);
            if (!parseCond(line.tok, line.no, &n.when)) return false;
            b.when = line.no;
        } else if (head == "expect") {
            if (b.expect != 0) return duplicate(b.expect);
            if (!parseCond(line.tok, line.no, &n.expect)) return false;
            n.has_expect = true;
            b.expect = line.no;
        } else if (head == "action") {
            if (b.action != 0) return duplicate(b.action);
            if (line.tok.size() < 2) {
                return failAt(line.no, "`action` 后面没有动作（只认 click / key / wait / profile_begin / "
                                       "profile_end / quit）");
            }
            // 词表去掉键名本身（`parseAction` 只认动作动词；与 `parseCond` 的"tok[0] = 键"不同）
            const std::vector<std::string> args(line.tok.begin() + 1, line.tok.end());
            if (!parseAction(args, line.no, &n.action)) return false;
            // 原始写法（去掉 `action` 这个键名本身）：trace 是 `action <text> applied`，⛔ 不要印两遍
            for (const std::string& w : args) {
                if (!n.action.text.empty()) n.action.text += ' ';
                n.action.text += w;
            }
            b.action = line.no;
        } else if (head == "next") {
            if (b.next != 0) return duplicate(b.next);
            if (line.tok.size() > 2) return failAt(line.no, "`next` 只认**一个**节点 id（⛔ 不是候选列表）");
            n.next = line.tok.size() == 2 ? line.tok[1] : std::string();  // 空 = 任务结束
            b.next = line.no;
        } else if (head == "timeout") {
            if (b.timeout != 0) return duplicate(b.timeout);
            if (line.tok.size() != 2) return failAt(line.no, "`timeout` 需要毫秒数");
            std::uint64_t ms = 0;
            if (!parseUnsigned(line.tok[1], &ms)) return failAt(line.no, "不是合法数字：" + line.tok[1]);
            n.timeout_ms = ms;
            n.timeout_explicit = true;
            b.timeout = line.no;
        } else {
            return failAt(line.no, "未知键：`" + line.tok[0] +
                                       "`（块内只认 when / action / next / timeout / expect）");
        }
    }
    if (!closeBlock()) return false;

    // ---- 入口：真源没给缺省 ⇒ 省略就是错 ----
    if (!have_entry) {
        return failAt(last_line > 0 ? last_line : 1, "缺少 `entry`（真源没给缺省：第一条语句要写 `entry <id>`）");
    }
    std::size_t entry_index = nodes.size();
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].id == entry) {
            entry_index = i;
            break;
        }
    }
    if (entry_index == nodes.size()) {
        return failAt(entry_line, "`entry` 指向不存在的节点：`" + entry + "`");
    }
    // ---- `next` 必须已定义（解析期就红，⛔ 不留到运行期"跟丢了"）----
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].next.empty()) continue;
        bool found = false;
        for (const Node& other : nodes) {
            if (other.id == nodes[i].next) {
                found = true;
                break;
            }
        }
        if (!found) {
            return failAt(blocks[i].next, "`next` 指向不存在的节点：`" + nodes[i].next + "`");
        }
    }
    // ---- 提交 ----
    nodes_ = std::move(nodes);
    entry_ = entry;
    origin_ = origin;
    current_ = entry_index;
    return true;
}

bool InteractionGraph::loadFile(const std::string& path, std::string* err) {
    std::FILE* f = openHostFileUtf8(path, "rb");
    if (f == nullptr) {
        if (err) *err = "打不开节点图：" + path;
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

void InteractionGraph::clear() {
    nodes_.clear();
    entry_.clear();
    origin_.clear();
    state_ = State::Idle;
    current_ = 0;
    next_index_ = 0;
    next_id_.clear();
    failure_ = GraphFailure::None;
    failure_detail_.clear();
    failure_report_.clear();
    wait_start_guest_ms_ = 0;
    wait_start_frame_ = 0;
    wait_start_insns_ = 0;
    wait_start_wall_ms_ = 0;
    samples_at_start_ = 0;
    changes_at_start_ = 0;
    last_progress_wall_ms_ = 0;
    last_progress_frame_ = 0;
    last_progress_insns_ = 0;
    applied_before_ = 0;
    wait_pending_ = false;
    wait_due_frame_ = 0;
    wait_due_guest_ms_ = 0;
    have_box_ = false;
    box_ = PixelRect{};
    hit_pending_ = false;
    issue_blocked_logged_ = false;
    sampler_ = nullptr;
    fail_samples_total_ = 0;
    fail_changes_total_ = 0;
    fail_blind_total_ = 0;
    fail_rects_.clear();
    fail_frame_ = FrameSample{};
    fail_release_stats_ = InputScript::ReleaseStats{};
    fail_last_release_ = InputScript::ReleaseInfo{};
    fail_script_finished_ = false;
    fail_applied_ = 0;
    fail_actions_ = 0;
    fail_quit_requested_ = false;
    rect_bindings_.clear();
    bound_ = false;
    trace_header_done_ = false;  // 新图重新开一份头（`trace_` 是调用方的汇，不清它）
    transitions_ = 0;
    actions_issued_ = 0;
    actuator_.clear();
}

const Node* InteractionGraph::findNode(const std::string& id) const {
    for (const Node& n : nodes_) {
        if (n.id == id) return &n;
    }
    return nullptr;
}

std::string InteractionGraph::defaultFailDumpPath(const std::string& graph_path) {
    return graph_path + ".fail.bmp";
}

// ---------------------------------------------------------------------------------------
// 采样接线：解析之后调一次
// ---------------------------------------------------------------------------------------
void InteractionGraph::bindSampler(FrameSampler& sampler) {
    sampler_ = &sampler;
    rect_bindings_.clear();
    bool watch_frame = false;
    for (Node& n : nodes_) {
        auto bind = [&](GraphCond& c) {
            if (condNeedsRectBinding(c.kind)) {
                c.rect_index = static_cast<int>(sampler.watchRect(c.rect));
                bool known = false;
                for (const RectBinding& b : rect_bindings_) {
                    if (b.index == static_cast<std::size_t>(c.rect_index)) known = true;
                }
                if (!known) {
                    RectBinding b;
                    b.rect = c.rect;
                    b.index = static_cast<std::size_t>(c.rect_index);
                    rect_bindings_.push_back(b);
                }
            }
            if (condNeedsFrameWatch(c.kind)) watch_frame = true;
        };
        bind(n.when);
        if (n.has_expect) bind(n.expect);
    }
    // ★ 整帧 hash 由 `HeadlessPresenter::present` **无条件**算（它同时是帧日志与 A4 的判据），
    //   所以 `setWatchFrame` 只是"这张图用不用它"的记账（报告 / 诊断用），⛔ 不是"打不打开采样"。
    sampler.setWatchFrame(watch_frame);
    bound_ = true;
}

void InteractionGraph::setTrace(std::FILE* out, bool diffable) {
    trace_ = out;
    trace_owned_ = false;  // 汇是调用方打开的（stderr / 文件），本类不关它
    trace_diffable_ = diffable;
    trace_header_done_ = false;
}

void InteractionGraph::writeTraceHeader() {
    if (trace_ == nullptr || trace_header_done_) return;
    trace_header_done_ = true;
    std::fprintf(trace_, "# interaction trace v1\n");
    // ★ 首行标 NON-DIFFABLE（真源 §4）：real-time 呈现档的时序不可复现，那份 trace 不参与 diff。
    std::fprintf(trace_, "%s\n", trace_diffable_ ? "# diffable" : "# NON-DIFFABLE");
}

void InteractionGraph::traceLine(const std::string& line) {
    if (trace_ == nullptr) return;
    std::fprintf(trace_, "%s\n", line.c_str());
    std::fflush(trace_);  // 失败 / 崩掉时 trace 必须已经落盘（它就是事后唯一的判据）
}

std::string InteractionGraph::describeCond(const GraphCond& c) const {
    // ★ trace / 报错只打 `GraphCond::text`（解析期算好的规范化写法）—— 判据与显示只有一份来源。
    if (!c.text.empty()) return c.text;
    return condKindName(c.kind);
}

// 一行"采样侧"摘要（`failureDetail()` 的尾巴）：**只报可判定的量**，⛔ 不报墙钟。
std::string InteractionGraph::samplesSummary(const FrameSampler& sampler) const {
    std::string out = "samples=" + u64s(sampler.sampleCount()) + " changes=" + u64s(sampler.changeCount());
    // ⚠ `blind` 只对"真的在看矩形"的图有意义：`FrameSampler::observeValues` 把空矩形列表也记成
    //   一次盲采样，只监视整帧 hash 的图里那个数会等于总采样数（打出来只会误导）。
    if (sampler.rectCount() > 0) out += " blind=" + u64s(sampler.blindSamples());
    const FrameSample& f = sampler.last();
    out += f.valid ? (" last_frame_hash=" + hex64(f.hash) + " at frame=" + u64s(f.frame))
                   : std::string(" last_frame_hash=(none)");
    return out;
}

// ---------------------------------------------------------------------------------------
// 状态机
// ---------------------------------------------------------------------------------------
const char* InteractionGraph::stateName() const {
    switch (state_) {
        case State::Idle: return "idle";
        case State::WaitWhen: return "wait-when";
        case State::Acting: return "acting";
        case State::WaitExpect: return "wait-expect";
        case State::Done: return "done";
        case State::Failed: return "failed";
    }
    return "?";
}

const Node* InteractionGraph::current() const {
    // ★ Idle 时还没进任何节点 ⇒ 也报 entry 之外就返回 nullptr（调用方据此打 `-`）。
    if (state_ == State::Idle || state_ == State::Done) return nullptr;
    if (current_ >= nodes_.size()) return nullptr;
    return &nodes_[current_];
}

void InteractionGraph::noteProgress(std::uint64_t frame, std::uint64_t insns, std::uint64_t wall_ms) {
    // ★ "有进展" = guest 指令**或**帧号变了。为什么不是"只看 insns"：房间空闲时帧会停、而 guest
    //   还在跑循环（insns 涨）；反过来 guest 阻塞在 GetMessageA 时 insns 停、帧也可能停 —— 那才是
    //   真停滞。两个都停 ⇒ 兜底时钟重新起算。
    if (frame == last_progress_frame_ && insns == last_progress_insns_) return;
    last_progress_frame_ = frame;
    last_progress_insns_ = insns;
    last_progress_wall_ms_ = wall_ms;
}

bool InteractionGraph::windowExpired(std::uint64_t timeout_ms, std::uint64_t guest_ms,
                                     std::uint64_t wall_ms) const {
    // 尺 ①：guest 毫秒（"等的那一屏没出现"）
    if (guest_ms > wait_start_guest_ms_ && guest_ms - wait_start_guest_ms_ > timeout_ms) return true;
    // 尺 ②：墙钟停滞兜底（guest 阻塞在 GetMessageA 时 guest 时钟是冻住的，光看 ① 会把整轮挂死）
    if (wall_ms > last_progress_wall_ms_ && wall_ms - last_progress_wall_ms_ > timeout_ms) return true;
    return false;
}

void InteractionGraph::enterNode(std::size_t index, std::uint64_t frame, std::uint64_t guest_ms,
                                 std::uint64_t insns, std::uint64_t wall_ms) {
    current_ = index;
    next_id_ = nodes_[index].next;
    next_index_ = 0;
    state_ = State::WaitWhen;
    // 等待窗口的起点（判定只读这四个时刻 + 两个单调计数）
    wait_start_frame_ = frame;
    wait_start_insns_ = insns;
    wait_start_guest_ms_ = guest_ms;
    wait_start_wall_ms_ = wall_ms;
    last_progress_frame_ = frame;
    last_progress_insns_ = insns;
    last_progress_wall_ms_ = wall_ms;
    samples_at_start_ = sampler_ != nullptr ? sampler_->sampleCount() : 0;
    changes_at_start_ = sampler_ != nullptr ? sampler_->changeCount() : 0;
    blind_at_start_ = sampler_ != nullptr ? sampler_->blindSamples() : 0;
    have_box_ = false;
    hit_pending_ = false;
    wait_pending_ = false;
    issue_blocked_logged_ = false;
    ++transitions_;
    const Node& n = nodes_[index];
    std::string line = "node " + n.id + " enter frame=" + u64s(frame) + " insn=" + u64s(insns) +
                       " guest_ms=" + u64s(guest_ms);
    if (!n.timeout_explicit) line += " timeout=" + u64s(n.timeout_ms) + "(default)";
    traceLine(line);
}

bool InteractionGraph::issueAction(WindowManager& wm, std::uint64_t frame, std::uint64_t guest_ms) {
    // `wm` 不参与：`click center` 的坐标 = 识别到的框中心（guest 客户区像素，与注入层同一坐标系）。
    (void)wm;
    Node& n = nodes_[current_];
    // ★ 绝对触发 = **下一个 Present**（+1）。理由见文件头第 2 条。
    const std::string at = "at frame " + u64s(frame + 1) + "\n";
    // ★ 按钮词只在**非 left** 时发：left 是缺省 ⇒ 时间线收到的仍是逐字节相同的 `click <x> <y>`
    //   （trace 可 diff、老图/老 trace 不因这条特性而变）。两条 emit 路径都必须带它。
    const std::string button_word =
        n.action.button == MouseButton::Left ? std::string() : std::string(" right");
    std::string text;

    switch (n.action.kind) {
        case GraphActionKind::Click:
            text = at + "click " + std::to_string(n.action.x) + " " + std::to_string(n.action.y) +
                   button_word + "\n";
            break;
        case GraphActionKind::ClickCenter: {
            if (!have_box_) {
                // 解析期已保证 when 是 screen rect ⇒ 命中时一定有框；走到这里说明内部账不一致。
                std::fprintf(stderr, "[interaction-graph] ⚠ `click center` 没有命中框（节点 %s）：这是实现 bug\n",
                             n.id.c_str());
                return false;
            }
            const int cx = n.when.rect.x + n.when.rect.w / 2;
            const int cy = n.when.rect.y + n.when.rect.h / 2;
            text = at + "click " + std::to_string(cx) + " " + std::to_string(cy) + button_word + "\n";
            break;
        }
        case GraphActionKind::Key:
            // ★ down 在下一帧、up 再下一帧：让 `InputScript` 自己的"等 guest 采样"逻辑生效
            //   （一次按下一定要被 guest 观察到；⛔ 不在这里自己算 hold）。
            text = at + "key_down " + std::to_string(n.action.vk) + "\n" //
                   "at frame " + u64s(frame + 2) + "\n"                    //
                   "key_up " + std::to_string(n.action.vk) + "\n";
            break;
        case GraphActionKind::Wait:
            // ★ `wait` 不碰输入（真源：纯延迟）⇒ 不追加任何动作，只记到期点。
            wait_pending_ = true;
            wait_due_frame_ = frame + n.action.wait_value;
            wait_due_guest_ms_ = guest_ms + n.action.wait_value;
            applied_before_ = actuator_.appliedCount();
            ++actions_issued_;
            return true;
        case GraphActionKind::ProfileBegin:
            text = at + "profile_begin " + n.action.label + "\n";
            break;
        case GraphActionKind::ProfileEnd:
            text = at + "profile_end " + n.action.label + "\n";
            break;
        case GraphActionKind::Quit:
            text = at + "quit\n";
            break;
    }

    std::string err;
    if (!actuator_.appendText(text, origin_ + ":node " + n.id, &err)) {
        if (!issue_blocked_logged_) {
            issue_blocked_logged_ = true;
            // ★ 不是失败：`InputScript` 的前置（动作全部已应用 / 没有未完成的抬起）会在几个块边界内
            //   自己满足（抬起等 guest 采样 / 等 hold 到期）。这里只留一行诊断，命中事实留在
            //   `hit_pending_` 里下个块边界重试；真等不到就由窗口到期报 timeout / no-sample。
            std::fprintf(stderr, "[interaction-graph] 动作暂缓（节点 %s）：%s\n", n.id.c_str(), err.c_str());
        }
        return false;
    }
    issue_blocked_logged_ = false;
    applied_before_ = actuator_.appliedCount();
    ++actions_issued_;
    if (actionFinishesTask(n.action.kind)) {
        // ★ `quit` 的语义就是"这次运行到此为止"：⛔ 不要等 actuator 的 applied 计数 ——
        //   `Machine::run` 在**同一个块边界**就会看到 `quitRequested()` 停下来，状态机再没有
        //   机会推进 ⇒ trace 会停在 `when … hit`，读起来像"半截失败"（第一版实测就是这样）。
        //   这里直接记"动作已应用 + 节点完成 + 任务完成"，与真实发生的顺序一致。
        traceLine("action " + n.action.text + " applied");
        advance(frame, guest_ms, 0, 0);
    }
    return true;
}

void InteractionGraph::advance(std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns,
                               std::uint64_t wall_ms) {
    const Node& n = nodes_[current_];
    traceLine("node " + n.id + " done");
    ++transitions_;
    if (n.next.empty()) {
        state_ = State::Done;
        traceLine("task done nodes=" + u64s(nodes_.size()) + " transitions=" + u64s(transitions_) +
                  " actions=" + u64s(actions_issued_));
        return;
    }
    std::size_t index = nodes_.size();
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (nodes_[i].id == n.next) {
            index = i;
            break;
        }
    }
    if (index == nodes_.size()) {
        // 解析期已经保证存在；走到这里说明 nodes_ 被改过 —— 响亮失败，⛔ 不静默 Done。
        fail(GraphFailure::Timeout, "内部错误：next 指向不存在的节点 `" + n.next + "`", n.when, frame, guest_ms,
             insns);
        return;
    }
    enterNode(index, frame, guest_ms, insns, wall_ms);
}

void InteractionGraph::fail(GraphFailure kind, const std::string& why, const GraphCond& waited, std::uint64_t frame,
                            std::uint64_t guest_ms, std::uint64_t insns) {
    if (state_ == State::Failed) return;  // ⛔ 失败只报一次（调用方可能会继续 tick）
    state_ = State::Failed;
    failure_ = kind;
    ++transitions_;

    // ---- 快照：失败之后 sampler / actuator 还会继续动，报告必须打"那一刻"的值 ----
    if (sampler_ != nullptr) {
        fail_samples_total_ = sampler_->sampleCount();
        fail_changes_total_ = sampler_->changeCount();
        fail_blind_total_ = sampler_->blindSamples();
        fail_rects_ = sampler_->rectValues();
        fail_frame_ = sampler_->last();
    }
    fail_release_stats_ = actuator_.releaseStats();
    fail_last_release_ = actuator_.lastRelease();
    fail_applied_ = actuator_.appliedCount();
    fail_actions_ = actuator_.actionCount();
    // "抬起账要点"能判定的部分：脚本有没有收口（没收口 = 还有未应用的动作 / 未完成的抬起）。
    fail_script_finished_ = actuator_.finished();
    fail_quit_requested_ = actuator_.quitRequested();

    const Node& n = nodes_[current_];
    const std::uint64_t window_samples =
        fail_samples_total_ > samples_at_start_ ? fail_samples_total_ - samples_at_start_ : 0;

    // ---- 短描述（`failureDetail()`）：一行说清"谁、等什么、等了多久、看到什么" ----
    failure_detail_ = std::string(graphFailureName(kind)) + "：节点 `" + n.id + "` 等 `" + describeCond(waited) +
                      "` 超过 " + u64s(n.timeout_ms) + " ms（frame " + u64s(wait_start_frame_) + ".." +
                      u64s(frame) + "，insn " + u64s(wait_start_insns_) + ".." + u64s(insns) + "，guest_ms " +
                      u64s(wait_start_guest_ms_) + ".." + u64s(guest_ms) + "）；窗口内采样 " + u64s(window_samples) +
                      "（累计 " + u64s(fail_samples_total_) + "）" + (why.empty() ? std::string() : "；" + why);
    if (sampler_ != nullptr) failure_detail_ += "；" + samplesSummary(*sampler_);

    // ---- 完整报告（多行；stderr 直接写）----
    std::string r;
    r += std::string("interaction-graph: FAIL ") + graphFailureName(kind) + "\n";
    r += "  graph    : " + origin_ + "\n";
    r += "  node     : " + n.id + "（node 行 " + std::to_string(n.line) + "）\n";
    r += "  waiting  : " + describeCond(waited) + "\n";
    r += "  window   : frame=" + u64s(wait_start_frame_) + ".." + u64s(frame) + "  insn=" +
         u64s(wait_start_insns_) + ".." + u64s(insns) + "  guest_ms=" + u64s(wait_start_guest_ms_) + ".." +
         u64s(guest_ms) + "  timeout=" + u64s(n.timeout_ms) + "ms" +
         (n.timeout_explicit ? std::string() : std::string("(default)")) + "\n";
    // ⚠ `blind` 只在**有受监视矩形**时才打：`FrameSampler::observeValues` 把"空矩形列表"也记成一次
    //   盲采样（`screen_sample.cpp` 的口径），于是在"只监视整帧 hash"的图里那个数会等于总采样数 ——
    //   打出来只会误导（"读不到像素"与"根本没在看矩形"是两件事）。
    r += "  samples  : window=" + u64s(window_samples) + " total=" + u64s(fail_samples_total_) +
         " changes=" + u64s(fail_changes_total_) +
         (fail_rects_.empty() ? std::string() : " blind=" + u64s(fail_blind_total_)) + "\n";
    // 每个受监视矩形：**最后一次采样值**与"按各自目标均值是否命中"（同一个矩形可能被不同节点用不同
    // 目标值引用 ⇒ 逐条件打，⛔ 不要合成一个"这个矩形命中了吗"）。
    r += "  rects    : " + u64s(rect_bindings_.size()) + " watched\n";
    for (const Node& nn : nodes_) {
        auto report = [&](const GraphCond& c) {
            // ★ 判据问的是"**登记过矩形**的条件"，⛔ 不是"kind == ScreenRect"：两者今天恒等，
            //   但 `script` 若将来也带一个框，报告要跟着它走（否则会少一行候选 / 打一行假候选）。
            if (!condNeedsRectBinding(c.kind)) return;
            const bool bound = c.rect_index >= 0;
            const bool have = bound && static_cast<std::size_t>(c.rect_index) < fail_rects_.size();
            const RectSample& s = have ? fail_rects_[static_cast<std::size_t>(c.rect_index)] : RectSample{};
            r += "    [" + (bound ? u64s(static_cast<std::uint64_t>(c.rect_index)) : std::string("-")) + "] " +
                 std::to_string(c.rect.x) + " " + std::to_string(c.rect.y) + " " + std::to_string(c.rect.w) + " " +
                 std::to_string(c.rect.h) + " want_avg=" + hexRgb(c.avg_rgb);
            if (!bound) {
                r += " last_avg=(未接线 bindSampler) hit=0";  // 忘了接线 ⇒ 永远不命中，报告里要能看出来
            } else if (!s.valid) {
                r += " last_avg=(无效采样) hit=0";
            } else {
                r += " last_avg=" + hexRgb(s.avg_rgb) + (rectAvgHits(s.avg_rgb, c.avg_rgb) ? " hit=1" : " hit=0");
            }
            r += " (" + nn.id + "." + (&c == &nn.when ? std::string("when") : std::string("expect")) + ")\n";
        };
        report(nn.when);
        if (nn.has_expect) report(nn.expect);
    }
    r += fail_frame_.valid
             ? ("  frame    : hash=" + hex64(fail_frame_.hash) + " at frame=" + u64s(fail_frame_.frame) +
                "" + "\n")
             : std::string("  frame    : (一次整帧采样都没有)\n");
    r += "  actuator : applied=" + u64s(fail_applied_) + "/" + u64s(fail_actions_) +
         " releases=" + u64s(fail_release_stats_.releases) + "(auto=" + u64s(fail_release_stats_.auto_releases) +
         ",explicit=" + u64s(fail_release_stats_.explicit_releases) + ")" +
         " deferred_ups=" + u64s(fail_release_stats_.deferred_ups) +
         " bounded_no_sample=" + u64s(fail_release_stats_.bounded_no_sample) +
         " quit=" + (fail_quit_requested_ ? "1" : "0") + " finished=" + (fail_script_finished_ ? "1" : "0") + "\n";
    if (fail_last_release_.valid) {
        r += std::string("             last_release=") +
             (fail_last_release_.is_key ? "key" : "mouse") + " press_frame=" + u64s(fail_last_release_.press_frame) +
             " release_frame=" + u64s(fail_last_release_.release_frame) + " press_ms=" +
             u64s(fail_last_release_.press_ms) + " release_ms=" + u64s(fail_last_release_.release_ms) + " auto=" +
             (fail_last_release_.auto_hold ? "1" : "0") + "\n";
    }
    if (!why.empty()) r += "  why      : " + why + "\n";
    failure_report_ = r;

    traceLine(std::string("FAIL ") + graphFailureName(kind) + " node=" + n.id + " waited_frame=" +
              u64s(wait_start_frame_) + ".." + u64s(frame) + " waited_insn=" + u64s(wait_start_insns_) + ".." +
              u64s(insns) + " waited_guest_ms=" + u64s(wait_start_guest_ms_) + ".." + u64s(guest_ms) + " samples=" +
              u64s(window_samples));
}

std::string InteractionGraph::failureReport() const { return failure_report_; }

bool InteractionGraph::tick(WindowManager& wm, const FrameSampler& sampler, std::uint64_t frame,
                            std::uint64_t guest_ms, std::uint64_t insns, std::uint64_t wall_ms) {
    if (nodes_.empty()) return false;
    if (state_ == State::Done || state_ == State::Failed) return false;  // ⛔ 失败 / 完成不重复报
    // ★ 判定只读**这次传进来的**采样器（`bindSampler` 只负责登记矩形与整帧开关）。
    sampler_ = &sampler;
    writeTraceHeader();

    // 命中（条件已成立）⇒ 尝试追加动作；`InputScript` 的前置没满足就下个块边界再试。
    auto tryIssue = [&](const Node& n) -> bool {
        if (issueAction(wm, frame, guest_ms)) {
            hit_pending_ = false;
            return true;
        }
        // 等不到就按窗口到期处理（失败照旧响亮：timeout / no-sample）
        if (windowExpired(n.timeout_ms, guest_ms, wall_ms)) {
            const bool blind = sampler.sampleCount() == samples_at_start_;
            fail(blind ? GraphFailure::NoSample : GraphFailure::Timeout,
                 std::string("条件已命中，但动作一直没能追加进 InputScript（") +
                     (blind ? "窗口内一次采样都没有" : "等待窗口到期") + "）",
                 n.when, frame, guest_ms, insns);
        }
        return false;
    };

    switch (state_) {
        case State::Idle: {
            std::size_t index = nodes_.size();
            for (std::size_t i = 0; i < nodes_.size(); ++i) {
                if (nodes_[i].id == entry_) {
                    index = i;
                    break;
                }
            }
            if (index == nodes_.size()) return false;  // 解析期已保证存在
            traceLine("task start origin=" + baseNameOf(origin_) + " nodes=" + u64s(nodes_.size()) + " entry=" + entry_);
            enterNode(index, frame, guest_ms, insns, wall_ms);
            return true;
        }
        case State::WaitWhen: {
            const Node& n = nodes_[current_];
            noteProgress(frame, insns, wall_ms);  // 停滞兜底的时钟：insns / frame 变了就是"有进展"
            if (!hit_pending_) {
                if (condHolds(n.when, sampler, samples_at_start_, changes_at_start_)) {
                    hit_pending_ = true;
                    if (condYieldsBox(n.when.kind)) {
                        // ★ 命中框 = 本节点 `click center` 的唯一坐标来源。
                        box_ = n.when.rect;
                        have_box_ = true;
                    }
                    traceLine("when " + describeCond(n.when) + " samples=" + u64s(sampler.sampleCount()) + " hit");
                } else if (windowExpired(n.timeout_ms, guest_ms, wall_ms)) {
                    const bool blind = sampler.sampleCount() == samples_at_start_ ||
                                       sampler.blindSamples() > blind_at_start_;
                    fail(blind ? GraphFailure::NoSample : GraphFailure::Timeout,
                         blind ? std::string("窗口内一次采样都没有（识别源失明：帧停住 / 源没接上）")
                               : std::string("条件在窗口内一次都没成立"),
                         n.when, frame, guest_ms, insns);
                    return true;
                }
            }
            if (!hit_pending_) return false;
            // 命中本身就是一次迁移（即使本 tick 还没能把动作追加进去）
            if (!tryIssue(n)) return true;
            // ★ `quit` 在 issueAction 里就已经把任务收口成 `Done`（运行会在同一个块边界停下、
            //   状态机再也没有下一次推进）⇒ ⛔ 不要把状态覆盖回 `Acting`：那会让 summary 报
            //   `state=acting`、`done()` 恒假、调用方把"任务成功"判成"没走完"（实测踩过）。
            if (state_ != State::Done) state_ = State::Acting;
            ++transitions_;
            return true;
        }
        case State::Acting: {
            const Node& n = nodes_[current_];
            bool applied = false;
            if (wait_pending_) {
                applied = n.action.wait_unit == TriggerKind::Frame ? frame >= wait_due_frame_
                                                                   : guest_ms >= wait_due_guest_ms_;
            } else {
                // ★ 动作真的被应用 = 追加进去的那一批**全部**应用完（Key 是 down + up 两条）。
                applied = actuator_.appliedCount() > applied_before_ &&
                          actuator_.appliedCount() == actuator_.actionCount();
            }
            if (!applied) {
                // ★ **必须**在这里也有到期判定：`InputScript` 的帧停滞兜底是**一次性**的
                //   （`stall_fired_`），一旦折叠过，后面的 `at frame` 动作就没有第二次兜底。
                //   没有这一段时，动作永远不完成 ⇒ 图永远停在 `acting`：不失败、不 done、不退出
                //   （复审探针实测 2000 个块边界后 `state=acting actionsApplied=1/2`）。
                if (windowExpired(n.timeout_ms, guest_ms, wall_ms)) {
                    const bool blind = sampler.sampleCount() == samples_at_start_ ||
                                       sampler.blindSamples() > blind_at_start_;
                    fail(blind ? GraphFailure::NoSample : GraphFailure::Timeout,
                         std::string("动作在窗口内一直没能应用") +
                             (blind ? "（窗口内一次有效采样都没有）" : "（等待窗口到期）"),
                         n.when, frame, guest_ms, insns);
                    return true;
                }
                return false;
            }
            wait_pending_ = false;
            traceLine("action " + n.action.text + " applied");
            ++transitions_;
            if (n.has_expect) {
                // ★ 窗口**重记**：`expect` 问的是"动作之后变了没有"，起点必须是"动作已应用"这一刻。
                state_ = State::WaitExpect;
                wait_start_frame_ = frame;
                wait_start_insns_ = insns;
                wait_start_guest_ms_ = guest_ms;
                wait_start_wall_ms_ = wall_ms;
                samples_at_start_ = sampler.sampleCount();
                changes_at_start_ = sampler.changeCount();
                blind_at_start_ = sampler.blindSamples();
                last_progress_frame_ = frame;
                last_progress_insns_ = insns;
                last_progress_wall_ms_ = wall_ms;
                return true;
            }
            advance(frame, guest_ms, insns, wall_ms);
            return true;
        }
        case State::WaitExpect: {
            const Node& n = nodes_[current_];
            noteProgress(frame, insns, wall_ms);
            // ⛔ 这里**不能提前 return**：`expect` 没等到新样本时仍然要走下面的到期判定，
            //   否则"动作后再也没有采样"会永远停在 `wait-expect`（复审探针实测）。
            if (expectMayMatch(n.expect, sampler, samples_at_start_) &&
                condHolds(n.expect, sampler, samples_at_start_, changes_at_start_)) {
                traceLine("expect " + describeCond(n.expect) + " samples=" + u64s(sampler.sampleCount()) + " hit");
                advance(frame, guest_ms, insns, wall_ms);
                return true;
            }
            if (windowExpired(n.timeout_ms, guest_ms, wall_ms)) {
                const bool blind = sampler.sampleCount() == samples_at_start_ ||
                                   sampler.blindSamples() > blind_at_start_;
                fail(blind ? GraphFailure::NoSample : GraphFailure::ExpectChange,
                     blind ? std::string("动作已应用，但窗口内一次采样都没有（识别源失明）⇒ 无法确认变化")
                           : std::string("动作已应用，但 expect 条件到点仍不成立（动作没生效？）"),
                     n.expect, frame, guest_ms, insns);
                return true;
            }
            return false;
        }
        case State::Done:
        case State::Failed:
            return false;
    }
    return false;
}

}  // namespace lift
