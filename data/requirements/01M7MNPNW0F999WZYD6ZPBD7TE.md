# S0 · 取证：effect 资源 200/201/203 的格式与取法 · ID3DXEffect 调用面 · GPU 后端缺口清单

- id: REQ-01M7MNPNW0F999WZYD6ZPBD7TE
- type: req
- status: open
- parent: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- tags: [emulator-lift, d3d9, spike]

## 一句话

在实现 FX/shader 之前，把三件事用读数钉死：① effect 资源 **200/201/203** 的格式与取法；② `ID3DXEffect` 到底被调了哪些方法
（从 callsite `0x004B0798` 之后与 vtable 槽位读）；③ 现有 `--raster gpu` 后端（SDL_GPU + MSL/DXBC）**缺在哪几处**才能吃 shader。

## 为什么需要它

caps 改成 `0xFFFF0300` 后引擎立刻调 `D3DXCreateEffectFromResourceA`（资源 201 = 效果等级 2，`Engine+0xB590` 只建一次），
但"这个资源是什么格式、里面有没有内嵌 shader、`ID3DXEffect` 被调了什么"目前**全是待核**。没有这三条，S1/S2 的设计只能猜。

## 判据（红/绿怎么判）

- 资源 201 有**字节级读数**（大小 / 前 32 字节 hex / 是否 D3DX9 effect 二进制 / 内嵌 shader 段数与版本）。
- `ID3DXEffect` 的方法集有**来源**（EA 或 vtable 槽位；读不到就明写"读不到"，⛔ 不许猜）。
- GPU 后端的缺口清单每条指到 `函数:行`，并写清"这条现在走 CPU 还是 GPU"。

## 未做

不含任何实现（创建 effect / shader 翻译 / caps 改动都在 S1/S2）。
