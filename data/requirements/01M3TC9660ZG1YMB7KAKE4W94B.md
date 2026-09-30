# 仓库迁移与重建

- id: REQ-01M3TC9660ZG1YMB7KAKE4W94B
- type: req
- status: doing
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- order: 10
- tags: [migration]

## 范围
把旧仓（`E:\Games\Eushully\天結`，只读）里**该带的东西**按新结构搬进本仓：只读语料入位、格式层与台账层重建、
模拟器与探针按新结构重写、知识层过准入规则后再进来。

## 非目标
不搬旧仓的知识资产（`analysis/` `tickets/` `docs-new/`）—— 它们走 K 线（清理 / 校验 / 重分类）。
不做"整仓搬迁"：每一批都必须能独立验收。

## 沿革
本节点**吸收了原 `PLAN.md` 的批次表**（M0…M8 / K1…K3 都是它的子节点），那份手写表已删除 ——
进度从此由这棵树回答（`pnpm tools requirements plan`）。
