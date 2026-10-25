#pragma once

#include "core/bytes.h"

#include <cstdint>
#include <string>
#include <vector>

namespace core {

struct AlfFile {
    std::string name;
    std::uint32_t archive_index = 0;
    std::uint32_t file_index = 0;
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

struct ConfigDefault {
    std::string key;
    std::string value;
};

struct AlfIndex {
    std::string signature;
    std::string title;
    std::int32_t zone = -1;
    std::uint32_t section_pos = 0;
    std::uint32_t original_length = 0;
    std::uint32_t packed_length = 0;
    std::vector<std::string> archives;
    std::vector<AlfFile> files;
    std::vector<ConfigDefault> config_defaults;
};

// 吃索引文件字节，产出归档列表 + 文件表 + 尾部发行默认配置；不读载荷。
AlfIndex parse_alf_index(ByteSpan bytes);

}  // namespace core
