// emulator-lift · **插件注入的点击也要跨过 guest 回调**（pure，顶层 CTest 注册）
//
// ★ 这条守卫防的是什么（红了意味着什么）：`ctx.input.click` 走 `IntentExecutor` → `PointerCore`，
//   而"按下与抬起落在**不同的** guest 回调"这条闸门靠喂给 `PointerCore` 的**帧线**（`frames`）。
//   2026-11-22 之前的失效形状：`IntentExecutor::atPoint` 传 `frames=0 / callback_boundary=false`
//   ⇒ 插件点击的抬起只受闸门①（KeyLevel 读点个数）约束，同一个回调里凑满就抬起
//   （`ctx.input.click` 对载入确认框「是」不生效的根因）。
//
// 判据（两条，逐条会红）：
//   ⑥ **插件那条驱动也供给帧线**：`IntentExecutor` 经 `setFrameSource` 拿到 `presenter.frameCount()`
//      之后，`ctx.input.click` 的抬起必须等到
//      `frames >= press_frames + PointerCore::kCallbackFramesBeforeRelease`。判据见
//      `inject/executor-click-crosses-callback`；**负对照** = `inject/executor-click-without-frame-line`
//      （不注入帧线 ⇒ 同一个回调里凑满闸门① 就抬起 ⇒ 上面那条判据会红的事实）。
//      ⛔ 这不是第二套阈值：两处判据都只读 `PointerCore` 的那一个常量。
//
// ★ 2026-11-27（用户裁决 `REQ-01M875NDRETPPNN68VCE3VMCT1`）：旧 ①–⑤（`InputPlan` 驱动的
//   click 两段形状 / 三闸门 / 墙钟兜底 / 拖拽 / 纯 move / 交还租约）随 `--input-plan` 退役**已删**。
//   `PointerCore` / `IntentExecutor` 本身是产品侧、本批**不动**（下一批删 `InputPlan` 类时再议
//   那些夹具的迁法）。本文件只留**插件路径**的两条（它们只用共享常量，不碰 `InputPlan`）。
//
// ⚠ 诚实边界：本用例判的是**注入层的时序**，⛔ 不判"引擎会不会命中那个宝藏"。
//   "真的能被引擎看见"的最终判据只能人工跑一次 SAVE05 点宝藏（命令与期望见交付报告 §8）。
#include <cstdint>
#include <cstdio>

#include "check.h"
#include "runtime/host_window.h"
#include "runtime/input.h"
#include "runtime/intent_executor.h"  // ⑥：插件侧唯一 pump（`PointerCore` 的闸门全靠它供给帧线）
#include "runtime/intent_queue.h"

namespace {

// 注入时钟（`IntentExecutor::setClockForTest`）：⛔ 测试不真睡，墙钟兜底也要能被判。
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

// VK_RBUTTON（Win32 常量；本文件只读电平）。
constexpr std::uint32_t kVkRButton = 0x02u;

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

}  // namespace

int main() {
    caseExecutorClickCrossesCallback();
    caseExecutorWithoutFrameLineReleasesEarly();
    return lift_test::finish("input-inject-visible");
}
