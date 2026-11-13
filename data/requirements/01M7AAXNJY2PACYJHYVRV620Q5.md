# emulator-lift · d3dx9 sprite 的 CPU 合成器是另一条 CPU 像素路径（删 CPU 光栅不会带走它）

- id: REQ-01M7AAXNJY2PACYJHYVRV620Q5
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift, sprite, cpu-retire]

## 缺口

`d3dx9_sprite.cpp`（约 `:932-1000`）有一份 **CPU sprite 合成器**：把批次直接混进 `dev.arena`（后缓冲）。
它不是 `rasterTriangle`，所以 `REQ-01M7A74YG08R65NJBFXF7DEJDK` 删 CPU 光栅时**不会**顺带删掉它；
`tests/d3dx9.cpp` 的像素断言就挂在这条路径上（该文件 0 个 `Draw*`）。

后果：产品里仍有一条"CPU 画像素"的路径（sprite 走 GPU 失败/被拒时）。在 GPU 唯一的口径下，它应当是**记账 + 跳过**，而不是静默 CPU 合成。

## 判据

- `d3dx9_sprite.cpp` 的 CPU 合成分支删除或改成"记账 + 跳过"（⛔ 不静默画）；GPU sprite 批仍是唯一实现。
- `tests/d3dx9.cpp` 的像素断言按 `REQ-01M7A74Y…` 的同一分档处理（结构 / GPU 臂 / 删）。
- 守卫：`d3d9p/sprite-no-cpu-composite`（Mock 或纯段）：让 sprite 批走"GPU 不可用"分支 ⇒ 断言未写像素 + 记账命中（修前会写 ⇒ 红）。
- 真机复核：TITLE/ADV/存档的 `sprite: draws=… gpu=… fallback=0` 保持不变。

## 非目标

- 不改 sprite 合批/顺序语义；不扩带纹理点/线。

## 依赖

`REQ-01M7A74YG08R65NJBFXF7DEJDK` 的账本/分档口径（同一批具名位与摘要字段）。
