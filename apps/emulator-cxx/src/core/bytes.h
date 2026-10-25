// C++ core 的字节原语。只做小端读取与定长字符串，不含任何 Node 依赖。
#pragma once

#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace core {

using Bytes = std::vector<std::uint8_t>;
using ByteSpan = std::span<const std::uint8_t>;

class CoreError : public std::runtime_error {
public:
    explicit CoreError(const std::string& what) : std::runtime_error(what) {}
};

inline std::uint32_t read_u32(ByteSpan bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        throw CoreError("read-u32-past-eof:" + std::to_string(offset));
    }
    return static_cast<std::uint32_t>(bytes[offset]) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

inline std::int32_t read_i32(ByteSpan bytes, std::size_t offset) {
    return static_cast<std::int32_t>(read_u32(bytes, offset));
}

inline std::string read_field(ByteSpan bytes, std::size_t offset, std::size_t size) {
    if (offset + size > bytes.size()) {
        throw CoreError("read-field-past-eof:" + std::to_string(offset));
    }
    std::size_t end = offset;
    while (end < offset + size && bytes[end] != 0) ++end;
    return std::string(reinterpret_cast<const char*>(bytes.data() + offset), end - offset);
}

inline std::string read_cstring(ByteSpan bytes, std::size_t& cursor) {
    std::size_t end = cursor;
    while (end < bytes.size() && bytes[end] != 0) ++end;
    if (end >= bytes.size()) throw CoreError("cstring-unterminated:" + std::to_string(cursor));
    std::string value(reinterpret_cast<const char*>(bytes.data() + cursor), end - cursor);
    cursor = end + 1;
    return value;
}

}  // namespace core
