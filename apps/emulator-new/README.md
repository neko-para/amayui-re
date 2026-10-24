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
- **CLI 入口**：没有渲染 / 音频输出；外部请求只记进 `m.effects`（`data/spec/core/README.md` §1 裁决）。
  ★ **输入**：`--input` / `--input-script` 提供一个**指令式**的脚本化输入后端（`platform/node/input.ts`），
  把「at 毫秒 + move / down / up / key」归约成核心的输入字 / 光标 / 按钮 / 鼠标事件；
  没有 `--input` 时核心看到 `inputHost === undefined`（没有设备：输入字 0、光标不可用、无事件）。
  `--input-idle-ms`（缺省 500）在脚本耗尽且输入环已启动后以 `input-idle` 收口；`--max-steps` / `--max-ticks` 是安全阀。
  ★ **当前帧**：CLI 报告（`--json` 的 `frame`）与 Electron debug 窗口都会给当前帧的名称 / 统一文件 id /
  帧槽 / 父帧 / ip；名称由安装索引解析，未解析时退回 id。
  ★ **Electron 输入**：renderer 的 `DomInputHost` 接鼠标光标 / 左右键 / 鼠标事件；键盘用**调试映射**
  （`Digit0..9` → 0..9、`KeyA..C` → 10..12，不是引擎真实按键表）。`0x12e` 鼠标命中查找目前只写 -1 +
  一次诊断（让鼠标移动不阻塞），悬停 / 点击菜单的命中语义仍留 gap。
  ★ **Electron 短睡眠**：`sleepHost` 真实 `setTimeout(ms)`；`0xc8` 的 `sleep 1` 不再抛 `sleep-host-missing`。
  ★ **headless 影片**：CLI 缺省注入一个"立即播完"的影片后端，LOGO 之后能继续走启动链；`--no-movie` 保留旧的"停在影片门"行为。
  ★ **跳过 LOGO**：`--skip-logo` 把引擎级 show-logo 标志的**初值**置 0（`0x130` 读到 0；这是 CLI
  的仿真参数，不是新的 spec 语义）。它用于直接调试 TITLE / 后续启动段，避免每次都等 LOGO 影片。
  ★ 引擎自己的 `0x9`（重置并重装根脚本）在复位例程里把该标志清 0；若回标题 / menu 走 `0x9`，
  重跑的根脚本会跳过 LOGO（真实输入路径是否触发 `0x9` 尚未 trace）。`--skip-logo` 只是把**冷启动**也置 0。
  **Electron 前端**（`src/platform/electron/**`）把 `06-window.md` 的窗口 / 图形设备 / 事件入口与
  `60-ops-render.md` 的 `0x20d` / `0x33f` / `0x204` 接到 Electron + Pixi：窗口、画布、渲染目标、离屏文字
  与 **AGF 纹理加载 / 上传**（`core/agf.ts` + `uploadTexture`）是真的；
  音频输出、ADV / Live2D 内部仍是显式未建模；输入已接 `DomInputHost`（鼠标 + 调试键盘映射；`0x12e` 命中查找仅部分）；BMP 分支仍响亮失败。
  ★ **影片**：`0x20f` 已接到 **pl_mpeg WASM**（MPEG-1 / MPEG-PS，只视频；音频忽略），按门前的影片对象表服务推进；
  解码帧上传到同槽纹理并同步 `presentNow()`。
  ★ **全屏语义不支持**（显式决策）：`screenMode=0` 被降级成 `set:WinX` × `set:WinY` 的普通窗口，
  不做显示器分辨率切换、不做全屏缩放；spec 仍按 `06-window` 记录引擎的原始请求。
  ★ **网格渲染是窄实现**：只画“4 顶点、四色一致”的全屏四边形（LOGO 版权页 / TITLE 背景）；
  逐顶点布局 / topology 尚未取证；其它网格跳过并在 diagnostics 记数。
  **绘制顺序是引擎侧行为，属于 spec 的范围**（不是平台自由选择）：无场景命令记录时按 `CORE-SCENE-DRAW-ORDER`
  的**键严格升序**把绘制项与网格放在同一条序列里，**同键时网格先于绘制项**（TITLE 的 key 0 黑色背景因此在
  key 10/100/… 的前景下面；LOGO 的 200002/200003 网格仍在 200000 影片项之后，影片槽最后另画）。
  ★ **负键口径按表分**：网格（以及快照里没有的辅助表 A / B）的负键不参与（收尾扫描用 0 作哨兵），
  **绘制项不做符号检查** —— 负键绘制项仍会被发出，按 key 升序排在最前。⛔ 不要写成"负键一律不参与"。
  ★ **场景命令记录族的写侧已建模**（`0x223` / `0x24d` / `0x24f` / `0x250` / `0x251` 写 `scene.commands`，
  同键替换、置脏、只有种类 1 置「有动画」）；**命令档已进入「只规划、不执行」**：
  `renderer/scene-command-plan.ts` 是纯规划器（命令键升序 / 时间戳盖章 / 三表合并序 / 按种类的阶段列表 /
  区间之下 / `clearTable`），`snapshot()` 把命令记录投影进 `SceneSnapshot.commands`；诊断是
  `scene-command-pass-planned`（命令数 / 阶段数 / 覆盖键数）与 `scene-command-pass-render-unmodeled`
  —— **阶段一个都没发、画面仍来自收尾扫描**，⛔ 不假装执行了命令档，⛔ 也不把「收尾扫描」当成等价物。
  颜色窗的渲染色落点已按 `70-vm` 改成“插值写渲染色 / 顶点缓冲，不写回当前颜色”。
  ★ **core 整份跑在 renderer**：`renderer/session.ts` 直接调用 `analyzeInstall` / `bootWindow` / `Machine`；
  main 只保留 BrowserWindow、文件系统 / 规范表读取、IPC 与进程生命周期。桥接协议见 `platform/electron/protocol.ts`。
- **未实现即停**（不 no-op）：缺口带 `{指令序号, 偏移, opcode, argc}`，停在它**前面**，可以当门禁用。
  ★ **唯一的显式例外**是"未建模子系统的请求边界"（见下节）：白名单里的 opcode 会**记录请求 + 推进**，
  不是静默 no-op；白名单外仍按"停在它前面"处理。
- **未建模子系统（白名单）**：音频输出、ADV 文字显示、Live2D 的内部语义不在本实现，但它们的指令**逐条登记**：
  - 音频（`0xb4` 装载 / `0xb5` 播放 / `0xbf` BGM）：只记 `audio.*` 请求；`0xbf` 仍清状态字第 9 位（spec 的引擎数据效果）。
  - ADV 文字（`0x2c7` SJIS 字符串工具）：记 `unmodeled.subsystem` 请求 + 首次诊断。
    ★ `0x23b`（CG 数字条画数值）**已建模**：TITLE 的 `Version 0.00.0000` 七位数字就是它画的（`core/cg-digit.ts`）。
  - Live2D（`0x341` 模型 / `0x345` 纹理 / `0x34e` 动作 / `0x352` 动作参数）：同上。
  - ⛔ 白名单不是"没有效果"的断言：每条都留 effect、可被 `--trace` / 报告计数；未列入的 `0x12e`（鼠标命中）、
    `0x140`（AGERC 对话框）、`0x23d` / `0x32b`（退出清理）仍然会停下来。
  - ⛔ 不许把新 opcode 直接加进白名单来"让它跑"：先看它有没有 spec 规则、是否改门位 / ip / 返回栈 / 输入字；
    它必须同时满足"只发请求、不改已规定引擎数据"才可进入。
- `handler` 表只包含 `data/spec` 里**已成篇**的 opcode；解不出的操作数类型 / 未取证的值一律**响亮失败**。
  ★ 处理器抛异常时，`Machine.run` 会给消息追加定位后缀
  `[opcode=… argc=… step=… offset=… ip=… script=… operands=[type:raw…]]` ⇒ CLI / Electron 日志能直接指到具体指令。
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

# 脚本化输入：真实启动链 → TITLE，1000ms 按一下键 1、1100ms 松开；输入耗尽 500ms 后收口
# 语法：<at_ms> move <x> <y> | down|up <button> | key <0..31> down|up | wheel <delta>
#   button 是按钮位号（0 = 左、1 = 右）；key 是引擎输入位号（0..31，不是 Windows VK 码）
node apps/emulator-new/src/main.ts --install "<安装目录>" \
  --input-script "1000 key 1 down; 1100 key 1 up" --input-idle-ms 500

# 同上，脚本从文件读（一行一条；# 注释）
node apps/emulator-new/src/main.ts --install "<安装目录>" --input input.txt

# 跳过 LOGO（show-logo 初值置 0），直接调试 TITLE / 后续启动段
node apps/emulator-new/src/main.ts --install "<安装目录>" --skip-logo \
  --input-script "1000 key 1 down; 1100 key 1 up"

# 保留旧行为：不给 headless 影片后端（LOGO 会停在影片门）
node apps/emulator-new/src/main.ts --install "<安装目录>" --no-movie

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
`--trace` 默认只打 **external + unmodeled**（音频 / 纹理加载请求 / 输入 / 未建模调用）；`string.set`、`array.lookup`、`engine.scalar.*` 这类**已建模的内部记账**只在 `--trace-all` 里出现。

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

# 跳过 LOGO（show-logo 初值置 0）；Electron 与 CLI 同名参数
pnpm --filter @amayui/emulator-new exec electron . --install "<安装目录>" --skip-logo
```

★ 受限沙箱里 Electron 需要 `--no-sandbox`（放在 `electron .` 与 app 路径之间）；正常桌面不要加。
★ main / preload 由 Electron 的 Node 直接跑 `.ts`（Node 24 类型剥壳）；renderer 走 `vite build`，
  并且 **core 的 `.ts` 也由这次构建一起打进 renderer bundle**（不是 main 的运行时）。
  `dist/renderer/**` 是生成物（已 gitignore）。
★ **两个窗口**：
  - **game 窗口**：只承载目标 canvas；core / VM / 场景 / Pixi 都在这个 renderer 里；
  - **debug 窗口**：先建、默认可见，加载同一个 renderer bundle 的 `?debug=1` 分支，只读显示
    `HostDiagnostics` / boot / status / log 投影；最上面一段是**当前脚本 / 帧**（名称 / 统一文件 id /
    帧槽 / 父帧 / 指令偏移），名称由安装索引解析。
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
- 前沿（"执行到哪一条 / 停在哪"）由两个 assets 守卫钉住：`emulator-new-logo.assets.test.mjs`（LOGO 棘轮）
  与 `emulator-new-title.assets.test.mjs`（真实启动链到 TITLE、键 1 派发、`input-idle` 收口）。
- **输入分层**：`core/input.ts` 只是 `InputHost` 接口（平台无关）；CLI 的 `platform/node/input.ts`（指令式脚本）
  与 Electron 的 `platform/electron/renderer/input-host.ts`（DOM 事件）是两个平台实现。⇒ 核心不知道 `--input` / 文件 / 命令行 / DOM。
- 浮点池的"未写槽"规范不说初值：默认**响亮失败**；要跑真实脚本必须显式选 `--uninitialized-float zero`，
  且每次读都会进 `diagnostics`（⛔ 不许静默补 0）。
