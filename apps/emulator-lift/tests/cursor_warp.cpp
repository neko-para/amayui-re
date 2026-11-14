// emulator-lift · `SetCursorPos` 的宿主光标 warp（`--cursor-warp`，缺省关）
//
// 判据（红了意味着什么）：
//   ① `cursor-warp/off-moves-nothing`：**缺省档**（`hostCursorWarpEnabled()==false`）下
//      `SetCursorPos(320,240)` ⇒ `eax==1`、假 sink 的 `warp_calls==0`、`GetCursorPos` 读到 (320,240)。
//      红了 ⇒ 缺省档开始动宿主光标（**用户机器被副作用污染**），或虚拟位置不再更新（`GetCursorPos` 变常量）。
//   ② `cursor-warp/on-calls-once-with-client-coords`：开关打开（照 design §6.2）。
//      窗口原点取 **(100,50)**（非零！）：`SetCursorPos(320,240)` ⇒ `eax==1`、`warp_calls==1`、
//      记录值 == **(220,190)**（= guest 屏幕坐标 − 窗口原点）。
//      红了 ⇒ 开了却不挪 / 挪了两次 / **忘了减 guest 窗口原点**（这种错在 win.x=0 的真机上完全看不见）。
//   ③ `cursor-warp/warp-failure-is-false`：假 sink 的 `hostCursorWarp` 返回 false ⇒ `eax==0`，
//      且虚拟位置**仍是** (320,240)（裁决：开着却挪不动 = 响亮失败，但 guest 可见状态照改）。
//      红了 ⇒ "挪不动也报成功"（静默成功）。
//   ④ `cursor-warp/no-window-no-warp`：guest 一个窗口都没建 ⇒ `eax==1`、`warp_calls==0`。
//      红了 ⇒ 没有窗口还去挪宿主光标（会把光标送到 `SDL_GetWindowPosition` 的野值上）；
//      也守"没有可挪的对象不算失败"（它必须 TRUE）。
//   ⑤ `cursor-warp/no-clamp`：`SetCursorPos(-5,900)`（窗口在 (100,50)）⇒ 记录 **(-105,850)**。
//      红了 ⇒ 有人"顺手 clamp"到 0..1279（Win32 允许负值与越界 ⇒ 与文档不符，且多屏 / 窗口化会失效）。
//   ⑥ `warp-echo/*`：回灌抑制队列的纯语义（`runtime/warp_echo.h`，无窗口 / 无 SDL）：
//      `consume-once` / `fifo-two-targets` / `miss-keeps-queue` / `capacity-drops-oldest` /
//      `clear-forgets`。判据：**窗口客户区像素** FIFO、每条只消费一次、未命中不消费、满了丢最旧；
//      按 guest 坐标比就挡不住越界 warp（`mapMouse` 的 clamp 丢了信息）。
//   ⑦ `cursor-warp/two-warps-get-cursor-pos-second`：复审反例 A 的断言版（假 sink 复现
//      "warp 成功 ⇒ note；motion 入口 ⇒ consume"两步）：同一帧两次 warp 之后 `GetCursorPos`
//      仍是第二次的值，两条回灌都不产生 `WM_MOUSEMOVE`。红了 ⇒ 单槽覆盖（#1 被当真人移动接受）。
//   ⑧ `cursor-warp/out-of-image-echo-window-pixels`：复审反例 B —— 1600×720 窗口 + 1280×720 image、
//      `SetCursorPos(1300,300)`；回灌经 `mapMouse` 会 clamp 到 1279，按窗口像素比必须吞，
//      `GetCursorPos` 仍是 (1300,300)。红了 ⇒ 匹配点退回 guest 坐标。
//
// ★ 本用例**不建真窗口、不碰真光标**：warp 一律走假 `PresentSink`（默认实现返回 false，
//   这里 override 成"记一笔"）。⇒ 可以在 `SDL_VIDEODRIVER=dummy` / 无桌面上跑。
// ★ 假 sink 的 `hostCursorWarp()` **无论 enabled 与否都计数**（shim 会先问 `hostCursorWarpEnabled()`）。
//   没有这条前提，① 就是恒真断言（"没调用"可能只是因为假 sink 自己没数）。
//
// ★ 为什么 ① 与 ② 必须成对存在：① 单独看今天就是绿的（现在根本没有 warp 概念）⇒ 它不能单独当守卫；
//   它是 ② 的**对照臂**（同一条路径上，开关真的是开关）。
//
// ⑨ `cursor-map/letterbox-inverse`：`runtime/cursor_map.h` 的纯几何（无窗口 / 无 sink）。
//    ★ 期望值**按 `letterboxFor(window_w, window_h, image_w, image_h)` 重新算过**（设计稿 §6.1
//      case 6 有一处自相矛盾：`scale=1, off=0` 时 `(640,360)` 不可能变成 `(64,36)`）。
//    ★ 其中"同一窗口 (1280,720)、image 换成 (640,480) ⇒ scale=1.5 / off_x=160"这一格就是
//      **签名必须带 image 尺寸**的证据（缺了它就只能硬编码 1280×720）。
//    红了 ⇒ 反函数漂了（与 `mapMouse` 不再是同一份公式）⇒ 非 1:1 窗口 / 用户拖动窗口后光标偏移。
// ⑩ `cursor-map/round-trip`：窗口 → 画面 → 窗口在整数缩放下逐位闭合（含 letterbox 偏移）。
//    红了 ⇒ 两个方向各写一遍的漂法（单独绿、合起来不闭合）。
// ⑪ `cursor-map/window-pixel-to-image`：`mapMouse` 唯一的组合入口（`mapWindowPixelToImage`）。
//    三格：窗口==image 恒等 · 1280×720 窗口 + 640×480 image ⇒ (320,240)→(107,160) ·
//    1600×720 窗口 + 1280×720 image ⇒ (800,360)→(640,360)。红了 ⇒ **image 尺寸被写回常数**
//    （复审变异实测：改回 1280×720 时 84/84 全绿 ⇒ 这条就是补那个缺口）。
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

#include "check.h"
#include "fixture.h"
#include "runtime/cursor_map.h"
#include "runtime/host_window.h"
#include "runtime/input.h"
#include "runtime/machine.h"
#include "runtime/present.h"
#include "runtime/warp_echo.h"
#include "shims/register.h"

namespace {

// 宿主光标的**测试替身**：只记调用，不碰 OS。
// ★ `model_echo` 打开时，`hostCursorWarp` 成功之后**自己**登记回灌期望 —— 与生产代码
//   （`present_sdl.cpp` 的 `SdlWindow::hostCursorWarp`）同一步：用 `cursor_map` 把 guest 客户区
//   坐标换成**窗口客户区像素**再进 `WarpEchoQueue`。夹具因此能复现"sink 看得到的那一层"。
class FakeCursorSink final : public lift::PresentSink {
public:
    bool submitFrame(const lift::SurfaceArena&, const lift::GuestSurface&, std::string*) override { return true; }
    bool hostCursorWarpEnabled() const override { return enabled; }
    bool hostCursorWarp(int x, int y) override {
        // ★ 计数器**不看 enabled**：① 的"没调用"必须来自 shim 的开关判定，而不是替身自己偷懒。
        ++warp_calls;
        last_x = x;
        last_y = y;
        if (!warp_returns) return false;
        if (model_echo) {
            int wx = 0;
            int wy = 0;
            if (lift::imageToWindowPixel(map, x, y, &wx, &wy)) echo.note(wx, wy);
        }
        return true;
    }

    bool enabled = false;
    bool warp_returns = true;
    int warp_calls = 0;
    int last_x = 0;
    int last_y = 0;
    // 回灌仿真的两件道具（见类注释；默认关 ⇒ 旧用例行为不变）。
    bool model_echo = false;
    lift::LetterboxMap map;  // 窗口客户区 ↔ image（image = guest 客户区尺寸 ⇒ 与 mapMouse 同口径）
    lift::WarpEchoQueue echo;
};

// 夹具 + 已注册的 user32 输入族 shim（SetCursorPos / GetCursorPos 都是**真语义**）。
struct CursorFixture {
    lift_fixture::Fixture f;
    FakeCursorSink* sink = nullptr;  // 归 `f.m.presenter()` 所有（unique_ptr）
    std::uint32_t set_stub = 0;
    std::uint32_t get_stub = 0;

    explicit CursorFixture(bool with_sink) {
        lift::registerUser32InputShims(f.m.shims());
        set_stub = f.use("user32", "SetCursorPos");
        get_stub = f.use("user32", "GetCursorPos");
        if (with_sink) {
            auto owned = std::make_unique<FakeCursorSink>();
            sink = owned.get();
            f.m.presenter().setSink(std::move(owned));
        }
    }

    std::uint32_t setCursorPos(int x, int y) {
        return f.call(set_stub, {static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)});
    }

    // 走真 `GetCursorPos`（guest 内存里的 POINT），读回 (x,y)。
    void getCursorPos(int* x, int* y) {
        const std::uint32_t pt = f.zeroData(8);
        const std::uint32_t rc = f.call(get_stub, {pt});
        LIFT_CHECK_EQ_U32(rc, 1u);
        *x = static_cast<int>(static_cast<std::int32_t>(f.m.mem().rd32(pt)));
        *y = static_cast<int>(static_cast<std::int32_t>(f.m.mem().rd32(pt + 4)));
    }
};

// 仿真 `present_sdl.cpp` 的 `SDL_EVENT_MOUSE_MOTION` 分支：生产顺序是**先**过回灌队列。
//   命中 ⇒ 整条丢弃（⛔ 不 mapMouse、不 input::mouseMove ⇒ 连 `WM_MOUSEMOVE` 都不发）。
//   未命中 ⇒ 与生产一样：窗口像素 → guest 客户区（同一份 `mapWindowPixelToImage`）再 `input::mouseMove`。
// ★ 队列用的是 FakeCursorSink 里那一份（与生产**同一个** `WarpEchoQueue` 类）⇒ 这组用例钉的是
//   "两层怎么接起来"，而不是重写公式。
bool deliverMotion(CursorFixture& c, int window_w, int window_h, int image_w, int image_h, double wx, double wy) {
    if (c.sink->echo.consume(wx, wy)) return false;
    int gx = 0;
    int gy = 0;
    if (!lift::mapWindowPixelToImage(window_w, window_h, image_w, image_h, wx, wy, &gx, &gy)) {
        gx = static_cast<int>(wx);
        gy = static_cast<int>(wy);
    }
    return lift::input::mouseMove(*c.f.m.cpu().windows, gx, gy, lift::InputSource::Os, 0);
}

void caseOffMovesNothing() {
    lift_test::g_current_case = "cursor-warp/off-moves-nothing";
    CursorFixture c(true);
    LIFT_CHECK(c.f.ok);
    if (!c.f.ok) return;
    // 有窗口（非零原点）也**不**挪 ⇒ 开关真的是开关，而不是"没窗口所以没挪"。
    const std::uint32_t hwnd = c.f.m.cpu().windows->createWindow("WarpOff", 0x00C00000u, 0, 100, 50, 1280, 720, 0);
    LIFT_CHECK_NE_U32(hwnd, 0u);
    c.sink->enabled = false;

    LIFT_CHECK_EQ_U32(c.setCursorPos(320, 240), 1u);
    LIFT_CHECK_EQ_I64(c.sink->warp_calls, 0);
    int x = -1;
    int y = -1;
    c.getCursorPos(&x, &y);
    LIFT_CHECK_EQ_I64(x, 320);
    LIFT_CHECK_EQ_I64(y, 240);
}

void caseOnCallsOnceWithClientCoords() {
    lift_test::g_current_case = "cursor-warp/on-calls-once-with-client-coords";
    CursorFixture c(true);
    LIFT_CHECK(c.f.ok);
    if (!c.f.ok) return;
    // ★ 非零窗口原点 **(100,50)**：忘掉"减 guest 窗口原点"的实现在 (0,0) 窗口上完全看不出来。
    const std::uint32_t hwnd = c.f.m.cpu().windows->createWindow("WarpOn", 0x00C00000u, 0, 100, 50, 1280, 720, 0);
    LIFT_CHECK_NE_U32(hwnd, 0u);
    c.sink->enabled = true;

    LIFT_CHECK_EQ_U32(c.setCursorPos(320, 240), 1u);
    LIFT_CHECK_EQ_I64(c.sink->warp_calls, 1);  // 多了 / 少了都红
    LIFT_CHECK_EQ_I64(c.sink->last_x, 220);    // 320 − 100
    LIFT_CHECK_EQ_I64(c.sink->last_y, 190);    // 240 − 50
    // guest 可见状态逐位不变：它看到的是**屏幕坐标**，不是客户区坐标。
    int x = -1;
    int y = -1;
    c.getCursorPos(&x, &y);
    LIFT_CHECK_EQ_I64(x, 320);
    LIFT_CHECK_EQ_I64(y, 240);
}

void caseWarpFailureIsFalse() {
    lift_test::g_current_case = "cursor-warp/warp-failure-is-false";
    CursorFixture c(true);
    LIFT_CHECK(c.f.ok);
    if (!c.f.ok) return;
    const std::uint32_t hwnd = c.f.m.cpu().windows->createWindow("WarpFail", 0x00C00000u, 0, 100, 50, 1280, 720, 0);
    LIFT_CHECK_NE_U32(hwnd, 0u);
    c.sink->enabled = true;
    c.sink->warp_returns = false;

    LIFT_CHECK_EQ_U32(c.setCursorPos(320, 240), 0u);  // ★ 开着却挪不动 ⇒ FALSE（⛔ 不假装成功）
    LIFT_CHECK_EQ_I64(c.sink->warp_calls, 1);
    // 已裁决的不对称：**虚拟位置照改**（它改的是 guest 自己的状态，且后续读点立刻能看到）。
    int x = -1;
    int y = -1;
    c.getCursorPos(&x, &y);
    LIFT_CHECK_EQ_I64(x, 320);
    LIFT_CHECK_EQ_I64(y, 240);
}

void caseNoWindowNoWarp() {
    lift_test::g_current_case = "cursor-warp/no-window-no-warp";
    CursorFixture c(true);
    LIFT_CHECK(c.f.ok);
    if (!c.f.ok) return;
    LIFT_CHECK_EQ_U32(c.f.m.cpu().windows->primaryWindow(), 0u);  // 前置：一个 guest 窗口都没有
    c.sink->enabled = true;

    // 没有可挪的对象 ⇒ 只改虚拟位置、**返回 TRUE**（不尝试 warp ⇒ 不算失败）。
    LIFT_CHECK_EQ_U32(c.setCursorPos(320, 240), 1u);
    LIFT_CHECK_EQ_I64(c.sink->warp_calls, 0);
    int x = -1;
    int y = -1;
    c.getCursorPos(&x, &y);
    LIFT_CHECK_EQ_I64(x, 320);
    LIFT_CHECK_EQ_I64(y, 240);
}

void caseNoClamp() {
    lift_test::g_current_case = "cursor-warp/no-clamp";
    CursorFixture c(true);
    LIFT_CHECK(c.f.ok);
    if (!c.f.ok) return;
    const std::uint32_t hwnd = c.f.m.cpu().windows->createWindow("WarpNeg", 0x00C00000u, 0, 100, 50, 1280, 720, 0);
    LIFT_CHECK_NE_U32(hwnd, 0u);
    c.sink->enabled = true;

    // Win32：屏幕坐标可为负 / 越出 1280×720（多显示器 / 窗口化）⇒ 全链**不 clamp**。
    LIFT_CHECK_EQ_U32(c.setCursorPos(-5, 900), 1u);
    LIFT_CHECK_EQ_I64(c.sink->warp_calls, 1);
    LIFT_CHECK_EQ_I64(c.sink->last_x, -105);  // -5 − 100
    LIFT_CHECK_EQ_I64(c.sink->last_y, 850);   // 900 − 50
}

// ---- `warp_echo`：回灌抑制队列的**纯**语义（`runtime/warp_echo.h`；无窗口、无 SDL）----
// 判据（红了意味着什么）：warp 之后 OS 回灌的 motion 必须被吞掉、且**只吞它自己那一条**。
//   ① `warp-echo/consume-once`：一条只命中一次；±2 px 内命中，>2 px 不命中。
//      红了 ⇒ 回灌被吞多次（真人的后续移动被吃掉）或容差写错。
//   ② `warp-echo/fifo-two-targets`：同一帧两次 warp ⇒ 两条都还在（后一条**不覆盖**前一条）；
//      先到的 #1 也能命中。红了 ⇒ 退回单槽（复审反例 A：第一条回灌被当成真人移动接受）。
//   ③ `warp-echo/miss-keeps-queue`：未命中**不消费**（别人的期望不能被顺带清掉）。
//   ④ `warp-echo/capacity-drops-oldest`：容量 8，塞 9 条丢最旧、其余仍在。
//      红了 ⇒ 队列无界（悬挂标记无限积累）或丢错端（新的被丢 ⇒ 真回灌永远等不到）。
//   ⑤ `warp-echo/clear-forgets`：真人动作后 `clear()` ⇒ 旧期望作废。
void caseWarpEchoConsumeOnce() {
    lift_test::g_current_case = "warp-echo/consume-once";
    lift::WarpEchoQueue q;
    q.note(100, 200);
    LIFT_CHECK(q.consume(100.0, 200.0));   // 正中
    LIFT_CHECK(!q.consume(100.0, 200.0));  // 每条只消费一次
    q.note(300, 400);
    LIFT_CHECK(q.consume(302.0, 398.0));   // 容差内（±2）
    LIFT_CHECK(!q.consume(303.0, 403.0));  // 超出容差（3 px）⇒ 不命中
}

void caseWarpEchoFifoTwoTargets() {
    lift_test::g_current_case = "warp-echo/fifo-two-targets";
    lift::WarpEchoQueue q;
    q.note(220, 190);  // warp #1 的窗口像素落点
    q.note(500, 350);  // warp #2（同一帧）—— ⛔ 不许覆盖 #1
    // 复审反例 A：回灌 #1 先到；单槽实现会把它当真人移动接受。
    LIFT_CHECK(q.consume(220.0, 190.0));
    LIFT_CHECK(q.consume(500.0, 350.0));  // #2 也还在
}

void caseWarpEchoMissKeepsQueue() {
    lift_test::g_current_case = "warp-echo/miss-keeps-queue";
    lift::WarpEchoQueue q;
    q.note(220, 190);
    LIFT_CHECK(!q.consume(300.0, 300.0));  // 非命中：不动队列
    LIFT_CHECK(q.consume(220.0, 190.0));   // 旧期望还在 ⇒ 真回灌仍会被吞
}

void caseWarpEchoCapacity() {
    lift_test::g_current_case = "warp-echo/capacity-drops-oldest";
    lift::WarpEchoQueue q;
    for (int i = 0; i < lift::WarpEchoQueue::kCapacity + 1; ++i) q.note(i * 10, i * 10);
    LIFT_CHECK(!q.consume(0.0, 0.0));   // 最旧那条被丢
    LIFT_CHECK(q.consume(10.0, 10.0));  // 第 2 条还在
    LIFT_CHECK(q.consume(80.0, 80.0));  // 最新一条还在（容量内的最后一条）
}

void caseWarpEchoClear() {
    lift_test::g_current_case = "warp-echo/clear-forgets";
    lift::WarpEchoQueue q;
    q.note(220, 190);
    q.clear();  // 按键 / 滚轮 / 键盘 ⇒ 旧期望作废
    LIFT_CHECK(!q.consume(220.0, 190.0));
}

// ---- 回灌抑制的**接线**（假 sink 复现 `present_sdl` 的两步：warp 成功 ⇒ note；motion 入口 ⇒ consume）----
// ① `cursor-warp/two-warps-get-cursor-pos-second`：复审反例 A 的断言版 —— 同一帧两次 warp
//    （`SetCursorPos(320,240)` → `SetCursorPos(600,400)`）之后，先到的 #1 回灌（窗口像素 (220,190)）
//    必须被吞，`GetCursorPos` **仍是第二次的值 (600,400)**、队列里不多 `WM_MOUSEMOVE`。
//    红了 ⇒ 单槽（#1 被 #2 覆盖）⇒ 回灌把虚拟光标反写成第一条 warp。
// ② `cursor-warp/out-of-image-echo-window-pixels`：复审反例 B —— 窗口 1600×720（pillarbox、
//    image 1280×720）、`SetCursorPos(1300,300)`。warp 落在窗口像素 (1460,300)；这条回灌若先过
//    `mapMouse` 会 clamp 到 1279（差 21 px）⇒ 在 guest 坐标里比永远对不上。按**窗口像素**比必须吞，
//    `GetCursorPos` 仍是 (1300,300)。红了 ⇒ 匹配点退回 guest 坐标（`mapWindowPixelToImage` 的 clamp
//    已经丢了信息）。
void caseTwoWarpsKeepSecondCursorPos() {
    lift_test::g_current_case = "cursor-warp/two-warps-get-cursor-pos-second";
    CursorFixture c(true);
    LIFT_CHECK(c.f.ok);
    if (!c.f.ok) return;
    lift::WindowManager* wm = c.f.m.cpu().windows;
    const std::uint32_t hwnd = wm->createWindow("WarpEchoTwo", 0x00C00000u, 0, 100, 50, 1280, 720, 0);
    LIFT_CHECK_NE_U32(hwnd, 0u);
    c.sink->enabled = true;
    c.sink->model_echo = true;
    // guest 客户区 1280×720 == image ⇒ 窗口像素与 guest 客户区同一坐标（identity）。
    c.sink->map = lift::letterboxFor(1280, 720, 1280, 720);

    LIFT_CHECK_EQ_U32(c.setCursorPos(320, 240), 1u);  // warp #1 ⇒ note(窗口像素 220,190)
    LIFT_CHECK_EQ_I64(c.sink->last_x, 220);
    LIFT_CHECK_EQ_I64(c.sink->last_y, 190);
    LIFT_CHECK_EQ_U32(c.setCursorPos(600, 400), 1u);  // warp #2（同一帧）⇒ note(500,350)
    LIFT_CHECK_EQ_I64(c.sink->last_x, 500);
    LIFT_CHECK_EQ_I64(c.sink->last_y, 350);
    int x = -1;
    int y = -1;
    c.getCursorPos(&x, &y);
    LIFT_CHECK_EQ_I64(x, 600);  // 两次 warp 之后 = 第二次的值
    LIFT_CHECK_EQ_I64(y, 400);
    const std::size_t q0 = wm->queueSize();

    // 回灌 #1（先到）：窗口像素 (220,190) ⇒ 必须吞（不 mapMouse、不发 WM_MOUSEMOVE）。
    LIFT_CHECK(!deliverMotion(c, 1280, 720, 1280, 720, 220.0, 190.0));
    // 回灌 #2。
    LIFT_CHECK(!deliverMotion(c, 1280, 720, 1280, 720, 500.0, 350.0));
    LIFT_CHECK_EQ_U32(wm->queueSize(), q0);  // 两条回灌都没变成消息
    c.getCursorPos(&x, &y);
    LIFT_CHECK_EQ_I64(x, 600);  // guest 可见状态逐位不变
    LIFT_CHECK_EQ_I64(y, 400);

    // 真人紧接着的移动必须照旧生效（⛔ 不做长期去抖）。
    LIFT_CHECK(deliverMotion(c, 1280, 720, 1280, 720, 300.0, 250.0));
    LIFT_CHECK_EQ_U32(wm->queueSize(), q0 + 1u);
    c.getCursorPos(&x, &y);
    LIFT_CHECK_EQ_I64(x, 400);  // 300 + 100（窗口原点）
    LIFT_CHECK_EQ_I64(y, 300);  // 250 + 50
}

void caseOutOfImageEchoWindowPixels() {
    lift_test::g_current_case = "cursor-warp/out-of-image-echo-window-pixels";
    CursorFixture c(true);
    LIFT_CHECK(c.f.ok);
    if (!c.f.ok) return;
    lift::WindowManager* wm = c.f.m.cpu().windows;
    // 窗口在 (0,0)：guest 屏幕坐标 == 客户区坐标，算式只剩 letterbox。
    const std::uint32_t hwnd = wm->createWindow("WarpEchoPillar", 0x00C00000u, 0, 0, 0, 1600, 720, 0);
    LIFT_CHECK_NE_U32(hwnd, 0u);
    c.sink->enabled = true;
    c.sink->model_echo = true;
    // 1600×720 窗口 + 1280×720 image：scale=1、off_x=(1600−1280)/2=160。
    c.sink->map = lift::letterboxFor(1600, 720, 1280, 720);

    LIFT_CHECK_EQ_U32(c.setCursorPos(1300, 300), 1u);
    LIFT_CHECK_EQ_I64(c.sink->last_x, 1300);  // guest 客户区（窗口在 0,0 ⇒ = 屏幕坐标）
    LIFT_CHECK_EQ_I64(c.sink->last_y, 300);
    // warp 落点 = 窗口像素 (160+1300, 300) = (1460,300)。这条回灌**过 mapMouse** 会 clamp 到
    // image 右缘 1279（复现复审反例 B 的"差 21 px"）—— 所以匹配必须在 clamp 之前的那一对方位做。
    int clamped_x = -1;
    int clamped_y = -1;
    LIFT_CHECK(lift::mapWindowPixelToImage(1600, 720, 1280, 720, 1460.0, 300.0, &clamped_x, &clamped_y));
    LIFT_CHECK_EQ_I64(clamped_x, 1279);
    LIFT_CHECK_EQ_I64(clamped_y, 300);
    const std::size_t q0 = wm->queueSize();

    // 回灌按**窗口像素** (1460,300) 到达 ⇒ 必须吞掉。
    LIFT_CHECK(!deliverMotion(c, 1600, 720, 1280, 720, 1460.0, 300.0));
    LIFT_CHECK_EQ_U32(wm->queueSize(), q0);
    int x = -1;
    int y = -1;
    c.getCursorPos(&x, &y);
    LIFT_CHECK_EQ_I64(x, 1300);  // ⛔ 不被改写成 1279
    LIFT_CHECK_EQ_I64(y, 300);

    // 真人移动照旧（落在左黑边里 ⇒ clamp 到画面左缘，这是 `mapMouse` 的既有口径）。
    LIFT_CHECK(deliverMotion(c, 1600, 720, 1280, 720, 0.0, 100.0));
    LIFT_CHECK_EQ_U32(wm->queueSize(), q0 + 1u);
    c.getCursorPos(&x, &y);
    LIFT_CHECK_EQ_I64(x, 0);
    LIFT_CHECK_EQ_I64(y, 100);
}

// ---- 纯几何：letterbox 的**唯一一份**公式（`runtime/cursor_map.h`；无窗口、无 sink）----
bool approxEqual(double a, double b) { return std::fabs(a - b) < 1e-9; }

void checkMap(const lift::LetterboxMap& map, double scale, double ox, double oy, const char* what) {
    ++lift_test::g_checks;
    if (!approxEqual(map.scale, scale) || !approxEqual(map.off_x, ox) || !approxEqual(map.off_y, oy)) {
        lift_test::failf(__FILE__, __LINE__, "%s: expected scale=%.6f off=(%.3f,%.3f), got scale=%.6f off=(%.3f,%.3f)",
                         what, scale, ox, oy, map.scale, map.off_x, map.off_y);
    }
}

void checkImageToWindow(const lift::LetterboxMap& map, int ix, int iy, int ex, int ey, const char* what) {
    int wx = 0;
    int wy = 0;
    LIFT_CHECK(lift::imageToWindowPixel(map, ix, iy, &wx, &wy));
    ++lift_test::g_checks;
    if (wx != ex || wy != ey) {
        lift_test::failf(__FILE__, __LINE__, "%s: image(%d,%d) expected window(%d,%d), got (%d,%d)", what, ix, iy, ex,
                         ey, wx, wy);
    }
}

void checkWindowToImage(const lift::LetterboxMap& map, double mx, double my, int ex, int ey, const char* what) {
    int ix = 0;
    int iy = 0;
    LIFT_CHECK(lift::windowPixelToImage(map, mx, my, &ix, &iy));
    ++lift_test::g_checks;
    if (ix != ex || iy != ey) {
        lift_test::failf(__FILE__, __LINE__, "%s: window(%.3f,%.3f) expected image(%d,%d), got (%d,%d)", what, mx, my,
                         ex, ey, ix, iy);
    }
}

void caseLetterboxInverse() {
    lift_test::g_current_case = "cursor-map/letterbox-inverse";
    // 每格的期望值 = min(ww/iw, wh/ih) 与居中偏移，逐个算出来的（不是抄设计稿）。
    const lift::LetterboxMap identity = lift::letterboxFor(1280, 720, 1280, 720);
    checkMap(identity, 1.0, 0.0, 0.0, "1280x720 window + 1280x720 image");
    const lift::LetterboxMap half = lift::letterboxFor(640, 360, 1280, 720);
    checkMap(half, 0.5, 0.0, 0.0, "640x360 window + 1280x720 image");
    const lift::LetterboxMap pillarbox = lift::letterboxFor(1600, 720, 1280, 720);
    checkMap(pillarbox, 1.0, 160.0, 0.0, "1600x720 window ⇒ 左右各 160 黑边");
    const lift::LetterboxMap letterbox = lift::letterboxFor(1280, 900, 1280, 720);
    checkMap(letterbox, 1.0, 0.0, 90.0, "1280x900 window ⇒ 上下各 90 黑边");
    // ★ 同一窗口 + **不同 image 尺寸** ⇒ scale / off_x 都变（签名必须带 image 尺寸的证据）。
    const lift::LetterboxMap tallImage = lift::letterboxFor(1280, 720, 640, 480);
    checkMap(tallImage, 1.5, 160.0, 0.0, "1280x720 window + 640x480 image（scale=min(2,1.5)=1.5）");

    // 画面 → 窗口：不 clamp（负 / 越界原样线性映射）
    checkImageToWindow(identity, 640, 360, 640, 360, "identity 中心");
    checkImageToWindow(identity, -5, 900, -5, 900, "identity 负 / 越界不 clamp");
    checkImageToWindow(half, 2, 2, 1, 1, "scale=0.5");
    checkImageToWindow(pillarbox, 0, 0, 160, 0, "pillarbox 左上 = 黑边右缘");
    checkImageToWindow(pillarbox, 640, 360, 800, 360, "pillarbox 中心");
    checkImageToWindow(letterbox, 0, 0, 0, 90, "letterbox 左上 = 黑边下缘");
    checkImageToWindow(letterbox, 640, 360, 640, 450, "letterbox 中心");
    checkImageToWindow(tallImage, 0, 0, 160, 0, "640x480 image 左上");
    checkImageToWindow(tallImage, 640, 480, 1120, 720, "640x480 image 右下（image 边界）");

    // 窗口 → 画面：取整后 clamp 到 0..image-1（黑边 / 窗口外落到最近的画面边缘）
    checkWindowToImage(identity, 0, 0, 0, 0, "identity 左上");
    checkWindowToImage(identity, 1279, 719, 1279, 719, "identity 右下");
    checkWindowToImage(identity, -100, 2000, 0, 719, "identity 越界 ⇒ clamp");
    checkWindowToImage(half, 320, 180, 640, 360, "scale=0.5 中心");
    checkWindowToImage(half, 639, 359, 1278, 718, "scale=0.5 右下");
    checkWindowToImage(half, 1000, 1000, 1279, 719, "scale=0.5 越界 ⇒ clamp");
    checkWindowToImage(pillarbox, 160, 0, 0, 0, "pillarbox 黑边右缘 = image x0");
    checkWindowToImage(pillarbox, 0, 0, 0, 0, "pillarbox 左上黑边 ⇒ clamp 到 (0,0)");
    checkWindowToImage(tallImage, 160, 0, 0, 0, "640x480 image 左上");
    checkWindowToImage(tallImage, 1120, 720, 639, 479, "640x480 image 右下越界 ⇒ clamp 到 image-1");

    // 非正尺寸 ⇒ 几何不可用（scale=0；两个方向都返回 false，且不写输出）
    const lift::LetterboxMap degenerate = lift::letterboxFor(0, 720, 1280, 720);
    checkMap(degenerate, 0.0, 0.0, 0.0, "window_w=0");
    const lift::LetterboxMap badImage = lift::letterboxFor(1280, 720, 0, 720);
    checkMap(badImage, 0.0, 0.0, 0.0, "image_w=0");
    int wx = 7;
    int wy = 7;
    LIFT_CHECK(!lift::imageToWindowPixel(degenerate, 0, 0, &wx, &wy));
    LIFT_CHECK_EQ_I64(wx, 7);  // 失败不写输出
    int ix = 7;
    int iy = 7;
    LIFT_CHECK(!lift::windowPixelToImage(badImage, 0, 0, &ix, &iy));
    LIFT_CHECK_EQ_I64(ix, 7);
}

void caseRoundTrip() {
    lift_test::g_current_case = "cursor-map/round-trip";
    // 整数 scale（1 / 2）⇒ 两个方向逐位闭合；offset 覆盖 0 / 左右黑边 / 上下黑边。
    const int windows[][2] = {{1280, 720}, {2560, 1440}, {1600, 720}, {1280, 900}};
    const int xs[] = {0, 1, 2, 639, 640, 1278, 1279};
    const int ys[] = {0, 1, 359, 360, 718, 719};
    for (const auto& win : windows) {
        const lift::LetterboxMap map = lift::letterboxFor(win[0], win[1], 1280, 720);
        for (const int gx : xs) {
            for (const int gy : ys) {
                int wx = 0;
                int wy = 0;
                if (!lift::imageToWindowPixel(map, gx, gy, &wx, &wy)) {
                    LIFT_CHECK(false);
                    continue;
                }
                int rx = -1;
                int ry = -1;
                LIFT_CHECK(lift::windowPixelToImage(map, static_cast<double>(wx), static_cast<double>(wy), &rx,
                                                   &ry));
                LIFT_CHECK_EQ_I64(rx, gx);
                LIFT_CHECK_EQ_I64(ry, gy);
            }
        }
    }
    // 非整数 scale：只要求**正向线性 + 端点**（像素对像素的往返在缩小档本来就不闭合）。
    const lift::LetterboxMap fractional = lift::letterboxFor(1920, 1080, 1280, 720);
    checkMap(fractional, 1.5, 0.0, 0.0, "1920x1080 window + 1280x720 image");
    checkImageToWindow(fractional, 0, 0, 0, 0, "scale=1.5 左上");
    checkImageToWindow(fractional, 1279, 719, 1919, 1079, "scale=1.5 右下");
}

// ---- `cursor_map` 的**组合入口**（`mapMouse` 只调它；window 尺寸 / **image 尺寸**都在签名里）----
// 判据（红了意味着什么）：`mapMouse` 的 image 尺寸一旦被写回常数（1280×720），这三格当场红 ——
// 复审实测：把 `mapMouse` 里的 `letterboxFor(ww, wh, frameImageW(), frameImageH())` 变异回
// `(…,1280,720)` 时 84/84 全绿（阻 3）。现在 `mapMouse` 只经 `mapWindowPixelToImage` 走，
// 而这里直接钉住"image 尺寸真的改变结果"。
void checkWindowPixelToImage(int ww, int wh, int iw, int ih, double mx, double my, int ex, int ey,
                             const char* what) {
    int ix = 0;
    int iy = 0;
    const bool ok = lift::mapWindowPixelToImage(ww, wh, iw, ih, mx, my, &ix, &iy);
    ++lift_test::g_checks;
    if (!ok || ix != ex || iy != ey) {
        lift_test::failf(__FILE__, __LINE__,
                         "%s: %dx%d window + %dx%d image, window(%.1f,%.1f) expected image(%d,%d), got (%d,%d)",
                         what, ww, wh, iw, ih, mx, my, ex, ey, ix, iy);
    }
}

void caseMapWindowPixelToImage() {
    lift_test::g_current_case = "cursor-map/window-pixel-to-image";
    // ① 窗口 == image ⇒ 恒等（scale=1、off=0；取整与 clamp 都不改变它）。
    checkWindowPixelToImage(640, 480, 640, 480, 320.0, 240.0, 320, 240, "640x480 窗口 + 640x480 image 恒等");
    // ② 1280×720 窗口 + 640×480 image（**非 720p 的帧**）：
    //    scale = min(1280/640, 720/480) = min(2, 1.5) = 1.5
    //    off_x = (1280 − 640×1.5)/2 = (1280 − 960)/2 = 160；off_y = (720 − 480×1.5)/2 = 0
    //    x = lround((320 − 160)/1.5) = lround(106.666…) = 107
    //    y = lround((240 − 0)/1.5)  = 160
    //    clamp 0..639 / 0..479 ⇒ (107,160)
    checkWindowPixelToImage(1280, 720, 640, 480, 320.0, 240.0, 107, 160,
                            "1280x720 窗口 + 640x480 image（scale=1.5 off_x=160）");
    // ③ 1600×720 窗口 + 1280×720 image（pillarbox）：
    //    scale = min(1600/1280, 720/720) = min(1.25, 1) = 1
    //    off_x = (1600 − 1280×1)/2 = 160；off_y = (720 − 720×1)/2 = 0
    //    x = lround((800 − 160)/1) = 640；y = lround((360 − 0)/1) = 360 ⇒ (640,360)
    checkWindowPixelToImage(1600, 720, 1280, 720, 800.0, 360.0, 640, 360, "1600x720 窗口 + 1280x720 image（off_x=160）");
    // 非正尺寸 ⇒ 几何不可用（与组合前的两段同一口径：返回 false、**不写输出**）。
    int ix = 7;
    int iy = 7;
    LIFT_CHECK(!lift::mapWindowPixelToImage(0, 720, 1280, 720, 0.0, 0.0, &ix, &iy));
    LIFT_CHECK_EQ_I64(ix, 7);
    LIFT_CHECK_EQ_I64(iy, 7);
}

}  // namespace

int main() {
    caseOffMovesNothing();
    caseOnCallsOnceWithClientCoords();
    caseWarpFailureIsFalse();
    caseNoWindowNoWarp();
    caseNoClamp();
    caseWarpEchoConsumeOnce();
    caseWarpEchoFifoTwoTargets();
    caseWarpEchoMissKeepsQueue();
    caseWarpEchoCapacity();
    caseWarpEchoClear();
    caseTwoWarpsKeepSecondCursorPos();
    caseOutOfImageEchoWindowPixels();
    caseLetterboxInverse();
    caseRoundTrip();
    caseMapWindowPixelToImage();
    return lift_test::finish("cursor-warp");
}
