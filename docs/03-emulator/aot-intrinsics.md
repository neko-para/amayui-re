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

## 7. 候选测量（2026-11-25）：per-function 计数，以及"派发口径"的盲区

> 需求单 `REQ-01M86VSP8CXH998HTJTD50MEGC`（内建普查的测量前置 M1–M4）·
> 读数：win32 / `dc65c8f` / RelWithDebInfo / shape `split8-O2-lto-abi` / 2026-11-25 03:30–05:00 ·
> 原始日志与复现命令：`.tmp/reveq/REQ-01M86VSP8CXH998HTJTD50MEGC/`（report.md / run.ps1 / logs/）

诊断：`src/aot/call_diag.*`（`LIFT_CALL_DIAG=1`，缺省零输出 / 每派发一次 bool）。候选表用
`.lst` 的 `COLLAPSED FUNCTION` 的 `{EA, 名字, 字节数}`：

| EA | 名字 | 大小 | 点击 `seg2-click2p5` | 装载 `load9` | FELLOW `s1-to-avaro` |
|---|---|---:|---:|---:|---:|
| `0x004E74B0` | `_memcpy`（已内建） | 0x361 | 157 | 22,504 | 226 |
| `0x004E78E0` | `_memcpy_0` | 0x361 | **0** | **9,148** | **0** |
| `0x004E7400` | `_memset` | 0x7A | 1,146 | 3,224 | 511 |
| `0x004E82C0` | `__ftol2_sse` | 0x1C | 393 | **155,122** | 1,428 |
| `0x004E8A19` | `_sprintf_s` | 0x1E | 0 | 0 | 0 |
| `0x004EA11D` | `_strncpy_s` | 0xB5 | 0 | 59 | 0 |
| `0x004E7C41` | `_strcpy_s` | 0x5F | 858 | 7,571 | 2,842 |
| `0x004E8BF1` | `_rand` | 0x21 | 0 | 0 | 0 |
| `0x004F8434` | `_memcmp` | 0x14D2 | 0 | **1** | 0 |

窗口（同一份日志里的 `=== profile window ===`）：点击 `wall 2,511.9 ms / iter 8,628,631 / insns 458,166,480`；
装载 `3,194.0 ms / 19,622,936 / 977,993,613`；FELLOW `s1-to-avaro` `801.1 ms / 7,348,211 / 436,065,186`。
按同窗口的 `lift` / `boundary` 折算（`insns×lift_ns/insn + dispatches×boundary_ns/dispatch`，**不是**逐次实测）：
装载窗口最大的是 `__ftol2_sse` ≈ 3.04 ms，`_memcpy` ≈ 1.44 ms、`_memcpy_0` ≈ 0.38 ms；
点击窗口最大 `_strcpy_s` ≈ 0.118 ms；FELLOW-s1 最大 `_strcpy_s` ≈ 0.274 ms。

### 7.1 这个口径只看到 0.19% 的 `_memcpy` 调用

整跑窗口（`whole-run`，`wall 28,429.7 ms / insns 3,823,688,139`）对账：`_memcpy` 的**派发可见**
calls = **50,099**，而同一次 run 的 `intrinsics: fast+decline` = **26,721,505**（闸门被求值次数 = 真调用数）
⇒ 派发可见占 **0.1875%**。同一窗口 `lift_dyn` = 38,551,874 次（间接 call/jmp 路径），
但它对本表 9 个候选的入口 **0 次**。

原因（发射形状，2026-11-24 的 D2/D3 注记）：立即数 `call` 被发成
`tail call i32 @lift_cc_<owner>(%st, %mem, <目标 EA>, %cpu)`（`emitTailCallTo`）⇒ 目标函数的
`switch`（内建闸门就在那里）照常执行，但**既不回派发循环、也不进 `lift_dyn`**。
⇒ 对**没有闸门**的候选，任何派发侧诊断（本表 + 既有 `memcpy_diag` + `window-dispatch-top`）
都只能给**下界**，且各候选的倍数不同；⛔ 不能把它当调用数，⛔ 不能换算成时间。

### 7.2 对"第 2 个内建收谁"意味着什么

1. 本表（派发口径）**不是**调用数；真调用数由 §7.4 的 `--no-s7` 诊断构建给出（两者差 533× 的那一项就是 `_memcpy`）。
2. ⇒ 判据只能在 §7.4 的真 calls 上取；本表的派发可见值只保留"顺带能看见的那部分"这一层意思。
3. `_memcpy_0` 的两件前提本轮**已核**（只读）：① 与 `0x4E74B0` 逐字节同体——
   PE 偏移 `0xe68b0` vs `0xe6ce0` 各 865 B，94 个 diff 字节全在 47 个 2 字节段里，
   每段是同一地址字段的低 16 位（46 段 +0x430、1 段 rel32 −0x430，目标同为 `0x4F1121`）；
   ② `lift-gen` 硬闸：临时表含 `0x4E74B0` + `0x4E78E0` 跑 `--closure-gate --intrinsics` ⇒ **exit 0**、
   `intrinsics … (2 unit roots)`、`.ll` 里有 `lintr_entry_4E78E0`。
   ⇒ 它**过得了**"逐字节同体 + 单元根"两道闸；量够不够见 §7.4。
4. `_memcmp` 在 `out/aot/lift_table.inc` 里有 `LIFT_AOT_ENTRY(0x004F8434u, lift_cc_4F8434)`（可发射），
   本轮装载窗口也真调到 1 次 ⇒ 普查里"0 个 call 站点 ⇒ 不发射"这句要按**发射形状**重写。

### 7.4 `--no-s7` 诊断构建：真 calls（2026-11-25）

`lift-gen --no-s7`（所有 `call` 回 trampoline，**不改源码**）生成 8 份 `.ll`，用产物同一套 obj/lib 链一份
诊断 exe（`.tmp/reveq/REQ-01M86VSP8CXH998HTJTD50MEGC/nos7/`；`lift_table.inc` / `lift_pin.h` 与产物逐字节相同）。
**开关生效的判据**：该 run 的 `lift_dyn = 0`（产物档同一场景 38,551,874）。

| 窗口 | `_memcpy` | `_memcpy_0` | `_memset` | `__ftol2_sse` | `_strcpy_s` |
|---|---:|---:|---:|---:|---:|
| 点击 `seg2-click2p5`（3 趟） | **2,559,742 / 2,559,744 / 2,559,744** | **63** | 1,218 / 1,222 / 1,222 | 393 | 858 |
| 装载 `load9` | **9,117,397** | **13,244** | 3,573 | 154,805 | 7,571 |
| FELLOW `s1-to-avaro` | **66,274** | **0** | 526 | 1,428 | 2,842 |

**对账**（整跑窗口）：`_memcpy` 派发计数 **26,721,353** vs 同一 run 的 `fast+decline` **26,721,503**
⇒ 差 **150（0.00056%）** ⇒ 诊断构建上"派发到该 EA" ≡ 真调用数。`_memcpy` 点击窗 `avg_n=4.36`
（11-24 seg2 是 4.39）⇒ 同一类窗口被复现。

**判定**（`ns/次` 是估价 = `insn/次 × 窗口 lift_ns/insn` + 该窗口 `boundary_ns/dispatch`；`_memcpy_0` 另给评测单的
33.39 insn / ≈136 ns 上界）：点击线上除已内建的 `_memcpy`（≈119 ms）外最大是 `_strcpy_s` ≈145 µs；
装载线上最大是 `__ftol2_sse` ≈3.0 ms、`_strcpy_s` ≈1.7 ms、`_memcpy_0` ≈0.68 ms（136 ns 上界 ⇒ 1.80 ms）、
`_memset` ≈0.21 ms ⇒ **没有第二个候选过线**（`_memcpy_0` 差 ≥11×）。⇒ **本阶段不落第二个内建**
（`aot/intrinsics.txt` 保持 1 行）。若将来某窗口 `_memcpy_0` 的 calls 涨到 ≥147 K（装载）/ ≥37 K（点击），
最小落表 = 表加一行 + `tests/ir/intrinsics_roots.txt` 加 `0x004E78E0` + 复用现有契约矩阵。

### 7.5 怎么复跑


```powershell
cd apps/emulator-lift
$env:LIFT_CALL_DIAG='1'; $env:LIFT_CALL_DIAG_WINDOW='*'
# 点击 / 装载 / FELLOW / 整跑对账 四个插件在 .tmp/reveq/REQ-01M86VSP8CXH998HTJTD50MEGC/
out\emulator-lift-aot.exe --game-dir ..\..\links\install --appdata <私有> --present headless `
  --skip-logo --audio off --time-limit 60 --log-level warn --profile --plugin <插件>
```

⚠ 已知跑不到：FELLOW 的 6 个窗口只拿到 1 个 —— 每收一个窗口的 `ctx.profile.end()` 要写系统剪贴板，
该回调的用户 JS CPU 2.56–2.90 ms > 2 ms 预算 ⇒ `run failed`（4/4 趟都停在 `scenario:settle:0`）。
其余 5 个窗口是**没测到**，不是 0 次；原因与读数见 `.tmp/reveq/REQ-01M86VSP8CXH998HTJTD50MEGC/report.md` §未做。

