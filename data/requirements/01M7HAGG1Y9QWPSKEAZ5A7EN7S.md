# 🐞S2 macOS arm64 构建链接失败：-Wl,-Map 是 GNU ld 语法，Apple ld64 只认小写 -map

- id: REQ-01M7HAGG1Y9QWPSKEAZ5A7EN7S
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- repro: apps/emulator-lift/CMakeLists.txt#-Wl,-map,
- severity: S2
- tags: [emulator-lift, cmake, build, platform]

## 一句话

macOS arm64 上 `emulator-lift-aot` **链接失败**：`CMakeLists.txt` 把 Apple ld64 与 GNU ld / ld.lld（ELF）混在同一支，
用了 ELF 侧的大写 `-Map`，而 ld64 只认小写 `-map` ⇒ `ld: unknown options: -Map`，整个
`cmake --build --preset macos-arm64` 停在**第 1 个链接步**（编译全部成功，前面的链接也都成功）。

## 根因（实测，2026-11-16 / macOS arm64 / Apple `ld-1230.1`）

| flag | 结果 |
|---|---|
| `-Wl,-Map,<path>` | `exit=1`，`ld: unknown options: -Map`，**不产 map** |
| `-Wl,-map,<path>` | `exit=0`，产出 ld64 map（头 `# Path:` / `# Arch: arm64` / `# Object files:`） |

★ **这不是构建环境的缺陷**：不需要 lld、不需要改 preset / toolchain / vcpkg、不需要装任何东西。
证法不止"最小程序"：把该 target 的**真实链接命令**（`ninja -C out -t commands emulator-lift-aot` 取到）只翻一个字母、
输出指到 `/tmp` —— 大写红、小写绿；两份 map 只差 `# Path:` 那一行 ⇒ 唯一变量是大小写。
`-fuse-ld=lld`（ld64.lld）同样是小写通、大写红，所以 `if(APPLE)` 这一支对两种 Mach-O 链接器都成立。

## 修法（一处分支）

`apps/emulator-lift/CMakeLists.txt` 的 `elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")` 按平台拆开：
`if(APPLE)` ⇒ `-Wl,-map,…`；`else()` ⇒ 原来的 `-Wl,-Map,…`（**选项串逐字符未变**，仅缩进 4→8）。
`if(MSVC)` 的 `/MAP:`(226) 与 `/STACK:`(231) **逐字节未变**（sha256 与 HEAD 相同）。
map 路径不变：`out/emulator-lift-aot.map` —— `CMakeLists.txt:217` 写明它是 T0 那批读数的证据，⛔ 不要因为这条修把它弄丢。

## 判据（收口读数）

- `cmake --build --preset macos-arm64` ⇒ exit 0；再跑一次 `ninja: no work to do.`（干净收敛）
- `grep -c 'Wl,-map,' out/build.ninja` = **1**、`grep -c 'Wl,-Map,' out/build.ninja` = **0**
- `out/emulator-lift-aot.map` = **2,909,221 B / 46,512 行 / 437 个 object**（含 `part00–07.obj`），`# Path:` + `# Sections:` + `# Symbols:` 齐
- `ctest --test-dir out` ⇒ **102 tests, 100% passed**：passed **96** / failed **0** / skipped **6**（两次独立复跑：39.5 s 与 45.8 s）
- 6 条 skip 全是**结构性**：`cli-time-limit` · `cli-cursor-warp` · `cli-aot-flags` · `cli-dump-frame` · `frame-dump-failure`
  （`#if !defined(_WIN32)` + `CreateProcessW`，或 MSVC `<io.h>`）· `gpu-raster`（dummy 视频建不出 GPU device）。
  **没有一条是"该跑却被静默跳过"。**

## 怎么查（这条缺陷如果回来了）

1. 红在 `emulator-lift-aot` 的**链接**步 ⇒ `grep -n 'Wl,-map\|Wl,-Map' apps/emulator-lift/CMakeLists.txt` 看那支落在哪个平台分支。
2. map 到底有没有产出 ⇒ `ls -l apps/emulator-lift/out/emulator-lift-aot.map && head -3 …`（非 0 字节 / `# Path:` 开头）。

★ **已知的静默面（裁决：不建用例）**：ld64 不给 `-map` 也照样链接成功，而没有任何用例检查 map 存在
⇒ 若把这行整条删掉，构建**不会红**，消失的只是 T0 读数的证据。所以第 2 条是"怎么查"，不是 guard。

## 未做

- ⛔ 不为它写 CTest / 仓库守卫：`AGENTS.md` §10 三问里"变了是静默的"不成立（构建期**响亮**失败，改了就在 mac 上一眼红）。
- ⛔ 未在 win32 / Linux 上实跑这两支 —— 它们只是"选项串逐字未改"，不是实测。
- 文档漂移（已报、未修）：`CTX.md` §B 第 40 行的 mac skip 清单是 **4** 条，实测 **6** 条（漏 `cli-cursor-warp` / `cli-aot-flags`）。
- 顺带记一条既有事实（非本单引入）：mac 上 `cli-aot-flags` 结构性不跑 ⇒ "mac ctest 100%" **不等于**与 win32 同等覆盖。
