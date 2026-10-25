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
    // fill-zero：按取址写 count 个 ENC(0)
    const auto target = address_of(ops[0]);
    const auto count = read_int(ops[1]);
    const auto zero = encode_int(0);
    for (std::int32_t i = 0; i < count; ++i) store_raw(target + 4u * static_cast<std::uint32_t>(i), zero);
    return;
}

void Machine::op_2d8(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // set-array-to
    const auto target = address_of(ops[0]);
    const auto value = encode_int(read_int(ops[1]));
    const auto count = read_int(ops[2]);
    for (std::int32_t i = 0; i < count; ++i) store_raw(target + 4u * static_cast<std::uint32_t>(i), value);
    return;
}

void Machine::op_61(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // lookup-array
    const auto index = read_int(ops[2]);
    const auto base = address_of(ops[1]);
    const auto width = (ops[0].type == 8 || ops[0].type == 14) ? 28u : 4u;
    write_pointer(ops[0], base + width * static_cast<std::uint32_t>(index));
    return;
}

void Machine::op_12c(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // lookup-array-2d：读序 4 → 3 → 5 → 2
    const auto row_width = read_int(ops[3]);
    const auto row = read_int(ops[2]);
    const auto column = read_int(ops[4]);
    const auto base = address_of(ops[1]);
    const auto width = (ops[0].type == 8 || ops[0].type == 14) ? 28u : 4u;
    const auto index = as_i32(as_u32(row) * as_u32(row_width) + as_u32(column));
    write_pointer(ops[0], base + width * static_cast<std::uint32_t>(index));
    return;
}

void Machine::op_64(const Instruction& instruction) {
    const Operand* ops = instruction.operands.data();
    // copy-local-array：块首 word 是 count，后接源 dword
    const auto target = address_of(ops[0]);
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

}  // namespace core
