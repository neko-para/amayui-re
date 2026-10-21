# 0x5D8F8 三格是否被存档覆盖（归属/用途已收口或转移，见正文）

- id: REQ-01M48EVKSNX139B8TX0CYXZ0A1
- type: req
- status: blocked
- parent: REQ-01M48DJ3PK48CXQXAV1M3EEQNR
- order: 12
- blocked_by: [REQ-01M4N44X62K02PJYKD5NRACK75]
- tags: [engine, ledger]

## ★ 2026-10-11：本问句**当前不可回答**，已按纪律改成 `blocked`

**新落的一条台账观察**（`KN-01M4N1ZX2P1V175V6C6R2P0B4W`，subject `save/decoder-absent-and-only-known-payload-read`）：
**本仓没有存档解码器** —— `apps/emulator/src` 只有 `host/` · `model/` · `vm/` 三个目录；全仓 `saveCodec|decodeSave|savedata` **0 命中**；对 `SAVE??.DAT` **载荷**的唯一读取是
`tools/test/fixtures.test.mjs` 的 `b.readUInt16LE(264 + i * 2)`（`i = 0..6`，七个时间戳字段），**其余一个字节都没被解过**。

⇒ 本问句（"`SAVE??.DAT` 里有没有存这三格"）**不是"查了没有"，而是"没有能查的东西"**：
* 它**不许**停在 `open`（那会让人以为"再找找就有了"）；
* 它也**不许**按 `dropped` 处理 —— `dropped_reason` 的措辞是"不做了"，而这件事**要做**，只是**依赖还没建出来**；
* 正确状态 = **`blocked` + `blocked_by` 指向那份依赖**：`REQ-01M4N44X62K02PJYKD5NRACK75`（存档写入侧与恢复路径取证）。旧 M4-1 计划已 drop（`apps/emulator` 废弃），存档侧现在由那一单负责。

## 剩下的这一件（问句不变）：三格是否被存档覆盖

问句：`SAVE??.DAT` 里有没有存这三个指针所指的表（或它们所描述的那三个 `30·cur` 计数）？
⇒ 这决定"只写不读"是不是"**留待存档**"（若是，它们就不是死数据）。

**已知障碍（实测，保留）**：存档的 pool 块是**编码 / 压缩过**的 ——
`SAVE78.DAT` 里裸 little-endian `F0 7F 0F 00` 与 `DC 8A 70 00` 均 **0 命中**。

## ★ 2026-10-10 收窄（去重）：本单只留一件

* ~~**用途**~~ ⇒ **已收口**（`KN-01M4GV5PZ45P055R4P4321392K`）：`0x5D8F8/0x5D8FC/0x5D900` 是 `write-only-no-readers`
  （全语料恰好 6 处：建立 4 + `_memset` 1 + 释放 1）。
* ~~**归属**~~ ⇒ **不在本单**：由 `REQ-01M4GSW2R999MBJQ9S9EEQ481E`（同父，order 15）跟踪。
  ★ 两单原先重复跟踪，按纪律去重（**不**用 `superseded` 表示"重复"）。

## 判据（收口条件）

* 要么：把"存档里有 / 没有这三格"按准入门登记进 `data/ledger/`（锚 = EA，或存档字节偏移 + 摘要），
  并把本单 `verify` 指到对应守卫（或 `repro`）；
* ⛔ 不许把"没找到"当成"没有" —— 那正是本单要变成结论的东西。
* ★ **解除 `blocked` 的条件**：`REQ-01M4N44X62K02PJYKD5NRACK75` 的存档侧取证有结论（能按偏移读 pool 块）。
