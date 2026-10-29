#include "core/agf.h"
#include "core/alf.h"
#include "core/config.h"
#include "core/encoding.h"
#include "core/install.h"
#include "core/save_host.h"
#include "core/movie.h"
#include "core/opcode_table.h"
#include "core/scene_raster.h"
#include "core/script.h"
#include "core/text_alpha.h"
#include "core/text_raster.h"
#include "core/vm.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <map>
#include <optional>
#include <string_view>
#include <cstdio>
#include <cstring>
#include <filesystem>
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

// 影片后端替身：确定性、不读文件、不出像素（像素只由产物帧决定）。
struct FakeMovieStats {
    int frames = 0;    // 产出的"新帧"次数
    int rewinds = 0;   // 循环重启次数
    int plays = 0;
    bool opened = false;
    bool closed = false;
    std::size_t bytes = 0;
};

class FakeMovieBackend final : public core::MovieBackend {
public:
    FakeMovieBackend(FakeMovieStats* stats, std::uint32_t width, std::uint32_t height,
                     std::uint8_t value)
        : stats_(stats) {
        frame_.width = width;
        frame_.height = height;
        frame_.rgba.assign(static_cast<std::size_t>(width) * height * 4u, value);
        for (std::size_t i = 3; i < frame_.rgba.size(); i += 4) frame_.rgba[i] = 255;
    }

    bool open(core::Bytes bytes, std::string&) override {
        stats_->opened = true;
        stats_->bytes = bytes.size();
        return true;
    }
    bool play() override {
        stats_->plays += 1;
        return true;
    }
    bool advance_to(double seconds) override {
        if (!(seconds > position_)) return false;
        position_ = seconds;
        if (position_ >= duration) {
            ended_ = true;
            return false;
        }
        if (!produce_frames) return false;
        stats_->frames += 1;
        return true;
    }
    double position() const override { return position_; }
    double stop_time() const override { return duration; }
    bool ended() const override { return ended_; }
    const core::MovieFrame& frame() const override { return frame_; }
    bool rewind() override {
        stats_->rewinds += 1;
        position_ = 0.0;
        ended_ = false;
        return true;
    }
    void close() override { stats_->closed = true; }

    double duration = 1.0;
    bool produce_frames = true;

private:
    FakeMovieStats* stats_;
    double position_ = 0.0;
    bool ended_ = false;
    core::MovieFrame frame_;
};

// 建一台带影片替身的机器：4×4 纹理槽 42 + `0x20f`（资源 7 / 槽 42 / 选项）。
std::vector<std::uint8_t> make_movie_script(std::uint32_t options, bool create_texture) {
    std::vector<std::uint8_t> bytes = make_script_with_body();
    if (create_texture) {
        emit_instruction(bytes, 0x1F8, {{0, 42}, {0, 4}, {0, 4}, {0, 0}});
    }
    emit_instruction(bytes, 0x20F, {{0, 7}, {0, 42}, {0, options}});
    emit_instruction(bytes, 0x1A8, {});  // 影片收尾后才执行到这里
    emit_instruction(bytes, 2, {});
    return bytes;
}

// ── 命名表（0xA1 / 0xA2 / 0xA3；CORE-OPS-TABLE-RESET-A1 / -BIND-A2 / -DISPATCH-A3） ──
bool check_menu_table(const core::OpcodeTable& table) {
    auto menu = make_script_with_body();
    emit_instruction(menu, 0x55, {{3, 0}, {0, 42}});  // 位置 0..4：全局整型 0 ← 42
    emit_instruction(menu, 0xA2, {{3, 0}, {0, 15}});  // 位置 5..9：键"42" → 15
    emit_instruction(menu, 0xA3, {{3, 0}, {0, 0}});   // 位置 10..14：命中 ⇒ ip ← 60
    emit_instruction(menu, 0x1A8, {});                // 位置 15
    emit_instruction(menu, 2, {});                    // 位置 16
    core::Machine menu_machine(0x12345678u, table);
    menu_machine.load_root(menu);
    if (!menu_machine.step() || !menu_machine.step()) {
        std::fprintf(stderr, "0xa2 bind fixture failed: %s\n",
                     menu_machine.last_stop().message.c_str());
        return false;
    }
    const auto& named = menu_machine.named_table();
    const auto bound = named.find("42");
    if (named.size() != 1 || bound == named.end() || bound->second != 15) {
        std::fprintf(stderr, "0xa2 named-table bind mismatch (size=%zu)\n", named.size());
        return false;
    }
    if (!menu_machine.step()) {  // 0xA3
        std::fprintf(stderr, "0xa3 dispatch failed: %s\n",
                     menu_machine.last_stop().message.c_str());
        return false;
    }
    if (menu_machine.frame().ip != 60 || menu_machine.frame().length_slot != 0) {
        std::fprintf(stderr, "0xa3 hit dispatch mismatch ip=%zu length=%u\n",
                     menu_machine.frame().ip, menu_machine.frame().length_slot);
        return false;
    }

    // 命中时第 2 个操作数**不被读**：给它一个不可读的类型（8）也不许抛。
    auto menu_hit = make_script_with_body();
    emit_instruction(menu_hit, 0x55, {{3, 0}, {0, 42}});
    emit_instruction(menu_hit, 0xA2, {{3, 0}, {0, 15}});
    emit_instruction(menu_hit, 0xA3, {{3, 0}, {8, 0}});
    emit_instruction(menu_hit, 0x1A8, {});
    emit_instruction(menu_hit, 2, {});
    core::Machine hit_machine(0x12345678u, table);
    hit_machine.load_root(menu_hit);
    for (int i = 0; i < 3; ++i) {
        if (!hit_machine.step()) {
            std::fprintf(stderr, "0xa3 read the default position on a hit: %s\n",
                         hit_machine.last_stop().message.c_str());
            return false;
        }
    }
    if (hit_machine.frame().ip != 60) {
        std::fprintf(stderr, "0xa3 hit-with-bad-default ip=%zu\n", hit_machine.frame().ip);
        return false;
    }

    // 0xA1 清空后，同一键不再命中 ⇒ 走默认位置。
    auto menu_clear = make_script_with_body();
    emit_instruction(menu_clear, 0x55, {{3, 0}, {0, 42}});  // 位置 0..4
    emit_instruction(menu_clear, 0xA2, {{3, 0}, {0, 7}});   // 位置 5..9
    emit_instruction(menu_clear, 0xA1, {});                 // 位置 10
    emit_instruction(menu_clear, 0xA3, {{3, 0}, {0, 16}});  // 位置 11..15：默认 16
    emit_instruction(menu_clear, 0x1A8, {});                // 位置 16
    emit_instruction(menu_clear, 2, {});                    // 位置 17
    core::Machine clear_machine(0x12345678u, table);
    clear_machine.load_root(menu_clear);
    for (int i = 0; i < 4; ++i) {  // 0x55 / 0xA2 / 0xA1 / 0xA3
        if (!clear_machine.step()) {
            std::fprintf(stderr, "0xa1 reset fixture failed: %s\n",
                         clear_machine.last_stop().message.c_str());
            return false;
        }
    }
    if (!clear_machine.named_table().empty() || clear_machine.frame().ip != 64) {
        std::fprintf(stderr, "0xa1 reset mismatch (size=%zu ip=%zu)\n",
                     clear_machine.named_table().size(), clear_machine.frame().ip);
        return false;
    }
    return true;
}

// ── 状态字 / 模式族 / 清位（0x1CF · 0x1BF · 0x19C · 0x19B · 0x0D9 · 0x1CE） ──
bool check_status_and_mode(const core::OpcodeTable& table) {
    // 0x1CF 原样覆盖 + 0x1BF **整字**清 0、第 0 位判据在清字之后。
    const auto make_status_script = [&](std::int32_t word) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x1CF, {{0, static_cast<std::uint32_t>(word)}});
        emit_instruction(bytes, 0x1BF, {});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    core::Machine status_a(0x12345678u, table);
    status_a.load_root(make_status_script(0x10002));
    if (!status_a.step() || !status_a.step()) {
        std::fprintf(stderr, "status fixture failed: %s\n",
                     status_a.last_stop().message.c_str());
        return false;
    }
    if (status_a.status_word() != 0 || status_a.status_neighbor() != 1 ||
        status_a.state_flags() != 0) {
        std::fprintf(stderr, "0x1bf whole-word clear mismatch word=%d neighbor=%d\n",
                     status_a.status_word(), status_a.status_neighbor());
        return false;
    }
    core::Machine status_b(0x12345678u, table);
    status_b.load_root(make_status_script(0x10001));
    if (!status_b.step() || !status_b.step() || status_b.status_word() != 0 ||
        status_b.status_neighbor() != 1) {
        std::fprintf(stderr, "0x1bf order (clear-then-test-bit0) mismatch\n");
        return false;
    }
    // 第 0 位为 1 ⇒ 相邻格**不动**（不是写 0）。
    auto status_neighbor = make_script_with_body();
    emit_instruction(status_neighbor, 0x1CF, {{0, 0}});
    emit_instruction(status_neighbor, 0x1BF, {});
    emit_instruction(status_neighbor, 0x1CF, {{0, 1}});
    emit_instruction(status_neighbor, 0x1BF, {});
    emit_instruction(status_neighbor, 2, {});
    core::Machine status_c(0x12345678u, table);
    status_c.load_root(status_neighbor);
    for (int i = 0; i < 4; ++i) {
        if (!status_c.step()) {
            std::fprintf(stderr, "status c fixture failed: %s\n",
                         status_c.last_stop().message.c_str());
            return false;
        }
    }
    if (status_c.status_word() != 1 || status_c.status_neighbor() != 1) {
        std::fprintf(stderr, "0x1bf neighbor overwrite mismatch word=%d neighbor=%d\n",
                     status_c.status_word(), status_c.status_neighbor());
        return false;
    }
    // 0x1cf 是覆盖而不是按位或：先写 −1 再写 0，整字必须变成 0。
    auto status_overwrite = make_script_with_body();
    emit_instruction(status_overwrite, 0x1CF, {{0, 0xFFFFFFFFu}});
    emit_instruction(status_overwrite, 0x1CF, {{0, 0}});
    emit_instruction(status_overwrite, 2, {});
    core::Machine status_d(0x12345678u, table);
    status_d.load_root(status_overwrite);
    if (!status_d.step() || !status_d.step() || status_d.status_word() != 0) {
        std::fprintf(stderr, "0x1cf overwrite mismatch word=%d\n", status_d.status_word());
        return false;
    }

    // 0x19C 的四支：C 置 D、E 不碰 D、F 什么都不动、全 0 支清 D + 清第 27 位。
    const auto make_mode_script = [&]() {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x19C, {});
        emit_instruction(bytes, 0x19B, {});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    core::Machine mode_none(0x12345678u, table);
    mode_none.load_root(make_mode_script());
    if (!mode_none.step()) return false;
    if (mode_none.mode_a() != 1 || mode_none.mode_b() != 0 || mode_none.mode_d() != 0 ||
        mode_none.mode_g() != 0 || (mode_none.state_flags() & (1u << 27)) != 0) {
        std::fprintf(stderr, "0x19c all-zero branch mismatch\n");
        return false;
    }
    if (!mode_none.step() || mode_none.mode_a() != 0) {
        std::fprintf(stderr, "0x19b did not clear mode A\n");
        return false;
    }

    core::Machine mode_c(0x12345678u, table);
    mode_c.seed_mode_cells(1, 0, 0);
    mode_c.load_root(make_mode_script());
    if (!mode_c.step() || mode_c.mode_d() != 1 || mode_c.mode_g() != 1 ||
        (mode_c.state_flags() & (1u << 27)) == 0) {
        std::fprintf(stderr, "0x19c C branch mismatch d=%d g=%d\n", mode_c.mode_d(),
                     mode_c.mode_g());
        return false;
    }
    core::Machine mode_e(0x12345678u, table);
    mode_e.seed_mode_cells(0, 1, 0);
    mode_e.load_root(make_mode_script());
    if (!mode_e.step() || mode_e.mode_d() != 0 || mode_e.mode_g() != 1 ||
        (mode_e.state_flags() & (1u << 27)) == 0) {
        std::fprintf(stderr, "0x19c E branch touched D (d=%d)\n", mode_e.mode_d());
        return false;
    }
    core::Machine mode_f(0x12345678u, table);
    mode_f.seed_mode_cells(0, 0, 1);
    mode_f.load_root(make_mode_script());
    if (!mode_f.step() || mode_f.mode_a() != 1 || mode_f.mode_b() != 0 ||
        mode_f.mode_g() != 0 || (mode_f.state_flags() & (1u << 27)) != 0) {
        std::fprintf(stderr, "0x19c F branch changed the mode bit\n");
        return false;
    }

    // 0x0d9：清门字第 12 位；那一格非 0 才另一个字清第 12 位（读-改-写，别整字写 0）。
    auto d9_script = make_script_with_body();
    emit_instruction(d9_script, 0xD9, {});
    emit_instruction(d9_script, 2, {});
    core::Machine d9_machine(0x12345678u, table);
    d9_machine.seed_d9_cells(1, (1 << 12) | (1 << 14));
    d9_machine.load_root(d9_script);
    if (!d9_machine.step() || d9_machine.d9_word() != (1 << 14) ||
        d9_machine.state_flags() != 0) {
        std::fprintf(stderr, "0x0d9 bit-12 clear mismatch word=0x%X\n", d9_machine.d9_word());
        return false;
    }
    core::Machine d9_empty(0x12345678u, table);
    d9_empty.seed_d9_cells(0, (1 << 12));
    d9_empty.load_root(d9_script);
    if (!d9_empty.step() || d9_empty.d9_word() != (1 << 12)) {
        std::fprintf(stderr, "0x0d9 cleared the word while its cell was 0\n");
        return false;
    }

    // 0x1ce：非 0 ⇒ 开关格 / 第 30 位 / 已启用 / 种子（宿主时钟）；门随后恒关。
    auto rng_script = make_script_with_body();
    emit_instruction(rng_script, 0x1CE, {{0, 7}});
    emit_instruction(rng_script, 2, {});
    core::Machine rng_machine(0x12345678u, table);
    rng_machine.set_host_clock_ms(1234);
    rng_machine.load_root(rng_script);
    if (!rng_machine.step() || rng_machine.rng_switch() != 7 ||
        rng_machine.rng_enabled() != 1 || rng_machine.rng_seed() != 1234u ||
        (rng_machine.state_flags() & (1u << 30)) == 0 ||
        rng_machine.frame().ip != 12) {
        std::fprintf(stderr, "0x1ce enable mismatch switch=%d seed=%u ip=%zu\n",
                     rng_machine.rng_switch(), rng_machine.rng_seed(),
                     rng_machine.frame().ip);
        return false;
    }
    const auto rng_ip = rng_machine.frame().ip;
    if (!rng_machine.step() || rng_machine.frame().ip != rng_ip) {
        std::fprintf(stderr, "0x1ce did not close the gate (ip moved)\n");
        return false;
    }
    auto rng_zero = make_script_with_body();
    emit_instruction(rng_zero, 0x1CE, {{0, 0}});
    emit_instruction(rng_zero, 2, {});
    core::Machine rng_off(0x12345678u, table);
    rng_off.set_host_clock_ms(9);
    rng_off.load_root(rng_zero);  // 开关 0 且第 30 位未置 ⇒ 空操作
    if (!rng_off.step() || rng_off.rng_enabled() != 0 || rng_off.rng_seed() != 0 ||
        (rng_off.state_flags() & (1u << 30)) != 0) {
        std::fprintf(stderr, "0x1ce zero-branch was not a no-op\n");
        return false;
    }

    // 门一直关着、且挡门的位**没有服务分支**（位 27 = `0x19c` 置位支）⇒ 必须给明确诊断与停因，
    // 否则外面只看到"停在原地 + 烧步数"（与"在等输入"分不开）。
    auto stall_script = make_script_with_body();
    emit_instruction(stall_script, 0x19C, {});
    emit_instruction(stall_script, 2, {});
    core::Machine stall_machine(0x12345678u, table);
    stall_machine.seed_mode_cells(1, 0, 0);
    stall_machine.load_root(stall_script);
    if (!stall_machine.step() || (stall_machine.state_flags() & (1u << 27)) == 0) {
        std::fprintf(stderr, "gate stall fixture did not set bit 27\n");
        return false;
    }
    for (int i = 0; i < 10001 && !stall_machine.halted(); ++i) stall_machine.step();
    if (!stall_machine.halted() ||
        stall_machine.last_stop().reason != core::StopReason::GateStalled ||
        stall_machine.errors().empty()) {
        std::fprintf(stderr, "gate stall was not diagnosed (halted=%d reason=%d errors=%zu)\n",
                     stall_machine.halted() ? 1 : 0,
                     static_cast<int>(stall_machine.last_stop().reason),
                     stall_machine.errors().size());
        return false;
    }
    return true;
}

// ── 执行区间（0x1F4 / 0x1F5；CORE-OPS-RUN-BRACKET） ──
bool check_run_bracket(const core::OpcodeTable& table) {
    auto bracket = make_script_with_body();
    emit_instruction(bracket, 0x1F4, {});
    emit_instruction(bracket, 0x1F4, {});
    emit_instruction(bracket, 0x1F5, {});
    emit_instruction(bracket, 0x1F5, {});
    emit_instruction(bracket, 2, {});
    core::Machine bracket_machine(0x12345678u, table);
    bracket_machine.set_host_clock_ms(1000);
    bracket_machine.load_root(bracket);
    if (!bracket_machine.step() || !bracket_machine.bracket_busy() ||
        bracket_machine.bracket_depth() != 0 ||
        bracket_machine.engine_now_ms() != 1000u ||
        bracket_machine.engine_previous_ms() != 0u) {
        std::fprintf(stderr, "0x1f4 enter mismatch\n");
        return false;
    }
    bracket_machine.set_host_clock_ms(1040);
    if (!bracket_machine.step() || bracket_machine.bracket_depth() != 1 ||
        bracket_machine.engine_now_ms() != 1000u ||
        bracket_machine.engine_previous_ms() != 0u) {
        std::fprintf(stderr, "0x1f4 busy branch read the clock or moved the timestamps\n");
        return false;
    }
    if (!bracket_machine.step() || bracket_machine.bracket_depth() != 0 ||
        !bracket_machine.bracket_busy()) {
        std::fprintf(stderr, "0x1f5 depth branch cleared the busy mark\n");
        return false;
    }
    if (!bracket_machine.step() || bracket_machine.bracket_busy()) {
        std::fprintf(stderr, "0x1f5 leave did not clear the busy mark\n");
        return false;
    }
    // 未进入就离开 = 空操作（不清忙、不读时钟）。
    auto lone_leave = make_script_with_body();
    emit_instruction(lone_leave, 0x1F5, {});
    emit_instruction(lone_leave, 2, {});
    core::Machine lone_machine(0x12345678u, table);
    lone_machine.set_host_clock_ms(2000);
    lone_machine.load_root(lone_leave);
    if (!lone_machine.step() || lone_machine.bracket_busy() ||
        lone_machine.engine_now_ms() != 0u) {
        std::fprintf(stderr, "0x1f5 unpaired leave was not a no-op\n");
        return false;
    }
    // 交叉判据：区间忙标记与 `0x20c` 的「别处驱动」是**同一格** ⇒ 忙时段里 `0x20c` 不更新时间戳。
    // 若实现把它们当成两格，这条会红（而单看任何一条都"看起来正常"）。
    auto busy_step = make_script_with_body();
    emit_instruction(busy_step, 0x1F4, {});
    emit_instruction(busy_step, 0x20C, {});
    emit_instruction(busy_step, 2, {});
    core::Machine busy_machine(0x12345678u, table);
    busy_machine.set_host_clock_ms(1000);
    busy_machine.load_root(busy_step);
    if (!busy_machine.step() || !busy_machine.bracket_busy()) return false;
    busy_machine.set_host_clock_ms(1040);
    if (!busy_machine.step() || busy_machine.engine_now_ms() != 1000u ||
        busy_machine.engine_previous_ms() != 0u || !busy_machine.frame_step_requested()) {
        std::fprintf(stderr, "0x20c updated the bracket timestamps while busy\n");
        return false;
    }
    return true;
}

// ── 输入采样 / 按键派发（0xFF · 0x100；CORE-INPUT-WORD / -KEY-DISPATCH） ──
bool check_input_dispatch(const core::OpcodeTable& table) {
    // 键总数初值 7（引擎 Input 构造）：0xfe 之前 0x100 / 0xff 就读这个值。
    core::Machine default_machine(0x12345678u, table);
    if (default_machine.key_total() != 7) {
        std::fprintf(stderr, "key total default must be 7, got %d\n", default_machine.key_total());
        return false;
    }

    // 0x101：先向平台采样一次，再清第 27 位 / 清输入字 / 写 A、B。
    auto poll = make_script_with_body();
    emit_instruction(poll, 0x101, {});
    emit_instruction(poll, 2, {});
    core::Machine poll_machine(0x12345678u, table);
    int poll_samples = 0;
    poll_machine.set_key_sampler([&poll_samples] {
        ++poll_samples;
        return 0b101u;
    });
    poll_machine.load_root(poll);
    if (!poll_machine.step() || poll_samples != 1 || poll_machine.input_word() != 0 ||
        poll_machine.input_a() != 1 || poll_machine.input_b() != 0) {
        std::fprintf(stderr, "0x101 must sample once then discard; samples=%d word=0x%X A=%d B=%d\n",
                     poll_samples, poll_machine.input_word(), poll_machine.input_a(),
                     poll_machine.input_b());
        return false;
    }

    auto sample = make_script_with_body();
    emit_instruction(sample, 0xFE, {{0, 2}});
    emit_instruction(sample, 0xFF, {});
    emit_instruction(sample, 2, {});
    core::Machine sample_machine(0x12345678u, table);
    sample_machine.load_root(sample);
    if (!sample_machine.step() || !sample_machine.step()) return false;
    if (sample_machine.key_total() != 2 || sample_machine.frame().key_limit != 2 ||
        sample_machine.frame().key_cursor != 0 || sample_machine.input_word() != 0) {
        std::fprintf(stderr, "0xff sample mismatch limit=%d word=0x%X\n",
                     sample_machine.frame().key_limit, sample_machine.input_word());
        return false;
    }
    core::Machine sampler_machine(0x12345678u, table);
    sampler_machine.set_key_sampler([] { return 0b101u; });
    sampler_machine.load_root(sample);
    if (!sampler_machine.step() || !sampler_machine.step() ||
        sampler_machine.input_word() != 0b101u ||
        sampler_machine.frame().key_limit != 2) {
        std::fprintf(stderr, "0xff sampler mismatch word=0x%X limit=%d\n",
                     sampler_machine.input_word(), sampler_machine.frame().key_limit);
        return false;
    }

    // 无按键 ⇒ 走"无按键"表项（编号 = 键总数），返回位置 = 下一条。
    auto dispatch_none = make_script_with_body();
    emit_instruction(dispatch_none, 0xFE, {{0, 2}});            // 0..2
    emit_instruction(dispatch_none, 0xFB, {{0, 2}, {0, 9}});    // 3..7：key_jumps[2] = 9
    emit_instruction(dispatch_none, 0x100, {});                 // 8
    emit_instruction(dispatch_none, 0x1A8, {});                 // 9
    emit_instruction(dispatch_none, 2, {});                     // 10
    core::Machine none_machine(0x12345678u, table);
    none_machine.load_root(dispatch_none);
    for (int i = 0; i < 3; ++i) {
        if (!none_machine.step()) {
            std::fprintf(stderr, "0x100 no-key fixture failed: %s\n",
                         none_machine.last_stop().message.c_str());
            return false;
        }
    }
    if (none_machine.frame().ip != 36 || none_machine.frame().ret_stack.empty() ||
        none_machine.frame().ret_stack.back() != 9) {
        std::fprintf(stderr, "0x100 no-key dispatch mismatch ip=%zu\n",
                     none_machine.frame().ip);
        return false;
    }

    // 有按键：游标前进、返回位置 = **当前位置**（重派发）。
    auto dispatch_key = make_script_with_body();
    emit_instruction(dispatch_key, 0xFE, {{0, 2}});             // 0..2
    emit_instruction(dispatch_key, 0xFB, {{0, 0}, {0, 9}});     // 3..7：key_jumps[0] = 9
    emit_instruction(dispatch_key, 0x100, {});                  // 8
    emit_instruction(dispatch_key, 0x1A8, {});                  // 9
    emit_instruction(dispatch_key, 2, {});                      // 10
    core::Machine key_machine(0x12345678u, table);
    key_machine.load_root(dispatch_key);
    if (!key_machine.step() || !key_machine.step()) return false;
    key_machine.set_input_word(0b101u);
    if (!key_machine.step() || key_machine.frame().key_cursor != 1 ||
        key_machine.frame().ip != 36 || key_machine.frame().ret_stack.empty() ||
        key_machine.frame().ret_stack.back() != 8) {
        std::fprintf(stderr, "0x100 key dispatch mismatch cursor=%d ip=%zu\n",
                     key_machine.frame().key_cursor, key_machine.frame().ip);
        return false;
    }
    // 未登记 ⇒ 游标**照样**推进，但放弃本轮剩余置位（只前进一条）。
    core::Machine unreg_machine(0x12345678u, table);
    unreg_machine.load_root(dispatch_key);
    if (!unreg_machine.step() || !unreg_machine.step()) return false;
    unreg_machine.set_input_word(0b010u);
    if (!unreg_machine.step() || unreg_machine.frame().key_cursor != 2 ||
        unreg_machine.frame().ip != 36 || !unreg_machine.frame().ret_stack.empty()) {
        std::fprintf(stderr, "0x100 unregistered key mismatch cursor=%d\n",
                     unreg_machine.frame().key_cursor);
        return false;
    }
    return true;
}

// ── 键盘 / 鼠标设备（0x109 · 0x108 · 0xCC · 0xCD） ──
// `CORE-INPUT-MOUSE-POS` / `-MOUSE-BUTTON` / `-CALLBACK-REGISTER` / `-MOUSE-CALLBACK-PUMP`。
bool check_input_devices(const core::OpcodeTable& table) {
    // 0x109：设备可用 ⇒ 写客户区坐标；不可用 ⇒ 两个都写 −100000（哨兵，不是"不写"）。
    auto cursor_script = make_script_with_body();
    emit_instruction(cursor_script, 0x109, {{3, 0}, {3, 1}});
    emit_instruction(cursor_script, 2, {});
    core::Machine cursor_machine(0x12345678u, table);
    cursor_machine.set_cursor_source([](std::int32_t& x, std::int32_t& y) {
        x = 640;
        y = 360;
        return true;
    });
    cursor_machine.load_root(cursor_script);
    if (!cursor_machine.step() || cursor_machine.read_int(core::Operand{3, 0}) != 640 ||
        cursor_machine.read_int(core::Operand{3, 1}) != 360) {
        std::fprintf(stderr, "0x109 cursor mismatch (%d,%d)\n",
                     cursor_machine.read_int(core::Operand{3, 0}),
                     cursor_machine.read_int(core::Operand{3, 1}));
        return false;
    }
    // 光标在客户区外仍是“源可用” ⇒ 必须写真实坐标（可为负），不是 -100000。
    core::Machine outside_machine(0x12345678u, table);
    outside_machine.set_cursor_source([](std::int32_t& x, std::int32_t& y) {
        x = -7;
        y = 760;
        return true;
    });
    outside_machine.load_root(cursor_script);
    if (!outside_machine.step() || outside_machine.read_int(core::Operand{3, 0}) != -7 ||
        outside_machine.read_int(core::Operand{3, 1}) != 760) {
        std::fprintf(stderr, "0x109 outside-client coords must stay real\n");
        return false;
    }
    core::Machine no_cursor_machine(0x12345678u, table);
    no_cursor_machine.load_root(cursor_script);
    if (!no_cursor_machine.step() || no_cursor_machine.read_int(core::Operand{3, 0}) != -100000 ||
        no_cursor_machine.read_int(core::Operand{3, 1}) != -100000) {
        std::fprintf(stderr, "0x109 unavailable-device sentinel mismatch\n");
        return false;
    }

    // 0x108：位 0 左 / 位 1 右；设备不可用 ⇒ 写 0（不是保持旧值）。
    auto button_script = make_script_with_body();
    emit_instruction(button_script, 0x55, {{3, 2}, {0, 99}});
    emit_instruction(button_script, 0x108, {{3, 2}});
    emit_instruction(button_script, 2, {});
    core::Machine button_machine(0x12345678u, table);
    button_machine.set_mouse_button_source([](std::int32_t& mask) {
        mask = 0b11;
        return true;
    });
    button_machine.load_root(button_script);
    if (!button_machine.step() || !button_machine.step() ||
        button_machine.read_int(core::Operand{3, 2}) != 0b11) {
        std::fprintf(stderr, "0x108 button bitmap mismatch (%d)\n",
                     button_machine.read_int(core::Operand{3, 2}));
        return false;
    }
    core::Machine no_button_machine(0x12345678u, table);
    no_button_machine.load_root(button_script);
    if (!no_button_machine.step() || !no_button_machine.step() ||
        no_button_machine.read_int(core::Operand{3, 2}) != 0) {
        std::fprintf(stderr, "0x108 unavailable-device must write 0\n");
        return false;
    }

    // 0xcc / 0xcd：时间闸 `elapsed >= interval`（含等号）才派发；压「当前位置 + 1」。
    // ★ 两次 pump 要两条 `0xcd`（一条指令执行完 ip 就过去了，不能靠"再跑一次同一条"）。
    auto mouse_cb = make_script_with_body();
    emit_instruction(mouse_cb, 0xCC, {{0, 100}, {0, 9}});  // 位置 0..4：间隔 100、目标 9
    emit_instruction(mouse_cb, 0xCD, {});                  // 位置 5：第一次（未到期）
    emit_instruction(mouse_cb, 0xCD, {});                  // 位置 6：第二次（到期）
    emit_instruction(mouse_cb, 0x1A8, {});                 // 位置 7（回调返回点）
    emit_instruction(mouse_cb, 2, {});                     // 位置 8
    emit_instruction(mouse_cb, 0x1A8, {});                 // 位置 9（回调目标）
    emit_instruction(mouse_cb, 5, {});                     // 位置 10：ret → 回 7
    core::Machine pump_machine(0x12345678u, table);
    pump_machine.set_host_clock_ms(1000);
    pump_machine.load_root(mouse_cb, 0x1234);
    if (!pump_machine.step() || pump_machine.mouse_interval() != 100 ||
        pump_machine.mouse_target() != 9 || pump_machine.mouse_owner() != 0x1234 ||
        pump_machine.mouse_last_ms() != 1000u) {
        std::fprintf(stderr, "0xcc registration mismatch interval=%d target=%d owner=%d last=%u\n",
                     pump_machine.mouse_interval(), pump_machine.mouse_target(),
                     pump_machine.mouse_owner(), pump_machine.mouse_last_ms());
        return false;
    }
    pump_machine.set_host_clock_ms(1050);  // 50 < 100 ⇒ 不派发
    if (!pump_machine.step() || pump_machine.frame().ip != 24 ||
        !pump_machine.frame().ret_stack.empty()) {
        std::fprintf(stderr, "0xcd dispatched before the interval elapsed (ip=%zu)\n",
                     pump_machine.frame().ip);
        return false;
    }
    pump_machine.set_host_clock_ms(1100);  // 100 >= 100（含等号）⇒ 派发
    if (!pump_machine.step() || pump_machine.frame().ip != 36 ||
        pump_machine.frame().ret_stack.empty() || pump_machine.frame().ret_stack.back() != 7 ||
        pump_machine.mouse_last_ms() != 1100u) {
        std::fprintf(stderr, "0xcd dispatch mismatch ip=%zu last=%u stack=%zu\n",
                     pump_machine.frame().ip, pump_machine.mouse_last_ms(),
                     pump_machine.frame().ret_stack.size());
        return false;
    }
    // 回调体（位置 9 的 0x1A8）走完 ⇒ ip = 40（位置 10 的 ret）。
    if (!pump_machine.step() || pump_machine.frame().ip != 40) {
        std::fprintf(stderr, "0xcd callback body mismatch ip=%zu\n", pump_machine.frame().ip);
        return false;
    }

    // 间隔 0 ⇒ 写成 1；目标 −1 ⇒ 压了返回位置又撤销（只前进一条，返回栈不变）。
    auto cancel_script = make_script_with_body();
    emit_instruction(cancel_script, 0xCC, {{0, 0}, {0, 0xFFFFFFFFu}});
    emit_instruction(cancel_script, 0xCD, {});
    emit_instruction(cancel_script, 2, {});
    core::Machine cancel_machine(0x12345678u, table);
    cancel_machine.set_host_clock_ms(2000);
    cancel_machine.load_root(cancel_script);
    if (!cancel_machine.step() || cancel_machine.mouse_interval() != 1) {
        std::fprintf(stderr, "0xcc interval 0 must become 1 (got %d)\n",
                     cancel_machine.mouse_interval());
        return false;
    }
    cancel_machine.set_host_clock_ms(2100);
    if (!cancel_machine.step() || cancel_machine.frame().ip != 24 ||
        !cancel_machine.frame().ret_stack.empty()) {
        std::fprintf(stderr, "0xcd cancel sentinel mismatch ip=%zu stack=%zu\n",
                     cancel_machine.frame().ip, cancel_machine.frame().ret_stack.size());
        return false;
    }
    return true;
}

// ── 暂停（= 区间忙）与时钟重盖（CORE-VM-LOOP · CORE-TIMING-CLOCK 的 0x23C） ──
bool check_pause_and_clock(const core::OpcodeTable& table) {
    // 暂停 = 区间忙标记 = 1 **且等待位 = 0** ⇒ 场景推进整段跳过（窗不盖 / 不插值 / 不到期）。
    const auto make_pause_script = [&](bool release) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x1FB, {{0, 1}, {0, 1}, {0, 0}, {0, 0},
                                        {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(bytes, 0x202, {{0, 1}, {0, 0}, {0, 100}, {0, 200}, {0, 0x00ABCDEFu}});
        emit_instruction(bytes, 0x1F4, {});
        if (release) emit_instruction(bytes, 0x21C, {});
        const std::uint32_t loop_position =
            static_cast<std::uint32_t>((bytes.size() - 60) / 4);  // 自环：位置 = 本条自身
        emit_instruction(bytes, 0x8C, {{0, loop_position}});
        emit_instruction(bytes, 2, {});
        return bytes;
    };

    core::Machine paused_machine(0x12345678u, table);
    paused_machine.set_host_clock_ms(1000);
    paused_machine.load_root(make_pause_script(false));
    for (int i = 0; i < 3; ++i) {  // 0x1FB / 0x202 / 0x1F4
        if (!paused_machine.step()) {
            std::fprintf(stderr, "pause fixture failed: %s\n",
                         paused_machine.last_stop().message.c_str());
            return false;
        }
    }
    paused_machine.set_host_clock_ms(1200);  // 窗本应在 1100 之后到期
    if (!paused_machine.step()) return false;
    const core::DrawItem* paused_item = paused_machine.draw_item(1);
    if (paused_item == nullptr || paused_item->color == 0xC8ABCDEFu ||
        paused_item->duration != 100) {
        std::fprintf(stderr, "paused scene advanced the window (color=0x%08X duration=%d)\n",
                     paused_item == nullptr ? 0u : paused_item->color,
                     paused_item == nullptr ? -1 : paused_item->duration);
        return false;
    }

    // 等待位置起 ⇒ 暂停不挡场景推进（同一时刻、同一窗必须到期）。
    core::Machine released_machine(0x12345678u, table);
    released_machine.set_host_clock_ms(1000);
    released_machine.load_root(make_pause_script(true));
    for (int i = 0; i < 4; ++i) {  // 0x1FB / 0x202 / 0x1F4 / 0x21C
        if (!released_machine.step()) return false;
    }
    released_machine.set_host_clock_ms(1200);
    if (!released_machine.step()) return false;
    const core::DrawItem* released_item = released_machine.draw_item(1);
    if (released_item == nullptr || released_item->color != 0xC8ABCDEFu ||
        released_item->duration != 0) {
        std::fprintf(stderr, "wait bit did not release the scene advance (color=0x%08X)\n",
                     released_item == nullptr ? 0u : released_item->color);
        return false;
    }

    // 0x23C：无条件重盖时钟对（不受区间忙门限制）。
    auto clock_script = make_script_with_body();
    emit_instruction(clock_script, 0x23C, {});
    emit_instruction(clock_script, 0x23C, {});
    emit_instruction(clock_script, 2, {});
    core::Machine clock_machine(0x12345678u, table);
    clock_machine.set_host_clock_ms(700);
    clock_machine.load_root(clock_script);
    if (!clock_machine.step() || clock_machine.engine_now_ms() != 700u ||
        clock_machine.engine_previous_ms() != 0u) {
        std::fprintf(stderr, "0x23c first stamp mismatch now=%u prev=%u\n",
                     clock_machine.engine_now_ms(), clock_machine.engine_previous_ms());
        return false;
    }
    clock_machine.set_host_clock_ms(800);
    if (!clock_machine.step() || clock_machine.engine_now_ms() != 800u ||
        clock_machine.engine_previous_ms() != 700u) {
        std::fprintf(stderr, "0x23c " "prev<-now" " mismatch now=%u prev=%u\n",
                     clock_machine.engine_now_ms(), clock_machine.engine_previous_ms());
        return false;
    }
    return true;
}

// ── 帧 / 本地块 / 标志 / 数值 / 场景读回（第二批 24 条） ──
bool check_second_batch(const core::OpcodeTable& table) {
    // 0x08 call-frame：目标槽未装载 ⇒ 只记诊断、不切帧；长度 3 已写 ⇒ 推进 12 字节。
    auto call_frame = make_script_with_body();
    emit_instruction(call_frame, 0x08, {{0, 1}});
    emit_instruction(call_frame, 2, {});
    core::Machine call_machine(0x12345678u, table);
    call_machine.load_root(call_frame);
    if (!call_machine.step() || call_machine.current_frame() != 0 ||
        call_machine.errors().size() != 1 || call_machine.frame().ip != 12) {
        std::fprintf(stderr, "0x08 unloaded slot mismatch frame=%zu ip=%zu errors=%zu\n",
                     call_machine.current_frame(), call_machine.frame().ip,
                     call_machine.errors().size());
        return false;
    }

    // 0x7b：两格按帧、初值 −1；0x199 进入；0x7c 离开（深度 = 本帧身份编号）。
    auto local = make_script_with_body();
    emit_instruction(local, 0x7B, {{0, 8}, {0, 0xFFFFFFFFu}});  // 位置 0..4：main=8、sub=−1
    emit_instruction(local, 0x199, {});                         // 位置 5
    emit_instruction(local, 0x1A8, {});                         // 位置 6（返回点）
    emit_instruction(local, 2, {});                             // 位置 7
    emit_instruction(local, 0x7C, {});                          // 位置 8（目标）
    emit_instruction(local, 2, {});                             // 位置 9
    core::Machine local_machine(0x12345678u, table);
    local_machine.load_root(local);
    if (local_machine.frame().local_main != -1 || local_machine.frame().local_sub != -1) {
        std::fprintf(stderr, "0x7b frame defaults must be -1\n");
        return false;
    }
    if (!local_machine.step() || local_machine.frame().local_main != 8 ||
        local_machine.frame().local_sub != -1 || local_machine.frame().ip != 20) {
        std::fprintf(stderr, "0x7b targets mismatch main=%d sub=%d ip=%zu\n",
                     local_machine.frame().local_main, local_machine.frame().local_sub,
                     local_machine.frame().ip);
        return false;
    }
    if (!local_machine.step() || local_machine.frame().ip != 32 ||
        local_machine.local_save_pos() != 6 || local_machine.local_save_depth() != 0 ||
        local_machine.local_save_word() != ((1u << 25) | (1u << 26)) ||
        local_machine.state_flags() != 0) {
        std::fprintf(stderr, "0x199 enter mismatch ip=%zu pos=%u word=0x%X\n",
                     local_machine.frame().ip, local_machine.local_save_pos(),
                     local_machine.local_save_word());
        return false;
    }
    if (!local_machine.step() || local_machine.frame().ip != 24 ||
        local_machine.state_flags() != (1u << 26) || local_machine.local_save_word() != 0) {
        std::fprintf(stderr, "0x7c leave mismatch ip=%zu flags=0x%X\n",
                     local_machine.frame().ip, local_machine.state_flags());
        return false;
    }
    // 门带着第 26 位 ⇒ 没有服务分支 ⇒ 一定停成 GateStalled（正确表现）。
    for (int i = 0; i < 10001 && !local_machine.halted(); ++i) local_machine.step();
    if (!local_machine.halted() ||
        local_machine.last_stop().reason != core::StopReason::GateStalled) {
        std::fprintf(stderr, "0x7c did not leave the gate closed with bit 26\n");
        return false;
    }

    // 0x19a / 0x1b6 / 0x1b7：三个互不相同的格；Y 归一成 0/1。
    auto flags = make_script_with_body();
    emit_instruction(flags, 0x1B7, {{0, 5}});
    emit_instruction(flags, 0x1B6, {{3, 0}});
    emit_instruction(flags, 0x19A, {{3, 1}});
    emit_instruction(flags, 0x1B7, {{0, 0}});
    emit_instruction(flags, 0x1B6, {{3, 2}});
    emit_instruction(flags, 2, {});
    core::Machine flag_machine(0x12345678u, table);
    flag_machine.load_root(flags);
    for (int i = 0; i < 2; ++i) {
        if (!flag_machine.step()) return false;
    }
    if (flag_machine.flag_y() != 1 || flag_machine.read_int(core::Operand{3, 0}) != 1 ||
        flag_machine.read_int(core::Operand{3, 1}) != flag_machine.flag_x()) {
        std::fprintf(stderr, "0x1b7/0x1b6/0x19a mismatch\n");
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        if (!flag_machine.step()) return false;
    }
    if (flag_machine.flag_y() != 0 || flag_machine.read_int(core::Operand{3, 2}) != 0) {
        std::fprintf(stderr, "0x1b7 normalization mismatch\n");
        return false;
    }

    // 0x1ad：落点格复位值 −1；执行后写当前层号（本版恒 0）。
    if (core::Machine(0x12345678u, table).layer_cell() != -1) {
        std::fprintf(stderr, "0x1ad layer cell must start at -1\n");
        return false;
    }
    auto layer = make_script_with_body();
    emit_instruction(layer, 0x1AD, {});
    emit_instruction(layer, 2, {});
    core::Machine layer_machine(0x12345678u, table);
    layer_machine.load_root(layer);
    if (!layer_machine.step() || layer_machine.layer_cell() != 0) {
        std::fprintf(stderr, "0x1ad layer cell mismatch (%d)\n", layer_machine.layer_cell());
        return false;
    }

    // 0x1b0：源 → 目标，复制的是编码位模式；n=0 一次都不复制。
    auto copy = make_script_with_body();
    emit_instruction(copy, 0x55, {{3, 0}, {0, 42}});
    emit_instruction(copy, 0x1B0, {{3, 0}, {3, 1}, {0, 1}});
    emit_instruction(copy, 2, {});
    core::Machine copy_machine(0x12345678u, table);
    copy_machine.load_root(copy);
    if (!copy_machine.step() || !copy_machine.step() ||
        copy_machine.read_int(core::Operand{3, 1}) != 42) {
        std::fprintf(stderr, "0x1b0 memcpy mismatch (%d)\n",
                     copy_machine.read_int(core::Operand{3, 1}));
        return false;
    }
    auto copy_zero = make_script_with_body();
    emit_instruction(copy_zero, 0x55, {{3, 0}, {0, 42}});
    emit_instruction(copy_zero, 0x1B0, {{3, 0}, {3, 1}, {0, 0}});
    emit_instruction(copy_zero, 2, {});
    core::Machine zero_machine(0x12345678u, table);
    zero_machine.load_root(copy_zero);
    if (!zero_machine.step() || !zero_machine.step() ||
        zero_machine.read_int(core::Operand{3, 1}) != 0) {
        std::fprintf(stderr, "0x1b0 copied with n=0\n");
        return false;
    }

    // 0x1c7 与 0x1cc：位 27 = 0（唯一可达支）；状态格与状态字**不是同一格**。
    auto cells = make_script_with_body();
    emit_instruction(cells, 0x1C7, {{3, 0}});
    emit_instruction(cells, 0x1CF, {{0, 7}});
    emit_instruction(cells, 0x1CC, {{3, 1}});
    emit_instruction(cells, 2, {});
    core::Machine cell_machine(0x12345678u, table);
    cell_machine.load_root(cells);
    for (int i = 0; i < 3; ++i) {
        if (!cell_machine.step()) return false;
    }
    if (cell_machine.read_int(core::Operand{3, 0}) != 0 ||
        cell_machine.read_int(core::Operand{3, 1}) != 0) {
        std::fprintf(stderr, "0x1c7/0x1cc mismatch bit=%d cell=%d\n",
                     cell_machine.read_int(core::Operand{3, 0}),
                     cell_machine.read_int(core::Operand{3, 1}));
        return false;
    }

    // 0x63 lea：把取址结果写进引用槽（写指向的值可读回）；非引用族目标 ⇒ 抛。
    auto lea = make_script_with_body();
    emit_instruction(lea, 0x55, {{3, 5}, {0, 42}});
    emit_instruction(lea, 0x63, {{6, 1}, {3, 5}});
    emit_instruction(lea, 0x55, {{3, 6}, {6, 1}});
    emit_instruction(lea, 2, {});
    core::Machine lea_machine(0x12345678u, table);
    lea_machine.load_root(lea);
    for (int i = 0; i < 3; ++i) {
        if (!lea_machine.step()) {
            std::fprintf(stderr, "0x63 lea failed: %s\n", lea_machine.last_stop().message.c_str());
            return false;
        }
    }
    if (lea_machine.read_int(core::Operand{3, 6}) != 42) {
        std::fprintf(stderr, "0x63 lea wrote the wrong address (%d)\n",
                     lea_machine.read_int(core::Operand{3, 6}));
        return false;
    }
    auto lea_bad = make_script_with_body();
    emit_instruction(lea_bad, 0x63, {{9, 0}, {3, 5}});
    emit_instruction(lea_bad, 2, {});
    core::Machine lea_bad_machine(0x12345678u, table);
    lea_bad_machine.load_root(lea_bad);
    if (lea_bad_machine.step() ||
        lea_bad_machine.last_stop().message.rfind("lea-destination-not-reference-tag", 0) != 0) {
        std::fprintf(stderr, "0x63 accepted a non-reference destination\n");
        return false;
    }

    // 0x60 随机：注入源；范围 = 0 ⇒ 先写 0 再抛。
    auto random_script = make_script_with_body();
    emit_instruction(random_script, 0x60, {{3, 1}, {0, 10}});
    emit_instruction(random_script, 2, {});
    core::Machine random_machine(0x12345678u, table);
    random_machine.set_random_source([] { return 0x40000000u; });
    random_machine.load_root(random_script);
    if (!random_machine.step() || random_machine.read_int(core::Operand{3, 1}) != 4 ||
        random_machine.random_counter() != 1 || random_machine.random_range() != 10) {
        std::fprintf(stderr, "0x60 random mismatch value=%d counter=%d\n",
                     random_machine.read_int(core::Operand{3, 1}),
                     random_machine.random_counter());
        return false;
    }
    auto random_zero = make_script_with_body();
    emit_instruction(random_zero, 0x60, {{3, 1}, {0, 0}});
    emit_instruction(random_zero, 2, {});
    core::Machine random_zero_machine(0x12345678u, table);
    random_zero_machine.load_root(random_zero);
    if (random_zero_machine.step() ||
        random_zero_machine.read_int(core::Operand{3, 1}) != 0) {
        std::fprintf(stderr, "0x60 range=0 must write 0 then throw\n");
        return false;
    }

    // 0x74 / 0x7f：同一格（`Engine+0x15290`）。
    // ★ 初值不是"分配残留"：启动期有确定写点 —— `0x417E98` 取配置键 `message:MessageSpeed`、
    //   `0x417EAB` 写格（取证见 docs/02-engine/ops-forensics-2026-10-title-option.md §6）。
    //   ⇒ "没写过就读"不再响亮失败，而是读出配置值；这里用**不给配置**的 Machine 断言它是 0。
    auto param = make_script_with_body();
    emit_instruction(param, 0x74, {{0, 0xFFFFFFF9u}});  // −7
    emit_instruction(param, 0x7F, {{3, 0}});
    emit_instruction(param, 2, {});
    core::Machine param_machine(0x12345678u, table);
    param_machine.load_root(param);
    if (!param_machine.step() || !param_machine.step() ||
        param_machine.read_int(core::Operand{3, 0}) != -7) {
        std::fprintf(stderr, "0x74/0x7f round trip mismatch (%d)\n",
                     param_machine.read_int(core::Operand{3, 0}));
        return false;
    }
    auto param_uninit = make_script_with_body();
    emit_instruction(param_uninit, 0x7F, {{3, 0}});
    emit_instruction(param_uninit, 2, {});
    core::Machine param_uninit_machine(0x12345678u, table);
    param_uninit_machine.load_root(param_uninit);
    if (!param_uninit_machine.step() || param_uninit_machine.read_int(core::Operand{3, 0}) != 0) {
        std::fprintf(stderr, "0x7f 初值 = %d（没有 message:MessageSpeed 时按缺省 0）\n",
                     param_uninit_machine.read_int(core::Operand{3, 0}));
        return false;
    }

    // 0xd0 时钟：本迭代读数。
    auto clock = make_script_with_body();
    emit_instruction(clock, 0xD0, {{3, 0}});
    emit_instruction(clock, 2, {});
    core::Machine clock_machine2(0x12345678u, table);
    clock_machine2.set_host_clock_ms(12345);
    clock_machine2.load_root(clock);
    if (!clock_machine2.step() || clock_machine2.read_int(core::Operand{3, 0}) != 12345) {
        std::fprintf(stderr, "0xd0 clock mismatch (%d)\n",
                     clock_machine2.read_int(core::Operand{3, 0}));
        return false;
    }

    // 0x191 / 0x2d0 / 0x2d1 / 0x2d3：浮点族（含减法/除法的方向）。
    auto floats = make_script_with_body();
    emit_instruction(floats, 0x191, {{4, 0}, {1, 0xC0600000u}});         // −3.5 → 3.5
    emit_instruction(floats, 0x2D0, {{4, 1}, {1, 0x40200000u}, {1, 0x40800000u}});  // 2.5+4
    emit_instruction(floats, 0x2D1, {{4, 2}, {1, 0x40E00000u}, {1, 0x40400000u}});  // 7−3
    emit_instruction(floats, 0x2D3, {{4, 3}, {1, 0x41000000u}, {1, 0x40000000u}});  // 8÷2
    emit_instruction(floats, 2, {});
    core::Machine float_machine(0x12345678u, table);
    float_machine.load_root(floats);
    for (int i = 0; i < 4; ++i) {
        if (!float_machine.step()) {
            std::fprintf(stderr, "float fixture failed: %s\n",
                         float_machine.last_stop().message.c_str());
            return false;
        }
    }
    if (float_machine.read_float(core::Operand{4, 0}) != 3.5 ||
        float_machine.read_float(core::Operand{4, 1}) != 6.5 ||
        float_machine.read_float(core::Operand{4, 2}) != 4.0 ||
        float_machine.read_float(core::Operand{4, 3}) != 4.0) {
        std::fprintf(stderr, "float ops mismatch %f %f %f %f\n",
                     float_machine.read_float(core::Operand{4, 0}),
                     float_machine.read_float(core::Operand{4, 1}),
                     float_machine.read_float(core::Operand{4, 2}),
                     float_machine.read_float(core::Operand{4, 3}));
        return false;
    }

    // 0x218 / 0x21a：查不到 ⇒ 三个输出写 0（覆盖旧值）。
    auto vec = make_script_with_body();
    emit_instruction(vec, 0x55, {{4, 0}, {0, 5}});
    emit_instruction(vec, 0x218, {{0, 9}, {4, 0}, {4, 1}, {4, 2}});
    emit_instruction(vec, 0x21A, {{0, 9}, {4, 3}, {4, 4}, {4, 5}});
    emit_instruction(vec, 2, {});
    core::Machine vec_machine(0x12345678u, table);
    vec_machine.load_root(vec);
    for (int i = 0; i < 3; ++i) {
        if (!vec_machine.step()) return false;
    }
    for (std::uint32_t slot = 0; slot <= 5; ++slot) {
        if (vec_machine.read_float(core::Operand{4, slot}) != 0.0) {
            std::fprintf(stderr, "0x218/0x21a slot %u not zero\n", slot);
            return false;
        }
    }

    // 0x217：写第一三元向量 —— 记录不存在 ⇒ 先建默认记录再写；只写 vec1，vec2 一个都不动。
    auto vec1 = make_script_with_body();
    emit_instruction(vec1, 0x217, {{0, 9}, {1, 0x3F800000u}, {1, 0x40000000u}, {1, 0x40400000u}});  // 1,2,3
    emit_instruction(vec1, 0x218, {{0, 9}, {4, 0}, {4, 1}, {4, 2}});
    emit_instruction(vec1, 0x21A, {{0, 9}, {4, 3}, {4, 4}, {4, 5}});
    emit_instruction(vec1, 2, {});
    core::Machine vec1_machine(0x12345678u, table);
    vec1_machine.load_root(vec1);
    for (int i = 0; i < 3; ++i) {
        if (!vec1_machine.step()) return false;
    }
    for (std::uint32_t slot = 0; slot < 3; ++slot) {
        const double value = vec1_machine.read_float(core::Operand{4, slot});
        if (value != static_cast<double>(slot + 1)) {
            std::fprintf(stderr, "0x217 vec1 slot %u = %f\n", slot, value);
            return false;
        }
    }
    for (std::uint32_t slot = 3; slot < 6; ++slot) {
        if (vec1_machine.read_float(core::Operand{4, slot}) != 0.0) {
            std::fprintf(stderr, "0x217 写到了第二组三元向量（slot %u）\n", slot);
            return false;
        }
    }
    const core::SceneRecord* vec1_record = vec1_machine.scene_record(9);
    if (vec1_record == nullptr || vec1_record->vec2[0] != 0.0f || vec1_record->vec2[1] != 0.0f ||
        vec1_record->vec2[2] != 0.0f) {
        std::fprintf(stderr, "0x217 的记录：vec2 必须保持 0\n");
        return false;
    }

    // 0xC0：把引擎标量写进第 1 个操作数（**不读操作数的值**）；推进格数 3。
    auto c0 = make_script_with_body();
    emit_instruction(c0, 0xC0, {{9, 7}});
    emit_instruction(c0, 2, {});
    core::Machine c0_machine(0x12345678u, table);
    c0_machine.load_root(c0);
    c0_machine.seed_engine_aa9e4(0x2A);
    if (!c0_machine.step()) return false;
    if (c0_machine.frame().length_slot != 3) {
        std::fprintf(stderr, "0xC0 推进格数 = %u（期望 3）\n", c0_machine.frame().length_slot);
        return false;
    }
    if (c0_machine.local_int(7) != 0x2A) {
        std::fprintf(stderr, "0xC0 写进局部格的值 = %d（期望 42）\n", c0_machine.local_int(7));
        return false;
    }

    // ── CONFIG.BIN 路径上的一批（0x142 / 0x1BB / 0x1A6 / 0x300 / 0x301 / 0x1D2 / 0x1D3 / 0x6E / 0x6F / 0x196）──
    // ★ 每条都断言"引擎可见的那一格"，⛔ 不断言实现细节；取证锚见 docs/02-engine/ops-forensics-2026-10-title-option.md。
    {
        // 0x142：写引擎标量 —— 值原样（⛔ 不许夹成 0/1）。
        auto script = make_script_with_body();
        emit_instruction(script, 0x142, {{0, 5}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        if (!machine.step() || machine.engine_aab70() != 5) {
            std::fprintf(stderr, "0x142 engine_aab70 = %d（期望 5）\n", machine.engine_aab70());
            return false;
        }
    }
    {
        // 0x1BB：0 ⇒ bit31（抑制）；1 ⇒ 0；其它 ⇒ 不写 + 一条诊断（不抛）。
        auto make = [&](std::uint32_t arg) {
            auto script = make_script_with_body();
            emit_instruction(script, 0x1BB, {{0, arg}});
            emit_instruction(script, 2, {});
            return script;
        };
        core::Machine zero(0x12345678u, table);
        zero.load_root(make(0));
        zero.step();
        if (zero.font_text_gate() != static_cast<std::int32_t>(0x80000000u)) {
            std::fprintf(stderr, "0x1BB(0) gate = 0x%X（期望 0x80000000）\n",
                         static_cast<unsigned>(zero.font_text_gate()));
            return false;
        }
        core::Machine one(0x12345678u, table);
        one.load_root(make(1));
        one.step();
        if (one.font_text_gate() != 0) {
            std::fprintf(stderr, "0x1BB(1) gate = %d（期望 0）\n", one.font_text_gate());
            return false;
        }
        // 其它值：先置 bit31，再给 2 ⇒ 保持 bit31、且多一条诊断。
        auto strange = make_script_with_body();
        emit_instruction(strange, 0x1BB, {{0, 0}});
        emit_instruction(strange, 0x1BB, {{0, 2}});
        emit_instruction(strange, 2, {});
        core::Machine other(0x12345678u, table);
        other.load_root(strange);
        other.step();
        other.step();
        if (other.font_text_gate() != static_cast<std::int32_t>(0x80000000u) || other.errors().size() != 1) {
            std::fprintf(stderr, "0x1BB(2) gate = 0x%X errors=%zu（期望保持 0x80000000 且 1 条）\n",
                         static_cast<unsigned>(other.font_text_gate()), other.errors().size());
            return false;
        }
    }
    {
        // 0x1A6：`strlen >> 1`（**按字节**，不是按字）。
        auto run = [&](const std::string& text) -> std::int32_t {
            auto script = make_script_with_body();
            emit_instruction(script, 0x1A6, {{9, 0}, {5, 0}});
            emit_instruction(script, 2, {});
            core::Machine machine(0x12345678u, table);
            machine.load_root(script);
            machine.write_text(core::Operand{5, 0}, text);
            machine.step();
            return machine.local_int(0);
        };
        if (run("ABCDE") != 2 || run("AB") != 1 || run("") != 0) {
            std::fprintf(stderr, "0x1A6 减半 = %d/%d/%d（期望 2/1/0）\n", run("ABCDE"), run("AB"), run(""));
            return false;
        }
    }
    {
        // 0x194：把"op2 与 op3 的字符串形式**逐字节相等**"归一成 1 / 0（读序 3 → 2）。
        auto run = [&](const std::string& lhs, const std::string& rhs) -> std::int32_t {
            auto script = make_script_with_body();
            emit_instruction(script, 0x194, {{9, 0}, {5, 0}, {5, 1}});
            emit_instruction(script, 2, {});
            core::Machine machine(0x12345678u, table);
            machine.load_root(script);
            machine.write_text(core::Operand{5, 0}, lhs);
            machine.write_text(core::Operand{5, 1}, rhs);
            machine.step();
            return machine.local_int(0);
        };
        if (run("abc", "abc") != 1 || run("abc", "abd") != 0 || run("", "") != 1) {
            std::fprintf(stderr, "0x194 字符串相等 = %d/%d/%d（期望 1/0/1）\n", run("abc", "abc"),
                         run("abc", "abd"), run("", ""));
            return false;
        }
        // 整数标签走同一个"格式化成字符串"的读法：type 0 的 5 与文本 "5" 相等。
        {
            auto script = make_script_with_body();
            emit_instruction(script, 0x194, {{9, 0}, {0, 5}, {5, 1}});
            emit_instruction(script, 2, {});
            core::Machine machine(0x12345678u, table);
            machine.load_root(script);
            machine.write_text(core::Operand{5, 1}, "5");
            machine.step();
            if (machine.local_int(0) != 1) {
                std::fprintf(stderr, "0x194 整数 5 与文本 \"5\" 应相等（实际 %d）\n", machine.local_int(0));
                return false;
            }
        }
    }
    {
        // 0x300：`A[idx] = op2 | (A[idx] & 0x10000)`（bit16 粘住）+ `B[idx] = op3`；下标不检查范围。
        auto script = make_script_with_body();
        emit_instruction(script, 0x300, {{0, 2}, {0, 0x10001}, {0, 1000}});
        emit_instruction(script, 0x300, {{0, 2}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x300, {{0, 10}, {0, 1}, {0, 7}});  // 越界下标：引擎不拦
        emit_instruction(script, 0x301, {{0, 2}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        for (int i = 0; i < 4; ++i) {
            if (!machine.step()) return false;
        }
        if (machine.font_slot_state(2) != 0x10000 || machine.font_slot_deadline(2) != 0) {
            std::fprintf(stderr, "0x300 A/B = 0x%X/%d（期望 0x10000/0）\n",
                         static_cast<unsigned>(machine.font_slot_state(2)), machine.font_slot_deadline(2));
            return false;
        }
        if (machine.font_slot_state(10) != 1 || machine.font_slot_deadline(10) != 7) {
            std::fprintf(stderr, "0x300 越界下标没有落格（state=%d deadline=%d）\n",
                         machine.font_slot_state(10), machine.font_slot_deadline(10));
            return false;
        }
        if (machine.font_slot_start(2) != 0) {
            std::fprintf(stderr, "0x301 没清 C[2]\n");
            return false;
        }
    }
    {
        // 0x1D2 / 0x1D3：入队 → 按 key 查回；未命中写 0/0（不是 −1）；门关时整条跳过。
        auto script = make_script_with_body();
        emit_instruction(script, 0x1D2, {{0, 3}, {0, 7}});      // key=3, value=7（同组第一条）
        emit_instruction(script, 0x1D2, {{0, 3}, {0, 9}});      // key=3, value=9（同组第二条，位 0 = 0）
        emit_instruction(script, 0x1D3, {{9, 1}, {9, 2}, {0, 0}, {9, 0}, {0, 3}});  // 命中：取**最后一条** ⇒ 9
        emit_instruction(script, 0x1D3, {{9, 3}, {9, 4}, {0, 0}, {9, 0}, {0, 9}});  // 未命中
        emit_instruction(script, 0x1BB, {{0, 0}});              // 关门
        emit_instruction(script, 0x1D2, {{0, 4}, {0, 8}});      // 被守卫跳过
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        for (int i = 0; i < 6; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "0x1D2/0x1D3 夹具提前停机（第 %d 步）\n", i + 1);
                return false;
            }
        }
        if (machine.local_int(1) != 1 || machine.local_int(2) != 9) {
            std::fprintf(stderr, "0x1D3 命中 = %d/%d（期望 1/9 ⇒ 同组取最后一条）\n",
                         machine.local_int(1), machine.local_int(2));
            return false;
        }
        if (machine.local_int(3) != 0 || machine.local_int(4) != 0) {
            std::fprintf(stderr, "0x1D3 未命中 = %d/%d（期望 0/0）\n", machine.local_int(3), machine.local_int(4));
            return false;
        }
        if (machine.font_entries().size() != 2) {
            std::fprintf(stderr, "0x1D2 门关时仍入队（%zu 条）\n", machine.font_entries().size());
            return false;
        }
    }
    {
        // 0x6E / 0x6F / 0x196：正文追加 / 行计数 / 振り仮名截断；空正文整段跳过。
        auto script = make_script_with_body();
        emit_instruction(script, 0x6E, {{0, 0}, {5, 0}});       // 正文 = 串 0
        emit_instruction(script, 0x6E, {{0, 0}, {5, 1}});       // 空串 ⇒ 不写、不追加
        emit_instruction(script, 0x6F, {{0, 0}});               // 行结束
        emit_instruction(script, 0x196, {{0, 0}, {5, 0}, {5, 2}});  // 正文 + 振り仮名
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.write_text(core::Operand{5, 0}, "AB");
        machine.write_text(core::Operand{5, 1}, "");
        std::string long_rubi(2000, 'x');
        machine.write_text(core::Operand{5, 2}, long_rubi);
        for (int i = 0; i < 4; ++i) {
            if (!machine.step()) return false;
        }
        const core::TextWindowRecord* window = machine.text_window(machine.default_group());
        if (window == nullptr || window->text != "ABAB" || window->lines != 1) {
            std::fprintf(stderr, "0x6E/0x6F 窗口 = %s / %d 行\n",
                         window == nullptr ? "(null)" : window->text.c_str(),
                         window == nullptr ? -1 : window->lines);
            return false;
        }
        if (window->rubi.size() != 1023) {
            std::fprintf(stderr, "0x196 振り仮名截断 = %zu 字节（期望 1023）\n", window->rubi.size());
            return false;
        }
        if (machine.current_text_slot() != 0) {
            std::fprintf(stderr, "0x6E 文本槽 = %d\n", machine.current_text_slot());
            return false;
        }
    }

    // ── 0x12F：排序置换（A 里写下标的升序排列；键 = dec(B[i]) + dec(C[i])）──
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x55, {{9, 10}, {0, 3}});  // B = [3,1,2]
        emit_instruction(script, 0x55, {{9, 11}, {0, 1}});
        emit_instruction(script, 0x55, {{9, 12}, {0, 2}});
        emit_instruction(script, 0x55, {{9, 20}, {0, 0}});  // C = [0,0,0]（经编码写）
        emit_instruction(script, 0x55, {{9, 21}, {0, 0}});
        emit_instruction(script, 0x55, {{9, 22}, {0, 0}});
        emit_instruction(script, 0x12F, {{9, 0}, {9, 10}, {9, 20}, {0, 3}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        for (int i = 0; i < 7; ++i) {
            if (!machine.step()) return false;
        }
        const std::int32_t expected[3] = {1, 2, 0};  // 键 1/2/3 对应的下标
        for (std::uint32_t i = 0; i < 3; ++i) {
            if (machine.local_int(i) != expected[i]) {
                std::fprintf(stderr, "0x12F A[%u] = %d（期望 %d）\n", i, machine.local_int(i),
                             expected[i]);
                return false;
            }
        }
        // N = 0：`A[0] := 0` 是**无条件**的原始写（不经过编码）。
        auto empty = make_script_with_body();
        emit_instruction(empty, 0x12F, {{9, 0}, {9, 10}, {9, 20}, {0, 0}});
        emit_instruction(empty, 2, {});
        core::Machine empty_machine(0x12345678u, table);
        empty_machine.load_root(empty);
        if (!empty_machine.step()) return false;
        if (empty_machine.local_int(0) != empty_machine.decode_int(0u)) {
            std::fprintf(stderr, "0x12F(N=0) 没有写原始 0\n");
            return false;
        }
    }

    // ── 配置族 + 滚轮（0x2CC / 0x2CD / 0x2CE / 0x2E6 / 0x2E7 / 0x2E8 / 0x2EA / 0x10D）──
    {
        core::ConfigStore config;
        config.set_int("message:AdvanceMesOnWheel", 3);
        config.set_int("message:AutoMessagePitch0", 11);
        config.set_int("message:AutoMessagePitch1", 13);
        config.set_int("message:AutoMessageOption", 5);
        config.set_int("display:ScreenMode", 1);
        auto script = make_script_with_body();
        emit_instruction(script, 0x2E6, {{0, 0}, {9, 0}});   // Pitch0 → loc0
        emit_instruction(script, 0x2E6, {{0, 1}, {9, 1}});   // Pitch1 → loc1
        emit_instruction(script, 0x2EA, {{9, 2}});           // Option → loc2
        emit_instruction(script, 0x2E8, {{0, 7}});           // Option = 7
        emit_instruction(script, 0x2EA, {{9, 3}});           // Option → loc3
        emit_instruction(script, 0x2E7, {{0, 1}, {0, 21}});  // Pitch1 = 21
        emit_instruction(script, 0x2E6, {{0, 1}, {9, 4}});   // Pitch1 → loc4
        emit_instruction(script, 0x2CC, {{9, 5}});           // AdvanceMesOnWheel → loc5
        emit_instruction(script, 0x2CD, {{0, 4}});           // AdvanceMesOnWheel = 4
        emit_instruction(script, 0x2CC, {{9, 6}});           // → loc6
        emit_instruction(script, 0x2CE, {{9, 7}});           // ScreenMode 缓存 → loc7
        emit_instruction(script, 0x10D, {{9, 8}});           // 滚轮 → loc8（并清）
        emit_instruction(script, 0x10D, {{9, 9}});           // 再读 ⇒ 0
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table, nullptr, std::move(config));
        machine.load_root(script);
        machine.add_wheel_delta(240);
        for (int i = 0; i < 13; ++i) {
            if (!machine.step()) return false;
        }
        const std::int32_t expected[10] = {11, 13, 5, 7, 21, 3, 4, 1, 240, 0};
        for (std::uint32_t i = 0; i < 10; ++i) {
            if (machine.local_int(i) != expected[i]) {
                std::fprintf(stderr, "配置族 loc[%u] = %d（期望 %d）\n", i, machine.local_int(i),
                             expected[i]);
                return false;
            }
        }
    }

    // ── 设置表 getter：0xC5（声道音量）/ 0x306（点击跳过效果）──
    {
        core::ConfigStore config;
        config.set_int("sound:Volume2", 37);
        config.set_int("system:EffectSkipOnClick", 3);
        auto script = make_script_with_body();
        emit_instruction(script, 0xC5, {{0, 2}, {9, 0}});   // 声道 2 → loc0 = 37
        emit_instruction(script, 0xC5, {{0, 5}, {9, 1}});   // 越界 ⇒ 不写、只记日志
        emit_instruction(script, 0x306, {{9, 2}});          // → loc2 = 3
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table, nullptr, std::move(config));
        machine.load_root(script);
        for (int i = 0; i < 3; ++i) {
            if (!machine.step()) return false;
        }
        if (machine.local_int(0) != 37 || machine.local_int(1) != 0 || machine.local_int(2) != 3) {
            std::fprintf(stderr, "0xC5/0x306 = %d/%d/%d（期望 37/0/3）\n", machine.local_int(0),
                         machine.local_int(1), machine.local_int(2));
            return false;
        }
        if (machine.errors().size() != 1) {
            std::fprintf(stderr, "0xC5 越界声道应记 1 条日志（实际 %zu）\n", machine.errors().size());
            return false;
        }
    }

    // ── 0xC6（音量 setter）/ 0xC7（声道开关 getter，Music 的负值分流与别的声道不同）──
    {
        core::ConfigStore config;
        config.set_int("sound:Music", -1);
        config.set_int("sound:SE", -1);
        config.set_int("sound:Voice", 0);
        config.set_int("sound:Movie", 5);
        auto script = make_script_with_body();
        emit_instruction(script, 0xC7, {{0, 1}, {9, 0}});
        emit_instruction(script, 0xC7, {{0, 2}, {9, 1}});
        emit_instruction(script, 0xC7, {{0, 3}, {9, 2}});
        emit_instruction(script, 0xC7, {{0, 4}, {9, 3}});
        emit_instruction(script, 0xC7, {{0, 0}, {9, 4}});   // 非法声道 ⇒ 不写 + 1 条日志
        emit_instruction(script, 0xC6, {{0, 2}, {0, 42}});  // Volume2 = 42
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table, nullptr, std::move(config));
        machine.load_root(script);
        for (int i = 0; i < 6; ++i) {
            if (!machine.step()) return false;
        }
        const std::int32_t expected[4] = {0, 1, 0, 1};
        for (std::uint32_t i = 0; i < 4; ++i) {
            if (machine.local_int(i) != expected[i]) {
                std::fprintf(stderr, "0xC7 loc[%u] = %d（期望 %d）\n", i, machine.local_int(i), expected[i]);
                return false;
            }
        }
        if (machine.local_int(4) != 0 || machine.config().get_int("sound:Volume2") != 42 ||
            machine.errors().size() != 1) {
            std::fprintf(stderr, "0xC6/0xC7 收尾不符（loc4=%d Volume2=%d errors=%zu）\n", machine.local_int(4),
                         machine.config().get_int("sound:Volume2"), machine.errors().size());
            return false;
        }
    }

    // 0x228：0x1ff 写平移 → 读回；记录不存在 ⇒ 标志 1 且三个浮点不写。
    auto trans = make_script_with_body();
    emit_instruction(trans, 0x1FF, {{0, 7}, {1, 0x40400000u}, {1, 0}, {1, 0}});  // 3,0,0
    emit_instruction(trans, 0x228, {{3, 0}, {0, 7}, {4, 0}, {4, 1}, {4, 2}});
    emit_instruction(trans, 2, {});
    core::Machine trans_machine(0x12345678u, table);
    trans_machine.load_root(trans);
    if (!trans_machine.step() || !trans_machine.step() ||
        trans_machine.read_int(core::Operand{3, 0}) != 0 ||
        trans_machine.read_float(core::Operand{4, 0}) != 3.0) {
        std::fprintf(stderr, "0x228 success path mismatch flag=%d x=%f\n",
                     trans_machine.read_int(core::Operand{3, 0}),
                     trans_machine.read_float(core::Operand{4, 0}));
        return false;
    }
    auto trans_missing = make_script_with_body();
    emit_instruction(trans_missing, 0x55, {{4, 0}, {0, 5}});
    emit_instruction(trans_missing, 0x228, {{3, 0}, {0, 8}, {4, 0}, {4, 1}, {4, 2}});
    emit_instruction(trans_missing, 2, {});
    core::Machine missing_machine(0x12345678u, table);
    missing_machine.load_root(trans_missing);
    for (int i = 0; i < 2; ++i) {
        if (!missing_machine.step()) return false;
    }
    if (missing_machine.read_int(core::Operand{3, 0}) != 1 ||
        missing_machine.read_float(core::Operand{4, 0}) != 5.0 || !missing_machine.errors().empty()) {
        std::fprintf(stderr, "0x228 missing-record path mismatch\n");
        return false;
    }

    // 0x21d：三张表各拷各的；没有源 ⇒ 上报且不标脏。
    auto scene_copy = make_script_with_body();
    emit_instruction(scene_copy, 0x1FB, {{0, 1}, {0, 1}, {0, 0}, {0, 0},
                                         {0, 4}, {0, 4}, {0, 0}, {0, 0}});
    emit_instruction(scene_copy, 0x21D, {{0, 1}, {0, 2}});
    emit_instruction(scene_copy, 2, {});
    core::Machine scene_copy_machine(0x12345678u, table);
    scene_copy_machine.load_root(scene_copy);
    scene_copy_machine.end_render();  // 清脏
    if (!scene_copy_machine.step()) return false;
    scene_copy_machine.end_render();
    if (!scene_copy_machine.step() || scene_copy_machine.draw_item(2) == nullptr ||
        !scene_copy_machine.scene_dirty()) {
        std::fprintf(stderr, "0x21d copy mismatch\n");
        return false;
    }
    auto scene_copy_missing = make_script_with_body();
    emit_instruction(scene_copy_missing, 0x21D, {{0, 9}, {0, 10}});
    emit_instruction(scene_copy_missing, 2, {});
    core::Machine missing_copy_machine(0x12345678u, table);
    missing_copy_machine.load_root(scene_copy_missing);
    missing_copy_machine.end_render();
    if (!missing_copy_machine.step() || missing_copy_machine.scene_dirty() ||
        missing_copy_machine.errors().size() != 1) {
        std::fprintf(stderr, "0x21d missing-source path mismatch\n");
        return false;
    }
    return true;
}

// ── 池翻页（0x6D；SPC-POOL-MEMFLIP） ──
bool check_memflip(const core::OpcodeTable& table) {
    // 发行配置（键缺失 = 0）⇒ 恒走异常支，选择不变。
    auto disabled = make_script_with_body();
    emit_instruction(disabled, 0x6D, {});
    emit_instruction(disabled, 2, {});
    core::Machine off_machine(0x12345678u, table);
    off_machine.load_root(disabled);
    if (off_machine.step() ||
        off_machine.last_stop().message.rfind("display-message:memflip", 0) != 0) {
        std::fprintf(stderr, "0x6d accepted a disabled memflip\n");
        return false;
    }

    // 启动期键 = 1：翻页只换选择（不搬数据），再翻一次回原组、原值仍在。
    core::ConfigStore config;
    config.set_int("set:EnableMemFlip", 1);
    auto flip = make_script_with_body();
    emit_instruction(flip, 0x55, {{3, 0}, {0, 42}});
    emit_instruction(flip, 0x6D, {});
    emit_instruction(flip, 0x55, {{3, 1}, {3, 0}});  // 另一组：未写过 ⇒ 0
    emit_instruction(flip, 0x6D, {});
    emit_instruction(flip, 0x55, {{3, 2}, {3, 0}});  // 翻回主组：42
    emit_instruction(flip, 2, {});
    core::Machine flip_machine(0x12345678u, table, nullptr, config);
    flip_machine.load_root(flip);
    for (int i = 0; i < 5; ++i) {
        if (!flip_machine.step()) {
            std::fprintf(stderr, "0x6d fixture failed: %s\n",
                         flip_machine.last_stop().message.c_str());
            return false;
        }
    }
    if (flip_machine.read_int(core::Operand{3, 1}) != 0 ||
        flip_machine.read_int(core::Operand{3, 2}) != 42) {
        std::fprintf(stderr, "0x6d flip mismatch (other=%d main=%d)\n",
                     flip_machine.read_int(core::Operand{3, 1}),
                     flip_machine.read_int(core::Operand{3, 2}));
        return false;
    }
    return true;
}

// ── 3D setter 族（0x21E / 0x21F；CORE-OPS-SCENE3D-21E / -21F） ──
bool check_scene3d_setters(const core::OpcodeTable& table) {
    // 「已存在」位为 0 ⇒ 记录**已建**但一个字段都不写（与"没调用"在表内容上不同）。
    auto gate_off = make_script_with_body();
    emit_instruction(gate_off, 0x21E, {{0, 3}, {0, 11}, {0, 22},
                                       {1, 0x42C80000u}, {1, 0x43480000u}, {1, 0x43960000u}});
    emit_instruction(gate_off, 2, {});
    core::Machine off_machine(0x12345678u, table);
    off_machine.load_root(gate_off);
    if (!off_machine.step()) return false;
    const core::DrawItem* off_item = off_machine.draw_item(3);
    if (off_item == nullptr || (off_item->flags & 1u) != 0 || off_item->config_g3 != 0 ||
        off_item->set21e_scale[0] != 1.0f || off_item->set21e_scale[5] != 1.0f) {
        std::fprintf(stderr, "0x21e wrote fields with the record-exists bit clear\n");
        return false;
    }

    // 位 0 = 1（由 0x1fb 置）⇒ 置「有窗」位、G3 ← 1、清窗起点、两个整数、三个浮点 ÷100。
    auto gate_on = make_script_with_body();
    emit_instruction(gate_on, 0x1FB, {{0, 3}, {0, 1}, {0, 0}, {0, 0},
                                      {0, 4}, {0, 4}, {0, 0}, {0, 0}});
    emit_instruction(gate_on, 0x21E, {{0, 3}, {0, 11}, {0, 22},
                                      {1, 0x42C80000u}, {1, 0x43480000u}, {1, 0x43960000u}});
    emit_instruction(gate_on, 2, {});
    core::Machine on_machine(0x12345678u, table);
    on_machine.load_root(gate_on);
    if (!on_machine.step() || !on_machine.step()) return false;
    const core::DrawItem* on_item = on_machine.draw_item(3);
    if (on_item == nullptr || (on_item->flags & 2u) == 0 || on_item->config_g3 != 1 ||
        on_item->window_start != 0 || on_item->set21e_int0 != 11 || on_item->set21e_int1 != 22 ||
        on_item->set21e_scale[0] != 1.0f || on_item->set21e_scale[5] != 2.0f ||
        on_item->set21e_scale[10] != 3.0f || !on_machine.scene_dirty()) {
        std::fprintf(stderr, "0x21e scale mismatch (s0=%f s5=%f s10=%f g3=%d)\n",
                     on_item == nullptr ? 0.0f : on_item->set21e_scale[0],
                     on_item == nullptr ? 0.0f : on_item->set21e_scale[5],
                     on_item == nullptr ? 0.0f : on_item->set21e_scale[10],
                     on_item == nullptr ? -1 : on_item->config_g3);
        return false;
    }

    // 0x21F：轴与角另存一份；矩阵按**度**换算（90° 绕 z ⇒ cos = 0）。
    auto rotate = make_script_with_body();
    emit_instruction(rotate, 0x1FB, {{0, 4}, {0, 1}, {0, 0}, {0, 0},
                                     {0, 4}, {0, 4}, {0, 0}, {0, 0}});
    emit_instruction(rotate, 0x21F, {{0, 4}, {0, 1}, {0, 2}, {1, 0}, {1, 0}, {1, 0x3F800000u},
                                     {1, 0x42B40000u}});
    emit_instruction(rotate, 2, {});
    core::Machine rotate_machine(0x12345678u, table);
    rotate_machine.load_root(rotate);
    if (!rotate_machine.step() || !rotate_machine.step()) return false;
    const core::DrawItem* rot_item = rotate_machine.draw_item(4);
    if (rot_item == nullptr || rot_item->config_g3 != 1 || rot_item->set21f_axis[2] != 1.0f ||
        rot_item->set21f_degrees != 90.0f || rot_item->set21f_rot[10] < 0.999f ||
        std::abs(rot_item->set21f_rot[5]) > 0.001f || std::abs(rot_item->set21f_rot[1] + 1.0f) > 0.001f) {
        std::fprintf(stderr, "0x21f rotation mismatch deg=%f r5=%f r10=%f\n",
                     rot_item == nullptr ? 0.0f : rot_item->set21f_degrees,
                     rot_item == nullptr ? 0.0f : rot_item->set21f_rot[5],
                     rot_item == nullptr ? 0.0f : rot_item->set21f_rot[10]);
        return false;
    }
    return true;
}

// ── 按住派发族（0xFC / 0x102 / 0x103 / 0x104 / 0x105） ──
bool check_input_hold(const core::OpcodeTable& table) {
    // 0xfc：两张回调表**所有帧**清空 + 模数写 0；别的（掩码 / 键总数 / 扫描上限 / 游标 / ip）都不动。
    auto reset = make_script_with_body();
    emit_instruction(reset, 0xFE, {{0, 5}});
    emit_instruction(reset, 0xFF, {});
    emit_instruction(reset, 0x103, {{0, 4}});
    emit_instruction(reset, 0x102, {{0, 7}, {0, 3}, {0, 20}});
    emit_instruction(reset, 0xFB, {{0, 4}, {0, 21}});
    emit_instruction(reset, 0x105, {{0, 9}});
    emit_instruction(reset, 0xFC, {});
    emit_instruction(reset, 2, {});
    core::Machine reset_machine(0x12345678u, table);
    reset_machine.load_root(reset);
    for (int i = 0; i < 5; ++i) {
        if (!reset_machine.step()) return false;
    }
    if (reset_machine.frame().hold_slots.size() != 1 || reset_machine.frame().key_jumps.size() != 1) {
        std::fprintf(stderr, "0x102/0xfb tables not registered before reset\n");
        return false;
    }
    reset_machine.seed_hold_cursor(0, 3);
    if (!reset_machine.step() || !reset_machine.step()) return false;  // 0x105 / 0xfc
    if (!reset_machine.frame().hold_slots.empty() || !reset_machine.frame().key_jumps.empty() ||
        reset_machine.hold_modulus() != 0 || reset_machine.hold_mask() != 9u ||
        reset_machine.key_total() != 5 || reset_machine.frame().key_limit != 5 ||
        reset_machine.frame().key_cursor != 0 || !reset_machine.hold_cursor() ||
        *reset_machine.hold_cursor() != 3 || reset_machine.frame().ip != 4u * 23u) {
        std::fprintf(stderr, "0xfc reset mismatch (mask=%u modulus=%d ip=%zu)\n",
                     reset_machine.hold_mask(), reset_machine.hold_modulus(),
                     reset_machine.frame().ip);
        return false;
    }

    // 0xfc 之后模数 0 ⇒ 下一次 0x104 在出口处响亮失败。
    auto reset_then_dispatch = make_script_with_body();
    emit_instruction(reset_then_dispatch, 0xFE, {{0, 2}});
    emit_instruction(reset_then_dispatch, 0x103, {{0, 4}});
    emit_instruction(reset_then_dispatch, 0xFC, {});
    emit_instruction(reset_then_dispatch, 0x104, {});
    emit_instruction(reset_then_dispatch, 2, {});
    core::Machine dz_machine(0x12345678u, table);
    dz_machine.load_root(reset_then_dispatch);
    for (int i = 0; i < 3; ++i) {
        if (!dz_machine.step()) return false;
    }
    if (dz_machine.step() ||
        dz_machine.last_stop().message.rfind("hold-cursor-divide-by-zero", 0) != 0) {
        std::fprintf(stderr, "0x104 with modulus 0 did not fail loudly\n");
        return false;
    }

    // 0x102：上界 31/9、后写覆盖、−1 哨兵、推进 7；槽号越界 ⇒ 抛且一槽不写。
    auto slots = make_script_with_body();
    emit_instruction(slots, 0x102, {{0, 31}, {0, 9}, {0, 1234}});
    emit_instruction(slots, 0x102, {{0, 31}, {0, 9}, {0, 0xFFFFFFFFu}});
    emit_instruction(slots, 2, {});
    core::Machine slot_machine(0x12345678u, table);
    slot_machine.load_root(slots);
    if (!slot_machine.step() || !slot_machine.step()) return false;
    const auto terminal = slot_machine.frame().hold_slots.find(319);
    if (terminal == slot_machine.frame().hold_slots.end() || terminal->second != -1 ||
        slot_machine.frame().ip != 56) {
        std::fprintf(stderr, "0x102 upper bound / overwrite mismatch (ip=%zu)\n",
                     slot_machine.frame().ip);
        return false;
    }
    auto slot_bad = make_script_with_body();
    emit_instruction(slot_bad, 0x102, {{0, 7}, {0, 9}, {0, 30}});
    emit_instruction(slot_bad, 0x102, {{0, 7}, {0, 10}, {0, 99}});
    emit_instruction(slot_bad, 2, {});
    core::Machine slot_bad_machine(0x12345678u, table);
    slot_bad_machine.load_root(slot_bad);
    if (!slot_bad_machine.step() || slot_bad_machine.step() ||
        slot_bad_machine.frame().hold_slots.size() != 1 ||
        slot_bad_machine.frame().hold_slots.at(79) != 30) {
        std::fprintf(stderr, "0x102 slot 10 was clamped or written\n");
        return false;
    }
    for (const auto key : {32, -1}) {
        auto key_bad = make_script_with_body();
        emit_instruction(key_bad, 0x102, {{0, static_cast<std::uint32_t>(key)}, {0, 0}, {0, 5}});
        emit_instruction(key_bad, 2, {});
        core::Machine key_bad_machine(0x12345678u, table);
        key_bad_machine.load_root(key_bad);
        if (key_bad_machine.step() || !key_bad_machine.frame().hold_slots.empty()) {
            std::fprintf(stderr, "0x102 accepted key %d\n", key);
            return false;
        }
    }

    // 0x103：模数原样（含 0 与负数）、只清当前帧槽。
    auto modulus = make_script_with_body();
    emit_instruction(modulus, 0x103, {{0, 7}});
    emit_instruction(modulus, 2, {});
    core::Machine modulus_machine(0x12345678u, table);
    modulus_machine.seed_hold_cursor(1, 5);
    modulus_machine.load_root(modulus);
    if (!modulus_machine.step() || modulus_machine.hold_modulus() != 7 ||
        !modulus_machine.hold_cursor() || *modulus_machine.hold_cursor() != 0 ||
        !modulus_machine.hold_cursor(1) || *modulus_machine.hold_cursor(1) != 5) {
        std::fprintf(stderr, "0x103 modulus / per-slot cursor mismatch\n");
        return false;
    }
    auto modulus_zero = make_script_with_body();
    emit_instruction(modulus_zero, 0x103, {{0, 0}});
    emit_instruction(modulus_zero, 2, {});
    core::Machine zero_mod_machine(0x12345678u, table);
    zero_mod_machine.load_root(modulus_zero);
    if (!zero_mod_machine.step() || zero_mod_machine.hold_modulus() != 0) {
        std::fprintf(stderr, "0x103 rejected modulus 0\n");
        return false;
    }

    // 0x105：32 位掩码原样（−1 = 全 1），不碰输入字。
    auto mask = make_script_with_body();
    emit_instruction(mask, 0x105, {{0, 0xFFFFFFFFu}});
    emit_instruction(mask, 2, {});
    core::Machine mask_machine(0x12345678u, table);
    mask_machine.set_input_word(0x5A);
    mask_machine.load_root(mask);
    if (!mask_machine.step() || mask_machine.hold_mask() != 0xFFFFFFFFu ||
        mask_machine.input_word() != 0x5A || mask_machine.frame().ip != 12) {
        std::fprintf(stderr, "0x105 mask mismatch (%u)\n", mask_machine.hold_mask());
        return false;
    }

    // 0x104：反向取最高置位；未命中不跳、撤销返回点；每条出口推进游标（模 7 回到 0）。
    auto dispatch = make_script_with_body();
    emit_instruction(dispatch, 0xFE, {{0, 8}});
    emit_instruction(dispatch, 0xFF, {});
    emit_instruction(dispatch, 0x103, {{0, 7}});
    emit_instruction(dispatch, 0x102, {{0, 7}, {0, 0}, {0, 20}});
    emit_instruction(dispatch, 0x104, {});
    for (int i = 0; i < 6; ++i) emit_instruction(dispatch, 0x1A8, {});
    emit_instruction(dispatch, 2, {});
    core::Machine dispatch_machine(0x12345678u, table);
    dispatch_machine.load_root(dispatch);
    for (int i = 0; i < 4; ++i) {  // 0xFE / 0xFF / 0x103 / 0x102（0xFF 会清输入字）
        if (!dispatch_machine.step()) {
            std::fprintf(stderr, "0x104 fixture failed: %s\n",
                         dispatch_machine.last_stop().message.c_str());
            return false;
        }
    }
    dispatch_machine.set_input_word((1u << 3) | (1u << 7));
    if (!dispatch_machine.step()) {
        std::fprintf(stderr, "0x104 dispatch failed: %s\n",
                     dispatch_machine.last_stop().message.c_str());
        return false;
    }
    if (dispatch_machine.frame().ip != 4u * 20u || dispatch_machine.frame().ret_stack.size() != 1 ||
        dispatch_machine.frame().ret_stack.back() != 15 || !dispatch_machine.hold_cursor() ||
        *dispatch_machine.hold_cursor() != 1) {
        std::fprintf(stderr, "0x104 dispatch mismatch ip=%zu cursor=%d\n",
                     dispatch_machine.frame().ip,
                     dispatch_machine.hold_cursor() ? *dispatch_machine.hold_cursor() : -1);
        return false;
    }

    // 0x104：掩码命中 ⇒ 返回点 = 本条 + 扫描上限夹到该键；上下文编号变化 ⇒ 清游标。
    auto masked = make_script_with_body();
    emit_instruction(masked, 0xFE, {{0, 8}});
    emit_instruction(masked, 0xFF, {});
    emit_instruction(masked, 0x103, {{0, 10}});
    emit_instruction(masked, 0x104, {});
    emit_instruction(masked, 0x104, {});
    emit_instruction(masked, 0x105, {{0, 1u << 7}});
    emit_instruction(masked, 0x104, {});
    emit_instruction(masked, 2, {});
    core::Machine masked_machine(0x12345678u, table);
    masked_machine.load_root(masked);
    for (int i = 0; i < 5; ++i) {  // 0xFE / 0xFF / 0x103 / 0x104 / 0x104
        if (!masked_machine.step()) return false;
    }
    if (masked_machine.hold_context_key() != 8 || !masked_machine.hold_cursor() ||
        *masked_machine.hold_cursor() != 2) {
        std::fprintf(stderr, "0x104 pre-mask cursor mismatch cursor=%d ctx=%d\n",
                     masked_machine.hold_cursor() ? *masked_machine.hold_cursor() : -1,
                     masked_machine.hold_context_key());
        return false;
    }
    if (!masked_machine.step()) return false;  // 0x105
    masked_machine.set_input_word(1u << 7);
    if (!masked_machine.step()) return false;  // 0x104：掩码命中
    if (!masked_machine.hold_cursor() || *masked_machine.hold_cursor() != 1 ||
        masked_machine.hold_context_key() != 7 || masked_machine.frame().key_limit != 7 ||
        !masked_machine.frame().ret_stack.empty()) {
        std::fprintf(stderr,
                     "0x104 masked/context path mismatch cursor=%d ctx=%d limit=%d stack=%zu\n",
                     masked_machine.hold_cursor() ? *masked_machine.hold_cursor() : -1,
                     masked_machine.hold_context_key(), masked_machine.frame().key_limit,
                     masked_machine.frame().ret_stack.size());
        return false;
    }
    return true;
}

// ── 存档族（0x19E / 0x1A0 / 0x1A1 / 0x1AB / 0x1AC / 0x1AE / 0x1AF） ──
namespace {
struct FakeSaveHost final : core::SaveHost {
    std::map<std::string, core::Bytes> files;
    std::vector<std::string> calls;
    bool confirm_answer = false;
    bool fail_dat_remove = false;
    bool fail_sth_remove = false;
    bool fail_dat_copy = false;
    bool fail_sth_copy = false;
    std::optional<core::SurfacePixels> surface;

    std::string app_data_root() override { return ""; }
    bool ensure_directory(const std::string&) override { return true; }
    core::ReadResult read(const std::string& path) override {
        const auto it = files.find(path);
        if (it == files.end()) return {core::ReadOutcome::OpenFailed, {}};
        return {core::ReadOutcome::Ok, it->second};
    }
    core::WriteOutcome write_create_always(const std::string& path, core::ByteSpan bytes) override {
        calls.push_back("write:" + path + ":" + std::to_string(bytes.size()));
        files[path] = core::Bytes(bytes.begin(), bytes.end());
        return core::WriteOutcome::Ok;
    }
    bool remove(const std::string& path) override {
        calls.push_back("remove:" + path);
        if (path.ends_with(".DAT") && fail_dat_remove) return false;
        if (path.ends_with(".STH") && fail_sth_remove) return false;
        return files.erase(path) != 0;
    }
    bool copy_overwrite(const std::string& from, const std::string& to) override {
        calls.push_back("copy:" + from + ">" + to);
        if (from.ends_with(".DAT") && fail_dat_copy) return false;
        if (from.ends_with(".STH") && fail_sth_copy) return false;
        const auto it = files.find(from);
        if (it == files.end()) return false;
        files[to] = it->second;
        return true;
    }
    bool confirm_overwrite(std::int32_t slot) override {
        calls.push_back("confirm:" + std::to_string(slot));
        return confirm_answer;
    }
    std::optional<core::SurfacePixels> capture_surface(std::int32_t object_id) override {
        calls.push_back("capture:" + std::to_string(object_id));
        return surface;
    }
    int count(const std::string& prefix) const {
        int total = 0;
        for (const auto& call : calls) {
            if (call.rfind(prefix, 0) == 0) total += 1;
        }
        return total;
    }
};

core::Bytes make_save_header(std::string_view magic, std::string_view title) {
    core::Bytes bytes(292, 0u);
    std::memcpy(bytes.data(), magic.data(), magic.size());
    std::memcpy(bytes.data() + 8, title.data(), title.size());
    const auto put16 = [&bytes](std::size_t offset, std::uint32_t value) {
        bytes[offset] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    put16(264, 2026);  // 年
    put16(266, 10);    // 月
    put16(268, 7);     // 星期（跳过）
    put16(270, 26);    // 日
    put16(272, 2);     // 时
    put16(274, 33);    // 分
    put16(276, 44);    // 秒
    put16(278, 77);    // 毫秒（跳过）
    bytes[280] = 0x10;  // 游戏时间（u32 原样）
    return bytes;
}
}  // namespace

bool check_save_ops(const core::OpcodeTable& table) {
    if (table.find(0x1A0) == nullptr || table.find(0x55) == nullptr) {
        return true;  // 与 check_input_dispatch 同一前提：没有真表就跳过
    }

    // 0x1a0：打不开 ⇒ 1 且 op3..9 保持原值；头合法 ⇒ 0 + 七个字段；短读/坏魔数 ⇒ 2。
    auto header_script = make_script_with_body();
    for (int i = 2; i <= 8; ++i) emit_instruction(header_script, 0x55, {{3, static_cast<std::uint32_t>(i)}, {0, 7}});
    emit_instruction(header_script, 0x1A0, {{3, 0}, {0, 1}, {3, 2}, {3, 3}, {3, 4}, {3, 5}, {3, 6}, {3, 7}, {3, 8}});
    emit_instruction(header_script, 2, {});
    FakeSaveHost host;
    host.files["SAVE\\SAVE01.DAT"] = make_save_header("S4SD", "TENKETSU");
    core::Machine save_machine(0x12345678u, table);
    save_machine.set_save_host(&host);
    save_machine.set_save_identity("TENKETSU", "S4");
    save_machine.load_root(header_script);
    for (int i = 0; i < 7; ++i) {
        if (!save_machine.step()) return false;
    }
    if (!save_machine.step()) return false;  // 0x1a0
    if (save_machine.read_int(core::Operand{3, 0}) != 0 ||
        save_machine.read_int(core::Operand{3, 2}) != 2026 ||
        save_machine.read_int(core::Operand{3, 7}) != 44 ||
        save_machine.read_int(core::Operand{3, 8}) != 0x10) {
        std::fprintf(stderr, "0x1a0 header read mismatch\n");
        return false;
    }

    // 打不开：状态 1，op3..9 一律不动。
    auto missing_script = make_script_with_body();
    for (int i = 2; i <= 8; ++i) emit_instruction(missing_script, 0x55, {{3, static_cast<std::uint32_t>(i)}, {0, 7}});
    emit_instruction(missing_script, 0x1A0, {{3, 0}, {0, 9}, {3, 2}, {3, 3}, {3, 4}, {3, 5}, {3, 6}, {3, 7}, {3, 8}});
    emit_instruction(missing_script, 2, {});
    FakeSaveHost missing_host;
    core::Machine missing_machine(0x12345678u, table);
    missing_machine.set_save_host(&missing_host);
    missing_machine.load_root(missing_script);
    for (int i = 0; i < 8; ++i) {
        if (!missing_machine.step()) return false;
    }
    if (missing_machine.read_int(core::Operand{3, 0}) != 1 ||
        missing_machine.read_int(core::Operand{3, 2}) != 7) {
        std::fprintf(stderr, "0x1a0 missing-file path wrote op3..\n");
        return false;
    }

    // 坏魔数（S3SD）⇒ 2（本实现只接受本族）。
    auto wrong_family = make_script_with_body();
    emit_instruction(wrong_family, 0x1A0, {{3, 0}, {0, 1}, {3, 2}, {3, 3}, {3, 4}, {3, 5}, {3, 6}, {3, 7}, {3, 8}});
    emit_instruction(wrong_family, 2, {});
    FakeSaveHost wrong_host;
    wrong_host.files["SAVE\\SAVE01.DAT"] = make_save_header("S3SD", "TENKETSU");
    core::Machine wrong_machine(0x12345678u, table);
    wrong_machine.set_save_host(&wrong_host);
    wrong_machine.set_save_identity("TENKETSU", "S4");
    wrong_machine.load_root(wrong_family);
    if (!wrong_machine.step() || wrong_machine.read_int(core::Operand{3, 0}) != 2) {
        std::fprintf(stderr, "0x1a0 accepted a foreign family magic\n");
        return false;
    }

    // 0x1a1：打不开 ⇒ 1；打开成功（哪怕头坏）⇒ op1 不变 + 整池重编码。
    auto load_script = make_script_with_body();
    emit_instruction(load_script, 0x55, {{3, 0}, {0, 7}});
    emit_instruction(load_script, 0x1A1, {{3, 1}, {0, 1}});
    emit_instruction(load_script, 2, {});
    FakeSaveHost load_host;
    load_host.files["SAVE\\SAVE01.DAT"] = core::Bytes(10, 0u);  // 头坏但打开成功
    core::Machine load_machine(0x12345678u, table);
    load_machine.set_save_host(&load_host);
    load_machine.load_root(load_script);
    load_machine.seed_global_int_count(0);
    if (!load_machine.step() || !load_machine.step()) return false;  // mov + 0x1a1
    const auto expected = static_cast<std::int32_t>(load_machine.encode_int(7));
    if (load_machine.read_int(core::Operand{3, 0}) != expected ||
        load_machine.read_int(core::Operand{3, 1}) != 0) {
        std::fprintf(stderr, "0x1a1 recode mismatch (%d vs %d)\n",
                     load_machine.read_int(core::Operand{3, 0}), expected);
        return false;
    }
    auto load_missing = make_script_with_body();
    emit_instruction(load_missing, 0x1A1, {{3, 1}, {0, 5}});
    emit_instruction(load_missing, 2, {});
    FakeSaveHost load_missing_host;
    core::Machine load_missing_machine(0x12345678u, table);
    load_missing_machine.set_save_host(&load_missing_host);
    load_missing_machine.load_root(load_missing);
    if (!load_missing_machine.step() || load_missing_machine.read_int(core::Operand{3, 1}) != 1) {
        std::fprintf(stderr, "0x1a1 missing-file status mismatch\n");
        return false;
    }

    // 0x1ab：两个文件都尝试；后一步覆盖前一步（都失败 ⇒ 2）。
    auto remove_script = make_script_with_body();
    emit_instruction(remove_script, 0x1AB, {{3, 0}, {0, 3}});
    emit_instruction(remove_script, 2, {});
    FakeSaveHost remove_host;
    remove_host.files["SAVE\\SAVE03.DAT"] = {1u};
    remove_host.files["SAVE\\SAVE03.STH"] = {2u};
    core::Machine remove_machine(0x12345678u, table);
    remove_machine.set_save_host(&remove_host);
    remove_machine.load_root(remove_script);
    if (!remove_machine.step() || remove_machine.read_int(core::Operand{3, 0}) != 0 ||
        remove_host.files.count("SAVE\\SAVE03.DAT") != 0) {
        std::fprintf(stderr, "0x1ab delete mismatch\n");
        return false;
    }
    FakeSaveHost remove_fail_host;
    remove_fail_host.files["SAVE\\SAVE03.DAT"] = {1u};
    remove_fail_host.fail_dat_remove = true;
    remove_fail_host.fail_sth_remove = true;
    core::Machine remove_fail_machine(0x12345678u, table);
    remove_fail_machine.set_save_host(&remove_fail_host);
    remove_fail_machine.load_root(remove_script);
    if (!remove_fail_machine.step() || remove_fail_machine.read_int(core::Operand{3, 0}) != 2 ||
        remove_fail_host.count("remove:") != 2) {
        std::fprintf(stderr, "0x1ab did not override status with the second file\n");
        return false;
    }

    // 0x1ac：覆盖目标（不因已存在而失败）。
    auto copy_script = make_script_with_body();
    emit_instruction(copy_script, 0x1AC, {{3, 0}, {0, 4}, {0, 5}});
    emit_instruction(copy_script, 2, {});
    FakeSaveHost copy_host;
    copy_host.files["SAVE\\SAVE04.DAT"] = {9u};
    copy_host.files["SAVE\\SAVE04.STH"] = {7u};
    copy_host.files["SAVE\\SAVE05.DAT"] = {1u};
    core::Machine copy_machine(0x12345678u, table);
    copy_machine.set_save_host(&copy_host);
    copy_machine.load_root(copy_script);
    if (!copy_machine.step() || copy_machine.read_int(core::Operand{3, 0}) != 0 ||
        copy_host.files["SAVE\\SAVE05.DAT"] != core::Bytes{9u} ||
        copy_host.files["SAVE\\SAVE05.STH"] != core::Bytes{7u}) {
        std::fprintf(stderr, "0x1ac overwrite mismatch\n");
        return false;
    }

    // 0x1ae：先建空文件、后捕捉；捕捉失败 ⇒ 2 且不删除。
    auto thumb_write = make_script_with_body();
    emit_instruction(thumb_write, 0x1AE, {{3, 0}, {0, 6}, {0, 3}});
    emit_instruction(thumb_write, 2, {});
    FakeSaveHost thumb_host;
    core::SurfacePixels surface;
    surface.width = 2;
    surface.height = 2;
    surface.bytes_per_pixel = 3;
    surface.stride_bytes = 6;
    surface.r_mask = 0x00FF0000u;
    surface.g_mask = 0x0000FF00u;
    surface.b_mask = 0x000000FFu;
    // bottom-up：第 1 行 = 画面最后一行（蓝、绿）。
    surface.pixels = {0, 0, 255, 0, 255, 0, 255, 0, 0, 0, 0, 255};
    thumb_host.surface = surface;
    core::Machine thumb_machine(0x12345678u, table);
    thumb_machine.set_save_host(&thumb_host);
    thumb_machine.load_root(thumb_write);
    if (!thumb_machine.step() || thumb_machine.read_int(core::Operand{3, 0}) != 0) return false;
    const auto bmp = thumb_host.files["SAVE\\SAVE06.STH"];
    if (bmp.size() != 54u + 16u || bmp[0] != 'B' || bmp[1] != 'M' ||
        (bmp[2] | (bmp[3] << 8)) != 16 + 40) {
        std::fprintf(stderr, "0x1ae BMP header mismatch (size=%zu)\n", bmp.size());
        return false;
    }
    FakeSaveHost thumb_fail_host;
    thumb_fail_host.surface = std::nullopt;
    core::Machine thumb_fail_machine(0x12345678u, table);
    thumb_fail_machine.set_save_host(&thumb_fail_host);
    thumb_fail_machine.load_root(thumb_write);
    if (!thumb_fail_machine.step() || thumb_fail_machine.read_int(core::Operand{3, 0}) != 2 ||
        thumb_fail_host.count("capture:") != 1 || thumb_fail_host.count("remove:") != 0) {
        std::fprintf(stderr, "0x1ae capture-failure path mismatch\n");
        return false;
    }

    // 0x1af：打不开优先于表面门（1 而不是 2）；0/1/2 被拒 ⇒ 2；合法 BMP ⇒ 0 + 纹理落地。
    auto thumb_read = make_script_with_body();
    emit_instruction(thumb_read, 0x1AF, {{3, 0}, {0, 6}, {0, 0}});
    emit_instruction(thumb_read, 2, {});
    FakeSaveHost read_missing_host;
    core::Machine read_missing_machine(0x12345678u, table);
    read_missing_machine.set_save_host(&read_missing_host);
    read_missing_machine.load_root(thumb_read);
    if (!read_missing_machine.step() || read_missing_machine.read_int(core::Operand{3, 0}) != 1) {
        std::fprintf(stderr, "0x1af open-failure must win over the surface gate\n");
        return false;
    }
    auto thumb_read_ok = make_script_with_body();
    emit_instruction(thumb_read_ok, 0x1AF, {{3, 0}, {0, 6}, {0, 9}});
    emit_instruction(thumb_read_ok, 2, {});
    FakeSaveHost read_ok_host;
    read_ok_host.files["SAVE\\SAVE06.STH"] = bmp;
    core::Machine read_ok_machine(0x12345678u, table);
    read_ok_machine.set_save_host(&read_ok_host);
    read_ok_machine.load_root(thumb_read_ok);
    if (!read_ok_machine.step() || read_ok_machine.read_int(core::Operand{3, 0}) != 0) {
        std::fprintf(stderr, "0x1af decode mismatch\n");
        return false;
    }
    const core::TextureRecord* decoded = read_ok_machine.texture(9);
    if (decoded == nullptr || decoded->width != 2 || decoded->height != 2 ||
        decoded->rgba.size() != 16 || decoded->rgba[0] != 255 || decoded->rgba[1] != 0 ||
        decoded->rgba[2] != 0) {  // 画面第 1 行、左像素 = 红
        std::fprintf(stderr, "0x1af texture landing mismatch\n");
        return false;
    }
    return true;
}

// ── 触点查询 / 命中查找（0x2FC · 0x12E） ──
bool check_mouse_and_hit_test(const core::OpcodeTable& table) {
    const auto make_mouse_script = [&]() {
        auto bytes = make_script_with_body();
        for (std::uint32_t slot = 2; slot <= 5; ++slot) {
            emit_instruction(bytes, 0x55, {{3, slot}, {0, 77}});  // 预置 2..5 槽
        }
        emit_instruction(bytes, 0x2FC, {{3, 1}, {3, 2}, {3, 3}, {3, 4}, {3, 5}});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    core::Machine mouse_none(0x12345678u, table);
    mouse_none.load_root(make_mouse_script());
    for (int i = 0; i < 5; ++i) {
        if (!mouse_none.step()) {
            std::fprintf(stderr, "0x2fc no-event fixture failed: %s\n",
                         mouse_none.last_stop().message.c_str());
            return false;
        }
    }
    if (mouse_none.read_int(core::Operand{3, 1}) != 0 ||
        mouse_none.read_int(core::Operand{3, 2}) != 77 ||
        mouse_none.read_int(core::Operand{3, 5}) != 77) {
        std::fprintf(stderr, "0x2fc no-event wrote the coordinates\n");
        return false;
    }
    core::Machine mouse_some(0x12345678u, table);
    mouse_some.set_touch_source([](core::Machine::TouchPoint& event) {
        event = core::Machine::TouchPoint{10, 20, 0x11, 3};
        return true;
    });
    mouse_some.load_root(make_mouse_script());
    for (int i = 0; i < 5; ++i) {
        if (!mouse_some.step()) return false;
    }
    if (mouse_some.read_int(core::Operand{3, 1}) != 1 ||
        mouse_some.read_int(core::Operand{3, 2}) != 10 ||
        mouse_some.read_int(core::Operand{3, 3}) != 20 ||
        mouse_some.read_int(core::Operand{3, 4}) != 0x11 ||
        mouse_some.read_int(core::Operand{3, 5}) != 3) {
        std::fprintf(stderr, "0x2fc event write mismatch\n");
        return false;
    }

    // 0x12e：TITLE 同形五框命中；只写第 1 个操作数，推进格数 = 17。
    // 夹具：margin = 0、框 [0,0x9c,0,0x9c] × 5、基准平面 = TITLE 的绘制坐标。
    const std::int32_t plane_x[5] = {0x44e, 0x3e0, 0x365, 0x2d9, 0x453};
    const std::int32_t plane_y[5] = {0x126, 0x192, 0x1e5, 0x21f, 0x22a};
    const auto make_hit_script = [&]() {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x12E,
                         {{9, 0x3f5}, {9, 1}, {9, 0x3f0}, {9, 0x3f1},
                          {9, 0xcd}, {9, 5}, {9, 0x69}, {9, 0}});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    const auto put_local = [](core::Machine& machine, std::uint32_t slot, std::int32_t value) {
        machine.write_int(core::Operand{9, slot}, value);
    };
    const auto get_local = [](core::Machine& machine, std::uint32_t slot) {
        return machine.read_int(core::Operand{9, slot});
    };
    const auto fill_hit = [&](core::Machine& machine, std::int32_t start, std::int32_t x,
                              std::int32_t y) {
        put_local(machine, 0x3f5, start);
        for (std::int32_t slot = 1; slot <= 4; ++slot) put_local(machine, slot, 0);
        for (int i = 0; i < 5; ++i) {
            const auto base = 0xcd + static_cast<std::uint32_t>(i) * 4;
            put_local(machine, base + 0, 0);
            put_local(machine, base + 1, 0x9c);
            put_local(machine, base + 2, 0);
            put_local(machine, base + 3, 0x9c);
            put_local(machine, 5 + static_cast<std::uint32_t>(i), plane_x[i]);
            put_local(machine, 0x69 + static_cast<std::uint32_t>(i), plane_y[i]);
        }
        put_local(machine, 0, 5);
        put_local(machine, 0x3f0, x);
        put_local(machine, 0x3f1, y);
        // 第 2..8 个操作数只读：预置哨兵，执行后必须仍是 77。
        for (std::int32_t slot = 0x3f6; slot <= 0x3fd; ++slot) put_local(machine, slot, 77);
    };
    const auto hit_result = [&](std::int32_t start, std::int32_t x, std::int32_t y) {
        core::Machine machine(0x12345678u, table);
        machine.load_root(make_hit_script());
        fill_hit(machine, start, x, y);
        if (!machine.step()) return std::int32_t{-999};
        for (std::int32_t slot = 0x3f6; slot <= 0x3fd; ++slot) {
            if (get_local(machine, slot) != 77) return std::int32_t{-998};
        }
        if (machine.frame().ip != 68) return std::int32_t{-997};
        return get_local(machine, 0x3f5);
    };
    if (hit_result(-1, 0x3c0, 0x220) != 2) {
        std::fprintf(stderr, "0x12e must hit the second box\n");
        return false;
    }
    if (hit_result(2, 0x3c0, 0x220) != -1) {
        std::fprintf(stderr, "0x12e must scan from start + 1\n");
        return false;
    }
    if (hit_result(-1, 0, 0) != -1) {
        std::fprintf(stderr, "0x12e must miss outside every box\n");
        return false;
    }
    // count = 0 ⇒ 不读框表，直接写 -1。
    {
        core::Machine machine(0x12345678u, table);
        machine.load_root(make_hit_script());
        fill_hit(machine, -1, 0x3c0, 0x220);
        put_local(machine, 0, 0);
        if (!machine.step() || get_local(machine, 0x3f5) != -1) {
            std::fprintf(stderr, "0x12e count=0 must write -1\n");
            return false;
        }
    }
    // 补偿四元组与当前框记录同址 ⇒ 跳过该项（不是命中、也不终止扫描）。
    // 下标 0 = 四元组本身（m = 0），下标 1 = 正常框；光标同时落在两者内 ⇒ 必须返回 1。
    {
        auto alias_script = make_script_with_body();
        emit_instruction(alias_script, 0x12E,
                         {{9, 0x3f5}, {9, 1}, {9, 0x3f0}, {9, 0x3f1},
                          {9, 1}, {9, 9}, {9, 0xa}, {9, 0}});
        emit_instruction(alias_script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(alias_script);
        put_local(machine, 0x3f5, -1);
        for (std::uint32_t slot = 1; slot <= 4; ++slot) put_local(machine, slot, 0);
        put_local(machine, 5, 0);
        put_local(machine, 6, 0x9c);
        put_local(machine, 7, 0);
        put_local(machine, 8, 0x9c);
        put_local(machine, 0, 2);
        put_local(machine, 9, 0);
        put_local(machine, 0xa, 0);
        put_local(machine, 0x3f0, 0);
        put_local(machine, 0x3f1, 0);
        if (!machine.step() || get_local(machine, 0x3f5) != 1) {
            std::fprintf(stderr, "0x12e same-address record must be skipped\n");
            return false;
        }
    }
    return true;
}

// ── 退出清理（0x1F6 · 0x23D · 0x32B · 0x2） ──
// TITLE 的退出路径必须先跑 0x1F6 / 0x23D / 0x32B，再 `exit`；缺 handler 会停在
// `opcode-without-handler:573/811`，窗口宿主也永远不该把"脚本已结束"当成"继续等用户关窗"。
bool check_exit_cleanup(const core::OpcodeTable& table) {
    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x1F6, {});
    emit_instruction(bytes, 0x23D, {});
    emit_instruction(bytes, 0x32B, {});
    emit_instruction(bytes, 0x2, {});
    core::Machine machine(0x12345678u, table);
    machine.load_root(bytes);
    for (int i = 0; i < 3; ++i) {
        if (!machine.step()) {
            std::fprintf(stderr, "exit cleanup fixture failed at step %d: %s\n", i,
                         machine.last_stop().message.c_str());
            return false;
        }
    }
    // 第 4 步是 `exit`：它把根帧结束成 Halted，因此 step() 返回 false 是**预期**。
    machine.step();
    if (!machine.halted() || machine.last_stop().reason != core::StopReason::Halted) {
        std::fprintf(stderr, "exit cleanup must end with root-exit, reason=%d msg=%s\n",
                     static_cast<int>(machine.last_stop().reason),
                     machine.last_stop().message.c_str());
        return false;
    }
    return true;
}

// ── 纹理槽 → 文件绑定（0x216 / 0x23d） ──
bool check_slot_imgid(const core::OpcodeTable& table) {
    // 从未写入的槽 500：记录表初始化写 -1。
    auto fresh = make_script_with_body();
    emit_instruction(fresh, 0x216, {{3, 6}, {0, 500}});
    core::Machine fresh_machine(0x12345678u, table);
    fresh_machine.load_root(fresh);
    if (!fresh_machine.step() || fresh_machine.global_int(6) != -1) {
        std::fprintf(stderr, "0x216 uncreated slot must read -1, got %d\n",
                     fresh_machine.global_int(6));
        return false;
    }

    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x1FA, {{0, 41}});
    emit_instruction(bytes, 0x23D, {});
    emit_instruction(bytes, 0x216, {{3, 1}, {0, 42}});
    emit_instruction(bytes, 0x216, {{3, 2}, {0, 999}});
    emit_instruction(bytes, 2, {});
    core::Machine machine(0x12345678u, table);
    machine.set_texture_file(41, 7);
    machine.set_texture_file(42, 8);
    machine.set_texture_file(999, 9);
    machine.set_surface_release_gate(41, 1);
    machine.set_surface_release_gate(42, 1);
    machine.load_root(bytes);
    for (int i = 0; i < 4; ++i) if (!machine.step()) return false;
    // 0x1fa 守卫非 0 ⇒ 41 保留；0x23d 守卫非 0 的 42 保留、守卫 0 的 999 清掉。
    if (machine.global_int(1) != 8 || machine.global_int(2) != -1) {
        std::fprintf(stderr, "0x216 after guards must read 8/-1, got %d/%d\n",
                     machine.global_int(1), machine.global_int(2));
        return false;
    }
    const auto* t41 = machine.texture(41);
    if (t41 == nullptr || t41->file != 7) {
        std::fprintf(stderr, "0x1fa release guard must keep slot 41, got %d\n",
                     t41 == nullptr ? -999 : t41->file);
        return false;
    }
    // 槽号越界是 UB：实现必须响亮失败，⛔ 不返回 -1。
    auto bad = make_script_with_body();
    emit_instruction(bad, 0x216, {{3, 3}, {0, 1000}});
    core::Machine oob(0x12345678u, table);
    oob.load_root(bad);
    oob.step();
    if (!oob.halted() ||
        oob.last_stop().message.find("texture-slot-out-of-range") == std::string::npos) {
        std::fprintf(stderr, "0x216 slot 1000 must fail loudly, got %s\n",
                     oob.last_stop().message.c_str());
        return false;
    }

    // 0x23d 把纹理槽对象也清掉：0x208 对槽 50 必须 0/0（记录仍保留来源 -1）。
    auto slot = make_script_with_body();
    emit_instruction(slot, 0x1F8, {{0, 50}, {0, 4}, {0, 4}, {0, 0}});
    emit_instruction(slot, 0x23D, {});
    emit_instruction(slot, 0x208, {{0, 50}, {3, 4}, {3, 5}});
    emit_instruction(slot, 2, {});
    core::Machine slot_machine(0x12345678u, table);
    slot_machine.load_root(slot);
    for (int i = 0; i < 3; ++i) if (!slot_machine.step()) return false;
    if (slot_machine.global_int(4) != 0 || slot_machine.global_int(5) != 0) {
        std::fprintf(stderr, "0x23d must empty texture slot 50, 0x208 got %d/%d\n",
                     slot_machine.global_int(4), slot_machine.global_int(5));
        return false;
    }
    return true;
}

// ── 场景颜色归一化（0x202 / 0x203 / 0x322 / 0x323） ──
bool check_color_normalization(const core::OpcodeTable& table) {
    auto bytes = make_script_with_body();
    // 0x203：key 1、第 2 操作数 7、alpha 0x44、color 0x112233。
    emit_instruction(bytes, 0x203, {{0, 1}, {0, 7}, {0, 0x44}, {0, 0x112233}});
    // 负 alpha / 负 color ⇒ 取现有值；第 2 操作数覆盖为 9。
    emit_instruction(bytes, 0x203, {{0, 1}, {0, 9}, {0, 0xffffffffu}, {0, 0xffffffffu}});
    emit_instruction(bytes, 2, {});
    core::Machine machine(0x12345678u, table);
    machine.load_root(bytes);
    if (!machine.step() || !machine.step()) return false;
    const auto it = machine.draw_items().find(1);
    if (it == machine.draw_items().end() || it->second.color != 0x44112233u ||
        it->second.set_color_int != 9) {
        std::fprintf(stderr, "0x203 negative color must keep existing ARGB and write op2\n");
        return false;
    }
    return true;
}

// ── 纹理尺寸（0x208；CORE-OPS-TEXTURE-SIZE） ──
bool check_texture_size(const core::OpcodeTable& table) {
    const auto make_size_script = [&](std::uint32_t id, bool create) {
        auto bytes = make_script_with_body();
        if (create) emit_instruction(bytes, 0x1F8, {{0, 3}, {0, 4}, {0, 6}, {0, 0}});
        emit_instruction(bytes, 0x208, {{0, id}, {3, 0}, {3, 1}});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    core::Machine size_machine(0x12345678u, table);
    size_machine.load_root(make_size_script(3, true));
    if (!size_machine.step() || !size_machine.step() ||
        size_machine.read_int(core::Operand{3, 0}) != 4 ||
        size_machine.read_int(core::Operand{3, 1}) != 6 || !size_machine.errors().empty()) {
        std::fprintf(stderr, "0x208 texture size mismatch w=%d h=%d\n",
                     size_machine.read_int(core::Operand{3, 0}),
                     size_machine.read_int(core::Operand{3, 1}));
        return false;
    }
    core::Machine size_oob(0x12345678u, table);
    size_oob.load_root(make_size_script(1000, false));
    if (!size_oob.step() || size_oob.errors().size() != 1 ||
        size_oob.read_int(core::Operand{3, 0}) != 0 ||
        size_oob.read_int(core::Operand{3, 1}) != 0) {
        std::fprintf(stderr, "0x208 out-of-range id was not reported\n");
        return false;
    }
    // ★ 上界是无符号比较：立即数 −1 也走越界支。
    core::Machine size_negative(0x12345678u, table);
    size_negative.load_root(make_size_script(0xFFFFFFFFu, false));
    if (!size_negative.step() || size_negative.errors().size() != 1 ||
        size_negative.read_int(core::Operand{3, 0}) != 0) {
        std::fprintf(stderr, "0x208 unsigned upper bound mismatch\n");
        return false;
    }
    // 范围内但槽空 ⇒ 0/0 且**不**上报（越界与槽空不是同一失败）。
    core::Machine size_empty(0x12345678u, table);
    size_empty.load_root(make_size_script(5, false));
    if (!size_empty.step() || !size_empty.errors().empty() ||
        size_empty.read_int(core::Operand{3, 0}) != 0 ||
        size_empty.read_int(core::Operand{3, 1}) != 0) {
        std::fprintf(stderr, "0x208 empty slot reported an error\n");
        return false;
    }
    return true;
}

// ── 全局重置（0x009；CORE-OPS-FRAME-EXIT-SCRIPT） ──
bool check_exit_script(const core::OpcodeTable& table) {
    auto exit_script = make_script_with_body();
    emit_instruction(exit_script, 0x130, {{3, 0}});  // 位置 0..2：全局整型 0 ← show-logo
    emit_instruction(exit_script, 0x09, {});         // 位置 3
    emit_instruction(exit_script, 2, {});            // 位置 4
    // 没有装载器：复位（状态字 / 帧槽 / 池）先发生，装载才失败。
    core::Machine no_loader(0x12345678u, table);
    no_loader.load_root(exit_script);
    if (!no_loader.step() || no_loader.read_int(core::Operand{3, 0}) != 1) {
        std::fprintf(stderr, "0x09 fixture did not read show-logo\n");
        return false;
    }
    if (no_loader.step() ||
        no_loader.last_stop().message != "exit-script-loader-missing" ||
        no_loader.state_flags() != 0 || !no_loader.frame().code.empty()) {
        std::fprintf(stderr, "0x09 reset-before-reload mismatch: %s\n",
                     no_loader.last_stop().message.c_str());
        return false;
    }
    // 有装载器：以统一文件 id 0 重装根脚本，并从它的 dword 0 继续。
    const auto root_dir =
        std::filesystem::temp_directory_path() / "amayui-cxx-selftest-install";
    std::filesystem::create_directories(root_dir);
    {
        std::ofstream root_file(root_dir / "ROOT.BIN", std::ios::binary);
        root_file.write(reinterpret_cast<const char*>(exit_script.data()),
                        static_cast<std::streamsize>(exit_script.size()));
    }
    core::Install install;
    install.root = root_dir;
    install.index.files.push_back(core::AlfFile{"ROOT.BIN", 0, 0, 0, 0});
    core::Machine reload(0x12345678u, table, &install);
    reload.load_root(exit_script);
    if (!reload.step() || reload.read_int(core::Operand{3, 0}) != 1) return false;
    if (!reload.step() || reload.current_frame() != 0 || reload.state_flags() != 0 ||
        reload.frame().ip != 0 || reload.frame().script_id != 0) {
        std::fprintf(stderr, "0x09 reload mismatch frame=%zu ip=%zu\n",
                     reload.current_frame(), reload.frame().ip);
        return false;
    }
    // 重装后第一条又是 0x130：这次 show-logo 已是 0 ⇒ 证明是从 dword 0 重新开始。
    if (!reload.step() || reload.read_int(core::Operand{3, 0}) != 0) {
        std::fprintf(stderr, "0x09 did not restart at dword 0 of the new root\n");
        return false;
    }
    std::filesystem::remove_all(root_dir);
    return true;
}

}  // namespace

// ── 设置界面这批指令（合成脚本；不依赖游戏安装 ⇒ 可以算 pure 档行为守卫）─────────────────────
// 每条都从"破坏就该红"的角度挑：早退不写键 / 越界不写落点 / 越界不读操作数 / 缺键不建键 /
// 同态 no-op / 后端不存在不清槽格 / 无同曲判据 / 无在播判据。
bool check_config_ui_surface(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "config-ui: %s\n", what);
        return false;
    };

    // ① 0x2cf 更换路径：缓存 0→1，且命名键被写成**归一值** 1
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2CF, {{0, 1}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.config_mut().set_int("display:ScreenMode", 0);
        machine.config_mut().set_int("display:ForceScreen", 0);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x2cf(1) 停机");
        if (machine.display_mode_cache() != 1) return fail("0x2cf(1) 没有把缓存改成 1");
        if (machine.config().get_int("display:ScreenMode") != 1) {
            return fail("0x2cf(1) 没有把 display:ScreenMode 写成 1");
        }
    }

    // ② 0x2cf 早退：目标 == 缓存 ⇒ 一个键都不写（键里放 7 当标记）
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2CF, {{0, 0}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.config_mut().set_int("display:ScreenMode", 7);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x2cf(0) 早退支停机");
        if (machine.display_mode_cache() != 0) return fail("0x2cf(0) 早退支改了缓存");
        if (machine.config().get_int("display:ScreenMode") != 7) {
            return fail("0x2cf(0) 早退支写了键（应当一个键都不写）");
        }
    }

    // ③ 0x2cf 越界静默：不改缓存、不写键、不记错误
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2CF, {{0, 2}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.config_mut().set_int("display:ScreenMode", 7);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x2cf(2) 越界支不该停机");
        if (machine.display_mode_cache() != 0 || machine.config().get_int("display:ScreenMode") != 7) {
            return fail("0x2cf(2) 越界支改了状态（应当是静默）");
        }
        if (!machine.errors().empty()) return fail("0x2cf(2) 越界支记了错误（应当静默）");
    }

    // ④ 0x307：缺键**不建键**；键存在时写**原值**（3 就是 3）
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x307, {{0, 3}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x307 停机");
        if (machine.config().has_named("system:EffectSkipOnClick")) {
            return fail("0x307 在缺键时把键建出来了（应当只上报、不创建）");
        }
        core::Machine with_key(key, table);
        with_key.config_mut().set_int("system:EffectSkipOnClick", 1);
        with_key.load_root(bytes);
        if (!with_key.step()) return fail("0x307（键存在）停机");
        if (with_key.config().get_int("system:EffectSkipOnClick") != 3) {
            return fail("0x307 没有写原值 3");
        }
    }

    // ⑤ 0x1b5：同一个值写进命名键**与**参数格（用 0x7f 读回参数格）
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x1B5, {{0, 7}});
        emit_instruction(bytes, 0x7F, {{9, 5}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.config_mut().set_int("message:MessageSpeed", 0);
        machine.load_root(bytes);
        if (!machine.step() || !machine.step()) return fail("0x1b5/0x7f 停机");
        if (machine.config().get_int("message:MessageSpeed") != 7) return fail("0x1b5 没写命名键");
        if (machine.local_int(5) != 7) return fail("0x1b5 没写参数格（0x7f 读回的不是 7）");
    }

    // ⑥ 0x1b8：命中写值 / 越界**不写落点**
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x1B8, {{0, 0}, {9, 5}});
        emit_instruction(bytes, 0x1B8, {{0, 1}, {9, 6}});
        emit_instruction(bytes, 0x55, {{9, 7}, {0, 42}});   // 落点预置 42
        emit_instruction(bytes, 0x1B8, {{0, 9}, {9, 7}});   // 越界
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.config_mut().set_int("message:AutoMessageTime0", 500);
        machine.config_mut().set_int("message:AutoMessageTime1", 2000);
        machine.load_root(bytes);
        for (int i = 0; i < 4; ++i) {
            if (!machine.step()) return fail("0x1b8 停机");
        }
        if (machine.local_int(5) != 500) return fail("0x1b8 档 0 没写到落点（期望 500）");
        if (machine.local_int(6) != 2000) return fail("0x1b8 档 1 没写到落点（期望 2000）");
        if (machine.local_int(7) != 42) return fail("0x1b8 越界改了落点（应当一个格都不写）");
    }

    // ⑦ 0x1b9：写对应键；越界**连第 2 个操作数都不读**（给不可读的指针类型也不许抛）
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x1B9, {{0, 1}, {0, 33}});
        emit_instruction(bytes, 0x1B9, {{0, 2}, {8, 0}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x1b9 停机");
        if (machine.config().get_int("message:AutoMessageTime1") != 33) return fail("0x1b9 没写档 1");
        if (!machine.step()) return fail("0x1b9 越界支读了第 2 个操作数（抛了）");
        if (machine.config().get_int("message:AutoMessageTime0") != 0) return fail("0x1b9 越界支写了键");
    }

    // ⑧ 0x1ba：音乐 ±3 阶梯（同侧 no-op）/ 音效同态 no-op
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x1BA, {{0, 1}, {0, 0}});
        emit_instruction(bytes, 0x1BA, {{0, 1}, {0, 0}});
        emit_instruction(bytes, 0x1BA, {{0, 1}, {0, 1}});
        emit_instruction(bytes, 0x1BA, {{0, 2}, {0, 1}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x1ba 声道停机");
        if (machine.config().get_int("sound:Music") != -3) return fail("0x1ba 关音乐没有 −3");
        if (!machine.step()) return fail("0x1ba 第二跳停机");
        if (machine.config().get_int("sound:Music") != -3) return fail("0x1ba 已在同侧却再减了 3");
        if (!machine.step()) return fail("0x1ba 第三跳停机");
        if (machine.config().get_int("sound:Music") != 0) return fail("0x1ba 开音乐没有 +3");
        if (!machine.step()) return fail("0x1ba 音效停机");
        if (!machine.sound_se_enabled()) return fail("0x1ba 同态请求把音效关掉了");
    }

    // ⑨ 0xb4 成功装载写「该槽当前资源号」这一格（`0x4b5004`）；0xb6 清**同一格**（`0x4b506a`）。
    //    ★ 旧版这里断言"0xb6 不清"并引了"规范"—— 规范里此前**没有** 0xb6 这一条（它在 spec 的未成篇 gap 里），
    //    而 0xb4 又按"后端存在"写了表 ⇒ 同一个 T_SE 上两条指令用了两套前提。现在两条都按「后端存在、后端受理」
    //    建模；槽号的两个界（后端 15 / 引擎表 10）与越界处置在 `check_audio_slot_bounds()` 里逐条咬。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xB4, {{0, 7}, {0, 9}});
        emit_instruction(bytes, 0xB6, {{0, 9}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0xb4 停机");
        if (machine.se_slot_resource(9) != 7) return fail("0xb4 没有写槽资源格");
        if (!machine.step()) return fail("0xb6 停机");
        if (machine.se_slot_resource(9) != 0) return fail("0xb6 没有清槽资源格（取证 0x4b506a）");
        if (machine.se_stop_requests() != 1) return fail("0xb6 没有发停止请求");
    }

    // ⑩ 0xb7 同曲连播两次 = **两次**播放请求（与 0xbf 的"同曲跳过"相反）；⑪ 0xb8 无"在播"判据
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xB7, {{0, 5}});
        emit_instruction(bytes, 0xB7, {{0, 5}});
        emit_instruction(bytes, 0xB8, {});
        emit_instruction(bytes, 0xB8, {});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        for (int i = 0; i < 4; ++i) {
            if (!machine.step()) return fail("0xb7/0xb8 停机");
        }
        if (machine.bgm_play_requests() != 2) return fail("0xb7 同曲连播没有发两次请求");
        if (machine.bgm_stop_requests() != 2) return fail("0xb8 连调两次没有发两次停止请求");
        if (machine.bgm_param() != 1) return fail("0xb8 改了参数格（规范：不改）");
        if (machine.bgm_track() != 0) return fail("0xb8 没有清曲目格");
    }

    // ⑮ 0x2f5：读序 4→1、五格落位（延时 / 资源 / pending / 起算 / 状态字）；越界只记日志不写
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2F5, {{0, 7}, {0, 3}, {0, 250}, {0, 2}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x2f5 停机");
        if (machine.voice_resource() != 7) return fail("0x2f5 没有把 op1 写成资源号");
        if (!machine.voice_pending()) return fail("0x2f5 没有置 pending");
        if (machine.voice_state(2) != 3) return fail("0x2f5 没有把 op2 写成该槽状态字");
        if (machine.voice_delay(2) != 250) return fail("0x2f5 没有把 op3 写成延时");
        if (machine.voice_start_ms() != 0) return fail("0x2f5 没有把起算清 0");
    }
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2F5, {{0, 0}, {0, 0}, {0, 0}, {0, 5}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x2f5 越界下标不该停机");
        if (machine.voice_pending()) return fail("0x2f5 越界下标仍写了槽（应当记日志后跳过）");
    }

    // ⑯ 0x2ff：**先写组开关位、才读 op2** ⇒ 组值落位；三个槽互不影响
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2FF, {{0, 1}, {0, 9}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x2ff 停机");
        if (machine.voice_group_enabled(1) != 1) return fail("0x2ff 没有置组开关位");
        if (machine.voice_group_value(1) != 9) return fail("0x2ff 没有写组值");
        if (machine.voice_group_enabled(0) != 0 || machine.voice_group_value(0) != 0) {
            return fail("0x2ff 写了别的槽");
        }
    }

    // ⑰ 0xc2：A 支（第 27 位）瞬间到位、**不动第 9 位**；B 支置第 9 位 + 间隔 / 步长两档
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xC2, {{0, 5000}, {0, 30}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0xc2（B 支）停机");
        if (!machine.bgm_fade_active()) return fail("0xc2 B 支没有置第 9 位（淡变进行中）");
        if (machine.bgm_fade_target() != 5000) return fail("0xc2 B 支没有写目标值 op1");
        if (machine.bgm_fade_interval_ms() != 3) return fail("0xc2 B 支：op2<1000 ⇒ 间隔 = op2/10");
        if (machine.bgm_fade_step() != 10) return fail("0xc2 B 支：op2<1000 ⇒ 步长 10");
    }
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xC2, {{0, 4000}, {0, 3500}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0xc2（B 支大数）停机");
        if (machine.bgm_fade_interval_ms() != 35) return fail("0xc2 B 支：op2>=1000 ⇒ 间隔 = op2/100");
        if (machine.bgm_fade_step() != 1) return fail("0xc2 B 支：op2>=1000 ⇒ 步长 1");
    }
    // ★ `0xc2` 的 A 支（门字第 27 位 = "立即到位"）**没法用合成脚本走到**：那一位同时是主循环要服务的
    //   "文本请求"位，本模型的门一旦看到**未服务**的位就只报诊断、不取指 ⇒ 在 A 支之前就被挡住。
    //   该支路按取证实现了（目标 / 步长 / 瞬间到位 / 不动第 9 位），但**没有行为级守卫** ⇒ 已登记为残差。

    // ⑱ 0xc4：正常支（无影片门）⇒ 1 次装载 + 1 次播放请求、镜像格清 0、文本记录追加一条
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xC4, {{0, 500}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0xc4 停机");
        if (machine.voice_play_requests() != 1) return fail("0xc4 没有发播放请求");
        if (machine.voice_load_requests() != 1) return fail("0xc4 没有发装载请求");
        if (machine.voice_mirror_resource(0) != 0) return fail("0xc4 正常支没有清镜像资源格");
        if (!machine.bgm_ducked()) return fail("0xc4 没有压低 BGM（门字第 14 位）");
        if (machine.text_span_count() != 1) return fail("0xc4 没有追加文本记录（文本门开着 ⇒ 必须追加）");
        if (machine.text_span_flags(0) != 0x40000000) return fail("0xc4 追加的记录 flags 不是 0x40000000");
        if (machine.text_span_resource(0) != 500) return fail("0xc4 追加的记录没有带 op1");
        if (machine.text_span_voice_slot(0) != 0) return fail("0xc4 追加的记录配音槽不是 0");
    }

    // ⑲ 0xc4：文本门被关（0x1bb 写 0 ⇒ 抑制）⇒ **不追加记录**
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x1BB, {{0, 0}});
        emit_instruction(bytes, 0xC4, {{0, 500}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x1bb 停机");
        if (!machine.step()) return fail("0xc4（文本门关）停机");
        if (machine.text_span_count() != 0) return fail("0xc4 在文本门关着时仍追加了记录");
        if (machine.voice_play_requests() != 1) return fail("0xc4 文本门关着时不该影响播放请求");
    }

    // ⑳ 0xc4 的**开关消费**：位0 ⇒ 置位16（位0 保留）；再来一次（位16 已置）⇒ 整格 0
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2FF, {{0, 0}, {0, 7}});
        emit_instruction(bytes, 0xC4, {{0, 500}});
        emit_instruction(bytes, 0xC4, {{0, 500}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x2ff 停机");
        if (!machine.step()) return fail("0xc4 第一次停机");
        if (machine.voice_group_enabled(0) != 0x10001) {
            return fail("0xc4 第一次开关消费：应当 位0 保留 + 置位16（= 0x10001）");
        }
        if (!machine.step()) return fail("0xc4 第二次停机");
        if (machine.voice_group_enabled(0) != 0) {
            std::fprintf(stderr, "config-ui: 0xc4 第二次开关消费后格 = 0x%X\n",
                         static_cast<unsigned>(machine.voice_group_enabled(0)));
            return fail("0xc4 第二次开关消费：位16 已置 ⇒ 整格 0");
        }
    }

    // ㉑ 0x82：记录向量为空 ⇒ **整条静默**（不写颜色、不发请求、不抛），且推进 11 格（下一条照常执行）
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x82, {{0, 0}, {0, 0}, {0, 2}, {0, 0x112233}, {0, 0x445566}});
        emit_instruction(bytes, 0xC4, {{0, 500}});  // 用一条**有可观察副作用**的指令验推进格数
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x82（空向量）不该停机");
        if (machine.font_primary_color() != 0xFFFFFF) return fail("0x82 在建界早退时仍写了主色");
        if (machine.font_secondary_color() != 0x000000) return fail("0x82 在建界早退时仍写了副色");
        if (!machine.errors().empty()) return fail("0x82 早退不该记错误");
        if (!machine.step()) return fail("0x82 之后的指令没有执行（推进格数不对）");
        if (machine.voice_play_requests() != 1) {
            return fail("0x82 之后的 0xc4 没有生效（0x82 的推进/长度不对）");
        }
    }

    // ㉒ 0x82：有记录时按 op3 位1 把两格颜色**借用**给这次绘制（字节反转），**返回前恢复**
    //    （引擎 `sub_466000` 写完颜色后逐字段把旧值写回：副色 `0x467371` / 主色 `0x4673e1`）；
    //    位3 + 记录 bit30 ⇒ 触发一次配音。
    //    ★ 这条钉的是"**借用而非改色**"：永久写会让后续 `0x204`（它的颜色来自这两格）用残留色画。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x76, {{0, 0x0A0B0C}});  // 主色先设成可辨认的值（存的是反转后的值）
        emit_instruction(bytes, 0x77, {{0, 0x0D0E0F}});  // 副色
        emit_instruction(bytes, 0xC4, {{0, 500}});       // 产出一条记录
        emit_instruction(bytes, 0x82, {{0, 0}, {0, 0}, {0, 2}, {0, 0x112233}, {0, 0x445566}});
        emit_instruction(bytes, 0x82, {{0, 0}, {0, 0}, {0, 8}, {0, 0}, {0, 0}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x76 停机");
        if (!machine.step()) return fail("0x77 停机");
        const auto primary_before = machine.font_primary_color();
        const auto secondary_before = machine.font_secondary_color();
        if (primary_before != 0x0C0B0A) return fail("0x76 没有按字节反转存主色（应 0x0C0B0A）");
        if (secondary_before != 0x0F0E0D) return fail("0x77 没有按字节反转存副色（应 0x0F0E0D）");
        if (!machine.step()) return fail("0xc4（产出记录）停机");
        if (!machine.step()) return fail("0x82（颜色支）停机");
        if (machine.font_primary_color() != primary_before ||
            machine.font_secondary_color() != secondary_before) {
            return fail("0x82 把颜色**永久**改了 —— 引擎是借用后归还（0x467371 副色 / 0x4673e1 主色），"
                        "本条返回时两格必须与进入时相同");
        }
        if (machine.voice_play_requests() != 1) return fail("0x82（op3=2）不该发配音请求");
        if (!machine.step()) return fail("0x82（配音支）停机");
        if (machine.voice_play_requests() != 2) return fail("0x82（op3=8）没有触发配音请求");
        if (machine.font_primary_color() != primary_before) return fail("0x82（op3=8）不该改颜色");
    }

    // ⑬ 0x198：槽下标 0 ⇒ **默认槽 1**；只写那一槽的两个偏移；不碰字号 / 行高 / 别的槽
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x197, {{0, 10}});
        emit_instruction(bytes, 0x198, {{0, 0}, {0, 100}, {0, 200}});
        emit_instruction(bytes, 0x198, {{0, 3}, {0, 7}, {0, 8}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x198 前置 0x197 停机");
        if (!machine.step()) return fail("0x198(0,100,200) 停机");
        // ★ 槽 0 不可达：`0` 是"默认槽"，写进去的是槽 1
        if (machine.text_slot_x(0) != 0 || machine.text_slot_y(0) != 0) {
            return fail("0x198 槽下标 0 写到了槽 0（规范：0 ⇒ 默认槽 1）");
        }
        if (machine.text_slot_x(1) != 100 || machine.text_slot_y(1) != 200) {
            return fail("0x198 没有把两个偏移写进默认槽 1");
        }
        if (!machine.step()) return fail("0x198(3,7,8) 停机");
        if (machine.text_slot_x(3) != 7 || machine.text_slot_y(3) != 8) return fail("0x198 没有写槽 3");
        if (machine.text_slot_x(1) != 100) return fail("0x198 写槽 3 时把槽 1 改了");
        if (machine.glyph_height() != 24) return fail("0x198 改了 A 支字号（规范：只写偏移；缺省 24）");
        if (machine.font_b_height() != 10) return fail("0x198 改了 B 支字号（规范：只写偏移；此前 0x197(10)）");
    }
    // ⑭ 0x198 越界下标：引擎里是未定义行为 ⇒ 本实现**夹紧后跳过**（不写任何槽、不停机）
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x198, {{0, 12}, {0, 5}, {0, 6}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x198 越界下标不该停机");
        for (std::size_t i = 0; i < core::Machine::text_slot_count(); ++i) {
            if (machine.text_slot_x(i) != 0 || machine.text_slot_y(i) != 0) {
                return fail("0x198 越界下标写了槽（应当夹紧后跳过）");
            }
        }
    }

    return true;
}

// ── 音效槽：引擎表（`T_SE+0x4BC` = 10）与后端槽数组（`T_Sound` = 15） ──
/** 实现的两个槽量与 **shape 声明**一致。
 *  ★ 为什么读 `data/shape/types.json` 而不是在这里再抄一遍数字：这两个数量曾经被写成一个 `15` ⇒
 *    "槽号 10..14"被当成合法的引擎格写进那张 10 格表（假造了 5 个引擎格）。
 *    取证在 `vm.h` 的两个常量旁（构造器逐格清零 / 相邻嵌入对象基址 / 后端三条受理边界）；
 *    这里只判"实现 == shape 声明"，⛔ 不解释语义。
 *  ★ 路径由 CMake 注入（`EMULATOR_CXX_SHAPE_TYPES_JSON`）：它**不拷贝**进构建目录 —— 它是守卫的输入，
 *    读不到就该红（拷贝会让人以为"文件在就算对"）。 */
bool check_audio_slot_table() {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "audio-slots: %s\n", what);
        return false;
    };
    const std::filesystem::path path(EMULATOR_CXX_SHAPE_TYPES_JSON);
    std::ifstream in{path};
    if (!in) {
        std::fprintf(stderr, "audio-slots: cannot open %s\n", path.string().c_str());
        return false;
    }
    nlohmann::json doc;
    in >> doc;
    const auto& types = doc.at("types");
    const auto member_count = [&types](const char* type, const char* off) -> long long {
        for (const auto& member : types.at(type).at("members")) {
            if (member.at("off") != off) continue;
            if (!member.contains("count") || !member.at("count").is_number_integer()) return -1;
            return member.at("count").get<long long>();
        }
        return -2;
    };
    const auto engine_table = member_count("T_SE", "0x4bc");
    if (engine_table < 0) return fail("shape 的 T_SE+0x4bc 不是条数有界的数组（count 缺失）");
    if (core::Machine::kSeSlotTableSize != engine_table) {
        return fail("实现的引擎槽表量 != shape 的 T_SE+0x4bc count");
    }
    // 后端的槽数组有 7 张同量（+0x51c / +0x564 / +0x5a0 / +0x5dc / +0x618 / +0x690 / +0x7f8）。
    for (const char* off : {"0x51c", "0x564", "0x5a0", "0x5dc", "0x618", "0x690", "0x7f8"}) {
        if (member_count("T_Sound", off) != core::Machine::kSoundSlotCount) {
            return fail("实现的后端槽量 != shape 的 T_Sound 槽数组 count");
        }
    }
    return true;
}

/** 音效三条的槽号边界：0..9 写引擎表；10..14 后端受理但引擎表外（本版只记诊断）；
 *  ≥15 后端拒绝（装载/停止都不动表、不记请求）。 */
bool check_audio_slot_bounds(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "audio-bounds: %s\n", what);
        return false;
    };
    const auto has_error = [](const core::Machine& machine, const std::string& prefix) {
        for (const auto& text : machine.errors()) {
            if (text.rfind(prefix, 0) == 0) return true;
        }
        return false;
    };
    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0xB4, {{0, 7}, {0, 9}});   // 0：表内
    emit_instruction(bytes, 0xB4, {{0, 8}, {0, 10}});  // 1：表外（后端受理）
    emit_instruction(bytes, 0xB4, {{0, 9}, {0, 15}});  // 2：后端拒绝
    emit_instruction(bytes, 0xB5, {{0, 14}});          // 3：后端受理的最后一格
    emit_instruction(bytes, 0xB5, {{0, 15}});          // 4：后端拒绝
    emit_instruction(bytes, 0xB6, {{0, 12}});          // 5：表外
    emit_instruction(bytes, 0xB6, {{0, 15}});          // 6：后端拒绝
    emit_instruction(bytes, 0xB6, {{0, 9}});           // 7：清表内那格
    emit_instruction(bytes, 2, {});
    core::Machine machine(0x12345678u, table);
    machine.load_root(bytes);
    if (!machine.step()) return fail("0xb4 槽 9 停机");
    if (machine.se_slot_resource(9) != 7) return fail("0xb4 槽 9 没有写表");
    if (machine.se_load_requests() != 1) return fail("0xb4 槽 9 没有记装载请求");
    if (!machine.step()) return fail("0xb4 槽 10 停机");
    if (machine.se_load_requests() != 2) return fail("0xb4 槽 10 该记装载请求（后端受理）");
    if (!has_error(machine, "se-load-slot-past-engine-table:10")) return fail("0xb4 槽 10 没有记表外诊断");
    if (machine.se_slot_resource(9) != 7) return fail("0xb4 槽 10 动了槽 9");
    if (!machine.step()) return fail("0xb4 槽 15 停机");
    if (machine.se_load_requests() != 2) return fail("0xb4 槽 15 不该记装载请求（后端拒绝）");
    if (!has_error(machine, "se-load-slot-out-of-range:15")) return fail("0xb4 槽 15 没有记越界诊断");
    if (!machine.step()) return fail("0xb5 槽 14 停机");
    if (machine.se_play_requests() != 1) return fail("0xb5 槽 14 没有记播放请求");
    if (!machine.step()) return fail("0xb5 槽 15 停机");
    if (machine.se_play_requests() != 1) return fail("0xb5 槽 15 不该记播放请求");
    if (!machine.step()) return fail("0xb6 槽 12 停机");
    if (machine.se_stop_requests() != 1) return fail("0xb6 槽 12 该记停止请求（后端受理）");
    if (!has_error(machine, "se-stop-slot-past-engine-table:12")) return fail("0xb6 槽 12 没有记表外诊断");
    if (!machine.step()) return fail("0xb6 槽 15 停机");
    if (machine.se_stop_requests() != 1) return fail("0xb6 槽 15 不该记停止请求");
    if (!has_error(machine, "se-stop-slot-out-of-range:15")) return fail("0xb6 槽 15 没有记越界诊断");
    if (!machine.step()) return fail("0xb6 槽 9 停机");
    if (machine.se_stop_requests() != 2) return fail("0xb6 槽 9 没有记停止请求");
    if (machine.se_slot_resource(9) != 0) return fail("0xb6 槽 9 没有清表");
    return true;
}

// ── 门字第 27 位的两条支路：`0xc2` 的"瞬间到位"与 `0xc4` 的"影片门" ──
/** 这两条支路**合成脚本走不到**：位 27 同时是主循环要服务的"影片 / 文本请求"位，而本模型的主循环
 *  只有第 0 / 10 / 13 位的服务分支 ⇒ 门会挡住、只报 `gate-bit-unserviced`。
 *  于是它们长期只有"按取证写了"的注释、没有行为级守卫（两条都登记成了残差）。
 *  ★ 这里用 `dispatch_for_test()`（门外直派）分别喂两条支路 —— ⛔ 不是放宽门，门本身的行为另有守卫
 *    （`gate-stalled` / `0x19c` 那条）。第 27 位由谁服务仍然未取证（仍是残差）。 */
bool check_gate_split_branches(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "gate-branches: %s\n", what);
        return false;
    };
    constexpr std::uint32_t kMovieGateBit = 1u << 27;

    // ① `0xc2` A 支（第 27 位 = 1）：目标 ← op1、步长 ← op2、**瞬间到位**（进度 = 目标）、
    //    **不动第 9 位**、不读混音配置、不写间隔。
    {
        core::Machine machine(0x12345678u, table);
        machine.state_flags_mut() |= kMovieGateBit;
        core::Instruction instruction{0, 0xC2, {{0, 7000}, {0, 250}}};
        machine.dispatch_for_test(instruction);
        if (machine.bgm_fade_target() != 7000) return fail("0xc2 A 支没有写目标值（op1）");
        if (machine.bgm_fade_step() != 250) return fail("0xc2 A 支没有写步长（op2）");
        if (machine.bgm_fade_progress() != 7000) return fail("0xc2 A 支没有瞬间到位（进度应 = 目标）");
        if (machine.bgm_fade_requests() != 1) return fail("0xc2 A 支没有记一次淡变请求");
        if (machine.bgm_fade_active()) return fail("0xc2 A 支动了第 9 位（规范：这一支不进淡变状态）");
        if (machine.bgm_fade_mix_mode() != 0) return fail("0xc2 A 支读了混音配置（规范：只在 B 支读）");
        if (machine.bgm_fade_interval_ms() != 0) return fail("0xc2 A 支写了间隔（规范：只在 B 支写）");
        // 门字本身不被这条改动（A 支对第 9 位"没有写动作"，也没清第 27 位）。
        if ((machine.state_flags() & kMovieGateBit) == 0) return fail("0xc2 A 支把第 27 位清掉了");
        if (machine.state_flags() != kMovieGateBit) return fail("0xc2 A 支改了门字的其它位");
    }

    // ③ `0xc2` B 支（第 27 位未置）：读配置定混音方式、置第 9 位、由 op2 算间隔 / 步长、目标 ← op1、进度清 0。
    {
        core::Machine fade(0x12345678u, table);
        fade.config_mut().set_int("set:TransferMusicVolume", 2);
        fade.dispatch_for_test(core::Instruction{0, 0xC2, {{0, 6400}, {0, 500}}});
        if (!fade.bgm_fade_active()) return fail("0xc2 B 支没有置第 9 位（不进淡变状态）");
        if (fade.bgm_fade_mix_mode() != 2) return fail("0xc2 B 支没有读混音配置 set:TransferMusicVolume");
        if (fade.bgm_fade_interval_ms() != 50) return fail("0xc2 B 支间隔算错（500 ⇒ 50ms）");
        if (fade.bgm_fade_step() != 10) return fail("0xc2 B 支步长算错（< 1000 ⇒ 10）");
        if (fade.bgm_fade_target() != 6400) return fail("0xc2 B 支没有把 op1 写进目标值");
        if (fade.bgm_fade_progress() != 0) return fail("0xc2 B 支没有把进度清 0");
        if (fade.bgm_fade_requests() != 1) return fail("0xc2 B 支没有记一次淡变请求");
        // 大值分支：≥ 1000 时按 /100、步长 = 1；边界 999 / 1000 各一次。
        fade.dispatch_for_test(core::Instruction{0, 0xC2, {{0, 1}, {0, 1500}}});
        if (fade.bgm_fade_interval_ms() != 15 || fade.bgm_fade_step() != 1) {
            return fail("0xc2 B 支大值分支算错（1500 ⇒ 15ms / 步长 1）");
        }
        fade.dispatch_for_test(core::Instruction{0, 0xC2, {{0, 1}, {0, 999}}});
        if (fade.bgm_fade_interval_ms() != 99 || fade.bgm_fade_step() != 10) {
            return fail("0xc2 B 支边界 999 算错（应为 99ms / 步长 10）");
        }
        fade.dispatch_for_test(core::Instruction{0, 0xC2, {{0, 1}, {0, 1000}}});
        if (fade.bgm_fade_interval_ms() != 10 || fade.bgm_fade_step() != 1) {
            return fail("0xc2 B 支边界 1000 算错（应为 10ms / 步长 1）");
        }
    }

    // ② `0xc4` 影片门支（第 27 位 = 1）：只把 op1 写进镜像格、**不发装载 / 播放请求**、
    //    但**仍然追加文本记录**、并且照样压低 BGM。
    {
        core::Machine machine(0x12345678u, table);
        machine.state_flags_mut() |= kMovieGateBit;
        core::Instruction instruction{0, 0xC4, {{0, 777}}};
        machine.dispatch_for_test(instruction);
        if (machine.voice_mirror_resource(0) != 777) return fail("0xc4 影片支没有把 op1 写进镜像资源格");
        if (machine.voice_mirror_a4(0) != 0) return fail("0xc4 影片支写的镜像第二格不是 0");
        if (machine.voice_play_requests() != 0) return fail("0xc4 影片支仍发了播放请求");
        if (machine.voice_load_requests() != 0) return fail("0xc4 影片支仍发了装载请求");
        if (!machine.bgm_ducked()) return fail("0xc4 影片支没有压低 BGM");
        if (machine.text_span_count() != 1) return fail("0xc4 影片支没有追加文本记录（规范：仍然追加）");
        if (machine.text_span_resource(0) != 777) return fail("0xc4 影片支的记录没有带 op1");
        if (machine.text_span_flags(0) != 0x40000000) return fail("0xc4 影片支的记录 flags 不是 0x40000000");
        if (machine.text_span_voice_slot(0) != 0) return fail("0xc4 影片支的记录配音槽不是 0");
    }

    return true;
}

// ── 两套字号：`0x75` = A 支（`0x204` 用）、`0x197` = B 支（记录绘制用） ──
/** 这条专门守"字号跟错格"这一类错：A 支缺省 24、B 支缺省 8、互不覆盖；`0x204` 用的必须是 A 支。
 *  ★ 历史：模型曾把 `0x75` 当"行高"、`0x197` 当"字形高度"，于是配置界面按 B 支的 10 画（帮助文字"极小"）。
 *  证据见 `vm.h` 里两个格的注释（EA 锚都在那里）。 */
bool check_font_branches(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "font-branches: %s\n", what);
        return false;
    };
    // ⑫ 0x75 / 0x197 是**两套字号**：A 支（0x75，缺省 24，`0x204` 出字形用它）/ B 支（0x197，缺省 8，
    //    跨度绘制路径才用），互不覆盖；0x197 还把十个文本槽一起写。
    //    ★ 历史教训：以前把 0x75 当"行高"、0x197 当"字形高度" ⇒ 配置界面里 `0x75(20)` 后 `0x197(10)`，
    //      0x204 就按 10 画（帮助文字"极小"）。取证见 `vm.h` 两个格的注释（EA 都在那里）。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 0x197, {{0, 10}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (machine.glyph_height() != 24) return fail("A 支字号缺省应当是 24（引擎初值 0x18）");
        if (machine.font_b_height() != 8) return fail("B 支字号缺省应当是 8");
        if (!machine.step()) return fail("0x75 停机");
        if (machine.glyph_height() != 20) return fail("0x75 没有写 A 支字号（0x204 用的就是它）");
        if (machine.font_b_height() != 8) return fail("0x75 顺带改了 B 支字号（两支应当互不覆盖）");
        if (!machine.step()) return fail("0x197 停机");
        if (machine.font_b_height() != 10) return fail("0x197 没有写 B 支字号");
        for (std::size_t i = 0; i < core::Machine::text_slot_count(); ++i) {
            if (machine.text_slot_glyph_height(i) != 10) return fail("0x197 没有把十个文本槽一起写");
        }
        if (machine.glyph_height() != 20) {
            return fail("0x197 把 0x204 用的 A 支字号改掉了（配置界面的帮助文字会因此变小）");
        }
    }
    return true;
}

// ── 渲染原语：数值文本 `0x205` 与清屏 `0x20e` ──
/** 判据是**整张纹理逐字节相等**：0x205 的输出必须与"把期望串用 0x204 画在同一位置"完全相同
 *  （像素 + 宽度 + 高度），因此格式化结果、横向落点与纵向对位**一起**被钉住。
 *  正向控制：参照串必须真的出墨；字体缺失 / 后端不可用时**当场红**（⛔ 不把"两边都失败"当成通过）。
 *  `0x20e`：清的是**当前渲染目标**（`0x20d` 写）。目标是纹理槽 ⇒ 该纹理表面变透明黑且内容版本 +1；
 *  非目标纹理不动；目标是后缓冲（-1）⇒ 模型里没有可写表面（宿主限制，见 SCOPE）。
 *  门控支（虚拟全屏类型 = 1 → 槽 38）**未建模**（模型没有 VFS 格）。 */
bool check_render_prims(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "render-prims: %s\n", what);
        return false;
    };
    struct Run {
        std::size_t errors = 0;
        core::StopReason reason = core::StopReason::Halted;
        std::map<std::uint32_t, core::Bytes> pixels;
        std::map<std::uint32_t, std::uint32_t> revisions;
        bool dirty = false;
        std::int32_t target = -1;
        std::size_t count = 0;
    };
    const auto run_script = [&](std::vector<std::uint8_t> script) {
        Run run;
        core::Machine machine(key, table);
        machine.load_root(script);
        const auto result = machine.run(32);
        run.errors = machine.errors().size();
        run.reason = result.reason;
        run.dirty = machine.scene_dirty();
        run.target = machine.render_target();
        run.count = machine.texture_count();
        for (const auto& entry : machine.textures()) {
            run.pixels[entry.first] = entry.second.rgba;
            run.revisions[entry.first] = entry.second.revision;
        }
        return run;
    };
    const auto bytes_of = [](const Run& run, std::uint32_t id) {
        const auto it = run.pixels.find(id);
        return it == run.pixels.end() ? core::Bytes{} : it->second;
    };
    const auto has_ink = [](const core::Bytes& pixels) {
        for (std::size_t i = 3; i < pixels.size(); i += 4) {
            if (pixels[i] != 0) return true;
        }
        return false;
    };
    const auto same = [](const Run& a, const Run& b) {
        return a.errors == b.errors && a.pixels == b.pixels;
    };
    // 0x205 的操作数：op1 纹理槽 / op2 字段原点 / op3 y / op4 值 / op5 格数 / op6 格式位。
    const auto draw_205 = [&](std::uint32_t cell, std::uint32_t slot, std::uint32_t anchor_x,
                              std::uint32_t value, std::uint32_t cells, std::uint32_t flags) {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1F8, {{0, 1}, {0, 160}, {0, 64}, {0, 0}});
        emit_instruction(script, 0x75, {{0, cell}});
        emit_instruction(script, 0x205,
                         {{0, slot}, {0, anchor_x}, {0, 10}, {0, value}, {0, cells}, {0, flags}});
        emit_instruction(script, 2, {});
        return run_script(script);
    };
    // 参照实现：把期望串用 `0x204` 画在同一张画布的 (x, 10)。
    const auto draw_text = [&](std::uint32_t cell, std::uint32_t x, const std::string& text) {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1F8, {{0, 1}, {0, 160}, {0, 64}, {0, 0}});
        emit_instruction(script, 0x75, {{0, cell}});
        emit_text_204(script, 1, x, 10, text);
        emit_instruction(script, 2, {});
        return run_script(script);
    };

    if (core::text_backend_available()) {
        // ⓪ 正向控制：参照串必须真出墨。字体缺失时 0x205 与 0x204 会"同样失败" ⇒ 不加这条就会静默绿。
        const Run control = draw_text(20, 20, "45");
        if (control.errors != 0 || !has_ink(bytes_of(control, 1))) {
            return fail("文本后端没出墨（字体缺失？）⇒ 本守卫无法判定，⛔ 不把两边都失败当成通过");
        }
        const auto expect = [&](const Run& actual, const Run& expected, const char* what) {
            if (!same(actual, expected)) {
                std::fprintf(stderr, "render-prims: %s\n", what);
                return false;
            }
            return true;
        };
        // ① 无格数且无符号位 ⇒ 空串：不画、不报错（⛔ 不是把 0 画出来）
        const Run blank = draw_205(20, 1, 0, 1234, 0, 0);
        if (has_ink(bytes_of(blank, 1)) || blank.errors != 0 || blank.reason != core::StopReason::Halted) {
            return fail("0x205 无格数且无符号位时不该画（cells=0 是空串，不是 '0'）");
        }
        // ② 放不下 ⇒ 只画低位数字（⛔ 不是 '#'）
        if (!expect(draw_205(20, 1, 0, 12345, 2, 0x10000), draw_text(20, 0, "45"),
                    "0x205 放不下时应当只画低位数字（2 格 12345 ⇒ \"45\"）")) return false;
        // ③ 三个对齐档：右齐（缺省）/ 居中（0x2）/ 左齐（0x4）；4 格 7 ⇒ 未用 3 个半角格
        if (!expect(draw_205(20, 1, 0, 7, 4, 0x10000), draw_text(20, 30, "7"),
                    "0x205 右齐：4 格 1 位应落在最右")) return false;
        if (!expect(draw_205(20, 1, 0, 7, 4, 0x10000 | 0x2), draw_text(20, 15, "7"),
                    "0x205 居中：0x2 应把 1 位放到 ⌊3×20÷4⌋ = 15")) return false;
        if (!expect(draw_205(20, 1, 0, 7, 4, 0x10000 | 0x4), draw_text(20, 0, "7"),
                    "0x205 左齐：0x4 应把 1 位放到 0")) return false;
        if (!expect(draw_205(20, 1, 0, 7, 4, 0x10000 | 0x2 | 0x4), draw_text(20, 15, "7"),
                    "0x205 0x2 与 0x4 同时置位时应按居中（0x2 优先）")) return false;
        // ④ 右齐 + 两位：3 格 99 ⇒ 未用 1 格（x = 10）
        if (!expect(draw_205(20, 1, 0, 99, 3, 0x10000), draw_text(20, 10, "99"),
                    "0x205 右齐没有按未用格数左移（3 格 99 应从第 2 格起）")) return false;
        // ⑤ `#` 的唯一入口：有符号且格数 < 2（0 与 1 都试；含格数为 0 的 clamp）
        if (!expect(draw_205(20, 1, 0, 7, 1, 0x10000 | 0x8), draw_text(20, 0, "#"),
                    "0x205 有符号且格数 = 1 时应画单个 '#'")) return false;
        if (!expect(draw_205(20, 1, 0, 7, 0, 0x10000 | 0x8), draw_text(20, 0, "#"),
                    "0x205 有符号且格数 = 0 时应画单个 '#'（第一格序号夹到 0）")) return false;
        const Run negative_cells = draw_205(20, 1, 0, 7, static_cast<std::uint32_t>(-1), 0x10000);
        if (has_ink(bytes_of(negative_cells, 1)) || negative_cells.errors != 0) {
            return fail("0x205 无符号且格数为负时应是空串（不画、不报错）");
        }
        // ⑥ 前导零：补满可用格（3 格 7 ⇒ "007"）；不补 ⇒ "7" 落在右端
        if (!expect(draw_205(20, 1, 0, 7, 3, 0x10000 | 0x1), draw_text(20, 0, "007"),
                    "0x205 的 0x1（补前导 0）没有补满字段")) return false;
        // ⑦ 符号：0x8 只对正数、0x10/0x20 只对零（0x10 优先）、负值一律 '-'
        if (!expect(draw_205(20, 1, 0, 5, 3, 0x10000 | 0x8), draw_text(20, 10, "+5"),
                    "0x205 的 0x8（正数带正号）没有画出 '+'")) return false;
        if (!expect(draw_205(20, 1, 0, static_cast<std::uint32_t>(-5), 3, 0x10000 | 0x8),
                    draw_text(20, 10, "-5"), "0x205 负值即使置 0x8 也应当是 '-'")) return false;
        if (!expect(draw_205(20, 1, 0, 0, 2, 0x10000 | 0x10), draw_text(20, 0, "+0"),
                    "0x205 的 0x10（零带正号）没有画出 '+'")) return false;
        if (!expect(draw_205(20, 1, 0, 0, 2, 0x10000 | 0x20), draw_text(20, 0, "-0"),
                    "0x205 的 0x20（零带负号）没有画出 '-'")) return false;
        if (!expect(draw_205(20, 1, 0, 0, 2, 0x10000 | 0x10 | 0x20), draw_text(20, 0, "+0"),
                    "0x205 的 0x10 与 0x20 同时置位时应是 '+'（0x10 优先）")) return false;
        if (!expect(draw_205(20, 1, 0, 5, 3, 0x10000 | 0x10), draw_text(20, 20, "5"),
                    "0x205 的 0x10 不应对非零值生效")) return false;
        if (!expect(draw_205(20, 1, 0, 5, 3, 0x10000 | 0x20), draw_text(20, 20, "5"),
                    "0x205 的 0x20 不应对非零值生效")) return false;
        // ⑧ 负值：符号占一格、数字按可用格截断；有符号也能补零
        if (!expect(draw_205(20, 1, 0, static_cast<std::uint32_t>(-1234), 3, 0x10000),
                    draw_text(20, 0, "-34"), "0x205 负值的格式 / 截断不对（3 格 −1234 ⇒ \"-34\"）")) return false;
        if (!expect(draw_205(20, 1, 0, static_cast<std::uint32_t>(-5), 3, 0x10000 | 0x1),
                    draw_text(20, 0, "-05"), "0x205 有符号 + 0x1 应当补成 \"-05\"")) return false;
        // ⑨ 全角 / 半角：缺省全角（数字与符号都有固定映射），0x10000 才是 ASCII
        if (!expect(draw_205(20, 1, 0, 77, 4, 0), draw_text(20, 40, "\x82\x56\x82\x56"),
                    "0x205 缺省应当扩成全角 S-JIS（4 格 77 ⇒「７７」从第 3 格起）")) return false;
        if (!expect(draw_205(20, 1, 0, 0, 2, 0x10), draw_text(20, 0, "\x81\x7B\x82\x4F"),
                    "0x205 全角的正号 / 数字映射不对（应 0x817B + 0x824F）")) return false;
        if (!expect(draw_205(20, 1, 0, 7, 1, 0x8), draw_text(20, 0, "\x81\x94"),
                    "0x205 全角的 '#' 应映射到 0x8194")) return false;
        // ⑩ 奇数格宽：引擎先乘后除（先除半格会差 1px）
        if (!expect(draw_205(15, 1, 0, 77, 4, 0x10000), draw_text(15, 15, "77"),
                    "0x205 奇数格宽右齐：应 ⌊2×15÷2⌋ = 15（不是 14）")) return false;
        if (!expect(draw_205(15, 1, 0, 7, 4, 0x10000 | 0x2), draw_text(15, 11, "7"),
                    "0x205 奇数格宽居中：应 ⌊3×15÷4⌋ = 11（不是 10）")) return false;
        // ⑪ 槽里没有可用表面 ⇒ 静默跳过（三条入口条件之一）
        const Run missing = draw_205(20, 99, 0, 1234, 4, 0);
        if (missing.errors != 0 || missing.reason != core::StopReason::Halted) {
            return fail("0x205 槽不存在时应当静默跳过（不报错）");
        }
        // ⑫ 不置脏 / 不改当前渲染目标 / 不改纹理槽表（与参照逐项比）
        const Run normal = draw_205(20, 1, 0, 99, 3, 0x10000);
        const Run reference = draw_text(20, 10, "99");
        if (normal.dirty != reference.dirty || normal.target != reference.target ||
            normal.count != reference.count) {
            return fail("0x205 改了参照不会改的状态（脏标记 / 当前渲染目标 / 纹理槽表）");
        }
    }

    // ⑬ 0x20e：清**当前渲染目标**。目标是纹理槽 ⇒ 该表面变透明黑、内容版本 +1；非目标纹理不动；
    //    目标是后缓冲（-1）⇒ 模型没有可写表面（宿主限制）。
    {
        const auto fill_one = [&](std::vector<std::uint8_t>& script, std::uint32_t id) {
            emit_instruction(script, 0x1F8, {{0, id}, {0, 32}, {0, 16}, {0, 0}});
            emit_instruction(script, 0x20B,
                             {{0, id}, {0, 0}, {0, 0}, {0, 32}, {0, 16}, {0, 255}, {0, 0x00FF00FFu}});
        };
        auto single = make_script_with_body();
        fill_one(single, 1);
        auto filled = single;
        emit_instruction(filled, 2, {});
        const Run before = run_script(filled);
        if (!has_ink(bytes_of(before, 1))) return fail("0x20E 夹具的前置填充没落地");

        auto cleared = single;
        emit_instruction(cleared, 0x20D, {{0, 1}});
        emit_instruction(cleared, 0x20E, {});
        emit_instruction(cleared, 2, {});
        const Run after = run_script(cleared);
        if (after.errors != 0) return fail("0x20E 报错");
        if (has_ink(bytes_of(after, 1))) return fail("0x20E 没有把当前渲染目标（纹理 1）清成透明黑");
        if (after.target != 1) return fail("0x20E 改变了当前渲染目标");
        if (after.count != before.count) return fail("0x20E 动了纹理槽表");
        const auto revision = after.revisions.find(1);
        if (revision == after.revisions.end() || revision->second <= before.revisions.at(1)) {
            return fail("0x20E 清了纹理却没有换内容版本（宿主不会重传）");
        }

        // 非目标纹理不动：两张都填色、目标是 1
        auto two = make_script_with_body();
        fill_one(two, 1);
        fill_one(two, 2);
        auto two_before = two;
        emit_instruction(two_before, 2, {});
        const Run kept_before = run_script(two_before);
        auto two_after = two;
        emit_instruction(two_after, 0x20D, {{0, 1}});
        emit_instruction(two_after, 0x20E, {});
        emit_instruction(two_after, 2, {});
        const Run kept = run_script(two_after);
        if (bytes_of(kept, 2) != bytes_of(kept_before, 2)) {
            return fail("0x20E 清了非目标纹理（⛔ 不是清纹理表）");
        }
        if (has_ink(bytes_of(kept, 1))) return fail("0x20E 没清目标纹理（双纹理夹具）");

        // 目标是后缓冲（缺省 -1）⇒ 纹理原样、不报错、目标还是 -1
        auto buffered = single;
        emit_instruction(buffered, 0x20E, {});
        emit_instruction(buffered, 2, {});
        const Run untouched = run_script(buffered);
        if (untouched.errors != 0) return fail("0x20E（后缓冲目标）报了错");
        if (bytes_of(untouched, 1) != bytes_of(before, 1)) {
            return fail("0x20E 在后缓冲目标下改了纹理");
        }
        if (untouched.target != -1) return fail("0x20E 把当前渲染目标从后缓冲改掉了");
    }
    return true;
}

// ── 字形 AA 档：格式选择 + 灰度级换算（引擎是 17 / 65 级，⛔ 不是 2^n） ──
/** 这条守"文字亮度"的**算术**那一半。
 *  · 格式只看三个配置格：总开关 `set:EnableAntiFont` 与子开关 `message:UseAntiFont` 都非 0 才开；
 *    级数 `message:AntiFontLevel` == 1 ⇒ 8 位，其余 ⇒ 4 位。任一为 0 ⇒ 1 位掩码。
 *  · 灰度级换算按引擎 `0x46DAAF` / `0x46DA9C`：`值 × 255 / 17`、`值 × 255 / 65`
 *    ⇒ **满覆盖 alpha = 240 / 251**，不是 255。
 *  ★ 历史：宿主回退（FreeType）曾经完全无视这三个格 ⇒ 满覆盖 255 ⇒ 设置界面文字比真机**亮一档**
 *    （与"配置根读成漫游、AA 开关根本没打开"是两件事，两条都要对）。 */
bool check_glyph_alpha() {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "glyph-alpha: %s\n", what);
        return false;
    };
    // ① 格式选择
    if (core::glyph_format(true, true, 0) != core::GlyphFormat::Gray4) {
        return fail("开关都开、级数 0 ⇒ 应当是 GRAY4（4 位）");
    }
    if (core::glyph_format(true, true, 1) != core::GlyphFormat::Gray8) {
        return fail("级数 1 ⇒ 应当是 GRAY8（8 位）");
    }
    if (core::glyph_format(true, true, 2) != core::GlyphFormat::Gray4) {
        return fail("级数只有 == 1 才是 8 位");
    }
    if (core::glyph_format(false, true, 0) != core::GlyphFormat::Mono ||
        core::glyph_format(true, false, 0) != core::GlyphFormat::Mono) {
        return fail("任一开关为 0 ⇒ 应当是 1 位掩码（⛔ 不是灰度）");
    }

    // ② 原始级数 → alpha（引擎同式）：17 级满覆盖 240、65 级满覆盖 251
    if (core::level_to_alpha(0, core::kGray4Levels) != 0) return fail("级数 0 应当是透明");
    if (core::level_to_alpha(16, core::kGray4Levels) != 240) {
        return fail("GRAY4 满覆盖应当是 240（值 × 255 / 17）；得 255 说明按 16 级算了");
    }
    if (core::level_to_alpha(64, core::kGray8Levels) != 251) {
        return fail("GRAY8 满覆盖应当是 251（值 × 255 / 65）；得 255 说明按 64 级算了");
    }

    // ③ 连续覆盖率 → 量化后 alpha：GRAY4 只能是 15 的倍数、上限 240；GRAY8 上限 251
    int previous = -1;
    for (int coverage = 0; coverage <= 255; ++coverage) {
        const int gray4 = core::coverage_to_alpha(coverage, core::kGray4Levels);
        if (gray4 > 240) return fail("GRAY4 量化后的 alpha 超过 240（满覆盖会比真机亮）");
        if (gray4 % 15 != 0) return fail("GRAY4 量化后的 alpha 不是 15 的倍数（级数写错了）");
        if (gray4 < previous) return fail("量化不是单调的");
        previous = gray4;
        const int gray8 = core::coverage_to_alpha(coverage, core::kGray8Levels);
        if (gray8 > 251) return fail("GRAY8 量化后的 alpha 超过 251");
    }
    if (core::coverage_to_alpha(255, core::kGray4Levels) != 240) {
        return fail("满覆盖率在 GRAY4 下应当是 240（真机偏暗的来源）");
    }
    return true;
}

// ── 写入三条支路（`CORE-OPS-RENDER-TEXT-204` 的合同；纯算术，不需要文本后端） ──
/** ① 覆盖值 = 0 ⇒ 一个字节都不写；② 通道值 = 通道满值 **或** 目标自身通道值 = 0 ⇒ 整像素覆写源色；
 *  ③ 其余 ⇒ 逐通道加权 `(源×值 + 目标×(满−值)) ÷ 满`、输出通道值 = max(目标, 覆盖值)。
 *  ★ 钉住的是"**哪一支**"：把 ② 写成"与黑色加权"（旧实现）会让空白画布上的墨迹颜色被压成 α 倍；
 *    把 ③ 写成覆写会让叠加失去混合。 */
bool check_glyph_compose() {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "compose: %s\n", what);
        return false;
    };
    // ① 覆盖值 0 ⇒ 不动（连 alpha 都不写）。
    {
        std::uint8_t texel[4] = {10, 20, 30, 40};
        core::compose_glyph_texel(texel, 200, 100, 50, 0);
        if (texel[0] != 10 || texel[1] != 20 || texel[2] != 30 || texel[3] != 40) {
            return fail("覆盖值 0 写了像素（应当是「一个字节都不写」）");
        }
    }
    // ② 目标通道值 0 ⇒ 整像素覆写：颜色是**源色原值**、通道值 = 覆盖值（不是 α 倍）。
    {
        std::uint8_t texel[4] = {0, 0, 0, 0};
        core::compose_glyph_texel(texel, 255, 128, 0, 240);
        if (texel[0] != 255 || texel[1] != 128 || texel[2] != 0 || texel[3] != 240) {
            std::fprintf(stderr, "compose: 得到 %u,%u,%u,%u\n", texel[0], texel[1], texel[2], texel[3]);
            return fail("目标通道值 = 0 时没有整像素覆写源色（把 α 乘进颜色了）");
        }
    }
    // ② 通道值 = 通道满值（255）⇒ 同样是覆写，且幂等。
    {
        std::uint8_t texel[4] = {7, 8, 9, 11};
        core::compose_glyph_texel(texel, 200, 100, 50, 255);
        if (texel[0] != 200 || texel[1] != 100 || texel[2] != 50 || texel[3] != 255) {
            return fail("通道值 = 255 时没有整像素覆写");
        }
        core::compose_glyph_texel(texel, 200, 100, 50, 255);
        if (texel[0] != 200 || texel[3] != 255) return fail("满覆盖重复写不是幂等");
    }
    // ③ 其余 ⇒ 加权；输出通道值 = max(目标, 覆盖值)。
    {
        std::uint8_t texel[4] = {255, 0, 0, 240};  // 目标：不透明红
        core::compose_glyph_texel(texel, 0, 255, 0, 15);
        const int expected_r = (0 * 15 + 255 * (255 - 15)) / 255;  // 240
        const int expected_g = (255 * 15 + 0 * (255 - 15)) / 255;  // 15
        if (texel[0] != expected_r || texel[1] != expected_g || texel[2] != 0) {
            std::fprintf(stderr, "compose: 加权结果 %u,%u,%u（期望 %d,%d,0）\n", texel[0], texel[1],
                         texel[2], expected_r, expected_g);
            return fail("逐通道加权不是 (源×值 + 目标×(满−值)) ÷ 满");
        }
        if (texel[3] != 240) return fail("输出通道值不是 max(目标, 覆盖值)");
    }
    return true;
}

// ── 抽干：宿主报了"有输入在按下" ⇒ 剩余待画格在同一帧内画完 ──
/** 引擎 `0x4096e0`–`0x4096f4` 的 do-while：有按键 / 跳过时不再"每帧一格"，而是把队列抽干。
 *  我们的信号是宿主每帧喂的 `set_input_pressed()`（主循环**被动观察**输入，与脚本主动取键
 *  `0xff` / `0x109` 无关）。
 *  ★ 判据：同一段 3 字文本 —— ① 无输入时走一帧只多一个字；② 报"有输入"之后**一帧**就把剩下两个字
 *  全画完（待画格归 0 且墨迹面积 ≥ 3 个字的量级）。 */
bool check_text_drain(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "drain: %s\n", what);
        return false;
    };
    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x80, {{0, 9}});
    emit_instruction(bytes, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 5}, {0, 5}});
    emit_instruction(bytes, 0x79, {{0, 9}, {0, 5}, {0, 5}});
    emit_instruction(bytes, 0x213, {{0, 9}, {0, 190500}, {0, 40}});
    emit_instruction(bytes, 0x75, {{0, 20}});
    emit_instruction(bytes, 0x76, {{0, 0xFFFFFF}});
    emit_instruction(bytes, 0x78, {{0, 0}});
    emit_instruction(bytes, 0x74, {{0, 5}});  // 每字间隔 5ms（自检 ConfigStore 没有发行默认表 ⇒ 不设就是 0）
    emit_instruction(bytes, 0x71, {{0, 9}});
    emit_instruction(bytes, 0x6E, {{0, 0}, {5, 0}});  // "ABC"
    emit_instruction(bytes, 0x75, {{0, 20}});
    emit_instruction(bytes, 0x75, {{0, 20}});
    emit_instruction(bytes, 0x75, {{0, 20}});
    emit_instruction(bytes, 2, {});
    core::Machine machine(key, table);
    machine.write_text(core::Operand{5, 0}, "ABC");
    machine.load_root(bytes);
    for (int i = 0; i < 9; ++i) {
        if (!machine.step()) return fail("0x71 之前就停机");
    }
    int guard_steps = 0;
    while (machine.pending_glyph_count() == 0 && guard_steps < 200) {
        if (!machine.step()) return fail("没走到 0x6e 就停机");
        ++guard_steps;
    }
    if (machine.pending_glyph_count() != 3) return fail("追加之后待画格数不是 3");
    // ① 无输入：一帧只画一个。
    if (!machine.step()) return fail("无输入那一帧停机");
    if (machine.pending_glyph_count() != 2) return fail("无输入时一帧画了不止一个（抽干被无条件打开了）");
    // ② 有输入：一帧抽干。
    machine.set_input_pressed(true);
    if (!machine.step()) return fail("抽干那一帧停机");
    if (machine.pending_glyph_count() != 0) {
        return fail("报了有输入却没有抽干（剩余格仍非 0）");
    }
    if (machine.text_typing()) return fail("抽干之后第 29 位没有清（引擎 `0x40953c`）");
    machine.set_input_pressed(false);
    return true;
}

// ── 打字节流：按**每字间隔毫秒**推进，每帧最多一格（未到期的帧一格都不画） ──
/** 引擎 `sub_409400` / `sub_453b60`：`elapsed <= n × interval` ⇒ **整个函数返回**（本帧一格都不画），
 *  超过才推进一格并把 n 推到 ⌊elapsed/interval⌋+1。启动 = `sub_453a60`（间隔 = `Font+0x560`）。
 *  ★ 这条区分两套模型：旧实现"每帧一格"在间隔 100ms 时也会每帧画一个字；引擎是**按毫秒**的。
 *  ★ 间隔 = 0 ⇒ 追加时直接走"立即抽干"（不置第 29 位、队列当步清空）。 */
bool check_text_pacing(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "pacing: %s\n", what);
        return false;
    };
    const auto build = [&](std::int32_t speed) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x80, {{0, 9}});
        emit_instruction(bytes, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x79, {{0, 9}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x213, {{0, 9}, {0, 190600}, {0, 40}});
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 0x76, {{0, 0xFFFFFF}});
        emit_instruction(bytes, 0x78, {{0, 0}});
        emit_instruction(bytes, 0x74, {{0, static_cast<std::uint32_t>(speed)}});
        emit_instruction(bytes, 0x71, {{0, 9}});
        emit_instruction(bytes, 0x6E, {{0, 0}, {5, 0}});  // "ABC"
        // 追加之后给足"帧"：`0x1a8` 是无副作用的空转指令（每步一条）。
        for (int i = 0; i < 80; ++i) emit_instruction(bytes, 0x1A8, {});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    const auto ink = [](core::Machine& machine) {
        const core::TextureRecord* texture = machine.texture(core::Machine::text_layer_texture_id(9));
        if (texture == nullptr) return std::size_t{0};
        std::size_t count = 0;
        for (std::size_t i = 3; i < texture->rgba.size(); i += 4) {
            if (texture->rgba[i] != 0) ++count;
        }
        return count;
    };
    // ① 间隔 100ms：追加后进打字态；前 5 帧（80ms）一格都不画；跨过 100ms 才画一格。
    {
        core::Machine machine(key, table);
        machine.write_text(core::Operand{5, 0}, "ABC");
        machine.load_root(build(100));
        for (int i = 0; i < 10; ++i) {
            if (!machine.step()) return fail("走到 0x6e 之前停机");
        }
        if (machine.pending_glyph_count() != 3) return fail("追加之后待画格数不是 3");
        if (!machine.text_typing()) return fail("间隔 > 0 时追加后应当置第 29 位（打字中）");
        if (machine.text_timer_interval_ms() != 100) return fail("计时器间隔不是 MessageSpeed");
        for (int i = 0; i < 5; ++i) {
            if (!machine.step()) return fail("节流期停机");
            if (machine.pending_glyph_count() != 3) {
                return fail("未到 100ms 的帧画了字（把「每 D 毫秒一格」实现成「每帧一格」）");
            }
        }
        int drew = -1;
        for (int i = 0; i < 5 && drew < 0; ++i) {
            if (!machine.step()) return fail("跨过间隔时停机");
            if (machine.pending_glyph_count() == 2) drew = i;
        }
        if (drew < 0) return fail("跨过 100ms 仍一格都没画（计时器没有到期分支）");
        if (ink(machine) == 0) return fail("到期那一帧没有真的落字");
        int guard = 0;
        while (machine.text_typing() && guard < 200) {
            if (!machine.step()) break;
            ++guard;
        }
        if (machine.text_typing()) return fail("末格画完之后第 29 位没有自清");
        if (machine.pending_glyph_count() != 0) return fail("清掉第 29 位时还有待画格");
    }
    // ② 间隔 = 0：追加当步走"立即抽干"——不置第 29 位、队列为空、字已经全画出来。
    {
        core::Machine machine(key, table);
        machine.write_text(core::Operand{5, 0}, "ABC");
        machine.load_root(build(0));
        for (int i = 0; i < 10; ++i) {
            if (!machine.step()) return fail("间隔 = 0 时在 0x6e 之前停机");
        }
        if (machine.text_typing()) return fail("间隔 = 0 时不该置第 29 位（应当立即抽干）");
        if (machine.pending_glyph_count() != 0) return fail("间隔 = 0 时没有同帧抽干");
        if (ink(machine) == 0) return fail("间隔 = 0 时字没有画出来");
    }
    return true;
}

// ── 文本记录标志位 2：**B 支字号**的置位者与消费口径 ──
/** 引擎：位 2 不是共用追加器置的，而是**两个调用方**在 flags 实参上置起（振り仮名 run / 层模式 = 1 的逐字支）；
 *  消费者按它选 B 支（`0x197`）或 A 支（`0x75`）。这条守三件：
 *   ① 主线正文 run 的记录**位 2 = 0**；② 振り仮名 run 追加的记录**位 2 置起**；
 *   ③ 字号支只有一处口径（`record_font_size`）：位 2 ⇒ B 支高度，否则 A 支；待画格也按它取字号。
 *  另守"文本目标 id 的位 31 置起 ⇒ 一条记录都不追加"（`0x1bb` 写 0x80000000）。 */
bool check_text_record_flags(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "record-flags: %s\n", what);
        return false;
    };
    const auto build = [&](bool suppress_gate) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x80, {{0, 9}});
        emit_instruction(bytes, 0x70, {{0, 9}, {0, 400}, {0, 120}, {0, 108}, {0, 52}});
        emit_instruction(bytes, 0x79, {{0, 9}, {0, 108}, {0, 52}});
        emit_instruction(bytes, 0x213, {{0, 9}, {0, 180700}, {0, 40}});
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 0x197, {{0, 10}});
        emit_instruction(bytes, 0x76, {{0, 0xFFFFFF}});
        if (suppress_gate) emit_instruction(bytes, 0x1BB, {{0, 0}});  // 0x80000000 ⇒ 抑制整批记录
        emit_instruction(bytes, 0x74, {{0, 100}});  // 打字中：待画格留在队列里
        emit_instruction(bytes, 0x71, {{0, 9}});
        emit_instruction(bytes, 0x196, {{0, 0}, {5, 0}, {5, 1}});
        for (int i = 0; i < 20; ++i) emit_instruction(bytes, 0x1A8, {});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    // ① 主文本 + 振り仮名：两条记录，位 2 只在振り仮名那条上。
    {
        auto bytes = build(false);
        core::Machine machine(key, table);
        machine.write_text(core::Operand{5, 0}, "AB");
        machine.write_text(core::Operand{5, 1}, "\x82\xA9\x82\xAB");  // かき
        machine.load_root(bytes);
        for (int i = 0; i < 40 && machine.pending_glyph_count() == 0; ++i) {
            if (!machine.step()) return fail("走到 0x196 之前停机");
        }
        if (machine.text_span_count() != 2) {
            std::fprintf(stderr, "record-flags: count=%zu\n", machine.text_span_count());
            return fail("主文本 + 振り仮名应当各追加一条文本记录");
        }
        if ((machine.text_span_flags(0) & 4) != 0) return fail("主线正文记录置了位 2（只有振り仮名 / 层模式支该置）");
        if (machine.text_span_text(0) != "AB") return fail("主线记录的文本不是主文本");
        if ((machine.text_span_flags(1) & 4) == 0) return fail("振り仮名记录没有置位 2（应当用 B 支字号）");
        if (machine.text_span_text(1) != "\x82\xA9\x82\xAB") return fail("振り仮名记录的文本不对");
        // ③ 字号支的唯一口径
        if (machine.record_font_size(0) != 0) return fail("位 2 未置时字号覆盖应当是 0（沿用 A 支）");
        if (machine.record_font_size(4) != 10) return fail("位 2 置起时字号覆盖应当是 B 支 = 10");
        if (machine.record_font_size(8) != 0) return fail("行结束记录（位 3）不该被当成 B 支");
        // 待画格：主文本的格 A 支（size = 0）、振り仮名的格 B 支（size = 10）
        const std::size_t count = machine.pending_glyph_count();
        if (count < 2) return fail("待画格太少（主文本或振り仮名没有入队）");
        if (machine.pending_glyph_size(0) != 0 || machine.pending_glyph_size(1) != 0) {
            return fail("主文本的待画格带了字号覆盖（A 支应当用缺省）");
        }
        for (std::size_t i = count; i-- > 2;) {
            if (machine.pending_glyph_size(i) != 10) return fail("振り仮名的待画格没有按 B 支字号（10）入队");
        }
    }
    // ② 振り仮名为空：只有主文本那一条记录，且位 2 = 0。
    {
        auto bytes = build(false);
        core::Machine machine(key, table);
        machine.write_text(core::Operand{5, 0}, "AB");
        machine.write_text(core::Operand{5, 1}, "");
        machine.load_root(bytes);
        for (int i = 0; i < 40 && machine.pending_glyph_count() == 0; ++i) {
            if (!machine.step()) return fail("（空振り仮名）走到 0x196 之前停机");
        }
        if (machine.text_span_count() != 1) return fail("空振り仮名时不该追加振り仮名记录");
        if ((machine.text_span_flags(0) & 4) != 0) return fail("空振り仮名时主线记录置了位 2");
    }
    // ③ 文本目标 id 的位 31 置起 ⇒ 整批记录都不追加（振り仮名那条也不追加）。
    {
        auto bytes = build(true);
        core::Machine machine(key, table);
        machine.write_text(core::Operand{5, 0}, "AB");
        machine.write_text(core::Operand{5, 1}, "\x82\xA9\x82\xAB");
        machine.load_root(bytes);
        for (int i = 0; i < 40 && machine.pending_glyph_count() == 0; ++i) {
            if (!machine.step()) return fail("（抑制）走到 0x196 之前停机");
        }
        if (machine.text_span_count() != 0) return fail("文本目标 id 置了位 31 却仍追加了记录");
    }
    return true;
}

// ── 振り仮名的横向定位：铺满格宽（两端各留一份步进）/ 过宽时以格中心对齐 ──
/** 引擎 `sub_465a20`（调用点 `0x46c379`）：振り仮名摆进**它自己那一格**（= 本条主文本最后一行的范围），
 *  `d = 格宽 − 整串量度宽`、`n = 假名数 + 1`：
 *   · d > 0 ⇒ 每档步进 ⌊d÷n⌋、首字 = 格左 + 步进 ⇒ **左右两端各留一份步进**（两端对齐）、假名之间等间距；
 *   · d ≤ 0 ⇒ 整串以格中心对齐 ⇒ 左侧伸出量与右侧伸出量相等。
 *  ★ 下面只用**绘制条目的矩形**做断言（不需要知道字体量度）：旧实现（左对齐 + 整串一个条目）
 *   必然让"左端缩进 > 0"与"右端缩进 = 左端缩进"同时不成立 ⇒ 红。 */
bool check_text_ruby(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "ruby: %s\n", what);
        return false;
    };
    // 建一台机器：主文本 + 振り仮名各排一遍，然后返回条目的矩形（键 = 基数 + 序号）。
    const auto layout = [&](const std::string& main_text, const std::string& ruby,
                            std::vector<core::DrawItem>* out) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x80, {{0, 9}});
        emit_instruction(bytes, 0x70, {{0, 9}, {0, 824}, {0, 120}, {0, 108}, {0, 52}});
        emit_instruction(bytes, 0x79, {{0, 9}, {0, 108}, {0, 52}});
        emit_instruction(bytes, 0x213, {{0, 9}, {0, 180700}, {0, 40}});
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 0x197, {{0, 10}});
        emit_instruction(bytes, 0x76, {{0, 0xFFFFFF}});
        emit_instruction(bytes, 0x71, {{0, 9}});
        emit_instruction(bytes, 0x196, {{0, 0}, {5, 0}, {5, 1}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.write_text(core::Operand{5, 0}, main_text);
        machine.write_text(core::Operand{5, 1}, ruby);
        machine.load_root(bytes);
        for (int i = 0; i < 200 && out->empty(); ++i) {
            if (!machine.step()) break;
        }
        for (std::uint32_t k = 0; k < 40; ++k) {
            const core::DrawItem* item = machine.draw_item(180700u + k);
            if (item == nullptr) continue;
            out->push_back(*item);
        }
    };
    // ① 窄振り仮名 × 宽格 ⇒ 两端各留一份步进、假名之间等间距。
    {
        std::vector<core::DrawItem> items;
        layout("ABCDEFGH", "\x82\xA9\x82\xAB\x82\xAD", &items);  // かきく（3 假名）
        if (items.size() != 11) return fail("条目数不是 8 主文本 + 3 振り仮名");
        const int cell_left = items[0].left;
        const int cell_right = items[7].right;
        const int left_inset = items[8].left - cell_left;
        const int right_inset = cell_right - items[10].right;
        const int gap1 = items[9].left - items[8].right;
        const int gap2 = items[10].left - items[9].right;
        if (left_inset <= 0) {
            return fail("振り仮名仍贴着格左（左对齐）—— 引擎是两端各留一份步进");
        }
        if (std::abs(left_inset - right_inset) > 1) {
            std::fprintf(stderr, "ruby: 左缩进=%d 右缩进=%d\n", left_inset, right_inset);
            return fail("两端缩进不相等（不是「铺满格宽」那一支）");
        }
        if (std::abs(gap1 - gap2) > 1 || std::abs(gap1 - left_inset) > 1) {
            std::fprintf(stderr, "ruby: gap1=%d gap2=%d inset=%d\n", gap1, gap2, left_inset);
            return fail("假名之间的间距不等于两端边距（步进没有均匀分摊）");
        }
    }
    // ② 宽振り仮名 × 窄格 ⇒ 以格中心对齐：左侧伸出量 ≈ 右侧伸出量。
    {
        std::vector<core::DrawItem> items;
        layout("A", "\x82\xA9\x82\xAB\x82\xAD\x82\xAF\x82\xB1", &items);  // かきくけこ（5 假名）
        if (items.size() != 6) return fail("条目数不是 1 主文本 + 5 振り仮名");
        const int cell_left = items[0].left;
        const int cell_right = items[0].right;
        const int overflow_left = cell_left - items[1].left;
        const int overflow_right = items[5].right - cell_right;
        if (overflow_left <= 0) {
            return fail("振り仮名比格宽却没有向左伸出 —— 不是「以格中心对齐」那一支");
        }
        if (std::abs(overflow_left - overflow_right) > 2) {
            std::fprintf(stderr, "ruby: 左伸出=%d 右伸出=%d 格宽=%d\n", overflow_left,
                         overflow_right, cell_right - cell_left);
            return fail("左右伸出量不相等（不是居中）");
        }
    }
    return true;
}

// ── typewriter：追加时只写布局格，绘制侧**每帧只推进一格** ──
/** 引擎：`0x6e` / `0x196` 追加时一次性写好布局格与文本记录（绘制条目也是**预先**排好、指向画布区域），
 *  但**落字**由推进器每帧推一格（`sub_409400` 每帧 → `0x4096a9` 序号 +1；帧间按 `message:MessageSpeed`
 *  睡眠；按键 / 跳过时一次抽干）。我们的 `step()` 就是"一帧"。
 *  ⇒ 判据（三半，缺一不可）：
 *   ① 追加那一步之后：待画格数 = 字符数、**画布仍是全透明**、绘制条目**已经**排好（布局是预先的）；
 *   ② 之后每走一步：待画格数 −1，且画布上出现墨；
 *   ③ 走满字符数步之后：待画格数 = 0。
 *  ★ 反例（旧实现）：追加那一步就把整段画进画布 ⇒ ① 的画布不透明（红）。 */
bool check_text_typewriter(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "typewriter: %s\n", what);
        return false;
    };
    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x80, {{0, 9}});
    emit_instruction(bytes, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 5}, {0, 5}});
    emit_instruction(bytes, 0x79, {{0, 9}, {0, 5}, {0, 5}});
    emit_instruction(bytes, 0x213, {{0, 9}, {0, 190300}, {0, 20}});
    emit_instruction(bytes, 0x75, {{0, 20}});
    emit_instruction(bytes, 0x76, {{0, 0xFFFFFF}});
    emit_instruction(bytes, 0x77, {{0, 0x000000}});
    emit_instruction(bytes, 0x78, {{0, 0}});
    emit_instruction(bytes, 0x74, {{0, 5}});  // 每字间隔 5ms（自检 ConfigStore 没有发行默认表 ⇒ 不设就是 0）
    emit_instruction(bytes, 0x71, {{0, 9}});
    emit_instruction(bytes, 0x6E, {{0, 0}, {5, 0}});  // "ABC" ⇒ 三个待画格
    // 之后几条"帧"（用幂等的设字号当空转指令）：每步揭示一格。
    emit_instruction(bytes, 0x75, {{0, 20}});
    emit_instruction(bytes, 0x75, {{0, 20}});
    emit_instruction(bytes, 0x75, {{0, 20}});
    emit_instruction(bytes, 2, {});
    core::Machine machine(key, table);
    machine.write_text(core::Operand{5, 0}, "ABC");
    machine.load_root(bytes);
    for (int i = 0; i < 9; ++i) {
        if (!machine.step()) return fail("0x71 之前就停机");
    }
    if (machine.pending_glyph_count() != 0) return fail("清空之后不该有待画格");
    // 走到 0x6e 真正执行（中间可能过门；门帧也是"帧"，只是队列空所以什么也不做）。
    int guard_steps = 0;
    while (machine.pending_glyph_count() == 0 && guard_steps < 200) {
        if (!machine.step()) {
            const auto& stop = machine.last_stop();
            std::fprintf(stderr, "typewriter: 走到 0x6e 之前停机 reason=%d opcode=0x%X msg=%s errors=%zu\n",
                         static_cast<int>(stop.reason), stop.opcode, stop.message.c_str(),
                         machine.errors().size());
            for (const auto& e : machine.errors()) std::fprintf(stderr, "  err=%s\n", e.c_str());
            return fail("没走到 0x6e 就停机");
        }
        ++guard_steps;
    }
    const auto ink = [&machine]() {
        const core::TextureRecord* texture =
            machine.texture(core::Machine::text_layer_texture_id(9));
        if (texture == nullptr) return std::size_t{0};
        std::size_t count = 0;
        for (std::size_t i = 3; i < texture->rgba.size(); i += 4) {
            if (texture->rgba[i] != 0) ++count;
        }
        return count;
    };
    // ① 追加之后：布局已排好（条目在），但一个像素都没落。
    if (machine.draw_item(190300) == nullptr || machine.draw_item(190301) == nullptr ||
        machine.draw_item(190302) == nullptr) {
        return fail("追加那一步没有把三个布局条目预先排好");
    }
    if (machine.pending_glyph_count() != 3) {
        return fail("追加之后待画格数不是 3 —— 追加时就画了（typewriter 被压平）");
    }
    if (core::text_backend_available() && ink() != 0) {
        return fail("追加那一步就落了像素 —— 引擎是每帧只推进一格");
    }
    // ② 每走一步揭示一格。
    const std::size_t before = ink();
    if (!machine.step()) return fail("第一帧停机");
    if (machine.pending_glyph_count() != 2) return fail("走一帧后待画格数没有 −1");
    if (core::text_backend_available() && ink() <= before) return fail("第一帧没有把第一个字画进画布");
    // ③ 走满后队列空。
    for (int i = 0; i < 4; ++i) {
        if (!machine.step()) break;
    }
    if (machine.pending_glyph_count() != 0) return fail("走满字符数帧之后待画格仍非 0");
    return true;
}


/** 同一个场景各画一次，唯一变量是三个 AA 配置格。判据 = 文本层画布里的**最大 alpha** 与**墨迹 RGB**：
 *  · 关 ⇒ 1 位掩码 ⇒ 覆盖像素全是 255（且源色原值）；
 *  · 开 ⇒ GRAY4 的 17 级 ⇒ 满覆盖 240，**RGB 仍是源色原值**（真机在空画布上走"整像素覆写"支，
 *    ⛔ 不是"与黑色加权" —— 后者会把 AA 边缘压成 α 倍、字更细更暗）。
 *  ★ 这条把"配置 → 格式 → 级换算 → 覆写/加权"整条链钉在一起：只改其中一环（回退路径又无视
 *   `message:UseAntiFont`、把 17 改回 16、把"目标 alpha=0"实现成加权）都会让本条变红。 */
bool check_glyph_brightness(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "glyph-brightness: %s\n", what);
        return false;
    };
    if (!core::text_backend_available()) return true;  // 没编文本后端 ⇒ 这条不成立（不是失败）

    // 返回 {最大 alpha, 墨迹里最小的 RGB 通道}；-1 表示画布没建出来。
    const auto draw_and_probe = [&](bool anti_alias) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x80, {{0, 9}});
        emit_instruction(bytes, 0x70, {{0, 9}, {0, 824}, {0, 120}, {0, 324}, {0, 570}});
        emit_instruction(bytes, 0x79, {{0, 9}, {0, 108}, {0, 52}});
        emit_instruction(bytes, 0x213, {{0, 9}, {0, 180500}, {0, 500}});
        emit_instruction(bytes, 0x75, {{0, 30}});
        emit_instruction(bytes, 0x76, {{0, 0xFFFFFF}});  // 主色 = 白（墨迹应当就是 FF FF FF）
        emit_instruction(bytes, 0x77, {{0, 0x000000}});
        emit_instruction(bytes, 0x78, {{0, 0}});  // 绘制模式 0 ⇒ 只画主色一遍（每像素只写一次）
        emit_instruction(bytes, 0x71, {{0, 9}});
        emit_instruction(bytes, 0x196, {{0, 0}, {5, 0}, {5, 1}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.config_mut().set_int("set:EnableAntiFont", 1);
        machine.config_mut().set_int("message:UseAntiFont", anti_alias ? 1 : 0);
        machine.config_mut().set_int("message:AntiFontLevel", 0);  // ⇒ GRAY4
        machine.write_text(core::Operand{5, 0}, "AB");
        machine.write_text(core::Operand{5, 1}, "");
        machine.load_root(bytes);
        for (int i = 0; i < 11; ++i) {
            if (!machine.step()) break;
        }
        const core::TextureRecord* texture =
            machine.texture(core::Machine::text_layer_texture_id(9));
        if (texture == nullptr) return std::pair<int, int>{-1, -1};
        int best_alpha = 0;
        int worst_rgb = 255;
        for (std::size_t i = 0; i + 3 < texture->rgba.size(); i += 4) {
            const int alpha = texture->rgba[i + 3];
            if (alpha == 0) continue;
            best_alpha = std::max(best_alpha, alpha);
            for (std::size_t c = 0; c < 3; ++c) {
                worst_rgb = std::min(worst_rgb, static_cast<int>(texture->rgba[i + c]));
            }
        }
        return std::pair<int, int>{best_alpha, worst_rgb};
    };

    const auto mono = draw_and_probe(false);
    const auto gray4 = draw_and_probe(true);
    if (mono.first != 255) {
        std::fprintf(stderr, "glyph-brightness: AA 关时最大 alpha 应当是 255（1 位掩码），实际 %d\n",
                     mono.first);
        return false;
    }
    if (gray4.first != 240) {
        std::fprintf(stderr,
                     "glyph-brightness: AA 开（GRAY4）时最大 alpha 应当是 240（值 × 255 / 17），实际 %d\n",
                     gray4.first);
        return false;
    }
    // ★ 空画布上的第一次写入是**整像素覆写**（引擎 `0x46DDD5`）⇒ 墨迹 RGB 必须是源色原值。
    //   坏法：与黑色加权 ⇒ 满覆盖 240 处 RGB = FF×240/255 = F0、AA 边缘更暗（字变细变暗）。
    if (gray4.second != 255) {
        std::fprintf(stderr,
                     "glyph-brightness: AA 开时墨迹 RGB 被压暗了（最小通道 = %d，期望 255）"
                     "—— 空画布上的首次写入必须是整像素覆写，不是与黑色加权\n",
                     gray4.second);
        return false;
    }
    return true;
}

// ── 玩家配置的根与保存闸（引擎用**本地**应用数据目录，⛔ 不是漫游） ──
/** 这条守"配置根选错"这一类错 —— 它的后果不是报错，而是**读到另一个世界**的配置
 *  （字体名 / AA 开关 / 消息速度全是另一份值），以及把我们的退时保存写进真机那份文件。
 *  · `set:UseAppDataFolder != 0` ⇒ `%LOCALAPPDATA%`；两个候选都给时**必须**选本地那个；
 *    只有漫游候选时⛔ 不许回退过去（宁可留在安装目录）。
 *  · 保存闸：真实玩家配置（落在解析出来的根里面）**默认不写**，要写得显式指路。 */
bool check_config_paths() {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "config-paths: %s\n", what);
        return false;
    };
    const std::filesystem::path local("C:/Users/x/AppData/Local");
    const std::filesystem::path roaming("C:/Users/x/AppData/Roaming");
    // `set:UseAppDataFolder` 的判据是**恰为 1**（spec 两处都这么写）：0 / 2 / 负数都不换根。
    if (core::use_app_data_folder_root(0) || !core::use_app_data_folder_root(1) ||
        core::use_app_data_folder_root(2) || core::use_app_data_folder_root(-1)) {
        return fail("set:UseAppDataFolder 的判据不是「恰为 1」（0 / 2 / -1 都不该换根）");
    }
    core::ConfigRootEnvironment env;
    env.local_app_data = local;
    env.roaming_app_data = roaming;
    if (core::player_config_root("E:/game", true, env) != local) {
        return fail("两个候选都在时必须选**本地**应用数据目录（引擎不读漫游根）");
    }
    if (core::player_config_root("E:/game", false, env) != std::filesystem::path("E:/game")) {
        return fail("set:UseAppDataFolder == 0 时根应当是安装目录");
    }
    env.local_app_data.clear();
    if (core::player_config_root("E:/game", true, env) != std::filesystem::path("E:/game")) {
        return fail("本地根缺失时回退到了漫游根（该值只用于诊断，不许参与选择）");
    }
    env.data_home = local;  // 非 Windows 的候选：同样不许是漫游那种"按用户配置"的根
    if (core::player_config_root("E:/game", true, env) != local) {
        return fail("XDG 数据目录没有参与选择");
    }

    // 保存闸
    const std::filesystem::path player = local / "Eushully" / "title" / "SYS4REG.INI";
    if (!core::path_is_within(player, local)) return fail("path_is_within 判错了前缀");
    if (core::path_is_within(roaming / "Eushully" / "x.ini", local)) {
        return fail("path_is_within 把根之外的路径判成在里面");
    }
    using Policy = core::ConfigSavePolicy;
    if (core::should_save_player_config(player, local, false, true, Policy::Default)) {
        return fail("默认策略写了真实玩家配置（外部只读数据）");
    }
    if (!core::should_save_player_config("tmp/copy.ini", local, true, true, Policy::Default)) {
        return fail("显式指路的落点却不写（--config 是用户自己的选择）");
    }
    if (core::should_save_player_config(player, local, false, false, Policy::Allow)) {
        return fail("没有发生引擎退出也写了配置");
    }
    if (!core::should_save_player_config(player, local, false, true, Policy::Allow)) {
        return fail("--config-save 没有生效");
    }
    if (core::should_save_player_config(player, local, true, true, Policy::Never)) {
        return fail("--no-config-save 没有生效");
    }
    return true;
}

// ── 文本层画布：`0x70/0x79/0x213/0x71` 的状态 + `0x6e/0x196/0x6f` 的栅格化 ──
/** 设置界面底部示例文本的**结构级**判据（不依赖游戏安装）：
 *  · 层 9 的画布 = 纹理 29，尺寸来自 `0x70`（824×120），屏幕位置 = (324,570)；
 *  · `0x213` 的键基数 = 逐字符绘制条目的键（键 = 基数 + 序号）；`0x71` 把画布清空并清掉旧条目；
 *  · `0x6e` / `0x196` 只推进**该层**的游标，`0x6f` 复位 x 并前进一行；
 *  · 再次 `0x71` ⇒ 画布回到全透明、旧条目被清。
 *  ★ 像素级观感不在这里断言（字体后端 / 宿主字体不同）；这里只断言"画进去了"（存在非透明像素）。 */
bool check_text_layer_surface(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "text-layer: %s\n", what);
        return false;
    };

    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x80, {{0, 9}});  // 默认槽 = 9（`show-text 0` 落到层 9）
    emit_instruction(bytes, 0x70, {{0, 9}, {0, 824}, {0, 120}, {0, 324}, {0, 570}});
    emit_instruction(bytes, 0x79, {{0, 9}, {0, 108}, {0, 52}});
    emit_instruction(bytes, 0x213, {{0, 9}, {0, 180500}, {0, 500}});
    emit_instruction(bytes, 0x75, {{0, 30}});   // A 支字号（主文本）
    emit_instruction(bytes, 0x197, {{0, 10}});  // B 支字号（振り仮名）
    emit_instruction(bytes, 0x76, {{0, 0xFFFFFF}});
    emit_instruction(bytes, 0x77, {{0, 0x000000}});
    emit_instruction(bytes, 0x78, {{0, 1}});
    emit_instruction(bytes, 0x71, {{0, 9}});
    emit_instruction(bytes, 0x196, {{0, 0}, {5, 0}, {5, 1}});  // 正文 AB + 振り仮名 R
    emit_instruction(bytes, 0x6E, {{0, 0}, {5, 0}});            // 再追加 AB
    emit_instruction(bytes, 0x6F, {{0, 0}});
    emit_instruction(bytes, 2, {});

    core::Machine machine(key, table);
    machine.write_text(core::Operand{5, 0}, "AB");
    machine.write_text(core::Operand{5, 1}, "R");
    machine.load_root(bytes);
    for (int i = 0; i < 10; ++i) {
        if (!machine.step()) return fail("0x71 之前就停机");
    }
    if (!machine.text_layer_texture_ready(9)) return fail("0x71 没有建出层 9 的画布");
    const core::TextureRecord* texture = machine.texture(core::Machine::text_layer_texture_id(9));
    if (texture == nullptr || texture->width != 824 || texture->height != 120) {
        return fail("层 9 的纹理不是 824×120（0x70 的 w/h 没落到画布）");
    }
    {
        bool any = false;
        for (std::size_t i = 3; i < texture->rgba.size(); i += 4) {
            if (texture->rgba[i] != 0) { any = true; break; }
        }
        if (any) return fail("0x71 之后画布不是全透明（清空没生效）");
    }
    if (machine.draw_item(180500) != nullptr) return fail("0x71 在还没有正文时就排了绘制条目");

    // 0x196 + 0x6e：游标从文本原点 (108,52) 向右推进；空串不推进。
    for (int debug_step = 0; debug_step < 2; ++debug_step) {
        if (!machine.step()) {
            std::fprintf(stderr, "text-layer debug: step=%d ip=%zu errors=%zu halted=%d\n",
                         debug_step, machine.frame().ip, machine.errors().size(), machine.halted() ? 1 : 0);
            for (const auto& e : machine.errors()) std::fprintf(stderr, "  err=%s\n", e.c_str());
            return fail("0x196 / 0x6e 停机");
        }
    }
    const std::int32_t after_text_x = machine.text_layer_cursor_x(9);
    if (core::text_backend_available() && after_text_x <= 108) {
        return fail("0x6e / 0x196 没有推进文本层游标");
    }
    if (machine.text_layer_cursor_y(9) != 52) return fail("0x6e / 0x196 改了游标 y（只有行结束才前进一行）");
    if (core::text_backend_available()) {
        bool any = false;
        for (std::size_t i = 3; i < texture->rgba.size(); i += 4) {
            if (texture->rgba[i] != 0) { any = true; break; }
        }
        if (!any) return fail("0x6e / 0x196 没有把字画进纹理 29（全透明）");
    }
    // 逐字符条目：AB(0x196) + AB(0x6e) = 4 个主文本条目 + 1 个振り仮名条目，键 = 180500 + 序号。
    const core::DrawItem* first = machine.draw_item(180500);
    if (first == nullptr || (first->flags & 1u) == 0) return fail("0x6e / 0x196 没有排逐字符绘制条目");
    if (first->texture_id != core::Machine::text_layer_texture_id(9)) return fail("逐字符条目指向的不是层 9 的画布");
    if (first->left != 108 || first->top != 52) return fail("第一个逐字符条目的源矩形不是文本原点 (108,52)");
    if (first->aux[0] != 324.0f + 108.0f || first->aux[1] != 570.0f + 52.0f) {
        return fail("第一个逐字符条目的位置不是「层左上 + 文本原点」");
    }
    const core::DrawItem* second = machine.draw_item(180501);
    if (second == nullptr || second->left <= first->left) return fail("逐字符条目的键没有按序号递增");
    if (machine.draw_item(180502) == nullptr) return fail("缺少振り仮名条目（key 应为 180502）");

    // 0x6f：x 复位到文本原点，y 前进「行間隙 6 + A 支字号 30」。
    if (!machine.step()) return fail("0x6f 停机");
    if (machine.text_layer_cursor_x(9) != 108) return fail("0x6f 没有把游标 x 复位到文本原点");
    if (machine.text_layer_cursor_y(9) != 52 + 30 + 6) return fail("0x6f 没有把游标 y 前进「字号 + 行間隙」");

    // 第二次 0x71：画布必须回到全透明（绘制项仍指向同一张画布）。
    auto clear_bytes = make_script_with_body();
    emit_instruction(clear_bytes, 0x80, {{0, 9}});
    emit_instruction(clear_bytes, 0x70, {{0, 9}, {0, 64}, {0, 32}, {0, 10}, {0, 20}});
    emit_instruction(clear_bytes, 0x213, {{0, 9}, {0, 190000}, {0, 10}});
    emit_instruction(clear_bytes, 0x75, {{0, 16}});
    emit_instruction(clear_bytes, 0x71, {{0, 9}});
    emit_instruction(clear_bytes, 0x6E, {{0, 0}, {5, 0}});
    emit_instruction(clear_bytes, 0x71, {{0, 9}});
    emit_instruction(clear_bytes, 2, {});
    core::Machine cleared(key, table);
    cleared.write_text(core::Operand{5, 0}, "X");
    cleared.load_root(clear_bytes);
    for (int i = 0; i < 7; ++i) {
        if (!cleared.step()) break;
    }
    const core::TextureRecord* cleared_texture =
        cleared.texture(core::Machine::text_layer_texture_id(9));
    if (cleared_texture == nullptr) return fail("第二次 0x71 把画布删了（应当只清空）");
    for (std::size_t i = 3; i < cleared_texture->rgba.size(); i += 4) {
        if (cleared_texture->rgba[i] != 0) return fail("第二次 0x71 之后画布仍有像素");
    }
    for (int i = 0; i < 10; ++i) {
        if (cleared.draw_item(190000u + static_cast<std::uint32_t>(i)) != nullptr) {
            return fail("第二次 0x71 之后旧逐字符条目还在");
        }
    }

    // 自动折行门：配置 `set:AutoLineFeed` 非 0 ⇒ 超右界回原点、y 前进一行；0 ⇒ 不折行。
    const auto make_wrap_script = []() {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x80, {{0, 9}});
        emit_instruction(bytes, 0x70, {{0, 9}, {0, 64}, {0, 64}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x79, {{0, 9}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x213, {{0, 9}, {0, 190100}, {0, 40}});
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 0x76, {{0, 0xFFFFFF}});
        emit_instruction(bytes, 0x77, {{0, 0x000000}});
        emit_instruction(bytes, 0x78, {{0, 1}});
        emit_instruction(bytes, 0x71, {{0, 9}});
        emit_instruction(bytes, 0x6E, {{0, 0}, {5, 0}});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    {
        auto wrap_bytes = make_wrap_script();
        core::Machine wrap_machine(key, table);
        wrap_machine.write_text(core::Operand{5, 0}, "AAAAAAAAAA");
        wrap_machine.config_mut().set_int("set:AutoLineFeed", 1);
        wrap_machine.load_root(wrap_bytes);
        for (int i = 0; i < 10; ++i) {
            if (!wrap_machine.step()) return fail("AutoLineFeed=1 停机");
        }
        if (!core::text_backend_available()) return true;
        if (wrap_machine.text_layer_cursor_y(9) <= 5) {
            return fail("AutoLineFeed=1 时超右界没有折行");
        }
        bool second_line = false;
        bool second_line_at_origin = false;
        for (std::uint32_t k = 0; k < 40; ++k) {
            const core::DrawItem* item = wrap_machine.draw_item(190100u + k);
            if (item != nullptr && item->top > 5) {
                second_line = true;
                if (item->left == 5) second_line_at_origin = true;
            }
        }
        if (!second_line) return fail("AutoLineFeed=1 时没有第二行的绘制条目");
        if (!second_line_at_origin) return fail("折行后没有回到文本原点 x=5");
        // ★ 折行是"提前结束当前行"，不是"把已排好的前缀搬到新行重排"：
        //   第一行的字符格必须留在原 y（= 文本原点），整行被搬走 ⇒ 这里红。
        std::size_t first_line_items = 0;
        for (std::uint32_t k = 0; k < 40; ++k) {
            const core::DrawItem* item = wrap_machine.draw_item(190100u + k);
            if (item != nullptr && item->top == 5 && item->left >= 5) ++first_line_items;
        }
        if (first_line_items == 0) {
            return fail("折行把第一行的字符格搬到了新行（前缀不应在新行重排；引擎的越界分支只提交该行、不改坐标）");
        }
    }
    {
        // 0x1c1 改宽 / 高拷贝（+0x24/+0x28）⇒ 折行阈值跟着改小。
        // 在 0x70 之后插入 0x1c1：槽 9、宽 20、高 64。
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x80, {{0, 9}});
        emit_instruction(bytes, 0x70, {{0, 9}, {0, 64}, {0, 64}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x1C1, {{0, 9}, {0, 20}, {0, 64}});
        emit_instruction(bytes, 0x79, {{0, 9}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x213, {{0, 9}, {0, 190100}, {0, 40}});
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 0x76, {{0, 0xFFFFFF}});
        emit_instruction(bytes, 0x77, {{0, 0x000000}});
        emit_instruction(bytes, 0x78, {{0, 1}});
        emit_instruction(bytes, 0x71, {{0, 9}});
        emit_instruction(bytes, 0x6E, {{0, 0}, {5, 0}});
        emit_instruction(bytes, 2, {});
        core::Machine pair_machine(key, table);
        pair_machine.write_text(core::Operand{5, 0}, "AAAAAAAAAA");
        pair_machine.config_mut().set_int("set:AutoLineFeed", 1);
        pair_machine.load_root(bytes);
        for (int i = 0; i < 11; ++i) {
            if (!pair_machine.step()) return fail("0x1c1 折行阈值用例停机");
        }
        if (!core::text_backend_available()) return true;
        if (pair_machine.text_layer_cursor_y(9) <= 31) {
            return fail("0x1c1 改小宽后折行阈值没有跟着变小");
        }
    }
    {
        auto no_wrap_bytes = make_wrap_script();
        core::Machine no_wrap(key, table);
        no_wrap.write_text(core::Operand{5, 0}, "AAAAAAAAAA");
        no_wrap.config_mut().set_int("set:AutoLineFeed", 0);
        no_wrap.load_root(no_wrap_bytes);
        for (int i = 0; i < 10; ++i) {
            if (!no_wrap.step()) return fail("AutoLineFeed=0 停机");
        }
        if (!core::text_backend_available()) return true;
        if (no_wrap.text_layer_cursor_y(9) != 5) {
            return fail("AutoLineFeed=0 时仍然折行了");
        }
    }
    // 文本记录：`0x196` 的排版层必须**追加一条记录**（不是只画像素）⇒ `0x82` 按下标 0 不能早退。
    // ★ 这条钉的是"记录下标空间与引擎一致"；本实现早先只画像素、`text_spans_` 为空，
    //   于是 0x82 对正文记录直接走建界早退（`0x82` 的用例 ㉑ 只覆盖了"向量为空"）。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x80, {{0, 9}});
        emit_instruction(bytes, 0x70, {{0, 9}, {0, 64}, {0, 64}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x79, {{0, 9}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x213, {{0, 9}, {0, 200000}, {0, 20}});
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 0x71, {{0, 9}});
        emit_instruction(bytes, 0x196, {{0, 0}, {5, 0}, {5, 1}});   // 正文 AB
        emit_instruction(bytes, 0x82, {{0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}});  // op2 = 记录 0
        emit_instruction(bytes, 0x6F, {{0, 0}});                    // 行结束 ⇒ 空串记录
        emit_instruction(bytes, 2, {});
        core::Machine records(key, table);
        records.write_text(core::Operand{5, 0}, "AB");
        records.load_root(bytes);
        for (int i = 0; i < 6; ++i) {
            if (!records.step()) return fail("0x196 之前就停机");
        }
        if (!records.step()) return fail("0x196 停机");
        if (records.text_span_count() != 1) {
            return fail("0x196 没有追加文本记录（0x82 会因此对正文记录早退）");
        }
        if (records.text_span_text(0) != "AB") return fail("0x196 追加的记录文本不是 AB");
        if (records.text_span_slot(0) != 9) return fail("0x196 追加的记录槽不是层 9");
        const auto draws_before = records.text_span_draw_requests();
        if (!records.step()) return fail("0x82 停机");
        if (records.text_span_draw_requests() != draws_before + 1) {
            return fail("0x82 对 0x196 产出的记录早退了（下标空间不一致）");
        }
        if (!records.step()) return fail("0x6f 停机");
        if (records.text_span_count() != 2) return fail("0x6f 没有追加行结束的空串记录");
        if (!records.text_span_text(1).empty()) return fail("行结束记录不是空串");
        if ((records.text_span_flags(1) & 8) == 0) {
            return fail("行结束记录没有置 bit3（标志 |= 8）");
        }
    }
    return true;
}

int main(int argc, char** argv) {
    // `--only-audio`：只跑音效 / 配音那条子集（供一条**独立注册**的 CTest 用）。
    // ★ 为什么要单独一个用例：spec 的证据要求"点得到某条用例"，而聚合的自检只有一个名字 ——
    //   子集用例让"音效槽量 / 两条门位分支"的回归有一个**能被点名的**失败入口。
    bool only_audio = false;
    bool only_font = false;
    bool only_glyph = false;
    bool only_config = false;
    bool only_typewriter = false;
    bool only_ruby = false;
    bool only_drain = false;
    bool only_render_prims = false;
    bool only_pacing = false;
    bool only_record_flags = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--only-audio") == 0) only_audio = true;
        else if (std::strcmp(argv[i], "--only-font") == 0) only_font = true;
        else if (std::strcmp(argv[i], "--only-glyph") == 0) only_glyph = true;
        else if (std::strcmp(argv[i], "--only-config") == 0) only_config = true;
        else if (std::strcmp(argv[i], "--only-typewriter") == 0) only_typewriter = true;
        else if (std::strcmp(argv[i], "--only-ruby") == 0) only_ruby = true;
        else if (std::strcmp(argv[i], "--only-drain") == 0) only_drain = true;
        else if (std::strcmp(argv[i], "--only-render-prims") == 0) only_render_prims = true;
        else if (std::strcmp(argv[i], "--only-pacing") == 0) only_pacing = true;
        else if (std::strcmp(argv[i], "--only-record-flags") == 0) only_record_flags = true;
        else {
            // ★ 拼错的子集开关必须当场失败：否则它会静默跑**全套**，CTest 照样绿（本条自检踩过）。
            std::fprintf(stderr, "core-selftest: unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
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

        if (only_record_flags) {
            if (!check_text_record_flags(table)) return 1;
            std::printf("core-selftest(record-flags): PASS\n");
            return 0;
        }
        if (only_pacing) {
            // 打字节流：每 D 毫秒一格、每帧最多一格；间隔 = 0 ⇒ 立即抽干。
            if (!check_text_pacing(table)) return 1;
        if (!check_text_record_flags(table)) return 1;
            std::printf("core-selftest(pacing): PASS\n");
            return 0;
        }
        if (only_render_prims) {
            // 数值文本 `0x205` 与清屏 `0x20E`（交接单第 3 项的两个空档）。
            if (!check_render_prims(table)) return 1;
            std::printf("core-selftest(render-prims): PASS\n");
            return 0;
        }
        if (only_font) {
            // 两套字号（`0x75` = A 支 / `0x197` = B 支）+ 选支的行为级断言。
            if (!check_font_branches(table)) return 1;
            std::printf("core-selftest(font): PASS\n");
            return 0;
        }
        if (only_glyph) {
            // 字形格式（1 位 / GRAY4 / GRAY8）与灰度级换算（17 / 65 级 ⇒ 满覆盖 240 / 251），
            // 外加端到端那条：AA 关 255 / 开 240。
            if (!check_glyph_alpha()) return 1;
            if (!check_glyph_compose()) return 1;
            if (!check_glyph_brightness(table)) return 1;
            std::printf("core-selftest(glyph): PASS\n");
            return 0;
        }
        if (only_config) {
            // 玩家配置的根（本地应用数据，⛔ 不是漫游）+ 退时保存的宿主闸。
            if (!check_config_paths()) return 1;
            std::printf("core-selftest(config): PASS\n");
            return 0;
        }
        if (only_drain) {
            if (!check_text_drain(table)) return 1;
            std::printf("core-selftest(drain): PASS\n");
            return 0;
        }
        if (only_ruby) {
            if (!check_text_ruby(table)) return 1;
            std::printf("core-selftest(ruby): PASS\n");
            return 0;
        }
        if (only_typewriter) {
            // typewriter：追加时只写布局格，绘制侧每帧推进一格。
            if (!check_text_typewriter(table)) return 1;
        if (!check_text_pacing(table)) return 1;
            std::printf("core-selftest(typewriter): PASS\n");
            return 0;
        }
        if (only_audio) {
            // 音效槽（后端 15 / 引擎表 10）+ 门位第 27 位的两条支路（`0xc2` A 支 / `0xc4` 影片门支）。
            if (!check_audio_slot_table()) return 1;
            if (!check_audio_slot_bounds(table)) return 1;
            if (!check_gate_split_branches(table)) return 1;
            std::printf("core-selftest(audio): PASS\n");
            return 0;
        }

        // 这两条不依赖游戏安装：放进**全量**自检里也会跑（子集开关只是让 CTest 点得到名字）。
        if (!check_glyph_alpha()) return 1;
        if (!check_glyph_compose()) return 1;
        if (!check_glyph_brightness(table)) return 1;
        if (!check_text_typewriter(table)) return 1;
        if (!check_text_ruby(table)) return 1;
        if (!check_text_drain(table)) return 1;
        if (!check_config_paths()) return 1;

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
        // 保存合同：固定段序 + 条件键；文件用 CRLF 且整体重写。
        config.set_int("display:VirtualFullScreen", 0);
        config.set_int("display:VirtualFullScreenType", 2);
        config.set_int("set:EnableAntiFont", 0);
        config.set_int("message:UseAntiFont", 1);
        config.set_string("message:Font", "Arial");
        config.save_player_file("core-selftest-save.ini");
        {
            std::ifstream saved("core-selftest-save.ini", std::ios::binary);
            const std::string text((std::istreambuf_iterator<char>(saved)),
                                   std::istreambuf_iterator<char>());
            if (text.rfind("[display]\r\n", 0) != 0 ||
                text.find("VirtualFullScreen=0\r\n") == std::string::npos ||
                text.find("[message]\r\n") == std::string::npos ||
                text.find("Font=Arial\r\n") == std::string::npos ||
                text.find("VirtualFullScreenType") != std::string::npos ||
                text.find("UseAntiFont") != std::string::npos ||
                text.find("\n") == std::string::npos || text.find("\r\n") == std::string::npos) {
                std::fprintf(stderr, "config save contract mismatch: %s\n", text.c_str());
                std::remove("core-selftest-save.ini");
                return 1;
            }
        }
        std::remove("core-selftest-save.ini");

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
            // ★ 引擎的字形合成（`0x46D9F0`）：满覆盖覆写；否则 outA = max(dstA, srcA)、
            //   outC = (srcC*srcA + dstC*(255-srcA))/255。⛔ 不是 alpha-over：同像素画两遍不得变实。
            {
                std::uint8_t texel[4] = {0, 0, 0, 0};
                core::compose_glyph_texel(texel, 255, 255, 255, 128);
                core::compose_glyph_texel(texel, 255, 255, 255, 128);
                if (texel[3] != 128) {
                    std::fprintf(stderr, "0x204 glyph composition not idempotent alpha=%u（alpha-over 会变成 192）\n",
                                 static_cast<unsigned>(texel[3]));
                    return 1;
                }
                std::uint8_t full[4] = {10, 20, 30, 40};
                core::compose_glyph_texel(full, 1, 2, 3, 255);
                if (full[0] != 1 || full[1] != 2 || full[2] != 3 || full[3] != 255) {
                    std::fprintf(stderr, "0x204 full-coverage compose must overwrite\n");
                    return 1;
                }
                std::uint8_t blend[4] = {200, 200, 200, 128};
                core::compose_glyph_texel(blend, 0, 0, 0, 128);
                if (blend[3] != 128 || blend[0] != 99) {
                    std::fprintf(stderr, "0x204 partial compose mismatch rgba=%u,%u,%u,%u\n",
                                 blend[0], blend[1], blend[2], blend[3]);
                    return 1;
                }
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

            // ★ 纵向对位：`0x204` 的 y = **字形格顶**，不是基线。绘制入口把「本次量度 ascent」加回
            //   （`0x46F37C` / `0x471253`）再落字；对位模式 = 1 时先做 `y + 缓存ascent − 本次ascent`，
            //   缓存 = 字体重建时对**参考字体**量到的 tmAscent（`Font+0x313EC` 那张 TEXTMETRICA，
            //   由 `GetTextMetricsA` 的出参写；`+0x313F0` = tmAscent）。参考字体是重建入口用**常量
            //   字面名 "ＭＳ ゴシック"** 建的那份（`0x456D09`），不是对象当前字面名（Amayui CN）！
            //   ⇒ 模式 1 的基线 = y + 参考 ascent（≈17@20px），模式 0 = y + 渲染 ascent（≈19）。
            //   ★ 坏法（已实测）：把缓存当 0 ⇒ 基线 = y ⇒ 整行上移一个 ascent（CONFIG1.BIN 的纹理 196
            //   被切成两半）；把缓存当渲染字体 ⇒ 整体低约 2px（用户口径「有点偏下」）。
            //   用 CJK 字形（CP932 "設"）让墨迹顶可分辨；两个模式各画一次，比较墨迹顶。
            const std::string cjk = "\x90\xDD";  // "設"
            auto draw_alignment = [&](bool mode_one) {
                auto script = make_script_with_body();
                emit_instruction(script, 0x1F8, {{0, 1}, {0, 96}, {0, 48}, {0, 0}});
                // ★ 字形高度用 `0x197` 设：**`0x75` 是行高**，两者是引擎里两组不同的格。
                //   这条用例曾用 `0x75` 设"字号" —— 那正是本模型以前把两个量合成一个格的证据。
                emit_instruction(script, 0x197, {{0, 20}});
                if (mode_one) emit_instruction(script, 0x2DB, {{0, 1}});  // 对位模式 1；缺省 = 0
                emit_text_204(script, 1, 0, 10, cjk);  // y = 10 = 字形格顶
                emit_instruction(script, 2, {});
                core::Machine machine(0x12345678u, table);
                machine.load_root(script);
                machine.run(8);
                const core::TextureRecord* texture = machine.texture(1);
                int top = -1;
                int bottom = -1;
                if (texture != nullptr) {
                    for (int row = 0; row < static_cast<int>(texture->height); ++row) {
                        bool any = false;
                        for (int column = 0; column < static_cast<int>(texture->width) && !any; ++column) {
                            const std::size_t at =
                                (static_cast<std::size_t>(row) * texture->width + column) * 4;
                            any = texture->rgba[at + 3] != 0;
                        }
                        if (!any) continue;
                        if (top < 0) top = row;
                        bottom = row;
                    }
                }
                return std::pair<int, int>{top, bottom};
            };
            const auto [mode1_top, mode1_bottom] = draw_alignment(true);
            const auto [mode0_top, mode0_bottom] = draw_alignment(false);
            // 本 emulator 刻意用渲染字体（Amayui CN，20px ascender = 19）做缓存量度（引擎量的是
            // 常量参考字体 MS Gothic；见 text_raster.cpp 的宿主策略说明）。「設」bbox top ≈17
            // ⇒ 墨迹顶 ≈ y + 2 = 12；容许 [10,14]（字体版本 / 取整）。
            if (mode1_top < 10 || mode1_top > 14 || mode1_bottom >= 40) {
                std::fprintf(stderr,
                             "0x204 vertical alignment mismatch (mode 1) top=%d bottom=%d"
                             "（期望 top∈[10,14]；基线 = y + 渲染字体的 ascent）\n",
                             mode1_top, mode1_bottom);
                return 1;
            }
            // 缓存 ascent == 本次量度（本 emulator 用渲染字体重测）⇒ 模式 0 与模式 1 的基线相同；
            // 两者差超过 1px 说明缓存又走成了别的字体 / 别的量度。
            if (mode0_top < mode1_top - 1 || mode0_top > mode1_top + 1 || mode0_bottom >= 40) {
                std::fprintf(stderr,
                             "0x204 vertical alignment mismatch (mode 0 vs 1) mode0_top=%d mode1_top=%d"
                             "（期望两者相同：缓存 = 本次 = 渲染字体的 ascent）\n",
                             mode0_top, mode1_top);
                return 1;
            }
            // ★ 宿主对齐补偿（2026-10-28 用户口径）：Amayui 的 ascent 比引擎的参考字体大
            //   （GDI 实测 20px 19 vs 17、28px 27 vs 24）⇒ 墨迹整体偏低。宿主策略 = **固定用渲染
            //   字体量度、再把基线抬 2px**（`EMULATOR_CXX_BASELINE_LIFT`，0 = 关）。
            //   判据：关掉补偿后墨迹顶必须**正好低 2px** —— 否则补偿没生效 / 被别处顶掉。
            const auto draw_top_with_lift = [&](const char* lift) {
                auto script = make_script_with_body();
                emit_instruction(script, 0x1F8, {{0, 1}, {0, 96}, {0, 48}, {0, 0}});
                emit_instruction(script, 0x75, {{0, 20}});
                emit_instruction(script, 0x2DB, {{0, 1}});
                emit_text_204(script, 1, 0, 10, cjk);
                emit_instruction(script, 2, {});
                if (lift != nullptr) {
#ifdef _WIN32
                    _putenv_s("EMULATOR_CXX_BASELINE_LIFT", lift);
#else
                    setenv("EMULATOR_CXX_BASELINE_LIFT", lift, 1);
#endif
                }
                core::Machine machine(0x12345678u, table);
                machine.load_root(script);
                machine.run(8);
                if (lift != nullptr) {
#ifdef _WIN32
                    _putenv_s("EMULATOR_CXX_BASELINE_LIFT", "");
#else
                    unsetenv("EMULATOR_CXX_BASELINE_LIFT");
#endif
                }
                const core::TextureRecord* texture = machine.texture(1);
                if (texture == nullptr) return -1;
                for (int row = 0; row < static_cast<int>(texture->height); ++row) {
                    for (int column = 0; column < static_cast<int>(texture->width); ++column) {
                        const std::size_t at =
                            (static_cast<std::size_t>(row) * texture->width + column) * 4;
                        if (texture->rgba[at + 3] != 0) return row;
                    }
                }
                return -1;
            };
            const int lift_top_default = draw_top_with_lift(nullptr);
            const int lift_top_off = draw_top_with_lift("0");
            if (lift_top_default < 0 || lift_top_off - lift_top_default != 2) {
                std::fprintf(stderr,
                             "0x204 宿主基线补偿没生效：默认墨迹顶=%d、lift=0 的墨迹顶=%d"
                             "（期望关掉补偿后**正好低 2px**）\n",
                             lift_top_default, lift_top_off);
                return 1;
            }
            // ★ 无 bake 数据时是 **FT 默认 render + 单趟**（无阴影）：绘制模式 1 与模式 0 的
            //   纹理必须逐位相同。坏法：回退路径又画了副色趟 ⇒ 模式 1 的墨迹多于模式 0。
            auto draw_ink = [&](bool mode_one) {
                auto script = make_script_with_body();
                emit_instruction(script, 0x1F8, {{0, 1}, {0, 96}, {0, 48}, {0, 0}});
                emit_instruction(script, 0x75, {{0, 20}});
                if (mode_one) emit_instruction(script, 0x2DB, {{0, 1}});
                emit_text_204(script, 1, 0, 10, cjk);
                emit_instruction(script, 2, {});
                core::Machine machine(0x12345678u, table);
                machine.load_root(script);
                machine.run(8);
                const core::TextureRecord* texture = machine.texture(1);
                std::size_t ink = 0;
                if (texture != nullptr) {
                    for (std::size_t i = 3; i < texture->rgba.size(); i += 4) {
                        if (texture->rgba[i] != 0) ++ink;
                    }
                }
                return ink;
            };
            if (draw_ink(true) != draw_ink(false)) {
                std::fprintf(stderr,
                             "0x204 fallback drew a secondary pass: mode1 ink=%zu mode0 ink=%zu\n",
                             draw_ink(true), draw_ink(false));
                return 1;
            }
            // ★ 字形格式只看引擎的 AA 配置（§38 修订）：AA 关 ⇒ 1 位掩码（alpha 只有 0/255）；
            //   AA 开 ⇒ 灰度（GRAY4 的 17 级 ⇒ 含中间值、满覆盖 240）。**不分同色 / 异色、也不分
            //   字形来源** —— 格式是引擎侧的选择（`Font+0x548` / `Font+0x35584`），不是宿主观感开关。
            //   ★ 旧口径是"同色 ⇒ Bold + 灰度 + 单趟"，它的后果是：配里关掉 AA 时我们仍画出抗锯齿、
            //     满覆盖 255 ⇒ 设置界面文字比真机亮一档。
            auto draw_alpha_binary = [&](bool same_color, bool anti_alias) {
                auto script = make_script_with_body();
                emit_instruction(script, 0x1F8, {{0, 1}, {0, 96}, {0, 48}, {0, 0}});
                emit_instruction(script, 0x75, {{0, 20}});
                emit_instruction(script, 0x76, {{0, 0xFFFFFF}});
                emit_instruction(script, 0x77, {{0, same_color ? 0xFFFFFF : 0x000000}});
                emit_instruction(script, 0x78, {{0, 1}});
                emit_text_204(script, 1, 0, 10, cjk);
                emit_instruction(script, 2, {});
                core::Machine machine(0x12345678u, table);
                machine.config_mut().set_int("set:EnableAntiFont", 1);
                machine.config_mut().set_int("message:UseAntiFont", anti_alias ? 1 : 0);
                machine.config_mut().set_int("message:AntiFontLevel", 0);  // ⇒ GRAY4
                machine.load_root(script);
                machine.run(8);
                const core::TextureRecord* texture = machine.texture(1);
                if (texture == nullptr) return false;
                for (std::size_t i = 3; i < texture->rgba.size(); i += 4) {
                    const int a = texture->rgba[i];
                    if (a != 0 && a != 255) return false;
                }
                return true;
            };
            const bool nobake_same_aa_off = draw_alpha_binary(true, false);
            const bool nobake_diff_aa_off = draw_alpha_binary(false, false);
            const bool nobake_same_aa_on = draw_alpha_binary(true, true);
            const bool nobake_diff_aa_on = draw_alpha_binary(false, true);
            // 对照开关 `SAME_COLOR_MONO`：AA 开着也强制 1 位（观感对比用，不改变配置语义）。
#ifdef _WIN32
            _putenv_s("EMULATOR_CXX_SAME_COLOR_MONO", "1");
#else
            setenv("EMULATOR_CXX_SAME_COLOR_MONO", "1", 1);
#endif
            const bool nobake_same_mono = draw_alpha_binary(true, true);
#ifdef _WIN32
            _putenv_s("EMULATOR_CXX_SAME_COLOR_MONO", "");
#else
            unsetenv("EMULATOR_CXX_SAME_COLOR_MONO");
#endif
            const std::filesystem::path atlas_path =
                std::filesystem::temp_directory_path() / "amayui-atlas-selftest.stub";
            { std::ofstream(atlas_path) << 'x'; }
#ifdef _WIN32
            _putenv_s("EMULATOR_CXX_GLYPH_ATLAS", atlas_path.string().c_str());
#else
            setenv("EMULATOR_CXX_GLYPH_ATLAS", atlas_path.string().c_str(), 1);
#endif
            const bool bake_diff_mono = draw_alpha_binary(false, false);
            // ★ 绘制模式 3 的副色趟 = **径向一圈**（1px），不是斜向光晕。
            //   引擎 `sub_471180` 的模式 3 支（`0x471A4A..0x471AFD`）+ `sub_46ECC0`：
            //   `step = 360 / (sqrt(off_x + off_y) * 8)`（`dbl_526A98=8.0` / `dbl_526A90=360.0`）
            //   ⇒ 偏移 (1,1) 时 12 趟；每趟 `(round(sin(rad)*off_x), round(cos(rad)*off_y))`，
            //   `rad = 角度 * 180 / π`（`dbl_5263F0=180.0` / `dbl_526A80=π`，照抄）⇒ 偏移落在 3×3
            //   邻域内。⇒ 副色像素到主色像素的**切比雪夫距离只能是 1**。
            //   坏法（已实测，用户口径「选项/角色设定的黑边比真机粗很多」）：按 ±1..±4 斜向各画一遍
            //   ⇒ 副色伸到距离 4（黑边 4px）。
            //   ★ 这里沿用上面刚设好的 bake stub（引擎字形 ⇒ 1-bit）：灰度会让半覆盖像素混进主色一侧，
            //     量出来的距离就不再是整数格。
            {
                auto ring_script = make_script_with_body();
                emit_instruction(ring_script, 0x1F8, {{0, 1}, {0, 64}, {0, 40}, {0, 0}});
                emit_instruction(ring_script, 0x75, {{0, 20}});
                emit_instruction(ring_script, 0x76, {{0, 0xFFFFFF}});
                emit_instruction(ring_script, 0x77, {{0, 0x000000}});
                emit_instruction(ring_script, 0x78, {{0, 3}});  // 绘制模式 3
                emit_text_204(ring_script, 1, 0, 8, cjk);
                emit_instruction(ring_script, 2, {});
                core::Machine ring_machine(0x12345678u, table);
                ring_machine.load_root(ring_script);
                ring_machine.run(8);
                const core::TextureRecord* texture = ring_machine.texture(1);
                if (texture == nullptr) {
                    std::fprintf(stderr, "0x204 mode 3 ring: texture missing\n");
                    return 1;
                }
                const int width = static_cast<int>(texture->width);
                const int height = static_cast<int>(texture->height);
                const auto texel = [&](int row, int column, int channel) {
                    return texture->rgba[(static_cast<std::size_t>(row) * width + column) * 4 +
                                         channel];
                };
                const auto primary_ink = [&](int row, int column) {
                    return texel(row, column, 3) != 0 &&
                           (texel(row, column, 0) | texel(row, column, 1) | texel(row, column, 2)) != 0;
                };
                const auto secondary_ink = [&](int row, int column) {
                    return texel(row, column, 3) != 0 &&
                           (texel(row, column, 0) | texel(row, column, 1) | texel(row, column, 2)) == 0;
                };
                std::size_t secondary_pixels = 0;
                std::size_t far_pixels = 0;
                // ★ 引擎的位移集合（照 `sub_471180`/`sub_46ECC0` 的公式算出来，**硬写在测试里**当 oracle）：
                //   `rad = 角度 × 180 ÷ π` ⇒ 12 趟收敛成这 6 个位移（各出现两次）。⛔ 不是"八方向圆"：
                //   (1,-1) / (-1,1) 这两个方向**不该有副色**（它们只在把角度"修正"成 ×π÷180 时才出现）。
                const int kRing[6][2] = {{0, 1}, {1, 1}, {1, 0}, {0, -1}, {-1, -1}, {-1, 0}};
                for (int row = 0; row < height; ++row) {
                    for (int column = 0; column < width; ++column) {
                        if (!secondary_ink(row, column)) continue;
                        ++secondary_pixels;
                        // 该副色像素必须能由"某个主色像素 + 某个引擎位移"解释（否则就是画远了 / 画偏了）。
                        bool explained = false;
                        for (const auto& offset : kRing) {
                            for (int sign = 0; sign < 2 && !explained; ++sign) {
                                const int dy = sign == 0 ? offset[0] : -offset[0];
                                const int dx = sign == 0 ? offset[1] : -offset[1];
                                const int ny = row - dy;
                                const int nx = column - dx;
                                if (ny < 0 || ny >= height || nx < 0 || nx >= width) continue;
                                explained = primary_ink(ny, nx);
                            }
                        }
                        if (!explained) ++far_pixels;
                    }
                }
                // 反向：主色按这 6 个位移平移的并集（去掉主色自身）必须**恰好**等于副色集合 ——
                // 这一条同时钉住"趟数不是 1"（单趟 (1,1) 会缺 5 个方向）、"不是八方向"（多出 2 个方向）
                // 与"没画远"。
                std::size_t missing_pixels = 0;
                for (int row = 0; row < height; ++row) {
                    for (int column = 0; column < width; ++column) {
                        if (!primary_ink(row, column)) continue;
                        for (const auto& offset : kRing) {
                            const int ny = row + offset[0];
                            const int nx = column + offset[1];
                            if (ny < 0 || ny >= height || nx < 0 || nx >= width) continue;
                            if (primary_ink(ny, nx)) continue;   // 主色趟会把它覆盖掉
                            if (!secondary_ink(ny, nx)) ++missing_pixels;
                        }
                    }
                }
                if (secondary_pixels == 0 || far_pixels != 0 || missing_pixels != 0) {
                    std::fprintf(stderr,
                                 "0x204 mode 3 副色趟与引擎的径向位移不符：secondary=%zu "
                                 "无法由（主色+6 个引擎位移）解释的=%zu 该有却没有的=%zu"
                                 "（引擎：12 趟、位移 = round(sin/cos(角度×180÷π)×偏移)，共 6 个方向各两次）\n",
                                 secondary_pixels, far_pixels, missing_pixels);
                    return 1;
                }
            }
#ifdef _WIN32
            // ★ 真机字形来源（Windows GDI）：开启 `EMULATOR_CXX_GLYPH_GDI` 后必须真的走 GDI，
            //   而且格式按引擎的 AA 配置（本 fixture 没有 AA 配置 ⇒ 1-bit，alpha 只含 0/255）。
            //   坏法：env 被忽略 / 提供者没建起来而静默回退到 FreeType ⇒ 来源不是 Gdi（当场红）。
            {
                _putenv_s("EMULATOR_CXX_GLYPH_GDI", "1");
                auto gdi_script = make_script_with_body();
                emit_instruction(gdi_script, 0x1F8, {{0, 1}, {0, 96}, {0, 48}, {0, 0}});
                emit_instruction(gdi_script, 0x75, {{0, 20}});
                emit_instruction(gdi_script, 0x76, {{0, 0xFFFFFF}});
                emit_instruction(gdi_script, 0x77, {{0, 0x000000}});
                emit_instruction(gdi_script, 0x78, {{0, 1}});
                emit_text_204(gdi_script, 1, 0, 10, cjk);
                emit_instruction(gdi_script, 2, {});
                core::Machine gdi_machine(0x12345678u, table);
                gdi_machine.load_root(gdi_script);
                gdi_machine.run(8);
                const core::TextGlyphSource source = core::last_glyph_source();
                std::size_t gdi_ink = 0;
                bool gdi_binary = true;
                if (const core::TextureRecord* texture = gdi_machine.texture(1)) {
                    for (std::size_t i = 3; i < texture->rgba.size(); i += 4) {
                        const int alpha = texture->rgba[i];
                        if (alpha == 0) continue;
                        ++gdi_ink;
                        if (alpha != 255) gdi_binary = false;
                    }
                }
                _putenv_s("EMULATOR_CXX_GLYPH_GDI", "");
                if (source != core::TextGlyphSource::Gdi || gdi_ink == 0 || !gdi_binary) {
                    std::fprintf(stderr,
                                 "0x204 GDI 字形来源没生效：source=%d ink=%zu binary=%d"
                                 "（期望 source=Gdi、有墨迹、无 AA 配置 ⇒ alpha 只含 0/255）\n",
                                 static_cast<int>(source), gdi_ink, gdi_binary ? 1 : 0);
                    return 1;
                }
                // ★ AA 档：把配置打开（`set:EnableAntiFont` + `message:UseAntiFont` ⇒ GRAY4，
                //   `AntiFontLevel = 0`）。这条钉住的是**引擎的 ÷17 换算**（`0x46DAAF`）：
                //   GRAY4 的原始值域是 0..16 ⇒ alpha = 0,15,30,…,240 ⇒ **上限 240、且有中间值**。
                //   坏法：把 GRAY4 当成 0..255 直接用 ⇒ alpha 上到 255（red）；不量化 ⇒ 中间值全无规律。
                {
                    auto aa_script = make_script_with_body();
                    emit_instruction(aa_script, 0x1F8, {{0, 1}, {0, 96}, {0, 48}, {0, 0}});
                    emit_instruction(aa_script, 0x75, {{0, 20}});
                    emit_instruction(aa_script, 0x76, {{0, 0xFFFFFF}});
                    emit_instruction(aa_script, 0x77, {{0, 0x000000}});
                    emit_instruction(aa_script, 0x78, {{0, 1}});
                    emit_text_204(aa_script, 1, 0, 10, cjk);
                    emit_instruction(aa_script, 2, {});
                    _putenv_s("EMULATOR_CXX_GLYPH_GDI", "1");
                    core::Machine aa_machine(0x12345678u, table);
                    aa_machine.config_mut().set_int("set:EnableAntiFont", 1);
                    aa_machine.config_mut().set_int("message:UseAntiFont", 1);
                    aa_machine.config_mut().set_int("message:AntiFontLevel", 0);
                    aa_machine.load_root(aa_script);
                    aa_machine.run(8);
                    int max_alpha = 0;
                    bool has_partial = false;
                    bool multiple_of_15 = true;
                    std::size_t aa_ink = 0;
                    if (const core::TextureRecord* texture = aa_machine.texture(1)) {
                        for (std::size_t i = 3; i < texture->rgba.size(); i += 4) {
                            const int alpha = texture->rgba[i];
                            if (alpha == 0) continue;
                            ++aa_ink;
                            if (alpha > max_alpha) max_alpha = alpha;
                            if (alpha != 255) has_partial = true;
                            // 引擎 `0x46DAAF` 是 `value × 255 ÷ 17`：原始值 0..17 ⇒ alpha 必是 15 的倍数。
                            if (alpha % 15 != 0) multiple_of_15 = false;
                        }
                    }
                    _putenv_s("EMULATOR_CXX_GLYPH_GDI", "");
                    if (aa_ink == 0 || !has_partial || max_alpha < 200 || !multiple_of_15) {
                        std::fprintf(stderr,
                                     "0x204 GDI 的 GRAY4 档不对：ink=%zu 有中间值=%d max_alpha=%d "
                                     "全是 15 的倍数=%d（引擎 ÷17 ⇒ alpha ∈ 0,15,…,240；"
                                     "把原始值直接当 alpha ⇒ 16；×255÷16 ⇒ 满覆盖 255、真机偏暗）\n",
                                     aa_ink, has_partial ? 1 : 0, max_alpha,
                                     multiple_of_15 ? 1 : 0);
                        return 1;
                    }
                }
            }
#endif
#ifdef _WIN32
            _putenv_s("EMULATOR_CXX_GLYPH_ATLAS", "");
#else
            unsetenv("EMULATOR_CXX_GLYPH_ATLAS");
#endif
            { std::error_code ignored; std::filesystem::remove(atlas_path, ignored); }
            if (!nobake_same_aa_off || !nobake_diff_aa_off || !bake_diff_mono) {
                std::fprintf(stderr,
                             "0x204 AA 关却没走 1 位掩码：same_off=%d diff_off=%d bake_diff=%d"
                             "（配置关掉抗锯齿 ⇒ 字形只能是 0/255；回退路径又无视配置 ⇒ 会变亮）\n",
                             nobake_same_aa_off ? 1 : 0, nobake_diff_aa_off ? 1 : 0,
                             bake_diff_mono ? 1 : 0);
                return 1;
            }
            if (nobake_same_aa_on || nobake_diff_aa_on) {
                std::fprintf(stderr,
                             "0x204 AA 开却没出灰度：same_on=%d diff_on=%d"
                             "（配置打开抗锯齿 ⇒ 应当有中间 alpha，且满覆盖 240 而不是 255）\n",
                             nobake_same_aa_on ? 1 : 0, nobake_diff_aa_on ? 1 : 0);
                return 1;
            }
            if (!nobake_same_mono) {
                std::fprintf(stderr, "0x204 SAME_COLOR_MONO 对照开关没有强制 1 位\n");
                return 1;
            }
            // ★ 纹理版本号必须**严格单调递增**：窗口路径的 GPU 纹理缓存按 revision 命中。
            //   一次重画 = 0x1f8（重建 + 清空）+ N 次 0x204；若重建把 revision 重置回 1，
            //   "每次重画都写同样多条文字" ⇒ 终值恒等 ⇒ 缓存命中上一轮的纹理 ⇒ 滚动/切页不刷新。
            //   坏法（已实测，用户口径「必须文字数量变多/变少才会刷新」）：0x1f8 清空后 revision = 1。
            //   两轮「重建 + 一条 0x204」：正常终值 = 4（建=1、画=2、再建=3、再画=4）；
            //   坏法终值 = 2（再建把版本号打回 1）。
            //   ★ 这里用**字符串池**（type 5）当文本源：内联文本（type 2）的字节放在指令流里，
            //     本测试要在 0x204 之后继续执行下一条指令，不能让它踩到文本字节。
            auto revision_script = make_script_with_body();
            emit_instruction(revision_script, 0x1F8, {{0, 1}, {0, 64}, {0, 32}, {0, 0}});
            emit_instruction(revision_script, 0x204, {{0, 1}, {0, 0}, {0, 6}, {5, 0}});
            emit_instruction(revision_script, 0x1F8, {{0, 1}, {0, 64}, {0, 32}, {0, 0}});
            emit_instruction(revision_script, 0x204, {{0, 1}, {0, 0}, {0, 6}, {5, 1}});
            emit_instruction(revision_script, 2, {});
            core::Machine revision_machine(0x12345678u, table);
            revision_machine.load_root(revision_script);
            revision_machine.write_text(core::Operand{5, 0}, "A");
            revision_machine.write_text(core::Operand{5, 1}, "B");
            revision_machine.run(1000);
            const auto final_revision = revision_machine.texture(1)->revision;
            if (final_revision <= 3) {
                std::fprintf(stderr,
                             "两轮「重建 + 同条数文字」后纹理版本号 = %u（≤3）："
                             "GPU 缓存会命中上一轮纹理，滚动重画不会刷新\n",
                             final_revision);
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
        emit_instruction(dirty_script, 0x21B, {{0, 1}});
        emit_instruction(dirty_script, 0x1F8, {{0, 1}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(dirty_script, 0x55, {{3, 0}, {0, 7}});
        emit_instruction(dirty_script, 2, {});
        core::ConfigStore render_config;
        render_config.set_int("set:DrawMode", 1);
        core::Machine dirty_machine(0x12345678u, table, nullptr, std::move(render_config));
        dirty_machine.load_root(dirty_script);
        if (!dirty_machine.step()) {  // 0x21B
            return 1;
        }
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
        if (!step_machine.step()) {  // 0x1F8
            return 1;
        }
        if (step_machine.consume_frame_step()) {
            std::fprintf(stderr, "dirty scene rendered without gate before 0x20C\n");
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

        // 门前闸门：脏本身不触发渲染；需要 0x21B 或等待/影片位。
        auto gate_script = make_script_with_body();
        emit_instruction(gate_script, 0x1F8, {{0, 1}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(gate_script, 0x1FB,
                         {{0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(gate_script, 0x1A8, {});
        emit_instruction(gate_script, 0x21B, {{0, 1}});
        emit_instruction(gate_script, 0x1A8, {});
        emit_instruction(gate_script, 2, {});
        core::ConfigStore gate_config;
        gate_config.set_int("set:DrawMode", 1);
        core::Machine gate_machine(0x12345678u, table, nullptr, std::move(gate_config));
        gate_machine.load_root(gate_script);
        if (!gate_machine.step() || gate_machine.consume_frame_step()) {
            std::fprintf(stderr, "dirty scene rendered without 0x21B\n");
            return 1;
        }
        if (!gate_machine.step() || gate_machine.consume_frame_step()) {
            std::fprintf(stderr, "dirty 0x1FB rendered without 0x21B\n");
            return 1;
        }
        if (!gate_machine.step()) {  // 0x1A8
            return 1;
        }
        if (!gate_machine.step()) {  // 0x21B = 1
            return 1;
        }
        if (!gate_machine.step() || !gate_machine.consume_frame_step()) {
            std::fprintf(stderr, "0x21B did not open the render gate\n");
            return 1;
        }

        // ── 影片：0x20f 的对象表 / 绑定门 / 影片位 / 门前服务（CORE-OPS-MOVIE / CORE-VM-MOVIE） ──
        {
            FakeMovieStats stats;
            core::ConfigStore movie_config;
            movie_config.set_int("set:DrawMode", 1);
            core::Machine movie_machine(0x12345678u, table, nullptr, std::move(movie_config));
            movie_machine.set_movie_bytes_loader([](std::uint32_t) { return core::Bytes{0, 0, 1, 0xBA}; });
            movie_machine.set_movie_backend_factory([&stats]() {
                auto backend = std::make_unique<FakeMovieBackend>(&stats, 4, 4, 0x40);
                return std::unique_ptr<core::MovieBackend>(std::move(backend));
            });
            movie_machine.load_root(make_movie_script(0, true));
            if (!movie_machine.step()) {  // 0x1f8 建空白纹理
                std::fprintf(stderr, "movie fixture create-texture failed: %s\n",
                             movie_machine.last_stop().message.c_str());
                return 1;
            }
            if (!movie_machine.step() || !movie_machine.movie_active()) {  // 0x20f
                std::fprintf(stderr, "0x20f did not start the movie: %s\n",
                             movie_machine.last_stop().message.c_str());
                return 1;
            }
            const core::MovieRecord* started = movie_machine.movie(42);
            if (started == nullptr || !started->present || !started->started ||
                started->file_id != 7 || started->options != 0) {
                std::fprintf(stderr, "0x20f object table mismatch\n");
                return 1;
            }
            if ((movie_machine.state_flags() & 0x2000u) == 0) {
                std::fprintf(stderr, "0x20f did not set the movie bit\n");
                return 1;
            }
            if (!stats.opened || stats.bytes != 4 || stats.plays != 1) {
                std::fprintf(stderr, "0x20f backend open/play mismatch\n");
                return 1;
            }
            // 影片位关住门：后续迭代只服务，不推进脚本。
            const std::size_t movie_ip = movie_machine.frame().ip;
            if (!movie_machine.step()) {
                std::fprintf(stderr, "movie service step failed\n");
                return 1;
            }
            if (movie_machine.frame().ip != movie_ip) {
                std::fprintf(stderr, "script advanced while the movie bit was set\n");
                return 1;
            }
            const core::TextureRecord* playing = movie_machine.texture(42);
            if (playing == nullptr || playing->rgba.size() != 4u * 4u * 4u ||
                playing->rgba[0] != 0x40 || playing->rgba[3] != 255 || playing->revision < 2) {
                std::fprintf(stderr, "movie frame was not written into the bound texture\n");
                return 1;
            }
            if (!movie_machine.consume_frame_step()) {
                std::fprintf(stderr, "movie new frame did not request a render step\n");
                return 1;
            }
            // 跑到收尾：对象槽被移除、后端关闭、影片位由位服务清掉。
            bool finished = false;
            for (int i = 0; i < 400; ++i) {
                if (!movie_machine.step()) {
                    std::fprintf(stderr, "movie run failed: %s\n",
                                 movie_machine.last_stop().message.c_str());
                    return 1;
                }
                if (!movie_machine.movie_active()) {
                    finished = true;
                    break;
                }
            }
            if (!finished || movie_machine.movie(42) != nullptr || !stats.closed) {
                std::fprintf(stderr, "movie teardown mismatch (finished=%d closed=%d)\n",
                             finished ? 1 : 0, stats.closed ? 1 : 0);
                return 1;
            }
            movie_machine.step();  // 影片位服务：没有已启动槽 ⇒ 清位放行
            if ((movie_machine.state_flags() & 0x2000u) != 0) {
                std::fprintf(stderr, "movie bit was not cleared after teardown\n");
                return 1;
            }
            const core::TextureRecord* last = movie_machine.texture(42);
            if (last == nullptr || last->rgba[0] != 0x40) {
                std::fprintf(stderr, "teardown did not fill the texture with the last frame\n");
                return 1;
            }
        }

        // 循环位：收尾 ⇒ 回到开头重新启动，保留对象与槽，不填纹理。
        {
            FakeMovieStats stats;
            core::ConfigStore loop_config;
            loop_config.set_int("set:DrawMode", 1);
            core::Machine loop_machine(0x12345678u, table, nullptr, std::move(loop_config));
            loop_machine.set_movie_bytes_loader([](std::uint32_t) { return core::Bytes{0, 0, 1, 0xBA}; });
            loop_machine.set_movie_backend_factory([&stats]() {
                return std::unique_ptr<core::MovieBackend>(
                    std::make_unique<FakeMovieBackend>(&stats, 4, 4, 0x40));
            });
            loop_machine.load_root(make_movie_script(1, true));  // 选项字第 0 位 = 循环
            for (int i = 0; i < 200; ++i) {
                if (!loop_machine.step()) {
                    std::fprintf(stderr, "looping movie failed: %s\n",
                                 loop_machine.last_stop().message.c_str());
                    return 1;
                }
            }
            if (stats.rewinds < 1 || !loop_machine.movie_active() ||
                loop_machine.movie(42) == nullptr || (loop_machine.state_flags() & 0x2000u) == 0) {
                std::fprintf(stderr, "looping movie did not restart in place (rewinds=%d)\n",
                             stats.rewinds);
                return 1;
            }
        }

        // `(选项字 & 3) == 2` ⇒ 收尾**不**发"填纹理"：纹理保持空白（这一支不出新帧）。
        {
            for (const std::uint32_t options : {0u, 2u}) {
                FakeMovieStats stats;
                core::ConfigStore fill_config;
                fill_config.set_int("set:DrawMode", 1);
                core::Machine fill_machine(0x12345678u, table, nullptr, std::move(fill_config));
                fill_machine.set_movie_bytes_loader([](std::uint32_t) { return core::Bytes{0, 0, 1, 0xBA}; });
                fill_machine.set_movie_backend_factory([&stats]() {
                    auto backend = std::make_unique<FakeMovieBackend>(&stats, 4, 4, 0x40);
                    backend->produce_frames = false;  // 播放期不出帧 ⇒ 只剩收尾这一次填纹理
                    return std::unique_ptr<core::MovieBackend>(std::move(backend));
                });
                fill_machine.load_root(make_movie_script(options, true));
                bool finished = false;
                bool seen = false;  // 先等它真的启动，否则第一趟就会把"槽为空"当成收尾
                for (int i = 0; i < 400; ++i) {
                    if (!fill_machine.step()) {
                        std::fprintf(stderr, "option=%u movie failed: %s\n", options,
                                     fill_machine.last_stop().message.c_str());
                        return 1;
                    }
                    if (fill_machine.movie_active()) seen = true;
                    if (seen && fill_machine.movie(42) == nullptr) {
                        finished = true;
                        break;
                    }
                }
                const core::TextureRecord* texture = fill_machine.texture(42);
                if (!finished || texture == nullptr) {
                    std::fprintf(stderr, "option=%u movie did not finish\n", options);
                    return 1;
                }
                const bool filled = texture->rgba[0] == 0x40;
                if (filled != (options == 0u)) {
                    std::fprintf(stderr, "option=%u fill-texture rule mismatch (filled=%d)\n",
                                 options, filled ? 1 : 0);
                    return 1;
                }
            }
        }

        // 失败路径：绑定门为空 / 取文件失败 ⇒ 已建对象留在槽里、未启动、不置影片位。
        {
            FakeMovieStats stats;
            core::Machine gate_movie(0x12345678u, table);
            gate_movie.set_movie_backend_factory([&stats]() {
                return std::unique_ptr<core::MovieBackend>(
                    std::make_unique<FakeMovieBackend>(&stats, 4, 4, 0x40));
            });
            gate_movie.set_movie_bytes_loader([](std::uint32_t) { return core::Bytes{0, 0, 1, 0xBA}; });
            gate_movie.load_root(make_movie_script(0, false));  // 槽 42 没有纹理绑定
            gate_movie.step();
            const core::MovieRecord* leftover = gate_movie.movie(42);
            if (gate_movie.last_stop().message.find("movie-texture-slot-empty") == std::string::npos ||
                leftover == nullptr || !leftover->present || leftover->started ||
                gate_movie.movie_active() || (gate_movie.state_flags() & 0x2000u) != 0) {
                std::fprintf(stderr, "movie binding-gate failure path mismatch: %s\n",
                             gate_movie.last_stop().message.c_str());
                return 1;
            }

            FakeMovieStats open_stats;
            core::Machine open_movie(0x12345678u, table);
            open_movie.set_movie_backend_factory([&open_stats]() {
                return std::unique_ptr<core::MovieBackend>(
                    std::make_unique<FakeMovieBackend>(&open_stats, 4, 4, 0x40));
            });
            open_movie.set_movie_bytes_loader([](std::uint32_t) -> core::Bytes {
                throw core::CoreError("fixture-no-such-file");
            });
            open_movie.load_root(make_movie_script(0, true));
            open_movie.step();  // 0x1f8
            open_movie.step();  // 0x20f → 取文件失败
            const core::MovieRecord* open_leftover = open_movie.movie(42);
            if (open_movie.last_stop().message.find("movie-open-failed") == std::string::npos ||
                open_leftover == nullptr || !open_leftover->present || open_leftover->started ||
                open_movie.movie_active()) {
                std::fprintf(stderr, "movie open-failure path mismatch: %s\n",
                             open_movie.last_stop().message.c_str());
                return 1;
            }
        }

        // ── 0x1f7 拆离：四张同键表一起删（绘制项 / 网格 / 辅助 A；辅助 B 无写入口） ──
        {
            auto detach_script = make_script_with_body();
            // 键 100：绘制项 + 网格 + 辅助 A；键 102：只放辅助 A，用来验区间。
            emit_instruction(detach_script, 0x1FB,
                             {{0, 100}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
            emit_instruction(detach_script, 0x322, {{0, 100}, {0, 0}, {0, 255}, {0, 0}});
            emit_instruction(detach_script, 0x344, {{0, 100}, {0, 7}});
            emit_instruction(detach_script, 0x344, {{0, 102}, {0, 8}});
            emit_instruction(detach_script, 0x1F7, {{0, 100}, {0, 3}});  // 左闭右开 [100, 103)
            emit_instruction(detach_script, 0x1A8, {});
            emit_instruction(detach_script, 2, {});
            core::Machine detach_machine(0x12345678u, table);
            detach_machine.load_root(detach_script);
            for (int i = 0; i < 4; ++i) {
                if (!detach_machine.step()) {
                    std::fprintf(stderr, "detach fixture failed: %s\n",
                                 detach_machine.last_stop().message.c_str());
                    return 1;
                }
            }
            if (detach_machine.draw_item_count() != 1 || detach_machine.mesh_count() != 1 ||
                detach_machine.aux_a_count() != 2) {
                std::fprintf(stderr, "detach fixture did not fill the four tables\n");
                return 1;
            }
            if (!detach_machine.step()) {  // 0x1f7 count=3
                std::fprintf(stderr, "0x1f7 step failed: %s\n",
                             detach_machine.last_stop().message.c_str());
                return 1;
            }
            if (detach_machine.draw_item_count() != 0 || detach_machine.mesh_count() != 0 ||
                detach_machine.aux_a_count() != 0) {
                std::fprintf(stderr, "0x1f7 did not delete from all same-key tables "
                                     "(draw=%zu mesh=%zu auxA=%zu)\n",
                             detach_machine.draw_item_count(), detach_machine.mesh_count(),
                             detach_machine.aux_a_count());
                return 1;
            }
            if (!detach_machine.scene_dirty()) {
                std::fprintf(stderr, "0x1f7 did not dirty the scene after deleting entries\n");
                return 1;
            }
            detach_machine.end_render();  // 清脏
            if (!detach_machine.step()) {  // 0x1A8
                return 1;
            }
            // 再拆一次空键：什么都没删 ⇒ 不标脏。
            auto empty_detach = make_script_with_body();
            emit_instruction(empty_detach, 0x1F7, {{0, 999}, {0, 0}});
            emit_instruction(empty_detach, 2, {});
            core::Machine empty_machine(0x12345678u, table);
            empty_machine.load_root(empty_detach);
            empty_machine.end_render();
            if (!empty_machine.step()) {
                std::fprintf(stderr, "0x1f7 empty-key step failed\n");
                return 1;
            }
            if (empty_machine.scene_dirty()) {
                std::fprintf(stderr, "0x1f7 dirtied the scene without deleting anything\n");
                return 1;
            }
        }


        if (!check_menu_table(table)) return 1;
        if (!check_status_and_mode(table)) return 1;
        if (!check_run_bracket(table)) return 1;
        if (!check_input_dispatch(table)) return 1;
        if (!check_input_devices(table)) return 1;
        if (!check_pause_and_clock(table)) return 1;
        if (!check_second_batch(table)) return 1;
        if (!check_scene3d_setters(table)) return 1;
        if (!check_memflip(table)) return 1;
        if (!check_save_ops(table)) return 1;
        if (!check_input_hold(table)) return 1;
        if (!check_mouse_and_hit_test(table)) return 1;
        if (!check_exit_cleanup(table)) return 1;
        if (!check_config_ui_surface(table)) return 1;
        if (!check_color_normalization(table)) return 1;
        if (!check_slot_imgid(table)) return 1;
        if (!check_texture_size(table)) return 1;
        if (!check_exit_script(table)) return 1;
        if (!check_audio_slot_table()) return 1;
        if (!check_audio_slot_bounds(table)) return 1;
        if (!check_gate_split_branches(table)) return 1;
        if (!check_font_branches(table)) return 1;
        if (!check_render_prims(table)) return 1;
        if (!check_text_layer_surface(table)) return 1;

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
