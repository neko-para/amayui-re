# 插件宿主 I/O ①：ctx.appdata（path + 同步只读 stat）

- id: REQ-01M88PH73NFG3CG6FD238NPCGW
- type: req
- status: done
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- verify: apps/emulator-lift/tests/plugin_appdata.cpp#appdata/stat-escape
- tags: [emulator-lift, plugin, qjs, io]

## 需求

插件 `RunCtx` 加**同步只读**宿主 I/O：`ctx.appdata = { available, path, stat(rel) }`；只 appdata 子树、只 RunCtx（`render` 的精确六成员不动）。

## 判据

- `stat(rel)` 返回 `{ exists, isFile?, isDir?, size?, error? }`；错误码 `no_appdata | invalid_path | escape | not_found | not_dir | denied | io`；**“不存在”是正常结果**（不抛）。
- 只读（⛔ 无 read / write / 列目录）；拒绝对路径 / `..` / NUL / 控制字符 / 非法 UTF-8；canonical 之后必须仍在 appdata 根下（否则 `escape`）；`size` 只在文件且取到大小给。
- stat 的宿主段包 `HostPause`（网络盘 / 杀软耗时 ⛔ 不计进用户 2 ms）。
- 无 `--appdata` ⇒ `available=false`、`path=null`，不报错；`--appdata` 落在 `--game-dir` 安装树里 ⇒ `attach` 失败（不当 appdata 根）。

## 落点（已交付）

- `src/runtime/appdata.h` · `src/plugin/plugin_appdata.{h,cpp}` · `PluginHost::attachAppData` · `aot_main` 接线 · `ui/plugin.h` · `script_quickjs.cpp` · `plugin.d.ts` · `plugin-runtime.md` §4.5/§4.6/§4.9/§6。
- 守卫：CTest `plugin-appdata`（`appdata/stat-file|stat-missing|stat-dir|stat-escape|stat-absolute|stat-no-appdata|stat-empty|stat-nul|stat-invalid-utf8|stat-symlink-escape|attach-*`）；`plugin/api-surface` 把 `appdata` 加进 REQUIRED；`render-surface` 不变。

## 裁决

- 错误走**结果对象**、⛔ 不把 `errno` / 平台文案漏给 JS（跨宿主稳定）。
- `path` 的口径 = 绝对宿主路径（Windows 反斜杠），失败消息用它定位。
