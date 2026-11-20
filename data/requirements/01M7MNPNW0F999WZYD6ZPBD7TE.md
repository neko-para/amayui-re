# S0 · 取证：effect 资源 200/201/203 的格式与取法 · ID3DXEffect 调用面 · GPU 后端缺口清单

- id: REQ-01M7MNPNW0F999WZYD6ZPBD7TE
- type: req
- status: done
- parent: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- done_reason: S0 四条判据的产出已落 docs/03-emulator/d3dx-effect.md（4 份 RT_RCDATA 的大小/sha256/内容/目标、d3dx9-effect/resources-pinned、ID3DXEffect 的 9 槽调用面、后端“整批拒绝”位清单）与制品契约 docs/03-emulator/caps.md；本单不含实现（实现归 P1/P2/P3/A/B，均已 done）。
- tags: [emulator-lift, d3d9, spike]

## 一句话

在实现 FX/shader 之前，把三件事用读数钉死：① **有限集合**——引擎会提交的 shader / effect 有哪些（调用点 + 字节码 + 哈希 + 版本 + 内嵌段数）；
② `ID3DXEffect` 到底被调了哪些方法（callsite `0x004B0798` 之后 + vtable 槽位）；③ 现有 `--raster gpu` 后端缺在哪几处才能吃 shader。

## 为什么需要它

构建期预转写（`REQ-01M7MNYPDXM2NEQR77DXYZM3CJ`）的前提是"集合有限且可数" —— 这个数必须点清、每个字节码要能取到并哈希，
否则"运行时查表"就没有表可查。caps 改成 `0xFFFF0300` 后引擎立刻调 `D3DXCreateEffectFromResourceA`（资源 201 = 等级 2），
但资源格式、内嵌 shader 段数、`ID3DXEffect` 方法集目前**全是待核**。

## 判据（红/绿怎么判）

- 集合有**清单**：每个 effect / shader 的调用点 EA + 资源号/来源 + 大小 + sha256 + shader 版本（vs/ps 几） + 内嵌段数。
- 至少一份字节码的**前 32 字节 hex** 与格式判定依据（D3DX9 effect 二进制 / 裸 shader 字节码 / 别的）。
- `ID3DXEffect` 的方法集有来源（EA 或 vtable 槽位；读不到就明写"读不到"）。
- 后端缺口清单每条指到 `函数:行`，并写清"这条现在走 CPU 还是 GPU"。

## 未做

不含任何实现（创建 effect / shader 转写 / caps 改动都在 S1/S2）。
