# 方法对账：A–F 与判据的现状（哪些已定 / 仍未定）

- id: REQ-01M5BKEAHQPN5PE4A2258C5V9J
- type: req
- status: dropped
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- order: 50
- dropped_reason: 这是 core spec 首批落地后的方法对账快照；其中的缺口（falsifiers / selector 指标 / per-mechanism 报告）已转由 spec 治理批次（REQ-01M5AKDR… / REQ-01M5AM4M5… / REQ-01M5D1G31…）与第三轮复核跟踪，本快照不再单独维护。
- tags: [engine spec method]

## 目的

方法单（`REQ-01M5851Z…`）正文的 A–F 是**开单时**的未定项；core spec 首批落地后，其中若干已经定案。
本单只做**对账**：每项现在算什么状态、真源在哪、还缺什么。⛔ 不改方法单正文（它保留原始讨论）。

## A 计量与效果模型

* **已定口径**（技能 `amayui-spec` §5）：指令是枚举底数、不是正确性分母；mark 分局部 / 绝对；守恒按义务去重；函数 ↔ 机制多对多。
* **仍未落**：账本与去重计算的软件实现；当前 `audit` 只有 per-rule 的 `residual` / `covers`。

## B 范围标记

* **已定**：局部 / 绝对口径、残差四类（unknown / unhandled / omitted / discarded）、替换的 `preserves/discards/chosen` + 范围。
* **已落**：规则字段；**未落**：条目级 marks 的独立数据（现在靠 `covers` 选择器）。

## C 边界普查

* **未做**：平台态 / 并发 / 时序的候选普查。`core/README.md` §1 已定"core 外不实现、不记录副作用"，所以它不再阻塞 core，但仍是方法单的未定项。

## D 条目与落点

* **已定并已落**：机制地图 + 条目形态 + spec↔EA 映射 + 草稿 / 合并约定（`data/spec/core/README.md`）。

## E 核验能力

* **部分**：`covers` 选择器有了，但**多数新规则的 covers 为空**（28 条里 22 条）；selector 的命中 / 错配指标、偏序检查都未实现。

## F 报告与试点

* **部分**：`spec report` 有；但 `pilot` 仍是单数、只有 58 块；缺 per-mechanism / per-slice 报告。

## 判据对账（方法单 §判据）

1. 正向 / 反向可查：**满足**（`pnpm tools spec show` / `at`）。
2. 未登记 = 0：**满足，但只对 58 块的试点域**。
3. 局部处理不吸走剩余、越界红：**部分**（变异条目在，但没有 per-rule 的重叠统计）。
4. 检查器 = 可执行投影：**部分**（现有守卫是结构断言；selector 多选 / 漏选未实现）。
5. 样例有可证伪预言：**缺** —— 28 条里 **21 条 `falsifiers` 为空**（全部是新合入的 ISA / POOL 规则）。
6. 继续 / 缩小 / 替换的策略：**满足**（首批后选择"继续扩大"）。

## 下一步（按缺口排序）

1. 给 21 条新规则各写一条**可证伪预言**（判据 5 的硬缺口）。
2. 实现 selector 指标（命中数 / 错配率）与按义务去重的守恒计算。
3. per-mechanism 报告 + 试点去单数化（`pilot` → `slices`）。

## 怎么查

`pnpm tools spec report` · `pnpm tools spec describe` · `data/spec/core/README.md` · 技能 `amayui-spec`
