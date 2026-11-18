# 🐞S3 AOT 臂缺 --font / --font-engine-name（随解释器退役删除，文档仍在引用）

- id: REQ-01M7Q9D7NMFVKAN4PY57B5Y5MM
- type: bug
- status: open
- parent: REQ-01M7JHSD5MWTB9N8WJC2Y2T69M
- repro: apps/emulator-lift/src/aot/aot_main.cpp#usage
- severity: S3
- tags: [emulator-lift, font, cli]

## 判据（存在性 + 观测）

- 现象：AOT 臂（唯一产品臂）不认 `--font` / `--font-engine-name`：`./out/emulator-lift-aot --font x` ⇒ exit 2「未知参数」。
- 溯源：解析它们的 `apps/emulator-lift/src/main.cpp` 随解释器 2026-11-16 退役被删（b6afe77）；`src/aot/aot_main.cpp` 的 `usage()` 与参数循环里没有这两个参数，全仓只有 `tests/ui_text.cpp` 调 `fontSetConfig`。
- 但文档仍把它们当现有能力：`repo:docs/03-emulator/fonts.md`（`--font` / `--font-engine-name` 的 A/B 档）、`repo:apps/emulator-lift/SCOPE.md` §5（调试覆盖 / 显式 A/B 对照档）。
- 影响：文档里的 A/B 复现命令跑不了；用户无法换宿主字体做对照。默认路径不受影响（主字体仍是 Amayui CN）。

## 修法（待做）

1. 在 `aot_main.cpp` 装回 `--font <path>`（`FontConfig::regular_path`，bold 由 `fontSetConfig` 派生）与 `--font-engine-name`（`use_engine_name`），落地时机与 `--baseline-lift` 相同（任何 FontEngine / SdlWindow 之前）。
2. 更新 `usable()` / `--help` / `docs/03-emulator/fonts.md` 的 A/B 命令；补 `tests/cli_aot_flags.cpp` 的参数面（mac 上是 SKIP，win32 才有判据）。
3. 边界：⛔ 不改字形来源策略（主字体仍是 Amayui CN；`--font-engine-name` 只改上报族名）。

## 不做 / 边界

- 不把 `--font` 做成"按族名查系统字体"；那是另一个设计。
- 本单与 `REQ-01M7Q9C16JR9ATFJK11A873B26`（基线按字号）分开：那张单只装回 `--baseline-lift*`。

## 复现

- `./apps/emulator-lift/out/emulator-lift-aot --font corpus/assets/fonts/Amayui-CN_cnjp.ttf --help` ⇒ 现状 exit 2；修好后 `--help` 里能看到两项。
