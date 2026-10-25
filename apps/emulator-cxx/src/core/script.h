#pragma once

#include "core/bytes.h"
#include "core/opcode_table.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace core {

struct ScriptHeader {
    std::string magic;
    std::string version;
    std::array<std::uint32_t, 6> counts{};
    // 三组 (长度, body 内 32 位字偏移)；无子头时全 0。
    std::array<std::pair<std::uint32_t, std::uint32_t>, 3> tables{};
    std::uint32_t subheader_length = 0;
    std::size_t header_size = 32;
    std::size_t body_offset = 32;
};

struct Operand {
    std::uint32_t type = 0;
    std::uint32_t raw = 0;
};

struct Instruction {
    std::size_t offset = 0;
    std::uint32_t opcode = 0;
    std::vector<Operand> operands;
};

ScriptHeader parse_script_header(ByteSpan bytes);

// 从 body 起点静态切分；未在表内 / 越界即停止并给出原因。
std::vector<Instruction> decode_instructions(
    ByteSpan body,
    const OpcodeTable& table,
    std::size_t max_instructions,
    std::string* error);

}  // namespace core
