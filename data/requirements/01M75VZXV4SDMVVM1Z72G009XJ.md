# 音频尖峰：feeder 在 shim 边界被主线程长时间挡在令牌外（单刀 1.3–1.7 s 全是 shimtok）⇒ 静音晚到 ⇒ 尾声重播 2–3 次

- id: REQ-01M75VZXV4SDMVVM1Z72G009XJ
- type: bug
- status: blocked
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- blocked_by: [REQ-01M75WWFGT8ZEBGEPENQHSYTB8]
- repro: graphs/se-eof.graph#click 830 592
- severity: S1
- tags: [emulator-lift audio token scheduler]

## 现象

GAMESTART 点"下一步"播 SE，SE 播完后**混着 SE 的尾声被重播 2–3 次**然后停。
用户机器可稳定复现；本机（`graphs/se-eof.graph` + `--audio sdl --sound --audio-dump`）也能复现。

## 读数（插桩后实测，本机 `out-inst`）

```
fills=32 notify=49 ... slot_reverify_fail=0 fill_max_ms=1743.79 fill_slow=4
```

单刀拆解（`wait`/`pre`/`guest`/`shimtok`/`shimblk`/`total`）：

| 刀 | guest | **shimtok** | shimblk | total |
|---|---|---|---|---|
| slot=1 q3 | 1342.25 | **1340.84** | 27 | 1578.03 |
| slot=11 q3 | 1716.85 | **1532.98** | 7 | 1743.79 |
| slot=11 q0 | 723.91 | **547.04** | 7 | 785.91 |
| slot=1 q1 | 615.26 | **614.96** | 2 | 615.26 |
| slot=11 q0 | 1511.91 | **1322.28** | 3 | 1551.57 |

⇒ **尖峰几乎全是 `shimtok`（feeder 在 shim 边界等 guest 令牌的时间）**，不是解码本身：
`guest − shimtok` 常常只有 1–3 ms；对照稳定刀 `slot=11` 的 `guest≈180 ms / shimtok≈2 ms` 才是真解码。
`wait`（外层 acquire）多数为 0，只有一次 156 ms ⇒ **不是"入场排队"，是进 fill 之后每次 shim 边界都被挡**。

## 机制

1. feeder 在 fill 里**不持令牌执行 guest 指令**，但**每个 shim 调用**都要取令牌（`ShimRegistry::invoke`）；
2. 主线程在同一把令牌上跑它自己的块与 shim ⇒ 两边**在同一临界区上串行**；
3. 切场景/播 SE 时主线程有长段"持令牌"的执行（块或长 shim），feeder 每次边界都排在后面 ⇒
   27 次边界累计 1.34 s（平均每次 ~50 ms）；
4. 一刀 fill 因此变成 1.3–1.7 s > 环长（2.000 s 的一半以上）⇒ **本该替换尾巴的静音晚到 1–2 圈** ⇒
   混着 SE 的尾声被重混 2–3 次，与听感完全吻合。

## 修法候选（要评估，别照抄）

- **(a) 主线程在块边界检查"feeder 正在等令牌" ⇒ 主动让出**（一个标志 + 一处边界检查，改动最小）；
- **(b) 令牌加优先级/公平**（等待者优先、或按等待时长提升）——影响所有线程，风险最大；
- **(c) 缩短主线程单次持令牌的时长**（时间片 / 批大小）；
- **(d) 减少 feeder 取令牌的次数**（合并 shim 调用、或把 fill 里可脱离 guest 的段整体 native 化）——
  和吞吐单方向一致，但**不能**指望它消掉尖峰：尖峰是"等"，不是"算"。

## 判据

- `fill_slow`（>1 s 的刀）应降到 **0**；`fill_max_ms` 应从 1.7 s 降到接近稳定刀（~200 ms）；
- `shimtok` 在单刀里的占比应降到与真解码相当的量级；
- `late_q` 不应长期 ≤ −1（游标已越过目标一整 quarter 以上）；
- 听感：SE 结束后不再重复。

## 关联

- 稳态占空（三流 120–150%）是另一件事 ⇒ `REQ-01M75PTEGCR1Y91FKKJ1CWPNH2`（Huffman/位读）。
  **本单解决尖峰，那张单解决稳态；两者都要，不能互相替代。**
- 插桩字段的读法要补进 `apps/emulator-lift/docs/audio-implementation.md`（本轮没动该文件）。

## 实测进展（2026-11-12，合并构建，`graphs/se-eof.graph`）

| run | `fill_max_ms` | `fill_slow` | WAV 里 lag=2.000 s 的重复段 |
|---|---|---|---|
| 旧（仅三层补偿） | 1743.8 | 4 | **有**（9.92–11.37 s） |
| 合并 #4 / #5 / #6 | 1687.7 / 1302.8 / 1045.9 | 3 / 1 / 1 | **0 组（3/3）** |

★ **诚实读法**：Huffman + rep 把单刀从 1.74 s 压到 1.05–1.30 s ⇒ 静音能在一圈内到达 ⇒ 听感重复消失；
但 `shimtok` 仍 **1.0–1.3 s**（令牌饥饿没动）⇒ 只是**掉到阈值以下**，不是根治。⇒ 本单保持 `blocked_by` M0/M1，
判据不变：**`fill_slow = 0` 且 `fill_max_ms` ≈ 0.2 s 量级**。

## 2026-11-12 本机（macOS）读数：不复现
`fill_max 126.55 / fill_slow 0 / shimtok≈0–3 ms`，`--audio-dump` 的 WAV 在 lag=2.000 s 无重复段 ⇒ Windows 的 1.3–1.7 s 尖峰本机不复现；A/B 见 `REQ-01M770JFDPZKKJR7D0ZZ09FJJ7`。
