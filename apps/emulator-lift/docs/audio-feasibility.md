# emulator-lift 音频可行性评估（只读）

- 范围：`apps/emulator-lift` 的音频相关 shim / 伪线程 / 引擎音频路径；语义锚 = `.lst` 的 EA。
- 方式：只读代码 + 只读语料（`disasm-at` / `disasm-fn` / `rg`）。⛔ 未改任何源码、未 `git add`、未运行游戏。
- 证据标签：**【实测】** = 本次用工具机械读出/逐字节比对过；**【静态推断】** = 从 `.lst` 指令 + SDK 常量读出的语义（可再核）；**【待核】** = 明确没核。
- 引用纪律：不引用 `apps/emulator` / `apps/emulator-new`（已废弃）。`apps/emulator-cxx` **未使用**（本报告没有一条结论依赖它）。

> **2026-11-07 追加（只读）**：`sub_4B5AA0` 的控制流可以**完全 native override**（只留
> `0x4B5B03 call sub_4B6C30` 给 guest），**不需要第二个 guest 栈**；详见
> `thread-model-feasibility.md` §5.1 的**方案 D**。本文下面的“协作式 guest 线程”是 D 的
> **后备**（B），不是唯一路线；D 优先，但仍需用户裁决（V1 路线）。
>
> **2026-11-07 实施记录**：V1 已裁决为 **D** 并落地，见 `docs/audio-implementation.md`。
> 实跑纠正了本文 §2.1(4) 的“只有 44.1k/16/2”：BGM 会建 **48k stereo**，SE 会建 **44.1k mono**
> ⇒ 逐缓冲格式转换是必需的。§3.5 的 P3-D 已完成（不建第二栈）；实测 fill ≈ 370 ms / 0.5 s。

---

## 0. 结论（先看这个）

**能直接实现，但卡在一条模型缺口上，而不是卡在"音频复杂"。**

1. **音频 DSP 面确实很薄**（支持用户的前提）：全引擎只有 **16bit / 44.1kHz / 立体声 PCM** 一种格式，**没有 3D / EAX / SetFX / 效果链**，**没有 MIDI / CD-DA / waveOut 后端在用**，**没有采样率转换需求**（设备按 44.1k 打开即可）。证据：`flags=0x181C8`、`WAVEFORMATEX` 全是 44.1k/16/2（EA `0x4B5EF6..0x4B5F0B`、`0x4B7202..0x4B7214`）；`waveOut` / `PlaySound` / `sndPlaySound` 在整份 `.lst` 里 **0 次**；`+0x10 GetCurrentPosition` / `+0x24 GetStatus` / `+0x44 SetFrequency` / `+0x54 SetFX` 在音频模块内 **0 次调用**（见 §2.1）。
2. **卡点是"消费端"**：能出声的 PCM 由 guest 自己的流式喂料线程产生。`sub_4B5CF0` 在 `0x4B5FB5..0x4B5FD4` 建 61 个事件、`0x4B5FE4` 建 `CreateThread(sub_4B5AA0)`；`sub_4B5AA0@0x4B5AB6` 用 `WaitForMultipleObjects(0x3D, …, INFINITE)` 等 DirectSound 的**位置通知事件**，每收到一个就 `sub_4B6C30(slot, quarter)` → `Lock/Unlock` 把**guest 解码器**（`WavePCM` / `OggVorbis` / `OggStream`）产出的 PCM 写进环形缓冲。**线程不跑 ⇒ 环里只有主线程在 Play 前预填的 3/4 圈**（`0x4B74B4..0x4B74C0` 的 `for(i=1;i<4)`），即最多 **1.5 s**；之后环空转/循环。⇒ 没有"消费端"，就只能听到 1.5 s（SE 短音能整条听完；BGM 只播 1.5 s 然后 0.5 s 静音循环）。【实测：`0x4B5FE4` / `0x4B5AB6` / `0x4B74B4`；静态推断：1.5 s 上限】
3. **要连续 BGM/语音，必须先解决“消费端”**。有两条路线：**D（首选）** 把 `sub_4B5AA0` 的控制流
   native 化（循环体只有 `0x4B5B03 call sub_4B6C30` 一处必须 guest），在主线程的确定性边界
   调用它，**不建第二个 guest 栈**；**B（后备）** 让一段 guest 代码在第二个栈上跑（协作式）。
   B 与 `README.md §2.5`（"⛔ 不把 guest 代码放多线程"）和 `SCOPE.md §1`（伪线程）**直接冲突**；
   D 不建线程，但仍是单线程口径的扩展。两条都必须先由用户裁决（见 thread 报告 V1）。
4. **宿主化（SDL 音频回调）只解决"送采样"，不解决"产采样"**。宿主回调可以从 guest 环里拉样本（这半件事很干净）；但它拉到的样本是 guest 线程写进去的。把"产采样"也搬到宿主 = 在宿主重写一遍 `SoundBuffer` + 三个解码器 + 15 槽语义，**工作量更大、语义更容易假**，不推荐（§3.1）。
5. **没有一个环节要求"逐位等价"才能开工**：整条链上 guest 观察得到的只有"事件有没有按时投递 / 返回码 / 状态字 / 音量值"。⇒ 可以分三刀落地（§3.6），第一刀 **3–5 人日**就能让标题 SE 出声并把 notify 机制钉进守卫。

| 判定 | 结论 |
|---|---|
| 音频 DSP 复杂度 | 低（单格式、无 3D、无效果、无重采样） |
| 音频**模型**复杂度 | 中（15 槽环形缓冲 × 4 位置通知 × 61 事件 + 一个喂料线程） |
| 能否直接实现 | **能**；分界点是"走 D（控制流 override）还是 B（协作式第二栈）" |
| 不裁决也能做的 | DS 设备模型（游标 + 位置通知 + 状态机）+ 宿主 sink 接口 + `--audio-dump`（第一刀） |
| 不裁决就做不到的 | 连续 BGM / 语音 / 循环点（>1.5 s 的任何内容） |
| 需要用户裁决 | ① 路线 D/C（thread 报告 V1）；② real-time 时钟口径（V4）；③ 分阶段验收 |

---

## 1. 现状盘点（代码 + `.lst`，不凭记忆）

### 1.1 音频相关 shim / stub / 记账

| 落点 | 文件:行 | 注册方式 | 返回 / 行为 | guest 可观察 | 留痕 |
|---|---|---|---|---|---|
| `DirectSoundCreate` | `src/shims/dsound.cpp:1194-1247`，注册 `:1342` | `add`（实现） | `DS_OK(0)` + guest 堆上的 `IDirectSound8` COM 对象（11 槽 vtable） | 非 0 对象 + 可调方法 | 每次创建一行 |
| `IDirectSound8` 方法表 | `dsound.cpp:1279-1294` | — | QueryInterface / AddRef / Release / CreateSoundBuffer / GetCaps / DuplicateSoundBuffer / SetCooperativeLevel / Get/SetSpeakerConfig 为真值；**Initialize(+36) / VerifyCertification(+40) 为 loud trap** | 见 §2.1 | 首次调用打一行 + Release 时打总账 |
| `IDirectSoundBuffer8` 方法表 | `dsound.cpp:1306-1334` | — | 见 §2.1；**Initialize(+40) / SetFX(+84) / AcquireResources(+88) / GetObjectInPath(+92) 为 loud trap** | 见 §2.1 | 同上 |
| `IDirectSoundNotify` | `dsound.cpp:318-342`，表 `:1296-1304` | — | QI 成功（**新建一个独立 COM 对象**）；`SetNotificationPositions` **不读参数、不存、不投递事件**，恒 `DS_OK` | 拿到一个对象；`DS_OK` | 只打首次 |
| MCI ×3 | `src/shims/winmm_audio.cpp:29-46`，注册 `:278-283` | `addStub` | `mciSendCommandA`/`mciSendStringA` → 0；返回串写空；`mciGetErrorStringA` → FALSE | 成功/空串 | 每次一行 |
| mixer ×3 | `winmm_audio.cpp:48-64`，注册 `:284-289` | `addStub` | 成功 + 假句柄 `0x00030001`；不作用于宿主 | 成功 + 句柄 | 每次一行 |
| mmio ×5 | `winmm_audio.cpp:123-273`，注册 `:290-299` | `addStub`（**但实现是真的**） | 沿 guest 文件句柄的同一个 `FILE*` 真解析 `RIFF`/`fmt `/`data`；`mmioClose` 不关宿主文件；未知 flag → `265` | 真字节 / 真失败 | Debug 级 |
| `IBasicAudio` | `src/shims/quartz_video.cpp:889-896` | 分派 | `get_Volume`/`get_Balance` → 0；`put_*` → `S_OK` | 0 / `S_OK` | 有 |
| MF 音频渲染器 | `src/shims/mf_video.cpp:56-58`，注册 `:112-113` | `addStub` | `MFCreateAudioRendererActivate` → `E_FAIL` + 空输出 | 失败 | 有 |
| `CreateThread` | `src/shims/kernel32_sync.cpp:1081-1117`，注册 `:1269` | `add`（**伪线程**） | 发真句柄 + 真 thread id（`0x1008` 起 +4）+ 真 `lpStartAddress`/`lpParameter` 记账；**永不执行入口** | 句柄、id、`ResumeThread` 旧值、`SetThreadPriority` 成功 | 一行 |
| `_beginthreadex` / `ExitThread` / `CreateRemoteThread` / `CreateTimerQueue*` / `DeleteTimerQueueTimer` | `kernel32_sync.cpp:1276-1289` | `addUnimplemented`（**硬 trap**） | 执行到就停机器 | — | trap |
| `timeSetEvent` / `timeKillEvent` / `timeGetDevCaps` | `src/shims/kernel32_time.cpp:237-320` | `add`（**协作式真定时器**） | 登记进 `cpu.shim.timers`，dispatcher 在**块边界**按虚拟时钟把 guest 栈铺成回调帧；`TIME_CALLBACK_EVENT_SET/PULSE` 返回 0（不支持） | 真 id / 真回调 | 有 |
| 伪线程模型 | `src/runtime/x86.h:126-128`（`next_pseudo_thread_id`）、`:81-165`（计时器 / 回调状态） | — | 见上 | — | — |
| 口径 | `SCOPE.md:21`（音频 silent）、`:25`（伪线程）、`:40-45`（线程=服务线程的依据）、`:59-63`（放弃了什么）；`README.md:105-117`（§2.5 线程模型） | — | 见 §1.2 | — | — |

**mmio 的记账缺陷（记账与实现不一致）**：`winmm_audio.cpp:290-299` 的 stub 描述写的是"返回非空假 HMMIO（**空流**）"、"mmioRead 返回 0 字节"，而 `:123-273` 的实现是**真 RIFF 解析**（`REQ-01M6QSVR5NJDQVFHY6RX3934DN` 修完后改了代码、没改描述）。`pnpm tools lift report` 那类"缺口清单"读的是注册描述 ⇒ 现在它会说 mmio 是空流。这是一条**记账失真**，建议在实现单里顺手修（不改行为，只改描述字符串）。

### 1.2 现在的"silent"是怎么被消化的

调用链（EA 全部可核）：

```
_WinMain → sub_406CE0@0x406CE0（读 sound:UseDirectSound / DirectSoundCooperativeLevel）
  ├─ sub_4B5C50@0x4B5C50  LoadLibraryA("DSOUND.DLL")@0x4B5C69 → [obj+0x418]
  │                        GetProcAddress("DirectSoundCreate")@0x4B5CA5 → [obj+0x41C]
  └─ sub_4B5CF0@0x4B5CF0  dsCreate（**Sound 对象 = 0x4B5B50 的 ctor 建的**）
       ├─ DirectSoundCreate(NULL,&obj+0x408,NULL)        @0x4B5D6C   ⇒ shim DS_OK + 对象
       ├─ SetCooperativeLevel(obj+0x408, hwnd, level)    @0x4B5DDD   ⇒ shim DS_OK（只记账）
       │    level 映射：配置 0→3(EXCL) / 1→2(PRIO) / 其它→1(NORM)   @0x4B5DAA..0x4B5DD0
       ├─ CreateSoundBuffer(primary: dwSize=0x24,flags=0xC1,bytes=0,fmt=0)
       │                                                 @0x4B5E61   ⇒ shim DS_OK + 1 字节占位 PCM
       ├─ Play(primary,0,0,1)                            @0x4B5EAD   ⇒ shim DS_OK
       ├─ CreateSoundBuffer(stream: dwSize=0x24,flags=0x181C8,
       │                    bytes=176400, WFX 44.1k/16/2)@0x4B5F19   ⇒ shim DS_OK + 真 guest 块
       ├─ Lock(stream,0,176400,…)                        @0x4B5F77   ⇒ shim 给真 guest 指针
       ├─ memset(ptr,0,176400)                           @0x4B5F83   ⇒ **引擎自己写的是 0**
       ├─ Unlock(...)                                    @0x4B5FA3   ⇒ shim 记账（pcmBytes=176400, nonzero=0）
       ├─ Play(stream,0,0,1)                             @0x4B5FB3   ⇒ shim DS_OK
       ├─ 61 × CreateEventA                              @0x4B5FC8   ⇒ shim 真事件对象（**全 unsignaled**）
       ├─ CreateThread(sub_4B5AA0, this, flags=4)        @0x4B5FE4   ⇒ **伪线程：句柄真、线程体不跑**
       ├─ ResumeThread / SetThreadPriority(h,2)          @0x4B5FF1 / 0x4B6000 ⇒ 返回形态值
       └─ return 1
```

`guest 能观察到的"成功"` = 上面每一步的返回码都真（0 / 非 0 句柄）。**只有两处是假的**：

1. `SetNotificationPositions` 成功但**不投递事件**（`dsound.cpp:339-342`）；
2. `CreateThread` 成功但**线程体不执行**（`kernel32_sync.cpp:1112-1116` 自己打了"不执行 start routine"）。

于是引擎停在"以为设备在放、以为喂料线程在跑"的状态，而实际是：**61 个事件永远 unsignaled、环形缓冲永远只有初始化时 memset 的 0**。这是 SCOPE §1 允许的"silent 但记账"，不是 no-op 失忆。

**真机日志锚**：`.tmp/a9-thread.md`（2026-11-05 实测）记下了 silent 层落地后的现场：

```
[dsound] Unlock: p1=0x10246E38 n1=176400 ⇒ DS_OK（本区间非 0 字节 0；累计 PCM 字节 176400，plays=0）
  kernel32!CreateThread  (stub 0x7FFE01CC, …)
stop      : unimplemented @ eip=7FFE01CC      insns = 109,536,868
```

⇒ **当时**停在线程；伪线程落地后越过它，但"线程体不执行"这一事实没变（`SCOPE.md:25`）。本次按要求没有重跑游戏，所以"当前 boot 到哪一帧"没有新现场；§1.3 / §2 的结论全部来自代码与语料。

### 1.3 引擎实际调用点（`.lst`）

#### (a) DirectSound —— 只有 1 个导出、1 个设备对象、2 类缓冲

| 项 | 证据 | 说明 |
|---|---|---|
| `DirectSoundCreate` | 字符串 `.data:0x52CEA0`；`push` `0x4B5C9F`；`GetProcAddress` `0x4B5CA5` | **唯一**取的导出 |
| `DirectSoundCreate8` | 全 `.lst` **0 次** | 不是"定义了没调"，是**根本没有** |
| `DirectSoundEnumerateA` / `DirectSoundCaptureCreate` / `DirectSoundCaptureEnumerateA` | 全 `.lst` **0 次** | 同上 |
| `IDirectSound` / `IDirectSound8` 字符串 | `0x4B7889` 的 `IDirectSound::Initialize メソッドが呼び出されなかった` 等 **仅错误串** | 不是接口引用 |
| 设备对象（`Sound::`） | ctor `sub_4B5B50@0x4B5B50`（`Sound::vftable` @`0x4B5B8D`），内嵌在引擎对象 `Engine+0x48A8`（`sub_406CE0` 的 `this+4666` dwords = `+0x48A8`） | 一台设备 |
| 15 槽 + 61 事件 + 线程句柄 | `[Sound+0x654+4s]` 槽指针 / `[Sound+0x690+24s]` 临界区 / `[Sound+0x7F8+4s]` 在飞计数 / `[Sound+0x424+4j]` 事件（61 个） / `[Sound+0x514]` 退出事件（=事件#60） / `[Sound+0x518]` 线程句柄 | 常量：`0x3D`=61、`0x18`=24、界 15 |
| 每个槽的流缓冲 | `CreateSoundBuffer` @`0x4B5F19`（设备启动时那个 176400 的"静音垫"）与 @`0x4B723F`（每槽 352800） | 见 §2.2 |

#### (b) DirectSound 对象的 vtable 槽调用点（音频模块 `0x4B5xxx–0x4B7xxx` 内**逐条枚举**；`+0x0C/+0x18` 属设备，其余属缓冲）

| 槽 | 方法 | 调用点 EA | 用在哪 |
|---|---|---|---|
| `+0x00` | QueryInterface | `0x4B7297` | 在**流缓冲**上 QI `IID_IDirectSoundNotify`（IID 数据在 `0x52E298`） |
| `+0x08` | Release | `0x4B7192` / `0x4B71AE`（`sub_4B7170`）· `0x4B64D4` / `0x4B64F8`（`sub_4B6440`） | 换缓冲 / 关机 |
| `+0x0C` | CreateSoundBuffer | `0x4B5E61`（主）· `0x4B5F19`（静音垫）· `0x4B723F`（每槽流） | — |
| `+0x18` | SetCooperativeLevel | `0x4B5DDD` | 一次 |
| `+0x2C` | Lock | `0x4B5F77`（静音垫）· `0x4B6CEA`（流，含回绕）· `0x4B6D7B`（Restore 后重试） | — |
| `+0x30` | Play | `0x4B5EAD`（主）· `0x4B5FB3`（静音垫）· `0x4B756F` / `0x4B75CB`（流；`sub_4B73E0`） | 全部 `dwFlags=1`（LOOPING） |
| `+0x34` | SetCurrentPosition | `0x4B743C` / `0x4B7514`（`sub_4B73E0`，参数 0） | 每次起播 |
| `+0x38` | SetFormat | `0x4B67CA` | 对**主缓冲**设一次（`[Sound+0x414]` 守卫） |
| `+0x3C` | SetVolume | `0x4B70C3`（`sub_4B70B0`） | 槽音量 |
| `+0x40` | SetPan | `0x4B7123`（`sub_4B7110`） | 槽"左右相对音量"（引擎用它做 pan/attenuation） |
| `+0x48` | Stop | `0x4B6B9C`（`sub_4B6B60`）· `0x4B64C2` / `0x4B64E6`（关机） | — |
| `+0x4C` | Unlock | `0x4B7024` | — |
| `+0x50` | Restore | `0x4B6D07` / `0x4B75B9` | 只走 `DSERR_BUFFERLOST` 分支 |
| `+0x10` `GetCurrentPosition` · `+0x14` `GetFormat` · `+0x24` `GetStatus` · `+0x44` `SetFrequency` · `+0x54` `SetFX` · `+0x58` `AcquireResources` · `+0x5C` `GetObjectInPath` | — | **整份音频模块 0 次** | ⇒ 引擎**不读播放游标**，它靠位置通知事件知道播放进度 |

#### (c) 其余"线程 / 定时器"API 的调用点（全 `.lst` 计数）

| API | 调用点数 | EA / 归属 | 与音频的关系 |
|---|---|---|---|
| `CreateThread` | **2** | `0x40261A`（`sub_4025F0`，入口 `StartAddress@0x402380`，DirectShow 异步读文件的辅助线程）· **`0x4B5FE4`（音频喂料线程 `sub_4B5AA0`）** | 音频用的就是这一个 |
| `_beginthreadex` | **1** | `0x47B407`（`sub_47B3A0` → 入口 `sub_47B200`，文件读取线程） | 无关 |
| `timeSetEvent` | **1** | `0x4CFB61`（`sub_4CFB20` ← `sub_4D5E50`，**DirectShow 渲染器**：`CMovieTextureRenderer`/`CBaseRenderer`/`CBaseVideoRenderer` 的 vtable 表项取址） | **音频不用多媒体定时器** |
| `CreateTimerQueue` / `CreateTimerQueueTimer` / `DeleteTimerQueueTimer` | **0**（只有 IAT 声明 `.data:0x51D214` / `0x51D21C`） | — | 定义了但没调用 |
| `waveOut*` / `PlaySound` / `sndPlaySound` / `auxSetVolume` | **0** | — | 定义了但没调用 |
| `mciSendCommandA` | **8**（`0x40420D 0x404273 0x404295 0x4042D9 0x4043D4 0x404479 0x404517 0x4045C2`，全在 `CD::` 类 `sub_4041C0`/`sub_404340` 一线） | CD-DA 后端 | **本作 BGM 不走它**（见 §2.2 后端选择） |
| `mciSendStringA` | **3**（`0x485DBB 0x485DDE 0x485E07`，`MIDI::` 类） | MIDI 后端 | 同上 |
| `mixerOpen` / `mixerSetControlDetails` / `mixerClose` | **6**（`0x404647 0x4046D1 0x4046DB 0x40473F 0x40474F 0x404759`，`CD::` 的音量控制） | CD 后端音量 | 同上 |
| `CreateSemaphoreA(…,1,name)` | 1（`_WinMain`；`README.md:109` 记的，**本次未复核**） | 单实例守卫 | 不是线程 |

**"定义了但没调用"的清单**（这类要显式说清，避免误判工作量）：`DirectSoundCreate8`、`DirectSoundEnumerate*`、`DirectSoundCapture*`、`waveOut*`、`PlaySound`、`sndPlaySound`、`CreateTimerQueue*`、`DeleteTimerQueueTimer`、`Concurrency::details::LoadLibraryAndCreateThread`（`.lst:0x4EC78B` 只有 `COLLAPSED` 函数、无调用引用）。

### 1.4 现状里值得顺手修的 4 条记账缺陷（不改行为）

1. `dsound.cpp:60-62` 写"引擎**不做 QI**（整份 `.lst` 里没有 IID_IDirectSound* 的符号/常量）" —— **符号**没有是真的（`0x52E298` 是 `unk_`），但**常量**在，且 `0x4B7297` 用它 QI 了流缓冲；同文件 `:363-365` 又写着"`sub_4B71C0` QI 它" ⇒ 同一文件自相矛盾。
2. `dsound.cpp:71-72` 的 `kIidDirectSoundBuffer8` 字节序需复核（其余 5 个 IID 与 `0x52E298` 一致；`0x52E298` 与 `kIidDirectSoundNotify` 逐字节相同【实测】）。
3. `dsound.cpp:288` 的 `kCapsCtrlFrequency = 0x00000008` 是**错标**：`DSBCAPS_CTRLFREQUENCY = 0x20`，`0x08 = DSBCAPS_LOCSOFTWARE`（值本身没被用于判断，但会误导下一个读代码的人）。
4. `winmm_audio.cpp:290-299` 的 stub 描述与实现相反（见 §1.1 末尾）。

### 1.5 当前 silent 的"消化方式"总结

- **不是**"假装没有音频设备"：`DirectSoundCreate` 成功、对象齐全、`CreateSoundBuffer` 真给 guest PCM 块、`Lock` 真给可写指针 ⇒ 引擎的**装载链**（WAV 头 / Ogg 解码器初始化 / 通知对象 QI）全部真跑过。
- **是**"设备有表无实"：游标不随时钟走、通知不投递、喂料线程不执行 ⇒ 环里始终是引擎自己 memset 的 0。
- 因此音频 silent **不产生**"引擎以为失败"的分支，也**不产生**"引擎以为成功但随后崩"的分支——它稳定地停在"有设备、无声音"。

---

## 2. DirectSound / winmm 的模型面

### 2.1 对象、方法、参数、返回码

**(1) `IDirectSound8`（引擎只用 3 个槽 + 1 个 CreateSoundBuffer）**

| 槽 | 方法 | 引擎参数 | 返回码要求 |
|---|---|---|---|
| `+0` | QueryInterface | **设备不 QI**（0 次） | — |
| `+4` | AddRef | — | — |
| `+8` | Release | 关机 `0x4B6534` | 0 |
| `+12` | CreateSoundBuffer | `DSBUFFERDESC{dwSize=0x24,dwFlags,dwBufferBytes,lpwfxFormat}`，`pUnkOuter=NULL` | `DS_OK` / 非 0 错误（引擎逐调用点判 `eax` 并走错误分支） |
| `+24` | SetCooperativeLevel | `(hwnd, level)`，level ∈ {1,2,3} | `DS_OK` |
| `+36`/`+40` | Initialize / VerifyCertification | 0 次 | — |
| `+16`/`+20`/`+28`/`+32` | GetCaps / DuplicateSoundBuffer / Get/SetSpeakerConfig | **0 次**（`GetCaps` 在 `dsound.cpp` 里实现了，但没有调用点） | 无 |

**(2) `IDirectSoundBuffer8`**（见 §1.3(b) 的完整枚举）。返回值语义上分两类：
- **成功/失败被引擎检查**：`CreateSoundBuffer`、`Lock`（`-2005401450 = 0x88780096 DSERR_BUFFERLOST` 触发 `Restore`+重试，`0x4B6CF4..0x4B6D60`）、`Unlock`（失败只打错误串）、`Play`、`Stop`、`Restore`、`SetFormat`（主缓冲）。
- **返回值只被用于打日志（不改控制流）**：`SetCurrentPosition`、`SetVolume`（`0x4B70B0`）、`SetPan`（`0x4B7110`）。

**(3) `IDirectSoundNotify`**（引擎真用）

- QI：`0x4B7297`，`IID = 0x52E298` = `{B0210783-89CD-11D0-AF08-00A0C925CD16}`【实测：字节逐个比对 `dsound.cpp:77-78` 常量，相同】。
- `SetNotificationPositions(notify, 4, &array)` @`0x4B7369`，数组在 `stream+0x424`，四项 = `{dwOffset, hEventNotify}`：

| # | dwOffset | hEventNotify 来源 |
|---|---|---|
| 0 | `0` | `[stream+0x414]`（ctor 从 `Sound+0x424+16*slot` 拷来的第 1 个事件） |
| 1 | `L/4`（`cdq`/`and edx,3`/`add`/`sar 2` = 有符号 `L/4`，`0x4B72F0..0x4B7305`） | `[stream+0x418]` |
| 2 | `L/2` | `[stream+0x41C]` |
| 3 | `3L/4` | `[stream+0x420]` |

其中 `L = 2 × nAvgBytesPerSec = 352800`（`0x4B71F9..0x4B7205`），即 **2.0 s 的环，四个通知点在 0 / 0.5 / 1.0 / 1.5 s**。

**(4) 参数与 `WAVEFORMATEX`**（引擎写死在 `.lst`，不是从资源读的）：

| 字段 | 值 | EA |
|---|---|---|
| `wFormatTag` | 1 (PCM) | `0x4B5F04`（`var_18 = 0x20001` 低 16 位） |
| `nChannels` | 2 | 同上（高 16 位） |
| `nSamplesPerSec` | 44100 | `0x4B5EFD`（`0xAC44`） |
| `nAvgBytesPerSec` | 176400 | `0x4B5ED0`（`0x2B110`） |
| `nBlockAlign` | 4 | `0x4B5F0B`（`var_C = 0x100004` 低 16 位） |
| `wBitsPerSample` | 16 | 同上（高 16 位） |
| `cbSize` | 0 | `0x4B5ECC` |

⇒ **单一格式：44.1 kHz / 16 bit / 2 ch / 176400 B/s**。没有第二份 `WAVEFORMATEX` 构造点（Ogg 解码器也从同一个位置拿到它——`sub_4B71C0` 的 `arg_0` 是解码器返回的 `WAVEFORMATEX*`）。

### 2.2 缓冲几何、PCM 数据、主缓冲、3D/EAX

**(a) 三个"缓冲"角色**

| 角色 | 建立点 | flags | bytes | 用途 | 音频上有没有贡献 |
|---|---|---|---|---|---|
| 主缓冲（primary） | `0x4B5E61` | `0xC1` = `PRIMARYBUFFER\|CTRLVOLUME\|CTRLPAN` | 0 | 打开设备 + 在 `0x4B67CA` 被 `SetFormat` 设成解码器格式 + `Play` 一次 | **无**（0 字节，没有数据） |
| "静音垫"（设备级流缓冲） | `0x4B5F19` | `0x181C8` = `GETCURRENTPOSITION2\|GLOBALFOCUS\|CTRLPOSITIONNOTIFY\|CTRLVOLUME\|CTRLPAN\|LOCSOFTWARE` | 176400 | `Lock→memset 0→Unlock→Play(looping)`，之后再没人写 | **无**（永久 0；作用是保持设备/混音器活着） |
| 每槽流缓冲 | `0x4B723F`（`sub_4B71C0`） | `0x181C8`（同上） | 352800（2 s） | 引擎真正往里写解码 PCM 的环；QI notify + 4 个位置通知 | **全部声音都在这里** |

**(b) 缓冲数据与宿主混音器的对应**（现状 + 目标）

- 现状：`createBufferObject`（`dsound.cpp:568-643`）为每个缓冲向 **guest 堆**分配 `dwBufferBytes` 字节（0 字节时给 1）并记在状态块 `kBufOffData(+0x0C)`；`Lock` 返回 `data + offset`（跨回绕时拆两段，`dsound.cpp:1018-1043`）。⇒ **宿主拿到的是一段 guest 地址区间**，按 guest EA 读它就能拿到 PCM（与呈现层读后缓冲同类，都是读 guest 内存，不是宿主指针）。
- 目标：宿主混音器 **逐缓冲**（15 个槽 + 静音垫 + 主缓冲）以 `play_cursor % bytes` 读样本，按 `SetVolume`（1/100 dB）+ `SetPan` 加权，求和后送设备。1 个 `SDL_AudioStream`（`AUDIO_S16LSB`, 2ch, 44100）就够；引擎没有别的格式。
- **不需要重采样**（设备按 44100 打开）；只有"宿主设备不支持 44100"时才需要 `SDL_SetAudioStreamFrequencyRatio` 一类转换——这在 `SDL3` 里是现成能力（`README.md:86` 已把 SDL3 定为宿主层）。

**(c) 主缓冲是否必须 / 3D / EAX**

- 真机上主缓冲是混音器的出口；**在本模拟器里不必**把它当"必须"——宿主自己就是混音器。但**必须保留它的返回码**（`CreateSoundBuffer`/`Play`/`SetFormat`/`Stop`/`Release` 都成功），因为引擎逐点检查。
- **3D / EAX：没有**。`0x181C8` 不含 `CTRL3D(0x10)` / `MUTE3DATMAXDISTANCE`；`SetFX(+0x54)` / `GetObjectInPath(+0x5C)` / `AcquireResources(+0x58)` 在音频模块 0 次调用；整份 `.lst` 没有 `EAX` / `I3DL2` / `DirectSound3D` 相关字符串【实测：`rg` 0 命中】。

### 2.3 状态机（引擎眼里的 DS 语义）

```
CreateSoundBuffer(flags,bytes,fmt) ──► buffer{state=STOPPED, cursor=0, notify[4]}
QI(IDirectSoundNotify)             ──► notify 对象
SetNotificationPositions(4, pts)   ──► 记下 4 个 (offset,event)
SetFormat(primary, fmt)            ──► 只对主缓冲（一次，[Sound+0x414] 守卫）
Lock(off,len)  ──► (ptr1,n1 [,ptr2,n2])     ← 真 guest 指针；跨回绕拆两段
Unlock(p1,n1,p2,n2) ──► DS_OK               ← 引擎只判非 0
SetCurrentPosition(0) ──► cursor=0
Play(0,0,1)        ──► state=PLAYING|LOOPING；游标从 cursor 开始走
   │  (设备时钟)  每越过 notify[i].offset → SetEvent(notify[i].event)（**只有 4 个/圈**）
   └─ 环：cursor = (cursor + Δ) % bytes
Stop()             ──► state=STOPPED；游标冻结
Restore()          ──► DS_OK（我们永不丢缓冲）
SetVolume(dB×100) / SetPan(v) ──► 只影响宿主增益
Release()          ──► refcount
```

- **`Lock` 的语义要点**：真实 DS 要求"不要锁正在播放的那段"；引擎严格遵守（先等通知、再锁"刚放完的那 1/4"）。⇒ 宿主回调读"正在播的那 1/4"与 guest 写"刚放完的那 1/4"**永远不重叠** —— 这是整条链能安全的根据。
- **游标**：`+0x2C Lock` 的 `dwOffset` 直接来自**引擎自己的通知偏移表**（`stream+0x424`，`0x4B6C73..0x4B6CA9`），不是从 `GetCurrentPosition` 读的；`Unlock` 时不带位置。⇒ 引擎与设备的"位置一致性"**完全由事件投递次序保证**，不由任何查询保证。
- **回绕**：`sub_4B6C30` 里 `size = off[cur] - off[prev]; if (size<0) size += L;`（`0x4B6CA1..0x4B6CB7`）⇒ 引擎自己算跨度，所以宿主只需保证"事件准时、每个偏移每圈一次"。
- **循环**：`Play` 恒 `LOOPING(1)`；真正的"重复/停止"由 guest 决定（`sub_4B6C30` 的 `[stream+0x2450]` = 重复起点、`[stream+0x2458]` = 0/1/2 状态、`[stream+0x2460]` = 在播标志、`[stream+0x2468]` = 已写圈数、`[stream+0x2470]` = 重复计数；`sub_4B6B60` 里 `Stop()` + `+0x2460=0`）。
- **设备丢失**：`Restore` 在 `0x4B6D07`（错误 `0x88780096` 分支）；我们永不丢缓冲 ⇒ 这条分支保持死路（但返回值必须是 `DS_OK`，否则引擎会打错误串并放弃该流）。
- **音量/频率/pan**：`SetVolume`（`0x4B70C3`，dB×100，负值）、`SetPan`（`0x4B7123`，`-10000..10000`）；**没有 `SetFrequency` 调用** ⇒ 不需要改变播放速率（这是"不需要重采样"的第二条根据）。
- **合作级别**：`level ∈ {1,2,3}`（NORMAL/PRIORITY/EXCLUSIVE，由配置 `sound:DirectSoundCooperativeLevel` 映射）；本次实现**不需要**真的独占设备（宿主混音器不与别人抢），但 `DSSCL_EXCLUSIVE` 下引擎会设主缓冲格式——我们的 `SetFormat` 恒成功，正好。

### 2.4 引擎的"混音线程"到底做什么

`sub_4B5AA0`（`.lst:0x4B5AA0`，69 行）逐字：

```
 0x4B5AA9  push 0FFFFFFFFh / push 0 / lea eax,[ebx+424h] / push 3Dh
 0x4B5AB6  call WaitForMultipleObjects      ← 61 个句柄，bWaitAll=FALSE，INFINITE
 0x4B5ABE  cmp esi, 3Ch   → 等于 60 就整线程退出（第 61 个事件 = 退出信号）
 0x4B5AC4  edi = esi >> 2                    ← 槽号
 0x4B5AC9  CS = [ebx + (edi*3 + 0xD2)*8]     = Sound+0x690+24*slot
 0x4B5AD7  EnterCriticalSection
 0x4B5ADD  if (Sound+0x654+4*slot == 0) → 跳过（槽已空）
 0x4B5AEF  InterlockedIncrement(Sound+0x7F8+4*slot)   ← 在飞计数
 0x4B5AF8  ecx = [Sound+0x654+4*slot]                 ← SoundBuffer 对象
 0x4B5AFF  push (esi & 3); call sub_4B6C30            ← 补第 (esi&3) 个 1/4 圈
 0x4B5B0C  InterlockedDecrement(...)
 0x4B5B16  LeaveCriticalSection
 0x4B5B29  WaitForMultipleObjects(…, INFINITE)  循环
```

`sub_4B6C30`（`0x4B6C30`，197 行 C，逐项）：

1. `if (!stream[0x2458]) { sub_4B6B60(stream); return 1; }`（状态 0 = 停）。
2. `L = stream[0x444]`；`off_prev = stream[0x424 + 8*((a2+3)%4)]`；`off_cur = stream[0x424 + 8*a2]`；`len = off_cur - off_prev; if (len<0) len += L;`
3. `Lock(stream[0x40C], off_prev, len, &p1,&n1,&p2,&n2, 0)`；若返回 `0x88780096` → `Restore` + 重新 Lock。
4. 从**解码器对象** `stream[0x247C]` 拉数据：`vtable+0x10` = 剩余长度、`+0x0C` = 数据指针、`+0x14` = 前进；`memcpy` 进 `p1`（必要时 `memcpy` 进 `p2`，把剩下的留在 `stream+0x44C` = `+0x244C` 残块）。
5. 数据被取空时：`if (stream[0x2450] == 0)` → 用静音值填满（`memset(p, stream[0x2454] != 8 ? 0x00 : 0x80, len)`，`0x4B6FBD..0x4B6FD1`）+ `stream[0x2458] = 2`（"已放完"状态）+ 解码器前进；否则按 `+0x2450`（重复起点）继续。
6. `stream[0x2468]++`；`Unlock(stream[0x40C], p1,n1,p2,n2)`；失败打错误串。
7. ★ 填充值只在 **8bit** 时是 `0x80`，16bit 时是 `0x00`；而本引擎只有 **16bit**（§2.1(4)）⇒ 永远填 0。

⇒ 线程的语义 = **"事件驱动的环形缓冲消费者/生产者"**，没有混音、没有时钟、没有音量—— **它只是把解码器的字节搬进环**。真正的"混音"在 DirectSound 里（真机），也就是**我们要在宿主实现的那一部分**。

### 2.5 线程 / 同步 / 定时器面（会不会被音频路径用到）

| 机制 | 音频路径用到？ | 证据 |
|---|---|---|
| `CreateThread`（裸 `__stdcall`，**非 CRT 线程**） | **是**（唯一的一处音频线程） | `0x4B5FE4` |
| `_beginthreadex` | 否（文件读取线程） | `0x47B407` |
| `WaitForMultipleObjects(61, …, INFINITE)` | **是** | `0x4B5AB6`，`0x4B5B29` |
| `WaitForSingleObject(thread, 3000)` | **是（关机）** | `0x4B6522` |
| `SetEvent(quit)` | **是（关机）** | `0x4B6510` |
| `CreateEventA` ×61 | **是** | `0x4B5FC8`（循环 `ebx=0x3D`） |
| `Enter/LeaveCriticalSection`（15 个，SpinCount=10000） | **是** | ctor `0x4B5B50`；用点 `0x4B5AD7`、`0x4B6570`、`0x4B6390`、`0x4B6440`、`0x4B6B60`、`0x4B73E0` |
| `TryEnterCriticalSection` | 否 | 音频模块 0 次 |
| `InterlockedIncrement/Decrement` | **是**（在飞计数） | `0x4B5AEF`、`0x4B6570`、`0x4B73E0`、`0x4B6B60` |
| 信号量 / 互斥量 | 否（`_WinMain` 的 `CreateSemaphoreA(…,1,…)` 是单实例守卫；`CMemAllocator` 的属于 DirectShow） | `README.md:107-110` |
| `timeSetEvent` / timer queue | **否** | §1.3(c)：唯一调用点在 DirectShow 渲染器 |

**CRT / TLS / TEB 面（这条最容易被低估，单列）**

- 静态链接 CRT（`.lst` 无 `msvcrt` 导入），但**用 TLS**：`TlsAlloc/TlsGetValue/TlsSetValue` 在 `0x4F094A`、`0x4FA951`、`0x4FCB13`、`0x4FCB5D`。
- 现模拟器的 TLS 是**进程一份**（`src/shims/kernel32_error_tls.cpp:84-…`：一个 guest TLS 数组 + 位图），TEB 一份（`src/runtime/image.cpp:419-440`：SEH 链 `+0`、StackBase/Limit、Self、`ThreadLocalStoragePointer +0x2C`）。
- ⇒ 让第二个 guest 线程跑起来要补：**每线程 TEB**（SEH 链 / StackBase / TLS 数组指针）、**fs_base 切换**、`GetCurrentThreadId` 返回当前线程 id、`Tls*` 按线程分表。这是"干净但必须做"的一坨（§3.5 P3）。

### 2.6 如果改成宿主音频回调，哪些 guest 可见状态必须保持一致

**必须保持一致（guest 会读 / 会据它分支）：**

1. **位置通知事件的投递次序与次数** —— 每圈每个 offset **一次**，按"游标越过"的顺序；事件是 auto-reset（`CreateEventA(0,0,0,0)` = auto-reset、非 signaled）。
2. **`Lock` 给回的区间与 `dwOffset` 严格对应**（引擎自己算跨度；宿主不能提前/推后"哪 1/4 圈可用"）。
3. **`Unlock` 的成功/失败**（失败会打错误串并放弃；`DSERR_BUFFERLOST` 会触发 `Restore` 重试）。
4. **`Play`/`Stop`/`SetCurrentPosition`/`SetFormat`/`Release` 的返回码**（引擎逐点检查）。
5. **`SetVolume`/`SetPan` 的值往返**（引擎在 fade 里连续写，见 `sub_4B6210`；我们不需要"读回"，但需要真的用它算增益，否则 fade 不出声变化）。
6. **引用计数 / 对象生命周期**（`sub_4B7170` 换缓冲时先 Release 旧对象；`Release` 到 0 才释放 guest 块）。
7. **`+0x2460` 之类的 guest 自己的状态**（`sub_4B6130` 读它判"在播"）—— 这由 guest 代码维护，但**线程不跑时它只在 Play/Stop 时被写**，所以 `sub_4B6130` 在播放中会一直答"在播"，即使环已空；这是 silent 下一个**可观察的语义偏差**。

**可以不保持（引擎 0 次调用 / 只影响诊断）：**

- `GetCurrentPosition`（`+0x10`）—— 引擎不读。但**别把它当"播放游标"用**：现在它返回"累计 Unlock 字节"（`dsound.cpp:1094-1096`），无界增长；实现真设备时必须改成 `cursor % bytes`，并区分 play/write 两个游标（真机 `write ≥ play`）。
- `GetStatus`（`+0x24`）、`GetCaps`（`+0x0C`）、`GetFormat`（`+0x14`）、`SetFrequency`（`+0x44`）。

---

## 3. 可行性判断

### 3.1 三条路径（先把选项摆清楚，再谈工作量）

| 路径 | 做法 | 能听到什么 | 代价 / 风险 |
|---|---|---|---|
| **A. 全宿主化**（README §2.5 字面意思） | 在宿主重写"解码 + 环形缓冲 + 15 槽 + 循环点"，把 guest 的 `SoundBuffer` 层变成空壳；`DirectSoundCreate` 等继续假装成功 | BGM/SE/语音全都能听 | 等于把引擎的音频层**实现两遍**（guest 里那份仍在、但永远不跑）；需要宿主 Ogg Vorbis 解码器（15 766 个 `.OGG`，BGM 单文件最大 8 MB）+ `AOGG` 容器语义；`sub_4B6130`（在播）/`sub_4B4F60`（装载失败抛 `Command_ShowMessage_Exception`）等 guest 可观察行为要**伪造**；slot/失败/异常语义与 guest 分叉。**工作量不比 B 小，语义更假。** |
| **B. 真 DS 设备模型 + 协作式 guest 线程**（后备） | 宿主实现"设备"（游标 / 通知 / 状态机 / 混音 / SDL 输出）；`CreateThread` 真跑 `sub_4B5AA0`（协作式调度，不引入宿主并发） | 全都能听，且语义与 `.lst` 一致 | 要动 `README §2.5` / `SCOPE §1` 的单线程口径（**用户裁决**）；要补每线程 TEB/TLS；实现量集中在调度器（§3.5 P3） |
| **D. 完全 native override 喂料线程**（thread 报告推荐） | 把 `sub_4B5AA0` 的控制流 native 化（只保留 `0x4B5B03 call sub_4B6C30` 给 guest），在主线程的确定性边界调它；不建第二栈 | 全都能听 | 不引入宿主并发，但每 0.5 s 一次 `sub_4B6C30`；超一帧预算就掉帧（可先零代码量，见 thread 报告 §5.1） |
| **C. 折中（先真设备、后喂料）** | 先做 B/D 的"设备"一半（游标 + notify + 混音 + 输出），`CreateThread` 仍是伪线程；然后再补喂料 | 第一刀：SE（≤1.5 s）**完整可听**；BGM 变 1.5 s + 0.5 s 静音的 2 s 循环。第二刀：连续 | 第一刀**不违反**任何现有口径；第二刀才需要裁决 |

**推荐 D → C/B**：D 把“消费端”缩成一次主线程调用，绕开真线程/第二栈与同步对象层；若实测
`sub_4B6C30` 每 0.5 s 的耗时超一帧预算，再升 C/B（D 的设备模型与 notify 簿记可原样复用）。

### 3.2 最小可听路径 vs 完整语义路径（分界线）

**分界线 = "谁产 PCM"。** 只要 PCM 由 guest 解码器产生并写进环，就要求 guest 解码器在某个
上下文里跑：D 在主线程的确定性边界调它（不建第二栈），B 用协作式第二栈；只有路径 A 才在宿主重写解码。

| 能力 | 最小可听路径（第一刀，C） | 完整语义路径（B） |
|---|---|---|
| DS 设备模型（游标 / notify / 状态机 / 环回绕） | **要** | 要 |
| 宿主混音 + 输出（SDL3，44.1k S16 立体声） | **要** | 要 |
| 位置通知投递（`SetEvent`） | **要**（否则 notch 不成立，且下一刀没有锚） | 要 |
| guest 解码器（WAV/Ogg/AOGG） | **主线程预填的 3/4 圈**（够 1.5 s） | 要（D 泵持续调 / B 线程里持续跑） |
| guest 喂料控制流 + 阻塞等待 + 线程句柄 | 不要 | **D：控制流 native、不建栈；B：要** |
| 每线程 TEB / TLS / `GetCurrentThreadId` | 不要 | D 不要；B 要 |
| 重采样 | 不要（44100 直出；设备不支持时用 SDL 的比例转换） | 同左 |
| 混音 | 要（多槽同时响：SE 0..10、BGM 11、语音 12..14） | 要 |
| 循环点 / 重复 / 停止 | 不要（环自循环） | 要（`+0x2450` / `+0x2458` 由 guest 逻辑驱动） |
| `--audio-dump` 之类可断言出口 | **要**（判据） | 要 |

### 3.3 哪些可以宿主化、哪些必须保留 guest 单线程语义

**可以宿主化（且应该宿主化）：**
- 采样输出与设备格式（SDL3 `SDL_AudioStream`，44.1k S16 立体声）。
- 混音求和 + 每缓冲增益（`SetVolume` → `10^(dB/2000)`，`SetPan` → 左右增益；引擎已把线性音量折成 dB，见 `sub_4B6210` 的 `log` 公式）。
- 播放游标的推进与 notify 的**触发时机**（这是"设备时钟"的宿主实现）。
- 主缓冲 / 静音垫的"存在性"（返回码 + 对象，但不参与混音）。

**必须保留 guest 语义（不能搬宿主）：**
- guest 环的**内容**（＝解码器输出，只有 guest 解码器能产）。
- "哪 1/4 圈现在可写"的**判定**（guest 自己算跨度、自己 Lock/Unlock）。
- slot 状态字（在用 / 在播 / 音量 / 循环点）与**错误路径**（装载失败抛 `Command_ShowMessage_Exception`、`SetFormat` 主缓冲失败等）。
- `SoundBuffer` 的临界区与在飞计数（它们保证"主线程换缓冲"与"线程正在写"不打架；协作式调度下也必须照跑）。

### 3.4 风险清单（按"会让第一刀失败"排序）

| # | 风险 | 为什么危险 | 处置 |
|---|---|---|---|
| R1 | **协作式线程做不出来 / 做出来死锁** | `WaitForMultipleObjects(INFINITE)` 现在是**硬 trap**（`kernel32_sync.cpp:670-677`）；要改成"挂起当前 guest 线程、切回主线程" | 先在**单线程**下把符号铺好（第一刀不动它）；第二刀用"shim 置 `ShimCall::take_control` + 切上下文"的既有机制（`shims.h:37-41`、`guest_call.h` 已有先例） |
| R2 | **事件投递源在哪个线程** | 若 SDL 回调线程里 `SetEvent`（改 guest 事件对象），就是"宿主线程碰 guest 内存"⇒ 与 guest 写并发 | 回调**只**写一个宿主侧的原子游标；`SetEvent` 由 dispatcher 在**块边界**按游标补投（与 `fireDueTimer` 同形，`machine.cpp:549-554`） |
| R3 | **回调里读 guest 环的并发安全** | 回调读"正在播的那 1/4"，guest 写"刚放完的那 1/4"——**协议上不重叠**，但边界那一字节可能跨 | 用"按 1/4 圈对齐 + 只在圈边界换缓冲"（引擎的通知偏移就是圈边界，天然对齐）；必要时拷贝到宿主影子缓冲再播 |
| R4 | **时钟 / 确定性（A4）** | 音频天然是墙钟；现有 `VirtualClock` 默认虚拟、`--real-time` 才接宿主（`clock.h`、`main.cpp` 的 `--real-time`） | 二选一：`--audio=virtual`（虚拟 sink，确定性，CTest 用）/ `--audio=sdl`（真设备，`clock=REAL-TIME`，A4 自动跳过并打印，已有先例）。**默认档要用户定** |
| R5 | **Ogg 解码在解释器里的吞吐** | BGM 每 0.5 s 要解 0.5 s 音频；解释器慢 ⇒ underrun ⇒ 爆音 | 第二刀验收就测"30 s 无 underrun"；不够就先 `--audio=virtual` 或把 Ogg 解码 native override（**单独一单**，不在本单） |
| R6 | **每线程 TEB / TLS** | 静态 CRT 用 TLS（`TlsGetValue/TlsSetValue`）；现实现是进程一份（`kernel32_error_tls.cpp`）⇒ 两线程共享 `errno` 等 | 每线程 TLS 数组 + TEB；实在不行先共享并**显式记账**（偏差，不是静默） |
| R7 | **线程句柄不是可等待对象** | `sub_4B6440` 关机时 `WaitForSingleObject(thread,3000)`；现在 `syncOf` 只认 Event/Semaphore/Mutex ⇒ 返回 `WAIT_FAILED`（不阻塞，所以现在不崩） | 线程句柄要"退出即 signaled"；同时保持"未 signaled + INFINITE = 硬错"的口径（除本线程以外） |
| R8 | **采样率 / 格式** | 引擎只有 44.1k/16/2；但引擎**不检查**我们给回的格式（`GetFormat` 0 次调用）⇒ 我们若私自改格式，guest 不会发现，**但会算错跨度** | 严格按 `WAVEFORMATEX` 原值记账（`dsound.cpp:797-831` 已经逐字段还回），混音按 `nBlockAlign` 解释 |
| R9 | **循环点** | BGM 的重复起点在 `+0x2450`（来自 `sub_4B6190` / `sub_418520` 的 `a2`），guest 靠它决定"重解码"还是"填 0" | 第一刀不碰；第二刀**不要**在宿主做循环，交给 guest |
| R10 | **静音 / 暂停 / 设备丢失** | `Restore` 我们永不丢 ⇒ 分支死；但若真丢（SDL 设备切换）要返回 `DSERR_BUFFERLOST` 才对，否则 guest 不会重填 | 明确"永不丢"（现状），并把它写成偏差；真丢时置 `buffer_lost` 一次性重填 |
| R11 | **15 槽 × 每槽 352800 B = 5.3 MB guest 堆** | 现 shim 每个 buffer 都真分配（`dsound.cpp:596`）；全开时 5 MB 级，堆能否承受 | 已能（`heap in_use 8.2 MB` 的现有运行日志），但要加 CTest 上限断言 |
| R12 | **`--audio-dump` 的判据脆弱** | 只看"非零字节"会被 memset 0 骗过（现在 PcmBytes 记账里 nonzero=0 就是证据） | 判据用 **RMS + 过零率 + 与源 WAV 的互相关**，不用"非零" |

### 3.5 工作量估算（人日，静态推断；无原型验证）

| 阶段 | 内容 | 人日 | 依赖 | 风险 |
|---|---|---|---|---|
| **P1** 真 DS 设备模型 | 游标 / 环 / 4 通知 / 状态机 / `SetNotificationPositions` 真存 / `Restore` / 游标改 `%bytes` / guest 侧 notify 表（保持"宿主无全局状态"） | 2–3 | 无 | 低 |
| **P2** 宿主 sink + `--audio-dump` | `AudioSink` 接口、混音（15 槽 + vol/pan）、`--audio-dump` WAV、虚拟时钟驱动（确定性档） | 2–3 | P1 | 低 |
| **P3** 协作式 guest 线程 | 第二上下文（regs/stack/TEB/fs_base）、`CreateThread` 真建栈、阻塞等待（`WaitForMultipleObjects` 挂起/唤醒）、线程句柄 signaled、`ExitThread`、每线程 TLS、`GetCurrentThreadId`、调度点（块边界） | 4–7 | P1 | **高**（新机制 + 死锁面） |
| **P4** SDL3 真输出 | `SDL_AudioStream`（44.1k S16）、设备丢失、回调→原子游标、`--audio=sdl/virtual` 档、`--real-time` 交互 | 2–3 | P2 | 中（宿主线程 + 重入） |
| **P5** 联调 / 解码吞吐 | BGM(Ogg) 在解释器下的 underrun、`mmio`、堆、SEH 每线程、错误码 | 3–8 | P3+P4 | **高**（未知） |
| 合计 | | **13–24 人日**（≈3–5 周单人） | | 可并行压到 2–3 周 |

★ 若走 **D**（thread 报告 §5.1）：把 **P3** 换成 **P3-D**（`sub_4B5AA0` 控制流 native + 主线程泵 +
整份 guest 上下文交换 + CS defer + join pump），约 3–5 人日；P1 / P2 / P4 / P5 不变。
D 不建第二栈、不改 `SCOPE §1` 的伪线程口径，但没有 P3 的 underrun 风险；代价是每 0.5 s 一次
`sub_4B6C30` 的主线程卡顿。

对照：路径 A（全宿主化）我估 **10–20 人日**，但**语义风险更高**（要伪造 guest 可观察行为），且不产生可复用的"设备模型"。

### 3.6 建议的第一刀（最小可验收增量 + 机械判据）

**目标：把"记账的静音设备"升级成"有播放时钟与位置通知的真设备模型"，不动线程。**
预期可听结果：**标题 SE 完整可听**；BGM **1.5 s 声音 + 0.5 s 静音**的 2 s 环（这不是缺陷，是第一刀的**预期**，写进验收）。

**改动面（建议）**

1. 新 `src/runtime/audio_device.{h,cpp}`：每缓冲状态 `{bytes, data_ea, fmt, flags, state, volume, pan, play_cursor, notify[4]{offset,event_ea}, laps}`，纯函数 `advance(Δ)` → 返回"本步跨过的通知点"列表。
2. `src/shims/dsound.cpp`：
   - `SetNotificationPositions`：把 4 个 `(offset,event)` 存进**guest 侧新块**（保持 §1.2 的"宿主无全局状态"），不再恒 `DS_OK` 空转；
   - `Play`/`Stop`/`SetCurrentPosition`/`Release` 驱动游标与状态；
   - `Unlock` 不再把 `cursor` 设成累计字节（`dsound.cpp:1094-1096`）；
   - `GetCurrentPosition` 返回 `play_cursor` / `write_cursor`（两者都 `% bytes`，`write = play + 环内已写未播`）。
3. 新 `AudioSink` 接口 + `--audio-dump <wav>`（与 `--present-screenshot` 同形；`present.*` 是现成范式）。
4. dispatcher（`machine.cpp` 块边界，`549-554` 附近）按 `audio_device.advance` 补投 `SetEvent`（**与 `fireDueTimer` 同形**，不新开宿主线程）。
5. 修 §1.4 的 4 条记账缺陷（纯文档/常量）。

**机械判据（新 CTest `audio-device-model`，pure 档；照 `tests/dsound.cpp` 的"自造机器码"方式）**

- (a) guest 程序：`DirectSoundCreate` → `CreateSoundBuffer{dwSize=36, flags=0x181C8, bytes=176400, fmt=44.1k/16/2}` → `QI(Notify)` → `SetNotificationPositions(4, {0, 44100, 88200, 132300})` → `Lock(0, 176400)` 写入**斜坡模式**（`s16 = i*37`）→ `Unlock` → `Play(0,0,1)`。
- (b) 以 250 ms 步进推进伪时钟 2 s（= 1 圈），断言：
  - 游标 = `(44100*4*t) mod 176400`；
  - 4 个事件各被 signaled **恰好 1 次**，顺序 `+1/4 → +1/2 → +3/4 → 回绕(0)`；
  - `Stop` 后游标不动、事件计数不变；`SetCurrentPosition(0)` 后重新从 0 走。
- (c) sink 拉到的前 176400 B **逐字节等于**斜坡（证明"宿主读到的就是 guest 写的"）。
- (d) 断言 `[dsound]` 日志里 `pcmBytes>0 && nonzeroBytes>0`（这条会红：现在 `nonzero` 恒 0 是因为引擎 memset，夹具里我们自己写斜坡）。
- (e) 61 句柄 `WaitForMultipleObjects(61, …, 0)` **轮询**返回 `0..59`（不再 `shimAbort`）—— 这是给第二刀的锚。
- 变异自检（照 `pnpm test:mutation` 的口径）：把 `advance()` 里的 `%bytes` 去掉 ⇒ (b) 红；把 notify 投递删掉 ⇒ (b) 红；把 `Lock` 的返回指针改成另一块 ⇒ (c) 红。

**可选（assets 档）**：`--game-dir … --present headless --time-limit 60 --audio-dump .tmp/a.wav --log-level info --log-cat dsound`，判据 = dump 的 RMS > 0 且 `Play` 行覆盖槽 11（BGM）/12-14（语音）。**注意**：这条只有在 P3 之后才有意义（否则 BGM 只有 1.5 s）。

### 3.7 卡在哪 / 要用户裁决什么

**卡点（一条，不是"复杂"）**：**能出声的采样只由 guest 线程产生**。
证据链闭合在四处：
1. `Output = 每槽 352800 B 的环`（`0x4B723F`）；
2. `环里只有主线程 Play 前预填的 3/4`（`0x4B74B4..0x4B74C0`：`for(i=1;i<4) sub_4B6C30(...)`）；
3. `第 4 个 1/4 圈的填充点 = 事件 #4s+0`（`0x4B72DE..0x4B7369` 的通知表 + `0x4B5AF8` 的 `i&3`）；
4. `事件的唯一投递者 = DirectSound 设备时钟`（真机），`唯一等待者 = sub_4B5AA0`（`0x4B5AB6`），而它由 `CreateThread` 创建（`0x4B5FE4`），当前是伪线程（`kernel32_sync.cpp:56-61`）。

⇒ 要连续声音，必须让 4 成立。**有三条办法**：**D** 把喂料控制流 native 化、在主线程边界调
`sub_4B6C30`（不建第二栈；推荐）；**B** 让 guest 线程跑（改 §2.5 口径）；**A** 在宿主重写解码+环。
D 不引入宿主并发，但吞吐是它的代价（见 thread 报告 §5.1）。

**需要用户裁决的 3 件事**：

- **V1（必须）**：路线选 **D（完全 native override 喂料线程，推荐）** 还是 **B/C（协作式第二栈 /
  真线程 + 令牌）**。D 不改 `SCOPE.md §1` 的“伪线程”口径（仍不建 guest 线程），但要把
  `CreateThread(sub_4B5AA0)` 的语义改成“登记 + 泵”；B 则直接改写 `SCOPE.md §1` 与
  `README.md §2.5`（guest 单线程）。若都不做 ⇒ 连续 BGM 只能走 A（宿主重写）。
- **V2（必须）**：音频的时钟档。`--audio=virtual`（确定性、A4 可保留、CTest 可断言、但**听不到**）与 `--audio=sdl`（真设备、`clock=REAL-TIME`、A4 自动跳过）是否都提供、默认哪一个。
- **V3（建议）**：是否接受"第一刀可听但会断"的分阶段验收（SE 完整 / BGM 1.5 s 环），把"连续播放"放到第二刀。

---

## 4. 证据与诚实项

### 4.1 已实测（本次工具机械读出）

| # | 结论 | 证据 |
|---|---|---|
| E1 | `DirectSoundCreate` 是引擎取的**唯一** DS 导出 | `.lst` `DirectSoundCreate` 共 4 处：1 字符串 `.data:0x52CEA0`、1 `push` `0x4B5C9F`、1 `GetProcAddress` 调用点 `0x4B5CA5`、1 错误串；`rg -c` 计数 |
| E2 | `DirectSoundCreate8` / `DirectSoundEnumerate*` / `DirectSoundCapture*` / `waveOut*` / `PlaySound` / `sndPlaySound` / `CreateTimerQueue*` 调用点 = 0 | `rg` 计数；`CreateTimerQueue*` 仅 `.data:0x51D214`、`0x51D21C` 的 IAT 声明 |
| E3 | `CreateThread` 调用点 = **2**（`0x40261A`、`0x4B5FE4`）；`_beginthreadex` = 1（`0x47B407`）；`timeSetEvent` = 1（`0x4CFB61`，属 DirectShow 渲染器） | `rg -n` + `disasm-fn show 0x…` 的"取址证据"表 |
| E4 | 流缓冲：`L = 2*nAvgBytesPerSec = 352800`，通知点 `0 / L/4 / L/2 / 3L/4`，事件来自 `[stream+0x414..0x420]` | `.lst` `0x4B71F9..0x4B7205`、`0x4B72DE..0x4B7369` |
| E5 | 61 个事件（`0x3D`），第 61 个（index 60）是退出信号；线程按 `index>>2 = slot`、`index&3 = 1/4 圈` | `.lst` `0x4B5FB5..0x4B5FD4`（循环）、`0x4B5AA9..0x4B5ABE`、`0x4B5AC4..0x4B5B03` |
| E6 | 15 槽 / 24 字节临界区 / 在飞计数偏移 | `.lst` `0x4B639A`（`cmp edi,0Fh` 界 15）、`0x4B63CA`（`[esi+edi*4+654h]`）、`0x4B5AC9`（`(slot*3+0xD2)*8` = `0x690+24*slot`）、`0x4B5AE7`（`[ebx+edi*4+7F8h]`） |
| E7 | 关机：15 槽 dtor → Stop/Release 静音垫与主缓冲 → `SetEvent(+0x514)` + `WaitForSingleObject(thread,3000)` → Release pDS | `.lst` `0x4B6470..0x4B6534` |
| E8 | `WAVEFORMATEX` = PCM / 2ch / 44100 / 176400 / blockAlign 4 / 16 bit / cbSize 0 | `.lst` `0x4B5ECC..0x4B5F0B` |
| E9 | 引擎对流缓冲只用 9 个槽（Lock/Play/SetCurrentPosition/SetFormat/SetVolume/SetPan/Stop/Unlock/Restore + Release/QI） | `.lst` 音频模块内 `[e??+2Ch/30h/34h/38h/3Ch/40h/48h/4Ch/50h]` 的全量枚举 |
| E10 | 引擎 QI 流缓冲的 `IID = 0x52E298`，字节 = `{B0210783-89CD-11D0-AF08-00A0C925CD16}` | `.lst` `0x4B7297` + `.data:0x52E298` 的 8 字节窗口 vs `dsound.cpp:77-78` |
| E11 | 三种输入容器魔数：`'RIFF'`(0x46464952) → `WavePCM`、`'OggS'`(0x5367674F) → `OggVorbis`、`'AOGG'`(0x47474F41) → `OggStream` | `.lst` `0x4B6570` 的 `switch(Buffer)`；`sub_4BBD10`/`sub_48D460`/`sub_48C870` 的 RTTI 名（`WavePCM`/`OggVorbis`/`OggStream`） |
| E12 | 解码器接口 = vtable `+4` open / `+8` 取状态 / `+12` 数据指针 / `+16` 长度 / `+20` 前进 | `.lst` `0x4B6EF7`（`[src+0x10]`）、`0x4B6F06`（`+0x0C`）、`0x4B6F65`（`+0x14`）、`0x4B6570` 的 `(*v8+4)(…)`；`WavePCM` vtable `0x52E094` 的 8 项 |
| E13 | 槽位分工：SE 0..10（脚本直呼 + 10 项延迟表）、BGM = 11、语音 = 12..14 | `.lst` `0x4B4F60`（`[T_SE+0x4BC+4*slot]`）、`0x4B5090`（10 项预载）、`0x48DB80`（`slot 0xB`）、`0x4BB840`（`slot = ch+12`） |
| E14 | 音频路径**不用** `timeSetEvent` / timer queue / 信号量 | 见 E3 与 §2.5 |
| E15 | 现模拟器：`SetNotificationPositions` 空实现、`CreateThread` 伪线程、`waitOne/waitMultiple` 未 signaled + 非 0 超时 = `shimAbort` | `dsound.cpp:339-342`；`kernel32_sync.cpp:1081-1117`、`:670-677`、`:741-751` |

### 4.2 静态推断（`.lst` + SDK 常量读出，可再核）

| # | 结论 | 依据 / 如何再核 |
|---|---|---|
| S1 | `0xC1 = PRIMARYBUFFER\|CTRLVOLUME\|CTRLPAN`；`0x181C8 = GETCURRENTPOSITION2\|GLOBALFOCUS\|CTRLPOSITIONNOTIFY\|CTRLVOLUME\|CTRLPAN\|LOCSOFTWARE` | 位名来自 SDK `dsound.h`；`.lst` 只给原值 `0x4B5E53` / `0x4B5EF6`。再核：对 `DSBCAPS_*` 常量表 |
| S2 | 主缓冲 `dwBufferBytes=0`、`lpwfxFormat=0` | 引擎在 `CreateSoundBuffer` 前把 36 字节 `DSBUFFERDESC` 全清 0（`0x4B5E2B..0x4B5E3D`），随后只填 `dwSize=0x24` / `dwFlags=0xC1`（`0x4B5E4C` / `0x4B5E53`） |
| S3 | "预填 3/4 圈 ⇒ 最多 1.5 s" | `0x4B74A7`（`cmp [esi+2464h],0`）→ `0x4B74B4..0x4B74C0` 的 `for(i=1;i<4) sub_4B6C30(this,i)` + 环长 2 s；**未在真跑上验证** |
| S4 | `sub_418520(device, 11, n)` 的 `n` = "预填几个 1/4 圈" | `sub_421AF0` 里 `v2/500`（毫秒/500 = 半秒段数）；`0x4B6570` 之后 `sub_418520` 的循环 `--v4; sub_4B6C30(slot,1)`。**待核**：为何固定 quarter 1 |
| S5 | `SetPan`（`+0x40`）被引擎当"左右相对音量/衰减"用 | 错误串 `"ERROR dsbSetPan:左右相対ボリューム変更に失敗しました"`（`0x4B7110`）；`sub_4B6210` 用 `log()` 折 dB |
| S6 | 15 槽 = 10 SE + 1 BGM + 3 语音 + 1 未用 | 界 15（`0x4B603E..0x4B6041` 的 `cmp edi,0Eh / ja`，**无符号**⇒ 负槽也被拒）、`0x4B639A..0x4B639D` 的 `cmp edi,0Fh / jl`（**有符号**⇒ 负槽会读表前）；0xB（`0x48DB80`）与 12..14（`0x4BB840`）由取样点定；**"第 10 号槽谁用"未核** |
| S7 | `[Sound+0x560]`（`this[344]`）是**死门**：全 `.lst` 只有 ctor 写它（`0x4B5C22 mov [edi+560h], ebx`，`ebx=0`），**没有任何置 1 的写入者**；而 `0x4B5C53 / 0x4B5D0C / 0x4B6026 / 0x4B60C6 / 0x4B6136 / 0x4B6196 / 0x4B6216 / 0x4B6353 / 0x4B65A3 / 0x4B691B` 全是 `cmp …,0` | ⇒ 所有"音频整体关闭"的早退分支**恒不成立**；真正的门是 `[Sound+0x408]`（pDS）与每槽 `[Sound+0x654+4s]`。这给 `RES-AUDIO-WAV-NO-BACKEND` 的"两个空指针是否同一状态"一个答案：**不是**（`+0x560` 恒 0，`+0x408` 才是后端） |
| S8 | 主音量默认 `[Sound+0x558] = 0x2710 (10000)`（`0x4B5C00`）；`sub_4B6210` 的 `this[342]` 就是它 | 逐槽音量在 `[Sound+0x618+4s]`（`this[390+s]`） |

### 4.3 待核（明确没核，别当结论用）

1. **CD / MIDI 后端的选择逻辑**：`Music::` ctor（`sub_489970@0x489970`）无脑 new 出 `NoMusic`/`CD`/`MIDI`/`PCM` 四个后端并挂在 `this[266..271]`；**哪个被选中**的判据没读（可能在 `Music::` 的 open/play 里）。⇒ "本作 BGM 不走 MCI" 的依据是"AOGG/OggS 文件 + `sound:UseDirectSound` 配置 + 无 CD 曲目"，**不是**"CD 后端不可能被调用"。
2. **`OggVorbis` / `OggStream` 的 `open` 是否真能在这条模拟器上跑通**（`sub_48CF70` / `sub_48D610`），以及解码吞吐。
3. **`AOGG` 容器的语义**（`OggStream` 派生自 `OggVorbis`，只多了 5 个字段；是否只是"带循环点的 Ogg"未读）。
4. **`GetCurrentPosition` 之外**，引擎是否有**别的**读"播放进度"的路（例如 `sub_4B6130` 的 `+0x2460` 在播放中是否会被别的路径更新）。
5. **`Sound+0x518` 之后**（`+0x51C..+0x55C`）还有哪些线程相关字段未读；`[Sound+0x560]` 已核（见 S7：死门）。
6. **本次没有重跑游戏**（按要求），所以"当前 boot 到哪一帧 / BGM 是否已被请求"没有新现场；`a9-thread.md` 是 2026-11-05 的日志。

### 4.4 已知的、与本次评估相关但不属于它的偏差（列出以免混淆）

- `SCOPE.md:21` 说 WAV 头/缓冲层是真的、输出仍静音 —— 本次评估确认**这句话现在仍然准确**。
- `SCOPE.md:25` 的"伪线程"与 `README.md:105-117` 一致；本报告的 §3.7 D1 就是要求改这一条。
- `SCOPE.md:36` 说"音频/视频的里程碑 = M4"；M4 单 `REQ-01M6K84KHPA1XF3B1HSHXME6W0` 的**范围写的是 MCI/mixer/mmio**，而 `.lst` 证明**真正的声音在 DirectSound 流式缓冲**上（MCI/mixer 只服务 CD/MIDI 后端）。⇒ **M4 的范围描述与语料不一致**，开工前应先改单（这是"声明的范围 vs 证据"的偏差，不是实现问题）。

---

## 5. 下一步可派给实现 subagent 的 3 个最小任务

**T1（P1，2–3 人日）· 真 DS 设备模型 + 位置通知（不动线程）**
- 改动：`src/runtime/audio_device.{h,cpp}`（新）、`src/shims/dsound.cpp`（`SetNotificationPositions` 真存、游标推进、`GetCurrentPosition` 改 `%bytes`）、`machine.cpp` 块边界补投 `SetEvent`。
- 交付判据：§3.6 的 CTest `audio-device-model` (a)–(e) 全绿 + 变异自检（`tools/mutate-check.mjs` 加 3 条）全红。
- ⛔ 不做：任何 guest 线程、任何 SDL 音频、任何格式变更。

**T2（P2 + P4，4–6 人日）· 宿主混音 + 输出 + `--audio-dump`**
- 改动：`AudioSink` 接口 + 混音（15 槽，`SetVolume`/`SetPan` 生效）+ `--audio-dump` + `--audio=virtual|sdl|off`（默认 `off`，避免改掉现有 A4 行为）。
- 交付判据：CTest（pure）用虚拟 sink 断言"两槽不同波形按 vol/pan 相加后的样本逐字节等于期望"；`--audio-dump` 产出的 WAV 能被 `ffprobe`/自写解析读成 44100/2ch/16bit 且 RMS>0（assets 档）。
- ⛔ 不做：线程。

**T3（P3，4–7 人日）· 协作式 guest 线程（只为音频）—— 依赖用户 V1 裁决（若选 D，则改做 P3-D）**
- 改动：`X86` 的第二上下文（regs + guest 栈 + TEB + `fs_base`）、`CreateThread` 真建栈/真入口、阻塞等待→挂起/唤醒、线程句柄"退出即 signaled"、`ExitThread`、每线程 TLS 数组、`GetCurrentThreadId`；调度点在 `machine.cpp` 的块边界（与 `fireDueTimer` 同形）。
- 交付判据：`CreateThread` 的 guest 程序（`WaitForMultipleObjects` + `CreateEvent` + 退出）在 CTest 里跑完且**线程体执行次数 ≥1**；随后"BGM 连续 30 s 无 underrun"（`--audio=sdl`，或虚拟 sink 下按圈计数断言 60 圈无空圈）。
- ⛔ 不做：宿主 `std::thread`、`_beginthreadex`、timer queue、DirectShow 线程。

---

*报告完。本文件只写在 `.tmp/`，未触碰源码、未 `git add`、未运行游戏。*
