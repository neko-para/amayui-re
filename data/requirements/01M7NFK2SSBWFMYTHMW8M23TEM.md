# D3DXMatrixDecompose（DirectXMath XMMatrixDecompose；存档推进不再响停）

- id: REQ-01M7NFK2SSBWFMYTHMW8M23TEM
- type: req
- status: done
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- verify: apps/emulator-lift/tests/d3dx9.cpp#d3dx9/matrix-decompose-trs
- tags: [emulator-lift, d3dx9, math]

## 一句话

实现 `D3DXMatrixDecompose`（`d3dx9_43`），数学核用 **DirectXMath `XMMatrixDecompose`**（不手写轮子）；
存档推进不再在 `7FFE0098` 响亮停。

## 为什么

- 用户 2026-11-18：`.tmp/appdata` 第 4 槽（SAVE03）加载后点击推进会撞
  `d3dx9_43!D3DXMatrixDecompose` 未实现；同形 run 三次复现（frames 1704 / 2028 / 2134）。
- 引擎有 5 个调用点（`0x49BA4A` / `0x49C817` / `0x4A9F50` / `0x4AA0F0` / `0x4AF05A`），其中 2 处查 HRESULT
  并打「マトリックスの分割に失敗しました」；**只消费 scale / translation，四元数不读**。
- 选型评估：134 个矩阵与真 `d3dx9_43.dll` 逐例判定一致（切变阈值 0.143 同源、零 scale/m[3][3]/透视列/负 det
  落点全对）；33 ns/call，5 站点 × 1000 节点 ≈ 0.17 ms/帧 ⇒ 性能不构成理由；`glm::decompose` 65/134 判定错。

## 要做

1. `vcpkg.json` + `directxmath`；`math::decomposeD3D(const float[16])` 用 `XMMatrixDecompose`：
   float 路径（转 double 丢 ulp）、`outScale/outTrans` 用 16 B scratch、**失败不写 quat**、成功失败都写 scale+translation。
2. `shims/d3dx9.cpp` 注册 + `pnpm tools lift sync --write`（缺口 5→4，`overrides.json` 整条移出）。
3. 测试 `d3dx9/matrix-decompose-{translation,rotation,scale,trs,zero-scale,shear,mirror,bad-pointer}`（8 条）；
   变异「四元数写共轭」`--match` 实测红。

## 判据

- 8 条子用例全绿；全量 `ctest` 107/107；`pnpm tools lift check` 绿（缺口 5→4）。
- `d3dx9/matrix-decompose-trs` 是 verify 锚（能力存在）。
- E2E **未做**（如实记录）：`--until 0x4A9F50` / `0x4AF05A` 不是派发块入口 ⇒ 永远 time-limit；
  正确入口是紧随的 `0x4A9F55` / `0x4AF05F`。SAVE03 → SC0000 是 2D 脚本动画，不进 RenderScene。

## 未做

- 真 run 到 3D 场景的 E2E：等有正确的 block-entry `--until` 与更长点击配方再做。
- 不改 `--until` 的实现（那是另一条仪器缺口；本单只记录正确用法）。
