# 插件宿主 I/O ②：loadSave 缺 SAVE<NN>.DAT 在首个 click 前 finish(5)

- id: REQ-01M88PHG1XEKQADS9YQHGFCFP9
- type: req
- status: done
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- verify: tools/test/plugin-loadsave-page.test.mjs#loadsave-page/save-missing
- tags: [emulator-lift, plugin, qjs, io]

## 需求

`loadSave(slot, …)` 在**第一个 click 之前** `stat <saveDir>/SAVE<NN>.DAT`；缺文件 ⇒ `ctx.finish(5, …)` 且**零 click**，把“缺档”从 30 s 门超时变成首拍可定位的失败。

## 判据

- 只查 `.DAT`：⛔ 不查 `.STH`（缺失不阻止读档）、⛔ 不查 `SAVE.DAT`（它不是槽索引，是脚本键持久化池）。
- slot 0..99；`slot >= 100` ⇒ warn + 跳过（`SAVE%2.2d` 三字形未取证）。
- 无 `ctx.appdata` / `available=false`、或 `stat(saveDir)` 报 `escape|invalid_path|denied|io` ⇒ **跳过 + 一行 warn**；`not_found|not_dir` ⇒ **按缺文件 fail**（目录不存在也算缺档）。
- `precheck:false` 显式跳过（一行 info）；`saveDir` 选项 / 配置 `save.saveDir` 可覆盖（相对 appdata 根）——给“存档在安装树（`set:UseAppDataFolder != 1`）”留逃生口。
- 有文件时链形状与旧行为逐行相同；失败 reason 点名绝对路径 + slot。

## 命门

- 2026-11-24/25 的“点行不弹确认框”曾被归因成页基 / 空槽；真正根因是把 `saves/` 复制到 `<appdata>` **根目录**（引擎读 `<appdata>/Local/Eushully/<标题>/SAVE/`）⇒ 列表为空。前置检查让这类配置错误在首拍现形。

## 落点（已交付）

- `scripts/plugins/lib/save.js` 的 `SAVE_DIR_REL` / `precheckSaveFile`；`plugin.d.ts`；`saves/README.md` §反例。
- 守卫：`tools/test/plugin-loadsave-page.test.mjs` 的 `loadsave-page/save-missing|save-dir-missing|save-precheck-off|no-appdata|save-dir-escape`；`plugin-runtime#control-script/save-missing-precheck`（临时 appdata，断言 0 click + code 5）。
