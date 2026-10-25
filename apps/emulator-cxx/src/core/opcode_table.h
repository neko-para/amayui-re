#pragma once

#include "core/bytes.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

struct OpcodeRow {
    std::uint32_t opcode = 0;
    std::uint32_t argc = 0;
    std::string name;
    bool has_handler = false;
};

class OpcodeTable {
public:
    static OpcodeTable load_from_file(const std::string& path);
    static OpcodeTable from_rows(std::vector<OpcodeRow> rows);

    const OpcodeRow* find(std::uint32_t opcode) const;
    std::size_t size() const { return rows_.size(); }

private:
    std::unordered_map<std::uint32_t, OpcodeRow> by_opcode_;
    std::vector<OpcodeRow> rows_;
};

}  // namespace core
