# `plugin.d.ts` / `examples`

本目录是**插件运行时**的 API 合同与参考示例；口径与切片见
[`docs/03-emulator/plugin-runtime.md`](../../../../docs/03-emulator/plugin-runtime.md)。

## 目录布局：**入口 vs 库**（S1，2026-11-25）

```
scripts/plugins/
├── plugin.d.ts / README.md      # 合同 + 本说明
├── examples/                    # 类型示例（不参与构建）
├── load-save.ts                 # 入口：save 的参考插件（id = load-save）
├── quit-at-title.ts             # 入口：TITLE → Quit 的 e2e 驱动器（CTest aot-quit-exit）
├── scenario-save07-battle.ts    # 入口（id = 文件名 stem）
├── scenario-save10-field.ts
├── scenario-fellow-switch.ts
├── field-point-visible.ts
└── lib/                         # **只被 import**：永远不是 --plugin 的 id 目标
    ├── save.ts                  #   loadSave / validateLoadSave / SAVE_DIR_REL / 前置检查（没有 default 导出）
    ├── settings.ts              #   插件侧**唯一**的取配置入口（config > params > 默认 + settings.schema）
    ├── boot-to-title.ts · gates.ts · script-positions.ts · guest-globals.ts
```

- **`--plugin <id>` 只查本目录的根**（`<exeDir>/scripts/plugins/<id>.js|.ts|.mts` 与编译期
  `LIFT_PLUGIN_DIR/<id>.js|.ts|.mts` 两处，**非递归**；S1b 起同一 id 也试 `.ts` / `.mts`，见下节）；⛔ 不查
  `lib/`、⛔ 不递归子目录。需要单独加载的东西必须在根上；库只被 `import`。
- id 语法 `^[A-Za-z0-9_-]{1,64}$`；含 `.` / `/` / `\` 的写法**一律当路径**（所以 `x.js` / `x.ts` 是
  cwd 相对路径，与旧版逐字相同）。两根都命中 ⇒ `exit 2` 并列全部候选；都不在 ⇒ `exit 2` 并列搜过的根与候选。
- ★ **`hud` 是保留 id**（S2）：`--overlay` = 「`--plugin hud` + 要窗口」的预设，解析的是
  `scripts/ui/hud.js`（`<exeDir>/scripts/ui/` → `LIFT_UI_SCRIPT_DIR`），**不是**本目录。
  ⇒ ⛔ 不要在本目录放 `hud.js`：它会**遮蔽**保留 id ⇒ 启动 `exit 2`（列路径 + "改名或给路径"），
  而不是被静默装上（⛔ 不静默被内置件遮住，也不静默换源）。保留 id 判据 **ASCII 大小写不敏感**
  （`--plugin HUD` 同样触发遮蔽 / 同样解析内置件）。
- import 根 = **入口文件所在目录**：入口用 `./lib/x.ts`（向下，允许）；`lib/` 内部用同目录 `./x.ts`。
  ⛔ 入口不能住在库的下层（`../` 越根会被 QJS 拒）。`.ts` / `.mts` 同形，且 specifier **必须带扩展名**
  （loader 不补扩展名；见下节）。★ 2026-11-28（T1）起**发行插件本身就是 `.ts`**（12 个），
  类型档 = `pnpm typecheck` 的 `plugins/tsconfig.json`（`strict` + `erasableSyntaxOnly`）。

## `.ts` 插件（E1）

宿主**直接装载** `.ts` / `.mts`，运行时不依赖 Node / tsc：`--plugin <路径>.ts`，或
`--plugin <id>`（同一 id 依次试 `<id>.js` → `<id>.ts` → `<id>.mts`）。引擎侧 = vendored
quickjs-ng 的 quickts E1 补丁（纯类型擦除），补丁口径见
[`third_party/quickjs-ng/README.md`](../../third_party/quickjs-ng/README.md)。

- **怎么判 TS**：只看来源的**扩展名**（ASCII 大小写不敏感）——`.ts` / `.mts` ⇒
  `JS_EVAL_FLAG_TYPESCRIPT`；`.js` **不加**（`.js` 行为逐字节不变）。四处 `JS_Eval` 共用同一个判定：
  插件入口 / module loader（被 import 的 `lib/*.ts`）/ classic 探针 / 一般 `load`。
  `.ts` 入口**强制 module**（跳过 classic / `JS_DetectModule`：ng 的公开探测 API 没有 flags）。
- **id 候选与 `.js` 优先**：`<id>.js` → `<id>.ts` → `<id>.mts`，**扩展名优先于根**；同一 id 同时有
  `.js` / `.ts` ⇒ **`.js` 优先**（`.js` 是历史行为；2026-11-28 起**发行件是 `.ts`** —— 给路径或让
  同一 id 的 `.js` 不存在即可，⛔ 不静默换源、⛔ 不报歧义）。
  保留 id `hud` 的遮蔽检查**只看 `hud.js`**（`hud.ts` 不参与；`.ts` HUD 不在本轮范围）。
- **import 扩展名策略**：loader 按 specifier 原文解析，**不补扩展名** ⇒ 多文件 `.ts` 必须写
  `import … from "./lib/x.ts"`（`import type` 整条擦除、不产生 module request）。写 `./lib/x` 会在
  `moduleNormalize` 处"读不到模块"失败（与 `.js` 的现有口径相同）。
- **支持的语法**（E1 擦除面）：`interface` / `type` 别名、注解（参数 / 变量 / 返回值 / 成员）、
  `as` / `as const` / `satisfies`、泛型（声明 / 调用 / 类型引用 / `>>` 嵌套）、联合 / 数组 / 元组 /
  字面量类型、`import type` / `export type`、class 修饰符与无体成员 / index signature、`declare`、
  可选属性 `?`、非空断言 `!`、`this` 参数、重载签名、`abstract class`。
  完整契约（`RenderCtx` / `RunCtx`）仍只看 `plugin.d.ts`。
- **不支持**（运行期必须报错，⛔ 不静默接受）：`enum` / `const enum`、构造参数属性
  `constructor(public x: T)`、含值的 `namespace`、JSX / `.tsx`、装饰器、`import =` / `export =`、
  legacy `<T>x`、`accessor` 字段、`a?.<T>()`（optional-chain 调用上的类型实参）。
  `erasableSyntaxOnly` 只查"可擦"，放行的写法仍可能运行期报错（如 `a?.<T>()`）。
- **门禁命令**：

  ```bash
  # 类型面（tsc 只证明"可擦"；运行期覆盖见下一条）
  pnpm typecheck     # 含 examples/tsconfig.json（已开 erasableSyntaxOnly）
  # 运行期面（宿主 loader + 引擎）
  ctest --test-dir apps/emulator-lift/out -R 'quickts|plugin-runtime|plugin-resolve' --output-on-failure
  ```

- **判据落点**：`tests/plugin_fixtures/ts-smoke/`（入口 + `lib/helper.ts`，`import type` 与运行期
  import 各一次）由 `plugin-runtime` 的 `plugin/ts-module` 判；`.js` 的 `f<a>(b)` 比较语义由
  `plugin/js-comparison-unchanged` 判；id 候选与 `.js` 优先由 `plugin-resolve` 判。

## 合同

- 模块 specifier：`@amayui/lift-plugin`（`plugin.d.ts` 是 ambient module）。
- 插件文件 = ES module：`export default { render?, start? } satisfies PluginModule`。
  `start(ctx)` 返回 `void`（只做 `ctx.at` / `ctx.once` / `ctx.task` 注册）；返回非 `undefined` ⇒ 装载失败。
  `onTrigger` / `initialState` 已整条删除（出现即装载失败，退出码 2）。
- 宿主 QJS 装载后只读 **default 导出**；不再读 `globalThis.render/start/onTrigger`，也没有运行时 `definePlugin`。
- `sample` / `screenshot` 不在插件 API 里（它们是 `emu-ctl` 的控制面组合）；插件读 `scriptPos` 用 `ctx.read.scriptPos`。
- guest 写**只走 `Tick`**：`tick.edit`（立即持久写）/ `tick.input`（入队、下一读点落地）/ `tick.patch`
  （本拍瞬时覆盖、下一拍恢复）。`RunCtx` 没有 `edit` / `input`。合同见 `docs/03-emulator/plugin-runtime.md` §4.5 / §4.15。

## 内建插件的配置域

内建插件**不再靠 `--plugin-param` 传具体参数**（那是夹具 / 诊断口）。配置写
`<--appdata>/plugin-config.json`（appdata 根，⛔ 不在 `SAVE/`；`scripts/clean-appdata.sh` 的
`rm -rf` 会把它一起删掉）：

```json
{"version":1,"domains":{"field.point-visible":{"on":1}}}
```

优先级：**config > `--plugin-param` > 默认**；"给了"的判据是 `domain.has(key)`（`slot:0` / `on:0`
这种 falsy 值也算给了）。没有 `--appdata` ⇒ `ctx.config.available=false`，插件完全回退到 params。

★ **`null` = 没给**（与 2026-11 前逐字一致）：`{"on":null}` 等价于没写这个 key ⇒ 回退
`--plugin-param` / 默认；⛔ 不 throw、⛔ 不当"值非法"（`null` 不是一种合法的设置值）。

★ **取配置只有一条路**（2026-11-25）：`./lib/settings.ts` 的 `openConfigDomain` / `slotSetting` /
`scriptListSetting` / `textSetting` / `intSetting` / `boolSetting`。⛔ 入口里再抄一份
`configDomainOrNull` / `hasConfig` / 自己解析值 —— 四份语义分叉过一次（`precheck` 收 `"true"`、
`on` 只收数字），改一处另一处静默过期。布尔是**并集语义**：`boolean` / `0|1` /
`"0" "1" "true" "false"`；其它值（含整数 `2`）一律 throw。

| 域 id | key | owner（文件） |
|---|---|---|
| `save` | `slot` (number)、`expect` (string[] 或逗号串)、可选 `saveDir` (string，相对 appdata 根)、`timeoutMs` (number)、`precheck` (boolean，缺省 true；false = 跳过存档文件前置检查) | 根入口 `load-save.ts`（库 = `lib/save.ts`） |
| `scenario.save10-field` | `slot` (number)、`expect` (string[] 或逗号串) | `scenario-save10-field.ts`（根） |
| `field.point-visible` | `on` (bool / `0|1` / `"true"|"false"`，缺省 false)、`types` (hex 串 `"1f,20"` 或数组)、`diag` (bool，缺省 false)；旧配置里的 `static` / `poll` **无害忽略** | `field-point-visible.ts`（根） |
| `settings.schema` | **保留域（机器写，⛔ 人不用手改）**：key = 插件域 id；值 = `{owner?, title, live, entries}`（`owner` = 插件名，可缺省） | 写 = `lib/settings.ts` 的 `registerSettings`；读 = `scripts/ui/settings-page.js` |

- ★ `field-point-visible.ts` 是**常驻开关**（用户裁决 2026-11-25：目标 = **无条件显示所有采集 / 挖掘点**，
  不是"在 FIELD 里无条件显示 `?`/`!`"）：它常驻 arm **一条** `afterPosition(…, { mode: "reenter" })`
  （判据 = **已知脚本位置**）：
  `DRAWOBJ_ENTRY_LO/HI` = **`DRAWOBJ.BIN` 的入口窗口** `[0x003C,0x06BC)`
  （#0 `detach-texture 3aca 32` → 点位槽循环尾；第二条循环是 `6202a` 元素对象，不含点位精灵）⇒
  **每次进 `DRAWOBJ` 一次**（`name = "fp:draw"`）。★ 为什么是 `DRAWOBJ` 入口而不是某个调用点：
  全语料 `call-script 5117` 只有 4 处（`RENDERMAP` #118 `0x0B04` · `FIELD` #2722 `0x0F034` /
  #9688 `0x0392BC` · `RESETMAPAN` #1 `0x0050`），**全部**从 `DRAWOBJ` 进 ⇒ 一条钩子覆盖所有调用者
  （含**逐帧**那条：`FIELD` #838 `call label_000392b0`）。调用点全表见
  `docs/03-emulator/scripts/FIELD.md` §⑧。
  `fp:draw` 里对类型命中（缺省 採集 1f / 発掘 20）的槽把派生渲染位 `50ff9f[slot]` **补上 bit0**
  （`flags | 0x1`；`DRAWOBJ` #7 的 bit0=1 才画）。**⛔ bit1 / bit2 一个都不写**：
  `SETOBJ` 每渲染帧 `fill-zero (global-int 50ff9f) 32` 后按"发现 / 触发状态"自己重算
  ⇒ **bit2 = 静态 ↔ 动效的选择位**、bit1 = 静态门的一半（`(flags&6)==2`）。
  ⇒ **已触发 = 动效、未触发 = 静态**由引擎决定；插件的旧写法 `|= 0x5` 把 bit2 强加上去，
  才出现"没采集过的点也在动"（真机读数 2026-11-26 / 存档 012：slot3 `0x5`、slot4/5/6 `0x3`）。
  ★ **为什么必须落在 `DRAWOBJ` 里**：同一渲染帧的 `RENDERMAP` 先 `SETOBJ` #2 `fill-zero` 再逐槽重算
  ⇒ 插件写在这张派生表上的位必被抹掉（所以**两条 `FIELD.BIN` setmap 钩子已删**：它们写在 SETOBJ
  之后但同帧就会被重算，留着只是让日志看起来"做了事"）。`fp:draw` 每进一次 `DRAWOBJ` 就 fire ⇒ ⛔ **不刷屏**：
  首帧 / `matched/patched` 变化才一行（行里带 `map=`），逐槽行只为**真的改了**的槽打，`diag=1` 时每 60 次一行。
  ⛔ 不轮询（没有 `afterDelay` / `poll` / `FIELD_MAIN_LOOP`）、⛔ 不读档（不 import `lib/save.ts` /
  `lib/boot-to-title.ts` / `lib/chains.ts`）、
  ⛔ 不点鼠标、⛔ 不写盘、⛔ 不 `finish`（正常路径常驻；进程按 `--time-limit` / 关窗结束）。
  ⇒ 验收用法 = **玩家自己读档**：`--overlay --plugin field-point-visible`（`--plugin` 与
  `--input-plan` / `--control-root` 互斥 ⇒ 没有"输入计划驱动"这条验收）；FIELD 的**自动化复现**仍归
  `scenario-save10-field.ts`。守卫：CTest `plugin-field-switch`（静态契约 + headless 常驻）与
  `plugin-runtime` 的 `plugin/field-switch-apply`（`DRAWOBJ` 入口：`0x2→0x3` / `0x6→0x7` 补 bit0，
  `0x3`/`0x5`/`0x7` 不写，同窗口不重复 fire / 下一帧重新补 / 同状态不刷日志 / `on=0` 不写）。
  ★ `?` / `!` 是 **A 链格标记**（`SO030` 槽 `0x49`），不是点位；`on=1` 之后它们照旧出现。
  ★ 设置面（D1/D3）：`start` 里 `registerSettings` 登记 `on`/`diag`（页面据此画），这两项
  **每次位置触发（fire）重读**；`types` 不在 schema 里 ⇒ 只在 `start` 读一次。
- ★ 旧配置里残留的 `slot` / `hold` / `poll` 对本插件**无害忽略**（不读、不报错；它不再读档、
  不再一次性结束、不再轮询）。

### `settings.schema`：插件自报的设置表（D1，2026-11-25）

- **谁写**：插件在 `start(ctx)`（Run 上下文，已有 `ctx.config`）调
  `registerSettings(ctx, { domain, owner?, title, live: "frame"|"run"|"restart", entries })`。它把
  `{owner?, title, live, entries}` 写进**保留域** `settings.schema` 的 key = `domain`（`owner` =
  插件名，可选；缺省 = 设置页只画旧 `<title>`）。形状：
  `{"version":1,"domains":{"settings.schema":{"field.point-visible":{"owner":"field-point-visible","title":"FIELD points","live":"run","entries":{...}}}}}`
  每项 = `{type:"bool"|"number", label, default?, min?, max?, step?}`。
- **谁读**：`scripts/ui/settings-page.js`（经典回退 HUD 的 `host.config`；`h.config === undefined`
  ⇒ 页面不打开）。它每帧**只读 schema 域**，每个可见项用 `get(key, default)`；⛔ 不缓存值。
- ★ **先 `all()` 比、相同不写**：schema 没变 ⇒ 一次 `set` 都不发生（不产生脏写）。`set` 失败
  （超上限 / 只读 / 没有 appdata）**只落一行 `ctx.log("warn", …)`** —— ⛔ 不许把 `start` 打死。
- ★ `settings.schema` 是**机器写的**：用户可以不管它，⛔ 不要手写进配置当"开关"（真值仍在各插件
  自己的域里）。**陈旧 schema**（上一个进程写的）无法从 JS 侧区分 ⇒ 页面标题下固定写
  `live: 正在运行的插件当拍生效；其余为预配置`；`live` 只是插件自报，⛔ 不当判据。
- ★ **live = 读点**：`live:"frame"|"run"` 只在消费者**自己的读点重读**时才成立（`field.point-visible`
  的 `on`/`diag` 每次 fire 重读；`types` 不在 schema 里 ⇒ 只读一次）。`live:"restart"` = 预配置。
- ★ 与页面**同进程**才有热改：`--overlay --plugin field-point-visible`（经典回退 HUD 与 Run 插件
  同进程、同一份 store）。跨进程（hud 写、另一个 run 进程读）改完要重启。
- 新增一条可配置项：**先**在 owner 的 `registerSettings.entries` 里登记（页面据此画控件）**再**在
  消费者读点用 `lib/settings.ts` 读它 —— 只登记不读 = 假开关，只读不登记 = 页面上改不到。

- 域自动创建、不注册、可共享；域 id = `^[A-Za-z0-9._-]{1,64}$`；域 id 与 key 都拒
  `__proto__` / `prototype` / `constructor`。未知顶层键 / 未知域内键**保留**（前向兼容）。
- 新增内建插件要在这里登记它的域（域 id / key / owner）——**消费者只有配置里的域名字符串**，
  没有这张表就只能靠读源码。

## 示例

`examples/` 下的 `.ts` 是 **v2 API 的参考插件**（一个文件一个教学点）。它们**不参与构建**，
但在 `pnpm typecheck` 的最后一档（`tsc -p …/plugins/examples/tsconfig.json`，flags 比 base 更严：
`noUncheckedIndexedAccess` / `exactOptionalPropertyTypes` / `erasableSyntaxOnly`），并且由 CTest
`quickts` 的 `ts-examples-compile` 逐个编译（要求 `import type` 整条擦掉、零 module request）。
改动 API 时单独跑这几条：

```bash
pnpm typecheck
node_modules/.bin/tsc -p apps/emulator-lift/scripts/plugins/examples/tsconfig.json
ctest --test-dir apps/emulator-lift/out -R 'quickts' --output-on-failure
```

| 文件 | 演示什么（v2） |
|---|---|
| `concurrent.ts` | 一个 Run 里多个 `ctx.once` + `Promise.race` / `Promise.all`（不新开 Run） |
| `dispatch.ts` | **没有 dispatch**：dispatch 属控制层；子流程用 `ctx.task` 组合、停用 `ctx.off` |
| `global-edit.ts` | `ctx.at` + `tick.edit.global.set`（立即持久写；读旧值走 `ctx.read`） |
| `hud-at-pos.ts` | tick 只改模块数据；`render(rctx)` 读数据 + `rctx.ui` / `rctx.draw`（tick 不碰 UI） |
| `keep-others.ts` | 多条 `ctx.at` 订阅 + 选择性 `ctx.off`（没有 armed 集合替换） |
| `load-save.ts` | `ctx.task` + 顺序 `await ctx.once(...)` + `tick.input.click` + task settle 收口 |
| `maxfires.ts` | `ctx.at` + 自己计数 + 到量 `ctx.off`（没有 maxFires） |
| `rearm.ts` | `ctx.off` + 新 `ctx.at`（新订阅）；v2 没有 Once / Reenter mode |
| `timeout.ts` | `ctx.once(where, { timeoutMs })` + `try/catch`；timeout = 失败截止（catch 里只能 log） |

- ★ **`start` 返回 Trigger 集合 / `onTrigger` 那一层已删除**：`start(ctx)` 返回 `void`，只做
  `ctx.at` / `ctx.once` / `ctx.task` 的注册；组合用 task、切换 Run 由控制层做。返回非 `undefined`
  或出现 `onTrigger` / `initialState` ⇒ 装载失败（⛔ 不静默忽略）。
- 示例只依赖 `plugin.d.ts` 的类型，不需要运行时 shim。
- ★ `script` 名是 **marker 文件名（含 `.BIN`）**、宿主**精确相等** ⇒ 示例里的 `"TITLE.BIN"` 等仍是
  **占位**，照抄进真插件要换成真实脚本名；`pos` 真实值从 1 开始、区间必须 `lo < hi`。
- 示例里的 0 坐标 / 0 区间 / 空颜色串同样是占位：运行期会被形状校验拒（`pos_zero` / `lo >= hi`），
  这是有意的 —— 示例只证明「只看 `plugin.d.ts` 能写出 v2 插件」。
