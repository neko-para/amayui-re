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
    const auto target = address_of_operand(ops[0]);
    const auto block = read_int(ops[1]);
    const auto* code = frame_mut().code.data();
    const auto size = frame_mut().code.size();
    const std::size_t block_offset = static_cast<std::size_t>(block) * 4;
    if (block_offset + 4 > size) throw CoreError("array-block-past-code");
    const auto count = read_u32(frame_mut().code, block_offset);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto source = read_u32(frame_mut().code, block_offset + 4 + i * 4);
        store_raw(target + 4u * i, encode_int(static_cast<std::int32_t>(source)));
    }
    (void)code;
    return;
}

void Machine::op_10c(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto value = read_int(ops[0]); const auto slot = read_int(ops[1]); if (value > 0x1f) throw CoreError("key-multi-range"); indirect_[slot] = value; return;
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
    const auto bit = as_u32(read_int(ops[1])); if (bit > 31) return; const auto value = read_int(ops[0]); write_int(ops[0], as_i32(as_u32(value) | (1u << bit))); return;
}

void Machine::op_136(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto bit = as_u32(read_int(ops[1])); if (bit > 31) return; const auto value = read_int(ops[0]); write_int(ops[0], as_i32(as_u32(value) & ~(1u << bit))); return;
}

void Machine::op_13f(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    const auto bit = as_u32(read_int(ops[2])); if (bit > 31) return; const auto value = as_u32(read_int(ops[1])); write_int(ops[0], static_cast<std::int32_t>((value >> bit) & 1u)); return;
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
    // 排序置换（0x12F / `sub_42F560`）：
    //   op1 / op2 / op3 = **三个数组的基址**（A 被写、B / C 只读），op4 = 元素个数 N。
    //   ① `A[0] := 0`（**无条件**，N = 0 也写；存的是**原始下标**，不是编码值）；
    //   ② N ≥ 2 时对 j = 1..N−1 做插入排序，键 = `dec(B[i]) + dec(C[i])`（i = A 里的下标；
    //      32 位回绕、**有符号**比较、升序），落位时写 `A[k+1] := j`；
    //   ③ 收尾 i = 0..N−1：`A[i] := enc(A[i])`。
    // ★ 三条引擎口径：op4 **每轮重求值**（不是缓存的值）；写 A 前**不做长度检查**（N 超长时静默越界，
    //   这是引擎行为，⛔ 不要替它加检查）；B / C 一个字节都不写。
    const std::uint32_t a = address_of_operand(ops[0]);
    const std::uint32_t b = address_of_operand(ops[1]);
    const std::uint32_t c = address_of_operand(ops[2]);
    store_raw(a, 0u);
    const auto key_of = [&](std::uint32_t index) {
        return static_cast<std::int32_t>(decode_int(read_raw(b + 4u * index)) +
                                         decode_int(read_raw(c + 4u * index)));
    };
    for (std::int32_t j = 1; j < read_int(ops[3]); ++j) {
        const auto key_j = key_of(static_cast<std::uint32_t>(j));
        std::int32_t k = j - 1;
        while (k >= 0) {
            const auto index_k = read_raw(a + 4u * static_cast<std::uint32_t>(k));
            if (key_of(index_k) <= key_j) break;  // 相等也落位（不交换）
            store_raw(a + 4u * static_cast<std::uint32_t>(k + 1), index_k);
            --k;
        }
        store_raw(a + 4u * static_cast<std::uint32_t>(k + 1), static_cast<std::uint32_t>(j));
    }
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
        write_int(ops[0], 0);                  // 先写 0，再抛
        throw CoreError("display-message:random-range-zero");
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
    // 乱数启用（CORE-OPS-RNG-ENABLE）：开关值只读、不归一、不夹取。
    const auto value = read_int(ops[0]);
    if (value != 0) {
        rng_switch_ = value;                 // 开关格 ← 第 1 个操作数（仅非 0 支写）
        state_flags_ |= 1u << 30;            // 门字第 30 位（服务分支不清位 ⇒ 启用后门恒关）
        rng_enabled_ = 1;
        rng_seed_ = scene_clock_ms_;         // 播种源 = 本迭代的宿主毫秒时钟
    } else if ((state_flags_ & (1u << 30)) != 0) {
        // 停用支：投递事件的条件未取证 ⇒ 不投递；「已启用」清 0、「种子」保留（规范不规定）。
        state_flags_ &= ~(1u << 30);
        rng_enabled_ = 0;
    }
    return;
}

}  // namespace core


