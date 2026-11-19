# 🐞S3 AOT 臂从不 guestToken().arm()：容器并发守卫在 AOT 是假绿（--assert-guest-token 全仓不存在）

- id: REQ-01M7JFEA7J8AWQHPXTW6GB41H0
- type: bug
- status: done
- parent: REQ-01M7G7GPBG9BK2360WSG6DBFNP
- verify: apps/emulator-lift/CMakeLists.txt#aot-token-summary
- repro: apps/emulator-lift/src/runtime/guest_token.cpp#check
- severity: S3
- done_reason: 四条修法交付：arm（4dcfb42）+ disarm（aot_run.cpp 收口，抄 machine.cpp:1879）+ 摘要 token 行（aot_host.cpp，只在 phase0Enabled 时打）+ 真 run CTest aot-token-summary（--profile 断言恰好一行/与解释器同形；缺省档断言不出现）。判据 2 的 violations=0 未达到：实测 violations=2 first=GuestHeap::free（确定性），按判据 2 后半开单 REQ-01M7SAHRTGZ760B35CQXASXT0R，未归零。形态守卫 tools/test/aot-token-shape.test.mjs 只补 disarm 删除（证明力弱：不证明运行期）。
- tags: [emulator-lift, aot, token]

## 收口（2026-11-19）

三条缺口已补：

- ① **`disarm()`**：`apps/emulator-lift/src/aot/aot_run.cpp` 的收口处（`audio_.end()` 之后，与 `arm()` 同一条
  单出口）调 `guestToken().disarm()`；落点与理由抄解释器 `machine.cpp:1879`（`end()` 之后、计数保留、只关 armed）。
- ② **摘要**：`AotHost::printRunSummary`（`aot_host.cpp`）加一行
  `token     : violations=<n> max-concurrent=<m>[ first=<site>]`，**与解释器 `machine.cpp:2732` 同一个格式串**；
  它只在 `guestToken().phase0Enabled()` 时打（与紧邻的 Phase-0 四行同一个闸门）⇒ 缺省档 stdout 逐字不变。
  读数（2026-11-19 / mac arm64 / RelWithDebInfo，`--profile --max-frames 1`）：
  `token     : violations=2 max-concurrent=1 first=GuestHeap::free`。
- ③ **守卫**：真 run 的 CTest `aot-token-summary`（`apps/emulator-lift/CMakeLists.txt`，顶层 ⇒ pure）：
  `--profile --max-frames 1` 断言**恰好一行**且与解释器同形；缺省档断言这一行与 Phase-0 四行**都不出现**
  （钉住 ② 的闸门）。另有形态守卫 `tools/test/aot-token-shape.test.mjs`（arm/disarm 成对 + 闸门，**证明力弱：
  不证明运行期**，只补运行期抓不到的 `disarm` 删除）。
- `--assert-guest-token` 仍**全仓不存在**（本单边界已裁"不补"；没有变）。

⚠ **判据 2 的读数不是 0**：`violations=2 first=GuestHeap::free`（`--max-frames 1/1/3` 三次相同）。
按本单判据 2 的后半"若真跑出非 0 ⇒ 那是缺陷，开新单修那个越界点" ⇒ 已开
`REQ-01M7SAHRTGZ760B35CQXASXT0R`（AOT 块循环不持令牌：续延在令牌外 free guest 堆），⛔ 没有把读数改成 0。
本单收口的是"arm / disarm / 摘要 / 守卫"四条修法；**`violations=0` 这一条由那张新单跟踪**。

## 现象（2026-11-17 立单时）

AOT 臂**当时从不** `guestToken().arm()`：全仓唯一生产调用点是解释器 `machine.cpp:947`，`src/aot` 里
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
   开新单修那个越界点，⛔ 不许把它调成 0。（2026-11-19 结果：非 0 ⇒ 已开 `REQ-01M7SAHRTGZ760B35CQXASXT0R`。）

## 边界 / 风险

- 已知【推理】"arm 之后今天不会立刻红"（容器访问全在 shim 内持令牌）⇒ 若 arm 后既有用例大面积变红，
  **停下报告**：那说明存在真实的越界访问，不能靠改弱判据过。
  ★ 2026-11-19 实测：不是"大面积变红"，而是真 run 的 `violations=2`（见上）⇒ 已按本条开新单，没有改弱判据。
- ⛔ 不把 `--assert-guest-token` 补成 CLI 开关（本单不需要；要补另开单）。
