# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

本节点**保持 open**，作为「当前实现总览与交接」持续更新，不按收口处理（用户口径 2026-10-29）。
2026-10-29 长程两轮：**emulator-cxx 已派发但未成篇的 opcode 清零**（`dispatched ∩ gaps = 0`）；
`0x2CF` 已实现；`0x82` / `0x1D0` 各有**可判定结论（B：带 next 的残差 + 需求单）**。

## 交付物在哪

- **规范**：`repo:data/spec`（真源）· 工具 `pnpm tools spec validate|report|describe`。
- **C++ 实现线**：`repo:apps/emulator-cxx`（preset `windows-x64`；`out/` 是 binaryDir）；参考实现 `repo:apps/emulator-new`。
- **取证记录**（锚 EA、含不确定项，⛔ 不是 spec）：`repo:docs/02-engine/ops-forensics-2026-10-title-option.md`。
- **本线需求单**：`REQ-01M5WD5Q5HDE246W3T97F3NA8C`。

## 宿主观测面（不是引擎语义）

- 播放：`--input-script` / `--play-headless` / `--tick-ms` / `--anchor-script` / `--expect-script` /
  `--require-script` / `--bmp`。
- 诊断：`--watch-locals` · `--trace-script` / `--trace-opcodes` · `--dump-scene-at N` · `--dump-texture <id>`。

## 当前需要处理（2026-10-29 第二轮结束）

1. **`0x82` 的真实绘制 / `0x1D0`**：结论走 **B**（`REQ-01M63XEX9NXKJAJMZYJDCEX96Y`）—— 共同前置是**读排版层**
   （`sub_466000` 1000+ 行、对表 `Font+0xD34` 的 push 站点未定位）；下一轮从那里起，不是"补实现"。
2. **spec 已成篇、实现是桩 / 归属错**（`REQ-01M63YS3T54VYCXCBJ6RPCHDAY`）：`op_2c7` 空返回、`op_2de` 恒 −1、
   L2D 四条共用空 `op_341`、`op_2ee` 不写命名键、`op_25b` 不发载图请求、**`0xb7`/`0xb8` 被当成 BGM 播放/停止**。
3. **`0x215`**：取证已齐、映射未建模（`REQ-01M63XE3X819G3WAY28Y0XSJKV`）。
4. **未做**：`mutate-check` 清单只跑 node 守卫（C++ 侧变异靠手动）；多条实体的 evidence 只引到站点守卫 / 通用 CTest，
   升 `reviewed` 前要具名用例或 `KN:` 锚。

## 这一轮（2026-10-29 长程两轮）收口了什么

- **spec 成篇 7 批 + 2 条单点**，共 **34 个 opcode** 从 `audit.gaps` 移出（329 → 295），新增 **28 条规则**（253 → 281）：
  配置族 + 滚轮（9）· 声音设定族（4）· 标量/字符串/数组（7）· 时钟/显示状态/消息淡变（3）· L2D 载入与标记（4）·
  声音槽 stop/play + 音乐渐变（4）· 文本槽定时释放（1 条跨 2 opcode）· `0x194`。
- **提交前必过的判据**：每批 `gaps` 精确划分 + `spec validate` 13/13；每批都有**独立 subagent 复审**（4 轮，共 70+ 条意见），
  硬伤逐条处置（`0x12f` 的 N=0/N=1 终态、`0x2c7` 的修正顺序、`0xb7` 不写进度、槽界有符号/无符号…）。
- **实现**：`0x194` 进 `emulator-cxx`（读序 3 → 2、逐字节比较）+ `core_selftest` 三条断言。
- **订正**：`0x12f` 的键/收尾带池密钥且互为逆、`0x1bb` 的读者远多于旧记录、`0x301` 的释放**没有** delete、
  `system:EffectSkipOnClick` **不是**内建默认、`0xb7`/`0xb8` 不是 BGM 控制。

## 当前门禁（实测）

`ctest --test-dir apps/emulator-cxx/out` 24/24 · `pnpm test` 1103 通过 · `pnpm tools spec validate` 13/13 ·
`pnpm tools requirements validate` 6/6 · `pnpm tools corpus validate` 8 通过 / 0 失败 / 1 警告。

## 已打通（实测）

TITLE：`0x12e` 命中框表 → `0xa1/a2/a3` 菜单分派 → key 3 → `0x3 call-script 52` ⇒ CONFIG.BIN → `0x3 call-script 20940`。
点击在**抬起**时生效；左键 = 引擎位 0。

## 工作口径（2026-10-29）

- 本节点**不关闭**：进度以 `pnpm tools requirements plan` 为准；变更历史看 `git log`。
- **spec 完整性判据**：所有影响可观察行为的字段 / 触发 / 转换都写全，独立实现者**只凭 spec**能写出唯一实现。
- **spec vs emulator**：spec 写领域状态机；emulator 是示范与第二实现证据；分歧按 `amayui-spec` §9.2 归因。
