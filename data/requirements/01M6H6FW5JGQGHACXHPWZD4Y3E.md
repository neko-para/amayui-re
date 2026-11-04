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

## 验收（2026-11-04 · M1a，审查者独立复跑）
- `ctest` **10/10 通过**；A1 `114 cases / 267 checks / 0 failures`
- A2 `136/136`（未映射 0）；A3 `entries=298299 checked=298299`，`decode_fail / len / mnem = 0 / 0 / 0`
- A4 到达 `eip=0x004BA890`，两次运行 hash 相同
- A5 动态缺口 **0**；静态缺口 213/310
- 仓库侧 `pnpm test` 1148 pass；`SCOPE.md` / 语料 / 旧仓未越界

## 剩余（M1 收口前必须做完）
1. **S0 覆盖清单**：`data/lift/overrides.{json,md}` + `tools/lift.mjs`（`describe` / `check` / `report` / `corpus-report`），进 `tools/cli.mjs` 域地图；`json-docs` 守卫绿。
2. **澄清「静态缺口 213 vs `addUnimplemented` 83」**：摊成「已登记 stub / 导入表里有但未登记 / 从未被引用」三类，进 `lift check`。
3. **S6 后四族**：文件与路径（大小写不敏感 + 通配符，映射 `--game-dir`）、**真 CP932**、计时、完整进程/模块信息；`wsprintfA` 真实现。
4. **S8**：变异清单固化 + `bench` 吞吐入报告。
5. **S4 剩余**：x87 缺的形态（47 中的余项）+ 20 个 MMX/SSE 形态；超越函数走宿主 libm + 登记 1 ulp。
6. **A4 输入归一化**：把路径字符串 / 命令行 / 环境块 / 时钟起点列入 trace 输入；`--real-time` 接 CLI（用了真时钟则逐字节断言自动跳过并明说）。

## 已知边界（不阻塞收口，要点名）
- `--until` 之外曾停在 `USER32!wsprintfA`（`LoadLibraryA("AGERC.DLL")` 的失败分支）⇒ 由 AGERC stub + `wsprintfA` 解决。
- 码页现状只有 ASCII 直通（非 ASCII 按字节直通 + 一行日志）。
- MMX/SSE 与部分 x87 形态执行即**响亮失败**（可见缺口，不是静默 no-op）。
- A4 的指令数**依赖 `--exe` 的路径写法**（guest 命令行 = 该字符串）：实测 38,456 / 44,184 / 53,134 ⇒ 引用必须连路径一起给。

## 落点与真源
- 代码 `apps/emulator-lift/{src,tests,third_party}`；口径 `README.md`；偏差 `SCOPE.md`；计划 `docs/plan-m1-cpu-base.md`
- 交接备忘 `.tmp/lift-m1a-handoff.md` · `.tmp/lift-m1b-handoff.md`（**不入库**）
