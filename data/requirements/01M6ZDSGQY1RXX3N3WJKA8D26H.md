# 🐞S2 emulator-lift · 后缓冲 surface 的 LockRect/UnlockRect 未实现（出 EU-ROOM 的路径停在这里）

- id: REQ-01M6ZDSGQY1RXX3N3WJKA8D26H
- type: bug
- status: open
- parent: REQ-01M3TC7BK0J9TBQKGGWQ35A3NR
- repro: docs/03-emulator/defects.md#BUG-15
- tags: [emulator-lift, d3d9]

## 现象

在 `EU-ROOM` 里按 ESC（走"退出房间"的同一条路径）⇒ 900 条指令后停：

```
stop      : stopped @ eip=004A4F8D
detail    : unimplemented D3DX9 call（shim com!IDirect3DSurface9::LockRect）
```

⇒ **出房间的路径今天走不完**；交互任务 `repo:apps/emulator-lift/scripts/interaction/room-exit.graph` 的
终态判据因此不可达（它会响亮失败，而不是静默通过）。

## 根因

`repo:apps/emulator-lift/src/shims/d3dx9.cpp` 的 `shimSurfaceLockRect` / `shimSurfaceUnlockRect`
对"后缓冲 surface（state == 设备状态块）"**明确 trap**（"⛔ 不把锁字段写进设备状态块"）。
房间退出路径要读后缓冲（转场 / 缩略图），于是停在这里。

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
