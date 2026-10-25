#include "core/install.h"

#include <fstream>
#include <vector>

namespace core {
namespace {
Bytes read_all(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw CoreError("open-failed:" + path.string());
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size < 0) throw CoreError("tell-failed:" + path.string());
    input.seekg(0, std::ios::beg);
    Bytes bytes(static_cast<std::size_t>(size));
    if (!bytes.empty()) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!input) throw CoreError("read-failed:" + path.string());
    }
    return bytes;
}
}  // namespace

Install open_install(const std::filesystem::path& install_dir) {
    Install install;
    install.root = install_dir;
    const auto index_path = install_dir / "SYS4INI.BIN";
    const Bytes bytes = read_all(index_path);
    install.index = parse_alf_index(bytes);
    return install;
}

Bytes read_install_file(const Install& install, std::uint32_t file_id) {
    if (file_id >= install.index.files.size()) {
        throw CoreError("file-id-out-of-range:" + std::to_string(file_id) +
                        "/" + std::to_string(install.index.files.size()));
    }
    const AlfFile& file = install.index.files[file_id];

    // 松散文件优先（CORE-FS 的临时引导口径）。
    const auto loose = install.root / file.name;
    if (std::filesystem::exists(loose)) {
        return read_all(loose);
    }

    if (file.archive_index >= install.index.archives.size()) {
        throw CoreError("archive-index-out-of-range:" + std::to_string(file.archive_index));
    }
    const auto archive_path = install.root / install.index.archives[file.archive_index];
    std::ifstream archive(archive_path, std::ios::binary);
    if (!archive) throw CoreError("archive-open-failed:" + archive_path.string());
    archive.seekg(file.offset, std::ios::beg);
    if (!archive) throw CoreError("archive-seek-failed:" + archive_path.string());
    Bytes bytes(file.length);
    if (!bytes.empty()) {
        archive.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!archive) throw CoreError("archive-read-failed:" + archive_path.string());
    }
    return bytes;
}

}  // namespace core
