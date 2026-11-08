#include "runtime/input.h"

#include <cstdint>

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

}  // namespace

std::uint32_t vkForMouseButton(MouseButton b) {
    switch (b) {
        case MouseButton::Left: return 0x01u;    // VK_LBUTTON
        case MouseButton::Right: return 0x02u;   // VK_RBUTTON
        case MouseButton::Middle: return 0x04u;  // VK_MBUTTON
    }
    return 0;
}

std::uint32_t flagForMouseButton(MouseButton b) {
    switch (b) {
        case MouseButton::Left: return kMkLButton;
        case MouseButton::Right: return kMkRButton;
        case MouseButton::Middle: return kMkMButton;
    }
    return 0;
}

std::uint32_t messageForMouseButton(MouseButton b, bool down) {
    switch (b) {
        case MouseButton::Left: return down ? kWmLButtonDown : kWmLButtonUp;
        case MouseButton::Right: return down ? kWmRButtonDown : kWmRButtonUp;
        case MouseButton::Middle: return down ? kWmMButtonDown : kWmMButtonUp;
    }
    return 0;
}

bool mouseMove(WindowManager& wm, int client_x, int client_y, InputSource src, std::uint32_t time_ms) {
    // 先更新光标（含所有权判定）；被拒绝 ⇒ 连消息也不发（保持 GetCursorPos 与 lParam 一致）。
    if (!wm.setCursorClientPos(wm.primaryWindow(), client_x, client_y, src)) return false;
    postToPrimary(wm, kWmMouseMove, wm.mouseButtons(), mouseLParam(client_x, client_y), time_ms);
    return true;
}

bool mouseButton(WindowManager& wm, int client_x, int client_y, MouseButton b, bool down, InputSource src,
                 std::uint32_t time_ms) {
    // ★ 所有权判定（先判后改，避免"光标动了、电平没动、消息没发"的半截输入）：
    //   · 脚本按着的按钮 / 被脚本 `key_down` 按着的 VK：OS 的按下 / 抬起都不参与（脚本的 hold 才能成立）。
    //   · OS 的**按下**坐标来自 OS 光标；光标归脚本时拒绝它（否则消息与 GetCursorPos 说两套话）。
    //   · OS 的**抬起**永远接受（哪怕光标归脚本）：否则"真人按着 → 脚本接管光标 → 松开被拒"
    //     会让 VK_LBUTTON 电平卡在按下。抬起用**当前虚拟光标**做 lParam（与 GetCursorPos 一致）。
    //   · 光标归脚本时 OS 不碰位置；否则 up 也要跟随事件坐标（与改动前 SDL 路径一致）。
    if (src == InputSource::Os) {
        if (wm.mouseButtonOwnedByScript(b)) return false;
        if (wm.keyOwnedByScript(vkForMouseButton(b))) return false;
        if (down && wm.cursorOwnedByScript()) return false;
        if (wm.cursorOwnedByScript()) {
            wm.cursorClientPos(wm.primaryWindow(), &client_x, &client_y);
        } else {
            wm.setCursorClientPos(wm.primaryWindow(), client_x, client_y, InputSource::Os);
        }
        if (!wm.setMouseButton(b, down, InputSource::Os)) return false;
        postToPrimary(wm, messageForMouseButton(b, down), wm.mouseButtons(), mouseLParam(client_x, client_y),
                      time_ms);
        return true;
    }
    // 与真机同序：先把光标放到按键位置，再改按钮电平，最后发消息。
    wm.setCursorClientPos(wm.primaryWindow(), client_x, client_y, InputSource::Script);
    wm.setMouseButton(b, down, InputSource::Script);
    postToPrimary(wm, messageForMouseButton(b, down), wm.mouseButtons(), mouseLParam(client_x, client_y),
                  time_ms);
    return true;
}

bool mouseWheel(WindowManager& wm, int client_x, int client_y, int delta_120, InputSource src,
                std::uint32_t time_ms) {
    if (!wm.setCursorClientPos(wm.primaryWindow(), client_x, client_y, src)) return false;
    const std::uint32_t wparam =
        (static_cast<std::uint32_t>(delta_120) << 16) | (wm.mouseButtons() & 0xFFFFu);
    postToPrimary(wm, kWmMouseWheel, wparam, mouseLParam(client_x, client_y), time_ms);
    return true;
}

bool key(WindowManager& wm, std::uint32_t vk, bool down, std::uint32_t scancode, InputSource src,
         std::uint32_t time_ms) {
    if (vk == 0) return false;
    if (!wm.setKeyDown(src, vk, down)) return false;
    std::uint32_t lparam = 1u | (scancode << 16);
    if (!down) lparam |= 0xC0000000u;  // previous state + transition
    postToPrimary(wm, down ? kWmKeyDown : kWmKeyUp, vk, lparam, time_ms);
    return true;
}

}  // namespace input
}  // namespace lift
