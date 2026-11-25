// emulator-lift · 叠层热键的**读数**语义（D4；2026-11-25 父级裁决）
//
// ★ 为什么单独一个头（⛔ 不放 `runtime/ui_input.h`）：那个头只描述**注入指针的叠层路由**
//   （`--ui-input=consume|pass` 的 sink 接口）；本头是"present 层每帧采一次键电平 ⇒ 脚本侧
//   一个单调计数"的读数语义，两者的消费者 / 判据都不同。本头 SDL-free、零依赖 ⇒ pure 单测
//   （`overlay` CTest 的 `overlay/key-*` 用例）直接判。
//
// ★ 读数是**单调按下沿计数**（不是电平）：插件按"变了"来切换显隐；电平会每帧重复触发。
// ★ **不吞键**：F9 仍到 guest（吞键要么改 `input::key` 漏斗 —— OS + 注入两路 —— 要么在
//   `handleEvent` 单点吞而注入路径绕过；两条都不值）。与游戏的冲突风险记在文档里。
// ★ 本期固定 F9，⛔ 不加 CLI（换键留给 `--overlay-key`，记进 `## 未做`）。
// ★ 采样点 = `present_sdl.cpp` 的 `drawOverlay`（唯一的每帧渲染入口），来源 =
//   `WindowManager::keyDown(kOverlayKeyVk)`（与 guest 的 `GetAsyncKeyState` 同一张电平表）；
//   没有 `WindowManager` ⇒ 不填读数 ⇒ 脚本侧 `undefined`（⛔ 不编 0）。
#pragma once

#include <cstdint>

namespace lift {

// VK_F9（Win32 虚拟键码；与 `WindowManager::keyDown` 的键空间同一个）。
inline constexpr std::uint32_t kOverlayKeyVk = 0x78;

// 名字只给文档 / 诊断（⛔ 不做 VK 名表的第二份真源）。
inline constexpr const char* kOverlayKeyName = "F9";

// 按下沿 +1、饱和不加（⛔ 回绕）：**纯函数**（present_sdl 每帧调一次；pure CTest 直接判）。
//   · prev=false, down=true  ⇒ +1（一次按下）
//   · 其余（保持 / 释放 / 空闲）⇒ 不变
inline std::uint32_t overlayKeyStep(bool prev_down, bool down, std::uint32_t seq) {
    if (!down || prev_down) return seq;
    if (seq == 0xFFFFFFFFu) return seq;  // 饱和：4_294_967_295 次按下之后不再增长
    return seq + 1u;
}

}  // namespace lift
