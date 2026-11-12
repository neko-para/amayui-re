// emulator-lift · MessageBox / AGERC「終了」框的**纯映射层**（不再自绘任何像素）
//
// ★ 口径（用户 2026-11-12 最终版，覆盖早先的"SDL 自绘"）：**直接用系统对话框**
//   （`SDL_ShowMessageBox`）模拟，只要正文 + 按钮；⛔ 不还原图标 / 位置 / 像素。
//   本头只放**纯函数**（uType/选择子 → `ModalSpec`）与策略解析；真正的弹框在
//   `PresentSink::showSystemMessageBox()`（`runtime/present.h`，窗口 sink = SDL 实现）。
//
// ★ 返回值表（uType 的 Win32 真值）：
//   `uType & 0x0F` 选按钮组；`0x100/0x200/0x300` = MB_DEFBUTTON2/3/4。
//   **`MB_YESNO` 的默认键与 X/Esc 一律 IDNO(7)**（硬要求）：boot 里唯一的框 `uType=0x34`，
//   答 YES(`cmp eax,6`) ⇒ 删 marker + 返回 1 ⇒ 弹"启动设置"然后退出（`0x4BA82B`）。
//
// ★ AGERC `_ShowDialog@12` 的 `id==3`（关窗二次确认「終了」）也走同一套 spec/系统框：
//   1 = プログラムを終了する / 2 = タイトルに戻る / 0 = キャンセル；默认 1、Esc/X = 0。
//   ⛔ 文案不从 AGERC 资源取（汉化件那份文本是乱码），文案在 `makeCloseConfirmSpec()`。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lift {

// 一个按钮：`id` = Win32 返回值（IDOK/IDCANCEL/1/2/0…），`label` = 系统框里显示的文字。
struct ModalButton {
    std::uint32_t id = 0;
    std::string label;
    bool is_default = false;  // 默认键（系统框的 Return 键）
};

// 一次系统对话框的全部**纯数据**。⛔ 不含几何 / 字体 / 帧缓冲（那些随自绘一起删掉了）。
struct ModalSpec {
    std::string caption;             // 标题
    std::vector<std::string> lines;  // 正文逻辑行（UTF-8；系统框用 `\n` 连接）
    std::vector<ModalButton> buttons;
    std::uint32_t default_result = 1;  // Enter / 默认按钮
    std::uint32_t esc_result = 1;      // Esc
    std::uint32_t close_result = 1;    // X（窗口关闭）
    // ---- 诊断（由 uType 派生；⛔ 不是 UI 状态）----
    bool unknown_type = false;          // 低 4 位不在 MB_* 表里 ⇒ 按 MB_OK 处理（调用方打 warn）
    bool defbutton_overridden = false;  // MB_YESNO 显式给了非 DEFAULT1 ⇒ 已强制回 IDNO（打 Debug）
};

// `uType` → spec（按钮集 / 默认键 / Esc / X 的返回值）。未知低 4 位 ⇒ MB_OK + `unknown_type`。
ModalSpec modalSpecFromUType(std::uint32_t u_type);
// MessageBoxA 的 spec：uType + 已解码的 UTF-8 标题/正文（正文按 \r\n 切行）。
ModalSpec makeMessageBoxSpec(std::uint32_t u_type, const std::string& caption_utf8,
                              const std::string& text_utf8);
// 默认键值（Enter 用）。**总是**某个按钮的 id。
std::uint32_t modalDefaultResult(const ModalSpec& spec);
// 正文 → 系统框的一段文本（行用 `\n` 连接）。纯函数（可测）。
std::string modalMessageText(const ModalSpec& spec);

// ---------------------------------------------------------------------------------------
// 关窗二次确认（AGERC `_ShowDialog@12` 的 id==3「終了」框；`REQ-01M72JFZ87DJNT52DW4HB8SEV6`）
// ---------------------------------------------------------------------------------------
// ★ 返回值（真机三臂，EA 锚在需求单里）：**1 = プログラムを終了する**（→ 0x405B21 全黑 + 继续关窗）、
//   **2 = タイトルに戻る**（→ 0x405A82 返回 0，不关窗）、**0 = キャンセル**（→ 0x405A95 撤减光，不关窗）。
//   Esc 与 X 都是 **0（cancel）**；默认按钮 = 1（真资源的 IDOK + 默认选中的 radio）。
//   ⛔ 文案**不从 AGERC 资源取**：标题「終了」+ 一句简短日文，只作提示。
ModalSpec makeCloseConfirmSpec();

// `--close-confirm` 的策略：`ask` = 弹系统框（缺省）；其余三档**不弹框**、直接按该选择返回。
//   `exit` = 今天的行为（等于 AGERC stub 恒返回 1）；`title` = 2；`cancel` = 0。
enum class CloseConfirmPolicy : std::uint8_t {
    Ask = 0,
    Exit = 1,
    Title = 2,
    Cancel = 3,
};
const char* closeConfirmPolicyName(CloseConfirmPolicy policy);
bool parseCloseConfirmPolicy(std::string_view text, CloseConfirmPolicy* out);
// Ask ⇒ `nullopt`（要弹系统框）；Exit/Title/Cancel ⇒ 1/2/0（策略短路）。
std::optional<std::uint32_t> closeConfirmPolicyResult(CloseConfirmPolicy policy);

// ---------------------------------------------------------------------------------------
// 调用账（运行摘要 `messagebox: calls=… last-type=… last-result=…` 的后两项）
//   `calls` 取自 `ShimDef::call_count`（`shims.cpp` 唯一 `++` 点），这两项只记**最后一次**。
//   ⛔ 不是判据数据：判据是 `--trace-shims` 的行与 `call_count`。
// ---------------------------------------------------------------------------------------
void noteMessageBoxCall(std::uint32_t u_type, std::uint32_t result);
std::uint64_t messageBoxCalls();
std::uint32_t messageBoxLastType();
std::uint32_t messageBoxLastResult();

}  // namespace lift
