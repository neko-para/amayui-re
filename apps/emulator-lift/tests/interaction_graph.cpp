// emulator-lift · 识别驱动交互状态机（节点图）的 pure 守卫
//
// ★ 判据（红了意味着什么）：`InteractionGraph` 的**解析 / 采样接线 / 状态机 / trace / 三种失败**
//   仍然满足 `docs/03-emulator/interaction-model.md` 与 `interaction_graph.h` 文件头写下的语义。
//   尤其：判定是**观测值的纯函数** —— 本文件自己给 `frame / guest_ms / insns / wall_ms`，
//   采样用 `FrameSampler::observeValues()` 注入，⛔ 一次也不靠真实墙钟推进。
// ★ 用例名 = kebab-case（需求单拿 `<文件>#<用例名>` 当 verify 锚）⇒ 每条都显式 `g_current_case`。
// ★ ⛔ 不断言"当前取值"（节点数 / 计数 / 字符串全文这类会因为下次改动而变红的东西）：
//   断言的是**关系**（"只进了 a 和 c，b 一次也没进"、"窗口内 0 采样 ⇒ NoSample"、"两遍 trace 相同"）。
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "check.h"
#include "runtime/host_window.h"
#include "runtime/input_script.h"
#include "runtime/interaction_graph.h"
#include "runtime/screen_sample.h"

#ifndef LIFT_SOURCE_DIR
#define LIFT_SOURCE_DIR "."
#endif

namespace {

bool startsWith(const std::string& s, const std::string& prefix) { return s.rfind(prefix, 0) == 0; }

bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

// trace 的读回：`setTrace` 收的是 `FILE*`，测试要拿文本比对 ⇒ 用 `tmpfile()`（C 标准库，
// 不落进工作树、不需要清理路径，也不会和别人的测试撞文件名）。
class TraceCapture {
public:
    TraceCapture() { file_ = std::tmpfile(); }
    ~TraceCapture() {
        if (file_ != nullptr) std::fclose(file_);
    }
    TraceCapture(const TraceCapture&) = delete;
    TraceCapture& operator=(const TraceCapture&) = delete;

    std::FILE* file() { return file_; }
    std::string text() {
        if (file_ == nullptr) return std::string();
        std::fflush(file_);
        std::rewind(file_);
        std::string out;
        char buf[4096];
        std::size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), file_)) > 0) out.append(buf, n);
        return out;
    }

private:
    std::FILE* file_ = nullptr;
};

// 确定性驱动器：`frame` 每步 +1、`guest_ms` / `wall_ms` 每步 +10、`insns` 每步 +1000。
// ★ 这些数字是**测试给的**，不是墙钟读数 ⇒ 同一串步骤两次运行逐位相同（trace-diff-stable 的前提）。
struct Driver {
    lift::WindowManager wm;
    lift::FrameSampler sampler;
    lift::InteractionGraph graph;
    std::uint64_t frame = 1;
    std::uint64_t guest_ms = 0;
    std::uint64_t insns = 0;
    std::uint64_t wall_ms = 0;
    std::uint64_t hash = 0x100;
    std::uint64_t hash_step = 1;
    std::uint64_t fed = 0;                        // 注入了多少次采样（= sampleCount 的期望值）
    std::vector<lift::RectSample> rects;          // 每次采样注入的矩形值（空 ⇒ 已登记矩形全部失效）
    // ★ 身份（`script` 条件的**电平**输入）：默认无效（= 没采到身份 ⇒ `script` 不成立）。
    //   测试里自己喂 —— 与真实 `Machine::last_marker_sample_` 同形。
    lift::MarkerSample identity;

    Driver() {
        // 点击的自动抬起要等 guest 采样；测试里没有 guest ⇒ 用一个小上限让它别挂在那里
        // （这是 `InputScript` 的显式上限，不是"绕过"）。
        graph.actuator().setMaxAutoHoldMs(16);
    }

    bool load(const std::string& text, const std::string& origin) {
        std::string err;
        const bool ok = graph.loadText(text, origin, &err);
        if (!ok) std::fprintf(stderr, "解析失败：%s\n", err.c_str());
        if (ok) graph.bindSampler(sampler);
        return ok;
    }

    // 一个块边界：先（可选）来一次采样，再 tick 图，再 tick 它驱动的 InputScript。
    void step(bool feed = true) {
        if (feed) {
            sampler.observeValues(frame, hash, rects);
            hash += hash_step;
            ++fed;
        }
        graph.tick(wm, sampler, frame, guest_ms, insns, wall_ms, &identity);
        graph.actuator().tick(wm, frame, guest_ms, insns, wall_ms);
        ++frame;
        guest_ms += 10;
        insns += 1000;
        wall_ms += 10;
    }

    // 跑到 `pred()` 成立或步数用完；返回 pred() 的最后一次结果。
    template <typename Pred>
    bool runUntil(Pred pred, int max_steps, bool feed = true) {
        for (int i = 0; i < max_steps; ++i) {
            if (pred()) return true;
            step(feed);
        }
        return pred();
    }
};

// 用于 `parse-errors`：一条图必须在 `origin:行:` 上报错，且图不留在半截状态。
void expectLoadError(const char* graph, int line, const char* needle) {
    lift::InteractionGraph g;
    std::string err;
    const bool ok = g.loadText(graph, "t.graph", &err);
    LIFT_CHECK(!ok);
    LIFT_CHECK(!g.active());  // 解析失败 ⇒ 不留半张图
    const std::string prefix = "t.graph:" + std::to_string(line) + ": ";
    LIFT_CHECK(startsWith(err, prefix));
    if (needle != nullptr && *needle != '\0') LIFT_CHECK(contains(err, needle));
    if (!startsWith(err, prefix)) std::fprintf(stderr, "  err = %s\n", err.c_str());
}

// `parse-ok` 用的合法图：除 `script` 外四种条件全用到，动作覆盖闭集合。
const char* kParseOkGraph =
    "# 一条合法图（注释行与空行都要被忽略）\n"
    "\n"
    "entry title\n"
    "node title\n"
    "  when screen hash 0xD6ED710894A39FA5\n"
    "  action click 944 562\n"
    "  next room\n"
    "  timeout 3000\n"
    "  expect screen changed\n"
    "node room\n"
    "  when screen rect 0 560 120 80 avg 0x1B2B3C\n"
    "  action click center\n"
    "  next idle\n"
    "node idle\n"
    "  when always\n"
    "  action key 0x1B\n"
    "  next w\n"
    "node w\n"
    "  when screen changed\n"
    "  action wait 3 frames\n"
    "  next p\n"
    "node p\n"
    "  when always\n"
    "  action profile_begin boot\n"
    "  next q\n"
    "node q\n"
    "  when always\n"
    "  action profile_end boot\n"
    "  next r\n"
    "node r\n"
    "  when always\n"
    "  action quit\n"
    "  next\n";

}  // namespace

int main() {
    using namespace lift_test;

    // =================================================================================
    // ① parse-ok —— 合法图的字段值 / `timeout` 缺省标记 / `GraphCond::text` 规范化
    // =================================================================================
    g_current_case = "parse-ok";
    {
        lift::InteractionGraph g;
        std::string err;
        const bool ok = g.loadText(kParseOkGraph, "ok.graph", &err);
        LIFT_CHECK(ok);
        if (!ok) {
            // 解析失败时后面那些字段断言没有意义（越界读只会让报告变成一次 crash）
            std::fprintf(stderr, "  err = %s\n", err.c_str());
            return finish("interaction-graph");
        }
        LIFT_CHECK(g.active());
        LIFT_CHECK(g.origin() == "ok.graph");
        LIFT_CHECK(g.entry() == "title");
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g.nodes().size()), 7u);
        LIFT_CHECK(g.findNode("room") != nullptr);
        LIFT_CHECK(g.findNode("nope") == nullptr);

        const lift::Node& title = g.nodes()[0];
        LIFT_CHECK(title.id == "title");
        LIFT_CHECK(title.when.kind == lift::CondKind::ScreenHash);
        LIFT_CHECK(title.when.hash == 0xD6ED710894A39FA5ull);
        // ★ `GraphCond::text` 是**规范化写法**（trace / 报错只打它）
        LIFT_CHECK(title.when.text == "screen hash 0xD6ED710894A39FA5");
        LIFT_CHECK(title.has_expect);
        LIFT_CHECK(title.expect.kind == lift::CondKind::ScreenChanged);
        LIFT_CHECK(title.expect.text == "screen changed");
        LIFT_CHECK(title.action.kind == lift::GraphActionKind::Click);
        LIFT_CHECK_EQ_I64(title.action.x, 944);
        LIFT_CHECK_EQ_I64(title.action.y, 562);
        LIFT_CHECK(title.next == "room");
        LIFT_CHECK(title.timeout_explicit);  // 显式写了 3000 ⇒ trace 不标 (default)
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(title.timeout_ms), 3000u);

        const lift::Node& room = g.nodes()[1];
        LIFT_CHECK(room.when.kind == lift::CondKind::ScreenRect);
        LIFT_CHECK_EQ_I64(room.when.rect.x, 0);
        LIFT_CHECK_EQ_I64(room.when.rect.y, 560);
        LIFT_CHECK_EQ_I64(room.when.rect.w, 120);
        LIFT_CHECK_EQ_I64(room.when.rect.h, 80);
        LIFT_CHECK_EQ_U32(room.when.avg_rgb, 0x1B2B3Cu);
        LIFT_CHECK(room.when.text == "screen rect 0 560 120 80 avg 0x1B2B3C");
        LIFT_CHECK(room.action.kind == lift::GraphActionKind::ClickCenter);
        LIFT_CHECK(room.action.text == "click center");
        // 省略 timeout ⇒ 固定缺省值 + `(default)` 标记
        LIFT_CHECK(!room.timeout_explicit);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(room.timeout_ms),
                          static_cast<std::uint32_t>(lift::kDefaultNodeTimeoutMs));

        const lift::Node& idle = g.nodes()[2];
        LIFT_CHECK(idle.when.kind == lift::CondKind::Always);
        LIFT_CHECK(idle.when.text == "always");
        LIFT_CHECK(idle.action.kind == lift::GraphActionKind::Key);
        LIFT_CHECK_EQ_U32(idle.action.vk, 0x1Bu);

        const lift::Node& w = g.nodes()[3];
        LIFT_CHECK(w.when.kind == lift::CondKind::ScreenChanged);
        LIFT_CHECK(w.when.text == "screen changed");
        LIFT_CHECK(w.action.kind == lift::GraphActionKind::Wait);
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(w.action.wait_value), 3u);
        LIFT_CHECK(w.action.wait_unit == lift::TriggerKind::Frame);

        LIFT_CHECK(g.nodes()[4].action.kind == lift::GraphActionKind::ProfileBegin);
        LIFT_CHECK(g.nodes()[4].action.label == "boot");
        LIFT_CHECK(g.nodes()[5].action.kind == lift::GraphActionKind::ProfileEnd);
        LIFT_CHECK(g.nodes()[6].action.kind == lift::GraphActionKind::Quit);
        LIFT_CHECK(g.nodes()[6].next.empty());  // `next`（不带 id）= 任务结束

        // ---- 采样接线只要求"接上不崩" ----
        // ⛔ 不断言 `rect_index` / `watchFrame()` / `rectAt(0)`：那是**接线的实现取舍**
        //   （换一种去重顺序、换一种登记策略就会红），`AGENTS.md` §10 明确不锁这类内部细节。
        //   真正要守的是"判定读的是哪些量"—— 由 ③-⑦ 的行为用例守（它们红才有意义）。
        lift::FrameSampler sampler;
        g.bindSampler(sampler);

        // ---- 失败转储的缺省名 ----
        LIFT_CHECK(lift::InteractionGraph::defaultFailDumpPath("a/b.graph") == "a/b.graph.fail.bmp");
    }

    // =================================================================================
    // ② parse-errors —— 逐条错误各一个断言，且错误串带**行号**
    // =================================================================================
    g_current_case = "parse-errors";
    {
        // 未知 `next` id：next 行 = 5
        expectLoadError(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action quit\n"
            "  next ghost\n",
            5, "ghost");
        // 缺 `entry`：报在最后一行（本图 4 行）
        expectLoadError(
            "node a\n"
            "  when always\n"
            "  action quit\n"
            "  next\n",
            4, "entry");
        // ★ `when script <名字>` 现在是**合法**条件（电平）—— 旧策略（解析期拒绝）的断言已按
        //   需求单 `REQ-01M74Z3DBK8K5MQVHZPCEX2SXP` 改成新语义，见 `script-cond-forms`。
        //   这里只留"词数不对"这一类**仍然**是解析期错误的形态。
        expectLoadError(
            "entry a\n"
            "node a\n"
            "  when script\n"
            "  action quit\n"
            "  next\n",
            3, "script");
        expectLoadError(
            "entry a\n"
            "node a\n"
            "  when script TITLE GAMESTART\n"
            "  action quit\n"
            "  next\n",
            3, "script");
        // `click center` 的 when 不是 `screen rect`（框中心没有坐标来源）
        expectLoadError(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action click center\n"
            "  next\n",
            4, "click center");
        // 块内重复键
        expectLoadError(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  when screen changed\n"
            "  action quit\n"
            "  next\n",
            4, "when");
        // 未知动作
        expectLoadError(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action jump\n"
            "  next\n",
            4, "jump");
        // `wait` 的单位只认 frames / ms
        expectLoadError(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action wait 3 hours\n"
            "  next\n",
            4, "hours");
        // VK 必须在 1..255
        expectLoadError(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action key 300\n"
            "  next\n",
            4, "VK");
        // 数字 / hex 解析失败
        expectLoadError(
            "entry a\n"
            "node a\n"
            "  when screen hash 0xZZ\n"
            "  action quit\n"
            "  next\n",
            3, "0xZZ");
        // 重复 `node id`
        expectLoadError(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action quit\n"
            "  next\n"
            "node a\n"
            "  when always\n"
            "  action quit\n"
            "  next\n",
            6, "重复");
        // 块内缺 `next`（报在 node 行）
        expectLoadError(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action quit\n",
            2, "next");
        // `entry` 指向不存在的节点（报在 entry 行）
        expectLoadError(
            "entry ghost\n"
            "node a\n"
            "  when always\n"
            "  action quit\n"
            "  next\n",
            1, "ghost");
        // 未知键
        expectLoadError(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action quit\n"
            "  next\n"
            "  retry 3\n",
            6, "retry");
    }

    // =================================================================================
    // ①b parse-uppercase-ok —— 大小写不敏感**不改变语义**：大写图与小写图解析成同一批字段
    // ★ 为什么单列一条：报错串与 `text` 的口径是**刻意混着**的（键名 / 条件头小写化，未知词回显
    //   **原文**，`action` 的 `text` 保留**原始词序**）。任何"顺手统一规范化"的重构都会改字节，
    //   而其余用例与三份真图**全是小写** ⇒ 没有这条就静默通过。
    // ⛔ 断言的是**契约**（大写输入 ⇒ 同一个 kind + 归一化后的 text），不是"当前取值"。
    // =================================================================================
    g_current_case = "parse-uppercase-ok";
    {
        lift::InteractionGraph g;
        std::string err;
        // ★ 大写覆盖**每一个**靠 `lowerCopy` 的词位：键名 / 条件头 / `screen` 子词 / `rect` 的 `avg`
        //   / `click` 的 `right` / `wait` 的单位词 / `profile_begin` / `exit`（后者全仓只在这里出现）。
        //   任何一处去掉 `lowerCopy` 都必须让本用例红 —— 漏一处就等于漏一个静默改坏的机会。
        const bool ok = g.loadText(
            "ENTRY A\n"
            "NODE A\n"
            "  WHEN SCREEN HASH 0xdeadbeef\n"
            "  ACTION CLICK 10 20 RIGHT\n"
            "  NEXT B\n"
            "  TIMEOUT 1500\n"
            "  EXPECT SCREEN CHANGED\n"
            "node B\n"
            "  WHEN SCREEN RECT 0 560 120 80 AVG 0x1B2B3C\n"
            "  ACTION WAIT 3 FRAMES\n"
            "  NEXT C\n"
            "node C\n"
            "  when ALWAYS\n"
            "  action WAIT 30 MS\n"
            "  next D\n"
            "node D\n"
            "  when always\n"
            "  action PROFILE_BEGIN Boot\n"
            "  next E\n"
            "node E\n"
            "  when always\n"
            "  action EXIT\n"
            "  next\n",
            "up.graph", &err);
        LIFT_CHECK(ok);
        // ★ 解析失败 ⇒ **不要**往下索引：失败时 `nodes()` 是空的，`nodes()[i]` 是 UB
        //   （实测：`screen rect … AVG` 那格坏掉时，旧写法把"一条断言红"变成 **SEGFAULT**，
        //   连 `finish()` 的汇总行都打不出来）。⇒ 其余断言挂在 `else` 上。
        if (!ok) {
            std::fprintf(stderr, "  err = %s\n", err.c_str());
        } else {
            LIFT_CHECK(g.active());
            LIFT_CHECK(g.nodes().size() == 5);
            const lift::Node& a = g.nodes()[0];
            LIFT_CHECK(a.when.kind == lift::CondKind::ScreenHash);
            LIFT_CHECK(a.when.hash == 0xDEADBEEFull);
            // 归一化：条件头一律小写 + hex 一律 `0x%016X`（trace 可 diff 的前提）
            LIFT_CHECK(a.when.text == "screen hash 0x00000000DEADBEEF");
            LIFT_CHECK(a.action.kind == lift::GraphActionKind::Click);
            LIFT_CHECK(a.action.x == 10 && a.action.y == 20);
            LIFT_CHECK(a.action.button == lift::MouseButton::Right);
            // ⚠ `action` 的 `text` 是**原始词序**（trace 打 `action <text> applied`）⇒ 大写原样留着
            LIFT_CHECK(a.action.text == "CLICK 10 20 RIGHT");
            LIFT_CHECK(a.has_expect);
            LIFT_CHECK(a.expect.kind == lift::CondKind::ScreenChanged);
            LIFT_CHECK(a.expect.text == "screen changed");
            LIFT_CHECK(a.timeout_ms == 1500 && a.timeout_explicit);
            // `screen rect … AVG …`：子词与 `avg` 关键字都是大写进来的
            const lift::Node& b = g.nodes()[1];
            LIFT_CHECK(b.when.kind == lift::CondKind::ScreenRect);
            LIFT_CHECK(b.when.rect.x == 0 && b.when.rect.y == 560);
            LIFT_CHECK(b.when.rect.w == 120 && b.when.rect.h == 80);
            LIFT_CHECK(b.when.avg_rgb == 0x1B2B3Cu);
            LIFT_CHECK(b.when.text == "screen rect 0 560 120 80 avg 0x1B2B3C");
            LIFT_CHECK(b.action.kind == lift::GraphActionKind::Wait);
            LIFT_CHECK(b.action.wait_unit == lift::TriggerKind::Frame);
            LIFT_CHECK(b.action.wait_value == 3);
            // 单位词两个方向各一次：`MS` ⇒ GuestMs
            LIFT_CHECK(g.nodes()[2].action.kind == lift::GraphActionKind::Wait);
            LIFT_CHECK(g.nodes()[2].action.wait_unit == lift::TriggerKind::GuestMs);
            LIFT_CHECK(g.nodes()[2].action.wait_value == 30);
            // `PROFILE_BEGIN`：label 是**原样**的（不归一化）
            LIFT_CHECK(g.nodes()[3].action.kind == lift::GraphActionKind::ProfileBegin);
            LIFT_CHECK(g.nodes()[3].action.label == "Boot");
            // `EXIT` 与 `quit` 同义（同一个 kind）
            LIFT_CHECK(g.nodes()[4].action.kind == lift::GraphActionKind::Quit);
            LIFT_CHECK(g.nodes()[4].next.empty());
        }
    }

    // =================================================================================
    // ①c parse-unknown-echoes-raw —— 未知词**原样回显**（大小写不被吞掉）
    // ★ 三条分别走 `parseCond` / `parseAction` / 主循环的"未知"分支 —— 这三处的报错串用的是
    //   **原文 token**，不是小写化后的词。改成小写化不会有别的用例红，但人就看不到自己写的是
    //   `FOO` 还是 `foo` 了。
    // =================================================================================
    g_current_case = "parse-unknown-echoes-raw";
    {
        expectLoadError("entry a\nnode a\n  when SCREEN FOO\n  action quit\n  next\n", 3, "FOO");
        expectLoadError("entry a\nnode a\n  when always\n  action JUMP\n  next\n", 4, "JUMP");
        expectLoadError("entry a\nnode a\n  when always\n  action quit\n  next\n  RETRY 3\n", 6, "RETRY");
    }

    // =================================================================================
    // ②c script-cond-forms —— `script <文本>` 的两种判定口径（**电平**条件）
    // ★ 红在这意味着：`script` 又退回"解析期拒绝"，或数字/名字分派错了口径
    //   （名字：大小写不敏感 + 可省略 `.BIN`；数字：十进制 / 0x ⇒ 按 id）。
    // =================================================================================
    g_current_case = "script-cond-forms";
    {
        lift::InteractionGraph g;
        std::string err;
        const bool ok = g.loadText(
            "node a\n"
            "  when script TITLE\n"
            "  action quit\n"
            "  next b\n"
            "node b\n"
            "  when script TITLE.BIN\n"
            "  action quit\n"
            "  next c\n"
            "node c\n"
            "  when script title.bin\n"
            "  action quit\n"
            "  next d\n"
            "node d\n"
            "  when script 21092\n"
            "  action quit\n"
            "  next e\n"
            "node e\n"
            "  when script 0x5264\n"
            "  action quit\n"
            "  next f\n"
            "node f\n"
            "  when script id:0x0000210A\n"
            "  action quit\n"
            "  next\n"
            "entry a\n",
            "synthetic:script-conds", &err);
        LIFT_CHECK(ok);
        if (!ok) std::fprintf(stderr, "  err = %s\n", err.c_str());
        if (ok) {
            // 名字形态：**原文**保留（报错 / trace 用），判定按名字
            const lift::Node& a = g.nodes()[0];
            LIFT_CHECK(a.when.kind == lift::CondKind::Script);
            LIFT_CHECK(!a.when.script_by_id);
            LIFT_CHECK(a.when.script == "TITLE");
            LIFT_CHECK(a.when.text == "script TITLE");
            LIFT_CHECK(!g.nodes()[1].when.script_by_id);  // TITLE.BIN
            LIFT_CHECK(!g.nodes()[2].when.script_by_id);  // title.bin（大小写不敏感）
            LIFT_CHECK(g.nodes()[1].when.script == "TITLE.BIN");
            // 数字形态：十进制 / 0x 都 ⇒ 按 id（`0x5264 == 21092`）
            LIFT_CHECK(g.nodes()[3].when.script_by_id);
            LIFT_CHECK_EQ_U32(g.nodes()[3].when.script_id, 21092u);
            LIFT_CHECK(g.nodes()[4].when.script_by_id);
            LIFT_CHECK_EQ_U32(g.nodes()[4].when.script_id, 0x5264u);
            // `id:0x…` 是**退化名**（不是数字 ⇒ 按名字走那条"始终接受"的路）
            LIFT_CHECK(!g.nodes()[5].when.script_by_id);
            LIFT_CHECK(g.nodes()[5].when.script == "id:0x0000210A");
        }
    }

    // =================================================================================
    // ②d script-cond-identity —— `condHolds` 用**假身份**注入（不碰真实 VM / 采样点）
    // ★ 红在这意味着：`script` 的判据不再是"valid && (按 id / 按名字)"，或退化名 / 省略后缀 /
    //   大小写中的一条口径漂了。
    // =================================================================================
    g_current_case = "script-cond-identity";
    {
        lift::FrameSampler sampler;  // 画面侧一概不喂：`script` 只读身份
        auto condName = [](const std::string& want) {
            lift::GraphCond c;
            c.kind = lift::CondKind::Script;
            c.script = want;
            c.text = "script " + want;
            return c;
        };
        auto condId = [](std::uint32_t id) {
            lift::GraphCond c;
            c.kind = lift::CondKind::Script;
            c.script_by_id = true;
            c.script_id = id;
            return c;
        };
        lift::MarkerSample ident;
        ident.valid = true;
        ident.cur = 7;
        ident.id = 21092;
        ident.name = "TITLE.BIN";

        LIFT_CHECK(lift::condHolds(condName("TITLE"), sampler, 0, 0, &ident));      // 省略 `.BIN`
        LIFT_CHECK(lift::condHolds(condName("title"), sampler, 0, 0, &ident));      // 大小写不敏感
        LIFT_CHECK(lift::condHolds(condName("TITLE.BIN"), sampler, 0, 0, &ident));  // 全名
        LIFT_CHECK(lift::condHolds(condId(21092), sampler, 0, 0, &ident));          // 按 id
        LIFT_CHECK(!lift::condHolds(condName("GAMESTART"), sampler, 0, 0, &ident)); // 不命中
        LIFT_CHECK(!lift::condHolds(condId(1), sampler, 0, 0, &ident));             // id 不命中
        // ★ 没有身份（没采样 / base 未发布）⇒ 不成立；⛔ 不是"无条件成立"也不是 always
        LIFT_CHECK(!lift::condHolds(condName("TITLE"), sampler, 0, 0, nullptr));
        ident.valid = false;
        LIFT_CHECK(!lift::condHolds(condName("TITLE"), sampler, 0, 0, &ident));
        // ★ 降级名 `id:0x%08X` **始终接受**（名字表某一刻读不到不该让条件静默失明）
        ident.valid = true;
        ident.id = 0x210A;
        ident.name = "id:0x0000210A";
        ident.name_degraded = true;
        LIFT_CHECK(lift::condHolds(condName("id:0x0000210A"), sampler, 0, 0, &ident));
        LIFT_CHECK(lift::condHolds(condName("ID:0x0000210a"), sampler, 0, 0, &ident));
        LIFT_CHECK(!lift::condHolds(condName("TITLE"), sampler, 0, 0, &ident));  // 降级名不是 TITLE
        LIFT_CHECK(lift::condHolds(condId(0x210A), sampler, 0, 0, &ident));      // 按 id 仍成立
    }

    // =================================================================================
    // ②e linear-parse —— 线性写法展开成同一份 `Node` IR（id / when / action / next / timeout / entry）
    // ★ 红在这意味着：`await` 的条件没落到 `when`、动作行没落到 `action`、隐式 next 链断了，
    //   或 `timeout` / `expect` 的作用域不再是"当前 step"。
    // =================================================================================
    g_current_case = "linear-parse";
    {
        lift::InteractionGraph g;
        std::string err;
        const bool ok = g.loadText(
            "await script TITLE\n"
            "click 1174 377\n"
            "timeout 2000\n"
            "expect screen changed\n"
            "await script GAMESTART\n"
            "click 715 576\n"
            "await script SN0000\n"
            "click 100 100\n",
            "synthetic:linear", &err);
        LIFT_CHECK(ok);
        if (!ok) std::fprintf(stderr, "  err = %s\n", err.c_str());
        if (ok) {
            // 线性模式不写 `entry` ⇒ entry = 第一个 step
            LIFT_CHECK(g.entry() == "step1");
            LIFT_CHECK(g.nodes().size() == 3);
            LIFT_CHECK(g.findNode("step2") != nullptr);
            LIFT_CHECK(g.findNode("step3") != nullptr);

            const lift::Node& s1 = g.nodes()[0];
            LIFT_CHECK(s1.id == "step1");
            LIFT_CHECK(s1.when.kind == lift::CondKind::Script);
            LIFT_CHECK(s1.when.script == "TITLE");
            LIFT_CHECK(s1.action.kind == lift::GraphActionKind::Click);
            LIFT_CHECK_EQ_I64(s1.action.x, 1174);
            LIFT_CHECK_EQ_I64(s1.action.y, 377);
            LIFT_CHECK(s1.next == "step2");           // 隐式 next = 下一个 step
            LIFT_CHECK(s1.timeout_explicit);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(s1.timeout_ms), 2000u);
            LIFT_CHECK(s1.has_expect);
            LIFT_CHECK(s1.expect.kind == lift::CondKind::ScreenChanged);

            const lift::Node& s2 = g.nodes()[1];
            LIFT_CHECK(s2.id == "step2");
            LIFT_CHECK(s2.when.script == "GAMESTART");
            LIFT_CHECK(s2.next == "step3");
            LIFT_CHECK(!s2.has_expect);       // ⛔ 线性写法**不默认** expect
            LIFT_CHECK(!s2.timeout_explicit); // 省略 ⇒ 缺省值

            const lift::Node& s3 = g.nodes()[2];
            LIFT_CHECK(s3.id == "step3");
            LIFT_CHECK(s3.when.kind == lift::CondKind::Script);
            LIFT_CHECK(s3.next.empty());      // 最后一个 step ⇒ 任务结束
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(s3.timeout_ms),
                              static_cast<std::uint32_t>(lift::kDefaultNodeTimeoutMs));
            // trace 里的动作写法 = 原始行（⛔ 不带 `action` 键名）
            LIFT_CHECK(s3.action.text == "click 100 100");
        }
    }

    // =================================================================================
    // ②f linear-mixed-and-always —— 没有 `await` 的动作行 = `always` step；与 `node` 块可混用；
    //     写了 `entry` 就照旧（线性缺省只在**没写** entry 时生效）
    // =================================================================================
    g_current_case = "linear-mixed-and-always";
    {
        lift::InteractionGraph g;
        std::string err;
        const bool ok = g.loadText(
            "node boot\n"
            "  when always\n"
            "  action quit\n"
            "  next\n"
            "click 10 10\n"          // 完整的 node 块之后 = 新 step（always）
            "timeout 500\n"
            "await screen changed\n"
            "key 27\n"
            "entry boot\n",          // 写了 entry ⇒ entry = boot（不是第一个 step）
            "synthetic:linear-mixed", &err);
        LIFT_CHECK(ok);
        if (!ok) std::fprintf(stderr, "  err = %s\n", err.c_str());
        if (ok) {
            LIFT_CHECK(g.entry() == "boot");
            LIFT_CHECK(g.nodes().size() == 3);
            LIFT_CHECK(g.nodes()[0].id == "boot");
            const lift::Node& a = g.nodes()[1];
            LIFT_CHECK(a.id == "step1");
            LIFT_CHECK(a.when.kind == lift::CondKind::Always);  // 没有 await ⇒ always
            LIFT_CHECK(a.action.kind == lift::GraphActionKind::Click);
            LIFT_CHECK(a.next == "step2");
            LIFT_CHECK(a.timeout_explicit);
            const lift::Node& b = g.nodes()[2];
            LIFT_CHECK(b.id == "step2");
            LIFT_CHECK(b.when.kind == lift::CondKind::ScreenChanged);  // await screen changed
            LIFT_CHECK(b.action.kind == lift::GraphActionKind::Key);
            LIFT_CHECK(b.next.empty());
        }
    }

    // =================================================================================
    // ②g linear-parse-errors —— 线性写法的解析期失败一律 `origin:行: 原因`
    // =================================================================================
    g_current_case = "linear-parse-errors";
    {
        // `await` 之后没有动作行（step 缺动作）
        expectLoadError("await script TITLE\n", 1, "step1");
        // 连续两个 `await`：前一个 step 还没有动作
        expectLoadError("await script TITLE\nawait script GAMESTART\nclick 1 1\n", 1, "step1");
        // step id 与已有 `node` id 冲突（报在 `await` / 动作行上）
        expectLoadError(
            "node step1\n  when always\n  action quit\n  next\n"
            "await script TITLE\nclick 1 1\n",
            5, "step1");
        // `node` 块里漏写 `action`，直接写了动作行 ⇒ 报在动作行（不静默多出一张图）
        expectLoadError("node a\n  when always\nclick 1 1\n", 3, "action");
        // step 里不能写 `next`（隐式）也不能写 `when`（条件来自 `await`）
        expectLoadError("await script TITLE\nclick 1 1\nnext\n", 3, "step");
        expectLoadError("await script TITLE\nwhen always\nclick 1 1\n", 2, "step");
        // `await` 后面的条件仍走同一个闭集合校验
        expectLoadError("await jump\nclick 1 1\n", 1, "jump");
        // `timeout` 仍只收毫秒数
        expectLoadError("await script TITLE\nclick 1 1\ntimeout later\n", 3, "later");
    }

    // =================================================================================
    // ②b parse-button-right —— 按钮词 `right` 必须落到 `GraphAction::button`
    // ★ 这条在"只收 `click center` / `click x y`"的旧解析器上必红（`click 10 20 right` 是 4 个词 ⇒
    //   旧码直接报错；`click center right` 旧码会把 `center` 当坐标解析失败）。
    // =================================================================================
    g_current_case = "parse-button-right";
    {
        lift::InteractionGraph g;
        std::string err;
        const bool ok = g.loadText(
            "entry a\n"
            "node a\n"
            "  when screen rect 0 0 10 10 avg 0x112233\n"
            "  action click center right\n"
            "  next b\n"
            "node b\n"
            "  when always\n"
            "  action click 10 20 right\n"
            "  next\n",
            "synthetic:button-right", &err);
        LIFT_CHECK(ok);
        if (!ok) std::fprintf(stderr, "  err = %s\n", err.c_str());
        if (ok) {
            const lift::Node& center = g.nodes()[0];
            LIFT_CHECK(center.action.kind == lift::GraphActionKind::ClickCenter);
            LIFT_CHECK(center.action.button == lift::MouseButton::Right);
            const lift::Node& fixed = g.nodes()[1];
            LIFT_CHECK(fixed.action.kind == lift::GraphActionKind::Click);
            LIFT_CHECK_EQ_I64(fixed.action.x, 10);
            LIFT_CHECK_EQ_I64(fixed.action.y, 20);
            LIFT_CHECK(fixed.action.button == lift::MouseButton::Right);
        }
    }

    // =================================================================================
    // ②c parse-button-default —— 省略按钮词 = `left`（两种写法都要）
    // =================================================================================
    g_current_case = "parse-button-default";
    {
        lift::InteractionGraph g;
        std::string err;
        const bool ok = g.loadText(
            "entry a\n"
            "node a\n"
            "  when screen rect 0 0 10 10 avg 0x112233\n"
            "  action click center\n"
            "  next b\n"
            "node b\n"
            "  when always\n"
            "  action click 944 562\n"
            "  next\n",
            "synthetic:button-default", &err);
        LIFT_CHECK(ok);
        if (!ok) std::fprintf(stderr, "  err = %s\n", err.c_str());
        if (ok) {
            LIFT_CHECK(g.nodes()[0].action.kind == lift::GraphActionKind::ClickCenter);
            LIFT_CHECK(g.nodes()[0].action.button == lift::MouseButton::Left);
            LIFT_CHECK_EQ_I64(g.nodes()[1].action.x, 944);
            LIFT_CHECK_EQ_I64(g.nodes()[1].action.y, 562);
            LIFT_CHECK(g.nodes()[1].action.button == lift::MouseButton::Left);
        }
    }

    // =================================================================================
    // ②d parse-button-illegal —— 词表外的词在**解析期**报错（带行号，且点名合法词）
    // ★ ⛔ 不静默退化成 left：那会让"写错一个词"变成"点了另一个按钮"。
    // =================================================================================
    g_current_case = "parse-button-illegal";
    {
        lift::InteractionGraph g;
        std::string err;
        const bool ok = g.loadText(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action click 1 2 sideways\n"
            "  next\n",
            "t.graph", &err);
        LIFT_CHECK(!ok);
        LIFT_CHECK(!g.active());  // 解析失败 ⇒ 不留半张图
        LIFT_CHECK(startsWith(err, "t.graph:4: "));
        LIFT_CHECK(contains(err, "left"));      // 合法词（词表真源见 interaction-model.md §1）
        LIFT_CHECK(contains(err, "right"));
        LIFT_CHECK(contains(err, "sideways"));  // 点名收到的是什么
        if (!startsWith(err, "t.graph:4: ")) std::fprintf(stderr, "  err = %s\n", err.c_str());
        // `click center <坏词>` 走同一条词表检查（不是"center 后面什么都不看"）
        lift::InteractionGraph g2;
        std::string err2;
        const bool ok2 = g2.loadText(
            "entry a\n"
            "node a\n"
            "  when screen rect 0 0 10 10 avg 0x112233\n"
            "  action click center sideways\n"
            "  next\n",
            "t.graph", &err2);
        LIFT_CHECK(!ok2);
        LIFT_CHECK(startsWith(err2, "t.graph:4: "));
        LIFT_CHECK(contains(err2, "right"));
    }

    // =================================================================================
    // ②e emit-button-carried —— 动作发到时间线之后，`actions()` 里那条 Click 的按钮对得上
    // ★ ⛔ 不锁字符串（"click … right" 的拼写不属于对外合同）：断言的是**按钮这个值**活着穿过
    //   解析 → 状态机 → actuator 三层。`input_script.h` 的 `Action::button` / `actions()` 是公开 seam。
    // ★ 两条 emit 路径（Click / ClickCenter）各跑一次：`issueAction` 里它们是两段独立代码。
    // =================================================================================
    g_current_case = "emit-button-carried";
    {
        auto firstClick = [](const lift::InputScript& script) -> const lift::InputScript::Action* {
            for (const lift::InputScript::Action& a : script.actions()) {
                if (a.kind == lift::InputScript::Kind::Click) return &a;
            }
            return nullptr;
        };

        // ① `click x y right` ⇒ Click 路径
        {
            Driver d;
            LIFT_CHECK(d.load(
                "entry a\n"
                "node a\n"
                "  when always\n"
                "  action click 10 20 right\n"
                "  next\n",
                "synthetic:emit-right"));
            LIFT_CHECK(d.runUntil([&] { return d.graph.actuator().appliedCount() >= 1; }, 40));
            const lift::InputScript::Action* a = firstClick(d.graph.actuator());
            LIFT_CHECK(a != nullptr);
            if (a != nullptr) LIFT_CHECK(a->button == lift::MouseButton::Right);
        }
        // ② `click center right` ⇒ ClickCenter 路径（漏改那一段就只有这里红）
        {
            Driver d;
            d.rects.push_back(lift::RectSample{/*valid=*/true, /*avg_rgb=*/0x112233, /*hash=*/0x1});
            LIFT_CHECK(d.load(
                "entry a\n"
                "node a\n"
                "  when screen rect 0 0 10 10 avg 0x112233\n"
                "  action click center right\n"
                "  next\n",
                "synthetic:emit-right-center"));
            LIFT_CHECK(d.runUntil([&] { return d.graph.actuator().appliedCount() >= 1; }, 40));
            const lift::InputScript::Action* a = firstClick(d.graph.actuator());
            LIFT_CHECK(a != nullptr);
            if (a != nullptr) LIFT_CHECK(a->button == lift::MouseButton::Right);
        }
        // ③ 左键（缺省）⇒ Left（同一条 seam 的另一半：⛔ 别把"缺省"变成别的按钮）
        {
            Driver d;
            LIFT_CHECK(d.load(
                "entry a\n"
                "node a\n"
                "  when always\n"
                "  action click 10 20\n"
                "  next\n",
                "synthetic:emit-left"));
            LIFT_CHECK(d.runUntil([&] { return d.graph.actuator().appliedCount() >= 1; }, 40));
            const lift::InputScript::Action* a = firstClick(d.graph.actuator());
            LIFT_CHECK(a != nullptr);
            if (a != nullptr) LIFT_CHECK(a->button == lift::MouseButton::Left);
        }
    }

    // =================================================================================
    // ②f button-word-accepted-by-timeline —— 图认的每个按钮词，时间线也必须认
    // ★ 防"图发出一个时间线不认识的字"：那会在**运行期**追加动作时解析失败（图已开跑才发现）。
    // =================================================================================
    g_current_case = "button-word-accepted-by-timeline";
    {
        struct WordCase {
            const char* word;
            lift::MouseButton button;
        };
        const WordCase cases[] = {{"left", lift::MouseButton::Left}, {"right", lift::MouseButton::Right}};
        for (const WordCase& c : cases) {
            lift::InputScript script;
            std::string err;
            const std::string text = std::string("at frame 2\nclick 1 1 ") + c.word + "\n";
            const bool ok = script.appendText(text, "synthetic:timeline-word", &err);
            if (!ok) std::fprintf(stderr, "  word=%s err=%s\n", c.word, err.c_str());
            LIFT_CHECK(ok);
            if (!ok) continue;
            LIFT_CHECK(script.actionCount() >= 1);
            bool found = false;
            for (const lift::InputScript::Action& a : script.actions()) {
                if (a.kind != lift::InputScript::Kind::Click) continue;
                found = true;
                LIFT_CHECK(a.button == c.button);  // 同一个词在两层里指的是同一个按钮
            }
            LIFT_CHECK(found);
        }
    }

    // =================================================================================
    // ③ only-next-target —— `next` 是**单个** id：a→c（b 存在但没人指向它）⇒ b 一次也不进
    // =================================================================================
    g_current_case = "only-next-target";
    {
        Driver d;
        TraceCapture cap;
        d.graph.setTrace(cap.file(), true);
        LIFT_CHECK(d.load(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action wait 1 frames\n"
            "  next c\n"
            "node b\n"
            "  when always\n"
            "  action quit\n"
            "  next\n"
            "node c\n"
            "  when always\n"
            "  action wait 1 frames\n"
            "  next\n",
            "synthetic:next"));
        LIFT_CHECK(d.runUntil([&] { return d.graph.done() || d.graph.failed(); }, 40));
        LIFT_CHECK(d.graph.done());
        LIFT_CHECK(!d.graph.failed());
        const std::string trace = cap.text();
        LIFT_CHECK(contains(trace, "node a enter"));
        LIFT_CHECK(contains(trace, "node c enter"));
        // ★ 判据：b 一次也没进（⛔ 不是"候选列表轮询"）
        LIFT_CHECK(!contains(trace, "node b enter"));
        LIFT_CHECK(contains(trace, "task done"));
    }

    // =================================================================================
    // ④ timeout-loud —— 喂"不匹配"的采样 ⇒ Timeout，报告含节点 / 条件文本 / 采样数
    // =================================================================================
    g_current_case = "timeout-loud";
    {
        Driver d;
        d.rects.push_back(lift::RectSample{/*valid=*/true, /*avg_rgb=*/0x000000, /*hash=*/0x1});
        LIFT_CHECK(d.load(
            "entry watch\n"
            "node watch\n"
            "  when screen rect 0 560 120 80 avg 0xFFFFFF\n"
            "  action quit\n"
            "  next\n"
            "  timeout 100\n",
            "synthetic:timeout"));
        LIFT_CHECK(d.runUntil([&] { return d.graph.failed(); }, 200));
        LIFT_CHECK(d.graph.failed());
        LIFT_CHECK(d.graph.failure() == lift::GraphFailure::Timeout);
        LIFT_CHECK(std::strcmp(d.graph.stateName(), "failed") == 0);
        const std::string report = d.graph.failureReport();
        LIFT_CHECK(contains(report, "timeout"));
        LIFT_CHECK(contains(report, "watch"));                                    // 节点 id
        LIFT_CHECK(contains(report, "screen rect 0 560 120 80 avg 0xFFFFFF"));    // 等的是什么
        LIFT_CHECK(contains(report, "total=" + std::to_string(d.fed)));           // 累计采样数
        LIFT_CHECK(contains(report, "last_avg=0x000000"));                        // 最后一次采样值
        LIFT_CHECK(contains(report, "hit=0"));
        LIFT_CHECK(!d.graph.failureDetail().empty());
        // ⛔ 失败之后不再重复报
        LIFT_CHECK(!d.graph.tick(d.wm, d.sampler, d.frame, d.guest_ms, d.insns, d.wall_ms, &d.identity));
    }

    // =================================================================================
    // ④b script-timeout-report —— `script` 条件失败时，报告必须写出**最后一次身份**
    // ★ 红在这意味着：`failureReport()` 丢了 `identity :` 行（"等 TITLE 超时"就分不清是
    //   "还在别的屏"还是"识别源失明"），或降级样本没标出来。
    // ⛔ 只断言身份（我们喂进去的那个）与"降级"标记，不断言帧号 / 耗时 / 采样数。
    // =================================================================================
    g_current_case = "script-timeout-report";
    {
        Driver d;
        LIFT_CHECK(d.load(
            "await script TITLE\n"
            "timeout 100\n"
            "quit\n",
            "synthetic:script-report"));
        // 假身份：名字与 id 都预置好（名字不命中 TITLE ⇒ 必然超时，报告里就是它）
        d.identity.valid = true;
        d.identity.cur = 7;
        d.identity.id = 0x210A;
        d.identity.name = "GAMESTART.BIN";
        d.identity.name_degraded = false;
        LIFT_CHECK(d.runUntil([&] { return d.graph.failed(); }, 200));
        LIFT_CHECK(d.graph.failure() == lift::GraphFailure::Timeout);
        const std::string report = d.graph.failureReport();
        LIFT_CHECK(contains(report, "waiting  : script TITLE"));
        // 名字 / id 都来自**喂进去的**假身份（不是读数）
        LIFT_CHECK(contains(report, "identity : cur=7 id=0x0000210A name=GAMESTART.BIN"));
        LIFT_CHECK(!contains(report, "（降级）"));

        // 降级样本 ⇒ 名字是 `id:0x…`，报告必须标出"（降级）"
        Driver deg;
        LIFT_CHECK(deg.load(
            "await script TITLE\n"
            "timeout 100\n"
            "quit\n",
            "synthetic:script-report-degraded"));
        deg.identity.valid = true;
        deg.identity.cur = 7;
        deg.identity.id = 0x210A;
        deg.identity.name = "id:0x0000210A";
        deg.identity.name_degraded = true;
        LIFT_CHECK(deg.runUntil([&] { return deg.graph.failed(); }, 200));
        const std::string degraded_report = deg.graph.failureReport();
        LIFT_CHECK(contains(degraded_report,
                            "identity : cur=7 id=0x0000210A name=id:0x0000210A（降级）"));
    }

    // =================================================================================
    // ⑤ no-sample-blind —— 一次采样都不喂 ⇒ NoSample（与 ④ 分开：识别源失明 ≠ 画面不对）
    // =================================================================================
    g_current_case = "no-sample-blind";
    {
        Driver d;
        LIFT_CHECK(d.load(
            "entry watch\n"
            "node watch\n"
            "  when screen rect 0 560 120 80 avg 0x1B2B3C\n"
            "  action quit\n"
            "  next\n"
            "  timeout 100\n",
            "synthetic:nosample"));
        LIFT_CHECK(d.runUntil([&] { return d.graph.failed(); }, 200, /*feed=*/false));
        LIFT_CHECK(d.graph.failed());
        LIFT_CHECK(d.graph.failure() == lift::GraphFailure::NoSample);
        const std::string report = d.graph.failureReport();
        LIFT_CHECK(contains(report, "no-sample"));
        LIFT_CHECK(contains(report, "window=0"));  // 窗口内一次采样都没有
        LIFT_CHECK(contains(report, "total=0"));
    }

    // =================================================================================
    // ⑥ expect-change-loud —— 动作已应用（actuator 真的应用了）但画面不变 ⇒ ExpectChange
    // =================================================================================
    g_current_case = "expect-change-loud";
    {
        Driver d;
        d.hash_step = 0;  // 每次采样的整帧 hash 都一样 ⇒ 不会有 `screen changed`
        LIFT_CHECK(d.load(
            "entry act\n"
            "node act\n"
            "  when always\n"
            "  action click 100 200\n"
            "  next\n"
            "  timeout 100\n"
            "  expect screen changed\n",
            "synthetic:expect"));
        const bool failed = d.runUntil([&] { return d.graph.failed(); }, 400);
        LIFT_CHECK(failed);
        LIFT_CHECK(d.graph.failure() == lift::GraphFailure::ExpectChange);
        // ★ 动作真的被应用过（否则 ExpectChange 这条用例证明不了"动作没生效"这一类）
        LIFT_CHECK(d.graph.actuator().appliedCount() >= 1);
        const std::string report = d.graph.failureReport();
        LIFT_CHECK(contains(report, "expect-change"));
        LIFT_CHECK(contains(report, "screen changed"));
        LIFT_CHECK(contains(report, "act"));  // 节点 id
    }

    // =================================================================================
    // ⑥b changed-counts-window —— `screen changed` 的判据是**窗口里变化发生过**，
    //     ⛔ 不是"最后这一帧与上一帧不同"（两次判定之间可能已经 Present 过好几帧）
    // =================================================================================
    g_current_case = "changed-counts-window";
    {
        Driver d;
        TraceCapture cap;
        d.graph.setTrace(cap.file(), true);
        LIFT_CHECK(d.load(
            "entry a\n"
            "node a\n"
            "  when screen changed\n"
            "  action quit\n"
            "  next\n",
            "synthetic:changed"));
        // 第一个块边界：一个样本，图在这里进节点（= 窗口起点）
        d.step(true);
        // 同一个块边界内又来**两个**样本：A→B（变了）、B→B（没变）
        d.sampler.observeValues(d.frame, 0xBB, {});
        d.sampler.observeValues(d.frame, 0xBB, {});
        // ★ 判据的两半都在这里：最后这一帧与上一帧**相同**，但窗口里"变化发生过"。
        //   换成 `last().changed` 的实现在这里一次也不会命中 ⇒ 本用例当场红。
        LIFT_CHECK(!d.sampler.last().changed);
        LIFT_CHECK(d.sampler.changeCount() >= 1);
        d.graph.tick(d.wm, d.sampler, d.frame, d.guest_ms, d.insns, d.wall_ms, &d.identity);
        const std::string trace = cap.text();
        LIFT_CHECK(contains(trace, "when screen changed"));
        LIFT_CHECK(contains(trace, "hit"));
    }

    // =================================================================================
    // ⑥b2 script-level-when —— `script` 是**电平**：身份**已经**是 TITLE 时也立刻成立
    // ★ 红在这意味着：判定退回了边沿（"等进入才发事件"）—— 那正是"等一屏 → 点一下"写不出来的坑，
    //   也是为什么要有这条独立的电平条件。
    // =================================================================================
    g_current_case = "script-level-when";
    {
        Driver d;
        TraceCapture cap;
        d.graph.setTrace(cap.file(), true);
        LIFT_CHECK(d.load(
            "await script TITLE\n"
            "click 1174 377\n"
            "await script GAMESTART\n"
            "quit\n",
            "synthetic:script-level"));
        // ★ 一开始身份就**停在** TITLE（不是"刚进入"）：电平 ⇒ 必须立刻命中。
        d.identity.valid = true;
        d.identity.cur = 3;
        d.identity.id = 21092;
        d.identity.name = "TITLE.BIN";
        LIFT_CHECK(d.graph.actionsIssued() == 0);
        d.step();  // idle → step1 enter
        d.step();  // step1 的 when(script TITLE) 命中 ⇒ 追加 click
        LIFT_CHECK(d.graph.actionsIssued() >= 1);
        // 身份换成 GAMESTART ⇒ step2（电平）要跟着成立
        d.identity.cur = 4;
        d.identity.id = 21099;
        d.identity.name = "GAMESTART.BIN";
        LIFT_CHECK(d.runUntil([&] { return d.graph.done(); }, 20));
        const std::string trace = cap.text();
        LIFT_CHECK(contains(trace, "node step1 enter"));
        LIFT_CHECK(contains(trace, "when script TITLE"));
        LIFT_CHECK(contains(trace, "node step2 enter"));  // 链没断：step1 → step2
        LIFT_CHECK(contains(trace, "task done"));
    }

    // =================================================================================
    // ⑥b3 script-expect-level —— `expect script …` **不等新样本**（身份是电平，不是帧计数）
    // ★ 红在这意味着：`condNeedsNewSample(Script)` 被错写成 true ⇒ 房间空闲、帧停住时
    //   `expect script …` 永远等不到（`interaction-model.md` §5 那个坑）。
    // =================================================================================
    g_current_case = "script-expect-level";
    {
        Driver d;
        LIFT_CHECK(d.load(
            "await always\n"
            "click 10 10\n"
            "expect script TITLE\n",
            "synthetic:script-expect"));
        d.identity.valid = true;
        d.identity.cur = 3;
        d.identity.id = 21092;
        d.identity.name = "TITLE.BIN";
        // 喂样本让 click 被应用（自动抬起要等 guest 采样），直到进 `wait-expect`
        LIFT_CHECK(d.runUntil(
            [&] { return std::strcmp(d.graph.stateName(), "wait-expect") == 0; }, 40, /*feed=*/true));
        // ★ 这一 tick **不喂新样本**：如果 script 还要求"动作之后的新样本"，这里会停在 wait-expect。
        d.step(false);
        LIFT_CHECK(d.graph.done());
    }

    // =================================================================================
    // ⑥c stall-fallback-expires —— guest 时钟冻住（阻塞在 GetMessageA）时，
    //     "insns 与 frame 都不再推进超过 timeout 墙钟毫秒" ⇒ 也必须判到期（⛔ 不挂死）
    // =================================================================================
    g_current_case = "stall-fallback-expires";
    {
        Driver d;
        d.hash_step = 0;
        LIFT_CHECK(d.load(
            "entry a\n"
            "node a\n"
            "  when screen hash 0xDEAD\n"
            "  action quit\n"
            "  next\n"
            "  timeout 100\n",
            "synthetic:stall"));
        // ★ 只推进墙钟：`frame` / `insns` / `guest_ms` 全部冻住（模拟 guest 阻塞）。
        //   没有停滞兜底 ⇒ 这里的 `guest_ms` 永远停在 0，窗口永远不会到期（整轮挂死）。
        for (int i = 0; i < 200 && !d.graph.failed(); ++i) {
            d.sampler.observeValues(d.frame, /*hash=*/0x1, {});
            d.graph.tick(d.wm, d.sampler, d.frame, /*guest_ms=*/0, /*insns=*/0, d.wall_ms, &d.identity);
            d.wall_ms += 10;
        }
        LIFT_CHECK(d.graph.failed());
        // 采样在增长（只是 hash 不对）⇒ 这是"画面不对"，不是"识别源失明"
        LIFT_CHECK(d.graph.failure() == lift::GraphFailure::Timeout);
    }

    // =================================================================================
    // ⑦ quit-marks-task-done —— `quit` 动作 = 任务收口（⛔ 不能停在 `acting`）
    // ★ 为什么需要它：`quit` 会把运行停掉，状态机再也拿不到下一次推进 ⇒ 如果 `issueAction`
    //   之后又被覆盖回 `Acting`，`done()` 恒假、summary 报 `state=acting`，调用方会把
    //   **成功走完的任务**判成"没走完"（实测踩过：exit 0 → exit 5）。
    // =================================================================================
    g_current_case = "quit-marks-task-done";
    {
        Driver d;
        LIFT_CHECK(d.load(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action quit\n"
            "  next\n",
            "synthetic:quit"));
        TraceCapture cap;
        d.graph.setTrace(cap.file(), true);
        // 第一次 tick 只做"进 entry"，第二次才判 `when` ⇒ 收口（命中 ⇒ 追加 quit ⇒ 立刻 Done）。
        d.step();
        d.step();
        LIFT_CHECK(d.graph.done());
        LIFT_CHECK(!d.graph.failed());
        LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(d.graph.failure()), 0u);
        LIFT_CHECK(std::string(d.graph.stateName()) == "done");
        LIFT_CHECK(d.graph.current() == nullptr);  // Done 之后没有"当前节点"
        LIFT_CHECK(contains(cap.text(), "action quit applied"));
        LIFT_CHECK(contains(cap.text(), "task done"));
        // 再 tick 一次也不许把它变回 acting / failed（不重复报）
        d.step(false);
        LIFT_CHECK(std::string(d.graph.stateName()) == "done");
        LIFT_CHECK(d.graph.done());
    }

    // =================================================================================
    // ⑧ acting-expires —— 动作永远不完成时**必须失败**，不许永远停在 acting
    // ★ 为什么需要它（复审探针发现）：`Acting` 原先没有到期判定，而 `InputScript` 的帧停滞兜底
    //   是**一次性**的 ⇒ 帧真停住（房间空闲就是这样）时，图会永远停在 `acting`：不失败、不 done、
    //   不退出。这条用例把帧号冻住、只推 guest/墙钟，要求它到期并报 Timeout。
    // =================================================================================
    g_current_case = "acting-expires";
    {
        Driver d;
        LIFT_CHECK(d.load(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action click 10 20\n"
            "  next\n"
            "  timeout 100\n",
            "synthetic:acting"));
        d.step();  // Idle ⇒ 进 entry
        d.step();  // 命中 ⇒ 追加动作（`at frame <F+1>`）⇒ Acting
        bool failed = false;
        for (int i = 0; i < 400 && !failed; ++i) {
            // `frame` / `insns` 冻住（模拟 guest 阻塞 / 帧停住）⇒ 追加的 `at frame F+1` 永不 due。
            d.graph.tick(d.wm, d.sampler, /*frame=*/2, d.guest_ms, /*insns=*/0, d.wall_ms, &d.identity);
            d.guest_ms += 10;
            d.wall_ms += 10;
            failed = d.graph.failed();
        }
        LIFT_CHECK(failed);  // ⛔ 不许永远停在 acting
        LIFT_CHECK(d.graph.failure() == lift::GraphFailure::Timeout);
    }

    // =================================================================================
    // ⑨ expect-needs-new-sample —— `expect` 不许用**动作前**的旧样本命中
    // ★ 复审探针发现：state 型 expect（hash / rect）原来只读最近一次采样 ⇒ 动作前那一帧本来
    //   就等于目标值时，expect 当场成立（"动作没生效"被吃成成功）。
    // =================================================================================
    g_current_case = "expect-needs-new-sample";
    {
        Driver d;
        // 目标 hash = 动作**之前**那一帧的值（Driver 的起始 hash = 0x100）。
        LIFT_CHECK(d.load(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action click 10 20\n"
            "  next\n"
            "  timeout 100\n"
            "  expect screen hash 0x100\n",
            "synthetic:expect-new"));
        d.sampler.observeValues(1, 0x100, {});  // 动作前的一次采样（hash 已经等于目标）
        // ① Idle→enter ② 命中→追加动作 ③ 动作应用→WaitExpect（此后不再采样）
        for (int i = 0; i < 5 && !d.graph.failed() && !d.graph.done(); ++i) d.step(false);
        for (int i = 0; i < 200 && !d.graph.failed() && !d.graph.done(); ++i) d.step(false);
        LIFT_CHECK(!d.graph.done());   // ⛔ 旧样本不算"动作之后变了"
        LIFT_CHECK(d.graph.failed());
        // 窗口内没有**新**样本 ⇒ 这是"识别源失明"，不是"动作没生效"
        LIFT_CHECK(d.graph.failure() == lift::GraphFailure::NoSample);
    }

    // =================================================================================
    // ⑩ rect-blind-is-no-sample —— 矩形一直读不到像素 ⇒ no-sample（不是 timeout）
    // ★ 真源 §2："无效 ≠ 一次有效样本"；只看 Present 次数会把"源接上了但读不到"报成 timeout。
    // =================================================================================
    g_current_case = "rect-blind-is-no-sample";
    {
        Driver d;
        LIFT_CHECK(d.load(
            "entry a\n"
            "node a\n"
            "  when screen rect 0 0 10 10 avg 0x112233\n"
            "  action quit\n"
            "  next\n"
            "  timeout 100\n",
            "synthetic:rect-blind"));
        // 采样在推进（Present 数增长），但每次都不带矩形值 ⇒ 该矩形没有一次有效样本。
        for (int i = 0; i < 40 && !d.graph.failed(); ++i) d.step(true);
        LIFT_CHECK(d.graph.failed());
        LIFT_CHECK(d.graph.failure() == lift::GraphFailure::NoSample);
    }

    // =================================================================================
    // ⑩b wait-wall-ms-parse —— `wait <n> wall_ms` 的**解析期**契约（图 / 线性两种写法）
    // ★ 红在这意味着：图不收 `wall_ms`（用户要的"等真实 500 ms"没有写法），或非法单位的报错串
    //   没把新单位列进去（人看不出该写哪个词）。
    // ⛔ 不断言"墙钟读到多少"：这里只断言解析出的 kind / 值。
    // =================================================================================
    g_current_case = "wait-wall-ms-parse";
    {
        lift::InteractionGraph g;
        std::string err;
        const bool ok = g.loadText(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action wait 500 wall_ms\n"
            "  next\n",
            "synthetic:wall-parse", &err);
        LIFT_CHECK(ok);
        if (!ok) {
            std::fprintf(stderr, "  err = %s\n", err.c_str());
        } else {
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(g.nodes().size()), 1u);
            const lift::Node& n = g.nodes()[0];
            LIFT_CHECK(n.action.kind == lift::GraphActionKind::Wait);
            LIFT_CHECK(n.action.wait_unit == lift::TriggerKind::WallMs);
            LIFT_CHECK_EQ_U32(static_cast<std::uint32_t>(n.action.wait_value), 500u);
            LIFT_CHECK(g.usesWallClock());
            // `clear()` / 下一次解析必须复位（否则空图也会被当成"含墙钟" ⇒ trace 无故降级）
            g.clear();
            LIFT_CHECK(!g.usesWallClock());
        }

        // 线性写法：`await always` + `wait 500 wall_ms` 也能过（同一个动作，隐式 step）
        lift::InteractionGraph lin;
        std::string lin_err;
        const bool lin_ok = lin.loadText("await always\nwait 500 wall_ms\nquit\n", "synthetic:wall-linear", &lin_err);
        LIFT_CHECK(lin_ok);
        if (lin_ok) {
            LIFT_CHECK(lin.usesWallClock());
            LIFT_CHECK(lin.nodes().size() >= 2);
            LIFT_CHECK(lin.nodes()[0].action.kind == lift::GraphActionKind::Wait);
            LIFT_CHECK(lin.nodes()[0].action.wait_unit == lift::TriggerKind::WallMs);
            LIFT_CHECK(lin.nodes()[0].action.wait_value == 500);
        }

        // 非法单位仍报错，且错误串**列出新单位**（`hours` 原样回显保持既有口径）
        lift::InteractionGraph bad;
        std::string bad_err;
        const bool bad_ok = bad.loadText(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action wait 3 hours\n"
            "  next\n",
            "t.graph", &bad_err);
        LIFT_CHECK(!bad_ok);
        LIFT_CHECK(!bad.active());
        LIFT_CHECK(contains(bad_err, "hours"));
        LIFT_CHECK(contains(bad_err, "wall_ms"));

        // ★ 闭集合只收 `wall_ms` 一个拼写：`wall` / `host_ms` 是**时间线**的别名，图不跟着放宽。
        expectLoadError(
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action wait 3 wall\n"
            "  next\n",
            4, "收到 wall）");
    }

    // =================================================================================
    // ⑩c wait-wall-ms-behavior —— 到期判据**只看宿主墙钟**：wall 0..499 不完成、500 完成并进下一 step
    // ★ 红在这意味着：`wall_ms` 被当成 guest 毫秒 / 帧数（等 500 guest_ms 会提前 / 推迟完成），
    //   或到期后没推进（图挂在 acting）。
    // ★ 用**测试自己给的** wall 值，⛔ 一次也不靠真实墙钟推进。
    // =================================================================================
    g_current_case = "wait-wall-ms-behavior";
    {
        Driver d;
        LIFT_CHECK(d.load(
            "await always\n"
            "wait 500 wall_ms\n"
            "quit\n",
            "synthetic:wall-behavior"));
        // 第 1 次 tick：Idle ⇒ 进 step1；第 2 次：`when always` 命中 ⇒ 追加 wait（due = wall + 500）
        d.graph.tick(d.wm, d.sampler, d.frame, d.guest_ms, d.insns, 0, &d.identity);
        d.graph.tick(d.wm, d.sampler, d.frame, d.guest_ms, d.insns, 0, &d.identity);
        LIFT_CHECK(std::string(d.graph.stateName()) == "acting");
        // ★ 这段也顺带把 guest_ms 推过 500（每步 +10）⇒ 若 WallMs 分支误读 guest_ms 就会在这里红
        for (std::uint64_t wall = 0; wall < 500; ++wall) {
            d.graph.tick(d.wm, d.sampler, d.frame, d.guest_ms, d.insns, wall, &d.identity);
            d.guest_ms += 10;
            LIFT_CHECK(!d.graph.done());
            LIFT_CHECK(!d.graph.failed());
        }
        // wall = 500 ⇒ 到期：应用到下一 step（step2 = quit），但还没执行它
        d.graph.tick(d.wm, d.sampler, d.frame, d.guest_ms, d.insns, 500, &d.identity);
        LIFT_CHECK(!d.graph.failed());
        const lift::Node* after_wait = d.graph.current();
        LIFT_CHECK(after_wait != nullptr);
        if (after_wait != nullptr) LIFT_CHECK(after_wait->id == "step2");
        LIFT_CHECK(!d.graph.done());
        // 下一个块边界：step2 的 `when always` 命中 ⇒ quit ⇒ 任务完成
        d.graph.tick(d.wm, d.sampler, d.frame, d.guest_ms, d.insns, 500, &d.identity);
        LIFT_CHECK(d.graph.done());
        LIFT_CHECK(!d.graph.failed());

        // ★ 裁决 3：`timeout` **仍然管** wall wait（⛔ 不是无限等待）。等不到就按现有规则响亮失败。
        Driver t;
        LIFT_CHECK(t.load(
            "await always\n"
            "timeout 100\n"
            "wait 1000 wall_ms\n"
            "quit\n",
            "synthetic:wall-timeout"));
        t.step(true);  // Idle ⇒ 进 step1（窗口起点 = 这次 tick 的 wall）
        t.step(true);  // `when always` 命中 ⇒ 追加 wait（due = wall + 1000）⇒ Acting
        LIFT_CHECK(std::string(t.graph.stateName()) == "acting");
        for (std::uint64_t wall = 0; wall <= 300 && !t.graph.failed(); ++wall) {
            t.graph.tick(t.wm, t.sampler, t.frame, t.guest_ms, t.insns, wall, &t.identity);
        }
        LIFT_CHECK(t.graph.failed());  // ⛔ 不许无限等
        LIFT_CHECK(t.graph.failure() == lift::GraphFailure::Timeout);
    }

    // =================================================================================
    // ⑩d wait-wall-ms-trace —— 含 `wall_ms` 的图 ⇒ `usesWallClock()` + trace 头 `# NON-DIFFABLE`
    // ★ 红在这意味着：墙钟等待的 trace 被当成可 diff（同一输入两次跑不再逐行相同，diff 会骗人），
    //   或反向：不含 `wall_ms` 的图被无故降级（对照组）。
    // =================================================================================
    g_current_case = "wait-wall-ms-trace";
    {
        Driver d;
        LIFT_CHECK(d.load(
            "await always\n"
            "wait 500 wall_ms\n"
            "quit\n",
            "synthetic:wall-trace"));
        LIFT_CHECK(d.graph.usesWallClock());
        TraceCapture cap;
        // = main.cpp 的表达式在 headless 档的取值（`!present_window_active` = true）
        d.graph.setTrace(cap.file(), /*diffable=*/!d.graph.usesWallClock());
        d.step();  // 第一次 tick 写 trace 头
        const std::string trace = cap.text();
        LIFT_CHECK(startsWith(trace, "# interaction trace v1\n"));
        const std::size_t first_nl = trace.find('\n');
        LIFT_CHECK(first_nl != std::string::npos);
        if (first_nl != std::string::npos) {
            const std::size_t second_end = trace.find('\n', first_nl + 1);
            const std::string second = second_end == std::string::npos
                                           ? trace.substr(first_nl + 1)
                                           : trace.substr(first_nl + 1, second_end - first_nl - 1);
            LIFT_CHECK(second == "# NON-DIFFABLE");
        }

        // 对照组：不含 `wall_ms` 的图 ⇒ 不触发降级（`frames` / `ms` 语义不变）
        Driver plain;
        LIFT_CHECK(plain.load("await always\nwait 1 frames\nquit\n", "synthetic:frames-trace"));
        LIFT_CHECK(!plain.graph.usesWallClock());
    }

    // =================================================================================
    // ⑪ trace-diff-stable —— 同一合成序列跑两遍 ⇒ trace 文本**逐字节**相同
    // =================================================================================
    g_current_case = "trace-diff-stable";
    {
        // 覆盖 try 四个事件：when hit / action applied / expect hit / node done / task done。
        const char* graph =
            "entry a\n"
            "node a\n"
            "  when always\n"
            "  action click 10 20\n"
            "  next b\n"
            "  expect screen changed\n"
            "node b\n"
            "  when screen hash 0xAA\n"
            "  action wait 1 frames\n"
            "  next\n";
        auto runOnce = [&](std::string* out) {
            Driver d;
            TraceCapture cap;
            d.graph.setTrace(cap.file(), true);
            const bool ok = d.load(graph, "synthetic:diff");
            for (int i = 0; i < 40; ++i) {
                if (i == 20) {
                    d.hash = 0xAA;  // 到第 20 步才让 b 的条件成立（固定脚本，两遍一样）
                    d.hash_step = 1;
                }
                d.step(true);
                if (d.graph.done() || d.graph.failed()) break;
            }
            *out = cap.text();
            return ok;
        };
        std::string first;
        std::string second;
        LIFT_CHECK(runOnce(&first));
        LIFT_CHECK(runOnce(&second));
        LIFT_CHECK(startsWith(first, "# interaction trace v1\n"));
        LIFT_CHECK(contains(first, "# diffable"));
        LIFT_CHECK(contains(first, "node b enter"));
        LIFT_CHECK(contains(first, "task done"));
        LIFT_CHECK(first == second);  // ★ 判据：同输入 ⇒ 逐字节同输出
    }

    // =================================================================================
    // ⑫ graph-scripts-parse —— 源码树里的 `scripts/interaction/*.graph` 必须都能解析
    // =================================================================================
    g_current_case = "graph-scripts-parse";
    {
        namespace fs = std::filesystem;
        std::vector<std::string> roots{std::string(LIFT_SOURCE_DIR) + "/scripts/interaction",
                                       "scripts/interaction"};
        std::vector<fs::path> found;
        std::string used;
        for (const std::string& root : roots) {
            std::error_code ec;
            if (!fs::is_directory(root, ec)) continue;
            std::vector<fs::path> files;
            for (const fs::directory_entry& entry : fs::directory_iterator(root, ec)) {
                if (!entry.is_regular_file(ec)) continue;
                if (entry.path().extension() != ".graph") continue;
                files.push_back(entry.path());
            }
            if (ec) continue;
            if (files.empty()) continue;
            std::sort(files.begin(), files.end());  // 固定顺序（两次运行同一串输出）
            found = files;
            used = root;
            break;
        }
        if (found.empty()) {
            // ★ 路径脚本已入库（见 `scripts/interaction/`）⇒ **找不到就是红**：
            //   静默 SKIP 会让"有人删了脚本"没有任何信号，而"脚本还能解析"正是这条用例的判据。
            //   ⛔ 不用 `LIFT_SKIP`（77）：那会把整条 `interaction-graph` 标成 Skipped，把别的判据一起藏掉。
            std::fprintf(stderr, "找不到 scripts/interaction/*.graph（试过 %s 与 cwd）\n", roots[0].c_str());
            LIFT_CHECK(false);
        } else {
            std::printf("graph-scripts: %zu 份（来自 %s）\n", found.size(), used.c_str());
            for (const fs::path& path : found) {
                lift::InteractionGraph g;
                std::string err;
                const bool ok = g.loadFile(path.string(), &err);
                if (!ok) std::fprintf(stderr, "  %s: %s\n", path.string().c_str(), err.c_str());
                LIFT_CHECK(ok);
                if (ok) {
                    LIFT_CHECK(g.active());
                    lift::FrameSampler sampler;
                    g.bindSampler(sampler);  // 接线也得能在真脚本上走通
                }
            }
        }
    }

    return finish("interaction-graph");
}
