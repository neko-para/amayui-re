# spike · FELLOW 切角色单帧 ~1 s 值不值得动（先答「真机是否也卡」）

- id: REQ-01M7ZFEWVMAPFGVJQEE8S80VCT
- type: spike
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, perf, fellow]

## 一句话

用户报「FELLOW 里点一下切角色明显卡顿」。已取证并落文档（`repo:docs/03-emulator/perf-fellow-switch.md`，提交 `7d3b424`）：卡顿 = **单帧 ≈1.0 s**（958–1076 ms / 433–437 M insn，30/30 复现；同跑稳态 8–9 ms）。本单**不是**"性能 bug"，只是一个待裁决的问题：要不要为它花力气 —— 而**前提问题先答**：真机是否也卡。

## 已取证（读数，详见文档）

- 归因：`lift` + `boundary` **56–61%**（guest 计算）；`shim` 34.9–38.2% 里 **≈26 点**是 guest 自己的忙等（`PeekMessageA` ≈16.2 万次 / `timeGetTime` ≈48.5 万次）；`present` 只 **1.4–1.9%**。
- 一次切换 ≈ **433 M insn / ≈474 K 个主循环回合**（914 insn、19.6 派发/回合）⇒ 是**引擎主循环空转** ~1 s，不是"逐像素合成"（逐像素转换在切换帧只 1.9–2%）。
- **不是被我们的 shim 逼上 CPU**：`GetDeviceCaps` 只当渲染器初始化时的**下限闸**（报小 ⇒ 直接 `E_FAIL` 初始化失败，不是"退 CPU 路"）；`CheckDeviceFormat` 只决定 X / A 格式（bpp 相同、都进 GPU 纹理）。

## 若要动，先回答这个前提

**真机（原 AGE.EXE + 真 D3D9）在同一次切换上是否也卡到秒级？** 没答之前：

- 我们与真机在"每条指令的模拟成本"上仍有 3–5 倍量级的差距（**父级口径，本单没有可复跑读数**）⇒ 那是另一条线（AOT 吞吐），不在本单；
- 不改语义的可省上界 = **15–25%**（1.0 s → 0.75–0.85 s；忙等 shim 的入口 / 出口税 10–20 点 + 派发税数点），文档 §7 已算过 ⇒ **不是修 bug 的量级**；`≈474 K 回合` 的结构性开销占 ~70% 入口，**改它就是改语义**。
- ⇒ 真机不卡 ⇒ 这是**已知偏差**，记进文档、不排期；真机也卡 ⇒ 再决定是否开实现单（先做文档 §8 的窗口级 `dispatch-top` 与慢帧 `script` / `pos` 采样）。

## 复现（能照抄）

配方已入库：`repo:apps/emulator-lift/scripts/recipes/fellow-switch.plan`（零 `wait`；窗口 = `click` → 等 `SETTOOLLIST` → 等回 `FELLOW` → 等主轮询体 `[0x1570,0x15A8)`）。

```bash
cd apps/emulator-lift
scripts/clean-appdata.sh ../../.tmp/fellow/appdata
../../.tmp/fellow-profile/bin/emulator-lift-aot.exe --present window --present-hidden \
  --game-dir ../../links/install --appdata ../../.tmp/fellow/appdata \
  --skip-logo --audio off --time-limit 240 --input-plan scripts/recipes/fellow-switch.plan \
  --profile --profile-slow-frames 100 --frame-log --input-plan-trace
```

⚠ 文档 §0 的墙钟读数绑的是冻结二进制 `sha256 128A7E16…`（当日 `HEAD` `04a6e24` + 脏树）；换今天的 exe 就是另一张表 ⇒ 判据是**形状**（+433 M insn 的慢帧、`input-plan: pending=0`），⛔ 不是 ms 数。
