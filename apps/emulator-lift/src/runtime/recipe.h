// emulator-lift · 输入**配方**（顺序模型）：一套语法、**一份**能力面
//
// ★ 为什么只有一份：`--input-plan`（AOT 臂，在 guest **读点**推进）与机器内的 `InteractionGraph`
//   驱动器（块边界推进；**2026-11-16 起没有 CLI 入口**，历史入口 `--interaction-graph` 已退役）
//   跑的是**同一种东西** —— 一串按文件顺序推进的指令。
//   ⇒ 语法 / 顺序 / 门的语义 / 每步守卫的宽度 / 完成度的结论**只有这一份**（本文件 + `recipe.cpp`）；
//   差别只有**推进点**与**动作怎么落地**（读点直接写 `WindowManager` vs 追加进 `InputScript`），
//   见 `runtime/input_plan.h` 与 `runtime/interaction_graph.h`。
//   ⛔ 不要在任一驱动器里再写一份解析或再解释一次"门成立是什么意思"。
//
// ---- 行写法（人写；`#` 起注释、空行忽略、行号照数）----
//
//     await script TITLE timeout 30000   ← 电平门：当前脚本身份 == TITLE（ASCII 大小写不敏感，
//                                          两侧都忽略 `.bin`；`id:0x%08X` 退化名始终接受）
//                                          ★ 行尾 `timeout <ms>` = **这个门**的守卫（等它成立的上限）
//     await position TITLE 0x0460 0x0520 timeout 5000
//                                        ← 同一层再加"脚本内位置"：`FROM <= pos < TO`（半开区间）
//     wait 5000                          ← 宿主单调毫秒（固定时长；⛔ 不带 timeout）
//     click 1070 480 left                ← x/y = 1280×720 参考系；按钮词裸写 left|right（省略 = left）
//                                          （⛔ 不带 timeout：点击没有可等待的东西）
//     mouse_move 1070 480                ← 只写光标（同 click 的第一段）；同一参考系 / 同一缩放
//     mouse_down left                    ← 用**当前设备光标位置**按下（⛔ 不带 x/y；必须有一条更早的
//                                          `mouse_move`）；`mouse_up` 同理（拖拽 up 不回跳）
//     profile_begin fellow-enter         ← 区间 profiler 的边界（label 只用于对齐 / 报错）
//     profile_end fellow-enter
//     quit                               ← 立刻结束这次运行
//
//   · **按文件顺序**推进：从第一条未消费的指令开始，遇到不成立的指令就停（⛔ 不越过它）。
//     ⇒ 没有 `next` / `when` / 条件分支 / 候选轮询：这就是"顺序执行"的模型本身。
//   · `await position` 的 FROM / TO 必须是**带 `0x` 前缀的 32 位 hex**，解析期校验 `FROM < TO`；
//     脚本名**不在解析期校验**（名字匹配是运行期的事）。位置读数只有一份
//     （`runtime/engine_marker.h` 的 `readScriptPositionSample`）。
//   · ★ `timeout <ms>` 是**行尾可选项**，只允许出现在 `await script` / `await position` 上：它守的是
//     **同一行**那个门。⛔ 单独一行写 `timeout` = 解析期硬错（旧的"守上一条指令"写法已删除，
//     ⛔ 不留两种写法）；写在 `wait` / `click` / `profile_*` / `quit` 后面同样是硬错 —— 那些指令
//     **没有可等待的东西**（`wait` 是固定时长、到点就走）。缺省由驱动器给（见 `stepTimeoutMs`）。
//   · `wait` 是**宿主墙钟毫秒**（与 `--input-plan` 原来的 `wait <ms>` 同一口径）⇒ 同一份配方两次跑的
//     完成时刻不可复现，因此带 `wait` 的 trace 一律 `NON-DIFFABLE`。★ 它**没有守卫**（固定时长
//     不需要"等它成立"）：卡住由 `--time-limit` / `--max-frames` 这层看门狗报。
//
// ---- 能力面：**只有一份** ----
// ★ 九条指令 + 行尾 `timeout` **每个驱动器都认**；⛔ 不存在"这个入口不认某条指令"的解析期硬错，也⛔ 不再有
//   `kPlanFeatures` / `kGraphFeatures` 这类**按入口分叉的能力面表**（2026-11-16 统一；用户口径：
//   "不要没有新生产者的兼容层"）。`timeout` / `profile_begin` / `profile_end` / `quit` 的**语义**
//   在各驱动器是同一份（本文件 + `recipe.cpp`）。
// ★ **入口只管"推进点"与"动作怎么落地"**：
//   · `--input-plan`（AOT 臂，**唯一**的用户入口）：guest **读点**推进（`InputPlan::atPoint`），
//     动作直接写 `WindowManager`；tick 级读数、位置、身份都只读 guest 内存。
//   · `InteractionGraph`（机器内驱动器，**没有 CLI 入口**）：**块边界**推进（`InteractionGraph::tick`），
//     动作经 `InputScript::appendText` 追加成绝对触发。
//   ⇒ 某条指令"落在哪个宿主对象"是**驱动器的实现细节**，⛔ 不是能力面的一部分。
//     把"我这臂没有落点"写成解析期硬错 = 用入口去分叉能力面（本文件 2026-11-16 之前的状态）。
// ★ 缺省守卫的**宽度**是驱动器给的策略（不是能力面）：`InteractionGraph` 给 5000 ms（它的"到期"有两把尺）；
//   AOT 臂**不给缺省**（它的看门狗是 `--time-limit` / `--max-frames`，而且它只有一把尺 = 宿主墙钟）。
//   显式 `timeout <ms>` 在两边语义**相同**。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "runtime/engine_marker.h"  // MarkerSample / ScriptPositionSample
#include "runtime/host_window.h"    // MouseButton

namespace lift {

// ---------------------------------------------------------------------------------------
// 指令（闭集合；一个 step = 一条指令）
// ---------------------------------------------------------------------------------------
enum class StepKind : std::uint8_t {
    AwaitScript = 0,    // `await script <名字|id>`（电平门）
    AwaitPosition = 1,  // `await position <名字|id> <FROM> <TO>`（电平门：身份 + 位置）
    Wait = 2,           // `wait <ms>`（宿主墙钟，非阻塞）
    Click = 3,          // `click <x> <y> [left|right]`
    ProfileBegin = 4,   // `profile_begin <label>`（区间 profiler 的起点）
    ProfileEnd = 5,     // `profile_end <label>`（区间 profiler 的终点）
    Quit = 6,           // `quit`（立刻结束这次运行）
    MouseMove = 7,      // `mouse_move <x> <y>`（只写光标；1280×720 参考系，缩放同 click）
    MouseDown = 8,      // `mouse_down [left|right]`（用**当前设备光标位置**；必须有一条更早的 mouse_move）
    MouseUp = 9,        // `mouse_up [left|right]`（用当前设备位置 ⇒ 拖拽 up 不回跳）
};
const char* stepKindName(StepKind k);

struct Step {
    std::string id;      // step1 / step2 …（按文件顺序；trace / 失败报告 / 报错定位用）
    StepKind kind = StepKind::Wait;
    // AwaitScript / AwaitPosition（原文；⛔ 判定只走 `scriptNameMatches`）
    std::string script;
    // AwaitPosition：半开区间 `[pos_from, pos_to)`（label 坐标 = 脚本体内绝对字节偏移）
    std::uint32_t pos_from = 0;
    std::uint32_t pos_to = 0;
    // Wait（宿主毫秒）
    std::uint64_t wait_ms = 0;
    // Click（1280×720 参考系；缺省左键）
    //   ★ MouseMove 复用 x/y（参考坐标）；MouseDown / MouseUp 复用 button（x/y **不用**：
    //     它们的坐标 = 设备当前位置，⛔ 不在解析期烤进 step）
    int x = 0;
    int y = 0;
    MouseButton button = MouseButton::Left;
    // ProfileBegin / ProfileEnd
    std::string label;
    // 行尾 `timeout <ms>`（只有 `await script` / `await position` 允许）：**同一行那个门**的守卫
    // （0 = 没写；缺省由驱动器给，见 `stepTimeoutMs`）
    std::uint64_t timeout_ms = 0;
    int line = 0;  // 指令那一行（报错定位）

    // 门 = "要等它成立"的指令（其余指令一到就执行）。
    bool isGate() const { return kind == StepKind::AwaitScript || kind == StepKind::AwaitPosition; }

    // **写动作** = 会改 guest 可见输入态 / 投影一条输入消息的指令（`click` / `mouse_move` /
    //   `mouse_down` / `mouse_up`）。★ S7 的"一个读点最多一条写动作"按**这一个**判据，
    //   ⛔ 不按"是不是门"（门 / `wait` / `profile_*` / `quit` 都不是写）。
    bool isInputWrite() const {
        return kind == StepKind::Click || kind == StepKind::MouseMove || kind == StepKind::MouseDown ||
               kind == StepKind::MouseUp;
    }
};

// ---------------------------------------------------------------------------------------
// 配方（纯数据；解析成功后只读）
// ---------------------------------------------------------------------------------------
class Recipe {
public:
    // 失败 ⇒ false + err 带 `origin:行: 原因`（成功才替换旧内容）。
    bool loadText(const std::string& text, const std::string& origin, std::string* err);
    bool loadFile(const std::string& path, std::string* err);
    void clear();

    bool active() const { return !steps_.empty(); }
    const std::vector<Step>& steps() const { return steps_; }
    std::size_t size() const { return steps_.size(); }
    const std::string& origin() const { return origin_; }

private:
    std::vector<Step> steps_;
    std::string origin_;
};

// 规范化写法（trace / 报错 / 失败报告用；⛔ 只在这里拼一次）。
std::string stepText(const Step& s);

// 门是否成立（**唯一一份**判定；`--input-plan` 与机器内驱动器共用）。
//   · 非门指令 ⇒ true（它们没有条件）。
//   · 身份来源 / 位置读数**读不到** ⇒ false（继续等；⛔ 不报错、⛔ 不假装通过）。
bool stepGateHolds(const Step& s, const MarkerSample* identity, const ScriptPositionSample& position);

// `script` 名字口径（**唯一一份**，两个驱动器共用）：ASCII 大小写不敏感 · **两侧**都忽略 `.BIN` 后缀 ·
// `id:0x%08X` 退化名**始终**接受。⛔ 不要在别的 TU 里再写一份（两份一定静默漂）。
bool scriptNameMatches(const std::string& want, const MarkerSample& sample);

// ---------------------------------------------------------------------------------------
// 每步守卫的**宽度**（唯一一份）
// ---------------------------------------------------------------------------------------
// 返回这条指令的守卫窗口（ms）；**0 = 不设窗口**（这个驱动器没有缺省，且作者也没写 `timeout`）。
//   · 显式 `timeout <ms>` **恒覆盖**（两个驱动器同一条；解析期保证只有门能带它）；
//   · 没写 ⇒ 用驱动器给的缺省 `default_ms`（0 = 这个驱动器不给缺省守卫）；
//   · ★ `wait <ms>` **没有守卫**（恒返回 0）：它是**固定时长**、到点就走，不需要"等它成立"。
//     ⛔ 不再有 `ms + 缺省` 那种补丁（那份补丁的来历是图节点上的 timeout 被拍平到 `wait` 上）；
//     真卡住由 `--time-limit` / `--max-frames` 这层看门狗报。
std::uint64_t stepTimeoutMs(const Step& s, std::uint64_t default_ms);

// 到期口径（唯一一份）：**已等的**严格超过窗口才算到期（`>` 不是 `>=`）⇒ `timeout 2000`
// 在"刚好等满 2000 ms"的那一刻**还没**到期。两个驱动器共用这一条。
bool stepWindowExpired(std::uint64_t elapsed_ms, std::uint64_t guard_ms);

// ---------------------------------------------------------------------------------------
// 完成度（**唯一一份**判据）：驱动器只负责算"我走到哪了"，结论与退出码口径在这里
// ---------------------------------------------------------------------------------------
enum class RecipeOutcome : std::uint8_t {
    Running = 0,   // 还有指令没消费（跑完了仍然这样 = **任务没走完**）
    Done = 1,      // 全部指令走完
    TimedOut = 2,  // 某一步的守卫到期（没走完，且知道是哪一条）
};

// 退出码（唯一一份）：`Done` ⇒ 0（**不覆盖**调用方的运行结论）；`Running` / `TimedOut` ⇒ 5。
// ★ 5 = 配方没走完的既有口径（历史上由解释器臂 `--interaction-graph` 的两处闸门定义；该入口已退役，
//   AOT 臂沿用同一个数，见 `src/aot/aot_main.cpp`）。
//   ⛔ 不许只打一行警告仍 exit 0 —— 那正是"配方没走完"变成假绿面的形状。
constexpr int kRecipeIncompleteExitCode = 5;
int recipeOutcomeExitCode(RecipeOutcome o);
// 名字（fault 摘要 / 失败报告用；**唯一一份**）。`Running` = "跑完了但还有指令没消费"。
const char* recipeOutcomeName(RecipeOutcome o);

}  // namespace lift
