// v2 夹具 · `ctx.once` / `ctx.task` / timeout 泵 / Run 存活
//
// 判据（每个 mode 对应一条 C++ 用例；红了意味着什么）：
//   · `once-resolve`    ：once 命中 ⇒ resolve 的 Tick 与 at 同形（id/script/pos/opcode）；
//                         临时 hook 命中即撤；task 的 continuation 在**泵**里跑完。
//   · `once-timeout`    ：不命中 + 短 timeout ⇒ 泵先 reject（catch 在泵里 drain 到、能 log），
//                         随后 fail Run（reason 点名 timeout）。
//   · `task-await`      ：task 的 `phase=1 → await → phase=2` 必须在**同一入口**完成。
//   · `task-close`      ：start 不注册工作 + task 立即 settle ⇒ 收口（readout `run=done`）。
//   · `task-keeps-run`  ：task `await ctx.once(长 timeout)` ⇒ Run **不收口**（run=waiting）。
//   · `task-reject`     ：task 抛 `boom` ⇒ fail Run（reason 含 boom）。
//   · `pump-budget`     ：once 超时后 task 进 `while(true) await Promise.resolve()` ⇒ 泵的预算窗打断。
//   · `bare-once`       ：**裸** `ctx.once`（不进 `ctx.task`）+ start 不注册 ⇒ Run 靠 once 活着。
//
// ★ guest 写回统一放在「dump 循环」里：start 起一个 task，每次读点 `await ctx.delay(0)` 之后
//   把模块变量**打成一行日志**（P2b：读点任务里没有 Tick ⇒ guest 写只走 Tick；C++ 用
//   `PluginHost::lastLogLine()` + `dumpInt` 解析）。⛔ 不是 v1 的 onTrigger。
const SCRIPT = "FIELD.BIN";
const POS = 0x0100;
const OUT_POS = 0xb120;
const OUT_SCRIPT_OK = 0xb121;
const OUT_ID = 0xb122;
const OUT_OPCODE_OK = 0xb123;
const OUT_PHASE = 0xb125;

let lastTick = null;
let phase = 0;
let dumpOut = {};
let dumpSeq = 0;

function put(ctx, id, value) {
    dumpOut[id] = value === undefined ? 0x7ffffffe : value;
}

// ★ QJS 沙箱没有 `JSON` 全局 ⇒ 手写"数字值"序列化（键 = 数字 id）。
function dumpLine(obj) {
    let out = "";
    for (const key in obj) {
        if (out.length !== 0) out += ",";
        out += '"' + key + '":' + String(obj[key]);
    }
    return "{" + out + "}";
}

function scheduleDump(ctx) {
    ctx.task(async (t) => {
        await t.delay(0);
        const mode = ctx.params.mode;
        if (mode === "once-resolve") {
            if (lastTick === null) {
                ctx.fail("once", "no tick recorded");
                return;
            }
            put(ctx, OUT_POS, lastTick.pos);
            put(ctx, OUT_SCRIPT_OK, lastTick.script === SCRIPT ? 1 : 0);
            put(ctx, OUT_ID, lastTick.id);
            put(ctx, OUT_OPCODE_OK, lastTick.opcode === 114 ? 1 : 0);
        } else if (mode === "task-await") {
            put(ctx, OUT_PHASE, phase);
        }
        dumpSeq += 1;
        log.info("dump#" + String(dumpSeq) + " " + dumpLine(dumpOut));
        dumpOut = {};
    });
}

const plugin = {
    start(ctx) {
        const mode = ctx.params.mode;
        if (mode === "once-resolve") {
            const pending = ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 1000 });
            ctx.task(async (tctx) => {
                const tick = await pending;
                lastTick = {
                    id: tick.id,
                    script: tick.now.script,
                    pos: tick.now.pos,
                    opcode: tick.now.opcode,
                };
                log.info("once-resolve: resumed pos=" + String(tick.now.pos) + " id=" + String(tick.id));
            });
            log.info("once-resolve: armed");
            scheduleDump(ctx);
            return;
        }
        if (mode === "once-timeout") {
            ctx.task(async () => {
                try {
                    const tick = await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 1 });
                    log.info("unexpected resolve pos=" + String(tick.now.pos));
                } catch (e) {
                    log.info("once-timeout caught: " + String(e && e.message ? e.message : e));
                }
            });
            log.info("once-timeout: armed");
            return;
        }
        if (mode === "task-await") {
            ctx.task(async () => {
                phase = 1;
                await Promise.resolve();
                phase = 2;
                log.info("task-await: continuation phase=" + String(phase));
            });
            log.info("task-await: registered");
            scheduleDump(ctx);
            return;
        }
        if (mode === "task-close") {
            ctx.task(async () => {
                await Promise.resolve();
                log.info("task-close: settled");
            });
            log.info("task-close: registered");
            return;
        }
        if (mode === "task-keeps-run") {
            ctx.task(async () => {
                const tick = await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 5000 });
                lastTick = { id: tick.id, script: tick.now.script, pos: tick.now.pos, opcode: tick.now.opcode };
                log.info("task-keeps-run: resumed pos=" + String(tick.now.pos));
            });
            log.info("task-keeps-run: armed");
            return;
        }
        if (mode === "task-reject") {
            ctx.task(async () => {
                throw new Error("boom");
            });
            return;
        }
        if (mode === "pump-budget") {
            ctx.task(async () => {
                try {
                    await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 1 });
                    log.info("pump-budget: unexpected resolve");
                } catch (e) {
                    log.info("pump-budget: caught, about to spin: " +
                        String(e && e.message ? e.message : e));
                    while (true) {
                        await Promise.resolve();
                    }
                }
            });
            log.info("pump-budget: armed");
            return;
        }
        if (mode === "bare-once") {
            const pending = ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 60000 });
            pending.then((tick) => {
                log.info("bare-once: resolved pos=" + String(tick.now.pos) + " id=" + String(tick.id));
            });
            log.info("bare-once: armed");
            return;
        }
        if (mode === "task-finish-reject") {
            ctx.task(async () => {
                throw new Error("late-boom");
            });
            ctx.finish(5, "bye");
            return;
        }
        if (mode === "task-quit-reject") {
            ctx.task(async () => {
                throw new Error("late-boom");
            });
            ctx.quit("bye");
            return;
        }
        throw new Error("s2b-async: unknown mode: " + String(mode));
    },
};

export default plugin;
