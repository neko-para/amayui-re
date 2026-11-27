// S2c 夹具 · `ctx.at(where, (tick, next) => …)` 的链 / `next()` 截断 / `ctx.off` / v2 `Tick`
//
// 判据（每个 mode 对应一条 C++ 用例；红了意味着什么）：
//   · `chain-order`     ：同点两条 at，h1 调 `next()`（两次 = 幂等）、h2 不调。一次派发里的
//     顺序 / 同一个 Tick 对象 / tick.id = 第一条订阅 / 两条的 hits 与 kind（"sub"）。
//   · `next-truncates`  ：h1 不调 next ⇒ 只跑 h1（h2 的 hits 不涨）；下一拍 h1 调 ⇒ [1,2]。
//   · `off`             ：onTrigger 里 `ctx.off(h2)`（true）再撤一次（false）；后续派发只跑 h1。
//   · `tick-off`        ：handler 里 `ctx.off(自己)` 不抛（门 = call_host）、**不截断**（h2 照跑）、
//     下一拍不再命中。
//   · `handler-error`   ：h1 抛 ⇒ 宿主一行 error + **撤整条订阅** + 截断；h2 不跑但 hits 照涨。
//   · `now`             ：`tick.now.{script,pos,opcode?}` = 本拍快照；平铺字段（deprecated）相等；
//     读不到 opcode ⇒ `now` 里**缺键**（`pos` 可经 params 换到未提交 pc 上）。
//   · `local`           ：`tick.local.{int,ptr,ptrDword}` 的语义值 / 越界与指针 0 ⇒ undefined。
//   · `stale`           ：Tick 存进模块变量，下一次 **call() 入口**（onTrigger）里 `local.*` ⇒
//     `stale_tick`；`now` 快照仍可读；`log` 仍可用。
//   · `multi-same-pos`  ：同点 4 条（at×2 + v1 onTick + once）⇒ 一条链；截断只影响 at 参与者，
//     v1 与 once 照跑（once 的 `.then` 在同一入口的 drain 里 ⇒ onTrigger 读到 onceRan=1）；
//     `duplicate` 只对 v1 生效 ⇒ 4 条都能装上。
//   · `keeps-run`       ：`start` 返回 null + 一条 at ⇒ Run 靠订阅存活（run=waiting）；handler 里
//     `ctx.off(self)` ⇒ 再一个读点后收口（run=done、表空）。
//   · `next-outside`    ：把 `next` 存下来、在 handler 返回之后（下一个 onTrigger）调用 ⇒ 抛
//     TypeError（消息含 `next_outside_chain`）；插件自己接住 ⇒ Run 不因这个用错 API 而崩。
//   · `drain-tick-local`：handler 里排的 microtask（同一次入口的 drain 里跑）读 `tick.local.int(0)`
//     ⇒ 读得到值、**不** stale（`ticks.clear()` 若挪到 drain 之前，这里就 stale）。
//
// ★ tick 入口只读：`ctx.edit/input/trigger/fail/finish/quit/profile/config/hook.*` 全 TypeError
//   ⇒ 本夹具只写**模块变量**，guest 写回统一放在 `onTrigger`（合法的 Run 回调）。
// ★ `off` 是唯一例外：只减操作，门 = `call_host != nullptr` ⇒ tick handler 里也能撤（`tick-off`）。
const SCRIPT = "FIELD.BIN";
const POS = 0x0100;
const UNDEF = 0x7ffffffe;  // undefined 的哨兵（⛔ 不是 0：0 是真实值）

// guest int 输出槽（C++ 侧按同一组 id 读回；int 池 = 0xB1B6 个槽 ⇒ 这些 id 都寻址得到）。
const OUT_A = 0xb130;
const OUT_B = 0xb131;
const OUT_C = 0xb132;
const OUT_N = 0xb133;
const OUT_F0 = 0xb134;
const OUT_F1 = 0xb135;
const OUT_F2 = 0xb136;
const OUT_F3 = 0xb137;
const OUT_F4 = 0xb138;
const OUT_F5 = 0xb139;
const OUT_F6 = 0xb13a;
const OUT_F7 = 0xb13b;
const OUT_F8 = 0xb13c;
const OUT_F9 = 0xb13d;

let order = [];
let firstTick = null;
let sameTick = true;
let allowNext = false;
let phase = 0;
let h1 = null;
let h2 = null;
let hNow = null;
let v1 = null;
let selfSub = null;
let onceRan = 0;
let tickRuns = 0;
let nowRec = null;
let localRec = null;
let savedTick = null;
let savedNext = null;   // `next-outside`：handler 里存下来、handler 返回之后再调
let drainRan = 0;       // `drain-tick-local`：drain 里那次 microtask 跑了几次
let drainValue = UNDEF; // 它读到的 `local.int(0)`（读不到 / stale 就保持哨兵）
let drainStale = 0;     // 那次读有没有抛 `stale_tick`

function note(value) {
    return value === undefined ? UNDEF : value;
}

function put(ctx, id, value) {
    ctx.edit.global.set("int", id, note(value));
}

function hitsOf(hooks, handle) {
    for (const row of hooks) {
        if (row.id === handle) return row.hits;
    }
    return -1;
}

function kindOf(hooks, handle) {
    for (const row of hooks) {
        if (row.id === handle) return row.kind;
    }
    return "?";
}

function remember(tick) {
    if (firstTick === null) {
        firstTick = tick;
    } else if (firstTick !== tick) {
        sameTick = false;
    }
}

const plugin = {
    start(ctx) {
        const mode = ctx.params.mode;
        if (mode === "chain-order") {
            h1 = ctx.at({ script: SCRIPT, pos: POS }, (tick, next) => {
                order.push(1);
                remember(tick);
                next();
                next();  // 幂等：第二次是 no-op，不抛
            });
            h2 = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                order.push(2);
                remember(tick);
                // 不调 next ⇒ 截断后续 at 参与者（这里后面没有别人，但判据看 order 与 hits）
            });
            if (typeof h1 !== "number" || h1 <= 0) throw new Error("chain-order: at h1 failed");
            if (typeof h2 !== "number" || h2 <= 0) throw new Error("chain-order: at h2 failed");
            ctx.log("info", "at-chain: installed mode=chain-order");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "next-truncates") {
            h1 = ctx.at({ script: SCRIPT, pos: POS }, (tick, next) => {
                order.push(1);
                if (allowNext) next();
            });
            h2 = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                order.push(2);
            });
            if (typeof h1 !== "number" || typeof h2 !== "number") {
                throw new Error("next-truncates: at failed");
            }
            ctx.log("info", "at-chain: installed mode=next-truncates");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "off") {
            h1 = ctx.at({ script: SCRIPT, pos: POS }, (tick, next) => {
                tick.log("info", "at-off: h1 hit");
                next();
            });
            h2 = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                tick.log("info", "at-off: h2 hit");
            });
            if (typeof h1 !== "number" || typeof h2 !== "number") {
                throw new Error("at-off: at failed");
            }
            ctx.log("info", "at-chain: installed mode=off");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "tick-off") {
            selfSub = ctx.at({ script: SCRIPT, pos: POS }, (tick, next) => {
                // ★ 门 = call_host（不是 inCallback）：tick handler 里 `off` 必须被接受。
                const removed = ctx.off(selfSub);
                tick.log("info", "tick-off: off=" + String(removed));
                next();  // 撤自己**不截断**：h2 照跑
            });
            const h2t = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                // ★ 带序号：`PluginHost::log` 对**连续相同**的消息去重（否则第二拍这行会被吞掉）。
                tickRuns += 1;
                tick.log("info", "tick-off: h2 ran #" + String(tickRuns));
            });
            h2 = h2t;
            if (typeof selfSub !== "number" || typeof h2t !== "number") {
                throw new Error("tick-off: at failed");
            }
            ctx.log("info", "at-chain: installed mode=tick-off");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "handler-error") {
            h1 = ctx.at({ script: SCRIPT, pos: POS }, () => {
                throw new Error("boom");
            });
            h2 = ctx.at({ script: SCRIPT, pos: POS }, () => {
                // 什么都不做：这样"第二次派发 log 不增长"才能区分"h2 跑了但没说话"。
            });
            if (typeof h1 !== "number" || typeof h2 !== "number") {
                throw new Error("handler-error: at failed");
            }
            ctx.log("info", "at-chain: installed mode=handler-error");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "now") {
            const pos = ctx.params.pos ? Number(ctx.params.pos) : POS;
            hNow = ctx.at({ script: SCRIPT, pos: pos }, (tick) => {
                if (typeof tick.now !== "object" || tick.now === null) {
                    throw new Error("now: tick.now 缺失");
                }
                nowRec = {
                    script: tick.now.script,
                    pos: tick.now.pos,
                    hasOpcode: ("opcode" in tick.now) ? 1 : 0,
                    opcode: tick.now.opcode,
                    flatScript: tick.script,
                    flatPos: tick.pos,
                    flatOpcode: tick.opcode,
                    id: tick.id,
                };
            });
            if (typeof hNow !== "number") throw new Error("now: at failed");
            ctx.log("info", "at-chain: installed mode=now");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "local") {
            hNow = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                const l = tick.local;
                if (typeof l !== "object" || l === null || typeof l.int !== "function" ||
                    typeof l.ptr !== "function" || typeof l.ptrDword !== "function") {
                    throw new Error("local: tick.local 形状错");
                }
                localRec = {
                    int0: l.int(0),
                    int7: l.int(7),
                    int8: l.int(8),
                    ptr0: l.ptr(0),
                    ptrDword0: l.ptrDword(0),
                    ptr2: l.ptr(2),
                    ptrDword1: l.ptrDword(1),
                };
            });
            if (typeof hNow !== "number") throw new Error("local: at failed");
            ctx.log("info", "at-chain: installed mode=local");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "stale") {
            hNow = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                savedTick = tick;
            });
            if (typeof hNow !== "number") throw new Error("stale: at failed");
            ctx.log("info", "at-chain: installed mode=stale");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "multi-same-pos") {
            h1 = ctx.at({ script: SCRIPT, pos: POS }, (tick, next) => {
                order.push(1);
                next();
            });
            h2 = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                order.push(2);  // 不调 next ⇒ 截断**后续 at 参与者**，但不影响 v1 / once
            });
            // v1 `ctx.hook.at`（Pos + V1At）：与 at / once 同点是**允许**的（去重只对 V1At 自己）
            v1 = ctx.hook.at(SCRIPT, POS);
            if (v1 === undefined) throw new Error("multi-same-pos: ctx.hook.at failed");
            v1.onTick((tick) => {
                order.push(99);
            });
            const pending = ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 5000 });
            pending.then(() => {
                onceRan = 1;  // 同一次派发的 drain 里必须跑到（onTrigger 会读它）
            });
            if (typeof h1 !== "number" || typeof h2 !== "number") {
                throw new Error("multi-same-pos: at failed");
            }
            ctx.log("info", "at-chain: installed mode=multi-same-pos");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "keeps-run") {
            // start 返回 null（runner done）；一条 at 订阅就是 Run 的存活来源。handler 里撤掉它。
            selfSub = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                ctx.off(selfSub);
            });
            if (typeof selfSub !== "number") throw new Error("keeps-run: at failed");
            ctx.log("info", "at-chain: installed mode=keeps-run");
            return null;
        }
        if (mode === "next-outside") {
            h1 = ctx.at({ script: SCRIPT, pos: POS }, (tick, next) => {
                // ★ 只存下来，**不**在 handler 里调：链外调用必须抛 `next_outside_chain`。
                savedNext = next;
            });
            if (typeof h1 !== "number") throw new Error("next-outside: at failed");
            ctx.log("info", "at-chain: installed mode=next-outside");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "drain-tick-local") {
            hNow = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                // ★ 这个 microtask 在**同一次入口的 drain** 里跑（runInstrHits 末尾）⇒ Tick 必须
                //   仍有效、`local.int(0)` 读得到值；`ticks.clear()` 若挪到 drain 之前就读不到。
                Promise.resolve().then(() => {
                    drainRan += 1;
                    try {
                        drainValue = tick.local.int(0);
                    } catch (e) {
                        const message = String(e && e.message ? e.message : e);
                        drainStale = message.indexOf("stale_tick") >= 0 ? 1 : 0;
                    }
                });
            });
            if (typeof hNow !== "number") throw new Error("drain-tick-local: at failed");
            ctx.log("info", "at-chain: installed mode=drain-tick-local");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        throw new Error("at-chain: unknown mode: " + String(mode));
    },
    onTrigger(ctx) {
        const mode = ctx.params.mode;
        const hooks = ctx.hook.list();
        if (mode === "chain-order") {
            put(ctx, OUT_A, order[0]);
            put(ctx, OUT_B, order[1]);
            put(ctx, OUT_C, order[2]);
            put(ctx, OUT_N, order.length);
            put(ctx, OUT_F0, sameTick ? 1 : 0);
            put(ctx, OUT_F1, firstTick === null ? 0 : firstTick.id);
            put(ctx, OUT_F2, h1 < h2 ? 1 : 0);
            put(ctx, OUT_F3, kindOf(hooks, h1) === "sub" ? 1 : 0);
            put(ctx, OUT_F4, kindOf(hooks, h2) === "sub" ? 1 : 0);
            put(ctx, OUT_F5, hitsOf(hooks, h1));
            put(ctx, OUT_F6, hitsOf(hooks, h2));
            ctx.off(h1);
            ctx.off(h2);
            return [];
        }
        if (mode === "next-truncates") {
            if (phase === 0) {
                // 截断拍：只有 h1（h2 的 hits 必须还是 0）。
                put(ctx, OUT_A, order[0]);
                put(ctx, OUT_B, order[1]);
                put(ctx, OUT_N, order.length);
                put(ctx, OUT_F0, hitsOf(hooks, h1));
                put(ctx, OUT_F1, hitsOf(hooks, h2));
                allowNext = true;
                order = [];
                phase = 1;
                return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump2" })];
            }
            // 继续拍：[1,2]，h2 的 hits 涨到 1。
            put(ctx, OUT_C, order.length);
            put(ctx, OUT_F2, order[0]);
            put(ctx, OUT_F3, order[1]);
            put(ctx, OUT_F4, hitsOf(hooks, h2));
            ctx.off(h1);
            ctx.off(h2);
            return [];
        }
        if (mode === "off") {
            if (phase === 0) {
                const first = ctx.off(h2);
                const second = ctx.off(h2);  // 幂等：已经不在 ⇒ false
                put(ctx, OUT_F0, first ? 1 : 0);
                put(ctx, OUT_F1, second ? 1 : 0);
                put(ctx, OUT_F2, ctx.hook.list().length);
                phase = 1;
                return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
            }
            ctx.off(h1);
            return [];
        }
        if (mode === "handler-error") {
            // 第二次派发之后：h2 还在（hits 涨），h1 已经不在表里。
            put(ctx, OUT_F0, hooks.length);
            put(ctx, OUT_F1, hitsOf(hooks, h2));
            put(ctx, OUT_F2, kindOf(hooks, h1) === "?" ? 1 : 0);
            ctx.off(h2);
            return [];
        }
        if (mode === "now") {
            const r = nowRec;
            if (r === null) {
                ctx.fail("now", "no hit recorded");
                return [];
            }
            put(ctx, OUT_A, r.script === SCRIPT ? 1 : 0);
            put(ctx, OUT_B, r.pos);
            put(ctx, OUT_C, r.hasOpcode);
            put(ctx, OUT_N, r.opcode);
            put(ctx, OUT_F0, r.flatPos);
            put(ctx, OUT_F1, r.flatScript === SCRIPT ? 1 : 0);
            put(ctx, OUT_F2, r.flatOpcode);
            put(ctx, OUT_F3, r.id);
            ctx.off(hNow);
            return [];
        }
        if (mode === "local") {
            const r = localRec;
            if (r === null) {
                ctx.fail("local", "no hit recorded");
                return [];
            }
            put(ctx, OUT_A, r.int0);
            put(ctx, OUT_B, r.int7);
            put(ctx, OUT_C, r.int8);
            put(ctx, OUT_N, r.ptr0);
            put(ctx, OUT_F0, r.ptrDword0);
            put(ctx, OUT_F1, r.ptr2);
            put(ctx, OUT_F2, r.ptrDword1);
            ctx.off(hNow);
            return [];
        }
        if (mode === "stale") {
            // ★ 本回调是 `call()` 入口（不是 tick 入口）⇒ 上一次的 Tick 必须已经 stale。
            let staleOk = 0;
            try {
                savedTick.local.int(0);
            } catch (e) {
                const message = String(e && e.message ? e.message : e);
                staleOk = message.indexOf("stale_tick") >= 0 ? 1 : 0;
            }
            let nowPos = 0xffff;
            try {
                nowPos = savedTick.now.pos;  // 快照：跨入口仍读到上一拍的值，**不抛**
            } catch (e) {
                nowPos = 0xfffe;
            }
            let logOk = 0;
            try {
                savedTick.log("info", "stale: late log");  // log 是宿主出口 ⇒ 仍可用
                logOk = 1;
            } catch (e) {
                logOk = 0;
            }
            put(ctx, OUT_A, staleOk);
            put(ctx, OUT_B, nowPos);
            put(ctx, OUT_C, logOk);
            ctx.off(hNow);
            return [];
        }
        if (mode === "multi-same-pos") {
            put(ctx, OUT_A, order[0]);
            put(ctx, OUT_B, order[1]);
            put(ctx, OUT_C, order[2]);
            put(ctx, OUT_N, order.length);
            put(ctx, OUT_F0, onceRan);
            put(ctx, OUT_F1, hooks.length);
            put(ctx, OUT_F2, hitsOf(hooks, h1));
            put(ctx, OUT_F3, hitsOf(hooks, h2));
            put(ctx, OUT_F4, kindOf(hooks, h1) === "sub" ? 1 : 0);
            put(ctx, OUT_F5, kindOf(hooks, h2) === "sub" ? 1 : 0);
            put(ctx, OUT_F6, kindOf(hooks, v1.id) === "v1" ? 1 : 0);
            v1.clear();
            ctx.off(h1);
            ctx.off(h2);
            return [];
        }
        if (mode === "next-outside") {
            // ★ 存下来的 next 在 handler 返回之后（本次 call() 入口里）调用 ⇒ 必须抛 TypeError。
            //   这里自己接住：Run 不因观察者用错 API 而崩（语义 = 抛给调用方，不是 fail Run）。
            let outsideOk = 0;
            let messageOk = 0;
            let nameOk = 0;
            try {
                savedNext();
            } catch (e) {
                outsideOk = 1;
                const message = String(e && e.message ? e.message : e);
                messageOk = message.indexOf("next_outside_chain") >= 0 ? 1 : 0;
                nameOk = (e && e.name === "TypeError") ? 1 : 0;
            }
            put(ctx, OUT_A, outsideOk);
            put(ctx, OUT_B, messageOk);
            put(ctx, OUT_C, nameOk);
            ctx.off(h1);
            return [];
        }
        if (mode === "drain-tick-local") {
            put(ctx, OUT_A, drainValue);
            put(ctx, OUT_B, drainStale);
            put(ctx, OUT_C, drainRan);
            ctx.off(hNow);
            return [];
        }
        ctx.fail("mode", "unexpected onTrigger for " + String(mode));
        return [];
    },
};

export default plugin;
