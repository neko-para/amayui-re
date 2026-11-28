// 合同 — plugin API 合同；后续变更需显式修订版本。
// 模块 specifier: "@amayui/lift-plugin"。
// 插件文件 = ES module；`export default { render?, start?, onTrigger?, initialState? } satisfies PluginModule<State>`。
// 宿主 QJS 装载后读 default 导出；不再读 globalThis.render/start/onTrigger。
//
// ★ v2（S2c/S3，2026-11-28；P0 补 `once` 全形态 / `delay` / Run 侧门；P1-fix 补 `waitAny`）：
//   新插件写 `start(ctx)` 装 `ctx.at` / `ctx.once` / `ctx.delay` / `ctx.task` / `ctx.waitAny`，
//   `start` 返回 `void`（不再返回 Trigger 集合）；指令点回调 / `once` 的 resolve 给一个 `Tick`，
//   写面在 Tick 上（`now` / `local` / `edit` / `input` / `patch` / `log`）。
//   `ctx.at` / `ctx.once` / `ctx.delay` / `ctx.task` 的门 = **Run 侧入口**（Run 回调 / task
//   continuation / 泵）；**render 里仍 TypeError**。tick handler 里也能用它们（没有 RunCtx 参数，
//   但存到模块变量再用 = 按 Run 侧允许）；tick 的**写面**仍是 `Tick`，v1 `ctx.edit/input/trigger/hook.*`
//   在 tick 里仍 TypeError。
//   v1 面（`ctx.trigger` / `ctx.hook.at` / `ctx.now` / `handle.onTick` / `onTrigger`）仍可用，但都是
//   **deprecated 兼容层**（见各成员注释）；examples 已全部按 v2 写（`scripts/plugins/examples/`）。
//
// 核心语义（先读这 8 条；第 4 条是 v1）：
// 1. Edit（ctx.edit）在触发点/安全点立即同步写，不排队；暂停时也可执行。
//     v2 在 Tick 上：tick.edit 是**同一个宿主方法**（立即持久写）。
// 2. Input（ctx.input）只入队，在 guest 读点按“一读点最多一条”串行施加；click 只保证 queued。
//     v2 在 Tick 上：tick.input 是**同一个宿主方法**（入队，下一读点落地）。
// 3. EngineRead（ctx.read）同步只读、不暂停、便宜；sample/screenshot 不在这里。
// 4. Trigger（ctx.trigger）具名；start/onTrigger 返回 Trigger|Trigger[]|null；数组=完整替换 armed 集合；
//    一个读点最多触发一个；null/[]=done；重复 name=fail。**deprecated（v1）**：v2 用 ctx.at / ctx.once。
// 5. UI（ctx.ui）只改宿主本地状态；onTrigger 不画，render 用 ctx.draw 画；下一次 render 才可见。
//     v2 分层：tick handler **不碰 UI**（只改插件模块数据），render(rctx) 读数据 + rctx.draw / rctx.ui。
// 6. render 也可写 ctx.ui（hover/动画），但只影响 UI 状态；render 不做 guest 写。
// 7. Run（ctx.run）是一次 callback 链的执行实例；一个 endpoint 同时一个 active，已有 ⇒ busy。
//    ★ v2：**不允许嵌套 Run**；组合用 ctx.task，切换 Run 由控制层做（dispatch 不在插件 API 里）。
// 8. ctx.finish 结束当前 Run（可映射旧 exit 5）；ctx.quit 请求宿主停机（WM_QUIT/stop_requested）。
//    ★ P0 起门 = **Run 侧入口**（Run 回调 / task continuation / 泵）；render 里仍 TypeError。
declare module "@amayui/lift-plugin" {
export type GlobalKind = "int" | "uint" | "float" | "string";
export type GlobalValue<K extends GlobalKind = GlobalKind> =
    K extends "string" ? string : number;

/** Edit/Input 的即时回执；Edit 立即 applied，Input 只有 queued。 */
export interface Receipt {
    readonly accepted: boolean;
    readonly queued: boolean;
    readonly applied: boolean;
    readonly code?: string;   // "id_out_of_range" | "type_mismatch" | "encode_failed" | "busy" ...
    readonly seq?: number;
}

/** Input 回执带 id；后续用 ctx.input.status(id) 查完成。 */
export interface InputReceipt extends Receipt {
    readonly id: number;
}
export type InputState = "queued" | "applied" | "failed" | "unknown";
export type MouseButton = "left" | "right" | "middle";

/**
 * 坐标统一为 1280×720 参考系；宿主按窗口缩放。
 *
 * ★ `"fileRead"` 是 `kernel32!ReadFile` 的**事件源**（`InputPoint::FileRead`）：它不算"输入族"，
 *   但它是宿主读点集合里的一个真实成员 ⇒ `ctx.now.point` / `ev.point` **会**出现这个值
 *   （C2a 复审第二轮 F5-R1：并集缺它会让 `switch (ev.point)` 在 strict 下穷尽性检查误报）。
 * ★ `"any"` = `InputPoint::Count`（只给 `atReadPoint` 用；`ctx.now.point` 不会是 "any"）。
 */
export type InputPoint =
    | "any" | "keyLevel" | "cursorPos" | "peekMsg" | "getMsg" | "msgWait"
    | "waitMessage" | "queueStatus" | "translate" | "dispatch" | "setCursorPos"
    | "fileRead";
export interface ScriptPos {
    readonly script: string;
    /** ★ 只有宿主**真的读到**位置才有这个键（`valid:false` ⇒ 缺键；⛔ 不编 0）。
     *  C2b 实现 = `RenderCtx.read.scriptPos()`（top 帧 + `pos_ok`）。 */
    readonly pos?: number;
    readonly valid: boolean;   // false = marker 暂时不可读；不要当成 0
}
export interface FrameInfo {
    readonly index: number;
    readonly script: string;
    /** `slot == cur`（当前执行点）—— 与经典 `game.local_frames()[].top` **同一个布尔**。
     *  ⛔ 不是"列表第一个"：`cur` 那一格不活动时整表 `top` 全 false（`pos` 也因此全缺）。 */
    readonly top: boolean;
    /** ★ 只有 **top 帧**（有执行点）才有这个键；非 top 帧缺键（⛔ 不拿 0 冒充）。
     *  `hash` / `presented` 在 runtime 里没有源 ⇒ 实现**不安装**（C4 再接）。 */
    readonly pos?: number;
    readonly hash?: number;
    readonly presented?: boolean;
}

/** EngineRead：同步、无暂停、便宜；global.get/frames/scriptPos/frameHash。
 *
 * ★ 切片状态（C2a/C2b 实现面，2026-11-20）：
 *   - `global.get`：**C2a 已实现**（`ctx.read.global.get`；RenderCtx.read 里同一份实现）；
 *   - `frames` / `scriptPos`：**C2b 只装在 `RenderCtx.read` 上**（来路 = 当帧 `readings.frames`
 *     的 top 帧）。⚠ `RunCtx.read` 里这两个属性**仍是 `undefined`**（位置读数在 Run 语境走
 *     `ctx.now.pos` / `ev.pos`；⛔ 不是"有但抛"）。
 *   - `frameHash`：**留 C4**（runtime 里没有帧 hash 源；唯一实现在宿主的 `Presenter::lastHash`）。
 *     ⚠ 实现的 `read` 对象**不安装**这个属性 ⇒ 调用是 `TypeError`（⛔ 不是"返回 0"；D-C2-6）。
 */
export interface ReadApi {
    readonly global: {
        get<K extends GlobalKind>(kind: K, id: number): GlobalValue<K>;
    };
    /** C2b（只在 `RenderCtx.read` 上；没有源 / 宿主报错 / 空表 ⇒ `undefined`，⛔ 不编空数组）。 */
    frames(): readonly FrameInfo[] | undefined;
    /** C2b（只在 `RenderCtx.read` 上；没有源 / 没有 top 帧 ⇒ `undefined`）。 */
    scriptPos(): ScriptPos | undefined;
    /** 留 C4（**不安装**这个属性 ⇒ 调用 TypeError；⛔ 不返回 0）。 */
    frameHash(): number;
}

/** Edit：立即同步；暂停/控制点也可；不排进 Input 队列。 */
export interface EditApi {
    readonly global: {
        set<K extends GlobalKind>(kind: K, id: number, value: GlobalValue<K>): Receipt;
    };
}

/** Input：串行、下一读点/按预算；一读点最多一条 guest 写。 */
export interface InputApi {
    click(x: number, y: number, button?: MouseButton): InputReceipt;
    move(x: number, y: number): InputReceipt;
    down(button: MouseButton): InputReceipt;
    up(button: MouseButton): InputReceipt;
    /** 查这条 Input 是否已被 guest 消费；queued 不等于完成。 */
    status(id: number): InputState;
}
export type TriggerMode = "once" | "reenter";
/** name 是插件侧状态名（不是 guest 数据）；纯 string，拼错/重复只在运行期暴露，用 ctx.fail 报。 */
/**
 * Timeout（写死）：
 * - afterScript/afterPosition/atReadPoint 可配 `timeoutMs`；afterDelay 的 `ms` 是延迟本身，
 *   **不接受/不叠加 timeoutMs**（用 DelayOptions；ms 到达就是延迟，不存在额外 timeout）。
 * - arm 时记 deadline（宿主墙钟）；TriggerRunner 在**每个 guest 读点的 atPoint（采样后、releaseDue 前）** poll；
 *   elapsed >= timeoutMs ⇒ 从 armed 集合移除 + onTrigger(TimeoutEvent{kind:"timeout",name,trigger})。
 * - **不自动 finish**；插件用 ctx.finish(5, reason)（旧 exit5）或返回新集合 re-arm。
 * - per-arm：重新 arm 重置 deadline；maxFires 继续累计。
 * - 暂停/headless/无读点 ⇒ 不 poll、不会超时（不是独立 wall-clock timer；恢复后到下一个读点才可能触发）。
 * - TimeoutEvent.name/trigger 必填；trigger 是超时的那个对象。
 */
export interface TriggerOptions {
    readonly name: string;   // ★ 纯 string 状态名；同一 armed 集合内唯一（运行期查重/拼错）
    mode?: TriggerMode;      // 默认 "once"
    timeoutMs?: number;      // 0/省略=不设；elapsed >= timeout 只发 timeout 事件；失败用 ctx.finish(5,reason)
    maxFires?: number;       // 属于对象、跨 arm 累计；耗尽后再返回同一对象=fail；新对象才新预算
}

/** 只比当前脚本名（电平门；无区间）；等价旧 `await script NAME`。
 *  ★ `script` 的值 = **marker 文件名**（例 `TITLE.BIN`，含后缀；⛔ 不是裸名 `TITLE`）。
 *    C1 的匹配是**精确相等**（大小写/.BIN 都不归一化；`await script` 那层才剥 `.BIN`）。 */
export interface AfterScriptTrigger {
    readonly name: string;   // 状态名
    readonly kind: "afterScript";
    readonly script: string;  // marker 文件名（例 `TITLE.BIN`）
    readonly mode: TriggerMode;
    readonly timeoutMs?: number;
    readonly maxFires?: number;
}
export interface AfterPositionTrigger {
    readonly name: string;
    readonly kind: "afterPosition";
    readonly script: string;  // marker 文件名（例 `SBUNKI.BIN`；精确相等）
    readonly lo: number;
    readonly hi: number;
    readonly mode: TriggerMode;
    readonly timeoutMs?: number;
    readonly maxFires?: number;
}
export interface AfterDelayTrigger {
    readonly name: string;
    readonly kind: "afterDelay";
    readonly delayMs: number;   // 宿主墙钟；NON-DIFFABLE；ms 是延迟本身，不叠 timeout
    readonly mode: TriggerMode;
    readonly maxFires?: number;
}
export interface AtReadPointTrigger {
    readonly name: string;
    readonly kind: "atReadPoint";
    readonly point: InputPoint;
    readonly maxFires: number;  // > 0
    readonly mode: TriggerMode;
    readonly timeoutMs?: number;
}
export type Trigger = AfterScriptTrigger | AfterPositionTrigger | AfterDelayTrigger | AtReadPointTrigger;
/** afterDelay 专用：没有 timeoutMs（ms 是延迟本身，不叠加）。 */
export interface DelayOptions {
    readonly name: string;
    mode?: TriggerMode;
    maxFires?: number;
}
export interface TriggerEventBase {
    /** ★ **marker 文件名**（例 `TITLE.BIN`，含后缀；⛔ 不是裸名）。`loadSave` 的链用 `.BIN`。 */
    readonly script?: string;
    readonly pos?: number;
    readonly posValid?: boolean;
    readonly point?: InputPoint;
    readonly readIndex: number;   // 第几个读点；用于 trace/去重
}
/** 只用于 trace/类型完整性；onTrigger 不会收到 StartEvent。 */
export interface StartEvent {
    readonly kind: "start";
}
export interface FiredEvent extends TriggerEventBase {
    readonly kind: "trigger";
    readonly name: string;
    readonly trigger: Trigger;
}
/** 超时事件：不是 fire，不计 maxFires；name/trigger 必填，trigger 是超时的那个对象。 */
export interface TimeoutEvent extends TriggerEventBase {
    readonly kind: "timeout";
    readonly name: string;
    readonly trigger: Trigger;
}
export type TriggerEvent = StartEvent | FiredEvent | TimeoutEvent;

export interface TriggerApi {
    /** 纯脚本名电平门；Once=首次匹配；Reenter=每次 false→true；无效位置不改变状态。 */
    afterScript(script: string, opts: TriggerOptions): AfterScriptTrigger;
    /** 半开 [lo,hi)；Once=首次有效 inside；Reenter=有效外→内（不是每读点）。 */
    afterPosition(script: string, lo: number, hi: number, opts: TriggerOptions): AfterPositionTrigger;
    /** 宿主墙钟延迟；暂停不冻结；NON-DIFFABLE；wait 就是链上的一个具名 afterDelay。 */
    afterDelay(ms: number, opts: DelayOptions): AfterDelayTrigger;
    /** 下一个匹配读点；maxFires 必填 >0。 */
    atReadPoint(point: InputPoint, maxFires: number, opts: TriggerOptions): AtReadPointTrigger;
}

/** UI 状态是宿主本地：不进 guest/存档/输入；onTrigger 只改状态，render 才画。 */
export interface UiState {
    readonly [id: string]: unknown;
}
export interface UiApi {
    show(id: string, data?: unknown): void;
    hide(id: string): void;
    set(id: string, data: unknown): void;
    visible(id: string): boolean;
    /** T 是调用方断言，不做运行时校验：ctx.ui.get<HudState>("hud")。 */
    get<T = unknown>(id: string): T | undefined;
    readonly state: UiState;
}
export interface DrawStyle {
    readonly fill?: string;
    readonly stroke?: string;
    readonly width?: number;
    readonly alpha?: number;
    /**
     * ★ S2：命中键（**只对 `draw.rect` 有意义**）。非空 ⇒ 这条 Rect 是命中区（resolved rect）；
     *   `text` / `measure` 上传它 ⇒ TypeError。缺省 = 不可命中。
     */
    readonly key?: string;
}
export interface TextStyle extends DrawStyle {
    readonly size?: number;
    readonly align?: "left" | "center" | "right";
    /** ★ S2：粗体（缺省 false；与经典 `ui.text({bold})` 同一条渲染路径）。 */
    readonly bold?: boolean;
}

/**
 * 只在 render 里用；发本帧 display list。
 * - 坐标 = 1280×720 逻辑系；宿主按窗口缩放。
 * - z 序 = 调用顺序（后调用在上）。
 * - 每帧开始清空；render 返回后提交本次 list。
 * - text.style.size/align/fill 同 draw；measure 用同一套 style 估算（`size` + **`bold`**；`fill` 不影响度量）。
 */
export interface DrawApi {
    rect(x: number, y: number, w: number, h: number, style?: DrawStyle): void;
    text(x: number, y: number, text: string, style?: TextStyle): void;
    /** `bold` 会**真的**改宽度 / 高度（与 `draw.text` 同一条字体路径；⛔ 不静默按非粗体量）。 */
    measure(text: string, style?: TextStyle): { readonly width: number; readonly height: number };
}
export type RunState = "running" | "waiting" | "done" | "failed" | "aborted";
export interface RunHandle { readonly id: number; }
export interface RunStatus {
    readonly handle: RunHandle;
    readonly plugin: string;
    readonly state: RunState;
    readonly armedNames: readonly string[];  // 当前 armed 集合的 name（注册顺序）
    readonly armed: readonly Trigger[];      // 与 armedNames 一一对应；必填
    readonly lastError?: string;
    readonly owner: number;   // Run 级 owner：创建者插件 id / 控制 session
}
export type DispatchResult =
    | { readonly ok: true; readonly busy: false; readonly handle: RunHandle }
    | { readonly ok: false; readonly busy: boolean; readonly code: string };
export interface RunEvent {
    readonly seq: number;     // 单调；events(since) 用它去重
    readonly name?: string;   // trigger/start 事件的状态名
    /** "start" 是伪事件（用于 trace/观察），不会作为 onTrigger 参数。 */
    readonly type: "start" | "done" | "failed" | "aborted";
    readonly handle: RunHandle;
    readonly plugin: string;
    readonly error?: string;
}

/** 一个 endpoint 同时一个 active Run；已有 ⇒ dispatch busy；abort 只允许 owner/控制层。 */
export interface RunApi {
    /** 同步：返回时 start 已跑完，armedNames 已就绪（除非 start 返回 []/null）。 */
    dispatch(module: string, params?: Readonly<Record<string, string>>): DispatchResult;
    status(handle: RunHandle): RunStatus;      // done/failed 后仍可查终态
    abort(handle: RunHandle): boolean;
    events(since?: number): readonly RunEvent[];
    current(): RunStatus | null;               // 本插件当前 active Run
}
/** 宿主的 profile 窗口（开始 / 结束）。
 *
 * ★ S2：**`RunCtx` 与 `RenderCtx` 都有**，但**返回形状不同**（有意）：
 *   · `RunCtx.profile`（`ProfileApi`）= 旧 C2 契约：`begin(label)` / `end()` 都是 **`void`**；
 *     失败 = TypeError；`end` 的报告文本由宿主写 **stdout**（D-C2-4）。
 *   · `RenderCtx.profile`（`RenderProfileApi`）= S2 复审 I3：`end()` 返回
 *     `{ok, text, error}`（与旧 UiScript 的 `host.profileEnd()` 同形）—— HUD 要判文本里的
 *     `INVALID` token；`begin` 仍是 `void`。
 * ★ `end()` 的宿主成本（生成报告 + 写剪贴板）走 `HostPause`，**不计入** 2 ms 回调预算。
 */
export interface ProfileApi {
    begin(label: string): void;
    end(): void;
}
/** `RenderCtx.profile`：`end()` 回传报告文本（判 `INVALID` 用）；`begin` 与 RunCtx 同形（void）。 */
export interface RenderProfileApi {
    begin(label: string): void;
    /** 与 `host.profileEnd()` 同形；失败也是**结果对象**（⛔ 不抛 —— 没有窗口是正常状态）。 */
    end(): { readonly ok: boolean; readonly text: string; readonly error: string };
}
export type LogLevel = "debug" | "info" | "warn" | "error";
export type LogApi = (level: LogLevel, message: string, data?: unknown) => void;

/** render 只拿当帧 readings；不阻塞、不做 guest 写；按钮可 dispatch。
 *
 * ★ C2b 状态：`mouse()` / `localFrames()` 已实现（当帧 `UiFrameReadings`；**读数没有值 ⇒ `undefined`**，
 *   ⛔ 不编 0 / 空数组）。类型上必须能表达这一点（`c2b1` 复审 F1）。
 * ★ C2b-2：`mouse().buttons`（三键电平）**已有生产读数源**（窗口装配的 `WindowManager`；与 guest
 *   输入同一张电平表）。类型仍是**可选**：宿主这一帧没有读数源时不安装这个键（⛔ 不编 `false`：
 *   那会让"鼠标按着"与"没有源"长得一样）⇒ 插件必须写 `mouse.buttons?.left ?? false` 这种形态。
 * ★ **S2**：为了让内置 `hud` 从经典 `game.*` 迁到 `ctx.game.*` 后**逐字节等价**，这里补上经典的
 *   **只读 getter**：`frame`（恒有）/ `fps?` / `script?` / `worst?` / `click?` / `hover?` / `active?`；
 *   **D4** 再加 `overlayKeySeq?`（固定 F9 的按下沿计数，无源 ⇒ `undefined`）。
 *   语义与 `--overlay` 的全局 `game.*` **逐条相同**（`script_quickjs.cpp` 同一个
 *   `jsGameProperty` 实现、同一份当帧 readings）：没有值 ⇒ `undefined`，`click` 是**一次性**对象
 *   （只在该帧有值），`hover` / `active` 是**只读 key**（不在命中区上 ⇒ `undefined`）。
 *   ⚠ `mouse` 仍是**方法**（⛔ 不是 getter）：必须写 `ctx.game.mouse()`。
 */
export interface GameRead {
    /** 没有源（`readings.frames == null`）/ 宿主报错 / 空表 ⇒ `undefined`（⛔ 不编空数组）。 */
    localFrames(): readonly FrameInfo[] | undefined;
    /** 这一帧没有鼠标读数 ⇒ `undefined`（⛔ 不编 `{x:0,y:0}`）。 */
    mouse():
        | {
            readonly x: number;
            readonly y: number;
            /** 有读数源才装（C2b-2 窗口装配 = 有）；没有源 ⇒ **缺键**（见上）。 */
            readonly buttons?: {
                readonly left: boolean;
                readonly right: boolean;
                readonly middle: boolean;
            };
        }
        | undefined;
    /** 当前帧号（宿主每帧都给 ⇒ **总是**数字）。 */
    readonly frame: number;
    /** 最近 1 s 的 Present 次数；没有读数 ⇒ `undefined`（⛔ 不编 0）。 */
    readonly fps?: number;
    /** 当前帧的 marker 文件名（例 `TITLE.BIN`）；没有 / 空 ⇒ `undefined`。 */
    readonly script?: string;
    /** 最近 3 s 最坏一帧；`guest_ms` 在块计时没开时**缺键**。 */
    readonly worst?: {
        readonly wall_ms: number;
        readonly guest_ms?: number;
        readonly shim_ms: number;
    };
    /** **一次性**点击载荷（抬起点窗口像素）；只在该帧有值。 */
    readonly click?: { readonly key: string; readonly x: number; readonly y: number };
    /** 指针当前所在命中区的 key；不在任何命中区上 ⇒ `undefined`。 */
    readonly hover?: string;
    /** 当前被按下捕获的 key；没有 ⇒ `undefined`。 */
    readonly active?: string;
    /**
     * 叠层热键（固定 **F9**）的**单调按下沿计数**（D4，2026-11-25 父级裁决）。
     *
     * ★ 唯一来源 = `present_sdl.cpp` 的 `drawOverlay` 每帧采一次 `WindowManager::keyDown`，
     *   按下沿 +1、饱和不回绕；**不吞键**（F9 仍到 guest）。
     * ★ 宿主这一帧没有按键源（没有 `WindowManager` / headless / 测试替身）⇒ **缺键**
     *   （`undefined`；⛔ 不编 0："没按过"与"没有源"必须分得开）。
     * ★ 用法：模块级记上一次的值，变了就切换显隐（⛔ 别把它当电平 —— 电平会每帧重复触发）。
     */
    readonly overlayKeySeq?: number;
}
/** render 只拿当帧 readings；模块级变量是 per-plugin 状态（render/onTrigger 共享）。
 *
 * ★ C2b 实现面：`ui` / `draw` / `read` / `game` / `params` / `log`；**S2 加 `profile`**；
 *   **2026-11-26 加回 `config`**（运行期 `Object.keys(ctx)` **恰好**这八个；`run` 在 interface 里
 *   **声明保留**、运行期**不安装**（C5）⇒ `typeof ctx.run === "undefined"`）。
 *   ⛔ 没有 `edit` / `input`（RunCtx 的写面；render 不做 guest 写）、`state` / `now` / `trigger` /
 *   `fail` / `finish` / `quit`（Run 语境），也⛔ 没有 `sample` / `screenshot`。
 * ★ **S2**：`ctx.profile.begin(label)` 是 `void`（失败 = TypeError）；`ctx.profile.end()` 返回
 *   `{ok, text, error}`（与旧 UiScript `host.profileEnd()` 同形；HUD 用它判 `INVALID`）。
 */
export interface RenderCtx {
    readonly ui: UiApi;          // render 可写 hover/动画状态
    readonly draw: DrawApi;      // 本帧 display list
    readonly read: ReadApi;
    readonly game: GameRead;
    /** ⚠ **C5**（声明保留；运行期**不安装** ⇒ `undefined`；⛔ 不是"有但抛"）。 */
    readonly run: RunApi;
    readonly params: Readonly<Record<string, string>>;
    readonly log: LogApi;
    /** S2：宿主的 profile 窗口（`end()` 回传报告文本；结束走 HostPause）。 */
    readonly profile: RenderProfileApi;
    /** 域式 JSON 配置（`<--appdata>/plugin-config.json`；**与经典 `host.config` 同一份实现**）。
     *  ★ render 可经宿主桥**写**这个文件（宿主在每帧 render 之后 flush 一次），但仍⛔ 不做
     *    guest 写 / 任意路径 I/O。没有 `--appdata` ⇒ `available:false`、`domain()` 抛
     *    `Error("no_appdata")`（⛔ 不做内存态）。 */
    readonly config: ConfigApi;
}

/** `ctx.now`：**本读点的读数快照**（C2a；与 `FiredEvent`/`TimeoutEvent` 的 `TriggerEventBase` 同源）。
 *
 * ★ **只在本回调内有效**（D-C2-7）：宿主**每次回调新建**这个值对象 ⇒ ⛔ 不要把它存进模块变量
 *   （存了以后下一次回调读到的是"那次回调的值"，而不是当前读点）。
 * ★ 有效才给值：`script` / `pos` / `mouse` 只在宿主那一边真的读到的时候出现（⛔ 不编 0 / 空串）；
 *   `posValid` / `readIndex` / `point` 恒有（`posValid=false` 表示"位置暂时读不到"，不要当成 0）。
 */
export interface RunNow {
    /** ★ **marker 文件名**（例 `TITLE.BIN`，含后缀；⛔ 不是裸名）。 */
    readonly script?: string;
    readonly pos?: number;
    readonly posValid: boolean;
    readonly readIndex: number;
    readonly point: InputPoint;
    readonly mouse?: { readonly x: number; readonly y: number };
}

/** `ctx.appdata.stat(rel)` 的结果对象（**同步**；不存在 / 非法路径都是正常结果，⛔ 不抛）。
 *
 * ★ 只读；宿主**只 appdata 子树**：`rel` 是相对 appdata 根的路径（`/` 或 `\` 都收），
 *   绝对路径 / `..` 段 / NUL ⇒ `invalid_path`；canonical 之后跑出根外（符号链接 / junction）⇒ `escape`。
 * ★ 错误码是稳定词表：`no_appdata` / `invalid_path` / `escape` / `not_found` / `not_dir` /
 *   `denied` / `io`（⛔ 不把 errno / 平台文案漏给 JS）。
 */
export interface AppDataStat {
    readonly exists: boolean;
    /** 只在 `exists` 时安装。 */
    readonly isFile?: boolean;
    /** 只在 `exists` 时安装。 */
    readonly isDir?: boolean;
    /** bytes；只在 `isFile` 且宿主 `file_size` 成功时安装。 */
    readonly size?: number;
    /** `exists=false` 时的原因（缺省 `not_found`）。 */
    readonly error?:
        | "no_appdata"
        | "invalid_path"
        | "escape"
        | "not_found"
        | "not_dir"
        | "denied"
        | "io";
}

/** `ctx.appdata`：宿主的 **`--appdata` 根**（只读 stat；只装在 `RunCtx`，⛔ `RenderCtx` 没有）。
 *
 * ★ 宿主**不知道** title / `set:SavePath`：存档目录由插件用常量拼（`save.js` 的 `SAVE_DIR_REL`）。
 *   `loadSave` 的前置检查：`saveDir`/`SAVE<NN>.DAT` **不存在**（含目录不存在）⇒ `finish(5)` + 0 click；
 *   只有 `escape` / `invalid_path` / `denied` / `io` 才跳过。
 *   ⚠ `set:UseAppDataFolder != 1`（存档在安装树）时，固定的 `SAVE_DIR_REL` 必然 `not_found` ⇒ 会 fail：
 *   用 `loadSave(..., { saveDir })` / 配置 `save.saveDir` 指到实际目录，或显式 `precheck:false`
 *   （逃生口；缺省 true，⛔ 不是让"真缺文件"静默通过）。
 * ★ `stat` 的宿主段**不计入** 2 ms 预算（与 `ctx.profile.end` 同一原则）。
 */
export interface AppDataApi {
    /** `--appdata` 非空且根可绝对化（且不落在只读安装树里）。false ⇒ `path=null`、`stat` 一律 `no_appdata`。 */
    readonly available: boolean;
    /** 绝对宿主路径（UTF-8；宿主分隔符口径）；不可用 ⇒ `null`。 */
    readonly path: string | null;
    /** 同步只读 stat；`rel` 相对 appdata 根。参数不是字符串 ⇒ **TypeError**（⛔ 不是结果对象）。 */
    stat(rel: string): AppDataStat;
}

/** `set` / `delete` 的回执（校验错误走 TypeError，不走这里）。 */
export interface ConfigReceipt {
    /**
     * `true` = **内存已改**（⛔ 不代表已落盘）。持久化在宿主每个回调末 flush；失败只有一次
     * `ctx.log("warn")` —— 回执里看不到那次失败（v1 口径，见 `plugin-runtime.md` §4.5）。
     */
    readonly applied: boolean;
    /** `read_only` = 配置文件本身不可写（或 appdata 根不可写）；`no_appdata` = 没有 `--appdata`。 */
    readonly code?: "read_only" | "no_appdata";
}

/** 一个配置域（`ctx.config.domain(id)`）。同一进程内所有域共享同一份 store。 */
export interface ConfigDomain {
    /** 没有这个 key ⇒ 返回 `fallback`（不给 = `undefined`）。 */
    get(key: string, fallback?: unknown): unknown;
    has(key: string): boolean;
    /** 只改内存并标脏（宿主在每个回调末 atomically flush 一次）；校验错误（key / 值 / 超限）⇒ TypeError。 */
    set(key: string, value: unknown): ConfigReceipt;
    /** true = 真的删掉了（不存在 / 只读 ⇒ false）。 */
    delete(key: string): boolean;
    /** 该域的深拷贝快照（原生对象；改它不影响 store）。 */
    all(): Record<string, unknown>;
}

/**
 * `ctx.config`：**域式 JSON 配置**（`<--appdata>/plugin-config.json`；**`RunCtx` 与 `RenderCtx`
 * 都有** —— 2026-11-26 加回 `RenderCtx`（7→8 成员）；render 可经宿主桥**写**这个文件，
 * 仍⛔ 不做 guest 写 / 任意路径 I/O）。
 *
 * 文件信封：`{"version":1,"domains":{"<id>":{...}}}`。未知顶层键与未知域内键**保留**（前向兼容）；
 * 域自动创建、不注册、可共享。域 id = `^[A-Za-z0-9._-]{1,64}$`；域 id 与 key 都拒
 * `__proto__` / `prototype` / `constructor`。值 = 原生 JS 值（对象 / 数组 / 字符串 / 有限数字 / 布尔 /
 * null；⛔ 不是 JSON 字符串 ⇒ 沙箱里不需要 `JSON`）。
 *
 * ★ 没有 `--appdata` ⇒ `available=false`，`domain()` 抛 `Error("no_appdata")`（⛔ 不做内存态）。
 * ★ 上限（`set` 与**装载期**同一套）：值嵌套 ≤ **8 层**（根值 = 1）、值树 ≤ **1024 节点**、
 *   单个字符串 / 对象 key ≤ **8192 字节**、key 长 ≤ **128**、域数 ≤ **256**、每域 key ≤ **1024**、
 *   整份文件 ≤ **1 MiB**；超限 ⇒ TypeError（装载期 ⇒ exit 2）。字符串 / key 必须是合法 UTF-8。
 * ★ 改配置的产物 = 该 appdata 根的 `plugin-config.json`（⛔ 不在 `SAVE/`；`clean-appdata.sh` 会删它）。
 * ★ 一个 appdata 只跑一个实例（v1；⛔ 不做文件锁）。
 */
export interface ConfigApi {
    /** 有没有可用的 appdata 根（false ⇒ `domain()` 抛 `Error("no_appdata")`）。 */
    readonly available: boolean;
    /** 域 id 非法 / 保留名 ⇒ TypeError；不可用 ⇒ `Error("no_appdata")`。 */
    domain(id: string): ConfigDomain;
}

/**
 * RunCtx：一次 Run 的 callback 上下文；Edit/Input/Read/Trigger/UI/Run。
 *
 * ★ 切片状态（C2a，2026-11-20；C2b 追加注释；2026-11-25 追加 `appdata` / `config`；
 *   2026-11-26 `config` 加回 `RenderCtx`；S2b/S2c 追加 `once` / `task` / `at` / `off`；
 *   P0 追加 `delay`，`once` 放开全部 `AtWhere`；P1-fix S2 追加 `waitAny`（多臂 + wall-clock 超时））：
 *   - **v2（推荐）**：`at` / `off` / `once` / `waitAny` / `delay` / `task` + `config` / `appdata` / `read` /
 *     `log` / `fail` / `finish` / `quit`；
 *   - **v1 兼容层（deprecated）**：`trigger` / `hook` / `now`（新代码见各成员的 `@deprecated`）；
 *   - 其它已实现：`edit` / `input` / `read.global.get` / `ui`（**只写**宿主本地状态，v2 里 UI 只归
 *     render）/ `profile` / `state` / `params`；
 *   - ⛔ **`RunCtx` 永远没有** `draw`（C2b 只把它装在 `RenderCtx` 上）/ `run` / `dispatch`（C5）——
 *     `ctx.run` 在 C2 是 `undefined`（⛔ 不是"有但抛"）；也不安装 `sample` / `screenshot`（永远不做）。
 */
export interface RunCtx<State = Record<string, unknown>> {
    readonly edit: EditApi;
    readonly input: InputApi;
    readonly read: ReadApi;
    /**
     * **v1 兼容层**：具名 armed 集合（afterScript / afterPosition / afterDelay / atReadPoint）。
     * @deprecated v2 用 `ctx.at`（同步订阅）/ `ctx.once`（async 等待）/ `ctx.task`（协程）；
     *   本字段仍按 v1 语义工作（start/onTrigger 返回 Trigger|Trigger[]|null），新插件不要再写。
     */
    readonly trigger: TriggerApi;
    /**
     * **脚本指令位置钩子**（v1）：`at(script,pos)` 只装"到点"（纯观察），句柄上的 `capture/stats` 读 local、
     * `patch` 改值（该指令读之前覆盖、下一条脚本指令派发前恢复 ⇒ **净变化 0**，⛔ 不进存档）。
     * ★ 触发 / 读 / 改是**三个独立能力**（用户裁决 2026-11-26 晚）；持久写仍走 `ctx.edit.global.set`。
     * ★ 只 `RunCtx` 有（`RenderCtx` 没有：render 不做 guest 写）。
     * @deprecated v2 的**触发**面用 `ctx.at`（链 + `next()`）/ `ctx.once`；
     *   `hook.at` 只剩"读写 local / patch"这一半用途（新代码若要 patch/capture 仍可用它）。
     */
    readonly hook: HookApi;
    /** **异步等待**一个脚本指令点（S2b）：命中 ⇒ resolve 一个 `Tick`（与 `ctx.at` 的 Tick 同形；
     *  `id` = 这条 `once` 自己的临时订阅）。★ S2c-2 起 `local` / `edit` / `input` 在 resolve 的
     *  `.then` 里可用（continuation 在同一入口的 drain 里跑）；`patch` 会回执 `once_tick`
     *  （临时条目已撤）。
     *  · `where` = **与 `ctx.at` 完全相同的 `AtWhere` 五种形态**（同一个 `readAtWhere`）：
     *    `{script,pos}` / `{script,lo,hi}`（`[lo,hi)`）/ `{script}` / `{opcode}` / `{script,opcode}`。
     *    ★ `{script}` 的语义 = **该脚本下一条指令**（第一条命中即 resolve）—— 用于"等下一拍"的
     *      采样（替代 v1 `atReadPoint("any")`；频率是脚本指令而非读点）。
     *  · **门 = Run 侧入口**：Run 回调 / `ctx.task` 的 continuation / 泵（读点或 render 前）里都能注册
     *    —— **H1 已取消**：可以"翻一页 → 判 → 再注册下一道门"地把动态链写下去（save 页链就靠这条）。
     *  · `opts.timeoutMs` **必填且 > 0**（缺省 / 非法 ⇒ TypeError）：到点 = **失败截止**
     *    （先 reject，让 `catch` 在泵里 drain 到；随后宿主记 timeout 并 **fail Run**）。
     *    ⛔ 不提供"永不结束的等待"；
     *  · 每次调用装一条**临时** hook（owner = 本 Run）；命中 / 超时 / Run 收口自动撤；
     *  · 同一个 Run 可以有多个 `once`（与 `ctx.hook.at` 走同一张 owner 表）。
     *  ★ 它让 Run **保持存活**（与 task / delay 一样）：等待期间的 Run 不收口。 */
    once(where: AtWhere, opts: { readonly timeoutMs: number }): Promise<Tick>;
    /**
     * **多臂等待**（P1-fix S2）：同时装 `arms.length` 条**临时**订阅，**第一条命中胜出**。
     *  · `arms` = **至少 1 条** `AtWhere`（与 `ctx.once` 完全相同的五种形态），最多 **32** 条；
     *    空数组 / 超过 32 条 / 形状非法 ⇒ **TypeError**（⛔ 不返回一个永不 settle 的 Promise）；
     *  · resolve `{ index, tick }`：`index` = 胜出的那一条在 `arms` 里的下标（注册序），`tick` =
     *    命中那一拍的 Tick（与 `ctx.once` 的 Tick 同形；临时条目已结算 ⇒ `tick.patch` 回执
     *    `once_tick`，`tick.local` / `edit` / `input` 仍可用）；
     *  · 胜出后**其余臂的临时 hook 立刻撤掉**（不留 pending）—— 这正是 `Promise.race([ctx.once(...)])`
     *    做不到的（`once` 不能撤）；
     *  · `opts.timeoutMs` **必填且 > 0**：到点 = **失败截止**（与 `ctx.once` 同一条）——先 reject
     *    （reason 列出全部 arms），让 `catch` 在泵里 drain 到；随后宿主记 timeout 并 **fail Run**
     *    （用户 catch 了也照样 fail；若 catch 里 `ctx.finish(5, …)`，Run 已是终态 ⇒ 读数是 finished）。
     *    ⛔ 不提供"永不结束的等待"；
     *  · 门 = **Run 侧入口**（Run 回调 / task continuation / 泵）；render 里 TypeError；
     *  · 等待期间让 Run **保持存活**（`hasPendingWork`）；命中 / 超时 / Run 收口清账；
     *  · `ctx.hook.clear()` / owner release 撤掉臂 ⇒ 整组被外部清掉时 **reject**（⛔ 不留永久 pending）。
     *  @example `const hit = await ctx.waitAny([{script:"FIELD.BIN"},{script:"BTL.BIN"}], {timeoutMs: 30000});`
     */
    waitAny(arms: readonly AtWhere[], opts: { readonly timeoutMs: number }): Promise<WaitAnyResult>;
    /** **墙钟延迟**（P0）：`await ctx.delay(ms)` ⇒ 至少等 `ms` 毫秒后 resolve。
     *  · `ms` **必填 / 整数 / `0 <= ms <= 24h`**（越界 / 小数 / 非数字 ⇒ TypeError；⛔ 不截断）；
     *  · **不是 trigger**：到点只 resolve，**不 fail Run**；由**既有泵**推进（读点 / render 入口）——
     *    ⛔ 没有独立 timer；
     *  · **NON-DIFFABLE**：墙钟随机器 / 负载变化，⛔ 不用它做 A/B、指纹或"多久发生"的判据
     *    （它是"等一拍"的脚本逻辑，替代 v1 `afterDelay`）；
     *  · 门 = **Run 侧入口**（Run 回调 / task continuation / 泵）；render 里 TypeError；
     *  · 等待期间让 Run **保持存活**（`hasPendingWork`）；Run 收口时清表（**不 resolve**、也不再跑 JS）。
     *  @example `ctx.task(async (t) => { await ctx.delay(500); t.input.click(640, 480); });`
     */
    delay(ms: number): Promise<void>;
    /** **协程**（S2b）：`fn(ctx)` 必须**同步**返回 Promise（否则 TypeError）；返回的 promise 进本
     *  Run 的 task 账。每个 host→JS 入口末尾（drain 之后）检查 settle：
     *  fulfilled ⇒ 移除；rejected ⇒ **fail Run**（reason = 拒绝原因）；pending ⇒ 保留。
     *  · `fn` 收到**当前 RunCtx**（同一个 `now` / `state`）；
     *  · 门 = **Run 侧入口**（Run 回调 / 泵）；render 里 TypeError。★ 但"当前 RunCtx"只在 Run 回调
     *    入口内存在 ⇒ 在 task continuation 里再 `ctx.task(...)` 会因缺当前 RunCtx 抛 TypeError
     *    （要组合用 `await` / `Promise.all`；`once` / `at` / `delay` 不需要它）。
     *  · 不需要 `await`：登记即返回 `undefined`；task 组合用 Promise 自身的 `await` / `Promise.all`。
     *  ★ 它让 Run **保持存活**：所有 task settled 之前 Run 不收口（`return []` 也不会）。 */
    task(fn: (ctx: RunCtx<State>) => Promise<unknown>): void;
    /**
     * **同步订阅**一个指令点（S2c）：命中时按**注册序**跑 handler；调 `next()` 继续，不调 = 截断。
     *  · 门 = **Run 侧入口**（Run 回调 / `ctx.task` 的 continuation / 泵）；render 里 TypeError；
     *  · 返回订阅 id（`> 0`）；`no_injection`（`--no-s7`）⇒ `undefined` + 宿主一行显眼 error；
     *  · 每条订阅让 Run **保持存活**（`hasPendingWork`）⇒ 要收口就 `ctx.off(id)` / `finish` / `quit`；
     *  · handler 必须**同步**（返回 Promise 会被忽略 + 一次性 warn；`next()` 必须在 await 之前调）；
     *  · handler 抛错 / 超预算 ⇒ **撤整条订阅** + 一行 error，Run 不 fail。
     *  · 链的顺序 = 注册序；`once` / `handle.onTick` 是**被动观察者**（截断不影响它们）。
     *  ★ `where` 的形态与运行时的 `readAtWhere` 逐条一致：
     *    `{script,pos}` · `{script,lo,hi}`（`[lo,hi)`）· `{script}`（整本脚本 · ⚠ 每拍命中）·
     *    `{opcode}`（全脚本）· `{script,opcode}`（该脚本内的该 opcode）。
     *    非法 / 模糊形状（`{}`、`{pos}`、`pos` 与 `lo/hi` 同给、小数、非字符串 script）⇒ TypeError。
     */
    at(where: AtWhere, handler: (tick: Tick, next: () => void) => void): number | undefined;
    /**
     * 撤一条订阅（**幂等**）：`true` = 这次真的撤掉了；`false` = 本来就不在（Run 已收口 / 撤过）。
     * ★ 门是"在插件上下文里"（**不是**"正在 Run 回调里"）：tick handler / task continuation /
     *   render 里都能撤（它只减，不写 guest、不让 Run 更长）。
     */
    off(handle: number): boolean;
    readonly ui: UiApi;
    /** ⚠ **C5**（C2a 里是 `undefined`）。Run 的 status/abort/events/dispatch。 */
    readonly run: RunApi;
    readonly profile: ProfileApi;
    /** per-Run；只放数据；不跨 endpoint/Run 共享。宿主建它时对 `initialState` 做**浅拷贝**（见下）。 */
    readonly state: State;
    readonly params: Readonly<Record<string, string>>;
    /** 宿主的 `--appdata` 根（只读 stat；只有 RunCtx 有 —— render 直接碰不到 appdata 子树）。 */
    readonly appdata: AppDataApi;
    /** 域式 JSON 配置（`<--appdata>/plugin-config.json`；`RunCtx` 与 `RenderCtx` 同形 ——
     *  ★ 2026-11-26 起 render 也可经宿主桥写这个文件，仍⛔ 不做任意路径 I/O）。 */
    readonly config: ConfigApi;
    /** `ctx.log(level, message, data?)`：**一行一条、有界、去重**；`data` 当前**不进日志行**
     *  （结构化数据请先 `String(...)` 再传 message）。 */
    readonly log: LogApi;
    /** **本读点的读数快照**；每次回调新建、只在本回调内有效（⛔ 不要存进模块变量）。
     *  @deprecated v2 用 `Tick.now`（`ctx.at` 的回调参数 / `ctx.once` 的 resolve）；
     *    本字段是 v1 兼容层，值仍按 v1 语义填。 */
    readonly now: RunNow;
    /** 失败当前 Run；name 是可选的具名状态，reason 是原因。
     *  ★ 终态异常：本回调里**已经提交**的 `ctx.input.*` 会被立刻丢弃（⛔ 不再落地）。 */
    fail(name?: string, reason?: string): void;
    /** 结束当前 Run；exitCode 可映射历史 exit 5。
     *  ★ 正常收口：**不**丢弃本回调提交的 input（最后一个 `ctx.input.click` 仍会落地）。 */
    finish(exitCode: number, reason?: string): void;
    /** 请求宿主停机（WM_QUIT + stop_requested）；Run 收口/releaseOwner。
     *  ★ 终态异常（同 `fail`）：本回调已提交的 input 会被立刻丢弃。 */
    quit(reason?: string): void;
}
/** `ctx.hook.at(script, pos)` 的句柄：**触发 / 读 / 改是三个独立能力**。
 *  · 触发 = `at` 本身（纯观察：只计数，⛔ 不读不写 guest）；
 *  · 读   = `capture(...)` + `stats()`（最近一次命中的 local 读数）；
 *  · 改   = `patch(target, value)`（这条指令读之前覆盖一个 dword，指令后恢复 ⇒ 净变化 0）。
 *  ★ 句柄函数从 `this.id` 取句柄 ⇒ ⛔ 不要解构（`const {patch} = h; patch(...)` 会失效）。 */
export interface InstrHookTarget {
    /** `local-ptr N` 指向的 dword（`DRAWOBJ 0x03A0` 的 `eq` 读的就是它：N = 2）。 */
    ptrLocal?: number;
    /** `local-int N`。 */
    localInt?: number;
    /** 全局池 id（与 `ctx.edit.global.set(kind, id, …)` 同一个 id 空间）。 */
    globalId?: number;
}
export interface InstrHookStats {
    readonly id: number;
    readonly script: string;
    readonly pos: number;
    readonly patch: boolean;
    readonly hits: number;
    readonly restores: number;
    /** ★ S0：恢复时"当前值已不是本 hook 写进去的那个"的次数（不写回；别人的写赢）。
     *  `restores + restore_conflicts` 覆盖"覆盖之后总有一次结算"，`skips` 才是"没能写"。 */
    readonly restore_conflicts: number;
    readonly skips: number;
    /** `capture({ints})` 的语义值（最近一次命中）。 */
    readonly ints: number[];
    /** `capture({ptrs})` 的**指针指向的 dword**（语义值；不可读 = 0）。 */
    readonly ptrTargets: number[];
    // ---- S2c 的形状面（可选：老调用方不需要读它们）----
    /** 这一行是哪种订阅：v1 `ctx.hook.at` / v2 `ctx.at` / `once` 的临时 hook。
     *  ⚠ 它**不是** patch 的种类（C++ 的 `InstrHookStatus.kind` 那个才是 `ptrDword` / `localInt` /
     *  `globalInt`；JS 面看不到后者）。 */
    readonly kind?: "v1" | "sub" | "once";
    /** `form` 决定哪个字段有意义（`pos` 只在 `"pos"` 形态下非 0）。 */
    readonly form?: "pos" | "range" | "script" | "opcode";
    /** ★ `lo` / `hi` / `opcode` 是 **uint32** 语义（区间端点 / opcode 可 > 2^31）：JS 出口用
     *  double，⛔ 不按 int32 截成负数。非 Opcode 形态 `opcode` 恒为 0（不是"缺键"）。 */
    readonly lo?: number;
    readonly hi?: number;
    readonly opcode?: number;
}
/** `ctx.at` 的匹配条件（S2c-2：五种形态，与运行时的 `readAtWhere` 逐条一致）。
 *  · `{script,pos}`：单点；
 *  · `{script,lo,hi}`：区间 `[lo,hi)`（`0 <= lo < hi <= 2^32-1`）；
 *  · `{script}`：整本脚本的每条指令（⚠ 高频面，成本由插件自担）；
 *  · `{opcode}` / `{script,opcode}`：opcode == 该值（可选按脚本收窄；opcode 0..2^32-1）。
 *  ★ `pos` / `lo` / `hi` / `opcode` 必须是**整数**（小数 ⇒ TypeError，⛔ 不截断）；
 *    `script` 必须是**非空字符串**（数字不会被 toString）。
 *  ★ 键**存在**但值是 `null` / `undefined` ⇒ TypeError（与"没给这个键"区分；⛔ 不把 null/undefined
 *    当"没给" —— 那会把 `{script, pos: undefined}` 静默变成 `{script}` 整本脚本订阅）。
 *  ★ 模糊形状（空对象 / 只有 `pos` / `pos` 与 `lo/hi` 或 `opcode` 同给 / `lo` 与 `hi` 缺一）⇒ TypeError。 */
export type AtWhere =
    | { readonly script: string; readonly pos: number }
    | { readonly script: string; readonly lo: number; readonly hi: number }
    | { readonly script: string }
    | { readonly script?: string; readonly opcode: number };
/** `Tick.now`：**本拍命中的值快照**（⛔ 不是"当前位置"；跨入口仍读到旧值 —— 这是有意的）。 */
export interface TickNow {
    readonly script: string;
    readonly pos: number;
    /** 读不到 ⇒ **缺键**（⛔ 不是 0：0 是真实 opcode；与 `InstrTick.opcode` 同口径）。 */
    readonly opcode?: number;
}
/** `Tick.local`：**活读** guest（语义值 = DEC）。有效期 = 产生 Tick 的那次入口（含其 drain）。
 *  ★ `index` 必须是 **0..2^32-1 的整数**：小数 / NaN ⇒ TypeError（⛔ 不截断到相邻槽）；
 *    越界 / 不可读 / 指针 0 / 没有 ENC key ⇒ `undefined`（边界，不是类型错）。 */
export interface TickLocal {
    /** `local-int index` 的**语义值**；越界 / 不可读 / 没有 ENC key ⇒ `undefined`。 */
    int(index: number): number | undefined;
    /** `local-ptr index` 的**指针值**（地址，⛔ 不解码）；越界 ⇒ `undefined`。 */
    ptr(index: number): number | undefined;
    /** `local-ptr index` **指向**的 dword 的语义值；越界 / 指针 0 / 不可读 ⇒ `undefined`。 */
    ptrDword(index: number): number | undefined;
}
/** **Tick**（S2c）：来自 `ctx.at` 的回调参数或 `ctx.once(...)` 的 resolve（v1 `handle.onTick`
 *  拿到的是**同一个对象**；它的静态类型仍是平铺的 `InstrTick`）；⛔ 不要自己造。
 *  ★ 有效期 = **产生它的那一次指令点入口（含该入口的 drain）**：跨入口用 `local` / `edit` /
 *    `input` / `patch` ⇒ 抛 `Error`（消息含 `stale_tick`）；`now` 是快照、`log` 是宿主
 *    出口 ⇒ 仍可用。
 *  ★ 一次派发**一个** Tick 对象（全链共享同一个快照）；`id` = 链里**第一条属于本实例的**订阅的
 *    句柄（多 `Impl` 同活时不会串到别人的句柄；要自己的 id 就在 `ctx.at` 时把返回值存起来）。
 *  ★ 写面（S2c-2）：`edit` 立即持久写、`input` 入队（下一读点落地）、`patch` 本拍瞬时覆盖。
 *    ⛔ tick 入口**不**打开 `in_callback_`：本对象上的这几个是唯一被放开的能力；同一个 handler 里
 *    从 RunCtx 调的 `ctx.fail` / `finish` / `quit` / `trigger` / `hook.*` / `profile` / `config`
 *    仍一律 TypeError。
 */
export interface Tick {
    readonly id: number;
    readonly now: TickNow;
    readonly local: TickLocal;
    /** 与 `RunCtx.edit` **同形**（同一个宿主方法；立即持久写 ⇒ 下一读点读得到）。 */
    readonly edit: EditApi;
    /** 与 `RunCtx.input` **同形**（入队，下一读点落地）。 */
    readonly input: InputApi;
    /**
     * **本拍瞬时覆盖**（S2c-2）：在这条指令**读之前**写一个 dword，下一拍派发前按 CAS 恢复。
     *  · `target` 与 `InstrHookHandle.patch` 同形（`{ptrLocal}` / `{localInt}` / `{globalId}` 三选一）；
     *  · 只作用于**本拍命中**的订阅条目：链里每个 handler 的 `patch` 归到**自己那条订阅**；
     *    链外（drain / once）回落到 `id` 那条；
     *  · handler 里读到的是**打补丁前**的值（写发生在 `on_hits` 返回之后）；
     *  · 回执：成功 `{accepted:true, applied:true}`；失败 `code` = `bad_target` / `no_key` /
     *    `too_many_slots`（本拍不同地址已到 8）/ `not_hit` / `no_such_hook`；
     *  · `once` 的 Tick 调 `patch` ⇒ `{accepted:false, applied:false, code:"once_tick"}`（once 的
     *    临时条目在 resolve 时已经结算 ⇒ 覆盖无处落地）。这个判据在**进 runtime 之前**，⛔ 不会因为
     *    条目已撤而退回 `no_such_hook`（那是"句柄不存在"的读数）。`local` / `edit` / `input` 仍可用。
     */
    patch(target: InstrHookTarget, value: number): Receipt;
    log(level: string, message: string, data?: unknown): void;
    // ---- v1 兼容层（平铺字段；新代码用 `now.*`）----
    /** @deprecated 用 `now.script`。 */
    readonly script: string;
    /** @deprecated 用 `now.pos`。 */
    readonly pos: number;
    /** @deprecated 用 `now.opcode`。 */
    readonly opcode?: number;
}
/** `ctx.waitAny` 的 resolve 值（P1-fix S2）。
 *  · `index` = 胜出的那一条臂在 `arms` 里的下标（**注册序**；0 基）；
 *  · `tick` = 命中那一拍的 `Tick`（与 `ctx.once` 的 Tick 同形；它的临时条目已结算 ⇒
 *    `tick.patch` 回执 `once_tick`）。
 *  ★ 有效期与 `Tick` 同一条：`local` / `edit` / `input` 只在产生它的那次入口（含 drain）内可用。 */
export interface WaitAnyResult {
    readonly index: number;
    readonly tick: Tick;
}
/** S2a/S2b 的旧名（= v2 `Tick` 的**平铺**视图；保留是因为 `handle.onTick` 的既有调用方与夹具
 *  用平铺字段）。★ 运行时两者是**同一个对象**（`handle.onTick` 拿到的也带 `now` / `local` /
 *  `edit` / `input` / `patch`）；这个别名只描述平铺字段。
 *  @deprecated 用 `Tick`（v1 兼容层，后续删除）。 */
export interface InstrTick {
    readonly id: number;
    readonly script: string;
    readonly pos: number;
    readonly opcode?: number;
    log(level: string, message: string, data?: unknown): void;
}
export interface InstrHookHandle {
    readonly id: number;
    /** ③ 改：装 / 换覆盖。`opts.maxHits` 只在前 N 次命中写（0/缺省 = 不限）。
     *  `opts.enabled === false` ⇒ 撤覆盖（hook 仍在，继续计数）。 */
    patch(target: InstrHookTarget | null, value?: number, opts?: { maxHits?: number; enabled?: boolean }): Receipt;
    /** ② 读：设采样清单（各最多 8 个；之后 `stats()` 读最近一次命中）。 */
    capture(opts: { ints?: number[]; ptrs?: number[] }): void;
    stats(): InstrHookStats;
    /** 撤这条 hook（未还原的覆盖会先写回）。 */
    clear(): void;
    /** ① 触发（S2a）：指令点命中 → 回调（每个句柄**恰好一个**；`null` = 撤；重复 = 替换）。
     *  ★ 回调入口 `inCallback()` 恒 false ⇒ 从 RunCtx 调的 `ctx.edit` / `input` / `trigger` /
     *    `fail` / `finish` / `quit` / `profile.*` / `appdata.stat` / `hook.*` 全部 TypeError。
     *    S2c-2 起**这个 Tick 自己**的 `edit` / `input` / `patch` 可用（门是 `*Attached()`，不是
     *    `in_callback_`）；`tick.log` 也可用。
     *  ★ 它让 Run **保持存活**（sync 订阅）：不撤它、也没有 task/once 时，`return []` 之后 Run
     *    不会收口（`ctx.hook.at` 本身是 v1 观察，**不算**）。
     *  @deprecated v2 的订阅面用 `ctx.at`（同点多条 + `next()` 链）/ `ctx.once`；
     *    `hook.at(...).onTick` 是 v1 兼容层（tick 对象是同一个 `Tick`，但静态类型是平铺的
     *    `InstrTick`）。 */
    onTick(fn: ((tick: InstrTick) => void) | null): this;
}
/** 脚本指令位置钩子（只 `RunCtx`；`RenderCtx` 没有）。 */
export interface HookApi {
    /** ① 触发：在某条脚本指令**执行前**装一条观察（`pos` = `label_%08x` 坐标）。
     *  同一 `(script,pos)` 只能一条；失败（`script_empty` / `pos_zero` / `duplicate` / `no_injection`）
     *  ⇒ `undefined`（宿主另打一行 error 点名 code；⛔ 不编假句柄）。
     *  ★ `no_injection` = 本构建在脚本派发点**没有**注入站点（`--no-s7` / 闭包没覆盖派发 EA）
     *    ⇒ 宿主另打一行**响亮** error（"ctx.hook.* 永不命中"），⛔ 不返回一个永远 hits=0 的句柄。 */
    at(script: string, pos: number): InstrHookHandle | undefined;
    list(): InstrHookStats[];
    /** 撤掉全部 hook（并把未还原的覆盖按 CAS 结算）。Run 收口 / `quit` 也会自动做。 */
    clear(): void;
}

export interface PluginModule<State = Record<string, unknown>> {
    /** 宿主在 start 之前初始化 ctx.state；省略 ⇒ {} as State。
     *  ★ **浅拷贝**（`Object.assign({}, initialState)`，D-C2-7）：沙箱里没有 `JSON` / `structuredClone`
     *    ⇒ 嵌套对象**不**深拷贝，也不承诺"同 runtime 两个 Run 共享/不共享"（C2 只有一个 Run）。
     *  ★ v2 不依赖它：逻辑数据放模块变量 / per-Run 数据对象即可。
     */
    initialState?: State;
    /** 可选；有它就每帧跑（C2b：传 `RenderCtx`；没有窗口 ⇒ 永不调用 + 一次性 stderr，D-C2-5）。 */
    render?: (ctx: RenderCtx) => void;
    /**
     * Run 的入口。★ v2：返回 **`void`** —— 只做注册（`ctx.at` / `ctx.once` / `ctx.task`），
     * Run 靠这些订阅 / task 存活；不要再返回 Trigger 集合。
     * v1 兼容：仍接受 `Trigger | Trigger[] | null`（返回数组 = 完整替换 armed 集合）。
     */
    start?: (ctx: RunCtx<State>) => Trigger | Trigger[] | null | void;
    /**
     * v1 触发回调（armed 集合消费点）。
     * @deprecated v2 没有 onTrigger：订阅用 `ctx.at`、async 等待用 `ctx.once`、组合用 `ctx.task`；
     *   本字段是兼容层（只在 start 返回了 Trigger 集合时才会被调用）。
     */
    onTrigger?: (ctx: RunCtx<State>, ev: FiredEvent | TimeoutEvent) => Trigger | Trigger[] | null;
}

// ---------------------------------------------------------------------------
// 典型用法（v2 伪代码；不写具体数值）
//
// 加载存档（ctx.task + 顺序 await ctx.once + tick.input.click）：
//   ★ `script` 是 **marker 文件名（含 `.BIN`）**，宿主做**精确相等**；`pos` 从 1 开始。
//   function start(ctx: RunCtx): void {
//     ctx.task(async (t) => {
//       // 门 = Run 侧入口 ⇒ 也可以在第一个 await **之后**再注册（P0 起 H1 取消）
//       const title = t.once({ script: "TITLE.BIN", pos: TITLE_POS }, { timeoutMs: 30000 });
//       const save  = t.once({ script: "SAVE.BIN",  pos: SAVE_POS  }, { timeoutMs: 30000 });
//       const titleTick = await title;
//       titleTick.input.click(LOAD_X, LOAD_Y);       // 入队，下一读点落地
//       const saveTick = await save;
//       saveTick.input.click(SLOT_X, rowY(slot));
//     });   // task settle + 没有订阅 ⇒ Run 自然收口（要 exit code 才用 finish；continuation 里也可）
//   }
//
// 同步订阅（ctx.at；同点多条 = 一条链：next() 继续、不调 = 截断）：
//   let id: number | undefined;
//   id = ctx.at({ script: "FIELD.BIN", lo: 0x1000, hi: 0x1100 }, (tick, next) => {
//     const before = tick.local.int(0) ?? 0;
//     tick.edit.global.set("int", ID, before | MASK);   // 立即持久写（与 ctx.edit 同一个宿主方法）
//     if (id !== undefined) ctx.off(id);                // 撤订阅（幂等；tick handler 里也能调）
//     next();
//   });
//
// Tick（ctx.at 的回调参数 / ctx.once 的 resolve）= { id, now, local, edit, input, patch, log }：
//   · `now = { script, pos, opcode? }`（值快照；⛔ 不要把 `Tick` 存下来跨入口用 —— `local` / `edit` /
//     `input` / `patch` 在下一个入口会 `stale_tick` 抛错；`now` / `log` 仍可用）。
//   · `local.int(i)` / `local.ptr(i)` / `local.ptrDword(i)`：活读；越界 / 指针 0 / 没有 ENC key ⇒ `undefined`。
//   · `edit` / `input` 与 RunCtx 同形；`patch(target, value)` 是**本拍瞬时覆盖**（下一拍按 CAS 恢复；
//     `once` 的 Tick 调它回 `once_tick`）。
//   · tick handler 里 `ctx.fail` / `finish` / `quit` / `trigger` / `hook.*` / `profile` / `config` 都是
//     TypeError —— 写面只有上面这几个 + `ctx.off`；日志用 `tick.log` / 全局 `log`。
//
// 模块装载 + 泛型 state（推荐 satisfies，不用 definePlugin）：
//   import type { PluginModule, RunCtx } from "@amayui/lift-plugin";
//   const plugin = { start, render } satisfies PluginModule;
//   export default plugin;   // 宿主读 default 导出；不再读命名导出/globalThis
//
// ---------------------------------------------------------------------------
// ⚠ **v1 兼容写法（deprecated）**：下面这些语义仍按原样实现（v1 插件不受影响），但新插件不要这样写。
//
// 加载存档的链（name = 状态名）：
//   start 返回第一个 Trigger；onTrigger 按 ev.name 返回下一个（数组 = **完整替换 armed 集合**）；
//   null / [] = done；一个读点最多触发一个（注册顺序）；重复 name = fail。
//
// 重新 arm 语义表（四种；属于 v1 Trigger 对象）：
//   Once        : 触发后=耗尽；再返回同一对象 ⇒ fail；要重复 ⇒ 新对象。
//   Reenter     : last_inside 跨 arm 保留；重新 arm 需先有效“外”再“内”；maxFires 续计。
//   AfterDelay  : 重新 arm 计时从新 arm 开始；maxFires 续计。
//   AtReadPoint : 重新 arm 从下一个匹配读点重新计数；maxFires 续计。
//   maxFires 属于对象、跨 arm 累计；耗尽后再返回同一对象 = fail；新对象才新预算。
//
// HUD 生命周期（onTrigger 改 UI 状态、render 每帧画）：
//   onTrigger 命中进屏位置 → ctx.ui.show("hud", { mode: "title" });
//   onTrigger 命中离屏位置 → ctx.ui.hide("hud");
//   render: if (ctx.ui.visible("hud")) ctx.draw.text(X, Y, "...");
//   onTrigger 不 draw；UI 状态是宿主本地；没有“离开区间自动 hide”的负向边沿。
//   ⇒ v2 等价写法：tick handler 只改模块数据，render(rctx) 读数据 + rctx.draw / rctx.ui。
// ---------------------------------------------------------------------------
//
// 注意：sample/screenshot 不在这里——它们是 emu-ctl 的控制面组合命令，插件回调不可调用。
// ---------------------------------------------------------------------------
}

// ---------------------------------------------------------------------------
// S3 · 独立全局 `log`（**插件上下文**的全局；不是 `@amayui/lift-plugin` 的导出）
//
// ★ 与 `ctx.log` 是同一份宿主实现（`PluginCallHost::log`）：一行一条、有界、连续去重；
//   `data` 当前**不进日志行**（结构化数据请先 `String(...)` 再传 `message`）。
// ★ 可用范围：`start` / `onTrigger`、tick handler、`render`、timeout/pump 的 continuation
//   —— 都不需要先从 `ctx` 上拿它。
// ★ `message` 非字符串 ⇒ 先 `String(message)`（所以 `log.info(42)` 打 `42`）。
// ★ 它是**不可写 / 不可配置**的全局：module 里 `log = …` 在 strict 下 TypeError；
//   要换名字用 `const log = …`（模块作用域 shadow，不影响本全局 / 不泄漏给别的模块）。
// ⚠ 只装在**插件装载路径**（`--plugin` 的 ES module）；经典 HUD / 内部 hud 回退脚本
//   （`QuickJsRuntime::load`）里 `typeof log === "undefined"` —— 类型声明只描述插件上下文。
// ---------------------------------------------------------------------------
interface LogGlobal {
    info(message: unknown, data?: unknown): void;
    warn(message: unknown, data?: unknown): void;
    error(message: unknown, data?: unknown): void;
    debug(message: unknown, data?: unknown): void;
}
declare const log: LogGlobal;

