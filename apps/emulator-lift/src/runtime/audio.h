// emulator-lift · 音频设备模型 + 真 feeder 线程（方案 C；virtual/无 real-time 回退方案 D 泵）
//
// ★ 口径（docs/thread-model-feasibility.md §5.1 / docs/audio-feasibility.md）：
//   - 引擎的音频喂料线程 `sub_4B5AA0` **不真跑**：`CreateThread(0x4B5AA0)` 只登记身份，
//     `ResumeThread` 把泵置为 runnable；线程体里的 12 段 native + 1 段 guest
//     （`0x4B5B03 call sub_4B6C30`）由本类在 **dispatcher 块边界**重新排成泵。
//   - ⛔ 不建第二宿主线程、不建第二 guest 栈 ⇒ 没有并发访存，arm64 与 x86 同结果。
//   - 设备模型（游标 / 4 个位置通知 / 状态机 / 混音）在宿主；PCM 仍由 guest 解码器产生。
//   - `sub_4B6C30` 用现有 `startNestedGuestCall()` 调（thiscall，ECX=stream，1 个栈参数 quarter）。
//   - 泵在进入/退出一次 fill 时做**整份 guest 上下文交换**（GPR / EFLAGS / x87 / XMM / 段 /
//     fs/gs / thread_id / last_error）。⛔ 只存 8 个 GPR 不够（Ogg 解码用浮点）。
//
// ★ 时钟档：
//   - `Virtual`：游标由 `VirtualClock` 驱动（100ns × 44100 / 10^7），确定性；CTest / dump 用。
//   - `Sdl`：游标由宿主设备"已消费字节"驱动（`pushed - queued`）；真正出声。
//   - 两种档位下通知点都由**游标越过**产生，且只在块边界投递给泵（没有宿主音频回调碰 guest 内存）。
//
// ★ 数据通路：宿主主线程混音（读 guest 环）→ `SDL_PutAudioStreamData`。SDL 的内部线程只碰
//   自己的队列，**从不读 guest 内存** ⇒ 即使 arm64 也不需要为音频做内存序模拟。
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "runtime/guest_call.h"
#include "runtime/native_overrides.h"
#include "runtime/x86.h"

namespace lift {

class GuestMemory;
class VirtualClock;
class Decoder;
class BlockCache;
enum class LogLevel : int;

enum class AudioMode : std::uint8_t {
    Off = 0,      // 不出声、不跑泵（旧 silent 口径）
    Virtual = 1,  // 虚拟设备时钟（确定性；可 dump）
    Sdl = 2,      // SDL3 真设备（能听到）
};

const char* audioModeName(AudioMode m);

enum class AudioPump : std::uint8_t {
    Idle = 0,       // 没有要做的 / 已经做完了
    Suspended = 1,  // 已经铺好 `sub_4B6C30` 的 guest 帧；调用方必须 continue
    Error = 2,      // 铺帧 / 参数失败；调用方按 `lastError()` 停下
};

// 泵要处理的一条位置通知（顺序 = 设备游标越过通知点的顺序）。
struct AudioNotifyEvent {
    std::uint32_t obj = 0;      // IDirectSoundBuffer8 的 guest EA
    std::uint32_t quarter = 0;  // 0..3（引擎 0x4B5AFF 的 `esi & 3`）
};

// 每条 DS 缓冲自己的 WAVEFORMATEX（引擎真的会建 44.1k mono / 48k stereo 两种；见实跑日志）。
// 宿主输出固定 48k / 16 bit / 2 ch；这里按采样率 / 声道做**逐缓冲**转换。
struct AudioFormat {
    std::uint32_t sample_rate = 48000;
    std::uint32_t channels = 2;
    std::uint32_t bits = 16;
    std::uint32_t block_align = 4;
    std::uint32_t avg_bytes = 192000;
};

struct AudioStats {
    std::uint64_t played_bytes = 0;
    std::uint64_t mixed_bytes = 0;
    std::uint64_t pushed_bytes = 0;
    std::uint64_t notifications = 0;
    std::uint64_t fills = 0;
    std::uint64_t fill_bytes = 0;
    std::uint64_t deferred = 0;
    std::uint64_t dropped = 0;
    std::uint64_t underruns = 0;
    std::uint32_t max_buffers = 0;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // `Machine::wire()` 调用：之后所有方法都假定 cpu/mem/clock 已装配。
    void attach(X86* cpu, GuestMemory* mem, VirtualClock* clock);

    // 由 `Machine::configureAudio()` / CLI 调用；可在 run 前反复调用（run 开始后改无效）。
    bool configure(AudioMode mode, const std::string& dump_path, std::string* err);

    bool begin(std::string* err);  // 打开 SDL 设备 / dump 文件；幂等
    void end();                    // 关闭设备、回填 WAV 大小；幂等
    bool enabled() const { return mode_ != AudioMode::Off; }
    AudioMode mode() const { return mode_; }
    bool threadMode() const { return thread_mode_; }
    const AudioStats& stats() const { return stats_; }
    const std::string& lastError() const { return last_error_; }

    // ---- 设备 / 缓冲（由 dsound shim 调）----
    void addBuffer(std::uint32_t state_ea, std::uint32_t obj_ea, std::uint32_t data_ea, std::uint32_t bytes,
                   std::uint32_t flags, const AudioFormat& fmt);
    void removeBuffer(std::uint32_t state_ea);
    void setFormat(std::uint32_t state_ea, const AudioFormat& fmt);
    void setNotify(std::uint32_t state_ea, std::uint32_t count, const std::uint32_t* offsets);
    void clearNotify(std::uint32_t state_ea);
    void play(std::uint32_t state_ea, bool looping);
    void stop(std::uint32_t state_ea);
    void setPosition(std::uint32_t state_ea, std::uint32_t pos);
    void setVolume(std::uint32_t state_ea, std::int32_t centibel);
    void setPan(std::uint32_t state_ea, std::int32_t pan);
    // `GetCurrentPosition` 用；没有这个 buffer ⇒ 0。
    std::uint32_t playedCursor(std::uint32_t state_ea) const;

    // ---- 喂料线程控制（由 kernel32_sync 调）----
    bool shouldOwnThread(std::uint32_t start_ea) const;
    bool registerFeeder(std::uint32_t start_ea, std::uint32_t param, std::uint32_t thread_id, std::uint32_t handle);
    void resumeFeeder();
    bool isFeederHandle(std::uint32_t handle) const;
    void noteEventSet(std::uint32_t handle);  // SetEvent 钩子：识别退出事件
    void joinFeeder();                        // WaitForSingleObject(feeder) 钩子
    bool feederRegistered() const { return feeder_.registered; }
    bool feederExited() const { return feeder_.exited; }
    bool inFeederFill() const { return in_fill_; }

    // ---- dispatcher ----
    AudioPump pump();       // 块边界调用；可能铺一个 guest 帧
    void afterGuestCall();  // `GuestCallStack::resume` 之后调用：还回被借用的 `[esp]`
    // run 在 fill 中间结束（--time-limit / --max-insns / 停止）时回滚：
    // 还 [esp] / 以 feeder 身份 LeaveCS / --在飞 / 恢复上下文 / 丢掉 audio 帧。
    void abortFill();
    const std::deque<AudioNotifyEvent>& pending() const { return pending_; }

    void dumpSummary(std::FILE* out) const;
    // 真线程 feeder 的 shim 块按需装进私有 cache（ComRegistry 新建 vtable 时会调）。
    void installFeederShim(std::uint32_t stub_ea, std::uint32_t shim_id);

    // ---- 仅供夹具（不参与运行）----
    std::uint64_t playedBytes() const { return stats_.played_bytes; }
    std::uint64_t mixedBytes() const { return stats_.mixed_bytes; }
    void clearPendingForTest() { pending_.clear(); }

private:
    struct Buffer {
        std::uint32_t state_ea = 0;
        std::uint32_t obj_ea = 0;
        std::uint32_t data_ea = 0;
        std::uint32_t bytes = 0;
        std::uint32_t flags = 0;
        AudioFormat fmt;
        bool playing = false;
        bool looping = false;
        std::uint32_t initial_pos = 0;   // Play / SetCurrentPosition 时的位置
        std::uint64_t start_mixed = 0;   // 这条流在全局混音时间线上的起点
        std::uint32_t notify_count = 0;
        std::uint32_t notify_off[4] = {};
        // 增益按 SetVolume/SetPan 缓存（guest 状态块仍是权威；这里只是混音快路径）。
        std::int32_t volume = 0;
        std::int32_t pan = 0;
    };

    struct Feeder {
        bool registered = false;
        bool runnable = false;
        bool exited = false;
        bool exit_requested = false;
        std::uint32_t start_ea = 0;
        std::uint32_t sound = 0;
        std::uint32_t thread_id = 0;
        std::uint32_t handle = 0;
        std::uint32_t quit_event = 0;  // [Sound+0x514]（= 事件 #60）
    };

    // 泵要借用的 guest 上下文（⛔ 不要只存 GPR：x87/XMM 也是线程私有）。
    struct Context {
        std::uint32_t r[8] = {};
        std::uint32_t eip = 0;
        std::uint32_t eflags = 0;
        std::uint16_t seg[6] = {};
        std::uint32_t fs_base = 0;
        std::uint32_t gs_base = 0;
        X87State fpu;
        std::uint32_t xmm[8][4] = {};
        std::uint32_t thread_id = 0;
        std::uint32_t last_error = 0;
    };

    Buffer* find(std::uint32_t state_ea);
    const Buffer* find(std::uint32_t state_ea) const;
    bool findSlot(std::uint32_t obj_ea, std::uint32_t* slot, std::uint32_t* stream) const;

    Context capture() const;
    void restore(const Context& c, std::uint32_t* shim_result);
    void fireCrossings(std::uint64_t old_played, std::uint64_t new_played);
    void enqueue(std::uint32_t obj_ea, std::uint32_t quarter);
    void generateTo(std::uint64_t target);
    void pushMixed(const std::uint8_t* pcm, std::size_t n);
    void advanceDevice();
    bool openSdl();
    void closeSdl();
    bool openDump();
    void closeDump();
    void writeDump(const std::uint8_t* pcm, std::size_t n);
    void logAt(lift::LogLevel level, const char* fmt, ...) const;

    X86* cpu_ = nullptr;
    GuestMemory* mem_ = nullptr;
    VirtualClock* clock_ = nullptr;

    AudioMode mode_ = AudioMode::Off;
    AudioMode configured_mode_ = AudioMode::Off;
    std::string dump_path_;
    std::string last_error_;
    bool begun_ = false;
    bool sdl_ok_ = false;
    bool dump_ok_ = false;
    bool logged_sdl_fail_ = false;
    bool warned_slot_mismatch_ = false;
    bool underrun_active_ = false;

    std::vector<Buffer> buffers_;
    std::deque<AudioNotifyEvent> pending_;
    Feeder feeder_;

    // 全局时间线（单位都是"混音输出字节"；44.1k/16/2 ⇒ 4 B/frame）
    std::uint64_t played_ = 0;     // 设备已经消费的字节
    std::uint64_t generated_ = 0;  // 已经混好并交给 sink/dump 的字节
    std::uint64_t pushed_ = 0;     // 已推给 SDL 的字节
    std::uint64_t queued_ = 0;     // SDL 队列里还没被设备取走的字节

    // SDL
    void* stream_ = nullptr;  // SDL_AudioStream*
    bool sdl_inited_ = false;

    // dump（44 字节 RIFF 头；结束时回填大小）
    std::FILE* dump_ = nullptr;
    std::uint64_t dump_bytes_ = 0;

    // 泵借用 `[esp]` 的现场：开始前写 snapshot.eip，resume 之后还回原返回地址。
    bool borrowed_ret_ = false;
    std::uint32_t borrowed_esp_ = 0;
    std::uint32_t saved_ret_ = 0;
    Context snapshot_;
    bool in_fill_ = false;
    std::uint32_t fill_cs_ = 0;
    std::uint32_t fill_inf_ea_ = 0;

    AudioStats stats_;
    // ★ 方案 C 的 feeder（真宿主线程）：自己的 guest CPU / 栈 / 私有块缓存 / 私有回调栈。
    //   ⛔ 不与主线程共用 `X86` / `BlockCache` / `GuestCallStack`。
    bool thread_mode_ = false;
    std::thread feeder_thread_;
    std::condition_variable feeder_cv_;
    std::mutex pending_mutex_;
    bool stop_requested_ = false;
    std::atomic<bool> feeder_error_{false};
    std::string feeder_error_text_;
    // 独立 mixer 线程（方案 C）：主线程卡在 Present/长 shim 时仍持续把 guest 环送设备。
    std::thread mix_thread_;
    std::mutex mix_wait_mutex_;
    std::condition_variable mix_cv_;
    bool mix_stop_ = false;
    X86 audio_cpu_;
    GuestCallStack audio_calls_;
    NativeOverrideTable audio_overrides_;
    std::unique_ptr<Decoder> audio_decoder_;
    std::unique_ptr<BlockCache> audio_cache_;
    // 只保护私有 cache 的装/取；⛔ 不要用设备模型的 mutex_（主线程混音会长时间持有它）。
    std::mutex feeder_cache_mutex_;
    std::uint32_t audio_stack_top_ = 0;
    static constexpr std::uint32_t kAudioStackSize = 0x00100000u;

    void startFeederThread();
    void feederThreadMain();
    void feederEvent(const AudioNotifyEvent& ev);
    bool runFeederGuest(std::string* err);
    bool setupFeederContext(std::string* err);
    // 混音一步（调用方持有 mutex_）：推进设备游标 / 生成 / 投递通知。主线程泵与独立 mixer
    // 线程共用；真线程模式下只由 mixer 线程调（主线程不再因 Present/长 shim 断音频）。
    void mixStep();
    void mixLoop();
    void startMixThread();
    void stopMixThread();

    // ★ 方案 C：feeder 线程会调 `stop`（sub_4B6C30 的状态 0 分支），主线程在 pump 里混音；
    //   设备模型/时间线必须互斥。递归：pump 内可能经 startNestedGuestCall 再进 stop（方案 D）。
    mutable std::recursive_mutex mutex_;
};

}  // namespace lift
