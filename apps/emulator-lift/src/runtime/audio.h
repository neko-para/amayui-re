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
    // ★ `sound=false`（缺省）= 静音：设备 / 混音 / 喂料 / guest 的 DS 调用全部照跑，
    //   只在**最终交付点**（`generateTo` 里 `out` 生成之后、`pushMixed` 之前）把缓冲置 0。
    //   ⛔ 不是"不建设备 / 不起 feeder / 提前退出 / 改 guest SetVolume"。
    bool configure(AudioMode mode, const std::string& dump_path, bool sound, std::string* err);

    bool begin(std::string* err);  // 打开 SDL 设备 / dump 文件；幂等
    void end();                    // 关闭设备、回填 WAV 大小；幂等
    bool enabled() const { return mode_ != AudioMode::Off; }
    AudioMode mode() const { return mode_; }
    // 缺省 true = 静音；`--sound` 时 false。与后端（`--audio`）正交。
    bool muted() const { return mute_; }
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

    // ---- dispatcher ----
    AudioPump pump();       // 块边界调用；可能铺一个 guest 帧
    void afterGuestCall();  // `GuestCallStack::resume` 之后调用：还回被借用的 `[esp]`
    // run 在 fill 中间结束（--time-limit / --max-insns / 停止）时回滚：
    // 还 [esp] / 以 feeder 身份 LeaveCS / --在飞 / 恢复上下文 / 丢掉 audio 帧。
    void abortFill();
    // ⚠ 只给夹具 / 单线程读：不取 `pending_mutex_`（thread 模式里 feeder 正在改这个 deque）。
    const std::deque<AudioNotifyEvent>& pending() const { return pending_; }

    void dumpSummary(std::FILE* out) const;
    // 真线程 feeder 的 shim 块按需装进私有 cache（ComRegistry 新建 vtable 时会调）。
    void installFeederShim(std::uint32_t stub_ea, std::uint32_t shim_id);

    // ---- 仅供夹具（不参与运行）----
    std::uint64_t playedBytes() const { return stats_.played_bytes; }
    std::uint64_t mixedBytes() const { return stats_.mixed_bytes; }
    // ⚠ 只给夹具 / 单线程：不取 `pending_mutex_`。
    void clearPendingForTest() { pending_.clear(); }
    // ★ 仅夹具：伪造 / 清除"某一刀 fill 正在跑"（`filling_obj_`）。0 = 清。
    //   存在理由：`filling_obj_` 只在 `feederEvent` 里置位，而 feeder 只在 thread 模式
    //   （`Sdl && realTime`）起 ⇒ 纯档 CTest 造不出"在飞 fill"，`removeBuffer` / `addBuffer`
    //   新增的等待分支在测试里永远走不到（守卫恒绿）。
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

    X86* cpu_ = nullptr;
    GuestMemory* mem_ = nullptr;
    VirtualClock* clock_ = nullptr;

    AudioMode mode_ = AudioMode::Off;
    AudioMode configured_mode_ = AudioMode::Off;
    std::string dump_path_;
    std::string last_error_;
    bool begun_ = false;
    bool sdl_ok_ = false;
    // `begin()` 时 SDL 设备**曾经**打开成功（summary 在 `end()` 之后打印，那时 `sdl_ok_` 已清）。
    bool sdl_device_opened_ = false;
    // `--audio sdl` 打不开设备、`begin()` 退回 virtual 时置位（summary 的 device=fallback）。
    bool sdl_fallback_ = false;
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

    std::vector<Buffer> buffers_;
    std::deque<AudioNotifyEvent> pending_;
    Feeder feeder_;

    // 全局时间线（单位都是"混音输出字节"；44.1k/16/2 ⇒ 4 B/frame）
    std::uint64_t played_ = 0;     // 设备已经消费的字节
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
    // ★ 方案 C 的 feeder（真宿主线程）：自己的 guest CPU / 栈 / 私有块缓存 / 私有回调栈。
    //   ⛔ 不与主线程共用 `X86` / `BlockCache` / `GuestCallStack`。
    bool thread_mode_ = false;
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
    std::string feeder_error_text_;
    // 独立 mixer 线程（方案 C）：主线程卡在 Present/长 shim 时仍持续把 guest 环送设备。
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
