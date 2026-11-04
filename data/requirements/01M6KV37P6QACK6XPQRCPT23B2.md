# 🐞S2 日文 guest 路径乱码：A 版路径按 CP932 却写成 UTF-8（GetModuleFileNameA/VFS）

- id: REQ-01M6KV37P6QACK6XPQRCPT23B2
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/file_path_ext.cpp#guest-exe-path-cp932
- repro: apps/emulator-lift/tests/file_path_ext.cpp#guest-exe-path-cp932
- severity: S2
- tags: [emulator-lift, m2, path]

## 现象

`--appdata` 里残留 `$$AGE.TMP` 时，引擎走"exe 版本自检 / `_.exe /FIX`"岔路，日志打出：

```
E:\Games\Eushully\螟ｩ邨申AGE.EXE
```

期望是 `E:\Games\Eushully\天結\AGE.EXE`（即 `天結` 的 **CP932** `93 56 8C 8B`，不是它的 UTF-8
`E5 A4 A9 E7 B5 90`）。

## 根因（一句话）

`setupProcessStructures` 把 `Vfs::guestExePath()` 的 **UTF-8 文本**原样写进 guest 的 exe 路径 /
命令行缓冲，而 `GetModuleFileNameA` / `GetCommandLineA` 又把这段字节**原样**交给 guest ——
A 版 API 的字节是 **CP932**，于是 `天結` 的 UTF-8 字节在引擎眼里是 CP932 的 `螟ｩ邨`。
同一处"UTF-8 字节 / CP932 字节"混用还让 `GetModuleFileNameW`（把 UTF-8 当 CP932 解）与
`GetModuleHandleA/W` 的模块名比较一起错。

## 修法

**VFS / 宿主内部只有一种表示 = UTF-8 文本**（`vfs.h` 的口径不变）；编码只发生在 API 边界：

- A 版**出口**编 CP932（`writeAnsiCp932`）、**入口**按 CP932 解（`ansiToUtf8`）；
- W 版一律 UTF-16（`writeWideUtf16` / `wideToUtf8Text`）：⛔ 不再经过 CP932，非 CP932 的字符
  不会被换成 `?`；
- `GetCommandLineA/W` 是**返回指针**的 API（调用方不给缓冲）⇒ 在 guest 里留一页转码缓冲
  （argv 块下面那一页；A @+0、W @+0x800），每次调用重写（幂等、无状态）；那一页被别的区域占了
  ⇒ **响亮失败**（布局冲突，⛔ 不静默写坏别人的镜像）；
- `runtime/cp932.{h,cpp}` 追加 `utf8ToCp932`（"按 CP932 编回字节"的唯一实现）与
  `cp932Truncate`（**成对**丢双字节字符：边界落在某个字符的 trail 上也不许切开）；
  `kernel32_file.cpp` 删掉自己那份同义实现，A 版的定长缓冲（`cFileName` / `lpTempFileName`）统一用它截断，
  并把 `GetTempFileNameA` 里直接打 CP932 字节的日志改成打转码后的文本。

## 复现 / 守卫

`apps/emulator-lift/tests/file_path_ext.cpp` 的用例 `guest-exe-path-cp932`：
guest 根 = `E:\Games\Eushully\天結`（= 真实缺省值），断言

- `GetModuleFileNameA` 的字节 == **CP932**（`天結` = `93 56 8C 8B`；⛔ 不是 UTF-8）；
- `GetCommandLineA` 指向的字节 == CP932（带引号）；`GetCommandLineW` == UTF-16；
- `GetModuleFileNameW` == `L"E:\Games\Eushully\天結\AGE.EXE"`；
- 把 ① 的字节**原样**喂回 `CreateFileA` ⇒ 必须命中 VFS overlay（引擎就是这么用 exe 路径的）；
- `FindFirstFileA` 的 `cFileName`（日文文件名）与 `GetFullPathNameA`（相对路径）同样是 CP932。

修前红 6 条（失败行里打印实际字节，正好是 `... E5 A4 A9 E7 B5 90 ...`），修后绿。

## 变异验证（实测；每次改坏都红，改回即绿）

1. A 版路径改回写 UTF-8 字节 ⇒ `guest-exe-path-cp932` 的 A 侧 2 条红。
2. W 版把 UTF-8 文本按 CP932 解 ⇒ W 侧 3 条红（`GetModuleFileNameW` + `GetCommandLineW`）。
3. `GetCommandLineA` 原样返回 `cmdline_ea` ⇒ 命令行那 1 条红。
4. `utf8ToCp932` 改成原样返回 ⇒ `file_path_ext` 6 条 + `locale_ext` 的 `cp932-byte-direction` 4 条红。

## 顺带核到的同一根因（其它面）

- 已修：`GetModuleFileNameW`、`GetCommandLineA`、`GetCommandLineW`（原"偏差 1"一并消掉）、
  `GetModuleHandleA/W` 与 `LoadLibraryA/W` 的名字入参、`GetTempFileNameA` 的日志编码。
- 本来就是 CP932 口径、本轮只补守卫：`FindFirstFileA` 的 `cFileName`、`GetFullPathNameA`、
  `GetCurrentDirectoryA`、`CreateFileA` 的日志与路径解析（`pathFromCp932` / `utf8ToCp932`）。
- 不涉及编码：`--trace-shims` 只打 module / name / EA 的十六进制（不含字符串）。
- ⚠ 留给下一轮（本轮文件在禁改清单里）：`ProcessSetup::guest_exe_path` 仍存 UTF-8 文本，
  `dumpTraceInputs` 用 `%s` 打它（日文缺省根下 trace 那一行是人读的 UTF-8，正常）；
  若将来把 guest 内存改成 CP932 口径，那一行要跟着转码。
