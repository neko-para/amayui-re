# memcpy 重叠不再回退解释器（按方向分流的前/后向软实现；guest 在 dst>src 时走后向）

- id: REQ-01M75WMG9TEYVF8Y27CHP9MTYF
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/tests/native_override.cpp#memcpy-ovl/dst>src-d8-n64
- tags: [emulator-lift native-override performance]

## 目标

把 `_memcpy` / `_memcpy_0` 的 native override 从"**重叠就回退解释器**"扩到**重叠也能接管**（用户口径：哪怕不用宿主 `memcpy`，软实现也比纯解释快得多）。
本期范围**只有重叠**；其余回退场景照旧回退。

## ★ 关键取证（推翻父级的一个错误指令）

guest 的 `_memcpy`（`0x4E74B0`，865 B，`.lst` 里 COLLAPSED ⇒ 字节从 PE 取）在重叠时**按方向分流**：

- `0x4E74C6 jbe 0x4E74D0` ⇒ `dst <= src` **前向**（`0x4E7508 F3 A5 rep movsd` + 字节尾表）；
- `0x4E74CA jb 0x4E7670` ⇒ `dst > src` 且重叠 **后向**（`0x4E768B FD std` / `0x4E768C F3 A5 rep movsd` / `0x4E768E FC cld`）；
- DF 成对 `std…cld`，出口恒 0；SSE2 支 `0x4F1120` 在本线**不可达**（`[0x55F2E8]` 唯一写者 `0x4F1117` = `IsProcessorFeaturePresent(10)`，shim 恒 0）。

⇒ 父级原指令"**重叠一律严格前向**"是**错的**（实测 M1 ⇒ `dst>src` 全族 7 条红）。
正确判据是"**结果与解释器臂逐字节相同**"，实现按方向分流（`copyForwardBytes` / `copyBackwardBytes`），用**真字节**差分证明。

## 落地

- `native_crt.cpp`：`copyForwardBytes` / `copyBackwardBytes`（`kMemcpyChunkMin = 32`；guest 自己就用 dword 计数 8 ⇒ 阈值有据且**不参与语义**）+ `MemcpyWhy` decline 桶；非重叠仍 `std::memcpy`。
- 守卫：pure `memcpy-ovl/*`（`same` / `dst<src` / `dst>src` × `n=0/1/3/4/7/8/64/4096` + `dst<src-d8-n64` / `dst>src-d8-n64` / `straddle-page-n64`）；
  assets `memcpy-overlap/*`（解释器臂 = **真 `_memcpy` 字节**）。
- `tools/mutate-check.mjs` 一条代表（M2p）。

## 收益口径（★ 别高估）

既有 4 次 run 稳定：`_memcpy_0 … fallbacks=36` / hits≈62.4k，`_memcpy … 0` / 73k
⇒ **重叠回退只占 0.027% 的调用量**；**单块 `n` 未取证**（没跑 emulator）。
⇒ 本单是"**消掉一类静默回退**"，不是"大头提速"。下一条命令验收口：`_memcpy_0 … fallbacks=0`，
且 declines 只剩 `CrossBoundary` / `Uncommitted`。

## 一条测试教训（给后来人）

变异 **M3**（把 `if(!overlap)` 改成 `if(1)`，即重叠也走宿主 `std::memcpy`）**不会红**：
本机 UCRT 的 `memcpy` 与 guest `_memcpy` 恰好同出一支 ⇒ 结果一样。
⇒ **"靠宿主 `memcpy`"不能当判据**，只有"真 guest 字节"的差分才作数（assets 档就是为了这个）。

## 判据 / 门禁

- `cmake --build out-mem` exit 0；`ctest -R "native|loader"` **4/4**；`ctest` 全量 **82/82**（142.01 s）；
  `git diff --check` exit 0；断言数 `native-override 279766/0`、`native-override-assets 1556/0`。
- 已知不比：时钟相位、`ECX/EDX/EFLAGS`。

## 账本备注

本单改动**随 `13a25a9` 提交**（提交说明写的是 "audio updates & ftol2"，未提 memcpy）——提交时机与一个正在干活的
agent 重叠导致。⛔ 不要改写历史；以本单为账。
