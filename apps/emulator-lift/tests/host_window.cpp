// emulator-lift · headless 窗口 / 消息队列 / 计时器（P0；pure）
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>

#include "check.h"
#include "runtime/host_window.h"
#include "runtime/machine.h"
#include "runtime/vfs.h"

int main() {
    using namespace lift_test;
    lift::WindowManager wm;
    g_current_case = "class-window";
    const std::uint32_t atom = wm.registerClass("ARCGameEngine", 0x401000u, 0);
    LIFT_CHECK(atom != 0);
    // 0x90CB0000 = WS_POPUP|WS_VISIBLE|WS_CAPTION|WS_SYSMENU|...
    const std::uint32_t hwnd = wm.createWindow("ARCGameEngine", 0x90CB0000u, 0, 0, 0, 1286, 764, 0);
    LIFT_CHECK(hwnd != 0);
    lift::HostWindow* w = wm.find(hwnd);
    LIFT_CHECK(w != nullptr);
    if (w) {
        LIFT_CHECK_EQ_I64(w->client_w, 1280);
        LIFT_CHECK_EQ_I64(w->client_h, 720);
        LIFT_CHECK(w->proc == 0x401000u);
    }

    g_current_case = "queue-noremove";
    lift::WinMessage msg;
    msg.hwnd = hwnd;
    msg.message = 0x0400;
    msg.wparam = 7;
    wm.post(msg);
    LIFT_CHECK_EQ_I64(wm.queueSize(), 1);
    lift::WinMessage got;
    LIFT_CHECK(wm.peek(&got, false));
    LIFT_CHECK_EQ_U32(got.message, 0x0400u);
    LIFT_CHECK_EQ_I64(wm.queueSize(), 1);
    LIFT_CHECK(wm.peek(&got, true));
    LIFT_CHECK_EQ_I64(wm.queueSize(), 0);

    g_current_case = "quit";
    wm.postQuit(3);
    LIFT_CHECK(wm.peek(&got, true));
    LIFT_CHECK_EQ_U32(got.message, 0x0012u);
    LIFT_CHECK_EQ_U32(got.wparam, 3u);

    g_current_case = "timer";
    lift::Machine m;
    std::string err;
    LIFT_CHECK(m.initBare(&err));
    LIFT_CHECK(m.prepareStack(&err));
    const std::uint32_t id = wm.setTimer(m.cpu(), hwnd, 0, 16);
    LIFT_CHECK(id != 0);
    m.clock().advance100ns(160000ull);  // 16 ms
    wm.pumpTimers(m.cpu());
    LIFT_CHECK(wm.peek(&got, true));
    LIFT_CHECK_EQ_U32(got.message, 0x0113u);  // WM_TIMER
    LIFT_CHECK_EQ_U32(got.wparam, id);
    LIFT_CHECK(wm.killTimer(hwnd, id));
    LIFT_CHECK_EQ_I64(wm.timerCount(), 0);

    g_current_case = "destroy";
    LIFT_CHECK(wm.destroyWindow(hwnd));
    LIFT_CHECK(wm.find(hwnd) == nullptr);

    // --appdata：特殊目录的 guest 路径 → 宿主 --appdata/<sub>
    g_current_case = "appdata-resolve";
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "lift-appdata-test";
    fs::create_directories(root / "game");
    fs::create_directories(root / "appdata" / "Roaming" / "sub");
    lift::Vfs v;
    v.setGameDir((root / "game").string());
    v.setAppDataDir((root / "appdata").string());
    std::string host, verr;
    LIFT_CHECK(v.resolve("C:\\Users\\Amayui\\AppData\\Roaming\\sub\\x.txt", &host, &verr, false));
    LIFT_CHECK(host == (root / "appdata" / "Roaming" / "sub" / "x.txt").string());
    LIFT_CHECK(!v.resolve("C:\\Users\\Amayui\\AppData\\Roaming\\nope\\x.txt", &host, &verr, true));
    lift::Vfs v2;
    v2.setGameDir((root / "game").string());
    LIFT_CHECK(!v2.resolve("C:\\Users\\Amayui\\AppData\\Roaming\\x.txt", &host, &verr, false));
    fs::remove_all(root);

    return finish("host-window");
}
