# 0xae 布局 1 的修复路径

- id: REQ-01M68PPN3PVQSZ5NQJJ6Z55JTZ
- type: req
- status: dropped
- parent: REQ-01M674XHSX2HSFWSKDWZV2C63D
- dropped_reason: 残差清单式节点：audit.json 的残差已带 what/next，需求树不重复登记；无 audit 归宿的条目已并入 REQ-01M694VKD97ERC5ZBHDA4AHQV2。
- tags: [spec residual]

## 目标

把 `0xae` 的**布局 1** 修复路径补完（批 2 已把三套布局的选择与应用写成合同，布局 1 的内部没读）。

## 具体项

- 入口：`0x4105C8` 起的块构造 + `sub_40F750` 的 `(1, 0x14)` 支（≈2 函数 / 300 行）。
- 要答：布局 1 的暂存块有哪些字段；位置表的下标语义（与布局 2 / 3 的差别）；"优先族推进 3 / 回退族 0 / 下标 −1 不变"这三条在布局 1 下是否仍成立。
- 对应残差 `RES-OPS-SAVE-VERSION-AE-LAYOUT1`（本轮保留的窄 `unknown`）。
