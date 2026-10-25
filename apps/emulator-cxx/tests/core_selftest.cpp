#include "core/agf.h"
#include "core/alf.h"
#include "core/config.h"
#include "core/opcode_table.h"
#include "core/script.h"
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

std::vector<std::uint8_t> make_agf_1x1() {
    std::vector<std::uint8_t> bytes(24, 0);
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
