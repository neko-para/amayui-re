// emulator-lift · **注入真的能被引擎看见**（pure，顶层 CTest 注册）
//
// ★ 这条守卫防的是什么（红了意味着什么）：
//   引擎的按键**只从电平读**（`GetAsyncKeyState`，EA `0x477241 / 0x47725D`），而
//   `WM_LBUTTONDOWN/UP` 在 WndProc 里只写一个**全语料没有读者**的 dword（`+0x1620`，EA `0x4B8DE6`）
//   + 清一个去重锁存（`+0x1E70`，EA `0x4B8DC5`）⇒ **消息路不能产生点击**。
//   于是"注入的 click 有没有被引擎看见"只有一个可判的读数：**按下之后 guest 有没有读过按键电平**。
//   2026-11-19 的失效根因正是：抬起闸门用的是 `WindowManager::inputSampleCount()`，而那个计数把
//   `GetCursorPos` 也算作样本 ⇒ 位置读点满足了闸门、按键一次都没被读到，按下就被抬起了
//   （现场读数 `reason=forced samples==press_samples`）。
//
// 判据（四条，逐条会红）：
//   ① **形状**：click 分两段 —— 阶段 ① 只 move（光标到位、⛔ 没有 `WM_LBUTTONDOWN`）；
//      阶段 ② 要等一个**已完成的位置读点**（引擎的位置样本被 `+0x74A4` 锁存，只有它真的调了
//      `GetCursorPos` 才会刷新）才按下。⛔ "move 与 down 同读点"的旧形状一旦回来，本用例立刻红。
//   ② **闸门**：按下之后**位置读点再多也不许抬起**（这一条就是 2026-11-19 的根因；把闸门换回
//      `inputSampleCount()` 时它当场红）；必须跨过 `kKeyLevelReadsBeforeRelease` 个 **KeyLevel** 读点。
//   ③ **兜底**：一次 KeyLevel 读点都不来的屏（纯消息型）⇒ 墙钟到期仍然抬起（⛔ 永久按住），
//      而且到期之前**不许**提前抬起；整屏不读位置时 click 也**一定**能按下（`kPressFallbackMs`）。
//   ④ **交还光标所有权**：注入会话结束（配方走完 / `releaseAll`）⇒ `releaseScriptOwnership()`，
//      真实鼠标重新生效（`host_window.h` 的 InputSource 段就是它的合同）。
//
// ⚠ 诚实边界：本用例判的是**注入层的时序与所有权**，⛔ 不判"引擎会不会命中那个宝藏"。
//   "真的能被引擎看见"的最终判据只能人工跑一次 SAVE05 点宝藏（命令与期望见交付报告 §8）。
#include <cstdint>
#include <cstdio>
#include <string>

#include "check.h"
#include "runtime/host_window.h"
#include "runtime/input.h"
#include "runtime/input_plan.h"

namespace {

// 注入时钟（`InputPlan::setClockForTest`）：⛔ 测试不真睡，墙钟兜底也要能被判。
std::uint64_t g_now_ms = 0;
std::uint64_t testNowMs() { return g_now_ms; }

// VK_LBUTTON / VK_RBUTTON（Win32 常量；本文件只读电平）。
constexpr std::uint32_t kVkLButton = 0x01u;
constexpr std::uint32_t kVkRButton = 0x02u;

int decodeX(std::uint32_t lparam) { return static_cast<int>(lparam & 0xFFFFu); }
int decodeY(std::uint32_t lparam) { return static_cast<int>(lparam >> 16); }

// 从队首 `from` 起找第一条 `message`（与 input_plan.cpp 的夹具同形；⛔ 不取走）。
bool findMessage(lift::WindowManager& wm, std::size_t from, std::uint32_t message, lift::WinMessage* out) {
    for (std::size_t i = from; i < wm.queueSize(); ++i) {
        lift::WinMessage m;
        if (!wm.peekAt(i, &m)) continue;
        if (m.message == message) {
            *out = m;
            return true;
        }
    }
    return false;
}

std::size_t countMessages(lift::WindowManager& wm, std::size_t from, std::uint32_t message) {
    std::size_t n = 0;
    for (std::size_t i = from; i < wm.queueSize(); ++i) {
        lift::WinMessage m;
        if (!wm.peekAt(i, &m)) continue;
        if (m.message == message) ++n;
    }
    return n;
}

// 一个真实形状的 guest 读点：**先**让投递层推进（flush-before-read），**再**让 shim 读。
//   `note=true` 模拟 `GetCursorPos` / `GetAsyncKeyState` 里的 `noteInputSample`（总采样数 +1）。
void readPoint(lift::WindowManager& wm, lift::InputPlan& plan, lift::InputPoint point, bool note) {
    plan.atPoint(wm, point);
    if (note) wm.noteInputSample(g_now_ms);
}

struct Fixture {
    lift::WindowManager wm;
    std::uint32_t hwnd = 0;
    bool ok = false;
    Fixture() {
        // 客户区 = 1280×720（无标题栏样式）⇒ 参考坐标恒等映射（`cursor_map.h`）。
        hwnd = wm.createWindow("InjTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
        ok = hwnd != 0;
    }
};

// =====================================================================================
// ① + ② · click 的两段形状 + 抬起闸门（**位置读点不许满足它**）
// =====================================================================================
void caseTwoPhaseClick() {
    lift_test::g_current_case = "inject/click-two-phase";
    Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    f.wm.setInputPlan(&plan);
    g_now_ms = 1000;
    std::string err;
    LIFT_CHECK(plan.loadText("click 100 200\n", "mem", &err));
    if (!err.empty()) std::fprintf(stderr, "loadText err: %s\n", err.c_str());

    // ---- 阶段 ①：臂上（只 move）。**不要**用位置读点当臂上点，否则"位置样本刷新"这一条会被
    //      同一次读点顺带满足 —— 用纯消息读点才能证明两段真的隔着读点。----
    const std::size_t q0 = f.wm.queueSize();
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);
    LIFT_CHECK(!f.wm.keyDown(kVkLButton));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessages(f.wm, q0, lift::input::kWmLButtonDown)), 0);
    int cx = 0;
    int cy = 0;
    f.wm.cursorClientPos(f.hwnd, &cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 100);  // 虚拟光标**立刻**到位
    LIFT_CHECK_EQ_I64(cy, 200);
    lift::WinMessage move_msg;
    LIFT_CHECK(findMessage(f.wm, q0, lift::input::kWmMouseMove, &move_msg));
    LIFT_CHECK_EQ_I64(decodeX(move_msg.lparam), 100);
    LIFT_CHECK_EQ_I64(decodeY(move_msg.lparam), 200);

    // ---- 只跨"一个读点"还不够：位置样本没刷新过 ⇒ **不按下**（旧形状会在这里按下）----
    g_now_ms += 1;
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);

    // ---- 阶段 ②：必须**完成**一个位置读点之后才按下 ----
    //   为什么是"完成之后的下一个读点"而不是"那次位置读点本身"：抬起/按下都在 shim 的读**之前**
    //   flush（flush-before-read）⇒ 在位置读点那一拍按下，等于"引擎还没采过位置就已经按下了"。
    //   ⇒ 先过掉位置读点（此刻仍然只有 move），下一个读点才按下。
    g_now_ms += 1;
    readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 1);   // 位置采样这一拍还没按
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);

    g_now_ms += 1;
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), lift::input::kMkLButton);
    LIFT_CHECK(f.wm.keyDown(kVkLButton));
    lift::WinMessage down_msg;
    LIFT_CHECK(findMessage(f.wm, q0, lift::input::kWmLButtonDown, &down_msg));
    LIFT_CHECK_EQ_I64(decodeX(down_msg.lparam), 100);  // 与 move **同一个**映射结果
    LIFT_CHECK_EQ_I64(decodeY(down_msg.lparam), 200);

    // ---- ② 位置读点**再多也不许抬起**（2026-11-19 的根因就在这里）----
    for (int i = 0; i < 8; ++i) {
        g_now_ms += 1;
        readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);  // 总采样数持续前进
    }
    LIFT_CHECK(f.wm.inputSampleCount() >= 8);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);  // ⛔ 还没抬起
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), lift::input::kMkLButton);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessages(f.wm, q0, lift::input::kWmLButtonUp)), 0);

    // ---- 按键电平读点：前 N-1 个还不够（N-1 时抬起落在读之前 ⇒ guest 一次也看不到按下）----
    for (std::uint64_t i = 0; i + 1 < lift::InputPlan::kKeyLevelReadsBeforeRelease; ++i) {
        g_now_ms += 1;
        readPoint(f.wm, plan, lift::InputPoint::KeyLevel, true);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
        LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), lift::input::kMkLButton);
    }
    // ---- 第 N 个 ⇒ 抬起 ----
    g_now_ms += 1;
    readPoint(f.wm, plan, lift::InputPoint::KeyLevel, true);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);
    LIFT_CHECK(!f.wm.keyDown(kVkLButton));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessages(f.wm, q0, lift::input::kWmLButtonUp)), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.releases()), 1);
}

// =====================================================================================
// ②' · 纯 move（`emu-ctl move` 的落地面）：只写光标，⛔ 不按键、⛔ 不冒充一次点击
// =====================================================================================
void caseMoveOnly() {
    lift_test::g_current_case = "inject/move-only";
    Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    f.wm.setInputPlan(&plan);
    g_now_ms = 2000;

    const std::size_t q0 = f.wm.queueSize();
    LIFT_CHECK(plan.enqueueMove(740, 355));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.queuedClicks()), 1);
    readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.queuedClicks()), 0);  // 已落地
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);         // ⛔ 不是"动作"
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessages(f.wm, q0, lift::input::kWmLButtonDown)), 0);
    int cx = 0;
    int cy = 0;
    f.wm.cursorClientPos(f.hwnd, &cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 740);
    LIFT_CHECK_EQ_I64(cy, 355);

    // 再点几个读点也不会有任何按下（move 不排队、不进 armed_）。
    for (int i = 0; i < 4; ++i) {
        g_now_ms += 1;
        readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);
    }
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);

    // 第二条 move 换个位置：唯一消息仍是 WM_MOUSEMOVE。
    const std::size_t q1 = f.wm.queueSize();
    LIFT_CHECK(plan.enqueueMove(10, 20));
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);
    f.wm.cursorClientPos(f.hwnd, &cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 10);
    LIFT_CHECK_EQ_I64(cy, 20);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessages(f.wm, q1, lift::input::kWmMouseMove)), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessages(f.wm, q1, lift::input::kWmLButtonDown)), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);
}

// =====================================================================================
// ③ · 两条墙钟兜底：不读位置也**一定**按下；不读按键也**一定**抬起（都在到期之前不动作）
// =====================================================================================
void caseWallclockFallbacks() {
    lift_test::g_current_case = "inject/wallclock";
    Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    f.wm.setInputPlan(&plan);
    g_now_ms = 3000;
    std::string err;
    LIFT_CHECK(plan.loadText("click 10 20\n", "mem", &err));

    // 臂上（纯消息读点；这个屏此后整屏只读消息 ⇒ 永远没有位置读点）。
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 1);

    // 兜底窗口内：**不**按下（按下要等"位置样本刷新"或兜底到期）。
    g_now_ms += lift::InputPlan::kPressFallbackMs / 5;
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 1);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);

    // 越过 `kPressFallbackMs` ⇒ 仍然按下（⛔ 不让 click 永远挂着）。
    g_now_ms += lift::InputPlan::kPressFallbackMs;
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), lift::input::kMkLButton);

    // 这个屏也**不读按键电平**：按键闸门永远不成立。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.deliverCalls(lift::InputPoint::KeyLevel)), 0);

    // 抬起兜底窗口内：**不**抬起（提前抬起 = 按下从没被看见）。
    g_now_ms += lift::InputPlan::kReleaseFallbackMs / 5;
    for (int i = 0; i < 10; ++i) {
        readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
    }
    // 越过 `kReleaseFallbackMs` ⇒ 抬起。
    g_now_ms += lift::InputPlan::kReleaseFallbackMs;
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);
}

// =====================================================================================
// ④ · 交还光标所有权（解释器臂 `input_script` 的收口同口径）
// =====================================================================================
void caseOwnershipHandback() {
    lift_test::g_current_case = "inject/handback";
    Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    f.wm.setInputPlan(&plan);
    g_now_ms = 4000;
    std::string err;
    LIFT_CHECK(plan.loadText("click 10 20\n", "mem", &err));

    // 注入前：OS 能挪光标。
    LIFT_CHECK(!f.wm.cursorOwnedByScript());
    LIFT_CHECK(lift::input::mouseMove(f.wm, 5, 5, lift::InputSource::Os, 0));
    int cx = 0;
    int cy = 0;
    f.wm.cursorClientPos(f.hwnd, &cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 5);

    // 注入臂上 = 光标归脚本（OS 的移动被拒）—— 这是既有的所有权合同，⛔ 本轮不改。
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);  // 阶段 ①：臂上
    LIFT_CHECK(f.wm.cursorOwnedByScript());
    LIFT_CHECK(!lift::input::mouseMove(f.wm, 6, 6, lift::InputSource::Os, 0));
    f.wm.cursorClientPos(f.hwnd, &cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 10);  // 仍是注入的目标位置

    // 走完一次 click（位置采样 → 按下 → N 个 KeyLevel 读点 → 抬起）；**这一刻**所有权还在脚本手里。
    readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);  // 位置采样（这一拍还没按）
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);   // 阶段 ②：按下
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
    for (std::uint64_t i = 0; i < lift::InputPlan::kKeyLevelReadsBeforeRelease; ++i) {
        g_now_ms += 1;
        readPoint(f.wm, plan, lift::InputPoint::KeyLevel, true);
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK(plan.outcome() == lift::RecipeOutcome::Done);  // 配方走完 = 会话收口

    // 收口：下一个读点把所有权交还 OS ⇒ 真鼠标重新生效（`GetCursorPos` 跟着变）。
    g_now_ms += 1;
    readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);
    LIFT_CHECK(!f.wm.cursorOwnedByScript());
    LIFT_CHECK(lift::input::mouseMove(f.wm, 300, 400, lift::InputSource::Os, 0));
    f.wm.cursorClientPos(f.hwnd, &cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 300);
    LIFT_CHECK_EQ_I64(cy, 400);

    // `releaseAll`（AOT 收口 / CTest teardown）也必须交还：再注入一次再收口。
    LIFT_CHECK(plan.enqueueMove(50, 60));
    readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);
    LIFT_CHECK(f.wm.cursorOwnedByScript());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.releaseAll(f.wm)), 0);  // 没有按下的
    LIFT_CHECK(!f.wm.cursorOwnedByScript());
    LIFT_CHECK(lift::input::mouseMove(f.wm, 7, 8, lift::InputSource::Os, 0));
}

}  // namespace

int main() {
    caseTwoPhaseClick();
    caseMoveOnly();
    caseWallclockFallbacks();
    caseOwnershipHandback();
    return lift_test::finish("input-inject-visible");
}
