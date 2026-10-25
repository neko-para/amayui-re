// 场景命令记录：0x223 种类 0（纹理覆盖）· 0x24D 种类 1（影片覆盖）· 0x24F 种类 2（抹除）·
// 0x250 / 0x251 种类 3 的两个变体（滑移模糊 / 缩放模糊）。
//
// ★ 本文件只有 handler 本体；读操作数 / 写表 / 标脏都走 Machine 的既有原语；
//   分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"

namespace core {

void Machine::op_223(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 种类 0：纹理覆盖命令（先建 / 换纹理请求，再替换记录）
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto texture_id = static_cast<std::uint32_t>(read_int(ops[1]));
    const auto a_start = read_int(ops[2]);
    const auto a_count = read_int(ops[3]);
    const auto b_start = read_int(ops[4]);
    const auto b_count = read_int(ops[5]);
    const auto delay = read_int(ops[6]);
    const auto duration = read_int(ops[7]);
    if (textures_.find(texture_id) == textures_.end()) {
        TextureRecord& texture = textures_[texture_id];
        texture.blank = true;
        texture.non_file = true;
        texture.revision++;
        texture.present = true;
        texture.width = 1280;
        texture.height = 720;
        texture.rgba.assign(static_cast<std::size_t>(texture.width) * texture.height * 4, 0);
    }
    SceneCommandRecord& record = scene_commands_[key];
    record = SceneCommandRecord{};
    record.kind = 0;
    record.key = key;
    record.texture_id = texture_id;
    record.a_start = a_start;
    record.a_count = a_count;
    record.b_start = b_start;
    record.b_count = b_count;
    record.delay = delay;
    record.duration = duration;
    return;
}

void Machine::op_24d(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 种类 1：影片覆盖命令；第 2 个操作数必须是已存在的影片纹理
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto texture_id = static_cast<std::uint32_t>(read_int(ops[1]));
    const auto a_start = read_int(ops[2]);
    const auto a_count = read_int(ops[3]);
    const auto left = read_int(ops[4]);
    const auto top = read_int(ops[5]);
    const auto width = read_int(ops[6]);
    const auto height = read_int(ops[7]);
    const auto mask_flag = read_int(ops[8]);
    (void)read_int(ops[9]);  // 影片名表索引（不写记录）
    const auto delay = read_int(ops[10]);
    const auto duration = read_int(ops[11]);
    if (textures_.find(texture_id) == textures_.end()) {
        throw CoreError("movie-texture-slot-empty:" + std::to_string(texture_id));
    }
    SceneCommandRecord& record = scene_commands_[key];
    record = SceneCommandRecord{};
    record.kind = 1;
    record.key = key;
    record.texture_id = texture_id;
    record.a_start = a_start;
    record.a_count = a_count;
    record.delay = delay;
    record.duration = duration;
    record.rect[0] = left;
    record.rect[1] = top;
    record.rect[2] = left + width;
    record.rect[3] = top + height;
    record.mask_flag = mask_flag;
    command_animating_ = true;  // 写表时置「有动画」
    return;
}

void Machine::op_24f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 种类 2：抹除命令（先校验、后写表）
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto texture_id = static_cast<std::uint32_t>(read_int(ops[1]));
    auto a_start = read_int(ops[2]);
    auto a_count = read_int(ops[3]);
    auto b_start = read_int(ops[4]);
    auto b_count = read_int(ops[5]);
    auto wipe_type = read_int(ops[6]);
    auto split = read_int(ops[7]);
    auto delay = read_int(ops[8]);
    auto duration = read_int(ops[9]);
    if (split < 1) {
        report_error("分割幅が０以下です");
        split = 1;
        wipe_type = -1;
        delay = 0;
        duration = 0;
    } else if (wipe_type < 0 || wipe_type > 11) {
        report_error("タイプが不正です");
        return;  // 一个字段都不写
    }
    if (textures_.find(texture_id) == textures_.end()) {
        TextureRecord& texture = textures_[texture_id];
        texture.blank = true;
        texture.non_file = true;
        texture.revision++;
        texture.present = true;
        texture.width = 1280;
        texture.height = 720;
        texture.rgba.assign(static_cast<std::size_t>(texture.width) * texture.height * 4, 0);
    }
    SceneCommandRecord& record = scene_commands_[key];
    record = SceneCommandRecord{};
    record.kind = 2;
    record.key = key;
    record.texture_id = texture_id;
    record.a_start = a_start;
    record.a_count = a_count;
    record.b_start = b_start;
    record.b_count = b_count;
    record.wipe_type = wipe_type;
    record.split = split;
    record.delay = delay;
    record.duration = duration;
    return;
}

void Machine::op_250(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 种类 3 变体 0：滑移模糊（长度 / 角度）
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto texture_id = static_cast<std::uint32_t>(read_int(ops[1]));
    const auto a_start = read_int(ops[2]);
    const auto a_count = read_int(ops[3]);
    const auto from_length = read_int(ops[4]);
    const auto from_angle = read_int(ops[5]);
    const auto to_length = read_int(ops[6]);
    const auto to_angle = read_int(ops[7]);
    const auto delay = read_int(ops[8]);
    const auto duration = read_int(ops[9]);
    if (textures_.find(texture_id) == textures_.end()) {
        TextureRecord& texture = textures_[texture_id];
        texture.blank = true;
        texture.non_file = true;
        texture.revision++;
        texture.present = true;
        texture.width = 1280;
        texture.height = 720;
        texture.rgba.assign(static_cast<std::size_t>(texture.width) * texture.height * 4, 0);
    }
    SceneCommandRecord& record = scene_commands_[key];
    record = SceneCommandRecord{};
    record.kind = 3;
    record.key = key;
    record.texture_id = texture_id;
    record.a_start = a_start;
    record.a_count = a_count;
    record.delay = delay;
    record.duration = duration;
    record.variant = 0;
    record.from[0] = from_length;
    record.to[0] = to_length;
    record.from[1] = from_angle;
    record.to[1] = to_angle;
    return;
}

void Machine::op_251(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 种类 3 变体 1：缩放模糊（长度 / 中心横 / 中心纵）
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto texture_id = static_cast<std::uint32_t>(read_int(ops[1]));
    const auto a_start = read_int(ops[2]);
    const auto a_count = read_int(ops[3]);
    const auto from_length = read_int(ops[4]);
    const auto from_cx = read_int(ops[5]);
    const auto from_cy = read_int(ops[6]);
    const auto to_length = read_int(ops[7]);
    const auto to_cx = read_int(ops[8]);
    const auto to_cy = read_int(ops[9]);
    const auto delay = read_int(ops[10]);
    const auto duration = read_int(ops[11]);
    if (textures_.find(texture_id) == textures_.end()) {
        TextureRecord& texture = textures_[texture_id];
        texture.blank = true;
        texture.non_file = true;
        texture.revision++;
        texture.present = true;
        texture.width = 1280;
        texture.height = 720;
        texture.rgba.assign(static_cast<std::size_t>(texture.width) * texture.height * 4, 0);
    }
    SceneCommandRecord& record = scene_commands_[key];
    record = SceneCommandRecord{};
    record.kind = 3;
    record.key = key;
    record.texture_id = texture_id;
    record.a_start = a_start;
    record.a_count = a_count;
    record.delay = delay;
    record.duration = duration;
    record.variant = 1;
    record.from[0] = from_length;
    record.to[0] = to_length;
    record.from[1] = from_cx;
    record.to[1] = to_cx;
    record.from[2] = from_cy;
    record.to[2] = to_cy;
    return;
}

}  // namespace core
