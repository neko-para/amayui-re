# 音频解码吞吐：ADV 每句 ~1.7 s（本机）/ ~0.75 s（参考机）的三个杠杆（Huffman/码本 · 起播重建解码器 · __ftol2_sse）

- id: REQ-01M75PTEGCR1Y91FKKJ1CWPNH2
- type: req
- status: open
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- tags: [emulator-lift audio performance decode]

## 目标

把**一刀 fill** 从 **200–250 ms（用户机）** 压到 **50–100 ms**，从而把 three-stream 占空从 120–150% 降到 ~50%。

## ★ 口径（先说清，否则预算必错）

- **两个绝对值来自两台机器**：本机 profile = **412 ms/刀**；用户机 trace 的 `total` = **200–250 ms/刀**（差 ~1.7×）。
  **比例可搬，绝对值不可搬。**
- 单刀 = `sub_4B6C30`（填一个 quarter）的全部墙钟；下表以**用户机 230 ms = 100%** 为基准，比例来自本机 profile。
- ⛔ **不要再用"残差"当预算**：早期用 `1237−823−176−42` 减出"Ogg 分页 65 ms/刀"是错的 ——
  12 刀直接求和是 `391.1 / 12 = 32.6 ms/刀`。**一律用直接求和。**

## 每刀预算（本机 profile 的比例，摊到用户机）

| 组成 | 占 fill | 用户机 ≈ms | 状态 |
|---|---|---|---|
| Huffman 子树 `0x4E1570`（look/首表/二分/adv） | **48.6%** 里的 68.9% | ~76 | 待做（S2） |
| └ 其中：位读 look/adv/位反转（`0x513220`/`0x5132D0`/`0x4E1510`） | 38.5% of Huffman | — | 被整棵吸收 |
| 码本向量 `0x4E1950` | 17.0% of Huffman | ~19 | 待做（S2） |
| PCM 转换（`sub_48D610` self，`0x48D74A`/`0x48D767`） | 9–16% | ~33 | 待做（需段级） |
| `__ftol2_sse` | 14% | ~32 | ✅ **已 done** |
| 其余（`sub_4E6480`/`sub_4E61E0`/self 时间/跨包窗口） | ~21% | ~48 | 未定性 |

⇒ **Huffman S2 + PCM 转换** 一起做，才能落到 ~100 ms；单靠 Huffman 只到 ~134 ms。

## 已否 / 不做

- **arena 优化**：`0x4D7A60`+`0x4D7AD0` ≈ **0.3 ms/刀** ⇒ 不是性能问题，只是**正确性约束**；只换叶子可结构性绕开它。
- **只换位读原语**（`513220`/`5132D0`）：最多 −61 ms，被 `0x4E1570` 整棵吸收 ⇒ 粒度取整棵。
- **整块换 `sub_4E3EE0`**：能到 ~3 ms，但风险高一个量级（arena 分配顺序、14 个 guest call、三个状态机）⇒ 不做。
- **段级 override 在 feeder 不生效**（`runFeederGuest` 只查函数级表）⇒ PCM 段必须先做
  `REQ-01M75W6BB3PY3MF42ZTS23KTBS`（feeder 段级 dispatch）。

## 与"尖峰"不是同一件事

- 用户听到的"SE 尾声重复 2–3 次"来自 **feeder 被令牌挡 1.3–1.7 s**（`shimtok` 占单刀 99%），**不是算力**
  ⇒ 见 `REQ-01M75VZXV4SDMVVM1Z72G009XJ`（令牌）与令牌模型复盘。
- 本单解决**稳态占空**；两者不能互相替代。

## 判据 / 读数

- **pump 模式**（`--audio off/virtual`）：窗口 profiler 读 `sub_4E2A30->sub_4E4370 incl`。
- **真线程模式**：只能用 `LIFT_AUDIO_TRACE=1` 的 `[audio] feeder fill … guest=… ov=N`
  （`ov` 不涨 = override 没生效；feeder 有独立 override 副本、计数器分开）。
- 收口：`guest=` 与 `fill_max_ms` 同时下降；比例读数进文档，**不进 test**。

## 未做

- 参考机与本机 2.3× 差距是否全为解释器速度（未拆）。
- `sub_4E6480` / `sub_4E61E0` 两块（~13%）**未定性**。
