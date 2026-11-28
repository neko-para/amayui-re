// v2 夹具 · **Run 收口必须释放本 Run 的订阅**
//
// 判据（红了意味着什么）：start 里装的 `ctx.at` 订阅在 Run 收口之后**还在** ⇒ 后续脚本指令
//   仍会跑 JS（`instrHookList()` 非空）。三种收口各一条：fail / done（无 pending）/ 预算超限。
const SCRIPT = "FIELD.BIN";
const POS = 0x0100;
const BUDGET_MARK = 0xb1b6;

const plugin = {
    start(ctx) {
        const handle = ctx.at({ script: SCRIPT, pos: POS }, (tick) => {
            tick.patch({ ptrLocal: 0 }, 1);
        });
        if (typeof handle !== "number" || handle <= 0) throw new Error("ctx.at failed（no_injection？）");
        log.info("hook-release: installed handle=" + handle);
        const mode = ctx.params.mode;
        if (mode === "fail") {
            ctx.fail("hook", "release-on-fail");
            return;
        }
        if (mode === "budget") {
            let x = 0;
            for (let i = 0; i < 50000000; i = i + 1) {
                x = x + i;
            }
            ctx.edit.global.set("int", BUDGET_MARK, x);
            return;
        }
        if (mode === "done") {
            // 撤掉唯一的订阅 ⇒ 没有待推工作 ⇒ 本入口收口。
            ctx.off(handle);
            return;
        }
        throw new Error("hook-release: unknown mode: " + String(mode));
    },
};

export default plugin;
