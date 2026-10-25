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

std::uint32_t Machine::pool_base(std::uint32_t tag) const { return pool_base(tag, global_flip_); }

std::uint32_t Machine::pool_base(std::uint32_t tag, std::uint32_t bank) const {
    // 六个全局族：一组 = 6 × 0x04000000 = 0x18000000，两组正好铺到 0x40000000（局部池起点）。
    if (tag >= 3 && tag <= 8) {
        return 0x10000000u + bank * 0x18000000u + (tag - 3) * 0x04000000u;
    }
    if (tag >= 9 && tag <= 14) {
        return 0x40000000u + static_cast<std::uint32_t>(current_frame_) * 0x00800000u +
               (tag - 9) * 0x00100000u;
    }
    throw CoreError("pool-base-unsupported-tag:" + std::to_string(tag));
}

std::uint32_t Machine::bank_key(std::uint32_t index) const {
    return global_flip_ == 0 ? index : kBankKeyStride + index;
}

void Machine::clear_global_pool_bank() {
    // 只清**当前选中 bank**（另一组保留原值）：键 >= kBankKeyStride 属于 bank 1。
    const auto in_bank = [this](std::uint32_t key) {
        return global_flip_ == 0 ? key < kBankKeyStride : key >= kBankKeyStride;
    };
    for (auto it = global_int_.begin(); it != global_int_.end();) {
        it = in_bank(it->first) ? global_int_.erase(it) : std::next(it);
    }
    for (auto it = global_float_.begin(); it != global_float_.end();) {
        it = in_bank(it->first) ? global_float_.erase(it) : std::next(it);
    }
    for (auto it = global_string_.begin(); it != global_string_.end();) {
        it = in_bank(it->first) ? global_string_.erase(it) : std::next(it);
    }
    return;
}

std::uint32_t Machine::address_of(const Operand& operand) const {
    const auto tag = operand.type;
    if (tag < 3 || tag > 14) throw CoreError("address-of-unsupported-tag:" + std::to_string(tag));
    const std::uint32_t width = (tag == 5 || tag == 11) ? 28u : 4u;
    return pool_base(tag) + width * operand.raw;
}

bool Machine::is_pointer_tag(std::uint32_t tag) {
    switch (tag) {
        case 6: case 7: case 8: case 12: case 13: case 14:
            return true;
        default:
            return false;
    }
}

std::uint32_t Machine::address_of_operand(const Operand& operand) const {
    // 「取址」（`0x63` / `0x1b0`）：值族给槽地址；引用族给**它指向的**地址（槽内容）。
    if (is_pointer_tag(operand.type)) return read_pointer(operand);
    return address_of(operand);
}

std::uint32_t Machine::next_random() {
    // 宿主随机源优先；缺省是确定性 LCG（同种子同序列）。返回值按**非负**用（规范：被除数非负）。
    if (random_source_) return random_source_() & 0x7FFFFFFFu;
    random_state_ = random_state_ * 1664525u + 1013904223u;
    return random_state_ >> 1;  // 31 位非负
}

bool Machine::resolve_address(std::uint32_t address, std::uint32_t& tag, std::uint32_t& index) const {
    for (std::uint32_t candidate = 3; candidate <= 8; ++candidate) {
        for (std::uint32_t bank = 0; bank <= 1; ++bank) {
            // 备组未登记（启动期键为 0）⇒ 落到备组窗口的访问**响亮失败**（⛔ 不补分配）。
            if (bank == 1 && !memflip_available_) continue;
            const std::uint32_t base = pool_base(candidate, bank);
            const std::uint32_t width = (candidate == 5) ? 28u : 4u;
            if (address >= base && address < base + 0x04000000u) {
                if ((address - base) % width != 0) return false;
                tag = candidate;
                index = bank * kBankKeyStride + (address - base) / width;  // 键里带 bank
                return true;
            }
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
        case 3: global_int_[index] = bits; note_global_int_write(index); return;
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
        case 3: global_int_[index] = encode_int(value); note_global_int_write(index); return;
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
        case 3: global_int_[index] = encode_int(static_cast<std::int32_t>(value)); note_global_int_write(index); return;
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
            const auto it = global_int_.find(bank_key(operand.raw));
            return it == global_int_.end() ? decode_int(encode_int(0)) : decode_int(it->second);
        }
        case 4: {
            const auto it = global_float_.find(bank_key(operand.raw));
            return it == global_float_.end() ? 0 : static_cast<std::int32_t>(it->second);
        }
        case 5: {
            const auto it = global_string_.find(bank_key(operand.raw));
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
            const auto it = global_int_.find(bank_key(operand.raw));
            return static_cast<double>(decode_int(it == global_int_.end() ? encode_int(0) : it->second));
        }
        case 4: {
            const auto it = global_float_.find(bank_key(operand.raw));
            return it == global_float_.end() ? 0.0 : static_cast<double>(it->second);
        }
        case 5: {
            const auto it = global_string_.find(bank_key(operand.raw));
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
            global_int_[bank_key(operand.raw)] = encode_int(value); note_global_int_write(operand.raw);
            return;
        case 4:
            global_float_[bank_key(operand.raw)] = static_cast<float>(value);
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
            global_int_[bank_key(operand.raw)] = encode_int(static_cast<std::int32_t>(value)); note_global_int_write(operand.raw);
            return;
        case 4:
            global_float_[bank_key(operand.raw)] = static_cast<float>(value);
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
        const auto it = global_string_.find(bank_key(operand.raw));
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
        global_string_[bank_key(operand.raw)] = std::move(value);
        return;
    }
    if (operand.type == 11) {
        frame_mut().local_string[operand.raw] = std::move(value);
        return;
    }
    throw CoreError("write-text-unsupported-tag:" + std::to_string(operand.type));
}

}  // namespace core
