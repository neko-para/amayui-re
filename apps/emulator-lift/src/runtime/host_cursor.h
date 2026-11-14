// emulator-lift · 宿主光标原语（平台层；`--cursor-warp` 的唯一 OS 出口）
//
// ★ 为什么单独一个文件：这是本线**唯一**会改变用户桌面光标位置的地方。它必须是
//   ① 一眼能看全的单 TU（`#ifdef` 选真实现，⛔ 不按平台拆源文件 —— 非本平台的空实现也要过编译，
//      见旧仓 `native/host-input/src/host_input.h:26-30` 的同一口径）；
//   ② **SDL-free**：`WindowManager` / pure CTest 只 include 这一个头，不把 SDL 拉进来。
//
// ★ 坐标口径：**宿主屏幕坐标**（物理像素虚拟屏；Win32 = 屏幕坐标，可负 / 越界；macOS = 全局显示
//   point，左上原点）。⛔ 不做 DPI / DIP 换算（窗口坐标本身就是同一空间，见 design §2.3 依据 2/3）。
// ★ 返回值诚实：系统拒绝 / 平台不支持 ⇒ false。⛔ 不许"返回 true 但没动"。
//   Windows 的 RDP / 锁屏等场景系统可能"成功但没动" ⇒ 兜底是调用方**一次性**的 `hostCursorQueryScreen`
//   读回自检（`present_sdl.cpp` 的 `hostCursorWarp`）。
#pragma once

namespace lift {

enum class HostCursorPlatform { Unsupported = 0, Win32, MacOS };

HostCursorPlatform hostCursorPlatform();
// Win32 / macOS ⇒ true；Linux 与其它 ⇒ false（`main.cpp` 据此在启动期 exit 2）。
bool hostCursorPlatformSupported();
const char* hostCursorPlatformName();  // "win32" / "macos" / "unsupported"

// 宿主屏幕坐标 → **真的**挪光标。false = 平台不支持 / 系统拒绝。
bool hostCursorWarpToScreen(int screen_x, int screen_y);
// 读回宿主光标（**只用于一次性自检**，⛔ 不在每帧路径上、⛔ 不参与 guest 语义）。
bool hostCursorQueryScreen(int* screen_x, int* screen_y);

}  // namespace lift
