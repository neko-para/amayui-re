#include "runtime/message_box.h"

#include <atomic>
#include <string>
#include <vector>

namespace lift {
namespace {

// ---- Win32 返回值（MessageBox 的 uType → 按钮集）----
constexpr std::uint32_t kIdOk = 1;
constexpr std::uint32_t kIdCancel = 2;
constexpr std::uint32_t kIdAbort = 3;
constexpr std::uint32_t kIdRetry = 4;
constexpr std::uint32_t kIdIgnore = 5;
constexpr std::uint32_t kIdYes = 6;
constexpr std::uint32_t kIdNo = 7;  // ★ MB_YESNO 的默认 / Esc / X（硬要求；`0x4BA82B cmp eax,6`）
constexpr std::uint32_t kIdTryAgain = 10;
constexpr std::uint32_t kIdContinue = 11;

// 调用账（见 message_box.h 的那一段；只由 shim 写、只由运行摘要读）。
// ★ 用原子：feeder 线程也会调 shim（虽已先判线程、不进系统框），但**计数**仍可能与主线程并发写。
std::atomic<std::uint64_t> g_messagebox_calls{0};
std::atomic<std::uint32_t> g_messagebox_last_type{0};
std::atomic<std::uint32_t> g_messagebox_last_result{0};

// 正文按 `\r\n` / `\n` / `\r` 切行（系统框自己会折行 ⇒ 这里**不**做字符折行）。
std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string cur;
    bool prev_cr = false;
    for (const char ch : text) {
        if (ch == '\r') {
            lines.push_back(cur);
            cur.clear();
            prev_cr = true;
        } else if (ch == '\n') {
            if (!prev_cr) {
                lines.push_back(cur);
                cur.clear();
            }
            prev_cr = false;
        } else {
            prev_cr = false;
            cur.push_back(ch);
        }
    }
    lines.push_back(cur);
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    return lines;
}

ModalSpec buildSpec(std::uint32_t u_type) {
    ModalSpec spec;
    const std::uint32_t low = u_type & 0x0Fu;
    auto add = [&spec](std::uint32_t id, const char* label) {
        spec.buttons.push_back(ModalButton{id, label, false});
    };
    switch (low) {
        case 0x0:  // MB_OK
            add(kIdOk, "OK");
            spec.esc_result = kIdOk;
            spec.close_result = kIdOk;
            break;
        case 0x1:  // MB_OKCANCEL
            add(kIdOk, "OK");
            add(kIdCancel, "Cancel");
            spec.esc_result = kIdCancel;
            spec.close_result = kIdCancel;
            break;
        case 0x2:  // MB_ABORTRETRYIGNORE
            add(kIdAbort, "Abort");
            add(kIdRetry, "Retry");
            add(kIdIgnore, "Ignore");
            // ⚠ 偏差：Win32 没有 Cancel ⇒ Esc/X 无效；我们没有 X 就出不去 ⇒ 取默认键（Abort）。
            spec.esc_result = kIdAbort;
            spec.close_result = kIdAbort;
            break;
        case 0x3:  // MB_YESNOCANCEL
            add(kIdYes, "Yes");
            add(kIdNo, "No");
            add(kIdCancel, "Cancel");
            spec.esc_result = kIdCancel;
            spec.close_result = kIdCancel;
            break;
        case 0x4:  // MB_YESNO（★ 默认键强制 No，见文件头的硬要求）
            add(kIdYes, "Yes");
            add(kIdNo, "No");
            spec.esc_result = kIdNo;
            spec.close_result = kIdNo;
            break;
        case 0x5:  // MB_RETRYCANCEL
            add(kIdRetry, "Retry");
            add(kIdCancel, "Cancel");
            spec.esc_result = kIdCancel;
            spec.close_result = kIdCancel;
            break;
        case 0x6:  // MB_CANCELTRYCONTINUE
            add(kIdCancel, "Cancel");
            add(kIdTryAgain, "Try Again");
            add(kIdContinue, "Continue");
            spec.esc_result = kIdCancel;
            spec.close_result = kIdCancel;
            break;
        default:
            // ⛔ 不静默：按 MB_OK 处理，调用方据 `unknown_type` 打一行 warn。
            add(kIdOk, "OK");
            spec.esc_result = kIdOk;
            spec.close_result = kIdOk;
            spec.unknown_type = true;
            break;
    }
    // MB_DEFBUTTONn：0x100 = DEFBUTTON2（下标 1）… 0x300 = DEFBUTTON4（下标 3）。
    // ⛔ MB_DEFBUTTON1 的值 = 0，与"没写"不可区分 ⇒ 只有非 0 才算"显式给了"。
    const unsigned defbutton = (u_type >> 8) & 0x0Fu;
    std::size_t index = 0;
    if (defbutton != 0 && !spec.buttons.empty()) {
        index = defbutton < spec.buttons.size() ? defbutton : spec.buttons.size() - 1;
    }
    if (spec.buttons.empty()) {
        spec.default_result = kIdOk;
        return spec;
    }
    if (low == 0x4) {
        // MB_YESNO：**永远 IDNO**。显式给了 DEFBUTTON2/3/4 也忽略（它们指向 No 或越界，
        // 唯一的"指向 Yes"是 DEFBUTTON1，而它的值与"没写"不可区分）⇒ 记一行 Debug 说明。
        index = spec.buttons.size() > 1 ? 1 : 0;
        if (defbutton != 0) spec.defbutton_overridden = true;
    }
    for (std::size_t i = 0; i < spec.buttons.size(); ++i) spec.buttons[i].is_default = (i == index);
    spec.default_result = spec.buttons[index].id;
    return spec;
}

}  // namespace

ModalSpec modalSpecFromUType(std::uint32_t u_type) { return buildSpec(u_type); }

ModalSpec makeMessageBoxSpec(std::uint32_t u_type, const std::string& caption_utf8,
                             const std::string& text_utf8) {
    ModalSpec spec = buildSpec(u_type);
    spec.caption = caption_utf8;
    spec.lines = splitLines(text_utf8);
    return spec;
}

std::uint32_t modalDefaultResult(const ModalSpec& spec) {
    if (spec.buttons.empty()) return spec.default_result;
    for (const ModalButton& b : spec.buttons)
        if (b.is_default) return b.id;
    return spec.default_result;
}

std::string modalMessageText(const ModalSpec& spec) {
    std::string text;
    for (std::size_t i = 0; i < spec.lines.size(); ++i) {
        if (i != 0) text.push_back('\n');
        text += spec.lines[i];
    }
    return text;
}

ModalSpec makeCloseConfirmSpec() {
    // ★ 真机三臂（EA 见需求单 `REQ-01M72JFZ87DJNT52DW4HB8SEV6`）：1 = 終了 / 2 = タイトル / 0 = キャンセル。
    //   ⛔ 文案不从 AGERC 资源取（汉化件文本乱码）；标题与提示只作可读性。
    ModalSpec spec;
    spec.caption = "終了";
    spec.lines = {"ゲームを終了しますか？"};
    spec.buttons.push_back(ModalButton{1, "プログラムを終了する", true});
    spec.buttons.push_back(ModalButton{2, "タイトルに戻る", false});
    spec.buttons.push_back(ModalButton{0, "キャンセル", false});
    spec.default_result = 1;
    spec.esc_result = 0;
    spec.close_result = 0;
    return spec;
}

const char* closeConfirmPolicyName(CloseConfirmPolicy policy) {
    switch (policy) {
        case CloseConfirmPolicy::Ask: return "ask";
        case CloseConfirmPolicy::Exit: return "exit";
        case CloseConfirmPolicy::Title: return "title";
        case CloseConfirmPolicy::Cancel: return "cancel";
    }
    return "?";
}

bool parseCloseConfirmPolicy(std::string_view text, CloseConfirmPolicy* out) {
    if (out == nullptr) return false;
    if (text == "ask") {
        *out = CloseConfirmPolicy::Ask;
        return true;
    }
    if (text == "exit") {
        *out = CloseConfirmPolicy::Exit;
        return true;
    }
    if (text == "title") {
        *out = CloseConfirmPolicy::Title;
        return true;
    }
    if (text == "cancel") {
        *out = CloseConfirmPolicy::Cancel;
        return true;
    }
    return false;
}

std::optional<std::uint32_t> closeConfirmPolicyResult(CloseConfirmPolicy policy) {
    switch (policy) {
        case CloseConfirmPolicy::Ask: return std::nullopt;
        case CloseConfirmPolicy::Exit: return 1u;
        case CloseConfirmPolicy::Title: return 2u;
        case CloseConfirmPolicy::Cancel: return 0u;
    }
    return std::nullopt;
}

void noteMessageBoxCall(std::uint32_t u_type, std::uint32_t result) {
    g_messagebox_calls.fetch_add(1, std::memory_order_relaxed);
    g_messagebox_last_type.store(u_type, std::memory_order_relaxed);
    g_messagebox_last_result.store(result, std::memory_order_relaxed);
}

std::uint64_t messageBoxCalls() { return g_messagebox_calls.load(std::memory_order_relaxed); }
std::uint32_t messageBoxLastType() { return g_messagebox_last_type.load(std::memory_order_relaxed); }
std::uint32_t messageBoxLastResult() { return g_messagebox_last_result.load(std::memory_order_relaxed); }

}  // namespace lift
