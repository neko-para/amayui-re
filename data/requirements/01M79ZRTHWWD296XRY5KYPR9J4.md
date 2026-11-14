# emulator-lift · sprite 采样自身 RT 的反馈环没有闸（sprite→RT 改造后新可达）

- id: REQ-01M79ZRTHWWD296XRY5KYPR9J4
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/gpu_batch.cpp#d3d9p/sprite-feedback-loop
- done_reason: 闸落地在 sprite 自己的路径上（不经 gpuEligible，所以 draw 路那条闸管不到）：compositeBatch 逐条目解析出 tbase 后、任何 rasterQueue 之前判 tbase == base ⇒ 记新具名位 43 kUnSpriteRtFeedbackLoop（不复用 28 —— 复用会让 draw 路与 sprite 路命中在台账里分不开、两条守卫互成恒真）+ spriteWarn + gpuNoteSpriteNotDrawn('采样纹理是正在渲染的 RT（反馈环）') + 对称 releaseEntry + continue。★ 父级裁决：**逐条目跳过**（不是我原来写的'跳过整批'）—— 同批其余条目照常合成，代价更小，且与既有'跳过而非回退'口径一致。守卫 d3d9p/sprite-feedback-loop（pure/Mock，挂既有 gpu-batch-order）：未排队 + sprite_skipped=1 + 原因含反馈环 + 具名位命中 + RT/后缓冲像素不变，另有反向条款（采另一张纹理 ⇒ 照常入队）。修前红：先只落用例 ⇒ 585 checks / 8 failures（该 sprite 真的进了 GPU 队列、queued 0→1、账上无跳过、unsupported 全 0）。变异（判据退化成 base+1 永不成立）⇒ 8 failures（父级复跑 1/1 红）。CPU 合成路径不存在（REQ-01M7AAXNJY2PACYJHYVRV620Q5 已删），报告写明未改。未加 L1-D3DX9-*：D3D9 对自采样 RT 是未定义行为、没有可引的对外合同 ⇒ 按内部安全闸落 design-notes 的 L3-sprite-01。
- tags: [emulator-lift, d3d9, sprite, rtt]

## 缺口

`REQ-01M79V2TVJH98BKH0C0WQV2FET`（sprite→RT）让 sprite 写进绑定的纹理 RT 之后，出现了一个**新可达**的反馈环：
同一批 sprite **采样自己正在渲染的 RT**（`tbase == base`）。sprite 路径不经 `gpuEligible`，所以 draw 路径那条反馈环闸
（`recordUnsupported(kUnRtFeedbackLoop)`）**管不到它**：GPU 读上传缓存、CPU 读 guest 都可能是**旧像素**，而结果看不出来。

## 判据

- `compositeBatch`（`src/runtime/d3dx9_sprite.cpp`）在合成前对每个采样纹理判 `tbase == base` ⇒ **记账 + 跳过该批/回退到后缓冲**（选一种并写死理由），
  ⛔ 不静默读旧像素。
- CPU 合成同病，按同一判据处理。
- 守卫 `d3d9p/sprite-feedback-loop`（纯段，Mock）：绑 RT 后让 sprite 采同一 RT ⇒ 断言"未排队 / 记账命中"，且后缓冲不被写。
- 变异：删掉该判据 ⇒ 用例红。

## 非目标

- 不改 sprite 的合批/顺序语义；不给 sprite 路径接整套 `gpuEligible`（成本大于收益，只需这一条闸）。
