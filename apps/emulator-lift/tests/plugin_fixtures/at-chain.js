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
//   · `where-range`     ：`{script,lo,hi}` = `[lo,hi)`：lo 命中 / hi-1 命中 / hi 不命中 / lo-1 不命中。
//   · `where-script`    ：`{script}` = 整本脚本的每条指令（两个不同 pos 都命中；换脚本 ⇒ 0）。
//   · `where-opcode`    ：`{opcode:114}` 在两条不同 pos 都命中；`{script,opcode}` 只在该脚本命中；
//     `{opcode:0}` 在**未提交 pc** 上不命中（⛔ 不编 0）、在真读到 0 时命中（正控制）。
//   · `where-mixed`     ：同拍 `{script}` + `{script,pos}` + `{opcode}` 按注册序成一条链。
//   · `where-big`       ：`{script,lo,hi}` 的 `lo/hi > 2^31` 从 `hook.list()` **原样**读回（uint32 口径）。
//   · `where-null-field`：where 的键**存在但值是 null/undefined** ⇒ TypeError（⛔ 不静默退化）+
//     合法形态仍装得上（`hook.list()` 不新增条目）。
//   · `tick-id`         ：多 Impl 同活时 `tick.id` 必须是本实例自己的句柄（⛔ 不是 `hits[0]`）。
//   · `tick-edit`       ：tick 里 `edit.global.set` ⇒ guest int 立即变；同一 handler 里 `ctx.fail` 仍
//     TypeError（"写面不是把 in_callback_ 打开"的读数）。
//   · `tick-input`      ：tick 里 `input.click` ⇒ 回执 queued、下一读点 applied。
//   · `stale-write`     ：旧 Tick 的 `edit` / `input` / `patch` ⇒ 抛 `stale_tick`，guest / 队列零变化。
//   · `tick-patch`      ：tick 里 `patch({localInt:0},5)` ⇒ 该条指令读到 5；下一拍恢复；CAS 冲突读数。
//   · `tick-patch-limit`：9 个不同地址 + 1 个越界 ⇒ 前 8 条生效、第 9 条 `too_many_slots` + `skips=1`、
//     越界 target ⇒ `bad_target`。
//   · `once-tick-writable`：`once` resolve 的 `.then` 里 `tick.local` / `tick.edit` 可用（同一入口 drain）。
//   · `once-tick-patch` ：同一个 `.then` 里 `tick.patch` ⇒ 回执 `code:"once_tick"`（⛔ 不是 no_such_hook）、
//     guest 零变化；`tick.local` / `tick.edit` 仍可用。
//
// ★ Tick 入口的**写面**（S2c-2）：`tick.edit` / `tick.input` / `tick.patch` 可用；从 RunCtx 调的
//   `ctx.edit/input/trigger/fail/finish/quit/profile/config/hook.*` 仍 TypeError。
// ★ `off` 是既有的例外：只减操作，门 = `call_host != nullptr` ⇒ tick handler 里也能撤（`tick-off`）。
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
let h3 = null;
let h0 = null;          // `where-opcode`：`{opcode:0}` 的订阅
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
// ---- S2c-2：`where` 过滤（五种形态）----
let whereRan = 0;       // 命中次数（range / script）
let wherePos = [];      // range / script 形态下每次命中的 `tick.now.pos`
let opAllRuns = 0;      // `{opcode:114}` 的命中次数（全局）
let opScopedRuns = 0;   // `{script,opcode:114}` 的命中次数（只在该脚本）
let opZeroRuns = 0;     // `{opcode:0}` 的命中次数（未提交 pc 上一拍都不许涨）
let bigRan = 0;         // `where-big`：大区间订阅的命中次数（不派发 ⇒ 0）
let nullThrew = 0;      // `where-null-field`：非法形态抛了几条
let nullType = 0;       // 其中是 TypeError 的条数
let nullSub = 0;        // 正控制：合法形态拿到的 id（> 0 ⇒ 合法面没坏）
let seenTickId = 0;     // `tick-id`：本实例看到的 `tick.id`
let seenTickRuns = 0;   // 它跑了几次

// ---- S2c-2b：Tick 的写面（edit / input / patch）与 stale 写 ----
const TICK_TARGET = 0xb150;  // guest int 输出 / 写面的目标槽（int 池 0xB1B6 个槽 ⇒ 寻址得到）
const PATCH_VALUE = 5;
let editRec = null;         // `tick-edit`：回执 + `ctx.fail` 是否仍 TypeError
let editFailType = 0;
let inputRec = null;        // `tick-input`：回执 + handler 内 status
let inputStatusInTick = "";
let inputStatusInDump = "";
let patchRec = null;        // `tick-patch-cas`：回执
let patchPreLocal = null;   // handler 里 patch 之后读到的 local.int(0)（必须是**打补丁前**的 7）
let patchStat = null;       // 第二次 onTrigger 读到的 restores / restore_conflicts
let patchReceipts = [];     // `tick-patch-limit`：10 条订阅的回执
let patchSubs = [];         // 它们的句柄（第 9 条 = too_many_slots 的读数）
let onceLocal = UNDEF;      // `once-tick-writable`：continuation 读到的 local.int(0)
let onceEdit = 0;           // 它的 tick.edit 回执 accepted&&applied
let onceErr = 0;
// ---- S2c-2 复审 F2：`once` 的 Tick 调 `tick.patch`（必须回执 `once_tick`）----
let oncePatchCode = "";
let oncePatchAccepted = 0;
let oncePatchApplied = 0;

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

// 读 `hook.list()` 的某个统计字段（S2c-2b：`restores` / `restore_conflicts` / `skips`）。
function statOf(hooks, handle, field) {
    for (const row of hooks) {
        if (row.id === handle) return row[field];
    }
    return -1;
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
                // ★ S2c-2 复审 F9：index 必须是整数 ⇒ 小数 TypeError（⛔ 不截断到相邻槽）。
                let fracType = 0;
                try {
                    l.int(0.5);
                } catch (e) {
                    fracType = (e && e.name === "TypeError") ? 1 : 2;
                }
                localRec = {
                    int0: l.int(0),
                    int7: l.int(7),
                    int8: l.int(8),
                    ptr0: l.ptr(0),
                    ptrDword0: l.ptrDword(0),
                    ptr2: l.ptr(2),
                    ptrDword1: l.ptrDword(1),
                    intFracType: fracType,
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
        // ---- S2c-2：`where` 过滤的五种形态 ----
        if (mode === "where-range") {
            h1 = ctx.at({ script: SCRIPT, lo: 0x0100, hi: 0x0108 }, (tick) => {
                whereRan += 1;
                wherePos.push(tick.now.pos);
            });
            if (typeof h1 !== "number") throw new Error("where-range: at failed");
            ctx.log("info", "at-chain: installed mode=where-range");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "where-script") {
            h1 = ctx.at({ script: SCRIPT }, (tick) => {
                whereRan += 1;
                wherePos.push(tick.now.pos);
            });
            if (typeof h1 !== "number") throw new Error("where-script: at failed");
            ctx.log("info", "at-chain: installed mode=where-script");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "where-opcode") {
            h1 = ctx.at({ opcode: 114 }, (tick, next) => {
                opAllRuns += 1;
                next();  // 不断链：后面那条 {script,opcode} 也要跑（判据是两边的命中次数）
            });
            h2 = ctx.at({ script: SCRIPT, opcode: 114 }, () => {
                opScopedRuns += 1;
            });
            // ★ S2c-2 复审 F3：`opcode == 0` 的订阅 —— 未提交 pc（读不到 opcode）时**一拍都不许命中**。
            //   修前的断言用的订阅值不是 0，所以"读失败 ⇒ 编 0"那条错它区分不出来。
            h0 = ctx.at({ opcode: 0 }, () => {
                opZeroRuns += 1;
            });
            if (typeof h1 !== "number" || typeof h2 !== "number" || typeof h0 !== "number") {
                throw new Error("where-opcode: at failed");
            }
            ctx.log("info", "at-chain: installed mode=where-opcode");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "where-mixed") {
            // 注册序 = 链序：`{script}` → `{script,pos}` → `{opcode}`。
            h1 = ctx.at({ script: SCRIPT }, (tick, next) => {
                order.push(1);
                next();
            });
            h2 = ctx.at({ script: SCRIPT, pos: POS }, (tick, next) => {
                order.push(2);
                next();
            });
            h3 = ctx.at({ opcode: 114 }, (tick) => {
                order.push(3);
            });
            if (typeof h1 !== "number" || typeof h2 !== "number" || typeof h3 !== "number") {
                throw new Error("where-mixed: at failed");
            }
            ctx.log("info", "at-chain: installed mode=where-mixed");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "where-big") {
            // ★ S2c-2 复审 F10：`lo/hi` 是 uint32 语义 ⇒ > 2^31 的值必须从 hook.list() **原样**读回
            //   （int32 出口会变成负数；C++ 侧回写 guest 再读回看不出来，所以判据在这里）。
            h1 = ctx.at({ script: SCRIPT, lo: 0x90000000, hi: 0x90000010 }, () => {
                bigRan += 1;
            });
            if (typeof h1 !== "number") throw new Error("where-big: at failed");
            ctx.log("info", "at-chain: installed mode=where-big");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "where-null-field") {
            // ★ S2c-2 复审 F1：键**存在**但值是 null/undefined ⇒ TypeError（与"没给这个键"区分）。
            //   修前 `{script, pos: undefined}` 会静默变成 `{script}` 整本脚本订阅（表里多一条）。
            const bad = [
                { script: SCRIPT, pos: undefined },
                { script: SCRIPT, pos: null },
                { script: SCRIPT, lo: null, hi: 0x108 },
                { script: SCRIPT, lo: 0x100, hi: undefined },
                { script: SCRIPT, opcode: undefined },
                { script: null },
            ];
            for (const where of bad) {
                try {
                    ctx.at(where, () => {});
                } catch (e) {
                    nullThrew += 1;
                    if (e && e.name === "TypeError") nullType += 1;
                }
            }
            // 正控制：合法形态仍装得上（"表里只剩一条"不是"at 整个坏了"）。
            nullSub = ctx.at({ script: SCRIPT, pos: POS }, () => {});
            if (typeof nullSub !== "number") throw new Error("where-null-field: good at failed");
            ctx.log("info", "at-chain: installed mode=where-null-field");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "tick-id") {
            // ★ 多 Impl 同活：两个宿主各装一条**同点**订阅；一次派发的 hits 里两条都出现
            //   ⇒ 每个 Impl 的 `tick.id` 必须是**自己**的句柄（⛔ 不是 `hits[0]`）。
            h1 = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                seenTickId = tick.id;
                seenTickRuns += 1;
            });
            if (typeof h1 !== "number") throw new Error("tick-id: at failed");
            ctx.log("info", "at-chain: installed mode=tick-id");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        // ---- S2c-2b：Tick 的写面（edit / input / patch）+ stale 写 + once 的 Tick ----
        if (mode === "tick-edit") {
            hNow = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                // ★ d.ts 的 Tick 成员表（S2c-2）：`edit` / `input` / `patch` 与 `now` / `local` 同形。
                if (typeof tick.edit !== "object" || tick.edit === null ||
                    typeof tick.edit.global !== "object" || tick.edit.global === null ||
                    typeof tick.edit.global.set !== "function") {
                    throw new Error("tick-edit: tick.edit 形状错");
                }
                if (typeof tick.input !== "object" || tick.input === null ||
                    typeof tick.input.click !== "function" || typeof tick.input.status !== "function") {
                    throw new Error("tick-edit: tick.input 形状错");
                }
                if (typeof tick.patch !== "function") throw new Error("tick-edit: tick.patch 形状错");
                const r = tick.edit.global.set("int", TICK_TARGET, 42);
                editRec = { accepted: r.accepted, applied: r.applied, queued: r.queued };
                // ★ "写面不是把 in_callback_ 打开"的唯一可判读数：同一个 tick handler 里
                //   `ctx.fail`（start 的那个 RunCtx）仍必须是 TypeError。
                try {
                    ctx.fail("tick-edit", "must-not-run");
                } catch (e) {
                    editFailType = (e && e.name === "TypeError") ? 1 : 2;
                }
            });
            if (typeof hNow !== "number") throw new Error("tick-edit: at failed");
            ctx.log("info", "at-chain: installed mode=tick-edit");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "tick-input") {
            hNow = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                const r = tick.input.click(10, 20);
                inputRec = {
                    accepted: r.accepted,
                    queued: r.queued,
                    applied: r.applied,
                    hasId: typeof r.id === "number" && r.id > 0,
                    id: r.id,
                };
                inputStatusInTick = tick.input.status(r.id);
            });
            if (typeof hNow !== "number") throw new Error("tick-input: at failed");
            ctx.log("info", "at-chain: installed mode=tick-input");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "stale-write") {
            hNow = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                savedTick = tick;
            });
            if (typeof hNow !== "number") throw new Error("stale-write: at failed");
            ctx.log("info", "at-chain: installed mode=stale-write");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "tick-patch") {
            hNow = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                const r = tick.patch({ localInt: 0 }, PATCH_VALUE);
                // ★ handler 必须看到**打补丁前**的值（写发生在 `on_hits` 返回之后）。
                patchPreLocal = tick.local.int(0);
                patchRec = {
                    accepted: r.accepted,
                    applied: r.applied,
                    code: r.code === undefined ? "" : String(r.code),
                };
            });
            if (typeof hNow !== "number") throw new Error("tick-patch: at failed");
            ctx.log("info", "at-chain: installed mode=tick-patch");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "tick-patch-limit") {
            // 9 条各自 patch 一个**不同**地址（localInt 0..7 + ptrLocal 0），第 10 条越界。
            for (let i = 0; i < 9; i += 1) {
                const target = i < 8 ? { localInt: i } : { ptrLocal: 0 };
                const value = 0x500 + i;
                const id = ctx.at({ script: SCRIPT, pos: POS }, (tick, next) => {
                    const r = tick.patch(target, value);
                    patchReceipts.push({
                        accepted: r.accepted,
                        code: r.code === undefined ? "" : String(r.code),
                    });
                    next();
                });
                if (typeof id !== "number") throw new Error("tick-patch-limit: at failed #" + String(i));
                patchSubs.push(id);
            }
            const bad = ctx.at({ script: SCRIPT, pos: POS }, (tick, next) => {
                const r = tick.patch({ localInt: 8 }, 1);  // int_count = 8 ⇒ 越界
                patchReceipts.push({
                    accepted: r.accepted,
                    code: r.code === undefined ? "" : String(r.code),
                });
                next();
            });
            if (typeof bad !== "number") throw new Error("tick-patch-limit: bad at failed");
            patchSubs.push(bad);
            ctx.log("info", "at-chain: installed mode=tick-patch-limit");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "once-tick-writable") {
            const pending = ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 5000 });
            pending.then((tick) => {
                onceRan += 1;
                try {
                    // ★ 这个 continuation 在**命中那一拍入口的 drain** 里跑 ⇒ Tick 仍有效。
                    const v = tick.local.int(0);
                    onceLocal = v === undefined ? UNDEF : v;
                    const r = tick.edit.global.set("int", TICK_TARGET, 0x77);
                    onceEdit = (r.accepted && r.applied) ? 1 : 0;
                } catch (e) {
                    onceErr = 1;
                }
            });
            ctx.log("info", "at-chain: installed mode=once-tick-writable");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "once-tick-patch") {
            // ★ S2c-2 复审 F2：`once` 的 Tick **不允许** `tick.patch`（订阅在 resolve 时已结算）⇒
            //   回执 `code:"once_tick"`（⛔ 不是 `no_such_hook`，那是"句柄不存在"的错误读数）。
            //   同一个 `.then` 里 `local` / `edit` 仍可用（写面不是把 in_callback_ 打开）。
            const pending = ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 5000 });
            pending.then((tick) => {
                onceRan += 1;
                try {
                    const v = tick.local.int(0);
                    onceLocal = v === undefined ? UNDEF : v;
                    const e = tick.edit.global.set("int", TICK_TARGET, 0x77);
                    onceEdit = (e.accepted && e.applied) ? 1 : 0;
                    const p = tick.patch({ localInt: 0 }, PATCH_VALUE);
                    oncePatchCode = p.code === undefined ? "" : String(p.code);
                    oncePatchAccepted = p.accepted ? 1 : 0;
                    oncePatchApplied = p.applied ? 1 : 0;
                } catch (err) {
                    onceErr = 1;
                }
            });
            ctx.log("info", "at-chain: installed mode=once-tick-patch");
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
            put(ctx, OUT_F3, r.intFracType);  // 1 = 小数 index 抛 TypeError（⛔ 不是截断成 int(0)）
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
        // ---- S2c-2：`where` 过滤 ----
        if (mode === "where-range") {
            put(ctx, OUT_A, whereRan);
            put(ctx, OUT_B, wherePos[0]);
            put(ctx, OUT_C, wherePos[1]);
            put(ctx, OUT_N, wherePos.length);
            // ★ S2c-2 复审 F4：`hook.list()` 的**形状面**逐字段（修前 JS 只断 kind / hits）。
            const row = hooks.length === 1 ? hooks[0] : null;
            put(ctx, OUT_F0, row !== null ? 1 : 0);
            put(ctx, OUT_F1, row !== null && row.kind === "sub" ? 1 : 0);
            put(ctx, OUT_F2, row !== null && row.form === "range" ? 1 : 0);
            put(ctx, OUT_F3, row !== null ? row.lo : -1);
            put(ctx, OUT_F4, row !== null ? row.hi : -1);
            put(ctx, OUT_F5, row !== null ? row.opcode : -1);
            put(ctx, OUT_F6, row !== null ? row.pos : -1);
            ctx.off(h1);
            return [];
        }
        if (mode === "where-script") {
            put(ctx, OUT_A, whereRan);
            put(ctx, OUT_B, wherePos[0]);
            put(ctx, OUT_C, wherePos[1]);
            ctx.off(h1);
            return [];
        }
        if (mode === "where-opcode") {
            put(ctx, OUT_A, opAllRuns);
            put(ctx, OUT_B, opScopedRuns);
            put(ctx, OUT_N, opZeroRuns);  // {opcode:0} 的 handler 次数（只在真读到 0 时涨）
            // ★ S2c-2 复审 F4/F10：`{opcode}` 两形态 + `{opcode:0}` 的 hook.list() 逐字段。
            const okLen = hooks.length === 3;
            put(ctx, OUT_F0, okLen ? 1 : 0);
            put(ctx, OUT_F1, okLen && hooks[0].form === "opcode" ? 1 : 0);
            put(ctx, OUT_F2, okLen ? hooks[0].opcode : -1);
            put(ctx, OUT_F3, okLen && hooks[1].form === "opcode" && hooks[1].script === SCRIPT ? 1 : 0);
            put(ctx, OUT_F4, okLen ? hooks[1].opcode : -1);
            put(ctx, OUT_F5, okLen && hooks[2].form === "opcode" ? 1 : 0);
            put(ctx, OUT_F6, okLen ? hooks[2].opcode : -1);
            ctx.off(h1);
            ctx.off(h2);
            ctx.off(h0);
            return [];
        }
        if (mode === "where-mixed") {
            put(ctx, OUT_A, order[0]);
            put(ctx, OUT_B, order[1]);
            put(ctx, OUT_C, order[2]);
            put(ctx, OUT_N, order.length);
            ctx.off(h1);
            ctx.off(h2);
            ctx.off(h3);
            return [];
        }
        if (mode === "where-big") {
            const row = hooks.length === 1 ? hooks[0] : null;
            put(ctx, OUT_A, row !== null ? row.lo : 0);
            put(ctx, OUT_B, row !== null ? row.hi : 0);
            put(ctx, OUT_C, row !== null && row.form === "range" ? 1 : 0);
            put(ctx, OUT_N, bigRan);
            // ★ 这一位是判据：int32 出口会把 0x90000000 读成 -1879048192 ⇒ 0。
            put(ctx, OUT_F0, row !== null && row.lo === 0x90000000 ? 1 : 0);
            put(ctx, OUT_F1, row !== null && row.hi === 0x90000010 ? 1 : 0);
            ctx.off(h1);
            return [];
        }
        if (mode === "where-null-field") {
            put(ctx, OUT_A, nullThrew);
            put(ctx, OUT_B, nullType);
            put(ctx, OUT_C, hooks.length);  // 6 条非法形态一条都不许装进来（只剩正控制那条）
            put(ctx, OUT_N, nullSub);
            ctx.off(nullSub);
            return [];
        }
        if (mode === "tick-id") {
            put(ctx, OUT_A, seenTickId);
            put(ctx, OUT_B, seenTickRuns);
            ctx.off(h1);
            return [];
        }
        if (mode === "tick-edit") {
            const r = editRec;
            if (r === null) {
                ctx.fail("tick-edit", "no receipt");
                return [];
            }
            put(ctx, OUT_A, r.accepted ? 1 : 0);
            put(ctx, OUT_B, r.applied ? 1 : 0);
            put(ctx, OUT_C, r.queued ? 1 : 0);
            put(ctx, OUT_N, editFailType);
            ctx.off(hNow);
            return [];
        }
        if (mode === "tick-input") {
            const r = inputRec;
            if (r === null) {
                ctx.fail("tick-input", "no receipt");
                return [];
            }
            put(ctx, OUT_A, r.accepted ? 1 : 0);
            put(ctx, OUT_B, r.queued ? 1 : 0);
            put(ctx, OUT_C, r.applied ? 1 : 0);
            put(ctx, OUT_N, r.hasId ? 1 : 0);
            put(ctx, OUT_F0, inputStatusInTick === "queued" ? 1 : 0);
            // 本回调里查（泵的 drain 在回调**之后**）仍是 queued；C++ 在 pump 之后用宿主队列判 applied。
            inputStatusInDump = ctx.input.status(r.id);
            put(ctx, OUT_F1, inputStatusInDump === "queued" ? 1 : 0);
            put(ctx, OUT_F2, r.id);
            ctx.off(hNow);
            return [];
        }
        if (mode === "stale-write") {
            // ★ Tick 跨入口：三个写面都必须抛 `stale_tick`，且 guest / 队列**零变化**。
            let throwEdit = 0;
            let msgEdit = 0;
            let throwInput = 0;
            let msgInput = 0;
            let throwPatch = 0;
            let msgPatch = 0;
            const stale = (e) => String(e && e.message ? e.message : e).indexOf("stale_tick") >= 0;
            try {
                savedTick.edit.global.set("int", TICK_TARGET, 0x7f);
            } catch (e) {
                throwEdit = 1;
                msgEdit = stale(e) ? 1 : 0;
            }
            try {
                savedTick.input.click(1, 2);
            } catch (e) {
                throwInput = 1;
                msgInput = stale(e) ? 1 : 0;
            }
            try {
                savedTick.patch({ localInt: 0 }, PATCH_VALUE);
            } catch (e) {
                throwPatch = 1;
                msgPatch = stale(e) ? 1 : 0;
            }
            put(ctx, OUT_A, throwEdit);
            put(ctx, OUT_B, msgEdit);
            put(ctx, OUT_C, throwInput);
            put(ctx, OUT_N, msgInput);
            put(ctx, OUT_F0, throwPatch);
            put(ctx, OUT_F1, msgPatch);
            ctx.off(hNow);
            return [];
        }
        if (mode === "tick-patch") {
            if (phase === 0) {
                const r = patchRec;
                if (r === null) {
                    ctx.fail("tick-patch", "no receipt");
                    return [];
                }
                put(ctx, OUT_A, r.accepted ? 1 : 0);
                put(ctx, OUT_B, r.applied ? 1 : 0);
                put(ctx, OUT_C, r.code === "" ? 0 : 1);
                put(ctx, OUT_F1, patchPreLocal === null ? UNDEF : patchPreLocal);
                phase = 1;
                return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump2" })];
            }
            put(ctx, OUT_N, statOf(hooks, hNow, "restores"));
            put(ctx, OUT_F0, statOf(hooks, hNow, "restore_conflicts"));
            ctx.off(hNow);
            return [];
        }
        if (mode === "tick-patch-limit") {
            let accepted = 0;
            let tooMany = 0;
            let badTarget = 0;
            for (const r of patchReceipts) {
                if (r.accepted) accepted += 1;
                if (r.code === "too_many_slots") tooMany += 1;
                if (r.code === "bad_target") badTarget += 1;
            }
            put(ctx, OUT_A, accepted);
            put(ctx, OUT_B, tooMany);
            put(ctx, OUT_C, badTarget);
            put(ctx, OUT_N, patchReceipts.length);
            put(ctx, OUT_F0, patchSubs.length === 10 ? statOf(hooks, patchSubs[8], "skips") : -1);
            for (const id of patchSubs) ctx.off(id);
            return [];
        }
        if (mode === "once-tick-writable") {
            put(ctx, OUT_A, onceRan);
            put(ctx, OUT_B, onceLocal);
            put(ctx, OUT_C, onceEdit);
            put(ctx, OUT_N, onceErr);
            return [];
        }
        if (mode === "once-tick-patch") {
            put(ctx, OUT_A, onceRan);
            put(ctx, OUT_B, oncePatchCode === "once_tick" ? 1 : 0);  // ★ 明确的 once_tick
            put(ctx, OUT_C, oncePatchAccepted);
            put(ctx, OUT_N, onceLocal);                              // local 仍可用（7）
            put(ctx, OUT_F0, onceEdit);                              // edit 仍可用
            put(ctx, OUT_F1, oncePatchApplied);
            put(ctx, OUT_F2, oncePatchCode === "" ? 1 : 0);          // 0 = 回执带了 code（不是抛、不是空）
            put(ctx, OUT_F3, onceErr);                               // 0 = 没有异常
            return [];
        }
        ctx.fail("mode", "unexpected onTrigger for " + String(mode));
        return [];
    },
};

export default plugin;
