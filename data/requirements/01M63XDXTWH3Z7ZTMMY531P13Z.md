# spec 成篇 · 配置族 + 滚轮（0x2cc/0x2cd/0x2ce/0x2e6/0x2e7/0x2e8/0x2ea/0x306/0x10d）

- id: REQ-01M63XDXTWH3Z7ZTMMY531P13Z
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- order: 1
- verify: tools/test/spec-gaps.test.mjs#★ 精确划分
- tags: [engine spec]

## 判据（可判定）

`audit.gaps[]` 里不再出现这 9 个 opcode：`0x2cc` `0x2cd` `0x2ce` `0x2e6` `0x2e7` `0x2e8` `0x2ea` `0x306` `0x10d`；
它们各自有 `###` 规则标题、`bindings` 站点、`audit.rules` 条目；`pnpm tools spec validate` 13/13 绿、
`pnpm tools spec-gaps` 的精确划分绿（收口凭据即这一条）。

## 已落（2026-10-29）

- `data/spec/chapters/55-config.md`：`CORE-CONFIG-WHEEL-ADVANCE`（0x2cc/0x2cd）·
  `CORE-CONFIG-DISPLAY-CACHE`（0x2ce）· `CORE-CONFIG-AUTO-PITCH`（0x2e6/0x2e7）·
  `CORE-CONFIG-AUTO-OPTION`（0x2e8/0x2ea）· `CORE-CONFIG-EFFECT-SKIP`（0x306）。
- `data/spec/chapters/60-ops-input.md`：`CORE-INPUT-WHEEL-READ`（0x10d）。
- 顺带订正：`CORE-OPS-FORWARD-DISPLAY-MODE` 的"归一后写入"措辞 + 残差 `RES-OPS-2CF-RANGE` 收窄；
  `CORE-CONFIG-STORE` 的"读取器分层"改成"启动配置与脚本读指令走同一个读取器"；
  `CORE-OPS-FORWARD-NAMED-KEY` 关于 `system:EffectSkipOnClick` 的存在性来源（不是内建默认）。

## 取证

- 两轮独立 `.lst` 复核（只读子任务）：0x2e6/0x2e7 的档号分支与"非法档号不读第 2 操作数"、
  0x10d 的"先清零后写落点"、0x2ce 读缓存而非配置，都有 EA 站点落进 `bindings.json`。
- 键的存在性来自构造期登记点 `sub_434D00`（`message:AdvanceMesOnWheel` / `AutoMessagePitch0/1` /
  `AutoMessageOption` / `display:ScreenMode` 初值 0）；`system:EffectSkipOnClick` **不在**其中。

## 独立复审（规则面，只读 data/spec）

共 17 条，全部处置，无回退：

- **会写出第二种实现的三条**：0x2e7 缺键语义 → 正文补"只覆盖已存在键"；0x10d 归因
  （"宿主累加"→"引擎窗口过程累加"）→ 正文改 + 残差 `RES-INPUT-WHEEL-MESSAGE`；
  缓存装载与强制全屏的先后 → 残差 `RES-CONFIG-DISPLAY-CACHE-LOAD-ORDER`。
- **与伴随数据矛盾**：`system:EffectSkipOnClick` 不在发行默认表 / 内建默认 → 两条正文改口径 +
  残差 `RES-CONFIG-EFFECT-SKIP-EXISTENCE`。
- **漏残差**：四个键的初值（已在正文写明内建默认 0）、脚本读命名键缺键是否弹框（在「配置库」一次写清）。
- **悬空引用 / 术语 / 绝对说法**：8 处正文修正（"见下一条"→指向「配置」；"画面模式"→"显示模式"；
  "推进宽度"→"推进格数"；"不改其它引擎数据"加"访问原语缓存"例外 …）。
- **绑定卫生**：6 条补 `storage: []` 与 handler 起点站点；`0x406c5b` 的描述与场景族对齐。

## 已知欠账（不影响收口）

- 6 条规则的证据共用 `emulator-core-selftest`（draft 相称）；升 reviewed 前要有具名用例或 KN。
