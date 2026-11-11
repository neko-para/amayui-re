# emulator-lift · D3D9 状态块 CreateStateBlock/Capture/Apply 是纯 stub：Live2D 每帧的状态泄漏到后续 2D 绘制

- id: REQ-01M7372BPVGBD247KAGHB53YK7
- type: bug
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/d3d9.cpp#d3d9/stateblock-capture-apply
- repro: apps/emulator-lift/src/shims/d3d9.cpp#shimCreateStateBlock
- severity: S2
- tags: [emulator-lift d3d9 state-block]

## 一句话
引擎每帧 Live2D 都走 `CreateStateBlock(D3DSBT_ALL) → draw → Apply → Release`（`0x47843A-0x47847B`），
而本线的 `CreateStateBlock` / `Capture` / `Apply` 是**纯形状 stub**（只校验参数、直接 `S_OK`）⇒
draw 里改的 FVF / 纹理绑定 / 渲染状态 / 变换**不还原**，泄漏到后续 2D 绘制。

## 已核（EA）
- `0x47843A-0x47847B`：`[+0xEC](1, &blk)` → Live2D draw → `blk[+0x14]`(Apply) → `blk[+8]`(Release)。
- D3D9 语义：状态块在**创建那一刻**就捕获（CreateStateBlock 文档 "a state block contains state data … when the object is created"）。
- 现状：`shims/d3d9.cpp:1044-1081`（Create 只造对象 + 校验 `type∈1..3`）· `:1007-1016`（Capture 直接 `S_OK`）·
  `:1018-1027`（Apply 直接 `S_OK`）· 文件头 `:972-975` 自注"只做形状"。

## 修法（最小）
在状态块对象里挂一块**宿主私有快照**（本线 `kPl*` 区里按 type 掩码选出的字段）：
Create = 捕获 · Capture = 重捕 · Apply = 写回，并重建 Apply 后仍被引用的绑定（纹理 / 流 / 索引 / 顶点声明）。
`D3DSBT_ALL / PIXELSTATE / VERTEXSTATE` 映射到掩码；对象销毁时释放它持有的引用。

## 守卫（修前必须红）
`tests/d3d9.cpp#d3d9/stateblock-capture-apply`：建块 → 改 `SetRenderState` / `SetTexture` / `SetFVF` →
`Apply` → `GetRenderState` / `GetTexture` / `GetFVF` 必须回到建块时的值；把 Apply 退回空转 ⇒ 红。

## 未做
- `GetDescription`（`+0x18`）不在本单：槽哨兵与越界读见同批的「参数边界校验」单。
- 状态块的精确字段集合先按本线 `kPl*` 区的实际字段落，不按 SDK 全字段猜（缺的字段另开单）。
