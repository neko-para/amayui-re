// 字体与文本：字体对象两格 / 主副色 / 字号 / 对位 / 绘制模式 / 名称查表；
// set-string · 脚本字符串键（save / load）· 配置版本 · 十进制文本→整数 · 文本请求标记。
//
// ★ 本文件只有 handler 本体；分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"
#include "core/vm_internal.h"
#include "core/text_raster.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace core {
namespace {
// `EMULATOR_CXX_TRACE_TEXT=1` ⇒ 把字号 / 行高 / 文本槽偏移的**每次写入**打到 stderr。
// ★ 诊断用（问"界面里这段文字为什么这么小"）：这三个量在引擎里是**不同的格**，混起来看不出是谁写的。
bool text_trace_enabled() {
    static const bool on = std::getenv("EMULATOR_CXX_TRACE_TEXT") != nullptr;
    return on;
}

/** CP932 字节串按一个显示字符切开：前导字节 0x81..0x9F / 0xE0..0xFC 带一个后继字节。 */
std::vector<std::string> split_sjis_units(const std::string& text) {
    std::vector<std::string> units;
    for (std::size_t i = 0; i < text.size();) {
        const auto lead = static_cast<unsigned char>(text[i]);
        const bool two = ((lead >= 0x81u && lead <= 0x9Fu) || (lead >= 0xE0u && lead <= 0xFCu)) &&
                         i + 1 < text.size();
        units.emplace_back(text.substr(i, two ? 2 : 1));
        i += two ? 2 : 1;
    }
    return units;
}
}  // namespace

void Machine::op_2ec(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    write_int(ops[0], parse_int_text(read_text(ops[1]))); return;  // 十进制文本转整数
}

void Machine::op_2eb(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    write_text(ops[0], config_.get_string("set:GameVersion")); return;  // 配置版本写进字符串记录
}

void Machine::op_1a4(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字体对象两格：读序 2 → 1
    font_pair_b_ = read_int(ops[1]);
    font_pair_a_ = read_int(ops[0]);
    return;
}

void Machine::op_76(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字体主色/副色：低 24 位字节倒序
    const auto value = as_u32(read_int(ops[0])) & 0xFFFFFFu;
    const auto swapped = ((value & 0xFFu) << 16) | (value & 0xFF00u) | ((value >> 16) & 0xFFu);
    if (instruction.opcode == 0x76) font_primary_color_ = as_i32(swapped);
    else font_secondary_color_ = as_i32(swapped);
    if (text_trace_enabled()) {
        // 诊断：题目是"为什么我们画出来是纯白而真机偏暗" ⇒ 先看**脚本到底给了什么颜色**、经哪条指令。
        std::fprintf(stderr, "[text] 0x%X color op=%d raw=0x%06X stored=0x%06X\n",
                     static_cast<unsigned>(instruction.opcode), read_int(ops[0]),
                     static_cast<unsigned>(value), static_cast<unsigned>(swapped));
    }
    return;
}

void Machine::op_2bd(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 两格 = 700 / 0
    const auto value = read_int(ops[0]) != 0 ? 700 : 0;
    font_metric_a_ = value;
    font_metric_b_ = value;
    return;
}

void Machine::op_2de(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字体名查表：宿主字体表未接，空表 ⇒ 一律未命中 -1
    (void)read_text(ops[1]);
    write_int(ops[0], -1);
    return;
}

void Machine::op_192(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // set-string：源按文本读，写入全局/局部字符串记录
    write_text(ops[0], read_text(ops[1]));
    return;
}

void Machine::op_194(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字符串相等（0x194 / `sub_42CF10`，argc 3、推进 7；规则 `CORE-OPS-TEXT-EQUAL-194`）：
    // 读序 **op3 → op2**（读序是合同），两者都按"格式化成字符串"的读法（整数标签 ⇒ 十进制文本，
    // 文本标签 ⇒ 原文本）。把"两串**逐字节相等**"归一成 1 / 0 写进 op1。
    // ★ 无早退、不记日志、不发请求；两条读都在写之前（任一条类型非法 ⇒ 抛且不写）。
    // ★ 推进格数槽在**读数之前**就写成 7（EA `0x42CF4F`，早于两次读数）—— 抛出时格数已生效。
    write_length(7);
    const std::string right = read_text(ops[2]);
    const std::string left = read_text(ops[1]);
    write_int(ops[0], left == right ? 1 : 0);
    return;
}

void Machine::op_195(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字符串不等（0x195 / `sub_42D010`，argc 3、推进 7；规则 `CORE-OPS-TEXT-UNEQUAL-195`）：
    // 与 `0x194` 同一条比较的两面 —— 读序也是 **op3 → op2**，两者都按"格式化成字符串"读，
    // 把"两串**逐字节不等**"归一成 1 / 0 写进 op1（⇒ 同一对操作数上两条互为 0/1 互换）。
    // ★ 推进格数槽同样在读数之前写 7（EA `0x42D04F`）。引擎里这条只被 0x195 用（表项 `0xA56F0` 单点注册）。
    write_length(7);
    const std::string right = read_text(ops[2]);
    const std::string left = read_text(ops[1]);
    write_int(ops[0], left != right ? 1 : 0);
    return;
}

void Machine::op_1a9(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // save-string：把源文本写进脚本字符串键
    if (ops[0].type != 5) throw CoreError("config-key-unsupported-tag");
    config_.save_script_string(ops[0].raw, read_text(ops[0]));
    return;
}

void Machine::op_1aa(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // load-string：读脚本字符串键并写回落点
    if (ops[0].type != 5) throw CoreError("config-key-unsupported-tag");
    write_text(ops[0], config_.load_script_string(ops[0].raw));
    return;
}

void Machine::op_1a3(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // load-int：键 = 全局整型池下标；结果无条件写回操作数
    const auto key = script_int_key_index(ops[0]);
    const auto value = config_.load_script_int(key);
    write_int(ops[0], value);
    return;
}

void Machine::op_1a2(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // save-int：把操作数的整数值写进脚本键
    const auto key = script_int_key_index(ops[0]);
    config_.save_script_int(key, read_int(ops[0]));
    return;
}

void Machine::op_1b1(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 设一个引擎整数档（0x1b1 / `sub_41FEA0`，argc 1、推进 3；规则 `CORE-OPS-ENGINE-CELL-1B1`）：
    // 只有一个操作数，原样写进**同一个固定格**（不归一、不夹取、不做范围检查）。ADV 脚本开头用它设 1。
    // ★ 该格的读者未取证 ⇒ 本模型只保存值（残差）。
    write_length(3);
    cell_1b1_ = read_int(ops[0]);
    return;
}

void Machine::op_2db(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字体对位模式 ← 操作数；重建入口未建模
    font_align_mode_ = read_int(ops[0]);
    return;
}

void Machine::op_78(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字体绘制模式 ← 操作数；重建入口未建模
    font_draw_mode_ = read_int(ops[0]);
    return;
}

// ── 文本层画布（`0x70` / `0x71` / `0x79` / `0x213` 的宿主侧） ──
// ★ 引擎把每个文本层做成一张离屏画布（纹理编号 = 20 + 层号），文本按下标逐字栅格进去；
//   场景消费时**每个字符一个绘制条目**（键 = 键基数 + 字符序号，源 = 该字符格）。
//   本实现也按逐字符条目落库；创建时机是追加时（引擎是推进器消费时，见 audit 的 TIMING 残差）。


void Machine::reveal_text_layer_to(std::size_t slot, std::size_t revealed) {
    if (slot >= text_layer_state_.size()) return;
    TextLayerState& layer = text_layer_state_[slot];
    layer.reveal_cursor = std::min(revealed, layer.cells.size());
    layer.reveal_due_ms = 0;
    layer.reveal_done_ms = 0;
}
int Machine::resolve_text_layer(int slot) const {
    if (slot == 0) slot = default_group_;
    return slot;
}

bool Machine::ensure_text_layer_texture(int slot) {
    if (slot < 0 || slot >= 10) return false;
    const ListRecord& rec = list_records_[static_cast<std::size_t>(slot)];
    const auto width = rec.f14;
    const auto height = rec.f18;
    if (width <= 0 || height <= 0) return false;
    const auto id = text_layer_texture_id(static_cast<std::size_t>(slot));
    auto it = textures_.find(id);
    const bool rebuild = it == textures_.end() || !it->second.slot.present ||
                         it->second.slot.rgba.empty() ||
                         it->second.slot.width != static_cast<std::uint32_t>(width) ||
                         it->second.slot.height != static_cast<std::uint32_t>(height);
    if (rebuild) {
        // 与 `0x1f8` 空白建纹理同口径：透明、无来源文件、revision **跨删除/重建单调递增**
        // （GPU 纹理缓存按 (id, revision) 命中；回到 1 会让画布显示上一条记录的内容）。
        auto& entry = texture_entry(id);
        entry.record.file = -1;
        entry.record.blank = true;
        entry.slot.present = true;
        entry.slot.width = static_cast<std::uint32_t>(width);
        entry.slot.height = static_cast<std::uint32_t>(height);
        entry.slot.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4,
                               0);
        entry.slot.revision = next_texture_revision(id);
        text_layer_state_[static_cast<std::size_t>(slot)].cursor_x = rec.f1c;
        text_layer_state_[static_cast<std::size_t>(slot)].cursor_y = rec.f20;
        scene_dirty_ = true;  // 画布被建 / 重建
    }
    text_layer_state_[static_cast<std::size_t>(slot)].texture_ready = true;
    return true;
}

void Machine::create_text_layer_item(int slot, int left, int top, int right, int bottom) {
    if (slot < 0 || slot >= 10) return;
    const ListRecord& rec = list_records_[static_cast<std::size_t>(slot)];
    if (!rec.key_base_set) return;  // 脚本没给键基数 ⇒ 不建条目（宿主兜底在 `0x70` 里）
    const auto id = text_layer_texture_id(static_cast<std::size_t>(slot));
    const TextureSlotObject* texture = texture_slot(id);
    if (texture == nullptr) return;
    left = std::max(0, left);
    top = std::max(0, top);
    right = std::min(static_cast<int>(texture->width), right);
    bottom = std::min(static_cast<int>(texture->height), bottom);
    if (right <= left || bottom <= top) return;
    TextLayerState& state = text_layer_state_[static_cast<std::size_t>(slot)];
    const auto key = static_cast<std::uint32_t>(rec.f68) + static_cast<std::uint32_t>(state.draw_index++);
    DrawItem& item = draw_items_[key];
    item.flags = 1u;
    item.texture_id = id;
    item.left = left;
    item.top = top;
    item.right = right;
    item.bottom = bottom;
    item.aux[0] = static_cast<float>(rec.f0c + left);
    item.aux[1] = static_cast<float>(rec.f10 + top);
    item.aux[2] = 0.0f;
    item.color = 0xFFFFFFFFu;
    item.target_color = 0xFFFFFFFFu;
    scene_dirty_ = true;  // 新绘制条目 ⇒ 下一次渲染步必须重画
}

void Machine::erase_text_layer_draw_items(int slot) {
    if (slot < 0 || slot >= 10) return;
    const ListRecord& rec = list_records_[static_cast<std::size_t>(slot)];
    TextLayerState& state = text_layer_state_[static_cast<std::size_t>(slot)];
    if (rec.key_base_set) {
        for (std::int32_t i = 0; i < state.draw_index; ++i) {
            draw_items_.erase(static_cast<std::uint32_t>(rec.f68) + static_cast<std::uint32_t>(i));
        }
    }
    state.draw_index = 0;
    scene_dirty_ = true;  // 条目被移除 ⇒ 下一次渲染步必须重画
}

void Machine::clear_text_layer(int slot) {
    if (!ensure_text_layer_texture(slot)) return;
    const auto id = text_layer_texture_id(static_cast<std::size_t>(slot));
    TextureEntry* entry = texture_mut(id);
    if (entry == nullptr || !entry->slot.present) return;
    std::fill(entry->slot.rgba.begin(), entry->slot.rgba.end(), 0);
    entry->slot.revision++;
    entry->record.non_file = true;
    const ListRecord& rec = list_records_[static_cast<std::size_t>(slot)];
    erase_text_layer_draw_items(slot);
    // ★ 清画布同时丢掉这一层的**字符格与揭示游标**：引擎那边 `0x71` 清的是布局向量（源），
    //   重放闸也据此从头开始（⛔ 不清会让重放把上一段的字补画进来）。
    TextLayerState& layer = text_layer_state_[static_cast<std::size_t>(slot)];
    layer.cells.clear();
    layer.reveal_cursor = 0;
    layer.reveal_due_ms = 0;
    layer.reveal_done_ms = 0;
    layer.gate_cycles = 0;
    // ★ 清画布同时丢掉这一层的**待画格**：引擎那边 `0x71` 清的是布局向量（源），我们的队列就是它的对应物
    //   —— 不清会让"清空后紧接着的下一帧"把上一段的字补画进来（可见的串字）。
    erase_pending_glyphs(slot);
    text_layer_state_[static_cast<std::size_t>(slot)].cursor_x = rec.f1c;
    text_layer_state_[static_cast<std::size_t>(slot)].cursor_y = rec.f20;
}

void Machine::append_text_span_record(int slot, const std::string& text, int x, int y, int x1,
                                      int y1, std::int32_t flags) {
    // 文本门 < 0（`0x1bb` 写 0x80000000）⇒ 排版层整批不追加记录（`0x46BE30` 的 `a5 >= 0` 分支）。
    if (font_text_gate_5ec7c_ < 0) return;
    TextSpanRecord record;
    record.slot = slot;
    record.x = x;
    record.y = y;
    record.x1 = x1;
    record.y1 = y1;
    record.flags = flags;
    record.text = text;
    text_spans_.push_back(std::move(record));
}

void Machine::draw_text_layer_text(int slot, const std::string& text, const std::string& rubi) {
    if (slot < 0 || slot >= 10) return;
    if (text.empty() && rubi.empty()) return;
    if (!ensure_text_layer_texture(slot)) return;
    const auto id = text_layer_texture_id(static_cast<std::size_t>(slot));
    TextLayerState& state = text_layer_state_[static_cast<std::size_t>(slot)];
    std::string error;
    const int start_x = state.cursor_x;
    const int start_y = state.cursor_y;
    const int line_height = (glyph_height_ > 0 ? glyph_height_ : 24) + 6;
    const ListRecord& rec = list_records_[static_cast<std::size_t>(slot)];
    const bool auto_line_feed = config_.get_int("set:AutoLineFeed") == 1;
    int line_start_x = start_x;  // 主文本最后一行的左端（振り仮名那一格的左端用它）
    int line_start_y = start_y;
    if (!text.empty()) {
        // 逐字符**布局**（量度 + 折行 + 逐字符绘制条目）+ 把真正的落字**入队**：
        // 引擎在追加时一次性写好布局格与文本记录，绘制侧每帧只推进一格（`sub_409400` → `0x4096a9`）。
        // ★ 自动折行门 = 配置 `set:AutoLineFeed`（引擎读进 `Font+0x57C`，见 `0x417B6E` / `0x46C132`）；
        //   行头禁则是"查一张本作恒空的表" ⇒ 折行只看右界与门（见 spec 的 `ADV-TEXT-APPEND`）。
        // ★ 文本记录（72 字节向量）：排版层在**每段结束时**追加一条（正常收尾 `0x46CAB7`；折行时先把
        //   当前段的前缀提交，`0x46C225`），并在**每次行结束**再追加一条空串记录（`sub_46AF90`，
        //   折行 `0x46C3B1` / `0x6f`）。`a3` = 文本门 `Engine+0x5EC7C`：它 < 0（抑制）时整批不追加
        //   （`0x46BE30` 的 `if (a5 >= 0)`）；正常是 0，行结束那条再或上 8。
        const std::vector<std::string> units = split_sjis_units(text);
        std::string line_text;
        for (std::size_t ui = 0; ui < units.size(); ++ui) {
            const std::string& unit = units[ui];
            int advance = 0;
            // 量度：画在画布外（blit 会越界丢弃，但 pen_x 仍然推进），避免先画后折行。
            if (!draw_text_into_texture(*this, id, -10000, state.cursor_y, unit, &error, &advance)) {
                if (!error.empty()) report_error(error);
                // 没有字体后端也要留下"这一段"的记录：排版账与栅格化是两件事。
                for (std::size_t j = ui; j < units.size(); ++j) line_text += units[j];
                break;
            }
            if (auto_line_feed && advance > 0 && state.cursor_x > rec.f1c &&
                state.cursor_x + advance > rec.f24) {
                append_text_span_record(slot, line_text, line_start_x, line_start_y, state.cursor_x,
                                        state.cursor_y + line_height, font_text_gate_5ec7c_);
                append_text_span_record(slot, std::string(), 0, 0, 0, 0,
                                        font_text_gate_5ec7c_ | 8);
                line_text.clear();
                state.cursor_x = rec.f1c;
                state.cursor_y += line_height;
                line_start_x = state.cursor_x;
                line_start_y = state.cursor_y;
            }
            const int x0 = state.cursor_x;
            const int y0 = state.cursor_y;
            create_text_layer_item(slot, x0, y0, x0 + advance, y0 + line_height);
            PendingGlyph pending;
            pending.slot = slot;
            pending.x = x0;
            pending.y = y0;
            // ★ 字号 / 模式 / 两色都是**追加当时**的快照（引擎在这里就逐字写出）：
            //   画的时候再读字体对象 ⇒ 别的行在这期间改 `0x75` 会让这一格按别人的字号画出来。
            pending.size = glyph_height_;
            pending.weight = font_metric_a();
            pending.draw_mode = font_draw_mode();
            pending.primary = font_primary_color();
            pending.secondary = font_secondary_color();
            pending.unit = unit;
            pending.x1 = x0 + advance;
            pending.y1 = y0 + line_height;
            // ★ 每层保留一份**字符格**：`0x300` 的循环演示闸要按它逐格重放（绘制条目被清掉后重建）。
            state.cells.push_back(pending);
            // ★ 闸开着的层由闸门泵推进（普通打字队列不再管它，否则同一条文本会有两个推进者）。
            if ((font_slot_state_[slot] & 1) == 0) pending_glyphs_.push_back(std::move(pending));
            state.cursor_x += advance;
            line_text += unit;
        }
        if (!line_text.empty()) {
            append_text_span_record(slot, line_text, line_start_x, line_start_y, state.cursor_x,
                                    state.cursor_y + line_height, font_text_gate_5ec7c_);
        }
    }
    if (!rubi.empty()) {
        // 振り仮名用 **B 支字号**（`0x197` 写的那一格；示例里是 10），画在主文本上方。
        //
        // ★ 横向算式（`sub_465a20`，调用点 `0x46c379`）：振り仮名摆进**它自己那一格** ——
        //   格 = 本条主文本**最后一行**占的范围（调用方把该格的 x / x1 写进两个局部，`a5` 就是两者之差
        //   = 格宽，`0x46c363`）；`d = 格宽 − 整串量度宽 W`、`n = 假名数 + 1`（= `⌊(字节数+3)÷2⌋`）：
        //     d > 0  ⇒ 每档步进 = ⌊d ÷ n⌋；首字 x = 格左 + 步进，每个假名再加一次步进
        //              （2 字节整份、1 字节半份，`0x465ef1`）⇒ 两端各留一份、恰好铺满格宽；
        //     d ≤ 0  ⇒ 不加步进，整串以格**中心**对齐：首字 x = 格左 − ⌊|d|÷2⌋（`0x465b25`）。
        //   ⛔ 不是"按行比例切分"（按比例只决定这一行分到几个假名，`0x46c2ed`）。
        // 振り仮名 run 的字号支 = **记录标志位 2**（B 支）—— 与"画记录"同一个口径（`record_font_size`）。
        const int ruby_size = record_font_size(4);
        const int ruby_y = start_y - ruby_size - 2;
        const int cell_x = line_start_x;
        const int cell_width = state.cursor_x - cell_x;  // 最后一行主文本的宽
        int whole_advance = 0;
        if (!draw_text_into_texture(*this, id, -10000, ruby_y, rubi, &error, &whole_advance,
                                    ruby_size)) {
            if (!error.empty()) report_error(error);
        } else {
            const int d = cell_width - whole_advance;
            const int gaps = static_cast<int>((rubi.size() + 3) / 2);  // 假名数 + 1
            const int step = d > 0 && gaps > 0 ? d / gaps : 0;
            const int shift = d <= 0 ? (-d) / 2 : 0;
            int x = cell_x + step - shift;
            for (const std::string& kana : split_sjis_units(rubi)) {
                int advance = 0;
                if (!draw_text_into_texture(*this, id, -10000, ruby_y, kana, &error, &advance,
                                            ruby_size)) {
                    if (!error.empty()) report_error(error);
                    continue;
                }
                create_text_layer_item(slot, x, ruby_y, x + advance, ruby_y + ruby_size + 2);
                PendingGlyph pending;
                pending.slot = slot;
                pending.x = x;
                pending.y = ruby_y;
                pending.size = ruby_size;
                pending.weight = font_metric_a();
                pending.draw_mode = font_draw_mode();
                pending.primary = font_primary_color();
                pending.secondary = font_secondary_color();
                pending.unit = kana;
                pending.x1 = x + advance;
                pending.y1 = ruby_y + ruby_size + 2;
                state.cells.push_back(pending);
                if ((font_slot_state_[slot] & 1) == 0) pending_glyphs_.push_back(std::move(pending));
                x += advance + (kana.size() == 1 ? step / 2 : step);
            }
            // 振り仮名 run 也**追加一条文本记录**（`sub_465a20` 的收尾）：**位 2 置起**（用 B 支字号），
            // 位置 = 这一格（主文本最后一行的范围）× 振り仮名那一行。主线 run 的记录不含位 2。
            append_text_span_record(slot, rubi, cell_x, ruby_y, cell_x + cell_width,
                                    ruby_y + ruby_size + 2, font_text_gate_5ec7c_ | 4);
        }
    }
    if (TextureEntry* entry = texture_mut(id)) entry->slot.revision++;
    scene_dirty_ = true;  // 文本层画布（含振り仮名）变了
}

void Machine::erase_pending_glyphs(int slot) {
    pending_glyphs_.erase(
        std::remove_if(pending_glyphs_.begin(), pending_glyphs_.end(),
                       [slot](const PendingGlyph& glyph) { return glyph.slot == slot; }),
        pending_glyphs_.end());
}

void Machine::draw_front_pending_glyph() {
    if (pending_glyphs_.empty()) return;
    const PendingGlyph glyph = pending_glyphs_.front();
    pending_glyphs_.erase(pending_glyphs_.begin());
    const auto slot = static_cast<std::size_t>(glyph.slot);
    if (slot >= 10) return;
    if (!text_layer_state_[slot].texture_ready) return;  // 画布被拆掉 / 没建 ⇒ 这个字丢掉
    std::string error;
    int advance = 0;
    // 用**追加当时**的样式快照画（⛔ 不是画的时候再读字体对象）—— 见 `PendingGlyph` 的注释。
    const TextStyleSnapshot style{glyph.size, glyph.weight, glyph.draw_mode, glyph.primary, glyph.secondary};
    if (!draw_text_into_texture(*this, text_layer_texture_id(slot), glyph.x, glyph.y, glyph.unit,
                                &error, &advance, glyph.size, &style)) {
        if (!error.empty()) report_error(error);
    }
    if (TextureEntry* entry = texture_mut(text_layer_texture_id(slot))) entry->slot.revision++;
    scene_dirty_ = true;  // 画布像素变了 ⇒ 下一次渲染步必须重画
}

void Machine::drain_pending_glyphs() {
    // 引擎 `0x4096e0`–`0x4096f4` / `sub_46CBF0` 的 do-while：每轮仍走"推进一格"那条路，循环到队列空。
    while (!pending_glyphs_.empty()) draw_front_pending_glyph();
}

void Machine::begin_text_typing_or_drain() {
    // 引擎 `0x41ec35` / `0x41fcf8` 的两档：**每字间隔 > 0 且状态字第 27 位未置** ⇒ 置第 29 位 +
    // 启动文本计时器（间隔 = `Font+0x560` = `param_cell_`、起点 = 现在）；否则 ⇒ 立即追加 + 同帧抽干
    // （`sub_46cbf0`）。
    // ★ 第 27 位的置位（按键 / 前置支）**不在本实现**：那个门位没有服务者，置了会让主循环卡住
    //   （见 `REQ-01M60CFPK2X36ZCQD21M3E5G7E` 与 `SCOPE.md`）。抽干本身与字节一致。
    const std::int32_t speed = param_cell_.value_or(0);
    if (speed > 0 && (state_flags_ & (1u << 27)) == 0) {
        state_flags_ |= (1u << 29);
        text_timer_start_ms_ = scene_clock_ms_;
        text_timer_interval_ms_ = speed;
        text_timer_step_ = 1;
        return;
    }
    drain_pending_glyphs();
}

// ── `0x300` 的循环演示闸（规则 `CORE-OPS-TEXT-SLOT-TIMER`） ──
// 引擎：文本泵 `sub_409400` 里"槽 1..9 任一状态非 0"⇒ 有槽在用（bit16 粘住）；泵每 `MessageSpeed`
// 毫秒调 `sub_45BE20` 揭示该层的下一格（层记录 `+0x84` 是揭示游标）；整段揭示完返回 1 ⇒ 记起点，
// `现在 − 起点 ≥ 期限` 时调 `sub_404F80` 清掉该层两段绘制键区间并把游标归 0，**闸位保持** ⇒ 下一拍
// 从第一格重来。`0x301` 是同一段清除的一次性入口。
void Machine::reveal_text_layer_cell(int slot) {
    TextLayerState& state = text_layer_state_[static_cast<std::size_t>(slot)];
    if (state.reveal_cursor >= state.cells.size()) return;
    const PendingGlyph glyph = state.cells[state.reveal_cursor];
    std::string error;
    int advance = 0;
    // 与普通打字机同一个口径：用**追加当时**的样式快照画。
    const TextStyleSnapshot style{glyph.size, glyph.weight, glyph.draw_mode, glyph.primary,
                                  glyph.secondary};
    if (!draw_text_into_texture(*this, text_layer_texture_id(static_cast<std::size_t>(slot)), glyph.x,
                                glyph.y, glyph.unit, &error, &advance, glyph.size, &style)) {
        if (!error.empty()) report_error(error);
    }
    // 绘制条目在"揭示"这一步重建（`sub_45BE20` 把这一格交给绘制侧）；源矩形 = 追加时排好的格。
    create_text_layer_item(slot, glyph.x, glyph.y, glyph.x1, glyph.y1);
    if (TextureEntry* entry = texture_mut(text_layer_texture_id(static_cast<std::size_t>(slot)))) {
        entry->slot.revision++;
    }
    ++state.reveal_cursor;
}

void Machine::reveal_all_text_layer_cells(int slot) {
    TextLayerState& state = text_layer_state_[static_cast<std::size_t>(slot)];
    while (state.reveal_cursor < state.cells.size()) reveal_text_layer_cell(slot);
}

void Machine::service_text_gate(int slot) {
    TextLayerState& state = text_layer_state_[static_cast<std::size_t>(slot)];
    if (state.cells.empty()) return;
    const std::int32_t speed = param_cell_.value_or(0);
    const std::uint32_t now = scene_clock_ms_;
    if (speed <= 0) {
        // 参数格 == 0：引擎走"抽干到末格"支并直接跳到槽循环尾（`0x409452`）⇒ 整段一次揭示，
        // **不记起点、不清条目、不重放**。
        reveal_all_text_layer_cells(slot);
        return;
    }
    if (state.reveal_cursor < state.cells.size()) {
        if (state.reveal_due_ms == 0) state.reveal_due_ms = now;
        if (now < state.reveal_due_ms) return;
        reveal_text_layer_cell(slot);
        state.reveal_due_ms = now + static_cast<std::uint32_t>(speed);
        if (state.reveal_cursor >= state.cells.size()) state.reveal_done_ms = now;
        return;
    }
    // 全部揭示完：停「期限」毫秒，然后清本层绘制条目 + 游标归 0（闸位保持）⇒ 下一拍从第一格重来。
    if (state.reveal_done_ms == 0) state.reveal_done_ms = now;
    const std::int32_t deadline = font_slot_deadline_[slot];
    const std::uint32_t hold = deadline > 0 ? static_cast<std::uint32_t>(deadline) : 0;
    if (now - state.reveal_done_ms < hold) return;
    erase_text_layer_draw_items(slot);
    state.reveal_cursor = 0;
    state.reveal_due_ms = 0;
    state.reveal_done_ms = 0;
    ++state.gate_cycles;
}

void Machine::service_text_layers() {
    // ── 先服务 `0x300` 的循环演示闸：只要有闸在，泵就继续跑（**不清第 29 位**） ──
    bool gate_active = false;
    bool advancing = false;
    for (int slot = 1; slot < 10; ++slot) {
        const auto it = font_slot_state_.find(slot);
        if (it == font_slot_state_.end()) continue;
        if ((it->second & 1) != 0) {      // 位 0 = 闸开着
            gate_active = true;
            it->second |= 0x10000;        // 位 16 粘住：这一刻有槽在用（`0x300` 写状态时保留它）
            // 引擎的"在推进"计数（`var_4`）只在**进了计时支**的槽上加一（位 0 置起且参数格 ≠ 0）。
            if (param_cell_.value_or(0) != 0) advancing = true;
            service_text_gate(slot);
        } else if ((it->second & 0x10000) != 0) {
            // 闸已关（`0x300` 写了位 0 = 0）⇒ 抽干剩余格，然后清该槽的记录（"抽干后清"）。
            // ★ 没有字符格的槽不在这里清：那是"只写过 0x300、从没当过演示闸"的槽，清洗它会把
            //   `0x300` 的写语义（位 16 粘住）一起抹掉（既有守卫就守这一格）。
            TextLayerState& state = text_layer_state_[static_cast<std::size_t>(slot)];
            if (!state.cells.empty()) {
                reveal_all_text_layer_cells(slot);
                it->second = 0;
            }
        }
    }
    if (gate_active) {
        // 引擎：没有槽进计时支（参数格 = 0 ⇒ 全部抽干）⇒ 清第 29 位（`0x4094EF`）；否则保持。
        if (advancing) state_flags_ |= (1u << 29);
        else state_flags_ &= ~(1u << 29);
        return;
    }
    // 打字中（第 29 位）才推进；**每 D 毫秒一格、每帧最多一格**（`sub_453b60` 的闸门：
    // `elapsed <= n × interval` ⇒ 本帧一格都不画，超出才画一格并把 n 推到 ⌊elapsed/interval⌋+1）。
    if ((state_flags_ & (1u << 29)) == 0) {
        // 不在打字态：队列本应已被"立即抽干"清空；万一有残留（宿主换了时钟 / 加载期）一次画完，
        // ⛔ 不要退回"每帧一格"（那会让"间隔 = 0"的脚本又变成打字机）。
        if (!pending_glyphs_.empty()) drain_pending_glyphs();
        return;
    }
    if (pending_glyphs_.empty()) {
        state_flags_ &= ~(1u << 29);  // 没有待画格 ⇒ 没有可服务的东西（引擎的"无活动槽"支）
        return;
    }
    if (input_pressed_) {
        // 按键 / 跳过 ⇒ **同帧抽干** + 清第 29 位（引擎 `0x40953c`–`0x409554`；它同时置第 27 位，
        // 本实现不置 —— 理由同 `begin_text_typing_or_drain`）。
        drain_pending_glyphs();
        state_flags_ &= ~(1u << 29);
        return;
    }
    if (text_timer_interval_ms_ <= 0) {  // 计时器没被正确启动（防御；正常路径间隔 > 0）
        draw_front_pending_glyph();
        if (pending_glyphs_.empty()) state_flags_ &= ~(1u << 29);
        return;
    }
    const std::uint32_t elapsed = scene_clock_ms_ - text_timer_start_ms_;
    const std::int64_t due = static_cast<std::int64_t>(text_timer_step_) * text_timer_interval_ms_;
    if (static_cast<std::int64_t>(elapsed) <= due) return;  // 未到期 ⇒ 本帧一格都不画
    const auto crossed = static_cast<std::int32_t>(
        elapsed / static_cast<std::uint32_t>(text_timer_interval_ms_));
    text_timer_step_ = crossed + 1;
    draw_front_pending_glyph();
    if (pending_glyphs_.empty()) state_flags_ &= ~(1u << 29);  // 末格 ⇒ 清第 29 位（`0x4096b2`）
}
void Machine::end_text_layer_line(int slot) {
    if (slot < 0 || slot >= 10) return;
    const ListRecord& rec = list_records_[static_cast<std::size_t>(slot)];
    TextLayerState& state = text_layer_state_[static_cast<std::size_t>(slot)];
    state.cursor_x = rec.f1c;
    // 行距 = 行間隙 + 字号（普通模式；`Font+0x564` 缺省 6、`Font+0x313D4` 是 A 支字号）。
    // 证据：`sub_46AF90` 的 0x46B001/06/0C/18/20/2E；`0x418617` 每次设字号把 `Font+0x4D0` 重写成 −字号。
    const int line_gap = 6;
    const int line_advance = (glyph_height_ > 0 ? glyph_height_ : 24) + line_gap;
    state.cursor_y += line_advance;
    // `sub_46AF90` 在 `a3 >= 0` 时还会追加一条**空串记录**（标志 |= 8）；`0x6f` 与折行共用这个入口。
    append_text_span_record(slot, std::string(), 0, 0, 0, 0, font_text_gate_5ec7c_ | 8);
}

void Machine::op_71(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 文本请求（`0x71` / `sub_41ED80` → `sub_45EC60`，argc 1）：
    // ① 当前帧「本条位置」← 本条指令的 dword 下标；② 清空该层的画布与游标；
    // ③ 把画布按层矩形排成一条绘制项（键基数 = `0x213` 写的 +104 格）。
    // ★ 引擎还会做两处整块拷贝 / 一次子系统内调用 / 有条件置状态字第 27 位 ⇒ **未建模**（残差）。
    const auto requested = read_int(ops[0]);
    frame_mut().position = static_cast<std::uint32_t>(frame_mut().ip / 4);
    const int slot = resolve_text_layer(requested);
    clear_text_layer(slot);
    return;
}

void Machine::op_1a5(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    font_string_ = read_text(ops[0]); return;  // 两个字符串入口，内部未建模
}

void Machine::op_197(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // `0x197` 设字号（`sub_41FDD0`，**argc 1**）：op1 = **字形高度（未缩放的逻辑像素）**，**没有**字体对象下标。
    // ★ 引擎的写点（`sub_418680`）：全局字号格 ← v；**十个槽全写** `槽+0xC8 = v`（展开、无下标、无越界检查）；
    //   LOGFONTA 的 `lfHeight = -v`、`lfWidth = -(v/2)`（向零截断）；随后重建三种 HFONT。
    // ★ 它**不碰**行高那一组（`Font+0x313D4`，那是 `0x75` 的活）—— 两件事一个格是本模型以前的错。
    // ★ 缺省 8（引擎 `0x46551C` 的初值），不是行高的 24。
    // ★ 最终像素 = round(|v| × 显示缩放 `Font+0x355E4`)，乘在**建字体**那一步（`0x45A7FE fmul [ebx+355E4h]`；
    //   0x204 走的是另一支：`0x45A3A9 fmul [ebx+355E4h]`）。该比例 = 视口 ÷ `set:WinX/WinY`，初值 1.0，
    //   只在「窗口态 + 虚拟全屏类型 2」时刷新（`0x46584E..0x46588A`）⇒ **设计分辨率下恒 1.0**；
    //   本宿主按逻辑分辨率渲染 ⇒ 1.0，**不需要**再乘。
    // ★ 这条写的是 **B 支**（`Font+0x355D8`）：只有跨度绘制路径在特定标志下用它；0x204 不用它。
    const auto value = read_int(ops[0]);
    font_b_height_ = value;
    for (auto& slot : text_slots_) slot.glyph_height = value;
    if (text_trace_enabled()) std::fprintf(stderr, "[text] 0x197 font_b_height=%d\n", value);
    return;
}

void Machine::op_75(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // `0x75` 设的是**A 支字号**（`sub_4185F0`）：`Font+0x313D4 ← v`（`0x418611`），
    // 两份 LOGFONTA 的 lfHeight ← −v（`0x418617` / `0x418619`），随后重建字体（`sub_459F40`）。
    // ★ **0x204 画文字用的就是这一支**（`0x459F63` 读它当逐字字体的高度）⇒ 它不是"行高"。
    glyph_height_ = read_int(ops[0]);
    if (text_trace_enabled()) std::fprintf(stderr, "[text] 0x75 glyph_height(family A)=%d\n", glyph_height_);
    return;
}

void Machine::op_198(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // `0x198` 设**文本槽的绘制原点偏移**（`sub_41FE10`，**argc 3**）：
    //   op1 = 槽下标（`0` ⇒ 默认槽 **1**；合法 0..9），op2 → `槽+0x0C`，op3 → `槽+0x10`。
    // ★ 它**不碰**字号、不重建字体：`sub_456400` 里仅 `0x456421` / `0x456424` 两处写内存。
    // ★ 槽指针为 0 时引擎不检查（直写 [0+…]），越界/负数是**未定义行为** ⇒ 这里夹紧后跳过（已知偏离）。
    int slot = read_int(ops[0]);
    if (slot == 0) slot = 1;
    if (slot < 0 || slot >= static_cast<int>(text_slots_.size())) return;
    text_slots_[static_cast<std::size_t>(slot)].x_offset = read_int(ops[1]);
    text_slots_[static_cast<std::size_t>(slot)].y_offset = read_int(ops[2]);
    if (text_trace_enabled()) {
        std::fprintf(stderr, "[text] 0x198 slot=%d x=%d y=%d\n", slot,
                     text_slots_[static_cast<std::size_t>(slot)].x_offset,
                     text_slots_[static_cast<std::size_t>(slot)].y_offset);
    }
    return;
}

void Machine::op_6e(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 追加正文（0x6E `show-text` / `sub_41EB20`）：读序 **op2（文本）→ op1（整型）**。
    // ★ 空串 ⇒ 引擎的追加入口立即返回：**不写文本槽、不追加、不登记计时**（这三件一起跳过）。
    // ★ `op1 == 0` ⇒ 引擎的**默认槽**（`Font+1228`，由 `0x80` 写）—— 不是第 0 槽。
    // ★ 正文进**文本层画布**（纹理 = 20 + 层号）的当前游标；状态位（0xAAB44 位 27/29）与计时队列
    //   （0x691EC，时长取 0x15290）依赖未取证的 `0x77A04` / `0x15290` ⇒ 残差。
    const std::string text = read_text(ops[1]);
    const auto requested = read_int(ops[0]);
    if (text.empty()) return;
    const int slot = resolve_text_layer(requested);
    if (slot < 0 || slot >= 10) return;
    current_text_slot_ = requested;
    text_windows_[slot].text += text;
    draw_text_layer_text(slot, text, std::string());
    // ★ 成功追加时置消息臂位的第 16 位（引擎 `0x41EB7D`：`or [esi+77A04h], 10000h`；只在
    //   `0x304` 已把臂位第 0 位置起的支路里做）。`0x305` 的「武装」= 位 0 与位 16 同时成立。
    //   ⛔ 不置它 ⇒ `0x305` 的整段块结束逻辑永远是死代码。
    if ((msg_arm_ & 1) != 0) msg_arm_ |= 0x10000;
    begin_text_typing_or_drain();
    return;
}

void Machine::op_6f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 行结束（0x6F `end-text-line` / `sub_41ECE0`）：只读 op1；无外部请求。
    // ★ op1 == 0 ⇒ 默认槽；行结束把游标 x 复位、y 前进一行（行距 = 字体对象的行高）。
    const auto requested = read_int(ops[0]);
    const int slot = resolve_text_layer(requested);
    if (slot < 0 || slot >= 10) return;
    text_windows_[slot].lines += 1;
    end_text_layer_line(slot);
    return;
}

void Machine::op_196(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 带振り仮名的追加（0x196 `display-furigana` / `sub_41FC20`）：读序
    // **op3（文本，无条件最先）→ op2（文本）→ op1（整型）**。
    // ★ op3 先被拷进 1024 字节缓冲（上限 **1023 字节**，超长静默截断）；空正文 ⇒ 整段跳过写。
    // ★ 主文本用 A 支字号，振り仮名用 B 支字号（示例里 `0x197(10)`）画在主文本上方。
    const std::string rubi = read_text(ops[2]);
    const std::string text = read_text(ops[1]);
    const auto requested = read_int(ops[0]);
    if (text.empty()) return;
    const int slot = resolve_text_layer(requested);
    if (slot < 0 || slot >= 10) return;
    current_text_slot_ = requested;
    TextWindowRecord& window = text_windows_[slot];
    window.text += text;
    window.rubi = rubi.size() > 1023 ? rubi.substr(0, 1023) : rubi;
    draw_text_layer_text(slot, text, window.rubi);
    // ★ 与 `0x6e` 同一条：成功追加时置消息臂位第 16 位（引擎 `0x41FCD8`）。
    if ((msg_arm_ & 1) != 0) msg_arm_ |= 0x10000;
    begin_text_typing_or_drain();
    return;
}

// ── 文本长度 / 字体槽表 / Font 条目登记表（CONFIG.BIN 路径） ──
//
// ★ 取证锚（EA）与不确定项见 `docs/02-engine/ops-forensics-2026-10-title-option.md` §3–§4。
// ★ 这一族都作用在 Font 对象（`Engine+0x14D30`）上；域名词多半未取证 ⇒ 结构名保持中性。

void Machine::op_1a6(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 文本长度减半（0x1A6 / `sub_42D110`）：读序 = op2（文本）→ 写 op1。
    // ★ 按**字节**取 `strlen >> 1`（不是按字）；空串 ⇒ 0；落点走整型写原语（给字符串池 ⇒ 类型异常）。
    const std::string text = read_text(ops[1]);
    write_int(ops[0], static_cast<std::int32_t>(text.size() / 2));
    return;
}

void Machine::op_300(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 装字体槽（0x300 / `sub_426990`）：读序 #1（下标）→ #2 → #3；`A[idx]` 在 #3 之前就写。
    // `A[idx] = op2 | (A[idx] & 0x10000)`：**bit16 粘住**，op2 的其它位覆盖低位；`B[idx] = op3`。
    // ★ 下标**不做范围检查**（引擎越界写是静默的）⇒ 这里用 map 表达，⛔ 不加检查。
    const auto index = read_int(ops[0]);
    const auto state = read_int(ops[1]);
    std::int32_t& slot_state = font_slot_state_[index];
    slot_state = state | (slot_state & 0x10000);
    const auto deadline = read_int(ops[2]);
    font_slot_deadline_[index] = deadline;
    return;
}

void Machine::op_301(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 释放字体槽（0x301 / `sub_4269F0`）：读 op1 一次、用两次 —— 先 `C[idx] = 0`，再清该层：
    //   `sub_404F80` 把揭示游标（层记录 `+0x84`）归 0，并删掉该层的两段绘制键区间。
    // ★ 它**不删字符格、不清画布像素**（所以关闸后再开闸能重新逐格揭示同一段文本）。
    const auto index = read_int(ops[0]);
    font_slot_start_[index] = 0;
    if (index >= 0 && index < 10) {
        erase_text_layer_draw_items(index);
        TextLayerState& state = text_layer_state_[static_cast<std::size_t>(index)];
        state.reveal_cursor = 0;
        state.reveal_due_ms = 0;
        state.reveal_done_ms = 0;
    }
    return;
}

void Machine::op_1d2(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 登记一条 Font 条目（0x1D2 / `sub_420380`）：
    //   ① 守卫 `Engine+0x5EC7C != 0` ⇒ **整条跳过**（推进格已由 step 写，且不读操作数、不抛）；
    //   ② 读序 op2 → op1；③ 入队：`+0x14 = op2`、`+0x18 = op1`、`+0x28 = 0x20000000 | (脏槽 != 0)`，
    //      脏槽非 0 时**同时清它**；④ **不写任何操作数**、无外部请求。
    if (font_text_gate_5ec7c_ != 0) return;
    const auto value = read_int(ops[1]);
    const auto key = read_int(ops[0]);
    FontEntry entry;
    entry.list = 0;  // 引擎取 `Engine+0x4CC` 的默认列表号；本版没有它的写者 ⇒ 固定 0（残差）
    entry.value = value;
    entry.key = key;
    entry.flags = 0x20000000u;
    const auto index = font_entries_.size();
    if (index < font_entry_dirty_.size() && font_entry_dirty_[index] != 0) {
        entry.flags |= 1u;
        font_entry_dirty_[index] = 0;
    }
    font_entries_.push_back(entry);
    return;
}

void Machine::op_1d3(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 按 key 查 Font 条目（0x1D3 / `sub_42D4A0`）：读序 op5（key）→ op4（起始下标）→ op3（读而不用）。
    // ★ op3 的值被丢弃，但**类型非法照样抛** ⇒ 必须真的读一次（不是跳过）。
    // 命中判据：`flags & 0x20000000` 且 `key` 相等；组边界 = **下一条**记录 flags 低位（bit0）置起。
    // ★ 命中**不中断扫描**（引擎 `0x457A07` 的循环继续）：同组里后命中的记录覆盖前面的 ⇒ 取**最后一条**；
    //   组边界在命中测试**之后**判（`0x457A02`），断组才停。未命中 ⇒ op1 写 0、op2 写 0（不是 −1）。
    const auto key = read_int(ops[4]);
    const auto start = read_int(ops[3]);
    (void)read_int(ops[2]);
    std::int32_t found = 0;
    std::int32_t value = 0;
    if (start >= 0 && static_cast<std::size_t>(start) < font_entries_.size()) {
        for (std::size_t i = static_cast<std::size_t>(start); i < font_entries_.size(); ++i) {
            const FontEntry& entry = font_entries_[i];
            if ((entry.flags & 0x20000000u) != 0 && entry.key == key) {
                found = 1;
                value = entry.value;  // ⛔ 不要在这里 break：同组后命中的覆盖前面的
            }
            if (i + 1 < font_entries_.size() && (font_entries_[i + 1].flags & 1u) != 0) break;
        }
    }
    write_int(ops[0], found);
    write_int(ops[1], value);
    return;
}


void Machine::op_7a(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 写文本 run 头的两个整数（0x7a / `sub_41F4E0` → `sub_45A910`，argc 3、推进 7；
    // 规则 `CORE-OPS-TEXT-RUN-HEADER-7A`）：第 1 个 = 文本槽（0 ⇒ 落默认槽），第 2 / 3 个原样写进
    // 该槽 run 头的两格（引擎里是"写指针之前 20 / 16 字节"那两格）。读序 **3 → 2 → 1**。
    // ★ `op1 == 0` 与其他文本指令同义：换成**默认槽**（`sub_45A910` 的 `[ecx+4CCh]`），
    //   ⛔ 不是"落到第 0 槽"。
    // ★ 消费者（把这些整数变成绘制请求那一段）未建模 ⇒ 本模型只保存值（残差）。
    write_length(7);
    const auto third = read_int(ops[2]);
    const auto second = read_int(ops[1]);
    const auto slot = read_int(ops[0]);
    const int resolved = resolve_text_layer(slot);
    if (resolved < 0 || resolved >= static_cast<int>(run_header_.size())) {
        throw CoreError("text-layer-slot-out-of-range:" + std::to_string(slot));
    }
    run_header_[static_cast<std::size_t>(resolved)] = RunHeader{second, third};
    return;
}

void Machine::op_193(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字符串拼接（0x193 / `sub_433710`，argc 3、推进 7；规则 `CORE-OPS-TEXT-CONCAT-193`）：
    // 落点 = 第 1 个操作数；结果 = **第 2 个 ++ 第 3 个**；读序 **3 → 2**（先右值再左值）。
    // 与 `0x192` 用同一个落点写原语（它支持 5 / 8 / 11 / 14）。
    write_length(7);
    const std::string right = read_text(ops[2]);
    const std::string left = read_text(ops[1]);
    write_text(ops[0], left + right);
    return;
}

void Machine::op_20a(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 立即重绘一个文本层（0x20a / `sub_423620`，argc 1、推进 3；规则 `CORE-OPS-TEXT-REDRAW-20A`）：
    // 第 1 个操作数 = 层号（0 ⇒ 默认层）。把该层**已揭示的格**贴到画布上；随后若门字第 30 位置起，
    // 还会**再读一次**层号并补画一个延迟格。
    // ★ 本模型的画布绘制在"追加 / 揭示"时就做了 ⇒ 贴已揭示格不再逐格重放；但仍要标脏让下一次
    //   渲染步重画这一层。
    write_length(3);
    const auto layer = read_int(ops[0]);
    const int slot = resolve_text_layer(layer);
    if (slot >= 0 && slot < 10) scene_dirty_ = true;
    // ★ 门字第 30 位置起 ⇒ **再读一次**层号并补画一个延迟格（该格随即被清）。
    if ((state_flags_ & (1u << 30)) != 0) {
        (void)read_int(ops[0]);
        if (!pending_glyphs_.empty()) {
            draw_front_pending_glyph();
            scene_dirty_ = true;
        }
    }
    return;
}

void Machine::op_243(const Instruction& instruction) {
    (void)instruction;
    // 消息等待三格（0x243 / `sub_41B180`，argc 0、推进 1；规则 `CORE-OPS-MSG-WAIT-243`）：
    // 写序 **(1, 0, 0)**；整段被一道位住：那道位已置 ⇒ **一格都不写**。无调用、不置门位。
    write_length(1);
    if ((msg_wait_gate_ & 2) != 0) return;
    msg_wait_a_ = 1;
    msg_wait_b_ = 0;
    msg_wait_c_ = 0;
    return;
}

void Machine::op_2f7(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 语音槽选项格 ← 1（0x2f7 / `sub_426890`，argc 1、推进 3；规则 `CORE-OPS-VOICE-OPTION-2F7`）：
    // 第 1 个 = 槽号；把该槽的选项格**整格写成 1**（⛔ 不是 or ⇒ 高位被清）。槽号不做范围检查（本版只规定 0..2）。
    write_length(3);
    const auto slot = read_int(ops[0]);
    if (slot < 0 || slot >= 3) return;
    voice_option_2f7_[static_cast<std::size_t>(slot)] = 1;
    return;
}

void Machine::op_304(const Instruction& instruction) {
    (void)instruction;
    // 消息块开始（0x304 / `sub_41A420`，argc 0、推进 1；规则 `CORE-OPS-MSG-BLOCK-304-305`）：
    // ① 把消息臂位**整格写 1**（顺手清掉上一块留下的第 16 位）；② 把当前层**已揭示的游标**存进"块开头"格。
    write_length(1);
    msg_arm_ = 1;
    const int slot = resolve_text_layer(current_text_slot_);
    msg_saved_cursor_ =
        (slot < 0 || slot >= 10) ? 0
                                 : static_cast<std::int32_t>(text_layer_revealed(static_cast<std::size_t>(slot)));
    return;
}

void Machine::op_305(const Instruction& instruction) {
    (void)instruction;
    // 消息块结束（0x305 / `sub_41B1C0`，argc 0、推进 1；规则 `CORE-OPS-MSG-BLOCK-304-305`）：
    // 门 = 消息臂位的第 0 位 **且** 第 16 位（第 16 位由 `0x6e` / `0x196` 成功追加时置）。
    // 未武装 ⇒ 只清臂位、什么都不做。武装 ⇒ ① 揭示游标回卷到块开头；② 读 `message:ReadTextSkip`，
    // 非 0 且"已读"命中时置门字第 27 位；③ 揭示该层**全部**格（引擎是在这里自旋到"揭示完"）；
    // ④ 写消息等待三格（最终 (1,0,0)；引擎写序 0 / 0 / 1）；⑤ 三条出口都清臂位。
    // ★ 本模型没有"已读表"（残差 `RES-MSG-BLOCK-READ-HIT`）⇒ ② 的两个条件只实现第一个；
    //   ⛔ 不许因此把 bit27 置起：模型里它没有服务者，会让主循环 gate-stalled。
    write_length(1);
    const bool armed = (msg_arm_ & 1) != 0 && (msg_arm_ & 0x10000) != 0;
    if (!armed) {
        msg_arm_ = 0;
        return;
    }
    const int slot = resolve_text_layer(current_text_slot_);
    if (slot >= 0 && slot < 10) {
        const auto saved = msg_saved_cursor_ < 0 ? std::size_t{0}
                                                 : static_cast<std::size_t>(msg_saved_cursor_);
        reveal_text_layer_to(static_cast<std::size_t>(slot), saved);
        reveal_all_text_layer_cells(slot);
    }
    msg_wait_a_ = 1;
    msg_wait_b_ = 0;
    msg_wait_c_ = 0;
    msg_arm_ = 0;
    return;
}
}  // namespace core
