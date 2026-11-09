# 🐞S3 emulator-lift · lift 覆盖清单把已实现的 mmio* 登记成 stub（理由还写「空流」）

- id: REQ-01M6ZSF6RH014QZ907WN6WRFJW
- type: bug
- status: done
- parent: REQ-01M6K84KHPA1XF3B1HSHXME6W0
- repro: apps/emulator-lift/src/shims/winmm_audio.cpp#mmio：WAV 头解析
- severity: S3
- done_reason: 登记用错了 API：这 5 个 mmio 导出是真实现（沿 guest 文件句柄读 RIFF），却走了 addStub ⇒ lift report 把它们算成「显式 stub」、理由还写「返回 0 字节（空流）」，让读报告的人以为 WAV 头解析还没做（而 SE(WAV) 那条缺陷正是靠它们收口的）。已改 addStub → add（理由一起删）+ pnpm tools lift sync --write 派生 overrides.json（82 → 77 条）；读数：lift report 变成「导入 310 = 实现 251 + 显式 stub 59 + 裸缺口 0」，mmio 五行都落在 implemented；pnpm tools lift check 绿（含 registry-sync）、ctest 67/67。边界：只改登记不改行为，mmio 的实现范围（只支持 guest 句柄 + FINDRIFF/FINDLIST/FINDCHUNK，其余 MMIOERR_CHUNKNOTFOUND）仍只在代码注释里。
- tags: [emulator-lift, registry]

## 现象

`pnpm tools lift report`（2026-11-09）把 5 个 mmio 导出列为 `stub`，理由写着「返回 0 字节（空流）」；
但 `apps/emulator-lift/src/shims/winmm_audio.cpp` 里的实现是**真 RIFF 读取**（沿 guest 文件句柄的同一个
`FILE*`，`mmioDescend` 找 RIFF/fmt/data，`mmioRead` 读头）。同一份文件里的注释就写着「⛔ 不是 no-op」。

⇒ 后果是状态误报：读 `lift report` 的人会以为 WAV 头解析还没做，而
`REQ-01M6QSVR5NJDQVFHY6RX3934DN`（标题 SE(WAV) 加载失败）恰恰是靠这 5 个导出收口的。

## 根因

登记用错了 API：`addStub(...)` 的语义是「有实现、但按 SCOPE 偏差**整体 stub**」（`kind=stub` + 理由），
而 mmio 这组是**真替换**（`kind=replace`）⇒ 应该走 `add(...)`，不该带理由。

## 修后（2026-11-09）

- `addStub` → `add`（5 条，理由一起删）；`pnpm tools lift sync --write` 派生 `data/lift/overrides.json`
  （82 → 77 条）。
- `pnpm tools lift report`：`导入 310 = 实现 251 + 显式 stub 59 + 裸缺口 0`，mmio 五行都在 `implemented`。
- 判据：`pnpm tools lift check` 绿（含 `[registry-sync]`：C++ 每条 addStub/addUnimplemented 与 JSON 逐字一致）
  + `ctest` 67/67。

## 边界

只改**登记**，不改行为：mmio 的实现范围（只支持 guest 句柄 + `FINDRIFF`/`FINDLIST`/`FINDCHUNK`，
其余返回 `MMIOERR_CHUNKNOTFOUND`）仍然只在代码注释里，不走登记字段。
