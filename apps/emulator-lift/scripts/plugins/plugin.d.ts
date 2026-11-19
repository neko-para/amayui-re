// v1 / 2026-11-19 / 合同 — plugin API 合同；后续变更需显式修订版本。
// 模块 specifier: "@amayui/lift-plugin"。
// 插件文件 = ES module；`export default { render?, start?, onTrigger?, initialState? } satisfies PluginModule<State>`。
// 宿主 QJS 装载后读 default 导出；不再读 globalThis.render/start/onTrigger。
//
// 核心语义（先读这 8 条）：
// 1. Edit（ctx.edit）在触发点/安全点立即同步写，不排队；暂停时也可执行。
// 2. Input（ctx.input）只入队，在 guest 读点按“一读点最多一条”串行施加；click 只保证 queued。
// 3. EngineRead（ctx.read）同步只读、不暂停、便宜；sample/screenshot 不在这里。
// 4. Trigger（ctx.trigger）具名；start/onTrigger 返回 Trigger|Trigger[]|null；数组=完整替换 armed 集合；
//    一个读点最多触发一个；null/[]=done；重复 name=fail。
// 5. UI（ctx.ui）只改宿主本地状态；onTrigger 不画，render 用 ctx.draw 画；下一次 render 才可见。
// 6. render 也可写 ctx.ui（hover/动画），但只影响 UI 状态；render 不做 guest 写。
// 7. Run（ctx.run）是一次 callback 链的执行实例；一个 endpoint 同时一个 active，已有 ⇒ busy。
// 8. ctx.finish 结束当前 Run（可映射旧 exit 5）；ctx.quit 请求宿主停机（WM_QUIT/stop_requested）。
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

/** 只比当前脚本名（电平门；无区间）；等价旧 `await script NAME`。 */
export interface AfterScriptTrigger {
    readonly name: string;   // 状态名
    readonly kind: "afterScript";
    readonly script: string;
    readonly mode: TriggerMode;
    readonly timeoutMs?: number;
    readonly maxFires?: number;
}
export interface AfterPositionTrigger {
    readonly name: string;
    readonly kind: "afterPosition";
    readonly script: string;
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
}
export interface TextStyle extends DrawStyle {
    readonly size?: number;
    readonly align?: "left" | "center" | "right";
}

/**
 * 只在 render 里用；发本帧 display list。
 * - 坐标 = 1280×720 逻辑系；宿主按窗口缩放。
 * - z 序 = 调用顺序（后调用在上）。
 * - 每帧开始清空；render 返回后提交本次 list。
 * - text.style.size/align/fill 同 draw；measure 用同一套 style 估算。
 */
export interface DrawApi {
    rect(x: number, y: number, w: number, h: number, style?: DrawStyle): void;
    text(x: number, y: number, text: string, style?: TextStyle): void;
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
export interface ProfileApi {
    begin(label: string): void;
    end(): void;
}
export type LogLevel = "debug" | "info" | "warn" | "error";
export type LogApi = (level: LogLevel, message: string, data?: unknown) => void;

/** render 只拿当帧 readings；不阻塞、不做 guest 写；按钮可 dispatch。
 *
 * ★ C2b 状态：`mouse()` / `localFrames()` 已实现（当帧 `UiFrameReadings`；**读数没有值 ⇒ `undefined`**，
 *   ⛔ 不编 0 / 空数组）。类型上必须能表达这一点（`c2b1` 复审 F1）。
 *   ⚠ `mouse().buttons`（三键电平）在 C2 的读数里**没有源** ⇒ 实现**不安装**这个键、类型是**可选**
 *   （⛔ 不编 `false`：那会让"鼠标按着"与"没有源"长得一样；C2b-2 的窗口装配 / C4 再接）。
 */
export interface GameRead {
    /** 没有源（`readings.frames == null`）/ 宿主报错 / 空表 ⇒ `undefined`（⛔ 不编空数组）。 */
    localFrames(): readonly FrameInfo[] | undefined;
    /** 这一帧没有鼠标读数 ⇒ `undefined`（⛔ 不编 `{x:0,y:0}`）。 */
    mouse():
        | {
            readonly x: number;
            readonly y: number;
            /** C2 没有按键电平源 ⇒ 缺键（见上）。 */
            readonly buttons?: {
                readonly left: boolean;
                readonly right: boolean;
                readonly middle: boolean;
            };
        }
        | undefined;
}
/** render 只拿当帧 readings；模块级变量是 per-plugin 状态（render/onTrigger 共享）。
 *
 * ★ C2b 实现面：`ui` / `draw` / `read` / `game` / `params` / `log`（`Object.keys(ctx)` **恰好**这六个）。
 *   ⛔ 没有 `edit` / `input`（RunCtx 的写面；render 不做 guest 写）、`state` / `now` / `trigger` /
 *   `profile` / `fail` / `finish` / `quit`（Run 语境），也⛔ 没有 `sample` / `screenshot`。
 */
export interface RenderCtx {
    readonly ui: UiApi;          // render 可写 hover/动画状态
    readonly draw: DrawApi;      // 本帧 display list
    readonly read: ReadApi;
    readonly game: GameRead;
    /** ⚠ **C5**（C2b 里是 `undefined`；⛔ 不是"有但抛"）。 */
    readonly run: RunApi;
    readonly params: Readonly<Record<string, string>>;
    readonly log: LogApi;
}

/** `ctx.now`：**本读点的读数快照**（C2a；与 `FiredEvent`/`TimeoutEvent` 的 `TriggerEventBase` 同源）。
 *
 * ★ **只在本回调内有效**（D-C2-7）：宿主**每次回调新建**这个值对象 ⇒ ⛔ 不要把它存进模块变量
 *   （存了以后下一次回调读到的是"那次回调的值"，而不是当前读点）。
 * ★ 有效才给值：`script` / `pos` / `mouse` 只在宿主那一边真的读到的时候出现（⛔ 不编 0 / 空串）；
 *   `posValid` / `readIndex` / `point` 恒有（`posValid=false` 表示"位置暂时读不到"，不要当成 0）。
 */
export interface RunNow {
    readonly script?: string;
    readonly pos?: number;
    readonly posValid: boolean;
    readonly readIndex: number;
    readonly point: InputPoint;
    readonly mouse?: { readonly x: number; readonly y: number };
}

/**
 * RunCtx：一次 Run 的 callback 上下文；Edit/Input/Read/Trigger/UI/Run。
 *
 * ★ 切片状态（C2a，2026-11-20；C2b 追加注释）：
 *   - 已实现：`edit` / `input` / `read.global.get` / `trigger` / `ui`（**只写**宿主本地状态）/
 *     `profile` / `state` / `params` / `log` / `fail` / `finish` / `quit` / `now`；
 *   - ⛔ **`RunCtx` 永远没有** `draw`（C2b 只把它装在 `RenderCtx` 上；`onTrigger` 不画）/
 *     `run` / `dispatch`（C5）——`ctx.run` 在 C2 是 `undefined`（⛔ 不是"有但抛"）；
 *     也不安装 `sample` / `screenshot`（永远不做）。
 */
export interface RunCtx<State = Record<string, unknown>> {
    readonly edit: EditApi;
    readonly input: InputApi;
    readonly read: ReadApi;
    readonly trigger: TriggerApi;
    readonly ui: UiApi;
    /** ⚠ **C5**（C2a 里是 `undefined`）。Run 的 status/abort/events/dispatch。 */
    readonly run: RunApi;
    readonly profile: ProfileApi;
    /** per-Run；只放数据；不跨 endpoint/Run 共享。宿主建它时对 `initialState` 做**浅拷贝**（见下）。 */
    readonly state: State;
    readonly params: Readonly<Record<string, string>>;
    /** `ctx.log(level, message, data?)`：**一行一条、有界、去重**；`data` 当前**不进日志行**
     *  （结构化数据请先 `String(...)` 再传 message）。 */
    readonly log: LogApi;
    /** **本读点的读数快照**；每次回调新建、只在本回调内有效（⛔ 不要存进模块变量）。 */
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
export interface PluginModule<State = Record<string, unknown>> {
    /** 宿主在 start 之前初始化 ctx.state；省略 ⇒ {} as State。
     *  ★ **浅拷贝**（`Object.assign({}, initialState)`，D-C2-7）：沙箱里没有 `JSON` / `structuredClone`
     *    ⇒ 嵌套对象**不**深拷贝，也不承诺"同 runtime 两个 Run 共享/不共享"（C2 只有一个 Run）。
     */
    initialState?: State;
    /** 可选；有它就每帧跑（C2b：传 `RenderCtx`；没有窗口 ⇒ 永不调用 + 一次性 stderr，D-C2-5）。 */
    render?: (ctx: RenderCtx) => void;
    start?: (ctx: RunCtx<State>) => Trigger | Trigger[] | null;
    onTrigger?: (ctx: RunCtx<State>, ev: FiredEvent | TimeoutEvent) => Trigger | Trigger[] | null;
}

// ---------------------------------------------------------------------------
// 典型用法（伪代码；不写具体数值）
//
// 加载存档的链（name = 状态名；不用 step 计数器 / ctx.state）：
//   const loadSlot = (slot: number) => ({
//     start(ctx: RunCtx): Trigger | Trigger[] | null {
//       ctx.input.click(X, Y);
//       return ctx.trigger.afterScript("TITLE", { name: "load:title" });
//     },
//     onTrigger(ctx: RunCtx, ev: FiredEvent | TimeoutEvent): Trigger | Trigger[] | null {
//       switch (ev.name) {
//         case "load:title":  ctx.input.move(X2, rowY(slot));
//                             return ctx.trigger.atReadPoint("any", 1, { name: "load:row" });
//         case "load:row":    ctx.input.click(X2, rowY(slot));
//                             return ctx.trigger.afterPosition("SBUNKI", C, D, { name: "load:sbunki" });
//         case "load:sbunki": ctx.input.click(X3, Y3); return [];
//         default:            ctx.fail(ev.name, `unknown name: ${ev.name}`); return [];
//       }
//     },
//   });
//   const start = (ctx: RunCtx<MyState>) => loadSlot(Number(ctx.params.slot)).start(ctx);
//   const onTrigger = (ctx: RunCtx<MyState>, ev: FiredEvent | TimeoutEvent) =>
//       loadSlot(Number(ctx.params.slot)).onTrigger(ctx, ev);
//
// 重新 arm 语义表（四种）：
//   Once        : 触发后=耗尽；再返回同一对象 ⇒ fail（与 maxFires 耗尽同档）；要重复 ⇒ 新对象。
//   Reenter     : last_inside 跨 arm 保留；重新 arm 需先有效“外”再“内”，不会立刻再触发；maxFires 续计。
//   AfterDelay  : 重新 arm 计时从新 arm 开始；maxFires 续计。
//   AtReadPoint : 重新 arm 从下一个匹配读点重新计数；maxFires 续计。
//   maxFires 耗尽后再返回同一对象一律 fail；新对象才有新预算/新边沿。
//
// 返回语义：数组是**完整的新 armed 集合（替换）**；刚触发的那个不会自动保留；
// 想保留其它 trigger 就把它们再放进返回数组。`null`/`[]` = done；重复 name = fail。
// maxFires 属于对象、跨 arm 累计；同一对象重新 arm 续计，耗尽后再返回 = fail；新对象才新预算。
//
// 模块装载 + 泛型 state（推荐 satisfies，不用 definePlugin）：
//   import type { PluginModule, RunCtx, FiredEvent, TimeoutEvent } from "@amayui/lift-plugin";
//   interface MyState { flags: Record<string, boolean> }
//   const plugin = {
//     initialState: { flags: {} } as MyState,
//     render, start, onTrigger,
//   } satisfies PluginModule<MyState>;
//   export default plugin;   // 宿主读 default 导出；不再读命名导出/globalThis
//
// read → edit（同一触发点，读旧值/写新值由调用顺序决定）：
//   const v = ctx.read.global.get("int", ID);
//   ctx.edit.global.set("int", ID, v | MASK);
//
// HUD 生命周期（进屏 show、另一个正向位置 hide、render 每帧画）：
//   onTrigger 命中 TITLE 区间 → ctx.ui.show("hud", { mode: "title" });
//   onTrigger 命中 FIELD 区间 → ctx.ui.set("hud", { mode: "field" });
//   onTrigger 命中另一个已知位置 B → ctx.ui.hide("hud");
//   render: if (ctx.ui.visible("hud")) ctx.draw.text(X, Y, "...");
//   onTrigger 不 draw；UI 状态是宿主本地；没有“离开区间自动 hide”的负向边沿。
//
// 只应在某些屏出现的 UI：用正向 afterPosition/afterScript 的 show/hide，
// 或插件内自己包 visibleWhen；不要新增核心 visibleWhen DSL。
//
// 并发 watch（一个 Run 一次 arm 多个具名 trigger；onTrigger 按 ev.name 分派）：
//   return [
//     ctx.trigger.afterPosition("A", A0, A1, { name: "hud:show" }),
//     ctx.trigger.afterPosition("B", B0, B1, { name: "hud:hide" }),
//     ctx.trigger.afterPosition("C", C0, C1, { name: "edit:flag" }),
//   ];
//   onTrigger: switch (ev.name) { case "hud:show": ...; case "hud:hide": ...; case "edit:flag": ...; }
//   一个读点最多触发一个（按注册顺序）；其余不自动保留（要保留就放进返回数组）。
//
// 注意：sample/screenshot 不在这里——它们是 emu-ctl 的控制面组合命令，插件回调不可调用。
// ---------------------------------------------------------------------------
}
