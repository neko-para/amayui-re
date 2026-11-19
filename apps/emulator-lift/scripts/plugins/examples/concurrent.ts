// 并发 watch：一次 arm 三个具名 Trigger，onTrigger 按 ev.name 分派
// （第五轮，v5.3.2）。
//
// 语义（v5.3.2）：
//   - 返回数组 = **完整的新 armed 集合（替换）**；刚触发的那个不会自动保留；
//   - 想保留其它 trigger，就把**同一批对象**再放进返回数组；
//   - null / [] = done；一个读点最多触发一个（注册顺序）；重复 name = fail。
//
// 本文件是"全部保留"的版本（三个都持久）；"触发后丢掉一次性 trigger"见 keep-others.ts。

import type {
    FiredEvent,
    PluginModule,
    RenderCtx,
    RunCtx,
    TimeoutEvent,
    Trigger,
} from "@amayui/lift-plugin";

const HUD_ID = "hud";

const A = { script: "TITLE", lo: 0x00000000, hi: 0x00000000 }; // 占位：hud:show
const B = { script: "FIELD", lo: 0x00000000, hi: 0x00000000 }; // 占位：hud:hide
const C = { script: "SC0000", lo: 0x00000000, hi: 0x00000000 }; // 占位：edit:flag

const N = {
    show: "hud:show",
    hide: "hud:hide",
    flag: "edit:flag",
} as const;

const GLOBAL_ID = 0; // 占位
const SET_BITS = 0; // 占位

const HUD_X = 0; // 占位
const HUD_Y = 0;
const HUD_W = 0;
const HUD_H = 0;
const HUD_FILL = ""; // 占位：颜色串

// Trigger 需要 ctx 才能创建 ⇒ initialState 只能留空；首读 undefined，必须 guard。
interface ConcurrentState {
    show?: Trigger;
    hide?: Trigger;
    flag?: Trigger;
}

function makeTriggers(ctx: RunCtx<ConcurrentState>): [Trigger, Trigger, Trigger] {
    return [
        ctx.trigger.afterPosition(A.script, A.lo, A.hi, {
            name: N.show,
            mode: "reenter",
        }),
        ctx.trigger.afterPosition(B.script, B.lo, B.hi, {
            name: N.hide,
            mode: "reenter",
        }),
        ctx.trigger.afterPosition(C.script, C.lo, C.hi, {
            name: N.flag,
            mode: "reenter",
        }),
    ];
}

function logArmed(ctx: RunCtx<ConcurrentState>): void {
    // v5.3.2：armedNames 与 armed 一一对应，两者都必填。
    const cur = ctx.run.current();
    ctx.log("debug", "concurrent: armed", {
        names: cur?.armedNames ?? [],
        count: cur?.armed.length ?? 0,
    });
}

function start(ctx: RunCtx<ConcurrentState>): Trigger[] {
    const [show, hide, flag] = makeTriggers(ctx);
    ctx.state.show = show;
    ctx.state.hide = hide;
    ctx.state.flag = flag;
    return [show, hide, flag];
}

function onTrigger(
    ctx: RunCtx<ConcurrentState>,
    ev: FiredEvent | TimeoutEvent,
): Trigger[] | null {
    if (ev.kind === "timeout") {
        ctx.ui.hide(HUD_ID);
        ctx.finish(5, `concurrent: gate timeout (${ev.name})`);
        return null;
    }

    const { show, hide, flag } = ctx.state;
    if (show === undefined || hide === undefined || flag === undefined) {
        ctx.fail(undefined, "concurrent: state not initialized");
        return null;
    }

    logArmed(ctx);

    switch (ev.name) {
        case N.show:
            ctx.ui.show(HUD_ID, { mode: "title" });
            break;

        case N.hide:
            ctx.ui.hide(HUD_ID);
            break;

        case N.flag: {
            const before = ctx.read.global.get("int", GLOBAL_ID);
            ctx.edit.global.set("int", GLOBAL_ID, before | SET_BITS);
            break;
        }

        default:
            ctx.fail(ev.name, "unknown state");
            return null;
    }

    // 三个都保留（同一批对象）⇒ 返回完整的新 armed 集合。
    return [show, hide, flag];
}

// HUD 本体由 render 画；onTrigger 只改 UI 状态。
function render(ctx: RenderCtx): void {
    if (!ctx.ui.visible(HUD_ID)) return;
    ctx.draw.rect(HUD_X, HUD_Y, HUD_W, HUD_H, { fill: HUD_FILL });
}

const plugin = {
    initialState: {},
    render,
    start,
    onTrigger,
} satisfies PluginModule<ConcurrentState>;

export default plugin;
