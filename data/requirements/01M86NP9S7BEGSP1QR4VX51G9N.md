# AOT · 库函数内建普查：第 2 个内建收谁（用派发份额 × FLIRT 名）

- id: REQ-01M86NP9S7BEGSP1QR4VX51G9N
- type: req
- status: done
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- done_reason: 普查完成（真 calls 由 --no-s7 诊断构建 + fast/decline 对账 0.00056%）：_memcpy_0 点击 63/装载 13,244 次，估价 3.0µs/0.68ms，远低于 5/20ms 线；其它候选更小；_memcpy 已内建过线。⇒ 不落第二个，表保持 1 行；将来 _memcpy_0 ≥147K（装载）/≥37K（点击）再落。
- tags: [emulator-lift, aot, intrinsics]

## 结论（2026-11-24 普查）

按现有三份数据求交，**能双证为静态 CRT 且还在窗口里出现的只有 `_memcpy` 自己**（三个块）；`_memcmp` /
`_strlen` / `_strcmp` / `_memmove_s` 等 **0 个 `call` 站点**（或不发射），`_memset` 已排除。
⇒ 本阶段**不落第二个内建**（`aot/intrinsics.txt` 保持 1 行）。但「第 2 个收谁」还不能最终判定 ——
数据缺口见下，测量前置已拆到 `REQ-01M86VSP8CXH998HTJTD50MEGC`（M1–M4）。

## 候选表（节选；完整 32 行见 `.tmp/reveq/intrinsics/candidates.csv`）

| EA | 名字 | 静态 call 站点 | 点击窗口 seg2 | 动态次数 | 契约 |
|---|---|---:|---|---:|---|
| `0x4E74B0` | `_memcpy` | 202 | **#1 / 26.5%** | **1,966,230** | 已内建 |
| `0x4E78E0` | `_memcpy_0` | 43 | 不在 top-20 | 未测 | 同族；待逐字节比体 |
| `0x4E7400` | `_memset` | 185 | 不在 top-20 | **1,074** | 已排除（<0.1 ms） |
| `0x4E82C0` | `__ftol2_sse` | 385 | 不在 top-20 | 未测 | x87/EDX:EAX/20 B scratch ⇒ 不是 v1 形态 |
| `0x4F8434` | `_memcmp` | **0** | 不在 | 0（不发射） | 不在 closure |
| `0x4F4620` | `_strlen` | **0** | 不在 | 0（死代码） | — |
| `0x4E8A19` | `_sprintf_s` | 365 | 不在 top-20 | 未测 | varargs ⇒ 不可精确表达 |
| `0x4ED5C0` | `_qsort` | 5 | 不在 top-20 | 未测 | guest 回调（§12.8 红线） |

## 数据缺口（⇒ 本单 blocked）

1. **装载帧没有 `window-dispatch-top`**：装载在第一个 `profile.begin` 之前 ⇒ 单里「三份窗口」实际只有
   点击窗口那一份是窗口级且内建后的；FELLOW 只有 `04a6e24` 旧二进制的**整跑**表。
2. **`window-dispatch-top` 是派发份额，不是单次成本**（`observations.md` 已纠正过）；「不在 top-20」
   只给次数上界（seg2 <≈136 K），⛔ 不能换算成 ms。
3. **只有 `_memcpy` / `_memset` 有逐次画像**（`LIFT_MEMCPY_DIAG`）；其余候选没有动态调用数。

## 已排除（反例，别再查）

`_memset`（1,074 次 / 2.5 s ⇒ <0.1 ms）· `__ftol2_sse`（契约不是 v1 发射器能表达的形态）·
`_sprintf_s`（varargs）· `_qsort`（guest 回调）· `_malloc` / `_free`（堆契约）·
`_memcmp` / `_strlen` / `_strcmp` / `_memmove`（0 站点或不发射）。`_memset` 的 FLIRT 在
`.lst:381846`，是活的，只是不热。

## 若测量后必须落一个

首选 **`_memcpy_0`（`0x4E78E0`）**：第二份 `_memcpy`（同 0x361 B / 同 FLIRT 名族、43 个静态站点），
是唯一几乎不新增契约面的候选；**前提**：与 `0x4E74B0` 逐字节同体 + 是 `components[].root`
（否则 `lift-gen` 的硬闸 exit 1）。次选 `_memset`（契约简单但量差 3 个数量级）；不选
`__ftol2_sse`（≈ `_memcpy` 的 3–5 倍工作量）。

## 边界

⛔ 只收**静态 CRT / 平台库函数**（FLIRT 名 + 反汇编契约双证）；⛔ 不收引擎自己的 handler（§12.8）；
⛔ 不按调用点 / EA 开特判；每个内建都要一份契约差分（`docs/03-emulator/aot-intrinsics.md` §5 六步）。

## 结果（2026-11-25：不落第二个内建）

真 calls（`--no-s7` 诊断构建，与 `fast+decline` 对账 **0.00056%**）：

- 点击 seg2：`_memcpy` 2,559,744 · `_memcpy_0` **63** · `_memset` 1,222 · `__ftol2_sse` 393 · `_strcpy_s` 858
- 装载 load9：`_memcpy` 9,117,397 · `_memcpy_0` **13,244** · `_memset` 3,573 · `__ftol2_sse` **154,805** ·
  `_strcpy_s` 7,571
- FELLOW s1：`_memcpy` 66,274 · `_memcpy_0` 0 · `_memset` 526 · `__ftol2_sse` 1,428 · `_strcpy_s` 2,842

`_memcpy_0` 估价：点击 **3.0 µs** / 装载 **0.68 ms**（136 ns 上界 1.8 ms）⇒ 差 5/20 ms 线 **≥11× / ≥500×**。
其它最大 `__ftol2_sse` 装载 ≈2.99 ms、`_strcpy_s` ≈1.69 ms；只有 `_memcpy`（已内建）过线。
⇒ `aot/intrinsics.txt` 保持 1 行。将来 `_memcpy_0` 的 calls 涨到 **≥147 K（装载）/ ≥37 K（点击）** 再落；
它两件硬前提仍成立（逐字节同体 + 是 unit root），只是量不够。
