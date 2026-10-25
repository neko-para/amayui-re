#include "core/alf.h"

#include "core/lzss.h"

#include <algorithm>

namespace core {
namespace {
constexpr std::size_t kHead = 268;
constexpr std::size_t kIcHead = 300;
constexpr std::size_t kPackInfo = 12;
constexpr std::size_t kArchiveEntry = 256;
constexpr std::size_t kNameLength = 64;
constexpr std::size_t kFileRecord = 80;
constexpr std::size_t kZoneOffset = 264;

bool supported_signature(const std::string& s) {
    if (s.size() != 4) return false;
    if (s[0] != 'S' || (s[1] != '3' && s[1] != '4')) return false;
    return (s[2] == 'I' || s[2] == 'A' || s[2] == 'C') &&
           (s[3] == 'C' || s[3] == 'I');
}

std::string read_title(ByteSpan bytes) {
    std::size_t end = 8;
    while (end < kHead && bytes[end] != 0) ++end;
    if (end >= kHead) throw CoreError("alf-title-unterminated");
    return std::string(reinterpret_cast<const char*>(bytes.data() + 8), end - 8);
}

std::size_t read_toc_i32(ByteSpan toc, std::size_t& at) {
    if (at + 4 > toc.size()) throw CoreError("alf-tail-truncated:" + std::to_string(at));
    const auto value = read_i32(toc, at);
    at += 4;
    return value < 0 ? static_cast<std::size_t>(-1) : static_cast<std::size_t>(value);
}
}  // namespace

AlfIndex parse_alf_index(ByteSpan bytes) {
    if (bytes.size() < 4) throw CoreError("alf-too-small");
    AlfIndex index;
    index.signature.assign(reinterpret_cast<const char*>(bytes.data()), 4);
    if (!supported_signature(index.signature)) {
        throw CoreError("alf-unsupported-signature:" + index.signature +
                        "（本实现只覆盖 S3/S4 族；S5 缺样本）");
    }
    if (bytes.size() < kHead) throw CoreError("alf-header-past-eof:" + std::to_string(bytes.size()));
    index.title = read_title(bytes);

    const bool is_ic = index.signature.ends_with("IC");
    index.zone = is_ic ? -1 : static_cast<std::int32_t>(read_u32(bytes, kZoneOffset));

    const bool raw_toc = index.signature.ends_with("AI");
    index.section_pos = raw_toc ? kHead : (index.signature.ends_with("AC") ? kHead : kIcHead);

    Bytes toc;
    if (raw_toc) {
        if (index.section_pos > bytes.size()) throw CoreError("alf-section-past-eof");
        toc.assign(bytes.begin() + index.section_pos, bytes.end());
        index.original_length = static_cast<std::uint32_t>(toc.size());
        index.packed_length = static_cast<std::uint32_t>(toc.size());
    } else {
        if (index.section_pos + kPackInfo > bytes.size()) throw CoreError("alf-section-past-eof");
        index.original_length = read_u32(bytes, index.section_pos);
        const auto original2 = read_u32(bytes, index.section_pos + 4);
        index.packed_length = read_u32(bytes, index.section_pos + 8);
        if (index.original_length != original2) {
            throw CoreError("alf-section-length-mismatch:" + std::to_string(index.original_length) +
                            "/" + std::to_string(original2));
        }
        if (index.section_pos + kPackInfo + index.packed_length > bytes.size()) {
            throw CoreError("alf-packed-past-eof:" + std::to_string(index.packed_length));
        }
        const ByteSpan packed = bytes.subspan(
            index.section_pos + kPackInfo, index.packed_length);
        toc = decompress_lzss(packed, index.original_length);
    }

    std::size_t at = 0;
    const auto take = [&](std::size_t n) {
        if (at + n > toc.size()) {
            throw CoreError("alf-toc-truncated:" + std::to_string(at) + "+" + std::to_string(n) +
                            ">" + std::to_string(toc.size()));
        }
        const auto value = at;
        at += n;
        return value;
    };

    const auto archive_count = read_u32(toc, take(4));
    index.archives.reserve(archive_count);
    for (std::uint32_t i = 0; i < archive_count; ++i) {
        const auto off = take(kArchiveEntry);
        index.archives.push_back(read_field(toc, off, kArchiveEntry));
    }

    const auto file_count = read_u32(toc, take(4));
    index.files.reserve(file_count);
    for (std::uint32_t i = 0; i < file_count; ++i) {
        const auto off = take(kFileRecord);
        index.files.push_back(AlfFile{
            read_field(toc, off, kNameLength),
            read_u32(toc, off + kNameLength),
            read_u32(toc, off + kNameLength + 4),
            read_u32(toc, off + kNameLength + 8),
            read_u32(toc, off + kNameLength + 12),
        });
    }

    // 尾部：发行默认配置。两个计数数组不属配置，只跳过；任何越界都响亮失败。
    if (at < toc.size()) {
        const auto tail_take = [&](std::size_t n) {
            if (at + n > toc.size()) throw CoreError("alf-tail-truncated:" + std::to_string(at));
            const auto off = at;
            at += n;
            return off;
        };
        const auto skip_counted = [&]() {
            const auto n = read_toc_i32(toc, at);
            if (n != static_cast<std::size_t>(-1) && n > 0) tail_take(n * 4);
        };

        const auto len1 = read_toc_i32(toc, at);
        if (len1 != static_cast<std::size_t>(-1) && len1 > 0) {
            skip_counted();
            skip_counted();
        }
        const auto len2 = read_toc_i32(toc, at);
        if (len2 != static_cast<std::size_t>(-1) && len2 > 0) {
            const auto n = read_toc_i32(toc, at);
            if (n == static_cast<std::size_t>(-1)) throw CoreError("alf-tail-truncated");
            for (std::size_t i = 0; i < n; ++i) {
                ConfigDefault entry;
                entry.key = read_cstring(toc, at);
                entry.value = read_cstring(toc, at);
                index.config_defaults.push_back(std::move(entry));
            }
        }
    }

    return index;
}

}  // namespace core
