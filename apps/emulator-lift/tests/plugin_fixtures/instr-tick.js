// S2a 夹具 · 指令点命中 → `handle.onTick(fn)`（只读反向 sink）
//
// 判据（红了意味着什么）：
//   · `mode=hit`   ：探针命中后 tick 回调必须跑；`tick.{id,script,pos,opcode}` 与宿主侧
//                    （`InstrHit`）逐项一致 —— 本夹具把 `seen`（闭包计数）/ 字段 / `stats().hits`
//                    写进 guest int，由 C++ 用 `readGlobal` 读回（⛔ 不是"没报错"）。
//   · `mode=throw` ：回调抛错 ⇒ 宿主侧 = 一行 error + **摘 handler** + Run **不 fail**；
//                    第二次命中不再有日志，但 `stats().hits` 继续涨（entry 还在计数）。
//   · `mode=done`  ：`return []` ⇒ Run 收口 ⇒ hook 表空；之后的探针**不再跑 JS**。
//
// ★ tick 是**只读**的：`ctx.edit/input/trigger/fail/finish/quit/profile/config/hook.*` 在 tick
//   入口全部 TypeError（`inCallback()` 恒 false）⇒ 本夹具的字段校验放在**下一次 onTrigger**
//   （那是合法的 Run 回调），而不是 tick 里 `ctx.fail`（那会先抛 TypeError，变成"handler 挂了"）。
const SCRIPT = "FIELD.BIN";
const POS = 0x0100;
// guest int 槽（C++ 侧按同一组 id 读回；池 = 0xB1B6 个槽 ⇒ 这些 id 都寻址得到）。
const OUT_SEEN = 0xb100;        // tick 回调被调了几次（闭包计数）
const OUT_STATS_HITS = 0xb101;  // `ctx.hook.list()[0].hits`（runtime 侧计数）
const OUT_POS = 0xb102;         // `tick.pos`
const OUT_OPCODE = 0xb103;      // `tick.opcode`
const OUT_SCRIPT_OK = 0xb104;   // `tick.script === SCRIPT`
const OUT_ID = 0xb105;          // `tick.id`

const plugin = {
    start(ctx) {
        const handle = ctx.hook.at(SCRIPT, POS);
        if (handle === undefined) throw new Error("instr-tick: ctx.hook.at failed");
        ctx.ui.state.tick = null;
        const mode = ctx.params.mode;
        let seen = 0;
        if (mode === "hit") {
            handle.onTick((tick) => {
                // ★ 只读硬边界：tick 入口 `inCallback()` 恒 false ⇒ 写类 trampoline 全 TypeError。
                //   只要有一个被**接受**，本夹具就抛 ⇒ `plugin/point-hit` 的日志行数当场红
                //   （比"没报错"强：它证明那条门真的在）。
                const writes = [
                    () => ctx.fail("tick", "should-not-work"),
                    () => ctx.edit.global.set("int", 0xb1b0, 1),
                    () => ctx.trigger.afterDelay(1, { name: "bad" }),
                ];
                for (const write of writes) {
                    let accepted = false;
                    try {
                        write();
                        accepted = true;
                    } catch (e) {
                        if (!e || e.name !== "TypeError") {
                            throw new Error("tick write gate: " + String(e));
                        }
                    }
                    if (accepted) throw new Error("tick write gate accepted a write");
                }
                seen += 1;
                ctx.ui.state.tick = {
                    seen,
                    id: tick.id,
                    script: tick.script,
                    pos: tick.pos,
                    opcode: tick.opcode,
                };
                // ★ 带序号：`PluginHost::log` 对**连续相同**的消息去重。
                tick.log("info", "tick#" + seen + " @" + tick.pos);
            });
        } else if (mode === "throw") {
            handle.onTick(() => {
                throw new Error("boom");
            });
        } else if (mode === "done") {
            // Run 收口之后**这条不许再跑**（C++ 判 log 行数不变）⇒ 它必须留下可观测的痕迹。
            handle.onTick((tick) => {
                tick.log("info", "tick-after-run");
            });
            // ★ S2b：`handle.onTick` 是 **sync 订阅** ⇒ 算"Run 还活着"（`hasPendingWork`）。
            //   本用例判的是"Run 收口时 hook 表被释放"，不是"订阅能让 Run 一直活着" ⇒ 先撤订阅
            //   再 `return []`（否则 runner.done 之后 Run 永远不收口，本用例会挂在 hook 表非空）。
            handle.onTick(null);
        } else {
            throw new Error("instr-tick: unknown mode: " + String(mode));
        }
        ctx.log("info", "instr-tick: installed mode=" + String(mode));
        if (mode === "done") return [];
        // 命中之后要有一个合法的 Run 回调把读数写回 guest（`afterPosition` = 读点上的电平门）。
        return [ctx.trigger.afterPosition("SCENE1", 0x1000, 0x1001, { name: "check" })];
    },
    onTrigger(ctx) {
        const t = ctx.ui.state.tick;
        if (t === null) {
            ctx.fail("tick", "no hit recorded");
            return [];
        }
        const hooks = ctx.hook.list();
        const runtimeHits = hooks.length > 0 ? hooks[0].hits : 0;
        ctx.edit.global.set("int", OUT_SEEN, t.seen);
        ctx.edit.global.set("int", OUT_STATS_HITS, runtimeHits);
        ctx.edit.global.set("int", OUT_POS, t.pos);
        ctx.edit.global.set("int", OUT_OPCODE, t.opcode);
        ctx.edit.global.set("int", OUT_SCRIPT_OK, t.script === SCRIPT ? 1 : 0);
        ctx.edit.global.set("int", OUT_ID, t.id);
        if (t.seen !== 2) ctx.fail("tick", "seen=" + String(t.seen));
        if (runtimeHits !== 2) ctx.fail("tick", "stats.hits=" + String(runtimeHits));
        if (t.pos !== POS) ctx.fail("tick", "pos=" + String(t.pos));
        if (t.opcode !== 114) ctx.fail("tick", "opcode=" + String(t.opcode));
        if (t.script !== SCRIPT) ctx.fail("tick", "script=" + String(t.script));
        return [];
    },
};

export default plugin;
