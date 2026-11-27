# `scripts/ui` —— HUD 脚本（`--overlay` / `--plugin hud`；`--ui` 已删除）

`--overlay` 的 HUD **不是 C++ 画出来的**：它由这份目录里的 JS 脚本经 QuickJS **同步**跑出
一份 display list，宿主再交给宿主字体栅格化、合成进窗口像素。默认脚本就是 `hud.js`
（贴左侧、**垂直居中**的面板）；按钮之类的外观 / 状态 / 主题在组件库 `kit.js`，
二级设置页在同目录的 `settings-page.js`。

★ **S2（2026-11-25）起 `hud.js` 是真插件**：入口是 `export default { render(ctx) }`，用
`ctx.draw.*` / `ctx.game.*` / `ctx.profile.*`（契约 = `scripts/plugins/plugin.d.ts` 的 `RenderCtx`，
说明 = `docs/03-emulator/plugin-runtime.md` §4.6）。`--overlay` = 「装载内置 `hud`（保留 id）+
要窗口」的预设；`--plugin hud` 是同一个入口（⛔ 不隐含开窗）。
★ **S3 起 `--ui` 已从 CLI 面删除**：装载点只有 `--plugin <id|path>`（`--overlay` 是它的窗口预设）。
`globalThis.render` 这个**经典入口仍在**，但只服务**内部 hud 回退**（`--overlay` + 无 render 的
`--plugin`，见下）；用户自己的脚本一律写成**插件模块**（`export default { render(ctx) }`）。
两个入口共用**同一份绘制体**、只换薄适配器（`classicHost()` / `pluginHost(ctx)`）。
★ `hud.js` 是 **module**：它 `import { Button } from './kit.js'` 与
`import { canOpen, drawPage, hasSettings } from './settings-page.js'` ⇒ 发行时 `hud.js` / `kit.js` /
`settings-page.js` **必须同目录**（`import` 的相对解析以 `hud.js` 所在目录为根；缺任一与缺 hud
同属**加载期硬失败**）。
★ **二级设置页 = 同目录的 `settings-page.js`**（D2/D4，2026-11-25）：`export { canOpen, hasSettings, drawPage, PAGE_KEYS }`，
只依赖 `host`（含 `host.config`）与调用方状态。它**不是** `--plugin` 的 id 目标（id 只查
`scripts/plugins/` 根）；只能由 `hud.js` `import` 或用户脚本 `<path>` 装载。

★ 脚本是**外置文件、运行时读**的：改完直接重跑，**不需要重编**（也⛔ 不要把 JS 抄进 C++）。

## 怎么用

```bash
# 默认 HUD（`--overlay` = 内置 hud 插件 + 要窗口）
emulator-lift-aot --present window --overlay ...

# 等价写法（显式插件；⛔ 不隐含 overlay ⇒ 想开窗要自己给 --present window）
emulator-lift-aot --present window --plugin hud ...

# 自己的脚本（插件模块：export default { render(ctx) }）
emulator-lift-aot --present window --plugin E:\my-hud.js ...

# 内部 hud 回退：`--overlay` + 一个**没有 render** 的插件 ⇒ 默认 hud 照画
emulator-lift-aot --present window --overlay --plugin E:\my-run-only.js ...
```

`--plugin <path>` 的脚本可以 `import` **它自己所在目录的子树**里的相对模块（例如把 `kit.js` 拷到
旁边，然后 `import { Button } from './kit.js'`）。⛔ bare specifier（`react`）/ 绝对路径 /
`data:` URL / `..` 逃出脚本目录一律**加载期拒绝**（启动期 exit 2，错误串带 `<path>:<行>:<列>`）。

`--overlay`（= `--plugin hud`）的保留 id 解析顺序：

1. `<exe 所在目录>/scripts/ui/hud.js` —— **发行布局**（`aot_main` 从 `argv[0]` 取目录）；
2. `<LIFT_UI_SCRIPT_DIR>/hud.js` —— **开发回退**（构建时注入的源码树路径，指向本目录）；
3. 两份都在场 ⇒ **exe 那份赢**；都不在 ⇒ **拒绝启动**，错误里点名试过的两个路径（⛔ 不静默回退成"空 HUD"）。

★ **用户插件根里的 `hud.js`**（`<exe>/scripts/plugins/hud.js` 或 `LIFT_PLUGIN_DIR/hud.js`）⇒ 启动
**exit 2** 并列出路径（`hud` 是保留 id；改名或给路径）——⛔ 不静默被内置件遮住。

**加载期**的失败（语法错 / 缺入口 / 字体不可用）是**硬失败**：`--overlay` / `--plugin hud` /
`--plugin <path>` 在启动期 exit 2，错误串带 `<path>:<行>:<列>`（字体不可用 = R5）。**运行期**抛错 /
超预算不是启动失败：一次性 `[overlay] …` 进 stderr + **关掉叠层**，**呈现照旧**（exit 0）。

## 版式 / 入口 / 按键（D4，2026-11-25）

`hud.js` 的 `drawHud` 是**唯一**绘制体（两个入口共用）；D4 之后的形状：

- **面板**：贴左、**垂直居中** —— `x = 0`、`y = round((720 − panelH) / 2)`（每帧按行数重算；
  `PAD = 8` / `SIZE = 16`，面板宽 = 最长行 + 2×`PAD`）。面板仍是 `elements[0]`。
- **行序**：`mouse` / `script` / `fps` / `worst` / 状态行 / `settings…`。没值的行**整行省略**（⛔ 不占位）；
  `settings…` 恒在**最后一行**。`frame <n>` 行已删（总帧数无诊断价值）。
- **设置入口**：最后一行 `settings…` 上叠一条 `fill:"#00000000"`、`key:"settings"` 的**透明命中
  Rect**（刚好盖住那一行）。点击 ⇒ 打开 `settings-page.js` 的二级页（页面开着时**不画 HUD 面板**，
  `settings…` 行与 PROFILE 都不画；`BACK` 关闭）。`h.config` **缺失**（`undefined`）⇒ 不打开，状态行
  `settings: 设置不可用`；桥在但 `available !== true` ⇒ `settings: 没有 appdata`（两种原因分开写；
  同一个脚本实例内改回来才清）。★ **空注册表**（桥可用但
  `settings.schema` 一个可画项都没有，`hasSettings(h) === false`）⇒ 也不打开，状态行
  `settings: 没有注册设置`（⛔ 不打开空白页、不造死控件），注册表回来才清。
- **设置状态行是模块级**（`hud.js` 的 `settingsPage.status`，由 `drawPage` 每帧写回；⛔ 不每帧新建）：
  成功点（`applied:true`）清空；失败（`settings: 只读` / `settings: <错误>`）留到下一次操作或下一次
  成功；**关闭页面不清**（状态行留在 HUD 上）。
- **PROFILE 默认隐藏**（不画、也不挂 `key` ⇒ 不可命中）。按键 = 固定 **F9**，读数 =
  `game.overlayKeySeq`（`undefined` = 没有源）；模块级记住上一次的值，**变了才切换**显隐（⛔ 当电平用）。
  显示时按钮画在面板**下方**（`key:"profile"`，`kit.Button` 的尺寸 / 主题）。★ F9 **不吞键**（仍到
  guest）⇒ 可能与游戏自己的 F9 动作同时触发；本期固定键，`--overlay-key` 记进 `## 未做`。
- **可复跑**：窗口档直接 `emulator-lift-aot --present window --overlay ...`，按 F9 两次看 PROFILE
  出现 / 消失（窗口档要真键；无键盘源时 `overlayKeySeq` 是 `undefined`，按钮不会出现）。结构判据 =
  `tests/ui_hud_script.cpp`（CTest `ui-hud-script`，假读数、无窗口 / 无 GPU）。

## 脚本形状

* **首选（S2）= 插件模块**：

  ```js
  const plugin = {
    render(ctx) { /* 每帧一次；用 ctx.draw / ctx.game / ctx.profile */ },
  };
  export default plugin;
  ```

  类型 = `scripts/plugins/plugin.d.ts` 的 `PluginModule` / `RenderCtx`。
* **旧经典形状（只剩内部 hud 回退使用；⛔ 不是 CLI 面）**：UTF-8、strict；两种形状（宿主**自动识别**，
  经典语义逐字不变）：
  * **classic**：顶层 `function render() { … }`（或 `globalThis.render = …`），⛔ 没有 `import`；
  * **module**：带 `import` / 顶层 `await` / `import.meta` 的脚本走 ES module loader。相对
    specifier 按**脚本自己所在目录**解析（不是 cwd），且只能在该目录的子树里；
* 旧入口必须是**显式赋值**：

  ```js
  globalThis.render = function render() { /* 每帧调用一次 */ };
  ```

  ★ module 里顶层的声明**不会**挂到 `globalThis` ⇒ 即便用 `import`，经典入口也**必须**显式写这一行。
  ★ `hud.js` 同时有两个入口：上面这行给**内部 hud 回退 loader**，`export default` 给插件
  （`--plugin hud` / `--overlay` 预设）。
* 经典 `render()` 里只能用三个宿主全局：`ui`（画 list）、`game`（读这一帧的读数）与 `host`（宿主能力，
  例如 PROFILE 的开始 / 结束）。类型声明见同目录的 `ui.d.ts`（随发行分发，⛔ 不参与构建）。
  插件 `render(ctx)` 的同一份数据面 = `ctx.draw` / `ctx.game` / `ctx.profile`（映射见下）。
* 每次调用前宿主已经清空 display list ⇒ `render` 只**追加**：⛔ 不要缓存 display list 元素 /
  不要建跨帧控件树（下一帧的 list 要从零再画）。★ **模块级 UI 状态是允许的**（例如 `hud.js` 的
  `profile.status` / `profile.running`）：那是脚本自己的状态，不是跨帧复用 display list。

```js
// 最小例子
globalThis.render = function render() {
  ui.rect({ x: 0, y: 0, w: 160, h: 40, fill: "#101820" });
  ui.text({ x: 8, y: 8, text: "frame " + game.frame, size: 16, color: "#EBFFEB" });
};
```

### `ui`

| 成员 | 说明 |
|---|---|
| `ui.rect({ x, y, w, h, fill, z?, key? })` | 参考像素（1280×720）的轴对齐矩形；`fill` = `#RRGGBB` / `#RRGGBBAA`，缺省 `#000000FF`。`key` **非空** ⇒ 这个矩形是一个命中区（见下），缺省 = 纯背景。★ `fill` 的 alpha == 0（如 `#00000000`）⇒ **不画**（只作**透明命中区**；⛔ 不会擦掉底下已画的像素 —— `hud.js` 的 `settings…` 入口就这么写） |
| `ui.text({ x, y, text, size?, color?, bold?, z? })` | 一行 UTF-8 文本；`size` 缺省 16，`color` 缺省 `#FFFFFFFF`，`bold` 缺省 false |
| `ui.measure(text, size) -> { w, h }` | 走宿主字体度量（`FontEngine`）；宿主没有字体 ⇒ `TypeError` |

`x/y/w/h/size` 必须是可转成 32 位整数的有限数字；`text` / `key` 必须是可转成 UTF-8 的字符串；
颜色只认 `#` 开头的 6 / 8 位 hex。未知键**暂时忽略**（校验表还没做，见需求单）。

### 命中 / 按钮原语（`key` 在 `ui.rect` 上）

C++ 侧**只有原语**（⛔ 没有 `ui.button`，也⛔ 没有 `ui.hit`）：`ui.rect({key})` 的 **resolved
rect**（缩放 / 居中 / DPR 之后的最终窗口像素）就是命中区。

* **命中顺序** = 上一帧**成功 present** 的绘制序**逆序**（后画的先命中）；没有成功帧 ⇒ 不命中。
* `game.click` = **一次性**对象 `{ key, x, y }`（`x/y` = 抬起点）：只在点击发生的那一帧有值，
  下一次 `render` 就是 `undefined`。⛔ 不要缓存它跨帧用。
* `game.hover` / `game.active` = 只读 key：指针下的 key / 当前被捕获的 key（没有 ⇒ `undefined`）。
* 事件边界：左键 down 命中 ⇒ **吞掉**并捕获；左键 up（捕获中）**无条件吞掉**，只有抬起点仍在
  同一条 rect 内才发一次 click；拖出 ⇒ 取消；motion / wheel / 右键**放行给 guest**；
  失焦 / 关窗 / 新的左键 down ⇒ 清捕获。

按钮的外观 / 状态在这上面用 JS 写，见 `kit.js`：

```js
import { Button } from './kit.js';

// 插件入口（S2）：把 ctx 适配器传给它（hud.js 的 pluginHost(ctx)）
export default {
  render(ctx) {
    Button({
      key: 'profile',
      rect: { x: 8, y: 8, w: 96, h: 28 },   // 参考像素；几何由调用方给
      label: 'PROFILE',
      on: { click: function (ev) { /* ev.key / ev.x / ev.y */ } },
    }, { rect: (o) => ctx.draw.rect(o.x, o.y, o.w, o.h, { fill: o.fill, key: o.key }),
         text: (o) => ctx.draw.text(o.x, o.y, o.text, { size: o.size, fill: o.color, bold: o.bold }),
         hover: ctx.game.hover, active: ctx.game.active, click: ctx.game.click });
  },
};

// 内部 hud 回退的经典入口：第二个参数省略 ⇒ 走全局 ui / game
globalThis.render = function render() {
  Button({ key: 'profile', rect: { x: 8, y: 8, w: 96, h: 28 }, label: 'PROFILE' });
};
```

`Button(spec, host?)` 返回当前状态（`'idle'` / `'hover'` / `'active'` / `'disabled'`）；
`host` 省略 = hud 回退的全局 `ui` / `game`（内部路径，⛔ 不是用户脚本入口）。`disabled: true` 的按钮
**不挂 key**（不可命中，宿主不会给它发 click）。★ 组件库只做**外观 / 状态 / 主题**：⛔ 不做通用
布局系统、不做窗口管理；`rect` 的 x/y/w/h 由调用方给。

`Checkbox(spec, host?)`（2026-11-25 追加；设置页的 bool 行用它）与 `Button` 同一套三态 / `disabled` /
`theme` / `on.hover|click` 口径，**同样不调 `measure`**。几何约定：`rect` 就是**整块命中区**
（例如设置页的一整行 26px），方框固定 **20×20** 贴 `rect` 右端、垂直居中（实心 = `checked`，
空心 = 未选中）；`label` 左对齐、`text_pad_x` 内缩。`disabled: true` ⇒ 不挂 key。

### 二级设置页 `settings-page.js`（D2/D4）

```js
import { canOpen, drawPage, hasSettings, PAGE_KEYS } from './settings-page.js';

// h = HudHost + h.config（`undefined` = 没有桥）；state = 调用方**模块级**持有（目前只有 status）。
if (canOpen(h) && hasSettings(h)) {
  const r = drawPage(h, state);
  if (r.closed) { /* 关闭二级页 */ }
}
```

- **`h.config`**：与插件 `ctx.config` **同一份 JS 形状**（`{ available, domain(id) }`），来路 =
  经典回退 HUD 的 `UiHostOps::config` / 插件档的 `ctx.config`（后者 **2026-11-26 加回 `RenderCtx`**；
  `docs/03-emulator/plugin-runtime.md`）。`undefined` ⇒ `canOpen(h) === false`、`drawPage` 返回
  `{ closed: true, status: "没有插件在跑" }` 且**不画控件**（⛔ 不造假开关）；生产上只有
  **测试替身**会造 `undefined`（`--plugin hud` / `--overlay` 都有 `ctx.config`，只是没有 `--appdata`
  时 `available:false`）。
- **`hasSettings(h)`**（`hud.js` 的入口用它）：`canOpen` 只回答"桥在不在"，本函数回答"打开之后有没有
  东西可画"（`settings.schema` 至少一项；坏形状照 `drawPage` 的规则跳过）。桥可用但空表 ⇒ **不打开**，
  状态行 `settings: 没有注册设置` —— ⛔ 打开一张只有 `SETTINGS` + live + `BACK` 的空白页。
- **读**：每帧每个可见项 `domain.get(key, default)`；`all()` **只**用于保留域 `settings.schema`
  （插件在 `start` 里 `registerSettings` 写进去；见 `scripts/plugins/README.md`）。坏 schema 形状跳过。
- **小节标题**：schema 带 `owner`（插件名）⇒ `<owner> · <title>`；缺省保持旧 `<title>`（⛔ 不写
  domain id —— 老的域表看起来与以前一样）。`live` 非空时仍追加 `  [<live>]`。
- **右键 = 返回**：`h.rightClick === true`（按下沿，由 `hud.js` 每帧算）⇒ 在交互段**最前面**照样
  `closed: true`，**任意位置**生效（含控件上），且同帧的 `h.click` 不再处理。左键逻辑不变。
- **写**：点击当拍 `set`；回执 `applied:false` ⇒ `settings: 只读` / `settings: 没有 appdata`；
  `set` 抛错 ⇒ 状态行 `settings: <message>`，**绝不冒出 render**。落盘由宿主在这次 render 末尾 flush。
  ★ 这几句是**调用方状态**（本文件没有模块级可变状态）：`hud.js` 用模块级对象持有 ⇒ 失败回执**跨帧
  留存**（成功点清空、关闭页面不清）—— ⛔ 不每帧新建那个对象（否则只活 1 帧）。
- **版式**：全屏背板 `#000000A6` + `key:"settings.bg"`（先画 = 模态）；页板 `x=0`、
  `y=round((720-H)/2)`、`PAD=8`、行高 `max(20, measure("Ag",16).h+2)`；bool 整行 26px 命中区
  `set:<domain>:<key>`；number `[−]`/`[+]` 24×24（`dec:`/`inc:`）+ 72 宽值文本；`BACK` 112×28
  （`settings.back`）；最多 12 项，超出 `… 还有 N 项`。
- ★ **`hud.js` 已经接上它**（D4）：最后一行 `settings…` 的透明命中 Rect（`key:"settings"`）打开页面；
  页面开着时 `hud.js` **不画 HUD 面板**、只画 `drawPage(h, state)`，`BACK` 关闭并回到 HUD。
  页面里的 `set/inc/dec` 由本文件处理（`h.config` 的写回执它自己消化）。**任意位置右键**也关闭页面
  （`h.rightClick` 的按下沿；与 `BACK` 同一条出口）。
- 判据 = `tests/ui_settings_page.cpp`（pure；假 `h` + 假 `config`）：版式/命中 key、bool 整行点击
  （`set` 恰好一次 + 同帧值变）、min/max 钳制与 `<invalid>`、`set` 抛错不冒出 render、
  `h.config === undefined` ⇒ 不打开、13 项 ⇒ 12 行 + `… 还有 1 项`、`owner` 标题前缀与无 owner 回退、
  任意位置右键 ⇒ `closed:true` 且不触发任何 `set`。

### 旧全局 ↔ 插件 `ctx` 的映射（S2）

| 旧（内部 hud 回退 / classic） | 插件（`--overlay` / `--plugin hud` / `--plugin <path>`） |
|---|---|
| `ui.rect({x,y,w,h,fill,key?})` | `ctx.draw.rect(x, y, w, h, { fill, key? })` |
| `ui.text({x,y,text,size,color,bold?})` | `ctx.draw.text(x, y, text, { size, fill, bold? })` |
| `ui.measure(text, size) -> {w,h}` | `ctx.draw.measure(text, { size }) -> {width,height}` |
| `game.frame/fps/script/worst/click/hover/active` | `ctx.game.frame/…`（同名只读 getter） |
| `game.overlayKeySeq`（D4） | `ctx.game.overlayKeySeq`（同名只读 getter） |
| `game.mouse`（getter） | `ctx.game.mouse()`（**方法**） |
| `game.local_frames()` | `ctx.game.localFrames()` |
| `host.profileBegin/End()`（结果对象） | `ctx.profile.begin()`（`void`）/ `ctx.profile.end()`（`{ok,text,error}`） |
| `host.config`（D2；属性；无桥 ⇒ `undefined`） | `ctx.config`（**2026-11-26 加回** `RenderCtx`，7→8 成员；同一形状 / 同一份实现） |

### `host`（方法表；**只在 `render` 内可调用**）

★ **S2**：插件侧用 `ctx.profile.begin(label)`（`void`；失败 = `TypeError`）/
`ctx.profile.end()`（返回与旧 `host.profileEnd()` 同形的 `{ok,text,error}` —— HUD 靠 `text` 判
`INVALID`；结束走 HostPause）。下面这节是**内部 hud 回退**的全局契约（⛔ 不是 CLI 面）。

`host` 是**方法表**（像 `ui`），不是读数（像 `game`）：对象**恒存在**，缺接线是**调用时**的
`TypeError`（⛔ 不是 `typeof host === 'undefined'` 那种形状差异）。

| 成员 | 说明 |
|---|---|
| `host.profileBegin(label) -> { ok, error }` | 开始一段窗口 profile；`label` = UTF-8（缺省串由宿主决定）。`ok:false` ⇒ 已有一段开着（**不 reset** 上一段），`error` 是人可读的原因 |
| `host.profileEnd() -> { ok, text, error }` | 结束这一段窗口 profile。**宿主内部**把 `text`（结论文本）写进系统剪贴板；`ok:false` ⇒ 没有开着的窗口（`text` 为空串） |
| `host.config` | `{ available, domain(id) } \| undefined`（**属性**，D2）：与插件 `ctx.config` 同一份 JS 形状；**没有接线 ⇒ `undefined`**（⛔ 不是抛错：HUD 每帧都读）。`available:false` / `undefined` ⇒ 设置页不打开（见下）。写盘 = 该帧 render 之后 flush 一次 |

* **返回对象所有键恒存在**；`error` 空串 = 没有错。
* **结束 ≠ 复制成功**：`ok:true` + `error` 非空 = "窗口关了、`text` 在，但没进剪贴板"。
  `text` **不回滚**（需求单明文"INVALID 仍复制"）。
* `INVALID` 的判定：报告文本里含 `INVALID` 这个 token（`sampled==0` / `unaccounted` 超限时
  C++ 侧写进去）。⛔ 脚本侧不要另立判据。
* 默认 HUD 的四态状态行：`开始` / `已复制` / `失败：<error>` / `INVALID（已复制）`。
* **用法错 ⇒ `TypeError`**（在 `render` 之外调用 / `label` 不可转成字符串 / 宿主没接线）；
  **领域失败 ⇒ 返回值**（不抛：HUD 要显示"失败"并继续画，抛错会让叠层被永久关掉）。
* ⛔ 副作用只发生在 `render` 里（`handleEvent` 不跑 JS / 不开 profile / 不写剪贴板）。

### `game`（只读 getter；这一帧没有值 ⇒ `undefined`，⛔ 不是 0）

★ **S2**：插件侧同一份读数在 `ctx.game.*`（同名只读 getter；`mouse` 是**方法** `mouse()`，
`local_frames()` 叫 `localFrames()`）。下面这节是**内部 hud 回退**的全局契约（⛔ 不是 CLI 面）。

| 属性 | 类型 | 什么时候有 |
|---|---|---|
| `game.frame` | `number` | 总是有（Present 计数） |
| `game.fps` | `number \| undefined` | 最近 1 s 至少 Present 过一次 |
| `game.mouse` | `{ x, y } \| undefined` | 有主窗口（guest 客户区坐标） |
| `game.script` | `string \| undefined` | 采到脚本身份且非空 |
| `game.worst` | `{ wall_ms, guest_ms?, shim_ms } \| undefined` | 最近 3 s 有样本；`guest_ms` **没开块计时时没有这个键** |
| `game.local_frames()` | `FrameInfo[] \| undefined` | 见下 |
| `game.click` | `{ key, x, y } \| undefined` | 这一帧有一次"命中并抬起"（**一次性**） |
| `game.hover` | `string \| undefined` | 指针在某个命中区上 |
| `game.active` | `string \| undefined` | 左键在某个命中区上按下、还没抬起 |
| `game.overlayKeySeq` | `number \| undefined` | D4：固定 **F9** 的**单调按下沿计数**（按下沿 +1、饱和不回绕）；没有按键源（headless / 测试 sink）⇒ `undefined`（⛔ 不编 0）。用法 = 记住上一次、**变了才切**显隐（⛔ 当电平用）。⛔ 不吞键：F9 仍到 guest |

属性是 getter（无 setter）：strict 下赋值抛 `TypeError`。`game.local_frames()` 是 `game` 上**唯一**的
**方法**（不可写 / 不可枚举 / 不可配置，strict 下赋值同样抛 `TypeError`）。

### `game.local_frames()` —— 帧列表 / 栈顶脚本名

guest 的帧区是 **40 槽**（每槽一条记录），宿主**按需**查询（⛔ 不是每帧推表），返回
**活动帧**数组（top-first）或 `undefined`：

```ts
FrameInfo = {
  slot: number;           // 0..39
  script_id: number;      // 记录 +0x3C（统一文件 id）的 uint32 原值
  parent: number;         // 记录 +0x38（parent）的 uint32 原值；4294967295 = 根
  script: string;         // 名字；读不到时是 `id:0x%08X`
  name_degraded: boolean; // script 是降级形态（id / 这一项仍然有效）
  top: boolean;           // slot === cur（当前执行点）
  pos?: number;           // 脚本内位置（label 坐标 = 0x3C + (pc − base)）；**只有 top 帧有**
}
```

* **活动** = 该槽的脚本体基址非 0 且已提交。⛔ 不是"id 非 0"、也⛔ 不是"0..cur 全算"
  （引擎可以乱序装载 / 释放任意槽、留下空洞）。
* **top** = `slot === cur`。引擎**没有独立的栈顶变量**，`cur` 是唯一执行点 ⇒ "栈顶"就是这个槽；
  `cur` 那一格不活动（例如根还没装载）时列表照给、**`top` 全 false**（⛔ 不挑最高槽冒充）。
* **顺序 = top-first**：`cur`（若活动）第一，其余活动槽按**槽号降序**。`parent` 原样给出，
  调用链由消费者自己走（这一版不做链排序）。
* **读不到 ⇒ `undefined`**（⛔ 不编空数组）：宿主会一次性在 stderr 打一行 `[frames] why=<原因>`。
  单槽读不到 ⇒ 那一项不进表（保守跳过，不是猜）。
* **`pos`（切片 3）**：只有 `top === true` 的那一项给 `pos`（= `0x3C + (pc − base)`，与
  `await position` **同一坐标系、同一公式**）；非 top 帧的 `+0x04` 是"该脚本被挂起时保存的
  续延 PC"，**不是**执行点 ⇒ 这一版直接不暴露（`undefined`）。采样时刻 = Present（不是块边界）：
  对 `pos >= X` 这类单调条件没有区别；窄区间可能"两次 Present 之间整个穿过"而漏掉。
* `script_id` / `parent` 是 **uint32 原值**：`0xFFFFFFFF` 在 JS 里是 `4294967295`，⛔ 不转成 `-1`。
* 默认 HUD 的 `script` 行取 `top === true` 的那一项（`cur` 那一格不活动 ⇒ `top` 全 false ⇒ 回退 `game.script`）。

## 安全边界（沙箱）

脚本跑在**裁剪过的** QuickJS 上下文里，⛔ 没有：`eval`、`Function`（含
`Function.prototype.constructor`）、`Date`、`Math.random`、`RegExp`、`Proxy`、`Map/Set`、
`TypedArray`、`WeakRef`、`performance`、`JSON`；⛔ 没有文件 / 网络 / 进程 / `print`。
★ `Promise` **有**（module 求值是 Promise；用户 2026-11-19 裁决"加而不删"），`BigInt` 是 0.18.0
`BaseObjects` 自带的当前表面 —— 保留的删除只有**功能目的**（`Math.random` 的确定性，
`eval` / `Function` / `.constructor` 的禁动态编译）。同一份读数**必然**产出同一份 list（可测）。

预算：内存 16 MiB、栈 512 KiB、每帧墙钟 20 ms + 100 万条字节码（超了 ⇒ 本帧失败，宿主关掉叠层
并照旧呈现）。这些上限**不是**给你写重活的：HUD 是诊断层，⛔ 不要在 `render` 里做耗时计算。

## 编辑器

目录里的 `tsconfig.json` 是**编辑器 / 手动检查**用的工程（`allowJs` + `checkJs` + `strict`）：
VS Code 打开 `scripts/ui` 就按它检查，`ui` / `game` / `render` 的类型与跳转来自同目录的 `ui.d.ts`。

```bash
pnpm exec tsc -p apps/emulator-lift/scripts/ui   # exit 0、无输出 = 干净
```

★ 它**不进** `pnpm typecheck` 的六份工程：它是脚本作者的编辑器提示，不是仓库构建门禁
（`ui.d.ts` 也不参与构建 / 不进发行 exe）。★ `tsconfig*.json` 属生态配置，不需要同名 `.md`
（`tools/test/json-docs.test.mjs` 的排除项）。

## 现在**没有**的

* **热重载**：脚本在装叠层时编译一次（插件换文件要重启；`--overlay` / `--plugin hud` 每次启动
  重读）。⇒ `host.reload()` / `host.time_budget_ms()` **都还没实现**（`overlay.md` §3.5 的目标 API 里的
  另外两项；调用时是 `TypeError`（没有这个成员），⛔ 不是静默 no-op）。
* 通用布局 / 锚点 / 百分比 / 窗口管理：组件库只按调用方给的 `rect` 画，命中区是宿主从
  `ui.rect({key})` 的 resolved rect 算的（`ui` 目前只有 `rect` / `text` / `measure`）。
* 校验表（未知键被忽略，不报错）。
* 发行打包规则（R10）：发行清单 = `scripts/ui/{hud.js,kit.js,settings-page.js,ui.d.ts,README.md}`，
  一起放到 `<exe 所在目录>/scripts/ui/`。**`hud.js` / `kit.js` / `settings-page.js` 必须同目录**
  （相对 `import` 按 `hud.js` 所在目录解析；缺任一 = 加载期硬失败 exit 2）。`release/` 目前还没有
  "复制 `scripts/ui/*`"的步骤 ⇒ 打包流程要自己按这份清单带上。（权威表 = `docs/03-emulator/overlay.md`
  的 R10 行，随文档轮同步。）

`hud.js` 的版式（贴左 + 垂直居中 / `PAD=8` / 字号 16 / 行序 / 末行 `settings…` / 颜色 `#101820CC`
80% 半透明底 + `#EBFFEB` 不透明字）就是 `tests/ui_hud_script.cpp` 的判据：改它要么同步改用例，
要么那条用例会红。

## 已知边界（半透明）

两条呈现路径**都**按 `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` 混合：
SDL_Renderer 用 `SDL_BLENDMODE_BLEND`；GPU 直出用一条专用混合绘制管线（`SDL_DrawGPUPrimitives`），
`directReadback` 用同一整数公式合成 ⇒ 屏幕 / 截图同值（±1 舍入）。
两条路径的窗口像素判据因此**共用**同一个混合期望值。
