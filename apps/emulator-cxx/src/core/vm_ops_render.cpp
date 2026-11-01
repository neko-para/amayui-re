// 渲染入口：0x20C 帧步进 · 0x20D 渲染目标 · 0x204 离屏文字 · 0x33F 场景混合，
// 外加同一批"呈现前写场景"的入口：0x20B 纹理矩形填充 · 0x23B CG 数字条 · 0x2DA CG 参数。
//
// ★ 本文件只有 handler 本体；分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"
#include "core/vm_internal.h"

#include "core/color.h"
#include "core/text_raster.h"

#include <algorithm>
#include <cmath>

namespace core {

void Machine::op_200(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 场景绘制模式 ← 脚本给的覆盖值（`CORE-OPS-RENDER-FRAME-STEP` 写点③；站点 `0x4231e5`）：
    // 用操作数**直接覆盖** —— 不重读配置、不校验范围、不钳上下界（第三态 2 / 3 / 负数都照写）。
    // ★ 前置：该操作数对应的对象**已被创建** —— 位图来自配置 `set:CreateObject`，位号 = 该操作数
    //   （与初始化那一次的位下标**同一口径**；值 ≥ 32 / 负值按平台移位语义掩到低 5 位，见残差）。
    //   该位未置起 ⇒ **整条不生效、一个字节都不写**（可证伪：`0x201` 读回仍是旧值）。
    //   ⛔ 这不是范围检查：位图来自配置，与模式值本身是不是 0 / 1 / 2 无关。
    const auto value = read_int(ops[0]);
    const auto bit = static_cast<std::uint32_t>(value) & 31u;
    if ((static_cast<std::uint32_t>(config_.get_int("set:CreateObject")) & (1u << bit)) == 0) return;
    draw_mode_ = value;
    return;
}

void Machine::op_201(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 场景绘制模式**原值交出**（站点 `0x4302c8`：把该格交给值写出原语）：不归一化 ——
    // 第三态取到 2 就交出 2（⛔ 不是 0 / 1 归一化的布尔）。
    // ★ `0x201` 整条尚未成篇：它的**落点身份**在 `RES-DRAW-MODE-BIT-INDEX` ② 里仍是 unknown
    //   ⇒ 这里按最小可观察形态实现 = 把原值写进第 1 个操作数槽（与同族 getter 一致）；⛔ 不另造语义。
    write_int(ops[0], draw_mode_);
    return;
}

void Machine::op_23b(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // CG 数字条（`CORE-OPS-SCENE-CG-NUMBER-23B`）：建绘制项（像素输出未建模）。
    // 读序是**合同**：第 2 个（判有效性）→ 1 → 3 → 6 → 7 →（清旧项）→ 每个被画出的位：4 → 5。
    const auto id = read_int(ops[1]);
    // 无效记录：编号不在 0..10（负数按越界），或 0..10 的**纹理槽 = 0**（= 没登记过；编号 10 的
    // 「纹理槽」是帧 0 的返回栈深度，刚复位时同样是 0）。
    // ⇒ 向错误通道上报**与 `0x2da` 同一条文本**（`0x424d79`），本条到此为止：
    //   **其余六个操作数一次都不读**（读它们会抛的类型异常因此不出现）、不删旧项、不建任何项。
    const bool record_missing = id >= 0 && id <= 10 && cg_params_[static_cast<std::size_t>(id)][0] == 0;
    if (id < 0 || id > 10 || record_missing) {
        report_error("CGNumberの引数が不正です．");
        return;
    }
    const auto start = read_int(ops[0]);
    const auto value = read_int(ops[2]);
    const auto digits = read_int(ops[5]);
    const auto flags = read_int(ops[6]);
    // 清旧项：与「拆离」（`0x1f7`）**同一组四张**按键有序的表、**固定表序**（偏移次序，⛔ 不是字母序）：
    //   绘制项表 → 网格表 → 辅助表 A（`+0x438`）→ 辅助表 B（`+0x448`）。
    // ★ 每张表只要真删到条目就各标一次脏（最多四次）；一条都没删到的表不标脏 ⇒ 用 erased 汇总一次。
    // ★ 场景命令表**不在这四张之内**：同键的命令记录**不随**这次清理消失。
    bool erased = false;
    const auto erase_one = [&](std::int32_t slot_key) {
        const auto index = static_cast<std::uint32_t>(slot_key);
        if (draw_items_.erase(index) > 0) erased = true;
        if (meshes_.erase(index) > 0) erased = true;
        if (scene_aux_a_.erase(slot_key) > 0) erased = true;
        if (scene_aux_b_.erase(slot_key) > 0) erased = true;
    };
    // 位数 = 0 ⇒ 清理区间为空（一条都不删、不标脏），一个位都不画，第 4 / 5 个操作数一次都不读。
    if (digits > 0) {
        for (std::int32_t i = 0; i < digits; ++i) erase_one(start + i);
    }
    if (erased) scene_dirty_ = true;
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
    TextureEntry& entry = it->second;
    entry.record.non_file = true;
    entry.slot.revision++;
    if (!entry.slot.present || entry.slot.rgba.empty()) return;
    const int left = std::max(0, x);
    const int top = std::max(0, y);
    const int right = std::min(static_cast<int>(entry.slot.width), x + w);
    const int bottom = std::min(static_cast<int>(entry.slot.height), y + h);
    if (right <= left || bottom <= top) return;
    const int a = std::max(0, alpha);
    const int sr = static_cast<int>((color >> 16) & 0xFFu);
    const int sg = static_cast<int>((color >> 8) & 0xFFu);
    const int sb = static_cast<int>(color & 0xFFu);
    for (int py = top; py < bottom; ++py) {
        for (int px = left; px < right; ++px) {
            const std::size_t d = (static_cast<std::size_t>(py) * entry.slot.width + px) * 4;
            entry.slot.rgba[d + 0] =
                clamp_byte((sr * a + entry.slot.rgba[d + 0] * (255 - a)) / 255);
            entry.slot.rgba[d + 1] =
                clamp_byte((sg * a + entry.slot.rgba[d + 1] * (255 - a)) / 255);
            entry.slot.rgba[d + 2] =
                clamp_byte((sb * a + entry.slot.rgba[d + 2] * (255 - a)) / 255);
            entry.slot.rgba[d + 3] = clamp_byte(a + entry.slot.rgba[d + 3] * (255 - a) / 255);
        }
    }
    return;
}

void Machine::op_20c(const Instruction& instruction) {
    (void)instruction;
    // 帧步进：时间戳 + 一次渲染请求 + 清帧标志
    // ★ 时间戳写的是**场景时钟对**（`CORE-TIMING-CLOCK` 的第 2 个门后写者：`0x41A1D0` 搬旧 /
    //   `0x41A1D6` 写新，仅区间忙标记 = 0 时）。它与 `0x1f4` / `0x23c` / 门前渲染步是**同一对**
    //   ⇒ 门前那一步被 (c) 取消时，这一对仍由它推进（时间不停走）。
    if (!driven_elsewhere_) stamp_scene_clock(host_now_ms());
    frame_step_requested_ = true;
    frame_flag_ = false;
    return;
}

void Machine::op_23c(const Instruction& instruction) {
    (void)instruction;
    // 无条件重盖时钟对（`CORE-TIMING-CLOCK` 的第三个门后写者：`0x41A2E7` 搬旧 / `0x41A2ED` 写新）：
    // 「上一步」←「现在」、「现在」← 本次读数；★ 与 `0x20c` 的唯一差别是**没有区间忙门**，
    // 与 `0x1f4` 的差别是不碰忙标记 / 嵌套计数。推进格数 = 1（step 已写）。
    stamp_scene_clock(host_now_ms());
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

// ── `0x205` 的「格式化 + 横向落点」（`CORE-OPS-RENDER-NUMTEXT-205`；正文 `60-ops-render.md:205-218`）──
// ★ 这是**逐字搬出来**的纯函数（原先的 lambda + x 算式），⛔ 不是重写：分支顺序、截断口径、
//   全角映射、先乘后除的算式一个字都没改。这样做只为让"格式化出的串 / 算出的 x"能被
//   **不依赖字体后端**的表驱动用例直接断言（差分像素断言是"投影一致"，判不了这一层）。
NumTextLayout layout_num_text(std::int32_t value, std::int32_t cells, std::int32_t flags,
                              std::int32_t anchor_x, int cell_width) {
    NumTextLayout layout;
    // 格式化成 ASCII，再按 0x10000 决定是否扩成全角 S-JIS。
    // ★ 引擎 `sub_4072F0` 的溢出口径：数字**放不下可用格数时丢掉高位**（只画低位那几格），
    //   ⛔ 不是退化成 '#'；'#' 只在"符号位都排不下"（有符号且格数 < 2）时出现。
    //   曾经按"放不下就画 '#'"实现 —— 那是把引擎的截断当成了溢出提示（取证 `0x407400` 的循环条件
    //   与 `0x40744F` 的唯一 `#` 写点）。
    int chars = 0;
    std::string text;
    const bool negative = value < 0;
    const bool plus = value > 0 ? (flags & 0x8) != 0 : (value == 0 && (flags & 0x10) != 0);
    const bool minus_zero = value == 0 && !plus && (flags & 0x20) != 0;
    const bool signed_text = negative || plus || minus_zero;
    if (signed_text && cells < 2) {   // 符号都排不下 ⇒ 单字符 '#'（引擎在这里写 0x23）
        text = "#";
        chars = 1;
    } else if (cells <= 0) {          // 无符号且没有格 ⇒ 空串（不画）
        // text 保持空串
    } else {
        const int available = cells - (signed_text ? 1 : 0);
        // 绝对值用 64 位取：引擎对 INT_MIN 取负会溢出（本版不规定那条边界，见 spec 残差）。
        const auto magnitude = negative ? static_cast<std::uint64_t>(-static_cast<std::int64_t>(value))
                                        : static_cast<std::uint64_t>(value);
        std::string digits;
        if (magnitude == 0) digits = "0";
        else {
            for (std::uint64_t v = magnitude; v > 0; v /= 10) {
                digits.insert(digits.begin(), static_cast<char>('0' + static_cast<int>(v % 10)));
            }
        }
        if (static_cast<int>(digits.size()) > available) {
            digits = digits.substr(digits.size() - static_cast<std::size_t>(available));
        }
        if ((flags & 0x1) != 0) {  // 0x1 = 补前导 0（补满可用格）
            digits.insert(0, static_cast<std::size_t>(available - static_cast<int>(digits.size())), '0');
        }
        if (signed_text) text.push_back(negative ? '-' : (plus ? '+' : '-'));
        text += digits;
        chars = static_cast<int>(text.size());
    }
    layout.ascii = text;
    layout.chars = chars;

    // X = op2 + 未用格数 × 格宽 × k：默认右齐(k=1)、0x2 居中(1/2)、0x4 左齐(0)。
    // 引擎的"第一格序号"在循环没跑时是 0（而不是 cells − 字符数）⇒ 这里夹到 0。
    const int free_cells = std::max(0, cells - chars);
    // k（以 1/2 为单位）：右齐 = 2/2、居中 = 1/2、左齐 = 0/2。
    // ★ 引擎**先乘后除**（`0x4074c0` 右齐半角 = free×格宽÷2；`0x407483` 居中半角 = free×格宽÷4）：
    //   ⛔ 不要先把格宽除成半角再乘 —— 奇数格宽会差 1px。
    const int k_num = (flags & 0x2) != 0 ? 1 : ((flags & 0x4) != 0 ? 0 : 2);
    const int divisor = (flags & 0x10000) != 0 ? 4 : 2;
    layout.x = anchor_x + (free_cells * cell_width * k_num) / divisor;

    // 全角化：'0'-'9' → 0x824F..，'-'/'+'/'#' → 0x817C/0x817B/0x8194，其它 → 0x8148。
    std::string sjis;
    if ((flags & 0x10000) != 0) {
        sjis = text;
    } else {
        for (const char c : text) {
            if (c >= '0' && c <= '9') {
                const auto code = static_cast<std::uint16_t>(0x824F + (c - '0'));
                sjis.push_back(static_cast<char>(code >> 8));
                sjis.push_back(static_cast<char>(code & 0xFF));
            } else if (c == '-') {
                sjis += "\x81\x7C";
            } else if (c == '+') {
                sjis += "\x81\x7B";
            } else if (c == '#') {
                sjis += "\x81\x94";
            } else {
                sjis += "\x81\x48";
            }
        }
    }
    layout.sjis = sjis;
    return layout;
}

void Machine::op_205(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 数值文本（0x205 / `sub_4233E0`，argc 6，推进 13）：**0x204 的数值兄弟** —— 同一个绘制入口、
    // 同样的 y 语义（格顶，落像素时 +ascent），只多了"格式化整数 + 按格数算 X"。
    // 读序 **2 → 6 → 5 → 4 →（格式化）→ 3 → 1**：
    //   op2 = 字段原点（**不是文本落点**）、op6 = 格式位、op5 = 字段格数、op4 = 值、op3 = y、op1 = 纹理槽号。
    const auto anchor_x = read_int(ops[1]);
    const auto flags = read_int(ops[5]);
    const auto cells = read_int(ops[4]);
    const auto value = read_int(ops[3]);
    // 格宽 = 「ADV 文本」条行距里**不含行間隙**的那一项 X，模式 ≠ 0 那支取 **字号格本身**。
    //   ★ 用 **A 支字号**（引擎 `Engine+0x46104` = 字号链写入的格宽；`0x75` 写的 `Font+0x313D4`，
    //     也是 0x204 出字形时用的那一支；⛔ 不是 `0x197` 的 B 支）。
    //   ⛔ 不要给"字号格 = 0"补任何缺省值：字号格为 0 ⇒ 未用格数 × 0 = 0 ⇒ **x 就应等于 anchor**。
    //     （缩放档与 `set:BlankExtentMode = 1` 两支本版未覆盖，见残差 `RES-RENDER-NUMTEXT-CELLWIDTH`。）
    const int cell_width = glyph_height_;
    // 格式化与落点 = 纯函数（正文 `:205-218`）；它**不抛**，所以读序仍是 2 → 6 → 5 → 4 → 3 → 1。
    const NumTextLayout layout = layout_num_text(value, cells, flags, anchor_x, cell_width);
    last_num_text_layout_ = layout;  // 观察口（⛔ 产品路径不读；见 `last_num_text_layout()`）
    const auto y = read_int(ops[2]);
    const auto slot = read_int(ops[0]);
    if (layout.ascii.empty() || slot < 0) return;
    const auto texture_id = static_cast<std::uint32_t>(slot);
    const TextureSlotObject* texture = texture_slot(texture_id);
    if (texture == nullptr || texture->rgba.empty()) return;  // 三条入口条件之一：槽里没有可用表面

    std::string error;
    if (draw_text_into_texture(*this, texture_id, layout.x, y, layout.sjis, &error)) {
        if (TextureEntry* entry = texture_mut(texture_id)) entry->slot.revision++;
    } else if (!error.empty()) {
        report_error(error);
    }
    return;
}

void Machine::op_20e(const Instruction& instruction) {
    (void)instruction;
    // 清屏（0x20E / `sub_41A200`，argc 0，推进 1）：`IDirect3DDevice9::Clear(0, NULL, 3, 0, 1.0, 0)`
    // ⇒ 清**当前渲染目标**整面成透明黑（没有矩形、没有操作数）。
    // ★ 本模型有当前渲染目标（`0x20d` 写）：目标是**纹理槽** ⇒ 该纹理表面清成透明黑，并按内容版本
    //   （`revision`）通知宿主重传；目标是**后缓冲**（-1）⇒ 交换链由宿主每帧重建、模型里没有可写的表面
    //   ⇒ 没有可观察状态（⛔ 不是"清纹理表"）。
    // ★ 门控支（虚拟全屏类型 == 1 且目标是后缓冲 ⇒ 再清纹理槽 38）**本模型未建模**（没有显示记录的
    //   虚拟全屏类型格）；引擎侧的域含义见残差 `RES-RENDER-CLEAR-VFS` / `RES-RENDER-CLEAR-38`。
    // ⛔ 不要顺手清纹理表 / 不要清非目标纹理：那不是这条的语义（见 ops-forensics §21）。
    // ★ **清屏请求**：这是全仓**唯一**发出清屏请求的地方（`CORE-OPS-RENDER-FRAME-STEP` 的
    //   「渲染步从不自行清屏」）。渲染步入口那次「清屏位」判定本版恒走"不清" ⇒ 请求序列上
    //   只在脚本执行本条时看得见清屏。⛔ 别把这条请求挪进渲染步：那是"每步先清黑"。
    note_renderer_request(RendererRequest::ClearScreen);
    if (render_target_ >= 0) {
        if (TextureEntry* entry = texture_mut(static_cast<std::uint32_t>(render_target_))) {
            std::fill(entry->slot.rgba.begin(), entry->slot.rgba.end(), std::uint8_t{0});
            entry->slot.revision++;
        }
    }
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
    const TextureSlotObject* texture = texture_slot(texture_id);
    if (texture == nullptr || texture->rgba.empty() || text.empty()) return;
    std::string error;
    if (draw_text_into_texture(*this, texture_id, x, y, text, &error)) {
        if (TextureEntry* entry = texture_mut(texture_id)) entry->slot.revision++;
    } else if (!error.empty()) {
        report_error(error);
    }
    return;
}

}  // namespace core
