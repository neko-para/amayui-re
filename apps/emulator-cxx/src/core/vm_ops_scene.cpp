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
    if (texture_slot(texture_id) == nullptr) ensure_scene_texture_slot(texture_id);
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

void Machine::op_229(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 场景自身变换与作用键区间（0x229 / `sub_423FE0`，argc 5、推进 11；规则
    // `CORE-OPS-SCENE-SELF-TRANSFORM-229`）：**先重置**场景自身记录与作用区间，再写新的
    // 「区间起点 = op1、计数 = op2」与「三元平移 = op3 / op4 / op5（浮点）」，最后标脏。
    // ★ 读序（EA 0x424011 / 0x42401B / 0x424030 / 0x42403C / 0x424048）：**2 → 1 → 3 → 4 → 5**。
    //   两个整型走 int 取用、三个浮点走 float 取用 ⇒ 浮点槽收整数操作数按 0.0 处理（不抛）。
    // ★ 重置发生在任何读数之前（EA 0x424008）⇒ 读数抛出时区间已经是 (0,0)、平移已经清零。
    write_length(11);
    scene_self_transform_ = SceneSelfTransform{};
    scene_self_transform_.active = true;
    const auto count = read_int(ops[1]);
    const auto start = read_int(ops[0]);
    const auto x = read_float(ops[2]);
    const auto y = read_float(ops[3]);
    const auto z = read_float(ops[4]);
    scene_self_transform_.range_start = static_cast<std::uint32_t>(start);
    scene_self_transform_.range_count = static_cast<std::uint32_t>(count);
    scene_self_transform_.translate[0] = static_cast<float>(x);
    scene_self_transform_.translate[1] = static_cast<float>(y);
    scene_self_transform_.translate[2] = static_cast<float>(z);
    mark_dirty();
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
    // ★ 门是**槽里有没有对象**（不是"记录存不存在"）：记录在、槽已释放 ⇒ 仍然是空槽。
    if (texture_slot(texture_id) == nullptr) {
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
    if (texture_slot(texture_id) == nullptr) ensure_scene_texture_slot(texture_id);
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
    if (texture_slot(texture_id) == nullptr) ensure_scene_texture_slot(texture_id);
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
    if (texture_slot(texture_id) == nullptr) ensure_scene_texture_slot(texture_id);
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

void Machine::op_32d(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 场景颜色（0x32d / `sub_427040` → `sub_499DF0`，argc 2、推进 5；规则 `CORE-OPS-SCENE-COLOR-32D`）：
    // 第 1 个操作数 = **透明度**（大于 255 先夹到 255），第 2 个 = RGB（`0xRRGGBB`）。
    // 四通道各自 /255 归一化、按 0..1 夹取并四舍五入成字节，拼成 ARGB 后**交给渲染模块**（命令 0x8B）。
    // ★ 引擎侧不保存可观察状态 ⇒ 本模型只记请求；不标脏（渲染模块那边才决定要不要重画）。
    write_length(5);
    auto alpha = read_int(ops[0]);
    const auto rgb = read_int(ops[1]);
    if (alpha > 255) alpha = 255;
    const auto r = static_cast<std::uint32_t>((rgb >> 16) & 0xFF);
    const auto g = static_cast<std::uint32_t>((rgb >> 8) & 0xFF);
    const auto b = static_cast<std::uint32_t>(rgb & 0xFF);
    scene_color_request_.argb = (static_cast<std::uint32_t>(alpha) << 24) | (r << 16) | (g << 8) | b;
    scene_color_request_.count += 1;
    return;
}

void Machine::op_32a(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 销毁一个天气子对象（0x32a / `sub_426F80` → `sub_4A0750`，argc 1、推进 3；
    // 规则 `CORE-OPS-SCENE-EFFECT-DESTROY-32A`）：按槽号取对象，**存在才销毁**（销毁 + 释放 + 槽置空），
    // 槽本来就空 ⇒ 什么都不做。槽号不做范围检查（越界是未定义行为）。
    write_length(3);
    const auto index = read_int(ops[0]);
    if (index < 0 || index >= 3) return;  // 本版只规定 0..2（越界不规定）
    scene_effect_present_[static_cast<std::size_t>(index)] = false;
    return;
}

void Machine::op_238(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 设两格（0x238 / `sub_4248C0`，argc 1、推进 3；规则 `CORE-OPS-ENGINE-CELL-238`）：
    // 第 1 格**清 0**、第 2 格原样 ← 操作数（调用点给的是 `5500` 这种毫秒量）。
    // ★ 两格的读者 / 是否由帧服务倒数未取证（残差）。
    write_length(3);
    const auto value = read_int(ops[0]);
    cell_238_left_ = 0;
    cell_238_value_ = value;
    return;
}

void Machine::op_215(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 按键查一条记录（0x215 / `sub_430340` → `sub_4ADC20`，argc 2、推进 5；规则 `CORE-OPS-SCENE-KEYED-215`）：
    // 第 2 个操作数是**键**，第 1 个是落点。命中且记录标志的**第 0 位**为 1 ⇒ 落点 ← 记录的整数值；
    // 记录不存在、或第 0 位为 0 ⇒ 落点 ← **-1**。不写第 2 个操作数。
    // ★ 该表的写者未取证（残差）⇒ 本模型通常查不到，返回 -1（引擎的同一支）。
    write_length(5);
    const auto key = read_int(ops[1]);
    const auto it = scene_records_.find(static_cast<std::uint32_t>(key));
    const bool hit = it != scene_records_.end() && (it->second.flags & 1) != 0;
    write_int(ops[0], hit ? it->second.tag : -1);
    return;
}


void Machine::op_242(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 按键把**同一个整数**写进两条记录（0x242 / `sub_4251A0` → `sub_4AD9A0`，argc 2、推进 5；
    // 规则 `CORE-OPS-SCENE3D-242`）：第 2 个 = 键、第 1 个 = 值；读序 **2 → 1**。
    // 记录不存在就先建（引擎那两步是「确保存在」），再把值写进两条记录的同一格。
    write_length(5);
    const auto value = read_int(ops[1]);
    const auto key = read_int(ops[0]);
    // 第一处：场景对象记录表里同一编号的**一条记录**（缺则先建 —— 建记录这一步标脏）。
    SceneRecord& object = scene_records_[static_cast<std::uint32_t>(key)];
    (void)object;
    // 第二处：另一张表里同一编号的记录（缺则先建）。
    KeyedPairRecord& record = scene_keyed_pair_[key];
    record.field_a = value;
    record.field_b = value;
    return;
}

void Machine::op_224(const Instruction& instruction) {
    (void)instruction;
    // 清空一个按 key 的容器（0x224 / `sub_41A290` → `sub_4AA180` → `sub_4A9BE0`，argc 0、推进 1；
    // 规则 `CORE-OPS-SCENE-CONTAINER-CLEAR-224`）：遍历所有节点 ⇒ 销毁每个节点的载荷 + 释放节点 ⇒
    // 把哨兵环复位、计数清 0。★ 不抛、不标脏、不发请求；填充者未取证（残差）⇒ 本模型只记条目数。
    write_length(1);
    container_224_items_ = 0;
    return;
}

void Machine::op_256(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 给一段键区间内的场景记录设「平移矩阵 + 一个标志字」（0x256 / `sub_425C30` → `sub_4ACD10`，
    // argc 5、推进 11；规则 `CORE-OPS-SCENE-RANGE-TRANSLATE-256`）。
    // 读序 **5 → 4 → 3 → 2 → 1**（三个浮点先读，再读个数与起点）。
    // 区间 = [起点, 起点 + 个数)；**只处理表里已存在**的记录（不改表的大小），每条写标志字 1 + 平移三分量，
    // 最后把场景标脏。「个数 ≤ 0」⇒ 区间空 ⇒ 一条都不处理。
    write_length(11);
    const double z = read_float(ops[4]);
    const double y = read_float(ops[3]);
    const double x = read_float(ops[2]);
    const auto count = read_int(ops[1]);
    const auto start = read_int(ops[0]);
    if (count > 0) {
        const auto first = start;
        const auto last = start + count;
        for (auto& [key, record] : scene_records_) {
            const auto position = static_cast<std::int32_t>(key);
            if (position < first || position >= last) continue;
            record.flag_256 = 1;
            record.matrix_translate[0] = static_cast<float>(x);
            record.matrix_translate[1] = static_cast<float>(y);
            record.matrix_translate[2] = static_cast<float>(z);
        }
    }
    scene_dirty_ = true;
    return;
}

void Machine::op_214(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 记录的**改键 / 搬运**（0x214 / `sub_423AE0` → `sub_4ABEF0`，argc 2、推进 5；
    // 规则 `CORE-OPS-SCENE3D-214`）：把 key = 第 2 个操作数的记录复制到 key = 第 1 个操作数，再删掉源。
    // 读序 **1 → 2**；源不存在 ⇒ 什么都不做（不建目标）。不标脏（引擎同一支）。
    write_length(5);
    const auto target_key = read_int(ops[0]);
    const auto source_key = read_int(ops[1]);
    const auto it = scene_records_.find(static_cast<std::uint32_t>(source_key));
    if (it == scene_records_.end()) return;
    const SceneRecord copy = it->second;
    scene_records_.erase(it);
    scene_records_[static_cast<std::uint32_t>(target_key)] = copy;
    return;
}
}  // namespace core
