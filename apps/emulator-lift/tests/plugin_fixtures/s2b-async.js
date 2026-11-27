// S2b 夹具 · `ctx.once` / `ctx.task` / timeout 泵 / Run 存活
//
// 判据（每个 mode 对应一条 C++ 用例；红了意味着什么）：
//   · `once-resolve`    ：once 命中 ⇒ resolve 的 Tick 与 `onTick` 同形（id/script/pos/opcode）；
//                         临时 hook 命中即撤；task 的 continuation 在**泵**里跑完（模块变量由下一次
//                         合法 Run 回调写回 guest —— tick / 泵都只读，写不了 guest）。
//   · `once-timeout`    ：不命中 + 短 timeout ⇒ 泵先 reject（catch 在泵里 drain 到、能 log，
//                         `ctx` 是"无 Tick"的 RunCtx 不崩），**随后** fail Run（reason 点名 timeout）。
//   · `task-await`      ：task 的 `phase=1 → await → phase=2` 必须在**同一入口**完成（跨入口看到 1 = 红）；
//                         task settle 后 Run 收口。
//   · `task-close`      ：start 返回 null + task 立即 settle ⇒ 收口（readout `run=done`）。
//   · `task-keeps-run`  ：task `await ctx.once(长 timeout)` ⇒ `runner.done()` 但 Run **不收口**
//                         （临时 hook 还在、readout `run=waiting`）；once 命中 ⇒ task 结束 ⇒ 收口。
//   · `task-reject`     ：task 抛 `boom` ⇒ fail Run（reason 含 boom）。
//   · `pump-budget`     ：once 命中后 task 进 `while(true) await Promise.resolve()` ⇒ **泵**的预算窗
//                         打断、Run fail（reason 含 interrupted）、guest 不卡死。
//   · `bare-once`       ：**裸** `ctx.once`（不进 `ctx.task`）+ `start` 返回 null ⇒ `runner.done()`
//                         但 Run 靠 once 活着（`hasPendingWork` 的 `onces` 支）；命中后收口（run=done）。
//   · `onTick-keeps-run`：`ctx.hook.at(...).onTick(fn)` + 返回 null ⇒ Run 靠 `hook_handlers` 支活着；
//                         撤订阅（onTrigger 里 `handle.onTick(null)`）后再来一个读点 ⇒ 收口。
//   · `once-hook-clear` ：task `await ctx.once(...)`，onTrigger 里 `ctx.hook.clear()` ⇒ once 必须被
//                         **reject**（否则 task 永远 pending、deadline 随条目消失 ⇒ Run 永不收口）。
//   · `task-finish-reject` / `task-quit-reject`：task 拒绝 + `ctx.finish`/`ctx.quit` 发生在**同一入口**
//                         ⇒ Run 的终态**不许**被翻成 failed（readout 仍是 finished/quit）。
//
// ★ tick 只读：tick 回调 / 泵里的 continuation 里 `ctx.edit/input/trigger/fail/finish/...` 全 TypeError
//   ⇒ 本夹具只写**模块变量**，guest 写回统一放在 `onTrigger`（合法的 Run 回调）。
const SCRIPT = "FIELD.BIN";
const POS = 0x0100;
// guest int 输出槽（C++ 侧按同一组 id 读回；int 池 = 0xB1B6 个槽 ⇒ 这些 id 都寻址得到）。
const OUT_POS = 0xb120;
const OUT_SCRIPT_OK = 0xb121;
const OUT_ID = 0xb122;
const OUT_OPCODE_OK = 0xb123;
const OUT_HOOKS = 0xb124;
const OUT_PHASE = 0xb125;

let lastTick = null;
let phase = 0;
// `onTick-keeps-run`：start 存的句柄；onTrigger 里用它撤订阅。
let storedHandle = null;

const plugin = {
    start(ctx) {
        const mode = ctx.params.mode;
        if (mode === "once-resolve") {
            const pending = ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 1000 });
            ctx.task(async (tctx) => {
                const tick = await pending;
                lastTick = {
                    id: tick.id,
                    script: tick.script,
                    pos: tick.pos,
                    opcode: tick.opcode,
                };
                tctx.log("info", "once-resolve: resumed pos=" + String(tick.pos) + " id=" + String(tick.id));
            });
            ctx.log("info", "once-resolve: armed");
            // 命中后由**下一个合法 Run 回调**把模块变量写回 guest（泵 / tick 都写不了 guest）。
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "dump" })];
        }
        if (mode === "once-timeout") {
            ctx.task(async (tctx) => {
                try {
                    const tick = await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 1 });
                    tctx.log("info", "unexpected resolve pos=" + String(tick.pos));
                } catch (e) {
                    // ★ 这条 catch 在**泵**里 drain 到：`ctx` 是"无 Tick"的 RunCtx —— 位置查询不崩，
                    //   写类仍 TypeError。父级裁决：timeout 是失败截止；用户 catch 了也照样 fail Run。
                    tctx.log("info", "once-timeout caught: " + String(e && e.message ? e.message : e));
                }
            });
            ctx.log("info", "once-timeout: armed");
            return null;
        }
        if (mode === "task-await") {
            ctx.task(async (tctx) => {
                phase = 1;
                await Promise.resolve();
                phase = 2;
                tctx.log("info", "task-await: continuation phase=" + String(phase));
            });
            ctx.log("info", "task-await: registered");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "check" })];
        }
        if (mode === "task-close") {
            // start 返回 null（runner done）+ task 在本入口末尾 drain 后 settle ⇒ Run 收口。
            ctx.task(async (tctx) => {
                await Promise.resolve();
                tctx.log("info", "task-close: settled");
            });
            ctx.log("info", "task-close: registered");
            return null;
        }
        if (mode === "task-keeps-run") {
            ctx.task(async (tctx) => {
                const tick = await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 5000 });
                lastTick = { id: tick.id, script: tick.script, pos: tick.pos, opcode: tick.opcode };
                tctx.log("info", "task-keeps-run: resumed pos=" + String(tick.pos));
            });
            ctx.log("info", "task-keeps-run: armed");
            return null;
        }
        if (mode === "task-reject") {
            ctx.task(async () => {
                throw new Error("boom");
            });
            return null;
        }
        if (mode === "pump-budget") {
            // ★ S2c：once **命中**的 continuation 现在在 tick 入口自己的 drain 里跑（每个入口都
            //   drain 到静止）⇒ 要让"忙等"落在**泵**里，必须用 **timeout 的 catch** 把它推后
            //   （泵先 reject 到期的一 once，再在同一预算窗 drain 出 catch 的忙等）。
            ctx.task(async (tctx) => {
                try {
                    await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 1 });
                    tctx.log("info", "pump-budget: unexpected resolve");
                } catch (e) {
                    tctx.log("info", "pump-budget: caught, about to spin: " +
                        String(e && e.message ? e.message : e));
                    while (true) {
                        await Promise.resolve();
                    }
                }
            });
            ctx.log("info", "pump-budget: armed");
            return null;
        }
        if (mode === "bare-once") {
            // ★ S2-3：裸 once（**不**包进 ctx.task）⇒ once 自己就是 Run 的存活来源
            //   （`hasPendingWork` 的 `onces` 支）。命中后 `.then` 的 continuation 在泵里跑。
            const pending = ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 60000 });
            pending.then((tick) => {
                ctx.log("info", "bare-once: resolved pos=" + String(tick.pos) + " id=" + String(tick.id));
            });
            ctx.log("info", "bare-once: armed");
            return null;
        }
        if (mode === "onTick-keeps-run") {
            // ★ S2-3：`handle.onTick` 订阅 = Run 存活来源（`hasPendingWork` 的 `hook_handlers` 支）。
            const handle = ctx.hook.at(SCRIPT, POS);
            if (handle === undefined) throw new Error("onTick-keeps-run: ctx.hook.at failed");
            storedHandle = handle;
            handle.onTick((tick) => {
                tick.log("info", "onTick-keeps-run: tick pos=" + String(tick.pos));
            });
            ctx.log("info", "onTick-keeps-run: armed");
            return null;  // runner done，但 Run 必须靠订阅活着
        }
        if (mode === "once-hook-clear") {
            // ★ S2-4：`ctx.hook.clear()` 会撤掉 once 的临时 hook。task 里 await 它 ⇒ 必须 reject，
            //   否则 task 永远 pending、deadline 随条目消失 ⇒ Run 永不收口。
            ctx.task(async (tctx) => {
                try {
                    await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 60000 });
                    tctx.log("info", "once-hook-clear: unexpected resolve");
                } catch (e) {
                    tctx.log("info", "once-hook-clear: caught " + String(e && e.message ? e.message : e));
                }
            });
            ctx.log("info", "once-hook-clear: armed");
            return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "clear" })];
        }
        if (mode === "task-finish-reject") {
            // ★ S2-1：task 立即拒绝 + `finish` 在**同一入口** ⇒ settle 必须看到"Run 已到终态"而静默丢弃。
            ctx.task(async () => {
                throw new Error("late-boom");
            });
            ctx.finish(5, "bye");
            return null;
        }
        if (mode === "task-quit-reject") {
            ctx.task(async () => {
                throw new Error("late-boom");
            });
            ctx.quit("bye");
            return null;
        }
        throw new Error("s2b-async: unknown mode: " + String(mode));
    },
    onTrigger(ctx) {
        const mode = ctx.params.mode;
        if (mode === "once-resolve") {
            if (lastTick === null) {
                ctx.fail("once", "no tick recorded");
                return [];
            }
            const hooks = ctx.hook.list();  // once 的临时 hook 命中即撤 ⇒ 必须 0
            ctx.edit.global.set("int", OUT_POS, lastTick.pos);
            ctx.edit.global.set("int", OUT_SCRIPT_OK, lastTick.script === SCRIPT ? 1 : 0);
            ctx.edit.global.set("int", OUT_ID, lastTick.id);
            ctx.edit.global.set("int", OUT_OPCODE_OK, lastTick.opcode === 114 ? 1 : 0);
            ctx.edit.global.set("int", OUT_HOOKS, hooks.length);
            return [];
        }
        if (mode === "task-await") {
            // ★ 这里必须写出 2：continuation 若漏了 start 入口的 drain，就会在本 onTrigger 的
            //   `call()` 末尾才跑 ⇒ 这里看到 1（跨入口污染）。
            ctx.edit.global.set("int", OUT_PHASE, phase);
            return [];
        }
        if (mode === "onTick-keeps-run") {
            // ★ S2-3：撤掉订阅 ⇒ `hasPendingWork` 的 `hook_handlers` 支消失 ⇒ 下一次读点收口。
            if (storedHandle === null) throw new Error("onTick-keeps-run: no stored handle");
            storedHandle.onTick(null);
            return null;
        }
        if (mode === "once-hook-clear") {
            // ★ S2-4：清掉**全局** hook（含 once 的临时 hook）⇒ once 的 promise 必须被 reject。
            ctx.hook.clear();
            return null;
        }
        ctx.fail("mode", "unexpected onTrigger for " + String(mode));
        return [];
    },
};

export default plugin;
