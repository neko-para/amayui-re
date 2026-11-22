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
//   ④ **交还租约**：注入会话结束（配方走完 / `releaseAll`）⇒ `input::releaseProgram()`，
//      真实鼠标重新生效（`runtime/input_device.h` 的 `Driver` / §3.2 #19 就是它的合同）。
//   ⑤ **回调边界**（2026-11-21 追加）：供给帧线的驱动（`InputPlan::requireCallbackBoundary`）⇒
//      按下与抬起必须落在**不同的 guest 回调**；否则闸门① 的 2 个 KeyLevel 读点可以全在同一个回调里
//      （现场失败臂 press→release 只跨 6 个读点），回调体的状态机一次也看不到按下电平。
//      判据见 `inject/click-crosses-callback`（⛔ 不是"某函数被调用了"）。
//   ⑥ **插件那条驱动也供给帧线**（2026-11-22 追加；同一张缺陷的另一半）：`IntentExecutor` 经
//      `setFrameSource` 拿到 `presenter.frameCount()` 之后，`ctx.input.click` 的抬起同样必须等到
//      `frames >= press_frames + PointerCore::kCallbackFramesBeforeRelease`。判据见
//      `inject/executor-click-crosses-callback`；**负对照** = `inject/executor-click-without-frame-line`
//      （不注入帧线 ⇒ 同一个回调里凑满闸门① 就抬起 ⇒ 上面那条判据会红的事实）。
//      ⛔ 这不是第二套阈值：两处判据都只读 `PointerCore` 的那一个常量。
//
// ⚠ 诚实边界：本用例判的是**注入层的时序与租约**，⛔ 不判"引擎会不会命中那个宝藏"。
//   "真的能被引擎看见"的最终判据只能人工跑一次 SAVE05 点宝藏（命令与期望见交付报告 §8）。
#include <cstdint>
#include <cstdio>
#include <string>

#include "check.h"
#include "runtime/host_window.h"
#include "runtime/input.h"
#include "runtime/input_plan.h"
#include "runtime/intent_executor.h"  // ⑥：插件侧唯一 pump（与 `InputPlan` 共用 `PointerCore`）
#include "runtime/intent_queue.h"

namespace {

// 注入时钟（`InputPlan::setClockForTest`）：⛔ 测试不真睡，墙钟兜底也要能被判。
std::uint64_t g_now_ms = 0;
std::uint64_t testNowMs() { return g_now_ms; }

// ⑥ **假帧线**（= 生产上 `presenter.frameCount()` 的位置）：每"走完一个 guest 回调"由用例 +1。
//   ★ 真实驱动在 Present 里 `++frame_count_`（`present.cpp`）⇒ 这里手动推同一个数，
//     所以"按下发生在帧 N、抬起等到帧 N+2"可以逐拍判。
std::uint64_t g_frames = 0;
std::uint64_t testFrameCount(void* /*ctx*/) { return g_frames; }

// ⑥ 插件 executor 的采样源：本用例只判 click 时序（不装 trigger）⇒ 读数全空。
//   `atPoint` 在没有 armed trigger 时**根本不会采样**，但接口要求一个实现。
struct NullReadings final : lift::plugin::ReadingsSource {
    lift::plugin::TriggerReadings sample(lift::InputPoint /*point*/) override { return {}; }
};

// ⑥ 一条 click intent（1280×720 参考坐标；owner = 本切片的单 owner）。
lift::plugin::Intent clickIntent(int x, int y, lift::MouseButton button) {
    lift::plugin::Intent intent;
    intent.kind = lift::plugin::IntentKind::Click;
    intent.cls = lift::plugin::IntentClass::GuestWrite;
    intent.origin = lift::plugin::Origin::QjsControl;
    intent.owner = 1;
    intent.x = x;
    intent.y = y;
    intent.button = button;
    return intent;
}

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

// ⑥ 同一个读点形状，驱动换成插件侧的 `IntentExecutor`（⛔ 不是第二套泵：就是 `atPoint` 一次）。
void readPoint(lift::WindowManager& wm, lift::plugin::IntentExecutor& exec, lift::InputPoint point,
               bool note) {
    exec.atPoint(wm, point);
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
// ⑤ · **合成 click 必须跨过一个完整的 guest 回调**（2026-11-21 现场：单发 click 4/5 成）
//
// 判据（红了意味着什么）：
//   ① 假 guest = **每个回调只读一次按键电平**（真实引擎的形状：回调体每帧读一次
//      `GetAsyncKeyState` 比对 `3f1`/`3f2`）；一条合成 click 的按下与抬起**必须落在不同的回调**；
//   ② 同一个回调里把宿主墙钟推过 `kReleaseFallbackMs`（旧判据的兜底腿）也**不许**抬起 ——
//      否则那个回调的状态机还没读到按下，抬起就已经发生（旧行为：`reason=keylevel`、press→release
//      只跨 6 个读点，全在同一回调）；
//   ③ 按下电平必须**活到下一个回调的按键读点**（不只是"某函数被调用了"）：假 guest 在下一个
//      回调读 `GetAsyncKeyState` 时按钮仍必须是按下的；
//   ④ 两个完整回调走完之后的下一个读点才抬起，且只抬一次。
// =====================================================================================
void caseClickCrossesCallback() {
    lift_test::g_current_case = "inject/click-crosses-callback";
    Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    plan.requireCallbackBoundary();  // 与 `aot_main` 的装配**同一句**（这个驱动供给帧线）
    f.wm.setInputPlan(&plan);
    g_now_ms = 6000;
    std::string err;
    const bool loaded = plan.loadText("click 640 360 right\n", "mem", &err);
    LIFT_CHECK(loaded);
    if (!loaded) return;

    // ---- 回调 A：臂上 → 按下（此后的按键读点 = 这个回调的状态机读）----
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);   // 阶段 ①：臂上（只 move）
    readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);  // 完成一个位置读点
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);   // 阶段 ②：按下
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.presenting()), 0);
    LIFT_CHECK(f.wm.keyDown(kVkRButton));

    // ---- 同一个回调的按键电平读点：读到按下（flush-before-read），但仍**不许**抬起 ----
    readPoint(f.wm, plan, lift::InputPoint::KeyLevel, true);
    LIFT_CHECK(f.wm.keyDown(kVkRButton));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);

    // ---- 旧判据的兜底腿：墙钟越过 `kReleaseFallbackMs` + 一串位置读点，仍然**不许**抬起 ----
    //   ★ 这 12 个位置读点就是现场失败臂那"6 个读点"的形状（同一个回调里的轮询体）。
    g_now_ms += lift::InputPlan::kReleaseFallbackMs + 100;
    for (int i = 0; i < 12; ++i) {
        readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);  // ★ 旧判据在此红
        LIFT_CHECK(f.wm.keyDown(kVkRButton));
    }

    // ---- 回调 A 走完（Present #1）⇒ 还差一个**完整**回调 ----
    plan.note(lift::PlanEventKind::Frame);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.presenting()), 1);
    readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);

    // ---- 回调 B：这个回调的按键读点必须**仍读到按下**（按下跨过了一个完整回调）----
    readPoint(f.wm, plan, lift::InputPoint::KeyLevel, true);
    LIFT_CHECK(f.wm.keyDown(kVkRButton));  // ★ 这是"按下被下一个回调看见"的判据
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
    // 回调 B 走完（Present #2）⇒ 下一个读点才抬起
    plan.note(lift::PlanEventKind::Frame);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.presenting()), 2);
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK(!f.wm.keyDown(kVkRButton));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.releases()), 1);
    // 读数（不是断言）：press/release 的回调号与保持时长，能直接对账"跨没跨回调"。
    std::printf("[click-crosses-callback] press_frames=0 release_frames=%llu hold_ms=%llu pending=%zu\n",
                static_cast<unsigned long long>(plan.presenting()),
                static_cast<unsigned long long>(g_now_ms - 6000), plan.pendingReleases());
}

// =====================================================================================
// ⑥ · **插件侧的同一条闸门**（`IntentExecutor` + 帧线）：`ctx.input.click` 也必须跨回调
//
// 缺陷形态（2026-11-22 之前）：`IntentExecutor::atPoint` 传 `frames=0 / callback_boundary=false`
//   ⇒ 插件点击的抬起只受闸门①（KeyLevel 读点个数）约束，同一个回调里凑满就抬起
//   （`ctx.input.click` 对载入确认框「是」不生效的根因）。
// 判据（红了意味着什么）：
//   ① 供给帧线时，按下发生在帧 N；**同一个回调里**把闸门① 凑满、墙钟还推过兜底② ⇒ 都不许抬起；
//   ② 帧 N+1（一个完整回调走完）仍然不许抬起；
//   ③ 帧 N+2（第二个完整回调走完）的下一个读点才抬起，且只抬一次；
//   ④ 负对照：**不注入帧线**（= 修复前的形状）⇒ 同一个回调里凑满闸门① 就抬起 ⇒ 上面 ①–③ 的判据
//      在那种形状下必然红。⇒ 这条闸门不是恒真，它真的在起作用。
// ⚠ 两段都只读 `PointerCore::kCallbackFramesBeforeRelease` / `kKeyLevelReadsBeforeRelease`，
//   ⛔ 不另抄阈值。
// =====================================================================================
void caseExecutorClickCrossesCallback() {
    lift_test::g_current_case = "inject/executor-click-crosses-callback";
    Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::plugin::IntentQueue queue;
    NullReadings source;
    lift::plugin::IntentExecutor exec(queue, source);
    exec.setClockForTest(&testNowMs);
    exec.setFrameSource(&g_frames, &testFrameCount);  // 与 `aot_main` 的 `setFrameSource` 同一句
    g_now_ms = 7000;
    g_frames = 0;
    const lift::plugin::Receipt receipt =
        queue.submit(clickIntent(640, 360, lift::MouseButton::Right));
    LIFT_CHECK(receipt.accepted);
    if (!receipt.accepted) return;

    // ---- 回调 A：臂上（只 move）→ 完成一个位置读点 → 按下（帧 0）----
    readPoint(f.wm, exec, lift::InputPoint::PeekMsg, false);   // 臂上（drainQueue → move）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(exec.armedPresses()), 1);
    readPoint(f.wm, exec, lift::InputPoint::CursorPos, true);  // 完成一个位置读点（这一拍还没按）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(exec.pendingReleases()), 0);
    readPoint(f.wm, exec, lift::InputPoint::PeekMsg, false);   // 阶段 ②：按下
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(exec.pendingReleases()), 1);
    LIFT_CHECK(f.wm.keyDown(kVkRButton));
    const std::uint64_t press_frames = g_frames;  // = 0

    // ---- ① 同一个回调里凑满闸门①（2 个 KeyLevel 读点）⇒ 仍然**不许**抬起 ----
    readPoint(f.wm, exec, lift::InputPoint::KeyLevel, true);
    readPoint(f.wm, exec, lift::InputPoint::KeyLevel, true);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(exec.pendingReleases()), 1);  // ★ 旧行为在此抬起
    LIFT_CHECK(f.wm.keyDown(kVkRButton));
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), lift::input::kMkRButton);
    // ---- 旧判据的兜底腿（墙钟 > kReleaseFallbackMs）也**不许**抬起 ----
    g_now_ms += lift::PointerCore::kReleaseFallbackMs + 100;
    readPoint(f.wm, exec, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(exec.pendingReleases()), 1);
    LIFT_CHECK(f.wm.keyDown(kVkRButton));

    // ---- ② 回调 A 走完（Present #1 = 帧 N+1）⇒ 还差一个完整回调 ----
    g_frames = press_frames + 1;
    readPoint(f.wm, exec, lift::InputPoint::KeyLevel, true);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(exec.pendingReleases()), 1);
    LIFT_CHECK(f.wm.keyDown(kVkRButton));

    // ---- ③ 回调 B 走完（Present #2 = 帧 N+2）⇒ 下一个读点才抬起 ----
    g_frames = press_frames + lift::PointerCore::kCallbackFramesBeforeRelease;
    readPoint(f.wm, exec, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(exec.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);
    LIFT_CHECK(!f.wm.keyDown(kVkRButton));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(exec.released()), 1);
    std::printf("[executor-click-crosses-callback] press_frames=%llu release_frames=%llu\n",
                static_cast<unsigned long long>(press_frames),
                static_cast<unsigned long long>(g_frames));
}

// =====================================================================================
// ⑥ 负对照：**不注入帧线**（= 修复前的 `frames=0 / callback_boundary=false`）
//
// 判据（红了意味着什么）：同一个回调里凑满闸门① ⇒ 抬起立刻发生，而且此刻外部帧数仍 < N+2。
//   ⇒ 如果上面的正判据也这样（`callback_boundary` 恒 false），`inject/executor-click-crosses-callback`
//     的第①–③ 条会**当场红**。本用例把那个"红的事实"钉成可执行的负对照（⛔ 不是恒真断言）。
// =====================================================================================
void caseExecutorWithoutFrameLineReleasesEarly() {
    lift_test::g_current_case = "inject/executor-click-without-frame-line";
    Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::plugin::IntentQueue queue;
    NullReadings source;
    lift::plugin::IntentExecutor exec(queue, source);  // ★ **不**调 setFrameSource = 修复前的形状
    exec.setClockForTest(&testNowMs);
    g_now_ms = 8000;
    g_frames = 0;
    const lift::plugin::Receipt receipt =
        queue.submit(clickIntent(640, 360, lift::MouseButton::Right));
    LIFT_CHECK(receipt.accepted);
    if (!receipt.accepted) return;

    // 与正判据**逐拍相同**的前半段（臂上 → 位置读点 → 按下）。
    readPoint(f.wm, exec, lift::InputPoint::PeekMsg, false);
    readPoint(f.wm, exec, lift::InputPoint::CursorPos, true);
    readPoint(f.wm, exec, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(exec.pendingReleases()), 1);
    const std::uint64_t press_frames = g_frames;  // = 0

    // 同一个回调里凑满闸门①（2 个 KeyLevel 读点）⇒ **立刻**抬起（没有 ②′）。
    readPoint(f.wm, exec, lift::InputPoint::KeyLevel, true);
    readPoint(f.wm, exec, lift::InputPoint::KeyLevel, true);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(exec.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);
    LIFT_CHECK(!f.wm.keyDown(kVkRButton));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(exec.released()), 1);
    // ★ 负对照的读数：抬起时**外部帧数根本没到** N+2 ⇒ 正判据在这里必然红。
    LIFT_CHECK(g_frames < press_frames + lift::PointerCore::kCallbackFramesBeforeRelease);
    std::printf("[executor-click-without-frame-line] press_frames=%llu release_frames=%llu (早于 N+2)\n",
                static_cast<unsigned long long>(press_frames),
                static_cast<unsigned long long>(g_frames));
}

// =====================================================================================
// S6 · **拖拽**（`mouse_move` → `mouse_down` → `mouse_move` → `mouse_up`）
//
// 判据（红了意味着什么）：
//   ① down 落在**第一个** `mouse_move` 之后（轻闸门：位置读点完成之后的下一个读点）；
//   ② 拖拽中每个动作各占一个读点（S7 写预算）⇒ guest 每一拍都能看到新的 `WM_MOUSEMOVE`；
//      第二拍 `WM_MOUSEMOVE` 的 wParam 带 `MK_LBUTTON`（设备电平的投影）；
//   ③ up 的 lParam = **B**（不是按下点 A）⇒ 拖拽 up 不回跳；
//   ④ 整个拖拽只有**一对** down/up，且 `mouse_down/up` 都**不进**待抬起队列
//      （⛔ 不套闸门①/②：up 是在普通消息读点上直写的，没有任何 KeyLevel 读点）。
// =====================================================================================
void caseDrag() {
    lift_test::g_current_case = "inject/drag";
    Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    f.wm.setInputPlan(&plan);
    g_now_ms = 5000;
    std::string err;
    const bool loaded = plan.loadText("mouse_move 100 100\nmouse_down\nmouse_move 200 200\nmouse_up\n", "mem", &err);
    if (!loaded) std::fprintf(stderr, "拖拽配方被拒：%s\n", err.c_str());
    LIFT_CHECK(loaded);
    if (!loaded) return;

    const std::size_t q0 = f.wm.queueSize();
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);       // move A（只写光标）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessages(f.wm, q0, lift::input::kWmLButtonDown)), 0);

    // 位置读点这一拍：move 之后**还没完成过** CursorPos 读点 ⇒ 不消费 `mouse_down`。
    g_now_ms += 1;
    readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);

    // 这个位置读点完成之后的下一个读点：按下（在**设备当前位置** A 上）。
    g_now_ms += 1;
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 2);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), lift::input::kMkLButton);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);  // ★ 不套闸门①

    // move B：同一个读点里不能再冒出 up（写预算）⇒ 这里只有 WM_MOUSEMOVE。
    g_now_ms += 1;
    const std::size_t q_move_b = f.wm.queueSize();
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessages(f.wm, q_move_b, lift::input::kWmMouseMove)), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessages(f.wm, q_move_b, lift::input::kWmLButtonUp)), 0);

    // up B：普通消息读点直写（⛔ 不等 KeyLevel）。
    g_now_ms += 1;
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 4);
    LIFT_CHECK_EQ_U32(f.wm.mouseButtons(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.releases()), 0);  // up 是配方直写，不是"自动抬起"
    LIFT_CHECK(plan.outcome() == lift::RecipeOutcome::Done);

    // ---- 只有一对 down/up；up 的 lParam 是 B（⛔ 不是按下点 A）----
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessages(f.wm, q0, lift::input::kWmLButtonDown)), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessages(f.wm, q0, lift::input::kWmLButtonUp)), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessages(f.wm, q0, lift::input::kWmMouseMove)), 2);
    lift::WinMessage down_msg;
    lift::WinMessage up_msg;
    LIFT_CHECK(findMessage(f.wm, q0, lift::input::kWmLButtonDown, &down_msg));
    LIFT_CHECK(findMessage(f.wm, q0, lift::input::kWmLButtonUp, &up_msg));
    LIFT_CHECK_EQ_I64(decodeX(down_msg.lparam), 100);
    LIFT_CHECK_EQ_I64(decodeY(down_msg.lparam), 100);  // 按下点 = 第一个 mouse_move 的位置
    LIFT_CHECK_EQ_I64(decodeX(up_msg.lparam), 200);
    LIFT_CHECK_EQ_I64(decodeY(up_msg.lparam), 200);
    LIFT_CHECK(decodeX(up_msg.lparam) != decodeX(down_msg.lparam));  // ⛔ 不回跳
    // 第二拍 move 的 wParam = 设备电平（按下之后带 MK_LBUTTON）。
    lift::WinMessage m;
    std::uint32_t second_move_wparam = 0;
    std::size_t moves = 0;
    for (std::size_t i = q0; i < f.wm.queueSize(); ++i) {
        if (!f.wm.peekAt(i, &m)) continue;
        if (m.message != lift::input::kWmMouseMove) continue;
        ++moves;
        if (moves == 2) second_move_wparam = m.wparam;
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(moves), 2);
    LIFT_CHECK_EQ_U32(second_move_wparam, lift::input::kMkLButton);

    // 配方走完之后的下一个读点：会话收口（交还租约 ⇒ 真鼠标重新生效）。
    g_now_ms += 1;
    readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);
    LIFT_CHECK(!f.wm.programDriving());
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
// ④ · 交还租约（解释器臂 `input_script` 的收口同口径：`input::releaseProgram`）
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
    LIFT_CHECK(!f.wm.programDriving());
    LIFT_CHECK(lift::input::mouseMove(f.wm, 5, 5, lift::InputSource::Os, 0));
    int cx = 0;
    int cy = 0;
    f.wm.cursorClientPos(f.hwnd, &cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 5);

    // 注入臂上 = Program 持租约（OS 的移动被拒，§3.2 #3）—— 这是 S4 的租约合同。
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);  // 阶段 ①：臂上
    LIFT_CHECK(f.wm.programDriving());
    LIFT_CHECK(!lift::input::mouseMove(f.wm, 6, 6, lift::InputSource::Os, 0));
    f.wm.cursorClientPos(f.hwnd, &cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 10);  // 仍是注入的目标位置

    // 走完一次 click（位置采样 → 按下 → N 个 KeyLevel 读点 → 抬起）；**这一刻**租约还在 Program 手里。
    readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);  // 位置采样（这一拍还没按）
    readPoint(f.wm, plan, lift::InputPoint::PeekMsg, false);   // 阶段 ②：按下
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
    for (std::uint64_t i = 0; i < lift::InputPlan::kKeyLevelReadsBeforeRelease; ++i) {
        g_now_ms += 1;
        readPoint(f.wm, plan, lift::InputPoint::KeyLevel, true);
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK(plan.outcome() == lift::RecipeOutcome::Done);  // 配方走完 = 会话收口

    // 收口：下一个读点把租约交还 OS ⇒ 真鼠标重新生效（`GetCursorPos` 跟着变）。
    g_now_ms += 1;
    readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);
    LIFT_CHECK(!f.wm.programDriving());
    LIFT_CHECK(lift::input::mouseMove(f.wm, 300, 400, lift::InputSource::Os, 0));
    f.wm.cursorClientPos(f.hwnd, &cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 300);
    LIFT_CHECK_EQ_I64(cy, 400);

    // `releaseAll`（AOT 收口 / CTest teardown）也必须交还：再注入一次再收口。
    LIFT_CHECK(plan.enqueueMove(50, 60));
    readPoint(f.wm, plan, lift::InputPoint::CursorPos, true);
    LIFT_CHECK(f.wm.programDriving());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.releaseAll(f.wm)), 0);  // 没有按下的
    LIFT_CHECK(!f.wm.programDriving());
    LIFT_CHECK(lift::input::mouseMove(f.wm, 7, 8, lift::InputSource::Os, 0));
}

}  // namespace

int main() {
    caseTwoPhaseClick();
    caseClickCrossesCallback();
    caseExecutorClickCrossesCallback();
    caseExecutorWithoutFrameLineReleasesEarly();
    caseDrag();
    caseMoveOnly();
    caseWallclockFallbacks();
    caseOwnershipHandback();
    return lift_test::finish("input-inject-visible");
}
