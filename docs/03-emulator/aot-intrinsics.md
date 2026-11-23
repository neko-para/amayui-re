# AOT · 构建期库内建（`aot/intrinsics.txt`）：`memcpy` 入口快路径

> 读数日期 2026-11-24 / win32 / dev `8ab0861` / RelWithDebInfo / `split8-O2-lto-abi`。
> 本文只写**口径 / 机制 / 实测**；表本身与判据在 `repo:apps/emulator-lift/aot/intrinsics.txt`（入库件）。

## 0. 一句话

对**静态 CRT 的库函数**（v1 只有 `_memcpy`），在它的单元根入口发射一段**带闸门的宿主快路径**：
闸门 Ok ⇒ 宿主 `memmove` 干完并按 cdecl 返回；拒绝 ⇒ `br` 回**原来的 lifted 函数体**。
它**不是** native override、不是运行期 EA→函数指针表，而是**生成期**决定，产物可 pin / 可 diff / 可审。

## 1. 为什么要它（读数）

SAVE11 点击后 2.5 s 窗口里 `_memcpy`（`0x004E74B0`，静态 CRT，`.lst` 的 FLIRT 名）：

| 项 | 读数 |
|---|---|
| 调用次数 | **2,293,913 次 / 2.5 s** |
| 拷贝尺寸 | **avg 4.39 B**；**p50 = p90 = 4**；99.993% 落 4–7 B 桶；最大 400 KB |
| 自身成本 | **76.6 M insns = 15.0%**，33.4 条/次、3.23 次派发/次 ⇒ ≈136 ns/次 |
| `rep_bulk` | 102/102 命中、0 decline，但只覆盖 **9% 字节**（n=4 根本不执行 `rep movsd`）|

⇒ 引擎在用"一次拷一个元素"的方式调 `memcpy`（尺寸是运行期变量，MSVC 内联不了）。
真机同一次调用 ≈12 ns（同样低效，但被 5–7 G insn/s 压没了）；我们这边变成 33 条 lifted 指令 +
3 次派发 ⇒ **约 11×**。

## 2. 机制

1. **表**（`aot/intrinsics.txt`）：`pe-sha256 <前缀>` + 每行 `ea / builtin / cc / args / ret`。
   `lift-gen` 读表时**硬校验**：① PE 的 sha256 前缀一致；② 该 EA 必须是**已发射单元的根**。
   任一不满足 ⇒ 构建期 `FATAL`（换 PE 后"闸门静默失效"不可能发生）。
2. **发射**（`lift_gen_emit`）：在单元根函数最前面
   `if (gate) { helper; EAX=dst; resume=mem[esp]; esp+=4; ret i32 resume } else { br 原第一个块 }`。
   闸门与执行是两个函数（`lift_intrinsic_memcpy_gate` / `_do`）：闸门**只读**、可在夹具里单独断言。
3. **闸门**：`n == 0`，或（`n <= 16` ∧ `dst+n ≤ 4 GiB` ∧ `src+n ≤ 4 GiB` ∧ `committed(dst,n)` ∧
   `committed(src,n)`）。`committed` 是页位图 `bit(page)`（≈1–2 ns/页），两段合计 ≈2–4 ns。
   **为什么必须有 `committed`**：未提交页在 lifted 版会在**那条 guest 指令**上抛 guest fault；
   宿主 `memmove` 打在 `PAGE_NOACCESS` 上会变成宿主访问违例（现场是 helper 里的 host 指令）⇒
   fault 归因/恢复会错位。所以它是**等价性前提**，不是保险丝。
4. **helper**：`memmove`（CRT 自己就按重叠方向 `std; rep movsl; cld` ⇒ 等价，不弱于 `memcpy`）、
   不读/写越界、不碰 DF。`EAX=dst` 与 `ESP+=4` 由 IR 写。
5. **开关**：`--no-intrinsics` 只给 A/B 与诊断（同二进制），缺省开；启动一次性打印命中的内建。

## 3. 读数（同二进制 A/B）

| 读数 | 关（原体） | 开（内建） | Δ |
|---|---:|---:|---:|
| 大帧 `n≈590` wall | 409.204 ms | **305.215 ms** | **−25.4%** |
| 大帧 `insns` | 182,898,267 | **151,172,806** | −17.3% |
| `_memcpy` insn/次 | 33.39 | **0.0014** | — |
| `_memcpy` 派发/次 | 3.23 | **1.00** | — |
| `_memcpy` 有效 ns/次 | ≈136 | **≈30** | −105.8 |
| seg2（2.5 s，同调用数归一） | ≈525.9 M insns | **440.6 M** | −16.2% |
| `move11` 窗口的 `_memcpy` | 76,595,519 insns | **3,315** | — |

门禁：`ctest` **138/138**（`lift-intrinsics`，闸门逐条 / 哨兵 / 寄存器 / helper vs `memmove` /
开↔关差分）· `pnpm test` 618 · `pnpm verify` 466 pass / 2 skip。
变异：去掉闸门的 `committed` 检查 ⇒ `lift-intrinsics` 红并点名"未提交页没被 decline"。

## 4. 显式裁决（**这些不是实现细节**）

1. **ECX / EDX 的残留值不复刻**：Windows ABI 里它们是 caller-saved ⇒ 调用方不许在返回后读。
   若某个调用方真读了，那说明它依赖 UB —— 应当作**独立缺陷**登记，而不是让内建去拟合。
2. **用 `memmove` 而不是 `memcpy`**：CRT 的实现本身就在重叠时按方向搬（等价 memmove）；
   用 `memmove` 我们与它一致，且比 C 标准的 `memcpy`（重叠 UB）更宽容。
3. **阈值 16** 是单点常量：n=4 占 99.99%，>16 的回退给 `rep_bulk`/原体处理。调它必须重测（§3 的表）。
4. **v1 只收 `_memcpy`**：`_memset` 同模板但**不随本版收**（先让"表 + 契约 + 闸门回退"这条流水线跑通）。

## 5. 加下一个内建（流程，别跳步）

1. 先**测**：它是不是热点（调用次数 × 单次成本）；不是热点就不收。
2. **定契约**：写清返回值 / 会写哪些内存 / callee-saved / 特殊寄存器（DF…）/ fault 行为；
   凡是不能精确表达的，不进表。
3. **写表**：`aot/intrinsics.txt` 加一行；`lift-gen` 会校验 EA 是单元根。
4. **契约测试**：矩阵（size × 对齐 × 重叠 × 未提交页）+ 开↔关差分（指纹）。
5. **变异**：至少一条（闸门或 helper 的关键判据），实测红。
6. **读数**：大帧 wall + insns + 该函数的 insn/次（用 `--profile` 前后的窗口 insns 折算）。

## 6. 复现

```bash
cd apps/emulator-lift
# 开 / 关 A/B（同一二进制；appdata 用 scripts/clean-appdata.sh 新建并复制 saves/）
out\emulator-lift-aot.exe --present window --present-hidden --game-dir ..\..\links\install ^
  --appdata ..\..\.tmp\intrinsics\appdata-<臂> --skip-logo --audio off --time-limit 400 ^
  --log-level warn --frame-log --profile --profile-slow-frames 150 --plugin ..\..\.tmp\field-bench\m11.js
# 关的那臂加 --no-intrinsics
ctest --test-dir out -R '^lift-intrinsics$' --output-on-failure
```
