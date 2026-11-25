# `plugin.d.ts` / `examples`

本目录是**插件运行时**的 API 合同与参考示例；口径与切片见
[`docs/03-emulator/plugin-runtime.md`](../../../../docs/03-emulator/plugin-runtime.md)。

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
| `save` | `slot` (number)、`expect` (string[] 或逗号串)、可选 `saveDir` (string，相对 appdata 根)、`timeoutMs` (number) | `lib/save.js` 的 `export default` 参考入口 |
| `scenario.save10-field` | `slot` (number)、`expect` (string[] 或逗号串) | `lib/scenario-save10-field.js` |
| `scenario.field-point-visible` | `slot` (number，缺省 10)、`on` (number/bool，缺省 0)、`types` (hex 串 `"1f,20"` 或数组)、`hold` (number，缺省 3000)、`diag` (number，缺省 0) | `lib/scenario-field-point-visible.js` |

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
