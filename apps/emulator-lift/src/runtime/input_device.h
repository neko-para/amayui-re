// emulator-lift · 输入设备实体（S4：**设计最终形态**）
//
// ★ 口径（`.tmp/input-virtual-mouse/design.md` §3.1 / §3.2 / §3.4 / §4.4）：
//   · **唯一租约** `lease_`（`Os` / `Program`；`Guest` 是写者，永不取得租约）；
//   · **唯一电平表** `key_level_[256]`（按钮 = 其中三个 VK）+ **逐电平来源** `level_owner_[256]`
//     （只用于「已经抬起的电平由谁收口」；⛔ 不是第二套 owner 数组）；
//   · 写方法返回 `WriteResult{accepted, change}`，调用方（`input::*`）**只在 `change==Down/Up`
//     时投影消息**（唯一例外是键盘 auto-repeat，见 §3.11）；
//   · 收口（`release` / `dropOsLevels`）只返回 `vector<LevelRef>`，**设备不 post**
//     （消息由 `input::releaseProgram` / `input::dropOsLevels` 逐条 post）。
// ★ 本文件 **不 include `host_window.h`**（否则成环）：它只依赖自己的枚举 / 映射；
//   屏幕 ↔ 客户区的换算仍由 `WindowManager` 做（窗口表在那边）。
// ★ 设备里**没有**时钟 / 消息队列 / 窗口（§3.1 的「关键，不是遗漏」）。
// ★ 写 API 不公开：private + `friend class WindowManager`，`WindowManager` 的同名 setter 是唯一
//   写入口（`input::*` 只调 `WindowManager`）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lift {

// =====================================================================================
// 输入来源（**公开施加面**的枚举；`WindowManager` 收它，转成设备侧的 `Driver`）
// ★ `Os` = SDL 真实事件（`Driver::Os`）· `Script` = 配方 / 控制层 / `InputScript`
//   （三者同属一个 Program 驱动者，因为它们从不并发 ⇒ `Driver::Program`）。
// =====================================================================================
enum class InputSource : std::uint8_t { Os = 0, Script = 1 };

// 写者 / 租约持有者。`Guest` 只是**写者**（guest 自己的 `SetCursorPos`），永远不持有租约（§3.4）。
enum class Driver : std::uint8_t { Os = 0, Program = 1, Guest = 2 };

// 某个电平**当前是谁抬起的**；`None` = 电平为 0。它不是「所有权策略」，是 down/up 配对
// 所需的来源标签（§3.1：取代 v1 的三套 owner）。
enum class LevelOwner : std::uint8_t { None = 0, Os = 1, Program = 2 };

enum class LevelChange : std::uint8_t { None = 0, Down = 1, Up = 2 };

struct WriteResult {
    bool accepted = false;                  // false = 被租约 / 来源规则拒绝（不写、不投影）
    LevelChange change = LevelChange::None;  // 只在 accepted 时有意义
};

// 收口时的一条待投影电平（`release` / `dropOsLevels` 的返回值）。
struct LevelRef {
    std::uint32_t vk = 0;
    LevelOwner owner = LevelOwner::None;
    bool mouse = false;  // true = 这个 vk 是鼠标按钮（0x01/0x02/0x04），投影成 WM_*UP
};

enum class MouseButton : std::uint8_t { Left = 0, Right = 1, Middle = 2 };
constexpr std::size_t kMouseButtonCount = 3;

// MK_* 位（`WM_*` 的 wParam 低位）。★ 与 `MouseButton` 一起下沉，供 `flagForMouseButton` 使用；
//   `runtime/input.h` 在 `lift::input` 里保留同名常量（调用点不变）。
constexpr std::uint32_t kMkLButton = 0x0001u;
constexpr std::uint32_t kMkRButton = 0x0002u;
constexpr std::uint32_t kMkMButton = 0x0010u;

// 按钮 ↔ VK / MK_* 位（**唯一一份**映射）。`runtime/input.h` 的同名函数保留声明并转发到这里，
// ⇒ 状态层（本文件 / `host_window.*`）不再反向依赖施加层（`input.h`）。
std::uint32_t vkForMouseButton(MouseButton b);
std::uint32_t flagForMouseButton(MouseButton b);
// 这个 VK 是不是鼠标按钮（0x01/0x02/0x04）；收口投影按它决定 `WM_*UP` 还是 `WM_KEYUP`。
bool isMouseButtonVk(std::uint32_t vk);

// =====================================================================================
// 设备实体（§3.1 的完整字段 / API）
// =====================================================================================
class InputDevice {
public:
    // ---- 读（**唯一**电平状态；guest 侧所有读点最终读这几行）----
    void cursorScreen(int* x, int* y) const {
        if (x) *x = static_cast<int>(screen_x_);
        if (y) *y = static_cast<int>(screen_y_);
    }
    bool keyDown(std::uint32_t vk) const { return vk < 256u && key_level_[vk] != 0; }
    // ★ 按钮电平是 `key_level_` 里三个 VK 的**只读派生**（⛔ 没有第二份 `mouse_buttons_`）。
    bool buttonDown(MouseButton b) const {
        const std::uint32_t vk = vkForMouseButton(b);
        return vk < 256u && key_level_[vk] != 0;
    }
    std::uint32_t buttons() const {
        std::uint32_t bits = 0;
        for (std::size_t i = 0; i < kMouseButtonCount; ++i) {
            const MouseButton b = static_cast<MouseButton>(i);
            if (buttonDown(b)) bits |= flagForMouseButton(b);
        }
        return bits;
    }
    Driver lease() const { return lease_; }
    bool programDriving() const { return lease_ == Driver::Program; }
    LevelOwner levelOwner(std::uint32_t vk) const {
        return vk < 256u ? static_cast<LevelOwner>(level_owner_[vk]) : LevelOwner::None;
    }
    // 收口前的**只读**快照（只给日志 / 测试；`release(Program)` 才是写）。
    std::vector<LevelRef> programHeldLevels() const;

private:
    friend class WindowManager;

    // ---- 写（**private**；只有 friend 能调；§3.6 的能力面）----
    // Guest 永远接受；Os / Program 按租约（§3.2 #1–#4）。
    bool moveToScreen(int screen_x, int screen_y, Driver d);
    WriteResult setButton(MouseButton b, bool down, Driver d);
    WriteResult setKey(std::uint32_t vk, bool down, Driver d);
    // 收口：清掉 owner == Program 的电平，返回待投影的 up 事件；**不发消息**（§3.4）。
    std::vector<LevelRef> release(Driver d);
    // 焦点丢失 / 暂停：清掉 owner == Os 的电平，返回待投影的 up 事件；**不发消息**（§3.4）。
    std::vector<LevelRef> dropOsLevels();
    // 测试 / 旧口径：全部电平与来源标签清零（不投影）。lease 也归 Os（整机复位）。
    void clearKeys();

    // ★ 字段（§3.1）：
    //   · `screen_x_/screen_y_`：guest **屏幕**坐标（客户区 = 屏幕 − 窗口原点，见 host_window.cpp）；
    //   · `key_level_[256]`：唯一电平表（按钮电平也在其中）；
    //   · `level_owner_[256]`：电平为 1 时记「谁抬起的」，为 0 时 `None`；
    //   · `lease_`：当前租约，只有 `Os` / `Program`。
    std::int32_t screen_x_ = 0;
    std::int32_t screen_y_ = 0;
    std::uint8_t key_level_[256] = {};
    std::uint8_t level_owner_[256] = {};
    Driver lease_ = Driver::Os;
};

}  // namespace lift
