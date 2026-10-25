#include "core/vm.h"

#include "core/agf.h"
#include "core/install.h"
#include "core/text_raster.h"

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

std::uint8_t clamp_byte(int value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0, 255));
}

// 会改场景（因而置脏、让下一次迭代渲染）的 opcode。按章归组；未实现的也先登记，
// 以后补 handler 时不会漏掉脏标记。
bool is_scene_mutating(std::uint32_t opcode) {
    switch (opcode) {
        case 0x1F7:  // 拆离绘制键
        case 0x1F8:  // 空白纹理
        case 0x1F9:  // 设置纹理
        case 0x1FA:  // 释放纹理
        case 0x1FB:  // 写绘制项
        case 0x1FD: case 0x1FE: case 0x1FF:  // 场景对象变换
        case 0x202: case 0x203:              // 绘制项颜色
        case 0x217: case 0x219: case 0x220:  // 3D 记录
        case 0x223: case 0x239: case 0x242:
        case 0x24D: case 0x24F: case 0x250: case 0x251:  // 场景命令表
        case 0x229: case 0x230: case 0x231: case 0x233: case 0x234: case 0x235:
        case 0x23B:                          // CG 数字条
        case 0x258: case 0x259:              // 1000 项表
        case 0x320: case 0x322: case 0x323:  // 网格
        case 0x344:                          // 辅助表 A
            return true;
        default:
            return false;
    }
}

// ARGB 逐通道插值（截断）。`t` 可以越出 [0,1]：规范只判上界，负延迟 / 未夹取的进度会外推。
std::uint32_t lerp_argb(std::uint32_t from, std::uint32_t to, double t) {
    const auto channel = [&](int shift) {
        const double a = static_cast<double>((from >> shift) & 0xFFu);
        const double b = static_cast<double>((to >> shift) & 0xFFu);
        const int value = static_cast<int>(a + (b - a) * t);
        return static_cast<std::uint32_t>(std::clamp(value, 0, 255));
    };
    return (channel(24) << 24) | (channel(16) << 16) | (channel(8) << 8) | channel(0);
}
}  // namespace

Machine::Machine(std::uint32_t int_key, const OpcodeTable& table, const Install* install,
                 ConfigStore config)
    : key_(int_key == 0 ? 1u : int_key), table_(table), install_(install), config_(std::move(config)) {
    // 主循环的渲染闸门：`set:DrawMode == 1` 才自动渲染。缺键按配置口径为 0（不自动渲染）。
    draw_mode_ = config_.get_int("set:DrawMode");
}

std::uint32_t Machine::encode_int(std::int32_t value) const {
    return rol32(ror32(as_u32(value), 7) ^ key_, 21);
}

std::int32_t Machine::decode_int(std::uint32_t bits) const {
    return as_i32(ror32(rol32(bits, 11) ^ key_, 25));
}

void Machine::advance_scene(std::uint32_t now_ms) {
    scene_previous_ms_ = scene_clock_ms_;
    scene_clock_ms_ = now_ms;
    scene_animating_ = false;

    // 共享推进：盖章 → 延迟内 / 推进中 → 到期收尾（`CORE-TIMING-WINDOW`）。
    // 返回本步派生色（只写进出参，不写记录字段；到期那一步才写当前颜色）。
    const auto advance_window = [&](std::uint32_t& flags, std::uint32_t& current,
                                    std::uint32_t& target, std::int32_t& delay,
                                    std::int32_t& duration, std::uint32_t& start,
                                    bool keep_window = false) {
        if ((flags & 2u) == 0) return current;
        if (duration <= 0) return current;  // 不盖章 / 不插值 / 不置动画 / 不到期
        if (start == 0) start = now_ms;
        if (now_ms <= start + static_cast<std::uint32_t>(delay) +
                          static_cast<std::uint32_t>(duration)) {
            scene_animating_ = true;
            if (now_ms <= start + static_cast<std::uint32_t>(delay)) return current;
            const auto elapsed = now_ms - start - static_cast<std::uint32_t>(delay);
            const auto progress = static_cast<double>(elapsed) / static_cast<double>(duration);
            return lerp_argb(current, target, progress);
        }
        // 到期：当前颜色 ← 终色；终色 ← 无颜色；延迟 / 持续时间清 0；清「有窗」位与起点。
        current = target;
        target = kNoColor;
        delay = 0;
        duration = 0;
        if (!keep_window) {
            flags &= ~2u;
            start = 0;
        }
        return current;
    };

    for (auto& [key, item] : draw_items_) {
        (void)key;
        if ((item.flags & 2u) != 0 && item.window_start == 0 &&
            (item.duration > 0 || item.rect_duration > 0)) {
            item.window_start = now_ms;  // 源矩形窗与颜色窗共用一次盖章
        }
        const bool rect_active = (item.flags & 2u) != 0 && item.rect_duration > 0;
        const bool rect_expired =
            rect_active && now_ms > item.window_start + static_cast<std::uint32_t>(item.rect_delay) +
                                         static_cast<std::uint32_t>(item.rect_duration);
        (void)advance_window(item.flags, item.color, item.target_color, item.delay, item.duration,
                             item.window_start, rect_active && !rect_expired);
        if (rect_expired) {
            // 到期那一档把结果写回条目的四条边（末帧或空矩形）。
            const int width = std::max(0, item.right - item.left);
            const int height = std::max(0, item.bottom - item.top);
            if ((item.rect_switch & 1) != 0 && item.rect_row_steps > 0 &&
                item.rect_step_total > 0) {
                const std::int64_t step = item.rect_step_total - 1;
                const int dx = width * static_cast<int>(step % item.rect_row_steps);
                const int dy = height * static_cast<int>(step / item.rect_row_steps);
                item.left += dx;
                item.right += dx;
                item.top += dy;
                item.bottom += dy;
            } else {
                item.right = item.left;
                item.bottom = item.top;
            }
            item.rect_delay = 0;
            item.rect_duration = 0;
            if (item.duration <= 0) {
                item.flags &= ~2u;
                item.window_start = 0;
            }
        }
    }
    for (auto& [key, mesh] : meshes_) {
        (void)key;
        const bool had_window = (mesh.flags & 2u) != 0;
        const auto derived = advance_window(mesh.flags, mesh.color, mesh.target_color, mesh.delay,
                                            mesh.duration, mesh.window_start);
        // 网格只在被推进的那一步按派生色重烘顶点缓冲（`CORE-TIMING-WINDOW`）。
        if (had_window && !mesh.colors.empty()) {
            for (auto& value : mesh.colors) value = derived;
        }
    }
    if (scene_animating_) scene_dirty_ = true;  // 有动画 ⇒ 下一迭代重绘
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
    scene_dirty_ = true;
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
        case 0xBF: (void)read_int(ops[0]); state_flags_ &= ~(1u << 9); return;  // BGM 请求；配置音量路径未建模
        case 0xB4: (void)read_int(ops[0]); (void)read_int(ops[1]); return;  // 音频装载请求
        case 0xB5: (void)read_int(ops[0]); return;  // 音频播放请求
        case 0x2C7: return;  // ADV 文字工具请求（内部未建模）
        case 0x341: case 0x345: case 0x34E: case 0x352: return;  // Live2D 请求边界（内部未建模）
        case 0x2EC: write_int(ops[0], parse_int_text(read_text(ops[1]))); return;  // 十进制文本转整数
        case 0x2EB: write_text(ops[0], config_.get_string("set:GameVersion")); return;  // 配置版本写进字符串记录
        case 0x23B: {  // CG 数字条：建绘制项（像素输出未建模）
            const auto id = read_int(ops[1]);
            if (id < 0 || id > 10) return;
            if (id < 10 && cg_params_[static_cast<std::size_t>(id)][0] == 0) return;
            const auto start = read_int(ops[0]);
            const auto value = read_int(ops[2]);
            const auto digits = read_int(ops[5]);
            const auto flags = read_int(ops[6]);
            if (digits > 0) {
                for (std::int32_t i = 0; i < digits; ++i) draw_items_.erase(static_cast<std::uint32_t>(start + i));
            }
            if (digits <= 0) return;
            const auto target_x = read_int(ops[3]);
            const auto target_y = read_int(ops[4]);
            const auto& cg = cg_params_[static_cast<std::size_t>(id)];
            const std::int32_t cell = cg[3] + cg[6];
            const std::int32_t number_digits = value == 0 ? 1 : static_cast<std::int32_t>(std::to_string(std::abs(value)).size());
            for (std::int32_t j = 0; j < digits; ++j) {
                const auto digit = static_cast<std::int32_t>((std::abs(value) / static_cast<std::int32_t>(std::pow(10, j))) % 10);
                const std::int32_t index = digits - 1 - j;
                std::int32_t x = target_x + index * cell;
                if ((flags & 2) != 0) x -= (digits - number_digits) * cell;
                else if ((flags & 4) != 0) x -= 0;
                DrawItem& item = draw_items_[static_cast<std::uint32_t>(start + j)];
                item.flags |= 1u;
                item.texture_id = static_cast<std::uint32_t>(cg[0]);
                item.left = cg[1] + cell * digit;
                item.top = cg[2];
                item.right = item.left + cg[3];
                item.bottom = item.top + cg[4];
                item.aux[0] = static_cast<float>(x);
                item.aux[1] = static_cast<float>(target_y);
                item.aux[2] = 0.0f;
            }
            return;
        }
        case 0x109: write_int(ops[0], -100000); write_int(ops[1], -100000); return;  // 无窗口：光标不可用
        case 0x108: write_int(ops[0], 0); return;  // 无窗口：按钮位图 0
        case 0xC8: { const auto ms = read_int(ops[0]); if (ms >= 10) state_flags_ |= 1u; return; }  // 计时器请求；短睡眠按无操作
        case 0xCC: mouse_interval_ = read_int(ops[0]); if (mouse_interval_ == 0) mouse_interval_ = 1; mouse_position_ = read_int(ops[1]); return;
        case 0xCD: return;  // 鼠标回调时间闸：无时钟/无回调时不派发
        case 0xFB: {  // 登记按键回调
            const auto key = read_int(ops[0]);
            const auto position = read_int(ops[1]);
            if (key < 0 || key > 31) throw CoreError("key-register-out-of-range");
            frame_mut().key_jumps[key] = position;
            return;
        }
        case 0x24E: input_mode_ = static_cast<std::uint32_t>(read_int(ops[0])); return;  // 主循环模式字
        case 0x2DA: {  // CG 参数记录七格
            const auto id = read_int(ops[0]);
            if (id < 0 || id > 10) return;
            for (std::size_t i = 0; i < 7; ++i) cg_params_[static_cast<std::size_t>(id)][i] = read_int(ops[i + 1]);
            return;
        }
        case 0x344: {  // 辅助表 A（Live2D 条目不透明桶）
            const auto value = read_int(ops[1]);
            const auto key = read_int(ops[0]);
            aux_a_[key] = {1, value};
            return;
        }
        case 0x1FF: {  // 平移矩阵
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            const auto x = read_float(ops[1]);
            const auto y = read_float(ops[2]);
            const auto z = read_float(ops[3]);
            SceneRecord& rec = scene_records_[key];
            rec.translate[0] = static_cast<float>(x);
            rec.translate[1] = static_cast<float>(y);
            rec.translate[2] = static_cast<float>(z);
            return;
        }
        case 0x1FD: {  // 缩放矩阵：百分之一
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            const auto x = read_float(ops[1]) / 100.0;
            const auto y = read_float(ops[2]) / 100.0;
            const auto z = read_float(ops[3]) / 100.0;
            SceneRecord& rec = scene_records_[key];
            rec.scale[0] = static_cast<float>(x);
            rec.scale[1] = static_cast<float>(y);
            rec.scale[2] = static_cast<float>(z);
            return;
        }
        case 0x1FE: {  // 旋转矩阵：轴 + 角度（度）
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            const auto ax = read_float(ops[1]);
            const auto ay = read_float(ops[2]);
            const auto az = read_float(ops[3]);
            const auto degrees = read_float(ops[4]);
            SceneRecord& rec = scene_records_[key];
            rec.rotate_axis[0] = static_cast<float>(ax);
            rec.rotate_axis[1] = static_cast<float>(ay);
            rec.rotate_axis[2] = static_cast<float>(az);
            rec.rotate_degrees = static_cast<float>(degrees);
            return;
        }
        case 0x20B: {  // 纹理矩形填充：夹到逻辑矩形，颜色 = (颜色 & 0xFFFFFF) | 0xFF000000
            const auto id = static_cast<std::uint32_t>(read_int(ops[0]));
            const auto x = read_int(ops[1]);
            const auto y = read_int(ops[2]);
            const auto w = read_int(ops[3]);
            const auto h = read_int(ops[4]);
            auto alpha = read_int(ops[5]);
            const auto color = as_u32(read_int(ops[6])) & 0xFFFFFFu;
            if (alpha > 255) alpha = 255;
            if (id > 999u) return;
            auto it = textures_.find(id);
            if (it == textures_.end()) return;
            TextureRecord& texture = it->second;
            texture.non_file = true;
                texture.revision++;
            if (texture.rgba.empty()) return;
            const int left = std::max(0, x);
            const int top = std::max(0, y);
            const int right = std::min(static_cast<int>(texture.width), x + w);
            const int bottom = std::min(static_cast<int>(texture.height), y + h);
            if (right <= left || bottom <= top) return;
            const int a = std::max(0, alpha);
            const int sr = static_cast<int>((color >> 16) & 0xFFu);
            const int sg = static_cast<int>((color >> 8) & 0xFFu);
            const int sb = static_cast<int>(color & 0xFFu);
            for (int py = top; py < bottom; ++py) {
                for (int px = left; px < right; ++px) {
                    const std::size_t d = (static_cast<std::size_t>(py) * texture.width + px) * 4;
                    texture.rgba[d + 0] = clamp_byte((sr * a + texture.rgba[d + 0] * (255 - a)) / 255);
                    texture.rgba[d + 1] = clamp_byte((sg * a + texture.rgba[d + 1] * (255 - a)) / 255);
                    texture.rgba[d + 2] = clamp_byte((sb * a + texture.rgba[d + 2] * (255 - a)) / 255);
                    texture.rgba[d + 3] = clamp_byte(a + texture.rgba[d + 3] * (255 - a) / 255);
                }
            }
            return;
        }
        case 0x20C: {  // 帧步进：时间戳 + 一次渲染请求 + 清帧标志
            if (!driven_elsewhere_) {
                engine_previous_ms_ = engine_now_ms_;
                engine_now_ms_ = scene_clock_ms_;
            }
            frame_step_requested_ = true;
            frame_flag_ = false;
            return;
        }
        case 0x20D: {  // 渲染目标：<1000 纹理槽，>=1000 后缓冲
            const auto id = read_int(ops[0]);
            if (static_cast<std::uint32_t>(id) < 1000u) {
                if (textures_.find(static_cast<std::uint32_t>(id)) != textures_.end()) render_target_ = id;
            } else {
                render_target_ = -1;
            }
            return;
        }
        case 0x33F: {  // 场景混合：alpha 先读、color 再读、mode 最后
            auto alpha = read_int(ops[1]);
            auto color = read_int(ops[2]);
            const auto mode = read_int(ops[0]);
            const auto key = static_cast<std::uint32_t>(mode);
            const auto item = draw_items_.find(key);
            if (alpha < 0) alpha = item == draw_items_.end() ? 255 : static_cast<std::int32_t>((item->second.color >> 24) & 0xFFu);
            if (color < 0) color = item == draw_items_.end() ? static_cast<std::int32_t>(0xFFFFFFFFu) : static_cast<std::int32_t>(item->second.color);
            if (alpha > 255) alpha = 255;
            blend_mode_ = mode;
            blend_color_ = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
            return;
        }
        case 0x204: {  // 离屏文字：读序 3 → 2 → 4 → 1；三条入口条件在请求之前
            const auto y = read_int(ops[2]);
            const auto x = read_int(ops[1]);
            const auto text = read_text(ops[3]);
            const auto id = read_int(ops[0]);
            if (id < 0) return;
            const auto texture_id = static_cast<std::uint32_t>(id);
            const TextureRecord* texture = this->texture(texture_id);
            if (texture == nullptr || texture->rgba.empty() || text.empty()) return;
            std::string error;
            if (draw_text_into_texture(*this, texture_id, x, y, text, &error)) {
                if (TextureRecord* mutable_texture = texture_mut(texture_id)) mutable_texture->revision++;
            } else if (!error.empty()) {
                report_error(error);
            }
            return;
        }
        case 0x1F6: draw_items_.clear(); meshes_.clear(); aux_a_.clear(); aux_b_.clear(); scene_dirty_ = false; return;  // 清四张同键表；脏标记清 0（不是置 1）
        case 0x20F: {  // play-movie：建对象、绑定门、启动后端（headless 立即收尾）
            const auto slot = read_int(ops[1]);
            const auto file_id = read_int(ops[0]);
            (void)read_int(ops[0]);
            const auto options = read_int(ops[2]);
            if (textures_.find(static_cast<std::uint32_t>(slot)) == textures_.end()) {
                throw CoreError("movie-texture-slot-empty:" + std::to_string(slot));
            }
            MovieRecord& movie = movies_[static_cast<std::uint32_t>(slot)];
            movie.present = true;
            movie.started = true;
            movie.file_id = static_cast<std::uint32_t>(file_id);
            movie.options = static_cast<std::uint32_t>(options);
            movie_active_ = true;
            state_flags_ |= (1u << 13);
            return;
        }
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
        case 0x223: {  // 种类 0：纹理覆盖命令（先建 / 换纹理请求，再替换记录）
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            const auto texture_id = static_cast<std::uint32_t>(read_int(ops[1]));
            const auto a_start = read_int(ops[2]);
            const auto a_count = read_int(ops[3]);
            const auto b_start = read_int(ops[4]);
            const auto b_count = read_int(ops[5]);
            const auto delay = read_int(ops[6]);
            const auto duration = read_int(ops[7]);
            if (textures_.find(texture_id) == textures_.end()) {
                TextureRecord& texture = textures_[texture_id];
                texture.blank = true;
                texture.non_file = true;
                texture.revision++;
                texture.present = true;
                texture.width = 1280;
                texture.height = 720;
                texture.rgba.assign(static_cast<std::size_t>(texture.width) * texture.height * 4, 0);
            }
            SceneCommandRecord& record = scene_commands_[key];
            record = SceneCommandRecord{};
            record.kind = 0;
            record.key = key;
            record.texture_id = texture_id;
            record.a_start = a_start;
            record.a_count = a_count;
            record.b_start = b_start;
            record.b_count = b_count;
            record.delay = delay;
            record.duration = duration;
            return;
        }
        case 0x24D: {  // 种类 1：影片覆盖命令；第 2 个操作数必须是已存在的影片纹理
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            const auto texture_id = static_cast<std::uint32_t>(read_int(ops[1]));
            const auto a_start = read_int(ops[2]);
            const auto a_count = read_int(ops[3]);
            const auto left = read_int(ops[4]);
            const auto top = read_int(ops[5]);
            const auto width = read_int(ops[6]);
            const auto height = read_int(ops[7]);
            const auto mask_flag = read_int(ops[8]);
            (void)read_int(ops[9]);  // 影片名表索引（不写记录）
            const auto delay = read_int(ops[10]);
            const auto duration = read_int(ops[11]);
            if (textures_.find(texture_id) == textures_.end()) {
                throw CoreError("movie-texture-slot-empty:" + std::to_string(texture_id));
            }
            SceneCommandRecord& record = scene_commands_[key];
            record = SceneCommandRecord{};
            record.kind = 1;
            record.key = key;
            record.texture_id = texture_id;
            record.a_start = a_start;
            record.a_count = a_count;
            record.delay = delay;
            record.duration = duration;
            record.rect[0] = left;
            record.rect[1] = top;
            record.rect[2] = left + width;
            record.rect[3] = top + height;
            record.mask_flag = mask_flag;
            command_animating_ = true;  // 写表时置「有动画」
            return;
        }
        case 0x24F: {  // 种类 2：抹除命令（先校验、后写表）
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            const auto texture_id = static_cast<std::uint32_t>(read_int(ops[1]));
            auto a_start = read_int(ops[2]);
            auto a_count = read_int(ops[3]);
            auto b_start = read_int(ops[4]);
            auto b_count = read_int(ops[5]);
            auto wipe_type = read_int(ops[6]);
            auto split = read_int(ops[7]);
            auto delay = read_int(ops[8]);
            auto duration = read_int(ops[9]);
            if (split < 1) {
                report_error("分割幅が０以下です");
                split = 1;
                wipe_type = -1;
                delay = 0;
                duration = 0;
            } else if (wipe_type < 0 || wipe_type > 11) {
                report_error("タイプが不正です");
                return;  // 一个字段都不写
            }
            if (textures_.find(texture_id) == textures_.end()) {
                TextureRecord& texture = textures_[texture_id];
                texture.blank = true;
                texture.non_file = true;
                texture.revision++;
                texture.present = true;
                texture.width = 1280;
                texture.height = 720;
                texture.rgba.assign(static_cast<std::size_t>(texture.width) * texture.height * 4, 0);
            }
            SceneCommandRecord& record = scene_commands_[key];
            record = SceneCommandRecord{};
            record.kind = 2;
            record.key = key;
            record.texture_id = texture_id;
            record.a_start = a_start;
            record.a_count = a_count;
            record.b_start = b_start;
            record.b_count = b_count;
            record.wipe_type = wipe_type;
            record.split = split;
            record.delay = delay;
            record.duration = duration;
            return;
        }
        case 0x250: {  // 种类 3 变体 0：滑移模糊（长度 / 角度）
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            const auto texture_id = static_cast<std::uint32_t>(read_int(ops[1]));
            const auto a_start = read_int(ops[2]);
            const auto a_count = read_int(ops[3]);
            const auto from_length = read_int(ops[4]);
            const auto from_angle = read_int(ops[5]);
            const auto to_length = read_int(ops[6]);
            const auto to_angle = read_int(ops[7]);
            const auto delay = read_int(ops[8]);
            const auto duration = read_int(ops[9]);
            if (textures_.find(texture_id) == textures_.end()) {
                TextureRecord& texture = textures_[texture_id];
                texture.blank = true;
                texture.non_file = true;
                texture.revision++;
                texture.present = true;
                texture.width = 1280;
                texture.height = 720;
                texture.rgba.assign(static_cast<std::size_t>(texture.width) * texture.height * 4, 0);
            }
            SceneCommandRecord& record = scene_commands_[key];
            record = SceneCommandRecord{};
            record.kind = 3;
            record.key = key;
            record.texture_id = texture_id;
            record.a_start = a_start;
            record.a_count = a_count;
            record.delay = delay;
            record.duration = duration;
            record.variant = 0;
            record.from[0] = from_length;
            record.to[0] = to_length;
            record.from[1] = from_angle;
            record.to[1] = to_angle;
            return;
        }
        case 0x251: {  // 种类 3 变体 1：缩放模糊（长度 / 中心横 / 中心纵）
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            const auto texture_id = static_cast<std::uint32_t>(read_int(ops[1]));
            const auto a_start = read_int(ops[2]);
            const auto a_count = read_int(ops[3]);
            const auto from_length = read_int(ops[4]);
            const auto from_cx = read_int(ops[5]);
            const auto from_cy = read_int(ops[6]);
            const auto to_length = read_int(ops[7]);
            const auto to_cx = read_int(ops[8]);
            const auto to_cy = read_int(ops[9]);
            const auto delay = read_int(ops[10]);
            const auto duration = read_int(ops[11]);
            if (textures_.find(texture_id) == textures_.end()) {
                TextureRecord& texture = textures_[texture_id];
                texture.blank = true;
                texture.non_file = true;
                texture.revision++;
                texture.present = true;
                texture.width = 1280;
                texture.height = 720;
                texture.rgba.assign(static_cast<std::size_t>(texture.width) * texture.height * 4, 0);
            }
            SceneCommandRecord& record = scene_commands_[key];
            record = SceneCommandRecord{};
            record.kind = 3;
            record.key = key;
            record.texture_id = texture_id;
            record.a_start = a_start;
            record.a_count = a_count;
            record.delay = delay;
            record.duration = duration;
            record.variant = 1;
            record.from[0] = from_length;
            record.to[0] = to_length;
            record.from[1] = from_cx;
            record.to[1] = to_cx;
            record.from[2] = from_cy;
            record.to[2] = to_cy;
            return;
        }
        case 0x320: {  // 建/换网格：保存逐顶点位置 / 颜色
            const auto count = read_int(ops[8]);
            const auto op10 = read_int(ops[9]);
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            if (count <= 0) return;
            MeshRecord& mesh = meshes_[key];
            mesh.flags |= 1u;
            mesh.count = count;
            mesh.op10 = op10;
            mesh.copies[0].clear();
            mesh.copies[1].clear();
            mesh.colors.clear();
            const auto address0 = address_of(ops[1]);
            const auto address1 = address_of(ops[2]);
            const auto address2 = address_of(ops[4]);
            const auto address3 = address_of(ops[5]);
            for (std::int32_t i = 0; i < count; ++i) {
                const auto step = static_cast<std::uint32_t>(i) * 4u;
                mesh.copies[0].push_back(read_raw(address0 + step));
                mesh.copies[1].push_back(read_raw(address1 + step));
                const auto high = as_u32(read_int_at(address2 + step)) & 0xFFu;
                const auto low = as_u32(read_int_at(address3 + step)) & 0xFFFFFFu;
                mesh.colors.push_back((high << 24) | low);
            }
            return;
        }
        case 0x322: {  // 网格当前颜色
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            (void)read_int(ops[1]);
            const auto alpha = read_int(ops[2]);
            const auto color = read_int(ops[3]);
            MeshRecord& mesh = meshes_[key];
            mesh.color = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
            mesh.target_color = mesh.color;
            for (auto& value : mesh.colors) value = mesh.color;
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
            mesh.target_color = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
            mesh.window_start = 0;  // 未盖章
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
        case 0x239: {  // 源矩形窗口：读序 6 → 5 → 4 → 3 → 2 → 1；「已存在」位为 0 ⇒ 一个字段都不写
            const auto rect_switch = read_int(ops[5]);
            const auto row_steps = read_int(ops[4]);
            const auto step_total = read_int(ops[3]);
            const auto duration = read_int(ops[2]);
            const auto delay = read_int(ops[1]);
            const auto key = static_cast<std::uint32_t>(read_int(ops[0]));
            DrawItem& item = draw_items_[key];
            if ((item.flags & 1u) == 0) return;
            item.flags |= 2u;
            item.window_start = 0;
            item.rect_delay = delay;
            item.rect_duration = duration;
            item.rect_step_total = step_total;
            item.rect_row_steps = row_steps;
            item.rect_switch = rect_switch;
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
                it->second.target_color = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(color) & 0xFFFFFFu);
                it->second.window_start = 0;  // 未盖章
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
            record.revision++;
            record.width = static_cast<std::uint32_t>(width);
            record.height = static_cast<std::uint32_t>(height);
            record.rgba.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4, 0);
            record.revision = 1;
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
            record.rgba = image.rgba;
            if (record.color_key != 0) {
                for (std::size_t i = 0; i + 3 < record.rgba.size(); i += 4) {
                    const auto argb = (static_cast<std::uint32_t>(record.rgba[i + 3]) << 24) |
                                      (static_cast<std::uint32_t>(record.rgba[i]) << 16) |
                                      (static_cast<std::uint32_t>(record.rgba[i + 1]) << 8) |
                                      record.rgba[i + 2];
                    if (argb == record.color_key) {
                        record.rgba[i] = 0; record.rgba[i + 1] = 0; record.rgba[i + 2] = 0; record.rgba[i + 3] = 0;
                    }
                }
            }
            record.present = true;
            record.revision++;
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
    // 每次迭代先读一次时钟、推进场景：颜色窗在门之前推进（`CORE-TIMING-WINDOW`）。
    advance_scene(external_clock_ ? host_clock_ms_ : scene_clock_ms_ + 16);
    // 门前渲染：主循环闸门（0x412739–0x4127E2）先要求
    // `(0x21B != 0) || (状态字 & 0x2400)`，脏只是更内层条件。
    const bool render_gate = scalar_21b_ != 0 || (state_flags_ & (0x400u | 0x2000u)) != 0;
    if (draw_mode_ == 1 && render_gate && (state_flags_ & (1u << 24)) == 0 &&
        (scene_dirty_ || scene_animating_ || command_animating_)) {
        frame_step_requested_ = true;
    }
    if (state_flags_ != 0) {
        // 影片位：headless 影片后端立即收尾，扫描对象表后放行。
        if ((state_flags_ & 1u) != 0) { state_flags_ &= ~1u; return true; }
        if ((state_flags_ & (1u << 13)) != 0) {
            for (auto& [slot, movie] : movies_) { (void)slot; movie.started = false; }
            movie_active_ = false;
            state_flags_ &= ~(1u << 13);
            return true;
        }
        // 等待位：放行看「场景有动画」，不是"表里还有没有未到期的窗"。
        if ((state_flags_ & (1u << 10)) != 0 && !scene_animating_) state_flags_ &= ~(1u << 10);
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
    if (is_scene_mutating(opcode)) scene_dirty_ = true;

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

RunResult Machine::run_until_script(std::uint32_t script_id, std::size_t max_steps) {
    std::size_t steps = 0;
    while (!halted_ && steps < max_steps) {
        if (frame().script_id == script_id) {
            stop_.reason = StopReason::ScriptReached;
            stop_.message = "script-reached";
            break;
        }
        if (!step()) {
            if (stop_.reason == StopReason::Halted) ++steps;
            break;
        }
        ++steps;
    }
    if (!halted_ && steps == max_steps && stop_.reason != StopReason::ScriptReached) {
        stop_.reason = StopReason::StepLimit;
        stop_.message = "step-limit";
    }
    stop_.steps = steps;
    return stop_;
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
