# spec 审稿清单（2026-10，只读 spec 的独立审稿）：14 类 60+ 条逐条溯源与修订

- id: REQ-01M5D2WF5ST4AZH3W6CBTZEA9W
- type: req
- status: open
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- tags: [engine spec, audit]

## 一句话

一个**只读 `data/spec/`、禁读实现**的审稿 agent 交了一份清单（成品在 `docs/02-engine/spec-audit-2026-10.md`，14 类 / 60+ 条）。本单是**逐条溯源与修订**的执行单：每条要么按汇编（EA）改成合同，要么显式降级/删除。

## 已确认的硬矛盾（优先）

| # | 位置 | 现象 | 处置方向 |
|---|---|---|---|
| 1 | `00-overview` 覆盖声明 | 把 config/ops/vm/容器句柄列为"不覆盖"，而它们已成篇 | 纯文本订正（覆盖 = manifest） |
| 2 | `10-format` 核验状态 | 六行都写"已核验"却都带"待核验什么"；与 `format/verification.md` 的 partial/unverified 冲突 | 逐行按 verification 表对齐 |
| 3 | `55-config` 值域 | "只有两类（整数/字符串）" vs "三类（含字符串列表）" | 以 `config-keys.json` 的 kind 为准 |
| 4 | `40-pools` type 8 | 支持表写 "0–13" 含 8，同表又列 8 为"不支持" | 按五张跳转表 case/default 取证后分段写 |
| 5 | `40-pools` 读浮宽度 | 表写"32 位浮点数"，规则写"返回 64 位" | 取证原语返回类型/调用约定 |
| 6 | 15-frame vs 60-ops | 容量 40/边界 39/错误文本两处写法不一（已部分修） | 统一到 15-frame 的上限合同，60-ops 只引用 |
| 7 | 15-frame vs 60-ops | `0x3` 没写父帧长度槽、`0x2` 却靠它推进 | 取证 `0x3` 是否写父帧长度槽 |
| 8 | 15-frame | "帧号"已定义为数组下标（已修），但**释放条件**（父+1==当前才释放）未写 | 与 `0x6`/存档一起取证 |
| 9 | `50-engine-state` | 章首说"引擎级不随帧变"，规则却写帧记录字段 | 该字段属帧（10-format 已定），删掉矛盾表述 |
| 10 | `70-vm` "一次迭代只做一件事" | 与"计时窗每帧推进"冲突；"帧"与"迭代"混用 | 取证主循环里窗推进的位置与次数 |

## 术语与引用缺口（各一条）

- **术语表缺口**：场景(28) / 帧记录(19) / 代码区(15) / 词表(15) / 纹理槽(8) / 对象槽(6) / 统一文件 id(3) / 未定义行为(3) / 释放守卫(2) / 随机余量(2) / memflip(1) —— 均无定义处。处置：在 `00-overview` 建**术语表**（定义或替换为已定义词）。
- **断裂引用**：「等待」(70-vm) / 「取表下标」(55-config) / 「容量与越界」(40-pools) / 「操作数记录数恒等式」(50) / "见残差"(10 处)；`chapters/README.md` 漏了 15-frame；`audit.planned` 仍写 timing="尚未处理"（已成篇）；`core/README` 指向不存在的 `chapters/core/<机制>/`。

## 判据

- 每条处置要么给 **EA 锚**（改合同），要么在 `audit` 里显式降级（残差/缺口），⛔ 不许"沉默删掉"；
- 每批改完：`pnpm tools spec validate` 13/13 + `pnpm test` 绿；
- 修完把 `docs/02-engine/spec-audit-2026-10.md` 的条目逐条标状态（`fixed` / `deferred` / `not-a-bug`）。

## 不做

- ⛔ 不引 age-format / `layout.mts` 作为依据（数字回 `.lst` 溯源）；
- ⛔ 不改 `audit` 的宽容度来让清单"消失"。
