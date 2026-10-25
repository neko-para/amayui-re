// 渲染入口：0x20C 帧步进 · 0x20D 渲染目标 · 0x204 离屏文字 · 0x33F 场景混合，
// 外加同一批"呈现前写场景"的入口：0x20B 纹理矩形填充 · 0x23B CG 数字条 · 0x2DA CG 参数。
//
// ★ 本文件只有 handler 本体；分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"
#include "core/vm_internal.h"
#include "core/text_raster.h"

#include <algorithm>
#include <cmath>

namespace core {
namespace {
// 纹理矩形填充（0x20B）的通道夹取。
std::uint8_t clamp_byte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}
}  // namespace

void Machine::op_23b(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // CG 数字条：建绘制项（像素输出未建模）
    const auto id = read_int(ops[1]);
    if (id < 0 || id > 10) return;
    if (id < 10 && cg_params_[static_cast<std::size_t>(id)][0] == 0) return;
    const auto start = read_int(ops[0]);
    const auto value = read_int(ops[2]);
    const auto digits = read_int(ops[5]);
    const auto flags = read_int(ops[6]);
    if (digits > 0) {
        for (std::int32_t i = 0; i < digits; ++i) draw_items_.erase(static_cast<std::uint32_t>(start + i));
    }
    if (digits <= 0) return;
    const auto target_x = read_int(ops[3]);
    const auto target_y = read_int(ops[4]);
    const auto& cg = cg_params_[static_cast<std::size_t>(id)];
    const std::int32_t cell = cg[3] + cg[6];
    const std::int32_t number_digits = value == 0 ? 1 : static_cast<std::int32_t>(std::to_string(std::abs(value)).size());
    for (std::int32_t j = 0; j < digits; ++j) {
        const auto digit = static_cast<std::int32_t>((std::abs(value) / static_cast<std::int32_t>(std::pow(10, j))) % 10);
        const std::int32_t index = digits - 1 - j;
        std::int32_t x = target_x + index * cell;
        if ((flags & 2) != 0) x -= (digits - number_digits) * cell;
        else if ((flags & 4) != 0) x -= 0;
        DrawItem& item = draw_items_[static_cast<std::uint32_t>(start + j)];
        item.flags |= 1u;
        item.texture_id = static_cast<std::uint32_t>(cg[0]);
        item.left = cg[1] + cell * digit;
        item.top = cg[2];
        item.right = item.left + cg[3];
        item.bottom = item.top + cg[4];
        item.aux[0] = static_cast<float>(x);
        item.aux[1] = static_cast<float>(target_y);
        item.aux[2] = 0.0f;
    }
    return;
}

void Machine::op_2da(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // CG 参数记录七格
    const auto id = read_int(ops[0]);
    if (id < 0 || id > 10) return;
    for (std::size_t i = 0; i < 7; ++i) cg_params_[static_cast<std::size_t>(id)][i] = read_int(ops[i + 1]);
    return;
}

void Machine::op_20b(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 纹理矩形填充：夹到逻辑矩形，颜色 = (颜色 & 0xFFFFFF) | 0xFF000000
    const auto id = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto x = read_int(ops[1]);
    const auto y = read_int(ops[2]);
    const auto w = read_int(ops[3]);
    const auto h = read_int(ops[4]);
    auto alpha = read_int(ops[5]);
    const auto color = as_u32(read_int(ops[6])) & 0xFFFFFFu;
    if (alpha > 255) alpha = 255;
    if (id > 999u) return;
    auto it = textures_.find(id);
    if (it == textures_.end()) return;
    TextureRecord& texture = it->second;
    texture.non_file = true;
        texture.revision++;
    if (texture.rgba.empty()) return;
    const int left = std::max(0, x);
    const int top = std::max(0, y);
    const int right = std::min(static_cast<int>(texture.width), x + w);
    const int bottom = std::min(static_cast<int>(texture.height), y + h);
    if (right <= left || bottom <= top) return;
    const int a = std::max(0, alpha);
    const int sr = static_cast<int>((color >> 16) & 0xFFu);
    const int sg = static_cast<int>((color >> 8) & 0xFFu);
    const int sb = static_cast<int>(color & 0xFFu);
    for (int py = top; py < bottom; ++py) {
        for (int px = left; px < right; ++px) {
            const std::size_t d = (static_cast<std::size_t>(py) * texture.width + px) * 4;
            texture.rgba[d + 0] = clamp_byte((sr * a + texture.rgba[d + 0] * (255 - a)) / 255);
            texture.rgba[d + 1] = clamp_byte((sg * a + texture.rgba[d + 1] * (255 - a)) / 255);
            texture.rgba[d + 2] = clamp_byte((sb * a + texture.rgba[d + 2] * (255 - a)) / 255);
            texture.rgba[d + 3] = clamp_byte(a + texture.rgba[d + 3] * (255 - a) / 255);
        }
    }
    return;
}

void Machine::op_20c(const Instruction& instruction) {
    (void)instruction;
    // 帧步进：时间戳 + 一次渲染请求 + 清帧标志
    if (!driven_elsewhere_) {
        engine_previous_ms_ = engine_now_ms_;
        engine_now_ms_ = scene_clock_ms_;
    }
    frame_step_requested_ = true;
    frame_flag_ = false;
    return;
}

void Machine::op_20d(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 渲染目标：<1000 纹理槽，>=1000 后缓冲
    const auto id = read_int(ops[0]);
    if (static_cast<std::uint32_t>(id) < 1000u) {
        if (textures_.find(static_cast<std::uint32_t>(id)) != textures_.end()) render_target_ = id;
    } else {
        render_target_ = -1;
    }
    return;
}

void Machine::op_33f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 场景混合：alpha 先读、color 再读、mode 最后
    auto alpha = read_int(ops[1]);
    auto color = read_int(ops[2]);
    const auto mode = read_int(ops[0]);
    const auto key = static_cast<std::uint32_t>(mode);
    const auto item = draw_items_.find(key);
    if (alpha < 0) alpha = item == draw_items_.end() ? 255 : static_cast<std::int32_t>((item->second.color >> 24) & 0xFFu);
    if (color < 0) color = item == draw_items_.end() ? static_cast<std::int32_t>(0xFFFFFFFFu) : static_cast<std::int32_t>(item->second.color);
    if (alpha > 255) alpha = 255;
    blend_mode_ = mode;
    blend_color_ = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
    return;
}

void Machine::op_204(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 离屏文字：读序 3 → 2 → 4 → 1；三条入口条件在请求之前
    const auto y = read_int(ops[2]);
    const auto x = read_int(ops[1]);
    const auto text = read_text(ops[3]);
    const auto id = read_int(ops[0]);
    if (id < 0) return;
    const auto texture_id = static_cast<std::uint32_t>(id);
    const TextureRecord* texture = this->texture(texture_id);
    if (texture == nullptr || texture->rgba.empty() || text.empty()) return;
    std::string error;
    if (draw_text_into_texture(*this, texture_id, x, y, text, &error)) {
        if (TextureRecord* mutable_texture = texture_mut(texture_id)) mutable_texture->revision++;
    } else if (!error.empty()) {
        report_error(error);
    }
    return;
}

}  // namespace core
