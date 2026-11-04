# 平台偏差未登记 + win32 的 VEH 归因零机械证据

- id: REQ-01M6JFZFN15MBPG7Y89WNZACFW
- type: req
- status: blocked
- parent: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- blocked_by: [REQ-01M6JHVQJ6Q4YHG8N2BJP2ANJJ]
- tags: [emulator-lift, m1, platform]

## 一句话
平台相关偏差与 Windows 证据缺口。平台 6 条已由用户 2026-11-04 裁决（1/2/3/5/6 落地，4 待用户批方案，7 待用户选）；本单剩下的是 **win32 证据 / 构建**，需要 Windows 机器。

## 已落地（2026-11-04）
- 宿主入口的非 ASCII 路径：`openHostFileUtf8`（`_wfopen_s`）。
- creation time：macOS 用 `stat.st_birthtimespec`（有 birthtime 就用，否则 ctime）；Windows 继续 SDL；
  其它 POSIX 接受 ctime 回退（`SCOPE.md` §1 已记）。
- `SetFileTime` creation：Windows `::SetFileTime` / macOS `setattrlist(ATTR_CMN_CRTIME)` / 其它 POSIX 跳过
  （`SCOPE.md` §1 已记）；`file-io#find-data-times` 现在写 creation 再读回，变异实测红。
- 超越函数 1 ulp、宿主页保护粒度：`SCOPE.md` §1 两行；宿主 fault 归因分平台实现写入 §4。
- A4 路径：用户裁决"依然给真实路径，不额外处理"；分析报告 `.tmp/path-analysis.md` 保留，**未改代码**。

## 仍缺（本单的阻塞）
- `fault_report` 的 Windows 分支 `SKIP 77` ⇒ **win32（优先平台）的 VEH 归因零机械证据**；
  该测试的 verify 在 Windows 上永远不跑。
- `-Werror` 只在 clang 口径有效（MSVC `cl` 会把 `-Werror` 当 D9002 静默忽略），仓库无 CI ⇒
  `windows-x64` / `linux-x64` preset 从未被自动构建。
- `hostPageSize()` 在 Windows 硬编码 `0x1000`（应 `GetSystemInfo().dwPageSize`）；
  `memory.cpp` 只 `NOMINMAX` 没 `WIN32_LEAN_AND_MEAN`。
- fault hook 在异常上下文里 `textAt()` 返回 `std::string`（分配），违反 `memory.h` 自己写的
  "只许 fprintf / 预格式化缓冲"。

## 阻塞（2026-11-04）
- **机器**：win32 VEH 集成用例 / Windows 构建需要 Windows 机器（当前 macOS arm64），agent 无法替代实跑。
- **决策已收口**（`REQ-01M6JHVQJ6Q4YHG8N2BJP2ANJJ` done）：A4 保持真实路径、等待改硬错误；
  本单只剩"需要 Windows 机器"这一条。
