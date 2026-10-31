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

std::int32_t Machine::local_int(std::uint32_t index) const {
    const auto& locals = frames_[current_frame_].local_int;
    const auto it = locals.find(index);
    return it == locals.end() ? 0 : decode_int(it->second);
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
    // 三个**引用族**全局池（类型 6 / 7 / 8）逐格清 0：`0x9` 的复位清单里它们与值族四个池并列
    // （`0x428b94` / `0x428bac` / `0x428bc4`）。★ 键是**地址**（不是 bank_key）⇒ 用本 bank 的地址窗口判。
    const std::uint32_t ref_bank_base = 0x10000000u + global_flip_ * 0x18000000u;
    for (auto it = ref_slots_.begin(); it != ref_slots_.end();) {
        const bool in_ref_bank = it->first >= ref_bank_base + 0x0C000000u &&
                                 it->first < ref_bank_base + 0x18000000u;
        it = in_ref_bank ? ref_slots_.erase(it) : std::next(it);
    }
    // 字符串族的**裸字叠层**跟着本 bank 的字符串池一起清（同一份"释放"口径；见 `store_raw`）。
    clear_string_record_words(ref_bank_base + 0x08000000u, 0x04000000u);
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
    footprint_.note_cell_read(slot);
    const auto it = ref_slots_.find(slot);
    return it == ref_slots_.end() ? 0u : it->second;
}

void Machine::write_pointer(const Operand& operand, std::uint32_t address) {
    footprint_.note_cell_write(address_of(operand));
    ref_slots_[address_of(operand)] = address;
}

std::uint32_t Machine::read_raw(std::uint32_t address) {
    footprint_.note_cell_read(address);
    std::uint32_t tag = 0;
    std::uint32_t index = 0;
    if (resolve_address(address, tag, index)) {
        switch (tag) {
            case 3: { const auto it = global_int_.find(index); return it == global_int_.end() ? encode_int(0) : it->second; }
            case 9: { const auto it = frame_mut().local_int.find(index); return it == frame_mut().local_int.end() ? encode_int(0) : it->second; }
            case 4: { const auto it = global_float_.find(index); return it == global_float_.end() ? 0u : std::bit_cast<std::uint32_t>(it->second); }
            case 10: { const auto it = frame_mut().local_float.find(index); return it == frame_mut().local_float.end() ? 0u : std::bit_cast<std::uint32_t>(it->second); }
            case 6: case 7: case 12: case 13: { const auto it = ref_slots_.find(address); return it == ref_slots_.end() ? 0u : it->second; }
            case 5: case 11: {
                const auto it = string_record_words_.find(address);
                if (it == string_record_words_.end()) {
                    // 记录内部布局属机器本体（40-pools「字符串记录」）：没写过的字没有可断言的取值
                    // ⇒ **响亮失败**，⛔ 不伪造 0。
                    throw CoreError("read-raw-string-record-unwritten:" + std::to_string(address));
                }
                return it->second;
            }
            default: throw CoreError("read-raw-unsupported-tag");
        }
    }
    // 记录内偏移（0x2d8 / 0x6c / 0x64 / 0x12f 的 4 字节步进会落到 `base + 4i`，不必 28 对齐）。
    if (is_string_record_word(address)) {
        const auto it = string_record_words_.find(address);
        if (it == string_record_words_.end()) {
            throw CoreError("read-raw-string-record-unwritten:" + std::to_string(address));
        }
        return it->second;
    }
    throw CoreError("read-raw-unmapped-address");
}

void Machine::store_raw(std::uint32_t address, std::uint32_t bits) {
    footprint_.note_cell_write(address);
    std::uint32_t tag = 0;
    std::uint32_t index = 0;
    if (resolve_address(address, tag, index)) {
        switch (tag) {
            case 3: global_int_[index] = bits; note_global_int_write(index); return;
            case 9: frame_mut().local_int[index] = bits; return;
            case 4: global_float_[index] = std::bit_cast<float>(bits); return;
            case 10: frame_mut().local_float[index] = std::bit_cast<float>(bits); return;
            case 6: case 7: case 12: case 13: ref_slots_[address] = bits; return;
            // 字符串族：写入按位模式直接写（`0x6c` / `0x2d8` / `0x64` 的 4 字节步进）——
            // 记录内部字段布局属机器本体 ⇒ 只登记这 4 字节字落在哪个地址（见 `read_raw`）。
            case 5: case 11: string_record_words_[address] = bits; return;
            default: throw CoreError("store-raw-unsupported-tag");
        }
    }
    if (is_string_record_word(address)) {
        string_record_words_[address] = bits;
        return;
    }
    throw CoreError("store-raw-unmapped-address");
}

bool Machine::is_string_record_word(std::uint32_t address) const {
    // 记录步长 28 字节、是 4 的倍数 ⇒ 池窗口内每个 4 字节对齐的地址都正好落在某条记录的
    // 偏移 0 / 4 / … / 24 上（记录内部布局不在本版，所以这里只判"落在字符串池里"）。
    if ((address & 3u) != 0) return false;
    for (std::uint32_t bank = 0; bank <= 1; ++bank) {
        if (bank == 1 && !memflip_available_) continue;  // 与 `resolve_address` 同口径：备组未登记 ⇒ 不解析
        const std::uint32_t base = pool_base(5, bank);
        if (address >= base && address < base + 0x04000000u) return true;
    }
    const std::uint32_t local_base = pool_base(11);
    return address >= local_base && address < local_base + 0x00100000u;
}

void Machine::clear_string_record_words(std::uint32_t base, std::uint32_t span) {
    for (auto it = string_record_words_.begin(); it != string_record_words_.end();) {
        const bool inside = it->first >= base && it->first < base + span;
        it = inside ? string_record_words_.erase(it) : std::next(it);
    }
}

bool Machine::pool_window_of(std::uint32_t address, std::uint32_t& base, std::uint32_t& span) const {
    // 六个全局族：每组 0x04000000（两组 bank）；六个局部族：每族 0x00100000。
    // 元素宽度都是 4 的倍数 ⇒ 窗口内 4 字节对齐的地址都是落在某个元素上的裸字地址。
    if ((address & 3u) != 0) return false;
    for (std::uint32_t candidate = 3; candidate <= 8; ++candidate) {
        for (std::uint32_t bank = 0; bank <= 1; ++bank) {
            if (bank == 1 && !memflip_available_) continue;  // 与 `resolve_address` 同口径
            const std::uint32_t candidate_base = pool_base(candidate, bank);
            if (address >= candidate_base && address < candidate_base + 0x04000000u) {
                base = candidate_base;
                span = 0x04000000u;
                return true;
            }
        }
    }
    for (std::uint32_t candidate = 9; candidate <= 14; ++candidate) {
        const std::uint32_t candidate_base = pool_base(candidate);
        if (address >= candidate_base && address < candidate_base + 0x00100000u) {
            base = candidate_base;
            span = 0x00100000u;
            return true;
        }
    }
    return false;
}

void Machine::check_array_write_range(std::uint32_t target, std::uint32_t count) const {
    if (count == 0) return;  // 一格都不写 ⇒ 没有落点，也谈不上越界
    std::uint32_t base = 0;
    std::uint32_t span = 0;
    if (!pool_window_of(target, base, span)) {
        throw CoreError("array-write-unmapped-target:" + std::to_string(target));
    }
    // 用 64 位算末格：count 很大时 32 位加法会回绕，那会把"越界"误判成"在界内"。
    const std::uint64_t end = static_cast<std::uint64_t>(target) +
                              4ull * static_cast<std::uint64_t>(count - 1);
    if (end >= static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(span)) {
        throw CoreError("array-write-past-pool:target=" + std::to_string(target) +
                        ":count=" + std::to_string(count));
    }
    return;
}

std::int32_t Machine::read_int_at(std::uint32_t address) {
    std::uint32_t tag = 0;
    std::uint32_t index = 0;
    if (!resolve_address(address, tag, index)) throw CoreError("read-int-unmapped-address");
    footprint_.note_cell_read(address);
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
    footprint_.note_cell_read(address);
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
    footprint_.note_cell_write(address);
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
    footprint_.note_cell_write(address);
    switch (tag) {
        case 3: global_int_[index] = encode_int(static_cast<std::int32_t>(value)); note_global_int_write(index); return;
        case 9: frame_mut().local_int[index] = encode_int(static_cast<std::int32_t>(value)); return;
        case 4: global_float_[index] = static_cast<float>(value); return;
        case 10: frame_mut().local_float[index] = static_cast<float>(value); return;
        default: throw CoreError("write-float-at-unsupported-tag");
    }
}

std::int32_t Machine::read_int(const Operand& operand) {
    // 诊断足迹：值族（3 / 4 / 5 / 9 / 10 / 11）的**格**；指针族由 `read_pointer` 记槽、`read_int_at` 记目标。
    if (operand.type == 3 || operand.type == 4 || operand.type == 5 || operand.type == 9 ||
        operand.type == 10 || operand.type == 11) {
        footprint_.note_cell_read(address_of(operand));
    }
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
    if (operand.type == 3 || operand.type == 4 || operand.type == 5 || operand.type == 9 ||
        operand.type == 10 || operand.type == 11) {
        footprint_.note_cell_read(address_of(operand));
    }
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
    if (operand.type == 3 || operand.type == 4 || operand.type == 9 || operand.type == 10) {
        footprint_.note_cell_write(address_of(operand));
    }
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
    if (operand.type == 3 || operand.type == 4 || operand.type == 9 || operand.type == 10) {
        footprint_.note_cell_write(address_of(operand));
    }
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
    // ★ 字符串容器句柄（`0x8005` / `0x800B`）：本条读出的是容器**第 0 个元素**的文本，
    //   不是记录里那串十进制引用（`CORE-OPS-TEXT-SET` 的源侧）。
    if (operand.type == 0x8005u || operand.type == 0x800Bu) {
        return read_string_handle(operand);
    }
    if (operand.type == 5) {
        footprint_.note_cell_read(address_of(operand));
        const auto it = global_string_.find(bank_key(operand.raw));
        return it == global_string_.end() ? std::string{} : it->second;
    }
    if (operand.type == 11) {
        footprint_.note_cell_read(address_of(operand));
        const auto it = frame_mut().local_string.find(operand.raw);
        return it == frame_mut().local_string.end() ? std::string{} : it->second;
    }
    // 指针族里的**字符串**两条（8 = 全局 / 14 = 局部的"指向字符串记录"）：
    // 槽里存的是地址，地址解析回 (池, 下标) 后按字符串记录取。★ 引擎的文本读原语支持 0–14 除 7/13，
    // 所以这两条必须能读；`read_int` 对它们**应当**抛（它们是地址，不是整数）。
    // 待核：指针指向的字符串记录在本模型里就是 `global_string_` / `local_string` 的那一格（残差）。
    if (operand.type == 8 || operand.type == 14) {
        const auto address = read_pointer(operand);
        std::uint32_t tag = 0;
        std::uint32_t index = 0;
        if (!resolve_address(address, tag, index)) throw CoreError("read-text-unmapped-address");
        footprint_.note_cell_read(address);
        if (tag == 5) {
            const auto it = global_string_.find(index);  // resolve_address 给的键已带 bank
            return it == global_string_.end() ? std::string{} : it->second;
        }
        if (tag == 11) {
            const auto it = frame_mut().local_string.find(index);
            return it == frame_mut().local_string.end() ? std::string{} : it->second;
        }
        throw CoreError("read-text-unsupported-pool-tag:" + std::to_string(tag));
    }
    // ★ 浮点源 ⇒ **定点十进制、小数点后 6 位**（`printf("%lf")`；末位舍入随宿主运行时，
    //   跨平台末位差异属明确舍弃）。⛔ 不许走 `read_int` 的"向零截断"。
    if (operand.type == 1 || operand.type == 4 || operand.type == 10) {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%lf", read_float(operand));
        return std::string(buffer);
    }
    // 整数源 ⇒ 十进制（可带负号、无前导零、无宽度填充）。
    const auto value = read_int(operand);
    return std::to_string(value);
}

// ── 字符串容器句柄（`CORE-POOL-HANDLE-STRING` / `CORE-CONTAINERS-*`）──
// 记录里的**十进制文本**就是容器引用；容器对象 = 元素是字符串记录的数组。
std::uint32_t Machine::parse_container_reference(const std::string& text) {
    // 与字符串记录的整数读同口径（C 的十进制解析）：非数字得 0（= 空引用）。
    return static_cast<std::uint32_t>(parse_int_text(text));
}

std::uint32_t Machine::slot_address_of(const Operand& operand) const {
    // 容器句柄 → 它所属的那个字符串池（0x8005 = 全局、0x800B = 局部）；字符串记录的宽是 28 字节。
    const std::uint32_t tag = operand.type == 0x8005u ? 5u : 11u;
    return pool_base(tag) + 28u * operand.raw;
}

std::string Machine::string_record_text(std::uint32_t index, bool local) const {
    if (local) {
        const auto it = frame().local_string.find(index);
        return it == frame().local_string.end() ? std::string{} : it->second;
    }
    const auto it = global_string_.find(bank_key(index));
    return it == global_string_.end() ? std::string{} : it->second;
}

std::size_t Machine::string_container_size(std::uint32_t reference) const {
    const auto it = string_containers_.find(reference);
    return it == string_containers_.end() ? 0 : it->second.size();
}

std::string Machine::string_container_first(std::uint32_t reference) const {
    const auto it = string_containers_.find(reference);
    if (it == string_containers_.end() || it->second.empty()) return std::string{};
    return it->second.front();
}

std::string Machine::read_string_handle(const Operand& operand) {
    const bool local = operand.type == 0x800Bu;
    // ★ 容器句柄的 raw 是**池下标**，但 `address_of` 的外延不含 0x8005 / 0x800B（那四个不是值族标签）。
    footprint_.note_cell_read(slot_address_of(operand));
    const std::string text = string_record_text(operand.raw, local);
    // 记录为空（从未写过 / 空容器）⇒ **空串**（不是"空引用"）。
    if (text.empty()) return std::string{};
    const auto reference = parse_container_reference(text);
    // 记录文本不是有效容器引用（被脚本改成非数字 / 伪造值）⇒ 属「容器句柄的失败路径」的
    // **未定义行为**：本模型显式诊断 + 响亮失败（⛔ 不把它当成空串）。
    const auto it = string_containers_.find(reference);
    if (it == string_containers_.end()) {
        report_error("string-container-invalid-reference:" + text);
        throw CoreError("string-container-invalid-reference:" + text);
    }
    if (it->second.empty()) return std::string{};
    return it->second.front();
}

void Machine::write_string_handle(const Operand& operand, std::string value) {
    const bool local = operand.type == 0x800Bu;
    footprint_.note_cell_write(slot_address_of(operand));
    std::string text = string_record_text(operand.raw, local);
    std::uint32_t reference = 0;
    if (text.empty()) {
        // 空记录 ⇒ 先建容器，把**引用的十进制文本**写进记录（记录级文本写入）。
        reference = next_string_container_reference_++;
        string_containers_[reference] = {};
        text = std::to_string(reference);
        if (local) {
            frame_mut().local_string[operand.raw] = text;
        } else {
            global_string_[bank_key(operand.raw)] = text;
        }
    } else {
        reference = parse_container_reference(text);
    }
    const auto it = string_containers_.find(reference);
    if (it == string_containers_.end()) {
        report_error("string-container-invalid-reference:" + text);
        throw CoreError("string-container-invalid-reference:" + text);
    }
    // 元素为空 ⇒ 追加第 1 个元素；不为空 ⇒ **覆写第 0 个**（其余元素不变）。
    if (it->second.empty()) {
        it->second.push_back(std::move(value));
    } else {
        it->second.front() = std::move(value);
    }
}

void Machine::write_text(const Operand& operand, std::string value) {
    // ★ 字符串容器句柄（`0x8005` / `0x800B`）：记录里保存的仍是容器引用（十进制文本），
    //   源文本去的是容器**首元素**（空容器 ⇒ 先建首元素；非空 ⇒ 覆写首元素）。见 `write_string_handle`。
    if (operand.type == 0x8005u || operand.type == 0x800Bu) {
        write_string_handle(operand, std::move(value));
        return;
    }
    if (operand.type == 5) {
        footprint_.note_cell_write(address_of(operand));
        global_string_[bank_key(operand.raw)] = std::move(value);
        return;
    }
    if (operand.type == 11) {
        footprint_.note_cell_write(address_of(operand));
        frame_mut().local_string[operand.raw] = std::move(value);
        return;
    }
    // ★ 指针族里的**字符串**两条（8 = 全局 / 14 = 局部的"指向字符串记录"）：槽里存的是
    //   **记录地址**，解引用后写那一条记录。引擎的落点分派（EA `0x433310`）接受 5 / 8 / 11 / 14，
    //   四条都汇到同一个收尾（EA `0x43362F` = `sub_40C120(记录, -1, 0, 源)`）⇒ 语义上与 5 / 11
    //   同一件事，只是多一层间接。只改那条记录的字节 + 长度；⛔ 不碰字体对象 / 文本层画布 / 绘制项。
    //   ⛔ 容器句柄（0x8005 / 0x800B）不走这里：它们走上一条**容器**路径（记录只存引用）。
    if (operand.type == 8 || operand.type == 14) {
        const auto address = read_pointer(operand);
        std::uint32_t tag = 0;
        std::uint32_t index = 0;
        if (!resolve_address(address, tag, index)) {
            // 引擎不解引用判空、也不查池界（空指针 / 跨界都是 UB）；本模型**响亮失败**，
            // ⛔ 不许静默写成 0 号记录。
            throw CoreError("write-text-unmapped-address:" + std::to_string(address));
        }
        footprint_.note_cell_write(address);
        if (tag == 5) {
            global_string_[index] = std::move(value);
            return;
        }
        if (tag == 11) {
            frame_mut().local_string[index] = std::move(value);
            return;
        }
        throw CoreError("write-text-unsupported-pool-tag:" + std::to_string(tag));
    }
    throw CoreError("write-text-unsupported-tag:" + std::to_string(operand.type));
}

// ── 错误通道（`60-ops-numeric` 的「错误通道」）──
// 引擎为一次触发发**一个**平台写请求：文本 + 它的长度。两个抑制条件都在引擎侧判定：
//   ① 输出句柄无效 ⇒ 整条被丢弃（不写、不抛、不中止）；
//   ② 宿主装了过滤器且过滤器返回非 0 ⇒ 同样丢弃。
// ★ 诊断本身**无条件**保留（`errors_` 只给测试 / 诊断读）；抑制只影响那次平台写请求 ⇒
//   用例可以同时断言"请求没发"与"诊断还在"。
// ★ 一次触发 = 一次调用 ⇒ 本函数是**唯一**拼请求的地方：⛔ 不要在 50+ 个 handler 里各拼一次。
void Machine::report_error(std::string text) {
    // 三道判定，**顺序固定且可观察**（`60-ops-numeric` 的「错误通道」）：
    // ① 装了**过滤器** ⇒ 先调用它：返回非 0 ⇒ 整条丢弃（不落盘、不抛、不中止），「已装」标志**保留**；
    // ② 返回 0（或本来就没装）⇒ **这时才**判**输出句柄**：无效 ⇒ 不写（同样不抛、不中止）；
    // ③ 无论 ② 有没有真的写出字节（**句柄无效时也一样**）⇒ 把「已装」标志清 0（过滤器是**一次性**的）。
    // ⛔ 不许把句柄判定提到过滤器之前（那会让一次性标志在"无日志文件"的配置下永远留着）。
    bool discard = false;
    if (error_filter_installed_ && error_filter_) {
        if (error_filter_() != 0) discard = true;
    }
    if (!discard) {
        if (error_handle_valid_ && error_writer_) {
            ++error_write_requests_;
            // 文本 + 它的长度：长度取**字节数**；文本里的 CRLF 已在各 handler 里逐字给出。
            error_writer_(text, text.size());
        }
        error_filter_installed_ = false;  // ③ 一次性
    }
    // 诊断**无条件**保留（抑制只影响平台写请求）。
    errors_.push_back(std::move(text));
}

// ── 输出对象（`45-objects`「对象前缀：类行为表与输出对象」）──
// ★ 三项操作都**作用于那个进程级静态对象**、不改变调用对象自己的状态；本条只落外部效果的组成：
//   追加 = 文本原样；按位置追加 = `(<脚本名>：<行号>行目) ` 前缀 + 文本；报告 = `\r\n致命的エラー[`
//   + 文本 + `]\r\n`，随后把文本交给根对象的错误处理。
// ★ 与**错误通道**（`report_error`：场景错误缓冲 + 一次平台写请求）是两路，⛔ 不许合并。
void Machine::output_append(const std::string& text) {
    if (output_object_io_.log) output_object_io_.log(text);
}

void Machine::output_append_with_position(const std::string& script_name, std::int32_t line,
                                          const std::string& text) {
    // 前缀逐字：`(` + 脚本名 + `：`(U+FF1A) + 行号 + `行目` + `)` + 一个半角空格。
    // ★ 脚本名 / 行号的来源未取证（残差 `RES-OBJ-LOG-PREFIX`）⇒ 由调用方给，⛔ 这里不去查"当前脚本位置"。
    std::string composed = "(" + script_name + "：" + std::to_string(line) + "行目) " + text;
    output_append(composed);
}

void Machine::output_report(const std::string& text) {
    ++output_reports_;
    output_append("\r\n致命的エラー[" + text + "]\r\n");
    // 基类操作只用到「窗口句柄 + 文本」那个错误处理入口（`45-objects` 的明文）；
    // 「两个整数 + 文本」的是行为表里的另一个报告入口，本模型只留接缝、基类不调。
    if (output_object_io_.root_error_window) output_object_io_.root_error_window(text);
}

}  // namespace core
