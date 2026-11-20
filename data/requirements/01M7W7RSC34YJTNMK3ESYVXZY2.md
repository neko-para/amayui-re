# 测试基建收口（P1–P5）：跨平台 spawn · temp/appdata/saves · skip 判据 · BMP/像素，各收一份

- id: REQ-01M7W7RSC34YJTNMK3ESYVXZY2
- type: req
- status: doing
- parent: REQ-01M7JHSFB2SSBBNSJ82YDED8M1
- tags: [emulator-lift, tests, toolchain]

## 一句话

把测试里重复的 10 套 spawn 脚手架、临时目录 / appdata / saves 复制、skip 判据、BMP / 像素工具各收成**一份**；
P1 已落地（`4062f05`），P2–P5 按子单推进。判据是**行为同结论 + 重复面归零**，⛔ 不是"测试变绿"。

## 为什么要收

- 10 处各写一套 `CreateProcessW` / `fork+execv`（其中 3 处已迁，剩 7 处）⇒ 改超时/收尸/CRLF 要改十遍，且只有部分平台能跑。
- `temp_directory_path` / `--appdata` / saves 复制 / `LIFT_AGE_EXE` 取法各写一遍；`skip.h` 之外还有 9 处重复 skip 判据。
- BMP / 像素比较三套读法（`BmpPixel` / 通道差 / 整文件相等）混在一起，改一处容易改弱判据。

## 已裁决（D1–D5，⛔ 不重新裁决）

- **D1 后端 = std/POSIX**（`tests/support/subprocess`），不引 Boost.Process；P2 做完若双后端维护成本仍高再评估
  （唯一受影响文件 `subprocess.cpp`）。
- **D2 `check.h` 留在 `tests/check.h`**（需求单 `repro:` 锚指着它）；`tests/*.cpp` 不搬家（`verify:` 锚 +
  `mutate-check.mjs` 的 `file:` + `input-plan-shape` 硬编码三处会同时红）。
- **D3 `plugin/load-save-e2e` 不拆独立 CTest**（仍是 `plugin-headless` 的子用例）。
- **D4 saves 复制 = 显式列表** `{SAVE05.DAT, SAVE05.STH, SAVE.DAT, RT.DAT}`；⛔ 不全量；`saves/` 只读。
- **D5 `stress/skip-report` 的合成负控留在 `LIFT_STRESS` 档**；`subprocess_skip.h` 逻辑收编进 `support/skip.h` 后删除。

## 不做

- 迁 Boost.Test：120 个 `main` / 241 处 `finish()` / 失败上下文 / `SKIP_RETURN_CODE 77` 四条语义重建，代价 >> 收益。
- 引 Boost.Filesystem / JSON（已有 `std::filesystem` / `nlohmann-json`）。
- 把 `add_test` 收进函数/宏（会让 pure/assets 分档判错、`cmake-test-entry` 解析不到）。
- 改任何断言 / 阈值；改 `control_snapshot` 的逐字节相等（那是"同一帧字节相同"的判据）。

## 判绿（整体）

- `rg 'CreateProcessW|std::system\(' apps/emulator-lift/tests` 只在 `tests/support/` 命中。
- `rg 'temp_directory_path' apps/emulator-lift/tests` ≤ 2（只在 `support/`）。
- skip 文案（"拒绝启动" / "没有可用的 GPU 光栅化后端" / "宿主字体不可用"）只剩 `support/skip.h`。
- `pnpm verify`（含 `cmake-test-entry`）绿；`ctest -N` 名称集合与 P1 后一致（除 P4 新增 `support-bmp`）。
- 每期**一次只迁一个文件 / 一条 CTest，一 commit 一 revert**；开工前存一份 `ctest -N` 名单对照。

## 落点

- 普查与逐条判绿：`.tmp/test-infra/survey.md`（临时区；结论已摘进本单与子单）。
- 父级裁决：`.tmp/test-infra/decisions.md`。
