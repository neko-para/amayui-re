// emulator-lift · CLI 层"宿主副作用开关"的**唯一**一份判据 + 文案（`--cursor-warp` / `--sound`）
//
// ★ 为什么抽这个头（`AGENTS.md` §10"不写第二份"）：这两件事的**运行时**判据本来就只有一处
//   （`SdlWindow::hostCursorWarpEnabled()` / `audio.cpp` 的 `mute_ = !sound`，见下），但
//   **CLI 面**的"有效状态"与"拒绝 / 说明"文案会随臂数变多：解释器臂（`src/main.cpp`）与
//   AOT 臂（`src/aot/aot_main.cpp`）都要回答"这轮真的会不会挪宿主光标 / 会不会出声"。
//   ⇒ 判据与那句话只落在这里，两臂**只做同一件事**：取值 + 打印。
//
// ★ 形状：**pure / SDL-free / 零分配**（与 `runtime/warp_echo.h`、`runtime/cursor_map.h` 同一类）：
//   只用 `constexpr` + 字符串字面量，不 include 任何平台 / SDL 头，可被 pure CTest 直接 include。
//   ⛔ 这里**不新增**任何运行时判据 —— 真正的"挪不挪"仍然由 sink 自己算：
//     · `present_sdl.cpp` 的 `hostCursorWarpEnabled() = impl_->cursor_warp && !windowHidden()`；
//     · `audio.cpp:141` 的 `mute_ = !sound`（唯一交付点 `generateTo` 末尾）。
//   本头只负责"**把同一个 bool 组合成给人看的状态**"，⛔ 不参与 guest 语义。
//
// ★ **AOT 臂的音频已接线（S0–S3）**：档位由 `--audio` 决定、`--sound` 真的进 `mute_`、`--audio-dump`
//   落 WAV、`run()` 入口 / 出口成对 `begin()` / `end()`、泵（`pump()`）已接在**块边界**（S3）。
//   ⇒ `--sound` 被**接受**，但默认档**不打任何通告**（2026-11-27 用户判据：解释性文字只进 `--help`；
//   ⛔ 不静默接受是指"不拒绝 / 不假装有声音"，不是"必须打一句说明书"）。
// ★ 洞 4（`REQ-01M7K04EY6D3GQNB5DRWP0DYJ8`）的教训：**文案引用的每一行读数都必须真的存在**。
//   `audio     :` / `audio-pump:` 只在 `--log-level info` **且** `AudioEngine` 已配置时打
//   （`aot_host.cpp` 的 `printRunSummary`）⇒ **只给 `--sound`** 时档位是 off，那两行**不存在**
//   ⇒ `kAotSoundHelp` 必须限定范围。
#pragma once

#include <cstdint>

namespace lift {

// =====================================================================================
// `--cursor-warp`：requested + 平台 + 可见性 ⇒ **有效状态**
//
// ★ 这是**唯一**一份映射。两个臂拿它算状态行（`cursor-warp: <name>`）与"隐藏了 ⇒ 它不会生效"
//   的那个 warn 判据。⛔ 不要在某个臂里另写 `if (cursor_warp && !hidden)`。
// ★ `Unsupported` 在 parse 期就被拒绝（`exit 2`），保留这个枚举值是为了让状态行也能表达
//   "万一它没被挡住"（例如解释器臂未来的 `--report` 类路径）。
// =====================================================================================
enum class CursorWarpState : std::uint8_t {
    Off = 0,      // 没给 `--cursor-warp`（缺省）
    On,           // 给了 + 平台支持 + 窗口**当前可见** ⇒ 这轮真的会挪宿主光标
    OnButHidden,  // 给了 + 平台支持，但窗口隐藏 ⇒ **静默无效**，必须 warn 一次
    Unsupported,  // 给了，但本平台没有实现（Linux / 其它）
};

constexpr CursorWarpState cursorWarpState(bool requested, bool platform_supported, bool window_hidden) {
    if (!requested) return CursorWarpState::Off;
    if (!platform_supported) return CursorWarpState::Unsupported;
    if (window_hidden) return CursorWarpState::OnButHidden;
    return CursorWarpState::On;
}

/** 状态行的**值**（`cursor-warp: <这里>`）；四个枚举值各一句，⛔ 不许在各臂里另写。 */
constexpr const char* cursorWarpStateName(CursorWarpState s) {
    switch (s) {
        case CursorWarpState::Off:
            return "off";
        case CursorWarpState::On:
            return "on";
        case CursorWarpState::OnButHidden:
            return "on-but-hidden（隐藏窗口 ⇒ 不挪宿主光标）";
        case CursorWarpState::Unsupported:
            return "unsupported（本平台未实现）";
    }
    return "off";
}

// =====================================================================================
// 两臂共用的**CLI 文案**（拒绝 / warn / --help 正文）
//
// ★ 带 `%s` 的是给 `std::fprintf(stderr, fmt, …)` 的格式串 —— `SdlWindow::init` 之前就能判，
//   ⛔ 不要在臂里再抄一份（抄一份就等于允许两臂说不一样的话）。
// =====================================================================================

/** `--help` 里 `--cursor-warp` 那三行（解释器臂与 AOT 臂**逐字相同**）。 */
inline constexpr const char* kCursorWarpHelp =
    "  --cursor-warp       让 guest 的 SetCursorPos 同时移动真实光标（缺省关；只在\n"
    "                      --present window/auto 且窗口可见时生效）。\n";

/** 平台闸门拒绝（`%s` = 程序名，`%s` = 平台名）；`exit 2` 之前那两句。 */
inline constexpr const char* kCursorWarpUnsupportedFormat =
    "%s: --cursor-warp 在本平台未实现（%s）；本线只做 win32 / macOS\n"
    "  ⇒ 去掉该参数（注入位置由配方决定：AOT 臂 `--input-plan`；见 README §4.2d）\n";

/** 窗口隐藏 ⇒ 这轮不生效（**一次性** stderr warn；判据 = `CursorWarpState::OnButHidden`）。 */
inline constexpr const char* kCursorWarpHiddenWarn =
    "  ⚠ --cursor-warp 本轮不生效（窗口隐藏 ⇒ 不挪宿主光标；只改 guest 虚拟光标）\n";

/** `--present headless` 拒绝窗口参数那句（`%s` = 这条臂**自己支持**的窗口参数清单）。 */
inline constexpr const char* kHeadlessRejectsWindowFlagsFormat = "--present headless 不接受窗口参数（%s）\n";

/** 解释器臂支持的窗口参数清单（该臂已退役；保留为历史口径的唯一一份参数串）。 */
inline constexpr const char* kWindowFlagsInterpreter =
    "--present-driver / --present-vsync / --present-hidden / --present-screenshot / --overlay / --cursor-warp";

/** AOT 臂支持的窗口参数清单（M0 起含 `--overlay`；S9 起含 `--ui-input`；S3 删掉 `--ui <path>`）。 */
inline constexpr const char* kWindowFlagsAot =
    "--present-driver / --present-vsync / --present-hidden / --present-screenshot / --overlay / "
    "--ui-input=consume|pass / --cursor-warp";

// =====================================================================================
// `--sound`：**接受**，且默认档**一个字都不打**
//
// ★ 用户的判据（2026-11-27）：默认档只留"出错/警告（可操作）"与插件自己的日志；解释性文字
//   （"这个参数什么意思 / 读数在哪看 / 口径是什么 / 跟踪单号"）一律进 `--help`（`kAotSoundHelp`）。
//   ⇒ 这里**没有** `kAotSoundNotice` 这类运行期通告：参数被接受的事实由"argv 不报错 + 真进了静音门"
//   回答，⛔ 不再靠逐次打印一句说明书。
// ★ 硬事实（S0–S2 接线之后）：`--sound` **真的**进 `AudioEngine` 的静音门（`mute_ = !sound`，唯一
//   交付点 `generateTo` 末尾）⇐ `aot_main` 把它传给 `AotHost::configureAudio()`；档位来自
//   `--audio off|sdl`，PCM 落盘来自 `--audio-dump`。⛔ 不许再说"本臂没有音频管线 / 不会有声音"。
// ★ S3 之后（`REQ-01M7JZM1QQ728KX1C0G2F232T1`）：泵接在**块边界**（`aot_run.cpp` 的 ⑤'，每
//   `kCheckStride` 条 guest 指令一次；AOT 恒主线程 D 泵 —— `cpu_.machine == nullptr` ⇒ S0 谓词恒
//   false）⇒ `fills / fill_bytes / pushed` 是**读数**，不再是结构性的 0。
// ★ 读数面（`REQ-01M7K04EY6D3GQNB5DRWP0DYJ8` 的洞 4/5，2026-11-17 实测）：`audio     :` /
//   `audio-pump:` 两行**只在给了 `--audio sdl` 或 `--audio-dump`、且 `--log-level info` 时**才有
//   （只给 `--sound` ⇒ 档位 off；默认 `--log-level warn` ⇒ 一行都不打）。`--audio-dump` 的 WAV
//   是 44 字节头 + 这一跑**真交付**的字节，⛔ 不保证有 PCM（判断"有没有 PCM"看
//   fills / fill_bytes / mixed / pushed，⛔ 别只看文件在不在）。
// ★ 判据落在**引擎状态**上：CTest `aot-audio-wiring`（真 run，读收口摘要的 `audio     : … muted=0`，
//   并用 `--audio-dump` 不给 `--sound` 的 `muted=1` 当对照组）。⛔ 不要拿"通告文案"当证据（洞 6）。
// =====================================================================================

/** `--help` 里 AOT 臂 `--sound` 那一行（解释性文字的唯一落点；**≤2 行纯用户事实**）。 */
inline constexpr const char* kAotSoundHelp =
    "  --sound              取消缺省静音门（muted=0）；只有配合 --audio sdl 才出声，\n"
    "                      --audio off 时无输出。\n";

}  // namespace lift
