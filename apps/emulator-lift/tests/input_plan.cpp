// emulator-lift · 输入投递层 S1 的**关系断言**（pure，顶层 CTest 注册）
//
// 判据（红了意味着什么）：
//   ① 每个读点调一次 shim ⇒ `InputPlan::deliverCalls(point)` 与 `ShimDef::call_count`
//      **逐点相等**。这两个数来自**两个独立来源**（计划自己的账 vs shim 框架唯一的 `++` 点
//      `shims.cpp`）⇒ 红了 = "有一条读点没经过投递入口"或"投递入口被挪走了"。
//   ② `InputPlan::armed()` 在 S1 恒 false（没有配方），且 `atPoint()` **不碰** `WindowManager`
//      —— 这一条由"去掉计划之后 `deliverCalls` 不再增长、而 shim 照常被调用"钉住。
//   ③ 没有计划（`setInputPlan(nullptr)`，= 缺省档）时投递层只有一次判空：计数不动，读点行为不变。
//
// ⚠ S1 的诚实边界：`ReadFile`（第 11 个投递点，只是**事件源**）**不在**本用例里 ——
//   它要一个有效文件句柄（`CreateFileA` + `Vfs` 的 game_dir），那属于 `tests/file_io.cpp` 的夹具面；
//   S1 里它由源码守卫 ② 的 `std::fread(` 谓词判"接线一致"，运行期关系断言留给 S2/S3
//   （那时才有会消费 `FileReadDone` 的门）。⛔ 不要为此在夹具里造第二份 VFS。
//
// ★ 引用单：`REQ-01M7G7EMX3JR272D8FVM7JFY9D`（父单 `REQ-01M7G7EJF062NKQ0D40RF3BZ4Z`）。
#include <cstdint>

#include "check.h"
#include "fixture.h"
#include "runtime/host_window.h"
#include "runtime/input_plan.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kWmKeyDown = 0x0100u;
constexpr std::uint32_t kWmChar = 0x0102u;
constexpr std::uint32_t kQsPostMessage = 0x0008u;

struct Point {
    lift::InputPoint point;
    const char* module;
    const char* name;
};

// 除 `ReadFile` 外的 10 个投递点（9 读点 + 1 写点）。顺序 = 实际调用顺序。
const Point kPoints[] = {
    {lift::InputPoint::GetMsg, "user32", "GetMessageA"},
    {lift::InputPoint::PeekMsg, "user32", "PeekMessageA"},
    {lift::InputPoint::MsgWait, "user32", "MsgWaitForMultipleObjects"},
    {lift::InputPoint::WaitMessage, "user32", "WaitMessage"},
    {lift::InputPoint::QueueStatus, "user32", "GetQueueStatus"},
    {lift::InputPoint::Translate, "user32", "TranslateMessage"},
    {lift::InputPoint::Dispatch, "user32", "DispatchMessageA"},
    {lift::InputPoint::KeyLevel, "user32", "GetAsyncKeyState"},
    {lift::InputPoint::CursorPos, "user32", "GetCursorPos"},
    {lift::InputPoint::SetCursorPos, "user32", "SetCursorPos"},
};

void postNull(lift::WindowManager* wm) {
    lift::WinMessage m;
    m.message = 0u;  // WM_NULL：只为了让 queue 类读点不落到 shimAbort（headless 空队列 = 响亮失败）
    wm->post(m);
}

void caseRelation() {
    lift_test::g_current_case = "input-plan/read-points-relation";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::registerUser32WindowShims(f.m.shims());
    lift::registerUser32InputShims(f.m.shims());

    lift::WindowManager* wm = f.m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return;

    lift::InputPlan plan;
    LIFT_CHECK(!plan.armed());  // S1：没有配方 ⇒ 恒空
    wm->setInputPlan(&plan);

    // 各 shim 的桩地址（`Fixture::use` 会 `installShim`，否则 guest 的 call 会去解码桩页字节）。
    lift::ShimDef* defs[sizeof(kPoints) / sizeof(kPoints[0])] = {};
    std::uint32_t stubs[sizeof(kPoints) / sizeof(kPoints[0])] = {};
    for (std::size_t i = 0; i < sizeof(kPoints) / sizeof(kPoints[0]); ++i) {
        defs[i] = f.m.shims().findMutable(kPoints[i].module, kPoints[i].name);
        LIFT_CHECK(defs[i] != nullptr);
        stubs[i] = f.use(kPoints[i].module, kPoints[i].name);
        LIFT_CHECK(stubs[i] != 0);
    }
    LIFT_CHECK(f.ok);
    if (!f.ok) return;

    const std::uint32_t msg = f.zeroData(28);      // GetMessageA / PeekMessageA 的 MSG 落点
    const std::uint32_t key_msg = f.zeroData(28);  // TranslateMessage / DispatchMessageA 的输入
    const std::uint32_t point = f.zeroData(8);     // GetCursorPos 的 POINT
    LIFT_CHECK(msg != 0 && key_msg != 0 && point != 0);

    // ---- 顺序：队列类先跑（必须有消息可拿，否则 GetMessageA 会响亮失败）----
    postNull(wm);
    LIFT_CHECK_EQ_U32(f.call(stubs[0], {msg, 0u, 0u, 0u}), 1u);  // GetMessageA（argc=4）
    postNull(wm);
    LIFT_CHECK_EQ_U32(f.call(stubs[1], {msg, 0u, 0u, 0u, 0u}), 1u);  // PeekMessageA（NOREMOVE）
    LIFT_CHECK_EQ_U32(f.call(stubs[2], {0u, 0u, 0u, 0u, kQsPostMessage}), 0u);  // MsgWait：WAIT_OBJECT_0+0
    LIFT_CHECK_EQ_U32(f.call(stubs[3], {}), 1u);                     // WaitMessage
    LIFT_CHECK_EQ_U32(f.call(stubs[4], {0u}), 0u);                   // GetQueueStatus（P0 恒 0）

    // TranslateMessage：MSG.message = WM_KEYDOWN, wParam = 'A'（走 `charForVirtualKey` 的字母分支）
    f.m.mem().wr32(key_msg + 4, kWmKeyDown);
    f.m.mem().wr32(key_msg + 8, 'A');
    LIFT_CHECK_EQ_U32(f.call(stubs[5], {key_msg}), 1u);  // TranslateMessage（会 post 一条 WM_CHAR）
    LIFT_CHECK_EQ_U32(f.call(stubs[6], {key_msg}), 0u);  // DispatchMessageA：hwnd=0 ⇒ DefWindowProc
    LIFT_CHECK_EQ_U32(f.call(stubs[7], {0x41u}), 0u);    // GetAsyncKeyState（没人按下 'A'）
    LIFT_CHECK_EQ_U32(f.call(stubs[8], {point}), 1u);    // GetCursorPos
    LIFT_CHECK_EQ_U32(f.call(stubs[9], {10u, 20u}), 1u); // SetCursorPos（无 sink ⇒ 只改虚拟光标）

    // ---- ① 两个独立来源逐点相等 ----
    for (std::size_t i = 0; i < sizeof(kPoints) / sizeof(kPoints[0]); ++i) {
        const std::uint64_t delivered = plan.deliverCalls(kPoints[i].point);
        const std::uint64_t shim_calls = defs[i] != nullptr ? defs[i]->call_count : 0;
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(delivered), static_cast<std::int64_t>(shim_calls));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(shim_calls), 1);
    }
    // `WM_CHAR` 是 TranslateMessage 自己 post 的（不经过投递层）⇒ 队列里确实多了一条；
    // 这条只用来证明上一步真的走了翻译分支，不参与关系断言。
    lift::WinMessage char_msg;
    bool saw_char = false;
    for (std::size_t i = 0; i < wm->queueSize(); ++i) {
        if (wm->peekAt(i, &char_msg) && char_msg.message == kWmChar) saw_char = true;
    }
    LIFT_CHECK(saw_char);

    // ---- ② + ③ 没有计划 ⇒ 投递层只有一次判空：计数不动，shim 照常被调用 ----
    const std::uint64_t before = plan.deliverCalls(lift::InputPoint::KeyLevel);
    const std::uint64_t shim_before = defs[7] != nullptr ? defs[7]->call_count : 0;
    wm->setInputPlan(nullptr);
    LIFT_CHECK_EQ_U32(f.call(stubs[7], {0x41u}), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.deliverCalls(lift::InputPoint::KeyLevel)),
                      static_cast<std::int64_t>(before));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(defs[7]->call_count),
                      static_cast<std::int64_t>(shim_before + 1));
    LIFT_CHECK(!plan.armed());
}

// =====================================================================================
// S4 + S2′ · 门 `{Always, Presents}` + flush-before-read + 待抬起队列
//
// 判据（红了意味着什么）——★ 全部是**关系**断言，不是"当前取值"断言：
//   ① 计划**生效** ⇒ 同一个读点上 `plan.deliverCalls(point)` 与 `ShimDef::call_count` **同步**增长
//      （两个独立来源：计划自己的账 vs shim 框架唯一的 `++` 点）。
//   ② 拾起之后 `WindowManager` 看到的是**按下态**（`keyDown(vk)` / `mouseButtons()`）——
//      即 flush-before-read 真的在**读之前**落地（不是"下一次"）。
//   ③ 待抬起队列在**结束前被清空**：`releaseAll()` 之后 `pendingReleases()==0` 且状态回到抬起态
//      ⇒ 下一次 run 不会看到"键一直按着"。
//   ④ `Presents` 门是**绝对**帧数、且差一帧不触发（第 N−1 帧的读点不按、第 N 帧之后的下一个读点按）。
// =====================================================================================
void caseGatesAndReleases() {
    lift_test::g_current_case = "input-plan/gates-and-releases";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::registerUser32WindowShims(f.m.shims());

    lift::WindowManager* wm = f.m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return;

    // 两个 step：① Always 起手按一次回车；② guest 累计 Present 到第 3 帧之后点一下 (100,200)。
    const char* const kPlan =
        "# 引擎面：门 + 待抬起\n"
        "await always\n"
        "key 13\n"
        "await presents 3\n"
        "click 100 200\n";
    lift::InputPlan plan;
    std::string plan_err;
    const bool loaded = plan.loadText(kPlan, "mem", &plan_err);
    if (!loaded) std::fprintf(stderr, "loadText err: %s\n", plan_err.c_str());
    LIFT_CHECK(loaded);
    if (!loaded) return;
    LIFT_CHECK(plan.armed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.steps()), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 0);

    wm->setInputPlan(&plan);
    lift::ShimDef* key_def = f.m.shims().findMutable("user32", "GetAsyncKeyState");
    LIFT_CHECK(key_def != nullptr);
    const std::uint32_t key_stub = f.use("user32", "GetAsyncKeyState");
    LIFT_CHECK(key_stub != 0);
    LIFT_CHECK(f.ok);
    if (!f.ok || key_def == nullptr) return;

    constexpr std::uint32_t kVkReturn = 13u;

    // ---- ① + ② 第 1 个读点：Always 立即到期 ⇒ 按下，且**同一次**读就看到按下态 ----
    LIFT_CHECK_EQ_U32(f.call(key_stub, {kVkReturn}), 0x8000u);  // 按下态会带 0x8000 位
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.deliverCalls(lift::InputPoint::KeyLevel)),
                      static_cast<std::int64_t>(key_def->call_count));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(key_def->call_count), 1);
    LIFT_CHECK(plan.pendingReleases() >= 1);
    LIFT_CHECK(wm->keyDown(kVkReturn));

    // ---- ③ 待抬起：guest 采样前进过一拍之后才抬（⛔ 不是"下一次读就抬"）----
    LIFT_CHECK_EQ_U32(f.call(key_stub, {kVkReturn}), 0x8000u);
    LIFT_CHECK(wm->keyDown(kVkReturn));
    LIFT_CHECK_EQ_U32(f.call(key_stub, {kVkReturn}), 0u);  // 抬起 ⇒ 同一次读看到"没按下"
    LIFT_CHECK(!wm->keyDown(kVkReturn));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK(plan.releases() >= 1);

    // ---- ④ `presents` 门：差一帧不触发；到点之后的下一个读点触发 ----
    plan.note(lift::PlanEventKind::Frame);
    plan.note(lift::PlanEventKind::Frame);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.presenting()), 2);
    LIFT_CHECK_EQ_U32(f.call(key_stub, {kVkReturn}), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);  // 还是第 1 步
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);                             // 没点
    plan.note(lift::PlanEventKind::Frame);                                 // 第 3 帧
    LIFT_CHECK_EQ_U32(f.call(key_stub, {kVkReturn}), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 2);
    LIFT_CHECK(wm->mouseButtons() != 0u);  // 同一次读点里按钮已是按下态
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);

    // ---- ③ 收尾保证：`releaseAll()` 把待抬起全部抬掉（一次 run 结束前必须清空）----
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.releaseAll(*wm)), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.releaseAll(*wm)), 0);  // 幂等

    // ---- 摘掉计划：读点行为回到"只有一次判空"（降级路径不变）----
    wm->setInputPlan(nullptr);
    LIFT_CHECK_EQ_U32(f.call(key_stub, {kVkReturn}), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(key_def->call_count), 6);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.deliverCalls(lift::InputPoint::KeyLevel)), 5);
}

}  // namespace

int main() {
    caseRelation();
    caseGatesAndReleases();
    return lift_test::finish("input-plan-read-points");
}
