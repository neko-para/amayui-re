# S6 偏差裁决（四条，已落地）：uUnique 起点进 SCOPE / 三时间改 SDL 真值 / GetTempPathA 两参数 / seek 校验访问位

- id: REQ-01M6JCZYWHVMZF8SAJ04XJGF29
- type: req
- status: done
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- done_reason: 四条按用户 2026-11-04 裁决全部落地：① uUnique=0 保持从 0，已写进 SCOPE §1；② 三个时间改为 SDL3 SDL_GetPathInfo 的宿主真值（FindFirstFileA/GetFileTime），SDL 失败才回退 mtime；③ GetTempPathA 按 (nBufferLength, lpBuffer) 两参数分别处理，(0,NULL) 回所需长度、(len>0,NULL) 回 0+87（宿主安全），不是偏差；④ SetFilePointer/SetFilePointerEx 要求 GENERIC_READ|GENERIC_WRITE，access=0 ⇒ FALSE+5。夹具 find-data-times 已改为验证三个时间彼此独立；守卫自检（mtime 回退 / 不校验 seek）都已实测红。ctest 20/20（safe 20/20）、boot-trace 逐字不变。
- tags: [emulator-lift, m1, s6, scope]

## 一句话
四条在 2026-11-04 全部裁决并落地：`uUnique=0` 保持从 0（已记 SCOPE）、三个时间改宿主真值、
`GetTempPathA` 按两个参数分别处理、seek 按接口定义校验访问位。

## 逐条
| # | 裁决 | 落地 |
|---|---|---|
| 1 | `GetTempFileNameA(uUnique=0)` **保持从 0** | `SCOPE.md` §1 新增「临时文件名序号」一行（用户 2026-11-04）；代码注释保留"有意偏差"说明 |
| 2 | 三个时间要**真实接口**（存档时间显示，错时间可见） | `vfs` 新增 `hostFileTimes`：**SDL3 `SDL_GetPathInfo` + `SDL_TimeToWindows`**（不新增依赖、实现里**没有平台 `#ifdef`**）；`FindFirstFileA` / `GetFileTime` 三值独立；SDL 失败才回退 mtime |
| 3 | `GetTempPathA` 是**两个参数** `(nBufferLength, lpBuffer)` | 分两种形态：`(0, NULL)` ⇒ 返回所需长度（文档："返回值 > nBufferLength ⇒ 所需大小"）；`(len>0, NULL)` 真机会写空指针，本模拟器改 `0 + ERROR_INVALID_PARAMETER(87)`（宿主安全）。**这不是偏差，不需要 SCOPE** |
| 4 | seek **按接口定义**实施 | `SetFilePointer` / `SetFilePointerEx` 要求 `GENERIC_READ|GENERIC_WRITE`：`access=0` ⇒ `FALSE + ERROR_ACCESS_DENIED(5)`；夹具补在 `access-zero-open` |

## 关于"平台抽象库"（boost / chrome base）的建议
不用引入：**SDL3 已经是本工程的依赖**，`SDL_GetPathInfo` 一次给出创建 / 修改 / 访问三个时间，
`SDL_TimeToWindows` 直接给 FILETIME（100ns 自 1601）⇒ 平台差异留在 SDL 内部，`vfs.cpp` 里没有平台 `#ifdef`。
将来若 SDL 抽象不够（例如需要 `FILE_WRITE_ATTRIBUTES` 这类 Win32 专属语义），再按"缺什么补什么"评估库。

## 验证
- `ctest` 20/20（`LIFT_SAFE_MEMORY=1` 也 20/20）· `boot-trace` 与改前逐字一致 · `pnpm test` 1159 pass / 0 fail ·
  `pnpm tools lift check` 绿 · `requirements validate` 6/6。
- 守卫：`file-io#find-data-times`（改回"三值 = mtime"会红：`SetFileTime` 设的两个过去值不再相等）、
  `file-io#access-zero-open`（去掉 seek 访问位检查会红）。两条都已加进 `tools/mutate-check.mjs` 清单。
