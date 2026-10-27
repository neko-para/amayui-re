#pragma once

#include "core/vm.h"

#include <cstdint>
#include <string>

namespace core {

// `0x204` 的平台侧：把一段 Shift-JIS 字节串逐字画进某个纹理槽的纹理。
// 字体 / 颜色 / 绘制模式 / 对位模式都从引擎的字体对象读（⛔ 不是操作数）。
// 返回 false 表示没有字体后端 / 目标纹理不可用；错误说明写进 `error`（可为 nullptr）。
bool draw_text_into_texture(Machine& machine, std::uint32_t texture_id, int x, int y,
                            const std::string& sjis, std::string* error = nullptr);

// 引擎的字形写入规则（`sub_46D9F0`）：满覆盖 ⇒ 覆写；否则 outA = max(dstA, srcA)、
// outC = (srcC*srcA + dstC*(255-srcA))/255（⛔ 不是 alpha-over，重复画不累加）。
// `texel` 是 RGBA 四字节；给自检 / 诊断用。
void compose_glyph_texel(std::uint8_t* texel, int r, int g, int b, int alpha);

// 本次构建是否带字体后端（FreeType + libiconv）。
bool text_backend_available();

// 最近一次 `0x204` 真正用的字形来源（诊断 / 守卫用；⛔ 不是产品选择器）。
// `FreeType` = 宿主回退；`Gdi` = 真机路径（Windows + `EMULATOR_CXX_GLYPH_GDI`）；`Bake` 目前不会出现
// （loader 是 stub，会退回 `FreeType`）。
enum class TextGlyphSource { None, FreeType, Gdi };
TextGlyphSource last_glyph_source();

}  // namespace core
