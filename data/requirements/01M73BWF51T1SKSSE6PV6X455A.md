# emulator-lift · quit-tail 夹具用 game_dir.string() 转坏非 ASCII 安装路径 ⇒ 子进程报「没有 --game-dir」

- id: REQ-01M73BWF51T1SKSSE6PV6X455A
- type: bug
- status: dropped
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- severity: S3
- dropped_reason: 夹具已删（2026-11-11 裁决，同 REQ-01M745ZGKZAHTCEQ9KQ7CY3JJ0）：这条修的是 quit-tail 夹具自己的子进程路径编码（Windows ANSI 代码页转坏 天結），夹具没了就没有守卫对象。口径（Windows 子进程命令行只用 UTF-16）落到 defects.md 的 BUG-21；仍在用的同族样例见 tests/title_entry_paths.cpp。
- tags: [emulator-lift test encoding]

## 一句话
`quit-tail` 夹具把安装目录用 `game_dir.string()` 转成窄串再拼进子进程命令行 ⇒ **日文路径在 Windows 上被转坏**，
子进程日志首行报 `没有 --game-dir`，测试在 `ctest` 里长期红（不是偶发）。

## 现象
- `ctest -R quit-tail` 红；子进程日志首行 = `没有 --game-dir`。
- 同族风险：其它用 `.string()` 传路径的夹具同样会坏。判据：`rg 'game_dir\.string\(\)|\.string\(\)' apps/emulator-lift/tests/`。

## 修法
- 路径在**边界上**保持 UTF-8 / 宽字符的一致口径（与 `main` 解析 `--game-dir` 的那一套同源），
  ⛔ 不要用 ANSI 代码页（会随系统区域设置漂）。
- 若夹具只需要"传一个路径给子进程"，就传原始 UTF-16 或 UTF-8，别在中途 `std::string` 化。

## 守卫
- `ctest -R quit-tail` 在**含非 ASCII 路径**的安装树上必须绿（现在红）；这是它自己的判据，不需要新用例。

## 修法（已落地）
- Windows 子进程命令行**只用原始 UTF-16**：`CreateProcessW` + `path::wstring()` → 子进程 `GetCommandLineW` → `argvUtf8()`（UTF-16→UTF-8）
  → `main` 存 UTF-8 → `vfs::pathFromUtf8`（UTF-8→`fs::path`）。**没有第三套约定**；POSIX 保持 `.string()`（原生 UTF-8）。
  与同族 `title_entry_paths.cpp` 一致。顺带修了 `LIFT_ASSETS_GAME_DIR` 日志里的 `.string()`。
- 变异：`game_dir.wstring()` 退回 `.string()`+`utf8ToWide` ⇒ 实测 **12/7、1.47 s、exit 8**；逐字节还原（sha256 `4AC694AD…3B97`）。
  清单 +1（`ctest:quit-tail` / `optInEnv: LIFT_ASSETS` / `ctestTimeoutSec: 1800`）。

## 复核读数（2026-11-11，`LIFT_ASSETS=1` 下 `ctest -R '^quit-tail$'`）
**12 checks / 7 failures（1.48 s）⇒ 24 checks / 1 failure（1203 s）**；子进程日志已逐字有 `game-dir : E:/Games/Eushully/天結/install`。
★ 剩下的那 1 条是**另一个缺陷**（`REQ-01M745ZGKZAHTCEQ9KQ7CY3JJ0`：`at frame` 锚在 boot 帧停滞后退化成墙钟）⇒ **本单不标 done**，
等那条修好后本单的守卫才会真正绿。

## 未做
- 不改 `quit-tail` 的尾段判据本身（读数形状仍按 `OBS-20261110-05`）。
- 不处理 `movie-logo` 的墙钟抖动（那是负载敏感，属另一类）。
