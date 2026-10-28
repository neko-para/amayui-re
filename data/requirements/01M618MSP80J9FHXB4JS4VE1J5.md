# ADV 文本层剩余残差：折行 / typewriter / 振り仮名 / 0x71 中间状态

- id: REQ-01M618MSP80J9FHXB4JS4VE1J5
- type: req
- status: open
- parent: REQ-01M5Z5D786WF65YWHFK7K8Z7Q8

## 已落地（2026-10-28）

- **折行门**：`set:AutoLineFeed`（引擎读进 `Font+0x57C`，site `0x417b6e` / `0x46c132`）。
  `ADV-TEXT-APPEND` 已写：非 0 且超右界（`sub_475CF0` 位 0，`x > 记录+0x24`）⇒ 折行；0 ⇒ 不折行。
- **实现**：逐字符绘制时先量度、超右界则回 `记录+0x1C` 原点、`y += 字号 + 行間隙`；自检用窄画布 +
  `set:AutoLineFeed=1/0` 两半断言（`core_selftest` 的 `check_text_layer_surface`）。
- **仍不规定**：禁则判定（`sub_45BCD0`）/ 整行重排 / 对齐 / 字距 / 连字 / 振り仮名按行比例切分（`sub_465A20`）。

## 范围（剩余）

- **逐字符条目的创建时机**（`RES-ADV-TEXT-LAYER-TIMING`）：引擎在推进器（`sub_409400` / `sub_45BE20`）里建，
  本实现在追加时建；一次性静态文本观感相同，逐帧中间态 / 重画次序可能不同。
- **`0x71` 的中间状态**（`RES-ADV-TEXT-LAYER-STATE`）：布局向量 / 定位记录 / `+0x84/+0x128/+0x88` 未建。
- **文本门 / 计时 / 状态位**（`RES-ADV-TEXT-APPEND-SLOT-BITS`、`-GATE`）：本版只规定画布与游标。
- **非普通行距模式**（`RES-ADV-TEXT-APPEND-LINE-MODES`）：模式格非 0 的分支未取证。
- **字形度量**（`RES-ADV-TEXT-APPEND-METRICS`）：平台字体 / 基线 / 行間隙的像素值。
- **振り仮名对齐**：当前左对齐 + 独立条目；引擎 `sub_465A20` 的横向公式仍未替换。

## 下一步（按取证成本）

1. typewriter：用逐帧截帧对照长文本，确认引擎是否逐帧只画前 N 个字符；
2. 振り仮名：读 `sub_465A20` 的横向公式，替换当前的左对齐近似；
3. `0x71` 中间状态：用第二次 `0x71` 的脚本观察重画是否重复条目。

## 判据

每单元收口时挂 `verify:`（CTest / 自检 / JS pure 用例），并让 spec 复审只读 `data/spec` 复核。
