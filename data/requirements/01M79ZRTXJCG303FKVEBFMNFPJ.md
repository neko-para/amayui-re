# emulator-lift · Clear / unlockRect 仍直读 renderTargetArena：收口到 colorTargetOfState（唯一判据）

- id: REQ-01M79ZRTXJCG303FKVEBFMNFPJ
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- done_reason: Clear 与 unlockRect 都收口到 colorTargetOfState 这个唯一判据；收口后 renderTargetArena 全仓零调用点 ⇒ 已从 .h/.cpp 删除（死代码不留旁路）。行为不变的论证：colorTargetOfState 先零值初始化、再用同样的 magic/committed 校验读同一个 kPlColorArena，失败时 arena==0 与旧 renderTargetArena/dev.pipeline==0?0 一致；textureInfoOfState 只读无副作用。按单里'不要求新用例'的口径没有加断言 —— 证据 = 既有 ctest 85/85 全绿（父级复跑）。变异把 arena 恒置 0 ⇒ 守卫生红（父级复跑 1/1 红）。已知小遗留：tests/gpu_batch.cpp:1126 的注释仍提到已删的 renderTargetArena（不在允许改动面，未动；rg 确认无代码引用）。
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
