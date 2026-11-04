# S6 六处语义未复核（打开模式 / 共享冲突 / uUnique / FIND_DATA 时间 / 权限降级 / SetFilePointerEx）

- id: REQ-01M6JCC22PHPC24ZQN4P65Z1K5
- type: req
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- verify: apps/emulator-lift/tests/file_io.cpp#access-zero-open
- tags: [emulator-lift, m1, tech-debt]

## 一句话
六处 S6 语义已逐项复核并落地：**S6-1 / S6-2 / S6-3 / S6-5 / S6-6 修实现**，**S6-4 用夹具把"三时间近似"钉住**。
每项都有能红的用例；改后 `boot-trace` 与改前**逐字一致**。

## 逐项结论与凭据（2026-11-04）
| 项 | 结论 | 落点 | 守卫用例 |
|---|---|---|---|
| S6-1 `access=0` | **修**：完整解码 `GENERIC_*` / `FILE_*_DATA`；`access=0` 不授予读/写；截断已有文件需写权（`TRUNCATE_EXISTING`、`CREATE_ALWAYS` 命中已存在）⇒ `INVALID_HANDLE_VALUE + 5` | `kernel32_file.cpp` | `file-io#access-zero-open`、`file-io#truncate-needs-write` |
| S6-2 共享冲突 | **修**：双向判定（新 access × 已有 share、已有 access × 新 share），只由访问位定义；`access=0` 两个方向都不参与（两个只查属性的探针可并开）；`FILE_SHARE_DELETE` / `DELETE` 不建模（留日志） | 同上 | `file-io#share-mode-conflict`（a/b/c/d） |
| S6-3 `GetTempFileNameA` | **修**：`uUnique!=0` ⇒ 用调用方值（低 16 位、`%04X` 大写）、**不创建**、返回原值；`uUnique==0` 保留"从 0 起找空号 + 创建空文件"（**有意偏差**：真机从系统时间起）；前缀按字节截 3、扩展名 `.TMP` | 同上 | `file-path-ext#temp-file-name` |
| S6-4 `FIND_DATA` 时间 | **实现真值**（2026-11-04 追加）：`vfs` 的 `hostFileTimes` 走 SDL3 `SDL_GetPathInfo`（三时间各写各的，含 creation/access）；SDL 失败才回退 mtime。夹具用 `SetFileTime` 设两个**不同**过去值，要求 FIND_DATA 与 `GetFileTime` 逐值相等且不等于 creation | `src/runtime/vfs.{h,cpp}` + 同上 | `file-io#find-data-times` |
| S6-5 `DuplicateHandle` | **修**：删 `access == 0 ⇒ true` 短路（零访问位的复制句柄不许改状态）；伪句柄 `GetCurrentThread/Process` **实体化**成真句柄表项（`payload=nullptr`、无 destroy）；「只支持同进程」保留并写进偏差 9 | `kernel32_sync.cpp` | `sync-ext#duplicate-access-and-pseudo`（a/b/c/d） |
| S6-6 `SetFilePointerEx` | **修**：自己算"基准 + distance"（`FILE_BEGIN/CURRENT/END`）⇒ 目标 `< 0` 给 `ERROR_NEGATIVE_SEEK(131)` 且**不动指针**（不再靠宿主 `errno`）；非法 method ⇒ 87；越界 seek 成功、文件不变大 | `kernel32_file.cpp` | `file-io#set-file-pointer-ex`（7 条） |

## 门禁（本机 macOS arm64）
- `ctest` **20/20**（含 `fault-report` / `memory-pages`）；`LIFT_SAFE_MEMORY=1 ctest` **20/20**。
- `boot-trace`（A4）改前 = 改后：`insns=60652 hash=D06091AF5DD19647 inputs=35CB9281F6D0881B`（run1/run2 相同，`eip=004BA890`）。
- 守卫自检（改→重建→红→还原→sha256 一致→重建→绿）：
  ① `st->readable = want_read || (!want_write)` 回填 ⇒ `file-io` 红（`access-zero-open` 3 处断言失败）；
  ② `if (e->access == 0) return true;` 加回 ⇒ `sync-ext` 红（`duplicate-access-and-pseudo` 2 处）。
- `pnpm tools lift check` 绿 · `pnpm test` 1159 pass / 0 fail · `git diff --check` 干净。

## 保留 / 已知边界（不阻塞本单）
- **`<待核>`（无真机采样，夹具刻意不钉）**：`access=0` × {CREATE_ALWAYS 不存在 / CREATE_NEW / OPEN_ALWAYS} 的真机返回码；
  `ERROR_ACCESS_DENIED(5)` 这个具体码；`uUnique` 的 `%04X` 形态；`DuplicateHandle` 的"新权限超源"矩阵；
  `FIND_DATA` 三时间在 NTFS/APFS 的真值；`SetFilePointerEx` 失败后指针不动（文档对 `Ex` 没写，我们靠"自算 + 不调宿主"保证）。
- **`SetFilePointerEx` / `SetFilePointer` 的访问位**：已按 S6 偏差裁决单（`REQ-01M6JCZYWHVMZF8SAJ04XJGF29`）改为
  要求 `GENERIC_READ|WRITE`（`access=0` ⇒ `FALSE + 5`），夹具在 `access-zero-open` 里。
- **`CREATE_ALWAYS` / `OPEN_ALWAYS` 的 `last_error=183`** 是另一条单：`REQ-01M6JCZYJWVER06R6CQJ1N0ADC`（可能改游戏分支，单独做）。
- 偏差落点：用户批准的一条（`GetTempFileNameA(uUnique=0)` 从 0 起）已写进 `SCOPE.md` §1；其余为已实现/澄清，不需要 SCOPE。
- 审计报告（六节详情 + 9 条"没能证明的事"）：`.tmp/s6-audit/report.md`（不入库）。
