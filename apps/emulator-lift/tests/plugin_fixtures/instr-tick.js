// v2 夹具 · 指令点命中 → `ctx.at` handler（只读 + Tick 写面）
//
// 判据（红了意味着什么）：
//   · `mode=hit`   ：探针命中后 handler 必须跑；`tick.{id,script,pos,opcode}` 与宿主侧逐项一致；
//                    tick 入口的 **RunCtx 写面**（`ctx.edit`）必须 TypeError。
//   · `mode=throw` ：handler 抛错 ⇒ 宿主 = 一行 error + **撤整条订阅** + Run **不 fail**；
//                    第二次命中不再有日志，但 entry 的 hits 继续涨。
//   · `mode=done`  ：订阅在 start 内撤掉 ⇒ Run 收口 ⇒ hook 表空；之后的探针**不再跑 JS**。
const SCRIPT = "FIELD.BIN";
const POS = 0x0100;
const OUT_SEEN = 0xb100;
const OUT_POS = 0xb102;
const OUT_OPCODE = 0xb103;
const OUT_SCRIPT_OK = 0xb104;
const OUT_ID = 0xb105;

let seen = 0;
let tickRec = null;

function scheduleDump(ctx) {
    ctx.task(async (t) => {
        await t.delay(0);
        if (tickRec === null) {
            ctx.fail("tick", "no hit recorded");
            return;
        }
        ctx.edit.global.set("int", OUT_SEEN, tickRec.seen);
        ctx.edit.global.set("int", OUT_POS, tickRec.pos);
        ctx.edit.global.set("int", OUT_OPCODE, tickRec.opcode);
        ctx.edit.global.set("int", OUT_SCRIPT_OK, tickRec.script === SCRIPT ? 1 : 0);
        ctx.edit.global.set("int", OUT_ID, tickRec.id);
    });
}

const plugin = {
    start(ctx) {
        const mode = ctx.params.mode;
        if (mode === "hit") {
            const handle = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                // ★ 写面硬边界：tick 入口里 RunCtx 的 `ctx.edit` 必须 TypeError。
                let accepted = false;
                try {
                    ctx.edit.global.set("int", 0xb1b0, 1);
                    accepted = true;
                } catch (e) {
                    if (!e || e.name !== "TypeError") throw new Error("tick write gate: " + String(e));
                }
                if (accepted) throw new Error("tick write gate accepted a write");
                seen += 1;
                tickRec = {
                    seen,
                    id: tick.id,
                    script: tick.script,
                    pos: tick.pos,
                    opcode: tick.opcode,
                };
                log.info("tick#" + seen + " @" + tick.pos);
            });
            if (typeof handle !== "number") throw new Error("instr-tick: ctx.at failed");
            scheduleDump(ctx);
            return;
        }
        if (mode === "throw") {
            ctx.at({ script: SCRIPT, pos: POS }, () => {
                throw new Error("boom");
            });
            return;
        }
        if (mode === "done") {
            const handle = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                log.info("tick-after-run");
            });
            // ★ 订阅是 Run 存活来源；撤掉它 ⇒ Run 在本入口收口（hook 表空）。
            ctx.off(handle);
            return;
        }
        throw new Error("instr-tick: unknown mode: " + String(mode));
    },
};

export default plugin;
