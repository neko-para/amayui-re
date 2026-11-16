# buildTarget 的假绿面：spawn 产品 exe 的守卫必须 add_dependencies 产品 exe（并加静态检查）

- id: REQ-01M7K2J7VXKJMK1BJ3H7YCVJFJ
- type: req
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, toolchain, mutation]

## 一句话

`buildTarget`（只重建守卫的 target）有一个**假绿面**：凡是守卫**spawn 产品 exe** 去读它的输出的用例，
变异落在产品 exe 的 TU 里时，`--target <守卫>` 只重建守卫 exe ⇒ 产品 exe 停在**变异前** ⇒
守卫跑的是旧二进制 ⇒ 变异**"没红"看起来像"守卫没抓住"**，其实是**根本没施加上**。

## 为什么单独立这一节点

这条不是"少了个优化"，是**变异自检的可信度问题**：它会让一条已经"实测红"的结论在下次跑时**静默变绿**，
而 `--list`（只看名字 / `from` 唯一 / buildTarget 名字对不对）**完全看不出来**。
发现过程：`REQ-01M7K04EY6D3GQNB5DRWP0DYJ8` 洞 7 的连带观察（实现者主动报的）。

## 已经修掉的一例（2026-11-17）

`cli-aot-flags`：变异在 `src/aot/aot_main.cpp`（产品 exe 的 TU），守卫 `lift-cli-aot-flags` 只是
`add_test(… COMMAND lift-cli-aot-flags $<TARGET_FILE:emulator-lift-aot>)` ⇒ 之前**假绿**。
`CMakeLists.txt` 已加 `add_dependencies(lift-cli-aot-flags emulator-lift-aot)`，
同形的 `lift-aot-audio-wiring` 也已带。

## 还要做

1. **枚举全部同形用例**：`apps/emulator-lift/CMakeLists.txt` 里凡是
   `add_test(… COMMAND <guard> … $<TARGET_FILE:emulator-lift-aot>)`（或 spawn 其它产品 exe）的，
   逐个确认对应 target 有 `add_dependencies(<target> <产品 exe>)`。
   ⚠ 目标不是"全加一遍"——只加**变异点可能落在产品 exe TU 里**的那些；但**先全量列出来**再判。
2. **加静态检查**（这才是本单的主体，判据是它会红）：
   解析 `CMakeLists.txt`：若某 `add_test` 的 COMMAND 里出现 `$<TARGET_FILE:<产品 exe>>`，
   则那条用例的**可执行 target** 必须有 `add_dependencies(<target> <产品 exe>)`。
   落点：与 `ctestCommandTarget` 同一处（`tools/mutate-check.mjs`），并在 `--list` 里报。
3. **修掉反向风险**：如果某条变异的 `file` 属于产品 exe 的源、而 `buildTarget` 是守卫 target 又**不能**
   加依赖（例：守卫不 spawn 而是 link 同一份代码），要写明**为什么它不需要**（一句话注释即可）。
   ⛔ 不许用"我看过了"代替。

## 判据

1. 静态检查**能红**：删掉 `add_dependencies(lift-cli-aot-flags emulator-lift-aot)` ⇒ `--list` 非 0 并点名
   "该用例 spawn 产品 exe，但缺 add_dependencies"。贴改动前红的原文。
2. 真跑一条同形变异（`--match '…'`）⇒ ✅ 红 · 1/1，且**耗时**与该条 `buildTarget` 的说法一致
   （应该是"守卫 exe + 产品 exe"两次小链，不是全量）。
3. `ctest` 全量 N/N 不变；`pnpm test` / `pnpm verify` 不变。
4. ⛔ 不放宽缺省：没有 `buildTarget` 的条目仍然全量构建。

## 未做 / 边界

- 不做"通用依赖图求解"（CMake 已经是真源，我们只读 `add_test` / `add_dependencies` 两类语句）。
- 不改 `finalizeBuildDirs` 的收尾全量重建（它兜的是"陈旧 exe"，与本单的"变异没进 exe"是两件事，
  两个都需要）。
