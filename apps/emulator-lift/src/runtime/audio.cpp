#include "runtime/audio.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
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

void AudioEngine::addBuffer(std::uint32_t state_ea, std::uint32_t obj_ea, std::uint32_t data_ea,
                            std::uint32_t bytes, std::uint32_t flags, const AudioFormat& fmt) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!enabled() || state_ea == 0) return;
    Buffer* b = find(state_ea);
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
    b->initial_pos = 0;
    b->start_mixed = generated_;
    b->notify_count = 0;
    b->volume = 0;
    b->pan = 0;
    stats_.max_buffers = std::max(stats_.max_buffers, static_cast<std::uint32_t>(buffers_.size()));
}

void AudioEngine::setFormat(std::uint32_t state_ea, const AudioFormat& fmt) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    if (b) b->fmt = fmt;
}

void AudioEngine::removeBuffer(std::uint32_t state_ea) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    for (auto it = buffers_.begin(); it != buffers_.end(); ++it) {
        if (it->state_ea != state_ea) continue;
        const std::uint32_t obj = it->obj_ea;
        buffers_.erase(it);
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(),
                                      [obj](const AudioNotifyEvent& e) { return e.obj == obj; }),
                       pending_.end());
        return;
    }
}

void AudioEngine::setNotify(std::uint32_t state_ea, std::uint32_t count, const std::uint32_t* offsets) {
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    if (b == nullptr) return;
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
        if (b->notify_off[i] == b->initial_pos) enqueue(b->obj_ea, i);
    }
}

void AudioEngine::stop(std::uint32_t state_ea) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    if (b == nullptr) return;
    // 真 DS：Stop 冻结当前播放位置；再 Play 从冻结处继续（不是回到 initial_pos）。
    b->initial_pos = playedCursor(state_ea);
    b->playing = false;
    if (std::getenv("LIFT_AUDIO_TRACE") != nullptr) {
        logAt(LogLevel::Info, "stop state=0x%08X obj=0x%08X cursor=%u", state_ea, b->obj_ea, b->initial_pos);
    }
}

void AudioEngine::setPosition(std::uint32_t state_ea, std::uint32_t pos) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    if (b == nullptr) return;
    b->initial_pos = b->bytes ? (pos % b->bytes) : 0u;
    const std::uint32_t align = b->fmt.block_align ? b->fmt.block_align : 4u;
    b->initial_pos -= b->initial_pos % align;  // 按**该缓冲**的帧对齐（mono 是 2，不是 4）
    if (b->playing) {
        // 真机 SetCurrentPosition 会让游标跳到新位置；引擎只在 Play 前调它，这里按同一时间线重排。
        b->start_mixed = generated_;
        for (std::uint32_t i = 0; i < b->notify_count; ++i) {
            if (b->notify_off[i] == b->initial_pos) enqueue(b->obj_ea, i);
        }
    }
}

void AudioEngine::setVolume(std::uint32_t state_ea, std::int32_t centibel) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    if (b) b->volume = std::clamp<std::int32_t>(centibel, -10000, 0);
}

void AudioEngine::setPan(std::uint32_t state_ea, std::int32_t pan) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    Buffer* b = find(state_ea);
    // DSBPAN_LEFT/RIGHT = ±10000（1/100 dB 的左右偏置）。
    if (b) b->pan = std::clamp<std::int32_t>(pan, -10000, 10000);
}

std::uint32_t AudioEngine::playedCursor(std::uint32_t state_ea) const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    const Buffer* b = find(state_ea);
    if (b == nullptr || b->bytes == 0) return 0;
    if (!b->playing) return b->initial_pos % b->bytes;
    const std::uint64_t start_frame = b->start_mixed / kBlockAlign;
    const std::uint64_t played_frame = played_ / kBlockAlign;
    if (played_frame <= start_frame) return b->initial_pos % b->bytes;
    const std::uint64_t src_elapsed = (played_frame - start_frame) * b->fmt.avg_bytes / kSampleRate;
    return static_cast<std::uint32_t>((b->initial_pos + src_elapsed) % b->bytes);
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

void AudioEngine::enqueue(std::uint32_t obj_ea, std::uint32_t quarter) {
    if (obj_ea == 0 || quarter > 3) return;
    {
        std::lock_guard<std::mutex> lk(pending_mutex_);
        for (const auto& e : pending_)
            if (e.obj == obj_ea && e.quarter == quarter) return;  // 自动复位事件语义：重复 SetEvent 合并
        if (pending_.size() >= 256) {
            pending_.pop_front();
            ++stats_.dropped;
        }
        pending_.push_back(AudioNotifyEvent{obj_ea, quarter});
        ++stats_.notifications;
    }
    // 真线程模式：唤醒 feeder；泵模式没有等待者，notify 无害。
    feeder_cv_.notify_one();
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
        for (const auto& c : crosses) enqueue(b.obj_ea, c.second);
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
    // ★ 真线程模式：主线程**不要**每块都去抢 audio mutex（那会和 mixer/feeder 形成
    //   cache-line ping-pong，实测把 fill 从 400ms 拖到 650ms）。直接无锁返回。
    if (thread_mode_) {
        if (feeder_error_.load()) {
            last_error_ = "音频 feeder 线程失败（详见 stderr）";
            return AudioPump::Error;
        }
        return AudioPump::Idle;
    }
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
        const bool started = startNestedGuestCall(
            *cpu_, kSub4B6C30, {quarter}, 0u, "audio:feeder",
            [self, slot, quarter, stream, cs, inf_ea, fill_start](X86& c, std::uint32_t cb_result,
                                                                  std::uint32_t* out) {
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
                if (std::getenv("LIFT_AUDIO_TRACE") != nullptr) {
                    const double ms =
                        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fill_start)
                            .count();
                    std::fprintf(stderr,
                                 "[audio] fill slot=%u quarter=%u ret=%u state=%u wrote=%u L=%u %.2f ms\n",
                                 slot, quarter, cb_result,
                                 c.mem->committed(stream + 0x2458, 4) ? c.mem->rd32(stream + 0x2458) : 0u,
                                 c.mem->committed(stream + 0x2468, 4) ? c.mem->rd32(stream + 0x2468) : 0u,
                                 c.mem->committed(stream + kStreamBytes, 4) ? c.mem->rd32(stream + kStreamBytes) : 0u,
                                 ms);
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
}

void AudioEngine::feederEvent(const AudioNotifyEvent& ev) {
    GuestToken& token = guestToken();
    // ★ 并发契约（guest_token.h）：feeder 是**真宿主线程**，它执行 guest 指令时与主线程并行；
    //   共享容器（GuestHeap / HandleTable / ComRegistry / Vfs / …）的互斥点在
    //   **每次 shim 调用**（`ShimRegistry::invoke`）与 **CS 字段**（本函数的 prologue/epilogue）。
    //   这里取令牌只覆盖"读槽 / 写 guest 可见的 CS 与在飞计数 / 建 feeder 上下文"这几段；
    //   中间整次 fill（`runFeederGuest`）**放开**令牌（否则 wall ≥ 两侧 CPU 之和，见报告）。
    token.acquire();
    std::uint32_t slot = 0;
    std::uint32_t stream = 0;
    if (!findSlot(ev.obj, &slot, &stream)) {
        token.release();
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
    if (mem_->rd32(feeder_.sound + kSoundSlots + 4u * slot) != stream ||
        mem_->rd32(stream + kStreamDsBuffer) != ev.obj) {
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
    // ★ 并发契约（guest_token.h）：feeder 的**guest 指令执行**不持令牌（主线程同时跑自己的块）；
    //   它每次 **shim 调用**由 `ShimRegistry::invoke` 取令牌 —— 所有 guest 可见容器的访问都在
    //   那里，所以"容器临界区"仍然互斥。⛔ 整次 fill 持令牌 = 单核串行：实测 wall ≥ 两侧 CPU
    //   之和（主线程 13.8 s + feeder 19.0 s ≈ 32.8 s CPU），而并行基线 wall 15.1 s。
    token.release();
    const bool ok = runFeederGuest(&run_err);
    token.acquire();  // 回到令牌里再改 guest 可见状态（在飞计数 / CS 字段）
    if (std::getenv("LIFT_AUDIO_TRACE") != nullptr) {
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - fill_start).count();
        std::uint64_t ov_hits = 0;
        for (std::size_t i = 0; i < audio_overrides_.size(); ++i) {
            if (const NativeOverride* ov = audio_overrides_.at(i)) ov_hits += ov->hits;
        }
        std::fprintf(stderr, "[audio] feeder fill slot=%u quarter=%u %.2f ms ov=%llu%s\n", slot, ev.quarter, ms,
                     static_cast<unsigned long long>(ov_hits), ok ? "" : " (FAILED)");
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
                 "mixed=%llu played=%llu notify=%llu deferred=%llu dropped=%llu underruns=%llu%s\n",
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
                 dump_path_.empty() ? "" : (" dump=" + dump_path_).c_str());
}

}  // namespace lift
