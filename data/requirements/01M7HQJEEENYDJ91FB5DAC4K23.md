# AOT 侧压力/性能档重建：判据 = AOT 的 IR 执行速度（替代随解释器退役的 6 条 LIFT_STRESS）

- id: REQ-01M7HQJEEENYDJ91FB5DAC4K23
- type: req
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, aot, stress, perf]

## 一句话

`LIFT_STRESS` 这个档（唯一允许跑真游戏的档）今天**全部由解释器驱动**，而那 6 条要随解释器退役
（用户 2026-11-16 裁决：AOT 速度已远超解释器，测试目标已不存在）⇒ **AOT 侧的压力/性能档要重建**，
否则端到端速度**没有任何自动化读数**。

## 为什么必须有（不是"锦上添花"）

- M-lift-2（`REQ-01M7FC3W0ZMA35PK3WV73YXB1Q`）明写"**release 端到端为唯一速度判据**"。
- 退役后 `LIFT_STRESS` 归零 ⇒ 这条判据将只剩人手跑，无法回归。

## 与旧档的关系（⛔ 不要照抄）

- 旧 6 条：**带 `--native-overrides all` 的解释器读数**，判据是"跟历史基线比"。
- 新档：判据是 **AOT 的 IR 执行速度**（无 override，`-O2` 的 lifted IR），⛔ **不是**"跟解释器比"。
- 阈值**在本机 win32 / RelWithDebInfo 上重定**（`AGENTS.md` §12.6：同机、release、交错 A/B ≥3 对、取中位；本机分辨不了 <5%）。
- 驱动：用能**确定性到达某个屏**的配方（今天 `await position` 已能给确定性门；见 `OBS-20261116-05`）⇒ ⛔ 不要再用 `wait` 计时器当门。

## 待测路径（用户 2026-11-16 指定；逐步补充，不一次做完）

| # | 路径 | 为什么值得测（已有读数/取证） |
|---|---|---|
| 1 | **boot → TITLE 稳定** | 旧 `stress-boot` / `stress-title-steady` 的对应物；启动链有已知的装载长活 |
| 2 | **打开 FELLOW** | `graphs/sn0000-quit.graph` 的注释：穿过一次 **~7–17 s 的同步长活**（nested guest call：窗口里一次 Present 都没有、指令照涨） |
| 3 | **加载存档** | `title-loaddata` 那条路径；存档写入侧同类（缩略图 STH + 1 MB DAT ≈ 7 s） |

★ 全部**依赖输入系统**才能确定性到达那个屏：`await position` 的确定性门（`OBS-20261116-05`）+ 配方完成度判据
（`REQ-01M7HP2YZ4CRMQHYVSS8HH611Z`，未走完 ⇒ 非 0 退出）⇒ **输入那部分落地后**再逐条补。

★ 度量口径：这些场景的特征是"**单帧执行大量指令**" ⇒ 读数用**单帧指令数 + 帧时间**
（⛔ 不拿 `.ll` 行数当效率指标，`AGENTS.md` §12.6）；动态读数仍要 ≥3 对交错 A/B 取中位。

## 判据（能红）

1. 同一份配方在 AOT 臂连跑 3 次，语义指纹逐位相同（`exit` / `frames` / `stop @` / arena 三元组 / `table misses`）。
2. 速度读数有**抖动界线**（≥3 对交错 A/B 的中位 + 极差），并写明"本机分辨不了 <5%"。
3. 该档只在 `LIFT_STRESS=1` 下注册（`assets` 档，⛔ 不进默认门禁）；缺资产 ⇒ `FATAL_ERROR`（不静默 skip）。
4. 一条"能红"的自检：把配方改成永不满足的门 ⇒ 必须失败（依赖 `REQ-01M7HP2YZ4CRMQHYVSS8HH611Z` 的配方完成度判据）。

## 未做

不实现（本单先立判据与驱动方式）；不删旧的 6 条（退役由 `REQ-01M7HQ57XV66FZQA5ECEGHWD71N` 的第 4 步安排）。
