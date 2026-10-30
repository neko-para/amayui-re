# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

本节点**保持 open**，作为「当前实现总览与交接」持续更新，不按收口处理（用户口径 2026-10-29）。
2026-10-30：**GameStart → ADV 界面已跑通**。
2026-10-31：四个提交（`323b5a5..6821e19`）的 spec 回归修复收口；`apps/emulator-new` **冻结**（见下）。

## 交付物 / 实现线

- **规范**：`repo:data/spec`（真源）· 工具 `pnpm tools spec validate|report|describe`。
- **唯一在维护的模拟器**：`repo:apps/emulator-cxx`（preset `windows-x64`；`out/` 是 binaryDir）。
- **`apps/emulator` 与 `apps/emulator-new` 已废弃**（只读遗产）：⛔ 不新增功能 / 不补守卫 / 不为它们改 spec。
- **取证记录**（锚 EA、含不确定项，⛔ 不是 spec）：`repo:docs/02-engine/ops-forensics-2026-10-title-option.md`。
- **本线需求单**：`REQ-01M5WD5Q5HDE246W3T97F3NA8C`；ADV 路径单 `REQ-01M6495R6H85GSSVDGXPJVTAGH` 已 done。

## 宿主观测面（不是引擎语义）

- 播放：`--input-script` / `--play-headless` / `--tick-ms` / `--anchor-script` / `--expect-script` /
  `--require-script` / `--bmp` / `--bmp-stride` / `--bmp-limit`。
- 诊断：`--watch-locals` · `--trace-script` / `--trace-opcodes` · `--dump-scene-at N` · `--dump-texture <id>`。

## 已打通（实测 2026-10-30）

- TITLE：`0x12e` 命中框表 → `0xa1/a2/a3` 菜单分派 → key 3 → `0x3 call-script 52` ⇒ CONFIG.BIN → `0x3 call-script 20940`。
- **GameStart → ADV**：GameStart 菜单 → `0x3 call-script 20964`（INITGAME）→ … → NOVEL(21096) → **SN0000(116)** →
  DRAWCHARM(70，画角色) → SC0000(115)。点击驱动：**`0x72` 等推进**（门字第 31 位）→ 左键放行 → 一句一停。
- 证据（headless，`presented` 3188 帧 / 57 s）：`.tmp/adv/bmp5` 的 `frame-0700`（夕阳天空）、
  `.tmp/adv/bmp6` 的 `frame-1500`（章节标题卡）、`.tmp/adv/bmp4` 的末帧（消息窗正文）。

## 历史：2026-10-30 收口了什么

- **补 30 条 opcode**，每条都有 spec 落点 + `bindings` 站点/证据 + `audit` 残差（并同步移出 `GAP-OPS-*`）。
- **两处模型纠错**：① 门字第 9 位（BGM 淡变）补最小推进；② `0x1fb` 才是「场景对象记录」的建记录者。
- **正文渲染**：逐字符绘制条目需要「绘制键基数」，ADV 的消息层脚本没给 ⇒ 加宿主兜底（残差 `ADV-TEXT-LAYER-DRAW-KEY`）。

## 2026-10-31 收口了什么

- **`0x1ce` 身份改判并重写**：不是「乱数启用」，是「逐格揭示当前文本」的启停 ⇒ 规则改名
  `CORE-OPS-TEXT-CELL-REVEAL-1CE`、移到字符串章；格单位 = **等长瓦片**（≠ 打字机的字符格）；
  两套机制的游标 / 单位 / 终止规则都不同。
- **等待门（`0x21c`）两处到期边界纠错**：颜色窗 `≥`（相等已到期）、消息计时器 `>`（相等仍窗内）；
  源矩形窗同样拉「待处理」闩（原先只有颜色窗拉）。
- **门条新增「第 29 位例外」**：打字进行中（且待处理队列格 ≠ 0）时门忙**仍派发一条** ——
  这也是 `0x1ce` 停用支可达的**唯一已知路径**。
- **`0x72` 补三格合同**：无条件取格数；置第 30 位 / 清格号 / 复位相位**仅当第 30 位未置**。
- spec 侧经**两轮独立复审**（规则面 + 实现面）并落地复审后的 14 条修正；实现侧补齐 8 处
  「改坏会红」的守卫（含 `0x72` 四写点、掉拍不补、画推进前的格号、周期不被复位）。

## 当前需要处理（2026-10-31 交接）

1. 🐞 **五个缺陷单等用户真机验证后再动**（不自行收口）：序章旁白 / 章节标题卡 / TITLE 按钮提前渲染 /
   SN0000 渐变反向 / SN0000 旁白贴边。
2. **`0x82` 真实绘制 / `0x1D0` 对表前置**：`REQ-01M63XEX9NXKJAJMZYJDCEX96Y`（共同前置 = 读排版层）。
3. **spec 成篇但 cxx 仍是桩**：`REQ-01M63YS3T54VYCXCBJ6RPCHDAY`（`op_2c7` / `op_341` 空返回、`op_2de` 恒 −1）。
4. **`0x215` 前置未定位**：`REQ-01M63XE3X819G3WAY28Y0XSJKV`（`0x194` 已收口）。
5. **`0x32a` / `0x32` 混合支 / `0x20a` 绑定站点**：`REQ-01M66W5F242YMCSER2ETG7DYCQ` 的剩余项。
6. **spec 证据去 emulator-new 化**：`REQ-01M67147KK2PSR3T4PNPPB653H` —— 本子树有 47 个 done 节点的
   `verify` 指向冻结件；它们**不失效**（仍可跑），但不宜再当唯一证据。

## 门禁（怎么查，不记数字）

`pnpm test` · `ctest --test-dir apps/emulator-cxx/out` · `pnpm typecheck` · `pnpm tools spec validate` ·
`pnpm tools requirements validate` · `git diff --check`。★ 数字会漂，一律以命令实测为准。

## 工作口径

- 本节点**不关闭**：进度以 `pnpm tools requirements plan` 为准；变更历史看 `git log`。
- **spec 完整性判据**：所有影响可观察行为的字段 / 触发 / 转换都写全，独立实现者**只凭 spec** 能写出唯一实现。
- **spec vs emulator**：spec 写领域状态机；emulator 是示范与第二实现证据；分歧按 `amayui-spec` §9.2 归因。
