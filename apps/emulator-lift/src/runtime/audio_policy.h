// emulator-lift · 音频**档位判定 + 参数面**的唯一一份纯判据（S0 / S1）
//
// ★ 为什么抽这个头：这两件事过去散在实现里，而且都**不可测**：
//   ① **定档**：`audio.cpp` 的 `begin()` 旧写法带一个 `has_machine` 门（"真 feeder 线程要
//      `Machine::layout().feeder_teb`；AOT 有意没有 Machine ⇒ 恒 false"）。那条门是**错的**：
//      AOT 没有 Machine，但宿主有同一份 `GuestLayout`（`AotHost::layout_`）⇒ 用"有没有 Machine"
//      当"能不能起 feeder"的替身，只会让 AOT 恒走主线程 D 泵（预读 lead = 600 ms ⇒ 所有音效从
//      触发到听见都有 ~0.6 s 间隔）。
//      ⇒ 判据提成纯函数 `audioThreadMode()`：**只由两个事实决定** —— 生效档位与宿主时钟。
//      "能不能起 feeder"由宿主装配期**必须**给 `AudioEngine::attach()` 一份 `GuestLayout`
//      （引用入参，不可为空）表达；⛔ 不再是一个可以"声明 true 而实际没有"的运行时布尔。
//   ② **参数面**：`--audio` 的值域是闭集（off / virtual / sdl），非法值必须 **exit 2 并点名合法值**
//      （⛔ 不放宽成"认不出就当 off" —— 那会让"以为有声"变成静默失效）。
//      ⇒ 判据提成纯函数 `parseAudioMode()`（`aot_main` 的唯一调用点）。
//
// ★ 本头**纯**：只 `<cstdint>` + 编译期字符串比较，不 include SDL / x86 / guest 头
//   ⇒ 可被 pure CTest 直接驱动（`tests/aot_audio_flags.cpp`），也**没有**第二份实现。
//   `audioModeName` 的**定义**仍在 `runtime/audio.cpp`（唯一一份），这里只声明。
//
// ★ S0 谓词判什么（两个输入都是事实，不是选项）：
//   · `mode`         = `AudioEngine::begin()` 那一刻**生效**的档（`--audio sdl` 打开设备失败会
//                      先退回 `Virtual` —— 回退发生在**算定档之前**，所以谓词看到的是真档位）；
//                      ★ 保留 `mode == Sdl` 这一项：`Virtual` 一律 D 泵 ⇒ dump 的确定性不被
//                      第二条线程破坏（设备打不开回退 virtual 时也就自然回 D 泵）。
//   · `real_time`    = 宿主时钟是不是墙钟（虚拟时钟的 tick 是主线程的确定性账本，不能由第二条
//                      guest 执行流并发推进：`audio.h` 文件头的时钟档口径）。
//   ⇒ 只有两者全真才进 thread 档。
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
//   两个输入全是**事实**：生效档位 / 宿主时钟是否墙钟。
//   ★ 第三个事实（"宿主给没给 GuestLayout"）**不在谓词里**：它由 `AudioEngine::attach()` 的
//     `const GuestLayout&` 入参在编译期钉死（引用不可空）⇒ "想 thread 却没有布局"不可表示。
//     ⛔ 不要把 `has_machine` / `feederLayoutAvailable()` 这类运行时布尔加回来：那正是
//     "先声明能起、失败了再回退 + 误导性 Warn"的形状（本头旧版注释自述过）。
constexpr bool audioThreadMode(AudioMode mode, bool real_time_clock) {
    return mode == AudioMode::Sdl && real_time_clock;
}

// =====================================================================================
// ★ 预读 lead（"在设备之前先垫多少声音"）—— 唯一一份常量，`audio.cpp` 的 `mixStep()` 与
//   `begin()` 的日志行都从这里取（⛔ 不要在 audio.cpp 里再写一份三元表达式）。
//
//   · 泵模式（D）：主线程在一次 fill 期间**暂停混音**（单执行流，fill 写 guest 环时不能同时读它）。
//     实测（RelWithDebInfo，48k 立体声 BGM）：一次 fill ≈ 360–460 ms 墙钟 ⇒ fill 之后到下一个
//     通知点只剩下 ~140 ms，所以 lead 必须 > 500ms 才能保证"fill 开始时队列里的音频撑得过整个
//     fill"；这里取 600ms（≈100ms 余量）。⛔ 这不是设备延迟，是 D 的吞吐代价；代价是 SE 也有
//     ~600ms 延迟。
//   · 真线程模式：feeder 与 mixer 都在自己的线程，主线程卡在 Present/长 shim 不再断音频。
//     独立 mixer 实测 fill ≈ 400 ms 也不影响设备队列；100 ms 足以覆盖 5 ms mixer 周期 + 抖动。
//   ★ 两个值 = `192000 B/s × 600/1000` 与 `× 100/1000`（48k / 16 bit / 2 ch）。
//     `audio.cpp` 里有 `static_assert` 把这两条等式钉住（防"改了采样率忘改 lead"）。
// =====================================================================================
inline constexpr std::uint64_t kLeadBytes = 115200ull;        // 600 ms（泵模式）
inline constexpr std::uint64_t kThreadLeadBytes = 19200ull;   // 100 ms（真 feeder 线程）

// ★ S0 的配套：thread 档 / 泵档各自的预读字节数。`mixStep()` 与 `begin()` 日志行**只调它**。
constexpr std::uint64_t leadBytesFor(bool thread_mode) {
    return thread_mode ? kThreadLeadBytes : kLeadBytes;
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
