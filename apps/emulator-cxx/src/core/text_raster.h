#pragma once

#include "core/vm.h"

#include <string>

namespace core {

// `0x204` 的平台侧：把一段 Shift-JIS 字节串逐字画进某个纹理槽的纹理。
// 字体 / 颜色 / 绘制模式 / 对位模式都从引擎的字体对象读（⛔ 不是操作数）。
// 返回 false 表示没有字体后端 / 目标纹理不可用；错误说明写进 `error`（可为 nullptr）。
bool draw_text_into_texture(Machine& machine, std::uint32_t texture_id, int x, int y,
                            const std::string& sjis, std::string* error = nullptr);

// 本次构建是否带字体后端（FreeType + libiconv）。
bool text_backend_available();

}  // namespace core
