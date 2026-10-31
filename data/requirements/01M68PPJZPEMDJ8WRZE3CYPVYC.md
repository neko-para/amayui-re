# 渲染步内部：38 目标 / 首帧 / 双渲染措辞

- id: REQ-01M68PPJZPEMDJ8WRZE3CYPVYC
- type: req
- status: open
- parent: REQ-01M674XHSX2HSFWSKDWZV2C63D
- tags: [spec residual]

## 目标

收敛渲染步内部在批 2 暴露的缺口。

## 具体项

1. **渲染步入口的内部**（`0x4B4040`，399 行 / 38 目标）：帧记录、"第一帧"判定、可绘制性谓词的早退（`0x4B4077 jz 0x4B444B`）。对应残差 `RES-VM-RENDERSTEP-FIRSTFRAME`（S1）。
2. **`RES-RENDER-STEP-DOUBLE`（render 章）与新合同对齐**：`CORE-VM-LOOP` 已改为"同迭代两次渲染步是允许的、渲染步内部没有去重闸"；render 章的该残差还写着"未取证"，要么收窄成"帧记录/首帧"、要么删。
3. **`RES-RENDER-STEP-DRAW-SWITCH`**：本轮只订正了 `what`（`0x200` 写操作数、`set:CreateObject` 只作前置位图）；若 38 目标读清后能定名，再收紧。
