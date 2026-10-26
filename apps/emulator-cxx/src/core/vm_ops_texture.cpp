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

#include <cmath>

namespace core {
namespace {

// 颜色归一化（`CORE-OPS-SCENE-202/203/322/323` 共用）：
// alpha > 255 ⇒ 255；alpha < 0 ⇒ 现有 alpha；color < 0 ⇒ 现有颜色；其余原样。
std::uint32_t normalized_color(int alpha, int color, std::uint32_t existing) {
    if (alpha > 255) alpha = 255;
    if (alpha < 0) alpha = static_cast<int>((existing >> 24) & 0xFFu);
    const auto rgb = color < 0 ? (existing & 0xFFFFFFu)
                               : (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
    return (static_cast<std::uint32_t>(alpha) << 24) | rgb;
}

}  // namespace

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

void Machine::op_23d(const Instruction& instruction) {
    (void)instruction;
    // 退出清理（CORE-OPS-MOVIE-CLEAR-23D）：销毁槽 42..999 的影片对象并清槽。
    // 同一次扫描也清掉这些槽的纹理记录来源文件格（场景侧解绑，`0x216` 可观察）；
    // 释放守卫非 0 的槽跳过（`CORE-OPS-MOVIE-CLEAR-23D` 的门的观察档）。
    for (auto& [slot, record] : textures_) {
        if (slot < 42u || slot >= 1000u || surface_release_gate_[slot] != 0) continue;
        record.file = -1;
        // 纹理槽对象也被释放（0x208 / 0x1fb 消费端看到空槽）；记录本身保留。
        record.present = false;
        record.width = 0;
        record.height = 0;
        record.rgba.clear();
    }
    // ★ 场景侧的槽→纹理绑定解绑未建模（见 RES-23D-SCENE-UNBIND）；纹理记录不在这里删除。
    bool erased = false;
    bool active = false;
    for (auto it = movies_.begin(); it != movies_.end();) {
        if (it->first < 42u) {
            if (it->second.present && it->second.started) active = true;
            ++it;
            continue;
        }
        if (it->second.backend) it->second.backend->close();
        it = movies_.erase(it);
        erased = true;
    }
    movie_active_ = active;
    if (erased) scene_dirty_ = true;
    return;
}

void Machine::op_32b(const Instruction& instruction) {
    (void)instruction;
    // 退出清理（CORE-OPS-SCENE-MESH-CLEAR-32B）：清空网格槽表 0..999。
    bool erased = false;
    for (auto it = meshes_.begin(); it != meshes_.end();) {
        if (it->first >= 1000u) {
            ++it;
            continue;
        }
        it = meshes_.erase(it);
        erased = true;
    }
    if (erased) scene_dirty_ = true;
    return;
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
    mesh.color = normalized_color(alpha, color, mesh.color);
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
    mesh.target_color = normalized_color(alpha, color, mesh.color);
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
        it->second.target_color = normalized_color(alpha, color, it->second.color);
        it->second.window_start = 0;  // 未盖章
    }
    return;
}

void Machine::op_203(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 直接设当前颜色（CORE-OPS-SCENE-203）：负 alpha / color 取现有值；第 2 个操作数落记录。
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto mode = read_int(ops[1]);
    const auto alpha = read_int(ops[2]);
    const auto color = read_int(ops[3]);
    DrawItem& item = draw_items_[key];
    item.set_color_int = mode;
    item.color = normalized_color(alpha, color, item.color);
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
    // 释放守卫非 0：只清共享对象槽，纹理记录 / 纹理槽都不动（CORE-OPS-TEXTURE-RELEASE）。
    if (id < surface_release_gate_.size() && surface_release_gate_[id] != 0) return;
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
    const bool present = found != nullptr && found->present;
    write_int(ops[1], present ? static_cast<std::int32_t>(found->width) : 0);
    write_int(ops[2], present ? static_cast<std::int32_t>(found->height) : 0);
    return;
}

void Machine::op_216(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 读纹理槽 → 文件绑定（CORE-OPS-TEXTURE-SLOT-IMGID）：先读槽号，再写目标；缺省 -1。
    const auto slot = read_int(ops[1]);
    // 槽号越界是未定义行为（RES-216-OUT-OF-RANGE）：⛔ 不复刻宿主布局，响亮失败。
    if (slot < 0 || static_cast<std::uint32_t>(slot) >= 1000u) {
        throw CoreError("texture-slot-out-of-range:" + std::to_string(slot));
    }
    const TextureRecord* record = texture(static_cast<std::uint32_t>(slot));
    write_int(ops[0], record == nullptr ? -1 : record->file);
    return;
}

void Machine::op_218(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 取第一三元向量（CORE-OPS-SCENE3D-GET-VEC1）：**只查不建**；查不到三个输出都写 0。
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    float values[3] = {0.0f, 0.0f, 0.0f};
    if (const SceneRecord* record = scene_record(key); record != nullptr) {
        values[0] = record->vec1[0];
        values[1] = record->vec1[1];
        values[2] = record->vec1[2];
    }
    write_float(ops[1], values[0]);
    write_float(ops[2], values[1]);
    write_float(ops[3], values[2]);
    return;
}

void Machine::op_21a(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 取第二三元向量（CORE-OPS-SCENE3D-GET-VEC2）：★ 与 0x218 读的是**不同的三个格**。
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    float values[3] = {0.0f, 0.0f, 0.0f};
    if (const SceneRecord* record = scene_record(key); record != nullptr) {
        values[0] = record->vec2[0];
        values[1] = record->vec2[1];
        values[2] = record->vec2[2];
    }
    write_float(ops[1], values[0]);
    write_float(ops[2], values[1]);
    write_float(ops[3], values[2]);
    return;
}

void Machine::op_228(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 取本地平移分量（CORE-OPS-SCENE3D-GET-LOCAL-TRANS）：读序 2 → 写 3 → 4 → 5 → 写 1（标志最后）。
    const auto key = static_cast<std::uint32_t>(read_int(ops[1]));
    const SceneRecord* record = scene_record(key);
    if (record == nullptr) {
        write_int(ops[0], 1);  // 静默失败：三个浮点一个都不写
        return;
    }
    write_float(ops[2], record->translate[0]);
    write_float(ops[3], record->translate[1]);
    write_float(ops[4], record->translate[2]);
    write_int(ops[0], 0);
    return;
}

void Machine::op_21d(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 记录复制（CORE-OPS-SCENE3D-21D）：第 2 个是**目标**、第 1 个是**源**（读序 2 → 1）。
    // 三张表互相独立；一张都没有源 ⇒ 上报错误通道且**一张表都不改**；成功才标脏（不置「有动画」）。
    const auto target = static_cast<std::uint32_t>(read_int(ops[1]));
    const auto source = static_cast<std::uint32_t>(read_int(ops[0]));
    bool copied = false;
    if (const DrawItem* item = draw_item(source); item != nullptr) {
        const DrawItem copy = *item;  // 先拷局部：目标键可能是新键，插入会 rehash
        draw_items_[target] = copy;
        copied = true;
    }
    if (const MeshRecord* mesh = this->mesh(source); mesh != nullptr) {
        const MeshRecord copy = *mesh;  // 值类型：顶点缓冲是新的一份
        meshes_[target] = copy;
        copied = true;
    }
    if (const auto it = aux_a_.find(static_cast<std::int32_t>(source)); it != aux_a_.end()) {
        aux_a_[static_cast<std::int32_t>(target)] = it->second;
        copied = true;
    }
    if (const auto it = aux_b_.find(static_cast<std::int32_t>(source)); it != aux_b_.end()) {
        aux_b_[static_cast<std::int32_t>(target)] = it->second;
        copied = true;
    }
    if (!copied) {
        report_error("scene-copy-source-missing:" + std::to_string(source));
        return;
    }
    scene_dirty_ = true;
    return;
}

DrawItem& Machine::draw_item_or_create(std::uint32_t key) {
    const auto it = draw_items_.find(key);
    if (it != draw_items_.end()) return it->second;
    // 建记录那一步标脏（`CORE-OPS-SCENE3D-RECORD-LOOKUP`）；默认记录见 `0x21e` 的规范引文。
    scene_dirty_ = true;
    return draw_items_[key];
}

std::array<float, 16> Machine::scale_matrix(double x, double y, double z) {
    std::array<float, 16> matrix{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    matrix[0] = static_cast<float>(x);
    matrix[5] = static_cast<float>(y);
    matrix[10] = static_cast<float>(z);
    return matrix;
}

std::array<float, 16> Machine::rotate_matrix_deg(double ax, double ay, double az, double degrees) {
    // ★ 规范只规定「度 → 弧度（× π/180）写进旋转矩阵」，**没有**规定轴归一化与元素布局 ⇒ 本实现
    //   选「轴按 hypot 归一化（全 0 时当作 1）+ Rodrigues + 行主序」。这是**实现选择**，不是规范结论。
    const double pi = 3.14159265358979323846;
    const double radians = degrees * pi / 180.0;
    double length = std::sqrt(ax * ax + ay * ay + az * az);
    if (length == 0.0) length = 1.0;
    const double x = ax / length;
    const double y = ay / length;
    const double z = az / length;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    const double t = 1.0 - c;
    std::array<float, 16> matrix{};
    matrix[0] = static_cast<float>(t * x * x + c);
    matrix[1] = static_cast<float>(t * x * y - s * z);
    matrix[2] = static_cast<float>(t * x * z + s * y);
    matrix[4] = static_cast<float>(t * x * y + s * z);
    matrix[5] = static_cast<float>(t * y * y + c);
    matrix[6] = static_cast<float>(t * y * z - s * x);
    matrix[8] = static_cast<float>(t * x * z - s * y);
    matrix[9] = static_cast<float>(t * y * z + s * x);
    matrix[10] = static_cast<float>(t * z * z + c);
    matrix[15] = 1.0f;
    return matrix;
}

void Machine::op_21e(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 缩放 setter（CORE-OPS-SCENE3D-21E）：读序 4 → 5 → 6 → 3 → 2 → 1（三个浮点先读）。
    const double z = read_float(ops[5]);
    const double y = read_float(ops[4]);
    const double x = read_float(ops[3]);
    const std::int32_t b = read_int(ops[2]);
    const std::int32_t a = read_int(ops[1]);
    DrawItem& item = draw_item_or_create(static_cast<std::uint32_t>(read_int(ops[0])));
    if ((item.flags & 1u) == 0) return;  // 「已存在」位为 0 ⇒ 一个字段都不写（记录已建）
    item.flags |= 2u;                    // 首字第 1 位（与 0x202 的「有窗」同一位）
    item.config_g3 = 1;
    item.window_start = 0;
    item.set21e_int0 = a;
    item.set21e_int1 = b;
    item.set21e_scale = scale_matrix(x / 100.0, y / 100.0, z / 100.0);  // ÷100 是语义（百分数）
    scene_dirty_ = true;
    scene_animating_ = true;
    return;
}

void Machine::op_21f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 旋转 setter（CORE-OPS-SCENE3D-21F）：读序 4 → 5 → 6 → 7 → 3 → 2 → 1。
    const double degrees = read_float(ops[6]);
    const double az = read_float(ops[5]);
    const double ay = read_float(ops[4]);
    const double ax = read_float(ops[3]);
    const std::int32_t b = read_int(ops[2]);
    const std::int32_t a = read_int(ops[1]);
    DrawItem& item = draw_item_or_create(static_cast<std::uint32_t>(read_int(ops[0])));
    if ((item.flags & 1u) == 0) return;
    item.flags |= 2u;
    item.config_g3 = 1;
    item.window_start = 0;
    item.set21f_int0 = a;
    item.set21f_int1 = b;
    item.set21f_axis[0] = static_cast<float>(ax);  // 轴与角**另存一份**（不是从矩阵反解）
    item.set21f_axis[1] = static_cast<float>(ay);
    item.set21f_axis[2] = static_cast<float>(az);
    item.set21f_degrees = static_cast<float>(degrees);  // 度，原样存
    item.set21f_rot = rotate_matrix_deg(ax, ay, az, degrees);
    scene_dirty_ = true;
    scene_animating_ = true;
    return;
}

}  // namespace core
