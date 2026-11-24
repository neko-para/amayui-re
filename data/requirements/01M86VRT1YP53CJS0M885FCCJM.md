# 插件 settle 回调的 2 ms CPU 预算把冷启动算进去（js_cpu_us=2217 ⇒ run=failed）

- id: REQ-01M86VRT1YP53CJS0M885FCCJM
- type: bug
- status: done
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- verify: apps/emulator-lift/tests/plugin_runtime.cpp#plugin/callback-host-pause
- repro: apps/emulator-lift/scripts/plugins/lib/scenario-fellow-switch.js#settle:
- severity: S3
- tags: [emulator-lift, input, plugin, budget]

## 结论（2026-11-25 已修）

根因：2 ms 预算罩住了用户 JS 里调宿主能力 `ctx.profile.end` 的那段（生成窗口报告 + 写系统剪贴板；
缺陷现场 `js_cpu_us=2217`、后续窗口 2.56–2.90 ms）。修法 = **宿主调用暂停**：`HostPause` 在宿主调用
前后取 `pluginThreadCpuNs()` 差，累加进 `paused_ns`；`js_cpu_ns` 与中断判据饱和扣除。
2 ms 数值、`ctx.profile.end` 路径、`AotHost` 剪贴板合同都不变；**本轮只接 `profile.end`**
（`log` / `profile.begin` / `edit` 未接，真出现超预算再按同一原则逐个接）。

- 守卫：`plugin/callback-host-pause`（假宿主 5 ms 忙等 ⇒ `js_cpu_us≈7`、`paused_us≈5001`；删 pause ⇒
  `js_cpu_us=5002 over=js` 红）+ `plugin/callback-js-band`（原生 10 ms 循环仍 `over=js`）；变异 1/1。
- S3：`aot-stress-fellow-switch` 删 `DISABLED`，**5 次连续 Passed**（~241 s/次，6 窗口/exit 0）。
- 文档：`budget.h` / `docs/03-emulator/plugin-runtime.md`（§4.2 / §4.6 / D5 / §12 表 / 行号映射）
  已同步"宿主调用暂停 + 本轮只接 `profile.end`"。

## 现象

`scenario-fellow-switch.js` 的**第 1 个 `settle` 回调**被判成超预算：

```
plugin : … run=failed reason=onTrigger: callback budget exceeded: scenario:settle:0
         elapsed_us=11122 ok=0 calls=1 build_us=57 js_us=11037
         js_cpu_us=2217 collect_us=3 gap_us=4 module_load_us=2879 budget_us=2000 over=js
```

- 2026-11-24 / win32 / dev `f58c2b7`，`aot-stress-fellow-switch` 的 run 1（240 s 窗口）。
- 预算 = `src/runtime/budget.h` 的 `kCallbackDeadlineNs`（2 ms CPU）。`module_load` / 建对象 / 收集
  **都不在**预算窗里（读数分开打），但宿主 `profile.end`（报告 + 剪贴板）仍在 `js_cpu_us` 里。
- 没有 CLI 口子能放宽它（`aot_main` 没有 `--plugin-budget` 一类开关）。

## 裁决（2026-11-25）

1. 2 ms **仍是每次回调**（⛔ 不做"回调序号 >1 才判"的 warmup 特例）。
2. 宿主能力调用（`ctx.profile.end`）**暂停**：它的 CPU 不进用户 JS 的 2 ms；⛔ 不整体调高预算。
3. ⛔ 不放宽判据：真超预算的 JS 回调仍必须 `over=js`（`plugin/callback-js-band`）。

## 判据

- 同一 `scenario-fellow-switch.js` 连跑 5 次，第 1 个 `settle` 回调不再 `over=js`；**5/5 Passed**；
- 构造一个**真的**超预算的回调（死循环 / 长循环）⇒ 仍 `run=failed`；
- `aot-stress-fellow-switch` 在翻页缺口修好后一起复跑（已 5/5 Passed）。
