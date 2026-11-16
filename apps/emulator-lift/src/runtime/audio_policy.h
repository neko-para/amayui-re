// emulator-lift · 音频**档位判定 + 参数面**的唯一一份纯判据（S0 / S1）
//
// ★ 为什么抽这个头：这两件事过去散在实现里，而且都**不可测**：
//   ① **定档**：`audio.cpp` 的 `begin()` 旧写法是 `(mode == Sdl && clock->realTime())` —— 它隐含
//      一个结构前提"有 Machine"：真 feeder 线程要 `cpu_->machine->layout().feeder_teb`
//      （`AudioEngine::setupFeederContext` 的第一道硬门），而 AOT 臂**有意** `cpu_.machine == nullptr`
//      ⇒ 旧写法只会先起 mixer 线程、等 `setupFeederContext` 失败之后**回退**成 D 泵（并打一条
//      误导性 Warn："feeder 真线程启动失败"）。
//      ⇒ 判据提成纯函数 `audioThreadMode()`：**AOT 从一开始就选 D**，⛔ 不靠失败回退。
//   ② **参数面**：`--audio` 的值域是闭集（off / virtual / sdl），非法值必须 **exit 2 并点名合法值**
//      （⛔ 不放宽成"认不出就当 off" —— 那会让"以为有声"变成静默失效）。
//      ⇒ 判据提成纯函数 `parseAudioMode()`（`aot_main` 的唯一调用点）。
//
// ★ 本头**纯**：只 `<cstdint>` + 编译期字符串比较，不 include SDL / x86 / guest 头
//   ⇒ 可被 pure CTest 直接驱动（`tests/aot_audio_flags.cpp`），也**没有**第二份实现。
//   `audioModeName` 的**定义**仍在 `runtime/audio.cpp`（唯一一份），这里只声明。
//
// ★ S0 谓词判什么（三个输入都是事实，不是选项）：
//   · `mode`         = `AudioEngine::begin()` 那一刻**生效**的档（`--audio sdl` 打开设备失败会
//                      先退回 `Virtual` —— 回退发生在**算定档之前**，所以谓词看到的是真档位）；
//   · `real_time`    = 宿主时钟是不是墙钟（虚拟时钟的 tick 是主线程的确定性账本，不能由第二条
//                      guest 执行流并发推进：`audio.h` 文件头的时钟档口径）；
//   · `has_machine`  = `cpu_ != nullptr && cpu_->machine != nullptr`（真 feeder 线程要
//                      `Machine::layout().feeder_teb`；AOT 有意没有 Machine）。
//   ⇒ 只有三者全真才进 thread 档；**AOT 恒 false ⇒ 恒 D 泵**。
#pragma once

#include <cstdint>

namespace lift {

enum class AudioMode : std::uint8_t {
    Off = 0,      // 不出声、不跑泵（缺省；`--audio off`）
    Virtual = 1,  // 虚拟设备时钟（确定性；可 dump）
    Sdl = 2,      // SDL3 真设备（能听到）
};

const char* audioModeName(AudioMode m);

// ★ S0：本引擎此刻该走**真 feeder 线程**（"thread 档"）吗？false = 主线程 D 泵。
//   三个输入全是**事实**：生效档位 / 宿主时钟是否墙钟 / 有没有 Machine。只有三者全真才进 thread 档。
constexpr bool audioThreadMode(AudioMode mode, bool real_time_clock, bool has_machine) {
    return mode == AudioMode::Sdl && real_time_clock && has_machine;
}

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
    if (audioTextEquals(value, "virtual")) return {true, AudioMode::Virtual, nullptr};
    if (audioTextEquals(value, "sdl")) return {true, AudioMode::Sdl, nullptr};
    return {false, AudioMode::Off, "只能是 off / virtual / sdl"};
}

}  // namespace lift
