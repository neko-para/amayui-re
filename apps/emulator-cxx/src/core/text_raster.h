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
    // 追加当时那一支字体对象的**字面名**（`ADV-TEXT-LAYER` 的位 2 = 变体选择 ⇒ 整支字体对象换）。
    // ★ 空串 = 没带（沿用 `Machine::font_string()`）。产品路径的宿主替换下名字不参与选择
    //   （SCOPE §0 D1），所以它只影响**显式对照档** `EMULATOR_CXX_GLYPH_GDI_FACE=engine` 取哪个面名。
    std::string face;
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

// ── 平台字体度量的**产品装配**（`CORE-OPS-FORWARD-PROLOGUE`「非 0 支的下一格来自度量」）──
// 量的是**渲染字体**（宿主替换后的 Amayui CN，见 `SCOPE.md` §1.1），⛔ 不查引擎的字面名 ——
// 所以请求面名（参考字体常量 / 描述自己的字面名）**只出现在请求里**，不决定量哪份字体。
// Windows 走 GDI（`GetTextMetricsW` + `GetGlyphOutlineW` 的黑框 + `GetTextExtentPoint32W`）；
// 其它平台走 FreeType 近似 —— ★ 这是**平台差异**（正文 `60-ops-forward.md:112`："这一支的写入值是平台输入"），
// 不是引擎语义，也⛔ 不代表"与真机数值等价"。
Machine::FontMetrics measure_rendering_font(std::int32_t size);

// 把 `measure_rendering_font` 包成 `Machine` 的度量接缝（`set_font_measurer` 的实参）。
Machine::FontMeasurer make_platform_font_measurer();

// 重建入口那一次「建参考字体 + 取量度」的产品装配（`CORE-OPS-LIST-SUBSYSTEM-SET-TAIL` 的 ③；
// `set_font_reference_builder` 的实参）。★ 与 `make_platform_font_measurer` 不是同一件事：
// 那个服务派生例程的两次度量，这个服务入口自己那一次（写进对象自己的两张量度结构）。
// 请求的**形态**（面名 / 高 / 宽 / 粗细 / 字符集）由引擎给、写进 `FontBuildSpec`；
// 实际量哪份字体仍按宿主替换策略（SCOPE §0 D1）—— 平台差异，⛔ 不代表与真机数值等价。
Machine::FontReferenceBuilder make_platform_reference_builder();

// 产品路径的字体装配：**装度量后端 + 打开宿主字体替换开关**（SCOPE §0 D1：名字不参与选择）。
// ★ `boot.cpp` 与 `core_selftest.cpp` 都调它 ⇒ 守卫断的是**同一条装配路径**（⛔ 不是自测里的旁路）。
void wire_platform_fonts(Machine& machine);

// 最近一次 `0x204` 真正用的字形来源（诊断 / 守卫用；⛔ 不是产品选择器）。
// `FreeType` = 宿主回退；`Gdi` = 真机路径（Windows + `EMULATOR_CXX_GLYPH_GDI`）；`Bake` 目前不会出现
// （loader 是 stub，会退回 `FreeType`）。
enum class TextGlyphSource { None, FreeType, Gdi };
TextGlyphSource last_glyph_source();

}  // namespace core
