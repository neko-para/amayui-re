# UI 图片文案更新（改配方里的文案，验收 = 人工确认预期）

- id: REQ-01M42S95MSZGPGKM4NM1BBN2ZC
- type: req
- status: doing
- parent: REQ-01M3TCENZ0BCBBFEEDJNMDR75K

## 范围

UI 图片（AGF）的**文案更新与效果迭代**：改一张已经中文化的界面图上的字（错字 / 措辞 / 术语 / 字号字重），
并把改动落成**可复现的配方变更**。这是"翻译更新"在**图片域**的对应物 —— 本节点目前只有这一支。

与基建的关系：**链路由 `REQ-01M41554NN4ACPMY01BB2CHZM8`（UI 图片烘焙链）承担**，
本节点只承担"拿到配方之后怎么改文案、怎么验收"。

## 判据

* 改一张图的文案 = 只改 `tools/ui-bake/recipes/<块>.json` 的 `text.layers[].text`（及必要的坐标 / 字号）；
* `pnpm tools ui-bake verify <块>` 必须**回 0**（逐像素相同）—— 改文案会让它**合理地变红**，
  所以收口凭据是"**改后的产物与人工确认的预期一致**"，而不是"回到 0"；
* `pnpm tools ui-bake build` 能把这批图**从原始件重新构建**出来（证明资源是构建物，不是旧仓搬来的）。

## 挂靠的口径

* 链路口径 / 生成器 / 合成三口径 / 坑表：见 `tools/ui-bake.md` 与 `pnpm tools ui-bake describe`（**不复述**）；
* 视觉规范（E1…E10、字体、描边、渐变）的真源是 `tools/ui-bake/effects.json` 与各 recipe；
* 本节点**不写**任何"某张图当前是第几版"之类的状态（那是 `corpus/assets/ui-images/versions.json`
  与 `pnpm tools ui-bake verify` 现场回答的事）。
