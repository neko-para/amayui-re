# `plugin.d.ts` / `examples`

本目录是**插件运行时**的 API 合同与参考示例；口径与切片见
[`docs/03-emulator/plugin-runtime.md`](../../../../docs/03-emulator/plugin-runtime.md)。

## 合同

- 模块 specifier：`@amayui/lift-plugin`（`plugin.d.ts` 是 ambient module）。
- 插件文件 = ES module：`export default { render?, start?, onTrigger?, initialState? } satisfies PluginModule<State>`。
- 宿主 QJS 装载后只读 **default 导出**；不再读 `globalThis.render/start/onTrigger`，也没有运行时 `definePlugin`。
- `sample` / `screenshot` 不在插件 API 里（它们是 `emu-ctl` 的控制面组合）；插件读 `scriptPos` 用 `ctx.read.scriptPos`。

## 示例

`examples/` 下的 `.ts` 是**参考插件**（旧 plan 翻译、global 读改写、HUD show/hide、dispatch、具名多 trigger、
rearm、timeout、并发）。它们**不在** `pnpm typecheck` 的六份 tsconfig 里，也不参与构建；改动 API 时用：

```bash
node_modules/.bin/tsc -p apps/emulator-lift/scripts/plugins/examples/tsconfig.json
```

- 示例里的数值都是占位；不要直接当生产脚本。
- 示例只依赖 `plugin.d.ts` 的类型，不需要运行时 shim。
