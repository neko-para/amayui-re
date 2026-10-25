#include "core/opcode_table.h"

#include <nlohmann/json.hpp>

#include <fstream>

namespace core {

OpcodeTable OpcodeTable::from_rows(std::vector<OpcodeRow> rows) {
    OpcodeTable table;
    table.rows_ = std::move(rows);
    for (const auto& row : table.rows_) {
        table.by_opcode_[row.opcode] = row;
    }
    return table;
}

OpcodeTable OpcodeTable::load_from_file(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw CoreError("opcode-table-open-failed:" + path);

    const auto json = nlohmann::json::parse(input);
    if (!json.is_array()) throw CoreError("opcode-table-not-array:" + path);

    std::vector<OpcodeRow> rows;
    rows.reserve(json.size());
    for (const auto& item : json) {
        OpcodeRow row;
        row.opcode = item.at("opcode").get<std::uint32_t>();
        row.argc = item.at("argc").get<std::uint32_t>();
        row.name = item.contains("name") ? item["name"].get<std::string>() : std::string{};
        row.has_handler = item.value("hasHandler", false);
        rows.push_back(std::move(row));
    }
    return from_rows(std::move(rows));
}

const OpcodeRow* OpcodeTable::find(std::uint32_t opcode) const {
    const auto it = by_opcode_.find(opcode);
    return it == by_opcode_.end() ? nullptr : &it->second;
}

}  // namespace core
