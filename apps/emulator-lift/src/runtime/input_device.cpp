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

bool isMouseButtonVk(std::uint32_t vk) {
    for (std::size_t i = 0; i < kMouseButtonCount; ++i) {
        if (vkForMouseButton(static_cast<MouseButton>(i)) == vk) return true;
    }
    return false;
}

// =====================================================================================
// 写规则（S4：§3.2 的转移表；⛔ 不再有 v1 的三套 owner）
//   记 L=key_level_、O=level_owner_、lease=lease_。设备**不 post** —— `input::*` 看 change 投影。
// =====================================================================================

bool InputDevice::moveToScreen(int screen_x, int screen_y, Driver d) {
    switch (d) {
        case Driver::Guest:
            // #1：guest 自己的 `SetCursorPos`：永远接受，不改 lease / 电平，也不发消息。
            screen_x_ = screen_x;
            screen_y_ = screen_y;
            return true;
        case Driver::Os:
            // #3：Program 持租约期间 OS 的移动被拒（`GetCursorPos` 与 `WM_MOUSEMOVE` 必须说同一个位置）。
            if (lease_ == Driver::Program) return false;
            // #2
            screen_x_ = screen_x;
            screen_y_ = screen_y;
            return true;
        case Driver::Program:
            // #4：Program 的移动**取得租约**（第一次有效移动就是取得时机，§3.4 第 2 条）。
            lease_ = Driver::Program;
            screen_x_ = screen_x;
            screen_y_ = screen_y;
            return true;
    }
    return false;
}

WriteResult InputDevice::setKey(std::uint32_t vk, bool down, Driver d) {
    WriteResult r;
    if (vk >= 256u) return r;
    if (d == Driver::Guest) return r;  // #22：Guest 只能写光标，不提供 setButton / setKey
    std::uint8_t& level = key_level_[vk];
    std::uint8_t& owner = level_owner_[vk];
    if (d == Driver::Os) {
        if (down) {
            // #5：OS 的上升在 Program 持租约期间一律拒绝。
            if (lease_ == Driver::Program) return r;
            // #7：已经按下 ⇒ 接受但不改电平（按钮不 repeat；键盘 repeat 是调用方的显式例外）。
            if (level != 0) {
                r.accepted = true;
                return r;
            }
            // #6
            level = 1;
            owner = static_cast<std::uint8_t>(LevelOwner::Os);
            r.accepted = true;
            r.change = LevelChange::Down;
            return r;
        }
        // ---- OS 的下降 ----
        if (level == 0) {
            // #8：孤儿 up —— 接受但不发消息（没有造成 1→0 转移）。
            r.accepted = true;
            return r;
        }
        if (owner == static_cast<std::uint8_t>(LevelOwner::Program)) {
            // #9：真人的松开不取消 Program 的 hold（接受、不写、不发）。
            r.accepted = true;
            return r;
        }
        // #10
        level = 0;
        owner = static_cast<std::uint8_t>(LevelOwner::None);
        r.accepted = true;
        r.change = LevelChange::Up;
        return r;
    }
    // ---- Program ----
    if (down) {
        // #11：第一次有效上升取得租约。
        if (lease_ == Driver::Os) lease_ = Driver::Program;
        if (level == 0) {
            // #12
            level = 1;
            owner = static_cast<std::uint8_t>(LevelOwner::Program);
            r.accepted = true;
            r.change = LevelChange::Down;
            return r;
        }
        if (owner == static_cast<std::uint8_t>(LevelOwner::Os)) {
            // #13：接管（不改电平、不重复 down；guest 已经看过那条 down）。
            owner = static_cast<std::uint8_t>(LevelOwner::Program);
            r.accepted = true;
            return r;
        }
        // #14：已经由 Program 抬起 ⇒ 接受但不动。
        r.accepted = true;
        return r;
    }
    // ---- Program 的下降 ----
    // #15：没有租约的 up 不生效。
    if (lease_ != Driver::Program) return r;
    if (level == 0) {
        // #16
        r.accepted = true;
        return r;
    }
    if (owner == static_cast<std::uint8_t>(LevelOwner::Os)) {
        // #18：Program 不取消真人按住的键。
        return r;
    }
    // #17
    level = 0;
    owner = static_cast<std::uint8_t>(LevelOwner::None);
    r.accepted = true;
    r.change = LevelChange::Up;
    return r;
}

WriteResult InputDevice::setButton(MouseButton b, bool down, Driver d) {
    if (static_cast<std::size_t>(b) >= kMouseButtonCount) return WriteResult{};
    return setKey(vkForMouseButton(b), down, d);
}

std::vector<LevelRef> InputDevice::release(Driver d) {
    std::vector<LevelRef> out;
    if (d != Driver::Program) return out;
    // #19：只清 `O==Program` 的电平（`O==Os` 的键不受影响，§3.4 第 8 条）。
    for (std::uint32_t vk = 0; vk < 256u; ++vk) {
        if (level_owner_[vk] != static_cast<std::uint8_t>(LevelOwner::Program)) continue;
        out.push_back(LevelRef{vk, LevelOwner::Program, isMouseButtonVk(vk)});
        key_level_[vk] = 0;
        level_owner_[vk] = static_cast<std::uint8_t>(LevelOwner::None);
    }
    lease_ = Driver::Os;
    return out;
}

std::vector<LevelRef> InputDevice::dropOsLevels() {
    std::vector<LevelRef> out;
    // #20：只清 `O==Os` 的电平；Program 的 hold 保留（注入不因失焦 / 暂停而丢）。
    for (std::uint32_t vk = 0; vk < 256u; ++vk) {
        if (level_owner_[vk] != static_cast<std::uint8_t>(LevelOwner::Os)) continue;
        out.push_back(LevelRef{vk, LevelOwner::Os, isMouseButtonVk(vk)});
        key_level_[vk] = 0;
        level_owner_[vk] = static_cast<std::uint8_t>(LevelOwner::None);
    }
    return out;
}

std::vector<LevelRef> InputDevice::programHeldLevels() const {
    std::vector<LevelRef> out;
    for (std::uint32_t vk = 0; vk < 256u; ++vk) {
        if (level_owner_[vk] != static_cast<std::uint8_t>(LevelOwner::Program)) continue;
        out.push_back(LevelRef{vk, LevelOwner::Program, isMouseButtonVk(vk)});
    }
    return out;
}

void InputDevice::clearKeys() {
    // 旧口径 / 测试出口：整机复位（电平、来源标签、租约）。⛔ 不投影任何消息。
    for (std::size_t i = 0; i < 256u; ++i) {
        key_level_[i] = 0;
        level_owner_[i] = static_cast<std::uint8_t>(LevelOwner::None);
    }
    lease_ = Driver::Os;
}

}  // namespace lift
