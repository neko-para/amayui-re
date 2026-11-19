// emulator-lift · 系统剪贴板（**SDL-free 声明**；唯一 include `<SDL3/SDL_clipboard.h>` 的 TU 是 `.cpp`）
//
// ★ 为什么单独一个 TU：`aot_host.cpp` 会 include 本头（overlay 的 PROFILE 桥在那里写剪贴板），
//   而 AOT 臂从不直接碰 SDL 类型（现状：`aot_host.cpp` 里 0 个 `SDL_`）。
//   `emulator-lift-runtime` 已经 `PUBLIC SDL3::SDL3` ⇒ 不新增任何依赖。
// ★ 线程：SDL 文档写"main thread only"。调用点 = `drawOverlay`（Present 主线程）⇒ 满足；
//   ⛔ 不要在 `handleEvent` 里调（裁决 4 已禁；而且它可能跑在 guest 的 `GetMessageA` shim 里）。
// ★ 判据：true = 已写入；false = 没写（`*err` 填 `SDL_SetClipboardText: <SDL_GetError()>`）。
//   本函数**不读回**系统剪贴板（那是环境面，不是我们的代码）。
#pragma once

#include <string>
#include <string_view>

namespace lift {

bool setSystemClipboard(std::string_view text, std::string* err);

}  // namespace lift
