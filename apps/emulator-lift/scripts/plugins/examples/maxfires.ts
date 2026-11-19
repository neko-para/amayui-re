// 同一 Trigger 对象跨 arm 累计 maxFires（第五轮，v5.3.2）。
//
// 语义（v5.3.2）：
//   - maxFires 属于**对象**、跨 arm 累计；同一对象重新 arm 继续扣预算；
//   - 耗尽后再返回同一对象 = 宿主 fail；新对象才新预算。
//
// ⛔ 数值全是占位。

import type {
    FiredEvent,
    PluginModule,
    RunCtx,
    TimeoutEvent,
    Trigger,
} from "@amayui/lift-plugin";

const NAME = "maxfires:pos";
const SCRIPT = "SC0000";
const POS_LO = 0x00000000;
const POS_HI = 0x00000000;

const MAX_FIRES = 2;

interface MaxState {
    trigger?: Trigger;
    fires: number;
}

function start(ctx: RunCtx<MaxState>): Trigger {
    const trigger = ctx.trigger.afterPosition(SCRIPT, POS_LO, POS_HI, {
        name: NAME,
        mode: "reenter",
        maxFires: MAX_FIRES, // 同一对象的预算，跨 arm 累计
    });
    ctx.state.trigger = trigger;
    return trigger;
}

function onTrigger(ctx: RunCtx<MaxState>, ev: FiredEvent | TimeoutEvent): Trigger | null {
    if (ev.kind === "timeout") {
        ctx.finish(5, `maxfires: gate timeout (${ev.name})`);
        return null;
    }

    const trigger = ctx.state.trigger;
    if (trigger === undefined) {
        ctx.fail(undefined, "maxfires: state not initialized");
        return null;
    }

    switch (ev.name) {
        case NAME: {
            // fires 由 initialState 给 0，首读即有值。
            ctx.state.fires += 1;

            if (ctx.state.fires < MAX_FIRES) {
                ctx.log("info", "maxfires: re-arm same object", {
                    fires: ctx.state.fires,
                    maxFires: MAX_FIRES,
                });
                // 同一对象 ⇒ 预算继续扣，不会重置。
                return trigger;
            }

            ctx.log("info", "maxfires: exhausted", {
                fires: ctx.state.fires,
                maxFires: MAX_FIRES,
            });

            // 耗尽后再 `return trigger` **类型上合法**（它仍是 Trigger），
            // 但宿主会 fail；类型上没有"已耗尽"的标记。插件只能自己记 fires。
            // 反例（保留说明）：return trigger;
            //
            // 想要新预算：重新 afterPosition(..., { maxFires: 2 }) 造一个新对象；
            // 旧对象此时已被宿主摘掉，同名新对象不会与它重复。
            return null;
        }

        default:
            ctx.fail(ev.name, "unknown state");
            return null;
    }
}

const plugin = {
    initialState: { fires: 0 },
    start,
    onTrigger,
} satisfies PluginModule<MaxState>;

export default plugin;
