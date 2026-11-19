// 典型用法：在某个 script.pos 触发时 global read → modify → write（第五轮，v5.3.2）。
//
// ⛔ 数值全是占位；不写具体 global id / 掩码。
// v5.3.2：默认导出 PluginModule；initialState 只放数据；Trigger 对象由 start 建。

import type {
    FiredEvent,
    GlobalKind,
    GlobalValue,
    PluginModule,
    RunCtx,
    TimeoutEvent,
    Trigger,
} from "@amayui/lift-plugin";

const NAME = "global:pos"; // 具名状态
const SCRIPT = "SC0000"; // 占位脚本名
const POS_LO = 0x00000000; // 占位：触发区间起点（半开）
const POS_HI = 0x00000000; // 占位：触发区间终点

const GLOBAL_ID = 0; // 占位：global 下标
const SET_BITS = 0; // 占位：要置的位
const CLEAR_BITS = 0; // 占位：要清的位

interface GlobalState {
    // Trigger 需要 ctx 才能创建 ⇒ 不能放进静态 initialState；只能 start 里写、onTrigger 里 guard。
    watch?: Trigger;
}

/** 泛型 helper：get<K extends GlobalKind> 可用。 */
function readAs<K extends GlobalKind>(
    ctx: RunCtx<GlobalState>,
    kind: K,
    id: number,
): GlobalValue<K> {
    return ctx.read.global.get(kind, id);
}

function makeTrigger(ctx: RunCtx<GlobalState>): Trigger {
    return ctx.trigger.afterPosition(SCRIPT, POS_LO, POS_HI, {
        name: NAME,
        mode: "reenter",
    });
}

function start(ctx: RunCtx<GlobalState>): Trigger {
    // 同一对象存进 state；之后每次 onTrigger 原样返回 = 重新 arm 同一个对象。
    const watch = makeTrigger(ctx);
    ctx.state.watch = watch;
    return watch;
}

function onTrigger(ctx: RunCtx<GlobalState>, ev: FiredEvent | TimeoutEvent): Trigger | null {
    if (ev.kind === "timeout") {
        ctx.finish(5, `global-edit: gate timeout (${ev.name})`);
        return null;
    }

    const watch = ctx.state.watch;
    if (watch === undefined) {
        ctx.fail(undefined, "global-edit: state not initialized");
        return null;
    }

    switch (ev.name) {
        case NAME: {
            const before = readAs(ctx, "int", GLOBAL_ID);
            const after = (before | SET_BITS) & ~CLEAR_BITS;

            const r = ctx.edit.global.set("int", GLOBAL_ID, after);
            if (!r.applied) {
                ctx.log("warn", "global write rejected", {
                    code: r.code,
                    accepted: r.accepted,
                    before,
                    after,
                });
            }
            return watch;
        }
        default:
            ctx.fail(ev.name, "unknown state");
            return null;
    }
}

const plugin = {
    initialState: {},
    start,
    onTrigger,
} satisfies PluginModule<GlobalState>;

export default plugin;
