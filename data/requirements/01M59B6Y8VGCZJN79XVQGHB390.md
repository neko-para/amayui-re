# spec 内容批次：按章节骨架逐章拆可判定子单（优先已有 accepted 证据的章）

- id: REQ-01M59B6Y8VGCZJN79XVQGHB390
- type: req
- status: open
- parent: REQ-01M5851Z51V1MTSWH414M7WN5Q
- tags: [engine spec]

## 背景

`data/spec/engine.json` 已立**章节骨架**（哪些章、每章到哪一步，一律 `pnpm tools spec chapters` 现查），其中只有 pools / operands / values 三章有成篇合同；
其余章（execution / containers / engine-state / objects / input / presentation / files / save / config / timing）只有问题与残差。

## 判据

* 每一章拆一张**可判定**子单：先写该章要回答的问题与"候选义务"从哪来，再写合同与守卫。
* 每张子单收口时必须能回答：这一章处理了哪些机器行为、哪些仍未处理（残差带下一步问题）。
* 不要求一次补完：优先补**已有 accepted 证据**能支撑的章（execution / config / objects），没有证据的章先做取证单。

## 非目标

* 不引入"整引擎覆盖率"目标；每章只对其声明的候选域负责。
* 不把 `docs/` 里的长调查复制进 spec；需要时只留指针。
