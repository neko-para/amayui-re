# overlay UI：插件化宿主（一个 host · 同一套 API）

> 本文是 overlay / UI 层的**设计裁决**：只写不变量、边界与可红判据。实施从 **M0** 起（§4）；
> 进度看 `pnpm tools requirements plan`，⛔ 本文不写状态与计数（`AGENTS.md` §10）。绑定 / 沙箱 / 值转换见
> `overlay-quickjs.md`；坐标合同见 `interaction-model.md` §1；控制层见 `control-plane.md`。
> 取证口径：C++ 事实给 `文件:符号`；引擎语义给 **EA**；盘点读数取证于 2026-11-18（main / win32）。

## 1. 能力盘点（读代码得到的事实）

> ★ **本节是 M0 前（2026-11-18）的盘点快照**，保留下来是为了说明"为什么要改"；逐条标了"M0 前 / M0 起"的时点。
> 当前实现的判据不在这里（见 §4）。

### 1.1 唯一一份 overlay 实现

| 项 | 事实 | 落点 |
|---|---|---|
| 数据 | 固定字段的结构体，不是声明式界面 | `runtime/overlay.h:OverlayState` / `OverlayStatus` |
| 内容 | 固定 7 行顺序：mouse / script / fps / frame / raster / present / worst | `runtime/overlay.cpp:formatOverlayLines` |
| 字体 | 5×7 位图，**只覆盖 ASCII**；未覆盖字符按空格画（`overlayGlyph` 返回 false） | `overlay.h:kOverlayGlyphW/kOverlayGlyphH`、`overlay.cpp:overlayGlyph` |
| 形状 | 一块不透明深底矩形 + 亮字；1 px 内边距 / 字距 / 行距；矩形外一个字节都不动（哨兵缓冲守卫） | `overlay.cpp:overlaySize/renderOverlay`、`tests/overlay.cpp` |
| 输入 | **零**：`handleEvent` 把键 / 鼠 / 轮全部交给 `input::*` | `present_sdl.cpp:SdlWindow::Impl::handleEvent` |
| 可见性 / 条件 | 只有全局 bool（默认关）；没有 `when` / 元素 / 布局（内容由 C++ 固定） | `present_sdl.h:SdlWindow::setOverlay`、`present.h:PresentSink::overlayEnabled` |
| 节流 | 文本最多 100 ms 重建一次；鼠标位置变化立即；**M0 前**：内容不变不重传。★ **M0 起**：sink 侧每帧重新栅格化 / 重上传（只有"文本重建"被 100 ms 节流；dirty-upload 留 M1/M2） | `present_sdl.cpp:kOverlayRefreshNs` |

**能**：在画面矩形左上角显示一块 ASCII 文本（鼠标 / 脚本身份 / fps / 帧号 / 光栅后端 / 呈现路径 / 最近 3 s 每帧最大值）；只改宿主送显像素；两条呈现路径都画；有守卫。
**不能**：面板 / 边框 / 图片、任意位置 / 锚点 / 缩放、中文、按 `script+pos` 显隐、多块界面 / z-order / 裁剪、点击命中与吞掉、插件化 / 热重载、任何声明式或脚本描述。

★ **2026-11-18 用户裁决（M2a）**：HUD 的 `raster` / `present` 两行**去掉** —— 开 overlay 时是人在体验，
这两个开关信息没有价值（不是"暂时隐藏"，是**不再产出**）。同一裁决下 HUD 改由
`scripts/ui/hud.js` 经 QuickJS 产出（见 `overlay-quickjs.md`），行顺序 = mouse / script / fps / frame / worst。

### 1.2 两条呈现路径的坐标语义已经不一致

| 路径 | 合成点 | 坐标语义 |
|---|---|---|
| SDL_Renderer | 主纹理之后、`--present-screenshot` 之前 | **逻辑坐标 (0,0) = 画面左上角**，随 `LETTERBOX` 缩放（`present_sdl.cpp:Impl::drawOverlayRenderer` 的 `SDL_RenderTexture(..., {0,0,w,h})`） |
| GPU 直出 | `letterbox` blit **之后** | 锚在画面矩形左上角，**1:1 窗口像素、不缩放**，超出 swapchain 裁掉（`d3d9_gpu.cpp:Impl::directPresent` → `Impl::blitOverlay`；回读时 `Impl::directReadback` 把同一块从 CPU 副本合成进窗口像素 —— swapchain 是 write-only） |

⇒ **M0 前**：同一份行、两种大小（M0 起两条路径已统一到 `presentOverlay` 的画布原点，见 §4 B1/B2）。新 UI 层必须先裁决掉它（D5）。

### 1.3 CLI 可达性与其它树

- **M0 之前**：`--overlay` 只在解释器臂的窗口参数清单里（`runtime/present_flags.h:kWindowFlagsInterpreter`）；
  AOT 臂的清单不含它（`present_flags.h:kWindowFlagsAot`），且解释器的 `main.cpp` 已删
  （`git log --oneline -- apps/emulator-lift/src/main.cpp`）⇒ 生产上不可达，只有 CTest
  通过 `SdlWindow::setOverlay` 能打开它（`tests/overlay.cpp`）。
- **M0 起**：`kWindowFlagsAot` 已含 `--overlay`（AOT 臂 `aot_main.cpp` 解析并接线），
  `--present window --present-hidden --overlay --max-frames 2` ⇒ exit 0（`tests/aot_overlay.cpp`）；
  `--present headless` 仍按窗口参数拒绝它。
- `apps/emulator` / `apps/emulator-new` **没有宿主 overlay**：前者只命中一条 VFS 注释（`src/host/fs.ts`）；
  后者的 `KIND_TEXTURE_OVERLAY` / `KIND_MOVIE_OVERLAY` 是 **guest 场景绘制**，不是宿主叠层
  （`src/platform/electron/renderer/scene-command-plan.ts`）。

## 2. 裁决（D）

| # | 裁决 | 边界 / 理由 |
|---|---|---|
| D1 | **一个 host + 同一套 API**：内置 HUD 与将来的"游戏内小窗口"是**同一类插件** —— 同一个 `UiHost`、同一份 display list、同一条输入路由 | 用户口径。⛔ 不为 HUD 单开一条渲染 / 输入路径 |
| D2 | **插件化**：一个插件 = 一个脚本文件（`apps/emulator-lift/scripts/ui/<name>.js`）；`--ui <path>`（可重复）、`--ui-input=consume\|pass`、`--ui-watch`（默认关）、`--ui-strict` | 编译进 C++ 的固定 HUD 不能扩展；热重载默认不做任何 stat。★ **`--ui-input=consume\|pass` 已实现（S9）**：缺省 **consume**；也收分离写法；它是**窗口参数**（`--present headless` 与 `--overlay` 走同一条 `present_window_flag` 拒绝）；非法值 `exit 2` 并点名合法值。★ `--ui-watch` / `--ui-strict` 仍未实现 |
| D3 | **语言 = JS/TS + QuickJS-ng v0.18.0**（逐字节原样 vendor，见 `third_party/quickjs-ng/README.md`；MIT） | 选型只看硬约束：可裁剪、可中断、迭代顺序按规范。Lua 只作对照（⛔ 不是方案）：longjmp 跨 C++ 帧、hash seed 每进程随机、x64-windows dynamic 要带 DLL |
| D4 | **每帧 display list**（立即模式）：脚本只产出元素描述，宿主独占校验 / 求值 / 布局 / 命中 / 光栅 / 合成；宿主只缓存**上一帧** resolved list 用于点击 | 没有跨帧控件树、脏矩形与 GPU 资源生命周期；resize / 缩放天然同步 |
| D5 | **坐标**：声明用 **1280×720 参考系**，合成用**窗口像素**（letterbox 之后）；等比 `s = min(pic_w/1280, pic_h/720)`、画面内居中；文字按最终像素重绘（freetype），⛔ 不放大位图 | 两条路径统一到窗口像素；`--scale` 只要仍走 `cursor_map.h:letterboxFor`，UI 零改动 |
| D6 | **输入单一 router**：OS 事件 / 注入 down / 注入 up 三个入口，都在调用 `input::*` **之前**问同一个 `UiClickRouter`；左键命中即吞 + 捕获；**down 吞 ⇒ up 吞**，**down 放 ⇒ up 放**；右键 / 中键 / 滚轮 / 键盘 M1 一律放行；motion 永远放行 | `input::*` 仍是唯一施加路径（`runtime/input.h` 口径不变）；吞掉 = **不调用** `input::*`。★ **实现范围 = `click`**：三个入口已接同一个 router（OS 在 `present_sdl.cpp`、注入在 `input_plan.cpp` 的 `armClickAt` / `emitRelease`）；`mouse_move` / `mouse_down` / `mouse_up` 三条原语**不过 router**（S9 residual —— 接上去会改拖拽 / 显式 hold 的语义） |
| D7 | **不写 guest**；`--present-screenshot` **含** UI；`--dump-frame` / `arena.hash` / 控制层 `screenshot`（guest 后缓冲冻结副本，`control_snapshot.h:ControlScreenshot.source`）**不含** | 与旧 overlay 同一条硬线；UI 只存在于宿主送显的窗口像素里 |
| D8 | **失败响亮**：启动期（编译 / 校验 / 缺资源）`exit 2` + `origin:行:列`；运行期 = 卸载该插件 + **一次性** stderr；⛔ 不画空格 / 方块 | 静默降级等于"看起来正常但界面错了" |
| D9 | **默认档零成本、零周期输出**：没有 `--ui` ⇒ 一次 bool 判空；有插件但本帧无可见元素 ⇒ 不布局 / 不栅格 / 不上传 / 不打印；`[ui]` 行按（插件, 错误种类）去重 | `AGENTS.md` §12.8；一次性收口行不收 |
| D10 | ★ **2026-11-19 部分反转**（原文：「⛔ **无 Promise / async**：`render` 必须**同步**产出 display list；宿主没有 job 队列」）：`Promise` **存在** —— `JS_AddIntrinsicPromise` 且**不删**（module 求值返回 Promise，是硬前提；需求单 `REQ-01M7QF01A00F9946DX0GPH23JQ`「裁决（2026-11-19）」第 4 条）；宿主有 job 泵（`script_quickjs.cpp:drainPendingJobs` → `JS_ExecutePendingJob`），但**只**跑 module 求值 / 关闭时的 microtask。**没变的**：`render` 仍必须**同步**产出 display list（没有跨帧 async / 协程）；`handleEvent` 仍不跑 JS | 一帧的 UI 由这一帧的输入决定；D10 限的是"跨帧异步"，不是语言里有没有 Promise |
| D11 | AOT 的 `script/pos` **块入口采样门是新增面**，不是"接上已有的" | 解释器有 `Machine::sampleMarker`；AOT 只有控制层 `sample` 时读一次（`aot_host.h:control_marker_`） |
| D12 | **不保留第二套**：M0 把两条绘制路径合并成 `presentOverlay`；M2 删 `formatOverlayLines` / `OverlayState` / `OverlayStatus`；`--overlay` 从 M2 起只是**外置** `scripts/ui/hud.js` 的参数别名（`--ui <path>` 打开任意脚本） | 两套实现必然漂移；旧 5×7 点阵随 M2 一起删（`--ui-glyphs bitmap` 测试档本切片不开） |

## 3. 数据模型与 API

### 3.1 元素（闭集合）

| kind | 必有 | 可选 |
|---|---|---|
| `screen` | `id` | `when` `z` `enabled` |
| `panel` | `id` + (`rect` 或 `anchor`+`size`) | `style` `clip` `z` `when` `interactive` |
| `rect` | `id` `rect` | `style` `z` `when` `interactive` |
| `text` | `id` + (`at` 或 `anchor`) + `text` | `size` `bold` `color` `align` `wrap` `when` `z` |
| `image` | `id` `rect` `src` | `fit`(stretch/contain/none) `tint` `when` `z` |

共同属性：`id`（`[A-Za-z0-9_.-]`，插件内唯一）、`when`、`z`（整数，缺省 = 文档顺序）、`visible`、
`interactive`（缺省 false）、`action`（`hide:<id>` / `toggle:<id>` / 回调名）、`pass_through`。

**布局**：绝对 `at`（参考像素）· 9 宫格 `anchor`（默认相对画面矩形，可选 `window`）· `size` 支持参考像素 /
百分比 / `auto`（panel = 子元素并集 + padding；text = 度量；image = 原始尺寸 × scale）· `scale` 相对参考系 ·
嵌套子元素相对父内容区、`clip` 缺省 on。

**样式**（最小集，未知键 = 启动硬错）：`fill` / `border` / `radius` / `padding` / `color` / `align` /
`line_height` / `font` / `opacity`。`text.size` 单位是**参考像素**，实际字形像素 = `round(size * s)`；
字形由**宿主字体**（FreeType）按最终像素取字（D5 / U1：支持中文，⛔ 不放大位图、⛔ 不退回 5×7 点阵）。

### 3.2 `when`（宿主要值的声明式门）

```js
when: { script: "SC0000", pos: [0x039C, 0x0460] }   // 与：身份 + 半开区间
when: { all: [A, B] }                                // 组合子：any / not 同形
when: { script: ["SC00*", "TITLE"] }                 // 多个候选 = 或
```

- `script` 匹配必须复用 `recipe.h:scriptNameMatches`（ASCII 大小写不敏感、两侧忽略 `.bin`、接受
  `id:0x%08X` 退化名）。⛔ 不另写一套匹配。
- `pos` = 半开区间 `[from,to)`（`from` 含、`to` 不含），解析期校验 `from < to`；与配方 `await position`
  同一份定义（`engine_marker.h:readScriptPositionSample`；采样点 `0x4123F0` / `0x4123E5`）。
- `script_ok && pos_ok` 才可能为真；不可读 ⇒ **false**（不是错误），计一个 `when_unavailable`。
- `ui.show(id)` / `hide(id)` 是同一字段之上的**覆盖层**：最终可见 = `when` **且** 覆盖状态；`hide` 一律赢，
  `show` 不能越过 `when`（⇒ 用 `show/hide` 驱动的面板不要同时写 `when`）。

### 3.3 z / clip / 命中

- display list 是**有序数组**：顺序 = 脚本调用 `ui.*` 的时刻，宿主给每个元素分配单调 `seq`。
  绘制按 `z` 升序、同 `z` 按 `seq` 升序（稳定）。
- `clip` 把子元素裁到内容矩形，被裁掉的部分**也不参与命中**；命中按绘制顺序**逆序**取第一个
  `interactive && visible`、且点在裁剪后矩形内的元素（`pass_through` 不参与）。
- 命中矩形直接用 resolved list 的**窗口像素** rect，⛔ 命中时不再做一次布局（避免"看到的"与"点到的"漂移）。
- 捕获状态机（D6）：`down` 命中 ⇒ `Captured`，`up` 无条件吞；`down` 放行 ⇒ 同按钮 `up` 必须放行。

### 3.4 校验（启动期；失败 = `exit 2` + `origin:行:列`）

未知 kind / 属性 / 类型 / `id` 重复或非法 / `when` 未知键 / `pos` 非 hex 或 `from>=to` / `src` 缺失或非 PNG /
`action` 悬空 / 百分比 `size` 无父容器 / 嵌套深度 > 8 / 元素数 > 2048 ⇒ 硬错。**一张表是唯一真源**；⛔ 不许"不认识就跳过"。

### 3.5 宿主 API（注入给脚本的能力面）

| 命名空间 | 成员 | 来源 / 边界 |
|---|---|---|
| `game` | `frame` / `insns` / `guest_ms` / `script` / `script_id` / `script_ok` / `metrics()` | 只读；`metrics()` 字段可缺，缺 ⇒ 脚本整行 / 整块省略。★ 位置读数**不在** `game` 上，走 `game.local_frames()[i].pos`（见下） |
| `game` | `local_int/float/string(i)`、`global_int/float/string(i)` | 帧记录 / 全局槽位只读；越界 ⇒ `undefined` + 计数（⛔ 读邻池） |
| `ui` | `screen/panel/rect/text/image`、`measure(text,size)`、`show/hide/toggle`、`log(level,msg)` | 唯一绘制入口；`log` 有界去重 |
| `host` | `profileBegin(label)`、`profileEnd()` | 宿主能力（方法表）。结束窗口 profile 并**在宿主内部**把结论文本写进系统剪贴板；返回 `{ok,error}` / `{ok,text,error}`（错误串口径见下） |
| `probe` | `read_u32(ea)`、`read_bytes(ea,n)` | **只读**、有上界、越界即错；必须显式 `--ui-probe`，启动打一行警告 |

★ **M2a 已落地的子集**（2026-11-18；**2026-11-19 切片 3** 追加 `host.profileBegin/End` 与帧列表
`pos`；**C2b-2** 追加 `mouse.buttons?`）：`game` = `frame` / `fps` / `mouse{x,y,buttons?}` / `script` /
`worst{wall_ms, guest_ms?, shim_ms}`（不可用 ⇒ `undefined`，⛔ 不编 0）；`ui` = `rect` / `text` /
`measure`（绝对参考像素；未知键 **暂时忽略**，M1 的校验表之后接）；`host` = `profileBegin` /
`profileEnd`（**只在 render 内可调用**；领域失败走返回值、⛔ 不抛 —— 抛错会让叠层被一枪永久关掉）。
`insns` / `guest_ms` / `script_id` / `metrics()`、`probe`、`show/hide/toggle/log`、`local/global` 未接（M3）。
★ `mouse.buttons = {left, right, middle}` 是**可选键**：窗口装配有 `WindowManager` 时来自
`InputDevice` 的唯一三键电平（`WindowManager::mouseButtons()`）；没有源（headless / 测试 sink）⇒
**缺键**（⛔ 不是 `false`）。`--overlay` / `--ui` 与 `--plugin` 的 `render` 拿到同一形状（`ui.d.ts`）。
★ **位置读数没有 `game.pos` / `game.pos_ok`**（目标 API 旧文里的那两个名字）：这一版只有
`game.local_frames()[i].pos`，且**只有 top 帧**有（见下）。⛔ 不要据此以为 `game.pos` 可用。
★ `host` 的目标 API 里 **`reload()` / `time_budget_ms()` 仍未实现**（切片 3 不做；没有这个成员，
⛔ 不是静默 no-op）。
★ `host.profileEnd()` 的 `ok:true` + `error` 非空 = "窗口关了、文本在，但没进剪贴板"；
文本**不回滚**（需求单明文"INVALID 仍复制"；INVALID 的 token 只在 C++ 报告文本里，JS 侧不另立判据）。
★ `game.local_frames()` 是**方法**（不是 getter）：返回活动帧数组（top-first；字段
`slot / script_id / parent / script / name_degraded / top / pos`），不可用 ⇒ `undefined`；
`pos`（= `0x3C + (pc − base)`，与 `await position` 同一公式）**只有 `top` 帧有**（非 top 是挂起时
保存的续延 PC，⛔ 不是执行点 ⇒ `undefined`）；证据与安全读取见 `docs/03-emulator/ui-frames.md`。

`global/local` 的实现边界（读之前先检查前置条件）：

- 索引 → 地址 = `base + idx*4`（int / float / ptr）或 `base + idx*28`（string）；下标不过编解码
  （`.text:0042AF2D`）。
- **int 值在内存里是编码位模式**：`DEC(x,key) = ror32(rol32(x,11) ^ key, 25)`，`key = Engine+0x5EC8C`
  （`.text:0040D500..0040D521`；`ENC` 在 `.text:004103B2..004103D5`）。float 不过编解码。
  `Engine+0x5EC90 = ENC(key,0)`，文档口径"非 0" ⇒ 先查非 0；否则 `unavailable`，⛔ 不拿 0 当 key 硬解。
- `ptr` 池不暴露（会把宿主 EA 泄漏给脚本）；`string` 过 CP932→UTF-8（`runtime/cp932.h`）。

### 3.6 每帧管线（顺序固定 ⇒ 确定性）

```
Present → beginFrame(geometry, frameState) → 投递输入（按 seq）→ update(dt, frame)
        → render(ui)（唯一绘制入口）→ validate → resolve(when / layout)
        → 有可见元素才 rasterize → presentOverlay(rgba, bbox)
```

命中与绘制用**同一份** resolved list；窗口在两次 Present 之间 resize 时，命中按旧几何、下一帧自动纠正（写进 API 文档，不假装实时）。

## 4. 里程碑与可红判据

> 每步都能在**没有脚本运行时 / 没有游戏**的情况下判红；资产档用例单独标注。

### M0 — 接线与统一合成（无脚本、无输入）

新开 AOT 臂的 `--overlay` CLI 面（`kWindowFlagsAot` 加它）；`PresentSink` 加
`presentOverlay(rgba,w,h,x,y)` 与 `presentGeometry`；两条路径都改成"窗口像素 + origin"；HUD 内容用
**最终模型子集**的 C++ display list（`src/ui/element.h` 的 POD + `UiBuilder` + 只做绝对 `at`/`size` 的
最小 resolve；走同一条通用合成路径，⛔ 不是第二套渲染器，⛔ 不是一次性 `vector<rect/text>`）。
M1 只加能力（layout / when / hit / clip），不改模型（N1）。

**B1（SDL_Renderer 路径的 1:1 合成，已裁决）**：画叠层的那一次 draw 前后临时
`SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED)`，画完恢复
`(帧 w, 帧 h, SDL_LOGICAL_PRESENTATION_LETTERBOX)`；⛔ 不做逆缩放。若实测有副作用（present 读数 / 状态），
**显式**把判据 2 降为"原点 ±1 px + 采样中心色"并写进 `observations.md`，⛔ 不许静默降级。

**B2（HUD 的参考系，已裁决）**：M0 HUD 走 D5 的 1280×720 参考系；判据 2 的"原点"是**画布原点**
（= 画面矩形原点 + `((pic_w−1280s)/2,(pic_h−720s)/2)`，`s = min(pic_w/1280, pic_h/720)`），
⛔ 不是画面矩形原点（非 16:9 时两者差居中偏移）。`uiCanvasOrigin(pic_w,pic_h) -> (s,ox,oy)` 是纯函数；
用例：16:9（如 640×360 ⇒ offset 0）+ 非 16:9（如 320×240 ⇒ `(0,30)`）各一条。

**U1（文本字形，已裁决）**：生产 HUD / UI 文本走**宿主字体**（`runtime/font.h:FontEngine` / FreeType，
`LIFT_HOST_FONT_PATH`），按**最终像素**重绘（D5），**必须支持中文 / 日文**；⛔ 不用旧 5×7 点阵当生产字形
（旧点阵只作 M2 的 `--ui-glyphs bitmap` 显式测试档）。M0 判据：中文串（如 `天結`）经宿主字体栅格出
**非空覆盖率**；字体缺失 ⇒ **启动硬错**（R5），⛔ 不静默退回点阵、⛔ 不画空白。

1. `--present window --present-hidden --overlay --max-frames 2` ⇒ `exit 0`（AOT 臂第一次有叠层）。
2. 截图：叠层 bbox 原点 == **画布原点**（见 B2），颜色精确值命中（沿用 `tests/overlay.cpp` 的判据形状）；
   16:9 与非 16:9 各一条（非 16:9 断言居中偏移）。
2b. 文本：宿主字体栅格（⛔ 点阵生产路径）；中文串（如 `天結`）经宿主字体得到**非空覆盖率**；
   字体缺失 ⇒ 启动硬错（R5）。
3. 关掉 `--overlay` ⇒ 窗口像素逐位等于"没有 UI"。
4. `--dump-frame` 逐字节不变、`arena.hash` 不变（对照用 **1 帧**；`--max-frames 2` 的第二帧随真实时钟变化，
   两轮**纯对照**也不同 ⇒ 2 帧不能当逐字节判据）。
5. 默认档整轮 stderr 无 `[ui]` / `[overlay]` 周期行。
6. 两条呈现路径都过；直出无 GPU ⇒ SKIP 77（⛔ 拿 renderer 假绿）。

★ 旧的"真窗口 + 真落盘"覆盖已删 ⇒ M0 重新立了"带 `--overlay` 的 AOT 子进程跑两帧 + 窗口 BMP 在
画布原点出现精确色块 + `--dump-frame` 两次逐字节相同"（`tests/aot_overlay.cpp`；缺资产 ⇒ SKIP 77），
⛔ 不靠 `tests/overlay.cpp` 的窗口用例冒充。
★ **`aot-overlay` 的测试脚手架已跨平台**：Windows 用 `CreateProcessW`，mac / Linux 用 `fork` + 子进程
`open`/`dup2`/`execv` + 父进程 `waitpid(WNOHANG)` 轮询 + 60 s `std::chrono` 超时（stdout/stderr 都重定向到
文件，⛔ 不拼 shell 字符串 / ⛔ 不用 `system("timeout …")`）⇒ **mac / Linux 也真跑**。只有真正的环境缺口
（GPU / 生成物 / 执行源 / 安装树 / 宿主字体）才 `LIFT_SKIP 77`；判据 1 / 4 / 5 的端到端证据不再只在
win32 取。

### M1 — `ui_core` + display list + 布局 / 条件 / 命中（无语言运行时）

用一个测试内的 C++ 假脚本（实现 `UiScript`，直接往 builder 塞元素）驱动整条管线；**M1 不开 `--ui` 文件入口**
（文件格式与语言一起定，⛔ 先造临时 JSON 再废弃）。

1. 校验表每类一条；错误文本带 `行:列`（校验器接收"来源 + 行"值对象，不依赖文件）。
2. 布局：绝对 / 锚点 / 百分比 / auto / 嵌套 clip 各一条；非 16:9 的等比 + 居中一条。
3. `when`：script 匹配（大小写 / `.bin` / id 退化）、pos 半开边界（`from` 含、`to` 不含）、
   all/any/not、不可读 ⇒ false（共 6 条）。
4. 命中：z 序、clip、`pass_through`、边界像素、捕获状态机。
5. 光栅：哨兵"只写 bbox"；bbox == 可见元素并集；不透明底 / 颜色精确值。
6. 确定性：同一 list + 同一 state 两次 resolve/raster 逐字节相同。

### M2 — QuickJS 宿主 + HUD 迁脚本 + 点击路由

★ **M2a 已落地（2026-11-18；M2a 本切片只做 QuickJS 宿主 + HUD 迁脚本；命中 / 捕获 / 点击路由在切片 2 落地，见下）**：
`--overlay` 的 HUD 由**外置** `scripts/ui/hud.js` 经 QuickJS 同步产出；`--ui <path>` 打开任意脚本
（显式优先、隐含 overlay）；`game` 注入 `frame / fps / mouse{x,y,buttons?} / script / worst` + 按需方法
`local_frames()`（帧区 40 槽的只读视图，top-first，`top = slot == cur`；证据
`docs/03-emulator/ui-frames.md`；HUD 的 `script` 行取 `top=true` 的项（没有 top 就回退 `game.script`））；`raster` / `present` 两行**已删**；底 `#101820CC`（80% 不透明，
renderer 路径 `SDL_BLENDMODE_BLEND`，直出也混合（D 单，`SDL_DrawGPUPrimitives`；win32 未验证））；旧 `OverlayState` / `OverlayStatus` /
`formatOverlayLines` / 5×7 点阵 / `buildHud` / `presentHud` 已删。M1 的布局 / when / 校验表、
M3 的数据面仍未做。`mouse.buttons` 的源与缺键口径见 §3.5（窗口装配有 `WindowManager` 才有）。

★ **M2a 之后已落地的切片**（2026-11-19）：切片 2 = 命中 / 捕获 / `game.click|hover|active` +
外置组件库 `kit.js`（`ui.rect({key})` 的 resolved rect 是命中区）；切片 3 = `host.profileBegin/End`
（结束在宿主内部写系统剪贴板）+ `hud.js` 改 module 并用 `kit.Button` 画 PROFILE + 帧列表 `pos`
（只有 top 帧；`0x3C + (pc − base)`）。

落 `overlay-quickjs.md` 的适配器；HUD 变成 `scripts/ui/hud.js`；display list 成为脚本唯一出口。
★ **S9 已落**：`UiClickRouter` 已接进 `present_sdl.cpp:Impl::handleEvent` 与 `input_plan.cpp` 的
`click` 路径；`--ui-input=consume|pass` 已实现（缺省 consume；窗口参数）。`--ui-strict` 仍未做。

1. HUD 脚本产出的像素与 M0 内置 HUD **结构等价**（行数 / 位置 / 颜色；允许字形不同）。
2. OS 点击命中 ⇒ guest 的 `WM_LBUTTONDOWN` **为零**、`WindowManager` 按钮电平不变（`tests/input_injection.cpp` 夹具）。
3. 未命中 ⇒ 与没有 UI 时逐位相同的消息序列。
4. 捕获：down 命中、up 在面板外 ⇒ up 仍被吞（guest 没有孤儿 up）。
5. 注入点击（**已实现，S9**）：`consume`（缺省）命中 ⇒ 配方 `fired` 照旧 +1、
   `InputPlan::uiConsumed()` +1、guest 消息为零、按钮电平与光标都不动；`pass` ⇒ 逐位与没有 UI
   相同。★ 摘要在**非 0** 时追加 `ui_consumed=N`（`InputPlan` 摘要字段）；控制层回执的
   `consumed_by_ui` **未做**（M3）。
6. 沙箱：访问文件 / 网络 / 墙钟 / 随机 ⇒ 运行时错误；`while(true){}` ⇒ 被中断 + 一次响亮；
   内存分配超限 ⇒ 被拒绝 + 卸载。
7. 语法错 ⇒ `exit 2` 带 `file:line`；热重载失败 ⇒ 旧实例继续画 + 一行错误。
8. **读数（不是门槛）**：10000 次调用 + 每帧 1 次 `render` 的墙钟，release / 同机 / 交错 ≥3 对；
   读数进 `observations.md`，阈值由读数定。

### M3 — `script/pos` 条件 + 数据查询 + 控制层面

AOT 块入口采样门；`game.script/pos` + `when` 的端到端；`global/local` 只读；控制层
`ui_list` / `ui_reload` / `ui_unload`；`click.consumed_by_ui`。

1. 假 Engine 夹具（复用 `tests/input_marker.cpp` 的 `commitZeroed` + 记录布局）：`when` 在 `pos`
   边界两侧各一条、script 改名一条、`cur` 换槽一条、不可读一条。
2. AOT 采样门：`eip ∈ {0x4123F0, 0x4123E5}` 才采样（静态形态 + 运行计数两层）。
3. `global/local`：int 的 DEC（已知 key 的合成值）、越界 `undefined`、key 未初始化 ⇒ `unavailable`。
4. 控制层：`ui_list` 回显插件名 / 元素数 / 上帧 repaints；`ui_reload` 成功 / 失败两条；
   `ui_unload` 后点击全穿透。
5. 真 boot（assets 档，可选）：不崩 + 无周期输出（⛔ 不断言业务正确性）。

### M4 — 图片 / 锚点进阶 / 热重载收尾

1. 图片：PNG 解码 + `stretch/contain/none`；缺图 / 坏图 `exit 2`；大图按上限拒绝。
2. 热重载：改文件 ⇒ 新实例生效（状态重置）且无泄漏（旧纹理 / 旧图像缓存释放）。
3. 形态守卫：`rg 'formatOverlayLines|OverlayState' apps/emulator-lift/src` 为空；
   `--overlay` 只出现在参数映射与文档别名表里。

## 5. 测试计划

| 档 | 用例（建议） | 覆盖 |
|---|---|---|
| pure（顶层 CTest） | `tests/ui_spec.cpp` · `ui_layout.cpp` · `ui_when.cpp` · `ui_hit.cpp` · `ui_raster.cpp` · `ui_route.cpp` · `ui_script_quickjs.cpp` · `ui_seq_order.cpp` · `ui_marker_gate.cpp` | 校验表 / 布局 / 条件 / 命中 / 只写 bbox / 同一 router / 沙箱与预算 / 确定性排序 / 采样门形态 |
| window / GPU | `tests/overlay.cpp`（迁移）+ `tests/present_window.cpp` | 两条路径的原点 / 尺寸 / 颜色；合成失败 ⇒ 呈现照旧 |
| assets | AOT 子进程 + `--max-frames 2` + `--present-screenshot` | 端到端"叠层真的上屏"、`--dump-frame` 不含 UI、无周期行 |
| 读数 | `lift-ui-script-bench`（独立 exe 或 `--bench` 档） | 墙钟 / 内存；进 `observations.md`，⛔ 不作门槛 |

★ **M0 已落地的用例名**（上表是建议；实际文件与 CTest 名）：`tests/ui_canvas.cpp`（`ui-canvas`，纯函数
`uiCanvasOrigin`）· `tests/ui_text.cpp`（`ui-text`，宿主字体中文覆盖率 + 字体缺失硬失败）·
`tests/overlay.cpp`（`overlay` / `overlay-gpu`，两条路径的画布原点 / 颜色 / 关掉后逐像素）·
`tests/present_window.cpp`（`present-window` 走 software、16:9 的 1:1 原点 / 颜色；`-gpu` 变体在
直出成立时才断言，否则 SKIP 77 —— 直出面的主证据是 `overlay-gpu`）·
`tests/aot_overlay.cpp`（`aot-overlay`，AOT 子进程 + 窗口截图 + `--dump-frame` 对照）。
★ **M2a 落地的用例**：`tests/ui_script_quickjs.cpp`（`ui-script-quickjs`，沙箱 / 预算 / api / readings / error / entry）·
`tests/ui_hud_script.cpp`（`ui-hud-script`，外置 `hud.js` 的行序 / 颜色 / 80% alpha / 无 raster·present）·
`tests/ui_frames.cpp`（`ui-frames`，帧列表活动判据 / top / 降级 / `game.local_frames()`）。

**变异自检**（每类一条代表，`tools/mutate-check.mjs`）：`when` 的 `pos` 比较 `>=`→`>` ⇒ `ui_when` 红 ·
router 的"down 放 ⇒ up 放"改掉 ⇒ `ui_hit` 红 · 采样门少一个 EA ⇒ `ui_marker_gate` 红 ·
`uiCanvasOrigin` 丢掉居中偏移 ⇒ `ui-canvas` 红（M0 已加进清单）· `seq` 排序改成按 id ⇒ `ui_seq_order` 红。

## 6. 风险

| # | 风险 | 处置 |
|---|---|---|
| R1 | 运行时不能中断 ⇒ 脚本死循环卡住模拟器 | `JS_SetInterruptHandler` 返非 0 ⇒ 不可 catch 的中断（`overlay-quickjs.md` §2） |
| R2 | 热重载失败策略与"错误必须停"的张力 | 启动期错误致命；运行期 / 热重载 = 卸载**新**实例、旧实例继续，并打 `FAIL`；`--ui-strict` 一刀切致命 |
| R3 | 吞点击改变 guest 输入 ⇒ 配方可复现性 | 默认档不带 UI；**`--ui-input=pass` 已实现**（注入 click 逐位与没有 UI 相同）；摘要 `ui_consumed=N`（仅非 0 时出现）；控制层回执的 `consumed_by_ui` 仍待做；文档写死 |
| R4 | AOT 每块新增 2 次比较 + 两个 EA 的采样 | 只在有插件时开；静态指令预算复核；读数进 `observations.md` |
| R5 | 字体依赖（`LIFT_HOST_FONT_PATH` 不在 ⇒ 启动硬错；C2b-2 起也覆盖插件 `render` 的 adopt） | `--ui-glyphs bitmap` 显式测试档；生产默认仍是字体，⛔ 不静默降级；adopt 失败（字体缺失 / 窗口没建）⇒ `exit 2` |
| R6 | "guest 客户区 == 帧尺寸"是既有假设；`--scale` 车道最终形状未知 | 先按相等验收；不等 / 变了记 observations，⛔ 不猜；`--scale` 只在 `letterboxFor` 之上接 |
| R7 | 插件绕过 API 改 guest | API 无写入口 + 形态守卫（`src/ui/**` 不出现 guest 写 API）+ 逐位断言 |
| R8 | QuickJS 内存 / 沙箱 / GC 与引用计数 / 静态链体积 / TS 产物（`.js` 与 `.d.ts` 漂移） | 见 `overlay-quickjs.md` §2、§5；体积是**估**，⛔ 不进判据；`.d.ts` 漂移守卫**未做**（M2a 只有声明文件） |
| R9 | **80% 半透明在 GPU 直出上不生效**：`SDL_BlitGPUTexture` 是拷贝、不混合 | 已解决（D，2026-11-18）：直出改为专用混合绘制管线（`SDL_DrawGPUPrimitives` + `SRC_ALPHA/ONE_MINUS_SRC_ALPHA`），`directReadback` 同公式；win32 未验证 |
| R10 | 外置 `scripts/ui/hud.js`（及其 module 依赖 `kit.js`）不在发行树 ⇒ `--overlay` 启动硬错 | 发行打包把 `scripts/ui/{hud.js,kit.js,ui.d.ts,README.md}` 一起带上（`release/` 待补）；**`hud.js` 与 `kit.js` 必须同目录**（`import './kit.js'` 按 `hud.js` 所在目录解析；缺 kit 与缺 hud 同属 exit 2）；开发回退 `LIFT_UI_SCRIPT_DIR` |

## 待核

1. `--scale` 车道的最终接口：改窗口、改帧、还是改参考系？本设计只兼容前两种。
2. ~~AOT 的 `0x4123F0` / `0x4123E5` 是否真出现在派发边界~~ **已证（2026-11-18）**：两个 EA **不在**派发表
   （`out/aot/lift_table.inc` 零命中；`lift_cc_4122E0` 的最后一个 case 是 `0x004123E3`），但在**闭包**
   （`aot/closure.txt:131` 的分量根 `0x00412290`）与**已生成 IR**（`bb_4123F0` / `bb_4123E5`）里。
   它们是分量函数体**内部**的基本块，只能由内部 `br` 进入 ⇒ M3 的采样门要**在发射 IR 时于这两个 bb 入口
   插钩子**，⛔ 不挂派发边界。
3. `LOCAL_POOL_SLOTS` 的 `count` 单位 **已闭合**：它是**记录内"计数槽"的字节偏移**（`0x08+4i`），
   不是元素个数、也不是字节数（review N11；佐证 `packages/age-format/src/engine/layout.mts:232-239`）。
   剩下"`global` 池常量要不要进 `engine_marker.h`"（现在在 `packages/age-format`，`Engine+0x5D800` 一族）
   由 M3 决定，非阻塞。
4. ~~`SDL_SetRenderLogicalPresentation(…, DISABLED)` 与 letterbox 的往返是否影响 present 读数 / 有无状态副作用~~
   **仅 identity 情形已实测（2026-11-18 / macOS arm64 / M0）**：renderer 用例（`tests/overlay.cpp` 的
   `overlay/renderer-window`）是**窗口 == 帧（640×360）**，逻辑呈现恒 identity ⇒ 它只钉住了
   "画布原点 / 颜色 / 矩形外 guest 色"的读数；**DISABLED↔LETTERBOX 的往返本身在非 identity 窗口上未证**
   —— 实测把整句 DISABLED 删掉的变异 **4/4 全绿**（`overlay` / `overlay-gpu` / `present-window` /
   `present-window-gpu`），即现有用例**对往返没有区分力**。⇒ 副作用面**未证**，⛔ 不要读成"往返无副作用"。
   M1 补"**强制 1000×1000 窗口**（窗口 ≠ 帧）+ 连提两帧"用例后再判；在那之前判据 2 仍是 identity 精确值。
   ★ 换 SDL 版本后要重测这一条（它依赖 SDL 内部 viewport 的往返行为）。
5. 文字编码：脚本是 UTF-8、字体 `cmap` 是 CP932 码位（`fonts.md`）—— 中文码位直查是否成立，不成立要过 `cp932Encode`。
   ★ M0 已落"宿主字体按 UTF-8 码位取字"（`ui/raster.cpp` + `tests/ui_text.cpp` 的 `天結` 非空覆盖率），
   但**游戏脚本文本**的 CP932→UTF-8 转换仍在 `runtime/cp932.h`（M3 接数据面时再核这一条）。
6. 控制层协议增量（`ui_*` / `consumed_by_ui`）的 `capabilities` 与客户端兼容。
7. HUD 的 `worst` 行在 AOT 臂：补"每帧 wall"宿主采样，还是整行省略（缺省取后者）；元素数 / 文本长度 /
   图片像素上限先给起点，读数用例之后定。

## 取证命令（怎么再核一遍）

```powershell
rg -n 'OverlayState|formatOverlayLines|kOverlayGlyphW' apps/emulator-lift/src/runtime/overlay.h
rg -n 'kWindowFlagsInterpreter|kWindowFlagsAot' apps/emulator-lift/src/runtime/present_flags.h
git log --oneline -1 -- apps/emulator-lift/src/main.cpp      # 解释器 main 已删 ⇒ 生产不可达
rg -n 'drawOverlay|presentOverlay|directOverlay' apps/emulator-lift/src/runtime/present_sdl.cpp
rg -n 'blitOverlay|不随画面缩放' apps/emulator-lift/src/runtime/d3d9_gpu.cpp
rg -i -n 'overlay|hud' apps/emulator/src                   # 只应命中 VFS 注释（没有宿主叠层）
rg -n 'KIND_TEXTURE_OVERLAY|KIND_MOVIE_OVERLAY' apps/emulator-new/src/platform/electron/renderer/scene-command-plan.ts
rg -n 'kMarkerLoopHeadEa|kMarkerPumpReturnEa|readScriptPositionSample' apps/emulator-lift/src/runtime/engine_marker.h
```
