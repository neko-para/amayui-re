#pragma once

#include "core/vm.h"

#include <cstdint>
#include <string>

namespace core {

// **追加当时的字形样式快照**（文本层的跨帧队列用）。给了它 ⇒ 字号 / 模式 / 两色都按它画，
// ⛔ 不再读字体对象。理由：引擎在**追加时**就量度 + 逐字写出（`sub_46BE30` 的 SelectObject /
// 逐字写出），之后的推进只呈现已经排好版的格；画的时候再读字体对象会让"追加之后、这一格画出来
// 之前"的 `0x75/0x76/0x77` 写到**别的行**上（实测：示例文本里一个字按 20 号画出来、字宽却是 30）。
struct TextStyleSnapshot {
    int size = 0;
    int weight = 0;  // 粗细（`0x2bd` 写的那两格）：决定用 Bold 还是 Regular 字体文件
    int draw_mode = 0;
    std::int32_t primary = 0;
    std::int32_t secondary = 0;
};

// `0x204` 的平台侧：把一段 Shift-JIS 字节串逐字画进某个纹理槽的纹理。
// 字体 / 颜色 / 绘制模式 / 对位模式都从引擎的字体对象读（⛔ 不是操作数）。
// 返回 false 表示没有字体后端 / 目标纹理不可用；错误说明写进 `error`（可为 nullptr）。
// `advance_out`（可选）：返回本次绘制实际推进的像素宽度（= 结束时 pen_x − x）。
// `font_size_override`（可选）：> 0 时覆盖字号（文本层振り仮名用 B 支字号；0 = 沿用 A 支）。
// `style`（可选）：追加当时的样式快照；非空 ⇒ 按它画，不再读字体对象。
bool draw_text_into_texture(Machine& machine, std::uint32_t texture_id, int x, int y,
                            const std::string& sjis, std::string* error = nullptr,
                            int* advance_out = nullptr, int font_size_override = 0,
                            const TextStyleSnapshot* style = nullptr);

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
