# emulator-lift · CMake：测试二进制收进 out/tests/ + LIFT_BUILD_TESTS 开关（默认 ON）

- id: REQ-01M76ZBNGRWASYF8D5GFVTN2GH
- type: req
- status: done
- parent: REQ-01M6H6G6DSRG1ZRD810Z0HTH8V
- done_reason: CMake 布局改造完成：LIFT_BUILD_TESTS（默认 ON）+ out/tests/ 收口 + cmakeTestTier 透明规则；读数见正文（out 110→25、off 臂 0 个 lift-*、ctest 83、pnpm test/verify 全绿）
- tags: [emulator-lift, cmake, build, tests]

## 一句话
`apps/emulator-lift/out/` 根下散落 **87 个测试可执行文件**；把它们统一到 `out/tests/`，并加 `LIFT_BUILD_TESTS`（默认 ON）控制"是否构建测试"，让产品构建（`-DLIFT_BUILD_TESTS=OFF`）只产出 `emulator-lift`。

## 现状（2026-11-12 本机 macos-arm64）
- `ls -1 out | wc -l` = 110；`find out -maxdepth 1 -type f -perm +111 | wc -l` = **87**。
- 测试 target 都在 `CMakeLists.txt` 里 `add_executable(lift-*)`；`add_test` 用 `$<TARGET_FILE:>` 解析路径。
- `tools/lift.mjs:1089-1090` 硬编码 `apps/emulator-lift/out/lift-bench(.exe)`（`lift-bench` = `tests/bench.cpp` 的测试 target）⇒ 改目录后必须同步。
- `tools/mutate-check.mjs` 的 CTest 条目只认 `buildDir`（需复核有没有二进制路径假设）。

## ★ 已裁决（⛔ 不要重新裁决）
- 开关名 = `LIFT_BUILD_TESTS`，**默认 ON**（现有门禁不变）。
- 测试二进制目录 = `${CMAKE_BINARY_DIR}/tests`；**产品二进制 `emulator-lift` 仍在 `${CMAKE_BINARY_DIR}`**（`tools/lift.mjs` 的 bench、`$<TARGET_FILE:>`、文档命令都依赖它）。
- **不允许**把 `add_test` 挪进 `if(...)` 后让 `cmakeTestTier` 判成 assets：分级真源 = 注册位置（AGENTS §5）。⇒ 必须同步改 `tools/lib/ledger.mjs` 的 `cmakeTestTier`，把**唯一条件**为 `LIFT_BUILD_TESTS` 的 `if` / 对应 `endif` 视作**透明**（不是 tier 门），并加用例。

## 要做
1. CMake 顶层加 `option(LIFT_BUILD_TESTS "Build emulator-lift tests / CTest suite" ON)`；把测试 target + `add_test` 整体包进 `if(LIFT_BUILD_TESTS)`；`-DLIFT_BUILD_TESTS=OFF` 时只构建 `emulator-lift`。
2. 测试 target 的 `RUNTIME_OUTPUT_DIRECTORY` 指到 `${CMAKE_BINARY_DIR}/tests`（Ninja 单配置；如支持 multi-config 则同时给 `_DEBUG/_RELEASE/...`）。产品 target 不许改。
3. `tools/lift.mjs` 的 `out/lift-bench` 路径 → `out/tests/lift-bench`（Windows `.exe` 同步）。
4. `cmakeTestTier`：`if(LIFT_BUILD_TESTS)` 透明；`tools/test/ledger.test.mjs` 加用例：透明 if 里的 `add_test` ⇒ `pure`；`if(LIFT_BUILD_TESTS AND WIN32)` ⇒ 仍 `assets`；`if(NOT LIFT_BUILD_TESTS)` 不透明（保持保守）。
5. 文档：仓库里指向 `out/lift-*` 的命令改到 `out/tests/lift-*`；`ctest --test-dir out` 不变。
6. 门禁（逐条贴读数）：`cmake --preset macos-arm64` → build → `ctest --test-dir out`（82/82 + 资产用例）；`-DLIFT_BUILD_TESTS=OFF` 重新 configure 只 build `emulator-lift` 并确认 `out/tests` 无新二进制；恢复 ON 后 `pnpm test` / `pnpm verify` / `pnpm tools requirements validate` 绿。

## 判据
- `ls out` 根只剩 `emulator-lift`（+ CMake 生成物 / 库）；测试可执行文件全在 `out/tests/`。
- `-DLIFT_BUILD_TESTS=OFF` 的构建树里**没有 `lift-*` 可执行文件**。
- `ctest` 仍能解析并跑全部已注册用例；`cmakeTestTier` 分级不变（ledger 用例 + `pnpm tools spec validate` 的证据档不降级）。
- `pnpm test` / `pnpm verify` 绿。

## 风险
- 资产用例的 `add_executable` 本就在 `if(EXISTS ... AND DEFINED ENV{LIFT_ASSETS})` 里；外面再包 `LIFT_BUILD_TESTS` = 双层 if，它们本来就是 assets，不受透明规则影响。
- `out/` 里的历史二进制不会被自动清理（生成物、gitignored）；验收用新构建树或先 `cmake --build --target clean`。
- 透明规则只认**唯一条件** `LIFT_BUILD_TESTS`；`if(NOT LIFT_BUILD_TESTS)` 不透明（否则会把反向分支也当无门）。

## 收口（2026-11-12，macOS arm64）
- `option(LIFT_BUILD_TESTS ... ON)`；测试 target / `add_test` 全包进单个 `if(LIFT_BUILD_TESTS)`；测试二进制落 `out/tests/`。
- 读数：`out` 根 110 项 → **25** 项；根下可执行只剩 `emulator-lift`；`out/tests` 74 个可执行；`ctest -N` 仍 **83**；
  `ctest -E overlay-window-smoke` = **82/82**（5 个 by-design SKIP）。
- OFF 臂：`cmake --preset macos-arm64 -B /tmp/lift-notests -DLIFT_BUILD_TESTS=OFF` → build `emulator-lift` 成功；
  `find /tmp/lift-notests -maxdepth 1 -name 'lift-*'` = **0**；`ctest -N` = **0 tests**。
- `cmakeTestTier` 透明规则（唯一条件 `LIFT_BUILD_TESTS`）+ ledger **40 pass**；`spec validate` 13/13 未登记 0；
  `pnpm test` 546；`pnpm verify` 460；`requirements validate` 6/6。
- 顺带：删 `out/` 根下 12 个旧测试二进制（6 个资产用例 + 6 个已无源码的历史 target）；`README.md:271/275` 的
  `out/lift-movie-logo.exe` 路径与"三次/3 轮"陈旧口径订正为 `out/tests/…` / 两次（`kCycles=2`）。
- 未做：`tools/mutate-check.mjs` 无需改（CTest 条目只认 buildDir + ctest 名）。
