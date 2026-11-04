# emulator-lift · M1 · guest CPU 核心 + §3.1 T0 七族基座

- id: REQ-01M6H6FW5JGQGHACXHPWZD4Y3E
- type: req
- status: doing
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- tags: [emulator-lift m1]

## 一句话
`apps/emulator-lift` 的第一条可运行线：执行**拆壳件的原机器码**，穿过静态 CRT 启动走到 `_WinMain@16`，并把 `platform-effort.md` §3.1 的 **T0 语义七族**做成自写的 Win32 形状层。
★ 判据与切片的真源：`apps/emulator-lift/docs/plan-m1-cpu-base.md`；本单只记**边界 / 判据 / 状态**。

## 已裁决（用户）
| # | 内容 |
|---|---|
| D1 | 执行源 = 拆壳件字节（`binary/age-sectfix`，已入库 LFS：`corpus/assets/age/AGE.EXE__dumped.sectfix.EXE`）；`.lst` 降为符号 / 对照 / 覆盖真源 |
| D2 | 纯内存解释 + 块缓存，**默认零生成代码** |
| D3 | x87 算术内核 = vendor 的 Berkeley SoftFloat `extF80`（BSD-3，`third_party/berkeley-softfloat/`），不进 vcpkg |
| D6 | 基座口径 = T0 七族；**不含** GDI32 的 7 个 / d3dx9 的 6 个；USER32 只留字符串族 |
| — | AGERC 整体按 `SCOPE.md` stub（3 个导出返回合理值 + 登记），**不做**真模块装载 |

## 判据
| # | 判据 | 档 |
|---|---|---|
| A1 | 语义夹具全绿 + `LIFT_SAFE_MEMORY=1` 复跑全绿 + 至少两处变异能变红 | pure |
| A2 | 助记符未映射 = 0 | pure + assets |
| A3 | 解码对照：`EA` 差与助记符不一致 = 0 | assets |
| A4 | 到 `_WinMain@16`；同一命令两次 trace 逐字节相同；**输入清单固定**（路径 / 命令行 / 环境块 / 时钟起点） | assets |
| A5 | trace 里出现的导入 ⊆ 实现 ∪ 登记 stub；**七族内不许是 stub**（除非写进 `SCOPE.md`） | assets |

## 验收（2026-11-04 · M1a，审查者独立复跑 —— **历史快照**，当前数值见下）
- `ctest` **10/10 通过**；A1 `114 cases / 267 checks / 0 failures`
- A2 `136/136`（未映射 0）；A3 `entries=298299 checked=298299`，`decode_fail / len / mnem = 0 / 0 / 0`
- A4 到达 `eip=0x004BA890`，两次运行 hash 相同
- A5 动态缺口 **0**；静态缺口 213/310
- 仓库侧 `pnpm test` 1148 pass；`SCOPE.md` / 语料 / 旧仓未越界

## 现查（2026-11-04 审计后，引用时用这一节）
- `ctest` **22/22**（含 `conformance-safe` / `pe-loader`）；`LIFT_SAFE_MEMORY=1 ctest` **22/22**。
- A1 `conformance` **183 cases / 383 checks / 0 failures**；A2 `136/136` 未映射 0。
- A3 `entries=298299`、`len_checked=298299`、`decode_fail/len/mnem = 0/0/0`；collapsed 1805。
- A4 到达 `eip=0x004BA890`；`insns=60652 hash=E0379BF37C2DFAF3`（PF 修正后；insns 不变）；
  两份 shim trace 各 108,914 字节且逐字节相同。
- A5 动态缺口 0；静态缺口 **164/310**（含派生缺口文件）。
- `pnpm test` 1159 pass / 0 fail；`pnpm tools lift check` 绿。

## 剩余（M1 收口前，2026-11-04 重算）
- **M1a 审计后的活单**：S6 三族夹具（`REQ-01M6JFZ73GTZE9F6967RRSH6CE`）、平台 / win32 证据（`REQ-01M6JFZFN15MBPG7Y89WNZACFW`）；
  文档漂移单 `REQ-01M6JFZG5RY4GMZQCAA7HKNP1T` 本轮已收口。
- **S4 剩余逐项复核**（x87 / MMX / SSE 的实际缺口）—— 不在 M1a 审计范围，未做。
- 原「剩余」里的 1（S0 覆盖清单）/ 2（静态缺口分类）/ 3（S6 后四族）/ 4（S8）/ 6（A4 输入归一化）
  **均已完成**（见 `git log` 与对应子节点），不再是待办。

## 已知边界（不阻塞收口，要点名）
- `--until` 之外曾停在 `USER32!wsprintfA`（`LoadLibraryA("AGERC.DLL")` 的失败分支）⇒ 由 AGERC stub + `wsprintfA` 解决。
- 码页现状只有 ASCII 直通（非 ASCII 按字节直通 + 一行日志）。
- MMX/SSE 与部分 x87 形态执行即**响亮失败**（可见缺口，不是静默 no-op）。
- A4 的指令数**依赖 `--exe` 的路径写法**（guest 命令行 = 该字符串）：实测 38,456 / 44,184 / 53,134 ⇒ 引用必须连路径一起给。

## 落点与真源
- 代码 `apps/emulator-lift/{src,tests,third_party}`；口径 `README.md`；偏差 `SCOPE.md`；计划 `docs/plan-m1-cpu-base.md`
- 交接备忘 `.tmp/lift-m1a-handoff.md` · `.tmp/lift-m1b-handoff.md`（**不入库**）
