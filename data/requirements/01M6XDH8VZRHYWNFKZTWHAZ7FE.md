# emulator-lift · 默认 ctest 瘦身：贵用例（option-roundtrip / movie-logo）移出默认档，档位判据 = 注册位置

- id: REQ-01M6XDH8VZRHYWNFKZTWHAZ7FE
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- verify: apps/emulator-lift/CMakeLists.txt#DEFINED ENV{LIFT_ASSETS}
- done_reason: 档位判据 = add_test 的注册位置（顶层无条件 ⇒ 默认档；落在 if() 里 ⇒ assets 档，解析只在 tools/lib/ledger.mjs 的 cmakeTestTier 一处）。可观测面 = ctest -N 的条数与名字：默认档不含 option-roundtrip / movie-logo；$env:LIFT_ASSETS=1 重新 configure 后它们出现，去掉变量再 configure 又消失。没有能红的自动守卫（拆掉闸只会让默认门禁变慢，不会让任何断言失败）⇒ 判据写在本字段；verify 挂 CMakeLists.txt 的 DEFINED ENV{LIFT_ASSETS} 锚 = 那两处 if 的闸（配置期求值）。不丢回归的替代证据 = 同案 pure 回归 d3d9/texture-bound-then-guest-release 仍在默认档。
- tags: [emulator-lift, ctest]

## 一句话

`apps/emulator-lift` 的**默认 `ctest` 只跑免安装的 pure 档**：两条按墙钟秒计的最贵用例（真启模拟器的 `option-roundtrip`、`movie-logo`）**不注册**，要显式 opt-in 才进默认门禁。

## 档位判据 = **注册位置**（不是清单、不是标签）

- 顶层**无条件** `add_test(...)` ⇒ **pure** ⇒ 默认 `ctest` 就跑（免游戏安装）。
- 落在 `if()` / `foreach()` / `function()` / `macro()` / `while()` / `block()` 里 ⇒ **assets** ⇒ 默认门禁不跑。
- 解析只有一处：`tools/lib/ledger.mjs` 的 `cmakeTestTier()`；它自己的守卫在 `tools/test/ledger.test.mjs`（把"一律 pure"退化成常量 ⇒ 当场红）。
- 所以本单**没有**新增清单文件 / 标签：把 `add_test` 挪进 `if(...)` 就是"移出默认门禁"，挪回顶层就是"移回默认门禁"。

## `DEFINED ENV{LIFT_ASSETS}` 是**配置期**求值（最容易踩的一条）

`if(EXISTS "…/install/SYS4INI.BIN" AND DEFINED ENV{LIFT_ASSETS})` 在 **configure** 时求值，且 `add_executable` 与 `add_test` 都在同一个 `if` 里（纯构建环境连 target 都不编）。

⇒ **只设环境变量、不重新 configure，`ctest -N` 不会变**。正确顺序：

```powershell
cd apps/emulator-lift
$env:LIFT_ASSETS=1; cmake --preset windows-x64; cmake --build --preset windows-x64
ctest --test-dir out -R "option-roundtrip|movie-logo" --output-on-failure
# 跑完必须**去掉环境变量再 configure 一次**回到默认档：
Remove-Item Env:LIFT_ASSETS; cmake --preset windows-x64
ctest --test-dir out -N   # 条数必须回到默认档
```

## 怎么查（判据的可观测面）

- `ctest --test-dir out -N`：条数 = 默认档。**本轮实测（2026-11-08）默认 69 条**；`$env:LIFT_ASSETS=1` 重新 configure 后 71 条（多出 `option-roundtrip` / `movie-logo`）。
- `ctest --test-dir out -R "option-roundtrip|movie-logo"`：默认档下应报 **0 tests**（不是"跳过"——是**根本没注册**）。
- `ctest --test-dir out -N -V` 看每条用例的注册位置来源；也可 `node tools/mutate-check.mjs --list` 看到哪些守卫在默认档（`buildDir: apps/emulator-lift/out` 那些）。
- 默认档的总时长：`ctest --test-dir out --output-on-failure` 末行 `Total Test time (real)`。

## 为什么瘦身不等于丢回归

- 这两条贵用例的价值是"真启模拟器跑真实脚本"，它们**仍然是可跑的**，只是默认不跑（opt-in 档）；删掉它们才是丢回归。
- 默认档里保留**同类判据的 pure 版**：例如纹理"被 guest 释放后仍绑定"的回归在 `d3d9`（顶层注册 ⇒ 默认档）的 `d3d9/texture-bound-then-guest-release` 里。这是本单"不丢回归"的替代证据 —— 真机/真脚本档 opt-in，语义判据本身留在默认档。
- ⛔ 不要为了让默认档变快而**放宽**任何守卫（删断言 / 把 SKIP 当 pass）。

## 判据

- 默认档 `ctest` 绿且**不含** `option-roundtrip` / `movie-logo`（`ctest -N` 里没有这两个名字）。
- `$env:LIFT_ASSETS=1` 重新 configure 后这两条**在**且能跑；去掉变量再 configure 后它们**又不在**。
- "同案 pure 回归仍在默认档"：`d3d9/texture-bound-then-guest-release` 在默认 `ctest` 里被执行（不是 SKIP 通过）。

## 边界（别当成已覆盖）

- 档位判据只认**注册位置**：把 `add_test` 留在顶层却让它内部 SKIP，仍然是"默认门禁在跑它"（这是另一类问题，见测试卫生：缺席要 `LIFT_SKIP`，不许 `diagnostic` + 裸 `return`）。
- `option-roundtrip` 的轮数不是用例名字符串里的 `3cycles`（名字是历史串），轮数由 `kCycles` 决定（已订正为 **2 轮**）——档位与轮数无关。
