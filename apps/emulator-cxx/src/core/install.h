#pragma once

#include "core/alf.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace core {

struct Install {
    std::filesystem::path root;
    AlfIndex index;
};

// 真实入口：安装目录 + SYS4INI.BIN → ALF 索引。
Install open_install(const std::filesystem::path& install_dir);

// 按统一文件 id（TOC 下标）取文件：安装根松散文件优先，否则从归档切片。
Bytes read_install_file(const Install& install, std::uint32_t file_id);

}  // namespace core
