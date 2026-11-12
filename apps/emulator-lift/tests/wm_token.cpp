// emulator-lift · **pure** 守卫：`WindowManager` 的**主线程亲和性**（REQ-01M76SXW0Y3R8QX1F94AB85M9H）
//
// ★ 这不是令牌（`guest_token.h` 那一套）。按 M0 的口径，`WindowManager` 是**主线程独占的宿主
//   对象**：块边界的 harness tick（`machine.cpp`）与 `GetMessageA` 的宿主事件泵
//   （`host_window.cpp` 的 YieldGuard）都在**合法让出令牌**的窗口里写它 ⇒ 在这里补
//   `guestToken().check()` 会把 M0 的设计判成 bug。所以本单守的是"**谁**在碰"，
//   不是"持不持牌"：非 owner 线程碰任何入口 ⇒ `offThread()` 计数 + 第一次 stderr 喊一行。
//
// 红了意味着什么（判据，缺一条都不算绿）：
//   ① `window-manager-token/main-thread-silent`：主线程正常用（`find` / `post`）⇒ `offThread()==0`
//      （正常路径一次也不记账；⛔ 这条同时是 ② 的负控 —— 否则"恒 +1"也能让 ② 绿）；
//   ② `window-manager-token/off-thread-find`：另起一条 `std::thread` 调 `find` ⇒ `offThread()>=1`
//      且 `firstOffThreadSite()` 含 `window.find`；★ 而且 `find` **照旧返回**窗口指针
//      （守卫只记账，⛔ 不改变返回值 / 行为）；
//   ③ `window-manager-token/site-granularity`：`resetOffThread()` 之后从 worker 碰**两个不同
//      入口**（`post` / `primaryWindow`）⇒ `>=2`、first site = 第一个 ⇒ 站点名粒度真的是
//      `window.<op>`，不是"一个 `window.manager` 糊全部"；
//   ④ `window-manager-token/unowned-silent`：**没有 owner** 的实例（显式
//      `setOwnerThread(std::thread::id{})`）⇒ worker 怎么碰都不计数（未设 owner 的纯夹具
//      **不制造假红**）。
//
// ★ 计数单位是"碰了几次入口"，不是"几次违规"：`find` 会被 `setCursorClientPos` /
//   `cursorClientPos` 嵌套调用，`get` 内部还会调 `pumpTimers` + `peek` ⇒ 断言只写 `>=1` / `>=2`，
//   ⛔ 不写死"每次恰好 +1"。
// ★ 不依赖 guest 内存 / `Machine` / 游戏安装 ⇒ 顶层无条件注册 = **pure** 档
//   （判据 = 注册位置，见 AGENTS.md §5）。`tools/mutate-check.mjs` 的
//   `ctest:window-manager-token` 指向它（删掉 `find` 入口的 `checkMainThread("window.find")`
//   ⇒ ② 红）。
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "check.h"
#include "runtime/host_window.h"

namespace {

// 在两个不同线程之间搬一个"worker 里看到的东西"（避免共享可变状态：断言用的值都在 join 之后读）。
struct WorkerFindResult {
    bool called = false;
    bool found = false;
};

void step(const char* what) { std::printf("[case] %s\n", what); }

}  // namespace

int main() {
    using namespace lift_test;

    // 用例名 = CTest 名（`window-manager-token`）：失败的 FAIL 行会带它。
    g_current_case = "window-manager-token";

    // 收尾要打印的读数（= 本轮真的记到的数，⛔ 不是常量）。
    unsigned long long off_find = 0;
    unsigned long long off_two = 0;
    const char* site_find = "-";

    // ---------------------------------------------------------------------------------
    // ① 主线程：正常用 ⇒ 一次也不计数
    // ---------------------------------------------------------------------------------
    step("main-thread-silent");
    {
        lift::WindowManager wm;  // owner = 构造线程（本线程）
        const std::uint32_t atom = wm.registerClass("WmTokenClass", 0x401000u, 0);
        LIFT_CHECK(atom != 0);
        const std::uint32_t hwnd = wm.createWindow("WmTokenClass", 0x00C00000u, 0, 0, 0, 320, 240, 0);
        LIFT_CHECK(hwnd != 0);
        LIFT_CHECK(wm.find(hwnd) != nullptr);
        lift::WinMessage msg;
        msg.hwnd = hwnd;
        msg.message = 0x0400u;
        wm.post(msg);
        LIFT_CHECK(wm.offThread() == 0);
        LIFT_CHECK(wm.firstOffThreadSite() == nullptr);

        // -----------------------------------------------------------------------------
        // ② worker 线程碰 `find` ⇒ 计数 + 点名（且**返回值不变**）
        // -----------------------------------------------------------------------------
        step("off-thread-find");
        WorkerFindResult got;
        std::thread worker([&wm, hwnd, &got] {
            got.called = true;
            // ⛔ 守卫只记账：非 owner 也必须拿到同一个指针（违规要能活着进摘要）。
            got.found = wm.find(hwnd) != nullptr;
        });
        worker.join();
        LIFT_CHECK(got.called);
        LIFT_CHECK(got.found);  // 行为不变
        LIFT_CHECK(wm.offThread() >= 1);
        const char* site = wm.firstOffThreadSite();
        LIFT_CHECK(site != nullptr && std::strstr(site, "window.find") != nullptr);
        off_find = wm.offThread();
        site_find = site ? site : "-";

        // -----------------------------------------------------------------------------
        // ③ 站点粒度：reset 之后两个**不同**入口 ⇒ >= 2，first = 第一个
        // -----------------------------------------------------------------------------
        step("site-granularity");
        wm.resetOffThread();
        LIFT_CHECK(wm.offThread() == 0);
        LIFT_CHECK(wm.firstOffThreadSite() == nullptr);
        std::thread worker2([&wm] {
            wm.post(lift::WinMessage{});
            (void)wm.primaryWindow();
        });
        worker2.join();
        LIFT_CHECK(wm.offThread() >= 2);
        const char* first_after_reset = wm.firstOffThreadSite();
        LIFT_CHECK(first_after_reset != nullptr && std::strstr(first_after_reset, "window.post") != nullptr);
        off_two = wm.offThread();
    }

    // ---------------------------------------------------------------------------------
    // ④ 没有 owner 的实例 ⇒ 不计数（未设 owner 的纯夹具不制造假红）
    //
    // ★ 构造函数的 owner = 构造线程，所以"没有 owner"只能**显式**表达：
    //   `setOwnerThread(std::thread::id{})`。没有这一条，"worker 碰 wm 就计数"的守卫会在
    //   那些只拿 `WindowManager` 当夹具、从不关心线程的实现上制造假红。
    // ---------------------------------------------------------------------------------
    step("unowned-silent");
    {
        lift::WindowManager wm;
        LIFT_CHECK(wm.offThread() == 0);
        wm.setOwnerThread(std::thread::id{});
        WorkerFindResult got;
        std::thread worker([&wm, &got] {
            got.called = true;
            got.found = wm.find(0x00010000u) != nullptr;  // 即使没这个窗口也只读表
            wm.post(lift::WinMessage{});
            (void)wm.windowCount();
        });
        worker.join();
        LIFT_CHECK(got.called);
        LIFT_CHECK(wm.offThread() == 0);
        LIFT_CHECK(wm.firstOffThreadSite() == nullptr);
    }

    std::printf("window-manager-token: worker-find off-thread=%llu first=%s | worker-two-entries off-thread=%llu\n",
                off_find, site_find, off_two);
    return finish("window-manager-token");
}
