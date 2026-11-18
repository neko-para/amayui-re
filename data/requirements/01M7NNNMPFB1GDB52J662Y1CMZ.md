# N1 · 雪景真 run 可达性：找到走到 202（Snow）的配方

- id: REQ-01M7NNNMPFB1GDB52J662Y1CMZ
- type: req
- status: open
- parent: REQ-01M7MNPGETP2CQJ3ACMCGEHKF5
- tags: [emulator-lift, d3d9, recipe]

## 一句话

找到一条能**真正走到 202（Snow）**的 run 配方（加载雪地图存档 + 推进），并用**派发块入口**的 `--until`
证明雪批已入队、位 21 0 次。

## 现状（2026-11-18 实测，结论：这份输入面到不了）

- **输入面**：`.tmp/appdata` 的四张存档全在**序章 / 地牢 2D ADV**：
  SAVE00 → `SN0000:0x46E8`（序章旁白）、SAVE02 → `SC0000:0x99C0`（地牢）、SAVE03 → `SC0000:0xCB9C`（地牢）。
  ★ 「第 4 槽 + 1 击」的 wipe 之后仍是**地牢 2D ADV**（再点 10 下仍在地牢）⇒ **不是雪景**。
- **运行**：两条链共 20,000 帧，挂 `--until 0x418340`（202 唯一创建点，`lift_table.inc` 的合法块入口）⇒ **0 命中**、`[warn]` 0 条。
- **静态**：202 唯一创建点 `0x418340`（`Set3DEffectSnow`）；唯一脚本上游 = `opcode 0x326`（handler `0x426E10`）；
  `i326` 在 941 份脚本清单里只有 1 次（`SETWEATHER.BIN:39`），且被 `global-int 4fdb == 2` 门控；
  `4fdb` 的 189 处脚本写点里没有一次写 2 ⇒ 天气型来自 **`SETL2DMOC` 数据记录**（`i341`/`i345`），
  **雪是地图数据驱动**，不是剧情脚本驱动。

## 卡住（需要人）

- ★ **用户口径（2026-11-18）：暂时也不知道具体雪景存档** ⇒ 本单保持**待验证**记录，不阻塞其他工作；有雪档再继续。
- **需要一份雪地图存档**（真机走到雪景存一槽；或已有存档给我）。没有它，换任何点击配方都到不了 202。
- agent 能绕的部分：静态解析 `SETL2DMOC` 的 238 条 `i341` 记录（天气列 = 2 的地图 id），与当前地图 id 对齐；
  或用 `LIFT_AOT_SHIM_TRACE=1` grep `D3DXCreateEffectFromResourceA` 的资源号 `202` 做存在性快判据。

## 下一步（有雪档后）

1. 把雪档放进 appdata，按加载屏行 y 选中它；用 `sc0000-wipe.plan` 同形（加载 + 推进）改槽位。
2. `--until 0x418340`（首次创建）；再 `--until 0x004B56D0`（`Snow::Render` / `SetMatrix("Proj")`）。
3. 记录位 21 / `skipped_draws` / 截图；落 tracked recipe。

## 判据

- 一条 recipe 跑到 202 的 draw（停止点/计数证据）；位 21 `kUnGpuVertexW` **0 次**、`skipped_draws` 不增。
- 雪在截图里可见；若不可见，给具名原因（被谁拒 / 哪一帧 / 什么条件没满足），⛔ 不写"应该能看到"。
- 读数进 `docs/03-emulator/gpu-perspective.md`；不进默认门禁。

## 未做

- ⛔ 不实现 202 / B 的代码；⛔ 不改 caps / 守卫。
- 若需要特定地图，写清前置；不把"需要更多点击"当失败。
