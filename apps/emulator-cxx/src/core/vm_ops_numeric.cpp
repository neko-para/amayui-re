// 数值 / 数组：算术 · 位 · 比较 · mov · 文本↔数；池数组的填充 / 拷贝 / 查找 / 标量数组；
// 外加标量键与容器转发（0x10C · 0xFE · 0x32F · 0x324）。
//
// ★ 本文件只有 handler 本体；分派仍在 vm.cpp 的 execute()，一个 opcode 一行。
// ★ 本文件的切分是**按职责**聚合，不严格等于规范章节边界（章节归属以 spec 正文为准）。
#include "core/vm.h"
#include "core/vm_internal.h"

namespace core {

void Machine::op_6c(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // fill-zero（CORE-OPS-ARRAY-FILL-ZERO）：目标走**取址原语**（`address_of_operand`）——
    // 引用族的落点是"它指向的那一格"，不是引用槽自己。⛔ 用 `address_of` 会把数据写进指针槽。
    const auto target = address_of_operand(ops[0]);
    const auto count = read_int(ops[1]);
    // 实现侧边界（`RES-ARRAY-FILL-ZERO-OOB` 的 impact）：count ≤ 0 不写；写满 count 格，但
    // **不许跨出目标池窗口**（跨窗就是静默写坏相邻族）⇒ 越界响亮失败，⛔ 不夹紧 count。
    check_array_write_range(target, count > 0 ? static_cast<std::uint32_t>(count) : 0u);
    const auto zero = encode_int(0);
    for (std::int32_t i = 0; i < count; ++i) store_raw(target + 4u * static_cast<std::uint32_t>(i), zero);
    return;
}

void Machine::op_2d8(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // set-array-to（CORE-OPS-ARRAY-SET-TO）：目标同样走取址原语。
    const auto target = address_of_operand(ops[0]);
    const auto value = encode_int(read_int(ops[1]));
    const auto count = read_int(ops[2]);
    // 实现侧边界（`RES-ARRAY-SET-TO-OOB`）：与 0x6c 同一条判据（⛔ 不截断成容量、不静默写相邻池）。
    check_array_write_range(target, count > 0 ? static_cast<std::uint32_t>(count) : 0u);
    for (std::int32_t i = 0; i < count; ++i) store_raw(target + 4u * static_cast<std::uint32_t>(i), value);
    return;
}

void Machine::op_61(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // lookup-array（CORE-OPS-ARRAY-LOOKUP）：落点必须是引用族；数组基址走取址原语。
    const auto index = read_int(ops[2]);
    const auto base = address_of_operand(ops[1]);
    if (!is_pointer_tag(ops[0].type)) {
        throw CoreError("lookup-array-destination-not-reference-tag:" + std::to_string(ops[0].type));
    }
    const auto width = (ops[0].type == 8 || ops[0].type == 14) ? 28u : 4u;
    write_pointer(ops[0], base + width * static_cast<std::uint32_t>(index));
    return;
}

void Machine::op_12c(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // lookup-array-2d（CORE-OPS-ARRAY-LOOKUP-2D）：读序 4 → 3 → 5 → 2；基址走取址原语。
    const auto row_width = read_int(ops[3]);
    const auto row = read_int(ops[2]);
    const auto column = read_int(ops[4]);
    const auto base = address_of_operand(ops[1]);
    if (!is_pointer_tag(ops[0].type)) {
        throw CoreError("lookup-array-2d-destination-not-reference-tag:" + std::to_string(ops[0].type));
    }
    const auto width = (ops[0].type == 8 || ops[0].type == 14) ? 28u : 4u;
    const auto index = as_i32(as_u32(row) * as_u32(row_width) + as_u32(column));
    write_pointer(ops[0], base + width * static_cast<std::uint32_t>(index));
    return;
}

void Machine::op_64(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // copy-local-array（CORE-OPS-ARRAY-COPY）：目标是**取址原语**给出的地址，块首 word 是 count。
    // 读序：目标地址 → 块编号 → 块首的 count → 逐格「读源 dword → ENC → 写目标」。
    // ★ **重叠闭式在本模型恒不可达**：源在帧自己的独立 `code` vector 里，目标在六个池之一
    //   （池地址窗口与 code vector 不重叠）⇒ 规范里"目标首 > 源首且区间重叠"那几段在这里没有站点。
    //   **合同仍成立**，只是本模型走不到；实现按"正向、逐格、先读后写"推进。
    // ★ count 是**有符号**量：spec 明写「个数 ≤ 0 时什么都不做」（`60-ops-array.md:118`）⇒
    //   ⛔ 不许按 u32 判（`0xFFFFFFFF` 在引擎里是 −1，不是"4G 个格"）。块编号越界 / `1 + count`
    //   个字越过代码区末尾属**未定义行为**（spec 不作断言）：本模型在那种情形选择
    //   **响亮失败 + 诊断**（`array-block-past-code`），⛔ 不静默、不夹紧、也不读半个块。
    const auto target = address_of_operand(ops[0]);
    const auto block = static_cast<std::int64_t>(read_int(ops[1]));
    const auto& code = frame_mut().code;
    const auto size = static_cast<std::int64_t>(code.size());
    const auto block_offset = block * 4;
    if (block < 0 || block_offset + 4 > size) {
        throw CoreError("array-block-past-code:block=" + std::to_string(block) + ":code-bytes=" +
                        std::to_string(size));
    }
    // 个数 = 块首第一个 32 位字，按**有符号**读出（有符号语义是合同的一部分，见上）。
    const auto count = read_i32(code, static_cast<std::size_t>(block_offset));
    if (count <= 0) return;  // 个数 ≤ 0 ⇒ 什么都不做（目标一格不写、不抛）
    if (static_cast<std::int64_t>(count) * 4 > size - block_offset - 4) {
        throw CoreError("array-block-past-code:block=" + std::to_string(block) + ":count=" +
                        std::to_string(count) + ":code-bytes=" + std::to_string(size));
    }
    // 实现侧边界（`RES-ARRAY-COPY-OOB` 的第二条守卫：目标区间越出目标池容量 ⇒ 显式失败）：
    // 与 0x6c / 0x2d8 共用同一条判据（⛔ 不夹紧 count、不静默写相邻池）。走到这里 count 已 > 0。
    check_array_write_range(target, static_cast<std::uint32_t>(count));
    for (std::int32_t i = 0; i < count; ++i) {
        const auto source = read_u32(code, static_cast<std::size_t>(block_offset + 4 + 4 * static_cast<std::int64_t>(i)));
        store_raw(target + 4u * static_cast<std::uint32_t>(i),
                  encode_int(static_cast<std::int32_t>(source)));
    }
    return;
}

void Machine::op_10c(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 按键写一格（`CORE-OPS-SCALAR-TABLE-10C`，`sub_4220B0`，argc 2）：
    // 第 2 个是**索引（动作号）**、第 1 个是**值（键号）**。读序 **2 → 1**（EA 0x4220d6 / 0x4220e1）：
    // 索引先读，值后读；索引读失败时值还没被读。
    // ① 值按**无符号**比较 > 31（含负值）⇒ 抛显示消息型异常、**不改任何状态**（EA 0x4220e6 / 0x4220ff）。
    //    文本逐字 = `.lst` 的 `.data:0052005C aSetkeymulti db 'SetKeyMultiの引数が不正です．',0`
    //    —— **有**全角句点、**没有**回车换行（同族里带 `\r\n` 的是 `SetTB` / `SetVolume` 那几条）。
    //    ★ spec `60-ops-scalar.md:155` 的引文少了那个全角句点（`SetKeyMultiの引数が不正です`）：
    //      这里是 spec 与 `.lst` 的矛盾，按 `.lst` 逐字实现、并报给 spec 侧改引文。
    // ② 用索引在「动作号 → 槽号」表里取一格（EA 0x4220eb）—— **索引完全不检查**：越界属 UB，不作语义断言。
    // ③ 把值写进「槽号 → 键号」表的该槽（EA 0x4220f3；−1 = 未绑定）。
    // ★ 本条**不碰**「键号 → 动作号」表（那是单键绑定 0x10b 维护的；EA 0x421c4b 那一侧）。
    const auto action = read_int(ops[1]);  // 索引（动作号），先读
    const auto value = read_int(ops[0]);   // 值（键号），后读
    if (static_cast<std::uint32_t>(value) > 31u) {
        throw DisplayMessageError("key-multi-range", "SetKeyMultiの引数が不正です．");
    }
    // ⛔ 不要给下标加夹取 / 范围检查：引擎那一格是无检查的数组索引（spec 明文：越界读属 UB）。
    const auto slot = action_slot_table_[static_cast<std::size_t>(action)];
    slot_key_table_[static_cast<std::size_t>(slot)] = value;
    return;
}

void Machine::op_fe(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto value = read_int(ops[0]); if (static_cast<std::uint32_t>(value) > 31u) throw CoreError("key-total-out-of-range"); key_total_ = value; return;
}

void Machine::op_107(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto index = read_int(ops[0]); const auto value = read_int(ops[1]); if (index >= 0 && index <= 0x1f) array_d551_[static_cast<std::size_t>(index)] = value; return;
}

void Machine::op_10b(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 标量表 32 格（第 2 个是索引、第 1 个是值；越界值静默跳过，见「引擎级状态」）。
    // ★ 这张表**同时就是**键绑定三张表里的「键号 → 动作号」：单键绑定那条指令写它（EA 0x421c4b），
    //   `0x10c` **不碰**它。⇒ ⛔ 不要再给 0x10c 建第二张键 → 动作表。
    const auto value = read_int(ops[0]); const auto index = read_int(ops[1]); if (index >= 0 && index <= 0x1f) array_d1383_[static_cast<std::size_t>(index)] = value; return;
}

void Machine::op_30a(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto value = read_int(ops[0]); const auto index = read_int(ops[1]); if (index < 0 || index > 7 || value < 0 || value > 0x1f) throw CoreError("scalar-array-range"); array_d1969_[static_cast<std::size_t>(index)] = value; return;
}

void Machine::op_32f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 容器按索引清一格
    const auto index = read_int(ops[0]);
    forward_container_[static_cast<std::uint32_t>(index)] = 0;
    return;
}

void Machine::op_324(const Instruction& instruction) {
    (void)instruction;
    // 转发无参复位：接收者是引擎内部对象，当前不建模其字段
    return;
}

void Machine::op_50(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // add：先读第 3 个、再读第 2 个
    const auto b = as_u32(read_int(ops[2]));
    const auto a = as_u32(read_int(ops[1]));
    write_int(ops[0], as_i32(a + b));
    return;
}

void Machine::op_51(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // sub：第 2 个 − 第 3 个；读取次序 3 → 2
    const auto b = as_u32(read_int(ops[2]));
    const auto a = as_u32(read_int(ops[1]));
    write_int(ops[0], as_i32(a - b));
    return;
}

void Machine::op_52(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // mul
    const auto b = as_u32(read_int(ops[2]));
    const auto a = as_u32(read_int(ops[1]));
    write_int(ops[0], as_i32(a * b));
    return;
}

void Machine::op_53(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // div，向零截断
    const auto divisor = read_int(ops[2]);
    if (divisor == 0) throw CoreError("divide-by-zero");
    write_int(ops[0], read_int(ops[1]) / divisor);
    return;
}

void Machine::op_54(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // mod，符号随被除数
    const auto divisor = read_int(ops[2]);
    if (divisor == 0) throw CoreError("mod-by-zero");
    write_int(ops[0], read_int(ops[1]) % divisor);
    return;
}

void Machine::op_2d5(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    write_float(ops[0], read_float(ops[1])); return;
}

void Machine::op_56(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto b = as_u32(read_int(ops[2])); const auto a = as_u32(read_int(ops[1])); write_int(ops[0], as_i32(a & b)); return;
}

void Machine::op_57(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto b = as_u32(read_int(ops[2])); const auto a = as_u32(read_int(ops[1])); write_int(ops[0], as_i32(a | b)); return;
}

void Machine::op_58(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto b = as_u32(read_int(ops[2])) & 31u; const auto a = read_int(ops[1]); write_int(ops[0], a >> b); return;
}

void Machine::op_59(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto b = as_u32(read_int(ops[2])) & 31u; const auto a = read_int(ops[1]); write_int(ops[0], as_i32(as_u32(a) << b)); return;
}

void Machine::op_5a(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto b = read_int(ops[2]); const auto a = read_int(ops[1]); write_int(ops[0], a == b ? 1 : 0); return;
}

void Machine::op_5b(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto b = read_int(ops[2]); const auto a = read_int(ops[1]); write_int(ops[0], a != b ? 1 : 0); return;
}

void Machine::op_5c(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto b = read_int(ops[2]); const auto a = read_int(ops[1]); write_int(ops[0], a < b ? 1 : 0); return;
}

void Machine::op_5d(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto b = read_int(ops[2]); const auto a = read_int(ops[1]); write_int(ops[0], a <= b ? 1 : 0); return;
}

void Machine::op_5e(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto b = read_int(ops[2]); const auto a = read_int(ops[1]); write_int(ops[0], a > b ? 1 : 0); return;
}

void Machine::op_5f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto b = read_int(ops[2]); const auto a = read_int(ops[1]); write_int(ops[0], a >= b ? 1 : 0); return;
}

void Machine::op_135(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 位号按**无符号**与 31 比较：0..31 才动作；越界（含负值）**不读、不写目标**，
    // 只把固定错误文本（含 CRLF）写进**错误通道**，然后**正常返回**（不抛、不停机）。
    // 锚：0x42F8DA `cmp edi,1Fh` + `ja` ⇒ 0x42F8E0 起是错误文本支（`CORE-OPS-BIT-SET-RESET`）；
    // 推进格数槽在判据**之前**就已写（0x42F8C1），所以"越界不改变状态"只对目标操作数成立。
    const auto bit = as_u32(read_int(ops[1]));
    if (bit > 31) {
        report_error("SetBitの引数が不正です．\r\n");
        return;
    }
    const auto value = read_int(ops[0]);
    write_int(ops[0], as_i32(as_u32(value) | (1u << bit)));
    return;
}

void Machine::op_136(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 与 `0x135` 同形，只差错误文本与"清位"（锚：0x42F94A `cmp edi,1Fh` + `ja`；
    // 推进格数槽在 0x42F931 先写）。
    const auto bit = as_u32(read_int(ops[1]));
    if (bit > 31) {
        report_error("RemBitの引数が不正です．\r\n");
        return;
    }
    const auto value = read_int(ops[0]);
    write_int(ops[0], as_i32(as_u32(value) & ~(1u << bit)));
    return;
}

void Machine::op_13f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 查位（CORE-OPS-BIT-TEST）：读序 **3（位号）→ 范围检查 → 2（源）→ 写 1**。
    // 位号按**无符号**与 31 比较；越界（含负值）⇒ **第 1 个保持原值**（不写 0）、源**也不读**，
    // 只把固定错误文本（含 CRLF）写进**错误通道**，然后正常返回（不抛、不停机）。
    // 与 0x135 / 0x136 同形，区别只是文本与"源 / 落点是两个操作数"。
    const auto bit = as_u32(read_int(ops[2]));
    if (bit > 31) {
        report_error("GetBitの引数が不正です．\r\n");
        return;
    }
    const auto value = as_u32(read_int(ops[1]));
    write_int(ops[0], static_cast<std::int32_t>((value >> bit) & 1u));
    return;
}

void Machine::op_55(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // mov（整型语义）
    write_int(ops[0], read_int(ops[1]));
    return;
}

void Machine::op_1b0(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 池拷贝（CORE-OPS-POOL-MEMCPY）：读序 n → 源址 → 目标址；复制的是**池里的编码位模式**。
    const auto count = read_int(ops[2]);
    const auto source = address_of_operand(ops[0]);
    const auto target = address_of_operand(ops[1]);
    for (std::int32_t i = 0; i < count; ++i) {
        const auto offset = 4u * static_cast<std::uint32_t>(i);
        store_raw(target + offset, read_raw(source + offset));  // 不解码、不再编码
    }
    return;
}

void Machine::op_191(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 浮点绝对值（CORE-OPS-FLOAT-ABS）：读浮点(op2) → |·| → 写浮点(op1)；−0.0 ⇒ +0.0。
    write_float(ops[0], std::abs(read_float(ops[1])));
    return;
}

void Machine::op_2d0(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 浮点加（CORE-OPS-SCALAR-FLOAT-ARITH）：读序 3 → 2。
    const auto b = read_float(ops[2]);
    const auto a = read_float(ops[1]);
    write_float(ops[0], a + b);
    return;
}

void Machine::op_2d1(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 浮点减：第 2 − 第 3（不是反的）；读序 2 → 3。
    const auto a = read_float(ops[1]);
    const auto b = read_float(ops[2]);
    write_float(ops[0], a - b);
    return;
}

void Machine::op_2d3(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 浮点除：第 2 ÷ 第 3；除数 0 **不检查**（按 IEEE 得 inf / nan，不抛）。
    const auto a = read_float(ops[1]);
    const auto b = read_float(ops[2]);
    write_float(ops[0], a / b);
    return;
}

void Machine::op_d0(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 时钟（CORE-OPS-CLOCK-D0）：把本迭代的宿主毫秒读数按整型写出（32 位、会回绕）。
    write_int(ops[0], as_i32(scene_clock_ms_));
    return;
}

void Machine::op_74(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 参数格写（CORE-OPS-PARAM-74-7F）：原样，不归一、不夹取。
    param_cell_ = read_int(ops[0]);
    return;
}

void Machine::op_7f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 参数格读：初值没有观察 ⇒ 没写过就**响亮失败**（⛔ 不伪造 0）。
    if (!param_cell_) throw CoreError("param-cell-uninitialized");
    write_int(ops[0], *param_cell_);
    return;
}

void Machine::op_12f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 排序置换（0x12F / `sub_42F560`；`CORE-OPS-ARRAY-SORT-12F`）：
    //   读序：A → B → C →（写 `A[0] ← 0`）→ N；**N 在每次判断循环条件时重读**（两处循环都是）。
    //   ① `A[0] ← 原始 0`（无条件，N = 0 也写；不是编码值）；
    //   ② N ≥ 2 时插入排序：键 = `dec(B[i]) + dec(C[i])`（32 位回绕、**有符号**比较、升序、相等不换位）；
    //   ③ 收尾 i = 0..N−1：`A[i] ← enc(A[i])`。
    // ★ **写序是合同**（重叠时由它唯一确定结果）：
    //   内层每次比较都**重读 A[j]**、都**重算右键 B[i] + C[i]** —— ⛔ 不许把右键缓存成 `key_j`：
    //   A 与 B / C 别名时，移位会改掉正在读的键，缓存会给出**不同的排列**。
    //   右移时（④）**重新读一次 A[j]** 再写 A[j+1]（②④ 之间没有写，两次读到的必是同一个值）。
    // ★ 不做越界检查（N 越界是 UB）；B / C 一个字节都不写。
    const std::uint32_t a = address_of_operand(ops[0]);
    const std::uint32_t b = address_of_operand(ops[1]);
    const std::uint32_t c = address_of_operand(ops[2]);
    store_raw(a, 0u);  // ① 无条件；发生在**第一次读 N 之前**
    // 键：两个 4 字节字各按整数池解码后求和，**32 位回绕**（用无符号加，避免有符号溢出的 UB）。
    const auto key_of = [&](std::uint32_t index) {
        return as_i32(as_u32(decode_int(read_raw(b + 4u * index))) +
                      as_u32(decode_int(read_raw(c + 4u * index))));
    };
    // ② 外层 i = 1 … N−1（每次判断条件都重读第 4 个操作数）。
    for (std::int32_t i = 1; i < read_int(ops[3]); ++i) {
        std::int32_t j = i - 1;
        while (j >= 0) {
            // ② 读 A[j]，用这个值取左键 B[A[j]] + C[A[j]]。
            const auto left_index = read_raw(a + 4u * static_cast<std::uint32_t>(j));
            const auto left = key_of(left_index);
            // ③ **重算**右键（⛔ 不是先算一张表 / 缓存一次）。
            const auto right = key_of(static_cast<std::uint32_t>(i));
            if (left <= right) break;  // 相等也落位（不交换）
            // ④ 重新读一次 A[j]，把它的原值写进 A[j+1]；j ← j − 1 并回到 ②。
            const auto move = read_raw(a + 4u * static_cast<std::uint32_t>(j));
            store_raw(a + 4u * static_cast<std::uint32_t>(j + 1), move);
            --j;
        }
        // ⑤ 内层结束后把 **i 的原始值**（不是键、不是编码值）写进 A[j+1]。
        store_raw(a + 4u * static_cast<std::uint32_t>(j + 1), static_cast<std::uint32_t>(i));
    }
    // ③ 收尾 i = 0..N−1：同样每次判断条件时重读 N。N = 0 ⇒ 不进循环，`A[0]` 保持原始 0。
    for (std::int32_t i = 0; i < read_int(ops[3]); ++i) {
        const std::uint32_t slot = a + 4u * static_cast<std::uint32_t>(i);
        store_raw(slot, encode_int(static_cast<std::int32_t>(read_raw(slot))));
    }
    return;
}

void Machine::op_60(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 随机（CORE-OPS-RANDOM-60）：读序 计数器 → 随机数 → 范围 → 落点。
    random_counter_ += 1;
    if (random_counter_ > 12) random_counter_ = 0;
    const auto bits = next_random();           // 非负（规范：被除数非负 ⇒ 余数非负）
    random_last_ = as_i32(bits);
    const auto range = read_int(ops[1]);
    random_range_ = range;
    if (range == 0) {
        // 范围 = 0：落点经**写整型**先写成 0，**然后**抛**显示消息型异常**（文本逐字，不可改写）；
        // 随机数已经取走一次，所以这次抽取**不回退**。⛔ 判据是"等于 0"（范围 −1 照常算余数）。
        write_int(ops[0], 0);
        throw DisplayMessageError("random-range-zero", "RANDOMの最大値が0になっています．");
    }
    write_int(ops[0], as_i32(bits) % range);   // 有符号余数（被除数非负）
    return;
}

void Machine::op_6d(const Instruction& instruction) {
    (void)instruction;
    // 池翻页（SPC-POOL-MEMFLIP）：把六个全局族的主 / 备选择**整体对调**（不搬数据、不重分配）。
    // 配置键在**执行期**再读一次（启动期那次只决定备组是否"已登记"）。
    if (config_.get_int("set:EnableMemFlip") == 0) {
        // 发行数据把该键显式置 0 ⇒ 本作的翻页指令恒走异常支。
        throw CoreError("display-message:memflip-unavailable");
    }
    global_flip_ ^= 1u;
    return;
}

void Machine::op_63(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // lea（CORE-OPS-POOL-LEA-63）：先取址(op2)，再校验 op1 是引用族，最后把**地址本身**写进引用槽。
    const auto address = address_of_operand(ops[1]);
    if (!is_pointer_tag(ops[0].type)) {
        throw CoreError("lea-destination-not-reference-tag:" + std::to_string(ops[0].type));
    }
    write_pointer(ops[0], address);  // 不编码
    return;
}

void Machine::op_1ce(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // 逐格揭示当前文本的启停（`CORE-OPS-TEXT-CELL-REVEAL-1CE`）：**不是**乱数启用。
    // 非 0 ⇒ 置门字第 30 位（逐格揭示活动中）+ 当前格号归 0 + 节拍器相位重置（相位起点 ← 本迭代的
    // 宿主毫秒时钟；**不是播种**）。0 ⇒ 只在第 30 位已置时动作：「重绘待处理」位（第 20 位）未置 ⇒
    // 先请求一次释放排版资源，再清第 30 位；**格号与相位都不复位**。
    // 操作数的值进「开关格」：唯一写点、全引擎无读者（也无取址）⇒ 对可观察行为没有后果（两条支都写）。
    const auto value = read_int(ops[0]);
    reveal_switch_ = value;
    if (value != 0) {
        state_flags_ |= (1u << 30);
        reveal_cursor_ = 0;
        reveal_tick_start_ms_ = scene_clock_ms_;
        reveal_tick_counter_ = 1;
    } else if ((state_flags_ & (1u << 30)) != 0) {
        // 「重绘待处理」（第 20 位）已置 ⇒ 这份排版马上要重画，别释放它（跳过释放、仍清活动位）。
        if ((state_flags_ & 0x100000u) == 0) ++text_layout_release_requests_;
        state_flags_ &= ~(1u << 30);
    }
    return;
}

}  // namespace core


