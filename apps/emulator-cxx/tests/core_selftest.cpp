#include "core/agf.h"
#include "core/alf.h"
#include "core/config.h"
#include "core/encoding.h"
#include "core/opcode_table.h"
#include "core/scene_raster.h"
#include "core/script.h"
#include "core/text_raster.h"
#include "core/vm.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

void put_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

void put_field(std::vector<std::uint8_t>& bytes, const char* text, std::size_t width) {
    const std::size_t length = std::strlen(text);
    if (length > width) throw std::runtime_error("fixture-field-too-long");
    bytes.insert(bytes.end(), text, text + length);
    bytes.resize(bytes.size() + (width - length), 0);
}

std::vector<std::uint8_t> make_alf_ai() {
    std::vector<std::uint8_t> bytes(268, 0);
    std::memcpy(bytes.data(), "S4AI", 4);
    std::memcpy(bytes.data() + 8, "selftest", 8);
    const std::uint32_t zone = 7;
    std::memcpy(bytes.data() + 264, &zone, 4);

    put_u32(bytes, 1);  // archive count
    put_field(bytes, "DATA1.ALF", 256);
    put_u32(bytes, 1);  // file count
    put_field(bytes, "SYSTEM4.BIN", 64);
    put_u32(bytes, 0);   // archive index
    put_u32(bytes, 0);   // file index
    put_u32(bytes, 0);   // offset
    put_u32(bytes, 16);  // length
    return bytes;
}

std::vector<std::uint8_t> make_script() {
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), {'S', 'Y', 'S', '4'});
    bytes.insert(bytes.end(), {'4', '5', '0', ' '});
    for (int i = 0; i < 6; ++i) put_u32(bytes, static_cast<std::uint32_t>(i + 1));
    put_u32(bytes, 28);
    const std::uint32_t pairs[6] = {1, 10, 2, 20, 3, 30};
    for (auto value : pairs) put_u32(bytes, value);

    put_u32(bytes, 2);  // exit, argc 0
    put_u32(bytes, 80);  // add, argc 3
    put_u32(bytes, 3); put_u32(bytes, 0);
    put_u32(bytes, 3); put_u32(bytes, 1);
    put_u32(bytes, 3); put_u32(bytes, 2);
    put_u32(bytes, 9);  // exit-script, argc 0
    return bytes;
}

std::vector<std::uint8_t> make_vm_script() {
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), {'S', 'Y', 'S', '4'});
    bytes.insert(bytes.end(), {'4', '5', '0', ' '});
    for (int i = 0; i < 6; ++i) put_u32(bytes, 0);
    put_u32(bytes, 28);
    for (int i = 0; i < 6; ++i) put_u32(bytes, 0);

    put_u32(bytes, 85); // mov(a=local-int 0, b=immediate 7)
    put_u32(bytes, 9); put_u32(bytes, 0);
    put_u32(bytes, 0); put_u32(bytes, 7);
    put_u32(bytes, 80); // add(a=local-int 0, b=local-int 0, c=immediate 5)
    put_u32(bytes, 9); put_u32(bytes, 0);
    put_u32(bytes, 9); put_u32(bytes, 0);
    put_u32(bytes, 0); put_u32(bytes, 5);
    put_u32(bytes, 2); // exit
    return bytes;
}

std::vector<std::uint8_t> make_call_script() {
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), {'S', 'Y', 'S', '4'});
    bytes.insert(bytes.end(), {'4', '5', '0', ' '});
    for (int i = 0; i < 6; ++i) put_u32(bytes, 0);
    put_u32(bytes, 28);
    for (int i = 0; i < 6; ++i) put_u32(bytes, 0);

    put_u32(bytes, 0x8f); // call position 9
    put_u32(bytes, 0); put_u32(bytes, 9);
    put_u32(bytes, 85); // mov local-int 0 = 42
    put_u32(bytes, 9); put_u32(bytes, 0);
    put_u32(bytes, 0); put_u32(bytes, 42);
    put_u32(bytes, 2); // exit（根帧结束）
    put_u32(bytes, 85); // 目标位置 9：mov local-int 1 = 99
    put_u32(bytes, 9); put_u32(bytes, 1);
    put_u32(bytes, 0); put_u32(bytes, 99);
    put_u32(bytes, 5); // ret → 位置 3
    return bytes;
}

std::vector<std::uint8_t> make_frontier_script() {
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), {'S', 'Y', 'S', '4'});
    bytes.insert(bytes.end(), {'4', '5', '0', ' '});
    for (int i = 0; i < 6; ++i) put_u32(bytes, 0);
    put_u32(bytes, 28);
    for (int i = 0; i < 6; ++i) put_u32(bytes, 0);

    put_u32(bytes, 0x1A7); // comment：载荷 type 2 不应被读
    put_u32(bytes, 2); put_u32(bytes, 0);
    put_u32(bytes, 0x1A8); // dev_ukn
    put_u32(bytes, 0x2F6); // stop voice group 0
    put_u32(bytes, 0); put_u32(bytes, 0);
    put_u32(bytes, 0x2F8); // set voice volume：音量 -20000 → 夹到 -10000
    put_u32(bytes, 0); put_u32(bytes, 0);
    put_u32(bytes, 0); put_u32(bytes, 0xFFFFB1E0u);
    put_u32(bytes, 0x149); // scalar 149 = 7
    put_u32(bytes, 0); put_u32(bytes, 7);
    put_u32(bytes, 0x21B); // scalar 21b = -5 → 1
    put_u32(bytes, 0); put_u32(bytes, 0xFFFFFFFBu);
    put_u32(bytes, 0x252); // scalar 252 = 9
    put_u32(bytes, 0); put_u32(bytes, 9);
    put_u32(bytes, 0x1CA); // named key = 3
    put_u32(bytes, 0); put_u32(bytes, 3);
    put_u32(bytes, 0x32F); // container[5] = 0
    put_u32(bytes, 0); put_u32(bytes, 5);
    put_u32(bytes, 2); // exit
    return bytes;
}

std::vector<std::uint8_t> make_list_script() {
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), {'S', 'Y', 'S', '4'});
    bytes.insert(bytes.end(), {'4', '5', '0', ' '});
    for (int i = 0; i < 6; ++i) put_u32(bytes, 0);
    put_u32(bytes, 28);
    for (int i = 0; i < 6; ++i) put_u32(bytes, 0);

    put_u32(bytes, 0x70); // 组 1；op2/op3=10/11，op4/op5=20/21
    for (auto v : {1, 10, 11, 20, 21}) { put_u32(bytes, 0); put_u32(bytes, static_cast<std::uint32_t>(v)); }
    put_u32(bytes, 0x79); // 组 1；30/31
    for (auto v : {1, 30, 31}) { put_u32(bytes, 0); put_u32(bytes, static_cast<std::uint32_t>(v)); }
    put_u32(bytes, 0x1C1); // 组 1；40/41
    for (auto v : {1, 40, 41}) { put_u32(bytes, 0); put_u32(bytes, static_cast<std::uint32_t>(v)); }
    put_u32(bytes, 2);
    return bytes;
}

// 一个脚本构造器：按 (操作数类型, 原始值) 直接写指令；测试只用到整型立即数（type 0）。
void emit_instruction(std::vector<std::uint8_t>& bytes, std::uint32_t opcode,
                      std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> operands) {
    put_u32(bytes, opcode);
    for (const auto& [type, raw] : operands) {
        put_u32(bytes, type);
        put_u32(bytes, raw);
    }
}

std::vector<std::uint8_t> make_script_with_body() {
    std::vector<std::uint8_t> bytes;
    bytes.insert(bytes.end(), {'S', 'Y', 'S', '4'});
    bytes.insert(bytes.end(), {'4', '5', '0', ' '});
    for (int i = 0; i < 6; ++i) put_u32(bytes, 0);
    put_u32(bytes, 28);
    for (int i = 0; i < 6; ++i) put_u32(bytes, 0);
    return bytes;
}

// `0x204` 的内联文本操作数：type 2 的 raw 是「代码区内的字偏移」，文本字节按 XOR 0xFF 编码。
void emit_text_204(std::vector<std::uint8_t>& bytes, std::uint32_t texture_id, std::uint32_t x,
                   std::uint32_t y, const std::string& text) {
    const std::size_t body_start = 60;  // 本文件的脚本头固定 60 字节
    const std::uint32_t text_word =
        static_cast<std::uint32_t>((bytes.size() - body_start) / 4) + 9;  // 1 + 4*2 words
    put_u32(bytes, 0x204);
    for (const auto& operand : {std::pair<std::uint32_t, std::uint32_t>{0, texture_id},
                                {0, x}, {0, y}, {2, text_word}}) {
        put_u32(bytes, operand.first);
        put_u32(bytes, operand.second);
    }
    for (const unsigned char byte : text) bytes.push_back(static_cast<std::uint8_t>(byte ^ 0xFFu));
    bytes.push_back(0xFFu);
    while ((bytes.size() - body_start) % 4 != 0) bytes.push_back(0xFFu);
}

std::vector<std::uint8_t> make_agf_1x1() {    std::vector<std::uint8_t> bytes(24, 0);
    const auto set_u32 = [&](std::size_t offset, std::uint32_t value) {
        std::memcpy(bytes.data() + offset, &value, 4);
    };
    set_u32(4, 1);   // version
    set_u32(8, 0);   // unknown0
    set_u32(12, 32); // meta unpacked
    set_u32(16, 32); // repeat
    set_u32(20, 32); // meta packed
    bytes.resize(24 + 32, 0);
    set_u32(24 + 20, 1); // width
    set_u32(24 + 24, 1); // height
    bytes[24 + 30] = 24;
    // body header: [0]=?, +4 unpacked, +8 packed
    bytes.resize(24 + 32 + 12, 0);
    set_u32(24 + 32 + 4, 4);
    set_u32(24 + 32 + 8, 4);
    bytes.push_back(0x30); // B
    bytes.push_back(0x20); // G
    bytes.push_back(0x10); // R
    bytes.push_back(0x00); // pad
    return bytes;
}

}  // namespace

int main() {
    try {
        // CP932 → UTF-8：引擎字符串/路径是 ANSI 字节，显示与宿主路径必须先解码。
        if (core::cp932_to_utf8("\x82\xA0") != "\xE3\x81\x82") {  // 「あ」
            std::fprintf(stderr, "cp932 decode mismatch\n");
            return 1;
        }
        if (core::path_to_utf8(core::path_from_engine("\x82\xA0")) != "\xE3\x81\x82") {
            std::fprintf(stderr, "engine path decode mismatch\n");
            return 1;
        }

        const auto alf_bytes = make_alf_ai();
        const auto alf = core::parse_alf_index(alf_bytes);
        if (alf.signature != "S4AI" || alf.title != "selftest" || alf.zone != 7) {
            std::fprintf(stderr, "ALF header mismatch\n");
            return 1;
        }
        if (alf.archives.size() != 1 || alf.archives[0] != "DATA1.ALF" || alf.files.size() != 1) {
            std::fprintf(stderr, "ALF TOC mismatch\n");
            return 1;
        }
        if (alf.files[0].name != "SYSTEM4.BIN" || alf.files[0].length != 16) {
            std::fprintf(stderr, "ALF file mismatch\n");
            return 1;
        }

        const auto agf = core::decode_agf(make_agf_1x1());
        if (agf.width != 1 || agf.height != 1 || agf.bpp != 24 || agf.rgba.size() != 4 ||
            agf.rgba[0] != 0x10 || agf.rgba[1] != 0x20 || agf.rgba[2] != 0x30 || agf.rgba[3] != 255) {
            std::fprintf(stderr, "AGF decode mismatch\n");
            return 1;
        }

        const auto script_bytes = make_script();
        const auto header = core::parse_script_header(script_bytes);
        if (header.magic != "SYS4" || header.version != "450 " || header.body_offset != 60) {
            std::fprintf(stderr, "script header mismatch\n");
            return 1;
        }
        if (header.tables[0].first != 1 || header.tables[0].second != 10 ||
            header.tables[2].first != 3 || header.tables[2].second != 30) {
            std::fprintf(stderr, "script table pointers mismatch\n");
            return 1;
        }

        const std::string table_path = "opcode-table.json";
        core::OpcodeTable table = core::OpcodeTable::from_rows({
            {2, 0, "exit", true},
            {9, 0, "exit-script", true},
            {80, 3, "add", true},
            {85, 2, "mov", true},
        });
        try {
            table = core::OpcodeTable::load_from_file(table_path);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "opcode table load failed, using fixture rows: %s\n", e.what());
        }

        std::string error;
        const auto instructions = core::decode_instructions(
            std::span<const std::uint8_t>(script_bytes).subspan(header.body_offset),
            table,
            16,
            &error);
        if (!error.empty()) {
            std::fprintf(stderr, "decode error: %s\n", error.c_str());
            return 1;
        }
        if (instructions.size() != 3 || instructions[0].opcode != 2 ||
            instructions[1].opcode != 80 || instructions[1].operands.size() != 3 ||
            instructions[2].opcode != 9) {
            std::fprintf(stderr, "decode instruction mismatch\n");
            return 1;
        }
        if (instructions[1].operands[2].raw != 2) {
            std::fprintf(stderr, "decode operand mismatch\n");
            return 1;
        }

        core::Machine machine(0x12345678u, table);
        machine.load_root(make_vm_script());
        const auto vm_result = machine.run(16);
        if (vm_result.reason != core::StopReason::Halted || vm_result.steps != 3) {
            std::fprintf(stderr, "VM run mismatch: reason=%d steps=%zu msg=%s\n",
                static_cast<int>(vm_result.reason), vm_result.steps, vm_result.message.c_str());
            return 1;
        }
        const auto local_it = machine.frame().local_int.find(0);
        if (local_it == machine.frame().local_int.end() || machine.decode_int(local_it->second) != 12) {
            std::fprintf(stderr, "VM local-int arithmetic mismatch\n");
            return 1;
        }

        core::Machine call_machine(0x12345678u, table);
        call_machine.load_root(make_call_script());
        const auto call_result = call_machine.run(16);
        if (call_result.reason != core::StopReason::Halted ||
            call_machine.read_int(core::Operand{9, 0}) != 42 ||
            call_machine.read_int(core::Operand{9, 1}) != 99) {
            std::fprintf(stderr, "VM call/ret mismatch: reason=%d local0=%d local1=%d msg=%s\n",
                static_cast<int>(call_result.reason),
                call_machine.read_int(core::Operand{9, 0}),
                call_machine.read_int(core::Operand{9, 1}),
                call_result.message.c_str());
            return 1;
        }

        core::ConfigVocabulary vocabulary = core::ConfigVocabulary::load_from_file("config-keys.json");
        core::ConfigStore config(std::move(vocabulary));
        config.load_defaults({{"SCREENX", "320"}, {"FONT", "Arial"}});
        if (config.get_int("set:WinX") != 320 || config.get_string("message:Font") != "Arial") {
            std::fprintf(stderr, "config defaults mismatch\n");
            return 1;
        }
        {
            std::ofstream ini("core-selftest.ini", std::ios::binary);
            ini << "[display]\nUseDirectDrawEmuration=1\n";
        }
        config.load_player_file("core-selftest.ini");
        std::remove("core-selftest.ini");
        if (config.get_int("display:UseDirectDrawEmuration") != 1) {
            std::fprintf(stderr, "config player override mismatch\n");
            return 1;
        }

        core::Machine list_machine(0x12345678u, table);
        list_machine.load_root(make_list_script());
        const auto list_result = list_machine.run(16);
        const auto& list = list_machine.list_record(1);
        if (list_result.reason != core::StopReason::Halted || list.f0c != 20 || list.f10 != 21 ||
            list.f14 != 10 || list.f18 != 11 || list.f1c != 30 || list.f20 != 31 ||
            list.f24 != 40 || list.f28 != 41 || list.registered != 1) {
            std::fprintf(stderr, "VM list mismatch: reason=%d steps=%zu\n",
                static_cast<int>(list_result.reason), list_result.steps);
            return 1;
        }

        core::Machine frontier(0x12345678u, table);
        frontier.load_root(make_frontier_script());
        const auto frontier_result = frontier.run(32);
        if (frontier_result.reason != core::StopReason::Halted ||
            frontier.voice_playing() || frontier.voice_volume(0) != -10000 ||
            frontier.scalar_149() != 7 || frontier.scalar_21b() != 1 ||
            frontier.scalar_252() != 9 ||
            frontier.named_message_read_text_skip() != 3 ||
            frontier.forward_container_at(5) != 0) {
            std::fprintf(stderr, "VM frontier mismatch: reason=%d steps=%zu msg=%s\n",
                static_cast<int>(frontier_result.reason), frontier_result.steps,
                frontier_result.message.c_str());
            return 1;
        }

        // 未实现 opcode：停在它前面；前一条 mov 的写入保留。
        auto unsupported_script = make_vm_script();
        unsupported_script.resize(60 + 20); // 只保留第一条 mov
        put_u32(unsupported_script, 610); // 表内无处理器
        put_u32(unsupported_script, 0);
        core::Machine unsupported(0x12345678u, table);
        unsupported.load_root(unsupported_script);
        const auto unsupported_result = unsupported.run(8);
        if (unsupported_result.reason != core::StopReason::UnsupportedOpcode ||
            unsupported_result.offset != 20) {
            std::fprintf(stderr, "VM unsupported mismatch: reason=%d offset=%zu\n",
                static_cast<int>(unsupported_result.reason), unsupported_result.offset);
            return 1;
        }

        // ── 渲染：颜色窗推进 + 派生色四档（CORE-TIMING-WINDOW） ──
        auto window_script = make_script_with_body();
        emit_instruction(window_script, 0x1F8, {{0, 1}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(window_script, 0x1FB,
                         {{0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(window_script, 0x203, {{0, 1}, {0, 0}, {0, 0}, {0, 0x00112233u}});
        emit_instruction(window_script, 0x202,
                         {{0, 1}, {0, 100}, {0, 100}, {0, 200}, {0, 0x00ABCDEFu}});
        emit_instruction(window_script, 2, {});
        core::Machine window_machine(0x12345678u, table);
        window_machine.load_root(window_script);
        for (int i = 0; i < 4; ++i) {
            if (!window_machine.step()) {
                std::fprintf(stderr, "timing window setup step %d failed\n", i);
                return 1;
            }
        }
        const core::DrawItem* window_item = window_machine.draw_item(1);
        if (window_item == nullptr || (window_item->flags & 2u) == 0 ||
            window_item->color != 0x00112233u || window_item->window_start != 0) {
            std::fprintf(stderr, "timing window setup mismatch\n");
            return 1;
        }
        window_machine.advance_scene(1000);  // 盖章
        if (window_item->window_start != 1000 || !window_machine.scene_animating()) {
            std::fprintf(stderr, "timing window stamp mismatch start=%u anim=%d\n",
                         window_item->window_start, window_machine.scene_animating());
            return 1;
        }
        window_machine.advance_scene(1100);  // 延迟边界：仍取当前颜色
        if (core::derive_color(*window_item, 1100, false) != 0x00112233u ||
            window_item->color != 0x00112233u) {
            std::fprintf(stderr, "timing window delay tier mismatch\n");
            return 1;
        }
        window_machine.advance_scene(1150);  // 推进中：插值不写回
        const auto mid_color = core::derive_color(*window_item, 1150, false);
        if (((mid_color >> 24) & 0xFFu) != 100 || window_item->color != 0x00112233u) {
            std::fprintf(stderr, "timing window interpolation mismatch mid=0x%08X current=0x%08X\n",
                         mid_color, window_item->color);
            return 1;
        }
        window_machine.advance_scene(1200);  // 终点（相等算窗内）：进度 1，仍不写回
        if (((core::derive_color(*window_item, 1200, false) >> 24) & 0xFFu) != 200 ||
            window_item->color != 0x00112233u) {
            std::fprintf(stderr, "timing window end-tier mismatch\n");
            return 1;
        }
        window_machine.advance_scene(1201);  // 到期：写回终色并清窗
        if ((window_item->flags & 2u) != 0 || window_item->delay != 0 ||
            window_item->duration != 0 || window_item->window_start != 0 ||
            window_item->color != 0xC8ABCDEFu || window_item->target_color != core::kNoColor ||
            window_machine.scene_animating()) {
            std::fprintf(stderr,
                         "timing window expiry mismatch flags=%u color=0x%08X target=0x%08X anim=%d\n",
                         window_item->flags, window_item->color, window_item->target_color,
                         window_machine.scene_animating());
            return 1;
        }

        // 等待位按「场景有动画」放行（`CORE-TIMING-WINDOW`）。
        auto wait_script = make_script_with_body();
        emit_instruction(wait_script, 0x1FB,
                         {{0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(wait_script, 0x202,
                         {{0, 1}, {0, 100}, {0, 100}, {0, 200}, {0, 0x00ABCDEFu}});
        emit_instruction(wait_script, 0x21C, {});
        emit_instruction(wait_script, 2, {});
        core::Machine wait_machine(0x12345678u, table);
        wait_machine.load_root(wait_script);
        wait_machine.set_host_clock_ms(1000);
        if (!wait_machine.step() || !wait_machine.step()) {  // 0x1FB / 0x202
            std::fprintf(stderr, "wait setup steps failed\n");
            return 1;
        }
        if (!wait_machine.step() || (wait_machine.state_flags() & (1u << 10)) == 0) {
            std::fprintf(stderr, "wait bit not set\n");
            return 1;
        }
        wait_machine.set_host_clock_ms(1150);
        if (!wait_machine.step() || (wait_machine.state_flags() & (1u << 10)) == 0) {
            std::fprintf(stderr, "wait released while animating\n");
            return 1;
        }
        wait_machine.set_host_clock_ms(1201);
        if (!wait_machine.step() || (wait_machine.state_flags() & (1u << 10)) != 0) {
            std::fprintf(stderr, "wait not released after animation ended\n");
            return 1;
        }

        // 0x20C：帧步进请求 + 时间戳 + 消费后强制完成清 0。
        auto frame_script = make_script_with_body();
        emit_instruction(frame_script, 0x20C, {});
        emit_instruction(frame_script, 2, {});
        core::Machine frame_machine(0x12345678u, table);
        frame_machine.load_root(frame_script);
        frame_machine.run(8);
        if (!frame_machine.frame_step_requested() || frame_machine.engine_now_ms() == 0) {
            std::fprintf(stderr, "0x20C frame request mismatch\n");
            return 1;
        }
        frame_machine.force_complete();
        if (!frame_machine.consume_frame_step() || frame_machine.frame_step_requested() ||
            frame_machine.forced_complete()) {
            std::fprintf(stderr, "0x20C consume mismatch\n");
            return 1;
        }

        // ── 渲染：0x20B 填充 + 0x33F 场景混合（tint / 模式） ──
        const auto make_blend_script = [&](std::uint32_t mode, std::uint32_t tint) {
            auto bytes = make_script_with_body();
            emit_instruction(bytes, 0x1F8, {{0, 1}, {0, 4}, {0, 4}, {0, 0}});
            emit_instruction(bytes, 0x20B,
                             {{0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 255}, {0, 0x00FFFFFFu}});
            emit_instruction(bytes, 0x1FB,
                             {{0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
            emit_instruction(bytes, 0x203, {{0, 1}, {0, 0}, {0, 255}, {0, 0x00FFFFFFu}});
            emit_instruction(bytes, 0x33F, {{0, mode}, {0, 255}, {0, tint}});
            emit_instruction(bytes, 2, {});
            return bytes;
        };
        core::Machine blend_machine(0x12345678u, table);
        blend_machine.load_root(make_blend_script(0, 0x00FF0000u));
        blend_machine.run(16);
        const core::Bytes tinted = core::rasterize_scene(blend_machine, 4, 4);
        if (tinted[0] != 255 || tinted[1] != 0 || tinted[2] != 0 || tinted[3] != 255) {
            std::fprintf(stderr, "scene blend tint mismatch pixel=(%u,%u,%u,%u)\n",
                         tinted[0], tinted[1], tinted[2], tinted[3]);
            return 1;
        }
        core::Machine multiply_machine(0x12345678u, table);
        multiply_machine.load_root(make_blend_script(2, 0x00FFFFFFu));
        multiply_machine.run(16);
        const core::Bytes multiplied = core::rasterize_scene(multiply_machine, 4, 4);
        if (multiplied[0] != 0 || multiplied[1] != 0 || multiplied[2] != 0 || multiplied[3] != 255) {
            std::fprintf(stderr, "scene blend multiply mismatch pixel=(%u,%u,%u,%u)\n",
                         multiplied[0], multiplied[1], multiplied[2], multiplied[3]);
            return 1;
        }
        const core::TextureRecord* filled = multiply_machine.texture(1);
        if (filled == nullptr || filled->rgba.size() != 64 || filled->rgba[0] != 255 ||
            filled->rgba[1] != 255 || filled->rgba[2] != 255 || !filled->non_file) {
            std::fprintf(stderr, "0x20B fill mismatch\n");
            return 1;
        }

        // ── 渲染：场景命令记录族 + 命令档（种类 0 / 2 / 3） ──
        const auto emit_scene_item = [&](std::vector<std::uint8_t>& bytes, std::uint32_t key,
                                         std::uint32_t texture, std::uint32_t color) {
            emit_instruction(bytes, 0x1F8, {{0, texture}, {0, 4}, {0, 4}, {0, 0}});
            emit_instruction(bytes, 0x20B,
                             {{0, texture}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 255}, {0, color}});
            emit_instruction(bytes, 0x1FB, {{0, key}, {0, texture}, {0, 0}, {0, 0},
                                            {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        };
        auto command_script = make_script_with_body();
        emit_scene_item(command_script, 1, 1, 0x00FF0000u);
        emit_scene_item(command_script, 2, 2, 0x000000FFu);
        emit_instruction(command_script, 0x1F8, {{0, 10}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(command_script, 0x223,
                         {{0, 5}, {0, 10}, {0, 1}, {0, 1}, {0, 2}, {0, 1}, {0, 0}, {0, 100}});
        emit_instruction(command_script, 2, {});
        core::Machine command_machine(0x12345678u, table);
        command_machine.load_root(command_script);
        command_machine.run(32);
        const auto start_record = command_machine.scene_commands().find(5);
        if (start_record == command_machine.scene_commands().end() ||
            start_record->second.kind != 0 || start_record->second.texture_id != 10 ||
            start_record->second.a_start != 1 || start_record->second.b_start != 2) {
            std::fprintf(stderr, "scene command 0x223 record mismatch\n");
            return 1;
        }
        command_machine.reset_scene_clock(1000);
        core::rasterize_scene(command_machine, 4, 4);
        const core::TextureRecord* fade_texture = command_machine.texture(10);
        if (fade_texture == nullptr || fade_texture->rgba[0] != 255 ||
            fade_texture->rgba[2] != 0) {
            std::fprintf(stderr, "kind0 fade progress 0 mismatch\n");
            return 1;
        }
        command_machine.reset_scene_clock(1050);
        core::rasterize_scene(command_machine, 4, 4);
        fade_texture = command_machine.texture(10);
        if (fade_texture == nullptr || fade_texture->rgba[0] == 0 || fade_texture->rgba[2] == 0) {
            std::fprintf(stderr, "kind0 fade progress 0.5 mismatch r=%u b=%u\n",
                         fade_texture ? fade_texture->rgba[0] : 0,
                         fade_texture ? fade_texture->rgba[2] : 0);
            return 1;
        }
        command_machine.reset_scene_clock(1100);
        core::rasterize_scene(command_machine, 4, 4);
        fade_texture = command_machine.texture(10);
        if (fade_texture == nullptr || fade_texture->rgba[0] != 0 || fade_texture->rgba[2] != 255) {
            std::fprintf(stderr, "kind0 fade progress 1 mismatch\n");
            return 1;
        }
        command_machine.reset_scene_clock(1101);
        core::rasterize_scene(command_machine, 4, 4);
        if (!command_machine.scene_commands().empty()) {
            std::fprintf(stderr, "command table not cleared after animation ended\n");
            return 1;
        }

        // 种类 2：类型越界 ⇒ 不写表 + 一条上报；分割幅 < 1 ⇒ 夹取后照写。
        auto invalid_type_script = make_script_with_body();
        emit_instruction(invalid_type_script, 0x1F8, {{0, 10}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(invalid_type_script, 0x24F,
                         {{0, 5}, {0, 10}, {0, 1}, {0, 1}, {0, 2}, {0, 1}, {0, 12}, {0, 1},
                          {0, 0}, {0, 100}});
        emit_instruction(invalid_type_script, 2, {});
        core::Machine invalid_type(0x12345678u, table);
        invalid_type.load_root(invalid_type_script);
        invalid_type.run(16);
        if (!invalid_type.scene_commands().empty() || invalid_type.errors().size() != 1) {
            std::fprintf(stderr, "kind2 type validation mismatch records=%zu errors=%zu\n",
                         invalid_type.scene_commands().size(), invalid_type.errors().size());
            return 1;
        }
        auto split_zero_script = make_script_with_body();
        emit_instruction(split_zero_script, 0x1F8, {{0, 10}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(split_zero_script, 0x24F,
                         {{0, 5}, {0, 10}, {0, 1}, {0, 1}, {0, 2}, {0, 1}, {0, 3}, {0, 0},
                          {0, 7}, {0, 9}});
        emit_instruction(split_zero_script, 2, {});
        core::Machine split_zero(0x12345678u, table);
        split_zero.load_root(split_zero_script);
        split_zero.run(16);
        const auto clamped = split_zero.scene_commands().find(5);
        if (clamped == split_zero.scene_commands().end() || clamped->second.wipe_type != -1 ||
            clamped->second.split != 1 || clamped->second.delay != 0 ||
            clamped->second.duration != 0 || split_zero.errors().size() != 1) {
            std::fprintf(stderr, "kind2 split clamp mismatch\n");
            return 1;
        }

        // 种类 3：记录写下变体格与「起值 / 终值」，进度用尽后持续时间清 0。
        auto blur_script = make_script_with_body();
        emit_scene_item(blur_script, 1, 1, 0x00FF0000u);
        emit_instruction(blur_script, 0x1F8, {{0, 10}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(blur_script, 0x250,
                         {{0, 5}, {0, 10}, {0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 20}, {0, 45},
                          {0, 0}, {0, 100}});
        emit_instruction(blur_script, 2, {});
        core::Machine blur_machine(0x12345678u, table);
        blur_machine.load_root(blur_script);
        blur_machine.run(24);
        const auto blur_record = blur_machine.scene_commands().find(5);
        if (blur_record == blur_machine.scene_commands().end() ||
            blur_record->second.kind != 3 || blur_record->second.variant != 0 ||
            blur_record->second.from[0] != 0 || blur_record->second.to[0] != 20 ||
            blur_record->second.from[1] != 0 || blur_record->second.to[1] != 45) {
            std::fprintf(stderr, "kind3 variant0 record mismatch\n");
            return 1;
        }
        blur_machine.reset_scene_clock(1000);
        core::rasterize_scene(blur_machine, 4, 4);
        blur_machine.reset_scene_clock(1200);
        core::rasterize_scene(blur_machine, 4, 4);
        if (!blur_machine.scene_commands().empty()) {
            std::fprintf(stderr, "kind3 command table not cleared after expiry\n");
            return 1;
        }

        // ── 渲染：源矩形窗（0x239） ──
        auto rect_script = make_script_with_body();
        emit_instruction(rect_script, 0x1F8, {{0, 1}, {0, 8}, {0, 8}, {0, 0}});
        emit_instruction(rect_script, 0x20B,
                         {{0, 1}, {0, 0}, {0, 0}, {0, 8}, {0, 8}, {0, 255}, {0, 0x00FFFFFFu}});
        emit_instruction(rect_script, 0x20B,
                         {{0, 1}, {0, 4}, {0, 0}, {0, 4}, {0, 8}, {0, 255}, {0, 0x00FF0000u}});
        emit_instruction(rect_script, 0x1FB,
                         {{0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(rect_script, 0x239,
                         {{0, 1}, {0, 0}, {0, 100}, {0, 4}, {0, 2}, {0, 3}});
        emit_instruction(rect_script, 2, {});
        core::Machine rect_machine(0x12345678u, table);
        rect_machine.load_root(rect_script);
        rect_machine.run(16);
        if (auto* item = rect_machine.draw_item_mut(1)) item->window_start = 0;
        rect_machine.set_host_clock_ms(1000);
        rect_machine.advance_scene(1000);
        const core::Bytes origin = core::rasterize_scene(rect_machine, 8, 8);
        rect_machine.set_host_clock_ms(1025);
        rect_machine.advance_scene(1025);
        const core::Bytes shifted = core::rasterize_scene(rect_machine, 8, 8);
        // 源矩形窗选的是图集里的另一块：位移后同一目标位置采到右半（红）。
        if (origin[3] == 0 ||
            origin[0] != 255 || origin[1] != 255 || origin[2] != 255 ||
            shifted[0] != 255 || shifted[1] != 0 || shifted[2] != 0) {
            std::fprintf(stderr, "source rect window step mismatch origin=(%u,%u,%u) shifted=(%u,%u,%u)\n",
                         origin[0], origin[1], origin[2], shifted[0], shifted[1], shifted[2]);
            return 1;
        }
        rect_machine.set_host_clock_ms(1125);
        rect_machine.advance_scene(1125);
        core::rasterize_scene(rect_machine, 8, 8);
        const core::DrawItem* rect_item = rect_machine.draw_item(1);
        if (rect_item == nullptr || (rect_item->flags & 2u) != 0 || rect_item->rect_duration != 0 ||
            rect_item->left != 4 || rect_item->top != 4) {
            std::fprintf(stderr, "source rect window expiry writeback mismatch flags=%u left=%d top=%d\n",
                         rect_item ? rect_item->flags : 0, rect_item ? rect_item->left : -1,
                         rect_item ? rect_item->top : -1);
            return 1;
        }

        // ── 渲染：0x204 离屏文字（Shift-JIS + FreeType） ──
        if (core::text_backend_available()) {
            auto text_script = make_script_with_body();
            emit_instruction(text_script, 0x1F8, {{0, 1}, {0, 128}, {0, 32}, {0, 0}});
            emit_text_204(text_script, 1, 0, 0, "ABC");
            emit_instruction(text_script, 2, {});
            core::Machine text_machine(0x12345678u, table);
            text_machine.load_root(text_script);
            text_machine.run(8);
            const core::TextureRecord* text_texture = text_machine.texture(1);
            std::size_t ink = 0;
            if (text_texture != nullptr) {
                for (std::size_t i = 3; i < text_texture->rgba.size(); i += 4) {
                    if (text_texture->rgba[i] != 0) ++ink;
                }
            }
            if (text_texture == nullptr || ink == 0 || !text_texture->non_file) {
                std::fprintf(stderr, "0x204 text raster mismatch ink=%zu\n", ink);
                return 1;
            }

            // 入口条件：文本为空 ⇒ 不画、不报错；槽不存在 ⇒ 静默跳过。
            auto empty_text_script = make_script_with_body();
            emit_instruction(empty_text_script, 0x1F8, {{0, 1}, {0, 32}, {0, 16}, {0, 0}});
            emit_text_204(empty_text_script, 1, 0, 0, "");
            emit_text_204(empty_text_script, 99, 0, 0, "A");
            emit_instruction(empty_text_script, 2, {});
            core::Machine empty_text(0x12345678u, table);
            empty_text.load_root(empty_text_script);
            empty_text.run(8);
            std::size_t empty_ink = 0;
            const core::TextureRecord* empty_texture = empty_text.texture(1);
            if (empty_texture != nullptr) {
                for (std::size_t i = 3; i < empty_texture->rgba.size(); i += 4) {
                    if (empty_texture->rgba[i] != 0) ++empty_ink;
                }
            }
            if (empty_ink != 0 || !empty_text.errors().empty()) {
                std::fprintf(stderr, "0x204 entry-condition mismatch ink=%zu errors=%zu\n",
                             empty_ink, empty_text.errors().size());
                return 1;
            }
        }

        // 种类 2：0 号图案（左→右按道跳）把区间 A 的红与区间 B 的蓝按步序切开。
        auto wipe_script = make_script_with_body();
        emit_scene_item(wipe_script, 1, 1, 0x00FF0000u);
        emit_scene_item(wipe_script, 2, 2, 0x000000FFu);
        emit_instruction(wipe_script, 0x1FB,
                         {{0, 5}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(wipe_script, 0x1F8, {{0, 10}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(wipe_script, 0x24F,
                         {{0, 5}, {0, 10}, {0, 1}, {0, 1}, {0, 2}, {0, 1}, {0, 0}, {0, 1},
                          {0, 0}, {0, 100}});
        emit_instruction(wipe_script, 2, {});
        core::Machine wipe_machine(0x12345678u, table);
        wipe_machine.load_root(wipe_script);
        wipe_machine.run(32);
        wipe_machine.reset_scene_clock(1000);
        core::rasterize_scene(wipe_machine, 4, 4);
        wipe_machine.reset_scene_clock(1050);
        core::rasterize_scene(wipe_machine, 4, 4);
        const core::TextureRecord* wiped = wipe_machine.texture(10);
        if (wiped == nullptr || wiped->rgba[0] != 255 || wiped->rgba[2] != 0 ||
            wiped->rgba[12] != 0 || wiped->rgba[14] != 255) {
            std::fprintf(stderr, "kind2 wipe mismatch x0=(%u,%u,%u) x3=(%u,%u,%u)\n",
                         wiped ? wiped->rgba[0] : 0, wiped ? wiped->rgba[1] : 0,
                         wiped ? wiped->rgba[2] : 0, wiped ? wiped->rgba[12] : 0,
                         wiped ? wiped->rgba[13] : 0, wiped ? wiped->rgba[14] : 0);
            return 1;
        }

        // 种类 1：第一帧把覆盖区间拷进影片纹理；随后尾段按遮罩取值把透明度乘成 0（flag != 1）。
        auto movie_script = make_script_with_body();
        emit_scene_item(movie_script, 1, 1, 0x00FF0000u);
        emit_instruction(movie_script, 0x1F8, {{0, 10}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(movie_script, 0x24D,
                         {{0, 5}, {0, 10}, {0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4},
                          {0, 0}, {0, 0}, {0, 0}, {0, 100}});
        emit_instruction(movie_script, 2, {});
        core::Machine movie_machine(0x12345678u, table);
        movie_machine.load_root(movie_script);
        movie_machine.run(32);
        movie_machine.reset_scene_clock(1000);
        core::rasterize_scene(movie_machine, 4, 4);
        const core::TextureRecord* movie_texture = movie_machine.texture(10);
        if (movie_texture == nullptr || movie_texture->rgba[3] != 0 ||
            movie_texture->rgba[0] != 255) {
            std::fprintf(stderr, "kind1 first-frame mask mismatch a=%u r=%u size=%zu tex=%p\n",
                         movie_texture ? movie_texture->rgba[3] : 0,
                         movie_texture ? movie_texture->rgba[0] : 0,
                         movie_texture ? movie_texture->rgba.size() : 0,
                         static_cast<const void*>(movie_texture));
            return 1;
        }

        // ── 渲染：门前渲染（脏标记 / 0x20C / DrawMode 闸门） ──
        auto dirty_script = make_script_with_body();
        emit_instruction(dirty_script, 0x1F8, {{0, 1}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(dirty_script, 0x55, {{3, 0}, {0, 7}});
        emit_instruction(dirty_script, 2, {});
        core::ConfigStore render_config;
        render_config.set_int("set:DrawMode", 1);
        core::Machine dirty_machine(0x12345678u, table, nullptr, std::move(render_config));
        dirty_machine.load_root(dirty_script);
        if (!dirty_machine.step() || !dirty_machine.consume_frame_step()) {
            std::fprintf(stderr, "dirty scene did not request a render\n");
            return 1;
        }
        core::rasterize_scene(dirty_machine, 4, 4);  // 渲染步清脏
        if (!dirty_machine.step() || dirty_machine.consume_frame_step()) {
            std::fprintf(stderr, "clean scene still requested a render\n");
            return 1;
        }

        // 0x1F6 清场景：脏标记被清 0，不能因为“清空”再请求一帧。
        auto clear_script = make_script_with_body();
        emit_instruction(clear_script, 0x1F8, {{0, 1}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(clear_script, 0x1F6, {});
        emit_instruction(clear_script, 2, {});
        core::ConfigStore clear_config;
        clear_config.set_int("set:DrawMode", 1);
        core::Machine clear_machine(0x12345678u, table, nullptr, std::move(clear_config));
        clear_machine.load_root(clear_script);
        clear_machine.step();  // 0x1F8
        clear_machine.consume_frame_step();
        core::rasterize_scene(clear_machine, 4, 4);
        if (!clear_machine.step() || clear_machine.consume_frame_step()) {
            std::fprintf(stderr, "0x1F6 must clear the dirty flag, not set it\n");
            return 1;
        }

        auto step_script = make_script_with_body();
        emit_instruction(step_script, 0x1F8, {{0, 1}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(step_script, 0x20C, {});
        emit_instruction(step_script, 2, {});
        core::ConfigStore step_config;
        step_config.set_int("set:DrawMode", 1);
        core::Machine step_machine(0x12345678u, table, nullptr, std::move(step_config));
        step_machine.load_root(step_script);
        if (!step_machine.step() || !step_machine.consume_frame_step()) {
            std::fprintf(stderr, "0x20C setup failed\n");
            return 1;
        }
        core::rasterize_scene(step_machine, 4, 4);
        if (!step_machine.step() || !step_machine.consume_frame_step()) {
            std::fprintf(stderr, "0x20C did not force a render step\n");
            return 1;
        }

        core::ConfigStore off_config;
        off_config.set_int("set:DrawMode", 0);
        core::Machine off_machine(0x12345678u, table, nullptr, std::move(off_config));
        off_machine.load_root(dirty_script);
        if (!off_machine.step() || off_machine.consume_frame_step()) {
            std::fprintf(stderr, "DrawMode=0 still auto-rendered\n");
            return 1;
        }

        // 窗走 step() 的宿主时钟路径：不能在预跑 / 一帧多指令时被一次性推完。
        auto clock_script = make_script_with_body();
        emit_instruction(clock_script, 0x1F8, {{0, 1}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(clock_script, 0x1FB,
                         {{0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(clock_script, 0x203, {{0, 1}, {0, 0}, {0, 0}, {0, 0x00112233u}});
        emit_instruction(clock_script, 0x202,
                         {{0, 1}, {0, 100}, {0, 100}, {0, 255}, {0, 0x00FFFFFFu}});
        for (int i = 0; i < 4; ++i) emit_instruction(clock_script, 0x1A8, {});
        emit_instruction(clock_script, 2, {});
        core::Machine clock_machine(0x12345678u, table);
        clock_machine.load_root(clock_script);
        clock_machine.set_host_clock_ms(1000);
        for (int i = 0; i < 4; ++i) clock_machine.step();  // 1F8 / 1FB / 203 / 202
        clock_machine.set_host_clock_ms(1100);
        clock_machine.step();  // 盖章
        clock_machine.set_host_clock_ms(1250);
        clock_machine.step();  // 进度 0.5
        const core::DrawItem* clock_item = clock_machine.draw_item(1);
        if (clock_item == nullptr || ((core::derive_color(*clock_item, 1250, false) >> 24) & 0xFFu) != 127) {
            std::fprintf(stderr, "host-clock window did not stay in progress\n");
            return 1;
        }
        clock_machine.set_host_clock_ms(1301);
        clock_machine.step();  // 到期
        clock_item = clock_machine.draw_item(1);
        if (clock_item == nullptr || ((clock_item->color >> 24) & 0xFFu) != 255) {
            std::fprintf(stderr, "host-clock window expiry mismatch\n");
            return 1;
        }

        std::printf(
            "core-selftest: PASS (alf files=%zu, script body=%zu, instructions=%zu, opcode table=%zu)\n",
            alf.files.size(),
            script_bytes.size() - header.body_offset,
            instructions.size(),
            table.size());
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "core-selftest: FAIL (%s)\n", e.what());
        return 1;
    }
}
