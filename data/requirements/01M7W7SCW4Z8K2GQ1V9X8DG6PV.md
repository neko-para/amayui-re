# P3 · temp/appdata/saves/paths/skip 收口

- id: REQ-01M7W7SCW4Z8K2GQ1V9X8DG6PV
- type: req
- status: open
- parent: REQ-01M7W7RSC34YJTNMK3ESYVXZY2
- tags: [emulator-lift, tests, fixtures]

## 做什么

- `temp_directory_path` 全部改 `support` 的 `TempDir`（RAII 清理）；
- `--appdata` 统一 `makeAppdata`（每跑一份私有目录）；
- `plugin_e2e` 的 saves 复制改 `copySaves(saves_dir, appdata, {SAVE05.DAT, SAVE05.STH, SAVE.DAT, RT.DAT})`（D4）；
- `LIFT_AGE_EXE` 的 7 条 `ENVIRONMENT` 与 8 条 argv 至少统一成 `paths.h` 的取法（注入方式可保留）；
- `support/skip.h` 收掉 9 处重复判据（GPU / 字体 / 拒绝启动 / 生成物）；删 `subprocess_skip.h`，逻辑并进 `skip.h`。

## 判绿

- `rg 'temp_directory_path' apps/emulator-lift/tests` ≤ 2（只在 `support/`）。
- `rg '拒绝启动|没有可用的 GPU 光栅化后端|宿主字体不可用' apps/emulator-lift/tests` 只剩 `support/skip.h`。
- `pnpm verify` 绿；`ctest` 与迁移前同结论；`saves/` 未被写入（`git status` 干净）。

## 边界

- ⛔ 不动 `saves/`（只读）；⛔ 不改 `check.h` 位置（D2）；⛔ 不把 `plugin/load-save-e2e` 拆出去（D3）。
