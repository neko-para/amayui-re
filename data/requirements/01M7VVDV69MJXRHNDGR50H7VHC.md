# 🐞S2 插件首回调偶发超 2 ms 预算 ⇒ run=failed（start: callback budget exceeded）

- id: REQ-01M7VVDV69MJXRHNDGR50H7VHC
- type: bug
- status: done
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- verify: apps/emulator-lift/tests/plugin_runtime.cpp#plugin/callback-cpu-budget
- repro: apps/emulator-lift/tests/plugin_e2e.cpp#plugin/load-save-e2e
- done_reason: 2 ms 判据改为线程 CPU 钟：Windows 主源 QueryThreadCycleTime + 一次性标定（本机 ≈3.97 cycles/ns）；GetThreadTimes 只作兜底 —— 实测粒度 15.625 ms 整 tick（0.5 ms 忙等读到 0 或 156250），做不到 2 ms，这就是不用它判的理由。窗口 = 用户 JS 本体，OS 抢占 / 等 I/O 不计；宿主段另有墙钟安全网 20 ms（reason 带 (host)）。守卫：plugin/callback-cpu-budget（op_limit=0 + 烧 CPU ⇒ 必须因预算失败且 reason 无 (host)）、plugin/callback-wall-only（主线程被挂起 6 ms ⇒ 墙钟超、CPU 60–487 µs ⇒ 必须不失败）。读数 js_cpu_us 137/70/82/16/137/159/47 全都不是 15625 的倍数；负载下 elapsed_us=641 vs js_cpu_us=140。★ 原「e2e 连跑 ≥10 次 0 次预算失败」这条读数本轮没做（判据由上面两条结构性守卫接管）。
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

## 判据（修好后；★ 2026-11-22 已改为线程 CPU 钟 —— 见末节）

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

## 2026-11-22 收口（提交 `0a15fd1` + `ea20c37`）：判据改成线程 CPU 钟

- ★ **2 ms 的窗口 = 用户 JS 本体**（`script_quickjs.cpp::call()` 的 `js_start_cpu_ns` 起算），**钟 = 本线程
  CPU 时间**；中断回调拿同一把钟比（⛔ 不能"CPU 起点 + 墙钟比较"）。模块装载 / ctx 构造 / TriggerSet 收集 /
  宿主调度不计；被 OS 抢占 / 等 I/O / 睡眠**也不计** ⇒ 并发不再诬告回调（本缺陷的失效方式）。
- Windows 主源 `QueryThreadCycleTime` + 一次性标定（4 个 ~200 µs 忙等，取 max(cycles/wall)，本机
  **≈3.97 cycles/ns**，只做一次；`call()` 在计时窗**之前**预热）。`GetThreadTimes` **只作兜底**：实测粒度 =
  **15.625 ms 整 tick**（0.5 ms 忙等读到 0 或 156250）⇒ **做不到 2 ms**，这就是不用它判的理由。
- 守卫（CTest `plugin-runtime`，顶层注册 ⇒ pure 档）：`plugin/callback-cpu-budget`（`op_limit=0` + 烧 CPU
  ⇒ 必须因预算失败、reason **无** `(host)`）+ `plugin/callback-wall-only`（主线程被挂起 6 ms ⇒ 墙钟超、
  CPU 只有 60–487 µs ⇒ **必须不失败**；win32 专有）。`repo:tools/mutate-check.mjs` 有对应条目（`diag.js_cpu_ns` → `diag.js_ns`）。
- 读数（win32 / 2026-11-21）：7 个回调 `js_cpu_us` = 137 / 70 / 82 / 16 / 137 / 159 / 47（全都**不是** 15625 的
  倍数 ⇒ 亚 tick 钟的独立佐证）；压 6 个忙等进程重跑：`load:confirm2 elapsed_us=641` vs `js_cpu_us=140` ⇒
  墙钟涨、CPU 不涨。宿主段另有安全网 = `deadline × 10`（2 ms ⇒ 20 ms，仍是墙钟，reason 带 `(host)`）。
- **没做**：原 `## 判据` 第 1 条（连跑 ≥10 次 0 次预算失败）本轮**没有**读数 ⇒ 判据由上面两条结构性守卫接管。
