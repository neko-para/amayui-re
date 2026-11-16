# 🐞S3 AOT 臂从不 guestToken().arm()：容器并发守卫在 AOT 是假绿（--assert-guest-token 全仓不存在）

- id: REQ-01M7JFEA7J8AWQHPXTW6GB41H0
- type: bug
- status: open
- parent: REQ-01M7G7GPBG9BK2360WSG6DBFNP
- repro: apps/emulator-lift/src/runtime/guest_token.cpp#check
- severity: S3
- tags: [emulator-lift, aot, token]

## 现象

AOT 臂**从不** `guestToken().arm()`：全仓唯一生产调用点是解释器 `machine.cpp:947`，`src/aot` 里
`guestToken` **0 命中**。而 `GuestToken::check()` 在未 arm 时**直接 `return true`**（`guest_token.cpp:210-217`）
⇒ 在 AOT 上：

- `violations` / `maxConcurrent` 恒 0（`GuestHeap::alloc/free/realloc`、`HandleTable::add`、`SurfaceArena` 的
  `check()` 全部变成**静默通过**）；
- `--assert-guest-token` 这个旗标**全仓源码里根本不存在**（`rg 'assert-guest-token' src` = 0 命中；
  只有 `README.md:1028` 与若干注释在说它）⇒ 不是"关着"，是"没有"。

⇒ **AOT 上的容器并发守卫是假绿**。今天看不出来（AOT 只有一条 guest 执行流，容器访问都在 shim 内持令牌），
但它是接音频 feeder 的**前置**：feeder 是第二条会写 guest 容器的流，而这是唯一能计数、能进摘要的地方。

## 修法（已定，⛔ 不要重新裁决）

1. AOT 启动时 `guestToken().arm(<与解释器同口径的开档条件>)`，收口时 `disarm()`（位置抄 `machine.cpp:947` / `:1857`）。
2. 摘要里加一行读数（`violations=` / `maxConcurrent=`），使"有没有越界"可观测 —— ⛔ 不要只把开关接上而读数无处可看。

## 判据（每条能红）

1. 一条 pure 用例：未 arm 时 `check()` 恒真；arm 之后同一处越界访问使 `violations` +1 且**摘要打出来**
   （形状抄既有 `guest_token` 用例）。改动前必红（今天 AOT 侧没有 arm 点）。
2. AOT 真跑一轮（assets 档或 `--max-frames` 小值）后摘要出现 `violations=0`；若真跑出非 0 ⇒ **那是缺陷**，
   开新单修那个越界点，⛔ 不许把它调成 0。

## 边界 / 风险

- 已知【推理】"arm 之后今天不会立刻红"（容器访问全在 shim 内持令牌）⇒ 若 arm 后既有用例大面积变红，
  **停下报告**：那说明存在真实的越界访问，不能靠改弱判据过。
- ⛔ 不把 `--assert-guest-token` 补成 CLI 开关（本单不需要；要补另开单）。
