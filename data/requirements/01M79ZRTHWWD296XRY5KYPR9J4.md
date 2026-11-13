# emulator-lift · sprite 采样自身 RT 的反馈环没有闸（sprite→RT 改造后新可达）

- id: REQ-01M79ZRTHWWD296XRY5KYPR9J4
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
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
