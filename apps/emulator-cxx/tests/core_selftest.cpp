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
#include <bit>
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
#include <tuple>
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

// 渲染步"开闸"的配置：**场景绘制模式 = 1**（配置 `set:DrawMode` 决定的那个格，`CORE-TIMING-WINDOW`
// 的 (a)）。★ 不设它 ⇒ 场景推进**连盖钟一起跳过** ⇒ 颜色窗 / 消息等待计时器 / 打字节拍全都停走；
// 这些用例要的是"窗 / 节拍怎么走"，所以先让门前那个渲染步真的跑起来。
// ⛔ 不是"为了让用例变绿"：这正是 spec 给的门槛（绘制模式 ≠ 1 ⇒ 不盖钟），用例必须显式满足它。
core::ConfigStore scene_step_config() {
    core::ConfigStore config;
    config.set_int("set:DrawMode", 1);
    return config;}
core::Machine scene_step_machine(const core::OpcodeTable& table, std::uint32_t seed = 0x12345678u) {
    core::Machine machine(seed, table, nullptr, scene_step_config());
    // (c) 的第二个前置：`0x21b` 的标量 ≠ 0 **或**等待 / 影片位为 1（`70-vm.md:217`）。
    // ★ 与 `set:DrawMode` 同理：这些夹具要的是"门前渲染步真的跑"，所以显式把标量置 1
    //   （等价于脚本先跑一条 `0x21b 1`）。⛔ 引擎初值是 0（`0x40DFFC` 初始化写 0）—— 这条只给夹具用，
    //   产品路径的默认值不动：(c) 的用例必须显式把它置 0。
    machine.set_scalar_21b(1);
    return machine;
}

core::Machine text_scene_machine(std::uint32_t key, const core::OpcodeTable& table) {
    core::Machine machine(key, table, nullptr, scene_step_config());
    machine.set_scalar_21b(1);  // 同 `scene_step_machine`：让门前渲染步真的跑起来
    return machine;
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
    // C 由 `0x88` 的产品路径写（⛔ 不再播种 C）：0x88(1) ⇒ C/D=1；随后 0x19c 必须走 C 支。
    auto mode_c_script = make_script_with_body();
    emit_instruction(mode_c_script, 0x88, {{0, 1}});
    emit_instruction(mode_c_script, 0x19C, {});
    emit_instruction(mode_c_script, 2, {});
    mode_c.load_root(mode_c_script);
    if (!mode_c.step() || !mode_c.step() || mode_c.mode_d() != 1 || mode_c.mode_g() != 1 ||
        (mode_c.state_flags() & (1u << 27)) == 0) {
        std::fprintf(stderr, "0x19c C branch mismatch d=%d g=%d\n", mode_c.mode_d(),
                     mode_c.mode_g());
        return false;
    }
    core::Machine mode_e(0x12345678u, table);
    mode_e.seed_mode_cells(1, 0);
    mode_e.load_root(make_mode_script());
    if (!mode_e.step() || mode_e.mode_d() != 0 || mode_e.mode_g() != 1 ||
        (mode_e.state_flags() & (1u << 27)) == 0) {
        std::fprintf(stderr, "0x19c E branch touched D (d=%d)\n", mode_e.mode_d());
        return false;
    }
    core::Machine mode_f(0x12345678u, table);
    mode_f.seed_mode_cells(0, 1);
    mode_f.load_root(make_mode_script());
    if (!mode_f.step() || mode_f.mode_a() != 1 || mode_f.mode_b() != 0 ||
        mode_f.mode_g() != 0 || (mode_f.state_flags() & (1u << 27)) != 0) {
        std::fprintf(stderr, "0x19c F branch changed the mode bit\n");
        return false;
    }
    // 0x0d9（`CORE-OPS-STATUS-CLEAR-D9`）：① 状态选择字第 12 位清 0（读-改-写，别整字写 0）；
    // ② **队列泵占用标记**非 0 ⇒ 对**待恢复副本**做同样的清位；标记为 0 ⇒ 副本**不动**。
    // ★ 旧模型拿"另一格非 0"当判据 ⇒ 这里先用泵状态把两格摆出来，再分两趟钉住判据是**标记**。
    // ★ 用 `dispatch_for_test` 直派：本条的可观察性正是"门开着时它是空转 / 泵期间它清副本"，
    //   而门字（第 12 / 14 位）非 0 时服务链会先服务它们、本迭代轮不到派发。
    {
        // 泵占用标记 = 1：活动字带第 12 / 14 位；副本带第 12 / 14 / 21 位 ⇒ 清 12、保 14 / 21。
        core::Machine d9_machine(0x12345678u, table);
        d9_machine.state_flags_mut() = (1u << 12) | (1u << 14);
        d9_machine.seed_status_restore_for_test((1u << 12) | (1u << 14) | (1u << 21));
        d9_machine.set_pump_state_for_test(1, true);
        d9_machine.dispatch_for_test(core::Instruction{0, 0xD9, {}});
        if (d9_machine.status_restore() != ((1u << 14) | (1u << 21)) ||            d9_machine.state_flags() != (1u << 14)) {
            std::fprintf(stderr, "0x0d9: copy=0x%X active=0x%X\n", d9_machine.status_restore(),
                         d9_machine.state_flags());
            return false;
        }
    }
    {
        // 泵占用标记 = 0：副本是旧值 ⇒ **不许**清它的第 12 位（判据是标记，不是"某个格非 0"）。
        core::Machine d9_empty(0x12345678u, table);
        d9_empty.state_flags_mut() = (1u << 12);
        d9_empty.seed_status_restore_for_test(1u << 12);
        d9_empty.dispatch_for_test(core::Instruction{0, 0xD9, {}});
        if (d9_empty.status_restore() != (1u << 12) || d9_empty.state_flags() != 0) {
            std::fprintf(stderr, "0x0d9 copy=0x%X active=0x%X (pump mark = 0)\n",
                         d9_empty.status_restore(), d9_empty.state_flags());
            return false;
        }    }
    // 0x1ce：非 0 ⇒ 开关格 / 第 30 位（逐格揭示活动中）/ 格号归 0 / 相位起点 = 宿主时钟；
    // 服务分支不负责清位（详细语义见 check_text_cell_reveal）。
    auto reveal_script = make_script_with_body();
    emit_instruction(reveal_script, 0x1CE, {{0, 7}});
    emit_instruction(reveal_script, 0x1A8, {});
    emit_instruction(reveal_script, 2, {});
    core::Machine reveal_machine = scene_step_machine(table);
    reveal_machine.set_host_clock_ms(1234);
    reveal_machine.load_root(reveal_script);
    if (!reveal_machine.step() || reveal_machine.reveal_switch() != 7 ||
        (reveal_machine.state_flags() & (1u << 30)) == 0 ||
        reveal_machine.reveal_cursor() != 0 || reveal_machine.reveal_tick_start_ms() != 1234u ||
        reveal_machine.frame().ip != 12) {
        std::fprintf(stderr, "0x1ce enable mismatch switch=%d phase=%u ip=%zu\n",
                     reveal_machine.reveal_switch(), reveal_machine.reveal_tick_start_ms(),
                     reveal_machine.frame().ip);
        return false;
    }
    // ★ 第 30 位**不交回迭代头**（`CORE-VM-STATE` 位序表第 8 项的服务后 = **继续扫描**）⇒ 门关着也
    //   不挡派发：扫到表尾照常执行一条（`CORE-VM-GATE`：⛔ 不许写成"任一位置起就挡住派发"）。
    const auto reveal_ip = reveal_machine.frame().ip;
    if (!reveal_machine.step() || reveal_machine.frame().ip != reveal_ip + 4) {
        std::fprintf(stderr, "0x1ce bit 30 held the dispatch (ip=%zu)\n",
                     reveal_machine.frame().ip);
        return false;
    }
    auto reveal_zero = make_script_with_body();
    emit_instruction(reveal_zero, 0x1CE, {{0, 0}});
    emit_instruction(reveal_zero, 2, {});
    core::Machine reveal_off = scene_step_machine(table);
    reveal_off.set_host_clock_ms(9);
    reveal_off.load_root(reveal_zero);  // 开关 0 且第 30 位未置 ⇒ 空操作
    if (!reveal_off.step() || (reveal_off.state_flags() & (1u << 30)) != 0 ||
        reveal_off.text_layout_release_requests() != 0) {
        std::fprintf(stderr, "0x1ce zero-branch was not a no-op\n");
        return false;
    }
    // 门字非 0、第 27 位由产品路径 `0x88(1) ⇒ 0x19c` 置起 ⇒ 该位有服务体（一次消息处理，服务后 =
    // **交回迭代头**）⇒ 本迭代**不派发、也不停死**（⛔ 旧模型把这种情形判成 `GateStalled`）。
    auto stall_script = make_script_with_body();
    emit_instruction(stall_script, 0x88, {{0, 1}});   // C/D ← 1（C 的产品写者）
    emit_instruction(stall_script, 0x19C, {});        // C 支 ⇒ 置第 27 位
    emit_instruction(stall_script, 0x2F7, {{0, 0}});  // 位 27 不交回时才轮得到它
    emit_instruction(stall_script, 2, {});
    core::Machine stall_machine(0x12345678u, table);
    stall_machine.load_root(stall_script);
    if (!stall_machine.step() || !stall_machine.step() ||
        (stall_machine.state_flags() & (1u << 27)) == 0) {
        std::fprintf(stderr, "gate fixture did not set bit 27\n");
        return false;
    }
    const std::size_t held_ip = stall_machine.frame().ip;    for (int i = 0; i < 10001 && !stall_machine.halted(); ++i) stall_machine.step();
    if (stall_machine.halted() ||
        stall_machine.last_stop().reason == core::StopReason::GateStalled) {
        std::fprintf(stderr, "bit 27 was treated as a stall (halted=%d reason=%d)\n",
                     stall_machine.halted() ? 1 : 0,
                     static_cast<int>(stall_machine.last_stop().reason));
        return false;
    }
    if (stall_machine.frame().ip != held_ip || stall_machine.voice_option_2f7(0) != 0) {
        std::fprintf(stderr, "bit 27 did not hold the iteration (ip=%zu opt=%d)\n",
                     stall_machine.frame().ip, stall_machine.voice_option_2f7(0));
        return false;
    }
    return true;
}
// ── 成对写（0x88）与模式族 C/D/E/F/G 的身份：CORE-OPS-SCALAR-PAIR-88 / CORE-OPS-MODE-BIT /
//    CORE-OPS-STATUS-CELL-READ（第 27 位 / C / D / E / G 必须是产品可达的同一组格） ──
bool check_pair_88_mode_cells(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "pair-88: %s\n", what);
        return false;
    };
    // ① 0x88(1)：同一个值写 C / D，G ← 1；非 0 支**不碰**第 27 位。0x19a 原样读 C（= 格 X）。
    auto script = make_script_with_body();
    emit_instruction(script, 0x88, {{0, 1}});
    emit_instruction(script, 0x1C7, {{9, 0}});   // 位 27 初值（应为 0）
    emit_instruction(script, 0x19A, {{9, 1}});   // C 的原值
    emit_instruction(script, 0x19C, {});          // C 非 0 ⇒ D←1、置第 27 位、G←1
    emit_instruction(script, 2, {});
    core::Machine machine(0x12345678u, table);
    machine.load_root(script);
    for (int i = 0; i < 3; ++i) {
        if (!machine.step()) return fail("0x88 / 0x1c7 / 0x19a 停机");
    }
    if (machine.mode_c() != 1 || machine.mode_d() != 1) {
        return fail("0x88(1) 没有把同一个值写进 C 与 D");
    }
    if (machine.mode_g() != 1) return fail("0x88(1) 没有把 G 写成常量 1");
    if ((machine.state_flags() & (1u << 27)) != 0) return fail("0x88 的非 0 支碰了第 27 位");
    if (machine.read_int(core::Operand{9, 0}) != 0) return fail("0x1c7 读到的初值不是 0");
    if (machine.read_int(core::Operand{9, 1}) != 1) return fail("0x19a 读到的 C 不是 0x88 写的值");
    // ② 0x19c 必须走「0x88 写出来的 C」那一支（C 支置 D、置第 27 位、G ← 1）。
    if (!machine.step()) return fail("0x19c 停机");
    if (machine.mode_d() != 1 || machine.mode_g() != 1 ||
        (machine.state_flags() & (1u << 27)) == 0) {
        return fail("0x19c 没有走 0x88 写出来的 C 支（D / G / 第 27 位）");
    }
    // ③ 第 27 位置起后 0x1c7 读回 1；再跑 0x88(1) 不得清位（只有为 0 支清位）。
    machine.dispatch_for_test(core::Instruction{0, 0x1C7, {core::Operand{9, 2}}});
    if (machine.read_int(core::Operand{9, 2}) != 1) return fail("0x1c7 没把置起的第 27 位读成 1");
    machine.dispatch_for_test(core::Instruction{0, 0x88, {core::Operand{0, 1}}});
    if ((machine.state_flags() & (1u << 27)) == 0) {
        return fail("0x88(1) 把已置的第 27 位清掉了（只有为 0 支才清）");
    }
    if (machine.mode_c() != 1 || machine.mode_d() != 1) {
        return fail("0x88(1) 第二次没有写 C / D");
    }
    // ④ 0x88(0)：C / D ← 0、清第 27 位；G 不动（仍 1）。
    machine.dispatch_for_test(core::Instruction{0, 0x88, {core::Operand{0, 0}}});
    if ((machine.state_flags() & (1u << 27)) != 0) return fail("0x88(0) 没有清第 27 位");
    if (machine.mode_c() != 0 || machine.mode_d() != 0) return fail("0x88(0) 没有把 C / D 写成 0");
    if (machine.mode_g() != 1) return fail("0x88(0) 不该清 G");
    // ⑤ E 就是 0x1cc 读的状态格（与 0x19c 的第二个判据同一格）；E 支不碰 D。
    machine.seed_mode_cells(1, 0);
    machine.dispatch_for_test(core::Instruction{0, 0x1CC, {core::Operand{9, 3}}});
    if (machine.read_int(core::Operand{9, 3}) != 1) {
        return fail("0x1cc 读的不是 0x19c 用的 E 格");
    }
    machine.dispatch_for_test(core::Instruction{0, 0x19C, {}});
    if ((machine.state_flags() & (1u << 27)) == 0) return fail("0x19c 的 E 支没有置第 27 位");
    if (machine.mode_d() != 0) return fail("0x19c 的 E 支碰了 D（spec：E 支不动 D）");
    return true;
}

// ── 第 27 位服务体（CORE-VM-GATE / CORE-VM-STATE 位序表第 16 行）：
//    置起 ⇒ 一次消息处理（seam 计数 +1）→ 让出一拍 → 窥视 → 交回迭代头（本迭代不派发） ──
bool check_gate_bit27_service(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "bit27-service: %s\n", what);
        return false;
    };
    auto script = make_script_with_body();
    emit_instruction(script, 0x88, {{0, 1}});   // C / D ← 1
    emit_instruction(script, 0x19C, {});        // C 支 ⇒ 置第 27 位
    emit_instruction(script, 0x2F7, {{0, 0}});  // 被挡住 ⇒ 不该执行
    emit_instruction(script, 2, {});
    core::Machine machine(0x12345678u, table);
    int processed = 0;
    int yielded = 0;
    int peeked = 0;
    core::Machine::HostMessageIo io;
    io.process_one = [&] { ++processed; };
    io.yield = [&] { ++yielded; };
    io.peek = [&] {
        ++peeked;
        return false;
    };
    machine.set_host_message_io(io);
    machine.load_root(script);
    if (!machine.step() || !machine.step()) return fail("0x88 / 0x19c 停机");
    if ((machine.state_flags() & (1u << 27)) == 0) return fail("fixture 没有置起第 27 位");
    const std::size_t held_ip = machine.frame().ip;
    const std::size_t before = machine.host_message_steps();
    if (!machine.step()) return fail("第 27 位服务体那一拍停机");
    if (machine.frame().ip != held_ip) return fail("第 27 位服务体派发了脚本指令（ip 前进了）");
    if (machine.host_message_steps() != before + 1) {
        return fail("第 27 位服务体没有计入一次消息处理");
    }
    if (processed != 1 || yielded != 1 || peeked != 1) {
        return fail("消息处理 / 让出一拍 / 窥视 不是各一次");
    }
    if (machine.voice_option_2f7(0) != 0) return fail("被挡住的 0x2f7 执行了");
    // 清位之后必须恢复派发（证明「ip 不变」不是因为机器停死）。
    machine.dispatch_for_test(core::Instruction{0, 0x88, {core::Operand{0, 0}}});
    if (!machine.step() || machine.frame().ip != held_ip + 12) {  // 0x2f7 自己把推进格数写成 3
        return fail("清掉第 27 位之后没有恢复派发");
    }
    if (machine.voice_option_2f7(0) != 1) return fail("0x2f7 在恢复派发后没有执行");
    if (machine.host_message_steps() != before + 1) {
        return fail("非第 27 位服务体的迭代也计了消息处理");
    }
    return true;
}

// ── 第 27 位的产品置点：0x71 的子系统调用返回非 0 / 追加前置支（ReadTextSkip ∧ 已读命中）/
//    0x305 ②（同一判据）；置起后本迭代不派发 ──
bool check_adv_text_bit27_set_points(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "adv-text-bit27: %s\n", what);
        return false;
    };
    // ① 0x71：文本子系统一次调用返回非 0 ⇒ 置第 27 位。
    auto request = make_script_with_body();
    emit_instruction(request, 0x80, {{0, 9}});
    emit_instruction(request, 0x70, {{0, 9}, {0, 50}, {0, 50}, {0, 0}, {0, 0}});
    emit_instruction(request, 0x71, {{0, 9}});
    emit_instruction(request, 0x2F7, {{0, 0}});
    emit_instruction(request, 2, {});
    core::Machine request_machine(0x12345678u, table);
    request_machine.set_text_request_result([](std::int32_t) { return 1; });
    request_machine.load_root(request);
    for (int i = 0; i < 3; ++i) {
        if (!request_machine.step()) return fail("0x80 / 0x70 / 0x71 停机");
    }
    if ((request_machine.state_flags() & (1u << 27)) == 0) {
        return fail("0x71 的子系统调用返回非 0 却没有置第 27 位");
    }
    const std::size_t request_ip = request_machine.frame().ip;
    const std::size_t request_steps = request_machine.host_message_steps();
    if (!request_machine.step() || request_machine.frame().ip != request_ip ||
        request_machine.host_message_steps() != request_steps + 1 ||
        request_machine.voice_option_2f7(0) != 0) {
        return fail("0x71 置起的第 27 位没有挡住本迭代的派发");
    }

    // ② 追加前置支（0x6e）：ReadTextSkip 非 0 ∧ 已读命中 ⇒ 置第 27 位，且**不置打字位**、同帧抽干。
    auto append = make_script_with_body();
    emit_instruction(append, 0x80, {{0, 9}});
    emit_instruction(append, 0x70, {{0, 9}, {0, 50}, {0, 50}, {0, 0}, {0, 0}});
    emit_instruction(append, 0x74, {{0, 1000}});  // 每字间隔 ≠ 0：没有前置支时一定会置第 29 位
    emit_instruction(append, 0x71, {{0, 9}});
    emit_instruction(append, 0x6E, {{0, 0}, {5, 0}});
    emit_instruction(append, 0x2F7, {{0, 0}});
    emit_instruction(append, 2, {});
    core::Machine append_machine(0x12345678u, table);
    append_machine.config_mut().set_int("message:ReadTextSkip", 1);
    append_machine.set_read_text_skip_hit([] { return true; });
    append_machine.write_text(core::Operand{5, 0}, "AB");
    append_machine.load_root(append);
    for (int i = 0; i < 5; ++i) {  // 0x80 / 0x70 / 0x74 / 0x71 / 0x6e
        if (!append_machine.step()) return fail("追加前置支的夹具停机");
    }
    if ((append_machine.state_flags() & (1u << 27)) == 0) {
        return fail("追加前置支没有置第 27 位");
    }
    if (append_machine.text_typing()) return fail("追加前置支置位后仍置了打字位（第 29 位）");
    if (append_machine.pending_glyph_count() != 0) return fail("追加前置支没有走同帧抽干");
    const std::size_t append_ip = append_machine.frame().ip;
    const std::size_t append_steps = append_machine.host_message_steps();
    if (!append_machine.step() || append_machine.frame().ip != append_ip ||
        append_machine.host_message_steps() != append_steps + 1) {
        return fail("追加前置支置起的第 27 位没有挡住本迭代的派发");
    }

    // ③ 0x305 ②：同一判据（ReadTextSkip 非 0 ∧ 已读命中）⇒ 置第 27 位。
    //    ★ 命中接缝先返回 false（0x6e 那一步不置位），到 0x305 之前再打开。
    auto block = make_script_with_body();
    emit_instruction(block, 0x80, {{0, 9}});
    emit_instruction(block, 0x70, {{0, 9}, {0, 50}, {0, 50}, {0, 0}, {0, 0}});
    emit_instruction(block, 0x74, {{0, 0}});  // 每字间隔 = 0 ⇒ 追加立即抽干（不置第 29 位）
    emit_instruction(block, 0x71, {{0, 9}});
    emit_instruction(block, 0x304, {});
    emit_instruction(block, 0x6E, {{0, 0}, {5, 0}});
    emit_instruction(block, 0x305, {});
    emit_instruction(block, 0x2F7, {{0, 0}});
    emit_instruction(block, 2, {});
    core::Machine block_machine(0x12345678u, table);
    bool read_hit = false;
    block_machine.config_mut().set_int("message:ReadTextSkip", 1);
    block_machine.set_read_text_skip_hit([&read_hit] { return read_hit; });
    block_machine.write_text(core::Operand{5, 0}, "AB");
    block_machine.load_root(block);
    for (int i = 0; i < 6; ++i) {  // 0x80 / 0x70 / 0x74 / 0x71 / 0x304 / 0x6e
        if (!block_machine.step()) return fail("0x305 夹具停机");
    }
    if ((block_machine.state_flags() & (1u << 27)) != 0) {
        return fail("0x6e 在已读命中为假时就置了第 27 位");
    }
    read_hit = true;
    if (!block_machine.step()) return fail("0x305 停机");
    if ((block_machine.state_flags() & (1u << 27)) == 0) {
        return fail("0x305 的 ② 没有在 ReadTextSkip ∧ 已读命中时置第 27 位");
    }
    const std::size_t block_ip = block_machine.frame().ip;
    if (!block_machine.step() || block_machine.frame().ip != block_ip ||
        block_machine.voice_option_2f7(0) != 0) {
        return fail("0x305 置起的第 27 位没有挡住本迭代的派发");
    }
    return true;
}

// ── 逐格揭示当前文本（0x1CE；`CORE-OPS-TEXT-CELL-REVEAL-1CE`） ──
// ★ 守卫必须能红的地方：启用把格号归 0（不是保留）、相位起点 = 本迭代的宿主毫秒时钟（不是"种子"）、
//   到点一拍**只**画一格且格号 +1 取模、**画的是推进前的格号**（`text_cell_last_draw_cursor()`）、
//   掉拍不补（跨多拍也只画一格）、停用**不复位**格号与相位但**仍写开关格**、节拍周期**不被本条复位**、
//   格数 0 时**不推进**（不夹成 1）、`0x72` 尾段的四个写点（格数 / 格号 / 相位起点 / 节拍计数）、
//   重绘待处理位置起时停用**不**请求释放。
// 一拍的门前段 + 第 30 位的服务体：`CORE-VM-STATE` 位序表第 8 项的服务后 = **继续扫描**
// ⇒ 逐格揭示的服务体是**门上的服务体**（不是"时钟推进"的一部分）。本夹具只喂"一拍"给它；
// 门的扫描 + 扫到表尾照常派发由 `check_gate_chain` 咬（⛔ 别把两件事混在一条里）。
void tick_cell_reveal(core::Machine& machine, std::uint32_t now_ms) {
    machine.advance_scene(now_ms);
    machine.service_gate_bit_for_test(30);}bool check_text_cell_reveal(const core::OpcodeTable& table) {
    const auto fail = [](const char* why) {
        std::fprintf(stderr, "0x1ce cell-reveal: %s\n", why);
        return false;
    };
    core::Machine machine(0x12345678u, table);
    tick_cell_reveal(machine, 1234);            // 本迭代的时钟读数（引擎每迭代读一次）
    machine.seed_reveal_cells(2, 3);        // 先让格号非 0、格数 = 3（否则取模推进看不出来）
    machine.dispatch_for_test(core::Instruction{0, 0x1CE, {{0, 7}}});
    if ((machine.state_flags() & (1u << 30)) == 0) return fail("非 0 支没有置活动位（第 30 位）");
    if (machine.reveal_cursor() != 0) return fail("非 0 支没有把当前格号归 0");
    if (machine.reveal_switch() != 7) return fail("开关格没有记下操作数");
    if (machine.reveal_tick_start_ms() != 1234u) return fail("相位起点不是本迭代的宿主毫秒时钟");
    if (machine.reveal_tick_counter() != 1) return fail("相位重置后节拍计数不是 1");
    // ★ 节拍长度**不是本条的写点**（规范：本条不复位它）。判据必须是"播种成 300 之后仍是 300"：
    //   ⛔ 别写成"读一次初值 500"—— 那在没有写者时是**恒真断言**（把实现改成写死 500 也不会红）。
    {
        core::Machine seeded(0x12345678u, table);
        seeded.seed_reveal_tick_period_ms(300);
        tick_cell_reveal(seeded, 700);
        seeded.dispatch_for_test(core::Instruction{0, 0x1CE, {{0, 3}}});
        if (seeded.reveal_tick_period_ms() != 300u) {
            return fail("非 0 支把节拍周期复位了（本条不写它 ⇒ 播种的 300 必须留着）");
        }
    }
    tick_cell_reveal(machine, 1733);  // 499 ms
    if (machine.text_cell_draw_requests() != 0 || machine.reveal_cursor() != 0) {
        return fail("不足一拍就画了格");
    }
    tick_cell_reveal(machine, 1734);  // 500 ms
    if (machine.text_cell_draw_requests() != 1) return fail("够一拍没有恰好请求画一格");
    // ★ 画的是**推进前**的格号：请求必须记下 0（推进之后才是 1）。
    //   把 `reveal_cursor_ = (reveal_cursor_ + 1) % reveal_count_` 挪到这条记录之前 ⇒ 这里读到 1 ⇒ 红。
    if (machine.text_cell_last_draw_cursor() != 0) {
        return fail("画格请求记的不是推进前的格号（记录点在推进之后）");
    }
    if (machine.reveal_cursor() != 1) return fail("一拍之后格号没有 +1（画的应是推进前的格号）");
    tick_cell_reveal(machine, 1734);  // 同一毫秒再服务
    if (machine.text_cell_draw_requests() != 1) return fail("同一毫秒被画了两次（一次服务最多一格）");
    tick_cell_reveal(machine, 2234);  // 第二拍
    if (machine.text_cell_draw_requests() != 2 || machine.reveal_cursor() != 2) {
        return fail("第二拍没有恰好画一格");
    }
    if (machine.text_cell_last_draw_cursor() != 1) return fail("第二拍画的不是推进前的格号 1");
    tick_cell_reveal(machine, 2734);  // 第三拍：格号 2 → (2+1) mod 3 = 0
    if (machine.reveal_cursor() != 0) return fail("格号没有按格数取模回 0");
    // ── 掉拍不补：一次服务跨 3 拍（1500 ms / 500 ms 周期）也只能画 **1** 格、格号只 +1 ──
    //   ⛔ 把服务改成"追赶式"（按 ticks 逐格补画）⇒ 这里红。
    {
        core::Machine catchup(0x12345678u, table);
        tick_cell_reveal(catchup, 1000);
        catchup.seed_reveal_cells(0, 3);
        catchup.dispatch_for_test(core::Instruction{0, 0x1CE, {{0, 1}}});
        tick_cell_reveal(catchup, 2500);  // 跨 3 拍（2500 − 1000 = 1500 = 3 × 500）
        if (catchup.text_cell_draw_requests() != 1) {
            return fail("跨 3 拍补画了多格（掉拍不补 ⇒ 一次服务最多画一格）");
        }
        if (catchup.reveal_cursor() != 1) return fail("跨 3 拍把格号推进了多格（掉拍不补）");
        tick_cell_reveal(catchup, 2500);  // 同一毫秒再服务：本拍已消费 ⇒ 不补
        if (catchup.text_cell_draw_requests() != 1) return fail("同一毫秒的重复服务又画了一格");
        tick_cell_reveal(catchup, 3000);  // 下一拍（1000 + 4 × 500）
        if (catchup.text_cell_draw_requests() != 2 || catchup.reveal_cursor() != 2) {
            return fail("跨拍之后没有恢复「每拍一格」");
        }
    }
    // 停用（0 且活动位已置）⇒ 清位；**格号与相位都不复位**；**开关格两条支都写**；非「重绘待处理」⇒ 请求一次释放。
    machine.seed_reveal_cells(2, 3);
    const auto phase_before = machine.reveal_tick_start_ms();
    machine.dispatch_for_test(core::Instruction{0, 0x1CE, {{0, 0}}});
    if ((machine.state_flags() & (1u << 30)) != 0) return fail("0 支没有清活动位");
    if (machine.reveal_cursor() != 2) return fail("停用把当前格号复位了");
    if (machine.reveal_tick_start_ms() != phase_before) return fail("停用把节拍器相位复位了");
    // ★ 开关格在**两条支都写**（引擎在分支之前就记下操作数）：停用支执行后必须变成 0（之前是 7）。
    //   把 `reveal_switch_ = value;` 挪进 `if (value != 0)` ⇒ 这里读到 7 ⇒ 红。
    if (machine.reveal_switch() != 0) return fail("停用支没有写开关格（写点被挪进了 if (value != 0)）");
    if (machine.text_layout_release_requests() != 1) {
        return fail("非重绘待处理时停用没有请求释放排版");
    }
    machine.dispatch_for_test(core::Instruction{0, 0x1CE, {{0, 0}}});
    if (machine.text_layout_release_requests() != 1) return fail("第 30 位未置时的 0 支不是空操作");
    // 再启用 ⇒ 格号回 0。
    machine.dispatch_for_test(core::Instruction{0, 0x1CE, {{0, 1}}});
    if (machine.reveal_cursor() != 0) return fail("再启用没有把格号归 0");
    // 「重绘待处理」（第 20 位）已置 ⇒ 停用**不**请求释放（这份排版马上要重画）。
    core::Machine pending(0x12345678u, table);
    tick_cell_reveal(pending, 100);
    pending.dispatch_for_test(core::Instruction{0, 0x1CE, {{0, 5}}});
    pending.set_state_flags_for_test(0x100000u);
    pending.dispatch_for_test(core::Instruction{0, 0x1CE, {{0, 0}}});
    if ((pending.state_flags() & (1u << 30)) != 0) return fail("重绘待处理时停用没有清活动位");
    if (pending.text_layout_release_requests() != 0) {
        return fail("重绘待处理置起时停用仍请求了释放");
    }
    // 格数 0（从未由排版侧写入）⇒ 每拍照画一格，但格号**不推进**（⛔ 不夹成 1）。
    core::Machine zero(0x12345678u, table);
    zero.dispatch_for_test(core::Instruction{0, 0x1CE, {{0, 2}}});
    tick_cell_reveal(zero, 500);
    tick_cell_reveal(zero, 1000);
    if (zero.text_cell_draw_requests() != 2) return fail("格数 0 时没有每拍请求画一格");
    if (zero.reveal_cursor() != 0) return fail("格数 0 时格号被推进了（不许夹成 1）");
    // ── `0x72` 尾段的同一批写点（`CORE-OPS-MSG-WAIT-INPUT-72` 的尾段 = 本条的启停那一侧）──
    //   引擎以"按当前文本取一次格数"的绘制入口取回**格数**，并把当前格号归 0、节拍器相位重置
    //   （起点 ← 当时的场景时钟、计数从 1 起算）。**删掉 vm_ops_texture.cpp 里那 4 行里的任何一行** ⇒ 这里红。
    //   ★ 判据用"四格先都写成别的值"的方式咬住每个写点（格数 7 / 格号 4 / 起点 1000 / 计数 9）。
    {
        core::Machine tail(0x12345678u, table);
        tail.write_text(core::Operand{5, 0}, "AB");
        tail.set_host_clock_ms(1000);
        tick_cell_reveal(tail, 1000);
        tail.dispatch_for_test(core::Instruction{0, 0x80, {{0, 9}}});
        tail.dispatch_for_test(
            core::Instruction{0, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 5}, {0, 5}}});
        tail.dispatch_for_test(core::Instruction{0, 0x75, {{0, 20}}});
        tail.dispatch_for_test(core::Instruction{0, 0x74, {{0, 0}}});
        tail.dispatch_for_test(core::Instruction{0, 0x71, {{0, 9}}});
        tail.dispatch_for_test(core::Instruction{0, 0x6E, {{0, 9}, {5, 0}}});
        const auto cells = tail.text_layer_cell_count(9);
        if (cells == 0) {
            std::fprintf(stderr, "0x72 尾段夹具没有排出文本格（0x6e 追加后格数 0）\n");
            return false;
        }
        // 启用（起点 ← 1000、计数 1），再把格数 / 格号播种成"不等于期望"，并推进几拍把计数抬起来。
        tail.dispatch_for_test(core::Instruction{0, 0x1CE, {{0, 1}}});
        tail.seed_reveal_cells(2, 7);
        tick_cell_reveal(tail, 1501);  // 一拍 ⇒ 计数 2、格号 3
        tick_cell_reveal(tail, 5000);  // 计数 9、格号 4
        if (tail.reveal_count() != 7 || tail.reveal_cursor() != 4 ||
            tail.reveal_tick_start_ms() != 1000u || tail.reveal_tick_counter() != 9) {
            std::fprintf(stderr,
                         "0x72 尾段夹具的四格没有先偏离期望值（count=%d cursor=%d start=%u counter=%u）\n",
                         tail.reveal_count(), tail.reveal_cursor(), tail.reveal_tick_start_ms(),
                         tail.reveal_tick_counter());
            return false;
        }
        tail.dispatch_for_test(core::Instruction{0, 0x72, {{0, 9}}});
        if (tail.reveal_count() != static_cast<std::int32_t>(cells)) {
            std::fprintf(stderr, "0x72 没有把格数写成当前文本层的格数（reveal_count=%d cells=%zu）\n",
                         tail.reveal_count(), cells);
            return false;
        }
        if (tail.reveal_cursor() != 0) {
            std::fprintf(stderr, "0x72 没有把当前格号归 0（cursor=%d）\n", tail.reveal_cursor());
            return false;
        }
        if (tail.reveal_tick_start_ms() != tail.scene_clock_ms()) {
            std::fprintf(stderr, "0x72 的节拍器相位起点不是当时的场景时钟（start=%u now=%u）\n",
                         tail.reveal_tick_start_ms(), tail.scene_clock_ms());
            return false;
        }
        if (tail.reveal_tick_counter() != 1) {
            std::fprintf(stderr, "0x72 没有把节拍计数重置成 1（counter=%u）\n",
                         tail.reveal_tick_counter());
            return false;
        }
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
    core::Machine bracket_machine = scene_step_machine(table);
    bracket_machine.set_host_clock_ms(1000);
    bracket_machine.load_root(bracket);
    if (!bracket_machine.step() || !bracket_machine.bracket_busy() ||
        bracket_machine.bracket_depth() != 0) {
        std::fprintf(stderr, "0x1f4 enter mismatch\n");
        return false;
    }
    bracket_machine.set_host_clock_ms(1040);
    if (!bracket_machine.step() || bracket_machine.bracket_depth() != 1) {
        std::fprintf(stderr, "0x1f4 busy branch counted the nest wrong\n");
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
    core::Machine lone_machine = scene_step_machine(table);
    lone_machine.reset_scene_clock(0);
    lone_machine.set_host_clock_ms(2000);
    lone_machine.load_root(lone_leave);
    if (!lone_machine.step() || lone_machine.bracket_busy()) {
        std::fprintf(stderr, "0x1f5 unpaired leave was not a no-op\n");
        return false;
    }
    // ★ `0x1f4` 的时钟写点（`CORE-TIMING-CLOCK`：`0x41A0CF` / `0x41A0D5`，**仅不忙时**）：
    //   不忙 ⇒ 先搬旧（上一次 ← 旧现在）、再写新（现在 ← 本次读数）；忙 ⇒ 只 +1，**两格都不动**。
    //   ★ 用直接派发（不经过 `step()` 的迭代头盖钟），否则"迭代头盖过一次"会把两格都动掉、判不出来。
    {
        core::Machine writer = scene_step_machine(table);
        writer.reset_scene_clock(500);
        writer.advance_scene(600);  // 现在 = 600、上一步 = 500（两格不同才能看出"搬旧"）
        writer.set_host_clock_ms(700);
        writer.dispatch_for_test(core::Instruction{0, 0x1F4, {}});
        if (!writer.bracket_busy() || writer.scene_clock_ms() != 700u ||
            writer.scene_previous_ms() != 600u) {
            std::fprintf(stderr,
                         "0x1f4 没有按「先搬旧再写新」写场景时钟对（now=%u prev=%u）\n",
                         writer.scene_clock_ms(), writer.scene_previous_ms());
            return false;
        }
        writer.set_host_clock_ms(900);
        writer.dispatch_for_test(core::Instruction{0, 0x1F4, {}});  // 忙支：只 +1
        if (writer.bracket_depth() != 1 || writer.scene_clock_ms() != 700u ||
            writer.scene_previous_ms() != 600u) {
            std::fprintf(stderr, "0x1f4 忙支动了时钟对 / 没有只 +1（now=%u prev=%u depth=%d）\n",
                         writer.scene_clock_ms(), writer.scene_previous_ms(), writer.bracket_depth());
            return false;
        }
    }
    // 交叉判据：区间忙标记与 `0x20c` 的「别处驱动」是**同一格** ⇒ 忙时段里 `0x20c` 不写时钟对
    // （请求 / 清帧标志照做，见 `CORE-OPS-FRAME-STEP-20C`）。
    // 若实现把它们当成两格，这条会红（而单看任何一条都"看起来正常"）。
    {
        core::Machine busy_machine = scene_step_machine(table);
        busy_machine.reset_scene_clock(500);
        busy_machine.advance_scene(600);
        busy_machine.set_driven_elsewhere(true);  // 区间忙标记（与 0x1f4 同一格）
        busy_machine.set_host_clock_ms(1040);
        busy_machine.dispatch_for_test(core::Instruction{0, 0x20C, {}});
        if (busy_machine.scene_clock_ms() != 600u || busy_machine.scene_previous_ms() != 500u ||
            !busy_machine.frame_step_requested()) {
            std::fprintf(stderr,
                         "0x20c 在忙时段写了时钟对（now=%u prev=%u）或没发请求\n",
                         busy_machine.scene_clock_ms(), busy_machine.scene_previous_ms());
            return false;
        }
        // 不忙 ⇒ 同一条指令**两格都动**（先搬旧、再写新）。
        busy_machine.set_driven_elsewhere(false);
        busy_machine.set_host_clock_ms(1100);
        busy_machine.dispatch_for_test(core::Instruction{0, 0x20C, {}});
        if (busy_machine.scene_clock_ms() != 1100u || busy_machine.scene_previous_ms() != 600u) {
            std::fprintf(stderr, "0x20c 不忙时没有按「先搬旧再写新」写（now=%u prev=%u）\n",
                         busy_machine.scene_clock_ms(), busy_machine.scene_previous_ms());
            return false;
        }
    }
    return true;}
// ── 待处理命令队列 / 泵 / `0x143` 投递 / `0x2`(−10) 收场 / 宿主消息泵节流 ──
/** 守的是 `CORE-OPS-RUN-BRACKET`（`70-vm.md:295-346`）、`CORE-OPS-EVENT-143`（`60-ops-scalar.md:169-199`）与
 *  `CORE-VM-LOOP`（`70-vm.md:8` / `:27`）。每条断言都指向 spec 句（见各段注释），不是"当前实现的取值"：
 *  · 泵入口两判按序（区间忙 / 空）—— `70-vm.md:306`；
 *  · 正命令 ⇒ 当前帧 37、父帧号 −10、长度槽 0（`70-vm.md:308-309`）；
 *  · 负命令 ⇒ 当前帧 = −命令、父帧号 −10；目标帧没装载脚本 ⇒ 显示消息型异常（`:310`）；
 *  · 泵帧收场五步（`:312`）；
 *  · `0x143` 四件事 + 投递值 = 格号 << 24 + `ip` 只 +4（`60-ops-scalar.md:173-183`）；
 *  · 节流 = 泵一次后隔 3 个迭代（`70-vm.md:27` / `:45`）。 */
bool check_command_queue(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "command-queue: %s\n", what);
        return false;
    };
    const std::uint32_t key = 0x12345678u;
    // 装载器夹具：四份脚本（统一文件 id 0..3；松文件优先）。帧 37 里跑的脚本由正命令指定。
    const auto root_dir = std::filesystem::temp_directory_path() / "amayui-cxx-selftest-command-queue";
    std::filesystem::create_directories(root_dir);
    auto plain = make_script_with_body();
    emit_instruction(plain, 0x1A8, {});
    emit_instruction(plain, 2, {});
    auto pump_exit = make_script_with_body();  // id 1：泵帧先执行 0x2（收场五步）
    emit_instruction(pump_exit, 2, {});
    emit_instruction(pump_exit, 0x1A8, {});
    auto nested_bracket = make_script_with_body();  // id 2：泵帧里再进 / 出一次区间
    emit_instruction(nested_bracket, 0x1F4, {});
    emit_instruction(nested_bracket, 0x1F5, {});
    emit_instruction(nested_bracket, 0x1A8, {});
    emit_instruction(nested_bracket, 2, {});
    auto plain_target = make_script_with_body();  // id 3：负命令的目标帧 / 链式再泵的目标
    emit_instruction(plain_target, 0x1A8, {});
    emit_instruction(plain_target, 2, {});
    const auto write_script = [&](const char* name, const std::vector<std::uint8_t>& bytes) {
        std::ofstream out(root_dir / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),                  static_cast<std::streamsize>(bytes.size()));
    };
    write_script("QUEUE0.BIN", plain);
    write_script("QUEUE1.BIN", pump_exit);
    write_script("QUEUE2.BIN", nested_bracket);
    write_script("QUEUE3.BIN", plain_target);
    core::Install install;
    install.root = root_dir;
    install.index.files.push_back(core::AlfFile{"QUEUE0.BIN", 0, 0, 0, 0});
    install.index.files.push_back(core::AlfFile{"QUEUE1.BIN", 0, 0, 0, 0});
    install.index.files.push_back(core::AlfFile{"QUEUE2.BIN", 0, 0, 0, 0});
    install.index.files.push_back(core::AlfFile{"QUEUE3.BIN", 0, 0, 0, 0});
    const auto bracket_script = [&](std::vector<std::uint8_t>& script) {
        emit_instruction(script, 0x1F4, {});
        emit_instruction(script, 0x1F5, {});
    };
    // ① 泵入口两判 + 空命令（`70-vm.md:306`）：空队列 ⇒ 泵不改任何格；0 = 只前进头。
    {
        auto script = make_script_with_body();
        bracket_script(script);
        emit_instruction(script, 0x2, {});
        core::Machine machine(key, table, &install);
        machine.load_root(script);
        if (!machine.step() || !machine.bracket_busy()) return fail("0x1f4 没有置忙");
        if (!machine.step()) return fail("空队列的 0x1f5 停机了");
        if (machine.bracket_busy()) return fail("0x1f5 没有清忙");
        if (machine.pump_busy()) return fail("空队列让泵置了队列泵占用标记");
        if (machine.suspended_frame() != -1) return fail("空队列让泵记了挂起帧号");
        if (machine.status_restore() != 0) return fail("空队列让泵写了待恢复副本");
        if (machine.frame_at(core::Machine::kPumpFrameSlot).loaded) {
            return fail("空队列的泵动了帧 37（入口判空失效）");
        }
        if (machine.current_frame() != 0 || machine.pending_command_count() != 0) {
            return fail("空队列的泵换了帧或留下命令");
        }
        if (machine.frame().ip != 8) return fail("0x1f5 没有把指令指针推进 4 字节");
    }
    {
        auto script = make_script_with_body();
        bracket_script(script);
        emit_instruction(script, 0x2, {});
        core::Machine machine(key, table, &install);
        machine.load_root(script);
        if (!machine.step()) return fail("0x1f4 停机");
        machine.enqueue_command_for_test(0);  // 空命令：头已前进、此外什么都不做
        if (machine.pending_command_count() != 1) return fail("测试口没有入队");
        if (!machine.step()) return fail("空命令的 0x1f5 停机了");
        if (machine.pending_command_count() != 0) return fail("空命令没有被取走");
        if (machine.pump_busy() || machine.suspended_frame() != -1 ||
            machine.status_restore() != 0) {
            return fail("空命令做「挂起」以外的多余动作");
        }
        if (machine.current_frame() != 0) return fail("空命令换了当前帧");
    }
    // ② 正命令（`70-vm.md:308-309`）：当前帧号 ← 37、装进帧 37、父帧号 = −10、ip = 代码区起点、
    //    长度槽 = 0；挂起三件（挂起帧号 / 待恢复副本 / 队列泵占用标记）同时成立，且**不恢复**调用帧。
    {
        auto script = make_script_with_body();
        bracket_script(script);
        emit_instruction(script, 0x2, {});
        core::Machine machine(key, table, &install);
        machine.state_flags_mut() = (1u << 15);  // 表外的位不挡派发；这里只用来验"整字进副本"
        machine.load_root(script);
        if (!machine.step()) return fail("0x1f4 停机");
        machine.enqueue_command_for_test(1);  // 正命令 = 统一文件 id 1
        if (!machine.step()) return fail("正命令的 0x1f5 停机了");
        if (machine.current_frame() != core::Machine::kPumpFrameSlot) {
            return fail("正命令没有把当前帧号 ← 37");
        }
        const auto& pump_frame = machine.frame_at(core::Machine::kPumpFrameSlot);
        if (!pump_frame.loaded || pump_frame.script_id != 1) {
            return fail("正命令没有把统一文件 id 装进帧 37");
        }
        if (pump_frame.parent != core::Machine::kPumpParentSentinel) {
            return fail("泵帧父帧号不是 −10（装载器没有照抄「父帧候选」格 / 泵没预置）");
        }
        if (pump_frame.ip != 0) return fail("泵帧指令指针不是代码区起点");
        if (pump_frame.length_slot != 0) return fail("泵帧长度槽不是 0（分派器会推进）");
        if (machine.suspended_frame() != 0) return fail("没有记住挂起帧号（= 调用泵的那一帧）");
        if (machine.status_restore() != (1u << 15)) {
            return fail("状态选择字没有整字存进待恢复副本");
        }
        if (machine.state_flags() != 0) return fail("挂起时没有把活动状态字清 0（门该打开）");
        if (!machine.pump_busy()) return fail("没有置队列泵占用标记");
        if (machine.pending_command_count() != 0) return fail("命令没有被取走（一趟取一条）");
    }
    // ③ 泵帧收场五步（`70-vm.md:312`）：① 接回挂起帧 ② 它的长度槽 ← 0 ③ 状态字 ← 副本，
    //    第 14 位取**活动字**的值、第 21 位清 0 ④ 标记 ← 0 ⑤ 队列非空 ⇒ 立即再泵一条。
    {
        auto script = make_script_with_body();
        bracket_script(script);
        emit_instruction(script, 0x2, {});
        core::Machine machine(key, table, &install);
        machine.load_root(script);
        if (!machine.step()) return fail("收场夹具 0x1f4 停机");
        machine.enqueue_command_for_test(1);
        if (!machine.step()) return fail("收场夹具 0x1f5 停机");
        if (machine.current_frame() != core::Machine::kPumpFrameSlot) return fail("夹具没进泵帧");
        // 泵帧里执行 0x2 之前：摆出"活动字 / 副本"两格与挂起帧的长度槽。
        machine.state_flags_mut() = (1u << 14);  // 活动字：第 14 位置起
        machine.seed_status_restore_for_test((1u << 21) | (1u << 10) | (1u << 15));
        machine.seed_frame_length_for_test(0, 7);  // 挂起帧长度槽先非 0 ⇒ 证明②真的清了
        if (!machine.step()) return fail("泵帧的 0x2 停机了");
        if (machine.current_frame() != 0) return fail("① 没有接回挂起帧");
        if (machine.frame().length_slot != 0) return fail("② 挂起帧的长度槽没有清 0");
        const std::uint32_t expected = (1u << 14) | (1u << 10) | (1u << 15);
        if (machine.state_flags() != expected) {
            std::fprintf(stderr, "command-queue: ③ state=0x%X expected=0x%X\n",
                         machine.state_flags(), expected);
            return fail("③ 状态字并回不对（第 14 位该取活动字、第 21 位该清 0）");
        }
        if (machine.pump_busy()) return fail("④ 队列泵占用标记没有清 0");
        if (machine.pending_command_count() != 0) return fail("⑤ 空队列却取了命令");
        if (machine.halted()) return fail("泵帧的 0x2 被当成根结束");
    }
    {
        // ⑤ 链式再泵：队列非空 ⇒ 立即再泵一条（第二趟重新挂起 **刚接回** 的那一帧）。
        auto script = make_script_with_body();
        bracket_script(script);
        emit_instruction(script, 0x2, {});
        core::Machine machine(key, table, &install);
        machine.load_root(script);
        if (!machine.step()) return fail("链式夹具 0x1f4 停机");
        machine.enqueue_command_for_test(1);  // 第一趟：进泵帧
        if (!machine.step()) return fail("链式夹具 0x1f5 停机");
        machine.enqueue_command_for_test(3);  // 第二趟的目标（正命令 = 文件 id 3）
        if (!machine.step()) return fail("链式收场的 0x2 停机了");
        if (machine.pending_command_count() != 0) return fail("⑤ 没有立即再泵（命令还留在队列里）");
        if (machine.current_frame() != core::Machine::kPumpFrameSlot) {
            return fail("⑤ 没有把第二条命令取成新的泵帧");
        }
        if (machine.frame_at(core::Machine::kPumpFrameSlot).script_id != 3) {
            return fail("⑤ 取的不是队列里那一条命令");
        }
        if (machine.suspended_frame() != 0) return fail("第二趟没有重新挂起（嵌套泵的两格该被覆盖）");
        if (!machine.pump_busy()) return fail("第二趟没有置队列泵占用标记");
    }
    // ④ 负命令（`70-vm.md:310`）：当前帧号 ← −命令；父帧号 ← −10（泵直接写、不经过装载器）、
    //    ip ← 代码区起点、长度槽 ← 0；目标帧没有装载脚本 ⇒ 显示消息型异常且**停在目标帧**。
    {
        auto script = make_script_with_body();
        bracket_script(script);
        emit_instruction(script, 0x2, {});
        core::Machine machine(key, table, &install);
        machine.load_root(script);
        machine.load_frame_for_test(3, 2);  // 先装帧 2（文件 id 3）
        if (!machine.step()) return fail("负命令夹具 0x1f4 停机");
        machine.enqueue_command_for_test(-2);
        if (!machine.step()) return fail("负命令的 0x1f5 停机了");
        if (machine.current_frame() != 2) return fail("负命令没有把当前帧号 ← −命令");
        const auto& target = machine.frame_at(2);
        if (target.parent != core::Machine::kPumpParentSentinel) {
            return fail("负命令没有把目标帧父帧号写成 −10");
        }
        if (target.ip != 0) return fail("负命令没有把指令指针指到代码区起点");
        if (target.length_slot != 0) return fail("负命令没有把长度槽清 0");
        if (!machine.pump_busy()) return fail("负命令没有置队列泵占用标记");
        if (machine.suspended_frame() != 0) return fail("负命令没有记住挂起帧号");
    }
    {
        auto script = make_script_with_body();
        bracket_script(script);
        emit_instruction(script, 0x2, {});
        core::Machine machine(key, table, &install);
        machine.load_root(script);
        if (!machine.step()) return fail("负命令失败支 0x1f4 停机");
        machine.enqueue_command_for_test(-3);  // 帧 3 没有装载脚本
        if (machine.step()) return fail("泵到未装载的目标帧却没有失败");
        if (machine.last_stop().message.rfind("display-message:", 0) != 0) {
            std::fprintf(stderr, "command-queue: message=%s\n", machine.last_stop().message.c_str());
            return fail("目标帧没有装载脚本没有走显示消息型异常");
        }
        if (machine.current_frame() != 3) return fail("失败时当前帧号没有停在目标帧（不该恢复）");
    }
    // ⑤ `0x143`（`60-ops-scalar.md:173-183`）：空表 ⇒ ip 只 +4、标记清 0；非 0 格 ⇒
    //    投递值 = 格号 << 24（32 位有符号）、格 0 不投、逐格顺序、投递期不泵（标记挡下）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x143, {});
        emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 2, {});
        core::Machine machine(key, table, &install);
        machine.load_root(script);
        const std::size_t ip_before = machine.frame().ip;
        if (!machine.step()) return fail("空表的 0x143 停机了");
        if (machine.frame().ip != ip_before + 4) {
            std::fprintf(stderr, "command-queue: 0x143 ip=%zu before=%zu\n", machine.frame().ip,
                         ip_before);
            return fail("0x143 之后 ip 不是只 +4（推进被做了两次？）");
        }
        if (machine.pump_busy()) return fail("0x143 结束时没有把队列泵占用标记清回 0");
        if (machine.pending_command_count() != 0) return fail("空表也投了命令");
    }
    {
        // ⑤ 泵取走一条**非法正命令**（号不在索引里）⇒ 显示消息型异常，当前帧号停在 37。
        //    ★ 用**小索引**（0..3）播种 `5 << 24`；文件库登记接口不参与这条（它测的是泵的失败分档）。
        //    ★ 0x143 的投递源（文件库槽位表）与"装载登记 ⇒ 投递 / 析构 ⇒ 不再投递"在
        //      `check_event_143_file_library()` 里单独咬（CTest `spec_event_143_file_library`）。
        auto script = make_script_with_body();
        emit_instruction(script, 0x1F4, {});
        emit_instruction(script, 0x1F5, {});  // 区间离开 ⇒ 出口泵取走那条命令
        emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 2, {});
        core::Machine machine(key, table, &install);
        machine.enqueue_command_for_test(static_cast<std::int32_t>(5u << 24));
        machine.load_root(script);
        if (!machine.step()) return fail("泵失败夹具 0x1f4 停机");
        if (machine.step()) return fail("泵到非法 id 却没有失败");
        if (machine.last_stop().message.rfind("display-message:", 0) != 0) {
            std::fprintf(stderr, "command-queue: message=%s\n", machine.last_stop().message.c_str());
            return fail("id 非法没有走显示消息型异常");
        }
        if (machine.current_frame() != core::Machine::kPumpFrameSlot) {
            return fail("正命令装载失败时当前帧号没有停在 37");
        }
        if (machine.pending_command_count() != 0) return fail("一趟取走的不止一条");
    }
    // ⑥ 队列泵占用标记挡下 `0x1f5` 出口泵（`70-vm.md:305` / `:315`）：泵帧里再进 / 出区间，
    //    出口**不许**开闸（否则队列会被套成两层）。
    {
        auto script = make_script_with_body();
        bracket_script(script);
        emit_instruction(script, 0x2, {});
        core::Machine machine(key, table, &install);
        machine.load_root(script);
        if (!machine.step()) return fail("嵌套区间夹具 0x1f4 停机");
        machine.enqueue_command_for_test(2);  // 正命令：泵帧里会执行 0x1f4 / 0x1f5
        if (!machine.step()) return fail("嵌套区间夹具 0x1f5 停机");
        if (!machine.pump_busy() || machine.current_frame() != core::Machine::kPumpFrameSlot) {
            return fail("夹具没有进泵帧");
        }
        if (!machine.step()) return fail("泵帧的 0x1f4 停机了");
        machine.enqueue_command_for_test(3);  // 出口泵若开闸就会取走它
        if (!machine.step()) return fail("泵帧的 0x1f5 停机了");
        if (machine.pending_command_count() != 1) {
            return fail("泵帧里 0x1f5 的出口泵没有被队列泵占用标记挡下");
        }
        if (!machine.pump_busy() || machine.current_frame() != core::Machine::kPumpFrameSlot) {
            return fail("泵帧里 0x1f5 出口改动了泵状态");
        }
        if (machine.bracket_busy()) return fail("泵帧里的 0x1f5 没有清区间忙标记");
    }
    // ⑦ 宿主消息泵节流（`CORE-VM-LOOP` / `70-vm.md:27`）：初值 1；每迭代看与常量 4 的按位与，
    //    非 0 ⇒ 本迭代泵一次、泵后回 1 再 +1 到 2；为 0 ⇒ +1 不泵。⇒ 泵一次后隔 3 个迭代再泵。
    {
        auto script = make_script_with_body();
        for (int i = 0; i < 12; ++i) emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 2, {});
        core::Machine machine(key, table, &install);
        machine.load_root(script);
        if (machine.should_pump_host()) return fail("第一次迭代之前就宣称该泵");
        const bool expect_pump[7] = {false, false, false, true, false, false, true};
        const std::uint32_t expect_counter[7] = {2, 3, 4, 2, 3, 4, 2};
        for (int i = 0; i < 7; ++i) {
            if (!machine.step()) return fail("节流夹具停机了");
            if (machine.should_pump_host() != expect_pump[i]) {
                std::fprintf(stderr, "command-queue: step=%d pump=%d\n", i,
                             machine.should_pump_host() ? 1 : 0);
                return fail("宿主消息泵的节拍不是「泵一次后隔 3 个迭代」");
            }
            if (machine.pump_counter() != expect_counter[i]) {
                std::fprintf(stderr, "command-queue: step=%d counter=%u\n", i, machine.pump_counter());
                return fail("迭代头计数器的序列不对（初值 1 / 泵后回 1 再 +1 到 2）");
            }
        }    }
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
// ── 输入合成（`CORE-INPUT-WORD` 的「各来源的位号合成」/「输入锁存」） ──
// 判据（每条都能红）：
// ① 键盘表 256 项、初值全 −1、固定 7 项；位号 = **格里的值**（⛔ 不加 4）
// ② 鼠标左右键的位号 = **格里的基位号 + 4**（初值 0 / 1 ⇒ 4 / 5）；交换开启 ⇒ 两位互换
// ③ 第一组手柄的位号 = **槽序号 + 4**（格里的按钮编号不参与）、循环上界 = **键总数**（不是按钮数）、
//    组头的按钮数 ≤ 0 ⇒ 整组不采样、手柄来源**不看**输入可用标志
// ④ `0xfe` 越界 ⇒ 显示消息型异常、键总数不变、推进格数仍是 3
// ⑤ `0x101` = **事件并入**路径（消费锁存）、`0xff` = **电平**路径（互不吞并）；槽锁存 + 吞并标记
// ⑥ `0xfd` 的"先解绑、再登记"；`0x10d` 的"先清零、后写落点"
bool check_input_composition(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "input-composition: %s\n", what);
        return false;
    };

    // ① 键盘来源：256 项的表、初值全 −1、7 项固定；位号**不加 4**。
    {
        core::Machine machine(key, table);
        if (machine.key_code_bit(0x26) != 0 || machine.key_code_bit(0x27) != 1 ||
            machine.key_code_bit(0x28) != 2 || machine.key_code_bit(0x25) != 3 ||
            machine.key_code_bit(0x0D) != 4 || machine.key_code_bit(0x20) != 5 ||
            machine.key_code_bit(0x08) != 6) {
            return fail("键盘表的 7 项固定映射不对（↑0 →1 ↓2 ←3 Enter4 Space5 Backspace6）");
        }
        std::size_t mapped = 0;
        for (std::size_t code = 0; code < core::Machine::kSystemKeyCount; ++code) {
            if (machine.key_code_bit(code) != -1) ++mapped;
        }
        if (core::Machine::kSystemKeyCount != 256 || mapped != 7) {
            std::fprintf(stderr, "键盘表：256 项里映射了 %zu 项（期望 7）\n", mapped);
            return fail("键盘表的形状不对（256 项 / 只映射 7 项）");
        }
        machine.set_key_code_source([](int code) { return code == 0x26; });
        if (machine.sample_level_input() != 0b1u) {
            return fail("按住 ↑ 没有落在第 0 位（把键盘的位号加了 4？）");
        }
        machine.set_key_code_source([](int code) { return code == 0x0D; });
        if (machine.sample_level_input() != (1u << 4)) return fail("按住 Enter 没有落在第 4 位");
        // 输入可用标志 = 0 ⇒ 键盘来源整段不做（键盘 / 鼠标 / 手势受它门控）。
        machine.set_input_available(false);
        if (machine.sample_level_input() != 0) return fail("输入可用标志为 0 时键盘来源仍在置位");
        machine.set_input_available(true);
    }

    // ② 鼠标左右键：位号 = 格里的基位号 + 4；初值 = 各自的槽号（槽 0/1 ⇒ 0/1）。
    {
        core::Machine machine(key, table);
        if (machine.key_action(0) != 0 || machine.key_action(1) != 1 || machine.key_action(2) != 30) {
            return fail("键槽映射的初值不对（各自的槽号，槽 2 覆写成 30）");
        }
        if (machine.key_slot_count() != 2) return fail("键槽组头的槽数不是 2");
        machine.set_mouse_button_source([](std::int32_t& mask) { mask = 1; return true; });  // 左键
        if (machine.sample_level_input() != (1u << 4)) return fail("按左键没有落在第 4 位（格值 + 4）");
        machine.set_mouse_button_source([](std::int32_t& mask) { mask = 2; return true; });  // 右键
        if (machine.sample_level_input() != (1u << 5)) return fail("按右键没有落在第 5 位");
        machine.set_mouse_swap(true);  // 交换开启 ⇒ 两位互换
        machine.set_mouse_button_source([](std::int32_t& mask) { mask = 1; return true; });
        if (machine.sample_level_input() != (1u << 5)) return fail("交换开启后左键没有落到第 5 位");
        // 输入可用标志 = 0 ⇒ 鼠标来源也不查平台。
        machine.set_input_available(false);
        if (machine.sample_level_input() != 0) return fail("输入可用标志为 0 时鼠标来源仍在置位");
    }

    // ③ 第一组手柄：槽序号 + 4、上界 = 键总数、组头按钮数、**不看**输入可用标志。
    {
        core::Machine machine(key, table);
        machine.set_joy_button_source([] { return 1u; });  // 只按住按钮 0
        machine.set_joy_button_count(0);                   // 组头 0（没有设备 / system:LimitJoy=1）
        if (machine.sample_level_input() != 0) return fail("组头按钮数为 0 时手柄位仍被置起");
        // ★ 设备只报告 1 个按钮，但采样的上界是**键总数**（7）⇒ 七格（初值都是按钮 0）全部落第 4..10 位。
        machine.set_joy_button_count(1);
        const std::uint32_t seven_slots = ((1u << 7) - 1u) << 4;
        if (machine.sample_level_input() != seven_slots) {
            std::fprintf(stderr, "手柄采样 = 0x%X（期望 0x%X）\n", machine.sample_level_input(),
                         seven_slots);
            return fail("手柄采样的上界不是键总数（七格初值都是按钮 0 ⇒ 第 4..10 位全起）");
        }
        machine.set_input_available(false);
        if (machine.sample_level_input() != seven_slots) {
            return fail("输入可用标志为 0 时手柄来源被误门掉了（它不看这个标志）");
        }
        machine.set_input_available(true);
        // 键总数 1 + 0x107 槽 0 写 0（按钮 0）⇒ 输入字**恰好**是第 4 位。
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x107, {{0, 0}, {0, 0}});
        emit_instruction(bytes, 0xFE, {{0, 1}});
        emit_instruction(bytes, 2, {});
        machine.load_root(bytes);
        if (!machine.step() || !machine.step()) return fail("0x107 / 0xfe 夹具停机了");
        if (machine.sample_level_input() != (1u << 4)) {
            return fail("键总数 1 + 槽 0 = 按钮 0 时不是恰好第 4 位（不是第 0 / 第 35 位）");
        }
    }

    // ③b 位号 ≥ 32 的落点：移位只取低 5 位 ⇒ 槽 28 的位号 32 落到第 0 位（⛔ 不是"丢弃"）。
    {
        core::Machine machine(key, table);
        machine.set_joy_button_source([] { return 1u; });  // 只按住按钮 0
        machine.set_joy_button_count(8);
        auto bytes = make_script_with_body();
        for (std::uint32_t slot = 0; slot < 28; ++slot) {
            emit_instruction(bytes, 0x107, {{0, slot}, {0, 1}});  // 槽 0..27 → 按钮 1（没按）
        }
        emit_instruction(bytes, 0x107, {{0, 28}, {0, 0}});        // 槽 28 → 按钮 0（按下）
        emit_instruction(bytes, 0xFE, {{0, 29}});                 // 键总数 29
        emit_instruction(bytes, 2, {});
        machine.load_root(bytes);
        for (int i = 0; i < 30; ++i) {
            if (!machine.step()) return fail("位号 ≥ 32 的夹具停机了");
        }
        const std::uint32_t word = machine.sample_level_input();
        if (word != 1u) {
            std::fprintf(stderr, "位号 ≥ 32 的采样 = 0x%X（期望 0x1：槽 28 ⇒ 位 32 ⇒ 落第 0 位）\n",
                         word);
            return fail("越过 32 的位号被丢弃了（应当按移位的低 5 位落位）");
        }
    }

    // ③c 手柄的方向位 = 0..3（与键盘方向键共用），**四个半平面各自独立**（斜方向两位同时亮）。
    {
        core::Machine machine(key, table);
        machine.set_joy_button_count(4);
        machine.set_joy_hat_four_way(true);
        auto angle = 0;
        machine.set_joy_hat_source([&angle] { return angle; });
        const auto bits_at = [&](int value) {
            angle = value;
            return machine.sample_level_input();
        };
        if (bits_at(0) != (1u << 0)) return fail("帽子 0° 不是只有第 0 位（上）");
        if (bits_at(4500) != ((1u << 0) | (1u << 1))) {
            std::fprintf(stderr, "帽子 45° = 0x%X（期望 0x3）\n", bits_at(4500));
            return fail("斜方向没有同时点亮两位（把帽子实现成了互斥的 8 方向枚举）");
        }
        if (bits_at(9000) != (1u << 1)) return fail("帽子 90° 不是只有第 1 位（右）");
        if (bits_at(18000) != (1u << 2)) return fail("帽子 180° 不是只有第 2 位（下）");
        if (bits_at(27000) != (1u << 3)) return fail("帽子 270° 不是只有第 3 位（左）");
        // 居中（−1）⇒ 这四位改由**摇杆轴**越过四条阈值给出（`CORE-INPUT-WORD` 的「手柄的方向位」）。
        // 设备能力：两轴都是 0..2000；初始化时采到的轴位置 = 1000 ⇒ 低 = 500、高 = 1500。
        machine.set_joy_axis_range(0, 0, 2000);
        machine.set_joy_axis_range(1, 0, 2000);
        machine.set_joy_stick_source([] { return std::array<std::uint32_t, 2>{1000u, 1000u}; });
        machine.sample_joy_axis_origin();
        machine.set_joy_stick_source([] {
            return std::array<std::uint32_t, 2>{1600u, 1000u};  // 轴 1 大于高阈值 ⇒ 右
        });
        machine.set_host_clock_ms(100);  // 越过 20ms 缓存窗口，让新轴值真的被查询
        angle = -1;
        if (machine.sample_level_input() != (1u << 1)) {
            return fail("帽子居中时摇杆轴没有给出方向位（右 ⇒ 第 1 位）");
        }
        // 设备能力不报"帽子可四向" ⇒ 一律按居中处理（即使平台给了角度）。
        machine.set_joy_hat_four_way(false);
        angle = 4500;
        machine.set_host_clock_ms(200);
        if (machine.sample_level_input() != (1u << 1)) {
            return fail("设备不报四向时仍按帽子角度给了方向位（应当按居中处理 ⇒ 用摇杆轴）");
        }
    }

    // ④ `0xfe` 越界：显示消息型异常、键总数不变、推进格数仍是 3。
    for (const std::int32_t value : {32, -1}) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xFE, {{0, static_cast<std::uint32_t>(value)}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (machine.step() || !machine.halted()) {
            std::fprintf(stderr, "0xfe(value=%d) 没有抛\n", value);
            return fail("0xfe 越界没有抛显示消息型异常");
        }
        if (machine.last_display_code() != "key-total-out-of-range") {
            std::fprintf(stderr, "0xfe(value=%d) 错误码 = %s\n", value,
                         machine.last_display_code().c_str());
            return fail("0xfe 越界的错误码不对");
        }
        if (machine.key_total() != 7) return fail("0xfe 越界时键总数被改了（应当一个字节都不写）");
        if (machine.frame().length_slot != 3) {
            std::fprintf(stderr, "0xfe 抛异常时推进格数 = %d（期望 3）\n", machine.frame().length_slot);
            return fail("0xfe 抛异常时推进格数不是 3");
        }
    }

    // ⑤ `0x101` 的事件并入路径（消费）vs `0xff` 的电平路径（不消费）；槽锁存 + 吞并标记。
    {
        core::Machine machine(key, table);
        machine.push_key_down_message(0x26);  // ↑ = 输入位 0
        if (machine.latch_bits() != 1u) return fail("键按下消息没有写进位号锁存字");
        if (machine.sample_event_input() != 0b1u) return fail("事件并入采样没有把锁存位并进输入字");
        if (machine.latch_bits() != 0) return fail("事件并入采样没有消费位号锁存字（没有清 0）");
        if (machine.sample_event_input() != 0b0u) return fail("同一个按下事件被第二次采样看到（没有消费）");
        // ★ 两条路径没有被混成一条：仍按住 ↑ 时 `0xff` 的电平采样照样置第 0 位。
        machine.set_key_code_source([](int code) { return code == 0x26; });
        if (machine.sample_level_input() != 0b1u) return fail("事件并入把电平来源吃掉了");
        // 槽锁存：左键**按下**消息（set:ClickOnUp = 0）⇒ 槽 0 ⇒ 基位号 0 + 4 = 第 4 位。
        machine.push_mouse_button_message(false, true);
        if (machine.latch_slots() != 1u) return fail("左键按下消息没有写进槽锁存字");
        if ((machine.sample_event_input() & (1u << 4)) == 0) return fail("槽锁存的并入没有落第 4 位");
        if (machine.latch_slots() != 0) return fail("槽锁存位没有被清掉");
        // 吞并标记：一次采样看到左右键正按下 ⇒ 标记置起 ⇒ **下一条**鼠标键消息被丢掉。
        machine.set_mouse_button_source([](std::int32_t& mask) { mask = 1; return true; });
        (void)machine.sample_event_input();
        if (!machine.mouse_swallow_pending()) return fail("采样看到左右键按下后没有置吞并标记");
        machine.push_mouse_button_message(false, true);
        if (machine.latch_slots() != 0 || machine.mouse_swallow_pending()) {
            return fail("吞并标记没有把这一条鼠标键消息丢掉");
        }
        machine.push_mouse_button_message(false, true);  // 标记已清 ⇒ 照常写入
        if (machine.latch_slots() != 1u) return fail("吞并后的下一条消息没有照常写入");
        // `set:ClickOnUp` 非 0 ⇒ 只有**抬起**消息写锁存。
        core::Machine up_machine(key, table);
        up_machine.set_click_on_up(1);
        up_machine.push_mouse_button_message(false, true);   // 按下：不写
        if (up_machine.latch_slots() != 0) return fail("set:ClickOnUp 非 0 时按下消息仍写了锁存");
        up_machine.push_mouse_button_message(false, false);  // 抬起：写
        if (up_machine.latch_slots() != 1u) return fail("set:ClickOnUp 非 0 时抬起消息没有写锁存");
        // ★ `0x101` 本身走的就是这条事件并入路径：它虽然把采样结果丢弃，但**消费**了锁存的事件。
        //   ⇒ 把 `0x101` 实现成 `0xff` 那种电平采样时，这里锁存字还是 1 ⇒ 红。
        auto poll = make_script_with_body();
        emit_instruction(poll, 0x101, {});
        emit_instruction(poll, 2, {});
        core::Machine poll_machine(key, table);
        poll_machine.load_root(poll);
        poll_machine.push_key_down_message(0x26);
        if (!poll_machine.step()) return fail("0x101 夹具停机了");
        if (poll_machine.latch_bits() != 0) return fail("0x101 没有走事件并入路径（锁存字没被消费）");
        if (poll_machine.input_word() != 0) return fail("0x101 没有把采样结果丢弃（输入字非 0）");
        if (poll_machine.input_a() != 1 || poll_machine.input_b() != 0) {
            return fail("0x101 的两个标量不是 A=1 / B=0");
        }
        // 引擎级标志 D 非 0 ⇒ 输入字第 6 位置起（0x88 写 C/D，0x19c 的 C 支把 D 置 1）。
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x88, {{0, 1}});
        emit_instruction(bytes, 0x19C, {});
        emit_instruction(bytes, 2, {});
        core::Machine mode_machine(key, table);
        mode_machine.load_root(bytes);
        if (!mode_machine.step() || !mode_machine.step()) return fail("0x88 / 0x19c 夹具停机了");
        if ((mode_machine.sample_event_input() & (1u << 6)) == 0) {
            return fail("引擎级标志 D 非 0 时第 6 位没有置起");
        }
    }

    // ⑥ `0xfd`：先解绑该槽旧逻辑键的键码，再把新键码登记到该槽的位号上。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xFD, {{0, 0}, {0, 28}});  // 槽 0 ← 逻辑键 28（Enter）
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0xfd 夹具停机了");
        if (machine.key_code_bit(0x0D) != 0) return fail("0xfd 没有把 Enter 的键码登记到第 0 位");
        if (machine.key_code_bit(0x26) != -1) return fail("0xfd 没有把槽 0 旧逻辑键（↑）的键码解绑");
        machine.set_key_code_source([](int code) { return code == 0x0D; });
        if (machine.sample_level_input() != 0b1u) {
            return fail("0xfd 之后按 Enter 没有只点亮第 0 位（键码表一格只存一个位号）");
        }
        machine.set_key_code_source([](int code) { return code == 0x26; });
        if (machine.sample_level_input() != 0) return fail("0xfd 之后按 ↑ 仍点亮了第 0 位（没有解绑）");
        // 连续两次 0xfd 到同一槽：第 2 条把上一条绑定的 Enter 键码抹掉。
        auto twice = make_script_with_body();
        emit_instruction(twice, 0xFD, {{0, 7}, {0, 28}});
        emit_instruction(twice, 0xFD, {{0, 7}, {0, 14}});  // 逻辑键 14 = Backspace（表 B：14 → 8）
        emit_instruction(twice, 2, {});
        core::Machine machine2(key, table);
        machine2.load_root(twice);
        if (!machine2.step() || !machine2.step()) return fail("两条 0xfd 夹具停机了");
        if (machine2.key_code_bit(0x0D) != -1) {
            return fail("第二条 0xfd 没有把上一条绑定的 Enter 键码抹掉");
        }
        if (machine2.key_code_bit(0x08) != 7) return fail("第二条 0xfd 没有把新键码登记到第 7 位");
    }

    // ⑦ `0x10d`：**先清零、后写落点** —— 落点写抛异常时累加格已经清 0。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x10D, {{2, 0}});  // 落点 = 内联文本（写整型不支持）⇒ 抛
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        machine.add_wheel_delta(120);
        machine.add_wheel_delta(-40);
        if (machine.step() || !machine.halted()) return fail("0x10d 用不可写落点没有抛");
        if (machine.wheel_delta() != 0) return fail("0x10d 抛之前没有先清零累加格（两步次序写反了）");
    }
    return true;
}

// ── 文本 / 字符串（`CORE-OPS-TEXT-SET` 的容器句柄与格式化、`CORE-OPS-TEXT-FONT-NAME-2DE`） ──
// 判据（每条都能红）：
// ① `0x192` 的落点是**字符串容器句柄**（`0x8005` / `0x800B`）时，记录里存的是**容器引用的十进制文本**、
//    源文本进**首元素**（空容器追加、非空覆写首元素）；记录文本**不被本条改写**
// ② 同一个句柄当**源**读 ⇒ 首元素文本；空记录 / 空容器 ⇒ **空串**；记录文本非数字 ⇒ 未定义行为（本实现诊断 + 停止）
// ③ 整数源 ⇒ 十进制；浮点源 ⇒ **定点 6 位小数**（`1.5` 与 `"1.500000"` 相等，`0x194` 同一读原语）
// ④ `0x2de` 在**系统字体名一览**里查：逐字节 + 长度相等、查前跳过**一个** `@`、未命中 −1
//    ★ 本节按**替换关闭**（引擎正文这一面）断查表规则；替换生效时的口径（任意非空名字 ⇒ 宿主下标 0）
//      由 `check_font_name_host_index` / CTest `spec_ops_2de_host_font_index` 单独守。
// ⑤ 表项标志字节的**位 0 = 等宽** ⇒ 该字体固定半宽（字宽 = 字号 ÷ 2），⛔ 不是"跳过这个字体"；
//    内建识别的 `IPA Pゴシック` / `IPA P明朝` 即使位 0 置起也按字形算
bool check_text_container_and_font_names(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "text-container: %s\n", what);
        return false;
    };
    constexpr std::uint32_t kGlobalStringHandle = 0x8005u;

    // ① / ②：容器句柄当落点、当源；记录里存的是引用文本。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x192, {{kGlobalStringHandle, 3}, {5, 0}});  // 句柄槽 3 ← 记录 0 的文本
        emit_instruction(bytes, 0x192, {{5, 9}, {kGlobalStringHandle, 3}}); // 记录 9 ← 句柄槽 3 的首元素
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.write_text(core::Operand{5, 0}, "hello");
        machine.load_root(bytes);
        if (!machine.step()) {
            std::fprintf(stderr, "0x192 容器落点的停因 = %s\n", machine.last_stop().message.c_str());
            return fail("0x192（容器落点）停机了");
        }
        const std::string reference_text = machine.string_record_text(3, false);
        if (reference_text.empty() || reference_text == "hello") {
            std::fprintf(stderr, "句柄落点的记录文本 = \"%s\"\n", reference_text.c_str());
            return fail("句柄落点的记录里没有写容器引用（源文本不该进记录）");
        }
        const auto reference = static_cast<std::uint32_t>(std::atoi(reference_text.c_str()));
        if (machine.string_container_size(reference) != 1) {
            return fail("句柄落点没有建出容器首元素");
        }
        if (machine.string_container_first(reference) != "hello") {
            return fail("容器首元素不是源文本");
        }
        if (!machine.step()) return fail("0x192（容器源）停机了");
        if (machine.string_record_text(9, false) != "hello") {
            return fail("句柄当源读到的不是首元素文本（记录文本不是引用？）");
        }
        // 覆写首元素：记录文本**不被改写**、元素被换掉。
        auto overwrite = make_script_with_body();
        emit_instruction(overwrite, 0x192, {{kGlobalStringHandle, 3}, {5, 1}});
        emit_instruction(overwrite, 2, {});
        machine.write_text(core::Operand{5, 1}, "world");
        machine.load_root(overwrite);
        if (!machine.step()) return fail("第二次 0x192 停机了");
        if (machine.string_record_text(3, false) != reference_text) {
            return fail("已有容器时记录文本被本条改写了");
        }
        if (machine.string_container_size(reference) != 1 ||
            machine.string_container_first(reference) != "world") {
            return fail("非空容器没有覆写首元素（或追加了第二个元素）");
        }
        // 空记录 / 空容器 ⇒ 空串。
        auto empty = make_script_with_body();
        emit_instruction(empty, 0x192, {{5, 10}, {kGlobalStringHandle, 7}});  // 从未写过的句柄槽
        emit_instruction(empty, 2, {});
        core::Machine empty_machine(key, table);
        empty_machine.load_root(empty);
        if (!empty_machine.step()) return fail("空句柄当源读时停机了");
        if (!empty_machine.string_record_text(10, false).empty()) {
            return fail("空记录 / 空容器没有读成空串");
        }
        // 记录文本非数字（被脚本改成 "abc"）⇒ 句柄失败路径的未定义行为：本实现诊断 + 响亮失败。
        auto broken = make_script_with_body();
        emit_instruction(broken, 0x192, {{5, 11}, {kGlobalStringHandle, 8}});
        emit_instruction(broken, 2, {});
        core::Machine broken_machine(key, table);
        broken_machine.write_text(core::Operand{5, 8}, "abc");
        broken_machine.load_root(broken);
        if (broken_machine.step() || !broken_machine.halted()) {
            return fail("记录文本不是数字时没有响亮失败（被当成空串了？）");
        }
    }

    // ③ 整数 ⇒ 十进制；浮点 ⇒ 定点 6 位小数；`0x194` 用同一条读原语。
    {
        const std::uint32_t one_and_half = std::bit_cast<std::uint32_t>(1.5f);
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x192, {{5, 0}, {0, static_cast<std::uint32_t>(-42)}});
        emit_instruction(bytes, 0x192, {{5, 1}, {1, one_and_half}});
        emit_instruction(bytes, 0x192, {{5, 2}, {5, 3}});  // 记录 2 ← 记录 3 的文本（对照）
        emit_instruction(bytes, 0x194, {{9, 0}, {1, one_and_half}, {5, 4}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.write_text(core::Operand{5, 3}, "1.500000");
        machine.write_text(core::Operand{5, 4}, "1.500000");
        machine.load_root(bytes);
        for (int i = 0; i < 4; ++i) {
            if (!machine.step()) return fail("文本格式化夹具停机了");
        }
        if (machine.string_record_text(0, false) != "-42") {
            std::fprintf(stderr, "整数源的文本 = %s\n", machine.string_record_text(0, false).c_str());
            return fail("整数源没有格式化成十进制（可带负号 / 无前导零）");
        }
        if (machine.string_record_text(1, false) != "1.500000") {
            std::fprintf(stderr, "浮点源的文本 = %s\n", machine.string_record_text(1, false).c_str());
            return fail("浮点源没有格式化成定点 6 位小数");
        }
        if (machine.local_int(0) != 1) {
            std::fprintf(stderr, "0x194(1.5, \"1.500000\") = %d\n", machine.local_int(0));
            return fail("浮点 1.5 与文本 \"1.500000\" 不相等（两条读不是同一原语）");
        }
    }

    // ④ `0x2de`：系统字体名一览的查找（逐字节 + 长度相等；跳过开头一个 `@`；未命中 −1）。
    //    ★ **替换关闭**这一面（引擎正文）：宿主策略把名字表换成单项宿主字体是**产品路径**的事
    //      （`wire_platform_fonts` ⇒ `set_font_substitution(true)`），本节刻意不打开它。
    //      ⇒ 「未命中 −1」在这里仍然成立；产品路径下它被 `spec_ops_2de_host_font_index` 取代
    //      （SCOPE §0 D1：名字**总是**解析到 Amayui，⛔ 不是"表空、查找恒失败"）。
    {
        core::Machine machine(key, table);
        machine.set_font_name_table({{"MS Gothic", 0}, {"Foo Bar", 1}, {"@At", 0}});
        const auto query = [&](const std::string& name) -> std::int32_t {
            auto bytes = make_script_with_body();
            emit_instruction(bytes, 0x2DE, {{3, 0}, {5, 0}});
            emit_instruction(bytes, 2, {});
            core::Machine probe = machine;  // 表随拷贝走；每次从同一个表出发
            probe.write_text(core::Operand{5, 0}, name);
            probe.load_root(bytes);
            if (!probe.step()) return -12345;
            return probe.global_int(0);
        };
        if (query("MS Gothic") != 0) return fail("0x2de 没命中表项 0");
        if (query("@MS Gothic") != 0) return fail("0x2de 没有跳过名字开头的一个 `@`");
        if (query("@Foo Bar") != 1) return fail("0x2de 跳过 `@` 之后没命中表项 1");
        if (query("ms gothic") != -1) return fail("0x2de 做了大小写折叠（应当逐字节比）");
        if (query("MS") != -1) return fail("0x2de 只比了前缀（长度必须相等）");
        if (query("At") != -1) return fail("0x2de 把表里的 `@At` 当成了 `At`（查表侧不跳 `@`）");
        if (query("nope") != -1) return fail("0x2de 未命中没有写 −1");
    }

    // ⑤ 表项标志字节的位 0 = **等宽**（`FIXED_PITCH`）：固定半宽支（字宽 = ∓字号/2，符号看对位模式）；
    //    ⛔ 不是"跳过这个字体"；`IPA P` 系列即使位 0 置起也按字形算。
    {
        // "IPA Pゴシック" / "IPA P明朝" 的 CP932 字节（引擎字符串是 ANSI 字节）。
        const std::string ipa_gothic = std::string("IPA P") + "\x83\x53\x83\x56\x83\x62\x83\x4E";
        const std::string ipa_mincho = std::string("IPA P") + "\x96\xBE\x92\xA9";
        core::Machine machine(key, table);
        machine.set_font_name_table({{"Amayui CN", 1}, {"MS Gothic", 0}, {ipa_gothic, 1}, {ipa_mincho, 1}});
        // 设对位模式 + 字面名（0x2db 写对位模式、0x1a5 写 A 支名字）。
        const auto set_face = [&](std::int32_t align, const std::string& face) {
            auto bytes = make_script_with_body();
            emit_instruction(bytes, 0x2DB, {{0, static_cast<std::uint32_t>(align)}});
            emit_instruction(bytes, 0x1A5, {{5, 0}});
            emit_instruction(bytes, 2, {});
            machine.write_text(core::Operand{5, 0}, face);
            machine.load_root(bytes);
            if (!machine.step() || !machine.step()) return false;
            return true;
        };
        // 字号 40 的固定半宽，**两个派发点各判一次**（`CORE-OPS-TEXT-FONT-NAME-2DE` 的对位模式表）：
        //   · 建平台字体那条（`PlatformFont`，画字）：= 2 ⇒ **+20**，否则（0 / 1 / ≥3）⇒ **−20**；
        //   · 描述派生那条（`DescriptionMetric`，量度格）：= 1 ⇒ **−20**，否则（0 / 2 / ≥3）⇒ **+20**。
        //   ⇒ 对位模式 **≥ 3** 时两路**异号**（画字 −20 / 量度格 +20）——把两条判据合并成一条，就会有
        //     一个取值变成不可达 ⇒ 这条红。
        using HalfPath = core::Machine::HalfWidthPath;
        const auto expect_half = [&](std::int32_t align, HalfPath path, std::int32_t want,
                                     const char* what) {
            if (!set_face(align, "Amayui CN")) return fail("0x2db / 0x1a5 夹具停机了");
            const std::int32_t got = machine.font_fixed_half_width(40, path);
            if (got != want) {
                std::fprintf(stderr, "对位模式 %d / %s 的固定半宽 = %d（期望 %d）\n", align,
                             path == HalfPath::PlatformFont ? "画字" : "量度格", got, want);
                return fail(what);
            }
            return true;
        };
        if (!expect_half(1, HalfPath::PlatformFont, -20, "画字：对位模式 1 没有给出 −字号/2")) return false;
        if (!expect_half(1, HalfPath::DescriptionMetric, -20, "量度格：对位模式 1 没有给出 −字号/2")) return false;
        if (!expect_half(2, HalfPath::PlatformFont, 20, "画字：对位模式 2 没有给出 +字号/2")) return false;
        if (!expect_half(2, HalfPath::DescriptionMetric, 20, "量度格：对位模式 2 没有给出 +字号/2")) return false;
        // 对位模式 0：画字那条**照正文取 −**（旧断言「本版窄 unknown ⇒ 0」已作废——正文现在写死了这一格）。
        if (!expect_half(0, HalfPath::PlatformFont, -20,
                         "画字：对位模式 0 没有按正文取 −字号/2（旧的「窄 unknown」断言已作废）")) {
            return false;
        }
        // 对位模式 ≥ 3：两个派发点**异号**（这条是本用例的主判据）。
        if (!expect_half(3, HalfPath::PlatformFont, -20,
                         "画字：对位模式 ≥3 没有取 −字号/2（两个派发点被合并了？）")) {
            return false;
        }
        if (!expect_half(3, HalfPath::DescriptionMetric, 20,
                         "量度格：对位模式 ≥3 没有取 +字号/2（两个派发点被合并了？）")) {
            return false;
        }
        if (!set_face(1, "MS Gothic")) return fail("0x2db / 0x1a5 夹具停机了");
        if (machine.font_fixed_half_width(40) != 0) return fail("位 0 未置起的字体被当成了固定半宽");
        if (!set_face(1, ipa_gothic)) return fail("0x2db / 0x1a5 夹具停机了");
        if (machine.font_fixed_half_width(40) != 0) {
            return fail("内建识别的 IPA Pゴシック 没有走\"按字形算\"那一支（位 0 不该生效）");
        }
        if (!set_face(1, ipa_mincho)) return fail("0x2db / 0x1a5 夹具停机了");
        if (machine.font_fixed_half_width(40) != 0) {
            return fail("内建识别的 IPA P明朝 没有走\"按字形算\"那一支");
        }
        // 光栅化那一半：固定半宽 ⇒ 每个字形的推进 = |字宽|（不是 FreeType 的 advance）。
        // ★ 没有字体后端 / 没有字体文件时这条判不了（与其它文本用例同口径）。
        const auto advance_of = [&](std::int32_t align, const std::string& face,
                                    std::int32_t flags) -> int {
            core::Machine draw = machine;
            draw.set_font_name_table({{face, flags}});
            auto bytes = make_script_with_body();
            emit_instruction(bytes, 0x2DB, {{0, static_cast<std::uint32_t>(align)}});
            emit_instruction(bytes, 0x1A5, {{5, 0}});
            emit_instruction(bytes, 2, {});
            draw.write_text(core::Operand{5, 0}, face);
            draw.load_root(bytes);
            if (!draw.step() || !draw.step()) return -1;
            auto& entry = draw.texture_entry(9);
            entry.slot.present = true;
            entry.slot.width = 400;
            entry.slot.height = 80;
            entry.slot.rgba.assign(static_cast<std::size_t>(400) * 80 * 4, 0);
            int advance = 0;
            std::string error;
            if (!core::draw_text_into_texture(draw, 9, 0, 0, "\x82\x6C", &error, &advance, 40)) {
                return -1;  // 没有字形后端 ⇒ 判不了
            }
            return advance;
        };
        const int half = advance_of(1, "Amayui CN", 1);
        const int normal = advance_of(1, "Amayui CN", 0);
        if (half > 0 && normal > 0) {
            if (half != 20) {  // 字号 40 ⇒ 固定半宽 20
                std::fprintf(stderr, "固定半宽的推进 = %d（期望 20）\n", half);
                return fail("固定半宽字体没有按\"字号 ÷ 2\"推进");
            }
            if (normal == half) {
                std::fprintf(stderr, "非等宽字体的推进也是 %d ⇒ 夹具分辨不出两档\n", normal);
                return fail("等宽分支没有真的改变推进（或对照字体恰好等宽）");
            }
        }
    }

    // ⑥ `0x75` / `0x197` / `0x1a5` / `0x2fe` 的**度量派生**（「入口体写的值 ≠ 重建后的最终值」）：
    //    一次派生发**两次**度量（第一次 = 常量参考字体 `ＭＳ ゴシック`、第二次 = 该描述自己的字面名），
    //    写回两格的是**第二次**；回退（AspectMode ≠ 0 时不回退；名字不在表 / 位 0 且非 IPA）⇒ 下一格 ← 高度 ÷ 2。
    {
        struct Calls {
            std::vector<std::string> faces;
            std::vector<std::int32_t> sizes;
        };
        const auto make_measurer = [](Calls& calls, std::int32_t line_height,
                                      std::int32_t average_width) {
            return [&calls, line_height, average_width](const std::string& face,
                                                        std::int32_t size) {
                calls.faces.push_back(face);
                calls.sizes.push_back(size);
                core::Machine::FontMetrics metrics;
                metrics.line_height = line_height;
                metrics.average_width = average_width;
                metrics.glyph_box_width = 0;      // 不触发"再缩一次"
                metrics.measured_box_width = 0;   // 同上（判据要求 > 0）
                return metrics;
            };
        };
        const std::string reference_face = "\x82\x6C\x82\x72\x20\x83\x53\x83\x56\x83\x62\x83\x4E";
        // ⑥a：对位模式 = 0 ⇒ 直接重建，**不发**度量；两格 = −字号 / −(字号/2)。
        {
            auto bytes = make_script_with_body();
            emit_instruction(bytes, 0x2DB, {{0, 0}});
            emit_instruction(bytes, 0x1A5, {{5, 0}});
            emit_instruction(bytes, 0x75, {{0, 20}});
            emit_instruction(bytes, 2, {});
            core::Machine machine(key, table);
            Calls calls;
            machine.set_font_measurer(make_measurer(calls, 20, 7));
            machine.write_text(core::Operand{5, 0}, "Amayui CN");
            machine.load_root(bytes);
            for (int i = 0; i < 3; ++i) {
                if (!machine.step()) return fail("⑹a 夹具停机了");
            }
            if (!calls.faces.empty()) return fail("对位模式 0 时发了度量请求（应当直接重建）");
            if (machine.font_description(false, 0).height != -20 ||
                machine.font_description(false, 0).width != -10) {
                return fail("对位模式 0 时入口体没有写 −字号 / −(字号/2)");
            }
        }
        // ⑥b：对位模式 = 1 + 名字在表里且非等宽 ⇒ 两格来自**第二次度量**；第一次量的是参考字体。
        {
            auto bytes = make_script_with_body();
            emit_instruction(bytes, 0x2DB, {{0, 1}});
            emit_instruction(bytes, 0x1A5, {{5, 0}});
            emit_instruction(bytes, 0x75, {{0, 20}});
            emit_instruction(bytes, 2, {});
            core::Machine machine(key, table);
            machine.set_font_name_table({{"Amayui CN", 0}});
            Calls calls;
            machine.set_font_measurer(make_measurer(calls, 20, 7));
            machine.write_text(core::Operand{5, 0}, "Amayui CN");
            machine.load_root(bytes);
            for (int i = 0; i < 3; ++i) {
                if (!machine.step()) return fail("⑹b 夹具停机了");
            }
            if (machine.font_description(false, 0).width != -7) {
                std::fprintf(stderr, "下一格 = %d（期望 −7 = −测得平均字宽）\n",
                             machine.font_description(false, 0).width);
                return fail("非 0 支的下一格不是来自度量（被写成了 −字号/2？）");
            }
            if (calls.faces.empty() || calls.faces[0] != reference_face) {
                return fail("第一次度量的不是常量参考字体（ＭＳ ゴシック）");
            }
            if (calls.faces.size() < 2 || calls.faces[1] != "Amayui CN") {
                return fail("第二次度量的不是该描述自己的字面名");
            }
            if (calls.faces.size() != 4) {  // 两份描述 × 两次（另有 0x1a5 的重建路径也跑了一次派生 ⇒ 8）
                std::fprintf(stderr, "度量请求数 = %zu\n", calls.faces.size());
            }
            // 写入值是**平台输入**：换一个平均字宽 ⇒ 下一格随之变（⛔ 不是从操作数算出来的）。
            core::Machine other(key, table);
            other.set_font_name_table({{"Amayui CN", 0}});
            Calls other_calls;
            other.set_font_measurer(make_measurer(other_calls, 20, 13));
            other.write_text(core::Operand{5, 0}, "Amayui CN");
            other.load_root(bytes);
            for (int i = 0; i < 3; ++i) {
                if (!other.step()) return fail("⑹b 对照夹具停机了");
            }
            if (other.font_description(false, 0).width != -13) {
                return fail("换一个测得字宽之后下一格没变（说明它不是来自度量）");
            }
        }
        // ⑥c：回退三条判据。
        const auto width_after = [&](std::int32_t align, const std::string& face,
                                     std::int32_t aspect_mode, std::int32_t flags,
                                     bool table_has_face) {
            auto bytes = make_script_with_body();
            emit_instruction(bytes, 0x2DB, {{0, static_cast<std::uint32_t>(align)}});
            emit_instruction(bytes, 0x1A5, {{5, 0}});
            emit_instruction(bytes, 0x75, {{0, 20}});
            emit_instruction(bytes, 2, {});
            core::Machine machine(key, table);
            if (table_has_face) machine.set_font_name_table({{face, flags}});
            machine.config_mut().set_int("display:AspectMode", aspect_mode);
            Calls calls;
            machine.set_font_measurer(make_measurer(calls, 20, 7));
            machine.write_text(core::Operand{5, 0}, face);
            machine.load_root(bytes);
            for (int i = 0; i < 3; ++i) {
                if (!machine.step()) return 999999;
            }
            return machine.font_description(false, 0).width;
        };
        // ② 名字不在表里 ⇒ 回退（下一格 = 高度 ÷ 2 = −10）。
        if (width_after(1, "Unknown Face", 0, 0, false) != -10) {
            return fail("名字不在字体名表里时没有回退到 高度 ÷ 2");
        }
        // ③ 表项位 0 置起且非 IPA P ⇒ 回退。
        if (width_after(1, "Amayui CN", 0, 1, true) != -10) {
            return fail("等宽（非 IPA P）时没有回退到 高度 ÷ 2");
        }
        // AspectMode ≠ 0 ⇒ **不回退**（即使名字不在表里也走度量）。
        if (width_after(1, "Unknown Face", 1, 0, false) != -7) {
            return fail("display:AspectMode ≠ 0 时仍然回退了（合同是「不回退」）");
        }
        // ⑥d：`0x1a5` 的重建路径同样要跑派生（入口体不分岔，重建路径分岔）。
        {
            auto bytes = make_script_with_body();
            emit_instruction(bytes, 0x2DB, {{0, 1}});
            emit_instruction(bytes, 0x1A5, {{5, 0}});
            emit_instruction(bytes, 2, {});
            core::Machine machine(key, table);
            machine.set_font_name_table({{"Amayui CN", 0}});
            Calls calls;
            machine.set_font_measurer(make_measurer(calls, 20, 9));
            machine.write_text(core::Operand{5, 0}, "Amayui CN");
            machine.load_root(bytes);
            if (!machine.step() || !machine.step()) return fail("⑥d 夹具停机了");
            if (machine.font_description(false, 0).width != -9) {
                std::fprintf(stderr, "0x1a5 之后的下一格 = %d（期望 −9）\n",
                             machine.font_description(false, 0).width);
                return fail("0x1a5 的重建路径没有按度量覆写下一格");
            }
        }
        // ⑥e：「半格」= **参考字形的黑框宽 ÷ 2**（⛔ 不是字号 ÷ 2），且再缩只在**对位模式 = 1** 那一档。
        //   注入一个按请求面名区分的假后端（字号固定 20 ⇒ "字号 ÷ 2" 是 10，与下面设的半格 12 **不同**，
        //   所以"阈值取的是哪一个"当场可分辨）：
        //     · 参考字体那次 ⇒ 黑框宽 24 ⇒ 半格（阈值）= 24 >> 1 = 12、分子 = 24 × 0.5 = 12；
        //     · 描述那次 ⇒ 量度框 60（文本 extent）、平均字宽 50、黑框宽由本例给。
        //   ★ 分母口径单列：再缩的分母是**本字形黑框宽**（`0x4584D2 mov eax,[…gmBlackBoxX]` →
        //     `0x458500 mov [ebp+var_13C],eax` → `0x458506 fild` → `0x458516 fdivp`），⛔ 不是量度框。
        //     ⇒ 本例把「本字形黑框」与「本字形量度框」**故意取成不同的数**（20 / 60）：
        //     换成量度框 ⇒ ② 变成 12 ÷ 60 × (−50) = −10 ⇒ 红；这也是"分母"唯一的判据。
        //   ① 本字形黑框宽 11：> 10（字号 ÷ 2 的**旧**阈值）但 ≤ 12（参考字形的半格）⇒ **不该再缩**，
        //      下一格 = −50；若阈值取了字号 ÷ 2 ⇒ 会缩成 trunc(12 ÷ 11 × (−50)) = trunc(−54.545…) = −54。
        //   ② 本字形黑框宽 20：> 12 ⇒ 缩一次：trunc(**12** ÷ **20** × (−50)) = **−30**
        //      （分子 = 参考盒宽 24 × 0.5 = 12、分母 = 本字形黑框 20、乘数 = −平均字宽 50）。
        //      错法各自的落点（都落在 −30 之外）：分母取量度框 60 ⇒ −10（本仓上一版）；
        //      分母取参考盒 24 ⇒ −25；分子取成本字形黑框 20 ⇒ −50。
        //   ③ 对位模式 2（不是 1 档）：同样的量度 ⇒ **不缩**，下一格 = −50（正文：其它非 0 档没有半格判据）。
        const auto width_with_metrics = [&](std::int32_t align, std::int32_t reference_box,
                                            std::int32_t glyph_box, std::int32_t measured_box,
                                            std::int32_t average_width) -> std::int32_t {
            auto bytes = make_script_with_body();
            emit_instruction(bytes, 0x2DB, {{0, static_cast<std::uint32_t>(align)}});
            emit_instruction(bytes, 2, {});
            core::Machine machine(key, table);
            machine.set_font_measurer([reference_box, glyph_box, measured_box,
                                       average_width](const std::string& face, std::int32_t) {
                core::Machine::FontMetrics metrics;
                metrics.line_height = 20;  // = 字号 ⇒ 不触发「测得行高 > 字号」的高度重算
                if (face == core::Machine::reference_font_face()) {
                    metrics.glyph_box_width = reference_box;  // 参考字形的黑框宽
                    metrics.measured_box_width = reference_box;
                    metrics.average_width = 10;
                } else {
                    metrics.glyph_box_width = glyph_box;
                    metrics.measured_box_width = measured_box;
                    metrics.average_width = average_width;
                }
                return metrics;
            });
            machine.set_font_substitution(true);
            machine.load_root(bytes);
            if (!machine.step()) return 999999;
            core::FontDescription desc;
            desc.face = "Amayui CN";
            machine.derive_font_description(desc, 20);
            return desc.width;
        };
        const auto width_with_box = [&](std::int32_t align, std::int32_t box_width) -> std::int32_t {
            return width_with_metrics(align, 24, box_width, 60, 50);
        };
        const auto expect_width = [&](std::int32_t align, std::int32_t box_width,
                                      std::int32_t want, const char* what) {
            const std::int32_t got = width_with_box(align, box_width);
            if (got != want) {
                std::fprintf(stderr, "对位模式 %d / 本字形黑框 %d ⇒ 下一格 = %d（期望 %d）\n", align,
                             box_width, got, want);
                return fail(what);
            }
            return true;
        };
        if (!expect_width(1, 11, -50,
                          "对位模式 1：黑框宽只超过「字号 ÷ 2」时也再缩了一次（半格不是字号的一半）")) {
            return false;
        }
        if (!expect_width(1, 20, -30,
                          "对位模式 1：再缩不是「参考盒宽 × 0.5」÷「本字形黑框宽」「−平均字宽」（分母取成量度框 / 分子取成本字形黑框？）")) {
            return false;
        }
        if (!expect_width(2, 20, -50, "对位模式 2 也做了半格再缩（正文：只有 = 1 那一档再缩）")) {
            return false;
        }
        // ⑥f：**奇数参考盒宽** ⇒ 阈值（整数右移）与分子（× 0.5 精确浮点）**不是同一个数**。
        //   取证（`60-ops-forward.md` 的「再缩一次」）：阈值 `0x4584C6` / `0x4584D8 shr ecx,1` /
        //   `0x4584DA cmp` / `0x4584DC jnb`；分子 `0x4584DE fild [ebp+var_78]`（参考盒宽原值）→
        //   `0x4584EE fmul dbl_51D7F8`（× 0.5）→ `0x458516 fdivp` → `0x458518 fimul [ebp+ho]`
        //   → `0x45851E __ftol2_sse`（唯一取整处，向零截断）。
        //   参考盒宽 25 ⇒ **阈值 12**（25 >> 1）、**分子 12.5**（25 × 0.5）。
        //   ① 本字形黑框 20（> 12）、量度框 20、平均字宽 50：
        //      trunc(**12.5** ÷ 20 × (−50)) = trunc(−31.25) = **−31**；
        //      若分子退回整数半格 12 ⇒ trunc(−30) = −30 ⇒ 本条红（R5 的错法）；
        //      若阈值漏掉右移（用 25）⇒ 20 > 25 不成立、不缩 ⇒ −50 ⇒ 本条红。
        //   ② 本字形黑框 13（= 阈值 + 1，恰好在“缩”的一侧）、量度框 13、平均字宽 50：
        //      trunc(12.5 ÷ 13 × (−50)) = trunc(−48.076…) = **−48**；
        //      若阈值写成向上取整（25 ÷ 2 向上 = 13）⇒ 13 > 13 不成立、不缩 ⇒ −50 ⇒ 本条红。
        //   ③ 本字形黑框 12（= 阈值，**不**缩）、量度框 12、平均字宽 50 ⇒ 下一格 = **−50**；
        //      若阈值偏小（如漏算参考盒宽 ⇒ 0）⇒ 会缩成 trunc(−52.08) = −52 ⇒ 本条红。
        //   ★ 这三例把「本字形量度框」与「本字形黑框」取成同一个数 ⇒ 断言**只依赖分子 / 阈值**
        //     （这两格相等 ⇒ 分母取哪一个都不影响结果）；**分母的口径由 ⑥e② 单独判**
        //     （那里黑框 20 / 量度框 60 故意不同）。
        const auto expect_width_odd = [&](std::int32_t glyph_box, std::int32_t want,
                                          const char* what) {
            const std::int32_t got = width_with_metrics(1, 25, glyph_box, glyph_box, 50);
            if (got != want) {
                std::fprintf(stderr,
                             "参考盒宽 25（奇数）/ 本字形黑框 %d ⇒ 下一格 = %d（期望 %d）\n",
                             glyph_box, got, want);
                return fail(what);
            }
            return true;
        };
        if (!expect_width_odd(20, -31,
                              "奇数参考盒宽：再缩的分子不是「参考盒宽 × 0.5」（退回整数半格 ⇒ −30）")) {
            return false;
        }
        if (!expect_width_odd(13, -48,
                              "奇数参考盒宽：阈值不是「参考盒宽右移一位」（向上取整 ⇒ 13 那一档不缩）")) {
            return false;
        }
        if (!expect_width_odd(12, -50,
                              "奇数参考盒宽：黑框宽等于阈值时也缩了（阈值偏小 / 丢掉了参考盒宽）")) {
            return false;
        }
        // ⑥g：**参考量度那次请求的字号 = 当时的 A 支字号**（⛔ 不是这次派生的传参、也不随描述变）。
        //   正文「参考字形 = 常量参考面名 + 固定字符、**按当时的 A 支字号**量的」。
        //   夹具：A 支字号 = 缺省 24（不执行 `0x75`），派生传参 20 ⇒ 两次请求的字号必须是 (24, 20)；
        //   把参考那次的实参写成派生传参 ⇒ (20, 20) ⇒ 本条红。
        {
            auto bytes = make_script_with_body();
            emit_instruction(bytes, 0x2DB, {{0, 1}});
            emit_instruction(bytes, 2, {});
            core::Machine machine(key, table);
            std::vector<std::int32_t> sizes;
            machine.set_font_measurer([&sizes](const std::string& face, std::int32_t size) {
                sizes.push_back(size);
                core::Machine::FontMetrics metrics;
                metrics.line_height = 20;
                if (face == core::Machine::reference_font_face()) {
                    metrics.glyph_box_width = 24;
                } else {
                    metrics.glyph_box_width = 10;
                    metrics.average_width = 10;
                }
                return metrics;
            });
            machine.set_font_substitution(true);
            machine.load_root(bytes);
            if (!machine.step()) return fail("⑥g 夹具停机了");
            if (machine.glyph_height() != 24) {
                std::fprintf(stderr, "⑥g 前置不成立：A 支字号 = %d（期望缺省 24）\n",
                             machine.glyph_height());
                return false;
            }
            core::FontDescription desc;
            desc.face = "Amayui CN";
            machine.derive_font_description(desc, 20);
            if (sizes.size() != 2) {
                std::fprintf(stderr, "⑥g 一次派生的度量请求数 = %zu（期望 2）\n", sizes.size());
                return fail("⑥g 派生没有恰好发两次度量");
            }
            if (sizes[0] != 24) {
                std::fprintf(stderr, "⑥g 参考度量的字号 = %d（期望 24 = 当时的 A 支字号，不是传参 20）\n",
                             sizes[0]);
                return fail("参考字形不是按当时的 A 支字号量的（写成了派生传参 ⇒ 随描述变）");
            }
            if (sizes[1] != 20) {
                std::fprintf(stderr, "⑥g 描述那次度量的字号 = %d（期望传参 20）\n", sizes[1]);
                return fail("描述那次度量的字号不是派生传参");
            }
        }
    }
    return true;
}

// ── 纹理装载（`CORE-OPS-TEXTURE-SET` / `-LOAD`） ──
// 判据（每条都能红）：
// ① 读序 **2 → 1 → 3 → 2** 且清槽在**读完第 3 个之后**：第 3 个读取抛类型异常 ⇒ 纹理槽仍是旧对象、记录未被写
// ② 失败分档：打开 / 锁定失败 ⇒ 记录与槽**保持原值**；读字节失败 / 解码失败 ⇒ **槽空 + 记录来源 = 无**；
//    上传失败（`display:TextureBit` ∉ {16,32}）⇒ 槽里留着**已建出**的对象 + 记录来源 = 无；都是显示消息型异常
// ③ 颜色键按**目标位宽**装配：32 ⇒ A8R8G8B8 全值相等；16 ⇒ **A4R4G4B4**（A 参与，取高 4 位）四段全等
// ④ BMP 分支：位深集合 1 / 4 / 8 / 24 / 32（16 拒）、行序自下而上、行跨距 32 位对齐、1 位 MSB 先
namespace {
// 24 位 BMP（无压缩）：`pixels_bottom_up` 是**自下而上**的 BGR 三元组序列。
std::vector<std::uint8_t> make_bmp24(std::uint32_t width, std::uint32_t height,
                                     const std::vector<std::uint8_t>& pixels_bottom_up) {
    const std::uint32_t row = ((width * 24u + 31u) / 32u) * 4u;
    std::vector<std::uint8_t> bytes(54u + static_cast<std::size_t>(row) * height, 0);
    bytes[0] = 'B';
    bytes[1] = 'M';
    auto put = [&bytes](std::size_t offset, std::uint32_t value) {
        bytes[offset + 0] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
        bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
    };
    put(2, static_cast<std::uint32_t>(bytes.size()));
    put(10, 54u);
    put(14, 40u);
    put(18, width);
    put(22, height);
    bytes[26] = 1;  // planes
    bytes[28] = 24;
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t s = (static_cast<std::size_t>(y) * width + x) * 3;
            const std::size_t d = 54u + static_cast<std::size_t>(y) * row + x * 3;
            bytes[d + 0] = pixels_bottom_up[s + 0];
            bytes[d + 1] = pixels_bottom_up[s + 1];
            bytes[d + 2] = pixels_bottom_up[s + 2];
        }
    }
    return bytes;
}

// 1 位 BMP（调色板 2 项，自下而上；每字节 8 像素、最高位先）。
std::vector<std::uint8_t> make_bmp1(std::uint32_t width, std::uint32_t height,
                                    std::uint8_t bottom_row_byte, std::uint8_t top_row_byte) {
    const std::uint32_t row = ((width * 1u + 31u) / 32u) * 4u;
    std::vector<std::uint8_t> bytes(54u + 8u + static_cast<std::size_t>(row) * height, 0);
    bytes[0] = 'B';
    bytes[1] = 'M';
    auto put = [&bytes](std::size_t offset, std::uint32_t value) {
        bytes[offset + 0] = static_cast<std::uint8_t>(value);
        bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
        bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
        bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
    };
    put(2, static_cast<std::uint32_t>(bytes.size()));
    put(10, 54u + 8u);
    put(14, 40u);
    put(18, width);
    put(22, height);
    bytes[26] = 1;
    bytes[28] = 1;
    put(46, 2u);  // 使用颜色数 = 2
    // 调色板：0 = 黑、1 = 白（B / G / R / 不使用）
    bytes[54] = 0; bytes[55] = 0; bytes[56] = 0;
    bytes[58] = 255; bytes[59] = 255; bytes[60] = 255;
    // 行序自下而上：先写底行
    bytes[54 + 8] = bottom_row_byte;
    bytes[54 + 8 + row] = top_row_byte;
    return bytes;
}

// 16 位 BMP（位深集合之外 ⇒ 必须解码失败）。
std::vector<std::uint8_t> make_bmp16(std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint8_t> bytes = make_bmp24(width, height,
                                                 std::vector<std::uint8_t>(
                                                     static_cast<std::size_t>(width) * height * 3, 0));
    // 把位深改成 16（像素数据长度不再匹配也无所谓：位深先被拒）。
    bytes[28] = 16;
    return bytes;
}

// 临时安装树：`make_alf_ai()` 的索引 + 一个目录；文件按需写（松散文件优先）。
struct TempInstall {
    std::filesystem::path root;
    core::Install install;

    explicit TempInstall(std::uint32_t seed) {
        root = std::filesystem::temp_directory_path() /
               ("amayui-cxx-texture-" + std::to_string(seed));
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root);
        install.root = root;
        install.index = core::parse_alf_index(make_alf_ai());
    }
    ~TempInstall() {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }
    void write(const std::string& name, const std::vector<std::uint8_t>& bytes) const {
        std::ofstream out(root / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
};
}  // namespace

bool check_texture_set(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "texture-set: %s\n", what);
        return false;
    };
    // 一个文件 = `make_alf_ai()` 的 index 0（松散文件 SYSTEM4.BIN 优先）。
    // ★ 颜色键用 **−1**（归一成 0 = **无键**）：这些用例判的是装载与像素布局，不是命中。
    const auto run_texture = [&](core::Machine& machine) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x1F9, {{0, 0}, {0, 5}, {0, static_cast<std::uint32_t>(-1)}});
        emit_instruction(bytes, 2, {});
        machine.load_root(bytes);
        return machine.step();
    };

    // ① 读序：第 3 个操作数读取抛类型异常 ⇒ 纹理槽仍是旧对象、记录未被写过。
    {
        TempInstall temp(1);
        temp.write("SYSTEM4.BIN", make_bmp24(1, 1, {0, 0, 0}));
        core::Machine machine(key, table, &temp.install);
        machine.config_mut().set_int("display:TextureBit", 32);
        auto& old = machine.texture_entry(5);
        old.slot.present = true;
        old.slot.width = 3;
        old.slot.height = 2;
        old.slot.rgba.assign(3u * 2u * 4u, 0x7F);
        old.record.file = 9;
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x1F9, {{0, 0}, {0, 5}, {2, 0}});  // 第 3 个 = 内联文本（读整必抛）
        emit_instruction(bytes, 2, {});
        machine.load_root(bytes);
        if (machine.step() || !machine.halted()) return fail("0x1f9 第 3 个操作数不可读时没有抛");
        if (machine.last_stop().message != "read-int-unsupported-tag:2") {
            std::fprintf(stderr, "停因 = %s\n", machine.last_stop().message.c_str());
            return fail("0x1f9 的读序不是 2 → 1 → 3（第 3 个不是那个先抛的？）");
        }
        const auto* slot = machine.texture_slot(5);
        if (slot == nullptr || !slot->present || slot->width != 3 || slot->height != 2) {
            return fail("第 3 个操作数抛异常时纹理槽被清掉了（清槽发生得太早）");
        }
        const auto* record = machine.texture_record(5);
        if (record == nullptr || record->file != 9) {
            return fail("第 3 个操作数抛异常时纹理记录被写了");
        }
    }

    // ② 失败分档。
    {
        // 解码失败：槽空、记录来源 = 无、显示消息型异常。
        TempInstall temp(2);
        temp.write("SYSTEM4.BIN", {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
        core::Machine machine(key, table, &temp.install);
        machine.config_mut().set_int("display:TextureBit", 32);
        auto& old = machine.texture_entry(5);
        old.slot.present = true;
        old.slot.width = 3;
        old.slot.height = 2;
        old.slot.rgba.assign(3u * 2u * 4u, 0x7F);
        old.record.file = 9;
        if (run_texture(machine) || !machine.halted()) return fail("解码失败时没有抛");
        if (machine.last_display_code() != "texture-decode-failed") {
            std::fprintf(stderr, "解码失败的错误码 = %s\n", machine.last_display_code().c_str());
            return fail("解码失败不是显示消息型异常（错误码不对）");
        }
        const auto* slot = machine.texture_slot(5);
        if (slot != nullptr && slot->present) return fail("解码失败后纹理槽里还留着对象");
        const auto* record = machine.texture_record(5);
        if (record == nullptr || record->file != -1) return fail("解码失败后记录来源不是「无」");
    }
    {
        // 打开 / 锁定失败：记录与槽**保持原值**。
        TempInstall temp(3);  // 一个文件都不写 ⇒ 归档也打不开
        core::Machine machine(key, table, &temp.install);
        machine.config_mut().set_int("display:TextureBit", 32);
        auto& old = machine.texture_entry(5);
        old.slot.present = true;
        old.slot.width = 3;
        old.slot.height = 2;
        old.slot.rgba.assign(3u * 2u * 4u, 0x7F);
        old.record.file = 9;
        if (run_texture(machine) || !machine.halted()) return fail("打开失败时没有抛");
        if (machine.last_display_code() != "texture-open-failed") {
            std::fprintf(stderr, "打开失败的错误码 = %s\n", machine.last_display_code().c_str());
            return fail("打开 / 锁定失败不是显示消息型异常（或类分错了）");
        }
        const auto* slot = machine.texture_slot(5);
        if (slot == nullptr || !slot->present || slot->width != 3) {
            return fail("打开 / 锁定失败时纹理槽被清了（应当保持原值）");
        }
        const auto* record = machine.texture_record(5);
        if (record == nullptr || record->file != 9) {
            return fail("打开 / 锁定失败时记录被写了（应当保持原值）");
        }
    }
    {
        // 读字节失败：槽空、记录来源 = 无（与解码失败同态）。
        TempInstall temp(4);
        temp.write("DATA1.ALF", {0x00, 0x00, 0x00, 0x00});  // 4 字节，但索引说 16 ⇒ 短读
        core::Machine machine(key, table, &temp.install);
        machine.config_mut().set_int("display:TextureBit", 32);
        auto& old = machine.texture_entry(5);
        old.slot.present = true;
        old.record.file = 9;
        if (run_texture(machine) || !machine.halted()) return fail("读字节失败时没有抛");
        if (machine.last_display_code() != "texture-read-failed") {
            std::fprintf(stderr, "读字节失败的错误码 = %s\n", machine.last_display_code().c_str());
            return fail("读字节失败与解码失败不同态（或类分错了）");
        }
        const auto* slot = machine.texture_slot(5);
        if (slot != nullptr && slot->present) return fail("读字节失败后纹理槽里还留着对象");
        const auto* record = machine.texture_record(5);
        if (record == nullptr || record->file != -1) return fail("读字节失败后记录来源不是「无」");
    }
    {
        // 上传失败（`display:TextureBit` ∉ {16,32}）：槽里留着**已建出**的对象、记录来源 = 无。
        TempInstall temp(5);
        temp.write("SYSTEM4.BIN", make_bmp24(1, 1, {0, 0, 0}));
        core::Machine machine(key, table, &temp.install);
        machine.config_mut().set_int("display:TextureBit", 8);
        if (run_texture(machine) || !machine.halted()) return fail("非法纹理位深时没有抛");
        if (machine.last_display_code() != "texture-upload-failed") {
            std::fprintf(stderr, "上传失败的错误码 = %s\n", machine.last_display_code().c_str());
            return fail("非法纹理位深不是上传失败");
        }
        const auto* slot = machine.texture_slot(5);
        if (slot == nullptr || !slot->present || slot->width != 1 || slot->height != 1) {
            return fail("上传失败时槽里没有留下已建出的对象");
        }
        const auto* record = machine.texture_record(5);
        if (record == nullptr || record->file != -1) return fail("上传失败后记录来源不是「无」");
    }

    // ③ 颜色键：32 位 = A8R8G8B8 全值相等；16 位 = A4R4G4B4（A 参与）。
    {
        // 像素：黑（BGR 0,0,0）与红（BGR 0,0,255），自下而上 ⇒ 两行。
        TempInstall temp(6);
        temp.write("SYSTEM4.BIN", make_bmp24(1, 2, {0, 0, 0, 0, 0, 255}));
        core::Machine machine(key, table, &temp.install);
        machine.config_mut().set_int("display:TextureBit", 32);
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x1F9, {{0, 0}, {0, 5}, {0, 0}});  // 键 0 ⇒ 归一成 0xFF000000（不透明黑）
        emit_instruction(bytes, 2, {});
        machine.load_root(bytes);
        if (!machine.step()) return fail("32 位颜色键夹具停机了");
        const auto* slot = machine.texture_slot(5);
        if (slot == nullptr || slot->rgba.size() != 8) return fail("32 位颜色键夹具没有纹理");
        // 行序自下而上 ⇒ 翻正后：第 0 行 = 红（BGR 0,0,255）、第 1 行 = 黑。
        // 黑像素命中 ⇒ 四段全 0；红像素不动。
        if (slot->rgba[7] != 0 || slot->rgba[4] != 0) return fail("32 位键没有清掉不透明黑像素");
        if (slot->rgba[3] != 255 || slot->rgba[0] != 255) {
            std::fprintf(stderr, "rgba = %u %u %u %u | %u %u %u %u (file=%d)\n", slot->rgba[0],
                         slot->rgba[1], slot->rgba[2], slot->rgba[3], slot->rgba[4], slot->rgba[5],
                         slot->rgba[6], slot->rgba[7],
                         machine.texture_record(5) ? machine.texture_record(5)->file : -99);
            return fail("32 位键误清了红像素");
        }
    }
    {
        // 16 位：像素 R = 0x0F（高 4 位 = 0），键 R = 0x0A（高 4 位 = 0）⇒ 16 位命中；32 位不命中。
        TempInstall temp(7);
        temp.write("SYSTEM4.BIN", make_bmp24(1, 1, {0, 0, 0x0F}));
        const auto load = [&](core::Machine& machine, std::int32_t bits) {
            machine.config_mut().set_int("display:TextureBit", bits);
            auto bytes = make_script_with_body();
            emit_instruction(bytes, 0x1F9, {{0, 0}, {0, 5}, {0, 0x000A00}});
            emit_instruction(bytes, 2, {});
            machine.load_root(bytes);
            if (!machine.step()) return false;
            return true;
        };
        core::Machine sixteen_machine(key, table, &temp.install);
        if (!load(sixteen_machine, 16)) return fail("16 位颜色键夹具停机了");
        const auto* sixteen = sixteen_machine.texture_slot(5);
        if (sixteen == nullptr || sixteen->rgba.size() != 4) return fail("16 位颜色键夹具没有纹理");
        if (sixteen->rgba[3] != 0) return fail("16 位键没有按 4 位通道命中（A 参与的全等）");
        core::Machine thirty_two_machine(key, table, &temp.install);
        if (!load(thirty_two_machine, 32)) return fail("32 位对照夹具停机了");
        const auto* thirty_two = thirty_two_machine.texture_slot(5);
        if (thirty_two == nullptr || thirty_two->rgba.size() != 4) return fail("32 位对照没有纹理");
        if (thirty_two->rgba[3] == 0) {
            return fail("32 位键按 16 位的口径命中了（两种目标位宽被混成一条）");
        }
    }

    // ④ BMP 分支：16 位拒；1 位 + 行序自下而上 + MSB 先。
    {
        TempInstall temp(8);
        temp.write("SYSTEM4.BIN", make_bmp16(1, 1));
        core::Machine machine(key, table, &temp.install);
        machine.config_mut().set_int("display:TextureBit", 32);
        if (run_texture(machine) || !machine.halted()) return fail("16 位 BMP 没有被拒");
        if (machine.last_display_code() != "texture-decode-failed") {
            return fail("16 位 BMP 不是解码失败（位深集合写宽了）");
        }
    }
    {
        // 8 像素宽 × 2 行：底行 = 全 1（白）、顶行 = 全 0（黑）⇒ 翻正后**第一行是黑**。
        TempInstall temp(9);
        temp.write("SYSTEM4.BIN", make_bmp1(8, 2, 0xFF, 0x00));
        core::Machine machine(key, table, &temp.install);
        machine.config_mut().set_int("display:TextureBit", 32);
        if (!run_texture(machine)) {
            std::fprintf(stderr, "1 位 BMP 装载停因 = %s\n", machine.last_stop().message.c_str());
            return fail("1 位 BMP 装载失败（位深集合或调色板路径）");
        }
        const auto* slot = machine.texture_slot(5);
        if (slot == nullptr || slot->width != 8 || slot->height != 2) return fail("1 位 BMP 尺寸不对");
        const auto at = [slot](std::size_t x, std::size_t y) {
            return slot->rgba[(y * slot->width + x) * 4];
        };
        if (at(0, 0) != 0 || at(7, 0) != 0) return fail("1 位 BMP 的行序没有翻正（顶行不是黑）");
        if (at(0, 1) != 255 || at(7, 1) != 255) return fail("1 位 BMP 的位序/调色板不对（底行不是白）");
        if (slot->rgba[3] != 255) return fail("1 位 BMP 的 A 没有补成满位");
    }
    return true;
}

// ── 纹理位深 `display:TextureBit` 的**三档 + 缺省来源**（`CORE-OPS-TEXTURE-LOAD` / `-SET`） ──
// 正文写死三点（`60-ops-texture.md` 的「上传纹理」条与「配置」条的构造期缺省面）：
//   ① 该键在引擎里**必有值**：配置库**构造期**就写内建默认 **32**（早于图形设备建立）⇒ 玩家文件只能
//      覆盖它、不会让它消失。★ 缺省**不在读取端**：读取器缺键返回 0（配置库那条规则），与本条无关。
//   ② 读取端**不做替换**：上传三档 = 16 ⇒ A4R4G4B4、32 ⇒ A8R8G8B8、**其它任何整数 ⇒ 上传失败**。
//      显式写 0 与「缺键读出的 0」**同值同后果** ⇒ 都落第三档（⛔ 不许把 0 抬成 32）。
//   ③ 第三档的失败发生在**上传**（建表面是二档、不失败）⇒ 槽里留着**已建出**的对象、记录来源 = 无。
// 判据（八条，每条都会红）：
//   ① `default_installed`：**不加载任何玩家文件** ⇒ `display:TextureBit` 读出 **32**（删掉构造期那一条 ⇒ 红）；
//   ② `explicit_zero`：显式 0 ⇒ 装载**失败**（把 0 / 缺键抬成 32 ⇒ 红）；
//   ③ `bit16_ok`：16 ⇒ 成功、`0x216` 读回文件号、颜色键按 A4R4G4B4 命中不透明黑（按 8 位比 ⇒ 红）；
//   ④ `bit32_ok`：32 ⇒ 成功、`0x216` 读回文件号、颜色键按 32 位全值相等（按 16 位比 ⇒ 红）；
//   ⑤ `bit24_fail`：24 ⇒ 失败（把它当"其它值按 32" ⇒ 红）；
//   ⑥ `surface_before_failure`：②/⑤ 失败之后槽里**仍有对象**（失败挪到建表面之前 ⇒ 红）；
//   ⑦ `default_used_by_loader`：只用构造期的默认值装载 ⇒ 成功（缺省真的来自构造期 ⇒ 红）；
//   ⑧ `reader_never_substitutes`：库里**没有**这个键 ⇒ 读到 0 ⇒ 第三档失败（读取端把缺键抬成 32 ⇒ 红）。
bool check_texture_bit_default_source(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "texture-bit: %s\n", what);
        return false;
    };
    // ① 缺省来源：一个**全新的**配置库 + 产品那条构造期登记（⛔ 不 load_player_file、⛔ 不 load_defaults）。
    {
        core::ConfigStore config;
        config.register_engine_defaults();
        if (!config.has_named("display:TextureBit")) {
            return fail("配置库构造期没有登记 display:TextureBit（⇒ 读取端只能读到 0 / 自己补缺省）");
        }
        const std::int32_t bits = config.get_int("display:TextureBit");
        if (bits != 32) {
            std::fprintf(stderr, "不加载玩家文件时 display:TextureBit = %d（期望 32）\n", bits);
            return fail("构造期写的缺省不是 32");
        }
    }
    // 一次按位深装载的全部观察量（栈上取完就抄出来 —— 不持有 Machine 指针）。
    struct BitProbe {
        bool halted = false;
        std::string display_code;
        int record_file = -999;   // 纹理记录的来源文件格（`0x216` 读的同一格）
        int reread = -999;        // `0x216` 写到全局整型 1 的值
        bool slot_present = false;
        int width = 0;
        int height = 0;
        std::vector<std::uint8_t> rgba;
    };
    // 夹具图：1×2、自下而上 = 黑、红（BGR）⇒ 翻正后第 0 行红、第 1 行黑（两行不同色 ⇒ "键命中了哪一块"可分辨）。
    // `explicit_write` = 是否**显式**写该键（false ⇒ 只用构造期登记的那份默认值）。
    // `install_defaults` = 是否走产品那条"构造期登记"（false ⇒ 库里**没有**这个键 ⇒ 读到 0 走第三档）。
    const auto probe_bits = [&](std::int32_t bits, bool explicit_write, bool install_defaults = true) {
        TempInstall temp(100 + static_cast<std::uint32_t>(bits) * 10 + (explicit_write ? 1u : 0u) +
                         (install_defaults ? 0u : 1000u));
        temp.write("SYSTEM4.BIN", make_bmp24(1, 2, {0, 0, 0, 0, 0, 255}));
        core::ConfigStore config;
        if (install_defaults) config.register_engine_defaults();  // 产品路径：构造期登记在先（缺省 32）
        if (explicit_write) config.set_int("display:TextureBit", bits);
        core::Machine machine(key, table, &temp.install, std::move(config));
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x1F9, {{0, 0}, {0, 5}, {0, 0}});  // 颜色键 0 ⇒ 归一成 0xFF000000
        emit_instruction(bytes, 0x216, {{3, 1}, {0, 5}});          // 记录来源 → 全局整型 1
        emit_instruction(bytes, 2, {});
        machine.load_root(bytes);
        BitProbe probe;
        for (int i = 0; i < 2; ++i) {
            if (!machine.step()) {
                probe.halted = true;
                break;
            }
        }
        probe.display_code = machine.last_display_code();
        const core::TextureRecord* record = machine.texture_record(5);
        probe.record_file = record == nullptr ? -999 : record->file;
        probe.reread = machine.global_int(1);
        if (const auto* slot = machine.texture_slot(5)) {
            probe.slot_present = slot->present;
            probe.width = slot->width;
            probe.height = slot->height;
            probe.rgba = slot->rgba;
        }
        return probe;
    };
    // ② / ⑤ / ⑥ 第三档（**其它任何整数**）：显式 0 与 24 都必须失败，且**表面已建出**、记录来源 = 无。
    for (const std::int32_t bits : {0, 24}) {
        const BitProbe probe = probe_bits(bits, true);
        if (!probe.halted) {
            std::fprintf(stderr, "display:TextureBit = %d 时没有停机（装载成功了？）\n", bits);
            return fail(bits == 0 ? "显式 0 没有落第三档（读取端把 0 / 缺键抬成了 32？）"
                                  : "24 没有落第三档（被当成其它值按 32 了？）");
        }
        if (probe.display_code != "texture-upload-failed") {
            std::fprintf(stderr, "display:TextureBit = %d 的失败码 = %s\n", bits,
                         probe.display_code.c_str());
            return fail("第三档的失败不是『上传失败』（失败点挪到别处了？）");
        }
        if (probe.record_file != -1) {
            std::fprintf(stderr, "display:TextureBit = %d 之后记录来源 = %d（期望 −1）\n", bits,
                         probe.record_file);
            return fail("第三档失败后记录来源不是「无」");
        }
        // ⑥ 建表面（二档、不失败）在上传（三档）之前 ⇒ 槽里留着**已建出**的 1×2 对象。
        if (!probe.slot_present || probe.width != 1 || probe.height != 2) {
            std::fprintf(stderr,
                         "display:TextureBit = %d 失败之后槽 = present %d / %dx%d（期望 1×2 已建出）\n",
                         bits, probe.slot_present ? 1 : 0, probe.width, probe.height);
            return fail("上传失败时槽里没有留下已建出的对象（失败挪到建表面之前了？）");
        }
    }
    // ③ / ④ 16 与 32 必须**成功**：记录来源 = 文件号 0、`0x216` 读回 0、颜色键按各自的目标位宽命中不透明黑
    //   （第 1 行四段全 0），红像素（第 0 行）不许被清。
    for (const std::int32_t bits : {16, 32}) {
        const BitProbe probe = probe_bits(bits, true);
        if (probe.halted) {
            std::fprintf(stderr, "display:TextureBit = %d 时停机了（%s）\n", bits,
                         probe.display_code.c_str());
            return fail(bits == 16 ? "16 位档装载失败（三档写错了？）" : "32 位档装载失败");
        }
        if (probe.record_file != 0 || probe.reread != 0) {
            std::fprintf(stderr, "display:TextureBit = %d ⇒ 记录来源 = %d / 0x216 读回 = %d（期望 0 / 0）\n",
                         bits, probe.record_file, probe.reread);
            return fail("成功档的记录来源不是文件号（或 0x216 读的不是同一格）");
        }
        if (probe.rgba.size() != 8) {
            std::fprintf(stderr, "display:TextureBit = %d 的夹具没有 1×2 纹理\n", bits);
            return fail("成功档没有纹理（尺寸 / 像素数不对）");
        }
        if (probe.rgba[0] != 255 || probe.rgba[3] != 255) {
            std::fprintf(stderr, "display:TextureBit = %d 误清了红像素（rgba[0..3] = %u %u %u %u）\n",
                         bits, probe.rgba[0], probe.rgba[1], probe.rgba[2], probe.rgba[3]);
            return fail("颜色键把不该命中的像素也清了");
        }
        for (std::size_t i = 4; i < 8; ++i) {
            if (probe.rgba[i] != 0) {
                std::fprintf(stderr,
                             "display:TextureBit = %d 没有按目标位宽命中不透明黑（第 1 行 = %u %u %u %u）\n",
                             bits, probe.rgba[4], probe.rgba[5], probe.rgba[6], probe.rgba[7]);
                return fail(bits == 16 ? "16 位档的颜色键没有按 A4R4G4B4 命中（按 8 位比了？）"
                                       : "32 位档的颜色键没有按全值相等命中（按 16 位比了？）");
            }
        }
    }
    // ⑦ 缺键不再被读取端替换：**只**用构造期默认（显式写 false）⇒ 与显式 32 同一条成功路径。
    //   ★ 它同时钉住"缺省来自构造期"：把构造期那一条删掉 ⇒ 这里读到 0 ⇒ 落第三档 ⇒ 红。
    {
        const BitProbe probe = probe_bits(32, false);
        if (probe.halted || probe.record_file != 0) {
            std::fprintf(stderr, "用构造期默认值装载 ⇒ 停因 %s / 记录来源 %d（期望成功 / 0）\n",
                         probe.display_code.c_str(), probe.record_file);
            return fail("构造期登记的默认值没有生效（缺键 ⇒ 第三档）");
        }
    }
    // ⑧ 读取端**不替换**：库里**没有**这个键（不登记构造期默认、也不显式写）⇒ 读到 0 ⇒ 第三档**失败**。
    //   ★ 这条直接钉「缺键 ⇒ 32」：只要读取端把缺键抬成 32，这次装载就会成功 ⇒ 本条红。
    //     （⑦ 抓不到它 —— ⑦ 的库里那个键是构造期写进去的，`has_named` 恒真。）
    {
        const BitProbe probe = probe_bits(0, false, /*install_defaults=*/false);
        if (!probe.halted) {
            std::fprintf(stderr, "库里没有该键时装载成功了（记录来源 %d）\n", probe.record_file);
            return fail("读取端把「缺键」抬成了 32（缺键 ⇒ 32 是正文没有的替换）");
        }
        if (probe.display_code != "texture-upload-failed" || probe.record_file != -1) {
            std::fprintf(stderr, "库里没有该键 ⇒ 停因 %s / 记录来源 %d（期望上传失败 / −1）\n",
                         probe.display_code.c_str(), probe.record_file);
            return fail("缺键没有落第三档（读取端自己补了一个缺省？）");
        }
    }
    return true;
}

// ── 帧流程（`CORE-OPS-FRAME-EXIT` 的 −11 支 / `CORE-OPS-FRAME-CALL-FRAME` / `0x8c`） ──
// ── 文本层：**场景绘制模式门** + `0x70` 的读序（`ADV-TEXT-LAYER-RECT`） ──
// 判据（每条都能红）：
// ① 场景绘制模式 ≠ 1 ⇒ `0x70` **不建 / 不改纹理槽**（旧路径只共用编号；本版不规定它的正面行为）、
//    `0x71` **仍然**复位文本游标 / 布局格 / 揭示游标，但**不清像素**、**不建逐字条目**；
// ② 场景绘制模式 = 1 ⇒ 画布照建、逐字条目照建（对照组必须与 ① 不同，否则夹具分辨不出两档）；
// ③ `0x70` 的读序 = **5 → 4 → 3 → 2 → 1**（读第 5 个就抛时，前 4 个一个都没读）。
bool check_text_layer_mode_gate(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "text-layer-mode: %s\n", what);
        return false;
    };
    const auto layer_script = [](bool append_text) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x80, {{0, 9}});
        emit_instruction(bytes, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x213, {{0, 9}, {0, 190000}, {0, 40}});
        emit_instruction(bytes, 0x79, {{0, 9}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x75, {{0, 16}});
        emit_instruction(bytes, 0x74, {{0, 0}});  // 间隔 0 ⇒ 立即抽干
        emit_instruction(bytes, 0x71, {{0, 9}});
        if (append_text) emit_instruction(bytes, 0x6E, {{0, 0}, {5, 0}});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    const auto run_with_mode = [&](bool texture_path, bool append_text) {
        core::Machine machine(key, table, nullptr,
                              texture_path ? scene_step_config() : core::ConfigStore{});
        machine.write_text(core::Operand{5, 0}, "AB");
        machine.load_root(layer_script(append_text));
        for (int i = 0; i < 32 && !machine.halted(); ++i) machine.step();
        return machine;
    };
    const auto id = core::Machine::text_layer_texture_id(9);
    {
        core::Machine legacy = run_with_mode(false, false);
        // ★ 判据只看 0x70 / 0x71：这两条在模式 ≠ 1 时**不碰纹理槽**（旧路径的正面行为不在本版）。
        //   ⚠ 追加路径（0x6e / 0x196）在本模型里仍会走宿主兜底建画布 ⇒ 这里不断言它。
        if (legacy.texture_slot(id) != nullptr) {
            return fail("场景绘制模式 ≠ 1 时 0x70 建了纹理画布（旧路径不该碰纹理槽）");
        }
        // 追加路径也走一遍：模式 ≠ 1 时**一条逐字条目都不许有**。
        core::Machine legacy_append = run_with_mode(false, true);
        bool any_item = false;
        for (std::uint32_t k = 0; k < 40; ++k) {
            if (legacy_append.draw_item(190000u + k) != nullptr) any_item = true;
        }
        if (any_item) return fail("场景绘制模式 ≠ 1 时仍建了逐字绘制条目");
        if (legacy.text_layer_cursor_x(9) != 5 || legacy.text_layer_cursor_y(9) != 5) {
            return fail("场景绘制模式 ≠ 1 时 0x71 没有复位文本游标（这一项不分模式）");
        }
        core::Machine texture = run_with_mode(true, true);
        if (texture.texture_slot(id) == nullptr) {
            return fail("场景绘制模式 = 1 时没有建出纹理画布");
        }
        bool texture_item = false;
        for (std::uint32_t k = 0; k < 40; ++k) {
            if (texture.draw_item(190000u + k) != nullptr) texture_item = true;
        }
        if (!texture_item) return fail("场景绘制模式 = 1 时没有建出逐字绘制条目");
    }
    const auto stop_after = [&](int throwing_index) {
        auto bytes = make_script_with_body();
        std::vector<std::pair<std::uint32_t, std::uint32_t>> operands = {
            {0, 9}, {0, 200}, {0, 120}, {0, 5}, {0, 5}};
        operands[static_cast<std::size_t>(throwing_index)] = {11, 0};
        emit_instruction(bytes, 0x80, {{0, 9}});
        emit_instruction(bytes, 0x70,
                         {operands[0], operands[1], operands[2], operands[3], operands[4]});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table, nullptr, scene_step_config());
        machine.load_root(bytes);
        if (!machine.step()) return std::string("layer-0x80-stop");
        if (machine.step()) return std::string("no-throw");
        return machine.last_stop().message;
    };
    if (stop_after(4) != "read-int-unsupported-tag:11") {
        std::fprintf(stderr, "op5 的停因 = %s\n", stop_after(4).c_str());
        return fail("0x70 没有从第 5 个操作数开始读");
    }
    if (stop_after(3) != "read-int-unsupported-tag:11") {
        return fail("0x70 的第 4 个操作数没有在第 5 个之后读");
    }
    if (stop_after(0) != "read-int-unsupported-tag:11") {
        return fail("0x70 的第 1 个操作数没有最后读");
    }
    return true;
}

// ── 文本层 `0x303` 对齐：逐行平移（spec `ADV-TEXT-DRAW-POSITION`） ──
// 判据（每条都能红）：
// ① 模式 1：行心 = 层左 + 该行起点 + 基准（层左 0 / 原点 20 / 基准 100 ⇒ 120，不是 100）；
// ② 两行宽度不同：两行各自 120；只有整块并集中心 120 ⇒ 块级近似，必红；
// ③ 模式 2：行右界 = 层左 + 起点 + 基准（120），不是基准 100；
// ④ 模式 0：无平移（首字仍在原点 20）；
// ⑤ `0x303` 写在追加之后：下一次绘制必须生效（重算后行心 120）；
// ⑥ 模式 1 → 0：上一次的 Δ 必须被清掉（首字回到 20，不是 110）；
// ⑦ `0x198` 移动层：x / y 都跟着走（层左 50 / 上 30 ⇒ 行心 170、行顶 50），且对齐**不写回**层记录。
bool check_text_layer_align(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    struct LineBox {
        int min_x = 1 << 30;
        int max_x = -(1 << 30);
        int min_y = 1 << 30;
        int max_y = -(1 << 30);
        bool any = false;
    };
    struct RunResult {
        std::map<int, LineBox> lines;  // 键 = 该行绘制项的 aux.y（同一行共享）
        int f0c = 0;
        int f10 = 0;
    };
    const auto collect = [](const core::Machine& machine) {
        std::map<int, LineBox> lines;
        for (std::uint32_t k = 0; k < 40; ++k) {
            const core::DrawItem* item = machine.draw_item(190000u + k);
            if (item == nullptr) continue;
            const int x0 = static_cast<int>(item->aux[0]);
            const int y0 = static_cast<int>(item->aux[1]);
            const int x1 = x0 + (item->right - item->left);
            const int y1 = y0 + (item->bottom - item->top);
            LineBox& line = lines[y0];
            line.min_x = std::min(line.min_x, x0);
            line.max_x = std::max(line.max_x, x1);
            line.min_y = std::min(line.min_y, y0);
            line.max_y = std::max(line.max_y, y1);
            line.any = true;
        }
        return lines;
    };
    const auto run = [&](int mode, bool two_lines, bool late_303, int post_mode = -1,
                         int move_x = 0, int move_y = 0) {
        auto script = make_script_with_body();
        emit_instruction(script, 0x80, {{0, 9}});
        emit_instruction(script, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x213, {{0, 9}, {0, 190000}, {0, 40}});
        if (!late_303) emit_instruction(script, 0x303, {{0, 9}, {0, mode}, {0, 100}});
        emit_instruction(script, 0x79, {{0, 9}, {0, 20}, {0, 20}});
        emit_instruction(script, 0x75, {{0, 16}});
        emit_instruction(script, 0x74, {{0, 0}});
        emit_instruction(script, 0x71, {{0, 9}});
        emit_instruction(script, 0x6E, {{0, 0}, {5, 0}});
        if (two_lines) {
            emit_instruction(script, 0x6F, {{0, 9}});
            emit_instruction(script, 0x6E, {{0, 0}, {5, 1}});
        }
        if (late_303) emit_instruction(script, 0x303, {{0, 9}, {0, mode}, {0, 100}});
        if (post_mode >= 0) emit_instruction(script, 0x303, {{0, 9}, {0, post_mode}, {0, 100}});
        if (move_x != 0 || move_y != 0) {
            emit_instruction(script, 0x198, {{0, 9}, {0, move_x}, {0, move_y}});
        }
        emit_instruction(script, 0x72, {{0, 9}});
        emit_instruction(script, 2, {});
        core::Machine machine(key, table, nullptr, scene_step_config());
        machine.write_text(core::Operand{5, 0}, "AB");
        machine.write_text(core::Operand{5, 1}, "ABCD");
        machine.load_root(script);
        for (int i = 0; i < 40 && !machine.halted(); ++i) machine.step();
        return RunResult{collect(machine), machine.list_record(9).f0c, machine.list_record(9).f10};
    };
    const auto center = [](const LineBox& line) { return (line.min_x + line.max_x) / 2; };

    // ① 单行、模式 1：行心 = 0 + 20 + 100 = 120；对齐不写回 f0c/f10。
    {
        const RunResult result = run(1, false, false);
        if (result.lines.size() != 1 || !result.lines.begin()->second.any) {
            std::fprintf(stderr, "text-layer-align: 单行模式 1 没有一条行\n");
            return false;
        }
        const int cx = center(result.lines.begin()->second);
        if (std::abs(cx - 120) > 2) {
            std::fprintf(stderr, "text-layer-align: 单行模式 1 行心=%d，期望 120（层左 0 + 原点 20 + 基准 100）\n", cx);
            return false;
        }
        if (result.f0c != 0 || result.f10 != 0) {
            std::fprintf(stderr, "text-layer-align: 对齐写回了层记录（f0c=%d f10=%d，应为 0/0）\n",
                         result.f0c, result.f10);
            return false;
        }
    }
    // ② 两行宽度不同、模式 1：两行各自 120；只有整块并集中心满足 ⇒ 块级近似。
    {
        const RunResult result = run(1, true, false);
        if (result.lines.size() != 2) {
            std::fprintf(stderr, "text-layer-align: 两行模式 1 只看到 %zu 行\n", result.lines.size());
            return false;
        }
        for (const auto& [y, line] : result.lines) {
            (void)y;
            const int cx = center(line);
            if (std::abs(cx - 120) > 2) {
                std::fprintf(stderr, "text-layer-align: 两行模式 1 某行行心=%d，期望 120（块级近似会偏）\n", cx);
                return false;
            }
        }
    }
    // ③ 模式 2：行右界 = 层左 + 起点 + 基准 = 120。
    {
        const RunResult result = run(2, false, false);
        if (result.lines.size() != 1) {
            std::fprintf(stderr, "text-layer-align: 模式 2 没有一条行\n");
            return false;
        }
        const int right = result.lines.begin()->second.max_x;
        if (std::abs(right - 120) > 2) {
            std::fprintf(stderr, "text-layer-align: 模式 2 行右界=%d，期望 120\n", right);
            return false;
        }
    }
    // ④ 模式 0：无平移，首字仍在原点 20。
    {
        const RunResult result = run(0, false, false);
        if (result.lines.size() != 1 || std::abs(result.lines.begin()->second.min_x - 20) > 1) {
            std::fprintf(stderr, "text-layer-align: 模式 0 首字左端=%d，期望 20（无平移）\n",
                         result.lines.empty() ? -1 : result.lines.begin()->second.min_x);
            return false;
        }
    }
    // ⑤ 0x303 写在追加之后：下次绘制必须生效（行心 120）。
    {
        const RunResult result = run(1, false, true);
        if (result.lines.size() != 1) {
            std::fprintf(stderr, "text-layer-align: 后置 0x303 没有一条行\n");
            return false;
        }
        const int cx = center(result.lines.begin()->second);
        if (std::abs(cx - 120) > 2) {
            std::fprintf(stderr, "text-layer-align: 后置 0x303 行心=%d，期望 120（追加时还没设对齐）\n", cx);
            return false;
        }
    }
    // ⑥ 模式 1 → 0：上一次的 Δ 必须被清掉（首字回到 20）。
    {
        const RunResult result = run(1, false, false, 0);
        if (result.lines.size() != 1 || std::abs(result.lines.begin()->second.min_x - 20) > 1) {
            std::fprintf(stderr, "text-layer-align: 1→0 首字左端=%d，期望 20（上一次的 Δ 没清）\n",
                         result.lines.empty() ? -1 : result.lines.begin()->second.min_x);
            return false;
        }
    }
    // ⑦ 0x198 移动层：x / y 都跟着走（行心 170、行顶 50），且不写回。
    {
        const RunResult result = run(1, false, false, -1, 50, 30);
        if (result.lines.size() != 1) {
            std::fprintf(stderr, "text-layer-align: 0x198 后没有一条行\n");
            return false;
        }
        const LineBox& line = result.lines.begin()->second;
        if (std::abs(center(line) - 170) > 2 || std::abs(line.min_y - 50) > 2) {
            std::fprintf(stderr, "text-layer-align: 0x198 后行心=%d 行顶=%d，期望 170 / 50\n",
                         center(line), line.min_y);
            return false;
        }
        if (result.f0c != 50 || result.f10 != 30) {
            std::fprintf(stderr, "text-layer-align: 0x198 的层左/上=%d/%d，期望 50/30\n",
                         result.f0c, result.f10);
            return false;
        }
    }
    return true;
}

// ── 文本层位置：`0x198` **移动层**（GAMESTART 说明文字落点；2026-11-02 用户口径的缺陷） ──
// 判据（每条都能红）：
// ① 0x198 写的是**层记录 +0x0C/+0x10**（与 `0x70` 第 4/5 个操作数同两格）：不是"另存一对偏移"，
//    也不改层的宽 / 高（`0x70` 写的尺寸必须原样保留）；
// ② 层号 0 ⇒ **默认层**（`0x80` 写的选择子），不是"槽 1"；
// ③ 逐字绘制条目的位置 = **层左/上 + 格左上**：GAMESTART 的 `0x70 9 492 92 0 0` + `0x198 9 706 425`
//    + 原点 (10,14) ⇒ 第 1 格必须落在 (716,439)。旧实现把 0x198 当独立偏移 ⇒ 条目在 (10,14)（屏幕左上角）。
bool check_text_layer_position_198(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "text-layer-198: %s\n", what);
        return false;
    };
    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x80, {{0, 9}});                                  // 默认层 = 9
    emit_instruction(bytes, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 0}, {0, 0}});  // 层 9：200x120 @(0,0)
    emit_instruction(bytes, 0x213, {{0, 9}, {0, 190000}, {0, 40}});           // 键基数
    emit_instruction(bytes, 0x79, {{0, 9}, {0, 10}, {0, 14}});                // 文本原点 (10,14)
    emit_instruction(bytes, 0x75, {{0, 16}});
    emit_instruction(bytes, 0x74, {{0, 0}});                                  // 间隔 0 ⇒ 立即抽干
    emit_instruction(bytes, 0x198, {{0, 9}, {0, 706}, {0, 425}});             // ★ 把层 9 搬到 (706,425)
    emit_instruction(bytes, 0x71, {{0, 9}});
    emit_instruction(bytes, 0x6E, {{0, 0}, {5, 0}});
    emit_instruction(bytes, 2, {});
    core::Machine machine(key, table, nullptr, scene_step_config());
    machine.write_text(core::Operand{5, 0}, "AB");
    machine.load_root(bytes);
    for (int i = 0; i < 32 && !machine.halted(); ++i) machine.step();

    const core::ListRecord& layer = machine.list_record(9);
    if (layer.f0c != 706 || layer.f10 != 425) {
        std::fprintf(stderr, "层 9 的左/上 = (%d,%d)\n", layer.f0c, layer.f10);
        return fail("0x198 没有把层 9 的左/上写成 (706,425)");
    }
    if (layer.f14 != 200 || layer.f18 != 120) {
        std::fprintf(stderr, "层 9 的宽/高 = (%d,%d)\n", layer.f14, layer.f18);
        return fail("0x198 改了层 9 的宽/高（它只搬位置）");
    }
    const core::DrawItem* item = machine.draw_item(190000u);
    if (item == nullptr) return fail("第一格逐字绘制条目没有建出来（0x70/0x71/0x6e 链断了）");
    if (item->aux[0] != 716.0f || item->aux[1] != 439.0f) {
        std::fprintf(stderr, "第 1 格的绘制位置 = (%.1f,%.1f)（期望 716,439）\n",
                     static_cast<double>(item->aux[0]), static_cast<double>(item->aux[1]));
        return fail("逐字条目没有落在「层左/上 + 格左上」——0x198 又被当成了独立偏移");
    }

    // 层号 0 ⇒ 默认层（这里是 9）：同一份脚本把层号换成 0，效果必须相同。
    auto default_bytes = make_script_with_body();
    emit_instruction(default_bytes, 0x80, {{0, 9}});
    emit_instruction(default_bytes, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 0}, {0, 0}});
    emit_instruction(default_bytes, 0x198, {{0, 0}, {0, 40}, {0, 50}});
    emit_instruction(default_bytes, 2, {});
    core::Machine default_machine(key, table, nullptr, scene_step_config());
    default_machine.load_root(default_bytes);
    for (int i = 0; i < 8 && !default_machine.halted(); ++i) default_machine.step();
    if (default_machine.list_record(9).f0c != 40 || default_machine.list_record(9).f10 != 50) {
        return fail("0x198 的层号 0 没有落到默认层 9");
    }
    return true;
}

// 判据（每条都能红）：
// ① `0x2` 的 −11 = **恢复存档**（不是写存档）：父帧候选 ← −1、**先读版本 2 后读版本 1**、
//    版本门（版本 1 = 2/3 ⇒ 全量；版本 1 = 1 ∧ 版本 2 = 10/20 ⇒ 只装载 + 位置；其余 ⇒ 什么都不做）、
//    推进格数清 0、**不切帧号 / 不释放帧 / 不抛**
// ② `0x6` 的前置 = **没有调试器**（有 ⇒ 退出型异常、什么都不装）；深度检查是**有符号**：
//    ≥ 40 ⇒ 显示消息型异常（类别 1）、负值 ⇒ **未定义行为**（本实现诊断 + 拒绝，⛔ 不钳成 0）
// ③ `0x8c` 的目标越界 = 未定义行为 ⇒ 本实现**显式诊断 + 停止**（⛔ 不钳制 / 不回绕）
bool check_frame_flow(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "frame-flow: %s\n", what);
        return false;
    };
    TempInstall temp(11);
    temp.write("SYSTEM4.BIN", make_script());

    // ① −11 支：版本门 + 读序 + 帧号不变 + 推进格数清 0。
    {
        const auto run_restore = [&](std::int32_t version1, std::int32_t version2,
                                     int& calls, bool& full_flag, std::int64_t& versions) {
            auto script = make_script_with_body();
            emit_instruction(script, 2, {});
            core::Machine machine(key, table, &temp.install);
            machine.config_mut().set_int("set:SaveVersion1", version1);
            machine.config_mut().set_int("set:SaveVersion2", version2);
            calls = 0;
            full_flag = false;
            machine.set_save_exit_restore(
                [&calls, &full_flag](core::Machine&, std::int32_t, std::int32_t,
                                     bool full_restore) {
                    ++calls;
                    full_flag = full_restore;
                });
            machine.load_root(script);
            machine.set_parent_candidate_for_test(-11);
            machine.load_frame_for_test(0, 0);
            const std::size_t frame_before = machine.current_frame();
            if (!machine.step()) return false;
            if (machine.halted()) return false;
            if (machine.current_frame() != frame_before) return false;
            if (machine.parent_candidate() != -1) return false;
            if (machine.frame().length_slot != 0) return false;
            versions = machine.save_exit_versions();
            return true;
        };
        int calls = 0;
        bool full = false;
        std::int64_t versions = 0;
        if (!run_restore(2, 7, calls, full, versions)) return fail("−11 支（版本 1 = 2）夹具失败");
        if (calls != 1 || !full) return fail("版本 1 = 2 没有做全量恢复（装载 + 位置 + 返回栈）");
        if (versions != ((static_cast<std::int64_t>(7) << 32) | 2)) {
            std::fprintf(stderr, "读到的版本 = 0x%llX（期望 版本2=7 / 版本1=2）\n",
                         static_cast<long long>(versions));
            return fail("−11 支读的两个版本不对（或读序反了：先版本 2、后版本 1）");
        }
        if (!run_restore(3, 0, calls, full, versions) || calls != 1 || !full) {
            return fail("版本 1 = 3 没有做全量恢复");
        }
        if (!run_restore(1, 10, calls, full, versions) || calls != 1 || full) {
            return fail("版本 1 = 1 / 版本 2 = 10 没有做「只装载 + 位置」那一档");
        }
        if (!run_restore(1, 20, calls, full, versions) || calls != 1 || full) {
            return fail("版本 1 = 1 / 版本 2 = 20 没有做「只装载 + 位置」那一档");
        }
        if (!run_restore(1, 0, calls, full, versions) || calls != 0) {
            return fail("版本 1 = 1 / 版本 2 = 0 不该做任何恢复");
        }
        if (!run_restore(0, 10, calls, full, versions) || calls != 0) {
            return fail("版本 1 = 0 不该做任何恢复");
        }
    }

    // ② `0x6`：调试器前置 + 有符号深度检查。
    {
        const auto load_frame_script = [] {
            auto script = make_script_with_body();
            emit_instruction(script, 0x06, {{0, 0}, {0, 3}});
            emit_instruction(script, 2, {});
            return script;
        };
        // 有调试器 ⇒ 退出型异常、什么都不装（当前帧号不动）。
        {
            core::Machine machine(key, table, &temp.install);
            machine.set_debugger_present(true);
            machine.load_root(load_frame_script());
            if (machine.step() || !machine.halted()) return fail("有调试器时 0x6 没有抛");
            if (machine.last_stop().message != "debugger-present") {
                std::fprintf(stderr, "停因 = %s\n", machine.last_stop().message.c_str());
                return fail("有调试器时的停因不对");
            }
            if (machine.frame().length_slot != 5) {
                return fail("0x6 的长度槽不是在第 1 步就写成 5（前置调用在读操作数之前）");
            }
        }
        // 深度 ≥ 40 ⇒ 显示消息型异常、类别 1（只终止）、帧号没动。
        {
            auto script = make_script_with_body();
            emit_instruction(script, 0x06, {{0, 0}, {0, 40}});
            emit_instruction(script, 2, {});
            core::Machine machine(key, table, &temp.install);
            machine.load_root(script);
            if (machine.step() || !machine.halted()) return fail("深度 40 时 0x6 没有抛");
            if (machine.last_display_code() != "frame-depth-limit") {
                std::fprintf(stderr, "深度失败的错误码 = %s\n", machine.last_display_code().c_str());
                return fail("深度失败不是显示消息型异常（或错误码不对）");
            }
        }
        // 负帧号 ⇒ 越界写 UB：本实现诊断 + 拒绝（⛔ 不钳成 0）。
        {
            auto script = make_script_with_body();
            emit_instruction(script, 0x06, {{0, 0}, {0, static_cast<std::uint32_t>(-1)}});
            emit_instruction(script, 2, {});
            core::Machine machine(key, table, &temp.install);
            machine.load_root(script);
            if (machine.step() || !machine.halted()) return fail("负帧号时 0x6 没有拒绝");
            if (machine.last_stop().message != "frame-load-negative-target:-1") {
                std::fprintf(stderr, "负帧号的停因 = %s\n", machine.last_stop().message.c_str());
                return fail("负帧号没有走「显式诊断 + 拒绝」（被钳成 0 了？）");
            }
        }
    }

    // ③ `0x8c` 的越界目标 ⇒ 显式诊断 + 停止（本版不编语义）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x8C, {{0, 9999}});
        emit_instruction(script, 2, {});
        core::Machine machine(key, table, &temp.install);
        machine.load_root(script);
        if (machine.step() || !machine.halted()) return fail("0x8c 越界目标没有拒绝");
        if (machine.last_stop().message.rfind("jump-out-of-code", 0) != 0) {
            std::fprintf(stderr, "0x8c 越界的停因 = %s\n", machine.last_stop().message.c_str());
            return fail("0x8c 越界没有显式诊断");
        }
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
    core::Machine pump_machine = scene_step_machine(table);
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
    core::Machine cancel_machine = scene_step_machine(table);
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
// ── 鼠标回调泵的**唯一入口**（`CORE-INPUT-MOUSE-CALLBACK-PUMP`）──
// 三条判据都咬**泵本体的进入次数**（`mouse_pump_entries()`），不只咬「上次派发时刻」：
// ① **一次派发 = 泵本体进入恰一次**：派发格执行两遍、或另有第二个调用点 ⇒ 计数 > 每派发 1。
// ② **没被派发时不跑**：同一帧里一条 `0xcd` 都没有、但宿主时刻早已过登记间隔 ⇒ 入口次数 **0**、
//    「上次派发时刻」不变。★ 这一条**只能**用入口次数断：第二入口若每迭代进一次泵、又被时间闸
//    挡回去，它一格都不改 ⇒ 只看 `mouse_last_ms()` 的断言恒绿（那一格只记"真的派发了"）。
// ③ **越闸 ≠ 改闸**：第 27 位置起时派发一条（它越过间隔）→ **清掉第 27 位** → 紧接一次派发
//    （不足间隔）⇒ 必须**不**派发（入口次数 +1，但时刻 / 返回栈 / 指令指针全不动）。
// ★ 第 27 位置起时 `step()` 会把控制交回迭代头（`CORE-VM-GATE` 的位序表第 16 行）⇒ 脚本指令
//   一条都不派发；所以本用例对 `0xCD`（与第 27 位置起期间的回调体）走 `dispatch_for_test` ——
//   那正是"经分派表那一格派发"的形状，也是引擎里"门自己派发一条"的形状。
bool check_mouse_pump_single_entry(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "mouse-pump-single-entry: %s\n", what);
        return false;
    };
    const auto pump = [](core::Machine& machine) {
        machine.dispatch_for_test(core::Instruction{0, 0xCD, {}});
    };
    // ── ① + ③：登记（间隔 100、目标 12）⇒ 越闸派发一条 ⇒ 清第 27 位 ⇒ 紧接一次派发 ──
    // 布局（dword 下标）：0..4 = `0xcc` · 5..11 = 无害占位（`0x1a8`）· 12..16 = 回调体（local 1 ← 7）·
    //   17 = 返回。★ 占位是有意的：登记之后 `ip = 20`（位置 5），泵只在下面显式派发的三处进。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xCC, {{0, 100}, {0, 12}});
        for (int i = 0; i < 7; ++i) emit_instruction(bytes, 0x1A8, {});   // 5..11
        emit_instruction(bytes, 0x55, {{9, 1}, {0, 7}});                  // 12..16：回调体
        emit_instruction(bytes, 5, {});                                   // 17：返回
        core::Machine machine = scene_step_machine(table);
        machine.set_host_clock_ms(1000);
        machine.load_root(bytes, 0x1234);
        if (!machine.step()) return fail("0xcc 登记那一步停机了");
        if (machine.mouse_last_ms() != 1000u) return fail("登记时「上次派发时刻」不是当前时刻");
        if (machine.mouse_pump_entries() != 0) return fail("登记不派发，却已经进过泵本体");
        // 越闸：第 27 位置起 ⇒ 距登记只过了 30ms（< 100）也必须派发。
        machine.set_state_flags_for_test(1u << 27);
        machine.set_host_clock_ms(1030);
        pump(machine);
        if (machine.mouse_pump_entries() != 1) {
            std::fprintf(stderr, "越闸派发后泵本体进入次数 = %zu（期望恰 1）\n",
                         machine.mouse_pump_entries());
            return fail("一次派发没有等于一次泵本体进入");
        }
        if (machine.frame().ip != 48 || machine.frame().ret_stack.size() != 1 ||
            machine.frame().ret_stack.back() != 6 || machine.mouse_last_ms() != 1030u) {
            std::fprintf(stderr, "越闸派发 ip=%zu 栈=%zu last=%u（期望 48 / 1 / 6 / 1030）\n",
                         machine.frame().ip, machine.frame().ret_stack.size(), machine.mouse_last_ms());
            return fail("越闸派发的跳转 / 返回位置 / 时刻不对");
        }
        // 回调体与返回：第 27 位置起时 `step()` 会被门挡住 ⇒ 同样经表派发（这是"回调体真的跑了"）。
        machine.dispatch_for_test(core::Instruction{0, 0x55, {{9, 1}, {0, 7}}});
        if (machine.local_int(1) != 7) return fail("回调体没有执行（local 1 不是 7）");
        machine.dispatch_for_test(core::Instruction{0, 5, {}});
        if (machine.frame().ip != 24 || !machine.frame().ret_stack.empty()) {
            std::fprintf(stderr, "回调返回 ip=%zu 栈=%zu（期望 24 / 0）\n", machine.frame().ip,
                         machine.frame().ret_stack.size());
            return fail("回调的返回没有回到压入的位置");
        }
        // 清掉第 27 位（产品写点就是 `0x101`）：之后再派发一次，距上次只有 0ms（< 100）。
        machine.dispatch_for_test(core::Instruction{0, 0x101, {}});
        if ((machine.state_flags() & (1u << 27)) != 0) return fail("0x101 没有清掉第 27 位（夹具前提）");
        pump(machine);
        if (machine.mouse_pump_entries() != 2) {
            std::fprintf(stderr, "第二次派发后泵本体进入次数 = %zu（期望恰 2）\n",
                         machine.mouse_pump_entries());
            return fail("第二次派发没有进到泵本体（或进了不止一次）");
        }
        if (machine.mouse_last_ms() != 1030u) {
            std::fprintf(stderr, "不足间隔却把「上次派发时刻」写成 %u（期望仍是 1030）\n",
                         machine.mouse_last_ms());
            return fail("越闸改了时间闸（「上次派发时刻」被重置）");
        }
        if (machine.frame().ip != 24 || !machine.frame().ret_stack.empty() || machine.local_int(1) != 7) {
            std::fprintf(stderr, "不足间隔却派发了 ip=%zu 栈=%zu local1=%d\n", machine.frame().ip,
                         machine.frame().ret_stack.size(), machine.local_int(1));
            return fail("不足间隔仍派发（越闸把间隔改成 0 / 把时刻清 0）");
        }
        // 正向对照（反"泵坏了所以上面两条都绿"）：同一台机器、同一格里等号到期（100 ≥ 100）⇒ 必须派发。
        machine.set_host_clock_ms(1130);
        pump(machine);
        if (machine.frame().ip != 48 || machine.frame().ret_stack.size() != 1 ||
            machine.frame().ret_stack.back() != 7 || machine.mouse_last_ms() != 1130u ||
            machine.mouse_pump_entries() != 3) {
            std::fprintf(stderr, "到期对照 ip=%zu 栈=%zu last=%u 入口=%zu（期望 48/1/7/1130/3）\n",
                         machine.frame().ip, machine.frame().ret_stack.size(), machine.mouse_last_ms(),
                         machine.mouse_pump_entries());
            return fail("到期（含等号）却没有派发 —— 时间闸的判据坏了");
        }
    }
    // ── ② 没被派发时不跑：自环 200 拍（宿主时刻每拍 +16ms，早已过 100ms 间隔）、一条 `0xcd` 都没有 ──
    // ★ 「上次派发时刻」在"第二入口被时间闸挡回去"时也不变 ⇒ 必须同时咬**入口次数**。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xCC, {{0, 100}, {0, 0xFFFFFFFFu}});  // 间隔 100、目标 −1（取消）
        emit_instruction(bytes, 0x8C, {{0, 5}});                      // 5..7：自环到位置 5
        core::Machine quiet = scene_step_machine(table);
        quiet.reset_scene_clock(1000);  // 非外部时钟 ⇒ 每迭代 +16ms（真实地"时间在走"）
        quiet.load_root(bytes, 0x1234);
        if (!quiet.step()) return fail("自环夹具的 0xcc 那一步停机了");
        const std::uint32_t registered = quiet.mouse_last_ms();
        for (int i = 0; i < 200; ++i) {
            if (!quiet.step()) return fail("自环那一步停机了");
        }
        if (quiet.host_now_ms() - registered < 100u) {
            std::fprintf(stderr, "宿主时刻只走了 %u ms（夹具前提：必须过 100ms 间隔）\n",
                         quiet.host_now_ms() - registered);
            return fail("夹具没有让时间越过间隔");
        }
        if (quiet.mouse_pump_entries() != 0) {
            std::fprintf(stderr, "没有 0xcd 的 200 拍里泵本体被进了 %zu 次\n", quiet.mouse_pump_entries());
            return fail("存在第二入口：没被派发也在跑泵本体");
        }
        if (quiet.mouse_last_ms() != registered) {
            std::fprintf(stderr, "没有 0xcd 却把「上次派发时刻」改成 %u（登记时 %u）\n",
                         quiet.mouse_last_ms(), registered);
            return fail("没被派发却更新了「上次派发时刻」");
        }
        // 正向对照：同一台机器、同一时刻真的派发一条 ⇒ 计数与时刻都动（反"计数器恒 0"）。
        pump(quiet);
        if (quiet.mouse_pump_entries() != 1 || quiet.mouse_last_ms() != quiet.host_now_ms()) {
            std::fprintf(stderr, "对照派发：入口=%zu last=%u now=%u\n", quiet.mouse_pump_entries(),
                         quiet.mouse_last_ms(), quiet.host_now_ms());
            return fail("真的派发一条时入口计数 / 时刻没有动");
        }
        // 目标 −1 = 取消：压了返回位置又撤销（这同时确认上面那一次真的走到了时间闸之后）。
        if (!quiet.frame().ret_stack.empty() || quiet.frame().ip != 20) {
            std::fprintf(stderr, "目标 −1 的派发 ip=%zu 栈=%zu（期望 20 / 0）\n", quiet.frame().ip,
                         quiet.frame().ret_stack.size());
            return fail("目标 −1 没有撤销返回位置");
        }
    }
    return true;
}
// ── 暂停（= 区间忙）与时钟重盖（CORE-VM-LOOP · CORE-TIMING-CLOCK 的 0x23C） ──
bool check_pause_and_clock(const core::OpcodeTable& table) {
    // 暂停 = 区间忙标记 = 1 **且等待位 = 0** ⇒ `CORE-TIMING-WINDOW` 的 (d)：
    // **门前的钟照盖**（忙判定在盖钟之后），只跳**请求** ⇒ 窗不前进、两个标记不重算。
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
    core::Machine paused_machine = scene_step_machine(table);
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
    paused_machine.consume_frame_step();
    if (!paused_machine.step()) return false;
    const core::DrawItem* paused_item = paused_machine.draw_item(1);
    if (paused_item == nullptr || paused_item->color == 0xC8ABCDEFu ||
        paused_item->duration != 100) {
        std::fprintf(stderr, "paused scene advanced the window (color=0x%08X duration=%d)\n",
                     paused_item == nullptr ? 0u : paused_item->color,
                     paused_item == nullptr ? -1 : paused_item->duration);
        return false;
    }
    // ★ 暂停**不挡盖钟**（`70-vm.md:216` / `:224`：忙判定在盖钟之后，它只跳请求）。
    if (paused_machine.scene_clock_ms() != 1200u) {
        std::fprintf(stderr, "执行区间暂停时场景时钟没跟上宿主读数（clock=%u）\n",
                     paused_machine.scene_clock_ms());
        return false;
    }
    if (paused_machine.consume_frame_step()) {
        std::fprintf(stderr, "执行区间暂停时仍然发了门前渲染请求\n");
        return false;
    }
    // 等待位置起 ⇒ 暂停不挡场景推进（同一时刻、同一窗必须到期）。
    core::Machine released_machine = scene_step_machine(table);
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
    // 0x23C：无条件重盖**场景时钟对**（`CORE-TIMING-CLOCK` 的第 3 个门后写者：`0x41A2E7` 搬旧 /
    // `0x41A2ED` 写新；不受区间忙门限制）。★ 与 `0x1f4` / `0x20c` 写的是**同一对**（`70-vm.md:190`）
    // ⇒ 两格都动、次序是「先搬旧再写新」；这两条一起把"另建一对引擎时刻"的错法挡在门外。
    {
        core::Machine clock_machine = scene_step_machine(table);
        clock_machine.reset_scene_clock(500);
        clock_machine.advance_scene(600);  // 现在 = 600、上一步 = 500
        clock_machine.set_host_clock_ms(700);
        clock_machine.dispatch_for_test(core::Instruction{0, 0x23C, {}});
        if (clock_machine.scene_clock_ms() != 700u || clock_machine.scene_previous_ms() != 600u) {
            std::fprintf(stderr, "0x23c 第一次重盖 mismatch now=%u prev=%u\n",
                         clock_machine.scene_clock_ms(), clock_machine.scene_previous_ms());
            return false;
        }
        // 区间忙标记置起 ⇒ 仍然重盖（`0x23c` 是唯一**无条件**的写者）。
        clock_machine.set_driven_elsewhere(true);
        clock_machine.set_host_clock_ms(800);
        clock_machine.dispatch_for_test(core::Instruction{0, 0x23C, {}});
        if (clock_machine.scene_clock_ms() != 800u || clock_machine.scene_previous_ms() != 700u) {
            std::fprintf(stderr, "0x23c 忙时段没有重盖时钟对（now=%u prev=%u）\n",
                         clock_machine.scene_clock_ms(), clock_machine.scene_previous_ms());
            return false;
        }
    }
    return true;
}
// ── 场景时钟门槛 / 渲染步收尾判据 / 持续时间 ≤ 0（S4 + S6 的 (c)/(d) 归位） ──
/** 守的是 `CORE-TIMING-WINDOW`（`70-vm.md:206-272`）、`CORE-TIMING-CLOCK`（`:186-204`）与
 *  `60-ops-scene` 的「命令档的前置与出档」：
 *  · **只有三个"连盖钟一起跳过"**（(a) 场景绘制模式 ≠ 1 / (b) 画面暂停闩 / (c) `0x21b` 空转）
 *    ⇒ **不盖钟、不判据、不发请求、不清帧标志、不推窗**；
 *  · **五个"只挡推进、盖钟照常"**（第 24 位 / 影片缓冲被另一处持有 / 场景既不需要重绘且无新帧 /
 *    渲染器未就绪 / **(d) 执行区间的暂停**）—— (d) 的忙判定在盖钟**之后** ⇒ 钟照盖、只跳请求；
 *  · **次序固定**：整段跳过 → 盖钟 → 只挡推进；
 *  · (c) **只取消门前渲染步这一个时钟写者**：场景时钟仍由 `0x1f4` / `0x20c` / `0x23c` 推进
 *    （`70-vm.md:217` 的作用域句）⇒ 标量恒 0 不会让时间停走；
 *  · **持续时间 ≤ 0**：绘制项**整段跳过**（不清窗位/起点、不写颜色）；网格**清起点 + 清窗位 + 置「有动画」**、不重烘；
 *  · 渲染步**收尾判据 = 场景待处理标记**（不是「有动画」），随后**无条件**清「强制完成」。
 * ★ 八个 (a)…(e) 的名字与个数照 `70-vm.md:217` / `:219-225`；⛔ 别把 (d) 写回第一组（S4 的错归因）。
 */
bool check_scene_clock_gate(const core::OpcodeTable& table) {
    const auto fail = [](const char* why) {
        std::fprintf(stderr, "scene-clock-gate: %s\n", why);
        return false;
    };
    // 夹具：一条绘制项 + 一个颜色窗（delay 0 / dur 100）；当前色 0x00112233、终色 = 0xC8ABCDEF。
    // `scalar_21b` = 脚本第一步 `0x21b` 的标量（(c) 的输入）；缺省 1 ⇒ 门前渲染步照常跑。
    const auto window_script = [&](std::uint32_t scalar_21b = 1) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x21B, {{0, scalar_21b}});  // 门前渲染的 (c)：标量
        emit_instruction(bytes, 0x1F8, {{0, 1}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(bytes, 0x1FB,
                         {{0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(bytes, 0x203, {{0, 1}, {0, 0}, {0, 0}, {0, 0x00112233u}});
        emit_instruction(bytes, 0x202, {{0, 1}, {0, 0}, {0, 100}, {0, 200}, {0, 0x00ABCDEFu}});
        for (int i = 0; i < 6; ++i) emit_instruction(bytes, 0x1A8, {});  // 空转
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    // 跑到"窗已盖章（start = 1000）、还在窗内"的那一状态（5 条夹具 + 盖章那步）。
    const auto running_machine = [&]() {
        core::Machine machine = scene_step_machine(table);
        machine.set_host_clock_ms(1000);
        machine.load_root(window_script());
        for (int i = 0; i < 6; ++i) {
            if (!machine.step()) return machine;
        }
        return machine;
    };

    // ① (a) 场景绘制模式 ≠ 1 ⇒ **连盖钟一起跳过**（旧模型：只不渲染、时钟照走）。
    //    spec：`70-vm.md:217` 的 (a) / `70-vm.md:218`「它们成立期间所有以场景时钟计的量都停走」。
    {
        core::Machine machine(0x12345678u, table);  // 无配置 ⇒ set:DrawMode = 0
        machine.set_host_clock_ms(1000);
        machine.load_root(window_script());
        for (int i = 0; i < 6; ++i) {
            if (!machine.step()) return fail("DrawMode = 0 夹具停机");
        }
        if (machine.scene_clock_ms() != 0) return fail("绘制模式 ≠ 1 仍然盖了场景时钟");
        if (const core::DrawItem* item = machine.draw_item(1);
            item == nullptr || item->window_start != 0) {
            return fail("绘制模式 ≠ 1 仍然推进了窗（窗起点被盖章）");
        }
        if (machine.frame_step_requested()) return fail("绘制模式 ≠ 1 仍然发了渲染步请求");
    }
    // ② 开闸基线：绘制模式 = 1 ⇒ 时钟按宿主读数走、窗被盖章。
    {
        core::Machine machine = scene_step_machine(table);
        machine.set_host_clock_ms(1000);
        machine.load_root(window_script());
        for (int i = 0; i < 6; ++i) {
            if (!machine.step()) return fail("开闸夹具停机");
        }
        const core::DrawItem* item = machine.draw_item(1);
        if (machine.scene_clock_ms() != 1000u) return fail("绘制模式 = 1 时场景时钟没跟上宿主读数");
        if (item == nullptr || item->window_start != 1000u || item->duration != 100) {
            return fail("开闸之后窗没有被盖章 / 推进");
        }
    }
    // ③ (b) 画面暂停闩 ≠ 0 ⇒ **连盖钟一起跳过**（与区间忙标记不是同一格）。
    {
        core::Machine machine = scene_step_machine(table);
        machine.set_host_clock_ms(1000);
        machine.scene_hold_begin();  // 暂停闩的写点①（置 1；模式 = 1 才生效）
        machine.load_root(window_script());
        for (int i = 0; i < 6; ++i) {
            if (!machine.step()) return fail("画面暂停闩夹具停机");
        }
        if (machine.scene_clock_ms() != 0) return fail("画面暂停闩 ≠ 0 仍然盖了场景时钟");
        if (const core::DrawItem* item = machine.draw_item(1);
            item == nullptr || item->window_start != 0) {
            return fail("画面暂停闩 ≠ 0 仍然推进了窗");
        }
        // 闩放开 ⇒ 时钟恢复（它是**持续状态**，不是瞬时条件）。
        machine.scene_hold_end();  // 暂停闩的写点②（清 0；模式 = 1 才生效）
        machine.set_host_clock_ms(1000);
        if (!machine.step() || machine.scene_clock_ms() != 1000u) {
            return fail("画面暂停闩放开之后场景时钟没有恢复");
        }
    }
    // ④ (d) 执行区间的暂停（区间忙 ≠ 0 **且**等待位 = 0）在**"只挡推进 / 请求"**那一组 ——
    //    spec `70-vm.md:220-225` 第 5 条：「忙判定在盖钟之后 ⇒ **钟照盖、只跳请求**」。
    //    ⛔ 它不是"整段跳过"（`:217` 明说连盖钟一起跳过的**只有三个**）。
    {
        core::Machine machine = scene_step_machine(table);
        machine.set_host_clock_ms(1000);
        machine.load_root(window_script());
        for (int i = 0; i < 5; ++i) {
            if (!machine.step()) return fail("暂停夹具停机");
        }
        machine.consume_frame_step();
        machine.set_driven_elsewhere(true);  // 区间忙标记（与 0x1f4 同一格）
        machine.set_host_clock_ms(1200);
        if (!machine.step()) return fail("暂停夹具停机");
        // ② 钟照盖（忙判定在盖钟**之后** ⇒ 这一迭代的钟先盖上，然后才判忙）。
        if (machine.scene_clock_ms() != 1200u) {
            return fail("执行区间暂停时没有盖场景时钟（spec：忙判定在盖钟之后）");
        }
        // 窗不前进（请求被挡下 ⇒ 推进子步没跑）：窗起点仍是 0（这一迭代本会盖到 1200）。
        const core::DrawItem* item = machine.draw_item(1);
        if (item == nullptr || item->window_start != 0 || item->duration != 100) {
            return fail("执行区间暂停时窗前进了（暂停应只挡请求 / 推进）");
        }
        if (machine.consume_frame_step()) {
            return fail("(d) 忙 ∧ 等待位 = 0 时仍然发了门前渲染请求");
        }
        // ③ 等待位 = 1 ⇒ 暂停不再挡它 ⇒ 请求照发、窗照推（`70-vm.md:216`）。
        machine.state_flags_mut() |= (1u << 10);
        if (!machine.step()) return fail("等待位夹具停机");
        if (machine.scene_clock_ms() != 1200u) return fail("等待位 = 1 时场景时钟没跟上");
        if (!machine.consume_frame_step()) {
            return fail("(d) 忙 ∧ 等待位 = 1 时应照发门前渲染请求");
        }
        if (const core::DrawItem* advanced = machine.draw_item(1);
            advanced == nullptr || advanced->window_start == 0) {
            return fail("等待位 = 1 时窗没有前进（暂停应不再挡推进）");
        }
    }
    // ④b (c) `0x21b` 标量 = 0 **且**等待位与影片位都为 0 ⇒ 本迭代**不盖门前这一次钟、不判据、
    //     不发请求、不清帧标志**，但**迭代继续**（照常过门 / 派发）；时钟仍由 `0x20c` 推进。
    //     spec：`70-vm.md:217` 的 (c) + 同句的作用域（"只取消门前渲染步这一个时钟写者"）。
    {
        core::Machine machine = scene_step_machine(table);
        machine.set_scalar_21b(0);  // 真机根脚本跑过 `0x21b 0` 之后的样子（引擎初值也是 0）
        machine.set_host_clock_ms(1000);
        machine.load_root(window_script(0));
        machine.set_frame_flag(true);
        const std::size_t ip_before = machine.frame().ip;
        for (int i = 0; i < 5; ++i) {
            if (!machine.step()) return fail("(c) 夹具停机");
        }
        if (machine.scene_clock_ms() != 0u) return fail("(c) 时门前渲染步仍然盖了场景时钟");
        if (machine.frame().ip == ip_before) return fail("(c) 让迭代停住了（本应照常过门 / 派发）");
        if (machine.consume_frame_step()) return fail("(c) 时仍然发了门前渲染请求");
        if (!machine.frame_flag()) return fail("(c) 时清掉了帧标志（spec：清帧标志也一起跳过）");
        if (const core::DrawItem* item = machine.draw_item(1);
            item == nullptr || item->window_start != 0) {
            return fail("(c) 时窗被推进了（should 只跳过门前渲染步的判据 / 推进）");
        }
        // ★ **标量恒为 0 不会让时间停走**：`0x20c` 是另三个写者之一（`CORE-TIMING-CLOCK`）。
        machine.set_host_clock_ms(1500);
        machine.dispatch_for_test(core::Instruction{0, 0x20C, {}});
        if (machine.scene_clock_ms() != 1500u) {
            return fail("(c) 期间 `0x20c` 没能推进场景时钟（⇒ 时间停走了）");
        }
        // 等待位 = 1 ⇒ (c) **必然放行**（掩码 `2400h` 是「或」）⇒ 门前这一次照常盖钟。
        machine.state_flags_mut() |= (1u << 10);
        machine.set_host_clock_ms(1600);
        if (!machine.step() || machine.scene_clock_ms() != 1600u) {
            return fail("等待位 = 1 时 (c) 没有放行（门前那次钟没盖）");
        }
        // 影片位 = 1 同样放行。
        machine.state_flags_mut() &= ~(1u << 10);
        machine.state_flags_mut() |= (1u << 13);
        machine.set_host_clock_ms(1700);
        if (!machine.step() || machine.scene_clock_ms() != 1700u) {
            return fail("影片位 = 1 时 (c) 没有放行（门前那次钟没盖）");
        }
        // 放行之后：门前那一次**请求**发出、并在请求之后清帧标志（`0x4127E7`）。
        machine.state_flags_mut() &= ~(1u << 13);
        machine.set_scalar_21b(1);
        machine.set_frame_flag(true);
        machine.mark_dirty();
        machine.set_host_clock_ms(1800);
        if (!machine.step() || machine.frame_flag()) {
            return fail("放行后的门前请求没有清帧标志（0x4127E7）");
        }
        if (!machine.consume_frame_step()) {
            return fail("放行后门前渲染步没有发出请求");
        }
    }
    // ④c 场景时钟被 (c) 冻住时，**轮询节拍与短睡眠照走** —— 否则 TITLE 的 `0xc8` + `0xcd` 环
    //     会把输入饿死（实测：8 条资产档全红）。判据两条：
    //     · `0xcd` 的时间闸读**现场宿主时刻**（`0x41ACF1 call timeGetTime` / `0x41ACF9 sub edx,
    //       [esi+68EF0h]`；`60-ops-input.md` 的「定时派发鼠标回调」只说"读一次当前时刻"）
    //       ⇒ 场景时钟冻在 500 也能派发；
    //     · `0xc8` 的短睡眠**不得是 no-op**（`CORE-OPS-SLEEP-C8`：「必须让**时间前进 n 毫秒**…
    //       把 `sleep 1` 当作"什么都不做"会让轮询循环以 CPU 全速空转，属于违反本条」）。
    {
        auto poll_script = make_script_with_body();
        emit_instruction(poll_script, 0x0CC, {{0, 100}, {0, 0xFFFFFFFFu}});  // 间隔 100、目标 −1（取消哨兵）
        emit_instruction(poll_script, 0x0CD, {});
        emit_instruction(poll_script, 2, {});
        core::Machine poll_machine = scene_step_machine(table);
        poll_machine.set_scalar_21b(0);        // 让 (c) 把门前那一次盖钟挡掉
        // ★ 场景时钟冻在 900、宿主时刻 1000：两者**必须不同**，这条才抓得住"0xcc 记的是场景时钟"。
        //   ⛔ 别把它写成"宿主时刻更小"（旧的 1010/1000）：宿主时刻是**单调**的（`set_host_clock_ms`
        //   的单调条），往回设会被夹住 —— 那样夹具自己先坏了，而不是被测的东西。
        poll_machine.reset_scene_clock(900);
        poll_machine.set_host_clock_ms(1000);
        poll_machine.load_root(poll_script);
        if (!poll_machine.step()) return fail("0xcc 轮询夹具停机");
        if (poll_machine.scene_clock_ms() != 900u) {
            return fail("轮询夹具：场景时钟被盖了（(c) 没挡住）");
        }
        if (poll_machine.mouse_last_ms() != 1000u) {
            return fail("0xcc 登记的「当前时刻」不是宿主时刻");
        }
        // 宿主走过 100ms（≥ 间隔）、场景时钟一格没动 ⇒ 时间闸必须放行。
        // ★ 场景时钟 1010 与登记时刻 1000 只差 10ms（< 间隔 100）⇒ 拿场景时钟算 elapsed 的实现会**不派发**
        //   （回绕掩盖不了：两者都非零且相减是小数）。这正是 (c) 落地后 TITLE 轮询环会不会停摆的判据。
        poll_machine.set_host_clock_ms(1150);
        if (!poll_machine.step()) return fail("0xcd 轮询夹具停机");
        if (poll_machine.mouse_last_ms() != 1150u) {
            return fail("0xcd 的时间闸没读现场宿主时刻（场景时钟不动 ⇒ 轮询停摆）");
        }
        if (poll_machine.scene_clock_ms() != 900u) {
            return fail("0xcd 把场景时钟当成了自己的读数（它会跟着 (c) 冻住）");
        }
        // 短睡眠让时间前进 n 毫秒（虚拟阻塞）—— ⛔ 不许是 no-op。
        const std::uint32_t before_sleep = poll_machine.host_now_ms();
        poll_machine.dispatch_for_test(core::Instruction{0, 0x0C8, {{0, 4}}});
        if (poll_machine.host_now_ms() != before_sleep + 4u) {
            return fail("0xc8 的短睡眠没有让时间前进 n 毫秒（spec 明禁 no-op）");
        }
    }
    // ⑤ 只挡推进 1/5：状态字第 24 位 ⇒ 盖钟照常、窗不前进。
    {
        core::Machine machine = running_machine();
        machine.state_flags_mut() |= (1u << 24);
        machine.set_host_clock_ms(1200);  // 本应到期（start 1000 + 100）
        if (!machine.step()) return fail("第 24 位夹具停机");
        const core::DrawItem* item = machine.draw_item(1);
        if (machine.scene_clock_ms() != 1200u) return fail("第 24 位把场景时钟也挡掉了（它只挡推进）");
        if (item == nullptr || item->duration != 100 || item->color == item->target_color) {
            return fail("第 24 位置起时窗仍然前进了（应只挡推进、盖钟照常）");
        }
    }
    // ⑥ 只挡推进 2/5：影片缓冲被另一处持有（`RES-WINDOW-MOVIE-HELD` 的缺省 false + 注入口）。
    {
        core::Machine machine = running_machine();
        machine.set_movie_held(true);
        machine.set_host_clock_ms(1200);
        if (!machine.step()) return fail("影片持有夹具停机");
        const core::DrawItem* item = machine.draw_item(1);
        if (machine.scene_clock_ms() != 1200u) return fail("影片持有把场景时钟也挡掉了");
        if (item == nullptr || item->duration != 100 || item->color == item->target_color) {
            return fail("影片缓冲被另一处持有时窗仍然前进");
        }
    }
    // ⑦ 只挡推进 4/5：渲染器未就绪（缺省 true + 注入口；判据体未取证）。
    {
        core::Machine machine = running_machine();
        machine.set_render_ready(false);
        machine.set_host_clock_ms(1200);
        if (!machine.step()) return fail("渲染器未就绪夹具停机");
        const core::DrawItem* item = machine.draw_item(1);
        if (machine.scene_clock_ms() != 1200u) return fail("渲染器未就绪把场景时钟也挡掉了");
        if (item == nullptr || item->duration != 100 || item->color == item->target_color) {
            return fail("渲染器未就绪时窗仍然前进");
        }
    }
    // ⑧ 只挡推进 3/5 + 「有待消费的新帧」读一次即消费。
    //    可观察口 = 网格的「持续时间 ≤ 0」分叉：**推进那一步**会清窗位 + 起点 + 置「有动画」。
    //    spec：`70-vm.md:219-223`（只挡推进第 3 条：场景既不需要重绘、也没有影片对象报"有待消费的新帧"，
    //    后者**读一次即消费**）/ `:226`（网格的 dur ≤ 0 分叉）。
    {
        const auto mesh_script = [&]() {
            auto bytes = make_script_with_body();
            emit_instruction(bytes, 0x21B, {{0, 1}});
            emit_instruction(bytes, 0x323, {{0, 7}, {0, 0}, {0, 0}, {0, 255}, {0, 0x00ABCDEFu}});
            for (int i = 0; i < 4; ++i) emit_instruction(bytes, 0x1A8, {});
            emit_instruction(bytes, 2, {});
            return bytes;
        };
        const auto idle_machine = [&]() {
            core::Machine machine = scene_step_machine(table);
            machine.set_host_clock_ms(1000);
            machine.load_root(mesh_script());
            if (!machine.step() || !machine.step()) return machine;  // 0x21B / 0x323
            machine.end_render();  // 渲染步收尾：清脏（⇒「场景既不需要重绘」）
            return machine;
        };
        // (i) 空转：dirty = 0、无动画、有窗但 dur ≤ 0 ⇒ **只挡推进** ⇒ 网格的窗位 / 起点都留着。
        {
            core::Machine machine = idle_machine();
            const core::MeshRecord* before = machine.mesh(7);
            if (before == nullptr || (before->flags & 2u) == 0 || before->window_start != 0) {
                return fail("网格夹具：0x323 没有排上 dur = 0 的窗");
            }
            machine.set_host_clock_ms(1100);
            if (!machine.step()) return fail("空转夹具停机");
            if (machine.scene_clock_ms() != 1100u) return fail("空转把场景时钟也挡掉了");
            const core::MeshRecord* mesh = machine.mesh(7);
            if (mesh == nullptr || (mesh->flags & 2u) == 0 || mesh->window_start != 0) {
                return fail("空转时网格的 dur ≤ 0 分叉仍然跑了（应只挡推进）");
            }
            // 场景又需要重绘 ⇒ 推进；网格的 dur ≤ 0 分叉：清起点 + 清窗位 + 置「有动画」，不重烘。
            machine.mark_dirty();
            if (!machine.step()) return fail("重绘夹具停机");
            const core::MeshRecord* advanced = machine.mesh(7);
            if (advanced == nullptr || (advanced->flags & 2u) != 0 || advanced->window_start != 0) {
                return fail("网格的 dur ≤ 0 分叉没有清窗位 / 起点");
            }
            if (!machine.scene_animating()) return fail("网格的 dur ≤ 0 分叉没有置「有动画」");
            if (advanced->color == advanced->target_color) {
                return fail("网格的 dur ≤ 0 窗走了「终色 → 当前色」收尾（⛔ 那一步不该发生）");
            }
        }
        // (ii) 「有待消费的新帧」= 1 ⇒ 条件 3 不成立 ⇒ 推进（并且这一读把它消费掉）。
        {
            core::Machine machine = idle_machine();
            machine.set_movie_new_frame(true);
            machine.set_host_clock_ms(1100);
            if (!machine.step()) return fail("新帧夹具停机");
            if (machine.movie_new_frame()) return fail("「有待消费的新帧」读了一次没有被消费");
            const core::MeshRecord* mesh = machine.mesh(7);
            if (mesh == nullptr || (mesh->flags & 2u) != 0 || mesh->window_start != 0) {
                return fail("「有待消费的新帧」没有让渲染步推进（条件 3 应被它否定）");
            }
        }
    }
    // ⑨ 渲染步**收尾判据 = 场景待处理标记**（不是「有动画」/ 命令动画），随后**无条件**清「强制完成」。
    //    spec：`60-ops-scene.md` 的「命令档的前置与出档」（为 0 ⇒ 把**整张**命令表清空）+ `70-vm.md:233`。
    {
        const auto command_script = [&]() {
            auto bytes = make_script_with_body();
            emit_instruction(bytes, 0x1F8, {{0, 10}, {0, 4}, {0, 4}, {0, 0}});
            emit_instruction(bytes, 0x223,
                             {{0, 5}, {0, 10}, {0, 1}, {0, 1}, {0, 2}, {0, 1}, {0, 0}, {0, 100}});
            emit_instruction(bytes, 2, {});
            return bytes;
        };
        core::Machine machine = scene_step_machine(table);
        machine.load_root(command_script());
        if (!machine.step() || !machine.step()) return fail("命令表夹具停机");
        if (machine.scene_commands().empty()) return fail("命令表夹具：0x223 没有写进命令记录");
        // ⑨-1 待处理标记 = 1（命令不在动、场景不在动）⇒ **保留**整表。
        //     旧判据（`!command_animating_ && !scene_animating_`）会在这里清表 ⇒ 本条红。
        machine.set_command_animating(false);
        machine.set_scene_pending(true);
        machine.force_complete();
        machine.end_render();
        if (machine.scene_commands().empty()) {
            return fail("渲染步收尾把判据用成了「有动画」/命令动画（待处理标记 = 1 时命令表必须保留）");
        }
        // ⑨-2 收尾**无条件**清「强制完成」（不管本步有没有人读过它）。
        if (machine.forced_complete()) return fail("渲染步收尾没有无条件清「强制完成」");
        // ⑨-3 待处理标记 = 0 ⇒ 清**整张**表（不是删一条）。
        machine.set_scene_pending(false);
        machine.end_render();
        if (!machine.scene_commands().empty()) {
            return fail("待处理标记 = 0 时没有清空整张命令表");
        }
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
    // 门带着第 26 位（本地块内的输入等待）：它是位序表第 6 项，服务后 = **交回迭代头**
    // （`CORE-VM-STATE` 第 6 行 / `CORE-OPS-FRAME-LOCAL-RET-7C` 的输入服务分支）⇒ 输入字为 0 时是
    // **合法等待**：位不清、脚本不前进、也不停死（门关着不是停因）。
    const std::size_t waiting_ip = local_machine.frame().ip;    for (int i = 0; i < 10001 && !local_machine.halted(); ++i) local_machine.step();
    if (local_machine.halted() ||
        (local_machine.state_flags() & (1u << 26)) == 0 ||
        local_machine.frame().ip != waiting_ip || !local_machine.errors().empty()) {
        std::fprintf(stderr,
                     "0x7c bit 26 must be a legal input wait, not a stall "
                     "(halted=%d reason=%d ip=%zu flags=0x%X errors=%zu)\n",
                     local_machine.halted() ? 1 : 0,
                     static_cast<int>(local_machine.last_stop().reason),
                     local_machine.frame().ip, local_machine.state_flags(),
                     local_machine.errors().size());
        return false;
    }
    // 输入字非 0 ⇒ 服务体清第 26 位、把输入字清 0（本帧没登记副目标 ⇒ 从返回点继续）；
    // ★ 清位那一拍**交回迭代头**（本迭代不派发），下一迭代门字归 0 才派发一条
    //   （`CORE-VM-GATE` 的两种收场：服务体交回 ⇒ 不派发；扫到表尾 ⇒ 派发）。
    local_machine.set_input_word(1);
    if (!local_machine.step() || (local_machine.state_flags() & (1u << 26)) != 0 ||
        local_machine.input_word() != 0 || local_machine.frame().ip != waiting_ip) {
        std::fprintf(stderr, "0x7c bit 26 service did not clear the bit / input word\n");
        return false;
    }
    if (!local_machine.step() || local_machine.frame().ip != waiting_ip + 4) {
        std::fprintf(stderr, "0x7c 清位之后没有在下一迭代派发（ip=%zu）\n",
                     local_machine.frame().ip);
        return false;
    }
    // 0x19a / 0x1b6 / 0x1b7：C 与 Y 两个互不相同的格；Y 归一成 0/1。
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
        flag_machine.read_int(core::Operand{3, 1}) != flag_machine.mode_c()) {
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
    // 0x1c7 与 0x1cc：位 27 = 0；状态格（= 模式族 E）与状态字**不是同一格**。
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
    core::Machine clock_machine2 = scene_step_machine(table);
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
    // 0x21d：三张表各拷各的 —— 绘制项 / 网格，以及**辅助表 B**（`+0x448`）；辅助表 A（`+0x438`）**不参与**。
    // 没有源 ⇒ 上报且不标脏。
    auto scene_copy = make_script_with_body();
    emit_instruction(scene_copy, 0x1FB, {{0, 1}, {0, 1}, {0, 0}, {0, 0},
                                         {0, 4}, {0, 4}, {0, 0}, {0, 0}});
    emit_instruction(scene_copy, 0x242, {{0, 1}, {0, 7}});   // 辅助表 A：**不**该被复制
    emit_instruction(scene_copy, 0x344, {{0, 1}, {0, 9}});   // 辅助表 B：**该**被复制
    emit_instruction(scene_copy, 0x21D, {{0, 1}, {0, 2}});
    emit_instruction(scene_copy, 2, {});
    core::Machine scene_copy_machine(0x12345678u, table);
    scene_copy_machine.load_root(scene_copy);
    scene_copy_machine.end_render();  // 清脏
    for (int i = 0; i < 3; ++i) {
        if (!scene_copy_machine.step()) return false;
    }
    scene_copy_machine.end_render();
    if (!scene_copy_machine.step() || scene_copy_machine.draw_item(2) == nullptr ||
        !scene_copy_machine.scene_dirty()) {
        std::fprintf(stderr, "0x21d copy mismatch\n");
        return false;
    }
    {
        std::int32_t copy_flags = 0, copy_value = 0, aux_a = 0, aux_a_b = 0;
        if (!scene_copy_machine.scene_aux_b_entry(2, copy_flags, copy_value) || copy_value != 9 ||
            (copy_flags & 1) == 0) {
            std::fprintf(stderr, "0x21d 没有复制辅助表 B（+0x448）\n");
            return false;
        }
        if (scene_copy_machine.scene_aux_a(2, aux_a, aux_a_b)) {
            std::fprintf(stderr, "0x21d 复制了辅助表 A（+0x438）——它不参与本复制\n");
            return false;
        }
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
    const core::TextureSlotObject* decoded = read_ok_machine.texture_slot(9);
    if (decoded == nullptr || decoded->width != 2 || decoded->height != 2 ||
        decoded->rgba.size() != 16 || decoded->rgba[0] != 0 || decoded->rgba[1] != 0 ||
        decoded->rgba[2] != 255) {  // 画面第 1 行、左像素 = 蓝
        // ★ 期望值随 BMP 行跨距的修正而改：行跨距 = ⌈宽 × 位深 ÷ 32⌉ × 4（`FMT-SAVE-STH`）⇒
        //   2×2 / 24 位的行跨距是 **8**（不是 6）。旧期望"红"是按错误的 6 字节行跨距读出来的错位行。
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
    // ★ 触点缓冲由**引擎**整体重建（`push_touch_message`）；坐标是平台给的**百分之一像素**
    //   ⇒ 引擎 ÷100 之后才写出（1000 → 10、2000 → 20）。
    mouse_some.push_touch_message({core::Machine::TouchPoint{1000, 2000, 0x11, 3}});
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
// ── 摇杆轴与四条阈值（`CORE-INPUT-WORD` 的「手柄的方向位」的摇杆半段） ──
// 判据（每条都能红）：
// ① 阈值 =（轴范围 + **初始化时采到的**轴位置）÷ 2、**向零截断**；**只采一次**（第二次不再改）
// ② 轴 1（横）< 低 ⇒ 左（位 3）、> 高 ⇒ 右（位 1）；轴 2（纵）< 低 ⇒ 上（位 0）、> 高 ⇒ 下（位 2）
// ③ 比较是**严格的**（等于阈值不算命中）且按**无符号**（0xFFFFFFFF 是大值 ⇒ 右 / 下，
//    写成有符号比较会变成左 / 上）
// ④ 20 毫秒缓存：距上次查询 ≤ 20ms（含边界）复用上次那批原始轴值；21ms 起重新查询
// ⑤ 「忽略摇杆」门只挡轴、**不挡帽子**；帽子可用时方向位由帽子给（轴不参与）
bool check_input_joy_axis(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "input-joy-axis: %s\n", what);
        return false;
    };
    struct AxisState {
        std::uint32_t x = 501;
        std::uint32_t y = 501;
        int hat = -1;  // −1 = 帽子居中 / 不报
    };
    AxisState state;
    std::uint32_t clock = 1000;
    // 一台"有手柄 + 帽子居中"的机器：两轴范围 [minimum, maximum]，初始化时轴位置 (x0, y0)。
    const auto make_machine = [&](std::uint32_t x0, std::uint32_t y0, std::uint32_t minimum = 0,
                                  std::uint32_t maximum = 1000) {
        core::Machine machine(key, table);
        machine.set_joy_button_count(4);
        machine.set_joy_hat_four_way(true);
        machine.set_joy_hat_source([&state] { return state.hat; });
        machine.set_joy_axis_range(0, minimum, maximum);
        machine.set_joy_axis_range(1, minimum, maximum);
        machine.set_joy_stick_source(
            [&state] { return std::array<std::uint32_t, 2>{state.x, state.y}; });
        state.x = x0;
        state.y = y0;
        state.hat = -1;
        machine.sample_joy_axis_origin();  // 初始化那次采样（只此一次）
        return machine;
    };
    // 采样一次方向位（位 0..3）：每次把宿主时钟推过 20ms 缓存窗口 ⇒ 读到的是当时那批轴值。
    const auto bits = [&](core::Machine& machine, std::uint32_t x, std::uint32_t y) {
        state.x = x;
        state.y = y;
        clock += 100;
        machine.set_host_clock_ms(clock);
        return machine.sample_level_input() & 0xFu;
    };
    // ① 阈值算式 + ② 每条轴 / 每个方向 + ③ 严格比较 / 无符号。
    //    范围 0..1000、初始化轴位置 501 ⇒ 低 = 501/2 = 250（250.5 向零截断）、高 = 1501/2 = 750。
    {
        auto machine = make_machine(501, 501);
        if (!machine.joy_axis_thresholds_ready() || machine.joy_axis_low(0) != 250 ||
            machine.joy_axis_high(0) != 750 || machine.joy_axis_low(1) != 250 ||
            machine.joy_axis_high(1) != 750 || machine.joy_axis_origin(0) != 501 ||
            machine.joy_axis_origin(1) != 501 || machine.joy_axis_origin_samples() != 1) {
            std::fprintf(stderr,
                         "阈值：low=(%u,%u) high=(%u,%u) origin=(%u,%u) samples=%zu"
                         "（期望 250/750、250/750、501/501、1）\n",
                         machine.joy_axis_low(0), machine.joy_axis_low(1), machine.joy_axis_high(0),
                         machine.joy_axis_high(1), machine.joy_axis_origin(0),
                         machine.joy_axis_origin(1), machine.joy_axis_origin_samples());
            return fail("阈值不是（范围 + 初始化轴位置）÷ 2 向零截断 / 轴位置没有只在初始化时采一次");
        }
        struct Case {
            std::uint32_t x;
            std::uint32_t y;
            std::uint32_t expected;
            const char* why;
        };
        const Case cases[] = {
            {250, 501, 0, "轴 1 等于低阈值不算命中（严格比较）"},
            {249, 501, 1u << 3, "轴 1 低于低阈值 ⇒ 左（位 3）"},
            {750, 501, 0, "轴 1 等于高阈值不算命中（严格比较）"},
            {751, 501, 1u << 1, "轴 1 高于高阈值 ⇒ 右（位 1）"},
            {501, 250, 0, "轴 2 等于低阈值不算命中（严格比较）"},
            {501, 249, 1u << 0, "轴 2 低于低阈值 ⇒ 上（位 0）"},
            {501, 750, 0, "轴 2 等于高阈值不算命中（严格比较）"},
            {501, 751, 1u << 2, "轴 2 高于高阈值 ⇒ 下（位 2）"},
            {751, 751, (1u << 1) | (1u << 2), "两轴同时越界 ⇒ 右 + 下同时点亮（四个判据各自独立）"},
            {249, 249, (1u << 3) | (1u << 0), "两轴同时越界 ⇒ 左 + 上同时点亮"},
            {0xFFFFFFFFu, 501, 1u << 1, "轴 1 按无符号比（0xFFFFFFFF 是大值 ⇒ 右）"},
            {501, 0xFFFFFFFFu, 1u << 2, "轴 2 按无符号比（0xFFFFFFFF 是大值 ⇒ 下）"},
        };
        for (const auto& item : cases) {
            const auto got = bits(machine, item.x, item.y);
            if (got != item.expected) {
                std::fprintf(stderr, "x=%u y=%u ⇒ 0x%X（期望 0x%X）：%s\n", item.x, item.y, got,
                             item.expected, item.why);
                return fail(item.why);
            }
        }
    }
    // ①b **只采一次**：初始化之后改设备范围并"再采一次" ⇒ 四条阈值不变（轴位置不重估）。
    {
        auto machine = make_machine(501, 501);
        machine.set_joy_axis_range(0, 0, 60000);
        machine.set_joy_axis_range(1, 0, 60000);
        state.x = 20000;
        state.y = 20000;
        machine.sample_joy_axis_origin();  // 第二次：不许采
        if (machine.joy_axis_origin_samples() != 1 || machine.joy_axis_low(0) != 250 ||
            machine.joy_axis_high(0) != 750 || machine.joy_axis_origin(0) != 501) {
            std::fprintf(stderr, "第二次采样后 origin=%u low=%u high=%u samples=%zu\n",
                         machine.joy_axis_origin(0), machine.joy_axis_low(0),
                         machine.joy_axis_high(0), machine.joy_axis_origin_samples());
            return fail("初始化之后又采了一次轴位置 / 重算了阈值");
        }
        // 20000 在新范围（0..60000）里"不算大"，但阈值必须仍是旧的 ⇒ 右 + 下。
        const auto got = bits(machine, 20000, 20000);
        if (got != ((1u << 1) | (1u << 2))) {
            std::fprintf(stderr, "改范围后 x=y=20000 ⇒ 0x%X（期望 0x6：仍按旧阈值）\n", got);
            return fail("阈值跟着设备范围重估了（把「初始化采一次」实现成了「每次重估」）");
        }
    }
    // ④ 20 毫秒缓存（含边界）。
    {
        auto machine = make_machine(751, 501);  // 原点 751 ⇒ 低 375、高 875
        machine.set_host_clock_ms(0);
        state.x = 2000;  // ⇒ 右
        state.y = 501;
        if ((machine.sample_level_input() & 0xFu) != (1u << 1)) {
            return fail("缓存夹具的第一次采样不是右");
        }
        if (machine.joy_axis_queries() != 1) return fail("第一次采样没有查询平台");
        state.x = 0;  // 若重新查询 ⇒ 左
        machine.set_host_clock_ms(20);  // 含边界 ⇒ 必须复用
        if ((machine.sample_level_input() & 0xFu) != (1u << 1)) {
            return fail("距上次查询 20ms（含边界）时没有复用上次那批原始轴值");
        }
        if (machine.joy_axis_queries() != 1) return fail("20ms 内又向平台查询了一次");
        machine.set_host_clock_ms(21);  // 超过 20ms ⇒ 重新查询
        if ((machine.sample_level_input() & 0xFu) != (1u << 3)) {
            return fail("距上次查询 21ms 后没有重新查询（缓存窗口不是 20ms）");
        }
        if (machine.joy_axis_queries() != 2) return fail("21ms 后的查询次数不是 2");
    }
    // ⑤ 「忽略摇杆」门只挡轴；帽子可用时方向位由帽子给。
    {
        auto machine = make_machine(501, 501);
        machine.set_joy_ignore_stick(true);
        if (bits(machine, 751, 751) != 0) {
            return fail("「忽略摇杆」置起时摇杆轴仍给出了方向位");
        }
        state.hat = 4500;  // 45° ⇒ 位 0 + 位 1
        if (bits(machine, 751, 751) != ((1u << 0) | (1u << 1))) {
            return fail("「忽略摇杆」门把帽子也挡掉了（它只挡轴）");
        }
        state.hat = 0;  // 帽子 0° ⇒ 只有位 0（轴再越界也不参与）
        if (bits(machine, 751, 751) != (1u << 0)) {
            return fail("帽子可用且不居中时摇杆轴仍然参与（应当由帽子给方向位）");
        }
        state.hat = -1;
    }
    return true;
}
// ── 命中查找的 32 位回绕（`CORE-INPUT-HIT-TEST-12E` 的「三个端点」） ──
// 判据（每条都能红）：
// ① `start` = 最大正数 ⇒ `start + 1` 回绕成 0 ⇒ 与 `start = −1` **等价**（从下标 0 起扫）
// ② `start ≤ −2` 且偏移**不回绕** ⇒ 首项落在框表**之前**：下标 −2 的项被扫到并命中 ⇒ 落点 −2
//    （夹到 0 / 跳过越界项的实现在这里给出 0）；下标 −1 的项命中时落点就是 −1（表内下标 0 也包含光标
//    ⇒ 夹到 0 的实现会给出 0）
// ③ `count` 使末项指针**回绕**到基址 ⇒ 立刻判"无命中"（一项都不扫）—— 即使下标 0 的框包含光标
// ④ `start = −2147483646` ⇒ `(start + 1) × 16` 回绕成 **48** ⇒ 首项 = 框表下标 **3**（不是"表前"、不是 0）
// ⑤ / ⑥ 四项判据在**32 位**里加减、按**回绕后的符号位**判（上溢 / 下溢各一条：64 位扩宽求和会给出相反结论）
// 夹具：框表基址 = 局部整型池槽 8（每项 4 个整数 = 16 字节 ⇒ 槽 0..3 = 下标 −2、槽 4..7 = 下标 −1），
//       每一项（含表前那两项）都是同一个"包含光标"的框 ⇒ **命中谁 = 从哪一项开始扫**。
bool check_input_hit_wrap(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "input-hit-wrap: %s\n", what);
        return false;
    };
    constexpr std::uint32_t kBoxes = 8;    // 框表基址所在槽
    constexpr std::uint32_t kMargin = 1004;  // 补偿四元组（全 0），与所有框都不同址
    constexpr std::uint32_t kPlaneX = 200;
    constexpr std::uint32_t kPlaneY = 300;
    constexpr std::uint32_t kStart = 1000;
    constexpr std::uint32_t kX = 1001;
    constexpr std::uint32_t kY = 1002;
    constexpr std::uint32_t kCount = 1003;
    auto script = make_script_with_body();
    emit_instruction(script, 0x12E,
                     {{9, kStart}, {9, kMargin}, {9, kX}, {9, kY},
                      {9, kBoxes}, {9, kPlaneX}, {9, kPlaneY}, {9, kCount}});
    emit_instruction(script, 2, {});
    const auto hit = [&](std::int32_t start, std::int32_t count) -> std::int32_t {
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        const auto put = [&machine](std::uint32_t slot, std::int32_t value) {
            machine.write_int(core::Operand{9, slot}, value);
        };
        // 表前两项 + 表内 8 项：全都是 [左 0, 右 0x9c, 上 0, 下 0x9c]（光标 (0x40,0x40) 在每一项内）。
        for (std::uint32_t item = 0; item < 10; ++item) {  // item 0 = 下标 −2、item 1 = 下标 −1
            const std::int32_t offset = static_cast<std::int32_t>(item) - 2;
            const auto slot = static_cast<std::int32_t>(kBoxes) + offset * 4;
            put(static_cast<std::uint32_t>(slot) + 0u, 0);
            put(static_cast<std::uint32_t>(slot) + 1u, 0x9c);
            put(static_cast<std::uint32_t>(slot) + 2u, 0);
            put(static_cast<std::uint32_t>(slot) + 3u, 0x9c);
        }
        // 两个基准平面按**同一个项下标**取值（下标 −2 / −1 落在表前那两格）。
        for (std::int32_t index = -2; index < 8; ++index) {
            put(static_cast<std::uint32_t>(static_cast<std::int32_t>(kPlaneX) + index), 0);
            put(static_cast<std::uint32_t>(static_cast<std::int32_t>(kPlaneY) + index), 0);
        }
        for (std::uint32_t slot = 0; slot < 4; ++slot) put(kMargin + slot, 0);  // 补偿四元组全 0
        put(kX, 0x40);
        put(kY, 0x40);
        put(kStart, start);
        put(kCount, count);
        if (!machine.step()) {
            std::fprintf(stderr, "0x12e wrap fixture halted: %s\n",
                         machine.last_stop().message.c_str());
            return std::int32_t{-999};
        }
        return machine.read_int(core::Operand{9, kStart});
    };
    // ① `start` = 最大正数 ⇒ `start + 1` 回绕成 0 ⇒ 与 `start = −1` 等价（从下标 0 起）。
    {
        const auto from_wrapped = hit(0x7FFFFFFF, 2);
        const auto from_minus_one = hit(-1, 2);
        if (from_wrapped != 0 || from_minus_one != 0) {
            std::fprintf(stderr, "start=0x7FFFFFFF ⇒ %d、start=−1 ⇒ %d（都期望 0）\n",
                         from_wrapped, from_minus_one);
            return fail("start = 最大正数 没有等价于 start = −1（把回绕当成了越界即无命中）");
        }
    }
    // ② `start = −3` ⇒ 偏移 = −32 **不回绕** ⇒ 首项 = 表内下标 −2 ⇒ 命中落点 −2。
    {
        const auto result = hit(-3, 4);
        if (result != -2) {
            std::fprintf(stderr, "start=−3 ⇒ %d（期望 −2：首项落在框表之前的下标 −2）\n", result);
            return fail("起始下标 ≤ −2 时首项没有落到框表之前（被夹到 0 或被跳过）");
        }
    }
    // ②b `start = −2` ⇒ 首项 = 下标 −1：命中时落点是 −1（表内下标 0 也包含光标 ⇒ 夹到 0 的实现在这里给 0）。
    {
        const auto result = hit(-2, 4);
        if (result != -1) {
            std::fprintf(stderr, "start=−2 ⇒ %d（期望 −1：表前那项的落点就是 −1）\n", result);
            return fail("起始下标 −2 的首项没有落在框表之前（被夹到 0）");
        }
    }
    // ③ `count` = 2^28 ⇒ `count × 16` 回绕 2^32 ⇒ 末项指针 = 基址 ⇒ 立刻判"无命中"（一项都不扫）。
    //    ★ 下标 0 的框包含光标 ⇒ 任何"扫了至少一项"的实现都会给出 0；给出 −1 才算按 32 位回绕算末项。
    {
        const auto result = hit(-1, 0x10000000);
        if (result != -1) {
            std::fprintf(stderr, "count 回绕 ⇒ %d（期望 −1：末项指针落在基址，一项都不扫）\n", result);
            return fail("count 使末项指针回绕之后仍在扫描（末项不是按 32 位回绕算的）");
        }
    }
    // ④ 正文端点 `start = −2147483646`（= 0x80000002）⇒ `start + 1` = 0x80000003、`× 16` 的 64 位值
    //    = 0x800000030 ⇒ **低 32 位 = 0x30 = 48** ⇒ 首项 = 框表 **+48 = 下标 3**（既不是"表前"，也不是 0）。
    //    ★ 按 64 位带符号无溢出算这一步会得到 −34359738320 字节的偏移（指针落到表前/越过末项 ⇒ 一项都不扫）。
    {
        const auto result = hit(-2147483646, 4);
        if (result != 3) {
            std::fprintf(stderr, "start=−2147483646 ⇒ %d（期望 3：首项偏移回绕到 48）\n", result);
            return fail("start = −2147483646 的首项偏移没有按 32 位回绕成 48（首项落点不是下标 3）");
        }
    }
    // ⑤ / ⑥ 32 位极值：四项判据是**先按 32 位加减（溢出回绕）再取符号位**，不是扩宽求和。
    //    下面两条**各自**把"改成 64 位无溢出算术"变成红灯 —— 它们的正确结论与 64 位结论**相反**。
    //    夹具：只有下标 0 这一项参与（`start = −1`、`count = 1`）⇒ 框 = 槽 8..11、基准 = 平面表第 0 项。
    const auto hit_box = [&](std::int32_t s0, std::int32_t s1, std::int32_t s2, std::int32_t s3,
                             std::int32_t m0, std::int32_t m1, std::int32_t m2, std::int32_t m3,
                             std::int32_t x, std::int32_t y, std::int32_t plane_x,
                             std::int32_t plane_y) -> std::int32_t {
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        const auto put = [&machine](std::uint32_t slot, std::int32_t value) {
            machine.write_int(core::Operand{9, slot}, value);
        };
        put(kBoxes + 0u, s0);
        put(kBoxes + 1u, s1);
        put(kBoxes + 2u, s2);
        put(kBoxes + 3u, s3);
        put(kMargin + 0u, m0);
        put(kMargin + 1u, m1);
        put(kMargin + 2u, m2);
        put(kMargin + 3u, m3);
        put(kPlaneX, plane_x);  // 项下标 0 ⇒ 基准平面取第 0 项（偏移 0）
        put(kPlaneY, plane_y);
        put(kX, x);
        put(kY, y);
        put(kStart, -1);
        put(kCount, 1);
        if (!machine.step()) {
            std::fprintf(stderr, "0x12e extreme fixture halted: %s\n",
                         machine.last_stop().message.c_str());
            return std::int32_t{-999};
        }
        return machine.read_int(core::Operand{9, kStart});
    };
    // ⑤ 上溢：`m1 + dx` = 0x7FFFFFFF + 0x7FFFFFFF = 0xFFFFFFFE ⇒ 32 位下**符号位为 1** ⇒ 不命中。
    //    （64 位无溢出 = 4294967294 ≥ 0 ⇒ 会命中下标 0；其余三项两边都是 0。）
    {
        const auto result = hit_box(0, 0x7FFFFFFF, 0, 0, 0, 0x7FFFFFFF, 0, 0, 0x7FFFFFFF, 0, 0, 0);
        if (result != -1) {
            std::fprintf(stderr, "dx=0x7FFFFFFF、m1=0x7FFFFFFF ⇒ %d（期望 −1：和回绕后符号位为 1）\n",
                         result);
            return fail("四项判据的和没有按 32 位回绕取符号（上溢被当成正数 ⇒ 命中）");
        }
    }
    // ⑥ 下溢：`m1 + dx − s0` 的 32 位值是 **5**（非负 ⇒ 通过），而真值是 −4294967291（64 位下为负）。
    //    夹具：x = INT32_MIN、基准 = 0x7FFFFFFB ⇒ dx 的 32 位值 = 5、64 位值 = −4294967291。
    //    （32 位：merged = 5 | 0 | 0 | 0 ⇒ 命中下标 0；64 位：第一项为负 ⇒ 不命中。）
    {
        const auto result = hit_box(0x7FFFFFFF, 5, 0, 0, 0, 0x7FFFFFFF, 0, 0,
                                    static_cast<std::int32_t>(0x80000000u) /* INT32_MIN */, 0,
                                    static_cast<std::int32_t>(0x7FFFFFFBu), 0);
        if (result != 0) {
            std::fprintf(stderr, "x=INT32_MIN、基准=0x7FFFFFFB ⇒ %d（期望 0：差回绕后为 5）\n", result);
            return fail("四项判据的差没有按 32 位回绕取符号（下溢被当成负数 ⇒ 不命中）");
        }
    }
    return true;
}
// ── 触点缓冲的整体重建（`CORE-INPUT-TOUCH-POINT`） ──
// 判据（每条都能红）：
// ① 每条触摸消息**清计数 → 释放 → 按点数重分配 → 读入**：点数从 N 降到 M ⇒
//    缓冲**项数 = M**（不得把旧触点留成"追加"），计数 = M，且旧触点全部不可见
// ② 0 点消息 ⇒ 计数 0（查询只写第 1 个操作数 0，其余四个**保持原值**）
// ③ 有效性 = 标志字的两位**与**（「抬起」位 0x4 必须为 0、「主触点」位 0x10 必须为 1）；
//    全部无效 ⇒ 计数清 0
// ④ 第 4 个附加字段 = 标志字、第 5 个 = 记录 +0x14：**逐位原样**（不取低 16 位、不规范化）
// ⑤ 坐标 = 百分之一像素 ÷100（**向零截断**）；多个有效触点 ⇒ 缓冲顺序第一个
// ⑥ 不设容量上限（一次很多点也看得见最后一个）
// ⑦ 「输入可用标志」为 0 ⇒ 只写结果 0（坐标 / 标志 / 附加量一个都不写）—— 即使缓冲里有有效触点
// ★ ① 断言的是缓冲的**项数**（`touch_buffer_size()`），⛔ 不是 `std::vector::capacity()`：容量是
//   宿主分配策略，spec 没有这条合同（容量只进诊断输出）。
bool check_input_touch_buffer(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "input-touch: %s\n", what);
        return false;
    };
    using TouchPoint = core::Machine::TouchPoint;
    // 一条 0x2fc 的夹具：第 2..5 个操作数预置 77（哨兵），执行后按合同写。
    const auto make_touch_script = []() {
        auto bytes = make_script_with_body();
        for (std::uint32_t slot = 1; slot <= 5; ++slot) {
            emit_instruction(bytes, 0x55, {{9, slot}, {0, 77}});
        }
        emit_instruction(bytes, 0x2FC, {{9, 1}, {9, 2}, {9, 3}, {9, 4}, {9, 5}});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    struct Query {
        std::int32_t ok = 0;
        std::int32_t x = 0;
        std::int32_t y = 0;
        std::int32_t flags = 0;
        std::int32_t extra = 0;
    };
    // 投一条带 `points` 的消息 → 跑一次 0x2fc → 读五个操作数。
    const auto query = [&](const std::vector<TouchPoint>& points) {
        core::Machine machine(0x12345678u, table);
        machine.push_touch_message(points);
        machine.load_root(make_touch_script());
        for (int i = 0; i < 6; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "0x2fc fixture halted: %s\n",
                             machine.last_stop().message.c_str());
                return Query{};
            }
        }
        Query out;
        out.ok = machine.read_int(core::Operand{9, 1});
        out.x = machine.read_int(core::Operand{9, 2});
        out.y = machine.read_int(core::Operand{9, 3});
        out.flags = machine.read_int(core::Operand{9, 4});
        out.extra = machine.read_int(core::Operand{9, 5});
        return out;
    };
    // ① 点数从 N = 3 降到 M = 1：**同一条机器**上先后投两条消息 ⇒ 容量必须是 M（不是 3）。
    {
        core::Machine machine(0x12345678u, table);
        machine.push_touch_message({TouchPoint{1100, 2200, 0x11, 11},
                                    TouchPoint{1200, 2200, 0x11, 12},
                                    TouchPoint{1300, 2200, 0x11, 13}});
        if (machine.touch_count() != 3 || machine.touch_buffer_size() != 3 ||
            machine.touch_rebuilds() != 1) {
            std::fprintf(stderr, "N=3 ⇒ count=%d size=%d capacity=%d rebuilds=%zu\n",
                         machine.touch_count(), machine.touch_buffer_size(),
                         machine.touch_buffer_capacity(), machine.touch_rebuilds());
            return fail("一条 3 点的触摸消息没有把缓冲整体建成 3 项");
        }
        auto script = make_script_with_body();
        for (std::uint32_t slot = 1; slot <= 5; ++slot) {
            emit_instruction(script, 0x55, {{9, slot}, {0, 77}});
        }
        emit_instruction(script, 0x2FC, {{9, 1}, {9, 2}, {9, 3}, {9, 4}, {9, 5}});
        emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 0x2FC, {{9, 1}, {9, 2}, {9, 3}, {9, 4}, {9, 5}});
        emit_instruction(script, 2, {});
        machine.load_root(script);
        const auto read_five = [&machine]() {
            return Query{machine.read_int(core::Operand{9, 1}), machine.read_int(core::Operand{9, 2}),
                         machine.read_int(core::Operand{9, 3}), machine.read_int(core::Operand{9, 4}),
                         machine.read_int(core::Operand{9, 5})};
        };
        // 第一次查询跑在**3 点**的缓冲上（应当是第 1 项：x = 1100/100 = 11）。
        for (int i = 0; i < 6; ++i) {
            if (!machine.step()) return fail("两次 0x2fc 的夹具停机");
        }
        const auto after_three = read_five();
        if (after_three.ok != 1 || after_three.x != 11 || after_three.y != 22 ||
            after_three.extra != 11) {
            std::fprintf(stderr, "3 点消息 ⇒ ok=%d x=%d y=%d extra=%d（期望 1/11/22/11）\n",
                         after_three.ok, after_three.x, after_three.y, after_three.extra);
            return fail("3 点消息的第一个触点没有按缓冲顺序取到");
        }
        // 再投一条只有 1 点的消息 ⇒ **容量必须是 1**（复用旧容量会留下 3），且旧触点全部不可见。
        machine.push_touch_message({TouchPoint{9900, 8800, 0x11, 42}});
        if (machine.touch_count() != 1 || machine.touch_buffer_size() != 1 ||
            machine.touch_rebuilds() != 2) {
            std::fprintf(stderr, "M=1 ⇒ count=%d size=%d capacity=%d rebuilds=%zu（size 必须 = 1）\n",
                         machine.touch_count(), machine.touch_buffer_size(),
                         machine.touch_buffer_capacity(), machine.touch_rebuilds());
            return fail("点数从 3 降到 1 之后仍复用旧容量 / 旧触点没被释放");
        }
        if (!machine.step()) return fail("两次 0x2fc 之间的空操作停机");
        if (!machine.step()) return fail("第二次 0x2fc 停机");
        const auto after_second = read_five();
        if (after_second.ok != 1 || after_second.x != 99 || after_second.y != 88 ||
            after_second.flags != 0x11 || after_second.extra != 42) {
            std::fprintf(stderr,
                         "重建后查询 ok=%d x=%d y=%d flags=0x%X extra=%d（期望 1/99/88/0x11/42）\n",
                         after_second.ok, after_second.x, after_second.y, after_second.flags,
                         after_second.extra);
            return fail("点数降到 1 之后查询仍看到旧缓冲的触点");
        }
    }
    // ② 0 点消息 ⇒ 计数 0；查询只写第 1 个操作数 0，其余四个**保持原值**。
    {
        core::Machine machine(0x12345678u, table);
        machine.push_touch_message({TouchPoint{1100, 2200, 0x11, 11}});
        machine.push_touch_message({});
        if (machine.touch_count() != 0 || machine.touch_buffer_size() != 0) {
            std::fprintf(stderr, "0 点消息 ⇒ count=%d size=%d capacity=%d\n", machine.touch_count(),
                         machine.touch_buffer_size(), machine.touch_buffer_capacity());
            return fail("0 点消息没有把计数与缓冲项数清成 0");
        }
        machine.load_root(make_touch_script());
        for (int i = 0; i < 6; ++i) {
            if (!machine.step()) return fail("0 点消息的夹具停机");
        }
        if (machine.read_int(core::Operand{9, 1}) != 0 ||
            machine.read_int(core::Operand{9, 2}) != 77 ||
            machine.read_int(core::Operand{9, 3}) != 77 ||
            machine.read_int(core::Operand{9, 4}) != 77 ||
            machine.read_int(core::Operand{9, 5}) != 77) {
            std::fprintf(stderr, "无触点时 op2..5 = %d/%d/%d/%d（期望 77/77/77/77）\n",
                         machine.read_int(core::Operand{9, 2}), machine.read_int(core::Operand{9, 3}),
                         machine.read_int(core::Operand{9, 4}), machine.read_int(core::Operand{9, 5}));
            return fail("没有触点时把坐标 / 附加字段写掉了（应当只写第 1 个操作数 0）");
        }
    }
    // ③ 有效性两位是**与**：「抬起」位置起 ⇒ 跳过；「主触点」位未置 ⇒ 跳过；两位都对 ⇒ 命中。
    {
        const auto lifted = query({TouchPoint{1100, 2200, 0x14, 11}});  // 0x4 置起 ⇒ 无效
        if (lifted.ok != 0 || lifted.x != 77) return fail("「抬起」位置起的触点没有被跳过");
        const auto not_primary = query({TouchPoint{1100, 2200, 0x1, 11}});  // 0x10 未置 ⇒ 无效
        if (not_primary.ok != 0 || not_primary.x != 77) return fail("「主触点」位未置的触点没有被跳过");
        const auto valid = query({TouchPoint{1100, 2200, 0x11, 11}});
        if (valid.ok != 1) return fail("两位都对的触点没有被返回（判据写成了「或」？）");
        // 全部无效 ⇒ 计数清 0（下一次仍返回"没有触点"）。
        core::Machine machine(0x12345678u, table);
        machine.push_touch_message({TouchPoint{1100, 2200, 0x14, 11}});
        machine.load_root(make_touch_script());
        for (int i = 0; i < 6; ++i) {
            if (!machine.step()) return fail("全无效的夹具停机");
        }
        if (machine.touch_count() != 0) {
            std::fprintf(stderr, "全无效 ⇒ count=%d（期望 0）\n", machine.touch_count());
            return fail("缓冲里全部无效时没有把触点计数清 0");
        }
    }
    // ④ 第 4 / 5 个附加字段**原样**：标志字 0xFFFFFFFB（0x4 清、0x10 置）、附加量 0xFFFFFFFF。
    {
        const auto result = query({TouchPoint{0, 0, -5, -1}});  // -5 == 0xFFFFFFFB、-1 == 0xFFFFFFFF
        if (result.ok != 1 || result.flags != -5 || result.extra != -1) {
            std::fprintf(stderr, "附加字段 flags=%d extra=%d（期望 −5 / −1 逐位原样）\n",
                         result.flags, result.extra);
            return fail("两个附加字段没有原样传递（取低 16 位 / 规范化 / 符号处理）");
        }
    }
    // ⑤ 坐标 ÷100 向零截断；多个有效触点 ⇒ 缓冲顺序第一个。
    {
        const auto truncated = query({TouchPoint{12345, -12345, 0x11, 0}});
        if (truncated.ok != 1 || truncated.x != 123 || truncated.y != -123) {
            std::fprintf(stderr, "坐标 12345/−12345 ⇒ %d/%d（期望 123/−123）\n", truncated.x,
                         truncated.y);
            return fail("坐标不是「百分之一像素 ÷100 向零截断」");
        }
        const auto first = query({TouchPoint{100, 200, 0x11, 1}, TouchPoint{300, 400, 0x11, 2}});
        if (first.ok != 1 || first.x != 1 || first.y != 2 || first.extra != 1) {
            std::fprintf(stderr, "多有效触点 ⇒ x=%d y=%d extra=%d（期望 1/2/1：缓冲顺序第一个）\n",
                         first.x, first.y, first.extra);
            return fail("多个有效触点时没有取缓冲顺序的第一个");
        }
    }
    // ⑥ 不设容量上限：一条消息带 64 点，前面 63 个都无效 ⇒ 必须能看到最后一个。
    {
        std::vector<TouchPoint> many;
        for (std::int32_t i = 0; i < 63; ++i) many.push_back(TouchPoint{i, i, 0x14, i});  // 抬起位置起
        many.push_back(TouchPoint{6300, 6400, 0x11, 63});
        core::Machine machine(0x12345678u, table);
        machine.push_touch_message(many);
        if (machine.touch_count() != 64 || machine.touch_buffer_size() != 64) {
            std::fprintf(stderr, "64 点 ⇒ count=%d size=%d capacity=%d\n", machine.touch_count(),
                         machine.touch_buffer_size(), machine.touch_buffer_capacity());
            return fail("一条消息带很多触点时缓冲被截断（引擎不设容量上限）");
        }
        machine.load_root(make_touch_script());
        for (int i = 0; i < 6; ++i) {
            if (!machine.step()) return fail("64 点消息的夹具停机");
        }
        if (machine.read_int(core::Operand{9, 1}) != 1 ||
            machine.read_int(core::Operand{9, 2}) != 63 ||
            machine.read_int(core::Operand{9, 3}) != 64 ||
            machine.read_int(core::Operand{9, 5}) != 63) {
            std::fprintf(stderr, "64 点里最后一个 ⇒ %d/%d/%d/%d（期望 1/63/64/…/63）\n",
                         machine.read_int(core::Operand{9, 1}),
                         machine.read_int(core::Operand{9, 2}),
                         machine.read_int(core::Operand{9, 3}),
                         machine.read_int(core::Operand{9, 5}));
            return fail("最后那个有效触点看不到（被截断到固定容量）");
        }
    }
    // ⑦ 「输入可用标志」为 0 ⇒ 本条**整段不做**：即使缓冲里有有效触点，也只写结果 0，
    //    坐标 / 标志 / 附加量**一个都不写**（⛔ 不许当成"有触点且在原点"）。
    {
        core::Machine machine(0x12345678u, table);
        machine.set_input_available(false);
        machine.push_touch_message({TouchPoint{1100, 2200, 0x11, 11}});
        machine.load_root(make_touch_script());
        for (int i = 0; i < 6; ++i) {
            if (!machine.step()) return fail("输入不可用的夹具停机");
        }
        const auto ok = machine.read_int(core::Operand{9, 1});
        const auto x = machine.read_int(core::Operand{9, 2});
        const auto y = machine.read_int(core::Operand{9, 3});
        const auto flags = machine.read_int(core::Operand{9, 4});
        const auto extra = machine.read_int(core::Operand{9, 5});
        if (ok != 0 || x != 77 || y != 77 || flags != 77 || extra != 77) {
            std::fprintf(stderr, "输入不可用 ⇒ %d/%d/%d/%d/%d（期望 0/77/77/77/77）\n", ok, x, y,
                         flags, extra);
            return fail("输入可用标志为 0 时仍写出了触点（0x2fc 没有看这个标志）");
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
    const auto* t41 = machine.texture_record(41);
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
// ── 文本落点：指针族（0x192 的 tag 8 / 14；CORE-OPS-TEXT-SET） ──
bool check_text_pointer_write(const core::OpcodeTable& table) {
    // ① 正例：`0x61` 让局部指针槽 0 指向 global-string 0，`0x192` 写穿那条记录。
    //    ★ 引擎落点分派（EA 0x433310）接受 5 / 8 / 11 / 14，四条汇到同一个记录级写入
    //      （EA 0x43362F = `sub_40C120(记录, -1, 0, 源)`）⇒ tag 14 = 比 5 多一层间接。
    auto write_script = make_script_with_body();
    emit_instruction(write_script, 0x61, {{14, 0}, {5, 0}, {9, 0}});
    emit_instruction(write_script, 0x192, {{14, 0}, {5, 1}});
    emit_instruction(write_script, 2, {});
    core::Machine machine(0x12345678u, table);
    machine.write_text(core::Operand{5, 0}, "seed");
    machine.write_text(core::Operand{5, 1}, "abc");
    machine.load_root(write_script);
    machine.run(8);
    if (machine.last_stop().reason != core::StopReason::Halted ||
        machine.read_text(core::Operand{5, 0}) != "abc") {
        std::fprintf(stderr,
                     "0x192 落点 type 14 没有写穿指针目标：got=%s halted=%d msg=%s\n",
                     machine.read_text(core::Operand{5, 0}).c_str(), machine.halted() ? 1 : 0,
                     machine.last_stop().message.c_str());
        return false;
    }
    // ①b 落点 type 8（**全局**指针池）：同一套解引用，只是换一张指针表。
    auto global_ptr = make_script_with_body();
    emit_instruction(global_ptr, 0x61, {{8, 0}, {5, 0}, {9, 0}});
    emit_instruction(global_ptr, 0x192, {{8, 0}, {5, 1}});
    emit_instruction(global_ptr, 2, {});
    core::Machine global_machine(0x12345678u, table);
    global_machine.write_text(core::Operand{5, 0}, "g0");
    global_machine.write_text(core::Operand{5, 1}, "g1");
    global_machine.load_root(global_ptr);
    global_machine.run(8);
    if (global_machine.last_stop().reason != core::StopReason::Halted ||
        global_machine.read_text(core::Operand{5, 0}) != "g1") {
        std::fprintf(stderr, "0x192 落点 type 8 没有写穿指针目标：got=%s msg=%s\n",
                     global_machine.read_text(core::Operand{5, 0}).c_str(),
                     global_machine.last_stop().message.c_str());
        return false;
    }
    // ② 源侧 type 14：同样经指针读（读完写进 global-string 2）。
    auto read_script = make_script_with_body();
    emit_instruction(read_script, 0x61, {{14, 0}, {5, 0}, {9, 0}});
    emit_instruction(read_script, 0x192, {{5, 2}, {14, 0}});
    emit_instruction(read_script, 2, {});
    core::Machine reader(0x12345678u, table);
    reader.write_text(core::Operand{5, 0}, "xyz");
    reader.load_root(read_script);
    reader.run(8);
    if (reader.last_stop().reason != core::StopReason::Halted ||
        reader.read_text(core::Operand{5, 2}) != "xyz") {
        std::fprintf(stderr, "0x192 源侧 type 14 没读出指针目标：got=%s halted=%d msg=%s\n",
                     reader.read_text(core::Operand{5, 2}).c_str(), reader.halted() ? 1 : 0,
                     reader.last_stop().message.c_str());
        return false;
    }
    // ③ 反例：指针槽从没写过（地址 0）⇒ 响亮失败；⛔ 不许静默写 0 号记录。
    auto null_script = make_script_with_body();
    emit_instruction(null_script, 0x192, {{14, 1}, {5, 1}});
    emit_instruction(null_script, 2, {});
    core::Machine null_machine(0x12345678u, table);
    null_machine.write_text(core::Operand{5, 0}, "keep");
    null_machine.write_text(core::Operand{5, 1}, "abc");
    null_machine.load_root(null_script);
    null_machine.step();
    if (!null_machine.halted() ||
        null_machine.last_stop().message.rfind("write-text-unmapped-address", 0) != 0 ||
        null_machine.read_text(core::Operand{5, 0}) != "keep") {
        std::fprintf(stderr, "0x192 空指针槽没有响亮失败（msg=%s）\n",
                     null_machine.last_stop().message.c_str());
        return false;
    }
    // ④ 回归：落点仍是**值族**（type 3）⇒ 照旧响亮失败（放宽只针对 8 / 14）。
    auto int_dest = make_script_with_body();
    emit_instruction(int_dest, 0x192, {{3, 0}, {5, 1}});
    emit_instruction(int_dest, 2, {});
    core::Machine int_machine(0x12345678u, table);
    int_machine.write_text(core::Operand{5, 1}, "abc");
    int_machine.load_root(int_dest);
    int_machine.step();
    if (!int_machine.halted() ||
        int_machine.last_stop().message.rfind("write-text-unsupported-tag:3", 0) != 0) {
        std::fprintf(stderr, "0x192 落点 type 3 的失败语义被放宽了（msg=%s）\n",
                     int_machine.last_stop().message.c_str());
        return false;
    }
    // ⑤ halt 报告：帧链必须给出 script / ip / file（文件内偏移）/ opcode ——
    //    事故里只有 `opcode=0x192` 时，靠这几格定位"哪个 BIN 的哪一条"。
    const std::string report = null_machine.stop_stack_report();
    if (report.find("[stack] #1 frame=0/") == std::string::npos ||
        report.find("script=0") == std::string::npos ||
        report.find("ip=0") == std::string::npos ||
        report.find("file=") == std::string::npos ||
        report.find("opcode=0x192") == std::string::npos) {
        std::fprintf(stderr, "halt 帧链报告缺字段：\n%s", report.c_str());
        return false;
    }
    return true;
}
// ── 文本比较族（0x194 相等 / 0x195 不等；CORE-OPS-TEXT-EQUAL-194 / CORE-OPS-TEXT-UNEQUAL-195） ──
// 锚：0x42CF10（0x194 的 handler，宽度槽 7 @0x42CF4F）· 0x42D010（0x195 的 handler，宽度槽 7 @0x42D04F）·
// 0x42CF8E / 0x42D08E（两条都调同一个定长比较子例程 sub_401540）· 0x42D093（三值结果只取"非 0 ⇒ 1"）。
// ★ 这两条是**同一条比较的两面**：同一对操作数上必须互补（写错方向 ⇒ 本条红）。
bool check_text_compare_ops(const core::OpcodeTable& table) {
    const std::size_t body_start = 60;  // make_script_with_body 的脚本头长度
    // ① 值契约：相等 / 不等 / 长度不同 / 空串 四个点，且每一步的推进格数都必须是 7。
    struct Case {
        const char* left;
        const char* right;
        std::int32_t equal;    // 0x194 的落点
        std::int32_t unequal;  // 0x195 的落点
    };
    const Case cases[] = {
        {"abc", "abc", 1, 0},
        {"abc", "abd", 0, 1},
        {"abc", "abc ", 0, 1},  // 长度不同 ⇒ 不等（先比长度，再看字节）
        {"", "", 1, 0},
        {"", "a", 0, 1},
    };
    for (const Case& item : cases) {
        auto script = make_script_with_body();
        emit_instruction(script, 0x194, {{9, 0}, {5, 0}, {5, 1}});
        emit_instruction(script, 0x195, {{9, 1}, {5, 0}, {5, 1}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.write_text(core::Operand{5, 0}, item.left);
        machine.write_text(core::Operand{5, 1}, item.right);
        machine.load_root(script);
        if (!machine.step() || machine.frame().length_slot != 7 ||
            machine.local_int(0) != item.equal) {
            std::fprintf(stderr,
                         "0x194(%s,%s) 落点=%d（期望 %d）长度槽=%u（期望 7）msg=%s\n", item.left,
                         item.right, machine.local_int(0), item.equal,
                         static_cast<unsigned>(machine.frame().length_slot),
                         machine.last_stop().message.c_str());
            return false;
        }
        if (!machine.step() || machine.frame().length_slot != 7 ||
            machine.local_int(1) != item.unequal) {
            std::fprintf(stderr,
                         "0x195(%s,%s) 落点=%d（期望 %d）长度槽=%u（期望 7）msg=%s\n", item.left,
                         item.right, machine.local_int(1), item.unequal,
                         static_cast<unsigned>(machine.frame().length_slot),
                         machine.last_stop().message.c_str());
            return false;
        }
    }
    // ② 先字符串化：整数 5 与文本 "5" 相等 ⇒ 0x195 得 0（⛔ 不是"按标签比较"）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x55, {{9, 0}, {0, 5}});  // 局部整型 0 ← 5
        emit_instruction(script, 0x195, {{9, 1}, {5, 0}, {9, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.write_text(core::Operand{5, 0}, "5");
        machine.load_root(script);
        machine.step();
        if (!machine.step() || machine.local_int(1) != 0) {
            std::fprintf(stderr, "0x195(整数 5, 文本 \"5\") 得 %d（期望 0）⇒ 按标签比较了\n",
                         machine.local_int(1));
            return false;
        }
    }
    // ③ 内联文本（type 2）：与 SETFATE.BIN 的真实站点同形（op3 = 代码区里的空串）。
    {
        auto empty_literal = make_script_with_body();
        // 字面量落在**两条指令之后**：0x195 占 1 + 3*2 个字，`0x2`（exit）占 1 个字 ⇒ 字面量 = 第 8 个字。
        const std::uint32_t literal_word =
            static_cast<std::uint32_t>((empty_literal.size() - body_start) / 4) + 7 + 1;
        emit_instruction(empty_literal, 0x195, {{9, 1}, {5, 0}, {2, literal_word}});
        emit_instruction(empty_literal, 2, {});
        empty_literal.push_back(0xFFu);  // 空串 = 单个终止符（XOR 0xFF 编解码）
        while ((empty_literal.size() - body_start) % 4 != 0) empty_literal.push_back(0xFFu);
        core::Machine blank(0x12345678u, table);
        blank.load_root(empty_literal);
        blank.step();
        if (blank.local_int(1) != 0) {
            std::fprintf(stderr, "0x195(空串, 内联空串) 得 %d（期望 0）msg=%s\n", blank.local_int(1),
                         blank.last_stop().message.c_str());
            return false;
        }
        core::Machine filled(0x12345678u, table);
        filled.write_text(core::Operand{5, 0}, "x");
        filled.load_root(empty_literal);
        filled.step();
        if (filled.local_int(1) != 1) {
            std::fprintf(stderr, "0x195(\"x\", 内联空串) 得 %d（期望 1）msg=%s\n", filled.local_int(1),
                         filled.last_stop().message.c_str());
            return false;
        }
    }
    // ④ 类型闸门：落点给文本族（type 5）⇒ 抛 write-int-unsupported-tag，且落点不被改写。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x195, {{5, 2}, {5, 0}, {5, 1}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.write_text(core::Operand{5, 0}, "abc");
        machine.write_text(core::Operand{5, 1}, "abd");
        machine.write_text(core::Operand{5, 2}, "keep");
        machine.load_root(script);
        machine.step();
        if (!machine.halted() ||
            machine.last_stop().message.rfind("write-int-unsupported-tag:5", 0) != 0 ||
            machine.read_text(core::Operand{5, 2}) != "keep") {
            std::fprintf(stderr, "0x195 落点是文本标签时没有响亮失败（msg=%s got=%s）\n",
                         machine.last_stop().message.c_str(),
                         machine.read_text(core::Operand{5, 2}).c_str());
            return false;
        }
    }
    // ⑤ 写序：读数失败（空指针槽）⇒ 抛且落点不变，但推进格数**已经是 7**（读数在写格数之后）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x195, {{9, 2}, {14, 0}, {5, 1}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.write_text(core::Operand{5, 1}, "abc");
        machine.load_root(script);
        machine.step();
        if (!machine.halted() ||
            machine.last_stop().message.rfind("read-text-unmapped-address", 0) != 0 ||
            machine.local_int(2) != 0 || machine.frame().length_slot != 7) {
            std::fprintf(stderr,
                         "0x195 读数失败时的写序不对：msg=%s 落点=%d 长度槽=%u（期望 7）\n",
                         machine.last_stop().message.c_str(), machine.local_int(2),
                         static_cast<unsigned>(machine.frame().length_slot));
            return false;
        }
    }
    return true;
}
// ── 1000 项表的两项标志（0x258 / 0x259；CORE-OPS-SCENE-TABLE-FLAGS） ──
// 锚：0x425D20（0x258 handler，长度字 5 @0x425D32）· 0x41A3A0（0x259 handler，长度字 1 @0x41A3AD）
// · 0x425D66 / 0x425D6C（位 0 → 两张表的 +8）· 0x425D8F / 0x425D95（位 1 → 两张表的 +0xC）
// · 0x41A3D0..0x41A3E5（1000 项清零循环）。
// ★ 两条**都不**把场景标脏（所以也不在白名单里），掩码高位不看，两张表同值。
bool check_scene_table_flags(const core::OpcodeTable& table) {
    // ① 0x258 的位语义：mask 1 / 2 / 3 / 4（高位不看）。
    struct Case {
        std::uint32_t mask;
        std::int32_t flag0;
        std::int32_t flag1;
    };
    const Case masks[] = {{0, 0, 0}, {1, 1, 0}, {2, 0, 1}, {3, 1, 1}, {4, 0, 0}, {7, 1, 1}};
    for (const Case& item : masks) {
        auto script = make_script_with_body();
        emit_instruction(script, 0x258, {{0, 7}, {0, item.mask}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        const core::TextureFlags flags = machine.texture_flags(7);
        if (machine.frame().length_slot != 5 || flags.flag0 != item.flag0 ||
            flags.flag1 != item.flag1) {
            std::fprintf(stderr, "0x258 mask=%u 得 (%d,%d)（期望 %d/%d）长度槽=%u\n", item.mask,
                         flags.flag0, flags.flag1, item.flag0, item.flag1,
                         static_cast<unsigned>(machine.frame().length_slot));
            return false;
        }
    }
    // ② 0x259 全清两项标志、**其它字段一格不碰**、长度槽 = 1。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x258, {{0, 7}, {0, 3}});
        emit_instruction(script, 0x258, {{0, 999}, {0, 3}});
        emit_instruction(script, 0x259, {});
        emit_instruction(script, 2, {});
        auto bytes = script;
        core::Machine machine(0x12345678u, table);
        // 预置一项的"非标志"字段：0x259 之后必须原样。
        machine.write_text(core::Operand{5, 0}, "keep");  // 不参与；只为确保池里有别的状态
        machine.load_root(bytes);
        machine.step();  // 0x258 7,3
        machine.step();  // 0x258 999,3
        const core::TextureFlags before = machine.texture_flags(7);
        machine.step();  // 0x259
        const core::TextureFlags after = machine.texture_flags(7);
        const core::TextureFlags tail = machine.texture_flags(999);
        if (before.flag0 != 1 || before.flag1 != 1 || after.flag0 != 0 || after.flag1 != 0 ||
            tail.flag0 != 0 || tail.flag1 != 0 || machine.frame().length_slot != 1) {
            std::fprintf(stderr,
                         "0x259 全清不对：before=(%d,%d) after=(%d,%d) tail=(%d,%d) 长度槽=%u\n",
                         before.flag0, before.flag1, after.flag0, after.flag1, tail.flag0, tail.flag1,
                         static_cast<unsigned>(machine.frame().length_slot));
            return false;
        }
        const core::TextureRecord* record = machine.texture_record(7);
        if (record == nullptr || record->file != -1) {
            std::fprintf(stderr, "0x259 动了别的字段（file 期望 -1）\n");
            return false;
        }
    }
    // ③ 两条都**不置脏**：先把脏清掉，再各跑一条，脏必须仍是 0。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x258, {{0, 2}, {0, 3}});
        emit_instruction(script, 0x259, {});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.end_render();  // 清脏
        machine.step();
        const bool dirty_after_258 = machine.scene_dirty();
        machine.step();
        const bool dirty_after_259 = machine.scene_dirty();
        if (dirty_after_258 || dirty_after_259) {
            std::fprintf(stderr, "0x258/0x259 不该把场景标脏（got %d/%d）\n", dirty_after_258 ? 1 : 0,
                         dirty_after_259 ? 1 : 0);
            return false;
        }
    }
    // ④ 编号越界（引擎是未定义行为，本仓口径 = 响亮失败）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x258, {{0, 1000}, {0, 1}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        if (!machine.halted() ||
            machine.last_stop().message.rfind("scene-table-index-out-of-range:1000", 0) != 0) {
            std::fprintf(stderr, "0x258 越界没有响亮失败（msg=%s）\n",
                         machine.last_stop().message.c_str());
            return false;
        }
    }
    return true;
}
// ── 几个"写固定格 / 发请求"的整数档指令（0x1b1 / 0x238 / 0x32a / 0x32d） ──
// 锚：0x41FEA0（0x1b1，推进 3）· 0x4248C0（0x238，推进 3）· 0x426F80（0x32a，推进 3）
// · 0x427040（0x32d，推进 5；透明度夹 255、RGB 拼 ARGB、每次执行都发请求）。
bool check_misc_cells(const core::OpcodeTable& table) {
    // ① 0x1b1：覆盖写一个固定格。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1B1, {{0, 7}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        if (machine.frame().length_slot != 3 || machine.cell_1b1() != 7) {
            std::fprintf(stderr, "0x1b1 写格不对：len=%u cell=%d\n",
                         static_cast<unsigned>(machine.frame().length_slot), machine.cell_1b1());
            return false;
        }
    }
    // ② 0x238：第 1 格清 0、第 2 格 ← 操作数。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x238, {{0, 5500}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        if (machine.frame().length_slot != 3 || machine.cell_238_left() != 0 ||
            machine.cell_238_value() != 5500) {
            std::fprintf(stderr, "0x238 两格不对：len=%u left=%d value=%d\n",
                         static_cast<unsigned>(machine.frame().length_slot), machine.cell_238_left(),
                         machine.cell_238_value());
            return false;
        }
    }
    // ②b 0x238 写的是消息等待三格的 B / C（与 0x243 同一组）：A 不动。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x243, {});
        emit_instruction(script, 0x238, {{0, 7}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        machine.step();
        if (machine.msg_wait(0) != 1 || machine.msg_wait(1) != 0 || machine.msg_wait(2) != 7) {
            std::fprintf(stderr, "0x238 没有写消息等待三格的 B/C：(%d,%d,%d)\n",
                         machine.msg_wait(0), machine.msg_wait(1), machine.msg_wait(2));
            return false;
        }
    }
    // ③ 0x32a：只销毁指到的那一槽。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x32A, {{0, 1}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.set_scene_effect_present(0, true);
        machine.set_scene_effect_present(1, true);
        machine.load_root(script);
        machine.step();
        if (machine.frame().length_slot != 3 || machine.scene_effect_present(1) ||
            !machine.scene_effect_present(0)) {
            std::fprintf(stderr, "0x32a 销毁了错的槽：len=%u s0=%d s1=%d\n",
                         static_cast<unsigned>(machine.frame().length_slot),
                         machine.scene_effect_present(0) ? 1 : 0,
                         machine.scene_effect_present(1) ? 1 : 0);
            return false;
        }
    }
    // ④ 0x32d：透明度夹 255、RGB 拼 ARGB、每次执行都发请求。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x32D, {{0, 300}, {0, 0x123456}});
        emit_instruction(script, 0x32D, {{0, 255}, {0, 0x123456}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        if (machine.frame().length_slot != 5 ||
            machine.scene_color_request().argb != 0xFF123456u ||
            machine.scene_color_request().count != 1) {
            std::fprintf(stderr, "0x32d 请求不对：len=%u argb=%08X count=%zu\n",
                         static_cast<unsigned>(machine.frame().length_slot),
                         machine.scene_color_request().argb, machine.scene_color_request().count);
            return false;
        }
        machine.step();
        if (machine.scene_color_request().count != 2) {
            std::fprintf(stderr, "0x32d 第二次执行没有发请求（count=%zu）\n",
                         machine.scene_color_request().count);
            return false;
        }
    }
    // ⑤ 0x215：命中且位 0 = 1 ⇒ 值；未命中 / 位 0 = 0 ⇒ -1。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x215, {{9, 0}, {0, 42}});
        emit_instruction(script, 2, {});
        core::Machine hit(0x12345678u, table);
        hit.set_scene_keyed_record(42, 1, 7);
        hit.load_root(script);
        hit.step();
        if (hit.local_int(0) != 7 || hit.frame().length_slot != 5) {
            std::fprintf(stderr, "0x215 命中时不对：值=%d len=%u\n", hit.local_int(0),
                         static_cast<unsigned>(hit.frame().length_slot));
            return false;
        }
        core::Machine flagged_off(0x12345678u, table);
        flagged_off.set_scene_keyed_record(42, 0, 7);
        flagged_off.load_root(script);
        flagged_off.step();
        if (flagged_off.local_int(0) != -1) {
            std::fprintf(stderr, "0x215 位 0 = 0 时必须得 -1（得 %d）\n", flagged_off.local_int(0));
            return false;
        }
        core::Machine missing(0x12345678u, table);
        missing.load_root(script);
        missing.step();
        if (missing.local_int(0) != -1) {
            std::fprintf(stderr, "0x215 未命中时必须得 -1（得 %d）\n", missing.local_int(0));
            return false;
        }
    }
    // ⑥ 0x224：计数清 0、推进 1。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x224, {});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.set_container_224_items(3);
        machine.load_root(script);
        machine.step();
        if (machine.container_224_items() != 0 || machine.frame().length_slot != 1) {
            std::fprintf(stderr, "0x224 清空不对：items=%zu len=%u\n", machine.container_224_items(),
                         static_cast<unsigned>(machine.frame().length_slot));
            return false;
        }
    }
    // ⑦ 0x7a：读序 3 → 2 → 1，两个整数进 run 头。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x7A, {{0, 3}, {0, 140}, {0, 517}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        const auto header = machine.run_header(3);
        if (machine.frame().length_slot != 7 || header.first != 140 || header.second != 517) {
            std::fprintf(stderr, "0x7a 写 run 头不对：len=%u a=%d b=%d\n",
                         static_cast<unsigned>(machine.frame().length_slot), header.first,
                         header.second);
            return false;
        }
    }
    // ⑦b 0x7a 的槽 0 落默认槽（0x80 设成 9），不是第 0 槽。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x80, {{0, 9}});
        emit_instruction(script, 0x7A, {{0, 0}, {0, 140}, {0, 517}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        machine.step();
        const auto default_slot = machine.run_header(9);
        const auto slot_zero = machine.run_header(0);
        if (default_slot.first != 140 || default_slot.second != 517 || slot_zero.first != 0 ||
            slot_zero.second != 0) {
            std::fprintf(stderr, "0x7a 槽 0 没落默认槽：default=(%d,%d) slot0=(%d,%d)\n",
                         default_slot.first, default_slot.second, slot_zero.first, slot_zero.second);
            return false;
        }
    }
    // ⑧ 0x1cb：读内建命名键（本测试的配置库没有该键 ⇒ 0），推进 3。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1CB, {{9, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        if (machine.frame().length_slot != 3 || machine.local_int(0) != 0) {
            std::fprintf(stderr, "0x1cb 读命名键不对：len=%u 值=%d\n",
                         static_cast<unsigned>(machine.frame().length_slot), machine.local_int(0));
            return false;
        }
    }
    // ⑧b 0x1ca → 0x1cb：写与读必须是同一张命名键表（不是两个副本）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1CA, {{0, 5}});
        emit_instruction(script, 0x1CB, {{9, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        machine.step();
        if (machine.local_int(0) != 5) {
            std::fprintf(stderr, "0x1ca 写的命名键 0x1cb 读不到（%d）\n", machine.local_int(0));
            return false;
        }
    }
    // ⑨ 0x256：只改区间内**已存在**的记录，并标脏。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x256, {{0, 5}, {0, 1}, {0, 1}, {0, 2}, {0, 3}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.set_scene_keyed_record(5, 1, 0);
        machine.set_scene_keyed_record(9, 1, 0);
        machine.load_root(script);
        machine.end_render();  // 清脏
        machine.step();
        float inside[3] = {0, 0, 0};
        float outside[3] = {0, 0, 0};
        const bool has_inside = machine.scene_record_translate(5, inside);
        const bool has_outside = machine.scene_record_translate(9, outside);
        if (machine.frame().length_slot != 11 || !has_inside || inside[0] != 1.0f ||
            inside[1] != 2.0f || inside[2] != 3.0f || !has_outside || outside[0] != 0.0f ||
            !machine.scene_dirty()) {
            std::fprintf(stderr,
                         "0x256 区间/标脏不对：len=%u in=(%.1f,%.1f,%.1f) out=%.1f dirty=%d\n",
                         static_cast<unsigned>(machine.frame().length_slot), inside[0], inside[1],
                         inside[2], outside[0], machine.scene_dirty() ? 1 : 0);
            return false;
        }
    }
    return true;
}
// ── 语音：全部停止并清状态（0x1bc；CORE-OPS-VOICE-ALL-STOP） ──
// 锚：0x4197A0（handler）· 0x4197B0（推进格数 1）· 0x4197BB（后端判空）· 0x4197DA（声道 12/13/14 停止）
// · 0x4197E8（两组选项位 + 两组待播请求 + 正在播，全清 0）。
// ★ 后端没挂上 ⇒ 一个停止请求都不发，但清格照做（两条支路都要能红）。
bool check_voice_all_stop(const core::OpcodeTable& table) {
    const auto run = [&](bool attached, core::Machine& machine) {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1BC, {});
        emit_instruction(script, 2, {});
        machine.load_root(script);
        machine.set_voice_backend_attached(attached);
        machine.set_voice_busy(1);
        for (std::size_t i = 0; i < 3; ++i) machine.set_voice_cells(i, 1, 1, 1, 1);
        machine.step();
        return machine.frame().ip;
    };
    core::Machine attached(0x12345678u, table);
    const std::size_t ip_after = run(true, attached);
    if (ip_after != 4 || attached.frame().length_slot != 1 || attached.voice_stop_requests() != 3) {
        std::fprintf(stderr, "0x1bc 后端已挂上时不对：ip=%zu len=%u stops=%zu（期望 4/1/3）\n", ip_after,
                     static_cast<unsigned>(attached.frame().length_slot), attached.voice_stop_requests());
        return false;
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (attached.voice_switch(i) != 0 || attached.voice_switch_478(i) != 0 ||
            attached.voice_mirror_resource(i) != 0 || attached.voice_mirror_a4(i) != 0) {
            std::fprintf(stderr, "0x1bc 第 %zu 槽没清干净\n", i);
            return false;
        }
    }
    if (attached.voice_busy() != 0) {
        std::fprintf(stderr, "0x1bc 没清「正在播」（%d）\n", attached.voice_busy());
        return false;
    }
    core::Machine detached(0x12345678u, table);
    const std::size_t ip_detached = run(false, detached);
    if (ip_detached != 4 || detached.voice_stop_requests() != 0) {
        std::fprintf(stderr, "0x1bc 后端没挂上时不该发停止请求（stops=%zu ip=%zu）\n",
                     detached.voice_stop_requests(), ip_detached);
        return false;
    }
    if (detached.voice_busy() != 0 || detached.voice_switch(0) != 0) {
        std::fprintf(stderr, "0x1bc 后端缺不上时也必须清格（busy=%d switch0=%d）\n",
                     detached.voice_busy(), detached.voice_switch(0));
        return false;
    }
    return true;
}
// ── 选项窗口：开窗 / 清表 / 等待（0x93 / 0x94 / 0x91 / 0x92；ADV-CHOICE-WINDOW） ──
// 锚：0x4191D0（0x93，推进 1，清表 + 关窗/置闩锁）· 0x419230（0x94，推进 1，开输入 + arm + 步长 10000）
// · 0x420740（0x91，推进 3 或 0）· 0x4207D0（0x92，推进 5 或 0）· 0x403EF0（清表六格）。
// ★ 核心合同：**推进格数 = 0 是唯一的阻塞手段**（下一拍重跑同一条）—— 闩锁就是用来放行它的。
bool check_choice_window(const core::OpcodeTable& table) {
    // ① 0x94 开窗：总闸 / arm / 移动命中开关置起、步长 10000、推进 1。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x90, {{0, 0}, {0, 0}, {0, 10}, {0, 10}, {0, -1}, {0, -1}, {0, 1}});
        emit_instruction(script, 0x94, {});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        machine.step();
        const core::ChoiceWindowState& window = machine.choice_window();
        if (machine.frame().length_slot != 1 || !window.input_gate || !window.arm ||
            !window.move_hit || window.step != 10000 || window.hover != -1) {
            std::fprintf(stderr,
                         "0x94 开窗状态不对：len=%u gate=%d arm=%d move=%d step=%d hover=%d\n",
                         static_cast<unsigned>(machine.frame().length_slot), window.input_gate ? 1 : 0,
                         window.arm ? 1 : 0, window.move_hit ? 1 : 0, window.step, window.hover);
            return false;
        }
    }
    // ①b 0x90 追加成功也把 arm 闸清 0（下一次 0x94 重算悬停）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x94, {});
        emit_instruction(script, 0x90, {{0, 0}, {0, 0}, {0, 10}, {0, 10}, {0, -1}, {0, -1}, {0, 1}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        if (!machine.choice_window().arm) {
            std::fprintf(stderr, "0x94 之后 arm 闸没置起\n");
            return false;
        }
        machine.step();
        if (machine.choice_window().arm) {
            std::fprintf(stderr, "0x90 追加成功后 arm 闸没清 0（第二次开窗不会重算悬停）\n");
            return false;
        }
    }
    // ② 0x93 两支：总闸开着 ⇒ 关掉（不置闩锁）；总闸关着 ⇒ 置闩锁。两支都清表、推进 1。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x90, {{0, 0}, {0, 0}, {0, 10}, {0, 10}, {0, -1}, {0, -1}, {0, 1}});
        emit_instruction(script, 0x94, {});   // 开窗（总闸 = 1）
        emit_instruction(script, 0x93, {});   // 关窗
        emit_instruction(script, 0x93, {});   // 总闸已关 ⇒ 置闩锁
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        machine.step();
        machine.step();
        const core::ChoiceWindowState& after_close = machine.choice_window();
        if (after_close.input_gate || after_close.latch || after_close.move_hit ||
            !machine.choices().empty() || machine.frame().length_slot != 1) {
            std::fprintf(stderr, "0x93 关窗支不对：gate=%d latch=%d move=%d size=%zu len=%u\n",
                         after_close.input_gate ? 1 : 0, after_close.latch ? 1 : 0,
                         after_close.move_hit ? 1 : 0, machine.choices().size(),
                         static_cast<unsigned>(machine.frame().length_slot));
            return false;
        }
        machine.step();
        if (!machine.choice_window().latch) {
            std::fprintf(stderr, "0x93 第二支没有置闩锁\n");
            return false;
        }
    }
    // ③ 0x91：闩锁已置 ⇒ 推进 3 且**不读操作数**（给个非法类型也不许抛）；闩锁未置 ⇒ 推进 0（阻塞）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x93, {});                       // 置闩锁
        emit_instruction(script, 0x91, {{8, 3}});                 // 闩锁已置：非法类型也不该被读
        emit_instruction(script, 2, {});
        core::Machine released(0x12345678u, table);
        released.load_root(script);
        released.step();  // 0x93
        released.step();  // 0x91
        if (released.halted() || released.frame().length_slot != 3 || released.choice_window().latch) {
            std::fprintf(stderr, "0x91 闩锁支不对：halted=%d len=%u latch=%d msg=%s\n",
                         released.halted() ? 1 : 0,
                         static_cast<unsigned>(released.frame().length_slot),
                         released.choice_window().latch ? 1 : 0,
                         released.last_stop().message.c_str());
            return false;
        }
        auto blocked_script = make_script_with_body();
        emit_instruction(blocked_script, 0x91, {{0, 7}});
        emit_instruction(blocked_script, 2, {});
        core::Machine blocked(0x12345678u, table);
        blocked.load_root(blocked_script);
        blocked.step();
        if (blocked.frame().length_slot != 0 || blocked.frame().ip != 0 ||
            blocked.choice_window().step != 7 || !blocked.choice_window().move_hit) {
            std::fprintf(stderr, "0x91 未置闩锁时没有阻塞：len=%u ip=%zu step=%d move=%d\n",
                         static_cast<unsigned>(blocked.frame().length_slot), blocked.frame().ip,
                         blocked.choice_window().step, blocked.choice_window().move_hit ? 1 : 0);
            return false;
        }
        // 阻塞的判据：再走一拍，执行位置**还在同一条**（下一拍重跑）。
        blocked.step();
        if (blocked.frame().ip != 0) {
            std::fprintf(stderr, "0x91 阻塞后执行位置前进了（ip=%zu）\n", blocked.frame().ip);
            return false;
        }
    }
    // ④ 0x92：读序 2 → 1（兜底目标 + 步长）；闩锁已置时**两个操作数都不读**、兜底目标也不写。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x92, {{0, 5}, {0, 123}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        if (machine.frame().length_slot != 0 || machine.choice_window().fallback != 123 ||
            machine.choice_window().step != 5) {
            std::fprintf(stderr, "0x92 未置闩锁时不对：len=%u fallback=%d step=%d\n",
                         static_cast<unsigned>(machine.frame().length_slot),
                         machine.choice_window().fallback, machine.choice_window().step);
            return false;
        }
        auto latched_script = make_script_with_body();
        emit_instruction(latched_script, 0x93, {});       // 置闩锁
        emit_instruction(latched_script, 0x92, {{8, 1}, {8, 2}});  // 非法类型：不该被读
        emit_instruction(latched_script, 2, {});
        core::Machine latched(0x12345678u, table);
        latched.load_root(latched_script);
        latched.step();
        const std::int32_t fallback_before = latched.choice_window().fallback;
        latched.step();
        if (latched.halted() || latched.frame().length_slot != 5 || latched.choice_window().latch ||
            latched.choice_window().fallback != fallback_before) {
            std::fprintf(stderr,
                         "0x92 闩锁支不对：halted=%d len=%u latch=%d fallback=%d→%d msg=%s\n",
                         latched.halted() ? 1 : 0,
                         static_cast<unsigned>(latched.frame().length_slot),
                         latched.choice_window().latch ? 1 : 0, fallback_before,
                         latched.choice_window().fallback, latched.last_stop().message.c_str());
            return false;
        }
    }
    // ⑤ 0x93 清表**会**把兜底跳转目标复位成 -1（7 格之一：`0x403F07 [ecx+74ACh] = -1`，
    //    `[+74ACh]` = engine `0CA84h` = `0x92` 写的那一格）。★ 这条以前断言"跨窗保留"，
    //    与语料相反 —— 症状是上一个窗口的兜底目标被带到下一个窗口（没选就关窗时跳到错的下标）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x93, {});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.set_choice_fallback(123);
        machine.load_root(script);
        machine.step();
        if (machine.choice_window().fallback != -1) {
            std::fprintf(stderr, "0x93 清表没有把兜底目标复位成 -1（%d）\n",
                         machine.choice_window().fallback);
            return false;
        }
    }
    // ⑥ 0x92 的兜底目标由**清表**复位（不是"跨窗保留"）：`0x92` 阻塞时不能先写兜底再走到
    //    0x93（闩锁未置 ⇒ 0x92 每拍重跑），所以这一条用 ⑤ 的同一条可观察路径：摆一个值 ⇒ 0x93 ⇒ -1。
    //    这里只钉"清表之后**再**开窗，兜底不会从上一窗漏过来"。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x93, {});
        emit_instruction(script, 0x94, {});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.set_choice_fallback(777);
        machine.load_root(script);
        machine.step();  // 0x93：清表 ⇒ 兜底 -1
        machine.step();  // 0x94：开窗
        if (machine.choice_window().fallback != -1) {
            std::fprintf(stderr, "清表后再开窗，兜底目标从上一窗漏过来了（%d）\n",
                         machine.choice_window().fallback);
            return false;
        }
    }
    return true;
}
// ── 選択肢の登録と付加整数（0x90 / 0x97；ADV-CHOICE-ADD-90 / ADV-CHOICE-SET-EXTRA-97） ──
// 锚：0x420640（0x90 handler，长度字 15 @0x420658）· 0x420693 / 0x4206A5（右 / 下 = 起点 + 宽高）
// · 0x4206C9（读序 7 → 6 → 5）· 0x4206FE（追加）· 0x403B39（容量 100）· 0x420707（满表抛异常）
// · 0x420726（追加成功后复位选中态）· 0x420910（0x97 handler，长度字 11）· 0x403D10（按四边全等找第一条）。
bool check_adv_choice(const core::OpcodeTable& table) {
    // ① 追加一条：矩形按「起点 + 宽高」存，三个入口按 5 / 6 / 7 归位，条目不跳（顺序前进 15 字）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x90, {{0, 10}, {0, 20}, {0, 30}, {0, 40}, {0, 5}, {0, 6}, {0, 7}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        const auto& choices = machine.choices();
        if (choices.size() != 1 || machine.frame().length_slot != 15 || machine.frame().ip != 60 ||
            machine.choice_index() != -1) {
            std::fprintf(stderr, "0x90 追加后状态不对：size=%zu len=%u ip=%zu index=%d\n", choices.size(),
                         static_cast<unsigned>(machine.frame().length_slot), machine.frame().ip,
                         machine.choice_index());
            return false;
        }
        const core::ChoiceEntry& entry = choices[0];
        if (entry.left != 10 || entry.top != 20 || entry.right != 40 || entry.bottom != 60 ||
            entry.enter != 5 || entry.leave != 6 || entry.decide != 7 || entry.extra != -1) {
            std::fprintf(stderr,
                         "0x90 记录不对：rect=(%d,%d,%d,%d) enter/leave/decide=%d/%d/%d extra=%d\n",
                         entry.left, entry.top, entry.right, entry.bottom, entry.enter, entry.leave,
                         entry.decide, entry.extra);
            return false;
        }
    }
    // ② 容量 100：第 100 条成功，第 101 条抛且一条都不写。
    {
        auto script = make_script_with_body();
        for (int i = 0; i <= static_cast<int>(core::kChoiceCapacity); ++i) {
            emit_instruction(script, 0x90, {{0, i}, {0, 0}, {0, 1}, {0, 1}, {0, -1}, {0, -1}, {0, -1}});
        }
        emit_instruction(script, 2, {});
        core::Machine full(0x12345678u, table);
        full.load_root(script);
        for (std::size_t i = 0; i < core::kChoiceCapacity; ++i) {
            if (!full.step()) {
                std::fprintf(stderr, "0x90 第 %zu 条就失败了：%s\n", i + 1,
                             full.last_stop().message.c_str());
                return false;
            }
        }
        if (full.choices().size() != core::kChoiceCapacity || full.halted()) {
            std::fprintf(stderr, "0x90 容量没到 %zu（size=%zu halted=%d）\n", core::kChoiceCapacity,
                         full.choices().size(), full.halted() ? 1 : 0);
            return false;
        }
        if (full.step() || full.last_stop().message.rfind("display-message:choice-table-full", 0) != 0 ||
            full.choices().size() != core::kChoiceCapacity) {
            std::fprintf(stderr, "0x90 满表没有响亮失败（msg=%s size=%zu）\n",
                         full.last_stop().message.c_str(), full.choices().size());
            return false;
        }
    }
    // ③ 操作数类型非法：抛在读数处，且**一条都不追加**。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x90, {{0, 1}, {0, 1}, {0, 1}, {0, 1}, {8, 3}, {0, -1}, {0, -1}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        if (!machine.halted() ||
            machine.last_stop().message.rfind("read-int-unsupported-tag:8", 0) != 0 ||
            !machine.choices().empty()) {
            std::fprintf(stderr, "0x90 类型非法时不该追加（msg=%s size=%zu）\n",
                         machine.last_stop().message.c_str(), machine.choices().size());
            return false;
        }
    }
    // ④ 0x97：按四边**全等**找第一条，只改那条的附加整数；找不到 / 差一边 ⇒ 一条都不改。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x90, {{0, 0}, {0, 0}, {0, 10}, {0, 10}, {0, -1}, {0, -1}, {0, 1}});
        emit_instruction(script, 0x90, {{0, 100}, {0, 0}, {0, 10}, {0, 10}, {0, -1}, {0, -1}, {0, 2}});
        emit_instruction(script, 0x97, {{0, 100}, {0, 0}, {0, 10}, {0, 10}, {0, 9}});   // 命中第二条
        emit_instruction(script, 0x97, {{0, 0}, {0, 0}, {0, 10}, {0, 9}, {0, 8}});      // 高差 1 ⇒ 不命中
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        machine.step();
        machine.step();
        if (machine.frame().length_slot != 11) {
            std::fprintf(stderr, "0x97 推进格数不是 11（%u）\n",
                         static_cast<unsigned>(machine.frame().length_slot));
            return false;
        }
        if (machine.choices().size() != 2 || machine.choices()[0].extra != -1 ||
            machine.choices()[1].extra != 9) {
            std::fprintf(stderr, "0x97 没有按矩形命中第二条（extra=%d/%d）\n",
                         machine.choices()[0].extra, machine.choices()[1].extra);
            return false;
        }
        if (!machine.step()) {
            std::fprintf(stderr, "0x97 未命中时不该失败：%s\n", machine.last_stop().message.c_str());
            return false;
        }
        if (machine.choices()[0].extra != -1 || machine.choices()[1].extra != 9) {
            std::fprintf(stderr, "0x97 差一条边的矩形竟然改到了字段（%d/%d）\n",
                         machine.choices()[0].extra, machine.choices()[1].extra);
            return false;
        }
    }
    return true;
}
// ── 场景记录交换 / 搬运（0x214；CORE-OPS-SCENE3D-214） ──
// 锚：0x423AE0（handler；0x423AFB push 2 先读、0x423B05 push 1 后读）· 0x4ABEF0（三条出口都写 B5AC←1；
// 两边都有记录时 0x4ABF5C/80/9D 三次整块拷贝 = 交换）。★ 判据：对称交换、单边搬移、无条件标脏。
bool check_scene_relocate(const core::OpcodeTable& table) {
    const auto build = [&](std::uint32_t key_a, std::uint32_t key_b, bool make_a, bool make_b) {
        auto bytes = make_script_with_body();
        const auto emit_item = [&](std::uint32_t key, std::uint32_t texture) {
            emit_instruction(bytes, 0x1FB,
                            {{0, key}, {0, texture}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        };
        if (make_a) emit_item(key_a, 100);
        if (make_b) emit_item(key_b, 200);
        emit_instruction(bytes, 0x214, {{0, key_a}, {0, key_b}});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    const auto run = [&](const core::Bytes& script, std::size_t steps, core::Machine& machine) {
        machine.load_root(script);
        for (std::size_t i = 0; i < steps; ++i) {
            if (!machine.step()) return false;
        }
        return true;
    };
    // ① 两个键都有记录 ⇒ 交换两半（SceneRecord 的 tag/flags 与 DrawItem 的 texture_id）。
    {
        core::Machine machine(0x12345678u, table);
        if (!run(build(5, 9, true, true), 3, machine)) {
            std::fprintf(stderr, "0x214 交换用例走到一半停机：%s\n", machine.last_stop().message.c_str());
            return false;
        }
        std::int32_t flags5 = 0, value5 = 0, flags9 = 0, value9 = 0;
        const bool has5 = machine.scene_keyed_record(5, flags5, value5);
        const bool has9 = machine.scene_keyed_record(9, flags9, value9);
        const auto item5 = machine.draw_items().find(5);
        const auto item9 = machine.draw_items().find(9);
        if (!has5 || !has9 || value5 != 200 || value9 != 100 ||
            item5 == machine.draw_items().end() || item9 == machine.draw_items().end() ||
            item5->second.texture_id != 200 || item9->second.texture_id != 100) {
            std::fprintf(stderr,
                         "0x214 没有交换整条记录：tag=%d/%d tex=%u/%u\n", value5, value9,
                         item5 == machine.draw_items().end() ? 0u : item5->second.texture_id,
                         item9 == machine.draw_items().end() ? 0u : item9->second.texture_id);
            return false;
        }
        if (!machine.scene_dirty()) {
            std::fprintf(stderr, "0x214 交换后没有标脏\n");
            return false;
        }
    }
    // ② 只有 B 有记录 ⇒ B 搬到 A、B 消失。
    {
        core::Machine machine(0x12345678u, table);
        if (!run(build(5, 9, false, true), 2, machine)) {
            std::fprintf(stderr, "0x214 单边搬运停机：%s\n", machine.last_stop().message.c_str());
            return false;
        }
        std::int32_t flags = 0, value = 0;
        if (!machine.scene_keyed_record(5, flags, value) || value != 200 ||
            machine.scene_keyed_record(9, flags, value) ||
            machine.draw_items().find(5) == machine.draw_items().end() ||
            machine.draw_items().find(9) != machine.draw_items().end()) {
            std::fprintf(stderr, "0x214 没有把 B 的整条记录搬到 A（tag=%d）\n", value);
            return false;
        }
    }
    // ③ 只有 A 有记录 ⇒ A 搬到 B、A 消失。
    {
        core::Machine machine(0x12345678u, table);
        if (!run(build(5, 9, true, false), 2, machine)) {
            std::fprintf(stderr, "0x214 反方向搬运停机：%s\n", machine.last_stop().message.c_str());
            return false;
        }
        std::int32_t flags = 0, value = 0;
        if (!machine.scene_keyed_record(9, flags, value) || value != 100 ||
            machine.scene_keyed_record(5, flags, value) ||
            machine.draw_items().find(9) == machine.draw_items().end() ||
            machine.draw_items().find(5) != machine.draw_items().end()) {
            std::fprintf(stderr, "0x214 没有把 A 的整条记录搬到 B（tag=%d）\n", value);
            return false;
        }
    }
    // ④ 两个都没有 ⇒ 不建记录，但仍标脏（三条出口都写 B5AC←1）。
    {
        core::Machine machine(0x12345678u, table);
        if (!run(build(5, 9, false, false), 1, machine)) {
            std::fprintf(stderr, "0x214 空表停机：%s\n", machine.last_stop().message.c_str());
            return false;
        }
        std::int32_t flags = 0, value = 0;
        if (machine.scene_keyed_record(5, flags, value) ||
            machine.scene_keyed_record(9, flags, value) || !machine.scene_dirty()) {
            std::fprintf(stderr, "0x214 两个都没有时不该建记录 / 必须标脏（dirty=%d）\n",
                         machine.scene_dirty() ? 1 : 0);
            return false;
        }
    }
    return true;
}
// ── 零散但必须有的行为守卫（0x193 / 0x2e9 / 0x2f7 / 0x32 / 0x10a / 0x20a） ──
// 这些 opcode 在复审里被点名"零 CTest"；每条只断言最容易回归的一两个点（全部有 spec 合同）。
bool check_misc_ops(const core::OpcodeTable& table) {
    // ① 0x2e9：固定格覆盖写（原样，含 -1）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x2E9, {{0, 7}});
        emit_instruction(script, 0x2E9, {{0, 0xFFFFFFFFu}});  // -1
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        if (machine.frame().length_slot != 3 || machine.cell_2e9() != 7) {
            std::fprintf(stderr, "0x2e9 写固定格不对：len=%u v=%d\n",
                         static_cast<unsigned>(machine.frame().length_slot), machine.cell_2e9());
            return false;
        }
        machine.step();
        if (machine.cell_2e9() != -1) {
            std::fprintf(stderr, "0x2e9 写 -1 没有原样存（%d）\n", machine.cell_2e9());
            return false;
        }
    }
    // ② 0x2f7：整格写 1（不是 or）；另两槽不动。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x2F7, {{0, 1}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.set_voice_option_2f7(0, 0x10005);
        machine.set_voice_option_2f7(1, 0x10005);
        machine.set_voice_option_2f7(2, 0x10005);
        machine.load_root(script);
        machine.step();
        if (machine.frame().length_slot != 3 || machine.voice_option_2f7(1) != 1 ||
            machine.voice_option_2f7(0) != 0x10005 || machine.voice_option_2f7(2) != 0x10005) {
            std::fprintf(stderr, "0x2f7 没整格写 1：%d/%d/%d\n", machine.voice_option_2f7(0),
                         machine.voice_option_2f7(1), machine.voice_option_2f7(2));
            return false;
        }
    }
    // ③ 0x193：左 ++ 右（读序 3→2，结果次序仍是左先）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x193, {{5, 0}, {5, 1}, {5, 2}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.write_text(core::Operand{5, 0}, "A");
        machine.write_text(core::Operand{5, 1}, "B");
        machine.write_text(core::Operand{5, 2}, "C");
        machine.load_root(script);
        machine.step();
        if (machine.frame().length_slot != 7 || machine.read_text(core::Operand{5, 0}) != "BC") {
            std::fprintf(stderr, "0x193 拼接不对：len=%u out=%s\n",
                         static_cast<unsigned>(machine.frame().length_slot),
                         machine.read_text(core::Operand{5, 0}).c_str());
            return false;
        }
    }
    // ④ 0x32：4×4 白块从源纹理拷到目标纹理（源 / 目标 id 方向反了 ⇒ 目标仍透明）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1F8, {{0, 1}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(script, 0x20B, {{0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 255}, {0, 0x00FFFFFFu}});
        emit_instruction(script, 0x1F8, {{0, 2}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(script, 0x32, {{0, 1}, {0, 2}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}, {0, 4}, {0, 4}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        for (int i = 0; i < 4; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "0x32 前置停机：%s\n", machine.last_stop().message.c_str());
                return false;
            }
        }
        const core::TextureSlotObject* target = machine.texture_slot(2);
        if (target == nullptr || target->rgba.size() < 4 || target->rgba[0] != 255 ||
            target->rgba[3] != 255) {
            std::fprintf(stderr, "0x32 目标纹理没有被源白块覆盖（rev=%d）\n",
                         target == nullptr ? -1 : static_cast<int>(target->revision));
            return false;
        }
    }
    // ⑤ 0x10a：记录换算前的虚拟屏坐标（请求未丢弃）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x10A, {{0, 100}, {0, 200}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        if (machine.frame().length_slot != 5 || machine.cursor_requests() != 1 ||
            machine.cursor_request_x() != 100 || machine.cursor_request_y() != 200) {
            std::fprintf(stderr, "0x10a 请求不对：len=%u n=%zu (%d,%d)\n",
                         static_cast<unsigned>(machine.frame().length_slot), machine.cursor_requests(),
                         machine.cursor_request_x(), machine.cursor_request_y());
            return false;
        }
    }
    // ⑥ 0x20a：位 30 未置 ⇒ 不消费待画格；仍把场景标脏（让下一次渲染步重画）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x80, {{0, 9}});
        emit_instruction(script, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 5}, {0, 5}});
        emit_instruction(script, 0x213, {{0, 9}, {0, 190700}, {0, 40}});
        emit_instruction(script, 0x75, {{0, 20}});
        emit_instruction(script, 0x74, {{0, 10000}});
        emit_instruction(script, 0x71, {{0, 9}});
        emit_instruction(script, 0x6E, {{0, 0}, {5, 0}});
        emit_instruction(script, 0x20A, {{0, 9}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.write_text(core::Operand{5, 0}, "AB");
        machine.load_root(script);
        for (int i = 0; i < 7; ++i) {
            if (!machine.step()) return false;
        }
        if (machine.pending_glyph_count() != 2) {
            std::fprintf(stderr, "0x20a 前置待画格不是 2（%zu）\n", machine.pending_glyph_count());
            return false;
        }
        machine.end_render();
        // ★ 这一步**必须走直派接缝**：普通打字期本迭代零派发（`70-vm.md`「门」条），而门忙时能
        //   派发的另一支是演示闸层 —— 那一支的字符格在闸里（`text_layer_cell_count`），**不在**
        //   待画队列 ⇒「打字中 + 位 30 未置 + 待画格非 0」这三个前置在脚本流里够不着。
        //   本条守的是 handler 自己的两半（位 30 未置 ⇒ 不消费待画格、仍标脏），不守可达性。
        machine.dispatch_for_test(core::Instruction{0, 0x20A, {{0, 9}}});
        if (machine.pending_glyph_count() != 2 || !machine.scene_dirty()) {
            std::fprintf(stderr, "0x20a 位 30=0 时不该消费待画格 / 必须标脏（pending=%zu dirty=%d）\n",
                         machine.pending_glyph_count(), machine.scene_dirty() ? 1 : 0);
            return false;
        }
    }
    return true;
}
// ── 场景记录的两半（0x1fb / 0x219 / 0x220 / 0x242；CORE-OPS-SCENE3D-*） ──
// 锚：0x4ACE50（0x1fb 写 +4 与 +0x24..+0x2C）· 0x4ACEE0（0x219 写 +0x24..+0x2C）· 0x4ACF20（0x217 写 vec1）
// · 0x4ABEF0（0x214 搬整条）· 0x4AD9A0（0x242 写 +408 的 +2D0 与 +438 的 +1F8）。
// ★ 判据：同一组引擎格在模型的 SceneRecord 与 DrawItem 两半上必须同步，否则消费者看不到。
bool check_scene_records(const core::OpcodeTable& table) {
    const auto step_n = [](core::Machine& machine, std::size_t count, const char* what) {
        for (std::size_t i = 0; i < count; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "scene-records(%s)：第 %zu 步停机：%s\n", what, i + 1,
                             machine.last_stop().message.c_str());
                return false;
            }
        }
        return true;
    };
    // ① 0x1fb 写 +0x24..+0x2C（op7 / op8 / 0）⇒ 0x21a 必须读回 7 / 8 / 0。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1FB, {{0, 5}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 7}, {0, 8}});
        emit_instruction(script, 0x21A, {{0, 5}, {4, 0}, {4, 1}, {4, 2}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        if (!step_n(machine, 2, "1fb->21a")) return false;
        if (machine.read_float(core::Operand{4, 0}) != 7.0 ||
            machine.read_float(core::Operand{4, 1}) != 8.0 ||
            machine.read_float(core::Operand{4, 2}) != 0.0) {
            std::fprintf(stderr, "0x1fb 写的 vec2 0x21a 读不到：%f/%f/%f\n",
                         machine.read_float(core::Operand{4, 0}),
                         machine.read_float(core::Operand{4, 1}),
                         machine.read_float(core::Operand{4, 2}));
            return false;
        }
    }
    // ② 0x219 写 vec2 与 DrawItem.aux（渲染读 aux）：0x21a 与 aux 都必须是 1 / 2 / 3。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1FB, {{0, 5}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x219, {{0, 5}, {1, 0x3F800000u}, {1, 0x40000000u}, {1, 0x40400000u}});
        emit_instruction(script, 0x21A, {{0, 5}, {4, 0}, {4, 1}, {4, 2}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        if (!step_n(machine, 3, "219->21a")) return false;
        if (machine.read_float(core::Operand{4, 0}) != 1.0 ||
            machine.read_float(core::Operand{4, 1}) != 2.0 ||
            machine.read_float(core::Operand{4, 2}) != 3.0) {
            std::fprintf(stderr, "0x219 的 vec2 没写进 0x21a：%f/%f/%f\n",
                         machine.read_float(core::Operand{4, 0}),
                         machine.read_float(core::Operand{4, 1}),
                         machine.read_float(core::Operand{4, 2}));
            return false;
        }
        const core::DrawItem* item = machine.draw_item(5);
        if (item == nullptr || item->aux[0] != 1.0f || item->aux[1] != 2.0f || item->aux[2] != 3.0f) {
            std::fprintf(stderr, "0x219 没同步 DrawItem.aux（渲染读它）\n");
            return false;
        }
    }
    // ③ 0x220 的窗位 / G0 / G3 必须同步到 DrawItem（消费者读 DrawItem，不读 SceneRecord）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1FB, {{0, 5}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x220, {{0, 5}, {0, 11}, {0, 22}, {0, 1}, {0, 2}, {0, 3}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        if (!step_n(machine, 2, "220")) return false;
        const core::SceneRecord* record = machine.scene_record(5);
        const core::DrawItem* item = machine.draw_item(5);
        if (record == nullptr || (record->flags & 2) == 0 || record->g[0] != 0 || record->g[1] != 11 ||
            record->g[2] != 22 || record->g[3] != 1) {
            std::fprintf(stderr, "0x220 的 SceneRecord 字段不对\n");
            return false;
        }
        if (item == nullptr || (item->flags & 2u) == 0 || item->config_g3 != 1 ||
            item->window_start != 0 || item->set21e_int0 != 11 || item->set21e_int1 != 22) {
            std::fprintf(stderr, "0x220 的窗位/配置格没同步到 DrawItem（窗不会生效）\n");
            return false;
        }
    }
    // ④ 0x242 两处写（+0x2D0 与第二张表的一格）：
    //    ① **第一张表缺该编号 ⇒ 建默认记录 ⇒ 场景必须脏**（`sub_4AAA50` 建记录分支 → `0x4AAA8D–97
    //      call sub_49A300`，`sub_49A300` 在 `0x49A620` 写 `[T_Scene+0B5ACh]=1`）；
    //    ② **记录已存在 ⇒ 写值不标脏**（两次写值本身都不置脏）；
    //    ③ **只有第二张表缺记录 ⇒ 不标脏**（`0x4AD9D7` 的第二张表路径不写 `0xB5AC`）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x242, {{0, 5}, {0, 7}});
        emit_instruction(script, 0x242, {{0, 5}, {0, 9}});
        emit_instruction(script, 0x242, {{0, 6}, {0, 11}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.end_render();
        if (!step_n(machine, 1, "242-create")) return false;
        std::int32_t pair_a = 0, pair_b = 0;
        const bool has_pair = machine.scene_aux_a(5, pair_a, pair_b);
        if (machine.scene_record_2d0(5) != 7 || !has_pair || pair_a != 7 || pair_b != 0 ||
            !machine.scene_dirty()) {
            std::fprintf(stderr,
                         "0x242 为第一张表建默认记录必须标脏：2d0=%d pair=%d/%d dirty=%d\n",
                         machine.scene_record_2d0(5), pair_a, pair_b,
                         machine.scene_dirty() ? 1 : 0);
            return false;
        }
        machine.end_render();
        if (!step_n(machine, 1, "242-existing")) return false;
        if (machine.scene_record_2d0(5) != 9 || !machine.scene_aux_a(5, pair_a, pair_b) ||
            pair_a != 9 || machine.scene_dirty()) {
            std::fprintf(stderr, "0x242 已存在记录时写值不该标脏（2d0=%d pair=%d dirty=%d）\n",
                         machine.scene_record_2d0(5), pair_a, machine.scene_dirty() ? 1 : 0);
            return false;
        }
        // ③ 预置第一张表的 6 号记录（场景记录表有、第二张表没有）⇒ 本条只为第二张表建记录。
        machine.set_scene_keyed_record(6, 0, 0);
        machine.end_render();
        if (!step_n(machine, 1, "242-pair-create")) return false;
        if (!machine.scene_aux_a(6, pair_a, pair_b) || pair_a != 11 ||
            machine.scene_record_2d0(6) != 11 || machine.scene_dirty()) {
            std::fprintf(stderr,
                         "0x242 只为第二张表建记录时不该标脏（2d0=%d pair=%d dirty=%d）\n",
                         machine.scene_record_2d0(6), pair_a, machine.scene_dirty() ? 1 : 0);
            return false;
        }
    }
    return true;
}
// ── 消息块（0x304 / 0x305；CORE-OPS-MSG-BLOCK-304-305） ──
// 锚：0x41B1E1（门 and 10001h）· 0x41EB7D / 0x41FCD8（0x6e / 0x196 置 10000h）· 0x41B212（回卷到 128h）
// · 0x41B300（自旋 sub_45BE20）· 0x41B32C/332/338（等待三格 B=0,C=0,A=1）· 0x41B348（未武装只清臂位）。
bool check_msg_block(const core::OpcodeTable& table) {
    // append_kind：0 = 不追加，1 = 0x6e，2 = 0x196，3 = 0x6e 空串。
    const auto build = [&](bool block, int append_kind, bool end, bool redraw) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x80, {{0, 9}});
        emit_instruction(bytes, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x213, {{0, 9}, {0, 190700}, {0, 40}});
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 0x74, {{0, 10000}});  // 打字间隔极大 ⇒ 后续帧不会偷揭示
        emit_instruction(bytes, 0x71, {{0, 9}});
        if (block) emit_instruction(bytes, 0x304, {});
        if (append_kind == 1) emit_instruction(bytes, 0x6E, {{0, 0}, {5, 0}});
        if (append_kind == 2) emit_instruction(bytes, 0x196, {{0, 0}, {5, 0}, {5, 1}});
        if (append_kind == 3) emit_instruction(bytes, 0x6E, {{0, 0}, {5, 2}});
        if (end) emit_instruction(bytes, 0x305, {});
        if (redraw) emit_instruction(bytes, 0x20A, {{0, 9}});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    const auto prepare = [&](core::Machine& machine) {
        machine.write_text(core::Operand{5, 0}, "AB");
        machine.write_text(core::Operand{5, 1}, "x");
        machine.write_text(core::Operand{5, 2}, "");
    };
    const auto steps = [&](core::Machine& machine, std::size_t count, const char* what) {
        for (std::size_t i = 0; i < count; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "msg-block(%s)：走到第 %zu 步停机：%s\n", what, i + 1,
                             machine.last_stop().message.c_str());
                return false;
            }
        }
        return true;
    };
    constexpr std::size_t kBase = 6;  // 0x80 / 0x70 / 0x213 / 0x75 / 0x74 / 0x71
    // ① 0x304 → 0x6e：bit16 必须置起；还没揭示任何格。
    {
        core::Machine machine(0x12345678u, table, nullptr, scene_step_config());  // ★ 场景绘制模式 1（纹理画布路径）
        prepare(machine);
        machine.load_root(build(true, 1, false, false));
        if (!steps(machine, kBase + 2, "6e")) return false;
        if ((machine.msg_arm() & 0x10000) == 0) {
            std::fprintf(stderr, "0x6e 成功追加后没有置臂位第 16 位（arm=0x%X）\n", machine.msg_arm());
            return false;
        }
        if (machine.text_layer_cell_count(9) != 2 || machine.text_layer_revealed(9) != 0) {
            std::fprintf(stderr, "0x6e 追加后格数/揭示不对：cells=%zu revealed=%zu\n",
                         machine.text_layer_cell_count(9), machine.text_layer_revealed(9));
            return false;
        }
    }
    // ② 武装后 0x305：回卷到块开头 → 全部揭示 → 等待三格 (1,0,0) → 清臂位。
    {
        core::Machine machine(0x12345678u, table, nullptr, scene_step_config());  // ★ 场景绘制模式 1（纹理画布路径）
        prepare(machine);
        machine.load_root(build(true, 1, true, false));
        if (!steps(machine, kBase + 2, "305-append")) return false;
        // ★ 普通打字（无 `0x300` 演示闸）⇒ 打字位命中时**本迭代零派发**（`70-vm.md`「门」条：
        //   「普通打字（没有开演示闸，无槽在用）时标志 = 0 ⇒ 打字期间本迭代零派发，要等服务体把
        //   第 29 位清掉（推进器自己走完，或输入把它提前抽干）才恢复派发」）⇒ 下一条 `0x305`
        //   要等第 29 位被清。这里用引擎自己的那条路（平台输入到达 ⇒ 同帧抽干并清位，`0x40953c`）。
        machine.set_input_pressed(true);
        if (!steps(machine, 1, "305-drain")) return false;
        machine.set_input_pressed(false);
        if (machine.text_typing()) return false;  // 夹具：抽干之后第 29 位必须已清
        if (!steps(machine, 1, "305")) return false;
        if (machine.msg_arm() != 0 || machine.msg_wait(0) != 1 || machine.msg_wait(1) != 0 ||
            machine.msg_wait(2) != 0) {
            std::fprintf(stderr, "0x305 武装后状态不对：arm=0x%X wait=(%d,%d,%d)\n", machine.msg_arm(),
                         machine.msg_wait(0), machine.msg_wait(1), machine.msg_wait(2));
            return false;
        }
        if (machine.text_layer_cell_count(9) == 0 ||
            machine.text_layer_revealed(9) != machine.text_layer_cell_count(9)) {
            std::fprintf(stderr, "0x305 武装后没有揭示全部：cells=%zu revealed=%zu\n",
                         machine.text_layer_cell_count(9), machine.text_layer_revealed(9));
            return false;
        }
    }
    // ③ 0x196 也置 bit16（两条追加指令同一分支）。
    {
        core::Machine machine(0x12345678u, table, nullptr, scene_step_config());  // ★ 场景绘制模式 1（纹理画布路径）
        prepare(machine);
        machine.load_root(build(true, 2, false, false));
        if (!steps(machine, kBase + 2, "196")) return false;
        if ((machine.msg_arm() & 0x10000) == 0) {
            std::fprintf(stderr, "0x196 成功追加后没有置臂位第 16 位（arm=0x%X）\n", machine.msg_arm());
            return false;
        }
    }
    // ④ 未武装（没有追加）：只清臂位，等待三格不动。
    {
        core::Machine machine(0x12345678u, table, nullptr, scene_step_config());  // ★ 场景绘制模式 1（纹理画布路径）
        machine.load_root(build(true, 0, true, false));
        if (!steps(machine, kBase + 2, "unarmed")) return false;
        if (machine.msg_arm() != 0 || machine.msg_wait(0) != 0 || machine.msg_wait(1) != 0 ||
            machine.msg_wait(2) != 0) {
            std::fprintf(stderr, "0x305 未武装时不该写等待格：arm=0x%X wait=(%d,%d,%d)\n",
                         machine.msg_arm(), machine.msg_wait(0), machine.msg_wait(1),
                         machine.msg_wait(2));
            return false;
        }
    }
    // ⑤ 空串 0x6e 不置 bit16（早退在写之前）⇒ 0x305 仍是未武装。
    {
        core::Machine machine(0x12345678u, table);
        prepare(machine);
        machine.load_root(build(true, 3, true, false));
        if (!steps(machine, kBase + 3, "empty")) return false;
        if (machine.msg_arm() != 0 || machine.msg_wait(0) != 0) {
            std::fprintf(stderr, "空串 0x6e 竟然武装了消息块：arm=0x%X wait0=%d\n", machine.msg_arm(),
                         machine.msg_wait(0));
            return false;
        }
    }
    // ⑥ 0x213 写基数 0 后，0x70 不得用宿主兜底覆盖它；条目键从 0 起（0 是合法基数）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x80, {{0, 9}});
        emit_instruction(script, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 5}, {0, 5}});
        emit_instruction(script, 0x213, {{0, 9}, {0, 0}, {0, 40}});
        emit_instruction(script, 0x75, {{0, 20}});
        emit_instruction(script, 0x74, {{0, 10000}});
        emit_instruction(script, 0x71, {{0, 9}});
        emit_instruction(script, 0x6E, {{0, 0}, {5, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table, nullptr, scene_step_config());  // ★ 场景绘制模式 1
        prepare(machine);
        machine.load_root(script);
        if (!steps(machine, kBase + 1, "base-zero")) return false;
        if (machine.text_layer_draw_key(9) != 0 || machine.draw_item(0) == nullptr) {
            std::fprintf(stderr,
                         "0x213 写基数 0 被宿主兜底覆盖 / 条目键不是 0（draw_key=%d item0=%d）\n",
                         machine.text_layer_draw_key(9), machine.draw_item(0) ? 1 : 0);
            return false;
        }
    }
    // ⑦ 0x72 的槽号越界必须响亮失败（模型不许越界索引文本层数组）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x72, {{0, 99}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.step();
        if (!machine.halted() ||
            machine.last_stop().message.rfind("text-layer-slot-out-of-range:99", 0) != 0) {
            std::fprintf(stderr, "0x72 槽号 99 没有响亮失败（msg=%s）\n",
                         machine.last_stop().message.c_str());
            return false;
        }
    }
    return true;
}
// 文本层的**键基数兜底**：本作 ADV 的消息层**不调** `0x213`（残差 `ADV-TEXT-LAYER-DRAW-KEY`），
// 所以 `0x70` 必须补一个宿主基数，且绘制 / 擦除都认它。★ 判据是"真的出现了绘制条目"，
// ⛔ 不是"兜底格被写了" —— 只看 `key_base_set`（= 脚本写过）会让消息层一个字都不显示，
// 表现为"点一下什么都不变"（本轮实际踩过）。
bool check_text_layer_fallback(const core::OpcodeTable& table) {
    constexpr std::int32_t kGroup = 9;
    const auto base = core::kTextLayerFallbackBase + kGroup * 1000;
    const auto build = [&](bool with_213) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x80, {{0, kGroup}});
        emit_instruction(bytes, 0x70, {{0, kGroup}, {0, 200}, {0, 120}, {0, 5}, {0, 5}});
        if (with_213) emit_instruction(bytes, 0x213, {{0, kGroup}, {0, 190700}, {0, 40}});
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 0x74, {{0, 0}});  // 间隔 0 ⇒ 立即抽干，格子一次全部排好
        emit_instruction(bytes, 0x71, {{0, kGroup}});
        emit_instruction(bytes, 0x6E, {{0, 0}, {5, 0}});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    const auto prepare = [&](core::Machine& machine) {
        machine.write_text(core::Operand{5, 0}, "AB");
    };
    const auto run = [&](const std::vector<std::uint8_t>& script) {
        // ★ 纹理画布路径 = **场景绘制模式 = 1**（初值来自配置 `set:DrawMode`）⇒ 夹具显式置 1。
        core::Machine machine(0x12345678u, table, nullptr, scene_step_config());
        prepare(machine);
        machine.load_root(script);
        for (int i = 0; i < 64 && !machine.halted(); ++i) machine.step();
        return machine;
    };
    // ① 没有 0x213：兜底基数上必须有条目（否则消息层只剩窗框）。
    {
        auto machine = run(build(false));
        if (machine.text_layer_draw_key(kGroup) != base) {
            std::fprintf(stderr, "0x70 没补兜底基数（draw_key=%d 期望 %d）\n",
                         machine.text_layer_draw_key(kGroup), base);
            return false;
        }
        if (machine.draw_item(static_cast<std::uint32_t>(base)) == nullptr) {
            std::fprintf(stderr, "没有 0x213 的层一条绘制条目都没建（键 %d）\n", base);
            return false;
        }
    }
    // ② 有 0x213：脚本基数**覆盖**兜底，条目落在脚本的键上。
    {
        auto machine = run(build(true));
        if (machine.text_layer_draw_key(kGroup) != 190700 ||
            machine.draw_item(190700) == nullptr) {
            std::fprintf(stderr, "0x213 的基数被兜底覆盖 / 条目不在脚本键上（draw_key=%d）\n",
                         machine.text_layer_draw_key(kGroup));
            return false;
        }
    }
    return true;
}
// 选项**消费侧**：窗口开着 + 左键 + 命中一条带"决定入口"的选项 ⇒ 执行位置 ← 该入口（jump，
// 不压返回点）、清表、关窗。★ 只验机制；"何时消费"的次序仍是残差 `ADV-CHOICE-CONSUMER`。
bool check_choice_consumer(const core::OpcodeTable& table) {
    // 布局：0x90(15 个 dword) + 0x94(1) + 0x91(3) = 19；落空路径的标记在 19，跳转目标在 22。
    auto script = make_script_with_body();
    emit_instruction(script, 0x90, {{0, 0}, {0, 0}, {0, 10}, {0, 10}, {0, -1}, {0, -1}, {0, 23}});
    emit_instruction(script, 0x94, {});
    emit_instruction(script, 0x91, {{0, 0}});
    emit_instruction(script, 0x2F7, {{0, 0}});  // 19：落空（0x91 闩锁支之后）会走到这里
    emit_instruction(script, 2, {});           // 22：落空路径到此为止
    emit_instruction(script, 0x2F7, {{0, 1}});  // 23：点中之后的**决定入口**
    emit_instruction(script, 2, {});
    core::Machine machine(0x12345678u, table);
    machine.set_cursor_source([](std::int32_t& x, std::int32_t& y) -> bool { x = 5; y = 5; return true; });
    int down = 0;  // 窗口开之前不按；开窗之后才按下（引擎里是"打开菜单的那一下"不被当选中）
    machine.set_mouse_button_source([&down](std::int32_t& mask) -> bool { mask = down; return true; });
    machine.load_root(script);
    if (!machine.step() || !machine.step()) {  // 0x90 建表 / 0x94 开窗
        std::fprintf(stderr, "choice-consumer 开窗前两步失败：%s\n", machine.last_stop().message.c_str());
        return false;
    }
    down = 1;  // 开窗之后新按下
    for (int i = 0; i < 6 && !machine.halted(); ++i) {
        if (!machine.step()) break;
    }
    if (machine.voice_option_2f7(1) != 1 || machine.voice_option_2f7(0) != 0 || !machine.choices().empty()) {
        std::fprintf(stderr, "点中选项没有跳到决定入口（decide=%d fallthrough=%d 条数=%zu）\n",
                     machine.voice_option_2f7(1), machine.voice_option_2f7(0), machine.choices().size());
        return false;
    }
    return true;
}
// `0x21c` 的谓词 A（定时器）：`0x238` 写的"起点 + 时长"必须在窗内挡住、到期才放行；
// ★ 相等仍算窗内（引擎 `now > start + duration` 才到期），所以 200 挡、201 放。
bool check_wait_timer(const core::OpcodeTable& table) {
    auto build = [&]() {
        auto script = make_script_with_body();
        emit_instruction(script, 0x238, {{0, 200}});
        emit_instruction(script, 0x21C, {});
        emit_instruction(script, 0x2F7, {{0, 0}});
        emit_instruction(script, 2, {});
        return script;
    };
    core::Machine machine = scene_step_machine(table);
    machine.load_root(build());
    if (!machine.step() || !machine.step()) {  // 0x238 / 0x21c
        std::fprintf(stderr, "wait-timer 起始两步失败：%s\n", machine.last_stop().message.c_str());
        return false;
    }
    // 第一拍服务会给起点盖章；起点 = 现在。
    if (!machine.step()) {
        std::fprintf(stderr, "wait-timer 盖章那拍停机\n");
        return false;
    }
    const auto waiting = [](const core::Machine& m) { return (m.state_flags() & 0x400u) != 0; };
    const auto start = static_cast<std::uint32_t>(machine.msg_wait(1));
    if (machine.msg_wait(2) != 200 || start == 0 || !waiting(machine)) {
        std::fprintf(stderr, "wait-timer 没建窗：B=%d C=%d waiting=%d\n", machine.msg_wait(1), machine.msg_wait(2),
                     waiting(machine) ? 1 : 0);
        return false;
    }
    const auto at = [&](std::uint32_t offset) {
        machine.set_host_clock_ms(start + offset);
        return machine.step();
    };
    if (!at(199) || !waiting(machine) || machine.voice_option_2f7(0) != 0) {
        std::fprintf(stderr, "wait-timer：199ms 就放行了\n");
        return false;
    }
    if (!at(200) || !waiting(machine) || machine.voice_option_2f7(0) != 0) {
        std::fprintf(stderr, "wait-timer：200ms（相等）就放行了，应仍算窗内\n");
        return false;
    }
    // 放行那一拍只服务、不派发（引擎的服务分支独占本拍）⇒ 再走一拍才执行下一条。
    if (!at(201) || waiting(machine)) {
        std::fprintf(stderr, "wait-timer：201ms 没有放行（waiting=%d）\n", waiting(machine) ? 1 : 0);
        return false;
    }
    if (!machine.step() || machine.voice_option_2f7(0) != 1) {
        std::fprintf(stderr, "wait-timer：放行后没执行下一条（opt=%d）\n", machine.voice_option_2f7(0));
        return false;
    }
    // 中止：`0x243` 把 (A,B,C) 写成 (1,0,0) ⇒ 没有窗，下一拍就放行。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x238, {{0, 5000}});
        emit_instruction(script, 0x243, {});
        emit_instruction(script, 0x21C, {});
        emit_instruction(script, 0x2F7, {{0, 0}});
        emit_instruction(script, 2, {});
        core::Machine m2 = scene_step_machine(table);
        m2.load_root(script);
        for (int i = 0; i < 3; ++i) {
            if (!m2.step()) {
                std::fprintf(stderr, "wait-timer(abort) 第 %d 步停机\n", i + 1);
                return false;
            }
        }
        m2.set_host_clock_ms(m2.scene_clock_ms() + 1);
        if (!m2.step() || (m2.state_flags() & 0x400u) != 0) {
            std::fprintf(stderr, "wait-timer：0x243 中止之后还在等（waiting=%d）\n",
                         (m2.state_flags() & 0x400u) != 0 ? 1 : 0);
            return false;
        }
        if (!m2.step() || m2.voice_option_2f7(0) != 1) {
            std::fprintf(stderr, "wait-timer：中止放行后没执行下一条（opt=%d）\n", m2.voice_option_2f7(0));
            return false;
        }
    }
    return true;
}
// `emulator-core-wait-colour-window`：`0x21c` 谓词 A 的「待处理」闩（引擎 `+0B5B4`）。
// 排窗（`0x202 delay 0 dur 200`）之后，窗**没走完之前**等待位（`0x400`）必须一直置起；
// 到期判据是 `现在 >= 起点+延迟+持续时间`（引擎 `jge`）⇒ `start + 200`（相等）**已到期**、那一拍放行。
// ★ 这与消息等待计时器（`0x238` 写的那对格）的 `现在 > 起点+时长`（`ja`，相等仍算窗内）**不是同一个比较**。
// ★ 判据是**时间**，不是"有没有待画格"：把闩写回恒 0 ⇒ 下一拍就放行 ⇒ 本条红。
// ★ 另两个用例守 `duration == 0`：盖章**不看持续时间**（引擎只判"起点为 0"）⇒ 第一次推进就盖章并立刻到期；
//   `dur 0 + delay 200` 则证明"延迟段仍算窗内、延迟走完那一刻到期"，绝不能把门永关。
bool check_wait_colour_window(const core::OpcodeTable& table) {
    const auto waiting = [](const core::Machine& m) { return (m.state_flags() & 0x400u) != 0; };
    const auto emit_item = [](std::vector<std::uint8_t>& bytes) {
        emit_instruction(bytes, 0x1FB,
                         {{0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
    };
    // 用例 1：dur = 200 的颜色窗。
    {
        auto script = make_script_with_body();
        emit_item(script);
        emit_instruction(script, 0x202, {{0, 1}, {0, 0}, {0, 200}, {0, 255}, {0, 0x00000000u}});
        emit_instruction(script, 0x21C, {});
        emit_instruction(script, 0x2F7, {{0, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine = scene_step_machine(table);
        machine.set_host_clock_ms(1000);
        machine.load_root(script);
        if (!machine.step() || !machine.step()) {  // 0x1FB / 0x202
            std::fprintf(stderr, "wait-colour-window 起始两步失败：%s\n",
                         machine.last_stop().message.c_str());
            return false;
        }
        if (!machine.scene_pending()) {
            std::fprintf(stderr, "wait-colour-window：0x202 没有置「待处理」闩（+0B5B4）\n");
            return false;
        }
        if (!machine.step() || !waiting(machine)) {  // 0x21c
            std::fprintf(stderr, "wait-colour-window：0x21c 没有置等待位\n");
            return false;
        }
        const core::DrawItem* item = machine.draw_item(1);
        if (item == nullptr || item->window_start == 0) {
            std::fprintf(stderr, "wait-colour-window：窗没有被盖章（start=%u）\n",
                         item == nullptr ? 0u : item->window_start);
            return false;
        }
        const auto start = item->window_start;
        // ★ 每一拍都按宿主的真实动作走：先 step（门 → 派发），再"走一次渲染步入"
        //   （`end_render()` = `0x4B40AA` / `0x4B40B0`：清脏位 `+0B5AC` 与待处理闩 `+0B5B4`）。
        //   本用例不设主循环模式字（`draw_mode == 0`）⇒ 门前渲染步不会请求帧，
        //   所以这里直接调渲染步入；⛔ 不调就等于"渲染步一直没跑"，那是另一种状态（闩本来就不清）。
        const auto at = [&](std::uint32_t offset) {
            machine.set_host_clock_ms(start + offset);
            if (!machine.step()) return false;
            machine.end_render();
            return true;
        };
        if (!at(0) || !waiting(machine)) {  // 起点那一拍
            std::fprintf(stderr, "wait-colour-window：start+0 就放行了\n");
            return false;
        }
        if (!at(199) || !waiting(machine)) {
            std::fprintf(stderr, "wait-colour-window：199ms 就放行了（窗还没走完）\n");
            return false;
        }
        if (!at(200) || waiting(machine)) {
            std::fprintf(stderr,
                         "wait-colour-window：200ms（相等）没有放行 —— 颜色窗在相等处已到期\n");
            return false;
        }
        // 放行是**门的服务分支**（那一拍不派发）⇒ 再走一拍才执行下一条。
        if (!machine.step() || machine.voice_option_2f7(0) != 1) {
            std::fprintf(stderr, "wait-colour-window：放行后没执行下一条（opt=%d）\n",
                         machine.voice_option_2f7(0));
            return false;
        }
    }
    // 用例 2：dur = 0 的排窗（盖章 → 立刻到期 ⇒ 不能把门永关）。
    {
        auto script = make_script_with_body();
        emit_item(script);
        emit_instruction(script, 0x202, {{0, 1}, {0, 0}, {0, 0}, {0, 255}, {0, 0x00000000u}});
        emit_instruction(script, 0x21C, {});
        emit_instruction(script, 0x2F7, {{0, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine = scene_step_machine(table);
        machine.set_host_clock_ms(1000);
        machine.load_root(script);
        if (!machine.step() || !machine.step() || !machine.step() || !waiting(machine)) {
            std::fprintf(stderr, "wait-colour-window(zero)：0x21c 没有置等待位\n");
            return false;
        }
        // ★ `CORE-TIMING-WINDOW`：「持续时间 ≤ 0 的窗**不走到期收尾**」—— 对绘制项，颜色窗那一段
        //   **整段跳过**：不插值、不写这一步的派生色、不改当前 / 终色、**不清起止与「有窗」位**。
        //   它的"到期"只是**读者视角**（`现在 >= 起点 + 延迟 + 0` 成立 ⇒ `derive_color` 取当前色）。
        //   ⇒ 第一拍仍然**盖章**（盖章只判"起点为 0"），但窗位 / 起点必须留着。
        //   ⛔ 断言"窗位被清 / 起点被清"就是回退了旧模型（把 dur ≤ 0 写成走正常收尾）。
        if (const core::DrawItem* item = machine.draw_item(1);
            item == nullptr || (item->flags & 2u) == 0 || item->window_start == 0 ||
            item->duration != 0 || item->color == item->target_color) {
            std::fprintf(stderr,
                         "wait-colour-window(zero)：dur=0 的窗走到了期收尾（flags=%u start=%u dur=%d "
                         "color=%08X target=%08X）—— 整段跳过 ⇒ 窗位 / 起点要留着、颜色不许被写\n",
                         item == nullptr ? 0u : item->flags, item == nullptr ? 0u : item->window_start,
                         item == nullptr ? -1 : item->duration,
                         item == nullptr ? 0u : item->color,
                         item == nullptr ? 0u : item->target_color);
            return false;
        }
        // 渲染步入（`0x4B40B0`）清闩：本模型的渲染步入是 `end_render()`。
        machine.end_render();
        machine.set_host_clock_ms(1016);
        if (!machine.step() || waiting(machine)) {
            std::fprintf(stderr, "wait-colour-window(zero)：时长 0 的窗把门永关了（pending=%d）\n",
                         machine.scene_pending() ? 1 : 0);
            return false;
        }
        if (!machine.step() || machine.voice_option_2f7(0) != 1) {
            std::fprintf(stderr, "wait-colour-window(zero)：放行后没执行下一条（opt=%d）\n",
                         machine.voice_option_2f7(0));
            return false;
        }
    }
    // 用例 3：dur = 0、delay = 200 ⇒ **延迟段仍算窗内**，延迟走完那一刻（相等）到期。
    // ★ 这条同时钉住"dur=0 的窗也会盖章"：不盖章就没有 1000 这个起点，也就不会有 200 ms 的等待。
    {
        auto script = make_script_with_body();
        emit_item(script);
        emit_instruction(script, 0x202, {{0, 1}, {0, 200}, {0, 0}, {0, 255}, {0, 0x00000000u}});
        emit_instruction(script, 0x21C, {});
        emit_instruction(script, 0x2F7, {{0, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine = scene_step_machine(table);
        machine.set_host_clock_ms(1000);
        machine.load_root(script);
        if (!machine.step() || !machine.step() || !machine.step() || !waiting(machine)) {
            std::fprintf(stderr, "wait-colour-window(zero+delay)：0x21c 没有置等待位\n");
            return false;
        }
        const core::DrawItem* item = machine.draw_item(1);
        if (item == nullptr || item->window_start != 1000) {
            std::fprintf(stderr,
                         "wait-colour-window(zero+delay)：dur=0 的窗没有在第一次推进时盖章（start=%u）\n",
                         item == nullptr ? 0u : item->window_start);
            return false;
        }
        const auto start = item->window_start;
        const auto at = [&](std::uint32_t offset) {
            machine.set_host_clock_ms(start + offset);
            if (!machine.step()) return false;
            machine.end_render();
            return true;
        };
        if (!at(199) || !waiting(machine)) {
            std::fprintf(stderr, "wait-colour-window(zero+delay)：延迟段内（199ms）就放行了\n");
            return false;
        }
        if (!at(200) || waiting(machine)) {
            std::fprintf(stderr, "wait-colour-window(zero+delay)：延迟走完（相等）没有放行\n");
            return false;
        }
        if (!machine.step() || machine.voice_option_2f7(0) != 1) {
            std::fprintf(stderr, "wait-colour-window(zero+delay)：放行后没执行下一条（opt=%d）\n",
                         machine.voice_option_2f7(0));
            return false;
        }
    }
    // 用例 4：只排**源矩形窗**（`0x239`）也挡门 —— 引擎的通用拉闩在种类分派之前（`0x4B1804`），不按种类。
    {
        auto script = make_script_with_body();
        emit_item(script);
        emit_instruction(script, 0x239, {{0, 1}, {0, 0}, {0, 100}, {0, 4}, {0, 2}, {0, 3}});
        emit_instruction(script, 0x21C, {});
        emit_instruction(script, 0x2F7, {{0, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine = scene_step_machine(table);
        machine.set_host_clock_ms(1000);
        machine.load_root(script);
        if (!machine.step() || !machine.step() || !machine.step() || !waiting(machine)) {
            std::fprintf(stderr, "wait-rect-window：0x21c 没有置等待位\n");
            return false;
        }
        const core::DrawItem* item = machine.draw_item(1);
        if (item == nullptr || item->window_start != 1000 || item->rect_duration != 100) {
            std::fprintf(stderr, "wait-rect-window：源矩形窗没有被盖章（start=%u dur=%d）\n",
                         item == nullptr ? 0u : item->window_start,
                         item == nullptr ? 0 : item->rect_duration);
            return false;
        }
        const auto start = item->window_start;
        const auto at = [&](std::uint32_t offset) {
            machine.set_host_clock_ms(start + offset);
            if (!machine.step()) return false;
            machine.end_render();
            return true;
        };
        if (!at(99) || !waiting(machine)) {
            std::fprintf(stderr, "wait-rect-window：源矩形窗没走完（99ms）就放行了\n");
            return false;
        }
        if (!at(100) || waiting(machine)) {
            std::fprintf(stderr, "wait-rect-window：源矩形窗到期（相等）没有放行\n");
            return false;
        }
        if (!machine.step() || machine.voice_option_2f7(0) != 1) {
            std::fprintf(stderr, "wait-rect-window：放行后没执行下一条（opt=%d）\n",
                         machine.voice_option_2f7(0));
            return false;
        }
    }
    return true;
}
// 自动消息（auto-message）：`0x1b7` 开启后，`0x72` 会按"时长公式"武装计时器，
// 到期**放行同一对位**（bit31 + bit30），不换行、不取下一行。
// 公式：`max(100, Time1 + Pitch1 × (N − 1 − 0x2e9 的格))`；本模型没有音频后端 ⇒ 语音恒不在播 ⇒ 第 1 组。
bool check_auto_message(const core::OpcodeTable& table) {
    const auto build = [&](std::int32_t auto_on) {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1B7, {{0, auto_on}});
        emit_instruction(script, 0x72, {{0, 9}});
        emit_instruction(script, 0x2F7, {{0, 0}});
        emit_instruction(script, 2, {});
        return script;
    };
    // ① Time1 = 2000、Pitch1 = 0 ⇒ 时长 2000；1999 ms 不放行、2000 ms 放行并执行下一条。
    {
        core::Machine machine = scene_step_machine(table);
        machine.set_config_int("message:AutoMessageTime1", 2000);
        machine.set_config_int("message:AutoMessagePitch1", 0);
        machine.load_root(build(1));
        if (!machine.step() || !machine.step()) {
            std::fprintf(stderr, "auto-message 起始两步失败：%s\n", machine.last_stop().message.c_str());
            return false;
        }
        if (!machine.auto_armed() || machine.auto_interval_ms() != 2000 || !machine.advance_waiting()) {
            std::fprintf(stderr, "自动消息没有武装：armed=%d interval=%u waiting=%d\n", machine.auto_armed() ? 1 : 0,
                         machine.auto_interval_ms(), machine.advance_waiting() ? 1 : 0);
            return false;
        }
        const auto armed_at = machine.scene_clock_ms();
        machine.set_host_clock_ms(armed_at + 1999);
        if (!machine.step()) {
            std::fprintf(stderr, "auto-message 第 1999ms 步停机\n");
            return false;
        }
        if (!machine.advance_waiting() || machine.voice_option_2f7(0) != 0) {
            std::fprintf(stderr, "自动消息提前放行了（1999ms）\n");
            return false;
        }
        machine.set_host_clock_ms(armed_at + 2000);
        if (!machine.step()) {
            std::fprintf(stderr, "auto-message 第 2000ms 步停机\n");
            return false;
        }
        if (machine.advance_waiting() || machine.voice_option_2f7(0) != 1) {
            std::fprintf(stderr, "自动消息到期没有放行 / 放行后没执行下一条（waiting=%d opt=%d）\n",
                         machine.advance_waiting() ? 1 : 0, machine.voice_option_2f7(0));
            return false;
        }
    }
    // ② Pitch1 = 2、cell = 0、N = 0 ⇒ 2000 + 2×(0−1−0) = 1998（系数与"−1−格"都真的进了公式）。
    {
        core::Machine machine = scene_step_machine(table);
        machine.set_config_int("message:AutoMessageTime1", 2000);
        machine.set_config_int("message:AutoMessagePitch1", 2);
        machine.load_root(build(1));
        if (!machine.step() || !machine.step()) {
            std::fprintf(stderr, "auto-message(pitch) 起始两步失败\n");
            return false;
        }
        if (machine.auto_interval_ms() != 1998) {
            std::fprintf(stderr, "Pitch 没有进公式：interval=%u（期望 1998）\n", machine.auto_interval_ms());
            return false;
        }
    }
    // ③ 自动消息关着（`0x1b7 0`）：不武装、等到 5000ms 也不放行（只有点击能放）。
    {
        core::Machine machine = scene_step_machine(table);
        machine.set_config_int("message:AutoMessageTime1", 2000);
        machine.load_root(build(0));
        if (!machine.step() || !machine.step()) {
            std::fprintf(stderr, "auto-message(off) 起始两步失败\n");
            return false;
        }
        machine.set_host_clock_ms(5000);
        if (!machine.step()) {
            std::fprintf(stderr, "auto-message(off) 第 5000ms 步停机\n");
            return false;
        }
        if (!machine.advance_waiting() || machine.auto_armed() || machine.voice_option_2f7(0) != 0) {
            std::fprintf(stderr, "自动消息关着却放行了（waiting=%d armed=%d opt=%d）\n",
                         machine.advance_waiting() ? 1 : 0, machine.auto_armed() ? 1 : 0,
                         machine.voice_option_2f7(0));
            return false;
        }
    }
    // ④ N 进公式：`0x71` 清空后行记录数 = 1，每 `0x6f` +1 ⇒ 两行结束时 N = 3；
    //    Time1 = 2000、Pitch1 = 10 ⇒ 2000 + 10×(3−1−0) = 2020。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x80, {{0, 9}});
        emit_instruction(script, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 5}, {0, 5}});
        emit_instruction(script, 0x75, {{0, 20}});
        emit_instruction(script, 0x74, {{0, 0}});
        emit_instruction(script, 0x71, {{0, 9}});
        emit_instruction(script, 0x6E, {{0, 0}, {5, 0}});
        emit_instruction(script, 0x6F, {{0, 9}});
        emit_instruction(script, 0x6F, {{0, 9}});
        emit_instruction(script, 0x1B7, {{0, 1}});
        emit_instruction(script, 0x72, {{0, 9}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.write_text(core::Operand{5, 0}, "AB");
        machine.set_config_int("message:AutoMessageTime1", 2000);
        machine.set_config_int("message:AutoMessagePitch1", 10);
        machine.load_root(script);
        for (int i = 0; i < 10; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "auto-message(N) 第 %d 步停机：%s\n", i + 1, machine.last_stop().message.c_str());
                return false;
            }
        }
        if (machine.line_record_count(9) != 3 || machine.auto_interval_ms() != 2020) {
            std::fprintf(stderr, "N 没有进公式：lines=%d interval=%u（期望 3 / 2020）\n",
                         machine.line_record_count(9), machine.auto_interval_ms());
            return false;
        }
    }
    return true;
}
// `0x32` 缩放拷贝的**像素公式**（语料：放大 = 双线性 4 邻域加权 + 0.5 截断；缩小 = 面积加权平均）。
// ★ 判据用 4×4 → 8×8 这种"每格都算得出来"的尺寸：最近邻会给 (1,1) = 源 (0,0)，
//   双线性给 (1,1) = 四邻域均值 —— 两者差得开，才是一条会红的断言。
bool check_texture_stretch(const core::OpcodeTable& table) {
    const auto fill = [](core::Machine& machine, std::uint32_t id, std::uint32_t w, std::uint32_t h,
                         const std::function<std::array<int, 4>(int, int)>& color) {
        auto& slot = machine.texture_entry(id).slot;
        slot.present = true;
        slot.width = w;
        slot.height = h;
        slot.rgba.assign(static_cast<std::size_t>(w) * h * 4u, 0);
        for (std::uint32_t y = 0; y < h; ++y) {
            for (std::uint32_t x = 0; x < w; ++x) {
                const auto c = color(static_cast<int>(x), static_cast<int>(y));
                const std::size_t at = (static_cast<std::size_t>(y) * w + x) * 4u;
                for (int k = 0; k < 4; ++k) slot.rgba[at + static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(c[k]);
            }
        }
    };
    const auto stretched = [&](core::Machine& machine, int steps) {
        for (int i = 0; i < steps; ++i) {
            if (!machine.step()) return false;
        }
        return true;
    };
    // ① 放大 4×4 → 8×8：红 = x·10、绿 = y·10、蓝 = 50、α = 255。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x32, {{0, 1}, {0, 2}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}, {0, 8}, {0, 8}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        fill(machine, 1, 4, 4, [](int x, int y) { return std::array<int, 4>{x * 10, y * 10, 50, 255}; });
        fill(machine, 2, 8, 8, [](int, int) { return std::array<int, 4>{0, 0, 0, 0}; });
        machine.load_root(script);
        if (!stretched(machine, 1)) {
            std::fprintf(stderr, "0x32 放大那一步停机：%s\n", machine.last_stop().message.c_str());
            return false;
        }
        const auto* out = machine.texture_slot(2);
        if (out == nullptr) {
            std::fprintf(stderr, "0x32 放大后目标纹理不见了\n");
            return false;
        }
        const auto at = [&](int x, int y, int c) {
            return static_cast<int>(out->rgba[(static_cast<std::size_t>(y) * 8u + static_cast<std::size_t>(x)) * 4u +
                                              static_cast<std::size_t>(c)]);
        };
        // (1,1)：sx = sy = 0.5 ⇒ 四邻域均值：红 (0+10+0+10)/4 = 5、绿 (0+0+10+10)/4 = 5。
        if (at(1, 1, 0) != 5 || at(1, 1, 1) != 5 || at(1, 1, 2) != 50) {
            std::fprintf(stderr, "0x32 放大 (1,1) 不是双线性：rgba=(%d,%d,%d,%d)\n", at(1, 1, 0), at(1, 1, 1),
                         at(1, 1, 2), at(1, 1, 3));
            return false;
        }
        // (2,0)：sx = 1.0、sy = 0 ⇒ 正好是源 (1,0)。
        if (at(2, 0, 0) != 10 || at(2, 0, 1) != 0) {
            std::fprintf(stderr, "0x32 放大 (2,0) 不等于源 (1,0)：rgba=(%d,%d)\n", at(2, 0, 0), at(2, 0, 1));
            return false;
        }
        // (7,7)：sx = sy = 3.5，索引夹到 3 ⇒ 源 (3,3)（边界复制）。
        if (at(7, 7, 0) != 30 || at(7, 7, 1) != 30) {
            std::fprintf(stderr, "0x32 放大 (7,7) 不等于源 (3,3)：rgba=(%d,%d)\n", at(7, 7, 0), at(7, 7, 1));
            return false;
        }
    }
    // ② 缩小 8×8 → 4×4：每 2×2 块同色 ⇒ 面积平均必须正好得到块色（最近邻抽点会时对时错）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x32, {{0, 3}, {0, 4}, {0, 0}, {0, 0}, {0, 8}, {0, 8}, {0, 0}, {0, 0}, {0, 4}, {0, 4}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        fill(machine, 3, 8, 8, [](int x, int y) { return std::array<int, 4>{x * 10, y * 10, 7, 255}; });
        fill(machine, 4, 4, 4, [](int, int) { return std::array<int, 4>{0, 0, 0, 0}; });
        machine.load_root(script);
        if (!stretched(machine, 1)) {
            std::fprintf(stderr, "0x32 缩小那一步停机：%s\n", machine.last_stop().message.c_str());
            return false;
        }
        const auto* out = machine.texture_slot(4);
        if (out == nullptr) {
            std::fprintf(stderr, "0x32 缩小后目标纹理不见了\n");
            return false;
        }
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const std::size_t at = (static_cast<std::size_t>(y) * 4u + static_cast<std::size_t>(x)) * 4u;
                const int red = out->rgba[at + 0], green = out->rgba[at + 1], blue = out->rgba[at + 2];
                // 面积平均：块 = 源 x∈[2x,2x+1] ⇒ 均值 = (20x + 20x+10)/2 = 20x+5（最近邻会给 20x）。
                if (red != x * 20 + 5 || green != y * 20 + 5 || blue != 7) {
                    std::fprintf(stderr, "0x32 缩小 (%d,%d) 不是面积平均：rgba=(%d,%d,%d)\n", x, y, red, green, blue);
                    return false;
                }
            }
        }
    }
    return true;
}
// `0x72` 的「等推进」闸（点击推进的唯一入口）：门位置起 ⇒ **一条指令都不派发**；
// 只有**左键**（位 0）到位才清位并继续。★ 右键（位 1）不算 —— 把它当放行会让右键菜单穿透。
bool check_adv_wait(const core::OpcodeTable& table) {
    auto build = [&]() {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x72, {{0, 9}});   // 等推进（槽 9）
        emit_instruction(bytes, 0x2F7, {{0, 0}});  // 语音槽 0 选项格 ← 1（可观察的"下一条真的跑了"）
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    const auto steps = [&](core::Machine& machine, int count, const char* what) {
        for (int i = 0; i < count; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "adv-wait(%s)：走到第 %d 步停机：%s\n", what, i + 1,
                             machine.last_stop().message.c_str());
                return false;
            }
        }
        return true;
    };
    // ① 没有输入：闸一直挡着，执行位置停在 0x72 之后（= 4 字节），后面的 0x2f7 不许跑。
    {
        core::Machine machine(0x12345678u, table);
        machine.set_mouse_button_source([](std::int32_t& mask) -> bool { mask = 0; return true; });
        machine.load_root(build());
        if (!steps(machine, 5, "idle")) return false;
        if (!machine.advance_waiting()) {
            std::fprintf(stderr, "0x72 没有置「等推进」位\n");
            return false;
        }
        // 0x72 占 3 个 32 位字（1 opcode + 2 操作数字），所以闸门置起后执行位置正当停在 12 字节。
        if (machine.frame().ip != 12 || machine.voice_option_2f7(0) != 0) {
            std::fprintf(stderr, "「等推进」期间竟然派发了指令（ip=%zu 字 opt=%d）\n",
                         static_cast<std::size_t>(machine.frame().ip / 4),
                         machine.voice_option_2f7(0));
            return false;
        }
    }
    // ② 只有右键按下：仍不许放行。
    {
        core::Machine machine(0x12345678u, table);
        machine.set_mouse_button_source([](std::int32_t& mask) -> bool { mask = 2; return true; });
        machine.load_root(build());
        if (!steps(machine, 5, "right")) return false;
        if (!machine.advance_waiting() || machine.voice_option_2f7(0) != 0) {
            std::fprintf(stderr, "右键把「等推进」放行了（waiting=%d opt=%d）\n",
                         machine.advance_waiting() ? 1 : 0, machine.voice_option_2f7(0));
            return false;
        }
    }
    // ④ **按住不放只放行一次**（引擎的输入是事件锁存，不是电平）：两条 0x72 连着来，
    //    按住期间第二条不许被同一次按下放行；松开再按才放行。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x72, {{0, 9}});
        emit_instruction(script, 0x2F7, {{0, 0}});
        emit_instruction(script, 0x72, {{0, 9}});
        emit_instruction(script, 0x2F7, {{0, 1}});
        emit_instruction(script, 2, {});
        int down = 0;
        core::Machine machine(0x12345678u, table);
        machine.set_mouse_button_source([&down](std::int32_t& mask) -> bool { mask = down; return true; });
        machine.load_root(script);
        if (!machine.step()) {  // 0x72 #1 武装
            std::fprintf(stderr, "adv-hold：step1 停机\n");
            return false;
        }
        down = 1;  // 按下
        if (!machine.step()) {  // 第 31 位的服务体放行（**交回迭代头 ⇒ 本迭代不派发**）
            std::fprintf(stderr, "adv-hold：step2 停机\n");
            return false;
        }
        if (machine.advance_waiting() || machine.voice_option_2f7(0) != 0) {
            std::fprintf(stderr, "adv-hold：第一次按下没有放行（opt0=%d waiting=%d）\n",
                         machine.voice_option_2f7(0), machine.advance_waiting() ? 1 : 0);
            return false;
        }
        if (!machine.step()) {  // 下一迭代：门字归 0 ⇒ 派发 0x2F7 #1
            std::fprintf(stderr, "adv-hold：放行之后停机\n");
            return false;
        }
        if (machine.voice_option_2f7(0) != 1 || machine.advance_waiting()) {
            std::fprintf(stderr, "adv-hold：放行之后没有执行下一条（opt0=%d waiting=%d）\n",
                         machine.voice_option_2f7(0), machine.advance_waiting() ? 1 : 0);
            return false;
        }
        if (!machine.step()) {  // 0x72 #2 武装（此时**仍按着**）
            std::fprintf(stderr, "adv-hold：step3 停机\n");
            return false;
        }
        if (!machine.advance_waiting()) {
            std::fprintf(stderr, "adv-hold：第二条 0x72 没武装\n");
            return false;
        }
        if (!machine.step()) {  // 按住的一拍：不许放行
            std::fprintf(stderr, "adv-hold：按住期间停机\n");
            return false;
        }
        if (!machine.advance_waiting() || machine.voice_option_2f7(1) != 0) {
            std::fprintf(stderr, "adv-hold：按住不放把第二条也放行了（opt1=%d）\n", machine.voice_option_2f7(1));
            return false;
        }
        down = 0;  // 松开
        if (!machine.step() || !machine.advance_waiting()) {
            std::fprintf(stderr, "adv-hold：松开那拍不该放行\n");
            return false;
        }
        down = 1;  // 再按
        if (!machine.step() || machine.advance_waiting()) {
            std::fprintf(stderr, "adv-hold：松开再按没有放行（waiting=%d）\n",
                         machine.advance_waiting() ? 1 : 0);
            return false;
        }
        if (!machine.step() || machine.voice_option_2f7(1) != 1) {
            std::fprintf(stderr, "adv-hold：放行之后没有执行下一条（opt1=%d）\n",                         machine.voice_option_2f7(1));
            return false;
        }
    }
    // ③ 左键按下：放行那一拍**交回迭代头**（`CORE-VM-STATE` 位序表第 18 行：第 31 位的服务后 =
    //    交回迭代头），下一迭代才执行下一条。
    {
        core::Machine machine(0x12345678u, table);
        machine.set_mouse_button_source([](std::int32_t& mask) -> bool { mask = 0; return true; });
        machine.load_root(build());
        if (!steps(machine, 3, "arm")) return false;
        machine.set_mouse_button_source([](std::int32_t& mask) -> bool { mask = 1; return true; });
        if (!machine.step()) {
            std::fprintf(stderr, "adv-wait(click)：点击那一步停机：%s\n",
                         machine.last_stop().message.c_str());
            return false;
        }
        if (machine.advance_waiting() || machine.voice_option_2f7(0) != 0) {
            std::fprintf(stderr, "左键没有清位 / 放行那一拍就派发了（waiting=%d opt=%d）\n",
                         machine.advance_waiting() ? 1 : 0, machine.voice_option_2f7(0));
            return false;
        }
        if (!machine.step() || machine.voice_option_2f7(0) != 1) {
            std::fprintf(stderr, "放行之后没有执行下一条（opt=%d）\n", machine.voice_option_2f7(0));
            return false;
        }
    }
    // ⑤ **位 31 置起之前的点击不许锁存**（实测回归：SN0000 第一句加载完被 TITLE 的 GAMESTART 点击
    //    直接跳过）。脚本先执行一条普通指令，再 0x72；点击发生在第一条指令那一拍（位 31 还没置起），
    //    0x72 置位后不许用这次点击放行；松开再按才放行。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x2F7, {{0, 2}});  // 位 31 置起之前的普通指令
        emit_instruction(script, 0x72, {{0, 9}});
        emit_instruction(script, 0x2F7, {{0, 1}});
        emit_instruction(script, 2, {});
        int down = 0;
        core::Machine machine(0x12345678u, table);
        machine.set_mouse_button_source([&down](std::int32_t& mask) -> bool { mask = down; return true; });
        machine.load_root(script);
        down = 1;  // 点击发生在 0x72 之前
        if (!machine.step()) {
            std::fprintf(stderr, "adv-prewait：预点击那一步停机\n");
            return false;
        }
        if (machine.voice_option_2f7(2) != 1 || machine.advance_waiting()) {
            std::fprintf(stderr, "adv-prewait：第一条普通指令没执行 / 提前置了等推进位\n");
            return false;
        }
        down = 0;  // 松开，再执行 0x72
        if (!machine.step() || !machine.advance_waiting()) {
            std::fprintf(stderr, "adv-prewait：0x72 没有武装\n");
            return false;
        }
        if (!machine.step()) {  // 第 31 位服务体：预点击不许放行
            std::fprintf(stderr, "adv-prewait：服务体那一步停机\n");
            return false;
        }
        if (!machine.advance_waiting() || machine.voice_option_2f7(1) != 0) {
            std::fprintf(stderr, "adv-prewait：位 31 置起之前的点击把第一条消息放行了（waiting=%d opt3=%d）\n",
                         machine.advance_waiting() ? 1 : 0, machine.voice_option_2f7(1));
            return false;
        }
        down = 1;  // 消息显示后再按：这才该放行
        if (!machine.step() || machine.advance_waiting()) {
            std::fprintf(stderr, "adv-prewait：新点击没有放行（waiting=%d）\n",
                         machine.advance_waiting() ? 1 : 0);
            return false;
        }
        if (!machine.step() || machine.voice_option_2f7(1) != 1) {
            std::fprintf(stderr, "adv-prewait：放行之后没有执行下一条（opt3=%d）\n",
                         machine.voice_option_2f7(1));
            return false;
        }
    }
    // ⑥ 起播清的是**整字**：输入字 / 键位号锁存 / 鼠标槽锁存 / 推进锁存都必须被丢掉
    //    （`0x41F055 mov dword ptr [ebx], 0`）。只清 `advance_input_latch_` 会让 0x101 / 0x100
    //    在 0x72 之后仍看到 0x72 之前的事件。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x72, {{0, 9}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.set_input_word(0x15u);
        machine.push_key_down_message(0x0D);          // VK_RETURN ⇒ 键位号锁存
        machine.push_mouse_button_message(false, true); // 槽锁存
        if (machine.latch_bits() == 0 || machine.latch_slots() == 0) {
            std::fprintf(stderr, "adv-prewait：测试装填失败（锁存没置起）\n");
            return false;
        }
        machine.load_root(script);
        if (!machine.step() || !machine.advance_waiting()) {
            std::fprintf(stderr, "adv-prewait：0x72 没有武装（清整字支）\n");
            return false;
        }
        if (machine.input_word() != 0 || machine.latch_bits() != 0 || machine.latch_slots() != 0) {
            std::fprintf(stderr, "adv-prewait：0x72 没有把整字 / 锁存清 0（word=0x%x bits=0x%x slots=0x%x）\n",
                         machine.input_word(), machine.latch_bits(), machine.latch_slots());
            return false;
        }
    }
    // ⑦ 跳过位（第 27 位）已置 ⇒ 0x72 **整段不做**：不武装第 31 位、不清输入字。
    //    门在 bit27 置起后不派发任何指令（服务体交回迭代头）⇒ 只能用 handler 级直派；
    //    正控是 0x88 先清 bit27，再跑同一条 0x72。
    {
        core::Machine machine(0x12345678u, table);
        machine.set_text_request_result([](std::int32_t) { return 1; });
        machine.dispatch_for_test(core::Instruction{0, 0x71, {{0, 9}}});
        if ((machine.state_flags() & (1u << 27)) == 0) {
            std::fprintf(stderr, "adv-skip：0x71 没把第 27 位置起\n");
            return false;
        }
        machine.set_input_word(0x15u);
        machine.dispatch_for_test(core::Instruction{0, 0x72, {{0, 9}}});
        if (machine.advance_waiting()) {
            std::fprintf(stderr, "adv-skip：跳过位置起时 0x72 仍然武装了第 31 位\n");
            return false;
        }
        if (machine.input_word() != 0x15u) {
            std::fprintf(stderr, "adv-skip：跳过位置起时 0x72 把输入字清了（word=0x%x）\n",
                         machine.input_word());
            return false;
        }
        // 正控：0x88 清第 27 位后再跑 0x72 ⇒ 清整字 + 武装第 31 位。
        machine.dispatch_for_test(core::Instruction{0, 0x88, {{0, 0}}});
        if ((machine.state_flags() & (1u << 27)) != 0) {
            std::fprintf(stderr, "adv-skip：0x88 没清第 27 位\n");
            return false;
        }
        machine.set_input_word(0x15u);
        machine.dispatch_for_test(core::Instruction{0, 0x72, {{0, 9}}});
        if (!machine.advance_waiting() || machine.input_word() != 0) {
            std::fprintf(stderr, "adv-skip：正控没有武装 / 清字（waiting=%d word=0x%x）\n",
                         machine.advance_waiting() ? 1 : 0, machine.input_word());
            return false;
        }
    }
    return true;
}
// 锚：0x423FE0（handler）· 0x423FF6（长度字 0x0B）· 0x424008（先重置）· 0x424011/0x42401B（先读 2 再读 1）
// · 0x424030/0x42403C/0x424048（三个浮点）· 0x49A6C0 / 0x49A6F0（写区间与平移）。
// ★ 判据两面：① 状态（区间起点 / 计数 / 三元平移 / 长度槽 11）；② 消费者真的按区间给**命中键**的绘制项加平移。
bool check_scene_self_transform(const core::OpcodeTable& table) {
    // 4x4 白块放在 (0,0)，键 = 5；可选用 0x229 设「区间 + X 平移」。
    const auto make_script = [&](bool with_transform, std::uint32_t start, std::uint32_t count,
                                 std::uint32_t translate_x) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x1F8, {{0, 1}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(bytes, 0x20B, {{0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 255}, {0, 0x00FFFFFFu}});
        emit_instruction(bytes, 0x1FB, {{0, 5}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        if (with_transform) {
            emit_instruction(bytes, 0x229, {{0, start}, {0, count}, {0, translate_x}, {0, 0}, {0, 0}});
        }
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    constexpr std::uint32_t kWidth = 8;
    const auto white_at = [](const core::Bytes& canvas, std::uint32_t x, std::uint32_t y) {
        const std::size_t at = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
        return at + 3 < canvas.size() && canvas[at] == 255 && canvas[at + 3] == 255;
    };
    // ① 没有 0x229 ⇒ 白块在 (0,0)，状态是"未激活"。
    {
        core::Machine machine(0x12345678u, table);
        machine.load_root(make_script(false, 0, 0, 0));
        machine.run(32);
        const core::Bytes canvas = core::rasterize_scene(machine, kWidth, 8);
        if (!white_at(canvas, 0, 0) || white_at(canvas, 4, 0) || machine.scene_self_transform().active) {
            std::fprintf(stderr, "0x229 缺席时画面/状态不对（白块应仍在 (0..3,0..3)）items=%zu\n",
                         machine.draw_item_count());
            return false;
        }
    }
    // ② 区间命中键 5、X 平移 2 ⇒ 白块右移 2；状态里三格都写对、长度槽 = 11。
    {
        core::Machine machine(0x12345678u, table);
        machine.load_root(make_script(true, 5, 1, 2));
        machine.run(32);
        const auto& self = machine.scene_self_transform();
        if (!self.active || self.range_start != 5 || self.range_count != 1 ||
            self.translate[0] != 2.0f || self.translate[1] != 0.0f || self.translate[2] != 0.0f) {
            std::fprintf(stderr, "0x229 状态不对：active=%d start=%u count=%u t=(%.1f,%.1f,%.1f)\n",
                         self.active ? 1 : 0, self.range_start, self.range_count, self.translate[0],
                         self.translate[1], self.translate[2]);
            return false;
        }
        const core::Bytes canvas = core::rasterize_scene(machine, kWidth, 8);
        if (white_at(canvas, 0, 0) || !white_at(canvas, 4, 0) || !white_at(canvas, 5, 0)) {
            std::fprintf(stderr, "0x229 的平移没有作用到区间内的绘制项（期望 (2,0) 起 4px）\n");
            return false;
        }
    }
    // ③ 区间不覆盖键 5（起点 6）⇒ 一个像素都不动（区间是**门槛**，不是"设了就全员平移"）。
    {
        core::Machine machine(0x12345678u, table);
        machine.load_root(make_script(true, 6, 1, 2));
        machine.run(32);
        const core::Bytes canvas = core::rasterize_scene(machine, kWidth, 8);
        if (!white_at(canvas, 0, 0) || white_at(canvas, 4, 0)) {
            std::fprintf(stderr, "0x229 的区间没有起门槛作用（键 5 不该被平移）\n");
            return false;
        }
    }
    // ④ 重写会**先重置**：两条 0x229（第一条区间宽、第二条窄且平移 0）⇒ 状态只等于第二条。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x229, {{0, 0}, {0, 1000}, {0, 7}, {0, 0}, {0, 0}});
        emit_instruction(bytes, 0x229, {{0, 5}, {0, 1}, {0, 0}, {0, 0}, {0, 0}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(bytes);
        machine.step();  // 第一条 0x229（区间 0..999、平移 X=7）
        machine.step();  // 第二条 0x229（先重置，再写窄区间）
        const auto& self = machine.scene_self_transform();
        if (self.range_start != 5 || self.range_count != 1 || self.translate[0] != 0.0f ||
            machine.frame().length_slot != 11) {
            std::fprintf(stderr,
                         "0x229 没有「先重置」：start=%u count=%u tx=%.1f 长度槽=%u（期望 5/1/0/11）\n",
                         self.range_start, self.range_count, self.translate[0],
                         static_cast<unsigned>(machine.frame().length_slot));
            return false;
        }
    }
    // ⑤ 读数失败（第 2 个操作数是没写过的字符串指针）⇒ 抛，但**重置已经发生**、长度槽已是 11。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x229, {{0, 5}, {8, 3}, {0, 0}, {0, 0}, {0, 0}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(bytes);
        machine.step();
        const auto& self = machine.scene_self_transform();
        if (!machine.halted() || machine.last_stop().message.rfind("read-int-unsupported-tag:8", 0) != 0 ||
            self.range_count != 0 || !self.active || machine.frame().length_slot != 11) {
            std::fprintf(stderr,
                         "0x229 读数失败的写序不对：msg=%s active=%d count=%u 长度槽=%u（期望 11）\n",
                         machine.last_stop().message.c_str(), self.active ? 1 : 0, self.range_count,
                         static_cast<unsigned>(machine.frame().length_slot));
            return false;
        }
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
    return true;}
// ── `0x9` 复位清单的其余可观察项（`CORE-OPS-FRAME-EXIT-SCRIPT`）────────────────────────
// 逐条对着 `data/spec/chapters/60-ops-frame.md` 的那份清单（行号见 S7 报告）：
//   · 「一个启动时登记的引擎重置钩子」—— 钩子必须被**调用**（它的两处写入在后续的
//     「帧槽全释放 / 状态字整字清 0 / 根帧装载写长度槽 0」之后都不可观察；能红的只有调用次数）；
//   · 「show-logo 标志 ← 0」—— 直接读那一格（`check_exit_script` 只经 `0x130` 间接读）；
//   · 「输入子系统执行一次更新（重新算出输入字）」—— 采样源非空 ⇒ 输入字被重算；
//   · 「纹理槽：set:CreateObject 第 1 位 ∧ set:AutoFreeTex 第 0 位 ⇒ 逐槽释放 1000 个」——
//     0..999 被释放、1000 不被释放、两个配置位各自缺一 ⇒ 不释放；
//   · 「按配置 set:DrawMode 重设的场景绘制模式」—— 构造后改配置，`0x9` 必须**重读**。
//   ⛔ 本函数不断言的两项（实现里没有对应结构 / 宿主接口 ⇒ 已在 S7 报告里交父级）：
//     「四组向量格按一份基准搬回」与「模式格为负 ⇒ 向游戏窗口发 0x1400」。
bool check_exit_script_reset_scope(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto root_dir = std::filesystem::temp_directory_path() / "amayui-cxx-selftest-exit-scope";
    std::filesystem::create_directories(root_dir);
    auto root_bytes = make_script_with_body();
    emit_instruction(root_bytes, 0x1A8, {});
    emit_instruction(root_bytes, 2, {});
    {
        std::ofstream out(root_dir / "ROOT.BIN", std::ios::binary);
        out.write(reinterpret_cast<const char*>(root_bytes.data()),
                  static_cast<std::streamsize>(root_bytes.size()));
    }
    core::Install install;
    install.root = root_dir;
    install.index.files.push_back(core::AlfFile{"ROOT.BIN", 0, 0, 0, 0});
    auto reset_script = make_script_with_body();
    emit_instruction(reset_script, 0x09, {});
    emit_instruction(reset_script, 2, {});
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "exit-script-reset: %s\n", what);
        return false;
    };
    // ① 正向：钩子被调、show-logo 0、输入字被重算、DrawMode 重读、0..999 的纹理槽被释放。
    {
        core::ConfigStore config;
        config.set_int("set:DrawMode", 1);
        config.set_int("set:CreateObject", 2);  // 第 1 位：纹理槽释放那一支
        config.set_int("set:AutoFreeTex", 1);   // 第 0 位
        core::Machine machine(key, table, &install, config);
        machine.set_show_logo(true);
        machine.set_key_sampler([] { return 0x5u; });
        machine.set_texture_file(7, 123);
        machine.texture_entry(7).slot.present = true;
        machine.set_texture_file(1000, 999);  // 表外（合法槽是 0..999）：⛔ 不许被清
        if (machine.texture_slot(7) == nullptr) return fail("夹具：纹理槽 7 没有布上");
        machine.set_config_int("set:DrawMode", 0);  // `0x9` 必须**重读**配置，不是沿用构造值
        machine.load_root(reset_script);
        if (!machine.step()) return fail("0x9 夹具执行失败");
        if (machine.show_logo()) return fail("0x9 没有把 show-logo 标志清 0");
        if (machine.engine_reset_hook_calls() != 1) {
            return fail("0x9 没有调用启动时登记的引擎重置钩子");
        }
        if (machine.input_word() != 0x5u) {
            std::fprintf(stderr, "exit-script-reset: input_word=0x%X\n", machine.input_word());
            return fail("0x9 没有让输入子系统更新一次（输入字没有重算）");
        }
        if (machine.draw_mode() != 0) {
            return fail("0x9 没有按配置 set:DrawMode 重设场景绘制模式");
        }
        const core::TextureRecord* freed = machine.texture_record(7);
        if (freed == nullptr || freed->file != -1 || machine.texture_slot(7) != nullptr) {
            return fail("0x9 没有按 CreateObject 位 1 ∧ AutoFreeTex 位 0 释放纹理槽 7");
        }
        const core::TextureRecord* outside = machine.texture_record(1000);
        if (outside == nullptr || outside->file != 999) {
            return fail("0x9 的纹理槽释放越过了 1000 个槽（0..999）");
        }
    }
    // ② 反方向：两个配置位各自缺一 ⇒ 一个槽都不释放（否则那一支的判据写成了"或"）。
    {
        core::ConfigStore config;
        config.set_int("set:DrawMode", 1);
        // ★ `set:CreateObject` 必须在**构造之后**才写成 0：构造期会拿绘制模式的值（1）当**位下标**
        //   去测 `1 << 1`（`CORE-OPS-RENDER-FRAME-STEP` 的「它还兼作一份配置的位下标」）；未置起 ⇒
        //   把该配置改写为 3 ⇒ 第 1 位反而置起。⇒ 夹具先给一个"该位置起"的值（构造期不动它），
        //   构造完再清掉它 —— 这才是"复位时第 1 位 = 0"这个前提的正确布法。
        config.set_int("set:CreateObject", 2);  // 第 1 位置起 ⇒ 构造期**不改写**它
        config.set_int("set:AutoFreeTex", 1);
        core::Machine machine(key, table, &install, config);
        machine.set_config_int("set:CreateObject", 0);  // 构造后再把第 1 位清掉
        machine.set_texture_file(7, 123);
        machine.load_root(reset_script);
        if (!machine.step()) return fail("0x9 反方向夹具（CreateObject 位 1 = 0）执行失败");
        const core::TextureRecord* kept = machine.texture_record(7);
        if (kept == nullptr || kept->file != 123) {
            return fail("set:CreateObject 第 1 位为 0 时 0x9 仍释放了纹理槽");
        }
    }
    {
        core::ConfigStore config;
        config.set_int("set:DrawMode", 1);
        config.set_int("set:CreateObject", 2);
        config.set_int("set:AutoFreeTex", 0);  // 第 0 位 = 0
        core::Machine machine(key, table, &install, config);
        machine.set_texture_file(7, 123);
        machine.load_root(reset_script);
        if (!machine.step()) return fail("0x9 反方向夹具（AutoFreeTex 位 0 = 0）执行失败");
        const core::TextureRecord* kept = machine.texture_record(7);
        if (kept == nullptr || kept->file != 123) {
            return fail("set:AutoFreeTex 第 0 位为 0 时 0x9 仍释放了纹理槽");
        }
    }
    std::filesystem::remove_all(root_dir);
    return true;
}
// ── 帧族形状（S1：`CORE-OPS-FRAME-CALL` / `-RET` / `-CALL-SCRIPT` / `-LOAD-FRAME` / `-EXIT` /
//    `-EXIT-SCRIPT` / `-LOCAL-RET-7C` / `-LOCAL-SET-7B`）──────────────────────────────────────
// ★ 这一批的判据全是 spec 写明、而实现缺失 / 偏离的点（每条都能被一处破坏弄红）：
//   · 返回栈**每帧 256**：第 257 次压栈必须**记诊断并拒绝**（⛔ 不许覆盖最旧 / 静默截断）；
//   · 新帧记录四格：返回栈栈顶 ← 0（内容不写）、两格本地块进入目标 ← −1、ip = 代码区起点、
//     长度槽 ← 0 —— `0x3` / `0x6` / `0x9` 走**同一条装载路径**；
//   · `0x6` 的父帧号 ← 「父帧候选」格（装载器只照抄，不是当前帧号）⇒ 泵的 −10 才可达；
//   · `0x2` 三支：父帧 ≥ 0 回帧 / **−10 = 泵挂起帧** / 其余负值（含 −11）与 −1 同一条退出路径；
//   · `0x5` 跳转支弹引擎级状态字保存栈（压入时清第 31 / 20 位；空栈**不写**；返回栈空要诊断）；
//   · `0x9` 复位**三个引用族池** ⇒ 槽里读回 0（不是只把目标值清掉）；
//   · 状态字第 26 位 = 本地块内输入等待：输入字 0 ⇒ 不清位、不停门；非 0 ⇒ 清位 + 输入字清 0，
//     登记过副进入目标则按它重入（返回点 = 当前指令的 dword 位置）。
bool check_frame_shape(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "frame-shape: %s\n", what);
        return false;
    };
    const std::uint32_t key = 0x12345678u;
    // 装载器夹具：三份脚本（统一文件 id 0 / 1 / 2；松散文件优先）。
    const auto root_dir = std::filesystem::temp_directory_path() / "amayui-cxx-selftest-frame-shape";
    std::filesystem::create_directories(root_dir);
    auto plain_child = make_script_with_body();
    emit_instruction(plain_child, 2, {});
    const auto write_script = [&](const char* name, const std::vector<std::uint8_t>& bytes) {
        std::ofstream out(root_dir / name, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),                  static_cast<std::streamsize>(bytes.size()));
    };
    write_script("FRAME0.BIN", plain_child);
    write_script("FRAME1.BIN", plain_child);
    write_script("FRAME2.BIN", plain_child);
    core::Install install;
    install.root = root_dir;
    install.index.files.push_back(core::AlfFile{"FRAME0.BIN", 0, 0, 0, 0});
    install.index.files.push_back(core::AlfFile{"FRAME1.BIN", 0, 0, 0, 0});
    install.index.files.push_back(core::AlfFile{"FRAME2.BIN", 0, 0, 0, 0});
    // ① 返回栈容量 256（`CORE-OPS-FRAME-CALL` 的成立条件最后一条）。
    //    先压 1 个可辨认的返回点（ip=0 ⇒ 位置 3），再压 255 个（ip=40 ⇒ 位置 13）⇒ 正好 256。
    {
        auto script = make_script_with_body();
        for (int i = 0; i < 14; ++i) emit_instruction(script, 0x1A8, {});  // 位置 10 必须落在代码内
        core::Machine machine(key, table);
        machine.load_root(script);
        machine.dispatch_for_test(core::Instruction{0, 0x8F, {{0, 10}}});  // 返回点 3，跳到位置 10
        for (int i = 0; i < 255; ++i) machine.dispatch_for_test(core::Instruction{0, 0x8F, {{0, 10}}});
        if (machine.frame().ret_stack.size() != core::Machine::kRetStackCapacity) {
            std::fprintf(stderr, "frame-shape: 返回栈没有占满 256（实得 %zu）\n",
                         machine.frame().ret_stack.size());
            return false;
        }
        if (!machine.errors().empty()) return fail("第 256 次压栈就记了诊断");
        if (machine.frame().ret_stack.front() != 3) return fail("占满 256 之前最旧一项就被动了");
        machine.dispatch_for_test(core::Instruction{0, 0x8F, {{0, 10}}});  // 第 257 次
        if (machine.errors().empty()) return fail("第 257 次压栈没有诊断（被静默吞掉）");
        if (machine.frame().ret_stack.size() != core::Machine::kRetStackCapacity) {
            return fail("第 257 次压栈越界写（栈顶被推过容量）");
        }
        if (machine.frame().ret_stack.front() != 3) return fail("第 257 次压栈覆盖了最旧一项");
    }
    // ② 新帧记录的形状 + `0x3` 的父帧候选：先把槽 1 弄脏（返回栈 / 两格本地块目标都非缺省），
    //    再执行 `0x3` ⇒ 新记录必须把三样都复位（⛔ 只复位其中一样抓不到另外两处）。
    {
        auto root3 = make_script_with_body();
        emit_instruction(root3, 0x03, {{0, 0}});  // 换脚本：统一文件 id 0
        emit_instruction(root3, 2, {});
        core::Machine machine(key, table, &install);
        machine.load_root(root3);
        machine.dispatch_for_test(core::Instruction{0, 0x06, {{0, 1}, {0, 1}}});  // 装 FRAME1 进槽 1
        machine.dispatch_for_test(core::Instruction{0, 0x08, {{0, 1}}});
          // 切到槽 1
        machine.dispatch_for_test(core::Instruction{0, 0x8F, {{0, 0}}});
          // 槽 1 压返回点
        machine.dispatch_for_test(core::Instruction{0, 0x7B, {{0, 9}, {0, 7}}});  // 两格 = 9 / 7
        machine.dispatch_for_test(core::Instruction{0, 0x02, {}});
                // 回槽 0
        if (machine.frame_at(1).ret_stack.empty() || machine.frame_at(1).local_main != 9 ||
            machine.frame_at(1).local_sub != 7) {
            return fail("夹具没有弄脏槽 1（下面的形状断言会退化成恒真）");
        }
        if (!machine.step()) return fail("0x3 没有执行（夹具脚本有问题）");
        if (machine.current_frame() != 1) return fail("0x3 没有切到子帧");
        const auto& child = machine.frame_at(1);
        if (!child.ret_stack.empty()) return fail("0x3 之后子帧返回栈栈顶不是 0");
        if (child.local_main != -1 || child.local_sub != -1) {
            return fail("0x3 之后子帧两格本地块进入目标不是 −1");
        }
        if (child.ip != 0 || child.length_slot != 0) {
            return fail("0x3 之后子帧没有『ip = 代码区起点、长度槽 0』");
        }
        if (child.parent != 0) return fail("0x3 之后子帧父帧号不是调用帧（0）");
        if (machine.parent_candidate() != 0) {
            return fail("0x3 没有在装帧前把「父帧候选」格写成调用时正在执行的帧号");
        }
    }
    // ③ 装载器只照抄「父帧候选」格（`CORE-OPS-FRAME-LOAD-FRAME` 的成立条件最后一条）：
    //    候选格 = −10 ⇒ 装出来的帧父帧号 = −10（泵的正命令就靠这一条；⛔ 不是"装载器读当前帧号"）。
    {
        auto root6 = make_script_with_body();
        emit_instruction(root6, 0x06, {{0, 2}, {0, 3}});  // 装 FRAME2 进槽 3
        emit_instruction(root6, 2, {});
        core::Machine machine(key, table, &install);
        machine.load_root(root6);
        machine.set_parent_candidate_for_test(-10);
        machine.load_frame_for_test(2, 3);  // 直接调装载器（泵那条路）
        if (machine.frame_at(3).parent != -10) {
            return fail("装载器没有照抄「父帧候选」格（−10）⇒ 泵帧的父帧号会变成当前帧号，0x2 的 −10 支永不达");
        }
        // 对照：`0x6` 自己是「父帧候选」的写者（写"装载时正在执行的帧号"），不是照抄者。
        if (!machine.step()) return fail("0x6 没有执行");
        if (machine.frame_at(3).parent != 0) {
            return fail("0x6 没有把「父帧候选」格写成装载时正在执行的帧号（0）");
        }
    }
    // ④ `0x2` 三支（`CORE-OPS-FRAME-EXIT`）：−10 = 泵挂起帧；−1 / −11 / 其余负值同一条退出路径。
    {
        auto pump_script = make_script_with_body();
        emit_instruction(pump_script, 2, {});
        core::Machine pump(key, table, &install);
        pump.load_root(pump_script);
        pump.set_parent_candidate_for_test(-10);
        pump.load_frame_for_test(2, 0);   // 当前帧 0 的父帧号 = −10（当前帧仍是 0）
        pump.set_pump_state_for_test(1, true);   // 挂起帧 = 1、队列泵占用标记 = 1
        pump.seed_frame_length_for_test(1, 7);   // 挂起帧长度槽先写非 0 ⇒ 证明它被清 0
        if (pump.frame_at(0).parent != -10) return fail("夹具：当前帧的父帧号不是 −10");
        pump.dispatch_for_test(core::Instruction{0, 0x02, {}});
        if (pump.current_frame() != 1) return fail("0x2 的 −10 支没有接回泵挂起帧");
        if (pump.pump_busy()) return fail("0x2 的 −10 支没有清队列泵占用标记");
        if (pump.frame().length_slot != 0) return fail("0x2 的 −10 支没有把挂起帧长度槽清 0");
        if (pump.halted()) return fail("0x2 的 −10 支被当成执行结束");
        const auto expect_exit = [&](std::int32_t parent) {
            core::Machine machine(key, table, &install);
            auto script = make_script_with_body();
            emit_instruction(script, 2, {});
            machine.load_root(script);
            if (parent != -1) {  // 根帧的父帧号本来就是 −1
                machine.set_parent_candidate_for_test(parent);
                machine.load_frame_for_test(0, 0);
            }
            if (machine.step()) return false;
            return machine.last_stop().reason == core::StopReason::Halted &&
                   machine.last_stop().message == "root-exit";
        };
        if (!expect_exit(-1)) return fail("父帧号 −1 没有走『整个执行结束』");
        // ★ −11 是**恢复存档专用入口**（新版合同）：它**不**走退出路径 —— 不切帧号、不释放帧、不抛异常，
        //   只把「父帧候选」写成 −1、读两个版本、把推进格数清 0。
        {
            core::Machine restore(key, table, &install);
            auto script = make_script_with_body();
            emit_instruction(script, 2, {});
            restore.load_root(script);
            restore.set_parent_candidate_for_test(-11);
            restore.load_frame_for_test(0, 0);
            if (!restore.step()) {
                std::fprintf(stderr, "−11 支停因 = %s\n", restore.last_stop().message.c_str());
                return fail("父帧号 −11 被当成了执行结束（它不该走退出路径）");
            }
            if (restore.halted()) return fail("父帧号 −11 被当成了执行结束");
            if (restore.parent_candidate() != -1) return fail("父帧号 −11 没有把「父帧候选」写成 −1");
            if (restore.frame().length_slot != 0) return fail("父帧号 −11 没有把推进格数清 0");
            if (restore.save_exit_version_reads() != 1) return fail("父帧号 −11 没有读两个存档版本");
        }
        if (!expect_exit(-2)) return fail("父帧号 −2 没有与 −1 走同一条退出路径（引擎不区分其余负值）");
    }
    // ⑤ `0x5` 的引擎级状态字保存栈：压入时清第 31 / 20 位；跳转支弹出即写；空栈不写。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x8F, {{0, 5}});  // 位置 0..2：call 位置 5（返回点 3）
        emit_instruction(script, 5, {});
           // 位置 3：ret
        emit_instruction(script, 2, {});
           // 位置 4
        for (int i = 0; i < 8; ++i) emit_instruction(script, 0x1A8, {});  // 位置 5..12
        core::Machine machine(key, table);
        machine.load_root(script);
        machine.push_engine_state_word_for_test(0x80100400u);  // 第 31 / 20 位 + 第 10 位
        machine.dispatch_for_test(core::Instruction{0, 0x8F, {{0, 5}}});
        machine.dispatch_for_test(core::Instruction{0, 0x05, {}});
        if (machine.state_flags() != 0x400u) {
            std::fprintf(stderr, "frame-shape: 0x5 跳转支还原的状态字 = 0x%X（期望 0x400）\n",
                         machine.state_flags());
            return fail("0x5 跳转支没有把保存栈弹出的值写回状态选择字（且压入时要清第 31 / 20 位）");
        }
        if (machine.engine_state_word_stack_size() != 0) return fail("0x5 跳转支没有弹出保存栈");
        if (!machine.errors().empty()) return fail("0x5 的正常返回记了诊断");
        // 空返回栈：未定义行为必须诊断（⛔ 不许静默成功），且只按长度槽 1 前进 4 字节。
        core::Machine empty(key, table);
        auto bare = make_script_with_body();
        emit_instruction(bare, 5, {});
        emit_instruction(bare, 2, {});
        empty.load_root(bare);
        if (!empty.step()) return fail("空返回栈的 0x5 不该停执行");
        if (empty.errors().empty()) return fail("空返回栈的 0x5 没有诊断（未定义行为被静默吞掉）");
        if (empty.frame().ip != 4) return fail("空返回栈的 0x5 没有只前进 4 字节（长度槽 1）");
        // 空**状态字保存栈** ⇒ 不写状态选择字（用直派避开"非 0 门字会挡住取指"的副作用）。
        core::Machine keep(key, table);
        auto bare2 = make_script_with_body();
        emit_instruction(bare2, 5, {});
        emit_instruction(bare2, 2, {});
        keep.load_root(bare2);
        keep.state_flags_mut() = 0x400u;
        keep.dispatch_for_test(core::Instruction{0, 0x05, {}});
        if (keep.state_flags() != 0x400u) return fail("空状态字栈时 0x5 写了状态选择字");
    }
    // ⑥ `0x9`：三个引用族池逐格清 0 + 根帧记录形状（与 0x3 / 0x6 同一条装载路径）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x55, {{3, 1}, {0, 42}});   // 位置 0..4：全局整型 1 = 42
        emit_instruction(script, 0x63, {{6, 0}, {3, 1}});
    // 位置 5..9：全局引用族 0 → 整型 1 的地址
        emit_instruction(script, 0x7B, {{0, 5}, {0, 6}});
    // 位置 10..14：两格本地块目标
        emit_instruction(script, 0x8F, {{0, 19}});
           // 位置 15..17：call 位置 19（返回点 18）
        emit_instruction(script, 0x1A8, {});
                 // 位置 18（返回点，占位）
        emit_instruction(script, 0x09, {});
                  // 位置 19：重置并重装根脚本
        emit_instruction(script, 2, {});
                     // 位置 20
        core::Machine machine(key, table, &install);
        machine.load_root(script);
        for (int i = 0; i < 4; ++i) {
            if (!machine.step()) return fail("0x9 夹具执行失败");
        }
        if (machine.pointer_slot(6, 0) == 0) {
            return fail("0x63 没有把地址写进全局引用族槽（下面的复位断言会退化成恒真）");
        }
        if (machine.frame().ret_stack.empty() || machine.frame().local_main != 5 ||
            machine.frame().local_sub != 6) {
            return fail("0x9 夹具：返回栈 / 两格本地块目标没有脏值");
        }
        if (!machine.step()) return fail("0x9 没有执行");
        if (machine.pointer_slot(6, 0) != 0) {
            std::fprintf(stderr, "frame-shape: 0x9 后 pointer_slot(6,0)=0x%X\n",
                         machine.pointer_slot(6, 0));
            return fail("0x9 没有清三个引用族全局池（槽里还读得到旧地址）");
        }
        const auto& root = machine.frame_at(0);
        if (!root.ret_stack.empty() || root.local_main != -1 || root.local_sub != -1) {
            return fail("0x9 重装的根帧没有复位『返回栈栈顶 0 / 两格 −1』");
        }
        if (root.ip != 0 || root.length_slot != 0 || root.parent != -1) {
            return fail("0x9 重装的根帧没有『ip = 代码区起点、长度槽 0、父帧号 −1』");
        }
        if (machine.current_frame() != 0) return fail("0x9 没有把帧号回 0");
    }
    // ⑦ 状态字第 26 位 = 本地块内的输入等待（`CORE-OPS-FRAME-LOCAL-RET-7C` 的输入服务分支）。
    //   (a) 输入字 0 ⇒ 位不清、门继续关着、**不是** GateStalled；输入非 0 ⇒ 清位 + 输入字清 0。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x7B, {{0, 8}, {0, 0xFFFFFFFFu}});  // 位置 0..4：main=8、sub=−1
        emit_instruction(script, 0x199, {});
                         // 位置 5
        emit_instruction(script, 0x1A8, {});
                         // 位置 6（返回点）
        emit_instruction(script, 2, {});
                             // 位置 7
        emit_instruction(script, 0x7C, {});
                          // 位置 8（目标）
        emit_instruction(script, 2, {});
                             // 位置 9
        core::Machine machine(key, table);
        machine.set_input_word(0);
        machine.load_root(script);
        for (int i = 0; i < 3; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "frame-shape: 位26 夹具第 %d 步失败 ip=%zu msg=%s\n", i,
                             machine.frame().ip, machine.last_stop().message.c_str());
                return fail("位 26 夹具执行失败");
            }
        }
        if ((machine.state_flags() & (1u << 26)) == 0 || machine.frame().ip != 24) {
            return fail("0x7c 之后状态字没有带着第 26 位、ip 不在返回点");
        }
        for (int i = 0; i < 10001 && !machine.halted(); ++i) machine.step();
        if (machine.halted()) return fail("第 26 位（有服务体）被当成 GateStalled 停死");
        if ((machine.state_flags() & (1u << 26)) == 0) {
            return fail("输入字 0 时第 26 位被服务体清掉了（应当继续等）");
        }
        if (machine.frame().ip != 24) return fail("输入字 0 时脚本前进了（门没关住）");
        if (!machine.errors().empty()) return fail("合法的第 26 位等待记了诊断");
        machine.set_input_word(1);
        if (!machine.step()) return fail("输入字非 0 时服务体把执行停了");
        if ((machine.state_flags() & (1u << 26)) != 0) return fail("输入字非 0 时没有清第 26 位");
        if (machine.input_word() != 0) return fail("输入服务没有把输入字清 0");
        if (machine.frame().ip != 24) return fail("没登记副目标时服务体不该移动执行位置");
        if (!machine.step() || machine.frame().ip != 28) {
            return fail("清位后脚本没有从返回点继续（位置 6 → 7）");
        }
    }
    //   (b) 登记过副进入目标 ⇒ 按它重入：存档字 = 状态字 | 第 25 位、状态字清 0、
    //       返回点 = 当前指令的 dword 位置、入块深度 = 本帧身份、ip ← 副目标、推进格数 0。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x7B, {{0, 8}, {0, 12}});  // 位置 0..4：main=8、sub=12
        emit_instruction(script, 0x199, {});
                 // 位置 5
        emit_instruction(script, 0x1A8, {});
                 // 位置 6（返回点）
        emit_instruction(script, 2, {});
                     // 位置 7
        emit_instruction(script, 0x7C, {});
                  // 位置 8（主目标）
        emit_instruction(script, 2, {});
                     // 位置 9
        emit_instruction(script, 0x1A8, {});
                 // 位置 10
        emit_instruction(script, 0x1A8, {});
                 // 位置 11
        emit_instruction(script, 0x7C, {});
                  // 位置 12（副目标）
        emit_instruction(script, 2, {});
                     // 位置 13
        core::Machine machine(key, table);
        machine.set_input_word(0);
        machine.load_root(script);
        for (int i = 0; i < 3; ++i) {
            if (!machine.step()) return fail("位 26 重入夹具执行失败");
        }
        if ((machine.state_flags() & (1u << 26)) == 0) return fail("夹具没有停在输入等待上");
        machine.set_input_word(1);
        if (!machine.step()) return fail("位 26 服务体执行失败");
        if (machine.frame().ip != 48) {
            std::fprintf(stderr, "frame-shape: 副目标重入 ip=%zu（期望 48 = 位置 12）\n",
                         machine.frame().ip);
            return fail("登记过副目标时没有按它重入");
        }
        if (machine.state_flags() != 0) return fail("重入时没有把状态选择字清 0（门应打开）");
        if (machine.local_save_word() != (1u << 25)) {
            return fail("重入时存档字不是『状态字 | 第 25 位』");
        }
        if (machine.local_save_pos() != 6) {
            return fail("重入时返回点不是当前指令的 dword 位置（位置 6）");
        }
        if (machine.local_save_depth() != 0) return fail("重入时入块深度不是本帧身份编号");
        if (machine.input_word() != 0) return fail("重入时没有把输入字清 0");
        // 副目标上的 0x7c 必须回到那个返回点，并把存档字清 0。
        if (!machine.step() || machine.frame().ip != 24 || machine.state_flags() != 0 ||
            machine.local_save_word() != 0) {
            return fail("副目标上的 0x7c 没有回到重入时记下的返回点");
        }    }
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
    // ⑨ 0xb4 成功装载写「该槽当前资源号」这一格（`0x4b5004`）；0xb6 清**同一格**（`0x4b506a`）与
    //    该通道的**活动**（`0x4b5075`）。
    //    ★ `0xb6` 有"后端不存在 ⇒ 一格都不清"的前置（`0x4b5056`）⇒ 这里先把后端接上；
    //    槽号的两个界（后端 15 / 引擎表 10）与越界处置在 `check_audio_slot_bounds()` 里逐条咬。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xB4, {{0, 7}, {0, 9}});
        emit_instruction(bytes, 0xB6, {{0, 9}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.set_se_backend_attached(true);
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
        // 起算时刻是该**槽**的起点格（每槽一份）：0 = 未盖章哨兵 ⇒ 交给下一次时间交付去盖章。
        if (machine.voice_slot_timer(2).start != 0) return fail("0x2f5 没有把起算清 0");
        if (!machine.voice_slot_timer(2).pending) return fail("0x2f5 没有置该槽的排队标记");
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
        // ★ 压低请求的**四条件**（`70-vm.md:21`）里有两格缺省不成立：「音乐淡出相关状态」格（`0xc0`
        //   读的那一格；它的写者 `0xc3` 未实现）与配置 `sound:MusicFadeOnVoicePlaying`。这里把前置
        //   摆出来 —— 断言的是"条件成立 ⇒ 第 14 位**写回状态选择字**"（旧版断言的 `bgm_ducked_`
        //   投影不算：它不写门字）。
        machine.seed_engine_aa9e4(1);
        machine.config_mut().set_int("sound:MusicFadeOnVoicePlaying", 1);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0xc4 停机");
        if (machine.voice_play_requests() != 1) return fail("0xc4 没有发播放请求");
        if (machine.voice_load_requests() != 1) return fail("0xc4 没有发装载请求");
        if (machine.voice_mirror_resource(0) != 0) return fail("0xc4 正常支没有清镜像资源格");
        if ((machine.state_flags() & (1u << 14)) == 0) {
            return fail("0xc4 没有把第 14 位写回状态选择字（压低待执行）");
        }
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
    // ⑬ 0x198：**移动文本层**（写层记录 +0x0C/+0x10，与 `0x70` 的第 4/5 个操作数同两格）；
    //    层号 0 ⇒ 默认层（初值 1）；只搬位置，不改宽 / 高、不碰字号。
    //    ★ 实测缺陷（2026-11-02，用户口径）：GAMESTART 的说明文字层 `0x70 9 492 92 0 0` +
    //      `0x198 9 706 425`，旧实现把 0x198 当"另一对偏移"存进字体槽 ⇒ 文字画到屏幕左上角。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x197, {{0, 10}});
        emit_instruction(bytes, 0x70, {{0, 3}, {0, 400}, {0, 300}, {0, 10}, {0, 20}});
        emit_instruction(bytes, 0x198, {{0, 0}, {0, 100}, {0, 200}});
        emit_instruction(bytes, 0x198, {{0, 3}, {0, 7}, {0, 8}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x198 前置 0x197 停机");
        if (!machine.step()) return fail("0x198 前置 0x70 停机");
        if (machine.list_record(3).f0c != 10 || machine.list_record(3).f10 != 20) {
            return fail("0x70 的层左 / 上没有被写进层记录");
        }
        if (!machine.step()) return fail("0x198(0,100,200) 停机");
        // ★ 层号 0 不是"槽 1"：它是**默认层**（`0x80` 写的选择子，初值 1）。
        if (machine.default_group() != 1) return fail("默认层初值不是 1");
        if (machine.list_record(1).f0c != 100 || machine.list_record(1).f10 != 200) {
            return fail("0x198 层号 0 没有落到默认层 1 的左 / 上");
        }
        if (machine.list_record(3).f0c != 10) return fail("0x198 写默认层时把层 3 的位置改了");
        if (!machine.step()) return fail("0x198(3,7,8) 停机");
        if (machine.list_record(3).f0c != 7 || machine.list_record(3).f10 != 8) {
            return fail("0x198 没有把左 / 上写进层 3");
        }
        // 位置不是"再叠一个偏移"：0x70 写的宽 / 高必须原样保留，字号也不动。
        if (machine.list_record(3).f14 != 400 || machine.list_record(3).f18 != 300) {
            return fail("0x198 改了层 3 的宽 / 高（它只搬位置）");
        }
        if (machine.list_record(1).f0c != 100) return fail("0x198 写层 3 时把默认层的位置改了");
        if (machine.glyph_height() != 24) return fail("0x198 改了 A 支字号（缺省 24）");
        if (machine.font_b_height() != 10) return fail("0x198 改了 B 支字号（此前 0x197(10)）");
    }
    // ⑭ 0x198 越界层号：引擎里是未定义行为 ⇒ 本实现**不写任何层、也不停机**。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x70, {{0, 2}, {0, 40}, {0, 40}, {0, 1}, {0, 2}});
        emit_instruction(bytes, 0x198, {{0, 12}, {0, 5}, {0, 6}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x198 越界层号不该停机");
        if (!machine.step()) return fail("0x198 越界层号不该停机（第二条）");
        for (std::size_t i = 0; i < 10; ++i) {
            const std::int32_t expect_x = (i == 2) ? 1 : 0;
            const std::int32_t expect_y = (i == 2) ? 2 : 0;
            if (machine.list_record(i).f0c != expect_x || machine.list_record(i).f10 != expect_y) {
                return fail("0x198 越界层号写了某个层的位置（应当不写、不停机）");
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
    machine.set_se_backend_attached(true);  // 0xb6 的"后端不存在 ⇒ 一格都不清"前置（`0x4b5056`）
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
    // ── 负槽号（`0xb6`）：后端那条是**有符号** `< 15` 比较 ⇒ 负值**通过**、请求照发；
    //    但两格都是表外写（spec `60-ops-voice.md:352`）⇒ 本作的选择 = **计一次停止请求、不做表外写、
    //    只记诊断**（登记为实现侧已知偏离）。★ 前置用 `0xb4` 摆（后端接上）；执行 `0xb6(-1)` 之后
    //    槽 9 的播放名格与通道记录必须一格不动。
    {
        auto negative = make_script_with_body();
        emit_instruction(negative, 0xB4, {{0, 7}, {0, 9}});
        emit_instruction(negative, 0xB6, {{0, 0xFFFFFFFFu}});  // 槽号 = −1
        emit_instruction(negative, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.set_se_backend_attached(true);
        machine.set_se_channel_for_test(9, true, 123, 50, 7);
        machine.load_root(negative);
        if (!machine.step()) return fail("负槽号夹具的 0xb4 停机");
        if (machine.se_slot_resource(9) != 7) return fail("负槽号夹具的 0xb4 没有写播放名格");
        if (!machine.step()) return fail("0xb6(槽号 = −1) 停机（负槽号不是异常）");
        if (machine.se_stop_requests() != 1) {
            return fail("0xb6(槽号 = −1) 没有记停止请求（负槽号通过后端的有符号比较）");
        }
        if (machine.se_slot_resource(9) != 7) {
            return fail("0xb6(槽号 = −1) 做了表外写（动了槽 9 的播放名格）");
        }
        const auto& channel = machine.se_channel(9);
        if (!channel.active || channel.start != 123 || channel.duration != 50 || channel.value != 7) {
            return fail("0xb6(槽号 = −1) 改了引擎通道记录（表外写不复刻）");
        }
        if (!has_error(machine, "se-stop-slot-negative:-1")) {
            return fail("0xb6(槽号 = −1) 没有记诊断");
        }
    }
    return true;
}
// ── 时间交付：两套到期计时器 + 第 14 → 9 位的接链（`CORE-VM-LOOP`） ──
/** 钉子（每条都能指向 spec 原句）：
 *  ① **音效 10 通道的懒盖章**：活动且起点为 0 ⇒ 起点 ← 本次读数；**未到期 ⇒ 该子系统重新置
 *     「仍有一件未完成的工作」**（`70-vm.md:19`）。★ 起点 0 是"未盖章"哨兵 ⇒ 盖章只能发生一次。
 *  ② **有符号到期**：判据是「读数 − 起点 ≥ 时长」的**有符号**比较（`70-vm.md:19`；`70-vm.md:192`
 *     明说音效 / 语音的到期判据是**有符号**、⛔ 不许并进颜色窗那一族）⇒ 相等已到期、差 1 未到期；
 *     起点落在"未来"时**有符号差为负 ⇒ 未到期**（无符号比较会判到期）。
 *  ③ **交付不发请求**：交付只读写引擎记录、把到期交给该子系统自己的释放 / 启动入口（`70-vm.md:23`）
 *     ⇒ 宿主请求计数在交付前后逐项不变。
 *  ④ **第 14 → 9 的接链**：语音推进 ⇒ 压低四条件成立 ⇒ 第 14 位**写回状态选择字**（`70-vm.md:21`）；
 *     第 14 位服务体在前置（语音空闲 ∧「仍有一件未完成的工作」≠ 0 ∧ 第 9 位未置）成立时清 14、
 *     置 9 并起计时（`70-vm.md:112`）；第 9 位服务体计时到清位（`70-vm.md:113`）—— 与 `0xc2` 共用
 *     同一计时器与清位点（`60-ops-voice.md:121` 的「同一生命周期」）。
 */
bool check_time_delivery(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "time-delivery: %s\n", what);
        return false;
    };
    constexpr std::uint32_t kBit9 = 1u << 9;
    constexpr std::uint32_t kBit14 = 1u << 14;
    // 宿主请求计数的九个观察口（交付前后必须逐项不变 —— ③）。
    const auto request_counts = [](const core::Machine& machine) {
        return std::vector<std::size_t>{
            machine.se_play_requests(),   machine.se_stop_requests(),   machine.se_load_requests(),
            machine.bgm_play_requests(),  machine.bgm_stop_requests(),  machine.bgm_fade_requests(),
            machine.voice_load_requests(), machine.voice_play_requests(), machine.voice_stop_requests(),
        };
    };
    // ① 懒盖章只发生一次：起点 0 ⇒ 起点 ← 本次读数；当步进度 0（未到期 ⇒ 重新置待办）。
    {
        core::Machine machine(0x12345678u, table);
        machine.set_se_channel_for_test(0, true, 0, 100, 42);
        machine.set_se_work_pending_for_test(true);
        machine.deliver_sound_time(1000);
        if (machine.se_channel(0).start != 1000) {
            return fail("① 懒盖章没有把起点写成本次读数");
        }
        if (!machine.se_channel(0).active) return fail("① 未到期就清了活动");
        if (machine.se_release_steps() != 0) return fail("① 未到期就走了释放入口");
        if (!machine.se_work_pending()) {
            return fail("① 未到期没有重新置「该子系统仍有一件未完成的工作」");
        }
        machine.deliver_sound_time(1050);
        if (machine.se_channel(0).start != 1000) {
            return fail("① 懒盖章发生了第二次（起点每一步都被盖 ⇒ “起点 0 是哨兵”被抹掉）");
        }
        if (machine.se_release_steps() != 0) return fail("① 第二步未到期就走了释放入口");
    }
    // ② 有符号到期：差 1 未到期；相等已到期并把该通道的值交给释放入口。
    {
        core::Machine machine(0x12345678u, table);
        machine.set_se_channel_for_test(0, true, 1000, 100, 42);
        machine.set_se_work_pending_for_test(true);
        machine.deliver_sound_time(1099);
        if (!machine.se_channel(0).active) {
            return fail("② 差 1 就判到期（判据应为「读数 − 起点 ≥ 时长」）");
        }
        if (machine.se_release_steps() != 0) return fail("② 差 1 就交给了释放入口");
        machine.deliver_sound_time(1100);
        if (machine.se_channel(0).active) {
            return fail("② 读数 − 起点 = 时长 没有判到期（相等已到期）");
        }
        if (machine.se_release_steps() != 1) return fail("② 到期没有把该通道交给释放入口（一次）");
        if (machine.se_last_released_value() != 42) {
            return fail("② 到期没有把该通道的值交给释放入口");
        }
        if (machine.se_work_pending()) {
            return fail("② 没有未完成的通道却仍置着「仍有一件未完成的工作」");
        }
        machine.deliver_sound_time(1200);
        if (machine.se_release_steps() != 1) return fail("② 到期之后该通道仍被交付（活动没清）");
    }
    {
        // ★ 符号性的判别用例：起点 = 0x80000010、读数 = 0x7FFFFFF0、时长 16。
        //   有符号差 = −32 ⇒ 未到期；若写成无符号，差 = 0xFFFFFFE0 ≥ 16 ⇒ 会判到期。
        core::Machine machine(0x12345678u, table);
        machine.set_se_channel_for_test(3, true, 0x80000010u, 16, 9);
        machine.set_se_work_pending_for_test(true);
        machine.deliver_sound_time(0x7FFFFFF0u);
        if (!machine.se_channel(3).active) {
            return fail("② 起点落在未来的通道被判到期（到期判据写成无符号了）");
        }
        if (machine.se_release_steps() != 0) return fail("② 起点在未来却走了释放入口");
    }
    // ① 的通道边界：第 10 个通道（下标 9）也在交付范围里（`70-vm.md:19` 的「10 个通道」）。
    {
        core::Machine machine(0x12345678u, table);
        for (std::size_t i = 0; i < 10; ++i) {
            machine.set_se_channel_for_test(i, true, 0, 1000, static_cast<std::int32_t>(i));
        }
        machine.set_se_work_pending_for_test(true);
        machine.deliver_sound_time(7);
        for (std::size_t i = 0; i < 10; ++i) {
            if (!machine.se_channel(i).active || machine.se_channel(i).start != 7) {
                return fail("① 10 个通道没有逐个懒盖章");
            }
        }
    }
    // ③ 交付不发任何宿主请求：请求计数逐项不变（音效 / BGM / 语音）。
    {
        core::Machine machine(0x12345678u, table);
        machine.seed_engine_aa9e4(1);
        machine.config_mut().set_int("sound:MusicFadeOnVoicePlaying", 1);
        machine.set_se_channel_for_test(9, true, 0, 10, 5);
        machine.set_se_work_pending_for_test(true);
        machine.set_voice_slot_for_test(2, 0, 0, 10, true);
        const auto before = request_counts(machine);
        machine.deliver_sound_time(1000);
        machine.deliver_voice_time(1000);
        if (request_counts(machine) != before) {
            return fail("③ 交付向宿主发了请求（请求计数变了）");
        }
    }
    // ④ 第 14 位服务体：三个前置齐 ⇒ 清 14、置 9、起计时（目标 = 满值、步长 1）。
    {
        core::Machine machine(0x12345678u, table);
        machine.state_flags_mut() |= kBit14;
        machine.set_voice_work_pending_for_test(true);  // 待办 ≠ 0
        machine.set_voice_busy(0);                      // 语音子系统空闲
        machine.service_gate_bit_for_test(14);          // 第 9 位未置
        if ((machine.state_flags() & kBit14) != 0) return fail("④ 第 14 位服务体没有清第 14 位");
        if ((machine.state_flags() & kBit9) == 0) return fail("④ 第 14 位服务体没有置第 9 位");
        if (machine.bgm_fade_target() != 10000) return fail("④ 交接没有把渐变目标设成满值");
        if (machine.bgm_fade_step() != 1) return fail("④ 交接没有把步长设成 1");
        // 第 9 位服务体：第一次只起计时，计时到才清位（与 `0xc2` 共用同一个计时器）。
        machine.advance_scene(2000, false);
        machine.service_gate_bit_for_test(9);
        if (!machine.bgm_fade_active()) return fail("④ 第 9 位的服务体提前清了位（计时没到）");
        machine.advance_scene(2000 + 10000, false);
        machine.service_gate_bit_for_test(9);
        if (machine.bgm_fade_active()) return fail("④ 计时到没有清第 9 位");
    }
    {
        core::Machine machine(0x12345678u, table);
        machine.state_flags_mut() |= kBit14;
        machine.set_voice_work_pending_for_test(true);
        machine.set_voice_busy(1);  // 语音不空闲 ⇒ 前置 1 不成立
        machine.service_gate_bit_for_test(14);
        if ((machine.state_flags() & kBit14) == 0) {
            return fail("④ 前置不成立时第 14 位被清掉（应保留、下个迭代再判）");
        }
        if ((machine.state_flags() & kBit9) != 0) return fail("④ 前置不成立时照样进了第 9 位");
    }
    {
        core::Machine machine(0x12345678u, table);
        machine.state_flags_mut() |= kBit14 | kBit9;  // 第 9 位已置 ⇒ 前置 3 不成立
        machine.set_voice_work_pending_for_test(true);
        machine.service_gate_bit_for_test(14);
        if ((machine.state_flags() & kBit14) == 0) {
            return fail("④ 第 9 位已置时第 14 位被清掉（应保留、下个迭代再判）");
        }
    }
    {
        core::Machine machine(0x12345678u, table);
        machine.state_flags_mut() |= kBit14;  // 待办标记 = 0 ⇒ 前置 2 不成立
        machine.service_gate_bit_for_test(14);
        if ((machine.state_flags() & kBit14) == 0) {
            return fail("④ 待办标记为 0 时第 14 位被清掉（应保留、下个迭代再判）");
        }
    }
    // ④b 语音推进 ⇒ 压低四条件成立 ⇒ 第 14 位**写回状态选择字**，随后服务链接成第 9 位。
    {
        core::Machine machine(0x12345678u, table);
        machine.seed_engine_aa9e4(1);                                       // 「音乐淡出相关状态」格非 0
        machine.config_mut().set_int("sound:MusicFadeOnVoicePlaying", 1);   // 配置非 0
        machine.set_voice_slot_for_test(1, 0, 0, 50, true);                 // 排队一段：延时 50
        if (machine.deliver_voice_time(1000)) return fail("④b 未到期的语音槽被判「有推进」");
        if (machine.voice_slot_timer(1).start != 1000) return fail("④b 语音槽没有懒盖章");
        if ((machine.state_flags() & kBit14) != 0) return fail("④b 未到期就置了第 14 位");
        if (!machine.deliver_voice_time(1050)) {
            return fail("④b 语音槽到「已过时间 ≥ 延时」没有返回「有推进」");
        }
        if ((machine.state_flags() & kBit14) == 0) {
            return fail("④b 语音推进没有把第 14 位写回状态选择字");
        }
        if (!machine.voice_work_pending()) {
            return fail("④b 置第 14 位时没有置「该子系统仍有一件未完成的工作」");
        }
        machine.service_gate_bit_for_test(14);
        if ((machine.state_flags() & kBit14) != 0 || (machine.state_flags() & kBit9) == 0) {
            return fail("④b 语音推进置起的第 14 位没有交接成第 9 位");
        }
    }
    // ④c 压低四条件任一不成立 ⇒ 第 14 位不置（语音推进本身照常）。
    {
        core::Machine machine(0x12345678u, table);
        machine.config_mut().set_int("sound:MusicFadeOnVoicePlaying", 1);
        machine.set_voice_slot_for_test(0, 0, 1000, 10, true);
        if (!machine.deliver_voice_time(1010)) return fail("④c 到期的语音槽没有返回「有推进」");
        if ((machine.state_flags() & kBit14) != 0) {
            return fail("④c 「音乐淡出相关状态」格为 0 时仍置了第 14 位");
        }
    }
    {
        core::Machine machine(0x12345678u, table);
        machine.seed_engine_aa9e4(1);
        machine.state_flags_mut() |= kBit9;  // 第 9 位已置 ⇒ 条件 3 不成立
        machine.config_mut().set_int("sound:MusicFadeOnVoicePlaying", 1);
        machine.set_voice_slot_for_test(0, 0, 1000, 10, true);
        if (!machine.deliver_voice_time(1010)) return fail("④c 到期的语音槽没有返回「有推进」");
        if ((machine.state_flags() & kBit14) != 0) {
            return fail("④c 第 9 位已置时仍置了第 14 位");
        }
    }
    {
        core::Machine machine(0x12345678u, table);
        machine.seed_engine_aa9e4(1);
        machine.config_mut().set_int("sound:MusicFadeOnVoicePlaying", 0);  // 配置为 0 ⇒ 条件 2 不成立
        machine.set_voice_slot_for_test(0, 0, 1000, 10, true);
        if (!machine.deliver_voice_time(1010)) return fail("④c 到期的语音槽没有返回「有推进」");
        if ((machine.state_flags() & kBit14) != 0) {
            return fail("④c 配置 sound:MusicFadeOnVoicePlaying = 0 时仍置了第 14 位");
        }
    }
    {
        // 主循环模式字第 0 位 = 1 ⇒ 条件 4 不成立（`0x24e` 写那个字）。
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x24E, {{0, 1}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("④c 夹具：0x24e 停机");
        machine.seed_engine_aa9e4(1);
        machine.config_mut().set_int("sound:MusicFadeOnVoicePlaying", 1);
        machine.set_voice_slot_for_test(0, 0, 1000, 10, true);
        if (!machine.deliver_voice_time(1010)) return fail("④c 到期的语音槽没有返回「有推进」");
        if ((machine.state_flags() & kBit14) != 0) {
            return fail("④c 主循环模式字第 0 位已置时仍置了第 14 位");
        }
    }
    return true;
}
// ── 音效通道登记（0x2bf）→ 交付 / 停止（`70-vm.md`「音效通道」的登记段 + `60-ops-voice.md` 0xb6）──
/** 钉子（每条都指向 spec 原句 / 绑定 EA）：
 *  ① **登记**（`0x2bf` → `sub_4B5170`）：活动 ← 1（`0x4b51b7`）、起点 ← 0（`0x4b51be`）、
 *     时长 ← 第 3 个操作数（`0x4b51a8`）、交值 ← 第 2 个操作数（`0x4b51c9`）、待办 ← 1（`0x4b51d0`）；
 *     通道号 ≥ 10 一格都不写（`0x4b5179`）。
 *  ② **交付读它们**：未到期 ⇒ 置待办；到期（相等已到期）⇒ 清活动、把交值交给释放入口。
 *  ③ **0x2bf → 0xb6**：停止清该通道的活动，**不改**起点 / 时长 / 交值；此后不再到期。
 *  ★ 变异：登记时写错字段（通道 / 交值 / 时长任一）或 0xb6 只清表不清活动 ⇒ 本条红。 */
bool check_se_registration_2bf(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "se-registration: %s\n", what);
        return false;
    };
    // ① 登记 + 字段逐项（含"≥10 一格都不写"）。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2BF, {{0, 3}, {0, 7}, {0, 50}});   // 通道 3 / 交值 7 / 时长 50
        emit_instruction(bytes, 0x2BF, {{0, 10}, {0, 9}, {0, 80}});  // 通道 10：一格都不写
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x2bf 通道 3 停机");
        const auto& channel = machine.se_channel(3);
        if (!channel.active) return fail("登记没有把活动置 1");
        if (channel.start != 0) return fail("登记没有把起点写 0（未盖章哨兵）");
        if (channel.duration != 50) return fail("时长不是第 3 个操作数（50）");
        if (channel.value != 7) return fail("交值不是第 2 个操作数（7）");
        if (!machine.se_work_pending()) return fail("登记没有把待办置起");
        for (std::size_t i = 0; i < 10; ++i) {
            if (i == 3) continue;
            if (machine.se_channel(i).active || machine.se_channel(i).duration != 0 ||
                machine.se_channel(i).value != 0) {
                return fail("登记动了别的通道");
            }
        }
        if (!machine.step()) return fail("0x2bf 通道 10 停机");
        for (std::size_t i = 0; i < 10; ++i) {
            if (i == 3) continue;
            if (machine.se_channel(i).active || machine.se_channel(i).duration != 0 ||
                machine.se_channel(i).value != 0) {
                return fail("通道 ≥ 10 的登记写了一格");
            }
        }
        // ≥10 单独一台机器：连"待办"也不能置（那一格由登记写，≥10 一个格都不写）。
        auto only_high = make_script_with_body();
        emit_instruction(only_high, 0x2BF, {{0, 10}, {0, 9}, {0, 80}});
        emit_instruction(only_high, 2, {});
        core::Machine ignored(0x12345678u, table);
        ignored.load_root(only_high);
        if (!ignored.step()) return fail("通道 10 夹具停机");
        if (ignored.se_work_pending()) return fail("通道 ≥ 10 的登记把待办置起了");
    }
    // ② 交付两次：未到期 ⇒ 置待办；到期（相等）⇒ 清活动、交出交值。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2BF, {{0, 3}, {0, 7}, {0, 50}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("交付夹具 0x2bf 停机");
        machine.deliver_sound_time(1000);  // 懒盖章：起点 ← 1000
        if (machine.se_channel(3).start != 1000) return fail("交付没有懒盖章");
        if (!machine.se_channel(3).active) return fail("未到期就清了活动");
        if (machine.se_release_steps() != 0) return fail("未到期就走了释放入口");
        if (!machine.se_work_pending()) return fail("未到期没有置待办");
        machine.deliver_sound_time(1049);  // 差 1 未到期
        if (!machine.se_channel(3).active) return fail("差 1 就判到期");
        machine.deliver_sound_time(1050);  // 读数 − 起点 = 时长 ⇒ 到期
        if (machine.se_channel(3).active) return fail("到期没有清活动");
        if (machine.se_release_steps() != 1) return fail("到期没有交给释放入口（一次）");
        if (machine.se_last_released_value() != 7) return fail("到期交出去的不是登记的交值 7");
        if (machine.se_work_pending()) return fail("到期之后待办仍置着");
    }
    // ③ 0x2bf → 0xb6：停止清活动、不改三格，且此后不再到期。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2BF, {{0, 3}, {0, 7}, {0, 50}});
        emit_instruction(bytes, 0xB6, {{0, 3}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.set_se_backend_attached(true);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x2bf → 0xb6 夹具：登记停机");
        machine.deliver_sound_time(1000);  // 先盖一次章（起点 = 1000）
        if (!machine.step()) return fail("0x2bf → 0xb6 夹具：0xb6 停机");
        const auto& channel = machine.se_channel(3);
        if (channel.active) return fail("0xb6 没有清该通道的活动");
        if (channel.start != 1000) return fail("0xb6 改写了起点");
        if (channel.duration != 50 || channel.value != 7) return fail("0xb6 改写了时长 / 交值");
        const std::size_t released = machine.se_release_steps();
        machine.deliver_sound_time(2000);
        if (machine.se_release_steps() != released) return fail("停止之后该通道仍被交付");
    }
    return true;
}
// ── 按通道停止（0xb6）：后端不存在 ⇒ 一格不清；存在 ⇒ 先请求、再无条件清两格 ──
/** 钉子（`CORE-OPS-AUDIO-SLOT-STOP-B6`）：
 *  ① 后端不存在（接缝缺省 = 不存在；夹具先接上后端用 `0xb4` 摆前置、再断开）⇒ 到此为止：
 *     不发请求、播放名格与活动**原样**（`0x4b5056`）。
 *  ② 后端存在 ⇒ 先发停止请求（后端受理槽 < 15），再无条件清「播放名」格（`0x4b506a`）与活动（`0x4b5075`）；
 *     起点 / 时长 / 交值**不动**（那三格只由登记写）。
 *  ③ 槽号 ≥ 15：后端拒绝 + 上报（不发请求）；表外写不复刻（只记诊断）。
 *  ④ `0xb4` 的后端前置（`60-ops-voice.md:73`）：后端不存在 ⇒ 不写格、不发装载请求；接上后端 ⇒ 真写。
 *  ★ 变异：没有后端前置（总是清）/ 清了表却没清活动 / `0xb4` 没有后端前置 ⇒ 本条红。 */
bool check_se_stop_b6(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "se-stop: %s\n", what);
        return false;
    };
    const auto prepare = [](core::Machine& machine) {
        machine.set_se_channel_for_test(9, true, 123, 50, 7);
    };
    // ① 后端不存在：先"接上后端 → `0xb4` 摆播放名格 → 断开后端"制造那个状态。
    //    ⛔ 不能用产品指令在"后端不存在"的机器上摆前置 —— `0xb4` 自己也要求后端存在
    //    （`60-ops-voice.md:73`：整段跳过、一格不写），那样摆出来的前置是假前置。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xB4, {{0, 7}, {0, 9}});
        emit_instruction(bytes, 0xB6, {{0, 9}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        prepare(machine);
        machine.set_se_backend_attached(true);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0xb4 停机");
        if (machine.se_slot_resource(9) != 7) return fail("0xb4 没有写播放名格");
        machine.set_se_backend_attached(false);  // 前置摆好之后后端消失 ⇒ 测的才是"后端不存在"
        if (!machine.step()) return fail("0xb6 停机");
        if (machine.se_slot_resource(9) != 7) return fail("后端不存在时 0xb6 仍清了播放名格");
        const auto& channel = machine.se_channel(9);
        if (!channel.active || channel.start != 123 || channel.duration != 50 || channel.value != 7) {
            return fail("后端不存在时 0xb6 仍改了通道记录");
        }
        if (machine.se_stop_requests() != 0) return fail("后端不存在时 0xb6 仍发了停止请求");
    }
    // ② 后端存在。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xB4, {{0, 7}, {0, 9}});
        emit_instruction(bytes, 0xB6, {{0, 9}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        prepare(machine);
        machine.set_se_backend_attached(true);
        machine.load_root(bytes);
        if (!machine.step()) return fail("后端存在：0xb4 停机");
        if (!machine.step()) return fail("后端存在：0xb6 停机");
        if (machine.se_slot_resource(9) != 0) return fail("后端存在时 0xb6 没有清播放名格");
        const auto& channel = machine.se_channel(9);
        if (channel.active) return fail("后端存在时 0xb6 没有清活动");
        if (channel.start != 123 || channel.duration != 50 || channel.value != 7) {
            return fail("0xb6 改了起点 / 时长 / 交值（那三格只由登记写）");
        }
        if (machine.se_stop_requests() != 1) return fail("后端存在时 0xb6 没有发停止请求");
    }
    // ③ 槽号 ≥15：后端拒绝 + 上报（不发请求），表外写不做。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xB4, {{0, 7}, {0, 9}});
        emit_instruction(bytes, 0xB6, {{0, 15}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        prepare(machine);
        machine.set_se_backend_attached(true);
        machine.load_root(bytes);
        if (!machine.step()) return fail("越界夹具 0xb4 停机");
        if (!machine.step()) return fail("越界夹具 0xb6 停机");
        if (machine.se_slot_resource(9) != 7) return fail("槽 15 的 0xb6 动了槽 9");
        if (machine.se_stop_requests() != 0) return fail("槽 15 不该记停止请求");
        bool reported = false;
        for (const auto& text : machine.errors()) {
            if (text.rfind("se-stop-slot-out-of-range:15", 0) == 0) reported = true;
        }
        if (!reported) return fail("槽 15 没有上报拒绝");
    }
    // ④ `0xb4` 的后端前置（`60-ops-voice.md:73`）：后端不存在 ⇒ 整段跳过（不发请求、一格不写）；
    //    接上后端 ⇒ 同一份脚本真写。两支都要咬住 —— 否则"永远返回"或"永远不返回"都能绿。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xB4, {{0, 7}, {0, 9}});
        emit_instruction(bytes, 2, {});
        {
            core::Machine absent(0x12345678u, table);  // 缺省 = 后端不存在
            absent.load_root(bytes);
            if (!absent.step()) return fail("0xb4（无后端）停机");
            if (absent.se_slot_resource(9) != 0) return fail("后端不存在时 0xb4 仍写了播放名格");
            if (absent.se_load_requests() != 0) return fail("后端不存在时 0xb4 仍发了装载请求");
            if (absent.se_backend_attached()) return fail("接缝缺省不是「后端不存在」");
        }
        {
            core::Machine attached(0x12345678u, table);
            attached.set_se_backend_attached(true);
            attached.load_root(bytes);
            if (!attached.step()) return fail("0xb4（有后端）停机");
            if (attached.se_slot_resource(9) != 7) return fail("后端存在时 0xb4 没有写播放名格");
            if (attached.se_load_requests() != 1) return fail("后端存在时 0xb4 没有记装载请求");
        }
    }
    return true;
}
// ── 0x143 的投递源 = 文件库的 256 格槽位表（`CORE-OPS-EVENT-143`）──
/** 钉子（`60-ops-scalar.md:190-198` + 绑定 `0x455948` / `0x454cd0`）：
 *  ① 文件库装载一个**扩展索引**（区号 ∈ 1..255）⇒ 按**它自带的区号**登记进槽位表
 *     （不是 0x143 私有的表）；区号 0 是基础索引那一格（0x143 不检查）；
 *  ② 执行 `0x143` ⇒ 每个非 0 槽投一条 `区号 << 24`（≥128 是负命令）；
 *  ③ 文件库析构（逐槽清 0）之后 ⇒ 一条也不投。
 *  ★ 变异：装载不登记（旧实现：产品路径恒 0）/ 循环从 0 起 / 析构不清 ⇒ 本条红。
 *  ★ 为什么用 `load_extension_index()` 而不是 `read_install_file()`：本模型装到的都是**基础索引**的
 *    文件（统一 id 的高 8 位 = 0），按 0x455948 它们只写格 0；AAI 扩展索引的解析本模型没有
 *    （残差）⇒ 文件库的"装载扩展索引"这一入口由该 seam 承担。 */
bool check_event_143_file_library(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "event-143: %s\n", what);
        return false;
    };
    auto script = make_script_with_body();
    emit_instruction(script, 0x1F4, {});   // 区间忙 ⇒ 0x143 尾跳的泵不取命令
    emit_instruction(script, 0x143, {});
    emit_instruction(script, 0x1A8, {});
    emit_instruction(script, 2, {});
    {
        core::Machine machine(0x12345678u, table);
        machine.load_extension_index(5);    // 区号 5
        machine.load_extension_index(255);  // 区号 255（≥128 ⇒ 投递值是负命令）
        machine.load_extension_index(0);    // 基础索引那一格：登记，但 0x143 不检查它
        machine.load_extension_index(256);  // 表外：不写、只记诊断
        if (machine.file_library_slot(5) == 0 || machine.file_library_slot(255) == 0 ||
            machine.file_library_slot(0) == 0) {
            return fail("装载扩展索引没有按它自带的区号登记进文件库槽位表");
        }
        if (machine.file_library_slot(7) != 0) return fail("没装载的槽号在槽位表里非 0");
        bool zone_reported = false;
        for (const auto& text : machine.errors()) {
            if (text.rfind("file-library-zone-out-of-range:256", 0) == 0) zone_reported = true;
        }
        if (!zone_reported) return fail("区号 256 没有记表外诊断（写进表外了？）");
        machine.load_root(script);
        if (!machine.step()) return fail("0x1f4 停机");
        if (!machine.step()) return fail("区间忙时的 0x143 停机了");
        if (machine.pending_command_count() != 2) {
            std::fprintf(stderr, "event-143: delivered=%zu\n", machine.pending_command_count());
            return fail("0x143 没有逐格投递（或把槽 0 也投了）");
        }
        const std::int32_t first = machine.queue_command(machine.queue_head());
        const std::int32_t second = machine.queue_command(machine.queue_head() + 1);
        if (first != static_cast<std::int32_t>(5u << 24)) {
            std::fprintf(stderr, "event-143: first=0x%X\n", first);
            return fail("5 号槽的投递值不是 5 << 24");
        }
        if (second != static_cast<std::int32_t>(255u << 24)) {
            std::fprintf(stderr, "event-143: second=0x%X\n", second);
            return fail("255 号槽没有按 32 位有符号投递（≥128 是负命令）");
        }
        if (machine.pump_busy()) return fail("0x143 结束时没有把队列泵占用标记清回 0");
        if (machine.current_frame() != 0) return fail("区间忙时泵竟然取了命令（入口第一条失效）");
    }
    {
        // 基础索引的文件（`read_install_file` 装到的全部是这种）：按统一 id 的高 8 位 = **区号 0** 登记
        // ⇒ 写槽 0；`0x143` 不投槽 0。★ 这一条让"每个装载点都登记"可观察（删掉装载登记 ⇒ 槽 0 恒 0）。
        const auto root_dir = std::filesystem::temp_directory_path() / "amayui-cxx-selftest-library-base";
        std::filesystem::create_directories(root_dir);
        auto plain = make_script_with_body();
        emit_instruction(plain, 0x1A8, {});
        emit_instruction(plain, 2, {});
        {
            std::ofstream out(root_dir / "LIBBASE0.BIN", std::ios::binary);
            out.write(reinterpret_cast<const char*>(plain.data()),
                      static_cast<std::streamsize>(plain.size()));
        }
        core::Install install;
        install.root = root_dir;
        install.index.files.push_back(core::AlfFile{"LIBBASE0.BIN", 0, 0, 0, 0});
        core::Machine base(0x12345678u, table, &install);
        if (base.file_library_slot(0) != 0) return fail("装载之前槽 0 就非 0");
        base.load_frame_for_test(0, 1);  // 正常装载路径 ⇒ `read_install_file` 之后登记
        if (base.file_library_slot(0) == 0) return fail("装载基础索引的文件没有登记槽 0（区号 = 高 8 位）");
        if (base.file_library_slot(5) != 0) return fail("基础索引的文件登记到了别的槽");
        base.load_root(script);
        if (!base.step()) return fail("基础索引夹具 0x1f4 停机");
        if (!base.step()) return fail("基础索引夹具 0x143 停机了");
        if (base.pending_command_count() != 0) return fail("0x143 投了槽 0（基础索引那一格）");
    }
    {
        // 文件库析构（`0x454cd0`：逐槽销毁非空格并清 0）之后 ⇒ 0x143 一条也不投。
        core::Machine released(0x12345678u, table);
        released.load_extension_index(5);        released.release_file_library();
        if (released.file_library_slot(5) != 0) return fail("文件库析构没有逐槽清 0");
        released.load_root(script);
        if (!released.step()) return fail("卸载夹具 0x1f4 停机");
        if (!released.step()) return fail("卸载后的 0x143 停机了");
        if (released.pending_command_count() != 0) {
            return fail("文件库析构之后 0x143 仍有投递（槽位表没有逐槽清 0）");
        }
    }
    return true;
}
// ── 输出对象（`CORE-OBJECTS-BASE` 的「输出对象」三项操作）──
/** 钉子（`45-objects.md:50-58` + `05-boot.md:191`）：
 *  ① 追加：文本**原样**交给日志落点；
 *  ② 按脚本位置追加：`(<脚本名>：<行号>行目) ` 前缀 + 文本（脚本名 / 行号来源未取证 ⇒ 由调用方给）；
 *  ③ 报告：日志落点逐字收到 `\r\n致命的エラー[` + 文本 + `]\r\n`，随后把文本交给根对象的错误处理
 *     （「窗口句柄 + 文本」那一项）。接线 = 选择显示请求（触发一次范围 0 的 `0x60`）。
 *  ★ 变异：报告只记 `errors_` 不发日志 seam / 前缀拼错 ⇒ 本条红。 */
bool check_output_object_report(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "output-object: %s\n", what);
        return false;
    };
    // ① / ② 两个追加操作。
    {
        core::Machine machine(0x12345678u, table);
        std::vector<std::string> log;
        std::string root_text;
        core::Machine::OutputObjectIo io;
        io.log = [&log](const std::string& text) { log.push_back(text); };
        io.root_error_window = [&root_text](const std::string& text) { root_text = text; };
        machine.set_output_object_io(io);
        machine.output_append("plain");
        machine.output_append_with_position("SC0000.BIN", 12, "text");
        if (log.size() != 2) return fail("追加没有落到日志落点");
        if (log[0] != "plain") return fail("「追加」没有把文本原样交出去");
        if (log[1] != "(SC0000.BIN：12行目) text") {
            std::fprintf(stderr, "output-object: prefix=%s\n", log[1].c_str());
            return fail("「按位置追加」的前缀不是 `(<脚本名>：<行号>行目) `");
        }
        if (!root_text.empty()) return fail("普通追加触发了根错误处理");
    }
    // ③ 报告：触发一次选择显示请求（`0x60` 范围 = 0）⇒ 日志逐字 + 根错误处理收到文本。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x60, {{3, 1}, {0, 0}});  // 落点 = 全局整型 1；范围 = 0
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        std::vector<std::string> log;
        std::string root_text;
        core::Machine::OutputObjectIo io;
        io.log = [&log](const std::string& text) { log.push_back(text); };
        io.root_error_window = [&root_text](const std::string& text) { root_text = text; };
        machine.set_output_object_io(io);
        machine.load_root(bytes);
        if (machine.step()) return fail("范围 0 的 0x60 没有停机");
        const std::string expected = "\r\n致命的エラー[RANDOMの最大値が0になっています．]\r\n";
        if (log.size() != 1) return fail("报告没有向日志落点写一次（或写了不止一次）");
        if (log[0] != expected) {
            std::fprintf(stderr, "output-object: report=%s\n", log[0].c_str());
            return fail("报告的日志文本不是 `\\r\\n致命的エラー[文本]\\r\\n`");
        }
        if (root_text != "RANDOMの最大値が0になっています．") {
            return fail("报告没有把文本交给根对象的错误处理");
        }
        if (machine.output_reports() != 1) return fail("报告计数不是 1");
        if (!machine.errors().empty()) return fail("报告被混进了错误通道（errors_）");
    }
    return true;
}
// ── 门字第 27 位的两条支路：`0xc2` 的"瞬间到位"与 `0xc4` 的"影片门" ──
/** 第 27 位现在**有服务体**（`vm.cpp` 的 `case 27` → `service_message_frame`：处理 → 让出 → 窥视 →
 *  交回迭代头），产品置点也已落地（`0x19c` 的 C / E 支置位、`0x88` 的 0 支清位，见
 *  `spec_vm_gate_bit27_service` 与 `spec_ops_88_mode_cells`）。
 *  ★ 这两条支路仍用 `dispatch_for_test()`（门外直派）喂，理由**不是"没有服务体"**：位一旦置起，
 *    本迭代就被门挡住、脚本自己派发不到下一条 ⇒ 要测"位已置时这条 handler 怎么走"只能门外直派。
 *    ⛔ 这不是放宽门：门本身的行为另有守卫（`spec_vm_gate_bit27_service` / `0x19c` 那条）。 */
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
        // 压低四条件（`70-vm.md:21`）：两格缺省不成立 ⇒ 先摆前置，再断言第 14 位**写回门字**。
        machine.seed_engine_aa9e4(1);
        machine.config_mut().set_int("sound:MusicFadeOnVoicePlaying", 1);
        core::Instruction instruction{0, 0xC4, {{0, 777}}};
        machine.dispatch_for_test(instruction);
        if (machine.voice_mirror_resource(0) != 777) return fail("0xc4 影片支没有把 op1 写进镜像资源格");
        if (machine.voice_mirror_a4(0) != 0) return fail("0xc4 影片支写的镜像第二格不是 0");
        if (machine.voice_play_requests() != 0) return fail("0xc4 影片支仍发了播放请求");
        if (machine.voice_load_requests() != 0) return fail("0xc4 影片支仍发了装载请求");
        if ((machine.state_flags() & (1u << 14)) == 0) {
            return fail("0xc4 影片支没有把第 14 位写回状态选择字（压低待执行）");
        }
        if (machine.text_span_count() != 1) return fail("0xc4 影片支没有追加文本记录（规范：仍然追加）");
        if (machine.text_span_resource(0) != 777) return fail("0xc4 影片支的记录没有带 op1");
        if (machine.text_span_flags(0) != 0x40000000) return fail("0xc4 影片支的记录 flags 不是 0x40000000");
        if (machine.text_span_voice_slot(0) != 0) return fail("0xc4 影片支的记录配音槽不是 0");
    }
    return true;
}
// ── 门：22 位有序服务链 + 表尾派发（`CORE-VM-GATE` / `CORE-VM-STATE`） ──
/** 钉子（每条都能指向 spec 原句）：
 *  ① `CORE-VM-STATE` 的位序表第 8 项 = 第 30 位：「逐格揭示：按节拍请求画一格并推进格号」，
 *     **服务后 = 继续扫描（落到第 13 位）** ⇒ 它**不**交回迭代头（`70-vm.md:117`）⇒ 扫到表尾仍派发一条；
 *     这正是 `0x1ce` 的 0 支在门忙时仍可被派发的机制（`70-vm.md:65`、`60-ops-text.md:484`）。
 *  ② 第 14 / 9 / 12 是表里连续的三项（14 → 9 → 12），14 与 9 的「服务后」都是**继续扫描**、
 *     第 12 位是「条件：置起配对位那一支交回迭代头，其余落到第 26 位」（`70-vm.md:112-114`）。
 *     ★ 第 12 位的**体内三支判据未取证**（`RES-CLEAR-D9-BIT12` / `60-ops-scalar.md:297`）⇒ 本实现只落
 *     区间忙这道**前置**（`CORE-OPS-RUN-BRACKET`：区间忙 ≠ 0 时第 12 / 13 位的服务体**不执行**）：
 *       · 区间忙 = 0 ⇒ 体执行、按保守（未取证 ⇒ "其余"里选安全的那一支）**交回迭代头** ⇒ 本迭代不派发；
 *       · 区间忙 ≠ 0 ⇒ 体不执行 ⇒ **不算交回迭代头** ⇒ 继续扫到表尾、照常派发一条
 *         （`70-vm.md:25`："忙标记……不挡表尾派发 …… 暂停不阻止脚本推进"）。
 *  ③ 多个位同时置起 ⇒ **先被检查到的位先服务**，命中之后若它交回迭代头，更晚的位本迭代不再看
 *     （`70-vm.md:84` / `:136` 的「命中」定义：命中 ≠ 拦住派发，但"没轮到"就是未命中）。
 *     位序表里第 26 位在第 6 项、第 10 位在第 14 项 ⇒ 26 先（⛔ 不是位号升序：10 < 26）。
 *  ④ 第 29 位是表里**唯一**"由服务体自己派发"的位：命中 ⇒ 先跑一趟文本推进器，再按**文本槽在用标志**
 *     （`[77984h]`：槽 1..9 任一状态字非 0）决定 —— 非 0 ⇒ 随即派发一条；为 0 ⇒ 交回迭代头
 *     （`70-vm.md:59` / `:64`、服务链位序表第 17 行 `70-vm.md:123`）。
 *     ★ 它是**本趟入口快照**：同一趟里随后被抽干的槽仍算在用（`70-vm.md:64` 末句）。
 *     ⛔ 判据不是命令队列的占用标记（那是另一个状态；取证 `report-text-gate.md` §1.3 / §5.3）。
 *  ⑤ 第 29 位的**两支**（`70-vm.md:64` / `report-text-gate.md` §3.4）：普通打字（无槽在用）⇒ 打字期
 *     **本迭代零派发**；演示闸层（`0x300` 开着、槽在用）⇒ **每迭代派发一条**。
 *  ★ 每条都咬住"服务体**做了那一件事**"与"本迭代**派发没派发**"两半：只咬一半就抓不到"位序表被删项"。 */
bool check_gate_chain(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "gate-chain: %s\n", what);
        return false;
    };
    // ① 第 30 位置起 ⇒ 服务它之后**扫到表尾仍派发一条**（`0x1ce` 的 0 支可达性）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1CE, {{0, 7}});  // 置活动位（第 30 位）+ 节拍器相位 ← 现在
        emit_instruction(script, 0x1A8, {});
        // 表尾派发要执行的那一条（无副作用）
        emit_instruction(script, 2, {});
        core::Machine machine = scene_step_machine(table);
        machine.set_host_clock_ms(1000);
        machine.load_root(script);
        if (!machine.step() || (machine.state_flags() & (1u << 30)) == 0) {
            return fail("夹具：0x1ce 没有置起第 30 位");
        }
        machine.set_host_clock_ms(1600);  // 够一拍（默认节拍周期 500ms、相位起点 = 1000）
        const auto draws_before = machine.text_cell_draw_requests();
        const std::size_t ip_before = machine.frame().ip;  // = 12（0x1ce 之后）
        if (!machine.step()) return fail("第 30 位置起的那一拍停机了");
        if (machine.text_cell_draw_requests() != draws_before + 1) {
            return fail("第 30 位没有被服务（位序表第 8 项没有跑到它的服务体）");
        }
        if (machine.frame().ip != ip_before + 4) {
            return fail("扫到表尾没有派发一条（门字非 0 也不该挡住表尾派发）");
        }
        if ((machine.state_flags() & (1u << 30)) == 0) {
            return fail("第 30 位的服务体不该清位（清位在文本推进路径的共享清理里）");
        }
        if (!machine.errors().empty()) return fail("合法门忙记了诊断");
    }
    // ② 第 14 → 9 → 12 的链：三个位同置时按**表序**被服务，第 12 位的两条去向都覆盖。
    //   (a) 区间忙 = 0 ⇒ 12 的服务体执行、交回迭代头 ⇒ 本迭代不派发。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 2, {});
        core::Machine machine = scene_step_machine(table);  // 淡变按场景时钟推进 ⇒ 渲染步要开闸
        machine.load_root(script);
        if (!machine.step()) return fail("链夹具：第一步就停机");
        // 第 9 位的淡变状态（`0xc2` 的 B 支）：目标 20、间隔 50ms、步长 10 ⇒ 两次到期就走完。
        machine.dispatch_for_test(core::Instruction{0, 0xC2, {{0, 20}, {0, 500}}});
        if (!machine.bgm_fade_active()) return fail("夹具：0xc2 没有置起第 9 位");
        machine.state_flags_mut() |= (1u << 14) | (1u << 12);
        const std::size_t ip_before = machine.frame().ip;
        for (int i = 0; i < 20 && !machine.halted(); ++i) machine.step();
        if (machine.bgm_fade_progress() != machine.bgm_fade_target()) {
            return fail("第 9 位没有被服务体推到位（链在 14 之后没有落到 9）");
        }
        if (machine.bgm_fade_active()) return fail("第 9 位的服务体没有按时清位");
        if ((machine.state_flags() & (1u << 14)) == 0) {
            return fail("第 14 位的服务体落空时必须保留该位（前置不成立 ⇒ 下个迭代再判）");
        }
        if ((machine.state_flags() & (1u << 12)) == 0) {
            return fail("第 12 位（区间忙 = 0）的服务体没有执行");
        }
        if (machine.frame().ip != ip_before) {
            return fail("第 12 位交回迭代头 ⇒ 本迭代不该派发");
        }
    }
    //   (b) 区间忙 ≠ 0 ⇒ 12 的服务体不执行 ⇒ 继续扫到表尾 ⇒ **仍派发一条**（暂停不挡表尾派发）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.set_driven_elsewhere(true);
       // 区间忙标记 = 1（`0x1f4` 置）
        machine.state_flags_mut() |= (1u << 12);  // 第 10 位必须为 0（等待位给暂停开闸）
        const std::size_t ip_before = machine.frame().ip;
        if (!machine.step()) return fail("区间忙 + 第 12 位的那一拍停机了");
        if (machine.frame().ip != ip_before + 4) {
            return fail("区间忙时第 12 位挡下了派发（暂停不挡表尾派发）");
        }
        if ((machine.state_flags() & (1u << 12)) == 0) {
            return fail("区间忙时第 12 位的服务体不该执行");
        }
    }
    // ③ 两个"会交回迭代头"的位同时置起 ⇒ 只服务**位序更早**的那个（本迭代不再服务后一个）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        machine.set_input_word(1);  // 第 26 位的服务体：输入字非 0 ⇒ 清位
        machine.state_flags_mut() |= (1u << 26) | (1u << 10);
        const std::size_t ip_before = machine.frame().ip;
        if (!machine.step()) return fail("两位同置的那一拍停机了");
        if ((machine.state_flags() & (1u << 26)) != 0) {
            return fail("位序更早的第 26 位没有被服务");
        }
        if ((machine.state_flags() & (1u << 10)) == 0) {
            return fail("本迭代还服务了位序更晚的第 10 位（扫描该在 26 的交回处停下）");
        }
        if (machine.frame().ip != ip_before) return fail("第 26 位交回迭代头 ⇒ 本迭代不该派发");
        if (!machine.step()) return fail("下一迭代停机了");
        if ((machine.state_flags() & (1u << 10)) != 0) {
            return fail("下一迭代没有服务第 10 位");
        }
        if (machine.frame().ip != ip_before) return fail("第 10 位也交回迭代头");
        if (!machine.step() || machine.frame().ip != ip_before + 4) {
            return fail("门字归 0 之后没有派发一条");
        }
    }
    // ④ 第 29 位（`CORE-VM-GATE` / `CORE-OPS-TEXT-SLOT-TIMER`）：命中 ⇒ 先跑一趟文本推进器，再按
    //    **文本槽在用标志**决定收场：**有槽在用 ⇒ 服务体自己派发一条**；**无槽在用时 ⇒ 交回迭代头**。
    //    spec 句：`70-vm.md` 的**服务链位序表第 17 行**（「文本推进器；命中且文本槽在用标志非 0 ⇒
    //    随即派发一条」「为 0 ⇒ 交回迭代头」）+「门」条第三收场（「由它的服务体自己派发一条」）。
    //    ★ 判据**不是**待处理命令队列的占用格：那一格在门上几乎恒 0（投递口就在同一次调用里顺手泵掉，
    //    取证 `report-text-gate.md` §5.3）⇒ 拿它当判据会让演示闸永远派发不出去。下面让**队列空着**
    //    也必须派发，正是为了把这条反向钉住。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 2, {});
        auto slotted_script = make_script_with_body();
        // 槽 1 的状态字 ← 2：**非 0**（⇒ 有槽在用），但没有闸位（bit0）/ 粘住位（bit16）
        // ⇒ 这一趟推进器什么都不做，`[77984h]` 仍然按"任一状态字非 0"为 1。
        emit_instruction(slotted_script, 0x300, {{0, 1}, {0, 2}, {0, 0}});
        emit_instruction(slotted_script, 0x1A8, {});
        emit_instruction(slotted_script, 0x1A8, {});
        emit_instruction(slotted_script, 2, {});
        // (a) 无槽在用 ⇒ 交回迭代头（本迭代零派发）。
        core::Machine bare(0x12345678u, table);
        bare.load_root(script);
        if (bare.text_slot_in_use()) return fail("夹具：没有写槽却报了「有槽在用」");
        bare.state_flags_mut() |= (1u << 29);
        const std::size_t bare_ip = bare.frame().ip;
        if (!bare.step()) return fail("第 29 位 + 无槽那一拍停机了");
        if (bare.frame().ip != bare_ip) {
            return fail("无槽在用时第 29 位仍派发了（应为交回迭代头）");
        }
        // (b) 有槽在用（槽状态字非 0）⇒ 服务体自己派发一条 —— **队列空着也必须派发**。
        core::Machine slotted(0x12345678u, table);
        slotted.load_root(slotted_script);
        if (!slotted.step()) return fail("0x300 那一拍停机了");
        if (!slotted.text_slot_in_use()) {
            return fail("0x300 写了非 0 状态字之后没有置「有槽在用」标志");
        }
        if (slotted.pending_command_count() != 0) return fail("夹具：队列不是空的");
        slotted.state_flags_mut() |= (1u << 29);
        const std::size_t slotted_ip = slotted.frame().ip;
        if (!slotted.step()) return fail("有槽 + 第 29 位那一拍停机了");
        if (slotted.frame().ip != slotted_ip + 4) {
            return fail("有槽在用时第 29 位没有派发一条（队列占用格规则会在这里交回迭代头）");
        }
        if (slotted.pending_command_count() != 0) {
            return fail("门上的第 29 位服务体不该取走命令（取命令只发生在泵里）");
        }
        if ((slotted.state_flags() & (1u << 29)) != 0) {
            return fail("夹具失效：派发时第 29 位仍置（无法证明判据是槽在用标志，而不是这一位本身）");
        }
    }
    // ⑤ 两支（`report-text-gate.md` §3.4）：**普通打字（没有 `0x300` 闸）⇒ 打字期零派发**；
    //    **演示闸（`0x300` 开着、槽在用）⇒ 每迭代派发一条**。
    {
        // 文本层夹具（`0x80` 默认槽 9 / `0x70` 层矩形 / `0x79` 原点 / `0x213` 键基数 / `0x71` 建画布）。
        const auto text_layer_setup = [&](std::vector<std::uint8_t>& bytes) {
            emit_instruction(bytes, 0x80, {{0, 9}});
            emit_instruction(bytes, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 5}, {0, 5}});
            emit_instruction(bytes, 0x79, {{0, 9}, {0, 5}, {0, 5}});
            emit_instruction(bytes, 0x213, {{0, 9}, {0, 190700}, {0, 40}});
            emit_instruction(bytes, 0x75, {{0, 20}});
            emit_instruction(bytes, 0x71, {{0, 9}});
        };
        // (a) 普通打字：`0x74` 起每字间隔、`0x6e` 追加两个待画格 ⇒ 打字期推进器走、脚本不前进。
        auto typing_script = make_script_with_body();
        text_layer_setup(typing_script);
        emit_instruction(typing_script, 0x74, {{0, 5}});  // 每字 5ms
        emit_instruction(typing_script, 0x6E, {{0, 0}, {5, 0}});  // "AB" ⇒ 两格
        emit_instruction(typing_script, 0x1A8, {});
              // 打字期间**不该**被执行
        emit_instruction(typing_script, 2, {});
        core::Machine typing = text_scene_machine(0x12345678u, table);
        typing.write_text(core::Operand{5, 0}, "AB");
        typing.load_root(typing_script);
        for (int i = 0; i < 8; ++i) {  // 夹具 6 条 + 0x74 + 0x6e
            if (!typing.step()) return fail("普通打字夹具：走到追加之后停机了");
        }
        if (!typing.text_typing()) return fail("间隔 > 0 时追加后应当置第 29 位（打字中）");
        if (typing.text_slot_in_use()) {
            return fail("普通打字不该有「有槽在用」（没有起 0x300 闸）");
        }
        const std::size_t typing_ip = typing.frame().ip;
        std::size_t last_pending = typing.pending_glyph_count();
        if (last_pending == 0) return fail("普通打字夹具：追加之后没有待画格");
        int drew = 0;
        for (int i = 0; i < 6 && typing.text_typing(); ++i) {
            if (!typing.step()) break;
            if (typing.frame().ip != typing_ip) {
                return fail("普通打字期间派发了一条指令（引擎在打字期零派发）");
            }
            if (typing.pending_glyph_count() < last_pending) ++drew;
            last_pending = typing.pending_glyph_count();
        }
        if (drew == 0) return fail("打字期推进器没跑（「零派发」退化成「零推进」）");
        if (typing.text_typing()) return fail("普通打字没有自己走完（第 29 位还置着）");
        if (!typing.step() || typing.frame().ip != typing_ip + 4) {
            return fail("打字结束后门没有重新打开（派发没有恢复）");
        }
        // (b) 演示闸：`0x300` 开闸（槽 9）后追加 ⇒ 槽在用 ⇒ 每迭代派发一条。
        auto demo_script = make_script_with_body();
        emit_instruction(demo_script, 0x300, {{0, 9}, {0, 1}, {0, 50}});  // 闸开
        text_layer_setup(demo_script);
        emit_instruction(demo_script, 0x74, {{0, 5}});
        emit_instruction(demo_script, 0x6E, {{0, 0}, {5, 0}});
        emit_instruction(demo_script, 0x1A8, {});
        emit_instruction(demo_script, 0x1A8, {});
        emit_instruction(demo_script, 2, {});
        core::Machine demo(0x12345678u, table);
        demo.write_text(core::Operand{5, 0}, "AB");
        demo.load_root(demo_script);
        for (int i = 0; i < 9; ++i) {  // 0x300 + 夹具 6 条 + 0x74 + 0x6e
            if (!demo.step()) return fail("演示闸夹具：走到追加之后停机了");
        }
        if (!demo.text_slot_in_use()) return fail("0x300 开闸之后没有置「有槽在用」标志");
        if (!demo.text_typing()) {
            return fail("演示闸层追加之后第 29 位没有置起（推进器根本不会被调用）");
        }
        if (demo.pending_glyph_count() != 0) {
            return fail("闸开着的层还在走普通打字队列（同一段文本有两个推进者）");
        }
        std::size_t demo_ip = demo.frame().ip;
        for (int i = 0; i < 2; ++i) {
            if (!demo.step()) return fail("演示闸期间停机了");
            if (demo.frame().ip != demo_ip + 4) {
                return fail("演示闸开着时没有每迭代派发一条（门停在 gate 上了）");
            }
            demo_ip = demo.frame().ip;
        }
    }
    return true;}// ── 两套字号：`0x75` = A 支（`0x204` 用）、`0x197` = B 支（记录绘制用） ──
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
bool check_font_pairs_1a5_2fe(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "font-pairs: %s\n", what);
        return false;
    };
    // 两对描述、每对两份变体（`60-ops-forward` 的「两条字符串入口不许合并」）：
    //   0x1a5 写 **A 对**（名原样 / '@'+名、高 ← −A 字号、下一格 ← −(A 字号/2)）；
    //   0x2fe 写 **B 对**（同形，用 B 字号）。两支互不覆盖。
    // ★ 先 `0x2db(1)` 把"对位模式"那格置 1 —— 正文把这一格定性为字体对象的对位模式
    //   （`CORE-OPS-FORWARD-PROLOGUE`；初值 0，不是实现侧假设）。
    //   ⇒ 这里**只断言 spec 明写的部分**：两份描述的高 ← −字号（无条件）、以及随后 `0x1a5` 按当前
    //   A 字号把高 / 下一格重算成 −20 / −10。「非 0 支的下一格来自度量」另有专条与用例守护，
    //   本用例不重复断它（⛔ 不把"非 0 支不写下一格"当成合同）。
    // ★ `font_rebuild_requests()` 是**实现侧可观察量**（spec 只说"随后重建字体"，没规定几次请求）；
    //   这里的 +1/+2/+3 只防"请求彻底消失"，⛔ 不构成 spec 不变量。
    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x2DB, {{0, 1}});      // 对位模式 = 1（`Font+0x313D0`）
    emit_instruction(bytes, 0x75, {{0, 20}});      // A 支字号 20
    emit_instruction(bytes, 0x197, {{0, 10}});     // B 支字号 10
    emit_instruction(bytes, 0x1A5, {{5, 0}});      // A 对 ← "MSG"（按文本读）
    emit_instruction(bytes, 0x2FE, {{5, 1}});      // B 对 ← "BIZ"（按文本读）
    emit_instruction(bytes, 2, {});
    core::Machine machine(key, table);
    machine.write_text(core::Operand{5, 0}, "MSG");
    machine.write_text(core::Operand{5, 1}, "BIZ");
    machine.load_root(bytes);
    if (!machine.step()) return fail("0x2db 停机");
    if (machine.font_align_mode() != 1) return fail("0x2db 没有写对位模式");
    const auto rebuilds_start = machine.font_rebuild_requests();
    if (!machine.step()) return fail("0x75 停机");
    if (machine.font_rebuild_requests() != rebuilds_start + 1) return fail("0x75 没有请求重建字体");
    if (machine.font_description(false, 0).height != -20 ||
        machine.font_description(false, 1).height != -20) {
        return fail("0x75 没有把 A 对两份描述的高写成 −字号");
    }
    // ★ 这里**不**断言 `0x75` 非 0 支对"下一格"做了什么：开关身份与那支的公式 spec 都没成篇
    //   （见函数头上面的注释）⇒ 断言方向一旦反了会拒绝一个正确实现。
    if (!machine.step()) return fail("0x197 停机");
    if (machine.font_rebuild_requests() != rebuilds_start + 2) return fail("0x197 没有请求重建字体");
    if (machine.font_description(true, 0).height != -10 ||
        machine.font_description(true, 1).height != -10 ||
        machine.font_description(true, 0).width != -5 ||
        machine.font_description(true, 1).width != -5) {
        return fail("0x197 没有把 B 对两份描述的高 / 下一格写成 −字号 / −(字号/2)");
    }
    // ── 0x1a5：只写 A 对 ──
    if (!machine.step()) return fail("0x1a5 停机");
    if (machine.font_rebuild_requests() != rebuilds_start + 3) return fail("0x1a5 没有请求重建字体");
    if (machine.font_description(false, 0).face != "MSG") return fail("0x1a5 描述一的名字不是操作数原样");
    if (machine.font_description(false, 1).face != "@MSG") return fail("0x1a5 描述二的名字不是 '@' + 操作数");
    for (std::size_t variant = 0; variant < 2; ++variant) {
        if (machine.font_description(false, variant).height != -20) {
            return fail("0x1a5 没有按 A 支字号重写两份描述的高");
        }
        if (machine.font_description(false, variant).width != -10) {
            return fail("0x1a5 没有按 A 支字号重写两份描述的下一格（−(20/2)）");
        }
    }
    if (machine.font_description(true, 0).face != "" || machine.font_description(true, 1).face != "") {
        return fail("0x1a5 改了 B 对的名字（两条字符串入口被合并了）");
    }
    if (machine.glyph_height() != 20) return fail("0x1a5 改了 A 支字号（改名字不该改字号）");
    // ── 0x2fe：只写 B 对 ──
    if (!machine.step()) return fail("0x2fe 停机");
    if (machine.font_rebuild_requests() != rebuilds_start + 4) return fail("0x2fe 没有请求重建字体");
    if (machine.font_description(true, 0).face != "BIZ") return fail("0x2fe 描述一的名字不是操作数原样");
    if (machine.font_description(true, 1).face != "@BIZ") return fail("0x2fe 描述二的名字不是 '@' + 操作数");
    for (std::size_t variant = 0; variant < 2; ++variant) {
        if (machine.font_description(true, variant).height != -10) {
            return fail("0x2fe 没有按 B 支字号重写两份描述的高");
        }
        if (machine.font_description(true, variant).width != -5) {
            return fail("0x2fe 没有按 B 支字号重写两份描述的下一格（−(10/2)）");
        }
    }
    if (machine.font_description(false, 0).face != "MSG" ||
        machine.font_description(false, 1).face != "@MSG") {
        return fail("0x2fe 改了 A 对的名字（两条字符串入口被合并了）");
    }
    if (machine.font_b_height() != 10) return fail("0x2fe 改了 B 支字号（改名字不该改字号）");
    return true;
}

bool check_font_scale_build(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "font-scale: %s\n", what);
        return false;
    };
    // `60-ops-forward`「字号是"未缩放"的量」：比例 = 视口 ÷ 配置客户区；只在窗口态 ∧ 虚拟全屏类型 = 2；
    // 高分量乘进建字体高度、宽分量只抄十槽两格；两个分量都相等 ⇒ 整趟什么都不写。
    // 视口 {480,300}、客户区 {320,240} ⇒ 宽 1.5 / 高 1.25（两个分量不同，抄错格会当场露馅）。
    // ★ 本用例里的 `font_build_height()` / `font_rebuild_requests()` / `font_scale_write_count()` /
    //   `text_slot_scale_*()` 都是**实现侧可观察量**，不是 spec 不变量：spec 只规定算式、抄格与
    //   "两个分量都相等 ⇒ 整趟不写"，没规定重建请求几次，也没让产品绘制读这些量（建字体缩放本模型
    //   未接真建字体 —— `text_gdi.cpp` 仍用未缩放 `lfHeight`，见子单）。它们只用来让"算错 / 漏抄 /
    //   短路失效"可证伪，⛔ 不要把它们当成合同。
    // ★ `reapply_font_settings()` 是"重算的第 ② 个时机"的**接缝**：src 里没有产品调用者（窗口 /
    //   显示模式变化那条路径未建模）⇒ 本用例直接调它，别读成"产品路径已被覆盖"。
    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x75, {{0, 40}});
    emit_instruction(bytes, 0x197, {{0, 16}});
    emit_instruction(bytes, 2, {});
    core::Machine machine(key, table);
    machine.config_mut().set_int("display:ScreenMode", 1);            // 窗口态
    machine.config_mut().set_int("display:VirtualFullScreen", 1);
    machine.config_mut().set_int("display:VirtualFullScreenType", 2);  // 条件 2
    machine.config_mut().set_int("set:WinX", 320);
    machine.config_mut().set_int("set:WinY", 240);
    machine.set_viewport_source([] { return core::Machine::Viewport{480, 300}; });
    machine.load_root(bytes);
    const auto writes_start = machine.font_scale_write_count();
    const auto rebuilds_start = machine.font_rebuild_requests();
    machine.reapply_font_settings();  // 重算的第 ② 个时机
    if (machine.font_scale_width() != 1.5) {
        std::fprintf(stderr, "宽分量 = %f（期望 480/320 = 1.5）\n", machine.font_scale_width());
        return false;
    }
    if (machine.font_scale_height() != 1.25) {
        std::fprintf(stderr, "高分量 = %f（期望 300/240 = 1.25）\n", machine.font_scale_height());
        return false;
    }
    if (machine.font_scale_write_count() != writes_start + 1) return fail("重算没有记一次写");
    if (machine.font_rebuild_requests() != rebuilds_start + 2) {
        return fail("重算后没有为 A / B 两支各请求一次重建字体");
    }
    for (std::size_t i = 0; i < core::Machine::text_slot_count(); ++i) {
        if (machine.text_slot_scale_width(i) != 1.5 || machine.text_slot_scale_height(i) != 1.25) {
            std::fprintf(stderr, "槽 %zu 的两格 = (%f, %f)（期望 (1.5, 1.25)）\n", i,
                         machine.text_slot_scale_width(i), machine.text_slot_scale_height(i));
            return false;
        }
    }
    if (!machine.step()) return fail("0x75 停机");
    // 建字体高度 = trunc(−字号 × 比例 + 0.5)（向零截断）：−40 × 1.25 + 0.5 = −49.5 ⇒ −49。
    if (machine.font_build_height(false) != -49) {
        std::fprintf(stderr, "A 支建字体高度 = %d（期望 trunc(-40*1.25+0.5) = -49）\n",
                     machine.font_build_height(false));
        return false;
    }
    if (!machine.step()) return fail("0x197 停机");
    // B 支：−16 × 1.25 + 0.5 = −19.5 ⇒ −19。
    if (machine.font_build_height(true) != -19) {
        std::fprintf(stderr, "B 支建字体高度 = %d（期望 trunc(-16*1.25+0.5) = -19）\n",
                     machine.font_build_height(true));
        return false;
    }
    // ── 同值再跑一趟：两个分量都相等 ⇒ 整趟什么都不写（不重抄槽、不重建字体）──
    const auto writes_after = machine.font_scale_write_count();
    const auto rebuilds_after = machine.font_rebuild_requests();
    machine.reapply_font_settings();
    if (machine.font_scale_write_count() != writes_after) return fail("同值重算又写了（缺少相等短路）");
    if (machine.font_rebuild_requests() != rebuilds_after) return fail("同值重算又重建字体（缺少相等短路）");
    for (std::size_t i = 0; i < core::Machine::text_slot_count(); ++i) {
        if (machine.text_slot_scale_width(i) != 1.5 || machine.text_slot_scale_height(i) != 1.25) {
            return fail("同值重算改动了十槽的两格");
        }
    }
    // ── 条件不成立 ⇒ 两个分量一律 1.0（不是"沿用上次"）──
    machine.config_mut().set_int("display:ScreenMode", 0);
    machine.config_mut().set_int("display:VirtualFullScreen", 0);
    machine.reapply_font_settings();
    if (machine.font_scale_width() != 1.0 || machine.font_scale_height() != 1.0) {
        return fail("窗口态 / 虚拟全屏类型条件不成立时两个分量没有回到 1.0");
    }
    if (machine.font_scale_write_count() != writes_after + 1) return fail("条件不成立那趟没有重抄十槽两格");
    for (std::size_t i = 0; i < core::Machine::text_slot_count(); ++i) {
        if (machine.text_slot_scale_width(i) != 1.0 || machine.text_slot_scale_height(i) != 1.0) {
            return fail("条件不成立那趟没有把十槽两格抄成 1.0");
        }
    }
    // 比例记 1.0 之后的建字体高度：−40 + 0.5 ⇒ −39；−16 + 0.5 ⇒ −15。
    if (machine.font_build_height(false) != -39) return fail("比例 1.0 时 A 支建字体高度不是 −39");
    if (machine.font_build_height(true) != -15) return fail("比例 1.0 时 B 支建字体高度不是 −15");
    return true;
}

bool check_key_tables_10c(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "key-tables: %s\n", what);
        return false;
    };
    // 键位映射表的**三张表**（`60-ops-scalar` 的「键位映射表（opcode 0xfd / 0x10c）」）：
    // A = 槽 → 逻辑键（`slot_key`）、B = 逻辑键 → 系统键码（`logical_key_code`）、
    // C = 系统键码 0..255 → 位号（`key_code_bit`；−1 = 未映射）。
    // 两条指令的第 1 个操作数都是**槽号**（= 输入位号）、第 2 个是**逻辑键号**；读序先 2 再 1。
    // 初始：C 的 7 项固定（↑0 →1 ↓2 ←3 Enter4 Space5 Backspace6），A 的槽 0..6 ← 逻辑键
    // 200 / 205 / 208 / 203 / 28 / 57 / 14（表 B 换成系统键码 38 / 39 / 40 / 37 / 13 / 32 / 8）。
    {
        // `0x10c`：槽 5 ← 逻辑键 28（Enter）⇒ **只**把 Enter 的系统键码登记成 5；A 不碰、不解绑。
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x10C, {{0, 5}, {0, 28}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) {
            std::fprintf(stderr, "合法 0x10c 停机：%s\n", machine.last_stop().message.c_str());
            return false;
        }
        if (machine.key_code_bit(0x0D) != 5) {
            std::fprintf(stderr, "Enter 的位号 = %d（期望 5）\n", machine.key_code_bit(0x0D));
            return fail("0x10c 没有把逻辑键的系统键码登记成第 1 个操作数");
        }
        if (machine.slot_key(5) != 57) return fail("0x10c 动了表 A（它只写表 C）");
        // ★ 不解绑：槽 5 原来的逻辑键（Space，系统键码 0x20）仍指着槽 5。
        if (machine.key_code_bit(0x20) != 5) return fail("0x10c 抹了旧键码（那是 0xfd 的解绑）");
    }
    // `0xfd`：三步固定顺序（解绑旧 → 改绑 A → 登记 C）。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xFD, {{0, 0}, {0, 28}});  // 槽 0 ← 逻辑键 28（Enter）
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) {
            std::fprintf(stderr, "合法 0xfd 停机：%s\n", machine.last_stop().message.c_str());
            return false;
        }
        if (machine.key_code_bit(0x26) != -1) return fail("0xfd 没有解绑该槽旧逻辑键（↑）的键码");
        if (machine.key_code_bit(0x0D) != 0) return fail("0xfd 没有把新逻辑键的系统键码登记成槽号");
        if (machine.slot_key(0) != 28) return fail("0xfd 没有把表 A 该槽改绑到第 2 个操作数");
    }
    {
        // 连续两条 `0xfd` 到同一槽：第 2 条把上一条绑定的 Enter 键码抹掉。
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0xFD, {{0, 7}, {0, 28}});
        emit_instruction(bytes, 0xFD, {{0, 7}, {0, 14}});  // 逻辑键 14 = Backspace（表 B：14 → 8）
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step() || !machine.step()) return fail("两条 0xfd 夹具停机了");
        if (machine.key_code_bit(0x0D) != -1) return fail("第 2 条 0xfd 没有解绑上一条的 Enter 键码");
        if (machine.key_code_bit(0x08) != 7) return fail("第 2 条 0xfd 没有把 Backspace 登记成槽 7");
        if (machine.slot_key(7) != 14) return fail("第 2 条 0xfd 没有改绑表 A");
    }
    // 只检查第 1 个操作数：**无符号** > 31（含 −1）⇒ 显示消息型异常、三张表一格不写、长度槽已是 5。
    for (const std::int32_t slot : {32, -1}) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x10C, {{0, static_cast<std::uint32_t>(slot)}, {0, 28}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (machine.step() || !machine.halted()) {
            std::fprintf(stderr, "0x10c(slot=%d) 没有抛\n", slot);
            return false;
        }
        if (machine.last_display_code() != "key-multi-range") {
            std::fprintf(stderr, "0x10c(slot=%d) 错误码 = %s\n", slot,
                         machine.last_display_code().c_str());
            return false;
        }
        if (machine.last_display_message() != "SetKeyMultiの引数が不正です．") {
            std::fprintf(stderr, "0x10c(slot=%d) 文本 = %s\n", slot,
                         machine.last_display_message().c_str());
            return false;
        }
        if (machine.frame().length_slot != 5) return fail("0x10c 的长度槽不是在检查之前写成 5");
        if (machine.key_code_bit(0x0D) != 4 || machine.key_code_bit(0x20) != 5 ||
            machine.slot_key(5) != 57) {
            return fail("0x10c 越界支改了表（应当三张表一格都不写）");
        }
        // `0xfd` 同形（文本不同）。
        auto fd = make_script_with_body();
        emit_instruction(fd, 0xFD, {{0, static_cast<std::uint32_t>(slot)}, {0, 28}});
        emit_instruction(fd, 2, {});
        core::Machine fd_machine(key, table);
        fd_machine.load_root(fd);
        if (fd_machine.step() || !fd_machine.halted()) {
            std::fprintf(stderr, "0xfd(slot=%d) 没有抛\n", slot);
            return false;
        }
        if (fd_machine.last_display_code() != "key-setting-range") {
            std::fprintf(stderr, "0xfd(slot=%d) 错误码 = %s\n", slot,
                         fd_machine.last_display_code().c_str());
            return false;
        }
        if (fd_machine.last_display_message() != "SetKeyの引数が不正です．") {
            std::fprintf(stderr, "0xfd(slot=%d) 文本 = %s\n", slot,
                         fd_machine.last_display_message().c_str());
            return false;
        }
        if (fd_machine.frame().length_slot != 5) return fail("0xfd 的长度槽不是在检查之前写成 5");
        if (fd_machine.key_code_bit(0x26) != 0 || fd_machine.key_code_bit(0x0D) != 4 ||
            fd_machine.slot_key(0) != 200) {
            return fail("0xfd 越界支改了表（应当三张表一格都不写）");
        }
    }
    // 读序 2 → 1：把第 2 个写成"读整必抛"的 type 11、第 1 个写成 type 2 ⇒ 先炸的必须是第 2 个。
    for (const std::uint32_t opcode : {0x10Cu, 0xFDu}) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, opcode, {{2, 0}, {11, 0}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (machine.step() || machine.last_stop().message != "read-int-unsupported-tag:11") {
            std::fprintf(stderr, "0x%X 的读序不是 2 → 1（停因 = %s）\n", opcode,
                         machine.last_stop().message.c_str());
            return false;
        }
    }
    return true;
}


// ── 表 B（逻辑键 → 系统键码）的 **93 条初值**（`CORE-OPS-SCALAR-TABLE-10C` 的「表 B 的初值」）──
// 判据（每条都能红）：
// ① 七个默认槽 0..6 依次绑逻辑键 200 / 205 / 208 / 203 / 28 / 57 / 14；经表 B 换成系统键码
//    38 / 39 / 40 / 37 / 13 / 32 / 8（= 表 C 的位 0..6：上 / 右 / 下 / 左 / Enter / Space / Backspace）；
// ② **93 个固定条目逐条**都在（表驱动；写错一条、少写一条、下标错位 ⇒ 红）；
// ③ 表内其余 **163 格恒为 0**：逻辑键 0 / 13 / 255 都换出 0（"未写到" ≠ "未映射"）；
// ④ 表外（256 / −1）是**未定义读**：不抛、不夹到 255 —— 本模型用 −1 表示"没有可用的系统键码"；
// ⑤ `0x10c` / `0xfd` 用逻辑键 **200**（默认槽 0）查得到键码 38，且两条的"解绑 / 不解绑"落点不同。
// ★ 这张表是 spec 的**合同**而不是实现细节：它错了只会让"按哪个键点亮哪个位"整体错位，
//   而任何一条用例都不会因此停机 ⇒ 必须有这条逐条的守卫。
bool check_scalar_key_table_b(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "scalar-key-table-b: %s\n", what);
        return false;
    };
    // ① 默认七个槽的逻辑键 + 经表 B / 表 C 的两段落点。
    {
        constexpr std::int32_t kSlotLogical[7] = {200, 205, 208, 203, 28, 57, 14};
        constexpr std::int32_t kSlotSystem[7] = {38, 39, 40, 37, 13, 32, 8};
        core::Machine machine(key, table);
        for (std::size_t slot = 0; slot < 7; ++slot) {
            if (machine.slot_key(slot) != kSlotLogical[slot]) {
                std::fprintf(stderr, "槽 %zu 的逻辑键 = %d（期望 %d）\n", slot,
                             machine.slot_key(slot), kSlotLogical[slot]);
                return fail("表 A 的七个默认槽没有绑到 spec 的逻辑键（200/205/208/203/28/57/14）");
            }
            if (machine.logical_key_code(kSlotLogical[slot]) != kSlotSystem[slot]) {
                std::fprintf(stderr, "逻辑键 %d 的系统键码 = %d（期望 %d）\n",
                             kSlotLogical[slot], machine.logical_key_code(kSlotLogical[slot]),
                             kSlotSystem[slot]);
                return fail("表 B 的 93 条初值里默认槽那七条不对（下标错位或值写错）");
            }
            if (machine.key_code_bit(static_cast<std::size_t>(kSlotSystem[slot])) !=
                static_cast<std::int32_t>(slot)) {
                std::fprintf(stderr, "系统键码 %d 的位号 = %d（期望 %zu）\n", kSlotSystem[slot],
                             machine.key_code_bit(static_cast<std::size_t>(kSlotSystem[slot])), slot);
                return fail("表 C 的七个默认位号不是经 表 A → 表 B 两段查表得来的");
            }
        }
        std::size_t mapped = 0;
        for (std::size_t code = 0; code < core::Machine::kSystemKeyCount; ++code) {
            if (machine.key_code_bit(code) != -1) ++mapped;
        }
        if (mapped != 7) {
            std::fprintf(stderr, "表 C 映射了 %zu 项（期望 7）\n", mapped);
            return fail("表 C 的初值不是只有七个默认项（表 B 的 93 条不该把它们全登记成位号）");
        }
    }
    // ② 93 个固定条目逐条（★ 与 spec 的表 B 清单**逐字**对应；表内其余格必须是 0）。
    {
        struct Pair {
            std::int32_t logical_key;
            std::int32_t system_code;
        };
        constexpr Pair kExpected[] = {
            {1, 27},   {2, 49},   {3, 50},   {4, 51},   {5, 52},   {6, 53},
            {7, 54},   {8, 55},   {9, 56},   {10, 57},  {11, 48},  {12, 109},
            {14, 8},   {15, 9},   {16, 81},  {17, 87},  {18, 69},  {19, 82},
            {20, 84},  {21, 89},  {22, 85},  {23, 73},  {24, 79},  {25, 80},
            {28, 13},  {29, 17},  {30, 65},  {31, 83},  {32, 68},  {33, 70},
            {34, 71},  {35, 72},  {36, 74},  {37, 75},  {38, 76},  {42, 16},
            {44, 90},  {45, 88},  {46, 67},  {47, 86},  {48, 66},  {49, 78},
            {50, 77},  {54, 16},  {56, 18},  {57, 32},  {59, 112}, {60, 113},
            {61, 114}, {62, 115}, {63, 116}, {64, 117}, {65, 118}, {66, 119},
            {67, 120}, {68, 121}, {69, 144}, {70, 145}, {71, 103}, {72, 104},
            {73, 105}, {74, 109}, {75, 100}, {76, 101}, {77, 102}, {78, 107},
            {79, 97},  {80, 98},  {81, 99},  {82, 96},  {83, 110}, {87, 122},
            {88, 123}, {112, 21}, {121, 28}, {123, 29}, {148, 25}, {156, 13},
            {157, 17}, {181, 111},{184, 18}, {199, 18}, {200, 38}, {201, 33},
            {203, 37}, {205, 39}, {207, 35}, {208, 40}, {209, 34}, {210, 45},
            {219, 91}, {220, 92}, {221, 93},
        };
        static_assert(sizeof(kExpected) / sizeof(kExpected[0]) == 93, "表 B 必须是 93 条");
        core::Machine machine(key, table);
        for (const auto& pair : kExpected) {
            const auto got = machine.logical_key_code(pair.logical_key);
            if (got != pair.system_code) {
                std::fprintf(stderr, "逻辑键 %d → %d（期望 %d）\n", pair.logical_key, got,
                             pair.system_code);
                return fail("表 B 的 93 条初值有一条不对（逐条对照 spec 的表 B）");
            }
        }
        // ③ 未列出的 163 格恒为 0（⇒ 换出系统键码 0，与"逻辑键 0"同效）。
        std::size_t nonzero = 0;
        for (std::int32_t logical = 0; logical < 256; ++logical) {
            if (machine.logical_key_code(logical) != 0) ++nonzero;
        }
        if (nonzero != 93) {
            std::fprintf(stderr, "表 B 里非 0 的格 = %zu（期望 93）\n", nonzero);
            return fail("表 B 不是「整表清 0 + 93 条」的形状（多写了 / 少写了 / 表外被补 0）");
        }
        for (const std::int32_t logical : {0, 13, 26, 255}) {
            if (machine.logical_key_code(logical) != 0) {
                std::fprintf(stderr, "逻辑键 %d → %d（期望 0）\n", logical,
                             machine.logical_key_code(logical));
                return fail("表内未写到的格不是 0（把\"未写到\"当成了\"未映射\"）");
            }
        }
        // ④ 表外（<0 / ≥256）是未定义读：不抛、不夹到 255。
        for (const std::int32_t logical : {256, 99999, -1}) {
            if (machine.logical_key_code(logical) != -1) {
                std::fprintf(stderr, "表外逻辑键 %d → %d（期望 −1 = 没有可用的系统键码）\n", logical,
                             machine.logical_key_code(logical));
                return fail("表外越界读被夹到表内（spec 明文：不抛、不夹到 255、不补 0）");
            }
        }
    }
    // ⑤ 两条脚本指令用逻辑键 200 查表（默认槽 0 = ↑ ⇒ 38）。
    {
        // `0x10c(槽 3, 逻辑键 200)`：只登记表 C ⇒ 38 的位号变成 3；表 A 不动（槽 3 仍是 203）。
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x10C, {{0, 3}, {0, 200}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) {
            std::fprintf(stderr, "0x10c(逻辑键 200) 停机：%s\n", machine.last_stop().message.c_str());
            return fail("0x10c 用逻辑键 200 查表失败（表 B 没有这一条？）");
        }
        if (machine.key_code_bit(38) != 3) {
            std::fprintf(stderr, "0x10c 之后 38 的位号 = %d（期望 3）\n", machine.key_code_bit(38));
            return fail("0x10c 没有把逻辑键 200 的系统键码（38）登记成槽号");
        }
        if (machine.slot_key(3) != 203) return fail("0x10c 动了表 A（它只写表 C）");
        // `0xfd(槽 0, 逻辑键 205)`：先解绑旧逻辑键 200 的键码（38 ← −1），再登记 205 的键码 39 ← 0。
        auto fd = make_script_with_body();
        emit_instruction(fd, 0xFD, {{0, 0}, {0, 205}});
        emit_instruction(fd, 2, {});
        core::Machine fd_machine(key, table);
        fd_machine.load_root(fd);
        if (!fd_machine.step()) {
            std::fprintf(stderr, "0xfd(逻辑键 205) 停机：%s\n", fd_machine.last_stop().message.c_str());
            return fail("0xfd 用逻辑键 205 查表失败（表 B 没有这一条？）");
        }
        if (fd_machine.key_code_bit(38) != -1) return fail("0xfd 没有解绑旧逻辑键 200 的键码（38）");
        if (fd_machine.key_code_bit(39) != 0) return fail("0xfd 没有把新逻辑键 205 的键码（39）登记成槽 0");
        if (fd_machine.slot_key(0) != 205) return fail("0xfd 没有把表 A 该槽改绑到第 2 个操作数");
    }
    return true;
}

// ── 派发（`CORE-OPS-TABLE-DISPATCH-A3`）：命中 ⇒ 用登记值；未命中 ⇒ 用默认位置 ──
// 判据（A1–A6，`RES-DISPATCH-DEFAULT-READ` 的六条；每一条都能红）：
// 装置（A1/A2 共用**同一段字节布局**，只差 `0xa2` 绑的是哪个键）：
//   字 0..4   `0x55` 全局整型 1 ← 75   （键格 ⇒ 文本 "75"）
//   字 5..9   `0x55` 全局整型 0 ← −1   （**落点格预置 −1** ⇒ "没写"/"写了 0"/"写了 7" 三态可分）
//   字 10..14 `0xa2` <键> ← 20         （A1: "75" → 20；A2: 别的键 → 20 ⇒ "75" 未绑定）
//   字 15..19 `0xa3` "75", 默认 26
//   字 20..24 `0x55` 落点 ← 7          （命中目标 = 登记值 20）
//   字 25     `0x02` halt
//   字 26..30 `0x55` 落点 ← 9          （默认目标 = 默认位置 26）
//   字 31     `0x02` halt
// A1 命中 ⇒ 落点 7（不是 9、不是 −1）；A2 未命中 ⇒ 落点 9；A3 命中 + 第 2 个操作数类型非法 ⇒
// **不报错**且落点 7（"判据先于读"）；A4 A3 去掉绑定 ⇒ **必须**抛类型异常（A3 的绿灯不是因为不校验读）；
// A5 登记值 0 ⇒ 仍然命中（跳到代码区第 0 格，⛔ 不是"缺键"）；A6 同一条按格重入 > 1 次即红（预算 8 步内必须 halt）。
bool check_dispatch_default_read(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "dispatch-default-read: %s\n", what);
        return false;
    };
    // 同一段布局：`bind_key` 是 `0xa2` 第 1 个操作数（3,1 = 键格 "75"；3,2 = 另一个键 ⇒ "75" 未绑定），
    // `bind_value` 是登记值，`default_operand` 是 `0xa3` 第 2 个操作数（类型 + 原始值）。
    // ★ 逐条指令**手写**在这里（不按"偏移算术"拼）：哪条指令落在哪个字必须一眼可见 ——
    //   目标字（20 / 26）与指令起点错位一个字节，本条就红。
    const auto build = [&](std::uint32_t bind_key, std::uint32_t bind_value,
                           std::uint32_t default_type, std::uint32_t default_raw) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x55, {{3, 1}, {0, 75}});           // 字 0..4
        emit_instruction(bytes, 0x55, {{3, 0}, {0, 0xFFFFFFFFu}});  // 字 5..9
        emit_instruction(bytes, 0xA2, {{3, bind_key}, {0, bind_value}});      // 字 10..14
        emit_instruction(bytes, 0xA3, {{3, 1}, {default_type, default_raw}});  // 字 15..19
        emit_instruction(bytes, 0x55, {{3, 0}, {0, 7}});            // 字 20..24：命中目标
        emit_instruction(bytes, 0x02, {});                          // 字 25：halt
        emit_instruction(bytes, 0x55, {{3, 0}, {0, 9}});            // 字 26..30：默认目标
        emit_instruction(bytes, 0x02, {});                          // 字 31：halt
        return bytes;
    };
    // A1 + A6：命中 ⇒ 落点 7；未命中 ⇒ 落点 9；A1 的同一装置同时按"预算 + 同格重入"跑一遍。
    {
        // A1
        core::Machine machine(key, table);
        machine.load_root(build(1, 20, 0, 26));
        for (int i = 0; i < 4; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "A1 第 %d 步停机：%s\n", i + 1,
                             machine.last_stop().message.c_str());
                return fail("A1 的夹具没跑到 0xa3（布局错位）");
            }
        }
        if (machine.frame().ip != 20u * 4u || machine.frame().length_slot != 0) {
            std::fprintf(stderr, "A1 派发后 ip=%zu length=%u（期望 80 / 0）\n",
                         machine.frame().ip, machine.frame().length_slot);
            return fail("命中没有把指令指针写到「代码区起点 + 4 × 登记值」");
        }
        if (machine.global_int(0) != -1) return fail("A1 的落点格不是预置的 −1（三态不可分）");
        if (!machine.step()) {
            std::fprintf(stderr, "A1 命中目标停机：%s\n", machine.last_stop().message.c_str());
            return fail("A1 的命中目标不是一个可执行指令（登记值不是指令起点）");
        }
        if (machine.global_int(0) != 7) {
            std::fprintf(stderr, "A1 落点 = %d（期望 7）\n", machine.global_int(0));
            return fail("命中没有用登记值（落到默认位置了？）");
        }
        // A6 —— **指令预算**：同一条按格重入 > 1 次即红。
        // ★ 引擎没有"预算"机制：预算是**本用例**的上界（`run(max_steps)` 同形）。判据是
        //   「命中支必须真的写指令指针」—— 少写它 ⇒ 推进格数 0 ⇒ 下一次仍派发同一个 `0xa3` 格
        //   ⇒ 预算耗尽而红。这也是上面 A1 之后必须**继续**跑完整个程序的原因。
        core::Machine budget(key, table);
        budget.load_root(build(1, 20, 0, 26));
        std::vector<std::size_t> visited;
        bool running = true;
        while (running && visited.size() < 8) {
            running = budget.step();
            visited.push_back(budget.last_stop().offset / 4);
        }
        const std::vector<std::size_t> expected = {0, 5, 10, 15, 20, 25};
        if (visited != expected || !budget.halted()) {
            std::fprintf(stderr, "A6 执行足迹 =");
            for (const auto cell : visited) std::fprintf(stderr, " %zu", cell);
            std::fprintf(stderr, "（期望 0 5 10 15 20 25 且已 halt）\n");
            return fail("同一条按格重入（命中支没有写指令指针 ⇒ 推进格数 0 ⇒ 原地打转）");
        }
    }
    // A2：未命中 ⇒ 默认位置（同一装置，只把 `0xa2` 绑到别的键）。
    {
        core::Machine machine(key, table);
        machine.load_root(build(2, 20, 0, 26));
        for (int i = 0; i < 4; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "A2 第 %d 步停机：%s\n", i + 1,
                             machine.last_stop().message.c_str());
                return fail("A2 的夹具没跑到 0xa3");
            }
        }
        if (machine.frame().ip != 26u * 4u || machine.frame().length_slot != 0) {
            std::fprintf(stderr, "A2 派发后 ip=%zu length=%u（期望 104 / 0）\n",
                         machine.frame().ip, machine.frame().length_slot);
            return fail("未命中没有把指令指针写到「代码区起点 + 4 × 默认位置」");
        }
        if (machine.global_int(0) != -1) return fail("A2 的落点格不是预置的 −1");
        if (!machine.step()) return fail("A2 的默认目标不可执行（默认位置不是指令起点）");
        if (machine.global_int(0) != 9) {
            std::fprintf(stderr, "A2 落点 = %d（期望 9）\n", machine.global_int(0));
            return fail("未命中没有走默认位置（落点不是默认目标的标记）");
        }
    }
    // A3：命中 + 第 2 个操作数类型非法（type 11 = 读整必抛）⇒ **不读它** ⇒ 不报错、落点 7。
    {
        core::Machine machine(key, table);
        machine.load_root(build(1, 20, 11, 0));
        for (int i = 0; i < 4; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "A3 第 %d 步停机：%s\n", i + 1,
                             machine.last_stop().message.c_str());
                return fail("命中时读了第 2 个操作数（\"判据先于读\"不成立）");
            }
        }
        if (machine.frame().ip != 20u * 4u) return fail("A3 命中后没有落到登记值的目标");
        if (!machine.step()) return fail("A3 的命中目标不可执行");
        if (machine.global_int(0) != 7) return fail("A3 的落点不是 7（命中支没有照常执行登记目标）");
        if (!machine.last_display_code().empty()) return fail("A3 报了显示消息型异常（只检查第 1 个操作数）");
    }
    // A4：A3 但去掉绑定 ⇒ **必须**报操作数类型异常（证明 A3 的绿灯不是"引擎根本不读默认位置"）。
    {
        core::Machine machine(key, table);
        machine.load_root(build(2, 20, 11, 0));
        for (int i = 0; i < 3; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "A4 前置第 %d 步停机：%s\n", i + 1,
                             machine.last_stop().message.c_str());
                return fail("A4 的夹具没跑到 0xa3");
            }
        }
        if (machine.step() || !machine.halted()) {
            std::fprintf(stderr, "A4 停因 = %s\n", machine.last_stop().message.c_str());
            return fail("未命中 + 默认位置类型非法却没有报类型异常（默认位置根本没被读）");
        }
        if (machine.last_stop().message != "read-int-unsupported-tag:11") {
            std::fprintf(stderr, "A4 停因 = %s\n", machine.last_stop().message.c_str());
            return fail("未命中支的失败原因不是「读默认位置失败」");
        }
        if (machine.global_int(0) != -1) return fail("A4 抛异常前就写了落点（次序不对）");
    }
    // A5：登记值 0 ⇒ **仍然命中**（跳过代码区第 0 格），⛔ 不是"缺键 / 值非 0 才算命中"。
    {
        core::Machine machine(key, table);
        machine.load_root(build(1, 0, 0, 26));
        for (int i = 0; i < 4; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "A5 第 %d 步停机：%s\n", i + 1,
                             machine.last_stop().message.c_str());
                return fail("A5 的夹具没跑到 0xa3");
            }
        }
        const auto bound = machine.named_table().find("75");
        if (bound == machine.named_table().end() || bound->second != 0) {
            return fail("A5 的绑定表里没有「75 → 0」（夹具没绑上）");
        }
        if (machine.frame().ip != 0 || machine.frame().length_slot != 0) {
            std::fprintf(stderr, "A5 派发后 ip=%zu（期望 0 = 代码区第 0 格）\n", machine.frame().ip);
            return fail("登记值 0 被当成「缺键」（命中判据被写成了「值非 0」）");
        }
        if (machine.global_int(0) != -1) return fail("A5 走了默认目标（落点已不是 −1）");
    }
    return true;
}


// ── 位号越界：0x135 / 0x136（`CORE-OPS-BIT-SET-RESET`）──
// 锚：`0x135` = 0x42F8C1（长度槽 5）· 0x42F8DA `cmp edi,1Fh` + `ja` · 0x42F8E0 起为错误文本支；
//     `0x136` = 0x42F931 / 0x42F94A / 0x42F950。越界支**不读、不写目标**，只把固定文本（含 CRLF）
//     写进**错误通道**，然后**正常返回**（不抛、不停机）。
// ★ 判据只咬**目标操作数**：推进格数槽在判据之前就被写（0x42F8C1 / 0x42F931，反例池），
//   ⛔ 不许把"状态不变"扩大到它。
bool check_bit_op_arg_invalid(const core::OpcodeTable& table, std::uint32_t opcode, const char* text,
                              std::int32_t in_range_target, std::int32_t in_range_expected) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "bit-op-arg-invalid: %s\n", what);
        return false;
    };
    // 位号 32 与 −1 都必须越界（**无符号**比较）；目标预置 −1 ⇒ 写 0 / 写 ~0 都会露馅。
    // ★ 错误通道的合同是**一次平台写请求（文本 + 长度）**，不是"诊断缓冲里多一条"（`60-ops-numeric.md:237`）
    //   ⇒ 每个用例都装 writer 接缝直接咬那次请求；`errors_` 只作补充。只断言 `errors_` 的话，
    //   把 `report_error` 的 writer 分支整个删掉仍然绿（假守卫）。
    for (const std::int32_t bit : {32, -1}) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, opcode, {{9, 0}, {0, static_cast<std::uint32_t>(bit)}});
        emit_instruction(bytes, 0x55, {{9, 1}, {0, 7}});  // 越界支必须正常返回：下一条要执行
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        std::size_t requests = 0;
        std::string requested_text;
        std::size_t requested_length = 0;
        machine.set_error_output_writer([&](const std::string& emitted, std::size_t length) {
            ++requests;
            requested_text = emitted;
            requested_length = length;
        });
        machine.set_error_output_handle_valid(true);
        machine.load_root(bytes);
        machine.write_int(core::Operand{9, 0}, -1);
        if (!machine.step() || machine.halted()) {
            std::fprintf(stderr, "0x%X(bit=%d) 越界支把机器停了：%s\n", opcode, bit,
                         machine.last_stop().message.c_str());
            return false;
        }
        if (machine.read_int(core::Operand{9, 0}) != -1) {
            std::fprintf(stderr, "0x%X(bit=%d) 写了目标操作数（=%d，期望仍 −1）\n", opcode, bit,
                         machine.read_int(core::Operand{9, 0}));
            return false;
        }
        if (machine.errors().size() != 1) {
            std::fprintf(stderr, "0x%X(bit=%d) 错误通道条数 = %zu（期望恰 1）\n", opcode, bit,
                         machine.errors().size());
            return false;
        }
        if (machine.errors().front() != text) {
            std::fprintf(stderr, "0x%X(bit=%d) 错误文本不是固定文本：%s\n", opcode, bit,
                         machine.errors().front().c_str());
            return false;
        }
        // ★ 平台写请求：恰一次，文本与长度逐字（长度 = 字节数）。
        if (requests != 1) {
            std::fprintf(stderr, "0x%X(bit=%d) 平台写请求 = %zu 条（期望恰 1）\n", opcode, bit, requests);
            return false;
        }
        if (requested_text != text || requested_length != requested_text.size()) {
            std::fprintf(stderr, "0x%X(bit=%d) 请求文本 / 长度不对（len=%zu）\n", opcode, bit,
                         requested_length);
            return false;
        }
        if (!machine.step() || machine.local_int(1) != 7) {
            return fail("越界支之后的指令没有执行（不是正常返回）");
        }
    }
    // 对照：位号 31 在界内 ⇒ **必须真的动作**（防"把越界写成恒不写"），且**不发**平台写请求。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, opcode, {{9, 0}, {0, 31}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        std::size_t requests = 0;
        machine.set_error_output_writer([&](const std::string&, std::size_t) { ++requests; });
        machine.set_error_output_handle_valid(true);
        machine.load_root(bytes);
        machine.write_int(core::Operand{9, 0}, in_range_target);
        if (!machine.step() || machine.read_int(core::Operand{9, 0}) != in_range_expected) {
            std::fprintf(stderr, "0x%X(bit=31) 目标 = %d（期望 %d）\n", opcode,
                         machine.read_int(core::Operand{9, 0}), in_range_expected);
            return false;
        }
        if (!machine.errors().empty()) return fail("位号 31 在界内却记了错误");
        if (requests != 0) return fail("位号 31 在界内却发了平台写请求");
    }
    return true;
}
bool check_rembit_arg_invalid(const core::OpcodeTable& table) {
    return check_bit_op_arg_invalid(table, 0x136, "RemBitの引数が不正です．\r\n", -1,
                                    static_cast<std::int32_t>(0x7FFFFFFFu));
}
bool check_setbit_arg_invalid(const core::OpcodeTable& table) {
    return check_bit_op_arg_invalid(table, 0x135, "SetBitの引数が不正です．\r\n", 0,
                                    static_cast<std::int32_t>(0x80000000u));
}
// ── 0x13F：位号越界（`CORE-OPS-BIT-TEST`）──
// 与 0x135 / 0x136 同形，但**源与落点是两个操作数**：越界 ⇒ 第 1 个保持原值、源**不被读**、
// 错误文本 `GetBitの引数が不正です．` + CRLF 进错误通道，正常返回（不抛、不停机）。
// ★ 判据里把"源"故意写成**不支持读整**的类型（type 2 = 代码区里的文本立即数）：
//   越界支只要读一次源就会抛 ⇒ 机器当场停。这样"越界时源也不读"才是**会红**的断言。
bool check_getbit_arg_invalid(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const std::string kText = "GetBitの引数が不正です．\r\n";
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "getbit-arg-invalid: %s\n", what);
        return false;
    };
    // ★ 与 0x135 / 0x136 一样：错误通道 = **一次平台写请求（文本 + 长度）** ⇒ 咬 writer 接缝，
    //   `errors_` 只作补充（只断言诊断缓冲抓不住"writer 分支被删掉"）。
    for (const std::int32_t bit : {32, -1}) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x13F, {{9, 0}, {2, 0}, {0, static_cast<std::uint32_t>(bit)}});
        emit_instruction(bytes, 0x55, {{9, 1}, {0, 7}});  // 越界支必须正常返回：下一条要执行
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        std::size_t requests = 0;
        std::string requested_text;
        std::size_t requested_length = 0;
        machine.set_error_output_writer([&](const std::string& emitted, std::size_t length) {
            ++requests;
            requested_text = emitted;
            requested_length = length;
        });
        machine.set_error_output_handle_valid(true);
        machine.load_root(bytes);
        machine.write_int(core::Operand{9, 0}, -1);       // 目标预置 −1：写 0 也会露馅
        if (!machine.step() || machine.halted()) {
            std::fprintf(stderr, "0x13F(bit=%d) 越界支把机器停了（读了源 / 抛了异常）：%s\n", bit,
                         machine.last_stop().message.c_str());
            return false;
        }
        if (machine.read_int(core::Operand{9, 0}) != -1) {
            std::fprintf(stderr, "0x13F(bit=%d) 写了目标操作数（=%d，期望仍 −1）\n", bit,
                         machine.read_int(core::Operand{9, 0}));
            return false;
        }
        if (machine.errors().size() != 1 || machine.errors().front() != kText) {
            std::fprintf(stderr, "0x13F(bit=%d) 错误通道 = %zu 条（期望恰 1 条固定文本）\n", bit,
                         machine.errors().size());
            return false;
        }
        if (requests != 1) {
            std::fprintf(stderr, "0x13F(bit=%d) 平台写请求 = %zu 条（期望恰 1）\n", bit, requests);
            return false;
        }
        if (requested_text != kText || requested_length != kText.size()) {
            std::fprintf(stderr, "0x13F(bit=%d) 请求文本 / 长度不对（len=%zu）\n", bit, requested_length);
            return false;
        }
        if (!machine.step() || machine.local_int(1) != 7) return fail("越界支之后的指令没有执行");
    }
    // 对照 1：位号 30 在界内 ⇒ 源的第 30 位（源 = 0x40000000）读成 1；结果**归一**成 1。
    // 对照 2：位号 0 ⇒ 0（不是"原值"、也不是掩码后的整字）。界内对照**不发**平台写请求。
    for (const auto& [bit, value, expected] :
         {std::tuple<std::int32_t, std::int32_t, std::int32_t>{30, 0x40000000, 1},
          std::tuple<std::int32_t, std::int32_t, std::int32_t>{0, 0x40000000, 0}}) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x13F, {{9, 0}, {9, 2}, {0, static_cast<std::uint32_t>(bit)}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        std::size_t requests = 0;
        machine.set_error_output_writer([&](const std::string&, std::size_t) { ++requests; });
        machine.set_error_output_handle_valid(true);
        machine.load_root(bytes);
        machine.write_int(core::Operand{9, 2}, value);
        if (!machine.step()) {
            std::fprintf(stderr, "0x13F(bit=%d) 合法位号把机器停了：%s\n", bit,
                         machine.last_stop().message.c_str());
            return false;
        }
        if (machine.local_int(0) != expected) {
            std::fprintf(stderr, "0x13F(bit=%d, src=%d) 落点 = %d（期望 %d）\n", bit, value,
                         machine.local_int(0), expected);
            return false;
        }
        if (!machine.errors().empty()) return fail("合法位号却记了错误");
        if (requests != 0) return fail("合法位号却发了平台写请求");
    }
    return true;
}
// ── 0x13F 越界**不写落点**（`CORE-OPS-BIT-TEST`）──
// 与 `check_getbit_arg_invalid` 分开一条：那条咬「错误通道 + 源不被读」，这条只咬**落点写不写**，
// 并且用**负控制**证明"落点还停在 −1"不是"这个格根本观察不到写入"。
// ① 位号 32（越界）、落点预置 −1、源 = 1 ⇒ 落点必须**仍是 −1**。
//    ★ 源取 1 是有意的：写 0、以及"把位号按低 5 位掩码后再算"（32 & 31 = 0 ⇒ 1）两种错法都会露馅。
// ② 正向对照（同一格、同样预置 −1）：位号 0 / 源 1 ⇒ 落点 1；位号 0 / 源 0xFFFFFFFE ⇒ 落点 **0**
//    ⇒ 「界内写 0」与「越界不写」在同一个观测量上必须分得开。
// ③ 负控制：同一段脚本换成一条**会写落点**的指令（`0x55`）⇒ 落点变 ⇒ 观测通道看得见写入。
bool check_getbit_nowrite(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const std::string kText = "GetBitの引数が不正です．\r\n";
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "getbit-nowrite: %s\n", what);
        return false;
    };
    // ① 越界不写落点。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x13F, {{9, 0}, {9, 2}, {0, 32}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        machine.write_int(core::Operand{9, 0}, -1);  // 落点预置 −1：写 0 也会露馅
        machine.write_int(core::Operand{9, 2}, 1);   // 源的第 0 位 = 1
        if (!machine.step() || machine.halted()) return fail("位号 32 把机器停了（越界支应正常返回）");
        if (machine.read_int(core::Operand{9, 0}) != -1) {
            std::fprintf(stderr, "位号 32：落点 = %d（期望仍 −1）\n",
                         machine.read_int(core::Operand{9, 0}));
            return fail("越界时写了落点（「不写」被写成「写 0」，或按低 5 位掩码算出了 1）");
        }
        // 非空转的前提：走的确实是**越界支**（固定文本进错误通道），不是"什么都没执行"。
        if (machine.errors().size() != 1 || machine.errors().front() != kText) {
            std::fprintf(stderr, "位号 32：错误通道 = %zu 条（期望恰 1 条固定文本）\n",
                         machine.errors().size());
            return fail("越界支没有走错误通道 ⇒ 这条不是在验「越界不写」");
        }
    }
    // ② 正向对照：位号 0 ⇒ 落点真的被写（1 与 0 两种结果各一次）。
    for (const auto& [source, expected] :
         {std::pair<std::int32_t, std::int32_t>{1, 1},
          std::pair<std::int32_t, std::int32_t>{static_cast<std::int32_t>(0xFFFFFFFEu), 0}}) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x13F, {{9, 0}, {9, 2}, {0, 0}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        machine.write_int(core::Operand{9, 0}, -1);
        machine.write_int(core::Operand{9, 2}, source);
        if (!machine.step() || machine.halted()) {
            std::fprintf(stderr, "位号 0（源 = %d）把机器停了\n", source);
            return false;
        }
        if (machine.read_int(core::Operand{9, 0}) != expected) {
            std::fprintf(stderr, "位号 0（源 = %d）：落点 = %d（期望 %d）\n", source,
                         machine.read_int(core::Operand{9, 0}), expected);
            return fail("界内的位结果不对（不是归一成 0 / 1 的那一位）");
        }
        if (!machine.errors().empty()) return fail("界内却记了错误");
    }
    // ③ 负控制：同一段脚本、同一条落点，换成会写落点的指令 ⇒ 落点变（观测通道有效）。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x55, {{9, 0}, {0, 12345}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        machine.write_int(core::Operand{9, 0}, -1);
        if (!machine.step() || machine.halted()) return fail("负控制（0x55）把机器停了");
        if (machine.read_int(core::Operand{9, 0}) != 12345) {
            std::fprintf(stderr, "负控制：落点 = %d（期望 12345）\n",
                         machine.read_int(core::Operand{9, 0}));
            return fail("会写落点的指令没有改变落点 ⇒ 上面「仍是 −1」的观测不算数");
        }
    }
    return true;
}
// ── 0x60 范围 = 0（`CORE-OPS-RANDOM-60`）──
// 四步里的第 3 步：落点经**写整型**先写 0，**然后**抛**显示消息型异常**（文本逐字）——
// 抛出之前随机数已经取走一次（不回退）。⛔ 判据是"等于 0"，不是"小于等于 0"。
bool check_random_range_zero(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const std::string kText = "RANDOMの最大値が0になっています．";
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "random-range-zero: %s\n", what);
        return false;
    };
    int draws = 0;
    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x60, {{3, 1}, {0, 0}});  // 范围 = 0
    emit_instruction(bytes, 2, {});
    core::Machine machine(key, table);
    machine.set_random_source([&draws] {
        ++draws;
        return 0x40000000u;
    });
    machine.load_root(bytes);
    // ★ 落点**先预置成非 0**：全局整型池的缺槽读出来本来就是 0 ⇒ 不预置的话，"写了 0"与
    //   "根本没写"在观测上一样（`pnpm test:mutation` 实测过：不写 0 的变异抓不住）。
    machine.write_int(core::Operand{3, 1}, 5);
    if (machine.step()) return fail("范围 = 0 没有抛（step 返回 true）");
    if (machine.read_int(core::Operand{3, 1}) != 0) {
        std::fprintf(stderr, "0x60 范围 = 0 时落点 = %d（抛出之前必须已被写成 0）\n",
                     machine.read_int(core::Operand{3, 1}));
        return false;
    }
    if (draws != 1) return fail("随机数没有恰取一次（或抛出时回退了）");
    if (machine.last_display_message() != kText) {
        std::fprintf(stderr, "0x60 显示消息 = 「%s」（期望「%s」）\n",
                     machine.last_display_message().c_str(), kText.c_str());
        return false;
    }
    if (machine.last_display_code() != "random-range-zero") {
        std::fprintf(stderr, "0x60 错误码 = %s\n", machine.last_display_code().c_str());
        return false;
    }
    if (machine.last_stop().message.rfind("display-message:random-range-zero", 0) != 0) {
        std::fprintf(stderr, "0x60 停因不是显示消息型异常：%s\n", machine.last_stop().message.c_str());
        return false;
    }
    // 对照：范围 = −1 **不抛**（判据是 = 0），余数 = 随机数 mod 1 = 0；随机数仍只取一次。
    {
        int negative_draws = 0;
        auto negative = make_script_with_body();
        emit_instruction(negative, 0x60, {{3, 1}, {0, 0xFFFFFFFFu}});  // −1
        emit_instruction(negative, 2, {});
        core::Machine negative_machine(key, table);
        negative_machine.set_random_source([&negative_draws] {
            ++negative_draws;
            return 0x40000000u;
        });
        negative_machine.load_root(negative);
        if (!negative_machine.step()) {
            std::fprintf(stderr, "0x60 范围 −1 抛了：%s\n", negative_machine.last_stop().message.c_str());
            return false;
        }
        if (negative_machine.read_int(core::Operand{3, 1}) != 0 || negative_draws != 1) {
            return fail("范围 −1 的余数 / 抽取次数不对");
        }
        if (!negative_machine.last_display_message().empty()) return fail("范围 −1 却记了显示消息");
    }
    return true;
}
// ── 0x64 块越界（`CORE-OPS-ARRAY-COPY`）──
// spec 把"块编号越界 / 个数越过代码区末尾"列为**未定义行为、不作断言**；本模型选择
// **响亮失败 + 诊断**（`array-block-past-code`），⛔ 不静默、不夹紧。判据：机器停在诊断上、
// 目标一格都没写；再给一条**合法块**的对照（否则"永远抛"也能让上两条绿）。
bool check_array_block_bounds(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "array-block-bounds: %s\n", what);
        return false;
    };
    // 对照：块 = 6 号字（0x64 占 5 个 word、0x2 占 1 个 word）⇒ count = 3、数据 0 / 1 / −1。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x64, {{9, 0}, {0, 6}});
        emit_instruction(bytes, 2, {});
        put_u32(bytes, 3);            // count
        put_u32(bytes, 0);            // ENC 的源 dword
        put_u32(bytes, 1);
        put_u32(bytes, 0xFFFFFFFFu);
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) {
            std::fprintf(stderr, "0x64 合法块把机器停了：%s\n", machine.last_stop().message.c_str());
            return false;
        }
        const std::int32_t expected[3] = {0, 1, -1};
        for (std::uint32_t i = 0; i < 3; ++i) {
            if (machine.local_int(i) != expected[i]) {
                std::fprintf(stderr, "0x64 合法块 A[%u] = %d（期望 %d）\n", i, machine.local_int(i),
                             expected[i]);
                return false;
            }
        }
    }
    // 越界三例：块号远在代码区外 / 块号为负 / count 越过代码区末尾。
    for (const std::int32_t block : {0x1000, -1}) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x64, {{9, 0}, {0, static_cast<std::uint32_t>(block)}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        for (std::uint32_t i = 0; i < 3; ++i) machine.write_int(core::Operand{9, i}, 7);
        if (machine.step()) {
            std::fprintf(stderr, "0x64(block=%d) 越界却继续执行（没有诊断）\n", block);
            return false;
        }
        if (machine.last_stop().message.rfind("array-block-past-code", 0) != 0) {
            std::fprintf(stderr, "0x64(block=%d) 诊断 = %s\n", block,
                         machine.last_stop().message.c_str());
            return false;
        }
        for (std::uint32_t i = 0; i < 3; ++i) {
            if (machine.local_int(i) != 7) return fail("越界支写了目标格（应当一格都不写）");
        }
    }
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x64, {{9, 0}, {0, 6}});
        emit_instruction(bytes, 2, {});
        put_u32(bytes, 100);  // count 说 100 个字，实际只有 2 个
        put_u32(bytes, 0);
        put_u32(bytes, 1);
        core::Machine machine(key, table);
        machine.load_root(bytes);
        for (std::uint32_t i = 0; i < 3; ++i) machine.write_int(core::Operand{9, i}, 7);
        if (machine.step()) return fail("块内 count 越过代码区末尾却继续执行");
        if (machine.last_stop().message.rfind("array-block-past-code", 0) != 0 ||
            machine.last_stop().message.find("count=100") == std::string::npos) {
            std::fprintf(stderr, "0x64(count 越界) 诊断 = %s\n", machine.last_stop().message.c_str());
            return false;
        }
        for (std::uint32_t i = 0; i < 3; ++i) {
            if (machine.local_int(i) != 7) return fail("count 越界支写了目标格（应当一格都不写）");
        }
    }
    // 个数 ≤ 0 ⇒ **什么都不做**（`60-ops-array.md:118`，有符号语义）：`0xFFFFFFFF` 在引擎里是 −1，
    // ⛔ 不许按 u32 判成"越过代码区末尾"。目标三格预置 7 ⇒ 执行后必须仍是 7，且**不停机**。
    for (const std::uint32_t raw_count : {0xFFFFFFFFu, 0u}) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x64, {{9, 0}, {0, 6}});
        emit_instruction(bytes, 2, {});
        put_u32(bytes, raw_count);
        put_u32(bytes, 0);  // 块里仍有数据字：本支按个数判，不看它们
        put_u32(bytes, 1);
        core::Machine machine(key, table);
        machine.load_root(bytes);
        for (std::uint32_t i = 0; i < 3; ++i) machine.write_int(core::Operand{9, i}, 7);
        if (!machine.step()) {
            std::fprintf(stderr, "0x64(count=%u) 把机器停了（个数 ≤ 0 应当什么都不做）：%s\n", raw_count,
                         machine.last_stop().message.c_str());
            return false;
        }
        for (std::uint32_t i = 0; i < 3; ++i) {
            if (machine.local_int(i) != 7) return fail("个数 ≤ 0 时写了目标格（应当一格都不写）");
        }
    }
    return true;
}
// ── 三条数组写指令的**实现侧边界**（`RES-ARRAY-FILL-ZERO-OOB` / `-SET-TO-OOB` / `-ARRAY-COPY-OOB`）──
// spec 的 impact：「实现必须拒绝或先记诊断再拒绝（不许 clamp、不许静默写坏相邻池）」。
// 数据：目标 = **局部整型池窗口的最后一格**（tag 9，下标 0x100000/4 − 1），count = 2 ⇒
// 第 2 格会落进相邻的局部浮点族窗口。本模型必须拒绝，且**一格都不写**（用预置值咬住）。
// ★ "每条数组的容量"在本模型里没有数据（指令只给地址、不给容量；引擎结构上同样无从比较）⇒
//   这里能机械判的是"跨出池窗口"这一档；⛔ 不是把 count 截断成容量（那会掩盖 UB）。
bool check_array_write_bounds(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    constexpr std::uint32_t kLastLocalInt = 0x00100000u / 4u - 1u;  // 262143
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "array-write-bounds: %s\n", what);
        return false;
    };
    // 0x6c / 0x2d8：目标 = 末格、count = 2 ⇒ 越界；目标格预置 1234、相邻浮点格预置 1.5。
    for (const std::uint32_t opcode : {0x6Cu, 0x2D8u}) {
        auto bytes = make_script_with_body();
        if (opcode == 0x6C) {
            emit_instruction(bytes, 0x6C, {{9, kLastLocalInt}, {0, 2}});
        } else {
            emit_instruction(bytes, 0x2D8, {{9, kLastLocalInt}, {0, 7}, {0, 2}});
        }
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        machine.write_int(core::Operand{9, kLastLocalInt}, 1234);
        machine.write_float(core::Operand{10, 0}, 1.5);
        if (machine.step()) {
            std::fprintf(stderr, "0x%X 目标跨出池窗口却继续执行\n", opcode);
            return false;
        }
        if (machine.last_stop().message.rfind("array-write-past-pool", 0) != 0) {
            std::fprintf(stderr, "0x%X 诊断 = %s\n", opcode, machine.last_stop().message.c_str());
            return false;
        }
        if (machine.local_int(kLastLocalInt) != 1234) return fail("越界支写了目标格");
        if (machine.read_float(core::Operand{10, 0}) != 1.5) return fail("越界支写坏了相邻池");
    }
    // 对照：0x6c 在窗口内（倒数第 2 格、count = 2）⇒ 两格都写 ENC(0)（读整 = 0），必须照常执行。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x6C, {{9, kLastLocalInt - 1u}, {0, 2}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        machine.write_int(core::Operand{9, kLastLocalInt - 1u}, 1234);
        machine.write_int(core::Operand{9, kLastLocalInt}, 1234);
        if (!machine.step()) {
            std::fprintf(stderr, "0x6c 窗口内的两格被拒了：%s\n", machine.last_stop().message.c_str());
            return false;
        }
        if (machine.local_int(kLastLocalInt - 1u) != 0 || machine.local_int(kLastLocalInt) != 0) {
            return fail("0x6c 窗口内没有写满两格 ENC(0)");
        }
    }
    // 0x64：目标 = 末格、块内 count = 2 ⇒ 越界（目标格与相邻浮点格都不许被碰）。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x64, {{9, kLastLocalInt}, {0, 6}});
        emit_instruction(bytes, 2, {});
        put_u32(bytes, 2);  // count
        put_u32(bytes, 5);  // 源 dword 0
        put_u32(bytes, 6);  // 源 dword 1
        core::Machine machine(key, table);
        machine.load_root(bytes);
        machine.write_int(core::Operand{9, kLastLocalInt}, 1234);
        machine.write_float(core::Operand{10, 0}, 1.5);
        if (machine.step()) return fail("0x64 目标区间跨出池窗口却继续执行");
        if (machine.last_stop().message.rfind("array-write-past-pool", 0) != 0) {
            std::fprintf(stderr, "0x64 诊断 = %s\n", machine.last_stop().message.c_str());
            return false;
        }
        if (machine.local_int(kLastLocalInt) != 1234) return fail("0x64 越界支写了目标格");
        if (machine.read_float(core::Operand{10, 0}) != 1.5) return fail("0x64 越界支写坏了相邻池");
    }
    return true;
}
// ── 0x6C / 0x2D8 的**字符串族落点**（`CORE-OPS-ARRAY-FILL-ZERO` / `-SET-TO`）──
// 字符串族元素是一条 **28 字节记录**（`40-pools` 的「字符串记录」：记录内部字段布局**不在本版**）。
// spec 的裸写口径：`0x6c` / `0x2d8` 仍是"从目标地址起逐 4 字节写 count 个 32 位字"，
// 目标落字符串族时同样如此（⛔ 不是按 28 字节步进）⇒ 本模型按地址登记那些 4 字节字。
// 判据走**指令面**：先写记录，再用 `0x1b0`（裸池拷贝）把记录里的字搬进局部整型池读回解码值。
// ★ 解码是双射 ⇒ "读回 = 原值"等价于"写进去的就是 `ENC(原值)` 的裸位模式"。
bool check_string_record_raw(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "string-record-raw: %s\n", what);
        return false;
    };
    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x6C, {{11, 0}, {0, 2}});          // 局部字符串 0：2 格 ← ENC(0)
    emit_instruction(bytes, 0x2D8, {{11, 1}, {0, 7}, {0, 2}});  // 局部字符串 1：2 格 ← ENC(7)
    emit_instruction(bytes, 0x1B0, {{11, 0}, {9, 0}, {0, 2}});  // 记录 0 的前 2 个字 → 局部整型 0/1
    emit_instruction(bytes, 0x1B0, {{11, 1}, {9, 2}, {0, 2}});  // 记录 1 的前 2 个字 → 局部整型 2/3
    emit_instruction(bytes, 2, {});
    core::Machine machine(key, table);
    machine.load_root(bytes);
    for (int i = 0; i < 4; ++i) {
        if (!machine.step()) {
            std::fprintf(stderr, "字符串族裸写第 %d 步失败：%s\n", i,
                         machine.last_stop().message.c_str());
            return false;
        }
    }
    const std::int32_t expected[4] = {0, 0, 7, 7};
    for (std::uint32_t i = 0; i < 4; ++i) {
        if (machine.local_int(i) != expected[i]) {
            std::fprintf(stderr, "字符串族读回 [%u] = %d（期望 %d）\n", i, machine.local_int(i),
                         expected[i]);
            return false;
        }
    }
    if (!machine.errors().empty()) return fail("字符串族裸写记了错误");
    return true;
}
// ── 错误通道的**平台写请求**与两个抑制条件（`60-ops-numeric` 的「错误通道」）──
// 一次触发 = **一个**请求（文本 + 长度）；① 输出句柄无效 ⇒ 整条丢弃；② 宿主过滤器返回非 0 ⇒ 丢弃。
// ★ 两个抑制条件下诊断**仍然保留**在 `errors_` 里（"没发请求"与"没记诊断"是两件事）。
bool check_error_channel(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const std::string kText = "SetBitの引数が不正です．\r\n";
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "error-channel: %s\n", what);
        return false;
    };
    struct Request {
        std::string text;
        std::size_t length = 0;
    };
    struct Outcome {
        bool stepped = false;
        std::vector<Request> requests;
        std::size_t errors = 0;
        std::string error_text;
        std::size_t write_requests = 0;
    };
    const auto run_once = [&](bool handle_valid, int filter_value) {
        Outcome out;
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x135, {{9, 0}, {0, 32}});  // 位号 32 ⇒ 错误通道
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.set_error_output_writer(
            [&out](const std::string& text, std::size_t length) {
                out.requests.push_back(Request{text, length});
            });
        machine.set_error_output_handle_valid(handle_valid);
        machine.set_error_output_filter([filter_value] { return filter_value; });
        machine.load_root(bytes);
        out.stepped = machine.step();
        out.errors = machine.errors().size();
        if (!machine.errors().empty()) out.error_text = machine.errors().front();
        out.write_requests = machine.error_write_requests();
        return out;
    };
    // ① 句柄有效 + 过滤器 0：恰一条请求，文本与长度逐字。
    {
        const Outcome out = run_once(true, 0);
        if (!out.stepped) return fail("错误通道触发把机器停了（它不该抛 / 不该中止）");
        if (out.requests.size() != 1) {
            std::fprintf(stderr, "平台写请求 = %zu 条（期望恰 1）\n", out.requests.size());
            return false;
        }
        if (out.requests.front().text != kText) {
            std::fprintf(stderr, "请求文本 = 「%s」（期望「%s」）\n", out.requests.front().text.c_str(),
                         kText.c_str());
            return false;
        }
        if (out.requests.front().length != kText.size()) {
            std::fprintf(stderr, "请求长度 = %zu（期望 %zu）\n", out.requests.front().length,
                         kText.size());
            return false;
        }
        if (out.write_requests != 1) return fail("请求计数 ≠ 1");
        if (out.errors != 1 || out.error_text != kText) return fail("发了请求却丢了诊断");
    }
    // ② 输出句柄无效 ⇒ 不发请求；诊断仍在。
    {
        const Outcome out = run_once(false, 0);
        if (!out.stepped) return fail("句柄无效支把机器停了");
        if (!out.requests.empty()) return fail("句柄无效却发了平台写请求");
        if (out.errors != 1 || out.error_text != kText) return fail("句柄无效时诊断没有保留");
    }
    // ③ 宿主过滤器返回非 0 ⇒ 丢弃；诊断仍在；**「已装」标志保留**（下一触发仍会调用它）。
    {
        const Outcome out = run_once(true, 7);
        if (!out.stepped) return fail("过滤器抑制支把机器停了");
        if (!out.requests.empty()) return fail("过滤器返回非 0 却发了请求");
        if (out.errors != 1) return fail("过滤器抑制时诊断没有保留");
    }
    // ④ 三道判定的**顺序与一次性**（`60-ops-numeric` 的「错误通道」）：
    //    ① 过滤器返回 0 但句柄无效 ⇒ 不写，**「已装」标志照样清 0**（⛔ 不许把句柄判定提到过滤器之前）；
    //    ② 过滤器返回非 0 ⇒ 整条丢弃、标志保留 ⇒ 第二次触发**仍**调过滤器。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x135, {{9, 0}, {0, 32}});  // 位号 32 ⇒ 错误通道
        emit_instruction(bytes, 0x135, {{9, 1}, {0, 32}});  // 第二次触发
        emit_instruction(bytes, 2, {});
        // 过滤器返回 0、句柄无效：两次都不写；过滤器第一次之后就被清掉（第二次不再调用）。
        core::Machine machine(key, table);
        std::size_t writes = 0;
        std::size_t filter_calls = 0;
        machine.set_error_output_writer(
            [&writes](const std::string&, std::size_t) { ++writes; });
        machine.set_error_output_handle_valid(false);
        machine.set_error_output_filter([&filter_calls] {
            ++filter_calls;
            return 0;
        });
        machine.load_root(bytes);
        if (!machine.step() || !machine.step()) return fail("错误通道两连发夹具停机了");
        if (writes != 0) return fail("句柄无效却发了平台写请求");
        if (filter_calls != 1) {
            std::fprintf(stderr, "过滤器被调用 %zu 次（期望 1：它是**一次性**的）\n", filter_calls);
            return fail("「已装」标志没有在句柄无效时也被清 0（把句柄判定提到了过滤器之前？）");
        }
        if (machine.errors().size() != 2) return fail("两次触发的诊断没有都保留");
        // 过滤器返回非 0：整条丢弃、标志**保留** ⇒ 第二次仍然调用它。
        core::Machine filter_machine(key, table);
        std::size_t filter_writes = 0;
        std::size_t nonzero_calls = 0;
        filter_machine.set_error_output_writer(
            [&filter_writes](const std::string&, std::size_t) { ++filter_writes; });
        filter_machine.set_error_output_handle_valid(true);
        filter_machine.set_error_output_filter([&nonzero_calls] {
            ++nonzero_calls;
            return 7;
        });
        filter_machine.load_root(bytes);
        if (!filter_machine.step() || !filter_machine.step()) {
            return fail("过滤器丢弃支夹具停机了");
        }
        if (filter_writes != 0) return fail("过滤器返回非 0 却发了请求");
        if (nonzero_calls != 2) {
            std::fprintf(stderr, "返回非 0 的过滤器被调用 %zu 次（期望 2：标志应当保留）\n",
                         nonzero_calls);
            return fail("过滤器返回非 0 时「已装」标志被清掉了（合同是保留）");
        }
    }
    return true;
}
// ── 0x12F 的**写序**（`CORE-OPS-ARRAY-SORT-12F`）：A 与 B 别名 ⇒ 右键必须每次比较重算 ──
// 数据：A = B = 局部整型 0 起（三格预置**解码 0**）、C = 局部整型 20 起（不写 ⇒ 解码 0）、N = 3。
// 写序推演（A = B 别名 ⇒ 移位会改掉正在读的键）：
//   `A[0] ← 原始 0` 把 B[0] 改成**原始 0**（解出的不是 0）；i = 1 落位后 A[1] 也是原始 0；
//   i = 2 第一次比较右移 `A[2] ← A[1]`（原始 0）⇒ **B[2]** 随之变成原始 0 ⇒
//   第二次比较的右键必须**重读 B[2]/C[2]**；缓存右键（旧实现）得到 [2,1,0]，按写序是 [1,2,0]。
bool check_sort_12f_alias(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x12F, {{9, 0}, {9, 0}, {9, 20}, {0, 3}});  // A = B（别名）
    emit_instruction(bytes, 2, {});
    core::Machine machine(key, table);
    machine.load_root(bytes);
    for (std::uint32_t i = 0; i < 3; ++i) machine.write_int(core::Operand{9, i}, 0);  // A 初值 = 解码 0
    if (!machine.step()) {
        std::fprintf(stderr, "0x12F 别名用例把机器停了：%s\n", machine.last_stop().message.c_str());
        return false;
    }
    // ★ "每次重算右键"的写序结果；**缓存右键**会给出 [2, 1, 0]（等价于把 A 的前两格互换）⇒
    //   这两格就是两种写序的判别点（数据由推演选定，不是抄实现）。
    const std::int32_t expected[3] = {1, 2, 0};
    for (std::uint32_t i = 0; i < 3; ++i) {
        if (machine.local_int(i) != expected[i]) {
            std::fprintf(stderr, "0x12F 别名 A[%u] = %d（每次重算的写序要求 %d）\n", i,
                         machine.local_int(i), expected[i]);
            return false;
        }
    }
    return true;
}
// ── 字体粗细：0x2bd（`CORE-OPS-SCALAR-VARIANT-PROLOGUE`）──
// 锚：0x42622A `mov eax,2BCh`（700）· 0x42622F / 0x426235 写字体对象两格；0x426243 / 0x42624B 是 0 支。
// ⛔ 它写的是**二选一常量**，不是操作数原值；两格相距很远，但同属那个字体对象。
// ★ 本模型不建模"尾跳进重建入口"（残差 `RES-OPS-SCALAR-VARIANT-REBUILD`）：同一条脚本里连跑两条
//   只是为了让 0 支**可证伪**（先前两格是 700，再要求它们变 0），⛔ 不是对控制流的断言。
bool check_font_weight_2bd(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "font-weight-2bd: %s\n", what);
        return false;
    };
    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x2BD, {{0, 1}});  // 非 0 ⇒ 两格都是 700
    emit_instruction(bytes, 0x2BD, {{0, 0}});  // 0 ⇒ 两格都是 0
    emit_instruction(bytes, 2, {});
    core::Machine machine(key, table);
    machine.load_root(bytes);
    if (!machine.step() || machine.font_metric_a() != 700 || machine.font_metric_b() != 700) {
        std::fprintf(stderr, "0x2bd(1) 两格 = %d/%d（期望 700/700）\n", machine.font_metric_a(),
                     machine.font_metric_b());
        return false;
    }
    if (!machine.step() || machine.font_metric_a() != 0 || machine.font_metric_b() != 0) {
        std::fprintf(stderr, "0x2bd(0) 两格 = %d/%d（期望 0/0）\n", machine.font_metric_a(),
                     machine.font_metric_b());
        return false;
    }
    if (!machine.errors().empty()) return fail("0x2bd 不该记错误");
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
            run.pixels[entry.first] = entry.second.slot.rgba;
            run.revisions[entry.first] = entry.second.slot.revision;
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
#if defined(EMULATOR_CXX_HAS_TEXT)
    // ★ 构建**声明了文本能力**（`EMULATOR_CXX_WITH_TEXT=ON` ⇒ `CMakeLists.txt` 给 `emulator-core` 的
    //   `PUBLIC EMULATOR_CXX_HAS_TEXT=1`）⇒ 后端不可用就是**真失败**：
    //   ⛔ 不许整段跳过 —— 旧写法 `if (text_backend_available()) { … }` 在无后端构建里让本函数
    //   直接 `return true`（CTest 绿而 0x205 的像素断言一条都没跑）。那是**结构假绿**。
    if (!core::text_backend_available()) {
        return fail("文本后端不可用 ⇒ 0x205 的像素断言无法判定");
    }
    {
#else
    // 本构建**不声明**文本能力（`EMULATOR_CXX_WITH_TEXT=OFF`，如 `linux-x64` 预设）⇒ 0x205 的像素断言
    // 无法判定：这里明确跳过（⛔ 不是"通过"）。算术前半由 `spec_ops_205_numtext_layout` 独立守住
    // （它不依赖字体后端），所以关掉文本后端不会让 0x205 变成零守卫。
    if (core::text_backend_available()) {
#endif
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
    core::Machine machine = text_scene_machine(key, table);
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
        // 门前渲染闸门要求 `0x21b != 0 || 状态字 & 0x2400`（`CORE-VM-LOOP`）。★ 这里用 `0x21b`：
        //   ⛔ 不能用"每拍置等待位（第 10 位）"那招 —— 第 10 位在服务链第 14 项、第 29 位（文本泵）
        //   在后面，置起等待位会把文本泵整个挡掉（那测的就成了门，不是文本泵）。
        emit_instruction(bytes, 0x21B, {{0, 1}});
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
        const core::TextureSlotObject* texture =
            machine.texture_slot(core::Machine::text_layer_texture_id(9));
        if (texture == nullptr) return std::size_t{0};
        std::size_t count = 0;
        for (std::size_t i = 3; i < texture->rgba.size(); i += 4) {
            if (texture->rgba[i] != 0) ++count;
        }
        return count;
    };
    // ① 间隔 100ms：追加后进打字态；前 5 帧（80ms）一格都不画；跨过 100ms 才画一格。
    {
        core::ConfigStore config;
        config.set_int("set:DrawMode", 1);  // 让门前渲染闸门真的能发出请求
        core::Machine machine(key, table, nullptr, config);
        machine.write_text(core::Operand{5, 0}, "ABC");
        machine.load_root(build(100));
        // ★ 剧本从 11 条指令开始（多了一条 `0x21b`）⇒ 走完 0x6e 要 11 步。
        for (int i = 0; i < 11; ++i) {
            if (!machine.step()) return fail("走到 0x6e 之前停机");
        }
        if (machine.scalar_21b() == 0) return fail("夹具：0x21b 没有打开门前渲染闸门");        if (machine.pending_glyph_count() != 3) return fail("追加之后待画格数不是 3");
        if (!machine.text_typing()) return fail("间隔 > 0 时追加后应当置第 29 位（打字中）");
        if (machine.text_timer_interval_ms() != 100) return fail("计时器间隔不是 MessageSpeed");
        // ★ 文本层变了必须请求渲染（否则"揭示一格"在没有任何动画时不会产生新帧）。
        machine.end_render();
        machine.consume_frame_step();
        for (int i = 0; i < 5; ++i) {
            if (!machine.step()) return fail("节流期停机");
            if (machine.pending_glyph_count() != 3) {
                return fail("未到 100ms 的帧画了字（把「每 D 毫秒一格」实现成「每帧一格」）");
            }
        }
        int drew = -1;
        for (int i = 0; i < 5 && drew < 0; ++i) {
            if (!machine.step()) return fail("跨过间隔时停机");
            if (machine.pending_glyph_count() == 2) drew = i;        }
        if (drew < 0) return fail("跨过 100ms 仍一格都没画（计时器没有到期分支）");
        // ★ 门前渲染步在门**之前**判定（`CORE-VM-LOOP`：场景推进 → 门前渲染步 → 过门）；
        //   文本泵是门上的第 29 位服务体 ⇒ 它画的这一格要**下一迭代**才被渲染闸门看见。
        if (!machine.step()) return fail("画完一格之后停机");
        if (!machine.consume_frame_step()) {
            return fail("文本揭示一格没有请求渲染（没有别的动画时这一格永远不上屏）");
        }
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
        for (int i = 0; i < 11; ++i) {
            if (!machine.step()) return fail("间隔 = 0 时在 0x6e 之前停机");
        }
        if (machine.text_typing()) return fail("间隔 = 0 时不该置第 29 位（应当立即抽干）");
        if (machine.pending_glyph_count() != 0) return fail("间隔 = 0 时没有同帧抽干");
        if (ink(machine) == 0) return fail("间隔 = 0 时字没有画出来");
    }
    return true;
}
// ── `0x300` 循环演示闸：逐格揭示 → 停「期限」→ 清条目 → 从第一格重来 ──
/** 引擎文本泵 `sub_409400`：槽 1..9 任一状态非 0 ⇒ 有槽在用（第 29 位保持）；每 `MessageSpeed` 毫秒
 *  从层记录 `+0x84` 揭示一格（`sub_45BE20`）；整段揭示完记起点，`现在−起点 ≥ 期限` 时清该层两段
 *  绘制键区间并把游标归 0、闸位保持（`sub_404F80`）⇒ 无限重放。`0x301` 是同一段清除的一次性入口。
 *  ★ 旧实现只把 0x300/0x301 写成两张无人读的 map ⇒ 示例文本只出现一次（用户口径："不会刷新"）。 */
bool check_text_slot_replay(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "text-gate: %s\n", what);
        return false;
    };
    const auto build = [&](std::int32_t deadline, std::int32_t speed) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x80, {{0, 9}});
        emit_instruction(bytes, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x79, {{0, 9}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x213, {{0, 9}, {0, 190700}, {0, 40}});
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 0x74, {{0, static_cast<std::uint32_t>(speed)}});
        emit_instruction(bytes, 0x300, {{0, 9}, {0, 1}, {0, static_cast<std::uint32_t>(deadline)}});
        emit_instruction(bytes, 0x71, {{0, 9}});
        emit_instruction(bytes, 0x6E, {{0, 0}, {5, 0}});  // "AB" ⇒ 两格
        emit_instruction(bytes, 0x6F, {{0, 0}});
        for (int i = 0; i < 400; ++i) emit_instruction(bytes, 0x1A8, {});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    const auto ink = [](core::Machine& machine) {
        const core::TextureSlotObject* texture =
            machine.texture_slot(core::Machine::text_layer_texture_id(9));
        if (texture == nullptr) return std::size_t{0};
        std::size_t count = 0;
        for (std::size_t i = 3; i < texture->rgba.size(); i += 4) {
            if (texture->rgba[i] != 0) ++count;
        }
        return count;
    };
    core::Machine machine = text_scene_machine(key, table);
    machine.write_text(core::Operand{5, 0}, "AB");
    machine.load_root(build(50, 20));
    for (int i = 0; i < 9; ++i) {  // 走完 0x6E（追加），停在 0x6F 之前
        if (!machine.step()) return fail("走到追加之前停机");
    }
    if (machine.text_layer_cell_count(9) != 2) {
        return fail("闸开着的层没有保留字符格（重放没有源）");
    }
    if (machine.text_layer_revealed(9) != 0) return fail("追加那一帧就揭示了格（应为 0）");
    if (machine.pending_glyph_count() != 0) {
        return fail("闸开着的层还在走普通打字队列（两个推进者）");
    }
    int last_cycles = 0;
    bool saw_erase = false;
    bool saw_ink = false;
    for (int i = 0; i < 600; ++i) {
        if (!machine.step()) break;
        if (ink(machine) > 0) saw_ink = true;
        const int cycles = machine.text_gate_cycles(9);
        if (cycles > last_cycles) {
            saw_erase = true;
            if (machine.draw_item(190700) != nullptr) {
                return fail("重放时刻没有清掉本层绘制条目（文本会一直叠着）");
            }
            if (machine.text_layer_revealed(9) != 0) return fail("重放时刻揭示游标没有归 0");
            last_cycles = cycles;
        }
        if (last_cycles >= 2) break;
    }
    if (!saw_ink) return fail("闸门层一个像素都没落（揭示没有真的画字）");
    if (!saw_erase || last_cycles < 2) {
        return fail("`0x300` 的演示闸没有循环（只展示一次 —— 用户口径的缺陷）");
    }
    if (machine.text_layer_cell_count(9) != 2) {
        return fail("重放期间字符格被改了（重放不需要重新追加）");
    }
    // ★ 闸在推进时第 29 位必须保着（否则主循环不再调泵 ⇒ 停在"揭示完"那一帧）。
    if (!machine.text_typing()) return fail("闸在跑的时候第 29 位被清了（泵会停）");
    // 参数格 = 0：整段一次揭示、**不重放**（引擎 `0x409452` 跳过记起点 / 期限清除），第 29 位清掉。
    {
        core::Machine zero_machine(key, table);
        zero_machine.write_text(core::Operand{5, 0}, "AB");
        zero_machine.load_root(build(50, 0));
        for (int i = 0; i < 300; ++i) {
            if (!zero_machine.step()) break;
        }
        if (zero_machine.text_layer_revealed(9) != zero_machine.text_layer_cell_count(9)) {
            return fail("参数格 = 0 时没有一次揭示完（引擎走抽干支）");
        }
        if (zero_machine.text_gate_cycles(9) != 0) {
            return fail("参数格 = 0 时竟然重放了（引擎跳过了记起点 / 期限清除）");
        }
        if (zero_machine.text_typing()) {
            return fail("参数格 = 0 抽干之后第 29 位没有清（泵会一直被调）");
        }
    }
    // `0x301`：立刻清本层绘制条目 + 揭示游标归 0；**不清画布像素、不删字符格**。
    const std::size_t ink_before = ink(machine);
    const std::size_t cells_before = machine.text_layer_cell_count(9);
    machine.dispatch_for_test(core::Instruction{0, 0x301, {{0, 9}}});
    if (machine.draw_item(190700) != nullptr) return fail("0x301 没有清掉本层绘制条目");
    if (machine.text_layer_revealed(9) != 0) return fail("0x301 没有把揭示游标归 0");
    if (machine.text_layer_cell_count(9) != cells_before) return fail("0x301 删掉了字符格");
    if (ink(machine) != ink_before) return fail("0x301 清了画布像素（它只清绘制条目与游标）");
    return true;
}

// ── 时钟单调：宿主样本不许把场景钟往回拨（示例文本"偶尔连播两三次"的根因） ──
/** 用户口径的缺陷：设置界面底部的 ADV 示例文本**偶尔连播 2–5 次**再回到正常节奏。
 *
 *  旧实现的链条（三环，缺一不可）：
 *   ① `0x0c8` 的短睡眠（n < 10）在**处理器体内**把 `host_clock_ms_` 推到未来（虚拟阻塞）；
 *   ② 窗口路径每个外层迭代又把 `host_clock_ms_` **写回**真实墙钟 —— 它比被推到的未来值小
 *      ⇒ **场景时钟往回走**（门前渲染步用 `host_clock_ms_` 盖钟）；
 *   ③ `0x300` 演示闸的「停期限」判据是**无符号**减法 `现在 − 起点 ≥ 期限`；往回拨 7ms
 *      就变成 `0xFFFFFFF9 ≥ 1000` ⇒ 期限**当场到期** ⇒ 清条目 + 挥标归 0 + 下一拍从第一格重来。
 *      连续几次 ⇒ 看到的就是"连播两三次，然后恢复正常速度"。
 *
 *  这条钉三件事（任一被改回去都当场红）：
 *   · **宿主样本单调**：往回设的样本被夹到"现在"，计进 `host_clock_clamps()`；
 *   · **场景钟不回拨**：`clock_backsteps() == 0`；且往回拨的那几拍**不许**让演示闸提前重放；
 *   · **一个写者**：装了 `set_sleep_host` ⇒ `0x0c8` 把 n 交给宿主、自己不再动 `host_clock_ms_`；
 *     没装（自测 / 无宿主的虚拟时钟）⇒ 仍虚拟推进 n 毫秒（`CORE-OPS-SLEEP-C8` 明禁 no-op）。
 */
bool check_clock_monotonic(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "clock-monotonic: %s\n", what);
        return false;
    };
    // 夹具与 `check_text_slot_replay` 同形：槽 9、2 个字符格、节拍 20ms、期限 1000ms。
    const auto build = [&]() {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x80, {{0, 9}});
        emit_instruction(bytes, 0x70, {{0, 9}, {0, 200}, {0, 120}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x79, {{0, 9}, {0, 5}, {0, 5}});
        emit_instruction(bytes, 0x213, {{0, 9}, {0, 190700}, {0, 40}});
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 0x74, {{0, 20}});
        emit_instruction(bytes, 0x300, {{0, 9}, {0, 1}, {0, 1000}});
        emit_instruction(bytes, 0x71, {{0, 9}});
        emit_instruction(bytes, 0x6E, {{0, 0}, {5, 0}});  // "AB" ⇒ 两格
        emit_instruction(bytes, 0x6F, {{0, 0}});
        for (int i = 0; i < 400; ++i) emit_instruction(bytes, 0x1A8, {});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    core::Machine machine = text_scene_machine(key, table);
    machine.write_text(core::Operand{5, 0}, "AB");
    // 走**外部时钟**（与窗口路径同形：宿主每个外层迭代给一个读数）。
    std::uint32_t clock = 1000;
    machine.set_host_clock_ms(clock);
    machine.load_root(build());
    const auto pump = [&](std::uint32_t advance_ms) {
        clock += advance_ms;
        machine.set_host_clock_ms(clock);
        return machine.step();
    };
    int guard = 0;
    while (machine.text_layer_cell_count(9) == 0) {  // 先等 `0x6e` 追加出字符格
        if (!pump(10)) return fail("追加之前停机（夹具坏了）");
        if (++guard > 200) return fail("字符格一直没追加（夹具坏了）");
    }
    while (machine.text_layer_revealed(9) < machine.text_layer_cell_count(9)) {
        if (!pump(10)) return fail("揭示完成之前停机（夹具坏了）");
        if (++guard > 400) return fail("揭示一直不完成（夹具坏了）");
    }
    const int cycles_before = machine.text_gate_cycles(9);
    const std::uint32_t host_before = machine.host_now_ms();
    const std::uint32_t scene_now = machine.scene_clock_ms();
    if (scene_now < 7u) return fail("夹具的场景时钟还在 0 附近（盖钟没跑）");
    // ① 往回拨的宿主样本：旧缺陷正是"短睡眠把钟推到未来、宿主采样又把它拨回"。
    machine.set_host_clock_ms(scene_now - 7u);
    if (machine.host_now_ms() != host_before) {
        return fail("往回设的宿主样本没有被夹住（场景钟会跟着回拨）");
    }
    if (machine.host_clock_clamps() == 0) return fail("往回设的宿主样本没有被记进 host_clock_clamps()");
    for (int i = 0; i < 4; ++i) {
        if (!pump(0)) break;
    }
    if (machine.clock_backsteps() != 0) return fail("场景时钟被往回拨了（单调不变量破了）");
    if (machine.text_gate_cycles(9) != cycles_before) {
        return fail("宿主钟回拨 7ms 就让演示闸的停期限当场到期（用户口径：示例文本会连播）");
    }
    // ② 正常路径：真的过了期限 ⇒ 必须重放（别把"不连播"修成"永不重放"）。
    for (int i = 0; i < 6; ++i) {
        if (!pump(200)) break;
    }
    if (machine.text_gate_cycles(9) == cycles_before) {
        return fail("过了期限仍不重放（停期限被修成了永不到期）");
    }
    // ③ 一个写者：装了宿主睡眠口 ⇒ 处理器体内不再自己推进宿主钟。
    {
        core::Machine seamed = text_scene_machine(key, table);
        seamed.set_host_clock_ms(5000);
        std::uint32_t slept = 0;
        seamed.set_sleep_host([&slept](std::uint32_t ms) { slept += ms; });
        seamed.dispatch_for_test(core::Instruction{0, 0xC8, {{0, 4}}});
        if (slept != 4u) return fail("装了宿主睡眠口，0x0c8 没有把 n 毫秒交给宿主");
        if (seamed.host_now_ms() != 5000u) {
            return fail("装了宿主睡眠口，处理器体内还自己推进宿主钟（两个写者会互相拨）");
        }
        // 没装 ⇒ 虚拟推进（自测 / 无宿主的虚拟时钟）：`CORE-OPS-SLEEP-C8` 明禁 no-op。
        core::Machine virtual_clock = text_scene_machine(key, table);
        virtual_clock.set_host_clock_ms(5000);
        virtual_clock.dispatch_for_test(core::Instruction{0, 0xC8, {{0, 4}}});
        if (virtual_clock.host_now_ms() != 5004u) {
            return fail("没有宿主睡眠口时 0x0c8 没有虚拟推进 n 毫秒（spec 明禁 no-op）");
        }
    }
    return true;
}
// ── 跨帧队列的**样式快照**：追加之后改字号，不许影响已经排好版的那一格 ──
/** 引擎在**追加时**就量度 + 逐字写出（`sub_46BE30` 的 SelectObject / 逐字写出），之后的推进只呈现
 *  已经排好版的格。旧实现（画的时候再读 `Font+0x313D4`）在设置界面被实测到：示例文本中间恰好跨帧的
 *  那一个字按**别的行的字号**画出来（30 → 20），字宽仍是 30 —— 看起来"一个字莫名变小"。
 *  这条钉两半：① 入队时带的是**追加当时的字号**；② 改字号之后画出来的字形高度仍按原字号。 */
bool check_text_pending_style_snapshot(const core::OpcodeTable& table) {
    const std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "pending-style: %s\n", what);
        return false;
    };
    if (!core::text_backend_available()) return true;  // 没有字体后端 ⇒ 字形断言无意义
    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x80, {{0, 9}});
    emit_instruction(bytes, 0x70, {{0, 9}, {0, 824}, {0, 120}, {0, 324}, {0, 570}});
    emit_instruction(bytes, 0x79, {{0, 9}, {0, 108}, {0, 52}});
    emit_instruction(bytes, 0x75, {{0, 30}});         // A 支 = 30（追加时的字号）
    emit_instruction(bytes, 0x76, {{0, 0xFFFFFF}});
    emit_instruction(bytes, 0x77, {{0, 0x000000}});
    emit_instruction(bytes, 0x78, {{0, 1}});
    emit_instruction(bytes, 0x71, {{0, 9}});
    emit_instruction(bytes, 0x74, {{0, 100}});        // 每字间隔 100ms ⇒ 这一格要**跨好几帧**才画出来
    emit_instruction(bytes, 0x6E, {{0, 0}, {5, 0}});  // 追加主文本 "M"
    emit_instruction(bytes, 0x75, {{0, 10}});         // ★ 追加之后、画出来之前把 A 支改成 10
    for (int i = 0; i < 12; ++i) emit_instruction(bytes, 0x1A8, {});
    emit_instruction(bytes, 2, {});
    core::Machine machine = text_scene_machine(key, table);
    machine.write_text(core::Operand{5, 0}, "M");
    machine.load_root(bytes);
    for (int i = 0; i < 40 && machine.pending_glyph_count() == 0; ++i) {
        if (!machine.step()) return fail("0x6E 之前停机");
    }
    if (machine.pending_glyph_count() != 1) return fail("待画格不是 1（追加没有入队）");
    if (machine.pending_glyph_size(0) != 30) {
        std::fprintf(stderr, "pending-style: queued size=%d（期望 30 = 追加当时的 A 支）\n",
                     machine.pending_glyph_size(0));
        return fail("待画格没有快照追加当时的字号");
    }
    for (int i = 0; i < 40 && machine.pending_glyph_count() != 0; ++i) {
        if (!machine.step()) return fail("放出待画格时停机");
    }
    if (machine.pending_glyph_count() != 0) return fail("待画格没有画出来");
    const core::TextureSlotObject* texture =
        machine.texture_slot(core::Machine::text_layer_texture_id(9));
    if (texture == nullptr) return fail("层 9 的画布不存在");
    int top = 1 << 30, bottom = -1;
    for (int y = 0; y < texture->height; ++y) {
        bool row_has_ink = false;
        for (int x = 0; x < texture->width && !row_has_ink; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * texture->width + x) * 4 + 3;
            if (i < texture->rgba.size() && texture->rgba[i] != 0) row_has_ink = true;
        }
        if (row_has_ink) {
            if (y < top) top = y;
            if (y > bottom) bottom = y;
        }
    }
    const int ink_height = bottom >= 0 ? bottom - top + 1 : 0;
    // 30 号 "M" 的墨迹高约 21；10 号约 7 ⇒ 阈值 15 分开两档。
    if (ink_height < 15) {
        std::fprintf(stderr, "pending-style: 墨迹高=%d（期望 >=15：按 30 号画；按 10 号画约 7）\n",
                     ink_height);
        return fail("待画格是按**画的时候**的字号画的（旧行为）");
    }
    // ── ② 粗细（`0x2bd`）也必须随格快照 ──
    // 实测缺陷（2026-10-29）：示例文本的 12 格里恰好跨帧的那一格按**常规体**画出来、其余是粗体，
    // 字宽却仍是按粗体量出来的 —— 因为 `draw_text_into_texture` 在**画的时候**现读
    // `machine.font_metric_a()` 挑字体文件，而这一帧脚本刚把 `0x2bd` 写成 0。
    // 这条钉三件：① 基准（追加时 700）；② 追加**之后**把字重改成 0 ⇒ 光栅结果必须与基准**完全相同**；
    // ③ 追加**之前**就是 0（对照）⇒ 必须与基准**不同**（证明夹具真能分辨两种字面，防假绿）。
    const auto run_weight_case = [&](int weight_change) -> int {
        auto script = make_script_with_body();
        emit_instruction(script, 0x80, {{0, 9}});
        emit_instruction(script, 0x70, {{0, 9}, {0, 824}, {0, 120}, {0, 324}, {0, 570}});
        emit_instruction(script, 0x79, {{0, 9}, {0, 108}, {0, 52}});
        emit_instruction(script, 0x75, {{0, 30}});
        emit_instruction(script, 0x76, {{0, 0xFFFFFF}});
        emit_instruction(script, 0x77, {{0, 0x000000}});  // 异色 ⇒ same_color=0，字重门才生效
        emit_instruction(script, 0x78, {{0, 1}});
        emit_instruction(script, 0x2BD, {{0, 1}});        // 基准：700（粗体）
        emit_instruction(script, 0x71, {{0, 9}});
        emit_instruction(script, 0x74, {{0, 100}});       // 跨几帧才放出来
        if (weight_change == 0) emit_instruction(script, 0x2BD, {{0, 0}});  // 对照：追加**之前**变细
        emit_instruction(script, 0x6E, {{0, 0}, {5, 0}});
        if (weight_change == 1) emit_instruction(script, 0x2BD, {{0, 0}});  // ★ 追加**之后**变细
        for (int i = 0; i < 12; ++i) emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 2, {});
        core::Machine m = text_scene_machine(key, table);
        m.write_text(core::Operand{5, 0}, "M");
        m.load_root(script);
        // 走完整段脚本（脚本末尾是 `exit`；12 条空转足够把那一格在 100ms 间隔后放出来）。
        for (int i = 0; i < 80 && !m.halted(); ++i) {
            if (!m.step()) break;
        }
        const core::TextureSlotObject* canvas =
            m.texture_slot(core::Machine::text_layer_texture_id(9));
        if (canvas == nullptr) return -1;
        int count = 0;
        for (std::size_t i = 3; i < canvas->rgba.size(); i += 4) {
            if (canvas->rgba[i] != 0) ++count;
        }
        return count;
    };
    const int ink_bold = run_weight_case(2);
    if (ink_bold <= 0) return true;  // 没有字体文件 / 字形后端 ⇒ 这条判不了
    const int ink_after_change = run_weight_case(1);
    const int ink_before_change = run_weight_case(0);
    if (ink_after_change != ink_bold) {
        std::fprintf(stderr,
                     "pending-style: 追加后改字重 → 墨迹 %d，基准（粗体）%d ⇒ 已排好版的格没有快照字重\n",
                     ink_after_change, ink_bold);
        return fail("待画格是按**画的时候**的字重画的（旧行为）");
    }
    if (ink_before_change == ink_bold) {
        std::fprintf(stderr,
                     "pending-style: 夹具分辨不出粗体/常规（两者墨迹都是 %d；Bold 字面缺失？）\n",
                     ink_bold);
        return fail("夹具没有分辨力：粗体与常规字面产生同样的墨迹");
    }
    return true;
}
// ── 文本记录标志位 2 的置位者与**字号那一侧**的消费口径 ──
/** 引擎：位 2 不是共用追加器置的，而是**调用方**在 flags 实参上置起（振り仮名 run 置、主线正文 run 不置）；
 *  ★ 位 2 的完整口径 = **变体选择**（换**整支**字体对象：字面名 / 字号 / 绘制模式；见 CTest
 *    `spec_adv_text_span_bit2` 与 `check_adv_text_span_bit2`）—— 本函数只守它的**字号那一侧**，别把
 *    这里的 `record_font_size` 读成"位 2 只有字号一个效果"。这条守三件：
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
        // 待画格：主文本的格带**追加当时的 A 支字号**（这条夹具里 `0x75` = 20）；振り仮名的格是 B 支（10）。
        // ★ 这里刻意断言"带了字号"而不是"size = 0 让绘制侧现读"——后者会让跨帧夹住的那一格
        //   被别的行改过的字号画出来（设置界面示例文本实测：30 → 20，一个字莫名变小）。
        const std::size_t count = machine.pending_glyph_count();
        if (count < 2) return fail("待画格太少（主文本或振り仮名没有入队）");
        if (machine.pending_glyph_size(0) != 20 || machine.pending_glyph_size(1) != 20) {
            return fail("主文本的待画格没有快照 A 支字号（这条夹具里是 20）");
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
// ── 行记录日志的标志字第 2 位 = 变体选择（`ADV-TEXT-LAYER` 的「消费者 = 绘制」/ 位表）──
// 正文 → 机械判据（三处的判据分开写，⛔ 不许写成"层的一格"、也⛔ 不许按那个没有写者的节点种类格分岔）：
//   ① **追加侧**判的是**它收到的标志参数**的第 2 位（那一刻记录还没写出）：主线正文 run 的参数位 2 未置
//      ⇒ 落库的是 A 支；振り仮名 run 的参数 | 4 ⇒ 落库的是 B 支。把追加侧写成"永远 A 支 / 按记录自己的
//      标志字" ⇒ 本条红（记录 1 的那一支会变回 A）。
//   ② **变体 = 换整支字体对象**（字面名 / 字号 / 绘制模式一起换，⛔ 不是只换字号）：两支的 `face` 不同、
//      字号不同、绘制模式取的是**对象**的那一格（本例 = 3）。把变体写成"只改字号" ⇒ 本条红。
//   ③ **两条绘制侧**（`0x82`）按**记录自己的**标志字第 2 位分岔：同一条 `0x82`、**同一个节点槽**，
//      记录 0（位 2 = 0）⇒ 段记录那条路径（绘制请求 +1、变体路径 +0）；
//      记录 1（位 2 = 1）⇒ 变体（逐字支）那条路径（变体请求 +1、绘制请求 +0），且它用的字体对象
//      是**整支** B 支（字面名 / 字号 / 绘制模式）。把分岔改回"节点种类格 / 层的一格" ⇒ 两条记录
//      会走同一条路 ⇒ 本条红。
//   ★ 逐字支的**逐字请求本身**本版未建模（只记请求与所用字体对象）；⛔ 不据此声称逐字像素正确。
bool check_adv_text_span_bit2(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "adv-text-span-bit2: %s\n", what);
        return false;
    };
    auto bytes = make_script_with_body();
    emit_instruction(bytes, 0x80, {{0, 9}});
    emit_instruction(bytes, 0x70, {{0, 9}, {0, 400}, {0, 120}, {0, 108}, {0, 52}});
    emit_instruction(bytes, 0x79, {{0, 9}, {0, 108}, {0, 52}});
    emit_instruction(bytes, 0x213, {{0, 9}, {0, 180700}, {0, 40}});
    emit_instruction(bytes, 0x75, {{0, 20}});   // A 支字号 20
    emit_instruction(bytes, 0x197, {{0, 10}});  // B 支字号 10
    emit_instruction(bytes, 0x1A5, {{5, 2}});   // A 支字面名（那一支字体对象的横排描述；操作数用独立记录）
    emit_instruction(bytes, 0x2FE, {{5, 3}});   // B 支字面名（**另一支**字体对象）
    emit_instruction(bytes, 0x78, {{0, 3}});    // 绘制模式 3（对象的一格；变体也按它取）
    emit_instruction(bytes, 0x71, {{0, 9}});
    emit_instruction(bytes, 0x196, {{0, 0}, {5, 0}, {5, 1}});
    // 两条记录、**同一个节点槽**（op1 = 9），只有记录自己的标志字第 2 位不同。
    emit_instruction(bytes, 0x82, {{0, 9}, {0, 0}, {0, 0}, {0, 0}, {0, 0}});
    emit_instruction(bytes, 0x82, {{0, 9}, {0, 1}, {0, 0}, {0, 0}, {0, 0}});
    emit_instruction(bytes, 2, {});
    core::Machine machine(key, table);
    machine.write_text(core::Operand{5, 0}, "AB");
    machine.write_text(core::Operand{5, 1}, "\x82\xA9\x82\xAB");  // かき
    machine.write_text(core::Operand{5, 2}, "FACE A");
    machine.write_text(core::Operand{5, 3}, "FACE B");
    machine.load_root(bytes);
    for (int i = 0; i < 40 && machine.text_span_count() == 0; ++i) {
        if (!machine.step()) return fail("0x196 之前停机了");
    }
    if (machine.text_span_count() != 2) {
        std::fprintf(stderr, "文本记录数 = %zu（期望 2：主文本 + 振り仮名）\n",
                     machine.text_span_count());
        return fail("主文本 + 振り仮名应当各追加一条文本记录");
    }
    if ((machine.text_span_flags(0) & 4) != 0) return fail("主线正文记录置了位 2");
    if ((machine.text_span_flags(1) & 4) == 0) return fail("振り仮名记录没有置位 2");
    // ① 追加侧按**参数**选支并把该支落库。
    const core::TextRunFont base = machine.text_span_run_font(0);
    const core::TextRunFont variant = machine.text_span_run_font(1);
    if (base.variant || !variant.variant) {
        return fail("追加侧没有按（收到的）标志参数的第 2 位选支");
    }
    // ② 变体 = 换**整支**字体对象：字面名 / 字号 / 绘制模式一起换。
    if (base.face != "FACE A" || variant.face != "FACE B") {
        std::fprintf(stderr, "两支的字面名 = {%s, %s}（期望 {FACE A, FACE B}）\n", base.face.c_str(),
                     variant.face.c_str());
        return fail("变体没有换字面名（只换了字号 ⇒ 变体不是「整支字体对象」）");
    }
    if (base.size != 20 || variant.size != 10) {
        std::fprintf(stderr, "两支的字号 = {%d, %d}（期望 {20, 10}）\n", base.size, variant.size);
        return fail("两支的字号不是各自那一格（A = `0x75` / B = `0x197`）");
    }
    if (base.draw_mode != 3 || variant.draw_mode != 3) {
        std::fprintf(stderr, "两支的绘制模式 = {%d, %d}（期望都是 3 = 对象那一格）\n", base.draw_mode,
                     variant.draw_mode);
        return fail("绘制模式不是字体对象的那一格（`0x78`）⇒ 变体没有把绘制模式带进整支");
    }
    // ③ 两条绘制侧按**记录自己的**标志字分岔（同一条指令、同一个节点槽）。
    const auto draws_before = machine.text_span_draw_requests();
    const auto alts_before = machine.text_span_alt_requests();
    for (int i = 0; i < 12 && machine.text_span_draw_requests() == draws_before; ++i) {
        if (!machine.step()) return fail("记录 0 的 0x82 之前停机了");
    }
    if (machine.text_span_draw_requests() != draws_before + 1) {
        return fail("位 2 未置的记录没有走段记录那条绘制路径");
    }
    if (machine.text_span_alt_requests() != alts_before) {
        return fail("位 2 未置的记录却走了变体（逐字支）那条路径");
    }
    for (int i = 0; i < 12 && machine.text_span_alt_requests() == alts_before; ++i) {
        if (!machine.step()) return fail("记录 1 的 0x82 之前停机了");
    }
    if (machine.text_span_alt_requests() != alts_before + 1) {
        std::fprintf(stderr, "变体路径请求数 = %zu（期望 +1）\n", machine.text_span_alt_requests());
        return fail("位 2 置起的记录没有走变体（逐字支）那条绘制路径（分岔判据不是记录自己的标志字？）");
    }
    if (machine.text_span_draw_requests() != draws_before + 1) {
        return fail("位 2 置起的记录也走了一遍段记录路径（两条路径没有分岔）");
    }
    const core::TextRunFont alt = machine.last_alt_run_font();
    if (!alt.variant || alt.face != "FACE B" || alt.size != 10 || alt.draw_mode != 3) {
        std::fprintf(stderr, "变体路径的字体对象 = {变体 %d, 面名 %s, 字号 %d, 绘制模式 %d}\n",
                     alt.variant ? 1 : 0, alt.face.c_str(), alt.size, alt.draw_mode);
        return fail("变体路径用的不是「整支」B 支字体对象（字面名 / 字号 / 绘制模式）");
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
        core::Machine machine(key, table, nullptr, scene_step_config());  // ★ 纹理画布路径 = 场景绘制模式 1
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
    core::Machine machine = text_scene_machine(key, table);
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
        const core::TextureSlotObject* texture =
            machine.texture_slot(core::Machine::text_layer_texture_id(9));
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
        const core::TextureSlotObject* texture =
            machine.texture_slot(core::Machine::text_layer_texture_id(9));
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
    core::Machine machine = text_scene_machine(key, table);
    machine.write_text(core::Operand{5, 0}, "AB");
    machine.write_text(core::Operand{5, 1}, "R");
    machine.load_root(bytes);
    for (int i = 0; i < 10; ++i) {
        if (!machine.step()) return fail("0x71 之前就停机");
    }
    if (!machine.text_layer_texture_ready(9)) return fail("0x71 没有建出层 9 的画布");
    const core::TextureSlotObject* texture =
        machine.texture_slot(core::Machine::text_layer_texture_id(9));
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
    core::Machine cleared(key, table, nullptr, scene_step_config());  // ★ 纹理画布路径 = 场景绘制模式 1
    cleared.write_text(core::Operand{5, 0}, "X");
    cleared.load_root(clear_bytes);
    for (int i = 0; i < 7; ++i) {
        if (!cleared.step()) break;
    }
    const core::TextureSlotObject* cleared_texture =
        cleared.texture_slot(core::Machine::text_layer_texture_id(9));
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
        core::Machine wrap_machine(key, table, nullptr, scene_step_config());  // ★ 场景绘制模式 1
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
// ── `0x324` 的接收者复位（`CORE-OPS-FORWARD-FIELD`；正文 `60-ops-forward.md:39-48`） ──
// 正文原句 → 机械判据（三条，互不依赖；⛔ 不是复述实现）：
//   ① 注入接收者 {a=1,b=2,c=3,marker=7}，执行 `0x324`（argc 0）⇒ 「释放接收者持有的三个对象、
//      把这三处引用清空，再清掉接收者自己的一个标记」⇒ 三处引用全 0、标记 0；复位恰一次；
//      并且「改的是**接收者自己的状态**，不是脚本可观察的数据」⇒ 接收者之外的引擎观察量一格不动。
//   ② 负控制：注同样的接收者，执行**另一条**无参指令（`0x20e` 清屏）⇒ 三处引用仍 {1,2,3}、标记仍 7、
//      复位计数仍 0 ⇒ 「只有 `0x324` 复位接收者」（否则换个 opcode 也能过，判据就没咬住）。
//   ③ **不注入**接收者（产品路径的缺省 = 引擎字段空）⇒ 不抛、不停机、错误通道不写、引擎状态一格不动，
//      只把「空请求」记一次。★ 正文**没有**规定字段为空时做什么（`60-ops-forward.md:56`：该字段本作
//      只有读、没有写）⇒ 本条**不断言行为**，只断言「没有发明行为」。
bool check_forward_field_324(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "forward-field-0x324: %s\n", what);
        return false;
    };
    // 一条无操作数的被测指令 + 收尾的 `0x2`（收尾只是让脚本形状与其它用例一致）。
    const auto no_arg_script = [](std::uint32_t opcode) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, opcode, {});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    // ① 注入接收者 ⇒ 一次完整复位。
    {
        core::Machine machine(key, table);
        machine.set_forward_receiver(core::Machine::ForwardReceiver{1, 2, 3, 7});
        machine.load_root(no_arg_script(0x324));
        const auto textures_before = machine.texture_count();
        const auto target_before = machine.render_target();
        const auto dirty_before = machine.scene_dirty();
        if (!machine.step()) return fail("0x324 停机了（argc 0 的复位不该抛 / 不该停）");
        if (!machine.forward_receiver().has_value()) return fail("注入的接收者没了");
        const auto& receiver = *machine.forward_receiver();
        if (receiver.owned_a != 0 || receiver.owned_b != 0 || receiver.owned_c != 0) {
            std::fprintf(stderr, "0x324 之后引用仍是 {%u,%u,%u}\n", receiver.owned_a,
                         receiver.owned_b, receiver.owned_c);
            return fail("复位不完整：还有引用没被清空");
        }
        if (receiver.marker != 0) return fail("0x324 没有清接收者自己的标记");
        if (machine.forward_receiver_resets() != 1) {
            std::fprintf(stderr, "复位计数 = %zu\n", machine.forward_receiver_resets());
            return fail("一次 0x324 不是恰好一次复位");
        }
        if (machine.render_target() != target_before || machine.texture_count() != textures_before ||
            machine.scene_dirty() != dirty_before) {
            return fail("0x324 改了接收者之外的引擎状态（本条只改接收者自己的状态）");
        }
    }
    // ② 负控制：同一台机器、同一个接收者，换成 `0x20e` ⇒ 接收者一格不动。
    {
        core::Machine machine(key, table);
        machine.set_forward_receiver(core::Machine::ForwardReceiver{1, 2, 3, 7});
        machine.load_root(no_arg_script(0x20e));
        if (!machine.step()) return fail("0x20e 停机了（负控制的对照指令）");
        if (!machine.forward_receiver().has_value()) return fail("对照执行后接收者没了");
        const auto& receiver = *machine.forward_receiver();
        if (receiver.owned_a != 1 || receiver.owned_b != 2 || receiver.owned_c != 3 ||
            receiver.marker != 7) {
            std::fprintf(stderr, "0x20e 之后引用是 {%u,%u,%u}、标记 %d\n", receiver.owned_a,
                         receiver.owned_b, receiver.owned_c, receiver.marker);
            return fail("别的无参指令也复位了接收者（复位点不止 0x324）");
        }
        if (machine.forward_receiver_resets() != 0) return fail("别的无参指令也计了复位");
    }
    // ③ 字段为空（不注入）：不许发明行为。
    {
        core::Machine machine(key, table);
        machine.load_root(no_arg_script(0x324));
        const auto textures_before = machine.texture_count();
        const auto target_before = machine.render_target();
        const auto dirty_before = machine.scene_dirty();
        if (!machine.step()) return fail("接收者为空时 0x324 停机了（正文没规定 ⇒ 不许发明失败）");
        if (machine.halted()) return fail("接收者为空时 0x324 让引擎停了");
        if (machine.forward_receiver().has_value()) return fail("空字段却把接收者建出来了");
        if (machine.forward_receiver_resets() != 0) return fail("空请求被计成了复位");
        if (machine.forward_receiver_null_requests() != 1) {
            std::fprintf(stderr, "空请求计数 = %zu\n", machine.forward_receiver_null_requests());
            return fail("空字段的一次请求没有恰好记一次");
        }
        if (!machine.errors().empty()) return fail("接收者为空时写了错误通道（那是发明出来的行为）");
        if (machine.render_target() != target_before || machine.texture_count() != textures_before ||
            machine.scene_dirty() != dirty_before) {
            return fail("接收者为空时 0x324 改了引擎状态（正文没规定后果）");
        }
    }
    return true;
}

// ── `0x25b` + 显示状态三格 + 两条重发路径（`CORE-OPS-RENDER-DISPLAY-STATE-25B`）──────────────
// 这一则规则的正文逐句落成机械判据（audit 的 8 条 falsifier，
// 去掉"重发②（显示对象释放后的恢复路径）"那条 —— 模型里没有对应对象 ⇒ 不建模）：
//   ① 窗口态 `0x25b 7` ⇒ 显示状态 2 / 图像号 7 / **恰好一次**载图（实参 7）；
//   ② 全屏态 `0x25b 7` ⇒ 两格照写、**0 次**载图（判据是**缓存 = 0**，不是"有没有窗口"、也不查命名键）；
//   ③ 接②把模式切回窗口态 ⇒ **再 1 次**载图（实参仍是 7）—— 这是重发①，⛔ 不是"补载"；
//   ④ 窗口态 `0x25b -3`（第 3 步不判符号 ⇒ 载图实参就是 −3）→ 全屏 → 回窗口 ⇒
//      **1 条**错误通道（逐字）+ 显示状态 1 + 外枠编号 0 + 1 次外枠请求（编号 0）；
//   ⑤ 状态 1 + 外枠编号 −5（图像号 12）：切回窗口态 ⇒ 外枠请求实参 **−5**（⛔ 不判符号、⛔ 不去载图）；
//   ⑥ 状态 0（初值）往返 ⇒ 两条请求**都不发**；
//   ⑦ 两条接缝**都不注入** ⇒ 不抛 / 不崩 / 不改格（正文没规定"缺后端"的后果 ⇒ 只能什么都不做）。
bool check_display_state_25b(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "display-state-0x25b: %s\n", what);
        return false;
    };
    // 本组用例只用到两条 argc 1 的指令（`0x25B` / `0x2CF`）⇒ 脚本 = 一串 (opcode, 操作数)。
    const auto make_script =
        [](std::initializer_list<std::pair<std::uint32_t, std::int32_t>> insns) {
            auto bytes = make_script_with_body();
            for (const auto& [opcode, operand] : insns) {
                emit_instruction(bytes, opcode, {{0, static_cast<std::uint32_t>(operand)}});
            }
            emit_instruction(bytes, 2, {});
            return bytes;
        };
    // 两条请求的探针：接缝注入后记录**实参序列**（光看计数分不清"发了 7"和"发了 0"）。
    struct Probe {
        std::vector<std::int32_t> loads;
        std::vector<core::Machine::OuterFrameRequest> outer;
        void attach(core::Machine& machine) {
            machine.set_display_image_loader([this](std::int32_t image_number) {
                loads.push_back(image_number);
            });
            machine.set_outer_frame_requester([this](core::Machine::OuterFrameRequest& request) {
                // ★ 先按 core 给的样子存一份（原点 0/0、宽高 0/0、常量 0xFF），**再**由宿主补宽高。
                outer.push_back(request);
                request.right = 1919;
                request.bottom = 1079;
            });
        }
    };
    // ① 窗口态（缓存初值 0）：三格 + 一次载图。
    {
        core::Machine machine(key, table);
        Probe probe;
        probe.attach(machine);
        machine.load_root(make_script({{0x25B, 7}}));
        if (!machine.step()) return fail("窗口态 0x25b 停机了（它不写操作数、不报错）");
        if (machine.display_mode_cache() != 0) return fail("前置不成立：这台机器不是窗口态");
        if (machine.display_state() != 2) {
            std::fprintf(stderr, "显示状态 = %d\n", machine.display_state());
            return fail("0x25b 没有把显示状态写成 2");
        }
        if (machine.display_image_number() != 7) {
            std::fprintf(stderr, "图像号 = %d\n", machine.display_image_number());
            return fail("0x25b 没有把图像号写成操作数");
        }
        if (machine.display_image_loads() != 1 || probe.loads.size() != 1) {
            std::fprintf(stderr, "载图计数 = %zu / 探针 %zu\n", machine.display_image_loads(),
                         probe.loads.size());
            return fail("窗口态 0x25b 不是恰好一次载图请求");
        }
        if (probe.loads[0] != 7) {
            std::fprintf(stderr, "载图实参 = %d（期望 7）\n", probe.loads[0]);
            return fail("载图用的是别的值（不是同一个操作数）");
        }
        if (machine.outer_frame_requests() != 0 || !probe.outer.empty()) {
            return fail("0x25b 发了外枠请求（第 3 步只有载图）");
        }
        if (!machine.errors().empty()) return fail("窗口态 0x25b 7 写了错误通道（负号 / 越界才谈错误）");
    }
    // ②③ 全屏态不载；切回窗口态**重发**（判据是缓存 = 0）。
    {
        core::Machine machine(key, table);
        Probe probe;
        probe.attach(machine);
        machine.load_root(make_script({{0x2CF, 1}, {0x25B, 7}, {0x2CF, 0}}));
        if (!machine.step()) return fail("0x2cf(1) 停机了");
        if (machine.display_mode_cache() != 1) return fail("前置不成立：0x2cf(1) 没把缓存改成 1");
        if (!machine.step()) return fail("全屏态 0x25b 停机了");
        if (machine.display_state() != 2 || machine.display_image_number() != 7) {
            std::fprintf(stderr, "全屏态：状态 %d / 图像号 %d\n", machine.display_state(),
                         machine.display_image_number());
            return fail("全屏态下前两步也被跳过了（它们是无条件的）");
        }
        if (machine.display_image_loads() != 0 || !probe.loads.empty()) {
            std::fprintf(stderr, "全屏态载图计数 = %zu\n", machine.display_image_loads());
            return fail("全屏态载了图（第三步的判据写反了）");
        }
        if (!machine.step()) return fail("0x2cf(0) 停机了");
        if (machine.display_mode_cache() != 0) return fail("0x2cf(0) 没有切回窗口态");
        if (machine.display_image_loads() != 1 || probe.loads.size() != 1) {
            std::fprintf(stderr, "切回窗口态后载图计数 = %zu\n", machine.display_image_loads());
            return fail("切回窗口态没有重发载图（重发① 那条块缺失）");
        }
        if (probe.loads[0] != 7) {
            std::fprintf(stderr, "重发实参 = %d（期望 7）\n", probe.loads[0]);
            return fail("重发用的不是图像号格的值");
        }
        if (!machine.errors().empty()) return fail("正常图像号的重发写了错误通道");
    }
    // ④ 负图像号：第 3 步照传；切回窗口态 ⇒ 错误通道 + 状态转 1 + 外枠编号 0 + 按 0 发外枠请求。
    //    ★ 外枠编号格先摆成 **9**：否则「忘了写外枠编号 ← 0」这条破坏会被初值 0 掩盖（写与不写都读到 0）。
    {
        core::Machine machine(key, table);
        Probe probe;
        probe.attach(machine);
        machine.seed_display_state_for_test(0, 0, 9);
        machine.load_root(make_script({{0x25B, -3}, {0x2CF, 1}, {0x2CF, 0}}));
        if (!machine.step()) return fail("窗口态 0x25b -3 停机了");
        if (probe.loads.size() != 1 || probe.loads[0] != -3) {
            std::fprintf(stderr, "负图像号载图实参 = %zu 条\n", probe.loads.size());
            return fail("第 3 步对负图像号做了符号处理（它只判缓存 = 0，号照传）");
        }
        if (!machine.step()) return fail("0x2cf(1) 停机了");
        if (!machine.step()) return fail("0x2cf(0) 停机了");
        if (machine.errors().size() != 1) {
            std::fprintf(stderr, "错误通道条数 = %zu\n", machine.errors().size());
            return fail("黑底替代不是恰好一条错误通道");
        }
        if (machine.errors()[0] != "外枠用ビットマップを読み込めませんでした。黒で代替します。") {
            std::fprintf(stderr, "错误文本 = %s\n", machine.errors()[0].c_str());
            return fail("黑底替代的错误文本不逐字");
        }
        if (machine.display_state() != 1) {
            std::fprintf(stderr, "黑底替代后显示状态 = %d\n", machine.display_state());
            return fail("黑底替代没有把显示状态改成 1（此后重发该走状态 1 那支）");
        }
        if (machine.outer_frame_number() != 0) {
            std::fprintf(stderr, "黑底替代后外枠编号 = %d\n", machine.outer_frame_number());
            return fail("黑底替代没有把外枠编号格写成 0");
        }
        if (machine.outer_frame_requests() != 1 || probe.outer.size() != 1) {
            std::fprintf(stderr, "外枠请求计数 = %zu\n", machine.outer_frame_requests());
            return fail("黑底替代没有按编号 0 发一次外枠请求");
        }
        if (probe.outer[0].frame_number != 0) {
            std::fprintf(stderr, "外枠请求实参 = %d\n", probe.outer[0].frame_number);
            return fail("黑底替代的外枠请求编号不是 0");
        }
        if (machine.display_image_loads() != 1) {
            std::fprintf(stderr, "载图计数 = %zu\n", machine.display_image_loads());
            return fail("黑底替代那条支又发了一次载图（它只发外枠请求）");
        }
    }
    // ⑤ 状态 1：用**外枠编号格**发请求，负数原样 ⇒ ⛔ 不判符号、⛔ 不看图像号。
    {
        core::Machine machine(key, table);
        Probe probe;
        probe.attach(machine);
        machine.seed_display_state_for_test(1, 12, -5);  // 状态 1 / 图像号 12 / 外枠编号 −5
        machine.load_root(make_script({{0x2CF, 1}, {0x2CF, 0}}));
        if (!machine.step()) return fail("0x2cf(1) 停机了（状态 1 前置）");
        if (!machine.step()) return fail("0x2cf(0) 停机了（状态 1 前置）");
        if (probe.outer.size() != 1 || machine.outer_frame_requests() != 1) {
            std::fprintf(stderr, "状态 1 的外枠请求条数 = %zu\n", probe.outer.size());
            return fail("状态 1 支没有发一次外枠请求");
        }
        if (probe.outer[0].frame_number != -5) {
            std::fprintf(stderr, "外枠请求实参 = %d（期望 −5 原样）\n", probe.outer[0].frame_number);
            return fail("状态 1 支对外枠编号做了符号处理（`CORE-OPS-RENDER-DISPLAY-STATE-25B` 明令不检查）");
        }
        if (!probe.loads.empty() || machine.display_image_loads() != 0) {
            return fail("状态 1 支去载图了（它只看外枠编号格，不看图像号）");
        }
        if (!machine.errors().empty()) return fail("状态 1 支写了错误通道");
        if (machine.display_state() != 1 || machine.outer_frame_number() != -5) {
            std::fprintf(stderr, "状态 1 支改了格：状态 %d / 外枠编号 %d\n", machine.display_state(),
                         machine.outer_frame_number());
            return fail("状态 1 支不该改这两格（请求失败才谈不改，成功更不该改）");
        }
        // 实参形式：常量 `0xFF` + 原点 `(0,0)`；宽 / 高是**宿主**的活（core 不许拿视口 / 客户区冒充）。
        if (probe.outer[0].constant != 0xFF) {
            std::fprintf(stderr, "常量 = 0x%X\n", probe.outer[0].constant);
            return fail("外枠请求的常量不是 0xFF");
        }
        if (probe.outer[0].left != 0 || probe.outer[0].top != 0) {
            return fail("外枠请求的矩形原点不是 (0,0)");
        }
        if (probe.outer[0].right != 0 || probe.outer[0].bottom != 0) {
            std::fprintf(stderr, "core 填的宽高 = %d × %d\n", probe.outer[0].right,
                         probe.outer[0].bottom);
            return fail("core 自己填了矩形宽高（那是目标画面区尺寸，模型没有渲染器对象 ⇒ 只能留给宿主）");
        }
    }
    // ⑥ 显示状态 0（初值 / 其它值）：两条重发路径都不做事。
    {
        core::Machine machine(key, table);
        Probe probe;
        probe.attach(machine);
        machine.load_root(make_script({{0x2CF, 1}, {0x2CF, 0}}));
        if (!machine.step()) return fail("0x2cf(1) 停机了（状态 0 前置）");
        if (!machine.step()) return fail("0x2cf(0) 停机了（状态 0 前置）");
        if (machine.display_state() != 0) return fail("前置不成立：显示状态不是初值 0");
        if (machine.display_image_loads() != 0 || machine.outer_frame_requests() != 0 ||
            !probe.loads.empty() || !probe.outer.empty()) {
            return fail("显示状态 0 时切回窗口态发了请求（正文 :360 说这类值什么都不做）");
        }
        if (!machine.errors().empty()) return fail("显示状态 0 的往返写了错误通道");
    }
    // ⑦ 两条接缝都不注入（产品路径的缺省）：不抛 / 不崩 / 不改格，只记账。
    {
        core::Machine machine(key, table);
        machine.load_root(make_script({{0x25B, 7}, {0x2CF, 1}, {0x2CF, 0}}));
        if (!machine.step()) return fail("没有载图接缝时 0x25b 停机了（正文没规定缺后端的后果）");
        if (machine.display_state() != 2 || machine.display_image_number() != 7) {
            return fail("没有载图接缝时前两步也没写（它们是无条件的）");
        }
        if (!machine.step() || !machine.step()) return fail("没有外枠接缝时模式往返停机了");
        if (machine.display_image_loads() != 2) {
            std::fprintf(stderr, "载图计数 = %zu（期望 2：0x25b 一次 + 重发一次）\n",
                         machine.display_image_loads());
            return fail("缺后端时重发① 没有记账（记账与「接没接后端」是两件事）");
        }
        if (!machine.errors().empty()) return fail("缺接缝却写了错误通道（那是发明出来的行为）");
    }
    {
        core::Machine machine(key, table);
        machine.seed_display_state_for_test(1, 0, 3);
        machine.load_root(make_script({{0x2CF, 1}, {0x2CF, 0}}));
        if (!machine.step() || !machine.step()) return fail("状态 1 + 空外枠接缝时停机了");
        if (machine.display_state() != 1 || machine.outer_frame_number() != 3) {
            std::fprintf(stderr, "空接缝下格被改了：状态 %d / 外枠编号 %d\n", machine.display_state(),
                         machine.outer_frame_number());
            return fail("缺外枠接缝时改了两格（正文 :368：失败不抛、不改这两格）");
        }
        if (machine.outer_frame_requests() != 1) {
            return fail("状态 1 支在空接缝下没记账");
        }
        if (!machine.errors().empty()) return fail("空外枠接缝写了错误通道（正文没这么说）");
    }
    {
        core::Machine machine(key, table);
        machine.seed_display_state_for_test(2, -4, 9);  // 状态 2 + 负图像号，两条接缝都空
        machine.load_root(make_script({{0x2CF, 1}, {0x2CF, 0}}));
        if (!machine.step() || !machine.step()) return fail("负图像号 + 空接缝时停机了");
        if (machine.errors().size() != 1) {
            std::fprintf(stderr, "错误通道条数 = %zu\n", machine.errors().size());
            return fail("空接缝下黑底替代没写错误通道（那是本条自己的语义，不是宿主的）");
        }
        if (machine.display_state() != 1 || machine.outer_frame_number() != 0) {
            return fail("空接缝下黑底替代没有改格（正文 :358 明令改）");
        }
        if (machine.outer_frame_requests() != 1) {
            return fail("空接缝下黑底替代没记一次外枠请求");
        }
    }
    return true;
}


// ── 字体度量的**产品接线**（`CORE-OPS-FORWARD-PROLOGUE`「非 0 支的下一格来自度量」） ──
// 这一则规则里**可机械断言**的部分：
//   · 一次派生发**两次**平台度量（「每一次派生发两次平台字体度量」那一句）；
//   · 写回那两格的是**第二次**的结果（⛔ 不是第一次、⛔ 不是参考字体那次）；
//   · 第一次的请求面名 = 常量参考字体，第二次 = **该描述自己的字面名**。
// 本仓的宿主简化（`SCOPE.md` §0 D1）：字形恒 Amayui ⇒ 名字不参与选择，回退判据 ②③ 在产品替换
// 生效时跳过；但**请求面名**仍是正文的形状 ⇒ 两次请求的面名必须不同（这一条可机械断言）。
// 判据（五条，互不依赖）：
//   ① 注入确定性假后端（参考字体那次与描述那次返回**不同**的固定量度）：`0x2db(1); 0x75(20)` ⇒
//      A 支**两份描述**的下一格都 == −平均字宽（−13），**不是** −字号/2（−10）；高度 == −字号（−20）。
//      ⇒ 断的是**值**：走回退得 −10、拿第一次的返回值得 −99，两种错法都能被区分。
//   ② 一次派生 `font_measure_requests()` **恰好 +2**（整条 `0x75` = 2 份描述 × 2 = +4）。
//   ③ 假后端记下两次请求的面名：第一次 == `reference_font_face()`、第二次 == 描述的 `face`
//      （⛔ 不要求两次的**返回值**不同：产品替换下两次量的都是 Amayui —— 守卫只断请求形态）。
//   ④ **产品接线**：新 Machine 调 `core::wire_platform_fonts(m)` ⇒ `has_font_measurer()` 与
//      `font_substitution()` 都为真，且再跑一次派生 `font_measure_requests()` +2。
//      ⇒ 把装配里那一行删掉 ⇒ ④ 红；这是唯一能抓住"接缝没接"的判据。
//   ⑤ **平台量度真的给出数**（同一段里，`#if defined(EMULATOR_CXX_HAS_TEXT)`）：`measure_rendering_font(20)`
//      的 `line_height` 与 `average_width` 都必须 > 0。
//      ★ 为什么要它：①②③④ 全部靠**注入的假后端** —— 把 `measure_rendering_font` 改成恒返回 `{}`，
//        那四条**一条都不响**（实测：改完之后四条用例仍全绿）。⇒ 这一条是"真后端活着"的唯一判据。
//      ⛔ 不把真机数值写成期望（只断 > 0）：数值随平台 / 字体版本变，钉数值是把宿主差异当合同。
// ★ 真机数值不可得 ⇒ 本用例只用注入的假后端断接线、只用「> 0」断真后端活着；⛔ 不声称与真机数值等价。
bool check_font_measure_wired(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "font-measure-wired: %s\n", what);
        return false;
    };
    // 确定性假后端：参考字体那次返回平均字宽 99、描述那次返回 13（两次**返回值不同** ⇒
    // "取错那次"会当场变成 −99）。行高 18 < 字号 20 ⇒ 不触发"测得行高 > 字号"的高度缩放；
    // 黑框 / 量度框留 0 ⇒ 不触发"再缩一次"（那一档的半格判据由
    // `check_text_container_and_font_names` ⑥e / ⑥f 单独守 —— 不是 `check_font_branches`）。
    struct Probe {
        std::vector<std::string> faces;
        std::vector<std::int32_t> sizes;
        void attach(core::Machine& machine) {
            machine.set_font_measurer([this](const std::string& face, std::int32_t size) {
                faces.push_back(face);
                sizes.push_back(size);
                core::Machine::FontMetrics metrics;
                metrics.line_height = 18;
                metrics.average_width = face == core::Machine::reference_font_face() ? 99 : 13;
                metrics.glyph_box_width = 0;
                metrics.measured_box_width = 0;
                return metrics;
            });
            // 产品装配的第二个动作（SCOPE §0 D1）：名字不参与选择 ⇒ 回退判据 ②③ 跳过。
            machine.set_font_substitution(true);
        }
    };
    // ②③ 单次派生：请求数 = 2，且两次的面名分别是参考字体常量与该描述自己的字面名。
    //   ★ 放在 ① 之前：②③ 断的是**请求形态**，① 断的是**写回值**；先跑请求形态才能让
    //     "删掉第一次请求 / 第二次写成参考字体" 各自红在它该红的那一条上。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2DB, {{0, 1}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        Probe probe;
        probe.attach(machine);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x2db(1) 停机了（单次派生的前置）");
        if (machine.font_align_mode() != 1) return fail("前置不成立：单次派生前对位模式不是 1");
        const std::string face = "Amayui CN";  // 描述自己的字面名（产品替换下不参与选择，只进请求）
        core::FontDescription desc;
        desc.face = face;
        const std::size_t before = machine.font_measure_requests();
        machine.derive_font_description(desc, 20);
        const std::size_t delta = machine.font_measure_requests() - before;
        if (delta != 2) {
            std::fprintf(stderr, "一次派生的度量请求增量 = %zu（期望 2）\n", delta);
            return fail("正文 :104『每一次派生发两次平台字体度量』不成立");
        }
        if (probe.faces.size() != 2) {
            std::fprintf(stderr, "一次派生的请求条数 = %zu（期望 2）\n", probe.faces.size());
            return fail("派生没有恰好发两次度量");
        }
        if (probe.faces[0] != core::Machine::reference_font_face()) {
            std::fprintf(stderr, "第一次请求的面名 = %s（期望参考字体常量）\n",
                         probe.faces[0].c_str());
            return fail("第一次度量的不是常量参考字体（ＭＳ ゴシック）");
        }
        if (probe.faces[1] != face) {
            std::fprintf(stderr, "第二次请求的面名 = %s（期望 %s）\n", probe.faces[1].c_str(),
                         face.c_str());
            return fail("第二次度量的不是该描述自己的字面名（写成了参考字体 / 别的名字）");
        }
        // 写回值取自第二次（③ 的第三面 / ① 的值）：−13，⛔ 不是 −99。
        if (desc.width != -13) {
            std::fprintf(stderr, "派生的下一格 = %d（期望 −13）\n", desc.width);
            return fail("写回值不是第二次度量的结果");
        }
    }
    // ① 两份描述的下一格 / 高度都来自**第二次**度量；`0x75` 这一步的请求数 = 2 份 × 2 次。
    //   ★ 夹具必须先给 A 支一个**非空名字**：重建入口的第一条就是"名字为空 ⇒ 什么都不做"
    //     （`CORE-OPS-LIST-SUBSYSTEM-SET-TAIL` 的 ①；`0x1a5` 是这个名字格唯一的写者，
    //     模型不建模引擎"字体对象建立时的默认面名"）。
    //     所以这里先 `0x1a5`，再比较 `0x75` 那一步的**增量** —— `0x1a5` 自己也会走一遍入口并派生。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2DB, {{0, 1}});
        emit_instruction(bytes, 0x1A5, {{5, 0}});
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        Probe probe;
        probe.attach(machine);
        machine.write_text(core::Operand{5, 0}, "Amayui CN");
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x2db(1) 停机了（对位模式夹具）");
        if (machine.font_align_mode() != 1) return fail("前置不成立：对位模式不是 1");
        if (!machine.step()) return fail("0x1a5（A 支名字）停机了");
        const std::size_t before_75 = probe.faces.size();
        if (!machine.step()) return fail("0x75(20) 停机了");
        for (std::size_t variant = 0; variant < 2; ++variant) {
            const auto& desc = machine.font_description(false, variant);
            if (desc.width != -13) {
                std::fprintf(stderr, "A 支描述 %zu 的下一格 = %d（期望 −13 = −测得平均字宽）\n",
                             variant, desc.width);
                return fail(
                    "非 0 支的下一格不是来自第二次度量（−10 = −字号/2 ⇒ 走了回退；−99 ⇒ 取了第一次）");
            }
            if (desc.height != -20) {
                std::fprintf(stderr, "A 支描述 %zu 的高度 = %d（期望 −20）\n", variant, desc.height);
                return fail("高度不是 −字号（测得行高 18 ≤ 字号 20 ⇒ 不该缩）");
            }
        }
        if (probe.faces.size() - before_75 != 4) {
            std::fprintf(stderr, "0x75 的度量请求数 = %zu（期望 4 = 2 份描述 × 2）\n",
                         probe.faces.size() - before_75);
            return fail("并非每份描述都恰好发两次度量（少发 / 多发了）");
        }
    }
    // ④ 产品接线：`core::wire_platform_fonts` 装后端 + 打开宿主替换开关，之后一次派生仍发两次度量。
    {
        core::Machine machine(key, table);
        core::wire_platform_fonts(machine);
        if (!machine.has_font_measurer()) {
            return fail("core::wire_platform_fonts 没有装度量后端（产品路径的派生会走回退）");
        }
        if (!machine.font_substitution()) {
            return fail("core::wire_platform_fonts 没有打开宿主字体替换开关（SCOPE §0 D1）");
        }
        core::FontDescription desc;
        desc.face = "Amayui CN";
        const std::size_t before = machine.font_measure_requests();
        machine.derive_font_description(desc, 20);
        const std::size_t delta = machine.font_measure_requests() - before;
        if (delta != 2) {
            std::fprintf(stderr, "产品接线下一次派生的度量增量 = %zu（期望 2）\n", delta);
            return fail("产品接线的派生没有发两次度量（后端没装上？）");
        }
        // ★ 不断言这两格的**数值**：真机数值不可得，产品后端量到的量随平台 / 字体变。
        // ⑤ 但**必须**断言真后端**活着**：上面的 ①②③④ 全用注入的假后端 —— 把
        //   `measure_rendering_font` 改成恒返回 `{}` 时它们一条都不响（复审实测四条全绿）。
        //   ⇒ 这里直接问平台要一次量度，只断「行高 / 平均字宽都是正数」这个**必要条件**。
#if defined(EMULATOR_CXX_HAS_TEXT)
        // 构建声明了文本能力 ⇒ 后端不可用就是**真失败**（与 `check_render_prims` 同口径：⛔ 不整段跳过）。
        const core::Machine::FontMetrics platform = core::measure_rendering_font(20);
        if (!(platform.line_height > 0 && platform.average_width > 0)) {
            std::fprintf(stderr, "平台量度(20) = 行高 %d / 平均字宽 %d / 黑框 %d / 量度框 %d\n",
                         platform.line_height, platform.average_width, platform.glyph_box_width,
                         platform.measured_box_width);
            return fail("平台字体量度没有给出正数（后端恒返回空量度 ⇒ 派生会把下一格写成 0）");
        }
#endif
    }
    return true;
}

// ── 尾跳进的重建入口（`CORE-OPS-LIST-SUBSYSTEM-SET-TAIL`；`0x78` / `0x2db`）──
// 正文四步 → 机械判据（每条都能红；⛔ 不依赖真实字体后端，全部用注入的确定性假后端）：
//   ① **空名早退**：名字格为空 ⇒ 入口什么都不做（不发那一次量度请求、不跑派生、错误通道不写）。
//      把早退删掉 ⇒ 本条红（请求计数 +1）。
//   ② **高度无条件 ← −字号**；★ **紧接高度的下一格在 =0 时不动**：
//      `0x2db(1)` 让派生把下一格写成 −平均字宽（−7），随后 `0x2db(0)` 必须**保持 −7**
//      （⛔ 不是被重写成 −字号/2 = −12）。把入口写成"无条件写下一格" ⇒ 本条红。
//   ③ **一次参考量度**：名字非空时每次进入口都发**一次**「建参考字体 + 取量度」，请求形态 =
//      参考面名 / 高 −字号 / 宽 −字号÷2 / 粗细 = 对象粗细格 / 字符集默认；量度落进**两张自己的**
//      结构（文本度量 + 字形度量）。把宽写成 −字号、把字符集写成 0、或少发一次 ⇒ 本条红。
//   ④ **分岔判据 = 对位模式是否为 0**（⛔ 不是 = 1）：对位模式 = 2 也必须跑派生 ⇒ 下一格 = −7；
//      写成"= 1" ⇒ 本条红。
//   ⑤ **A 支 / B 支两套**：A 支（不销毁）对自己的**两份**描述各派生一次（度量 +4）、生命周期日志为空；
//      B 支（销毁）先换回表面字体再销毁**三枚**旧引用、只对**横排那一份**派生一次（度量 +2）、
//      竖排那一份的下一格保持入口体写的 −字号/2、量度只落**文本**那一张（⛔ 与 A 支不是同一张）。
//      把两套合并 / B 支也派生两份 / 少销毁一枚 ⇒ 本条红。
//   ⑥ **两个名字格不同格**：改 B 支的名字格（`0x2fe`）⇒ A 支的描述与请求计数**不变**。
//   ★ 入口尾段的字形缓冲重分配与逐字装载**本版不建模**（残差 `RES-OPS-SET-TAIL-FIELDS`）⇒ 这里不断言。
//   ★ 模型不建模引擎"字体对象建立时写入的默认面名"（未取证）⇒ 没写过名字时名字格是空串，正好走 ①。
bool check_ops_set_tail(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    using Branch = core::FontRebuildBranch;
    using Lifecycle = core::FontLifecycle;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "set-tail: %s\n", what);
        return false;
    };
    // 确定性假后端（⛔ 不声称与真机数值等价；只让"哪一次、什么形态、写哪张结构"可证伪）：
    //   · 派生例程的两次度量：参考字体那次平均字宽 99、描述那次 7（⇒ 写回 −7，与 −字号/2 = −12 可分辨）；
    //     行高 = 字号 ⇒ 不触发高度重算；黑框 / 量度框 0 ⇒ 不触发"再缩一次"；
    //   · 入口那一次「建字体 + 取量度」：文本度量 {100, 17, 8}、字形度量 {13}。
    struct Probe {
        std::vector<core::FontBuildSpec> specs;
        std::vector<std::string> faces;
        void attach(core::Machine& machine) {
            machine.set_font_measurer([this](const std::string& face, std::int32_t) {
                faces.push_back(face);
                core::Machine::FontMetrics metrics;
                metrics.line_height = 18;  // < 字号 ⇒ 不触发"测得行高 > 字号"的高度重算
                metrics.average_width = face == core::Machine::reference_font_face() ? 99 : 7;
                metrics.glyph_box_width = 0;
                metrics.measured_box_width = 0;
                return metrics;
            });
            machine.set_font_reference_builder([this](const core::FontBuildSpec& spec) {
                specs.push_back(spec);
                core::FontBuildMetrics metrics;
                metrics.text.height = 100;
                metrics.text.ascent = 17;
                metrics.text.average_width = 8;
                metrics.glyph.box_width = 13;
                return metrics;
            });
            // 宿主替换（SCOPE §0 D1）：派生例程跳过回退判据 ②③ ⇒ 下一格真的来自度量。
            machine.set_font_substitution(true);
        }
    };
    const auto same_text_metric = [](const core::FontTextMetric& a,
                                     const core::FontTextMetric& b) {
        return a.height == b.height && a.ascent == b.ascent && a.average_width == b.average_width;
    };
    // ① 空名早退：`0x75(20)` 的入口体照写自己的两格，但**入口**因名字空什么都不做。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 0x2DB, {{0, 2}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        Probe probe;
        probe.attach(machine);
        machine.load_root(bytes);
        if (!machine.step()) return fail("（空名）0x75(20) 停机了");
        if (!machine.step()) return fail("（空名）0x2db(2) 停机了");
        if (machine.font_build_requests(Branch::NonDestroy) != 0) {
            return fail("名字为空时入口仍发了「建参考字体 + 取量度」请求（① 的空名早退没了）");
        }
        if (machine.font_measure_requests() != 0) {
            std::fprintf(stderr, "空名时度量请求数 = %zu（期望 0）\n",
                         machine.font_measure_requests());
            return fail("名字为空时入口仍跑了派生（① 的空名早退没了）");
        }
        if (!probe.specs.empty()) return fail("空名时平台侧仍收到了建字体请求");
        if (!machine.font_lifecycle_log().empty()) {
            return fail("空名时销毁那一套仍发了两枚 / 三枚旧引用销毁请求");
        }
        if (!machine.errors().empty()) {
            std::fprintf(stderr, "空名时错误通道 = %zu 条\n", machine.errors().size());
            return fail("名字为空被当成了报错（正文：什么都不做、⛔ 不是报错）");
        }
        if (machine.font_description(false, 0).height != -20 ||
            machine.font_description(false, 1).height != -20) {
            return fail("空名时入口改了两份描述的高度（正文：一个格都不写）");
        }
        if (machine.font_description(false, 0).width != -10) {
            return fail("空名时入口改了下一格（正文：一个格都不写）");
        }
    }
    // ②③④ 高度 / 下一格 / 一次参考量度 / 分岔判据。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2BD, {{0, 1}});   // 粗细格 = 700（入口请求的粗细取它）
        emit_instruction(bytes, 0x75, {{0, 20}});   // A 支字号 20（入口：名字还空 ⇒ 早退）
        emit_instruction(bytes, 0x1A5, {{5, 0}});   // A 支名字 ← 非空（这个名字格唯一的写者）
        emit_instruction(bytes, 0x2DB, {{0, 1}});   // 对位模式 1 ⇒ 两份描述各派生一次
        emit_instruction(bytes, 0x2DB, {{0, 0}});   // 对位模式 0 ⇒ 派生不跑、下一格**不动**
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        Probe probe;
        probe.attach(machine);
        machine.write_text(core::Operand{5, 0}, "Amayui CN");
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x2bd(1) 停机了");
        if (!machine.step()) return fail("0x75(20) 停机了");
        if (machine.font_build_requests(Branch::NonDestroy) != 0) {
            return fail("A 支名字还空着时入口就发了请求（空名早退没生效）");
        }
        if (!machine.step()) return fail("0x1a5（A 支名字）停机了");
        // ② + ③：名字非空 ⇒ 高度 ← −字号（两份）；**一次**量度请求，形态钉死。
        if (machine.font_description(false, 0).height != -20 ||
            machine.font_description(false, 1).height != -20) {
            return fail("入口没有把两份描述的高度写成 −字号（正文 ②）");
        }
        if (machine.font_description(false, 0).width != -10) {
            return fail("入口体（`0x1a5`）没写下一格 —— 夹具前提不成立");
        }
        if (machine.font_build_requests(Branch::NonDestroy) != 1) {
            std::fprintf(stderr, "参考量度请求数 = %zu（期望 1）\n",
                         machine.font_build_requests(Branch::NonDestroy));
            return fail("名字非空时入口没有恰好发**一次**「建参考字体 + 取量度」请求");
        }
        const auto& spec = machine.font_build_spec(Branch::NonDestroy);
        if (spec.face != core::Machine::reference_font_face()) {
            std::fprintf(stderr, "参考字体的面名 = %s（期望常量 ＭＳ ゴシック）\n",
                         spec.face.c_str());
            return fail("那一次参考量度建的不是常量参考字体");
        }
        if (spec.height != -20 || spec.width != -10) {
            std::fprintf(stderr, "参考字体的高 / 宽 = %d / %d（期望 −20 / −10 = −字号 / −字号÷2）\n",
                         spec.height, spec.width);
            return fail("参考字体的高 / 宽形态不对（宽被写成了 −字号？）");
        }
        if (spec.weight != 700) {
            std::fprintf(stderr, "参考字体的粗细 = %d（期望 700 = 对象的粗细格）\n", spec.weight);
            return fail("参考字体的粗细不是取对象粗细格（`0x2bd`）");
        }
        if (spec.charset != 1) {
            std::fprintf(stderr, "参考字体的字符集 = %d（期望 1 = 默认）\n", spec.charset);
            return fail("参考字体的字符集不是默认（正文：字符集默认）");
        }
        const auto& text = machine.font_text_metric(Branch::NonDestroy);
        const auto& glyph = machine.font_glyph_metric(Branch::NonDestroy);
        if (text.height != 100 || text.ascent != 17 || text.average_width != 8) {
            std::fprintf(stderr, "文本度量 = {高 %d, ascent %d, 平均字宽 %d}（期望 {100, 17, 8}）\n",
                         text.height, text.ascent, text.average_width);
            return fail("入口没有把量度写进**文本度量**那张结构");
        }
        if (glyph.box_width != 13) {
            std::fprintf(stderr, "字形度量 = {黑框 %d}（期望 13）\n", glyph.box_width);
            return fail("入口没有把量度写进**字形度量**那张结构（两张结构被合并了？）");
        }
        if (machine.font_measure_requests() != 0) {
            return fail("对位模式 = 0 时入口仍跑了派生（正文 ④：为 0 就跳过派生）");
        }
        // 对位模式 = 1 ⇒ 两份描述各一次派生 ⇒ 下一格 = −平均字宽（−7），度量 +4。
        if (!machine.step()) return fail("0x2db(1) 停机了");
        for (std::size_t variant = 0; variant < 2; ++variant) {
            if (machine.font_description(false, variant).width != -7) {
                std::fprintf(stderr, "A 支描述 %zu 的下一格 = %d（期望 −7 = −测得平均字宽）\n",
                             variant, machine.font_description(false, variant).width);
                return fail("对位模式 ≠ 0 时没有对两份描述各跑一次派生");
            }
            if (machine.font_description(false, variant).height != -20) {
                return fail("派生后高度不是 −字号");
            }
        }
        if (machine.font_measure_requests() != 4) {
            std::fprintf(stderr, "A 支一次入口的派生度量数 = %zu（期望 4 = 2 份 × 2）\n",
                         machine.font_measure_requests());
            return fail("A 支入口的派生次数不是「两份描述各一次」");
        }
        if (!machine.font_lifecycle_log().empty()) {
            return fail("A 支（不销毁旧句柄那一套）也销毁了旧引用");
        }
        // 对位模式 = 0 ⇒ 派生不跑；★ 下一格**保持 −7**（⛔ 不是被重写成 −字号/2）。
        if (!machine.step()) return fail("0x2db(0) 停机了");
        if (machine.font_description(false, 0).width != -7) {
            std::fprintf(stderr, "=0 之后 A 支描述 0 的下一格 = %d（期望保持 −7）\n",
                         machine.font_description(false, 0).width);
            return fail("入口体在 =0 时把下一格重写了（正文：紧接高度的下一格不动）");
        }
        if (machine.font_description(false, 0).height != -20 ||
            machine.font_description(false, 1).height != -20) {
            return fail("=0 之后高度不是 −字号");
        }
        if (machine.font_measure_requests() != 4) {
            return fail("=0 时仍跑了派生（判据被写成了「≠ 1」之类？）");
        }
        if (machine.font_build_requests(Branch::NonDestroy) != 3) {
            std::fprintf(stderr, "参考量度请求数 = %zu（期望 3：`0x1a5` + 两次 `0x2db`）\n",
                         machine.font_build_requests(Branch::NonDestroy));
            return fail("每次进入口都该发一次参考量度请求");
        }
        // ⑤ 对位模式 = 2（既不是 0 也不是 1）⇒ 派生**必须**跑（判据是"是否为 0"）。
        auto bytes2 = make_script_with_body();
        emit_instruction(bytes2, 0x1A5, {{5, 0}});
        emit_instruction(bytes2, 0x2DB, {{0, 2}});
        emit_instruction(bytes2, 2, {});
        core::Machine other(key, table);
        Probe other_probe;
        other_probe.attach(other);
        other.write_text(core::Operand{5, 0}, "Amayui CN");
        other.load_root(bytes2);
        if (!other.step() || !other.step()) return fail("（对位模式 2）夹具停机了");
        if (other.font_description(false, 0).width != -7) {
            std::fprintf(stderr, "对位模式 2 之后下一格 = %d（期望 −7）\n",
                         other.font_description(false, 0).width);
            return fail("对位模式 = 2 时没跑派生（判据被写成了「是否 = 1」）");
        }
        if (other.font_measure_requests() != 4) {
            return fail("对位模式 = 2 时的派生度量数不是 4（两份各一次）");
        }
    }
    // ⑤ B 支（销毁旧句柄那一套）：三枚旧引用 + 只横排一份派生 + 自己的那一张文本度量。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2BD, {{0, 1}});   // 粗细 700
        emit_instruction(bytes, 0x75, {{0, 20}});   // A 支字号 20
        emit_instruction(bytes, 0x1A5, {{5, 0}});   // A 支名字
        emit_instruction(bytes, 0x2DB, {{0, 1}});   // 对位模式 1 ⇒ A 支入口派生两份（+4）
        emit_instruction(bytes, 0x197, {{0, 10}});  // B 支字号 10（名字还空 ⇒ 入口早退）
        emit_instruction(bytes, 0x2FE, {{5, 1}});   // B 支名字 ⇒ 销毁那一套入口
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        Probe probe;
        probe.attach(machine);
        machine.write_text(core::Operand{5, 0}, "FACE A");
        machine.write_text(core::Operand{5, 1}, "FACE B");
        machine.load_root(bytes);
        for (int i = 0; i < 4; ++i) {
            if (!machine.step()) return fail("B 支夹具的前置指令停机了");
        }
        // 走到 `0x197` 之前：A 支入口跑过一次（+1 请求、+4 度量）。
        const auto a_text_before = machine.font_text_metric(Branch::NonDestroy);
        const auto a_glyph_before = machine.font_glyph_metric(Branch::NonDestroy);
        const auto a_builds_before = machine.font_build_requests(Branch::NonDestroy);
        if (!machine.step()) return fail("0x197(10) 停机了");
        if (machine.font_build_requests(Branch::Destroy) != 0 ||
            !machine.font_lifecycle_log().empty()) {
            return fail("B 支名字还空着时销毁那一套就动旧引用了（空名早退没生效）");
        }
        if (machine.font_description(true, 0).height != -10 ||
            machine.font_description(true, 0).width != -5) {
            return fail("`0x197` 的入口体没有把 B 支两份描述的高 / 下一格写成 −字号 / −字号÷2");
        }
        if (!machine.step()) return fail("0x2fe（B 支名字）停机了");
        // 三枚旧引用 + 换回表面字体（次序是合同：先换回、再销毁）。
        const auto& log = machine.font_lifecycle_log();
        if (log.size() != 4 || log[0] != Lifecycle::SurfaceRestore ||
            log[1] != Lifecycle::ReleaseHandle || log[2] != Lifecycle::ReleaseHandle ||
            log[3] != Lifecycle::ReleaseHandle) {
            std::fprintf(stderr, "生命周期日志长度 = %zu（期望 4 = 换回表面 + 三枚销毁）\n", log.size());
            return fail("销毁那一套没有『先把绘制表面换回原字体、再销毁三枚旧引用』（或次序反了）");
        }
        // 只对**横排那一份**派生：度量 +2（⛔ 不是 +4），竖排那一份保持入口体写的 −5。
        if (machine.font_measure_requests() != 6) {
            std::fprintf(stderr, "B 支入口之后的度量总数 = %zu（期望 6 = A 支 4 + B 支 2）\n",
                         machine.font_measure_requests());
            return fail("B 支入口不是「只对横排那一份跑一次派生」（两份都跑了？）");
        }
        if (machine.font_description(true, 0).width != -7) {
            std::fprintf(stderr, "B 支横排那一份的下一格 = %d（期望 −7 = −测得平均字宽）\n",
                         machine.font_description(true, 0).width);
            return fail("B 支横排那一份没有跑派生");
        }
        if (machine.font_description(true, 1).width != -5) {
            std::fprintf(stderr, "B 支竖排那一份的下一格 = %d（期望保持 −5 = −字号÷2）\n",
                         machine.font_description(true, 1).width);
            return fail("B 支的竖排那一份也参与了派生（正文：不参与）");
        }
        const auto& b_spec = machine.font_build_spec(Branch::Destroy);
        if (b_spec.face != "FACE B" || b_spec.height != -10 || b_spec.width != -5) {
            std::fprintf(stderr, "销毁那一套建的字体形态 = {面名 %s, 高 %d, 宽 %d}\n",
                         b_spec.face.c_str(), b_spec.height, b_spec.width);
            return fail("销毁那一套建的不是**它自己的**新字体（面名 / 高 / 宽 形态不对）");
        }
        if (b_spec.weight != 700 || b_spec.charset != 1) {
            return fail("销毁那一套的粗细 / 字符集形态不对");
        }
        // 两套的量度**不落同一张结构**：B 支只写文本那一张，A 支那两张一格不动。
        if (!same_text_metric(machine.font_text_metric(Branch::Destroy),
                              core::FontTextMetric{100, 17, 8})) {
            return fail("销毁那一套没有把新建字体的文本度量写进它自己的那一张结构");
        }
        if (machine.font_glyph_metric(Branch::Destroy).box_width != 0) {
            return fail("销毁那一套也取了字形度量（正文：只取一次文本度量）");
        }
        if (!same_text_metric(machine.font_text_metric(Branch::NonDestroy), a_text_before) ||
            machine.font_glyph_metric(Branch::NonDestroy).box_width != a_glyph_before.box_width) {
            return fail("销毁那一套改了**不销毁那一套**的量度结构（两套的量度被当成同一份缓存了）");
        }
        if (machine.font_build_requests(Branch::NonDestroy) != a_builds_before) {
            return fail("B 支入口给 A 支计了一次请求（两支的计数被合并了）");
        }
        // ⑥ 两个名字格不同格：改 B 支的名字格不碰 A 支的描述。
        if (machine.font_description(false, 0).face != "FACE A" ||
            machine.font_description(false, 1).face != "@FACE A") {
            return fail("改 B 支的名字格把 A 支的字面名也改了（两个名字格被合并了）");
        }
        if (machine.font_description(true, 0).face != "FACE B" ||
            machine.font_description(true, 1).face != "@FACE B") {
            return fail("B 支的两份描述不是「原样 / '@' + 原样」两个变体");
        }
    }
    return true;
}

// ── `0x2de` 在**宿主字体替换**生效时（`SCOPE.md` §0 D1 / §1.1）──
// 正文（`CORE-OPS-TEXT-FONT-NAME-2DE`）写的是"在**系统字体名一览**里查这个名字：命中写下标、未命中写 −1"。
// 本仓的宿主简化把整张表换成**一项**：下标 0 = 宿主族名（`Amayui CN`）、标志字节 = 0
// ⇒ **任何非空名字**都解析到宿主字体（写回 **0**），空名字仍按正文的未命中写 **−1**。
// ★ 旧口径「表空 ⇒ 查找恒失败 ⇒ `0x2de` 恒 −1」**已作废**：它不是"名字解析到宿主字体"，而是
//   "引擎的选择/枚举成分整个不见了"。用户 2026-11-01 的裁决是**任意写都路由到 Amayui CN**（SCOPE §0 D1）。
// 判据（每条都能红）：
//   ① 产品装配 `core::wire_platform_fonts` 之后：`font_substitution()` 为真，名字表**恰好一项**、
//      下标 0 的族名 == `host_font_family()`、标志字节 == 0。
//      ★ 把 `set_font_substitution` 里装表那一步删掉 ⇒ 表为空 ⇒ ① 红（"表空、查找恒失败"正是要防的旧形态）。
//   ② `0x2de` 传**三个不同的名字**（宿主族名 / 真机表里有的 `ＭＳ ゴシック` / 一个**不存在**的名字）
//      ⇒ 落点都写回 **0**。★ 把 `find_font_name` 改回"恒 −1" ⇒ ② 红（这是本用例的主判据）。
//   ③ **空名字** ⇒ 按正文写 **−1**（表里没有空名项；⛔ 不要把"任意名字"扩成"含空名字"）。
//   ④ 负控制（替换**关闭** + 一张只含 `ＭＳ ゴシック` 的字面表）⇒ 另外两个名字写 −1、表里那个写 0：
//      证明 ② 的 0 来自宿主替换，而不是"落点观察不到写入"或"表里恰好有这些名字"。
//   ⑤ 渲染相关逻辑**不额外跳转**（SCOPE §0 D1 的「⛔ 不额外跳转渲染相关逻辑」）：
//      替换生效时 `font_fixed_half_width(40)` 的**两条派发点**都返回 0（标志字节 0 ⇒ 位 0 未置起），
//      且一次派生仍发**两次**度量、写回值取自第二次、**不回退**（名字能解析 + 位 0 未置 ⇒ 回退判据②③
//      判不出来 ⇒ 下一格是 −平均字宽，⛔ 不是 −字号/2）。★ 把宿主项的标志字节写成 1 ⇒ ⑤ 红。
bool check_font_name_host_index(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "font-name-host-index: %s\n", what);
        return false;
    };
    // 真机字体名表里确实有的一项（`ＭＳ ゴシック` 的 CP932 字节 = 引擎字符串口径）。
    const std::string ms_gothic = "\x82\x6C\x82\x72\x20\x83\x53\x83\x56\x83\x62\x83\x4E";
    const std::string absent = "No Such Face";
    // 一次 `0x2de`：第 2 个操作数 = 名字、第 1 个 = 落点（全局整型槽 0）。
    const auto query = [](core::Machine& machine, const std::string& name) -> std::int32_t {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2DE, {{3, 0}, {5, 0}});
        emit_instruction(bytes, 2, {});
        core::Machine probe = machine;  // 表 / 替换开关随拷贝走
        probe.write_text(core::Operand{5, 0}, name);
        probe.load_root(bytes);
        if (!probe.step()) return -12345;
        return probe.global_int(0);
    };
    const auto expect_query = [&](core::Machine& machine, const std::string& name,
                                  std::int32_t want, const char* what) {
        const std::int32_t got = query(machine, name);
        if (got != want) {
            std::fprintf(stderr, "0x2de(\"%s\") = %d（期望 %d）\n", name.c_str(), got, want);
            return fail(what);
        }
        return true;
    };
    core::Machine machine(key, table);
    // ① 产品装配 ⇒ 开关打开 + 名字表恰好一项（下标 0 = 宿主族名、标志字节 0）。
    core::wire_platform_fonts(machine);
    if (!machine.font_substitution()) {
        return fail("core::wire_platform_fonts 没有打开宿主字体替换开关（SCOPE §0 D1）");
    }
    if (machine.font_name_count() != 1) {
        std::fprintf(stderr, "替换生效后名字表项数 = %zu（期望 1 = 单项宿主字体）\n",
                     machine.font_name_count());
        return fail("替换生效时名字表不是「单项 = 宿主字体」（表空 ⇒ 查找恒失败，正是作废的旧口径）");
    }
    const core::Machine::FontNameEntry* host = machine.font_name_entry(0);
    if (host == nullptr || host->name != core::Machine::host_font_family()) {
        std::fprintf(stderr, "表项 0 的族名 = %s（期望 %s）\n",
                     host == nullptr ? "<无>" : host->name.c_str(),
                     core::Machine::host_font_family().c_str());
        return fail("名字表的主机字体项不在下标 0 / 族名不是宿主族名");
    }
    if (host->flags != 0) {
        std::fprintf(stderr, "表项 0 的标志字节 = %d（期望 0）\n", host->flags);
        return fail("主机字体项的标志字节不是 0（位 0 = 等宽 ⇒ 渲染路径会被这条表项改掉）");
    }
    // ② 三个不同的名字（含一个不存在的名字）⇒ 都写回宿主下标 0。
    if (!expect_query(machine, core::Machine::host_font_family(), 0,
                      "0x2de 查宿主族名没有写回 0")) {
        return false;
    }
    if (!expect_query(machine, ms_gothic, 0,
                      "0x2de 查一个真机表里有的名字没有写回 0（名字解析没路由到宿主字体）")) {
        return false;
    }
    if (!expect_query(machine, absent, 0,
                      "0x2de 查一个不存在的名字没有写回 0（任意写都该路由到宿主字体；恒 −1 ⇒ 本条红）")) {
        return false;
    }
    // ③ 空名字 ⇒ 正文的未命中（−1）。
    if (!expect_query(machine, std::string(), -1,
                      "0x2de 查空名字没有写回 −1（正文的未命中语义没有例外）")) {
        return false;
    }
    // ④ 负控制：替换关闭 + 只含 `ＭＳ ゴシック` 的字面表 ⇒ 同一批查询判得出命中与未命中。
    {
        core::Machine literal(key, table);
        literal.set_font_name_table({{ms_gothic, 0}});
        if (literal.font_substitution()) return fail("负控制的前置不成立：替换开关本来是开的");
        if (!expect_query(literal, core::Machine::host_font_family(), -1,
                          "替换关闭时查宿主族名居然命中了（负控制不成立）")) {
            return false;
        }
        if (!expect_query(literal, absent, -1, "替换关闭时查不存在的名字居然命中了（负控制不成立）")) {
            return false;
        }
        if (!expect_query(literal, ms_gothic, 0,
                          "替换关闭时表里有的名字没命中（负控制不成立 ⇒ ④ 断言不到东西）")) {
            return false;
        }
    }
    // ⑤ 渲染相关逻辑不额外跳转：固定半宽两条路径都返回 0；派生两次度量、写回第二次、不回退。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x2DB, {{0, 1}});  // 对位模式 = 1（非 0 ⇒ 派生例程会被调用）
        emit_instruction(bytes, 0x1A5, {{5, 0}});  // A 支字面名 = 一个**不在真机表里**的名字
        emit_instruction(bytes, 2, {});
        core::Machine draw(key, table);
        core::wire_platform_fonts(draw);
        struct Probe {
            std::vector<std::string> faces;
            void attach(core::Machine& machine) {
                machine.set_font_measurer([this](const std::string& face, std::int32_t) {
                    faces.push_back(face);
                    core::Machine::FontMetrics metrics;
                    metrics.line_height = 18;  // < 字号 20 ⇒ 不触发高度缩放
                    metrics.average_width = face == core::Machine::reference_font_face() ? 99 : 13;
                    metrics.glyph_box_width = 0;
                    metrics.measured_box_width = 0;
                    return metrics;
                });
            }
        };
        Probe probe;
        probe.attach(draw);
        draw.write_text(core::Operand{5, 0}, absent);
        draw.load_root(bytes);
        if (!draw.step() || !draw.step()) return fail("⑤ 的 0x2db / 0x1a5 夹具停机了");
        // 固定半宽：两条派发点的判据不同，但**标志字节 0 ⇒ 两条都返回 0**（⛔ 不是"跳过这个字体"）。
        const auto platform_half =
            draw.font_fixed_half_width(40, core::Machine::HalfWidthPath::PlatformFont);
        const auto metric_half =
            draw.font_fixed_half_width(40, core::Machine::HalfWidthPath::DescriptionMetric);
        if (platform_half != 0 || metric_half != 0) {
            std::fprintf(stderr, "固定半宽：画字 = %d / 量度格 = %d（都期望 0）\n", platform_half,
                         metric_half);
            return fail("主机字体项让固定半宽生效了（标志字节不是 0 ⇒ 渲染路径被改了）");
        }
        core::FontDescription desc;
        desc.face = absent;  // 名字不在真机表里，但替换生效 ⇒ 解析得到 ⇒ 不回退
        const std::size_t before = draw.font_measure_requests();
        draw.derive_font_description(desc, 20);
        if (draw.font_measure_requests() - before != 2) {
            std::fprintf(stderr, "一次派生的度量增量 = %zu（期望 2）\n",
                         draw.font_measure_requests() - before);
            return fail("替换生效后派生不再发两次度量（两次度量保留这一条被动了）");
        }
        if (desc.width != -13) {
            std::fprintf(stderr, "派生的下一格 = %d（期望 −13 = −测得平均字宽；−10 ⇒ 走了回退）\n",
                         desc.width);
            return fail("名字能解析 + 标志字节 0 时仍然回退了（回退判据②③ 的行为变了）");
        }
        if (probe.faces.size() < 2 || probe.faces[0] != core::Machine::reference_font_face() ||
            probe.faces.back() != absent) {
            std::fprintf(stderr, "两次请求的面名数 = %zu\n", probe.faces.size());
            return fail("两次度量的请求面名不是「参考字体常量 → 该描述自己的字面名」");
        }
    }
    return true;
}
// ── 数组地址算术的 32 位回绕（`CORE-OPS-ARRAY-LOOKUP` / `-2D`；正文 `60-ops-array.md:84` / `:110`） ──
// 正文原句 → 机械判据：
//   · 「地址算术是 **32 位回绕**：`取址 + 宽度 × 下标` 只保留低 32 位」（`:84`）⇒
//     基址 0xFFFFF000 + 4×0x400 必须落回 0x00000000（写成 64 位 / 被钳住都会读出别的值）；
//   · 「元素宽度由**落点的类型标签**决定」（`:61-67`）⇒ 落点 tag 6 步长 4、tag 8 / 14 步长 28
//     （写死 4 ⇒ 后两条红）；
//   · 「落点是值族…时抛**类型异常**，不是静默写错池」（`:70`）⇒ 停机原因是 Error、错误文本是类型异常，
//     且落点那一格**一个字节都没动**；
//   · 2D：「下标 = **操作数 3 × 操作数 4 + 操作数 5**（先乘后加）」（`:92`）+「也按 **32 位回绕**算」（`:110`）⇒
//     行 2 / 行宽 10 / 列 3 必须与 `0x61` 下标 23 的地址相同（≠ 下标 26），
//     行 0x10000 × 行宽 0x10000 的乘积必须回绕成下标 0。
bool check_array_lookup_address_wrap(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "array-lookup-address-wrap: %s\n", what);
        return false;
    };
    // ① 32 位回绕：0xFFFFF000 + 4 × 0x400 = 0x1_0000_0000 ⇒ 只留低 32 位 = 0。
    {
        core::Machine machine(key, table);
        machine.seed_pointer_for_test(6, 0, 0xFFFFF000u);   // 数组基址放在引用族槽 {6,0}
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x61, {{6, 1}, {6, 0}, {0, 0x400}});
        emit_instruction(bytes, 2, {});
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x61 停机了");
        const auto address = machine.pointer_slot(6, 1);
        if (address != 0u) {
            std::fprintf(stderr, "0x61 基址 0xFFFFF000 + 4×0x400 = 0x%08X（期望 0x00000000）\n", address);
            return fail("地址算术没有按 32 位回绕（或被钳在基址附近）");
        }
    }
    // ② 元素宽度来自**落点标签**：tag 6 ⇒ 4；tag 8 / 14（全局 / 局部指针）⇒ 28。
    {
        core::Machine machine(key, table);
        machine.seed_pointer_for_test(6, 0, 0x20000000u);
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x61, {{6, 1}, {6, 0}, {0, 3}});    // 落点 = 局部指针 ⇒ 步长 4
        emit_instruction(bytes, 0x61, {{8, 0}, {6, 0}, {0, 3}});    // 落点 = 全局指针 ⇒ 步长 28
        emit_instruction(bytes, 0x61, {{14, 0}, {6, 0}, {0, 3}});   // 落点 = 局部指针（tag 14）⇒ 28
        emit_instruction(bytes, 2, {});
        machine.load_root(bytes);
        for (int i = 0; i < 3; ++i) {
            if (!machine.step()) return fail("0x61 步长用例停机了");
        }
        if (machine.pointer_slot(6, 1) != 0x2000000Cu) {
            std::fprintf(stderr, "落点 tag 6 的地址 = 0x%08X（期望 0x2000000C = 基址 + 4×3）\n",
                         machine.pointer_slot(6, 1));
            return fail("落点族 6 的步长不是 4");
        }
        if (machine.pointer_slot(8, 0) != 0x20000054u) {
            std::fprintf(stderr, "落点 tag 8 的地址 = 0x%08X（期望 0x20000054 = 基址 + 28×3）\n",
                         machine.pointer_slot(8, 0));
            return fail("落点族 8 的步长不是 28（按 4 算了？）");
        }
        if (machine.pointer_slot(14, 0) != 0x20000054u) {
            std::fprintf(stderr, "落点 tag 14 的地址 = 0x%08X（期望 0x20000054）\n",
                         machine.pointer_slot(14, 0));
            return fail("落点族 14 的步长不是 28（按 4 算了？）");
        }
    }
    // ③ 落点是值族 ⇒ 类型异常，落点一格未写（局部整型槽预置 42）。
    {
        core::Machine machine(key, table);
        machine.seed_pointer_for_test(6, 0, 0x20000000u);
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x55, {{9, 0}, {0, 42}});   // 落点预置 42
        emit_instruction(bytes, 0x61, {{9, 0}, {6, 0}, {0, 1}});
        emit_instruction(bytes, 2, {});
        machine.load_root(bytes);
        if (!machine.step()) return fail("落点预置的 0x55 停机了");
        if (machine.step()) return fail("0x61 落点是值族却没有停机");
        if (machine.last_stop().reason != core::StopReason::Error) {
            return fail("落点是值族时的停机原因不是异常档");
        }
        if (machine.last_stop().message.rfind("lookup-array-destination-not-reference-tag", 0) != 0) {
            std::fprintf(stderr, "错误文本：%s\n", machine.last_stop().message.c_str());
            return fail("落点是值族时的错误文本不是类型异常那一档");
        }
        if (machine.read_int(core::Operand{9, 0}) != 42) {
            std::fprintf(stderr, "落点被改了：%d（期望仍是 42）\n",
                         machine.read_int(core::Operand{9, 0}));
            return fail("类型异常之前就写了落点（既不是抛，也不是静默写错池）");
        }
        if (machine.pointer_slot(6, 0) != 0x20000000u) return fail("类型异常改动了数组基址槽");
    }
    // ④ 2D 的次序：行 2 × 行宽 10 + 列 3 = 23 ⇒ 与 `0x61` 下标 23 同址（先乘后加；写成 2×(10+3)=26 会不同）。
    {
        core::Machine machine(key, table);
        machine.seed_pointer_for_test(6, 0, 0x20000000u);
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x12c, {{6, 1}, {6, 0}, {0, 2}, {0, 10}, {0, 3}});
        emit_instruction(bytes, 0x61, {{6, 2}, {6, 0}, {0, 23}});
        emit_instruction(bytes, 2, {});
        machine.load_root(bytes);
        if (!machine.step() || !machine.step()) return fail("0x12c / 0x61 停机了");
        const auto two_d = machine.pointer_slot(6, 1);
        const auto flat = machine.pointer_slot(6, 2);
        const auto wrong = 0x20000000u + 4u * 26u;   // 行 ×(行宽 + 列) 会落到这里
        if (two_d != flat || two_d == wrong) {
            std::fprintf(stderr, "0x12c 地址 = 0x%08X；0x61 下标 23 = 0x%08X；次序写反 = 0x%08X\n",
                         two_d, flat, wrong);
            return fail("2D 下标不是先乘后加（或没按 32 位算）");
        }
        if (two_d != 0x20000000u + 4u * 23u) {
            std::fprintf(stderr, "0x12c 地址 = 0x%08X（期望 0x%08X）\n", two_d, 0x20000000u + 92u);
            return fail("2D 地址算术不对");
        }
    }
    // ⑤ 2D 下标也按 32 位回绕：行 0x10000 × 行宽 0x10000 = 0x1_0000_0000 ⇒ 下标 0 ⇒ 地址 = 基址。
    {
        core::Machine machine(key, table);
        machine.seed_pointer_for_test(6, 0, 0x20000000u);
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x12c, {{6, 1}, {6, 0}, {0, 0x10000}, {0, 0x10000}, {0, 0}});
        emit_instruction(bytes, 2, {});
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x12c 回绕用例停机了");
        if (machine.pointer_slot(6, 1) != 0x20000000u) {
            std::fprintf(stderr, "0x12c 行×行宽 回绕后地址 = 0x%08X（期望 0x20000000）\n",
                         machine.pointer_slot(6, 1));
            return fail("2D 下标的乘积没有按 32 位回绕");
        }
    }
    return true;
}

// ── 数值族 `0x52`..`0x5f`（`CORE-OPS-NUMERIC-ARITH` / `-COMPARE`；正文 `60-ops-numeric.md:103-167`） ──
// 正文原句 → 机械判据（逐行独立，左 = 输入，右 = **精确值**）：
//   · 「乘法按 32 位截断」（`:119`）⇒ `0x52(0x10000,0x10000)` = 0；
//   · 「商向零截断（−7 ÷ 2 = −3）」（`:120`）⇒ `0x53`；「余数的符号跟随被除数」⇒ `0x54`；
//   · 「除数为 0 时引擎不做检查…中止发生在**写第 1 个操作数之前** ⇒ 落点保持原值」（`:120`）⇒
//     `0x53(1,0)` 之后落点仍是预置的 7，停机原因是异常、错误文本是除零；
//   · 「位数取第 3 个操作数的低 5 位…⛔ 不许 clamp 到 31」（`:122`）⇒ `0x59(1,63)` = 0x80000000、
//     `0x58(−1,33)` = −1；「0x58 是符号扩展右移」（`:123`）⇒ `0x58(−8,1)` = −4；
//   · 「按位与 / 或不对符号做任何解释」（`:124`）⇒ `0x57(−1,0)` = −1、`0x56(−1,0)` = 0；
//   · 「大小比较是**有符号**的：−1 < 0 为真」（`:153`）⇒ `0x5f(−1,0)` = 0、`0x5c(−1,0)` = 1；
//   · 「方向是第 2 个 对 第 3 个；结果只有 0 / 1」（`:142` / `:154-155`）⇒ 六条在 (5,2) 上的精确值。
bool check_numeric_family_52_5f(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "numeric-family-52-5f: %s\n", what);
        return false;
    };
    struct CellObs {
        bool ran = false;            // 被测那一步是否正常返回（false = 停机 / 抛）
        std::int32_t value = 0;      // 局部整型槽 0 的读回值（= 写进第 1 个操作数的值）
        core::StopReason reason = core::StopReason::Running;
        std::string message;
    };
    // 跑一段脚本（`steps` 条指令），回读停机状态 + 落点值。
    const auto run_cell = [&](const std::vector<std::uint8_t>& bytes, std::size_t steps) {
        core::Machine machine(key, table);
        machine.load_root(bytes);
        CellObs obs;
        for (std::size_t i = 0; i < steps; ++i) {
            obs.ran = machine.step();
            if (!obs.ran) break;
        }
        obs.value = machine.read_int(core::Operand{9, 0});
        obs.reason = machine.last_stop().reason;
        obs.message = machine.last_stop().message;
        return obs;
    };
    // 一条被测指令：落点 = 局部整型槽 0。
    const auto one = [](std::uint32_t opcode,
                        std::initializer_list<std::pair<std::uint32_t, std::uint32_t>> operands) {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, opcode, operands);
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    struct Case {
        std::uint32_t opcode;
        std::uint32_t a;
        std::uint32_t b;
        std::int32_t expected;
        const char* what;
    };
    const Case cases[] = {
        {0x52, 0x10000u, 0x10000u, 0, "0x52 乘法没有按 32 位截断（0x10000 平方 = 0）"},
        {0x53, 0xFFFFFFF9u, 2u, -3, "0x53 商没有向零截断（−7 ÷ 2 = −3，不是 −4）"},
        {0x54, 0xFFFFFFF9u, 2u, -1, "0x54 余数的符号没有跟随被除数（−7 mod 2 = −1）"},
        {0x54, 7u, 0xFFFFFFFEu, 1, "0x54 余数在除数为负时不是 1（7 mod −2 = 1）"},
        {0x58, 0xFFFFFFF8u, 1u, -4, "0x58 不是算术右移（−8 >> 1 = −4，不是 2147483644）"},
        {0x58, 0xFFFFFFFFu, 33u, -1, "0x58 的位数没有取低 5 位（−1 >> 33 = −1）"},
        {0x59, 1u, 33u, 2, "0x59 的位数没有取低 5 位（1 << 33 = 2）"},
        {0x59, 1u, 63u, static_cast<std::int32_t>(0x80000000u),
         "0x59 把位数 clamp 到 31 了（1 << 63 = 0x80000000）"},
        {0x56, 0xFFFFFFFFu, 0u, 0, "0x56 的按位与把符号当真了（−1 & 0 = 0）"},
        {0x57, 0xFFFFFFFFu, 0u, -1, "0x57 的按位或把符号当真了（−1 | 0 = −1）"},
        // 六条比较：方向 = 第 2 个 对 第 3 个（a = 5、b = 2），真 = 1 / 假 = 0（⛔ 不是 −1）。
        {0x5a, 5u, 2u, 0, "0x5a 相等比较错（5 = 2 为假）"},
        {0x5b, 5u, 2u, 1, "0x5b 不等比较错（5 ≠ 2 为真）"},
        {0x5c, 5u, 2u, 0, "0x5c 比较方向反了（5 < 2 为假）"},
        {0x5d, 5u, 2u, 0, "0x5d 比较方向反了（5 ≤ 2 为假）"},
        {0x5e, 5u, 2u, 1, "0x5e 比较方向反了（5 > 2 为真）"},
        {0x5f, 5u, 2u, 1, "0x5f 比较方向反了（5 ≥ 2 为真）"},
        {0x5c, 2u, 5u, 1, "0x5c 与 0x5e 不是彼此的镜像（2 < 5 为真）"},
        {0x5e, 2u, 5u, 0, "0x5e 与 0x5c 不是彼此的镜像（2 > 5 为假）"},
        // 有符号：−1 < 0 为真（按无符号比较会把 −1 判成最大的数）。
        {0x5c, 0xFFFFFFFFu, 0u, 1, "0x5c 用了无符号比较（−1 < 0 为真）"},
        {0x5f, 0xFFFFFFFFu, 0u, 0, "0x5f 用了无符号比较（−1 ≥ 0 为假）"},
    };
    for (const auto& c : cases) {
        const auto obs = run_cell(one(c.opcode, {{9, 0}, {0, c.a}, {0, c.b}}), 1);
        if (!obs.ran) {
            std::fprintf(stderr, "%s：被测指令停机了（%s）\n", c.what, obs.message.c_str());
            return fail("数值族指令不该有失败路径（除零那一档单独测）");
        }
        if (obs.value != c.expected) {
            std::fprintf(stderr, "%s：得到 %d（期望 %d）\n", c.what, obs.value, c.expected);
            return fail(c.what);
        }
    }
    // 除零：落点预置 7 ⇒ 中止发生在写第 1 个操作数之前 ⇒ 落点仍是 7；停机原因是异常，文本是除零。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x55, {{9, 0}, {0, 7}});
        emit_instruction(bytes, 0x53, {{9, 0}, {0, 1}, {0, 0}});
        emit_instruction(bytes, 2, {});
        const auto obs = run_cell(bytes, 2);
        if (obs.ran) return fail("0x53 除数为 0 却没有中止");
        if (obs.reason != core::StopReason::Error) return fail("0x53 除零的停机原因不是异常档");
        if (obs.message.rfind("divide-by-zero", 0) != 0) {
            std::fprintf(stderr, "错误文本：%s\n", obs.message.c_str());
            return fail("0x53 除零的错误文本不对");
        }
        if (obs.value != 7) {
            std::fprintf(stderr, "0x53 除零之后落点 = %d（期望仍是 7）\n", obs.value);
            return fail("除零中止之前就写了落点");
        }
    }
    // 0x54 除零同样在写落点之前中止（同一条正文句子的另一半）。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x55, {{9, 0}, {0, 7}});
        emit_instruction(bytes, 0x54, {{9, 0}, {0, 1}, {0, 0}});
        emit_instruction(bytes, 2, {});
        const auto obs = run_cell(bytes, 2);
        if (obs.ran) return fail("0x54 除数为 0 却没有中止");
        if (obs.reason != core::StopReason::Error) return fail("0x54 除零的停机原因不是异常档");
        if (obs.message.rfind("mod-by-zero", 0) != 0) {
            std::fprintf(stderr, "错误文本：%s\n", obs.message.c_str());
            return fail("0x54 除零的错误文本不对");
        }
        if (obs.value != 7) return fail("0x54 除零中止之前就写了落点");
    }
    return true;
}

// ── `0x205` 的格式化与横向落点（`CORE-OPS-RENDER-NUMTEXT-205`；正文 `60-ops-render.md:205-218`）──
// ★ 为什么单列一条：`emulator-core-render-prims` 的 0x205 段是**差分像素** —— 把期望串用 `0x204`
//   画在同一后端的同一画布上，再要求两边整块像素相同。那是**投影一致**：两侧共用同一套字形映射时，
//   共同的错会一起相等；而且它整段包在"有没有字体后端"里，没有后端就判不了。这条用例直接断言
//   **格式化出的字节串**与**算出的 x**，两种病都堵得上，且**不依赖字体后端**。
// 正文原句 → 机械判据（左 = 输入，右 = 精确值）：
//   · 「数字位数超过可用数字格 ⇒ 丢掉高位数字」(:208)：2 格 12345 ⇒ `45`；
//   · 「横向位置 = 第 2 个操作数 + 未用格数 × 格宽 × k，算术都是整除、向零截断」(:212)；
//   · 「右齐（缺省）⇒ 全角 未用格数 × 格宽、半角 ÷ 2；居中（0x2）⇒ 全角 ÷ 2、半角 ÷ 4；
//     左齐（0x4 且未置 0x2）⇒ 0」(:215)：4 格 1 位、格宽 20 ⇒ 30 / 15 / 0；
//   · 「引擎**先乘后除**」（`0x4074c0` / `0x407483`）⇒ 4 格 77、格宽 15 ⇒ 2×15×2÷4 = 15（先除半得 7）；
//   · 「`#` 的唯一入口：有符号且格数 < 2」(:209)：1 格 / 0 格 + 0x8 ⇒ `#`；
//   · 「无符号且格数 ≤ 0 ⇒ 空串（不画、不报错）」(:209)：格数 0 ⇒ ``；
//   · 「0x10 优先于 0x20」(:206) ⇒ 零 + 0x10|0x20 ⇒ `+0`；「0x10 / 0x20 只对零」(:235) ⇒ 5 + 0x10 ⇒ `5`；
//   · 「负值一律 `-`（即使置了 0x8）」(:235) ⇒ −5 + 0x8 ⇒ `-5`；
//   · 「0x1 ⇒ 数字补零补满可用数字格」(:207) ⇒ 3 格 7 ⇒ `007`；
//   · 「不置 0x10000 ⇒ 每个字符就地扩成全角」(:210) ⇒ 77 ⇒ 0x8256 0x8256。
bool check_numtext_layout(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "numtext-layout-0x205: %s\n", what);
        return false;
    };
    // ── ① 表驱动：纯函数 `layout_num_text(value, cells, flags, anchor_x, cell_width)` ──
    struct Row {
        std::int32_t value;
        std::int32_t cells;
        std::int32_t flags;
        std::int32_t anchor_x;
        int cell_width;
        const char* ascii;  // 期望格式化串（ASCII）
        int chars;          // 期望已写字符数
        int x;              // 期望横向落点
        const char* sjis;   // 期望最终字节串；nullptr = 只判"半角档 sjis == ascii"
        const char* what;   // 失败时说的话（这条在防什么坏）
    };
    const Row rows[] = {
        {12345, 2, 0x10000, 0, 20, "45", 2, 0, nullptr,
         "2 格 12345 应当丢高位只留 \"45\"（⛔ 不是 \"#\"、也不是 \"12\""},
        {7, 4, 0x10000, 0, 20, "7", 1, 30, nullptr,
         "右齐（缺省）+ 半角：4 格 1 位 ⇒ 3×20×2÷4 = 30"},
        {7, 4, 0x10000 | 0x2, 0, 20, "7", 1, 15, nullptr,
         "居中（0x2）+ 半角：3×20×1÷4 = 15"},
        {7, 4, 0x10000 | 0x4, 0, 20, "7", 1, 0, nullptr,
         "左齐（0x4）：x 不动（⛔ 不是半格 / 全格）"},
        {7, 4, 0x10000 | 0x2 | 0x4, 0, 20, "7", 1, 15, nullptr,
         "0x2 与 0x4 同时置位 ⇒ 按居中（0x2 优先）"},
        {77, 4, 0x10000, 0, 15, "77", 2, 15, nullptr,
         "奇数格宽先乘后除：2×15×2÷4 = 15（先把格宽除半会得 7）"},
        {7, 1, 0x8, 0, 20, "#", 1, 0, "\x81\x94",
         "有符号且格数 = 1 ⇒ 单个 '#'（全角 0x8194），未用格数夹到 0"},
        {7, 0, 0x8, 0, 20, "#", 1, 0, nullptr,
         "有符号且格数 = 0 ⇒ 单个 '#'（未用格数不是负数）"},
        {7, 0, 0, 0, 20, "", 0, 0, "",
         "无符号且格数 = 0 ⇒ 空串（不画、不报错）"},
        {0, 2, 0x10 | 0x20, 0, 20, "+0", 2, 0, "\x81\x7B\x82\x4F",
         "零 + 0x10|0x20 ⇒ '+'（0x10 优先），全角 0x817B 0x824F"},
        // ★ 下面两行单独钉住两个位各自的语义：只有它们在场，"把 0x10 与 0x20 的位对调"才会红
        //   （两位同时置时 '+' 仍成立，单看那一行抓不到位对调）。
        {0, 2, 0x10, 0, 20, "+0", 2, 0, nullptr,
         "零 + 0x10 ⇒ '+'（0x20 才管负号）"},
        {0, 2, 0x20, 0, 20, "-0", 2, 0, "\x81\x7C\x82\x4F",
         "零 + 0x20 ⇒ '-'（全角 0x817C 0x824F）"},
        {5, 3, 0x10, 0, 20, "5", 1, 40, nullptr,
         "0x10 只对零生效：5 仍然只有一位；全角右齐 ⇒ 2×20 = 40"},
        {-5, 3, 0x8, 0, 20, "-5", 2, 20, "\x81\x7C\x82\x54",
         "负值一律 '-'（即使置了 0x8）；未用 1 格 ⇒ 20；全角 0x817C 0x8254"},
        {7, 3, 0x10000 | 0x1, 0, 20, "007", 3, 0, nullptr,
         "0x1 补零补满可用数字格（3 格 ⇒ \"007\"）"},
        {77, 4, 0, 0, 20, "77", 2, 40, "\x82\x56\x82\x56",
         "不置 0x10000 ⇒ 每个字符就地扩成全角（'7' ⇒ 0x8256）；未用 2 格 ⇒ 40"},
    };
    for (const Row& row : rows) {
        const auto layout =
            core::layout_num_text(row.value, row.cells, row.flags, row.anchor_x, row.cell_width);
        if (layout.ascii != row.ascii) {
            std::fprintf(stderr,
                         "value=%d cells=%d flags=0x%X anchor=%d cell_width=%d ⇒ ascii=\"%s\"（期望 \"%s\"）\n",
                         row.value, row.cells, row.flags, row.anchor_x, row.cell_width,
                         layout.ascii.c_str(), row.ascii);
            return fail(row.what);
        }
        if (layout.chars != row.chars) {
            std::fprintf(stderr, "value=%d cells=%d flags=0x%X ⇒ chars=%d（期望 %d）\n", row.value,
                         row.cells, row.flags, layout.chars, row.chars);
            return fail(row.what);
        }
        if (layout.x != row.x) {
            std::fprintf(stderr,
                         "value=%d cells=%d flags=0x%X anchor=%d cell_width=%d ⇒ x=%d（期望 %d）\n",
                         row.value, row.cells, row.flags, row.anchor_x, row.cell_width, layout.x,
                         row.x);
            return fail(row.what);
        }
        if (row.sjis != nullptr) {
            if (layout.sjis != row.sjis) {
                std::fprintf(stderr, "value=%d cells=%d flags=0x%X ⇒ sjis 字节数 %zu（期望 %zu）\n",
                             row.value, row.cells, row.flags, layout.sjis.size(),
                             std::strlen(row.sjis));
                return fail(row.what);
            }
        } else if ((row.flags & 0x10000) != 0 && layout.sjis != layout.ascii) {
            // 半角档：`0x10000` ⇒ 文本按半角 ASCII 落字（正文 :210）
            return fail("置了 0x10000 的档，sjis 必须与 ascii 逐字节相同");
        }
        if ((row.flags & 0x10000) == 0 && layout.sjis.size() != layout.ascii.size() * 2) {
            // 全角档：每个 ASCII 字符 ⇒ 两个 S-JIS 字节（本表只用 0-9 / '-' / '+' / '#'）
            return fail("全角档的 sjis 长度不是 ascii 的两倍（有字符没走全角映射）");
        }
    }
    // ── ② 接线：`op_205` 真的调用纯函数、并把结果存进观察口（参数顺序写反 ⇒ 这里红）──
    // ★ 这一档**不需要纹理、也不需要字体后端**：格数 = 0 ⇒ 空串档 ⇒ 三条入口条件的第一条就挡住绘制。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x205, {{0, 1}, {0, 0}, {0, 10}, {0, 1234}, {0, 0}, {0, 0}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x205 的空串档停机了（不画、不报错）");
        if (machine.halted()) return fail("0x205 的空串档让引擎停了");
        const auto& layout = machine.last_num_text_layout();
        if (!layout.ascii.empty() || layout.chars != 0 || layout.x != 0) {
            std::fprintf(stderr, "空串档 ascii=\"%s\" chars=%d x=%d\n", layout.ascii.c_str(),
                         layout.chars, layout.x);
            return fail("0x205 的空串档没有把 layout 记成空（或观察口压根没写）");
        }
        if (!machine.errors().empty()) return fail("0x205 的空串档写了错误通道");
    }
    // ── ③ 参数顺序 + 格宽来源：op2 = 字段原点、op5 = 格数、op6 = 格式位、op4 = 值；
    //    格宽 = `0x75` 写的 A 支字号（这里 20）⇒ 4 格 7、半角、右齐、原点 10 ⇒ x = 10 + 3×20×2÷4 = 40。
    //    槽里没有纹理对象 ⇒ 不画（同样不需要字体后端），但 layout 必须已经算出来。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x75, {{0, 20}});
        emit_instruction(bytes, 0x205, {{0, 1}, {0, 10}, {0, 10}, {0, 7}, {0, 4}, {0, 0x10000}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x75 停机了（格宽的前置）");
        if (!machine.step()) return fail("槽里没有纹理时 0x205 停机了（三条入口条件之一应静默跳过）");
        if (machine.halted()) return fail("槽里没有纹理时 0x205 让引擎停了");
        const auto& layout = machine.last_num_text_layout();
        if (layout.ascii != "7" || layout.chars != 1 || layout.x != 40 || layout.sjis != "7") {
            std::fprintf(stderr, "接线档 ascii=\"%s\" chars=%d x=%d sjis=%zu 字节\n",
                         layout.ascii.c_str(), layout.chars, layout.x, layout.sjis.size());
            return fail("0x205 的 layout 与它的操作数 / 格宽对不上（参数顺序或格宽来源写错）");
        }
        if (!machine.errors().empty()) return fail("槽里没有纹理时 0x205 写了错误通道");
    }
    // ── ④ 格宽来源 = **字号格本身**，⛔ 没有缺省值：`0x75(0)` 之后 4 格 7、半角、右齐、原点 10 ⇒
    //    x **必须等于 anchor**（未用格数 × 0 = 0）。旧实现给"字号格 ≤ 0 ⇒ 8"的缺省 ⇒ x = 22 ⇒ 本条红。
    //    ★ 这条钉的是「正文没有这个 8」：格宽取字号格，字号格是 0 就乘出 0。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x75, {{0, 0}});
        emit_instruction(bytes, 0x205, {{0, 1}, {0, 10}, {0, 10}, {0, 7}, {0, 4}, {0, 0x10000}});
        emit_instruction(bytes, 2, {});
        core::Machine machine(key, table);
        machine.load_root(bytes);
        if (!machine.step()) return fail("0x75(0) 停机了（字号格 0 的前置）");
        if (!machine.step()) return fail("字号格为 0 时 0x205 停机了");
        const auto& layout = machine.last_num_text_layout();
        if (layout.ascii != "7" || layout.x != 10) {
            std::fprintf(stderr, "字号格 0 ⇒ ascii=\"%s\" x=%d（期望 \"7\" / 10）\n",
                         layout.ascii.c_str(), layout.x);
            return fail("字号格 = 0 时 x 不等于 anchor（给格宽补了一个正文没有的缺省值）");
        }
    }
    return true;
}

// ── 场景绘制模式（`CORE-OPS-RENDER-FRAME-STEP`「场景绘制模式（合同）」）──
// ★ 每条都能被一处破坏弄红：
//   · `0x200` 用操作数**直接覆盖**（不重读配置、不校验范围、不钳）⇒ 第三态 2 写进去就是 2；
//   · `0x200` 的前置 = 配置 `set:CreateObject` 的**该位已置起**（位号 = 操作数，与初始化同口径）⇒
//     未置起时**整条不生效、一个字节都不写**（⛔ 不是"写 0"、也⛔ 不是"范围检查"）；
//   · `0x201` 把该格**原值**交出（⛔ 不归一化）；
//   · 构造期拿该值当位下标：`1 << 值` 那一位未置起 ⇒ 把 `set:CreateObject` 写成 **3**；已置起 ⇒ 不动；
//   · 复位路径（整块清零写 0 + 按同一配置重设）：`0x9` 之后回到**配置值**（覆盖脚本写的第三态）；
//   · 画面暂停闩的两个写点都以「模式 = 1」为前提：模式 ≠ 1 时置 / 清**都不动**。
bool check_draw_mode(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "draw-mode: %s\n", what);
        return false;
    };
    // ① 第三态：`0x200(2)` 之后 `0x201` 读回 **2**（⛔ 不是 1 / 0）；门前渲染步必须**不发出**。
    //    前置位：`set:CreateObject` 的第 2 位（位号 = 操作数 2）。构造期会按 DrawMode = 1 去测第 1 位
    //    ⇒ 先给 2（该位置起 ⇒ 它不改写），构造后再换成"只有第 2 位"的 4。
    {
        core::ConfigStore config;
        config.set_int("set:DrawMode", 1);
        config.set_int("set:CreateObject", 2);
        auto script = make_script_with_body();
        emit_instruction(script, 0x200, {{0, 2}});
        emit_instruction(script, 0x201, {{3, 0}});  // 落到全局整型 0
        emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table, nullptr, config);
        if (machine.draw_mode() != 1) return fail("构造期没有按配置 set:DrawMode 取初始值");
        machine.set_config_int("set:CreateObject", 4);  // 第 2 位（1 << 2）
        machine.set_host_clock_ms(1000);
        machine.load_root(script);
        machine.consume_frame_step();
        if (!machine.step()) return fail("0x200(2) 停机了");
        if (machine.draw_mode() != 2) return fail("0x200 没有用操作数直接覆盖（第三态被归一化 / 钳掉）");
        if (!machine.step()) return fail("0x201 停机了");
        if (machine.global_int(0) != 2) {
            std::fprintf(stderr, "0x201 读回 %d（期望 2）\n", machine.global_int(0));
            return fail("0x201 没有把场景绘制模式的**原值**交出去（被归一化成 0 / 1）");
        }
        if (!machine.errors().empty()) return fail("0x200 / 0x201 写了错误通道");
        machine.consume_frame_step();
        if (!machine.step()) return fail("第三态夹具的 0x1A8 停机了");
        if (machine.consume_frame_step()) {
            return fail("第三态（2）仍然发了门前渲染步请求（第 1 支写成了「非 0」）");
        }
    }
    // ② 0x200 的前置：该位未置起 ⇒ 整条不生效（连格都不写；用 0x201 读回旧值）。
    {
        core::ConfigStore config;
        config.set_int("set:DrawMode", 1);
        config.set_int("set:CreateObject", 2);  // 只有第 1 位
        auto script = make_script_with_body();
        emit_instruction(script, 0x200, {{0, 2}});  // 第 2 位未置起
        emit_instruction(script, 0x201, {{3, 1}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table, nullptr, config);
        machine.load_root(script);
        if (!machine.step()) return fail("0x200(2)（位未置起）停机了");
        if (machine.draw_mode() != 1) return fail("set:CreateObject 的该位未置起时 0x200 仍然写了场景绘制模式");
        if (!machine.step()) return fail("0x201 停机了（位未置起夹具）");
        if (machine.global_int(1) != 1) return fail("0x200 不生效时 0x201 读回的不是旧值");
    }
    // ③ 构造期位下标（`set:CreateObject` 没有脚本可读入口 ⇒ 白盒读配置；spec 的四个方向）。
    {
        struct Case {
            std::int32_t draw_mode;
            std::int32_t create_object;
            std::int32_t expected;
            const char* why;
        };
        const Case cases[] = {
            {1, 0, 3, "第 1 位未置起 ⇒ 写成 3"},
            {1, 2, 2, "第 1 位置起 ⇒ 不得改写"},
            {2, 1, 3, "第 2 位未置起 ⇒ 写成 3"},
            {2, 4, 4, "第 2 位置起 ⇒ 不得改写"},
        };
        for (const auto& c : cases) {
            core::ConfigStore config;
            config.set_int("set:DrawMode", c.draw_mode);
            config.set_int("set:CreateObject", c.create_object);
            core::Machine machine(0x12345678u, table, nullptr, config);
            const auto actual = machine.config().get_int("set:CreateObject");
            if (actual != c.expected) {
                std::fprintf(stderr, "构造期位下标：DrawMode=%d CreateObject=%d ⇒ %d（期望 %d；%s）\n",
                             c.draw_mode, c.create_object, actual, c.expected, c.why);
                return fail("构造期没有拿场景绘制模式当 set:CreateObject 的位下标（或写值不是 3）");
            }
        }
    }
    // ④ 复位：整块清零写 0 + 按配置重设 ⇒ `0x9` 之后回到**配置值**（覆盖脚本写的第三态）。
    {
        const auto root_dir = std::filesystem::temp_directory_path() / "amayui-cxx-selftest-draw-mode";
        std::filesystem::create_directories(root_dir);
        auto root_bytes = make_script_with_body();
        emit_instruction(root_bytes, 0x1A8, {});
        emit_instruction(root_bytes, 2, {});
        {
            std::ofstream out(root_dir / "ROOT.BIN", std::ios::binary);
            out.write(reinterpret_cast<const char*>(root_bytes.data()),
                      static_cast<std::streamsize>(root_bytes.size()));
        }
        core::Install install;
        install.root = root_dir;
        install.index.files.push_back(core::AlfFile{"ROOT.BIN", 0, 0, 0, 0});
        core::ConfigStore config;
        config.set_int("set:DrawMode", 1);
        config.set_int("set:CreateObject", 2);
        auto script = make_script_with_body();
        emit_instruction(script, 0x200, {{0, 2}});
        emit_instruction(script, 0x09, {});
        core::Machine machine(0x12345678u, table, &install, config);
        machine.set_config_int("set:CreateObject", 4);  // 让 0x200(2) 的前置位成立
        machine.load_root(script);
        if (!machine.step()) return fail("复位夹具的 0x200(2) 停机了");
        if (machine.draw_mode() != 2) return fail("复位夹具：脚本没有把模式写成第三态");
        if (!machine.step()) return fail("0x9 停机了");
        if (machine.draw_mode() != 1) {
            std::fprintf(stderr, "复位后模式 = %d（期望配置值 1）\n", machine.draw_mode());
            return fail("0x9 没有按配置重设场景绘制模式（复位路径的终值不是配置值）");
        }
        std::filesystem::remove_all(root_dir);
    }
    // ⑤ 暂停闩的两个写点都以「模式 = 1」为前提。
    {
        // (i) 模式 = 2：置 1 / 清 0 **都不动**。
        core::ConfigStore config;
        config.set_int("set:DrawMode", 2);
        config.set_int("set:CreateObject", 4);  // 第 2 位置起 ⇒ 构造期不改写（与闩无关）
        core::Machine machine(0x12345678u, table, nullptr, config);
        if (machine.draw_mode() != 2) return fail("闩夹具：模式不是第三态");
        machine.scene_hold_begin();
        if (machine.scene_hold_latch() != 0) return fail("模式 ≠ 1 时闩被置 1");
        machine.scene_hold_end();
        if (machine.scene_hold_latch() != 0) return fail("模式 ≠ 1 时闩被写（清 0）");
        // (ii) 模式 = 1 时置起、随后模式变第三态 ⇒ 清 0 不动（闩是**持续状态**）。
        core::ConfigStore config2;
        config2.set_int("set:DrawMode", 1);
        config2.set_int("set:CreateObject", 2);
        auto script = make_script_with_body();
        emit_instruction(script, 0x200, {{0, 2}});
        emit_instruction(script, 2, {});
        core::Machine machine2(0x12345678u, table, nullptr, config2);
        machine2.set_config_int("set:CreateObject", 4);
        machine2.scene_hold_begin();
        if (machine2.scene_hold_latch() != 1) return fail("模式 = 1 时闩没有置 1");
        machine2.load_root(script);
        if (!machine2.step() || machine2.draw_mode() != 2) return fail("闩夹具：模式没切到第三态");
        machine2.scene_hold_end();
        if (machine2.scene_hold_latch() != 1) return fail("模式 ≠ 1 时清 0 把闩动了");
    }
    return true;
}

// ── CG 数字条 + 四张同键表的**表身份**（`CORE-OPS-SCENE-CG-NUMBER-23B`）──
// ★ 判据（每条都能被一处破坏弄红）：
//   · 「清旧项」删的是与「拆离」（`0x1f7`）**同一组四张**按键有序的表 —— 夹具把网格（`+0x428`）/
//     辅助表 A（`+0x438`）/ 辅助表 B（`+0x448`）都布在清理区间里：漏掉其中任一张 ⇒ 那几个键仍在；
//   · **场景命令表不在这四张之内**：同键的命令记录必须留着（下一次渲染步还能遍历到它）；
//   · 非法记录编号 ⇒ 错误通道「CGNumberの引数が不正です．」，**其余六个操作数一次都不读**
//     （第 3..7 个放 type 2 的文本操作数：真读了会抛 ⇒ 本条当场红）、不建项、不删旧项；
//   · 0..9 的**纹理槽 = 0** ⇒ 同样上报并到此为止；编号 **10** 也受这条约束（它的「纹理槽」= 帧 0
//     的返回栈深度），但编号 10 本身**是合法编号**（纹理槽非 0 时照常建项）；
//   · 位数 = 0 ⇒ 清理区间为空（旧条目一条都不删）、不建项、第 4 / 5 个操作数一次都不读。
// ★ 本用例**不能**区分「绘制项表被清」与「随后被重建」：清理区间 = 建项区间（`[start, start+n)`），
//   本模型的建项覆盖整个区间 ⇒ 绘制项表的删除由聚合用例的四张表断言（`0x1f7`）守。
bool check_cg_number_tables(const core::OpcodeTable& table) {
    const std::string kCgError = "CGNumberの引数が不正です．";
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "cg-number-tables: %s\n", what);
        return false;
    };
    const auto has_error = [](const core::Machine& machine, const std::string& text) {
        return machine.errors().size() == 1 && machine.errors()[0] == text;
    };
    // ① 四张表 + 命令表：键 [100, 105) 上布旧条目，然后 `0x23b(start = 100, 位数 = 5)`。
    {
        auto script = make_script_with_body();
        // CG 记录 3：纹理槽 5 / 条带左上 (0,0) / 单字宽 4 / 字高 4 / 字内空隙 0 / 字距 0。
        emit_instruction(script, 0x2DA, {{0, 3}, {0, 5}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        for (std::uint32_t key = 100; key <= 104; ++key) {  // 绘制项表（纹理 1：与重建后的 5 不同）
            emit_instruction(script, 0x1FB,
                             {{0, key}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        }
        emit_instruction(script, 0x322, {{0, 101}, {0, 0}, {0, 255}, {0, 0}});  // 网格表
        emit_instruction(script, 0x322, {{0, 102}, {0, 0}, {0, 255}, {0, 0}});
        emit_instruction(script, 0x242, {{0, 102}, {0, 7}});   // 辅助表 A（+0x438）
        emit_instruction(script, 0x242, {{0, 103}, {0, 7}});
        emit_instruction(script, 0x344, {{0, 103}, {0, 8}});   // 辅助表 B（+0x448）
        emit_instruction(script, 0x344, {{0, 104}, {0, 8}});
        // 同键命令记录（命令表是**第五张**表，不参与这次清理）。
        emit_instruction(script, 0x223, {{0, 103}, {0, 1}, {0, 0}, {0, 0},
                                         {0, 0}, {0, 0}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x23B, {{0, 100}, {0, 3}, {0, 12}, {0, 0}, {0, 0}, {0, 5}, {0, 0}});
        emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        for (int i = 0; i < 13; ++i) {  // 2DA + 5×1FB + 2×322 + 2×242 + 2×344 + 223
            if (!machine.step()) {
                std::fprintf(stderr, "cg 夹具有条目失败了：%s\n", machine.last_stop().message.c_str());
                return fail("夹具没跑起来");
            }
        }
        std::int32_t aux_a = 0, aux_a_b = 0, aux_b_flags = 0, aux_b_value = 0;
        if (machine.draw_item_count() != 5 || machine.mesh_count() != 2 ||
            machine.scene_aux_b_count() != 2 || !machine.scene_aux_a(102, aux_a, aux_a_b) ||
            !machine.scene_aux_a(103, aux_a, aux_a_b) ||
            !machine.scene_aux_b_entry(103, aux_b_flags, aux_b_value) ||
            machine.scene_commands().count(103) != 1) {
            std::fprintf(stderr, "cg 夹具没布满四张表 + 命令表：draw=%zu mesh=%zu auxB=%zu\n",
                         machine.draw_item_count(), machine.mesh_count(),
                         machine.scene_aux_b_count());
            return fail("夹具没布满四张同键表 + 命令表");
        }
        // 清脏但**保住命令表**：`end_render` 的收尾判据是「场景待处理标记 = 0 ⇒ 清命令表」
        // （`CORE-SCENE-DRAW-ORDER` 的命令表生命周期）⇒ 先把该标记置起，否则夹具自己就把命令记录清了。
        machine.set_scene_pending(true);
        machine.end_render();  // 清脏
        if (!machine.step()) return fail("0x23b 停机了");
        const bool aux_a_empty = !machine.scene_aux_a(102, aux_a, aux_a_b) &&
                                 !machine.scene_aux_a(103, aux_a, aux_a_b);
        const bool aux_b_empty = !machine.scene_aux_b_entry(103, aux_b_flags, aux_b_value) &&
                                 !machine.scene_aux_b_entry(104, aux_b_flags, aux_b_value);
        if (machine.mesh_count() != 0 || !aux_a_empty || !aux_b_empty) {
            std::fprintf(stderr, "0x23b 没有清掉四张表（mesh=%zu auxA空=%d auxB空=%d）\n",
                         machine.mesh_count(), aux_a_empty ? 1 : 0, aux_b_empty ? 1 : 0);
            return fail("0x23b 的「清旧项」漏了某张同键表（或表身份认错）");
        }
        if (machine.scene_commands().count(103) != 1) {
            return fail("0x23b 把同键的场景命令记录一起删了（命令表不在这四张之内）");
        }
        // 绘制项被**重建**成 cg 记录那张纹理（5）；旧的那批指向纹理 1。
        for (std::uint32_t key = 100; key <= 104; ++key) {
            const core::DrawItem* item = machine.draw_item(key);
            if (item == nullptr || (item->flags & 1u) == 0 || item->texture_id != 5) {
                std::fprintf(stderr, "键 %u 的绘制项不是 cg 记录建出来的那一条\n", key);
                return fail("0x23b 没有按 cg 记录重建绘制项");
            }
        }
        if (!machine.scene_dirty()) return fail("0x23b 删掉旧条目之后没有标脏");
    }
    // ② 非法编号（11）⇒ 上报固定文本；其余六个操作数一次都不读（第 3..7 个是 type 2）× 不删旧项。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x1FB,
                         {{0, 200}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x23B, {{0, 200}, {0, 11}, {2, 0}, {2, 0},
                                         {2, 0}, {2, 0}, {2, 0}});
        emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        if (!machine.step()) return fail("非法编号夹具的 0x1fb 停机了");
        if (!machine.step()) {
            std::fprintf(stderr, "0x23b 读了其余操作数：%s\n", machine.last_stop().message.c_str());
            return fail("记录编号非法时 0x23b 仍然读了其余六个操作数（夹具里的文本操作数被当整数读）");
        }
        if (!has_error(machine, kCgError)) return fail("记录编号非法时错误通道不是「CGNumberの引数が不正です．」");
        if (machine.draw_item(200) == nullptr) return fail("记录编号非法时 0x23b 删掉了旧项");
        if (machine.draw_item_count() != 1) return fail("记录编号非法时 0x23b 建了项");
    }
    // ③ 0..9 的纹理槽 = 0 ⇒ 同样上报；编号 10 也受同一条约束；编号 10 本身合法（纹理槽非 0 时照建）。
    {
        auto zero_slot = make_script_with_body();
        emit_instruction(zero_slot, 0x2DA, {{0, 4}, {0, 0}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(zero_slot, 0x23B, {{0, 210}, {0, 4}, {2, 0}, {2, 0},
                                            {2, 0}, {2, 0}, {0, 1}});
        emit_instruction(zero_slot, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(zero_slot);
        if (!machine.step()) return fail("纹理槽 0 夹具的 0x2da 停机了");
        if (!machine.step()) return fail("纹理槽 0 时 0x23b 读了其余操作数 / 抛了异常");
        if (!has_error(machine, kCgError)) return fail("纹理槽 = 0 时错误通道不是「CGNumberの引数が不正です．」");
        if (machine.draw_item_count() != 0) return fail("纹理槽 = 0 时 0x23b 仍然建了项");
    }
    {
        auto id10 = make_script_with_body();
        emit_instruction(id10, 0x2DA, {{0, 10}, {0, 0}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(id10, 0x23B, {{0, 220}, {0, 10}, {2, 0}, {2, 0},
                                       {2, 0}, {2, 0}, {0, 1}});
        emit_instruction(id10, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(id10);
        if (!machine.step()) return fail("编号 10 夹具的 0x2da 停机了");
        if (!machine.step()) return fail("编号 10（纹理槽 = 0）时 0x23b 读了其余操作数 / 抛了异常");
        if (!has_error(machine, kCgError)) {
            return fail("编号 10 的「纹理槽」= 0 时没有上报（判据写成了 id < 10）");
        }
    }
    {
        auto id10_ok = make_script_with_body();
        emit_instruction(id10_ok, 0x2DA, {{0, 10}, {0, 5}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(id10_ok, 0x23B, {{0, 230}, {0, 10}, {0, 7}, {0, 0}, {0, 0}, {0, 1}, {0, 0}});
        emit_instruction(id10_ok, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(id10_ok);
        if (!machine.step()) return fail("编号 10（合法）夹具的 0x2da 停机了");
        if (!machine.step()) return fail("编号 10（纹理槽非 0）时 0x23b 停机了");
        if (machine.draw_item(230) == nullptr || !machine.errors().empty()) {
            return fail("编号 10 被当成越界（判据该是「不在 0..10」）");
        }
    }
    // ④ 位数 = 0 ⇒ 清理区间为空（旧条目不删）、不建项、第 4 / 5 个操作数一次都不读。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x2DA, {{0, 3}, {0, 5}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x1FB,
                         {{0, 300}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x23B, {{0, 300}, {0, 3}, {0, 12}, {2, 0}, {2, 0}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        for (int i = 0; i < 2; ++i) {
            if (!machine.step()) return fail("位数 0 夹具的前置停机了");
        }
        machine.end_render();
        if (!machine.step()) return fail("位数 = 0 时 0x23b 读了第 4 / 5 个操作数");
        if (machine.draw_item(300) == nullptr) return fail("位数 = 0 时 0x23b 删了旧条目（区间应为空）");
        if (machine.draw_item_count() != 1) return fail("位数 = 0 时 0x23b 建了项");
        if (!machine.errors().empty()) return fail("位数 = 0 时 0x23b 写了错误通道（应为静默返回）");
    }
    return true;
}

// ── 绘制顺序（`CORE-SCENE-DRAW-ORDER`）：收尾扫描的四张表合并访问与平键序 ──
// ★ 判据：同键平键序 = 辅助表 B → 辅助表 A → 网格 → 绘制项；**每张表各自**判自己的「第 16 位 =
//   本轮已消费」—— 一张表里的条目被消费过，⛔ **不**阻止同键的其它表发绘制请求。
//   （旧实现用一个 `continue` 把两张表耦合起来：网格的「第 16 位」置起会连带跳过同键的绘制项。）
bool check_scene_draw_scan_order(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "scene-draw-order: %s\n", what);
        return false;
    };
    constexpr std::uint32_t kWidth = 8;
    const auto pixel = [](const core::Bytes& canvas, std::size_t at) {
        const std::uint8_t v = at + 3 < canvas.size() ? canvas[at] : 0;
        return static_cast<int>(v);
    };
    // 4x4 白纹理 + 同键（5）的绘制项 + 同键的空网格记录（`0x322` 只建记录）。
    const auto make_script = [&]() {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x1F8, {{0, 1}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(bytes, 0x20B, {{0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 255}, {0, 0x00FFFFFFu}});
        emit_instruction(bytes, 0x1FB,
                         {{0, 5}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(bytes, 0x322, {{0, 5}, {0, 0}, {0, 255}, {0, 0}});
        emit_instruction(bytes, 2, {});
        return bytes;
    };
    // ① 网格的「第 16 位」置起 ⇒ 只跳过网格，同键的绘制项**照画**（白色像素在 (0,0)）。
    {
        core::Machine machine(0x12345678u, table);
        machine.load_root(make_script());
        machine.run(32);
        core::MeshRecord* mesh = machine.mesh_mut(5);
        if (mesh == nullptr) return fail("夹具：网格记录没建出来");
        mesh->flags |= (1u << 16);
        const core::Bytes canvas = core::rasterize_scene(machine, kWidth, 8);
        if (pixel(canvas, 0) != 255) {
            std::fprintf(stderr, "像素(0,0) 红 = %d（期望 255：绘制项应照画）\n", pixel(canvas, 0));
            return fail("网格被消费之后同键的绘制项没有画出来（两张表被同一个判据耦合）");
        }
        core::MeshRecord* after = machine.mesh_mut(5);
        if (after == nullptr || (after->flags & (1u << 16)) != 0) {
            return fail("被访问过的网格条目没有清掉第 16 位");
        }
    }
    // ② 绘制项的「第 16 位」置起 ⇒ 只跳过绘制项，同键的网格**照画**（用真的 4 顶点三角形）。
    {
        core::Machine machine(0x12345678u, table);
        machine.load_root(make_script());
        machine.run(32);
        core::DrawItem* item = machine.draw_item_mut(5);
        core::MeshRecord* mesh = machine.mesh_mut(5);
        if (item == nullptr || mesh == nullptr) return fail("夹具：绘制项 / 网格没建出来");
        item->flags |= (1u << 16);
        mesh->flags |= 1u;
        mesh->count = 4;
        mesh->copies[0] = {std::bit_cast<std::uint32_t>(0.0f), std::bit_cast<std::uint32_t>(8.0f),
                           std::bit_cast<std::uint32_t>(0.0f), std::bit_cast<std::uint32_t>(8.0f)};
        mesh->copies[1] = {std::bit_cast<std::uint32_t>(0.0f), std::bit_cast<std::uint32_t>(0.0f),
                           std::bit_cast<std::uint32_t>(8.0f), std::bit_cast<std::uint32_t>(8.0f)};
        mesh->color = 0xFFFF00FFu;        // 不透明品红：与白色的绘制项在像素上可区分
        mesh->target_color = 0xFFFF00FFu;
        const core::Bytes canvas = core::rasterize_scene(machine, kWidth, 8);
        // 绘制项被消费 ⇒ (0,0) 只剩网格的三角形：红 255 / 绿 0（⛔ 不是白色的 255/255）。
        if (canvas.size() < 4 || canvas[0] != 255 || canvas[1] != 0 || canvas[3] != 255) {
            std::fprintf(stderr, "像素(0,0) = (%u,%u,%u,%u)（期望 255,0,255,255 = 只有网格）\n",
                         canvas.size() > 3 ? canvas[0] : 0, canvas.size() > 3 ? canvas[1] : 0,
                         canvas.size() > 3 ? canvas[2] : 0, canvas.size() > 3 ? canvas[3] : 0);
            return fail("绘制项被消费之后同键的网格没有画出来（或白色的绘制项仍然画了一遍）");
        }
    }
    return true;
}

// ── 网格：建 / 换（`CORE-OPS-SCENE-320`）：十个操作数的读序 + 36 字节 / 9 分量的装配 ──
// 判据（每条都能被一处破坏弄红）：
//   · 装配后的顶点缓冲 = 「顶点数 × 36 字节 / 9 个 32 位分量」，格式字 `0x244`；
//   · 九个分量的来源（合同表）：位置 x / y / z ← 第 2 / 3 / 4 段；位置第四分量 ← 引擎自建的
//     **常量 1.0**（⛔ 不是操作数）；色字 ← 第 5、6 段合成的整字；两套纹理坐标 ← 第 7、8 段，
//     且**两套共用同一对源段**；
//   · 位置前两个分量各减去**同一个**常量（两轴「原值 − 装配值」相等）、第三个分量**不减**；
//     ★ 该常量的取值正文不规定 ⇒ 这里**只**断「同一个」+「确实减了」，⛔ 不断它的值；
//   · 每段源数组的**每顶点步长 = 1 个 32 位字**（段里每个顶点的值都不同 ⇒ 步长写成 4 个字当场红）；
//   · 三张副本数组：第 1 张 = 第 2 段原样 / 第 2 张 = 第 3 段原样 / 第 3 张 = 合成色字；
//     **装配之后**才写、且烘色**不改**它（原逐顶点颜色是"不变量数组"，重复烘焙不累积）；
//   · 顶点数 = 5 ⇒ 四顶点一批 + **余数逐个**（第 5 个顶点与第 1..4 个同形、且不丢）；
//   · 重烘门：当前颜色 = 无颜色 ⇒ **不重烘**；`0xFFFFFFFE` ⇒ 重烘（逐字节 ×0xFE÷255 向下取整）；
//     `0x80FFFFFF`（低 24 位全 1、透明度不是 0xFF）⇒ **也**必须重烘（⛔ 只比低 24 位会漏烘）；
//   · 顶点数 = 0 ⇒ 错误通道恰一条（文本 = 「顶点数非法」）、记录**一格不写**、**不标脏**；
//     构建成功 ⇒ **无条件**标脏（两种重烘支都标）；
//   · 第 10 个操作数落记录、`count` = 顶点数；四个读序判据（第 9 个 / 第 5 个地址 / 第 10 个 /
//     第 4 个地址分别先于它们后面那一步被读）。
bool check_scene_mesh_320(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "scene-mesh-320: %s\n", what);
        return false;
    };
    // 段的落点：位置 x / y / z 与两套纹理坐标用全局**浮点**族（type 4），色字两段用全局**整型**
    // 族（type 3）。段与段之间留空，互不重叠；每段 = 顶点数个字（每顶点 1 个 32 位字）。
    constexpr std::uint32_t kSegX = 0;
    constexpr std::uint32_t kSegY = 8;
    constexpr std::uint32_t kSegZ = 16;
    constexpr std::uint32_t kSegU = 24;
    constexpr std::uint32_t kSegV = 32;
    constexpr std::uint32_t kSegH = 0;  // 整型：高 8 位那一段
    constexpr std::uint32_t kSegL = 8;  // 整型：低 24 位那一段
    const float xs[5] = {10.0f, 11.0f, 12.0f, 13.0f, 14.0f};
    const float ys[5] = {20.0f, 21.0f, 22.0f, 23.0f, 24.0f};
    const float zs[5] = {30.0f, 31.0f, 32.0f, 33.0f, 34.0f};
    const float us[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    const float vs[5] = {1.0f, 0.75f, 0.5f, 0.25f, 0.0f};
    const std::uint32_t highs[5] = {0x7Fu, 0x11u, 0x22u, 0x33u, 0x44u};
    const std::uint32_t lows[5] = {0x123456u, 0x223456u, 0x323456u, 0x423456u, 0x523456u};
    const auto composed_color = [&](int i) -> std::uint32_t {
        return (highs[i] << 24) | (lows[i] & 0xFFFFFFu);
    };
    const auto seed_pools = [&](core::Machine& machine, int count) {
        for (int i = 0; i < count; ++i) {
            const auto at = static_cast<std::uint32_t>(i);
            machine.write_float(core::Operand{4, kSegX + at}, xs[i]);
            machine.write_float(core::Operand{4, kSegY + at}, ys[i]);
            machine.write_float(core::Operand{4, kSegZ + at}, zs[i]);
            machine.write_float(core::Operand{4, kSegU + at}, us[i]);
            machine.write_float(core::Operand{4, kSegV + at}, vs[i]);
            machine.write_int(core::Operand{3, kSegH + at}, static_cast<std::int32_t>(highs[i]));
            machine.write_int(core::Operand{3, kSegL + at}, static_cast<std::int32_t>(lows[i]));
        }
    };
    // 一条 `0x320`：操作数序 = 1 编号 / 2..4 位置段 / 5、6 色段 / 7、8 纹理坐标段 / 9 顶点数 / 10 整数。
    const auto emit_320 = [&](std::vector<std::uint8_t>& bytes, std::uint32_t key, int count,
                              std::uint32_t op10) {
        emit_instruction(bytes, 0x320,
                         {{0, key},
                          {4, kSegX},
                          {4, kSegY},
                          {4, kSegZ},
                          {3, kSegH},
                          {3, kSegL},
                          {4, kSegU},
                          {4, kSegV},
                          {0, static_cast<std::uint32_t>(count)},
                          {0, op10}});
    };
    // 顶点缓冲的逐分量判据（含三张副本数组 + 布局 + 字节数）。
    const auto check_vertices = [&](const core::MeshRecord& mesh, int count) -> bool {
        if (mesh.vertex_buffer.format != 0x244u) {
            std::fprintf(stderr, "顶点格式字 = %03X（期望 244 = XYZRHW | DIFFUSE | TEX2）\n",
                         mesh.vertex_buffer.format);
            return fail("顶点格式字不是 0x244");
        }
        const auto& words = mesh.vertex_buffer.words;
        if (words.size() != static_cast<std::size_t>(count) * 9u) {
            std::fprintf(stderr, "顶点缓冲 %zu 个字（期望 %d × 9）\n", words.size(), count);
            return fail("顶点缓冲不是每顶点 9 个 32 位分量");
        }
        if (words.size() * sizeof(std::uint32_t) != static_cast<std::size_t>(count) * 36u) {
            return fail("顶点缓冲不是每顶点 36 字节");
        }
        if (mesh.count != count) return fail("记录的顶点数不等于第 9 个操作数");
        if (mesh.copies[0].size() != static_cast<std::size_t>(count) ||
            mesh.copies[1].size() != static_cast<std::size_t>(count) ||
            mesh.copies[2].size() != static_cast<std::size_t>(count)) {
            return fail("三张副本数组不是「顶点数」个字（各 n × 4 字节）");
        }
        for (int i = 0; i < count; ++i) {
            const auto base = static_cast<std::size_t>(i) * 9u;
            const auto ax = std::bit_cast<float>(words[base + 0]);
            const auto ay = std::bit_cast<float>(words[base + 1]);
            const auto az = std::bit_cast<float>(words[base + 2]);
            const auto aw = std::bit_cast<float>(words[base + 3]);
            if (az != zs[i]) {
                std::fprintf(stderr, "顶点 %d 的第三个分量 = %f（期望 %f：第三个分量不减）\n", i,
                             static_cast<double>(az), static_cast<double>(zs[i]));
                return fail("位置的第三个分量被减了常量（正文：第三个分量取原值）");
            }
            if (ax == xs[i] || ay == ys[i]) {
                std::fprintf(stderr, "顶点 %d 的位置 = (%f,%f)（期望两轴各减去同一个常量）\n", i,
                             static_cast<double>(ax), static_cast<double>(ay));
                return fail("位置前两个分量没有减去常量");
            }
            if ((xs[i] - ax) != (ys[i] - ay)) {
                std::fprintf(stderr, "顶点 %d 两轴减去量 = (%f,%f)（期望同一个量）\n", i,
                             static_cast<double>(xs[i] - ax), static_cast<double>(ys[i] - ay));
                return fail("位置前两个分量减的不是同一个常量");
            }
            if (aw != 1.0f) return fail("位置第四个分量不是引擎自建的常量 1.0");
            if (words[base + 4] != composed_color(i)) {
                std::fprintf(stderr, "顶点 %d 的色字 = %08X（期望合成的 %08X）\n", i, words[base + 4],
                             composed_color(i));
                return fail("色字不是第 5、6 段合成的整字（或装配后顺手烘了色）");
            }
            if (words[base + 5] != std::bit_cast<std::uint32_t>(us[i])) {
                return fail("第一套纹理坐标的 u 不是第 7 段的直拷");
            }
            if (words[base + 6] != std::bit_cast<std::uint32_t>(vs[i])) {
                return fail("第一套纹理坐标的 v 不是第 8 段的直拷");
            }
            if (words[base + 7] != words[base + 5] || words[base + 8] != words[base + 6]) {
                return fail("第二套纹理坐标没跟第一套共用同一对源段");
            }
            // 三张副本数组：第 1 张 ← 第 2 段原样、第 2 张 ← 第 3 段原样、第 3 张 ← 合成色字。
            if (mesh.copies[0][static_cast<std::size_t>(i)] != std::bit_cast<std::uint32_t>(xs[i])) {
                return fail("第 1 张副本数组不是第 2 段原样");
            }
            if (mesh.copies[1][static_cast<std::size_t>(i)] != std::bit_cast<std::uint32_t>(ys[i])) {
                return fail("第 2 张副本数组不是第 3 段原样");
            }
            if (mesh.copies[2][static_cast<std::size_t>(i)] != composed_color(i)) {
                return fail("第 3 张副本数组不是第 5、6 段合成的逐顶点颜色");
            }
        }
        return true;
    };
    // ① 顶点数 = 4（四顶点一批）+ 当前颜色 = 无颜色 ⇒ 不重烘、色字 = 装配结果、构建成功即标脏。
    {
        auto bytes = make_script_with_body();
        emit_320(bytes, 41, 4, 0x5A5Au);
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(bytes);
        seed_pools(machine, 4);
        if (!machine.step()) return fail("0x320（顶点数 4）第一步停机了");
        const core::MeshRecord* mesh = machine.mesh(41);
        if (mesh == nullptr) return fail("0x320 没建出网格记录");
        if ((mesh->flags & 1u) == 0) return fail("0x320 没置「已存在」位");
        if (mesh->op10 != 0x5A5A) return fail("第 10 个操作数没写进记录");
        if (!check_vertices(*mesh, 4)) return false;
        if (!machine.scene_dirty()) return fail("构建成功后没有标脏");
    }
    // ② 顶点数 = 5 ⇒ 一批 4 个 + 余数 1 个：第 5 个顶点不许丢、且与前面同形（同一分量同一相对位置）。
    {
        auto bytes = make_script_with_body();
        emit_320(bytes, 42, 5, 0x77u);
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(bytes);
        seed_pools(machine, 5);
        if (!machine.step()) return fail("0x320（顶点数 5）第一步停机了");
        const core::MeshRecord* mesh = machine.mesh(42);
        if (mesh == nullptr) return fail("0x320（顶点数 5）没建出网格记录");
        if (!check_vertices(*mesh, 5)) return false;
    }
    // ③ 重烘门：当前颜色 = 0xFFFFFFFE（透明度 0xFF）**不是**无颜色 ⇒ 必须重烘。
    //    原色 0x7F123456 × 系数 0xFFFFFFFE 逐字节 ÷255 向下取整 ⇒ 0x7F123455（只有蓝字节被乘 0xFE）；
    //    副本数组不变（原逐顶点颜色是不变量）。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x322, {{0, 43}, {0, 0}, {0, 255}, {0, 0x00FFFFFEu}});
        emit_320(bytes, 43, 1, 3);
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(bytes);
        seed_pools(machine, 1);
        if (!machine.step()) return fail("0x322 设色停机了");
        if (!machine.step()) return fail("0x320（重烘支）停机了");
        const core::MeshRecord* mesh = machine.mesh(43);
        if (mesh == nullptr || mesh->vertex_buffer.words.size() != 9u) {
            return fail("重烘夹具：网格 / 顶点缓冲没建出来");
        }
        if (mesh->color != 0xFFFFFFFEu) {
            std::fprintf(stderr, "当前颜色 = %08X（期望 FFFFFFFE）\n", mesh->color);
            return fail("0x322 的当前颜色没写对（重烘门的分母）");
        }
        if (mesh->vertex_buffer.words[4] != 0x7F123455u) {
            std::fprintf(stderr, "烘后色字 = %08X（期望 7F123455 = 7F123456 × FFFFFFFE ÷255）\n",
                         mesh->vertex_buffer.words[4]);
            return fail("当前颜色不是无颜色却没有按当前颜色重烘");
        }
        if (mesh->copies[2][0] != 0x7F123456u) {
            return fail("烘色改了第 3 张副本数组（原逐顶点颜色是不变量）");
        }
    }
    // ④ 重烘门的**整字**判据：0x80FFFFFF 的低 24 位全是 1，但它不是无颜色（0xFFFFFFFF）
    //    ⇒ 也必须重烘 ⇒ 0x7F123456 × 0x80FFFFFF ÷255 = 0x3F123456（只比低 24 位会漏烘）。
    {
        auto bytes = make_script_with_body();
        emit_instruction(bytes, 0x322, {{0, 44}, {0, 0}, {0, 0x80}, {0, 0x00FFFFFFu}});
        emit_320(bytes, 44, 1, 4);
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(bytes);
        seed_pools(machine, 1);
        if (!machine.step()) return fail("0x322（0x80FFFFFF）停机了");
        if (!machine.step()) return fail("0x320（0x80FFFFFF）停机了");
        const core::MeshRecord* mesh = machine.mesh(44);
        if (mesh == nullptr || mesh->vertex_buffer.words.size() != 9u) {
            return fail("整字门夹具：网格 / 顶点缓冲没建出来");
        }
        if (mesh->color != 0x80FFFFFFu) return fail("夹具：0x322 没写出 0x80FFFFFF");
        if (mesh->vertex_buffer.words[4] != 0x3F123456u) {
            std::fprintf(stderr, "烘后色字 = %08X（期望 3F123456：低 24 位全 1 也必须重烘）\n",
                         mesh->vertex_buffer.words[4]);
            return fail("重烘门只比了低 24 位（0x80FFFFFF 被当成无颜色）");
        }
    }
    // ⑤ 顶点数 = 0 ⇒ 错误通道恰一条「顶点数非法」、记录**一格不写**、**不标脏**（构建失败）。
    {
        auto bytes = make_script_with_body();
        emit_320(bytes, 45, 4, 9);
        emit_320(bytes, 45, 0, 10);
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(bytes);
        seed_pools(machine, 4);
        if (!machine.step()) return fail("0x320（顶点数 4，失败夹具）停机了");
        machine.end_render();  // 清脏：判"失败支不标脏"必须从 0 起
        const core::MeshRecord before = *machine.mesh(45);
        if (!machine.step()) return fail("顶点数 = 0 时 0x320 停机了（正文：只上报、不抛）");
        if (machine.errors().size() != 1 || machine.errors()[0] != "顶点数非法") {
            std::fprintf(stderr, "错误通道 %zu 条（期望恰 1 条「顶点数非法」）\n",
                         machine.errors().size());
            return fail("顶点数 ≤ 0 没有把错误文本写进错误通道");
        }
        if (machine.scene_dirty()) return fail("构建失败也把场景标脏了");
        const core::MeshRecord* after = machine.mesh(45);
        if (after == nullptr) return fail("失败支把记录删了（正文：不建网格 ⇒ 旧记录不动）");
        if (after->count != before.count || after->op10 != before.op10 ||
            after->flags != before.flags ||
            after->vertex_buffer.words != before.vertex_buffer.words ||
            after->copies[0] != before.copies[0] || after->copies[1] != before.copies[1] ||
            after->copies[2] != before.copies[2]) {
            return fail("构建失败的支改动了网格记录（正文：那些格一个都不写）");
        }
    }
    // ⑥ 是**替换**不是追加：同一个编号再建一次（顶点数 4 → 2）⇒ 旧的三张副本数组与顶点缓冲一起被换掉
    //    （正文：「重复调用先释放旧缓冲」）。
    {
        auto bytes = make_script_with_body();
        emit_320(bytes, 46, 4, 1);
        emit_320(bytes, 46, 2, 2);
        emit_instruction(bytes, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(bytes);
        seed_pools(machine, 4);
        if (!machine.step() || !machine.step()) return fail("0x320（替换夹具）停机了");
        const core::MeshRecord* mesh = machine.mesh(46);
        if (mesh == nullptr) return fail("0x320（替换夹具）没建出网格记录");
        if (mesh->count != 2 || mesh->op10 != 2 || mesh->vertex_buffer.words.size() != 18u ||
            mesh->copies[0].size() != 2 || mesh->copies[1].size() != 2 ||
            mesh->copies[2].size() != 2) {
            std::fprintf(stderr, "替换后 count=%d op10=%d 缓冲=%zu 字（期望 2 / 2 / 18）\n",
                         mesh->count, mesh->op10, mesh->vertex_buffer.words.size());
            return fail("0x320 是「追加」不是「替换」（旧的副本数组 / 顶点缓冲没被换掉）");
        }
        if (!check_vertices(*mesh, 2)) return false;
    }
    // ⑦ 读序是合同（读序的可观察后果 = 操作数类型异常时的抛出顺序）：四个"谁先被读"判据。
    //    type 8 ⇒ `read-int-unsupported-tag:8`；type 2 ⇒ `address-of-unsupported-tag:2`。
    {
        const auto run = [&](const char* what, std::vector<std::uint8_t> bytes,
                             const char* expected) -> bool {
            core::Machine machine(0x12345678u, table);
            machine.load_root(bytes);
            if (machine.step()) {
                std::fprintf(stderr, "读序夹具（%s）没有抛\n", what);
                return fail("读序夹具没有抛（操作数类型非法却没被读到）");
            }
            if (machine.last_stop().message != expected) {
                std::fprintf(stderr, "读序夹具（%s）抛出 %s（期望 %s）\n", what,
                             machine.last_stop().message.c_str(), expected);
                return fail("读序与本条合同不符（先读的操纵数不是正文点名的那一个）");
            }
            return true;
        };
        // ① 第 9 个（顶点数）先读：它非法 ⇒ 报 read-int；第 5 个（地址）也非法，却还没轮到。
        auto first = make_script_with_body();
        emit_instruction(first, 0x320,
                         {{0, 51}, {4, kSegX}, {4, kSegY}, {4, kSegZ}, {2, 0}, {3, kSegL},
                          {4, kSegU}, {4, kSegV}, {8, 0}, {0, 1}});
        emit_instruction(first, 2, {});
        if (!run("第 9 个先读", first, "read-int-unsupported-tag:8")) return false;
        // ② 第 5、6 个的地址先于第 10 个：第 5 个非法（address-of）、第 10 个也非法（read-int）。
        auto second = make_script_with_body();
        emit_instruction(second, 0x320,
                         {{0, 52}, {4, kSegX}, {4, kSegY}, {4, kSegZ}, {2, 0}, {3, kSegL},
                          {4, kSegU}, {4, kSegV}, {0, 1}, {8, 0}});
        emit_instruction(second, 2, {});
        if (!run("第 5 个地址先读完", second, "address-of-unsupported-tag:2")) return false;
        // ③ 第 10 个先于第 4、3、2 个的地址：第 10 个非法（read-int）、第 4 个也非法（address-of）。
        auto third = make_script_with_body();
        emit_instruction(third, 0x320,
                         {{0, 53}, {4, kSegX}, {4, kSegY}, {2, 0}, {3, kSegH}, {3, kSegL},
                          {4, kSegU}, {4, kSegV}, {0, 1}, {8, 0}});
        emit_instruction(third, 2, {});
        if (!run("第 10 个先读", third, "read-int-unsupported-tag:8")) return false;
        // ④ 第 4 个的地址先于第 1 个（编号）：第 4 个非法（address-of）、第 1 个也非法（read-int）。
        auto fourth = make_script_with_body();
        emit_instruction(fourth, 0x320,
                         {{8, 0}, {4, kSegX}, {4, kSegY}, {2, 0}, {3, kSegH}, {3, kSegL},
                          {4, kSegU}, {4, kSegV}, {0, 1}, {0, 1}});
        emit_instruction(fourth, 2, {});
        if (!run("第 4 个地址先读", fourth, "address-of-unsupported-tag:2")) return false;
    }
    return true;
}

// ── 媒体对象表（`60-ops-render.md` 的「媒体对象表（合同）」+ `60-ops-wait.md` 的「播影片」）──
// ★ 这张表 = 影片对象表 = 「纹理族」的**共享对象槽**（同一批槽位、同一套编号）。
// 判据（每条都能被一处破坏弄红）：
//   · **填** = 五条指令，**全部按第 2 个操作数**选格：`0x20f` / `0x236` / `0x240` / `0x241` / `0x24d`
//     （夹具把第 1 个操作数写成另一个编号：按第 1 个选格 ⇒ 断言当场红）；
//   · **同槽已有对象 ⇒ 不重建、不释放**：格上仍是**原来那个对象**（身份号不变、已启动状态与
//     已记下的资源 id 都还在）—— ⛔ 不是"先释放再建一个新对象"；
//   · `0x24d` 的**次序**（先填媒体格、后判纹理槽）：纹理槽为空而抛出时，那一格**仍然填着**，
//     且**旧命令记录不动**（记录仍是原来那条种类 0）；
//   · **清** = ① 纹理族三条写指令清自己那个编号（`0x1f8` 第 1 个 / `0x1f9` 第 2 个 /
//     `0x1fa` 第 1 个）② `0x210` 第 1 个 / `0x249` 第 2 个 ③ **每拍扫描**（到结束条件且循环位 = 0）
//     ⑤ `0x23d` 的 `42..999`（0..41 不动）；
//   · **上限一千 = 扫描的循环边界**：槽号 1000 的填**不被拒绝**，而两条整表遍历都不含它。
// ★ 本用例**不覆盖**（模型里没有对应载体，见交付回执的"未做"）：
//   ① 两项逐媒体对象谓词（到期 / 占用）及其「先到期、后占用」的次序 —— 正文说那两个目标
//     （媒体纹理渲染器 / 媒体会话）是**引擎对象**，四个可证伪条只能**白盒插桩**；本模型没有这两个目标。
//   ② 清的第 ④ 条（「本拍不计入」为真 ⇒ 释放并置空）—— 正文自承那个判定的**领域身份未取证**。
bool check_media_table(const core::OpcodeTable& table) {
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "media-table: %s\n", what);
        return false;
    };
    // 该格上有对象（`present`）⇒ true。空槽 / 没填过 ⇒ false。
    const auto filled = [](const core::Machine& machine, std::uint32_t slot) {
        const core::MovieRecord* record = machine.movie(slot);
        return record != nullptr && record->present;
    };
    const auto identity_of = [](const core::Machine& machine, std::uint32_t slot) {
        const core::MovieRecord* record = machine.movie(slot);
        return record == nullptr ? std::uint64_t{0} : record->identity;
    };
    // 播影片的确定性替身（与聚合用例同一套：不读文件、帧内容固定）。
    const auto install_fake_movie = [](core::Machine& machine, FakeMovieStats& stats) {
        machine.set_movie_bytes_loader([](std::uint32_t) { return core::Bytes{0, 0, 1, 0xBA}; });
        machine.set_movie_backend_factory([&stats]() {
            return std::unique_ptr<core::MovieBackend>(
                std::make_unique<FakeMovieBackend>(&stats, 4, 4, 0x40));
        });
    };
    // ① 三条「整条尚未成篇」的填格指令：**按第 2 个操作数**选格、格空才建。
    //    夹具把第 1 个操作数写成 900 / 901 / 902（另一个编号）：按第 1 个选格 ⇒ 下面同时红。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x236, {{0, 900}, {0, 50}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x240, {{0, 901}, {0, 51}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x241, {{0, 902}, {0, 52}, {0, 0}, {0, 0}, {0, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        for (int i = 0; i < 3; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "填格夹具第 %d 步停机：%s\n", i + 1,
                             machine.last_stop().message.c_str());
                return fail("0x236 / 0x240 / 0x241 没有 handler（执行到就抛）");
            }
        }
        if (!filled(machine, 50) || !filled(machine, 51) || !filled(machine, 52)) {
            return fail("三条填格指令没有按第 2 个操作数往媒体对象表填格");
        }
        if (filled(machine, 900) || filled(machine, 901) || filled(machine, 902)) {
            return fail("填格指令按第 1 个操作数选格了（合同：全部按**第 2 个**操作数）");
        }
        if (!machine.errors().empty()) return fail("填格指令写了错误通道");
    }
    // ② 同槽已有对象 ⇒ **不重建、不释放**（旧对象留在格里，只改配置 / 启动）。
    {
        FakeMovieStats stats;
        auto script = make_script_with_body();
        emit_instruction(script, 0x1F8, {{0, 60}, {0, 4}, {0, 4}, {0, 0}});  // 纹理绑定（0x20f 的门）
        emit_instruction(script, 0x20F, {{0, 7}, {0, 60}, {0, 0}});
        emit_instruction(script, 0x236, {{0, 8}, {0, 60}, {0, 0}, {0, 0}});  // 同槽换一条指令再填
        emit_instruction(script, 0x240, {{0, 9}, {0, 60}, {0, 0}, {0, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        install_fake_movie(machine, stats);
        machine.load_root(script);
        for (int i = 0; i < 2; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "同槽复用夹具第 %d 步停机：%s\n", i + 1,
                             machine.last_stop().message.c_str());
                return fail("0x1f8 / 0x20f 没跑起来");
            }
        }
        const std::uint64_t object = identity_of(machine, 60);
        if (object == 0 || !filled(machine, 60)) return fail("夹具：0x20f 没在槽 60 上建出对象");
        for (int i = 0; i < 2; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "同槽复用夹具填格第 %d 步停机：%s\n", i + 1,
                             machine.last_stop().message.c_str());
                return fail("同槽再填时 0x236 / 0x240 停机了");
            }
        }
        const core::MovieRecord* after = machine.movie(60);
        if (after == nullptr || after->identity != object) {
            return fail("同槽已有对象时重建了对象（对象身份号变了：旧对象被释放 / 换成了新对象）");
        }
        if (!after->started || after->file_id != 7) {
            std::fprintf(stderr, "同槽复用后 started=%d file_id=%u（期望 1 / 7）\n",
                         after->started ? 1 : 0, after->file_id);
            return fail("同槽再填把旧对象释放了（已启动状态 / 已记下的资源 id 被清掉）");
        }
    }
    // ③ `0x24d` 的次序是合同：**先填媒体格、后判纹理槽**。
    {
        // (i) 纹理槽为空 ⇒ 抛出；但媒体格**已经填上**、旧命令记录**不动**。
        auto script = make_script_with_body();
        emit_instruction(script, 0x223,
                         {{0, 70}, {0, 5}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x24D,
                         {{0, 70}, {0, 73}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
                          {0, 0}, {0, 0}, {0, 0}, {0, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        if (!machine.step()) {
            std::fprintf(stderr, "0x24d 夹具：0x223 停机 %s\n",
                         machine.last_stop().message.c_str());
            return fail("0x24d 夹具的 0x223 没跑起来");
        }
        if (machine.step()) return fail("0x24d 的纹理槽为空却没有抛出（绑定门丢了）");
        if (machine.last_stop().message.find("movie-texture-slot-empty") == std::string::npos) {
            std::fprintf(stderr, "抛出的文本 = %s\n", machine.last_stop().message.c_str());
            return fail("0x24d 纹理槽为空时抛的不是那条（次序 / 门写错）");
        }
        if (!filled(machine, 73)) {
            return fail("0x24d 在判纹理槽之前没有填媒体格（次序反了 / 这一条只写了命令记录）");
        }
        const auto record = machine.scene_commands().find(70);
        if (record == machine.scene_commands().end() || record->second.kind != 0) {
            return fail("0x24d 在抛出前就把旧命令记录删了（正文：旧记录不动）");
        }
        // (ii) 纹理槽非空 ⇒ 记录写成种类 1；媒体格也在。
        auto ok_script = make_script_with_body();
        emit_instruction(ok_script, 0x1F8, {{0, 74}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(ok_script, 0x24D,
                         {{0, 71}, {0, 74}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},
                          {0, 0}, {0, 0}, {0, 0}, {0, 0}});
        emit_instruction(ok_script, 2, {});
        core::Machine ok_machine(0x12345678u, table);
        ok_machine.load_root(ok_script);
        for (int i = 0; i < 2; ++i) {
            if (!ok_machine.step()) {
                std::fprintf(stderr, "0x24d 正例第 %d 步停机：%s\n", i + 1,
                             ok_machine.last_stop().message.c_str());
                return fail("0x24d 正例没跑起来");
            }
        }
        const auto ok_record = ok_machine.scene_commands().find(71);
        if (ok_record == ok_machine.scene_commands().end() || ok_record->second.kind != 1) {
            return fail("0x24d 没有把命令记录写成种类 1");
        }
        if (!filled(ok_machine, 74)) return fail("0x24d 没有填媒体对象表（只写了命令记录）");
    }
    // ④ 清 ①：纹理族三条写指令在建立 / 释放之前清**自己那个编号**的一格。
    {
        // `0x1f8`（第 1 个操作数）：同一编号上已有播放对象 ⇒ 建纹理之前必须先清掉它
        //   （⛔ 不是"对象与纹理共存"）；**邻格不动**（清的只是它自己那个编号）。
        // ★ 为什么不在这里用起播的影片当夹具：影片位一置起，门就不再派发脚本（`CORE-VM-MOVIE`）
        //   ⇒ 起播后**根本走不到**下一条 `0x1f8`。所以这一格用"填格指令建出来的播放类对象"。
        auto script = make_script_with_body();
        emit_instruction(script, 0x236, {{0, 0}, {0, 42}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x240, {{0, 0}, {0, 43}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x1F8, {{0, 42}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        for (int i = 0; i < 3; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "0x1f8 清格夹具第 %d 步停机：%s\n", i + 1,
                             machine.last_stop().message.c_str());
                return fail("0x1f8 清格夹具没跑起来");
            }
        }
        if (filled(machine, 42)) {
            return fail("0x1f8 建纹理前没有清该编号的共享对象槽（媒体对象与纹理共存）");
        }
        if (!filled(machine, 43)) {
            return fail("0x1f8 把别的编号的格子也清了（服务范围是**它自己那个编号**）");
        }
        // `0x1f9`（第 2 个操作数）：清发生在读第 1 个操作数之前 ⇒ 后面「取文件」失败也清过了。
        auto f9_script = make_script_with_body();
        emit_instruction(f9_script, 0x236, {{0, 0}, {0, 43}, {0, 0}, {0, 0}});
        emit_instruction(f9_script, 0x1F9, {{0, 1}, {0, 43}, {0, 0}});
        emit_instruction(f9_script, 2, {});
        core::Machine f9_machine(0x12345678u, table);  // 没有安装目录 ⇒ 走到「取文件」时抛
        f9_machine.load_root(f9_script);
        if (!f9_machine.step()) return fail("0x1f9 夹具的填格步骤停机了");
        if (f9_machine.step()) return fail("0x1f9 在没有安装目录时没有抛出（夹具失效）");
        if (filled(f9_machine, 43)) {
            return fail("0x1f9 没有在读第 1 个操作数之前清该编号的共享对象槽（媒体对象还在）");
        }
        // `0x1fa`（第 1 个操作数）。
        auto fa_script = make_script_with_body();
        emit_instruction(fa_script, 0x240, {{0, 0}, {0, 44}, {0, 0}, {0, 0}});
        emit_instruction(fa_script, 0x1FA, {{0, 44}});
        emit_instruction(fa_script, 2, {});
        core::Machine fa_machine(0x12345678u, table);
        fa_machine.load_root(fa_script);
        for (int i = 0; i < 2; ++i) {
            if (!fa_machine.step()) {
                std::fprintf(stderr, "0x1fa 夹具第 %d 步停机：%s\n", i + 1,
                             fa_machine.last_stop().message.c_str());
                return fail("0x1fa 清格夹具没跑起来");
            }
        }
        if (filled(fa_machine, 44)) {
            return fail("0x1fa 释放纹理前没有清该编号的共享对象槽");
        }
    }
    // ④ 清 ②：`0x210` 取**第 1 个**操作数、`0x249` 取**第 2 个**，把该格释放并置空。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x236, {{0, 0}, {0, 45}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x236, {{0, 0}, {0, 46}, {0, 0}, {0, 0}});
        emit_instruction(script, 0x210, {{0, 45}});                    // 第 1 个操作数 = 槽号
        emit_instruction(script, 0x240, {{0, 0}, {0, 48}, {0, 0}, {0, 0}});
        // `0x249`：第 2 个操作数 = 槽号。0x249 把第 1 个操作数也拿来当槽号 ⇒ 清的是 48、46 留着 ⇒ 红。
        emit_instruction(script, 0x249, {{0, 48}, {0, 46}, {0, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        for (int i = 0; i < 5; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "清格夹具第 %d 步停机：%s\n", i + 1,
                             machine.last_stop().message.c_str());
                return fail("0x210 / 0x249 没有 handler（执行到就抛）");
            }
        }
        if (filled(machine, 45)) {
            return fail("0x210 没有把该格的对象释放并置空（它取**第 1 个**操作数）");
        }
        if (filled(machine, 46)) {
            return fail("0x249 没有把该格的对象释放并置空（它取**第 2 个**操作数）");
        }
        if (!filled(machine, 48)) return fail("夹具：0x240 的第 2 个操作数（48）没填上");
        if (!machine.errors().empty()) return fail("清格指令写了错误通道（正文：不报错）");
    }
    // ④ 清 ③：每拍扫描 —— 到结束条件且**循环位 = 0** ⇒ 释放并置空；循环位 = 1 ⇒ 留着。
    {
        // (i) 非循环：跑到结束之后那一格必须变空；同时**没被喂过的格子不得被扫描清掉**
        //     （"把创建当成结束"的坏法）。
        FakeMovieStats stats;
        auto script = make_script_with_body();
        emit_instruction(script, 0x236, {{0, 0}, {0, 50}, {0, 0}, {0, 0}});  // 只建、未启动
        emit_instruction(script, 0x1F8, {{0, 42}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(script, 0x20F, {{0, 7}, {0, 42}, {0, 0}});
        emit_instruction(script, 0x1A8, {});
        emit_instruction(script, 2, {});
        core::ConfigStore config;
        config.set_int("set:DrawMode", 1);   // 场景钟要真的走（后端按引擎时钟换算"当前位置"）
        config.set_int("set:CreateObject", 2);  // 构造期位下标：第 1 位置起 ⇒ 不改写
        core::Machine machine(0x12345678u, table, nullptr, config);
        machine.set_scalar_21b(1);
        install_fake_movie(machine, stats);
        machine.load_root(script);
        for (int i = 0; i < 3; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "扫描夹具第 %d 步停机：%s\n", i + 1,
                             machine.last_stop().message.c_str());
                return fail("每拍扫描夹具没跑到起播");
            }
        }
        if (!filled(machine, 42) || !filled(machine, 50)) return fail("扫描夹具：对象没建出来");
        bool finished = false;
        for (int i = 0; i < 400; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "扫描夹具运行中停机：%s\n",
                             machine.last_stop().message.c_str());
                return fail("影片播放期间停机");
            }
            if (!filled(machine, 42)) {
                finished = true;
                break;
            }
        }
        if (!finished) {
            return fail("非循环影片播完之后那一格仍在（每拍扫描没有释放 / 循环位判据反了）");
        }
        if (!stats.closed) return fail("扫描销毁时没有关闭该对象的播放后端（只置了空）");
        if (!filled(machine, 50)) {
            return fail("每拍扫描把「只建、未启动」的格子也清了（把「创建」当成了「结束」）");
        }
        if (machine.movie_active()) return fail("扫描销毁之后「有活动对象」仍为真");
        // (ii) 循环位 = 1：到结束条件也**不**销毁（回到开头重新启动，槽保持）。
        FakeMovieStats loop_stats;
        auto loop_script = make_script_with_body();
        emit_instruction(loop_script, 0x1F8, {{0, 42}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(loop_script, 0x20F, {{0, 7}, {0, 42}, {0, 1}});
        emit_instruction(loop_script, 0x1A8, {});
        emit_instruction(loop_script, 2, {});
        core::Machine loop_machine(0x12345678u, table, nullptr, config);
        loop_machine.set_scalar_21b(1);
        install_fake_movie(loop_machine, loop_stats);
        loop_machine.load_root(loop_script);
        for (int i = 0; i < 200; ++i) {
            if (!loop_machine.step()) {
                std::fprintf(stderr, "循环夹具第 %d 步停机：%s\n", i + 1,
                             loop_machine.last_stop().message.c_str());
                return fail("循环影片夹具没跑起来");
            }
        }
        if (!filled(loop_machine, 42) || loop_stats.rewinds < 1) {
            std::fprintf(stderr, "循环影片：槽 42 %s、rewinds=%d\n",
                         filled(loop_machine, 42) ? "在" : "被清了", loop_stats.rewinds);
            return fail("循环位 = 1 时每拍扫描仍然销毁了对象（结束条件漏了循环位）");
        }
    }
    // ④ 清 ⑤：`0x23d` 的整表遍历 = 槽号区间 `42..999`（0..41 不动、上界是"上限一千"）。
    {
        auto script = make_script_with_body();
        emit_instruction(script, 0x236, {{0, 0}, {0, 41}, {0, 0}, {0, 0}});     // 区间**左端之外**
        emit_instruction(script, 0x236, {{0, 0}, {0, 42}, {0, 0}, {0, 0}});     // 区间**左端**
        emit_instruction(script, 0x236, {{0, 0}, {0, 999}, {0, 0}, {0, 0}});    // 区间**右端之内**
        emit_instruction(script, 0x236, {{0, 0}, {0, 1000}, {0, 0}, {0, 0}});   // 表外（不在 `42..999`）
        emit_instruction(script, 0x23D, {});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        machine.load_root(script);
        for (int i = 0; i < 5; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "0x23d 夹具第 %d 步停机：%s\n", i + 1,
                             machine.last_stop().message.c_str());
                return fail("0x23d 的填格 / 退出清理没跑起来");
            }
        }
        if (!filled(machine, 41)) return fail("0x23d 动了槽 0..41（合同：0..41 不动）");
        if (filled(machine, 42)) return fail("0x23d 没有清掉区间左端（42）的媒体对象");
        if (filled(machine, 999)) return fail("0x23d 没有清掉区间右端之内（999）的媒体对象");
        if (!filled(machine, 1000)) {
            return fail("0x23d 把区间上界写成了「全部槽」（表外落点不该被这条遍历碰到）");
        }
        if (!machine.errors().empty()) return fail("0x23d 写了错误通道（正文：空槽静默跳过、不报错）");
    }
    // ⑤ **上限一千 = 扫描的循环边界**，不是指令选格时的判据：槽号 1000 的填**不被拒绝**。
    {
        FakeMovieStats stats;
        auto script = make_script_with_body();
        emit_instruction(script, 0x1F8, {{0, 1000}, {0, 4}, {0, 4}, {0, 0}});  // 0x1f8 组不做范围检查
        emit_instruction(script, 0x20F, {{0, 7}, {0, 1000}, {0, 0}});
        emit_instruction(script, 2, {});
        core::Machine machine(0x12345678u, table);
        install_fake_movie(machine, stats);
        machine.load_root(script);
        for (int i = 0; i < 2; ++i) {
            if (!machine.step()) {
                std::fprintf(stderr, "槽号 1000 夹具第 %d 步停机：%s\n", i + 1,
                             machine.last_stop().message.c_str());
                return fail("槽号 1000 被拒绝了（把「上限一千是扫描边界」写成了指令的判据）");
            }
        }
        if (!machine.errors().empty()) {
            return fail("槽号 1000 进了错误通道（那条五条的填格指令不检查槽号）");
        }
        if (!filled(machine, 1000)) {
            return fail("槽号 1000 的填没有落进表外落点（本模型：不检查、不越界）");
        }
    }
    return true;
}

// ── 渲染步的**批次闸**（`CORE-OPS-RENDER-FRAME-STEP` 的「渲染步（合同边界）」）──
/** 判据全部落在两个可观察面上：**渲染器请求序列**（`renderer_requests()`）与**引擎格**
 *  （脏位 / 「强制完成」/ 渲染步的"现在" / 命令表 / 当前渲染目标），⛔ 不是"读代码读起来对"。
 *
 *  ⓪ 清屏位判定在**开始批次之前**：把该位注入非 0 ⇒ 请求序列 = `清屏 → 开始批次 → 结束批次 → 呈现`
 *     （正文点名：⛔ 不许把"开始批次"写成渲染步的第一个动作）。该位在复位（`0x9`）时清 0。
 *  ① 成功一步（步末目标 = 后缓冲）：序列恰为 `开始批次 → 结束批次 → 呈现` ——
 *     **批次在呈现之前结束**，呈现是**最后一项**平台绘制请求；全程**没有清屏请求**
 *     （渲染步从不自行清屏）。步末格被写（脏位清 0、「强制完成」清 0）。
 *  ② 呈现的门 = 步末当前渲染目标是后缓冲：目标 = 纹理槽 ⇒ 步照跑，但**不提交**（无呈现请求）。
 *  ③ 开始批次**被拒绝** ⇒ 整步立即结束：序列只有 `开始批次`（**没有**结束批次 / 呈现 / 清屏）、
 *     不遍历（遍历计数不增、画布为空）、**不写任何步末格**（脏位 / 「强制完成」/ 渲染步的"现在"）
 *     且不清命令表。
 *  ④ 拒绝不是永久的：同一台机器上解除注入 ⇒ 下一步照常走完整序列并清命令表。
 *  ⑤ 「渲染步从不自行清屏」**不是恒真**：同一个请求通道里，显式清屏指令 `0x20e` 必须记一条清屏请求。
 *
 *  把「结束批次」写成无条件配对 / 呈现门删掉 / 闸挪到遍历之后 / 收尾挪到闸之前 /
 *  渲染步里塞一条清屏请求 ⇒ 本条红。 */
bool check_render_batch_gate(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "render-batch-gate: %s\n", what);
        return false;
    };
    using Req = core::Machine::RendererRequest;
    const auto log = [](const core::Machine& machine) {
        std::string text;
        for (const auto request : machine.renderer_requests()) {
            const char* name = request == Req::ClearScreen ? "clear"
                               : request == Req::BeginBatch ? "begin"
                               : request == Req::EndBatch  ? "end"
                                                           : "present";
            if (!text.empty()) text += ',';
            text += name;
        }
        return text;
    };
    const auto expect_log = [&](const core::Machine& machine, const std::string& want,
                                const char* what) {
        if (log(machine) != want) {
            std::fprintf(stderr, "render-batch-gate: %s（请求序列 = [%s]，期望 [%s]）\n", what,
                         log(machine).c_str(), want.c_str());
            return false;
        }
        return true;
    };
    // ⓪ 清屏位判定在开始批次之前；复位把它清 0。
    {
        core::Machine machine(key, table);
        if (machine.scene_clear_bit() != 0) return fail("清屏位初值不是 0（本版没有置 1 的来源）");
        machine.set_scene_clear_bit_for_test(1);
        (void)core::rasterize_scene(machine, 4, 4);
        if (!expect_log(machine, "clear,begin,end,present",
                        "清屏位非 0 时，清屏请求必须排在开始批次**之前**"
                        "（开始批次不是渲染步的第一个动作）")) return false;
        // 复位（`0x9`）是**该位唯一的写点**（写 0）：复位体在装载根脚本之前就发生 ⇒ 没有装载器
        // 也能观察到这一格（`exit-script-loader-missing` 只是随后停机）。
        auto reset_script = make_script_with_body();
        emit_instruction(reset_script, 0x09, {});
        emit_instruction(reset_script, 2, {});
        core::Machine reset_machine(key, table);
        reset_machine.set_scene_clear_bit_for_test(1);
        reset_machine.load_root(reset_script);
        (void)reset_machine.step();
        if (reset_machine.scene_clear_bit() != 0) {
            return fail("复位没有把清屏位清 0（正文：该位唯一的写点是复位时清 0）");
        }
    }
    // ① 成功一步（步末目标 = 后缓冲，缺省 −1）。
    {
        core::Machine machine(key, table);
        machine.mark_dirty();
        machine.force_complete();
        const core::Bytes canvas = core::rasterize_scene(machine, 8, 4);
        if (canvas.size() != 8u * 4u * 4u) return fail("成功的一步没有产出画布（遍历没跑）");
        if (machine.render_traversals() != 1) return fail("成功的一步遍历次数 ≠ 1");
        if (machine.renderer_requests().empty() ||
            machine.renderer_requests().back() != Req::Present) {
            return fail("呈现不是渲染步的最后一项请求");
        }
        if (!expect_log(machine, "begin,end,present",
                        "成功一步的请求序列不是 开始批次 → 结束批次 → 呈现")) return false;
        if (machine.renderer_request_count(Req::ClearScreen) != 0) {
            return fail("渲染步自行发了清屏请求（正文：渲染步从不自行清屏）");
        }
        // 请求序列两条：① 结束批次在呈现之前；② 各恰一次。
        if (machine.renderer_request_count(Req::BeginBatch) != 1 ||
            machine.renderer_request_count(Req::EndBatch) != 1 ||
            machine.renderer_request_count(Req::Present) != 1) {
            return fail("成功一步里某个请求的次数不是 1（配对 / 提交都该恰一次）");
        }
        if (machine.scene_dirty() || machine.forced_complete()) {
            return fail("成功的一步没有写步末格（脏位 / 「强制完成」）");
        }
    }
    // ② 目标 = 纹理槽 ⇒ 步照跑，但**不提交**。
    {
        core::Machine machine(key, table);
        machine.set_texture_file(5, 1);  // 造一格纹理（`0x20d` 只接受已存在的编号）
        auto script = make_script_with_body();
        emit_instruction(script, 0x20D, {{0, 5}});
        emit_instruction(script, 2, {});
        machine.load_root(script);
        const auto result = machine.run(8);
        if (result.reason != core::StopReason::Halted || machine.render_target() != 5) {
            return fail("夹具没能把当前渲染目标切到纹理槽 5");
        }
        machine.clear_renderer_requests();
        const core::Bytes canvas = core::rasterize_scene(machine, 8, 4);
        if (canvas.size() != 8u * 4u * 4u) return fail("目标是纹理时渲染步没有照跑（不遍历 / 不绘制）");
        if (!expect_log(machine, "begin,end", "目标是纹理时不该提交（呈现门 = 后缓冲）")) return false;
        if (machine.renderer_request_count(Req::Present) != 0) {
            return fail("目标是纹理却发了一次呈现请求");
        }
    }
    // ③ 开始批次被拒绝 ⇒ 整步立即结束。
    {
        core::Machine machine(key, table);
        machine.mark_dirty();
        machine.force_complete();
        machine.reset_scene_clock(100);  // 让下面那条命令的进度已到期（否则它会置"有动画"）
        // 遍历的**引擎状态后果**也要咬住：种类 0 的命令记录会在遍历里把目标纹理的内容版本 +1。
        auto& command_texture = machine.texture_entry(3);
        command_texture.slot.present = true;
        command_texture.slot.width = 4;
        command_texture.slot.height = 4;
        command_texture.slot.rgba.assign(4u * 4u * 4u, 0u);
        const auto revision_before = command_texture.slot.revision;
        core::SceneCommandRecord command;
        command.kind = 0;
        command.texture_id = 3;
        command.timestamp = 1;  // 起点已过 ⇒ 进度 = 1（不置「有动画」、不挡住收尾清表）
        machine.scene_commands_mut()[1] = command;
        // 再留一条空记录 ⇒ 「不清命令表」另有可观察量（成功的一步会把整张表清空）。
        machine.scene_commands_mut()[7] = core::SceneCommandRecord{};
        machine.set_scene_pending(false);
        machine.set_renderer_begin_batch_refuses(true);
        const core::Bytes canvas = core::rasterize_scene(machine, 8, 4);
        if (!canvas.empty()) return fail("开始批次被拒绝却仍产出了画布（遍历 / 绘制没有立刻结束）");
        if (machine.render_traversals() != 0) return fail("开始批次被拒绝却仍遍历了场景");
        if (machine.texture_slot(3)->revision != revision_before) {
            return fail("开始批次被拒绝却仍遍历了场景（目标纹理的内容版本被推进）");
        }
        if (!expect_log(machine, "begin", "被拒绝的一步只应发出『开始批次』")) return false;
        if (machine.renderer_request_count(Req::EndBatch) != 0) {
            return fail("开始批次被拒绝却仍发出了配对的『结束批次』（不是无条件配对）");
        }
        if (machine.renderer_request_count(Req::Present) != 0) {
            return fail("开始批次被拒绝却仍发出呈现请求");
        }
        if (machine.renderer_request_count(Req::ClearScreen) != 0) {
            return fail("开始批次被拒绝的那一步发了清屏请求（渲染步从不自行清屏）");
        }
        if (!machine.scene_dirty()) return fail("开始批次被拒绝却写了步末格（脏位被清）");
        if (!machine.forced_complete()) return fail("开始批次被拒绝却写了步末格（「强制完成」被清）");
        if (machine.render_now_ms() != 0) {
            return fail("开始批次被拒绝却写了步头格（渲染步的“现在”）");
        }
        if (machine.scene_commands().size() != 2) {
            return fail("开始批次被拒绝却清了命令表");
        }
        // ④ 拒绝不是永久的：解除注入 ⇒ 下一步照常，并且这一步才清命令表。
        machine.set_renderer_begin_batch_refuses(false);
        machine.clear_renderer_requests();
        const core::Bytes again = core::rasterize_scene(machine, 8, 4);
        if (again.size() != 8u * 4u * 4u) return fail("解除注入之后渲染步没有恢复");
        if (!expect_log(machine, "begin,end,present", "解除注入之后的请求序列")) return false;
        // ★ 反向控制：恢复后那条命令必须**真的**推进过目标纹理的内容版本 —— 否则上一段的
        //   「版本没变」是恒真的（夹具从来没被遍历到）。
        if (machine.texture_slot(3)->revision == revision_before) {
            return fail("夹具没有触发遍历（恢复后目标纹理的内容版本没变）⇒ 「拒绝不绘制」成了恒真断言");
        }
        if (machine.scene_commands().size() != 0) return fail("恢复后的成功一步没有清命令表");
    }
    // ⑤ 清屏请求**只**随显式清屏指令出现（让「渲染步从不自行清屏」不是恒真断言）。
    {
        core::Machine machine(key, table);
        auto script = make_script_with_body();
        emit_instruction(script, 0x20E, {});
        emit_instruction(script, 2, {});
        machine.load_root(script);
        const auto result = machine.run(8);
        if (result.reason != core::StopReason::Halted) return fail("0x20e 的夹具没有跑完");
        if (machine.renderer_request_count(Req::ClearScreen) != 1) {
            return fail("显式清屏指令 0x20e 没有记一条清屏请求"
                        "（那样「渲染步从不自行清屏」就成了恒真断言）");
        }
        machine.clear_renderer_requests();
        (void)core::rasterize_scene(machine, 4, 4);
        if (machine.renderer_request_count(Req::ClearScreen) != 0) {
            return fail("0x20e 之后紧跟的渲染步又发了一条清屏请求");
        }
    }
    return true;
}

// ══ 存档布局 1（`CORE-OPS-SAVE-VERSION-AE`，`60-ops-scalar.md:306-321`）══
// 夹具构造：一帧暂存块 = 32B 记录头（+4 = 4 字节 ASCII 格式标记）→ 4B 长度前缀（**含自身**）→ 六个 u32。
void put_i32_at(std::vector<std::uint8_t>& bytes, std::size_t offset, std::int32_t value) {
    const auto raw = static_cast<std::uint32_t>(value);
    for (int byte = 0; byte < 4; ++byte) {
        bytes[offset + static_cast<std::size_t>(byte)] =
            static_cast<std::uint8_t>((raw >> (8 * byte)) & 0xFFu);
    }
}
std::vector<std::uint8_t> make_staged_block(const char* marker, const std::vector<std::int32_t>& words,
                                            std::int32_t prefix = -1) {
    std::vector<std::uint8_t> bytes(32, 0);
    std::memcpy(bytes.data() + 4, marker, 4);  // ★ 4 字节 ASCII 标记（与长度前缀是**两格**）
    put_u32(bytes, static_cast<std::uint32_t>(
                       prefix >= 0 ? prefix : 4 + 4 * static_cast<std::int32_t>(words.size())));
    for (const auto word : words) put_u32(bytes, static_cast<std::uint32_t>(word));
    return bytes;
}
std::vector<std::uint8_t> make_code_body(const std::vector<std::int32_t>& words) {
    std::vector<std::uint8_t> bytes;
    for (const auto word : words) put_u32(bytes, static_cast<std::uint32_t>(word));
    return bytes;
}
// 帧管理器整块：定长头 **0x900** + 每帧一条记录（步长 **0x41C**）。两个下标数组的起点在块内 **+0x8F8**
// （定长头里）且**步长同 = 0x41C** ⇒ 第 i 帧的两格落在 `0x8F8 + 0x41C × i` / `0x8FC + 0x41C × i`。
std::vector<std::uint8_t> make_frame_manager_block(
    const std::vector<std::pair<std::int32_t, std::int32_t>>& indices) {
    std::vector<std::uint8_t> block(0x900 + 0x41C * indices.size(), 0);
    for (std::size_t i = 0; i < indices.size(); ++i) {
        const std::size_t stride_offset = 0x41C * i;
        put_i32_at(block, 0x8F8 + stride_offset, indices[i].first);   // 优先族（换脚本：第二对表）
        put_i32_at(block, 0x8FC + stride_offset, indices[i].second);  // 回退族（另一族：第一对表）
    }
    return block;
}

// 判据（每条都能红）：
// ① 32 字节记录头 → 4 字节长度前缀（**含自身**）→ 六个 u32 成三对（整数 / 偏移**不许对调**）；
// ② 格式标记按**无符号字节逐比**与 `"080 "` 比：小于 ⇒ 旧格式支（**负载不读**）、不小于 ⇒ 三对支；
//    ⛔ 按 4 字节整数比 ⇒ `"/zzz"` 会落进三对支 ⇒ 本条的 ③ 段红；
// ③ 同一格 > 引擎自己的标记 ⇒ 报「引擎版本太旧」，但**分档不变**；
// ④ 长度前缀 N 的负载长度 = **N − 4**（把它当成 N 或把前缀算在负载之外 ⇒ ⑤ / ⑥ 段红）；
// ⑤ 记录头读不满 32 字节 ⇒ 不合法（⛔ 不是"读多少算多少"）。
bool check_save_layout1_record(const core::OpcodeTable& table) {
    (void)table;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "save-layout1-record: %s\n", what);
        return false;
    };
    // 三对 = (2, 8) / (3, 4) / (0, 0)：每对的整数与偏移**互不相同** ⇒ 对调必然红。
    const std::vector<std::int32_t> pairs = {2, 8, 3, 4, 0, 0};
    const auto marker = [](const char* text) {
        return core::ByteSpan(reinterpret_cast<const std::uint8_t*>(text), 4);
    };
    // ① `"080 "`（与常量相等 ⇒ 不小于）⇒ 按三对解释。
    {
        const auto block = make_staged_block("080 ", pairs);
        const auto staged = core::parse_save_staged_frame(block, core::ByteSpan{});
        if (!staged.header_ok) return fail("32 字节记录头没有读满");
        if (staged.marker != core::SaveFormatMarker::Current) return fail("\"080 \" 没有落进三对支");
        if (!staged.payload_read) return fail("\"080 \" 的负载没有读");
        if (staged.pairs[0].integer != 2 || staged.pairs[0].word_offset != 8) {
            return fail("第一对的整数 / 偏移被对调（或没读对）");
        }
        if (staged.pairs[1].integer != 3 || staged.pairs[1].word_offset != 4) {
            return fail("第二对的整数 / 偏移被对调（或没读对）");
        }
        if (staged.pairs[2].integer != 0 || staged.pairs[2].word_offset != 0) {
            return fail("第三对（保留槽位）读出来不对");
        }
    }
    // ② `"070 "` < `"080 "` ⇒ 旧格式支：**负载不读**（三对保持 0），即使负载里放着六个非零数。
    {
        const auto block = make_staged_block("070 ", pairs);
        const auto staged = core::parse_save_staged_frame(block, core::ByteSpan{});
        if (staged.marker != core::SaveFormatMarker::Legacy) return fail("\"070 \" 没有被判成旧格式");
        if (staged.payload_read) return fail("旧格式支读了负载");
        if (staged.pairs[0].integer != 0 || staged.pairs[1].word_offset != 0) {
            return fail("旧格式支填了三对");
        }
    }
    // ③ ★ 关键分档：`"/zzz"`。**字符串**口径：首字节 0x2F < `'0'` ⇒ 比 `"080 "` 小 ⇒ 旧格式支。
    //    **小端整数**口径：0x7A7A7A2F ≫ `"080 "` 的 0x20303830 ⇒ 会走三对支 ⇒ 这里必红。
    {
        const auto block = make_staged_block("/zzz", pairs);
        const auto staged = core::parse_save_staged_frame(block, core::ByteSpan{});
        if (staged.marker != core::SaveFormatMarker::Legacy) {
            return fail("格式标记按**整数**比了（\"/zzz\" 被判成比 \"080 \" 大）");
        }
        if (staged.payload_read) return fail("\"/zzz\" 走了三对支");
    }
    // ④ 同一格 > 引擎自己的标记 ⇒ 报「引擎版本太旧」，分档不变（仍按三对解释）。
    {
        const auto block = make_staged_block("081 ", pairs);
        const auto staged = core::parse_save_staged_frame(block, marker("080 "));
        if (!staged.engine_outdated) return fail("标记 > 引擎自己的标记却没有报「引擎版本太旧」");
        if (staged.marker != core::SaveFormatMarker::Current) return fail("「引擎版本太旧」改变了分档");
        if (!staged.payload_read) return fail("「引擎版本太旧」把负载也吞了");
        const auto same = core::parse_save_staged_frame(block, marker("081 "));
        if (same.engine_outdated) return fail("标记 = 引擎自己的标记却报了「引擎版本太旧」");
        const auto no_engine = core::parse_save_staged_frame(block, core::ByteSpan{});
        if (no_engine.engine_outdated) return fail("引擎标记为空（来源未取证）时仍判了「引擎版本太旧」");
    }
    // ⑤ 长度前缀**含自身**：N = 4 + 24 = 28、负载恰好 24 字节 ⇒ 读；负载截掉 4 字节 ⇒ 短读、不读。
    {
        const auto full = make_staged_block("080 ", pairs);  // prefix = 28，负载 24
        if (!core::parse_save_staged_frame(full, core::ByteSpan{}).payload_read) {
            return fail("完整块（前缀 28 + 负载 24）没有读");
        }
        const core::Bytes trimmed(full.begin(), full.end() - 4);
        if (core::parse_save_staged_frame(trimmed, core::ByteSpan{}).payload_read) {
            return fail("负载少 4 字节仍按三对读（长度前缀被算在负载之外？）");
        }
    }
    // ⑥ 反向：前缀写成 **24**（= 4 + 20，只够五个 u32）而负载给了六个 ⇒ 声明的负载装不下六个 u32
    //    ⇒ **不读**。把 N 当负载长度（24）的实现会读进六个数 ⇒ 这里红。
    {
        const auto block = make_staged_block("080 ", pairs, 24);
        if (core::parse_save_staged_frame(block, core::ByteSpan{}).payload_read) {
            return fail("前缀把长度算成\"不含自身\"了（N = 24 却读进了六个数）");
        }
    }
    // ⑦ 记录头读不满 32 字节 ⇒ header_ok = false。
    {
        core::Bytes short_block(31, 0);
        if (core::parse_save_staged_frame(short_block, core::ByteSpan{}).header_ok) {
            return fail("31 字节的记录头被判成读满");
        }
    }
    return true;
}

// 判据（每条都能红）：
// ① 帧管理器**整块**回填：两个「每帧当前位置下标」的值**来自存档**（块内 +0x8F8 / +0x8FC，步长 0x41C）；
//    未被覆盖的帧槽**一格不动**；长度不是「定长头 + 整数条记录」⇒ 拒 + 一格不写 + 诊断；
// ② 重建：代码块整块读入、三对写进帧记录、**指令指针初值 = 代码块基址**（⛔ 不从存档读回 IP）；
// ③ 两族位置还原：优先族读**第二对**表、推进 **3**；某族下标 < 0 ⇒ **回退另一族**（第一对表）、推进 **0**；
//    两族都 < 0 ⇒ 指令指针与推进格数**一格不改**（⛔ 不是写 0 / 写表首）；
// ④ `0xae` 的三档版本门：`(1, ≠20)` / 其它主版本 ⇒ **不应用**；`(1,20)` ⇒ 应用；标志 = 0 ⇒ 连键都不读；
//    应用之后按当前帧号分叉：等于存档帧号 ⇒ 清标志；不等 ⇒ 帧号 + 1（重装留痕）。
bool check_save_layout1_restore(const core::OpcodeTable& table) {
    constexpr std::uint32_t key = 0x12345678u;
    const auto fail = [](const char* what) {
        std::fprintf(stderr, "save-layout1-restore: %s\n", what);
        return false;
    };
    // 一帧代码块（字偏移）：字 4..7 = 优先族表 [9, 6, 5, 7]，字 8..11 = 回退族表 [1, 2, 3, 4]。
    const std::vector<std::int32_t> code_words = {0, 0, 0, 0, 9, 6, 5, 7, 1, 2, 3, 4, 0, 0, 0, 0};
    const std::vector<std::uint8_t> code_body = make_code_body(code_words);
    // 三对：第一对（回退族）表在字 8；第二对（优先族）表在字 4；第三对保留。
    const std::vector<std::int32_t> pairs = {2, 8, 3, 4, 0, 0};
    const std::vector<std::uint8_t> staged_block = make_staged_block("080 ", pairs);

    // ① 帧管理器整块。
    {
        core::Machine machine(key, table);
        const auto block = make_frame_manager_block({{1, 2}, {-1, 2}});
        machine.seed_save_frame_index_for_test(2, 7, 8);  // 「未被覆盖的帧槽」的对照值
        if (!machine.restore_save_frame_manager(block)) return fail("合法的帧管理器整块被拒了");
        if (machine.save_frame_index(0, true) != 1) return fail("帧 0 的优先族下标不是来自存档");
        if (machine.save_frame_index(0, false) != 2) return fail("帧 0 的回退族下标不是来自存档");
        if (machine.save_frame_index(1, true) != -1) {
            return fail("帧 1 的优先族下标（存档写 −1）没读出来");
        }
        if (machine.save_frame_index(1, false) != 2) return fail("帧 1 的回退族下标没读出来");
        if (machine.save_frame_index(2, true) != 7 || machine.save_frame_index(2, false) != 8) {
            return fail("未被覆盖的帧槽被改了（本版：一格不动）");
        }
        core::Bytes misaligned(0x900 + 4, 0);
        machine.seed_save_frame_index_for_test(0, 5, 6);
        machine.clear_errors();
        if (machine.restore_save_frame_manager(misaligned)) {
            return fail("长度不合「定长头 + 整数条记录」的整块没有被拒");
        }
        if (machine.save_frame_index(0, true) != 5) return fail("拒了却仍写了帧 0 的下标");
        if (machine.errors().empty()) return fail("拒了却没有诊断");
        core::Bytes too_short(0x900 - 1, 0);
        if (machine.restore_save_frame_manager(too_short)) return fail("短于定长头的整块没有被拒");
    }

    // ②③ 重建 + 两族位置还原（直接调应用步）。
    {
        core::Machine machine(key, table);
        machine.load_root(make_script_with_body());
        const auto staged = core::parse_save_staged_frame(staged_block, core::ByteSpan{});
        machine.rebuild_save_frame_layout1(0, code_body, staged);
        if (machine.frame().code.size() != code_body.size()) return fail("代码块没有整块读入");
        if (machine.frame().save_pairs[1].word_offset != 4) return fail("三对没有写进帧记录");
        if (!machine.frame().save_pairs_ready) return fail("三对没有标记为可用");
        if (machine.frame().ip != 0) return fail("指令指针初值不是**代码块基址**");
        // 优先族（第二对表）：下标 1 ⇒ 表[1] = 6 ⇒ ip = 4 × 6 = 24，推进 3。
        machine.seed_save_frame_index_for_test(0, 1, 2);
        if (!machine.resolve_save_layout1_position(0)) return fail("优先族命中却没报命中");
        if (machine.frame().ip != 24) {
            std::fprintf(stderr, "ip = %zu（期望 24 = 4 × 表[1]）\n", machine.frame().ip);
            return fail("优先族命中之后指令指针不是 代码基址 + 4 × 表[下标]");
        }
        if (machine.frame().length_slot != 3) return fail("优先族命中之后推进格数不是 3");
        // 优先族下标 < 0 ⇒ **回退另一族**（第一对表）：下标 2 ⇒ 表[2] = 3 ⇒ ip = 12，推进 0。
        machine.seed_save_frame_index_for_test(0, -1, 2);
        if (!machine.resolve_save_layout1_position(0)) return fail("优先族不命中时没有回退另一族");
        if (machine.frame().ip != 12) return fail("回退族的指令指针不是 代码基址 + 4 × 表[下标]");
        if (machine.frame().length_slot != 0) return fail("回退族命中之后推进格数不是 0");
        // 两族都 < 0 ⇒ 指令指针与推进格数**一格不改**。
        machine.seed_save_frame_index_for_test(0, -1, -1);
        if (machine.resolve_save_layout1_position(0)) return fail("两族都不命中却报了命中");
        if (machine.frame().ip != 12) return fail("两族都不命中却改了指令指针");
        if (machine.frame().length_slot != 0) return fail("两族都不命中却改了推进格数");
        // 下标越出代码块 ⇒ 该族不命中 ⇒ 仍回退另一族（并记诊断）。
        machine.clear_errors();
        machine.seed_save_frame_index_for_test(0, 100000, 1);
        if (!machine.resolve_save_layout1_position(0)) return fail("优先族下标越界时没有回退另一族");
        if (machine.frame().ip != 8) return fail("回退族下标 1 ⇒ 期望 ip = 8");
        if (machine.frame().length_slot != 0) return fail("回退族命中之后推进格数不是 0");
        if (machine.errors().empty()) return fail("表越出代码块却没有诊断");
        // 旧格式支（标记 < `"080 "`）⇒ 不重建：代码块不动、三对不可用。
        core::Machine legacy(key, table);
        legacy.load_root(make_script_with_body());
        const auto legacy_staged =
            core::parse_save_staged_frame(make_staged_block("070 ", pairs), core::ByteSpan{});
        legacy.rebuild_save_frame_layout1(0, code_body, legacy_staged);
        if (legacy.frame().code == code_body) return fail("旧格式支仍把代码块整块读进去了");
        if (legacy.frame().save_pairs_ready) return fail("旧格式支仍把三对标成可用");
        if (legacy.errors().empty()) return fail("旧格式支没有留痕");
    }

    // ④ `0xae` 的三档版本门 + 应用 + 帧号分叉。
    {
        const auto make_staged = [&](std::int32_t frame_number,
                                     const std::vector<std::pair<std::int32_t, std::int32_t>>& indices) {
            core::StagedSaveLayout1 staged;
            staged.present = true;
            staged.frame_number = frame_number;
            staged.frame_manager_block = make_frame_manager_block(indices);
            core::StagedSaveFrameLayout1 frame;
            frame.staged_block = staged_block;
            frame.code_body = code_body;
            staged.frames.push_back(std::move(frame));
            return staged;
        };
        const auto run_ae = [&](core::Machine& machine, std::int32_t version1, std::int32_t version2,
                                std::int32_t flag, const core::StagedSaveLayout1& staged) {
            auto script = make_script_with_body();
            emit_instruction(script, 0xAE, {});  // argc 0
            emit_instruction(script, 2, {});
            machine.config_mut().set_int("set:SaveVersion1", version1);
            machine.config_mut().set_int("set:SaveVersion2", version2);
            machine.seed_save_version_flag_for_test(flag);
            machine.seed_staged_save_for_test(staged);
            machine.load_root(script);
            return machine.step();
        };
        // 等于存档帧号 + 优先族命中 ⇒ ip = 4 × 6 = 24，推进 3（派发器再推 3 字 ⇒ 36），清标志。
        {
            core::Machine machine(key, table);
            if (!run_ae(machine, 1, 20, 1, make_staged(0, {{1, 2}}))) {
                std::fprintf(stderr, "停因 = %s\n", machine.last_stop().message.c_str());
                return fail("0xae 的布局 1 一支把机器停了");
            }
            if (machine.frame().ip != 36) {
                std::fprintf(stderr, "ip = %zu（期望 24 + 3 字 = 36）\n", machine.frame().ip);
                return fail("优先族命中之后的指令指针不对（推进 3 个字也没体现）");
            }
            if (machine.frame().length_slot != 3) return fail("优先族的推进格数不是 3");
            if (machine.frame().save_pairs[0].integer != 2 ||
                machine.frame().save_pairs[1].integer != 3 ||
                machine.frame().save_pairs[2].integer != 0) {
                return fail("三对没有从暂存块写进帧记录的三对槽");
            }
            if (machine.save_version_flag() != 0) return fail("等于存档帧号时没有清标志");
            if (machine.current_frame() != 0) return fail("等于存档帧号却改了帧号");
        }
        // 优先族下标 = −1 ⇒ 回退另一族：ip = 4 × 3 = 12，推进 0（派发器不再推）。
        {
            core::Machine machine(key, table);
            if (!run_ae(machine, 1, 20, 1, make_staged(0, {{-1, 2}}))) {
                return fail("回退族一支把机器停了");
            }
            if (machine.frame().ip != 12) return fail("回退族的指令指针不是 4 × 表[下标]");
            if (machine.frame().length_slot != 0) return fail("回退族的推进格数不是 0");
            if (machine.save_version_flag() != 0) return fail("等于存档帧号时没有清标志");
        }
        // 当前帧号 ≠ 存档帧号 ⇒ 帧号 + 1（重装留痕），**不清标志**。
        {
            core::Machine machine(key, table);
            if (!run_ae(machine, 1, 20, 1, make_staged(5, {{1, 2}}))) {
                return fail("帧号不等一支把机器停了");
            }
            if (machine.current_frame() != 1) return fail("帧号不等时没有帧号 + 1");
            if (machine.save_version_flag() != 1) return fail("帧号不等时却清了标志");
            bool reload_note = false;
            for (const auto& error : machine.errors()) {
                if (error.find("save-frame-reload-unmodeled") != std::string::npos) reload_note = true;
            }
            if (!reload_note) return fail("帧号不等时的「重装」既没做也没留痕");
            if (machine.frame_at(0).ip != 24) return fail("帧号不等时帧 0 的位置没有按两族算出来");
        }
        // 主版本 1 / 次版本 ≠ 20 ⇒ 不应用：推进格数 1、标志仍在、下标一格不动、无"容器未建模"。
        {
            core::Machine machine(key, table);
            machine.seed_save_frame_index_for_test(0, 7, 8);
            if (!run_ae(machine, 1, 0, 1, make_staged(0, {{1, 2}}))) {
                return fail("(1, 0) 一支把机器停了");
            }
            if (machine.frame().length_slot != 1) return fail("(1, 0) 的推进格数不是留在 1");
            if (machine.save_version_flag() != 1) return fail("(1, 0) 却清了标志");
            if (machine.save_frame_index(0, true) != 7 || machine.save_frame_index(0, false) != 8) {
                return fail("(1, 0) 却回填了帧管理器的下标");
            }
            if (machine.frame().save_pairs_ready) return fail("(1, 0) 却重建了帧");
            for (const auto& error : machine.errors()) {
                if (error.find("save-container-unmodeled") != std::string::npos) {
                    return fail("(1, 0) 不该走到布局 1 的应用步");
                }
            }
        }
        // 主版本 2（次版本 = 7 ≠ 20）⇒ 走**布局 2**（不比较次版本）；布局 2 未建模 ⇒ 响亮失败。
        {
            core::Machine machine(key, table);
            if (run_ae(machine, 2, 7, 1, make_staged(0, {{1, 2}}))) {
                return fail("主版本 2 没有响亮失败（布局 2 未建模）");
            }
            if (!machine.halted()) return fail("主版本 2 的失败没有停机");
            if (machine.last_stop().message.find("save-layout-unmodeled:2") == std::string::npos) {
                std::fprintf(stderr, "停因 = %s\n", machine.last_stop().message.c_str());
                return fail("主版本 2 的停因不是「布局 2 未建模」");
            }
        }
        // 标志 = 0 ⇒ 早退：**连一个命名键都不读**（这里两个键是 (2, 7)，读了就会抛）。
        {
            core::Machine machine(key, table);
            if (!run_ae(machine, 2, 7, 0, make_staged(0, {{1, 2}}))) {
                return fail("标志 = 0 时读了版本键（主版本 2 那支把它停了）");
            }
            if (machine.frame().length_slot != 1) return fail("标志 = 0 时推进格数不是 1");
            if (!machine.errors().empty()) return fail("标志 = 0 时留了诊断");
            if (machine.current_frame() != 0) return fail("标志 = 0 时改了帧号");
        }
        // 暂存区没有（读档路径未成篇）⇒ 不伪装：留痕、标志仍在、推进格数 1。
        {
            core::Machine machine(key, table);
            core::StagedSaveLayout1 absent;  // present = false
            if (!run_ae(machine, 1, 20, 1, absent)) return fail("没有暂存区时把机器停了");
            bool unmodeled = false;
            for (const auto& error : machine.errors()) {
                if (error.find("save-container-unmodeled") != std::string::npos) unmodeled = true;
            }
            if (!unmodeled) return fail("没有暂存区却没有留痕");
            if (machine.save_version_flag() != 1) return fail("没有暂存区却清了标志");
            if (machine.frame().length_slot != 1) return fail("没有暂存区时推进格数不是 1");
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
    bool only_text_pointer = false;
    bool only_text_gate = false;
bool only_clock_monotonic = false;
    bool only_text_compare = false;
    bool only_scene_self = false;
    bool only_scene_table = false;
    bool only_adv_choice = false;
    bool only_choice_window = false;
    bool only_voice_all_stop = false;
    bool only_misc_cells = false;
    bool only_scene_relocate = false;
    bool only_msg_block = false;
    bool only_scene_records = false;
    bool only_misc_ops = false;
    bool only_text_layer_fallback = false;
    bool only_adv_wait = false;
    bool only_texture_stretch = false;
    bool only_auto_message = false;
    bool only_choice_consumer = false;
    bool only_wait_timer = false;
    bool only_wait_colour_window = false;
    bool only_text_cell_reveal = false;
    bool only_rembit_arg_invalid = false;
    bool only_setbit_arg_invalid = false;
    bool only_font_weight_2bd = false;
    bool only_getbit_arg_invalid = false;
    bool only_random_range_zero = false;
    bool only_array_block_bounds = false;
    bool only_array_write_bounds = false;
    bool only_string_record_raw = false;
    bool only_error_channel = false;
    bool only_sort_12f_alias = false;
    bool only_pair_88 = false;
    bool only_bit27_service = false;
    bool only_adv_text_bit27 = false;
    bool only_font_pairs = false;
    bool only_font_scale = false;
    bool only_font_measure_wired = false;
    bool only_font_name_host = false;
    bool only_set_tail = false;
    bool only_text_span_bit2 = false;
    bool only_key_tables = false;
    bool only_input_composition = false;
    bool only_text_container = false;
    bool only_texture_set = false;
    bool only_frame_flow = false;
    bool only_text_layer_mode = false;
    bool only_text_layer_align = false;
    bool only_text_layer_position = false;
    bool only_se_registration = false;
    bool only_se_stop = false;
    bool only_event_library = false;
    bool only_output_object = false;
    bool only_forward_field = false;
    bool only_address_wrap = false;
    bool only_numeric_family = false;
    bool only_numtext_layout = false;
    bool only_display_state = false;
    bool only_texture_bit = false;
    bool only_mouse_pump_entry = false;
    bool only_getbit_nowrite = false;
    bool only_key_table_b = false;
    bool only_dispatch_default_read = false;
    // `CORE-OPS-RENDER-FRAME-STEP`「场景绘制模式」/ `CORE-OPS-SCENE-CG-NUMBER-23B`+四张表 /
    // `CORE-SCENE-DRAW-ORDER` 的收尾扫描：三条各自独立注册的 CTest（spec 证据要点得到名字）。
    bool only_draw_mode = false;
    bool only_cg_digit_tables = false;
    bool only_scene_draw_order = false;
    // `CORE-OPS-RENDER-FRAME-STEP` 的「媒体对象表（合同）」：填 5 条 / 清 5 条来源 / 上限一千。
    bool only_media_table = false;
    // `CORE-OPS-SCENE-320`（网格建 / 换）：读序 + 36 字节 / 9 分量的装配 + 三张副本 + 重烘门。
    bool only_mesh_320 = false;
    // `CORE-OPS-RENDER-FRAME-STEP` 的「渲染步（合同边界）」：清屏位判定 → **批次闸** →
    // 结束批次（只在成功之后）→ 呈现（门 = 步末目标是后缓冲）→ 步内收尾。
    bool only_render_batch = false;
    bool only_joy_axis = false;
    bool only_hit_wrap = false;
    bool only_touch_buffer = false;
    // `CORE-OPS-SAVE-VERSION-AE` 的**布局 1**（主版本 1 / 次版本 20）：暂存块分档 + 恢复与两族位置还原。
    bool only_save_layout1_record = false;
    bool only_save_layout1_restore = false;
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
        else if (std::strcmp(argv[i], "--only-text-pointer") == 0) only_text_pointer = true;
        else if (std::strcmp(argv[i], "--only-text-gate") == 0) only_text_gate = true;
        else if (std::strcmp(argv[i], "--only-clock-monotonic") == 0) only_clock_monotonic = true;
        else if (std::strcmp(argv[i], "--only-text-compare") == 0) only_text_compare = true;
        else if (std::strcmp(argv[i], "--only-scene-self") == 0) only_scene_self = true;
        else if (std::strcmp(argv[i], "--only-scene-table") == 0) only_scene_table = true;
        else if (std::strcmp(argv[i], "--only-adv-choice") == 0) only_adv_choice = true;
        else if (std::strcmp(argv[i], "--only-choice-window") == 0) only_choice_window = true;
        else if (std::strcmp(argv[i], "--only-voice-all-stop") == 0) only_voice_all_stop = true;
        else if (std::strcmp(argv[i], "--only-misc-cells") == 0) only_misc_cells = true;
        else if (std::strcmp(argv[i], "--only-scene-relocate") == 0) only_scene_relocate = true;
        else if (std::strcmp(argv[i], "--only-msg-block") == 0) only_msg_block = true;
        else if (std::strcmp(argv[i], "--only-scene-records") == 0) only_scene_records = true;
        else if (std::strcmp(argv[i], "--only-misc-ops") == 0) only_misc_ops = true;
        else if (std::strcmp(argv[i], "--only-text-layer-fallback") == 0) only_text_layer_fallback = true;
        else if (std::strcmp(argv[i], "--only-adv-wait") == 0) only_adv_wait = true;
        else if (std::strcmp(argv[i], "--only-texture-stretch") == 0) only_texture_stretch = true;
        else if (std::strcmp(argv[i], "--only-auto-message") == 0) only_auto_message = true;
        else if (std::strcmp(argv[i], "--only-choice-consumer") == 0) only_choice_consumer = true;
        else if (std::strcmp(argv[i], "--only-wait-timer") == 0) only_wait_timer = true;
        else if (std::strcmp(argv[i], "--only-wait-colour-window") == 0) only_wait_colour_window = true;
        else if (std::strcmp(argv[i], "--only-cell-reveal") == 0) only_text_cell_reveal = true;
        else if (std::strcmp(argv[i], "--only-rembit-arg-invalid") == 0) only_rembit_arg_invalid = true;
        else if (std::strcmp(argv[i], "--only-setbit-arg-invalid") == 0) only_setbit_arg_invalid = true;
        else if (std::strcmp(argv[i], "--only-font-weight-2bd") == 0) only_font_weight_2bd = true;
        else if (std::strcmp(argv[i], "--only-getbit-arg-invalid") == 0) only_getbit_arg_invalid = true;
        else if (std::strcmp(argv[i], "--only-random-range-zero") == 0) only_random_range_zero = true;
        else if (std::strcmp(argv[i], "--only-array-block-bounds") == 0) only_array_block_bounds = true;
        else if (std::strcmp(argv[i], "--only-array-write-bounds") == 0) only_array_write_bounds = true;
        else if (std::strcmp(argv[i], "--only-string-record-raw") == 0) only_string_record_raw = true;
        else if (std::strcmp(argv[i], "--only-error-channel") == 0) only_error_channel = true;
        else if (std::strcmp(argv[i], "--only-12f-alias") == 0) only_sort_12f_alias = true;
        else if (std::strcmp(argv[i], "--only-88-mode-cells") == 0) only_pair_88 = true;
        else if (std::strcmp(argv[i], "--only-bit27-service") == 0) only_bit27_service = true;
        else if (std::strcmp(argv[i], "--only-adv-text-bit27") == 0) only_adv_text_bit27 = true;
        else if (std::strcmp(argv[i], "--only-font-pairs") == 0) only_font_pairs = true;
        else if (std::strcmp(argv[i], "--only-font-scale") == 0) only_font_scale = true;
        else if (std::strcmp(argv[i], "--only-font-measure-wired") == 0) only_font_measure_wired = true;
        else if (std::strcmp(argv[i], "--only-font-name-host") == 0) only_font_name_host = true;
        else if (std::strcmp(argv[i], "--only-set-tail") == 0) only_set_tail = true;
        else if (std::strcmp(argv[i], "--only-text-span-bit2") == 0) only_text_span_bit2 = true;
        else if (std::strcmp(argv[i], "--only-key-tables") == 0) only_key_tables = true;
        else if (std::strcmp(argv[i], "--only-input-composition") == 0) only_input_composition = true;
        else if (std::strcmp(argv[i], "--only-text-container") == 0) only_text_container = true;
        else if (std::strcmp(argv[i], "--only-texture-set") == 0) only_texture_set = true;
        else if (std::strcmp(argv[i], "--only-frame-flow") == 0) only_frame_flow = true;
        else if (std::strcmp(argv[i], "--only-text-layer-mode") == 0) only_text_layer_mode = true;
        else if (std::strcmp(argv[i], "--only-text-layer-align") == 0) only_text_layer_align = true;
        else if (std::strcmp(argv[i], "--only-text-layer-pos") == 0) only_text_layer_position = true;
        else if (std::strcmp(argv[i], "--only-se-registration") == 0) only_se_registration = true;
        else if (std::strcmp(argv[i], "--only-se-stop") == 0) only_se_stop = true;
        else if (std::strcmp(argv[i], "--only-event-library") == 0) only_event_library = true;
        else if (std::strcmp(argv[i], "--only-output-object") == 0) only_output_object = true;
        else if (std::strcmp(argv[i], "--only-forward-field") == 0) only_forward_field = true;
        else if (std::strcmp(argv[i], "--only-address-wrap") == 0) only_address_wrap = true;
        else if (std::strcmp(argv[i], "--only-numeric-family") == 0) only_numeric_family = true;
        else if (std::strcmp(argv[i], "--only-numtext-layout") == 0) only_numtext_layout = true;
        else if (std::strcmp(argv[i], "--only-display-state") == 0) only_display_state = true;
        else if (std::strcmp(argv[i], "--only-texture-bit") == 0) only_texture_bit = true;
        else if (std::strcmp(argv[i], "--only-mouse-pump-entry") == 0) only_mouse_pump_entry = true;
        else if (std::strcmp(argv[i], "--only-getbit-nowrite") == 0) only_getbit_nowrite = true;
        else if (std::strcmp(argv[i], "--only-key-table-b") == 0) only_key_table_b = true;
        else if (std::strcmp(argv[i], "--only-dispatch-default-read") == 0) only_dispatch_default_read = true;
        else if (std::strcmp(argv[i], "--only-draw-mode") == 0) only_draw_mode = true;
        else if (std::strcmp(argv[i], "--only-cg-digit-tables") == 0) only_cg_digit_tables = true;
        else if (std::strcmp(argv[i], "--only-scene-draw-order") == 0) only_scene_draw_order = true;
        else if (std::strcmp(argv[i], "--only-media-table") == 0) only_media_table = true;
        else if (std::strcmp(argv[i], "--only-mesh-320") == 0) only_mesh_320 = true;
        else if (std::strcmp(argv[i], "--only-render-batch") == 0) only_render_batch = true;
        else if (std::strcmp(argv[i], "--only-joy-axis") == 0) only_joy_axis = true;
        else if (std::strcmp(argv[i], "--only-hit-wrap") == 0) only_hit_wrap = true;
        else if (std::strcmp(argv[i], "--only-touch-buffer") == 0) only_touch_buffer = true;
        else if (std::strcmp(argv[i], "--only-save-layout1-record") == 0) only_save_layout1_record = true;
        else if (std::strcmp(argv[i], "--only-save-layout1-restore") == 0) only_save_layout1_restore = true;
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
        if (only_rembit_arg_invalid) {
            // `0x136`：位号 32 / −1 ⇒ 目标原样不动、不抛、错误通道恰一条固定文本；位号 31 对照。
            if (!check_rembit_arg_invalid(table)) return 1;
            std::printf("core-selftest(rembit-arg-invalid): PASS\n");
            return 0;
        }
        if (only_setbit_arg_invalid) {
            // `0x135`：同形；位号 31 作用在 0 上 ⇒ 0x80000000。
            if (!check_setbit_arg_invalid(table)) return 1;
            std::printf("core-selftest(setbit-arg-invalid): PASS\n");
            return 0;
        }
        if (only_font_weight_2bd) {
            // `0x2bd`：非 0 ⇒ 字体对象两格都是 700；0 ⇒ 两格都是 0。
            if (!check_font_weight_2bd(table)) return 1;
            std::printf("core-selftest(font-weight-2bd): PASS\n");
            return 0;
        }
        if (only_getbit_arg_invalid) {
            // `0x13f`：位号 32 / −1 ⇒ 落点原样不动、源也不读、错误通道恰一条固定文本；界内对照归一成 0/1。
            if (!check_getbit_arg_invalid(table)) return 1;
            std::printf("core-selftest(getbit-arg-invalid): PASS\n");
            return 0;
        }
        if (only_random_range_zero) {
            // `0x60`：范围 = 0 ⇒ 先写 0、再抛显示消息型异常（固定文本）；范围 = −1 不抛。
            if (!check_random_range_zero(table)) return 1;
            std::printf("core-selftest(random-range-zero): PASS\n");
            return 0;
        }
        if (only_array_block_bounds) {
            // `0x64`：块号 / count 越过代码区 ⇒ 响亮失败 + 诊断，目标一格不写；合法块对照。
            if (!check_array_block_bounds(table)) return 1;
            std::printf("core-selftest(array-block-bounds): PASS\n");
            return 0;
        }
        if (only_array_write_bounds) {
            // 数组写区间跨出目标池窗口 ⇒ 拒绝 + 诊断（三条写指令共用判据），一格都不写。
            if (!check_array_write_bounds(table)) return 1;
            std::printf("core-selftest(array-write-bounds): PASS\n");
            return 0;
        }
        if (only_string_record_raw) {
            // `0x6c` / `0x2d8` 的字符串族落点：记录内 4 字节裸写（读回按裸位模式）。
            if (!check_string_record_raw(table)) return 1;
            std::printf("core-selftest(string-record-raw): PASS\n");
            return 0;
        }
        if (only_error_channel) {
            // 错误通道：一次触发 = 一次平台写请求；两个抑制条件；诊断无条件保留。
            if (!check_error_channel(table)) return 1;
            std::printf("core-selftest(error-channel): PASS\n");
            return 0;
        }
        if (only_sort_12f_alias) {
            // `0x12f` 的写序：A 与 B 别名时右键每次比较重算（⛔ 不许缓存成 key_j）。
            if (!check_sort_12f_alias(table)) return 1;
            std::printf("core-selftest(12f-alias): PASS\n");
            return 0;
        }
        if (only_pair_88) {
            // `0x88` 的落点 = 模式族 C / D / G（⛔ 不是死写）；`0x19c` 用 `0x88` 写的 C；
            // `0x1cc` 的状态格 = 模式族 E。
            if (!check_pair_88_mode_cells(table)) return 1;
            std::printf("core-selftest(88-mode-cells): PASS\n");
            return 0;
        }
        if (only_bit27_service) {
            // 第 27 位服务体：一次消息处理 + 让出一拍 + 窥视，然后交回迭代头（本迭代不派发）。
            if (!check_gate_bit27_service(table)) return 1;
            std::printf("core-selftest(bit27-service): PASS\n");
            return 0;
        }
        if (only_adv_text_bit27) {
            // 第 27 位的产品置点：`0x71` 调用返回非 0 / 追加前置支 / `0x305` ②。
            if (!check_adv_text_bit27_set_points(table)) return 1;
            std::printf("core-selftest(adv-text-bit27): PASS\n");
            return 0;
        }
        if (only_pacing) {
            // 打字节流：每 D 毫秒一格、每帧最多一格；间隔 = 0 ⇒ 立即抽干。
            if (!check_text_pacing(table)) return 1;
            // 跨帧队列的样式快照：追加之后改字号不得影响已排好的那一格。
            if (!check_text_pending_style_snapshot(table)) return 1;
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
        if (only_draw_mode) {
            // `0x200` 覆盖 / `0x201` 原值交出 / 构造期位下标 / 复位两个写点 / 暂停闩两个写点的门。
            if (!check_draw_mode(table)) return 1;
            std::printf("core-selftest(draw-mode): PASS\n");
            return 0;
        }
        if (only_cg_digit_tables) {
            // `0x23b` 的清旧项（四张同键表）/ 非法编号与纹理槽 0 的上报 / 位数 = 0。
            if (!check_cg_number_tables(table)) return 1;
            std::printf("core-selftest(cg-digit-tables): PASS\n");
            return 0;
        }
        if (only_scene_draw_order) {
            // 收尾扫描：四张表合并访问、平键序 B → A → 网格 → 绘制项、每张表各自判第 16 位。
            if (!check_scene_draw_scan_order(table)) return 1;
            std::printf("core-selftest(scene-draw-order): PASS\n");
            return 0;
        }
        if (only_media_table) {
            // 媒体对象表：填 5 条（按第 2 个操作数）/ 同槽不重建 / 0x24d 的次序 / 清 5 条来源 /
            // 上限一千 = 每拍扫描的循环边界（不是指令判据）。
            if (!check_media_table(table)) return 1;
            std::printf("core-selftest(media-table): PASS\n");
            return 0;
        }
        if (only_mesh_320) {
            // 网格建 / 换（`CORE-OPS-SCENE-320`）：十个操作数的读序 / 每顶点 36 字节 9 分量 /
            // 九个分量的来源 / 位置前两分量减同一个常量 / 步长 1 个字 / 三张副本数组 /
            // 四顶点一批 + 余数 / 重烘门的整字判据 / 顶点数 ≤ 0 的错误通道与"不标脏"。
            if (!check_scene_mesh_320(table)) return 1;
            std::printf("core-selftest(scene-mesh-320): PASS\n");
            return 0;
        }
        if (only_render_batch) {
            // 渲染步的批次闸（`CORE-OPS-RENDER-FRAME-STEP`「渲染步（合同边界）」）：清屏位判定
            // 在闸之前 / 拒绝 ⇒ 不配对、不复位、不遍历、不呈现、不清命令表、不写步末格 /
            // 结束批次只在成功之后 / 呈现（门 = 后缓冲）是最后一项 / 渲染步从不自行清屏。
            if (!check_render_batch_gate(table)) return 1;
            std::printf("core-selftest(render-batch-gate): PASS\n");
            return 0;
        }
        if (only_font) {
            // 两套字号（`0x75` = A 支 / `0x197` = B 支）+ 选支的行为级断言。
            if (!check_font_branches(table)) return 1;
            std::printf("core-selftest(font): PASS\n");
            return 0;
        }
        if (only_font_pairs) {
            // `0x1a5` / `0x2fe`：各写本支那一对描述（名原样 / '@'+名、高 / 下一格按本支字号重算）。
            if (!check_font_pairs_1a5_2fe(table)) return 1;
            std::printf("core-selftest(font-pairs): PASS\n");
            return 0;
        }
        if (only_font_scale) {
            // 建字体缩放：比例 = 视口 ÷ 配置客户区；十槽两格；相等短路；建字体高度截断公式。
            if (!check_font_scale_build(table)) return 1;
            std::printf("core-selftest(font-scale): PASS\n");
            return 0;
        }
        if (only_font_measure_wired) {
            // 字体度量的**产品接线**（`CORE-OPS-FORWARD-PROLOGUE`「非 0 支的下一格来自度量」）：
            // 一次派生两次度量 / 写回取自第二次 / 两次请求面名不同 / `wire_platform_fonts` 真的装上后端。
            if (!check_font_measure_wired(table)) return 1;
            std::printf("core-selftest(font-measure-wired): PASS\n");
            return 0;
        }
        if (only_font_name_host) {
            // `0x2de` 在**宿主字体替换**生效时（SCOPE §0 D1）：名字表 = 单项宿主字体 ⇒ 任意非空名字写回 0、
            // 空名字 −1；负控制（替换关闭）证明这是替换带来的；固定半宽两支 0 / 派生不回退。
            if (!check_font_name_host_index(table)) return 1;
            std::printf("core-selftest(font-name-host): PASS\n");
            return 0;
        }
        if (only_set_tail) {
            // 尾跳进的重建入口（`0x78` / `0x2db`）：空名早退 / 高度无条件 / 一次参考量度（两张结构）/
            // 按对位模式是否为 0 分岔 / A 支两份派生、B 支三枚旧引用 + 只横排一份派生。
            if (!check_ops_set_tail(table)) return 1;
            std::printf("core-selftest(set-tail): PASS\n");
            return 0;
        }
        if (only_text_span_bit2) {
            // 行记录日志的标志字第 2 位 = 变体选择：追加侧按**参数**选支、绘制侧按**记录自己**的标志字分岔、
            // 变体 = 换**整支**字体对象（字面名 / 字号 / 绘制模式）。
            if (!check_adv_text_span_bit2(table)) return 1;
            std::printf("core-selftest(text-span-bit2): PASS\n");
            return 0;
        }
        if (only_key_tables) {
            // `0x10c` 三张表：① 不碰、② 只读、③ 写；无符号值 > 31 抛固定文本；读序 2 → 1。
            if (!check_key_tables_10c(table)) return 1;
            std::printf("core-selftest(key-tables): PASS\n");
            return 0;
        }
        if (only_input_composition) {
            // 输入合成（`CORE-INPUT-WORD`）：键盘 256 表 / 鼠标 + 4 / 手柄槽序号 + 4 / 0xfe / 0x101 / 锁存。
            if (!check_input_composition(table)) return 1;
            std::printf("core-selftest(input-composition): PASS\n");
            return 0;
        }
        if (only_text_container) {
            // `0x192` 的容器句柄 / 格式化 + `0x2de` 的系统字体名一览（含位 0 = 等宽）。
            if (!check_text_container_and_font_names(table)) return 1;
            std::printf("core-selftest(text-container): PASS\n");
            return 0;
        }
        if (only_texture_set) {
            // `0x1f9` 的读序 / 清槽时点 / 失败分档 + 颜色键的目标位宽 + BMP 位深与行序。
            if (!check_texture_set(table)) return 1;
            std::printf("core-selftest(texture-set): PASS\n");
            return 0;
        }
        if (only_text_layer_align) {
            if (!check_text_layer_align(table)) return 1;
            std::printf("core-selftest(text-layer-align): PASS\n");
            return 0;
        }
        if (only_text_layer_mode) {
            if (!check_text_layer_mode_gate(table)) return 1;
            std::printf("core-selftest(text-layer-mode): PASS\n");
            return 0;
        }
        if (only_text_layer_position) {
            if (!check_text_layer_position_198(table)) return 1;
            std::printf("core-selftest(text-layer-pos): PASS\n");
            return 0;
        }
        if (only_frame_flow) {
            // `0x2` 的 −11 支 / `0x6` 的前置与有符号深度 / `0x8c` 越界。
            if (!check_frame_flow(table)) return 1;
            std::printf("core-selftest(frame-flow): PASS\n");
            return 0;
        }
        if (only_se_registration) {
            // `0x2bf` 的登记五格（活动 / 起点 / 时长 / 交值 / 待办）、通道 ≥10 不写、交付读它们。
            if (!check_se_registration_2bf(table)) return 1;
            std::printf("core-selftest(se-registration-2bf): PASS\n");
            return 0;
        }
        if (only_se_stop) {
            // `0xb6`：后端不存在 ⇒ 一格不清；存在 ⇒ 先请求、再清两格（播放名 + 活动）。
            if (!check_se_stop_b6(table)) return 1;
            std::printf("core-selftest(se-stop-b6): PASS\n");
            return 0;
        }
        if (only_event_library) {
            // `0x143` 的投递源 = 文件库槽位表：装载登记 ⇒ 投号<<24；析构 ⇒ 不再投。
            if (!check_event_143_file_library(table)) return 1;
            std::printf("core-selftest(event-143-file-library): PASS\n");
            return 0;
        }
        if (only_output_object) {
            // 输出对象三项：追加 / 位置前缀追加 / 报告（日志逐字 + 根错误处理）。
            if (!check_output_object_report(table)) return 1;
            std::printf("core-selftest(output-object): PASS\n");
            return 0;
        }
        if (only_forward_field) {
            // `0x324`（`CORE-OPS-FORWARD-FIELD`）：复位接收者（三引用清空 + 标记清 0）、
            // 只动接收者自己的状态；接收者为空 ⇒ 只记一次请求、不发明行为。
            if (!check_forward_field_324(table)) return 1;
            std::printf("core-selftest(forward-field): PASS\n");
            return 0;
        }
        if (only_address_wrap) {
            // `0x61` / `0x12c`（`CORE-OPS-ARRAY-LOOKUP` / `-2D`）：地址算术 32 位回绕、
            // 步长来自落点族（4 / 28）、2D 先乘后加、落点值族 ⇒ 类型异常且一格未写。
            if (!check_array_lookup_address_wrap(table)) return 1;
            std::printf("core-selftest(address-wrap): PASS\n");
            return 0;
        }
        if (only_numeric_family) {
            // `0x52`..`0x5f`（`CORE-OPS-NUMERIC-ARITH` / `-COMPARE`）：32 位截断 / 向零截断 /
            // 余数随被除数 / 位数取低 5 位 / 算术右移 / 有符号比较 / 真值只 1,0 / 除零不写落点。
            if (!check_numeric_family_52_5f(table)) return 1;
            std::printf("core-selftest(numeric-family): PASS\n");
            return 0;
        }
        if (only_numtext_layout) {
            // `0x205`（`CORE-OPS-RENDER-NUMTEXT-205`）：格式化（丢高位 / `#` / 补零 / 符号 / 全角化）
            // 与横向落点（三档 k / 先乘后除），**不依赖字体后端**；外加 op_205 的接线（参数顺序 / 格宽）。
            if (!check_numtext_layout(table)) return 1;
            std::printf("core-selftest(numtext-layout): PASS\n");
            return 0;
        }
        if (only_display_state) {
            // `0x25b`（`CORE-OPS-RENDER-DISPLAY-STATE-25B`）：显示状态三格 + 载图 / 外枠两条请求 +
            // 重发①（切回窗口态后按状态三支走，含负图像号的黑底替代）。
            if (!check_display_state_25b(table)) return 1;
            std::printf("core-selftest(display-state-25b): PASS\n");
            return 0;
        }
        if (only_mouse_pump_entry) {
            // 鼠标回调泵的**唯一入口**（`CORE-INPUT-MOUSE-CALLBACK-PUMP`）：一次派发 = 泵本体进入
            // 恰一次 / 没有 0xcd 的 200 拍里一次都不进 / 越闸（第 27 位）不改成"闸门常开"。
            if (!check_mouse_pump_single_entry(table)) return 1;
            std::printf("core-selftest(mouse-pump-entry): PASS\n");
            return 0;
        }
        if (only_getbit_nowrite) {
            // `0x13f` 越界**不写落点**（`CORE-OPS-BIT-TEST`）：落点预置 −1 / 位号 32 ⇒ 仍是 −1；
            // 界内位号 0 照常写 1 与 0；负控制（换成会写落点的指令）⇒ 落点变。
            if (!check_getbit_nowrite(table)) return 1;
            std::printf("core-selftest(getbit-nowrite): PASS\n");
            return 0;
        }
        if (only_key_table_b) {
            // 表 B（逻辑键 → 系统键码）的 93 条初值 + 七个默认槽 + 表内未写到的格 = 0 + 表外未定义。
            if (!check_scalar_key_table_b(table)) return 1;
            std::printf("core-selftest(key-table-b): PASS\n");
            return 0;
        }
        if (only_dispatch_default_read) {
            // `0xa3` 派发：命中 ⇒ 登记值 / 未命中 ⇒ 默认位置；"判据先于读"；登记值 0 仍命中；
            // 同一条按格重入 > 1 次即红（预算内的执行足迹）。
            if (!check_dispatch_default_read(table)) return 1;
            std::printf("core-selftest(dispatch-default-read): PASS\n");
            return 0;
        }
        if (only_joy_axis) {
            // `CORE-INPUT-WORD` 的摇杆半段：四条阈值（范围 + 初始化轴位置 ÷2 向零截断、**只采一次**）、
            // 每条轴 / 每个方向的位、严格比较、无符号、20ms 缓存、「忽略摇杆」门只挡轴。
            if (!check_input_joy_axis(table)) return 1;
            std::printf("core-selftest(joy-axis): PASS\n");
            return 0;
        }
        if (only_hit_wrap) {
            // `CORE-INPUT-HIT-TEST-12E` 的三个 32 位回绕端点（start 最大正数 / start ≤ −2 / count 回绕）。
            if (!check_input_hit_wrap(table)) return 1;
            std::printf("core-selftest(hit-wrap): PASS\n");
            return 0;
        }
        if (only_touch_buffer) {
            // `CORE-INPUT-TOUCH-POINT`：每条触摸消息整体重建（点数 N→M 后容量 = M）、有效性两位、
            // 两个附加字段原样、坐标 ÷100 向零截断、不设容量上限。
            if (!check_input_touch_buffer(table)) return 1;
            std::printf("core-selftest(touch-buffer): PASS\n");
            return 0;
        }
        if (only_save_layout1_record) {
            // `CORE-OPS-SAVE-VERSION-AE` 布局 1 的**暂存块**：32B 记录头 → 4B 长度前缀（含自身）→
            // 六个 u32 成三对；格式标记按**无符号字节逐比**与 `"080 "` 分三档。
            if (!check_save_layout1_record(table)) return 1;
            std::printf("core-selftest(save-layout1-record): PASS\n");
            return 0;
        }
        if (only_save_layout1_restore) {
            // 同一规则的**恢复侧**：帧管理器整块回填 + 两族位置还原 + `0xae` 的三档版本门与帧号分叉。
            if (!check_save_layout1_restore(table)) return 1;
            std::printf("core-selftest(save-layout1-restore): PASS\n");
            return 0;
        }
        if (only_texture_bit) {
            // `display:TextureBit` 的三档 + **缺省来源**（`CORE-OPS-TEXTURE-LOAD` / `-SET`）：
            // 构造期写 32 / 读取端不替换 / 16 与 32 成功 / 0 与 24 落第三档 / 失败发生在建表面之后。
            if (!check_texture_bit_default_source(table)) return 1;
            std::printf("core-selftest(texture-bit-default-source): PASS\n");
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
        if (only_text_pointer) {
            // `0x192` 的落点 / 源可以是指针族（tag 8 / 14）+ halt 帧链报告的形状。
            if (!check_text_pointer_write(table)) return 1;
            std::printf("core-selftest(text-pointer): PASS\n");
            return 0;
        }
        if (only_text_gate) {
            // `0x300` 的循环演示闸：逐格揭示 → 停期限 → 清条目 → 从第一格重来。
            if (!check_text_slot_replay(table)) return 1;
            std::printf("core-selftest(text-gate): PASS\n");
            return 0;
        }
        if (only_clock_monotonic) {
            // 时钟单调：宿主样本不许把场景钟往回拨（`0x300` 停期限被判据当场满足的根因）。
            if (!check_clock_monotonic(table)) return 1;
            std::printf("core-selftest(clock-monotonic): PASS\n");
            return 0;
        }
        if (only_text_compare) {
            // `0x194` / `0x195`：同一条比较的两面（1/0 互补）+ 推进格数 7 + 类型闸门。
            if (!check_text_compare_ops(table)) return 1;
            std::printf("core-selftest(text-compare): PASS\n");
            return 0;
        }
        if (only_scene_table) {            // 0x258 / 0x259：1000 项表的两项标志（按位掩码写 / 全清），不置脏。
            if (!check_scene_table_flags(table)) return 1;
            std::printf("core-selftest(scene-table): PASS\n");
            return 0;
        }
        if (only_scene_relocate) {
            // `0x214`：两个键的整条场景记录交换 / 单边搬运 / 无条件标脏。
            if (!check_scene_relocate(table)) return 1;
            std::printf("core-selftest(scene-relocate): PASS\n");
            return 0;
        }
        if (only_msg_block) {
            // `0x304` / `0x305`：0x6e/0x196 置 bit16 ⇒ 回卷 + 全揭示 + 等待三格 (1,0,0)。
            if (!check_msg_block(table)) return 1;
            std::printf("core-selftest(msg-block): PASS\n");
            return 0;
        }
        if (only_scene_records) {
            // 0x1fb / 0x219 / 0x220 / 0x242：SceneRecord 与 DrawItem 两半必须同步。
            if (!check_scene_records(table)) return 1;
            std::printf("core-selftest(scene-records): PASS\n");
            return 0;
        }
        if (only_misc_ops) {
            // 0x193 / 0x2e9 / 0x2f7 / 0x32 / 0x10a / 0x20a：复审点名的零测试 opcode。
            if (!check_misc_ops(table)) return 1;
            std::printf("core-selftest(misc-ops): PASS\n");
            return 0;
        }
        if (only_text_layer_fallback) {
            // `0x70` 的键基数兜底：没有 `0x213` 的层也必须出绘制条目。
            if (!check_text_layer_fallback(table)) return 1;
            std::printf("core-selftest(text-layer-fallback): PASS\n");
            return 0;
        }
        if (only_text_cell_reveal) {
            // `0x1ce`：逐格揭示当前文本的启停 + 每拍画一格并推进格号（`CORE-OPS-TEXT-CELL-REVEAL-1CE`）。
            if (!check_text_cell_reveal(table)) return 1;
            std::printf("core-selftest(text-cell-reveal): PASS\n");
            return 0;
        }
        if (only_wait_timer) {
            // `0x21c` 谓词 A：定时器窗内挡住、到期（相等仍算窗内）才放行。
            if (!check_wait_timer(table)) return 1;
            std::printf("core-selftest(wait-timer): PASS\n");
            return 0;
        }
        if (only_wait_colour_window) {
            // `0x21c` 谓词 A 的「待处理」闩（`+0B5B4`）：颜色窗没走完之前不放行。
            if (!check_wait_colour_window(table)) return 1;
            std::printf("core-selftest(wait-colour-window): PASS\n");
            return 0;
        }
        if (only_choice_consumer) {
            // 选项消费：点中 ⇒ 执行位置 ← 决定入口。
            if (!check_choice_consumer(table)) return 1;
            std::printf("core-selftest(choice-consumer): PASS\n");
            return 0;
        }
        if (only_auto_message) {
            // 自动消息：`0x1b7` 开启 ⇒ `0x72` 武装计时器，到期放行等推进位。
            if (!check_auto_message(table)) return 1;
            std::printf("core-selftest(auto-message): PASS\n");
            return 0;
        }
        if (only_texture_stretch) {
            // `0x32`：放大双线性、缩小面积平均（不是最近邻）。
            if (!check_texture_stretch(table)) return 1;
            std::printf("core-selftest(texture-stretch): PASS\n");
            return 0;
        }
        if (only_adv_wait) {
            // `0x72`：等推进期间不派发；只有左键放行；右键不算。
            if (!check_adv_wait(table)) return 1;
            std::printf("core-selftest(adv-wait): PASS\n");
            return 0;
        }
        if (only_misc_cells) {
            if (!check_misc_cells(table)) return 1;
            std::printf("core-selftest(misc-cells): PASS\n");
            return 0;
        }
        if (only_voice_all_stop) {
            if (!check_voice_all_stop(table)) return 1;
            std::printf("core-selftest(voice-all-stop): PASS\n");
            return 0;
        }
        if (only_choice_window) {
            if (!check_choice_window(table)) return 1;
            std::printf("core-selftest(choice-window): PASS\n");
            return 0;
        }
        if (only_adv_choice) {
            // `0x90` 追加选项 / `0x97` 按矩形写附加整数（第 0 章开场的选项菜单）。
            if (!check_adv_choice(table)) return 1;
            std::printf("core-selftest(adv-choice): PASS\n");
            return 0;
        }
        if (only_scene_self) {
            // `0x229`：先重置再写「作用键区间 + 三元平移」，消费者只对区间内的绘制项加平移。
            if (!check_scene_self_transform(table)) return 1;
            std::printf("core-selftest(scene-self): PASS\n");
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
            if (!check_text_pending_style_snapshot(table)) return 1;
            std::printf("core-selftest(typewriter): PASS\n");
            return 0;
        }
        if (only_audio) {
            // 音效槽（后端 15 / 引擎表 10）+ 门位第 27 位的两条支路（`0xc2` A 支 / `0xc4` 影片门支）。
            if (!check_audio_slot_table()) return 1;
            if (!check_audio_slot_bounds(table)) return 1;
            if (!check_time_delivery(table)) return 1;
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
        // `0x21c` 的「待处理」闩（`+0B5B4`）也进全量自检：它是 TITLE / ADV 的公共前置。
        if (!check_wait_colour_window(table)) return 1;
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
        // ★ 到期判据 = `现在 >= 起点+延迟+持续时间`（引擎 `0x4B17F9` 的 `jge`）⇒ 相等**已到期**。
        window_machine.advance_scene(1199);  // 终点前 1 ms：仍在窗内，不写回
        const auto near_end_alpha = (core::derive_color(*window_item, 1199, false) >> 24) & 0xFFu;
        if ((window_item->flags & 2u) == 0 || window_item->color != 0x00112233u ||
            near_end_alpha <= 100 || near_end_alpha >= 200) {
            std::fprintf(stderr, "timing window last-in-window tier mismatch alpha=%u\n",
                         near_end_alpha);
            return 1;
        }
        window_machine.advance_scene(1200);  // 相等即到期：写回终色并清窗
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
        // 等待位与颜色窗：**窗没走完之前不放行**（`0x21c` 谓词 A 里的 `+0B5B4`）。
        // ★ 旧版这里断言的是"颜色窗还在走，等待位也必须放行"（只看待画格 / 打字态）——那正是 TITLE 的
        //   mesh 200000 在**任何一帧渲染前**被 `0x1f7` 拆掉的成因（`0x323` 排窗之后 `0x21c` 立刻放行）。
        //   引擎的 `sub_407E20` 最后一条指令就是 `mov eax,[ecx+0B5B4h]`（0x407E97）⇒ 它进判据。
        //   窗走完之后的那一拍才放行（独立用例：`emulator-core-wait-colour-window`）。
        auto wait_script = make_script_with_body();
        emit_instruction(wait_script, 0x1FB,
                         {{0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(wait_script, 0x202,
                         {{0, 1}, {0, 100}, {0, 100}, {0, 200}, {0, 0x00ABCDEFu}});
        emit_instruction(wait_script, 0x21C, {});
        emit_instruction(wait_script, 2, {});
        core::Machine wait_machine = scene_step_machine(table);
        wait_machine.load_root(wait_script);
        wait_machine.set_host_clock_ms(1000);
        if (!wait_machine.step() || !wait_machine.step()) {  // 0x1FB / 0x202
            std::fprintf(stderr, "wait setup steps failed\n");
            return 1;
        }
        if (!wait_machine.scene_pending()) {
            std::fprintf(stderr, "0x202 排窗没有置「待处理」闩（+0B5B4）\n");
            return 1;
        }
        if (!wait_machine.step() || (wait_machine.state_flags() & (1u << 10)) == 0) {
            std::fprintf(stderr, "wait bit not set\n");
            return 1;
        }
        // 窗推进中（delay 100 + duration 100）：等待位必须**保持**置起。
        wait_machine.set_host_clock_ms(1150);
        if (!wait_machine.step()) {
            std::fprintf(stderr, "wait release step halted: %s\n",
                         wait_machine.last_stop().message.c_str());
            return 1;
        }
        if ((wait_machine.state_flags() & (1u << 10)) == 0) {
            std::fprintf(stderr, "颜色窗还在推进（1150ms）就放行了等待位\n");
            return 1;
        }
        // 0x20C：帧步进请求 + 时间戳；**「强制完成」的清点在渲染步收尾，不在 `consume_frame_step`**。
        // ★ spec 句（`CORE-TIMING-WINDOW` 的「不粘住」）：渲染步**收尾**（在"待处理 = 0 ⇒ 清空命令表"
        //   这一步**之后**）**无条件**把「强制完成」清 0 —— 不管本步有没有人读过它；它**只活一个渲染步**。
        //   ⇒ 读一次（`consume_frame_step` / `forced_complete`）**不消耗**它，清点只有 `end_render`。
        auto frame_script = make_script_with_body();
        emit_instruction(frame_script, 0x20C, {});
        emit_instruction(frame_script, 2, {});
        core::Machine frame_machine = scene_step_machine(table);
        frame_machine.load_root(frame_script);
        frame_machine.run(8);
        if (!frame_machine.frame_step_requested() || frame_machine.scene_clock_ms() == 0) {
            std::fprintf(stderr, "0x20C frame request mismatch\n");
            return 1;
        }
        frame_machine.force_complete();
        if (!frame_machine.consume_frame_step() || frame_machine.frame_step_requested()) {
            std::fprintf(stderr, "0x20C consume mismatch\n");
            return 1;
        }
        // ⛔ 不许在这里清：`consume_frame_step` 只是"取走一次渲染请求"。
        if (!frame_machine.forced_complete()) {
            std::fprintf(stderr, "「强制完成」被 `consume_frame_step` 清掉了（它只活一个渲染步，清点在收尾）\n");
            return 1;
        }
        // 同一步内多次读取**不消耗**它。
        if (!frame_machine.forced_complete() || !frame_machine.forced_complete()) {
            std::fprintf(stderr, "「强制完成」读一次就被清掉了（读点只判它是否 = 1）\n");
            return 1;
        }
        // 渲染步收尾 ⇒ 无条件清 0。
        frame_machine.end_render();
        if (frame_machine.forced_complete()) {
            std::fprintf(stderr, "渲染步收尾没有清「强制完成」\n");
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
        const core::TextureEntry* filled = multiply_machine.texture(1);
        if (filled == nullptr || filled->slot.rgba.size() != 64 || filled->slot.rgba[0] != 255 ||
            filled->slot.rgba[1] != 255 || filled->slot.rgba[2] != 255 || !filled->record.non_file) {
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
        const core::TextureSlotObject* fade_texture = command_machine.texture_slot(10);
        if (fade_texture == nullptr || fade_texture->rgba[0] != 255 ||
            fade_texture->rgba[2] != 0) {
            std::fprintf(stderr, "kind0 fade progress 0 mismatch\n");
            return 1;
        }
        command_machine.reset_scene_clock(1050);
        core::rasterize_scene(command_machine, 4, 4);
        fade_texture = command_machine.texture_slot(10);
        if (fade_texture == nullptr || fade_texture->rgba[0] == 0 || fade_texture->rgba[2] == 0) {
            std::fprintf(stderr, "kind0 fade progress 0.5 mismatch r=%u b=%u\n",
                         fade_texture ? fade_texture->rgba[0] : 0,
                         fade_texture ? fade_texture->rgba[2] : 0);
            return 1;
        }
        command_machine.reset_scene_clock(1100);
        core::rasterize_scene(command_machine, 4, 4);
        fade_texture = command_machine.texture_slot(10);
        if (fade_texture == nullptr || fade_texture->rgba[0] != 0 || fade_texture->rgba[2] != 255) {
            std::fprintf(stderr, "kind0 fade progress 1 mismatch\n");
            return 1;
        }
        command_machine.reset_scene_clock(1101);
        // ★ 收尾判据是**场景待处理标记**（`60-ops-scene` 的「命令档的前置与出档」），它由**渲染步的
        //   推进子步**清 0 再重算 —— 真实主循环里那一步在本渲染步之前。这里显式走一次那个子步，
        //   否则读到的是上一步留下的闩（"命令还在动"），命令表就不该被清。
        command_machine.advance_scene(1101);
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
        blur_machine.advance_scene(1200);  // 渲染步的推进子步（清「场景待处理标记」再重算）
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
            const core::TextureEntry* text_texture = text_machine.texture(1);
            std::size_t ink = 0;
            if (text_texture != nullptr) {
                for (std::size_t i = 3; i < text_texture->slot.rgba.size(); i += 4) {
                    if (text_texture->slot.rgba[i] != 0) ++ink;
                }
            }
            if (text_texture == nullptr || ink == 0 || !text_texture->record.non_file) {
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
            const core::TextureSlotObject* empty_texture = empty_text.texture_slot(1);
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
                const core::TextureSlotObject* texture = machine.texture_slot(1);
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
                const core::TextureSlotObject* texture = machine.texture_slot(1);
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
                const core::TextureSlotObject* texture = machine.texture_slot(1);
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
                const core::TextureSlotObject* texture = machine.texture_slot(1);
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
                const core::TextureSlotObject* texture = ring_machine.texture_slot(1);
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
                if (const core::TextureSlotObject* texture = gdi_machine.texture_slot(1)) {
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
                    if (const core::TextureSlotObject* texture = aa_machine.texture_slot(1)) {
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
            const auto final_revision = revision_machine.texture_slot(1)->revision;
            if (final_revision <= 3) {
                std::fprintf(stderr,
                             "两轮「重建 + 同条数文字」后纹理版本号 = %u（≤3）："
                             "GPU 缓存会命中上一轮纹理，滚动重画不会刷新\n",
                             final_revision);
                return 1;
            }
            // ★ **释放槽对象 → 重建**也必须接着涨（`0x1fa` 只清槽里的对象，但窗口路径的 GPU 缓存仍记得
            //   `(id, revision)`）。实测缺陷（2026-10-29，用户口径 + 本机窗口复现）：设置界面
            //   （CONFIG1.BIN）↔ 初始配置界面（Game Start）来回一次，后进的那个界面对应的整张
            //   美术图**整张不画**（背景透出来）或画成**对方的内容** —— 两界面用同一批纹理编号，
            //   前者 `0x1fa` 释放、后者重建时版本号若回到 1，缓存就命中上一条记录的纹理。
            //   坏法：0x1f8 建（rev=1）→ 0x1fa 删整条记录 → 0x1f8 再建（rev 又 = 1）。
            // ★ 同时守「记录 vs 槽对象」两层：`0x1fa` 之后**记录还在**（来源写成「无」、颜色键 / 空白
            //   标记不动）、**槽对象没了**；`0x208` 在"记录在、槽空"时仍必须回 0/0。
            auto release_script = make_script_with_body();
            emit_instruction(release_script, 0x1F8, {{0, 7}, {0, 64}, {0, 32}, {0, 0}});
            emit_instruction(release_script, 0x1FA, {{0, 7}});
            emit_instruction(release_script, 0x208, {{0, 7}, {3, 1}, {3, 2}});  // 槽空 ⇒ 0/0
            emit_instruction(release_script, 0x1F8, {{0, 7}, {0, 64}, {0, 32}, {0, 0}});
            emit_instruction(release_script, 0x208, {{0, 7}, {3, 3}, {3, 4}});
            emit_instruction(release_script, 2, {});
            core::Machine release_machine(0x12345678u, table);
            release_machine.load_root(release_script);
            for (int i = 0; i < 2; ++i) {
                if (!release_machine.step()) break;
            }
            const core::TextureRecord* released_record = release_machine.texture_record(7);
            if (released_record == nullptr) {
                std::fprintf(stderr,
                             "0x1fa 把纹理**记录**删掉了：spec 只清槽里的对象、记录（来源 / 颜色键 / "
                             "空白标记）必须保留\n");
                return 1;
            }
            if (released_record->file != -1 || !released_record->blank) {
                std::fprintf(stderr,
                             "0x1fa 之后记录字段不对：file=%d blank=%d（期望 file=-1、blank 保持 1）\n",
                             released_record->file, released_record->blank ? 1 : 0);
                return 1;
            }
            if (release_machine.texture_slot(7) != nullptr) {
                std::fprintf(stderr, "0x1fa 之后纹理槽对象还在（期望已释放：present=0）\n");
                return 1;
            }
            release_machine.run(1000);
            if (release_machine.global_int(1) != 0 || release_machine.global_int(2) != 0) {
                std::fprintf(stderr,
                             "0x1fa 之后 0x208 必须回 0/0（记录在、槽空），实得 %d/%d\n",
                             release_machine.global_int(1), release_machine.global_int(2));
                return 1;
            }
            if (release_machine.global_int(3) != 64 || release_machine.global_int(4) != 32) {
                std::fprintf(stderr, "重建之后 0x208 必须回 64/32，实得 %d/%d\n",
                             release_machine.global_int(3), release_machine.global_int(4));
                return 1;
            }
            const core::TextureSlotObject* rebound = release_machine.texture_slot(7);
            if (rebound == nullptr || rebound->revision <= 1) {
                std::fprintf(stderr,
                             "0x1fa 释放后重建同一编号：revision=%u（必须 > 1；回到 1 会让窗口路径"
                             "命中上一条记录的 GPU 纹理 —— 两个界面互相污染）\n",
                             rebound == nullptr ? 0u : rebound->revision);
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
        const core::TextureSlotObject* wiped = wipe_machine.texture_slot(10);
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
        const core::TextureSlotObject* movie_texture = movie_machine.texture_slot(10);
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
        // 0x1F6 清场景：四张同键表**整块清空**（绘制项 `+0x408` / 网格 `+0x428` / 辅助表 A `+0x438` /
        // 辅助表 B `+0x448`），且脏标记被清 0 —— 不能因为"清空"再请求一帧。
        // ★ 四张都布：漏清辅助表 A 的旧实现（只清 `+0x448`）在"辅助 A 还有条目"这条上当场红。
        auto clear_script = make_script_with_body();
        emit_instruction(clear_script, 0x1F8, {{0, 1}, {0, 4}, {0, 4}, {0, 0}});
        emit_instruction(clear_script, 0x1FB,
                         {{0, 1}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
        emit_instruction(clear_script, 0x322, {{0, 1}, {0, 0}, {0, 255}, {0, 0}});
        emit_instruction(clear_script, 0x242, {{0, 1}, {0, 7}});
        emit_instruction(clear_script, 0x344, {{0, 1}, {0, 8}});
        emit_instruction(clear_script, 0x1F6, {});
        emit_instruction(clear_script, 2, {});
        core::ConfigStore clear_config;
        clear_config.set_int("set:DrawMode", 1);
        core::Machine clear_machine(0x12345678u, table, nullptr, std::move(clear_config));
        clear_machine.load_root(clear_script);
        for (int i = 0; i < 5; ++i) {  // 1F8 + 1FB + 322 + 242 + 344
            if (!clear_machine.step()) {
                std::fprintf(stderr, "0x1F6 夹具没跑起来：%s\n", clear_machine.last_stop().message.c_str());
                return 1;
            }
        }
        std::int32_t clear_a = 0, clear_b = 0, clear_flags = 0, clear_value = 0;
        if (clear_machine.draw_item_count() == 0 || clear_machine.mesh_count() == 0 ||
            !clear_machine.scene_aux_a(1, clear_a, clear_b) ||
            !clear_machine.scene_aux_b_entry(1, clear_flags, clear_value)) {
            std::fprintf(stderr, "0x1F6 夹具没布满四张表（draw=%zu mesh=%zu auxB=%zu）\n",
                         clear_machine.draw_item_count(), clear_machine.mesh_count(),
                         clear_machine.scene_aux_b_count());
            return 1;
        }
        clear_machine.consume_frame_step();
        core::rasterize_scene(clear_machine, 4, 4);
        if (!clear_machine.step() || clear_machine.consume_frame_step()) {
            std::fprintf(stderr, "0x1F6 must clear the dirty flag, not set it\n");
            return 1;
        }
        if (clear_machine.draw_item_count() != 0 || clear_machine.mesh_count() != 0 ||
            clear_machine.scene_aux_a(1, clear_a, clear_b) ||
            clear_machine.scene_aux_b_entry(1, clear_flags, clear_value)) {
            std::fprintf(stderr, "0x1F6 没有清空四张同键表（draw=%zu mesh=%zu auxB=%zu）\n",
                         clear_machine.draw_item_count(), clear_machine.mesh_count(),
                         clear_machine.scene_aux_b_count());
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
        off_machine.set_host_clock_ms(900);
        off_machine.load_root(dirty_script);
        if (!off_machine.step() || off_machine.consume_frame_step()) {
            std::fprintf(stderr, "DrawMode=0 still auto-rendered\n");
            return 1;
        }
        // ★ 绘制模式 ≠ 1 是"连**盖钟**一起跳过"那一类（`CORE-TIMING-WINDOW` 的 (a)）——
        //   旧模型只做到"不渲染、时钟照走"；spec 说成立期间**所有以场景时钟计的量都停走**。
        //   ⇒ 判据是 `scene_clock_ms` **没动**（⛔ 不是"看起来没渲染"）。
        if (off_machine.scene_clock_ms() != 0u) {
            std::fprintf(stderr, "DrawMode=0 仍然盖了场景时钟（clock=%u）\n",
                         off_machine.scene_clock_ms());
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
        core::Machine clock_machine = scene_step_machine(table);
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
            const core::TextureSlotObject* playing = movie_machine.texture_slot(42);
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
            const core::TextureSlotObject* last = movie_machine.texture_slot(42);
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
                const core::TextureSlotObject* texture = fill_machine.texture_slot(42);
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
        // ── 0x1f7 拆离：四张同键表一起删（绘制项 / 网格 / 辅助表 A / 辅助表 B）──
        // ★ 表身份按**偏移**定死（`60-ops-scene` 的「旧名对照（A / B 以本段为准）」）：绘制项 `+0x408`
        //   → 网格 `+0x428` → 辅助表 **A** `+0x438`（`0x242` 一族写）→ 辅助表 **B** `+0x448`
        //   （`0x344` 一族写）。⛔ 不是字母序、也不是"辅助 B 无写入口"。
        //   ⇒ 夹具必须**四张都布**：只布三张时"漏删辅助表 A"是抓不到的（旧夹具就是这样恒绿）。
        {
            auto detach_script = make_script_with_body();
            // 键 100：四张表各一条；键 102：只放两张辅助表，用来验区间。
            emit_instruction(detach_script, 0x1FB,
                             {{0, 100}, {0, 1}, {0, 0}, {0, 0}, {0, 4}, {0, 4}, {0, 0}, {0, 0}});
            emit_instruction(detach_script, 0x322, {{0, 100}, {0, 0}, {0, 255}, {0, 0}});
            emit_instruction(detach_script, 0x242, {{0, 100}, {0, 7}});   // 辅助表 A
            emit_instruction(detach_script, 0x344, {{0, 100}, {0, 11}});  // 辅助表 B
            emit_instruction(detach_script, 0x242, {{0, 102}, {0, 8}});
            emit_instruction(detach_script, 0x344, {{0, 102}, {0, 12}});
            emit_instruction(detach_script, 0x1F7, {{0, 100}, {0, 3}});  // 左闭右开 [100, 103)
            emit_instruction(detach_script, 0x1A8, {});
            emit_instruction(detach_script, 2, {});
            core::Machine detach_machine(0x12345678u, table);
            detach_machine.load_root(detach_script);
            for (int i = 0; i < 6; ++i) {
                if (!detach_machine.step()) {
                    std::fprintf(stderr, "detach fixture failed: %s\n",
                                 detach_machine.last_stop().message.c_str());
                    return 1;
                }
            }
            if (detach_machine.draw_item_count() != 1 || detach_machine.mesh_count() != 1 ||
                detach_machine.scene_aux_b_count() != 2) {
                std::fprintf(stderr, "detach fixture did not fill the four tables\n");
                return 1;
            }
            std::int32_t aux_a = 0, aux_a_b = 0;
            if (!detach_machine.scene_aux_a(100, aux_a, aux_a_b) ||
                !detach_machine.scene_aux_a(102, aux_a, aux_a_b)) {
                std::fprintf(stderr, "detach fixture did not fill 辅助表 A (+0x438)\n");
                return 1;
            }
            if (!detach_machine.step()) {  // 0x1f7 count=3
                std::fprintf(stderr, "0x1f7 step failed: %s\n",
                             detach_machine.last_stop().message.c_str());
                return 1;
            }
            const bool aux_a_empty = !detach_machine.scene_aux_a(100, aux_a, aux_a_b) &&
                                     !detach_machine.scene_aux_a(102, aux_a, aux_a_b);
            if (detach_machine.draw_item_count() != 0 || detach_machine.mesh_count() != 0 ||
                !aux_a_empty || detach_machine.scene_aux_b_count() != 0) {
                std::fprintf(stderr, "0x1f7 did not delete from all same-key tables "
                                     "(draw=%zu mesh=%zu auxA_empty=%d auxB=%zu)\n",
                             detach_machine.draw_item_count(), detach_machine.mesh_count(),
                             aux_a_empty ? 1 : 0, detach_machine.scene_aux_b_count());
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
        if (!check_text_cell_reveal(table)) return 1;
        if (!check_run_bracket(table)) return 1;
        if (!check_command_queue(table)) return 1;
        if (!check_input_dispatch(table)) return 1;
        if (!check_input_composition(table)) return 1;
        if (!check_text_container_and_font_names(table)) return 1;
        if (!check_texture_set(table)) return 1;
        if (!check_texture_bit_default_source(table)) return 1;
        if (!check_text_layer_mode_gate(table)) return 1;
        if (!check_text_layer_position_198(table)) return 1;
        if (!check_frame_flow(table)) return 1;
        if (!check_input_devices(table)) return 1;
        if (!check_pause_and_clock(table)) return 1;
        if (!check_scene_clock_gate(table)) return 1;
        if (!check_second_batch(table)) return 1;
        if (!check_scene3d_setters(table)) return 1;
        if (!check_memflip(table)) return 1;
        if (!check_save_ops(table)) return 1;
        if (!check_input_hold(table)) return 1;
        if (!check_mouse_and_hit_test(table)) return 1;
        if (!check_input_joy_axis(table)) return 1;
        if (!check_input_hit_wrap(table)) return 1;
        if (!check_input_touch_buffer(table)) return 1;
        if (!check_exit_cleanup(table)) return 1;
        if (!check_config_ui_surface(table)) return 1;
        if (!check_color_normalization(table)) return 1;
        if (!check_slot_imgid(table)) return 1;
        if (!check_texture_size(table)) return 1;
        if (!check_exit_script(table)) return 1;
        if (!check_exit_script_reset_scope(table)) return 1;
        if (!check_frame_shape(table)) return 1;
        if (!check_audio_slot_table()) return 1;
        if (!check_audio_slot_bounds(table)) return 1;
        if (!check_time_delivery(table)) return 1;
        if (!check_se_registration_2bf(table)) return 1;
        if (!check_se_stop_b6(table)) return 1;
        if (!check_event_143_file_library(table)) return 1;
        if (!check_output_object_report(table)) return 1;
        if (!check_gate_split_branches(table)) return 1;
        if (!check_gate_chain(table)) return 1;
        if (!check_font_branches(table)) return 1;
        if (!check_font_pairs_1a5_2fe(table)) return 1;
        if (!check_font_scale_build(table)) return 1;
        if (!check_font_name_host_index(table)) return 1;
        if (!check_key_tables_10c(table)) return 1;
        if (!check_scalar_key_table_b(table)) return 1;
        if (!check_dispatch_default_read(table)) return 1;
        if (!check_rembit_arg_invalid(table)) return 1;
        if (!check_setbit_arg_invalid(table)) return 1;
        if (!check_getbit_arg_invalid(table)) return 1;
        if (!check_random_range_zero(table)) return 1;
        if (!check_array_block_bounds(table)) return 1;
        if (!check_array_write_bounds(table)) return 1;
        if (!check_string_record_raw(table)) return 1;
        if (!check_error_channel(table)) return 1;
        if (!check_sort_12f_alias(table)) return 1;
        if (!check_pair_88_mode_cells(table)) return 1;
        if (!check_gate_bit27_service(table)) return 1;
        if (!check_adv_text_bit27_set_points(table)) return 1;
        if (!check_font_weight_2bd(table)) return 1;
        if (!check_render_prims(table)) return 1;
        if (!check_draw_mode(table)) return 1;
        if (!check_cg_number_tables(table)) return 1;
        if (!check_scene_draw_scan_order(table)) return 1;
        if (!check_media_table(table)) return 1;
        if (!check_render_batch_gate(table)) return 1;
        if (!check_scene_mesh_320(table)) return 1;
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
