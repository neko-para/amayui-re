# 🐞S2 VFS 不认 guest 根前缀：引擎读自己的版本失败 ⇒ MessageBox ⇒ ExitProcess(0)

- id: REQ-01M6MPJ147RF41GJECZECXKC20
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/file_path_ext.cpp#guest-root-absolute
- repro: apps/emulator-lift/tests/file_path_ext.cpp#guest-root-absolute
- severity: S2
- tags: [emulator-lift, m2, path]

## 复现
```
apps/emulator-lift/out/agents/v5/emulator-lift.exe --game-dir ..\..\links\install \
    --appdata ..\..\.tmp\appdata --max-insns 1200000000
```
boot 跑到 **112,255,445** 条指令后被 guest 自己 `ExitProcess(0)`，修前日志：
```
[shim] version!GetFileVersionInfoSizeA: VFS 解析不了 "E:\Games\Eushully\天結\AGE.EXE"
       （[notdir] 段 "Games" 在 "E:\Projects\amayui-re\links\install" 里不存在）⇒ 返回 0 + 错误 2
[shim] user32!MessageBoxA: 模态 no-op ⇒ 返回 IDOK(1)
[shim] kernel32!ExitProcess: guest 请求退出：exit_code=0
```
机械 repro（都挂进已有 CTest）：`apps/emulator-lift/tests/file_path_ext.cpp` 的 `guest-root-absolute`
（CTest `file-path-ext`）· `apps/emulator-lift/tests/pe_resource.cpp` 的 `age-version-vfs-guest-root`
（CTest `pe-resource`）。把 `Vfs::resolve` 的入口归一改回"CP932 当 UTF-8"（= 修前口径）即可重放上面三行。
## 根因
guest 内存里的 A 版路径是 **CP932 字节**、`Vfs` 的 guest 路径文本是 **UTF-8**，而**版本族是唯一没在
边界解码的调用方**（`call.str(0)` 直接喂 `resolve`）⇒ 缺省 guest 根 `E:\Games\Eushully\天結` 里的
`天結` 两侧是两套字节（CP932 `93 56 8C 8B` / UTF-8 `E5 A4 A9 E7 B5 90`）⇒ 根前缀按字节比不中 ⇒
`Games` 被当成 `--game-dir` 下第一段 ⇒ `[notdir]`。同一原因让日志把 CP932 写进 UTF-8 的 stderr
（`天結` 显示成 `?V??`）。
- 文件族（`repo:apps/emulator-lift/src/shims/kernel32_file.cpp`）入口就 `cp932ToWide` + `utf16ToUtf8`，
  所以 `CreateFileA` / `FindFirstFileA` / `GetFullPathNameA` 一直是对的；
- 结构问题：**guest 根这一档**原本只是 `splitGuestImpl` 内部顺带做的前缀剥离，没写成 `resolve` 的
  显式一档，也没有"根先于 appdata"的注释与判据。
## 修法
1. **一条编码规则，解析入口与日志共用**：`guestPathForLog`（`repo:apps/emulator-lift/src/runtime/vfs.h` /
   `repo:apps/emulator-lift/src/runtime/vfs.cpp`）——合法 UTF-8 原样返回，否则按 CP932 解码；
   `resolve` / `find` / `splitGuestImpl` 入口都先过它 ⇒ Vfs 内部只有一套字节（⛔ 不猜第三个码页）。
2. **`resolve` 把 guest 根前缀写成显式一档**，顺序 **overlay → guest 根（最具体）→ `--appdata` → 相对路径**。
   前缀剥离只有一份实现 `stripGuestRootPrefix`（`isAbsoluteText` / `splitGuestImpl` / `resolve` 共用），
   只认**完整段前缀**（`…\Eushully\…`、`…\天結X`、根只出现在中间 一律不命中），盘符 / 大小写 /
   `\`·`/` / 末尾分隔符变体全认；余下文本**再走一遍** `splitRelativeText`（`..` 归一化 + `[escape]`）
   与分段解析（大小写不敏感 / overlay / 越界检查）——⛔ 没有"剥完直接拼宿主路径"。
3. **日志**：`vfs.cpp` 的解析失败文本、`kernel32_file.cpp` 的 `pathFailure`、`kernel32_version.cpp`
   的所有路径日志都走 `guestPathForLog`；修后同一行是 `…\天結\AGE.EXE`。
4. overlay **自然命中**：`resolve` 第一档就是 `matchesOverlay`（绝对化 + 按段大小写不敏感）⇒ 版本族
   拿到的宿主路径 = `--exe` 拆壳件 ⇒ 读 **DEBUG 件**的 `RT_VERSION`；**没有**为这条加任何特例。
## 期望 / 实际
- 期望：`version!GetFileVersionInfoSizeA("<guest 根>\AGE.EXE")` > 0（768 字节 = 拆壳件的 `RT_VERSION`）、
  `lpdwHandle` 写 0；`<guest 根>\SAVE\X.DAT` ⇒ `<game-dir>\SAVE\X.DAT`。
- 修后两条都成立（`VerQueryValueA` 拿到 `FileVersion = "4.60B"` 与 LegalCopyright）；两条用例全绿。
## 影响
阻断 boot：启动自检读不到自己的版本 ⇒ `MessageBoxA`（模态 no-op）⇒ `ExitProcess(0)`。所有
"guest 用 A 版 API 拿到自己的路径再喂回路径 API"的调用点都有同一隐患（版本族第一个撞上）。
## 变异验证（改坏就红，实测）
1. `stripGuestRootPrefix` 恒 `false`（去掉 guest 根档）⇒ `pe-resource` 红：
   `FAIL [age-version-vfs-guest-root] … overlayGuestPath() == "E:\Games\Eushully\天結\AGE.EXE"`。
2. "完整段前缀"改成"任意包含"（`text.find(root)`）⇒ `file-path-ext` 红：
   `FAIL [guest-root-absolute] … !v.resolve("C:\backup\Games\Eushully\天結\AGE.EXE", …)`。
3. `guestPathForLog` 改回原样返回（= 修前口径）⇒ 复现上面三行日志，且上面两条用例同时红。
## 不做 / 边界
- ⛔ 不认"路径里含 guest 根的某一段就剥"；⛔ 不在剥完前缀后绕过安全检查拼宿主路径。
- 盘符仍全部剥掉并映射到同一个 `--game-dir`（本模拟器只有一张"盘"，既有口径与用例不变）⇒
  "不是 guest 根的绝对路径"落在相对路径那一档，中间段不存在就响亮失败（反例用例即此）。
- `version!GetFileVersionInfoSizeW` 一族本镜像没有导入，不在本单。

## 修后 boot
`insns` 112,255,445 → **112,257,823**（+2,378；版本自检之后几乎全是 shim 调用，指令数涨得慢），
`version!GetFileVersionInfoSizeA` = **768 字节**（来源 `cpu.image`，宿主路径 = overlay 的 `--exe` 拆壳件）、
`VerQueryValueA` 拿到 `FileVersion = "4.60B"` / `LegalCopyright`，**没有再弹那次框**。
**下一个缺口 = 音频（DirectSound）**：`.lst` `0x00406D25` 的 `sub_4B5C50`（`LoadLibraryA("DSOUND.DLL")`）
返回 0 ⇒ `0x00406D38` 推 `"DSOUND.DLLの読み込みに失敗しました…DirectXをインストールしてください．"`
⇒ `sub_4065F0`（MessageBox）⇒ 清理后 `ExitProcess(0)`；日志里紧挨着
`[shim] kernel32!LoadLibraryA: 不认识的模块 "DSOUND.DLL" ⇒ 返回 0 + 错误 126`。SCOPE 把音频判
"延后实现（M4）/ silent"，但现在的 silent 只覆盖 MCI / mixer / mmio（winmm），**不覆盖 DSOUND.DLL 的加载**
⇒ 要么让 `LoadLibraryA` 认得它并给假模块 + 假 `DirectSoundCreate(8)`（"请求 + 返回码"口径），要么补一层
DSOUND silent。属**音频簇**，不属本单（也不在本单可改的文件里）。
