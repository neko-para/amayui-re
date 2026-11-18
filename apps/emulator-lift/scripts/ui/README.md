# `scripts/ui` —— HUD 脚本（`--overlay` / `--ui`）

`--overlay` 的左上角 HUD **不是 C++ 画出来的**：它由这份目录里的 JS 脚本经 QuickJS **同步**跑出
一份 display list，宿主再交给宿主字体栅格化、合成进窗口像素。默认脚本就是 `hud.js`。

★ 脚本是**外置文件、运行时读**的：改完直接重跑，**不需要重编**（也⛔ 不要把 JS 抄进 C++）。

## 怎么用

```bash
# 默认 HUD（外置 hud.js）
emulator-lift-aot --present window --overlay ...

# 自己的脚本（--ui 隐含 --overlay；与 --overlay 同给时 --ui 赢）
emulator-lift-aot --present window --ui E:\my-hud.js ...
```

`--overlay` 且没给 `--ui` 时的解析顺序：

1. `<exe 所在目录>/scripts/ui/hud.js` —— **发行布局**（`aot_main` 从 `argv[0]` 取目录）；
2. `<LIFT_UI_SCRIPT_DIR>/hud.js` —— **开发回退**（构建时注入的源码树路径，指向本目录）；
3. 都不在 ⇒ **拒绝启动**，错误里点名试过的两个路径（⛔ 不静默回退成"空 HUD"）。

**加载期**的失败（语法错 / 缺入口 / 字体不可用）是**硬失败**：`--overlay` 在启动期就 exit 2，
`--ui` 的错误串带 `<path>:<行>:<列>`（字体不可用 = R5）。**运行期**抛错 / 超预算不是启动失败：
一次性 `[overlay] …` 进 stderr + **关掉叠层**，**呈现照旧**（exit 0）。

## 脚本形状

* UTF-8、单文件、**无 `import`**、strict 语义；
* 入口必须是**显式赋值**：

  ```js
  globalThis.render = function render() { /* 每帧调用一次 */ };
  ```

* `render()` 里只能用两个宿主全局：`ui`（画 list）与 `game`（读这一帧的读数）。
  类型声明见同目录的 `ui.d.ts`（随发行分发，⛔ 不参与构建）。
* 每次调用前宿主已经清空 display list ⇒ `render` 只**追加**，不要保存状态。

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
| `ui.rect({ x, y, w, h, fill, z? })` | 参考像素（1280×720）的轴对齐矩形；`fill` = `#RRGGBB` / `#RRGGBBAA`，缺省 `#000000FF` |
| `ui.text({ x, y, text, size?, color?, bold?, z? })` | 一行 UTF-8 文本；`size` 缺省 16，`color` 缺省 `#FFFFFFFF`，`bold` 缺省 false |
| `ui.measure(text, size) -> { w, h }` | 走宿主字体度量（`FontEngine`）；宿主没有字体 ⇒ `TypeError` |

`x/y/w/h/size` 必须是可转成 32 位整数的有限数字；`text` 必须是 UTF-8 字符串；颜色只认 `#` 开头
的 6 / 8 位 hex。未知键**暂时忽略**（校验表还没做，见需求单）。

### `game`（只读 getter；这一帧没有值 ⇒ `undefined`，⛔ 不是 0）

| 属性 | 类型 | 什么时候有 |
|---|---|---|
| `game.frame` | `number` | 总是有（Present 计数） |
| `game.fps` | `number \| undefined` | 最近 1 s 至少 Present 过一次 |
| `game.mouse` | `{ x, y } \| undefined` | 有主窗口（guest 客户区坐标） |
| `game.script` | `string \| undefined` | 采到脚本身份且非空 |
| `game.worst` | `{ wall_ms, guest_ms?, shim_ms } \| undefined` | 最近 3 s 有样本；`guest_ms` **没开块计时时没有这个键** |
| `game.local_frames()` | `FrameInfo[] \| undefined` | 见下 |

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
* `script_id` / `parent` 是 **uint32 原值**：`0xFFFFFFFF` 在 JS 里是 `4294967295`，⛔ 不转成 `-1`。
* 默认 HUD 的 `script` 行取 `top === true` 的那一项（`cur` 那一格不活动 ⇒ `top` 全 false ⇒ 回退 `game.script`）。

## 安全边界（沙箱）

脚本跑在**裁剪过的** QuickJS 上下文里，⛔ 没有：`eval`、`Function`（含
`Function.prototype.constructor`）、`Date`、`Math.random`、`RegExp`、`Proxy`、`Map/Set`、
`TypedArray`、`Promise`、`BigInt`、`WeakRef`、`performance`、`JSON`；⛔ 没有文件 / 网络 / 进程 /
`print`。所以同一份读数**必然**产出同一份 list（可测）。

预算：内存 16 MiB、栈 512 KiB、每帧墙钟 20 ms + 100 万条字节码（超了 ⇒ 本帧失败，宿主关掉叠层
并照旧呈现）。这些上限**不是**给你写重活的：HUD 是诊断层，⛔ 不要在 `render` 里做耗时计算。

## 现在**没有**的

* **热重载**：脚本在装叠层时编译一次（`--ui` 换文件要重启；`--overlay` 每次启动重读）。
* 布局 / 锚点 / 百分比 / 命中测试 / 多文件 import（`ui` 目前只有 `rect` / `text` / `measure`）。
* 校验表（未知键被忽略，不报错）。

`hud.js` 的版式（内边距 8 / 字号 16 / 行序 / 颜色 `#101820CC` 80% 半透明底 + `#EBFFEB` 不透明字）
就是 `tests/ui_hud_script.cpp` 的判据：改它要么同步改用例，要么那条用例会红。

## 已知边界（半透明）

两条呈现路径**都**按 `SRC_ALPHA / ONE_MINUS_SRC_ALPHA` 混合：
SDL_Renderer 用 `SDL_BLENDMODE_BLEND`；GPU 直出用一条专用混合绘制管线（`SDL_DrawGPUPrimitives`），
`directReadback` 用同一整数公式合成 ⇒ 屏幕 / 截图同值（±1 舍入）。
两条路径的窗口像素判据因此**共用**同一个混合期望值。
