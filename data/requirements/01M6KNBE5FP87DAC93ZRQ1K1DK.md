# M2 · boot 推进第二轮：小族（shlwapi / imagehlp / imm32）+ 写入落点裁决

- id: REQ-01M6KNBE5FP87DAC93ZRQ1K1DK
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, m2]

## 一句话
把 boot 继续往前推的第二轮：**小族**（`shlwapi` / `imagehlp` / `imm32`）已落地并有用例；boot 现在能跑到
**104.5M 条指令**、撞上"引擎要在**游戏目录相对路径**下建临时文件"这一步，然后走 `MessageBoxA(IDOK)` ⇒ `ExitProcess(0)`。
⇒ 下一段的卡点不是"缺一条 API"，而是**写入目标该落在哪**（`--game-dir` vs `--appdata`）。

## 已落地（每条都有会红的 CTest 用例）
| 导入 | 语义 | 用例 |
|---|---|---|
| `shlwapi!PathRemoveFileSpecA` | Win32 真语义：去最后一段（`\` / `/`）、`C:\a`→`C:\`、根目录不变、无分隔符 ⇒ 空串 + FALSE；返回"是否真的移除了东西" | `tests/file_path_ext.cpp#path-remove-file-spec` |
| `imagehlp!MakeSureDirectoryPathExists` | 逐段建目录（已存在算成功），落盘走 VFS / `--appdata` 口径 | `tests/file_path_ext.cpp`（同族用例） |
| `imm32!ImmAssociateContext` | `SCOPE.md` §1：完整 IME 不做 ⇒ 返回 **NULL** + 一行日志（⛔ 不是假成功） | `tests/user32_window.cpp` |

## boot 现状（2026-11-05，实测）
```
stop      : stopped @ eip=004EE0EA（shim 主动结束：kernel32!ExitProcess，exit_code=0）
insns     : 104,562,250
shims     : imported 310 / called-unimplemented 0
```
走到这里的路径（日志在 `.tmp/boot3.log`）：
`SetCursor` → 注册表 no-op 回落 → `PathRemoveFileSpecA("Eushully\天結いキャッスルマイスター\SYS4REG.INI")`
→ `LoadLibraryA("Shell32.dll")` + `GetProcAddress("SHGetFolderPathA")` 返回 127（引擎回落到已实现的 `SHGetSpecialFolderPathA`）
→ `CreateFileA("Eushully\天結いキャッスルマイスター\$$AGE.TMP")` **失败 2（不存在）**
→ `PathRemoveFileSpecA` + `MakeSureDirectoryPathExists` + 再 `CreateFileA` 仍失败 ⇒ `MessageBoxA` ⇒ `ExitProcess(0)`。

## ★ 下一段的真卡点（需要裁决）
1. **引擎把"自己的目录"算成了 `Eushully\天結いキャッスルマイスター\`（旧安装路径的尾巴）**，而我们的
   `--game-dir` 根就是安装树根 ⇒ 这个子目录在 VFS 里不存在。
2. `MakeSureDirectoryPathExists` 当时**真的在安装树里建了** `links/install/Eushully/天結いキャッスルマイスター/SAVE`
   （已手工删除）⇒ **测试卫生红线**：`AGENTS.md` / plan D5 都要求"⛔ 不写 `--game-dir`"。
   ⇒ 必须先决定"引擎往游戏目录写"这一类**该落到哪**，再实现，否则每跑一次就污染安装树。
3. 候选（要用户拍板或按 plan D5 的既有口径）+ 代价：
   - **A. 写重定向**：VFS 把 `--game-dir` 下的**写**请求重定向到 `--appdata` 的影子目录（读仍读安装树）。
     代价：要区分读/写；引擎若"先读自己写的文件"会读不到（影子目录要先覆盖读）。
   - **B. 影子安装树**：把安装树按需**稀疏**地复制到 `--appdata`（只复制引擎碰到的文件），`--game-dir` 只读。
     代价：首次复制/符号链接的开销与"哪些文件算碰到"的判定。
   - **C. 只做测试卫生**：实现照旧写 `--game-dir`，但每个 assets 用例跑前备份、跑后恢复，并断言"安装树字节未变"。
     代价：并发跑会打架；且"引擎真的能写"这件事本身就与 plan 的口径冲突。
   - ★ plan D5 已经明确"**不引入通用 `--work-dir` 覆盖**"，所以 A/B 里任何一种都得说清它与 D5 的边界。

## 没做（不在本轮范围）
- **gdi32 region 7 条**（`CreateRectRgn` / `CombineRgn` / `GetRegionData` / `PtInRegion` …）：boot 还没撞到（plan §6 P4）。
- `ole32!CoCreateInstance` / `oleaut32` VARIANT：属 M2 的 AGERC / 图像路径。
- `winmm` 音频、`mf*` 影片、`d3dx9`：M3/M4。
- 真实 SDL 输入 / 光标形状：P3。

## 边界
- 本单**只**负责"小族实现 + boot 卡点定位"；写入落点的裁决单独定（上面 §「下一段的真卡点」）。
- ⛔ 不要再让任何用例/探索运行写 `--game-dir`；发现写了就当作本单的 bug。

## ★ 2026-11-05 追加：卡点已定位到"引擎算错了自己的安装根"，不是"缺 API"
给 CreateFileA 的解析失败日志加上 dwCreationDisposition 后重跑（日志 `.tmp/boot5.log`）：
```
[shim] kernel32!CreateFileA: Eushully\天結いキャッスルマイスター\$$AGE.TMP 解析失败：dwCreationDisposition=3（必须已存在）
[shim] shlwapi!PathRemoveFileSpecA: "Eushully\天結いキャッスルマイスター\$$AGE.TMP" ⇒ "Eushully\天結いキャッスルマイスター"（TRUE）
（同样三步重复 3 次，然后 MessageBoxA ⇒ ExitProcess(0)）
```
- `3` = `OPEN_EXISTING` ⇒ 引擎是在**探测一个已存在的 `$$AGE.TMP`**，不是要新建它 ⇒ 失败是"文件本来就不该在"的正常分支，
  引擎随后 `PathRemoveFileSpecA` + `MakeSureDirectoryPathExists` 是在**准备目录**，然后再探测一次。
- ⇒ **真正可疑的是路径本身**：`Eushully\天結いキャッスルマイスター\...` 是**相对路径**，而且带着旧安装树的两段
  （`Eushully` / `天結いキャッスルマイスター`）。我们的 `--game-dir` 根**就是**游戏目录，所以这个相对路径在我们的 VFS 里
  多出两段。下一步取证（下一轮第一件事）：
  1. `GetModuleFileNameA` 现在返回给 guest 的是什么（`grep -n shimGetModuleFileNameA src/shims/kernel32_process.cpp`）；
  2. `.lst` 里 `0x00490382` 的调用者怎么拼这个路径（`pnpm tools disasm-at describe --ea 0x00490382` 往上找）；
  3. 引擎是否把"安装根"算成了 exe 目录的**父目录**（`E:\Games`），于是相对化后留下 `Eushully\天結いキャッスルマイスター`。
- ★ 判据：如果确认是"引擎按 exe 路径推导安装根"，那我们的 `--exe` / `GetModuleFileNameA` 就要给出**与安装树一致的 guest 路径**
  （例如 `C:\Eushully\天結いキャッスルマイスター\AGE.EXE` 且 VFS 把该前缀映射到 `--game-dir`），而不是改引擎语义。
- ★ 另：本轮把 `VirtualFree(MEM_RELEASE)` 从"只认 LIFO"改成"任意顺序整段释放"（见 `REQ-01M6JJ6ZYNT6FP5ESHRTFG9HA4`），
  boot 里那 5 条 `ERROR_INVALID_PARAMETER` 已消失；但它发生在 `ExitProcess` 之后的收尾路径上，**不改变**本卡点。
