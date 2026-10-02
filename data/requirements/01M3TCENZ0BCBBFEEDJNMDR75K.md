# 翻译更新

- id: REQ-01M3TCENZ0BCBBFEEDJNMDR75K
- type: req
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 40
- tags: [translation]

## 范围
**纯业务**：文案的更新 / 润色 / 术语统一（旧仓那张"已翻译脚本的评估与更新"流程单）。

## 前置（基建，已定案，不在本节点内实施）
数据模型已拍板为 **patch 叠加层**：入库的只有 patch，`data` 与 `src` 都是实时视图。
实现落在「翻译环境重建」下的节点 `REQ-01M3XJXVYBRFW1VT7RD8SNRXKV`（设计见 `docs/01-translation/patch-design.md`）。
⇒ 本节点**不承担** patch 工具与视图生成，只承担"拿到视图之后怎么改文案"。

## 判据
（待写：文案变更能落成 patch、能重建视图、能被复核。）
