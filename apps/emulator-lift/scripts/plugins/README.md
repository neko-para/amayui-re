# `plugin.d.ts` / `examples`

本目录是**插件运行时**的 API 合同与参考示例；口径与切片见
[`docs/03-emulator/plugin-runtime.md`](../../../../docs/03-emulator/plugin-runtime.md)。

## 目录布局：**入口 vs 库**（S1，2026-11-25）

```
scripts/plugins/
├── plugin.d.ts / README.md      # 合同 + 本说明
├── examples/                    # 类型示例（不参与构建）
├── load-save.js                 # 入口：save 的参考插件（id = load-save）
├── scenario-save07-battle.js    # 入口（id = 文件名 stem）
├── scenario-save10-field.js
├── scenario-fellow-switch.js
├── scenario-field-point-visible.js
└── lib/                         # **只被 import**：永远不是 --plugin 的 id 目标
    ├── save.js                  #   loadSave / SAVE_DIR_REL / 前置检查（没有 default 导出）
    ├── boot-to-title.js · chains.js · script-positions.js · guest-globals.js · recipe.js
```

- **`--plugin <id>` 只查本目录的根**（`<exeDir>/scripts/plugins/<id>.js` 与编译期
  `LIFT_PLUGIN_DIR/<id>.js` 两处，**非递归**）；⛔ 不查 `lib/`、⛔ 不递归子目录。
  需要单独加载的东西必须在根上；库只被 `import`。
- id 语法 `^[A-Za-z0-9_-]{1,64}$`；含 `.` / `/` / `\` 的写法**一律当路径**（所以 `x.js` 是 cwd 相对路径，
  与旧版逐字相同）。两根都命中 ⇒ `exit 2` 并列全部候选；都不在 ⇒ `exit 2` 并列搜过的根与候选。
- ★ **`hud` 是保留 id**（S2）：`--overlay` = 「`--plugin hud` + 要窗口」的预设，解析的是
  `scripts/ui/hud.js`（`<exeDir>/scripts/ui/` → `LIFT_UI_SCRIPT_DIR`），**不是**本目录。
  ⇒ ⛔ 不要在本目录放 `hud.js`：它会**遮蔽**保留 id ⇒ 启动 `exit 2`（列路径 + "改名或给路径"），
  而不是被静默装上（⛔ 不静默被内置件遮住，也不静默换源）。
- import 根 = **入口文件所在目录**：入口用 `./lib/x.js`（向下，允许）；`lib/` 内部用同目录 `./x.js`。
  ⛔ 入口不能住在库的下层（`../` 越根会被 QJS 拒）。

## 合同

- 模块 specifier：`@amayui/lift-plugin`（`plugin.d.ts` 是 ambient module）。
- 插件文件 = ES module：`export default { render?, start?, onTrigger?, initialState? } satisfies PluginModule<State>`。
- 宿主 QJS 装载后只读 **default 导出**；不再读 `globalThis.render/start/onTrigger`，也没有运行时 `definePlugin`。
- `sample` / `screenshot` 不在插件 API 里（它们是 `emu-ctl` 的控制面组合）；插件读 `scriptPos` 用 `ctx.read.scriptPos`。

## 内建插件的配置域

内建插件**不再靠 `--plugin-param` 传具体参数**（那是夹具 / 诊断口）。配置写
`<--appdata>/plugin-config.json`（appdata 根，⛔ 不在 `SAVE/`；`scripts/clean-appdata.sh` 的
`rm -rf` 会把它一起删掉）：

```json
{"version":1,"domains":{"scenario.field-point-visible":{"slot":10,"on":1,"hold":3000}}}
```

优先级：**config > `--plugin-param` > 默认**；"给了"的判据是 `domain.has(key)`（`slot:0` / `on:0`
这种 falsy 值也算给了）。没有 `--appdata` ⇒ `ctx.config.available=false`，插件完全回退到 params。

| 域 id | key | owner（文件） |
|---|---|---|
| `save` | `slot` (number)、`expect` (string[] 或逗号串)、可选 `saveDir` (string，相对 appdata 根)、`timeoutMs` (number)、`precheck` (boolean，缺省 true；false = 跳过存档文件前置检查) | 根入口 `load-save.js`（库 = `lib/save.js`） |
| `scenario.save10-field` | `slot` (number)、`expect` (string[] 或逗号串) | `scenario-save10-field.js`（根） |
| `scenario.field-point-visible` | `slot` (number，缺省 10)、`on` (number/bool，缺省 0)、`types` (hex 串 `"1f,20"` 或数组)、`hold` (number，缺省 3000)、`diag` (number，缺省 0) | `scenario-field-point-visible.js`（根） |

- 域自动创建、不注册、可共享；域 id = `^[A-Za-z0-9._-]{1,64}$`；域 id 与 key 都拒
  `__proto__` / `prototype` / `constructor`。未知顶层键 / 未知域内键**保留**（前向兼容）。
- 新增内建插件要在这里登记它的域（域 id / key / owner）——**消费者只有配置里的域名字符串**，
  没有这张表就只能靠读源码。

## 示例

`examples/` 下的 `.ts` 是**参考插件**（旧 plan 翻译、global 读改写、HUD show/hide、dispatch、具名多 trigger、
rearm、timeout、并发）。它们**不在** `pnpm typecheck` 的六份 tsconfig 里，也不参与构建；改动 API 时用：

```bash
node_modules/.bin/tsc -p apps/emulator-lift/scripts/plugins/examples/tsconfig.json
```

- 示例里的数值都是占位；不要直接当生产脚本。
- 示例只依赖 `plugin.d.ts` 的类型，不需要运行时 shim。
