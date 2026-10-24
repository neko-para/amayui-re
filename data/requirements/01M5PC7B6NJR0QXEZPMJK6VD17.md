# 残差第二十批：辅助表 B（3D 网格模型）矩阵槽

- id: REQ-01M5PC7B6NJR0QXEZPMJK6VD17
- type: req
- status: done
- parent: REQ-01M5BK8QMQYK7XQG707YVMQ202
- verify: tools/test/emulator-new-scene.test.mjs#★ 0x344 场景条目：置存在位 + 写值 + 标脏
- tags: [spec residual]

# 残差第二十批：辅助表 B（3D 网格模型）矩阵槽

## 一句话

辅助表 B 的七个矩阵槽全部有读出点：合成后只乘进**世界阶段**；三对"基/目标"槽 + 三个时间窗，窗内插值、到期折叠。残差关闭。

## 结论

- `temp = T(−p) · M(累积旋转) · M(基1) · M(基2) · M(基3) · T(+p)`，再乘世界；无视图 / 投影。
- 三对槽：缩放 / 旋转 / 平移；窗内缩放/平移逐元素插值、旋转按轴角插值后重建；到期折叠。
- 写者：`0x333` 窗/颜色；`0x338` 缩放目标 + 开关；`0x335/336/337` 即时基槽；`0x339/33a` 渐变目标；`0x33e` 累积旋转自乘；`0x334` 复位。
- `+0x1FC` 是矩阵（累积旋转）；`+0x1F4/+0x1F8` 才是标志。
- 更正 batch19：`+0x1FC` 写者 `0x33e`（`0x33d` 是相机/投影）；`+0x110/+0x190` 写者 `0x339/0x33a`。

## 正文 / 实现

正文只加 3 句领域话，不写偏移。实现不建模（core 无 3D 路径）；后续把 `0x330–0x33e` 注册成最小写者，避免 `unimplemented` 停机。

## 仍开着

`T(−p)X T(+p)` 意图 · 三对槽领域命名 · `+0x1F4/+0x1F8` 位语义 · 同键叠放真机 · 真实绘制 S2+ · 保存读侧。

## 判据

`pnpm test` 1020/0 · `pnpm test:assets` 203/0（1 skip）· `pnpm test:mutation` 336/336 · `pnpm tools spec validate` 13/13 · `pnpm tools requirements validate` 6/6 · `pnpm typecheck` 六工程绿。

## 怎么查

`pnpm tools spec show CORE-SCENE-DRAW-ORDER` · `docs/02-engine/spec-audit-2026-10.md` §37。
