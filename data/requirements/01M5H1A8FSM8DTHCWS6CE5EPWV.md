# emulator-new 表现层：Electron 窗口 + Pixi 渲染（按 spec）

- id: REQ-01M5H1A8FSM8DTHCWS6CE5EPWV
- type: req
- status: done
- parent: REQ-01M3TCB0S0BTW41KW0JBJMWHHH
- done_reason: Electron 45 + Pixi 8 + Vite 已接入；窗口/图形/事件入口按 06-window 落成 core/window.ts + platform/electron，显式 ignored 22 条；0x20d/0x33f/0x204 按 60-ops-render 实现并有 pure 守卫；--demo/--smoke 用真实 handler 跑通、截图文字可见；真实 --install 跑到 SYSTEM4.BIN（96196 步 / 314 tick，停在未服务门位 0x2000）。门禁：pnpm typecheck 六工程绿、pnpm test 703/703、spec validate 13/13、electron smoke EXIT=0。残差：纹理像素解码、输入转发、逐位字形/表面格式、present 所有权。
- tags: [emulator, spec]

## 判据

- `apps/emulator-new` 有 Electron 窗口壳与 Pixi 渲染后端；`pnpm --filter @amayui/emulator-new electron` 可启动。
- 启动期的窗口类 / 建窗 / 附加 / 图形设备 / 事件入口按 `data/spec/chapters/06-window.md` 落成**平台无关核心 + Electron 平台实现**；被忽略的项（顶部菜单、IME、图标、精确风格位等）在实现里有**显式标记**，不静默跳过。
- `0x20d` / `0x33f` / `0x204` 按 `data/spec/chapters/60-ops-render.md` 实现，有 `@env pure` 守卫与 fake `RenderHost`。
- 有安装目录时能完成建窗 → 图形就绪 → 根脚本执行；无安装目录时有清晰错误，不伪装成已启动。
- `pnpm typecheck` / `pnpm test` 绿；Electron 冒烟可显式跑。

## 非目标

- 与 D3D9 逐位 / 像素级等价；音频、原生输入、发行打包。
- 把 Electron / Pixi 细节写进 `data/spec/**`（spec 仍是平台边界的真源）。
