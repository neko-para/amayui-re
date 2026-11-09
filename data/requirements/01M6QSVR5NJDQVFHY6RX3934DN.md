# 🐞S1 emulator-lift · 标题 SE(WAV) 加载失败走到 Command_ShowMessage_Exception

- id: REQ-01M6QSVR5NJDQVFHY6RX3934DN
- type: bug
- status: done
- parent: REQ-01M6K84KHPA1XF3B1HSHXME6W0
- verify: apps/emulator-lift/tests/dsound.cpp#dsound-edge-create-buffers-and-get-format
- repro: apps/emulator-lift/src/shims/winmm_audio.cpp
- severity: S1
- done_reason: 两处根因都已修并各有守卫：① mmio 不再是空流（RIFF/fmt /data 真读 guest 文件）—— 证据是引擎能报出真 WAV 格式：本机 2026-11-09 带声音跑到 TITLE，CreateSoundBuffer #2=176400B/2ch/44.1k、#3=176400B/1ch/44.1k（SE 单声道）、#4=384000B/48k 流式缓冲，全部 DS_OK；② IDirectSoundBuffer8::QueryInterface(IID_IDirectSoundNotify) 返回真对象、SetNotificationPositions 真存并投递（日志 count=4 offsets=0,44100,…），挂在 dsound-edge-create-buffers-and-get-format。同一条路径再没有任何 RaiseException / Command_ShowMessage_Exception / 「WAVファイル…読み込みに失敗」（grep 计数 0），exit 0、called-unimplemented 0。★ 留下的边界只有原本就写明的：mmio 没有 pure 夹具（需要真 guest 文件句柄），由这条实机路径覆盖。

## 复现

- 用户口径（2026-11-06）：标题前的 LOGO / 渐变之后走到 `kernel32!RaiseException`。
- 本机复现（**墙钟**，不按 frame 数）：`--game-dir <install> --appdata <fresh> --native-overrides all
  --raster gpu --present headless --real-time --time-limit 90`；在标题脚本
  `play-sound-effect 39a3 1` 这一步停住。`.tmp/SYS4REG.INI` 与最小 `[display] UseIVideoWindow=1`
  都能复现（与 `UseIVideoWindow` 无关）。
- 运行日志：`CreateFileA("SE005.WAV")` 先失败（外层文件），引擎按 ALF 取出后 `mmioOpenA` 拿到
  guest 文件句柄；随后 `RaiseException`。

## 调用链（EA）

`_WinMain 0x4BB51C` → `0x420B3A`（`sub_420B00`，标题脚本）→ `sub_4B4F60`（SE 加载）→
`sub_4559C0`（文件号→句柄）→ `sub_4B6570`（WAV 类型分派）→ `sub_4BBD10` / `sub_4BBEF0`（WAV 头）→
`sub_4B71C0`（CreateSoundBuffer + QI）→ 返回 0 → `Command_ShowMessage_Exception`
（"WAVファイル SE005.WAV の読み込みに失敗しました"）→ `_CxxThrowException` → `RaiseException`
（返回地址 `0x4E8AB6`；SEH 未实现 ⇒ trap）。

## 实际根因（两个）

1. **`mmio*` 是空流**：`sub_4BBEF0` 用 `mmioOpenA` / `mmioDescend` / `mmioRead` 读
   `RIFF` / `fmt ` / `data`；原 no-op 让 `mmioRead` 恒 0 ⇒ WAV 头读失败。
2. **`IDirectSoundNotify` 缺失**：`sub_4B71C0` 在 `CreateSoundBuffer` 后 QI
   `{B0210783-89CD-11D0-AF08-00A0C925CD16}`；缓冲只回 `E_NOINTERFACE` ⇒ 同一失败分支。

## 修后

- `mmio` 沿 **guest 文件句柄的同一个 `FILE*`** 真读 RIFF / fmt / data（`mmioClose` 不关宿主文件；
  未知 flags / chunk 返回 `MMIOERR_CHUNKNOTFOUND(265)`）。
- `IDirectSoundBuffer8::QueryInterface(IID_IDirectSoundNotify)` 返回 `IDirectSoundNotify` 形状对象，
  `SetNotificationPositions` 返回 S_OK（静音：不投递事件）。
- MCI / mixer 仍是 SCOPE §1 的 silent no-op；音频不出声不变。

## 验证

- 同一条墙钟/GPU/override 命令：**不再 RaiseException**；下一处缺口变成
  `d3dx9_43!D3DXCreateTextureFromFileInMemory`（另属 M3/M4，不在本单）。
- `ctest` 的 `dsound`：100 checks / 0 failures（新增 notify QI + SetNotificationPositions）。
- ⚠ mmio 本身还没有 pure 夹具（需要真实 guest 文件句柄；本轮用实机路径 + `mmio` 日志验证）。
