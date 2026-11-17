# AOT LTO / single-LL 对照：默认保持 split8-O2（已被 REQ-01M7M1B… 取代）

- id: REQ-01M7KX2BXE1KK5MAXGMGACNEXC
- type: spike
- status: done
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- done_reason: 默认保持 split8-O2：LTO 今天对 tick/note/dyn 零内联（函数级属性不对等；codegen 两侧同为 apple-m1），链接 +29–45 s / 2.4–4.3 GB；single LL 等价且更慢更大；B1x 证明收益存在但代价 +3.1% code / +36 s link，下一步优先 IR 内联 tick。详见 docs/03-emulator/aot-lto-evaluation.md。
- tags: [emulator-lift, lto, efficiency]

## 设问

AOT 是否该开 LTO（跨 IR + shim）或改回 single LL？默认形状是否要改？

## 结论（2026-11-17 / macOS arm64 / Apple clang 17 / RelWithDebInfo）

- **默认保持** AGENTS §12.8 的 `split8 -c -O2`。LTO B0（8 份 + link）、B1（+`lift_abi.cpp`）、B2（+整个 runtime/shim）对 `lift_tick` / `lift_note_write` / `lift_dyn` **一条都没内联**：AOT IR 函数**没有** `target-cpu/target-features` **函数属性**，而 clang 的 C++ 前端会把 `apple-m1` 写进运行库 TU 的每个函数属性 ⇒ `areInlineCompatible()` 判不对等（离线 `llvm-link`+`opt`：原样 17,544→17,561；删两侧属性 → 3,086；补属性后 `.obj` 与默认逐字节相同）。
- ★ **这不是 codegen feature 缺失**：`clang -###` 对 `.ll` 与 C++ 的 cc1 行逐项相同（`-target-cpu apple-m1` + 同一串 feature）；mac 上 IR 的 codegen 本来就用 apple-m1。`-mcpu=apple-m1` 对 `.ll` 无效（默认已是 apple-m1，flag 映射到 `apple-a14`，产物逐字节相同）。mac 上 `-mcpu=generic` 关 feature ⇒ `__text` −53,912 B、lifted NEON 7,745→3,709（−52%）⇒ feature 确实在用。
- win x64：IR 与 C++ 默认都是 `x86-64`/SSE2（0 feature）；8 份全编 `+AVX2` −0.40% 条、VEX 115,206、`%ymm` 8,892、`.text` −8,915 B。<待核：win 运行机 feature 集>。要用 AVX/SSE4 是**可移植性裁决**（两边一起改 + `-ffp-contract=off`），不是补 IR 属性。
- 唯一被内联的是跨 part `lift_cc_*`：2,267→1,543，且**净增代码**（lift +0.17%）；A→B 的 −2% 是 ld64 隐式死剥（A-ds 对照拆开）。
- **single LL 不划算**：与 B0 等价（`liftCc` 同样 1,543，tick/note/dyn 仍跨 TU），`__text` +0.16%，clang 段 7.35 s → 38.9 s。
- **代价**：LTO 链接 29.05 s / 2.40 GB（B0）、44.97 s / 4.25 GB（B2）；今天常规链接 0.21 s / 247 MB。
- **诊断档 B1x**（把 `target-cpu/target-features` **函数属性**注进 8 份 IR）：tick 站点 −86%、note −100%，但 `__text` +3.1%、lift +7.6%；固定 guest 工作量墙钟 −4.6~8%（mac，LSP 背景负载污染，⛔ 不作 win 判据）。
- **语义指纹** A/B0/B1/B2/C/B1x 全同：`exit/frames/stop @/arena/table misses` 逐位相同。

## 下一步（未做，二选一）

1. **最便宜**：`lift-gen` 把 `insns += n` 直接发成 IR（不依赖 LTO；但记账出现第二份实现，需守卫，未实测）。
2. **LTO + 发射 `target-cpu/target-features` 函数属性**（值由 CMake 传入，⛔ 不写死 apple-m1；需改 AGENTS §12.8 冻结裁决；win32 feature 集不同、+3% 代码、+36 s 链接、3 GB RSS）。

## 证据

`docs/03-emulator/aot-lto-evaluation.md`（命令 / 读数 / 语义指纹 / 复核；§5.2 已按“函数属性 vs codegen feature”校正）。
实验用的 opt-in 补丁**未入库**，留档 `.tmp/lto/opt-in.patch`；默认档保持 HEAD 的 `-c -O2` + `-flto` 硬闸。

## 后续（2026-11-17）

上面"默认保持 split8-O2"已被 `REQ-01M7M1BSZD1HTGQW1M6PENE2TB` 取代：`253cbe0` 让生成物发射
`target-cpu` / `target-features` 函数属性之后，`areInlineCompatible()` 的闸解开，`_lift_tick` 站点
**−86.4%**、`_lift_note_write` **−100%**，FELLOW init 窗口墙钟 **−12.61%**（5/5 对全负）⇒ mac 默认
改成 LTO（`LIFT_AOT_LTO=ON`，shape `split8-O2-lto-abi`）。读数与实际配方见
`docs/03-emulator/aot-lto-evaluation.md` §10。本单正文与读数保留为历史快照，⛔ 不改写。
