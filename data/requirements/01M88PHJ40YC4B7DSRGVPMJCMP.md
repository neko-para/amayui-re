# 插件宿主 I/O ③：ctx.config（appdata 域式 JSON + 回调末原子 flush）+ 内建插件迁移

- id: REQ-01M88PHJ40YC4B7DSRGVPMJCMP
- type: req
- status: done
- parent: REQ-01M7T0VJPG2S0RZ1HNWW0HH3HY
- verify: apps/emulator-lift/tests/plugin_config.cpp#config/limits-on-load
- tags: [emulator-lift, plugin, qjs, io]

## 需求

`ctx.config = { available, domain(id) }`：宿主在 `<--appdata>/plugin-config.json` 维护**域式 JSON**，插件按域 id 读写自己的 kv；⛔ 不再用命令行传插件具体参数（`--plugin-param` 保留为夹具 / 诊断口）。

## 判据

- 信封 `{ "version": 1, "domains": { "<id>": { … } } }`；域自动创建、不注册、不探测冲突；未知顶层 / 域内键**保留**（前向兼容）。
- 域 id `^[A-Za-z0-9._-]{1,64}$`；域 id 与 key 拒绝 `__proto__` / `prototype` / `constructor`；值 = 原生 JS（对象 / 数组 / 串 / 有限数 / 布尔 / null），⛔ 不是 JSON 字符串（沙箱无 `JSON`）。
- 上限（合同）：深度 8 / 1024 节点 / 单串 8192 B / 文件 1 MiB / 256 域 / 每域 1024 key；**attach 期也校验**（深层但 ≤1 MiB 的文件不会在 `get`/`all` 递归时崩栈）。
- `set` / `delete` 只改内存 + 标脏；**每个插件回调末原子 flush 一次**（临时文件 + `MoveFileEx(REPLACE_EXISTING|WRITE_THROUGH)`）；写失败 warn 一次、不重试风暴、不永久熔断；`applied` = “内存已改”（`code` 仅 `read_only` / `no_appdata`）。
- 坏 JSON / `version != 1` / 域值非对象 ⇒ **装载失败 ⇒ 进程 exit 2**（点名文件）；无 `--appdata` ⇒ `config.available=false`、`domain()` 抛 `no_appdata`；只 `RunCtx`。
- 读 / 写宿主段（含 JSON↔JS 转换）包 `HostPause`；一个 appdata 一个实例（v1 无锁，文档写明）。
- 内建插件优先级 **config > `--plugin-param` > 默认**（`has()` 判“给了”，falsy 也算）；`--plugin-param` 形状错误仍 exit 2。

## 迁移（已交付）

- 域：`save`（`slot` / `expect` / 可选 `saveDir` / `timeoutMs` / `precheck`）、`scenario.save10-field`（`slot` / `expect`）、`scenario.field-point-visible`（`slot` / `on` / `types` / `hold` / `diag`）。
- FIELD 验收：`plugin-config.json`（`scenario.field-point-visible`：`slot 10` / `on 1`）+ 嵌套 appdata，**不带** `--plugin-param` ⇒ `forced=3/3`、`run=finished exit=0`。
- `clean-appdata.sh` 会 `rm -rf` 整个 appdata ⇒ 配置一起删；⛔ 不放 `SAVE/`。

## 落点（已交付）

- `src/runtime/config.h` · `src/plugin/plugin_config.{h,cpp}` · `PluginHost::attachConfig` · `ui/plugin.h` · `script_quickjs.cpp` · `plugin.d.ts` · `plugin-runtime.md` §4.5/§4.6/§4.9/§6/D1 · `scripts/plugins/README.md` 域表 · `saves/README.md`。
- 守卫：CTest `plugin-config`（`config/roundtrip|unknown-preserved|domain-autocreate|bad-json-exit2|bad-top-level|bad-domain-value|proto-key|limits|limits-on-load|limits-key-name|invalid-utf8|missing-root|document-size|flush-on-callback|cross-instance-read|read-only-receipt|no-appdata`）· 进程级 `plugin/bad-config-exit2` · `plugin-runtime#control-script/config-roundtrip` · `plugin/config-host-pause` · 变异 `proto-key` / `unknown-preserved` / `flush-on-callback` 各实测红。
