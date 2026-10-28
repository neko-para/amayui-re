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
    const bool rebuild = it == textures_.end() || it->second.rgba.empty() ||
                         it->second.width != static_cast<std::uint32_t>(width) ||
                         it->second.height != static_cast<std::uint32_t>(height);
    if (rebuild) {
        // 与 `0x1f8` 空白建纹理同口径：透明、无来源文件、revision 单调递增。
        TextureRecord& texture = textures_[id];
        texture.file = -1;
        texture.blank = true;
        texture.present = true;
        texture.width = static_cast<std::uint32_t>(width);
        texture.height = static_cast<std::uint32_t>(height);
        texture.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4,
                            0);
        texture.revision++;
        text_layer_state_[static_cast<std::size_t>(slot)].cursor_x = rec.f1c;
        text_layer_state_[static_cast<std::size_t>(slot)].cursor_y = rec.f20;
    }
    text_layer_state_[static_cast<std::size_t>(slot)].texture_ready = true;
    return true;
}

void Machine::create_text_layer_item(int slot, int left, int top, int right, int bottom) {
    if (slot < 0 || slot >= 10) return;
    const ListRecord& rec = list_records_[static_cast<std::size_t>(slot)];
    if (rec.f68 <= 0) return;
    const auto id = text_layer_texture_id(static_cast<std::size_t>(slot));
    const TextureRecord* texture = this->texture(id);
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
}

void Machine::erase_text_layer_draw_items(int slot) {
    if (slot < 0 || slot >= 10) return;
    const ListRecord& rec = list_records_[static_cast<std::size_t>(slot)];
    TextLayerState& state = text_layer_state_[static_cast<std::size_t>(slot)];
    if (rec.f68 > 0) {
        for (std::int32_t i = 0; i < state.draw_index; ++i) {
            draw_items_.erase(static_cast<std::uint32_t>(rec.f68) + static_cast<std::uint32_t>(i));
        }
    }
    state.draw_index = 0;
}

void Machine::clear_text_layer(int slot) {
    if (!ensure_text_layer_texture(slot)) return;
    const auto id = text_layer_texture_id(static_cast<std::size_t>(slot));
    TextureRecord* texture = texture_mut(id);
    if (texture == nullptr) return;
    std::fill(texture->rgba.begin(), texture->rgba.end(), 0);
    texture->revision++;
    texture->non_file = true;
    const ListRecord& rec = list_records_[static_cast<std::size_t>(slot)];
    erase_text_layer_draw_items(slot);
    text_layer_state_[static_cast<std::size_t>(slot)].cursor_x = rec.f1c;
    text_layer_state_[static_cast<std::size_t>(slot)].cursor_y = rec.f20;
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
    if (!text.empty()) {
        // 逐字符绘制 + 逐字符绘制条目（键 = 键基数 + 本层条目序号）：与引擎的 per-cell 提交同形，
        // 也让“文本层与窗口 key 交错”成为可表达的行为（整张画布一次贴出是它的退化情形）。
        // ★ 自动折行门 = 配置 `set:AutoLineFeed`（引擎读进 `Font+0x57C`，见 0x417B6E / 0x46C132）；
        //   超右界时的禁则 / 整行重排不在本实现（残差）。
        for (const std::string& unit : split_sjis_units(text)) {
            int advance = 0;
            // 量度：画在画布外（blit 会越界丢弃，但 pen_x 仍然推进），避免先画后折行。
            if (!draw_text_into_texture(*this, id, -10000, state.cursor_y, unit, &error, &advance)) {
                if (!error.empty()) report_error(error);
                return;
            }
            if (auto_line_feed && advance > 0 && state.cursor_x > rec.f1c &&
                state.cursor_x + advance > rec.f24) {
                state.cursor_x = rec.f1c;
                state.cursor_y += line_height;
            }
            const int x0 = state.cursor_x;
            const int y0 = state.cursor_y;
            if (!draw_text_into_texture(*this, id, x0, y0, unit, &error, &advance)) {
                if (!error.empty()) report_error(error);
                return;
            }
            create_text_layer_item(slot, x0, y0, x0 + advance, y0 + line_height);
            state.cursor_x += advance;
        }
    }
    if (!rubi.empty()) {
        // 振り仮名用 **B 支字号**（`0x197` 写的那一格；示例里是 10），画在主文本上方。
        const int ruby_size = font_b_height_ > 0 ? font_b_height_ : 10;
        const int ruby_y = start_y - ruby_size - 2;
        int ruby_advance = 0;
        if (!draw_text_into_texture(*this, id, start_x, ruby_y, rubi, &error, &ruby_advance,
                                    ruby_size)) {
            if (!error.empty()) report_error(error);
        } else {
            create_text_layer_item(slot, start_x, ruby_y, start_x + ruby_advance,
                                   ruby_y + ruby_size + 2);
        }
    }
    if (TextureRecord* texture = texture_mut(id)) texture->revision++;
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
    // 释放字体槽（0x301 / `sub_4269F0`）：读 op1 一次、用两次 —— 先 `C[idx] = 0`，再释放该槽。
    // ★ 槽表本身（Font+1044+4*idx 的窗口记录）**本版未建模**：释放的"容器删除 + 载荷析构"
    //   没有可观测的替代物 ⇒ 这里只清 C，并在残差里记着（⛔ 不要伪造一次释放）。
    const auto index = read_int(ops[0]);
    font_slot_start_[index] = 0;
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
    // 未命中 ⇒ op1 写 0、op2 写 0（不是 −1）。
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
                value = entry.value;
                break;
            }
            if (i + 1 < font_entries_.size() && (font_entries_[i + 1].flags & 1u) != 0) break;
        }
    }
    write_int(ops[0], found);
    write_int(ops[1], value);
    return;
}

}  // namespace core
