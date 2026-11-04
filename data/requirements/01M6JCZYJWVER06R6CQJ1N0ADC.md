# CreateFileA：CREATE_ALWAYS/OPEN_ALWAYS 命中已存在文件缺 last_error=183

- id: REQ-01M6JCZYJWVER06R6CQJ1N0ADC
- type: bug
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/file_io.cpp#create-always-last-error
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

## 落地（2026-11-04）
- `kernel32_file.cpp` 的 `createFile()`：在 `exists` / `disposition` 都解析完之后、任何失败返回之前算
  `already_existed = exists && (CREATE_ALWAYS || OPEN_ALWAYS)`；**只在句柄建成功之后**写
  `cpu.shim.last_error = 183`。其余情况**不碰** `last_error`（Win32 成功时也不清零）。
- 截断语义不变：`CREATE_ALWAYS` 仍截断，`OPEN_ALWAYS` 命中已存在仍**不**截断 ⇒ 183 只影响 last_error。
- 失败路径（共享冲突 / 无写权 / 宿主打开失败）不会顺带写成 183。

## 判据（已落实为 `file-io#create-always-last-error`）
- 首次（文件不存在）`CREATE_ALWAYS` ⇒ 句柄有效 + 清 0 之后 `last_error == 0`（不是 183）；
- 第二次 `CREATE_ALWAYS`（已存在）⇒ 句柄有效 + `last_error == 183` + 宿主文件被截断成 0 字节；
- `OPEN_ALWAYS` 存在 ⇒ 183 且内容保留（写 `"keep"` 后仍是 `"keep"`）；不存在 ⇒ 新建空文件、非 183；
- `OPEN_EXISTING` / `TRUNCATE_EXISTING` 命中已存在 ⇒ 非 183；`CREATE_NEW` 命中已存在 ⇒ `INVALID_HANDLE_VALUE + 80`。

## 门禁（本机 macOS arm64）
- `ctest` **20/20**；`LIFT_SAFE_MEMORY=1 ctest` **20/20**。
- A4 `boot-trace`（CTest 环境、与记录基线同一命令）改后 = 改前：
  `insns=60652 hash=D06091AF5DD19647 inputs=35CB9281F6D0881B`（run1/run2 相同，`eip=004BA890`）。
  另做了一次独立 A/B（短相对路径）：`insns=38456` 在开/关 183 两个构建下逐值相同
  ⇒ 本次改动的 last_error **没有改变游戏在 L2 之前的分支**。
- 变异自检：`pnpm tools mutate check --match 183` ⇒ `ctest:file-io` 退出码 8（`create-always-last-error` 红）。

## 保留 / 边界
- 本单只改"成功 + 已存在"这一格；`CREATE_ALWAYS` 命中已存在且 `access=0` 的真机返回码仍属 S6 的 `<待核>` 清单。
- A4 的指令数依赖 `--exe` 的路径写法（38,456 / 44,184 / 53,134 / 60,652 四种输入），引用必须连命令一起给。
