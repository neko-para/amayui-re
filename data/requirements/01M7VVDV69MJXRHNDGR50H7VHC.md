# 🐞S2 插件首回调偶发超 2 ms 预算 ⇒ run=failed（start: callback budget exceeded）

- id: REQ-01M7VVDV69MJXRHNDGR50H7VHC
- type: bug
- status: open
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- repro: apps/emulator-lift/tests/plugin_e2e.cpp#plugin/load-save-e2e
- tags: [emulator-lift, plugin, qjs]

## 现象

C2c 手工 headless（`--present headless --skip-logo --audio off --time-limit 90 --plugin …/lib/save.js --plugin-param slot=5`）
**5 次同命令、机器无其它负载，其中 2 次**：

```
# 第 1 次
plugin    : … calls=1 run=failed reason=start: callback budget exceeded
# 第 4 次
plugin    : … calls=5 run=failed reason=onTrigger: callback budget exceeded: load:sbunki
```

其余 `calls=7 run=done`。⇒ **不止 `start` 冷启动**：`onTrigger` 的 `load:sbunki`
（3 个终态 trigger + click + log）也会偶发超 2 ms / 100k ops。

## 为什么不是"C2c 的链错"

- 同一份 `save.js`、同一 appdata、同一条命令：3 次成功；两次失败分别停在 `calls=1`（`start`）与
  `calls=5`（`onTrigger: load:sbunki`）——都不是链的坐标/门错，而是回调被预算截断。
- 失败面是 C2a 的预算执行器（`PluginHost::runCallback` 的 deadline + op_limit），不是链的坐标/门。
- 影响：任何"顺带守预算"的 e2e 会随机红；生产里插件回调也可能被误杀（用户可见：插件起不来 / 链停在中间）。

## 待查（按可疑度）

1. 预算的 deadline 起点是否把 **QJS 冷启动 / 首次 JIT / 模块副作用**算进 2 ms（`script_quickjs.cpp`）；
   `start` 与 `onTrigger` 两条路径是否共用同一个起点口径；
2. `Number.isInteger` / 对象构造 / `ctx.log` / `click` 是否在预算内；预算是否应只罩"用户代码"而不罩
   首次解释器暖机 / 宿主 `log`；
3. 失败读数是否只有 `reason`、没有实际耗时（要加 opt-in 的 elapsed 读数，⛔ 不进默认输出）；
4. 100k ops 是否被 QJS 的 op 计数口径放大（`start` 一次就要 5 位数的 ops？）。

## 判据（修好后）

- 同命令 headless 连跑 ≥10 次，`callback budget exceeded`（`start` 与 `onTrigger` 两处）= **0**
  （不是"偶发变少"）；
- 若保留预算，超限必须仍响亮失败（⛔ 不把预算失败改成静默跳过）；
- e2e `plugin/load-save-e2e` 由"可能随机红"变成稳定绿（win32 上复核）。

## 不做

不改 C2c 的链 / 坐标；不把预算放宽成"永远够"（那会让跑飞的插件拖死 guest）；不把失败转 77 掩盖。

## 2026-11-21 补充（同一个执行器的第三处落点）

- 把 `save.js` 的确认门挪到读段之后（`c464651`）之后，链在 headless e2e 里**更常**走到 `load:confirm2`，于是失败面变成
  `onTrigger: callback budget exceeded: load:confirm2`（同一族：2 个 expect 终态 trigger + click + log）。
- 本机（无其它负载）3 跑 = 2 绿 1 红；同一 ctest 在 `c464651`（未拆分）上 3/3 绿 —— **不足以归因到重构**
  （失败的是墙钟 2 ms，`src/runtime/budget.h`；成功面不回显子进程日志，无法确认它走没走同一拍）。
- 判据不变（连跑 ≥10 次 0 次 budget 失败）；建议先加 `待查 #3` 的 opt-in elapsed 读数，再决定"放宽 / 只罩用户代码"。
