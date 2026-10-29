// 分派 + 主循环（70-vm）。★ 指令实现不在这个文件：一个 opcode 一个函数，按族分在
// `vm_ops_*.cpp`（影片 0x20F 在 `vm_movie.cpp`）；值/池原语在 `vm_value.cpp`；计时窗在 `vm_timing.cpp`。
// 这个文件只保留：分派表（一个 opcode 一行）、帧协议（长度槽 / 跳转 / 返回栈）、
// `is_scene_mutating`（脏白名单）、构造、`step()` / `run()`。
#include "core/vm.h"

#include <cstdio>

namespace core {
namespace {
// 会改场景（因而置脏、让下一次迭代渲染）的 opcode。按章归组；未实现的也先登记，
// 以后补 handler 时不会漏掉脏标记。
bool is_scene_mutating(std::uint32_t opcode) {
    switch (opcode) {
        // 0x1F7（拆离）不在这里：它只在真的删掉条目时才标脏，由处理器自己写。
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
}  // namespace

Machine::Machine(std::uint32_t int_key, const OpcodeTable& table, const Install* install,
                 ConfigStore config)
    : key_(int_key == 0 ? 1u : int_key), table_(table), install_(install), config_(std::move(config)) {
    // 主循环的渲染闸门：`set:DrawMode == 1` 才自动渲染。缺键按配置口径为 0（不自动渲染）。
    draw_mode_ = config_.get_int("set:DrawMode");
    // 参数格（`Engine+0x15290`，`0x74` 写 / `0x7f` 读）的初值 = 配置键 `message:MessageSpeed`
    // —— 启动期有确定写点：`0x417E98` 取键、`0x417EAB` 写格（取证见 ops-forensics 文档 §6）。
    // ★ 这解决了 spec 的 `RES-PARAM-21668-INIT`（"初值没有观察"）：它不是分配残留，是配置。
    param_cell_ = config_.get_int("message:MessageSpeed");
    // 画面模式缓存（`Engine+0xA40D8`）的初值 = 配置 `display:ScreenMode`（引擎启动时由该键写入）。
    display_mode_cache_ = config_.get_int("display:ScreenMode");
    // 启动期登记：键非 0 才认为"备组已分配"（发行数据里该键恒 0 ⇒ 只有异常支可达）。
    memflip_available_ = config_.get_int("set:EnableMemFlip") != 0;
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
    root.local_depth = static_cast<std::int32_t>(script_id);  // 本帧的身份编号（本地块深度判据用）
    current_frame_ = 0;
    halted_ = false;
    scene_dirty_ = true;
    stop_ = {};
}

Frame& Machine::frame_mut() {
    return frames_[current_frame_];
}

void Machine::note_missing_opcode(std::uint32_t opcode) {
    for (auto& entry : missing_opcodes_) {
        if (entry.first == opcode) {
            entry.second += 1;
            return;
        }
    }
    missing_opcodes_.emplace_back(opcode, 1);
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

void Machine::execute(const Instruction& instruction) {
    switch (instruction.opcode) {
        case 0x01: op_01(instruction); return;
        case 0x02: op_02(instruction); return;
        case 0x88: op_88(instruction); return;
        case 0xC0: op_c0(instruction); return;
        case 0x142: op_142(instruction); return;
        case 0x12F: op_12f(instruction); return;
        case 0x2CC: op_2cc(instruction); return;
        case 0x2CD: op_2cd(instruction); return;
        case 0x2CE: op_2ce(instruction); return;
        case 0x2CF: op_2cf(instruction); return;
        case 0x307: op_307(instruction); return;
        case 0x2E6: op_2e6(instruction); return;
        case 0x2E7: op_2e7(instruction); return;
        case 0x2E8: op_2e8(instruction); return;
        case 0x2EA: op_2ea(instruction); return;
        case 0xC5: op_c5(instruction); return;
        case 0xC6: op_c6(instruction); return;
        case 0xC7: op_c7(instruction); return;
        case 0x306: op_306(instruction); return;
        case 0x10D: op_10d(instruction); return;
        case 0x1B5: op_1b5(instruction); return;
        case 0x1B8: op_1b8(instruction); return;
        case 0x1B9: op_1b9(instruction); return;
        case 0x1BA: op_1ba(instruction); return;
        case 0xB6: op_b6(instruction); return;
        case 0xB7: op_b7(instruction); return;
        case 0xB8: op_b8(instruction); return;
        case 0xBA: op_ba(instruction); return;
        case 0x1BB: op_1bb(instruction); return;
        case 0x1A6: op_1a6(instruction); return;
        case 0x300: op_300(instruction); return;
        case 0x301: op_301(instruction); return;
        case 0x1D2: op_1d2(instruction); return;
        case 0x1D3: op_1d3(instruction); return;
        case 0x6E: op_6e(instruction); return;
        case 0x6F: op_6f(instruction); return;
        case 0x196: op_196(instruction); return;
        case 0x03: op_03(instruction); return;
        case 0x09: op_09(instruction); return;
        case 0x06: op_06(instruction); return;
        case 0x08: op_08(instruction); return;
        case 0x6C: op_6c(instruction); return;
        case 0x2D8: op_2d8(instruction); return;
        case 0x61: op_61(instruction); return;
        case 0x60: op_60(instruction); return;
        case 0x63: op_63(instruction); return;
        case 0x6D: op_6d(instruction); return;
        case 0x74: op_74(instruction); return;
        case 0x7B: op_7b(instruction); return;
        case 0x7C: op_7c(instruction); return;
        case 0x7F: op_7f(instruction); return;
        case 0xD0: op_d0(instruction); return;
        case 0x191: op_191(instruction); return;
        case 0x199: op_199(instruction); return;
        case 0x19A: op_19a(instruction); return;
        case 0x1AD: op_1ad(instruction); return;
        case 0x19E: op_19e(instruction); return;
        case 0x1A0: op_1a0(instruction); return;
        case 0x1A1: op_1a1(instruction); return;
        case 0x1AB: op_1ab(instruction); return;
        case 0x1AC: op_1ac(instruction); return;
        case 0x1AE: op_1ae(instruction); return;
        case 0x1AF: op_1af(instruction); return;
        case 0x1B0: op_1b0(instruction); return;
        case 0x1B6: op_1b6(instruction); return;
        case 0x1B7: op_1b7(instruction); return;
        case 0x1C7: op_1c7(instruction); return;
        case 0x1CC: op_1cc(instruction); return;
        case 0x218: op_218(instruction); return;
        case 0x217: op_217(instruction); return;
        case 0x21A: op_21a(instruction); return;
        case 0x21D: op_21d(instruction); return;
        case 0x21E: op_21e(instruction); return;
        case 0x21F: op_21f(instruction); return;
        case 0x228: op_228(instruction); return;
        case 0x2D0: op_2d0(instruction); return;
        case 0x2D1: op_2d1(instruction); return;
        case 0x2D3: op_2d3(instruction); return;
        case 0x12C: op_12c(instruction); return;
        case 0x64: op_64(instruction); return;
        case 0x10C: op_10c(instruction); return;
        case 0x308: op_308(instruction); return;
        case 0xFE: op_fe(instruction); return;
        case 0xBF: op_bf(instruction); return;
        case 0xB4: op_b4(instruction); return;
        case 0xB5: op_b5(instruction); return;
        case 0x2C7: op_2c7(instruction); return;
        case 0x341: case 0x345: case 0x34E: case 0x352: op_341(instruction); return;
        case 0x2EC: op_2ec(instruction); return;
        case 0x2EB: op_2eb(instruction); return;
        case 0x23B: op_23b(instruction); return;
        case 0x109: op_109(instruction); return;
        case 0x108: op_108(instruction); return;
        case 0xC8: op_c8(instruction); return;
        case 0xCC: op_cc(instruction); return;
        case 0xCD: op_cd(instruction); return;
        case 0xFB: op_fb(instruction); return;
        case 0xFC: op_fc(instruction); return;
        case 0xFF: op_ff(instruction); return;
        case 0x102: op_102(instruction); return;
        case 0x103: op_103(instruction); return;
        case 0x104: op_104(instruction); return;
        case 0x105: op_105(instruction); return;
        case 0x100: op_100(instruction); return;
        case 0x12E: op_12e(instruction); return;
        case 0x2FC: op_2fc(instruction); return;
        case 0x24E: op_24e(instruction); return;
        case 0x2DA: op_2da(instruction); return;
        case 0x344: op_344(instruction); return;
        case 0x1FF: op_1ff(instruction); return;
        case 0x1FD: op_1fd(instruction); return;
        case 0x1FE: op_1fe(instruction); return;
        case 0x20B: op_20b(instruction); return;
        case 0x216: op_216(instruction); return;
        case 0x208: op_208(instruction); return;
        case 0x20C: op_20c(instruction); return;
        case 0x23C: op_23c(instruction); return;
        case 0x20D: op_20d(instruction); return;
        case 0x33F: op_33f(instruction); return;
        case 0x204: op_204(instruction); return;
        case 0x205: op_205(instruction); return;
        case 0x20E: op_20e(instruction); return;
        case 0x1F6: op_1f6(instruction); return;
        case 0x1F4: op_1f4(instruction); return;
        case 0x1F5: op_1f5(instruction); return;
        case 0x20F: op_20f(instruction); return;
        case 0x21C: op_21c(instruction); return;
        case 0x101: op_101(instruction); return;
        case 0xAE: op_ae(instruction); return;
        case 0x2EE: op_2ee(instruction); return;
        case 0x10F: op_10f(instruction); return;
        case 0x248: op_248(instruction); return;
        case 0x8B: op_8b(instruction); return;
        case 0x261: op_261(instruction); return;
        case 0x260: op_260(instruction); return;
        case 0x107: op_107(instruction); return;
        case 0x10B: op_10b(instruction); return;
        case 0x30A: op_30a(instruction); return;
        case 0x25B: op_25b(instruction); return;
        case 0x143: op_143(instruction); return;
        case 0x130: op_130(instruction); return;
        case 0x1A4: op_1a4(instruction); return;
        case 0x76: case 0x77: op_76(instruction); return;
        case 0x2BD: op_2bd(instruction); return;
        case 0x2DE: op_2de(instruction); return;
        case 0x192: op_192(instruction); return;
        case 0x19B: op_19b(instruction); return;
        case 0x19C: op_19c(instruction); return;
        case 0x1BF: op_1bf(instruction); return;
        case 0x1CF: op_1cf(instruction); return;
        case 0x1A9: op_1a9(instruction); return;
        case 0x1AA: op_1aa(instruction); return;
        case 0x1A3: op_1a3(instruction); return;
        case 0x1A2: op_1a2(instruction); return;
        case 0x223: op_223(instruction); return;
        case 0x24D: op_24d(instruction); return;
        case 0x24F: op_24f(instruction); return;
        case 0x250: op_250(instruction); return;
        case 0x251: op_251(instruction); return;
        case 0x320: op_320(instruction); return;
        case 0x23D: op_23d(instruction); return;
        case 0x32B: op_32b(instruction); return;
        case 0x322: op_322(instruction); return;
        case 0x323: op_323(instruction); return;
        case 0x1FB: op_1fb(instruction); return;
        case 0x239: op_239(instruction); return;
        case 0x202: op_202(instruction); return;
        case 0x203: op_203(instruction); return;
        case 0x1F7: op_1f7(instruction); return;
        case 0x1F8: op_1f8(instruction); return;
        case 0x1FA: op_1fa(instruction); return;
        case 0x1F9: op_1f9(instruction); return;
        case 0x303: op_303(instruction); return;
        case 0x212: op_212(instruction); return;
        case 0x213: op_213(instruction); return;
        case 0x25D: op_25d(instruction); return;
        case 0x1C1: op_1c1(instruction); return;
        case 0x79: op_79(instruction); return;
        case 0xA1: op_a1(instruction); return;
        case 0xA2: op_a2(instruction); return;
        case 0xA3: op_a3(instruction); return;
        case 0x2DB: op_2db(instruction); return;
        case 0x78: op_78(instruction); return;
        case 0x73: op_73(instruction); return;
        case 0x71: op_71(instruction); return;
        case 0x80: op_80(instruction); return;
        case 0x70: op_70(instruction); return;
        case 0x32F: op_32f(instruction); return;
        case 0x324: op_324(instruction); return;
        case 0x1CA: op_1ca(instruction); return;
        case 0x149: op_149(instruction); return;
        case 0x21B: op_21b(instruction); return;
        case 0x252: op_252(instruction); return;
        case 0xD9: op_d9(instruction); return;
        case 0x1A7: op_1a7(instruction); return;
        case 0x1A8: op_1a8(instruction); return;
        case 0x2F8: op_2f8(instruction); return;
        case 0x2F6: op_2f6(instruction); return;
        case 0x05: op_05(instruction); return;
        case 0x50: op_50(instruction); return;
        case 0x51: op_51(instruction); return;
        case 0x52: op_52(instruction); return;
        case 0x53: op_53(instruction); return;
        case 0x54: op_54(instruction); return;
        case 0x1A5: case 0x2FE: op_1a5(instruction); return;
        case 0x197: op_197(instruction); return;
        case 0x198: op_198(instruction); return;
        case 0x75: op_75(instruction); return;
        case 0x2D5: op_2d5(instruction); return;
        case 0x56: op_56(instruction); return;
        case 0x57: op_57(instruction); return;
        case 0x58: op_58(instruction); return;
        case 0x59: op_59(instruction); return;
        case 0x5A: op_5a(instruction); return;
        case 0x5B: op_5b(instruction); return;
        case 0x5C: op_5c(instruction); return;
        case 0x5D: op_5d(instruction); return;
        case 0x5E: op_5e(instruction); return;
        case 0x5F: op_5f(instruction); return;
        case 0x135: op_135(instruction); return;
        case 0x1CE: op_1ce(instruction); return;
        case 0x136: op_136(instruction); return;
        case 0x13F: op_13f(instruction); return;
        case 0x55: op_55(instruction); return;
        case 0x8c: op_8c(instruction); return;
        case 0x8f: op_8f(instruction); return;
        case 0xC2: op_c2(instruction); return;
        case 0xC4: op_c4(instruction); return;
        case 0x82: op_82(instruction); return;
        case 0x2F5: op_2f5(instruction); return;
        case 0x2FF: op_2ff(instruction); return;
        case 0xa0: op_a0(instruction); return;
        default:
            throw CoreError(
                "opcode-not-implemented:0x" + [&] {
                    char buffer[16];
                    std::snprintf(buffer, sizeof(buffer), "%X", instruction.opcode);
                    return std::string(buffer);
                }());
    }
}

// 门之前的影片对象表服务（`CORE-VM-MOVIE`）：每次迭代、过门之前，若「有活动对象」为 1 就扫一遍。
// ★ 它只做轮询 / 收尾 / 循环 / 填纹理；影片位（门上那一位）只管把门关住。

bool Machine::step() {
    if (halted_) return false;
    // 每次迭代先读一次时钟并写进场景；**暂停**（区间忙标记 = 1 且等待位 = 0）时只更新时间戳，
    // 场景推进 / 影片逐槽计时 / 门前渲染步都整段跳过（`CORE-VM-LOOP`：暂停就是区间忙标记，同一格）。
    const bool paused = driven_elsewhere_ && (state_flags_ & (1u << 10)) == 0;
    advance_scene(external_clock_ ? host_clock_ms_ : scene_clock_ms_ + 16, !paused);
    // 门之前的影片对象表服务（`CORE-VM-MOVIE`）：新帧会置脏，所以必须在门前那次渲染判定之前。
    if (!paused) service_movies();
    // 门之前的**逐字符显现**（typewriter）：引擎的推进器每帧跑一次、帧间按 `message:MessageSpeed` 睡眠，
    // 等待输入时**也在跑**（那正是"字还在打"的形态）⇒ 这里不受 `paused` 影响。
    service_text_layers();
    // 门前渲染：主循环闸门（0x412739–0x4127E2）先要求
    // `(0x21B != 0) || (状态字 & 0x2400)`，脏只是更内层条件。
    const bool render_gate = scalar_21b_ != 0 || (state_flags_ & (0x400u | 0x2000u)) != 0;
    if (!paused && draw_mode_ == 1 && render_gate && (state_flags_ & (1u << 24)) == 0 &&
        (scene_dirty_ || scene_animating_ || command_animating_)) {
        frame_step_requested_ = true;
    }
    if (state_flags_ != 0) {
        // 第 0 位（长睡眠）：本实现没有计时器服务，先按"服务一次就清位"处理。
        if ((state_flags_ & 1u) != 0) { state_flags_ &= ~1u; return true; }
        // 影片位（门上 0x2000）：只清位；对象表仍有已启动影片槽 ⇒ 再置起（`CORE-VM-MOVIE`）。
        // 它不轮询、不销毁、不填纹理 —— 那些都在门之前的 service_movies 里。
        if ((state_flags_ & (1u << 13)) != 0) {
            state_flags_ &= ~(1u << 13);
            if (movie_active_) state_flags_ |= (1u << 13);
            return true;
        }
        // 等待位：放行看「场景有动画」，不是"表里还有没有未到期的窗"。
        if ((state_flags_ & (1u << 10)) != 0 && !scene_animating_) state_flags_ &= ~(1u << 10);
        // 位 0 / 10 / 13 有服务分支；其余位**没有**（规范只登记存在；第 30 位按规范「启用后门恒关」）。
        // ★ 连续撞到"没有分支的位"时给一个明确诊断与停因：否则外面只看到"停在原地 + 烧步数"，
        //   与"在等输入"完全分不开。这不是引擎语义，是观测。
        constexpr std::uint32_t kServicedGateBits = (1u << 0) | (1u << 10) | (1u << 13);
        if ((state_flags_ & kServicedGateBits) == 0) {
            gate_stall_steps_ += 1;
            if (!gate_stall_reported_) {
                gate_stall_reported_ = true;
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "gate-bit-unserviced:0x%X",
                              static_cast<unsigned>(state_flags_));
                report_error(buffer);
            }
            constexpr std::size_t kGateStallLimit = 10000;
            if (gate_stall_steps_ >= kGateStallLimit) {
                char buffer[96];
                std::snprintf(buffer, sizeof(buffer), "gate-stalled:0x%X",
                              static_cast<unsigned>(state_flags_));
                halted_ = true;
                stop_.reason = StopReason::GateStalled;
                stop_.offset = frame_mut().ip;
                stop_.message = buffer;
                return false;
            }
        } else {
            gate_stall_steps_ = 0;
        }
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
        // 诊断扫场（`--sweep-missing`）：缺 handler 不中止，记账后当空操作继续 —— 一次跑遍界面
        // 就能把所有缺口按首次出现顺序列出来。⛔ 这不是引擎语义（跳过会让脚本失真）。
        const std::string what = e.what();
        const std::string prefix = "opcode-not-implemented:";
        if (sweep_missing_ && what.compare(0, prefix.size(), prefix) == 0) {
            note_missing_opcode(opcode);
        } else {
            halted_ = true;
            stop_.reason = StopReason::Error;
            stop_.message = what;
            return false;
        }
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


