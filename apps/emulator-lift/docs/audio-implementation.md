# emulator-lift 音频实现（方案 C/D）— 设备模型 + 真 feeder 线程 / 泵回退

- 状态：**已落地并升级到方案 C**（2026-11-07）。当前路线：
  - `--audio=sdl` + 宿主时钟（窗口 / `--real-time`）⇒ **方案 C**：真宿主 feeder 线程
    （自己的 `X86` / 专用 guest 栈 / 私有 `BlockCache`/`Decoder`，直接执行 `sub_4B6C30`）
    + 独立 mixer 线程；引擎每槽 `CRITICAL_SECTION` 映射到宿主递归锁。
  - `--audio=virtual` 或无 `--real-time` ⇒ **方案 D 回退**：主线程块边界的泵（确定性；CTest 用）。
- 代码落点：`src/runtime/audio.{h,cpp}` · `src/runtime/critical_sections.h` ·
  `src/runtime/sync_guest.h` · `src/shims/dsound.cpp` · `src/shims/kernel32_sync.cpp` ·
  `src/runtime/com.{h,cpp}` · `src/runtime/shims.{h,cpp}` · `src/runtime/machine.{h,cpp}` · `src/main.cpp`。
- 守卫：`tests/audio.cpp`（CTest `audio-device`，pure）。⛔ 不打开 SDL 设备。
- 本文只写**实现口径 / 实测 / 偏差**；引擎语义结论仍走 `AGENTS.md` §6 与 `data/spec/`。

---

## 0.0 2026-11-07 升级：为什么从 D 改成 C

用户实测 D 的两个问题：**SE 延迟 ~600 ms**（lead 要覆盖整个 fill）和**偶尔断裂**（主线程
Present / 长 shim 期间 mixer 不推进）。这两件事都不是"把线程体 native 化"能解决的 ——
D 本质上是**主线程协程**：decode 和 mix 都在主线程，fill 一停，音频就停。

C 把两条执行流拆开：

| 线程 | 职责 | 关键事实 |
|---|---|---|
| 主线程 | 游戏 VM / 脚本 / Present | 不再被 fill 阻塞；`pump()` 在 C 模式下无锁返回 |
| **feeder 线程** | 只跑 `sub_4B6C30`（guest 解码 + 写环） | 自己的 `X86`、专用 guest 栈（0x6FD00000..0x6FE00000）、私有块缓存/Decoder；`fs_base` 仍是主 TEB（偏差见 §5） |
| **mixer 线程** | 设备游标 / 混音 / SDL 输出 / 通知 | 每 ~5 ms 一次；主线程卡住也不断音频 |

同步边界 = **引擎自己的每槽 CS**：`EnterCriticalSection` 现在映射到宿主 per-EA 递归锁
（`CriticalSectionTable`），feeder 在真机上会阻塞的地方现在**真的阻塞**；`ComRegistry` /
`ShimRegistry` 的宿主表加锁；设备模型用 `AudioEngine::mutex_`。fill 期间 mixer / 主线程仍可运行，
环形缓冲的"读当前 1/4 圈、写刚放完 1/4 圈"保持不重叠。

实测（RelWithDebInfo，48k stereo BGM，`--native-overrides all`）：

```text
feeder fill：381–462 ms（产 0.5 s 音频；与主线程/mixer 并发）
thread lead：100 ms；SE 延迟 ≈ 100 ms
30 s 实时：fills=52  notify=52  underruns=0
dump：4–29 s 连续，RMS 4.5k–6.4k；lag2 相关 ≈ 0（不是 2 s 环）
```

⇒ D 的设备模型 / 通知 / 逐缓冲格式 / 混音代码**原样复用**；C 只换了"谁在什么线程执行
decode 与 mix"，并补了宿主锁层。下面的 §1–§4 仍准确描述设备模型；§2 的泵路径只在
virtual/无 real-time 时走。

---

## 0. 一句话（D 回退路径）

**设备是宿主的，PCM 是 guest 的，喂料是主线程在块边界上的一次嵌套 guest 调用。**

```
dsound shim (宿主)                     AudioEngine (宿主)                          guest
─────────────────                     ─────────────────────                       ─────
CreateSoundBuffer ───────────────► addBuffer(state,obj,data,bytes,WAVEFORMATEX)
SetNotificationPositions ────────► setNotify(4 × {offset,event})
Play / Stop / SetCurrentPosition ─► 设备状态机
                                       │
SDL 设备已消费字节 ────────────────► advanceDevice() ── 越过通知点 ──► pending 队列
                                       │                                   │
                                       │   dispatcher 块边界 ◄─────────────┘
                                       │        │
                                       │        └─ startNestedGuestCall(0x4B6C30,{quarter},ECX=stream)
                                       │                                   │
                                       │   guest sub_4B6C30 解码/写环 ◄────┘
                                       │        │
                                       │   continuation：--在飞 / LeaveCS / 恢复整份上下文
                                       │
混音（读 guest 环）◄────────────────  generateTo(played+lead) ──► SDL_PutAudioStreamData
```

---

## 1. 设备模型

### 1.1 三条缓冲角色（实测，与 `.lst` 一致）

| 角色 | bytes | WAVEFORMATEX（实测） | 说明 |
|---|---|---|---|
| primary | 0 | 44.1k/2ch/16（`SetFormat` 后可变） | 不参与混音；保留返回码 |
| 静音垫 | 176400 | 44.1k/2ch/16 | 引擎 memset 0 后循环 Play；不参与可听输出 |
| 每槽流 | 176400 / 384000 | **44.1k/1ch/16 或 48k/2ch/16** | 真正的 BGM / SE / 语音；全部参与混音 |

★ 早先的评估（`docs/audio-feasibility.md` §2.1(4)）只看到"44.1k/16/2"那一处 `WAVEFORMATEX`；
本次实跑发现 Ogg BGM 会建 **48k stereo / 384000 B（2 s）**，SE 会建 **44.1k mono / 176400 B**。
⇒ 实现必须**逐缓冲**读格式并做采样率/声道转换，⛔ 不能全局写死 44.1k stereo。

### 1.2 游标与通知

- 宿主输出固定 **48k / S16 / 2ch**（`kSampleRate=48000`，`kBytesPerSec=192000`）。
- 每条缓冲记录 `initial_pos`（`SetCurrentPosition`）、`start_mixed`（开始播放时的全局输出字节）、
  `WAVEFORMATEX` 的 `avg_bytes`。某条缓冲的"已播源字节" =
  `(played_out_frames - start_out_frames) * avg_bytes / 48000`；`% bytes` 得 `GetCurrentPosition`。
- 通知点 `dwOffset` 是**该缓冲自己的字节**；越过时入队 `(obj, quarter)`，`quarter` = 数组下标（0..3）。
  **一次推进跨过多个点时必须按位置排序**再入队 —— 否则回绕 0 会排在 3/4 之前（实测错序：
  0,1,2,0,3…；修复后 0,1,2,3,0…）。
- 入队去重（同一 `(obj,quarter)` 未消费时不重复入队）：对应 auto-reset 事件的合并语义。

### 1.3 混音

- 逐缓冲线性插值重采样到 48k；mono 复制到 L/R；16-bit 与 8-bit 支持（实测只有 16-bit）。
- 音量 `1/100 dB → 10^(v/2000)`；pan `-10000..10000` 线性左右衰减；int32 累加后 clip 到 int16。
- 混音只读 guest 环的"正在播"区间；引擎在**刚放完**的 1/4 圈写，所以写区间永远在播放游标之后
  （1.5 s 以上）。D 里两者不会同时执行：fill 期间 `pump` 直接返回 Idle，⛔ 不混音。
- `--audio-dump` 与 `sdl` 可同时用（tee）；dump 是 48k/16/2 WAV。

### 1.4 lead（C = 100 ms；D 回退 = 600 ms）

- **C（真线程）**：mixer 独立运行，fill 不再阻塞混音 ⇒ 队列只需覆盖 mixer 5 ms 周期 + 抖动；
  当前 **lead = 100 ms**，SE 延迟 ≈ 100 ms。
- **D 回退**：fill 期间暂停混音，队列必须撑过整个 fill（~370 ms）+ 到下一通知点的 140 ms
  ⇒ **lead = 600 ms**。
- C 实测（RelWithDebInfo、`--native-overrides all`、48k stereo BGM）：
  ```text
  feeder fill 381–462 ms；30 s：fills=52 / notify=52 / underruns=0
  dump 4–29 s 连续，RMS 4.5k–6.4k，lag2 ≈ 0
  ```
- ⚠ **`--audio=virtual` 不能当实时吞吐判据**：虚拟时钟是"每条指令 100 ns"，一次 fill 的虚拟时间
  远大于它产出的 0.5 s 音频 ⇒ 虚拟档下泵会落后、dump 出现 2 s 环。这是**时钟档**的性质；
  实时验收必须用 `--real-time`/窗口 + `sdl`。

---

## 2. 喂料泵（native 控制流）

### 2.1 线程身份

| 事件 | 落点 | 行为 |
|---|---|---|
| `CreateThread(0x4B5AA0, Sound*, flags=4)` | `kernel32_sync.cpp` | 句柄照发；`AudioEngine::registerFeeder` 读 `Sound+0x514`（退出事件 #60）；**不执行线程体** |
| `ResumeThread(h)` | 同上 | 挂起计数 1→0；`resumeFeeder()` 置 runnable |
| `SetEvent(quit)` | `shimSetEvent` | `noteEventSet(h)` 识别 #60；下一个块边界退出（清 pending） |
| `WaitForSingleObject(h, 3000)` | `waitOne` | `isFeederHandle` ⇒ `joinFeeder()`，返回 `WAIT_OBJECT_0`；此后不再写 guest 环（防 UAF） |
| `GetCurrentThreadId`（fill 内） | fill 上下文 | `thread_id = feeder id`（0x1008），fill 结束恢复主线程 id |

其余 `CreateThread`（DirectShow 读文件）仍是伪线程，行为不变。

### 2.2 一次 fill 的 12 段 native + 1 段 guest

`sub_4B5AA0` 的循环体（`docs/thread-model-feasibility.md` §5.1 D.1）在泵里逐段重排：

1. 从 pending 取一条 `(obj, quarter)`；
2. `findSlot(obj)`：扫 `[Sound+0x654+4s]` 的 15 个 `SoundBuffer*`，匹配 `[stream+0x40C] == obj`；
3. CS = `Sound+0x690+24s`；若 `[CS+0x0C]` 是**别的**线程 ⇒ **defer**（保留队首，下个块边界再试）；
4. `guestEnterCriticalSectionAs(cs, 0x1008)`（写 guest 可见的 RecursionCount / OwningThread）；
5. `++[Sound+0x7F8+4s]`（在飞计数）；
6. `startNestedGuestCall(0x4B6C30, {quarter}, cleanup=0, ECX=stream)`；
7. 续延：`--在飞`；`guestLeaveCriticalSectionAs`；恢复整份上下文；`fills++`。

### 2.3 整份上下文交换（D 的头号静默风险）

泵在**块边界**插入，主线程的 x87 栈可能正活着。fill 结束后 Ogg 解码器会把 x87/XMM 改成任意值；
只存 8 个 GPR 会让主线程后续浮点静默用错值。
保存/恢复：`r[8]` / `eip` / `eflags` / `seg[6]` / `fs_base` / `gs_base` / `X87State`（含 control/status/top/tag/
8×16 B 寄存器）/ `xmm[8][4]` / `shim.thread_id` / `shim.last_error`。

### 2.4 借 `[esp]` 当返回点（不改 `GuestCallStack`）

`startNestedGuestCall` 从 `[esp]` 取"返回地址"，而 `resume()` 在续延返回后会把 `eip` 设成它。
泵需要回到**块边界**（不是当前函数的调用者），所以：

1. 保存 `saved_esp = esp`、`saved_ret = [esp]`、`snapshot = 全上下文`；
2. 把 `[esp]` 临时写成 `snapshot.eip`；
3. `cleanup_bytes = 0` ⇒ `resume()` 最终设 `eip = snapshot.eip`、`esp = saved_esp`、`eax = snapshot.eax`；
4. 续延里恢复其余字段，并把 `*shim_result = snapshot.eax`；
5. dispatcher 在 `resume()` 之后调用 `audio_.afterGuestCall()`，把 `[esp] = saved_ret` 还回去。

`sub_4B6C30` 是 thiscall + 1 个栈参数 ⇒ `cleanup_bytes = 0`；它的 `retn 4` 自己弹掉参数。
若铺帧失败：恢复 `[esp]` / 上下文 / 在飞计数 / CS，并让 `pump()` 返回 Error（dispatcher 停机器）。

### 2.5 单执行流纪律

- `pump()` 在 `cpu.calls->active()` / `in_fill_` / `timer_firing` 时**直接返回 Idle**；
- dispatcher 在嵌套 guest 调用期间**不 pump 窗口消息、不触发 winmm 定时器**（`nested_guest_active`）：
  否则 WndProc / 定时器会插进 fill 的 x87 活值中间，或读 guest 环正在写的区间；
- 混音只在块边界、fill 之外进行。

---

## 3. DirectSound shim 的改动

| 方法 | 之前 | 现在 |
|---|---|---|
| `QI(IDirectSoundNotify)` | 建一个**无状态**对象 | 建 notify 状态块（owner = buffer 状态 EA），`SetNotificationPositions` 能找回 buffer |
| `SetNotificationPositions` | 恒 `DS_OK`、不读参数 | 校验 count ≤ 4 / 参数已提交；存 4×`{offset,event}`；`audio->setNotify(owner, offsets)` |
| `Play` / `Stop` / `SetCurrentPosition` | 只写 guest 状态 | 同时驱动 `AudioEngine` 状态机 |
| `SetVolume` / `SetPan` | 只记 guest 字段 | 同时更新混音增益（guest 字段仍是权威） |
| `Unlock` | 游标 = 累计 PCM 字节 | 有音频引擎时**不覆盖**设备游标 |
| `GetCurrentPosition` | 返回累计 Unlock 字节 | 有引擎时返回 `audio->playedCursor(state)`（%bytes） |
| `createBufferObject` / `Duplicate` / `Release` | 只分配/回收 guest 块 | 同时 `addBuffer` / `removeBuffer`；`Release` 先让引擎忘掉该流再 free |
| `Release(Notify)` | 会被当成设备打错账本 | 只释放 notify 接口状态块；**位置留在 buffer**（真 DS 语义，⛔ 不 clearNotify） |

`audioFormatOf(Wfx)` 把 `WAVEFORMATEX` 的 `samples/channels/bits/block_align/avg_bytes` 交给引擎；
没有 fmt 的 primary 给 44.1k/16/2 占位（bytes=0，不混音）。

---

## 4. 实测验收（2026-11-07）

### 4.1 实时（能听到）

**C（真线程，当前默认）**：

```text
out\emulator-lift.exe --game-dir <install> --appdata <fresh> --skip-logo \
  --native-overrides all --present headless --real-time --audio sdl \
  --audio-dump .tmp/audio-thread-final/title.wav --time-limit 30
```

```text
[audio] feeder=host-thread（方案 C）；lead=19200 B（100 ms）
fills=52  notify=52  deferred=0  dropped=0  underruns=0
played=5,794,560 B (30.2 s)   mixed=5,813,760 B (30.3 s)
feeder fill 墙钟：381–462 ms（与主线程/mixer 并发）
dump：4–29 s 连续；RMS 4.5k–6.4k；lag2 相关 ≈ 0（不是 2 s 环）
```

**D 回退（`--audio=virtual` / 无 `--real-time`）**：

```text
fills=30  notify=30  underruns=0；fill 344–425 ms；lead 600 ms；SE 延迟 ≈ 600 ms
```

### 4.2 虚拟（确定性 / dump）

```text
--audio virtual --audio-dump ... --max-insns 400000000
filled=21–23（虚拟时钟下泵会落后；dump 出现 2 s 环）
```

⇒ 这是虚拟时钟的性质（见 §1.4），**不是**实时输出的结论。CTest 用虚拟档只验证设备模型 /
通知次序 / 逐字节混音，不验证"实时吞吐"。

### 4.3 守卫

`ctest --test-dir apps/emulator-lift/out -R audio-device`：

- 48k stereo：通知 0,1,2,3,0 次序；`Stop` 冻结当前游标；`SetCurrentPosition` 重排；
  `removeBuffer` 清 pending（COM `Release → destroyBufferBlocks → removeBuffer` 是同一入口）；
- **一次 2.5 s 的大推进**：1/4、1/2、3/4、回绕 0 必须在同一个 `fireCrossings` 里按位置排序
  （删掉 sort ⇒ 该用例红）；
- dump 前 0.5 s 与 guest 环斜坡**逐字节**相等；
- 44.1k mono：重采样到 48k，L==R；第 1000 个输出帧等于线性插值期望值（不是只查第 0 帧）；
- 喂料身份：register/resume/quit/join；
- 泵的整份上下文：fake `sub_4B6C30` 改掉全部 GPR / EFLAGS（`add eax,1`）/ x87（`fld1`）/
  XMM（`movd`）⇒ 返回后逐字段复原；**铺帧后立刻**断言 CS owner=0x1008、RecursionCount=1、
  在飞=1；返回后 CS/在飞成对归零；`[esp]` 还回原返回地址。

---

## 5. 明确偏差 / 未做

1. **guest 的 61 个位置通知事件不 SetEvent**：喂料线程已 native 化，没有 guest 等待它们；
   引擎也不在别处等。`SetNotificationPositions` 仍保存 `hEventNotify`，但投递走宿主 pending 队列。
   若将来有 guest 代码轮询这些事件，需要补"同时 SetEvent + 消费"。
2. **join**：C 模式是 `SetEvent(quit)` + 真 `std::thread::join()`（join 前 `releaseAll` 主线程 CS）；
   D 回退仍是"直接置 exited / 清 pending / 返回 WAIT_OBJECT_0"。
3. **不是通用并行**：C 只给音频开了 feeder + mixer 两条宿主线程；有自己的 `X86` / guest 栈 /
   私有块缓存，但 **`fs_base` / TLS 仍用主线程的**，也没有 x86 TSO 全局软模拟。并发正确性只依赖
   引擎自己的每槽 CS 协议 + 环形缓冲"读当前圈 / 写刚放完圈"不重叠；游戏 VM/脚本仍单线程。
   ⛔ 若把更多 guest 代码放线程，才需要完整 TSO / 每线程 TEB-TLS。
4. **SE 延迟 ≈ lead**：C = **100 ms**；D 回退 = 600 ms。C 的 mixer 独立，fill 慢不再直接变成 SE 延迟。
5. **重采样是线性插值**：48k→48k 直通；44.1k→48k 有轻微高频损失。引擎不检查宿主格式，
   但 PCM 不是逐位等于真机（真机由 DS 混音器重采样）。
6. **满音量/pan 近似**：`10^(v/2000)` + 线性 pan，未做 DS 的精确 pan law / 扬声器几何。
7. **fill 吞吐是硬约束**：RelWithDebInfo 下 370 ms / 0.5 s；更慢的宿主或 Debug 构建会 underrun。
   ⛔ 不要用 Debug 跑音频（用户口径：Debug boot > 30 s）。
8. **TITLE BGM 已实测**；hover SE 走同一条 `Play → notify → fill → 混音` 路径，但"鼠标悬停触发
   哪个槽"属于输入/脚本路径，本次未做自动化点击验收。
9. **非 LOOPING 的 `Play` 仍按循环处理**：引擎所有流式 `Play` 都是 `DSBPLAY_LOOPING=1`
   （`.lst` 0x4B756F / 0x4B75CB / 0x4B5FB3），所以这条没有实际影响；`looping` 已存但混音器
   没有按它截断。若将来有非循环流，需要补"到 bytes 停止 + GetStatus 更新"。
10. **同一 `Machine` 只支持一次音频 run**：`begin()` 发现上一轮的 `feeder_` / `buffers_` 还在
    ⇒ 明确返回错误（`Machine::run` 打 `[audio] begin 失败`），⛔ 不静默清空设备模型。需要第二次
    run 就新建 `Machine`。
11. **fill 中结束会回滚**：`--time-limit` / `--max-insns` / 停止正好落在 `sub_4B6C30` 中间时，
    `AudioEngine::abortFill()` 会还 `[esp]`、以 feeder 身份 LeaveCS、`--在飞`、恢复上下文、
    丢掉 audio 帧；`r.ea` 仍是停在 fill 内的真实 EIP（诊断用）。日志有一行 Warn。
12. **Lock 的窗口被收紧**：`offset >= bytes` / `dwBytes > bytes` / `DSBLOCK_ENTIREBUFFER`
    + `offset != 0` 都返回 `DSERR_INVALIDPARAM`；`Unlock` 同时校验 p2/n2。引擎只锁 1/4 圈，
    不受影响；这是防 guest 误用导致越界写。
13. **音量 / pan 先钳位再混音**：`SetVolume` 钳到 `[-10000, 0]`，`SetPan` 钳到 `[-10000, 10000]`；
    累加器用 int64（防多个满增益流把 int32 撑溢出）。DS 的精确 pan law 仍未做。
14. **TLS / TEB 不切换**：C 有独立 feeder 线程和 `X86`，但 `fs_base` 仍指向主线程 TEB；
    fill 里 `TlsSetValue` 会写进程级 TLS 数组，`fs:[0]` SEH 用主线程链。Ogg 解码不依赖它们
    （prefill 已在主线程跑同一函数）；这是 C 的窄边界，不是"每线程 TEB/TLS"的通用实现。
15. **notify 接口 Release 不清位置**：位置属于 buffer（真 DS 语义）；`Release(IDirectSoundNotify)`
    只释放接口状态块，AudioEngine 里的 4 个 offset 保留到 buffer `removeBuffer`。⛔ 不要改回
    "Release notify ⇒ clearNotify"，那会让正常 COM 用法直接静音。

---

## 6. 怎么查 / 怎么改

- **听不到**：先看 `[audio] mode=...` 与 `SDL 音频设备：...`；没有设备会退回 `virtual`（有日志）。
  再看 `fills` / `underruns`：`fills=0` ⇒ 没进喂料（`CreateThread` 没命中 0x4B5AA0 或音频 Off）。
- **有爆音/断续**：先看 `[audio] feeder=...`。
  - `host-thread`：mixer 独立；`underruns` 来自 mixer 线程本身（SDL 设备/主线程不再直接相关）。
    看 `LIFT_AUDIO_TRACE=1` 的 feeder fill 墙钟；若持续 >500ms，feeder 会落后（BGM 圈重复）。
    检查是否有新的长 shim 持锁（`ComRegistry`/`ShimRegistry` 已改成 fn 锁外执行）。
  - `主线程泵`：fill > lead ⇒ 增大 `kLeadBytes` 或优化解码。
  ⛔ 不要先怀疑混音；先用 `underruns` / fill 墙钟定位。
- **线程**：C 模式三条线程 = 主线程 / feeder / mixer；`--audio=virtual` 或没有 `--real-time` 会
  自动回退成单线程泵（日志 `feeder=主线程泵`）。
- **音调不对**：检查该缓冲的 `WAVEFORMATEX` 是否被 `audioFormatOf` 正确带进 `AudioFormat`；
  特别是 48k stereo 与 44.1k mono。
- **通知错序**：`fireCrossings` 必须先收集 `(pos,quarter)` 再排序；⛔ 不要按 quarter 循环直接入队。
- **改设备模型**：`tests/audio.cpp` 的 48k 斜坡 / 44.1k mono 两个用例是机械判据；先让它们红/绿。
