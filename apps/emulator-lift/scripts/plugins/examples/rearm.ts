// 重 arm 语义表（v5.3.2）的活例子：
//   Once        : 触发后=耗尽；再返回同一对象 ⇒ 运行期 fail；要重复 ⇒ 新对象。
//   Reenter     : last_inside 跨 arm 保留；重新 arm 需先有效“外”再“内”，不会立刻再触发；
//                 maxFires 续计。
//   maxFires 耗尽后再返回同一对象一律 fail；新对象才有新预算。
//
// 类型上无法表达"已耗尽"：Once 的旧对象仍是 Trigger，`return [oldOnce, reenter]` 照样过 tsc，
// 只有宿主会 fail。下面用两种正确写法绕开：
//   - Once 重复 → 每次触发后造**新对象**；
//   - Reenter 耗尽 → 从返回数组里**去掉**旧对象。
//
// ⛔ 数值全是占位。

import type {
    FiredEvent,
    PluginModule,
    RunCtx,
    TimeoutEvent,
    Trigger,
} from "@amayui/lift-plugin";

const ONCE = "rearm:once";
const REENTER = "rearm:reenter";

const ONCE_SCRIPT = "TITLE";
const REENTER_SCRIPT = "FIELD";
const POS_LO = 0x00000000;
const POS_HI = 0x00000000;

const REENTER_MAX_FIRES = 2;

interface RearmState {
    once?: Trigger;
    reenter?: Trigger;
    onceFires: number;
    reenterFires: number;
    reenterDone: boolean;
}

function makeOnce(ctx: RunCtx<RearmState>): Trigger {
    return ctx.trigger.afterPosition(ONCE_SCRIPT, POS_LO, POS_HI, {
        name: ONCE,
        mode: "once",
    });
}

function makeReenter(ctx: RunCtx<RearmState>): Trigger {
    return ctx.trigger.afterPosition(REENTER_SCRIPT, POS_LO, POS_HI, {
        name: REENTER,
        mode: "reenter",
        maxFires: REENTER_MAX_FIRES,
    });
}

function start(ctx: RunCtx<RearmState>): Trigger[] {
    const once = makeOnce(ctx);
    const reenter = makeReenter(ctx);
    ctx.state.once = once;
    ctx.state.reenter = reenter;
    // onceFires / reenterFires 由 initialState 给 0，首读即有值。
    return [once, reenter];
}

function onTrigger(ctx: RunCtx<RearmState>, ev: FiredEvent | TimeoutEvent): Trigger[] | null {
    if (ev.kind === "timeout") {
        ctx.finish(5, `rearm: gate timeout (${ev.name})`);
        return null;
    }

    const { once, reenter } = ctx.state;
    if (once === undefined || reenter === undefined) {
        ctx.fail(undefined, "rearm: state not initialized");
        return null;
    }

    switch (ev.name) {
        case ONCE: {
            ctx.state.onceFires += 1;

            // Once 触发后旧对象已耗尽；**不能**把旧对象再放进返回数组：
            //   return [once, reenter];   // 类型合法，运行期 fail
            // 要再来一次，只能造新对象（新边沿/新预算）。
            const next = makeOnce(ctx);
            ctx.state.once = next;
            // Reenter 可能已经耗尽并被移出集合 ⇒ 不能再把旧对象放回去（运行期 fail）。
            return ctx.state.reenterDone ? [next] : [next, reenter];
        }

        case REENTER: {
            ctx.state.reenterFires += 1;

            if (ctx.state.reenterFires < REENTER_MAX_FIRES) {
                // 同一对象重新 arm：last_inside 跨 arm 保留 ⇒ 必须先出区间再进，
                // 否则不会立刻再触发；maxFires 继续扣。
                return [once, reenter];
            }

            // maxFires 耗尽：同一对象再返回 = 运行期 fail；从集合里去掉（保留 Once）。
            ctx.state.reenterDone = true;
            return [once];
        }

        default:
            ctx.fail(ev.name, "unknown state");
            return null;
    }
}

const plugin = {
    initialState: { onceFires: 0, reenterFires: 0, reenterDone: false },
    start,
    onTrigger,
} satisfies PluginModule<RearmState>;

export default plugin;
