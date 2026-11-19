// Timeout：配置 + 响应 + 超时后 re-arm 一个新对象（v5.3.2）。
//
// 配置：ctx.trigger.afterScript(..., { name, timeoutMs })。
// 响应：onTrigger 收到 ev.kind === "timeout" ⇒ 旧配方 exit 5 用 ctx.finish(5, reason) 映射。
//
// ⛔ 数值全是占位。

import type {
    FiredEvent,
    PluginModule,
    RunCtx,
    TimeoutEvent,
    Trigger,
} from "@amayui/lift-plugin";

const GATE_TITLE = "gate:title";
const GATE_RETRY = "gate:retry";
const GATE_SETTLE = "gate:settle";

const TITLE_SCRIPT = "TITLE";
const RETRY_SCRIPT = "FIELD";

const TITLE_TIMEOUT_MS = 30_000; // 占位：旧配方行尾 timeout 30000
const RETRY_TIMEOUT_MS = 5_000; // 占位：重试门的超时
const SETTLE_DELAY_MS = 500; // 占位：纯固定等待
const MAX_RETRIES = 3; // 占位

interface TimeoutState {
    retries: number;
}

function start(ctx: RunCtx<TimeoutState>): Trigger {
    return ctx.trigger.afterScript(TITLE_SCRIPT, {
        name: GATE_TITLE,
        timeoutMs: TITLE_TIMEOUT_MS,
    });
}

/**
 * 每次超时都造**新对象** = 新超时预算。
 * 不复用旧对象：d.ts 没写 timeout 是否让 Trigger 耗尽/从 armed 集合摘除；
 * 返回新对象可同时兼容两种情况（返回数组是完整替换）。
 */
function makeRetry(ctx: RunCtx<TimeoutState>): Trigger {
    return ctx.trigger.afterScript(RETRY_SCRIPT, {
        name: GATE_RETRY,
        timeoutMs: RETRY_TIMEOUT_MS,
    });
}

function onTrigger(ctx: RunCtx<TimeoutState>, ev: FiredEvent | TimeoutEvent): Trigger | null {
    if (ev.kind === "timeout") {
        switch (ev.name) {
            case GATE_TITLE:
                // 旧语义：门超时 ⇒ exit 5 + 点名。
                ctx.finish(5, ev.name + " timeout");
                return null;

            case GATE_RETRY: {
                // 展示"超时后 re-arm 一个新 trigger"，而不是直接收口。
                ctx.state.retries += 1;
                if (ctx.state.retries >= MAX_RETRIES) {
                    ctx.finish(5, ev.name + " timeout after " + ctx.state.retries + " retries");
                    return null;
                }
                ctx.log("warn", "gate:retry timed out; re-arm a NEW trigger object", {
                    retries: ctx.state.retries,
                });
                return makeRetry(ctx); // 新对象，不是旧对象
            }

            case GATE_SETTLE:
                // afterDelay 没叠 timeoutMs，正常不会到这里；防御性收口。
                ctx.finish(5, ev.name + " timeout (unexpected)");
                return null;

            default:
                ctx.fail(ev.name, "unknown timeout");
                return null;
        }
    }

    switch (ev.name) {
        case GATE_TITLE:
            // TITLE 门命中后转到重试门（同样是新对象）。
            return makeRetry(ctx);

        case GATE_RETRY:
            // 纯固定等待：afterDelay **不叠 timeoutMs**。
            // 旧 `wait` 是固定时长、没有守卫；卡住由外层 --time-limit / --max-frames 看门狗负责。
            return ctx.trigger.afterDelay(SETTLE_DELAY_MS, { name: GATE_SETTLE });

        case GATE_SETTLE:
            ctx.finish(0, "gate settled");
            return null;

        default:
            ctx.fail(ev.name, "unknown state");
            return null;
    }
}

const plugin = {
    initialState: { retries: 0 },
    start,
    onTrigger,
} satisfies PluginModule<TimeoutState>;

export default plugin;
