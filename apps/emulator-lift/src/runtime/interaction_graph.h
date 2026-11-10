// emulator-lift · 识别驱动的交互状态机（任务 = 节点图：when → action → next）
//
// ★ **真源是 `docs/03-emulator/interaction-model.md`**：节点 schema / 五种 `when` 源 /
//   三种失败 / 确定性口径全在那边。本文件只是它的**接口**，⛔ 不在注释里复述 schema 语义
//   （两份 schema 必漂，见 `AGENTS.md` §10）。
// ★ 与 `runtime/input_script.h` 的关系：时间线是**过渡形态**，两者并存（真源 §7 第 3 期）。
//   动作**不是**在这里直接改窗口状态 —— 本类把动作**追加进** `InputScript`（`actuator()`），
//   由它去发 WM_* / 处理"一次按下一定被 guest 观察到"（自动 hold / 延迟抬起）。
//   ⇒ ⛔ 不要在这里另写一套按下 / 抬起：那会绕过已守卫的采样语义。
// ★ 判定是**观测值的纯函数**：采样在 `runtime/screen_sample.h`（Present 时刻），
//   这里只在块边界读 `FrameSampler` 的单调计数（`sampleCount` / `changeCount`）。
//
// ---- 行写法（人写；`#` 起注释、空行忽略）----
//
//     entry title                       ← 必填一次：从哪个节点开始
//     node title                        ← 一个节点一块，`node` 行开新块
//       when screen hash 0xD6ED710894A39FA5
//       action click 944 562
//       next room
//       timeout 3000
//       expect screen changed
//     node room
//       when screen rect 0 560 120 80 avg 0x1B2B3C
//       action click center
//       next
//
//   条件的写法（闭集合，与真源 §1 的表一一对应）：
//     always | screen hash <hex64> | screen rect <x> <y> <w> <h> avg <0xRRGGBB> |
//     screen changed | script <名字>（今天**解析期拒绝**，见真源 §6）
//   动作的写法：click <x> <y> [left|right] | click center [left|right] | key <VK> |
//     wait <n> frames|ms | profile_begin <label> | profile_end <label> | quit
//   按钮词只有 `left` / `right`（省略 = left；⛔ 没有 `middle` / `hold`）——
//   词表与 JSON 写法的真源见 `docs/03-emulator/interaction-model.md` §1（这里只列有哪些词）。
//   `next` 不带 id = 任务结束；`timeout` 省略 = 缺省 5000 ms（trace 里标 `(default)`）。
//   `click center` 只在 `when screen rect …` 下合法（框中心是它唯一的坐标来源）。
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "runtime/host_window.h"
#include "runtime/input_script.h"
#include "runtime/screen_sample.h"

namespace lift {

// ---------------------------------------------------------------------------------------
// 条件项（`when` 与 `expect` 共用同一个闭集合）
// ---------------------------------------------------------------------------------------
enum class CondKind : std::uint8_t {
    Always = 0,       // 恒成立（纯推进）
    ScreenHash = 1,   // 整帧 hash 相等
    ScreenRect = 2,   // 矩形均值命中（产出框 ⇒ `click center` 的坐标来源）
    ScreenChanged = 3,// 自进入等待以来"与上一次采样不同"发生过（读 `changeCount()`）
    Script = 4,       // 引擎脚本身份（marker 源；**今天解析期拒绝**）
};
const char* condKindName(CondKind k);

struct GraphCond {
    CondKind kind = CondKind::Always;
    std::uint64_t hash = 0;                 // ScreenHash
    PixelRect rect;                         // ScreenRect
    std::uint32_t avg_rgb = 0;              // ScreenRect 的目标均值（0xRRGGBB）
    std::string script;                     // Script（未实现）
    std::string text;                       // 规范化后的可读写法（trace / 报错用，⛔ 不参与判定）
    int rect_index = -1;                    // bindSampler 填（= 该矩形在 sampler 里的索引）
};
// 只读 `FrameSampler` 的单调计数：不采样、不读像素。
// ⚠ 这条声明写在 `struct GraphCond` **之后**：`GraphCond` 按引用/值做参数时编译器要知道它的完整
//   定义才能实例化签名（前向声明只够指针 / 引用**声明**，不够这里）。⟹ 顺序是编译前提，不是排版偏好。
bool condHolds(const GraphCond& c, const FrameSampler& sampler, std::uint64_t samples_at_start,
               std::uint64_t changes_at_start);

// ---------------------------------------------------------------------------------------
// 动作（闭集合）
// ---------------------------------------------------------------------------------------
enum class GraphActionKind : std::uint8_t {
    Click = 0,       // click x y [left|right]
    ClickCenter = 1, // click center [left|right]（= 上一个命中框的中心；真源 §1）
    Key = 2,         // key <VK>
    Wait = 3,        // wait <n> frames|ms（纯延迟，不碰输入）
    ProfileBegin = 4,
    ProfileEnd = 5,
    Quit = 6,
};
const char* graphActionKindName(GraphActionKind k);

struct GraphAction {
    GraphActionKind kind = GraphActionKind::Quit;
    int x = 0;                       // Click
    int y = 0;                       // Click
    // Click / ClickCenter 的输入目标：词表只有 `left` / `right`，缺省 `Left`；
    // `Left` 发到时间线**不带词**（`click <x> <y>`），只有 `Right` 才写成 `click <x> <y> right`。
    MouseButton button = MouseButton::Left;
    std::uint32_t vk = 0;            // Key
    std::uint64_t wait_value = 0;    // Wait
    TriggerKind wait_unit = TriggerKind::Frame;  // Wait（只认 Frame / GuestMs）
    std::string label;               // ProfileBegin / ProfileEnd
    std::string text;                // 原始写法（日志 / 报错定位）
};

struct Node {
    std::string id;
    GraphCond when;
    GraphAction action;
    std::string next;                // 空 = 任务结束
    std::uint64_t timeout_ms = 0;
    bool timeout_explicit = false;   // false ⇒ 缺省值（trace 标 `(default)`）
    bool has_expect = false;
    GraphCond expect;
    int line = 0;                    // `node` 那一行（报错定位）
};

// ---------------------------------------------------------------------------------------
// 三种失败（真源 §2）。`None` = 还没失败。
// ---------------------------------------------------------------------------------------
enum class GraphFailure : std::uint8_t { None = 0, Timeout = 1, NoSample = 2, ExpectChange = 3 };
const char* graphFailureName(GraphFailure f);

// 缺省 `timeout`（真源 §1：必填，但解析器给一个固定缺省值）。
constexpr std::uint64_t kDefaultNodeTimeoutMs = 5000;

class InteractionGraph {
public:
    // ---- 解析（失败 ⇒ false + err 带 `origin:行: 原因`；成功 ⇒ 旧图被替换）----
    bool loadText(const std::string& text, const std::string& origin, std::string* err);
    bool loadFile(const std::string& path, std::string* err);
    void clear();

    bool active() const { return !nodes_.empty(); }
    const std::string& origin() const { return origin_; }
    const std::vector<Node>& nodes() const { return nodes_; }
    const Node* findNode(const std::string& id) const;
    const std::string& entry() const { return entry_; }

    // ---- 结果 ----
    bool done() const { return state_ == State::Done; }
    bool failed() const { return state_ == State::Failed; }
    GraphFailure failure() const { return failure_; }
    const std::string& failureDetail() const { return failure_detail_; }
    // 失败时的完整报告（多行；`Machine`/`main` 直接写 stderr）：
    //   失败类别 · 节点 · 等的是什么 · 等了多久（帧 / 指令 / guest 毫秒）· 采样数 ·
    //   **每个受监视矩形/整帧的最后一次采样值**（"全部候选"）。
    std::string failureReport() const;
    // 失败时"最后画面"该写到哪（调用方据此调 `--interaction-fail-dump` 的缺省名）。
    static std::string defaultFailDumpPath(const std::string& graph_path);

    // ---- trace（一行一事件；确定性字段只有 节点 id / frame / insn / guest_ms / 采样值 / 动作/结果）----
    void setTrace(std::FILE* out, bool diffable);
    bool traceEnabled() const { return trace_ != nullptr; }

    // ---- 采样接线（解析之后调一次）----
    void bindSampler(FrameSampler& sampler);

    // ---- 动作出口：追加进这个 `InputScript`（`Machine` 负责 tick 它）----
    InputScript& actuator() { return actuator_; }
    const InputScript& actuator() const { return actuator_; }

    // 块边界推进。返回 true = 本 tick 追加了动作（或发生了状态迁移）。
    //   `wm` 只用于 `click center` 的坐标（框中心）与 Wait 的判定基数，实际注入走 actuator。
    bool tick(WindowManager& wm, const FrameSampler& sampler, std::uint64_t frame, std::uint64_t guest_ms,
              std::uint64_t insns, std::uint64_t wall_ms);

    // ---- 可观测账（守卫 / run summary 用）----
    // 当前节点（Idle/Done/Failed 时为 nullptr；summary / 失败报告用）。
    const Node* current() const;
    std::uint64_t stateChanges() const { return transitions_; }
    std::uint64_t actionsIssued() const { return actions_issued_; }
    const char* stateName() const;

private:
    enum class State : std::uint8_t { Idle = 0, WaitWhen = 1, Acting = 2, WaitExpect = 3, Done = 4, Failed = 5 };

    struct RectBinding {
        PixelRect rect;
        std::size_t index = 0;
    };

    // `waited` = 失败时正在等的那个条件（`when` 或 `expect`）—— 报告里的"等的是什么"只打它的 `text`
    void fail(GraphFailure kind, const std::string& why, const GraphCond& waited, std::uint64_t frame,
              std::uint64_t guest_ms, std::uint64_t insns);
    bool issueAction(WindowManager& wm, std::uint64_t frame, std::uint64_t guest_ms);
    void enterNode(std::size_t index, std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns,
                   std::uint64_t wall_ms);
    // 两把尺都到期才叫到期：`guest_ms` 超过窗口 + `insns`/`frame` 都不推进的墙钟超过窗口
    bool windowExpired(std::uint64_t timeout_ms, std::uint64_t guest_ms, std::uint64_t wall_ms) const;
    // 停滞兜底的记账：`insns` 或 `frame` 变了 ⇒ "最近一次有进展"就是现在
    void noteProgress(std::uint64_t frame, std::uint64_t insns, std::uint64_t wall_ms);
    // 动作已应用（+ `expect` 命中 / 没有 expect）⇒ 推进到 `next`（空 ⇒ Done）
    void advance(std::uint64_t frame, std::uint64_t guest_ms, std::uint64_t insns, std::uint64_t wall_ms);
    void writeTraceHeader();
    void traceLine(const std::string& line);
    std::string describeCond(const GraphCond& c) const;
    std::string samplesSummary(const FrameSampler& sampler) const;

    std::vector<Node> nodes_;
    std::string entry_;
    std::string origin_;
    State state_ = State::Idle;
    std::size_t current_ = 0;
    std::size_t next_index_ = 0;
    GraphFailure failure_ = GraphFailure::None;
    std::string failure_detail_;
    // 等待窗口的起点（判定只读单调计数 + 这两个时刻）
    std::uint64_t wait_start_guest_ms_ = 0;
    std::uint64_t wait_start_frame_ = 0;
    std::uint64_t wait_start_insns_ = 0;
    std::uint64_t wait_start_wall_ms_ = 0;
    std::uint64_t samples_at_start_ = 0;
    std::uint64_t changes_at_start_ = 0;
    // 窗口起点的"盲采样"计数（矩形全部读不到像素的次数）。`no-sample` 要认它：
    // 真源 §2 的"无效 ≠ 一次有效样本"—— 只看 Present 次数会把"源接上了但读不到"报成 timeout。
    std::uint64_t blind_at_start_ = 0;
    std::uint64_t last_progress_wall_ms_ = 0;
    std::uint64_t last_progress_frame_ = 0;
    std::uint64_t last_progress_insns_ = 0;
    // 动作已应用（= actuator 的 appliedCount 增长）之后才开始 expect_change 的窗口
    std::uint64_t applied_before_ = 0;
    // Wait 动作的到期点
    bool wait_pending_ = false;
    std::uint64_t wait_due_frame_ = 0;
    std::uint64_t wait_due_guest_ms_ = 0;
    // 这次 `screen rect` 命中产出的框（`click center` 用）
    bool have_box_ = false;
    PixelRect box_;
    // 条件已经命中，但 `actuator_` 还没准备好接动作（还有未应用的动作 / 未完成的抬起）：
    // 保留命中事实、下个块边界重试追加 —— ⛔ 不丢命中、⛔ 不因此重判条件。
    bool hit_pending_ = false;
    // 追加被挡下的诊断只打一次（每次重试都打会把 stderr 冲掉）；成功追加后复位。
    bool issue_blocked_logged_ = false;
    // 采样接线（`bindSampler` 记住来源：窗口起点要记计数，失败快照要读采样值）
    const FrameSampler* sampler_ = nullptr;
    // 失败瞬间的快照。为什么不留着"到时候再读"：失败之后调用方还会继续 Present / 继续 tick actuator，
    // 那时 `sampler` 与 `actuator_` 的值已经不是"失败时的候选"了 —— 报告必须打**那一刻**的。
    std::uint64_t fail_samples_total_ = 0;
    std::uint64_t fail_changes_total_ = 0;
    std::uint64_t fail_blind_total_ = 0;
    std::vector<RectSample> fail_rects_;
    FrameSample fail_frame_;
    InputScript::ReleaseStats fail_release_stats_;
    InputScript::ReleaseInfo fail_last_release_;
    std::size_t fail_applied_ = 0;
    std::size_t fail_actions_ = 0;
    bool fail_script_finished_ = false;  // false ⇒ 还有未应用的动作 / 未完成的抬起（"抬起账要点"）
    bool fail_quit_requested_ = false;
    std::string failure_report_;  // `fail()` 一次算好（多行；调用方直接写 stderr）
    // 动作 → 目标节点（`next` 在 block 结束时填好；`next_index_` 就是它）
    std::string next_id_;
    // 采样接线
    std::vector<RectBinding> rect_bindings_;
    bool bound_ = false;
    bool trace_watch_frame_ = false;
    // trace
    std::FILE* trace_ = nullptr;
    bool trace_owned_ = false;
    bool trace_diffable_ = true;
    bool trace_header_done_ = false;
    // 账
    std::uint64_t transitions_ = 0;
    std::uint64_t actions_issued_ = 0;
    InputScript actuator_;
};

}  // namespace lift
