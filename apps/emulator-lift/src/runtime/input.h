// emulator-lift · 输入应用层（★ SDL 真实事件与 `--input-script` 注入的**唯一**汇合点）
//
// ★ 为什么要有这一层（2026-11-08）：在它之前，"一次输入"的落地代码只存在于
//   `present_sdl.cpp` 的 `handleEvent` 里 —— 那是 SDL 回调，只有 OS 能喂它。于是
//   「把光标放到 Option 上点一下」必须靠窗口焦点 + 真实光标位置 + `SendInput`，不可脚本化。
//   现在两条来源都调用这里：**同一段代码**更新 `WindowManager` 的唯一状态（影子键电平 /
//   鼠标按钮位图 / 虚拟光标），并 post **同一组** `WM_*` 消息。guest 侧所有读点
//   （`GetAsyncKeyState` / `GetCursorPos` / `PeekMessageA`+`DispatchMessageA`）因此看到同一份状态。
//
// ★ 坐标口径：参数是 **guest 客户区坐标**（= 引擎的 1280×720 虚拟屏空间）。
//   `client → screen` 的换算**只有一处**：`WindowManager::setCursorClientPos`（加窗口原点），
//   `GetCursorPos` / `ScreenToClient` 读的就是它。⛔ 本层不另写一套映射。
//   SDL 的"窗口像素 → guest 客户区"映射仍留在 `present_sdl.cpp` 的 `mapMouse`（它需要窗口尺寸，
//   是 SDL 平台的属性）；注入脚本的坐标本来就是 guest 空间，不需要再过一次。
//
// ★ 来源与优先级：见 `host_window.h` 的 `InputSource`。本层只负责"把一次输入交给状态源"，
//   不决定谁赢——所有权的判定在 `WindowManager`。
#pragma once

#include <cstdint>

#include "runtime/host_window.h"

namespace lift {
namespace input {

// Win32 输入消息 / 鼠标键位（**唯一一份**；present_sdl 与注入脚本共用，⛔ 不要各自再定义）
constexpr std::uint32_t kWmKeyDown = 0x0100u;
constexpr std::uint32_t kWmKeyUp = 0x0101u;
constexpr std::uint32_t kWmMouseMove = 0x0200u;
constexpr std::uint32_t kWmLButtonDown = 0x0201u;
constexpr std::uint32_t kWmLButtonUp = 0x0202u;
constexpr std::uint32_t kWmRButtonDown = 0x0204u;
constexpr std::uint32_t kWmRButtonUp = 0x0205u;
constexpr std::uint32_t kWmMButtonDown = 0x0207u;
constexpr std::uint32_t kWmMButtonUp = 0x0208u;
constexpr std::uint32_t kWmMouseWheel = 0x020Au;
constexpr std::uint32_t kMkLButton = 0x0001u;
constexpr std::uint32_t kMkRButton = 0x0002u;
constexpr std::uint32_t kMkMButton = 0x0010u;

// 按钮 ↔ VK / MK_* 位 / WM_* 消息（**唯一一份**映射：`host_window.cpp` 的按钮状态也读它）。
std::uint32_t vkForMouseButton(MouseButton b);
std::uint32_t flagForMouseButton(MouseButton b);
std::uint32_t messageForMouseButton(MouseButton b, bool down);

// 应用一次输入。`client_x/client_y` 是 guest 客户区坐标；`time_ms` 进 `MSG.time`。
// 返回 false = 这次输入被**所有者**拒绝（脚本拥有该键 / 光标而来源是 OS）——调用方据此决定
// 要不要打日志；状态与队列都**没有**被改动（一致：不会出现"消息动了、电平没动"）。
bool mouseMove(WindowManager& wm, int client_x, int client_y, InputSource src, std::uint32_t time_ms);
bool mouseButton(WindowManager& wm, int client_x, int client_y, MouseButton b, bool down, InputSource src,
                 std::uint32_t time_ms);
bool mouseWheel(WindowManager& wm, int client_x, int client_y, int delta_120, InputSource src,
                std::uint32_t time_ms);
bool key(WindowManager& wm, std::uint32_t vk, bool down, std::uint32_t scancode, InputSource src,
         std::uint32_t time_ms);

}  // namespace input
}  // namespace lift
