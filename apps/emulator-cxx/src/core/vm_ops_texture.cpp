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
    for (auto& [slot, entry] : textures_) {
        if (slot < 42u || slot >= 1000u || surface_release_gate_[slot] != 0) continue;
        // ★ 记录与槽对象分开：这里只把**来源文件**写成「无」并清**槽里的对象**；颜色键 / 空白标记等
        //   记录字段保留（与 `0x1fa` 同口径）。
        entry.record.file = -1;
        entry.slot.present = false;
        entry.slot.width = 0;
        entry.slot.height = 0;
        entry.slot.rgba.clear();
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
    // ★ 排窗那一支在引擎里写**同一条指令对**：脏位 `+0B5AC` = 1 与"待处理"闩 `+0B5B4` = 1
    //   （`0x4AD14B` / `0x4AD155` 是同形的两条 `mov dword ptr […], 1`；本函数这一族共 24 处）。
    //   少了闩 ⇒ `0x21c` 在窗还没走完时就放行（TITLE 的 mesh 200000 在**任何一帧渲染前**被 `0x1f7` 拆掉）。
    scene_dirty_ = true;
    scene_pending_ = true;
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
    // ★ 引擎里 0x1fb 是**场景对象记录**的建记录者：置记录首字第 0 位、并把第 2 个操作数写进
    //   记录的整数标签（0x215 读它；0x220 的门也看这个位）。本模型把同一动作同时记在记录上。
    SceneRecord& record = scene_records_[key];
    record.flags |= 1;
    record.tag = static_cast<std::int32_t>(item.texture_id);
    item.left = left;
    item.top = top;
    item.right = left + width;
    item.bottom = top + height;
    item.aux[0] = static_cast<float>(read_int(ops[6]));
    item.aux[1] = static_cast<float>(read_int(ops[7]));
    item.aux[2] = 0.0f;
    // ★ 引擎的 `+0x24..+0x2C`（第二三元向量）在 0x1fb 里也写这三个数；`0x21a` 读的就是它。
    //   模型里 0x219 写 vec2，这里必须同步，否则 `0x1fb → 0x21a` 读回 0。
    record.vec2[0] = item.aux[0];
    record.vec2[1] = item.aux[1];
    record.vec2[2] = item.aux[2];
    return;
}

void Machine::op_258(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 1000 项表的**两项标志**（0x258 / `sub_425D20`，argc 2、推进 5；规则 `CORE-OPS-SCENE-TABLE-FLAGS`）：
    // 按编号定位两张表（活动表 + 快照）的同一项，把第 1 格 ← 掩码第 0 位、第 2 格 ← 掩码第 1 位；
    // **两张表同值**（⛔ 不是"一张存位 0、另一张存位 1"），掩码高位一律不看。
    // ★ 本模型只建活动表（快照是存档侧；见残差）⇒ 写一张表。
    // ★ 读序 **2 → 1**（`sub_425D20`：`0x425D3D push 2` 先读位掩码、`0x425D4B push 1` 后读编号）。
    //   编号**不做**范围检查（引擎的索引没有比较，越界写落到快照表）——本版只规定 0..999，
    //   越界按本仓口径**响亮失败**（与 `0x216` 同一条策略）。
    write_length(5);
    const auto mask = read_int(ops[1]);
    const auto index = read_int(ops[0]);
    if (index < 0 || index >= 1000) throw CoreError("scene-table-index-out-of-range:" + std::to_string(index));
    TextureRecord& record = texture_entry(static_cast<std::uint32_t>(index)).record;
    record.flag0 = (mask & 1) != 0 ? 1 : 0;
    record.flag1 = (mask & 2) != 0 ? 1 : 0;
    return;
}

void Machine::op_259(const Instruction& instruction) {
    (void)instruction;
    // 1000 项表**全清两项标志**（0x259 / `sub_41A3A0`，argc 0、推进 1；规则 `CORE-OPS-SCENE-TABLE-FLAGS`）：
    // 把两张表全部 1000 项的这两格清 0，**其它字段一格不碰**；不调任何函数、不置脏。
    write_length(1);
    for (auto& [id, entry] : textures_) {
        if (id >= 1000u) continue;  // 表只有 0..999 这 1000 项
        entry.record.flag0 = 0;
        entry.record.flag1 = 0;
    }
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
    // 源矩形窗与颜色窗共用同一条"排窗"写点（脏位 + 待处理闩），`0x239` 自己那一支同样置两格。
    scene_dirty_ = true;
    scene_pending_ = true;
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
        // 引擎：`0x4AD14B` 脏位 = 1、`0x4AD155` "待处理"闩（`+0B5B4`）= 1，两条紧挨着 ⇒ 一起写。
        scene_dirty_ = true;
        scene_pending_ = true;
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
    // ★ 记录与槽对象**分开写**：`0x1f8` 只动「来源 = 无 / 空白标记」与槽里的像素，
    //   **颜色键等记录字段保持原值**（spec「空白创建纹理」；`0x1fa` 之后记录还在，所以这里能保住）。
    TextureEntry& entry = texture_entry(id);
    entry.record.file = -1;
    entry.record.blank = true;
    entry.slot.present = true;
    entry.slot.width = static_cast<std::uint32_t>(width);
    entry.slot.height = static_cast<std::uint32_t>(height);
    entry.slot.revision = next_texture_revision(id);   // ★ 跨释放 / 重建也单调（见 next_texture_revision）
    entry.slot.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
    // ★ `revision` 必须**严格单调递增**（这里只加不减）。窗口路径的 GPU 纹理缓存按 `revision` 命中：
    //   一次重画 = 0x1f8 重建 + N 次 0x204/0x205；若重建把版本号重置回 1，则"每次重画都写同样多条文字"
    //   的终值恒等 ⇒ 缓存命中上一轮的纹理 ⇒ 滚动 / 切页时画面停在上一次的文字。2026-10-27 实测：
    //   CONFIG1.BIN 的列表每次滚动重画**恰好 14 条** 0x204（9 条标签 + 5 条值），版本号终值恒为 15
    //   ⇒ 只有条数变化时才刷新（用户口径「必须文字数量变多/变少才会刷新」）。
    //   ⛔ 不要为了"让空白纹理重新上传"而重置版本号。
    return;
}

void Machine::op_1fa(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 释放纹理（CORE-OPS-TEXTURE-RELEASE）：**只清槽里的对象**，纹理记录保留 ——
    // ★ 记录 vs 槽对象是两层：「空槽」≠「空白纹理」。旧实现删整条记录，把颜色键一起丢了，
    //   还让同一编号重建时版本号打回 1（GPU 缓存把上一条记录的纹理当成当前纹理）。
    const auto id = static_cast<std::uint32_t>(read_int(ops[0]));
    (void)read_int(ops[0]);
    // 释放守卫非 0：只清共享对象槽，纹理记录 / 纹理槽都不动（CORE-OPS-TEXTURE-RELEASE）。
    if (id < surface_release_gate_.size() && surface_release_gate_[id] != 0) return;
    TextureEntry* entry = texture_mut(id);
    if (entry == nullptr) return;  // 没有记录 ⇒ 也没有槽对象可清
    // ★ `next_texture_revision` 自己会把这一条用过的最高版本算进下限，所以下面清槽不会让
    //   以后重建的版本号回退（否则 GPU 缓存会命中"上一条记录"的纹理，实测两个界面互相污染）。
    entry->record.file = -1;        // 记录的来源文件写成「无」；颜色键 / 空白标记不动
    entry->slot.present = false;    // 槽里的对象释放（平台纹理）
    entry->slot.width = 0;
    entry->slot.height = 0;
    entry->slot.rgba.clear();
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
    // ★ 这是「换纹理」：**记录**整条替换、槽对象换成新像素（旧的颜色键 / 空白标记都不留）。
    TextureEntry entry;
    entry.record.file = file;
    entry.record.color_key =
        raw_key < 0 ? 0u : (0xFF000000u | (static_cast<std::uint32_t>(raw_key) & 0xFFFFFFu));
    entry.slot.width = image.width;
    entry.slot.height = image.height;
    entry.slot.rgba = image.rgba;
    if (entry.record.color_key != 0) {
        for (std::size_t i = 0; i + 3 < entry.slot.rgba.size(); i += 4) {
            const auto argb = (static_cast<std::uint32_t>(entry.slot.rgba[i + 3]) << 24) |
                              (static_cast<std::uint32_t>(entry.slot.rgba[i]) << 16) |
                              (static_cast<std::uint32_t>(entry.slot.rgba[i + 1]) << 8) |
                              entry.slot.rgba[i + 2];
            if (argb == entry.record.color_key) {
                entry.slot.rgba[i] = 0; entry.slot.rgba[i + 1] = 0;
                entry.slot.rgba[i + 2] = 0; entry.slot.rgba[i + 3] = 0;
            }
        }
    }
    entry.slot.present = true;
    // ★ 替换同一槽时版本号必须**前进**，且要接着"这个编号用过的最高版本"往前（GPU 缓存按
    //   revision 命中）：旧槽对象在就取旧值 + 1；槽已被 `0x1fa` 释放时同样接着下限涨
    //   （⛔ 不能从 1 重新开始 —— 那会命中上一条记录留在缓存里的纹理）。
    entry.slot.revision = next_texture_revision(static_cast<std::uint32_t>(id));
    textures_[static_cast<std::uint32_t>(id)] = std::move(entry);
    return;
}

std::uint32_t Machine::next_texture_revision(std::uint32_t id) {
    auto& floor = texture_revision_floor_[id];
    // ★ 先把条目**当前**的版本算进下限：文本绘制 / 场景效果走的是 `slot.revision++`，
    //   它们不经过这里 —— 若只信下限，可能发一个**已被用过**的版本号，GPU 缓存就命中旧内容。
    const auto it = textures_.find(id);
    if (it != textures_.end() && it->second.slot.revision > floor) floor = it->second.slot.revision;
    return ++floor;
}

void Machine::ensure_scene_texture_slot(std::uint32_t id) {
    // 场景命令族（`0x223` / `0x24f` / `0x250` / `0x251`）的「没有就建一张 1280×720 透明画布」。
    // ★ 判据是**槽对象**是否为空：`0x1fa` 之后记录还在、槽已释放 ⇒ 这里照建（与旧的"删整条再建"等价）。
    TextureEntry& entry = texture_entry(id);
    entry.record.file = -1;  // 画布没有来源文件（与 `0x1f8` 空白创建同口径）
    entry.record.blank = true;
    entry.record.non_file = true;
    entry.slot.present = true;
    entry.slot.width = 1280;
    entry.slot.height = 720;
    entry.slot.revision = next_texture_revision(id);
    entry.slot.rgba.assign(1280u * 720u * 4u, 0);
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
    // ★ 判据是**槽里的对象**：`0x1fa` 之后记录还在（颜色键等字段保原值）但槽已释放 ⇒ 这里仍然是 0/0。
    const TextureSlotObject* found = texture_slot(static_cast<std::uint32_t>(id));
    write_int(ops[1], found != nullptr ? static_cast<std::int32_t>(found->width) : 0);
    write_int(ops[2], found != nullptr ? static_cast<std::int32_t>(found->height) : 0);
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
    // ★ 读的是**记录的来源文件格**（不是槽对象）：`0x1fa` 把来源写成「无」之后这里读 -1。
    const TextureRecord* record = texture_record(static_cast<std::uint32_t>(slot));
    write_int(ops[0], record == nullptr ? -1 : record->file);
    return;
}

void Machine::op_217(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写第一三元向量（CORE-OPS-SCENE3D-SET-VEC1）：读序 2 → 3 → 4 → 1（三个浮点先读，编号最后读）。
    // ★ 没有「对象已存在」门：编号不存在 ⇒ 先建一条默认记录再照写；编号不做范围检查。
    // ⛔ 写的只是**第一**组三个浮点；第二组由 `0x219` 写（不许合并）。
    // 读序 2 → 3 → 4 → 1（`.lst` `sub_423B30`：先 2、再 3、再 4）。
    const double x = read_float(ops[1]);
    const double y = read_float(ops[2]);
    const double z = read_float(ops[3]);
    SceneRecord& record = scene_records_[static_cast<std::uint32_t>(read_int(ops[0]))];
    record.vec1[0] = static_cast<float>(x);
    record.vec1[1] = static_cast<float>(y);
    record.vec1[2] = static_cast<float>(z);
    scene_dirty_ = true;
    return;
}



void Machine::op_220(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 场景对象：写变换（平移 + 两个整数 + 标志）（0x220 / CORE-OPS-SCENE3D-SET-TRANSFORM，argc 6、推进 13）。
    // 读序 **4 → 5 → 6 → 3 → 2 → 1**（三个浮点先读、再三个整数、编号最后）。
    // ★ 门在「建记录」之后：位 0 为 0 ⇒ 只留下默认记录、一个字段都不写（⛔ 不是「什么都不做」）。
    // ★ 位 1 与 `0x202` 的「有窗」是同一位；G0/G3 与窗起点 / 配置格同格（见规范）。
    // 读序 4 → 5 → 6 → 3 → 2 → 1（`.lst` 0x423E01 / 0x423E0D / 0x423E19 / 0x423E3E / 0x423E46 / 0x423E50）。
    const double x = read_float(ops[3]);
    const double y = read_float(ops[4]);
    const double z = read_float(ops[5]);
    const auto g2 = read_int(ops[2]);   // 第 3 个操作数
    const auto g1 = read_int(ops[1]);   // 第 2 个操作数
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    SceneRecord& record = scene_records_[key];
    if ((record.flags & 1) == 0) return;  // 只建记录，不写字段
    record.flags |= 2;                    // 位 1 = 已配置（与 0x202 的「有窗」同一位）
    record.g[0] = 0;
    record.g[1] = g1;
    record.g[2] = g2;
    record.g[3] = 1;
    record.translate4[0] = static_cast<float>(x);
    record.translate4[1] = static_cast<float>(y);
    record.translate4[2] = static_cast<float>(z);
    // ★ 位 1 与 `0x202` 的「有窗」同一位、G0 与窗起点同格、G3 与 `0x21e/21f` 的配置格同格；
    //   这些格的**消费者读 `DrawItem`**（`scene_raster.cpp:437`）⇒ 必须同步到 DrawItem，
    //   ⛔ 只写 `SceneRecord` 会让 0x220 的窗完全不可见。
    DrawItem& item = draw_item_or_create(key);
    item.flags |= 2u;
    item.window_start = 0;
    item.config_g3 = 1;
    item.set21e_int0 = g1;
    item.set21e_int1 = g2;
    scene_dirty_ = true;
    scene_animating_ = true;
    // 同一族 setter 在引擎里也写 `[this+0B5B4h] = 1`（`0x4B00FB` / `0x4B0257` / `0x4B034B` 等）⇒ 待处理闩。
    scene_pending_ = true;
    return;
}
void Machine::op_219(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写第二三元向量（CORE-OPS-SCENE3D-SET-VEC2）：与 0x217 同形（读序 2 → 3 → 4 → 1、查不到先建、编号不检查），
    // 但写的是记录的**第二组**三个浮点格；⛔ 不许与 0x217 合并（两条写不同的格）。
    // 读序 2 → 3 → 4 → 1（`.lst` 0x423BC1 / 0x423BCD / 0x423BD9）。
    const double x = read_float(ops[1]);
    const double y = read_float(ops[2]);
    const double z = read_float(ops[3]);
    const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
    SceneRecord& record = scene_records_[key];
    record.vec2[0] = static_cast<float>(x);
    record.vec2[1] = static_cast<float>(y);
    record.vec2[2] = static_cast<float>(z);
    // ★ 引擎的 `+0x24..+0x2C` 与 0x1fb 写的是同一组；渲染读 `DrawItem.aux` ⇒ 两半必须同步。
    DrawItem& item = draw_item_or_create(key);
    item.aux[0] = static_cast<float>(x);
    item.aux[1] = static_cast<float>(y);
    item.aux[2] = static_cast<float>(z);
    scene_dirty_ = true;
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
    // 同一族 setter 在引擎里也写 `[this+0B5B4h] = 1`（`0x4B00FB` / `0x4B0257` / `0x4B034B` 等）⇒ 待处理闩。
    scene_pending_ = true;
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
    // 同一族 setter 在引擎里也写 `[this+0B5B4h] = 1`（`0x4B00FB` / `0x4B0257` / `0x4B034B` 等）⇒ 待处理闩。
    scene_pending_ = true;
    return;
}


void Machine::op_32(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 纹理缩放拷贝（0x32 / `sub_41E2D0`，argc 10、推进 21；规则 `CORE-OPS-TEXTURE-STRETCH-32`）。
    // 角色：1 = **源**纹理 id、2 = **目标**纹理 id；3..6 = 源矩形 (x, y, 宽, 高)；7..10 = 目标矩形 (x, y, 宽, 高)。
    // 读序：先 3..10（预算矩形），再 1 / 2。源或目标纹理不存在 ⇒ **只记错误、不抛**（引擎同一支）。
    write_length(21);
    const auto src_x = read_int(ops[2]);
    const auto src_y = read_int(ops[3]);
    const auto src_w = read_int(ops[4]);
    const auto src_h = read_int(ops[5]);
    const auto dst_x = read_int(ops[6]);
    const auto dst_y = read_int(ops[7]);
    const auto dst_w = read_int(ops[8]);
    const auto dst_h = read_int(ops[9]);
    const auto src_id = static_cast<std::uint32_t>(read_int(ops[0]));
    const auto dst_id = static_cast<std::uint32_t>(read_int(ops[1]));
    const TextureSlotObject* source = texture_slot(src_id);
    TextureEntry* target = texture_mut(dst_id);
    if (source == nullptr || source->width == 0 || source->height == 0) {
        report_error("texture-stretch-no-source");
        return;
    }
    if (target == nullptr || target->slot.width == 0 || target->slot.height == 0) {
        report_error("texture-stretch-no-target");
        return;
    }
    if (dst_w <= 0 || dst_h <= 0 || src_w <= 0 || src_h <= 0) return;
    // 目标矩形的四条边夹进目标纹理；源矩形夹进源纹理（引擎用两块纹理各自的裁剪字段）。
    const int left = std::max(0, dst_x);
    const int top = std::max(0, dst_y);
    const int right = std::min(static_cast<int>(target->slot.width), dst_x + dst_w);
    const int bottom = std::min(static_cast<int>(target->slot.height), dst_y + dst_h);
    if (right <= left || bottom <= top) return;
    std::vector<std::uint8_t> canvas = target->slot.rgba;
    const int sx0 = std::max(0, src_x);
    const int sy0 = std::max(0, src_y);
    const int sx1 = std::min(static_cast<int>(source->width), src_x + src_w);
    const int sy1 = std::min(static_cast<int>(source->height), src_y + src_h);
    const int span_x = std::max(0, sx1 - sx0);
    const int span_y = std::max(0, sy1 - sy0);
    if (span_x == 0 || span_y == 0) return;
    const int dest_w = right - left;
    const int dest_h = bottom - top;
    // 采样比 = 源跨度 / 目标跨度。引擎 `sub_4A87A0` 按比例把目标夹进目标纹理后再分派：
    // srcW<=dstW 且 srcH<=dstH ⇒ 放大（EnlargeTexture），两个方向都 ≥ ⇒ 缩小（ShrinkTexture），
    // 一边放一边缩 ⇒ 临时介面两趟（本模型按放大支处理，见残差 `RES-TEX-STRETCH-MIXED`）。
    const double scale_x = static_cast<double>(span_x) / static_cast<double>(dest_w);
    const double scale_y = static_cast<double>(span_y) / static_cast<double>(dest_h);
    const bool shrink = scale_x >= 1.0 && scale_y >= 1.0;
    const auto pixel = [&](int x, int y, int channel) -> int {
        x = std::clamp(x, 0, static_cast<int>(source->width) - 1);
        y = std::clamp(y, 0, static_cast<int>(source->height) - 1);
        const std::size_t at = (static_cast<std::size_t>(y) * source->width + static_cast<std::size_t>(x)) * 4u;
        return at + 3u < source->rgba.size() ? static_cast<int>(source->rgba[at + static_cast<std::size_t>(channel)])
                                            : 0;
    };
    for (int oy = 0; oy < dest_h; ++oy) {
        for (int ox = 0; ox < dest_w; ++ox) {
            const std::size_t to = (static_cast<std::size_t>(top + oy) * target->slot.width +
                                    static_cast<std::size_t>(left + ox)) * 4u;
            if (to + 3u >= canvas.size()) continue;
            if (shrink) {
                // 缩小 = **面积加权平均**（引擎 `sub_4A42C0`：区间比例 0x4A43DD / 0x4A4423，
                // 逐源像素取 0x4A44F8，最后除以权重 0x4A4631）。
                const double x0f = static_cast<double>(ox) * scale_x;
                const double x1f = static_cast<double>(ox + 1) * scale_x;
                const double y0f = static_cast<double>(oy) * scale_y;
                const double y1f = static_cast<double>(oy + 1) * scale_y;
                double sum[4] = {0.0, 0.0, 0.0, 0.0};
                double total = 0.0;
                for (int sy = static_cast<int>(std::floor(y0f)); sy < static_cast<int>(std::ceil(y1f)); ++sy) {
                    const double wy = std::min(y1f, static_cast<double>(sy + 1)) -
                                      std::max(y0f, static_cast<double>(sy));
                    if (wy <= 0.0) continue;
                    for (int sx = static_cast<int>(std::floor(x0f)); sx < static_cast<int>(std::ceil(x1f)); ++sx) {
                        const double wx = std::min(x1f, static_cast<double>(sx + 1)) -
                                          std::max(x0f, static_cast<double>(sx));
                        if (wx <= 0.0) continue;
                        const double weight = wx * wy;
                        total += weight;
                        for (int c = 0; c < 4; ++c) sum[c] += weight * pixel(sx0 + sx, sy0 + sy, c);
                    }
                }
                if (total <= 0.0) continue;
                for (int c = 0; c < 4; ++c) {
                    const int value = static_cast<int>(std::trunc(sum[c] / total + 0.5));
                    canvas[to + static_cast<std::size_t>(c)] =
                        static_cast<std::uint8_t>(std::clamp(value, 0, 255));
                }
                continue;
            }
            // 放大 = **双线性**（引擎 `sub_4A3F90`：sx = u·srcW/dstW（`0x4A3FA6` imul + `0x4A3FCB`
            // fdivp），整数部分 `__ftol2_sse`（0x4A3FE3）截断、`0x4A3FFB` fsubp 取小数，
            // 索引夹到 srcW-1（0x4A400C–0x4A4012），四邻域 0x4A4043/0x4A4095/0x4A40E3/0x4A418C，
            // 每通道 `sub_49EE00`：加权 + 0.5（0x49EE33）后截断并夹 0..255（0x49EE39–0x49EE4E））。
            const double sx_f = static_cast<double>(ox) * scale_x;
            const double sy_f = static_cast<double>(oy) * scale_y;
            int ix = static_cast<int>(std::trunc(sx_f));
            int iy = static_cast<int>(std::trunc(sy_f));
            const double fx = sx_f - static_cast<double>(ix);
            const double fy = sy_f - static_cast<double>(iy);
            ix = std::clamp(ix, 0, std::max(0, span_x - 1));
            iy = std::clamp(iy, 0, std::max(0, span_y - 1));
            const int ix1 = std::clamp(ix + 1, 0, std::max(0, span_x - 1));
            const int iy1 = std::clamp(iy + 1, 0, std::max(0, span_y - 1));
            for (int c = 0; c < 4; ++c) {
                const double p00 = pixel(sx0 + ix, sy0 + iy, c);
                const double p10 = pixel(sx0 + ix1, sy0 + iy, c);
                const double p01 = pixel(sx0 + ix, sy0 + iy1, c);
                const double p11 = pixel(sx0 + ix1, sy0 + iy1, c);
                const double value = (1.0 - fy) * ((1.0 - fx) * p00 + fx * p10) +
                                     fy * ((1.0 - fx) * p01 + fx * p11);
                canvas[to + static_cast<std::size_t>(c)] =
                    static_cast<std::uint8_t>(std::clamp(static_cast<int>(std::trunc(value + 0.5)), 0, 255));
            }
        }
    }
    target->slot.rgba = std::move(canvas);
    target->slot.revision = next_texture_revision(dst_id);
    target->record.non_file = true;
    return;
}

void Machine::op_72(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 等玩家推进（0x72 / `sub_41EEF0`，argc 1、推进 3；规则 `CORE-OPS-MSG-WAIT-INPUT-72`）：
    // 第 1 个操作数 = **消息槽**（0 = 默认槽）。本条把"等推进"位置起（门字第 31 位）⇒ 本拍之后
    // **一条指令都不派发**，直到输入（左键 / skip）或自动消息计时器放行。
    // ★ 本条**不靠把推进格数改回 0**；长度字照写 3，放行后从**下一条**继续。
    write_length(3);
    const auto slot = read_int(ops[0]);
    // ★ 槽号越界是未定义行为；本仓口径是**响亮失败**（与 `0x216` / `0x258` 同一条策略），
    //   ⛔ 不许让它落进 `std::array<TextLayerState,10>` 的越界索引（0x304/0x305 会立刻用它取层）。
    const int resolved = resolve_text_layer(slot);
    if (resolved < 0 || resolved >= 10) {
        throw CoreError("text-layer-slot-out-of-range:" + std::to_string(slot));
    }
    current_text_slot_ = slot;
    // ★ 0x72 尾段的同一批写点（`CORE-OPS-TEXT-CELL-REVEAL-1CE`）：把当前文本槽的**格数**取进「格数」格
    //   （引擎以 a3 = −1 调绘制入口，out 参数写格数）、当前格号归 0、节拍器相位重置；
    //   第 30 位（逐格揭示活动中）在未置时置起 —— 引擎是"先测后置"，净效果与无条件 or 相同。
    reveal_count_ = static_cast<std::int32_t>(text_layer_cell_count(static_cast<std::size_t>(resolved)));
    reveal_cursor_ = 0;
    reveal_tick_start_ms_ = scene_clock_ms_;
    reveal_tick_counter_ = 1;
    state_flags_ |= (1u << 31);
    state_flags_ |= (1u << 30);
    // 自动消息：`0x1b7` 写的引擎格非 0 ⇒ 按"时长公式"武装计时器；到期由逐帧服务**放行**这一位
    // （与点击同一对操作：清 bit31 + bit30，不换行、不取下一行）。
    // 时长 = max(100, TimeX + PitchX × (N − 1 − 0x2e9 的格))；本模型没有音频后端 ⇒ 语音恒"不在播"
    // ⇒ 取第 1 组（Time1 / Pitch1）。N = 该槽的行记录数。
    auto_armed_ = false;
    auto_interval_ms_ = 0;
    if (flag_y_ != 0) {
        const std::int32_t n = line_records_[static_cast<std::size_t>(resolved)];
        const std::int64_t time1 = config_.get_int("message:AutoMessageTime1");
        const std::int64_t pitch1 = config_.get_int("message:AutoMessagePitch1");
        const std::int64_t raw = time1 + pitch1 * (static_cast<std::int64_t>(n) - 1 - cell_2e9_);
        auto_interval_ms_ = static_cast<std::uint32_t>(std::max<std::int64_t>(100, raw));
        auto_due_ms_ = scene_clock_ms_ + auto_interval_ms_;
        auto_armed_ = true;
    }
    return;
}

void Machine::service_auto_message() {
    // 自动消息的到期服务：`0x72` 武装后，到 `auto_due_ms_` 就**放行**同一对位（bit31 + bit30）。
    // ⛔ 不换行、不取下一行、不改执行位置 —— 下一行仍由脚本产生（与点击放行完全同一条路）。
    if (!auto_armed_) return;
    if ((state_flags_ & (1u << 31)) == 0) {
        auto_armed_ = false;  // 已被点击 / 跳过放行
        return;
    }
    if (scene_clock_ms_ < auto_due_ms_) return;
    state_flags_ &= ~(1u << 31);
    state_flags_ &= ~(1u << 30);
    auto_armed_ = false;
}

void Machine::service_advance_wait() {
    // 0x72 的放行：输入是**事件锁存**（一次按下只放行一次）。
    // ★ 语料：窗口过程把"按下"或进输入字（`0x4B8DE6`），消费者读一次就清（`0x4772A9/B7`、
    //   `0x41330B`）；**不是**每帧按电平读 —— 后者会让"按住不放"一句接一句推进（实测回归）。
    std::int32_t mask = 0;
    const bool have = mouse_button_source_ && mouse_button_source_(mask);
    const bool down = have && (mask & 1) != 0;
    if (down && !advance_input_prev_) advance_input_latch_ = true;  // 上升沿 ⇒ 锁一次
    advance_input_prev_ = down;
    if ((state_flags_ & (1u << 31)) == 0) return;  // 没在等：只更新边沿状态（按住不算新的按下）
    const bool skip = (state_flags_ & (1u << 27)) != 0;  // 读跳过 ⇒ 直接放行
    if (advance_input_latch_ || skip) {
        advance_input_latch_ = false;  // 消费掉这一次按下
        state_flags_ &= ~(1u << 31);
        state_flags_ &= ~(1u << 30);
        auto_armed_ = false;
    }
}

void Machine::op_10a(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 光标瞬移（0x10a / `sub_421EA0`，argc 2、推进 5；规则 `CORE-OPS-INPUT-SET-CURSOR-10A`）：
    // 两个操作数是**虚拟屏坐标**；引擎把它换算成客户区 / 屏幕坐标后调平台 SetCursorPos。
    // ★ 换算是平台边界（窗口句柄、缩放、ClientToScreen）⇒ 本模型只记请求，不改引擎状态。
    write_length(5);
    cursor_request_x_ = read_int(ops[0]);
    cursor_request_y_ = read_int(ops[1]);
    cursor_requests_ += 1;
    return;
}
}  // namespace core
