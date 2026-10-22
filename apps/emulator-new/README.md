# apps/emulator-new —— 只读 `data/spec` 的参考实现

> ⛔ **本目录是 spec 的消费者，不是 spec 的来源。**
> 它**只读 `data/spec/`**（指令表 + 章节规则）；⛔ 不 import `@amayui/age-format`、
> `data/shape`、`data/ledger`、`corpus/`，也⛔ 不复制 `apps/emulator`（已废弃，仅作架构参考）。
> 守卫：`tools/test/emulator-new-isolation.test.mjs`（会红）。

## 1. 这一层做什么

**真实入口**：安装目录 → `SYS4INI.BIN`（ALF 索引）→ **统一文件 id == TOC 条目下标** → 取根脚本 **id 0**（`SYSTEM4.BIN`）
→ 解析脚本头 → **执行**：取指 → 按 opcode 选处理器 → 执行 → 按「操作数个数」槽推进 `4 × (2×argc+1)` 字节；
门关着时只推进虚拟时钟并服务状态位（`70-vm.md`）。

- 取件顺序：**安装根下的松散文件优先**，否则按 TOC 的 `(archive, offset, length)` 从 `DATA*.ALF` **切片**（不整份读）。
  ⛔ 不许"直接读某个脚本文件"当启动：那会跳过索引与初始化流程（`--script` 只是调试口）。
- ALF 目录区：4 KiB 环形窗口 LZSS；**匹配的位置是窗口内的绝对写位置**（不是距离），窗口初值 0、写指针起点 `0xFEE`
  —— 这两点是"能把真实 21109 条目的索引解出来"的实现判据（见 `tools/test/emulator-new-alf.test.mjs` 与 boot assets 守卫）。

- **启动不是只有脚本**：`src/core/boot.ts` 把 `_WinMain@16` → 主循环之间的阶段逐条登记（`modeled` / `partial` / `host-only` / `not-modeled`）；
  `--install` 的文本与 `--json` 都输出这份清单。⛔ 跳过的阶段（窗口 / 自更新 / 定时器…）不许静默消失；配置只到 **partial**（库已建立，显示设备副作用未建模）。
- **配置**（`55-config.md`）：索引尾部的发行默认表 → 可选 `SYS4REG.INI` 覆盖 → 配置库；
  `--config <file>` 指定玩家文件；缺省路径 = 配置根（`USEAPPDATAFOLDER`：用户配置目录 / 安装目录）+ `REGFILEPATH` + 文件名。
  `--init-config` 把当前配置写成默认文件（必须配 `--config`；不写安装目录）。
  脚本的 `0x1a2` / `0x1a3` / `0x1a9` / `0x1aa` 只支持全局值族（type 3 / 5）；引用族先响亮失败。
- **CLI 入口**没有渲染 / 音频 / 输入；外部请求只记进 `m.effects`（`data/spec/core/README.md` §1 裁决）。
  **Electron 前端**（`src/platform/electron/**`）把 `06-window.md` 的窗口 / 图形设备 / 事件入口与
  `60-ops-render.md` 的 `0x20d` / `0x33f` / `0x204` 接到 Electron + Pixi：窗口、画布、渲染目标与离屏文字是真的；
  音频、输入转发、纹理像素解码仍是显式未建模（UI 的 diagnostics 面板逐条列出）。
  ★ **core 整份跑在 renderer**：`renderer/session.ts` 直接调用 `analyzeInstall` / `bootWindow` / `Machine`；
  main 只保留 BrowserWindow、文件系统 / 规范表读取、IPC 与进程生命周期。桥接协议见 `platform/electron/protocol.ts`。
- **未实现即停**（不 no-op）：缺口带 `{指令序号, 偏移, opcode, argc}`，停在它**前面**，可以当门禁用。
- `handler` 表只包含 `data/spec` 里**已成篇**的 opcode；解不出的操作数类型 / 未取证的值一律**响亮失败**。
- 整型编码的 key 按规范**运行期生成**；虚拟时钟步长、未初始化浮点的读法是**显式参数**：⛔ 没有"悄悄用 0"的默认路径。
- `--decode` 是纯解码走查（不解语义），用来检验 `CORE-ISA` 的结构规则够不够实现一个解码器。

## 2. 怎么跑

```bash
# 真实入口：安装目录 → 索引 → 根脚本 id 0（key 由实现运行期生成；--key 仅用于复现位模式）
node apps/emulator-new/src/main.ts --install "<安装目录>"

# 配置：读显式玩家文件；缺失 ⇒ 发行默认。生成一份默认文件（要求 --config）
node apps/emulator-new/src/main.ts --install "<安装目录>" --config /tmp/SYS4REG.INI --init-config

# 换一个统一文件 id（例：0x5262 = LOGO.BIN）做诊断
node apps/emulator-new/src/main.ts --install "<安装目录>" --root-id 0x5262

# 调试口：直接给一份脚本文件（不是启动路径）
node apps/emulator-new/src/main.ts --script <file>

# 命中停止条件（如 0x20f play-movie）就停下（停止条件是"停在它前面"，不执行它）
node apps/emulator-new/src/main.ts --script <file> --stop-at 0x20f

# 仿真参数：时钟步长（毫秒）与未初始化浮点策略（zero ⇒ 读成 0 并记诊断）
node apps/emulator-new/src/main.ts --script <file> --tick-ms 16 --uninitialized-float zero

# 墙钟模式：等待门 / 渐变按真实毫秒走（5 秒渐变不会被虚拟步长快进；CLI 会按 4ms 睡）
node apps/emulator-new/src/main.ts --install "<安装目录>" --real-time

# 只解码（走完代码区），验证头 / 切分 / 表指针
node apps/emulator-new/src/main.ts --script <file> --decode

# 机器可读（直接 node；`pnpm` 会先打印脚本横幅，会污染 JSON）
node apps/emulator-new/src/main.ts --script <file> --json

# 运行中看进度（stderr；每 N 步一行）
node apps/emulator-new/src/main.ts --install "<安装目录>" --progress 10000

# 流式看**外部 / 未建模**效果与诊断（stderr；stdout 仍只有最终报告）
node apps/emulator-new/src/main.ts --install "<安装目录>" --trace

# 连内部记账（string.set / array.lookup / engine.scalar.*）也逐条打
node apps/emulator-new/src/main.ts --install "<安装目录>" --trace-all

# JSON 里带**全量**效果明细（默认只带 effectCounts；可能上千条）
node apps/emulator-new/src/main.ts --install "<安装目录>" --json --effects
```

### `--key` 是什么

`--key` 不是游戏的安装 / 解密密钥，而是引擎**整型池位模式**的编解码密钥（`40-pools` 的
`ENC/DEC`）。引擎在构造期用启动时刻的毫秒值播种、`rand` 重抽到**非 0**，每个进程一份，
之后不再改变；它不是常量，也不由脚本 / 配置 / 用户输入提供。

**正常情况下不需要传 `--key`**：emulator-new 按规范在运行期生成一个非 0 值（`generateIntKey`）。
对已实现的语义路径，任意非 0 key 的行为等价（编码 / 解码互逆，池里的原值相同）；
只有要复现某一组**原始池位模式**时，才用 `--key <非0 u32>` 注入显式值。
`--key 0` 不是合法值（会破坏 `ENC(0) ≠ 0` 与引擎的非 0 前置），入口直接拒绝。

默认是**静默执行**：运行中不打印，结束后一次性输出报告；需要中间输出就用 `--progress` / `--trace`。
`--trace` 默认只打 **external + unmodeled**（音频 / 纹理 / 输入 / 未建模调用）；`string.set`、`array.lookup`、`engine.scalar.*` 这类**已建模的内部记账**只在 `--trace-all` 里出现。

### Electron 窗口（Pixi 渲染）

```bash
# 依赖：electron / pixi.js / vite 已在 apps/emulator-new/package.json
pnpm --filter @amayui/emulator-new build:renderer

# 非游戏的可视化自检：真窗口 + Pixi，用真实 core handlers 驱动
# 0x1f8 / 0x204 / 0x1fb / 0x33f / 0x20d（不读游戏安装）
# 同时会打开一个独立的只读 debug 窗口（diagnostics 投影）
pnpm --filter @amayui/emulator-new exec electron . --demo

# 同上 + 截图到 .tmp/emulator-new-electron-smoke.png 后退出（自动化用）
pnpm --filter @amayui/emulator-new exec electron . --smoke

# 真实入口：安装目录 → 索引 → 窗口 / 图形设备 → 根脚本
pnpm --filter @amayui/emulator-new exec electron . --install "<安装目录>"
```

★ 受限沙箱里 Electron 需要 `--no-sandbox`（放在 `electron .` 与 app 路径之间）；正常桌面不要加。
★ main / preload 由 Electron 的 Node 直接跑 `.ts`（Node 24 类型剥壳）；renderer 走 `vite build`，
  并且 **core 的 `.ts` 也由这次构建一起打进 renderer bundle**（不是 main 的运行时）。
  `dist/renderer/**` 是生成物（已 gitignore）。
★ **两个窗口**：
  - **game 窗口**：只承载目标 canvas；core / VM / 场景 / Pixi 都在这个 renderer 里；
  - **debug 窗口**：先建、默认可见，加载同一个 renderer bundle 的 `?debug=1` 分支，只读显示
    `HostDiagnostics` / boot / status / log 投影。
  core 的 `createWindow` 调整的是 game 窗口；debug 窗口不跑 core、不回写状态。
★ **窗口顺序**：game renderer 自己需要一个 BrowserWindow 才能跑 ⇒ main 先建隐藏的 game bootstrap 窗口；
  core 的 `createWindow` 再按配置调整同一个窗口并 `show()`。见 `window-host.ts` 头注释。
★ **桥**：renderer 的 `Host` 走 `host.*`（文件 / 环境）、`BootHost` 走 `boot.*`（窗口 / 显示器）；
  core 的 `MessageHost` 保持同步，main 把 close / focus 事件推进 renderer 的本地队列。
  game renderer 的低频 diagnostics 投影走 `renderer:event` → main → `debug:event`（约 100ms 节流）。
  ⛔ main 不 import `core/**`（隔离守卫会红）；debug 窗口不能修改 core 状态。
★ `--demo` / `--smoke` 是**非游戏**自检：它们不证明与真机等价，只证明窗口 + Pixi + spec handler 这条链路能跑。
★ **时钟**：CLI 用虚拟 `tickMs`（显式仿真参数，适合无头快跑）；Electron 的 `--install` 传宿主毫秒时钟
（`MachineOptions.clock`），所以渐变 / 等待按**墙钟**走 —— 5 秒的版权渐变不会被虚拟步长快进成 1 秒。
★ **字体**：renderer 统一使用 **Amayui CN（更纱黑体 cnjp 变体）**：`corpus/assets/fonts/Amayui-CN_cnjp*.ttf`
（LFS）由 Vite 作为 asset 打进 `dist/renderer/assets/`，`renderer.ts` 在 Pixi 初始化前用 `FontFace` 预加载；
引擎的 face name（`0x1a5`）只记录，不参与字体选择。字体缺失 ⇒ 退回 `sans-serif` 并写日志。

## 3. 本目录的硬口径

- **分层：核心 / 平台**。`src/core/**` 是平台无关的核心（ALF / LZSS / 解码 / 配置库 / 地址空间 / 值原语 / 执行器 /
  处理器 / 启动记账 / 取件 / 分析入口 / **窗口与渲染请求边界**）；`src/platform/node/**` 是 Node 实现
  （`node:fs` / `node:path` / `process.env`）；`src/platform/electron/**` 是 Electron 壳：
  main 只做窗口 / IPC / 生命周期，renderer 里的 `session.ts` 才是 core 的宿主；`src/main.ts` 只接 CLI argv 与退出码。
  ⛔ 核心不 import 任何 `node:*` / `electron` / `pixi.js`。
- **核心的守卫是类型工程** `tsconfig.core.json`：`"types": []` ⇒ 在 `src/core/**` 写 `Buffer.alloc(1)` 或
  `import fs from 'node:fs'` 当场编译失败（`pnpm typecheck` 的六个工程之一）。
  `tsconfig.json` 覆盖核心 + Node 平台 + Electron main / preload，所以是 `types: ["node"]`；
  `tsconfig.renderer.json` 是渲染进程（`types: []` + `lib: DOM`）；它通过 import 把 core 的可达闭包一起检查
  （core 仍然不能碰 Node / DOM 专属 API），**不能**并进前者（它会放行 Node 全局）。
- **IO 全异步**：`Host`（核心定义的平台接口）的**取件 / 写盘**全是 `Promise`（`join` / `dirname` / `env` / `homeDir` 是纯同步）；`Install.open` / `readById` / `readConfigFile` /
  `loadOpcodeTable` / `loadConfigKeys` / `analyzeBytes` / `analyzeInstall` / `Machine.run` 都返回 `Promise`，
  处理器类型是 `void | Promise<void>`，分派循环 `await` 它。⇒ 同一份核心可以在浏览器 / worker 里跑。
- **规范数据由平台读成纯数据**：`platform/node/spec.ts` 读 `data/spec/format/*.json`，
  核心只收 `Map` / 词表对象（`CoreEnv`）—— ⛔ 核心不认识 `data/spec` 的路径。
- 字节是 `Uint8Array`（不是 `Buffer`）；latin1 的"一字节一字符"口径由 `core/bytes.ts` 显式实现
  （⛔ 不用 `TextDecoder('latin1')`：那是 windows-1252 的别名，会改 0x80–0x9F）。
- 只读 `data/spec`：真正 import `node:fs` 的只有 `platform/node/host.ts`（读安装目录 / 索引 / 归档 / 松散文件；
  玩家配置只写显式路径）、`platform/node/spec.ts`（读规范表）与 `platform/electron/**`（启动窗口 / 生成物）；
  `core/**` 与 `main.ts` **零 fs**（隔离守卫会红）。renderer 的 `Host` 实现只发 `host.*` RPC，不碰 Node。
- TypeScript 只写**可擦除语法**（类型标注 / `interface` / `type` / `import type`），由 Node 原生剥壳直接跑；
  **只有 renderer 有构建步骤**（`vite build`：Pixi 的 ESM、DOM 代码，以及一并打进去的 core）。main / preload /
  CLI 都没有构建步骤；core 的源码仍是平台无关的一份。
- 缺口不是失败：它是"spec 还缺哪一条 opcode 的语义"的机器清单；`--strict` 时才用退出码 1。
- 前沿（"执行到哪一条 / 停在哪"）由 `tools/test/emulator-new-logo.assets.test.mjs` 的**棘轮**钉住：只许往前走。
- 浮点池的"未写槽"规范不说初值：默认**响亮失败**；要跑真实脚本必须显式选 `--uninitialized-float zero`，
  且每次读都会进 `diagnostics`（⛔ 不许静默补 0）。
