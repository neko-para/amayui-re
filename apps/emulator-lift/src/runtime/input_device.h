// emulator-lift · 输入设备实体（S1：**过渡形态**，纯搬家）
//
// ★ 口径（`.tmp/input-virtual-mouse/design.md` §3.1 / §4.1–§4.3）：S1 把 2026-11-19 之前散在
//   `WindowManager` 的 6 组输入状态搬进这里，写方法**逐字照抄**当时的规则
//   （逐键 `key_owner_` / 逐按钮 `button_owner_` / 光标 `cursor_script_owned_` 三套 owner 都还在）。
//   S3 删掉了 `mouse_buttons_`：按钮位图是 `key_down_` 里三个 VK 的**只读派生**。
//   ⛔ 仍未到设计目标形态：S4 才引入「单一租约 / `Driver` / `LevelOwner`」。
// ★ 本文件 **不 include `host_window.h`**（否则成环）：它只依赖自己的枚举 / 映射；
//   屏幕 ↔ 客户区的换算仍由 `WindowManager` 做（窗口表在那边）。
// ★ 写 API 不公开：`InputDevice` 的写方法 private + `friend class WindowManager`，
//   `WindowManager` 的同名 setter 是唯一写入口（`input::*` 只调 `WindowManager`）。
#pragma once

#include <cstddef>
#include <cstdint>

namespace lift {

// =====================================================================================
// 输入来源（★ 随状态一起从 `host_window.h` 搬来；**唯一输入状态源**现在是 `InputDevice`）
// ★ 2026-11-08：这里有两个来源 —— `Os`（SDL 真实事件）与 `Script`（机器内注入；原 `--input-script`
//   的 CLI 入口 2026-11-16 已退役，注入侧只剩配方 actuator 与 pure CTest 夹具）。
//   两者**走同一段应用代码**（`runtime/input.h`），所以不存在"注入只发消息、GetAsyncKeyState 仍读
//   OS"的第二份状态。
// ★ **优先级（显式规则，互相不污染）**：
//   · 每个 VK 有一个所有者（`Os` 默认 / `Script`）。脚本按下某键 ⇒ 该键归脚本；期间 OS 的
//     按下 / 抬起 / **焦点丢失清键** 一律**不生效**（注入不因窗口失焦而丢）。
//   · 脚本 `key_up` / `mouse_up` 把该键**交还** OS（状态置抬起、所有者清空）。
//   · 光标：脚本第一次 `mouse_move` 起归脚本；期间 OS 的鼠标移动**不生效**（不更新也不发消息，
//     保持 `GetCursorPos` 与 `WM_MOUSEMOVE` 的 lParam 一致）；OS 的鼠标按键同样不生效
//     （它们的坐标来自 OS 光标，而那份位置没有被采用 —— 半截输入比拒绝更糟）。
//   · 脚本**结束**（动作跑完 / `quit`）⇒ `releaseScriptOwnership()`：释放脚本按下的键 / 按钮、
//     交还光标所有权 —— 真人接着玩不会撞上"卡住的键"。
// =====================================================================================
enum class InputSource : std::uint8_t { Os = 0, Script = 1 };

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

// =====================================================================================
// 设备实体（S1 = 纯搬家；字段与规则 = 搬家前 `WindowManager` 的那一份，逐字一致）
// =====================================================================================
class InputDevice {
public:
    // ---- 读（搬家前那几个 getter 的实现逐字搬来；`checkMainThread` 留在 `WindowManager`）----
    void cursorScreen(int* x, int* y) const {
        if (x) *x = static_cast<int>(cursor_x_);
        if (y) *y = static_cast<int>(cursor_y_);
    }
    bool keyDown(std::uint32_t vk) const { return vk < 256u && key_down_[vk] != 0; }
    bool keyOwnedByScript(std::uint32_t vk) const {
        return vk < 256u && key_owner_[vk] == static_cast<std::uint8_t>(InputSource::Script);
    }
    // ★ S3：按钮电平 / 位图都是 `key_down_` 里三个 VK 的**只读派生**（⛔ 没有第二份 `mouse_buttons_`）。
    bool buttonDown(MouseButton b) const {
        const std::uint32_t vk = vkForMouseButton(b);
        return vk < 256u && key_down_[vk] != 0;
    }
    std::uint32_t buttons() const {
        std::uint32_t bits = 0;
        for (std::size_t i = 0; i < kMouseButtonCount; ++i) {
            const MouseButton b = static_cast<MouseButton>(i);
            if (buttonDown(b)) bits |= flagForMouseButton(b);
        }
        return bits;
    }
    bool mouseButtonOwnedByScript(MouseButton b) const {
        const auto idx = static_cast<std::size_t>(b);
        return idx < kMouseButtonCount &&
               button_owner_[idx] == static_cast<std::uint8_t>(InputSource::Script);
    }
    bool cursorOwnedByScript() const { return cursor_script_owned_; }

private:
    friend class WindowManager;

    // ---- 写（规则逐字照抄；只有 friend 能调）----
    // guest 自己的 `SetCursorPos`：只写位置，不改所有权 / 不发消息。
    void setCursorScreen(int x, int y) {
        cursor_x_ = x;
        cursor_y_ = y;
    }
    // 客户区 → 屏幕的换算由 `WindowManager` 做；这里只做所有权判定 + 写位置。
    bool moveToScreen(int screen_x, int screen_y, InputSource src);
    bool setKey(InputSource src, std::uint32_t vk, bool down);
    bool setMouseButton(MouseButton b, bool down, InputSource src);
    void clearKeys();
    void clearOsInput();
    void releaseScriptOwnership();

    // ★ 字段 = 搬家前 `WindowManager` 私有段的 6 组（S1 纯搬家；S3 删掉了 `mouse_buttons_`，
    //   按钮位图现在是 `key_down_` 三个 VK 的只读派生；S4 才收敛成单一租约 + `level_owner_`）：
    //   · `cursor_x_/cursor_y_`：guest **屏幕**坐标（客户区 = 屏幕 − 窗口原点，见 host_window.cpp）；
    //   · `key_down_[256]` / `key_owner_[256]`：逐键电平 + 所有者（**按钮电平也在这张表里**）；
    //   · `button_owner_[3]`：逐按钮所有者；
    //   · `cursor_script_owned_`：光标所有权。
    std::int32_t cursor_x_ = 0;
    std::int32_t cursor_y_ = 0;
    std::uint8_t key_down_[256] = {};
    std::uint8_t key_owner_[256] = {};
    std::uint8_t button_owner_[kMouseButtonCount] = {};
    bool cursor_script_owned_ = false;
};

}  // namespace lift
