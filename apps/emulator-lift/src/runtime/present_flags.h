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
//   ⇒ 本头里的 `kAotSoundNotice` 是"**接受 + 说实话**"那一条一次性说明（⛔ 不是拒绝启动，
//   ⛔ 也不静默接受，⛔ 不再说"没有音频管线 / 不会有声音"）。
// ★ 洞 4（`REQ-01M7K04EY6D3GQNB5DRWP0DYJ8`）的教训：这条说明**引用的每一行读数都必须真的存在**。
//   `audio     :` / `audio-pump:` 只在 `AudioEngine::enabled()` 时打（`aot_host.cpp` 的
//   `printRunSummary`）⇒ **只给 `--sound`** 时档位是 off，那两行**不存在** ⇒ 文案必须限定范围。
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
    "  --cursor-warp       让 guest 的 SetCursorPos **同时移动你机器的真实光标**（缺省关；\n"
    "                      只在 --present window/auto 且窗口可见时生效；宿主副作用，\n"
    "                      除\"开着却挪不动 ⇒ FALSE\"那一裁决例外外，虚拟光标 / GetCursorPos / "
    "WM_* 逐位不变）\n";

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

/** AOT 臂支持的窗口参数清单（M0 起含 `--overlay`；B 起含 `--ui <path>`；S9 起含 `--ui-input`）。 */
inline constexpr const char* kWindowFlagsAot =
    "--present-driver / --present-vsync / --present-hidden / --present-screenshot / --overlay / --ui <path> / "
    "--ui-input=consume|pass / --cursor-warp";

// =====================================================================================
// `--sound`：**接受 + 一条与代码同形的一次性说明**
//
// ★ 裁决（用户口径）：用户要的是"**能指定**" ⇒ ⛔ 不许 `exit 2`（那是另一种"无法指定"）；
//   同时 ⛔ 不许静默接受（静默 ⇒ 用户以为该有声音）。
// ★ 现在的硬事实（S0–S2 接线之后）：`--sound` **真的**进 `AudioEngine` 的静音门
//   （`mute_ = !sound`，唯一交付点 `generateTo` 末尾）⇐ `aot_main` 把它传给
//   `AotHost::configureAudio()`；档位来自 `--audio off|virtual|sdl`，PCM 落盘来自 `--audio-dump`。
//   ⇒ 这条说明**必须**与代码同形：⛔ 不许再说"本臂没有音频管线 / 不会有声音"（那是接线前的形状，
//   也是一句会被守卫 `cli-aot-flags` 当场判红的假话）。
// ★ S3 之后这条说明的**边界**（2026-11-17，`REQ-01M7JZM1QQ728KX1C0G2F232T1`）：
//   泵已接在**块边界**（`aot_run.cpp` 的 ⑤'，每 `kCheckStride` 条 guest 指令一次；AOT 恒主线程
//   D 泵 —— `cpu_.machine == nullptr` ⇒ S0 谓词恒 false）⇒ `fills / fill_bytes / pushed` 不再是
//   结构性的 0，它们是**读数**（有没有真喂到料取决于跑到了哪、以及本机有没有设备）。
//   ⇒ 这条说明⛔ 不许再说"泵还没接线 / 这三个量本轮会是 0"：那是一句会被守卫当场判红的假话
//     （同一个理由见上面 105-106 行；S3 之前的原文就是那么写的）。
// ★ 打印点**只有一个**（`aot_main.cpp` 的参数校验段），且在 GPU bring-up **之前**
//   ⇒ ① 一次（不是每帧）；② 命令行一给就能判（守卫 `cli-aot-flags` 因此不需要 GPU）。
// ★ **这份文本不许再当证据**（洞 6）：`cli-aot-flags` 的 `sound-accepted-with-once-notice` 曾断言
//   "这段话里含 `muted=0`"——那是**自证式断言**（只断言自己打的那句话，`--sound` 的接线丢了照绿）。
//   真正的判据落在**引擎状态**上：CTest `aot-audio-wiring`（真 run，读收口摘要的
//   `audio     : … muted=0`，并用 `--audio virtual` 不给 `--sound` 的 `muted=1` 当对照组）。
// ★ 洞 4/5 的口径（2026-11-17，`REQ-01M7K04EY6D3GQNB5DRWP0DYJ8`）：
//   · 读数那两行**只在 `--audio virtual|sdl` 或 `--audio-dump` 时**存在（只给 `--sound` ⇒ 档位 off）：
//     实测 `--sound` ⇒ stdout 无 `audio     :` / `audio-pump:` 行；`--sound --audio virtual` ⇒ 两行都在。
//   · `--audio-dump` 的字节数**照实测定**（2026-11-17，AOT 臂，`--exe` 拆壳件 +
//     `--game-dir ../../dist/install` + `--audio-dump <tmp>.wav --max-frames 300 --skip-logo
//     --present headless`）：**仍是 44 B（data=0）**，同一次 run 的读数是
//     `fills=12 fill_bytes=1152000 mixed=0 pushed=0` ⇒ S3 接线之后 dump **也不保证有 PCM**
//     （交付字节账 = `mixed` / `pushed`；`fills` / `fill_bytes` 只说明 guest 真的写过环）。
//     ⛔ 不许把"S3 之后就有 PCM"写进文案——那是没验证过的承诺。
// ★ 跟踪单 id 写在 `kAotSoundNotice` 里（`REQ-01M7G7GPBG9BK2360WSG6DBFNP` = S0–S2；
//   `REQ-01M7JZM1QQ728KX1C0G2F232T1` = S3 泵 / S4 出声）。
//   守卫 `cli-aot-flags` 按**形态**判（必须出现 `REQ-`、且不许出现 `TODO(`）——换单号不会让它红。
// =====================================================================================
inline constexpr const char* kAotSoundNotice =
    "emulator-lift-aot: --sound 已接受：取消缺省静音门（muted=0），并真正传给音频引擎"
    "（不再是硬写 false）\n"
    "  · 档位由 --audio 决定（缺省 off / virtual / sdl）；--audio-dump <path> 把交付缓冲写成 WAV\n"
    "  · 音频泵在**块边界**推进（每 4096 条 guest 指令一次；AOT 恒主线程 D 泵）\n"
    "  · 读数：**仅当给了 --audio virtual|sdl 或 --audio-dump 时**，收口时 stdout 才有 "
    "`audio     :` 一行（mode / device / muted / muted_bytes / …）与 `audio-pump:` 一行"
    "（calls / fills / fill_bytes / pushed）；只给 --sound 时档位是 off ⇒ **这两行都不存在**\n"
    "  · --audio-dump 的 WAV = 44 字节头 + 这一跑**真交付**（mixed）的字节（data 记在头里）："
    "没有真交付时它就是 44 字节（data=0）—— S3 接泵之后也**不保证**有 PCM\n"
    "  · 判断\"有没有 PCM\"看 fills / fill_bytes / mixed / pushed，⛔ 别只看文件在不在\n"
    "  · 跟踪单：REQ-01M7G7GPBG9BK2360WSG6DBFNP（S0 定档 / S1 参数面 / S2 生命周期）、"
    "REQ-01M7JZM1QQ728KX1C0G2F232T1（S3 泵）\n";

/** `--help` 里 AOT 臂 `--sound` 那一行（与上面那条说明同一口径：接受，且真的进静音门）。 */
inline constexpr const char* kAotSoundHelp =
    "  --sound              取消缺省静音门（muted=0）；档位仍由 --audio 决定\n"
    "                      （缺省 off ⇒ 不开设备 / 不产生 PCM；读数 `audio     :` 一行**只在**\n"
    "                      给了 --audio virtual|sdl 或 --audio-dump 时才有）\n";

}  // namespace lift
