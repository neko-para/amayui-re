#include "core/script.h"

#include <algorithm>

namespace core {
namespace {
// 无符号逐字节比较前 4 字节，遇 NUL 或满 4 字节即停；只取符号。
int compare_version(ByteSpan bytes, std::size_t offset, const char* expected) {
    for (std::size_t i = 0; i < 4; ++i) {
        const std::uint8_t a = bytes[offset + i];
        const std::uint8_t b = static_cast<std::uint8_t>(expected[i]);
        if (a != b) return a < b ? -1 : 1;
        if (a == 0) return 0;
    }
    return 0;
}
}  // namespace

ScriptHeader parse_script_header(ByteSpan bytes) {
    if (bytes.size() < 32) throw CoreError("script-too-small:" + std::to_string(bytes.size()));

    ScriptHeader header;
    header.magic.assign(reinterpret_cast<const char*>(bytes.data()), 4);
    if (header.magic != "SYS4" && header.magic != "SYS3") {
        throw CoreError("script-bad-magic:" + header.magic);
    }
    header.version.assign(reinterpret_cast<const char*>(bytes.data() + 4), 4);

    for (std::size_t i = 0; i < 6; ++i) {
        header.counts[i] = read_u32(bytes, 8 + i * 4);
    }

    if (compare_version(bytes, 4, "080 ") >= 0) {
        header.subheader_length = read_u32(bytes, 32);
        if (header.subheader_length < 4) {
            throw CoreError("script-bad-subheader-length:" + std::to_string(header.subheader_length));
        }
        header.header_size = 32 + header.subheader_length;
        if (header.header_size > bytes.size()) {
            throw CoreError("script-subheader-past-eof:" + std::to_string(header.header_size));
        }
        // 固定 6 个 32 位整数；子头短于 28 时缺的保持 0，长于 28 时多出的不解释。
        const std::size_t available = header.subheader_length - 4;
        for (std::size_t i = 0; i < 3; ++i) {
            const std::size_t pair_offset = 32 + 4 + i * 8;
            if (pair_offset + 8 <= 32 + header.subheader_length && (i * 8 + 8) <= available) {
                header.tables[i] = {read_u32(bytes, pair_offset), read_u32(bytes, pair_offset + 4)};
            }
        }
    } else {
        header.subheader_length = 0;
        header.header_size = 32;
    }

    header.body_offset = header.header_size;
    return header;
}

std::vector<Instruction> decode_instructions(
    ByteSpan body,
    const OpcodeTable& table,
    std::size_t max_instructions,
    std::string* error) {
    std::vector<Instruction> instructions;
    std::size_t at = 0;

    while (at < body.size() && instructions.size() < max_instructions) {
        if (at + 4 > body.size()) {
            if (error) *error = "opcode-past-eof@" + std::to_string(at);
            break;
        }
        const auto opcode = read_u32(body, at);
        if (opcode > 0x3FF) {
            if (error) *error = "opcode-out-of-domain@" + std::to_string(at) + ":" + std::to_string(opcode);
            break;
        }
        const OpcodeRow* row = table.find(opcode);
        if (row == nullptr || !row->has_handler) {
            if (error) *error = "opcode-without-handler@" + std::to_string(at) + ":" + std::to_string(opcode);
            break;
        }

        const std::size_t size = 4 + static_cast<std::size_t>(row->argc) * 8;
        if (at + size > body.size()) {
            if (error) *error = "instruction-past-eof@" + std::to_string(at) + ":" + std::to_string(size);
            break;
        }

        Instruction instruction;
        instruction.offset = at;
        instruction.opcode = opcode;
        instruction.operands.reserve(row->argc);
        for (std::uint32_t i = 0; i < row->argc; ++i) {
            instruction.operands.push_back(
                Operand{read_u32(body, at + 4 + i * 8), read_u32(body, at + 4 + i * 8 + 4)});
        }
        instructions.push_back(std::move(instruction));
        at += size;
    }

    return instructions;
}

}  // namespace core
