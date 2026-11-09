# 🐞S2 emulator-lift · 后缓冲 surface 的 LockRect/UnlockRect 未实现（出 EU-ROOM 的路径停在这里）

- id: REQ-01M6ZDSGQY1RXX3N3WJKA8D26H
- type: bug
- status: done
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- verify: apps/emulator-lift/tests/d3d9.cpp#d3d9/backbuffer-lockrect
- repro: docs/03-emulator/defects.md#BUG-15
- tags: [emulator-lift, d3d9]

## 判据（修好后）

- `room-exit.graph` 真机跑：**exit 0** + trace 末尾 `task done`，终态 = TITLE 的稳定帧 hash
  （与 `room-enter.graph` 的 title 节点同一个值）。
- 后缓冲 `LockRect` 的两个出参按 D3D9 文档：`pBits` 指后缓冲像素（guest 地址）、`Pitch` = 后缓冲 pitch；
  ⛔ 不许把它写进**纹理**的锁字段（那会改坏设备状态块）。
- 至少一条 pure 守卫：`apps/emulator-lift/tests/d3d9.cpp` 里"后缓冲 surface 的 LockRect 返回 pBits/Pitch，
  且锁一个小矩形不改 pitch；UnlockRect 之后能再锁"。

## 读数（2026-11-09，macOS arm64 / RelWithDebInfo，`--skip-logo`）

- ESC 在 insn `1,400,000,000` 落下 ⇒ 停于 `1,400,000,900`；房间稳定帧 hash `0x96E51755F00B2E9E`（第 141 帧起）。
- 同一房间点 **CG 鑑賞**（`1100,150`）正常切画面 ⇒ 不是"房间完全不吃输入"。
- 缺陷史 `repo:docs/03-emulator/defects.md` 的 BUG-15；"硬停被记成 `Stopped` ⇒ exit 0"的同族读数在
  BUG-15 与需求单 `REQ-01M6WW9R6SJ7P8XA4522XCR48A`。

## 不做

- ⛔ 不把 `LockRect` 改成静默返回成功（房间会读到垃圾像素，比停住更难查）。
- ⛔ 不为了这条改 `.lst` / 语料。

## 收口（2026-11-09，macOS arm64 / RelWithDebInfo）

- **"未实现"这个缺陷本身已消除**：`LockRect(backbuffer)` 真机调用成功 2 次（`Pitch=5120`、`pBits=0x28000008`、矩形 0,0..1280,720）
  + `UnlockRect` 2 次，旧 trap 消失。锁字段落 `GuestSurface`（⛔ 未进 `'D9D1'` / `'TEX1'`）；
  `Pitch` = 整幅行距、`pBits` = 矩形左上；锁前 GPU 屏障（镜像目标 ⇒ `syncToGuest`，否则 `flush`）；解锁不标脏。
  契约失败（已锁 / 未锁 / 越界 / DISCARD / 无 LOCKABLE / MSAA / 未定义 flags / 坏出参）⇒ `D3DERR_INVALIDCALL` + Warn；本仓崩坏才 trap。
- 守卫：`apps/emulator-lift/tests/d3d9.cpp#d3d9/backbuffer-lockrect`（修前恢复 trap ⇒ 11 failures）·
  `apps/emulator-lift/tests/gpu_batch.cpp#gpu-batch/backbuffer-lock-barrier`（含补注册的 `gpu-batch-order`）。
- 变异：`--match backbuffer` 5/5 实测红后还原（pBits 去偏移 / Pitch 用子矩形宽 / 删已锁检查 / 删屏障 / 删 LOCKABLE）；
  复审另手工验 M4（Unlock 不清）红。
- ★ **端到端判据仍不成立，但不是本单的缺口**：`room-exit.graph` 现在停在新 gap
  `kernel32!RaiseException`（`Command_Exit_Exception` 的 C++ EH 展开，`eip=0x4123C4` 返回 `0x004123C9`）
  ⇒ 已另开 `REQ-01M708DF24KY0QZNEHY66S0HYW`；本单不再承载 e2e 终态，A 单的终态判据挂在那一单上。
