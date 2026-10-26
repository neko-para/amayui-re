#include "input_script.h"

#include "core/encoding.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace app {
namespace {

std::string_view trim(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
    return text.substr(begin, end - begin);
}

std::vector<std::string_view> split_ws(std::string_view line) {
    std::vector<std::string_view> parts;
    std::size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index])) != 0) ++index;
        const std::size_t begin = index;
        while (index < line.size() && std::isspace(static_cast<unsigned char>(line[index])) == 0) ++index;
        if (index > begin) parts.push_back(line.substr(begin, index - begin));
    }
    return parts;
}

// 十进制整数；`what` 只用于错误信息。
std::int64_t parse_decimal(std::string_view token, const char* what, std::size_t line_no,
                           std::string_view line) {
    if (token.empty()) {
        throw std::runtime_error("input-script:" + std::to_string(line_no) + ": " + what + " 缺一个整数：" +
                                 std::string(line));
    }
    std::size_t consumed = 0;
    long long value = 0;
    try {
        value = std::stoll(std::string(token), &consumed, 10);
    } catch (const std::exception&) {
        throw std::runtime_error("input-script:" + std::to_string(line_no) + ": " + what + " 不是整数：" +
                                 std::string(line));
    }
    if (consumed != token.size()) {
        throw std::runtime_error("input-script:" + std::to_string(line_no) + ": " + what + " 不是整数：" +
                                 std::string(line));
    }
    return value;
}

}  // namespace

std::vector<InputEvent> parse_input_script(const std::string& text) {
    std::vector<InputEvent> events;
    // 分隔符：换行或 `;`（与 emulator-new 的 `parseInputScript` 同一套语法）。
    std::size_t line_no = 0;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        std::size_t end = text.find_first_of("\n;", begin);
        if (end == std::string::npos) end = text.size();
        const std::string raw = text.substr(begin, end - begin);
        begin = end + 1;
        ++line_no;
        std::string_view line = trim(raw);
        if (const auto hash = line.find('#'); hash != std::string_view::npos) {
            line = trim(line.substr(0, hash));
        }
        if (line.empty()) continue;
        const std::vector<std::string_view> parts = split_ws(line);
        if (parts.size() < 2) {
            throw std::runtime_error("input-script:" + std::to_string(line_no) +
                                     ": 至少要 `at 命令`：" + std::string(line));
        }
        const auto at = parse_decimal(parts[0], "at", line_no, line);
        if (at < 0) {
            throw std::runtime_error("input-script:" + std::to_string(line_no) +
                                     ": at 必须是非负毫秒：" + std::string(line));
        }
        InputEvent event;
        event.at_ms = static_cast<std::uint32_t>(at);
        const std::string_view command = parts[1];
        if (command == "move") {
            if (parts.size() < 4) {
                throw std::runtime_error("input-script:" + std::to_string(line_no) + ": move 需要 x y：" +
                                         std::string(line));
            }
            event.kind = InputEvent::Kind::Move;
            event.x = static_cast<std::int32_t>(parse_decimal(parts[2], "x", line_no, line));
            event.y = static_cast<std::int32_t>(parse_decimal(parts[3], "y", line_no, line));
        } else if (command == "down" || command == "up") {
            if (parts.size() < 3) {
                throw std::runtime_error("input-script:" + std::to_string(line_no) + ": " + std::string(command) +
                                         " 需要按钮编号 0..31：" + std::string(line));
            }
            event.kind = InputEvent::Kind::Button;
            event.button = static_cast<std::int32_t>(parse_decimal(parts[2], "button", line_no, line));
            if (event.button < 0 || event.button > 31) {
                throw std::runtime_error("input-script:" + std::to_string(line_no) +
                                         ": 按钮编号必须是 0..31：" + std::string(line));
            }
            event.down = command == "down";
        } else if (command == "key") {
            if (parts.size() < 4) {
                throw std::runtime_error("input-script:" + std::to_string(line_no) +
                                         ": key 需要 `key N down|up`：" + std::string(line));
            }
            event.kind = InputEvent::Kind::Key;
            event.key = static_cast<std::int32_t>(parse_decimal(parts[2], "key", line_no, line));
            if (event.key < 0 || event.key > 31) {
                throw std::runtime_error("input-script:" + std::to_string(line_no) +
                                         ": 键编号必须是 0..31（引擎输入位号，不是 VK）：" + std::string(line));
            }
            const std::string_view state = parts[3];
            if (state != "down" && state != "up") {
                throw std::runtime_error("input-script:" + std::to_string(line_no) +
                                         ": key 的第三个词必须是 down / up：" + std::string(line));
            }
            event.down = state == "down";
        } else if (command == "wheel") {
            if (parts.size() < 3) {
                throw std::runtime_error("input-script:" + std::to_string(line_no) + ": wheel 需要整数增量：" +
                                         std::string(line));
            }
            event.kind = InputEvent::Kind::Wheel;
            event.delta = static_cast<std::int32_t>(parse_decimal(parts[2], "delta", line_no, line));
        } else {
            throw std::runtime_error("input-script:" + std::to_string(line_no) + ": 未知命令 " +
                                     std::string(command) + "：" + std::string(line));
        }
        events.push_back(event);
    }
    // 稳定排序：同一时刻保持书写顺序（`std::stable_sort` 就是"按 at 分组不动"）。
    std::stable_sort(events.begin(), events.end(),
                     [](const InputEvent& a, const InputEvent& b) { return a.at_ms < b.at_ms; });
    return events;
}

std::vector<InputEvent> load_input_script(const std::string& path) {
    std::ifstream input(core::path_from_utf8(path), std::ios::binary);
    if (!input) throw std::runtime_error("input-script-open-failed:" + path);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return parse_input_script(buffer.str());
}

ScriptedInput::ScriptedInput(std::vector<InputEvent> events) : events_(std::move(events)) {
    for (const auto& event : events_) last_time_ms_ = std::max(last_time_ms_, event.at_ms);
}

void ScriptedInput::advance(std::int64_t now_ms) {
    if (now_ms < 0) return;  // 锚点之前：一条都不应用
    while (next_ < events_.size() && static_cast<std::int64_t>(events_[next_].at_ms) <= now_ms) {
        const InputEvent& event = events_[next_++];
        ++applied_;
        last_event_at_ = static_cast<std::int64_t>(event.at_ms);
        switch (event.kind) {
            case InputEvent::Kind::Move:
                x_ = event.x;
                y_ = event.y;
                cursor_available_ = true;
                break;
            case InputEvent::Kind::Button:
                // ★ 不伪造光标：没有先 `move` 就按下 ⇒ 按钮位照记，但"设备可用"仍是 false，
                //   `0x108` 因此写 0（规范：`0x108/0x109/0x2fc` 共用**一个**输入可用标志）。
                if (event.down) {
                    buttons_ |= 1 << event.button;
                } else {
                    buttons_ &= ~(1 << event.button);
                }
                break;
            case InputEvent::Kind::Key:
                if (event.down) {
                    keys_ |= 1u << event.key;
                } else {
                    keys_ &= ~(1u << event.key);
                }
                break;
            case InputEvent::Kind::Wheel:
                // 滚轮：累加增量；引擎侧由 `0x10d` 读并清（宿主每轮推一次）。
                wheel_delta_ += event.delta;
                break;
        }
    }
}

bool ScriptedInput::cursor(std::int32_t& x, std::int32_t& y) const {
    if (!cursor_available_) return false;
    x = x_;
    y = y_;
    return true;
}

bool ScriptedInput::buttons(std::int32_t& mask) const {
    // ★ 与 `0x109` **共用一个可用性**（`CORE-INPUT-MOUSE-POS` / `-MOUSE-BUTTON`：同一引擎标志）：
    //   没有 `move`（光标不可用）时按钮也报"设备不可用"，`0x108` 因此写 0。
    if (!cursor_available_) return false;
    mask = buttons_;
    return true;
}

bool ScriptedInput::touch(core::Machine::TouchPoint&) const {
    return false;  // 桌面鼠标-only：没有触点缓冲（与窗口路径同口径）
}

void ScriptedInput::bind(core::Machine& machine) {
    machine.set_key_sampler([this] { return keys(); });
    machine.set_cursor_source([this](std::int32_t& x, std::int32_t& y) { return cursor(x, y); });
    machine.set_mouse_button_source([this](std::int32_t& mask) { return buttons(mask); });
    machine.set_touch_source([this](core::Machine::TouchPoint& point) { return touch(point); });
}

}  // namespace app
