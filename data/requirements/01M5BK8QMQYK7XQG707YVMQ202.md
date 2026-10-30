# 当前实现总览与交接（会话 / 设备继续）

- id: REQ-01M5BK8QMQYK7XQG707YVMQ202
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 0
- tags: [handoff, engine spec]

## 一句话

本节点**保持 open**，作为「当前实现总览与交接」持续更新，不按收口处理（用户口径 2026-10-29）。
2026-10-30：**GameStart → ゲーム開始 → ADV 界面（夕阳背景 + 旁白正文）已跑通**，该路径上缺的 opcode 全部补齐。

## 交付物在哪

- **规范**：`repo:data/spec`（真源）· 工具 `pnpm tools spec validate|report|describe`。
- **C++ 实现线**：`repo:apps/emulator-cxx`（preset `windows-x64`；`out/` 是 binaryDir）；参考实现 `repo:apps/emulator-new`。
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

## 这一轮（2026-10-30）收口了什么

- **补 30 条 opcode**，每条都有 spec 落点 + `bindings` 站点/证据 + `audit` 残差（并同步移出 `GAP-OPS-*`）：
  `0x1bc 0x32a 0x32d 0x1b1 0x238 0x215 0x242 0x219 0x220 0x224 0x7a 0x1cb 0x256 0x2e9 0x214`
  `0x193 0x20a 0x243 0x2f7 0x304 0x305 0x32 0x72 0x10a`。
- **两处模型纠错**：① 门字第 9 位（BGM 淡变）原先没有服务分支 ⇒ 补最小推进；
  ② `0x1fb` 才是「场景对象记录」的建记录者（置 bit0 + 写整数标签），`0x215`/`0x220` 与它同表。
- **正文渲染**：逐字符绘制条目需要「绘制键基数」，ADV 的消息层脚本没给 ⇒ 加宿主兜底（残差 `ADV-TEXT-LAYER-DRAW-KEY`）。

## 当前需要处理（2026-10-30 交接，按优先级）

1. 🐞 **序章「夕阳 ↔ 光柱塔」之间的中心旁白不出**，点击直接跳到下一幕 —— `REQ-01M65A72Y5ZHJPXMGPMHB0KH92`。
2. 🐞 **章节标题卡位置歪**（真机在左侧中心、现在画在下方中央）+ 其后的**人物轮廓整体渲染错** —— `REQ-01M65A7BCESQG4C2B59V1HT3SB`。
3. **文本层绘制键基数是宿主兜底**（残差 `ADV-TEXT-LAYER-DRAW-KEY`）——⛔ 不是引擎语义，取证后要撤。
4. **`0x305` 的「自旋到该层揭示完」** 用「揭示全部格」实现 ⇒ **打字机逐字效果丢失**（已知偏离）。
5. **ADV 之后**：`CALCREVISE.BIN` 停在 `0x132`；`SC0000` 还缺 `0x26 0x28 0xc3 0x140 0x230 0x233 0x234 0x235`。
6. **`0x82` 的真实绘制 / `0x1D0` 的对表前置**（`REQ-01M63XEX9NXKJAJMZYJDCEX96Y`）：共同前置是读排版层。
7. **spec 已成篇、实现是桩 / 归属错**（`REQ-01M63YS3T54VYCXCBJ6RPCHDAY`）：`op_2c7` / `op_2de` / L2D 四条 /
   `op_2ee` / `op_25b`，以及 `0xb7`/`0xb8` 被当成 BGM 播放/停止。

## 当前门禁（实测 2026-10-30，macOS 干净树）

`pnpm test` **1113 通过 / 0 失败** · `ctest` **37/37** · `pnpm typecheck` 六工程通过 · `pnpm tools spec validate` 13/13 ·
`pnpm tools requirements validate` **6/6** · `pnpm tools routes validate` 12/12 · `pnpm tools corpus validate` 8 通过 / 0 失败 / 1 警告。

## 2026-10-30 独立复审（`323b5a5`）与清理

- 全量清单：`repo:docs/02-engine/review-323b5a5-2026-10-30.md`；审计记录：`docs/02-engine/spec-audit-2026-10.md` §九十五。
- 已修（均有 CTest / mutation 守卫）：`0x305` 武装位（`0x6e` / `0x196` 置 bit16）；`0x214` 读序 / 标脏 / 交换；
  `0x238` 写消息等待格；`0x219` / `0x220` / `0x242` 两半同步；`0x217` / `0x220` 读序；文本层渲染请求与 fallback base；
  `0x7a` 默认槽；`0x1ca` / `0x1cb` 同一存储；`0x258` 读序；`0x90` arm / 清表 / 兜底；`0x229` GPU 路径；
  14 个零测试 opcode 全部补 CTest；站点证据新增“RULES 必须覆盖 rule”守卫；干净树两门禁转绿。
- 未修（已登记在 review §7）：`0x2e9` 自动消息偏移的消费者、`0x10a` 换算、`0x1bc` 声道 / 后端、`0x32d` / `0x10a` 请求消费者、
  选项消费侧、`0x93` / `0x94` 命中格身份、`0x305` 自旋 / bit27 / 已读、`0x229` 命令档跳过、`0x224` 容器载荷。
- 本节点仍不关闭；未修项按 review §7 继续。

## 工作口径

- 本节点**不关闭**：进度以 `pnpm tools requirements plan` 为准；变更历史看 `git log`。
- **spec 完整性判据**：所有影响可观察行为的字段 / 触发 / 转换都写全，独立实现者**只凭 spec** 能写出唯一实现。
- **spec vs emulator**：spec 写领域状态机；emulator 是示范与第二实现证据；分歧按 `amayui-spec` §9.2 归因。
