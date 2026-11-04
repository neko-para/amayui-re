# CreateFileA：CREATE_ALWAYS/OPEN_ALWAYS 命中已存在文件缺 last_error=183

- id: REQ-01M6JCZYJWVER06R6CQJ1N0ADC
- type: bug
- status: open
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- repro: apps/emulator-lift/src/shims/kernel32_file.cpp#createFile
- severity: S2
- tags: [emulator-lift, m1, s6]

## 一句话
`CreateFileA` 在 `CREATE_ALWAYS` / `OPEN_ALWAYS` **命中已存在文件**时没有把 `last_error` 置成
`ERROR_ALREADY_EXISTS(183)`；文档明写这两个 disposition 在"文件已存在"时必须置 183（函数仍成功）。
这是"首次运行 / 已有存档"的经典判据，guest 会用 `GetLastError()` 区分分支。

## 现状（2026-11-04 S6 审计）
- `apps/emulator-lift/src/shims/kernel32_file.cpp` 的 `createFile()` 在**成功路径**完全不碰
  `cpu.shim.last_error`；所以第二次 `CREATE_ALWAYS` 打开同一文件时，`GetLastError()` 是上一次遗留值，不是 183。
- 文档（CreateFileA `dwCreationDisposition` 表）：
  - `CREATE_ALWAYS`：文件存在 ⇒ "the function succeeds, and **last-error code is set to ERROR_ALREADY_EXISTS (183)**"；
  - `OPEN_ALWAYS`：同上（存在 ⇒ 183；不存在 ⇒ 新建且 `last_error` 不置 183）。
- 派生口径：`CREATE_NEW` 失败走 `ERROR_FILE_EXISTS(80)`（已实现）；`TRUNCATE_EXISTING` / `OPEN_EXISTING` 不置 183。

## 游戏侧可达性（`.lst`，审计已核）
`disposition=2`（CREATE_ALWAYS）共 8 处：`0x4089AD` / `0x42D59E` / `0x42DA7C` / `0x42E269` / `0x435593` /
`0x4355DA` / `0x438C95` / `0x45478B` / `0x49F63A`；`disposition=4`（OPEN_ALWAYS）1 处：`0x4547CB`。
⇒ **可能改变游戏分支**（这正是它不并入 S6 那批"低风险夹具"的原因）。

## 判据（待写实）
- 第二次 `CREATE_ALWAYS` 打开已存在文件 ⇒ 句柄有效 + `last_error == 183`；
- 首次（文件不存在）`CREATE_ALWAYS` ⇒ 句柄有效 + `last_error != 183`（先清零再调，断言未被写成 183）；
- `OPEN_ALWAYS` 存在 ⇒ 183；不存在 ⇒ 新建且非 183；
- 宿主盘断言：文件内容按 disposition 的既定语义处理（CREATE_ALWAYS 截断）。
- 夹具落 `tests/file_io.cpp`，用例名建议 `create-always-last-error`（顶层 pure CTest `file-io` 已注册，无需改 CMake）。

## 风险 / 验收要求
- 改完必须**单独**跑一次 assets 档 `boot-trace` 并对照 A4 的 `insns/hash/inputs`；变了就停下来分析（说明游戏走了 183 分支）。
- 与 `REQ-01M6JCC22PHPC24ZQN4P65Z1K5`（S6）是同一族的**另一条**：不要混在一起改。
