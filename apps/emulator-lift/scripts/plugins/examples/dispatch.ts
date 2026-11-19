// 典型用法：UI 插件在按钮按下时 dispatch 一个子 Run，并在 render 里画它的状态
// （第五轮，v5.3.2）。
//
// ⛔ 数值 / 文案全是占位；不写具体坐标。
// d.ts 语义（原文）："Run 是一次 callback 链的执行实例；一个 endpoint 同时一个 active，
// 已有 ⇒ busy"；dispatch 同步返回，返回时 start 已跑完。

import type {
    DispatchResult,
    PluginModule,
    RenderCtx,
    RunHandle,
    RunState,
    RunStatus,
} from "@amayui/lift-plugin";

const CHILD_MODULE = "child-run"; // 占位：子 Run 的 module 名
const BUTTON_X = 0; // 占位：按钮矩形
const BUTTON_Y = 0;
const BUTTON_W = 0;
const BUTTON_H = 0;
const STATUS_X = 0; // 占位：状态文字位置
const STATUS_Y = 0;
const PARAMS: Readonly<Record<string, string>> = { slot: "0" }; // 占位

const FONT_SIZE = 0; // 占位
const BUTTON_FILL = ""; // 占位：颜色串
const BUTTON_FILL_HOVER = ""; // 占位
const BUTTON_STROKE = ""; // 占位
const TEXT_FILL = ""; // 占位
const IDLE_LABEL = ""; // 占位：空闲文案（本地化提供）

const HOVER_ID = "dispatch:hover";
const EVENT_SEQ_ID = "dispatch:eventSeq";

// d.ts:244 明确：模块级变量是 per-plugin 状态（render/onTrigger 共享）。
// 同一 module 被多个 endpoint 装载时是否共享仍没写（P2）。
let handle: RunHandle | null = null;
let mouseWasDown = false;

function hit(x: number, y: number): boolean {
    return x >= BUTTON_X && x < BUTTON_X + BUTTON_W && y >= BUTTON_Y && y < BUTTON_Y + BUTTON_H;
}

/** 一个 endpoint 只有一个 active；busy 时不要覆盖已有 handle。 */
function dispatchOnce(ctx: RenderCtx): void {
    const r: DispatchResult = ctx.run.dispatch(CHILD_MODULE, PARAMS);
    if (r.ok) {
        // DispatchResult 可判别：ok=true ⇒ busy=false 且 handle 必有。
        handle = r.handle;
        // v5.3.2：dispatch 同步，返回时 start 已跑完 ⇒ armedNames 现在就绪。
        const st = ctx.run.status(r.handle);
        ctx.log("info", "child dispatched", {
            armedNames: st.armedNames,
            armedCount: st.armed.length,
        });
        return;
    }
    if (r.busy) {
        ctx.log("info", "dispatch busy: a run is already active on this endpoint");
        return;
    }
    ctx.log("warn", "dispatch failed", { code: r.code });
}

function statusOf(handleOrNull: RunHandle | null, ctx: RenderCtx): RunStatus | null {
    return handleOrNull === null ? null : ctx.run.status(handleOrNull);
}

/** 消化 events(since)：RunEvent.seq 单调；RunEvent.name 仍为可选。 */
function drainEvents(ctx: RenderCtx): void {
    const since = ctx.ui.get<number>(EVENT_SEQ_ID) ?? 0;
    const events = ctx.run.events(since);
    for (const ev of events) {
        ctx.log("debug", `child run ${ev.type}`, {
            handle: ev.handle.id,
            plugin: ev.plugin,
            name: ev.name,
        });
    }
    const last = events.at(-1);
    if (last !== undefined) ctx.ui.set(EVENT_SEQ_ID, last.seq);
}

function render(ctx: RenderCtx): void {
    // ★ 两个"没有值"的口都要判（C2b）：`mouse()` 这一帧没有读数 ⇒ `undefined`；
    //   而 `buttons`（三键电平）在 C2 没有读数源 ⇒ **缺键**（⛔ 不编 false）。
    //   ⇒ 两者都按"全部抬起"处理，hover 也直接 false（⛔ 不拿 `{x:0,y:0}` 算命中）。
    const mouse = ctx.game.mouse();
    const down = mouse !== undefined && mouse.buttons !== undefined && mouse.buttons.left;
    const hover = mouse !== undefined && hit(mouse.x, mouse.y);
    // render 没有按下/抬起事件，只有 buttons 电平 ⇒ 自己做一个边沿。
    const pressed = down && !mouseWasDown && hover;
    mouseWasDown = down;

    if (pressed) dispatchOnce(ctx);
    drainEvents(ctx);

    const status = statusOf(handle, ctx);
    const label: string | RunState = status === null ? IDLE_LABEL : status.state;

    // render 可写 UI 状态，也可以用 ctx.draw 发本帧 display list。
    ctx.ui.set(HOVER_ID, hover);
    ctx.draw.rect(BUTTON_X, BUTTON_Y, BUTTON_W, BUTTON_H, {
        fill: hover ? BUTTON_FILL_HOVER : BUTTON_FILL,
        stroke: BUTTON_STROKE,
    });
    ctx.draw.text(STATUS_X, STATUS_Y, label, { size: FONT_SIZE, fill: TEXT_FILL });

    // 一个 endpoint 一个 active：current() 给的是"本插件当前 active Run"。
    const active = ctx.run.current();
    if (active !== null && active.state !== "done" && active.state !== "failed" && active.state !== "aborted") {
        ctx.log("debug", "child run still active", {
            handle: active.handle.id,
            armedNames: active.armedNames,
            armedCount: active.armed.length,
        });
    }
}

const plugin = { render } satisfies PluginModule;

export default plugin;
