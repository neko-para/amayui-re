#include "runtime/audio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>

#include "runtime/clock.h"
#include "runtime/critical_sections.h"
#include "runtime/decode.h"
#include "runtime/exec.h"
#include "runtime/guest_call.h"
#include "runtime/guest_token.h"
#include "runtime/log.h"
#include "runtime/memory.h"
#include "runtime/native_overrides.h"
#include "runtime/shims.h"
#include "runtime/sync_guest.h"

namespace lift {
// ★ 只读计量（**定义在 `shims.cpp`**，只给 `LIFT_AUDIO_TRACE` 的 fill 时间分解用）：
//   本线程在 **shim 边界**（`ShimRegistry::invoke` 的内层取令牌）上"真的被挡住"的累计墙钟（ns）
//   与次数。`thread_local` ⇒ 只有**调用它自己线程**的那份会被累加/读出 —— feeder 的 fill
//   收尾在**同一条线程**上读它，所以不会混进主线程的等待。
//   ⛔ 不是契约、不参与任何判定；口径见 shims.cpp 里同名定义上的注释。
extern thread_local std::uint64_t t_shim_token_wait_ns;
extern thread_local std::uint64_t t_shim_token_wait_count;
namespace {
constexpr std::uint32_t kSampleRate = 48000u;
constexpr std::uint32_t kBlockAlign = 4u;  // 16 bit / 2 ch
constexpr std::uint32_t kBytesPerSec = kSampleRate * kBlockAlign;  // 192000
// 设备时钟的单位换算：VirtualClock 每 10,000,000 tick = 1 s。
constexpr std::uint64_t kTicksPerSec = 10'000'000ull;

// ★ 预读 lead：D 在一次 fill 期间**暂停混音**（单执行流，fill 写 guest 环时不能同时读它）。
//   实测（RelWithDebInfo，48k 立体声 BGM、`--native-overrides all`）：一次 fill ≈ 360 ms 墙钟。
//   fill 之后到下一个通知点还有 500ms - 360ms = 140ms，所以 lead 必须 > 500ms 才能保证
//   "fill 开始时队列里的音频撑得过整个 fill"；这里取 600ms（≈100ms 余量）。
//   ⛔ 这不是设备延迟，是 D 的吞吐代价；代价是 SE 也有 ~600ms 延迟。
constexpr std::uint64_t kLeadBytes = (kBytesPerSec * 600ull) / 1000ull;  // 115200 B（泵模式）
// 真线程模式：feeder 与 mixer 都在自己的线程，主线程卡在 Present/长 shim 不再断音频。
// 独立 mixer 实测 fill ≈ 400 ms 也不影响设备队列；100 ms 足以覆盖 5 ms mixer 周期 + 抖动。
constexpr std::uint64_t kThreadLeadBytes = (kBytesPerSec * 100ull) / 1000ull;  // 19200 B

// ★ S1：`stop()` / `setPosition()` 等"正在跑的那一刀 fill"的上限。实测单刀 fill 380–460 ms
//   （RelWithDebInfo，48k 立体声 BGM），3 s 留足一个数量级余量；超时只 Warn + 计数，
//   ⛔ 不把主线程挂死。
constexpr std::uint32_t kFillWaitTimeoutMs = 3000;

// 引擎写死的线程体入口（`.lst` 0x004B5AA0；见 docs/thread-model-feasibility.md §2.1）。
constexpr std::uint32_t kFeederStartEa = 0x004B5AA0u;
constexpr std::uint32_t kSoundSlots = 0x654u;       // [Sound+0x654+4*s] = SoundBuffer*
constexpr std::uint32_t kSoundCsBase = 0x690u;      // [Sound+0x690+24*s] = CRITICAL_SECTION
constexpr std::uint32_t kSoundInflight = 0x7F8u;    // [Sound+0x7F8+4*s] = 在飞计数
constexpr std::uint32_t kSoundQuitEvent = 0x514u;   // 事件 #60
constexpr std::uint32_t kSlotCount = 15u;
constexpr std::uint32_t kStreamDsBuffer = 0x40Cu;   // SoundBuffer +0x40C = IDirectSoundBuffer8
constexpr std::uint32_t kStreamBytes = 0x444u;      // SoundBuffer +0x444 = 环长度 L
constexpr std::uint32_t kSub4B6C30 = 0x004B6C30u;
// feeder 真线程的"返回哨兵"：sub_4B6C30 的 `retn 4` 会弹到这里，循环随即结束。
// 与 GuestCallStack 的 0x7FFEF008 / winmm 的 0x7FFEF004 错开。
constexpr std::uint32_t kFeederReturnEa = 0x7FFEF00Cu;

std::int32_t gainQ16(double amp) {
    if (!(amp > 0.0)) return 0;
    const double q = amp * 65536.0;
    if (q >= 2147483000.0) return 2147483000;
    return static_cast<std::int32_t>(q + 0.5);
}

void putLe16(std::uint8_t* p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}
void putLe32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v & 0xFF);
    p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    p[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

// ★ 只读诊断（`LIFT_AUDIO_TRACE` 专用）：读 guest 的一个 4 字节字段，**没提交 / 读不到就打 `?`**。
//   ⛔ 不要用 0 冒充"读不到" —— 那会把"没提交"读成"值是 0"，两条完全不同的结论就混了。
//   ⛔ 纯读：`committed` + `rd32`，不写内存、不改任何状态。
void traceDword(const GuestMemory* mem, std::uint32_t ea, char* out, std::size_t n) {
    if (mem != nullptr && mem->committed(ea, 4)) {
        std::snprintf(out, n, "0x%08X", mem->rd32(ea));
    } else if (n > 0) {
        std::snprintf(out, n, "?");
    }
}
}  // namespace

const char* audioModeName(AudioMode m) {
    switch (m) {
        case AudioMode::Off: return "off";
        case AudioMode::Virtual: return "virtual";
        case AudioMode::Sdl: return "sdl";
    }
    return "?";
}

AudioEngine::AudioEngine() = default;

AudioEngine::~AudioEngine() { end(); }

void AudioEngine::attach(X86* cpu, GuestMemory* mem, VirtualClock* clock) {
    cpu_ = cpu;
    mem_ = mem;
    clock_ = clock;
}

void AudioEngine::logAt(lift::LogLevel level, const char* fmt, ...) const {
    va_list ap;
    va_start(ap, fmt);
    lift::logV(level, LogDsound, "[audio] ", fmt, ap);
    va_end(ap);
}

bool AudioEngine::configure(AudioMode mode, const std::string& dump_path, bool sound, std::string* err) {
    if (begun_) {
        if (err) *err = "音频已 begin，不能再改配置";
        return false;
    }
    configured_mode_ = mode;
    dump_path_ = dump_path;
    // ★ 缺省静音：`--sound` 才出声。它与 `--audio`（后端选择）正交：设备 / 混音 / 喂料 /
    //   guest 的 DS 调用全都照跑，只在交付点覆盖缓冲（见 `generateTo`）。
    mute_ = !sound;
    // `--audio-dump` 是机械判据入口：即使没给 `--audio`，也要有虚拟设备时钟才能产出 PCM。
    if (configured_mode_ == AudioMode::Off && !dump_path_.empty()) configured_mode_ = AudioMode::Virtual;
    return true;
}

bool AudioEngine::begin(std::string* err) {
    if (begun_) return true;
    mode_ = configured_mode_;
    sdl_fallback_ = false;
    sdl_device_opened_ = false;
    if (mode_ == AudioMode::Off) {
        begun_ = true;
        return true;
    }
    // ★ 音频与"同一个 Machine 跑第二轮"不兼容：guest 的 DS 对象 / Sound 槽 / 喂料线程身份
    //   还活着，而宿主时间线要从 0 重来。⛔ 不要静默清空设备模型（那会让第二轮完全没声）；
    //   明确拒绝，调用方新建 Machine。
    if (feeder_.registered || !buffers_.empty()) {
        if (err) *err = "音频不支持同一 Machine 的第二次 run（guest DS 对象仍在；请新建 Machine）";
        return false;
    }
    // 同一台 Machine 可以 run 多次：上一轮结束后把时间线 / 缓冲 / 喂料身份清掉。
    // ⛔ 不清 `configured_mode_` / `dump_path_`（那是配置）。
    buffers_.clear();
    pending_.clear();
    feeder_ = Feeder{};
    played_ = generated_ = pushed_ = queued_ = 0;
    stats_ = AudioStats{};
    in_fill_ = false;
    borrowed_ret_ = false;
    // ★ 防残留：`filling_obj_` 只在 `feederEvent` 收尾清 0；`end()` 走 `joinFeeder()` 时 fill 会
    //   自己收尾，所以这**今天**不是 bug —— 但 `removeBuffer` / `addBuffer` 新加的等待判据
    //   直接以它为条件，留下一个非 0 的残值会让下一轮的重置路径等一个不存在的 fill
    //   （最坏每次吃满 3 s）。同一 Machine 的第二次 run 在这里已被上面拒掉，所以清它无害。
    filling_obj_.store(0);
    if (!cpu_ || !mem_ || !clock_) {
        if (err) *err = "AudioEngine 没有 attach（cpu/mem/clock）";
        return false;
    }
    if (!dump_path_.empty() && !openDump()) {
        logAt(LogLevel::Warn, "dump 打不开：%s（继续，只是没有 WAV）", dump_path_.c_str());
    }
    if (mode_ == AudioMode::Sdl) {
        if (!openSdl()) {
            logAt(LogLevel::Warn, "SDL3 音频设备打不开（%s）⇒ 退回 virtual（不出声，仍可 dump）",
                  SDL_GetError());
            mode_ = AudioMode::Virtual;
            sdl_fallback_ = true;
        } else {
            sdl_device_opened_ = true;
        }
    }
    // ★ 真线程只在"真设备 + 宿主时钟"下启用：虚拟时钟的 tick 是主线程的确定性账本，
    //   不能由第二条 guest 执行流并发推进。窗口模式本来就会 `--real-time`。
    thread_mode_ = (mode_ == AudioMode::Sdl && clock_ && clock_->realTime());
    begun_ = true;
    if (thread_mode_) startMixThread();
    if (mode_ != AudioMode::Off) {
        logAt(LogLevel::Info,
              "mode=%s %u Hz / 16 bit / 2 ch（lead=%llu B）；游标=%s；feeder=%s；"
              "muted=%d%s；PCM 由 guest 解码器产生，宿主只做混音与输出",
              audioModeName(mode_), kSampleRate,
              static_cast<unsigned long long>(thread_mode_ ? kThreadLeadBytes : kLeadBytes),
              mode_ == AudioMode::Sdl ? "SDL 设备已消费字节" : "VirtualClock",
              thread_mode_ ? "host-thread（方案 C）" : "主线程泵（方案 D）", mute_ ? 1 : 0,
              mute_ ? "（缺省；--sound 可听）" : "（--sound）");
    }
    return true;
}

bool AudioEngine::openSdl() {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        logAt(LogLevel::Warn, "SDL_InitSubSystem(AUDIO) 失败：%s", SDL_GetError());
        return false;
    }
    sdl_inited_ = true;
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_S16LE;
    spec.channels = 2;
    spec.freq = static_cast<int>(kSampleRate);
    SDL_AudioStream* stream =
        SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream == nullptr) {
        logAt(LogLevel::Warn, "SDL_OpenAudioDeviceStream 失败：%s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sdl_inited_ = false;
        return false;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        logAt(LogLevel::Warn, "SDL_ResumeAudioStreamDevice 失败：%s", SDL_GetError());
        SDL_DestroyAudioStream(stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sdl_inited_ = false;
        return false;
    }
    stream_ = stream;
    sdl_ok_ = true;
    const char* name = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    logAt(LogLevel::Info, "SDL 音频设备：%s", name ? name : "（默认设备，名字不可用）");
    return true;
}

void AudioEngine::closeSdl() {
    if (stream_) {
        SDL_DestroyAudioStream(static_cast<SDL_AudioStream*>(stream_));
        stream_ = nullptr;
    }
    sdl_ok_ = false;
    if (sdl_inited_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sdl_inited_ = false;
    }
}

bool AudioEngine::openDump() {
    dump_ = std::fopen(dump_path_.c_str(), "wb");
    if (!dump_ ) return false;
    std::uint8_t hdr[44] = {};
    std::memcpy(hdr + 0, "RIFF", 4);
    putLe32(hdr + 4, 36u);  // 回填
    std::memcpy(hdr + 8, "WAVEfmt ", 8);
    putLe32(hdr + 16, 16u);
    putLe16(hdr + 20, 1u);  // PCM
    putLe16(hdr + 22, 2u);  // channels
    putLe32(hdr + 24, kSampleRate);
    putLe32(hdr + 28, kBytesPerSec);
    putLe16(hdr + 32, static_cast<std::uint16_t>(kBlockAlign));
    putLe16(hdr + 34, 16u);
    std::memcpy(hdr + 36, "data", 4);
    putLe32(hdr + 40, 0u);  // 回填
    dump_ok_ = std::fwrite(hdr, 1, sizeof(hdr), dump_) == sizeof(hdr);
    dump_bytes_ = 0;
    if (!dump_ok_) {
        std::fclose(dump_);
        dump_ = nullptr;
    }
    return dump_ok_;
}

void AudioEngine::closeDump() {
    if (!dump_) return;
    const std::uint64_t data = dump_bytes_;
    std::uint8_t patch[4];
    std::fseek(dump_, 4, SEEK_SET);
    putLe32(patch, static_cast<std::uint32_t>(36ull + data));
    std::fwrite(patch, 1, 4, dump_);
    std::fseek(dump_, 40, SEEK_SET);
    putLe32(patch, static_cast<std::uint32_t>(data));
    std::fwrite(patch, 1, 4, dump_);
    std::fclose(dump_);
    dump_ = nullptr;
    dump_ok_ = false;
}

void AudioEngine::writeDump(const std::uint8_t* pcm, std::size_t n) {
    if (!dump_ || !dump_ok_ || n == 0) return;
    if (std::fwrite(pcm, 1, n, dump_) != n) {
        logAt(LogLevel::Warn, "dump 写失败（已写 %llu B）⇒ 停止 dump",
              static_cast<unsigned long long>(dump_bytes_));
        dump_ok_ = false;
        closeDump();
        return;
    }
    dump_bytes_ += n;
}

void AudioEngine::abortFill() {
    if (!in_fill_ && !borrowed_ret_) return;
    if (!cpu_ || !mem_) {
        in_fill_ = false;
        borrowed_ret_ = false;
        return;
    }
    logAt(LogLevel::Warn, "run 在音频 fill 中结束（time-limit / max-insns / 停止）⇒ 回滚 [esp] / CS / 在飞计数");
    if (borrowed_ret_) {
        if (mem_->committed(borrowed_esp_, 4)) mem_->wr32(borrowed_esp_, saved_ret_);
        borrowed_ret_ = false;
    }
    if (in_fill_) {
        if (mem_->committed(fill_inf_ea_, 4)) {
            const std::uint32_t cur = mem_->rd32(fill_inf_ea_);
            if (cur > 0) mem_->wr32(fill_inf_ea_, cur - 1u);
        }
        // fill 可能自己又递归进同一个 CS（例如 sub_4B6B60 的 stop 路径）；回滚时把以 feeder
        // 身份持有的层数全部退掉（最多 8 层），否则 owner 会留在 guest 结构里。
        for (int i = 0; i < 8 && fill_cs_ != 0; ++i) {
            if (guestCriticalSectionOwner(*cpu_, fill_cs_) != feeder_.thread_id) break;
            std::string leave_err;
            if (!guestLeaveCriticalSectionAs(*cpu_, fill_cs_, feeder_.thread_id, &leave_err)) {
                std::fprintf(stderr, "[audio] abortFill：离开 CS 失败：%s\n", leave_err.c_str());
                break;
            }
        }
        std::uint32_t ignored = 0;
        restore(snapshot_, &ignored);
        if (cpu_->calls) cpu_->calls->reset();
        in_fill_ = false;
    }
    feeder_.exited = true;
    feeder_.runnable = false;
    feeder_.exit_requested = false;
    pending_.clear();
}

void AudioEngine::end() {
    if (!begun_ && !dump_ && !stream_) return;
    stopMixThread();
    if (thread_mode_ && feeder_thread_.joinable()) joinFeeder();
    abortFill();
    if (feeder_.registered && !feeder_.exited) {
        logAt(LogLevel::Info, "运行结束：喂料泵仍在运行（fills=%llu）⇒ 直接关闭（不再写 guest）",
              static_cast<unsigned long long>(stats_.fills));
    }
    closeSdl();
    closeDump();
    if (mode_ != AudioMode::Off) {
        logAt(LogLevel::Info,
              "统计：played=%llu mixed=%llu pushed=%llu notify=%llu fills=%llu deferred=%llu "
              "dropped=%llu underruns=%llu",
              static_cast<unsigned long long>(stats_.played_bytes),
              static_cast<unsigned long long>(stats_.mixed_bytes),
              static_cast<unsigned long long>(stats_.pushed_bytes),
              static_cast<unsigned long long>(stats_.notifications),
              static_cast<unsigned long long>(stats_.fills),
              static_cast<unsigned long long>(stats_.deferred),
              static_cast<unsigned long long>(stats_.dropped),
              static_cast<unsigned long long>(stats_.underruns));
        if (std::getenv("LIFT_AUDIO_TRACE") != nullptr) {
            for (const auto& b : buffers_) {
                logAt(LogLevel::Info,
                      "buffer state=0x%08X obj=0x%08X bytes=%u %uch/%uHz/%ubit playing=%d notify=%u "
                      "start=%llu initial=%u vol=%d pan=%d",
                      b.state_ea, b.obj_ea, b.bytes, b.fmt.channels, b.fmt.sample_rate, b.fmt.bits,
                      b.playing ? 1 : 0, b.notify_count, static_cast<unsigned long long>(b.start_mixed),
                      b.initial_pos, b.volume, b.pan);
            }
        }
    }
    begun_ = false;
}

AudioEngine::Buffer* AudioEngine::find(std::uint32_t state_ea) {
    for (auto& b : buffers_)
        if (b.state_ea == state_ea) return &b;
    return nullptr;
}

const AudioEngine::Buffer* AudioEngine::find(std::uint32_t state_ea) const {
    for (const auto& b : buffers_)
        if (b.state_ea == state_ea) return &b;
    return nullptr;
}

// ★ 走 `mutex_`（递归）：调用方可能已持它（`pump` 的 D 分支 / `fireCrossings` 的调用链），
//   也可能没持（`feederEvent` 在令牌里）。锁序是 **令牌 → `mutex_`**（每个 DS shim 都是这个
//   顺序）⇒ feeder 持令牌时取 `mutex_` 是安全的；⛔ 反过来"持 `mutex_` 再等令牌"只有 D 模式的
//   `pump()` 会做，而 D 模式没有 feeder 线程。
//   返回指针只在 `mutex_` 内有效：调用方必须在同一次持锁里读完 `generation`（`buffers_` 是
//   vector ⇒ 别的线程 `addBuffer` / `removeBuffer` 会让它失效）。
AudioEngine::Buffer* AudioEngine::findByObj(std::uint32_t obj_ea) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (obj_ea == 0) return nullptr;
    for (auto& b : buffers_)
        if (b.obj_ea == obj_ea) return &b;
    return nullptr;
}

bool AudioEngine::isFeederThread() const {
    // ⛔ 只作"是不是 feeder 自己"的判断（EOF 路径 `0x4B6C58` 在 fill 里调 stop ⇒ 自等会死锁）。
    //   ★ 用 feeder 线程**自己**登记的 id，而不是 `feeder_thread_.joinable()/get_id()`：
    //     `joinFeeder()` 会在主线程 join 同一个 `std::thread` 对象，而 feeder 此刻可能正跑
    //     fill 并在其中调 `stop()` ⇒ 并发读 `std::thread` 是数据竞争。id 是 atomic 的、只读一次。
    if (!thread_mode_) return false;
    const std::thread::id id = feeder_tid_.load();
    return id != std::thread::id{} && id == std::this_thread::get_id();
}

void AudioEngine::setFillInFlightForTest(std::uint32_t obj_ea) {
    // ★ 仅夹具（见 audio.h 的声明）：伪造"某一刀 fill 正在跑"。
    //   在 `pending_mutex_` 下 store + notify_all —— 与 `feederEvent` 收尾清 0 同一纪律
    //   （等的人用 `pending_cv_` 配这把锁 ⇒ 不会丢唤醒）。
    //   ⛔ 不碰 `mutex_` / 令牌 / 任何生产状态；⛔ 不进生产路径。
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        filling_obj_.store(obj_ea);
    }
    pending_cv_.notify_all();
}

bool AudioEngine::waitFillDone(std::uint32_t obj_ea, std::uint32_t timeout_ms, bool* waited) {
    // ⛔ 不用 `mutex_`：等待期间必须放开它（fill 收尾要它）。
    //   `filling_obj_` 是 atomic，置/清都在别处；这里只作谓词。
    //   清 0 的那一侧在 `pending_mutex_` 下 store 并 notify_all ⇒ 没有丢唤醒。
    std::unique_lock<std::mutex> lk(pending_mutex_);
    if (waited) *waited = false;
    if (filling_obj_.load() != obj_ea) return true;
    if (waited) *waited = true;
    return pending_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                [this, obj_ea] { return filling_obj_.load() != obj_ea; });
}

bool AudioEngine::waitFill(std::uint32_t obj_ea, std::unique_lock<std::recursive_mutex>& lock) {
    // ★ 死锁判据（S1）：`filling_obj_ == obj` 意味着那一刀 fill **正持有槽 CS**（`feederEvent`
    //   在拿到 CS、重验完槽之后才置标志，`++在飞`/`runFeederGuest` 紧跟其后）⇒ 主线程此刻
    //   **不可能**持有那把 CS ⇒ 等它跑完不会与"引擎在同一把 CS 里递归进 Stop"互相等。
    //   反过来，"已 pop 但还在 CS 上打转 / 还没进 CS"的那一段**不置**这个标志 —— 那一段由
    //   代次校验处理（见 `feederEvent`），所以这里等不到它。
    //   ⛔ 等待期间**不持 `mutex_`、不持令牌**：fill 的每个 shim 都要令牌，收尾还要 `mutex_`。
    lock.unlock();
    bool waited = false;
    bool done = true;
    // ★ 只读读数：时钟夹住**谓词等待本身**（`t1` 取在 `lock.lock()` **之前**）。
    //   为什么：`fill_wait_ms` 的语义是"阻塞在那一刀 fill 上"，与 `fill_max_ms` 同一把尺；
    //   重取 `mutex_` 还要等混音器的 `mixStep`（跨整个 `generateTo`）⇒ 算进去会把两个量搅成
    //   一个，A/B 就看不出补偿的收益。代价：**不含重取锁的时间**，⛔ 不要当"调用方总卡顿"读。
    const auto t0 = std::chrono::steady_clock::now();
    {
        GuestToken::YieldGuard yield(guestToken());
        done = waitFillDone(obj_ea, kFillWaitTimeoutMs, &waited);
    }
    const auto t1 = std::chrono::steady_clock::now();
    lock.lock();
    if (waited) {
        ++stats_.fill_waits;
        // ★ 只读诊断（`fill_wait_ms` / `fill_wait_max_ms` 的唯一累计点）：与 `fill_waits`
        //   同一个集合 ⇒ 前者可除以后者。⛔ 不参与任何判定，读数是读数。
        const std::uint64_t wait_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
        stats_.fill_wait_us += wait_us;
        stats_.fill_wait_max_us = std::max(stats_.fill_wait_max_us, wait_us);
    }
    if (!done) {
        ++stats_.fill_wait_timeouts;
        logAt(LogLevel::Warn,
              "等“正在跑的 fill”超时 %u ms（obj=0x%08X）⇒ 照常返回：这一刀可能与本次重定位并发"
              "（见 docs/audio-implementation.md §5 第 16 条）",
              kFillWaitTimeoutMs, obj_ea);
    }
    return done;
}

// ★★ M0（REQ-01M75WWFGT8ZEBGEPENQHSYTB8）：**等 `mutex_` 不需要令牌 ⇒ 先让出令牌再等锁**。
//   为什么现在要放：混音器（`mixLoop`）**跨整个 `mixStep`** 持 `mutex_`，而 mixer **从不取令牌**
//   ⇒ 主线程若在令牌里等这把锁，就把"一次混音"（+ `generateTo`）算进了自己的持有时间，音频
//   feeder 的每次交接都要陪着等。
//   ⇒ 下面每个 DS 入口的第一句都是 `YieldGuard`（让出令牌），**然后**才取 `mutex_`；返回时
//     `lock` 先析构（放 `mutex_`）、`yield` 后析构（取回令牌）⇒ **锁序仍是 令牌 → `mutex_`**，
//     ⛔ 不会出现"持 `mutex_` 等令牌"的反向等待。
//   为什么放令牌是安全的（三句）：
//     ① 让出时**我们还没拿到 `mutex_`**，也不持有任何 audio 容器的引用（`state_ea` 是 guest 地址，
//        `Buffer*` 一律在拿到 `mutex_` **之后**才 `find`）⇒ 手上没有"别人会改的引用"。
//     ② 函数体碰的一切（`buffers_` / `pending_` / `stats_` / `generated_` / 句柄外状态）都由
//        AudioEngine 自己的锁保护（`mutex_` / `pending_mutex_`）⇒ 与 feeder / 混音器照旧互斥；
//        guest 内存是**数据面**，从不由令牌保护（guest_token.h 的星号注）。
//     ③ 令牌只在 `mutex_` 放掉之后才取回，取回后函数即返回 ⇒ 不存在"沿用放令牌之前的查表结论"。
//   ⛔ 不要给 `playedCursor` / `fillLagQuarter` 加 YieldGuard：它们会被**已持 `mutex_`** 的调用链
//     调到（`stop` / `pump`），那时让出令牌 = 持 `mutex_` 等令牌（反向锁序）。
void AudioEngine::addBuffer(std::uint32_t state_ea, std::uint32_t obj_ea, std::uint32_t data_ea,
                            std::uint32_t bytes, std::uint32_t flags, const AudioFormat& fmt) {
    GuestToken::YieldGuard yield(guestToken());  // ★ M0（总口径见本函数上方那条注释）
    // ★ 需要 `unique_lock`：同一 state 换 obj 时要等"正在跑的那一刀 fill"（见 `waitFill`）。
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    if (!enabled() || state_ea == 0) return;
    Buffer* b = find(state_ea);
    if (b != nullptr && b->obj_ea != obj_ea) {
        // ★★ 同一 `state_ea` 换成另一条 obj（防御性：今天的 dsound 路径每次新分配 `st`，几乎
        //   走不到；但堆复用 + 旧条目未删时会走到）。**先把旧代作废、再决定等不等**：
        //   与 `stop()` 同序（阻断新 crossing → `++generation` → `dropPending(旧 obj)`）。
        //   ⛔ 为什么必须显式 `dropPending(old_obj)`：下面原有的 `dropPending(obj_ea)` 丢的是
        //   **新** obj，旧 obj 的排队条目本来只能靠代次懒丢 ⇒ 那一刀会继续往旧环写。
        const std::uint32_t old_obj = b->obj_ea;
        b->playing = false;
        b->looping = false;
        ++b->generation;
        dropPending(old_obj);
        // ★ 自等判据：feeder 自己（fill 内 Release / EOF 自停）⇒ 不等，直接往下写。
        //   ⛔ 不要加 `thread_mode_ &&`（理由见 `removeBuffer` 同一条注记）。
        if (!isFeederThread() && filling_obj_.load() == old_obj) {
            waitFill(old_obj, lock);
            // ★★ `waitFill` 放开了 `mutex_` ⇒ `buffers_`（vector）可能已被 erase / 重排，
            //   `b` 指针必然失效 ⇒ 必须重查（`removeBuffer` 用"先 erase 后等"绕开了这一步）。
            b = find(state_ea);
        }
    }
    // ⛔ 新 state（没有旧 obj）不要为了"对称"去等一个不存在的 obj。
    if (b == nullptr) {
        buffers_.push_back(Buffer{});
        b = &buffers_.back();
        b->state_ea = state_ea;
    }
    b->obj_ea = obj_ea;
    b->data_ea = data_ea;
    b->bytes = bytes;
    b->flags = flags;
    b->fmt = fmt;
    if (b->fmt.block_align == 0) b->fmt.block_align = (b->fmt.bits / 8u) * b->fmt.channels;
    if (b->fmt.block_align == 0) b->fmt.block_align = 4;
    if (b->fmt.avg_bytes == 0) b->fmt.avg_bytes = b->fmt.sample_rate * b->fmt.block_align;
    b->playing = false;
    b->looping = false;
    // 新缓冲（或同一 state 重新建）⇒ 旧代次的通知一律作废："每次换代都清"。
    ++b->generation;
    dropPending(obj_ea);
    b->initial_pos = 0;
    b->start_mixed = generated_;
    b->notify_count = 0;
    b->volume = 0;
    b->pan = 0;
    stats_.max_buffers = std::max(stats_.max_buffers, static_cast<std::uint32_t>(buffers_.size()));
}

void AudioEngine::setFormat(std::uint32_t state_ea, const AudioFormat& fmt) {
    GuestToken::YieldGuard yield(guestToken());  // ★ M0（总口径见 `addBuffer` 上方）
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    if (b) b->fmt = fmt;
}

void AudioEngine::removeBuffer(std::uint32_t state_ea) {
    GuestToken::YieldGuard yield(guestToken());  // ★ M0（总口径见 `addBuffer` 上方）
    // ★ 需要 `unique_lock`：要等"正在跑的那一刀 fill"（见 `waitFill`）。
    //   这条等待挡在 `destroyBufferBlocks`（`shims/dsound.cpp`）的 `heap->free` **之前** ——
    //   它先 `removeBuffer` 再 free guest 环与状态块，而设备模型读的正是 `b.data_ea`
    //   ⇒ 不等就会让在飞那一刀往已 free 的 guest 堆块里写。
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    for (auto it = buffers_.begin(); it != buffers_.end(); ++it) {
        if (it->state_ea != state_ea) continue;
        const std::uint32_t obj = it->obj_ea;
        // ★ 顺序：**先 erase、再 dropPending、最后才等**（与 `stop()` 的"先等后重查"相反）。
        //   理由：erase 之后 `fireCrossings` 不再为它入队、fill 的代次校验也找不到它
        //   （`findByObj` → null ⇒ `stale`）⇒ 这次等待只可能被**当前这一刀**唤醒一次，
        //   不会被"下一刀"延长；也**不需要** `stop()` 那套 `find()` + obj 身份重校验
        //   （这个 state 已经删了，等完直接返回）。
        //   代价：金丝雀 `fill_overlapped` 仍会 +1 —— 那正是文档已列的良性成因 ②。
        buffers_.erase(it);
        // ★ D2：`pending_` 归 `pending_mutex_` 管；这里以前是唯一一处**在 `mutex_` 下裸动 deque**
        //   的地方（宿主 UB：feeder / `enqueue` 都在 `pending_mutex_` 下动它）。
        //   ⚠ **例外**：D 模式（`--audio virtual` / 无 real-time）没有 feeder 线程、也没有 mixer
        //   线程 ⇒ `pump()` 是单执行流，**有意**不持 `pending_mutex_` 直接操作 `pending_`
        //   （不能跨 fill 持锁：混音器每次 `enqueue` 都会被 fill 的 380–460 ms 堵住）。
        //   ⛔ 不要在 thread 模式里加任何无锁 `pending_` 路径。
        dropPending(obj);
        // ★ 自等判据（与 `stop()` 的 :598 同文）：feeder **自己**调进来（fill 内的 DS `Release` /
        //   EOF 自停 `0x4B6C58`）必须早退，否则它等的是自己 —— 不是永久挂死（`waitFillDone`
        //   上限 = `kFillWaitTimeoutMs`），但那 3 s 会每刀都吃一次。
        //   ⛔ 判据只写 `!isFeederThread()`，**不要**再加 `thread_mode_ &&`：`isFeederThread()`
        //   自己已经含 `!thread_mode_ ⇒ false`；而 D 模式下 `filling_obj_` 恒 0，加门零收益，
        //   却会让夹具（纯档只有 Virtual）永远走不进这个分支 ⇒ 守卫恒绿。
        if (!isFeederThread() && filling_obj_.load() == obj) waitFill(obj, lock);
        // 等完直接返回：这个 state 已经从模型里删了，没有"重查 / 重建"的必要。
        return;
    }
}

void AudioEngine::setNotify(std::uint32_t state_ea, std::uint32_t count, const std::uint32_t* offsets) {
    GuestToken::YieldGuard yield(guestToken());  // ★ M0（总口径见 `addBuffer` 上方）
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    if (b == nullptr) return;
    b->notify_count = std::min<std::uint32_t>(count, 4u);
    for (std::uint32_t i = 0; i < b->notify_count; ++i) b->notify_off[i] = offsets ? offsets[i] : 0u;
    for (std::uint32_t i = b->notify_count; i < 4u; ++i) b->notify_off[i] = 0u;
}

void AudioEngine::clearNotify(std::uint32_t state_ea) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    if (b) b->notify_count = 0;
}

void AudioEngine::play(std::uint32_t state_ea, bool looping) {
    GuestToken::YieldGuard yield(guestToken());  // ★ M0（总口径见 `addBuffer` 上方）
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    if (b == nullptr) return;
    // ★ D3（API 语义）：真 DS 对**已经在播**的缓冲再 `Play` 返回 **DS_OK**、继续播、只让最后一次
    //   的 flags 生效，且**不重置游标、不重发 offset 0 通知**（MS 文档 `IDirectSoundBuffer8::Play`
    //   的 "Playing a Buffer That Is Already Playing" 一节；见 docs/audio-implementation.md §5）。
    //   ⇒ 这里 must no-op（仍更新 `looping`）。
    //   ⛔ 不要改成返回 S_FALSE：那会让引擎的 `arg==0 → sub_4B6190` 分支以为"没在播"而重锚。
    if (b->playing) {
        b->looping = looping;
        ++stats_.play_on_playing;
        return;
    }
    b->playing = true;
    b->looping = looping;
    b->start_mixed = generated_;
    if (std::getenv("LIFT_AUDIO_TRACE") != nullptr) {
        logAt(LogLevel::Info, "play state=0x%08X obj=0x%08X bytes=%u %uch/%uHz/%ubit loop=%d",
              state_ea, b->obj_ea, b->bytes, b->fmt.channels, b->fmt.sample_rate, b->fmt.bits,
              looping ? 1 : 0);
    }
    // ★ 真 DS：Play 时游标从 `initial_pos` 开始；若某个通知点正好等于它，该点立即满足。
    //   （引擎只 SetCurrentPosition(0)，所以通常只命中 offset 0。）
    for (std::uint32_t i = 0; i < b->notify_count; ++i) {
        if (b->notify_off[i] == b->initial_pos) enqueue(b->obj_ea, i, b->generation);
    }
}

void AudioEngine::stop(std::uint32_t state_ea) {
    GuestToken::YieldGuard yield(guestToken());  // ★ M0（总口径见 `addBuffer` 上方）
    // ★ 需要 `unique_lock`：等"正在跑的那一刀 fill"时要放开 `mutex_`（见 `waitFill`）。
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    if (b == nullptr) return;
    // 真 DS：Stop 冻结当前播放位置；再 Play 从冻结处继续（不是回到 initial_pos）。
    // ★ 冻结值必须在 `playing=false` **之前**取：真机 Stop 冻结的就是这一刻的设备游标。
    const std::uint32_t frozen = playedCursor(state_ea);
    b->playing = false;  // 先阻断新的 crossing（`fireCrossings` 只看 playing 的缓冲）
    b->looping = false;
    // ★★ D1（见 docs/audio-implementation.md §5 第 16 条）：引擎**不在 Stop 里排空位置通知**，
    //   而它在 `sub_4B6B60`（Stop）之后才做重定位 / 预填 / Play —— 那段**可能不在槽 CS 里**。
    //   `jns` 的语义是"这次 `--` 之后计数 ≥ 0 就**跳过** EnterCriticalSection"：
    //     · 空闲（0）进来 ⇒ 新值 −1 ⇒ 不跳 ⇒ **进 CS**（所以正常起播整段持 CS）；
    //     · fill 进行中（计数 1）：
    //        - 经 `sub_4B73E0` 起播：它自己先 `--`（1→0，跳过它自己的 CS），随后
    //          `sub_4B6B60` 的 `--`（0→−1）⇒ 不跳 ⇒ **进 CS 并阻塞到 fill 结束** ⇒ 串行化 ✓
    //        - **直接**调 `sub_4B6B60`（`sub_4B60C0` 的 dsStop、`sub_418520` 的 P2）：它自己的
    //          `--`（1→0）⇒ **跳过 CS** ⇒ Stop 与 fill 并发 ✗
    //   （跳过 CS 只对**同线程递归**是正确的；另一条线程在跑 fill 时跳过去就是真并发。）
    //   真机上"越过通知点"的那一刀 fill 在 µs/ms 级就被喂料线程消费掉了，这个窗口没人注意；
    //   emulator 单刀 fill 380–460 ms 把它放大成常态 ⇒ 一条**已经越过通知点**的通知会跨过 Stop
    //   被投递，或一条 fill 与 Stop/重定位并发地把旧相位写回环。补偿分三层（恢复真机因果次序）：
    //     ① `dropPending(obj)`：**排队**（还没被 pop）的直接丢；
    //     ② `++generation`：**已 pop、还没跑**的在 `feederEvent` 自查代次后丢（关掉"pop 之后、
    //        拿 CS 之前"的窗口）；
    //     ③ `filling_obj_ == obj`：**正在跑**的那一刀有界等它跑完（真机次序：Stop 在 fill 之后
    //        生效）。死锁判据见 `waitFill` 的注释。
    //   ⛔ 不要改成"等 feeder 把队列排空"：feeder 要那把槽 CS，而 P1 的 Stop 正持着它 ⇒ 必然超时。
    ++b->generation;
    const std::uint32_t obj = b->obj_ea;
    dropPending(obj);
    if (thread_mode_ && !isFeederThread() && filling_obj_.load() == obj) {
        // feeder 自己调（EOF 路径 `0x4B6C58`）必须跳过，否则自等。
        waitFill(obj, lock);
        // ★★ Yield 之后**必须重新查表**：等待期间别的线程可能已经 erase / 重排 `buffers_`
        //    （它是 vector，旧指针已失效）。
        b = find(state_ea);
        if (b == nullptr) return;
        // ★ 重校验**身份**（`obj` 见上）：`state_ea` 可能已被复用给**另一条 obj**
        //    （`removeBuffer` + `addBuffer` 同一 state）⇒ 不重校验就会把**旧 obj 的** frozen
        //    游标写进新 Buffer。⛔ 不要在这里"重做一遍 stop"（那是另一件事）。
        if (b->obj_ea != obj) return;
    }
    // 冻结值是 Stop 这一刻的游标：之后设备继续走（以及被作废的那一刀 fill）都不影响它。
    b->initial_pos = b->bytes ? (frozen % b->bytes) : 0u;
    if (std::getenv("LIFT_AUDIO_TRACE") != nullptr) {
        // ★ 只读诊断：除原有的 state/obj/cursor/gen 再补两栏 ——
        //   `feeder=%d` = **本调用是不是跑在 feeder 线程上**（`isFeederThread()`；EOF 自停
        //     `0x4B6C58` 就是这条路径：它会**跳过分层等待**，这正是"能不能等住"的分水岭）；
        //   `bytes=` = 该 Buffer 的环长度（`initial_pos` 是 mod bytes 的 ⇒ 没有它读不出圈数）。
        //   ⛔ 纯读：`isFeederThread()` 只读 atomic id、`bytes` 只读字段。
        logAt(LogLevel::Info, "stop state=0x%08X obj=0x%08X cursor=%u gen=%llu feeder=%d bytes=%u", state_ea,
              obj, b->initial_pos, static_cast<unsigned long long>(b->generation), isFeederThread() ? 1 : 0,
              b->bytes);
    }
}

void AudioEngine::setPosition(std::uint32_t state_ea, std::uint32_t pos) {
    GuestToken::YieldGuard yield(guestToken());  // ★ M0（总口径见 `addBuffer` 上方）
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    if (b == nullptr) return;
    // ★ 引擎只在 **Stop 之后**调它（`0x4B7441` 起播 seek 0 与 `0x4B751A` 按圈补齐后再 seek 0），
    //   而 `stop()` 已经 dropPending + `++generation`（+ 等完在飞 fill）⇒ 正常情况下这里立即返回。
    //   仍然显式作废一次：防"Stop 之后又被别处投递"的迟到通知把旧位置的通知喂给新位置
    //   （真机重定位后游标在新位置，旧位置的通知不该再出现）。
    ++b->generation;
    const std::uint32_t obj = b->obj_ea;
    dropPending(obj);
    if (thread_mode_ && !isFeederThread() && filling_obj_.load() == obj) {
        waitFill(obj, lock);
        // ★★ 同上：Yield 之后必须重新查表。
        b = find(state_ea);
        if (b == nullptr) return;
        // ★ 重校验**身份**（`obj` 是本次等待之前那一代算出来的）：`state_ea` 可能已被复用给
        //    另一条 obj ⇒ 不重校验就会把这条新位置写进**别人的** Buffer。⛔ 不要重做一遍重定位。
        if (b->obj_ea != obj) return;
    }
    b->initial_pos = b->bytes ? (pos % b->bytes) : 0u;
    const std::uint32_t align = b->fmt.block_align ? b->fmt.block_align : 4u;
    b->initial_pos -= b->initial_pos % align;  // 按**该缓冲**的帧对齐（mono 是 2，不是 4）
    if (std::getenv("LIFT_AUDIO_TRACE") != nullptr) {
        // ★ 只读诊断（原本这里**一行 trace 都没有**）：`setpos` 是 B2 的另一半 ——
        //   `pos=` 是**请求**的位置，`initial=` 是对齐后真正写进 `initial_pos` 的值
        //   （Play 把它当续读起点：`generateTo` 用 `initial_pos` 当源起点），`playing=` 说明
        //   这次重定位有没有顺带重排 `start_mixed` 并补发通知。
        //   ⛔ 纯读：只读字段，不写、不改判定。
        logAt(LogLevel::Info, "setpos state=0x%08X obj=0x%08X pos=%u initial=%u bytes=%u playing=%d", state_ea,
              obj, pos, b->initial_pos, b->bytes, b->playing ? 1 : 0);
    }
    if (b->playing) {
        // 真机 SetCurrentPosition 会让游标跳到新位置；引擎只在 Play 前调它，这里按同一时间线重排。
        b->start_mixed = generated_;
        for (std::uint32_t i = 0; i < b->notify_count; ++i) {
            if (b->notify_off[i] == b->initial_pos) enqueue(b->obj_ea, i, b->generation);
        }
    }
}

void AudioEngine::setVolume(std::uint32_t state_ea, std::int32_t centibel) {
    GuestToken::YieldGuard yield(guestToken());  // ★ M0（总口径见 `addBuffer` 上方）
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    if (b) b->volume = std::clamp<std::int32_t>(centibel, -10000, 0);
}

void AudioEngine::setPan(std::uint32_t state_ea, std::int32_t pan) {
    GuestToken::YieldGuard yield(guestToken());  // ★ M0（总口径见 `addBuffer` 上方）
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    // DSBPAN_LEFT/RIGHT = ±10000（1/100 dB 的左右偏置）。
    if (b) b->pan = std::clamp<std::int32_t>(pan, -10000, 10000);
}

std::uint32_t AudioEngine::cursorOf(const Buffer& b) const {
    // ⚠ 这是**唯一一份**"设备游标"算式：`playedCursor` 与 `late_q` 都走它（⛔ 不要各写一份）。
    //   语义与原来的 `playedCursor` 逐字相同（只把 `b` 从指针换成引用）。
    if (b.bytes == 0) return 0;
    if (!b.playing) return b.initial_pos % b.bytes;
    const std::uint64_t start_frame = b.start_mixed / kBlockAlign;
    const std::uint64_t played_frame = played_ / kBlockAlign;
    if (played_frame <= start_frame) return b.initial_pos % b.bytes;
    const std::uint64_t src_elapsed = (played_frame - start_frame) * b.fmt.avg_bytes / kSampleRate;
    return static_cast<std::uint32_t>((b.initial_pos + src_elapsed) % b.bytes);
}

std::uint32_t AudioEngine::playedCursor(std::uint32_t state_ea) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const Buffer* b = find(state_ea);
    if (b == nullptr) return 0;
    return cursorOf(*b);
}

// ★ 只读诊断（`LIFT_AUDIO_TRACE` 的 `late_q`）—— **定义**（写在这里，是为了公式只有一处）：
//
//     ring_bytes = 该 obj 在设备模型里的环长度（`Buffer::bytes`；= DSB 的 dwBytes）
//     q_bytes    = ring_bytes / 4                      // 一个 quarter 的字节数
//     target     = quarter 的**目标环位置**（字节）：
//                    有 `notify_off[quarter]` 且 < ring_bytes ⇒ 用它（引擎真正用于点火的那个偏移）
//                    ∈ [0, ring_bytes)          ⇒ 否则退回 quarter * q_bytes（引擎的 0/4、1/4、2/4、3/4）
//     cursor     = `cursorOf(b)`                        // 此刻设备游标（字节，落在 [0, ring_bytes)）
//     late_q     = (target - cursor) / q_bytes          // ★ **带符号**、**不取整**、**不做** mod 归一
//
//   ⇒ 单位是 quarter（浮点，打两位小数）：0 = 正好在目标上；+0.8 ≈ 落后 0.8 个 quarter（fill 提前的
//     常态）；负数 = 游标**已经越过**这一刀的目标（fill **迟了**，越负越迟）；≥ 4 = 落后一整圈以上
//     （目标已被上一圈越过）—— 这三种正是"重播 2 s"要区分的形状。
//   ★ 为什么**不**用整数："迟了 0.96 个 quarter"整数除法会截成 **0**（C++ 向零截断），
//     正好把"稍微迟了"抹成"正好"—— 而要看的正是这一档。⛔ 也不归一（mod ring_bytes）：
//     归一会把"迟了"折成"提前"。
//   ⛔ 纯读：只读 `buffers_` + `played_`；不发通知、不改 `playing` / `initial_pos` / 任何计数。
bool AudioEngine::fillLagQuarter(std::uint32_t obj_ea, std::uint32_t quarter, std::uint32_t* ring_bytes,
                                 double* late_q) const {
    if (obj_ea == 0) return false;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (const auto& b : buffers_) {
        if (b.obj_ea != obj_ea) continue;
        if (b.bytes < 4u) return false;  // 环小到一个 quarter 都没有 ⇒ 分母无意义
        const std::uint64_t q_bytes = b.bytes / 4u;
        std::uint64_t target = static_cast<std::uint64_t>(quarter) * q_bytes;
        if (quarter < b.notify_count && b.notify_off[quarter] < b.bytes) {
            target = b.notify_off[quarter];
        }
        const std::int64_t delta = static_cast<std::int64_t>(target) -
                                   static_cast<std::int64_t>(cursorOf(b));
        if (ring_bytes) *ring_bytes = b.bytes;
        if (late_q) *late_q = static_cast<double>(delta) / static_cast<double>(q_bytes);
        return true;
    }
    return false;
}

bool AudioEngine::findSlot(std::uint32_t obj_ea, std::uint32_t* slot, std::uint32_t* stream) const {
    if (feeder_.sound == 0 || obj_ea == 0 || mem_ == nullptr) return false;
    for (std::uint32_t s = 0; s < kSlotCount; ++s) {
        const std::uint32_t stream_ea = mem_->rd32(feeder_.sound + kSoundSlots + 4u * s);
        if (stream_ea == 0 || !mem_->committed(stream_ea + kStreamDsBuffer, 4)) continue;
        if (mem_->rd32(stream_ea + kStreamDsBuffer) != obj_ea) continue;
        if (slot) *slot = s;
        if (stream) *stream = stream_ea;
        return true;
    }
    return false;
}

AudioEngine::Context AudioEngine::capture() const {
    Context c;
    for (int i = 0; i < 8; ++i) c.r[i] = cpu_->r[i];
    c.eip = cpu_->eip;
    c.eflags = cpu_->eflags;
    for (int i = 0; i < 6; ++i) c.seg[i] = cpu_->seg[i];
    c.fs_base = cpu_->fs_base;
    c.gs_base = cpu_->gs_base;
    c.fpu = cpu_->fpu;
    std::memcpy(c.xmm, cpu_->xmm, sizeof(c.xmm));
    c.thread_id = cpu_->shim.thread_id;
    c.last_error = cpu_->shim.last_error;
    return c;
}

void AudioEngine::restore(const Context& c, std::uint32_t* shim_result) {
    for (int i = 0; i < 8; ++i) cpu_->r[i] = c.r[i];
    cpu_->eip = c.eip;
    cpu_->eflags = c.eflags;
    for (int i = 0; i < 6; ++i) cpu_->seg[i] = c.seg[i];
    cpu_->fs_base = c.fs_base;
    cpu_->gs_base = c.gs_base;
    cpu_->fpu = c.fpu;
    std::memcpy(cpu_->xmm, c.xmm, sizeof(c.xmm));
    cpu_->shim.thread_id = c.thread_id;
    cpu_->shim.last_error = c.last_error;
    // `GuestCallStack::resume()` 在续延返回之后写 eip/esp/eax；把 eax 通过 *shim_result 交回，
    // eip/esp 由"借用 [esp] = snapshot.eip + cleanup=0"那套保证（见 pump()）。
    *shim_result = c.r[kEax];
}

void AudioEngine::enqueue(std::uint32_t obj_ea, std::uint32_t quarter, std::uint64_t generation) {
    if (obj_ea == 0 || quarter > 3) return;
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        for (const auto& e : pending_) {
            // 自动复位事件语义：重复 SetEvent 合并。★ 代次必须一起比——否则"旧代次还排在队里"
            // 会把同 quarter 的新代次条目吞掉（Stop 会先 drop，正常情况下不会出现）。
            if (e.obj == obj_ea && e.quarter == quarter && e.generation == generation) return;
        }
        if (pending_.size() >= 256) {
            pending_.pop_front();
            ++stats_.dropped;
        }
        pending_.push_back(AudioNotifyEvent{obj_ea, quarter, generation});
        ++stats_.notifications;
    }
    // 真线程模式：唤醒 feeder；泵模式没有等待者，notify 无害。
    feeder_cv_.notify_one();
}

// ★ 所有对 `pending_` 的"删除"都走这里（D2）：`pending_` 归 `pending_mutex_` 管，
//   锁序 `mutex_ → pending_mutex_`；⛔ 不要在 `mutex_` 下裸动 deque。
void AudioEngine::dropPending(std::uint32_t obj_ea) {
    if (obj_ea == 0) return;
    std::lock_guard<std::mutex> lk(pending_mutex_);
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                  [obj_ea](const AudioNotifyEvent& e) { return e.obj == obj_ea; }),
                   pending_.end());
}

void AudioEngine::fireCrossings(std::uint64_t old_played, std::uint64_t new_played) {
    if (new_played <= old_played) return;
    const std::uint64_t old_frame = old_played / kBlockAlign;
    const std::uint64_t new_frame = new_played / kBlockAlign;
    for (auto& b : buffers_) {
        if (!b.playing || b.bytes == 0 || b.notify_count == 0) continue;
        if (new_played <= b.start_mixed) continue;
        const std::uint64_t start_frame = b.start_mixed / kBlockAlign;
        // 缓冲自己的字节时间线：offset 是**这条缓冲**的字节偏移，不能拿 48k 立体声输出字节直接比。
        // ★ 必须加 `initial_pos`（SetCurrentPosition 的起点）；否则非 0 起播时通知会晚一整圈。
        const std::uint64_t rel_old = b.initial_pos +
                                      (old_frame > start_frame
                                           ? (old_frame - start_frame) * b.fmt.avg_bytes / kSampleRate
                                           : 0ull);
        const std::uint64_t rel_new = b.initial_pos +
                                      (new_frame > start_frame
                                           ? (new_frame - start_frame) * b.fmt.avg_bytes / kSampleRate
                                           : 0ull);
        if (rel_new <= rel_old) continue;
        // ★ 一次推进可能跨过多个通知点（fill 期间虚拟时钟仍在走）。必须按**位置**排序后再入队：
        //   按 quarter 序号入队会把"回绕 0"排在"3/4"之前（实测错序：0,1,2,0,3…）。
        std::vector<std::pair<std::uint64_t, std::uint32_t>> crosses;
        for (std::uint32_t i = 0; i < b.notify_count; ++i) {
            const std::uint64_t off = b.notify_off[i];
            if (off >= b.bytes) continue;
            const std::int64_t base = static_cast<std::int64_t>(rel_old) - static_cast<std::int64_t>(off);
            std::int64_t k = base >= 0 ? base / static_cast<std::int64_t>(b.bytes) + 1 : 0;
            if (k < 0) k = 0;
            std::uint64_t pos = off + static_cast<std::uint64_t>(k) * b.bytes;
            std::uint32_t guard = 0;
            bool truncated = false;
            while (pos <= rel_new) {
                if (guard++ >= 64u) {
                    truncated = true;
                    break;
                }
                crosses.emplace_back(pos, i);
                pos += b.bytes;
            }
            if (truncated) ++stats_.dropped;
        }
        std::sort(crosses.begin(), crosses.end(),
                  [](const std::pair<std::uint64_t, std::uint32_t>& a,
                     const std::pair<std::uint64_t, std::uint32_t>& c) {
                      return a.first != c.first ? a.first < c.first : a.second < c.second;
                  });
        for (const auto& c : crosses) enqueue(b.obj_ea, c.second, b.generation);
    }
}

void AudioEngine::pushHostPcm(const std::int16_t* interleaved, std::uint32_t frames) {
    if (interleaved == nullptr || frames == 0) return;
    if (host_pcm_.empty()) {
        // 从"当前已经生成到的输出帧"开始顺序播（生成点之后 = 设备队列的尾部）。
        host_start_frame_ = generated_ / kBlockAlign;
    }
    host_pcm_.insert(host_pcm_.end(), interleaved, interleaved + static_cast<std::size_t>(frames) * 2u);
}

void AudioEngine::clearHostPcm() {
    host_pcm_.clear();
    host_start_frame_ = 0;
}

void AudioEngine::generateTo(std::uint64_t target) {
    if (target <= generated_) return;
    std::uint64_t n = target - generated_;
    n &= ~static_cast<std::uint64_t>(kBlockAlign - 1);  // 输出帧对齐（48k / S16 / 2ch）
    if (n == 0) return;
    const std::uint64_t out_frames64 = n / kBlockAlign;
    if (out_frames64 > 0xFFFFFFFFull) return;
    const std::uint32_t out_frames = static_cast<std::uint32_t>(out_frames64);
    std::vector<std::int64_t> acc(static_cast<std::size_t>(out_frames) * 2u, 0);
    const std::uint64_t out_start_frame = generated_ / kBlockAlign;

    for (const auto& b : buffers_) {
        if (!b.playing || b.bytes < b.fmt.block_align || b.fmt.block_align == 0) continue;
        if (b.fmt.bits != 16 && b.fmt.bits != 8) continue;
        if (b.fmt.channels == 0 || b.fmt.sample_rate == 0) continue;
        if (generated_ < b.start_mixed) continue;
        const std::uint32_t bpf = b.fmt.block_align;
        const std::uint32_t sample_bytes = b.fmt.bits / 8u;
        // 防御畸形 WAVEFORMATEX：block_align 必须至少容纳实际读取的 L/R 样本，否则会读越界。
        if (bpf < sample_bytes * std::min<std::uint32_t>(b.fmt.channels, 2u)) continue;
        const std::uint32_t total_frames = b.bytes / bpf;
        if (total_frames == 0) continue;
        if (!mem_->committed(b.data_ea, b.bytes)) continue;

        // 这条流的源位置（源帧，16.16 定点）：initial_pos + 已输出时间 * 源采样率 / 48000。
        const std::uint64_t elapsed_out = out_start_frame - b.start_mixed / kBlockAlign;
        const std::uint64_t init_frame = b.initial_pos / bpf;
        const std::uint64_t num = elapsed_out * b.fmt.sample_rate;
        std::uint64_t pos_q16 = (init_frame << 16) + ((num << 16) / kSampleRate);
        const std::uint64_t step_q16 = (static_cast<std::uint64_t>(b.fmt.sample_rate) * 65536ull + kSampleRate / 2) /
                                       kSampleRate;

        // 1/100 dB → 幅度；DS 的 0 dB = 1.0，-10000 = -100 dB。pan 是线性左右衰减。
        const double amp = std::pow(10.0, static_cast<double>(b.volume) / 2000.0);
        const double pl = b.pan <= 0 ? 1.0 : (10000 - b.pan) / 10000.0;
        const double pr = b.pan >= 0 ? 1.0 : (10000 + b.pan) / 10000.0;
        const std::int32_t gl = gainQ16(amp * pl);
        const std::int32_t gr = gainQ16(amp * pr);

        auto readFrame = [&](std::uint32_t idx, std::int32_t* l, std::int32_t* r) {
            const std::uint8_t* p = mem_->host(b.data_ea + idx * bpf);
            if (b.fmt.bits == 16) {
                *l = static_cast<std::int16_t>(p[0] | (p[1] << 8));
                if (b.fmt.channels >= 2) {
                    *r = static_cast<std::int16_t>(p[2] | (p[3] << 8));
                } else {
                    *r = *l;
                }
            } else {
                *l = (static_cast<std::int32_t>(p[0]) - 128) << 8;
                if (b.fmt.channels >= 2) {
                    *r = (static_cast<std::int32_t>(p[1]) - 128) << 8;
                } else {
                    *r = *l;
                }
            }
        };

        std::uint64_t pos = pos_q16;
        for (std::uint32_t o = 0; o < out_frames; ++o) {
            const std::uint32_t idx = static_cast<std::uint32_t>((pos >> 16) % total_frames);
            const std::uint32_t frac = static_cast<std::uint32_t>(pos & 0xFFFFu);
            const std::uint32_t idx2 = (idx + 1u) % total_frames;
            std::int32_t l0 = 0, r0 = 0, l1 = 0, r1 = 0;
            readFrame(idx, &l0, &r0);
            if (frac != 0) readFrame(idx2, &l1, &r1);
            const std::int32_t l = l0 + static_cast<std::int32_t>((static_cast<std::int64_t>(l1 - l0) * frac) >> 16);
            const std::int32_t r = r0 + static_cast<std::int32_t>((static_cast<std::int64_t>(r1 - r0) * frac) >> 16);
            acc[static_cast<std::size_t>(o) * 2u + 0] += (static_cast<std::int64_t>(l) * gl) >> 16;
            acc[static_cast<std::size_t>(o) * 2u + 1] += (static_cast<std::int64_t>(r) * gr) >> 16;
            pos += step_q16;
        }
    }

    // ★ 影片音频（宿主 PCM 源）：按输出帧序号从 `host_pcm_` 混入（48k/S16/2ch，已含音量）。
    if (!host_pcm_.empty()) {
        for (std::uint32_t o = 0; o < out_frames; ++o) {
            const std::uint64_t f = out_start_frame + o;
            if (f < host_start_frame_) continue;
            const std::uint64_t idx = (f - host_start_frame_) * 2u;
            if (idx + 1u >= host_pcm_.size()) break;
            acc[static_cast<std::size_t>(o) * 2u + 0] += host_pcm_[idx + 0u];
            acc[static_cast<std::size_t>(o) * 2u + 1] += host_pcm_[idx + 1u];
        }
        if (out_start_frame + out_frames > host_start_frame_) {
            const std::uint64_t consumed_frames = out_start_frame + out_frames - host_start_frame_;
            const std::uint64_t consumed =
                std::min<std::uint64_t>(consumed_frames * 2u, static_cast<std::uint64_t>(host_pcm_.size()));
            host_pcm_.erase(host_pcm_.begin(), host_pcm_.begin() + static_cast<std::ptrdiff_t>(consumed));
            host_start_frame_ += consumed / 2u;
        }
    }

    std::vector<std::uint8_t> out(static_cast<std::size_t>(n), 0);
    for (std::uint32_t o = 0; o < out_frames; ++o) {
        std::int64_t l = acc[static_cast<std::size_t>(o) * 2u + 0];
        std::int64_t r = acc[static_cast<std::size_t>(o) * 2u + 1];
        if (l > 32767) l = 32767;
        if (l < -32768) l = -32768;
        if (r > 32767) r = 32767;
        if (r < -32768) r = -32768;
        const std::uint16_t ul = static_cast<std::uint16_t>(static_cast<std::int16_t>(l));
        const std::uint16_t ur = static_cast<std::uint16_t>(static_cast<std::int16_t>(r));
        const std::size_t off = static_cast<std::size_t>(o) * 4u;
        out[off + 0] = static_cast<std::uint8_t>(ul & 0xFF);
        out[off + 1] = static_cast<std::uint8_t>(ul >> 8);
        out[off + 2] = static_cast<std::uint8_t>(ur & 0xFF);
        out[off + 3] = static_cast<std::uint8_t>(ur >> 8);
    }
    // ★ 唯一最终交付点：`out` 生成之后、`pushMixed` 之前。静音只在这里覆盖缓冲
    //   （一份缓冲、两个 sink ⇒ SDL 与 dump 必然一致）。
    //   ⛔ 不许挪进 `pushMixed` / `writeDump` 的任一支路；⛔ 不许改成"不建设备 / 不起 feeder /
    //   提前退出 / 改 guest SetVolume / 降填充率"——那些都会改变 guest 侧行为或计数。
    if (mute_) {
        std::memset(out.data(), 0, out.size());
        stats_.muted_bytes += n;
    }
    generated_ += n;
    stats_.mixed_bytes += n;
    pushMixed(out.data(), static_cast<std::size_t>(n));
}

void AudioEngine::pushMixed(const std::uint8_t* pcm, std::size_t n) {
    if (n == 0) return;
    if (sdl_ok_ && stream_) {
        if (SDL_PutAudioStreamData(static_cast<SDL_AudioStream*>(stream_), pcm, static_cast<int>(n))) {
            pushed_ += n;
            stats_.pushed_bytes = pushed_;
        } else {
            if (!logged_sdl_fail_) {
                logged_sdl_fail_ = true;
                logAt(LogLevel::Warn, "SDL_PutAudioStreamData 失败：%s ⇒ 之后只写 dump", SDL_GetError());
            }
            sdl_ok_ = false;
        }
    }
    writeDump(pcm, n);
}

void AudioEngine::advanceDevice() {
    if (mode_ == AudioMode::Sdl && sdl_ok_ && stream_) {
        const int q = SDL_GetAudioStreamQueued(static_cast<SDL_AudioStream*>(stream_));
        if (q >= 0) {
            queued_ = static_cast<std::uint64_t>(q);
        } else if (!logged_sdl_fail_) {
            logged_sdl_fail_ = true;
            logAt(LogLevel::Warn, "SDL_GetAudioStreamQueued 失败：%s", SDL_GetError());
        }
        const std::uint64_t new_played = pushed_ >= queued_ ? pushed_ - queued_ : 0ull;
        if (new_played > played_) {
            fireCrossings(played_, new_played);
            played_ = new_played;
        }
    } else {
        const std::uint64_t now = clock_->now100ns();
        const std::uint64_t new_played = now * kBytesPerSec / kTicksPerSec;
        if (new_played > played_) {
            fireCrossings(played_, new_played);
            played_ = new_played;
        }
    }
    stats_.played_bytes = played_;
}

void AudioEngine::mixStep() {
    // 调用方持有 mutex_。
    advanceDevice();
    if (mode_ == AudioMode::Sdl && sdl_ok_) {
        const std::uint64_t lead = thread_mode_ ? kThreadLeadBytes : kLeadBytes;
        const std::uint64_t target = played_ + lead;
        if (target > generated_) generateTo(target);
        bool playing = false;
        for (const auto& b : buffers_)
            if (b.playing && b.bytes != 0) playing = true;
        if (queued_ == 0 && playing) {
            if (!underrun_active_) {
                ++stats_.underruns;
                underrun_active_ = true;
            }
        } else {
            underrun_active_ = false;
        }
    } else if (dump_ok_) {
        if (played_ > generated_) generateTo(played_);
    } else {
        generated_ = played_;
    }
}

void AudioEngine::mixLoop() {
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(mix_wait_mutex_);
            mix_cv_.wait_for(lk, std::chrono::milliseconds(5), [this] { return mix_stop_; });
            if (mix_stop_) break;
        }
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!begun_) break;
        mixStep();
    }
}

void AudioEngine::startMixThread() {
    if (mix_thread_.joinable()) return;
    mix_stop_ = false;
    mix_thread_ = std::thread(&AudioEngine::mixLoop, this);
}

void AudioEngine::stopMixThread() {
    {
        std::lock_guard<std::mutex> lk(mix_wait_mutex_);
        mix_stop_ = true;
    }
    mix_cv_.notify_all();
    if (mix_thread_.joinable()) mix_thread_.join();
}

AudioPump AudioEngine::pump() {
    if (!enabled() || !begun_ || !cpu_ || !mem_) return AudioPump::Idle;
    // ★★ 锁序前提 —— 下面这个早退**就是**它（⛔ 不要删、不要放宽、不要把 thread 模式放到它后面）：
    //   `pump()` 是全文**唯一**一处 `mutex_ → 令牌` 的路径（本函数里 `std::lock_guard` 取
    //   `mutex_`、`GuestToken::Guard` 取令牌），与全局锁序（**令牌 → `mutex_`**：每个 DS shim /
    //   `feederEvent`）相反。它不成环靠的是一条不变量：**D 模式（`thread_mode_ == false`）
    //   没有 feeder 线程、也没有 mixer 线程 ⇒ 除本线程外没有令牌持有者** ⇒ "持 `mutex_` 等令牌"
    //   永远等不到一个"要拿 `mutex_` 才能放令牌"的线程 ⇒ 反向锁序不可能成环。
    //   ⇒ thread 模式**必须**在这一块里返回：它既有 feeder 又有 mixer，正是可能持令牌的"别人"。
    //   真线程模式：主线程也**不要**每块都去抢 audio mutex（那会和 mixer/feeder 形成
    //   cache-line ping-pong，实测把 fill 从 400ms 拖到 650ms）。直接无锁返回。
    if (thread_mode_) {
        if (feeder_error_.load()) {
            last_error_ = "音频 feeder 线程失败（详见 stderr）";
            return AudioPump::Error;
        }
        return AudioPump::Idle;
    }
    // ★ 走到这里的**只有** `thread_mode_ == false`（上面那块已返回）⇒ 本引擎当前没有 feeder
    //   线程、也没有 mixer 线程。这就是上面那条不变量的唯一来源：
    //   `thread_mode_` 只在 `begin()` 由 `Sdl && clock_->realTime()` 置真；它从 true 变 false
    //   的**唯一**路径是 `startFeederThread()` 里 `setupFeederContext()` 失败 —— 那一步发生在
    //   **建线程之前**（失败时 `thread_mode_` 改回 false、`stopMixThread()`，日志"退回主线程泵"）
    //   ⇒ **不存在"feeder 线程已经在跑、而 `pump()` 走 D"的状态**（复审
    //     `.tmp/audio-token/compensation-change-review.md` §4 / `feed-paths-audit.md` §4 #2）。
    //   ⛔ 因此这里**不再加**"是否曾经起过线程"的闩/断言位：它要防的状态今天不可达，删掉它
    //     少一份过期保证。⚠ 但若将来给 `std::thread` 的构造加 catch 并继续运行、或加 feeder
    //     restart，**先回来改这里**：那时 `pump()` 的反向锁序前提（"除本线程外没有令牌持有者"）
    //     就不再成立，需要重新引入一道可判的硬门。
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // ★ 单执行流纪律：任何嵌套 guest 调用进行中都不混音、不铺新帧。
    //   fill 自己会写 guest 环；此时读环 = 真机上"另一个线程正在写"的竞态，D 里必须避免。
    if (in_fill_) return AudioPump::Idle;
    if (cpu_->calls && cpu_->calls->active()) return AudioPump::Idle;
    if (cpu_->shim.callback.active) return AudioPump::Idle;
    if (cpu_->shim.timer_firing) return AudioPump::Idle;

    mixStep();

    if (!feeder_.registered || !feeder_.runnable || feeder_.exited) return AudioPump::Idle;
    if (feeder_.exit_requested) {
        feeder_.exited = true;
        feeder_.runnable = false;
        feeder_.exit_requested = false;
        pending_.clear();
        logAt(LogLevel::Info, "喂料泵收到退出事件（#60）⇒ 退出；fills=%llu",
              static_cast<unsigned long long>(stats_.fills));
        return AudioPump::Idle;
    }

    while (!pending_.empty()) {
        const AudioNotifyEvent ev = pending_.front();
        // ★ 与 `feederEvent` 同一条语义（两条重播路径必须一个判据）：Stop / SetCurrentPosition
        //   之后，之前排队的条目作废。D 模式下同一线程其实不会过期，但成本为零、少一处"两种
        //   模式行为不同"的隐式约定。`findByObj` 走递归 `mutex_`（本函数已持有）。
        const Buffer* gen_b = findByObj(ev.obj);
        if (gen_b == nullptr || gen_b->generation != ev.generation) {
            pending_.pop_front();
            ++stats_.stale;
            continue;
        }
        std::uint32_t slot = 0;
        std::uint32_t stream = 0;
        if (!findSlot(ev.obj, &slot, &stream)) {
            if (!warned_slot_mismatch_) {
                warned_slot_mismatch_ = true;
                logAt(LogLevel::Warn,
                      "通知的 obj=0x%08X 在 Sound+0x654 的 15 个槽里找不到（[stream+0x40C] 不匹配）⇒ "
                      "丢掉该通知；音频布局可能已漂（fills 会停）",
                      ev.obj);
            }
            pending_.pop_front();
            ++stats_.dropped;
            continue;
        }
        const std::uint32_t cs = feeder_.sound + kSoundCsBase + 24u * slot;
        if (!mem_->committed(cs, 0x18)) {
            pending_.pop_front();
            ++stats_.dropped;
            continue;
        }
        const std::uint32_t owner = guestCriticalSectionOwner(*cpu_, cs);
        if (owner != 0 && owner != feeder_.thread_id) {
            // ★ D.4-③：主线程正持有这个槽的 CS（真机上 feeder 会阻塞）。本次不填，
            //   保留在队首，下一个块边界再试。⛔ 不插进"主线程正在改这个槽"的中途。
            ++stats_.deferred;
            return AudioPump::Idle;
        }
        const std::uint32_t slot_obj = mem_->rd32(feeder_.sound + kSoundSlots + 4u * slot);
        if (slot_obj == 0 || slot_obj != stream) {
            if (!warned_slot_mismatch_) {
                warned_slot_mismatch_ = true;
                logAt(LogLevel::Warn,
                      "槽 %u 的 SoundBuffer*=0x%08X 与通知的 obj=0x%08X 不一致 ⇒ 丢掉该通知", slot,
                      slot_obj, ev.obj);
            }
            pending_.pop_front();
            ++stats_.dropped;
            continue;
        }
        std::string err;
        if (!guestEnterCriticalSectionAs(*cpu_, cs, feeder_.thread_id, &err)) {
            last_error_ = "音频泵进入临界区失败：" + err;
            return AudioPump::Error;
        }
        const std::uint32_t inf_ea = feeder_.sound + kSoundInflight + 4u * slot;
        const std::uint32_t inf = mem_->committed(inf_ea, 4) ? mem_->rd32(inf_ea) : 0u;
        if (mem_->committed(inf_ea, 4)) mem_->wr32(inf_ea, inf + 1u);
        pending_.pop_front();

        // ---- 保存整份 guest 上下文，借用 [esp] 作为"返回点" ----
        snapshot_ = capture();
        if (!mem_->committed(snapshot_.r[kEsp], 4)) {
            if (mem_->committed(inf_ea, 4)) mem_->wr32(inf_ea, inf);
            guestLeaveCriticalSectionAs(*cpu_, cs, feeder_.thread_id, nullptr);
            last_error_ = "音频泵：guest esp 不在已提交栈上";
            return AudioPump::Error;
        }
        borrowed_esp_ = snapshot_.r[kEsp];
        saved_ret_ = mem_->rd32(borrowed_esp_);
        borrowed_ret_ = true;
        mem_->wr32(borrowed_esp_, snapshot_.eip);  // `start()` 会把它当作 ret_ea 读走
        cpu_->shim.thread_id = feeder_.thread_id;
        cpu_->shim.last_error = 0;
        in_fill_ = true;
        fill_cs_ = cs;
        fill_inf_ea_ = inf_ea;

        AudioEngine* self = this;
        // ★ 单令牌（guest_token.h）：这条 pump 路径会**铺一个 guest 帧**（dispatcher 随后执行它）。
        //   主线程此时已持有令牌（Machine::run 的块边界）⇒ 这里只是重入；它保证
        //   "将来谁在别的线程调 pump()" 也会被令牌挡住（⛔ 不许有第二条无令牌的 guest 入口）。
        GuestToken::Guard pump_token(guestToken());
        guestToken().check("audio.pump");
        const std::uint32_t quarter = ev.quarter;
        const auto fill_start = std::chrono::steady_clock::now();
        // ★ 只读诊断（`late_q`）：**fill 开始**那一刻的滞后量。公式与"为什么带符号/不归一"见
        //   `fillLagQuarter` 的定义注释。⚠ 必须在 `startNestedGuestCall` **之前**取（那才是 fill 开始）。
        std::uint32_t lag_bytes = 0;
        double lag_q = 0.0;
        const std::uint32_t ev_obj = ev.obj;  // trace 用（捕获；`ev` 本身不捕，只捕需要的字段）
        const bool lag_ok = fillLagQuarter(ev_obj, quarter, &lag_bytes, &lag_q);
        const bool started = startNestedGuestCall(
            *cpu_, kSub4B6C30, {quarter}, 0u, "audio:feeder",
            [self, slot, quarter, stream, cs, inf_ea, fill_start, lag_ok, lag_bytes, lag_q,
             ev_obj](X86& c, std::uint32_t cb_result, std::uint32_t* out) {
                if (c.mem->committed(inf_ea, 4)) {
                    const std::uint32_t cur = c.mem->rd32(inf_ea);
                    if (cur > 0) c.mem->wr32(inf_ea, cur - 1u);
                }
                std::string leave_err;
                if (!guestLeaveCriticalSectionAs(c, cs, self->feeder_.thread_id, &leave_err)) {
                    std::fprintf(stderr, "[audio] 离开槽临界区失败：%s\n", leave_err.c_str());
                }
                self->restore(self->snapshot_, out);
                self->in_fill_ = false;
                ++self->stats_.fills;
                // ★ 只读诊断计数（dumpSummary 的 `fill_max_ms` / `fill_slow`）：⛔ 不参与判定。
                const std::uint64_t fill_us = static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                         fill_start)
                        .count());
                self->stats_.fill_max_us = std::max(self->stats_.fill_max_us, fill_us);
                if (fill_us > 1000000ull) ++self->stats_.fill_slow;
                if (std::getenv("LIFT_AUDIO_TRACE") != nullptr) {
                    const double ms = static_cast<double>(fill_us) / 1000.0;
                    // ★ 只读诊断：`[stream+0x2450]`（循环重解码标志）/`+0x2458`/`+0x2470` 是 B1 的
                    //   判据；**读不到就打 `?`**（⛔ 不打 0）。`bytes=` 与 `late_q` 的分母同一个值。
                    char f2450[16];
                    char f2458[16];
                    char f2470[16];
                    char bytes_txt[16];
                    char late_txt[24];
                    traceDword(c.mem, stream + 0x2450u, f2450, sizeof f2450);
                    traceDword(c.mem, stream + 0x2458u, f2458, sizeof f2458);
                    traceDword(c.mem, stream + 0x2470u, f2470, sizeof f2470);
                    if (lag_ok) {
                        std::snprintf(bytes_txt, sizeof bytes_txt, "%u", lag_bytes);
                        std::snprintf(late_txt, sizeof late_txt, "%0.2f", lag_q);
                    } else {
                        std::snprintf(bytes_txt, sizeof bytes_txt, "?");
                        std::snprintf(late_txt, sizeof late_txt, "?");
                    }
                    std::fprintf(stderr,
                                 "[audio] fill slot=%u quarter=%u ret=%u state=%u wrote=%u L=%u "
                                 "obj=0x%08X bytes=%s 2450=%s 2458=%s 2470=%s late_q=%s %.2f ms\n",
                                 slot, quarter, cb_result,
                                 c.mem->committed(stream + 0x2458, 4) ? c.mem->rd32(stream + 0x2458) : 0u,
                                 c.mem->committed(stream + 0x2468, 4) ? c.mem->rd32(stream + 0x2468) : 0u,
                                 c.mem->committed(stream + kStreamBytes, 4) ? c.mem->rd32(stream + kStreamBytes) : 0u,
                                 ev_obj, bytes_txt, f2450, f2458, f2470, late_txt, ms);
                }
                return true;
            },
            &err, /*callee_ecx=*/stream);
        if (!started) {
            in_fill_ = false;
            if (borrowed_ret_) {
                if (mem_->committed(borrowed_esp_, 4)) mem_->wr32(borrowed_esp_, saved_ret_);
                borrowed_ret_ = false;
            }
            // 铺帧阶段已经改过线程身份 / last_error；失败时也要还回去（否则主线程带着 0x1008 继续跑）。
            cpu_->shim.thread_id = snapshot_.thread_id;
            cpu_->shim.last_error = snapshot_.last_error;
            if (mem_->committed(inf_ea, 4)) mem_->wr32(inf_ea, inf);
            guestLeaveCriticalSectionAs(*cpu_, cs, feeder_.thread_id, nullptr);
            last_error_ = "音频泵铺 sub_4B6C30 帧失败：" + err;
            return AudioPump::Error;
        }
        return AudioPump::Suspended;
    }
    return AudioPump::Idle;
}

void AudioEngine::afterGuestCall() {
    if (borrowed_ret_) {
        if (mem_ && mem_->committed(borrowed_esp_, 4)) mem_->wr32(borrowed_esp_, saved_ret_);
        borrowed_ret_ = false;
    }
}

bool AudioEngine::shouldOwnThread(std::uint32_t start_ea) const {
    return enabled() && start_ea == kFeederStartEa && !feeder_.registered;
}

bool AudioEngine::registerFeeder(std::uint32_t start_ea, std::uint32_t param, std::uint32_t thread_id,
                                 std::uint32_t handle) {
    if (!enabled() || start_ea != kFeederStartEa || feeder_.registered) return false;
    if (param == 0 || !mem_->committed(param, 4) || !mem_->committed(param + kSoundSlots, kSlotCount * 4u) ||
        !mem_->committed(param + kSoundCsBase, kSlotCount * 24u) ||
        !mem_->committed(param + kSoundInflight, kSlotCount * 4u)) {
        logAt(LogLevel::Warn, "音频喂料线程的 param=0x%08X 不是可读的 Sound 对象 ⇒ 不接管", param);
        return false;
    }
    feeder_.registered = true;
    feeder_.runnable = false;
    feeder_.exited = false;
    feeder_.exit_requested = false;
    feeder_.start_ea = start_ea;
    feeder_.sound = param;
    feeder_.thread_id = thread_id;
    feeder_.handle = handle;
    feeder_.quit_event = mem_->committed(param + kSoundQuitEvent, 4) ? mem_->rd32(param + kSoundQuitEvent) : 0u;
    logAt(LogLevel::Info,
          "登记喂料泵：Sound=0x%08X thread=0x%08X handle=0x%08X quitEvent=0x%08X；"
          "start routine 不执行（12 段 native + 0x4B6C30 由泵调）",
          param, thread_id, handle, feeder_.quit_event);
    return true;
}

void AudioEngine::resumeFeeder() {
    if (!feeder_.registered || feeder_.exited) return;
    feeder_.runnable = true;
    if (thread_mode_) {
        startFeederThread();
        logAt(LogLevel::Info, "喂料泵已启动（host-thread）；等待设备游标越过通知点");
        return;
    }
    logAt(LogLevel::Info, "喂料泵已启动（runnable）；等待设备游标越过通知点");
}

bool AudioEngine::isFeederHandle(std::uint32_t handle) const {
    return feeder_.registered && handle != 0 && handle == feeder_.handle;
}

void AudioEngine::noteEventSet(std::uint32_t handle) {
    if (!feeder_.registered || handle == 0 || handle != feeder_.quit_event) return;
    if (thread_mode_) {
        {
            std::lock_guard<std::mutex> lk(pending_mutex_);
            stop_requested_ = true;
        }
        feeder_cv_.notify_all();
        logAt(LogLevel::Info, "SetEvent(quit=0x%08X) ⇒ feeder 线程退出", handle);
        return;
    }
    feeder_.exit_requested = true;
    logAt(LogLevel::Info, "SetEvent(quit=0x%08X) ⇒ 喂料泵将在下一个块边界退出", handle);
}

void AudioEngine::joinFeeder() {
    if (!feeder_.registered) return;
    if (thread_mode_) {
        // 主线程可能正持有某个槽 CS 停在 run 里；feeder 会永久阻塞在 Enter 上 ⇒ 先放开本线程的锁。
        // ★ 单令牌：`releaseAll` 会写 guest 可见的 CS 字段 ⇒ 必须在令牌里做。
        //   `end()` 那条路径主线程**不持**令牌，而 feeder 可能正在 fill 里 ⇒ 先取一次再放。
        //   若 feeder 正持令牌跑 fill，这里会等它跑完（join 本来也要等）；若它正阻塞在
        //   `releaseAll` 要放开的那个 CS 上，它已经 Yield 了令牌 ⇒ 这里能立刻取到。
        {
            const bool held = guestToken().heldByCurrentThread();
            if (!held) guestToken().acquire();
            if (cpu_ && cpu_->cs_table) cpu_->cs_table->releaseAll(*cpu_, cpu_->shim.thread_id);
            if (!held) guestToken().release();
        }
        {
            std::lock_guard<std::mutex> lk(pending_mutex_);
            stop_requested_ = true;
        }
        feeder_cv_.notify_all();
        // ★ 单令牌：主线程可能正持有令牌（这是 guest 的 WaitForSingleObject shim 调进来的）。
        //   不放开就 join ⇒ feeder 永远等不到令牌跑完手上的块 ⇒ 死锁。
        //   Yield 在"本线程没持令牌"时是无操作（`end()` 里 join 就是这种情形）。
        GuestToken::YieldGuard yield(guestToken());
        if (feeder_thread_.joinable()) feeder_thread_.join();
        feeder_.exited = true;
        feeder_.runnable = false;
        feeder_.exit_requested = false;
        {
            std::lock_guard<std::mutex> lk(pending_mutex_);
            pending_.clear();
        }
        logAt(LogLevel::Info, "WaitForSingleObject(喂料线程) ⇒ feeder 线程已 join（fills=%llu）",
              static_cast<unsigned long long>(stats_.fills));
        return;
    }
    if (!feeder_.exited) {
        feeder_.exited = true;
        feeder_.runnable = false;
        feeder_.exit_requested = false;
        pending_.clear();
        logAt(LogLevel::Info, "WaitForSingleObject(喂料线程) ⇒ 泵标记退出（fills=%llu）；此后不再写 guest 环",
              static_cast<unsigned long long>(stats_.fills));
    }
}

void AudioEngine::installFeederShim(std::uint32_t stub_ea, std::uint32_t shim_id) {
    std::lock_guard<std::mutex> lock(feeder_cache_mutex_);
    if (audio_cache_) audio_cache_->installShim(stub_ea, shim_id);
}

bool AudioEngine::setupFeederContext(std::string* err) {
    // 专用 guest 栈：主栈在 0x6FF00000..0x70000000；feeder 用下面 1 MiB（0x6FD00000..0x6FE00000）。
    audio_stack_top_ = 0x6FE00000u;
    if (!mem_->commitZeroed(audio_stack_top_ - kAudioStackSize, kAudioStackSize, RegionKind::Stack,
                            "audio-feeder-stack")) {
        if (err) *err = std::string("feeder 栈提交失败：") + mem_->lastError();
        return false;
    }
    audio_decoder_ = std::make_unique<Decoder>();
    if (!audio_decoder_->ok()) {
        if (err) *err = std::string("feeder Decoder 初始化失败：") + audio_decoder_->lastError();
        return false;
    }
    audio_cache_ = std::make_unique<BlockCache>(audio_decoder_.get(), mem_);
    // ★ 这份拷贝**继承 `X86::machine`（宿主侧 Machine 归属）**，是有意的：
    //   feeder 是同一台 Machine 的另一个 X86 视图（同一份 mem / com / 堆），若它执行到释放
    //   COM 对象的路径，回收必须落到同一个 SurfaceArena，而不是因为 `activeMachine()` 变空/易主
    //   就把 arena 永久漏掉（REQ-01M6X4MSCMMFEXYXCSK6V61QR2）。
    //   并发安全：所有容器访问都在 `ShimRegistry::invoke` 里发生，而它持 guest 令牌
    //   （见 runtime/guest_token.h 的不变式）—— feeder 的 guest 指令执行本身不持令牌。
    audio_cpu_ = *cpu_;
    if (cpu_->native_overrides) audio_overrides_ = *cpu_->native_overrides;
    // ★ 段级 override 的私有副本：**只装 `feeder_ok == true`** 的项（filter + add，不是整表 `operator=`）。
    //   整表拷会把 `sub_414AC0`（boot 期主线程装载器的中段）也带进 feeder —— feeder 永远走不到它，
    //   真走到了只会 Declined + 永久停用 + 一行 stderr 噪声。
    //   ⛔ 必须在 `audio_cpu_ = *cpu_` **之后**：那段拷贝会一并继承快照来源指针 `native_blocks`。
    //   ★ 这里只**读** Machine 的表（add 的源），写的是 `audio_block_overrides_`（本对象私有）。
    audio_block_overrides_.clear();
    if (cpu_->native_blocks != nullptr) {
        for (std::size_t i = 0; i < cpu_->native_blocks->size(); ++i) {
            const NativeBlockOverride* b = cpu_->native_blocks->at(i);
            if (b == nullptr || !b->feeder_ok) continue;
            std::string block_err;
            if (!audio_block_overrides_.add(b->entry_ea, b->resume_ea, b->name, b->fn, &block_err,
                                            /*feeder_ok=*/true)) {
                // 不致命（penalty = 性能退化，不是正确性）：但要**响亮**，否则 `bov=0` 没人解释得了。
                logAt(LogLevel::Warn, "feeder 段级 override 拷贝失败（%s）⇒ 这一项在 feeder 里走解释器",
                      block_err.c_str());
            }
        }
    }
    audio_cpu_.cache = audio_cache_.get();
    audio_cpu_.calls = &audio_calls_;
    audio_calls_.reset();
    audio_cpu_.audio = this;
    audio_cpu_.shim.thread_id = feeder_.thread_id;
    audio_cpu_.shim.last_error = 0;
    audio_cpu_.shim.timers.clear();
    audio_cpu_.shim.callback = GuestCallbackState{};
    audio_cpu_.shim.timer_firing = false;
    audio_cpu_.shim.frame_dump.clear();
    audio_cpu_.shim.max_frames = 0;
    audio_cpu_.insns = 0;
    audio_cpu_.running = true;
    audio_cpu_.stop_why = nullptr;
    audio_cpu_.fpu.reset();
    std::memset(audio_cpu_.xmm, 0, sizeof(audio_cpu_.xmm));
    // 预装启动时已有的 shim 块；运行期新增的由 `installFeederShim` / 按需 range 检查补齐。
    for (const ShimDef& d : cpu_->shims->snapshot()) {
        if (d.stub_ea != 0) audio_cache_->installShim(d.stub_ea, d.id);
    }
    return true;
}

void AudioEngine::startFeederThread() {
    if (feeder_thread_.joinable()) return;
    std::string err;
    if (!setupFeederContext(&err)) {
        logAt(LogLevel::Warn, "feeder 真线程启动失败（%s）⇒ 退回主线程泵", err.c_str());
        thread_mode_ = false;
        stopMixThread();
        return;
    }
    stop_requested_ = false;
    feeder_error_ = false;
    feeder_error_text_.clear();
    feeder_thread_ = std::thread(&AudioEngine::feederThreadMain, this);
}

void AudioEngine::feederThreadMain() {
    // ★ 登记/注销自己的线程 id（`isFeederThread()` 用）：⛔ 不要改成读 `feeder_thread_`，
    //   主线程会在 `joinFeeder()` 里 join 同一个对象（见 `isFeederThread` 的注释）。
    feeder_tid_.store(std::this_thread::get_id());
    for (;;) {
        AudioNotifyEvent ev;
        {
            std::unique_lock<std::mutex> lk(pending_mutex_);
            feeder_cv_.wait(lk, [this] { return stop_requested_ || !pending_.empty(); });
            if (stop_requested_) break;
            ev = pending_.front();
            pending_.pop_front();
        }
        feederEvent(ev);
        if (feeder_error_.load()) break;
    }
    feeder_tid_.store(std::thread::id{});
}

void AudioEngine::feederEvent(const AudioNotifyEvent& ev) {
    GuestToken& token = guestToken();
    // ★ 并发契约（guest_token.h）：feeder 是**真宿主线程**，它执行 guest 指令时与主线程并行；
    //   共享容器（GuestHeap / HandleTable / ComRegistry / Vfs / …）的互斥点在
    //   **每次 shim 调用**（`ShimRegistry::invoke`）与 **CS 字段**（本函数的 prologue/epilogue）。
    //   这里取令牌只覆盖"读槽 / 写 guest 可见的 CS 与在飞计数 / 建 feeder 上下文"这几段；
    //   中间整次 fill（`runFeederGuest`）**放开**令牌（否则 wall ≥ 两侧 CPU 之和，见报告）。
    //
    // ★★ "每条早退都有账"（本轮逐条点过；⛔ 以后改早退路径必须回来对账）：
    //   ① `findSlot` 失败                    → `stats_.slot_miss`（D4：obj 反演不到槽）
    //   ② `guestTryEnterCriticalSectionAs` 硬错 → `feeder_error_` + Warn 行（**致命**，不另设计数：
    //      它不静默 —— feeder 主循环随即退出）
    //   ③ 槽 CS 不在已提交内存                → 同 ②（致命、响亮）
    //   ④ 抢 CS 的 1 ms 重试期间 `stop_requested_` → 不计数（这是**我们主动不填**，不是通知丢了；
    //      `joinFeeder` 会打一行 Info）
    //   ⑤ 拿到 CS 之后 `stop_requested_`        → 同 ④（第二个检查点）
    //   ⑥ **槽重验失败**（下面 `rd32(...) != stream || rd32(stream+0x40C) != ev.obj`）
    //                                          → `stats_.slot_reverify_fail` ★ 本轮新增
    //   ⑦ 代次不符（Stop / 重定位抢先）          → `stats_.stale`
    //   ⑧ fill 之后 LeaveCS 失败 / `runFeederGuest` 失败 → `feeder_error_` + Warn 行（致命、响亮）
    //   ⇒ 除 ②③⑧ 三条**致命**路径（都会停下 feeder 并打日志）与 ④⑤（主动退出）外，每条早退都有计数。
    //   ★ ⑥ 以前是**唯一没有账**的那条：硬账的证据是 `notify`（入队数）比 `fills` 多，而
    //     `stale` / `dropped` / `slot_miss` 全 0 —— 那种差额只能落在这里。
    //
    // ★ 只读诊断（`LIFT_AUDIO_TRACE` 的 fill 时间分解；⛔ 不参与任何判定，只取墙钟）：
    //   `wait_ms`  = 入口 → **外层** `token.acquire()` 返回（等主线程放令牌）。
    //     ⚠ 口径上限：本函数是**已经 pop 之后**才进来的（`feederThreadMain` 先 pop 再调），
    //       所以"通知在队列里排了多久"**不在这里** —— 那一段要给入队打时间戳，本轮**不做**
    //       （只新增字段，不改 `AudioNotifyEvent`）。
    //   `pre_ms`   = 拿到令牌 → `fill_start`（findSlot + 抢槽 CS 的 1 ms 重试 + 建上下文）。
    //   `guest_ms` = `runFeederGuest()` 的墙钟（guest 指令 + 每次 shim 调用的**内层**取令牌等待）。
    //   `shimtok_ms` = `guest_ms` 里卡在 **shim 边界**等令牌的那部分（`shims.cpp` 里
    //     `t_shim_token_wait_ns` 的 thread_local 累计；本文件顶部有它的 extern 声明）。
    //   `shimblk`  = 上面那段账里的次数（这一刀有几次 shim 取令牌**真的被挡住**）。
    //   `total`    = 已有那个 `ms`（`fill_start` → trace 行；此时 guest 已跑完、环已写完、LeaveCS 之前）。
    const auto t_enter = std::chrono::steady_clock::now();
    token.acquire();
    const auto t_token = std::chrono::steady_clock::now();
    std::uint32_t slot = 0;
    std::uint32_t stream = 0;
    if (!findSlot(ev.obj, &slot, &stream)) {
        // ★ D4：API 不定义"位置事件 ↔ 哪个槽"，引擎的 obj→slot 反演（`&events[4*slot]`
        //   `0x4B6656` + `[Sound+0x654+4s]` `0x4B667F`）在换槽窗口里会失败。丢掉比填到已经
        //   换掉的 stream 更安全，但**必须留计数**，否则这条路径永久静默。
        //   ⛔ 先 `token.release()` 再取 `mutex_`：锁序并不禁止持令牌取 `mutex_`，真正的理由是
        //   **持有时间** —— 主线程的每个 shim 都要令牌，持令牌等 `mutex_`（混音器可能正持它做
        //   一次 `generateTo`）会把整条主线程拖住。
        token.release();
        {
            std::lock_guard<std::recursive_mutex> lk(mutex_);
            ++stats_.slot_miss;
            if (!warned_slot_miss_) {
                warned_slot_miss_ = true;
                logAt(LogLevel::Warn,
                      "feeder：通知的 obj=0x%08X 在 Sound+0x654 的 15 个槽里找不到（[stream+0x40C] "
                      "不匹配）⇒ 丢掉该 fill；之后同类只计 stats.slot_miss",
                      ev.obj);
            }
        }
        return;
    }
    const std::uint32_t cs = feeder_.sound + kSoundCsBase + 24u * slot;
    std::string err;
    // ★ 真机 feeder 在 EnterCriticalSection 上**阻塞**。本模拟器的阻塞必须发生在**不持令牌**
    //   的时候：持令牌阻塞 = 主线程等不到令牌、feeder 等不到 CS ⇒ 死锁。
    //   拿不到就放开令牌睡 1 ms 再试（主线程会在某个块边界放开那个槽的 CS）。
    for (;;) {
        if (guestTryEnterCriticalSectionAs(audio_cpu_, cs, feeder_.thread_id, &err)) break;
        // ★ 只有"宿主锁正被另一线程持有"才是**可重试的争用**（try_lock 失败，err 为空）。
        //   `enter()` 的非争用失败（guest 账本与宿主锁不一致 / 临界区未提交）是硬错：
        //   以前会响亮失败，⛔ 不要把它降级成 1 ms 无限空转（err 会被丢弃）。
        if (!err.empty()) {
            token.release();
            feeder_error_ = true;
            feeder_error_text_ = "feeder 进入槽 CS 失败：" + err;
            logAt(LogLevel::Warn, "%s", feeder_error_text_.c_str());
            return;
        }
        if (!mem_->committed(cs, 0x18)) {
            token.release();
            feeder_error_ = true;
            feeder_error_text_ = "feeder 进入槽 CS 失败：临界区不在已提交内存里";
            logAt(LogLevel::Warn, "%s", feeder_error_text_.c_str());
            return;
        }
        {
            std::lock_guard<std::mutex> lk(pending_mutex_);
            if (stop_requested_) {
                token.release();
                return;
            }
        }
        GuestToken::YieldGuard yield(token);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        if (stop_requested_) {
            // 已经被要求退出：不要再填这一圈（joinFeeder 正在等我放锁）。
            std::string leave_err;
            guestLeaveCriticalSectionAs(audio_cpu_, cs, feeder_.thread_id, &leave_err);
            token.release();
            return;
        }
    }
    // ★ 上面的 findSlot 是**进 CS 之前**的扫描；主线程可能刚换了槽。拿到 CS 后必须重验，
    //   否则会填到已经换掉的 stream（真机上 feeder 是拿到 CS 后才读槽指针的）。
    //   ★ 这一刀**丢掉**（不写环、不 +在飞）—— 判定与以前**逐字相同**，只是**补了一个计数**
    //     `slot_reverify_fail`（见本函数开头的早退清单 ⑥：它是原先唯一没有账的早退）。
    if (mem_->rd32(feeder_.sound + kSoundSlots + 4u * slot) != stream ||
        mem_->rd32(stream + kStreamDsBuffer) != ev.obj) {
        {
            std::lock_guard<std::recursive_mutex> lk(mutex_);  // `stats_` 归 `mutex_`（dumpSummary 也读它）
            ++stats_.slot_reverify_fail;
        }
        std::string leave_err;
        guestLeaveCriticalSectionAs(audio_cpu_, cs, feeder_.thread_id, &leave_err);
        token.release();
        return;
    }
    // ★★ D1：**拿到槽 CS 之后**再查一次该 obj 的代次（`findByObj` 走递归 `mutex_`；本处持令牌，
    //   锁序令牌 → `mutex_` 与每个 DS shim 相同）。这条关掉"pop 之后、拿 CS 之前被主线程
    //   Stop/重定位"的窗口：条目在 pop 时还是有效代次，但拿到 CS 之前主线程已经
    //   Stop / SetCurrentPosition（`++generation`）⇒ 这一刀 fill 属于**旧位置**，跑了就会把
    //   BGM 写回旧相位。⇒ 不跑 fill、不碰环、不 +在飞，只留计数并放掉 CS。
    //   ★ 置 `filling_obj_` 必须与这次代次校验**在同一把 `mutex_`** 下：`stop()` 的
    //   `++generation` 与"要不要等 filling_obj_ == obj"也在同一把锁下 ⇒ 两者不会交错，
    //   否则会留下"校验过了但标志还没置"的窗口，主线程就会以为没有 fill 在跑。
    bool stale = false;
    {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        const Buffer* gb = findByObj(ev.obj);  // 递归锁：这里已经持 `mutex_`
        if (gb == nullptr || gb->generation != ev.generation) {
            ++stats_.stale;
            stale = true;
        } else {
            filling_obj_.store(ev.obj);
        }
    }
    if (stale) {
        std::string leave_err;
        guestLeaveCriticalSectionAs(audio_cpu_, cs, feeder_.thread_id, &leave_err);
        token.release();
        return;
    }
    const std::uint32_t inf_ea = feeder_.sound + kSoundInflight + 4u * slot;
    const std::uint32_t inf = mem_->committed(inf_ea, 4) ? mem_->rd32(inf_ea) : 0u;
    if (mem_->committed(inf_ea, 4)) mem_->wr32(inf_ea, inf + 1u);

    std::memset(audio_cpu_.r, 0, sizeof(audio_cpu_.r));
    audio_cpu_.esp() = audio_stack_top_ - 8u;
    mem_->wr32(audio_cpu_.esp(), kFeederReturnEa);
    mem_->wr32(audio_cpu_.esp() + 4u, ev.quarter);
    audio_cpu_.ecx() = stream;
    audio_cpu_.eip = kSub4B6C30;
    audio_cpu_.eflags = 0x202u;
    audio_cpu_.fpu.reset();
    std::memset(audio_cpu_.xmm, 0, sizeof(audio_cpu_.xmm));
    audio_cpu_.shim.thread_id = feeder_.thread_id;
    audio_cpu_.shim.last_error = 0;
    audio_cpu_.running = true;
    audio_cpu_.stop_why = nullptr;

    std::string run_err;
    const auto fill_start = std::chrono::steady_clock::now();
    // ★ 只读诊断（`late_q`）：**fill 开始**那一刻的滞后量（公式与"为什么带符号/不归一"见
    //   `fillLagQuarter` 的定义注释）。⛔ 纯读：`buffers_` + `played_`，不发通知、不改状态。
    std::uint32_t lag_bytes = 0;
    double lag_q = 0.0;
    const bool lag_ok = fillLagQuarter(ev.obj, ev.quarter, &lag_bytes, &lag_q);
    // ★ 只读诊断：`guest_ms` 里"卡在 shim 边界等令牌"的基线（thread_local，本线程）。
    const std::uint64_t shimtok_ns_begin = t_shim_token_wait_ns;
    const std::uint64_t shimtok_n_begin = t_shim_token_wait_count;
    // ★ 并发契约（guest_token.h）：feeder 的**guest 指令执行**不持令牌（主线程同时跑自己的块）；
    //   它每次 **shim 调用**由 `ShimRegistry::invoke` 取令牌 —— 所有 guest 可见容器的访问都在
    //   那里，所以"容器临界区"仍然互斥。⛔ 整次 fill 持令牌 = 单核串行：实测 wall ≥ 两侧 CPU
    //   之和（主线程 13.8 s + feeder 19.0 s ≈ 32.8 s CPU），而并行基线 wall 15.1 s。
    token.release();
    const bool ok = runFeederGuest(&run_err);
    const auto t_guest_done = std::chrono::steady_clock::now();
    const std::uint64_t guest_shimtok_ns = t_shim_token_wait_ns - shimtok_ns_begin;
    const std::uint64_t guest_shimtok_n = t_shim_token_wait_count - shimtok_n_begin;
    token.acquire();  // 回到令牌里再改 guest 可见状态（在飞计数 / CS 字段）
    // ★ 金丝雀（S1）：fill 结束后该 obj 的代次是不是还是当初那一代。**不一致 = 有一次 Stop /
    //   重定位与这一刀在时间上重叠**。★ 注意：`stop()` 是"先 `++generation` 再等" ⇒ 被第 3 层
    //   正常等住的那一次也会让它 +1，所以它**不是错误计数**（生产里可以是 > 0）。
    //   ★ 它还混了三种**良性**成因（都不是"没兜住"）：① fill 里的 EOF 自停（`0x4B6C58`
    //   会在 fill 内调 `stop()`）、② `removeBuffer`（Release 把这条流删了 ⇒ `findByObj` 为空）、
    //   ③ 同一 state 重建（`addBuffer` 的 `++generation`）。所以**不要**按"它非 0 ⇒ 有错"去读。
    //   真正"没兜住"的判据是 `fill_wait_timeouts > 0`（那一次的 re-anchor 与 fill 真的并发）。
    //   ⛔ 只记账，不回滚已写进环的字节。
    {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        const Buffer* overlapped = findByObj(ev.obj);  // 递归锁：这里已经持 `mutex_`
        if (overlapped == nullptr || overlapped->generation != ev.generation) ++stats_.fill_overlapped;
    }
    // ★ 清"正在跑"标志：到这一行为止 guest 环的写入已经结束，剩下的 trace / `--在飞` /
    //   LeaveCS 属于收尾，不在 `stop()` 的等待范围内。存 0 与 notify 都在 `pending_mutex_` 下，
    //   等的人也用同一把锁 ⇒ 不会丢唤醒。所有早退路径（`!ok` / LeaveCS 失败）都在这之后，
    //   所以标志不会漏清。
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        filling_obj_.store(0);
    }
    pending_cv_.notify_all();
    // ★ 只读诊断计数（dumpSummary 的 `fill_max_ms` / `fill_slow`）：在 `fill_start` → 这里
    //   这一段上记账（与 trace 里那个 `total` 同一段）。⛔ 不参与判定；`fill_slow` 的门槛
    //   固定 1000 ms（**环长 2 s、单刀 > 1 s ⇒ 已经在吃掉一圈的余量**，是诊断阈值不是判据）。
    const auto t_trace = std::chrono::steady_clock::now();
    const std::uint64_t fill_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t_trace - fill_start).count());
    if (std::getenv("LIFT_AUDIO_TRACE") != nullptr) {
        const double total_ms = static_cast<double>(fill_us) / 1000.0;
        const double wait_ms =
            std::chrono::duration<double, std::milli>(t_token - t_enter).count();
        const double pre_ms = std::chrono::duration<double, std::milli>(fill_start - t_token).count();
        const double guest_ms = std::chrono::duration<double, std::milli>(t_guest_done - fill_start).count();
        const double shimtok_ms = static_cast<double>(guest_shimtok_ns) / 1.0e6;
        std::uint64_t ov_hits = 0;
        for (std::size_t i = 0; i < audio_overrides_.size(); ++i) {
            if (const NativeOverride* ov = audio_overrides_.at(i)) ov_hits += ov->hits;
        }
        // ★ 段级 override 的**独立计数器**（`bov= / bovd= / bovf=`）：`audio_block_overrides_` 的
        //   私有副本汇总，⛔ 不是主表（`native-ovr:` 只汇总主表；`ov=` 也只是函数级私有表的 hits）。
        //   ★ 为什么两个读数必须分开（`docs/audio-implementation.md` 的"两个假绿陷阱"）：
        //     ① `ov=` 会**下降** —— PCM 段把每样本一次的 `__ftol2_sse` 函数级命中吃掉了
        //        ⇒ 盯 `ov=` 会得出"没生效"的反向结论；
        //     ② 段级在 feeder 里没有别的读数通道 ⇒ `bov=0` 就是"没生效"（或 `feeder_ok` filter 漏了）。
        //   ★ 计数在 feeder 线程上写、也在同一条线程（fill 收尾）上读 ⇒ 无竞争。
        //   ★ 量级：每刀 fill 只跑一个 quarter 的一段解码 ⇒ 实测 `bov` **每刀约 48–55**
        //     （ch=2；随 ch / 每 quarter 的解码包数变）⇒ ⛔ 不是"每样本一次"，也⛔ 不是"每刀 = ch"。
        std::uint64_t bov_hits = 0;
        std::uint64_t bov_declines = 0;
        std::uint64_t bov_fallbacks = 0;
        for (std::size_t i = 0; i < audio_block_overrides_.size(); ++i) {
            if (const NativeBlockOverride* bo = audio_block_overrides_.at(i)) {
                bov_hits += bo->hits;
                bov_declines += bo->declines;
                bov_fallbacks += bo->fallbacks;
            }
        }
        // ★ 只读诊断：`[stream+0x2450/0x2458/0x2470]` 是 B1 的判据；**读不到就打 `?`**（⛔ 不打 0）。
        char f2450[16];
        char f2458[16];
        char f2470[16];
        char bytes_txt[16];
        char late_txt[24];
        traceDword(mem_, stream + 0x2450u, f2450, sizeof f2450);
        traceDword(mem_, stream + 0x2458u, f2458, sizeof f2458);
        traceDword(mem_, stream + 0x2470u, f2470, sizeof f2470);
        if (lag_ok) {
            std::snprintf(bytes_txt, sizeof bytes_txt, "%u", lag_bytes);
            std::snprintf(late_txt, sizeof late_txt, "%0.2f", lag_q);
        } else {
            std::snprintf(bytes_txt, sizeof bytes_txt, "?");
            std::snprintf(late_txt, sizeof late_txt, "?");
        }
        std::fprintf(stderr,
                     "[audio] feeder fill slot=%u quarter=%u obj=0x%08X bytes=%s 2450=%s 2458=%s 2470=%s "
                     "late_q=%s wait=%.2f pre=%.2f guest=%.2f shimtok=%.2f shimblk=%llu total=%.2f ms ov=%llu "
                     "bov=%llu bovd=%llu bovf=%llu%s\n",
                     slot, ev.quarter, ev.obj, bytes_txt, f2450, f2458, f2470, late_txt, wait_ms, pre_ms, guest_ms,
                     shimtok_ms, static_cast<unsigned long long>(guest_shimtok_n), total_ms,
                     static_cast<unsigned long long>(ov_hits), static_cast<unsigned long long>(bov_hits),
                     static_cast<unsigned long long>(bov_declines), static_cast<unsigned long long>(bov_fallbacks),
                     ok ? "" : " (FAILED)");
    }
    if (mem_->committed(inf_ea, 4)) {
        const std::uint32_t cur = mem_->rd32(inf_ea);
        if (cur > 0) mem_->wr32(inf_ea, cur - 1u);
    }
    std::string leave_err;
    if (!guestLeaveCriticalSectionAs(audio_cpu_, cs, feeder_.thread_id, &leave_err)) {
        token.release();
        feeder_error_ = true;
        feeder_error_text_ = "feeder 离开槽 CS 失败：" + leave_err;
        return;
    }
    token.release();
    if (!ok) {
        feeder_error_ = true;
        feeder_error_text_ = "feeder guest 执行失败：" + run_err;
        logAt(LogLevel::Warn, "%s", feeder_error_text_.c_str());
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lk(mutex_);
        ++stats_.fills;
        // ★ 只读诊断计数（与 `fills` 同一处、同一口径：**只统计跑完的这一刀**）。
        stats_.fill_max_us = std::max(stats_.fill_max_us, fill_us);
        if (fill_us > 1000000ull) ++stats_.fill_slow;
        if (mem_->committed(stream + kStreamBytes, 4)) stats_.fill_bytes += mem_->rd32(stream + kStreamBytes) / 4u;
    }
}

bool AudioEngine::runFeederGuest(std::string* err) {
    const std::uint32_t page = audio_cpu_.shims ? audio_cpu_.shims->stubPage() : 0u;
    const std::uint32_t slots = audio_cpu_.shims ? audio_cpu_.shims->stubSlots() : 0u;
    std::uint32_t next_call_target = 0;
    for (;;) {
        // ★ 这里**不持令牌**：feeder 的 guest 指令执行与主线程并行；共享容器的互斥在
        //   `ShimRegistry::invoke`（每次 shim 调用）与 `guestEnterCriticalSectionAs`（CS 字段）里。
        //   ⛔ 不要在这里加 `guestToken().check` —— 它会把"合法的并行 guest 执行"判成违规。
        if (audio_cpu_.eip == kFeederReturnEa) return true;
        if (!audio_cpu_.running) {
            if (err) *err = audio_cpu_.stop_why ? audio_cpu_.stop_why : "guest 主动停止";
            return false;
        }
        // 函数级 native override（feeder 有私有副本；_memcpy/_memset 是 Ogg 解码的热路径）。
        if (next_call_target != 0 && audio_cpu_.eip == next_call_target) {
            next_call_target = 0;
            if (NativeOverride* ov = audio_overrides_.find(audio_cpu_.eip)) {
                const char* why = nullptr;
                bool suspended = false;
                if (audio_overrides_.invoke(*ov, audio_cpu_, *mem_, &why, &suspended)) {
                    if (suspended) {
                        if (err) *err = "feeder 里的 native override 试图再铺 guest 帧（不支持）";
                        return false;
                    }
                    continue;
                }
            }
        }
        // ★ 段级 native override（**函数中段**的固定 EA）：feeder 的**私有副本**。
        //   插在函数级之后、`audio_cache_->get()` 之前（与主线程 dispatcher 同序：段级在块取之前）。
        //   ⛔ 不解引用 `audio_cpu_.native_blocks` —— 那指向 Machine 的表（快照来源），
        //      `hits / declines / fallbacks / disabled` 都是非原子裸字段：共享 = 数据竞争，且
        //      feeder 的一次 `Declined` 会 `disabled = true` **永久停用主线程**那一项。
        //   ⛔ 不 tick 时钟：`VirtualClock::now_100ns_` 非原子（`clock.h`），主线程在同一对象上
        //      `tick(1)` ⇒ 无锁 RMW = UB；而 real-time 下 `now100ns()` 根本不读它 ⇒ 零收益。
        //   ⛔ 不碰 `window_prof_`（主线程独占的非原子容器）、⛔ 不加 `noteInsn` / trace 打印
        //      （feeder 的块执行路径本来就不做；那是热路径，不是语义差）。
        //   ★ `audio_cpu_.insns += n` 保留：feeder 侧它今天没有读者（只对齐语义），读数看 `guest=`。
        //
        //   ★ `next_call_target` 的坑：命中段级的那一轮它**必须是 0**。段入口不是 call 目标
        //      （`0x48D767` 是 fall-through + 回边进来的）。`Replaced` 之后 `continue` 不会再消费它；
        //      若将来给"被 call 的段"做 override，`call_target` 会先在上面那个函数级 `if` 里被清 0
        //      （那一支是无条件清），所以形状仍然成立 —— 但别把这段挪到那个 `if` 之前。
        if (!audio_block_overrides_.empty()) {
            if (NativeBlockOverride* blk = audio_block_overrides_.find(audio_cpu_.eip)) {
                const std::uint32_t entry = audio_cpu_.eip;
                std::uint64_t replaced = 0;
                const BlockOverrideOutcome outcome = blk->fn(audio_cpu_, *mem_, &replaced);
                if (outcome == BlockOverrideOutcome::Replaced) {
                    ++blk->hits;
                    // `replaced == 0` 视为"没报"⇒ 记 1 条（照抄主线程 dispatcher 的兜底；
                    // PCM 段保证 replaced >= 15*n，见 native_pcm.cpp）。
                    audio_cpu_.insns += (replaced != 0 ? replaced : 1ull);
                    continue;
                }
                if (outcome == BlockOverrideOutcome::Declined) {
                    // 与主线程同口径：**第一次** Declined ⇒ 计入 declines + 永久停用该项（私有副本上），
                    // 打一行 stderr 后本次落回解释器。之后 `find` 不再返回它。
                    if (!blk->disabled) {
                        ++blk->declines;
                        blk->disabled = true;
                        std::fprintf(stderr,
                                     "[audio] feeder 段级 %s @ %08X 前置条件不匹配 ⇒ 本次运行停用该项"
                                     "（回退解释器，主线程那一项不受影响）\n",
                                     blk->name.c_str(), entry);
                    }
                } else {
                    // Failed：不 disable（下一次块入口还会重试；保正确性），stderr 只一行。
                    if (blk->fallbacks == 0)
                        std::fprintf(stderr, "[audio] feeder 段级 %s @ %08X 执行失败 ⇒ 回退解释器\n",
                                     blk->name.c_str(), entry);
                    ++blk->fallbacks;
                }
            }
        }
        // 运行期新增的 COM/DS vtable stub：第一次落到它时补进私有 cache。
        if (page != 0 && audio_cpu_.eip >= page && audio_cpu_.eip < page + slots * 4u) {
            bool missing = false;
            {
                std::lock_guard<std::mutex> lock(feeder_cache_mutex_);
                missing = audio_cache_->peek(audio_cpu_.eip) == nullptr;
            }
            if (missing) {
                const std::uint32_t id = audio_cpu_.shims->idForStub(audio_cpu_.eip);
                if (id != 0) {
                    std::lock_guard<std::mutex> lock(feeder_cache_mutex_);
                    audio_cache_->installShim(audio_cpu_.eip, id);
                }
            }
        }
        Block* b = nullptr;
        {
            std::lock_guard<std::mutex> lock(feeder_cache_mutex_);
            b = audio_cache_->get(audio_cpu_.eip);
        }
        if (b == nullptr) {
            if (err) *err = audio_cache_->lastError();
            return false;
        }
        if (b->is_shim) {
            if (!audio_cpu_.shims->invokeById(audio_cpu_, b->shim_id)) {
                if (err) *err = "shim 块指向不存在的 id";
                return false;
            }
            continue;
        }
        for (const Insn& in : b->insns) {
            const bool control = opIsControl(in.op);
            audio_cpu_.eip = in.ea;
            if (!control) audio_cpu_.eip = in.ea + in.length;
            if (!execInsn(audio_cpu_, in)) {
                if (err) *err = audio_cpu_.stop_why ? audio_cpu_.stop_why : "指令执行失败";
                return false;
            }
            ++audio_cpu_.insns;
            if (control) {
                next_call_target = (in.op == Op::Call) ? audio_cpu_.eip : 0;
                break;
            }
        }
    }
}

void AudioEngine::dumpSummary(std::FILE* out) const {
    if (!out) return;
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    // `device` 把"没有设备"与"静音"分开：ok = SDL 真设备；fallback = sdl 打不开退回 virtual；
    // none = virtual / off（本来就没有宿主设备）。
    const char* device = sdl_device_opened_ ? "ok" : (sdl_fallback_ ? "fallback" : "none");
    std::fprintf(out,
                 "audio     : mode=%s device=%s muted=%d muted_bytes=%llu fills=%llu fill_bytes=%llu "
                 "mixed=%llu played=%llu notify=%llu deferred=%llu dropped=%llu underruns=%llu "
                 "stale=%llu slot_miss=%llu play_on_playing=%llu fill_waits=%llu "
                 "fill_wait_timeouts=%llu fill_overlapped=%llu"
                 // ★ 本轮新增的五个**只读观测**（放在末尾；⛔ 原有字段顺序没动）：
                 //   slot_reverify_fail = feeder 拿到槽 CS 后重验失败（见 audio.h 的说明）；
                 //   fill_max_ms / fill_slow = 单刀 fill 的最大墙钟与 >1000 ms 的刀数；
                 //   fill_wait_ms / fill_wait_max_ms = 第 3 层"等正在跑的那一刀"的累计 / 单次
                 //     最大墙钟（口径 = 只量谓词等待，不含重取 `mutex_`；见 `waitFill`）。
                 " slot_reverify_fail=%llu fill_max_ms=%.2f fill_slow=%llu"
                 " fill_wait_ms=%.2f fill_wait_max_ms=%.2f%s\n",
                 audioModeName(mode_), device, mute_ ? 1 : 0,
                 static_cast<unsigned long long>(stats_.muted_bytes),
                 static_cast<unsigned long long>(stats_.fills),
                 static_cast<unsigned long long>(stats_.fill_bytes),
                 static_cast<unsigned long long>(stats_.mixed_bytes),
                 static_cast<unsigned long long>(stats_.played_bytes),
                 static_cast<unsigned long long>(stats_.notifications),
                 static_cast<unsigned long long>(stats_.deferred),
                 static_cast<unsigned long long>(stats_.dropped),
                 static_cast<unsigned long long>(stats_.underruns),
                 static_cast<unsigned long long>(stats_.stale),
                 static_cast<unsigned long long>(stats_.slot_miss),
                 static_cast<unsigned long long>(stats_.play_on_playing),
                 static_cast<unsigned long long>(stats_.fill_waits),
                 static_cast<unsigned long long>(stats_.fill_wait_timeouts),
                 static_cast<unsigned long long>(stats_.fill_overlapped),
                 static_cast<unsigned long long>(stats_.slot_reverify_fail),
                 static_cast<double>(stats_.fill_max_us) / 1000.0,
                 static_cast<unsigned long long>(stats_.fill_slow),
                 static_cast<double>(stats_.fill_wait_us) / 1000.0,
                 static_cast<double>(stats_.fill_wait_max_us) / 1000.0,
                 dump_path_.empty() ? "" : (" dump=" + dump_path_).c_str());
}

}  // namespace lift
