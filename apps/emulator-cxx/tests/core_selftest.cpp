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
#include "core/text_raster.h"
#include "core/vm.h"

#include <cmath>
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

    // 0x74 / 0x7f：同一格；没写过就读 ⇒ 响亮失败。
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
    if (param_uninit_machine.step()) {
        std::fprintf(stderr, "0x7f read an uninitialized param cell\n");
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
        if (!check_color_normalization(table)) return 1;
        if (!check_slot_imgid(table)) return 1;
        if (!check_texture_size(table)) return 1;
        if (!check_exit_script(table)) return 1;

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
