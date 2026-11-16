# 🐞S3 `--time-limit 0` 的 help 说"0 = 不限"、代码却 exit 2 ⇒ 固定传 0 的探索脚本换二进制就整轮跑不起来

- id: REQ-01M711JSWPKHGYKGM4NQ7QG7YY
- type: bug
- status: done
- parent: REQ-01M6XVYA68DQWWMQPY795WPN0Q
- repro: docs/03-emulator/observations.md#time-limit
- severity: S3
- done_reason: 该能力（--time-limit 的 CLI 边界）随解释器退役（2026-11-16）；AOT 臂 --time-limit 仍在用，但其边界断言已无端到端守卫 —— 已知覆盖缺口，登记在 REQ-01M7HQ57XV66FZQA5ECGHWD71N 正文。
- tags: [emulator-lift, cli, usability]

## 判据（存在性 + 观测）

- **help 与代码不一致**：解释器 `apps/emulator-lift/src/main.cpp`（已随解释器退役）的 usage 第 106 行写
  `--time-limit <秒>   墙钟上限（探索用；0 = 不限，缺省）`，而同一文件 `:305-309` 的实现是
  `time_limit_sec = strtod(...); if (time_limit_sec <= 0.0) { "需要正数秒"; return 2; }`。
- **可复现**：`.\out\emulator-lift.exe --time-limit 0 ...` ⇒ 立刻 `exit 2`、无运行摘要（实测 188 ms）。
  这条不是"文档小错"：`run.ps1` 这一类脚本把 `--time-limit 0` 当"不限"写死在 argv 里 ⇒ **换一份二进制就整轮跑不起来**，
  而且失败得很早，看起来像"模拟器坏了"。（本批测量时就是这样踩到的：*.tmp/reveq/…/run.ps1* 固定传 `--time-limit 0`。）
- 判据：**三处必须一致** —— usage 文本 / 解析接受的值域 / 实际行为。候选：① 代码接受 `0`（= 不限，与 help 一致）；
  ② 代码只接受正数，usage 改成"缺省不限（省略该参数）"。⛔ 不许只改一边。

## 为什么现在不做（严重度）

- 不影响引擎语义、不影响已有门的任何用例（默认不带这个 flag）。按"只影响探索脚本、失败得很早且可见"取 **S3**。
- 但它会让**同一个脚本在本仓的不同二进制上表现不同**（旧二进制接受 0、新二进制拒绝）⇒ 归因成本高，值得单独一条。

## 修法边界

- 选 ① 时注意：`time_limit_sec` 是 `double`，`0` = 不限的实现要保证 `--time-limit 0` 与"不给这个 flag"**完全等价**（含摘要里的读数）。
- 选 ② 时把 `*.tmp*/` 与 `apps/emulator-lift/scripts/**` 里所有 `--time-limit 0` 的写法一起看（脚本不在门禁里，但会在下一个人手里炸）。
- 守卫：一条 pure CTest 或 `main` 级用例断言"help 里出现的每个 `--time-limit` 取值规则与实际解析一致"很难写死；
  更实际的是**断言 `--time-limit 0` 的行为等价于省略**（选 ① 时），这样 help 就不可能再骗人。

## 与别的单的关系

- 发现于 `REQ-01M6XVZG4T9WY8SQ1WS7P8RNW9` 的测量（借 `run.ps1` 的 argv 时踩到）。
- 同类"help 与代码不一致"若还有别的，**不要并进本单**：本单只收 `--time-limit`。
