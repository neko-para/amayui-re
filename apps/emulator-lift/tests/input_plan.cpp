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
//        抬起用**同一个**映射结果；★ 注入形状（2026-11-19）= move（臂上，**只**写光标）→
//        跨一个**已完成**的位置读点 ⇒ down → 按下之后**至少 2 个 KeyLevel 读点** ⇒ up
//        （闸门常量见 `input_plan.h`；两条腿 = `keylevel` / `wallclock`）。
//   ⑤ 第四条指令 `await position`（**脚本内位置**的电平门，半开区间 `[FROM, TO)`）：
//      · 命中 / 边界（`pos == FROM` 命中、`pos == TO` 不命中）/ 区间外继续等；
//      · `base==0` / `pc < base` / `pc` 未提交 / `pc − base` 越上界 ⇒ 不满足、不崩、不施加；
//      · `FROM >= TO` / 缺参数 / 非 `0x` hex ⇒ 解析期失败；未知脚本名**不**在解析期校验。
//   ⑥ **能力面只有一份**（`REQ-01M7HP2Q1N0N004M8R4RPABWSV` 的前置）：行尾 `timeout` / `profile_begin` /
//      `profile_end` / `quit` 在**本臂**同样被接受并落地（⛔ 不再是"这个入口不认"的 exit 2）：
//      · `timeout <ms>` **只能写在 `await …` 那一行的末尾**（守的就是同一行那个门）；到期 ⇒
//        `outcome()==TimedOut` + 失败报告点名 `origin:行` + 门/动作；未到期（差 1 ms）**不**判失败；
//      · `wait` **没有守卫**（固定时长、到点就走）：显式给它 `timeout` = 解析期硬错；
//      · `quit` ⇒ `postQuit(0)`（WM_QUIT）+ 不再推进；`quit` 之后还有指令 ⇒ `pending != 0`；
//      · `profile_begin/end` ⇒ 区间收口（`profileWindows()`）。
//   ⑦ **完成度是判据**：门永不成立 ⇒ `outcome()==Running` + `pending != 0` +
//      `recipeOutcomeExitCode()` = **5**（⛔ 不是 0）；全部走完 ⇒ `Done` + 退出码 0。
//   ⑧ **跨行 `timeout` / `wait` 带 `timeout` / `click` 带 `timeout` ⇒ 解析期硬错**（文案点名修法）；
//      **超长 `wait`（例 20000 ms）不再被判 timeout**（删掉 `ms + 缺省` 那条补丁的证明）。
//   ⑨ **S9**：注入点击过叠层的**同一个** `UiClickRouter`（D6）—— 命中 ⇒ `fired` +1、`uiConsumed` +1、
//      guest 消息为零 / 光标不动 / down 吞 ⇒ up 吞；未命中 ⇒ 与**没有 router**逐位相同。
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
#include <vector>

#include "check.h"
#include "fixture.h"
#include "runtime/cursor_map.h"  // referenceToClientPixel（判据 6 的纯函数）
#include "runtime/host_window.h"
#include "runtime/input.h"  // kMk* / WM_* 的闭集合
#include "runtime/input_plan.h"
#include "runtime/interaction_graph.h"  // S4/S6：`issueAction` 的 mouse 分支（动作文本可观察面）
#include "runtime/memory.h"
#include "runtime/ui_input.h"  // S9：注入 click 的叠层路由（UiInputRouter）
#include "shims/register.h"

namespace {

constexpr std::uint32_t kWmKeyDown = 0x0100u;
constexpr std::uint32_t kWmChar = 0x0102u;
constexpr std::uint32_t kQsPostMessage = 0x0008u;
constexpr std::uint32_t kWmQuit = 0x0012u;  // WM_QUIT（`quit` 的交付物；队尾虚拟消息）

// 鼠标消息（只用于本文件的坐标 / 抬起断言；值来自 `runtime/input.h` 的闭集合）。
constexpr std::uint32_t kWmMouseMove = 0x0200u;
constexpr std::uint32_t kWmLButtonDown = 0x0201u;
constexpr std::uint32_t kWmLButtonUp = 0x0202u;
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

// 从 `from` 起数指定消息（⛔ 不取走）。
std::size_t countMessage(lift::WindowManager& wm, std::size_t from, std::uint32_t message) {
    std::size_t n = 0;
    for (std::size_t i = from; i < wm.queueSize(); ++i) {
        lift::WinMessage m;
        if (wm.peekAt(i, &m) && m.message == message) ++n;
    }
    return n;
}

// 队列的逐条指纹（`message/wParam/lParam` 三元组）—— **重复跑**判据用它（⛔ 不比"画面动了"）。
std::string dumpQueue(lift::WindowManager& wm) {
    std::string out;
    char buf[64];
    for (std::size_t i = 0; i < wm.queueSize(); ++i) {
        lift::WinMessage m;
        if (!wm.peekAt(i, &m)) continue;
        std::snprintf(buf, sizeof(buf), "%04X/%04X/%08X;", static_cast<unsigned>(m.message),
                      static_cast<unsigned>(m.wparam), static_cast<unsigned>(m.lparam));
        out += buf;
    }
    return out;
}

// 只取**按钮消息**的顺序（`D` = down / `U` = up）—— 连点串行化的判据是"DOWN,UP,DOWN,UP"。
std::string downUpSequence(lift::WindowManager& wm) {
    std::string out;
    for (std::size_t i = 0; i < wm.queueSize(); ++i) {
        lift::WinMessage m;
        if (!wm.peekAt(i, &m)) continue;
        if (m.message == kWmLButtonDown) {
            out += 'D';
        } else if (m.message == kWmLButtonUp) {
            out += 'U';
        }
    }
    return out;
}

// WM_* 的 lParam 编码（低 16 位 x、高 16 位 y；与 `runtime/input.cpp` 的 `mouseLParam` 同形）。
std::uint32_t mouseLParam(int x, int y) {
    return (static_cast<std::uint32_t>(static_cast<std::uint16_t>(y)) << 16) |
           static_cast<std::uint32_t>(static_cast<std::uint16_t>(x));
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

    // ---- ③ 到期 ⇒ 同一次读点里 wait 被消费、click **臂上**（只 move、⛔ 还没按下）----
    g_now_ms = 1500;
    plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 1);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);  // ★ 2026-11-19：move 与 down 不再同一读点
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    int cur_x = 0;
    int cur_y = 0;
    wm->cursorClientPos(hwnd, &cur_x, &cur_y);
    LIFT_CHECK_EQ_I64(cur_x, 100);  // 虚拟光标**立刻**到位（`GetCursorPos` 读得到）
    LIFT_CHECK_EQ_I64(cur_y, 200);

    // ---- ④ 跨过一个**已完成的位置读点** ⇒ 按下（阶段 ②）----
    g_now_ms = 1501;
    plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), lift::input::kMkLButton);

    // ---- ⑤ ★ 位置读点**不能**满足抬起闸门（旧实现把 `GetCursorPos` 也算样本 ⇒ 就在这里抬起）----
    for (int i = 0; i < 4; ++i) {
        wm->noteInputSample(0);  // 真 shim 在 `GetCursorPos` 里 +1：总样本数继续涨
        g_now_ms += 1;
        plan.atPoint(*wm, lift::InputPoint::CursorPos);
    }
    LIFT_CHECK(wm->inputSampleCount() >= 4);  // 位置采样确实在前进
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);  // ⛔ 但**还没**抬起
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), lift::input::kMkLButton);

    // ---- ⑥ 一个 KeyLevel 读点还不够：N=2（抬起落在读之前 ⇒ N=1 时 guest 一次也看不到按下）----
    g_now_ms += 1;
    plan.atPoint(*wm, lift::InputPoint::KeyLevel);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);

    // ---- ⑦ 第二个 KeyLevel 读点 ⇒ 按下真的被读过 ⇒ 抬起 ----
    g_now_ms += 1;
    plan.atPoint(*wm, lift::InputPoint::KeyLevel);
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

    // ---- ③ 身份恢复 ⇒ 门成立、click **落地**（阶段 ①：光标到位；按下要等一个位置读点）----
    put32(f.m, kEngineGlobal, kEngineBase);
    waiting.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(waiting.fired()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(waiting.armedPresses()), 1);

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
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 1);  // 阶段 ①：只 move
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
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
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(robust.armedPresses()), 1);  // 阶段 ①：只 move
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
        // ★ S6：三条鼠标原语的负例（⛔ 不放松成"静默不认"、⛔ 不用 (0,0) 猜坐标）：
        //   `mouse_move` 缺参 / 非整数 / 参数过多；`mouse_down|up` 之前在**文件里**没有 `mouse_move`
        //   ⇒ 点名"需要先 mouse_move"；按钮词只认裸写 left|right；它们都没有可等待的东西。
        {"mouse_move 1\n", "mem:1:", "缺参数"},
        {"mouse_move\n", "mem:1:", "缺参数"},
        {"mouse_move 1 2 3\n", "mem:1:", "缺参数"},
        {"mouse_move a b\n", "mem:1:", "整数"},
        {"mouse_down\n", "mem:1:", "mouse_move"},
        {"mouse_up\n", "mem:1:", "mouse_move"},
        {"mouse_down left\n", "mem:1:", "mouse_move"},
        {"mouse_move 1 2\nmouse_down middle\n", "mem:2:", "按钮词"},
        {"mouse_move 1 2\nmouse_up left extra\n", "mem:2:", "最多"},
        {"mouse_move 1 2 timeout 5000\n", "mem:1:", "没有可等待的东西"},
        {"mouse_move 1 2\nmouse_down timeout 5000\n", "mem:2:", "没有可等待的东西"},
        {"await script TITLE\nmouse_up\n", "mem:2:", "mouse_move"},  // 门不算"更早的 mouse_move"
        // `timeout` / `profile_*` / `quit` 的**参数**仍然硬错（能力面统一了，⛔ 不是"什么都收"）：
        // ★ 行内 `timeout`：孤立成行 / 挂在 `wait` / `click` / `quit` / `profile_*` 上都是硬错，
        //   而且文案必须**指向修法**（不是只说"这里不对"）。
        {"await script TITLE\ntimeout 30000\n", "mem:2:", "不能单独一行"},
        {"click 1 2 timeout 5000\n", "mem:1:", "没有可等待的东西"},
        {"quit timeout 5000\n", "mem:1:", "没有可等待的东西"},
        {"profile_begin w timeout 5000\n", "mem:1:", "没有可等待的东西"},
        {"profile_end w timeout 5000\n", "mem:1:", "没有可等待的东西"},
        {"wait 5000 timeout 9000\n", "mem:1:", "写回 `wait 5000`"},
        // 缺参 / 非数字 / 0 / 不是行尾 ⇒ 硬错（点出行号）。
        {"await script TITLE timeout\n", "mem:1:", "毫秒数"},
        {"await script TITLE timeout abc\n", "mem:1:", "正整数"},
        {"await script TITLE timeout 0\n", "mem:1:", "正整数"},
        {"await position TITLE 0x039C 0x0460 timeout 2500 extra\n", "mem:1:", "行尾"},
        {"profile_begin\n", "mem:1:", "label"},
        {"profile_end a b\n", "mem:1:", "label"},
        {"quit now\n", "mem:1:", "不带参数"},
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

    // 跨行 `timeout` 的文案必须**指向修法**（"写到它守的那一行末尾"）—— 只说"这里不对"帮不了人。
    {
        lift::InputPlan cross;
        std::string cross_err;
        LIFT_CHECK(!cross.loadText("await script TITLE\ntimeout 30000\nclick 1 2\n", "mem", &cross_err));
        LIFT_CHECK(cross_err.find("mem:2:") != std::string::npos);
        LIFT_CHECK(cross_err.find("末尾") != std::string::npos);
    }

    // ---- 合法配方：**九条指令 + 行尾 `timeout`** 都吃（能力面只有一份），`#` / 空行 / 行尾注释 /
    //      大小写指令名都忽略 ----
    lift::InputPlan plan;
    std::string err;
    const bool ok = plan.loadText(
        "\n# 注释\nAwait script TITLE timeout 2500  # 行尾注释\n\nWait 250\nclick 1 2 right\n"
        "profile_begin w\nprofile_end w\n"
        "await POSITION some-name 0x0000039C 0x00000460 timeout 7000  # 未知脚本名不在解析期校验\n"
        "mouse_move 1070 480\nmouse_down right\nmouse_up\n"
        "quit\n",
        "mem", &err);
    if (!ok) std::fprintf(stderr, "合法配方被拒：%s\n", err.c_str());
    LIFT_CHECK(ok);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.steps()), 10);  // `timeout` 不是一条指令
    // 行尾 `timeout` 挂在**同一行那个门**上（两种门都验；`wait` / `click` 等不能带）。
    LIFT_CHECK(plan.stepAt(0) != nullptr && plan.stepAt(0)->timeout_ms == 2500u);
    LIFT_CHECK(plan.stepAt(5) != nullptr && plan.stepAt(5)->timeout_ms == 7000u);
    LIFT_CHECK(plan.stepAt(1) != nullptr && plan.stepAt(1)->timeout_ms == 0u);  // `wait` 没有守卫
    LIFT_CHECK(plan.stepAt(2) != nullptr && plan.stepAt(2)->timeout_ms == 0u);  // `click` 也没有
    // ★ S6：三条原语的 step 模型（`Step` 复用 x/y/button；down/up 的坐标**不**在解析期烤进来）。
    LIFT_CHECK(plan.stepAt(6) != nullptr && plan.stepAt(6)->kind == lift::StepKind::MouseMove);
    LIFT_CHECK(plan.stepAt(6) != nullptr && plan.stepAt(6)->x == 1070 && plan.stepAt(6)->y == 480);
    LIFT_CHECK(plan.stepAt(7) != nullptr && plan.stepAt(7)->kind == lift::StepKind::MouseDown);
    LIFT_CHECK(plan.stepAt(7) != nullptr && plan.stepAt(7)->button == lift::MouseButton::Right);
    LIFT_CHECK(plan.stepAt(8) != nullptr && plan.stepAt(8)->kind == lift::StepKind::MouseUp);
    LIFT_CHECK(plan.stepAt(8) != nullptr && plan.stepAt(8)->button == lift::MouseButton::Left);
    // 规范化写法（trace / 失败报告用）：词表与解析器同一份。
    LIFT_CHECK(lift::stepText(*plan.stepAt(6)) == "mouse_move 1070 480");
    LIFT_CHECK(lift::stepText(*plan.stepAt(7)) == "mouse_down right");
    LIFT_CHECK(lift::stepText(*plan.stepAt(8)) == "mouse_up");
    LIFT_CHECK(std::string(lift::stepKindName(lift::StepKind::MouseMove)) == "mouse_move");
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
    plan.atPoint(*wm, lift::InputPoint::CursorPos);  // 阶段 ①：臂上（只 move）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 1);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);  // ⛔ 同一次读点里**没有**按下
    plan.atPoint(*wm, lift::InputPoint::CursorPos);  // 阶段 ②：跨过一个完成的位置读点 ⇒ 按下
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);

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
    plan2.atPoint(*wm, lift::InputPoint::CursorPos);  // 臂上（只 move）
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
    plan2.atPoint(*wm, lift::InputPoint::CursorPos);  // 跨过一个完成的位置读点 ⇒ 按下
    lift::WinMessage d2;
    LIFT_CHECK(findMessage(*wm, q1, kWmLButtonDown, &d2));  // 省略按钮词 = left
    decodeMouseLParam(d2.lparam, &down_x, &down_y);
    LIFT_CHECK_EQ_I64(down_x, 1070);
    LIFT_CHECK_EQ_I64(down_y, 480);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), lift::input::kMkLButton);
}

// =====================================================================================
// 判据 5（墙钟兜底腿）· 没人读按键电平 ⇒ 按下之后 `kReleaseFallbackMs` 到期仍然抬起
//
// 判据（红了意味着什么）：
//   ① 这个屏**一次也不调** `GetAsyncKeyState`（注入了一个纯消息型屏）⇒ 按键闸门永远不成立；
//   ② 但按下**不能**因此永久挂着 ⇒ 宿主墙钟到期必须抬起（否则下一次 run 会看到"按钮一直按着"）；
//   ③ 到期之前**不许**提前抬起（提前抬起 = 按下从没被看见，正是 2026-11-19 的失效形状）。
// =====================================================================================
void caseForcedRelease() {
    lift_test::g_current_case = "input-plan/wallclock-release";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::WindowManager* wm = f.m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return;
    LIFT_CHECK(wm->createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u) != 0);

    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    std::string err;
    LIFT_CHECK(plan.loadText("click 10 20\n", "mem", &err));
    g_now_ms = 10000;
    plan.atPoint(*wm, lift::InputPoint::CursorPos);  // 臂上
    plan.atPoint(*wm, lift::InputPoint::CursorPos);  // 按下（跨过一个完成的位置读点）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 1);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), lift::input::kMkLButton);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);

    // ⛔ 没有一次 KeyLevel 读点、时钟只走了兜底窗口的 1/5 ⇒ **不**抬起。
    g_now_ms += lift::InputPlan::kReleaseFallbackMs / 5;
    for (int i = 0; i < 20; ++i) plan.atPoint(*wm, lift::InputPoint::GetMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), lift::input::kMkLButton);

    // 墙钟越过兜底窗口 ⇒ 下一个读点抬起（定数、非读点计数）。
    g_now_ms += lift::InputPlan::kReleaseFallbackMs;
    plan.atPoint(*wm, lift::InputPoint::GetMsg);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
    LIFT_CHECK(plan.releases() >= 1);
}

// =====================================================================================
// `--input-plan-trace`（opt-in；`REQ-01M7PG086DG4BBPA40VCVZTP4X` B）
//
// 判据（红了意味着什么）：
//   ① **开档有行**：每条 step 成立 / 施加时**恰好一行**，且行里有 `plan:<文件>:<行>` +
//      `kind=` + `script=` + `pos=` + `frame=`（缺一个 ⇒ 诊断"配方 click 不被消费"时又只能猜）；
//   ② **关档零行**（缺省档输出逐字节不变 —— 这是"opt-in"的唯一机械判据）；
//   ③ click 的**抬起**也有一行，且 `reason=keylevel|wallclock` 如实区分两条腿
//      （`wallclock` + `key_reads==press_key_reads` = "guest 一次也没读到按下电平"，这正是 D 的判据面）；
//   ④ `frame=` 的来源 = `presenting()`（`note(Frame)` 的累计数）⇒ 这里推 3 次断言 `frame=3`，
//      ⛔ 不许抄成一个自造的常数。
// =====================================================================================
std::string readSink(std::FILE* fp) {
    std::fflush(fp);
    std::rewind(fp);
    std::string out;
    char buf[512];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), fp)) > 0) out.append(buf, n);
    return out;
}

void caseTrace() {
    lift_test::g_current_case = "input-plan/trace";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::WindowManager* wm = f.m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return;
    LIFT_CHECK(wm->createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u) != 0);
    LIFT_CHECK(publishEngine(f.m, "TITLE.BIN", 42u));
    std::string err;

    // ---- ① 开档：两条 step 各一行（门 + click），行里有文件:行 / kind / script / pos / frame ----
    std::FILE* sink = std::tmpfile();
    LIFT_CHECK(sink != nullptr);
    if (sink == nullptr) return;
    {
        lift::InputPlan plan;
        plan.setClockForTest(&testNowMs);
        plan.attachGuestMemory(&f.m.mem());
        LIFT_CHECK(plan.loadText("await script title\nclick 10 20\n", "mem", &err));
        plan.setTrace(sink);
        wm->setInputPlan(&plan);
        plan.note(lift::PlanEventKind::Frame);  // Present 读数 ⇒ frame=3（判据 ④）
        plan.note(lift::PlanEventKind::Frame);
        plan.note(lift::PlanEventKind::Frame);
        plan.atPoint(*wm, lift::InputPoint::CursorPos);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 2);
        const std::string t = readSink(sink);
        LIFT_CHECK(t.find("plan:mem:1 applied kind=await-script") != std::string::npos);
        LIFT_CHECK(t.find("plan:mem:2 applied kind=click") != std::string::npos);
        LIFT_CHECK(t.find("script=TITLE.BIN") != std::string::npos);
        LIFT_CHECK(t.find("pos=") != std::string::npos);
        LIFT_CHECK(t.find("frame=3") != std::string::npos);
        LIFT_CHECK(t.find("state_seq=-") != std::string::npos);  // 没开控制层 ⇒ 不编造 0
        // ---- ③ keylevel 腿：跨一个位置读点按下，再喂 2 个 KeyLevel 读点 ⇒ 抬起那一行 ----
        g_now_ms = 20000;
        plan.atPoint(*wm, lift::InputPoint::CursorPos);  // 阶段 ②：按下
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
        plan.atPoint(*wm, lift::InputPoint::KeyLevel);  // 1/2
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
        plan.atPoint(*wm, lift::InputPoint::KeyLevel);  // 2/2 ⇒ 抬起
        const std::string t2 = readSink(sink);
        LIFT_CHECK(t2.find("release kind=click") != std::string::npos);
        LIFT_CHECK(t2.find("reason=keylevel") != std::string::npos);
        // 抬起行必须带**闸门**的读数（key_reads / press_key_reads）——"按键被看见没有"的唯一读数面
        LIFT_CHECK(t2.find("key_reads=") != std::string::npos);
        LIFT_CHECK(t2.find("press_key_reads=") != std::string::npos);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    }
    // ---- ② 关档（缺省）：同一份配方跑完 ⇒ sink 里**零字节** ----
    {
        std::FILE* quiet_sink = std::tmpfile();
        LIFT_CHECK(quiet_sink != nullptr);
        if (quiet_sink == nullptr) return;
        lift::InputPlan quiet;
        quiet.setClockForTest(&testNowMs);
        quiet.attachGuestMemory(&f.m.mem());
        LIFT_CHECK(quiet.loadText("await script title\nclick 30 40\n", "mem", &err));
        // ⛔ 不调 setTrace：缺省档
        for (int i = 0; i < 20; ++i) quiet.atPoint(*wm, lift::InputPoint::CursorPos);
        const std::string q = readSink(quiet_sink);
        LIFT_CHECK(q.empty());
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(quiet.fired()), 1);  // 配方本身照旧施加
        (void)quiet.releaseAll(*wm);
        std::fclose(quiet_sink);
    }
    // ---- ③ 墙钟兜底腿：不推进 KeyLevel 读点 ⇒ 兜底抬起那一行 `reason=wallclock` ----
    {
        std::FILE* fsink = std::tmpfile();
        LIFT_CHECK(fsink != nullptr);
        if (fsink == nullptr) return;
        lift::InputPlan forced;
        forced.setClockForTest(&testNowMs);
        LIFT_CHECK(forced.loadText("click 5 6\n", "mem", &err));
        forced.setTrace(fsink);
        g_now_ms = 30000;
        forced.atPoint(*wm, lift::InputPoint::CursorPos);  // 阶段 ①：臂上
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(forced.armedPresses()), 1);
        forced.atPoint(*wm, lift::InputPoint::CursorPos);  // 阶段 ②：按下
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(forced.pendingReleases()), 1);
        g_now_ms += lift::InputPlan::kReleaseFallbackMs;
        for (int i = 0; i < 3 && forced.pendingReleases() > 0; ++i) {
            forced.atPoint(*wm, lift::InputPoint::GetMsg);
        }
        const std::string t3 = readSink(fsink);
        LIFT_CHECK(t3.find("plan:mem:1 applied kind=click") != std::string::npos);
        LIFT_CHECK(t3.find("reason=wallclock") != std::string::npos);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(forced.pendingReleases()), 0);
        std::fclose(fsink);
    }
    std::fclose(sink);
}



// =====================================================================================
// 判据 ⑥ · 能力面只有一份：行尾 `timeout` 在本臂（读点臂）**到期** ⇒ 失败并点名
//
// 判据（红了意味着什么）：
//   ① 行尾 `timeout 500`（守**同一行**那个门）：**差 1 ms 不判**、超 1 ms 判（`>` 不是 `>=`）；
//   ② 判失败之后：`outcome()==TimedOut`、`pending` 不动、`fired` 不增（⛔ 失败后继续点）；
//   ③ `failureReport()` 点名 `mem:<行>` + 门/动作 + 指令原文（人能照着修）；
//   ④ 共享宽度公式 `stepTimeoutMs`：显式覆盖缺省、`wait` **恒 0**（固定时长、没有守卫）、缺省 0 ⇒ 0；
//   ⑤ `wait` 没有守卫（图臂的缺省 5000 也吃不到它）：guest 时钟冲过 `ms + 5000` 也**不**判 timeout。
// =====================================================================================
void caseStepTimeout() {
    lift_test::g_current_case = "input-plan/step-timeout";

    // ---- ④ 宽度公式（唯一一份；两个入口共用）----
    {
        lift::Step gate;
        gate.kind = lift::StepKind::AwaitScript;
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::stepTimeoutMs(gate, 0)), 0);      // 本臂无缺省
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::stepTimeoutMs(gate, 5000)), 5000);  // 解释器臂缺省
        gate.timeout_ms = 250;
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::stepTimeoutMs(gate, 5000)), 250);  // 显式覆盖
        lift::Step wait;
        wait.kind = lift::StepKind::Wait;
        wait.wait_ms = 25000;
        // ★ `wait` 是固定时长、到点就走 ⇒ **没有守卫**（⛔ 不再有 `ms + 缺省` 那条补丁）：两个入口都是 0。
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::stepTimeoutMs(wait, 5000)), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(lift::stepTimeoutMs(wait, 0)), 0);
        // "到期"的比较只有一份：严格大于
        LIFT_CHECK(!lift::stepWindowExpired(500, 500));
        LIFT_CHECK(lift::stepWindowExpired(501, 500));
        LIFT_CHECK(!lift::stepWindowExpired(1000000, 0));  // 0 = 没有窗口
    }

    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::WindowManager* wm = f.m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return;
    LIFT_CHECK(wm->createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u) != 0);
    // 身份 = TITLE，但配方等的是 NOSUCHSCRIPT ⇒ 门永不成立
    LIFT_CHECK(publishEngine(f.m, "TITLE.BIN", 42u));

    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    plan.attachGuestMemory(&f.m.mem());
    std::string err;
    const bool loaded = plan.loadText("await script NOSUCHSCRIPT timeout 500\nclick 10 20\n", "mem", &err);
    if (!loaded) std::fprintf(stderr, "带 timeout 的配方被拒：%s\n", err.c_str());
    LIFT_CHECK(loaded);
    if (!loaded) return;
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.steps()), 2);  // timeout 不是一条指令
    LIFT_CHECK(!plan.failed());
    LIFT_CHECK(plan.outcome() == lift::RecipeOutcome::Running);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pending()), 2);

    // ---- ① 差 1 ms：不判失败 ----
    g_now_ms = 1000;
    plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK(!plan.failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);
    g_now_ms = 1500;  // 已等 500 ms ⇒ 还没到期（严格大于）
    plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK(!plan.failed());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);

    // ---- ② 超 1 ms：到期 ⇒ 失败（**同一个读点**就判）----
    g_now_ms = 1501;
    plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK(plan.failed());
    LIFT_CHECK(plan.outcome() == lift::RecipeOutcome::TimedOut);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pending()), 2);
    const lift::Step* stuck = plan.currentStep();
    LIFT_CHECK(stuck != nullptr);
    if (stuck != nullptr) {
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(stuck->line), 1);
        LIFT_CHECK(stuck->kind == lift::StepKind::AwaitScript);
    }

    // ---- ③ 报告点名：`mem:1` + 门 + 指令原文 ----
    const std::string report = plan.failureReport();
    LIFT_CHECK(report.find("FAIL timeout") != std::string::npos);
    LIFT_CHECK(report.find("mem:1") != std::string::npos);
    LIFT_CHECK(report.find("await script NOSUCHSCRIPT") != std::string::npos);
    LIFT_CHECK(report.find("门") != std::string::npos);
    LIFT_CHECK(report.find("pending=2") != std::string::npos);

    // ---- ② 失败之后不再推进：再喂 10 个读点，fired / pending 一格不动 ----
    for (int i = 0; i < 10; ++i) plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pending()), 2);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);

    // ---- ⑤ `wait` **没有守卫**（删掉 `ms + 5000` 那条补丁的证明）----
    //   图臂有两把尺：guest 毫秒 + 墙钟。headless 下 guest 时钟可以远远跑在墙钟前面 ⇒ 旧公式会给
    //   `wait 20000` 一个 25000 的守卫，于是 guest 时钟一冲过 25000 就判 timeout（"固定等待"变成了
    //   假失败）。现在 `wait` 恒 0 ⇒ 同一条 `wait 20000` 冲过去**不失败**，墙钟到点才走完。
    //   ⛔ 不要把它改成"缩小到刚好不触发"：判据是"守卫根本不存在"。
    {
        lift_fixture::Fixture gf;
        LIFT_CHECK(gf.ok);
        if (!gf.ok) return;
        lift::WindowManager* gwm = gf.m.cpu().windows;
        LIFT_CHECK(gwm != nullptr);
        if (gwm == nullptr) return;
        lift::InteractionGraph graph;
        std::string gerr;
        LIFT_CHECK(graph.loadText("wait 20000\n", "mem:graph", &gerr));
        graph.tick(*gwm, 0, 0, 0, 0, nullptr, nullptr);  // 进入 wait：due = wall 20000
        // guest 时钟 25001 > 旧的 `ms + 5000` = 25000；墙钟还远没到 20000 ⇒ 旧公式在这里判 timeout。
        graph.tick(*gwm, 1, 25001, 100, 100, nullptr, nullptr);
        LIFT_CHECK(!graph.failed());
        LIFT_CHECK(!graph.done());
        graph.tick(*gwm, 2, 25002, 200, 20001, nullptr, nullptr);  // 墙钟到点 ⇒ 走完
        LIFT_CHECK(!graph.failed());
        LIFT_CHECK(graph.done());
    }
}

// =====================================================================================
// 判据 ⑦ · **完成度是判据**：门永不成立 ⇒ `pending != 0` ⇒ 退出码 **5**（⛔ 不是 0）
//
// 判据（红了意味着什么）：这一条就是"图 → 配方"改写会不会把 CTest 变成假绿的唯一闸门。
//   · `Routing`（还有指令没消费）⇒ `pending != 0` 且 `recipeOutcomeExitCode()` = 5；
//   · `Done` ⇒ `pending == 0` 且 `recipeOutcomeExitCode()` = 0（**不覆盖**调用方的运行结论）；
//   · `failureReport()` 在 `Done` 时是空串（⛔ 不许把成功也打成失败）。
// =====================================================================================
void caseCompletion() {
    lift_test::g_current_case = "input-plan/completion";

    // ---- 退出码口径（唯一一份；= 解释器臂那两处 exit 5）----
    LIFT_CHECK_EQ_I64(lift::recipeOutcomeExitCode(lift::RecipeOutcome::Done), 0);
    LIFT_CHECK_EQ_I64(lift::recipeOutcomeExitCode(lift::RecipeOutcome::Running), 5);
    LIFT_CHECK_EQ_I64(lift::recipeOutcomeExitCode(lift::RecipeOutcome::TimedOut), 5);
    LIFT_CHECK_EQ_I64(lift::kRecipeIncompleteExitCode, 5);

    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::WindowManager* wm = f.m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return;
    LIFT_CHECK(wm->createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u) != 0);
    LIFT_CHECK(publishEngine(f.m, "TITLE.BIN", 42u));

    // ---- ① 门永不成立（没有 `timeout` ⇒ 不会判超时，只会"没走完"）----
    lift::InputPlan stuck;
    stuck.setClockForTest(&testNowMs);
    stuck.attachGuestMemory(&f.m.mem());
    std::string err;
    LIFT_CHECK(stuck.loadText("await position TITLE 0x388C 0x38A4\nclick 1070 480\n", "mem", &err));
    for (int i = 0; i < 25; ++i) {
        g_now_ms += 1000;  // ★ 走很久也不算超时：没写 `timeout` ⇒ 本臂没有缺省窗口
        stuck.atPoint(*wm, lift::InputPoint::CursorPos);
    }
    LIFT_CHECK(!stuck.failed());
    LIFT_CHECK(stuck.outcome() == lift::RecipeOutcome::Running);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(stuck.pending()), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(stuck.fired()), 0);
    LIFT_CHECK_EQ_I64(lift::recipeOutcomeExitCode(stuck.outcome()), 5);
    const std::string report = stuck.failureReport();
    LIFT_CHECK(report.find("FAIL incomplete") != std::string::npos);
    LIFT_CHECK(report.find("mem:1") != std::string::npos);
    LIFT_CHECK(report.find("await position TITLE 0x0000388C 0x000038A4") != std::string::npos);
    // 摘要里 `pending=` 必须在（读这一行最短的形式：steps= fired= pending=）
    LIFT_CHECK(stuck.summary().find("steps=2 fired=0 pending=2") != std::string::npos);

    // ---- ② 反过来：全部走完 ⇒ Done + 退出码 0 + 空报告 ----
    lift::InputPlan done;
    done.setClockForTest(&testNowMs);
    done.attachGuestMemory(&f.m.mem());
    LIFT_CHECK(done.loadText("await script TITLE\nclick 10 20\n", "mem", &err));
    done.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK(done.outcome() == lift::RecipeOutcome::Done);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(done.pending()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(done.fired()), 1);
    LIFT_CHECK_EQ_I64(lift::recipeOutcomeExitCode(done.outcome()), 0);
    LIFT_CHECK(done.failureReport().empty());
    LIFT_CHECK(done.currentStep() == nullptr);
    LIFT_CHECK(done.summary().find("pending=0") != std::string::npos);
    (void)done.releaseAll(*wm);
}

// =====================================================================================
// 判据 ⑥' · `profile_begin|end` 与 `quit` 在**本臂**落地（不再是"这个入口不认"）
//
// 判据（红了意味着什么）：
//   ① 三条指令在 `--input-plan` 的解析器上**都被接受**（旧形状是 exit 2 的硬错）；
//   ② `profile_begin` + `profile_end` 收口一次（`profileWindows() == 1`），未配对的 `profile_end`
//      不算收口（响亮提示在 stderr，不是失败）；
//   ③ `quit` ⇒ `wm` 队列尾出现 WM_QUIT（与解释器臂同一个交付点）+ `quitRequested()` 为真 +
//      **不再推进**（后面的 `click` 永远不施加 ⇒ `pending != 0`）。
// =====================================================================================
void caseProfileAndQuit() {
    lift_test::g_current_case = "input-plan/profile-and-quit";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::WindowManager* wm = f.m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return;
    LIFT_CHECK(wm->createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u) != 0);
    LIFT_CHECK(publishEngine(f.m, "TITLE.BIN", 42u));
    g_now_ms = 1000;

    // ---- ① 三条指令都被接受（旧形状：`timeout` / `profile_*` / `quit` ⇒ "这个入口不认" + exit 2）----
    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    plan.attachGuestMemory(&f.m.mem());
    std::string err;
    // ★ `wait 500` 夹在 begin / end 之间：`atPoint` 会在**同一个读点**把连续可满足的指令全部消费掉
    //   （flush-before-read），没有这条 `wait` 就观察不到"窗口开着"那一刻。
    const bool loaded = plan.loadText(
        "profile_begin title\n"
        "await script TITLE timeout 30000\n"
        "wait 500\n"
        "profile_end title\n"
        "quit\n"
        "click 10 20\n",
        "mem", &err);
    if (!loaded) std::fprintf(stderr, "profile/quit 配方被拒（能力面又分叉了）：%s\n", err.c_str());
    LIFT_CHECK(loaded);
    if (!loaded) return;
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.steps()), 6);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.profileWindows()), 0);

    // ---- ② `profile_end` 没有配对的 `begin`：响亮提示但不失败（先验一条独立配方）----
    {
        lift::InputPlan orphan;
        orphan.setClockForTest(&testNowMs);
        LIFT_CHECK(orphan.loadText("profile_end lonely\n", "mem", &err));
        orphan.atPoint(*wm, lift::InputPoint::CursorPos);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(orphan.profileWindows()), 0);
        LIFT_CHECK(orphan.outcome() == lift::RecipeOutcome::Done);  // 仪器问题不是任务失败
    }

    // ---- ③ 正常一跑：begin 落地 ⇒ open；`wait` 到期后 end 收口 ⇒ quit 投 WM_QUIT ----
    g_now_ms = 2000;
    plan.atPoint(*wm, lift::InputPoint::CursorPos);  // profile_begin + 门成立 + wait 起算（未到期）
    LIFT_CHECK(plan.profileOpen());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.profileWindows()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pending()), 4);  // wait 还没到期 ⇒ 剩 4 条未消费
    g_now_ms = 2500;
    plan.atPoint(*wm, lift::InputPoint::CursorPos);  // wait 到期 + profile_end 收口 + quit
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.profileWindows()), 1);
    LIFT_CHECK(!plan.profileOpen());
    LIFT_CHECK(plan.quitRequested());
    LIFT_CHECK(*plan.quitFlag());
    // WM_QUIT 是队列尾的一条虚拟消息（`WindowManager::peekAt(queueSize())`）
    lift::WinMessage quit_msg;
    LIFT_CHECK(wm->peekAt(wm->queueSize(), &quit_msg));
    LIFT_CHECK_EQ_U32(quit_msg.message, kWmQuit);
    // quit 之后的 `click` 永远不施加 ⇒ 任务没走完（⛔ quit 不是"万能成功"）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pending()), 1);
    LIFT_CHECK(plan.outcome() == lift::RecipeOutcome::Running);
    for (int i = 0; i < 10; ++i) plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pending()), 1);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
}

// =====================================================================================
// S6 · `mouse_move` / `mouse_down` / `mouse_up`：缩放 / 轻闸门 / 直写（§4.6 判据 1）
//
// 判据（红了意味着什么）：
//   ① `mouse_move 1070 480` 在 1920×1080 客户区 ⇒ 设备 client = (1605,720)（与 `click` 同一公式）；
//      `WM_MOUSEMOVE` 的 lParam 是同一个映射结果（⛔ 第二套缩放）。
//   ② `mouse_down` **轻闸门**：上一条 `mouse_move` 之后**没完成过** CursorPos 读点 ⇒ **不消费**该 step
//      （光标读点那一拍也还没按）；跨过它之后的下一个读点才按下。
//   ③ `mouse_down` / `mouse_up` ⛔ **不进**待抬起队列（`pendingReleases` 恒 0、`armedPresses` 恒 0）：
//      释放由配方自己控制（Q3：⛔ 不套闸门①/②）。
//   ④ `mouse_down` 的墙钟兜底：整屏不读位置 ⇒ `kPressFallbackMs` 到期仍然按下（⛔ 永远挂着）。
// =====================================================================================
void caseMousePrimitives() {
    lift_test::g_current_case = "input-plan/mouse-primitives";
    g_now_ms = 5000;

    // ---- ① 缩放：非 1:1 客户区，`mouse_move` 与 `click` 的映射同一份 ----
    {
        lift::WindowManager wm;
        const std::uint32_t hwnd = wm.createWindow("PlanTest", 0u, 0u, 0u, 0u, 1920, 1080, 0u);
        LIFT_CHECK(hwnd != 0);
        lift::InputPlan plan;
        std::string err;
        LIFT_CHECK(plan.loadText("mouse_move 1070 480\n", "mem", &err));
        const std::size_t q0 = wm.queueSize();
        plan.atPoint(wm, lift::InputPoint::PeekMsg);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pending()), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 0);  // move 不是"动作"
        int cx = 0;
        int cy = 0;
        wm.cursorClientPos(hwnd, &cx, &cy);
        LIFT_CHECK_EQ_I64(cx, 1605);  // 1070 * 1920 / 1280
        LIFT_CHECK_EQ_I64(cy, 720);   // 480 * 1080 / 720
        lift::WinMessage move_msg;
        LIFT_CHECK(findMessage(wm, q0, kWmMouseMove, &move_msg));
        LIFT_CHECK_EQ_U32(move_msg.lparam, mouseLParam(1605, 720));
        LIFT_CHECK_EQ_U32(move_msg.wparam, 0u);  // 没有按下的按钮
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessage(wm, q0, kWmLButtonDown)), 0);
    }

    // ---- ② + ③ 轻闸门 / 不进待抬起队列 ----
    {
        lift::WindowManager wm;
        const std::uint32_t hwnd = wm.createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
        LIFT_CHECK(hwnd != 0);
        lift::InputPlan plan;
        plan.setClockForTest(&testNowMs);
        std::string err;
        LIFT_CHECK(plan.loadText("mouse_move 100 100\nmouse_down\n", "mem", &err));
        const std::size_t q0 = wm.queueSize();
        g_now_ms = 6000;
        plan.atPoint(wm, lift::InputPoint::PeekMsg);  // 只把光标放过去（写预算也用掉）
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);
        LIFT_CHECK_EQ_U32(wm.mouseButtons(), 0u);
        // 指针读点这一拍：move 之后**还没完成过** CursorPos 读点 ⇒ 不消费（⛔ 不猜"位置已经旧了"）。
        g_now_ms += 1;
        plan.atPoint(wm, lift::InputPoint::CursorPos);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);
        LIFT_CHECK_EQ_U32(wm.mouseButtons(), 0u);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessage(wm, q0, kWmLButtonDown)), 0);
        // 这个 CursorPos 读点完成之后 ⇒ 下一个读点按下（且只有 down，不进 pending_）。
        g_now_ms += 1;
        plan.atPoint(wm, lift::InputPoint::PeekMsg);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 2);
        LIFT_CHECK_EQ_U32(wm.mouseButtons(), lift::input::kMkLButton);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);  // ★ 不套闸门①/②
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 0);
        lift::WinMessage down_msg;
        LIFT_CHECK(findMessage(wm, q0, kWmLButtonDown, &down_msg));
        LIFT_CHECK_EQ_U32(down_msg.lparam, mouseLParam(100, 100));  // 设备当前位置
        LIFT_CHECK_EQ_U32(down_msg.wparam, lift::input::kMkLButton);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessage(wm, q0, kWmLButtonDown)), 1);
        (void)plan.releaseAll(wm);  // 这条 mouse_down 没有 up ⇒ 收尾（⛔ 不留给下一条配方）
        LIFT_CHECK_EQ_U32(wm.mouseButtons(), 0u);
        // `mouse_up` 直写：不排队、不进 armed_、不等任何闸门 ⇒ 下一个读点立刻发 WM_LBUTTONUP。
        lift::InputPlan up_plan;
        up_plan.setClockForTest(&testNowMs);
        LIFT_CHECK(up_plan.loadText("mouse_move 100 100\nmouse_down\nmouse_up\n", "mem", &err));
        const std::size_t q1 = wm.queueSize();
        up_plan.atPoint(wm, lift::InputPoint::PeekMsg);   // move
        up_plan.atPoint(wm, lift::InputPoint::CursorPos);  // 位置读点（down 的闸门还没满足）
        up_plan.atPoint(wm, lift::InputPoint::KeyLevel);   // down（⛔ KeyLevel 不是 down 的闸门，这里只是下一个读点）
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(up_plan.stepsApplied()), 2);
        // 预算：down 那条写动作已用完 ⇒ 同一读点里 `mouse_up` 留到下一点（S7）。
        g_now_ms += 1;
        up_plan.atPoint(wm, lift::InputPoint::KeyLevel);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(up_plan.stepsApplied()), 3);
        LIFT_CHECK_EQ_U32(wm.mouseButtons(), 0u);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(up_plan.pendingReleases()), 0);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessage(wm, q1, kWmLButtonUp)), 1);
        lift::WinMessage up_msg;
        LIFT_CHECK(findMessage(wm, q1, kWmLButtonUp, &up_msg));
        LIFT_CHECK_EQ_U32(up_msg.lparam, mouseLParam(100, 100));
    }

    // ---- ④ 墙钟兜底：整屏不读位置 ⇒ kPressFallbackMs 到期仍然按下（⛔ 提前按）----
    {
        lift::WindowManager wm;
        LIFT_CHECK(wm.createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u) != 0);
        lift::InputPlan plan;
        plan.setClockForTest(&testNowMs);
        std::string err;
        LIFT_CHECK(plan.loadText("mouse_move 10 20\nmouse_down\n", "mem", &err));
        g_now_ms = 7000;
        plan.atPoint(wm, lift::InputPoint::PeekMsg);  // only move（纯消息型屏）
        LIFT_CHECK_EQ_U32(wm.mouseButtons(), 0u);
        g_now_ms += lift::InputPlan::kPressFallbackMs / 5;
        plan.atPoint(wm, lift::InputPoint::PeekMsg);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);  // ⛔ 还没按
        LIFT_CHECK_EQ_U32(wm.mouseButtons(), 0u);
        g_now_ms += lift::InputPlan::kPressFallbackMs;
        plan.atPoint(wm, lift::InputPoint::PeekMsg);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 2);
        LIFT_CHECK_EQ_U32(wm.mouseButtons(), lift::input::kMkLButton);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
        (void)plan.releaseAll(wm);
        LIFT_CHECK_EQ_U32(wm.mouseButtons(), 0u);
    }
}

// =====================================================================================
// S4/S6 · `InteractionGraph::issueAction` 的 **mouse 分支**（设计 §4.6）：`mouse_down` / `mouse_up`
//   的临时解析器要求 `have_pos_` ⇒ 图必须为每条 down/up **在同一段追加文本里**补一条**同点**
//   `mouse_move`（⛔ 不是第二套按下 / 抬起）。
//
// 判据（红了意味着什么）：图发出的动作**文本序列**必须是
//   `mouse_move (x,y)` → [`mouse_move (x,y)` → `mouse_down`] → [`mouse_move (x,y)` → `mouse_up`]；
//   少了那条 move ⇒ `appendText` 解析失败（动作永远不会被追加）；move 换了点 ⇒ down/up 落错位置。
//   ★ 可观察面 = `InputScript::actions()[i]` 的 `kind` / `x` / `y` / `button` / `text` /
//     `trigger.value` + `InteractionGraph::actionsIssued()`。`InteractionGraph` **没有 CLI**
//     （见其文件头），`actuator()` 是它唯一的动作出口 ⇒ 这里断的就是它实际发出的东西。
//   ⛔ 不要为此给 `InputScript` 加"跨 `appendText` 保留位置"的第二套解析状态。
// =====================================================================================
void caseGraphMouseActions() {
    lift_test::g_current_case = "input-plan/graph-mouse-actions";
    lift::WindowManager wm;
    const std::uint32_t hwnd = wm.createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
    LIFT_CHECK(hwnd != 0);
    if (hwnd == 0) return;
    lift::InteractionGraph graph;
    std::string err;
    const bool loaded = graph.loadText("mouse_move 300 400\nmouse_down\nmouse_up\n", "mem:graph-mouse", &err);
    if (!loaded) std::fprintf(stderr, "图配方被拒：%s\n", err.c_str());
    LIFT_CHECK(loaded);
    if (!loaded) return;
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(graph.steps().size()), 3);

    // 块边界推进（与 `Machine` 的块边界顺序相同）：actuator 先应用到期动作，图再追加下一步。
    lift::InputScript& act = graph.actuator();
    std::uint64_t frame = 0;
    for (int i = 0; i < 200 && !graph.done() && !graph.failed(); ++i) {
        ++frame;
        const std::uint64_t guest_ms = frame * 100ull;
        act.tick(wm, frame, guest_ms, guest_ms, guest_ms);
        graph.tick(wm, frame, guest_ms, guest_ms, guest_ms, nullptr, nullptr);
    }
    LIFT_CHECK(!graph.failed());
    LIFT_CHECK(graph.done());
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(graph.actionsIssued()), 3);  // 三条步骤各发一次

    const std::vector<lift::InputScript::Action>& acts = act.actions();
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(acts.size()), 5);
    if (acts.size() != 5) return;
    // ---- ① 形状：move → (move, down) → (move, up) ----
    LIFT_CHECK(acts[0].kind == lift::InputScript::Kind::MouseMove);
    LIFT_CHECK(acts[1].kind == lift::InputScript::Kind::MouseMove);
    LIFT_CHECK(acts[2].kind == lift::InputScript::Kind::MouseDown);
    LIFT_CHECK(acts[3].kind == lift::InputScript::Kind::MouseMove);
    LIFT_CHECK(acts[4].kind == lift::InputScript::Kind::MouseUp);
    // ---- ② 文本面：down/up 前**紧挨着**同点 `mouse_move`（§4.6 明写）----
    LIFT_CHECK(acts[0].text == "mouse_move 300 400");
    LIFT_CHECK(acts[1].text == "mouse_move 300 400");
    LIFT_CHECK(acts[2].text == "mouse_down");
    LIFT_CHECK(acts[3].text == "mouse_move 300 400");
    LIFT_CHECK(acts[4].text == "mouse_up");
    // ---- ③ 坐标面：三条 `mouse_move` 都是图自己的 last_move（300,400）；down/up 的坐标就是它 ----
    const int move_idx[] = {0, 1, 3};
    for (const int k : move_idx) {
        LIFT_CHECK_EQ_I64(acts[k].x, 300);
        LIFT_CHECK_EQ_I64(acts[k].y, 400);
    }
    LIFT_CHECK(acts[2].button == lift::MouseButton::Left);
    LIFT_CHECK(acts[4].button == lift::MouseButton::Left);
    // ---- ④ 触发面：每次追加 = `at frame <当时 frame + 1>`（动作只在**下一个 Present** 落地）----
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(acts[0].trigger.value), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(acts[1].trigger.value), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(acts[2].trigger.value), 3);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(acts[3].trigger.value), 4);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(acts[4].trigger.value), 4);
    // ---- ⑤ 设备面：动作真的落地 —— 光标在 (300,400)，队列里有一条落在 (300,400) 的 DOWN ----
    //   ⚠ 图臂是**测试夹具**（没有 CLI）：`InputScript` 的收口（追加批次 `finished()`）会在
    //     down 应用完就补一条 UP ⇒ 这里不断言"最终仍按着"，只断言"DOWN 真的发过、位置正确"。
    int cx = 0;
    int cy = 0;
    wm.cursorClientPos(hwnd, &cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 300);
    LIFT_CHECK_EQ_I64(cy, 400);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessage(wm, 0, kWmLButtonDown)), 1);
    lift::WinMessage down_msg;
    LIFT_CHECK(findMessage(wm, 0, kWmLButtonDown, &down_msg));
    LIFT_CHECK_EQ_U32(down_msg.lparam, mouseLParam(300, 400));
    LIFT_CHECK_EQ_U32(down_msg.wparam, lift::input::kMkLButton);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessage(wm, 0, kWmMouseMove)), 3);  // 每条 down/up 各补一条
}

// =====================================================================================
// S6 · 拖拽：down A → move B → up B（§4.6 判据 2）
//
// 判据（红了意味着什么）：
//   ① 序列 = MOVE(A) → DOWN(A) → MOVE(B) → UP(B)：拖拽中每条鼠标动作各占**一个读点**（S7 的写预算），
//      ⛔ 不是同一个读点里只剩最后一个位置；
//   ② `WM_MOUSEMOVE` 的 wParam 在按下之后带 `MK_LBUTTON`（**设备电平的投影**）；
//   ③ `UP` 的 lParam = **B**（不是按下点 A）⇒ 拖拽 up 不回跳（M4）。
// =====================================================================================
void caseDrag() {
    lift_test::g_current_case = "input-plan/drag";
    lift::WindowManager wm;
    const std::uint32_t hwnd = wm.createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
    LIFT_CHECK(hwnd != 0);
    if (hwnd == 0) return;
    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    std::string err;
    const bool loaded = plan.loadText("mouse_move 100 100\nmouse_down\nmouse_move 200 200\nmouse_up\n", "mem",
                                      &err);
    if (!loaded) std::fprintf(stderr, "拖拽配方被拒：%s\n", err.c_str());
    LIFT_CHECK(loaded);
    if (!loaded) return;
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.steps()), 4);

    const std::size_t q0 = wm.queueSize();
    g_now_ms = 8000;
    plan.atPoint(wm, lift::InputPoint::PeekMsg);  // ① MOVE(A)
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);
    g_now_ms += 1;
    plan.atPoint(wm, lift::InputPoint::CursorPos);  // ② 位置读点：down 的轻闸门还没满足
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);
    g_now_ms += 1;
    plan.atPoint(wm, lift::InputPoint::PeekMsg);  // ③ DOWN(A)；预算用完 ⇒ move B 留到下一点
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 2);
    LIFT_CHECK_EQ_U32(wm.mouseButtons(), lift::input::kMkLButton);
    g_now_ms += 1;
    plan.atPoint(wm, lift::InputPoint::PeekMsg);  // ④ MOVE(B)（wParam 带 MK_LBUTTON）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 3);
    int cx = 0;
    int cy = 0;
    wm.cursorClientPos(hwnd, &cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 200);
    LIFT_CHECK_EQ_I64(cy, 200);
    g_now_ms += 1;
    plan.atPoint(wm, lift::InputPoint::PeekMsg);  // ⑤ UP(B)（回读设备当前位置 ⇒ 不回跳）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 4);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pending()), 0);
    LIFT_CHECK(plan.outcome() == lift::RecipeOutcome::Done);
    LIFT_CHECK(!plan.failed());
    LIFT_CHECK_EQ_U32(wm.mouseButtons(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);  // ⛔ 没进待抬起队列
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.releases()), 0);         // up 是配方直写，不是"自动抬起"

    // ---- 消息面：只有一对 down/up；up 的 lParam 是 B（⛔ 不是按下点 A）----
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessage(wm, q0, kWmLButtonDown)), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessage(wm, q0, kWmLButtonUp)), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessage(wm, q0, kWmMouseMove)), 2);
    lift::WinMessage m;
    std::vector<std::uint32_t> moves;
    std::vector<std::uint32_t> move_wparams;
    std::uint32_t down_lparam = 0;
    std::uint32_t up_lparam = 0;
    for (std::size_t i = q0; i < wm.queueSize(); ++i) {
        if (!wm.peekAt(i, &m)) continue;
        if (m.message == kWmMouseMove) {
            moves.push_back(m.lparam);
            move_wparams.push_back(m.wparam);
        } else if (m.message == kWmLButtonDown) {
            down_lparam = m.lparam;
        } else if (m.message == kWmLButtonUp) {
            up_lparam = m.lparam;
        }
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(moves.size()), 2);
    if (moves.size() == 2) {
        LIFT_CHECK_EQ_U32(moves[0], mouseLParam(100, 100));
        LIFT_CHECK_EQ_U32(moves[1], mouseLParam(200, 200));
        LIFT_CHECK_EQ_U32(move_wparams[0], 0u);                    // 按下之前：没有 MK 位
        LIFT_CHECK_EQ_U32(move_wparams[1], lift::input::kMkLButton);  // 按下之后：电平的投影
    }
    LIFT_CHECK_EQ_U32(down_lparam, mouseLParam(100, 100));
    LIFT_CHECK_EQ_U32(up_lparam, mouseLParam(200, 200));
    LIFT_CHECK(up_lparam != mouseLParam(100, 100));  // ⛔ 不回跳
}

// =====================================================================================
// S6 · **重复跑**（REQ 判据 3）：同一份拖拽配方在两个**全新 fixture** 上各跑一遍
//
// 判据（红了意味着什么）：两遍的**消息序列**（message + wParam + lParam）逐条相同；
//   两遍都 `pending == 0`；没有门 timeout 误报。⛔ 不以"画面动了"为判据。
// =====================================================================================
const char* const kDragRecipe =
    "mouse_move 300 200\nmouse_down\nmouse_move 640 360\nmouse_up\n";

std::string runDragOnce(bool* ok) {
    *ok = false;
    lift::WindowManager wm;
    const std::uint32_t hwnd = wm.createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
    if (hwnd == 0) return std::string();
    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    std::string err;
    if (!plan.loadText(kDragRecipe, "mem", &err)) return err;
    g_now_ms = 9000;  // 两遍同一时钟起点（⛔ 不让墙钟兜底参与判据）
    const lift::InputPoint points[] = {
        lift::InputPoint::PeekMsg,   // move A
        lift::InputPoint::CursorPos,  // 位置读点（down 的闸门：这一拍还不满足）
        lift::InputPoint::PeekMsg,    // down
        lift::InputPoint::PeekMsg,    // move B
        lift::InputPoint::PeekMsg,    // up
    };
    for (const lift::InputPoint p : points) {
        g_now_ms += 1;
        plan.atPoint(wm, p);
    }
    *ok = plan.stepsApplied() == 4 && plan.pending() == 0 && !plan.failed() &&
          plan.outcome() == lift::RecipeOutcome::Done && plan.pendingReleases() == 0 &&
          wm.mouseButtons() == 0u;
    return dumpQueue(wm);
}

void caseDragRepeat() {
    lift_test::g_current_case = "input-plan/drag-repeat";
    bool ok_a = false;
    bool ok_b = false;
    const std::string dump_a = runDragOnce(&ok_a);
    const std::string dump_b = runDragOnce(&ok_b);
    LIFT_CHECK(ok_a);
    LIFT_CHECK(ok_b);
    LIFT_CHECK(!dump_a.empty());
    LIFT_CHECK(dump_a == dump_b);  // ★ 逐条相同（message/wParam/lParam）
    std::printf("[drag-repeat] a=%s b=%s\n", dump_a.c_str(), dump_b.c_str());
}

// =====================================================================================
// S7 · **一个读点最多一条写动作**（§4.7 判据 1/2）
//
// 判据（红了意味着什么）：
//   ① 两条 `mouse_move` 分两个读点各发**一条** `WM_MOUSEMOVE`（同一个读点里不能只剩最后一个位置）；
//   ② 控制层同一读点连投两条 move ⇒ 只落**第一条**，第二条留在队首（`queuedClicks() == 1`）。
// =====================================================================================
void caseOneWritePerReadPoint() {
    lift_test::g_current_case = "input-plan/one-write-per-read-point";
    g_now_ms = 10000;
    {
        lift::WindowManager wm;
        const std::uint32_t hwnd = wm.createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
        LIFT_CHECK(hwnd != 0);
        lift::InputPlan plan;
        std::string err;
        LIFT_CHECK(plan.loadText("mouse_move 100 100\nmouse_move 200 200\n", "mem", &err));
        const std::size_t q0 = wm.queueSize();
        plan.atPoint(wm, lift::InputPoint::PeekMsg);
        int cx = 0;
        int cy = 0;
        wm.cursorClientPos(hwnd, &cx, &cy);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);
        LIFT_CHECK_EQ_I64(cx, 100);
        LIFT_CHECK_EQ_I64(cy, 100);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessage(wm, q0, kWmMouseMove)), 1);
        plan.atPoint(wm, lift::InputPoint::PeekMsg);
        wm.cursorClientPos(hwnd, &cx, &cy);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 2);
        LIFT_CHECK_EQ_I64(cx, 200);
        LIFT_CHECK_EQ_I64(cy, 200);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessage(wm, q0, kWmMouseMove)), 2);
    }
    {
        // ② 控制层立即队列：同一个读点连投两条 move ⇒ 第一条落地、第二条留队首。
        lift_test::g_current_case = "input-plan/control-queue-one-write";
        lift::WindowManager wm;
        const std::uint32_t hwnd = wm.createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
        LIFT_CHECK(hwnd != 0);
        lift::InputPlan plan;
        LIFT_CHECK(plan.enqueueMove(10, 10));
        LIFT_CHECK(plan.enqueueMove(20, 20));
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.queuedClicks()), 2);
        const std::size_t q0 = wm.queueSize();
        plan.atPoint(wm, lift::InputPoint::PeekMsg);
        int cx = 0;
        int cy = 0;
        wm.cursorClientPos(hwnd, &cx, &cy);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.queuedClicks()), 1);  // ★ 其余留队首
        LIFT_CHECK_EQ_I64(cx, 10);
        LIFT_CHECK_EQ_I64(cy, 10);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessage(wm, q0, kWmMouseMove)), 1);
        plan.atPoint(wm, lift::InputPoint::PeekMsg);
        wm.cursorClientPos(hwnd, &cx, &cy);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.queuedClicks()), 0);
        LIFT_CHECK_EQ_I64(cx, 20);
        LIFT_CHECK_EQ_I64(cy, 20);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(countMessage(wm, q0, kWmMouseMove)), 2);
    }
}

// =====================================================================================
// S7 · **连点串行化**（§4.7 判据 3 / Q8-A）
//
// 判据（红了意味着什么）：
//   ① 第一条 click 还按着（`pending_`）⇒ 第二条 **不消费**（`stepsApplied()` 停在 1），
//      ⛔ 也不再"同按钮先抬起"（那样第一条的 down 一次都不会被 guest 读到）；
//   ② 抬起第一条之后的下一个读点才 arm 第二条；
//   ③ 消息序列必须是 **DOWN,UP,DOWN,UP** —— ⛔ 不是 DOWN,DOWN,UP,UP。
// =====================================================================================
void caseTwoClicksSerialized() {
    lift_test::g_current_case = "input-plan/two-clicks-serialized";
    lift::WindowManager wm;
    const std::uint32_t hwnd = wm.createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
    LIFT_CHECK(hwnd != 0);
    if (hwnd == 0) return;
    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    std::string err;
    const bool loaded = plan.loadText("click 100 100\nclick 200 200\n", "mem", &err);
    LIFT_CHECK(loaded);
    if (!loaded) return;
    g_now_ms = 11000;
    const std::size_t q0 = wm.queueSize();
    int cx = 0;
    int cy = 0;

    plan.atPoint(wm, lift::InputPoint::CursorPos);  // 第一条 arm（只 move）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 1);
    wm.cursorClientPos(hwnd, &cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 100);
    LIFT_CHECK_EQ_I64(cy, 100);

    plan.atPoint(wm, lift::InputPoint::CursorPos);  // 第一条 press（跨过一个完成的位置读点）
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);

    // 第一条还按着 ⇒ 第二个 click 的两个读点都**不消费**（也不强制抬起）。
    for (int i = 0; i + 1 < static_cast<int>(lift::InputPlan::kKeyLevelReadsBeforeRelease); ++i) {
        plan.atPoint(wm, lift::InputPoint::KeyLevel);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
        LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);
    }
    // 第 N 个 KeyLevel 读点：第一条抬起（写动作）⇒ 同读点**不再** arm 第二条。
    plan.atPoint(wm, lift::InputPoint::KeyLevel);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 1);  // ★ 写预算

    plan.atPoint(wm, lift::InputPoint::CursorPos);  // 第二条 arm
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 2);
    wm.cursorClientPos(hwnd, &cx, &cy);
    LIFT_CHECK_EQ_I64(cx, 200);
    LIFT_CHECK_EQ_I64(cy, 200);
    plan.atPoint(wm, lift::InputPoint::CursorPos);  // 第二条 press
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
    for (std::uint64_t i = 0; i < lift::InputPlan::kKeyLevelReadsBeforeRelease; ++i) {
        plan.atPoint(wm, lift::InputPoint::KeyLevel);
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.releases()), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.stepsApplied()), 2);
    LIFT_CHECK(plan.outcome() == lift::RecipeOutcome::Done);

    // ★ 消息序列：DOWN,UP,DOWN,UP（⛔ DOWN,DOWN,UP,UP = 两条按在同一拍里）
    const std::string seq = downUpSequence(wm);
    std::printf("[two-clicks-serialized] down/up seq=%s stepsApplied=%llu\n", seq.c_str(),
                static_cast<unsigned long long>(plan.stepsApplied()));
    LIFT_CHECK(seq == "DUDU");
    lift::WinMessage m;
    std::uint32_t down_lparams[2] = {0u, 0u};
    std::uint32_t up_lparams[2] = {0u, 0u};
    std::size_t nd = 0;
    std::size_t nu = 0;
    for (std::size_t i = q0; i < wm.queueSize(); ++i) {
        if (!wm.peekAt(i, &m)) continue;
        if (m.message == kWmLButtonDown && nd < 2) down_lparams[nd++] = m.lparam;
        if (m.message == kWmLButtonUp && nu < 2) up_lparams[nu++] = m.lparam;
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(nd), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(nu), 2);
    LIFT_CHECK_EQ_U32(down_lparams[0], mouseLParam(100, 100));
    LIFT_CHECK_EQ_U32(up_lparams[0], mouseLParam(100, 100));
    LIFT_CHECK_EQ_U32(down_lparams[1], mouseLParam(200, 200));
    LIFT_CHECK_EQ_U32(up_lparams[1], mouseLParam(200, 200));
    LIFT_CHECK_EQ_U32(wm.mouseButtons(), 0u);
}

// =====================================================================================
// ⑨ S9 · 注入点击过叠层的**同一个** UiClickRouter（D6 的三个入口里的注入 down / up）
//
// 判据（红了意味着什么）：
//   ① **命中 ⇒ 全吞**：`--ui-input=consume`（缺省）下，配方 / 控制层的 `click` 命中叠层 ⇒
//      `fired` 照旧 +1、`uiConsumed()` +1，而 guest **一条 `WM_MOUSEMOVE/WM_*BUTTON*` 都没有**、
//      按钮电平不动、**光标不动**（臂上不调 `input::mouseMove`）；
//   ② **down 吞 ⇒ up 吞**：up 在 router 上的返回值**不**改变决定（这里模拟 ClickRouter 的 capture：
//      Up 返回 true；但即使返回 false，`InputPlan` 也必须吞 —— 由 `swallowed` 决定）；
//   ③ **时序不变**：吞掉的 click 仍然要跨过按下闸门③与抬起闸门①才收口 —— 与不吞时同一个形状；
//   ④ **未命中 ⇒ 逐位相同**：装了 router 但点在 rect 外 ⇒ 消息序列与**完全没有 router**逐位相同，
//      `uiConsumed()==0`（这是 `--ui-input=pass` 之外的第二条"guest 不变"证据）。
// =====================================================================================
struct RecordingRouter : lift::UiInputRouter {
    // 命中区（**guest 客户区**坐标；本夹具自己的判定，⛔ 不是第二份 ClickRouter）。
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool capture = false;
    int down_count = 0;
    int up_count = 0;
    std::vector<lift::UiPointerPhase> phases;
    std::vector<int> px;
    std::vector<int> py;

    bool routeInjectedPointer(lift::UiPointerPhase phase, lift::MouseButton button, int cx, int cy) override {
        if (button != lift::MouseButton::Left) return false;  // 右 / 中键一律放行（D6）
        phases.push_back(phase);
        px.push_back(cx);
        py.push_back(cy);
        if (phase == lift::UiPointerPhase::Down) {
            ++down_count;
            const bool hit = w > 0 && h > 0 && cx >= x && cy >= y && cx < x + w && cy < y + h;
            capture = hit;
            return hit;
        }
        ++up_count;
        // 模拟 ClickRouter：down 接了的 up 才吞。⛔ 这个返回值**不是** InputPlan 的判据
        // （down 吞 ⇒ up 吞），它只让本夹具与 `ui/hit.cpp` 的语义同形。
        const bool swallow = capture;
        capture = false;
        return swallow;
    }
};

void caseUiConsumedClick() {
    lift_test::g_current_case = "input-plan/click-consumed-by-ui";
    lift_fixture::Fixture f;
    LIFT_CHECK(f.ok);
    if (!f.ok) return;
    lift::WindowManager* wm = f.m.cpu().windows;
    LIFT_CHECK(wm != nullptr);
    if (wm == nullptr) return;
    const std::uint32_t hwnd = wm->createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u);
    LIFT_CHECK(hwnd != 0);

    RecordingRouter router;
    router.w = 100;
    router.h = 100;  // client (10,20) 落在里面
    wm->setUiInputRouter(&router);

    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    g_now_ms = 1000;
    std::string err;
    LIFT_CHECK(plan.loadText("click 10 20\n", "mem", &err));

    const std::size_t q0 = wm->queueSize();
    int cur0_x = -1;
    int cur0_y = -1;
    wm->cursorClientPos(hwnd, &cur0_x, &cur0_y);

    // ---- 阶段①：臂上，但被叠层吞掉 ⇒ 不 move、不按下、光标不动 ----
    plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.fired()), 1);         // 动作照旧算施加
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.uiConsumed()), 1);    // 叠层吞了它
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.armedPresses()), 1);  // 三段时序仍在
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(wm->queueSize()), static_cast<std::int64_t>(q0));
    int cur_x = 0;
    int cur_y = 0;
    wm->cursorClientPos(hwnd, &cur_x, &cur_y);
    LIFT_CHECK_EQ_I64(cur_x, cur0_x);
    LIFT_CHECK_EQ_I64(cur_y, cur0_y);

    // ---- 阶段②：跨过一个已完成的位置读点 ⇒ 生成待抬起，但**没有** WM_LBUTTONDOWN ----
    plan.atPoint(*wm, lift::InputPoint::CursorPos);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 1);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(wm->queueSize()), static_cast<std::int64_t>(q0));
    wm->cursorClientPos(hwnd, &cur_x, &cur_y);
    LIFT_CHECK_EQ_I64(cur_x, cur0_x);
    LIFT_CHECK_EQ_I64(cur_y, cur0_y);

    // ---- 闸门①：跨过 kKeyLevelReadsBeforeRelease 个 KeyLevel 读点才抬起；up 也**不进 guest** ----
    for (std::uint64_t i = 0; i < lift::InputPlan::kKeyLevelReadsBeforeRelease; ++i) {
        plan.atPoint(*wm, lift::InputPoint::KeyLevel);
    }
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.pendingReleases()), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(plan.releases()), 1);
    LIFT_CHECK_EQ_U32(wm->mouseButtons(), 0u);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(wm->queueSize()), static_cast<std::int64_t>(q0));
    wm->cursorClientPos(hwnd, &cur_x, &cur_y);
    LIFT_CHECK_EQ_I64(cur_x, cur0_x);
    LIFT_CHECK_EQ_I64(cur_y, cur0_y);

    // ---- router 收到 Down→Up，坐标一致（= guest 客户区，与 click 的映射同一个空间）----
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(router.phases.size()), 2);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(router.down_count), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(router.up_count), 1);
    if (router.phases.size() == 2) {
        LIFT_CHECK(router.phases[0] == lift::UiPointerPhase::Down);
        LIFT_CHECK(router.phases[1] == lift::UiPointerPhase::Up);
        LIFT_CHECK_EQ_I64(router.px[0], 10);
        LIFT_CHECK_EQ_I64(router.py[0], 20);
        LIFT_CHECK_EQ_I64(router.px[1], 10);
        LIFT_CHECK_EQ_I64(router.py[1], 20);
    }
    // ---- 摘要：只在吞过时追加 `ui_consumed=N`（默认档逐字不变）----
    LIFT_CHECK(plan.summary().find("ui_consumed=1") != std::string::npos);
    LIFT_CHECK(plan.outcome() == lift::RecipeOutcome::Done);
    // 读数（不是断言）：一行能直接对账"吞了几条 / guest 收到几条 / 光标在哪"。
    std::printf("[click-consumed-by-ui] fired=%llu ui_consumed=%llu guest_queued=%zu buttons=0x%X "
                "cursor=(%d,%d) router_phases=%zu at=(%d,%d)\n",
                static_cast<unsigned long long>(plan.fired()),
                static_cast<unsigned long long>(plan.uiConsumed()), wm->queueSize(),
                static_cast<unsigned>(wm->mouseButtons()), cur_x, cur_y, router.phases.size(),
                router.px.empty() ? -1 : router.px[0], router.py.empty() ? -1 : router.py[0]);
}

// 跑一遍"arm → press → release"的确定性场景，返回 guest 消息队列的逐条指纹。
std::string runClickScenario(lift::UiInputRouter* router, std::uint64_t* fired, std::uint64_t* consumed,
                             bool* ok) {
    *ok = false;
    *fired = 0;
    *consumed = 0;
    lift_fixture::Fixture f;
    if (!f.ok) return std::string();
    lift::WindowManager* wm = f.m.cpu().windows;
    if (wm == nullptr) return std::string();
    if (wm->createWindow("PlanTest", 0u, 0u, 0u, 0u, 1280, 720, 0u) == 0) return std::string();
    if (router != nullptr) wm->setUiInputRouter(router);
    lift::InputPlan plan;
    plan.setClockForTest(&testNowMs);
    g_now_ms = 2000;
    std::string err;
    if (!plan.loadText("click 10 20\n", "mem", &err)) return std::string();
    plan.atPoint(*wm, lift::InputPoint::CursorPos);  // 臂上
    plan.atPoint(*wm, lift::InputPoint::CursorPos);  // 按下（跨过一个完成的位置读点）
    for (std::uint64_t i = 0; i < lift::InputPlan::kKeyLevelReadsBeforeRelease; ++i) {
        plan.atPoint(*wm, lift::InputPoint::KeyLevel);  // 抬起
    }
    *fired = plan.fired();
    *consumed = plan.uiConsumed();
    *ok = true;
    return dumpQueue(*wm);
}

void caseRouterMissIdentical() {
    lift_test::g_current_case = "input-plan/click-router-miss-identical";
    // ---- 对照组：**没有** router ----
    std::uint64_t fired_a = 0;
    std::uint64_t consumed_a = 0;
    bool ok_a = false;
    const std::string seq_a = runClickScenario(nullptr, &fired_a, &consumed_a, &ok_a);
    LIFT_CHECK(ok_a);
    // ---- 实验组：装了 router，但 click 落在命中区外（未命中 ⇒ 放行）----
    RecordingRouter miss;
    miss.x = 200;
    miss.y = 200;
    miss.w = 50;
    miss.h = 50;
    std::uint64_t fired_b = 0;
    std::uint64_t consumed_b = 0;
    bool ok_b = false;
    const std::string seq_b = runClickScenario(&miss, &fired_b, &consumed_b, &ok_b);
    LIFT_CHECK(ok_b);
    // 两条序列必须**逐位相同**（同一条 click 的 move / down / up 三元组）。
    LIFT_CHECK(seq_a == seq_b);
    LIFT_CHECK(!seq_a.empty());  // 真的产生了消息（否则"相同"是空的）
    LIFT_CHECK(seq_a.find("0201/") != std::string::npos);  // WM_LBUTTONDOWN
    LIFT_CHECK(seq_a.find("0202/") != std::string::npos);  // WM_LBUTTONUP
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fired_a), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(fired_b), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(consumed_a), 0);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(consumed_b), 0);  // 未命中 ⇒ 没有吞
    // 未命中的 click 仍要问 router（Down + Up 各一次）——证明"放行"不是"没接"。
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(miss.down_count), 1);
    LIFT_CHECK_EQ_I64(static_cast<std::int64_t>(miss.up_count), 1);
    std::printf("[click-router-miss-identical] seq_equal=%d fired(no_router/miss)=%llu/%llu "
                "ui_consumed(no_router/miss)=%llu/%llu seq=%s\n",
                seq_a == seq_b ? 1 : 0, static_cast<unsigned long long>(fired_a),
                static_cast<unsigned long long>(fired_b), static_cast<unsigned long long>(consumed_a),
                static_cast<unsigned long long>(consumed_b), seq_a.c_str());
}

}  // namespace

int main() {
    caseRelation();
    caseInstructionsAndClock();
    caseScriptGate();
    casePositionGate();
    caseStepTimeout();
    caseCompletion();
    caseProfileAndQuit();
    caseParseErrors();
    caseCoordinateScale();
    caseForcedRelease();
    caseTrace();
    caseMousePrimitives();
    caseGraphMouseActions();
    caseDrag();
    caseDragRepeat();
    caseOneWritePerReadPoint();
    caseTwoClicksSerialized();
    caseUiConsumedClick();
    caseRouterMissIdentical();
    return lift_test::finish("input-plan-read-points");
}
