// 字体与文本：字体对象两格 / 主副色 / 字号 / 对位 / 绘制模式 / 名称查表；
// set-string · 脚本字符串键（save / load）· 配置版本 · 十进制文本→整数 · 文本请求标记。
//
// ★ 本文件只有 handler 本体；分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"
#include "core/encoding.h"
#include "core/vm_internal.h"
#include "core/text_raster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace core {
namespace {
// `EMULATOR_CXX_TRACE_TEXT=1` ⇒ 把字号 / 行高 / 文本槽偏移的**每次写入**打到 stderr。
// ★ 诊断用（问"界面里这段文字为什么这么小"）：这三个量在引擎里是**不同的格**，混起来看不出是谁写的。
bool text_trace_enabled() {
    static const bool on = std::getenv("EMULATOR_CXX_TRACE_TEXT") != nullptr;
    return on;
}

// ★ 诊断（宿主观察口，⛔ 不是引擎语义）：`EMULATOR_CXX_TEXT_GATE_TRACE` 非空 ⇒ 把演示闸
//   （`0x300`）的**每次状态变化**（揭示一格 / 到期清除 + 重放）打一行，带**两个时刻**：
//   `host` = 迭代头那次宿主毫秒采样（门前的场景时钟由它盖），`now` = 本层的场景时钟。
//   `=2` 连"本轮什么都没做"的服务调用也打（用来量调用密度）。
//   用途：量"重放间隔是不是固定的"（用户口径：偶尔连播两三次再恢复正常）。
int text_gate_trace_level() {
    static const int level = [] {
        const char* value = std::getenv("EMULATOR_CXX_TEXT_GATE_TRACE");
        return value == nullptr ? 0 : std::atoi(value);
    }();
    return level;
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

std::int32_t Machine::find_font_name(const std::string& name) const {
    // ★ **宿主字体替换生效时**（`font_substitution_`，产品路径恒真；SCOPE §0 D1）：
    //   名字**不参与选择** ⇒ 名字表恒为「下标 0 = 宿主族名 + 标志字节 0」，**任何非空名字**都解析到它
    //   （⇒ `0x2de` 写回 0，⛔ 不是"表空、查找恒失败"的 −1）。开头的 `@` 也不影响 —— 它本来就解析到同一项。
    //   **空名字**照旧按正文的未命中写 −1：表里没有空名项，宿主族名也不是空串（`CORE-OPS-TEXT-FONT-NAME-2DE`
    //   的「未命中写 −1」没有例外）。表里连下标 0 都没有（没装宿主项）⇒ 同样按未命中（⛔ 不许返回一个
    //   会越界取表项的下标）。
    if (font_substitution_) {
        if (name.empty() || font_names_.empty()) return -1;
        return 0;
    }
    // 查名前跳过名字**开头的一个** `@`（只跳一个）。比较**逐字节**且**长度必须相等**：
    // 先按两者较短的长度比字节，再比长度（等价于 `==`，但⛔ 不做大小写折叠 / 码页转换 / 去空白）。
    std::string_view lookup(name);
    if (!lookup.empty() && lookup.front() == '@') lookup.remove_prefix(1);
    for (std::size_t i = 0; i < font_names_.size(); ++i) {
        const auto& entry = font_names_[i];
        const std::size_t shorter = std::min(entry.name.size(), lookup.size());
        bool equal = true;
        for (std::size_t k = 0; k < shorter; ++k) {
            if (entry.name[k] != lookup[k]) {
                equal = false;
                break;
            }
        }
        if (equal && entry.name.size() == lookup.size()) return static_cast<std::int32_t>(i);
    }
    return -1;
}

const std::string& Machine::host_font_family() {
    // 宿主族名（UTF-8 字节；GDI 档按它取字）。★ 与 `text_gdi.cpp` 的 `kHostFontFamily`、
    // `text_raster.cpp` 的 `kHostFaceUtf8` 是**同一个名字**（三处各写一份是历史：core 不 include 平台层）。
    static const std::string family = "Amayui CN";
    return family;
}

void Machine::set_font_substitution(bool enabled) {
    font_substitution_ = enabled;
    if (!enabled) return;
    // 装上宿主那一项（SCOPE §0 D1）：整表换成单项 `{宿主族名, 0}` —— 正文的"启动时枚举系统字体"
    // 一旦被替换策略接管，表里就只剩这一项（名字解析见 `find_font_name`）。
    // ★ 标志字节 = 0 ⇒ 位 0（等宽）**不置起** ⇒ `font_fixed_half_width` 的两条派发点都返回 0，
    //   画字 / 量度路径的行为与"名字不在表里"时**一致**（⛔ 不额外跳转渲染相关逻辑）。
    font_names_.assign(1, FontNameEntry{host_font_family(), 0});
}

bool Machine::font_fixed_half_width() {
    // 兼容旧观察口：等价于"`font_fixed_half_width(size)` 适用"（用 A 支字号量；画字那条路径的判据）。
    return font_fixed_half_width(glyph_height_ > 0 ? glyph_height_ : 24) != 0;
}

const std::string& Machine::reference_font_face() {
    // 常量参考字体：面名 `ＭＳ ゴシック`（CP932 字节；引擎字符串是 ANSI 字节）。
    static const std::string face = "\x82\x6C\x82\x72\x20\x83\x53\x83\x56\x83\x62\x83\x4E";
    return face;
}

void Machine::derive_font_description(FontDescription& desc, std::int32_t size) {
    // 派生例程（`60-ops-forward` 的「非 0 支的下一格来自度量」）：**两次度量，量的不是同一个字体**。
    // ① 第一次：**常量参考字体**（面名 `ＭＳ ゴシック`、高度 = −**A 支字号**、宽度 = −A 支字号 ÷ 2、
    //    字符 0x8C83）—— ★ 量的字号**按当时的 A 支字号**（`glyph_height_`），**不随这次派生的描述 /
    //    传参变**（正文「参考字形 = 常量参考面名 + 固定字符、按当时的 A 支字号量的」）⇒ 半格阈值不漂。
    //    ⇒ 结果进该对象自己的**参考量度**（⛔ 不是写回值）。
    if (font_measurer_) {
        font_reference_metric_ = font_measurer_(reference_font_face(), glyph_height_);
        ++font_measure_requests_;
    }
    // ② 第二次：**该描述自己的字面名**（同一个固定字符）⇒ 写回两格的是这一次。
    FontMetrics measured{};
    const bool measured_ok = font_measurer_ != nullptr;
    if (measured_ok) {
        measured = font_measurer_(desc.face, size);
        ++font_measure_requests_;
    }
    // ③ 回退判据（**按下序判**）：配置 `display:AspectMode` ≠ 0 ⇒ 不回退、走度量；否则
    //    ① 该对象的对位模式 = 0 ⇒ 回退（发行路径上恒不成立：调用点都先判过 ≠ 0）；
    //    ② 该描述自己存着的字面名（跳过开头一个 `@` 后）不在字体名表里 ⇒ 回退；
    //    ③ 表项第 0 位已置（等宽）且不属于 `IPA P` 系列 ⇒ 回退。
    // ★ ②③ 在**宿主字体替换**生效时**跳过**（`font_substitution_`，产品路径恒真）：
    //   SCOPE §0 D1 —— 字形总是 Amayui、名字**不参与选择**，所以"名字不在表里"不再构成回退依据。
    //   ⛔ 这是 SCOPE 的简化，**不是引擎语义**（引擎会判②③，见 `CORE-OPS-FORWARD-PROLOGUE` 的回退三条）；
    //   出处见 `apps/emulator-cxx/SCOPE.md` §0 D1 / §3。判据 ① 与 `display:AspectMode` 那一层**照正文保留**。
    // ★ 行为核对（替换生效时**应当不变**）：名字表现在能解析（`find_font_name` 恒 0）、宿主项标志字节 = 0
    //   ⇒ 即便**不跳过**②③，非空字面名那两步也判不出回退（`index ≥ 0` 且 `位 0 未置起`）。保留跳过是为了
    //   **空字面名**那一档也不回退 —— 那时 ② 会判成"名字不在表里"（正文语义），而产品路径的字形根本不看名字。
    //   ⛔ 不要因为"名字现在总能解析"就把这段跳过删掉（空名字那一档会变），也⛔ 不要借机改绘制路径。
    bool fallback = !measured_ok;  // 没有度量后端 ⇒ 量度不可得 ⇒ 回退
    if (!fallback && config_.get_int("display:AspectMode") == 0) {
        if (font_align_mode_ == 0) {
            fallback = true;
        } else if (!font_substitution_) {
            std::string name = desc.face;
            if (!name.empty() && name.front() == '@') name.erase(name.begin());
            const auto index = find_font_name(name);
            if (index < 0) {
                fallback = true;
            } else {
                const auto& entry = font_names_[static_cast<std::size_t>(index)];
                const bool fixed = (entry.flags & 1) != 0;
                const bool ipa = cp932_to_utf8(name).rfind("IPA P", 0) == 0;
                if (fixed && !ipa) fallback = true;
            }
        }
    }
    if (fallback) {
        // 回退 ⇒ **下一格 ← 高度 ÷ 2**（高度是负的 ⇒ 数值等于 −字号/2）；高度仍 ← −字号（不缩放）。
        desc.height = -size;
        desc.width = desc.height / 2;
        return;
    }
    // 高度 ← −字号；其中一侧在测得的字体行高 > 字号时改成 trunc(字号 ÷ 测得行高 × (−字号))。
    desc.height = -size;
    if (measured.line_height > size && measured.line_height > 0) {
        desc.height = static_cast<std::int32_t>(static_cast<double>(size) /
                                                static_cast<double>(measured.line_height) *
                                                static_cast<double>(-size));
    }
    // 下一格 ← **−测得的平均字宽**；本字形黑框宽超过"半格"阈值 ⇒ 再缩一次。
    // ★ 这一段（`0x4584C6`–`0x45851E`）里有**三个各不相同的量** ①②③（⛔ 别合并、别串用），再乘上 ④：
    //   ① **阈值**（比较用）= **参考字形黑框宽整数右移一位**（`0x4584C6 mov ecx,[esi+313D8h]`
    //      取参考盒宽 → `0x4584D8 shr ecx,1` → `0x4584DA cmp ecx,eax` / `0x4584DC jnb`：
    //      阈值 ≥ 本字形盒宽 ⇒ 跳过整段），取自**参考字形**；
    //      被比的是**本字形**的黑框宽（`0x4584D2 mov eax,[ebp+var_8C.gmBlackBoxX]`）。
    //      ⇒ 参考盒宽为奇数时这个阈值**向下取整**。
    //   ② **再缩的分子** = 同一个参考字形黑框宽的 **× 0.5（精确浮点，不取整）**
    //      （`0x4584DE fild [ebp+var_78]` 取参考盒宽原值 → `0x4584EE fmul dbl_51D7F8`；
    //      `.data:0051D7F8 dbl_51D7F8 dq 0.5` ⇒ 这里确实是 ×0.5，不是另一套取整）。
    //   ③ **再缩的分母** = **本字形的黑框宽**（`0x4584D2 mov eax,[ebp+var_8C.gmBlackBoxX]`
    //      → `0x458500 mov [ebp+var_13C],eax` → `0x458506 fild [ebp+var_13C]` → `0x458516 fdivp`；
    //      `eax` 在 `0x4584D2`–`0x458500` 之间没被改写 ⇒ ③ 与 ① 比的是**同一格**）。
    //      ⛔ 不是 `measured_box_width`：那是 `GetTextExtentPoint32W` 量出的**文本 extent**
    //      （与黑框是两个口径，见 `text_gdi.h`），而 `0x4584D2` 读的是 `gmBlackBoxX`。
    //   ④ **乘数** = 本字形的 **(−平均字宽)**（`0x458518 fimul [ebp+ho]`）。
    //   ⑤ 整式 = `trunc(② ÷ ③ × ④)`，唯一取整在 `0x45851E __ftol2_sse`（向零截断）。
    //   （`0x4584E4` / `0x45850C` 那两处 `test-jns-fadd flt_51D868` 都**紧跟**在 `fild` 之后；
    //   `.data:0051D868 flt_51D868 dd 4.2949673e9`（= 2^32）且只在源值 < 0 时加 ⇒ 是
    //   "无符号 → double"的编译器序列，不是第四套判据；本支的盒宽取自 `gmBlackBoxX`（非负）⇒ 恒不触发。）
    //   ⇒ 参考盒宽为**奇数**时 ① 与 ② 差 0.5（例：参考盒宽 25 ⇒ 阈值 12、分子 12.5）。
    //   ⛔ 别把①的整数半格当分子（R5 的错法：奇数盒宽少 1），也⛔ 别把②的 12.5 当阈值。
    //   ⛔ 别把③换成 `measured_box_width`（本仓上一版的错法：两个口径在非等宽字体上整条分岔）。
    // ★ 「半格」**不是字号 ÷ 2**（`CORE-OPS-FORWARD-PROLOGUE`）：①② 都来自该字体缓存里那个
    //   **参考字形**（常量参考面名 + 固定字符，按当时的 **A 支字号**量）的**黑框宽度**，即上面第 ① 次度量的结果
    //   （`font_reference_metric_`）—— ⛔ 不是字号的一半；只有"测得行高恰好等于字号"的字体上两者数值才相同。
    // ★ 「再缩一次」只在**对位模式 = 1** 这一档（其它非 0 档没有半格判据、也没有这一步）。
    desc.width = -measured.average_width;
    const std::int32_t half_cell_threshold = font_reference_metric_.glyph_box_width / 2;
    if (font_align_mode_ == 1 && measured.glyph_box_width > 0 &&
        measured.glyph_box_width > half_cell_threshold) {
        // 分子 = 参考盒宽 × 0.5（精确浮点）；⛔ 不是上面那个整数半格 `half_cell_threshold`。
        const double shrink_numerator =
            static_cast<double>(font_reference_metric_.glyph_box_width) * 0.5;
        // 分母 = **本字形黑框宽**（③）。`> 0` 是**除法保护**：引擎里阈值取自无符号字段
        // （`gmBlackBoxX`，非负）⇒「③ > ①」已隐含 ③ > 0；这里显式写出来，后端给 0 时不许除。
        desc.width = static_cast<std::int32_t>(shrink_numerator /
                                               static_cast<double>(measured.glyph_box_width) *
                                               static_cast<double>(-measured.average_width));
    }
}

std::int32_t Machine::font_fixed_half_width(std::int32_t size, HalfWidthPath path) {
    // `0x2de` 的位 0 = `FIXED_PITCH`（等宽）⇒ **固定半宽支**：这条名字按固定半宽用。
    // ★ **符号由两个消费者各自的对位模式判据决定，两者判据不同**（`CORE-OPS-TEXT-FONT-NAME-2DE`）：
    //   · 建平台字体那条（画字 / 派发点一）：对位模式 = 2 ⇒ **+字号/2**，否则（0 / 1 / ≥3）⇒ **−字号/2**；
    //   · 描述派生那条（量度格 / 派发点二）：对位模式 = 1 ⇒ **−字号/2**，否则（0 / 2 / ≥3）⇒ **+字号/2**。
    //   ⇒ 对位模式 ≥ 3 时两条路径**异号**（画字拿负、量度格拿正）。
    //   ⛔ 不许把两条合并成一条判据（那会让 0 / ≥3 里有一个取值变成不可达）。
    //   ⛔ 位 0 置起**不会**让这个字体被跳过（它照常参与画字）。
    const std::string& face = !font_a_pair_.variants[0].face.empty() ? font_a_pair_.variants[0].face
                                                                    : font_b_pair_.variants[0].face;
    if (face.empty()) return 0;
    // ★ 内建识别的 `IPA Pゴシック` / `IPA P明朝`：即使位 0 置起也走按字形算那一支（名字是 CP932 字节）。
    if (cp932_to_utf8(face).rfind("IPA P", 0) == 0) return 0;
    const auto index = find_font_name(face);
    if (index < 0) return 0;
    const auto& entry = font_names_[static_cast<std::size_t>(index)];
    if ((entry.flags & 1) == 0) return 0;  // 位 0 未置起 ⇒ 按字形算
    if (path == HalfWidthPath::DescriptionMetric) {
        return font_align_mode_ == 1 ? -(size / 2) : size / 2;
    }
    return font_align_mode_ == 2 ? size / 2 : -(size / 2);
}

void Machine::op_2de(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字体名查表（`CORE-OPS-TEXT-FONT-NAME-2DE`）：第 2 个是**字体名文本**、第 1 个是落点。
    // 在**系统字体名一览**里查这个名字，命中写表项下标、未命中写 **−1**。
    // ★ 宿主字体替换生效时（SCOPE §0 D1 / 产品路径恒真）名字表 = 「下标 0 = 宿主族名」⇒ **任何非空名字**
    //   都写回 0（含不存在的名字、含 `@` 开头的名字）；空名字仍写 −1。查找本身的规则在 `find_font_name`。
    // ★ 这张表**不是**"已登记名字"那两张（`0x1a5` / `0x2fe` 之后登记进字体对象的两张向量）：
    //   它是启动时枚举系统字体的结果；两者不互相替代。本模型不自己枚举（接缝 `set_font_name_table`；
    //   替换生效时由 `set_font_substitution` 只装宿主那一项）。
    const std::string name = read_text(ops[1]);
    write_int(ops[0], find_font_name(name));
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
    // 字体对位模式 ← 操作数，然后**尾跳**进该对象的重建入口（`CORE-OPS-LIST-SUBSYSTEM-SET-TAIL`：
    // 本条自己不产生返回值，重建入口的返回值直接由分派器接收）。
    // ★ 尾跳进的是**不销毁旧句柄**的那一套（正文「重建入口的两种形态」）。
    font_align_mode_ = read_int(ops[0]);
    run_font_rebuild_entry(FontRebuildBranch::NonDestroy);
    return;
}

void Machine::op_78(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 字体绘制模式 ← 操作数，然后**尾跳**进同一个重建入口（与 `0x2db` 同形、**不同字段**）。
    // ★ 绘制模式**不在**入口体内读（正文：它由绘制路径读）⇒ 这里写格 + 尾跳，入口不因它分岔。
    font_draw_mode_ = read_int(ops[0]);
    run_font_rebuild_entry(FontRebuildBranch::NonDestroy);
    return;
}

// ── 重建入口（`CORE-OPS-LIST-SUBSYSTEM-SET-TAIL`）──
// 正文的四步，按序落地：
//   ① 名字为空 ⇒ 入口**什么都不做、直接返回**（⛔ 不是"重建一个空字体"，也⛔ 不是报错）；
//   ② 名字非空 ⇒ 两份描述的**高度**都**无条件**写成 −字号（与对位模式无关）；
//      ★ **紧接高度的下一格本入口一概不写**（正文："为 0 ⇒ 两份描述的高度保持 ② 的 −字号、
//      紧接高度的下一格**不动**"；只有 ④ 的派生会重写它）。
//   ③ 随后向平台发**一次**「建参考字体 + 取量度」的请求，把量度写进**两张自己的**结构
//      （一张文本度量、一张字形度量）。★ 这一请求与派生例程的两次度量**不是同一件事**：
//      派生那两次是逐描述量的，这里这一次是入口自己的；两处落的**不是同一张结构**。
//   ④ 最后按**对位模式是否为 0** 分岔（★ 判据是"是否为 0"，⛔ 不是"是否为 1"）：
//      为 0 ⇒ 跳过派生；不为 0 ⇒ A 支对**自己的两份描述各跑一次**（共两次），
//      B 支**只对自己的横排那一份跑一次**（竖排不参与派生）。
//   ⑤ 两套入口的生命周期差：B 支重建前**先销毁三枚**已有的平台字体引用 —— 两枚是对象自己的、
//      第三枚是**当时选在绘制表面上的那一枚旧引用**（销毁它之前先把绘制表面换回原来的字体）；
//      然后建新字体并取一次**文本度量**（只文本，不取字形度量）。
//   ⑦ 入口尾段（参考量度与派生都跑完之后）还有一段**字形缓冲的重分配与字形装载**
//      （两枚缓冲先释放、再按描述里的一个块算尺寸重新分配、然后逐字装载）—— **本版不建模**
//      （正文如此；两块缓冲的领域角色与逐字装载行为见残差 `RES-OPS-SET-TAIL-FIELDS`）⇒
//      ⛔ 不许据此认为"引擎里没有这一段"，也⛔ 不许据它规定字形位图的内容。
// ★ 名字格：两套各自的名字格就是**各自那一对的横排描述的字面名**（A 对 / B 对是两个不同对象 ⇒
//   两个名字格不同格，⛔ 不许合并）。模型不建模引擎"字体对象建立时写入的默认面名"（未取证）
//   ⇒ 没写过名字的时候这里读到空串，正好走 ① 那一支。
void Machine::run_font_rebuild_entry(FontRebuildBranch branch) {
    const bool destroy = branch == FontRebuildBranch::Destroy;
    FontDescriptionPair& pair = destroy ? font_b_pair_ : font_a_pair_;
    const std::int32_t size = destroy ? font_b_height_ : glyph_height_;
    ++font_rebuild_requests_;
    // ① 空名早退：一个格都不写、一个请求都不发。
    if (pair.variants[0].face.empty()) return;
    // ② 两份描述的高度无条件 ← −字号（⛔ 不动下一格）。
    for (auto& variant : pair.variants) variant.height = -size;
    // ③ 一次「建参考字体 + 取量度」。
    FontBuildSpec spec;
    spec.height = -size;
    spec.width = -(size / 2);          // 整数除以 2 向零截断（引擎 `cdq; sub eax,edx; sar eax,1`）
    spec.weight = font_metric_a_;      // 粗细 = 该字体对象的粗细格（`0x2bd` 写的那一格）
    spec.charset = 1;                  // 字符集 = 默认（Win32 DEFAULT_CHARSET）
    if (destroy) {
        // B 支：先把绘制表面换回原字体，再销毁三枚旧引用，然后建**新字体**（= 该支自己的字面名）。
        font_lifecycle_log_.push_back(FontLifecycle::SurfaceRestore);
        for (int i = 0; i < 3; ++i) font_lifecycle_log_.push_back(FontLifecycle::ReleaseHandle);
        spec.face = pair.variants[0].face;
    } else {
        // A 支：建的是**常量参考字体**（面名 `ＭＳ ゴシック`），与这一支自己的字面名无关。
        spec.face = reference_font_face();
    }
    if (destroy) {
        font_build_spec_destroy_ = spec;
        ++font_build_requests_destroy_;
    } else {
        font_build_spec_non_destroy_ = spec;
        ++font_build_requests_non_destroy_;
    }
    if (font_reference_builder_) {
        const FontBuildMetrics metrics = font_reference_builder_(spec);
        if (destroy) {
            // 销毁那一套只把**新建字体**的文本度量写进**它自己的另一张**结构（不取字形度量）。
            font_text_metric_destroy_ = metrics.text;
        } else {
            font_text_metric_non_destroy_ = metrics.text;
            font_glyph_metric_non_destroy_ = metrics.glyph;
        }
    }
    // ④ 对位模式是否为 0 的分岔（判据是"是否为 0"）。
    if (font_align_mode_ != 0) {
        if (destroy) {
            derive_font_description(pair.variants[0], size);  // 只横排那一份（共一次）
        } else {
            for (auto& variant : pair.variants) derive_font_description(variant, size);  // 共两次
        }
    }
    // ⑦ 入口尾段的字形缓冲重分配 + 逐字装载本版不建模（残差 `RES-OPS-SET-TAIL-FIELDS`）。
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
    // ★ 逐字绘制条目**只在场景绘制模式 = 1 的纹理画布路径**上建（模式 ≠ 1 ⇒ 旧路径不建条目、
    //   也不读键基数 / 数量）。判据是**绘制项表**，⛔ 不是"屏幕上有没有字"。
    if (draw_mode_ != 1) return;
    const ListRecord& rec = list_records_[static_cast<std::size_t>(slot)];
    // 键基数可用 = 脚本写过 `0x213` **或** `0x70` 补过兜底（`key_base_ready()`）。
    // ⛔ 只看 `key_base_set` 会让"没调 0x213 的层"（= 本作 ADV 的消息层）一条条目都不建。
    if (!rec.key_base_ready()) return;
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
    if (rec.key_base_ready()) {
        for (std::int32_t i = 0; i < state.draw_index; ++i) {
            draw_items_.erase(static_cast<std::uint32_t>(rec.f68) + static_cast<std::uint32_t>(i));
        }
    }
    state.draw_index = 0;
    scene_dirty_ = true;  // 条目被移除 ⇒ 下一次渲染步必须重画
}

void Machine::clear_text_layer(int slot) {
    if (slot < 0 || slot >= 10) return;
    // ★ 纹理画布存在于否由**场景绘制模式**分岔（`62-adv-text` 的「文本层：矩形、原点、清空与画布」）：
    //   模式 = 1 才有画布（才清像素、才释放逐字条目）；模式 ≠ 1 走旧路径（本版只规定否定面）。
    if (draw_mode_ == 1 && ensure_text_layer_texture(slot)) {
        const auto id = text_layer_texture_id(static_cast<std::size_t>(slot));
        TextureEntry* entry = texture_mut(id);
        if (entry != nullptr && entry->slot.present) {
            std::fill(entry->slot.rgba.begin(), entry->slot.rgba.end(), 0);
            entry->slot.revision++;
            entry->record.non_file = true;
        }
        erase_text_layer_draw_items(slot);
    }
    // ★ 下面这几件**不分模式、任何模式都做**：文本游标复位到文本原点、布局字符格序列清空、
    //   揭示游标归零、清掉待画格。
    const ListRecord& rec = list_records_[static_cast<std::size_t>(slot)];
    TextLayerState& layer = text_layer_state_[static_cast<std::size_t>(slot)];
    layer.cells.clear();
    layer.reveal_cursor = 0;
    layer.reveal_due_ms = 0;
    layer.reveal_done_ms = 0;
    layer.gate_cycles = 0;
    // 清画布同时丢掉这一层的**待画格**：引擎那边 `0x71` 清的是布局向量（源），我们的队列就是它的对应物
    // —— 不清会让"清空后紧接着的下一帧"把上一段的字补画进来（可见的串字）。
    erase_pending_glyphs(slot);
    text_layer_state_[static_cast<std::size_t>(slot)].cursor_x = rec.f1c;
    text_layer_state_[static_cast<std::size_t>(slot)].cursor_y = rec.f20;
}

TextRunFont Machine::text_run_font(std::int32_t flags) const {
    // 一位 → 一支字体对象（`ADV-TEXT-LAYER` 的「位 2 = 变体选择」）：置起 ⇒ 变体那一支（B 支），
    // 否则 A 支。★ 换的是**整支**（字面名 / 字号 / 绘制模式 / 粗细一起换），⛔ 不是只换字号。
    // ★ 绘制模式与粗细是**字体对象**的格（`0x78` / `0x2bd` 各一格）⇒ 两支取到的是同一个值；
    //   差别落在字面名与字号上。⛔ 不另建"第二支的绘制模式格"——正文没有它的写者（残差）。
    TextRunFont font;
    font.variant = (flags & 4) != 0;
    const FontDescriptionPair& pair = font.variant ? font_b_pair_ : font_a_pair_;
    font.face = pair.variants[0].face;  // 横排那一份的字面名
    font.size = font.variant ? (font_b_height_ > 0 ? font_b_height_ : 10)
                             : (glyph_height_ > 0 ? glyph_height_ : 24);
    font.draw_mode = font_draw_mode_;
    font.weight = font_metric_a_;
    return font;
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
    // ★ **追加侧**判的是**它收到的标志参数**（`flags`）的第 2 位 —— 那一刻记录还没写出 ⇒
    //   ⛔ 不许写成"读记录自己的标志字"（那是循环）。把该位置进记录的同时，按它选**整支**
    //   字体对象并落库（绘制侧之后按**记录自己**的标志字取同一支）。
    record.font = text_run_font(flags);
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
            // ★ 字号 / 模式 / 字面名 / 两色都是**追加当时**的快照（引擎在这里就逐字写出）：
            //   画的时候再读字体对象 ⇒ 别的行在这期间改 `0x75` 会让这一格按别人的字号画出来。
            // ★ 这一趟的支由**本段收到的标志参数**的第 2 位选（`font_text_gate_5ec7c_` 原样传下去）：
            //   主线正文 run 的位 2 未置 ⇒ A 支。
            const TextRunFont run_font = text_run_font(font_text_gate_5ec7c_);
            pending.size = glyph_height_;
            pending.weight = run_font.weight;
            pending.draw_mode = run_font.draw_mode;
            pending.face = run_font.face;
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
        // 振り仮名 run 的标志参数**位 2 置起** ⇒ 变体那一支（B 支）：字号 / 字面名 / 绘制模式
        // **整支**换（⛔ 不是只换字号）。追加侧判的是**要传下去的参数**，不是记录自己的标志字。
        const TextRunFont ruby_font = text_run_font(font_text_gate_5ec7c_ | 4);
        const int ruby_size = ruby_font.size;
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
                pending.weight = ruby_font.weight;
                pending.draw_mode = ruby_font.draw_mode;
                pending.face = ruby_font.face;
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
    TextStyleSnapshot style{glyph.size, glyph.weight, glyph.draw_mode, glyph.primary, glyph.secondary};
    style.face = glyph.face;  // 变体支 = 那一支自己的字面名（整支字体对象换了）
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

bool Machine::read_text_skip_requested() const {
    // `CORE-OPS-MSG-BLOCK-304-305` ②：`message:ReadTextSkip` 非 0 **且**「已读」命中 ⇒ 置第 27 位。
    // ★ 与 `0x6e` / `0x196` 的追加前置支是**同一判据、同一格**（spec 原文）。
    // ★ 「已读」用的是哪张表 / 键是什么未取证（残差 `RES-MSG-BLOCK-READ-HIT`）⇒ 由接缝提供，
    //   缺省 = 无命中。⛔ 不要把"键非 0"单独当成充分条件（那会把未读过的文本也当跳过）。
    return config_.get_int("message:ReadTextSkip") != 0 && read_text_skip_hit_ &&
           read_text_skip_hit_();
}

void Machine::begin_text_typing_or_drain() {
    // 引擎 `0x41ec35` / `0x41fcf8` 的两档：**每字间隔 > 0 且状态字第 27 位未置** ⇒ 置第 29 位 +
    // 启动文本计时器（间隔 = `Font+0x560` = `param_cell_`、起点 = 现在）；否则 ⇒ 立即追加 + 同帧抽干
    // （`sub_46cbf0`）。
    // ★ 第 27 位由两处产品路径置起：`0x71` 的子系统调用返回非 0，以及本条之前的
    //   **追加前置支**（`0x6e` / `0x196`，判据 = `message:ReadTextSkip` 非 0 ∧ 已读命中）。
    //   它一旦置起，本条就走"立即追加 + 同帧抽干"（spec：旁路）。
    const std::int32_t speed = param_cell_.value_or(0);
    // ★ 演示闸（`0x300`）开着的层：字符格交给闸门泵，不进打字队列（见 `draw_text_layer_text`）。
    //   但**必须把打字位置起** —— 推进器正是闸的服务者、且**只在打字位置起时被调用**
    //   （`60-ops-text.md:375-386`）；不置它，闸开着的层永远不会被揭示。
    //   参数格 = 0 ⇒ 推进器下一次调用就抽干并清位（一次成、不重放）；≠ 0 ⇒ 保持置起、按闸的节奏循环。
    {
        const int slot = resolve_text_layer(current_text_slot_);
        if (slot >= 0 && slot < 10 && (font_slot_state_[slot] & 1) != 0) {
            state_flags_ |= (1u << 29);
            return;
        }
    }
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
    TextStyleSnapshot style{glyph.size, glyph.weight, glyph.draw_mode, glyph.primary,
                            glyph.secondary};
    style.face = glyph.face;
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
    // 诊断（见 `text_gate_trace_level()`）：每个**改变状态**的分支打一行；`=2` 连无事发生的调用也打。
    const int trace = text_gate_trace_level();
    static std::uint32_t trace_last_host[16] = {};
    const auto trace_line = [&](const char* what) {
        const std::uint32_t host = host_clock_ms_;
        const std::uint32_t last = trace_last_host[slot & 15];
        std::printf("[textgate] %-7s host=%u dt=%u now=%u slot=%d cursor=%zu/%zu due=%u done=%u "
                    "speed=%d hold=%d cycles=%d clamps=%u backsteps=%u\n",
                    what, host, last == 0 ? 0u : host - last, now, slot, state.reveal_cursor,
                    state.cells.size(), state.reveal_due_ms, state.reveal_done_ms, speed,
                    static_cast<int>(font_slot_deadline_[slot]), state.gate_cycles,
                    host_clock_clamps_, clock_backsteps_);
        trace_last_host[slot & 15] = host;
    };
    if (speed <= 0) {
        // 参数格 == 0：引擎走"抽干到末格"支并直接跳到槽循环尾（`0x409452`）⇒ 整段一次揭示，
        // **不记起点、不清条目、不重放**。
        if (trace != 0) trace_line("drain");
        reveal_all_text_layer_cells(slot);
        return;
    }
    if (state.reveal_cursor < state.cells.size()) {
        if (state.reveal_due_ms == 0) state.reveal_due_ms = now;
        if (now < state.reveal_due_ms) {
            if (trace >= 2) trace_line("wait");
            return;
        }
        reveal_text_layer_cell(slot);
        state.reveal_due_ms = now + static_cast<std::uint32_t>(speed);
        if (state.reveal_cursor >= state.cells.size()) state.reveal_done_ms = now;
        if (trace != 0) trace_line("reveal");
        return;
    }
    // 全部揭示完：停「期限」毫秒，然后清本层绘制条目 + 游标归 0（闸位保持）⇒ 下一拍从第一格重来。
    if (state.reveal_done_ms == 0) state.reveal_done_ms = now;
    const std::int32_t deadline = font_slot_deadline_[slot];
    const std::uint32_t hold = deadline > 0 ? static_cast<std::uint32_t>(deadline) : 0;
    if (now - state.reveal_done_ms < hold) {
        if (trace >= 2) trace_line("hold");
        return;
    }
    erase_text_layer_draw_items(slot);
    state.reveal_cursor = 0;
    state.reveal_due_ms = 0;
    state.reveal_done_ms = 0;
    ++state.gate_cycles;
    if (trace != 0) trace_line("replay");
}

// 「文本槽在用标志」（机器格 `[77984h]`，取证 `report-text-gate.md` §1）：
// 槽 1..9 **任一状态字非 0** ⇒ 1（`0x409421 cmp dword ptr [edi],0` → `0x409426 mov [esi+77984h],1`；
// 入口 `0x409413` 先清 0）。★ 槽 0 不参与 —— 服务体的槽循环从槽 1（`0x7798C`）起。
// ⛔ 它不是待处理命令队列的占用标记：后者的判空口径是「头 ≥ 尾」（`0x40FB73` / `0x40FB79`），
//    全语料没有"队列非空"的单格；把这一位当队列用会让演示闸在打字期永不派发（§5.3）。
bool Machine::text_slot_in_use() const {
    for (int slot = 1; slot < 10; ++slot) {
        const auto it = font_slot_state_.find(slot);
        if (it != font_slot_state_.end() && it->second != 0) return true;
    }
    return false;
}

// **文本推进器的一趟**（`sub_409400`）。返回值 = 这一趟的**文本槽在用标志**（`[77984h]`）：
// 门上的第 29 位服务体就是拿它决定「随即派发一条」还是「交回迭代头」（`0x41342D`）。
// ★ 它是**入口快照**：引擎在槽循环里先置标志（`0x409426`）、之后才可能把该槽抽干并清 0
//   （`0x4094C2` / `0x4094C4`）⇒ 本趟被抽干的槽**照样算进去**，而门读的就是这份快照。
bool Machine::service_text_layers() {
    const bool slot_in_use = text_slot_in_use();
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
        return slot_in_use;
    }
    // 打字中（第 29 位）才推进；**每 D 毫秒一格、每帧最多一格**（`sub_453b60` 的闸门：
    // `elapsed <= n × interval` ⇒ 本帧一格都不画，超出才画一格并把 n 推到 ⌊elapsed/interval⌋+1）。
    if ((state_flags_ & (1u << 29)) == 0) {
        // 不在打字态：队列本应已被"立即抽干"清空；万一有残留（宿主换了时钟 / 加载期）一次画完，
        // ⛔ 不要退回"每帧一格"（那会让"间隔 = 0"的脚本又变成打字机）。
        if (!pending_glyphs_.empty()) drain_pending_glyphs();
        return slot_in_use;
    }
    if (pending_glyphs_.empty()) {
        state_flags_ &= ~(1u << 29);  // 没有待画格 ⇒ 没有可服务的东西（引擎的"无活动槽"支）
        return slot_in_use;
    }
    if (input_pressed_) {
        // 按键 / 跳过 ⇒ **同帧抽干** + 清第 29 位（引擎 `0x40953c`–`0x409554`；它同时置第 27 位）。
        // ★ 本批只落 `0x305` / `0x71` / 追加前置支这三处置点；**推进器这一处置第 27 位尚未落**
        //   （另单）。抽干与清位本身不变。
        drain_pending_glyphs();
        state_flags_ &= ~(1u << 29);
        return slot_in_use;
    }
    if (text_timer_interval_ms_ <= 0) {  // 计时器没被正确启动（防御；正常路径间隔 > 0）
        draw_front_pending_glyph();
        if (pending_glyphs_.empty()) state_flags_ &= ~(1u << 29);
        return slot_in_use;
    }
    const std::uint32_t elapsed = scene_clock_ms_ - text_timer_start_ms_;
    const std::int64_t due = static_cast<std::int64_t>(text_timer_step_) * text_timer_interval_ms_;
    if (static_cast<std::int64_t>(elapsed) <= due) return slot_in_use;  // 未到期 ⇒ 本帧一格都不画
    const auto crossed = static_cast<std::int32_t>(
        elapsed / static_cast<std::uint32_t>(text_timer_interval_ms_));
    text_timer_step_ = crossed + 1;
    draw_front_pending_glyph();
    if (pending_glyphs_.empty()) state_flags_ &= ~(1u << 29);  // 末格 ⇒ 清第 29 位（`0x4096b2`）
    return slot_in_use;
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
    if (slot >= 0 && slot < 10) ++line_records_[static_cast<std::size_t>(slot)];
}

void Machine::op_71(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 文本请求（`0x71` / `sub_41ED80` → `sub_45EC60`，argc 1）：
    // ① 当前帧「本条位置」← 本条指令的 dword 下标；② 清空该层的画布与游标；
    // ③ 把画布按层矩形排成一条绘制项（键基数 = `0x213` 写的 +104 格）。
    // ★ 引擎还会做两处整块拷贝 / 一次子系统内调用 / 有条件置状态字第 27 位：
    //   本模型只落**条件置位**（`CORE-OPS-LIST-TEXT-REQUEST`：文本子系统的一次调用返回非 0 ⇒ 置
    //   第 27 位，与 `0x88` 清的是同一位）。那次调用的返回值领域含义未定 ⇒ 由接缝提供、缺省 0。
    const auto requested = read_int(ops[0]);
    frame_mut().position = static_cast<std::uint32_t>(frame_mut().ip / 4);
    const std::int32_t request_result = text_request_result_ ? text_request_result_(requested) : 0;
    const int slot = resolve_text_layer(requested);
    if (slot >= 0 && slot < 10) line_records_[static_cast<std::size_t>(slot)] = 1;  // 清空后追加 1 条
    clear_text_layer(slot);
    if (request_result != 0) state_flags_ |= (1u << 27);
    return;
}

// 字体对象的两对描述（`CORE-OPS-FORWARD-PROLOGUE`）：
//   · A 对（描述 A1 @+4D0 / A2 @+18E54）由 `0x75` 设字号、`0x1a5` 设字面名；
//   · B 对（描述 B1 @+50C / B2 @+18E90）由 `0x197` 设字号、`0x2fe` 设字面名。
// 两份**不是副本而是变体**：同一对的两份高 / 下一格完全相同，只有字面名不同（原样 / `'@'` + 原样）。
// 写序按引擎入口体：先下一格（−(字号/2)），再高（−字号），最后两份字面名（EA 0x43297c..0x4329bb /
// 0x432e78..0x432ead）。整数除以 2 是**向零截断**（引擎 `cdq; sub eax,edx; sar eax,1`）。
void Machine::write_font_pair(FontDescriptionPair& pair, const std::string& face, std::int32_t size) {
    const std::int32_t width = -(size / 2);
    const std::int32_t height = -size;
    pair.variants[0].face = face;
    pair.variants[1].face = "@" + face;
    for (auto& variant : pair.variants) {
        variant.width = width;
        variant.height = height;
    }
    return;
}

void Machine::op_1a5(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // `0x1a5` 设**A 支那一对**描述的字面名（`sub_432970`，argc 1，操作数**按文本读**：
    // 按整型读会得到另一件事）。⛔ 不许与 `0x2fe` 合并：合并会在改 A 支名字时把 B 支名字一起改掉。
    // 它读**这一支自己的字号**（A 支：`Font+0x313D4`，EA 0x43297c）重写两份描述的高 / 下一格。
    // ★ 入口体**不分岔**：写完高 = −字号、下一格 = −字号/2 就请求重建；**重建路径**自己按对位模式
    //   分岔：≠ 0 ⇒ 对该支两份描述跑**同一个**度量派生例程、**覆写**高度与下一格；= 0 ⇒ 不覆写。
    const std::string face = read_text(ops[0]);
    write_font_pair(font_a_pair_, face, glyph_height_);
    run_font_rebuild_entry(FontRebuildBranch::NonDestroy);
    return;
}

void Machine::op_2fe(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // `0x2fe` 设**B 支那一对**描述的字面名（`sub_432dd0`，argc 1，按文本读；EA 0x432e78..0x432ecd）。
    // 与 `0x1a5` 同形，但写另一对：`0x75`/`0x1a5` 与 `0x197`/`0x2fe` 各一对、互不覆盖。
    // ★ B 支的重建入口是**另一个函数**（`sub_45A6E0`），但派生例程是同一个；
    //   而且那一套是**销毁旧句柄**的那一套 ⇒ 只对**横排那一份**跑一次派生（见下 `0x197`）。
    const std::string face = read_text(ops[0]);
    write_font_pair(font_b_pair_, face, font_b_height_);
    run_font_rebuild_entry(FontRebuildBranch::Destroy);
    return;
}

void Machine::op_197(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // `0x197` 设字号（`sub_41FDD0`，**argc 1**）：op1 = **字形高度（未缩放的逻辑像素）**，**没有**字体对象下标。
    // ★ 引擎的写点（`sub_418680`）：全局字号格 ← v；**十个槽全写** `槽+0xC8 = v`（展开、无下标、无越界检查）；
    //   B 对描述的两份变体：`lfHeight = -v`、`lfWidth = -(v/2)`（向零截断）；随后重建字体。
    // ★ 它**不碰**行高那一组（`Font+0x313D4`，那是 `0x75` 的活）—— 两件事一个格是本模型以前的错。
    // ★ 缺省 8（引擎 `0x46551C` 的初值），不是行高的 24。
    const auto value = read_int(ops[0]);
    font_b_height_ = value;
    for (auto& slot : text_slots_) slot.glyph_height = value;
    // ★ 入口体**先无条件写**：该支两份描述 `高度 ← −字号`、`下一格 ← −(字号 ÷ 2)`（**不看**对位模式）。
    const std::int32_t height = -value;
    const std::int32_t width = -(value / 2);
    for (auto& variant : font_b_pair_.variants) {
        variant.height = height;
        variant.width = width;
    }
    // ★ 随后请求重建：走 **B 支（销毁旧句柄那一套）** 的重建入口 —— 它自己按对位模式分岔，
    //   并且**只对横排那一份**跑派生（共一次，⛔ 不是两份各一次）。
    run_font_rebuild_entry(FontRebuildBranch::Destroy);
    if (text_trace_enabled()) std::fprintf(stderr, "[text] 0x197 font_b_height=%d\n", value);
    return;
}

void Machine::op_75(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // `0x75` 设的是**A 支字号**（`sub_4185F0`）：`Font+0x313D4 ← v`（`0x418611`），
    // A 对两份描述的 lfHeight ← −v（`0x418617` / `0x418619`），随后重建字体（`sub_459F40`）。
    // ★ **0x204 画文字用的就是这一支**（`0x459F63` 读它当逐字字体的高度）⇒ 它不是"行高"。
    // ★ 两份描述的下一格按**那个对象自己的开关**（`0x4185FD` 的 `cmp [esi+313D0h],0`）分两支：
    //   0 支把 −(v/2) 写进两份的下一格（`0x41865E..0x41866D`）；非 0 支走 `sub_459A20` 的派生路径 ——
    //   正文把这一格定性为**对位模式**（`0x2db` 写的那格；初值 0，与绘制模式那格不同），
    //   非 0 支**由平台字体度量写回**（见 `CORE-OPS-FORWARD-PROLOGUE` 的「非 0 支的下一格来自度量」）。
    //   ⇒ 本模型读的 `font_align_mode_` 就是那格对位模式（合同，不是实现侧假设）。
    const auto value = read_int(ops[0]);
    glyph_height_ = value;
    // ★ 入口体**先无条件写**：两份描述 `高度 ← −字号`、`下一格 ← −(字号 ÷ 2)`（**不看**对位模式）。
    for (auto& variant : font_a_pair_.variants) {
        variant.height = -value;
        variant.width = -(value / 2);
    }
    // ★ 随后请求重建：走 **A 支（不销毁旧句柄那一套）** 的重建入口 —— 它自己按对位模式分岔，
    //   非 0 时对**两份描述各跑一次**派生（共两次），并写参考字体的两张量度结构。
    run_font_rebuild_entry(FontRebuildBranch::NonDestroy);
    if (text_trace_enabled()) std::fprintf(stderr, "[text] 0x75 glyph_height(family A)=%d\n", glyph_height_);
    return;
}

// ── 建字体缩放（`60-ops-forward`「字号是"未缩放"的量」；`sub_465840`）──
// 视口尺寸是**平台输入**：缺省按配置的客户区尺寸（⇒ 比例 1.0），宿主 / 守卫用接缝注入真实视口。
Machine::Viewport Machine::viewport_input() const {
    if (viewport_source_) return viewport_source_();
    return Viewport{config_.get_int("set:WinX"), config_.get_int("set:WinY")};
}

// 重算缩放的**两个时机**：① 启动、字体对象建立之后一次（构造器）；② 引擎重新应用字体设置的路径
// （`sub_465840` 的调用点 0x41806E / 0x4692D7；本模型没有对应 opcode ⇒ `reapply_font_settings` 接缝）。
// 合同（逐句）：
//   · 两个分量初值 1.0；**只在窗口态 ∧ 虚拟全屏类型 = 2** 时按商重算，其余一律 1.0；
//   · 高分量 = 视口高 ÷ 配置客户区高（`set:WinY`，EA 0x465884），它乘进 lfHeight；
//     宽分量 = 视口宽 ÷ 配置客户区宽（`set:WinX`，EA 0x465875），**不参与**字号缩放；
//   · 两个分量会连同高分量一起抄进**十个文本槽的两格**（`槽+0x74` 宽 / `槽+0x78` 高，EA 0x4658C3..0x465965）；
//   · 两个分量各自与"上次算出的那个"比，**两个都相等 ⇒ 整趟什么都不写**（EA 0x46588D..0x465A11），
//     不重抄槽、不重建字体。
void Machine::refresh_font_scale() {
    double width = 1.0;
    double height = 1.0;
    // 「窗口态」= 建窗的形态字：配置 `display:ScreenMode`（`06-window`「窗口创建」的形态判据；
    // `display:ForceScreen == 1` 时引擎在建窗前把它写成 1 ⇒ 这里按同一个谓词取）。
    const bool windowed = config_.get_int("display:ScreenMode") != 0 ||
                          config_.get_int("display:ForceScreen") == 1;
    // 条件 2：虚拟全屏类型 = 2（EA 0x465866 的 `cmp [eax+48Ch],2`）。
    if (windowed && config_.get_int("display:VirtualFullScreenType") == 2) {
        const auto viewport = viewport_input();
        const auto client_width = config_.get_int("set:WinX");
        const auto client_height = config_.get_int("set:WinY");
        // 配置客户区没有尺寸时两个分量留 1.0（实现侧防御：引擎那一步是无检查的除法，
        // 0 会得到 inf；那不是可建模的引擎语义）。平台输入本来应当为正。
        if (client_width > 0 && client_height > 0) {
            width = static_cast<double>(viewport.width) / static_cast<double>(client_width);
            height = static_cast<double>(viewport.height) / static_cast<double>(client_height);
        }
    }
    if (width == font_scale_width_ && height == font_scale_height_) return;  // 整趟什么都不写
    font_scale_width_ = width;
    font_scale_height_ = height;
    for (auto& slot : text_slots_) {
        slot.scale_width = width;
        slot.scale_height = height;
    }
    ++font_scale_write_count_;
    // 缩放后重建字体：A 支入口（0x4659fc）/ B 支入口（0x465a03）各一次请求。
    request_font_rebuild();
    request_font_rebuild();
    return;
}

void Machine::reapply_font_settings() {
    refresh_font_scale();
}

std::int32_t Machine::font_build_height(bool branch_b) const {
    // `高度 ← trunc(高度 × 比例 + 0.5)`：高度是**负值**（字号取相反数）⇒ 向零截断等价于"向上取整"，
    // ⛔ 不是四舍五入（在负值上两者差 1）。加的是字面常数 0.5（`dbl_51D7F8`）。
    const auto& pair = branch_b ? font_b_pair_ : font_a_pair_;
    const double scaled = static_cast<double>(pair.variants[0].height) * font_scale_height_ + 0.5;
    return static_cast<std::int32_t>(std::trunc(scaled));
}

void Machine::op_198(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // `0x198` 设**文本层的左上位置**（`sub_41FE10` → `sub_456400`，**argc 3**、推进 7）：
    //   op1 = 层号（`0` ⇒ 默认层，与 `0x70` / `0x71` / `0x79` 同一个选择子），
    //   op2 → 层记录 +0x0C（左），op3 → 层记录 +0x10（上）。
    // ★ 这两格**就是** `0x70` 第 4 / 5 个操作数写的那两格（`0x45d68a` / `0x45d690`）：
    //   `0x70` 先写尺寸与初始位置、`0x198` 只搬位置 —— 不改宽 / 高、不建 / 不重建画布、不清内容。
    //   逐字绘制路径拿它当**层左上**（`0x45bff2` 的 `x = [层+0x0C] + cell.x`），不是"再加一次的偏移"。
    //   ⇒ 实测（2026-11-02）：GAMESTART 的说明文字层 `0x70 9 492 92 0 0` + `0x198 9 706 425`
    //     必须落在 (706,425) 那块棕色说明区；把它当独立偏移 ⇒ 文字画到屏幕左上角。
    // ▼ 读序 **3 → 2 → 1**（`0x41FE10` 先 push 3、再 push 2、再 push 1）。
    // ▼ 记录指针为空 ⇒ 一格都不写（`0x456417` / `0x456419` 的 `test eax,eax` / `jz`）；
    //   层号越界是 UB（引擎拿它索引字体对象的层指针表）⇒ 本实现选择不写、不诊断。
    const auto top = read_int(ops[2]);
    const auto left = read_int(ops[1]);
    auto group = read_int(ops[0]);
    if (group == 0) group = default_group_;
    if (group < 0 || group >= 10) return;
    ListRecord& record = list_records_[static_cast<std::size_t>(group)];
    record.f0c = left;
    record.f10 = top;
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
    ++text_appends_;
    draw_text_layer_text(slot, text, std::string());
    // ★ 成功追加时置消息臂位的第 16 位（引擎 `0x41EB7D`：`or [esi+77A04h], 10000h`；只在
    //   `0x304` 已把臂位第 0 位置起的支路里做）。`0x305` 的「武装」= 位 0 与位 16 同时成立。
    //   ⛔ 不置它 ⇒ `0x305` 的整段块结束逻辑永远是死代码。
    if ((msg_arm_ & 1) != 0) msg_arm_ |= 0x10000;
    // 追加前置支（`62-adv-text` 的「逐字符显示」）：门判**之前**若「读文本跳过」命中 ⇒ 置第 27 位，
    // 于是本条随后落进旁路（不置打字位），直到下一次清掉第 27 位。
    // ★ spec 同时说会「置一个标志格」，但那一格的身份与消费者未取证（残差 `RES-ADV-TEXT-APPEND-PRE27`）
    //   ⇒ 本模型只落**已定**的第 27 位置位，⛔ 不发明一个标志格。
    if (read_text_skip_requested()) state_flags_ |= (1u << 27);
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
    ++text_appends_;
    draw_text_layer_text(slot, text, window.rubi);
    // ★ 与 `0x6e` 同一条：成功追加时置消息臂位第 16 位（引擎 `0x41FCD8`）。
    if ((msg_arm_ & 1) != 0) msg_arm_ |= 0x10000;
    // 追加前置支（`62-adv-text` 的「逐字符显示」）：门判**之前**若「读文本跳过」命中 ⇒ 置第 27 位，
    // 于是本条随后落进旁路（不置打字位），直到下一次清掉第 27 位。
    // ★ spec 同时说会「置一个标志格」，但那一格的身份与消费者未取证（残差 `RES-ADV-TEXT-APPEND-PRE27`）
    //   ⇒ 本模型只落**已定**的第 27 位置位，⛔ 不发明一个标志格。
    if (read_text_skip_requested()) state_flags_ |= (1u << 27);
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
    // ★ `0x300` **只写槽状态字与期限**（`0x4269D3` / `0x4269DF`）：`sub_426990` 全 28 行**不含门字**
    //   `0AAB44h` ⇒ 它**不置第 29 位**（取证 `report-text-gate.md` §4.1）。
    //   它置起的是「**槽在用**」—— 由第 29 位服务体那一趟从槽状态字读回来（`0x409421` / `0x41342D`）；
    //   门位由文本追加（`0x6e` / `0x196`）或块结束（`0x305`）置起。
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
    // 未武装 ⇒ 只清臂位、什么都不做。武装 ⇒
    //   ① 揭示游标**回卷**到块开头；
    //   ② 读 `message:ReadTextSkip`，非 0 且"已读"命中 ⇒ 置门字第 27 位 —— 这两步都在**分叉之前**，
    //      且 ② 的结果**直接决定**走哪一支（`62-adv-text.md:394-397`）；
    //   ③ **按 spec 二选一**（`62-adv-text.md:398-404`）：
    //      · **打字机支**（每字间隔 ≠ 0 ∧ 第 27 位未置）⇒ 置第 29 位 + 起计时器、**不揭示全部、
    //        不写等待三格**（余下的格交给逐帧文本泵）；
    //      · **揭示支**（其余）⇒ 揭示该层**全部**格，再按两道前置决定要不要写消息等待三格 (1,0,0)。
    //   ④ 所有出口都清臂位。
    // ★ **打字机支本模型未实现（子单，残差待登记）**：本实现总是走揭示支，所以"武装 + 每字间隔 ≠ 0"
    //   在本模型会整段揭示并写三格 —— ⛔ 这不是合同，是已知缺口（`0x6e` 自己那条打字机路径是另一件
    //   事，已有守卫）。守卫 `spec_adv_text_bit27_set_points` ③ 特意把 0x74 设 0 以绕开这一支。
    // ★ 「已读」表未建模（残差 `RES-MSG-BLOCK-READ-HIT`）⇒ 命中的那一半由接缝提供（缺省无命中）；
    //   `message:ReadTextSkip` 非 0 是 spec 明写的必要条件。条件成立 ⇒ 置第 27 位（不是"永远不置"）。
    write_length(1);
    const bool armed = (msg_arm_ & 1) != 0 && (msg_arm_ & 0x10000) != 0;
    if (!armed) {
        msg_arm_ = 0;
        return;
    }
    const int slot = resolve_text_layer(current_text_slot_);
    if (slot >= 0 && slot < 10) {
        // ① 揭示游标回卷到块开头（两支都做，排在分叉之前）。
        const auto saved = msg_saved_cursor_ < 0 ? std::size_t{0}
                                                 : static_cast<std::size_t>(msg_saved_cursor_);
        reveal_text_layer_to(static_cast<std::size_t>(slot), saved);
    }
    // ② 读 `message:ReadTextSkip`：非 0 **且**「已读」命中 ⇒ 置门字第 27 位（"跳过"）。
    // ★ 与 `0x6e` / `0x196` 的追加前置支是同一判据、同一格；排在分叉之前且结果直接决定走哪一支
    //   （⛔ 不许把它挪进任一支）。「已读」表未建模（残差 `RES-MSG-BLOCK-READ-HIT`）⇒ 接缝缺省无命中。
    if (read_text_skip_requested()) state_flags_ |= (1u << 27);
    if (slot >= 0 && slot < 10) {
        // ③ 揭示该层全部格（引擎在这里自旋到"揭示完"）。本模型的 `0x305` 没有单列打字机分支
        //   （见残差；本批只补第 ② 步）。
        reveal_all_text_layer_cells(slot);
    }
    msg_wait_a_ = 1;
    msg_wait_b_ = 0;
    msg_wait_c_ = 0;
    msg_arm_ = 0;
    return;
}
}  // namespace core
