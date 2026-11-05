# M2 · 音频 silent 层（DSOUND.DLL + DirectSound 设备/缓冲记账）+ MessageBoxA 打文本

- id: REQ-01M6MQPHSVZZSB8D4221Z0N1JW
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/dsound.cpp#dsound-edge-lock-write-unlock-play-accounting
- done_reason: 音频 silent 层 + MessageBoxA 文本已落地；原卡点混音线程由 REQ-01M6MQQA737CB8BAENQK088GP4 的伪线程裁决解决（CreateThread 只发句柄、不跑 start routine）。DSOUND 的 176400 字节 PCM 账本、Play/Stop/游标、MessageBoxA CP932 文本都有会红的夹具；boot 越过 0x004B5FE4 并到达 Present 第一帧。音频仍按 SCOPE 不出声，position-notify / 真宿主音频设备不在本单。
- tags: [emulator-lift, m2, audio]

## 修前现场（实测）
`LoadLibraryA("DSOUND.DLL") ⇒ 0 + 错误 126` ⇒ `stop @ eip=004EE0EA（ExitProcess）insns=112,257,823`；
`MessageBoxA` 那时只打一句 no-op ⇒ "引擎到底抱怨什么"在日志里看不见（本单第二件事）。

## 取证（`.lst`；锚 = EA，⛔ 不是 `.c` 行号）
- `0x00406D25 → 0x00406D2B call sub_4B5C50`（LoadDLL）返回非 1 ⇒ `0x00406D38`「DSOUND.DLLの読み込みに失敗…」⇒
  `0x00406D40 sub_4065F0`（MessageBox）⇒ 清理 ⇒ `ExitProcess(0)`。
- `sub_4B5C50` **只取一个导出**：`0x004B5C64 LoadLibraryA("DSOUND.DLL")` → `0x004B5C9F push aDirectsoundcre`
  （`.data:0052CEA0` = `"DirectSoundCreate"`）→ `0x004B5CA6 call eax`。整份 `.lst` 里 `DirectSoundCreate8` /
  `DirectSoundEnumerateA` / `DirectSoundCaptureCreate` / `IID_IDirectSound*` **0 命中** ⇒ ⛔ 不登记那些导出，引擎**不做 QI**。
- `sub_4B5CF0`（dsCreate，`0x00406D86` 调）在设备上真调的槽：`0x004B5C7A` DirectSoundCreate(NULL,&pDS,NULL) ·
  `0x004B5DDD` **+24 SetCooperativeLevel** · `0x004B5E61`/`0x004B5F19` **+12 CreateSoundBuffer**（primary/流式）·
  `0x004B649B`/`0x004B64A5` **+8 Release**。★ **没调** +16 GetCaps / +20 Duplicate / +28 GetSpeakerConfig。
- 缓冲上真调的槽：`0x004B5F77` **+44 Lock**(0,176400) · `0x004B5FA3` **+76 Unlock** · `0x004B5EAD`/`0x004B5FB3`
  **+48 Play**(0,0,1) · `0x004B6B7F` **+72 Stop** · `0x004B6C30` 还读 **+80 Restore**。
- **WAVEFORMATEX 原值**（`0x004B5EF6..0x004B5F0B`，引擎自己填）：`tag=1(PCM)` · `ch=2` · `44100Hz` · `avgBytes=176400` ·
  `blockAlign=4` · `16bit` · `cbSize=0`；流式 `dwBufferBytes=0x2B110=176400`、`dwFlags=0x181C8`，primary `0xC1`/零字节。
  随后 `0x004B5F83 memset(锁指针,0,176400)` —— **它真的往我们给的内存里写**。

## 做法
1. `kernel32_process.cpp` 的 `kLoadableModules` **表尾**追加 `"dsound.dll"`（假句柄 `0x7F19xxxx`）。⛔ 不插中间：
   假句柄 = `0x7F000000 + 表内序号×64KiB`，插中间会平移后面所有模块的句柄。
2. 新增 `shims/dsound.cpp`：`shims.add("dsound","DirectSoundCreate",…)`（动态导出，与 ddraw 同一条 G1 转发表）+
   `ComRegistry` 建 **IDirectSound8（11 槽）/ IDirectSoundBuffer8（24 槽）**；**账本全在 guest 堆状态块**
   （`'DSD8'`/`'DSB1'`，挂 `obj+8`）⇒ 宿主侧**零全局状态**（两台机器 / 两个夹具互不影响 = A4 前提）。
3. 已实现（静音设备的真值）：设备 `QI/AddRef/Release/CreateSoundBuffer/GetCaps/DuplicateSoundBuffer/SetCooperativeLevel/
   GetSpeakerConfig/SetSpeakerConfig`；缓冲 `QI/AddRef/Release/GetCaps/GetCurrentPosition/GetFormat/GetVolume/GetPan/
   GetFrequency/GetStatus/Lock/Play/SetCurrentPosition/SetFormat/SetVolume/SetPan/SetFrequency/Stop/Unlock/Restore`。
   **loud trap（`fn=nullptr` 显式占位撑满真实大小）**：设备 `+36 Initialize`/`+40 VerifyCertification`；缓冲
   `+40 Initialize`/`+84 SetFX`/`+88 AcquireResources`/`+92 GetObjectInPath`。
4. `Lock` 给**真 guest 内存**（`heap->alloc(dwBufferBytes)`；primary 的 0 字节给 1 字节占位）；`Unlock` 记窗口字节 +
   数**非 0 字节**；`Play/Stop/Set*` 记账；**同一对象同一方法只打第一次**，`Release` 打该对象最终账本。
5. `user32_menu.cpp`：`MessageBoxA` 打 `type=0x… caption="…" text="…"`（**CP932→UTF-8**，走 `runtime/cp932`；
   控制字符转义成 `\r\n` ⇒ 一行一条）。⛔ 返回语义不变（仍 `IDOK(1)`、不建窗口、不阻塞）。

## 判据（会红）
CTest `dsound`（`tests/dsound.cpp`，pure 顶层）：`…load-library-and-getproc-address` / `…create-device` /
`…query-interface-and-cooperative-level` / `…create-buffers-and-get-format` / `…lock-write-unlock-play-accounting` /
`…duplicate-sound-buffer` / `…call-after-release-is-loud` / `…silent-ledger-log`；CTest `user32-window`：
`message-box-cp932-log` / `message-box-cp932-log-run`。引擎写 PCM = **176400 字节**，两条独立路径
（`GetCurrentPosition` 游标 + 日志「累计 PCM 字节 176400」）；Play 调 4 次 ⇒ 日志**只许 2 行**（重复不打）+ `plays=1`；
WAVEFORMATEX 原值往返（`GetFormat` 逐字段 + `CreateSoundBuffer` 日志同一串）。

## 变异验证（实测 4 条，逐条 red → 已还原）
1. 删 `Unlock` 的 `bumpState(pcmBytes)` ⇒ `dsound` 红 4 条（游标 0≠176400、日志缺「累计 PCM 字节 176400」）。
2. `Play` 的 `if (firstTime(…))` 改恒真 ⇒ `…silent-ledger-log` 红：`expected 0x2, got 0x3`。
3. `MessageBoxA` 的 `cp932ToWide` 换成原样返回 ⇒ `user32-window` 红 2 条（日志里是 `\x83G…` 乱码）。
4. `kLoadableModules` 去掉 `"dsound.dll"` ⇒ `dsound` 红 3 条，并当场打出修前那行「不认识的模块 … 错误 126」。

## 修后 boot（两次一致 ⇒ 确定性；墙钟 ≈ 28–29 s）
```
[shim] kernel32!LoadLibraryA: 装载 "DSOUND.DLL" ⇒ 假句柄 0x7F190000
[shim] kernel32!GetProcAddress: dsound!DirectSoundCreate ⇒ thunk 0x7FFE05D0（已实现）
[dsound] CreateSoundBuffer: #2 bytes=176400 flags=0x000181C8 tag=1(PCM) ch=2 rate=44100Hz bits=16 blockAlign=4
[dsound] Unlock: p1=0x10246E38 n1=176400 ⇒ DS_OK（本区间非 0 字节 0；累计 PCM 字节 176400，plays=0）
stop      : unimplemented @ eip=7FFE01CC（kernel32!CreateThread）   insns = 109,536,868
```
`insns` 112,257,823 → **109,536,868**；本轮 boot **0 条 MessageBoxA**（没有再弹框）⇒ DSOUND 缺口确实越过。

## 当前卡点（所以保持 `doing`，未到第一帧）
`0x004B5FE4 call CreateThread`（`sub_4B5CF0` 建**混音线程**）：按 `SCOPE.md` §1 线程族是**硬错误** ⇒ boot 停在这里。
它**不是**音频 silent 层没做完（设备/缓冲已建好、PCM 账本 176400 字节已记）。往下要一条**裁决**：guest 单线程口径
是否允许"伪线程"（返假句柄、线程体永不运行）—— 那要先改 `SCOPE.md` §1 与 `README.md` §2.5，⛔ 不是本单能顺手做的。

## 不做 / 边界
⛔ 不登记引擎没取过的导出；⛔ 不产出声音（不建宿主音频设备、不重采样、游标 = 已提交字节数）；
⛔ 不发 position-notify 事件；⛔ 不改 `MessageBoxA` 返回语义；⛔ 不改 `SCOPE.md` / `docs/**`。
