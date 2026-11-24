# 插件 settle 回调的 2 ms CPU 预算把冷启动算进去（js_cpu_us=2217 ⇒ run=failed）

- id: REQ-01M86VRT1YP53CJS0M885FCCJM
- type: bug
- status: open
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- repro: apps/emulator-lift/scripts/plugins/lib/scenario-fellow-switch.js#settle:
- severity: S3
- tags: [emulator-lift, input, plugin, budget]

## 现象

`scenario-fellow-switch.js` 的**第 1 个 `settle` 回调**被判成超预算：

```
plugin : … run=failed reason=onTrigger: callback budget exceeded: scenario:settle:0
         elapsed_us=11122 ok=0 calls=1 build_us=57 js_us=11037
         js_cpu_us=2217 collect_us=3 gap_us=4 module_load_us=2879 budget_us=2000 over=js
```

- 2026-11-24 / win32 / dev `f58c2b7`，`aot-stress-fellow-switch` 的 run 1（240 s 窗口）。
- 预算 = `src/runtime/budget.h` 的 `kCallbackDeadlineNs`（2 ms CPU）。`module_load` / 建对象 / 收集
  **都不在**预算窗里（读数分开打），但冷启动的 JIT/QuickJS 初始化仍记在 `js_cpu_us` 里。
- 没有 CLI 口子能放宽它（`aot_main` 没有 `--plugin-budget` 一类开关）。

## 要裁决的问题

1. **2 ms 是不是"每次回调"的口径**？若是，第 1 次回调（冷）是否应单独给 warmup 预算（或按
   "回调序号 >1 才判"）？
2. 还是把 `scenario:settle:0` 的 `js_cpu_us` 里"首次进入"的成本拆出去（例如把 `module_load` 之外的
   首次解释器初始化也记到 `build_us`）？
3. ⛔ 不要把预算整体放宽到"看起来能过"（那会让真超时的回调也静默通过）。

## 判据

- 同一 `scenario-fellow-switch.js` 连跑 5 次，第 1 个 `settle` 回调不再 `over=js`；
- 构造一个**真的**超预算的回调（死循环 / 长循环）⇒ 仍 `run=failed`（证明判据没被放宽掉）；
- `aot-stress-fellow-switch` 在翻页缺口修好后一起复跑。
