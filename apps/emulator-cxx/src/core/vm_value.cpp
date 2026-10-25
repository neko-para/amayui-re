// 值与池的原语（40-pools / 15-frame）：操作数读写、ENC/DEC、地址解析、裸 dword 访问、文本编解码。
//
// ★ 这些是**机器细节**，不承载引擎语义；语义在各 `vm_ops_*.cpp` 的 handler 里。
//   读序（先读哪个操作数）属于 handler，不属于这里 —— 这里只做"读到什么类型、写到哪里"。
#include "core/vm.h"
#include "core/vm_internal.h"

namespace core {

std::uint32_t Machine::encode_int(std::int32_t value) const {
    return rol32(ror32(as_u32(value), 7) ^ key_, 21);
}

std::int32_t Machine::decode_int(std::uint32_t bits) const {
    return as_i32(ror32(rol32(bits, 11) ^ key_, 25));
}

std::uint32_t Machine::script_int_key_index(const Operand& operand) {
    if (operand.type == 3) return operand.raw;
    if (operand.type == 6 || operand.type == 12) {
        const auto address = read_pointer(operand);
        const auto base = 0x10000000u;
        if (address < base || address >= base + 0x04000000u || (address - base) % 4u != 0) {
            throw CoreError("config-key-address-outside-global-int-pool");
        }
        return (address - base) / 4u;
    }
    throw CoreError("config-key-unsupported-tag:" + std::to_string(operand.type));
}

std::uint32_t Machine::pool_base(std::uint32_t tag) const {
    if (tag >= 3 && tag <= 8) return 0x10000000u + (tag - 3) * 0x04000000u;
    if (tag >= 9 && tag <= 14) {
        return 0x40000000u + static_cast<std::uint32_t>(current_frame_) * 0x00800000u +
               (tag - 9) * 0x00100000u;
    }
    throw CoreError("pool-base-unsupported-tag:" + std::to_string(tag));
}

std::uint32_t Machine::address_of(const Operand& operand) const {
    const auto tag = operand.type;
    if (tag < 3 || tag > 14) throw CoreError("address-of-unsupported-tag:" + std::to_string(tag));
    const std::uint32_t width = (tag == 5 || tag == 11) ? 28u : 4u;
    return pool_base(tag) + width * operand.raw;
}

bool Machine::resolve_address(std::uint32_t address, std::uint32_t& tag, std::uint32_t& index) const {
    for (std::uint32_t candidate = 3; candidate <= 8; ++candidate) {
        const std::uint32_t base = pool_base(candidate);
        const std::uint32_t width = (candidate == 5) ? 28u : 4u;
        if (address >= base && address < base + 0x04000000u) {
            if ((address - base) % width != 0) return false;
            tag = candidate;
            index = (address - base) / width;
            return true;
        }
    }
    for (std::uint32_t candidate = 9; candidate <= 14; ++candidate) {
        const std::uint32_t base = pool_base(candidate);
        const std::uint32_t width = (candidate == 11) ? 28u : 4u;
        if (address >= base && address < base + 0x00100000u) {
            if ((address - base) % width != 0) return false;
            tag = candidate;
            index = (address - base) / width;
            return true;
        }
    }
    return false;
}

std::uint32_t Machine::read_pointer(const Operand& operand) const {
    const auto slot = address_of(operand);
    const auto it = ref_slots_.find(slot);
    return it == ref_slots_.end() ? 0u : it->second;
}

void Machine::write_pointer(const Operand& operand, std::uint32_t address) {
    ref_slots_[address_of(operand)] = address;
}

std::uint32_t Machine::read_raw(std::uint32_t address) {
    std::uint32_t tag = 0;
    std::uint32_t index = 0;
    if (!resolve_address(address, tag, index)) throw CoreError("read-raw-unmapped-address");
    switch (tag) {
        case 3: { const auto it = global_int_.find(index); return it == global_int_.end() ? encode_int(0) : it->second; }
        case 9: { const auto it = frame_mut().local_int.find(index); return it == frame_mut().local_int.end() ? encode_int(0) : it->second; }
        case 4: { const auto it = global_float_.find(index); return it == global_float_.end() ? 0u : std::bit_cast<std::uint32_t>(it->second); }
        case 10: { const auto it = frame_mut().local_float.find(index); return it == frame_mut().local_float.end() ? 0u : std::bit_cast<std::uint32_t>(it->second); }
        case 6: case 7: case 12: case 13: { const auto it = ref_slots_.find(address); return it == ref_slots_.end() ? 0u : it->second; }
        default: throw CoreError("read-raw-unsupported-tag");
    }
}

void Machine::store_raw(std::uint32_t address, std::uint32_t bits) {
    std::uint32_t tag = 0;
    std::uint32_t index = 0;
    if (!resolve_address(address, tag, index)) throw CoreError("store-raw-unmapped-address");
    switch (tag) {
        case 3: global_int_[index] = bits; return;
        case 9: frame_mut().local_int[index] = bits; return;
        case 4: global_float_[index] = std::bit_cast<float>(bits); return;
        case 10: frame_mut().local_float[index] = std::bit_cast<float>(bits); return;
        case 6: case 7: case 12: case 13: ref_slots_[address] = bits; return;
        default: throw CoreError("store-raw-unsupported-tag");
    }
}

std::int32_t Machine::read_int_at(std::uint32_t address) {
    std::uint32_t tag = 0;
    std::uint32_t index = 0;
    if (!resolve_address(address, tag, index)) throw CoreError("read-int-unmapped-address");
    switch (tag) {
        case 3: { const auto it = global_int_.find(index); return it == global_int_.end() ? 0 : decode_int(it->second); }
        case 9: { const auto it = frame_mut().local_int.find(index); return it == frame_mut().local_int.end() ? 0 : decode_int(it->second); }
        case 4: { const auto it = global_float_.find(index); return it == global_float_.end() ? 0 : static_cast<std::int32_t>(it->second); }
        case 10: { const auto it = frame_mut().local_float.find(index); return it == frame_mut().local_float.end() ? 0 : static_cast<std::int32_t>(it->second); }
        default: throw CoreError("read-int-at-unsupported-tag");
    }
}

double Machine::read_float_at(std::uint32_t address) {
    std::uint32_t tag = 0;
    std::uint32_t index = 0;
    if (!resolve_address(address, tag, index)) throw CoreError("read-float-unmapped-address");
    switch (tag) {
        case 3: { const auto it = global_int_.find(index); return static_cast<double>(decode_int(it == global_int_.end() ? encode_int(0) : it->second)); }
        case 9: { const auto it = frame_mut().local_int.find(index); return static_cast<double>(decode_int(it == frame_mut().local_int.end() ? encode_int(0) : it->second)); }
        case 4: { const auto it = global_float_.find(index); return it == global_float_.end() ? 0.0 : it->second; }
        case 10: { const auto it = frame_mut().local_float.find(index); return it == frame_mut().local_float.end() ? 0.0 : it->second; }
        default: throw CoreError("read-float-at-unsupported-tag");
    }
}

void Machine::write_int_at(std::uint32_t address, std::int32_t value) {
    std::uint32_t tag = 0;
    std::uint32_t index = 0;
    if (!resolve_address(address, tag, index)) throw CoreError("write-int-unmapped-address");
    switch (tag) {
        case 3: global_int_[index] = encode_int(value); return;
        case 9: frame_mut().local_int[index] = encode_int(value); return;
        case 4: global_float_[index] = static_cast<float>(value); return;
        case 10: frame_mut().local_float[index] = static_cast<float>(value); return;
        default: throw CoreError("write-int-at-unsupported-tag");
    }
}

void Machine::write_float_at(std::uint32_t address, double value) {
    std::uint32_t tag = 0;
    std::uint32_t index = 0;
    if (!resolve_address(address, tag, index)) throw CoreError("write-float-unmapped-address");
    switch (tag) {
        case 3: global_int_[index] = encode_int(static_cast<std::int32_t>(value)); return;
        case 9: frame_mut().local_int[index] = encode_int(static_cast<std::int32_t>(value)); return;
        case 4: global_float_[index] = static_cast<float>(value); return;
        case 10: frame_mut().local_float[index] = static_cast<float>(value); return;
        default: throw CoreError("write-float-at-unsupported-tag");
    }
}

std::int32_t Machine::read_int(const Operand& operand) {
    switch (operand.type) {
        case 0:
            return as_i32(operand.raw);
        case 1:
            return static_cast<std::int32_t>(std::bit_cast<float>(operand.raw));
        case 3: {
            const auto it = global_int_.find(operand.raw);
            return it == global_int_.end() ? decode_int(encode_int(0)) : decode_int(it->second);
        }
        case 4: {
            const auto it = global_float_.find(operand.raw);
            return it == global_float_.end() ? 0 : static_cast<std::int32_t>(it->second);
        }
        case 5: {
            const auto it = global_string_.find(operand.raw);
            return it == global_string_.end() ? 0 : parse_int_text(it->second);
        }
        case 6: case 7: case 12: case 13: {
            const auto address = read_pointer(operand);
            return address == 0 ? 0 : read_int_at(address);
        }
        case 9: {
            const auto it = frame_mut().local_int.find(operand.raw);
            return it == frame_mut().local_int.end() ? 0 : decode_int(it->second);
        }
        case 10: {
            const auto it = frame_mut().local_float.find(operand.raw);
            return it == frame_mut().local_float.end() ? 0 : static_cast<std::int32_t>(it->second);
        }
        default:
            throw CoreError("read-int-unsupported-tag:" + std::to_string(operand.type));
    }
}

double Machine::read_float(const Operand& operand) {
    switch (operand.type) {
        case 0:
            return static_cast<double>(as_i32(operand.raw));
        case 1:
            return static_cast<double>(std::bit_cast<float>(operand.raw));
        case 3: {
            const auto it = global_int_.find(operand.raw);
            return static_cast<double>(decode_int(it == global_int_.end() ? encode_int(0) : it->second));
        }
        case 4: {
            const auto it = global_float_.find(operand.raw);
            return it == global_float_.end() ? 0.0 : static_cast<double>(it->second);
        }
        case 5: {
            const auto it = global_string_.find(operand.raw);
            return it == global_string_.end() ? 0.0 : parse_float_text(it->second);
        }
        case 6: case 7: case 12: case 13: {
            const auto address = read_pointer(operand);
            return address == 0 ? 0.0 : read_float_at(address);
        }
        case 9: {
            const auto it = frame_mut().local_int.find(operand.raw);
            return static_cast<double>(decode_int(it == frame_mut().local_int.end() ? encode_int(0) : it->second));
        }
        case 10: {
            const auto it = frame_mut().local_float.find(operand.raw);
            return it == frame_mut().local_float.end() ? 0.0 : static_cast<double>(it->second);
        }
        default:
            throw CoreError("read-float-unsupported-tag:" + std::to_string(operand.type));
    }
}

void Machine::write_int(const Operand& operand, std::int32_t value) {
    switch (operand.type) {
        case 3:
            global_int_[operand.raw] = encode_int(value);
            return;
        case 4:
            global_float_[operand.raw] = static_cast<float>(value);
            return;
        case 6: case 7: case 12: case 13: {
            const auto address = read_pointer(operand);
            if (address != 0) write_int_at(address, value);
            return;
        }
        case 9:
            frame_mut().local_int[operand.raw] = encode_int(value);
            return;
        case 10:
            frame_mut().local_float[operand.raw] = static_cast<float>(value);
            return;
        default:
            throw CoreError("write-int-unsupported-tag:" + std::to_string(operand.type));
    }
}

void Machine::write_float(const Operand& operand, double value) {
    switch (operand.type) {
        case 3:
            global_int_[operand.raw] = encode_int(static_cast<std::int32_t>(value));
            return;
        case 4:
            global_float_[operand.raw] = static_cast<float>(value);
            return;
        case 6: case 7: case 12: case 13: {
            const auto address = read_pointer(operand);
            if (address != 0) write_float_at(address, value);
            return;
        }
        case 9:
            frame_mut().local_int[operand.raw] = encode_int(static_cast<std::int32_t>(value));
            return;
        case 10:
            frame_mut().local_float[operand.raw] = static_cast<float>(value);
            return;
        default:
            throw CoreError("write-float-unsupported-tag:" + std::to_string(operand.type));
    }
}

std::string Machine::read_text(const Operand& operand) {
    if (operand.type == 2) {
        const auto& code = frame_mut().code;
        std::size_t offset = static_cast<std::size_t>(operand.raw) * 4;
        std::string text;
        while (offset < code.size()) {
            const std::uint8_t raw = code[offset++];
            const std::uint8_t decoded = static_cast<std::uint8_t>(raw ^ 0xFFu);
            if (decoded == 0) break;
            text.push_back(static_cast<char>(decoded));
        }
        return text;
    }
    if (operand.type == 5) {
        const auto it = global_string_.find(operand.raw);
        return it == global_string_.end() ? std::string{} : it->second;
    }
    if (operand.type == 11) {
        const auto it = frame_mut().local_string.find(operand.raw);
        return it == frame_mut().local_string.end() ? std::string{} : it->second;
    }
    const auto value = read_int(operand);
    return std::to_string(value);
}

void Machine::write_text(const Operand& operand, std::string value) {
    if (operand.type == 5) {
        global_string_[operand.raw] = std::move(value);
        return;
    }
    if (operand.type == 11) {
        frame_mut().local_string[operand.raw] = std::move(value);
        return;
    }
    throw CoreError("write-text-unsupported-tag:" + std::to_string(operand.type));
}

}  // namespace core
