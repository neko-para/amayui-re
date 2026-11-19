#include "runtime/input_device.h"

namespace lift {

// =====================================================================================
// 按钮 ↔ VK / MK_* 位（**唯一一份**；2026-11-19 S1 从 `runtime/input.cpp` 逐字搬来）
//   `runtime/input.h` 的 `input::vkForMouseButton` / `input::flagForMouseButton` 保留同名声明，
//   实现改为转发到这里 ⇒ 调用点不变，状态层也不再反向依赖施加层。
// =====================================================================================
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

// =====================================================================================
// 写规则（S1：搬家前 `WindowManager::setCursorPos/setCursorClientPos/setKeyDown/
//   setMouseButton/clearKeys/clearOsInput/releaseScriptOwnership` 的函数体；
//   S3 只删掉 `mouse_buttons_` 的写入 —— 位图现在是 `key_down_` 的只读派生，其余规则逐字未变；
//   S4 才收敛成单一租约）
// =====================================================================================

bool InputDevice::moveToScreen(int screen_x, int screen_y, InputSource src) {
    // ★ 脚本拥有光标期间，OS 的鼠标移动不生效（见 `InputSource`）：这样 `GetCursorPos` 与
    //   `WM_MOUSEMOVE` 的 lParam 永远说同一个位置。
    if (src == InputSource::Os && cursor_script_owned_) return false;
    if (src == InputSource::Script) cursor_script_owned_ = true;
    cursor_x_ = screen_x;
    cursor_y_ = screen_y;
    return true;
}

bool InputDevice::setKey(InputSource src, std::uint32_t vk, bool down) {
    if (vk >= 256u) return false;
    // ★ 所有权规则（见 `InputSource`）：脚本拥有的键，OS 的按下 / 抬起都不生效。
    if (src == InputSource::Os && key_owner_[vk] == static_cast<std::uint8_t>(InputSource::Script)) {
        return false;
    }
    // 脚本抬起 ⇒ 该键交还 OS（`down == false` 时所有者清空）。
    key_owner_[vk] = down ? static_cast<std::uint8_t>(src) : static_cast<std::uint8_t>(InputSource::Os);
    key_down_[vk] = down ? 1u : 0u;
    return true;
}

bool InputDevice::setMouseButton(MouseButton b, bool down, InputSource src) {
    const auto idx = static_cast<std::size_t>(b);
    if (idx >= kMouseButtonCount) return false;
    const std::uint32_t vk = vkForMouseButton(b);
    if (!setKey(src, vk, down)) return false;
    button_owner_[idx] = down ? static_cast<std::uint8_t>(src) : static_cast<std::uint8_t>(InputSource::Os);
    // ★ S3：这里**只写 VK 电平**（上面的 `setKey`）。按钮位图由 `key_down_` 派生
    //   （`InputDevice::buttons()`），⛔ 不再有第二份 `mouse_buttons_` 可发散。
    return true;
}

void InputDevice::clearKeys() {
    for (std::size_t i = 0; i < 256u; ++i) {
        key_down_[i] = 0;
        key_owner_[i] = 0;
    }
    for (std::uint8_t& o : button_owner_) o = 0;
}

void InputDevice::clearOsInput() {
    for (std::size_t i = 0; i < 256u; ++i) {
        if (key_owner_[i] == static_cast<std::uint8_t>(InputSource::Script)) continue;
        key_down_[i] = 0;
    }
    // ★ S3：按钮电平已由上面的键循环清掉（按钮 = 三个 VK）；这里只清**所有者**。
    for (std::size_t i = 0; i < kMouseButtonCount; ++i) {
        if (button_owner_[i] == static_cast<std::uint8_t>(InputSource::Script)) continue;
        button_owner_[i] = 0;
    }
}

void InputDevice::releaseScriptOwnership() {
    for (std::size_t i = 0; i < 256u; ++i) {
        if (key_owner_[i] != static_cast<std::uint8_t>(InputSource::Script)) continue;
        key_down_[i] = 0;
        key_owner_[i] = 0;
    }
    // ★ S3：脚本拥有的按钮电平已由上面的键循环清掉（`setMouseButton` 同时把
    //   `key_owner_[vk]` 置成 Script）；这里只清**所有者**。
    for (std::size_t i = 0; i < kMouseButtonCount; ++i) {
        if (button_owner_[i] != static_cast<std::uint8_t>(InputSource::Script)) continue;
        button_owner_[i] = 0;
    }
    cursor_script_owned_ = false;
}

}  // namespace lift
