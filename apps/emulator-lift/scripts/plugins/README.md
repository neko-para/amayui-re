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
├── field-point-visible.js
└── lib/                         # **只被 import**：永远不是 --plugin 的 id 目标
    ├── save.js                  #   loadSave / SAVE_DIR_REL / 前置检查（没有 default 导出）
    ├── settings.js              #   插件侧**唯一**的取配置入口（config > params > 默认 + settings.schema）
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
  而不是被静默装上（⛔ 不静默被内置件遮住，也不静默换源）。保留 id 判据 **ASCII 大小写不敏感**
  （`--plugin HUD` 同样触发遮蔽 / 同样解析内置件）。
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
{"version":1,"domains":{"field.point-visible":{"on":1}}}
```

优先级：**config > `--plugin-param` > 默认**；"给了"的判据是 `domain.has(key)`（`slot:0` / `on:0`
这种 falsy 值也算给了）。没有 `--appdata` ⇒ `ctx.config.available=false`，插件完全回退到 params。

★ **`null` = 没给**（与 2026-11 前逐字一致）：`{"on":null}` 等价于没写这个 key ⇒ 回退
`--plugin-param` / 默认；⛔ 不 throw、⛔ 不当"值非法"（`null` 不是一种合法的设置值）。

★ **取配置只有一条路**（2026-11-25）：`./lib/settings.js` 的 `openConfigDomain` / `slotSetting` /
`scriptListSetting` / `textSetting` / `intSetting` / `boolSetting`。⛔ 入口里再抄一份
`configDomainOrNull` / `hasConfig` / 自己解析值 —— 四份语义分叉过一次（`precheck` 收 `"true"`、
`on` 只收数字），改一处另一处静默过期。布尔是**并集语义**：`boolean` / `0|1` /
`"0" "1" "true" "false"`；其它值（含整数 `2`）一律 throw。

| 域 id | key | owner（文件） |
|---|---|---|
| `save` | `slot` (number)、`expect` (string[] 或逗号串)、可选 `saveDir` (string，相对 appdata 根)、`timeoutMs` (number)、`precheck` (boolean，缺省 true；false = 跳过存档文件前置检查) | 根入口 `load-save.js`（库 = `lib/save.js`） |
| `scenario.save10-field` | `slot` (number)、`expect` (string[] 或逗号串) | `scenario-save10-field.js`（根） |
| `field.point-visible` | `on` (bool / `0|1` / `"true"|"false"`，缺省 false)、`types` (hex 串 `"1f,20"` 或数组)、`diag` (bool，缺省 false)；旧配置里的 `poll` **无害忽略** | `field-point-visible.js`（根） |
| `settings.schema` | **保留域（机器写，⛔ 人不用手改）**：key = 插件域 id；值 = `{title, live, entries}` | 写 = `lib/settings.js` 的 `registerSettings`；读 = `scripts/ui/settings-page.js` |

- ★ `field-point-visible.js` 是**常驻开关**（用户裁决 2026-11-25：目标 = **在 setmap 时无条件显示所有
  采集 / 挖掘点**，不是"在 FIELD 里无条件显示 `?`/`!`"）：它常驻 arm **两个**
  `afterPosition("FIELD.BIN", …, { mode: "reenter" })`（判据 = **setmap 完成的已知脚本位置**：
  `FIELD_SETMAP_A_LO/HI` = `call-script 511a`(SETOBJ) 在进图主路径的返回点 ·
  `FIELD_SETMAP_B_LO/HI` = `0x035130` 块的返回点），任一处**外→内** fire 时把 32 槽派生渲染位里
  类型命中的槽 **置 `DRAW|ALPHA`（`|= 0x5`）** —— `DRAWOBJ` 的 bit0=1 才画、bit2=1 才不是 alpha 0；
  **⛔ 不清任何位**（尤其 bit1：A 链格表与 FIELD 的战利品抽取都消费它）。`50ff9f` 是 VM 每次进图
  重算的派生表 ⇒ 每次 setmap 重新施加。⛔ 不轮询（没有 `afterDelay` / `poll` / `FIELD_MAIN_LOOP`）、
  ⛔ 不读档（不 import `lib/save.js` / `lib/boot-to-title.js` / `lib/chains.js`）、⛔ 不点鼠标、
  ⛔ 不写盘、⛔ 不 `finish`（正常路径常驻；进程按 `--time-limit` / 关窗结束）。
  ⇒ 验收用法 = **玩家自己读档**：`--overlay --plugin field-point-visible`（`--plugin` 与
  `--input-plan` / `--control-root` 互斥 ⇒ 没有"输入计划驱动"这条验收）；FIELD 的**自动化复现**仍归
  `scenario-save10-field.js`。守卫：CTest `plugin-field-switch`（静态契约 + headless 常驻）与
  `plugin-runtime` 的 `plugin/field-switch-apply`（`0x3→0x7` / `0x5→0x5` / 不重复写 / 换图重施加 /
  `on=0` 不写）。★ `?` / `!` 是 **A 链格标记**（`SO030` 槽 `0x49`），不是点位；`on=1` 之后它们照旧出现。
  ★ 设置面（D1/D3）：`start` 里 `registerSettings` 登记 `on`/`diag`（页面据此画），这两项
  **每次位置触发（fire）重读**；`types` 不在 schema 里 ⇒ 只在 `start` 读一次。
- ★ 旧配置里残留的 `slot` / `hold` / `poll` 对本插件**无害忽略**（不读、不报错；它不再读档、
  不再一次性结束、不再轮询）。

### `settings.schema`：插件自报的设置表（D1，2026-11-25）

- **谁写**：插件在 `start(ctx)`（Run 上下文，已有 `ctx.config`）调
  `registerSettings(ctx, { domain, title, live: "frame"|"run"|"restart", entries })`。它把
  `{title, live, entries}` 写进**保留域** `settings.schema` 的 key = `domain`。形状：
  `{"version":1,"domains":{"settings.schema":{"field.point-visible":{"title":"FIELD points","live":"run","entries":{...}}}}}`
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
  消费者读点用 `lib/settings.js` 读它 —— 只登记不读 = 假开关，只读不登记 = 页面上改不到。

- 域自动创建、不注册、可共享；域 id = `^[A-Za-z0-9._-]{1,64}$`；域 id 与 key 都拒
  `__proto__` / `prototype` / `constructor`。未知顶层键 / 未知域内键**保留**（前向兼容）。
- 新增内建插件要在这里登记它的域（域 id / key / owner）——**消费者只有配置里的域名字符串**，
  没有这张表就只能靠读源码。

## 示例

`examples/` 下的 `.ts` 是**参考插件**（旧 plan 翻译、global 读改写、HUD show/hide、dispatch、具名多 trigger、
rearm、timeout、并发）。它们**在** `pnpm typecheck` 的最后一档（`tsc -p …/plugins/examples/tsconfig.json`，
flags 比 base 更严：`noUncheckedIndexedAccess` / `exactOptionalPropertyTypes`），但**不参与构建**；
改动 API 时单独跑同一条命令：

```bash
node_modules/.bin/tsc -p apps/emulator-lift/scripts/plugins/examples/tsconfig.json
```

- 示例里的数值都是占位；不要直接当生产脚本。
- 示例只依赖 `plugin.d.ts` 的类型，不需要运行时 shim。
- ★ `script` 名是 **marker 文件名（含 `.BIN`）**、宿主**精确相等** ⇒ 示例里 `"TITLE"` / `"SBUNKI"`
  这类裸名字只是**占位**，照抄进真插件会永不命中（`plugin.d.ts` 的用法段已改成 `.BIN` 写法）。
