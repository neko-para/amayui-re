# AOT · 构建期库内建表：_memcpy 入口快路径（闸门 + 回退）

- id: REQ-01M84VAK09EGFW1T0H6GDR5PZN
- type: req
- status: done
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- verify: apps/emulator-lift/tests/lift_intrinsics.cpp#lift-intrinsics
- tags: [emulator-lift, aot, intrinsics, perf]

# AOT · 构建期库内建表：`_memcpy` 入口快路径（闸门 + 回退）

## 一句话

静态 CRT 的 `_memcpy`（`0x004E74B0`）在 FIELD 点击路径上被调 **2.29 M 次 / 2.5 s**、尺寸 p50=p90=**4 B**
⇒ 每次 33 条 lifted 指令 + 3.2 次派发 ≈136 ns。构建期在它的单元根入口发射"带闸门快路径 +
回退原体"（宿主 `memmove`），把这段降到 ≈30 ns/次；大帧 **409 → 305 ms（−25.4%）**。
形态与判据见 `repo:docs/03-emulator/aot-intrinsics.md`；表是 `repo:apps/emulator-lift/aot/intrinsics.txt`（入库件）。

## 判据

1. 表被 `lift-gen` **硬校验**：PE 的 sha256 前缀一致 + 每个 EA 仍是**已发射单元的根**，否则构建期 `FATAL`
   （换 PE 后"闸门静默失效"不可能）。
2. 闸门**只读**（可在夹具里单独断言），拒绝时回退到**原 lifted 函数体**、逐字节不变；闸门判据 =
   `n==0` 或（`n<=16` ∧ 两端 `+n ≤ 4 GiB` ∧ `committed(dst,n)` ∧ `committed(src,n)`）。
3. CTest `lift-intrinsics`（默认档）：矩阵（size × 对齐 × 重叠 × 未提交页）+ dst 外哨兵 +
   EAX/callee-saved/DF + helper vs `memmove` + 开↔关差分。
4. 变异：去掉闸门的 `committed` 检查 ⇒ 该用例红并点名"未提交页没被 decline"。
5. 五项指纹（`exit` / `stop @` / arena / `table misses`）与关闭内建时逐位相同（`frames` 是墙钟驱动
   插件的非确定性，不列入）。

## 读数（同二进制 A/B，2026-11-24 / win32 / dev `8ab0861`）

| 读数 | 关 | 开 | Δ |
|---|---:|---:|---:|
| 大帧 wall | 409.204 ms | **305.215 ms** | −25.4% |
| 大帧 insns | 182,898,267 | **151,172,806** | −17.3% |
| `_memcpy` insn/次 | 33.39 | 0.0014 | — |
| `_memcpy` ns/次（折算） | ≈136 | **≈30** | −105.8 |
| seg2（2.5 s，同调用数归一） | ≈525.9 M insns | **440.6 M** | −16.2% |

## 未做 / 待核

- **`_memset`（`0x004E7400`）**：同模板，下一版收（先让流水线跑通）。
- **阈值 16 未调**：n=4 占 99.99%；调它会改变回退比例，必须重测同一张表。
- **ECX/EDX 残留不拟合**：ABI 不保证（显式裁决，见文档 §4-1）。
- `--no-intrinsics` 路径仍付"闸门调用 + 参数读"的净税（只在 A/B / 诊断时走，不影响缺省档）。
