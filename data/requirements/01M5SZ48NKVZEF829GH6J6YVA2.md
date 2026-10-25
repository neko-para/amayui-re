# 补齐 boot→TITLE 路径缺失的 17 条指令（0x009/0x0A1-A3/0x0D9/0x0FF/0x100/0x12E/0x19B/0x19C/0x1BF/0x1CE/0x1CF/0x1F4/0x1F5/0x208/0x2FC）

- id: REQ-01M5SZ48NKVZEF829GH6J6YVA2
- type: req
- status: done
- parent: REQ-01M5SZ3M7GCR0M23RSYBD7GEA5
- verify: tools/test/emulator-cxx-dispatch.test.mjs#★ 到 TITLE 用到的 17 条必须在分派表里

## 判据

- `pnpm tools requirements validate` 绿（本节点的 `verify` 指向真实存在的守卫用例）。
- `tools/test/emulator-cxx-dispatch.test.mjs`：**分派表 ↔ handler 声明**两个方向都不许漏，且 17 条在表里。
- `ctest --test-dir apps/emulator-cxx/out/build/windows-x64 --output-on-failure`：selftest / smoke / boot-play / logo 全绿。
- `emulator-cxx --install <安装> --footprint --max-steps 2000000`：仍进到脚本 21092（TITLE），且不撞「未实现」停点。

## 范围怎么定的（可复算）

- 判据不是"emulator-new 有什么"，而是 **boot → TITLE 的脚本链静态足迹**（`0` / `20936` / `20947…21027` / `21089…21092` / `21037`）
  ∩ **C++ 分派表里缺 handler** 的差集 ⇒ 17 条：
  `0x009 0x0a1 0x0a2 0x0a3 0x0d9 0x0ff 0x100 0x12e 0x19b 0x19c 0x1bf 0x1ce 0x1cf 0x1f4 0x1f5 0x208 0x2fc`。
- 排除 audio / text-subsystem / live2d：这 17 条不属于那三个模块（对照 17 条在 `data/spec/chapters/60-ops-*.md` 的规则归属）。
- **动态足迹**（`--footprint`）证明这 17 条里没有一条在本实现的 boot 路径上被取指 —— 也就是说它们**不会**改变"能不能到 TITLE"，
  只会改变 TITLE 的菜单 / 输入 / 状态路径能不能走。范围取静态足迹正是为了覆盖这些没被走到的分支。

## 非显然的口径

- `0x12e` 是**显式未建模**：只写 −1（没有命中）+ 一条首次诊断；条目表 / 偏移数组的布局仍是 audit gap。⛔ 不是"猜出来的命中语义"。
- 门字第 27 位（`0x19c` 置位支）与第 30 位（`0x1ce` 非 0 支）**没有服务分支**：第 30 位按规范是「启用后门恒关」，
  其余位只登记存在。⇒ 执行到这两条之后 `run()` 会一路服务到 step 上限，主循环表现为"门一直关着"。这是规范要求，不是 handler 写错。
- 输入与鼠标是**平台边界**：本层没有输入后端 ⇒ `0xff` 恒采到 0、`0x2fc` 恒"没有事件"、`0x12e` 恒"没有命中"。
  测试通过 `set_input_word()` / `set_key_sampler()` / `set_mouse_event_source()` 注入，⛔ 这几个注入点不是产品路径。
- `0x1f4` 的「区间忙」复用 `0x20c` 的「别处驱动」标记（同一格，语料证据：`0x68EB8`）；
  时间戳复用引擎级的「当前 / 上一时刻」（`0x5A2B4` / `0x5A2B8`），⛔ 不是按帧记录的两格。

## 下一步

- 这 17 条的语义守卫在 `apps/emulator-cxx/tests/core_selftest.cpp`（每条至少一个会红的断言）；C++ 侧没有 `pnpm test` 的变异设施，
  分派层的一致性由 `tools/test/emulator-cxx-dispatch.test.mjs` + `tools/mutate-check.mjs` 的一条变异守着。
- 真要让 TITLE 菜单"点得动"，下一步是接输入后端（键盘 / 鼠标），那属于宿主层，不在本节点。
