# 插件的 2 ms JS 预算把宿主 trampoline 也算了进去（profile.end 一次约 2.35 ms）

- id: REQ-01M81XWG2GQJPVJZ65484A8WDV
- type: req
- status: open
- parent: REQ-01M7JHSDWT13XEZJHEET2BDH1C
- tags: [emulator-lift, plugin, budget]

# 🐞S3 插件的 2 ms JS 预算把宿主 trampoline 也算了进去（`profile.end()` 一次 ≈2.35 ms）

- id: (auto)
- type: req
- status: open
- parent: REQ-01M7JHSDWT13XEZJHEET2BDH1C

- tags: [emulator-lift, plugin, budget, perf]

## 一句话

插件回调的 2 ms 预算是按 **CPU 时钟**扣的，但它把回调体内的**宿主 trampoline** 也算进 `js_cpu_us`。
`ctx.profile.end()` 一次要做 `endWindowText` + `setSystemClipboard`（宿主），实测 **≈2.35 ms CPU**
⇒ 一个只调用 `profile.begin/end` 的**合法**场景会被预算判红（`over=js`），而它根本没在 JS 里做重活。

## 读数（win32 / 2026-11-22 / dev）

- 触发臂：`scenario:settle:0` ⇒ `js_us=2796 / js_cpu_us=2463 / budget_us=2000 / over=js`。
- 对照臂（临时注释 `ctx.profile.end()`）：`js_us=114 / js_cpu_us=112` ⇒ 差 ≈ **2351 µs**。
- 上述 e2e 的 3 趟正式跑（6 次 FELLOW 切换）**没有**触发（0 条 `over=`）⇒ 这是边界账，不是常态。

## 判据（修法要点）

1. 预算窗口只计 **JS 体**（QuickJS 执行时间）；宿主 trampoline（`profile.*` / `finish` / `quit` 等）
   不计入，或至少用一张**显式**的"宿主函数白名单"扣掉。
2. **负对照**：一个纯 JS 死循环 / 长循环仍必须被判红 —— 预算不能被架空。
3. 读数：同一场景 `js_cpu_us` ≤ 2000，且 `over=` 不再出现；3/3 正式 e2e 仍 `run=finished exit=0`。

## 复现

```bash
cd apps/emulator-lift
.\out\emulator-lift-aot.exe --game-dir ../../links/install --appdata ../../.tmp/plug-click/appdata-1 `
  --present headless --skip-logo --audio off --time-limit 300 --log-level info `
  --profile --plugin scripts/plugins/lib/scenario-fellow-switch.js
# 观察 settle 阶段的 js_us / js_cpu_us / budget_us / over
```
