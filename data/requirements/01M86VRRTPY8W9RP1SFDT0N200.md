# 插件路的 loadSave 翻页点击会丢（2/3 run 翻不到页 ⇒ FELLOW 场景红）

- id: REQ-01M86VRRTPY8W9RP1SFDT0N200
- type: bug
- status: done
- parent: REQ-01M7G7EJF062NKQ0D40RF3BZ4Z
- verify: tools/test/plugin-loadsave-page.test.mjs#loadsave-page/first-click-swallowed
- repro: apps/emulator-lift/scripts/plugins/lib/scenario-fellow-switch.js#loadSave(9
- severity: S2
- tags: [emulator-lift, input, plugin, defect]

## 结论（2026-11-25 已修）

根因不是"点击被吞"或坐标：`save.js` 的 `PAGE_STALL_LIMIT = 60` 来自错误假设（"60 个读点 ≈ 1 s"），
实测读点 **≈366 个/s** ⇒ 60 拍 ≈ 0.16 s；而一次插件点击的抬起要跨两个完整 guest 回调
（闸门②′）⇒ 页号变要 **76–461 个读点**。旧窗口比点击自身还短：正常时靠 `turnr` 重试接住，
第一下真被吞时重试自己也要 76 拍、只剩 60 ⇒ 结构上救不回来；多余的重试点击还会把页多翻一格。

- 修法：`PAGE_STALL_LIMIT` **60 → 1200**（≈2.6× 实测上界）+ `load:row` 点行前再核一次页号。
- 实测：同一构建、同一 `saves/` ⇒ 旧 60 **2/2 红**（签名逐字同）；新 1200 **3/3 到 REIGN.BIN**
  （calls 141/333/313）；槽 7 → FIELD → 插件 `forced=1/1` → `exit 0`。
- 守卫：`tools/test/plugin-loadsave-page.test.mjs`（3 条，`loadsave-page/first-click-swallowed` 等）+
  变异 1/1 红；`pnpm test` 621 pass。
- ⛔ 没有改 `saves/`、没有用 `--input-plan`。

## 现象

`scenario-fellow-switch.js`（`scripts/plugins/lib/`）的 `loadSave(9)` 在插件路上**翻页点击会丢**：

- 2026-11-24 / win32 / dev `f58c2b7`，`aot-stress-fellow-switch` 三次独立 run：
  **2 次**停在 `load-save: page not reached (target 0, still 1)`（翻页门两轮各 60 读点都没等到页号变 ⇒
  `ctx.finish(5)`），**1 次**翻过去了。
- 同一份 `saves/`、同一天 ⇒ **是抖的**，不是"页号一定不对"。今天的持久化页是 **1**
  （两次 run 都读到 `page 1 (target 0, slot 9)`）。
- `save.js` 的页链在点 `(45,357)` / `(1235,357)` 之后靠"再等页号变"兜（60 读点 + 一次重试）；
  LOAD 屏读点慢（实测 ~0.3–0.5 次/s）⇒ 那一下丢了就撞满 `--time-limit 240`。

## 判断（待核）

`IntentExecutor` 的插件路**不施加"闸门②'"**（按下必须跨过完整回调；`scenario-fellow-switch.js` 文件头
已记这条缺口）⇒ 与 `REQ-01M7M967MKB65PCY3HAJRTFX9X`（单次 click 不保证按下被一拍看到）同一类，
但那条只修了 `--input-plan` 路。**`<待核>`**：把闸门②' 接进插件路是否就能消掉抖动。

## 影响

- `aot-stress-fellow-switch`（`REQ-01M7HQJEEENYDJ91FB5DAC4K23` 的路径 2 自动档）3/3 红，已暂时
  `DISABLED`（见该 CTest 的注释）。
- 任何依赖 `loadSave` 的插件场景（含 `scenario-save10-field.js`）都有同一抖动风险。

## 判据

- 同一 `saves/`、同一窗口连跑 **5 次**，`loadSave(9)` 全部到达 REIGN.BIN（`run=finished exit=0`）；
- 把 `FELLOW_READ_INPUT_LO/HI` 换成轮询体（`script-positions.js` 的负对照）⇒ 门超时 ⇒ `finish(5)`
  （证明"能红"仍在）；
- `aot-stress-fellow-switch` 恢复注册并通过（删掉 `DISABLED`）。
