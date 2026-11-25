# AOT LTO 评估（A / B0 / B1 / B2 / C，opt-in 实验）

> ⚠ **本节结论已被 §10 取代（2026-11-17）**：LTO 现在是 mac 的默认形状（`-DLIFT_AOT_LTO=OFF` 回退）；下面的读数保留为历史快照。

- 仓库 `/Users/nekosu/Documents/Projects/amayui-re`（macOS arm64，一仓一树）· 起点 `9468226`（工作树初始干净）
- 机器：12 核 / 36 GB · Apple clang 17.0.0（`/usr/bin/clang` 与 `/usr/bin/c++` 同一份）
- 构建：Ninja · RelWithDebInfo · vcpkg 复用 `apps/emulator-lift/out/vcpkg_installed`（⛔ 未重新 install）
- ⚠ **墙钟读数的背景负载**：实验期间机器上有 LSP 索引等背景负载（父级已告知）⇒ **静态读数是主判据**；
  所有墙钟都按"交错 A/B、取中位数"采，并在每处标注"可能被背景负载污染"。
- ⛔ **默认档未改**：`CMakeLists.txt` 已回退到 HEAD 的 `-c -O2` + `-flto` 硬闸；本文 §2 描述的 opt-in
  补丁**没有入库**，留档在 `.tmp/lto/opt-in.patch`（要复现实验就按它重加；⛔ 不要把它当默认形状）。
- ⛔ `out/` 未改（`out/build.ninja` 仍是 12:10 的 mtime、`grep -c -- -flto` = 0）；⛔ 未碰 `.tmp/sc0000/`。

---

## 0. TL;DR（一句话结论 + 六条读数）

1. **现状确认**：LTO 是关的（`set(LIFT_AOT_CLANG_FLAGS -c -O2)`；configure 期硬闸把 `-flto` 列为禁止项；
   `out/build.ninja` / `out/compile_commands.json` 里 `-flto` 计数 = **0**；`LIFT_AOT_SHAPE=\"split8-O2\"`）。
   `lift_tick` / `lift_note_write` / `lift_dyn` 都是**真调用**：`bl` 站点 **101,765 / 38,535 / 5,658**。
2. **LTO 今天什么也没买到**：B0（8 份 IR + link `-flto`）、B1（+`lift_abi.cpp`）、B2（+整个 runtime）的
   `__text` 相对"同口径去死代码"的 A 对照只有 **+0.12% / +0.12% / +2.08%**；tick/note/dyn 的站点数
   **一条没少**（102,595 / 38,756 / 5,674，与 B0 完全相同）。**唯一被内联的是跨 part 的 `lift_cc_*`**：
   站点 2,267 → **1,543**（−724，32%），而且内联是**净增代码**（lift 代码 +0.17%）。
3. **为什么 tick/note/dyn 内联不掉（已实测定因）**：AOT IR 的函数**没有** `target-cpu` / `target-features`
   **函数属性**，而 clang 的 C++ 前端会把 `apple-m1` 写进运行库 TU（`lift_abi.cpp` / 所有 shim）的每个函数
   属性 ⇒ LLVM 的 `areInlineCompatible()` 判“运行库 → AOT”不相容 ⇒ **一条都不内联**。★ 这是**属性**
   不对等，**不是** codegen 的 feature 不同：`clang -###` 对 `.ll` 与 C++ 的 cc1 行逐项相同（都是
   `-target-cpu apple-m1` + 同一串 feature）；mac 上 IR 的 codegen 并没有 feature 缺失。证据见 §5.2
   （离线 `llvm-link`+`opt`：原样 17,544 → 17,561；把两侧属性都删掉 → **3,086**；补属性后 `.obj` 与默认逐字节相同）。
4. **代价**：`-flto` 链接 = **29.1 s（2.40 GB 峰值）**（B0）/ **45.0 s（4.25 GB）**（B2），而今天的常规链接
   = **0.21 s（247 MB）**。C（single LL）没有 LTO 链接，但 clang 段从 7.35 s 涨到 **38.9 s（2.67 GB）**。
5. **C（single LL）与 B0 等价**：跨 part 内联量完全相同（liftCc 2,267 → 1,543），tick/note/dyn 照样跨 TU；
   `__text` 比 A 大 **+0.16%**，构建更慢。⇒ **"改回 single LL"不划算**。
6. **诊断档 B1x（把运行库的 feature 注进 8 份 IR）**：tick 站点 **102,595 → 13,893（−86%）**、
   note_write **38,756 → 0（−100%）**；但 `__text` **+3.1%**、lift 代码 +7.6%；
   固定 guest 工作量（`--max-insns 4e8` / `8e8`）的墙钟 **−8% / −4.6%**（mac，交错取中位）。
   ⇒ LTO 真正能买到的东西（tick/note 内联）**要先把函数属性补上**（值由 CMake 传入，⛔ 不写死 apple-m1），
   而且**静态是变大的、动态是变快的**。

**建议：默认不改（保持 §12.8 的 `split8 -c -O2`）。** LTO 今天零收益、纯成本；C 也不划算。
只有"LTO + lift-gen 发射 target-cpu/target-features **函数属性**"这条路能拿到 mac 上 5–8% 墙钟，
代价是 +3% 代码、+36 s 链接、3 GB RSS，且要动 AGENTS §12.8 的冻结裁决（详见 §8）。
★ 注意：这不是“启用 mac 上原本没用到的 CPU feature”（mac 上 feature 本来就在用），而是“让 LTO 内联器
认得出两侧是同一个 target”；win x64 的 AVX/SSE4 才是真正的“要不要提高 feature 基线”裁决。

---

## 1. A：确认现状（命令 + 读数）

```bash
# ① 关闭的 LTO（HEAD 的原文）
$ git show HEAD:apps/emulator-lift/CMakeLists.txt | sed -n '302p;313p;326,336p'
#   set(LIFT_AOT_CLANG_FLAGS -c -O2)
#   #   ② 出现 `-O1` / `-O0` / `-Os` / `-Oz` / `-flto` / `llc` ⇒ 直接失败；
#   if(NOT "${LIFT_AOT_CLANG_FLAGS}" STREQUAL "-c;-O2")  → FATAL_ERROR
#   foreach(_bad IN ITEMS -O0 -O1 -Os -Oz -flto -flto=thin llc) → FATAL_ERROR

# ② 产物里确实没有 LTO
$ grep -c -- -flto apps/emulator-lift/out/build.ninja              # 0
$ grep -c -- -flto apps/emulator-lift/out/compile_commands.json    # 0
$ grep -o 'LIFT_AOT_SHAPE=\\"[^"]*\\"' apps/emulator-lift/out/build.ninja | sort -u
#   LIFT_AOT_SHAPE=\"split8-O2\"

# ③ tick / note / dyn 仍是真调用（`bl` 站点）
$ LLVM=/opt/homebrew/opt/llvm/bin
$ $LLVM/llvm-objdump -d --no-show-raw-insn out/emulator-lift-aot | grep -c 'bl.*_lift_tick'         # 101765
$ $LLVM/llvm-objdump -d --no-show-raw-insn out/emulator-lift-aot | grep -c 'bl.*_lift_note_write'   # 38535
$ $LLVM/llvm-objdump -d --no-show-raw-insn out/emulator-lift-aot | grep -cE '[[:space:]]b[l]?[[:space:]].*_lift_dyn'   # 5,658（`bl` 5,080 + 尾调用 `b` 579）
$ $LLVM/llvm-nm -n --defined-only out/emulator-lift-aot | grep -E ' _lift_(tick|note_write|dyn)$'
#   0000000100b58868 T _lift_tick   /   0000000100b58854 T _lift_note_write   /   0000000100b59368 T _lift_dyn
```

**这不是 bug**：`-flto` 禁令是 **AGENTS §12.8 第 3 条**的冻结裁决（8 份 `-c -O2`，为了把 clang 段
120.7 s 压到 21.6 s），HEAD 那份 `CMakeLists.txt` 的注释明写"把 -O2 降级 / 换 LTO 是唯一能让拆分构建
在墙钟上好看却在运行期变慢的自欺路径"。本轮只是按用户要求做 **opt-in 实验**，默认档一字未动（§2 有证据）。

---

## 2. 改了什么（opt-in，默认 OFF 逐字节不变）

`apps/emulator-lift/CMakeLists.txt` 新增三个 `option`（`LIFT_AOT_LTO` / `_RUNTIME` 默认 **OFF**；`_ABI` 默认 ON 但**只在 `LTO=ON` 时才接线**）：

| 选项 | 默认 | 语义 |
|---|---|---|
| `LIFT_AOT_LTO` | **OFF** | ON ⇒ `LIFT_AOT_CLANG_FLAGS` 变 `-c -O2 -flto`；硬闸放行 `-flto`（仍拒 `-O0/-O1/-Os/-Oz/llc`）；`emulator-lift-aot` 链接行加 `-flto`；shape 带 `-lto` |
| `LIFT_AOT_LTO_ABI` | ON（仅 `LTO=ON` 时有意义） | `src/runtime/lift_abi.cpp` 也编 bitcode |
| `LIFT_AOT_LTO_RUNTIME` | OFF | 整个 `emulator-lift-runtime`（含 shim）都编 bitcode；`-flto` 用 `INTERFACE` 传给所有链它的 target |

shape 变成**可辨识**的：`split8-O2-lto-parts`（只有 8 份）/ `split8-O2-lto-abi`（+`lift_abi.cpp`）/
`split8-O2-lto-abi-rt`（+整个 runtime）。**OFF 档不产生任何后缀**。

**默认 OFF 逐字节不变的证据**（两道）：

1. 同一份源码、默认 OFF 重新 configure（`.tmp/lto/build-a`）：`grep -c -- -flto build.ninja` = **0**，
   shape = **`split8-O2`**；AOT 主规则段（8 条 `clang -c -O2` 命令 + 生成命令 + 链接行 + DEFINES）与
   `out/build.ninja` 逐行 diff **只有 vcpkg 路径形态不同**（`out/` 里是相对 `vcpkg_installed/…`，新树指到
   `out/vcpkg_installed` 的绝对路径），其余**完全一致**。
2. 这棵树今天完整构建出来的二进制与 `out/emulator-lift-aot` **读数逐项相同**：
   `__text 14,399,532` · 总 native 3,608,111 · lift 2,500,533 · tick 101,765 · note 38,535 · dyn 5,658 · liftCc 2,267。
   （`out/` 自己没有重建：mtime 仍是 12:10/12:11，`-flto` 计数仍是 0。）

---

## 3. 构建树与耗时（全部一次性实验树，⛔ 不进常规轮次）

| 档 | 树 | 形状 | ① runtime+gen | ② .ll 生成 | ③ clang 段 | ④ 链接 | 合计 | 链接峰值 RSS |
|---|---|---|---|---|---|---|---|---|
| **A** 基线（新树复刻） | `.tmp/lto/build-a` | `split8-O2` | 27.0 s | 15.7 s | **7.35 s**（8×`-c -O2`，并行） | **0.21 s** | ≈ 52.5 s | 247 MB |
| **B0** 8 份 + link LTO | `.tmp/lto/build-b0` | `split8-O2-lto-parts` | 45.6 s\* | 15.7 s | 4.39 s（只发 bitcode） | **29.05 s** | ≈ 99 s | **2.40 GB** |
| **B1** +`lift_abi.cpp` | `.tmp/lto/build-b0`（增量） | `split8-O2-lto-abi` | —（1 TU 重编） | — | — | ≈ 30 s | 增量 50 s | ≈ 2.4 GB |
| **B2** +整个 runtime | `.tmp/lto/build-b2` | `split8-O2-lto-abi-rt` | 24.7 s | 17.2 s | 3.99 s | **44.97 s** | ≈ 93 s | **4.25 GB** |
| **C** single LL | `.tmp/lto/build-c` | `single-O2` | 25.7 s | 15.0 s | **38.92 s**（1 份 134 MB `.ll`） | 0.15 s | ≈ 82 s | 2.67 GB（clang） |
| **B1x**（诊断档，§5.3） | `.tmp/lto/fixedfeat` | 手工（B1 链接行） | — | — | 6.23 s | 36.63 s | ≈ 43 s（增量） | 3.04 GB |

\* B0 的 `gen` 45.6 s 与其它树的 24.7–27.0 s 是**同一条命令**的波动（背景负载），所以合计只作量级参考。
★ 与 `docs/03-emulator/aot-instruction-budget.md` 的 21.6 s 对账：本报告 ① 里的 `.ll 生成 15.7 s + clang 7.35 s ≈ 23.0 s`
——**那个 21.6 s 的口径是"生成 + 8 份 clang"**，今天复现得到 23.0 s（同一台机器、同一天，背景负载下）。
★ 单份 `.ll` 的 clang 段今天实测 **38.9 s**，**不是**设计文档里的 120.7 s（那个读数在更早的树/更高负载下取的，
本轮复现不出来；`__text` 与 8 份一致说明产物形状没变）。

`out/` 的链接时间也单独复测过（用 `out/` 自己的对象、把 `-o`/`-map` 改到 `.tmp`）：**0.21 s**，且产物与
`out/emulator-lift-aot` 的 `__text`/指令数逐项相同 ⇒ 下面的 A 读数与 `out/` 是同一份东西。

---

## 4. 静态对比（主判据）

口径（同 `§1.2`，对象换成**最终二进制**；脚本 `.tmp/lto/measure.mjs` + `summarize.mjs`）：
`__text` = `llvm-size` 的 Section `__text`；native 条数 = `llvm-objdump -d` 的指令行数；
`lift` = 落在 `_lift_cc_*` 符号区间（`[addr, 下一个 T/t 符号)`）里的指令；`runtime` = 名字出现在
`libemulator-lift-runtime.a` 里的符号；`other` = 第三方 + libc++。

| 档 | `__text` | 总 native | lift | runtime/shim | other | tick | note | dyn | liftCc |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| **A**（= `out/`） | 14,399,532 | 3,608,111 | 2,500,533 | 440,886 | 658,464 | 101,765 | 38,535 | 5,658 | 2,267 |
| **A-ds**（对照：只加 `-dead_strip`） | 14,095,848 | 3,532,166 | 2,500,533 | 415,220 | 608,209 | 101,765 | 38,535 | 5,658 | 2,267 |
| **B0**（8 份 LTO） | 14,112,480 | 3,536,324 | 2,504,689 | 415,220 | 608,211 | 102,595 | 38,756 | 5,674 | **1,543** |
| **B1**（+abi） | 14,112,380 | 3,536,299 | 2,504,676 | 415,210 | 608,209 | 102,595 | 38,756 | 5,674 | **1,543** |
| **B2**（+runtime） | 14,389,236 | 3,605,495 | 2,504,676 | **481,116** | 611,517 | 102,595 | 38,756 | 5,674 | **1,543** |
| **C**（single LL） | 14,422,124 | 3,613,759 | 2,506,181 | 440,886 | 658,464 | 102,611 | 38,758 | 5,674 | **1,543** |
| **B1x**（诊断档：feature 修好） | 14,852,364 | 3,721,295 | **2,689,677** | 415,205 | 608,209 | **13,893** | **0** | 5,678 | 1,653 |

相对 A 的变化（`__text` / 总 native）：

| 档 | `__text` Δ | 总 native Δ | 关键内联 |
|---|---:|---:|---|
| B0 | −287,052（−1.99%） | −71,787（−1.99%） | 跨 part `lift_cc` −724 站点；lift 代码 **+0.17%** |
| B1 | −287,152（−1.99%） | −71,812 | 与 B0 **逐项相同**（`__text` 只差 100 B）⇒ **给 `lift_abi.cpp` 加 LTO 完全无效** |
| B2 | −10,296（−0.07%） | −2,616 | runtime 桶 **+65,896（+15.9%）**：shim↔shim 内联是净增代码；tick/note/dyn 站点**不变** |
| C | **+22,592（+0.16%）** | +5,648 | 跨 part 内联与 B0 完全相同（2,267→1,543）；tick/note/dyn 跨 TU，站点 +846/+223/+16 |
| B1x | **+452,832（+3.14%）** | +113,184（+3.14%） | tick −88,702；note −38,756（符号被删）；dyn 不动 |

★ **重要口径提醒（别被−2%骗了）**：`-flto` 链接会让 **ld64 隐式做死代码剥离**（同一条链接行加
`-Wl,-dead_strip` 就得到 A-ds：−303,684 B / −75,945 条，全部是 BZ2/PNG/SDL/`LstIndex` 等**与本议题无关**的
静态库函数）。所以 A→B0 的 −2% **不是 LTO 的功劳**；**纯 LTO 的效应 = B0 vs A-ds = `__text` +16,632 B /
总 native +4,158 条（+0.12%）**——**LTO 让代码略微变大**。
（C 没有 `-flto`，所以它跟 A 同为"未死剥"档，可直接比：+22,592 B / +5,648 条。）

---

## 5. 内联证据（谁被内联、谁没有、为什么）

### 5.1 站点级读数

- **跨 part `lift_cc_*`：被内联。** 2,267 → 1,543（B0/B1/B2/C 都是 1,543；B1x 1,653）。
  但**每个被内联的块体是复制进调用者的**（被调函数仍被派发表取地址 ⇒ 不能删），所以 lift 代码
  反而 **+4,156 条**，tick/note 站点也因复制而 **+830 / +221**。⇒ 这是"为省一次 `bl` 复制一整个块体"，
  静态上是亏的。
- **`lift_tick` / `lift_note_write`：在 B0/B1/B2/C 里一条都没内联**（站点 102,595 / 38,756 与 B0 完全一致；
  `_lift_tick` 在 B1/B2 里只是从 `T` 变成 `t`——被 internalize，但**没有被内联**）。
- **`lift_dyn`：任何档都没内联**（5,658 → 5,674/5,678；它是 `musttail` 尾调用 + 查表的派发器，
  内联价值与可行性都低）。
- **shim 面**：AOT IR 里**没有对 shim 的直接调用**——只有 5 个运行库符号：`lift_tick` / `lift_note_write` /
  `lift_dyn` / `lift_stop` / `lift_x87_sem`；shim 走 `lift_dyn` + 派发表。所以"跨 IR 和 shim 内联"实际
  等价于"**能不能把 `lift_dyn`（和它的被调）内联进 AOT**"⇒ 答案是不能（同上）。B2 里确实发生了
  **runtime 内部**的 LTO（runtime 桶 +65,896 条、调用站点重排：`_SDL_SetError_REAL` 2,090、
  `lift::guestFault` 1,091、`___clang_call_terminate` 1,007 等），但**没有一条运行库函数进 AOT**。

### 5.2 定因：`areInlineCompatible()` 被**函数级属性**挡住（离线复现）

★ **校正（2026-11-17 复核）**：这里说的“feature 不匹配”是 **LLVM 函数属性**层面，**不是 codegen 的
target feature**。`clang -###` 对 `.ll` 与 C++ 的 cc1 行**逐项相同**（`-target-cpu apple-m1` + 同一 27 个
显式 feature）⇒ mac 上 AOT IR 的 codegen 与 runtime/shim **是同一份 clang 默认**，没有“shim 有、IR 没有”
的 feature 缺失。差异只在：`lift-gen` 发射的 `.ll` 函数**没有** `"target-cpu"/"target-features"` 属性，
而 clang 的 C++ 前端会把 `apple-m1` 写进每个函数属性；`areInlineCompatible` 比较属性 ⇒ 拒绝内联。
- `-mcpu=apple-m1` 对 `.ll` 无效：默认已经是 apple-m1（该 flag 实际映射到 `apple-a14`，产物逐字节相同）。
- 给 part00 的每个 `define` 补属性后，`.obj` 与默认**逐字节相同** ⇒ 属性只影响 LTO 内联，不改 codegen。
- **mac 量化**：`-mcpu=generic` 关掉 feature ⇒ `__text` −53,912 B(−0.37%)、lifted NEON **7,745→3,709
  (−52%)** ⇒ feature 确实在用；60 帧指纹逐位相同。
- **win x64**：IR 与 C++ 默认都是 `-target-cpu x86-64`、0 feature（SSE2）；8 份全编 `+AVX2` −0.40% 条、
  VEX 115,206、`%ymm` 8,892、`.text` −8,915 B。<待核：win 运行机 feature 集>（无 win 环境，未实测）。

```bash
$ LLVM=/opt/homebrew/opt/llvm/bin
# 8 份 .ll 是"目标无关 IR"：函数上没有任何 target 属性
$ grep -m1 -A2 "^define i32 @lift_cc_" .tmp/lto/build-b0/aot/lift_aot.part00.ll
#   define i32 @lift_cc_401000(ptr %st, ptr %mem, i32 %ea, ptr %cpu) {      ← 无 attributes
# 运行库 bitcode 的每个函数都带 apple-m1 的 feature
$ $LLVM/llvm-dis .tmp/lto/build-b0/CMakeFiles/emulator-lift-runtime.dir/src/runtime/lift_abi.cpp.o -o /tmp/lift_abi.ll
$ grep -m1 -oE '"target-cpu"="[^"]*"' /tmp/lift_abi.ll        # "target-cpu"="apple-m1"
$ grep -m1 -oE '"target-features"="[^"]*"' /tmp/lift_abi.ll    # "+aes,+altnzcv,…,+v8a,+zcm,+zcz"

# 把 part00 与 lift_abi 合起来，跑一次 inline：
$ $LLVM/llvm-link part00.ll /tmp/lift_abi.ll -o /tmp/m.bc && $LLVM/opt -passes=inline /tmp/m.bc -o /tmp/m2.bc
$ $LLVM/llvm-dis /tmp/m2.bc -o /tmp/m2.ll && grep -c 'call void @lift_tick' /tmp/m2.ll
#   17,544（原地不动；内联别的函数后反而 17,561）
# 再把两侧的 target-cpu / target-features 属性全删掉（sed 掉那两个属性字符串）后同样处理：
#   17,544 → 3,086        ← 82% 的 tick 调用立刻被内联
```

⇒ **机械原因是函数属性不对等**（LLVM 的 `areInlineCompatible` 要求 callee 的 feature 是 caller 的
子集；“无属性” vs “+apple-m1” 判失败），**不是** codegen 的 target feature 不同（§5.2 开头的校正）。
这也解释了为什么 B0/B1/B2 三个档的 tick 站点**一分不差**：三种 LTO 配置都过不了这道闸。补
`-mcpu=apple-m1` 到 AOT 的 clang 命令**没用**——实测 `clang -c -O2 -flto -mcpu=apple-m1` 对 `.ll`
输入**不会**给函数加 target 属性（B1f 档与 B1 逐项相同）。要修只能**让 lift-gen 在 IR 里发射这两个
函数属性**（或构建期后处理 `.ll`；属性值由 CMake 传入，⛔ 不要写死 apple-m1）。

### 5.3 诊断档 B1x：把运行库的 feature 注进 8 份 IR 之后

做法（纯 `.tmp`，⛔ 不改 CMake/源码，见 `.tmp/lto/fixedfeat-lto.sh`）：把
`"target-cpu"="apple-m1" "target-features"="+aes,…,+zcz"` 作为 `attributes #99` 加到每份 `.ll` 的**每个
函数定义**上，其余与 B1 完全一样（8 份 + `lift_abi.cpp` bitcode、链接 `-flto`）。

结果：**tick 站点 102,595 → 13,893（−86%）**、**`lift_note_write` 38,756 → 0（符号被删）**、
`_lift_tick` 仍留 13,893 个站点（大函数里没内联的那部分）；`lift_dyn` 仍 5,678（不动）。
代价：`__text` **+3.14%**、lift 代码 **+7.6%**（每处内联的是"空指针检查 + 取 0x6d8 + 加 + 存"，
比原来的"2 条取参 + bl"多 1–2 条）。
⇒ **tick 内联在静态上是亏的、在动态上是赚的**（见 §7）。

---

## 6. 语义指纹（A/B0/B1/B2/C/B1x 逐位相同）

命令（`.tmp/lto/run-fingerprint.mjs`，每个档**各自全新** `--appdata`）：

```bash
node .tmp/lto/run-fingerprint.mjs <bin> .tmp/lto/appdata/<档> /tmp/run-<档>.txt --frames 60
#   = --present headless --skip-logo --max-frames 60 --audio off --game-dir links/install --appdata <新目录>
```

| 档 | exit | frames | stop | arena（in_use / peak / surfaces / heap_blocks） | table misses |
|---|---:|---:|---|---|---:|
| A（`out/`） | 0 | 60 | `stopped @ eip=00498AE4`（max-frames · shim com!IDirect3DDevice9::Present） | 108595152 / 108595152 / 21 / 6670 | 0 |
| B0 | 0 | 60 | 同上 `00498AE4` | 同上 | 0 |
| B1 | 0 | 60 | 同上 | 同上 | 0 |
| B2 | 0 | 60 | 同上 | 同上 | 0 |
| C | 0 | 60 | 同上 | 同上 | 0 |
| B1x | 0 | 60 | 同上 | 同上 | 0 |

★ `insns` / `lookups` **不是**判据（实测同一档跨 run 也会变：A 784,198,286 vs B0 786,219,273 vs B1
783,402,820 vs C 783,905,002，−0.36%…+0.26% —— guest 循环吃墙钟，指令数随帧率漂）。表里只列
**稳定**的五项。
⚠ **局限**：60 帧 ≈ 7.8 亿 guest 指令、4,200 万次派发、33,509 个派发表项，覆盖的是"能跑到第一个
`Present` 计数 60"的那部分块（闭包里 10,254 个函数只有一部分被执行）；**没覆盖**音频、窗口/真 GPU 呈现、
存档写盘等面。逐块等价性不能由这个场景证明，只能说"可观测的语义指纹逐位相同"。

---

## 7. 动态（mac，⚠ 背景负载下交错采集，不作 win 判据）

固定 guest 工作量（`--max-insns`，工作量和**是否内联无关**：`lift_tick` 的记账语义不变）：

| 工作量 | A（中位） | B1（中位） | B1x（中位） | 读法 |
|---|---:|---:|---:|---|
| 4×10⁸ guest insns（启动段，0 帧），5 对交错 | **0.75 s**（0.74–0.81） | 0.78 s（3 对） | **0.69 s**（0.68–0.70） | B1 ≈ A（没内联 ⇒ 没变化）；B1x **−8%** |
| 8×10⁸ guest insns（≈95 帧），3 对交错 | 2.17 s（2.16–2.28） | — | **2.07 s**（2.06–2.09） | B1x **−4.6%** |

⇒ 与静态相反：**tick/note 内联换来的是动态收益**（去掉 ~90k 个 `bl` + 参数装配 + `ret`）。
⚠ 这是 **mac / arm64 / 12 核带背景负载**的读数；⛔ 不能当 win 侧判据（AGENTS §12.6：这台机器分辨不了 <5%，
而这里的 4.6% 已经在门槛附近）。要拿它当判据必须在 win 树、独占窗口、≥3 对交错复测。

---

## 8. 结论与建议

### 8.1 结论

1. **LTO 今天没有价值**：B0/B1/B2 相对 A 的 `__text` 收益全部来自 ld64 的隐式死代码剥离（与 LTO 无关）；
   纯 LTO 效应是 **+0.12% 代码**，且 `-flto` 链接比常规链接慢 **140–210 倍**（0.21 s → 29–45 s）、
   峰值内存 **2.4–4.3 GB**（常规 250 MB）。**tick / note / dyn / shim 一条都没被内联**。
2. **给 `lift_abi.cpp` 单独加 `-flto`（B1）是纯无效**：与 B0 逐项相同（`__text` 只差 100 B）。
   它确实被 LTO 了（`_lift_tick` 从 `T` 变 `t`），但 feature 闸门让内联不发生。
3. **整库 LTO（B2）反而让 runtime/shim 变大 15.9%**（shim↔shim 内联的复制），`__text` 比 A 只小 0.07%。
4. **C（single LL）不划算**：跨 part 内联量与 B0 相同（2,267→1,543），但 clang 段 7.35 s → **38.9 s**、
   `__text` **+0.16%**；它避开的只是 29 s 的 LTO 链接，却又把 clang 段加回去 31 s。而且它**同样**
   内联不到 tick/note/dyn（它们仍跨 TU）。⇒ **不要为省 LTO 而改回 single LL**。
5. **唯一有真实收益的组合是 "LTO + IR 带 target 属性"（B1x）**：tick −86%、note −100% 站点，
   静态 +3.1%、动态 −4.6…−8%（mac）。这不是"LTO 本身"的功劳，而是"**把运行库变成可内联的 IR**"的功劳。

### 8.2 建议（按推荐顺序）

1. **默认保持 OFF（A）**。LTO 现状零收益、纯成本；C 也不划算。⛔ 不要为了"看起来更快"改默认。
2. 如果目标是那 5–8% 的动态收益，**最便宜的路不是 LTO，而是让 tick/note 的记账直接在 IR 里**：
   由 lift-gen 把 `%cpu->insns += n` 发成 `getelementptr/load/add/store`（本来就是抬升代码的常规做法），
   顺带把 `lift_note_write` 的调用保持为真调用。这样**不需要 LTO**、不增加 29 s 链接、不影响 §12.8 的
   拆分裁决。⚠ 这条有语义风险（记账逻辑出现第二份实现 ⇒ 必须由"生成物与运行库同一份语义真源"的
   守卫钉住，类似 `lift_abi` 的契约测试）；**本轮未实测**，只是 §5.3 读数指向的下一步。
3. 如果非要走 LTO：需要**两条裁决**——
   - **AGENTS §12.8 第 3 条**要改成"默认 `-c -O2`；LTO 是 opt-in 实验档"（本轮的 `LIFT_AOT_LTO` 已经是
     这条的机制，但"默认值"还在 §12.8 的冻结里）；
   - **lift-gen 必须发射 `"target-cpu"/"target-features"`**（与运行库 TU 一致），否则 LTO 永远内联不到
     运行库。⚠ 这条在 win32 上要发 `x86_64` 的 features（不同 CPU 的 feature 集不同 ⇒ 要绑 `-mcpu` 或
     走"运行时同源"的策略），风险比 mac 高。
   - 代价要接受：`-flto` 链接 ~30–45 s / 2.4–4.3 GB（每次改 AOT IR 都要付），`__text` **+3%**。

### 8.3 风险 / 待核

- 本报告只在 **mac / arm64 / Apple clang 17** 上做；win 树（clang-cl + link.exe）**未验证**（`-flto` 在
  win32 上很可能不工作，代码里已加 `message(WARNING)` 提示，⛔ 没有放宽 OFF 档的闸）。
- 60 帧场景的覆盖有限（§6 已列）。
- `insns` / `lookups` 有 ±0.2–0.4% 抖动（guest 吃墙钟），**不能**当判据；墙钟读数有背景负载。
- B1x 是**手工诊断档**（`.tmp` 里注入属性），不是 CMake 档；它的 shape 打印仍是 `split8-O2-lto-abi`
  （因为它复用 B1 的编译宏），别拿它当"已落地的档"。
- `lift_dyn` 为什么在内联可行的情况下也不内联（`musttail` + 查表 + 间接调用）：**未单独定因**，留给后续。

---

## 9. 复现（脚本 / 日志 / 读数都在 `.tmp/lto/`）

| 文件 | 作用 |
|---|---|
| `.tmp/lto/measure.mjs` | 单二进制：`__text` / 总 native / lift 区间 / 各类调用站点 |
| `.tmp/lto/summarize.mjs` | 多二进制同口径三桶表（lift / runtime / other） |
| `.tmp/lto/symdiff.mjs` | 两二进制按符号区间的差（首轮 B0 分析用） |
| `.tmp/lto/run-build.sh` | 分步构建 + 计时（`/usr/bin/time -lp`），`split` / `single` 两模式 |
| `.tmp/lto/relink.sh` | 只跑链接命令（拿链接真墙钟 + 峰值 RSS） |
| `.tmp/lto/tuned-lto.sh` / `.tmp/lto/fixedfeat-lto.sh` | B1f（`-mcpu`）/ B1x（注入 target 属性）诊断档 |
| `.tmp/lto/run-fingerprint.mjs` | 短 run 语义指纹 |
| `.tmp/lto/ab-timing.sh` + `ab-timing.txt` | 固定 `--max-insns` 的 A/B 墙钟（5 对 / 3 对，交错） |
| `.tmp/lto/log-a.txt` / `log-b0.txt` / `log-b2.txt` / `log-c.txt` / `log-b1x.txt` | 各档分步日志（含 `real` / `maximum resident set size`） |
| `.tmp/lto/{a,b0,b1,b2,c,b1f,b1x}.json` / `table*.md` | 读数（JSON + 表格） |

构建树：`.tmp/lto/build-a`（A，默认 OFF 对照）· `.tmp/lto/build-b0`（B0，后改成 B1）· `.tmp/lto/build-b2` ·
`.tmp/lto/build-c` · `.tmp/lto/tuned`（B1f）· `.tmp/lto/fixedfeat`（B1x）。
⚠ 父级先前看到过 `.tmp/lto/build-b1/CMakeCache.txt`：那棵树**先被重命名为 build-b0**（CMake 树不能改名，
所以 B0 是新建的 `build-b0`；B1 是在 `build-b0` 上 `-DLIFT_AOT_LTO_ABI=ON` 增量重配的）。

configure 命令（以 B1 为例；其余只改 `-DLIFT_AOT_LTO*` / `-DLIFT_AOT_SPLIT`）：

```bash
cmake -S apps/emulator-lift -B .tmp/lto/build-b0 -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/apps/vcpkg/scripts/buildsystems/vcpkg.cmake" \
  -DVCPKG_TARGET_TRIPLET=arm64-osx -DVCPKG_MANIFEST_MODE=ON \
  -DVCPKG_MANIFEST_DIR="$PWD/apps/emulator-lift" \
  -DVCPKG_INSTALLED_DIR="$PWD/apps/emulator-lift/out/vcpkg_installed" \
  -DVCPKG_MANIFEST_INSTALL=OFF -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DLIFT_BUILD_TESTS=OFF -DLIFT_AOT_LTO=ON -DLIFT_AOT_LTO_ABI=ON
```

---

## 10. 2026-11-17 复核：IR 属性对齐后 LTO 默认开启（mac）

> 口径：`/Users/nekosu/Documents/Projects/amayui-re` · macOS arm64（Apple M1 / 12 核 / 36 GB）·
> Apple clang 17.0.0 · Ninja · RelWithDebInfo · 起点 `253cbe0`（本单 = 把 opt-in 落成默认）·
> A/B 交错、每 run 全新 appdata、同机同天。
> ⚠ 下面所有数字都是 **2026-11-17 的一次实测快照**；⛔ 不是断言、不进 test。§1–§9 的旧读数
> （opt-in 实验、默认 OFF 那一轮）保留为历史快照，⛔ 不改写。

### 10.1 改了什么

1. **`253cbe0`（上一单）**：configure 期用**同一份 clang** 探一个极小 C 文件，把前端写进函数属性的
   `"target-cpu"` / `"target-features"` 串读出来，经 `--ir-target-cpu` / `--ir-target-features` 交给
   `lift-gen` ⇒ 8 份 `.ll` 的每个 `define` 都挂属性（另由 `lift-aot-ir-target-attrs` 做构建期判据）。
   这一步解掉了 LLVM `areInlineCompatible()` 的闸（上一轮 §5.2 的定因）。
2. **本单**：把上一轮的 opt-in 档落成**默认**（`apps/emulator-lift/CMakeLists.txt`）：
   - `LIFT_AOT_LTO`：**APPLE 默认 ON**；其它平台默认 **OFF**（LTO 只在 mac 的 Apple clang 17 + ld64
     实测过，win32/Linux 未验证；要试显式 `-DLIFT_AOT_LTO=ON`）。
   - `LIFT_AOT_LTO_ABI` 默认 **ON**（`src/runtime/lift_abi.cpp` 也编 bitcode）；`LIFT_AOT_LTO_RUNTIME`
     保持 **OFF**（B2 那轮整库 LTO 只让 runtime 变大、无收益）。
   - configure 期硬闸**按档位分叉**（`LIFT_AOT_SPLIT>1` 时两档都断言）：OFF ⇒ 必须恰好 `-c;-O2`，
     出现 `-flto/-flto=thin` 一律 `FATAL_ERROR`；ON ⇒ 必须恰好 `-c;-O2;-flto`，仍拒
     `-O0/-O1/-Os/-Oz/-flto=thin/llc`。
   - shape：OFF = `splitN-O2`；ON = `split8-O2-lto-abi`（`-parts` / `-rt` 分支按 ABI/RUNTIME 档位）。
   - 启动横幅按 shape 里的 `-lto` 派生：`LIFT_AOT_SHAPE="split8-O2-lto-abi"（split=8 · -O2 · LTO）`。
   - 回退 = `-DLIFT_AOT_LTO=OFF`（一行，⛔ 不用回滚源码）。
3. 语义面**零改动**：`--ir-target-*` / `lift-aot-ir-target-attrs` / 探针一行未动；⛔ 不引入
   `-march/-mcpu/-mtune`；⛔ 不打开 `LIFT_AOT_LTO_RUNTIME`。
4. ⚠ 上一轮实验在 `out/CMakeCache.txt` 里留过 `LIFT_AOT_LTO:UNINITIALIZED=OFF` ⇒ 重新评估默认值时
   先 `cmake --preset macos-arm64 -U 'LIFT_AOT_LTO.*' -U LIFT_AOT_LTO`（否则 `option()` 会沿用缓存里的 OFF）。

### 10.2 静态（A = LTO OFF · B = LTO ON `-abi` 档；同机同天）

| 量 | A（OFF，`split8-O2`） | B（ON，`split8-O2-lto-abi`） | Δ |
|---|---:|---:|---:|
| `__text` 字节 | 14,399,532 | 14,852,364 | **+452,832（+3.14%）** |
| 总 native 条数 | 3,608,111 | 3,721,295 | +113,184（+3.14%） |
| lift 条数 | 2,500,533 | 2,689,677 | **+189,144（+7.56%）** |
| `_lift_tick` 站点 | 101,765 | 13,893 | **−87,872（−86.4%）** |
| `_lift_note_write` 站点 | 38,535 | **0**（符号被删） | **−38,535（−100%）** |
| `_lift_dyn` 站点 | 5,658 | 5,678 | +20（+0.35%） |
| 跨 part `_lift_cc_*` 站点 | 2,267 | 1,653 | −614（−27.1%） |
| 二进制体积 | 19,596,800 B | 20,801,920 B | +6.15% |

命令：`node .tmp/lto/measure.mjs <binary> --out <json>`（口径 = `llvm-objdump -d` 的指令行数）。
⇒ 静态是**变大**的（每个内联点复制"判空 + 取 + 加 + 存"），换来的是动态减少 ~88k 个 `bl` + 参数装配 + `ret`。
`_lift_note_write` 在 B 里连符号都没有（被 internalize + 内联，内联体改调
`lift::noteGuestWrite(BlockCache*,uint,uint)`，38,698 个站点）。

### 10.3 窗口（FELLOW 进入段，`profile_begin/end fellow-init`；10 run 交错 A,B×5）

| # | 档 | wall (ms) | insns | lift % | boundary % | shim % | instrument % |
|---|---|---:|---:|---:|---:|---:|---:|
| A1 | OFF | 619.4 | 492,382,900 | 50.7 | 29.1 | 16.2 | 4.0 |
| B1 | ON | 541.1 | 492,550,239 | 47.7 | 29.0 | 18.4 | 4.9 |
| A2 | OFF | 622.2 | 492,480,599 | 52.0 | 27.7 | 16.0 | 4.2 |
| B2 | ON | 540.3 | 492,475,609 | 47.4 | 29.1 | 18.5 | 5.0 |
| A3 | OFF | 621.5 | 492,333,744 | 51.5 | 27.9 | 16.2 | 4.3 |
| B3 | ON | 540.7 | 492,722,042 | 47.4 | 29.2 | 18.5 | 4.9 |
| A4 | OFF | 618.9 | 492,263,392 | 51.3 | 28.6 | 16.1 | 4.0 |
| B4 | ON | 545.3 | 492,509,867 | 46.4 | 29.1 | 19.8 | 4.7 |
| A5 | OFF | 621.0 | 492,201,040 | 51.3 | 28.5 | 16.1 | 4.0 |
| B5 | ON | 544.8 | 492,379,106 | 47.7 | 28.3 | 18.6 | 5.4 |

**中位**：

| 量 | A（OFF） | B（ON） | Δ |
|---|---:|---:|---:|
| `wall` | **621.27 ms**（618.88–622.21） | **542.96 ms**（540.28–545.34） | **−78.31 ms（−12.61%）** |
| `insns` | 492,358,322 | 492,530,053 | +171,731（**+0.03%**，在 ±0.2% 抖动内，**不作效率判据**） |
| `lift` 桶 | 319.4 ms（51.4%） | 257.2 ms（47.5%） | **−62.2 ms（−19.5%）** |
| `boundary` 桶 | 177.0 ms（28.6%） | 157.5 ms（29.1%） | −19.5 ms（−11.0%） |
| `shim` 桶 | 100.3 ms（16.1%） | 100.7 ms（18.6%） | +0.4 ms（+0.4%，不变） |
| `instrument` 桶 | 25.8 ms | 26.6 ms | ≈ 不变 |

**逐对差**（A−B，负 = ON 更快）：5/5 全负。

| 对 | A (ms) | B (ms) | Δ ms | Δ % |
|---|---:|---:|---:|---:|
| 1 | 619.36 | 541.08 | −78.28 | −12.64% |
| 2 | 622.21 | 540.28 | −81.93 | −13.17% |
| 3 | 621.51 | 540.73 | −80.78 | −13.00% |
| 4 | 618.88 | 545.34 | −73.54 | −11.88% |
| 5 | 621.02 | 544.83 | −76.19 | −12.27% |

`lift + boundary` 的绝对包络（`wall − shim − partial − instrument`）中位 A **495.0 ms** / B **414.1 ms**
（−80.9 ms，其中 lift −62.2、boundary −19.5）⇒ 省下的 ~80 ms 全落在 lifted 代码段，与静态 tick/note
站点读数方向一致；`shim` 不变说明不是"少做了宿主调用"。
窗口有效性 10/10：`=== profile window: fellow-init ===` 有打印 · `frames=0` · `unaccounted 0.000 ms` ·
`coverage 1.68–1.88` · `verdict = 候选（boundary 27.7–29.6%）`。

### 10.4 实际配方（⛔ 不是任务给的原始 plan）

两处偏差都是实测修正的：

1. **`profile_begin` 必须在 `await script FELLOW` 之后**（给定配方把它放在 SN0000 右键之后 ⇒ 窗口里
   混进 12 次 Present，`frames != 0`、insns 也偏低）。
2. **用 `--max-frames 6000` 收口，⛔ 不要用 `quit`**：`quit` 投在"下一个读点"，同一档跨 run 的
   `frames`（5440–5631）与 `stop @EA` 会漂 ⇒ 语义指纹不可复现。固定帧收口时 10/10 的 `frames=6000`、
   `stop @ eip=00498AE4` 恒同。窗口在更早的 `await position FELLOW` 处已结束，不受影响。

可粘贴的 plan（存成 `/tmp/fellow-init.plan`）：

```
await position TITLE 0x0460 0x0520 timeout 30000
click 1186 373
await position GAMESTART 0x0664 0x0724 timeout 30000
click 830 592
await position SN0000 0x46B8 0x470C timeout 35000
click 500 500 right
await script FELLOW timeout 60000
profile_begin fellow-init
await position FELLOW 0x1684 0x16A4 timeout 60000
profile_end fellow-init
```

命令行（`--appdata` 必须是**全新副本**；`--game-dir links/install` 只读）：

```bash
cp -R .tmp/p2/appdata2 /tmp/appdata-lto-default      # 任意"未跑过"的 appdata 快照
/usr/bin/time -lp out/emulator-lift-aot --present headless --skip-logo --audio off \
  --game-dir links/install --appdata /tmp/appdata-lto-default \
  --max-frames 6000 --time-limit 180 --input-plan /tmp/fellow-init.plan
```

（本节的 10 次 run 用 `.tmp/lto-feature/fellow-init-fp.plan`，等价于上面这份；每次一份全新 appdata。
`insns` 是本机 mac 的 4.92 亿，任务给的口径 5–7 亿来自 win 上的另一段读数 —— 两档 insns 相等即可比。）

### 10.5 语义指纹、代价、平台范围

**语义指纹 10/10 逐位相同**（A 与 B 全部 10 次 run）：

| 项 | 值 |
|---|---|
| `exit` | 0 |
| `frames` | 6000（`--max-frames 6000`） |
| `stop @` | `eip=00498AE4`（max-frames · shim `Present`） |
| arena | `in_use=125625400` / `peak=125625400` / `surfaces=31` |
| `heap_blocks` | 7863 |
| `table misses` | 0（`entries=33509`，`hits=lookups`） |
| `input-plan` | `steps=10 fired=3 pending=0` |

总 `insns` 不进指纹（±0.16% 抖动，`AGENTS.md` §12.6 已写明不作判据）。

**代价（同机同天）**：

| 项 | OFF | ON（`-abi`） |
|---|---:|---:|
| configure | 2.83 s | 2.45 s |
| 全量构建 | 0.06 s（no-op） | **65.05 s / 3.05 GB**（`INTERFACE -flto` 让 ~96 条链接都跑 LTO 后端） |
| AOT 臂链接（重复链接一次） | **0.16 s / 250 MB** | **37.42 s / 3.03 GB** |
| `__text` | 14,399,532 B | +3.14% |

⇒ 每次改 AOT IR / 运行库后重建都要付这 ~37 s / 3 GB 的链接。

**平台范围**：只在 **mac（macOS arm64 · Apple clang 17 · ld64）** 实测。**win32（clang-cl + lld-link）
与 Linux 未验证** ⇒ `LIFT_AOT_LTO` 在非 APPLE 上默认 **OFF**；显式 `-DLIFT_AOT_LTO=ON` 属未验证路径
（CMake 会打 `WARNING`，不拦）。覆盖面只有 `fellow-init` 一个窗口（4.92 亿 guest insns、0 帧、纯计算段），
没有覆盖音频 / 真 GPU 呈现 / 存档写盘 / 其它场景。

---

## 11. 2026-11-17：lift_tick 去判空后的内联读数

### 11.1 改了什么

`apps/emulator-lift/src/runtime/lift_abi.cpp`：`lift_tick` 的

```cpp
if (!cpu_ptr) { fprintf(stderr, …); fflush(stderr); _Exit(4); }
```

删掉，改成 `assert(cpu_ptr != nullptr)`（`#include <cassert>`）。理由（源码注释里同一份）：
唯一调用方是**生成物**（`lift_gen_emit.cpp` 发 `call void @lift_tick`），trampoline 恒传
`&AotRunner::cpu_`；`runtime/` 与 `tests/` 零调用、也没有测试断言过 null 行为。用 `assert`
而不是静默：RelWithDebInfo / Release（`NDEBUG`）下**零代码零分支**，Debug 档仍能点名
`cpu_ptr == nullptr`。`lift_one` / `lift_shim` 的判空**保留**（不是 10 万站点级热路径）。
`lift_abi.h` 的契约注释同步改成"必须非空；Debug 档 assert"。
⛔ 没有动 `flushTick` 的发射、`insns` 记账语义、`X86` 布局；没有加 `always_inline`。

### 11.2 站点读数（mac arm64 / RelWithDebInfo / `split8-O2-lto-abi`）

```bash
llvm-objdump -d --no-show-raw-insn --demangle <bin> | rg -c 'bl\s+.*_lift_tick'
llvm-nm <bin> | rg _lift_tick
```

| 项 | before（HEAD 7efba89 产物） | after（本轮） |
|---|---:|---:|
| `bl _lift_tick` 站点 | **13,893** | **0** |
| `_lift_tick` 符号 | `0000000100e2f7f0 t _lift_tick` | **符号被删**（`llvm-nm` 里没有） |
| `__text` | 14,852,376 B | 13,903,808 B（**−948,568 B，−6.39%**） |
| 可执行文件 | 20,801,920 B | 19,644,384 B（−5.56%） |

⇒ 上一轮 B 档"大函数里没内联的那 13,893 个"在本轮**全部内联**：没有残留 `bl`，连符号都被删了。
因此**没有 inline 拒绝理由可拿**（`opt -pass-remarks-missed=inline` 无从触发），也不需要
`always_inline`。

### 11.3 逐函数 census（IR 站点 vs 反汇编 store）——判据**不成立**

```bash
node .tmp/tick-inline/census.mjs apps/emulator-lift/out/aot .tmp/tick-inline/after.dis \
     .tmp/tick-inline/census.json
```

口径：IR 侧数 `part*.ll` 里每个 `@lift_cc_*` 的 `call void @lift_tick(ptr %cpu, i32 n)`；
反汇编侧按 **CFG must 分析**跟踪 `cpu` / `cpu+0x6d8` 的寄存器（`-O2` 会把基址提进寄存器，
`str xN,[xInsns]` 里不再有 `#0x6d8`），数对 `offsetof(X86, insns)=0x6d8` 的 store。
`offsetof` 由探针实测 = **0x6d8**（`sizeof(X86)=1792`）。

| 项 | 读数 |
|---|---:|
| IR `@lift_cc_*` 函数 | 10,254 |
| IR `call void @lift_tick` 站点 | **104,949**（Σn = 348,208） |
| 其中"从入口 switch 可达"的站点 | 104,848（**101 个在 IR 死块里**） |
| 反汇编 store（direct `[cpu,#0x6d8]` + hoisted `[xInsns]`） | **102,185**（100,828 + 1,357） |
| 逐函数 `store == IR 站点` | **8,138 / 10,254** |
| 逐函数不等 | 2,116 |

差值直方图（`store − IR`，按函数数）：`+1:672 · +2:199 · −1:507 · −2:238 · −3:109 · −4:88 …`
（长尾到 `−86 / +8`）。完整 2,116 条不等清单在 `.tmp/tick-inline/census.json`（观测产物，不入库）。

**这个判据在本 build 上不可用**，原因不是"没内联"，而是优化把
"一个 IR 调用点 = 一个 store"这个前提打破了。三条可复现的证据：

**① LTO 把整个 `lift_cc_*` 函数内联进彼此（会给调用方**加** tick）。**
IR 里 `call i32 @lift_cc_*` 共 **2,571** 个站点，反汇编里 `bl _lift_cc_*` 只剩 **605** 个
⇒ ~1,966 个整函数内联。例：`lift_cc_401A81` 的 IR 只有 1 个 `lift_tick(…, i32 2)`，但机器码有
**2** 个 store：

```text
1003f1e64:  ldr  x9, [x3, #0x6d8]
1003f1e68:  add  x9, x9, #0x2
1003f1e6c:  str  x9, [x3, #0x6d8]      ; 自己的 tick(2)
…
1003f1e90:  ldr  x10, [x3, #0x6d8]
1003f1e94:  add  x10, x10, #0x5
1003f1e98:  str  x10, [x3, #0x6d8]     ; 内联进来的 lift_cc_4E6F18 的 tick(5)
```

`lift_cc_401A81` 的 IR 里紧跟着 `call i32 @lift_cc_4E6F18(ptr %st, ptr %mem, i32 5140248, ptr %cpu)`；
`lift_cc_4E6F18` 只有一个 switch case `i32 5140248 → bb_4E6F18`，而 `bb_4E6F18` 里恰好只有一个
`lift_tick(…, i32 5)`。⇒ 多出来的 store 是**被内联的 callee 的 tick**，不是重复计数。

**② 相同块被合并（会把两个 IR 站点合到一个 store）。**
例：`lift_cc_402A00` 的 IR 有 8 个 tick（6,4,5,5,4,4,3,4，Σ35），机器码只有 **7** 个 store（Σ31）：
switch 的 `0x402A42` 与 `0x402A4F` 两个 IR 块（各一个 `lift_tick(…, i32 4)`）落到**同一个**机器块：

```text
1003f9aa4:  b.eq   0x1003f9ab8          ; case 0x402A42
1003f9ab4:  b.ne   0x1003f9c50          ; case 0x402A4F 落到 fallthrough
1003f9ab8:  …                            ; 共享块
1003f9ad4:  ldr    x13, [x3, #0x6d8]
1003f9ad8:  add    x13, x13, #0x4
1003f9adc:  str    x13, [x3, #0x6d8]     ; 一份 store 服务两个 IR tick 站点
```

**③ IR 死块**：101 个 `call void @lift_tick` 位于从入口 `switch` 到不了的块里（生成物的
"只能从别的入口进"的块），codegen 当死代码删掉 —— 它们在 IR 文本里，但从来不会执行。

另有 **5 个函数用跳转表**（`br xN`），census 的机器 CFG 解不出 `br` 的目标，逐函数数字不可用：
`lift_cc_4F6E1E`（IR 89 / store 3）、`lift_cc_50D260`（94/15）、`lift_cc_4EDBDE`（50/13）、
`lift_cc_425A10`（15/2）、`lift_cc_426120`（20/6）。⇒ 这 5 个只能按"再无 `bl _lift_tick`"判。

### 11.4 语义指纹（60 帧，before/after 各自全新 appdata）

```bash
node .tmp/lto/run-fingerprint.mjs <bin> .tmp/tick-inline/appdata-<档> /tmp/fp-<档>.txt --frames 60
```

| 项 | before | after |
|---|---|---|
| `exit` | 0 | 0 |
| `frames` | 60 | 60 |
| `stop @` | `eip=00498AE4`（max-frames · shim `Present`） | 同左 |
| arena `in_use / peak / surfaces` | 108595152 / 108595152 / 21 | 108595152 / 108595152 / 21 |
| `table misses` | 0（entries=33509） | 0（entries=33509） |

**五项逐位相同**。附注：`heap_blocks` 两档都是 6904（§6 表里是 6670 —— 那是另一个 appdata
快照的读数，不是本轮判据）；`insns` 793,429,194 → 793,463,481（+0.004%，**不作判据**）。

### 11.5 FELLOW 窗口（`--max-frames 6000`，每次全新 appdata）

```bash
/usr/bin/time -lp apps/emulator-lift/out/emulator-lift-aot --present headless --skip-logo \
  --audio off --game-dir links/install --appdata <新目录> --max-frames 6000 --time-limit 180 \
  --input-plan .tmp/lto-feature/fellow-init-fp.plan
```

| run | `wall` | `lift` | `boundary` | `shim` | 窗口 `insns` |
|---|---:|---:|---:|---:|---:|
| 1 | 560.024 ms | 263.758 ms (47.1%) | 154.891 ms (27.7%) | 116.018 ms (20.7%) | 492,724,228 |
| 2 | 557.720 ms | 260.366 ms (46.7%) | 154.423 ms (27.7%) | 108.382 ms (19.4%) | 492,367,689 |
| 3 | 543.056 ms | 255.922 ms (47.1%) | 158.255 ms (29.1%) | 101.368 ms (18.7%) | 492,475,103 |
| 中位 | **557.720 ms** | 260.366 ms | 154.891 ms | 108.382 ms | — |

`frames=0` · `unaccounted 0.000 ms` · `coverage 1.86` · `verdict = 候选（boundary 27.7%）` 三次一致；
总帧收口 `stop @ eip=00498AE4`、`frames=6000`。

**对比 §10 的 B 中位 542.96 ms**：本轮中位 557.72 ms（**+2.7%**），最好一次 543.06 ms（+0.02%）。
⚠ 采集时 `uptime` 1 分钟负载 **4.67**（这台机器有背景负载）；且这是三次同向 run、**没有做
交错 A/B**，按 `AGENTS.md` §12.6 不能据此判"变慢/变快" —— 只能说本轮**没有稳定复现 ≤543 ms**。

### 11.6 门禁与结论

`ctest --test-dir out --output-on-failure`：**105 条 · 101 passed · 4 skipped · 0 failed**
（79.73 s）。skip 逐条原因：

| 用例 | 原因 |
|---|---|
| `cli-aot-flags` | AOT 参数面用 `CreateProcessW` 起子进程（Windows 专有） |
| `gpu-raster` | CTest 设 `SDL_VIDEODRIVER=dummy` ⇒ 建不出 GPU 设备（SKIP 77） |
| `frame-dump-failure` | stderr 捕获 / 目录当文件写用 MSVC `<io.h>`（Windows 专有） |
| `aot-audio-wiring` | 接线用 `CreateProcessW` 起子进程（Windows 专有） |

**一句话结论**：本轮之后**没有任何 `bl _lift_tick`**（符号也被删）—— 上一轮残留的 13,893 个
站点**全部内联**，不需要 `always_inline`、也没有拒绝理由要提供。但**"每个函数 store 数 == IR
站点数"这个逐函数判据在本 build 上不成立**（8,138/10,254 相等）：LTO 同时把整个 `lift_cc_*`
内联进彼此（2,571 个 IR 调用点 → 605 个 `bl`）、并合并相同块，两个方向都会让 store 数偏离
IR 站点数（§11.3 的 ①②③ 各有逐例反汇编）。要把这个判据变成可用的门禁，需要**按 IR 记账**
（例如给每个 tick 站点一个可追踪标记，或把内联进来的 callee tick 计入调用方），那属于本轮
边界之外的发射改造。

---

## 12. 2026-11-18：win32 默认开启（clang-cl 22.1.2 + lld-link）—— `/Ob1` 是那道缺失的闸

> 口径：`E:\Projects\amayui-re` · win32（AMD Ryzen 7 7735H · 8C/16T）· clang / clang-cl / lld-link / llvm-lib
> **22.1.2** · CMake 4.3.0-rc1 · Ninja 1.13.2 · RelWithDebInfo · 起点 = `303bc96`（`main`）·
> A/B 交错、每 run 全新 appdata、同机同天。
> ⚠ 所有数字都是 **2026-11-18 的一次实测快照**；⛔ 不是断言、不进 test。§1–§9（opt-in 实验）与 §10/§11（mac）
> 的旧读数**不改写**。

### 12.1 改了什么

1. **默认值**：`LIFT_AOT_LTO` 在 `APPLE OR WIN32` 上默认 **ON**；Linux 仍 **OFF**（未验证）。
   `LIFT_AOT_LTO_ABI` 保持默认 ON（`src/runtime/lift_abi.cpp` 编 bitcode）；`LIFT_AOT_LTO_RUNTIME` 保持 OFF。
   shape 仍是 `split8-O2-lto-abi`，启动横幅一行不变（`-lto` 从 shape 派生）。
2. **`lift_abi.cpp` 单点 `/Ob2`**（★ 本节的**核心**，见 12.2）：只在 WIN32 追加，非 WIN32 仍是 `-flto`。
3. **不做**的事：⛔ 不动全局 flags（`CMAKE_CXX_FLAGS_RELWITHDEBINFO` 的 `/Ob1` 保留）；
   ⛔ 不按平台收窄链接行的 `-flto`（实测 `out/build.ninja` 带 `-flto` 链接成功，lld-link 自动 LTO，
   只打一条 `warning: ignoring unknown argument '-flto'`）；⛔ 不动 `LIFT_AOT_SPLIT`、⛔ 不引入 `-flto=thin`。
4. 回退 = `-DLIFT_AOT_LTO=OFF`（一行）；⚠ 若 `out/` 的 cache 里已有显式 `LIFT_AOT_LTO:BOOL=OFF`
   （例如旧树 / `out-user`），要**默认档**得先 `-U LIFT_AOT_LTO`（或删掉那条 cache 项）—— `option()` 会沿用缓存值。

### 12.2 管线与根因：为什么第一轮"LTO 跑了却没内联任何一个热函数"

**管线（能通，已验）**：`clang -c -O2 -flto --target=x86_64-pc-windows-msvc`（8 份 `.ll`）与
`clang-cl -c … -flto`（`lift_abi.cpp`）都产 **LLVM bitcode**（magic `42 43 C0 DE`），`llvm-lib` 归档，
`lld-link` 链接。**lld-link 自动对 bitcode 输入跑 LTO**（不需要链接行的 `-flto`；给了只打一条 warning）。
探针：bitcode 归档成员 + native `.obj` ⇒ 被调函数被 internalize + 内联（PDB 里连符号都没有）。

**第一轮（只有管线、没有 `/Ob2`）的负结果**：跨 part `lift_cc_*` 真被内联（直接调用 1,107 → 636；
抽样 `lift_cc_4E6F18` 272 → 0、`lift_cc_4E6DB8` 49 → 0），但
**`lift_tick` 104,750 → 105,617、`lift_note_write` 39,075 → 39,319、`lift_dyn` 5,189 → 5,198**
—— **一条都没内联**，窗口墙钟反而 **+2.75%**（3/3 对全正）。
⇒ 那组读数**不能**读作"LTO 在 win32 无收益"，它测的是"只内联了跨 part 调用"的 LTO。

⚠ 这一轮还踩过一个**读数陷阱**：`llvm-nm` 在 win32 PE 上打印 `no symbols`（lld-link 不写 COFF 符号表），
改用 `.pdb` publics 取地址后，ON 档里 `lift_tick` 被 internalize + 挪地址 ⇒ publics 里查不到 ⇒
"按 PDB 地址数 `callq`"读成 **0 站点**（假阴性）。**正确口径 = 函数体地址**（父级用
`lift_cc_401000` 里的 `callq` 目标 + 前 3 条指令逐字对照钉死了这一点）。

**根因（机械、已验）**：`CMAKE_CXX_FLAGS_RELWITHDEBINFO` = `/Zi /O2 /Ob1 /DNDEBUG`，
其中 MSVC 的 **`/Ob1`**（"只内联标了 `inline` 的函数"）被 clang-cl 落成**函数级 `noinline`**：
`lift_abi.cpp` 的 bitcode IR 里 `lift_tick`（`#8`）/ `lift_note_write`（`#4`）/ `lift_dyn`（`#4`）的
attributes 都带 `noinline` ⇒ LTO **结构上**不可能内联它们。
**不是** mac §5.2 那个 `areInlineCompatible()`：AOT 侧 `attributes #0 = { "target-cpu"="x86-64"
"target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" }` 与 `lift_abi` 的一模一样。

**`/Ob` 覆盖顺序（3 行小文件验过）**：`clang-cl /c /O2 /Ob1 /Ob2 -flto` ⇒ IR **没有** `noinline`；
`/Ob2 /Ob1` ⇒ **有**。⇒ **后写的赢**，所以 `COMPILE_OPTIONS "-flto;/Ob2"` 追加在全局 `/Ob1` 之后有效。

### 12.3 静态（A = LTO OFF `split8-O2` · B = LTO ON `split8-O2-lto-abi` + `/Ob2`；同机同天）

口径 = **函数体地址**上的 `callq`（⛔ 不经过 PDB public）。`lift_tick` / `lift_note_write` 用指令序列指纹
在反汇编里定位函数体（`testq/je/movl/movq 0x260(%rcx)/jmp`；`movl %edx,%eax/addq %rax,0x6e0(%rcx)/retq`），
先在 OFF 上自校验命中已知地址，再到 ON 上找。

| 量 | A（OFF） | B（ON + `/Ob2`） | Δ |
|---|---:|---:|---:|
| 反汇编指令行 | 3,502,918 | 3,314,520 | **−5.38%** |
| `__text` | 13,826,294 B | 13,315,318 B | **−3.70%** |
| 可执行文件 | 15,929,856 B | 15,283,200 B | −4.06% |
| **`lift_tick` 调用** | **104,750** | **0**（函数体在反汇编里已不存在） | **−100%** |
| **`lift_note_write` 调用** | **39,075** | **0**（函数体已不存在） | **−100%** |
| **`lift_dyn` 调用** | **5,189**（+603 `jmp`） | **5,198**（+612 `jmp`） | **+0.17%**（与 mac +0.35% 同向） |
| 跨 part `lift_cc_*` 调用 | 1,107（269 distinct） | 621（230 distinct） | −43.9% |
| 直接 `callq` 总数 | 192,185 | 87,174 | **−54.6%** |

⇒ 与 mac 同一形态：两个热 shim 全内联、`lift_dyn` 不动（它是分配器式的大函数），**代码变小**（mac 那轮是变大 —— mac
的 tick 还带着判空，win32 这轮已先做过 §11 的去判空）。

### 12.4 A/B（FELLOW init 窗口，`--max-frames 6000`，热缓存，每 run 全新 appdata，顺序交替 A→B / B→A）

窗口有效：6 次 run 全部 `frames=0` · 窗口 guest `insns` **612.37M–613.92M（±0.13%）** ·
`unaccounted 0.000 ms` · `coverage 1.27–1.46×` · 窗内文件 shim 只有 `kernel32!CreateFileA` **N=68**
（热缓存下 12–14 ms ≈ 窗口墙钟的 1%）。

| pair | A（OFF）win ms | B（ON+/Ob2）win ms | Δ ms | Δ % |
|---|---:|---:|---:|---:|
| 1 | 1319 | 1197 | −122 | **−9.25%** |
| 2 | 2240 | 1597 | −643 | **−28.71%** |
| 3 | 2094 | 1588 | −506 | **−24.16%** |
| 4 | 1570 | 1479 | −91 | **−5.80%** |
| 5 | 1836 | 1819 | −17 | **−0.93%** |

- **方向：5/5 对 ON 更快**；**逐对 Δ 中位 = −122 ms（−9.25%）**；两侧中位 1836 → 1588 ms（−13.5%）。
- ⚠ **噪声说明**：A 侧自己在 1319–2240 ms 之间漂（窗口 insns 只差 ±0.13%）⇒ 单看某一对不可靠；
  结论只建立在"5/5 同向 + 逐对中位"。幅度区间 −0.9%…−28.7%，真实值大概率在 −9%…−14%
  （与 mac §10 的 −12.61% 重合）。进程 CPU 时间两侧接近（A 59.0/76.3/73.3/66.9/72.4 s，
  B 58.6/69.4/73.7/66.5/67.1 s）⇒ 不是"少做了活"。

### 12.5 语义指纹

10/10 run（A/B 各 5）**逐项相同**：`exit 0` · `frames 6000`（`--max-frames 6000`）·
`stop @ eip=00498AE4`（max-frames · shim `com!IDirect3DDevice9::Present`）。
另外默认档（无 `-D`、`-U LIFT_AOT_LTO` 后）单独跑 2 次，同样 `exit 0` / `frames 6000` / `stop @ 00498AE4`，
且**启动横幅**是 `LIFT_AOT_SHAPE="split8-O2-lto-abi"`。

### 12.6 门禁与代价

- `ctest --test-dir out --output-on-failure`（默认档构建）：**107/107 passed · 0 failed**（127.8 s）。
- configure（默认档，`-U LIFT_AOT_LTO`）：**9.81 s**，STATUS `LIFT_AOT_LTO: ON（shape=split8-O2-lto-abi）`，
  cache `LIFT_AOT_LTO:BOOL=ON`（`-D` 一个都没给 ⇒ **默认就是 ON**）。
- 全量 build（`--parallel 4`）：**193.9 s** · 120/120 · 峰值 **922.2 MB/进程 / 960.1 MB 合计**。
- **AOT 臂链接：100.1 s**（OFF 0.71 s；无 `/Ob2` 那轮 119.8 s）⇒ 每次改 AOT IR / 运行库都要付这 ~100 s。
- ⚠ 并行构建有个**旧竞态**：`vcpkg z-applocal` 在 `out/tests/` 上并发拷 DLL 会互撞（默认并行度下 8+6 个目标红，
  `The process cannot access the file because it is being used by another process.`）；`--parallel 4` 一次过。
  与 LTO 无关，但 LTO 让链接变长会放大它。

### 12.7 平台范围

mac（Apple clang 17 + ld64）与 win32（clang-cl + lld-link 22.1.2）实测；**Linux（ld.lld）未验证**
⇒ `LIFT_AOT_LTO` 在 Linux 上仍默认 **OFF**，显式 `-DLIFT_AOT_LTO=ON` 属未验证路径（CMake 只告警、不拦）。
覆盖面：`fellow-init` 一个窗口（~6.1 亿 guest insns、0 帧、纯计算段 + 一次 68 次 `CreateFileA`）。

### 12.8 复现（win32）

```powershell
cd apps/emulator-lift
cmake --preset windows-x64 -U LIFT_AOT_LTO     # 默认档：应打印 shape=split8-O2-lto-abi、cache LIFT_AOT_LTO=ON
cmake --build --preset windows-x64 --parallel 4
ctest --test-dir out --output-on-failure

# 静态（口径 = 函数体地址；⛔ 不要用 PDB public 判"有没有内联"）
#   OFF：lift_tick 0x140B9B730 / note_write 0x140B9B710 / dyn 0x140B9C400
#   ON ：lift_tick（函数体已不存在）/ note_write（已不存在）/ dyn 0x140C7C3E0
#   脚本：.tmp/lto-win32/{symbols.ps1,findseq.mjs,measure.mjs,dis8.ps1}

# A/B（FELLOW init 窗口，每次全新 appdata）
<exe> --present headless --skip-logo --audio off --game-dir links/install --appdata <全新副本> `
      --max-frames 6000 --time-limit 180 --input-plan .tmp/lto-win32/fellow-init.plan
```

⚠ **读数口径两条硬规矩**（本轮踩过）：
1. `llvm-nm` 在 win32 PE 上不可用（`no symbols`）；符号地址要取 `.pdb` publics，但**内联后的函数会从 publics 消失**
   ⇒ 判"是否内联"必须**按函数体地址/指令序列**数 `callq`，⛔ 不能按"符号还在不在"。
2. 冷盘会让同一段 guest 代码的墙钟差 ±50%（窗口内含 `CreateFileA`）⇒ **先 warmup 再测**，且只认"逐对方向 + 中位"。
