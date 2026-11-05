# M2 · 环境变量与 EXE 路径 overlay（LOCALAPPDATA 回退 / guest 根 = 游戏目录）

- id: REQ-01M6KRWSRD9WSKKAQFHHPY5HWN
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/file_path_ext.cpp#guest-env-block
- tags: [emulator-lift, m2]

## 一句话
把 guest 的**环境变量**与**特殊目录**收成一份真源，并让引擎眼里的主模块是**游戏目录里的 `AGE.EXE`**
（读到的字节由 VFS overlay 来自我们真正在跑的拆壳件）。收口判据：boot 不再往安装树里写
`SYS4REG.INI` / `SAVE` / `$$AGE.TMP`，它们落到 `--appdata` 之下。

## 为什么（2026-11-05 用户裁决）
引擎（`sub_4900F0` / `sub_409A40`）在 `set:UseAppDataFolder == 1` 时，XP 以上的分支**只**调
`SHGetFolderPathA(CSIDL_LOCAL_APPDATA|CSIDL_FLAG_CREATE)`；我们没有这个导出 ⇒ AppData 路径为空 ⇒
回落到配置里的相对 `set:RegFilePath`（`Eushully\天結いキャッスルマイスター`）⇒
`SYS4REG.INI` / `SAVE` / `$$AGE.TMP` 全落在 `--game-dir`（安装树）里。而环境块此前是写死的
`PATH=.;TEMP=E:\TMP`（`TEMP` 还是一条 VFS 里不存在的悬空路径），与 `SHGetSpecialFolderPathA`
返回的文本**两套各说各话**。

## 做成什么
- **环境块唯一真源** `lift::guestEnvironment(bool have_appdata)`（`runtime/vfs.cpp`）：
  同时喂给 `setupProcessStructures` 的 guest 块、`GetEnvironmentVariableA/W`、
  `GetEnvironmentStringsA/W`、`--trace-inputs` 的哈希。集合（无 `--appdata` 时去掉带 ★ 的四项）：
  `APPDATA★` / `COMSPEC` / `HOMEDRIVE` / `HOMEPATH` / `LOCALAPPDATA★` / `PATH=.` /
  `SystemDrive` / `SystemRoot` / `TEMP★` / `TMP★` / `USERPROFILE` / `windir`。
- **特殊目录**：`SHGetSpecialFolderPathA` / 新增 `SHGetFolderPathA` 的文本 = 同一组常量
  （`C:\Users\Amayui\AppData\{Local,Roaming}`、`C:\Users\Amayui\Documents`），经 VFS 落 `--appdata`。
- **没有 `--appdata`**：AppData 派生项**一律不提供**（`GetEnvironmentVariableA` = 0 + 203），与
  `SHGetSpecialFolderPathA` 的 FALSE + 5 同一口径；`GetTempPathA` 也响亮失败（⛔ 不再写
  `--game-dir/.lift-tmp`）。
- **临时目录**：`TEMP` / `TMP` / `GetTempPathA` = `C:\Users\Amayui\AppData\Local\Temp` →
  `<--appdata>/Local/Temp`；`setAppDataDir` 清它以保证 A4 逐字节可复现。
- **guest exe 路径** = `<guest 根>\AGE.EXE`（宿主 `--exe` 串只进 trace 输入清单，⛔ 不进 guest）。
- **VFS overlay**：只对一个文件生效（绝对化 + 大小写不敏感比较）；`resolve` / `find` / `toGuest`
  三处一致，命中打一行日志（来源 + sha256 前 8 位）。

## 判据（每条都有会红的用例；三条实测变异见 §验证）
- `apps/emulator-lift/tests/file_path_ext.cpp#guest-env-block`：环境块 == 真源；A/W 取值一致；
  特殊目录文本与 env **完全一致**；TEMP/TMP/GetTempPathA 同一处且能在 `--appdata` 下建出来。
- 同文件 `#guest-env-without-appdata`：没有 `--appdata` ⇒ 四项不提供 + 203 / FALSE+5 / E_ACCESSDENIED。
- 同文件 `#guest-exe-path`：`GetModuleFileNameA` / `GetCommandLineA` 只见 `D:\GAME\AGE.EXE`。
- 同文件 `#exe-overlay`：三种写法都读到拆壳件字节；`FindFirstFileA` 的枚举项与 `GetFileSize` 同一个大小；
  别的文件不重定向；宿主上两份字节都没被改。
- 同文件 `#no-writes-into-game-dir`：`setGameDir` 不删 `<game_dir>/.lift-tmp`；无 `--appdata` 时
  `GetTempPathA` 失败且 `--game-dir` 里没多出临时目录。
- `apps/emulator-lift/tests/boot_trace.cpp#install-tree-untouched`：boot 跑完后 `links/install` 根上那几个
  已知写入点（`SYS4REG.INI` / `SAVE` / `$$AGE.TMP` / `_.exe`）未变（2026-11-05 缩减：不再递归快照全树）。

## 验证（实测）
- 变异①（删掉 `LOCALAPPDATA`）⇒ `guest-env-block` 红（6 条断言）。
- 变异②（`matchesOverlay` 恒 false）⇒ `exe-overlay` 红（5 条断言）。
- 变异③（guest exe 路径改回 `img.path`）⇒ `guest-exe-path` 红（8 条断言）。
- boot（`--game-dir links/install --appdata .tmp/appdata`）：`SYS4REG.INI` / `SAVE` 落到
  `.tmp/appdata/Local/Eushully/天結いキャッスルマイスター/`；`links/install` 快照逐项未变。
- 停在 `user32!MessageBoxA`「AGERC.DLLのバージョンが異なります」⇒ `ExitProcess(0)`（105,094,044 条）：
  剩下的卡点是 AGERC 版本（另一条单 `REQ-01M6KGS8R0TWHY3KJQ12WJRXMZ`），不是写入落点。

## 不做 / 边界
- ⛔ 不改安装树里的 `SYS4INI.BIN`（`set:RegFilePath` 就存在里面）；那两个相对段**不是** exe 推导出来的。
- ⛔ 不做通用路径重定向（overlay 只有一个槽位）。
- ⛔ 不动 `user32_*` / `gdi32_*` / `memory.*` / `x86.*` / `exec_*` / `com.*` / `surface.*` / `present.*` / `font.*` / `ddraw.*`。
- `dynamic_exports.cpp` 里 `SHGetFolderPathA` 那条"OS 没有这个导出"已随实现移除（XP 的 shell32 有它）。
