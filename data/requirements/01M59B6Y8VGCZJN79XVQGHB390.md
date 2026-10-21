# spec 内容批次：按机制逐批拆可判定子单（组织真源 = core/README）

- id: REQ-01M59B6Y8VGCZJN79XVQGHB390
- type: req
- status: open
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- tags: [engine spec]

## 背景

原先按"章节骨架"拆内容批次；组织轴已改为**机制**（`data/spec/core/README.md` 是组织真源）。
本单保留为**内容批次的伞**：每一批按一个机制拆可判定的子单。

## 现状

* 首批已收口：`core spec · 机制地图与首批机制`（`REQ-01M5AJWPM…`，5/5 子单 done）。
  含 `CORE-ISA` 9 条、`CORE-POOL` 19 条（12 新 + 7 既有）、`apps/emulator-new` 骨架与守卫。
* 本单**不再**按旧章节名拆：`repo:data/spec/chapters/README.md` 的 6 章只是当前落点，不是分区。

## 判据

* 每个机制拆一张可判定子单：先写闭集与要回答的问题，再写条目与 spec↔EA 映射。
* 每张子单收口时能回答：处理了哪些机器行为、哪些仍未处理（残差带 next）。
* 优先补已有 accepted 证据的机制；没有证据的先做取证单。

## 非目标

* 不引入"整引擎覆盖率"目标；每个机制只对其闭集负责。
* 不把 `docs/` 的长调查复制进 spec；需要时只留指针。
