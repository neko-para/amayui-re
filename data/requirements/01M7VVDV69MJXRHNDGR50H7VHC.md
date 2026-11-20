# 🐞S2 插件首回调偶发超 2 ms 预算 ⇒ run=failed（start: callback budget exceeded）

- id: REQ-01M7VVDV69MJXRHNDGR50H7VHC
- type: bug
- status: open
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- repro: apps/emulator-lift/tests/plugin_e2e.cpp#plugin/load-save-e2e
- tags: [emulator-lift, plugin, qjs]

## 现象

C2c 手工 headless（`--present headless --skip-logo --audio off --time-limit 90 --plugin …/lib/save.js --plugin-param slot=5`）
3 次同命令、机器无其它负载，其中 **1 次**：

```
plugin    : … calls=1 run=failed reason=start: callback budget exceeded
```

其余 2 次 `calls=7 run=done`。⇒ **首回调（`start`）偶发超过 2 ms / 100k ops 预算**。

## 为什么不是"C2c 的链错"

- 同一份 `save.js`、同一 appdata、同一条命令：2 次成功；失败那次停在 `calls=1`（`start` 刚返回/刚执行完）。
- 失败面是 C2a 的预算执行器（`PluginHost::runCallback` 的 deadline + op_limit），不是链的坐标/门。
- 影响：任何"顺带守预算"的 e2e 会随机红；生产里插件首回调也可能被误杀（用户可见：插件起不来）。

## 待查（按可疑度）

1. `start` 的预算是否把 **QJS 冷启动 / 首次 JIT / 模块副作用**算进 2 ms（`script_quickjs.cpp` 的 deadline 起点）；
2. `Number.isInteger` / 对象构造 / 日志是否在预算内；预算是否应只罩"用户代码"而不罩首次解释器暖机；
3. 失败读数是否只有 `reason`、没有实际耗时（要加 opt-in 的 elapsed 读数，⛔ 不进默认输出）。

## 判据（修好后）

- 同命令 headless 连跑 ≥10 次，`start: callback budget exceeded` = 0（不是"偶发变少"）；
- 若保留预算，超限必须仍响亮失败（⛔ 不把预算失败改成静默跳过）；
- e2e `plugin/load-save-e2e` 由"可能随机红"变成稳定绿（win32 上复核）。

## 不做

不改 C2c 的链 / 坐标；不把预算放宽成"永远够"（那会让跑飞的插件拖死 guest）；不把失败转 77 掩盖。
