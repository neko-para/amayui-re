// emulator-lift · 音频设备模型 + **feeder 线程**（唯一一条喂料路线）
//
// ★ 口径（`docs/03-emulator/audio.md` §1；判据只有一份 = `runtime/audio_policy.h`）：
//   - 引擎的音频喂料线程 `sub_4B5AA0` **不执行它的线程体**：`CreateThread(0x4B5AA0)` 只登记身份，
//     `ResumeThread` 启动**真宿主 feeder 线程**；线程体里的 12 段 native + 1 段 guest
//     （`0x4B5B03 call sub_4B6C30`）由 feeder 在自己的 guest 上下文里跑。
//   - feeder 有自己的 guest 栈 / TEB / 寄存器现场；`sub_4B6C30` 由宿主注入的 guest 执行器跑
//     （解释器臂 = 本文件的 `runFeederGuest`；AOT 臂 = `FeederGuestRunner`）。lead = 100 ms。
//   - `--audio` **只决定输出位置**（sdl 设备 / 无输出 / `--audio-dump` 的 WAV）。⛔ 没有主线程泵
//     （D 泵）这条第二路线，也没有 600 ms 的 D 档 lead（2026-11-27 用户裁决）。
//   - 设备模型（游标 / 4 个位置通知 / 状态机 / 混音）在宿主；PCM 仍由 guest 解码器产生。
//   - 一次 fill 做**整份 guest 上下文交换**（GPR / EFLAGS / x87 / XMM / 段 / fs/gs / thread_id /
//     last_error）。⛔ 只存 8 个 GPR 不够（Ogg 解码用浮点）。
//   - ★ feeder 有**自己的 TEB**（`GuestLayout::feeder_teb`），⛔ 不与主线程共用：共用会让 feeder 的
//     SEH prologue 改写主线程的 `fs:[0]`（BUG-22：Quit 的 C++ throw 派发到已复用的 record）。
//
// ★ 时钟：唯一档 = 墙钟（`Clock`）。设备游标由宿主设备"已消费字节"驱动（`pushed - queued`）；
//   没有真设备时（dump / 夹具）由 `now100ns() - origin_100ns_` 驱动。两种情况下通知点都由
//   **游标越过**产生，且只在宿主侧投递给 feeder（没有宿主音频回调碰 guest 内存）。
//
// ★ 数据通路：mixer 线程混音（读 guest 环）→ `SDL_PutAudioStreamData`。SDL 的内部线程只碰
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

#include "runtime/audio_policy.h"
#include "runtime/guest_call.h"
#include "runtime/native_overrides.h"
#include "runtime/x86.h"

namespace lift {

class GuestMemory;
class Clock;
class Decoder;
class BlockCache;
struct GuestLayout;
enum class LogLevel : int;

// ★ AOT 臂的 feeder guest 执行器（**引擎只转发**；⛔ `audio.cpp` 不 include 任何 aot 头）。
//   · 缺省 `nullptr` ⇒ 引擎走本文件的解释器路径 `runFeederGuest()`（解释器臂逐字不变）；
//   · AOT 宿主管 `ctx` 指向自己，`run` 用 **AOT 派发循环**执行 guest（`AGENTS.md` §12.8：
//     AOT 的执行来源只有 IR + shim，⛔ 不走解释器）。
//   · `entry_ea` / `return_ea` **由引擎传入**（`kSub4B6C30` / `kFeederReturnEa` 的唯一来源在
//     `audio.cpp`）⇒ ⛔ 不要在宿主侧另写 `0x7FFEF00C`。
//   · 返回 false + `*err` ⇒ 调用方置 `feeder_error_`（与解释器路径同一条失败面，不是静默）。
struct FeederGuestRunner {
    bool (*run)(void* ctx, X86& cpu, std::uint32_t entry_ea, std::uint32_t return_ea,
                std::string* err) = nullptr;
    void* ctx = nullptr;
};

// ★ `AudioMode` / `audioModeName` / `parseAudioMode`（S1 参数面）只有一份：
//   `runtime/audio_policy.h`（纯头，可被 pure CTest 直接驱动）。⛔ 不要在这里再写一份。

// 一条位置通知（顺序 = 设备游标越过通知点的顺序）。
struct AudioNotifyEvent {
    std::uint32_t obj = 0;      // IDirectSoundBuffer8 的 guest EA
    std::uint32_t quarter = 0;  // 0..3（引擎 0x4B5AFF 的 `esi & 3`）
    // ★ 产生这条通知时该 Buffer 的 generation。Stop / SetCurrentPosition 会 `++generation`
    //   让**已经排队**与**已 pop、还没跑**的条目一起失效（见 `stop()` 的注释）。
    //   只作废该 obj 自己的待办 ⇒ 别条流不受影响。
    std::uint64_t generation = 0;
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
    // 交付点被静音门覆盖掉的字节数（缺省 == mixed_bytes；`--sound` 时 0）。
    // 用来区分"有内容被静音"与"没数据 / 没设备" ⇒ 见 dumpSummary 的 muted_bytes。
    std::uint64_t muted_bytes = 0;
    std::uint64_t pushed_bytes = 0;
    std::uint64_t notifications = 0;
    std::uint64_t fills = 0;
    std::uint64_t fill_bytes = 0;
    std::uint64_t deferred = 0;
    std::uint64_t dropped = 0;
    // ★ `underruns` = **episode** 计数：设备队列空 **∪ 环陈旧**（宿主替 guest 补静音，见下面四个
    //   字段）。★ 口径已从"只在 SDL 队列 == 0 时计"扩一档（2026-11-13，`M-07` 同步改）⇒
    //   "`underruns == 0` 才算健康"的口径继续成立，分项看 `underrun_silence_*`。
    std::uint64_t underruns = 0;
    // ★ underrun 插静音（`.tmp/audio-phase/design.md` §1.5；判据 = **写见证**，⛔ 不是内容比对）：
    //   underrun_silence_bytes = 宿主替 guest 填的静音**输出字节**（48k/16/2 ⇒ 每帧 +4；同一
    //     输出帧多条流都陈旧只 +4 一次）—— "不可听的部分有多少"；
    //   underrun_silence_events = "过圈发现环陈旧"的次数（每 quarter 每圈最多 1 次）= episode；
    //   fill_witnessed = `[stream+0x2468]` 前后**变了**的 fill 数（= 真写了环的刀）；
    //     `fills - fill_witnessed` = 引擎早退（`[+0x2458]==0` 的 Stop 分支，那条不 inc）
    //     **或**那一刀 `[+0x2468]` 读不到（按"写了"处理但不算见证）。
    //   guest_silence_fills = 收尾时 `[+0x2458] >= 2` **且被见证**的 fill 数（引擎"放完 + 填 0"）
    //     ⇒ 它 > 0 而 `underrun_silence_bytes == 0` 就是"guest 自己写的静音"，不是宿主插的。
    //   不变式：① `underrun_silence_bytes <= mixed_bytes`（静音也是内容，照常进 mixed）；
    //     ② `guest_silence_fills <= fill_witnessed <= fills`；③ `underruns >= underrun_silence_events`。
    //   ★ 只进 summary 的观测口径不变（读数进 docs，不进 test 的绝对值断言）。
    std::uint64_t underrun_silence_bytes = 0;
    std::uint64_t underrun_silence_events = 0;
    std::uint64_t fill_witnessed = 0;
    std::uint64_t guest_silence_fills = 0;
    // ★ D1/D4 的诊断（只进 summary，不作为断言取值；见 docs/audio-implementation.md §5）：
    //   stale = 被 Stop / SetCurrentPosition 作废掉的通知条目（generation 不符）；
    //   slot_miss = feeder `findSlot` 失败（obj 反演不到槽；API 不定义"事件↔槽"）；
    //   play_on_playing = 对已在播的缓冲再 Play（真 DS 的 DS_OK + 继续播语义）；
    //   fill_waits / fill_wait_timeouts = `stop()` / `setPosition()` 等"正在跑的那一刀 fill"
    //     的次数与超时次数（只计**真的等过**的，即 `filling_obj_ == obj` 那一刻）。
    //   fill_overlapped（旧名 fill_raced）= **有 Stop / 重定位与这一刀在时间上重叠**（判据：
    //     fill 结束后该 obj 的代次已经不是当初那一代）。★ 它**不是错误计数**：第 3 层会等这一刀
    //     跑完再重锚，重叠是**被处理掉**的正常情形；而且它混了三种**良性**成因 ——
    //     ① fill 里的 EOF 自停（`stop()` 在 fill 内被调，`0x4B6C58`）、② `removeBuffer`
    //     （`Release` 把这条流删了 ⇒ 连 Buffer 都没了）、③ 同一 state 重建（`addBuffer` 的
    //     `++generation`）。真正"没兜住"的判据是 `fill_wait_timeouts > 0`
    //     （⛔ 只记账，不回滚已写进环的字节）。
    std::uint64_t stale = 0;
    std::uint64_t slot_miss = 0;
    std::uint64_t play_on_playing = 0;
    std::uint64_t fill_waits = 0;
    std::uint64_t fill_wait_timeouts = 0;
    std::uint64_t fill_overlapped = 0;
    // ★ 只读诊断（本轮为"边界处重播 2 s"加的**观测**；⛔ 都不作为断言取值，⛔ 不参与任何判定）：
    //   slot_reverify_fail = `feederEvent` **拿到槽 CS 之后**重验槽失败（`[Sound+0x654+4s]` 或
    //     `[stream+0x40C]` 已被主线程换掉）⇒ 这一刀丢掉，不写环、不 +在飞。
    //     ★ 它堵的是本函数原先**唯一没有账**的早退：notify 入队数 > fills 的差额在
    //       `stale` / `dropped` / `slot_miss` 全 0 时只能落在这里（见 audio.cpp 里
    //       `feederEvent` 的早退清单）。
    //   fill_max_us / fill_slow = 单刀 fill 的**最大**墙钟（微秒）与"超过 1000 ms"的刀数。
    //     ⛔ 只记账：慢刀是诊断线索（环长 2 s 而单刀 > 2 s ⇒ 会落后一整圈），不是错误判据。
    std::uint64_t slot_reverify_fail = 0;
    std::uint64_t fill_max_us = 0;
    std::uint64_t fill_slow = 0;
    //   fill_wait_us / fill_wait_max_us = 第 3 层"等正在跑的那一刀"的**累计 / 单次最大**墙钟
    //     （微秒；summary 里按毫秒打）。只统计**真的进过 wait** 的那几次（与 `fill_waits`
    //     同一个集合 ⇒ 两者可除）。★ 口径 = **只量谓词等待本身**（`t1` 取在重取 `mutex_`
    //     之前）⇒ 不含混音器占用，⛔ 不要当"调用方总卡顿"读。
    //     ⛔ 只读诊断：不作为断言取值（读数进 docs/03-emulator/observations.md，不进 test）。
    std::uint64_t fill_wait_us = 0;
    std::uint64_t fill_wait_max_us = 0;
    // ★ S2（`REQ-01M7K2J7FP60YHNGTEFR5EGGA7`）：`generateTo()` 的**越界门**命中次数
    //   （`out_frames64 > 0xFFFFFFFF`）。越界那一刻仍然 `return`（一次混音不可能产出 4 Gi 帧），
    //   ⛔ 但**不许静默**：每次 +1、首次一行 Warn；读数 = `dumpSummary` 的 `gen_overflow=`。
    //   正常档恒 0（游标原点已改成"这一轮引擎的起点"）；非 0 说明设备时钟 / 原点又跑飞了。
    std::uint64_t gen_overflow = 0;
    std::uint32_t max_buffers = 0;
};

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // 宿主装配：之后所有方法都假定 cpu/mem/clock 已装配。
    // ★ **feeder 布局是结构前提**（不是运行时选项）：真 feeder 线程要自己的一页 TEB +
    //   一段专用 guest 栈，二者都来自 `GuestLayout`（`GuestLayout::feeder_teb` /
    //   `feeder_stack_top` / `feeder_stack_size`，唯一真源在 `runtime/image.h`）。
    //   `AudioEngine::attach(..., const GuestLayout&)` 把它**拷进引擎**（不存指针：`Machine` 的
    //   `audio_` 声明在 `layout_` 之前，析构逆序会让指针悬垂）。
    //   ⇒ "能不能起 feeder"在编译期由这个引用钉死；⛔ 不再有 `has_machine` / 能力位那种
    //     可以"声明 true 而实际为空"的布尔。
    //   ★ 三参重载是**过渡期兼容**（老夹具）：它等价于"宿主没有布局" ⇒ `resumeFeeder()` 会
    //     **显式 Warn** 并且不建 feeder 线程（不是静默地假装有）。生产装配一律四参。
    void attach(X86* cpu, GuestMemory* mem, Clock* clock);
    void attach(X86* cpu, GuestMemory* mem, Clock* clock, const GuestLayout& layout);

    // 由 `Machine::configureAudio()` / CLI 调用；可在 run 前反复调用（run 开始后改无效）。
    // ★ `sound=false`（缺省）= 静音：设备 / 混音 / 喂料 / guest 的 DS 调用全部照跑，
    //   只在**最终交付点**（`generateTo` 里 `out` 生成之后、`pushMixed` 之前）把缓冲置 0。
    //   ⛔ 不是"不建设备 / 不起 feeder / 提前退出 / 改 guest SetVolume"。
    // ★ `dump_path` 非空 = `--audio-dump`：它**不是档位**，只在 `mode==Off` 时把这一跑标记成
    //   "设备无关的 dump 路径"（墙钟游标 + 交付点写 WAV）。`--audio` 的值域只有 off / sdl。
    bool configure(AudioMode mode, const std::string& dump_path, bool sound, std::string* err);

    bool begin(std::string* err);  // 打开 SDL 设备 / dump 文件；幂等
    void end();                    // 关闭设备、回填 WAV 大小；幂等
    // ★ 这一跑要不要跑混音 / 泵（`mode_ != Off` 或 dump / 夹具的设备无关路径）。
    //   ⚠ `--audio sdl` 打不开设备 ⇒ 这里是 **false**（没有输出 ⇒ 不跑混音），但
    //   `audioRequested()` 仍是 true（摘要要如实写 `device=none`）。
    bool enabled() const { return mode_ != AudioMode::Off || device_free_; }
    // ★ 这一跑**要过**音频吗（摘要 / readout 的门）：`--audio off` 且没有 `--audio-dump` ⇒ false。
    //   与 `enabled()` 分开的理由：设备打不开时"没有输出"不等于"没要音频" —— ⛔ 不许把失败
    //   静默成"缺省档"（那样收口摘要里连 `device=none` 都看不到）。
    bool audioRequested() const {
        return configured_mode_ != AudioMode::Off || dump_requested_ || device_free_;
    }
    // ★ dump 路径 / 夹具钩子要求"没有设备也跑混音"（墙钟游标）。⛔ 这不是档位、不进 `--audio`。
    bool deviceFreeMix() const { return device_free_; }
    AudioMode mode() const { return mode_; }
    // ★ 只读：`configure()` 之后 / `begin()` 之前的**配置事实**（`mode()` 要等 begin 才是真值）。
    //   存在理由：readout 打在 `begin()` 之前，而 `--audio sdl` 失败后 `mode()` 变 Off ⇒
    //   读数要用"配置时请求的档"。⛔ 不在测试里重抄配置规则。
    AudioMode configuredMode() const { return configured_mode_; }
    // 缺省 true = 静音；`--sound` 时 false。与后端（`--audio`）正交。
    bool muted() const { return mute_; }
    const AudioStats& stats() const { return stats_; }
    const std::string& lastError() const { return last_error_; }
    // ★ feeder 的失败面（只读）：块边界 / 收口**必须**看它 —— feeder 挂了不许"继续跑但没声"。
    //   `feederErrorText()` 走独立小锁（文字由 feeder 线程写、主线程读）。
    bool feederFailed() const { return feeder_error_.load(); }
    std::string feederErrorText() const;
    // ★ 注入 guest 执行器（缺省 nullptr = 走本文件的解释器 `runFeederGuest`）。只能在 begin 之前调。
    void setFeederGuestRunner(const FeederGuestRunner& runner) { runner_ = runner; }

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

    // ---- 影片音频（宿主 PCM 源；由 dshow 的 AudioRenderer 调）----
    // 追加一段 **48k / S16 / 2ch** 的宿主 PCM，从当前生成点尾部顺序混进设备输出。
    // 同步（什么时候推）由 quartz 侧的 `dshow::AudioRenderer` 决定，这里只负责混音。
    void pushHostPcm(const std::int16_t* interleaved, std::uint32_t frames);
    void clearHostPcm();
    bool hostPcmActive() const { return !host_pcm_.empty(); }

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

    // ---- feeder ----
    // run 在 fill 中间结束（--time-limit / --max-insns / 停止）时回滚：
    // 还 [esp] / 以 feeder 身份 LeaveCS / 在飞计数 / 恢复上下文 / 丢掉 audio 帧。
    void abortFill();
    // ⚠ 只给夹具 / 单线程读：不取 `pending_mutex_`（thread 模式里 feeder 正在改这个 deque）。
    const std::deque<AudioNotifyEvent>& pending() const { return pending_; }

    void dumpSummary(std::FILE* out) const;
    // ★ 默认档摘要（`REQ-01M7AG06MXS6PK2B53B29QM76Z`）要的**只读快照**：与 `dumpSummary` 同一份
    //   `stats_`，但只在 `mutex_` 下拷几个字段 —— 默认档不再打 428 B 的 `audio :` 全表，
    //   关键量进 `read :` 一行，异常项进 `=== 异常 ===`。⛔ 不新增状态、不改任何计数。
    struct Snapshot {
        const char* mode = "off";
        const char* device = "none";
        bool muted = false;
        double fill_max_ms = 0.0;
        std::uint64_t fills = 0;
        // ★ 喂料 / 交付的**字节账**（`fills` 只回答"跑了几刀"，回答不了"喂了多少"）。
        //   口径 = 环长 L / 4，与 `dumpSummary` 的 `fill_bytes=` 同一份 `stats_`（⛔ 不另算）。
        std::uint64_t fill_bytes = 0;
        std::uint64_t pushed_bytes = 0;
        std::uint64_t muted_bytes = 0;
        std::uint64_t underruns = 0;
        std::uint64_t fill_slow = 0;
        std::uint64_t fill_wait_timeouts = 0;
        std::uint64_t slot_reverify_fail = 0;
        std::uint64_t underrun_silence_events = 0;
        std::uint64_t fill_overlapped = 0;
        std::uint64_t fill_witnessed = 0;
    };
    Snapshot snapshot() const;
    // 真线程 feeder 的 shim 块按需装进私有 cache（ComRegistry 新建 vtable 时会调）。
    void installFeederShim(std::uint32_t stub_ea, std::uint32_t shim_id);

    // ---- 仅供夹具（不参与运行）----
    // ★ 仅夹具（⛔ **不是** CLI 档位、⛔ 不进 `--help`）：没有设备时也让混音跑起来
    //   （墙钟游标 + 交付点），并**不启动 mix 线程**（由夹具用 `mixOnceForTest()` 同步推进，
    //   否则设备模型的断言会与 5 ms 周期的 mixer 线程抢时序）。存在理由：`audio-device` 那批
    //   pure 用例判的是设备模型 / 通知次序 / 混音字节，它们必须能在**没有声卡**的机器上跑；
    //   而 `virtual` 档已于 2026-11-22 移除（`--audio` 只剩 off / sdl）⇒ 夹具需要一条不进
    //   参数面的路。`--audio-dump` 是产品面的等价路径（它自己会置位 `device_free_`），
    //   不需要这个钩子。
    //   ⛔ 生产路径不要调它（`aot_main` / `Machine::configureAudio` 都不碰）。
    void enableDeviceFreeMixForTest() { device_free_for_test_ = true; }
    // ★ 仅夹具：**同步**跑一次混音（设备游标 + 生成 + 投递通知）。⛔ 不铺 guest 帧、
    //   ⛔ 不执行 guest 代码 —— 那是 feeder 线程的事。存在理由：设备模型那批 pure 用例
    //   需要"我推进时钟 ⇒ 我立刻能看到通知"，而产品路径由 5 ms 的 mixer 线程驱动。
    void mixOnceForTest();
    std::uint64_t playedBytes() const { return stats_.played_bytes; }
    std::uint64_t mixedBytes() const { return stats_.mixed_bytes; }
    // ⚠ 只给夹具 / 单线程：不取 `pending_mutex_`。
    void clearPendingForTest() { pending_.clear(); }
    // ★ 仅夹具：伪造 / 清除"某一刀 fill 正在跑"（`filling_obj_`）。0 = 清。
    //   存在理由：`filling_obj_` 只在 `feederEvent` 里置位，而 feeder 要真 guest 执行到
    //   `sub_4B6C30` 才起 ⇒ 纯档 CTest 造不出"在飞 fill"，`removeBuffer` / `addBuffer`
    //   的等待分支在测试里永远走不到（守卫恒绿）。
    //   实现在 audio.cpp：在 `pending_mutex_` 下 store 并 `notify_all`（与 fill 收尾同一纪律，
    //   否则等的人会丢唤醒）。⛔ 不进生产路径；⛔ 不要改成无锁 check-then-set
    //   （见 `filling_obj_` 成员上的注记）。
    void setFillInFlightForTest(std::uint32_t obj_ea);
    // ★ 仅夹具：等价于"某一刀 fill 跑完并写了环"（`witness[quarter]++`）。
    //   存在理由：纯档一次 `Machine::run` 只能铺/跑一帧（`machine.cpp` 收尾会 `audio_.end()`），
    //   而"过圈陈旧"最早出现在第 2 圈 ⇒ 一条 pure 用例里跑不完两圈真 fill。
    //   ⛔ 只喂 witness，不写 guest 内存、不改 `playing` / 游标 / 统计 / 不假装见过 guest 的静音。
    void noteFillForTest(std::uint32_t obj_ea, std::uint32_t quarter);
    // ★ 仅夹具：读回某 quarter 的写见证计数（`Buffer::witness[quarter]`，见该成员的注记）。
    //   存在理由：`noteFillWitness` 的 `++witness[quarter]` 是"真 fill 写完环 ⇒ 下个圈进该
    //   quarter 不判陈旧"的**唯一**机械来源，而它是**内部状态**（stats 里只有 `fill_witnessed`，
    //   挂在同一个 `if` 的**另一行**）⇒ 只断言 stats 钉不住它（实测：删掉那行全部旧用例仍绿）。
    //   ⛔ 只读（取 `mutex_`、不做任何判定）；⛔ 不进生产路径。
    std::uint64_t fillWitnessForTest(std::uint32_t obj_ea, std::uint32_t quarter);
    // 仅夹具：读回 `filling_obj_`（断言"清 0 真的生效"；生产路径不要用）。
    std::uint32_t fillInFlightForTest() const { return filling_obj_.load(); }
    // ★ 只读公开面：本线程是否是音频 feeder 线程（`isFeederThread()` 的公开转发）。
    //   存在理由：feeder 也会调 shim（`sub_4B6C30` 的 fill 里），而 `MessageBoxA` 这类
    //   "只能主线程进模态"的 shim 必须能在**不碰 wm/presenter** 的前提下先判自己在哪条线程上
    //   （`runtime/message_box.cpp` / `shims/user32_menu.cpp`）。⛔ 不是新状态、不改任何行为。
    bool onFeederThread() const { return isFeederThread(); }

    // ---- 仅夹具：feeder 的 TEB / 栈 / 执行器 ----
    // ★ 存在理由（BUG-22 回归口）：`setupFeederContext` 要真 guest 执行到 fill 才被调到，
    //   而 pure CTest 没有设备 ⇒ 造不出那条路径。这两个入口让"feeder 有
    //   自己的 TEB（≠ 主线程 TEB）"变成一条**不需要设备**的结构断言；⛔ 不进生产路径。
    //   `setupFeederContextForTest` 只做上下文（不建线程、不开设备），与 `setupFeederContext` 同一份实现。
    bool setupFeederContextForTest(std::string* err);
    std::uint32_t feederTebForTest() const { return feeder_teb_; }
    std::uint32_t feederFsBaseForTest() const { return audio_cpu_.fs_base; }
    std::uint32_t feederStackTopForTest() const { return audio_stack_top_; }
    bool feederLayoutGivenForTest() const { return feeder_layout_given_; }

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
        // ★ 失效代次：`stop()` / `setPosition()` / `addBuffer()` 会 `++generation`，
        //   使该 obj 已排队的 `AudioNotifyEvent` 与已 pop 未跑的 fill 一起作废。
        //   ⛔ 不要用它替代 `playing`（它不改播放状态，只标记"通知属于哪一代"）。
        std::uint64_t generation = 0;
        std::uint32_t initial_pos = 0;   // Play / SetCurrentPosition 时的位置
        std::uint64_t start_mixed = 0;   // 这条流在全局混音时间线上的起点
        std::uint32_t notify_count = 0;
        std::uint32_t notify_off[4] = {};
        // ★ 写见证（`.tmp/audio-phase/design.md` §1.2 / §1.3；全部在 `mutex_` 下）：
        //   `witness[q]` = 该 quarter 区"跑完并写了环"的 fill 计数（宿主侧账，来源 = `[+0x2468]`
        //     在 fill 前后变了）；`consumed[q]` = 上一次**开始读**这个 quarter 时的 `witness` 值。
        //   ⇒ 读指针进入 quarter q 时 `witness[q] == consumed[q]` = 这一圈 q 区**没有被任何跑完的
        //     fill 重写过** ⇒ 该区交付静音（⛔ 不是重播上一圈）。
        //   ⛔ 判据是**写见证**，不是内容比对：guest 自己"放完 + memset 0"也是全 0
        //     （`[+0x2458]==2` 走的是同一条 inc 路径 ⇒ 记见证 ⇒ 不会被判陈旧）。
        std::uint64_t witness[4] = {};
        std::uint64_t consumed[4] = {};
        std::uint32_t read_quarter = 0;  // 读指针当前所在 quarter（过圈检测）
        bool read_started = false;       // 起播 / 复位后第一次判定还没做
        // ★ 当前 `read_quarter` 是否已判陈旧（**跨 `generateTo` 调用**保持：一次混音往往只
        //   覆盖 quarter 的一部分，不能每帧重判 —— 转换时 `consumed` 已被更新成 `witness`，
        //   重判会把健康 quarter 判成陈旧）。只在 quarter 切换时（重新）赋值。
        bool read_stale = false;
        // 增益按 SetVolume/SetPan 缓存（guest 状态块仍是权威；这里只是混音快路径）。
        std::int32_t volume = 0;
        std::int32_t pan = 0;
    };

    struct Feeder {
        bool registered = false;
        bool runnable = false;
        bool exited = false;
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
    // ★ 写见证的复位（design §1.3 的表）：把整圈视为**已预填** —— `consumed[q] = witness[q];
    //   ++witness[q]`，然后 `read_started = false`。锚：P1 预填 q=1,2,3 是主线程 guest 调用
    //   （不经 `feederEvent` ⇒ 宿主无账），q0 由 offset-0 通知补（design §1.7 假设 A1）。
    //   调用方持有 `mutex_`；`addBuffer` / `setNotify` / `play`（false→true）/ `setPosition`
    //   （playing 时）各调一次。
    void markRingFresh(Buffer& b);
    // ring 偏移 → quarter：first i with `notify_off[i] > ring_off`，否则 0。要求 `notify_count==4`
    //   且 off 升序（调用方先判；off0==0 时与 guest 的 `[off[q-1], off[q])` 同构）。
    static std::uint32_t quarterOfRingOffset(const Buffer& b, std::uint32_t ring_off);
    // 一刀 fill **跑完**后的写见证记账（design §1.2 事实 2 / §1.5）。`read_ok` = fill 前后两次
    //   `[+0x2468]` 都读得到；`pre`/`post` 是那两次的值；`guest_silence` = 收尾时 `[+0x2458]>=2`。
    //   读不到 ⇒ **按"写了"处理**（宁可不插静音，也不误插）+ 不计见证。
    //   调用方持有 `mutex_`（`feederEvent` 的收尾块 / `pump` 的 continuation 里各一处）。
    void noteFillWitness(std::uint32_t obj_ea, std::uint32_t quarter, bool read_ok, std::uint32_t pre,
                         std::uint32_t post, bool guest_silence);
    // 按 guest 的 IDirectSoundBuffer8 EA 反查（feeder 只有 obj）。★ 内部取递归 `mutex_`，
    //   返回的指针只在同一次持锁里有效（`buffers_` 是 vector）。
    Buffer* findByObj(std::uint32_t obj_ea);
    bool findSlot(std::uint32_t obj_ea, std::uint32_t* slot, std::uint32_t* stream) const;
    // ★ 只读诊断（`late_q` 用）：该缓冲此刻的设备游标（字节，落在 [0, bytes)）。
    //   与 `playedCursor` **同一份算式**（后者现在只是"查表 + 本函数"）⇒ 两处不会漂。
    //   ⛔ 纯读：不发通知、不改 `playing` / `initial_pos` / 任何计数。
    std::uint32_t cursorOf(const Buffer& b) const;
    // ★ 只读诊断（`LIFT_AUDIO_TRACE` 的 `late_q`）：**fill 开始**那一刻，该 quarter 的
    //   **目标环位置**与**设备游标**的差，单位 = quarter（公式见 audio.cpp 的实现注释）。
    //   返回 false = 找不到该 obj / 环长度太小 ⇒ 调用方打 `?`（⛔ 不要拿 0 冒充"读不到"）。
    bool fillLagQuarter(std::uint32_t obj_ea, std::uint32_t quarter, std::uint32_t* ring_bytes,
                        double* late_q) const;
    // 本函数是否跑在 feeder 线程上（EOF 路径 `0x4B6C58` 会在 fill 里调到 `stop()`）。
    bool isFeederThread() const;
    // 等"正在跑的那一刀 fill"结束（S1）。返回 true = 已结束；false = 3000 ms 超时。
    // `*waited` 只在谓词一开始不成立、真的进过 wait 时为 true。
    bool waitFillDone(std::uint32_t obj_ea, std::uint32_t timeout_ms, bool* waited);
    // ★ 调用方持有 `lock`（`mutex_`）；等待期间**放开**它，返回时重新持有。
    //   返回 false = 超时（照常继续）。⛔ 调用方必须在返回后重新 `find()`：
    //   等待期间别的线程可能已经 erase / 重排 `buffers_`。
    bool waitFill(std::uint32_t obj_ea, std::unique_lock<std::recursive_mutex>& lock);

    Context capture() const;
    void restore(const Context& c, std::uint32_t* shim_result);
    void fireCrossings(std::uint64_t old_played, std::uint64_t new_played);
    void enqueue(std::uint32_t obj_ea, std::uint32_t quarter, std::uint64_t generation);
    // 丢掉该 obj 的排队通知。★ 只在 `pending_mutex_` 下动 `pending_`（锁序 `mutex_ → pending_mutex_`）。
    void dropPending(std::uint32_t obj_ea);
    void generateTo(std::uint64_t target);
    void pushMixed(const std::uint8_t* pcm, std::size_t n);
    void advanceDevice();
    bool openSdl();
    void closeSdl();
    bool openDump();
    void closeDump();
    void writeDump(const std::uint8_t* pcm, std::size_t n);
    void logAt(lift::LogLevel level, const char* fmt, ...) const;
    // ★ feeder 的失败面（feeder 线程调；文字在 `feeder_err_mutex_` 下写、主线程经
    //   `feederErrorText()` 读）。`feeder_error_` 是 atomic ⇒ AOT 的块边界可以无锁轮询。
    void setFeederError(std::string text);

    X86* cpu_ = nullptr;
    GuestMemory* mem_ = nullptr;
    Clock* clock_ = nullptr;

    AudioMode mode_ = AudioMode::Off;
    AudioMode configured_mode_ = AudioMode::Off;
    std::string dump_path_;
    std::string last_error_;
    bool begun_ = false;
    bool sdl_ok_ = false;
    // `begin()` 时 SDL 设备**曾经**打开成功（summary 在 `end()` 之后打印，那时 `sdl_ok_` 已清）。
    bool sdl_device_opened_ = false;
    // `--audio sdl` 打不开设备 ⇒ `begin()` 落回 `Off`（见 `begin`）。summary 的 `device=` 只写
    // `ok` / `none`：⛔ 不再有 `fallback` 这个"其实还有 D 泵在跑"的档位含义。
    // ★ `--audio-dump` 给了（`end()` 之后 summary 仍要知道"有没有 dump 这个 sink"）。
    bool dump_requested_ = false;
    // ★ dump / 夹具要求"没有设备也跑混音"（虚拟时钟）。⛔ 不是档位（`--audio` 只有 off / sdl）。
    bool device_free_ = false;
    bool device_free_for_test_ = false;
    // 缺省静音：见 `configure` / `generateTo` 的交付点门。与 `--audio` 后端正交。
    bool mute_ = true;
    bool dump_ok_ = false;
    bool logged_sdl_fail_ = false;
    bool warned_slot_mismatch_ = false;
    // feeder 的 `findSlot` 失败（D4）首次一行 Warn；之后只 `stats_.slot_miss` 计数。
    bool warned_slot_miss_ = false;
    // 环陈旧（宿主插静音）首次一行 Warn；之后只 `underrun_silence_bytes/events` 计数。
    bool warned_underrun_ = false;
    bool underrun_active_ = false;
    // `generateTo` 越界（S2）首次一行 Warn；之后只 `stats_.gen_overflow` 计数。
    bool warned_gen_overflow_ = false;

    std::vector<Buffer> buffers_;
    std::deque<AudioNotifyEvent> pending_;
    Feeder feeder_;

    // ★ 全局时间线（单位都是"混音输出字节"；44.1k/16/2 ⇒ 4 B/frame）
    std::uint64_t played_ = 0;     // 设备已经消费的字节
    // ★ 游标原点（S2）：`begin()` 那一刻的 `clock_->now100ns()`。没有真设备时（dump / 夹具的
    //   设备无关路径）`advanceDevice()` 用 `now - origin_100ns_` 算 `played_` —— ⛔ 不是 `now`
    //   的绝对值（AOT 是墙钟 ⇒ 那等于"自开机"）。
    std::uint64_t origin_100ns_ = 0;
    // 影片音频：宿主 PCM（48k/S16/2ch）+ 它在输出帧轴上的起点。
    std::vector<std::int16_t> host_pcm_;
    std::uint64_t host_start_frame_ = 0;
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
    // ★ feeder（真宿主线程）：自己的 guest CPU / 栈 / 私有块缓存 / 私有回调栈。
    //   ⛔ 不与主线程共用 `X86` / `BlockCache` / `GuestCallStack`。
    std::thread feeder_thread_;
    std::condition_variable feeder_cv_;
    std::mutex pending_mutex_;
    // ★ S1：**正在跑**的那一刀 fill 属于哪个 obj（0 = 没有）。只有 `feederEvent` 在
    //   `runFeederGuest` 之前置、跑完清 0 并 `pending_cv_.notify_all()`。
    //   置它的那一步与"代次校验"在**同一把 `mutex_`** 下 ⇒ 与 `stop()` 的
    //   `++generation` + "要不要等"不会交错（关掉"校验过了但还没置标志"的窗口）。
    //   ⛔ 不要改成无锁的 check-then-set：那会留下一小段主线程看不见在跑 fill 的窗口。
    std::atomic<std::uint32_t> filling_obj_{0};
    // 等 `filling_obj_` 归零用（配 `pending_mutex_`；⛔ 不用 `mutex_`：等待期间必须放开它）。
    std::condition_variable pending_cv_;
    // feeder 线程**自己**登记的 id（`feederThreadMain` 首尾 store）⇒ `isFeederThread()` 不必读
    // `feeder_thread_`（那会与主线程的 `join()` 竞争同一个 `std::thread` 对象）。
    std::atomic<std::thread::id> feeder_tid_{};
    bool stop_requested_ = false;
    std::atomic<bool> feeder_error_{false};
    // ★ 失败文字：feeder 线程写、主线程（AOT 块边界 / 摘要）读 ⇒ 必须走这把独立小锁。
    //   ⛔ 不要裸读/裸写这个 `std::string`（那是数据竞争）；锁序上它与 audio 的锁无关。
    std::string feeder_error_text_;
    mutable std::mutex feeder_err_mutex_;
    // ★ 注入的 guest 执行器（缺省 nullptr = 解释器 `runFeederGuest`；AOT 宿主管这个指针）。
    FeederGuestRunner runner_;
    // 独立 mixer 线程：主线程卡在 Present/长 shim 时仍持续把 guest 环送设备。
    std::thread mix_thread_;
    std::mutex mix_wait_mutex_;
    std::condition_variable mix_cv_;
    bool mix_stop_ = false;
    X86 audio_cpu_;
    GuestCallStack audio_calls_;
    NativeOverrideTable audio_overrides_;
    // ★ 段级 override 的**私有副本**（`Machine::block_overrides_` 里 `feeder_ok == true` 的那些项）：
    //   `setupFeederContext()` 在 `audio_cpu_ = *cpu_` 之后 filter + add 一份。
    //   ★ 为什么必须是副本：`hits / declines / fallbacks / disabled` 全是**非原子**裸字段；
    //     共享主表 = 数据竞争，且 feeder 的一次 `Declined` 会 `disabled = true` **永久停用主线程**
    //     那一项（反之亦然）。⛔ dispatch 不许解引用 `audio_cpu_.native_blocks`（那是快照来源）。
    //   ★ 两个执行流各自停用（副本是启动时快照）：主线程的 `disabled` 不会传给 feeder，反之亦然。
    //   ★ 计数只在 feeder 线程上写、也在同一条线程（fill 收尾）上读 ⇒ 天然无竞争；
    //     ⛔ 不要让主线程读它（`LIFT_AUDIO_TRACE` 的 `bov=` 就住在 fill 收尾那一行）。
    NativeBlockTable audio_block_overrides_;
    std::unique_ptr<Decoder> audio_decoder_;
    std::unique_ptr<BlockCache> audio_cache_;
    // 只保护私有 cache 的装/取；⛔ 不要用设备模型的 mutex_（主线程混音会长时间持有它）。
    std::mutex feeder_cache_mutex_;
    // ★ feeder 的布局（**拷贝**，不存指针）：`attach(..., const GuestLayout&)` 写入。
    //   `feeder_layout_given_ == false` = 三参过渡重载 ⇒ `resumeFeeder()` 显式 Warn + 不建线程；
    //   `feeder_layout_invalid_ == true` = 宿主给了布局但值不可用（0）⇒ `begin()` **响亮失败**。
    std::uint32_t feeder_teb_ = 0;
    std::uint32_t feeder_stack_top_ = 0;
    std::uint32_t feeder_stack_size_ = 0;
    bool feeder_layout_given_ = false;
    bool feeder_layout_invalid_ = false;
    std::uint32_t audio_stack_top_ = 0;
    // SDL 设备缓冲的帧数（`SDL_GetAudioDeviceFormat`；**设备延迟**的账，与 lead 分开报）。
    std::uint32_t dev_sample_frames_ = 0;

    void startFeederThread();
    void feederThreadMain();
    void feederEvent(const AudioNotifyEvent& ev);
    bool runFeederGuest(std::string* err);
    bool setupFeederContext(std::string* err);
    // 混音一步（调用方持有 mutex_）：推进设备游标 / 生成 / 投递通知。由 mixer 线程
    // （`mixLoop`）或夹具的 `mixOnceForTest()` 调。
    void mixStep();
    void mixLoop();
    void startMixThread();
    void stopMixThread();

    // ★ feeder 线程会调 `stop`（sub_4B6C30 的状态 0 分支），mixer 线程同时混音；
    //   设备模型/时间线必须互斥。递归：fill 内可能经 startNestedGuestCall 再进 stop。
    mutable std::recursive_mutex mutex_;
};

}  // namespace lift
