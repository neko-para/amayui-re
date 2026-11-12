// emulator-lift · MessageBox 的**宿主自绘模态框**（纯逻辑 + 唯一一处模态循环）
//
// ★ 口径（用户 2026-11-12，覆盖旧裁决）：MessageBox 走 **系统模态 + SDL 自行实现的窗口**。
//   这不是"又一条 stub"：面板 / 正文 / 按钮由宿主 **CPU 合成一整幅不透明 RGBA8**，经
//   `PresentSink::modalPresenter()` 一次性贴上（两条呈现路径各自 blit，见 present_sdl.cpp）。
//   ⇒ 同一套 `ModalSpec` / `runModalDialog` 将来给 `_ShowDialog@12`（AGERC 设置对话框）复用。
//   ⛔ 旧口径（"恒 IDOK / 不建窗口 / 不阻塞"）是**历史**（`SCOPE.md` §1 已改判）。
//
// ★ 分层的理由（可测性）：`ModalState` / `modalLayout` / `modalHitTest` / `modalSpecFromUType`
//   都是**纯函数**（零 SDL、零 Machine、零线程）⇒ `tests/messagebox.cpp` 能单步喂输入并断言
//   返回值 / 命中 / 焦点；只有 `runModalDialog` 碰宿主（令牌 Yield / 时钟 / WindowManager）。
//
// ★ 返回值表（uType 的 Win32 真值；权威表见 `.tmp/audio-token/messagebox-design.md` §4）：
//   `uType & 0x0F` 选按钮组；`0x100/0x200/0x300` = MB_DEFBUTTON2/3/4。
//   **`MB_YESNO` 的默认键与 X/Esc 一律 IDNO(7)**（硬要求）：boot 里唯一的框 `uType=0x34`，
//   答 YES(`cmp eax,6`) ⇒ 删 marker + 返回 1 ⇒ 弹"启动设置"然后退出（`0x4BA82B`）。
//
// ★ 叠字走宿主 `FontEngine`（FreeType + Amayui CN），⛔ 不复用 `--overlay` 的 5×7 ASCII 位图
//   （`overlay.h` 明说只覆盖 ASCII，画不了日文）。缺字体后端 ⇒ 面板/按钮照画 + 一次 warn（⛔ 不静默空白）。
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lift {

class Machine;
class FontEngine;
struct X86;

// ---------------------------------------------------------------------------------------
// 几何（**唯一一处**常量；测试从 `modalLayout()` 拿矩形，⛔ 不抄一份坐标）
// ---------------------------------------------------------------------------------------
struct ModalRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    bool empty() const { return w <= 0 || h <= 0; }
};

inline constexpr int kModalPanelW = 520;   // 面板固定宽
inline constexpr int kModalPad = 18;       // 面板内边距
inline constexpr int kModalTitleH = 28;    // 标题行高（空标题 ⇒ 不占）
inline constexpr int kModalTitlePx = 18;   // 标题字号
inline constexpr int kModalLineH = 22;     // 正文行高
inline constexpr int kModalTextPx = 16;    // 正文字号
inline constexpr int kModalTextGap = 18;   // 正文块与按钮行之间
inline constexpr int kModalButtonW = 96;
inline constexpr int kModalButtonH = 28;
inline constexpr int kModalButtonGap = 12;
// 正文自动换行的**字符格**上限（半角 = 1 格、非 ASCII = 2 格）。只是显示层的折行，
// ⛔ 不影响 spec（`lines` 是切好的逻辑行）。
inline constexpr int kModalTextCells = 48;

// 一个按钮：`id` = Win32 返回值（IDOK/IDCANCEL/…），`label` 只影响显示。
struct ModalButton {
    std::uint32_t id = 0;
    std::string label;
    bool is_default = false;
};

// 一次模态框的全部**纯数据**（调用方填好即可复用驱动器）。
struct ModalSpec {
    std::string caption;             // 标题（空 ⇒ 不占标题行）
    std::vector<std::string> lines;  // 正文逻辑行（UTF-8；已按 \r\n 切开）
    std::vector<ModalButton> buttons;
    std::uint32_t default_result = 1;  // Enter / quit 收口（= 默认按钮的 id）
    std::uint32_t esc_result = 1;      // Esc
    std::uint32_t close_result = 1;    // X（关窗）
    // ---- 诊断（由 uType 派生；⛔ 不是 UI 状态）----
    bool unknown_type = false;          // 低 4 位不在 MB_* 表里 ⇒ 按 MB_OK 处理（调用方打 warn）
    bool defbutton_overridden = false;  // MB_YESNO 显式给了非 DEFAULT1 ⇒ 已强制回 IDNO（打 Debug）
};

// 一帧输入快照（宿主侧的值；生产由 `WindowManager` 填，测试脚本化它）。
struct ModalInput {
    int mouse_x = 0;
    int mouse_y = 0;
    bool lbutton = false;  // MK_LBUTTON 电平
    bool enter = false;    // VK_RETURN
    bool escape = false;   // VK_ESCAPE
    bool tab = false;      // VK_TAB
    bool shift = false;    // VK_SHIFT（Tab 反向）
    bool left = false;     // VK_LEFT
    bool right = false;    // VK_RIGHT
    bool close_requested = false;  // X（SDL 关窗请求；模态期间被挂起）
    bool quit_requested = false;   // WM_QUIT（脚本 quit / marker 超时）⇒ 用默认值收口
};

// 纯几何（屏幕坐标 = guest/画面坐标，0..screen_w-1 × 0..screen_h-1）。
struct ModalLayout {
    int screen_w = 0;
    int screen_h = 0;
    ModalRect panel;
    ModalRect title;                 // 标题矩形（空标题 ⇒ 0 尺寸）
    ModalRect text;                  // 正文块矩形（行从 y 往下按 kModalLineH 排）
    std::vector<ModalRect> button_rects;  // 与 spec.buttons 一一对应
    int focus = 0;                   // 当前焦点按钮（ModalState 改）
};

// ---------------------------------------------------------------------------------------
// 纯函数
// ---------------------------------------------------------------------------------------
// `uType` → spec（按钮集 / 默认键 / Esc / X 的返回值）。未知低 4 位 ⇒ MB_OK + `unknown_type`。
ModalSpec modalSpecFromUType(std::uint32_t u_type);
// MessageBoxA 的 spec：uType + 已解码的 UTF-8 标题/正文（正文按 \r\n 切行 + 按格折行）。
ModalSpec makeMessageBoxSpec(std::uint32_t u_type, const std::string& caption_utf8,
                              const std::string& text_utf8);
// 默认键值（Enter 用）。**总是**某个按钮的 id。
std::uint32_t modalDefaultResult(const ModalSpec& spec);

ModalLayout modalLayout(const ModalSpec& spec, int screen_w, int screen_h);
// 命中按钮 ⇒ 返回它的 id；没命中 ⇒ 0。`(x,y)` = 画面坐标。
std::uint32_t modalHitTest(const ModalSpec& spec, const ModalLayout& layout, int x, int y);

// ---------------------------------------------------------------------------------------
// 模态状态机（纯；`runModalDialog` 每帧调一次 `step`）
// ---------------------------------------------------------------------------------------
class ModalState {
public:
    explicit ModalState(ModalSpec spec, int screen_w = 1280, int screen_h = 720);

    // 喂一帧输入：`nullopt` = 继续等；有值 = 最终返回值（提交点在**按下沿**，见 .cpp）。
    std::optional<std::uint32_t> step(const ModalInput& in);

    const ModalSpec& spec() const { return spec_; }
    const ModalLayout& layout() const { return layout_; }
    // 需要重绘（焦点 / 悬停 / 按下状态变了 / 从未画过）。
    bool dirty() const { return dirty_; }
    void markPresented() { dirty_ = false; }
    int hovered() const { return hover_; }
    bool pressed() const { return pressed_; }
    bool quitRequested() const { return quit_requested_; }

private:
    ModalSpec spec_;
    ModalLayout layout_;
    int focus_ = 0;
    int hover_ = -1;
    bool prev_lbutton_ = false;
    bool prev_tab_ = false;
    bool prev_left_ = false;
    bool prev_right_ = false;
    bool pressed_ = false;
    bool dirty_ = true;
    bool quit_requested_ = false;
};

// ---------------------------------------------------------------------------------------
// 呈现缝（宿主侧；本头**不含 SDL 类型**）
//   `PresentSink::modalPresenter()` 的返回值；headless / 测试替身默认 nullptr。
// ---------------------------------------------------------------------------------------
class ModalPresenter {
public:
    virtual ~ModalPresenter() = default;
    // 贴一整幅**不透明 RGBA8**（stride = w*4）。false = 这一帧没送出去（err 必须填）。
    virtual bool presentModal(const std::uint8_t* rgba, int w, int h, std::string* err) = 0;
    // 等宿主事件（真实睡 ms；模态循环把它放在 `GuestToken::YieldGuard` 里）。
    virtual void waitHostEvents(int wait_ms) = 0;
    // 用户关了窗口（模态期间只置位，⛔ 不 post WM_CLOSE；见 present_sdl.cpp）。
    virtual bool closeRequested() const = 0;
    virtual bool modalActive() const = 0;
    // 收口：释放模态纹理 / 恢复被挂起的关窗宽限。
    virtual void endModal() = 0;
};

// ★ 重入断言：对话框里**不许**再弹对话框（嵌套 ⇒ 直接返回默认值 + 一次 warn）。
bool modalDialogInFlight();

// 驱动（**唯一一处**模态循环）：每帧 YieldGuard 等待 → tickInputScript/tickInteraction →
// 读 WindowManager → step → CPU 合成 → presentModal。返回最终结果（总是 spec 里的某个 id）。
std::uint32_t runModalDialog(Machine& machine, X86& cpu, const ModalSpec& spec);

// CPU 合成：末帧变暗（α=160）+ 居中面板 + FontEngine 字形 + 按钮；输出**整幅不透明 RGBA8**。
//   `backdrop` 可空（boot 还没有任何帧 ⇒ 纯色底 #202020）；`font_missing` 报告"没有字体后端"。
bool composeModalFrame(const ModalSpec& spec, const ModalLayout& layout, const ModalInput& in,
                       FontEngine* fonts, const std::uint8_t* backdrop, int backdrop_w, int backdrop_h,
                       int backdrop_pitch, std::vector<std::uint8_t>* out, int* out_w, int* out_h,
                       bool* font_missing);

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
