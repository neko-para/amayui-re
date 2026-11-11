# D3D9 剩余缺口索引（7 条待拆单；含已否证清单）

- id: REQ-01M7485DCS1RBQSDDH6F68HS1J
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift d3d9 index]

## 一句话
本单只做**路由**：剩余未拆单的 D3D9 缺口逐条列在这里。**每行必须先单独拆单**（本仓"一张单只收一类失效"）才动代码；
拆单后把该行搬进新单、并从本表删除，保持本表**只列"还没拆的"**。

## 表
| # | 缺口 | 位置（符号） | 判据建议 | 触发面 |
|---|---|---|---|---|
| 1 | sprite 合成目标恒为后缓冲（不看 `kPlColorSurface` / viewport / scissor） | `d3dx9_sprite.cpp::compositeBatch` | RT 存在时 sprite 必须写进 RT，后缓冲不变 | 今天引擎无 RTT（`SetRenderTarget` 恒 NULL）⇒ 潜在 |
| 2 | `GetDeviceCaps` 与实现脱钩（`LineCaps` 等；`Caps/Caps2` 恒 0） | `d3d9.cpp::fillCaps` | caps 位由"同一份实现集"生成 | 引擎只读 `MaxTextureWidth/Height` |
| 3 | `ValidateDevice` 恒 `S_OK,1` | `d3d9_pipeline.cpp` 的 `validateDevice` | 复用 `drawPrecheck` 的判据 | 无 `.lst` 站点 |
| 4 | `GetBackBuffer` 每次新建对象 | `shims/d3d9.cpp` 的 `shimGetBackBuffer` | 连续两次同 EA + refcount=2 | 影响"同一目标"判据 ⇒ 每次 `SetTarget` 走屏障 |
| 5 | `MinIndex` / 负 `BaseVertexIndex` 被丢 | `d3d9_pipeline.cpp` 的 `drawIndexedPrimitive` | 合法区间 `[min, min+n)`；`base` 按 int32 | 两站点都是 `0/0` |
| 6 | `SetRenderState` 的 `default:` 静默接受一切 | 同上 `setRenderState` | 未实现状态必须 `recordUnsupported` | sRGB 写等 |
| 7 | GPU 侧 FF 覆盖率可疑（`fragments` 实为**顶点数**） | `d3d9_gpu.cpp` | 逐批"应有像素 vs 实得像素"对账 | 92 万像素 vs 124 批 |

## 已被否证、别再查（负结果索引）
纹理副本陈旧 · guest 覆盖镜像擦批 · 深度清值 · 视口二次映射 · scissor · 没进 render pass · mip 回归 · `LIFT_NO_MIP` 对 GPU 无效 ·
FF 管线本身 · 顶点链 · 三张 2048² 图集的内容（level 0 哈希 == guest）· mip 选级公式（实测返回 2）· `ID3DXSprite::Begin` 无 ALPHABLEND（1013/1013 带 `0x10`）。
★ 详细证据在各单正文与 `.tmp/d3d9/a*.md`。

## 未做
- 不在这里写实现细节。
- 被裁"边界外"的东西仍写进对应单的 `## 未做`，不塞回本表。
