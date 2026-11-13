# emulator-lift · Clear / unlockRect 仍直读 renderTargetArena：收口到 colorTargetOfState（唯一判据）

- id: REQ-01M79ZRTXJCG303FKVEBFMNFPJ
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, d3d9, refactor]

## 缺口

`colorTargetOfState()`（`apps/emulator-lift/src/runtime/d3d9_pipeline.cpp`）已是非 sprite 侧"当前颜色目标"的唯一判据
（`prepareContext` / sprite CPU 合成 / GPU `rasterPrepare` / `rasterSyncToGuest` 四面同源）。但还有两处**直读** `renderTargetArena`
并各自写 `!=0 ? … : dev.arena`：`src/runtime/d3d9.cpp` 的 `Clear`（约 :610）与 `unlockRect`（约 :1217）。

今天两处 key 相同、不会分歧；但这是"两份判据、一份规则"的形状 —— 下一次改目标语义（例如深度纹理 RT、多 RT）时，
先改的那份会被另一份静默打回。

## 判据

- 两处改成调用同一个 `colorTargetOfState()`（或它的窄接口），⛔ 不再各写 `?: dev.arena`。
- 守卫：**不需要新用例** —— 现有 `gpu-batch/order` 的 `d3d9p/render-to-target` 与 `save-graph` 覆盖行为；本单只做收口 + `rg` 断言（可选：一条"源码里只允许出现一处 `?: dev.arena`"的机械检查，若仓库已有同类先例才做）。
- 变异：把 `colorTargetOfState` 改成恒返回 `dev.arena` ⇒ 现有 `d3d9p/sprite-render-target` / `save-graph` 应红（证明收口后仍被现有守卫覆盖）。

## 非目标

- 不改语义；不碰 sprite / depth 的既有测试。
