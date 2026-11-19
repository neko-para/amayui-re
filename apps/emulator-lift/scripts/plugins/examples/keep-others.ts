// 新用例：一次 arm 3 个具名 Trigger；某个触发后，返回数组里**重新包含另外两个**
// （保留它们），不包含已触发的一次性 trigger（第五轮，v5.3.2）。
//
// ⛔ 数值全是占位。
// 这是"完整替换 armed 集合"语义的关键写法：刚触发的那个不会自动保留。

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

interface KeepState {
    show?: Trigger;
    hide?: Trigger;
    flag?: Trigger;
    flagDone: boolean;
}

function makeTriggers(ctx: RunCtx<KeepState>): [Trigger, Trigger, Trigger] {
    return [
        ctx.trigger.afterPosition(A.script, A.lo, A.hi, {
            name: N.show,
            mode: "reenter",
        }),
        ctx.trigger.afterPosition(B.script, B.lo, B.hi, {
            name: N.hide,
            mode: "reenter",
        }),
        // 一次性：maxFires 属于对象、跨 arm 累计；1 次后耗尽。
        // 耗尽后再把这个对象放进返回数组 = 宿主 fail ⇒ 必须从集合里去掉。
        ctx.trigger.afterPosition(C.script, C.lo, C.hi, {
            name: N.flag,
            mode: "reenter",
            maxFires: 1,
        }),
    ];
}

/** 返回"保留下来的"两个 + 尚未触发的一次性 trigger（如果还没触发）。 */
function keepOthers(ctx: RunCtx<KeepState>): Trigger[] | null {
    const { show, hide, flag, flagDone } = ctx.state;
    if (show === undefined || hide === undefined) return null;

    const set: Trigger[] = [show, hide]; // 同一批对象 ⇒ 保留
    if (!flagDone) {
        if (flag === undefined) return null;
        set.push(flag);
    }
    return set;
}

function start(ctx: RunCtx<KeepState>): Trigger[] {
    const [show, hide, flag] = makeTriggers(ctx);
    ctx.state.show = show;
    ctx.state.hide = hide;
    ctx.state.flag = flag;
    // flagDone 由 initialState 给 false，首读即有值。
    return [show, hide, flag];
}

function onTrigger(ctx: RunCtx<KeepState>, ev: FiredEvent | TimeoutEvent): Trigger[] | null {
    if (ev.kind === "timeout") {
        ctx.ui.hide(HUD_ID);
        ctx.finish(5, `keep-others: gate timeout (${ev.name})`);
        return null;
    }

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
            ctx.state.flagDone = true; // 一次性已用完；keepOthers 不再包含它
            break;
        }

        default:
            ctx.fail(ev.name, "unknown state");
            return null;
    }

    const next = keepOthers(ctx);
    if (next === null) {
        ctx.fail(undefined, "keep-others: state not initialized");
        return null;
    }
    return next;
}

function render(ctx: RenderCtx): void {
    if (!ctx.ui.visible(HUD_ID)) return;
    ctx.draw.rect(HUD_X, HUD_Y, HUD_W, HUD_H, { fill: HUD_FILL });
}

const plugin = {
    initialState: { flagDone: false },
    render,
    start,
    onTrigger,
} satisfies PluginModule<KeepState>;

export default plugin;
