// emulator-lift · 指针施加核心（`InputPlan` 与（C1b）`IntentExecutor` 的**共享一份**）
//
// ★ 为什么有这个基类（2026-11-19 · C1a 行为不变的机械抽取）：click 的"两段 + 抬起 + 三个闸门"
//   原本只存在于 `InputPlan` 的**私有**成员里（`armMoveAt` / `armClickAt` / `pressDue` /
//   `releaseDue` / `emitRelease` / `releaseAll` + 待抬起队列）。C1b 的 executor 要的正是同一段
//   语义 ⇒ 提成**非多态**基类，两个派生类**共用一份实现**。⛔ 一旦设成虚函数，第二个派生类就能
//   "重写一部分" —— 那正是"一个能力两处实现"的入口。
//   · 生命周期：两个派生类都是**栈上 / 成员变量**的具体对象，从不经基类指针 delete
//     ⇒ 析构 protected 非虚（`PointerCore*` 上 delete 是编译错误，不是运行期 UB）。
//
// ★ **W2 边界（本类不持有读数源）**：
//   · **没有钟**：`InputPlan` 的守卫钟要扣暂停（`nowMs()`），executor 用宿主墙钟 ⇒ **两把钟**，
//     由派生类自己算好、经 `PointerPoint::now_ms` 传进来；
//   · **没有 `deliver_calls_` / `read_points_`**：`tools/test/input-plan-shape.test.mjs` ③ 断言
//     `input_plan.cpp` 里 `++deliver_calls_[` **恰好 1 处**（= 那个类的 `atPoint`）；读点计数
//     是**派生类自己的账**，core 只吃 `PointerPoint` 里的三个读数
//     （`key_level_reads` / `cursor_reads_done` / `read_index`）。
//   ⇒ 一个读点 = 一次 `PointerPoint` 值参数；core 里没有"这个读点是谁的"这种第二份状态。
//
// ★ 行为口径的**唯一**偏离（C1a 显式登记）：一个读点内只取**一次**钟（`pt.now_ms`），而旧实现
//   在 `releaseDue` 的循环里每个待抬起项各取一次。阈值是 250 / 500 ms 的兜底，
//   测试注入钟下**逐位相同**；生产上最多差一个读点（≤ 一次读点的墙钟）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "runtime/input_device.h"  // MouseButton
#include "runtime/input_point.h"   // InputPoint / kInputPointCount

namespace lift {

class WindowManager;

// 计划消息的 `MSG.time`：**没有** guest 时钟来源（投递不在 tick 上下文里被调用）⇒ 恒 0。
// 引擎不读它（`interaction-model.md` 的门不碰 MSG.time）。★ 与三个闸门常量放在一起：
// core 与派生类（`applyMouseButton` / `closeSessionIfDone`）用的是同一个数（⛔ 不各写一份 0）。
inline constexpr std::uint32_t kPointerMessageTimeMs = 0;

// =====================================================================================
// 一个读点的**读数**（纯值；core 里没有钟、没有计数 ⇒ 派生类算一次、传进来）
// =====================================================================================
struct PointerPoint {
    InputPoint point = InputPoint::Count;  // 本读点（trace / 抬起那一行用）
    std::uint64_t now_ms = 0;              // 宿主毫秒（**派生类自己的钟**：InputPlan 扣暂停）
    // 本读点已计入的 KeyLevel 读点数（**闸门 ①** 的唯一起点：按下之后至少跨过 N 个这种读点）。
    std::uint64_t key_level_reads = 0;
    // **本读点之前**完成过的 CursorPos 读点数（闸门 ③ / `mouse_down` 的轻闸门）。
    std::uint64_t cursor_reads_done = 0;
    // 读点序号（trace / 待抬起记录的读数；不是性能判据）。
    std::uint64_t read_index = 0;
    // 施加它的 step 行（`InputPlan` 用；executor 恒 0）。
    int line = 0;
};

// =====================================================================================
// `PointerCore` —— 到期的按下 / 抬起 / 写预算的**唯一**实现
// =====================================================================================
class PointerCore {
public:
    // **已按下**、等抬起的按钮（闸门 = `kKeyLevelReadsBeforeRelease` 个 KeyLevel 读点 / 墙钟兜底）。
    struct PendingRelease {
        MouseButton button = MouseButton::Left;
        int x = 0;                       // **已缩放**的客户区坐标（move / down / up 共用）
        int y = 0;
        std::uint64_t press_samples = 0;      // 按下那一刻的 `inputSampleCount`（**只是读数**，⛔ 不是闸门）
        std::uint64_t press_key_reads = 0;    // 按下那一刻的 KeyLevel 读点数 ⇒ **闸门 ①**
        std::uint64_t press_ms = 0;           // 按下那一刻的宿主墙钟 ⇒ **兜底 ②**
        std::uint64_t press_reads = 0;        // 按下那一刻的读点序号（trace 读数）
        int line = 0;                     // 施加它的 step 行（trace 的 `plan:<文件>:<行>`）
        // ★ C1c：提交者的 owner（`Intent::owner`；配方 / 控制层恒 0）。`releaseOwner` 只认它。
        std::uint32_t owner = 0;
        // ★ S9：down 被叠层吞掉 ⇒ up 也**必须**吞（D6），且两者都**不**调 `input::*`。
        //   这里记下 down 的决定；`emitRelease` 只看它，⛔ 不再问一次 router（"down 吞 ⇒ up 吞"）。
        bool swallowed = false;
    };

    // **臂上**（光标已落地、还没按下）的一条 click：等"完成过一次 CursorPos 读点"后才按下。
    struct ArmedPress {
        MouseButton button = MouseButton::Left;
        int x = 0;  // **已缩放**的客户区坐标（按下 / 抬起共用；⛔ 不重新映射）
        int y = 0;
        std::uint64_t arm_reads = 0;           // 臂上时的读点序号（trace 读数）
        std::uint64_t arm_cursor_done = 0;     // 臂上时**已完成**的 CursorPos 读点数 ⇒ 闸门 ③
        std::uint64_t arm_ms = 0;              // 臂上时的宿主墙钟 ⇒ 兜底 ③
        int line = 0;
        // ★ C1c：提交者的 owner（`pressDue` 转进 `PendingRelease` 时**原样带过去**）。
        std::uint32_t owner = 0;
        // ★ S9：down 在**臂上那一刻**就被叠层吞掉了（`routeInjectedPointer(Down,…)` 返回 true）。
        //   ⇒ 不调 `input::mouseMove`、`pressDue` 不调 `input::mouseButton(down)`；但**保留**这条
        //   ArmedPress（三段时序 / 闸门 ①③ 照旧），这样 up 的时机与不吞时同一个形状。
        bool swallowed = false;
    };

    // -------------------------------------------------------------------------------------
    // ★ **注入形状**（2026-11-19 裁决；锚 = `GetAsyncKeyState` 电平读点，⛔ 不是总采样数）
    //
    // 为什么闸门必须按 KeyLevel 读点：引擎的按键**只从电平读**（`GetAsyncKeyState`，
    //   EA `0x477241 / 0x47725D`），而 `WM_LBUTTONDOWN/UP` 在 WndProc 里只写一个**全语料无读者**
    //   的 dword（`+0x1620`，EA `0x4B8DE6`）+ 清去重锁存（`+0x1E70`）⇒ **消息路不能产生点击**。
    //   而 `WindowManager::inputSampleCount()` 把 `GetCursorPos` 也算作样本 ⇒ 位置读点能满足
    //   "等两拍"、按键却一次都没被读到 —— 那正是 2026-11-19 查出来的失效根因（现场读数
    //   `reason=forced samples==press_samples`）⇒ ⛔ **不许**再拿总样本数当闸门。
    // 唯一答案 = `PointerPoint::key_level_reads`（派生类从**它自己的** `deliverCalls(KeyLevel)`
    //   算好传进来 —— 那个计数的唯一来源是派生类的 `atPoint` 第一句，⛔ 没有第二份账）。
    // -------------------------------------------------------------------------------------
    // ① 按下之后至少**跨过**这么多个 KeyLevel 读点才抬起。
    //   ★ 为什么是 2 而不是 1：计数在 `atPoint` 的**第一句** +1，而抬起是**读之前** flush 的
    //     （flush-before-read）⇒ N=1 时抬起落在那个读点自己的读**之前**，guest 一次也看不到按下；
    //     N=2 才留下**至少一次** KeyLevel 读**真的读到**按下电平（读数 N=2 ⇒ 1 次观察；
    //     若按下那次本身是 KeyLevel 读点则 2 次）。
    static constexpr std::uint64_t kKeyLevelReadsBeforeRelease = 2;
    // ② 抬起墙钟兜底：按下之后这么久（宿主单调毫秒，扣暂停）还没够 ①的读点 ⇒ **仍然抬起**。
    //   ★ 为什么 500 ms：实测输入心跳 = `GetCursorPos` **20.02 Hz**（OBS-20261108-07，≈50 ms）
    //     ⇒ 500 ms ≈ **10 个周期**，还在轮询按键的屏都会在兜底之前满足 ①；它只防"这个屏根本
    //     不读按键"（那种屏上按键本来就不可能被看见）⇒ 行为与改动前相同，⛔ 不会永久按住。
    static constexpr std::uint64_t kReleaseFallbackMs = 500;
    // ③ 按下之前：光标必须先落地、且**至少跨过一个读点**（⛔ move 与 down 同读点）。
    //   · 判据 = 臂上之后**完成过**至少一次 `InputPoint::CursorPos` 读点：引擎的位置样本被
    //     `+0x74A4` 锁存，只有它**真的调了** `GetCursorPos` 才会把锁存刷新到新位置
    //     （EA `0x404029 / 0x4040A9`；清零只在注册热点 `0x403BBE / 0x403C39`）；
    //   · 有的屏整屏不读位置 ⇒ 墙钟兜底（250 ms ≈ 5 个周期）保证 click **一定会按下**。
    static constexpr std::uint64_t kPressFallbackMs = 250;

    // ★ 抬起那一行的**唯一外部消费者**（`--input-plan-trace` 的 `plan:<文件>:<行> reason=…`）。
    //   为什么是裸函数指针 + `void*`（同 `InputPlan::setProfileMarkHook` 的既有风格）：
    //   缺省 nullptr ⇒ 每次抬起只多一次判空；⛔ 不用 `std::function`（间接跳转 + 可能的分配）。
    //   文本格式属于**派生类**（`plan:文件:行` 是配方口径）⇒ core 只把读数交出去。
    using ReleaseTraceFn = void (*)(void* ctx, const PendingRelease& r, bool key_seen, bool wallclock,
                                    std::uint64_t samples, InputPoint point);
    void setReleaseTrace(void* ctx, ReleaseTraceFn fn) {
        trace_ctx_ = ctx;
        trace_ = fn;
    }

    // ---- 读数（摘要 / 测试；⛔ 不是性能判据）----
    std::uint64_t fired() const { return fired_; }
    std::uint64_t released() const { return released_; }
    // ★ S9：**被叠层吞掉**的注入 click 数（只有 `--ui-input=consume` + 叠层命中时才涨）。
    std::uint64_t uiConsumed() const { return ui_consumed_; }
    bool buttonPending(MouseButton b) const;
    std::size_t armedPresses() const { return armed_.size(); }
    std::size_t pendingReleases() const { return releases_pending_; }
    std::uint32_t writesThisPoint() const { return writes_this_point_; }

    // ★ 收尾：把**所有**待抬起的按钮抬起（⚠ 不是 step、不推进入口）。
    //   调用点只有一次性的收口（AOT 的 `aot_main` 跑完之后 / CTest 的收尾）——
    //   保证"下一次 run 不会看到按钮一直按着"。返回抬起个数。
    //   ★ 收口顺序见实现：先 `emitRelease` 处理 `pending_`（显式 click 的按下），再
    //     `input::releaseProgram` 处理显式 `mouse_down` / `key_down` 留下的 Program 电平。
    std::size_t releaseAllPending(WindowManager& wm);
    // ★ C1c：释放**一个 owner** 的指针状态（施工图 §4.4）：
    //   · `pending_` 里该 owner 的 ⇒ **真的** post up（⛔ 不能只 erase：那样 guest 永远看到按着）；
    //   · `armed_` 里该 owner 的 ⇒ 直接丢（**没按下过** ⇒ 没有要抬的，⛔ 不伪造一次 up）。
    //   ⛔ 不在这里调 `input::releaseProgram`：那是**全抬**（会清掉别的 owner 的电平），
    //   收口只由派生类的 `closeSessionIfDone`（三队列全空）做。返回真的抬起了几个。
    std::size_t releaseOwner(WindowManager& wm, std::uint32_t owner);

protected:
    ~PointerCore() = default;  // ★ protected 非虚：禁止 `PointerCore*` 上的 delete（不是多态基类）

    // ★ **唯一**的 click 施加核心（阶段 ①）：映射坐标 + `input::mouseMove` + 进 `armed_`。
    //   配方 step 与立即队列**都**走它（同一份 `referenceToClientPixel` + 同一份 `input::*`）。
    //   `owner` 缺省 0：配方 / 控制层的既有调用点逐字不变；executor 把 `Intent::owner` 传进来。
    void armClickAt(WindowManager& wm, int ref_x, int ref_y, MouseButton button, const PointerPoint& pt,
                    std::uint32_t owner = 0);
    // ★ 纯 move 的落地点（`move` 命令 / click 的第一段共用同一份映射与 `input::mouseMove`）。
    void armMoveAt(WindowManager& wm, int ref_x, int ref_y, const PointerPoint& pt);
    // ★ 阶段 ②：把到期的 `armed_` 按下（`input::mouseButton(down)`）并转进 `pending_`。
    //   ★ S7：一次读点**最多按一条**；已在 `pending_` 的按钮**跳过**（等它抬起，⛔ 不重复按下）。
    void pressDue(WindowManager& wm, const PointerPoint& pt);
    // ★ 到期的**抬起**（闸门 ① / 兜底 ②）：一次读点最多抬一条（写预算）；trace 走钩子。
    void releaseDue(WindowManager& wm, const PointerPoint& pt);

    // ---- 派生类直接用的状态（`InputPlan::clear()` 的复位 / `atPoint` 的四段写预算）----
    std::vector<PendingRelease> pending_;  // 已按下、等抬起
    std::vector<ArmedPress> armed_;        // 光标已落地、等"一次完成的位置采样"再按下
    // ★ **写预算**（S7 / Q2）：本读点已经施加了几条写动作（0 / 1；见 `InputPlan::atPoint` 的四段）。
    std::uint32_t writes_this_point_ = 0;
    // 本读点**之前**已经完成过的 CursorPos 读点数（"位置样本的年龄"刻度；见 `kPressFallbackMs`）。
    std::uint64_t cursor_reads_done_ = 0;
    std::uint64_t fired_ = 0;     // 已施加 click 数
    std::uint64_t released_ = 0;
    // ★ S9：被叠层吞掉的注入 click 数（Down 那一瞬 +1；见 `uiConsumed()`）。
    std::uint64_t ui_consumed_ = 0;
    // ★ `mouse_down` 轻闸门（S6 / Q3）：上一次**设备移动**时的 `cursor_reads_done_` 与宿主墙钟
    //   （`armMoveAt` 刷新；见 `input_plan.h` 类头的"轻闸门"段）。
    std::uint64_t last_move_cursor_done_ = 0;
    std::uint64_t last_move_ms_ = 0;
    std::size_t releases_pending_ = 0;
    // 本次注入会话是否已经把光标所有权交还过（`closeSessionIfDone` 只做一次；新的注入会清掉它）。
    bool session_closed_ = true;

    // ★ 把所有指针状态复位（`InputPlan::clear()` 那一段的机械改写；⛔ `ui_consumed_` 不在这里 ——
    //   旧 `clear()` 也不复位它，行为逐位不变）。
    void clearPointerState();

private:
    void emitRelease(WindowManager& wm, const PendingRelease& r);
    // 抬起那一行的钩子（缺省 nullptr ⇒ 每次抬起一次判空；见 `setReleaseTrace`）。
    void* trace_ctx_ = nullptr;
    ReleaseTraceFn trace_ = nullptr;
};

}  // namespace lift
