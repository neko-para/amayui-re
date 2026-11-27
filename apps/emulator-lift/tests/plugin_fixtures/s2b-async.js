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
            ctx.task(async (tctx) => {
                await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 5000 });
                tctx.log("info", "pump-budget: resumed (about to spin)");
                while (true) {
                    await Promise.resolve();
                }
            });
            ctx.log("info", "pump-budget: armed");
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
        ctx.fail("mode", "unexpected onTrigger for " + String(mode));
        return [];
    },
};

export default plugin;
