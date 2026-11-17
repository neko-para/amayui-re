# AOT boundary 桶切六刀：把块边界宿主税的 29% 切成可指名的子桶（≥10% 才动）

- id: REQ-01M7JV8S4MGY87K76YDQ9WRKA8
- type: req
- status: open
- parent: REQ-01M7JHSEZVMYB4XHEN32V7V6HJ
- tags: [emulator-lift, efficiency, profile]

## 一句话

把 `boundary`（块边界宿主税）从**一行 29%** 切成六行具名子桶，再看**哪一条 ≥10%**：
控制层闸门（`kCheckStride`）· 停机判定 · `--time-limit` 检查 · `pumpHostEvents` · 合成 EA 泵 · 定时器泵。

## 为什么

`fellow-init`（打开 FELLOW 的 1.4–2.3 s）窗口 profile 的结论是：`lift` 50% · **`boundary` 28.7–29.9%** · `shim` 17.5–18.6%，
且**没有任何单条 shim ≥10%**（最大 `PeekMessageA` 5.3%）⇒ 按"具名行 ≥10% 才算候选"的口径，
**`boundary` 是唯一够格的候选**，但它现在是**六件事的合计**，所以"候选"还不等于"知道该改哪一条"。
出处：`REQ-01M7JNX5MR7RWTBNBAE70PBCQP` 的落地读数（`coverage` 1.20–1.22，读数有效）。

## 判据

1. **六个子桶各自一行**，且**闭式不变**：`wall = lift + boundary + shim + partial + instrument + unaccounted`（误差 ≤1%），
   `boundary` == 六个子桶之和（误差 ≤1%）。⛔ 不许把某一项漏掉再"差额算进最大项"。
2. **`coverage` 仍在 `[0.5, 2]`**，否则本次读数作废（口径同窗口 profile）。
3. **子桶划分按"哪一段代码"**，不按"猜的原因"：每段 = 一个可指名的函数/区间（`文件:符号`），
   ⛔ 不许出现"其它"这种兜底桶（兜底只许 `unaccounted`，而它结构上恒 ~0）。
4. **不改 guest 可见语义、不改默认档输出**：新增子桶只在开窗时打印；不开窗一行不多。
5. **只有 ≥10% 的子桶才算下一刀的候选**；全部 <10% ⇒ 结论写"边界税均匀分布，暂不优化"（合格交付，⛔ 不为凑产出挑 5% 的行）。
6. 单条变异：让某个子桶不再累加（或归到别的桶）⇒ 判据 1 的"六项和 == boundary"当场红。

## 落点

`apps/emulator-lift/src/aot/aot_run.cpp`（六段都在同一处：块顶 → 分派前）+ `window_prof.{h,cpp}`（桶与闭式）+
`tests/aot_window_profile.cpp`（子桶求和的守卫）+ `tools/mutate-check.mjs` 一条。

## 未做

不预判任何子桶该占多少（那正是本单要测的）· 不顺手改 `kCheckStride`（先量再裁）· 不与 `lift` 桶的派发表查找归属
（窗口 profile 偏离 5）合并处理。

## 静态对照（2026-11-17 · arm64 · 只读普查）

- 静态块边界税 **27.71%**（693,001 / 2,500,533；每 guest 指令 1.99 条），与 profile 的 `boundary` 28.7–29.9% **同量级同方向但不是同一个量**（静态分子不含 trampoline 的闸门/泵/派发表，多算了分支/ret/flags）⇒ ⛔ 不能拿 27.71% 复核 29%。详见 `.tmp/insn-overhead/report.md` §5。
- 静态上**没有单桶 ≥10% 可一刀砍**（最大 `movImm` 14.0%，分散在 10,254 个函数）。唯一接近两位数的是 `lift_tick`（站点级 12.2%；**不属本单六桶**，它在 `lift` 里）⇒ 本单六刀即使切完，单桶很可能仍 <10%。
- 优先级建议：先做 `REQ-01M7FRY04PTP9788QZPVPKA5ND` 的 **tick 内联**（动态 −50%~−62% 的 tick，≈−10%~−14% 执行条数 <待核>）；本单保留为**归因工具**，不据此改判据。
