// 纹理槽 / 绘制项 / 网格 / 颜色窗 / 变换：1F6 清表 · 1F7 拆离 · 1F8 建空白 · 1F9 设纹理 ·
// 1FA 释放 · 1FB 绘制项 · 202 / 203 颜色 · 239 源矩形窗 · 320 / 322 / 323 网格 ·
// 1FD / 1FE / 1FF 场景对象变换 · 344 辅助表 A。
//
// ★ 本文件只有 handler 本体；分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"
#include "core/vm_internal.h"
#include "core/agf.h"
#include "core/install.h"

namespace core {

void Machine::op_344(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 辅助表 A（Live2D 条目不透明桶）
    const auto value = read_int(ops[1]);
    const auto key = read_int(ops[0]);
    aux_a_[key] = {1, value};
    return;
}

void Machine::op_1ff(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 平移矩阵
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto x = read_float(ops[1]);
    const auto y = read_float(ops[2]);
    const auto z = read_float(ops[3]);
    SceneRecord& rec = scene_records_[key];
    rec.translate[0] = static_cast<float>(x);
    rec.translate[1] = static_cast<float>(y);
    rec.translate[2] = static_cast<float>(z);
    return;
}

void Machine::op_1fd(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 缩放矩阵：百分之一
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto x = read_float(ops[1]) / 100.0;
    const auto y = read_float(ops[2]) / 100.0;
    const auto z = read_float(ops[3]) / 100.0;
    SceneRecord& rec = scene_records_[key];
    rec.scale[0] = static_cast<float>(x);
    rec.scale[1] = static_cast<float>(y);
    rec.scale[2] = static_cast<float>(z);
    return;
}

void Machine::op_1fe(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 旋转矩阵：轴 + 角度（度）
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto ax = read_float(ops[1]);
    const auto ay = read_float(ops[2]);
    const auto az = read_float(ops[3]);
    const auto degrees = read_float(ops[4]);
    SceneRecord& rec = scene_records_[key];
    rec.rotate_axis[0] = static_cast<float>(ax);
    rec.rotate_axis[1] = static_cast<float>(ay);
    rec.rotate_axis[2] = static_cast<float>(az);
    rec.rotate_degrees = static_cast<float>(degrees);
    return;
}

void Machine::op_1f6(const Instruction& instruction) {
    (void)instruction;
    draw_items_.clear(); meshes_.clear(); aux_a_.clear(); aux_b_.clear(); scene_dirty_ = false; return;  // 清四张同键表；脏标记清 0（不是置 1）
}

void Machine::op_320(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 建/换网格：保存逐顶点位置 / 颜色
    const auto count = read_int(ops[8]);
    const auto op10 = read_int(ops[9]);
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    if (count <= 0) return;
    MeshRecord& mesh = meshes_[key];
    mesh.flags |= 1u;
    mesh.count = count;
    mesh.op10 = op10;
    mesh.copies[0].clear();
    mesh.copies[1].clear();
    mesh.colors.clear();
    const auto address0 = address_of(ops[1]);
    const auto address1 = address_of(ops[2]);
    const auto address2 = address_of(ops[4]);
    const auto address3 = address_of(ops[5]);
    for (std::int32_t i = 0; i < count; ++i) {
        const auto step = static_cast<std::uint32_t>(i) * 4u;
        mesh.copies[0].push_back(read_raw(address0 + step));
        mesh.copies[1].push_back(read_raw(address1 + step));
        const auto high = as_u32(read_int_at(address2 + step)) & 0xFFu;
        const auto low = as_u32(read_int_at(address3 + step)) & 0xFFFFFFu;
        mesh.colors.push_back((high << 24) | low);
    }
    return;
}

void Machine::op_322(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 网格当前颜色
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    (void)read_int(ops[1]);
    const auto alpha = read_int(ops[2]);
    const auto color = read_int(ops[3]);
    MeshRecord& mesh = meshes_[key];
    mesh.color = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
    mesh.target_color = mesh.color;
    for (auto& value : mesh.colors) value = mesh.color;
    return;
}

void Machine::op_323(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 网格颜色窗
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto delay = read_int(ops[1]);
    const auto duration = read_int(ops[2]);
    const auto alpha = read_int(ops[3]);
    const auto color = read_int(ops[4]);
    MeshRecord& mesh = meshes_[key];
    mesh.flags |= 2u;
    mesh.delay = delay;
    mesh.duration = duration;
    mesh.target_color = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
    mesh.window_start = 0;  // 未盖章
    return;
}

void Machine::op_1fb(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 绘制纹理：只入队
    (void)read_int(ops[5]);
    (void)read_int(ops[3]);
    (void)read_int(ops[4]);
    (void)read_int(ops[2]);
    const auto left = read_int(ops[2]);
    const auto top = read_int(ops[3]);
    const auto width = read_int(ops[4]);
    const auto height = read_int(ops[5]);
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    DrawItem& item = draw_items_[key];
    item.flags |= 1u;
    item.texture_id = static_cast<std::uint32_t>(read_int(ops[1]));
    item.left = left;
    item.top = top;
    item.right = left + width;
    item.bottom = top + height;
    item.aux[0] = static_cast<float>(read_int(ops[6]));
    item.aux[1] = static_cast<float>(read_int(ops[7]));
    item.aux[2] = 0.0f;
    return;
}

void Machine::op_239(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 源矩形窗口：读序 6 → 5 → 4 → 3 → 2 → 1；「已存在」位为 0 ⇒ 一个字段都不写
    const auto rect_switch = read_int(ops[5]);
    const auto row_steps = read_int(ops[4]);
    const auto step_total = read_int(ops[3]);
    const auto duration = read_int(ops[2]);
    const auto delay = read_int(ops[1]);
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    DrawItem& item = draw_items_[key];
    if ((item.flags & 1u) == 0) return;
    item.flags |= 2u;
    item.window_start = 0;
    item.rect_delay = delay;
    item.rect_duration = duration;
    item.rect_step_total = step_total;
    item.rect_row_steps = row_steps;
    item.rect_switch = rect_switch;
    return;
}

void Machine::op_202(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 颜色窗：已存在位为 1 才排窗
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto delay = read_int(ops[1]);
    const auto duration = read_int(ops[2]);
    const auto alpha = read_int(ops[3]);
    const auto color = read_int(ops[4]);
    auto it = draw_items_.find(key);
    if (it != draw_items_.end() && (it->second.flags & 1u) != 0) {
        it->second.flags |= 2u;
        it->second.delay = delay;
        it->second.duration = duration;
        it->second.target_color = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
        it->second.window_start = 0;  // 未盖章
    }
    return;
}

void Machine::op_203(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 直接设当前颜色
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    (void)read_int(ops[1]);
    const auto alpha = read_int(ops[2]);
    const auto color = read_int(ops[3]);
    DrawItem& item = draw_items_[key];
    item.color = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
    return;
}

void Machine::op_1f7(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 按键拆离：四张同键表（绘制项 / 网格 / 辅助 A / 辅助 B）一起删；场景命令表不动
    const auto count = read_int(ops[1]);
    const auto key = read_int(ops[0]);
    bool erased = false;
    const auto erase_one = [&](std::int32_t slot_key) {
        const auto index = static_cast<std::uint32_t>(slot_key);
        if (draw_items_.erase(index) > 0) erased = true;
        if (meshes_.erase(index) > 0) erased = true;
        if (aux_a_.erase(slot_key) > 0) erased = true;
        if (aux_b_.erase(slot_key) > 0) erased = true;
    };
    // 负数个数也走「≤ 1」的单键路径；区间是左闭右开。
    if (count <= 1) {
        erase_one(key);
    } else {
        for (std::int32_t i = 0; i < count; ++i) erase_one(key + i);
    }
    // 只有真的删掉东西才标脏（没删 ⇒ 脏标记不动）。
    if (erased) scene_dirty_ = true;
    return;
}

void Machine::op_1f8(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 空白建纹理
    const auto id = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto width = read_int(ops[1]);
    const auto height = read_int(ops[2]);
    (void)read_int(ops[3]);
    TextureRecord& record = textures_[id];
    record.file = -1;
    record.blank = true;
    record.present = true;
    record.revision++;
    record.width = static_cast<std::uint32_t>(width);
    record.height = static_cast<std::uint32_t>(height);
    record.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
    record.revision = 1;
    return;
}

void Machine::op_1fa(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 释放纹理
    const auto id = static_cast<std::uint32_t>(read_int(ops[0]));
    (void)read_int(ops[0]);
    textures_.erase(id);
    return;
}

void Machine::op_1f9(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 设置纹理：读序 2 → 1 → 3 → 2；无平台上传，只建记录
    const auto id = read_int(ops[1]);
    const auto file = read_int(ops[0]);
    const auto raw_key = read_int(ops[2]);
    (void)read_int(ops[1]);
    if (install_ == nullptr) throw CoreError("texture-loader-missing");
    const core::Bytes bytes = core::read_install_file(*install_, static_cast<std::uint32_t>(file));
    const core::DecodedAgf image = core::decode_agf(bytes);
    TextureRecord record;
    record.file = file;
    record.color_key = raw_key < 0 ? 0u : (0xFF000000u | (static_cast<std::uint32_t>(raw_key) & 0xFFFFFFu));
    record.width = image.width;
    record.height = image.height;
    record.rgba = image.rgba;
    if (record.color_key != 0) {
        for (std::size_t i = 0; i + 3 < record.rgba.size(); i += 4) {
            const auto argb = (static_cast<std::uint32_t>(record.rgba[i + 3]) << 24) |
                              (static_cast<std::uint32_t>(record.rgba[i]) << 16) |
                              (static_cast<std::uint32_t>(record.rgba[i + 1]) << 8) |
                              record.rgba[i + 2];
            if (argb == record.color_key) {
                record.rgba[i] = 0; record.rgba[i + 1] = 0; record.rgba[i + 2] = 0; record.rgba[i + 3] = 0;
            }
        }
    }
    record.present = true;
    record.revision++;
    textures_[static_cast<std::uint32_t>(id)] = record;
    return;
}

void Machine::op_208(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 查询纹理尺寸（CORE-OPS-TEXTURE-SIZE）：编号（读）→ 宽（写）→ 高（写）。
    const auto id = read_int(ops[0]);
    // ★ 上界是**无符号**比较：id > 999（负值也算越界）⇒ 上报一条 + 两个输出写 0。
    if (static_cast<std::uint32_t>(id) > 999u) {
        report_error("texture-id-out-of-range:" + std::to_string(id));
        write_int(ops[1], 0);
        write_int(ops[2], 0);
        return;
    }
    // 「槽空」与「越界」不是同一失败：槽空静默、两个输出 0。
    const TextureRecord* found = texture(static_cast<std::uint32_t>(id));
    write_int(ops[1], found == nullptr ? 0 : static_cast<std::int32_t>(found->width));
    write_int(ops[2], found == nullptr ? 0 : static_cast<std::int32_t>(found->height));
    return;
}

}  // namespace core
