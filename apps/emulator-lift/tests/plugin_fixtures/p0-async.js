// P0 夹具 · v2 前置能力：`ctx.once` 全形态 / `ctx.delay` / Run 侧门（H1/H2）
//
// 判据（每个 mode 对应一条/一组 C++ 用例；红了意味着什么）：
//   · `once-where-range-lo`：`once({script,lo,hi})` 的 **lo 边界** —— `lo-1` 不命中（临时 hook 还在）、
//                            `lo` 命中（hook 被撤 + continuation 在本次 tick 入口的 drain 里跑）。
//   · `once-where-range-hi`：同区间的 **hi 边界** —— `hi` 不命中、`hi-1` 命中。
//   · `once-where-script`  ：`once({script})` = **该脚本下一条指令**；连续两次（第一次 resolve 的
//                            continuation 里注册第二次）在**两个不同 pos** 都 resolve。
//   · `once-where-opcode`  ：`once({opcode})` 只按 PC 上的 opcode 命中（不看 script）。
//   · `delay`              ：`ctx.delay(5)` 让 Run **保持存活**（run=waiting）；到期后由**泵** resolve，
//                            continuation 在同一个泵里跑完；**不 fail Run**（不是 trigger）。
//   · `delay-validation`   ：`ms` 必填 / 整数 / `0 <= ms <= 24h`；非法值一律 TypeError（⛔ 不截断）。
//   · `once-from-task`     ：task **第一次 await 之后**再注册第二个 once 并 await ⇒ 两次都命中
//                            （H1 取消：`ctx.once` 的门是 Run 侧入口，不只是同步首段）。
//   · `finish-from-task`   ：task 的 continuation 里 `ctx.finish(5)` ⇒ `run=finished exit=5`（H2 取消）。
//   · `at-from-task`       ：task 的 continuation 里 `ctx.at(...)` 装订阅 ⇒ task settle 后 Run 仍活着；
//                            命中后 handler 里 `ctx.off(self)` ⇒ 再一个读点收口。
//
// ★ 写面纪律不变：task continuation / at handler 里**不写 guest**（`ctx.edit` / `ctx.input` 的 v1 门
//   仍是 `inCallback()`，本轮不动）；读数一律用 `log.*`（C++ 侧 `logLines()` / `lastLogLine()`）。
const SCRIPT = "FIELD.BIN";
const POS = 0x0100;
const POS2 = 0x0110;  // 第二个点（{script} 的"两条不同 pos" + at-from-task 的命中点）
const LO = 0x0110;    // once({script,lo,hi}) 的区间 [LO,HI)
const HI = 0x0120;
const OPCODE = 114;   // 只是**值**（与 `instr-tick.js` / `at-chain.js` 同一个数；⛔ 不是语义结论）

const plugin = {
    start(ctx) {
        const mode = ctx.params.mode;
        if (mode === "once-where-range-lo" || mode === "once-where-range-hi") {
            ctx.task(async (t) => {
                const tick = await ctx.once({ script: SCRIPT, lo: LO, hi: HI }, { timeoutMs: 60000 });
                log.info("once-where: range resolved mode=" + mode + " pos=" + String(tick.now.pos));
            });
            return null;
        }
        if (mode === "once-where-script") {
            ctx.task(async (t) => {
                const a = await ctx.once({ script: SCRIPT }, { timeoutMs: 60000 });
                log.info("once-where: script first pos=" + String(a.now.pos));
                const b = await ctx.once({ script: SCRIPT }, { timeoutMs: 60000 });
                log.info("once-where: script second pos=" + String(b.now.pos));
            });
            return null;
        }
        if (mode === "once-where-opcode") {
            ctx.task(async (t) => {
                const tick = await ctx.once({ opcode: OPCODE }, { timeoutMs: 60000 });
                log.info("once-where: opcode resolved pos=" + String(tick.now.pos));
            });
            return null;
        }
        if (mode === "delay") {
            ctx.task(async (t) => {
                log.info("delay: before");
                await ctx.delay(5);
                log.info("delay: after");
            });
            return null;
        }
        if (mode === "delay-validation") {
            // 必填 / 整数 / 0..24h：这 6 种都必须 TypeError（⛔ 不截断 1.5、不 toString "5"）。
            const bad = [undefined, -1, 1.5, 86400001, "5", null];
            let rejected = 0;
            for (const v of bad) {
                try {
                    if (v === undefined) ctx.delay();
                    else ctx.delay(v);
                } catch (e) {
                    if (e && e.name === "TypeError") rejected += 1;
                }
            }
            if (rejected !== bad.length) {
                throw new Error("delay-validation: rejected " + String(rejected) + "/" + String(bad.length));
            }
            ctx.task(async (t) => {
                // 0 合法：到期 = 下一个泵（本用例不动 wait 语义）。
                await ctx.delay(0);
                log.info("delay-validation: ok");
            });
            return null;
        }
        if (mode === "once-from-task") {
            ctx.task(async (t) => {
                const a = await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 60000 });
                log.info("once-from-task: first pos=" + String(a.now.pos));
                // ★ H1 取消的判据：这一行在**第一次 await 之后**（continuation 里）注册第二个 once。
                const b = await ctx.once({ script: SCRIPT, pos: POS2 }, { timeoutMs: 60000 });
                log.info("once-from-task: second pos=" + String(b.now.pos));
            });
            return null;
        }
        if (mode === "finish-from-task") {
            ctx.task(async (t) => {
                await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 60000 });
                log.info("finish-from-task: finishing");
                // ★ H2 取消的判据：continuation 里 finish（Run 侧入口）。
                ctx.finish(5, "task-finish");
            });
            return null;
        }
        if (mode === "at-from-task") {
            ctx.task(async (t) => {
                await ctx.once({ script: SCRIPT, pos: POS }, { timeoutMs: 60000 });
                // ★ H1/H2 取消的判据：continuation 里装同步订阅；它让 Run 保持存活。
                atHandle = ctx.at({ script: SCRIPT, pos: POS2 }, (tick, next) => {
                    log.info("at-from-task: hit pos=" + String(tick.now.pos));
                    ctx.off(atHandle);
                    next();
                });
                log.info("at-from-task: installed id=" + String(atHandle));
            });
            return null;
        }
        throw new Error("p0-async: unknown mode: " + String(mode));
    },
};

let atHandle = null;

export default plugin;

