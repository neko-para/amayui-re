// 合同 — plugin API v2（最终形态；v1 兼容层已于 2026-11-28 删除）。
// 模块 specifier: "@amayui/lift-plugin"。
// 插件文件 = ES module；`export default { render?, start? } satisfies PluginModule`。
// 宿主 QJS 装载后读 default 导出；`onTrigger` / `initialState` 装载期即报错。
//
// 核心语义（先读这 7 条）：
// 1. `start(ctx)` 只做注册；Run 的存活 = 没有待推的异步工作（task / once / waitAny / delay / at）。
// 2. `ctx.at` / `ctx.once` / `ctx.task` / `ctx.delay` / `ctx.waitAny` / `ctx.off` / `ctx.finish` /
//    `ctx.quit` / `ctx.fail` / `ctx.profile` / `ctx.config` / `ctx.appdata` 的入口是 **Run 侧入口**
//    （Run 回调 / task continuation / 泵）；render 里 TypeError。
// 3. 指令点回调 / `once` 的 resolve 给一个 `Tick`：`now` / `local` / `edit` / `input` / `patch`。
//    Tick 只在产生它的那次入口内有效（跨入口 `local`/`edit`/`input`/`patch` ⇒ stale_tick）。
// 4. `ctx.edit` / `ctx.input`（Run 回调 / task continuation 里）与 `tick.edit` / `tick.input`
//    （tick handler 里）是同一个宿主方法；render 里都不做 guest 写。
// 5. `ctx.fail` = 当前 Run 失败；`ctx.finish` = 结束 Run + 记 exit（**不改进程 exit**）；
//    `ctx.quit` = 请求宿主停机。
// 6. `render(rctx)` 只画：`rctx = { draw, ui, config, game }`（⛔ 没有 read/log/profile/params）。
// 7. 日志：全局 `log.info/warn/error/debug`（`ctx.log` / `tick.log` 已删除）。
declare module "@amayui/lift-plugin" {
    export type GlobalKind = "int" | "uint" | "float" | "string";
    export type GlobalValue<K extends GlobalKind = GlobalKind> = K extends "string" ? string : number;

    /** Edit/Input 的即时回执；Edit 立即 applied，Input 只有 queued。 */
    export interface Receipt {
        readonly accepted: boolean;
        readonly queued: boolean;
        readonly applied: boolean;
        readonly code?: string;
        readonly seq?: number;
    }

    /** Input 回执带 id；后续用 `input.status(id)` 查完成。 */
    export interface InputReceipt extends Receipt {
        readonly id: number;
    }
    export type InputState = "queued" | "applied" | "failed" | "unknown";
    export type MouseButton = "left" | "right" | "middle";

    /** 坐标统一为 1280×720 参考系；宿主按窗口缩放。 */
    export interface ScriptPos {
        readonly script: string;
        readonly pos?: number;
        readonly valid: boolean;
    }
    export interface FrameInfo {
        readonly index: number;
        readonly script: string;
        readonly top: boolean;
        readonly pos?: number;
        readonly hash?: number;
        readonly presented?: boolean;
    }

    /** EngineRead：同步、无暂停、便宜（RunCtx 只有 global.get；RenderCtx 没有 read）。 */
    export interface ReadApi {
        readonly global: {
            get<K extends GlobalKind>(kind: K, id: number): GlobalValue<K>;
        };
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
        status(id: number): InputState;
    }

    /** `ctx.at` / `ctx.once` / `ctx.waitAny` 的 where（五种形态）。 */
    export type AtWhere =
        | { readonly script: string; readonly pos: number }
        | { readonly script: string; readonly lo: number; readonly hi: number }
        | { readonly script: string }
        | { readonly opcode: number }
        | { readonly script: string; readonly opcode: number };

    /** `ctx.once` 的选项：`timeoutMs` 必填且 > 0（到期 = 失败截止，reject + fail Run）。 */
    export interface OnceOptions {
        readonly timeoutMs: number;
    }
    /** `ctx.waitAny` 的选项：`timeoutMs` 必填且 > 0。 */
    export interface WaitAnyOptions {
        readonly timeoutMs: number;
    }
    export interface WaitAnyResult {
        readonly index: number;
        readonly tick: Tick;
    }

    /** `Tick.now` = 本拍快照；读不到 opcode ⇒ 缺键。 */
    export interface TickNow {
        readonly script: string;
        readonly pos: number;
        readonly opcode?: number;
    }
    /** `Tick.local`：活读 guest local；越界 / 指针 0 / 没有 key ⇒ undefined。 */
    export interface TickLocal {
        int(index: number): number | undefined;
        ptr(index: number): number | undefined;
        ptrDword(index: number): number | undefined;
    }
    /** `tick.patch` 的目标（三选一）。 */
    export type InstrHookTarget =
        | { readonly localInt: number }
        | { readonly ptrLocal: number }
        | { readonly globalId: number };
    /** `tick.patch` 的回执（`code`：`once_tick` / `too_many_slots` / `bad_target` …）。 */
    export interface InstrHookStats {
        readonly id: number;
        readonly script: string;
        readonly pos: number;
        readonly kind: string;
        readonly form: string;
        readonly lo: number;
        readonly hi: number;
        readonly opcode: number;
        readonly patch: boolean;
        readonly hits: number;
        readonly restores: number;
        readonly restore_conflicts: number;
        readonly skips: number;
    }

    /** 指令点回调 / `once` resolve 的 Tick。 */
    export interface Tick {
        /** 链里第一条订阅的句柄（要自己的 id 就在 `at` 时存返回值）。 */
        readonly id: number;
        readonly now: TickNow;
        readonly local: TickLocal;
        readonly edit: EditApi;
        readonly input: InputApi;
        /** 本拍 one-shot 覆盖（下一拍按 CAS 恢复；`once` 的 Tick 回 `once_tick`）。 */
        patch(target: InstrHookTarget, value: number): Receipt;
        /** deprecated 平铺（与 `now` 同值；新代码用 `tick.now`）。 */
        readonly script: string;
        readonly pos: number;
        readonly opcode?: number;
    }

    /** `ctx.profile.begin/end`：宿主 profile 桥（RunCtx 的 `end` 返回 void）。 */
    export interface ProfileApi {
        begin(label: string): void;
        end(): void;
    }

    export interface AppDataStat {
        readonly exists: boolean;
        readonly isFile: boolean;
        readonly isDir: boolean;
        readonly size: number;
        readonly error?: string;
    }
    export interface AppDataApi {
        readonly available: boolean;
        readonly path: string | null;
        stat(rel: string): AppDataStat;
    }

    export interface ConfigReceipt {
        readonly applied: boolean;
        readonly code?: string;
    }
    export interface ConfigDomain {
        get(key: string, fallback?: unknown): unknown;
        has(key: string): boolean;
        set(key: string, value: unknown): ConfigReceipt;
        delete(key: string): boolean;
        all(): Record<string, unknown>;
    }
    export interface ConfigApi {
        readonly available: boolean;
        domain(id: string): ConfigDomain;
    }

    /** RunCtx：`start(ctx)` 的参数，也是 `ctx.task(async (t) => …)` 的 continuation 参数。 */
    export interface RunCtx {
        readonly edit: EditApi;
        readonly input: InputApi;
        readonly read: ReadApi;
        readonly params: Readonly<Record<string, string>>;
        readonly appdata: AppDataApi;
        readonly config: ConfigApi;
        readonly profile: ProfileApi;
        /** 一条同步订阅（同点多条 = 一条链：`next()` 继续、不调 = 截断）。 */
        at(where: AtWhere, handler: (tick: Tick, next: () => void) => void): number;
        /** 撤一条订阅（幂等：不存在 ⇒ false）。 */
        off(id: number): boolean;
        /** 单点等待；命中 resolve `Tick`，`timeoutMs` 到期 reject + fail Run。 */
        once(where: AtWhere, options: OnceOptions): Promise<Tick>;
        /** 多臂等待：第一条命中胜出，其余臂立刻撤掉。 */
        waitAny(arms: readonly AtWhere[], options: WaitAnyOptions): Promise<WaitAnyResult>;
        /** 墙钟延迟（不是 trigger；由泵推进；不 fail Run）。 */
        delay(ms: number): Promise<void>;
        /** 登记一个协程（必须返回 Promise）；settle 之后 Run 才可能收口。 */
        task(fn: (ctx: RunCtx) => Promise<unknown>): void;
        fail(name: string, reason: string): void;
        finish(exitCode: number, reason?: string): void;
        quit(reason?: string): void;
    }

    export interface DrawStyle {
        readonly fill?: string;
        readonly color?: string;
        readonly size?: number;
        readonly bold?: boolean;
        readonly key?: string;
        readonly z?: number;
        /** 以下字段运行期**不支持**（TypeError）；留在类型里只为示例展示"不支持"的写法。 */
        readonly stroke?: string;
        readonly width?: number;
        readonly alpha?: number;
        readonly align?: string;
    }
    export interface TextStyle extends DrawStyle {}
    export interface DrawApi {
        rect(x: number, y: number, w: number, h: number, style?: DrawStyle): void;
        text(x: number, y: number, text: string, style?: TextStyle): void;
        measure(text: string, style?: TextStyle): { readonly width: number; readonly height: number };
    }

    export type UiState = unknown;
    export interface UiApi {
        show(id: string, state?: UiState): void;
        hide(id: string): void;
        set(id: string, state: UiState): void;
        visible(id: string): boolean;
        get(id: string): UiState | undefined;
        readonly state: UiState;
    }

    /** RenderCtx.game：本帧读数（无源 ⇒ undefined；⛔ 不编 0）。 */
    export interface GameRead {
        readonly frame: number;
        readonly fps?: number;
        readonly script?: string;
        readonly worst?: { readonly frames: readonly FrameInfo[]; readonly ms: number };
        readonly click?: { readonly x: number; readonly y: number; readonly button?: string };
        readonly hover?: { readonly x: number; readonly y: number };
        readonly active?: boolean;
        readonly overlayKeySeq?: number;
        mouse(): { readonly x: number; readonly y: number; readonly buttons: { readonly left: boolean; readonly right: boolean; readonly middle: boolean } } | undefined;
        localFrames(): readonly FrameInfo[] | undefined;
    }

    /** RenderCtx：`render(rctx)` 的参数（**只有**这四个成员）。 */
    export interface RenderCtx {
        readonly draw: DrawApi;
        readonly ui: UiApi;
        readonly config: ConfigApi;
        readonly game: GameRead;
    }

    export interface PluginModule {
        render?(ctx: RenderCtx): void;
        start?(ctx: RunCtx): void;
    }
}

// ---------------------------------------------------------------------------
// 独立全局 `log`（**插件上下文**的全局；不是 `@amayui/lift-plugin` 的导出）
//
// ★ 与其余日志出口是同一份宿主实现（一行一条、有界、连续去重）；`data` 当前不进日志行。
// ★ 可用范围：start / task continuation / at handler / render / 泵。
// ★ `message` 非字符串 ⇒ 先 `String(message)`（所以 `log.info(42)` 打 `42`）。
// ★ 只装在**插件装载路径**（`--plugin` 的 ES module）。
// ---------------------------------------------------------------------------
interface LogGlobal {
    info(message: unknown, data?: unknown): void;
    warn(message: unknown, data?: unknown): void;
    error(message: unknown, data?: unknown): void;
    debug(message: unknown, data?: unknown): void;
}
declare const log: LogGlobal;
