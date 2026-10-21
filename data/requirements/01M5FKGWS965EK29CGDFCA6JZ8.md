# FMT-SAVE 内容批次：存档写入 / 恢复归属

- id: REQ-01M5FKGWS965EK29CGDFCA6JZ8
- type: req
- status: open
- parent: REQ-01M59B6Y8VGCZJN79XVQGHB390
- tags: [spec, save]

## 一句话

机制表里的 `FMT-SAVE` 没有需求子单；存档的"写哪些区域 / 恢复时重建还是保留"散在台账与需求里，没有收成合同。

## 闭集（先回答这些）

* 存档样本（`corpus/fixtures/`）：有哪几类、各自的字段与容器。
* 写入原语：写哪些区域、用什么编码 / 压缩。
* 恢复：哪些状态**重建**（从脚本装载）、哪些**保留**（从存档读回）、哪些两者都有。
* 归属：存档格式是引擎合同还是游戏数据（判据见 `docs/00-origin/project-intent.md`）。

## 判据

* 至少 1 条"存档不是状态快照，而是引擎选择持久化的那部分状态"的合同（带样本锚）；
* 恢复侧至少 1 条"重建 vs 保留"的合同或残差（带 next）；
* `pnpm tools spec validate` 13/13；`pnpm tools fixtures list` 能列出现有样本。

## 怎么查

`pnpm tools fixtures list` · `pnpm tools fixtures describe` · `pnpm tools spec report`。
