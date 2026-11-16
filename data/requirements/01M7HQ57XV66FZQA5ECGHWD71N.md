# 解释器边界：冻结为验证臂 · 3 条可搬 AOT · 4 条靠补 --report/--lst · 6 条依赖 --native-overrides 只能退役

- id: REQ-01M7HQ57XV66FZQA5ECGHWD71N
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- done_reason: 解释器已按用户裁决（2026-11-16）退役：6 条 CLI 级 CTest（cli-time-limit / cli-cursor-warp / exit-code / host-encoding / stdout-quiet / cli-dump-frame）与 emulator-lift target、src/main.cpp 一并删除；13 条驱动解释器的 CTest 全部归位（1 条搬 AOT、6 条退役、6 条随 target 消失）；🐞S2 已修（guest_exit.h + lift-guest-exit）。退役代价登记在 REQ-01M7J6D62QFRZC2454GV7CKJDJ。
- tags: [emulator-lift, boundary, interpreter]

## 一句话

**解释器 = 冻结的验证臂**：不再新增功能，只保留"有 CTest 依赖它"的面；它的**唯一护城河是 `--native-overrides`**（AOT 结构上不该有）。
本单只登记边界与迁移顺序，**不删任何东西**。

## 现状（2026-11-16 实测复核 —— ⛔ 旧表"3 可搬 / 4 靠诊断面"的归属是错的）

| 分类 | 条数 | 谁 | 结论 |
|---|---|---|---|
| **真能搬** | **1** | `lift/no-gpu-refuses-start` | 实测 `lift-gpu-batch --no-gpu-refuses-start out/emulator-lift-aot` = 9 checks/0 failures、两臂 stderr 逐字相同 ⇒ 只改 CMake 一个实参 |
| **退役（用户裁决 2026-11-16）** | **5** | `cli-cursor-warp` · `cli-dump-frame` · `cli-time-limit` · `exit-code` · `stdout-quiet` | 夹具是**测试自造的最小 PE32**，而 AOT 的 IR 是构建期从**被 pin 的那份 PE** 抬升的 ⇒ 换 PE 结构性无意义（实测 `exit 2` pin 核对失败）。其中 `stdout-quiet` 还靠 `--native-overrides`（§12.8 禁止的第三份语义实现）。★ 用户裁决：**随解释器一起退役**，⛔ 不按真 PE 重写 |
| **退役** | **6** | `feeder-seg-e2e` · `movie-logo` · `stress-*`（4 条） | 用户裁决：AOT 已远超解释器速度，6 条 override 压力读数"目标已经没有了" |

★ `--native-overrides` 是**结构性解释器专有**：AOT 的执行源只有 lifted IR + shim。
★ ⛔ **`--lst` 永不给 AOT 补**：`tools/test/aot-arm-shape.test.mjs` 的 33 个禁词里有 `--lst`（补了就红）。
★ ⛔ **`--report` 也不补**：它读 `BlockCache` 的覆盖账，AOT 结构上没有（AOT 侧只有派发表/计数，语义不同）。
★ `host-encoding` 例外：夹具用真 PE（pin 不挡），但它读的 ② 是解释器 `usage()` 第一行、③ 要 `--report` + `game-dir` 回显
⇒ 归入同一裁决：**退役**（⛔ 不补 `--report`）。

★ **覆盖缺口（显式登记，⛔ 不许静默丢）**：这 6 条退役后，**AOT 的 CLI 面**（参数解析 / 退出码 / stdout 静默）
在本仓**没有任何端到端断言**了 —— 只剩 pure 单测与 assets 档的真跑。🐞`REQ-01M7HP2YRZG222E0NE2H6MW6NK`
就是这类缺口抓到过的真实缺陷 ⇒ 再出现同类问题只能靠人工两臂比对或新立 assets 用例。

## 规则（本单生效）

1. **AOT = 产品臂**：面向用户/自动化的开关只进 AOT；新 CTest 默认挂 AOT，挂不上要写理由。
2. **解释器只做"保绿"**：不新增功能；`overlay` / `profile` / `--dump-frame` 按需再补，今天冻结。
3. **不许给解释器开第二套输入语义**（ADR `REQ-01M7HP2Q1N0N004M8R4RPABWSV`）。
4. **两臂比对的现状如实写**：今天**没有**自动化两臂比对；它唯一真正发挥作用的一次是撞出 AOT 丢 guest 退出码
   （`REQ-01M7HP2YRZG222E0NE2H6MW6NK`）。⛔ 不要把"可以比对"当成已兑现的收益。

## 判据（每条能红）

1. 搬一条 ⇒ AOT 上跑通且**判据逐条不变**。⛔ 尤其防"静默改弱"：`cli-time-limit` 的五项比对里
   `trace-in` / `blocks` AOT 都没有，而 `lineStartingWith` 缺行返回占位串 ⇒ 两边都缺 ⇒ `a==b` 恒真。
2. 修 🐞S2 ⇒ AOT 的 guest 退出码映射与解释器一致（非零 ⇒ 2），且 `exit-code` 的
   `exit-process(3)` 不再是"pin 给的假绿"。
3. 冻结生效 ⇒ 解释器臂的开关数量**不再增加**（口径，不建守卫）。
4. 退役前置：`REQ-01M7HP2YZ4CRMQHYVSS8HH611Z`（配方未走完 ⇒ exit 5）—— **已 done**（verify → `CMakeLists.txt#input-plan-read-points`）。

## 退役顺序

1. ✅ 修 🐞S2（`return r.ok() ? 0 : 2` → 失败优先 + `exitCodeForGuestExit(shim.exit_code)`；映射搬到纯头
   `src/runtime/guest_exit.h`，AOT 仍未 include `machine.h`）。
2. ✅ **已做**：`lift/no-gpu-refuses-start` 换成 AOT 臂（判据一条未改；9 checks/0 failures；变异施加点同步换到
   `aot_main.cpp`，否则变异打解释器/守卫跑 AOT ⇒ 假绿）。✅ 修 🐞`REQ-01M7HP2YRZG222E0NE2H6MW6NK`
   （`guest_exit.h` 纯头 + `lift-guest-exit` 纯档守卫；AOT 的 e2e 结构性测不到，如实记）。
   ⇒ 剩下的就是"6 条退役 + 删 target"。
3. 6 条**退役**（5 条 CLI 夹具 + `host-encoding`）· AOT 压力/性能新档见 `REQ-01M7HQJEEENYDJ91FB5DAC4K23`。
4. 最后才删 `emulator-lift` target。⚠ 硬前置：`$<TARGET_FILE:emulator-lift>` 共 **7 条实参**，
   且有 **8 条 mutation 条目**（`tools/mutate-check.mjs` 的 `guard:'ctest:<名字>'`）绑在 5 个名字上
   ⇒ 删/改名必须同轮处理，否则 `pnpm test:mutation` 报"守卫不存在"、configure 直接失败。
   ⚠ 别顺手删 `InteractionGraph` 类：`input-expect` / `profile-window` 还在用（CLI 入口已 0 命中）。
