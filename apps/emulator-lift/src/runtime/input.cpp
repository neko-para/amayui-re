#include "runtime/input.h"

#include <cstdint>
#include <vector>

namespace lift {
namespace input {
namespace {

// Win32 的 `WM_*` lParam：低 16 位 x、高 16 位 y（有符号短整型，这里按位截断，与 SDL 路径一致）。
std::uint32_t mouseLParam(int client_x, int client_y) {
    return (static_cast<std::uint32_t>(static_cast<std::uint16_t>(client_y)) << 16) |
           static_cast<std::uint32_t>(static_cast<std::uint16_t>(client_x));
}

void postToPrimary(WindowManager& wm, std::uint32_t message, std::uint32_t wparam, std::uint32_t lparam,
                   std::uint32_t time_ms) {
    WinMessage m;
    m.hwnd = wm.primaryWindow();
    m.message = message;
    m.wparam = wparam;
    m.lparam = lparam;
    m.time = time_ms;
    wm.post(m);
}

// 收口投影：`LevelRef` → 一条 up 消息（wParam / lParam 从设备**写后回读**，§3.4 第 6 条）。
void postLevelUp(WindowManager& wm, const LevelRef& level, std::uint32_t time_ms) {
    const std::uint32_t hwnd = wm.primaryWindow();
    int client_x = 0;
    int client_y = 0;
    wm.cursorClientPos(hwnd, &client_x, &client_y);
    if (level.mouse) {
        for (std::size_t i = 0; i < kMouseButtonCount; ++i) {
            const MouseButton b = static_cast<MouseButton>(i);
            if (::lift::vkForMouseButton(b) != level.vk) continue;
            postToPrimary(wm, messageForMouseButton(b, /*down=*/false), wm.mouseButtons(),
                          mouseLParam(client_x, client_y), time_ms);
            return;
        }
        return;  // 标了 mouse 但 VK 对不上：不可能（⛔ 不伪造一条错误消息）
    }
    // 键盘 up：与 `input::key(..., down=false, scancode=0)` 同一个 lParam 形状（设备没有 scancode）。
    std::uint32_t lparam = 1u | 0xC0000000u;
    postToPrimary(wm, kWmKeyUp, level.vk, lparam, time_ms);
}

}  // namespace

// ★ S1：唯一定义已下沉到 `runtime/input_device.cpp`（状态层不再 include 施加层）；
//   这里保留同名函数**转发**，所有调用点不变。
std::uint32_t vkForMouseButton(MouseButton b) { return ::lift::vkForMouseButton(b); }

std::uint32_t flagForMouseButton(MouseButton b) { return ::lift::flagForMouseButton(b); }

std::uint32_t messageForMouseButton(MouseButton b, bool down) {
    switch (b) {
        case MouseButton::Left: return down ? kWmLButtonDown : kWmLButtonUp;
        case MouseButton::Right: return down ? kWmRButtonDown : kWmRButtonUp;
        case MouseButton::Middle: return down ? kWmMButtonDown : kWmMButtonUp;
    }
    return 0;
}

bool mouseMove(WindowManager& wm, int client_x, int client_y, InputSource src, std::uint32_t time_ms) {
    // 先更新光标（租约判定在设备里：#3 拒绝 OS、#4 取得租约）；被拒绝 ⇒ 连消息也不发
    // （保持 `GetCursorPos` 与 `WM_MOUSEMOVE` 的 lParam 一致）。
    if (!wm.setCursorClientPos(wm.primaryWindow(), client_x, client_y, src)) return false;
    // ★ S2：消息 = 设备的投影（**写后回读**）；wParam = 设备当前按钮位图（§3.3 的不变量）。
    int lx = 0;
    int ly = 0;
    wm.cursorClientPos(wm.primaryWindow(), &lx, &ly);
    postToPrimary(wm, kWmMouseMove, wm.mouseButtons(), mouseLParam(lx, ly), time_ms);
    return true;
}

bool mouseButton(WindowManager& wm, int client_x, int client_y, MouseButton b, bool down, InputSource src,
                 std::uint32_t time_ms) {
    // ★ S4：⛔ 不再有「按钮 / 键 / 光标被脚本拥有」三条 OS 特例。规则只剩设备 §3.2 的转移表：
    //   · 光标：Win32 的按钮事件带位置。OS 的移动可能被租约拒绝（#3 / #5），但 OS 的**抬起**
    //     仍要落地（#9 / #10）⇒ 抬起被拒时用设备当前位置（与 `GetCursorPos` 一致），不丢抬起。
    //   · 电平：`setButton` 返回 `WriteResult`；**只在 change==Down/Up 时 post**
    //     （#7/#8/#9/#13/#14/#16 = accepted 但不投影）。
    if (!wm.setCursorClientPos(wm.primaryWindow(), client_x, client_y, src)) {
        if (down) return false;  // #5：Program 持租约期间 OS 的上升被拒 ⇒ 不写、不发
    }
    const WriteResult wr = wm.setMouseButton(b, down, src);
    if (!wr.accepted) return false;
    if (wr.change == LevelChange::None) return true;  // 接受但电平没转移（⛔ 不重复 down / 孤儿 up）
    int lx = 0;
    int ly = 0;
    wm.cursorClientPos(wm.primaryWindow(), &lx, &ly);
    postToPrimary(wm, messageForMouseButton(b, down), wm.mouseButtons(), mouseLParam(lx, ly), time_ms);
    return true;
}

bool mouseWheel(WindowManager& wm, int client_x, int client_y, int delta_120, InputSource src,
                std::uint32_t time_ms) {
    // ★ S4 / Q3b（§3.2 #21）：滚轮**只是事件**，不参与租约 / 电平，也不会因为光标写失败被丢。
    //   先 best-effort 写光标；失败（Program 持租约）⇒ 用设备当前位置，仍然 post。
    (void)wm.setCursorClientPos(wm.primaryWindow(), client_x, client_y, src);
    int lx = 0;
    int ly = 0;
    wm.cursorClientPos(wm.primaryWindow(), &lx, &ly);
    const std::uint32_t wparam =
        (static_cast<std::uint32_t>(delta_120) << 16) | (wm.mouseButtons() & 0xFFFFu);
    postToPrimary(wm, kWmMouseWheel, wparam, mouseLParam(lx, ly), time_ms);
    return true;
}

bool key(WindowManager& wm, std::uint32_t vk, bool down, std::uint32_t scancode, InputSource src,
         std::uint32_t time_ms, bool repeat) {
    if (vk == 0) return false;
    const WriteResult wr = wm.setKeyDown(src, vk, down);
    if (!wr.accepted) return false;
    // 投影规则：**只在电平转移时发消息**（§3.2）。两个显式例外：
    //   ① 键盘 auto-repeat（#23）：`down && repeat` 即使电平不变也发 `WM_KEYDOWN`（bit30=1）；
    //   ② 批量收口不在这里（见 `releaseProgram` / `dropOsLevels`）。
    const bool level_changed = wr.change == LevelChange::Down || wr.change == LevelChange::Up;
    const bool repeat_post = down && repeat && wr.change == LevelChange::None;
    if (!level_changed && !repeat_post) return true;
    std::uint32_t lparam = 1u | (scancode << 16);
    if (!down) {
        lparam |= 0xC0000000u;  // previous state + transition
    } else if (repeat_post) {
        lparam |= 0x40000000u;  // 重复键：bit30 = 之前就是按下态（Win32 auto-repeat）
    }
    postToPrimary(wm, down ? kWmKeyDown : kWmKeyUp, vk, lparam, time_ms);
    return true;
}

std::vector<LevelRef> releaseProgram(WindowManager& wm, std::uint32_t time_ms) {
    std::vector<LevelRef> levels = wm.releaseProgramLevels();
    for (const LevelRef& level : levels) postLevelUp(wm, level, time_ms);
    return levels;
}

std::vector<LevelRef> dropOsLevels(WindowManager& wm, std::uint32_t time_ms) {
    std::vector<LevelRef> levels = wm.dropOsLevelsState();
    for (const LevelRef& level : levels) postLevelUp(wm, level, time_ms);
    return levels;
}

}  // namespace input
}  // namespace lift
