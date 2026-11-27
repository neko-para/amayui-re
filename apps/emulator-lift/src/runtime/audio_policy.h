// emulator-lift · 音频**参数面 + 预读常量**（`--audio` 的值域 + lead）
//
// ★ 音频只有一条喂料路线：**feeder 线程**（真宿主线程；见 `audio.h`）。`--audio` 只决定
//   **输出位置**：
//     · `off`（缺省）⇒ 没有音频输出（不跑混音 / 不起 feeder）；
//     · `sdl`       ⇒ SDL3 设备（打不开设备 = 没有输出，档位落回 off，摘要写 `device=none`）；
//     · `--audio-dump <路径>` ⇒ 设备无关的 WAV 输出（**不是档位**，`--audio` 的值域仍只有 off / sdl）。
//   ⛔ 没有"主线程泵 / D 泵 / 虚拟时钟档"这回事了（2026-11-27 用户裁决整条移除）。
//
// ★ 本头**纯**：只 `<cstdint>` + 编译期字符串比较，不 include SDL / x86 / guest 头
//   ⇒ 可被 pure CTest 直接驱动（`tests/aot_audio_flags.cpp`），也**没有**第二份实现。
//   `audioModeName` 的**定义**仍在 `runtime/audio.cpp`（唯一一份），这里只声明。
#pragma once

#include <cstdint>

namespace lift {

enum class AudioMode : std::uint8_t {
    Off = 0,  // 不出声、不跑混音（缺省；`--audio off`；`--audio sdl` 打不开设备时也落回这里）
    Sdl = 1,  // SDL3 真设备
};

const char* audioModeName(AudioMode m);

// =====================================================================================
// ★ 预读 lead（"在设备之前先垫多少声音"）—— 唯一一份常量，`audio.cpp` 的 `mixStep()` 与
//   `begin()` 的日志行都从这里取（⛔ 不要在 audio.cpp 里再写一个三元表达式）。
//
//   · feeder 与 mixer 都在自己的线程，主线程卡在 Present/长 shim 不再断音频。
//     独立 mixer 实测 fill ≈ 400 ms 也不影响设备队列；100 ms 足以覆盖 5 ms mixer 周期 + 抖动。
//   · `192000 B/s × 100/1000`（48k / 16 bit / 2 ch）。`audio.cpp` 有 `static_assert` 把它钉住
//     （防"改了采样率忘改 lead"）。
// =====================================================================================
inline constexpr std::uint64_t kThreadLeadBytes = 19200ull;  // 100 ms（feeder 线程）

// `--audio <值>` 的解析结果。`error` 是**静态串**（只有 `!ok` 时非空；调用方直接 `%s`）。
struct AudioModeParse {
    bool ok = false;
    AudioMode mode = AudioMode::Off;
    const char* error = nullptr;
};

// 编译期字符串相等（⛔ 不用 `std::string`：本头要能被 pure CTest 在无分配的前提下驱动）。
constexpr bool audioTextEquals(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) return false;
    while (*a != '\0' && *b != '\0') {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

// ★ S1：`--audio` 的值域判据（**严格**：大小写敏感、不接受前后缀 / 空串）。
//   ⛔ 不要放宽成"认不出就当 Off"：那会让"以为有声"变成一条**静默**失效（本仓最贵的一类 bug）。
constexpr AudioModeParse parseAudioMode(const char* value) {
    if (audioTextEquals(value, "off")) return {true, AudioMode::Off, nullptr};
    if (audioTextEquals(value, "sdl")) return {true, AudioMode::Sdl, nullptr};
    return {false, AudioMode::Off, "只能是 off / sdl"};
}

}  // namespace lift
