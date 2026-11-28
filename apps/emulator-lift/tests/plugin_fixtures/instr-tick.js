// v2 夹具 · 指令点命中 → `ctx.at` handler（只读 + Tick 写面）
//
// 判据（红了意味着什么）：
//   · `mode=hit`   ：探针命中后 handler 必须跑；`tick.now.{script,pos,opcode}` 与宿主侧逐项一致；
//                    RunCtx 的写面（`ctx.edit`）不存在 ⇒ TypeError；读数用 `tick.edit` 写回 guest。
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

const plugin = {
    start(ctx) {
        const mode = ctx.params.mode;
        if (mode === "hit") {
            ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
                // ★ 写面硬边界：RunCtx 没有 `ctx.edit`（guest 写只走 Tick）⇒ TypeError。
                let accepted = false;
                try {
                    ctx.edit.global.set("int", 0xb1b0, 1);
                    accepted = true;
                } catch (e) {
                    if (!e || e.name !== "TypeError") throw new Error("tick write gate: " + String(e));
                }
                if (accepted) throw new Error("RunCtx edit accepted a write");
                seen += 1;
                // ★ P2b：回读用**本拍的 Tick**（读点任务里没有 Tick）。
                tick.edit.global.set("int", OUT_SEEN, seen);
                tick.edit.global.set("int", OUT_POS, tick.now.pos);
                tick.edit.global.set("int", OUT_OPCODE, tick.now.opcode);
                tick.edit.global.set("int", OUT_SCRIPT_OK, tick.now.script === SCRIPT ? 1 : 0);
                tick.edit.global.set("int", OUT_ID, tick.id);
                log.info("tick#" + seen + " @" + String(tick.now.pos));
            });
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
