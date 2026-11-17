# N1 · 雪景真 run 可达性：找到走到 202（Snow）的配方

- id: REQ-01M7NNNMPFB1GDB52J662Y1CMZ
- type: req
- status: open
- parent: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- tags: [emulator-lift, d3d9, recipe]

## 一句话

找到一条能**真正走到 202（Snow）**的 run 配方（加载第 4 槽 + 推进到 wipe 之后），
并用 `--until` 的**派发块入口**定位 202 的 draw，证明雪批已入队、位 21（`kUnGpuVertexW`）0 次。

## 为什么

- A（202 的 shim / CPU `vs_1_0` / `ps_1_0` / binding）与 B（GPU 透视校正插值）都已实现并通过夹具级判据；
  但 B 的 E2E 4 次尝试（`--until 0x00418340`）**一次未命中** ⇒ 现有配方走不到 202。
- A/B 的验收明确是"夹具级"；"雪真的出现在真 run"需要一条可达配方，这是 N1 的产出，不是 A/B 的实现缺口。

## 要做

1. 从 `apps/emulator-lift/scripts/recipes/sc0000-wipe.plan`（加载 SAVE03 + 1 击）继续：wipe 之后进入 3D 场景，
   找到触发 202 的条件（天气 / 场景 / 脚本 opcode `0x326` 的 `SetBlindWipe` 后续？）。
2. 用**派发块入口**做 `--until`（⛔ 不用 `call` 返回点；见 BUG-31）：定位 202 的 `DrawPrimitiveUP` / `SetMatrix("Proj")` 之后的批。
3. 记录：`kUnGpuVertexW` 次数、`skipped_draws`、`snow-pixels` 的对应帧、截图。
4. 把可用配方落成 tracked `apps/emulator-lift/scripts/recipes/`（文件名/注释写清前置：第 4 槽 + 几次点击）。

## 判据

- 一条 recipe 跑到 202 的 draw（有停止点/计数证据）；位 21 `kUnGpuVertexW` **0 次**、`skipped_draws` 不增。
- 雪在截图里可见（若不可见，必须给出具名原因：被谁拒 / 哪一帧 / 什么条件没满足，⛔ 不写"应该能看到"）。
- 读数进 `docs/03-emulator/wipe-profile.md` 或 `docs/03-emulator/gpu-perspective.md`（带日期/口径）；**不进默认门禁**
  （业务正确性不作判据）。

## 未做

- ⛔ 不实现 202 / B 的代码；⛔ 不改 caps / 守卫。
- 若 202 需要特定剧情推进，写清前置，不把"需要更多点击"当成失败。
