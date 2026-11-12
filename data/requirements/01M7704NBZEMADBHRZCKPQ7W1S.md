# emulator-lift · 剩余 Windows 专有守卫搬 POSIX（cli-time-limit / cli-dump-frame / title-entry-paths / movie-logo / frame-dump-failure）

- id: REQ-01M7704NBZEMADBHRZCKPQ7W1S
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, tests, portability, macos]

## 一句话
macOS 上仍有几条守卫因为"子进程启动用 `CreateProcessW`"而 `LIFT_SKIP`；`option_roundtrip.cpp` 已经有一条可复用的
POSIX `runChild`（`posix_spawn` + stdout/stderr 同一日志 fd + `waitpid` 超时），把剩下的搬过去即可让它们在两台设备上都跑。
资产（`links/install`）两台设备都有 ⇒ **跳过原因不是缺资产**。

## 现状（2026-11-12，`rg -n 'Windows 专有|只在 Windows' apps/emulator-lift/tests`）
| 文件 | 现状 | 处置 |
|---|---|---|
| `tests/cli_time_limit.cpp:196` | 非 Windows `LIFT_SKIP("…CreateProcessW…")` | 搬 POSIX `runChild` |
| `tests/cli_dump_frame.cpp:179` | 同上 | 同上 |
| `tests/title_entry_paths.cpp:158-161` | 非 Windows `LIFT_SKIP` | 同上 |
| `tests/movie_logo.cpp:149-152` | 非 Windows `LIFT_SKIP` | 同上（Debug 树的 SKIP 保留：那是构建类型，不是平台） |
| `tests/frame_dump_failure.cpp:69` | 用 MSVC `<io.h>` 造"目录当文件写" | 用 POSIX 等价（`open(dir, O_WRONLY)` / `EISDIR`）或按能力分支 |
| `tests/fault_report.cpp` / `host_encoding.cpp` | 已有两平台分支（无需改） | 只复核一遍 |

## 模板
`apps/emulator-lift/tests/option_roundtrip.cpp` 的 `runChild`（POSIX 分支）：
`posix_spawn` + `posix_spawn_file_actions_addopen(STDOUT_FILENO, log, O_WRONLY|O_CREAT|O_TRUNC)` 再
`adddup2(STDOUT_FILENO, STDERR_FILENO)`（同一个 open file description ⇒ 同一份日志；⛔ 不要两次 addopen）；
`waitpid(WNOHANG)` 轮询 + 到点 `SIGKILL`；`WIFEXITED` / `WIFSIGNALED`（`128+sig`）映射。
Windows 分支保留 `CreateProcessW`（机械改动）。

## 判据
- 四条守卫在 macOS 上不再因"平台"SKIP；`LIFT_SKIP` 只剩**环境**原因（GPU / 窗口 / 安装 / Debug 树）。
- 各自判据一条不放宽（超时、退出码、日志断言原样）。
- `ctest --test-dir out`（`LIFT_ASSETS=1` 时含资产用例）全绿；`git diff` 只多 tests + 必要的 CMake 注释。

## 风险
- `cli-time-limit` 的判据是"到点 exit 码"；POSIX 的 `SIGKILL` 映射要与非 Windows 的期望一致（先读它的判据再改）。
- `title_entry_paths` / `movie_logo` 的子进程参数不同（`movie-logo` 还要 corpus exe 与 appdata）⇒
  不要硬套 `makeChildArgs`，只复用 `runChild` 的机制。
