#include "core/vm.h"

#include "core/agf.h"
#include "core/install.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace core {
namespace {
std::uint32_t rol32(std::uint32_t value, int amount) {
    return std::rotl(value, amount);
}
std::uint32_t ror32(std::uint32_t value, int amount) {
    return std::rotr(value, amount);
}
std::int32_t as_i32(std::uint32_t value) {
    return static_cast<std::int32_t>(value);
}
std::int32_t parse_int_text(const std::string& text) {
    const char* begin = text.c_str();
    char* end = nullptr;
    const long value = std::strtol(begin, &end, 10);
    return end == begin ? 0 : static_cast<std::int32_t>(value);
}

double parse_float_text(const std::string& text) {
    const char* begin = text.c_str();
    char* end = nullptr;
    const double value = std::strtod(begin, &end);
    return end == begin ? 0.0 : value;
}

std::uint32_t as_u32(std::int32_t value) {
    return static_cast<std::uint32_t>(value);
}
}  // namespace

Machine::Machine(std::uint32_t int_key, const OpcodeTable& table, const Install* install,
                 ConfigStore config)
    : key_(int_key == 0 ? 1u : int_key), table_(table), install_(install), config_(std::move(config)) {}

std::uint32_t Machine::encode_int(std::int32_t value) const {
    return rol32(ror32(as_u32(value), 7) ^ key_, 21);
}

std::int32_t Machine::decode_int(std::uint32_t bits) const {
    return as_i32(ror32(rol32(bits, 11) ^ key_, 25));
}

void Machine::load_root(const Bytes& script, std::uint32_t script_id) {
    const ScriptHeader header = parse_script_header(script);
    Frame& root = frames_[0];
    root = Frame{};
    root.loaded = true;
    root.code.assign(script.begin() + header.body_offset, script.end());
    root.ip = 0;
    root.length_slot = 0;
    root.parent = -1;
    root.script_id = script_id;
    current_frame_ = 0;
    halted_ = false;
    stop_ = {};
}

Frame& Machine::frame_mut() {
    return frames_[current_frame_];
}

void Machine::write_length(std::uint32_t words) {
    frame_mut().length_slot = words;
}

void Machine::jump_to(std::uint32_t position) {
    if (position > frame_mut().code.size() / 4) {
        throw CoreError("jump-out-of-code:" + std::to_string(position));
    }
    frame_mut().ip = static_cast<std::size_t>(position) * 4;
    write_length(0);
}

void Machine::push_return(std::uint32_t position) {
    frame_mut().ret_stack.push_back(static_cast<std::int32_t>(position));
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

void Machine::execute(const Instruction& instruction) {
    Frame& f = frame_mut();
    const auto& ops = instruction.operands;
    switch (instruction.opcode) {
        case 0x01:  // abort：退出型异常，不写长度槽
            halted_ = true;
            stop_.reason = StopReason::Error;
            stop_.message = "abort";
            return;
        case 0x02:  // exit：父帧号 >= 0 回到父帧；根帧结束
            if (f.parent >= 0) {
                current_frame_ = static_cast<std::size_t>(f.parent);
            } else {
                halted_ = true;
                stop_.reason = StopReason::Halted;
                stop_.message = "root-exit";
            }
            return;
        case 0x88: {  // 成对写 + 条件清门字第 27 位
            const auto value = read_int(ops[0]);
            scalar_88a_ = value;
            scalar_88b_ = value;
            if (value != 0) {
                scalar_88c_ = 1;
            } else {
                state_flags_ &= ~(1u << 27);
            }
            return;
        }
        case 0x03: {  // call-script：父帧挂起，子帧 = 父帧号 + 1
            if (install_ == nullptr) throw CoreError("call-script-loader-missing");
            if (current_frame_ >= kFrameCount - 1) throw CoreError("frame-depth-limit");
            const auto file_id = read_int(ops[0]);
            Frame& parent = frame_mut();
            parent.position = static_cast<std::uint32_t>(parent.ip / 4);
            parent.length_slot = 3;
            const core::Bytes script = core::read_install_file(
                *install_, static_cast<std::uint32_t>(file_id));
            const core::ScriptHeader header = core::parse_script_header(script);
            const std::size_t child_index = current_frame_ + 1;
            Frame& child = frames_[child_index];
            child = Frame{};
            child.loaded = true;
            child.code.assign(script.begin() + header.body_offset, script.end());
            child.ip = 0;
            child.length_slot = 0;
            child.parent = static_cast<std::int32_t>(current_frame_);
            child.script_id = static_cast<std::uint32_t>(file_id);
            current_frame_ = child_index;
            return;
        }
        case 0x06: {  // load-frame：装进目标槽，不切换当前帧
            write_length(5);
            if (install_ == nullptr) throw CoreError("load-frame-loader-missing");
            const auto file_id = read_int(ops[0]);
            const auto target = read_int(ops[1]);
            if (target < 0 || target >= static_cast<int>(kFrameCount)) throw CoreError("frame-depth-limit");
            const core::Bytes script = core::read_install_file(*install_, static_cast<std::uint32_t>(file_id));
            const core::ScriptHeader header = core::parse_script_header(script);
            Frame& slot = frames_[static_cast<std::size_t>(target)];
            slot = Frame{};
            slot.loaded = true;
            slot.code.assign(script.begin() + header.body_offset, script.end());
            slot.ip = 0;
            slot.length_slot = 0;
            slot.parent = static_cast<std::int32_t>(current_frame_);
            slot.script_id = static_cast<std::uint32_t>(file_id);
            return;
        }
        case 0x6C: {  // fill-zero：按取址写 count 个 ENC(0)
            const auto target = address_of(ops[0]);
            const auto count = read_int(ops[1]);
            const auto zero = encode_int(0);
            for (std::int32_t i = 0; i < count; ++i) store_raw(target + 4u * static_cast<std::uint32_t>(i), zero);
            return;
        }
        case 0x2D8: {  // set-array-to
            const auto target = address_of(ops[0]);
            const auto value = encode_int(read_int(ops[1]));
            const auto count = read_int(ops[2]);
            for (std::int32_t i = 0; i < count; ++i) store_raw(target + 4u * static_cast<std::uint32_t>(i), value);
            return;
        }
        case 0x61: {  // lookup-array
            const auto index = read_int(ops[2]);
            const auto base = address_of(ops[1]);
            const auto width = (ops[0].type == 8 || ops[0].type == 14) ? 28u : 4u;
            write_pointer(ops[0], base + width * static_cast<std::uint32_t>(index));
            return;
        }
        case 0x12C: {  // lookup-array-2d：读序 4 → 3 → 5 → 2
            const auto row_width = read_int(ops[3]);
            const auto row = read_int(ops[2]);
            const auto column = read_int(ops[4]);
            const auto base = address_of(ops[1]);
            const auto width = (ops[0].type == 8 || ops[0].type == 14) ? 28u : 4u;
            const auto index = as_i32(as_u32(row) * as_u32(row_width) + as_u32(column));
            write_pointer(ops[0], base + width * static_cast<std::uint32_t>(index));
            return;
        }
        case 0x64: {  // copy-local-array：块首 word 是 count，后接源 dword
            const auto target = address_of(ops[0]);
            const auto block = read_int(ops[1]);
            const auto* code = frame_mut().code.data();
            const auto size = frame_mut().code.size();
            const std::size_t block_offset = static_cast<std::size_t>(block) * 4;
            if (block_offset + 4 > size) throw CoreError("array-block-past-code");
            const auto count = read_u32(frame_mut().code, block_offset);
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto source = read_u32(frame_mut().code, block_offset + 4 + i * 4);
                store_raw(target + 4u * i, encode_int(static_cast<std::int32_t>(source)));
            }
            (void)code;
            return;
        }
        case 0x10C: { const auto value = read_int(ops[0]); const auto slot = read_int(ops[1]); if (value > 0x1f) throw CoreError("key-multi-range"); indirect_[slot] = value; return; }
        case 0x308: (void)read_int(ops[0]); return;  // 宿主接口转发请求；字段未建模
        case 0xFE: { const auto value = read_int(ops[0]); if (static_cast<std::uint32_t>(value) > 31u) throw CoreError("key-total-out-of-range"); key_total_ = value; return; }
        case 0x21C: state_flags_ |= (1u << 10); return;  // 置等待位；放行由 step 的等待服务做
        case 0x101: state_flags_ &= ~(1u << 27); input_word_ = 0; input_a_ = 1; input_b_ = 0; return;
        case 0xAE: { write_length(1); if (save_version_flag_ == 0) return; throw CoreError("save-version-repair-unmodeled"); }
        case 0x2EE: scalar_2ee_ = read_int(ops[0]); return;
        case 0x10F: scalar_10f_ = read_int(ops[0]); return;
        case 0x248: scalar_248_ = read_int(ops[0]); return;
        case 0x8B: font_8b_ = read_int(ops[0]); return;
        case 0x261: engine5_[0] = read_int(ops[0]); return;
        case 0x260: engine5_[4] = read_int(ops[3]); engine5_[3] = read_int(ops[2]); engine5_[2] = read_int(ops[1]); engine5_[1] = read_int(ops[0]); return;
        case 0x107: { const auto index = read_int(ops[0]); const auto value = read_int(ops[1]); if (index >= 0 && index <= 0x1f) array_d551_[static_cast<std::size_t>(index)] = value; return; }
        case 0x10B: { const auto value = read_int(ops[0]); const auto index = read_int(ops[1]); if (index >= 0 && index <= 0x1f) array_d1383_[static_cast<std::size_t>(index)] = value; return; }
        case 0x30A: { const auto value = read_int(ops[0]); const auto index = read_int(ops[1]); if (index < 0 || index > 7 || value < 0 || value > 0x1f) throw CoreError("scalar-array-range"); array_d1969_[static_cast<std::size_t>(index)] = value; return; }
        case 0x25B: scalar_25b_b_ = read_int(ops[0]); scalar_25b_a_ = 2; return;
        case 0x143: write_length(0); frame_mut().ip += 4; return;
        case 0x130: write_int(ops[0], show_logo_); return;
        case 0x1A4:  // 字体对象两格：读序 2 → 1
            font_pair_b_ = read_int(ops[1]);
            font_pair_a_ = read_int(ops[0]);
            return;
        case 0x76: case 0x77: {  // 字体主色/副色：低 24 位字节倒序
            const auto value = as_u32(read_int(ops[0])) & 0xFFFFFFu;
            const auto swapped = ((value & 0xFFu) << 16) | (value & 0xFF00u) | ((value >> 16) & 0xFFu);
            if (instruction.opcode == 0x76) font_primary_color_ = as_i32(swapped);
            else font_secondary_color_ = as_i32(swapped);
            return;
        }
        case 0x2BD: {  // 两格 = 700 / 0
            const auto value = read_int(ops[0]) != 0 ? 700 : 0;
            font_metric_a_ = value;
            font_metric_b_ = value;
            return;
        }
        case 0x2DE:  // 字体名查表：宿主字体表未接，空表 ⇒ 一律未命中 -1
            (void)read_text(ops[1]);
            write_int(ops[0], -1);
            return;
        case 0x192:  // set-string：源按文本读，写入全局/局部字符串记录
            write_text(ops[0], read_text(ops[1]));
            return;
        case 0x1A9:  // save-string：把源文本写进脚本字符串键
            if (ops[0].type != 5) throw CoreError("config-key-unsupported-tag");
            config_.save_script_string(ops[0].raw, read_text(ops[0]));
            return;
        case 0x1AA: {  // load-string：读脚本字符串键并写回落点
            if (ops[0].type != 5) throw CoreError("config-key-unsupported-tag");
            write_text(ops[0], config_.load_script_string(ops[0].raw));
            return;
        }
        case 0x1A3: {  // load-int：键 = 全局整型池下标；结果无条件写回操作数
            const auto key = script_int_key_index(ops[0]);
            const auto value = config_.load_script_int(key);
            write_int(ops[0], value);
            return;
        }
        case 0x1A2: {  // save-int：把操作数的整数值写进脚本键
            const auto key = script_int_key_index(ops[0]);
            config_.save_script_int(key, read_int(ops[0]));
            return;
        }
        case 0x320: {  // 建/换网格：顶点数 <= 0 不建
            const auto count = read_int(ops[8]);
            const auto op10 = read_int(ops[9]);
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            if (count <= 0) return;
            MeshRecord& mesh = meshes_[key];
            mesh.flags |= 1u;
            mesh.count = count;
            mesh.op10 = op10;
            return;
        }
        case 0x322: {  // 网格当前颜色
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            (void)read_int(ops[1]);
            const auto alpha = read_int(ops[2]);
            const auto color = read_int(ops[3]);
            MeshRecord& mesh = meshes_[key];
            mesh.color = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
            return;
        }
        case 0x323: {  // 网格颜色窗
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            const auto delay = read_int(ops[1]);
            const auto duration = read_int(ops[2]);
            const auto alpha = read_int(ops[3]);
            const auto color = read_int(ops[4]);
            MeshRecord& mesh = meshes_[key];
            mesh.flags |= 2u;
            mesh.delay = delay;
            mesh.duration = duration;
            mesh.color = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
            return;
        }
        case 0x1FB: {  // 绘制纹理：只入队
            (void)read_int(ops[5]);
            (void)read_int(ops[3]);
            (void)read_int(ops[4]);
            (void)read_int(ops[2]);
            const auto left = read_int(ops[2]);
            const auto top = read_int(ops[3]);
            const auto width = read_int(ops[4]);
            const auto height = read_int(ops[5]);
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            DrawItem& item = draw_items_[key];
            item.flags |= 1u;
            item.texture_id = static_cast<std::uint32_t>(read_int(ops[1]));
            item.left = left;
            item.top = top;
            item.right = left + width;
            item.bottom = top + height;
            item.aux[0] = static_cast<float>(read_int(ops[6]));
            item.aux[1] = static_cast<float>(read_int(ops[7]));
            item.aux[2] = 0.0f;
            return;
        }
        case 0x202: {  // 颜色窗：已存在位为 1 才排窗
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            const auto delay = read_int(ops[1]);
            const auto duration = read_int(ops[2]);
            const auto alpha = read_int(ops[3]);
            const auto color = read_int(ops[4]);
            auto it = draw_items_.find(key);
            if (it != draw_items_.end() && (it->second.flags & 1u) != 0) {
                it->second.flags |= 2u;
                it->second.delay = delay;
                it->second.duration = duration;
                it->second.color = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
            }
            return;
        }
        case 0x203: {  // 直接设当前颜色
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            (void)read_int(ops[1]);
            const auto alpha = read_int(ops[2]);
            const auto color = read_int(ops[3]);
            DrawItem& item = draw_items_[key];
            item.color = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
            return;
        }
        case 0x1F7: {  // 拆离绘制键
            const auto count = read_int(ops[1]);
            const auto key = read_int(ops[0]);
            if (count <= 1) draw_items_.erase(static_cast<std::uint32_t>(key));
            else for (std::int32_t i = 0; i < count; ++i) draw_items_.erase(static_cast<std::uint32_t>(key + i));
            return;
        }
        case 0x1F8: {  // 空白建纹理
            const auto id = static_cast<std::uint32_t>(read_int(ops[0]));
            const auto width = read_int(ops[1]);
            const auto height = read_int(ops[2]);
            (void)read_int(ops[3]);
            TextureRecord& record = textures_[id];
            record.file = -1;
            record.blank = true;
            record.present = true;
            record.width = static_cast<std::uint32_t>(width);
            record.height = static_cast<std::uint32_t>(height);
            return;
        }
        case 0x1FA: {  // 释放纹理
            const auto id = static_cast<std::uint32_t>(read_int(ops[0]));
            (void)read_int(ops[0]);
            textures_.erase(id);
            return;
        }
        case 0x1F9: {  // 设置纹理：读序 2 → 1 → 3 → 2；无平台上传，只建记录
            const auto id = read_int(ops[1]);
            const auto file = read_int(ops[0]);
            const auto raw_key = read_int(ops[2]);
            (void)read_int(ops[1]);
            if (install_ == nullptr) throw CoreError("texture-loader-missing");
            const core::Bytes bytes = core::read_install_file(*install_, static_cast<std::uint32_t>(file));
            const core::DecodedAgf image = core::decode_agf(bytes);
            TextureRecord record;
            record.file = file;
            record.color_key = raw_key < 0 ? 0u : (0xFF000000u | (static_cast<std::uint32_t>(raw_key) & 0xFFFFFFu));
            record.width = image.width;
            record.height = image.height;
            record.present = true;
            textures_[static_cast<std::uint32_t>(id)] = record;
            return;
        }
        case 0x303: {  // 写 f120 / f124；记录必须已存在
            const auto third = read_int(ops[2]);
            const auto second = read_int(ops[1]);
            auto group = read_int(ops[0]);
            if (group == 0) group = default_group_;
            if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
            auto& rec = list_records_[static_cast<std::size_t>(group)];
            rec.f120 = second;
            rec.f124 = third;
            return;
        }
        case 0x212: {  // 记录已存在才写 f64
            auto group = read_int(ops[0]);
            if (group == 0) group = default_group_;
            if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
            list_records_[static_cast<std::size_t>(group)].f64 = read_int(ops[1]);
            return;
        }
        case 0x213: {  // 写 f68 / f6c
            const auto b = read_int(ops[2]);
            const auto a = read_int(ops[1]);
            auto group = read_int(ops[0]);
            if (group == 0) group = default_group_;
            if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
            auto& rec = list_records_[static_cast<std::size_t>(group)];
            rec.f68 = a;
            rec.f6c = b;
            return;
        }
        case 0x25D: {  // 写 f114 / f118
            const auto b = read_int(ops[2]);
            const auto a = read_int(ops[1]);
            auto group = read_int(ops[0]);
            if (group == 0) group = default_group_;
            if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
            auto& rec = list_records_[static_cast<std::size_t>(group)];
            rec.f114 = a;
            rec.f118 = b;
            return;
        }
        case 0x1C1: {  // 写列表项另一对字段（与 0x70 的第四对共享）
            const auto b = read_int(ops[2]);
            const auto a = read_int(ops[1]);
            auto group = read_int(ops[0]);
            if (group == 0) group = default_group_;
            if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
            auto& rec = list_records_[static_cast<std::size_t>(group)];
            rec.f24 = a;
            rec.f28 = b;
            return;
        }
        case 0x79: {  // 写列表项一对字段：读序 3 → 2 → 1
            const auto b = read_int(ops[2]);
            const auto a = read_int(ops[1]);
            auto group = read_int(ops[0]);
            if (group == 0) group = default_group_;
            if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
            auto& rec = list_records_[static_cast<std::size_t>(group)];
            rec.f1c = a;
            rec.f20 = b;
            return;
        }
        case 0x2DB:  // 字体对位模式 ← 操作数；重建入口未建模
            font_align_mode_ = read_int(ops[0]);
            return;
        case 0x78:  // 字体绘制模式 ← 操作数；重建入口未建模
            font_draw_mode_ = read_int(ops[0]);
            return;
        case 0x73: {  // 写列表记录十格 + 固定格
            const auto op10 = read_int(ops[9]);
            scalar_d107656_ = op10 == 0 ? 1 : op10;
            std::array<std::int32_t, 9> args{};
            for (int k = 9; k >= 1; --k) args[static_cast<std::size_t>(k - 1)] = read_int(ops[static_cast<std::size_t>(k - 1)]);
            auto group = args[0];
            if (group == 0) group = default_group_;
            if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
            auto& rec = list_records_[static_cast<std::size_t>(group)];
            rec.f3c = args[3];
            rec.f40 = args[4];
            rec.f44 = args[5];
            rec.f48 = as_i32(as_u32(args[4]) + as_u32(args[6]));
            rec.f4c = as_i32(as_u32(args[5]) + as_u32(args[7]));
            rec.f50 = args[1];
            rec.f54 = args[2];
            rec.f58 = 1;
            rec.f5c = args[8];
            rec.f60 = args[8];
            return;
        }
        case 0x71:  // 文本请求：写当前帧“本条位置”；文本子系统未建模
            frame_mut().position = static_cast<std::uint32_t>(frame_mut().ip / 4);
            return;
        case 0x80:  // 默认列表组 ← 第 1 个操作数（不归一、不夹取）
            default_group_ = read_int(ops[0]);
            return;
        case 0x70: {  // 设置列表项：组号 0 = 默认组；四对字段
            auto group = read_int(ops[0]);
            if (group == 0) group = default_group_;
            if (group < 0 || group >= 10) throw CoreError("list-group-out-of-range");
            const auto a3 = read_int(ops[1]);
            const auto a4 = read_int(ops[2]);
            const auto a5 = read_int(ops[3]);
            const auto a6 = read_int(ops[4]);
            auto& record = list_records_[static_cast<std::size_t>(group)];
            record.f0c = a5;
            record.f10 = a6;
            record.f14 = a3;
            record.f18 = a4;
            record.f24 = a3;
            record.f28 = a4;
            record.f7c = a3;
            record.f80 = a4;
            record.registered = 1;
            return;
        }
        case 0x32F: {  // 容器按索引清一格
            const auto index = read_int(ops[0]);
            forward_container_[static_cast<std::uint32_t>(index)] = 0;
            return;
        }
        case 0x324:  // 转发无参复位：接收者是引擎内部对象，当前不建模其字段
            return;
        case 0x1CA:  // 命名键 message:ReadTextSkip ← 第 1 个操作数
            named_message_read_text_skip_ = read_int(ops[0]);
            return;
        case 0x149:  // 薄标量写：原样
            scalar_149_ = read_int(ops[0]);
            return;
        case 0x21B:  // 薄标量写：归一成 0/1
            scalar_21b_ = read_int(ops[0]) == 0 ? 0 : 1;
            return;
        case 0x252:  // 薄标量写：原样
            scalar_252_ = read_int(ops[0]);
            return;
        case 0x1A7:  // comment：带一个操作数但不读它
            return;
        case 0x1A8:  // dev_ukn：无操作数 no-op
            return;
        case 0x2F8: {  // set-voice-volume：先读第 2 个（音量），再读第 1 个（组号）
            const auto volume = read_int(ops[1]);
            const auto group = read_int(ops[0]);
            if (group < 0 || group > 2) throw CoreError("voice-group-out-of-range");
            voice_volumes_[static_cast<std::size_t>(group)] =
                std::clamp<std::int32_t>(volume, -10000, 10000);
            return;
        }
        case 0x2F6: {  // stop-voice-group：无声音后端时只清引擎级 4 格并重算标志
            const auto group = read_int(ops[0]);
            if (group < 0 || group > 2) throw CoreError("voice-group-out-of-range");
            voice_engine_cells_[static_cast<std::size_t>(group)].fill(0);
            voice_playing_ = false;  // 后端不存在：不询问任何通道
            return;
        }
        case 0x05: {  // ret
            write_length(1);
            if (f.ret_stack.empty()) return;
            const auto position = f.ret_stack.back();
            f.ret_stack.pop_back();
            if (position == -1) return;
            jump_to(static_cast<std::uint32_t>(position));
            return;
        }
        case 0x50: {  // add：先读第 3 个、再读第 2 个
            const auto b = as_u32(read_int(ops[2]));
            const auto a = as_u32(read_int(ops[1]));
            write_int(ops[0], as_i32(a + b));
            return;
        }
        case 0x51: {  // sub：第 2 个 − 第 3 个；读取次序 3 → 2
            const auto b = as_u32(read_int(ops[2]));
            const auto a = as_u32(read_int(ops[1]));
            write_int(ops[0], as_i32(a - b));
            return;
        }
        case 0x52: {  // mul
            const auto b = as_u32(read_int(ops[2]));
            const auto a = as_u32(read_int(ops[1]));
            write_int(ops[0], as_i32(a * b));
            return;
        }
        case 0x53: {  // div，向零截断
            const auto divisor = read_int(ops[2]);
            if (divisor == 0) throw CoreError("divide-by-zero");
            write_int(ops[0], read_int(ops[1]) / divisor);
            return;
        }
        case 0x54: {  // mod，符号随被除数
            const auto divisor = read_int(ops[2]);
            if (divisor == 0) throw CoreError("mod-by-zero");
            write_int(ops[0], read_int(ops[1]) % divisor);
            return;
        }
        case 0x1A5: case 0x2FE: font_string_ = read_text(ops[0]); return;  // 两个字符串入口，内部未建模
        case 0x197: { const auto value = read_int(ops[0]); font_size_ = value; return; }
        case 0x75: font_size_ = read_int(ops[0]); return;  // 字体重建请求未建模
        case 0x2D5: write_float(ops[0], read_float(ops[1])); return;
        case 0x56: { const auto b = as_u32(read_int(ops[2])); const auto a = as_u32(read_int(ops[1])); write_int(ops[0], as_i32(a & b)); return; }
        case 0x57: { const auto b = as_u32(read_int(ops[2])); const auto a = as_u32(read_int(ops[1])); write_int(ops[0], as_i32(a | b)); return; }
        case 0x58: { const auto b = as_u32(read_int(ops[2])) & 31u; const auto a = read_int(ops[1]); write_int(ops[0], a >> b); return; }
        case 0x59: { const auto b = as_u32(read_int(ops[2])) & 31u; const auto a = read_int(ops[1]); write_int(ops[0], as_i32(as_u32(a) << b)); return; }
        case 0x5A: { const auto b = read_int(ops[2]); const auto a = read_int(ops[1]); write_int(ops[0], a == b ? 1 : 0); return; }
        case 0x5B: { const auto b = read_int(ops[2]); const auto a = read_int(ops[1]); write_int(ops[0], a != b ? 1 : 0); return; }
        case 0x5C: { const auto b = read_int(ops[2]); const auto a = read_int(ops[1]); write_int(ops[0], a < b ? 1 : 0); return; }
        case 0x5D: { const auto b = read_int(ops[2]); const auto a = read_int(ops[1]); write_int(ops[0], a <= b ? 1 : 0); return; }
        case 0x5E: { const auto b = read_int(ops[2]); const auto a = read_int(ops[1]); write_int(ops[0], a > b ? 1 : 0); return; }
        case 0x5F: { const auto b = read_int(ops[2]); const auto a = read_int(ops[1]); write_int(ops[0], a >= b ? 1 : 0); return; }
        case 0x135: { const auto bit = as_u32(read_int(ops[1])); if (bit > 31) return; const auto value = read_int(ops[0]); write_int(ops[0], as_i32(as_u32(value) | (1u << bit))); return; }
        case 0x136: { const auto bit = as_u32(read_int(ops[1])); if (bit > 31) return; const auto value = read_int(ops[0]); write_int(ops[0], as_i32(as_u32(value) & ~(1u << bit))); return; }
        case 0x13F: { const auto bit = as_u32(read_int(ops[2])); if (bit > 31) return; const auto value = as_u32(read_int(ops[1])); write_int(ops[0], static_cast<std::int32_t>((value >> bit) & 1u)); return; }
        case 0x55:  // mov（整型语义）
            write_int(ops[0], read_int(ops[1]));
            return;
        case 0x8c: {  // 无条件跳转
            write_length(3);
            const auto target = read_int(ops[0]);
            if (target != -1) jump_to(static_cast<std::uint32_t>(target));
            return;
        }
        case 0x8f: {  // call：压“下一条”的位置（本条位置 + 3）
            const auto target = read_int(ops[0]);
            if (target == -1) {
                write_length(3);
                return;
            }
            push_return(static_cast<std::uint32_t>(f.ip / 4 + 3));
            jump_to(static_cast<std::uint32_t>(target));
            return;
        }
        case 0xa0: {  // 条件跳转：只读命中的那一支
            const auto condition = read_int(ops[0]);
            const auto target = read_int(condition != 0 ? ops[1] : ops[2]);
            write_length(7);
            if (target != -1) jump_to(static_cast<std::uint32_t>(target));
            return;
        }
        default:
            throw CoreError(
                "opcode-not-implemented:0x" + [&] {
                    char buffer[16];
                    std::snprintf(buffer, sizeof(buffer), "%X", instruction.opcode);
                    return std::string(buffer);
                }());
    }
}

bool Machine::step() {
    if (halted_) return false;
    if (state_flags_ != 0) {
        // 只建模“场景不再有动画”这一路等待服务：清第 10 位后放行。
        if ((state_flags_ & (1u << 10)) != 0) state_flags_ &= ~(1u << 10);
        return true;
    }
    Frame& f = frame_mut();
    if (f.ip + 4 > f.code.size()) {
        halted_ = true;
        stop_.reason = StopReason::Error;
        stop_.offset = f.ip;
        stop_.message = "ip-past-code";
        return false;
    }

    const auto opcode = read_u32(f.code, f.ip);
    stop_.offset = f.ip;
    stop_.opcode = opcode;
    if (stop_at_opcode_ != 0 && opcode == stop_at_opcode_) {
        halted_ = true;
        stop_.reason = StopReason::StoppedAtOpcode;
        stop_.message = "stop-opcode";
        return false;
    }
    if (opcode > 0x3FF) {
        halted_ = true;
        stop_.reason = StopReason::BadOpcode;
        stop_.message = "opcode-out-of-domain:" + std::to_string(opcode);
        return false;
    }
    const OpcodeRow* row = table_.find(opcode);
    if (row == nullptr || !row->has_handler) {
        halted_ = true;
        stop_.reason = StopReason::UnsupportedOpcode;
        stop_.message = "opcode-without-handler:" + std::to_string(opcode);
        return false;
    }

    const std::size_t size = 4 + static_cast<std::size_t>(row->argc) * 8;
    if (f.ip + size > f.code.size()) {
        halted_ = true;
        stop_.reason = StopReason::Error;
        stop_.message = "instruction-past-code:" + std::to_string(size);
        return false;
    }

    Instruction instruction;
    instruction.offset = f.ip;
    instruction.opcode = opcode;
    instruction.operands.reserve(row->argc);
    for (std::uint32_t i = 0; i < row->argc; ++i) {
        instruction.operands.push_back(Operand{
            read_u32(f.code, f.ip + 4 + i * 8),
            read_u32(f.code, f.ip + 4 + i * 8 + 4),
        });
    }

    try {
        write_length(2 * row->argc + 1);
        execute(instruction);
    } catch (const std::exception& e) {
        halted_ = true;
        stop_.reason = StopReason::Error;
        stop_.message = e.what();
        return false;
    }

    if (!halted_) {
        const auto advance = 4u * frame_mut().length_slot;
        if (advance == 0) {
            // 控制转移已自行设置 ip；不额外推进。
        } else {
            frame_mut().ip += advance;
        }
    }
    return !halted_;
}

RunResult Machine::run(std::size_t max_steps) {
    std::size_t steps = 0;
    while (!halted_ && steps < max_steps) {
        if (!step()) {
            if (stop_.reason == StopReason::Halted) ++steps;
            break;
        }
        ++steps;
    }
    if (!halted_ && steps == max_steps) {
        stop_.reason = StopReason::StepLimit;
        stop_.message = "step-limit";
    }
    stop_.steps = steps;
    return stop_;
}

}  // namespace core
