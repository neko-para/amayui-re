// emulator-lift · 输入投递层（关系断言 + 四条指令的配方语义；pure，顶层 CTest 注册）
//
// 判据（红了意味着什么）：
//   ① 每个读点调一次 shim ⇒ `InputPlan::deliverCalls(point)` 与 `ShimDef::call_count`
//      **逐点相等**。这两个数来自**两个独立来源**（计划自己的账 vs shim 框架唯一的 `++` 点
//      `shims.cpp`）⇒ 红了 = "有一条读点没经过投递入口"或"投递入口被挪走了"。
//   ② `caseRelation` **不装配方** ⇒ `armed()` 恒 false，且 `atPoint()` 只累加计数
//      —— 这一条由"去掉计划之后 `deliverCalls` 不再增长、而 shim 照常被调用"钉住。
//   ③ 没有计划（`setInputPlan(nullptr)`，= 缺省档）时投递层只有一次判空：计数不动，读点行为不变。
//   ④ 三条指令（`await script` / `wait` / `click`）的语义（`REQ-01M7HBG0NYH957M9EK3JVMF5CC`）：
//      · `await script` 是**电平**门；名字大小写不敏感、两侧忽略 `.bin`；身份读不到 ⇒ **继续等**；
//      · `wait` 的 deadline 用**注入时钟**判（不需要真睡）：未到期不施加、到期才施加；
//      · 未知指令 / 旧指令 / 缺参数 / 负毫秒 / `=left` ⇒ `loadText` 失败且点名 `origin:行:`；
//      · `click` 的坐标从 **1280×720 参考系**缩放到客户区，`mouseMove` / `mouseButton` /
//        抬起用**同一个**映射结果；抬起语义的 sampled / forced 两条腿都在 **click** 上验。
//   ⑤ 第四条指令 `await position`（**脚本内位置**的电平门，半开区间 `[FROM, TO)`）：
//      · 命中 / 边界（`pos == FROM` 命中、`pos == TO` 不命中）/ 区间外继续等；
//      · `base==0` / `pc < base` / `pc` 未提交 / `pc − base` 越上界 ⇒ 不满足、不崩、不施加；
//      · `FROM >= TO` / 缺参数 / 非 `0x` hex ⇒ 解析期失败；未知脚本名**不**在解析期校验。
//
// ⚠ 诚实边界：`ReadFile`（第 11 个投递点，只是**事件源**）**不在**本用例里 ——
//   它要一个有效文件句柄（`CreateFileA` + `Vfs` 的 game_dir），那属于 `tests/file_io.cpp` 的夹具面；
//   它由源码守卫 ② 的 `std::fread(` 谓词判"接线一致"。⛔ 不要为此在夹具里造第二份 VFS。
// ★ `caseRelation` 的逐字原文是 `tools/test/input-plan-shape.test.mjs` ③ 与
//   `tools/mutate-check.mjs` 的匹配锚 ⇒ ⛔ 不要改它的断言文本。
//
// ★ 引用单：`REQ-01M7G7EMX3JR272D8FVM7JFY9D`（父单 `REQ-01M7G7EJF062NKQ0D40RF3BZ4Z`）
//   + `REQ-01M7HBG0NYH957M9EK3JVMF5CC`（三条指令 + 第四条 `await position`）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "check.h"
#include "fixture.h"
#include "runtime/cursor_map.h"  // referenceToClientPixel（判据 6 的纯函数）
#include "runtime/host_window.h"
#include "runtime/input.h"  // kMk* / WM_* 的闭集合
#include "runtime/input_plan.h"
#include "runtime/memory.h"
#include "shims/register.h"

namespace {

constexpr std::uint32_t kWmKeyDown = 0x0100u;
constexpr std::uint32_t kWmChar = 0x0102u;
constexpr std::uint32_t kQsPostMessage = 0x0008u;

// 鼠标消息（只用于本文件的坐标 / 抬起断言；值来自 `runtime/input.h` 的闭集合）。
constexpr std::uint32_t kWmMouseMove = 0x0200u;
constexpr std::uint32_t kWmLButtonDown = 0x0201u;
constexpr std::uint32_t kWmRButtonDown = 0x0204u;
constexpr std::uint32_t kWmRButtonUp = 0x0205u;

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
// 三条指令 + 第四条 `await position` · 夹具（假 Engine / 注入时钟 / 消息解码）
//
// ★ 假 Engine 的形状与 `tests/input_marker.cpp` 同源：只摆 `MarkerSampler` 需要的那几格
//   （单例 / `cur` / 第 0 条帧记录 / 名字表根）。⛔ 不依赖游戏安装 / LFS。
// ★ 时钟注入（`setClockForTest`）：`wait` 的判据**不需要真睡**（判据 3）——
//   测试推进 `g_now_ms` 再调一次 `atPoint` 就是"下一个读点"。
// =====================================================================================
constexpr std::uint32_t kEngineBase = 0x00480000u;
constexpr std::uint32_t kEngineSize = 0x00100000u;
constexpr std::uint32_t kEngineGlobal = 0x0055E1BCu;
constexpr std::uint32_t kNameBase = 0x004A0000u;
constexpr std::uint32_t kCodeBase = 0x004C0000u;
constexpr std::uint32_t kFrameBase = kEngineBase + lift::kEngineFrameBase;
constexpr std::uint32_t kCurEa = kEngineBase + lift::kEngineCurOffset;
constexpr std::uint32_t kFileDbEa = kEngineBase + lift::kFileIndexOffset;

std::uint64_t g_now_ms = 0;
std::uint64_t testNowMs() { return g_now_ms; }

void put32(lift::Machine& m, std::uint32_t ea, std::uint32_t v) {
    LIFT_CHECK(m.mem().committed(ea, 4u));
    m.mem().wr32(ea, v);
}

// 摆一条"身份 = id、名字 = name"的假 Engine（名字按 80 字节条目、NUL 结尾）。
bool publishEngine(lift::Machine& m, const char* name, std::uint32_t id) {
    if (!m.mem().commitZeroed(kEngineBase, kEngineSize, lift::RegionKind::Heap, "fake-engine")) {
        std::fprintf(stderr, "commitZeroed 失败：%s\n", m.mem().lastError());
        return false;
    }
    put32(m, kEngineGlobal, kEngineBase);
    put32(m, kCurEa, 0u);
    put32(m, kFrameBase, kCodeBase);  // 记录 +0x00 = 已提交的代码区基址
    put32(m, kFrameBase + lift::kFrameIdentityOffset, id);
    put32(m, kFileDbEa + lift::kFileRootCountOffset, id + 1u);
    put32(m, kFileDbEa + lift::kFileRootNameBaseOffset, kNameBase);
    const std::uint32_t entry = kNameBase + lift::kFileNameStride * id;
    const std::size_t n = std::strlen(name);
    for (std::size_t i = 0; i < n; ++i) m.mem().wr8(entry + static_cast<std::uint32_t>(i),
                                                    static_cast<std::uint8_t>(name[i]));
    m.mem().wr8(entry + static_cast<std::uint32_t>(n), 0u);
    return true;
}

// 把一条消息的 lParam 解回客户区 (x, y)（Win32：低 16 位 x、高 16 位 y，按有符号短整型）。
void decodeMouseLParam(std::uint32_t lparam, int* x, int* y) {
    *x = static_cast<std::int16_t>(lparam & 0xFFFFu);
    *y = static_cast<std::int16_t>((lparam >> 16) & 0xFFFFu);
}

// 在队列里找**指定消息**（从 `from` 起；返回 false = 没找到）。
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

// =====================================================================================
// 判据 3 + 5（sampled 腿）· `await script` 电平门 + `wait` 注入时钟 + click 的按下 / 抬起
//
// 判据（红了意味着什么）：
//   ① `await script` 在身份 == 名字时**同一次读点**通过；`wait 500` 的 deadline **未到期不施加**、
//      到期**同一次读点**施加 click（不需要真睡）；
//   ② click 按下后同一次读点就能读到位图；guest 采样前进过一拍后**自动抬起**；
//   ③ `fired` 只数 click（`await` / `wait` 不算动作）⇒ 摘要 `fired=1` 的口径。
// =====================================================================================
void caseInstructionsAndClock() {
    lift_test::g_current_case = "input-plan/gates-and-releases";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::WindowManager* wm = f.m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return;
    const std::uint32_t hwnd = wm->createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
    LIFT_CHECK(hwnd != 0);
    LIFT_CHECK(publishEngine(f.m, "TITLE.BIN", 42u));

    const char* const kPlan =
        "# 三条指令：电平门 + 宿主等待 + 点击\n"
        "await script title\n"
        "wait 500\n"
        "click 100 200\n";
    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    plan.attachGuestMemory(&f.m.mem());
    std::string plan_err;
    const bool loaded = plan.loadText(kPlan, "mem", &plan_err);
    if (!loaded) std::fprintf(stderr, "loadText err: %s\n", plan_err.c_str());
    LIFT_CHECK(loaded);
    if (!loaded) return;
    LIFT_CHECK(plan.armed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.steps()), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);

    wm->setInputPlan(&plan);

    // ---- ① 第 1 个读点：身份就是 TITLE（got 带 `.BIN`，want 没有且小写）⇒ 门通过；wait 起算 ----
    g_now_ms = 1000;
    plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);

    // ---- ② 未到期（差 1 ms）⇒ 不施加 ----
    g_now_ms = 1499;
    plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);

    // ---- ③ 到期 ⇒ 同一次读点里 wait 被消费、click 被施加 ----
    g_now_ms = 1500;
    plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 1);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), lift::input::kMkLButton);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);

    // ---- ④ sampled 腿：guest 采样前进过一拍 ⇒ 自动抬起（click 上验，⛔ 不再靠 key）----
    wm->noteInputSample(0);
    plan.atPoint(*wm, lift::InputPoint::CursorPos);  // 计数刚 +1（还没"观察过按下之后再一拍"）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
    wm->noteInputSample(0);
    plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
    LIFT_CHECK(plan.releases() >= 1);
}

// =====================================================================================
// 判据 2（负腿）· `await script` 是电平门，且身份读不到 ⇒ **继续等**（⛔ 不报错、⛔ 不瞎点）
// =====================================================================================
void caseScriptGate() {
    lift_test::g_current_case = "input-plan/script-gate";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::WindowManager* wm = f.m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return;
    LIFT_CHECK(wm->createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u) != 0);

    // ---- ⓪ 名字匹配的**唯一一份**：大小写 + 两侧 `.BIN`（旧实现只剥 got 一侧 ⇒ 第一条会红）----
    lift::MarkerSample s;
    s.valid = true;
    s.id = 42u;
    s.name = "TITLE";
    LIFT_CHECK(lift::scriptNameMatches("TITLE.BIN", s));
    s.name = "TITLE.BIN";
    LIFT_CHECK(lift::scriptNameMatches("title", s));
    s.name = "OTHER.BIN";
    LIFT_CHECK(!lift::scriptNameMatches("TITLE", s));
    s.name = "id:0x0000002A";
    LIFT_CHECK(lift::scriptNameMatches("id:0x0000002a", s));

    // ---- ① 身份是别的脚本 ⇒ 门**一直不成立**，点了就是错 ----
    LIFT_CHECK(publishEngine(f.m, "TITLE.BIN", 42u));
    lift::InputPlan other;
    other.attachGuestMemory(&f.m.mem());
    std::string err;
    LIFT_CHECK(other.loadText("await script NOSUCHSCRIPT\nclick 10 20\n", "mem", &err));
    for (int i = 0; i < 5; ++i) other.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(other.fired()), 0);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);

    // ---- ② 身份读不到（base==0）⇒ 也是"继续等"：⛔ 不报错、⛔ 不施加 ----
    put32(f.m, kEngineGlobal, 0u);
    lift::InputPlan waiting;
    waiting.attachGuestMemory(&f.m.mem());
    LIFT_CHECK(waiting.loadText("await script TITLE\nclick 10 20\n", "mem", &err));
    for (int i = 0; i < 5; ++i) waiting.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(waiting.fired()), 0);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);

    // ---- ③ 身份恢复 ⇒ 门成立、click 施加 ----
    put32(f.m, kEngineGlobal, kEngineBase);
    waiting.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(waiting.fired()), 1);
    LIFT_CHECK(wm->mouseButtons() != 0u);

    // ---- ④ 没有挂内存来源 ⇒ 门也永远不成立（⛔ 不假装通过）----
    lift::InputPlan detached;
    LIFT_CHECK(detached.loadText("await script TITLE\nclick 10 20\n", "mem", &err));
    for (int i = 0; i < 5; ++i) detached.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(detached.fired()), 0);
}

// =====================================================================================
// 判据 5 · 第四条指令 `await position`：脚本内位置的电平门（半开区间）
//
// 夹具：`publishEngine` 摆好身份；`frame[cur].+0x00` 指向一块已提交的"脚本体"，
// `+0x04` 由每个用例设成"label 坐标 → PC"的逆映射（`pc = base + (pos − header_len)`）。
// 判据（红了意味着什么）：
//   ① 区间内命中 ⇒ 同一个读点施加 click；区间外 ⇒ 继续等（⛔ 不报错、不施加）；
//   ② 边界：`pos == FROM` **命中**、`pos == TO` **不命中**（半开区间）；
//   ③ 读不到位置（base==0 / pc < base / pc 未提交 / pc−base 越上界）⇒ 不满足、不崩；
//   ④ 位置门**同时**要求 script 匹配（脚本名不符 ⇒ 即使 pos 在区间内也不通过）。
// =====================================================================================
void casePositionGate() {
    lift_test::g_current_case = "input-plan/position-gate";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::WindowManager* wm = f.m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return;
    LIFT_CHECK(wm->createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u) != 0);
    LIFT_CHECK(publishEngine(f.m, "TITLE.BIN", 42u));

    // `publishEngine` 已把 `frame[cur].+0x00` 预置成 kCodeBase（已提交）⇒ 就拿它当脚本体。
    const std::uint32_t body = kCodeBase;
    auto setPc = [&](std::uint32_t pos) {
        put32(f.m, kFrameBase + lift::kFramePcOffset, body + (pos - lift::kScriptHeaderLen));
    };

    // ---- ① + ②：命中 / 左边界（FROM 含）/ 区间外继续等 ----
    lift::InputPlan plan;
    std::string err;
    const bool loaded = plan.loadText("await position title 0x039C 0x0460\nclick 10 20\n", "mem", &err);
    if (!loaded) std::fprintf(stderr, "位置配方被拒：%s\n", err.c_str());
    LIFT_CHECK(loaded);
    if (!loaded) return;
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.steps()), 2);
    plan.attachGuestMemory(&f.m.mem());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);

    setPc(0x0398);  // 区间**左外**（S0 段的尾巴）
    plan.atPoint(*wm, lift::InputPoint::PeekMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 0);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);

    setPc(0x039C);  // **pos == FROM** ⇒ 命中
    plan.atPoint(*wm, lift::InputPoint::PeekMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 2);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), lift::input::kMkLButton);
    (void)plan.releaseAll(*wm);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);

    // ---- ②：右边界（TO 不含）——`pos == TO` 不通过、`pos == 末指令起点` 通过 ----
    lift::InputPlan to_plan;
    LIFT_CHECK(to_plan.loadText("await position TITLE 0x039C 0x0460\nclick 10 20\n", "mem", &err));
    to_plan.attachGuestMemory(&f.m.mem());
    setPc(0x0460);  // **pos == TO** ⇒ 不通过（半开区间）
    to_plan.atPoint(*wm, lift::InputPoint::PeekMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(to_plan.fired()), 0);
    setPc(0x045C);  // 末指令（jmp）的起点 ⇒ 通过
    to_plan.atPoint(*wm, lift::InputPoint::PeekMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(to_plan.fired()), 1);
    (void)to_plan.releaseAll(*wm);

    // ---- ①：位置门**同时**要求 script 匹配（pos 在区间内、名字不符 ⇒ 不通过）----
    lift::InputPlan other;
    LIFT_CHECK(other.loadText("await position NOSUCHSCRIPT 0x039C 0x0460\nclick 10 20\n", "mem", &err));
    other.attachGuestMemory(&f.m.mem());
    setPc(0x039C);
    for (int i = 0; i < 3; ++i) other.atPoint(*wm, lift::InputPoint::PeekMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(other.fired()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(other.stepsApplied()), 0);

    // ---- ③ 读不到位置 ⇒ 不满足、不崩、不施加（每种都记一次内部计数）----
    lift::InputPlan robust;
    LIFT_CHECK(robust.loadText("await position TITLE 0x039C 0x0460\nclick 10 20\n", "mem", &err));
    robust.attachGuestMemory(&f.m.mem());
    const std::uint64_t misses0 = robust.positionMisses();

    // (a) 引擎没发布帧记录基址（`base==0`）
    put32(f.m, kFrameBase + lift::kFrameBodyBaseOffset, 0u);
    robust.atPoint(*wm, lift::InputPoint::PeekMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(robust.fired()), 0);
    put32(f.m, kFrameBase + lift::kFrameBodyBaseOffset, body);

    // (b) `pc < base`（不是脚本体里的指针）
    put32(f.m, kFrameBase + lift::kFramePcOffset, body - 4u);
    robust.atPoint(*wm, lift::InputPoint::PeekMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(robust.fired()), 0);

    // (c) `pc` 未提交（读到空页）
    put32(f.m, kFrameBase + lift::kFramePcOffset, 0x10000000u);
    robust.atPoint(*wm, lift::InputPoint::PeekMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(robust.fired()), 0);

    // (d) `pc − base` 超过语料上界（pc 仍在同一块已提交区域里 ⇒ 只有上界这一条能拦住）
    constexpr std::uint32_t kBigBody = 0x30000000u;
    constexpr std::uint32_t kBigSize = 0x02000000u;  // 32 MiB > kScriptBodyMaxBytes
    LIFT_CHECK(f.m.mem().commitZeroed(kBigBody, kBigSize, lift::RegionKind::Heap, "fake-body"));
    put32(f.m, kFrameBase + lift::kFrameBodyBaseOffset, kBigBody);
    put32(f.m, kFrameBase + lift::kFramePcOffset, kBigBody + lift::kScriptBodyMaxBytes + 4u);
    robust.atPoint(*wm, lift::InputPoint::PeekMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(robust.fired()), 0);
    LIFT_CHECK(robust.positionMisses() >= misses0 + 4);

    // 恢复到合法位置 ⇒ 同一个计划仍然能通过（门是电平，不是一次性的）
    put32(f.m, kFrameBase + lift::kFrameBodyBaseOffset, body);
    setPc(0x0400);
    robust.atPoint(*wm, lift::InputPoint::PeekMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(robust.fired()), 1);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), lift::input::kMkLButton);
    (void)robust.releaseAll(*wm);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
}

// =====================================================================================
// 判据 1 + 4 · 解析期硬错：旧指令 / 未知指令 / 缺参数 / 负毫秒 / `=left` ⇒ 带 `origin:行:`
// =====================================================================================
void caseParseErrors() {
    lift_test::g_current_case = "input-plan/parse-errors";
    struct BadPlan {
        const char* text;
        const char* want_line;  // `mem:<行>:` —— 判据是"点出**那一行**"
        const char* want_word;  // 报错里必须出现的词（点名失效原因）
    };
    const BadPlan kBads[] = {
        {"await presents 120\n", "mem:1:", "未知指令"},
        {"await always\n", "mem:1:", "未知指令"},
        {"key 13\n", "mem:1:", "未知指令"},
        {"node title\n", "mem:1:", "未知指令"},
        {"when script TITLE\n", "mem:1:", "未知指令"},
        {"wait 500 wall_ms\n", "mem:1:", "wait"},
        {"wait -5\n", "mem:1:", "正整数"},
        {"wait 0\n", "mem:1:", "正整数"},
        {"wait\n", "mem:1:", "wait"},
        // `await position` 的解析期四条：区间倒置 / 缺参数 / 非 hex / 裸 hex。
        {"await position TITLE 0x0460 0x039C\n", "mem:1:", "FROM < TO"},
        {"await position TITLE 0x039C\n", "mem:1:", "await position"},
        {"await position TITLE 0x039C 0x46G\n", "mem:1:", "hex"},
        {"await position TITLE 39C 460\n", "mem:1:", "hex"},
        {"await position\n", "mem:1:", "await position"},
        {"click 1\n", "mem:1:", "缺参数"},
        {"click 1 2 =left\n", "mem:1:", "按钮词"},
        {"# 注释\nawait script TITLE\nawait presents 2\n", "mem:3:", "未知指令"},
        {"\n# 只有注释\n", "mem:", "没有任何指令"},
    };
    for (const BadPlan& bad : kBads) {
        lift::InputPlan plan;
        std::string err;
        const bool ok = plan.loadText(bad.text, "mem", &err);
        LIFT_CHECK(!ok);
        LIFT_CHECK(!plan.armed());
        const bool has_line = err.find(bad.want_line) != std::string::npos;
        const bool has_word = err.find(bad.want_word) != std::string::npos;
        if (!has_line || !has_word) {
            std::fprintf(stderr, "坏配方没按口径报错（want %s / %s，got `%s`）\n", bad.want_line, bad.want_word,
                         err.c_str());
        }
        LIFT_CHECK(has_line);
        LIFT_CHECK(has_word);
        // 被拒的那个词要**回显**（否则人只看到"按钮词不对"但不知道是哪个词 —— `=left` 曾报成空串）。
        if (std::string(bad.text).find("=left") != std::string::npos) {
            LIFT_CHECK(err.find("=left") != std::string::npos);
        }
    }

    // ---- 合法配方：四条指令都吃，`#` / 空行 / 行尾注释 / 大小写指令名都忽略 ----
    lift::InputPlan plan;
    std::string err;
    const bool ok = plan.loadText(
        "\n# 注释\nAwait script TITLE  # 行尾注释\n\nWait 250\nclick 1 2 right\n"
        "await POSITION some-name 0x0000039C 0x00000460  # 未知脚本名不在解析期校验\n",
        "mem", &err);
    if (!ok) std::fprintf(stderr, "合法配方被拒：%s\n", err.c_str());
    LIFT_CHECK(ok);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.steps()), 4);
}

// =====================================================================================
// 判据 6 · 坐标缩放：1280×720 参考系 → 客户区；move / down / up 用**同一个**结果
// =====================================================================================
void caseCoordinateScale() {
    lift_test::g_current_case = "input-plan/coordinate-scale";
    // ---- ⓪ 纯函数（两组以上尺寸；一组非 1:1；0 ⇒ 退回参考系；⛔ 不 clamp）----
    int x = 0;
    int y = 0;
    LIFT_CHECK(lift::referenceToClientPixel(1280, 720, 1070, 480, &x, &y));
    LIFT_CHECK_EQ_I64(x, 1070);
    LIFT_CHECK_EQ_I64(y, 480);
    LIFT_CHECK(lift::referenceToClientPixel(1920, 1080, 1070, 480, &x, &y));
    LIFT_CHECK_EQ_I64(x, 1605);  // 1070 * 1920 / 1280
    LIFT_CHECK_EQ_I64(y, 720);   // 480 * 1080 / 720
    LIFT_CHECK(lift::referenceToClientPixel(1600, 900, 640, 360, &x, &y));
    LIFT_CHECK_EQ_I64(x, 800);
    LIFT_CHECK_EQ_I64(y, 450);
    LIFT_CHECK(lift::referenceToClientPixel(0, 0, 1070, 480, &x, &y));  // 没有窗口 ⇒ 恒等
    LIFT_CHECK_EQ_I64(x, 1070);
    LIFT_CHECK_EQ_I64(y, 480);
    LIFT_CHECK(lift::referenceToClientPixel(1920, 1080, -10, 730, &x, &y));  // 越界原样缩放（⛔ 不 clamp）
    LIFT_CHECK_EQ_I64(x, -15);
    LIFT_CHECK_EQ_I64(y, 1095);

    // ---- ① 端到端：非 1:1 客户区，click 的 move / down / up 必须逐位相同 ----
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::WindowManager* wm = f.m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return;
    const std::uint32_t hwnd = wm->createWindow("PlanTest", 0u, 0u, 0u, 0u, 1920, 1080, 0u);
    LIFT_CHECK(hwnd != 0);
    LIFT_CHECK(publishEngine(f.m, "TITLE.BIN", 42u));

    lift::InputPlan plan;
    std::string err;
    LIFT_CHECK(plan.loadText("click 1070 480 right\n", "mem", &err));
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);
    plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 1);

    const std::size_t q0 = 0;
    lift::WinMessage move_msg;
    lift::WinMessage down_msg;
    LIFT_CHECK(findMessage(*wm, q0, kWmMouseMove, &move_msg));
    LIFT_CHECK(findMessage(*wm, q0, kWmRButtonDown, &down_msg));
    int move_x = 0;
    int move_y = 0;
    int down_x = 0;
    int down_y = 0;
    decodeMouseLParam(move_msg.lparam, &move_x, &move_y);
    decodeMouseLParam(down_msg.lparam, &down_x, &down_y);
    LIFT_CHECK_EQ_I64(move_x, 1605);
    LIFT_CHECK_EQ_I64(move_y, 720);
    LIFT_CHECK_EQ_I64(down_x, 1605);  // ★ 与 move **同一个**映射结果
    LIFT_CHECK_EQ_I64(down_y, 720);
    int cur_x = 0;
    int cur_y = 0;
    wm->cursorClientPos(hwnd, &cur_x, &cur_y);
    LIFT_CHECK_EQ_I64(cur_x, 1605);
    LIFT_CHECK_EQ_I64(cur_y, 720);

    // 抬起：用按下时存下的同一对（`PendingRelease`），消息 lParam 也是它。
    const std::size_t before_up = wm->queueSize();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.releaseAll(*wm)), 1);
    lift::WinMessage up_msg;
    LIFT_CHECK(findMessage(*wm, before_up, kWmRButtonUp, &up_msg));
    int up_x = 0;
    int up_y = 0;
    decodeMouseLParam(up_msg.lparam, &up_x, &up_y);
    LIFT_CHECK_EQ_I64(up_x, 1605);
    LIFT_CHECK_EQ_I64(up_y, 720);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);

    // ---- ② 1:1 客户区：默认按钮 = left，坐标原样 ----
    LIFT_CHECK(wm->destroyWindow(hwnd));
    const std::uint32_t hwnd2 = wm->createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
    LIFT_CHECK(hwnd2 != 0);
    lift::InputPlan plan2;
    LIFT_CHECK(plan2.loadText("click 1070 480\n", "mem", &err));
    const std::size_t q1 = wm->queueSize();
    plan2.atPoint(*wm, lift::InputPoint::CursorPos);
    lift::WinMessage d2;
    LIFT_CHECK(findMessage(*wm, q1, kWmLButtonDown, &d2));  // 省略按钮词 = left
    decodeMouseLParam(d2.lparam, &down_x, &down_y);
    LIFT_CHECK_EQ_I64(down_x, 1070);
    LIFT_CHECK_EQ_I64(down_y, 480);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), lift::input::kMkLButton);
}

// =====================================================================================
// 判据 5（forced 腿）· 连续读点兜底抬起（click 上验：队列类读点不采样也不会永远按着）
// =====================================================================================
void caseForcedRelease() {
    lift_test::g_current_case = "input-plan/forced-release";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::WindowManager* wm = f.m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return;
    LIFT_CHECK(wm->createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u) != 0);

    lift::InputPlan plan;
    std::string err;
    LIFT_CHECK(plan.loadText("click 10 20\n", "mem", &err));
    plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 1);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), lift::input::kMkLButton);
    // ⛔ 不推进采样计数：只靠"连续 kForceReleaseReads 个读点"这一条兜底。
    for (int i = 0; i < 20 && plan.pendingReleases() > 0; ++i) plan.atPoint(*wm, lift::InputPoint::GetMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
    LIFT_CHECK(plan.releases() >= 1);
}

}  // namespace

int main() {
    caseRelation();
    caseInstructionsAndClock();
    caseScriptGate();
    casePositionGate();
    caseParseErrors();
    caseCoordinateScale();
    caseForcedRelease();
    return lift_test::finish("input-plan-read-points");
}
